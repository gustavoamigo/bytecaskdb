// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecaskdb_txn.cc — MariaDBTxn implementation.

#include "bytecaskdb_txn.h"
#include "ha_bytecaskdb.h"
#include "key_encoding.h"

#include "my_global.h"
#include "handler.h"
#include "mysql/plugin.h"
#include "sql_priv.h"
#include "mysqld_error.h"

#include <algorithm>
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
    stmt_ops_mark_ = ops_.size();
    stmt_row_count_deltas_ = row_count_deltas_;
  }
}

// ---------------------------------------------------------------------------
// Write buffering
// ---------------------------------------------------------------------------

// Pins the OCC snapshot for this statement/transaction on first use. Every
// read and every buffered write goes through here so that the snapshot
// predates any value the statement acts on: the engine's commit-time
// write-write check compares against this snapshot, and a key that another
// transaction committed after it counts as a conflict. Taking the snapshot
// later (at the first write) would let an autocommit read-modify-write
// silently overwrite a concurrent commit. Skipped in deferred-INSERT mode,
// whose commit is snapshot-less by design (see begin_deferred_insert).
void MariaDBTxn::ensure_snapshot() {
  if (!snap_ && !deferred_insert_) {
    snap_.emplace(db_->snapshot());
  }
}

void MariaDBTxn::buffer_put(const uint8_t *key, size_t klen,
                            const uint8_t *val, size_t vlen,
                            bool guard_absent) {
  ensure_snapshot();

  std::vector<uint8_t> k(key, key + klen);
  std::vector<uint8_t> v;
  if (val && vlen > 0) {
    v.assign(val, val + vlen);
  }

  // Update RYOW overlay.
  lookup_[k] = v;

  // Append to ordered log.
  ops_.push_back(Op{Op::Put, std::move(k), std::move(v), guard_absent});
}

void MariaDBTxn::buffer_del(const uint8_t *key, size_t klen) {
  ensure_snapshot();

  std::vector<uint8_t> k(key, key + klen);

  // Update RYOW overlay: tombstone.
  lookup_[k] = std::nullopt;

  // Append to ordered log.
  ops_.push_back(Op{Op::Del, std::move(k), {}, false});
}

