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
#include "bytecask.hpp"
#include "bytecask_view.h"
#include "catalog.h"
#include "key_encoding.h"
#include "row_encoding.h"
#include "bytecaskdb_txn.h"

#include <cassert>
#include <cstring>
#include <exception>
#include <utility>
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
  // Both primary key and secondary indexes support ordered navigation and range scans
  return HA_READ_NEXT | HA_READ_ORDER | HA_READ_RANGE;
}

// ---------------------------------------------------------------------------
// store_lock — no-op: engine uses OCC, no THR_LOCK needed.
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
                           HA_CREATE_INFO *create_info) {
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
  meta.schema_version = 2;  // v2 includes secondary index support
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

  // Extract secondary indexes (skip the primary key).
  uint32_t key_count = table_arg->s->keys;
  for (uint32_t i = 0; i < key_count; ++i) {
    if (i == table_arg->s->primary_key) {
      continue;  // Skip primary key
    }
    const KEY &key_info = table_arg->key_info[i];

    IndexMeta idx_meta;
    idx_meta.index_id  = static_cast<uint16_t>(i);
    idx_meta.key_parts = static_cast<uint16_t>(key_info.user_defined_key_parts);
    idx_meta.is_unique = (key_info.flags & HA_NOSAME) ? 1 : 0;

    // Extract column indexes for each key part.
    idx_meta.column_indexes.resize(idx_meta.key_parts);
    for (uint16_t j = 0; j < idx_meta.key_parts; ++j) {
      idx_meta.column_indexes[j] = static_cast<uint16_t>(key_info.key_part[j].fieldnr - 1);
    }

    meta.indexes.push_back(std::move(idx_meta));
  }

  if (!catalog_put_table_meta(g_db, meta, name)) {
    return HA_ERR_GENERIC;
  }

  if (create_info->auto_increment_value > 0) {
    catalog_seed_autoinc(tid, create_info->auto_increment_value - 1);
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

  // ref_length: 5-byte prefix (1 ns + 4 tid) + PK key length (or 8-byte
  // synthetic rowid for PK-less tables).
  const uint pk = table->s->primary_key;
  if (pk == MAX_KEY) {
    ref_length = 5 + 8;
    seed_rowid_counter_if_needed();
  } else {
    ref_length = 5 + table->key_info[pk].key_length;
    if (table->found_next_number_field) {
      seed_autoinc_counter_if_needed();
    }
  }
  return 0;
}

// Seeds the per-table synthetic rowid counter from disk on first open,
// by reverse-scanning the row namespace for this table_id. Cheap: a
// single radix-tree traversal returns the largest existing key in O(1).
void ha_bytecaskdb::seed_rowid_counter_if_needed() const {
  if (catalog_peek_rowid(table_id_) != 0) { return; }

  auto upper = table_id_upper_bound(table_id_);
  bytecask::Snapshot snap = g_db->snapshot();
  uint64_t hw = 0;
  for (auto &k : snap.rkeys_from({}, as_view(upper))) {
    if (!key_belongs_to_table(u8_data(k), k.size(), table_id_)) { break; }
    if (k.size() >= 13) {
      hw = read_be64(u8_data(k) + 5);
    }
    break;
  }
  catalog_seed_rowid(table_id_, hw);
}

// Seeds the autoinc counter on first open of a PK table with AUTO_INCREMENT.
// Reverse-scans the row namespace to find the last key, restores it into a
// scratch buffer, then reads the autoinc field value.
void ha_bytecaskdb::seed_autoinc_counter_if_needed() const {
  if (catalog_peek_autoinc(table_id_) != 0) { return; }

  const uint pk_idx = table->s->primary_key;
  const uint pk_len = table->key_info[pk_idx].key_length;
  auto upper = table_id_upper_bound(table_id_);
  bytecask::Snapshot snap = g_db->snapshot();
  uint64_t hw = 0;

  for (auto &k : snap.rkeys_from({}, as_view(upper))) {
    if (!key_belongs_to_table(u8_data(k), k.size(), table_id_)) { break; }
    if (k.size() >= 5 + pk_len) {
      std::vector<uchar> scratch(table->s->rec_buff_length, 0);
      key_restore(scratch.data(),
                  const_cast<uchar *>(u8_data(k)) + 5,
                  &table->key_info[pk_idx],
                  pk_len);
      Field *ai_field = table->found_next_number_field;
      if (ai_field) {
        uchar *saved_ptr = ai_field->ptr;
        ai_field->ptr = scratch.data() + ai_field->offset(table->record[0]);
        hw = static_cast<uint64_t>(ai_field->val_int());
        ai_field->ptr = saved_ptr;
      }
    }
    break;
  }
  catalog_seed_autoinc(table_id_, hw);
}

// ---------------------------------------------------------------------------
// AUTO_INCREMENT
// ---------------------------------------------------------------------------

void ha_bytecaskdb::get_auto_increment(ulonglong /*offset*/,
                                        ulonglong /*increment*/,
                                        ulonglong nb_desired_values,
                                        ulonglong *first_value,
                                        ulonglong *nb_reserved_values) {
  if (nb_desired_values == 0) { nb_desired_values = 1; }
  *first_value = catalog_alloc_autoinc_range(table_id_,
                                             static_cast<uint64_t>(nb_desired_values));
  *nb_reserved_values = nb_desired_values;
}

int ha_bytecaskdb::reset_auto_increment(ulonglong value) {
  catalog_reset_autoinc(table_id_, value > 0 ? value - 1 : 0);
  return 0;
}

void ha_bytecaskdb::update_create_info(HA_CREATE_INFO *create_info) {
  if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
    create_info->auto_increment_value = catalog_peek_autoinc(table_id_) + 1;
  }
}
// ---------------------------------------------------------------------------

