// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// fixture.cppm — the prototype's schema on a fresh database, and the runtime
// handles a plan needs:
//   CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT);
//   CREATE INDEX t_by_name ON t (name);

module;
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

export module bytecask.sql.tests.fixture;

export import bytecask.sql.runtime;
export import bytecask.sql.tests.util;

export namespace sql_test {

struct Fixture {
  TempDir dir{"plan"};
  std::unique_ptr<bytecask::DB> db;  // DB is not movable: open() is elided into new
  std::unique_ptr<bytecask::sql::LiveCatalog> live;
  bc_db bdb{};
  bc_catalog bcat{};
  bc_ctx* cx{nullptr};

  Fixture() { open(true); }
  ~Fixture() { close(); }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  void open(bool create) {
    db.reset(new bytecask::DB(bytecask::DB::open(dir.path())));
    live = std::make_unique<bytecask::sql::LiveCatalog>(*db);
    if (create) {
      using bytecask::sql::PhysType;
      ddl(bytecask::sql::CreateTableDdl{
          "t", {col("id", PhysType::I64, false), col("name", PhysType::Bytes, true), col("qty", PhysType::I64, true)}, {"id"}});
      ddl(bytecask::sql::CreateIndexDdl{"t_by_name", "t", {{"name"}}});
    }
    bdb = {db.get()};
    bcat = {live.get()};
    cx = bc_ctx_new(&bdb, &bcat, 1u << 20);
  }

  // The live catalog's current version. The reference is valid until the
  // next DDL publishes another.
  [[nodiscard]] auto cat() const -> const bytecask::sql::Catalog& { return *live->current(); }
  void ddl(const bytecask::sql::Ddl& d) { (void)live->apply(d); }

  // A column of the SQL type each physical type is the widening of.
  static auto col(std::string name, bytecask::sql::PhysType t, bool nullable) -> bytecask::sql::ColumnSpec {
    using bytecask::sql::SqlKind;
    auto kind = SqlKind::BigInt;
    switch (t) {
      case bytecask::sql::PhysType::I64: kind = SqlKind::BigInt; break;
      case bytecask::sql::PhysType::F64: kind = SqlKind::Double; break;
      case bytecask::sql::PhysType::Bool: kind = SqlKind::Boolean; break;
      case bytecask::sql::PhysType::Bytes: kind = SqlKind::Bytea; break;
    }
    return {std::move(name), {kind}, nullable, {}};
  }

  void close() {
    bc_ctx_free(cx);
    cx = nullptr;
    live.reset();
    if (db) db->close();
    db.reset();
  }

  void reopen() {
    close();
    open(false);
  }

  // Result rows of the last execution, decoded by the plan's result types.
  auto results(std::span<const bytecask::sql::PhysType> types) -> std::vector<std::vector<bc_value>> {
    std::vector<std::vector<bc_value>> out;
    auto rl = bytecask::sql::make_result_layout(types);
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx, &row, &len)) {
      out.push_back(bytecask::sql::decode_row(rl, std::as_bytes(std::span{row, len})));
    }
    return out;
  }
};

// A plan's result_types as the bc_type codes a bc_schema_ref carries.
inline auto type_codes(std::span<const bytecask::sql::PhysType> types) -> std::vector<std::uint8_t> {
  return bytecask::sql::result_type_codes(types);
}

inline auto params(std::vector<bc_value> v) -> bc_params { return bc_params{std::move(v)}; }

// Every column of every table, as a bc_schema_ref declares them: what a
// hand-written test plan may read or set.
inline auto all_cols(const bytecask::sql::Catalog& cat) -> std::vector<bc_col_ref> {
  std::vector<bc_col_ref> out;
  for (const auto& [id, t] : cat.tables()) {
    for (const auto& c : t.columns) out.push_back({id, c.id, static_cast<std::uint8_t>(c.type)});
  }
  return out;
}

// Every table and index, and every table: what a hand-written test plan may
// name and write.
inline auto all_objects(const bytecask::sql::Catalog& cat) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  for (const auto& [id, t] : cat.tables()) {
    out.push_back(id);
    for (const auto& ix : t.indexes) out.push_back(ix.id);
  }
  return out;
}
inline auto all_tables(const bytecask::sql::Catalog& cat) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  for (const auto& [id, t] : cat.tables()) out.push_back(id);
  return out;
}

// A bc_schema_ref and the arrays it points into. Each get() is a new
// reference, with a fresh id.
struct SchemaRef {
  std::vector<std::uint32_t> read, written;
  std::vector<bc_col_ref> cols;
  std::vector<std::uint8_t> codes;
  std::uint64_t version{0};

  [[nodiscard]] auto get() const -> bc_schema_ref {
    auto n = [](const auto& v) { return static_cast<std::uint32_t>(v.size()); };
    return {bc_ref_id(), version, read.data(), n(read), written.data(), n(written),
            cols.data(), n(cols), codes.data(), n(codes)};
  }
};

// Counts the live entries of a table or an index, straight from the engine.
inline auto count_entries(bytecask::DB& db, std::uint8_t ns, std::uint32_t id) -> int {
  bytecask::sql::Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, ns, id);
  auto n = 0;
  for (const auto& k : db.keys_from({}, prefix)) {
    if (k.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), k.begin())) break;
    ++n;
  }
  return n;
}

}  // namespace sql_test
