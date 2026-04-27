// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecaskdb_txn.cc — MariaDBTxn implementation.

#include "bytecaskdb_txn.h"
#include "key_encoding.h"

#include "my_global.h"
#include "handler.h"
#include "mysql/plugin.h"
#include "sql_priv.h"

#include <cassert>
#include <cstring>

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// MariaDBTxn lifecycle
// ---------------------------------------------------------------------------

MariaDBTxn::~MariaDBTxn() {
  if (snap_) {
    bytecask_snapshot_free(snap_);
    snap_ = nullptr;
  }
}

void MariaDBTxn::begin_if_needed(THD *thd, handlerton *hton) {
  if (!snap_) {
    snap_ = bytecask_snapshot(db_);
  }

  bool multi = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  if (multi && !registered_all_) {
    trans_register_ha(thd, true, hton, 0);
    registered_all_ = true;
  }
  if (!registered_stmt_) {
    trans_register_ha(thd, false, hton, 0);
    registered_stmt_ = true;
  }
}

// ---------------------------------------------------------------------------
// Write buffering
// ---------------------------------------------------------------------------

void MariaDBTxn::buffer_put(const uint8_t *key, size_t klen,
                            const uint8_t *val, size_t vlen) {
  std::vector<uint8_t> k(key, key + klen);
  std::vector<uint8_t> v(val, val + vlen);

  // Update RYOW overlay.
  lookup_[k] = v;

  // Append to ordered log.
  ops_.push_back(Op{Op::Put, std::move(k), std::move(v)});
}

void MariaDBTxn::buffer_del(const uint8_t *key, size_t klen) {
  std::vector<uint8_t> k(key, key + klen);

  // Update RYOW overlay: tombstone.
  lookup_[k] = std::nullopt;

  // Append to ordered log.
  ops_.push_back(Op{Op::Del, std::move(k), {}});
}

// ---------------------------------------------------------------------------
// RYOW reads
// ---------------------------------------------------------------------------

int MariaDBTxn::get(const uint8_t *key, size_t klen,
                    uint8_t **out_val, size_t *out_val_len) {
  // Check buffer first.
  std::vector<uint8_t> k(key, key + klen);
  auto it = lookup_.find(k);
  if (it != lookup_.end()) {
    if (!it->second.has_value()) {
      // Tombstone — key was deleted in this txn.
      return 0;
    }
    const auto &val = it->second.value();
    auto *buf = static_cast<uint8_t *>(malloc(val.size()));
    if (!buf) { return -1; }
    std::memcpy(buf, val.data(), val.size());
    *out_val = buf;
    *out_val_len = val.size();
    return 1;
  }

  // Fall through to snapshot.
  if (!snap_) { return -1; }
  return bytecask_snapshot_get(snap_, key, klen, out_val, out_val_len);
}