bool MariaDBTxn::buffered_key_present(const uint8_t *key, size_t klen) {
  if (lookup_.empty()) { return false; }
  auto it = lookup_.find(std::vector<uint8_t>(key, key + klen));
  return it != lookup_.end() && it->second.has_value();
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

  ensure_snapshot();
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

  ensure_snapshot();
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
// moving its begin(). Both forward and reverse now use default_sentinel_t.

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

  std::optional<bytecask::KeyIterator> snap_it;
  if (snap_) {
    auto range = snap_->keys_from({}, as_view(lo, lo_len));
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

  std::optional<bytecask::ReverseKeyIterator> snap_it;
  if (snap_) {
    auto range = snap_->rkeys_from({}, as_view(hi, hi_len));
    snap_it.emplace(std::move(range.begin()));
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = reverse_buffer_start(hi_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.begin(), lookup_.end(),
      std::move(lo_vec), table_id, index_id);
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::riter_prefix(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    uint32_t table_id) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::ReverseEntryIterator> snap_it;
  if (snap_) {
    auto range = snap_->riter_from({}, as_view(hi, hi_len));
    snap_it.emplace(std::move(range.begin()));
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = reverse_buffer_start(hi_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.begin(), lookup_.end(),
      std::move(lo_vec), table_id);
}

// Largest buffered key <= hi, or end() when every buffered key is above hi.
// Mirrors rkeys_from(hi) on the snapshot side.
MariaDBTxn::LookupMap::const_iterator
MariaDBTxn::reverse_buffer_start(const std::vector<uint8_t> &hi) const {
  auto it = lookup_.upper_bound(hi);
  if (it == lookup_.begin()) { return lookup_.end(); }
  return --it;
}

// ---------------------------------------------------------------------------
// Commit / rollback
// ---------------------------------------------------------------------------

int MariaDBTxn::commit(THD * /*thd*/, bool all) {
  if (bulk_copy_mode_ && (all || !registered_all_)) {
    // Flush any tail rows the copy loop left buffered, then fall through.
    // end_bulk_insert normally does this already; this covers paths that
    // reach commit without it (e.g. an ALTER with zero source rows).
    int e = bulk_flush(true);
    bulk_reset();
    if (e) {
      revert_row_count_deltas();
      reset();
      return e;
    }
  }

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

  const bool deferred = deferred_insert_;
  try {
    // Build the WritePlan and replay ops in insertion order. A deferred
    // INSERT has no snapshot — its ensure_absent guards do the dup check.
    bytecask::WritePlan plan = snap_ ? bytecask::WritePlan{std::move(*snap_)}
                                     : bytecask::WritePlan{};
    snap_.reset();

    for (const auto &op : ops_) {
      switch (op.kind) {
      case Op::Put:
        if (op.guard_absent) {
          plan.ensure_absent(as_view(op.key));
        }
        plan.put(as_view(op.key), as_view(op.val));
        break;
      case Op::Del:
        plan.del(as_view(op.key));
        break;
      }
    }

    bool committed = db_->apply_batch(bytecask::WriteOptions{.sync = true},
                                      std::move(plan)).has_value();
    if (!committed) {
      if (deferred) {
        // Snapshot-less plan: the only precondition is ensure_absent, so a
        // conflict is a duplicate primary key.
        report_deferred_dup_key();
        revert_row_count_deltas();
        reset();
        return HA_ERR_FOUND_DUPP_KEY;
      }
      revert_row_count_deltas();
      reset();
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      return HA_ERR_LOCK_DEADLOCK;
    }
    reset();
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "[bytecaskdb] commit failed: %s\n", e.what());
    revert_row_count_deltas();
    reset();
    return HA_ERR_INTERNAL_ERROR;
  }
}

void MariaDBTxn::rollback(THD * /*thd*/, bool all) {
  // Discard any pending bulk-copy batch. Rows already flushed to the
  // #sql-xxx keyspace are reclaimed when MariaDB drops the temp table
  // (delete_table → del_range), on this path or via ddl_log after a crash.
  bulk_reset();

  if (!all && registered_all_) {
    // Statement rollback within a session transaction: undo only this
    // statement. Earlier statements stay buffered for the session commit.
    restore_row_count_deltas(stmt_row_count_deltas_);
    truncate_ops(stmt_ops_mark_);
    deferred_insert_ = false;
    deferred_handler_ = nullptr;
    deferred_reporter_ = nullptr;
    registered_stmt_ = false;
    return;
  }
  revert_row_count_deltas();
  reset();
}

void MariaDBTxn::truncate_ops(std::size_t mark) {
  if (mark > ops_.size()) { mark = ops_.size(); }
  ops_.resize(mark);
  lookup_.clear();
  for (const auto &op : ops_) {
    if (op.kind == Op::Put)
      lookup_[op.key] = op.val;
    else
      lookup_[op.key] = std::nullopt;
  }
}

void MariaDBTxn::restore_row_count_deltas(const RowCountDeltas &saved) {
  for (auto &[table_id, entry] : row_count_deltas_) {
    int64_t saved_delta = 0;
    if (auto it = saved.find(table_id); it != saved.end()) {
      saved_delta = it->second.delta;
    }
    const int64_t undo = entry.delta - saved_delta;
    if (undo == 0) { continue; }
    if (entry.counter) {
      entry.counter->fetch_add(-undo);
    } else {
      catalog_row_count_add(table_id, -undo);
    }
  }
  row_count_deltas_ = saved;
}

void MariaDBTxn::report_deferred_dup_key() {
  // ops_ is still intact here; the first guarded key that exists in the
  // live DB is the one the ensure_absent guard rejected.
  const std::vector<uint8_t> *dup = nullptr;
  for (const auto &op : ops_) {
    if (op.kind != Op::Put || !op.guard_absent) { continue; }
    bool present = false;
    try {
      present = db_->contains_key({}, as_view(op.key));
    } catch (...) {
      present = false;
    }
    if (present) { dup = &op.key; break; }
  }
  if (deferred_reporter_ && dup && deferred_reporter_(*dup)) {
    return;
  }
  // No handler (or no identifiable key): same error code the server uses,
  // with the WITH_KEY_NAME wording and no rendered value. The format is a
  // literal on purpose: the server's message table is reached through THD
  // members, and this plugin's view of THD (sql_class.h without WITH_WSREP)
  // does not match the server's layout, so only exported functions may take
  // a THD. ER_DUP_ENTRY's own format takes a key *index*, so it cannot be
  // paired with a key name.
  my_printf_error(ER_DUP_ENTRY, "Duplicate entry '%-.192s' for key '%-.192s'",
                  MYF(0), "", "PRIMARY");
}

void MariaDBTxn::reset() {
  snap_.reset();
  ops_.clear();
  lookup_.clear();
  row_count_deltas_.clear();
  stmt_ops_mark_ = 0;
  stmt_row_count_deltas_.clear();
  deferred_insert_ = false;
  deferred_handler_ = nullptr;
  deferred_reporter_ = nullptr;
  registered_stmt_ = false;
  registered_all_ = false;
  bulk_reset();
}

void MariaDBTxn::track_row_count_delta(uint32_t table_id,
                                       std::atomic<int64_t> *row_count,
                                       int64_t delta) {
  if (row_count) {
    row_count->fetch_add(delta);
  } else {
    catalog_row_count_add(table_id, delta);
  }

  auto &entry = row_count_deltas_[table_id];
  entry.delta += delta;
  if (!entry.counter) {
    entry.counter = row_count;
  }
}

void MariaDBTxn::revert_row_count_deltas() {
  restore_row_count_deltas({});
}

// ---------------------------------------------------------------------------
// Bulk-copy mode
// ---------------------------------------------------------------------------

void MariaDBTxn::begin_bulk_copy(std::size_t flush_threshold_bytes) {
  bulk_copy_mode_ = true;
  bulk_flush_threshold_ = flush_threshold_bytes;
  bulk_plan_ = bytecask::WritePlan{};
  bulk_bytes_ = 0;
  bulk_seen_.clear();
}

void MariaDBTxn::bulk_reset() {
  bulk_copy_mode_ = false;
  bulk_plan_ = bytecask::WritePlan{};
  bulk_bytes_ = 0;
  bulk_flush_threshold_ = 0;
  bulk_seen_.clear();
}

void MariaDBTxn::bulk_buffer_put(const uint8_t *key, std::size_t klen,
                                 const uint8_t *val, std::size_t vlen) {
  static constexpr uint8_t kEmpty[1] = {0};
  const uint8_t *vp = (val && vlen > 0) ? val : kEmpty;
  const std::size_t vl = (val && vlen > 0) ? vlen : 0;
  bulk_plan_.put(as_view(key, klen), as_view(vp, vl));
  bulk_bytes_ += klen + vlen;
}

void MariaDBTxn::bulk_note_key(const uint8_t *key, std::size_t klen) {
  bulk_seen_.emplace(key, key + klen);
}

int MariaDBTxn::bulk_flush(bool sync) {
  if (bulk_bytes_ == 0 && bulk_seen_.empty()) {
    return 0;
  }
  try {
    bool committed =
        db_->apply_batch(bytecask::WriteOptions{.sync = sync},
                         std::move(bulk_plan_)).has_value();
    bulk_plan_ = bytecask::WritePlan{};
    bulk_bytes_ = 0;
    bulk_seen_.clear();
    if (!committed) {
      // A snapshot-less plan cannot conflict; a false here is an engine fault.
      return HA_ERR_INTERNAL_ERROR;
    }
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "[bytecaskdb] bulk flush failed: %s\n", e.what());
    bulk_plan_ = bytecask::WritePlan{};
    bulk_bytes_ = 0;
    bulk_seen_.clear();
    return HA_ERR_INTERNAL_ERROR;
  }
}

bool MariaDBTxn::bulk_pk_exists(const uint8_t *key, std::size_t klen) {
  if (bulk_seen_.find(std::vector<uint8_t>(key, key + klen)) != bulk_seen_.end()) {
    return true;
  }
  try {
    return db_->contains_key({}, as_view(key, klen));
  } catch (...) {
    return false;
  }
}

bool MariaDBTxn::bulk_unique_prefix_exists(const uint8_t *prefix,
                                          std::size_t plen) {
  if (bulk_seen_.find(std::vector<uint8_t>(prefix, prefix + plen)) !=
      bulk_seen_.end()) {
    return true;
  }
  try {
    for (auto &k : db_->keys_from({}, as_view(prefix, plen))) {
      return k.size() >= plen &&
             std::memcmp(u8_data(k), prefix, plen) == 0;
    }
  } catch (...) {
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Savepoints
// ---------------------------------------------------------------------------

void MariaDBTxn::savepoint_set(void *sv) {
  *static_cast<uint32_t *>(sv) = static_cast<uint32_t>(ops_.size());
}

void MariaDBTxn::savepoint_rollback(void *sv) {
  truncate_ops(*static_cast<const uint32_t *>(sv));
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
      buf_begin_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(hi)),
      table_id_(table_id) {
  load_snap_current();
  advance();
}

MariaDBTxn::MergeIterator::MergeIterator(
    std::optional<bytecask::KeyIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> hi,
    uint32_t table_id, uint16_t index_id)
    : snap_key_fwd_(std::move(snap_it)),
      reverse_(false),
      buf_it_(buf_it),
      buf_begin_(buf_it),
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
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_begin,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> lo,
    uint32_t table_id)
    : snap_rev_(std::move(snap_it)),
      reverse_(true),
      buf_it_(buf_it),
      buf_begin_(buf_begin),
      buf_end_(buf_end),
      bound_(std::move(lo)),
      table_id_(table_id) {
  load_snap_current();
  advance();
}

MariaDBTxn::MergeIterator::MergeIterator(
    std::optional<bytecask::ReverseKeyIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_begin,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> lo,
    uint32_t table_id, uint16_t index_id)
    : snap_key_rev_(std::move(snap_it)),
      reverse_(true),
      buf_it_(buf_it),
      buf_begin_(buf_begin),
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
    if (snap_fwd_) return *snap_fwd_ == std::default_sentinel;
    if (snap_key_fwd_) return *snap_key_fwd_ == std::default_sentinel;
    return true;
  }
  if (snap_rev_) return *snap_rev_ == std::default_sentinel;
  if (snap_key_rev_) return *snap_key_rev_ == std::default_sentinel;
  return true;
}

