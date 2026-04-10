// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// ha_bytecaskdb.h — MariaDB storage engine handler for ByteCaskDB.
//
// Phase 1 scope: CREATE TABLE, INSERT, SELECT *, DROP TABLE.
// No UPDATE, DELETE, secondary indexes, or transactions.

#pragma once

// MariaDB handler API — included from the MariaDB development package.
// my_global.h must come first: it defines uchar, unlikely(), and other
// prerequisite macros that handler.h and the private server headers rely on.
#include "my_global.h"
#include "handler.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bytecask_c.h"

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// Global state — shared across all tables on this server instance.
// Initialised once in bytecaskdb_init() / destroyed in bytecaskdb_deinit().
// Thread safety: bytecask_db_t itself is thread-safe for concurrent reads
// and serialised writes (ByteCaskDB SWMR model).  The global pointer is set
// once at plugin init and never mutated after that, so no locking needed.
// ---------------------------------------------------------------------------

extern bytecask_db_t *g_db;           // single DB instance for this server
extern handlerton     *bytecaskdb_hton;

// Assigns a monotonically increasing 32-bit table identifier to each table
// path, stored in memory only.  Phase 2 will persist this in the DB.
uint32_t get_or_assign_table_id(const std::string &table_path);
void     remove_table_id(const std::string &table_path);

// ---------------------------------------------------------------------------
// ha_bytecaskdb — the handler class registered with MariaDB.
// ---------------------------------------------------------------------------

class ha_bytecaskdb : public handler {
public:
  ha_bytecaskdb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_bytecaskdb() override = default;

  // -------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------

  // Called when MariaDB executes CREATE TABLE.  We open a ByteCaskDB
  // under the given path to verify it can be created.
  int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *create_info) override;

  // Called when a statement opens the table.
  int open(const char *name, int mode, uint test_if_locked) override;

  // Called when a statement closes the table.
  int close() override;

  // Called when MariaDB executes DROP TABLE.
  int delete_table(const char *name) override;

  // -------------------------------------------------------------------
  // Write path
  // -------------------------------------------------------------------

  // Called for each INSERT row.
  int write_row(const uchar *buf) override;

  // Phase 1: UPDATE not supported.
  int update_row(const uchar * /*old_data*/,
                 const uchar * /*new_data*/) override {
    return HA_ERR_WRONG_COMMAND;
  }

  // Phase 1: DELETE not supported.
  int delete_row(const uchar * /*buf*/) override {
    return HA_ERR_WRONG_COMMAND;
  }

  // -------------------------------------------------------------------
  // Full table scan (SELECT * FROM t)
  // -------------------------------------------------------------------

  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_end() override;

  // -------------------------------------------------------------------
  // Position / random access by row reference
  // -------------------------------------------------------------------

  // Stores the PK of table->record[0] into ref.
  void position(const uchar *record) override;
  // Fetches the row for the PK stored in ref.
  int rnd_pos(uchar *buf, uchar *pos) override;

  // -------------------------------------------------------------------
  // Metadata / capabilities
  // -------------------------------------------------------------------

  // Returns capability flags for this engine.
  ulonglong table_flags() const override {
    return HA_NO_TRANSACTIONS | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_REC_NOT_IN_SEQ;
  }

  // Returns capability flags for the given index.
  ulong index_flags(uint /*idx*/, uint /*part*/,
                    bool /*all_parts*/) const override {
    return 0;
  }

  // Called by the optimizer to gather row-count / key-count statistics.
  // We return HA_POS_ERROR (unknown) for now.
  int info(uint flag) override;

  // Returns the maximum key length (not used in Phase 1).
  uint max_supported_key_length() const override {
    return MAX_KEY_LENGTH;
  }

  // Returns the maximum number of keys (not used in Phase 1).
  uint max_supported_keys() const override { return MAX_KEY; }

  // Lock management — no-op for Phase 1.
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

private:
  // Encodes the primary key of row `buf` into a ByteCaskDB key prefixed with
  // this table's 4-byte big-endian table_id.  Returns the encoded key bytes.
  std::vector<uint8_t> encode_pk(const uchar *buf) const;

  uint32_t table_id_{0};     // assigned in open()

  // Iterator state for the current full-table scan.
  bytecask_iter_t *scan_iter_{nullptr};

  // Prefix used to scope this table's keys in the shared DB.
  // encode_pk_prefix() returns the 4-byte big-endian table_id used as
  // the lower bound for iter_from() and the boundary check in rnd_next().
  static void write_table_id_prefix(uint8_t *buf4, uint32_t table_id);
};

} // namespace bytecaskdb
