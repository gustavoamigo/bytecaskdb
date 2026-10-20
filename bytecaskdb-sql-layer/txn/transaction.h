// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// transaction.h — a buffered OCC transaction over the engine's public API.
//
// Transaction buffers writes and reads from a snapshot, giving statement-level
// atomicity, session-level BEGIN/COMMIT/ROLLBACK, read-your-own-writes (RYOW),
// and OCC conflict detection at commit time. It knows nothing of a SQL
// server: the MariaDB plugin derives its per-THD adapter from it, which
// registers with the server's transaction coordinator and maps a
// CommitOutcome to an HA_ERR_* code (docs/transaction_design.md).
//
// Dual-structure write buffer:
//   ops_    — ordered operation log (preserves cross-key causality for commit)
//   lookup_ — sorted map overlay for O(log n) RYOW lookups
//
// At commit, ops_ is replayed into a WritePlan in insertion order — matching
// how WritePlan stores operations internally (std::vector<WriteOp>).
//
// A plain header and not a module: the plugin, built by CMake with another
// toolchain, compiles transaction.cpp itself.
//
// Not thread-safe: one transaction belongs to one session.

#pragma once

#include "include/bytecask.hpp"
#include "bytecaskdb-sql-layer/common/bytecask_view.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace bytecask::sql {

// What commit() or bulk_flush() did.
struct CommitOutcome {
  enum class Status {
    Committed,
    // The plan's write-write check or a guard failed: nothing was written.
    Conflict,
    // A deferred insert's ensure_absent guard failed (begin_deferred_insert):
    // nothing was written. duplicate names the key.
    DuplicateKey,
    // The engine threw, or refused a plan that cannot conflict: error holds
    // the exception, or is null for the refusal. Nothing this transaction
    // buffered is visible, though a bulk copy's earlier flushes are.
    EngineError,
  };
  // DuplicateKey: the first guarded key that now exists, and the row the
  // insert tried to write with it.
  struct Duplicate {
    std::vector<uint8_t> key;
    std::vector<uint8_t> row;
  };

  Status status{Status::Committed};
  // DuplicateKey only; nullopt when no guarded key exists any more.
  std::optional<Duplicate> duplicate;
  std::exception_ptr error;
};

class Transaction {
public:
  // Operation kinds for the ordered log.
  struct Op {
    enum Kind { Put, Del };
    Kind kind;
    std::vector<uint8_t> key;
    std::vector<uint8_t> val;  // empty for Del
    // Put only: emit ensure_absent(key) into the WritePlan before the put.
    // Used by the deferred INSERT path to carry the PK dup check to commit.
    bool guard_absent{false};
  };

  // RYOW lookup type: nullopt = tombstone (deleted within this txn).
  using LookupMap =
      std::map<std::vector<uint8_t>, std::optional<std::vector<uint8_t>>>;

  // Read options for every read, fetched per read so a caller's setting
  // takes effect at once. Null reads with the engine's defaults.
  using ReadOptionsFn = bytecask::ReadOptions (*)();

  // -------------------------------------------------------------------
  // MergeIterator — two-pointer merge of snapshot iter + lookup_ map.
  //
  // The snapshot iterator is one of bytecask::EntryIterator (forward) or
  // bytecask::ReverseEntryIterator (reverse), or their key-only forms.
  // Stored as optionals; at most one is engaged. The buffer side walks in
  // the same direction as the snapshot side, and on each step the smaller
  // (forward) or larger (reverse) key is emitted; on a tie the buffer wins
  // and a tombstone suppresses the snapshot key. Only keys that start with
  // the scan's namespace prefix `ns` are emitted.
  // -------------------------------------------------------------------

  class MergeIterator {
  public:
    // Forward construction (entries).
    MergeIterator(std::optional<bytecask::EntryIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> hi,
                  std::vector<uint8_t> ns);

    // Forward construction (key-only snapshot).
    MergeIterator(std::optional<bytecask::KeyIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> hi,
                  std::vector<uint8_t> ns);

    // Reverse construction (entries). buf_it is the largest buffered key
    // <= hi, or buf_end when there is none.
    MergeIterator(std::optional<bytecask::ReverseEntryIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_begin,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> lo,
                  std::vector<uint8_t> ns);

