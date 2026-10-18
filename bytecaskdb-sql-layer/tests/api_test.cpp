// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The embedded API end to end: the seven statements of the prototype, in
// order, from SQL text to rows, then a fresh open of the same directory to
// show the catalog and the data persist.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;
using sql_test::bytes;

namespace {

const Value kNull = std::monostate{};
constexpr auto kCreateTable = "CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT)";
constexpr auto kCreateIndex = "CREATE INDEX t_by_name ON t (name)";
constexpr auto kInsert = "INSERT INTO t VALUES ($1, $2, $3)";
constexpr auto kById = "SELECT id, qty FROM t WHERE id = $1";
constexpr auto kByName = "SELECT id FROM t WHERE name >= $1 AND name < $2";
constexpr auto kUpdate = "UPDATE t SET qty = qty - $2 WHERE id = $1";
constexpr auto kDelete = "DELETE FROM t WHERE id = $1";

using Grid = std::vector<std::vector<Value>>;

auto i(std::int64_t v) -> Value { return v; }

auto ids(const Rows& r) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  for (const auto& row : r.rows) out.push_back(std::get<std::int64_t>(row[0]));
  return out;
}

}  // namespace

TEST_CASE("embedded API: the seven statements, then a fresh open", "[sql][api]") {
  sql_test::TempDir dir{"api"};
  {
    auto db = Database::open(dir.path());
    CHECK(db->execute(kCreateTable).affected == 0);
    CHECK(db->execute(kCreateIndex).affected == 0);

    const auto& insert = db->prepare(kInsert);
    CHECK(insert.param_types() == std::vector<PhysType>{PhysType::I64, PhysType::Bytes, PhysType::I64});
    CHECK(db->execute(insert, {i(1), bytes("apple"), i(10)}).affected == 1);
    CHECK(db->execute(insert, {i(2), bytes("banana"), i(20)}).affected == 1);
    CHECK(db->execute(insert, {i(3), kNull, i(30)}).affected == 1);
    CHECK(db->execute(insert, {i(4), bytes("cherry"), kNull}).affected == 1);
    try {
      (void)db->execute(insert, {i(1), bytes("again"), i(0)});
      FAIL("a duplicate key must throw");
    } catch (const ExecError& e) {
      CHECK(e.code() == BC_E_CONSTRAINT);
      CHECK(std::string{e.what()}.find("unique") != std::string::npos);
    }

    auto r = db->execute(kById, {i(1)});
    CHECK(r.columns == std::vector<std::string>{"id", "qty"});
    CHECK(r.rows == Grid{{i(1), i(10)}});
    CHECK(db->execute(kById, {i(4)}).rows == Grid{{i(4), kNull}});
    CHECK(db->execute(kById, {i(9)}).rows.empty());
    CHECK(ids(db->execute(kByName, {bytes("a"), bytes("c")})) == std::vector<std::int64_t>{1, 2});

    CHECK(db->execute(kUpdate, {i(1), i(3)}).affected == 1);
    CHECK(db->execute(kById, {i(1)}).rows == Grid{{i(1), i(7)}});
    CHECK(db->execute(kDelete, {i(2)}).affected == 1);
    CHECK(db->execute(kDelete, {i(2)}).affected == 0);
    CHECK(ids(db->execute(kByName, {bytes("a"), bytes("z")})) == std::vector<std::int64_t>{1, 4});

    CHECK_THROWS_AS(db->execute(kById, {bytes("x")}), std::invalid_argument);
    CHECK_THROWS_AS(db->execute(kById, {}), std::invalid_argument);
    CHECK_THROWS_AS(db->prepare("SELECT id FROM t JOIN u ON t.id = u.id"), ast::SqlError);
    CHECK(db->cached_plans() == 7);  // each text compiled once
    db->close();
  }
  // A fresh open: the catalog is reloaded from namespace 0x01 and the plans
  // are compiled again.
  auto db = Database::open(dir.path());
  CHECK(db->execute(kById, {i(1)}).rows == Grid{{i(1), i(7)}});
  CHECK(db->execute(kById, {i(2)}).rows.empty());
  CHECK(db->execute(kById, {i(3)}).rows == Grid{{i(3), i(30)}});
  CHECK(ids(db->execute(kByName, {bytes("a"), bytes("z")})) == std::vector<std::int64_t>{1, 4});
  CHECK(db->execute(kInsert, {i(5), bytes("banana"), i(50)}).affected == 1);
  CHECK(ids(db->execute(kByName, {bytes("b"), bytes("c")})) == std::vector<std::int64_t>{5});
  CHECK(db->execute(kUpdate, {i(5), i(8)}).affected == 1);
  CHECK(db->execute(kById, {i(5)}).rows == Grid{{i(5), i(42)}});
  CHECK_THROWS_AS(db->execute(kCreateTable), std::invalid_argument);  // the table survived the reopen
}

TEST_CASE("embedded API: a cached plan is recompiled after DDL", "[sql][api]") {
  sql_test::TempDir dir{"api_ddl"};
  auto db = Database::open(dir.path());
  (void)db->execute(kCreateTable);
  const auto& insert = db->prepare(kInsert);
  CHECK(insert.plan_source().find("IDX_T_BY_NAME") == std::string::npos);
  (void)db->execute(insert, {i(1), bytes("apple"), i(10)});
  // The index is created after the plan: the INSERT must now maintain it.
  (void)db->execute(kCreateIndex);
  (void)db->execute(insert, {i(2), bytes("apple"), i(20)});
  CHECK(insert.plan_source().find("IDX_T_BY_NAME") != std::string::npos);
  // Row 1 was backfilled by CREATE INDEX, row 2 by the recompiled INSERT.
  CHECK(ids(db->execute(kByName, {bytes("apple"), bytes("apple\x01")})) == std::vector<std::int64_t>{1, 2});
}
