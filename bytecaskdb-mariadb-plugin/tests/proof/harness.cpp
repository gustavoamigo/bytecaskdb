// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// harness.cpp — PluginTestHarness implementation.

#include "harness.h"
#include "key_encoding.h"
#include "row_encoding.h"

#include <cstring>
#include <unistd.h>

namespace bytecaskdb::testing {

PluginTestHarness::PluginTestHarness(TableSpec spec) {
  static std::atomic<int> counter{0};
  path_ = std::filesystem::temp_directory_path() /
          ("bcdb_proof_" + std::to_string(::getpid()) +
           "_" + std::to_string(counter.fetch_add(1)));
  std::filesystem::remove_all(path_);
  std::filesystem::create_directories(path_);

  holder_ = std::make_unique<DBHolder>(path_);
  g_db = &holder_->db;
  bytecaskdb_hton = &hton_;

  build_table_structures(spec);
  register_catalog(spec);

  handler_ = std::make_unique<ha_bytecaskdb>(&hton_, &share_);
  handler_->table = &table_;

  // Open the handler (initializes table_id_, ref_length, etc.).
  handler_->open("./test/proof_t", 0, 0);
  handler_->info(0);

  // Begin a transaction via external_lock.
  g_stub_thd_options = OPTION_BEGIN;
  handler_->external_lock(&thd_, F_WRLCK);
  txn_ = static_cast<MariaDBTxn *>(thd_get_ha_data(&thd_, &hton_));
}

PluginTestHarness::~PluginTestHarness() {
  handler_->external_lock(&thd_, F_UNLCK);
  g_stub_thd_options = 0;
  handler_.reset();

  catalog_drop_row_count(table_id_);
  catalog_drop_rowid(table_id_);
  catalog_drop_autoinc(table_id_);
  catalog_evict_from_cache("./test/proof_t");

  g_db = nullptr;
  bytecaskdb_hton = nullptr;
  holder_.reset();
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

void PluginTestHarness::build_table_structures(const TableSpec &spec) {
  uint num_cols = spec.num_int_columns;
  uint num_keys = 1 + static_cast<uint>(spec.secondary_indexes.size());

  // Record buffer: null_bytes + (4 bytes per int column).
  uint null_bytes = (num_cols + 7) / 8;
  uint reclength = null_bytes + num_cols * 4;
  record_buf_.resize(reclength, 0);

  // Fields.
  fields_.reserve(num_cols);
  field_ptrs_.reserve(num_cols + 1);
  for (uint i = 0; i < num_cols; ++i) {
    auto f = std::make_unique<Field_long>();
    f->ptr = record_buf_.data() + null_bytes + i * 4;
    f->field_index = static_cast<uint16_t>(i);
    f->field_length = 4;
    field_ptrs_.push_back(f.get());
    fields_.push_back(std::move(f));
  }
  field_ptrs_.push_back(nullptr);  // NULL-terminated

  // TABLE_SHARE.
  share_.primary_key = spec.has_pk ? 0 : MAX_KEY;
  share_.reclength = reclength;
  share_.null_bytes = static_cast<uint16_t>(null_bytes);
  share_.fields = num_cols;
  share_.keys = num_keys;

  // Keys: PK is key[0] (first column), secondary indexes follow.
  uint total_key_parts = 0;
  if (spec.has_pk) total_key_parts += 1;
  for (const auto &idx : spec.secondary_indexes) {
    total_key_parts += idx.key_parts;
  }
  key_parts_.resize(total_key_parts);
  keys_.resize(num_keys);

  uint kp_offset = 0;
  if (spec.has_pk) {
    keys_[0].key_length = 4;
    keys_[0].user_defined_key_parts = 1;
    keys_[0].flags = HA_NOSAME;
    keys_[0].key_part = &key_parts_[kp_offset];
    key_parts_[kp_offset].field = field_ptrs_[0];
    key_parts_[kp_offset].fieldnr = 1;
    key_parts_[kp_offset].field_index = 0;
    key_parts_[kp_offset].length = 4;
    key_parts_[kp_offset].store_length = 4;
    ++kp_offset;
  }

  for (uint s = 0; s < spec.secondary_indexes.size(); ++s) {
    const auto &idx_spec = spec.secondary_indexes[s];
    uint key_idx = s + 1;
    keys_[key_idx].key_length = idx_spec.key_parts * 4;
    keys_[key_idx].user_defined_key_parts = idx_spec.key_parts;
    keys_[key_idx].flags = idx_spec.is_unique ? HA_NOSAME : 0;
    keys_[key_idx].key_part = &key_parts_[kp_offset];
    for (uint p = 0; p < idx_spec.key_parts; ++p) {
      uint col = idx_spec.column_indexes[p];
      key_parts_[kp_offset + p].field = field_ptrs_[col];
      key_parts_[kp_offset + p].fieldnr = static_cast<uint16_t>(col + 1);
      key_parts_[kp_offset + p].field_index = static_cast<uint16_t>(col);
      key_parts_[kp_offset + p].length = 4;
      key_parts_[kp_offset + p].store_length = 4;
    }
    kp_offset += idx_spec.key_parts;
  }

  // TABLE.
  table_.s = &share_;
  table_.key_info = keys_.data();
  table_.field = field_ptrs_.data();
  table_.record[0] = record_buf_.data();
  table_.record[1] = nullptr;
}

void PluginTestHarness::register_catalog(const TableSpec &spec) {
  uint32_t tid = catalog_alloc_table_id(g_db);
  table_id_ = tid;

  TableMeta meta;
  meta.table_id = tid;
  meta.schema_version = 2;
  meta.full_name = normalize_table_name("./test/proof_t");
  meta.reclength = static_cast<uint16_t>(share_.reclength);
  meta.null_bytes = share_.null_bytes;
  meta.pk_parts = spec.has_pk ? 1 : 0;

  for (uint i = 0; i < spec.num_int_columns; ++i) {
    ColumnMeta col;
    col.field_type = MYSQL_TYPE_LONG;
    col.field_length = 4;
    col.is_nullable = 0;
    col.charset_id = 0;
    meta.columns.push_back(col);
  }

  for (const auto &idx_spec : spec.secondary_indexes) {
    IndexMeta idx;
    idx.index_id = static_cast<uint16_t>(idx_spec.index_id);
    idx.key_parts = static_cast<uint16_t>(idx_spec.key_parts);
    idx.is_unique = idx_spec.is_unique ? 1 : 0;
    for (uint col : idx_spec.column_indexes) {
      idx.column_indexes.push_back(static_cast<uint16_t>(col));
    }
    meta.indexes.push_back(idx);
  }

  catalog_put_table_meta(g_db, meta, "./test/proof_t");
}

void PluginTestHarness::fill_record(uchar *buf, const std::vector<int32_t> &vals) {
  std::memset(buf, 0, share_.reclength);
  for (uint i = 0; i < vals.size() && i < share_.fields; ++i) {
    int32_t v = vals[i];
    std::memcpy(buf + share_.null_bytes + i * 4, &v, sizeof(v));
    fields_[i]->ptr = buf + share_.null_bytes + i * 4;
  }
}

int PluginTestHarness::insert_row(const std::vector<int32_t> &column_values) {
  fill_record(record_buf_.data(), column_values);
  return handler_->write_row(record_buf_.data());
}

int PluginTestHarness::update_row(const std::vector<int32_t> &old_vals,
                                   const std::vector<int32_t> &new_vals) {
  // Simulate the read-before-update protocol: encode old PK and set
  // current_row_key_ as if a prior read had positioned the cursor.
  std::vector<uchar> old_buf(share_.reclength, 0);
  fill_record(old_buf.data(), old_vals);
  auto old_key = encode_pk(&table_, old_buf.data(), table_id_);
  handler_->set_current_row_key(old_key);

  // Encode new record into record[0].
  fill_record(record_buf_.data(), new_vals);

  return handler_->update_row(old_buf.data(), record_buf_.data());
}

int PluginTestHarness::delete_row(const std::vector<int32_t> &column_values) {
  // Simulate the read-before-delete protocol.
  fill_record(record_buf_.data(), column_values);
  auto key = encode_pk(&table_, record_buf_.data(), table_id_);
  handler_->set_current_row_key(key);

  return handler_->delete_row(record_buf_.data());
}

int PluginTestHarness::commit() {
  int rc = txn_->commit(&thd_, true);
  // After full commit, re-begin to keep txn_cached_ usable for next DML.
  handler_->external_lock(&thd_, F_WRLCK);
  txn_ = static_cast<MariaDBTxn *>(thd_get_ha_data(&thd_, &hton_));
  return rc;
}

void PluginTestHarness::rollback() {
  txn_->rollback(&thd_, true);
  // After rollback, re-begin to keep txn_cached_ usable.
  handler_->external_lock(&thd_, F_WRLCK);
  txn_ = static_cast<MariaDBTxn *>(thd_get_ha_data(&thd_, &hton_));
}

void PluginTestHarness::stmt_boundary() {
  txn_->commit(&thd_, false);
  handler_->external_lock(&thd_, F_WRLCK);
}

void PluginTestHarness::begin_stmt() {
  handler_->external_lock(&thd_, F_WRLCK);
  txn_ = static_cast<MariaDBTxn *>(thd_get_ha_data(&thd_, &hton_));
}

void PluginTestHarness::inject_concurrent_write(
    const std::vector<int32_t> &column_values) {
  // Fill a temporary record buffer with the given values.
  std::vector<uchar> buf(share_.reclength, 0);
  for (uint i = 0; i < column_values.size() && i < share_.fields; ++i) {
    int32_t v = column_values[i];
    std::memcpy(buf.data() + share_.null_bytes + i * 4, &v, sizeof(v));
    fields_[i]->ptr = buf.data() + share_.null_bytes + i * 4;
  }

  // Encode the PK for this row.
  auto key = encode_pk(&table_, buf.data(), table_id_);

  // Write directly to the DB, bypassing the transaction — this creates
  // a concurrent modification that the harness's snapshot won't see.
  auto key_view = bytecask::BytesView{
      reinterpret_cast<const std::byte *>(key.data()), key.size()};
  auto val_view = bytecask::BytesView{
      reinterpret_cast<const std::byte *>(buf.data()), buf.size()};
  holder_->db.put(bytecask::WriteOptions{.sync = true}, key_view, val_view);

  // Restore field pointers to the main record buffer.
  for (uint i = 0; i < fields_.size(); ++i) {
    fields_[i]->ptr = record_buf_.data() + share_.null_bytes + i * 4;
  }
}

int64_t PluginTestHarness::row_counter() const {
  return catalog_row_count(table_id_);
}

} // namespace bytecaskdb::testing
