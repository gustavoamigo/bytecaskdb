// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// transaction.cpp — Transaction implementation.

#include "bytecaskdb-sql-layer/txn/transaction.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace bytecask::sql {

namespace {
auto outcome(CommitOutcome::Status status, std::exception_ptr error = {})
    -> CommitOutcome {
  CommitOutcome out;
  out.status = status;
  out.error = std::move(error);
  return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Transaction::Begun Transaction::begin_if_needed(bool multi) {
  Begun begun;
  if (multi && !snap_) {
    snap_.emplace(db_->snapshot());
  }

  if (multi && !in_txn_) {
    in_txn_ = true;
    begun.transaction = true;
  }
  if (!in_stmt_) {
    in_stmt_ = true;
    begun.statement = true;
    stmt_ops_mark_ = ops_.size();
    stmt_row_count_deltas_ = row_count_deltas_;
  }
  return begun;
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
void Transaction::ensure_snapshot() {
  if (!snap_ && !deferred_insert_) {
    snap_.emplace(db_->snapshot());
  }
}

void Transaction::buffer_put(const uint8_t *key, size_t klen,
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

void Transaction::buffer_del(const uint8_t *key, size_t klen) {
  ensure_snapshot();

  std::vector<uint8_t> k(key, key + klen);

  // Update RYOW overlay: tombstone.
  lookup_[k] = std::nullopt;

  // Append to ordered log.
  ops_.push_back(Op{Op::Del, std::move(k), {}, false});
}

bool Transaction::buffered_key_present(const uint8_t *key, size_t klen) {
  if (lookup_.empty()) { return false; }
  auto it = lookup_.find(std::vector<uint8_t>(key, key + klen));
  return it != lookup_.end() && it->second.has_value();
}

// ---------------------------------------------------------------------------
// RYOW reads
// ---------------------------------------------------------------------------

int Transaction::get(const uint8_t *key, size_t klen, bytecask::Bytes &out) {
  if (!lookup_.empty()) {
    std::vector<uint8_t> k(key, key + klen);
    auto it = lookup_.find(k);
    if (it != lookup_.end()) {
      if (!it->second.has_value()) {
        return 0;
      }
      assign_bytes(out, it->second.value());
      return 1;
    }
  }

  ensure_snapshot();
  try {
    if (snap_) {
      return snap_->get(read_options(), as_view(key, klen), out) ? 1 : 0;
    }
    return db_->get(read_options(), as_view(key, klen), out) ? 1 : 0;
  } catch (...) {
    return -1;
  }
}

bool Transaction::exists(const uint8_t *key, size_t klen) {
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
      return snap_->contains_key(read_options(), as_view(key, klen));
    }
    return db_->contains_key(read_options(), as_view(key, klen));
  } catch (...) {
    return false;
  }
}

std::size_t Transaction::count_range(const uint8_t *lo, size_t lo_len,
                                     const uint8_t *hi, size_t hi_len,
                                     std::size_t limit) {
  // Contradictory predicates (a > 5 AND a < 3) give lo >= hi: a range that
  // holds no rows. count_keys refuses one, so it is answered here.
  if (!std::lexicographical_compare(lo, lo + lo_len, hi, hi + hi_len)) {
    return 0;
  }
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }
  auto n = snap_->count_keys(as_view(lo, lo_len), as_view(hi, hi_len), limit);
  if (n >= limit) {
    return limit;
  }
  // A buffered put of a key the snapshot lacks adds one; a buffered delete
  // of a key it holds takes one away (that key is among the n counted).
  const std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  const std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  for (auto it = lookup_.lower_bound(lo_vec);
       it != lookup_.end() && it->first < hi_vec; ++it) {
    const bool in_snap = snap_->contains_key(
        read_options(), as_view(it->first.data(), it->first.size()));
    if (it->second && !in_snap) {
      if (++n >= limit) {
        return limit;
      }
    } else if (!it->second && in_snap) {
      --n;
    }
  }
  return n;
}

// Each opener takes the snapshot iterator as the begin() of the snapshot's
// subrange<It, default_sentinel_t>.

std::unique_ptr<Transaction::MergeIterator> Transaction::iter_entries(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    std::vector<uint8_t> ns) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::EntryIterator> snap_it;
  if (snap_) {
    auto range = snap_->iter_from(read_options(), as_view(lo, lo_len));
    snap_it.emplace(range.begin());
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.lower_bound(lo_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.end(), std::move(hi_vec),
      std::move(ns));
}

std::unique_ptr<Transaction::MergeIterator> Transaction::iter_keys(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    std::vector<uint8_t> ns) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::KeyIterator> snap_it;
  if (snap_) {
    auto range = snap_->keys_from(read_options(), as_view(lo, lo_len));
    snap_it.emplace(range.begin());
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = lookup_.lower_bound(lo_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.end(), std::move(hi_vec),
      std::move(ns));
}

std::unique_ptr<Transaction::MergeIterator> Transaction::riter_keys(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    std::vector<uint8_t> ns) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::ReverseKeyIterator> snap_it;
  if (snap_) {
    auto range = snap_->rkeys_from(read_options(), as_view(hi, hi_len));
    snap_it.emplace(range.begin());
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = reverse_buffer_start(hi_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.begin(), lookup_.end(),
      std::move(lo_vec), std::move(ns));
}

std::unique_ptr<Transaction::MergeIterator> Transaction::riter_entries(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    std::vector<uint8_t> ns) {
  if (!snap_) {
    snap_.emplace(db_->snapshot());
  }

  std::optional<bytecask::ReverseEntryIterator> snap_it;
  if (snap_) {
    auto range = snap_->riter_from(read_options(), as_view(hi, hi_len));
    snap_it.emplace(range.begin());
  }

  std::vector<uint8_t> lo_vec(lo, lo + lo_len);
  std::vector<uint8_t> hi_vec(hi, hi + hi_len);
  auto buf_it = reverse_buffer_start(hi_vec);

  return std::make_unique<MergeIterator>(
      std::move(snap_it), buf_it, lookup_.begin(), lookup_.end(),
      std::move(lo_vec), std::move(ns));
}

// Largest buffered key <= hi, or end() when every buffered key is above hi.
// Mirrors rkeys_from(hi) on the snapshot side.
Transaction::LookupMap::const_iterator
Transaction::reverse_buffer_start(const std::vector<uint8_t> &hi) const {
  auto it = lookup_.upper_bound(hi);
  if (it == lookup_.begin()) { return lookup_.end(); }
  return --it;
}

// ---------------------------------------------------------------------------
// Commit / rollback
// ---------------------------------------------------------------------------

CommitOutcome Transaction::fail(CommitOutcome outcome) {
  revert_row_count_deltas();
  reset();
  return outcome;
}

CommitOutcome Transaction::commit(bool all, bool sync) {
  using Status = CommitOutcome::Status;
  if (bulk_copy_mode_ && (all || !in_txn_)) {
    // Flush any tail rows the copy loop left buffered, then fall through.
    // end_bulk_insert normally does this already; this covers paths that
    // reach commit without it (e.g. an ALTER with zero source rows).
    auto flushed = bulk_flush(true);
    bulk_reset();
    if (flushed.status != Status::Committed) {
      return fail(std::move(flushed));
    }
  }

  if (!all && in_txn_) {
    // Statement commit within a multi-statement session txn.
    // Buffer stays for the session-level commit.
    in_stmt_ = false;
    return {};
  }

  if (ops_.empty()) {
    reset();
    return {};
  }

  const bool deferred = deferred_insert_;
  try {
    // Build the WritePlan and replay ops in insertion order. A deferred
    // INSERT has no snapshot — its ensure_absent guards do the dup check.
    bytecask::WritePlan plan = snap_ ? bytecask::WritePlan{std::move(*snap_)}
                                     : bytecask::WritePlan{limits_};
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

    bool committed =
        db_->apply_batch(bytecask::WriteOptions{.sync = sync}, std::move(plan))
            .has_value();
    if (!committed) {
      if (deferred) {
        // Snapshot-less plan: the only precondition is ensure_absent, so a
        // conflict is a duplicate primary key. ops_ is still intact here.
        return fail(deferred_dup_key());
      }
      return fail(outcome(Status::Conflict));
    }
    reset();
    return {};
  } catch (const std::exception &) {
    return fail(outcome(Status::EngineError, std::current_exception()));
  }
}

void Transaction::rollback(bool all) {
  // Discard any pending bulk-copy batch. Rows already flushed to the
  // #sql-xxx keyspace are reclaimed when MariaDB drops the temp table
  // (delete_table → del_range), on this path or via ddl_log after a crash.
  bulk_reset();

  if (!all && in_txn_) {
    // Statement rollback within a session transaction: undo only this
    // statement. Earlier statements stay buffered for the session commit.
    restore_row_count_deltas(stmt_row_count_deltas_);
    truncate_ops(stmt_ops_mark_);
    deferred_insert_ = false;
    in_stmt_ = false;
    return;
  }
  revert_row_count_deltas();
  reset();
}

void Transaction::truncate_ops(std::size_t mark) {
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

void Transaction::restore_row_count_deltas(const RowCountDeltas &saved) {
  for (auto &[table_id, entry] : row_count_deltas_) {
    int64_t saved_delta = 0;
    if (auto it = saved.find(table_id); it != saved.end()) {
      saved_delta = it->second.delta;
    }
    const int64_t undo = entry.delta - saved_delta;
    if (undo == 0) { continue; }
    entry.counter->fetch_add(-undo);
  }
  row_count_deltas_ = saved;
}

CommitOutcome Transaction::deferred_dup_key() const {
  // The first guarded key that exists in the live DB is the one the
  // ensure_absent guard rejected.
  auto out = outcome(CommitOutcome::Status::DuplicateKey);
  for (const auto &op : ops_) {
    if (op.kind != Op::Put || !op.guard_absent) { continue; }
    bool present = false;
    try {
      present = db_->contains_key(read_options(), as_view(op.key));
    } catch (...) {
      present = false;
    }
    if (present) {
      out.duplicate = CommitOutcome::Duplicate{op.key, op.val};
      break;
    }
  }
  return out;
}

void Transaction::reset() {
  snap_.reset();
  ops_.clear();
  lookup_.clear();
  row_count_deltas_.clear();
  stmt_ops_mark_ = 0;
  stmt_row_count_deltas_.clear();
  savepoints_.clear();
  deferred_insert_ = false;
  in_stmt_ = false;
  in_txn_ = false;
  bulk_reset();
}

void Transaction::track_row_count_delta(uint32_t table_id,
                                        std::atomic<int64_t> &row_count,
                                        int64_t delta) {
  row_count.fetch_add(delta);

  auto &entry = row_count_deltas_[table_id];
  entry.delta += delta;
  if (!entry.counter) {
    entry.counter = &row_count;
  }
}

void Transaction::revert_row_count_deltas() {
  restore_row_count_deltas({});
}

// ---------------------------------------------------------------------------
// Bulk-copy mode
// ---------------------------------------------------------------------------

void Transaction::begin_bulk_copy(std::size_t flush_threshold_bytes) {
  bulk_copy_mode_ = true;
  bulk_flush_threshold_ = flush_threshold_bytes;
  bulk_plan_ = bytecask::WritePlan{limits_};
  bulk_bytes_ = 0;
  bulk_seen_.clear();
}

void Transaction::bulk_reset() {
  bulk_copy_mode_ = false;
  bulk_plan_ = bytecask::WritePlan{limits_};
  bulk_bytes_ = 0;
  bulk_flush_threshold_ = 0;
  bulk_seen_.clear();
}

void Transaction::bulk_buffer_put(const uint8_t *key, std::size_t klen,
                                  const uint8_t *val, std::size_t vlen) {
  static constexpr uint8_t kEmpty[1] = {0};
  const uint8_t *vp = (val && vlen > 0) ? val : kEmpty;
  const std::size_t vl = (val && vlen > 0) ? vlen : 0;
  bulk_plan_.put(as_view(key, klen), as_view(vp, vl));
  bulk_bytes_ += klen + vlen;
}

void Transaction::bulk_note_key(const uint8_t *key, std::size_t klen) {
  bulk_seen_.emplace(key, key + klen);
}

CommitOutcome Transaction::bulk_flush(bool sync) {
  using Status = CommitOutcome::Status;
  if (bulk_bytes_ == 0 && bulk_seen_.empty()) {
    return {};
  }
  try {
    bool committed =
        db_->apply_batch(bytecask::WriteOptions{.sync = sync},
                         std::move(bulk_plan_)).has_value();
    bulk_plan_ = bytecask::WritePlan{limits_};
    bulk_bytes_ = 0;
    bulk_seen_.clear();
    if (!committed) {
      // A snapshot-less plan cannot conflict; a false here is an engine fault.
      return outcome(Status::EngineError);
    }
    return {};
  } catch (const std::exception &) {
    bulk_plan_ = bytecask::WritePlan{limits_};
    bulk_bytes_ = 0;
    bulk_seen_.clear();
    return outcome(Status::EngineError, std::current_exception());
  }
}

bool Transaction::bulk_pk_exists(const uint8_t *key, std::size_t klen) {
  if (bulk_seen_.find(std::vector<uint8_t>(key, key + klen)) != bulk_seen_.end()) {
    return true;
  }
  try {
    return db_->contains_key(read_options(), as_view(key, klen));
  } catch (...) {
    return false;
  }
}

bool Transaction::bulk_unique_prefix_exists(const uint8_t *prefix,
                                            std::size_t plen) {
  if (bulk_seen_.find(std::vector<uint8_t>(prefix, prefix + plen)) !=
      bulk_seen_.end()) {
    return true;
  }
  try {
    // The first key at or after the prefix is the only candidate.
    auto keys = db_->keys_from(read_options(), as_view(prefix, plen));
    auto it = keys.begin();
    if (it == std::default_sentinel) { return false; }
    const auto &k = *it;
    return k.size() >= plen && std::memcmp(u8_data(k), prefix, plen) == 0;
  } catch (...) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Savepoints
// ---------------------------------------------------------------------------

// Each entry of savepoints_ records where the op log stood and what the row
// counters were.

std::uint32_t Transaction::savepoint_set() {
  savepoints_.push_back({ops_.size(), row_count_deltas_});
  return static_cast<std::uint32_t>(savepoints_.size() - 1);
}

void Transaction::savepoint_rollback(std::uint32_t savepoint) {
  if (savepoint >= savepoints_.size()) { return; }
  const auto &sp = savepoints_[savepoint];
  restore_row_count_deltas(sp.row_count_deltas);
  truncate_ops(sp.ops_mark);
  // The savepoint itself survives ROLLBACK TO; later ones are gone.
  savepoints_.resize(savepoint + 1);
}

void Transaction::savepoint_release(std::uint32_t savepoint) {
  if (savepoint < savepoints_.size()) { savepoints_.resize(savepoint); }
}

namespace {
// Lexicographic byte compare; a proper prefix sorts first.
int lex_compare(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
  const int c = std::memcmp(a, b, std::min(alen, blen));
  if (c != 0) return c;
  return (alen < blen) ? -1 : (alen > blen) ? 1 : 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// MergeIterator
//
// The snapshot side is forward (EntryIterator, KeyIterator) or reverse
// (ReverseEntryIterator, ReverseKeyIterator) depending on which constructor
// was used; the buffer side walks the same way.
// ---------------------------------------------------------------------------

Transaction::MergeIterator::MergeIterator(
    std::optional<bytecask::EntryIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> hi,
    std::vector<uint8_t> ns)
    : snap_fwd_(std::move(snap_it)),
      reverse_(false),
      buf_it_(buf_it),
      buf_begin_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(hi)),
      ns_(std::move(ns)) {
  load_snap_current();
  advance();
}

Transaction::MergeIterator::MergeIterator(
    std::optional<bytecask::KeyIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> hi,
    std::vector<uint8_t> ns)
    : snap_key_fwd_(std::move(snap_it)),
      reverse_(false),
      buf_it_(buf_it),
      buf_begin_(buf_it),
      buf_end_(buf_end),
      bound_(std::move(hi)),
      ns_(std::move(ns)) {
  load_snap_current();
  advance();
}

Transaction::MergeIterator::MergeIterator(
    std::optional<bytecask::ReverseEntryIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_begin,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> lo,
    std::vector<uint8_t> ns)
    : snap_rev_(std::move(snap_it)),
      reverse_(true),
      buf_it_(buf_it),
      buf_begin_(buf_begin),
      buf_end_(buf_end),
      bound_(std::move(lo)),
      ns_(std::move(ns)) {
  load_snap_current();
  advance();
}

Transaction::MergeIterator::MergeIterator(
    std::optional<bytecask::ReverseKeyIterator> snap_it,
    LookupMap::const_iterator buf_it,
    LookupMap::const_iterator buf_begin,
    LookupMap::const_iterator buf_end,
    std::vector<uint8_t> lo,
    std::vector<uint8_t> ns)
    : snap_key_rev_(std::move(snap_it)),
      reverse_(true),
      buf_it_(buf_it),
      buf_begin_(buf_begin),
      buf_end_(buf_end),
      bound_(std::move(lo)),
      ns_(std::move(ns)) {
  load_snap_current();
  advance();
}

void Transaction::MergeIterator::next() {
  advance();
}

bool Transaction::MergeIterator::in_namespace(const uint8_t *key,
                                              size_t len) const {
  return len >= ns_.size() &&
         std::equal(ns_.begin(), ns_.end(), key);
}

bool Transaction::MergeIterator::snap_at_end() const {
  if (!reverse_) {
    if (snap_fwd_) return *snap_fwd_ == std::default_sentinel;
    if (snap_key_fwd_) return *snap_key_fwd_ == std::default_sentinel;
    return true;
  }
  if (snap_rev_) return *snap_rev_ == std::default_sentinel;
  if (snap_key_rev_) return *snap_key_rev_ == std::default_sentinel;
  return true;
}

void Transaction::MergeIterator::snap_step() {
  if (!reverse_) {
    if (snap_fwd_) ++(*snap_fwd_);
    else if (snap_key_fwd_) ++(*snap_key_fwd_);
  } else {
    if (snap_rev_) ++(*snap_rev_);
    else if (snap_key_rev_) ++(*snap_key_rev_);
  }
}

void Transaction::MergeIterator::load_snap_current() {
  snap_valid_ = false;

  if (snap_at_end()) return;

  const uint8_t *kp = nullptr;
  size_t klen = 0;

  if (!reverse_) {
    if (snap_fwd_) {
      const auto &entry = **snap_fwd_;
      kp = u8_data(entry.key);
      klen = entry.key.size();
      snap_val_ptr_ = u8_data(entry.value);
      snap_val_len_ = entry.value.size();
    } else if (snap_key_fwd_) {
      const auto &key_bytes = **snap_key_fwd_;
      kp = u8_data(key_bytes);
      klen = key_bytes.size();
      snap_val_ptr_ = nullptr;
      snap_val_len_ = 0;
    }
  } else {
    if (snap_rev_) {
      const auto &entry = **snap_rev_;
      kp = u8_data(entry.key);
      klen = entry.key.size();
      snap_val_ptr_ = u8_data(entry.value);
      snap_val_len_ = entry.value.size();
    } else if (snap_key_rev_) {
      const auto &key_bytes = **snap_key_rev_;
      kp = u8_data(key_bytes);
      klen = key_bytes.size();
      snap_val_ptr_ = nullptr;
      snap_val_len_ = 0;
    }
  }

  if (!kp) return;

  if (!in_namespace(kp, klen)) {
    return;
  }

  if (!reverse_ && !bound_.empty() &&
      lex_compare(kp, klen, bound_.data(), bound_.size()) >= 0) {
    return;
  }

  snap_key_ptr_ = kp;
  snap_key_len_ = klen;
  snap_valid_ = true;
}

void Transaction::MergeIterator::buf_step() {
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

bool Transaction::MergeIterator::buf_candidate_valid() const {
  if (buf_it_ == buf_end_) return false;
  const auto &bk = buf_it_->first;
  if (!reverse_ && !bound_.empty() &&
      lex_compare(bk.data(), bk.size(), bound_.data(), bound_.size()) >= 0) {
    return false;
  }
  return in_namespace(bk.data(), bk.size());
}

void Transaction::MergeIterator::emit_buf() {
  cur_key_ = buf_it_->first;
  assign_bytes(cur_val_, buf_it_->second.value());
  buf_step();
  valid_ = true;
}

void Transaction::MergeIterator::emit_snap() {
  cur_key_.assign(snap_key_ptr_, snap_key_ptr_ + snap_key_len_);
  assign_bytes(cur_val_, snap_val_ptr_, snap_val_len_);
  snap_step();
  load_snap_current();
  valid_ = true;
}

void Transaction::MergeIterator::advance() {
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

    // Both sides have a candidate.
    const auto &bk = buf_it_->first;
    const int cmp = lex_compare(bk.data(), bk.size(), snap_key_ptr_, snap_key_len_);

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

}  // namespace bytecask::sql
