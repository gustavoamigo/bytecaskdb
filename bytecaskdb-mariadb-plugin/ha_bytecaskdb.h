// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// ha_bytecaskdb.h — MariaDB storage engine handler for ByteCaskDB.

#pragma once

#include "my_global.h"
#include "handler.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bytecask_c.h"
#include "bytecaskdb_txn.h"

namespace bytecaskdb {

struct TableMeta;

// ---------------------------------------------------------------------------
// Global state — shared across all tables on this server instance.
// ---------------------------------------------------------------------------

extern bytecask_db_t *g_db;
extern handlerton    *bytecaskdb_hton;

// ---------------------------------------------------------------------------
// Persistent catalog functions — defined in bytecaskdb_plugin.cc.
// ---------------------------------------------------------------------------

uint32_t         catalog_alloc_table_id(bytecask_db_t *db);
bool             catalog_put_table_meta(bytecask_db_t *db,
                                        const TableMeta &meta,
                                        const char *name);
bool             catalog_delete_table_meta(bytecask_db_t *db,
                                           const char *name);
void             catalog_evict_from_cache(const char *name);
bool             catalog_rename_table_meta(bytecask_db_t *db,
                                           const char *from,
                                           const char *to);
std::optional<uint32_t> catalog_lookup_table_id(const char *name);
const TableMeta *catalog_lookup_meta(uint32_t table_id);

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

  int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *create_info) override;
  int open(const char *name, int mode, uint test_if_locked) override;
  int close() override;
  int delete_table(const char *name) override;
  int rename_table(const char *from, const char *to) override;

  // -------------------------------------------------------------------
  // Write path
  // -------------------------------------------------------------------

  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;

  // -------------------------------------------------------------------
  // Full table scan
  // -------------------------------------------------------------------

  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_end() override;

  // -------------------------------------------------------------------
  // Primary key index access
  // -------------------------------------------------------------------

  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;

  // -------------------------------------------------------------------
  // External lock — transaction lifecycle
  // -------------------------------------------------------------------

  int external_lock(THD *thd, int lock_type) override;

  // -------------------------------------------------------------------
  // Position / random access by row reference
  // -------------------------------------------------------------------

  void position(const uchar *record) override;
  int rnd_pos(uchar *buf, uchar *pos) override;

  // -------------------------------------------------------------------
  // Metadata / capabilities
  // -------------------------------------------------------------------

  ulonglong table_flags() const override {
    return HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
           HA_TABLE_SCAN_ON_INDEX |
           HA_REC_NOT_IN_SEQ |
           HA_BINLOG_ROW_CAPABLE;
  }

  ulong index_flags(uint idx, uint part,
                    bool all_parts) const override;

  int info(uint flag) override;

  uint max_supported_key_length() const override { return MAX_KEY_LENGTH; }
  uint max_supported_keys() const override { return MAX_KEY; }

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

private:
  std::vector<uint8_t> encode_current_pk(const uchar *buf) const;
  int index_read_current(uchar *buf);

  uint32_t table_id_{0};
  uint16_t schema_version_{1};

  std::unique_ptr<MariaDBTxn::MergeIterator> merge_scan_;
  std::unique_ptr<MariaDBTxn::MergeIterator> merge_index_;

  static void write_table_id_prefix(uint8_t *buf4, uint32_t table_id);
};

} // namespace bytecaskdb
