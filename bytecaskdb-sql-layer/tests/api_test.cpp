// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The embedded API end to end: the seven statements of the prototype, in
// order, from SQL text to rows, then a fresh open of the same directory to
// show the catalog and the data persist.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

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
    // An upper bound alone: NULL < x is unknown, so row 3 (name NULL) is not returned.
    CHECK(ids(db->execute("SELECT id FROM t WHERE name < $1", {bytes("c")})) == std::vector<std::int64_t>{1, 2});
    CHECK(ids(db->execute("SELECT id FROM t WHERE name <= $1", {bytes("cherry")})) ==
          std::vector<std::int64_t>{1, 2, 4});
    CHECK(ids(db->execute("SELECT id FROM t WHERE name > $1", {bytes("apple")})) == std::vector<std::int64_t>{2, 4});

    CHECK(db->execute(kUpdate, {i(1), i(3)}).affected == 1);
    CHECK(db->execute(kById, {i(1)}).rows == Grid{{i(1), i(7)}});
    CHECK(db->execute(kDelete, {i(2)}).affected == 1);
    CHECK(db->execute(kDelete, {i(2)}).affected == 0);
    CHECK(ids(db->execute(kByName, {bytes("a"), bytes("z")})) == std::vector<std::int64_t>{1, 4});

    CHECK_THROWS_AS(db->execute(kById, {bytes("x")}), ParamError);
    CHECK_THROWS_AS(db->execute(kById, {}), ParamError);
    CHECK_THROWS_AS(db->prepare("SELECT id FROM t JOIN u ON t.id = u.id"), ast::SqlError);
    CHECK(db->cached_plans() == 10);  // each text compiled once
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
  CHECK_THROWS_AS(db->execute(kCreateTable), DdlRefused);  // the table survived the reopen
}

TEST_CASE("embedded API: a cached write plan maintains an index created after it", "[sql][api]") {
  sql_test::TempDir dir{"api_ddl"};
  auto db = Database::open(dir.path());
  (void)db->execute(kCreateTable);
  const auto& insert = db->prepare(kInsert);
  (void)db->execute(insert, {i(1), bytes("apple"), i(10)});
  auto compiled = db->compilations();
  // The index is created after the plan, which names no index: the runtime
  // writes the entry, and the plan is not recompiled.
  (void)db->execute(kCreateIndex);
  (void)db->execute(insert, {i(2), bytes("apple"), i(20)});
  CHECK(db->compilations() == compiled);
  CHECK(insert.plan_source().find("IDX_") == std::string::npos);
  // Row 1 was backfilled by CREATE INDEX, row 2 by the cached INSERT.
  CHECK(ids(db->execute(kByName, {bytes("apple"), bytes("apple\x01")})) == std::vector<std::int64_t>{1, 2});
}

TEST_CASE("embedded API: a plan the catalog refuses is recompiled and run again", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_recompile"};
  auto db = Database::open(dir.path());
  (void)db->execute(kCreateTable);
  (void)db->execute(kCreateIndex);
  const auto& insert = db->prepare(kInsert);
  const auto& by_id = db->prepare(kById);
  const auto& by_name = db->prepare(kByName);
  (void)db->execute(insert, {i(1), bytes("apple"), i(10)});
  CHECK(db->execute(by_id, {i(1)}).rows == Grid{{i(1), i(10)}});
  auto compiled = db->compilations();

  // The table is dropped and created again: new ids, so every cached plan is
  // refused by bc_ctx_run, recompiled from its text, and runs on the new table.
  (void)db->execute("DROP TABLE t");
  (void)db->execute(kCreateTable);
  (void)db->execute(kCreateIndex);
  CHECK(db->execute(insert, {i(2), bytes("banana"), i(20)}).affected == 1);
  CHECK(db->execute(by_id, {i(2)}).rows == Grid{{i(2), i(20)}});
  CHECK(db->execute(by_id, {i(1)}).rows.empty());
  CHECK(ids(db->execute(by_name, {bytes("a"), bytes("c")})) == std::vector<std::int64_t>{2});
  CHECK(db->compilations() == compiled + 3);
  // Once recompiled they run without another compile.
  CHECK(db->execute(insert, {i(3), bytes("cherry"), i(30)}).affected == 1);
  CHECK(db->compilations() == compiled + 3);
  // Parameters a plan the catalog still accepts does not take are refused
  // without a compile, however often they are sent: the runtime says the
  // plan fits (bc_ctx_check).
  for (int k = 0; k < 3; ++k) {
    CHECK_THROWS_AS(db->execute(insert, {i(9), bytes("x"), bytes("y")}), ParamError);
  }
  CHECK(db->compilations() == compiled + 3);
  // A DDL that leaves the plan compatible moves the version: the runtime is
  // asked, says the plan fits, and the parameters are refused uncompiled.
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  CHECK_THROWS_AS(db->execute(insert, {i(9), bytes("x"), bytes("y")}), ParamError);
  CHECK(db->compilations() == compiled + 3);
  CHECK(db->execute(insert, {i(9), bytes("x"), i(90)}).affected == 1);
  CHECK(db->compilations() == compiled + 3);

  // The parameters are checked against the recompiled plan: qty is now
  // bytes. Parameters of the new types are taken at once, though the stale
  // plan would have refused them; old-typed ones are refused before the run.
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BYTEA)");
  CHECK(db->execute(insert, {i(4), bytes("date"), bytes("forty")}).affected == 1);
  CHECK_THROWS_AS(db->execute(insert, {i(5), bytes("elder"), i(50)}), ParamError);
  (void)db->execute("DROP TABLE t");
  (void)db->execute(kCreateTable);
  CHECK_THROWS_AS(db->execute(insert, {i(6), bytes("fig"), bytes("sixty")}), ParamError);
  CHECK(db->execute(insert, {i(6), bytes("fig"), i(60)}).affected == 1);
  CHECK(db->execute(by_id, {i(6)}).rows == Grid{{i(6), i(60)}});
}