bool MariaDBTxn::exists(const uint8_t *key, size_t klen) {
  std::vector<uint8_t> k(key, key + klen);
  auto it = lookup_.find(k);
  if (it != lookup_.end()) {
    return it->second.has_value();
  }

  if (!snap_) { return false; }
  return bytecask_snapshot_contains_key(snap_, key, klen) == 1;
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::iter_prefix(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    uint32_t table_id) {
  // Open snapshot iterator at lo.
  bytecask_iter_t *snap_iter = nullptr;
  if (snap_) {
    snap_iter = bytecask_snapshot_iter_open(snap_, lo, lo_len);
  }

  // Find first buffer entry >= lo.
  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.lower_bound(lo_vec);

  return std::make_unique<MergeIterator>(
      snap_iter, buf_it, lookup_.end(), std::move(hi_vec), table_id);
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::riter_prefix(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    uint32_t table_id) {
  // Open reverse snapshot iterator at hi.
  bytecask_iter_t *snap_iter = nullptr;
  if (snap_) {
    snap_iter = bytecask_snapshot_riter_open(snap_, hi, hi_len);
  }

  // Find last buffer entry <= hi (reverse iteration).
  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.upper_bound(hi_vec);
  if (buf_it != lookup_.begin()) {
    --buf_it;  // Move to last entry <= hi
  }

  return std::make_unique<MergeIterator>(
      snap_iter, buf_it, lookup_.end(), std::move(lo_vec), table_id);
}

// ---------------------------------------------------------------------------
// Commit / rollback
// ---------------------------------------------------------------------------

int MariaDBTxn::commit(THD * /*thd*/, bool all) {
  if (!all && registered_all_) {
    // Statement commit within a multi-statement session txn.
    // Buffer stays for the session-level commit.
    registered_stmt_ = false;
    return 0;
  }

  if (ops_.empty()) {
    reset();
    return 0;
  }

  // Build WritePlan from snapshot + replay ops in insertion order.
  auto *plan = bytecask_write_plan_new_with_snapshot(snap_);
  snap_ = nullptr;  // consumed by the plan

  for (const auto &op : ops_) {
    switch (op.kind) {
    case Op::Put:
      bytecask_write_plan_put(plan, op.key.data(), op.key.size(),
                              op.val.data(), op.val.size());
      break;
    case Op::Del:
      bytecask_write_plan_del(plan, op.key.data(), op.key.size());
      break;
    }
  }

  int rc = bytecask_apply_batch(db_, plan, /*sync=*/1);
  // plan is consumed regardless of outcome.

  if (rc == 1) {
    reset();
    return 0;
  }
  if (rc == 0) {
    reset();
    return HA_ERR_LOCK_DEADLOCK;
  }
  // rc < 0
  reset();
  return HA_ERR_INTERNAL_ERROR;
}

void MariaDBTxn::rollback(THD * /*thd*/, bool all) {
  if (!all && registered_all_) {
    // Statement rollback within session txn — clear buffer (conservative).
    ops_.clear();
    lookup_.clear();
    registered_stmt_ = false;
    return;
  }
  reset();
}

void MariaDBTxn::reset() {
  if (snap_) {
    bytecask_snapshot_free(snap_);
    snap_ = nullptr;
  }
  ops_.clear();
  lookup_.clear();
  registered_stmt_ = false;
  registered_all_ = false;
}

// ---------------------------------------------------------------------------
// MergeIterator
// ---------------------------------------------------------------------------

MariaDBTxn::MergeIterator::MergeIterator(
    bytecask_iter_t *snap_iter,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> hi,
    uint32_t table_id)
    : snap_iter_(snap_iter),
      buf_it_(buf_it),
      buf_end_(buf_end),
      hi_(std::move(hi)),
      table_id_(table_id) {
  // Load initial snapshot position.
  load_snap_current();
  // Advance to the first valid merged entry.
  advance();
}

MariaDBTxn::MergeIterator::~MergeIterator() {
  if (snap_iter_) {
    bytecask_iter_free(snap_iter_);
  }
}

void MariaDBTxn::MergeIterator::next() {
  advance();
}

void MariaDBTxn::MergeIterator::load_snap_current() {
  snap_valid_ = false;
  if (!snap_iter_ || !bytecask_iter_valid(snap_iter_)) {
    return;
  }

  uint8_t *key_buf = nullptr;
  size_t key_len = 0;
  if (bytecask_iter_key(snap_iter_, &key_buf, &key_len) != 0) {
    return;
  }

  // Check if key belongs to this table (within [lo, hi) range).
  if (!key_belongs_to_table(key_buf, key_len, table_id_)) {
    bytecask_free_buf(key_buf);
    return;
  }

  // Check upper bound.
  if (!hi_.empty() &&
      (key_len >= hi_.size() &&
       std::memcmp(key_buf, hi_.data(), hi_.size()) >= 0)) {
    bytecask_free_buf(key_buf);
    return;
  }

  snap_key_.assign(key_buf, key_buf + key_len);
  bytecask_free_buf(key_buf);

  uint8_t *val_buf = nullptr;
  size_t val_len = 0;
  if (bytecask_iter_value(snap_iter_, &val_buf, &val_len) != 0) {
    return;
  }
  snap_val_.assign(val_buf, val_buf + val_len);
  bytecask_free_buf(val_buf);

  snap_valid_ = true;
}

bool MariaDBTxn::MergeIterator::snap_in_range() const {
  return snap_valid_;
}

void MariaDBTxn::MergeIterator::advance() {
  valid_ = false;

  for (;;) {
    // Check if buffer entry is in range.
    bool buf_valid = (buf_it_ != buf_end_);
    if (buf_valid && !hi_.empty()) {
      const auto &bk = buf_it_->first;
      if (bk.size() >= hi_.size() &&
          std::memcmp(bk.data(), hi_.data(), hi_.size()) >= 0) {
        buf_valid = false;
      }
    }
    // Also check table membership for buffer keys.
    if (buf_valid) {
      if (!key_belongs_to_table(buf_it_->first.data(),
                                buf_it_->first.size(), table_id_)) {
        buf_valid = false;
      }
    }

    if (!snap_valid_ && !buf_valid) {
      // Both exhausted.
      return;
    }

    if (!snap_valid_ && buf_valid) {
      // Only buffer has data.
      if (buf_it_->second.has_value()) {
        cur_key_ = buf_it_->first;
        cur_val_ = buf_it_->second.value();
        ++buf_it_;
        valid_ = true;
        return;
      }
      // Tombstone — skip.
      ++buf_it_;
      continue;
    }

    if (snap_valid_ && !buf_valid) {
      // Only snapshot has data.
      cur_key_ = snap_key_;
      cur_val_ = snap_val_;
      bytecask_iter_next(snap_iter_);
      load_snap_current();
      valid_ = true;
      return;
    }

    // Both have data — compare keys.
    const auto &bk = buf_it_->first;
    int cmp = (bk.size() == snap_key_.size())
                  ? std::memcmp(bk.data(), snap_key_.data(), bk.size())
                  : (bk < snap_key_ ? -1 : 1);

    if (cmp < 0) {
      // Buffer key is smaller.
      if (buf_it_->second.has_value()) {
        cur_key_ = bk;
        cur_val_ = buf_it_->second.value();
        ++buf_it_;
        valid_ = true;
        return;
      }
      // Tombstone — skip buffer entry.
      ++buf_it_;
      continue;
    }

    if (cmp == 0) {
      // Equal keys — buffer wins.
      bool has_val = buf_it_->second.has_value();
      if (has_val) {
        cur_key_ = bk;
        cur_val_ = buf_it_->second.value();
      }
      // Advance both.
      ++buf_it_;
      bytecask_iter_next(snap_iter_);
      load_snap_current();
      if (has_val) {
        valid_ = true;
        return;
      }
      // Tombstone — skip both, continue loop.
      continue;
    }

    // cmp > 0: snapshot key is smaller.
    cur_key_ = snap_key_;
    cur_val_ = snap_val_;
    bytecask_iter_next(snap_iter_);
    load_snap_current();
    valid_ = true;
    return;
  }
}

} // namespace bytecaskdb