    // Reverse construction (key-only snapshot).
    MergeIterator(std::optional<bytecask::ReverseKeyIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_begin,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> lo,
                  std::vector<uint8_t> ns);

    ~MergeIterator() = default;

    MergeIterator(const MergeIterator &) = delete;
    MergeIterator &operator=(const MergeIterator &) = delete;

    bool valid() const { return valid_; }
    void next();

    // Pointers into internally owned buffers; valid until next().
    const uint8_t *key_data() const { return cur_key_.data(); }
    size_t key_len() const { return cur_key_.size(); }
    const uint8_t *value_data() const {
      return reinterpret_cast<const uint8_t *>(cur_val_.data());
    }
    size_t value_len() const { return cur_val_.size(); }

    // Swap buffers with caller — preserves capacity across rows, avoiding
    // per-row heap allocations in steady state.
    void swap_value(bytecask::Bytes &other) { cur_val_.swap(other); }
    void swap_key(std::vector<uint8_t> &other) { cur_key_.swap(other); }

    // Moves cur_val_ out — caller takes ownership. Iterator value is left empty.
    bytecask::Bytes steal_value() { return std::move(cur_val_); }

  private:
    void advance();
    void load_snap_current();
    bool snap_at_end() const;
    void snap_step();
    void buf_step();
    // True when the buffer cursor holds a candidate inside this scan's
    // namespace and, for forward scans, below the hi bound.
    bool buf_candidate_valid() const;
    bool in_namespace(const uint8_t *key, size_t len) const;
    void emit_buf();
    void emit_snap();

    // At most one of these is engaged.
    std::optional<bytecask::EntryIterator>        snap_fwd_;
    std::optional<bytecask::ReverseEntryIterator> snap_rev_;
    std::optional<bytecask::KeyIterator>          snap_key_fwd_;
    std::optional<bytecask::ReverseKeyIterator>   snap_key_rev_;
    bool reverse_{false};

    // Buffer cursor. Forward: buf_it_ walks [buf_it_, buf_end_). Reverse:
    // buf_it_ is the current candidate and steps toward buf_begin_;
    // buf_end_ marks exhaustion in both directions.
    LookupMap::const_iterator buf_it_;
    LookupMap::const_iterator buf_begin_;
    LookupMap::const_iterator buf_end_;
    std::vector<uint8_t> bound_;  // forward: hi (exclusive); reverse: lo (informational only)
    std::vector<uint8_t> ns_;     // every emitted key starts with it

    // Cached snapshot key/value — points into mmap (or io_buf for active file).
    // Valid until snap_step() is called.
    const uint8_t *snap_key_ptr_{nullptr};
    size_t snap_key_len_{0};
    const uint8_t *snap_val_ptr_{nullptr};
    size_t snap_val_len_{0};
    bool snap_valid_{false};

    // Current output.
    std::vector<uint8_t> cur_key_;
    bytecask::Bytes cur_val_;
    bool valid_{false};
  };

  // -------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------

  // `limits` bound every snapshot-less WritePlan this transaction builds (a
  // deferred insert's commit and each bulk-copy batch); the plan checks
  // them, the engine's defaults if none are given, so they should be the
  // limits the DB was opened with.
  Transaction(bytecask::DB *db, bytecask::SizeLimits limits,
              ReadOptionsFn read_options = nullptr)
      : db_(db), limits_(limits), read_options_(read_options),
        bulk_plan_{limits} {}
  ~Transaction() = default;  // optional<Snapshot> handles cleanup

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  // What begin_if_needed started: the caller registers each with its
  // transaction coordinator, the transaction before the statement.
  struct Begun {
    bool transaction{false};
    bool statement{false};
  };

  // Starts a statement if none is open, and with `multi` (a multi-statement
  // transaction: BEGIN, or autocommit off) a transaction if none is open,
  // acquiring the snapshot the transaction reads from.
  Begun begin_if_needed(bool multi);

  bool is_active() const { return snap_.has_value(); }

  // -------------------------------------------------------------------
  // Write buffering
  // -------------------------------------------------------------------

  void buffer_put(const uint8_t *key, size_t klen,
                  const uint8_t *val, size_t vlen,
                  bool guard_absent = false);
  void buffer_del(const uint8_t *key, size_t klen);

