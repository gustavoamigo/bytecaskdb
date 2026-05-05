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

#include "bytecask.hpp"
#include "bytecaskdb_txn.h"
#include "catalog.h"

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// Global state — shared across all tables on this server instance.
// ---------------------------------------------------------------------------

extern bytecask::DB  *g_db;
extern handlerton    *bytecaskdb_hton;

// ---------------------------------------------------------------------------
// Persistent catalog functions — defined in bytecaskdb_plugin.cc.
// ---------------------------------------------------------------------------

uint32_t         catalog_alloc_table_id(bytecask::DB *db);
bool             catalog_put_table_meta(bytecask::DB *db,
                                        const TableMeta &meta,
                                        const char *name);
bool             catalog_delete_table_meta(bytecask::DB *db,
                                           const char *name);
void             catalog_evict_from_cache(const char *name);
bool             catalog_rename_table_meta(bytecask::DB *db,
                                           const char *from,
                                           const char *to);
std::optional<uint32_t> catalog_lookup_table_id(const char *name);
const TableMeta *catalog_lookup_meta(uint32_t table_id);

uint64_t         catalog_alloc_rowid(uint32_t table_id);
uint64_t         catalog_alloc_rowid_range(uint32_t table_id, uint64_t count);
uint64_t         catalog_peek_rowid(uint32_t table_id);
void             catalog_seed_rowid(uint32_t table_id, uint64_t high_water);
void             catalog_reset_rowid(uint32_t table_id, uint64_t value);
void             catalog_drop_rowid(uint32_t table_id);

uint64_t         catalog_alloc_autoinc_range(uint32_t table_id, uint64_t count);
uint64_t         catalog_peek_autoinc(uint32_t table_id);
std::atomic<uint64_t> *catalog_autoinc_ptr(uint32_t table_id);
void             catalog_seed_autoinc(uint32_t table_id, uint64_t high_water);
void             catalog_reset_autoinc(uint32_t table_id, uint64_t value);
void             catalog_drop_autoinc(uint32_t table_id);

int64_t          catalog_row_count(uint32_t table_id);
std::atomic<int64_t> *catalog_row_count_ptr(uint32_t table_id);
void             catalog_row_count_add(uint32_t table_id, int64_t delta);
void             catalog_row_count_reset(uint32_t table_id);
void             catalog_drop_row_count(uint32_t table_id);

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
  int delete_all_rows() override;

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
  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;

  // -------------------------------------------------------------------
  // External lock — transaction lifecycle
  // -------------------------------------------------------------------

  int external_lock(THD *thd, int lock_type) override;

  // -------------------------------------------------------------------
  // AUTO_INCREMENT
  // -------------------------------------------------------------------

  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  int reset_auto_increment(ulonglong value) override;
  void update_create_info(HA_CREATE_INFO *create_info) override;

  // -------------------------------------------------------------------
  // Position / random access by row reference
  // -------------------------------------------------------------------

  void position(const uchar *record) override;
  int rnd_pos(uchar *buf, uchar *pos) override;

  // -------------------------------------------------------------------
  // Metadata / capabilities
  // -------------------------------------------------------------------

  ulonglong table_flags() const override {
    return HA_REC_NOT_IN_SEQ |
           HA_BINLOG_ROW_CAPABLE |
           HA_NULL_IN_KEY |
           HA_CAN_INDEX_BLOBS |
           HA_AUTO_PART_KEY |
           HA_CAN_VIRTUAL_COLUMNS |
           HA_PRIMARY_KEY_IN_READ_INDEX;
  }

  const char *index_type(uint) override { return "BYTECASK"; }

  ulong index_flags(uint idx, uint part,
                    bool all_parts) const override;

  int info(uint flag) override;

  ha_rows records_in_range(uint index, const key_range *min_key,
                           const key_range *max_key, page_range *pages) override;

  uint max_supported_key_length() const override { return MAX_KEY_LENGTH; }
  uint max_supported_key_part_length() const override { return 3072; }
  uint max_supported_keys() const override { return MAX_KEY; }

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

  int extra(enum ha_extra_function operation) override;

  int check(THD *thd, HA_CHECK_OPT *check_opt) override;

  // -------------------------------------------------------------------
  // Inplace ALTER support (DROP FOREIGN KEY)
  // -------------------------------------------------------------------

  enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *altered_table,
      Alter_inplace_info *ha_alter_info) override;
  bool inplace_alter_table(TABLE *altered_table,
                           Alter_inplace_info *ha_alter_info) override;
  bool commit_inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info,
                                  bool commit) override;

  // -------------------------------------------------------------------
  // FK metadata exposure
  // -------------------------------------------------------------------

  int get_foreign_key_list(THD *thd,
                           List<FOREIGN_KEY_INFO> *f_key_list) override;

