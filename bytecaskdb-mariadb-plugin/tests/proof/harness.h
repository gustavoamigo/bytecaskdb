// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// harness.h — PluginTestHarness for proof tests.
//
// Provides a self-contained test environment that opens a real bytecask::DB,
// builds TABLE/KEY/Field structures from a TableSpec, registers metadata in
// the catalog, and creates an ha_bytecaskdb handler instance. DML methods
// (write_row, update_row, delete_row) can be called directly without a live
// MariaDB server process.

#pragma once

#include "bytecask.hpp"
#include "ha_bytecaskdb.h"
#include "bytecaskdb_txn.h"
#include "catalog.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace bytecaskdb::testing {

struct IndexSpec {
  uint index_id;
  uint key_parts;
  bool is_unique;
  std::vector<uint> column_indexes;
};

struct TableSpec {
  uint num_int_columns{2};
  bool has_pk{true};
  std::vector<IndexSpec> secondary_indexes;
};

class PluginTestHarness {
public:
  explicit PluginTestHarness(TableSpec spec);
  ~PluginTestHarness();

  PluginTestHarness(const PluginTestHarness &) = delete;
  PluginTestHarness &operator=(const PluginTestHarness &) = delete;

  // DML entry points — call ha_bytecaskdb methods directly.
  int insert_row(const std::vector<int32_t> &column_values);
  int update_row(const std::vector<int32_t> &old_vals,
                 const std::vector<int32_t> &new_vals);
  int delete_row(const std::vector<int32_t> &column_values);

  // Transaction control.
  int commit();
  void rollback();
  void stmt_boundary();   // commit(all=false) — statement boundary within txn
  void begin_stmt();      // external_lock(F_WRLCK) — start a new statement

  // Injects a concurrent write to the same key the harness would use for
  // the given column values. Creates an OCC conflict at commit time.
  void inject_concurrent_write(const std::vector<int32_t> &column_values);

  // Inspection.
  bytecask::DB &db() { return holder_->db; }
  int64_t row_counter() const;
  ha_bytecaskdb &handler() { return *handler_; }
  MariaDBTxn &txn() { return *txn_; }
  uint32_t table_id() const { return table_id_; }

private:
  void build_table_structures(const TableSpec &spec);
  void register_catalog(const TableSpec &spec);
  void fill_record(uchar *buf, const std::vector<int32_t> &vals);

  struct DBHolder {
    bytecask::DB db;
    explicit DBHolder(const std::filesystem::path &dir)
        : db{bytecask::DB::open(dir)} {}
  };

  std::filesystem::path path_;
  std::unique_ptr<DBHolder> holder_;
  uint32_t table_id_{0};

  // Stubbed MariaDB structures.
  TABLE_SHARE share_{};
  TABLE table_{};
  std::vector<KEY> keys_;
  std::vector<KEY_PART_INFO> key_parts_;
  std::vector<std::unique_ptr<Field_long>> fields_;
  std::vector<Field *> field_ptrs_;
  std::vector<uchar> record_buf_;

  // Handler and transaction.
  handlerton hton_{};
  THD thd_{};
  std::unique_ptr<ha_bytecaskdb> handler_;
  MariaDBTxn *txn_{nullptr};
};

} // namespace bytecaskdb::testing