  // Enter deferred-dup-check mode for a plain autocommit INSERT: no snapshot
  // is acquired, and commit() builds a snapshot-less WritePlan whose
  // ensure_absent guards (see Op::guard_absent) carry the PK dup check.
  // A commit conflict on such a plan is a DuplicateKey outcome. Cleared when
  // the statement commits or rolls back.
  bool in_deferred_insert() const { return deferred_insert_; }
  // True once this statement/transaction holds a snapshot or buffered
  // writes. Deferred-INSERT mode may only begin before either exists.
  bool has_state() const { return snap_.has_value() || !ops_.empty(); }
  void begin_deferred_insert() { deferred_insert_ = true; }

  // In-memory presence probe against the write buffer only (no snapshot, no
  // DB access). Used by the deferred INSERT path to catch duplicates within
  // the same statement.
  bool buffered_key_present(const uint8_t *key, size_t klen);

  // -------------------------------------------------------------------
  // RYOW reads
  // -------------------------------------------------------------------

  // Checks lookup_ first (tombstone = not found), then snapshot.
  // Returns 1 if found (writes to out), 0 if not found, -1 on error.
  int get(const uint8_t *key, size_t klen, bytecask::Bytes &out);

  // Returns true if key exists (in buffer or snapshot).
  bool exists(const uint8_t *key, size_t klen);

  // Keys in [lo, hi) this transaction sees — its snapshot plus its own
  // buffered writes — counted no further than `limit`. The snapshot's count
  // reads at most two records; each buffered key in the range costs one
  // existence check, and the checks stop at `limit`. 0 if lo >= hi.
  std::size_t count_range(const uint8_t *lo, size_t lo_len,
                          const uint8_t *hi, size_t hi_len, std::size_t limit);

  // Merge iterators over [lo, hi) combining snapshot + buffer, emitting only
  // keys that start with `ns`: entries (keys and values), or keys only. The
  // reverse forms start at the largest key <= hi and stop at the namespace's
  // end.
  std::unique_ptr<MergeIterator> iter_entries(
      const uint8_t *lo, size_t lo_len,
      const uint8_t *hi, size_t hi_len,
      std::vector<uint8_t> ns);

  std::unique_ptr<MergeIterator> riter_entries(
      const uint8_t *hi, size_t hi_len,
      const uint8_t *lo, size_t lo_len,
      std::vector<uint8_t> ns);

  std::unique_ptr<MergeIterator> iter_keys(
      const uint8_t *lo, size_t lo_len,
      const uint8_t *hi, size_t hi_len,
      std::vector<uint8_t> ns);

  std::unique_ptr<MergeIterator> riter_keys(
      const uint8_t *hi, size_t hi_len,
      const uint8_t *lo, size_t lo_len,
      std::vector<uint8_t> ns);

  // -------------------------------------------------------------------
  // Commit / rollback / savepoints
  // -------------------------------------------------------------------

  // `all` commits the transaction; without it, the statement. A statement
  // commit inside a multi-statement transaction keeps the buffer for the
  // transaction's commit; any other commit applies it, fdatasync'd when
  // `sync`. On any outcome but Committed the row-count deltas are reverted.
  CommitOutcome commit(bool all, bool sync);
  void rollback(bool all);

  // A savepoint is named by the index this returns; rolling back to it
  // keeps it, and releasing it drops it and every later one.
  std::uint32_t savepoint_set();
  void savepoint_rollback(std::uint32_t savepoint);
  void savepoint_release(std::uint32_t savepoint);

  // Row count tracking — called by write_row/delete_row to record the
  // delta so it can be reverted on rollback or commit failure. Applies
  // `delta` to `row_count` now.
  void track_row_count_delta(uint32_t table_id,
                             std::atomic<int64_t> &row_count,
                             int64_t delta);

  // -------------------------------------------------------------------
  // Bulk-copy mode — batched writes for ALTER TABLE ... ALGORITHM=COPY
  //
  // The copy loop targets a hidden #sql-xxx table whose keyspace no other
  // transaction can see until the final catalog rename. In this mode
  // write_row's puts bypass ops_/lookup_/snap_ entirely (leaving the
  // concurrently-running source-table MergeIterator untouched) and land in
  // bulk_plan_, which is flushed to the DB in fixed-size batches. Each flush
  // is its own atomic apply_batch; a crash mid-copy leaves an invisible
  // orphan keyspace that MariaDB's ddl_log reclaims via delete_table.
  // -------------------------------------------------------------------