int ha_bytecaskdb::close() {
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

  // Get table metadata to find secondary indexes to clean up.
  const TableMeta *meta = catalog_lookup_meta(tid.value());

  // Atomic del_range of all row keys + secondary index keys + catalog entry.
  auto lower = table_id_prefix(tid.value());
  auto upper = table_id_upper_bound(tid.value());
  auto cat_key = table_meta_key(name);

  bytecask::WritePlan plan;

  // Delete all primary key data [0x02 | table_id].
  plan.del_range(as_view(lower), as_view(upper));

  // Delete all secondary index ranges [0x03 | table_id | index_id].
  // Schema v2 always has indexes (may be empty)
  if (meta) {
    for (const auto &index : meta->indexes) {
      auto idx_lower = index_id_prefix(tid.value(), index.index_id);
      auto idx_upper = index_id_upper_bound(tid.value(), index.index_id);
      plan.del_range(as_view(idx_lower), as_view(idx_upper));
    }
  }

  // Delete catalog entry.
  plan.del(as_view(cat_key));

  try {
    (void)g_db->apply_batch({.sync = true}, std::move(plan));
  } catch (const std::exception &) {
    return HA_ERR_GENERIC;
  }

  catalog_evict_from_cache(name);
  return 0;
}

// ---------------------------------------------------------------------------
// delete_all_rows() — TRUNCATE TABLE / DELETE without WHERE
// ---------------------------------------------------------------------------

