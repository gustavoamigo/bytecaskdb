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
#include "mysqld_error.h"

#include <cassert>
#include <cstring>

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// MariaDBTxn lifecycle
// ---------------------------------------------------------------------------

void MariaDBTxn::begin_if_needed(THD *thd, handlerton *hton) {
  bool multi = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  if (multi && !snap_) {
    snap_.emplace(db_->snapshot());
  }

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
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::vector<uint8_t> k(key, key + klen);
  std::vector<uint8_t> v;
  if (val && vlen > 0) {
    v.assign(val, val + vlen);
  }

  // Update RYOW overlay.
  lookup_[k] = v;

  // Append to ordered log.
  ops_.push_back(Op{Op::Put, std::move(k), std::move(v)});
}

void MariaDBTxn::buffer_del(const uint8_t *key, size_t klen) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::vector<uint8_t> k(key, key + klen);

  // Update RYOW overlay: tombstone.
  lookup_[k] = std::nullopt;

  // Append to ordered log.
  ops_.push_back(Op{Op::Del, std::move(k), {}});
}

// ---------------------------------------------------------------------------
// RYOW reads
// ---------------------------------------------------------------------------

int MariaDBTxn::get(const uint8_t *key, size_t klen, bytecask::Bytes &out) {
  if (!lookup_.empty()) {
    std::vector<uint8_t> k(key, key + klen);
    auto it = lookup_.find(k);
    if (it != lookup_.end()) {
      if (!it->second.has_value()) {
        return 0;
      }
      const auto &val = it->second.value();
      out.assign(reinterpret_cast<const std::byte *>(val.data()),
                 reinterpret_cast<const std::byte *>(val.data() + val.size()));
      return 1;
    }
  }

  try {
    if (snap_) {
      return snap_->get({}, as_view(key, klen), out) ? 1 : 0;
    }
    return db_->get({}, as_view(key, klen), out) ? 1 : 0;
  } catch (...) {
    return -1;
  }
}

bool MariaDBTxn::exists(const uint8_t *key, size_t klen) {
  if (!lookup_.empty()) {
    std::vector<uint8_t> k(key, key + klen);
    auto it = lookup_.find(k);
    if (it != lookup_.end()) {
      return it->second.has_value();
    }
  }

  try {
    if (snap_) {
      return snap_->contains_key({}, as_view(key, klen));
    }
    return db_->contains_key({}, as_view(key, klen));
  } catch (...) {
    return false;
  }
}