void MariaDBTxn::MergeIterator::snap_step() {
  if (!reverse_) {
    if (snap_fwd_) ++(*snap_fwd_);
    else if (snap_key_fwd_) ++(*snap_key_fwd_);
  } else {
    if (snap_rev_) ++(*snap_rev_);
    else if (snap_key_rev_) ++(*snap_key_rev_);
  }
}

void MariaDBTxn::MergeIterator::load_snap_current() {
  snap_valid_ = false;

  if (snap_at_end()) return;

  const uint8_t *kp = nullptr;
  size_t klen = 0;

  if (!reverse_) {
    if (snap_fwd_) {
      const auto &entry = **snap_fwd_;
      kp = reinterpret_cast<const uint8_t *>(entry.key.data());
      klen = entry.key.size();
      snap_val_ptr_ = reinterpret_cast<const uint8_t *>(entry.value.data());
      snap_val_len_ = entry.value.size();
    } else if (snap_key_fwd_) {
      const auto &key_bytes = **snap_key_fwd_;
      kp = reinterpret_cast<const uint8_t *>(key_bytes.data());
      klen = key_bytes.size();
      snap_val_ptr_ = nullptr;
      snap_val_len_ = 0;
    }
  } else {
    if (snap_rev_) {
      const auto &entry = **snap_rev_;
      kp = reinterpret_cast<const uint8_t *>(entry.key.data());
      klen = entry.key.size();
      snap_val_ptr_ = reinterpret_cast<const uint8_t *>(entry.value.data());
      snap_val_len_ = entry.value.size();
    } else if (snap_key_rev_) {
      const auto &key_bytes = **snap_key_rev_;
      kp = reinterpret_cast<const uint8_t *>(key_bytes.data());
      klen = key_bytes.size();
      snap_val_ptr_ = nullptr;
      snap_val_len_ = 0;
    }
  }

  if (!kp) return;

  if (use_index_filter_) {
    if (!key_belongs_to_index(kp, klen, table_id_, index_id_)) {
      return;
    }
  } else {
    if (!key_belongs_to_table(kp, klen, table_id_)) {
      return;
    }
  }

  if (!reverse_ && !bound_.empty()) {
    if (klen >= bound_.size() &&
        std::memcmp(kp, bound_.data(), bound_.size()) >= 0) {
      return;
    }
  }

  snap_key_ptr_ = kp;
  snap_key_len_ = klen;
  snap_valid_ = true;
}

