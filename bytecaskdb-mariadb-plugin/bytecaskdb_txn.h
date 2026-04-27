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

#include "bytecask_c.h"

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
  // -------------------------------------------------------------------

  class MergeIterator {
  public:
    // Takes ownership of snap_iter. lo/hi define the key range [lo, hi).
    MergeIterator(bytecask_iter_t *snap_iter,
                  LookupMap::const_iterator buf_it,
                  LookupMap::const_iterator buf_end,
                  std::vector<uint8_t> hi,
                  uint32_t table_id);
    ~MergeIterator();

    MergeIterator(const MergeIterator &) = delete;
    MergeIterator &operator=(const MergeIterator &) = delete;

    bool valid() const { return valid_; }
    void next();

    // Returns pointers to internally owned buffers. Valid until next().
    const uint8_t *key_data() const { return cur_key_.data(); }
    size_t key_len() const { return cur_key_.size(); }
    const uint8_t *value_data() const { return cur_val_.data(); }
    size_t value_len() const { return cur_val_.size(); }

  private:
    void advance();
    void load_snap_current();
    bool snap_in_range() const;

    bytecask_iter_t *snap_iter_;
    LookupMap::const_iterator buf_it_;
    LookupMap::const_iterator buf_end_;
    std::vector<uint8_t> hi_;
    uint32_t table_id_;

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

  explicit MariaDBTxn(bytecask_db_t *db) : db_(db) {}
  ~MariaDBTxn();

  // Acquires snapshot if not held, registers with MariaDB's transaction
  // coordinator via trans_register_ha.
  void begin_if_needed(THD *thd, handlerton *hton);

  bool is_active() const { return snap_ != nullptr; }

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
  // Returns 1 if found (writes to out_val/out_val_len), 0 if not found,
  // -1 on error.
  int get(const uint8_t *key, size_t klen,
          uint8_t **out_val, size_t *out_val_len);

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

  // -------------------------------------------------------------------
  // Commit / rollback
  // -------------------------------------------------------------------

  // Returns 0 on success, HA_ERR_LOCK_DEADLOCK on conflict,
  // HA_ERR_INTERNAL_ERROR on engine error.
  int commit(THD *thd, bool all);
  void rollback(THD *thd, bool all);

private:
  void reset();

  bytecask_db_t *db_;
  bytecask_snapshot_t *snap_{nullptr};

  // Ordered operation log — replayed into WritePlan at commit time.
  std::vector<Op> ops_;

  // RYOW overlay — fast lookups by key.  nullopt = tombstone.
  LookupMap lookup_;

  bool registered_stmt_{false};
  bool registered_all_{false};
};

} // namespace bytecaskdb