// Helpers to extract a single iterator from a subrange<It, sentinel> by
// moving its begin(). We can do this safely because the underlying iterator
// is the only stateful piece — the sentinel is either default_sentinel_t
// (forward) or another iterator value (reverse).

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::iter_prefix(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    uint32_t table_id) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::EntryIterator> snap_it;
  if (snap_) {
    auto range = snap_->iter_from({}, as_view(lo, lo_len));
    snap_it.emplace(std::move(range.begin()));
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.lower_bound(lo_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.end(), std::move(hi_vec), table_id);
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::iter_index_prefix(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    uint32_t table_id, uint16_t index_id) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::EntryIterator> snap_it;
  if (snap_) {
    auto range = snap_->iter_from({}, as_view(lo, lo_len));
    snap_it.emplace(std::move(range.begin()));
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.lower_bound(lo_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.end(), std::move(hi_vec),
      table_id, index_id);
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::riter_index_prefix(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    uint32_t table_id, uint16_t index_id) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::ReverseEntryIterator> snap_it;
  bytecask::ReverseEntryIterator snap_end{};
  if (snap_) {
    auto range = snap_->riter_from({}, as_view(hi, hi_len));
    snap_end = range.end();
    snap_it.emplace(std::move(range.begin()));
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.upper_bound(hi_vec);
  if (buf_it != lookup_.begin()) {
    --buf_it;
  }

  return std::make_unique<MergeIterator>(
      std::move(snap_it), std::move(snap_end),
      buf_it, lookup_.end(), std::move(lo_vec),
      table_id, index_id);
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::riter_prefix(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    uint32_t table_id) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::ReverseEntryIterator> snap_it;
  bytecask::ReverseEntryIterator snap_end{};
  if (snap_) {
    auto range = snap_->riter_from({}, as_view(hi, hi_len));
    snap_end = range.end();
    snap_it.emplace(std::move(range.begin()));
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.upper_bound(hi_vec);
  if (buf_it != lookup_.begin()) {
    --buf_it;
  }

  return std::make_unique<MergeIterator>(
      std::move(snap_it), std::move(snap_end),
      buf_it, lookup_.end(), std::move(lo_vec), table_id);
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

  try {
    // Build WritePlan from snapshot + replay ops in insertion order.
    bytecask::WritePlan plan{std::move(*snap_)};
    snap_.reset();

    for (const auto &op : ops_) {
      switch (op.kind) {
      case Op::Put:
        plan.put(as_view(op.key), as_view(op.val));
        break;
      case Op::Del:
        plan.del(as_view(op.key));
        break;
      }
    }

    bool committed = db_->apply_batch(bytecask::WriteOptions{.sync = true},
                                      std::move(plan));
    reset();
    if (!committed) {
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      return HA_ERR_LOCK_DEADLOCK;
    }
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "[bytecaskdb] commit failed: %s\n", e.what());
    reset();
    return HA_ERR_INTERNAL_ERROR;
  }
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
  snap_.reset();
  ops_.clear();
  lookup_.clear();
  registered_stmt_ = false;
  registered_all_ = false;
}

// ---------------------------------------------------------------------------
// Savepoints
// ---------------------------------------------------------------------------

void MariaDBTxn::savepoint_set(void *sv) {
  *static_cast<uint32_t *>(sv) = static_cast<uint32_t>(ops_.size());
}

void MariaDBTxn::savepoint_rollback(void *sv) {
  uint32_t mark = *static_cast<const uint32_t *>(sv);
  ops_.resize(mark);
  lookup_.clear();
  for (const auto &op : ops_) {
    if (op.kind == Op::Put)
      lookup_[op.key] = op.val;
    else
      lookup_[op.key] = std::nullopt;
  }
}

void MariaDBTxn::savepoint_release(void * /*sv*/) {
}

// ---------------------------------------------------------------------------
// MergeIterator
//
// The buffer side always walks forward (matches the pre-migration C-API
// implementation). The snapshot side is forward (EntryIterator) or reverse
// (ReverseEntryIterator) depending on which constructor was used.
// ---------------------------------------------------------------------------

MariaDBTxn::MergeIterator::MergeIterator(
    std::optional<bytecask::EntryIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> hi,
    uint32_t table_id)
    : snap_fwd_(std::move(snap_it)),
      reverse_(false),
      buf_it_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(hi)),
      table_id_(table_id) {
  load_snap_current();
  advance();
}

MariaDBTxn::MergeIterator::MergeIterator(
    std::optional<bytecask::EntryIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> hi,
    uint32_t table_id, uint16_t index_id)
    : snap_fwd_(std::move(snap_it)),
      reverse_(false),
      buf_it_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(hi)),
      table_id_(table_id),
      index_id_(index_id),
      use_index_filter_(true) {
  load_snap_current();
  advance();
}

MariaDBTxn::MergeIterator::MergeIterator(
    std::optional<bytecask::ReverseEntryIterator> snap_it,
    bytecask::ReverseEntryIterator snap_end,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> lo,
    uint32_t table_id)
    : snap_rev_(std::move(snap_it)),
      snap_rev_end_(std::move(snap_end)),
      reverse_(true),
      buf_it_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(lo)),
      table_id_(table_id) {
  load_snap_current();
  advance();
}

MariaDBTxn::MergeIterator::MergeIterator(
    std::optional<bytecask::ReverseEntryIterator> snap_it,
    bytecask::ReverseEntryIterator snap_end,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> lo,
    uint32_t table_id, uint16_t index_id)
    : snap_rev_(std::move(snap_it)),
      snap_rev_end_(std::move(snap_end)),
      reverse_(true),
      buf_it_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(lo)),
      table_id_(table_id),
      index_id_(index_id),
      use_index_filter_(true) {
  load_snap_current();
  advance();
}