void MariaDBTxn::MergeIterator::buf_step() {
  if (!reverse_) {
    ++buf_it_;
    return;
  }
  if (buf_it_ == buf_begin_) {
    buf_it_ = buf_end_;
  } else {
    --buf_it_;
  }
}

bool MariaDBTxn::MergeIterator::buf_candidate_valid() const {
  if (buf_it_ == buf_end_) return false;
  const auto &bk = buf_it_->first;
  if (!reverse_ && !bound_.empty() &&
      bk.size() >= bound_.size() &&
      std::memcmp(bk.data(), bound_.data(), bound_.size()) >= 0) {
    return false;
  }
  if (use_index_filter_) {
    return key_belongs_to_index(bk.data(), bk.size(), table_id_, index_id_);
  }
  return key_belongs_to_table(bk.data(), bk.size(), table_id_);
}

void MariaDBTxn::MergeIterator::emit_buf() {
  cur_key_ = buf_it_->first;
  assign_bytes(cur_val_, buf_it_->second.value());
  buf_step();
  valid_ = true;
}

void MariaDBTxn::MergeIterator::emit_snap() {
  cur_key_.assign(snap_key_ptr_, snap_key_ptr_ + snap_key_len_);
  assign_bytes(cur_val_, snap_val_ptr_, snap_val_len_);
  snap_step();
  load_snap_current();
  valid_ = true;
}

