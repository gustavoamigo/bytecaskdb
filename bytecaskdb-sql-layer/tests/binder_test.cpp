// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include "bytecaskdb-sql-layer/sql/binder.hpp"
#include "fixture.hpp"

using namespace bytecask::sql;

namespace {

auto bind_sql(const Catalog& cat, std::string_view sql) -> bound::Bound { return bound::bind(ast::parse(sql), cat); }

auto rejected(const Catalog& cat, std::string_view sql) -> std::pair<std::string, std::int32_t> {
  try {
    (void)bind_sql(cat, sql);
  } catch (const ast::SqlError& e) {
    return {e.what(), e.pos()};
  }
  FAIL("accepted: " << sql);
  return {};
}

}  // namespace

TEST_CASE("binder: names become ids, parameters get types from use", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat;
  {
    auto b = bind_sql(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v BYTEA NOT NULL, w BIGINT)");
    auto& s = std::get<bound::CreateTable>(b.stmt);
    CHECK(s.pk == std::vector<std::string>{"k"});
    REQUIRE(s.columns.size() == 3);
    CHECK(s.columns[1].type == PhysType::Bytes);
    CHECK_FALSE(s.columns[1].nullable);
    CHECK(s.columns[2].nullable);
  }
  {
    auto b = bind_sql(cat, "INSERT INTO t VALUES ($1, $2, $3)");
    auto& s = std::get<bound::Insert>(b.stmt);
    CHECK(s.table->id == 1);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::Bytes, PhysType::I64});
    REQUIRE(s.values.size() == 3);
    CHECK(s.values[1]->param == 1);
  }
  {
    auto b = bind_sql(cat, "INSERT INTO t (qty, id) VALUES ($1, $2)");
    auto& s = std::get<bound::Insert>(b.stmt);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
    CHECK(s.values[0]->param == 1);
    CHECK_FALSE(s.values[1].has_value());  // name is not given: NULL
    CHECK(s.values[2]->param == 0);
  }
  {
    auto b = bind_sql(cat, "SELECT id, qty FROM t WHERE id = $1");
    auto& s = std::get<bound::Select>(b.stmt);
    CHECK(s.targets == std::vector<std::uint32_t>{0, 2});
    REQUIRE(s.where.size() == 1);
    CHECK(s.where[0].col_id == 0);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64});
  }
  {
    auto b = bind_sql(cat, "SELECT id FROM t WHERE $1 <= name AND name < $2");
    auto& s = std::get<bound::Select>(b.stmt);
    CHECK(s.where[0].op == ast::Compare::Op::Ge);  // normalised: column on the left
    CHECK(b.param_types == std::vector<PhysType>{PhysType::Bytes, PhysType::Bytes});
  }
  {
    auto b = bind_sql(cat, "UPDATE t SET qty = qty - $2 WHERE id = $1");
    auto& s = std::get<bound::Update>(b.stmt);
    REQUIRE(s.set.size() == 1);
    CHECK(s.set[0].first == 2);
    CHECK(s.set[0].second.kind == bound::Expr::Kind::Sub);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
  }
  {
    auto b = bind_sql(cat, "DELETE FROM t WHERE id = $1");
    CHECK(std::get<bound::Delete>(b.stmt).where.size() == 1);
  }
}

TEST_CASE("binder: anything outside the scope is rejected with a message and position", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat;
  //                       0         1         2         3         4
  //                       01234567890123456789012345678901234567890123
  CHECK(rejected(cat, "SELECT id FROM nope WHERE id = $1").second == 0);
  CHECK(rejected(cat, "SELECT nope FROM t WHERE id = $1") == std::pair<std::string, std::int32_t>{
                                                                   "column \"nope\" does not exist", 7});
  CHECK(rejected(cat, "SELECT id FROM t WHERE name = $1 AND qty = $1").second == 43);  // $1 bytea, then bigint
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = $2").first ==
        "could not determine data type of parameter $1");
  CHECK(rejected(cat, "UPDATE t SET name = name - $1 WHERE id = $2").first == "operator - is only defined on bigint");
  CHECK(rejected(cat, "UPDATE t SET id = $1 WHERE id = $2").second == 13);
  CHECK(rejected(cat, "INSERT INTO t VALUES ($1, $2)").first == "INSERT has 2 values for 3 columns");
  CHECK(rejected(cat, "INSERT INTO t (name) VALUES ($1)").first == "column \"id\" is NOT NULL and has no value");
  CHECK(rejected(cat, "CREATE TABLE u (k INTEGER PRIMARY KEY)").second == 16);
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT)").first == "a table needs a PRIMARY KEY");
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = qty").first == "WHERE must compare a column with a parameter");
}