  void begin_bulk_copy(std::size_t flush_threshold_bytes);
  bool in_bulk_copy() const { return bulk_copy_mode_; }
  void end_bulk_copy() { bulk_copy_mode_ = false; }

  // Drop the pending batch and leave bulk-copy mode. Rows already flushed to
  // the #sql-xxx keyspace are reclaimed when MariaDB drops the temp table.
  void abort_bulk_copy() { bulk_reset(); }

  // Appends a put to the pending batch and tracks its size.
  void bulk_buffer_put(const uint8_t *key, std::size_t klen,
                       const uint8_t *val, std::size_t vlen);

  // Records a key (PK key or unique-index prefix) for within-batch
  // duplicate detection. Cleared on every flush.
  void bulk_note_key(const uint8_t *key, std::size_t klen);

  bool bulk_should_flush() const {
    return bulk_bytes_ >= bulk_flush_threshold_;
  }

  // Commits the pending batch: Committed, or EngineError. Resets the batch
  // and clears bulk_seen_ either way.
  CommitOutcome bulk_flush(bool sync);

  // Duplicate probes for bulk mode: check the current unflushed batch, then
  // the DB directly (already-flushed batches). No snapshot — the #sql-xxx
  // keyspace is exclusive for the duration of the ALTER.
  bool bulk_pk_exists(const uint8_t *key, std::size_t klen);
  bool bulk_unique_prefix_exists(const uint8_t *prefix, std::size_t plen);

protected:
  bytecask::DB &db() const { return *db_; }

private:
  bytecask::ReadOptions read_options() const {
    return read_options_ ? read_options_() : bytecask::ReadOptions{};
  }
  void ensure_snapshot();
  LookupMap::const_iterator
  reverse_buffer_start(const std::vector<uint8_t> &hi) const;
  void reset();
  void revert_row_count_deltas();
  void bulk_reset();
  // The outcome of a failed commit: reverts the row-count deltas and
  // resets the transaction.
  CommitOutcome fail(CommitOutcome outcome);

  struct RowCountDelta {
    int64_t delta{0};
    std::atomic<int64_t> *counter{nullptr};
  };
  using RowCountDeltas = std::map<uint32_t, RowCountDelta>;

  // Rewinds the write buffer to its first `mark` ops and rebuilds the RYOW
  // overlay from what remains. Shared by statement rollback and savepoints.
  void truncate_ops(std::size_t mark);
  // Undoes the row-count changes made since `saved` was captured and makes
  // `saved` the current delta set. Shared by statement rollback and savepoints.
  void restore_row_count_deltas(const RowCountDeltas &saved);

  // A deferred-INSERT commit conflict as a DuplicateKey outcome: the first
  // guarded key that now exists, and its row.
  CommitOutcome deferred_dup_key() const;

  bytecask::DB *db_;
  bytecask::SizeLimits limits_;
  ReadOptionsFn read_options_;
  std::optional<bytecask::Snapshot> snap_;

  // Ordered operation log — replayed into WritePlan at commit time.
  std::vector<Op> ops_;

  // RYOW overlay — fast lookups by key.  nullopt = tombstone.
  LookupMap lookup_;

  // Per-table row count deltas accumulated during this transaction.
  // Reverted on rollback or commit failure.
  RowCountDeltas row_count_deltas_;

  // Where the current statement began, captured when the statement begins
  // in begin_if_needed. A statement-level rollback inside a
  // multi-statement transaction rewinds to here and leaves earlier
  // statements' work in the buffer.
  std::size_t stmt_ops_mark_{0};
  RowCountDeltas stmt_row_count_deltas_;

  struct Savepoint {
    std::size_t ops_mark;
    RowCountDeltas row_count_deltas;
  };
  std::vector<Savepoint> savepoints_;

  bool in_stmt_{false};
  bool in_txn_{false};

  // Set by begin_deferred_insert(); see that method. Reset by reset().
  bool deferred_insert_{false};

  // Bulk-copy mode state (see begin_bulk_copy). Isolated from ops_/lookup_.
  bool bulk_copy_mode_{false};
  bytecask::WritePlan bulk_plan_;
  std::size_t bulk_bytes_{0};
  std::size_t bulk_flush_threshold_{0};
  std::set<std::vector<uint8_t>> bulk_seen_;
};

}  // namespace bytecask::sql
