// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// fixture.cppm — the prototype's schema on a fresh database, and the runtime
// handles a plan needs:
//   CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT);
//   CREATE INDEX t_by_name ON t (name);

module;
#include <cstdint>
#include <memory>
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

// Every column of a row, by its layout: NULL, or the typed value.
inline auto decode_row(const bytecask::sql::RowLayout& layout, bytecask::sql::BytesView row)
    -> std::vector<bc_value> {
  using bytecask::sql::PhysType;
  std::vector<bc_value> out;
  for (const auto& c : layout.columns) {
    if (bytecask::sql::row_is_null(row, c)) {
      out.emplace_back(std::monostate{});
      continue;
    }
    switch (c.type) {
      case PhysType::I64: out.emplace_back(bytecask::sql::row_i64(row, c.pos).value_or(0)); break;
      case PhysType::F64: out.emplace_back(bytecask::sql::row_f64(row, c.pos).value_or(0.0)); break;
      case PhysType::Bool: out.emplace_back(bytecask::sql::row_bool(row, c.pos).value_or(false)); break;
      case PhysType::Bytes: {
        auto v = bytecask::sql::row_bytes(row, c.pos).value_or(bytecask::sql::BytesView{});
        out.emplace_back(bytecask::sql::Bytes(v.begin(), v.end()));
        break;
      }
    }
  }
  return out;
}

struct Fixture {
  TempDir dir{"plan"};
  std::unique_ptr<bytecask::DB> db;  // DB is not movable: open() is elided into new
  bytecask::sql::Catalog cat;
  bc_db bdb{};
  bc_catalog bcat{};
  bc_ctx* cx{nullptr};

  Fixture() { open(true); }
  ~Fixture() { close(); }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  void open(bool create) {
    db.reset(new bytecask::DB(bytecask::DB::open(dir.path())));
    cat = bytecask::sql::Catalog::load(*db);
    if (create) {
      using bytecask::sql::PhysType;
      (void)cat.create_table(*db, "t",
                             {{"id", PhysType::I64, false}, {"name", PhysType::Bytes, true}, {"qty", PhysType::I64, true}},
                             {"id"});
      (void)cat.create_index(*db, "t_by_name", "t", {"name"});
    }
    bdb = {db.get()};
    bcat = {&cat};
    cx = bc_ctx_new(&bdb, &bcat, 1u << 20);
  }

  void close() {
    bc_ctx_free(cx);
    cx = nullptr;
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
      out.push_back(decode_row(rl, std::as_bytes(std::span{row, len})));
    }
    return out;
  }
};

// A plan's result_types as the bc_type codes a bc_schema_ref carries.
inline auto type_codes(std::span<const bytecask::sql::PhysType> types) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  for (auto t : types) out.push_back(static_cast<std::uint8_t>(t));
  return out;
}

inline auto params(std::vector<bc_value> v) -> bc_params { return bc_params{std::move(v)}; }

}  // namespace sql_test
