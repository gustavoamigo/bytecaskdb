// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecaskdb_txn.h — Per-THD transaction state for the ByteCaskDB MariaDB plugin.
//
// MariaDBTxn buffers writes and reads from a snapshot, giving statement-level
// atomicity, session-level BEGIN/COMMIT/ROLLBACK, read-your-own-writes (RYOW),
// and OCC conflict detection at commit time.
//
// Dual-structure write buffer:
//   ops_    — ordered operation log (preserves cross-key causality for commit)
//   lookup_ — sorted map overlay for O(log n) RYOW lookups
//
// At commit, ops_ is replayed into a WritePlan in insertion order — matching
// how WritePlan stores operations internally (std::vector<WriteOp>).

#pragma once

#include "bytecask.hpp"
#include "bytecask_view.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

// Forward declarations for MariaDB types.
struct THD;
struct handlerton;

namespace bytecaskdb {

class MariaDBTxn {
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

  // -------------------------------------------------------------------
  // MergeIterator — two-pointer merge of snapshot iter + lookup_ map.
  //
  // The snapshot iterator is one of bytecask::EntryIterator (forward) or
  // bytecask::ReverseEntryIterator (reverse). Stored as optionals; at most
  // one is engaged. The buffer side is always walked forward (matching the
  // pre-migration C-API behavior).
  // -------------------------------------------------------------------

  class MergeIterator {
  public:
    // Forward construction (full table).
    MergeIterator(std::optional<bytecask::EntryIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> hi,
                  uint32_t table_id);

    // Forward construction (secondary index — key-only snapshot).
    MergeIterator(std::optional<bytecask::KeyIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> hi,
                  uint32_t table_id, uint16_t index_id);

    // Reverse construction (full table).
    MergeIterator(std::optional<bytecask::ReverseEntryIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> lo,
                  uint32_t table_id);

    // Reverse construction (secondary index — key-only snapshot).
    MergeIterator(std::optional<bytecask::ReverseKeyIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> lo,
                  uint32_t table_id, uint16_t index_id);

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

    // At most one of these is engaged.
    std::optional<bytecask::EntryIterator>        snap_fwd_;
    std::optional<bytecask::ReverseEntryIterator> snap_rev_;
    std::optional<bytecask::KeyIterator>          snap_key_fwd_;
    std::optional<bytecask::ReverseKeyIterator>   snap_key_rev_;
    bool reverse_{false};

    LookupMap::const_iterator buf_it_;
    LookupMap::const_iterator buf_end_;
    std::vector<uint8_t> bound_;            // forward: hi (exclusive); reverse: lo (informational only)
    uint32_t table_id_;
    uint16_t index_id_{0};        // 0 = primary key (table) iteration
    bool use_index_filter_{false}; // true = use key_belongs_to_index

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

  explicit MariaDBTxn(bytecask::DB *db) : db_(db) {}
  ~MariaDBTxn() = default;  // optional<Snapshot> handles cleanup

  // Acquires snapshot if not held, registers with MariaDB's transaction
  // coordinator via trans_register_ha.
  void begin_if_needed(THD *thd, handlerton *hton);

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
  // A commit conflict on such a plan is reported as HA_ERR_FOUND_DUPP_KEY.
  // Cleared by reset() at commit/rollback.
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

  // Opens a merge iterator over [lo, hi) combining snapshot + buffer.
  std::unique_ptr<MergeIterator> iter_prefix(
      const uint8_t *lo, size_t lo_len,
      const uint8_t *hi, size_t hi_len,
      uint32_t table_id);

  std::unique_ptr<MergeIterator> riter_prefix(
      const uint8_t *hi, size_t hi_len,
      const uint8_t *lo, size_t lo_len,
      uint32_t table_id);

  // Opens a merge iterator over [lo, hi) for a specific secondary index.
  std::unique_ptr<MergeIterator> iter_index_prefix(
      const uint8_t *lo, size_t lo_len,
      const uint8_t *hi, size_t hi_len,
      uint32_t table_id, uint16_t index_id);

  std::unique_ptr<MergeIterator> riter_index_prefix(
      const uint8_t *hi, size_t hi_len,
      const uint8_t *lo, size_t lo_len,
      uint32_t table_id, uint16_t index_id);

  // -------------------------------------------------------------------
  // Commit / rollback / savepoints
  // -------------------------------------------------------------------

  // Returns 0 on success, HA_ERR_LOCK_DEADLOCK on conflict,
  // HA_ERR_INTERNAL_ERROR on engine error.
  int commit(THD *thd, bool all);
  void rollback(THD *thd, bool all);

  void savepoint_set(void *sv);
  void savepoint_rollback(void *sv);
  void savepoint_release(void *sv);

  // Row count tracking — called by write_row/delete_row to record the
  // delta so it can be reverted on rollback or commit failure.
  void track_row_count_delta(uint32_t table_id,
                             std::atomic<int64_t> *row_count,
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

  // Commits the pending batch. Returns 0, or HA_ERR_INTERNAL_ERROR on
  // engine failure. Resets the batch and clears bulk_seen_.
  int bulk_flush(bool sync);

  // Duplicate probes for bulk mode: check the current unflushed batch, then
  // the DB directly (already-flushed batches). No snapshot — the #sql-xxx
  // keyspace is exclusive for the duration of the ALTER.
  bool bulk_pk_exists(const uint8_t *key, std::size_t klen);
  bool bulk_unique_prefix_exists(const uint8_t *prefix, std::size_t plen);

private:
  void ensure_snapshot();
  void reset();
  void revert_row_count_deltas();
  void bulk_reset();

  bytecask::DB *db_;
  std::optional<bytecask::Snapshot> snap_;

  // Ordered operation log — replayed into WritePlan at commit time.
  std::vector<Op> ops_;

  // RYOW overlay — fast lookups by key.  nullopt = tombstone.
  LookupMap lookup_;

  struct RowCountDelta {
    int64_t delta{0};
    std::atomic<int64_t> *counter{nullptr};
  };

  // Per-table row count deltas accumulated during this transaction.
  // Reverted on rollback or commit failure.
  std::map<uint32_t, RowCountDelta> row_count_deltas_;

  bool registered_stmt_{false};
  bool registered_all_{false};

  // Set by begin_deferred_insert(); see that method. Reset by reset().
  bool deferred_insert_{false};

  // Bulk-copy mode state (see begin_bulk_copy). Isolated from ops_/lookup_.
  bool bulk_copy_mode_{false};
  bytecask::WritePlan bulk_plan_{};
  std::size_t bulk_bytes_{0};
  std::size_t bulk_flush_threshold_{0};
  std::set<std::vector<uint8_t>> bulk_seen_;
};

} // namespace bytecaskdb
