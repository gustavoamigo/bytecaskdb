// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// ha_bytecaskdb.cc — MariaDB handler implementation for ByteCaskDB.
//
// Implements: CREATE/DROP/RENAME TABLE, INSERT, UPDATE, DELETE,
// full table scan, PK index access (point + range).

#include "ha_bytecaskdb.h"
#include "table.h"
#include "field.h"
#include "key.h"
#include "bytecask_c.h"
#include "catalog.h"
#include "key_encoding.h"
#include "row_encoding.h"
#include "bytecaskdb_txn.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bytecaskdb {

// Helper: get-or-create the per-THD MariaDBTxn from ha_data.
static MariaDBTxn *get_or_create_txn(THD *thd, handlerton *hton) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) {
    txn = new MariaDBTxn(g_db);
    thd_set_ha_data(thd, hton, txn);
  }
  return txn;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ha_bytecaskdb::ha_bytecaskdb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {}

// ---------------------------------------------------------------------------
// index_flags()
// ---------------------------------------------------------------------------

ulong ha_bytecaskdb::index_flags(uint idx, uint /*part*/,
                                  bool /*all_parts*/) const {
  if (table_share && idx == table_share->primary_key) {
    return HA_READ_NEXT | HA_READ_ORDER | HA_READ_RANGE;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Lock management — no-op (no transactions yet)
// ---------------------------------------------------------------------------

THR_LOCK_DATA **ha_bytecaskdb::store_lock(THD * /*thd*/, THR_LOCK_DATA **to,
                                          enum thr_lock_type /*lock_type*/) {
  return to;
}

// ---------------------------------------------------------------------------
// external_lock() — transaction lifecycle
// ---------------------------------------------------------------------------

int ha_bytecaskdb::external_lock(THD *thd, int lock_type) {
  if (lock_type != F_UNLCK) {
    auto *txn = get_or_create_txn(thd, bytecaskdb_hton);
    txn->begin_if_needed(thd, bytecaskdb_hton);
  }
  // F_UNLCK: no-op — commit/rollback via handlerton callbacks.
  return 0;
}

// ---------------------------------------------------------------------------
// create() — called for CREATE TABLE
// ---------------------------------------------------------------------------

int ha_bytecaskdb::create(const char *name, TABLE *table_arg,
                           HA_CREATE_INFO * /*create_info*/) {
  if (!g_db) { return HA_ERR_GENERIC; }

  // Check if table already exists in catalog.
  if (catalog_lookup_table_id(name).has_value()) {
    return HA_ERR_TABLE_EXIST;
  }

  // Allocate a new table_id.
  uint32_t tid = catalog_alloc_table_id(g_db);
  if (tid == 0) {
    return HA_ERR_GENERIC;
  }

  // Build TableMeta from the TABLE structure.
  TableMeta meta;
  meta.table_id       = tid;
  meta.schema_version = 1;
  meta.full_name      = normalize_table_name(name);
  meta.reclength      = static_cast<uint16_t>(table_arg->s->reclength);
  meta.null_bytes     = static_cast<uint16_t>(table_arg->s->null_bytes);
  meta.pk_parts       = (table_arg->s->primary_key != MAX_KEY)
                            ? table_arg->key_info[table_arg->s->primary_key].user_defined_key_parts
                            : 0;

  // Populate column metadata.
  uint32_t col_count = table_arg->s->fields;
  meta.columns.resize(col_count);
  for (uint32_t i = 0; i < col_count; ++i) {
    Field *f = table_arg->field[i];
    meta.columns[i].field_type   = static_cast<uint16_t>(f->type());
    meta.columns[i].field_length = static_cast<uint16_t>(f->field_length);
    meta.columns[i].is_nullable  = f->real_maybe_null() ? 1 : 0;
    meta.columns[i].charset_id   = f->charset() ? static_cast<uint16_t>(f->charset()->number) : 0;
  }

  if (!catalog_put_table_meta(g_db, meta, name)) {
    return HA_ERR_GENERIC;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// open() — called when a statement first opens the table
// ---------------------------------------------------------------------------

int ha_bytecaskdb::open(const char *name, int /*mode*/,
                         uint /*test_if_locked*/) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto tid = catalog_lookup_table_id(name);
  if (!tid.has_value()) {
    return HA_ERR_NO_SUCH_TABLE;
  }
  table_id_ = tid.value();

  const auto *meta = catalog_lookup_meta(table_id_);
  if (meta) {
    schema_version_ = static_cast<uint16_t>(meta->schema_version);
  }

  scan_iter_  = nullptr;
  index_iter_ = nullptr;

  // ref_length: 5-byte prefix (1 ns + 4 tid) + PK key length.
  ref_length = 5 + table->key_info[table->s->primary_key].key_length;
  return 0;
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::close() {
  if (scan_iter_) {
    bytecask_iter_free(scan_iter_);
    scan_iter_ = nullptr;
  }
  if (index_iter_) {
    bytecask_iter_free(index_iter_);
    index_iter_ = nullptr;
  }
  merge_scan_.reset();
  merge_index_.reset();
  return 0;
}

// ---------------------------------------------------------------------------
// delete_table() — called for DROP TABLE
// ---------------------------------------------------------------------------

int ha_bytecaskdb::delete_table(const char *name) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto tid = catalog_lookup_table_id(name);
  if (!tid.has_value()) {
    return 0;  // Nothing to delete.
  }

  // Atomic del_range of all row keys + catalog entry in a single WritePlan.
  auto lower = table_id_prefix(tid.value());
  auto upper = table_id_upper_bound(tid.value());
  auto cat_key = table_meta_key(name);

  auto *plan = bytecask_write_plan_new();
  if (!plan) { return HA_ERR_GENERIC; }

  bytecask_write_plan_del_range(plan, lower.data(), lower.size(),
                                upper.data(), upper.size());
  bytecask_write_plan_del(plan, cat_key.data(), cat_key.size());

  int rc = bytecask_apply_batch_if(g_db, plan, /*sync=*/1);
  if (rc < 0) { return HA_ERR_GENERIC; }

  catalog_evict_from_cache(name);
  return 0;
}

// ---------------------------------------------------------------------------
// rename_table()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::rename_table(const char *from, const char *to) {
  if (!g_db) { return HA_ERR_GENERIC; }
  if (!catalog_rename_table_meta(g_db, from, to)) {
    return HA_ERR_GENERIC;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// write_row() — INSERT with duplicate PK detection
// ---------------------------------------------------------------------------

int ha_bytecaskdb::write_row(const uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  auto key = encode_current_pk(buf);
  auto val = encode_row(table, buf, schema_version_);

  // Eager dup check: lookup_ then snapshot.
  if (txn->exists(key.data(), key.size())) {
    return HA_ERR_FOUND_DUPP_KEY;
  }

  txn->buffer_put(key.data(), key.size(), val.data(), val.size());
  return 0;
}

// ---------------------------------------------------------------------------
// update_row()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::update_row(const uchar *old_data, const uchar *new_data) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  auto old_pk = encode_current_pk(old_data);
  auto new_pk = encode_current_pk(new_data);
  auto new_val = encode_row(table, new_data, schema_version_);

  if (old_pk == new_pk) {
    // PK unchanged — simple overwrite.
    txn->buffer_put(new_pk.data(), new_pk.size(),
                    new_val.data(), new_val.size());
  } else {
    // PK changed — dup check new pk, delete old, insert new.
    if (txn->exists(new_pk.data(), new_pk.size())) {
      return HA_ERR_FOUND_DUPP_KEY;
    }
    txn->buffer_del(old_pk.data(), old_pk.size());
    txn->buffer_put(new_pk.data(), new_pk.size(),
                    new_val.data(), new_val.size());
  }

  return 0;
}

// ---------------------------------------------------------------------------
// delete_row()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::delete_row(const uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  auto key = encode_current_pk(buf);
  txn->buffer_del(key.data(), key.size());
  return 0;
}

// ---------------------------------------------------------------------------
// rnd_init() / rnd_next() / rnd_end() — full table scan
// ---------------------------------------------------------------------------

int ha_bytecaskdb::rnd_init(bool /*scan*/) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  auto lo = table_id_prefix(table_id_);
  auto hi = table_id_upper_bound(table_id_);
  merge_scan_ = txn->iter_prefix(lo.data(), lo.size(),
                                  hi.data(), hi.size(), table_id_);
  if (!merge_scan_) {
    return HA_ERR_GENERIC;
  }
  return 0;
}

int ha_bytecaskdb::rnd_next(uchar *buf) {
  if (!merge_scan_ || !merge_scan_->valid()) {
    return HA_ERR_END_OF_FILE;
  }

  // Decode PK from key into record buffer.
  decode_pk(table, merge_scan_->key_data(), merge_scan_->key_len(), buf);

  // Decode row value.
  decode_row(table, merge_scan_->value_data(), merge_scan_->value_len(), buf);

  merge_scan_->next();
  return 0;
}

int ha_bytecaskdb::rnd_end() {
  merge_scan_.reset();
  return 0;
}

// ---------------------------------------------------------------------------
// index_init() / index_end()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_init(uint idx, bool /*sorted*/) {
  active_index = idx;
  return 0;
}

