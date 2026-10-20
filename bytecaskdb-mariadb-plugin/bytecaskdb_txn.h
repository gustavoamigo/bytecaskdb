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

#pragma once

#include "bytecask.hpp"
#include "bytecask_view.h"
#include "bytecaskdb-sql-layer/txn/transaction.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Forward declarations for MariaDB types.
struct THD;
struct handlerton;

namespace bytecaskdb {

class ha_bytecaskdb;

class MariaDBTxn : public bytecask::sql::Transaction {
public:
  explicit MariaDBTxn(bytecask::DB *db);

  // Acquires snapshot if not held, registers with MariaDB's transaction
  // coordinator via trans_register_ha.
  void begin_if_needed(THD *thd, handlerton *hton);

  // Enter deferred-dup-check mode for a plain autocommit INSERT (see
  // Transaction::begin_deferred_insert). A commit conflict on such a plan is
  // reported as HA_ERR_FOUND_DUPP_KEY, with the duplicate's key value
  // rendered by `handler`, which owns the TABLE the server's formatter
  // needs. The handler stays registered for the rest of the statement:
  // commit runs before the statement's tables are unlocked, and the handler
  // deregisters itself on unlock/close in case that order is ever different.
  // Cleared when the transaction leaves deferred mode.
  // Returns true if it reported the error; false to fall back to the
  // generic report (the key belongs to a table the reporter does not own).
  // Given the duplicate key and the row the INSERT tried to write with it.
  using DupKeyReporter = std::function<bool(const std::vector<uint8_t> &pk,
                                            const std::vector<uint8_t> &row)>;
  void begin_deferred_insert(const ha_bytecaskdb *handler,
                             DupKeyReporter reporter) {
    Transaction::begin_deferred_insert();
    deferred_handler_ = handler;
    deferred_reporter_ = std::move(reporter);
  }
  void forget_deferred_handler(const ha_bytecaskdb *handler) {
    if (deferred_handler_ == handler) {
      deferred_handler_ = nullptr;
      deferred_reporter_ = nullptr;
    }
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

  // A null `row_count` names the table's catalog counter.
  void track_row_count_delta(uint32_t table_id,
                             std::atomic<int64_t> *row_count,
                             int64_t delta);

  // Commits the pending bulk-copy batch. Returns 0, or HA_ERR_INTERNAL_ERROR
  // on engine failure.
  int bulk_flush(bool sync);

private:
  // Clears the handler once the base class has left deferred mode.
  void sync_deferred_handler();

  // Raises ER_DUP_ENTRY for a deferred insert's duplicate through the
  // registered handler (with the key value) or, without one, with an empty
  // value.
  void report_deferred_dup_key(const bytecask::sql::CommitOutcome &out);

  const ha_bytecaskdb *deferred_handler_{nullptr};  // identity only
  DupKeyReporter deferred_reporter_;
};

} // namespace bytecaskdb
