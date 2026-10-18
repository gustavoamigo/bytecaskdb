// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.hpp — tables, their permanent column ids, primary keys and
// secondary indexes, kept in memory and persisted under namespace 0x01.
//
// prototype: the minimum the end-to-end prototype needs (#497). Written from
// scratch: the plugin's catalog is GPL-2.0 and this directory is MIT. Its key
// space follows the plugin's where they overlap (counters under 01 01, table
// records under 01 02); records are keyed by id, with a name entry under
// 01 03, as #489 decided.
//
// Key space:
//   01 01 01        next object id (tables, indexes, layouts), u64 big-endian
//   01 01 02        catalog version, bumped by every DDL, u64 big-endian
//   01 02 <id:4>    table record (indexes included), append-only serialization
//   01 03 <name>    name entry: kind(1) id(4, big-endian)
//
// Not thread-safe: one Catalog per process, mutated under the caller's lock.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bytecaskdb-sql-layer/codec/key_codec.hpp"
#include "bytecaskdb-sql-layer/codec/row_codec.hpp"
#include "include/bytecask.hpp"

namespace bytecask::sql {

struct Column {
  std::uint32_t id;  // permanent; never reused
  std::string name;
  PhysType type;
  bool nullable;
};

// A non-unique secondary index: its columns, then the primary key's.
struct Index {
  std::uint32_t id;
  std::string name;
  std::vector<std::uint32_t> col_ids;
};

struct Table {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t version{1};  // bumped by every DDL on this table
  std::uint32_t layout_id{0};
  std::uint32_t next_col_id{0};
  std::vector<Column> columns;
  std::vector<std::uint32_t> pk_col_ids;
  std::vector<Index> indexes;
  RowLayout layout;

  [[nodiscard]] auto column(std::string_view name) const -> const Column*;
  [[nodiscard]] auto column(std::uint32_t id) const -> const Column*;
  [[nodiscard]] auto index(std::string_view name) const -> const Index*;
};

struct ColumnSpec {
  std::string name;
  PhysType type;
  bool nullable;
};

// What a key builder needs for a table (its primary key) or an index.
struct KeyLayout {
  std::uint8_t ns;
  const Table* table;
  const Index* index;  // nullptr for a table's primary key
  std::vector<KeyPart> parts;  // every part: index columns, then primary key
  std::size_t index_parts;     // how many of them precede the primary key
};

// A layout for result rows: the column types in emit order.
struct ResultLayout {
  std::vector<PhysType> types;
  RowLayout layout;
};

class Catalog {
public:
  // Reads every table record under namespace 0x01.
  [[nodiscard]] static auto load(DB& db) -> Catalog;

  // Each DDL is one atomic batch: the record, the name entry, the counters,
  // and for CREATE INDEX the entries for rows already in the table. Throws
  // std::invalid_argument for a duplicate name or a bad definition.
  auto create_table(DB& db, const std::string& name, const std::vector<ColumnSpec>& cols,
                    const std::vector<std::string>& pk) -> const Table&;
  auto create_index(DB& db, const std::string& name, const std::string& table,
                    const std::vector<std::string>& cols) -> const Index&;

  [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }
  [[nodiscard]] auto table(std::string_view name) const -> const Table*;
  [[nodiscard]] auto table(std::uint32_t id) const -> const Table*;
  // A table or an index: both draw ids from one counter.
  [[nodiscard]] auto key_layout(std::uint32_t table_or_index) const -> const KeyLayout*;
  // A table's row layout, or a result layout.
  [[nodiscard]] auto row_layout(std::uint32_t layout_id) const -> const RowLayout*;
  [[nodiscard]] auto table_of_layout(std::uint32_t layout_id) const -> const Table*;

  // prototype: result layouts live in memory only, interned by their types,
  // with ids from kResultLayoutBase up in order of first use. A plan's result
  // layout id is therefore stable within a process, not across processes.
  static constexpr std::uint32_t kResultLayoutBase = 0x8000'0000u;
  auto result_layout(const std::vector<PhysType>& types) -> std::uint32_t;
  [[nodiscard]] auto result_types(std::uint32_t layout_id) const -> const std::vector<PhysType>*;

  // The constants a plan on this table needs, in the names generated plans
  // use: TBL_, LAY_, IDX_, and per column <COL>_OFF or <COL>_SLOT and <COL>_COL.
  [[nodiscard]] auto constants(const Table& t) const
      -> std::vector<std::pair<std::string, std::uint32_t>>;

private:
  void index_table(Table t);
  void rebuild_key_layouts();
  auto next_id(std::uint32_t n) -> std::uint32_t;  // reserves n ids, returns the first

  std::uint64_t next_id_{1};
  std::uint64_t version_{0};
  std::map<std::uint32_t, Table> tables_;
  std::map<std::string, std::uint32_t, std::less<>> names_;
  std::map<std::uint32_t, KeyLayout> key_layouts_;
  std::map<std::uint32_t, std::uint32_t> layout_tables_;  // layout id -> table id
  std::vector<std::unique_ptr<ResultLayout>> results_;
};

// Encodes the primary key of `t` from a row: kNsRow, the table id, the parts.
[[nodiscard]] auto encode_pk_from_row(const Table& t, BytesView row, Bytes& out) -> bool;

}  // namespace bytecask::sql
