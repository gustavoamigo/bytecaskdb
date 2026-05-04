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

#undef WITH_WSREP
#include <mysql/server/private/sql_class.h>
#include <mysql/server/private/sql_alter.h>
#include <mysql/service_thd_alloc.h>
#include <private/service_versions.h>

#include <cassert>
#include <cstring>
#include <exception>
#include <utility>
#include <vector>

extern "C" {
struct thd_alloc_service_st *thd_alloc_service =
    reinterpret_cast<struct thd_alloc_service_st *>(VERSION_thd_alloc);
struct my_print_error_service_st *my_print_error_service =
    reinterpret_cast<struct my_print_error_service_st *>(VERSION_my_print_error);
}

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
  return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
         HA_KEYREAD_ONLY;
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
    txn_cached_ = txn;
  } else {
    txn_cached_ = nullptr;
  }
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
  meta.schema_version = 2;  // may be upgraded to 3 if FKs present
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

  // Extract FK constraints from create_info->alter_info->key_list (covers
  // both CREATE TABLE ... FOREIGN KEY and copy-ALTER ADD FOREIGN KEY).
  // Must use create_info->alter_info (not thd->lex->alter_info) because
  // mysql_prepare_alter_table() merges existing FKs into a local copy.
  Alter_info *alter_info = create_info->alter_info;
  if (alter_info && alter_info->key_list.elements) {
    List_iterator<Key> key_it(alter_info->key_list);
    Key *k;
    while ((k = key_it++)) {
      if (k->type != Key::FOREIGN_KEY) continue;
      auto *fk = static_cast<Foreign_key *>(k);
      FKMeta fk_meta;
      fk_meta.name.assign(fk->constraint_name.str, fk->constraint_name.length);
      fk_meta.ref_db.assign(fk->ref_db.str, fk->ref_db.length);
      fk_meta.ref_table.assign(fk->ref_table.str, fk->ref_table.length);
      List_iterator<Key_part_spec> col_it(fk->columns);
      Key_part_spec *col;
      while ((col = col_it++)) {
        fk_meta.fk_cols.emplace_back(col->field_name.str, col->field_name.length);
      }
      List_iterator<Key_part_spec> ref_it(fk->ref_columns);
      Key_part_spec *ref_col;
      while ((ref_col = ref_it++)) {
        fk_meta.ref_cols.emplace_back(ref_col->field_name.str, ref_col->field_name.length);
      }
      fk_meta.update_opt = static_cast<uint8_t>(fk->update_opt);
      fk_meta.delete_opt = static_cast<uint8_t>(fk->delete_opt);
      meta.fks.push_back(std::move(fk_meta));
    }
  }
  meta.schema_version = meta.fks.empty() ? 2 : 3;

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

  row_count_atomic_ = catalog_row_count_ptr(table_id_);
  autoinc_atomic_ = catalog_autoinc_ptr(table_id_);

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
      std::vector<uint8_t> key_tmp(u8_data(k) + 5, u8_data(k) + 5 + pk_len);
      undo_mem_comparable(key_tmp.data(), &table->key_info[pk_idx], pk_len);
      key_restore(scratch.data(), key_tmp.data(),
                  &table->key_info[pk_idx], pk_len);
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
  // For composite PKs (next_number_keypart > 0), MariaDB calls per-row but
  // passes the total row estimate. Always allocate exactly what's requested.
  if (nb_desired_values == 0) { nb_desired_values = 1; }
  // For per-row calls (composite PK), allocate only 1 to avoid gaps.
  ulonglong alloc = (table->s->next_number_keypart > 0) ? 1 : nb_desired_values;
  *first_value = catalog_alloc_autoinc_range(table_id_, static_cast<uint64_t>(alloc));
  *nb_reserved_values = alloc;
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
  catalog_row_count_reset(table_id_);
  return 0;
}
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

  auto *txn = txn_cached_;
  if (!txn) { return HA_ERR_GENERIC; }

  // MariaDB 10.11 does not call update_auto_increment() from ha_write_row;
  // each engine is responsible for calling it inside write_row.
  if (table->next_number_field && buf == table->record[0]) {
    int err = update_auto_increment();
    if (err) { return err; }
  }

  // For PK-less tables, allocate a fresh synthetic rowid for this row.
  const bool no_pk = (table->s->primary_key == MAX_KEY);
  const uint64_t rowid = no_pk ? catalog_alloc_rowid(table_id_) : 0;

  auto &key = encode_pk_buf_;
  encode_pk_into(key, table, buf, table_id_, rowid);
  auto &val = encode_row_buf_;
  encode_row_into(val, table, buf, schema_version_);

  // PK-less rows can't collide on the primary key (synthetic rowids are
  // monotonic), so skip the dup check there. For PK tables, eager dup check.
  if (!no_pk && txn->exists(key.data(), key.size())) {
    errkey = saved_errkey_ = table->s->primary_key;
    return HA_ERR_FOUND_DUPP_KEY;
  }

  // Check unique secondary index constraints.
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (meta) {
    for (const auto &index : meta->indexes) {
      if (index.is_unique) {
        // SQL standard: NULL != NULL — skip dup check if any key part is NULL.
        const KEY &ki = table->key_info[index.index_id];
        bool has_null = false;
        for (uint p = 0; p < ki.user_defined_key_parts; ++p) {
          Field *f = table->field[ki.key_part[p].fieldnr - 1];
          if (f->is_null_in_record(buf)) { has_null = true; break; }
        }
        if (has_null) continue;

        auto &unique_prefix = encode_unique_sec_key_buf_;
        encode_unique_sec_key_into(unique_prefix, table, buf, table_id_,
                                    index.index_id, index.index_id);

        auto upper = index_id_upper_bound(table_id_, index.index_id);
        auto iter = txn->iter_index_prefix(unique_prefix.data(), unique_prefix.size(),
                                           upper.data(), upper.size(),
                                           table_id_, index.index_id);
        if (iter && iter->valid() &&
            iter->key_len() >= unique_prefix.size() &&
            std::memcmp(iter->key_data(), unique_prefix.data(), unique_prefix.size()) == 0) {
          errkey = saved_errkey_ = static_cast<uint>(index.index_id);
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
      auto &sec_key = encode_sec_key_buf_;
      encode_sec_key_into(sec_key, table, buf, table_id_,
                           index.index_id, index.index_id, rowid);
      txn->buffer_put(sec_key.data(), sec_key.size(), nullptr, 0);
    }
  }

  // Remember the full key for any immediately-following update_row /
  // delete_row / position call. Copy (not move) — keep our buffer's capacity.
  current_row_key_ = key;

  // Seed the autoinc counter from the actual field value so explicit INSERTs
  // (e.g. INSERT VALUES (5, ...)) bump the counter for subsequent auto values.
  if (table->next_number_field && buf == table->record[0]) {
    uint64_t val = static_cast<uint64_t>(table->next_number_field->val_int());
    if (val > 0) {
      catalog_seed_autoinc(table_id_, val);
    }
  }

  catalog_row_count_add(table_id_, 1);
  return 0;
}
// ---------------------------------------------------------------------------

int ha_bytecaskdb::update_row(const uchar *old_data, const uchar *new_data) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = txn_cached_;
  if (!txn) { return HA_ERR_GENERIC; }

  const bool no_pk = (table->s->primary_key == MAX_KEY);

  // Old PK is always the key we just read; current_row_key_ has the real
  // bytes (including the synthetic rowid for PK-less tables).
  std::vector<uint8_t> old_pk = current_row_key_;
  if (old_pk.empty()) {
    // Defensive fallback for the PK case if no read happened first.
    encode_pk_into(old_pk, table, old_data, table_id_);
  }

  // For PK-less tables the rowid persists across the update, so the new
  // primary key is the same. For PK tables the new PK comes from new_data.
  auto &new_pk = encode_new_pk_buf_;
  uint64_t rowid = 0;
  if (no_pk) {
    new_pk = old_pk;  // copy
    if (old_pk.size() >= 13) { rowid = read_be64(old_pk.data() + 5); }
  } else {
    encode_pk_into(new_pk, table, new_data, table_id_);
  }

  auto &new_val = encode_row_buf_;
  encode_row_into(new_val, table, new_data, schema_version_);

  // Handle secondary indexes.
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (meta) {
    const uint8_t *pk_bytes     = old_pk.data() + 5;
    const size_t   pk_bytes_len = old_pk.size() - 5;

    // When the PK changes, every secondary index entry must be rewritten
    // because the PK suffix in the secondary key differs even when the
    // user-visible key parts didn't. PK-less tables never change PK.
    const bool pk_changing = !no_pk && (old_pk != new_pk);

    auto index_in_write_set = [&](const KEY &ki) {
      for (uint p = 0; p < ki.user_defined_key_parts; ++p) {
        if (bitmap_is_set(table->write_set,
                          ki.key_part[p].field->field_index)) {
          return true;
        }
      }
      return false;
    };

    for (const auto &index : meta->indexes) {
      const KEY &ki = table->key_info[index.index_id];
      if (!pk_changing && !index_in_write_set(ki)) continue;

      auto &old_sec_key = encode_old_sec_key_buf_;
      auto &new_sec_key = encode_new_sec_key_buf_;
      encode_sec_key_into(old_sec_key, table, old_data, table_id_,
                           index.index_id, index.index_id, rowid);
      encode_sec_key_into(new_sec_key, table, new_data, table_id_,
                           index.index_id, index.index_id, rowid);
      if (old_sec_key != new_sec_key) {
        if (index.is_unique) {
          const KEY &ki = table->key_info[index.index_id];
          bool has_null = false;
          for (uint p = 0; p < ki.user_defined_key_parts; ++p) {
            Field *f = table->field[ki.key_part[p].fieldnr - 1];
            if (f->is_null_in_record(new_data)) { has_null = true; break; }
          }
          if (!has_null) {
            const uint sec_key_field_len = ki.key_length;
            auto &unique_prefix = encode_unique_sec_key_buf_;
            encode_unique_sec_key_into(unique_prefix, table, new_data, table_id_,
                                        index.index_id, index.index_id);
            auto uhi = index_id_upper_bound(table_id_, index.index_id);
            auto uiter = txn->iter_index_prefix(unique_prefix.data(), unique_prefix.size(),
                                                uhi.data(), uhi.size(),
                                                table_id_, index.index_id);
            if (uiter && uiter->valid() &&
                uiter->key_len() >= unique_prefix.size() &&
                std::memcmp(uiter->key_data(), unique_prefix.data(), unique_prefix.size()) == 0) {
              const uint8_t *uk = uiter->key_data();
              size_t uklen = uiter->key_len();
              bool is_self = (uklen >= 7 + sec_key_field_len + pk_bytes_len &&
                              std::memcmp(uk + 7 + sec_key_field_len,
                                          pk_bytes, pk_bytes_len) == 0);
              if (!is_self) {
                errkey = saved_errkey_ = static_cast<uint>(index.index_id);
                return HA_ERR_FOUND_DUPP_KEY;
              }
            }
          }
        }

        txn->buffer_del(old_sec_key.data(), old_sec_key.size());
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
  // should reference. Copy to keep the encode buffer's capacity.
  current_row_key_ = new_pk;

  return 0;
}

// ---------------------------------------------------------------------------
// delete_row()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::delete_row(const uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = txn_cached_;
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
  catalog_row_count_add(table_id_, -1);
  return 0;
}
// ---------------------------------------------------------------------------

int ha_bytecaskdb::rnd_init(bool /*scan*/) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = txn_cached_;
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
  decode_pk(table, merge_scan_->key_data(), merge_scan_->key_len(), buf,
            decode_pk_scratch_);

  // Move value to persistent buffer before advancing (BLOB pointers need it).
  row_value_buf_ = merge_scan_->steal_value();
  decode_row(table,
             reinterpret_cast<const uint8_t *>(row_value_buf_.data()),
             row_value_buf_.size(), buf);

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
// check() — CHECK TABLE implementation
// ---------------------------------------------------------------------------

int ha_bytecaskdb::check(THD * /*thd*/, HA_CHECK_OPT * /*check_opt*/) {
  if (!g_db) return HA_ADMIN_FAILED;

  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (!meta || meta->indexes.empty())
    return HA_ADMIN_OK;

  auto *txn = txn_cached_;
  if (!txn) return HA_ADMIN_FAILED;

  int rc = rnd_init(true);
  if (rc) return HA_ADMIN_FAILED;

  uchar *buf = table->record[0];
  bool corrupt = false;

  while ((rc = rnd_next(buf)) == 0) {
    for (const auto &index : meta->indexes) {
      // Build the full secondary key (including PK suffix) for this row.
      // For PK-less tables, extract the synthetic rowid from current_row_key_.
      uint64_t rowid = 0;
      if (table->s->primary_key == MAX_KEY && current_row_key_.size() >= 13) {
        rowid = read_be64(current_row_key_.data() + 5);
      }
      auto sec_key = encode_sec_key(table, buf, table_id_,
                                     index.index_id, index.index_id, rowid);
      if (!txn->exists(sec_key.data(), sec_key.size())) {
        corrupt = true;
        break;
      }
    }
    if (corrupt) break;
  }

  rnd_end();
  return corrupt ? HA_ADMIN_CORRUPT : HA_ADMIN_OK;
}

// ---------------------------------------------------------------------------
// index_init() / index_end()
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_init(uint idx, bool /*sorted*/) {
  active_index = idx;
  keyread_only_ = false;
  return 0;
}

int ha_bytecaskdb::index_end() {
  merge_index_.reset();
  active_index = MAX_KEY;
  keyread_only_ = false;
  return 0;
}

int ha_bytecaskdb::extra(enum ha_extra_function operation) {
  switch (operation) {
    case HA_EXTRA_KEYREAD:
      keyread_only_ = true;
      break;
    case HA_EXTRA_NO_KEYREAD:
      keyread_only_ = false;
      break;
    default:
      break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// index_read_map() — PK point lookup + range scan start
// ---------------------------------------------------------------------------

int ha_bytecaskdb::index_read_map(uchar *buf, const uchar *key,
                                   key_part_map keypart_map,
                                   enum ha_rkey_function find_flag) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto *txn = txn_cached_;
  if (!txn) { return HA_ERR_GENERIC; }

  if (active_index == table->s->primary_key) {
    // Primary key operations: direct row access [0x02 | table_id | pk]
    uint pk_idx = table->s->primary_key;
    const KEY &pk_info = table->key_info[pk_idx];
    uint pk_len = pk_info.key_length;

    // Compute how many bytes of the search key are actually provided.
    uint actual_prefix_len = 0;
    for (uint i = 0; i < pk_info.user_defined_key_parts; ++i) {
      if (!(keypart_map & (key_part_map(1) << i))) break;
      actual_prefix_len += pk_info.key_part[i].store_length;
    }

    search_key_buf_.resize(5 + pk_len);
    search_key_buf_[0] = kNsRow;
    search_key_buf_[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
    search_key_buf_[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
    search_key_buf_[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
    search_key_buf_[4] = static_cast<uint8_t>( table_id_        & 0xFF);
    std::memcpy(search_key_buf_.data() + 5, key, actual_prefix_len);
    if (actual_prefix_len < pk_len) {
      std::memset(search_key_buf_.data() + 5 + actual_prefix_len, 0,
                  pk_len - actual_prefix_len);
    }
    normalize_padspace_pk(search_key_buf_.data() + 5, &pk_info);
    make_mem_comparable(search_key_buf_.data() + 5, &pk_info, pk_len);

    if (find_flag == HA_READ_KEY_EXACT) {
      int found = txn->get(search_key_buf_.data(), search_key_buf_.size(),
                           row_value_buf_);
      if (found < 0) { return HA_ERR_GENERIC; }
      if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

      key_restore(buf, key, const_cast<KEY *>(&pk_info), pk_len);

      decode_row(table,
                 reinterpret_cast<const uint8_t *>(row_value_buf_.data()),
                 row_value_buf_.size(), buf);
      save_current_row_key(search_key_buf_.data(), search_key_buf_.size());
      return 0;
    }

    if (find_flag == HA_READ_PREFIX_LAST ||
        find_flag == HA_READ_PREFIX_LAST_OR_PREV) {
      std::vector<uint8_t> hi_key(5 + pk_len);
      std::memcpy(hi_key.data(), search_key_buf_.data(), 5 + actual_prefix_len);
      std::memset(hi_key.data() + 5 + actual_prefix_len, 0xFF,
                  pk_len - actual_prefix_len);
      make_mem_comparable(hi_key.data() + 5, &pk_info, pk_len);

      auto lo = table_id_prefix(table_id_);
      merge_index_ = txn->riter_prefix(hi_key.data(), hi_key.size(),
                                        lo.data(), lo.size(), table_id_);
      if (!merge_index_) { return HA_ERR_GENERIC; }

      int rc = index_read_current(buf);
      if (rc != 0) return rc;

      if (current_row_key_.size() < 5 + actual_prefix_len) {
        return HA_ERR_KEY_NOT_FOUND;
      }
      if (std::memcmp(current_row_key_.data() + 5,
                      search_key_buf_.data() + 5, actual_prefix_len) != 0) {
        return HA_ERR_KEY_NOT_FOUND;
      }
      return 0;
    }

    // Forward range scan: open merge iterator at search key.
    auto hi = table_id_upper_bound(table_id_);
    merge_index_ = txn->iter_prefix(search_key_buf_.data(), search_key_buf_.size(),
                                     hi.data(), hi.size(), table_id_);
    if (!merge_index_) { return HA_ERR_GENERIC; }

    return index_read_current(buf);

  } else {
    // Secondary index operations: access via index namespace [0x03 | table_id | index_id]
    const KEY &key_info = table->key_info[active_index];
    uint sec_key_len = key_info.key_length;

    search_key_buf_.resize(7 + sec_key_len);
    search_key_buf_[0] = kNsIndex;
    search_key_buf_[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
    search_key_buf_[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
    search_key_buf_[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
    search_key_buf_[4] = static_cast<uint8_t>( table_id_        & 0xFF);
    search_key_buf_[5] = static_cast<uint8_t>((active_index >> 8) & 0xFF);
    search_key_buf_[6] = static_cast<uint8_t>( active_index       & 0xFF);
    // keypart_map tells us which key parts are valid in `key`. Only copy those
    // bytes; zero-fill the rest so the scan starts at the correct position.
    uint actual_packed_len = 0;
    for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
      if (!(keypart_map & (key_part_map(1) << i))) break;
      actual_packed_len += key_info.key_part[i].store_length;
    }
    std::memcpy(search_key_buf_.data() + 7, key, actual_packed_len);
    if (actual_packed_len < sec_key_len) {
      std::memset(search_key_buf_.data() + 7 + actual_packed_len, 0,
                  sec_key_len - actual_packed_len);
    }
    // The optimizer key buffer is in key_copy() format (includes VARCHAR length
    // prefix). encode_sec_key() strips that prefix via fix_varchar_key_encoding;
    // apply the same transformation here so the search key matches stored keys.
    fix_varchar_key_encoding(search_key_buf_.data() + 7, table, active_index);
    make_mem_comparable(search_key_buf_.data() + 7, &key_info, sec_key_len);

    // Save the covered-prefix bytes for index_next_same comparison.
    // Use actual_packed_len (includes null indicators) not just data length.
    sec_search_key_.assign(search_key_buf_.begin(),
                           search_key_buf_.begin() + 7 + actual_packed_len);

    auto hi = index_id_upper_bound(table_id_, static_cast<uint16_t>(active_index));

    if (find_flag == HA_READ_AFTER_KEY) {
      // Position strictly after all entries with this secondary key value.
      // Append 0xFF bytes to create an upper bound past all PK suffixes.
      uint suffix_len = pk_suffix_length(table);
      sec_row_key_buf_.resize(search_key_buf_.size() + suffix_len);
      std::memcpy(sec_row_key_buf_.data(), search_key_buf_.data(),
                  search_key_buf_.size());
      std::memset(sec_row_key_buf_.data() + search_key_buf_.size(), 0xFF,
                  suffix_len);
      merge_index_ = txn->iter_index_prefix(sec_row_key_buf_.data(),
                                            sec_row_key_buf_.size(),
                                            hi.data(), hi.size(),
                                            table_id_, static_cast<uint16_t>(active_index));
    } else {
      // For HA_READ_KEY_EXACT and other modes, start at the search key position.
      merge_index_ = txn->iter_index_prefix(search_key_buf_.data(),
                                            search_key_buf_.size(),
                                            hi.data(), hi.size(),
                                            table_id_, static_cast<uint16_t>(active_index));
    }
    if (!merge_index_) { return HA_ERR_GENERIC; }

    if (find_flag == HA_READ_KEY_EXACT) {
      if (!merge_index_->valid()) { return HA_ERR_KEY_NOT_FOUND; }
      // Verify the found key actually has the search prefix (sans PK suffix).
      size_t prefix_len = 7 + actual_packed_len;
      if (merge_index_->key_len() < prefix_len ||
          std::memcmp(merge_index_->key_data(), search_key_buf_.data(), prefix_len) != 0) {
        return HA_ERR_KEY_NOT_FOUND;
      }
    }

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

  // Safety: verify the key still belongs to our index namespace.
  if (!key_belongs_to_index(merge_index_->key_data(), merge_index_->key_len(),
                            table_id_, static_cast<uint16_t>(active_index))) {
    return HA_ERR_END_OF_FILE;
  }

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

  auto *txn = txn_cached_;
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

  auto *txn = txn_cached_;
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
    decode_pk(table, merge_index_->key_data(), merge_index_->key_len(), buf,
              decode_pk_scratch_);
    row_value_buf_ = merge_index_->steal_value();
    decode_row(table,
               reinterpret_cast<const uint8_t *>(row_value_buf_.data()),
               row_value_buf_.size(), buf);
    save_current_row_key(merge_index_->key_data(), merge_index_->key_len());
    return 0;
  } else {
    // Secondary index access: two-step lookup
    // 1. Extract PK from secondary index key
    const KEY &key_info = table->key_info[active_index];
    uint sec_key_len = key_info.key_length;
    std::size_t pk_len = 0;
    const uint8_t *pk_ptr = extract_pk_from_sec_key(
        merge_index_->key_data(), merge_index_->key_len(),
        sec_key_len, &pk_len);
    if (!pk_ptr) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    // KEYREAD short-circuit: when the optimizer only needs the indexed
    // columns, decode them directly from the secondary key instead of
    // fetching the full row. Falls back to the slow path if any key part
    // is of a type the decoder does not handle (VARCHAR, BLOB, nullable).
    if (keyread_only_ &&
        decode_sec_key_into_record(table, &key_info,
                                    merge_index_->key_data(),
                                    merge_index_->key_len(),
                                    buf)) {
      // Save the synthesized PK so position()/rnd_pos() still work if the
      // optimizer later switches keyread off mid-scan.
      sec_row_key_buf_.resize(5 + pk_len);
      sec_row_key_buf_[0] = kNsRow;
      sec_row_key_buf_[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
      sec_row_key_buf_[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
      sec_row_key_buf_[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
      sec_row_key_buf_[4] = static_cast<uint8_t>( table_id_        & 0xFF);
      std::memcpy(sec_row_key_buf_.data() + 5, pk_ptr, pk_len);
      save_current_row_key(sec_row_key_buf_.data(), sec_row_key_buf_.size());
      return 0;
    }

    // 2. Build primary key for row lookup [0x02 | table_id | pk-or-rowid]
    sec_row_key_buf_.resize(5 + pk_len);
    sec_row_key_buf_[0] = kNsRow;
    sec_row_key_buf_[1] = static_cast<uint8_t>((table_id_ >> 24) & 0xFF);
    sec_row_key_buf_[2] = static_cast<uint8_t>((table_id_ >> 16) & 0xFF);
    sec_row_key_buf_[3] = static_cast<uint8_t>((table_id_ >>  8) & 0xFF);
    sec_row_key_buf_[4] = static_cast<uint8_t>( table_id_        & 0xFF);
    std::memcpy(sec_row_key_buf_.data() + 5, pk_ptr, pk_len);

    // 3. Fetch full row from primary key namespace
    auto *txn = txn_cached_;
    if (!txn) { return HA_ERR_GENERIC; }

    int rc = txn->get(sec_row_key_buf_.data(), sec_row_key_buf_.size(),
                      row_value_buf_);
    if (rc != 1) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    decode_row(table,
               reinterpret_cast<const uint8_t *>(row_value_buf_.data()),
               row_value_buf_.size(), buf);
    save_current_row_key(sec_row_key_buf_.data(), sec_row_key_buf_.size());
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

  auto *txn = txn_cached_;
  if (!txn) { return HA_ERR_GENERIC; }

  int found = txn->get(pos, ref_length, row_value_buf_);
  if (found < 0) { return HA_ERR_GENERIC; }
  if (found == 0) { return HA_ERR_KEY_NOT_FOUND; }

  decode_pk(table, pos, ref_length, buf, decode_pk_scratch_);

  decode_row(table,
             reinterpret_cast<const uint8_t *>(row_value_buf_.data()),
             row_value_buf_.size(), buf);

  // Save the position-key as the current row key in case mutation follows.
  save_current_row_key(pos, ref_length);

  return 0;
}

// ---------------------------------------------------------------------------
// info() — optimizer statistics
// ---------------------------------------------------------------------------

int ha_bytecaskdb::info(uint flag) {
  if (flag & HA_STATUS_VARIABLE) {
    stats.records = static_cast<ha_rows>(
        std::max(int64_t{0}, row_count_atomic_->load()));
  }
  if (flag & HA_STATUS_AUTO) {
    stats.auto_increment_value = autoinc_atomic_->load() + 1;
  }
  if (flag & HA_STATUS_ERRKEY) {
    errkey = saved_errkey_;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// records_in_range() — estimate rows in key range for optimizer
// ---------------------------------------------------------------------------

ha_rows ha_bytecaskdb::records_in_range(uint /*index*/, const key_range */*min_key*/,
                                         const key_range */*max_key*/,
                                         page_range */*pages*/) {
  return stats.records > 0 ? std::max(ha_rows(2), stats.records / 10) : 2;
}

// ---------------------------------------------------------------------------
// save_current_row_key() — private helper
// ---------------------------------------------------------------------------

void ha_bytecaskdb::save_current_row_key(const uint8_t *data, std::size_t len) {
  current_row_key_.assign(data, data + len);
}

// ---------------------------------------------------------------------------
// check_if_supported_inplace_alter() — DROP FK and column rename are inplace
// ---------------------------------------------------------------------------

enum_alter_inplace_result ha_bytecaskdb::check_if_supported_inplace_alter(
    TABLE * /*altered_table*/, Alter_inplace_info *ha_alter_info) {
  const ulonglong dominated =
      ALTER_DROP_FOREIGN_KEY | ALTER_COLUMN_NAME;
  if (ha_alter_info->handler_flags & dominated) {
    return HA_ALTER_INPLACE_NO_LOCK;
  }
  return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

// ---------------------------------------------------------------------------
// inplace_alter_table() — DROP FK / column rename: update catalog
// ---------------------------------------------------------------------------

bool ha_bytecaskdb::inplace_alter_table(TABLE * /*altered_table*/,
                                         Alter_inplace_info *ha_alter_info) {
  if (!g_db) { return true; }

  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (!meta) { return false; }

  bool has_drop_fk = (ha_alter_info->handler_flags & ALTER_DROP_FOREIGN_KEY) != 0;
  bool has_rename  = (ha_alter_info->handler_flags & ALTER_COLUMN_NAME) != 0;

  if (meta->fks.empty() && !has_drop_fk) {
    return false;
  }

  TableMeta updated = *meta;
  pre_alter_fks_ = updated.fks;

  // Handle DROP FOREIGN KEY
  if (has_drop_fk) {
    List_iterator<Alter_drop> it(ha_alter_info->alter_info->drop_list);
    Alter_drop *drop;
    while ((drop = it++)) {
      if (drop->type != Alter_drop::FOREIGN_KEY) continue;
      const char *drop_name = drop->name;
      bool found = false;
      for (auto fk_it = updated.fks.begin(); fk_it != updated.fks.end(); ++fk_it) {
        if (my_strcasecmp(system_charset_info, fk_it->name.c_str(), drop_name) == 0) {
          updated.fks.erase(fk_it);
          found = true;
          break;
        }
      }
      if (!found) {
        my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0), drop_name);
        return true;
      }
    }
  }

  // Handle column renames — update FK column references
  if (has_rename) {
    List_iterator<Create_field> field_it(ha_alter_info->alter_info->create_list);
    Create_field *cf;
    while ((cf = field_it++)) {
      if (!cf->field) continue;
      if (my_strcasecmp(system_charset_info,
                        cf->field->field_name.str, cf->field_name.str) == 0)
        continue;
      for (auto &fk : updated.fks) {
        for (auto &col : fk.fk_cols) {
          if (my_strcasecmp(system_charset_info,
                            col.c_str(), cf->field->field_name.str) == 0) {
            col.assign(cf->field_name.str, cf->field_name.length);
          }
        }
      }
    }
  }

  updated.schema_version = updated.fks.empty() ? 2 : 3;

  const char *table_name = table->s->normalized_path.str;
  if (!catalog_put_table_meta(g_db, updated, table_name)) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// commit_inplace_alter_table() — rollback restores pre-ALTER FK list
// ---------------------------------------------------------------------------

bool ha_bytecaskdb::commit_inplace_alter_table(TABLE * /*altered_table*/,
                                                Alter_inplace_info * /*ha_alter_info*/,
                                                bool commit) {
  if (commit) {
    pre_alter_fks_.clear();
    return false;
  }

  // Rollback: restore the original FK list.
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (!meta) { return true; }

  TableMeta restored = *meta;
  restored.fks = std::move(pre_alter_fks_);
  restored.schema_version = restored.fks.empty() ? 2 : 3;

  const char *table_name = table->s->normalized_path.str;
  if (!catalog_put_table_meta(g_db, restored, table_name)) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// get_foreign_key_list() — expose FK metadata for DROP FK resolution
// ---------------------------------------------------------------------------

int ha_bytecaskdb::get_foreign_key_list(THD *thd,
                                         List<FOREIGN_KEY_INFO> *f_key_list) {
  const TableMeta *meta = catalog_lookup_meta(table_id_);
  if (!meta) { return 0; }

  for (const auto &fk : meta->fks) {
    auto *fk_info = static_cast<FOREIGN_KEY_INFO *>(
        thd_alloc(thd, sizeof(FOREIGN_KEY_INFO)));
    new (fk_info) FOREIGN_KEY_INFO();

    fk_info->foreign_id = thd_make_lex_string(thd, nullptr,
                                               fk.name.c_str(),
                                               fk.name.length(), 1);
    fk_info->foreign_db = thd_make_lex_string(thd, nullptr,
                                               table->s->db.str,
                                               table->s->db.length, 1);
    fk_info->foreign_table = thd_make_lex_string(thd, nullptr,
                                                  table->s->table_name.str,
                                                  table->s->table_name.length, 1);
    fk_info->referenced_db = thd_make_lex_string(thd, nullptr,
                                                  fk.ref_db.c_str(),
                                                  fk.ref_db.length(), 1);
    fk_info->referenced_table = thd_make_lex_string(thd, nullptr,
                                                     fk.ref_table.c_str(),
                                                     fk.ref_table.length(), 1);
    fk_info->referenced_key_name = thd_make_lex_string(thd, nullptr,
                                                        "PRIMARY", 7, 1);
    fk_info->update_method = static_cast<enum_fk_option>(fk.update_opt);
    fk_info->delete_method = static_cast<enum_fk_option>(fk.delete_opt);

    for (const auto &col : fk.fk_cols) {
      LEX_CSTRING *col_name = thd_make_lex_string(thd, nullptr,
                                                   col.c_str(), col.length(), 1);
      fk_info->foreign_fields.push_back(col_name);
    }
    for (const auto &col : fk.ref_cols) {
      LEX_CSTRING *col_name = thd_make_lex_string(thd, nullptr,
                                                   col.c_str(), col.length(), 1);
      fk_info->referenced_fields.push_back(col_name);
    }

    f_key_list->push_back(fk_info);
  }
  return 0;
}

} // namespace bytecaskdb