void MariaDBTxn::MergeIterator::advance() {
  valid_ = false;

  for (;;) {
    const bool buf_valid = buf_candidate_valid();

    if (!snap_valid_ && !buf_valid) {
      return;
    }

    if (!snap_valid_) {
      if (buf_it_->second.has_value()) { emit_buf(); return; }
      buf_step();  // tombstone with nothing to suppress
      continue;
    }

    if (!buf_valid) {
      emit_snap();
      return;
    }

    // Both sides have a candidate: lexicographic compare, shorter-is-less.
    const auto &bk = buf_it_->first;
    const size_t n = std::min(bk.size(), snap_key_len_);
    int cmp = std::memcmp(bk.data(), snap_key_ptr_, n);
    if (cmp == 0) {
      cmp = (bk.size() < snap_key_len_) ? -1
          : (bk.size() > snap_key_len_) ?  1 : 0;
    }

    if (cmp == 0) {
      // Same key on both sides: the buffer's version wins; a tombstone
      // hides the snapshot entry.
      const bool has_val = buf_it_->second.has_value();
      if (has_val) {
        cur_key_ = bk;
        assign_bytes(cur_val_, buf_it_->second.value());
      }
      buf_step();
      snap_step();
      load_snap_current();
      if (has_val) { valid_ = true; return; }
      continue;
    }

    // Forward emits the smaller key first; reverse emits the larger.
    const bool buf_first = reverse_ ? (cmp > 0) : (cmp < 0);
    if (buf_first) {
      if (buf_it_->second.has_value()) { emit_buf(); return; }
      buf_step();
      continue;
    }
    emit_snap();
    return;
  }
}

} // namespace bytecaskdb