TEST_CASE("embedded API: parameters are checked against the plan that will run", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_stale_params"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k INTEGER PRIMARY KEY, v BIGINT)");
  const auto& by_k = db->prepare("SELECT v FROM t WHERE k = $1");
  CHECK(db->execute(by_k, {i(1)}).rows.empty());
  // The key is recreated as BIGINT: the parameter's physical type is the
  // same, so the cached plan's types fit, but its INTEGER range is the
  // dropped table's. The plan is verified, recompiled, and only then is
  // the parameter checked, against BIGINT.
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("INSERT INTO t VALUES ($1, $2)", {i(3'000'000'000), i(7)});
  CHECK(db->execute(by_k, {i(3'000'000'000)}).rows == Grid{{i(7)}});
}

TEST_CASE("embedded API: a cached plan is checked once per catalog version", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_plan_checks"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, v BIGINT)");
  const auto& by_k = db->prepare("SELECT v FROM t WHERE k = $1");
  (void)db->execute(by_k, {i(1)});
  CHECK(db->plan_checks_for_testing() == 0);  // compiled against the current version
  // A DDL on another table moves the version: the plan is checked once, still
  // fits, and is marked with that version; later runs are not checked.
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  auto compiled = db->compilations();
  for (int k = 0; k < 3; ++k) (void)db->execute(by_k, {i(1)});
  CHECK(db->plan_checks_for_testing() == 1);
  CHECK(db->compilations() == compiled);
  // Another DDL: one more check.
  (void)db->execute("DROP TABLE elsewhere");
  for (int k = 0; k < 3; ++k) (void)db->execute(by_k, {i(1)});
  CHECK(db->plan_checks_for_testing() == 2);
  CHECK(db->compilations() == compiled);
  // A plan marked as fitting is checked again after a DDL that breaks it,
  // recompiled, and runs on the new table.
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("INSERT INTO t VALUES ($1, $2)", {i(1), i(9)});
  CHECK(db->execute(by_k, {i(1)}).rows == Grid{{i(9)}});
  CHECK(db->plan_checks_for_testing() == 3);
  CHECK(db->compilations() == compiled + 2);  // the INSERT's first compile, and by_k's recompile
  CHECK(db->execute(by_k, {i(1)}).rows == Grid{{i(9)}});
  CHECK(db->plan_checks_for_testing() == 3);
}

TEST_CASE("embedded API: a DOUBLE read through an index keeps -0.0", "[sql][api]") {
  sql_test::TempDir dir{"api_negzero"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE dd (id BIGINT PRIMARY KEY, d DOUBLE PRECISION)");
  (void)db->execute("CREATE INDEX dd_d ON dd (d)");
  (void)db->execute("INSERT INTO dd VALUES ($1, $2)", {i(1), Value{-0.0}});
  auto rows = db->execute("SELECT d FROM dd WHERE d >= $1", {Value{-1.0}}).rows;
  REQUIRE(rows.size() == 1);
  CHECK(std::signbit(std::get<double>(rows[0][0])));
}

TEST_CASE("embedded API: compatibility decides which cached plans recompile", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_compat"};
  auto db = Database::open(dir.path());
  (void)db->execute(kCreateTable);
  (void)db->execute(kCreateIndex);
  const auto& insert = db->prepare(kInsert);
  const auto& by_id = db->prepare(kById);
  const auto& by_name = db->prepare(kByName);
  (void)db->execute(insert, {i(1), bytes("apple"), i(10)});
  (void)db->execute(by_id, {i(1)});
  (void)db->execute(by_name, {bytes("a"), bytes("b")});
  auto compiled = db->compilations();
  CHECK(compiled == 3);

  // ADD COLUMN invalidates nothing; rows the old INSERT writes take the default.
  (void)db->execute("ALTER TABLE t ADD COLUMN tag BIGINT DEFAULT 5");
  CHECK(db->execute(insert, {i(2), bytes("banana"), i(20)}).affected == 1);
  CHECK(db->execute(by_id, {i(2)}).rows == Grid{{i(2), i(20)}});
  CHECK(ids(db->execute(by_name, {bytes("a"), bytes("c")})) == std::vector<std::int64_t>{1, 2});
  CHECK(db->compilations() == compiled);
  // Row 1, written before the column, was rewritten with the default.
  CHECK(db->execute("SELECT id, tag FROM t WHERE id = $1", {i(1)}).rows == Grid{{i(1), i(5)}});
  CHECK(db->execute("SELECT id, tag FROM t WHERE id = $1", {i(2)}).rows == Grid{{i(2), i(5)}});
  compiled = db->compilations();

  // CREATE INDEX recompiles nothing: the write plans maintain it.
  (void)db->execute("CREATE INDEX t_by_qty ON t (qty)");
  (void)db->execute(by_id, {i(1)});
  (void)db->execute(by_name, {bytes("a"), bytes("b")});
  CHECK(db->execute(insert, {i(3), bytes("cherry"), i(30)}).affected == 1);
  CHECK(db->compilations() == compiled);
  CHECK(ids(db->execute("SELECT id FROM t WHERE qty >= $1", {i(15)})) == std::vector<std::int64_t>{2, 3});
  compiled = db->compilations();

  // DROP INDEX recompiles the plans that scan it, which then have no plan;
  // the write plans keep running, uncompiled.
  (void)db->execute("DROP INDEX t_by_name");
  CHECK_THROWS_AS(db->execute(by_name, {bytes("a"), bytes("b")}), ast::SqlError);
  // A failed recompile keeps the statement's old plan whole and still
  // incompatible: the next execute tries again and fails the same way,
  // never running the old code (ASan checks the schema ref it reads).
  CHECK_THROWS_AS(db->execute(by_name, {bytes("a"), bytes("b")}), ast::SqlError);
  CHECK(by_name.plan_source().find("IDX_3") != std::string::npos);
  CHECK(db->execute(insert, {i(4), bytes("date"), i(40)}).affected == 1);
  CHECK(db->execute(by_id, {i(4)}).rows == Grid{{i(4), i(40)}});
  CHECK(db->compilations() == compiled);

  // DROP TABLE: every plan on it is refused at recompile.
  (void)db->execute("DROP TABLE t");
  try {
    (void)db->execute(by_id, {i(1)});
    FAIL("a plan on a dropped table must be refused");
  } catch (const ast::SqlError& e) {
    CHECK(std::string{e.what()} == "relation \"t\" does not exist");
  }
  CHECK_THROWS_AS(db->execute(insert, {i(5), bytes("x"), i(1)}), ast::SqlError);
  (void)db->execute("DROP TABLE IF EXISTS t");
  CHECK_THROWS_AS(db->execute("DROP TABLE t"), DdlRefused);
}

TEST_CASE("embedded API: a unique index refuses duplicates, treats nulls as distinct, and moves with UPDATE",
          "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_unique"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE c (id BIGINT PRIMARY KEY, email BYTEA, w BIGINT)");
  (void)db->execute("CREATE UNIQUE INDEX c_email ON c (email)");
  auto ix_id = db->catalog()->index("c_email").second->id;
  constexpr auto kIns = "INSERT INTO c VALUES ($1, $2, $3)";
  constexpr auto kSetEmail = "UPDATE c SET email = $2 WHERE id = $1";
  constexpr auto kByEmail = "SELECT id FROM c WHERE email >= $1 AND email < $2";
  auto refused = [&](std::string_view sql, std::vector<Value> params) {
    try {
      (void)db->execute(sql, params);
    } catch (const ExecError& e) {
      CHECK(e.code() == BC_E_CONSTRAINT);
      CHECK(std::string{e.what()}.find("unique constraint of object " + std::to_string(ix_id)) != std::string::npos);
      return true;
    }
    return false;
  };
  CHECK(db->execute(kIns, {i(1), bytes("a"), i(0)}).affected == 1);
  CHECK(refused(kIns, {i(2), bytes("a"), i(0)}));
  CHECK(db->execute(kIns, {i(3), kNull, i(0)}).affected == 1);
  CHECK(db->execute(kIns, {i(4), kNull, i(0)}).affected == 1);  // nulls are distinct
  CHECK(db->execute("INSERT INTO c (id, w) VALUES ($1, $2)", {i(6), i(0)}).affected == 1);  // email defaults to NULL

  CHECK(db->execute(kSetEmail, {i(1), bytes("b")}).affected == 1);
  CHECK(db->execute(kIns, {i(5), bytes("a"), i(0)}).affected == 1);  // "a" is free again
  CHECK(refused(kSetEmail, {i(5), bytes("b")}));
  CHECK(db->execute(kSetEmail, {i(1), bytes("b")}).affected == 1);  // to its own value: no collision
  CHECK(db->execute(kSetEmail, {i(3), bytes("c")}).affected == 1);  // from NULL
  CHECK(db->execute(kSetEmail, {i(5), kNull}).affected == 1);       // to NULL
  CHECK(db->execute("UPDATE c SET w = $2 WHERE id = $1", {i(1), i(9)}).affected == 1);
  // The index holds one entry per row, at its current value.
  CHECK(ids(db->execute(kByEmail, {bytes("a"), bytes("z")})) == std::vector<std::int64_t>{1, 3});
  CHECK(db->execute(kIns, {i(7), bytes("a"), i(0)}).affected == 1);
  CHECK(ids(db->execute("SELECT id FROM c WHERE email = $1", {bytes("a")})) == std::vector<std::int64_t>{7});

  // A unique index over rows that already collide is refused as the
  // duplicate it would hold: BC_C_UNIQUE with the table's id (23505).
  (void)db->execute(kIns, {i(8), bytes("q"), i(1)});
  (void)db->execute(kIns, {i(9), bytes("r"), i(1)});
  auto table_id = db->catalog()->table("c")->id;
  try {
    (void)db->execute("CREATE UNIQUE INDEX c_w ON c (w)");
    FAIL("a unique index over duplicates was created");
  } catch (const ExecError& e) {
    REQUIRE(e.constraint());
    CHECK(e.constraint()->constraint == BC_C_UNIQUE);
    CHECK(e.constraint()->object_id == table_id);  // the index never existed: its id is the next DDL's
  }
  CHECK(db->catalog()->index("c_w").second == nullptr);
  db->close();

  auto again = Database::open(dir.path());
  CHECK(again->catalog()->index("c_email").second->unique);
  try {
    (void)again->execute(kIns, {i(10), bytes("b"), i(0)});
    FAIL("the unique index must survive a reopen");
  } catch (const ExecError& e) {
    CHECK(e.code() == BC_E_CONSTRAINT);
  }
}

TEST_CASE("embedded API: ranges on a DESC index part, nullable and not", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_desc"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE s (id BIGINT PRIMARY KEY, score BIGINT, rank BIGINT NOT NULL)");
  (void)db->execute("INSERT INTO s VALUES ($1, $2, $3)", {i(1), i(10), i(1)});
  (void)db->execute("INSERT INTO s VALUES ($1, $2, $3)", {i(2), i(20), i(2)});
  (void)db->execute("CREATE INDEX s_score ON s (score DESC)");  // backfilled
  (void)db->execute("CREATE INDEX s_rank ON s (rank DESC)");
  (void)db->execute("INSERT INTO s VALUES ($1, $2, $3)", {i(3), kNull, i(3)});
  (void)db->execute("INSERT INTO s VALUES ($1, $2, $3)", {i(4), i(30), i(4)});
  (void)db->execute("INSERT INTO s VALUES ($1, $2, $3)", {i(5), i(20), i(5)});
  auto q = [&](std::string_view where, std::vector<Value> p) {
    return ids(db->execute("SELECT id FROM s WHERE " + std::string{where}, p));
  };
  using V = std::vector<std::int64_t>;
  auto check_all = [&] {
    // Index order is descending score, nulls last; no range returns the null.
    CHECK(q("score >= $1", {i(20)}) == V{4, 2, 5});
    CHECK(q("score > $1", {i(20)}) == V{4});
    CHECK(q("score < $1", {i(30)}) == V{2, 5, 1});
    CHECK(q("score <= $1", {i(20)}) == V{2, 5, 1});
    CHECK(q("score >= $1 AND score < $2", {i(10), i(30)}) == V{2, 5, 1});
    CHECK(q("score > $1 AND score <= $2", {i(10), i(30)}) == V{4, 2, 5});
    CHECK(q("score = $1", {i(20)}) == V{2, 5});
    CHECK(q("score >= $1", {i(100)}).empty());
    CHECK(q("rank < $1", {i(3)}) == V{2, 1});
    CHECK(q("rank >= $1", {i(4)}) == V{5, 4});
    CHECK(q("rank > $1 AND rank <= $2", {i(1), i(3)}) == V{3, 2});
  };
  check_all();
  // DELETE and UPDATE keep the DESC entries.
  CHECK(db->execute("UPDATE s SET score = $2 WHERE id = $1", {i(1), i(25)}).affected == 1);
  CHECK(q("score < $1", {i(30)}) == V{1, 2, 5});
  CHECK(db->execute("UPDATE s SET score = $2 WHERE id = $1", {i(1), i(10)}).affected == 1);
  db->close();
  db = Database::open(dir.path());
  check_all();
}

TEST_CASE("embedded API: names that are not C identifiers work end to end", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_names"};
  auto db = Database::open(dir.path());
  (void)db->execute(R"(CREATE TABLE "a-b c" ("my id" BIGINT PRIMARY KEY, "x*/y" BYTEA, "1st" BIGINT))");
  (void)db->execute(R"(CREATE UNIQUE INDEX "the index" ON "a-b c" ("x*/y"))");
  constexpr auto kIns = R"(INSERT INTO "a-b c" VALUES ($1, $2, $3))";
  CHECK(db->execute(kIns, {i(1), bytes("p"), i(10)}).affected == 1);
  CHECK(db->execute(kIns, {i(2), bytes("q"), i(20)}).affected == 1);
  CHECK_THROWS_AS(db->execute(kIns, {i(3), bytes("q"), i(30)}), ExecError);
  // The header names the objects; in a // line "*/" is plain text.
  const auto& ins = db->prepare(kIns);
  CHECK(ins.plan_source().find("a-b c.x*/y") != std::string::npos);
  CHECK(ids(db->execute(R"(SELECT "my id" FROM "a-b c" WHERE "x*/y" >= $1 AND "x*/y" < $2)", {bytes("a"), bytes("z")})) ==
        std::vector<std::int64_t>{1, 2});
  CHECK(db->execute(R"(UPDATE "a-b c" SET "1st" = "1st" - $2 WHERE "my id" = $1)", {i(1), i(3)}).affected == 1);
  CHECK(db->execute(R"(SELECT "1st" FROM "a-b c" WHERE "my id" = $1)", {i(1)}).rows == Grid{{i(7)}});
  CHECK(db->execute(R"(DELETE FROM "a-b c" WHERE "my id" = $1)", {i(2)}).affected == 1);
  (void)db->execute(R"(DROP TABLE "a-b c")");
}

TEST_CASE("embedded API: a quoted name keeps its case, and nothing in a name escapes the plan's comment",
          "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_quoted"};
  auto db = Database::open(dir.path());
  (void)db->execute(R"(CREATE TABLE "Foo" (k BIGINT PRIMARY KEY, v BIGINT))");
  (void)db->execute("CREATE TABLE foo (k BIGINT PRIMARY KEY, v BIGINT)");  // another table, as in PostgreSQL
  (void)db->execute(R"(INSERT INTO "Foo" VALUES ($1, $2))", {i(1), i(10)});
  (void)db->execute("INSERT INTO foo VALUES ($1, $2)", {i(1), i(20)});
  CHECK(db->execute(R"(SELECT v FROM "Foo" WHERE k = $1)", {i(1)}).rows == Grid{{i(10)}});
  CHECK(db->execute("SELECT v FROM FOO WHERE k = $1", {i(1)}).rows == Grid{{i(20)}});  // unquoted folds to foo

  // Names that would end or splice a C comment: a backslash-newline before
  // "/", a "*/", a carriage return, "??/" (a backslash as a trigraph). Were
  // any of them to reach the lexer as more than comment text, the "#error"
  // line would be compiled and the plan would not build.
  const std::vector<std::string> evils{"*\\\n/\n#error injected\n/*", "x */\n#error injected\n",
                                       "cr\r\n#error injected", "t?" "?/\n#error injected", "back\\slash\\"};
  for (const auto& evil : evils) {
    INFO(evil);
    auto quoted = "\"" + evil + "\"";
    (void)db->execute("CREATE TABLE e (k BIGINT PRIMARY KEY, " + quoted + " BIGINT)");
    (void)db->execute("INSERT INTO e VALUES ($1, $2)", {i(1), i(5)});
    auto sel = "SELECT " + quoted + " FROM e WHERE k = $1";
    CHECK(db->execute(sel, {i(1)}).rows == Grid{{i(5)}});
    const auto& src = db->prepare(sel).plan_source();
    // Three header lines, each a // comment with no raw backslash, "?" or
    // carriage return: only \xNN escapes.
    auto header = src.substr(0, src.find("#include"));
    std::size_t lines = 0;
    for (std::size_t at = 0; at < header.size();) {
      auto end = header.find('\n', at);
      auto line = header.substr(at, end - at);
      CHECK(line.starts_with("// "));
      for (std::size_t k = line.find('\\'); k != std::string::npos; k = line.find('\\', k + 1)) {
        CHECK(line.compare(k, 2, "\\x") == 0);
      }
      CHECK(line.find('\r') == std::string::npos);
      CHECK(line.find('?') == std::string::npos);
      CHECK_FALSE(line.ends_with("\\"));
      ++lines;
      at = end + 1;
    }
    CHECK(lines == 3);
    CHECK(src.find("\n#error") == std::string::npos);
    (void)db->execute("DROP TABLE e");
  }
}

TEST_CASE("embedded API: table_rows reads a table in key order; errors carry their detail; calls after close() throw",
          "[sql][api]") {
  sql_test::TempDir dir{"api_table_rows"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (a BIGINT, b TEXT, c VARCHAR(4) NOT NULL DEFAULT 'x', PRIMARY KEY (a, b))");
  auto table_id = db->catalog()->table("t")->id;
  constexpr auto kIns = "INSERT INTO t (a, b) VALUES ($1, $2)";
  CHECK(db->execute(kIns, {i(2), bytes("a")}).affected == 1);
  CHECK(db->execute(kIns, {i(1), bytes("z")}).affected == 1);
  CHECK(db->execute("INSERT INTO t VALUES ($1, $2, $3)", {i(1), bytes("b"), bytes("yy")}).affected == 1);

  auto constraint_of = [&](std::string_view sql, std::vector<Value> params) -> std::uint32_t {
    try {
      (void)db->execute(sql, params);
    } catch (const ExecError& e) {
      auto c = e.constraint();
      REQUIRE(c);
      CHECK(e.code() == BC_E_CONSTRAINT);
      CHECK(c->object_id == table_id);
      CHECK(c->encode() == e.detail());
      return c->constraint;
    }
    FAIL("the statement was accepted");
    return 0;
  };
  CHECK(constraint_of(kIns, {i(2), bytes("a")}) == BC_C_UNIQUE);
  // A field wider than its bits would name another constraint or object.
  constexpr ConstraintDetail kWideId{BC_C_UNIQUE, ConstraintDetail::kMaxObjectId + 1};
  constexpr ConstraintDetail kWideKind{0x100, 1};
  constexpr ConstraintDetail kUnknownKind{99, 1};  // fits the byte, but bc_plan.h defines no such kind
  CHECK_THROWS_AS((void)kUnknownKind.encode(), std::out_of_range);
  CHECK_THROWS_AS((void)kWideId.encode(), std::out_of_range);
  CHECK_THROWS_AS((void)kWideKind.encode(), std::out_of_range);
  CHECK(constraint_of("INSERT INTO t VALUES ($1, $2, $3)", {i(3), bytes("a"), kNull}) == BC_C_NOT_NULL);

  auto check_rows = [&](Database& d) {
    auto r = d.table_rows("t");
    CHECK(r.columns == std::vector<std::string>{"a", "b", "c"});
    REQUIRE(r.types.size() == 3);
    CHECK(r.types[0].kind == SqlKind::BigInt);
    CHECK(r.types[2] == SqlType{SqlKind::Varchar, 4});
    // Primary-key order: (1, "b"), (1, "z"), (2, "a"); c from its default where unset.
    CHECK(r.rows == Grid{{i(1), bytes("b"), bytes("yy")}, {i(1), bytes("z"), bytes("x")}, {i(2), bytes("a"), bytes("x")}});
  };
  check_rows(*db);
  auto sel = db->execute("SELECT c, a FROM t WHERE a = $1 AND b = $2", {i(1), bytes("b")});
  CHECK(sel.types == std::vector<SqlType>{SqlType{SqlKind::Varchar, 4}, SqlType{SqlKind::BigInt}});
  CHECK_THROWS_AS(db->table_rows("nope"), ast::SqlError);
  db->close();
  // Every call after close() is refused, as the engine's are; close() again is not.
  CHECK_THROWS_AS(db->execute(kIns, {i(9), bytes("q")}), bytecask::DbClosed);
  CHECK_THROWS_AS(db->prepare("SELECT a FROM t WHERE a = $1 AND b = $2"), bytecask::DbClosed);
  CHECK_THROWS_AS(db->catalog(), bytecask::DbClosed);
  CHECK_THROWS_AS(db->table_rows("t"), bytecask::DbClosed);
  db->close();

  auto again = Database::open(dir.path());
  check_rows(*again);
  again->close();

  // A stored row of another layout generation is refused, never decoded
  // with the current layout's slots: one row's generation field (its first
  // two bytes, little-endian) is rewritten through the engine.
  {
    auto engine = bytecask::DB::open(dir.path());
    auto prefix = row_prefix(table_id);
    Bytes key, row;
    for (const auto& e : engine.iter_from({}, prefix)) {
      key.assign(e.key.begin(), e.key.end());
      row.assign(e.value.begin(), e.value.end());
      break;
    }
    REQUIRE(has_prefix(key, prefix));
    row[0] = std::byte{2};
    engine.put({}, key, row);
    engine.close();
  }
  auto third = Database::open(dir.path());
  try {
    (void)third->table_rows("t");
    FAIL("a row of another layout generation was decoded");
  } catch (const ExecError& e) {
    CHECK(e.code() == BC_E_CORRUPT);
    CHECK(std::string{e.what()}.starts_with("row of layout generation 2"));
  }

  // A present bit for a column the row predates (ADD COLUMN after it was
  // written) points outside the row, where the column would read as 0: a
  // damaged row, refused, never read as a value.
  (void)third->execute("CREATE TABLE p (k BIGINT PRIMARY KEY, a BIGINT)");
  CHECK(third->execute("INSERT INTO p VALUES ($1, $2)", {i(1), i(5)}).affected == 1);
  (void)third->execute("ALTER TABLE p ADD COLUMN c BIGINT");
  auto third_cat = third->catalog();  // held: p_table points into it
  const auto* p_table = third_cat->table("p");
  auto p_id = p_table->id;
  auto c_id = p_table->column("c")->id;
  CHECK(third->table_rows("p").rows == Grid{{i(1), i(5), kNull}});
  third->close();
  {
    auto engine = bytecask::DB::open(dir.path());
    auto prefix = row_prefix(p_id);
    Bytes key, row;
    for (const auto& e : engine.iter_from({}, prefix)) {
      key.assign(e.key.begin(), e.key.end());
      row.assign(e.value.begin(), e.value.end());
      break;
    }
    REQUIRE(has_prefix(key, prefix));
    row[kRowHeaderBytes + c_id / 8] |= std::byte{1} << (c_id % 8);  // c's present bit
    engine.put({}, key, row);
    engine.close();
  }
  auto fourth = Database::open(dir.path());
  try {
    (void)fourth->table_rows("p");
    FAIL("a present column outside its row was read");
  } catch (const ExecError& e) {
    CHECK(e.code() == BC_E_CORRUPT);
  }
  fourth->close();
}

TEST_CASE("embedded API: values outside their declared type, an omitted NOT NULL column, one PRIMARY KEY",
          "[sql][api]") {
  sql_test::TempDir dir{"api_ranges"};
  auto db = Database::open(dir.path());
  (void)db->execute(
      "CREATE TABLE r (k BIGINT PRIMARY KEY, s SMALLINT, n INTEGER, v VARCHAR(3), f REAL, d NUMERIC(4, 2), "
      "m BIGINT NOT NULL DEFAULT 0, z BIGINT NOT NULL)");
  auto table_id = db->catalog()->table("r")->id;
  constexpr auto kIns = "INSERT INTO r (k, s, n, v, f, d, z) VALUES ($1, $2, $3, $4, $5, $6, $7)";
  auto row = [&](std::int64_t k) { return std::vector<Value>{i(k), kNull, kNull, kNull, kNull, kNull, i(0)}; };
  auto refusal = [&](std::string_view sql, const std::vector<Value>& params) -> std::pair<std::uint32_t, std::string> {
    try {
      (void)db->execute(sql, params);
    } catch (const ExecError& e) {
      auto c = e.constraint();
      REQUIRE(c);
      CHECK(c->object_id == table_id);
      CHECK_FALSE(e.conflict());
      return {c->constraint, e.what()};
    }
    FAIL("accepted: " << sql);
    return {};
  };
  auto with = [&](std::size_t at, Value v) {
    auto p = row(1);
    p[at] = std::move(v);
    return p;
  };

  // Each declared range, at its edge: the edge is written, one past it refused with BC_C_RANGE.
  CHECK(refusal(kIns, with(1, i(32768))) == std::pair<std::uint32_t, std::string>{BC_C_RANGE,
        "smallint out of range for column \"s\" of table \"r\""});
  CHECK(refusal(kIns, with(1, i(-32769))).first == BC_C_RANGE);
  CHECK(refusal(kIns, with(2, i(std::int64_t{1} << 31))).second.starts_with("integer out of range"));
  CHECK(refusal(kIns, with(3, bytes("abcd"))).second.starts_with("value too long for type character varying(3)"));
  CHECK(refusal(kIns, with(4, Value{1e39})).second.starts_with("value out of range: overflow"));
  CHECK(refusal(kIns, with(4, Value{1e-50})).second.starts_with("value out of range: underflow"));
  CHECK(refusal(kIns, with(5, i(10000))).second.starts_with("numeric field overflow"));  // 100.00 in NUMERIC(4, 2)
  CHECK(refusal(kIns, with(5, i(-10000))).first == BC_C_RANGE);
  CHECK(db->execute(kIns, {i(1), i(32767), i((std::int64_t{1} << 31) - 1), bytes("\xC3\xA9\xC3\xA9\xC3\xA9"),
                           Value{3.0e38}, i(9999), i(0)})
            .affected == 1);  // VARCHAR(3) counts code points: "ééé" is 6 bytes
  CHECK(db->execute(kIns, {i(2), i(-32768), i(-(std::int64_t{1} << 31)), bytes("abc"), Value{0.0}, i(-9999), i(0)})
            .affected == 1);
  // REAL overflow is PostgreSQL's: a value refused when its float rounding is
  // infinite. 2^128 - 2^103 is the first that rounds up; the double just
  // below it rounds to FLT_MAX.
  constexpr auto kFirstInf = 0x1p128 - 0x1p103;
  CHECK(refusal(kIns, with(4, Value{kFirstInf})).second.starts_with("value out of range: overflow"));
  CHECK(refusal(kIns, with(4, Value{-kFirstInf})).first == BC_C_RANGE);
  CHECK(db->execute(kIns, {i(5), kNull, kNull, kNull, Value{std::nextafter(kFirstInf, 0.0)}, kNull, i(0)}).affected == 1);
  // A REAL is stored as its float rounding, as PostgreSQL stores a float4:
  // the double below 2^128 - 2^103 as FLT_MAX, and 0.1 as 0.100000001490116…
  CHECK(db->execute(kIns, {i(6), kNull, kNull, kNull, Value{0.1}, kNull, i(0)}).affected == 1);
  CHECK(db->execute(kIns, {i(7), kNull, kNull, kNull, Value{-std::nextafter(kFirstInf, 0.0)}, kNull, i(0)}).affected == 1);
  auto real_of = [&](std::int64_t k) {
    auto r = db->execute("SELECT f FROM r WHERE k = $1", {i(k)});
    REQUIRE(r.rows.size() == 1);
    return std::get<double>(r.rows[0][0]);
  };
  CHECK(real_of(5) == static_cast<double>(std::numeric_limits<float>::max()));
  CHECK(real_of(7) == -static_cast<double>(std::numeric_limits<float>::max()));
  CHECK(real_of(6) == static_cast<double>(0.1f));
  CHECK(real_of(6) != 0.1);
  // A REAL key or index part is found by the value it was written with: a
  // lookup, a bound, and an UPDATE or DELETE by key round a REAL as the row
  // does, as PostgreSQL rounds a $1 typed real.
  (void)db->execute("CREATE TABLE rk (r REAL PRIMARY KEY, x REAL, n BIGINT)");
  (void)db->execute("CREATE INDEX rk_x ON rk (x)");
  CHECK(db->execute("INSERT INTO rk VALUES ($1, $2, $3)", {Value{0.1}, Value{0.1}, i(1)}).affected == 1);
  CHECK(db->execute("INSERT INTO rk VALUES ($1, $2, $3)", {Value{0.5}, Value{0.5}, i(2)}).affected == 1);
  CHECK(db->execute("SELECT n FROM rk WHERE r = $1", {Value{0.1}}).rows == Grid{{i(1)}});
  CHECK(db->execute("UPDATE rk SET n = $2 WHERE r = $1", {Value{0.1}, i(10)}).affected == 1);
  CHECK(db->execute("SELECT n FROM rk WHERE x <= $1", {Value{0.1}}).rows == Grid{{i(10)}});  // 0.1 rounded: included
  CHECK(db->execute("SELECT n FROM rk WHERE x > $1", {Value{0.1}}).rows == Grid{{i(2)}});    // and excluded here
  // The same REAL again is a duplicate key, whichever double rounds to it.
  for (auto again : {0.1, static_cast<double>(0.1f)}) {
    try {
      (void)db->execute("INSERT INTO rk VALUES ($1, $2, $3)", {Value{again}, kNull, i(3)});
      FAIL("a duplicate REAL key was inserted");
    } catch (const ExecError& e) {
      REQUIRE(e.constraint());
      CHECK(e.constraint()->constraint == BC_C_UNIQUE);
    }
  }
  CHECK(db->execute("DELETE FROM rk WHERE r = $1", {Value{0.1}}).affected == 1);
  CHECK(db->table_rows("rk").rows.size() == 1);

  // A key value, a lookup's or a bound's, is checked against its column's
  // type as PostgreSQL checks a typed $n: outside it is BC_C_RANGE, never
  // rounded to a neighbour or wrapped. 1e-50 would round to the 0.0 row and
  // 1e39 to the infinite one.
  CHECK(db->execute("INSERT INTO rk VALUES ($1, $2, $3)", {Value{0.0}, Value{0.0}, i(4)}).affected == 1);
  constexpr auto kInf = std::numeric_limits<double>::infinity();
  CHECK(db->execute("INSERT INTO rk VALUES ($1, $2, $3)", {Value{kInf}, Value{kInf}, i(5)}).affected == 1);
  auto key_refusal = [&](std::string_view sql, const std::vector<Value>& params) -> std::string {
    try {
      (void)db->execute(sql, params);
    } catch (const ExecError& e) {
      REQUIRE(e.constraint());
      CHECK(e.constraint()->constraint == BC_C_RANGE);
      return e.what();
    }
    FAIL("accepted: " << sql);
    return {};
  };
  CHECK(key_refusal("SELECT n FROM rk WHERE r = $1", {Value{1e-50}}).starts_with("value out of range: underflow"));
  CHECK(key_refusal("SELECT n FROM rk WHERE r = $1", {Value{1e39}}).starts_with("value out of range: overflow"));
  CHECK(key_refusal("SELECT n FROM rk WHERE x < $1", {Value{1e-50}}).starts_with("value out of range: underflow"));
  CHECK(key_refusal("DELETE FROM rk WHERE r = $1", {Value{-1e39}}).starts_with("value out of range: overflow"));
  CHECK(db->execute("SELECT n FROM rk WHERE r = $1", {Value{kInf}}).rows == Grid{{i(5)}});
  CHECK(db->execute("SELECT n FROM rk WHERE x < $1", {Value{0.25}}).rows == Grid{{i(4)}});
  // An INTEGER key and bound: 3000000000 is not an INTEGER (22003), not "no row".
  (void)db->execute("CREATE TABLE ik (k INTEGER PRIMARY KEY, n INTEGER, v VARCHAR(2))");
  (void)db->execute("CREATE INDEX ik_n ON ik (n)");
  (void)db->execute("CREATE INDEX ik_v ON ik (v)");
  CHECK(db->execute("INSERT INTO ik VALUES ($1, $2, $3)", {i(1), i(1), bytes("ab")}).affected == 1);
  CHECK(key_refusal("SELECT k FROM ik WHERE n < $1", {i(3'000'000'000)}).starts_with("integer out of range"));
  CHECK(key_refusal("SELECT n FROM ik WHERE k = $1", {i(-3'000'000'000)}).starts_with("integer out of range"));
  // A modifier is not a type's range: a VARCHAR(2) key compared with a longer
  // value finds nothing, as a $n typed varchar does; text that is not UTF-8
  // is refused (22021).
  CHECK(db->execute("SELECT k FROM ik WHERE v = $1", {bytes("abc")}).rows.empty());
  CHECK(key_refusal("SELECT k FROM ik WHERE v = $1", {bytes("\xFF")}).starts_with("invalid byte sequence"));

  // A REAL DEFAULT is stored the same way.
  (void)db->execute("CREATE TABLE rd (k BIGINT PRIMARY KEY, f REAL DEFAULT 0.1)");
  CHECK(db->execute("INSERT INTO rd (k) VALUES ($1)", {i(1)}).affected == 1);
  CHECK(db->table_rows("rd").rows.at(0).at(1) == Value{static_cast<double>(0.1f)});

  // DATE, TIMESTAMP and TIME: PostgreSQL's ranges, in days and microseconds
  // from 1970. TIME includes 24:00:00.
  (void)db->execute("CREATE TABLE tm (k BIGINT PRIMARY KEY, t TIME, d DATE, ts TIMESTAMP)");
  auto tm_id = db->catalog()->table("tm")->id;
  constexpr auto kInsTm = "INSERT INTO tm VALUES ($1, $2, $3, $4)";
  constexpr std::int64_t kDay = 86'400'000'000;
  constexpr std::int64_t kMinDate = -2'440'588;      // 4714-11-24 BC
  constexpr std::int64_t kEndDate = 2'145'042'906;   // 5874898-01-01, the first day past the range
  constexpr std::int64_t kMinTs = -210'866'803'200'000'000;
  CHECK(db->execute(kInsTm, {i(1), i(kDay), i(kMinDate), i(kMinTs)}).affected == 1);
  CHECK(db->execute(kInsTm, {i(2), i(0), i(kEndDate - 1), i(std::numeric_limits<std::int64_t>::max())}).affected == 1);
  auto tm_refusal = [&](std::vector<Value> p) -> std::string {
    try {
      (void)db->execute(kInsTm, p);
    } catch (const ExecError& e) {
      REQUIRE(e.constraint());
      CHECK(e.constraint()->constraint == BC_C_RANGE);
      CHECK(e.constraint()->object_id == tm_id);
      return e.what();
    }
    FAIL("accepted");
    return {};
  };
  CHECK(tm_refusal({i(3), i(kDay + 1), kNull, kNull}).starts_with("time out of range"));
  CHECK(tm_refusal({i(3), i(-1), kNull, kNull}).starts_with("time out of range"));
  CHECK(tm_refusal({i(3), kNull, i(kMinDate - 1), kNull}).starts_with("date out of range"));
  CHECK(tm_refusal({i(3), kNull, i(kEndDate), kNull}).starts_with("date out of range"));
  CHECK(tm_refusal({i(3), kNull, kNull, i(kMinTs - 1)}).starts_with("timestamp out of range"));
  // TEXT and VARCHAR are UTF-8, checked on write, and VARCHAR(n) counts code
  // points in the same pass: invalid UTF-8 is BC_C_RANGE ("invalid byte
  // sequence", 22021), never counted into a length.
  (void)db->execute("CREATE TABLE tx (k BIGINT PRIMARY KEY, t TEXT, v VARCHAR(3))");
  constexpr auto kInsTx = "INSERT INTO tx VALUES ($1, $2, $3)";
  auto raw = [](std::string_view s) { return bytes(s); };
  std::string continuation_run = "\xC3";
  continuation_run.append(1000, '\x80');  // one lead byte, then stray continuations
  for (const auto& bad : {continuation_run, std::string{"\xFF"}, std::string{"a\0b", 3}, std::string{"\xC0\x80"},
                          std::string{"\xED\xA0\x80"}, std::string{"\xF4\x90\x80\x80"}, std::string{"\xE2\x82"}}) {
    for (std::size_t at : {1u, 2u}) {
      std::vector<Value> p{i(1), kNull, kNull};
      p[at] = raw(bad);
      try {
        (void)db->execute(kInsTx, p);
        FAIL("invalid UTF-8 was written");
      } catch (const ExecError& e) {
        REQUIRE(e.constraint());
        CHECK(e.constraint()->constraint == BC_C_RANGE);
        CHECK(std::string{e.what()}.starts_with("invalid byte sequence for encoding \"UTF8\""));
      }
    }
  }
  // A four-byte code point is one: three of them fit VARCHAR(3).
  CHECK(db->execute(kInsTx, {i(1), raw("\xF0\x9F\x98\x80"), raw("\xF0\x9F\x98\x80\xC3\xA9z")}).affected == 1);
  CHECK(db->table_rows("tx").rows.size() == 1);

  // UPDATE checks the value it computes, as INSERT does the one it is given.
  CHECK(refusal("UPDATE r SET n = n - $2 WHERE k = $1", {i(1), i(-1)}).second.starts_with("integer out of range"));
  CHECK(refusal("UPDATE r SET s = $2 WHERE k = $1", {i(2), i(-40000)}).first == BC_C_RANGE);

  // A NOT NULL column left out with no default is refused when the row is
  // built, as a NULL given for it is: a primary key column among them.
  CHECK(refusal("INSERT INTO r (k) VALUES ($1)", {i(3)}) ==
        std::pair<std::uint32_t, std::string>{BC_C_NOT_NULL,
                                              "null value in column \"z\" of table \"r\" violates its not-null "
                                              "constraint"});
  CHECK(refusal("INSERT INTO r (z) VALUES ($1)", {i(3)}).second.starts_with("null value in column \"k\""));
  CHECK(db->execute("INSERT INTO r (k, z) VALUES ($1, $2)", {i(3), i(7)}).affected == 1);  // m takes its default
  // A row with both is BC_C_RANGE, as in PostgreSQL, whose type checks come
  // first: NOT NULL is checked when the row ends.
  CHECK(refusal(kIns, {i(4), kNull, i(std::int64_t{1} << 40), kNull, kNull, kNull, kNull}).first == BC_C_RANGE);
  CHECK(refusal(kIns, {kNull, i(1), i(1), bytes("abcd"), kNull, kNull, i(0)}).first == BC_C_RANGE);

  // Nothing refused was written.
  auto rows = db->table_rows("r");
  REQUIRE(rows.rows.size() == 6);
  CHECK(rows.rows[0][1] == i(32767));
  CHECK(rows.rows[1][2] == i(-(std::int64_t{1} << 31)));
  CHECK(rows.rows[2][6] == i(0));

  // One PRIMARY KEY a table; two are refused, not merged into a composite key.
  CHECK_THROWS_AS(db->execute("CREATE TABLE two (a BIGINT PRIMARY KEY, b BIGINT PRIMARY KEY)"), ast::SqlError);
  CHECK_THROWS_AS(db->execute("CREATE TABLE two (a BIGINT PRIMARY KEY, b BIGINT, PRIMARY KEY (b))"), ast::SqlError);
  CHECK(db->catalog()->table("two") == nullptr);

  // A statement that conflicts on every retry is told from an error with the
  // same code (BC_CONFLICT and BC_E_IO are both 1), end to end: every commit
  // is made to conflict, and nothing is written.
  static_assert(static_cast<int>(BC_CONFLICT) == static_cast<int>(BC_E_IO));
  // A conflict is made only by conflicted(): BC_CONFLICT is not a bc_error_code.
  static_assert(!std::is_constructible_v<ExecError, std::string, decltype(BC_CONFLICT)>);
  static_assert(std::is_constructible_v<ExecError, std::string, decltype(BC_E_IO)>);
  constexpr auto kInsK = "INSERT INTO r (k, z) VALUES ($1, $2)";
  db->conflict_on_every_commit_for_testing(true);
  try {
    (void)db->execute(kInsK, {i(9), i(0)});
    FAIL("a statement conflicting on every retry committed");
  } catch (const ExecError& e) {
    CHECK(e.conflict());
    CHECK(e.code() == BC_CONFLICT);
    CHECK_FALSE(e.constraint());
  }
  db->conflict_on_every_commit_for_testing(false);
  CHECK(db->table_rows("r").rows.size() == 6);
  CHECK(db->execute(kInsK, {i(9), i(0)}).affected == 1);
  CHECK_FALSE(ExecError("I/O", BC_E_IO).conflict());
  db->close();
}
