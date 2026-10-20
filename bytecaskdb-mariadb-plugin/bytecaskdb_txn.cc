// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecaskdb_txn.cc — MariaDBTxn: the SQL layer's transaction, registered
// with MariaDB.

#include "bytecaskdb_txn.h"
#include "degraded.h"
#include "ha_bytecaskdb.h"
#include "key_encoding.h"

#include "my_global.h"
#include "handler.h"
#include "mysql/plugin.h"
#include "sql_priv.h"
#include "mysqld_error.h"

#include <cstdio>
#include <exception>

namespace bytecaskdb {

using Status = bytecask::sql::CommitOutcome::Status;

namespace {

// Logs an engine error the transaction caught, as the plugin always has.
void log_engine_error(const char *what, const std::exception_ptr &error) {
  if (!error) { return; }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &e) {
    fprintf(stderr, "[bytecaskdb] %s failed: %s\n", what, e.what());
  } catch (...) {
  }
}

}  // namespace

MariaDBTxn::MariaDBTxn(bytecask::DB *db)
    : Transaction(db, kPluginSizeLimits, &plugin_read_options) {}

void MariaDBTxn::begin_if_needed(THD *thd, handlerton *hton) {
  const bool multi = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  const auto begun = Transaction::begin_if_needed(multi);
  if (begun.transaction) {
    trans_register_ha(thd, true, hton, 0);
  }
  if (begun.statement) {
    trans_register_ha(thd, false, hton, 0);
  }
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::iter_prefix(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    uint32_t table_id) {
  return iter_entries(lo, lo_len, hi, hi_len, table_id_prefix(table_id));
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::riter_prefix(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    uint32_t table_id) {
  return riter_entries(hi, hi_len, lo, lo_len, table_id_prefix(table_id));
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::iter_index_prefix(
    const uint8_t *lo, size_t lo_len,
    const uint8_t *hi, size_t hi_len,
    uint32_t table_id, uint16_t index_id) {
  return iter_keys(lo, lo_len, hi, hi_len, index_id_prefix(table_id, index_id));
}

std::unique_ptr<MariaDBTxn::MergeIterator> MariaDBTxn::riter_index_prefix(
    const uint8_t *hi, size_t hi_len,
    const uint8_t *lo, size_t lo_len,
    uint32_t table_id, uint16_t index_id) {
  return riter_keys(hi, hi_len, lo, lo_len, index_id_prefix(table_id, index_id));
}

int MariaDBTxn::commit(THD * /*thd*/, bool all) {
  const auto out = Transaction::commit(all, plugin_sync_at_commit());
  int rc = 0;
  switch (out.status) {
  case Status::Committed:
    break;
  case Status::Conflict:
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    rc = HA_ERR_LOCK_DEADLOCK;
    break;
  case Status::DuplicateKey:
    report_deferred_dup_key(out);
    rc = HA_ERR_FOUND_DUPP_KEY;
    break;
  case Status::EngineError:
    abort_if_degraded(db(), "commit");
    log_engine_error("commit", out.error);
    rc = HA_ERR_INTERNAL_ERROR;
    break;
  }
  sync_deferred_handler();
  return rc;
}

void MariaDBTxn::rollback(THD * /*thd*/, bool all) {
  Transaction::rollback(all);
  sync_deferred_handler();
}

void MariaDBTxn::sync_deferred_handler() {
  if (!in_deferred_insert()) {
    deferred_handler_ = nullptr;
    deferred_reporter_ = nullptr;
  }
}

void MariaDBTxn::report_deferred_dup_key(
    const bytecask::sql::CommitOutcome &out) {
  if (deferred_reporter_ && out.duplicate &&
      deferred_reporter_(out.duplicate->key, out.duplicate->row)) {
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

void MariaDBTxn::track_row_count_delta(uint32_t table_id,
                                       std::atomic<int64_t> *row_count,
                                       int64_t delta) {
  Transaction::track_row_count_delta(
      table_id, row_count ? *row_count : *catalog_row_count_ptr(table_id),
      delta);
}

int MariaDBTxn::bulk_flush(bool sync) {
  const auto out = Transaction::bulk_flush(sync);
  if (out.status == Status::Committed) { return 0; }
  if (out.error) {
    abort_if_degraded(db(), "bulk flush");
    log_engine_error("bulk flush", out.error);
  }
  return HA_ERR_INTERNAL_ERROR;
}

void MariaDBTxn::savepoint_set(void *sv) {
  *static_cast<uint32_t *>(sv) = Transaction::savepoint_set();
}

void MariaDBTxn::savepoint_rollback(void *sv) {
  Transaction::savepoint_rollback(*static_cast<const uint32_t *>(sv));
}

void MariaDBTxn::savepoint_release(void *sv) {
  Transaction::savepoint_release(*static_cast<const uint32_t *>(sv));
}

} // namespace bytecaskdb
