// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecaskdb_txn.h — Per-THD transaction state for the ByteCaskDB MariaDB plugin.
//
// MariaDBTxn is the SQL layer's transaction (bytecaskdb-sql-layer/txn/,
// MIT) with MariaDB's contact points added: it registers the statement and
// the transaction with the server's coordinator, maps a commit outcome to an
// HA_ERR_* code and the error the server reports, stores savepoints in the
// server's slots, and names tables and indexes by id in the plugin's key
// layout. Buffering, RYOW reads, the merge iterators, OCC commit, savepoints,
// deferred-insert guards and bulk-copy mode are the base class's.
//
// The base is private, so every call goes through this adapter: a
// MariaDBTxn does not convert to a Transaction outside it.

#pragma once

#include "bytecask.hpp"
#include "bytecask_view.h"
#include "bytecaskdb-sql-layer/txn/transaction.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Forward declarations for MariaDB types.
struct THD;
struct handlerton;

namespace bytecaskdb {

class ha_bytecaskdb;

class MariaDBTxn : private bytecask::sql::Transaction {
public:
  using Transaction::MergeIterator;

  explicit MariaDBTxn(bytecask::DB *db);

  // Acquires snapshot if not held, registers with MariaDB's transaction
  // coordinator via trans_register_ha.
  void begin_if_needed(THD *thd, handlerton *hton);

  using Transaction::is_active;
  using Transaction::has_state;

  void buffer_put(const uint8_t *key, size_t klen,
                  const uint8_t *val, size_t vlen,
                  bool guard_absent = false) {
    Transaction::buffer_put({key, klen}, {val, vlen}, guard_absent);
  }
  void buffer_del(const uint8_t *key, size_t klen) {
    Transaction::buffer_del({key, klen});
  }

  // Enter deferred-dup-check mode for a plain autocommit INSERT (see
  // Transaction::begin_deferred_insert). A commit conflict on such a plan is
  // reported as HA_ERR_FOUND_DUPP_KEY, with the duplicate's key value
  // rendered by `handler`, which owns the TABLE the server's formatter
  // needs. The handler stays registered for the rest of the statement:
  // commit runs before the statement's tables are unlocked, and the handler
  // deregisters itself on unlock/close in case that order is ever different.
  // The reporter returns true if it reported the error; false to fall back
  // to the generic report (the key belongs to a table it does not own).
  using DupKeyReporter = DuplicateReporter;
  using Transaction::in_deferred_insert;
  void begin_deferred_insert(const ha_bytecaskdb *handler,
                             DupKeyReporter reporter) {
    Transaction::begin_deferred_insert(handler, std::move(reporter));
  }
  void forget_deferred_handler(const ha_bytecaskdb *handler) {
    forget_deferred_reporter(handler);
  }
  bool buffered_key_present(const uint8_t *key, size_t klen) const {
    return Transaction::buffered_key_present({key, klen});
  }

  // Returns 1 if found (writes to out), 0 if not found, -1 on error.
  int get(const uint8_t *key, size_t klen, bytecask::Bytes &out) {
    return Transaction::get({key, klen}, out);
  }
  bool exists(const uint8_t *key, size_t klen) {
    return Transaction::exists({key, klen});
  }
  std::size_t count_range(const uint8_t *lo, size_t lo_len,
                          const uint8_t *hi, size_t hi_len, std::size_t limit) {
    return Transaction::count_range({lo, lo_len}, {hi, hi_len}, limit);
  }

  // Merge iterators over [lo, hi) of a table's rows or one of its
  // secondary indexes, in the plugin's key layout (key_encoding.h).
  std::unique_ptr<MergeIterator> iter_prefix(
      const uint8_t *lo, size_t lo_len,
      const uint8_t *hi, size_t hi_len,
      uint32_t table_id);

  std::unique_ptr<MergeIterator> riter_prefix(
      const uint8_t *hi, size_t hi_len,
      const uint8_t *lo, size_t lo_len,
      uint32_t table_id);

  std::unique_ptr<MergeIterator> iter_index_prefix(
      const uint8_t *lo, size_t lo_len,
      const uint8_t *hi, size_t hi_len,
      uint32_t table_id, uint16_t index_id);

  std::unique_ptr<MergeIterator> riter_index_prefix(
      const uint8_t *hi, size_t hi_len,
      const uint8_t *lo, size_t lo_len,
      uint32_t table_id, uint16_t index_id);

  // Returns 0 on success, HA_ERR_LOCK_DEADLOCK on conflict,
  // HA_ERR_FOUND_DUPP_KEY on a deferred insert's duplicate,
  // HA_ERR_INTERNAL_ERROR on engine error; raises the matching server error.
  int commit(THD *thd, bool all);
  void rollback(THD *thd, bool all);

  // `sv` is the hton-sized slot the server gives each savepoint
  // (savepoint_offset = sizeof(uint32_t)); it holds the savepoint's index.
  void savepoint_set(void *sv);
  void savepoint_rollback(void *sv);
  void savepoint_release(void *sv);

  // `row_count` is the table's catalog counter, or null to look it up.
  void track_row_count_delta(uint32_t table_id,
                             std::atomic<int64_t> *row_count,
                             int64_t delta) {
    Transaction::track_row_count_delta(table_id, delta, row_count);
  }

  using Transaction::begin_bulk_copy;
  using Transaction::in_bulk_copy;
  using Transaction::end_bulk_copy;
  using Transaction::abort_bulk_copy;
  using Transaction::bulk_should_flush;

  void bulk_buffer_put(const uint8_t *key, std::size_t klen,
                       const uint8_t *val, std::size_t vlen) {
    Transaction::bulk_buffer_put({key, klen}, {val, vlen});
  }
  void bulk_note_key(const uint8_t *key, std::size_t klen) {
    Transaction::bulk_note_key({key, klen});
  }
  bool bulk_pk_exists(const uint8_t *key, std::size_t klen) {
    return Transaction::bulk_pk_exists({key, klen});
  }
  bool bulk_unique_prefix_exists(const uint8_t *prefix, std::size_t plen) {
    return Transaction::bulk_unique_prefix_exists({prefix, plen});
  }

  // Commits the pending bulk-copy batch. Returns 0, or HA_ERR_INTERNAL_ERROR
  // on engine failure.
  int bulk_flush(bool sync);

private:
  // Logs an engine error and aborts if it left the engine degraded, as the
  // plugin does for every engine write; returns HA_ERR_INTERNAL_ERROR.
  int engine_error(const bytecask::sql::CommitOutcome &out);
};

} // namespace bytecaskdb