#ifdef PLUGIN_TESTING
  void set_current_row_key(const std::vector<uint8_t> &key) {
    current_row_key_ = key;
  }
#endif

private:
  // Saves `current_row_key_` from a slice the iterator gave us.
  void save_current_row_key(const uint8_t *data, std::size_t len);
  int index_read_current(uchar *buf);

  // Lazily seeds the per-table synthetic rowid counter on first open of
  // a PK-less table by scanning for the largest existing key.
  void seed_rowid_counter_if_needed() const;
  // Seeds autoinc counter on first open of a table with AUTO_INCREMENT.
  void seed_autoinc_counter_if_needed() const;

  uint32_t table_id_{0};
  uint16_t schema_version_{1};

  std::unique_ptr<MariaDBTxn::MergeIterator> merge_scan_;
  std::unique_ptr<MariaDBTxn::MergeIterator> merge_index_;

  // Saved search key prefix (binary, after fix_varchar_key_encoding) from
  // index_read_map — used by index_next_same for prefix comparison.
  std::vector<uint8_t> sec_search_key_;

  // Full on-disk key of the row most recently materialized by rnd_next /
  // index_read_map / rnd_pos. Reused by update_row / delete_row / position
  // so that PK-less synthetic rowids (which are not present in record[0])
  // round-trip correctly.
  std::vector<uint8_t> current_row_key_;

  // Reused across index_read_map calls to avoid per-call heap allocation.
  std::vector<uint8_t> search_key_buf_;

  // Scratch buffer for decode_pk() to avoid per-row heap allocation.
  std::vector<uint8_t> decode_pk_scratch_;

  // Reused across secondary-index row lookups to avoid per-row allocation.
  std::vector<uint8_t> sec_row_key_buf_;

  // Per-handler encode buffers reused across write_row / update_row calls.
  std::vector<uint8_t> encode_pk_buf_;
  std::vector<uint8_t> encode_new_pk_buf_;          // update_row only
  std::vector<uint8_t> encode_row_buf_;
  std::vector<uint8_t> encode_sec_key_buf_;         // write_row
  std::vector<uint8_t> encode_unique_sec_key_buf_;  // write_row + update_row
  std::vector<uint8_t> encode_old_sec_key_buf_;     // update_row
  std::vector<uint8_t> encode_new_sec_key_buf_;     // update_row

  // Saved across write_row → get_dup_key → info(HA_STATUS_ERRKEY) round-trip.
  uint saved_errkey_{0};

  // Snapshot of FK list before inplace ALTER, for rollback.
  std::vector<FKMeta> pre_alter_fks_;

  // Row value buffer kept alive for BLOB pointer lifetime.
  // decode_row sets BLOB field pointers into this buffer.
  bytecask::Bytes row_value_buf_;

  // Cached atomic pointers for lock-free info() access.
  std::atomic<int64_t> *row_count_atomic_ = nullptr;
  std::atomic<uint64_t> *autoinc_atomic_ = nullptr;

  // Cached txn pointer, set at external_lock and valid until unlock.
  MariaDBTxn *txn_cached_ = nullptr;

  // Set by HA_EXTRA_KEYREAD / cleared by HA_EXTRA_NO_KEYREAD. When true and
  // the active index is a covering one whose key parts are decodable, the
  // index path skips the secondary→PK row fetch.
  bool keyread_only_ = false;

  static void write_table_id_prefix(uint8_t *buf4, uint32_t table_id);
};

} // namespace bytecaskdb
