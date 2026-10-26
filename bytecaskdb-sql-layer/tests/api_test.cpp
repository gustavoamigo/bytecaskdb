// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The embedded API end to end: the seven statements of the prototype, in
// order, from SQL text to rows, then a fresh open of the same directory to
// show the catalog and the data persist.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <thread>
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

// What a statement is refused with before it runs, or "" if it runs: an
// ast::SqlError, or a FeatureNotSupported (0A000), the planner's "no plan:"
// among them.
template <typename Stmt>
auto sql_refusal(Database& db, const Stmt& stmt, const std::vector<Value>& params) -> std::string {
  try {
    (void)db.execute(stmt, params);
  } catch (const ast::SqlError& e) {
    return e.what();
  } catch (const FeatureNotSupported& e) {
    return e.what();
  }
  return "";
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
  // Of an UPDATE's parameters out of range, a SET value's is reported
  // first, as PostgreSQL folds the target list before the WHERE.
  (void)db->execute("CREATE TABLE u (s VARCHAR(4) NOT NULL, n SMALLINT PRIMARY KEY)");
  try {
    (void)db->execute("UPDATE u SET s = $2 WHERE n = $1", {i(32768), Value{sql_test::bytes("00000")}});
    FAIL("ran");
  } catch (const ExecError& e) {
    CHECK(sqlstate(e) == "22001");
  }
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

TEST_CASE("embedded API: a parameter compared with DECIMALs of two scales is refused", "[sql][api]") {
  sql_test::TempDir dir{"api_decimal_scales"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE d (a NUMERIC(10, 2), b NUMERIC(10, 4), n NUMERIC(10, 0), j BIGINT, "
                    "PRIMARY KEY (a, b, n, j))");
  // One scaled integer would be 1.00 for a and 0.0100 for b: refused before
  // it runs, whatever its value, a SQL error and not a range refusal.
  CHECK(sql_refusal(*db, "SELECT a FROM d WHERE a = $1 AND b = $1 AND n = $2 AND j = $3", {i(100), i(1), i(1)}) ==
        "parameter $1 is compared with a (numeric(10,2)) and b (numeric(10,4))");
  // So is a DECIMAL of scale 2 and an integer.
  CHECK(sql_refusal(*db, "SELECT a FROM d WHERE a = $1 AND b = $2 AND n = $3 AND j = $1", {i(100), i(1), i(1)}) ==
        "parameter $1 is compared with a (numeric(10,2)) and j (bigint)");
  // At scale 0 a DECIMAL's integer is the number: NUMERIC(10, 0) and BIGINT share one.
  CHECK(db->execute("INSERT INTO d VALUES ($1, $2, $3, $4)", {i(100), i(10000), i(7), i(7)}).affected == 1);
  CHECK(db->execute("SELECT a FROM d WHERE a = $1 AND b = $2 AND n = $3 AND j = $3", {i(100), i(10000), i(7)})
            .rows.size() == 1);
}

TEST_CASE("embedded API: a parameter a recompile finds compared with two types is refused", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_recompile_two_types"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (a BIGINT, b BIGINT, PRIMARY KEY (a, b))");
  const auto& both = db->prepare("SELECT a FROM t WHERE a = $1 AND b = $1");
  CHECK(db->execute(both, {i(1)}).rows.empty());
  // b is recreated as INTEGER, still an i64: the cached plan is recompiled,
  // and the recompile at execute finds $1 compared with two SQL types.
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (a BIGINT, b INTEGER, PRIMARY KEY (a, b))");
  CHECK(sql_refusal(*db, both, {i(1)}) == "parameter $1 is compared with a (bigint) and b (integer)");
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

  // DROP INDEX recompiles the plans that scan it: by name, a full scan now.
  (void)db->execute("DROP INDEX t_by_name");
  CHECK(ids(db->execute(by_name, {bytes("a"), bytes("b")})) == std::vector<std::int64_t>{1});
  CHECK(by_name.plan_source().find("IDX_3") == std::string::npos);
  // One that then has no plan, a filter on a DOUBLE only its index took, is
  // refused; the write plans keep running, uncompiled.
  (void)db->execute("ALTER TABLE t ADD COLUMN d DOUBLE PRECISION DEFAULT 1.5");
  (void)db->execute("CREATE INDEX t_by_d ON t (d)");
  const auto& by_d = db->prepare("SELECT id FROM t WHERE d >= $1");
  CHECK(ids(db->execute(by_d, {Value{1.0}})) == std::vector<std::int64_t>{1, 2, 3});
  const auto by_d_index = "IDX_" + std::to_string(db->catalog()->index("t_by_d").second->id);
  compiled = db->compilations();
  (void)db->execute("DROP INDEX t_by_d");
  CHECK_THROWS_AS(db->execute(by_d, {Value{1.0}}), FeatureNotSupported);
  // A failed recompile keeps the statement's old plan whole and still
  // incompatible: the next execute tries again and fails the same way,
  // never running the old code (ASan checks the schema ref it reads).
  CHECK_THROWS_AS(db->execute(by_d, {Value{1.0}}), FeatureNotSupported);
  CHECK(by_d.plan_source().find(by_d_index) != std::string::npos);
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

TEST_CASE("embedded API: a table without a primary key keeps every row, duplicates included, under hidden row ids",
          "[sql][api]") {
  sql_test::TempDir dir{"api_row_id"};
  auto rows_of = [](Database& d) {
    auto r = d.table_rows("k");
    CHECK(r.columns == std::vector<std::string>{"a", "b"});  // never the row id
    return r.rows;
  };
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE k (a INTEGER, b TEXT)");
    const auto& ins = db->prepare("INSERT INTO k VALUES ($1, $2)");
    CHECK(ins.param_types() == std::vector<PhysType>{PhysType::I64, PhysType::Bytes});  // the row id is no parameter
    CHECK(db->execute(ins, {i(1), bytes("x")}).affected == 1);
    CHECK(db->execute(ins, {i(1), bytes("x")}).affected == 1);  // a duplicate is another row
    CHECK(db->execute("INSERT INTO k (b) VALUES ($1)", {bytes("b only")}).affected == 1);
    CHECK(sql_refusal(*db, "INSERT INTO k VALUES ($1, $2, $3)", {i(1), bytes("x"), i(3)}) ==
          "INSERT has more expressions than target columns");
    // Rows come back in the order they were inserted: the row id's.
    CHECK(rows_of(*db) == Grid{{i(1), bytes("x")}, {i(1), bytes("x")}, {kNull, bytes("b only")}});

    // A rolled-back insert leaves a gap, never a row.
    {
      auto t = db->begin();
      (void)t.execute(ins, {i(9), bytes("gone")});
      t.rollback();
    }
    // Two transactions inserting into the table both commit: their row ids
    // differ, so no write conflicts.
    auto t1 = db->begin();
    auto t2 = db->begin();
    (void)t1.execute(ins, {i(2), bytes("t1")});
    (void)t2.execute(ins, {i(2), bytes("t2")});
    CHECK(t1.commit() == CommitOutcome::Committed);
    CHECK(t2.commit() == CommitOutcome::Committed);

    // Concurrent autocommit inserts all land.
    {
      std::vector<std::jthread> threads;
      for (int w = 0; w < 4; ++w) {
        threads.emplace_back([&, w] {
          for (int n = 0; n < 50; ++n) (void)db->execute(ins, {i(100 + w), bytes("c")});
        });
      }
    }
    CHECK(db->execute("SELECT COUNT(*) FROM k WHERE b = $1", {bytes("c")}).rows == Grid{{i(200)}});

    CHECK(db->execute("UPDATE k SET b = $1 WHERE a = $2", {bytes("y"), i(1)}).affected == 2);
    CHECK(db->execute("DELETE FROM k WHERE a >= $1", {i(100)}).affected == 200);
    // No primary key is grouped, so no column depends on the group.
    try {
      (void)db->execute("SELECT a, b FROM k GROUP BY a");
      FAIL("an ungrouped column was accepted");
    } catch (const ast::SqlError& e) {
      CHECK(e.sqlstate() == "42803");
    }
    CHECK(db->execute("SELECT a, COUNT(*) FROM k GROUP BY a ORDER BY a").rows ==
          Grid{{i(1), i(2)}, {i(2), i(2)}, {kNull, i(1)}});
    // A unique index on a column holds as on any table.
    (void)db->execute("CREATE TABLE u (v INTEGER)");
    (void)db->execute("CREATE UNIQUE INDEX u_v ON u (v)");
    CHECK(db->execute("INSERT INTO u VALUES ($1)", {i(1)}).affected == 1);
    CHECK_THROWS_AS(db->execute("INSERT INTO u VALUES ($1)", {i(1)}), ExecError);
  }
  // Reopened: the rows are all there, and the next row id is past them.
  auto db = Database::open(dir.path());
  CHECK(db->execute("INSERT INTO k VALUES ($1, $2)", {i(3), bytes("after")}).affected == 1);
  CHECK(rows_of(*db) == Grid{{i(1), bytes("y")}, {i(1), bytes("y")}, {kNull, bytes("b only")}, {i(2), bytes("t1")},
                             {i(2), bytes("t2")}, {i(3), bytes("after")}});
}

TEST_CASE("embedded API: literals are bound after the caller's parameters, and a recompile folds them again",
          "[sql][api]") {
  sql_test::TempDir dir{"api_literals"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE l (k INTEGER PRIMARY KEY, d NUMERIC(6,2), t TEXT)");
  const auto& ins = db->prepare("INSERT INTO l VALUES ($1, 1.25, 'one')");
  // The constants are no parameter a caller binds or is told of.
  CHECK(ins.param_types() == std::vector<PhysType>{PhysType::I64});
  CHECK(ins.params().size() == 1);
  CHECK(db->execute(ins, {i(1)}).affected == 1);
  CHECK(db->execute(ins, {i(2)}).affected == 1);
  try {
    (void)db->execute(ins, {i(3), i(4)});
    FAIL("an extra parameter was taken");
  } catch (const ParamError& e) {
    CHECK(std::string{e.what()} == "statement takes 1 parameters, got 2");
  }
  CHECK(db->execute("SELECT k, d, t FROM l WHERE t = 'one' AND k >= 2").rows == Grid{{i(2), i(125), bytes("one")}});
  CHECK(db->execute("SELECT k, 'x', 2.5 FROM l WHERE k = 1").rows == Grid{{i(1), bytes("x"), i(25)}});

  // Recompiled after a DDL, a literal is folded to the type it meets now.
  const auto& by_d = db->prepare("SELECT k FROM l WHERE d = 1.25");
  CHECK(db->execute(by_d, {}).rows.size() == 2);
  (void)db->execute("DROP TABLE l");
  (void)db->execute("CREATE TABLE l (k INTEGER PRIMARY KEY, d NUMERIC(6,3), t TEXT)");
  CHECK(db->execute(ins, {i(1)}).affected == 1);
  CHECK(db->table_rows("l").rows == Grid{{i(1), i(1250), bytes("one")}});
  CHECK(db->execute(by_d, {}).rows == Grid{{i(1)}});
  (void)db->execute("DROP TABLE l");
  (void)db->execute("CREATE TABLE l (k INTEGER PRIMARY KEY, d NUMERIC(6,1), t TEXT)");
  // At scale 1, 1.25 is no value d holds: refused, as on a first compile.
  CHECK(sql_refusal(*db, by_d, {}) == "comparing d (numeric(6,1)) with a constant its type does not hold exactly is "
                                      "not supported yet");

  // A SELECT without FROM: one row of its constants, or none under LIMIT 0.
  auto one = db->execute("SELECT 5, -7.80, 'a', NULL, TRUE, 1 + 1.5");
  CHECK(one.columns == std::vector<std::string>(6, "?column?"));
  CHECK(one.rows == Grid{{i(5), i(-780), bytes("a"), kNull, Value{true}, i(25)}});
  CHECK(one.types == std::vector<SqlType>{{SqlKind::Integer}, {SqlKind::Decimal, 0, 0, 2}, {SqlKind::Text},
                                          {SqlKind::Text}, {SqlKind::Boolean}, {SqlKind::Decimal, 0, 0, 1}});
  CHECK(db->execute("SELECT 1 LIMIT 0").rows.empty());
  CHECK(db->execute("SELECT 1 LIMIT 1").rows == Grid{{i(1)}});
  // And in a transaction, beside its writes.
  auto t = db->begin();
  CHECK(t.execute("INSERT INTO l VALUES (9, 0.5, 'nine')").affected == 1);
  CHECK(t.execute("SELECT k FROM l WHERE t = 'nine'").rows == Grid{{i(9)}});
  CHECK(t.execute("SELECT 'in a transaction'").rows == Grid{{bytes("in a transaction")}});
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(db->execute("SELECT d FROM l WHERE k = 9").rows == Grid{{i(5)}});
}

TEST_CASE("embedded API: a stored column that does not decode is BC_E_CORRUPT, never 0, empty or NULL",
          "[sql][api]") {
  sql_test::TempDir dir{"api_strict_rows"};
  std::uint32_t p_id = 0, c_id = 0, d_id = 0;
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE p (k BIGINT PRIMARY KEY, a BIGINT, s TEXT)");
    for (std::int64_t k = 1; k <= 3; ++k) {
      CHECK(db->execute("INSERT INTO p VALUES ($1, $2, $3)", {i(k), i(5), bytes("abc")}).affected == 1);
    }
    // Rows 1 to 3 predate c and d: they read them as NULL.
    (void)db->execute("ALTER TABLE p ADD COLUMN c BIGINT");
    (void)db->execute("ALTER TABLE p ADD COLUMN d TEXT");
    auto cat = db->catalog();  // held: p points into it
    const auto* p = cat->table("p");
    p_id = p->id;
    c_id = p->column("c")->id;
    d_id = p->column("d")->id;
    CHECK(db->execute("SELECT c, d FROM p WHERE k = $1", {i(1)}).rows == Grid{{kNull, kNull}});
    db->close();
  }
  // Damage three rows through the engine: row 1 gets c's present bit, row
  // 2 d's, and row 3's var_end for s runs past the row.
  {
    auto engine = bytecask::DB::open(dir.path());
    auto rewrite = [&](std::int64_t k, auto&& damage) {
      Bytes key = row_prefix(p_id);
      key_put_i64(key, k, {PhysType::I64});
      Bytes row;
      REQUIRE(engine.get({}, key, row));
      damage(row);
      engine.put({}, key, row);
    };
    rewrite(1, [&](Bytes& row) { row[kRowHeaderBytes + c_id / 8] |= std::byte{1} << (c_id % 8); });
    rewrite(2, [&](Bytes& row) { row[kRowHeaderBytes + d_id / 8] |= std::byte{1} << (d_id % 8); });
    rewrite(3, [&](Bytes& row) {
      // The var_end table starts at the header's var_table (bytes 4 and 5);
      // s is the row's only variable slot.
      auto var_table = std::to_integer<std::size_t>(row[4]) | (std::to_integer<std::size_t>(row[5]) << 8);
      row[var_table] = std::byte{0xFF};
      row[var_table + 1] = std::byte{0xFF};
    });
    engine.close();
  }
  auto db = Database::open(dir.path());
  auto corrupt = [&](std::string_view sql, std::int64_t k, std::vector<Value> more = {}) {
    std::vector<Value> params{i(k)};
    params.insert(params.end(), more.begin(), more.end());
    try {
      (void)db->execute(sql, params);
      FAIL(sql << " read a column that does not decode, at row " << k);
    } catch (const ExecError& e) {
      CHECK(e.code() == BC_E_CORRUPT);
    }
  };
  corrupt("SELECT c FROM p WHERE k = $1", 1);  // a present fixed column the row predates
  corrupt("SELECT d FROM p WHERE k = $1", 2);  // a present variable column the row predates
  corrupt("SELECT s FROM p WHERE k = $1", 3);  // a variable column that runs past the row
  // An UPDATE copies the old row: the damage is refused there too.
  corrupt("UPDATE p SET a = $2 WHERE k = $1", 1, {i(6)});
  // A column the damage does not touch still reads.
  CHECK(db->execute("SELECT a FROM p WHERE k = $1", {i(1)}).rows == Grid{{i(5)}});
  try {
    (void)db->table_rows("p");
    FAIL("table_rows read a column that does not decode");
  } catch (const ExecError& e) {
    CHECK(e.code() == BC_E_CORRUPT);
    // Row 1, the first in key order, names its damaged column.
    CHECK(std::string{e.what()}.starts_with("column " + std::to_string(c_id) + " does not decode"));
  }
  db->close();
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

TEST_CASE("embedded API: an UPDATE of rows past the plan arena's size writes them all", "[sql][api]") {
  // Each new row is built into memory of its own, not the 4 MiB arena a
  // statement's views share, so a statement may write more rows than fit
  // there.
  sql_test::TempDir dir{"api_many_rows"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE w (g BIGINT, n BIGINT, v BYTEA, PRIMARY KEY (g, n))");
  constexpr std::int64_t kRows = 5000;  // ~1 KiB each: over 4 MiB in all
  auto old = bytes(std::string(1000, 'a'));
  {
    auto t = db->begin();
    for (std::int64_t n = 0; n < kRows; ++n) t.execute("INSERT INTO w VALUES ($1, $2, $3)", {i(1), i(n), old});
    t.commit();
  }
  auto updated = bytes(std::string(1000, 'b'));
  CHECK(db->execute("UPDATE w SET v = $2 WHERE g = $1", {i(1), updated}).affected == kRows);
  CHECK(db->execute("SELECT count(*) FROM w WHERE g = $1", {i(1)}).rows == Grid{{i(kRows)}});
  CHECK(db->execute("SELECT v FROM w WHERE g = $1 AND n = $2", {i(1), i(kRows - 1)}).rows == Grid{{updated}});
  CHECK(db->execute("DELETE FROM w WHERE g = $1", {i(1)}).affected == kRows);
  CHECK(db->execute("SELECT count(*) FROM w WHERE g = $1", {i(1)}).rows == Grid{{i(0)}});
}

TEST_CASE("embedded API: a statement describes its parameters, result and command before it runs",
          "[sql][api][server]") {
  sql_test::TempDir dir{"api_describe"};
  auto db = Database::open(dir.path());
  CHECK(db->prepare("CREATE TABLE a (k INTEGER PRIMARY KEY, amt NUMERIC(6, 2), note VARCHAR(8))").command() ==
        Command::CreateTable);
  (void)db->execute("CREATE TABLE a (k INTEGER PRIMARY KEY, amt NUMERIC(6, 2), note VARCHAR(8))");
  const SqlType integer{SqlKind::Integer};
  const SqlType amount{SqlKind::Decimal, 0, 6, 2};
  const SqlType note{SqlKind::Varchar, 8};

  const auto& insert = db->prepare("INSERT INTO a VALUES ($1, $2, $3)");
  CHECK(insert.command() == Command::Insert);
  REQUIRE(insert.params().size() == 3);
  CHECK(insert.params()[1].type == amount);
  CHECK(insert.params()[1].assigned_only);
  CHECK(insert.columns().empty());

  // Compared or added: not assigned only. SET col = $n assigns.
  const auto& add = db->prepare("UPDATE a SET amt = amt + $1 WHERE k = $2");
  CHECK(add.command() == Command::Update);
  REQUIRE(add.params().size() == 2);
  CHECK(!add.params()[0].assigned_only);
  CHECK(add.params()[1].type == integer);
  CHECK(!add.params()[1].assigned_only);
  const auto& set = db->prepare("UPDATE a SET note = $1 WHERE k = $2");
  CHECK(set.params()[0].type == note);
  CHECK(set.params()[0].assigned_only);

  const auto& select = db->prepare("SELECT amt, note FROM a WHERE k = $1");
  CHECK(select.command() == Command::Select);
  REQUIRE(select.columns().size() == 2);
  CHECK(select.columns()[0].name == "amt");
  CHECK(select.columns()[0].type == amount);
  CHECK(select.columns()[1].type == note);
  const auto& count = db->prepare("SELECT count(*) FROM a WHERE k = $1");
  REQUIRE(count.columns().size() == 1);
  CHECK(count.columns()[0].type == SqlType{SqlKind::BigInt});

  CHECK(db->prepare("DELETE FROM a WHERE k = $1").command() == Command::Delete);
  CHECK(db->prepare("CREATE INDEX a_note ON a (note)").command() == Command::CreateIndex);
  CHECK(db->prepare("DROP TABLE a").command() == Command::DropTable);
}

TEST_CASE("embedded API: sqlstate reports PostgreSQL's SQLSTATE for each error", "[sql][api][server]") {
  sql_test::TempDir dir{"api_sqlstate"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE s (k BIGINT PRIMARY KEY, n SMALLINT NOT NULL)");
  (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(1), i(1)});
  auto state = [&](auto&& run) -> std::string {
    try {
      run();
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  CHECK(state([&] { (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(1), i(2)}); }) == "23505");
  CHECK(state([&] { (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(2), kNull}); }) == "23502");
  CHECK(state([&] { (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(3), i(1 << 16)}); }) == "22003");
  CHECK(state([&] { (void)db->execute("SELEC 1"); }) == "42601");
  // The planner's refusal: a statement in the subset it can't plan, a
  // filter on a DOUBLE PRECISION column.
  (void)db->execute("CREATE TABLE sd (k BIGINT PRIMARY KEY, d DOUBLE PRECISION)");
  CHECK(state([&] { (void)db->execute("SELECT k FROM sd WHERE d < $1", {Value{1.0}}); }) == "0A000");
  CHECK(state([&] { (void)db->execute("CREATE TABLE s (k BIGINT PRIMARY KEY)"); }) == "42P07");
  CHECK(state([&] { (void)db->execute("DROP TABLE nope"); }) == "42P01");
  CHECK(state([&] { (void)db->execute("SELECT n FROM nope WHERE k = $1", {i(1)}); }) == "42P01");
  CHECK(state([&] { (void)db->execute("SELECT nope FROM s WHERE k = $1", {i(1)}); }) == "42703");
  CHECK(state([&] { (void)db->execute("SELECT n FROM s WHERE k = $2", {i(1), i(1)}); }) == "42P18");
  CHECK(state([&] { (void)db->table_rows("nope"); }) == "42P01");
  CHECK(state([&] { (void)db->execute("SELECT n FROM s WHERE k = $1", {}); }) == "08P01");
  auto txn = db->begin();
  CHECK(state([&] { (void)txn.execute("CREATE TABLE u (k BIGINT PRIMARY KEY)"); }) == "0A000");
  CHECK(state([&] { (void)txn.execute("SELECT n FROM s WHERE k = $1", {i(1)}); }) == "25P02");
  CHECK(sqlstate(SerializationFailure{"x"}) == "40001");
  CHECK(sqlstate(ExecError::conflicted("x")) == "40001");
  CHECK(sqlstate(TransactionResolutionUnknown{}) == "08007");
  CHECK(sqlstate(ExecError{"x", BC_E_LIMIT}) == "54000");
  CHECK(sqlstate(ExecError{"x", BC_E_IO}) == "58030");
  CHECK(sqlstate(bytecask::DbClosed{}) == "57P01");
  CHECK(sqlstate(std::runtime_error{"x"}) == "XX000");
}

TEST_CASE("embedded API: refit describes a cached statement against the live catalog", "[sql][api][server]") {
  sql_test::TempDir dir{"api_refit"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY, v BIGINT)");
  const auto& select = db->prepare("SELECT v FROM r WHERE k = $1");
  CHECK(select.params().at(0).type == SqlType{SqlKind::BigInt});
  (void)db->execute("DROP TABLE r");
  (void)db->execute("CREATE TABLE r (k DOUBLE PRECISION PRIMARY KEY, v TEXT)");
  CHECK(select.params().at(0).type == SqlType{SqlKind::BigInt});  // the plan it would have run
  db->refit(select);
  CHECK(select.params().at(0).type == SqlType{SqlKind::Double});
  CHECK(select.columns().at(0).type == SqlType{SqlKind::Text});
  CHECK(db->execute(select, {2.5}).rows.empty());
}

TEST_CASE("embedded API: every plan the search costed returns the same rows", "[sql][api][search]") {
  sql_test::TempDir dir{"api_alternatives"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE o (k BIGINT PRIMARY KEY, a BIGINT NOT NULL, b BIGINT, c BIGINT)");
  (void)db->execute("CREATE INDEX o_a ON o (a)");
  (void)db->execute("CREATE INDEX o_ab ON o (a, b)");
  (void)db->execute("CREATE TABLE p (k BIGINT PRIMARY KEY, v BIGINT)");
  for (std::int64_t k = 0; k < 40; ++k) {
    auto b = k % 3 == 0 ? Value{} : i(k % 5);
    (void)db->execute("INSERT INTO o VALUES ($1, $2, $3, $4)", {i(k), i(k % 4), b, i(k % 7)});
    if (k < 7 && k != 3) (void)db->execute("INSERT INTO p VALUES ($1, $2)", {i(k), i(k % 4)});
  }
  // The rows as a multiset, and in order where the statement orders them.
  auto sorted = [](Grid g) {
    std::ranges::sort(g);
    return g;
  };
  struct Case {
    std::string sql;
    std::vector<Value> params;
    bool ordered;
    std::size_t at_least;  // plans the search costs for it
  };
  const std::vector<Case> cases{
      {"SELECT k, b FROM o WHERE a = $1", {i(1)}, false, 2},
      {"SELECT k FROM o WHERE a = $1 AND b >= $2", {i(2), i(1)}, false, 2},
      {"SELECT count(*) FROM o WHERE a = $1", {i(3)}, false, 4},
      {"SELECT count(b) FROM o WHERE a = $1 AND k < $2", {i(0), i(30)}, false, 3},
      {"SELECT min(b) FROM o WHERE a = $1", {i(1)}, false, 3},
      {"SELECT sum(c) FROM o WHERE a = $1 AND k >= $2", {i(2), i(5)}, false, 3},
      {"SELECT k FROM o WHERE a = $1 ORDER BY k DESC", {i(1)}, true, 1},
      {"SELECT k, b FROM o WHERE a = $1 ORDER BY b, k", {i(2)}, true, 1},
      {"SELECT o.k, p.v FROM o, p WHERE a = $1 AND p.k = o.c", {i(1)}, false, 2},
      {"SELECT count(DISTINCT p.v) FROM o, p WHERE a = $1 AND p.k = o.c", {i(3)}, false, 2},
      // p keyed by a parameter alone: read once, then o's scan; or o's scan
      // with a lookup of p per row. p's key 3 has no row.
      {"SELECT o.k, p.v FROM p, o WHERE p.k = $1 AND a = $2", {i(1), i(2)}, false, 4},
      {"SELECT o.k, p.v FROM p, o WHERE p.k = $1 AND a = $2", {i(3), i(2)}, false, 4},
      {"SELECT count(*) FROM p, o WHERE p.k = $1 AND a = $2", {i(1), i(2)}, false, 4},
      {"SELECT o.k FROM p, o WHERE p.k = $1 AND a = $2 ORDER BY o.k LIMIT 3", {i(5), i(1)}, true, 1},
      // Outer joins: o.c 3 and 4..6 find no row of p, or none whose v passes.
      {"SELECT o.k, p.v FROM o LEFT JOIN p ON p.k = o.c WHERE a = $1", {i(1)}, false, 1},
      {"SELECT o.k, p.v FROM o LEFT JOIN p ON p.k = o.c AND p.v > $2 WHERE a = $1", {i(1), i(1)}, false, 1},
      {"SELECT o.k, p.v FROM o LEFT JOIN p ON p.v = o.b WHERE a = $1", {i(2)}, false, 1},
      {"SELECT o.k, p.v FROM p RIGHT JOIN o ON p.k = o.c WHERE a = $1 AND p.v IS NULL", {i(3)}, false, 1},
      {"SELECT count(p.v) FROM o LEFT JOIN p ON p.k = o.c WHERE a = $1", {i(0)}, false, 1},
      {"SELECT o.k, p.v FROM o LEFT JOIN p ON p.k = o.c WHERE a = $1 ORDER BY p.v, o.k", {i(1)}, true, 1},
      // o the right side, by its index on a, which holds o.k too: it reads
      // rows, where the join matches nothing a row of NULLs.
      {"SELECT p.k, o.k FROM p LEFT JOIN o ON o.a = p.v", {}, false, 1},
      {"SELECT p.k FROM p LEFT JOIN o ON o.a = p.v AND o.k > $1 WHERE o.k IS NULL", {i(30)}, false, 1},
      {"SELECT p.k, o.k FROM p LEFT JOIN o ON o.a = p.v ORDER BY o.k, p.k", {}, true, 1},
      // o.c only in the WHERE above the join: o's index on a does not hold it.
      {"SELECT o.k FROM o LEFT JOIN p ON p.k = o.k WHERE a = $1 AND (p.v = $2 OR o.c = $3)", {i(1), i(1), i(2)}, false, 1},
  };
  const std::string cross = "SELECT o.k, p.v FROM p, o WHERE p.k = $1 AND a = $2";
  CHECK(db->execute(cross, {i(1), i(2)}).rows.size() == 10);
  CHECK(db->execute(cross, {i(3), i(2)}).rows.empty());
  // The parameters execute refuses, out of their column's type among them.
  (void)db->execute("CREATE TABLE q (k INTEGER PRIMARY KEY)");
  CHECK_THROWS_AS(db->alternatives_for_testing("SELECT k FROM q WHERE k = $1", {i(std::int64_t{1} << 40)}), ExecError);
  CHECK_THROWS_AS(db->alternatives_for_testing("SELECT k FROM q WHERE k = $1", {}), ParamError);
  for (const auto& c : cases) {
    INFO(c.sql);
    auto want = db->execute(c.sql, c.params).rows;
    auto all = db->alternatives_for_testing(c.sql, c.params);
    CHECK(all.size() >= c.at_least);
    for (std::size_t n = 0; n < all.size(); ++n) {
      INFO("plan " << n);
      if (c.ordered) {
        CHECK(all[n].rows == want);
      } else {
        CHECK(sorted(all[n].rows) == sorted(want));
      }
    }
  }
}

TEST_CASE("embedded API: a left outer join NULL-extends each left row no right row matches", "[sql][api]") {
  sql_test::TempDir dir{"api_outer_join"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE l (k INTEGER PRIMARY KEY, a INTEGER NOT NULL)");
  (void)db->execute("CREATE TABLE r (j INTEGER PRIMARY KEY, a INTEGER NOT NULL)");
  for (std::int64_t k = 1; k <= 3; ++k) (void)db->execute("INSERT INTO l VALUES ($1, $2)", {i(k), i(k)});
  for (auto [j, a] : {std::pair{10, 1}, {11, 1}, {12, 2}}) (void)db->execute("INSERT INTO r VALUES ($1, $2)", {i(j), i(a)});
  auto rows = [&](std::string_view sql) { return db->execute(sql).rows; };
  const Grid by_a{{i(1), i(10)}, {i(1), i(11)}, {i(2), i(12)}, {i(3), Value{}}};
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON l.a = r.a ORDER BY l.k, r.j") == by_a);
  CHECK(rows("SELECT l.k, r.j FROM r RIGHT JOIN l ON l.a = r.a ORDER BY l.k, r.j") == by_a);
  // ON decides the pairs, WHERE the joined rows: a NULL-extended row is in
  // the first, out of the second.
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON l.a = r.a AND r.j > 10 ORDER BY l.k") ==
        Grid{{i(1), i(11)}, {i(2), i(12)}, {i(3), Value{}}});
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON l.a = r.a WHERE r.j > 10 ORDER BY l.k") ==
        Grid{{i(1), i(11)}, {i(2), i(12)}});
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON l.a = r.a AND l.k = 2 ORDER BY l.k") ==
        Grid{{i(1), Value{}}, {i(2), i(12)}, {i(3), Value{}}});
  CHECK(rows("SELECT l.k FROM l LEFT JOIN r ON l.a = r.a WHERE r.j IS NULL") == Grid{{i(3)}});
  // A NOT NULL column of the right side is NULL in an extended row, so not counted.
  CHECK(rows("SELECT count(r.a), count(*) FROM l LEFT JOIN r ON l.a = r.a WHERE l.k = 3") == Grid{{i(0), i(1)}});
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON r.j = 99 ORDER BY l.k") ==
        Grid{{i(1), Value{}}, {i(2), Value{}}, {i(3), Value{}}});
  CHECK_THROWS_AS(db->execute("SELECT l.k FROM l FULL JOIN r ON l.a = r.a"), ast::SqlError);
  // The right side keyed by a parameter and a left column: a NULL parameter,
  // or a left value too long for any key, matches nothing, and the left row
  // stays, NULL-extended.
  (void)db->execute("CREATE TABLE rc (a INTEGER NOT NULL, s TEXT NOT NULL, PRIMARY KEY (a, s))");
  (void)db->execute("CREATE TABLE ls (k INTEGER PRIMARY KEY, s TEXT NOT NULL)");
  (void)db->execute("INSERT INTO rc VALUES (1, 'x')");
  (void)db->execute("INSERT INTO ls VALUES (1, 'x')");
  (void)db->execute("INSERT INTO ls VALUES (2, $1)", {Value{Bytes(70000, std::byte{'y'})}});
  const std::string keyed = "SELECT ls.k, rc.a FROM ls LEFT JOIN rc ON rc.s = ls.s AND rc.a = $1 ORDER BY ls.k";
  CHECK(db->execute(keyed, {i(1)}).rows == Grid{{i(1), i(1)}, {i(2), Value{}}});
  CHECK(db->execute(keyed, {Value{}}).rows == Grid{{i(1), Value{}}, {i(2), Value{}}});
}

