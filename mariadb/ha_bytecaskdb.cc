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

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bytecaskdb {

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

  // Paginated delete of all row keys for this table.
  auto lower = table_id_prefix(tid.value());
  auto upper = table_id_upper_bound(tid.value());

  static constexpr int kBatchSize = 4096;

  for (;;) {
    auto *iter = bytecask_iter_open(g_db, lower.data(), lower.size());
    if (!iter) { break; }

    auto *plan = bytecask_write_plan_new();
    if (!plan) {
      bytecask_iter_free(iter);
      break;
    }

    int count = 0;
    while (bytecask_iter_valid(iter) && count < kBatchSize) {
      uint8_t *key = nullptr;
      std::size_t key_len = 0;
      if (bytecask_iter_key(iter, &key, &key_len) != 0) { break; }

      // Past upper bound — done.
      if (key_len >= upper.size() &&
          std::memcmp(key, upper.data(), upper.size()) >= 0) {
        bytecask_free_buf(key);
        break;
      }

      bytecask_write_plan_del(plan, key, key_len);
      bytecask_free_buf(key);
      ++count;
      bytecask_iter_next(iter);
    }

    bytecask_iter_free(iter);

    if (count == 0) {
      bytecask_write_plan_free(plan);
      break;
    }

    int rc = bytecask_apply_batch_if(g_db, plan, /*sync=*/1);
    if (rc < 0) {
      return HA_ERR_GENERIC;
    }

    if (count < kBatchSize) {
      break;  // Last batch.
    }
  }

  // Delete catalog entry.
  catalog_delete_table_meta(g_db, name);
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

  auto key = encode_current_pk(buf);
  auto val = encode_row(table, buf, schema_version_);

  auto *plan = bytecask_write_plan_new();
  if (!plan) { return HA_ERR_GENERIC; }

  // Guard: reject if this PK already exists.
  bytecask_write_plan_ensure_absent(plan, key.data(), key.size());
  bytecask_write_plan_put(plan, key.data(), key.size(),
                          val.data(), val.size());

  int rc = bytecask_apply_batch_if(g_db, plan, /*sync=*/1);
  if (rc == 0) {
    return HA_ERR_FOUND_DUPP_KEY;
  }
  if (rc < 0) {
    return HA_ERR_GENERIC;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// update_row()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::update_row(const uchar *old_data, const uchar *new_data) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto old_pk = encode_current_pk(old_data);
  auto new_pk = encode_current_pk(new_data);
  auto new_val = encode_row(table, new_data, schema_version_);

  auto *plan = bytecask_write_plan_new();
  if (!plan) { return HA_ERR_GENERIC; }

  if (old_pk == new_pk) {
    // PK unchanged — simple overwrite.
    bytecask_write_plan_put(plan, new_pk.data(), new_pk.size(),
                            new_val.data(), new_val.size());
  } else {
    // PK changed — delete old, insert new with duplicate check.
    bytecask_write_plan_del(plan, old_pk.data(), old_pk.size());
    bytecask_write_plan_ensure_absent(plan, new_pk.data(), new_pk.size());
    bytecask_write_plan_put(plan, new_pk.data(), new_pk.size(),
                            new_val.data(), new_val.size());
  }

  int rc = bytecask_apply_batch_if(g_db, plan, /*sync=*/1);
  if (rc == 0) {
    return HA_ERR_FOUND_DUPP_KEY;
  }
  if (rc < 0) {
    return HA_ERR_GENERIC;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// delete_row()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::delete_row(const uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto key = encode_current_pk(buf);

  auto *plan = bytecask_write_plan_new();
  if (!plan) { return HA_ERR_GENERIC; }

  bytecask_write_plan_del(plan, key.data(), key.size());

  int rc = bytecask_apply_batch_if(g_db, plan, /*sync=*/1);
  if (rc < 0) {
    return HA_ERR_GENERIC;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// rnd_init() / rnd_next() / rnd_end() — full table scan
// ---------------------------------------------------------------------------

int ha_bytecaskdb::rnd_init(bool /*scan*/) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto prefix = table_id_prefix(table_id_);
  scan_iter_ = bytecask_iter_open(g_db, prefix.data(), prefix.size());
  if (!scan_iter_) {
    return HA_ERR_GENERIC;
  }
  return 0;
}

int ha_bytecaskdb::rnd_next(uchar *buf) {
  if (!scan_iter_) { return HA_ERR_END_OF_FILE; }

  if (!bytecask_iter_valid(scan_iter_)) {
    return HA_ERR_END_OF_FILE;
  }

  uint8_t *key_buf = nullptr;
  std::size_t key_len = 0;
  if (bytecask_iter_key(scan_iter_, &key_buf, &key_len) != 0) {
    return HA_ERR_GENERIC;
  }

  if (!key_belongs_to_table(key_buf, key_len, table_id_)) {
    bytecask_free_buf(key_buf);
    return HA_ERR_END_OF_FILE;
  }

  // Decode PK into the record buffer.
  decode_pk(table, key_buf, key_len, buf);
  bytecask_free_buf(key_buf);

  // Read and decode value.
  uint8_t *val_buf = nullptr;
  std::size_t val_len = 0;
  if (bytecask_iter_value(scan_iter_, &val_buf, &val_len) != 0) {
    return HA_ERR_GENERIC;
  }

  decode_row(table, val_buf, val_len, buf);
  bytecask_free_buf(val_buf);

  bytecask_iter_next(scan_iter_);
  return 0;
}

int ha_bytecaskdb::rnd_end() {
  if (scan_iter_) {
    bytecask_iter_free(scan_iter_);
    scan_iter_ = nullptr;
  }
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
  if (index_iter_) {
    bytecask_iter_free(index_iter_);
    index_iter_ = nullptr;
  }
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
    // Point lookup via bytecask_get.
    uint8_t *val_buf = nullptr;
    std::size_t val_len = 0;
    int found = bytecask_get(g_db, search_key.data(), search_key.size(),
                             &val_buf, &val_len);
    if (found < 0) { return HA_ERR_GENERIC; }
    if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

    // Restore key columns into record buffer.
    key_restore(buf, key, &table->key_info[pk_idx], pk_len);

    decode_row(table, val_buf, val_len, buf);
    bytecask_free_buf(val_buf);
    return 0;
  }

  // Range scan: open iterator at search key.
  if (index_iter_) {
    bytecask_iter_free(index_iter_);
  }
  index_iter_ = bytecask_iter_open(g_db, search_key.data(),
                                    search_key.size());
  if (!index_iter_) { return HA_ERR_GENERIC; }

  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_next()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_next(uchar *buf) {
  if (!index_iter_) { return HA_ERR_END_OF_FILE; }
  bytecask_iter_next(index_iter_);
  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_first()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_first(uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto prefix = table_id_prefix(table_id_);
  if (index_iter_) {
    bytecask_iter_free(index_iter_);
  }
  index_iter_ = bytecask_iter_open(g_db, prefix.data(), prefix.size());
  if (!index_iter_) { return HA_ERR_GENERIC; }

  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_read_current() — shared helper for index methods
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_read_current(uchar *buf) {
  if (!index_iter_ || !bytecask_iter_valid(index_iter_)) {
    return HA_ERR_END_OF_FILE;
  }

  uint8_t *key_buf = nullptr;
  std::size_t key_len = 0;
  if (bytecask_iter_key(index_iter_, &key_buf, &key_len) != 0) {
    return HA_ERR_GENERIC;
  }

  if (!key_belongs_to_table(key_buf, key_len, table_id_)) {
    bytecask_free_buf(key_buf);
    return HA_ERR_END_OF_FILE;
  }

  decode_pk(table, key_buf, key_len, buf);
  bytecask_free_buf(key_buf);

  uint8_t *val_buf = nullptr;
  std::size_t val_len = 0;
  if (bytecask_iter_value(index_iter_, &val_buf, &val_len) != 0) {
    return HA_ERR_GENERIC;
  }

  decode_row(table, val_buf, val_len, buf);
  bytecask_free_buf(val_buf);
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

  uint8_t *val_buf = nullptr;
  std::size_t val_len = 0;

  int found = bytecask_get(g_db, pos, ref_length, &val_buf, &val_len);
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