int ha_bytecaskdb::index_end() {
  merge_index_.reset();
  active_index = MAX_KEY;
  return 0;
}

// ---------------------------------------------------------------------------
// index_read_map() — PK point lookup + range scan start
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_read_map(uchar *buf, const uchar *key,
                                   key_part_map keypart_map,
                                   enum ha_rkey_function find_flag) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  // Build the search key from MariaDB's packed key format.
  uint pk_idx = table->s->primary_key;
  uint pk_len = table->key_info[pk_idx].key_length;

  std::vector<uint8_t> search_key(5 + pk_len);
  search_key[0] = kNsRow;
  search_key[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
  search_key[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
  search_key[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
  search_key[4] = static_cast<uint8_t>( table_id_        & 0xFF);
  std::memcpy(search_key.data() + 5, key, pk_len);

  if (find_flag == HA_READ_KEY_EXACT) {
    // Point lookup via txn->get().
    uint8_t *val_buf = nullptr;
    std::size_t val_len = 0;
    int found = txn->get(search_key.data(), search_key.size(),
                         &val_buf, &val_len);
    if (found < 0) { return HA_ERR_GENERIC; }
    if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

    // Restore key columns into record buffer.
    key_restore(buf, key, &table->key_info[pk_idx], pk_len);

    decode_row(table, val_buf, val_len, buf);
    bytecask_free_buf(val_buf);
    return 0;
  }

  // Range scan: open merge iterator at search key.
  auto hi = table_id_upper_bound(table_id_);
  merge_index_ = txn->iter_prefix(search_key.data(), search_key.size(),
                                   hi.data(), hi.size(), table_id_);
  if (!merge_index_) { return HA_ERR_GENERIC; }

  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_next()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_next(uchar *buf) {
  if (!merge_index_ || !merge_index_->valid()) { return HA_ERR_END_OF_FILE; }
  merge_index_->next();
  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_first()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_first(uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  auto lo = table_id_prefix(table_id_);
  auto hi = table_id_upper_bound(table_id_);
  merge_index_ = txn->iter_prefix(lo.data(), lo.size(),
                                   hi.data(), hi.size(), table_id_);
  if (!merge_index_) { return HA_ERR_GENERIC; }

  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_read_current() — shared helper for index methods
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_read_current(uchar *buf) {
  if (!merge_index_ || !merge_index_->valid()) {
    return HA_ERR_END_OF_FILE;
  }

  decode_pk(table, merge_index_->key_data(), merge_index_->key_len(), buf);
  decode_row(table, merge_index_->value_data(), merge_index_->value_len(), buf);
  return 0;
}

// ---------------------------------------------------------------------------
// position() / rnd_pos()
// ---------------------------------------------------------------------------

void ha_bytecaskdb::position(const uchar *record) {
  auto pk = encode_current_pk(record);
  assert(pk.size() <= ref_length);
  std::memcpy(ref, pk.data(), pk.size());
}

int ha_bytecaskdb::rnd_pos(uchar *buf, uchar *pos) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  uint8_t *val_buf = nullptr;
  std::size_t val_len = 0;

  int found = txn->get(pos, ref_length, &val_buf, &val_len);
  if (found < 0) { return HA_ERR_GENERIC; }
  if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

  // Decode PK from ref into record buffer.
  decode_pk(table, pos, ref_length, buf);

  decode_row(table, val_buf, val_len, buf);
  bytecask_free_buf(val_buf);
  return 0;
}

// ---------------------------------------------------------------------------
// info() — optimizer statistics
// ---------------------------------------------------------------------------

int ha_bytecaskdb::info(uint /*flag*/) {
  stats.records = HA_POS_ERROR;
  return 0;
}

// ---------------------------------------------------------------------------
// encode_current_pk() — private helper
// ---------------------------------------------------------------------------

std::vector<uint8_t> ha_bytecaskdb::encode_current_pk(const uchar *buf) const {
  return bytecaskdb::encode_pk(table, buf, table_id_);
}

} // namespace bytecaskdb