TEST_CASE("embedded API: an uncorrelated subquery runs once, on the statement's snapshot, before its plan",
          "[sql][api]") {
  sql_test::TempDir dir{"api_subquery"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE g (id INTEGER PRIMARY KEY, v INTEGER)");
  (void)db->execute("CREATE TABLE h (k INTEGER PRIMARY KEY, w INTEGER)");
  for (auto [id, v] : {std::pair<std::int64_t, Value>{1, i(10)}, {2, i(20)}, {3, Value{}}, {4, i(20)}, {5, i(30)}}) {
    (void)db->execute("INSERT INTO g VALUES ($1, $2)", {i(id), v});
  }
  for (auto [k, w] : {std::pair<std::int64_t, Value>{1, i(20)}, {2, Value{}}, {3, i(5)}}) {
    (void)db->execute("INSERT INTO h VALUES ($1, $2)", {i(k), w});
  }
  auto ids = [&](std::string_view sql) {
    std::vector<std::int64_t> out;
    for (const auto& r : db->execute(sql).rows) out.push_back(std::get<std::int64_t>(r[0]));
    std::ranges::sort(out);
    return out;
  };
  using V = std::vector<std::int64_t>;
  CHECK(ids("SELECT id FROM g WHERE v = (SELECT MAX(v) FROM g)") == V{5});
  CHECK(ids("SELECT id FROM g WHERE id = (SELECT MAX(k) FROM h)") == V{3});
  CHECK(ids("SELECT id FROM g WHERE v = (SELECT w FROM h WHERE k = 2)").empty());  // NULL matches nothing
  CHECK(ids("SELECT id FROM g WHERE v IN (SELECT w FROM h)") == V{2, 4});
  // h holds a NULL, so NOT IN is never true: NULL where no value equals v.
  CHECK(ids("SELECT id FROM g WHERE v NOT IN (SELECT w FROM h)").empty());
  CHECK(ids("SELECT id FROM g WHERE v NOT IN (SELECT w FROM h WHERE w IS NOT NULL)") == V{1, 5});
  CHECK(ids("SELECT id FROM g WHERE v > ALL (SELECT w FROM h WHERE w IS NOT NULL)") == V{5});
  CHECK(ids("SELECT id FROM g WHERE v > ALL (SELECT w FROM h WHERE k > 9)") == V{1, 2, 3, 4, 5});  // over none: true
  CHECK(ids("SELECT id FROM g WHERE v < ANY (SELECT w FROM h)") == V{1});
  CHECK(ids("SELECT id FROM g WHERE EXISTS (SELECT 1 FROM h WHERE w = 5)") == V{1, 2, 3, 4, 5});
  CHECK(ids("SELECT id FROM g WHERE NOT EXISTS (SELECT k, w FROM h)").empty());
  // EXISTS reads no column: over a join it filters the left side, and in an
  // outer join's ON the right one, the left rows staying.
  CHECK(ids("SELECT g.id FROM g JOIN h ON g.v = h.w WHERE EXISTS (SELECT 1 FROM h WHERE k = 3)") == V{2, 4});
  CHECK(ids("SELECT g.id FROM g JOIN h ON g.v = h.w WHERE NOT EXISTS (SELECT 1 FROM h)").empty());
  CHECK(db->execute("SELECT g.id, h.k FROM g LEFT JOIN h ON g.v = h.w AND NOT EXISTS (SELECT 1 FROM h) WHERE g.id = 2")
            .rows == Grid{{i(2), Value{}}});
  CHECK(db->execute("SELECT (SELECT w FROM h WHERE k = 3), (SELECT w FROM h WHERE k = 9)").rows == Grid{{i(5), Value{}}});
  try {
    (void)db->execute("SELECT id FROM g WHERE v = (SELECT w FROM h)");
    FAIL("a scalar subquery of three rows ran");
  } catch (const ExecError& e) {
    CHECK(sqlstate(e) == "21000");
  }
  // A write's subquery reads the rows as they were before the statement.
  CHECK(db->execute("UPDATE g SET v = v + 100 WHERE v = (SELECT MAX(v) FROM g)").affected == 1);
  CHECK(ids("SELECT id FROM g WHERE v > 100") == V{5});
  // In a transaction, its earlier writes are the subquery's too.
  auto t = db->begin();
  (void)t.execute("INSERT INTO h VALUES ($1, $2)", {i(9), i(10)});
  auto rows = t.execute("SELECT id FROM g WHERE v IN (SELECT w FROM h)").rows;
  CHECK(rows.size() == 3);  // 10, 20, 20
  CHECK(t.commit() == CommitOutcome::Committed);
  // Every plan the search costs for the outer query agrees.
  for (auto sql : {"SELECT id FROM g WHERE id IN (SELECT k FROM h)", "SELECT id FROM g WHERE v >= (SELECT MIN(w) FROM h)"}) {
    INFO(sql);
    auto want = db->execute(sql).rows;
    std::ranges::sort(want);
    for (auto& alt : db->alternatives_for_testing(sql, {})) {
      std::ranges::sort(alt.rows);
      CHECK(alt.rows == want);
    }
  }
}

TEST_CASE("embedded API: table aliases qualify columns and hide the table's name; AS names the output",
          "[sql][api]") {
  sql_test::TempDir dir{"api_alias"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE g (id INTEGER PRIMARY KEY, v INTEGER)");
  (void)db->execute("CREATE TABLE h (k INTEGER PRIMARY KEY, w INTEGER)");
  for (auto [id, v] : {std::pair<std::int64_t, std::int64_t>{1, 30}, {2, 10}, {3, 20}}) {
    (void)db->execute("INSERT INTO g VALUES ($1, $2)", {i(id), i(v)});
  }
  (void)db->execute("INSERT INTO h VALUES (1, 7)");
  auto r = db->execute("SELECT x.id AS ident, v total FROM g AS x WHERE x.v > 10 ORDER BY total");
  CHECK(r.columns == std::vector<std::string>{"ident", "total"});
  CHECK(r.rows == Grid{{i(3), i(20)}, {i(1), i(30)}});
  // ORDER BY an output name, over the input column of that name.
  CHECK(db->execute("SELECT id AS v, v AS id FROM g ORDER BY v DESC").rows ==
        Grid{{i(3), i(20)}, {i(2), i(10)}, {i(1), i(30)}});
  CHECK(db->execute("SELECT x.id, y.w FROM g x JOIN h AS y ON x.id = y.k").rows == Grid{{i(1), i(7)}});
  CHECK(db->execute("SELECT COUNT(*) AS n, MAX(x.v) m FROM g x").columns == std::vector<std::string>{"n", "m"});
  CHECK(db->execute("SELECT 1 AS one").columns == std::vector<std::string>{"one"});
  CHECK(db->execute("SELECT x.v FROM g x GROUP BY x.v ORDER BY x.v").rows == Grid{{i(10)}, {i(20)}, {i(30)}});
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return std::string{"ran"};
  };
  CHECK(state("SELECT g.id FROM g x") == "42P01");  // the alias hides the table's name
  CHECK(state("SELECT id FROM g x JOIN h x ON x.id = x.k") == "42712");
  CHECK(state("SELECT x.id FROM g x") == "ran");
  CHECK(state("SELECT x.id FROM g AS x (a, b)") == "42601");  // renaming columns is not supported
}

TEST_CASE("embedded API: a choice point's plan text shows both plans, and each returns the same rows",
          "[sql][api][search][choice]") {
  sql_test::TempDir dir{"api_choice"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE c (k BIGINT PRIMARY KEY, a BIGINT, b BIGINT, v BIGINT)");
  (void)db->execute("CREATE INDEX c_a ON c (a)");
  (void)db->execute("CREATE INDEX c_b ON c (b)");
  // a = 0 on 150 of the 200 rows, b = 0 on 10 of them; NULLs in both.
  for (std::int64_t k = 0; k < 200; ++k) {
    auto a = k % 4 == 3 ? i(k % 7) : i(0);
    auto b = k % 20 == 0 ? i(0) : (k % 11 == 0 ? Value{} : i(k % 9 + 1));
    (void)db->execute("INSERT INTO c VALUES ($1, $2, $3, $4)", {i(k), k % 50 == 49 ? Value{} : a, b, i(k)});
  }
  const std::string sql = "SELECT k, v FROM c WHERE a = $1 AND b = $2";
  // Counted when compiled: 200 rows, 20 entries under each index. The plan
  // counts c_a's range and scans it below 21 entries, c_b's from there.
  const auto& src = db->prepare(sql).plan_source();
  INFO(src);
  CHECK(src.find("count0 = bc_count_range(cx, IDX_") != std::string::npos);
  CHECK(src.find("if (count0 < 21) {") != std::string::npos);
  CHECK(src.find("} else {") != std::string::npos);
  auto sorted = [](Grid g) {
    std::ranges::sort(g);
    return g;
  };
  for (const auto& params : std::vector<std::vector<Value>>{{i(0), i(0)}, {i(3), i(0)}, {i(0), i(5)}, {i(6), i(9)}}) {
    Grid want;
    for (std::int64_t k = 0; k < 200; ++k) {
      auto a = k % 50 == 49 ? std::optional<std::int64_t>{} : (k % 4 == 3 ? k % 7 : 0);
      auto b = k % 20 == 0 ? std::optional<std::int64_t>{0} : (k % 11 == 0 ? std::nullopt : std::optional{k % 9 + 1});
      if (a == std::get<std::int64_t>(params[0]) && b == std::get<std::int64_t>(params[1])) want.push_back({i(k), i(k)});
    }
    INFO(std::get<std::int64_t>(params[0]) << ", " << std::get<std::int64_t>(params[1]));
    CHECK(sorted(db->execute(sql, params).rows) == sorted(want));
    // The cheapest plan forced to each branch, then each other plan.
    auto all = db->alternatives_for_testing(sql, params);
    CHECK(all.size() >= 2);
    for (std::size_t n = 0; n < all.size(); ++n) {
      INFO("plan " << n);
      CHECK(sorted(all[n].rows) == sorted(want));
    }
  }
}
