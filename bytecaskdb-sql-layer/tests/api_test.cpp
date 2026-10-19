// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The embedded API end to end: the seven statements of the prototype, in
// order, from SQL text to rows, then a fresh open of the same directory to
// show the catalog and the data persist.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
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

  // A unique index over rows that already collide is refused.
  (void)db->execute(kIns, {i(8), bytes("q"), i(1)});
  (void)db->execute(kIns, {i(9), bytes("r"), i(1)});
  CHECK_THROWS_AS(db->execute("CREATE UNIQUE INDEX c_w ON c (w)"), DdlRefused);
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

TEST_CASE("embedded API: table_rows reads a table in key order; errors carry their detail and rows their types",
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
  CHECK_THROWS_AS(db->table_rows("nope"), std::invalid_argument);
  db->close();
  // Every call after close() is refused, as the engine's are; close() again is not.
  CHECK_THROWS_AS(db->execute(kIns, {i(9), bytes("q")}), bytecask::DbClosed);
  CHECK_THROWS_AS(db->prepare("SELECT a FROM t WHERE a = $1 AND b = $2"), bytecask::DbClosed);
  CHECK_THROWS_AS(db->catalog(), bytecask::DbClosed);
  CHECK_THROWS_AS(db->table_rows("t"), bytecask::DbClosed);
  db->close();

  auto again = Database::open(dir.path());
  check_rows(*again);
}