void MariaDBTxn::MergeIterator::next() {
  advance();
}

bool MariaDBTxn::MergeIterator::snap_at_end() const {
  if (!reverse_) {
    if (!snap_fwd_) return true;
    return *snap_fwd_ == std::default_sentinel;
  }
  if (!snap_rev_ || !snap_rev_end_) return true;
  return *snap_rev_ == *snap_rev_end_;
}

void MariaDBTxn::MergeIterator::snap_step() {
  if (!reverse_ && snap_fwd_) {
    ++(*snap_fwd_);
  } else if (reverse_ && snap_rev_) {
    ++(*snap_rev_);
  }
}

void MariaDBTxn::MergeIterator::load_snap_current() {
  snap_valid_ = false;

  if (snap_at_end()) return;

  const std::pair<bytecask::Bytes, bytecask::Bytes> *kv = nullptr;
  if (!reverse_) {
    kv = &(**snap_fwd_);
  } else {
    kv = &(**snap_rev_);
  }
  const auto *kp = u8_data(kv->first);
  const auto klen = kv->first.size();

  if (use_index_filter_) {
    if (!key_belongs_to_index(kp, klen, table_id_, index_id_)) {
      return;
    }
  } else {
    if (!key_belongs_to_table(kp, klen, table_id_)) {
      return;
    }
  }

  // Forward: stop when key >= hi (bound_).
  // Reverse: don't enforce the lower bound here (matches pre-migration
  // behavior; row encoding already prefixes by table id).
  if (!reverse_ && !bound_.empty()) {
    if (klen >= bound_.size() &&
        std::memcmp(kp, bound_.data(), bound_.size()) >= 0) {
      return;
    }
  }

  snap_key_.assign(kp, kp + klen);
  snap_val_.assign(u8_data(kv->second),
                   u8_data(kv->second) + kv->second.size());
  snap_valid_ = true;
}

void MariaDBTxn::MergeIterator::advance() {
  valid_ = false;

  for (;;) {
    bool buf_valid = (buf_it_ != buf_end_);
    if (buf_valid && !reverse_ && !bound_.empty()) {
      const auto &bk = buf_it_->first;
      if (bk.size() >= bound_.size() &&
          std::memcmp(bk.data(), bound_.data(), bound_.size()) >= 0) {
        buf_valid = false;
      }
    }
    if (buf_valid) {
      if (use_index_filter_) {
        if (!key_belongs_to_index(buf_it_->first.data(),
                                  buf_it_->first.size(),
                                  table_id_, index_id_)) {
          buf_valid = false;
        }
      } else {
        if (!key_belongs_to_table(buf_it_->first.data(),
                                  buf_it_->first.size(), table_id_)) {
          buf_valid = false;
        }
      }
    }

    if (!snap_valid_ && !buf_valid) {
      return;
    }

    if (!snap_valid_ && buf_valid) {
      if (buf_it_->second.has_value()) {
        cur_key_ = buf_it_->first;
        cur_val_ = buf_it_->second.value();
        ++buf_it_;
        valid_ = true;
        return;
      }
      ++buf_it_;
      continue;
    }

    if (snap_valid_ && !buf_valid) {
      cur_key_ = snap_key_;
      cur_val_ = snap_val_;
      snap_step();
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
      if (buf_it_->second.has_value()) {
        cur_key_ = bk;
        cur_val_ = buf_it_->second.value();
        ++buf_it_;
        valid_ = true;
        return;
      }
      ++buf_it_;
      continue;
    }

    if (cmp == 0) {
      bool has_val = buf_it_->second.has_value();
      if (has_val) {
        cur_key_ = bk;
        cur_val_ = buf_it_->second.value();
      }
      ++buf_it_;
      snap_step();
      load_snap_current();
      if (has_val) {
        valid_ = true;
        return;
      }
      continue;
    }

    // cmp > 0
    cur_key_ = snap_key_;
    cur_val_ = snap_val_;
    snap_step();
    load_snap_current();
    valid_ = true;
    return;
  }
}

} // namespace bytecaskdb