int ha_bytecaskdb::delete_all_rows() {
  if (!g_db) { return HA_ERR_GENERIC; }

  const TableMeta *meta = catalog_lookup_meta(table_id_);

  auto lower = table_id_prefix(table_id_);
  auto upper = table_id_upper_bound(table_id_);

  bytecask::WritePlan plan;
  plan.del_range(as_view(lower), as_view(upper));

  if (meta) {
    for (const auto &index : meta->indexes) {
      auto idx_lower = index_id_prefix(table_id_, index.index_id);
      auto idx_upper = index_id_upper_bound(table_id_, index.index_id);
      plan.del_range(as_view(idx_lower), as_view(idx_upper));
    }
  }

  try {
    (void)g_db->apply_batch({.sync = true}, std::move(plan));
  } catch (const std::exception &) {
    return HA_ERR_GENERIC;
  }

  if (table->s->primary_key == MAX_KEY) {
    catalog_reset_rowid(table_id_, 0);
  }
  if (table->found_next_number_field) {
    catalog_reset_autoinc(table_id_, 0);
  }
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

  auto *txn = get_or_create_txn(ha_thd(), bytecaskdb_hton);
  txn->begin_if_needed(ha_thd(), bytecaskdb_hton);

  // MariaDB 10.11 does not call update_auto_increment() from ha_write_row;
  // each engine is responsible for calling it inside write_row.
  if (table->next_number_field && buf == table->record[0]) {
    int err = update_auto_increment();
    if (err) { return err; }
  }

  // For PK-less tables, allocate a fresh synthetic rowid for this row.
  const bool no_pk = (table->s->primary_key == MAX_KEY);
  const uint64_t rowid = no_pk ? catalog_alloc_rowid(table_id_) : 0;

  auto key = encode_pk(table, buf, table_id_, rowid);
  auto val = encode_row(table, buf, schema_version_);

  // PK-less rows can't collide on the primary key (synthetic rowids are
  // monotonic), so skip the dup check there. For PK tables, eager dup check.
  if (!no_pk && txn->exists(key.data(), key.size())) {
    errkey = table->s->primary_key;
    return HA_ERR_FOUND_DUPP_KEY;
  }

  // Check unique secondary index constraints.
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (meta) {
    for (const auto &index : meta->indexes) {
      if (index.is_unique) {
        // For unique constraints, check if any key with this secondary key prefix exists
        auto unique_prefix = encode_unique_sec_key(table, buf, table_id_, index.index_id, index.index_id);

        // Open iterator at the unique prefix to see if any matching keys exist
        auto upper = index_id_upper_bound(table_id_, index.index_id);
        auto iter = txn->iter_index_prefix(unique_prefix.data(), unique_prefix.size(),
                                           upper.data(), upper.size(),
                                           table_id_, index.index_id);
        if (iter && iter->valid()) {
          errkey = lookup_errkey = static_cast<uint>(index.index_id);
          return HA_ERR_FOUND_DUPP_KEY;
        }
      }
    }
  }

  // Buffer primary key operation.
  txn->buffer_put(key.data(), key.size(), val.data(), val.size());

  // Buffer secondary index operations.
  if (meta) {
    for (const auto &index : meta->indexes) {
      auto sec_key = encode_sec_key(table, buf, table_id_,
                                     index.index_id, index.index_id, rowid);
      txn->buffer_put(sec_key.data(), sec_key.size(), nullptr, 0);
    }
  }

  // Remember the full key for any immediately-following update_row /
  // delete_row / position call.
  current_row_key_ = std::move(key);

  // Seed the autoinc counter from the actual field value so explicit INSERTs
  // (e.g. INSERT VALUES (5, ...)) bump the counter for subsequent auto values.
  if (table->next_number_field && buf == table->record[0]) {
    uint64_t val = static_cast<uint64_t>(table->next_number_field->val_int());
    if (val > 0) {
      catalog_seed_autoinc(table_id_, val);
    }
  }

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

  const bool no_pk = (table->s->primary_key == MAX_KEY);

  // Old PK is always the key we just read; current_row_key_ has the real
  // bytes (including the synthetic rowid for PK-less tables).
  std::vector<uint8_t> old_pk = current_row_key_;
  if (old_pk.empty()) {
    // Defensive fallback for the PK case if no read happened first.
    old_pk = encode_pk(table, old_data, table_id_);
  }

  // For PK-less tables the rowid persists across the update, so the new
  // primary key is the same. For PK tables the new PK comes from new_data.
  std::vector<uint8_t> new_pk;
  uint64_t rowid = 0;
  if (no_pk) {
    new_pk = old_pk;
    if (old_pk.size() >= 13) { rowid = read_be64(old_pk.data() + 5); }
  } else {
    new_pk = encode_pk(table, new_data, table_id_);
  }

  auto new_val = encode_row(table, new_data, schema_version_);

  // Handle secondary indexes.
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (meta) {
    // PK suffix bytes start at offset 5; for PK-less tables this is the rowid.
    const uint8_t *pk_bytes     = old_pk.data() + 5;
    const size_t   pk_bytes_len = old_pk.size() - 5;

    for (const auto &index : meta->indexes) {
      // Locate the old secondary index entry by scanning for the PK suffix.
      // This avoids relying on field->ptr pointing into the old record buffer,
      // which is unreliable for VARCHAR fields under move_field_offset.
      const uint sec_key_field_len = table->key_info[index.index_id].key_length;
      auto lo   = index_id_prefix(table_id_, index.index_id);
      auto hi   = index_id_upper_bound(table_id_, index.index_id);
      auto scan = txn->iter_index_prefix(lo.data(), lo.size(),
                                         hi.data(), hi.size(),
                                         table_id_, index.index_id);

      std::vector<uint8_t> old_sec_key;
      while (scan && scan->valid()) {
        const uint8_t *k    = scan->key_data();
        const size_t   klen = scan->key_len();
        if (klen >= 7 + sec_key_field_len + pk_bytes_len &&
            std::memcmp(k + 7 + sec_key_field_len, pk_bytes, pk_bytes_len) == 0) {
          old_sec_key.assign(k, k + klen);
          break;
        }
        scan->next();
      }

      auto new_sec_key = encode_sec_key(table, new_data, table_id_,
                                         index.index_id, index.index_id, rowid);
      if (old_sec_key != new_sec_key) {
        if (!old_sec_key.empty()) {
          txn->buffer_del(old_sec_key.data(), old_sec_key.size());
        }
        txn->buffer_put(new_sec_key.data(), new_sec_key.size(), nullptr, 0);
      }
    }
  }

  if (old_pk == new_pk) {
    // PK unchanged — simple overwrite. (Always the case for PK-less tables.)
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

  // The post-update key is what subsequent calls (rare, but possible)
  // should reference.
  current_row_key_ = std::move(new_pk);

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

  const bool no_pk = (table->s->primary_key == MAX_KEY);

  // Use the key from the row we just read so PK-less synthetic rowids
  // round-trip correctly. Fall back to re-encoding for the rare PK case
  // where delete is called without a prior read.
  std::vector<uint8_t> key = current_row_key_;
  if (key.empty()) {
    key = encode_pk(table, buf, table_id_);
  }

  uint64_t rowid = 0;
  if (no_pk && key.size() >= 13) {
    rowid = read_be64(key.data() + 5);
  }

  // Buffer secondary index deletions first.
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (meta) {
    for (const auto &index : meta->indexes) {
      auto sec_key = encode_sec_key(table, buf, table_id_,
                                     index.index_id, index.index_id, rowid);
      txn->buffer_del(sec_key.data(), sec_key.size());
    }
  }

  // Buffer primary key deletion.
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

  // Save the on-disk key for any follow-up update_row / delete_row /
  // position call.
  save_current_row_key(merge_scan_->key_data(), merge_scan_->key_len());

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

  if (active_index == table->s->primary_key) {
    // Primary key operations: direct row access [0x02 | table_id | pk]
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
      bytecask::Bytes val;
      int found = txn->get(search_key.data(), search_key.size(), val);
      if (found < 0) { return HA_ERR_GENERIC; }
      if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

      // Restore key columns into record buffer.
      key_restore(buf, key, &table->key_info[pk_idx], pk_len);

      decode_row(table, u8_data(val), val.size(), buf);
      save_current_row_key(search_key.data(), search_key.size());
      return 0;
    }

    // Range scan: open merge iterator at search key.
    auto hi = table_id_upper_bound(table_id_);
    merge_index_ = txn->iter_prefix(search_key.data(), search_key.size(),
                                     hi.data(), hi.size(), table_id_);
    if (!merge_index_) { return HA_ERR_GENERIC; }

    return index_read_current(buf);

  } else {
    // Secondary index operations: access via index namespace [0x03 | table_id | index_id]
    const KEY &key_info = table->key_info[active_index];
    uint sec_key_len = key_info.key_length;

    std::vector<uint8_t> search_key(7 + sec_key_len);
    search_key[0] = kNsIndex;
    search_key[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
    search_key[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
    search_key[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
    search_key[4] = static_cast<uint8_t>( table_id_        & 0xFF);
    search_key[5] = static_cast<uint8_t>((active_index >> 8) & 0xFF);
    search_key[6] = static_cast<uint8_t>( active_index       & 0xFF);
    // keypart_map tells us which key parts are valid in `key`. Only copy those
    // bytes; zero-fill the rest so the scan starts at the correct position.
    uint actual_packed_len = 0;
    uint prefix_len = 0;
    for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
      if (!(keypart_map & (key_part_map(1) << i))) break;
      actual_packed_len += key_info.key_part[i].store_length;
      prefix_len        += key_info.key_part[i].length;
    }
    std::memcpy(search_key.data() + 7, key, actual_packed_len);
    if (actual_packed_len < sec_key_len) {
      std::memset(search_key.data() + 7 + actual_packed_len, 0,
                  sec_key_len - actual_packed_len);
    }
    // The optimizer key buffer is in key_copy() format (includes VARCHAR length
    // prefix). encode_sec_key() strips that prefix via fix_varchar_key_encoding;
    // apply the same transformation here so the search key matches stored keys.
    fix_varchar_key_encoding(search_key.data() + 7, table, active_index);

    // Save only the covered-prefix bytes for index_next_same comparison.
    sec_search_key_.assign(search_key.begin(), search_key.begin() + 7 + prefix_len);

    // For secondary indexes, always use range scan (even for exact lookups)
    // because we need to iterate through potentially multiple matches
    auto hi = index_id_upper_bound(table_id_, static_cast<uint16_t>(active_index));
    merge_index_ = txn->iter_index_prefix(search_key.data(), search_key.size(),
                                          hi.data(), hi.size(),
                                          table_id_, static_cast<uint16_t>(active_index));
    if (!merge_index_) { return HA_ERR_GENERIC; }

    return index_read_current(buf);
  }
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
// index_next_same() — advance within an equality range on a secondary index.
//
// The default handler uses key_cmp_if_same() which compares the field in
// record[0] against the original packed key.  That comparison breaks for
// VARCHAR because fix_varchar_key_encoding() strips the length prefix from
// stored keys, making the stored format diverge from the packed key format
// that key_cmp_if_same() expects.
//
// Instead we compare the binary secondary-key prefix directly: the first
// sec_search_key_.size() bytes of the stored key must match the saved search
// key (both already have the VARCHAR length prefix removed).
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_next_same(uchar *buf, const uchar * /*key*/,
                                    uint /*keylen*/) {
  if (!merge_index_ || !merge_index_->valid()) { return HA_ERR_END_OF_FILE; }
  if (active_index == table->s->primary_key) {
    // Primary index has unique keys — there is never a "next same".
    return HA_ERR_END_OF_FILE;
  }

  merge_index_->next();
  if (!merge_index_->valid()) { return HA_ERR_END_OF_FILE; }

  // Verify the stored key still shares the same search-key prefix.
  if (!sec_search_key_.empty() &&
      (merge_index_->key_len() < sec_search_key_.size() ||
       std::memcmp(merge_index_->key_data(),
                   sec_search_key_.data(),
                   sec_search_key_.size()) != 0)) {
    return HA_ERR_END_OF_FILE;
  }

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

  if (active_index == table->s->primary_key) {
    // Primary key iteration: scan table row data [0x02 | table_id]
    auto lo = table_id_prefix(table_id_);
    auto hi = table_id_upper_bound(table_id_);
    merge_index_ = txn->iter_prefix(lo.data(), lo.size(),
                                    hi.data(), hi.size(), table_id_);
  } else {
    // Secondary index iteration: scan index data [0x03 | table_id | index_id]
    auto lo = index_id_prefix(table_id_, static_cast<uint16_t>(active_index));
    auto hi = index_id_upper_bound(table_id_, static_cast<uint16_t>(active_index));
    merge_index_ = txn->iter_index_prefix(lo.data(), lo.size(),
                                          hi.data(), hi.size(),
                                          table_id_, static_cast<uint16_t>(active_index));
  }
  if (!merge_index_) { return HA_ERR_GENERIC; }

  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_prev()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_prev(uchar *buf) {
  if (!merge_index_ || !merge_index_->valid()) { return HA_ERR_END_OF_FILE; }
  merge_index_->next(); // In reverse iterator, next() goes backwards
  return index_read_current(buf);
}

// ---------------------------------------------------------------------------
// index_last()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_last(uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  if (active_index == table->s->primary_key) {
    // Primary key reverse iteration: start from end of table range
    auto hi = table_id_upper_bound(table_id_);
    auto lo = table_id_prefix(table_id_);
    merge_index_ = txn->riter_prefix(hi.data(), hi.size(),
                                     lo.data(), lo.size(), table_id_);
  } else {
    // Secondary index reverse iteration: start from end of index range
    auto hi = index_id_upper_bound(table_id_, static_cast<uint16_t>(active_index));
    auto lo = index_id_prefix(table_id_, static_cast<uint16_t>(active_index));
    merge_index_ = txn->riter_index_prefix(hi.data(), hi.size(),
                                           lo.data(), lo.size(),
                                           table_id_, static_cast<uint16_t>(active_index));
  }
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

  if (active_index == table->s->primary_key) {
    // Primary key access: key is encoded PK, value is row data
    decode_pk(table, merge_index_->key_data(), merge_index_->key_len(), buf);
    decode_row(table, merge_index_->value_data(), merge_index_->value_len(), buf);
    save_current_row_key(merge_index_->key_data(), merge_index_->key_len());
    return 0;
  } else {
    // Secondary index access: two-step lookup
    // 1. Extract PK from secondary index key
    const KEY &key_info = table->key_info[active_index];
    uint sec_key_len = key_info.key_length;
    auto pk_bytes = extract_pk_from_sec_key(merge_index_->key_data(),
                                             merge_index_->key_len(),
                                             sec_key_len);
    if (pk_bytes.empty()) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    // 2. Build primary key for row lookup [0x02 | table_id | pk-or-rowid]
    std::vector<uint8_t> row_key(5 + pk_bytes.size());
    row_key[0] = kNsRow;
    // Write table_id in big-endian
    row_key[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
    row_key[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
    row_key[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
    row_key[4] = static_cast<uint8_t>( table_id_        & 0xFF);
    std::memcpy(row_key.data() + 5, pk_bytes.data(), pk_bytes.size());

    // 3. Fetch full row from primary key namespace
    auto *txn = static_cast<MariaDBTxn *>(
        thd_get_ha_data(ha_thd(), bytecaskdb_hton));
    if (!txn) { return HA_ERR_GENERIC; }

    bytecask::Bytes row_val;
    int rc = txn->get(row_key.data(), row_key.size(), row_val);
    if (rc != 1) {
      return HA_ERR_KEY_NOT_FOUND;  // Row was deleted
    }

    // 4. Decode PK and row data into MariaDB buffer
    decode_pk(table, row_key.data(), row_key.size(), buf);
    decode_row(table, u8_data(row_val), row_val.size(), buf);
    save_current_row_key(row_key.data(), row_key.size());
    return 0;
  }
}

// ---------------------------------------------------------------------------
// position() / rnd_pos()
// ---------------------------------------------------------------------------

void ha_bytecaskdb::position(const uchar *record) {
  // Prefer the on-disk key from the most recent read — required for PK-less
  // tables (synthetic rowid is not in record[0]) and equivalent for PK tables.
  if (!current_row_key_.empty() && current_row_key_.size() <= ref_length) {
    std::memcpy(ref, current_row_key_.data(), current_row_key_.size());
    return;
  }
  // Fallback: re-encode from record (only valid for PK tables).
  auto pk = encode_pk(table, record, table_id_);
  assert(pk.size() <= ref_length);
  std::memcpy(ref, pk.data(), pk.size());
}

int ha_bytecaskdb::rnd_pos(uchar *buf, uchar *pos) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = static_cast<MariaDBTxn *>(
      thd_get_ha_data(ha_thd(), bytecaskdb_hton));
  if (!txn) { return HA_ERR_GENERIC; }

  bytecask::Bytes val;

  int found = txn->get(pos, ref_length, val);
  if (found < 0) { return HA_ERR_GENERIC; }
  if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

  // Decode PK from ref into record buffer.
  decode_pk(table, pos, ref_length, buf);

  decode_row(table, u8_data(val), val.size(), buf);

  // Save the position-key as the current row key in case mutation follows.
  save_current_row_key(pos, ref_length);

  return 0;
}

// ---------------------------------------------------------------------------
// info() — optimizer statistics
// ---------------------------------------------------------------------------

int ha_bytecaskdb::info(uint flag) {
  if (flag & HA_STATUS_VARIABLE) {
    auto lo = table_id_prefix(table_id_);
    auto hi = table_id_upper_bound(table_id_);
    ha_rows count = 0;
    for (auto &k : g_db->keys_from({}, as_view(lo))) {
      if (k.size() < hi.size() ||
          std::memcmp(u8_data(k), hi.data(), hi.size()) >= 0) {
        break;
      }
      ++count;
    }
    stats.records = count;
  }
  if (flag & HA_STATUS_AUTO) {
    stats.auto_increment_value = catalog_peek_autoinc(table_id_) + 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// records_in_range() — estimate rows in key range for optimizer
// ---------------------------------------------------------------------------

ha_rows ha_bytecaskdb::records_in_range(uint /*index*/, const key_range */*min_key*/,
                                         const key_range */*max_key*/,
                                         page_range */*pages*/) {
  // Stub: return unknown for now. MariaDB will use default estimates.
  return HA_POS_ERROR;
}

// ---------------------------------------------------------------------------
// save_current_row_key() — private helper
// ---------------------------------------------------------------------------

void ha_bytecaskdb::save_current_row_key(const uint8_t *data, std::size_t len) {
  current_row_key_.assign(data, data + len);
}

} // namespace bytecaskdb
