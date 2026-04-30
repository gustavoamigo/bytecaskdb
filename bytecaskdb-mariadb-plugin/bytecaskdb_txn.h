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

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
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

    // Forward construction (secondary index).
    MergeIterator(std::optional<bytecask::EntryIterator> snap_it,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> hi,
                  uint32_t table_id, uint16_t index_id);

    // Reverse construction (full table).
    MergeIterator(std::optional<bytecask::ReverseEntryIterator> snap_it,
                  bytecask::ReverseEntryIterator snap_end,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> lo,
                  uint32_t table_id);

    // Reverse construction (secondary index).
    MergeIterator(std::optional<bytecask::ReverseEntryIterator> snap_it,
                  bytecask::ReverseEntryIterator snap_end,
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
    const uint8_t *value_data() const { return cur_val_.data(); }
    size_t value_len() const { return cur_val_.size(); }

  private:
    void advance();
    void load_snap_current();
    bool snap_at_end() const;
    void snap_step();

    // At most one of these is engaged.
    std::optional<bytecask::EntryIterator>        snap_fwd_;
    std::optional<bytecask::ReverseEntryIterator> snap_rev_;
    // For reverse iteration the sentinel is a same-typed iterator value.
    std::optional<bytecask::ReverseEntryIterator> snap_rev_end_;
    bool reverse_{false};

    LookupMap::const_iterator buf_it_;
    LookupMap::const_iterator buf_end_;
    std::vector<uint8_t> bound_;            // forward: hi (exclusive); reverse: lo (informational only)
    uint32_t table_id_;
    uint16_t index_id_{0};        // 0 = primary key (table) iteration
    bool use_index_filter_{false}; // true = use key_belongs_to_index

    // Cached snapshot key/value (owned copies).
    std::vector<uint8_t> snap_key_;
    std::vector<uint8_t> snap_val_;
    bool snap_valid_{false};

    // Current output (owned copies).
    std::vector<uint8_t> cur_key_;
    std::vector<uint8_t> cur_val_;
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
                  const uint8_t *val, size_t vlen);
  void buffer_del(const uint8_t *key, size_t klen);

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
  // Commit / rollback
  // -------------------------------------------------------------------

  // Returns 0 on success, HA_ERR_LOCK_DEADLOCK on conflict,
  // HA_ERR_INTERNAL_ERROR on engine error.
  int commit(THD *thd, bool all);
  void rollback(THD *thd, bool all);

private:
  void reset();

  bytecask::DB *db_;
  std::optional<bytecask::Snapshot> snap_;

  // Ordered operation log — replayed into WritePlan at commit time.
  std::vector<Op> ops_;

  // RYOW overlay — fast lookups by key.  nullopt = tombstone.
  LookupMap lookup_;

  bool registered_stmt_{false};
  bool registered_all_{false};
};

} // namespace bytecaskdb
