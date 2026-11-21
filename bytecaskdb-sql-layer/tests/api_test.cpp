// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The embedded API end to end: the seven statements of the prototype, in
// order, from SQL text to rows, then a fresh open of the same directory to
// show the catalog and the data persist.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <thread>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.text;
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

TEST_CASE("embedded API: IF NOT EXISTS and IF EXISTS skip a DDL with PostgreSQL's notice", "[sql][api]") {
  sql_test::TempDir dir{"api"};
  auto db = Database::open(dir.path());
  CHECK(db->execute("CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v INTEGER)").notices.empty());
  (void)db->execute("INSERT INTO t VALUES (1, 2)");
  auto notice = [&](std::string_view sql) {
    auto r = db->execute(sql);
    REQUIRE(r.notices.size() == 1);
    return std::pair{r.notices[0].sqlstate, r.notices[0].message};
  };
  // The name taken: nothing changes, the definition is not read (a column twice is no error).
  CHECK(notice("CREATE TABLE IF NOT EXISTS t (a INTEGER, a INTEGER)") ==
        std::pair{std::string{"42P07"}, std::string{"relation \"t\" already exists, skipping"}});
  CHECK(db->catalog()->table({kPublicSchema, "t"})->columns.size() == 2);
  CHECK(db->execute("SELECT id, v FROM t").rows.size() == 1);
  CHECK(notice("DROP TABLE IF EXISTS nope") ==
        std::pair{std::string{"00000"}, std::string{"table \"nope\" does not exist, skipping"}});
  CHECK(notice("DROP INDEX IF EXISTS nope").second == "index \"nope\" does not exist, skipping");
  CHECK(notice("DROP VIEW IF EXISTS nope").second == "view \"nope\" does not exist, skipping");
  CHECK(notice("DROP PROCEDURE IF EXISTS nope(BIGINT, VARCHAR(3))").second ==
        "procedure nope(pg_catalog.int8,pg_catalog.varchar) does not exist, skipping");
  // Without IF NOT EXISTS the name taken is still 42P07.
  CHECK_THROWS(db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY)"));
  CHECK(db->execute("DROP TABLE IF EXISTS t").notices.empty());
  db->close();
}

TEST_CASE("embedded API: CREATE ROLE, GRANT and REVOKE are DDL kept in the catalog, with PostgreSQL's tags and notices",
          "[sql][api][roles]") {
  sql_test::TempDir dir{"api"};
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
    CHECK(db->prepare("CREATE ROLE r").command() == Command::CreateRole);
    CHECK(db->prepare("GRANT SELECT ON t TO r").command() == Command::Grant);
    CHECK(db->prepare("REVOKE SELECT ON t FROM r").command() == Command::Revoke);
    CHECK(db->prepare("DROP ROLE r").command() == Command::DropRole);
    CHECK(db->prepare("CREATE DOMAIN d AS INTEGER").command() == Command::CreateDomain);
    CHECK(db->prepare("CREATE TYPE ty").command() == Command::CreateType);
    CHECK(db->prepare("CREATE SEQUENCE s").command() == Command::CreateSequence);
    CHECK(db->prepare("DROP DOMAIN d").command() == Command::DropDomain);
    CHECK(db->prepare("DROP TYPE ty").command() == Command::DropType);
    CHECK(db->prepare("DROP SEQUENCE s").command() == Command::DropSequence);
    CHECK(db->execute("DROP SEQUENCE IF EXISTS s").notices.at(0).message == "sequence \"s\" does not exist, skipping");
    CHECK(db->execute("DROP DOMAIN IF EXISTS d").notices.at(0).message == "type \"d\" does not exist, skipping");
    (void)db->execute("CREATE ROLE r");
    (void)db->execute("GRANT SELECT, UPDATE (v) ON t TO r, PUBLIC");
    const auto* t = db->catalog()->table({kPublicSchema, "t"});
    CHECK(db->catalog()->acl(t->id).size() == 4);  // per grantee, the table's and the column's
    // Every session is the superuser: nothing is denied.
    (void)db->execute("INSERT INTO t VALUES (1, 2)");
    CHECK(db->execute("SELECT v FROM t").rows.size() == 1);
    auto r = db->execute("DROP ROLE IF EXISTS nope, other");
    REQUIRE(r.notices.size() == 2);
    CHECK(r.notices[1].sqlstate == "00000");
    CHECK(r.notices[1].message == "role \"other\" does not exist, skipping");
    CHECK_THROWS(db->execute("DROP ROLE r"));  // it holds privileges (2BP01)
    db->close();
  }
  {
    auto db = Database::open(dir.path());
    REQUIRE(db->catalog()->role("r") != nullptr);
    (void)db->execute("REVOKE ALL ON t FROM r, PUBLIC");
    CHECK(db->catalog()->acl(db->catalog()->table({kPublicSchema, "t"})->id).empty());
    CHECK(db->execute("DROP ROLE r").notices.empty());
    CHECK(db->catalog()->roles().empty());
    db->close();
  }
}

TEST_CASE("embedded API: a cached write plan maintains an index created after it", "[sql][api]") {
  sql_test::TempDir dir{"api_ddl"};
  auto db = Database::open(dir.path());
  (void)db->execute(kCreateTable);
  const auto& insert = db->prepare(kInsert);
  (void)db->execute(insert, {i(1), bytes("apple"), i(10)});
  auto compiled = db->compilations();
  // The index is created after the plan, which names no index: the plan
  // binds again once after the DDL, as every cached plan does, and the
  // runtime writes the entry.
  (void)db->execute(kCreateIndex);
  (void)db->execute(insert, {i(2), bytes("apple"), i(20)});
  CHECK(db->compilations() == compiled + 1);
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

  // The table is dropped and created again: the catalog moved, so every
  // cached plan binds again at its next run, and runs on the new table.
  (void)db->execute("DROP TABLE t");
  (void)db->execute(kCreateTable);
  (void)db->execute(kCreateIndex);
  CHECK(db->execute(insert, {i(2), bytes("banana"), i(20)}).affected == 1);
  CHECK(db->execute(by_id, {i(2)}).rows == Grid{{i(2), i(20)}});
  CHECK(db->execute(by_id, {i(1)}).rows.empty());
  CHECK(ids(db->execute(by_name, {bytes("a"), bytes("c")})) == std::vector<std::int64_t>{2});
  // Each of the three plans binds again once after the three DDL: only the
  // version a plan is behind matters, not how far behind.
  CHECK(db->compilations() == compiled + 3);
  // Once bound again they run without another compile.
  CHECK(db->execute(insert, {i(3), bytes("cherry"), i(30)}).affected == 1);
  CHECK(db->compilations() == compiled + 3);
  // Parameters the plan does not take are refused without a compile,
  // however often they are sent.
  for (int k = 0; k < 3; ++k) {
    CHECK_THROWS_AS(db->execute(insert, {i(9), bytes("x"), bytes("y")}), ParamError);
  }
  CHECK(db->compilations() == compiled + 3);
  // Any DDL moves the version: the plan binds again, once.
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  CHECK_THROWS_AS(db->execute(insert, {i(9), bytes("x"), bytes("y")}), ParamError);
  CHECK(db->compilations() == compiled + 4);
  CHECK(db->execute(insert, {i(9), bytes("x"), i(90)}).affected == 1);
  CHECK(db->compilations() == compiled + 4);

  // qty is now bytes. The held INSERT keeps the parameter types it fixed,
  // as a PostgreSQL prepared statement does: a BIGINT $3 for a BYTEA column
  // is the binder's error. By text the statement binds afresh, with the new
  // types, and a later prepare of the text returns that statement.
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BYTEA)");
  CHECK_THROWS_AS(db->execute(insert, {i(4), bytes("date"), bytes("forty")}), ast::SqlError);
  CHECK(db->execute(kInsert, {i(4), bytes("date"), bytes("forty")}).affected == 1);
  CHECK_THROWS_AS(db->execute(kInsert, {i(5), bytes("elder"), i(50)}), ParamError);
  CHECK(&db->prepare(kInsert) != &insert);
  CHECK(db->prepare(kInsert).params().at(2).type == SqlType{SqlKind::Bytea});
  CHECK(insert.params().at(2).type == SqlType{SqlKind::BigInt});
  // Back to BIGINT: the held statement fits again and runs.
  (void)db->execute("DROP TABLE t");
  (void)db->execute(kCreateTable);
  CHECK_THROWS_AS(db->execute(insert, {i(6), bytes("fig"), bytes("sixty")}), ParamError);
  CHECK(db->execute(insert, {i(6), bytes("fig"), i(60)}).affected == 1);
  CHECK(db->execute(by_id, {i(6)}).rows == Grid{{i(6), i(60)}});
}

TEST_CASE("embedded API: a plan that reads a table twice is recompiled when the table's ids change", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_instances"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE s (id BIGINT PRIMARY KEY, parent BIGINT)");
  for (auto [id, parent] : {std::pair{1, 0}, {2, 1}, {3, 1}}) {
    (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(id), i(parent)});
  }
  const auto& pairs = db->prepare("SELECT x.id, y.id FROM s x JOIN s y ON x.id = y.parent WHERE x.id >= $1 ORDER BY 2");
  CHECK(db->execute(pairs, {i(0)}).rows == Grid{{i(1), i(2)}, {i(1), i(3)}});
  // Both uses read the one table's rows, named in the plan by its ids and not an instance's: a plan that
  // named the instances would never be found fit, or never refused.
  CHECK(pairs.plan_source().find("// objects: ") != std::string::npos);
  auto compiled = db->compilations();
  // A DDL binds the plan again; it reads the same rows. A column: the plan keeps its table's key.
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  CHECK(db->execute(pairs, {i(0)}).rows == Grid{{i(1), i(2)}, {i(1), i(3)}});
  (void)db->execute("ALTER TABLE s ADD COLUMN extra BIGINT");
  CHECK(db->execute(pairs, {i(0)}).rows == Grid{{i(1), i(2)}, {i(1), i(3)}});
  // The table dropped and created again, with other rows: new ids, and the plan, bound again, reads the new
  // table through both uses.
  (void)db->execute("DROP TABLE s");
  (void)db->execute("CREATE TABLE s (id BIGINT PRIMARY KEY, parent BIGINT)");
  (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(7), i(0)});
  (void)db->execute("INSERT INTO s VALUES ($1, $2)", {i(8), i(7)});
  CHECK(db->execute(pairs, {i(0)}).rows == Grid{{i(7), i(8)}});
  CHECK(db->compilations() > compiled);
  (void)db->execute("DROP TABLE s");
  CHECK_THROWS(db->execute(pairs, {i(0)}));
}

TEST_CASE("embedded API: parameters are checked against the plan that will run", "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_stale_params"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k INTEGER PRIMARY KEY, v BIGINT)");
  const auto& by_k = db->prepare("SELECT v FROM t WHERE k = $1");
  CHECK(db->execute(by_k, {i(1)}).rows.empty());
  // The key is recreated as BIGINT. By text the statement binds afresh: $1
  // is BIGINT, and the parameter is checked against it. The held statement
  // keeps the INTEGER $1 it fixed, as PostgreSQL's prepared statement does:
  // bound again at INTEGER, compared with the BIGINT key, and a value
  // outside INTEGER is refused before the run (22003).
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("INSERT INTO t VALUES ($1, $2)", {i(3'000'000'000), i(7)});
  (void)db->execute("INSERT INTO t VALUES ($1, $2)", {i(5), i(8)});
  CHECK(db->execute("SELECT v FROM t WHERE k = $1", {i(3'000'000'000)}).rows == Grid{{i(7)}});
  CHECK(db->execute(by_k, {i(5)}).rows == Grid{{i(8)}});
  CHECK(by_k.params().at(0).type == SqlType{SqlKind::Integer});
  try {
    (void)db->execute(by_k, {i(3'000'000'000)});
    FAIL("ran");
  } catch (const ExecError& e) {
    CHECK(sqlstate(e) == "22003");
  }
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

TEST_CASE("embedded API: a cached plan rebinds once after each DDL, a GRANT included, and not again",
          "[sql][api][catalog]") {
  sql_test::TempDir dir{"api_plan_rebinds"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, v BIGINT)");
  const auto& by_k = db->prepare("SELECT v FROM t WHERE k = $1");
  (void)db->execute(by_k, {i(1)});
  (void)db->execute("SELECT k FROM t WHERE v = $1", {i(1)});
  auto compiled = db->compilations();
  for (int k = 0; k < 3; ++k) (void)db->execute(by_k, {i(1)});
  CHECK(db->compilations() == compiled);  // bound at the live catalog
  // A DDL on another table moves the version: each cached text binds again
  // once, held or by text, and runs without another compile after.
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  for (int k = 0; k < 3; ++k) {
    (void)db->execute(by_k, {i(1)});
    (void)db->execute("SELECT v FROM t WHERE k = $1", {i(1)});
    (void)db->execute("SELECT k FROM t WHERE v = $1", {i(1)});
  }
  CHECK(db->compilations() == compiled + 2);
  // A GRANT is a DDL: one more rebind each.
  (void)db->execute("CREATE ROLE r");
  (void)db->execute("GRANT SELECT ON t TO r");
  for (int k = 0; k < 3; ++k) (void)db->execute(by_k, {i(1)});
  CHECK(db->compilations() == compiled + 3);
  // Bound again after the table is dropped and created again, it runs on the new table.
  (void)db->execute("DROP TABLE t");
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, v BIGINT)");
  (void)db->execute("INSERT INTO t VALUES ($1, $2)", {i(1), i(9)});
  CHECK(db->execute(by_k, {i(1)}).rows == Grid{{i(9)}});
  CHECK(db->compilations() == compiled + 5);  // the INSERT's first compile, and by_k's rebind
  CHECK(db->execute(by_k, {i(1)}).rows == Grid{{i(9)}});
  CHECK(db->compilations() == compiled + 5);
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

TEST_CASE("embedded API: every cached plan recompiles once after a DDL, ADD COLUMN and CREATE INDEX included",
          "[sql][api][catalog]") {
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

  // ADD COLUMN: each cached plan binds again once, and runs with its
  // columns unchanged; rows the INSERT without a column list writes take the
  // default. A second run compiles nothing.
  (void)db->execute("ALTER TABLE t ADD COLUMN tag BIGINT DEFAULT 5");
  CHECK(db->execute(insert, {i(2), bytes("banana"), i(20)}).affected == 1);
  CHECK(db->execute(by_id, {i(2)}).rows == Grid{{i(2), i(20)}});
  CHECK(ids(db->execute(by_name, {bytes("a"), bytes("c")})) == std::vector<std::int64_t>{1, 2});
  CHECK(db->compilations() == compiled + 3);
  CHECK(db->execute(by_id, {i(2)}).rows == Grid{{i(2), i(20)}});
  CHECK(db->compilations() == compiled + 3);
  // Row 1, written before the column, was rewritten with the default.
  CHECK(db->execute("SELECT id, tag FROM t WHERE id = $1", {i(1)}).rows == Grid{{i(1), i(5)}});
  CHECK(db->execute("SELECT id, tag FROM t WHERE id = $1", {i(2)}).rows == Grid{{i(2), i(5)}});
  compiled = db->compilations();

  // CREATE INDEX: each plan binds again once; the writes maintain the index.
  (void)db->execute("CREATE INDEX t_by_qty ON t (qty)");
  (void)db->execute(by_id, {i(1)});
  (void)db->execute(by_name, {bytes("a"), bytes("b")});
  CHECK(db->execute(insert, {i(3), bytes("cherry"), i(30)}).affected == 1);
  CHECK(db->compilations() == compiled + 3);
  (void)db->execute(by_id, {i(1)});
  CHECK(db->compilations() == compiled + 3);
  CHECK(ids(db->execute("SELECT id FROM t WHERE qty >= $1", {i(15)})) == std::vector<std::int64_t>{2, 3});
  compiled = db->compilations();

  // DROP INDEX recompiles the plans that scan it: by name, a full scan now.
  (void)db->execute("DROP INDEX t_by_name");
  CHECK(ids(db->execute(by_name, {bytes("a"), bytes("b")})) == std::vector<std::int64_t>{1});
  CHECK(by_name.plan_source().find("IDX_3") == std::string::npos);
  // A filter on a DOUBLE its index took is a filter on the row once the
  // index is dropped.
  (void)db->execute("ALTER TABLE t ADD COLUMN d DOUBLE PRECISION DEFAULT 1.5");
  (void)db->execute("CREATE INDEX t_by_d ON t (d)");
  const auto& by_d = db->prepare("SELECT id FROM t WHERE d >= $1");
  CHECK(ids(db->execute(by_d, {Value{1.0}})) == std::vector<std::int64_t>{1, 2, 3});
  CHECK(by_d.plan_source().find("IDX_") != std::string::npos);
  (void)db->execute("DROP INDEX t_by_d");
  CHECK(ids(db->execute(by_d, {Value{1.0}})) == std::vector<std::int64_t>{1, 2, 3});
  CHECK(by_d.plan_source().find("IDX_") == std::string::npos);
  // One that then fails to bind again, its table dropped, is refused; the
  // other plans bind again once and run.
  (void)db->execute("CREATE TABLE dd (k BIGINT PRIMARY KEY, d DOUBLE PRECISION)");
  const auto& dd_by_d = db->prepare("SELECT k FROM dd WHERE d >= $1");
  CHECK(db->execute(dd_by_d, {Value{1.0}}).rows.empty());
  const auto dd_source = dd_by_d.plan_source();
  compiled = db->compilations();
  (void)db->execute("DROP TABLE dd");
  CHECK_THROWS_AS(db->execute(dd_by_d, {Value{1.0}}), ast::SqlError);
  // A failed rebind keeps the statement's old plan whole and behind the
  // catalog: the next execute tries again and fails the same way, never
  // running the old code (ASan checks the schema ref it reads).
  CHECK_THROWS_AS(db->execute(dd_by_d, {Value{1.0}}), ast::SqlError);
  CHECK(dd_by_d.plan_source() == dd_source);
  CHECK(db->execute(insert, {i(4), bytes("date"), i(40)}).affected == 1);
  CHECK(db->execute(by_id, {i(4)}).rows == Grid{{i(4), i(40)}});
  CHECK(db->compilations() == compiled + 2);

  // DROP TABLE: every plan on it is refused when it binds again.
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
  auto ix_id = db->catalog()->index({kPublicSchema, "c_email"}).second->id;
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
  auto table_id = db->catalog()->table({kPublicSchema, "c"})->id;
  try {
    (void)db->execute("CREATE UNIQUE INDEX c_w ON c (w)");
    FAIL("a unique index over duplicates was created");
  } catch (const ExecError& e) {
    REQUIRE(e.constraint());
    CHECK(e.constraint()->constraint == BC_C_UNIQUE);
    CHECK(e.constraint()->object_id == table_id);  // the index never existed: its id is the next DDL's
  }
  CHECK(db->catalog()->index({kPublicSchema, "c_w"}).second == nullptr);
  db->close();

  auto again = Database::open(dir.path());
  CHECK(again->catalog()->index({kPublicSchema, "c_email"}).second->unique);
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
  auto table_id = db->catalog()->table({kPublicSchema, "t"})->id;
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
  const auto* p_table = third_cat->table({kPublicSchema, "p"});
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
    const auto* p = cat->table({kPublicSchema, "p"});
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
      REQUIRE(key_put_i64(key, k, {PhysType::I64}));
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
  auto table_id = db->catalog()->table({kPublicSchema, "r"})->id;
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
  auto tm_id = db->catalog()->table({kPublicSchema, "tm"})->id;
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
  CHECK(db->catalog()->table({kPublicSchema, "two"}) == nullptr);

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

TEST_CASE("embedded API: $n::T in a SELECT without FROM is of type T and range-checked as one", "[sql][api]") {
  sql_test::TempDir dir{"api_param_cast"};
  auto db = Database::open(dir.path());
  const auto& q = db->prepare("SELECT $1::integer, $1::integer + 1");
  REQUIRE(q.params().size() == 1);
  CHECK(q.params()[0].type == SqlType{SqlKind::Integer});
  REQUIRE(q.columns().size() == 2);
  CHECK(q.columns()[0].name == "int4");
  CHECK(q.columns()[1].name == "?column?");
  CHECK(db->execute("SELECT $1::integer, $1::integer + 1", {i(41)}).rows == Grid{{i(41), i(42)}});
  CHECK(db->execute("SELECT $1::integer + 1", {kNull}).rows == Grid{{kNull}});
  auto state = [&](std::int64_t v) -> std::string {
    try {
      (void)db->execute("SELECT $1::integer + 1", {i(v)});
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  CHECK(state(2147483646) == "ran");
  CHECK(state(2147483647) == "22003");  // the sum
  CHECK(state(2147483648) == "22003");  // $1 itself
}

TEST_CASE("embedded API: a DDL is bound at execute, against the catalog it runs in; a query at prepare",
          "[sql][api]") {
  sql_test::TempDir dir{"api_ddl_bind"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  auto state = [&](auto&& run) -> std::string {
    try {
      run();
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  // A DDL's binder error: it prepares, describes itself, and throws at execute.
  const auto& index = db->prepare("CREATE INDEX u_a ON u (a)");
  CHECK(index.is_ddl());
  CHECK(index.command() == Command::CreateIndex);
  CHECK(db->refit("CREATE INDEX u_a ON u (a)", ResultCheck::none()).ddl);
  CHECK(state([&] { (void)db->execute(index, {}); }) == "42P01");
  CHECK(state([&] { (void)db->execute("CREATE INDEX u_a ON u (a)"); }) == "42P01");
  const auto& add = db->prepare("ALTER TABLE t ADD COLUMN a BIGINT");
  CHECK(state([&] { (void)db->execute(add, {}); }) == "42701");
  CHECK(state([&] { (void)db->prepare("CREATE VIEW v AS SELECT nope FROM t"); }) == "ran");
  CHECK(state([&] { (void)db->execute("CREATE VIEW v AS SELECT nope FROM t"); }) == "42703");
  CHECK(state([&] { (void)db->prepare("GRANT SELECT ON t TO nobody"); }) == "ran");
  CHECK(state([&] { (void)db->execute("GRANT SELECT ON t TO nobody"); }) == "42704");
  CHECK(db->catalog()->view({kPublicSchema, "v"}) == nullptr);
  // Bound where it runs: the table it names, created after the prepare, is found.
  (void)db->execute("CREATE TABLE u (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute(index, {});
  CHECK(db->catalog()->index({kPublicSchema, "u_a"}).second != nullptr);
  // A query's binder error is still thrown at prepare, and nothing is cached.
  const auto plans = db->cached_plans();
  CHECK(state([&] { (void)db->prepare("SELECT a FROM w"); }) == "42P01");
  CHECK(db->cached_plans() == plans);
  CHECK(state([&] { (void)db->prepare("SELECT nope FROM t"); }) == "42703");
  // A syntax error is the parse's, at prepare, DDL or not.
  CHECK(state([&] { (void)db->prepare("CREATE INDEX ON"); }) == "42601");
  db->close();
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
  // A feature PostgreSQL refuses as not supported: FOR UPDATE with DISTINCT.
  CHECK(state([&] { (void)db->execute("SELECT DISTINCT n FROM s FOR UPDATE"); }) == "0A000");
  CHECK(state([&] { (void)db->execute("CREATE TABLE s (k BIGINT PRIMARY KEY)"); }) == "42P07");
  CHECK(state([&] { (void)db->execute("DROP TABLE nope"); }) == "42P01");
  CHECK(state([&] { (void)db->execute("SELECT n FROM nope WHERE k = $1", {i(1)}); }) == "42P01");
  CHECK(state([&] { (void)db->execute("SELECT nope FROM s WHERE k = $1", {i(1)}); }) == "42703");
  CHECK(state([&] { (void)db->execute("SELECT n FROM s WHERE k = $2", {i(1), i(1)}); }) == "42P18");
  CHECK(state([&] { (void)db->table_rows("nope"); }) == "42P01");
  CHECK(state([&] { (void)db->execute("SELECT n FROM s WHERE k = $1", {}); }) == "08P01");
  auto txn = db->begin();
  CHECK(state([&] { (void)txn.execute("CREATE TABLE u AS SELECT 1.5"); }) == "0A000");
  CHECK(state([&] { (void)txn.execute("SELECT n FROM s WHERE k = $1", {i(1)}); }) == "25P02");
  CHECK(sqlstate(SerializationFailure{"x"}) == "40001");
  CHECK(sqlstate(ExecError::conflicted("x")) == "40001");
  CHECK(sqlstate(TransactionResolutionUnknown{}) == "08007");
  CHECK(sqlstate(ExecError{"x", BC_E_LIMIT}) == "54000");
  CHECK(sqlstate(ExecError{"x", BC_E_IO}) == "58030");
  CHECK(sqlstate(bytecask::DbClosed{}) == "57P01");
  CHECK(sqlstate(std::runtime_error{"x"}) == "XX000");
}

TEST_CASE("embedded API: refit binds a held statement again with the types it fixed", "[sql][api][server]") {
  sql_test::TempDir dir{"api_refit"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY, v BIGINT)");
  const auto& select = db->prepare("SELECT v FROM r WHERE k = $1");
  CHECK(select.params().at(0).type == SqlType{SqlKind::BigInt});
  // Another column type, same parameter: bound again, and the same record.
  (void)db->execute("ALTER TABLE r ADD COLUMN w BIGINT");
  db->refit(select);
  CHECK(select.params().at(0).type == SqlType{SqlKind::BigInt});
  // A key of another type: the BIGINT $1 it fixed no longer fits (42883,
  // double precision = bigint has an operator in PostgreSQL, not here: the
  // binder's refusal), and its result column changed: refit refuses it, and
  // params() keeps what the statement fixed.
  (void)db->execute("DROP TABLE r");
  (void)db->execute("CREATE TABLE r (k DOUBLE PRECISION PRIMARY KEY, v TEXT)");
  CHECK_THROWS(db->refit(select));
  CHECK(select.params().at(0).type == SqlType{SqlKind::BigInt});
  CHECK(select.columns().at(0).type == SqlType{SqlKind::BigInt});
  // By text the statement binds afresh, and describes the live catalog.
  auto record = db->refit("SELECT v FROM r WHERE k = $1", ResultCheck::none());
  CHECK(record.params.at(0).type == SqlType{SqlKind::Double});
  CHECK(record.columns.at(0).type == SqlType{SqlKind::Text});
  CHECK(db->execute("SELECT v FROM r WHERE k = $1", {2.5}).rows.empty());
  // Its result column changed, so a record fixed before is 0A000.
  (void)db->execute("DROP TABLE r");
  (void)db->execute("CREATE TABLE r (k DOUBLE PRECISION PRIMARY KEY, v BIGINT)");
  try {
    (void)db->refit("SELECT v FROM r WHERE k = $1", ResultCheck::given(record));
    FAIL("refit");
  } catch (const FeatureNotSupported& e) {
    CHECK(sqlstate(e) == "0A000");
    CHECK(std::string{e.what()} == "cached plan must not change result type");
  }
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
      {"SELECT o.k, p.v FROM o LEFT JOIN p ON p.k = o.c AND FALSE WHERE a = $1", {i(1)}, false, 1},
      {"SELECT o.k, p.v FROM o JOIN p ON TRUE WHERE a = $1 AND p.k < $2", {i(1), i(3)}, false, 2},
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
      // A table with itself, and three tables: the join order and the access of each table vary, as does a
      // hash join of the first two on a column no key reaches, and each agrees.
      {"SELECT x.k, y.k FROM o x JOIN o y ON x.b = y.b WHERE x.a = $1 AND y.a = $2", {i(1), i(2)}, false, 2},
      {"SELECT x.k, y.k FROM o x JOIN o y ON x.a = y.a AND x.k < y.k WHERE x.k < $1 AND y.k > $2", {i(12), i(30)}, false, 1},
      {"SELECT x.k, p.v, y.k FROM o x, p, o y WHERE x.a = $1 AND p.k = x.c AND y.c = p.k", {i(1)}, false, 2},
      {"SELECT x.k, y.k, p.k FROM o x, o y, p WHERE x.b = y.b AND y.c = p.v AND x.a = $1", {i(2)}, false, 2},
      {"SELECT x.k, y.k, p.k FROM o x, o y, p WHERE x.b = y.b AND y.c = p.v AND x.a = $1 ORDER BY x.k, y.k, p.k", {i(2)}, true, 1},
      {"SELECT x.k, y.k, p.k FROM o x JOIN o y ON x.k < y.k JOIN p ON p.k <> y.c WHERE x.a = $1 AND y.a = $2", {i(1), i(3)}, false, 1},
      // A comparison, a predicate over both sides of a join, and an outer join of three.
      {"SELECT o.k, p.k FROM o JOIN p ON o.c <> p.k WHERE a = $1", {i(1)}, false, 1},
      {"SELECT o.k, p.k FROM o JOIN p ON NOT (o.c = p.k) OR o.b IS NULL WHERE a = $1", {i(0)}, false, 1},
      {"SELECT o.k, p.v, q.k FROM o LEFT JOIN p ON p.k = o.c LEFT JOIN o q ON q.k = p.v WHERE o.a = $1", {i(1)}, false, 1},
      {"SELECT o.k, p.v, q.k FROM o LEFT JOIN p ON p.k = o.c AND p.v > $2 LEFT JOIN o q ON q.b = p.v AND q.k <> o.k WHERE o.a = $1",
       {i(1), i(0)}, false, 1},
      {"SELECT o.k, p.v, q.k FROM o LEFT JOIN p ON p.k = o.c JOIN o q ON q.k = p.v WHERE o.a = $1 ORDER BY 1, 2, 3", {i(2)}, true, 1},
      {"SELECT o.k, q.v FROM o JOIN o x ON x.k = o.c LEFT JOIN p q ON q.k = x.k WHERE o.a = $1", {i(1)}, false, 1},
      // A RIGHT JOIN after the first join: its table, then a group of two NULL-extended together.
      {"SELECT x.k, p.v, o.k FROM o x JOIN p ON p.k = x.c RIGHT JOIN o ON p.v = o.b WHERE o.a = $1", {i(1)}, false, 1},
      {"SELECT x.k, p.v, o.k FROM o x JOIN p ON p.k = x.c AND x.a = $2 RIGHT JOIN o ON p.v = o.b WHERE o.a = $1",
       {i(2), i(1)}, false, 1},
      {"SELECT x.k, p.v, o.k FROM o x JOIN p ON p.k = x.c RIGHT JOIN o ON p.v = o.b AND x.k > o.k WHERE o.a = $1 ORDER BY 3, 1, 2",
       {i(0)}, true, 1},
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

TEST_CASE("embedded API: a FULL JOIN keeps the rows of both sides, NULL-extended where the ON matches none", "[sql][api]") {
  sql_test::TempDir dir{"api_full_join"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE l (k INTEGER PRIMARY KEY, a INTEGER)");
  (void)db->execute("CREATE TABLE r (j INTEGER PRIMARY KEY, a INTEGER)");
  (void)db->execute("CREATE TABLE n (a INTEGER, s INTEGER)");  // no primary key: a hidden row id
  (void)db->execute("INSERT INTO l VALUES (1, 1), (2, 2), (3, NULL)");
  (void)db->execute("INSERT INTO r VALUES (10, 1), (11, 1), (12, 4), (13, NULL)");
  (void)db->execute("INSERT INTO n VALUES (2, 20), (NULL, 21), (5, 22)");
  auto rows = [&](std::string_view sql) { return db->execute(sql).rows; };
  auto sorted = [](Grid g) {
    std::ranges::sort(g);
    return g;
  };
  const Value null{};
  // Every pair the ON makes, each row of l it pairs with nothing, then each
  // row of r: a NULL key matches nothing, on either side.
  const Grid both{{i(1), i(10)}, {i(1), i(11)}, {i(2), null}, {i(3), null}, {null, i(12)}, {null, i(13)}};
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a ORDER BY l.k, r.j") == both);
  CHECK(sorted(rows("SELECT l.k, r.j FROM l FULL OUTER JOIN r ON r.a = l.a")) == sorted(both));
  // The ON decides the pairs; the WHERE is applied after the NULL-extension.
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a AND r.j > 10 ORDER BY l.k, r.j") ==
        Grid{{i(1), i(11)}, {i(2), null}, {i(3), null}, {null, i(10)}, {null, i(12)}, {null, i(13)}});
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a WHERE r.j IS NULL ORDER BY l.k") ==
        Grid{{i(2), null}, {i(3), null}});
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a WHERE l.k IS NULL ORDER BY r.j") ==
        Grid{{null, i(12)}, {null, i(13)}});
  // ON TRUE pairs every row; ON FALSE none.
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON TRUE").size() == 12);
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON FALSE").size() == 7);
  // USING: the merged column is the side's that has a row.
  CHECK(rows("SELECT a, l.k, r.j FROM l FULL JOIN r USING (a) ORDER BY a, l.k, r.j") ==
        Grid{{i(1), i(1), i(10)}, {i(1), i(1), i(11)}, {i(2), i(2), null}, {i(4), null, i(12)}, {null, i(3), null}, {null, null, i(13)}});
  // A table with no primary key on either side.
  CHECK(rows("SELECT n.s, l.k FROM n FULL JOIN l ON n.a = l.a ORDER BY n.s, l.k") ==
        Grid{{i(20), i(2)}, {i(21), null}, {i(22), null}, {null, i(1)}, {null, i(3)}});
  CHECK(rows("SELECT l.k, n.s FROM l FULL JOIN n ON n.a = l.a ORDER BY n.s, l.k") ==
        Grid{{i(2), i(20)}, {null, i(21)}, {null, i(22)}, {i(1), null}, {i(3), null}});
  // ORDER BY a column it does not select, and a LIMIT over the sorted rows.
  CHECK(rows("SELECT l.k FROM l FULL JOIN r ON l.a = r.a ORDER BY r.j DESC, l.k LIMIT 3") == Grid{{i(2)}, {i(3)}, {null}});
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a LIMIT 2").size() == 2);
  // A table joined to itself.
  CHECK(rows("SELECT x.k, y.k FROM l AS x FULL JOIN l AS y ON x.a = y.k ORDER BY x.k, y.k") ==
        Grid{{i(1), i(1)}, {i(2), i(2)}, {i(3), null}, {null, i(3)}});
  // A view of a FULL JOIN, its WHERE and the query's above the join.
  (void)db->execute("CREATE VIEW v AS SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a");
  CHECK(rows("SELECT j FROM v WHERE k IS NULL ORDER BY j") == Grid{{i(12)}, {i(13)}});
  CHECK_THROWS_AS(db->execute("SELECT l.k FROM l FULL JOIN r ON l.a < r.a"), ast::SqlError);
  // NOT (a <> b) is a = b, as PostgreSQL reads it: joinable.
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON NOT (l.a <> r.a) ORDER BY l.k, r.j") == both);
  // A bare $n ORDER BY key sorts on nothing but is the statement's parameter.
  const auto& by_param = db->prepare("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a ORDER BY $1, 1, 2");
  CHECK(by_param.params().size() == 1);
  CHECK(db->execute(by_param, {bytes("5")}).rows == both);  // text, as PostgreSQL types it there
  // The WHERE's IN and NOT IN of a subquery test each arm's rows against one set.
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a WHERE r.j IN (SELECT j FROM r WHERE j > 10) ORDER BY r.j") ==
        Grid{{i(1), i(11)}, {null, i(12)}, {null, i(13)}});
  CHECK(rows("SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a WHERE l.k NOT IN (SELECT a FROM r WHERE a IS NOT NULL) "
             "ORDER BY l.k") == Grid{{i(2), null}, {i(3), null}});
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
  // A constant ON: FALSE (or NULL) pairs no row, so each left row is NULL-extended; TRUE pairs every one.
  const Grid none{{i(1), Value{}}, {i(2), Value{}}, {i(3), Value{}}};
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON FALSE ORDER BY l.k") == none);
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON NULL ORDER BY l.k") == none);
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON l.a = r.a AND FALSE ORDER BY l.k") == none);
  CHECK(rows("SELECT l.k, r.j FROM l LEFT JOIN r ON TRUE WHERE l.k = 3 ORDER BY r.j") ==
        Grid{{i(3), i(10)}, {i(3), i(11)}, {i(3), i(12)}});
  CHECK(rows("SELECT count(*) FROM l JOIN r ON TRUE") == Grid{{i(9)}});
  CHECK(rows("SELECT count(*) FROM l JOIN r ON FALSE") == Grid{{i(0)}});
  CHECK(rows("SELECT l.k FROM l WHERE NOT FALSE AND (l.k = 1 OR NULL)") == Grid{{i(1)}});
  CHECK(rows("SELECT l.k FROM l WHERE NULL").empty());
  // Aggregates and groups over the rows a constant FALSE leaves, none: one
  // row of NULL and 0 with no GROUP BY, and no group with one.
  CHECK(rows("SELECT min(l.a), max(l.k), count(*), sum(l.a) FROM l WHERE FALSE") == Grid{{Value{}, Value{}, i(0), Value{}}});
  CHECK(rows("SELECT min(r.j) FROM l JOIN r ON FALSE") == Grid{{Value{}}});
  CHECK(rows("SELECT l.a, count(*) FROM l WHERE NULL GROUP BY l.a").empty());
  CHECK(rows("SELECT l.a, max(r.j) FROM l JOIN r ON FALSE GROUP BY l.a").empty());
  CHECK(rows("SELECT DISTINCT l.a FROM l WHERE l.k > 1 AND FALSE").empty());
  CHECK(rows("SELECT l.k FROM l WHERE l.a = (SELECT max(r.j) FROM r WHERE NULL)").empty());
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

// The rows below are PostgreSQL 16's, for the same statements on the same rows.
TEST_CASE("embedded API: an inner join in parentheses, and joins in a comma list", "[sql][api][join]") {
  sql_test::TempDir dir{"api_join_groups"};
  auto db = Database::open(dir.path());
  for (auto t : {"a", "b", "c"}) (void)db->execute(std::string{"CREATE TABLE "} + t + " (id INTEGER PRIMARY KEY, k INTEGER)");
  (void)db->execute("INSERT INTO a VALUES (1, 1), (2, 2), (3, NULL)");
  (void)db->execute("INSERT INTO b VALUES (1, 1), (2, 3)");
  (void)db->execute("INSERT INTO c VALUES (1, 1), (2, 2)");
  auto rows = [&](std::string_view sql) { return db->execute(sql).rows; };
  const Value n{};
  CHECK(rows("SELECT a.id, b.id, c.id FROM a JOIN (b JOIN c ON b.k = c.k) ON a.k = b.k") == Grid{{i(1), i(1), i(1)}});
  CHECK(rows("SELECT a.id, b.id, c.id FROM a LEFT JOIN b ON a.k = b.k, c WHERE c.k = a.k ORDER BY 1") ==
        Grid{{i(1), i(1), i(1)}, {i(2), n, i(2)}});
  CHECK(rows("SELECT a.id, b.id, c.id FROM a, b JOIN c ON b.k = c.k ORDER BY 1") ==
        Grid{{i(1), i(1), i(1)}, {i(2), i(1), i(1)}, {i(3), i(1), i(1)}});
  // A LEFT JOIN after a comma extends its own left side's rows: each of a's with each of b's.
  CHECK(rows("SELECT a.id, b.id, c.id FROM a, b LEFT JOIN c ON b.k = c.k ORDER BY 1, 2") ==
        Grid{{i(1), i(1), i(1)}, {i(1), i(2), n}, {i(2), i(1), i(1)}, {i(2), i(2), n}, {i(3), i(1), i(1)}, {i(3), i(2), n}});
  // The ON of the join in parentheses cannot read a (42P01), as in PostgreSQL.
  try {
    (void)db->execute("SELECT 1 FROM a JOIN (b JOIN c ON a.k = c.k) ON a.k = b.k");
    FAIL("accepted");
  } catch (const ast::SqlError& e) {
    CHECK(e.sqlstate() == "42P01");
  }
}

TEST_CASE("embedded API: joins of three or more tables, a table with itself, and ON by any comparison", "[sql][api][join]") {
  sql_test::TempDir dir{"api_joins"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE h1 (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER)");
  (void)db->execute("CREATE TABLE h2 (id INTEGER PRIMARY KEY, a INTEGER, c INTEGER)");
  (void)db->execute("CREATE INDEX h2_a ON h2 (a)");
  (void)db->execute("CREATE TABLE h3 (k INTEGER PRIMARY KEY, a INTEGER)");
  auto insert = [&](std::string_view table, std::initializer_list<std::initializer_list<Value>> rows) {
    for (const auto& r : rows) {
      auto sql = "INSERT INTO " + std::string{table} + (r.size() == 3 ? " VALUES ($1, $2, $3)" : " VALUES ($1, $2)");
      (void)db->execute(sql, std::vector<Value>{r});
    }
  };
  const Value n{};
  // Each table has a row of key 0, which a row of NULLs would read as, if a join read a key from one.
  insert("h1", {{i(0), i(3), i(5)}, {i(1), i(1), i(10)}, {i(2), i(2), i(20)}, {i(3), i(2), n}, {i(4), n, i(40)}, {i(5), i(5), i(50)}});
  insert("h2", {{i(0), i(4), i(50)}, {i(1), i(1), i(100)}, {i(2), i(2), i(200)}, {i(3), i(2), n}, {i(4), n, i(400)}, {i(6), i(6), i(600)}});
  insert("h3", {{i(0), i(4)}, {i(1), i(1)}, {i(2), i(2)}, {i(3), n}, {i(7), i(7)}});
  auto rows = [&](std::string_view sql) { return db->execute(sql).rows; };
  // three tables, inner
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 JOIN h2 ON h1.a = h2.a JOIN h3 ON h2.a = h3.a ORDER BY 1, 2, 3") ==
        Grid{{i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(2)}, {i(3), i(2), i(2)}, {i(3), i(3), i(2)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1, h2, h3 WHERE h1.a < h2.a AND h2.a <> h3.a ORDER BY 1, 2, 3") ==
        Grid{{i(0), i(0), i(1)}, {i(0), i(0), i(2)}, {i(0), i(0), i(7)}, {i(0), i(6), i(0)}, {i(0), i(6), i(1)}, {i(0), i(6), i(2)}, {i(0), i(6), i(7)}, {i(1), i(0), i(1)}, {i(1), i(0), i(2)}, {i(1), i(0), i(7)}, {i(1), i(2), i(0)}, {i(1), i(2), i(1)}, {i(1), i(2), i(7)}, {i(1), i(3), i(0)}, {i(1), i(3), i(1)}, {i(1), i(3), i(7)}, {i(1), i(6), i(0)}, {i(1), i(6), i(1)}, {i(1), i(6), i(2)}, {i(1), i(6), i(7)}, {i(2), i(0), i(1)}, {i(2), i(0), i(2)}, {i(2), i(0), i(7)}, {i(2), i(6), i(0)}, {i(2), i(6), i(1)}, {i(2), i(6), i(2)}, {i(2), i(6), i(7)}, {i(3), i(0), i(1)}, {i(3), i(0), i(2)}, {i(3), i(0), i(7)}, {i(3), i(6), i(0)}, {i(3), i(6), i(1)}, {i(3), i(6), i(2)}, {i(3), i(6), i(7)}, {i(5), i(6), i(0)}, {i(5), i(6), i(1)}, {i(5), i(6), i(2)}, {i(5), i(6), i(7)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h3, h2, h1 WHERE h1.id = h2.id AND h3.k = h2.id ORDER BY 1") ==
        Grid{{i(0), i(0), i(0)}, {i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(3), i(3), i(3)}});
  CHECK(rows("SELECT count(*) FROM h1 JOIN h2 ON h1.a = h2.a JOIN h3 ON h2.a = h3.a") ==
        Grid{{i(5)}});
  // a chain of left joins
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 LEFT JOIN h2 ON h1.a = h2.a LEFT JOIN h3 ON h2.a = h3.a ORDER BY 1, 2, 3") ==
        Grid{{i(0), Value{}, Value{}}, {i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(2)}, {i(3), i(2), i(2)}, {i(3), i(3), i(2)}, {i(4), Value{}, Value{}}, {i(5), Value{}, Value{}}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 LEFT JOIN h2 ON h1.a = h2.a LEFT JOIN h3 ON h1.a = h3.a AND h2.c > 100 ORDER BY 1, 2, 3") ==
        Grid{{i(0), Value{}, Value{}}, {i(1), i(1), Value{}}, {i(2), i(2), i(2)}, {i(2), i(3), Value{}}, {i(3), i(2), i(2)}, {i(3), i(3), Value{}}, {i(4), Value{}, Value{}}, {i(5), Value{}, Value{}}});
  CHECK(rows("SELECT h1.id, h3.k FROM h1 LEFT JOIN h2 ON h1.a = h2.a LEFT JOIN h3 ON h2.a = h3.a WHERE h3.k IS NULL ORDER BY 1, 2") ==
        Grid{{i(0), Value{}}, {i(4), Value{}}, {i(5), Value{}}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 LEFT JOIN h2 ON h1.a = h2.a JOIN h3 ON h2.a = h3.a ORDER BY 1, 2, 3") ==
        Grid{{i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(2)}, {i(3), i(2), i(2)}, {i(3), i(3), i(2)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 JOIN h2 ON h1.id = h2.id LEFT JOIN h3 ON h2.a = h3.a ORDER BY 1, 2, 3") ==
        Grid{{i(0), i(0), i(0)}, {i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(3), i(3), i(2)}, {i(4), i(4), Value{}}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 LEFT JOIN h2 ON h1.a = h2.a LEFT JOIN h3 ON h2.id = h3.k ORDER BY 1, 2, 3") ==
        Grid{{i(0), Value{}, Value{}}, {i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(3)}, {i(3), i(2), i(2)}, {i(3), i(3), i(3)}, {i(4), Value{}, Value{}}, {i(5), Value{}, Value{}}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 LEFT JOIN h2 ON h1.a = h2.a JOIN h3 ON h2.id = h3.k ORDER BY 1, 2, 3") ==
        Grid{{i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(3)}, {i(3), i(2), i(2)}, {i(3), i(3), i(3)}});
  // A RIGHT JOIN as the first join is the LEFT JOIN of its tables swapped, and later joins follow it.
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h2 RIGHT JOIN h1 ON h1.a = h2.a JOIN h3 ON h2.a = h3.a ORDER BY 1, 2, 3") ==
        Grid{{i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(2)}, {i(3), i(2), i(2)}, {i(3), i(3), i(2)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h2 RIGHT JOIN h1 ON h1.a = h2.a LEFT JOIN h3 ON h2.id = h3.k ORDER BY 1, 2, 3") ==
        Grid{{i(0), Value{}, Value{}}, {i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(3)}, {i(3), i(2), i(2)}, {i(3), i(3), i(3)}, {i(4), Value{}, Value{}}, {i(5), Value{}, Value{}}});
  // A RIGHT JOIN after the first join keeps every row of its table against the join before it, a group of tables
  // NULL-extended together: its own ON decides its rows, and the RIGHT JOIN's ON matches them.
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 JOIN h2 ON h1.a = h2.a RIGHT JOIN h3 ON h2.a = h3.a ORDER BY 3, 1, 2") ==
        Grid{{n, n, i(0)}, {i(1), i(1), i(1)}, {i(2), i(2), i(2)}, {i(2), i(3), i(2)}, {i(3), i(2), i(2)}, {i(3), i(3), i(2)}, {n, n, i(3)}, {n, n, i(7)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 JOIN h2 ON h1.a = h2.a AND h2.c > 100 RIGHT JOIN h3 ON h2.a = h3.a ORDER BY 3, 1, 2") ==
        Grid{{n, n, i(0)}, {n, n, i(1)}, {i(2), i(2), i(2)}, {i(3), i(2), i(2)}, {n, n, i(3)}, {n, n, i(7)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1 JOIN h2 ON h1.a = h2.a RIGHT JOIN h3 ON h2.a = h3.a WHERE h1.id IS NULL ORDER BY 3") ==
        Grid{{n, n, i(0)}, {n, n, i(3)}, {n, n, i(7)}});
  CHECK(rows("SELECT h1.id, h2.id, h3.k, x.id FROM h1 JOIN h2 ON h1.a = h2.a RIGHT JOIN h3 ON h2.a = h3.a LEFT JOIN h1 x ON x.id = h1.id "
             "ORDER BY 3, 1, 2") ==
        Grid{{n, n, i(0), n}, {i(1), i(1), i(1), i(1)}, {i(2), i(2), i(2), i(2)}, {i(2), i(3), i(2), i(2)}, {i(3), i(2), i(2), i(3)},
             {i(3), i(3), i(2), i(3)}, {n, n, i(3), n}, {n, n, i(7), n}});
  // After a comma, its group is the join's own items: the comma's are joined to its rows.
  CHECK(rows("SELECT h1.id, h2.id, h3.k FROM h1, h2 RIGHT JOIN h3 ON h2.id = h3.k WHERE h1.id < 2 ORDER BY 1, 3, 2") ==
        Grid{{i(0), i(0), i(0)}, {i(0), i(1), i(1)}, {i(0), i(2), i(2)}, {i(0), i(3), i(3)}, {i(0), n, i(7)},
             {i(1), i(0), i(0)}, {i(1), i(1), i(1)}, {i(1), i(2), i(2)}, {i(1), i(3), i(3)}, {i(1), n, i(7)}});
  // Its USING column is its right table's, the one it keeps.
  CHECK(rows("SELECT a, h1.id, h2.id, h3.k FROM h1 JOIN h2 USING (a) RIGHT JOIN h3 USING (a) ORDER BY 4, 2, 3") ==
        Grid{{i(4), n, n, i(0)}, {i(1), i(1), i(1), i(1)}, {i(2), i(2), i(2), i(2)}, {i(2), i(2), i(3), i(2)}, {i(2), i(3), i(2), i(2)},
             {i(2), i(3), i(3), i(2)}, {n, n, n, i(3)}, {i(7), n, n, i(7)}});
  CHECK(rows("SELECT h1.a, count(h2.id), count(h3.k), count(*) FROM h1 LEFT JOIN h2 ON h1.a = h2.a LEFT JOIN h3 ON h2.a = h3.a GROUP BY h1.a ORDER BY 1") ==
        Grid{{i(1), i(1), i(1), i(1)}, {i(2), i(4), i(4), i(4)}, {i(3), i(0), i(0), i(1)}, {i(5), i(0), i(0), i(1)}, {Value{}, i(0), i(0), i(1)}});
  // a table joined with itself
  CHECK(rows("SELECT x.id, y.id FROM h1 x JOIN h1 y ON x.a = y.a ORDER BY 1, 2") ==
        Grid{{i(0), i(0)}, {i(1), i(1)}, {i(2), i(2)}, {i(2), i(3)}, {i(3), i(2)}, {i(3), i(3)}, {i(5), i(5)}});
  CHECK(rows("SELECT x.id, y.id FROM h1 x JOIN h1 y ON x.id < y.id WHERE x.a = y.a ORDER BY 1, 2") ==
        Grid{{i(2), i(3)}});
  CHECK(rows("SELECT h1.id, y.id FROM h1 JOIN h1 AS y ON h1.id = y.id ORDER BY 1") ==
        Grid{{i(0), i(0)}, {i(1), i(1)}, {i(2), i(2)}, {i(3), i(3)}, {i(4), i(4)}, {i(5), i(5)}});
  CHECK(rows("SELECT x.id, y.id, z.id FROM h1 x JOIN h1 y ON x.id < y.id JOIN h1 z ON y.id < z.id ORDER BY 1, 2, 3") ==
        Grid{{i(0), i(1), i(2)}, {i(0), i(1), i(3)}, {i(0), i(1), i(4)}, {i(0), i(1), i(5)}, {i(0), i(2), i(3)}, {i(0), i(2), i(4)}, {i(0), i(2), i(5)}, {i(0), i(3), i(4)}, {i(0), i(3), i(5)}, {i(0), i(4), i(5)}, {i(1), i(2), i(3)}, {i(1), i(2), i(4)}, {i(1), i(2), i(5)}, {i(1), i(3), i(4)}, {i(1), i(3), i(5)}, {i(1), i(4), i(5)}, {i(2), i(3), i(4)}, {i(2), i(3), i(5)}, {i(2), i(4), i(5)}, {i(3), i(4), i(5)}});
  CHECK(rows("SELECT x.id, y.id FROM h1 x LEFT JOIN h1 y ON x.a = y.a AND x.id <> y.id ORDER BY 1, 2") ==
        Grid{{i(0), Value{}}, {i(1), Value{}}, {i(2), i(3)}, {i(3), i(2)}, {i(4), Value{}}, {i(5), Value{}}});
  CHECK(rows("SELECT x.id, y.id FROM h1 x RIGHT JOIN h1 y ON x.a < y.a ORDER BY 1, 2") ==
        Grid{{i(0), i(5)}, {i(1), i(0)}, {i(1), i(2)}, {i(1), i(3)}, {i(1), i(5)}, {i(2), i(0)}, {i(2), i(5)}, {i(3), i(0)}, {i(3), i(5)}, {Value{}, i(1)}, {Value{}, i(4)}});
  CHECK(rows("SELECT x.id, y.id FROM h1 x JOIN h2 z ON x.a = z.a JOIN h1 y ON z.a = y.a ORDER BY 1, 2") ==
        Grid{{i(1), i(1)}, {i(2), i(2)}, {i(2), i(2)}, {i(2), i(3)}, {i(2), i(3)}, {i(3), i(2)}, {i(3), i(2)}, {i(3), i(3)}, {i(3), i(3)}});
  // any comparison
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a < h2.a ORDER BY 1, 2") ==
        Grid{{i(0), i(0)}, {i(0), i(6)}, {i(1), i(0)}, {i(1), i(2)}, {i(1), i(3)}, {i(1), i(6)}, {i(2), i(0)}, {i(2), i(6)}, {i(3), i(0)}, {i(3), i(6)}, {i(5), i(6)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a <= h2.a ORDER BY 1, 2") ==
        Grid{{i(0), i(0)}, {i(0), i(6)}, {i(1), i(0)}, {i(1), i(1)}, {i(1), i(2)}, {i(1), i(3)}, {i(1), i(6)}, {i(2), i(0)}, {i(2), i(2)}, {i(2), i(3)}, {i(2), i(6)}, {i(3), i(0)}, {i(3), i(2)}, {i(3), i(3)}, {i(3), i(6)}, {i(5), i(6)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a > h2.a ORDER BY 1, 2") ==
        Grid{{i(0), i(1)}, {i(0), i(2)}, {i(0), i(3)}, {i(2), i(1)}, {i(3), i(1)}, {i(5), i(0)}, {i(5), i(1)}, {i(5), i(2)}, {i(5), i(3)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a >= h2.a ORDER BY 1, 2") ==
        Grid{{i(0), i(1)}, {i(0), i(2)}, {i(0), i(3)}, {i(1), i(1)}, {i(2), i(1)}, {i(2), i(2)}, {i(2), i(3)}, {i(3), i(1)}, {i(3), i(2)}, {i(3), i(3)}, {i(5), i(0)}, {i(5), i(1)}, {i(5), i(2)}, {i(5), i(3)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a <> h2.a ORDER BY 1, 2") ==
        Grid{{i(0), i(0)}, {i(0), i(1)}, {i(0), i(2)}, {i(0), i(3)}, {i(0), i(6)}, {i(1), i(0)}, {i(1), i(2)}, {i(1), i(3)}, {i(1), i(6)}, {i(2), i(0)}, {i(2), i(1)}, {i(2), i(6)}, {i(3), i(0)}, {i(3), i(1)}, {i(3), i(6)}, {i(5), i(0)}, {i(5), i(1)}, {i(5), i(2)}, {i(5), i(3)}, {i(5), i(6)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a = h2.a AND h1.b < h2.c ORDER BY 1, 2") ==
        Grid{{i(1), i(1)}, {i(2), i(2)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 JOIN h2 ON h1.a = h2.a OR h1.b IS NULL ORDER BY 1, 2") ==
        Grid{{i(1), i(1)}, {i(2), i(2)}, {i(2), i(3)}, {i(3), i(0)}, {i(3), i(1)}, {i(3), i(2)}, {i(3), i(3)}, {i(3), i(4)}, {i(3), i(6)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 LEFT JOIN h2 ON h1.a < h2.a ORDER BY 1, 2") ==
        Grid{{i(0), i(0)}, {i(0), i(6)}, {i(1), i(0)}, {i(1), i(2)}, {i(1), i(3)}, {i(1), i(6)}, {i(2), i(0)}, {i(2), i(6)}, {i(3), i(0)}, {i(3), i(6)}, {i(4), Value{}}, {i(5), i(6)}});
  CHECK(rows("SELECT h1.id, h2.id FROM h1 LEFT JOIN h2 ON h1.a = h2.a AND NOT (h1.b = h2.c) ORDER BY 1, 2") ==
        Grid{{i(0), Value{}}, {i(1), i(1)}, {i(2), i(2)}, {i(3), Value{}}, {i(4), Value{}}, {i(5), Value{}}});

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
  // The names PostgreSQL requires: a table named twice needs aliases, and an ON sees the tables before it.
  CHECK(state("SELECT id FROM h1 x JOIN h1 y ON x.id = y.id") == "42702");
  CHECK(state("SELECT h1.id FROM h1 x JOIN h1 y ON x.id = y.id") == "42P01");
  CHECK(state("SELECT 1 FROM h1, h1") == "42712");
  CHECK(state("SELECT 1 FROM h1 x, h2 x") == "42712");
  CHECK(state("SELECT 1 FROM h1 JOIN h2 ON h1.a = h3.a JOIN h3 ON h2.a = h3.a") == "42P01");
  // What the layer does not take yet is refused, never answered wrongly.
  CHECK(state("SELECT 1 FROM h1 x LEFT JOIN h2 y ON x.a = y.a RIGHT JOIN h3 z ON y.a = z.a") == "42601");
  CHECK(state("SELECT 1 FROM h1 x RIGHT JOIN h2 y ON x.a = y.a RIGHT JOIN h3 z ON y.a = z.a") == "42601");
  // USING and NATURAL after the first join read the columns of the join before them, as PostgreSQL 17's.
  CHECK(rows("SELECT * FROM h1 NATURAL JOIN h2 NATURAL JOIN h3 ORDER BY a, id") ==
        Grid{{i(1), i(1), i(10), i(100), i(1)}, {i(2), i(2), i(20), i(200), i(2)}, {i(2), i(3), n, n, i(2)}});
  CHECK(rows("SELECT * FROM h1 LEFT JOIN h3 USING (a) LEFT JOIN h2 USING (id) ORDER BY id") ==
        Grid{{i(0), i(3), i(5), n, i(4), i(50)}, {i(1), i(1), i(10), i(1), i(1), i(100)}, {i(2), i(2), i(20), i(2), i(2), i(200)},
             {i(3), i(2), n, i(2), i(2), n}, {i(4), n, i(40), n, n, i(400)}, {i(5), i(5), i(50), n, n, n}});
  CHECK(state("SELECT 1 FROM h1 JOIN h2 USING (id) JOIN h3 USING (a)") == "42702");  // h1.a and h2.a
  CHECK(state("SELECT 1 FROM h1 JOIN h2 USING (id) JOIN h3 USING (k)") == "42703");
  // Up to 32 tables, each here h1 joined on its key to the one before it; a 33rd is 0A000.
  auto self_joined = [](int n) {
    std::string from = "h1 t0";
    std::string where;
    for (auto k = 1; k < n; ++k) {
      from += ", h1 t" + std::to_string(k);
      where += (k == 1 ? " WHERE " : " AND ") + std::string{"t"} + std::to_string(k) + ".id = t" + std::to_string(k - 1) + ".id";
    }
    return "SELECT count(*) FROM " + from + where;
  };
  CHECK(rows(self_joined(9)) == Grid{{i(6)}});
  CHECK(rows(self_joined(32)) == Grid{{i(6)}});
  CHECK(state(self_joined(33)) == "0A000");
  CHECK(state("SELECT 1 FROM h1 x JOIN h1 y ON x.id = y.id FOR UPDATE") == "42601");
}

TEST_CASE("embedded API: a correlated subquery runs for each outer row, on the statement's snapshot", "[sql][api]") {
  sql_test::TempDir dir{"api_correlated"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE g (id INTEGER PRIMARY KEY, v INTEGER, s TEXT)");
  (void)db->execute("CREATE TABLE h (k INTEGER PRIMARY KEY, w INTEGER, t TEXT)");
  (void)db->execute("INSERT INTO g VALUES (1, 1, 'x'), (2, 2, 'y'), (3, NULL, 'z'), (4, 4, NULL), (5, 5, 'x')");
  (void)db->execute("INSERT INTO h VALUES (1, 1, 'x'), (2, 2, 'q'), (3, NULL, 'z'), (4, 4, 'x'), (5, 4, NULL)");
  auto ids = [&](std::string_view sql, std::vector<Value> params = {}) {
    std::vector<std::int64_t> out;
    for (const auto& r : db->execute(sql, params).rows) out.push_back(std::get<std::int64_t>(r[0]));
    std::ranges::sort(out);
    return out;
  };
  using V = std::vector<std::int64_t>;
  // e061_13_01_01 and _02: b = a reads the outer row's a; a NULL a matches
  // no row, so NOT IN of its empty set is true.
  CHECK(ids("SELECT v FROM g WHERE v IN (SELECT w FROM h WHERE w = v)") == V{1, 2, 4});
  CHECK(db->execute("SELECT v FROM g WHERE v NOT IN (SELECT w FROM h WHERE w = v) ORDER BY v").rows ==
        Grid{{i(5)}, {Value{}}});
  CHECK(ids("SELECT id FROM g WHERE EXISTS (SELECT 1 FROM h WHERE h.t = g.s)") == V{1, 3, 5});
  CHECK(ids("SELECT id FROM g WHERE NOT EXISTS (SELECT 1 FROM h WHERE h.t = g.s)") == V{2, 4});
  CHECK(ids("SELECT id FROM g WHERE v >= ALL (SELECT w FROM h WHERE h.k <= g.id)") == V{1, 2});
  CHECK(ids("SELECT id FROM g WHERE v > ANY (SELECT w FROM h WHERE h.t = g.s)") == V{5});
  CHECK(ids("SELECT id FROM g WHERE v IN (SELECT MAX(w) FROM h WHERE h.k <= g.id)") == V{1, 2, 4});
  CHECK(ids("SELECT id FROM g WHERE v IN (SELECT COUNT(*) FROM h WHERE h.w = g.v)") == V{1});
  // Beside the statement's own parameters, and an alias of the outer table.
  CHECK(ids("SELECT id FROM g o WHERE v > $1 AND EXISTS (SELECT 1 FROM h WHERE h.w = o.v AND h.k > $2)", {i(1), i(3)}) == V{4});
  // A $n only a subquery names is the statement's, before its constants.
  CHECK(ids("SELECT id FROM g WHERE v IN (SELECT w FROM h WHERE h.k > $1)", {i(3)}) == V{4});
  CHECK(ids("SELECT COUNT(*) FROM g WHERE id = $1 AND EXISTS (SELECT 1 FROM h WHERE h.w = g.v)", {i(4)}) == V{1});
  CHECK(ids("SELECT COUNT(*) FROM g WHERE id = $1 AND EXISTS (SELECT 1 FROM h WHERE h.w = g.v)", {i(3)}) == V{0});
  // A write's run reads the rows as they were before the statement.
  CHECK(db->execute("DELETE FROM g WHERE EXISTS (SELECT 1 FROM g x WHERE x.id = 5) AND v IN (SELECT w FROM h WHERE h.k = g.id)")
            .affected == 3);
  CHECK(ids("SELECT id FROM g") == V{3, 5});
  CHECK(db->execute("UPDATE g SET v = 7 WHERE NOT EXISTS (SELECT 1 FROM h WHERE h.w = g.v)").affected == 2);

  // The outer table read again under an alias, the outer one named by its
  // own name, as PostgreSQL reads it; an UPDATE and a DELETE whose run reads
  // the table they write sees it as it was before the statement.
  (void)db->execute("CREATE TABLE s (id INTEGER PRIMARY KEY, v INTEGER)");
  (void)db->execute("INSERT INTO s VALUES (1, 5), (2, 1), (3, NULL)");
  CHECK(ids("SELECT id FROM s WHERE EXISTS (SELECT 1 FROM s AS x WHERE x.id = s.v)") == V{2});
  CHECK(ids("SELECT id FROM s WHERE NOT EXISTS (SELECT 1 FROM s x WHERE id = s.v)") == V{1, 3});
  // No v is 2 or 3 before the statement: rows 2 and 3 are set, though row
  // 3's run comes after row 2's v became 3.
  CHECK(db->execute("UPDATE s SET v = 3 WHERE NOT EXISTS (SELECT 1 FROM s x WHERE x.v = s.id)").affected == 2);
  CHECK(db->execute("SELECT id, v FROM s ORDER BY id").rows == Grid{{i(1), i(5)}, {i(2), i(3)}, {i(3), i(3)}});
  // A chain 1 -> 2 -> 3 -> 1: each row's v names a row before the
  // statement, row 1 included though deleted before row 3's run.
  (void)db->execute("DELETE FROM s");
  (void)db->execute("INSERT INTO s VALUES (1, 2), (2, 3), (3, 1), (4, NULL)");
  CHECK(db->execute("DELETE FROM s WHERE EXISTS (SELECT 1 FROM s x WHERE x.id = s.v)").affected == 3);
  CHECK(ids("SELECT id FROM s") == V{4});

  // GROUP BY, ORDER BY and LIMIT in the subquery.
  (void)db->execute("CREATE TABLE p (id INTEGER PRIMARY KEY, v INTEGER)");
  (void)db->execute("CREATE TABLE q (k INTEGER PRIMARY KEY, w INTEGER, t TEXT)");
  (void)db->execute("INSERT INTO p VALUES (1, 2), (2, 9), (3, 4), (4, 5)");
  (void)db->execute("INSERT INTO q VALUES (1, 2, 'a'), (2, 2, 'b'), (3, 4, 'a'), (4, 5, 'b'), (5, 7, 'a')");
  // The largest w per t among k >= id: for 1 7 and 5; 2 7 and 5; 3 7 and 5; 4 7 and 5.
  CHECK(ids("SELECT id FROM p WHERE v IN (SELECT MAX(w) FROM q WHERE q.k >= p.id GROUP BY t)") == V{4});
  // The smallest w among k >= id: 2, 2, 4, 5.
  CHECK(ids("SELECT id FROM p WHERE v IN (SELECT w FROM q WHERE q.k >= p.id ORDER BY w LIMIT 1)") == V{1, 3, 4});

  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)} + " " + e.what();
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return std::string{"ran"};
  };
  auto refused = [&](std::string_view sql, std::string_view why) {
    INFO(sql);
    CHECK(state(sql) == "42601 " + std::string{why} + " is not supported yet");
  };
  // Refused, each precisely (#836).
  (void)db->execute("CREATE VIEW qv AS SELECT k, w FROM q");
  (void)db->execute("CREATE VIEW qg AS SELECT w, COUNT(*) AS n FROM q GROUP BY w");
  refused("SELECT id FROM p WHERE EXISTS (SELECT 1 FROM qv WHERE qv.w = p.v)", "a correlated subquery where a view is read");
  refused("SELECT id FROM p WHERE EXISTS (SELECT 1 FROM qg WHERE qg.w = p.v)", "a correlated subquery where a view is read");
  refused("SELECT w FROM qg WHERE EXISTS (SELECT 1 FROM q WHERE q.w = qg.w)", "a correlated subquery where a view is read");
  refused("DELETE FROM p WHERE EXISTS (SELECT 1 FROM qv WHERE qv.w = p.v)", "a correlated subquery where a view is read");
  refused("SELECT id FROM p WHERE EXISTS (SELECT 1 WHERE p.v = 1)", "a correlated subquery without FROM");
  refused("SELECT id FROM p WHERE v IN (SELECT MAX(w) FROM q WHERE q.k <= p.id GROUP BY p.v)",
          "a column of the query around a subquery in its GROUP BY");
  refused("SELECT id FROM p WHERE v IN (SELECT w FROM q WHERE q.k <= p.id ORDER BY p.v)",
          "a column of the query around a subquery in its ORDER BY");
  refused("SELECT id FROM p WHERE v IN (SELECT MAX(w) FROM q WHERE q.k <= p.id HAVING MAX(w) > p.v)",
          "a column of the query around a subquery in its HAVING");
  // The outer table, aliased, read again by its name in the subquery.
  CHECK(state("SELECT id FROM p o WHERE EXISTS (SELECT 1 FROM p WHERE p.v = o.id)")
            .starts_with("42601 a subquery that reads the table of the query around it, naming the outer one by an alias"));
  // A join in the subquery, the outer table among its tables under an alias.
  refused("SELECT id FROM p WHERE EXISTS (SELECT 1 FROM q JOIN p x ON x.id = q.k WHERE q.w = p.v)",
          "a correlated subquery of a join");
  // A pair the layer does not compare: the outer column named as such.
  (void)db->execute("CREATE TABLE n (k INTEGER PRIMARY KEY, d NUMERIC(10,2))");
  refused("SELECT id FROM p WHERE EXISTS (SELECT 1 FROM n WHERE n.d = p.v)",
          "comparing d (numeric(10,2)) with the outer column v (integer) in a correlated subquery");
  // A name the subquery's alias hides and no outer table has: 42P01.
  CHECK(state("SELECT id FROM p WHERE EXISTS (SELECT 1 FROM q AS x WHERE q.k = 1)").starts_with("42P01"));
  // In a procedure's statement.
  (void)db->execute(R"(CREATE PROCEDURE cp(x INTEGER) LANGUAGE plpgsql AS $$
    BEGIN UPDATE p SET v = x WHERE EXISTS (SELECT 1 FROM q WHERE q.w = p.v); END $$)");
  auto call_state = [&]() -> std::string {
    try {
      (void)db->call("cp", {i(1)});
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)} + " " + e.what();
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return std::string{"ran"};
  };
  CHECK(call_state() == "42601 a correlated subquery in a procedure's statement is not supported yet");
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
  // A column list renames the first columns: the old name is gone.
  auto renamed = db->execute("SELECT x.a, b FROM g AS x (a, b) ORDER BY a");
  CHECK(renamed.columns == std::vector<std::string>{"a", "b"});
  CHECK(renamed.rows == Grid{{i(1), i(30)}, {i(2), i(10)}, {i(3), i(20)}});
  CHECK(db->execute("SELECT * FROM g AS x (a)").columns == std::vector<std::string>{"a", "v"});
  CHECK(state("SELECT x.id FROM g AS x (a, b)") == "42703");
  CHECK(state("SELECT * FROM g AS x (a, b, c)") == "42P10");
  CHECK(state("SELECT * FROM g AS x (a, a)") == "42701");
}

TEST_CASE("embedded API: SELECT * and table.* expand to the FROM tables' columns, in order", "[sql][api]") {
  sql_test::TempDir dir{"api_star"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE g (id INTEGER PRIMARY KEY, v INTEGER)");
  (void)db->execute("CREATE TABLE h (id INTEGER PRIMARY KEY, w INTEGER)");
  (void)db->execute("CREATE TABLE np (p INTEGER, q INTEGER)");
  (void)db->execute("INSERT INTO g VALUES (1, 10)");
  (void)db->execute("INSERT INTO g VALUES (2, 20)");
  (void)db->execute("INSERT INTO h VALUES (1, 7)");
  (void)db->execute("INSERT INTO np VALUES (3, 4)");
  auto all = db->execute("SELECT * FROM g ORDER BY id");
  CHECK(all.columns == std::vector<std::string>{"id", "v"});
  CHECK(all.rows == Grid{{i(1), i(10)}, {i(2), i(20)}});
  CHECK(db->execute("SELECT x.*, v FROM g AS x WHERE x.v > 10").rows == Grid{{i(2), i(20), i(20)}});
  CHECK(db->execute("SELECT * FROM g JOIN h ON g.id = h.id").columns == std::vector<std::string>{"id", "v", "id", "w"});
  // A USING column is named once, first.
  auto using_star = db->execute("SELECT * FROM g JOIN h USING (id)");
  CHECK(using_star.columns == std::vector<std::string>{"id", "v", "w"});
  CHECK(using_star.rows == Grid{{i(1), i(10), i(7)}});
  CHECK(db->execute("SELECT h.*, g.v FROM g JOIN h ON g.id = h.id").rows == Grid{{i(1), i(7), i(10)}});
  CHECK(db->execute("SELECT DISTINCT * FROM np").rows == Grid{{i(3), i(4)}});
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
  CHECK(state("SELECT *") == "42601");
  CHECK(state("SELECT nope.* FROM g") == "42P01");
  CHECK(state("SELECT g.* FROM g x") == "42P01");
  CHECK(state("SELECT id FROM g WHERE * = 1") == "42601");
}

TEST_CASE("embedded API: * and / of constants fold as PostgreSQL's operators type them", "[sql][api]") {
  sql_test::TempDir dir{"api_mul_div"};
  auto db = Database::open(dir.path());
  CHECK(db->execute("SELECT 6 * 7, 7 / 2, -7 / 2, 2 + 3 * 4, 8 / 2 / 2").rows ==
        Grid{{i(42), i(3), i(-3), i(14), i(2)}});
  CHECK(db->execute("SELECT 2147483648 * 2").rows == Grid{{i(4294967296)}});  // past INTEGER: BIGINT
  CHECK(db->execute("SELECT NULL * 2, NULL / 0, 1 / NULL").rows == Grid{{Value{}, Value{}, Value{}}});
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
  CHECK(state("SELECT 1 / 0") == "22012");
  CHECK(state("SELECT 1.5 / 0") == "22012");
  CHECK(state("SELECT 2147483647 * 2") == "22003");
  CHECK(state("SELECT 9223372036854775807 * 2") == "22003");
  CHECK(state("SELECT (-9223372036854775807 - 1) / -1") == "22003");
  CHECK(state("SELECT 1 % 0") == "22012");
}

TEST_CASE("embedded API: a numeric quotient of constants has PostgreSQL's scale", "[sql][api]") {
  sql_test::TempDir dir{"api_numeric_quotient"};
  auto db = Database::open(dir.path());
  // select_div_scale: 16 significant digits, more where the dividend is the
  // smaller by a base-10000 digit, and the finer operand's scale at least.
  auto q = db->execute("SELECT 5.0 / 2, 10.0 / 4, 7.5 / 0.5, 100.0 / 3, -5.0 / 2, 5.0 / -2, 2.25 / 1.5, 3 / 2.0");
  CHECK(q.rows == Grid{{i(25000000000000000), i(25000000000000000), i(150000000000000000), i(333333333333333333),
                        i(-25000000000000000), i(-25000000000000000), i(15000000000000000), i(15000000000000000)}});
  CHECK(q.types[0] == SqlType{SqlKind::Decimal, 0, 0, 16});
  CHECK(q.types[3] == SqlType{SqlKind::Decimal, 0, 0, 16});
  // Rounded half away from zero at the scale.
  CHECK(db->execute("SELECT 20.0 / 3, -20.0 / 3, 10.0 / 3").rows ==
        Grid{{i(66666666666666667), i(-66666666666666667), i(33333333333333333)}});
  // A larger dividend leaves fewer places: 12 here.
  auto big = db->execute("SELECT 100000.0 / 3");
  CHECK(big.rows == Grid{{i(33333333333333333)}});
  CHECK(big.types[0] == SqlType{SqlKind::Decimal, 0, 0, 12});
  CHECK(db->execute("SELECT NULL / 1.5, 1.5 / NULL").rows == Grid{{Value{}, Value{}}});
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
  CHECK(state("SELECT 1.5 / 0") == "22012");
  // A scale or a quotient past the 18 digits a DECIMAL holds is not taken.
  CHECK(state("SELECT 1.0 / 3") == "42601");  // 20 digits of scale
  CHECK(state("SELECT 1 / 2.0") == "42601");
  CHECK(state("SELECT 0.0 / 2") == "42601");
}

TEST_CASE("embedded API: expressions over columns in the select list", "[sql][api]") {
  sql_test::TempDir dir{"api_column_expressions"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto yes = Value{true};
  auto no = Value{false};
  auto null = Value{};
  (void)db->execute(
      "CREATE TABLE ce (id INTEGER PRIMARY KEY, i INTEGER, s SMALLINT, b BIGINT, d DECIMAL(6,2), z INTEGER, t TEXT, f DOUBLE PRECISION)");
  (void)db->execute("INSERT INTO ce VALUES (1, 1, 100, 9000000000000000000, 2.50, NULL, 'a', 1.5)");
  (void)db->execute("INSERT INTO ce VALUES (2, 2, 200, 5, 2.50, 0, 'b', 2.5)");
  (void)db->execute("INSERT INTO ce VALUES (3, 3, 300, 7, 10.00, 4, 'c', 0.5)");
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

  // + - * / with PostgreSQL's result types; / of integers truncates.
  auto ints = db->execute("SELECT i + i, i - 1, i * i, i / 2, 7 / i FROM ce ORDER BY id");
  CHECK(ints.rows == Grid{{i(2), i(0), i(1), i(0), i(7)}, {i(4), i(1), i(4), i(1), i(3)}, {i(6), i(2), i(9), i(1), i(2)}});
  CHECK(ints.columns == std::vector<std::string>{"?column?", "?column?", "?column?", "?column?", "?column?"});
  CHECK(ints.types == std::vector<SqlType>(5, SqlType{SqlKind::Integer}));
  CHECK(db->execute("SELECT s * s FROM ce WHERE id = 1").types[0] == SqlType{SqlKind::SmallInt});
  CHECK(db->execute("SELECT s * i FROM ce WHERE id = 1").types[0] == SqlType{SqlKind::Integer});
  CHECK(db->execute("SELECT i * b FROM ce WHERE id = 2").types[0] == SqlType{SqlKind::BigInt});
  CHECK(db->execute("SELECT b + 1 FROM ce ORDER BY id").rows == Grid{{i(9000000000000000001)}, {i(6)}, {i(8)}});

  // DECIMALs: + and - at the finer scale, * at the sum; an integer meets them.
  auto dec = db->execute("SELECT d + d, d + i, d - i, d * d, d * i FROM ce ORDER BY id");
  CHECK(dec.types == std::vector<SqlType>{{SqlKind::Decimal, 0, 0, 2}, {SqlKind::Decimal, 0, 0, 2}, {SqlKind::Decimal, 0, 0, 2},
                                          {SqlKind::Decimal, 0, 0, 4}, {SqlKind::Decimal, 0, 0, 2}});
  CHECK(dec.rows == Grid{{i(500), i(350), i(150), i(62500), i(250)},
                         {i(500), i(450), i(50), i(62500), i(500)},
                         {i(2000), i(1300), i(700), i(1000000), i(3000)}});

  // NULL in, NULL out; an operand a NULL column leaves the other unevaluated.
  CHECK(db->execute("SELECT z + 1, z * 2, 1 - z FROM ce ORDER BY id").rows ==
        Grid{{null, null, null}, {i(1), i(0), i(1)}, {i(5), i(8), i(-3)}});

  // 22012 for a zero divisor, a column or a parameter; a NULL dividend raises none.
  CHECK(state("SELECT i / z FROM ce") == "22012");
  CHECK(state("SELECT d / 1 FROM ce") == "42601");  // a numeric quotient: its scale follows the values
  CHECK(db->execute("SELECT i / z FROM ce WHERE id = 1").rows == Grid{{null}});
  CHECK(db->execute("SELECT i / z FROM ce WHERE id = 3").rows == Grid{{i(0)}});
  {
    const auto& st = db->prepare("SELECT id, i / $1 FROM ce WHERE id < 3 ORDER BY id");
    CHECK(db->execute(st, {i(1)}).rows == Grid{{i(1), i(1)}, {i(2), i(2)}});
    try {
      (void)db->execute(st, {i(0)});
      FAIL("a zero divisor ran");
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "22012");
    }
  }
  // AND and OR evaluate an operand only while the result is open, so a guard works.
  CHECK(db->execute("SELECT z <> 0 AND i / z > 0 FROM ce ORDER BY id").rows == Grid{{null}, {no}, {no}});
  CHECK(db->execute("SELECT z = 0 OR i / z > 0 FROM ce WHERE id > 1 ORDER BY id").rows == Grid{{yes}, {no}});

  // Range: a SMALLINT or INTEGER result is checked against its type, BIGINT's against its own.
  CHECK(state("SELECT s * s FROM ce") == "22003");
  CHECK(state("SELECT i * 1000000000 FROM ce") == "22003");
  CHECK(state("SELECT b * 2 FROM ce") == "22003");
  CHECK(state("SELECT b + b FROM ce WHERE id = 1") == "22003");
  CHECK(db->execute("SELECT s * 100 FROM ce WHERE id = 1").rows == Grid{{i(10000)}});

  // Comparisons as values: three-valued; numbers meet at the finer scale.
  CHECK(db->execute("SELECT i < 2, i <= 2, i = 2, i <> 2, i >= 2, i > 2 FROM ce ORDER BY id").rows ==
        Grid{{yes, yes, no, yes, no, no}, {no, yes, yes, no, yes, no}, {no, no, no, yes, yes, yes}});
  CHECK(db->execute("SELECT i < z, z = z, z <> 0 FROM ce ORDER BY id").rows ==
        Grid{{null, null, null}, {no, yes, no}, {yes, yes, yes}});
  CHECK(db->execute("SELECT d < i, d = 2.5, d > 2 FROM ce ORDER BY id").rows ==
        Grid{{no, yes, yes}, {no, yes, yes}, {no, no, yes}});
  CHECK(db->execute("SELECT t = 'b', t < 'b', t >= 'b' FROM ce ORDER BY id").rows ==
        Grid{{no, yes, no}, {yes, no, yes}, {no, no, yes}});
  CHECK(db->execute("SELECT i + 1 = 3 AS hit FROM ce ORDER BY id").columns == std::vector<std::string>{"hit"});
  CHECK(db->execute("SELECT i < (SELECT MAX(id) FROM ce), i = (SELECT MIN(id) FROM ce) FROM ce ORDER BY id").rows ==
        Grid{{yes, yes}, {yes, no}, {no, no}});
  CHECK(db->execute("SELECT i < (SELECT MAX(id) FROM ce WHERE id > 99) FROM ce WHERE id = 1").rows == Grid{{null}});
  CHECK(state("SELECT i < t FROM ce") == "42883");
  CHECK(state("SELECT i + t FROM ce") == "42883");
  CHECK(db->execute(db->prepare("SELECT i + $1 < 4 FROM ce WHERE id = 1"), {i(2)}).rows == Grid{{yes}});

  // IS [NOT] NULL of an expression, and AND, OR, NOT in SQL's logic.
  CHECK(db->execute("SELECT z IS NULL, z IS NOT NULL, z + 1 IS NULL, i + i IS NOT NULL FROM ce ORDER BY id").rows ==
        Grid{{yes, no, yes, yes}, {no, yes, no, yes}, {no, yes, no, yes}});
  CHECK(db->execute("SELECT NOT (i > 1), i > 1 AND z IS NULL, i > 2 OR z IS NULL FROM ce ORDER BY id").rows ==
        Grid{{yes, no, yes}, {no, no, no}, {no, no, yes}});
  CHECK(db->execute("SELECT (z > 0) AND (z < 5), (z > 0) OR (z < 5), NOT (z > 0) FROM ce WHERE id = 1").rows ==
        Grid{{null, null, null}});
  CHECK(db->execute("SELECT (z > 0) AND false, (z > 0) OR true FROM ce WHERE id = 1").rows == Grid{{no, yes}});
  CHECK(state("SELECT i AND true FROM ce") == "42804");
  CHECK(state("SELECT NOT i FROM ce") == "42804");
  // Constants fold: IS NULL, NOT, AND, OR of them.
  CHECK(db->execute("SELECT NULL IS NULL, 1 IS NOT NULL, NOT TRUE, TRUE AND NULL, FALSE AND NULL, TRUE OR NULL, NULL OR NULL").rows ==
        Grid{{yes, yes, no, null, no, yes, null}});

  // Over a join, below a sort, from a covering index, in a set operation, a view.
  (void)db->execute("CREATE TABLE ce2 (id INTEGER PRIMARY KEY, w INTEGER)");
  (void)db->execute("INSERT INTO ce2 VALUES (1, 10)");
  (void)db->execute("INSERT INTO ce2 VALUES (3, 30)");
  CHECK(db->execute("SELECT ce.i + ce2.w, ce.i * ce2.w < 50 FROM ce JOIN ce2 ON ce.id = ce2.id ORDER BY ce2.w").rows ==
        Grid{{i(11), yes}, {i(33), no}});
  CHECK(db->execute("SELECT i * 2 FROM ce ORDER BY i DESC").rows == Grid{{i(6)}, {i(4)}, {i(2)}});
  CHECK(db->execute("SELECT i * 2 FROM ce ORDER BY id DESC LIMIT 2").rows == Grid{{i(6)}, {i(4)}});
  (void)db->execute("CREATE INDEX ce_i ON ce (i)");
  CHECK(db->execute("SELECT i + 1 FROM ce WHERE i >= 2 ORDER BY i").rows == Grid{{i(3)}, {i(4)}});
  CHECK(db->execute("SELECT i * 2 FROM ce UNION ALL SELECT w FROM ce2").rows.size() == 5);
  (void)db->execute("CREATE VIEW cv AS SELECT id, i * 2 AS dbl FROM ce");
  CHECK(db->execute("SELECT dbl FROM cv WHERE id = 3").rows == Grid{{i(6)}});

  // What is still refused.
  CHECK(state("SELECT f * 2 FROM ce") == "42601");               // REAL and DOUBLE PRECISION arithmetic
  CHECK(state("SELECT f < 1 FROM ce") == "42601");
  CHECK(state("SELECT -f < 1 FROM ce") == "42601");
  CHECK(state("SELECT abs(f) < 1 FROM ce") == "42601");  // a comparison of floats as a value
  CHECK(state("SELECT SUM(i) + 1 FROM ce") == "42601");
  CHECK(state("SELECT i + 1, COUNT(*) FROM ce GROUP BY i") == "42601");
  CHECK(state("SELECT $1 + 1 FROM ce") == "42601");
  CHECK(state("UPDATE ce SET i = i * 2") == "42601");
  CHECK(state("SELECT id FROM ce WHERE i * 2 = 4") == "42601");
}


TEST_CASE("embedded API: DATE - DATE is the INTEGER count of days between them", "[sql][api]") {
  sql_test::TempDir dir{"api_date_difference"};
  auto db = Database::open(dir.path());
  auto null = Value{};
  constexpr std::int64_t kMinDate = -2'440'588;     // 4714-11-24 BC, the type's first day
  constexpr std::int64_t kMaxDate = 2'145'042'905;  // 5874897-12-31, its last
  (void)db->execute("CREATE TABLE dd (id INTEGER PRIMARY KEY, d DATE, e DATE, ts TIMESTAMP, t TIME, n INTEGER)");
  constexpr auto kIns = "INSERT INTO dd VALUES ($1, $2, $3, $4, $5, $6)";
  (void)db->execute(kIns, {i(1), i(20'751), i(-1), i(0), i(0), i(1)});  // 2026-10-25, 1969-12-31
  (void)db->execute(kIns, {i(2), i(kMinDate), i(kMaxDate), null, null, null});
  (void)db->execute(kIns, {i(3), null, i(0), null, null, null});
  auto state = [&](std::string_view sql, std::vector<Value> params = {}) -> std::string {
    try {
      (void)db->execute(db->prepare(sql), params);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return std::string{"ran"};
  };

  // Of columns: before 1970, at the type's ends both ways, NULL.
  auto cols = db->execute("SELECT d - e, e - d, d - DATE '1970-01-01' FROM dd ORDER BY id");
  CHECK(cols.rows == Grid{{i(20'752), i(-20'752), i(20'751)},
                          {i(-2'147'483'493), i(2'147'483'493), i(kMinDate)},
                          {null, null, null}});
  CHECK(cols.types == std::vector<SqlType>(3, SqlType{SqlKind::Integer}));
  CHECK(db->execute("SELECT d - e - n, (d - e) * 2 FROM dd WHERE id = 1").rows == Grid{{i(20'751), i(41'504)}});

  // An untyped $1 or literal beside a DATE is read as one, as PostgreSQL resolves it.
  const auto& by_param = db->prepare("SELECT d - $1, $1 - e FROM dd WHERE id = $2");
  CHECK(db->execute(by_param, {i(0), i(1)}).rows == Grid{{i(20'751), i(1)}});
  CHECK(db->execute(by_param, {i(kMaxDate), i(2)}).rows == Grid{{i(-2'147'483'493), i(0)}});
  CHECK(db->execute(by_param, {null, i(1)}).rows == Grid{{null, null}});
  CHECK(db->execute("SELECT d - '1969-12-31', d - NULL FROM dd WHERE id = 1").rows == Grid{{i(20'752), null}});
  CHECK(state("SELECT d - 'x' FROM dd") == "22007");

  // Constants fold.
  auto folded = db->execute("SELECT DATE '5874897-12-31' - DATE '4714-11-24 BC', DATE '1969-12-31' - DATE '1970-01-01', "
                            "DATE '2026-01-01' - '2020-01-01', '2020-01-01' - DATE '2026-01-01', DATE '2026-01-01' - NULL");
  CHECK(folded.rows == Grid{{i(2'147'483'493), i(-1), i(2'192), i(-2'192), null}});
  CHECK(folded.types == std::vector<SqlType>(5, SqlType{SqlKind::Integer}));

  // No such operator in PostgreSQL: 42883; time + time is ambiguous there: 42725.
  for (const auto* sql : {"SELECT d + e FROM dd", "SELECT d * e FROM dd", "SELECT d - 1.5 FROM dd", "SELECT n - d FROM dd",
                          "SELECT DATE '2026-01-01' + DATE '2026-01-01'", "SELECT DATE '2026-01-01' - 5::bigint"}) {
    CHECK(state(sql) == "42883");
  }
  CHECK(state("SELECT t + t FROM dd") == "42725");
  CHECK(state("SELECT TIME '10:00' + TIME '10:00'") == "42725");
  // Operators PostgreSQL has and the layer does not yet: refused, not 42883.
  for (const auto* sql : {"SELECT d + n FROM dd", "SELECT d - 1 FROM dd", "SELECT 1 + d FROM dd", "SELECT d - ts FROM dd",
                          "SELECT ts - ts FROM dd", "SELECT t - t FROM dd", "SELECT d + t FROM dd", "SELECT t * 2 FROM dd",
                          "SELECT DATE '2026-01-01' + 1"}) {
    CHECK(state(sql) == "42601");
  }

  // A $n, an unknown literal or NULL beside a date or time value under +, *
  // or /: decided as PostgreSQL resolves the operator, before the untyped
  // operand is read as the other's type, so a literal that is not a valid
  // DATE or TIME answers the same as one that is.
  for (const auto* sql : {"SELECT d + '1' FROM dd", "SELECT d + '2020-01-01' FROM dd", "SELECT '2020-01-01' + d FROM dd",
                          "SELECT d + NULL FROM dd", "SELECT DATE '2026-01-01' + NULL", "SELECT DATE '2026-01-01' + '1'"}) {
    CHECK(state(sql) == "42725");  // date + unknown: date + integer, + interval, + time
  }
  for (const auto* sql : {"SELECT d * '2' FROM dd", "SELECT d * NULL FROM dd", "SELECT d / '2' FROM dd", "SELECT ts * '2' FROM dd",
                          "SELECT '2' / t FROM dd", "SELECT TIMESTAMP '2026-01-01 00:00' / NULL"}) {
    CHECK(state(sql) == "42883");
  }
  for (const auto* sql : {"SELECT t + '1' FROM dd", "SELECT t + '01:00' FROM dd", "SELECT t * '2' FROM dd", "SELECT t / '2' FROM dd",
                          "SELECT '2' * t FROM dd", "SELECT ts + '1' FROM dd", "SELECT TIME '10:00' + NULL",
                          "SELECT TIME '10:00' * '2'"}) {
    CHECK(state(sql) == "42601");  // PostgreSQL runs these: time + interval, time scaled, timestamp + interval
  }
  CHECK(state("SELECT d + $1 FROM dd", {i(1)}) == "42725");
  CHECK(state("SELECT d * $1 FROM dd", {i(1)}) == "42883");
  CHECK(state("SELECT $1 / t FROM dd", {i(1)}) == "42883");
  CHECK(state("SELECT t * $1 FROM dd", {i(1)}) == "42601");
}

TEST_CASE("embedded API: EXTRACT of a DATE, TIME or TIMESTAMP is PostgreSQL's numeric of the field", "[sql][api]") {
  sql_test::TempDir dir{"api_extract"};
  auto db = Database::open(dir.path());
  auto null = Value{};
  (void)db->execute("CREATE TABLE ev (id INTEGER PRIMARY KEY, d DATE, ts TIMESTAMP, tz TIMESTAMPTZ, t TIME, n INTEGER)");
  (void)db->execute("INSERT INTO ev VALUES (1, '2026-10-25', '2026-10-25 12:34:56.789', '2026-10-25 12:00:00+02', '12:34:56.5', 1)");
  (void)db->execute("INSERT INTO ev VALUES (2, '0044-03-15 BC', '0044-03-15 01:02:03 BC', NULL, '24:00:00', 2)");
  (void)db->execute("INSERT INTO ev VALUES (3, NULL, NULL, NULL, NULL, 3)");
  auto state = [&](std::string_view sql, std::vector<Value> params = {}) -> std::string {
    try {
      (void)db->execute(db->prepare(sql), params);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return std::string{"ran"};
  };

  // A DATE's fields, as PostgreSQL 17 gives them (before 1 AD too), NULL of NULL.
  auto dates = db->execute(
      "SELECT EXTRACT(YEAR FROM d), EXTRACT(MONTH FROM d), EXTRACT(DAY FROM d), EXTRACT(DOW FROM d), EXTRACT(DOY FROM d), "
      "EXTRACT(QUARTER FROM d), EXTRACT(WEEK FROM d), EXTRACT(ISOYEAR FROM d), EXTRACT(ISODOW FROM d), "
      "EXTRACT(CENTURY FROM d), EXTRACT(MILLENNIUM FROM d), EXTRACT(DECADE FROM d), EXTRACT(JULIAN FROM d), "
      "EXTRACT(EPOCH FROM d) FROM ev ORDER BY id");
  CHECK(dates.rows == Grid{{i(2026), i(10), i(25), i(0), i(298), i(4), i(43), i(2026), i(7), i(21), i(3), i(202), i(2'461'339),
                            i(1'792'886'400)},
                           {i(-44), i(3), i(15), i(5), i(74), i(1), i(11), i(-44), i(5), i(-1), i(-1), i(-5), i(1'705'428),
                            i(-63'517'824'000)},
                           std::vector<Value>(14, null)});
  CHECK(dates.types == std::vector<SqlType>(14, SqlType{SqlKind::Decimal, 0, 0, 0}));
  CHECK(dates.columns == std::vector<std::string>(14, "extract"));

  // Time of day: SECOND at scale 6, MILLISECONDS at 3, MICROSECONDS whole;
  // a TIMESTAMPTZ in UTC.
  auto times = db->execute(
      "SELECT EXTRACT(HOUR FROM ts), EXTRACT(MINUTE FROM ts), EXTRACT(SECOND FROM ts), EXTRACT(MILLISECONDS FROM ts), "
      "EXTRACT(MICROSECONDS FROM ts), EXTRACT(HOUR FROM tz), EXTRACT(TIMEZONE FROM tz), EXTRACT(HOUR FROM t), "
      "EXTRACT(EPOCH FROM t) FROM ev WHERE id = 1");
  CHECK(times.rows == Grid{{i(12), i(34), i(56'789'000), i(56'789'000), i(56'789'000), i(10), i(0), i(12), i(45'296'500'000)}});
  CHECK(times.types == std::vector<SqlType>{SqlType{SqlKind::Decimal, 0, 0, 0}, SqlType{SqlKind::Decimal, 0, 0, 0},
                                            SqlType{SqlKind::Decimal, 0, 0, 6}, SqlType{SqlKind::Decimal, 0, 0, 3},
                                            SqlType{SqlKind::Decimal, 0, 0, 0}, SqlType{SqlKind::Decimal, 0, 0, 0},
                                            SqlType{SqlKind::Decimal, 0, 0, 0}, SqlType{SqlKind::Decimal, 0, 0, 0},
                                            SqlType{SqlKind::Decimal, 0, 0, 6}});
  CHECK(db->execute("SELECT EXTRACT(HOUR FROM t), EXTRACT(YEAR FROM ts) FROM ev WHERE id = 2").rows == Grid{{i(24), i(-44)}});

  // Any spelling PostgreSQL's units take; a constant folds to the same value;
  // a typed $n and the clock are values like a column.
  CHECK(db->execute("SELECT EXTRACT('Years' FROM d), EXTRACT(\"MON\" FROM d), EXTRACT(microsecondsxyz FROM ts), "
                    "EXTRACT(msecond FROM ts), EXTRACT(usecond FROM ts) FROM ev WHERE id = 1")
            .rows == Grid{{i(2026), i(10), i(56'789'000), i(56'789'000), i(56'789'000)}});
  CHECK(db->execute("SELECT EXTRACT(WEEK FROM DATE '2026-10-25'), EXTRACT(SECOND FROM TIMESTAMP '2026-10-25 12:34:56.789'), "
                    "EXTRACT(FOO FROM CAST(NULL AS DATE))")
            .rows == Grid{{i(43), i(56'789'000), null}});
  CHECK(db->execute(db->prepare("SELECT EXTRACT(DOY FROM $1::date)"), {i(365)}).rows == Grid{{i(1)}});  // 1971-01-01
  CHECK(db->execute("SELECT EXTRACT(YEAR FROM CURRENT_DATE) > 2000, EXTRACT(YEAR FROM d) + n FROM ev WHERE id = 1").rows ==
        Grid{{Value{true}, i(2027)}});

  // A unit no type has, one the type lacks; an operand of no type or another type.
  CHECK(state("SELECT EXTRACT(FOO FROM d) FROM ev") == "22023");
  CHECK(state("SELECT EXTRACT(HOUR FROM d) FROM ev") == "0A000");
  CHECK(state("SELECT EXTRACT(NOW FROM d) FROM ev") == "0A000");
  CHECK(state("SELECT EXTRACT(YEAR FROM t) FROM ev") == "0A000");
  CHECK(state("SELECT EXTRACT(TIMEZONE FROM ts) FROM ev") == "0A000");
  CHECK(state("SELECT EXTRACT(HOUR FROM DATE '2026-01-01')") == "0A000");
  CHECK(state("SELECT EXTRACT(YEAR FROM $1)", {i(1)}) == "42725");
  CHECK(state("SELECT EXTRACT(YEAR FROM '2026-01-01')") == "42725");
  CHECK(state("SELECT EXTRACT(YEAR FROM NULL)") == "42725");
  CHECK(state("SELECT EXTRACT(YEAR FROM n) FROM ev") == "42883");
  CHECK(state("SELECT EXTRACT(YEAR FROM 1.5)") == "42883");
  // Not built: EPOCH and JULIAN of a timestamp, past 18 digits or a fraction of a day.
  CHECK(state("SELECT EXTRACT(EPOCH FROM ts) FROM ev") == "42601");
  CHECK(state("SELECT EXTRACT(JULIAN FROM TIMESTAMP '2026-01-01 00:00')") == "42601");
}

TEST_CASE("embedded API: a database written before #780's row and key format is refused when it opens", "[sql][api]") {
  sql_test::TempDir dir{"api_old_format"};
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE o (k INTEGER PRIMARY KEY, v TEXT)");
    (void)db->execute("INSERT INTO o VALUES (1, 'a')");
    db->close();
  }
  // The table's record, `01 02 <id>`, as format 6 wrote it: its format field
  // is all an earlier build's record need differ in for this build to refuse it.
  {
    auto engine = bytecask::DB::open(dir.path());
    const Bytes tables{std::byte{0x01}, std::byte{0x02}};
    std::vector<std::pair<Bytes, Bytes>> records;
    for (const auto& e : engine.iter_from({}, tables)) {
      if (e.key.size() < tables.size() || !std::ranges::equal(e.key.first(tables.size()), tables)) break;
      records.emplace_back(Bytes(e.key.begin(), e.key.end()), Bytes(e.value.begin(), e.value.end()));
    }
    REQUIRE(records.size() == 1);
    auto& [key, record] = records[0];
    REQUIRE(record[0] == std::byte{7});
    record[0] = std::byte{6};
    engine.put({}, key, record);
    engine.close();
  }
  try {
    (void)Database::open(dir.path());
    FAIL("a format-6 database opened");
  } catch (const std::runtime_error& e) {
    CHECK(std::string{e.what()}.find("earlier SQL-layer build") != std::string::npos);
  }
}

TEST_CASE("embedded API: a narrow integer key compared with a wider subquery's value past its range is a filter, never a truncated key",
          "[sql][api][subquery]") {
  sql_test::TempDir dir{"api_sub_narrow"};
  auto db = Database::open(dir.path());
  // INTEGER and SMALLINT keys are 4 and 2 bytes (types_design.md, *Key
  // encoding*); a BIGINT subquery's value may be past either.
  (void)db->execute("CREATE TABLE n (i INTEGER PRIMARY KEY, s SMALLINT NOT NULL)");
  (void)db->execute("CREATE INDEX n_s ON n (s)");
  (void)db->execute("CREATE TABLE w (k INTEGER PRIMARY KEY, big BIGINT, mid INTEGER)");
  for (auto [id, sv] : {std::pair<std::int64_t, std::int64_t>{-2147483648, -32768}, {-1, -1}, {5, 0}, {2147483647, 32767}}) {
    (void)db->execute("INSERT INTO n VALUES ($1, $2)", {i(id), i(sv)});
  }
  for (const auto* row : {"(1, 3000000000, 40000)", "(2, -3000000000, -40000)", "(3, 5, 5)"}) {
    (void)db->execute(std::string{"INSERT INTO w VALUES "} + row);
  }
  auto ids = [&](std::string_view sql) {
    std::vector<std::int64_t> out;
    for (const auto& r : db->execute(sql).rows) out.push_back(std::get<std::int64_t>(r[0]));
    std::ranges::sort(out);
    return out;
  };
  using V = std::vector<std::int64_t>;
  const V all{-2147483648, -1, 5, 2147483647};
  // Past INTEGER above: below it is every row, at or above it none.
  CHECK(ids("SELECT i FROM n WHERE i < (SELECT MAX(big) FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE i <= (SELECT MAX(big) FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE i > (SELECT MAX(big) FROM w)").empty());
  CHECK(ids("SELECT i FROM n WHERE i >= (SELECT MAX(big) FROM w)").empty());
  CHECK(ids("SELECT i FROM n WHERE i = (SELECT MAX(big) FROM w)").empty());
  // Past INTEGER below.
  CHECK(ids("SELECT i FROM n WHERE i > (SELECT MIN(big) FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE i >= (SELECT MIN(big) FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE i < (SELECT MIN(big) FROM w)").empty());
  CHECK(ids("SELECT i FROM n WHERE i <= (SELECT MIN(big) FROM w)").empty());
  // Inside the range, the wider value still finds its row.
  CHECK(ids("SELECT i FROM n WHERE i = (SELECT big FROM w WHERE k = 3)") == V{5});
  // A SMALLINT index part and an INTEGER subquery past it.
  CHECK(ids("SELECT i FROM n WHERE s < (SELECT MAX(mid) FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE s > (SELECT MAX(mid) FROM w)").empty());
  CHECK(ids("SELECT i FROM n WHERE s >= (SELECT MIN(mid) FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE s = (SELECT mid FROM w WHERE k = 3)").empty());
  // ANY and ALL compare against the subquery's extremes, past the range too.
  CHECK(ids("SELECT i FROM n WHERE i < ANY (SELECT big FROM w)") == all);
  CHECK(ids("SELECT i FROM n WHERE i > ALL (SELECT big FROM w WHERE big < 0)") == all);
  CHECK(ids("SELECT i FROM n WHERE i > ANY (SELECT big FROM w WHERE big > 10)").empty());
  // A subquery of the key's own width still keys the lookup.
  CHECK(ids("SELECT i FROM n WHERE i = (SELECT mid FROM w WHERE k = 3)") == V{5});
  CHECK(db->prepare("SELECT i FROM n WHERE i = (SELECT mid FROM w WHERE k = 3)").plan_source().find("bc_get(") !=
        std::string::npos);
}

TEST_CASE("embedded API: a $n only a subquery names is one of the statement's parameters", "[sql][api][subquery]") {
  sql_test::TempDir dir{"api_sub_param"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t0 (c0 BIGINT NOT NULL, c1 BIGINT NOT NULL, PRIMARY KEY (c0))");
  auto ids = [&](std::string_view sql, std::vector<Value> params) {
    std::vector<std::int64_t> out;
    for (const auto& r : db->execute(sql, std::move(params)).rows) out.push_back(std::get<std::int64_t>(r[0]));
    return out;
  };
  using V = std::vector<std::int64_t>;
  const auto* only_sub = "SELECT c0 FROM t0 WHERE c0 IN (SELECT c0 FROM t0 WHERE c0 = $1) ORDER BY c0";
  // Each $n takes its own slot, never one of the constants' after it.
  CHECK(ids(only_sub, {i(0)}).empty());
  for (const auto* row : {"(0, 10)", "(1, 20)", "(2, 10)", "(3, 30)"}) {
    (void)db->execute(std::string{"INSERT INTO t0 VALUES "} + row);
  }
  CHECK(ids(only_sub, {i(0)}) == V{0});
  CHECK(ids(only_sub, {i(9)}).empty());
  // A constant beside the $n in the subquery's WHERE.
  const auto* with_const = "SELECT c0 FROM t0 WHERE c0 IN (SELECT c0 FROM t0 WHERE c1 = 10 AND c0 > $1) ORDER BY c0";
  CHECK(ids(with_const, {i(0)}) == V{2});
  CHECK(ids(with_const, {i(2)}).empty());
  // A $n in the outer WHERE too, numbered before and after the subquery's.
  CHECK(ids("SELECT c0 FROM t0 WHERE c0 > $1 AND c0 IN (SELECT c0 FROM t0 WHERE c1 = $2) ORDER BY c0", {i(0), i(10)}) ==
        V{2});
  CHECK(ids("SELECT c0 FROM t0 WHERE c0 IN (SELECT c0 FROM t0 WHERE c1 = $2) AND c0 < $1 ORDER BY c0", {i(3), i(10)}) ==
        V{0, 2});
  CHECK(ids("SELECT c0 FROM t0 WHERE c1 = (SELECT c1 FROM t0 WHERE c0 = $1) AND c0 <> 5 ORDER BY c0", {i(2)}) == V{0, 2});
  CHECK(ids("SELECT c0 FROM t0 WHERE c1 = (SELECT c1 FROM t0 WHERE c0 = $1) AND c0 <> 5 ORDER BY c0", {i(7)}).empty());
}

TEST_CASE("embedded API: CASE, COALESCE and NULLIF over columns, parameters and subqueries", "[sql][api]") {
  sql_test::TempDir dir{"api_case_columns"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto null = Value{};
  (void)db->execute(
      "CREATE TABLE cc (id INTEGER PRIMARY KEY, i INTEGER, s SMALLINT, d DECIMAL(6,2), z INTEGER, t TEXT, f DOUBLE PRECISION)");
  (void)db->execute("INSERT INTO cc VALUES (1, 1, 100, 2.50, NULL, 'a', 1.5)");
  (void)db->execute("INSERT INTO cc VALUES (2, 2, 200, 2.50, 0, 'b', 2.5)");
  (void)db->execute("INSERT INTO cc VALUES (3, 3, 300, 10.00, 4, 'c', 0.5)");
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
  auto rows = [&](std::string_view sql) { return db->execute(sql).rows; };

  // Searched: the first true WHEN, else ELSE, else NULL; a NULL condition is not true.
  auto bands = db->execute("SELECT CASE WHEN i < 2 THEN 'low' WHEN i < 3 THEN 'mid' ELSE 'high' END FROM cc ORDER BY id");
  CHECK(bands.rows == Grid{{text("low")}, {text("mid")}, {text("high")}});
  CHECK(bands.columns == std::vector<std::string>{"case"});
  CHECK(bands.types == std::vector<SqlType>{SqlType{SqlKind::Text}});
  CHECK(rows("SELECT CASE WHEN z > 0 THEN 1 END FROM cc ORDER BY id") == Grid{{null}, {null}, {i(1)}});
  CHECK(rows("SELECT CASE WHEN i > 1 THEN NULL END FROM cc ORDER BY id") == Grid{{null}, {null}, {null}});
  // Simple: CASE x WHEN v tests x = v.
  CHECK(rows("SELECT CASE i WHEN 1 THEN 10 WHEN 3 THEN 30 ELSE 0 END FROM cc ORDER BY id") == Grid{{i(10)}, {i(0)}, {i(30)}});
  CHECK(rows("SELECT CASE z WHEN 0 THEN 'zero' WHEN 4 THEN 'four' END FROM cc ORDER BY id") ==
        Grid{{null}, {text("zero")}, {text("four")}});
  // A WHEN that is AND, OR, NOT, IS NULL, or a subquery's boolean.
  CHECK(rows("SELECT CASE WHEN z IS NULL THEN 'n' WHEN z = 0 OR i > 5 THEN 'z' ELSE 'p' END FROM cc ORDER BY id") ==
        Grid{{text("n")}, {text("z")}, {text("p")}});
  CHECK(rows("SELECT CASE WHEN NOT (i > 1) AND z IS NULL THEN 1 ELSE 0 END FROM cc ORDER BY id") == Grid{{i(1)}, {i(0)}, {i(0)}});
  CHECK(rows("SELECT CASE WHEN i > (SELECT MIN(id) FROM cc) THEN 'x' ELSE 'y' END FROM cc ORDER BY id") ==
        Grid{{text("y")}, {text("x")}, {text("x")}});

  // COALESCE and NULLIF.
  CHECK(rows("SELECT COALESCE(z, 99), COALESCE(z, i), COALESCE(t, 'q') FROM cc ORDER BY id") ==
        Grid{{i(99), i(1), text("a")}, {i(0), i(0), text("b")}, {i(4), i(4), text("c")}});
  CHECK(db->execute("SELECT COALESCE(z, i) FROM cc").columns == std::vector<std::string>{"coalesce"});
  CHECK(rows("SELECT NULLIF(z, 0), NULLIF(i, 2) FROM cc ORDER BY id") == Grid{{null, i(1)}, {null, null}, {i(4), i(3)}});
  CHECK(db->execute("SELECT NULLIF(i, 2) FROM cc").columns == std::vector<std::string>{"nullif"});
  CHECK(rows("SELECT COALESCE(NULLIF(z, 0), i) FROM cc ORDER BY id") == Grid{{i(1)}, {i(2)}, {i(4)}});
  CHECK(rows("SELECT COALESCE(z, (SELECT MAX(id) FROM cc)) FROM cc ORDER BY id") == Grid{{i(3)}, {i(0)}, {i(4)}});
  {
    const auto& st = db->prepare("SELECT COALESCE(z, $1) FROM cc ORDER BY id");
    CHECK(db->execute(st, {i(7)}).rows == Grid{{i(7)}, {i(0)}, {i(4)}});
  }

  // Result types: integers widen; a numeric is at the finest scale of its branches.
  CHECK(db->execute("SELECT CASE WHEN i = 1 THEN s ELSE i END FROM cc").types[0] == SqlType{SqlKind::Integer});
  CHECK(rows("SELECT CASE WHEN i = 1 THEN s ELSE i END FROM cc ORDER BY id") == Grid{{i(100)}, {i(2)}, {i(3)}});
  auto dec = db->execute("SELECT COALESCE(z, d), CASE WHEN i = 1 THEN d ELSE 0.125 END FROM cc ORDER BY id");
  CHECK(dec.types == std::vector<SqlType>{{SqlKind::Decimal, 0, 0, 2}, {SqlKind::Decimal, 0, 0, 3}});
  CHECK(dec.rows == Grid{{i(250), i(2500)}, {i(0), i(125)}, {i(400), i(125)}});
  CHECK(rows("SELECT CASE WHEN i < 3 THEN f END FROM cc ORDER BY id") == Grid{{Value{1.5}}, {Value{2.5}}, {null}});
  CHECK(state("SELECT CASE WHEN i = 1 THEN f ELSE 0 END FROM cc") == "42601");  // a float beside another number

  // A branch is evaluated only when taken: an untaken one raises nothing.
  CHECK(rows("SELECT CASE WHEN z <> 0 THEN i * 100 / z ELSE 0 END FROM cc ORDER BY id") == Grid{{i(0)}, {i(0)}, {i(75)}});
  CHECK(rows("SELECT COALESCE(z, i / z) FROM cc WHERE id = 3") == Grid{{i(4)}});
  CHECK(rows("SELECT COALESCE(i, i / z) FROM cc ORDER BY id") == Grid{{i(1)}, {i(2)}, {i(3)}});
  CHECK(state("SELECT CASE WHEN id > 1 THEN i / z END FROM cc") == "22012");  // the taken one raises
  CHECK(state("SELECT CASE WHEN i > 5 THEN i ELSE 1 / 0 END FROM cc") == "22012");  // a constant, folded when planned, as PostgreSQL

  // Errors.
  CHECK(state("SELECT CASE WHEN i THEN 1 END FROM cc") == "42804");
  CHECK(state("SELECT CASE WHEN i > 0 THEN i ELSE t END FROM cc") == "42804");
  CHECK(state("SELECT COALESCE(i, t) FROM cc") == "42804");
  CHECK(state("SELECT CASE WHEN i > 0 THEN i ELSE 'x' END FROM cc") == "22P02");
  CHECK(state("SELECT NULLIF(i, t) FROM cc") == "42883");

  // Over a join, in a set operation, in a view.
  (void)db->execute("CREATE TABLE cc2 (id INTEGER PRIMARY KEY, w INTEGER)");
  (void)db->execute("INSERT INTO cc2 VALUES (1, 10)");
  (void)db->execute("INSERT INTO cc2 VALUES (3, 30)");
  CHECK(rows("SELECT CASE WHEN cc.i + cc2.w > 20 THEN 1 ELSE 0 END FROM cc JOIN cc2 ON cc.id = cc2.id ORDER BY cc2.w") ==
        Grid{{i(0)}, {i(1)}});
  CHECK(rows("SELECT COALESCE(z, 9) FROM cc WHERE id = 1 UNION ALL SELECT 5 ORDER BY 1") == Grid{{i(5)}, {i(9)}});
  (void)db->execute("CREATE VIEW ccv AS SELECT id, COALESCE(z, 0) AS zz FROM cc");
  CHECK(rows("SELECT zz FROM ccv ORDER BY id") == Grid{{i(0)}, {i(0)}, {i(4)}});
}

TEST_CASE("embedded API: CASE, COALESCE and NULLIF of constants fold as PostgreSQL's do", "[sql][api]") {
  sql_test::TempDir dir{"api_case"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto one = [&](std::string_view sql) { return db->execute(sql).rows; };
  // Searched and simple CASE: the first true WHEN, else ELSE, else NULL.
  CHECK(one("SELECT CASE WHEN 1 = 2 THEN 10 WHEN 2 = 2 THEN 20 ELSE 30 END, CASE 3 WHEN 1 THEN 'a' WHEN 3 THEN 'c' END") ==
        Grid{{i(20), text("c")}});
  CHECK(one("SELECT CASE WHEN 0 = 1 THEN 1 END, CASE 0 WHEN 2 THEN 1 END, CASE NULL WHEN NULL THEN 1 ELSE 2 END") ==
        Grid{{Value{}, Value{}, i(2)}});
  CHECK(one("SELECT CASE WHEN NULL THEN 1 ELSE 2 END").front().front() == i(2));  // a NULL condition is not true
  CHECK(db->execute("SELECT CASE WHEN 0 = 1 THEN 1 END, COALESCE(NULL, 2), NULLIF(1, 1)").columns ==
        std::vector<std::string>{"case", "coalesce", "nullif"});
  // A WHEN after the one that matched is not evaluated.
  CHECK(one("SELECT CASE WHEN 1 = 1 THEN 1 WHEN 1 / 0 = 1 THEN 2 END").front().front() == i(1));
  // COALESCE and NULLIF.
  CHECK(one("SELECT COALESCE(NULL, NULL, 3, 4), COALESCE(NULL, NULL), NULLIF(1, 2), NULLIF(2, 2), NULLIF(NULL, 1), NULLIF(1, NULL)") ==
        Grid{{i(3), Value{}, i(1), Value{}, Value{}, i(1)}});
  CHECK(one("SELECT NULLIF('a', 'a'), NULLIF('a', 'b'), COALESCE(NULL, 'x'), NULLIF('1', 1), NULLIF(1, 1.0)") ==
        Grid{{Value{}, text("a"), text("x"), Value{}, Value{}}});
  // An unknown literal takes the other branches' type; integers widen.
  CHECK(one("SELECT COALESCE(NULL, '7'), CASE WHEN 1 = 2 THEN 1 ELSE 2 END") == Grid{{text("7"), i(2)}});
  CHECK(one("SELECT COALESCE(1, 2147483648) + 1").front().front() == i(2));
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
  CHECK(state("SELECT CASE WHEN 1 THEN 1 END") == "42804");        // a condition must be boolean
  CHECK(state("SELECT CASE WHEN 1 = 1 THEN 1 ELSE true END") == "42804");
  CHECK(state("SELECT COALESCE(1, true)") == "42804");
  CHECK(state("SELECT COALESCE(1, 'a')") == "22P02");
  CHECK(state("SELECT CASE WHEN 1 = 1 THEN 1 ELSE 'a' END") == "22P02");  // type-checked though not taken
  CHECK(state("SELECT NULLIF(1, 'a')") == "22P02");
  CHECK(state("SELECT NULLIF(1, true)") == "42883");
  CHECK(state("SELECT CASE 1 WHEN true THEN 1 END") == "42883");
  CHECK(state("SELECT CASE WHEN 1 = 2 THEN 1 ELSE 1 / 0 END") == "22012");  // the branch taken raises
  CHECK(state("SELECT COALESCE(NULL, 1 / 0)") == "22012");
  // PostgreSQL skips a result that is not taken; here its type is unknown, so it is refused.
  CHECK(state("SELECT COALESCE(1, 1 / 0, 2)") == "42601");
  CHECK(state("SELECT CASE WHEN 1 = 1 THEN 1 ELSE 2 / 0 END") == "42601");
  CHECK(state("SELECT CASE 0 WHEN 2, 2 THEN 1 END") == "42601");  // PostgreSQL has no list of WHEN values
  (void)db->execute("CREATE TABLE cc (id INTEGER PRIMARY KEY, v INTEGER)");
  // Of columns they are the next test's.
  CHECK(state("SELECT CASE WHEN v = 1 THEN 1 END FROM cc") == "ran");
  CHECK(state("SELECT COALESCE(v, 0) FROM cc") == "ran");
  CHECK(state("SELECT NULLIF(v, 0) FROM cc") == "ran");
}

TEST_CASE("embedded API: || and the string functions of constants fold as PostgreSQL's do", "[sql][api]") {
  sql_test::TempDir dir{"api_strfn"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto one = [&](std::string_view sql) { return db->execute(sql).rows.front().front(); };
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
  // ||: strings, and any other type through its text; NULL on either side.
  CHECK(one("SELECT 'foo' || 'bar' || 'é'") == text("foobaré"));
  CHECK(one("SELECT 'a' || 1 || true || 2.50") == text("a1true2.50"));
  CHECK(one("SELECT 1 || 'a'") == text("1a"));
  CHECK(one("SELECT 'a' || NULL") == Value{});
  CHECK(state("SELECT 1 || 2") == "42883");
  // UPPER and LOWER fold ASCII (COLLATE "C").
  CHECK(db->execute("SELECT UPPER('aé'), LOWER('AÉ'), UPPER(NULL)").rows == Grid{{text("Aé"), text("aÉ"), Value{}}});
  // Lengths count characters and bytes.
  CHECK(db->execute("SELECT CHAR_LENGTH('日本'), CHARACTER_LENGTH('ab'), OCTET_LENGTH('日本'), CHAR_LENGTH(NULL)").rows ==
        Grid{{i(2), i(2), i(6), Value{}}});
  // SUBSTRING: PostgreSQL's text_substring, a start before the string counting toward the length.
  CHECK(db->execute("SELECT SUBSTRING('foobar' FROM 3), SUBSTRING('foobar' FROM 2 FOR 3), SUBSTRING('foobar' FROM -1 FOR 3), "
                    "SUBSTRING('foobar' FROM 0), SUBSTRING('foobar' FROM 9), SUBSTRING('foobar' FROM 5 FOR 2147483647), "
                    "SUBSTRING('日本語' FROM 2 FOR 1)")
            .rows == Grid{{text("obar"), text("oob"), text("f"), text("foobar"), text(""), text("ar"), text("本")}});
  CHECK(one("SELECT SUBSTRING('foobar' FROM 2 FOR NULL::integer)") == Value{});
  CHECK(state("SELECT SUBSTRING('foobar' FROM 1 FOR -1)") == "22011");
  CHECK(state("SELECT SUBSTRING('foobar' FROM 3000000000)") == "42883");
  CHECK(state("SELECT SUBSTRING('foobar' FROM 1 FOR 'x')") == "22P02");
  CHECK(state("SELECT SUBSTRING('foobar' FROM 'o+')") == "42601");  // the regular-expression form: not supported yet
  // TRIM: spaces, or the characters given, by character.
  CHECK(db->execute("SELECT TRIM('  a '), TRIM(LEADING FROM '  a '), TRIM(TRAILING FROM '  a '), TRIM('xy' FROM 'yxaxy'), "
                    "TRIM('é' FROM 'éaé'), BTRIM('xa', 'x'), LTRIM('xxa', 'x'), RTRIM('axx', 'x'), TRIM('' FROM ' a ')")
            .rows == Grid{{text("a"), text("a "), text("  a"), text("a"), text("a"), text("a"), text("a"), text("a"), text(" a ")}});
  // POSITION: 1-based characters, 0 for none, 1 for an empty substring.
  CHECK(db->execute("SELECT POSITION('an' IN 'banana'), POSITION('x' IN 'banana'), POSITION('' IN 'ab'), POSITION('本' IN '日本語')")
            .rows == Grid{{i(2), i(0), i(1), i(2)}});
  CHECK(one("SELECT POSITION(NULL IN 'a')") == Value{});
  // OVERLAY.
  CHECK(db->execute("SELECT OVERLAY('abcdef' PLACING 'XY' FROM 2), OVERLAY('abcdef' PLACING 'XY' FROM 2 FOR 3), "
                    "OVERLAY('abc' PLACING 'XY' FROM 5)")
            .rows == Grid{{text("aXYdef"), text("aXYef"), text("abcXY")}});
  CHECK(state("SELECT OVERLAY('abc' PLACING 'x' FROM 0)") == "22011");
  CHECK(state("SELECT OVERLAY('abc' PLACING 'x' FROM 2147483647 FOR 1)") == "22003");
  // A type no function takes is 42883, a wrong number of arguments too.
  CHECK(state("SELECT UPPER(1)") == "42883");
  CHECK(state("SELECT UPPER(NULL::integer)") == "42883");
  CHECK(state("SELECT UPPER('a', 'b')") == "42883");
  CHECK(state("SELECT TRIM(1)") == "42883");
  CHECK(state("SELECT POSITION(1 IN 'a')") == "42883");
  // An error raised under CASE is raised if its branch is taken, and a branch not taken that raises is refused.
  CHECK(state("SELECT CASE WHEN 1 = 2 THEN SUBSTRING('a' FROM 1 FOR -1) ELSE 'x' END") == "42601");
  CHECK(state("SELECT CASE WHEN 1 = 1 THEN SUBSTRING('a' FROM 1 FOR -1) END") == "22011");
  // The output column is named as PostgreSQL names it.
  CHECK(db->execute("SELECT 'a' || 'b', TRIM('a'), POSITION('a' IN 'b'), SUBSTRING('a' FROM 1), CHARACTER_LENGTH('a'), OVERLAY('a' PLACING 'b' FROM 1)")
            .columns == std::vector<std::string>{"?column?", "btrim", "position", "substring", "character_length", "overlay"});
  (void)db->execute("CREATE TABLE sf (id INTEGER PRIMARY KEY, v VARCHAR(20))");
  (void)db->execute("INSERT INTO sf VALUES (1, 'ab' || 'c')");
  (void)db->execute("INSERT INTO sf VALUES (2, UPPER('x'))");
  CHECK(db->execute("SELECT v FROM sf WHERE v = LOWER('ABC')").rows == Grid{{text("abc")}});
  // Of a column they are select-list values (below); in a WHERE, not yet.
  CHECK(db->execute("SELECT v || 'x', UPPER(v) FROM sf WHERE id = 1").rows == Grid{{text("abcx"), text("ABC")}});
  CHECK(state("SELECT id FROM sf WHERE UPPER(v) = 'ABC'") == "42601");
}

TEST_CASE("embedded API: a view is read as a table, expanded at each use, and kept in the catalog", "[sql][api][view]") {
  sql_test::TempDir dir{"api_view"};
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto state = [](Database& db, std::string_view sql) -> std::string {
    try {
      (void)db.execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    } catch (const DdlRefused& e) {
      return std::string{e.sqlstate()};
    } catch (const FeatureNotSupported&) {
      return std::string{"0A000"};
    }
    return std::string{"ran"};
  };
  {
    auto db = Database::open(dir.path());
    auto run = [&](std::string_view sql) { return db->execute(sql).rows; };
    (void)run("CREATE TABLE vt (id INTEGER PRIMARY KEY, a INTEGER, b VARCHAR(10))");
    (void)run("INSERT INTO vt VALUES (1, 10, 'x')");
    (void)run("INSERT INTO vt VALUES (2, 20, 'y')");
    (void)run("INSERT INTO vt VALUES (3, 30, 'z')");
    (void)run("CREATE TABLE vu (id INTEGER PRIMARY KEY, c INTEGER)");
    (void)run("INSERT INTO vu VALUES (2, 200)");
    (void)run("INSERT INTO vu VALUES (3, 300)");
    (void)run("INSERT INTO vu VALUES (4, 400)");

    // A view of a WHERE, with a column renamed; its columns are its select list's names.
    (void)run("CREATE VIEW v AS SELECT id, a AS aa, b FROM vt WHERE a > 10");
    CHECK(run("SELECT * FROM v ORDER BY id") == Grid{{i(2), i(20), text("y")}, {i(3), i(30), text("z")}});
    CHECK(db->execute("SELECT * FROM v").columns == std::vector<std::string>{"id", "aa", "b"});
    CHECK(run("SELECT aa FROM v WHERE id = 3") == Grid{{i(30)}});
    CHECK(run("SELECT aa, id FROM v ORDER BY aa DESC") == Grid{{i(30), i(3)}, {i(20), i(2)}});
    CHECK(run("SELECT v.b FROM v WHERE v.aa >= 30") == Grid{{text("z")}});
    CHECK(run("SELECT COUNT(*) FROM v") == Grid{{i(2)}});
    CHECK(run("SELECT id FROM v WHERE aa = 20 OR b = 'z' ORDER BY id") == Grid{{i(2)}, {i(3)}});
    CHECK(run("SELECT x.id FROM v AS x WHERE x.aa = 20") == Grid{{i(2)}});
    // Read at each use: a row written since shows.
    (void)run("INSERT INTO vt VALUES (4, 40, 'w')");
    CHECK(run("SELECT COUNT(*) FROM v") == Grid{{i(3)}});
    // A column the view does not have is the view's to refuse, not the table's.
    CHECK(state(*db, "SELECT a FROM v") == "42703");
    CHECK(state(*db, "SELECT nope.id FROM v") == "42P01");

    // A column list names the columns; fewer names than columns rename the first ones.
    (void)run("CREATE VIEW w (x, y) AS SELECT id, a FROM vt");
    CHECK(db->execute("SELECT * FROM w").columns == std::vector<std::string>{"x", "y"});
    CHECK(run("SELECT x, y FROM w WHERE y >= 30 ORDER BY x") == Grid{{i(3), i(30)}, {i(4), i(40)}});
    CHECK(state(*db, "CREATE VIEW w3 (x, y, z) AS SELECT id, a FROM vt") == "42601");
    CHECK(state(*db, "CREATE VIEW w4 (x, x) AS SELECT id, a FROM vt") == "42701");
    CHECK(state(*db, "CREATE VIEW w5 AS SELECT id, id FROM vt") == "42701");

    // In a join, with a table or with another view, and in subqueries.
    CHECK(run("SELECT v.aa, vu.c FROM v JOIN vu ON v.id = vu.id ORDER BY vu.c") == Grid{{i(20), i(200)}, {i(30), i(300)}, {i(40), i(400)}});
    CHECK(run("SELECT * FROM vu JOIN v ON vu.id = v.id WHERE vu.c > 200 ORDER BY vu.c") ==
          Grid{{i(3), i(300), i(3), i(30), text("z")}, {i(4), i(400), i(4), i(40), text("w")}});
    CHECK(run("SELECT b, c FROM v JOIN vu ON v.id = vu.id WHERE c = 200") == Grid{{text("y"), i(200)}});  // each column's side is found
    CHECK(run("SELECT w.y, vu.c FROM w, vu WHERE w.x = vu.id AND w.y = 30") == Grid{{i(30), i(300)}});
    CHECK(state(*db, "SELECT * FROM w, v WHERE w.x = v.id") == "42601");  // two views of one table: a table joined with itself
    CHECK(run("SELECT c FROM vu WHERE id IN (SELECT id FROM v WHERE aa > 20) ORDER BY c") == Grid{{i(300)}, {i(400)}});
    CHECK(run("SELECT c FROM vu WHERE EXISTS (SELECT id FROM v WHERE aa = 20)").size() == 3);
    CHECK(state(*db, "SELECT id FROM v JOIN vu ON v.id = vu.id") == "42702");  // both sides have id
    CHECK(state(*db, "SELECT * FROM v JOIN vu USING (id)") == "42601");  // USING a view's column: not supported yet
    CHECK(state(*db, "SELECT v.aa FROM v LEFT JOIN vu ON v.id = vu.id") == "42601");  // an outer join of a view with a WHERE
    (void)run("CREATE VIEW plain AS SELECT id, a FROM vt");
    CHECK(run("SELECT plain.a, vu.c FROM plain LEFT JOIN vu ON plain.id = vu.id WHERE plain.id < 3 ORDER BY plain.id") ==
          Grid{{i(10), Value{}}, {i(20), i(200)}});
    CHECK(run("SELECT vu.c, plain.a FROM vu RIGHT JOIN plain ON plain.id = vu.id WHERE plain.id < 3 ORDER BY plain.id") ==
          Grid{{Value{}, i(10)}, {i(200), i(20)}});

    // A view of a view.
    (void)run("CREATE VIEW vv AS SELECT aa AS q, b FROM v WHERE id < 4");
    CHECK(run("SELECT q FROM vv ORDER BY q") == Grid{{i(20)}, {i(30)}});
    CHECK(run("SELECT * FROM vv WHERE b = 'z'") == Grid{{i(30), text("z")}});

    // A constant in a view's select list, and a view of no table.
    (void)run("CREATE VIEW k AS SELECT id, 7 AS seven FROM vt");
    CHECK(run("SELECT id, seven FROM k WHERE id = 1") == Grid{{i(1), i(7)}});
    // An aggregate of a view column that is an expression: of its value per row.
    const auto k_rows = std::get<std::int64_t>(run("SELECT COUNT(*) FROM k").at(0).at(0));
    CHECK(run("SELECT SUM(seven) FROM k") == Grid{{i(7 * k_rows)}});
    (void)run("CREATE VIEW one AS SELECT 1 AS n");
    CHECK(run("SELECT n FROM one") == Grid{{i(1)}});

    // A grouped view: the query reads its groups; its WHERE keeps groups (a HAVING).
    (void)run("INSERT INTO vt VALUES (5, 10, 'x')");
    (void)run("CREATE VIEW g AS SELECT a, COUNT(*) AS n, MIN(id) AS lo FROM vt GROUP BY a");
    CHECK(run("SELECT a, n FROM g ORDER BY a") == Grid{{i(10), i(2)}, {i(20), i(1)}, {i(30), i(1)}, {i(40), i(1)}});
    CHECK(run("SELECT a FROM g WHERE n = 2") == Grid{{i(10)}});
    CHECK(run("SELECT lo FROM g WHERE a = 10 AND n >= 2") == Grid{{i(1)}});
    CHECK(run("SELECT a FROM g WHERE a > 100").empty());
    (void)run("CREATE VIEW ga AS SELECT a FROM vt GROUP BY a HAVING a >= 30");
    CHECK(run("SELECT a FROM ga ORDER BY a") == Grid{{i(30)}, {i(40)}});
    // Grouping a grouped view again by its own groups is the view.
    CHECK(run("SELECT a FROM ga GROUP BY a ORDER BY a") == Grid{{i(30)}, {i(40)}});
    CHECK(run("SELECT a FROM ga WHERE a = 40 GROUP BY a HAVING a = 40") == Grid{{i(40)}});
    CHECK(run("SELECT a FROM ga WHERE a = 40 GROUP BY a HAVING a = 30").empty());
    // MIN, MAX and COUNT(DISTINCT) cannot tell a value once from many: they read the base rows the view keeps.
    CHECK(run("SELECT MAX(a), MIN(a) FROM ga") == Grid{{i(40), i(30)}});
    CHECK(run("SELECT COUNT(DISTINCT a) FROM ga WHERE a < 40") == Grid{{i(1)}});
    // Any other aggregate reads the view's groups as rows: a derived relation.
    CHECK(run("SELECT SUM(a) FROM ga") == Grid{{i(70)}});
    CHECK(run("SELECT COUNT(*) FROM ga") == Grid{{i(2)}});
    CHECK(run("SELECT DISTINCT a FROM ga ORDER BY a") == Grid{{i(30)}, {i(40)}});
    CHECK(run("SELECT SUM(n), MAX(lo) FROM g WHERE a < 40") == Grid{{i(4), i(3)}});
    CHECK(run("SELECT n, COUNT(*) FROM g GROUP BY n ORDER BY n") == Grid{{i(1), i(3)}, {i(2), i(1)}});
    // A derived relation has no primary key: a column it does not group is 42803, as PostgreSQL's.
    CHECK(state(*db, "SELECT a, n FROM g GROUP BY a") == "42803");
    CHECK(run("SELECT a FROM g JOIN vu ON g.a = vu.id").empty());  // a grouped view joined with a table
    // The statement's LIMIT and OFFSET count the relation's rows above, never the view's groups.
    CHECK(run("SELECT COUNT(*) FROM g LIMIT 1") == Grid{{i(4)}});
    CHECK(run("SELECT DISTINCT n FROM g ORDER BY n LIMIT 1") == Grid{{i(1)}});
    CHECK(run("SELECT n, COUNT(*) FROM g GROUP BY n ORDER BY n DESC LIMIT 1 OFFSET 1") == Grid{{i(1), i(3)}});
    // A view of one group (no GROUP BY) filtered by constants, a column's or a key's range: its
    // group over the rows it keeps, over none too. A point lookup may end the plan before it
    // reads, which gives no group: refused.
    (void)run("CREATE VIEW c1 AS SELECT COUNT(*) AS c, SUM(a) AS s FROM vt WHERE a > 15");
    CHECK(run("SELECT SUM(c), MAX(s) FROM c1") == Grid{{i(3), i(90)}});
    (void)run("CREATE VIEW c2 AS SELECT COUNT(*) AS c FROM vt WHERE id > 1 AND id < 4");
    CHECK(run("SELECT SUM(c) FROM c2") == Grid{{i(2)}});
    (void)run("CREATE VIEW c3 AS SELECT COUNT(*) AS c, SUM(a) AS s FROM vt WHERE id < 0");
    CHECK(run("SELECT COUNT(*), SUM(c), MAX(s) FROM c3") == Grid{{i(1), i(0), Value{}}});
    (void)run("CREATE VIEW c4 AS SELECT COUNT(*) AS c FROM vt WHERE id = 3");
    CHECK(state(*db, "SELECT SUM(c) FROM c4") == "0A000");
    // One level: a view that reads a grouped view as a relation is not read as one.
    (void)run("CREATE VIEW gg AS SELECT n, SUM(lo) AS s FROM g GROUP BY n");
    CHECK(run("SELECT n, s FROM gg ORDER BY n") == Grid{{i(1), i(2 + 3 + 4)}, {i(2), i(1)}});
    CHECK(state(*db, "SELECT SUM(s) FROM gg") == "42601");
    for (auto v : {"gg", "c1", "c2", "c3", "c4"}) (void)run(std::string{"DROP VIEW "} + v);

    // A grouped view joined with a table: its groups are the join's outer rows, USING and
    // NATURAL included, and the table may be the one the view reads.
    auto sorted = [&](std::string_view sql) {
      auto rows = run(sql);
      std::ranges::sort(rows);
      return rows;
    };
    CHECK(sorted("SELECT g.n, vu.c FROM g JOIN vu ON vu.id = g.lo") == Grid{{i(1), i(200)}, {i(1), i(300)}, {i(1), i(400)}});
    CHECK(sorted("SELECT vu.c, g.a FROM vu, g WHERE vu.id = g.lo AND g.a > 20") == Grid{{i(300), i(30)}, {i(400), i(40)}});
    (void)run("CREATE VIEW gu AS SELECT id, COUNT(*) AS m FROM vt GROUP BY id");
    CHECK(sorted("SELECT * FROM gu JOIN vu USING (id)") == Grid{{i(2), i(1), i(200)}, {i(3), i(1), i(300)}, {i(4), i(1), i(400)}});
    CHECK(sorted("SELECT id, c FROM vu NATURAL JOIN gu WHERE c < 400") == Grid{{i(2), i(200)}, {i(3), i(300)}});
    CHECK(sorted("SELECT lo, c FROM g LEFT JOIN vu ON vu.id = g.lo") ==
          Grid{{i(1), Value{}}, {i(2), i(200)}, {i(3), i(300)}, {i(4), i(400)}});
    CHECK(sorted("SELECT c, lo FROM vu RIGHT JOIN g ON vu.id = g.lo WHERE g.n = 2") == Grid{{Value{}, i(1)}});
    CHECK(sorted("SELECT g.n, vt.b FROM g JOIN vt ON vt.a = g.a WHERE g.n = 2") == Grid{{i(2), text("x")}, {i(2), text("x")}});
    CHECK(run("SELECT c FROM g JOIN vu ON vu.id = g.lo LIMIT 2").size() == 2);
    CHECK(run("SELECT c FROM g JOIN vu ON vu.id = g.lo LIMIT 2 OFFSET 2").size() == 1);
    // Refused: the view on an outer join's NULL-extended side, an aggregate over the join, and
    // ORDER BY, whose sort would hold the view's rows.
    CHECK(state(*db, "SELECT c FROM vu LEFT JOIN g ON vu.id = g.lo") == "42601");
    CHECK(state(*db, "SELECT COUNT(*) FROM g JOIN vu ON vu.id = g.lo") == "42601");
    CHECK(state(*db, "SELECT c FROM g JOIN vu ON vu.id = g.lo ORDER BY c") == "0A000");
    CHECK(state(*db, "SELECT * FROM g JOIN gu ON g.lo = gu.id") == "42601");
    (void)run("DROP VIEW gu");

    // A view that cuts rows (DISTINCT, LIMIT) is created, and not read yet.
    (void)run("CREATE VIEW d AS SELECT DISTINCT a FROM vt");
    CHECK(state(*db, "SELECT a FROM d") == "42601");
    CHECK(state(*db, "CREATE VIEW dp AS SELECT a FROM vt WHERE a = $1") == "42P02");

    // INSERT through a view of one table writes the table: the view's columns are the
    // table's they select; its WHERE is not checked (no WITH CHECK OPTION).
    CHECK(db->execute("INSERT INTO v VALUES (9, 9, 'q')").affected == 1);
    CHECK(run("SELECT a, b FROM vt WHERE id = 9") == Grid{{i(9), text("q")}});
    CHECK(run("SELECT id FROM v WHERE id = 9").empty());
    // A row shorter than the view fills its first columns; the table's defaults the rest.
    CHECK(db->execute("INSERT INTO w VALUES (10)").affected == 1);
    CHECK(run("SELECT a, b FROM vt WHERE id = 10") == Grid{{Value{}, Value{}}});
    // A column list, RETURNING over the view's columns, a view of a view, and a FROM alias's names.
    CHECK(run("INSERT INTO v (b, id) VALUES ('r', 11) RETURNING *") == Grid{{i(11), Value{}, text("r")}});
    (void)run("CREATE VIEW ww AS SELECT y, x FROM w");
    CHECK(run("INSERT INTO ww VALUES (120, 12) RETURNING x + 1, y") == Grid{{i(13), i(120)}});
    CHECK(run("SELECT a FROM vt WHERE id = 12") == Grid{{i(120)}});
    (void)run("CREATE VIEW al AS SELECT x.p, x.q FROM vt AS x (p, q)");
    CHECK(run("INSERT INTO al VALUES (13, 130) RETURNING p, q") == Grid{{i(13), i(130)}});
    CHECK(db->execute("INSERT INTO v SELECT id + 100, aa, b FROM v WHERE id = 3").affected == 1);
    CHECK(run("SELECT a FROM vt WHERE id = 103") == Grid{{i(30)}});
    // What PostgreSQL refuses: a view it cannot write through (55000), a column that is an
    // expression (0A000), two view columns of one table column (42601), a row too long (42601).
    CHECK(state(*db, "INSERT INTO d VALUES (1)") == "55000");
    CHECK(state(*db, "INSERT INTO g (a) VALUES (1)") == "55000");
    CHECK(state(*db, "INSERT INTO one VALUES (1)") == "55000");
    CHECK(state(*db, "INSERT INTO k VALUES (14, 7)") == "0A000");
    CHECK(state(*db, "INSERT INTO k (id) VALUES (14)") == "ran");
    (void)run("CREATE VIEW twice AS SELECT id, id AS id2 FROM vt");
    CHECK(state(*db, "INSERT INTO twice VALUES (15, 15)") == "42601");
    CHECK(state(*db, "INSERT INTO w VALUES (16, 1, 2)") == "42601");
    CHECK(state(*db, "INSERT INTO w (nope) VALUES (16)") == "42703");
    try {
      (void)db->execute("INSERT INTO d VALUES (1)");
      FAIL("an INSERT through a DISTINCT view ran");
    } catch (const ast::SqlError& e) {
      CHECK(std::string{e.what()} == "cannot insert into view \"d\"");
      CHECK(e.detail() == "Views containing DISTINCT are not automatically updatable.");
      CHECK(e.hint().starts_with("To enable inserting into the view"));
    }
    (void)run("DROP VIEW twice");
    (void)run("DROP VIEW ww");
    (void)run("DROP VIEW al");
    (void)run("DELETE FROM vt WHERE id > 5");
    // UPDATE and DELETE of a view, and DDL against one, are refused.
    CHECK(state(*db, "UPDATE v SET aa = 1") == "0A000");
    CHECK(state(*db, "DELETE FROM v") == "0A000");
    CHECK(state(*db, "CREATE INDEX vi ON v (id)") == "0A000");
    CHECK(run("SELECT COUNT(*) FROM vt").front().front() == i(5));
    // A view in a subquery of a write.
    CHECK(db->execute("DELETE FROM vu WHERE id IN (SELECT id FROM v WHERE aa = 40)").affected == 1);
    CHECK(run("SELECT COUNT(*) FROM vu") == Grid{{i(2)}});
    (void)run("INSERT INTO vu VALUES (4, 400)");

    // DDL: the names are one namespace, and a drop names its kind.
    CHECK(state(*db, "CREATE VIEW v AS SELECT id FROM vt") == "42P07");
    CHECK(state(*db, "CREATE VIEW vt AS SELECT id FROM vu") == "42P07");
    CHECK(state(*db, "CREATE TABLE v (id INTEGER PRIMARY KEY)") == "42P07");
    CHECK(state(*db, "CREATE VIEW bad AS SELECT id FROM nope") == "42P01");
    CHECK(state(*db, "CREATE VIEW bad AS SELECT nope FROM vt") == "42703");
    CHECK(state(*db, "CREATE VIEW bad AS SELECT * FROM v, vu, vt") == "42601");
    CHECK(state(*db, "CREATE VIEW bad AS SELECT id FROM vt WITH CHECK OPTION") == "42601");
    CHECK(state(*db, "DROP VIEW vt") == "42809");
    CHECK(state(*db, "DROP TABLE v") == "42809");
    CHECK(state(*db, "DROP INDEX v") == "42809");
    CHECK(state(*db, "DROP VIEW nope") == "42P01");
    CHECK(state(*db, "DROP VIEW IF EXISTS nope") == "ran");
    // A view keeps what it reads from being dropped, as PostgreSQL's RESTRICT does.
    CHECK(state(*db, "DROP TABLE vt") == "2BP01");
    CHECK(state(*db, "DROP VIEW v") == "2BP01");  // vv reads it
    CHECK(run("SELECT COUNT(*) FROM vv") == Grid{{i(2)}});
    db->close();
  }
  // A fresh open reads the same views, and plans compiled through a view follow it when it is dropped and created again.
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql) { return db->execute(sql).rows; };
  CHECK(run("SELECT q FROM vv ORDER BY q") == Grid{{i(20)}, {i(30)}});
  const auto& stmt = db->prepare("SELECT n FROM one");
  CHECK(db->execute(stmt, {}).rows == Grid{{i(1)}});
  (void)run("DROP VIEW one");
  CHECK(state(*db, "SELECT n FROM one") == "42P01");
  (void)run("CREATE VIEW one AS SELECT 2 AS n");
  CHECK(db->execute(stmt, {}).rows == Grid{{i(2)}});  // the cached plan was rebuilt through the new view
  // The view is bound against the tables as they are when it is read. PostgreSQL keeps the columns a star
  // gave a view; here a view the tables have outgrown is refused, never given more columns.
  (void)run("CREATE VIEW everything AS SELECT * FROM vu");
  (void)run("CREATE VIEW over_everything AS SELECT c FROM everything");
  CHECK(db->execute("SELECT * FROM everything").columns == std::vector<std::string>{"id", "c"});
  (void)run("ALTER TABLE vu ADD COLUMN extra INTEGER DEFAULT 5");
  CHECK(state(*db, "SELECT * FROM everything") == "0A000");
  CHECK(state(*db, "SELECT c FROM over_everything") == "0A000");
  (void)run("DROP VIEW over_everything");
  (void)run("DROP VIEW everything");
  (void)run("CREATE VIEW everything AS SELECT * FROM vu");
  CHECK(db->execute("SELECT * FROM everything").columns == std::vector<std::string>{"id", "c", "extra"});
  // Dropping what is read, once its views are gone.
  for (auto name : {"vv", "v", "w", "plain", "k", "one", "g", "ga", "d", "everything"}) (void)run(std::string{"DROP VIEW "} + name);
  (void)run("DROP TABLE vt");
  (void)run("DROP TABLE vu");
}

// A view's dependencies are what the binder resolved while it bound the
// view's query, in every position a subquery may take (#870): a table read
// only by a subquery in ORDER BY was missed, so DROP TABLE of it succeeded
// and the view then failed. Each is RESTRICT, as PostgreSQL 17's 2BP01, and
// names the view that reads the relation itself, not one reading it through
// another view.
TEST_CASE("embedded API: a view's dependencies are every relation its query reads, a subquery's in any position", "[sql][api][view]") {
  sql_test::TempDir dir{"api_view_deps"};
  auto refusal = [](Database& db, std::string_view sql) -> std::string {
    try {
      (void)db.execute(sql);
    } catch (const DdlRefused& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    } catch (const FeatureNotSupported&) {
      return std::string{"0A000"};
    }
    return std::string{"ran"};
  };
  auto dependent = [](std::string_view kind, std::string_view name, std::string_view view) {
    return "2BP01 cannot drop " + std::string{kind} + " " + std::string{name} + " because other objects depend on it: view " +
           std::string{view} + " depends on " + std::string{kind} + " " + std::string{name};
  };
  {
    auto db = Database::open(dir.path());
    auto run = [&](std::string_view sql) { return db->execute(sql).rows; };
    (void)run("CREATE TABLE t (a INTEGER, b INTEGER)");
    (void)run("INSERT INTO t VALUES (1, 2)");
    (void)run("INSERT INTO t VALUES (3, 4)");
    for (auto name : {"u_target", "u_where", "u_in", "u_exists", "u_order", "u_arm", "u_q"}) {
      (void)run(std::string{"CREATE TABLE "} + name + " (a INTEGER, b INTEGER)");
      (void)run(std::string{"INSERT INTO "} + name + " VALUES (5, 6)");
    }
    (void)run("CREATE VIEW v_target AS SELECT a, (SELECT max(b) FROM u_target) AS m FROM t");
    (void)run("CREATE VIEW v_where AS SELECT a FROM t WHERE a < (SELECT max(b) FROM u_where)");
    (void)run("CREATE VIEW v_in AS SELECT a FROM t WHERE a IN (SELECT a FROM u_in)");
    (void)run("CREATE VIEW v_exists AS SELECT a FROM t WHERE EXISTS (SELECT 1 FROM u_exists)");
    (void)run("CREATE VIEW v_order AS SELECT a FROM t ORDER BY (SELECT max(b) FROM u_order)");
    (void)run("CREATE VIEW v_arm AS SELECT a FROM t UNION SELECT b FROM u_arm");
    // A view read only by a subquery in ORDER BY, itself over a table.
    (void)run("CREATE VIEW q AS SELECT a, b FROM u_q WHERE a > 0");
    (void)run("CREATE VIEW v_order_view AS SELECT a FROM t ORDER BY (SELECT max(a) FROM q)");
    CHECK(run("SELECT a FROM v_order_view") == Grid{{i(1)}, {i(3)}});
    // GROUP BY and HAVING take no subquery yet, nor a view a WITH query
    // (frontend_design.md): refused, so no view is created without what it reads.
    CHECK(refusal(*db, "CREATE VIEW v_group AS SELECT a FROM t GROUP BY a, (SELECT max(b) FROM u_where)") == "42601");
    CHECK(refusal(*db, "CREATE VIEW v_having AS SELECT a FROM t GROUP BY a HAVING count(*) > (SELECT max(b) FROM u_where)") == "42601");
    CHECK(refusal(*db, "CREATE VIEW v_cte AS WITH c AS (SELECT a FROM u_where) SELECT a FROM c") == "42601");
    db->close();
  }
  // Stored with the view: a reopen keeps them.
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql) { return db->execute(sql).rows; };
  CHECK(refusal(*db, "DROP TABLE t") == dependent("table", "t", "v_target"));
  CHECK(refusal(*db, "DROP TABLE u_target") == dependent("table", "u_target", "v_target"));
  CHECK(refusal(*db, "DROP TABLE u_where") == dependent("table", "u_where", "v_where"));
  CHECK(refusal(*db, "DROP TABLE u_in") == dependent("table", "u_in", "v_in"));
  CHECK(refusal(*db, "DROP TABLE u_exists") == dependent("table", "u_exists", "v_exists"));
  CHECK(refusal(*db, "DROP TABLE u_order") == dependent("table", "u_order", "v_order"));
  CHECK(refusal(*db, "DROP TABLE u_arm") == dependent("table", "u_arm", "v_arm"));
  CHECK(refusal(*db, "DROP VIEW q") == dependent("view", "q", "v_order_view"));
  // u_q is q's, not v_order_view's: the view that reads it itself is named.
  CHECK(refusal(*db, "DROP TABLE u_q") == dependent("table", "u_q", "q"));
  // Once a view is gone, what only it read drops.
  for (auto name : {"v_target", "v_where", "v_in", "v_exists", "v_order", "v_arm", "v_order_view", "q"}) {
    (void)run(std::string{"DROP VIEW "} + name);
  }
  for (auto name : {"u_target", "u_where", "u_in", "u_exists", "u_order", "u_arm", "u_q", "t"}) {
    CHECK(refusal(*db, std::string{"DROP TABLE "} + name) == "ran");
  }
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

TEST_CASE("embedded API: a set operation streams its arms, names its columns from the first and resolves their types", "[sql][api][setop]") {
  sql_test::TempDir dir{"api_setop"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  auto sorted = [](Grid g) {
    std::ranges::sort(g);
    return g;
  };
  (void)run("CREATE TABLE sa (id INTEGER PRIMARY KEY, x INTEGER)");
  (void)run("CREATE TABLE sb (id BIGINT PRIMARY KEY, y BIGINT, f DOUBLE PRECISION)");
  for (auto [id, x] : std::vector<std::pair<int, std::optional<int>>>{{1, 1}, {2, 2}, {3, 2}, {4, {}}, {5, {}}}) {
    (void)db->execute("INSERT INTO sa VALUES ($1, $2)", {i(id), x ? i(*x) : kNull});
  }
  for (auto [id, y] : std::vector<std::pair<int, std::optional<int>>>{{1, 2}, {2, 3}, {3, {}}, {4, 3}, {5, 2}}) {
    (void)db->execute("INSERT INTO sb VALUES ($1, $2, $3)", {i(id), y ? i(*y) : kNull, y ? Value{*y + 0.5} : kNull});
  }

  // Column names are the first arm's; a column both arms give is of the type they resolve to.
  auto r = db->execute("SELECT x AS first, id FROM sa UNION ALL SELECT y AS second, id FROM sb");
  CHECK(r.columns == std::vector<std::string>{"first", "id"});
  CHECK(r.types[0].kind == SqlKind::BigInt);  // INTEGER meets BIGINT
  CHECK(r.rows.size() == 10);
  CHECK(db->execute("SELECT x FROM sa UNION SELECT f FROM sb").types[0].kind == SqlKind::Double);
  CHECK(db->execute("SELECT x FROM sa UNION SELECT y FROM sb").columns == std::vector<std::string>{"x"});

  // An integer meeting a double compares as a double and comes back as one.
  CHECK(sorted(run("SELECT x FROM sa UNION SELECT f FROM sb")) ==
        sorted(Grid{{Value{1.0}}, {Value{2.0}}, {Value{2.5}}, {Value{3.5}}, {kNull}}));
  CHECK(sorted(run("SELECT f FROM sb INTERSECT SELECT x FROM sa")) == Grid{{kNull}});
  CHECK(sorted(run("SELECT x FROM sa EXCEPT ALL SELECT y FROM sb")) == sorted(Grid{{kNull}, {i(1)}}));

  // The counts of ALL.
  CHECK(sorted(run("SELECT x FROM sa INTERSECT ALL SELECT y FROM sb")) == sorted(Grid{{kNull}, {i(2)}, {i(2)}}));
  CHECK(sorted(run("SELECT y FROM sb EXCEPT ALL SELECT x FROM sa")) == Grid{{i(3)}, {i(3)}});
  CHECK(sorted(run("SELECT x FROM sa UNION ALL SELECT y FROM sb")).size() == 10);
  CHECK(sorted(run("SELECT x FROM sa UNION SELECT y FROM sb")) == sorted(Grid{{kNull}, {i(1)}, {i(2)}, {i(3)}}));

  // A prepared statement runs again with other parameters, its structures made anew.
  const auto& stmt = db->prepare("SELECT x FROM sa WHERE id = $1 UNION SELECT y FROM sb WHERE id = $2");
  CHECK(db->execute(stmt, {i(1), i(2)}).rows == Grid{{i(1)}, {i(3)}});
  CHECK(db->execute(stmt, {i(1), i(1)}).rows == Grid{{i(1)}, {i(2)}});
  CHECK(db->execute(stmt, {i(1), i(99)}).rows == Grid{{i(1)}});
  CHECK(db->execute(stmt, {kNull, kNull}).rows.empty());
  for (const auto& all : db->alternatives_for_testing("SELECT x FROM sa UNION SELECT y FROM sb", {})) {
    CHECK(sorted(all.rows) == sorted(Grid{{kNull}, {i(1)}, {i(2)}, {i(3)}}));
  }

  // An arm that is a join, and one that reads an index's keys alone.
  (void)run("CREATE TABLE sc (id INTEGER PRIMARY KEY, z INTEGER)");
  for (int k = 1; k <= 3; ++k) (void)db->execute("INSERT INTO sc VALUES ($1, $2)", {i(k), i(k * 10)});
  CHECK(sorted(run("SELECT sa.x FROM sa, sc WHERE sa.id = sc.id UNION SELECT y FROM sb WHERE id = 2")) ==
        sorted(Grid{{i(1)}, {i(2)}, {i(3)}}));
  CHECK(sorted(run("SELECT sa.x FROM sa, sc WHERE sa.id = sc.id EXCEPT ALL SELECT y FROM sb WHERE id < 3")) ==
        sorted(Grid{{i(1)}, {i(2)}}));
  (void)run("CREATE TABLE sj (id BIGINT PRIMARY KEY, g BIGINT, name BYTEA)");
  (void)run("CREATE INDEX sj_by_name ON sj (name)");
  for (auto [id, g, name] : std::vector<std::tuple<int, int, const char*>>{{1, 1, "apple"}, {2, 1, "banana"}, {3, 2, "cherry"}, {4, 2, "apple"}}) {
    (void)db->execute("INSERT INTO sj VALUES ($1, $2, $3)", {i(id), i(g), bytes(name)});
  }
  const std::string covered =
      "SELECT name FROM sj WHERE name >= $1 AND name < $2 UNION ALL SELECT name FROM sj WHERE id = $3 INTERSECT SELECT name FROM sj WHERE g = $4";
  INFO(db->prepare(covered).plan_source());
  CHECK(db->prepare(covered).plan_source().find("bc_key_part_bytes") != std::string::npos);  // the index covers the first arm
  CHECK(sorted(run(covered, {bytes("a"), bytes("b"), i(2), i(1)})) ==
        sorted(Grid{{bytes("apple")}, {bytes("apple")}, {bytes("banana")}}));
  CHECK(sorted(run("SELECT name FROM sj WHERE g = $1 EXCEPT SELECT name FROM sj WHERE g = $2", {i(2), i(1)})) ==
        sorted(Grid{{bytes("cherry")}}));

  // An arm's IN or NOT IN of a subquery tests the statement's set, run once before the arms.
  CHECK(sorted(run("SELECT id FROM sa WHERE id IN (SELECT y FROM sb) UNION ALL SELECT id FROM sb WHERE id NOT IN "
                   "(SELECT x FROM sa WHERE x IS NOT NULL)")) == sorted(Grid{{i(2)}, {i(3)}, {i(3)}, {i(4)}, {i(5)}}));
  CHECK(sorted(run("SELECT x FROM sa INTERSECT SELECT y FROM sb WHERE y IN (SELECT x FROM sa)")) == Grid{{i(2)}});

  // Inside a transaction a set operation reads what the transaction wrote.
  auto txn = db->begin();
  (void)txn.execute("INSERT INTO sa VALUES (6, 3)");
  CHECK(sorted(txn.execute("SELECT x FROM sa EXCEPT SELECT y FROM sb WHERE id = 1").rows) ==
        sorted(Grid{{kNull}, {i(1)}, {i(3)}}));
  txn.rollback();

  // Refusals.
  CHECK(sql_refusal(*db, "(SELECT x FROM sa UNION SELECT y FROM sb ORDER BY x) UNION SELECT 1", {})
            .find("ORDER BY on a parenthesised set operation") != std::string::npos);
  CHECK(sql_refusal(*db, "SELECT DISTINCT x FROM sa UNION SELECT y FROM sb", {}).find("aggregated, grouped or DISTINCT") !=
        std::string::npos);
  CHECK(sql_refusal(*db, "SELECT x FROM sa UNION SELECT 'a'", {}).find("literal of no type") != std::string::npos);
  CHECK(sql_refusal(*db, "SELECT x FROM sa UNION SELECT 1", {}).empty());
  CHECK(sql_refusal(*db, "SELECT x FROM sa UNION SELECT NULL", {}).find("literal of no type") != std::string::npos);
  CHECK(sql_refusal(*db, "SELECT 1 UNION SELECT NULL", {}).find("literal of no type") != std::string::npos);
  CHECK(sql_refusal(*db, "SELECT 'a' UNION SELECT 'b'", {}).empty());
}

TEST_CASE("embedded API: a set operation holds the keys of one side and fails past the statement's memory budget", "[sql][api][setop]") {
  sql_test::TempDir dir{"api_setop_mem"};
  auto db = Database::open(dir.path(), {.statement_memory_bytes = 64u << 10});
  (void)db->execute("CREATE TABLE big (id INTEGER PRIMARY KEY, x INTEGER)");
  auto txn = db->begin();
  for (int k = 0; k < 2000; ++k) (void)txn.execute("INSERT INTO big VALUES ($1, $1)", {i(k)});
  (void)txn.commit();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  // Nothing is collected but the side that is held: the other streams past it.
  CHECK(state("SELECT x FROM big UNION ALL SELECT x FROM big LIMIT 5") == "ran");
  CHECK(state("SELECT x FROM big INTERSECT SELECT x FROM big WHERE id < 0") == "54000");
  CHECK(state("SELECT x FROM big WHERE id < 0 EXCEPT SELECT x FROM big") == "54000");
  CHECK(state("SELECT x FROM big WHERE id < 100 EXCEPT SELECT x FROM big WHERE id < 100") == "ran");
  CHECK(state("SELECT x FROM big UNION SELECT x FROM big WHERE id < 0") == "54000");
}

TEST_CASE("embedded API: SUM, AVG, MIN and MAX of a DOUBLE, and DISTINCT values, as PostgreSQL's float8", "[sql][api][aggregate]") {
  sql_test::TempDir dir{"api_double_aggregates"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  (void)run("CREATE TABLE fa (id INTEGER PRIMARY KEY, f DOUBLE PRECISION)");
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  for (auto [id, f] : std::vector<std::pair<int, double>>{{1, nan}, {2, nan}, {3, -0.0}, {4, 0.0}, {5, 1.5}}) {
    (void)db->execute("INSERT INTO fa VALUES ($1, $2)", {i(id), Value{f}});
  }
  // NaN is one value and above every number; -0.0 and 0.0 are one.
  CHECK(run("SELECT COUNT(DISTINCT f), COUNT(f) FROM fa") == Grid{{i(3), i(5)}});
  auto row = run("SELECT MAX(f), MIN(f), SUM(f), AVG(f), MAX(DISTINCT f) FROM fa").at(0);
  CHECK(std::isnan(std::get<double>(row[0])));
  CHECK(std::get<double>(row[1]) == 0.0);
  CHECK(std::isnan(std::get<double>(row[2])));
  CHECK(std::isnan(std::get<double>(row[3])));
  CHECK(std::isnan(std::get<double>(row[4])));
  CHECK(run("SELECT SUM(DISTINCT f), AVG(DISTINCT f), MIN(f) FROM fa WHERE id >= 3") == Grid{{Value{1.5}, Value{0.75}, Value{-0.0}}});
  // A sum that overflows from finite values is 22003, as PostgreSQL's.
  (void)run("CREATE TABLE fb (id INTEGER PRIMARY KEY, f DOUBLE PRECISION)");
  (void)db->execute("INSERT INTO fb VALUES (1, $1)", {Value{1e308}});
  (void)db->execute("INSERT INTO fb VALUES (2, $1)", {Value{1e308}});
  CHECK(run("SELECT SUM(DISTINCT f) FROM fb") == Grid{{Value{1e308}}});
  try {
    (void)run("SELECT SUM(f) FROM fb");
    FAIL("expected an overflow");
  } catch (const ExecError& e) {
    CHECK(std::string{sqlstate(e)} == "22003");
  }
}

TEST_CASE("embedded API: an aggregate of an expression folds its value per row, as PostgreSQL's", "[sql][api][aggregate]") {
  sql_test::TempDir dir{"api_expression_aggregates"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  auto state = [&](std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
    try {
      (void)db->execute(sql, params);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "ran";
  };
  (void)run("CREATE TABLE g (id INTEGER PRIMARY KEY, k INTEGER, v INTEGER)");
  (void)run("CREATE INDEX g_v ON g (v)");
  (void)run("CREATE TABLE h (id INTEGER PRIMARY KEY, gid INTEGER, w INTEGER)");
  (void)run("INSERT INTO g VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30), (4, 2, NULL), (5, NULL, 50)");
  (void)run("INSERT INTO h VALUES (1, 1, 100), (2, 1, 200), (3, 3, 300)");
  const Value null{};

  // One aggregate row: the expression is NULL where an operand is, and skipped.
  CHECK(run("SELECT SUM(v * 2) FROM g WHERE id >= $1", {i(0)}) == Grid{{i(220)}});
  CHECK(run("SELECT COUNT(v + 1), COUNT(1), SUM(1), MIN(v - k), MAX(v * k) FROM g") == Grid{{i(4), i(5), i(5), i(9), i(60)}});
  CHECK(run("SELECT SUM(COALESCE(v, 0)), SUM(CASE WHEN v > 15 THEN 1 ELSE 0 END) FROM g") == Grid{{i(110), i(3)}});
  CHECK(run("SELECT SUM(v * $1) FROM g", {i(3)}) == Grid{{i(330)}});
  // Over the one row a key finds, and over none.
  CHECK(run("SELECT SUM(v * 2), COUNT(v * 2) FROM g WHERE id = 3") == Grid{{i(60), i(1)}});
  CHECK(run("SELECT SUM(v * 2) FROM g WHERE id = 4") == Grid{{null}});
  CHECK(run("SELECT COUNT(v * 2) FROM g WHERE id = 4") == Grid{{i(0)}});
  CHECK(run("SELECT SUM(v * 2) FROM g WHERE id = 99") == Grid{{null}});
  // Over an index's range.
  CHECK(run("SELECT SUM(v * 2), MIN(v + 1) FROM g WHERE v > 15") == Grid{{i(200), i(21)}});

  // Grouped: in each group, and in HAVING, the call the select list makes or one of its own.
  CHECK(run("SELECT k, SUM(v * 2), COUNT(v + k), MIN(-v), MAX(v / 3) FROM g GROUP BY k ORDER BY k") ==
        Grid{{i(1), i(60), i(2), i(-20), i(6)}, {i(2), i(60), i(1), i(-30), i(10)}, {null, i(100), i(0), i(-50), i(16)}});
  CHECK(run("SELECT k, SUM(v * 2) FROM g GROUP BY k HAVING SUM(v * 2) > $1 ORDER BY k", {i(60)}) == Grid{{null, i(100)}});
  CHECK(run("SELECT k FROM g GROUP BY k HAVING MAX(v * 2) < 100 ORDER BY k") == Grid{{i(1)}, {i(2)}});
  CHECK(run("SELECT k, SUM(v) FROM g GROUP BY k HAVING MIN(v + 1) > 15 ORDER BY k") == Grid{{i(2), i(30)}, {null, i(50)}});
  // Over a join, of both tables' columns.
  CHECK(run("SELECT SUM(g.v * h.w) FROM g JOIN h ON g.id = h.gid") == Grid{{i(12000)}});
  CHECK(run("SELECT g.id, SUM(h.w * 2) FROM g LEFT JOIN h ON g.id = h.gid GROUP BY g.id ORDER BY g.id") ==
        Grid{{i(1), i(600)}, {i(2), null}, {i(3), i(600)}, {i(4), null}, {i(5), null}});
  // Of a view column that is an expression.
  (void)run("CREATE VIEW gv AS SELECT id, k, v * 2 AS w FROM g");
  CHECK(run("SELECT SUM(w), SUM(w + 1) FROM gv") == Grid{{i(220), i(224)}});

  // An error the argument raises is the statement's, on a row whose operands are not NULL.
  CHECK(state("SELECT SUM(v * 100000000) FROM g") == "22003");
  CHECK(state("SELECT k, SUM(v * $1) FROM g GROUP BY k", {i(100000000)}) == "22003");
  CHECK(state("SELECT COUNT(v / 0) FROM g") == "22012");
  CHECK(run("SELECT COUNT(v / 0) FROM g WHERE v IS NULL") == Grid{{i(0)}});
  // Refused as PostgreSQL refuses them, or not taken yet.
  CHECK(state("SELECT SUM(SUM(v)) FROM g") == "42803");
  CHECK(state("SELECT k, MAX(v + COUNT(*)) FROM g GROUP BY k") == "42803");
  CHECK(state("SELECT SUM($1) FROM g", {i(1)}) == "42725");
  CHECK(state("SELECT COUNT($1) FROM g", {i(1)}) == "42P18");
  // An untyped literal is unknown as an untyped $n is; COUNT counts it.
  CHECK(state("SELECT SUM(NULL) FROM g") == "42725");
  CHECK(state("SELECT AVG('1') FROM g") == "42725");
  CHECK(state("SELECT SUM(DISTINCT NULL) FROM g") == "42725");
  CHECK(run("SELECT COUNT(NULL), COUNT('a') FROM g") == Grid{{i(0), i(5)}});
  CHECK(run("SELECT SUM(NULL::int) FROM g") == Grid{{Value{}}});
  CHECK(state("SELECT SUM(DISTINCT v * 2) FROM g") == "0A000");
  CHECK(state("SELECT COUNT(DISTINCT w) FROM gv") == "0A000");
  CHECK(run("SELECT COUNT(DISTINCT v), SUM(DISTINCT v) FROM g") == Grid{{i(4), i(110)}});
}

TEST_CASE("embedded API: a filter on a DOUBLE or REAL compares in PostgreSQL's float8 order", "[sql][api]") {
  sql_test::TempDir dir{"api_double_compare"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  auto ids = [&](std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::int64_t> out;
    for (const auto& r : run(sql, params)) out.push_back(std::get<std::int64_t>(r[0]));
    std::ranges::sort(out);
    return out;
  };
  using Ids = std::vector<std::int64_t>;
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const auto inf = std::numeric_limits<double>::infinity();
  (void)run("CREATE TABLE fc (id INTEGER PRIMARY KEY, f DOUBLE PRECISION, r REAL, g INTEGER)");
  const std::vector<std::pair<int, std::optional<double>>> rows{{1, nan}, {2, -0.0}, {3, 0.0}, {4, 1.5}, {5, inf}, {6, -inf}, {7, {}}};
  for (const auto& [id, f] : rows) {
    auto v = f ? Value{*f} : kNull;
    (void)db->execute("INSERT INTO fc VALUES ($1, $2, $3, $4)", {i(id), v, v, i(id % 2)});
  }
  // NaN equals NaN and is above +inf; -0.0 equals 0.0; NULL matches nothing.
  CHECK(ids("SELECT id FROM fc WHERE f = $1", {Value{nan}}) == Ids{1});
  CHECK(ids("SELECT id FROM fc WHERE f > $1", {Value{inf}}) == Ids{1});
  CHECK(ids("SELECT id FROM fc WHERE f = $1", {Value{0.0}}) == Ids{2, 3});
  CHECK(ids("SELECT id FROM fc WHERE f = $1", {Value{-0.0}}) == Ids{2, 3});
  CHECK(ids("SELECT id FROM fc WHERE f < $1", {Value{0.0}}) == Ids{6});
  CHECK(ids("SELECT id FROM fc WHERE f >= $1 AND id >= $2", {Value{1.0}, i(0)}) == Ids{1, 4, 5});
  CHECK(ids("SELECT id FROM fc WHERE f <> $1", {Value{nan}}) == Ids{2, 3, 4, 5, 6});
  CHECK(ids("SELECT id FROM fc WHERE NOT (f < $1) OR f IS NULL", {Value{1.5}}) == Ids{1, 4, 5, 7});
  CHECK(ids("SELECT id FROM fc WHERE f BETWEEN $1 AND $2", {Value{-0.0}, Value{inf}}) == Ids{2, 3, 4, 5});
  CHECK(ids("SELECT id FROM fc WHERE f IN ($1, $2)", {Value{nan}, Value{0.0}}) == Ids{1, 2, 3});
  // A REAL compares the float of the parameter, as PostgreSQL types $n real.
  (void)db->execute("UPDATE fc SET r = $1 WHERE id = $2", {Value{0.1}, i(4)});
  CHECK(ids("SELECT id FROM fc WHERE r = $1", {Value{0.1}}) == Ids{4});
  CHECK(ids("SELECT id FROM fc WHERE r <= $1 AND r > $2", {Value{0.1}, Value{0.0}}) == Ids{4});
  CHECK(ids("SELECT id FROM fc WHERE r = $1", {Value{nan}}) == Ids{1});
  // And a REAL constant, the float its text rounds to.
  CHECK(ids("SELECT id FROM fc WHERE r = '0.1'") == Ids{4});
  CHECK(ids("SELECT id FROM fc WHERE r IN ('0.1', 'NaN')") == Ids{1, 4});
  // Joined: a DOUBLE equality filters the pairs, NaN with NaN and -0.0 with 0.0.
  CHECK(run("SELECT a.id, b.id FROM fc a JOIN fc b ON a.f = b.f WHERE a.id < b.id") == Grid{{i(2), i(3)}});
  CHECK(run("SELECT a.id, b.id FROM fc a LEFT JOIN fc b ON a.f = b.f AND a.id < b.id WHERE a.id <= $1 ORDER BY a.id", {i(3)}) ==
        Grid{{i(1), kNull}, {i(2), i(3)}, {i(3), kNull}});
  // HAVING on a grouped DOUBLE and on aggregates of one.
  CHECK(run("SELECT f FROM fc GROUP BY f HAVING f > $1 ORDER BY f LIMIT 2", {Value{1.0}}) == Grid{{Value{1.5}}, {Value{inf}}});
  CHECK(run("SELECT count(*) FROM fc GROUP BY f HAVING f >= $1", {Value{nan}}) == Grid{{i(1)}});
  CHECK(run("SELECT g FROM fc GROUP BY g HAVING max(f) = $1", {Value{nan}}) == Grid{{i(1)}});
  CHECK(run("SELECT g FROM fc GROUP BY g HAVING min(f) < $1", {Value{0.0}}) == Grid{{i(0)}});
  CHECK(run("SELECT g FROM fc WHERE id >= $1 AND id <= $2 GROUP BY g HAVING sum(f) >= $3", {i(2), i(4), Value{1.5}}) == Grid{{i(0)}});
}

TEST_CASE("embedded API: two columns of one table compare as a filter of its rows", "[sql][api]") {
  sql_test::TempDir dir{"api_column_compare"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  auto ids = [&](std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::int64_t> out;
    for (const auto& r : run(sql, params)) out.push_back(std::get<std::int64_t>(r[0]));
    std::ranges::sort(out);
    return out;
  };
  using Ids = std::vector<std::int64_t>;
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  (void)run("CREATE TABLE cc (id INTEGER PRIMARY KEY, i INTEGER, s SMALLINT, b BIGINT, f DOUBLE PRECISION, r REAL, "
            "t TEXT, v VARCHAR(8), c CHAR(4))");
  auto row = [&](int id, Value iv, Value sv, Value bv, Value fv, Value rv, Value tv, Value vv, Value cv) {
    (void)db->execute("INSERT INTO cc VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)", {i(id), iv, sv, bv, fv, rv, tv, vv, cv});
  };
  auto text = [](std::string_view x) { return Value{sql_test::bytes(x)}; };
  row(1, i(1), i(1), i(1), Value{1.5}, Value{1.0}, text("a"), text("a"), text("a"));
  row(2, i(2), i(3), i(2), Value{2.0}, Value{2.0}, text("b "), text("b"), text("b"));
  row(3, i(3), i(2), i(9007199254740993), Value{9007199254740992.0}, Value{nan}, text("c"), text("d"), text("c"));
  row(4, i(16777217), kNull, i(4), Value{nan}, Value{16777216.0}, kNull, text("e"), text("e"));
  row(5, i(0), i(0), i(0), Value{-0.0}, Value{0.0}, text("x"), text("x"), kNull);
  // An integer with a DOUBLE or REAL: as float8, PostgreSQL's cast of the
  // integer, NaN above every number and -0.0 equal to 0.0.
  CHECK(ids("SELECT id FROM cc WHERE i >= f") == Ids{2, 5});
  CHECK(ids("SELECT id FROM cc WHERE i < f") == Ids{1, 3, 4});
  CHECK(ids("SELECT id FROM cc WHERE f = i") == Ids{2, 5});
  CHECK(ids("SELECT id FROM cc WHERE b = f") == Ids{2, 3, 5});  // 2^53 + 1 is 2^53 as a float8
  CHECK(ids("SELECT id FROM cc WHERE i = r") == Ids{1, 2, 5});  // 2^24 + 1 is not 2^24 as a float8
  CHECK(ids("SELECT id FROM cc WHERE r > f") == Ids{3});
  CHECK(ids("SELECT id FROM cc WHERE r = f") == Ids{2, 5});
  // Integers of any width; the operator as written; inside AND, OR and NOT.
  CHECK(ids("SELECT id FROM cc WHERE s < i") == Ids{3});
  CHECK(ids("SELECT id FROM cc WHERE i <> b") == Ids{3, 4});
  CHECK(ids("SELECT id FROM cc WHERE NOT (s = b) OR id = $1", {i(1)}) == Ids{1, 2, 3});
  CHECK(ids("SELECT id FROM cc WHERE id >= $1 AND i = b", {i(2)}) == Ids{2, 5});
  // TEXT with VARCHAR and CHAR(n) as text: the CHAR's spaces cut, the other's kept.
  CHECK(ids("SELECT id FROM cc WHERE t = v") == Ids{1, 5});
  CHECK(ids("SELECT id FROM cc WHERE c = t") == Ids{1, 3});
  CHECK(ids("SELECT id FROM cc WHERE v > t") == Ids{3});
  // In a join, of one table's rows, and in UPDATE and DELETE.
  CHECK(run("SELECT a.id, b.id FROM cc a JOIN cc b ON a.id = b.i AND b.s < b.b ORDER BY a.id") == Grid{{i(3), i(3)}});
  CHECK(run("SELECT a.id, b.id FROM cc a LEFT JOIN cc b ON a.id = b.id AND b.i < b.f WHERE a.id <= $1 ORDER BY a.id", {i(2)}) ==
        Grid{{i(1), i(1)}, {i(2), kNull}});
  CHECK(db->execute("UPDATE cc SET s = b WHERE s > i").affected == 1);
  CHECK(db->execute("DELETE FROM cc WHERE i > r").affected == 1);
  CHECK(ids("SELECT id FROM cc WHERE s = b") == Ids{1, 2, 5});
}

TEST_CASE("embedded API: the distinct values of an aggregate are held in the statement's memory and fail past it", "[sql][api][aggregate]") {
  sql_test::TempDir dir{"api_distinct_agg_mem"};
  auto db = Database::open(dir.path(), {.statement_memory_bytes = 64u << 10});
  (void)db->execute("CREATE TABLE big (id INTEGER PRIMARY KEY, x INTEGER)");
  auto txn = db->begin();
  for (int k = 0; k < 2000; ++k) (void)txn.execute("INSERT INTO big VALUES ($1, $1)", {i(k)});
  (void)txn.commit();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  CHECK(state("SELECT SUM(DISTINCT x) FROM big") == "54000");
  CHECK(state("SELECT AVG(DISTINCT x) FROM big") == "54000");
  CHECK(state("SELECT SUM(DISTINCT x) FROM big WHERE id < 10") == "ran");
  // MIN and MAX of the distinct values are those of all of them: nothing is held.
  CHECK(state("SELECT MIN(DISTINCT x), MAX(DISTINCT x) FROM big") == "ran");
}

TEST_CASE("embedded API: ORDER BY of a set operation's rows sorts them as a SELECT's, and a LIMIT keeps the first", "[sql][api][setop]") {
  sql_test::TempDir dir{"api_setop_order"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  (void)run("CREATE TABLE oa (id INTEGER PRIMARY KEY, x INTEGER, f DOUBLE PRECISION)");
  (void)run("CREATE TABLE ob (id BIGINT PRIMARY KEY, y BIGINT, f DOUBLE PRECISION)");
  for (auto [id, x] : std::vector<std::pair<int, std::optional<int>>>{{1, 3}, {2, 1}, {3, {}}, {4, 2}}) {
    (void)db->execute("INSERT INTO oa VALUES ($1, $2, $3)", {i(id), x ? i(*x) : kNull, x ? Value{*x * 0.5} : kNull});
  }
  for (auto [id, y] : std::vector<std::pair<int, std::optional<int>>>{{1, 2}, {2, 5}, {3, {}}}) {
    (void)db->execute("INSERT INTO ob VALUES ($1, $2, $3)", {i(id), y ? i(*y) : kNull, y ? Value{*y * 0.5} : kNull});
  }
  // NULLS LAST ascending, FIRST descending; an INTEGER meeting a BIGINT is BIGINT.
  CHECK(run("SELECT x FROM oa UNION ALL SELECT y FROM ob ORDER BY x") ==
        Grid{{i(1)}, {i(2)}, {i(2)}, {i(3)}, {i(5)}, {kNull}, {kNull}});
  CHECK(run("SELECT x FROM oa UNION ALL SELECT y FROM ob ORDER BY 1 DESC") ==
        Grid{{kNull}, {kNull}, {i(5)}, {i(3)}, {i(2)}, {i(2)}, {i(1)}});
  CHECK(run("SELECT x FROM oa UNION ALL SELECT y FROM ob ORDER BY x DESC NULLS LAST LIMIT 3") ==
        Grid{{i(5)}, {i(3)}, {i(2)}});
  CHECK(run("SELECT x FROM oa UNION SELECT y FROM ob ORDER BY x") == Grid{{i(1)}, {i(2)}, {i(3)}, {i(5)}, {kNull}});
  CHECK(run("SELECT x FROM oa EXCEPT SELECT y FROM ob ORDER BY x") == Grid{{i(1)}, {i(3)}});
  CHECK(run("SELECT x FROM oa INTERSECT SELECT y FROM ob ORDER BY x") == Grid{{i(2)}, {kNull}});
  // Two keys, the second descending; a column the key does not name rides along.
  CHECK(run("SELECT f, x FROM oa UNION ALL SELECT f, y FROM ob ORDER BY 1 DESC, x") ==
        Grid{{kNull, kNull}, {kNull, kNull}, {Value{2.5}, i(5)}, {Value{1.5}, i(3)}, {Value{1.0}, i(2)}, {Value{1.0}, i(2)},
             {Value{0.5}, i(1)}});
  // The plan sorts once, keeping only a LIMIT's rows.
  CHECK(db->prepare("SELECT x FROM oa UNION ALL SELECT y FROM ob ORDER BY x LIMIT 3").plan_source().find("bc_sorter_open(cx, 3)") !=
        std::string::npos);
  CHECK(db->prepare("SELECT x FROM oa UNION ALL SELECT y FROM ob ORDER BY x").plan_source().find("bc_sorter_open(cx, 0)") !=
        std::string::npos);
  // A subquery's set operation orders its rows too.
  CHECK(run("SELECT id FROM oa WHERE x = (SELECT y FROM ob UNION ALL SELECT 7 ORDER BY 1 LIMIT 1) ORDER BY id") ==
        Grid{{i(4)}});
  // A DOUBLE's sign of zero survives the sort.
  (void)run("INSERT INTO oa VALUES (9, 0, $1)", {Value{-0.0}});
  auto z = run("SELECT f FROM oa WHERE id = 9 UNION ALL SELECT f FROM ob WHERE id = 9 ORDER BY f");
  REQUIRE(z.size() == 1);
  CHECK(std::signbit(std::get<double>(z[0][0])));
  // Names and positions are checked as PostgreSQL checks them.
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  CHECK(state("SELECT x FROM oa UNION SELECT y FROM ob ORDER BY 2") == "42P10");
  CHECK(state("SELECT x FROM oa UNION SELECT y FROM ob ORDER BY y") == "42703");
  CHECK(state("SELECT x FROM oa UNION SELECT y FROM ob ORDER BY oa.x") == "42P01");
  CHECK(state("SELECT x, x FROM oa UNION SELECT y, y FROM ob ORDER BY x") == "42702");
  CHECK(state("SELECT x FROM oa UNION SELECT y FROM ob ORDER BY x + 1") == "0A000");  // only output columns, as PostgreSQL
}

TEST_CASE("embedded API: ORDER BY an expression of the FROM columns sorts by its value per row", "[sql][api]") {
  sql_test::TempDir dir{"api_order_expr"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql, const std::vector<Value>& params = {}) { return db->execute(sql, params).rows; };
  (void)run("CREATE TABLE oe (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, s SMALLINT)");
  (void)run("CREATE TABLE of2 (id INTEGER PRIMARY KEY, a INTEGER)");
  (void)run("CREATE INDEX oe_a ON oe (a)");
  (void)run("INSERT INTO oe VALUES (1, 30, 'x', 200), (2, 10, 'z', 300), (3, NULL, 'y', 1), (4, 20, 'y', -5)");
  (void)run("INSERT INTO of2 VALUES (1, 5), (2, 6)");
  // NULL sorts last ascending and first descending, as a column's does.
  CHECK(run("SELECT id FROM oe ORDER BY a + 5") == Grid{{i(2)}, {i(4)}, {i(1)}, {i(3)}});
  CHECK(run("SELECT id FROM oe ORDER BY -a") == Grid{{i(1)}, {i(4)}, {i(2)}, {i(3)}});
  CHECK(run("SELECT id FROM oe ORDER BY -a DESC NULLS LAST") == Grid{{i(2)}, {i(4)}, {i(1)}, {i(3)}});
  CHECK(run("SELECT id FROM oe ORDER BY a % 7 NULLS FIRST, id") == Grid{{i(3)}, {i(1)}, {i(2)}, {i(4)}});
  CHECK(run("SELECT id FROM oe ORDER BY b || 'a' DESC, id") == Grid{{i(2)}, {i(3)}, {i(4)}, {i(1)}});
  CHECK(run("SELECT id FROM oe ORDER BY coalesce(a, 0), id") == Grid{{i(3)}, {i(2)}, {i(4)}, {i(1)}});
  // An index on a gives a's order, never -a's: the plan sorts, keeping LIMIT's rows.
  CHECK(run("SELECT id FROM oe WHERE a > 5 ORDER BY -a LIMIT 2") == Grid{{i(1)}, {i(4)}});
  CHECK(db->prepare("SELECT id FROM oe WHERE a > 5 ORDER BY -a LIMIT 2").plan_source().find("bc_sorter_open(cx, 2)") !=
        std::string::npos);
  // An output name or a position of an expression orders by the expression.
  CHECK(run("SELECT -a AS a FROM oe ORDER BY a") == Grid{{i(-30)}, {i(-20)}, {i(-10)}, {kNull}});
  CHECK(run("SELECT a + 1 FROM oe ORDER BY 1 DESC") == Grid{{kNull}, {i(31)}, {i(21)}, {i(11)}});
  // Columns of two joined tables, and a constant key, which orders nothing.
  CHECK(run("SELECT oe.id FROM oe JOIN of2 ON oe.id = of2.id ORDER BY oe.a + of2.a") == Grid{{i(2)}, {i(1)}});
  CHECK(run("SELECT id FROM oe ORDER BY 1 + 1, id DESC") == Grid{{i(4)}, {i(3)}, {i(2)}, {i(1)}});
  // An alias's column inside a function's args.
  CHECK(run("SELECT x.id FROM oe x ORDER BY abs(x.a - 25), x.id") == Grid{{i(1)}, {i(4)}, {i(2)}, {i(3)}});
  // A key's error is the statement's: 200 * 200 is past SMALLINT.
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  CHECK(state("SELECT id FROM oe ORDER BY s * s") == "22003");
  CHECK(state("SELECT id FROM oe ORDER BY id / 0") == "22012");
  // DISTINCT orders by selected items only (42P10); two output names of
  // different expressions are ambiguous (42702).
  CHECK(state("SELECT DISTINCT a FROM oe ORDER BY a + 1") == "42P10");
  CHECK(state("SELECT a + 1 AS x, a + 2 AS x FROM oe ORDER BY x") == "42702");
  // Not yet: random(), a whole row, and an expression key of an aggregated query.
  CHECK(state("SELECT id FROM oe ORDER BY random(1, 9)") == "42601");
  CHECK(state("SELECT id FROM oe ORDER BY oe.*") == "42601");
  CHECK(state("SELECT a FROM oe GROUP BY a ORDER BY a + 1") == "42601");
  CHECK(state("SELECT count(*) AS n FROM oe ORDER BY n") == "42601");
  // A correlated subquery is refused (42601) inside a function's args too,
  // whether the subquery is the arg or the outer column is an arg within it.
  CHECK(state("SELECT id FROM oe ORDER BY coalesce((SELECT a FROM of2 WHERE of2.id = oe.id), 0)") == "42601");
  CHECK(state("SELECT id FROM oe ORDER BY (SELECT coalesce(oe.a, 0) FROM of2 WHERE of2.id = 1)") == "42601");
  // A view's column that is an expression, and a view's own ORDER BY of one.
  (void)run("CREATE VIEW ov AS SELECT id, -a AS na FROM oe");
  CHECK(run("SELECT id FROM ov ORDER BY na + 1 DESC NULLS LAST") == Grid{{i(2)}, {i(4)}, {i(1)}, {i(3)}});
  (void)run("CREATE VIEW ov2 AS SELECT id FROM oe ORDER BY a * -1");
  CHECK(run("SELECT id FROM ov2") == Grid{{i(1)}, {i(4)}, {i(2)}, {i(3)}});
}

TEST_CASE("embedded API: ORDER BY of a set operation's rows is charged to the statement's budget, a LIMIT bounds it", "[sql][api][setop]") {
  sql_test::TempDir dir{"api_setop_order_mem"};
  auto db = Database::open(dir.path(), {.statement_memory_bytes = 64u << 10});
  (void)db->execute("CREATE TABLE big (id INTEGER PRIMARY KEY, x INTEGER)");
  auto txn = db->begin();
  for (int k = 0; k < 2000; ++k) (void)txn.execute("INSERT INTO big VALUES ($1, $1)", {i(k)});
  (void)txn.commit();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  // The sorter holds every row of the combined arms; past the budget the statement fails, never spilling.
  CHECK(state("SELECT x FROM big UNION ALL SELECT x FROM big ORDER BY x") == "54000");
  // A LIMIT holds only its rows, though every row is read.
  CHECK(state("SELECT x FROM big UNION ALL SELECT x FROM big ORDER BY x DESC LIMIT 5") == "ran");
  CHECK(db->execute("SELECT x FROM big UNION ALL SELECT x FROM big ORDER BY x DESC LIMIT 3").rows ==
        Grid{{i(1999)}, {i(1999)}, {i(1998)}});
}

TEST_CASE("embedded API: the time is read once per statement, and once per transaction", "[sql][api][time]") {
  sql_test::TempDir dir{"api_clock"};
  auto db = Database::open(dir.path());
  constexpr std::int64_t kDay = 86'400'000'000;
  std::int64_t now = 20'000 * kDay + 3'723'456'789;  // 1 02:03:03.456789 into day 20000
  db->clock_for_testing([&] { return now; });
  auto first = [&](std::string_view sql, Database& d) { return d.execute(sql).rows.at(0); };

  CHECK(first("SELECT CURRENT_DATE", *db) == std::vector<Value>{i(20'000)});
  CHECK(first("SELECT LOCALTIME", *db) == std::vector<Value>{i(3'723'456'789 % kDay)});
  CHECK(first("SELECT LOCALTIME(0), LOCALTIME(2), LOCALTIMESTAMP(1)", *db) ==
        std::vector<Value>{i(3'723'000'000 + 1'000'000 * 0), i(3'723'460'000), i(20'000 * kDay + 3'723'500'000)});
  // Two readings in a statement are one instant; the cached plan reads the clock again.
  CHECK(first("SELECT LOCALTIMESTAMP, LOCALTIMESTAMP", *db) == std::vector<Value>{i(now), i(now)});
  now += 5 * kDay;
  CHECK(first("SELECT CURRENT_DATE", *db) == std::vector<Value>{i(20'005)});

  // A transaction's statements read the time it began, however late they run.
  auto t = db->begin();
  CHECK(t.execute("SELECT CURRENT_DATE").rows.at(0) == std::vector<Value>{i(20'005)});
  now += 3 * kDay;
  CHECK(t.execute("SELECT CURRENT_DATE").rows.at(0) == std::vector<Value>{i(20'005)});
  t.rollback();
  CHECK(first("SELECT CURRENT_DATE", *db) == std::vector<Value>{i(20'008)});

  // Before 1970 the day is the floor and the time of day stays positive.
  now = -3'600'000'000;  // 1969-12-31 23:00:00
  CHECK(first("SELECT CURRENT_DATE, LOCALTIME", *db) == std::vector<Value>{i(-1), i(kDay - 3'600'000'000)});

  // CURRENT_TIMESTAMP is a timestamptz: the same instant, UTC. CURRENT_TIME
  // is a TIME WITH TIME ZONE: the time of day at the session's zone, UTC,
  // offset 0, rounded to its precision; one instant with the others.
  now = 1'793'095'123'456'789;  // 2026-10-26 09:58:43.456789 UTC
  CHECK(first("SELECT CURRENT_TIMESTAMP, now(), CURRENT_TIMESTAMP(0), LOCALTIMESTAMP", *db) ==
        std::vector<Value>{i(now), i(now), i(now - 456'789), i(now)});
  CHECK(db->execute("SELECT now()").types.at(0).kind == SqlKind::TimestampTz);
  auto day_time = now % kDay;  // 09:58:43.456789
  CHECK(first("SELECT CURRENT_TIME, CURRENT_TIME(0), CURRENT_TIME(2), LOCALTIME", *db) ==
        std::vector<Value>{i(timetz_pack({day_time, 0})), i(timetz_pack({day_time - 456'789, 0})),
                           i(timetz_pack({day_time + 3'211, 0})), i(day_time)});
  auto r = db->execute("SELECT CURRENT_TIME(0)");
  CHECK(r.types.at(0).kind == SqlKind::TimeTz);
  CHECK(to_text(r.rows.at(0).at(0), r.types.at(0)) == "09:58:43+00");
  // Rounded up past the last second of the day, it is 24:00:00, as in PostgreSQL.
  now = 20'000 * kDay + kDay - 300'000;
  r = db->execute("SELECT CURRENT_TIME(0)");
  CHECK(to_text(r.rows.at(0).at(0), r.types.at(0)) == "24:00:00+00");
}

TEST_CASE("embedded API: a WHERE compares a column with the clock, read on each run", "[sql][api][time]") {
  sql_test::TempDir dir{"api_clock_where"};
  auto db = Database::open(dir.path());
  constexpr std::int64_t kDay = 86'400'000'000;
  std::int64_t now = 20'000 * kDay + 3'600'000'000;  // 01:00 of day 20000
  db->clock_for_testing([&] { return now; });
  (void)db->execute("CREATE TABLE ev (id INTEGER PRIMARY KEY, d DATE, t TIME, ts TIMESTAMP, tz TIMESTAMPTZ)");
  (void)db->execute("CREATE INDEX ev_d ON ev (d)");
  for (std::int64_t k = 1; k <= 3; ++k) {
    auto at = (19'999 + k) * kDay;  // days 20000, 20001, 20002 at midnight
    (void)db->execute("INSERT INTO ev VALUES ($1, $2, $3, $4, $5)", {i(k), i(19'999 + k), i(k * 3'600'000'000 - 1), i(at), i(at)});
  }
  auto ids = [&](std::string_view sql) {
    std::vector<std::int64_t> out;
    for (const auto& row : db->execute(sql).rows) out.push_back(std::get<std::int64_t>(row.at(0)));
    return out;
  };
  using V = std::vector<std::int64_t>;
  CHECK(ids("SELECT id FROM ev WHERE d = CURRENT_DATE") == V{1});
  CHECK(ids("SELECT id FROM ev WHERE CURRENT_DATE < d ORDER BY id") == V{2, 3});  // written clock first
  CHECK(ids("SELECT id FROM ev WHERE t < LOCALTIME") == V{1});
  CHECK(ids("SELECT id FROM ev WHERE ts <= LOCALTIMESTAMP ORDER BY id") == V{1});
  CHECK(ids("SELECT id FROM ev WHERE tz > now() ORDER BY id") == V{2, 3});
  // Either timestamp with either: the session's zone is UTC.
  CHECK(ids("SELECT id FROM ev WHERE ts < CURRENT_TIMESTAMP AND tz < LOCALTIMESTAMP(0) ORDER BY id") == V{1});
  CHECK(ids("SELECT id FROM ev WHERE d = CURRENT_DATE OR id = 3 ORDER BY id") == V{1, 3});
  CHECK(ids("SELECT id FROM ev WHERE id IN (SELECT id FROM ev WHERE d > CURRENT_DATE) ORDER BY id") == V{2, 3});

  // The cached plan reads the clock again on each run.
  now += kDay;
  CHECK(ids("SELECT id FROM ev WHERE d = CURRENT_DATE") == V{2});
  CHECK(ids("SELECT id FROM ev WHERE CURRENT_DATE < d ORDER BY id") == V{3});
  CHECK(db->execute("UPDATE ev SET t = NULL WHERE d < CURRENT_DATE").affected == 1);
  CHECK(db->execute("DELETE FROM ev WHERE d > CURRENT_DATE").affected == 1);
  CHECK(ids("SELECT id FROM ev ORDER BY id") == V{1, 2});
  CHECK(ids("SELECT id FROM ev WHERE t IS NULL") == V{1});

  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)} + " " + e.what();
    }
    return "ran";
  };
  // No operator in PostgreSQL: 42883, the types in the order written.
  CHECK(state("SELECT id FROM ev WHERE id < CURRENT_DATE").starts_with("42883 operator does not exist: integer < date"));
  CHECK(state("SELECT id FROM ev WHERE CURRENT_DATE = t").starts_with(
      "42883 operator does not exist: date = time without time zone"));
  CHECK(state("SELECT id FROM ev WHERE t < LOCALTIMESTAMP").starts_with(
      "42883 operator does not exist: time without time zone < timestamp without time zone"));
  // A date with a timestamp compares the date's midnight: not yet.
  CHECK(state("SELECT id FROM ev WHERE d < CURRENT_TIMESTAMP").starts_with("42601"));
  CHECK(state("SELECT id FROM ev WHERE ts < CURRENT_DATE").starts_with("42601"));
  CHECK(state("SELECT id FROM ev WHERE t < CURRENT_TIME").starts_with("42601"));
  // <> and != are written NOT (=); the error names <>, as PostgreSQL's does.
  CHECK(state("SELECT id FROM ev WHERE id <> CURRENT_DATE").starts_with("42883 operator does not exist: integer <> date"));
  CHECK(state("SELECT id FROM ev WHERE CURRENT_TIMESTAMP != id").starts_with(
      "42883 operator does not exist: timestamp with time zone <> integer"));
  CHECK(state("SELECT id FROM ev WHERE id <> DATE '2020-01-01'").starts_with("42883 operator does not exist: integer <> date"));
  // The time compared with anything but a column is not built yet.
  CHECK(state("SELECT id FROM ev WHERE CURRENT_DATE > DATE '2020-01-01'").starts_with(
      "42601 comparing the time with a constant, a parameter or the time is not supported yet"));
  CHECK(state("SELECT id FROM ev WHERE CURRENT_DATE = CURRENT_DATE").starts_with("42601 comparing the time"));

  // A procedure's statements keep no clock slot: the first CALL, which
  // binds the body, refuses the time in its WHERE.
  (void)db->execute("CREATE PROCEDURE purge() LANGUAGE plpgsql AS $$ BEGIN DELETE FROM ev WHERE d < CURRENT_DATE; END $$");
  (void)db->execute("CREATE PROCEDURE touch() LANGUAGE plpgsql AS $$ BEGIN UPDATE ev SET t = NULL WHERE ts < now(); END $$");
  auto call = [&](std::string_view name) -> std::string {
    try {
      (void)db->call(name, {});
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)} + " " + e.what();
    }
    return "ran";
  };
  CHECK(call("purge").starts_with("0A000 the time (CURRENT_DATE, CURRENT_TIMESTAMP, now(), LOCALTIME, LOCALTIMESTAMP)"));
  CHECK(call("touch").starts_with("0A000 the time"));
  CHECK(ids("SELECT id FROM ev ORDER BY id") == V{1, 2});
}

TEST_CASE("embedded API: a TIME WITH TIME ZONE column keeps its time and offset, ordered as PostgreSQL's timetz", "[sql][api][timetz]") {
  sql_test::TempDir dir{"api_timetz"};
  constexpr std::int64_t kHour = 3'600'000'000;
  std::int64_t now = 20'000 * 86'400'000'000LL + 9 * kHour + 123'456;  // 09:00:00.123456 UTC
  auto text = [](Database& d, std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::string> out;
    auto r = d.execute(std::string{sql}, params);
    for (const auto& row : r.rows) {
      std::string line;
      for (std::size_t c = 0; c < row.size(); ++c) line += (c == 0 ? "" : "|") + to_text(row.at(c), r.types.at(c));
      out.push_back(line);
    }
    return out;
  };
  using S = std::vector<std::string>;
  {
    auto db = Database::open(dir.path());
    db->clock_for_testing([&] { return now; });
    (void)db->execute(
        "CREATE TABLE tz (t TIME WITH TIME ZONE PRIMARY KEY, n INTEGER, at TIMETZ DEFAULT CURRENT_TIME(0), "
        "lt TIMETZ DEFAULT LOCALTIME, ct TIMETZ DEFAULT CURRENT_TIMESTAMP, plain TIME DEFAULT CURRENT_TIME)");
    (void)db->execute("CREATE INDEX tz_n ON tz (n)");
    // Text with no offset is at the session's zone, UTC.
    (void)db->execute("INSERT INTO tz (t, n) VALUES ('12:00+01', 1), ('11:00', 2), ('10:30:00.5-00:30', 3), "
                      "('24:00:00-15:59:59', 4), ('00:00+15:59:59', 5)");
    // The UTC time first, then the offset east of UTC, highest first: 12:00+01 before 11:00+00.
    CHECK(text(*db, "SELECT t, n FROM tz ORDER BY t") ==
          S{"00:00:00+15:59:59|5", "12:00:00+01|1", "11:00:00+00|2", "10:30:00.5-00:30|3", "24:00:00-15:59:59|4"});
    CHECK(text(*db, "SELECT n FROM tz WHERE t > '11:00+00' ORDER BY t") == S{"3", "4"});
    CHECK(text(*db, "SELECT n FROM tz WHERE t = '11:00:00Z'") == S{"2"});
    // Equal only with the same offset: 12:00+01 is not 11:00+00.
    CHECK(text(*db, "SELECT n FROM tz WHERE t = '12:00+01:00'") == S{"1"});
    CHECK_THROWS(db->execute("INSERT INTO tz (t, n) VALUES ('12:00:00+01', 9)"));
    (void)db->execute("INSERT INTO tz (t, n) VALUES ('11:00+00:01', 6)");  // another offset: another key
    // A time compared with it is at offset 0.
    CHECK(text(*db, "SELECT n FROM tz WHERE t <= TIME '11:00' ORDER BY t") == S{"5", "6", "1", "2"});
    CHECK(text(*db, "SELECT min(t), max(t) FROM tz") == S{"00:00:00+15:59:59|24:00:00-15:59:59"});
    // A parameter is the packed value; the defaults are the statement's reading at offset 0.
    CHECK(text(*db, "SELECT n FROM tz WHERE t = $1", {Value{timetz_pack({10 * kHour + kHour / 2 + 500'000, -1800})}}) == S{"3"});
    CHECK(text(*db, "SELECT at, lt, ct, plain FROM tz WHERE n = 1") ==
          S{"09:00:00+00|09:00:00.123456+00|09:00:00.123456+00|09:00:00.123456"});
    CHECK(text(*db, "SELECT n FROM tz WHERE lt = CURRENT_TIME AND at <= LOCALTIME ORDER BY n") == S{"1", "2", "3", "4", "5", "6"});
    // In a select list too, LOCALTIME read as a time with time zone beside one.
    CHECK(text(*db, "SELECT lt = LOCALTIME, CURRENT_TIME = LOCALTIME, ct = CURRENT_TIME FROM tz WHERE n = 1") == S{"t|t|t"});
    // Casts: to a time drops the offset, a time or a timestamptz is at offset 0.
    CHECK(text(*db, "SELECT CAST(TIMETZ '12:34:56.5-03:30' AS TIME), CAST(TIME '01:02' AS TIMETZ), "
                    "CAST(TIMESTAMPTZ '2026-10-27 10:00:00+02' AS TIMETZ), CAST(TIMETZ '12:00+05' AS TEXT)") ==
          S{"12:34:56.5|01:02:00+00|08:00:00+00|12:00:00+05"});
    auto state = [&](std::string_view sql) -> std::string {
      try {
        (void)db->execute(sql);
      } catch (const std::exception& e) {
        return std::string{sqlstate(e)};
      }
      return "ran";
    };
    auto message = [&](std::string_view sql) -> std::string {
      try {
        (void)db->execute(sql);
      } catch (const std::exception& e) {
        return std::string{sqlstate(e)} + " " + e.what();
      }
      return "ran";
    };
    // Assigned, a timestamptz is its UTC time at offset 0, as PostgreSQL's assignment cast.
    CHECK(state("INSERT INTO tz (t, n) VALUES (TIMESTAMPTZ '2020-01-01 05:00+03', 7)") == "ran");
    CHECK(text(*db, "SELECT t FROM tz WHERE n = 7") == S{"02:00:00+00"});
    (void)db->execute("DELETE FROM tz WHERE n = 7");
    // A pair with no assignment cast is 42804, worded for a value, not a DEFAULT.
    CHECK(message("INSERT INTO tz (t, n) VALUES (TIMESTAMP '2020-01-01 05:00', 8)") ==
          "42804 column \"t\" is of type time with time zone but expression is of type timestamp without time zone");
    CHECK(message("INSERT INTO tz (t, n) VALUES (CURRENT_DATE, 8)") ==
          "42804 column \"t\" is of type time with time zone but expression is of type date");
    CHECK(message("UPDATE tz SET plain = CURRENT_DATE WHERE n = 1") ==
          "42804 column \"plain\" is of type time without time zone but expression is of type date");
    CHECK(state("SELECT CAST(TIMETZ '12:00' AS TIMESTAMP)") == "42846");
    CHECK(state("SELECT CAST(LOCALTIMESTAMP AS TIMETZ)") == "42601");  // a cast of the clock: not yet
    CHECK(state("SELECT CAST(TIMESTAMP '2020-01-01' AS TIMETZ)") == "42846");
    CHECK(state("SELECT TIMETZ '12:00+16'") == "22009");
    CHECK(state("SELECT TIMETZ '24:00:01'") == "22008");
    CHECK(state("SELECT TIMETZ '12:00 PST'") == "22007");  // no zone names: the layer has no time zone database
    CHECK(state("SELECT TIMETZ '12:00' = DATE '2020-01-01'") == "42883");
    CHECK(state("SELECT TIMETZ '12:00' - TIMETZ '11:00'") == "42883");
    CHECK(state("SELECT TIMETZ '12:00' + '1 hour'") == "42725");
    CHECK(state("SELECT TIMETZ '12:00' - '1 hour'") == "42601");  // an interval: not yet
    CHECK(state("SELECT TIMETZ '12:00' + TIME '01:00'") == "42601");
    CHECK(state("SELECT sum(t) FROM tz") == "42883");
    CHECK(state("SELECT EXTRACT(HOUR FROM t) FROM tz") == "0A000");
    CHECK(state("SELECT n FROM tz WHERE plain < CURRENT_TIME") == "42601");  // a time read as a timetz: not yet
  }
  auto db = Database::open(dir.path());
  CHECK(text(*db, "SELECT t FROM tz WHERE n = 3") == S{"10:30:00.5-00:30"});
  CHECK(text(*db, "SELECT n FROM tz WHERE t < '11:00+00' ORDER BY t") == S{"5", "6", "1"});
}

TEST_CASE("embedded API: a TIMESTAMP WITH TIME ZONE column keeps the instant, in UTC, across a reopen", "[sql][api][timestamptz]") {
  sql_test::TempDir dir{"api_timestamptz"};
  auto text = [](Database& d, std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::string> out;
    auto r = d.execute(std::string{sql}, params);
    for (const auto& row : r.rows) out.push_back(to_text(row.at(0), r.types.at(0)));
    return out;
  };
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE ev (id INTEGER PRIMARY KEY, at TIMESTAMPTZ, plain TIMESTAMP)");
    (void)db->execute("CREATE INDEX ev_at ON ev (at)");
    // An offset is taken off; text without one is UTC.
    (void)db->execute("INSERT INTO ev VALUES (1, '2026-10-27 10:00:00+02', '2026-10-27 08:00:00')");
    (void)db->execute("INSERT INTO ev VALUES (2, '2026-10-27 10:00:00Z', NULL)");
    (void)db->execute("INSERT INTO ev VALUES (3, '2026-10-27 10:00:00', NULL)");
    (void)db->execute("INSERT INTO ev VALUES (4, '2026-10-27 05:30:00-04:30', NULL)");
    (void)db->execute("INSERT INTO ev VALUES (5, NULL, NULL)");
    CHECK(text(*db, "SELECT at FROM ev WHERE id = 1") == std::vector<std::string>{"2026-10-27 08:00:00+00"});
    CHECK(text(*db, "SELECT at FROM ev ORDER BY at, id") == std::vector<std::string>{"2026-10-27 08:00:00+00", "2026-10-27 10:00:00+00",
                                                                                      "2026-10-27 10:00:00+00", "2026-10-27 10:00:00+00", ""});
    CHECK(text(*db, "SELECT id FROM ev WHERE at = '2026-10-27 12:00:00+02' ORDER BY id") == std::vector<std::string>{"2", "3", "4"});
    CHECK(text(*db, "SELECT min(at) FROM ev") == std::vector<std::string>{"2026-10-27 08:00:00+00"});
    CHECK(text(*db, "SELECT max(at) FROM ev") == std::vector<std::string>{"2026-10-27 10:00:00+00"});
    // Against a timestamp and a date constant, the timestamp read as UTC.
    CHECK(text(*db, "SELECT id FROM ev WHERE at = TIMESTAMP '2026-10-27 08:00:00'") == std::vector<std::string>{"1"});
    CHECK(text(*db, "SELECT id FROM ev WHERE at >= DATE '2026-10-27' AND at < DATE '2026-10-28' AND id < 3 ORDER BY id") ==
          std::vector<std::string>{"1", "2"});
    // Casts of constants.
    CHECK(text(*db, "SELECT CAST(TIMESTAMPTZ '2026-10-27 23:30:00-02' AS DATE)") == std::vector<std::string>{"2026-10-28"});
    CHECK(text(*db, "SELECT CAST(TIMESTAMPTZ '2026-10-27 10:00:00+02' AS TIMESTAMP)") == std::vector<std::string>{"2026-10-27 08:00:00"});
    CHECK(text(*db, "SELECT CAST(TIMESTAMP '2026-10-27 08:00:00' AS TIMESTAMPTZ)") == std::vector<std::string>{"2026-10-27 08:00:00+00"});
    CHECK(text(*db, "SELECT CAST(DATE '2026-10-27' AS TIMESTAMPTZ)") == std::vector<std::string>{"2026-10-27 00:00:00+00"});
    CHECK(text(*db, "SELECT CAST(TIMESTAMPTZ '2026-10-27 10:00:00+02' AS VARCHAR)") == std::vector<std::string>{"2026-10-27 08:00:00+00"});
    // A string assigned to a timestamptz column is read with its offset.
    (void)db->execute("UPDATE ev SET at = '2000-01-01 00:00:00 BC' WHERE id = 5");
    CHECK(text(*db, "SELECT at FROM ev WHERE id = 5") == std::vector<std::string>{"2000-01-01 00:00:00+00 BC"});
    // A time zone offset past 15 hours, or no digits, is refused.
    CHECK_THROWS(db->execute("INSERT INTO ev VALUES (9, '2026-10-27 10:00:00+16', NULL)"));
    CHECK_THROWS(db->execute("INSERT INTO ev VALUES (9, '2026-10-27 10:00:00+', NULL)"));
  }
  auto db = Database::open(dir.path());
  CHECK(text(*db, "SELECT at FROM ev WHERE id = 4") == std::vector<std::string>{"2026-10-27 10:00:00+00"});
  CHECK(text(*db, "SELECT id FROM ev WHERE at > '2026-10-27 09:00:00Z' ORDER BY id") == std::vector<std::string>{"2", "3", "4"});
}

TEST_CASE("embedded API: a CHAR(n) column ignores trailing spaces and is returned padded to n", "[sql][api][char]") {
  sql_test::TempDir dir{"api_char"};
  auto state = [](Database& d, std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
    try {
      (void)d.execute(std::string{sql}, params);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ok";
  };
  auto text = [](Database& d, std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::string> out;
    auto r = d.execute(std::string{sql}, params);
    for (const auto& row : r.rows) out.push_back(to_text(row.at(0), r.types.at(0)));
    return out;
  };
  using V = std::vector<std::string>;
  {
    auto db = Database::open(dir.path());
    CHECK(state(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a CHAR(5), b CHARACTER(3), c CHAR, v VARCHAR(8))") == "ok");
    CHECK(db->execute("SELECT a, b, c FROM t").types ==
          std::vector<SqlType>{{SqlKind::Char, 5}, {SqlKind::Char, 3}, {SqlKind::Char, 1}});
    CHECK(state(*db, "INSERT INTO t VALUES (1, 'ab', 'xyz', 'q', 'ab ')") == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, 'x', 'q', 'a')", {i(2), bytes("abcde   ")}) == "ok");  // spaces past n are cut
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, 'x', 'q', 'a')", {i(3), bytes("ab ")}) == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES (4, 'é', 'x', 'q', 'a')") == "ok");
    // Too long once spaces are cut: 22001, as PostgreSQL's.
    CHECK(state(*db, "INSERT INTO t VALUES (9, 'abcdef', 'x', 'q', 'a')") == "22001");
    CHECK(state(*db, "INSERT INTO t VALUES ($1, 'ab', 'x', 'q', 'a')", {i(9)}) == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES (10, 'ab', 'x', 'qq', 'a')") == "22001");
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, 'x', 'q', 'a')", {i(11), bytes("abcdef")}) == "22001");
    CHECK(text(*db, "SELECT a FROM t ORDER BY id") == V{"ab   ", "abcde", "ab   ", "é    ", "ab   "});
    CHECK(text(*db, "SELECT b FROM t WHERE id = 2") == V{"x  "});
    // A constant is compared without its trailing spaces, as a CHAR's are.
    CHECK(text(*db, "SELECT id FROM t WHERE a = 'ab' ORDER BY id") == V{"1", "3", "9"});
    CHECK(text(*db, "SELECT id FROM t WHERE a = 'ab        ' ORDER BY id") == V{"1", "3", "9"});
    CHECK(text(*db, "SELECT id FROM t WHERE a = 'é'") == V{"4"});
    CHECK(text(*db, "SELECT id FROM t WHERE a > 'ab' ORDER BY id") == V{"2", "4"});
    // Two CHARs compare without their spaces; a CHAR with a VARCHAR as text,
    // the CHAR's spaces cut and the VARCHAR's kept.
    CHECK(text(*db, "SELECT a = b FROM t WHERE id = 4") == V{"f"});
    CHECK(text(*db, "SELECT a = v FROM t WHERE id = 1") == V{"f"});
    CHECK(text(*db, "SELECT a = v FROM t WHERE id = 3") == V{"f"});
    CHECK(text(*db, "SELECT a = v FROM t WHERE id = 9") == V{"f"});
    (void)db->execute("UPDATE t SET v = 'ab' WHERE id = 1");
    CHECK(text(*db, "SELECT a = v FROM t WHERE id = 1") == V{"t"});
    CHECK(text(*db, "SELECT DISTINCT a FROM t ORDER BY a") == V{"ab   ", "abcde", "é    "});
    CHECK(state(*db, "UPDATE t SET a = $1 WHERE id = 1", {bytes("z  ")}) == "ok");
    CHECK(text(*db, "SELECT a FROM t WHERE id = 1") == V{"z    "});
    CHECK(text(*db, "SELECT id FROM t WHERE a = 'z'") == V{"1"});
    // NULL is not padded.
    CHECK(state(*db, "INSERT INTO t VALUES (12, NULL, NULL, NULL, NULL)") == "ok");
    CHECK(db->execute("SELECT a FROM t WHERE id = 12").rows == Grid{{kNull}});
    // A DEFAULT is stored as any value is.
    CHECK(state(*db, "CREATE TABLE d (x CHAR(4) DEFAULT 'a ', y INTEGER)") == "ok");
    CHECK(state(*db, "INSERT INTO d (y) VALUES (1)") == "ok");
    CHECK(text(*db, "SELECT x FROM d") == V{"a   "});
    CHECK(state(*db, "CREATE TABLE d2 (x CHAR(2) DEFAULT 'abc')") == "42601");
    // Not yet: a key, an index, a parameter compared, a cast, LIKE.
    CHECK(state(*db, "CREATE TABLE k (a CHAR(3) PRIMARY KEY)") == "0A000");
    CHECK(state(*db, "CREATE TABLE k (a CHAR(3) UNIQUE)") == "0A000");
    CHECK(state(*db, "CREATE INDEX t_a ON t (a)") == "0A000");
    CHECK(state(*db, "SELECT id FROM t WHERE a = $1", {bytes("ab")}) == "42601");
    CHECK(state(*db, "SELECT CAST('ab' AS CHAR(5))") == "42601");
    CHECK(state(*db, "SELECT id FROM t WHERE a LIKE 'a%'") != "ok");
    CHECK(state(*db, "CREATE TABLE z (a CHAR(0))") == "42601");
    CHECK(state(*db, "CREATE TABLE z (a CHAR(8 CHARACTERS))") == "42601");
  }
  // The type survives a reopen, and what was stored stays stripped.
  auto db = Database::open(dir.path());
  CHECK(text(*db, "SELECT a FROM t WHERE id = 2") == V{"abcde"});
  CHECK(text(*db, "SELECT id FROM t WHERE a = 'ab' ORDER BY id") == V{"3", "9"});
  CHECK(db->execute("SELECT a FROM t").types.at(0) == SqlType{SqlKind::Char, 5});
}

TEST_CASE("embedded API: a CHECK is tested on every INSERT and UPDATE, on the row as written", "[sql][api]") {
  sql_test::TempDir dir{"api_check"};
  auto state = [](Database& db, std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
    try {
      (void)db.execute(std::string{sql}, params);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ok";
  };
  {
    auto db = Database::open(dir.path());
    CHECK(state(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER CHECK (a > 0), b INTEGER DEFAULT 5, "
                     "CONSTRAINT lt CHECK (a IS NULL OR a < b))") == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, $3)", {i(1), i(1), i(2)}) == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, $3)", {i(2), i(0), i(2)}) == "23514");     // a > 0 false
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, $3)", {i(2), i(3), i(2)}) == "23514");     // lt false
    CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, $3)", {i(2), kNull, i(2)}) == "ok");       // NULL passes
    CHECK(state(*db, "INSERT INTO t (id, a) VALUES ($1, $2)", {i(3), i(4)}) == "ok");          // b is its default, 5
    CHECK(state(*db, "INSERT INTO t (id, a) VALUES ($1, $2)", {i(4), i(5)}) == "23514");       // 5 < 5 false
    CHECK(state(*db, "UPDATE t SET a = 0 WHERE id = 1") == "23514");
    CHECK(state(*db, "UPDATE t SET b = 1 WHERE id = 1") == "23514");  // a column the CHECK reads, not a column it names in SET
    CHECK(state(*db, "UPDATE t SET b = $1 WHERE id = $2", {i(9), i(1)}) == "ok");
    // A refused row is not written.
    CHECK(db->execute("SELECT id FROM t ORDER BY id").rows == Grid{{i(1)}, {i(2)}, {i(3)}});
    CHECK(db->execute("SELECT a, b FROM t WHERE id = 1").rows == Grid{{i(1), i(9)}});
    CHECK(state(*db, "ALTER TABLE t ADD COLUMN c INTEGER CHECK (c > 0)") == "42601");
  }
  auto db = Database::open(dir.path());  // the constraints are in the catalog
  CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, $3)", {i(7), i(0), i(2)}) == "23514");
  CHECK(state(*db, "INSERT INTO t VALUES ($1, $2, $3)", {i(7), i(1), i(2)}) == "ok");
}

TEST_CASE("embedded API: a cancel flag or a passed deadline fails a statement with 57014, and aborts its transaction",
          "[sql][api]") {
  sql_test::TempDir dir{"api_cancel"};
  auto db = Database::open(dir.path());
  (void)db->execute(kCreateTable);
  const auto& insert = db->prepare(kInsert);
  for (std::int64_t k = 0; k < 1000; ++k) (void)db->execute(insert, {i(k), bytes("x"), i(k)});
  constexpr auto kScan = "SELECT id FROM t WHERE id >= $1";

  auto failure = [](auto&& run) -> std::pair<std::string, std::string> {
    try {
      run();
    } catch (const ExecError& e) {
      return {std::string{sqlstate(e)}, e.what()};
    }
    return {"", ""};
  };

  CancelFlag flag;
  ExecutionOptions cancelled{.cancel = &flag};
  CHECK(db->execute(kScan, {i(0)}, cancelled).rows.size() == 1000);  // not set: runs to the end
  flag.cancel();
  CHECK(flag.cancelled());
  CHECK(failure([&] { (void)db->execute(kScan, {i(0)}, cancelled); }) ==
        std::pair<std::string, std::string>{"57014", "canceling statement due to user request"});
  flag.reset();
  CHECK(db->execute(kScan, {i(0)}, cancelled).rows.size() == 1000);

  ExecutionOptions late{.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1}};
  CHECK(failure([&] { (void)db->execute(kScan, {i(0)}, late); }) ==
        std::pair<std::string, std::string>{"57014", "canceling statement due to statement timeout"});
  ExecutionOptions roomy{.deadline = std::chrono::steady_clock::now() + std::chrono::hours{1}};
  CHECK(db->execute(kScan, {i(0)}, roomy).rows.size() == 1000);

  // In a transaction the cancelled statement aborts it, as any failed one.
  auto t = db->begin();
  CHECK(t.execute(kById, {i(1)}).rows.size() == 1);
  flag.cancel();
  CHECK(failure([&] { (void)t.execute(kScan, {i(0)}, cancelled); }).first == "57014");
  CHECK(t.is_aborted());
  CHECK_THROWS_AS(t.execute(kById, {i(1)}), TransactionAborted);
  CHECK(t.commit() == CommitOutcome::RolledBack);
}

TEST_CASE("embedded API: CREATE and DROP PROCEDURE are DDL, kept in the catalog with PostgreSQL's SQLSTATEs",
          "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure"};
  auto state = [](Database& db, std::string_view sql) -> std::string {
    try {
      (void)db.execute(sql);
    } catch (const ExecError& e) {
      return std::string{sqlstate(e)};
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return std::string{"ran"};
  };
  auto neword = std::string{
      "CREATE PROCEDURE neword(no_w_id INTEGER, INOUT no_c_last VARCHAR(16), OUT total NUMERIC(12, 2)) "
      "LANGUAGE plpgsql AS $$ BEGIN SELECT c_last INTO no_c_last FROM customer WHERE c_w_id = no_w_id; END $$"};
  {
    auto db = Database::open(dir.path());
    CHECK(db->prepare(neword).command() == Command::CreateProcedure);
    CHECK(db->prepare("DROP PROCEDURE neword").command() == Command::DropProcedure);
    CHECK(state(*db, neword) == "ran");  // the table it names need not exist
    CHECK(db->catalog()->procedure({kPublicSchema, "neword"})->definition == neword);
    CHECK(state(*db, neword) == "42723");
    CHECK(state(*db, "CREATE PROCEDURE p(a INT, a INT) LANGUAGE plpgsql AS $$ BEGIN END $$") == "42P13");
    CHECK(state(*db, "CREATE PROCEDURE p(a NUMERIC) LANGUAGE plpgsql AS $$ BEGIN END $$") == "0A000");  // no precision: a column's type only
    CHECK(state(*db, "CREATE PROCEDURE p(a INT) LANGUAGE plpgsql AS $$ BEGIN IF END $$") == "42601");
    CHECK(state(*db, "CREATE OR REPLACE PROCEDURE neword(no_w_id BIGINT, INOUT no_c_last VARCHAR(16), OUT total "
                     "NUMERIC(12, 2)) LANGUAGE plpgsql AS $$ BEGIN END $$") == "0A000");  // an overload
    CHECK(state(*db, "CALL neword(1, 'x', NULL)") == "0A000");
    CHECK(state(*db, "CREATE TABLE neword (id INTEGER PRIMARY KEY)") == "ran");  // its own namespace
    {
      auto txn = db->begin();
      (void)txn.execute("DROP PROCEDURE neword");  // DDL of the transaction: rolled back with it
      CHECK(txn.procedure("neword") == nullptr);
      txn.rollback();
      CHECK(db->catalog()->procedure({kPublicSchema, "neword"}) != nullptr);
    }
  }
  {
    auto db = Database::open(dir.path());
    REQUIRE(db->catalog()->procedure({kPublicSchema, "neword"}) != nullptr);
    CHECK(state(*db, "DROP PROCEDURE neword(TEXT)") == "42883");
    CHECK(state(*db, "DROP PROCEDURE IF EXISTS neword(TEXT)") == "ran");
    CHECK(state(*db, "DROP PROCEDURE neword(INTEGER, VARCHAR, NUMERIC)") == "ran");  // every argument's type
    CHECK(db->catalog()->procedure({kPublicSchema, "neword"}) == nullptr);
    CHECK(state(*db, "DROP PROCEDURE neword") == "42883");
    CHECK(state(*db, "DROP PROCEDURE IF EXISTS neword") == "ran");
    CHECK(db->catalog()->table({kPublicSchema, "neword"}) != nullptr);
  }
}

TEST_CASE("embedded API: a procedure runs its statements in one transaction, each seeing the ones before",
          "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_run"};
  auto db = Database::open(dir.path());
  auto run = [&](std::string_view sql) { return db->execute(sql).rows; };
  (void)run("CREATE TABLE stock (id INTEGER PRIMARY KEY, qty INTEGER, name VARCHAR(10))");
  (void)run("CREATE TABLE orders (id INTEGER PRIMARY KEY, item INTEGER, amount INTEGER)");
  (void)run("INSERT INTO stock VALUES (1, 10, 'a')");
  (void)run("INSERT INTO stock VALUES (2, 20, 'b')");
  (void)run("INSERT INTO stock VALUES (3, 30, 'c')");
  (void)run(R"(CREATE PROCEDURE take(o_id INTEGER, n INTEGER, INOUT total INTEGER, OUT last_name VARCHAR(10))
LANGUAGE plpgsql AS $$
DECLARE
  q INTEGER;
  k INTEGER := 0;
BEGIN
  total := 0;
  FOR i IN 1..n LOOP
    SELECT qty, name INTO q, last_name FROM stock WHERE id = i;
    IF q > 15 THEN
      UPDATE stock SET qty = qty - 1 WHERE id = i;
      SELECT qty INTO q FROM stock WHERE id = i;   -- sees the update
      UPDATE stock SET qty = qty - 1 WHERE id = i; -- a second write of the row
    ELSE
      k := k + 1;
    END IF;
    total := total + q;
  END LOOP;
  INSERT INTO orders VALUES (o_id, n, total);
  IF NOT FOUND THEN
    total := -1;
  END IF;
END $$)");
  auto out = db->call("take", {i(7), i(3), Value{}, Value{}});
  CHECK(out.columns == std::vector<std::string>{"total", "last_name"});
  // 10 + (20 - 1) + (30 - 1); the last row read is 'c'.
  CHECK(out.rows == Grid{{i(58), Value{sql_test::bytes("c")}}});
  CHECK(run("SELECT qty FROM stock ORDER BY id") == Grid{{i(10)}, {i(18)}, {i(28)}});
  CHECK(run("SELECT item, amount FROM orders WHERE id = 7") == Grid{{i(3), i(58)}});

  // A failed statement aborts the procedure, and its transaction keeps nothing.
  CHECK_THROWS_AS(db->call("take", {i(7), i(1), Value{}, Value{}}), ExecError);  // 7 is in orders: 23505
  CHECK(run("SELECT qty FROM stock ORDER BY id") == Grid{{i(10)}, {i(18)}, {i(28)}});
  // Inside a transaction, the procedure's writes are the transaction's.
  {
    auto t = db->begin();
    auto r = t.call("take", {i(8), i(2), Value{}, Value{}});
    CHECK(r.rows == Grid{{i(27), Value{sql_test::bytes("b")}}});  // 10 + (18 - 1)
    CHECK(t.execute("SELECT qty FROM stock WHERE id = 2").rows == Grid{{i(16)}});
    t.rollback();
  }
  CHECK(run("SELECT qty FROM stock WHERE id = 2") == Grid{{i(18)}});
}

TEST_CASE("embedded API: a procedure's errors are PostgreSQL's, found at CREATE or at the call", "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_errors"};
  auto db = Database::open(dir.path());
  auto state = [&](auto&& f) -> std::string {
    try {
      f();
    } catch (const std::exception& e) {
      UNSCOPED_INFO(e.what());
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  auto ddl = [&](std::string_view sql) { return state([&] { (void)db->execute(sql); }); };
  auto call = [&](std::string_view name, std::vector<Value> args) { return state([&] { (void)db->call(name, args); }); };
  (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER, s TEXT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10, 'x')");
  (void)db->execute("INSERT INTO t VALUES (2, 10, 'y')");
  auto proc = [](std::string_view name, std::string_view args, std::string_view body) {
    return "CREATE PROCEDURE " + std::string{name} + "(" + std::string{args} + ") LANGUAGE plpgsql AS $$ " +
           std::string{body} + " $$";
  };
  // STRICT: no row is P0002, a second row P0003; without STRICT the first row, or NULLs.
  CHECK(ddl(proc("one", "k INTEGER, INOUT r INTEGER", "BEGIN SELECT v INTO STRICT r FROM t WHERE id = k; END")) == "ran");
  CHECK(db->call("one", {i(1), Value{}}).rows == Grid{{i(10)}});
  CHECK(call("one", {i(9), Value{}}) == "P0002");
  CHECK(ddl(proc("many", "INOUT r INTEGER", "BEGIN SELECT v INTO STRICT r FROM t WHERE v = 10; END")) == "ran");
  CHECK(call("many", {Value{}}) == "P0003");
  CHECK(ddl(proc("loose", "k INTEGER, INOUT r INTEGER, INOUT f BOOLEAN",
                 "BEGIN SELECT v INTO r FROM t WHERE id = k; f := FOUND; END")) == "ran");
  CHECK(db->call("loose", {i(9), i(5), Value{}}).rows == Grid{{Value{}, Value{false}}});
  CHECK(db->call("loose", {i(2), Value{}, Value{}}).rows == Grid{{i(10), Value{true}}});
  // A name that is a variable and a column of the statement's table: 42702, at the call (bodies bind there).
  CHECK(ddl(proc("clash", "v INTEGER", "BEGIN UPDATE t SET s = 'z' WHERE id = v; END")) == "ran");
  CHECK(call("clash", {i(1)}) == "42702");
  // A column of the INSERT's target is no conflict: VALUES cannot name it.
  CHECK(ddl(proc("ins", "id INTEGER, v INTEGER", "BEGIN INSERT INTO t VALUES (id, v, 'n'); END")) == "ran");
  CHECK(call("ins", {i(3), i(30)}) == "ran");
  CHECK(db->execute("SELECT v FROM t WHERE id = 3").rows == Grid{{i(30)}});
  // A variable of another type than the column it meets.
  CHECK(ddl(proc("typed", "k TEXT", "BEGIN UPDATE t SET v = 1 WHERE id = k; END")) == "ran");
  CHECK(call("typed", {Value{sql_test::bytes("1")}}) == "42601");  // a form not built yet, as the binder refuses it
  // Arithmetic past the variable's type, and a NULL FOR bound.
  CHECK(ddl(proc("big", "INOUT x SMALLINT", "BEGIN x := x * 1000; END")) == "ran");
  CHECK(call("big", {i(100)}) == "22003");
  CHECK(ddl(proc("nullfor", "n INTEGER", "BEGIN FOR i IN 1..n LOOP NULL; END LOOP; END")) == "ran");
  CHECK(call("nullfor", {Value{}}) == "22004");
  // A condition that is not BOOLEAN, a SELECT with nowhere for its rows, EXIT outside a loop.
  CHECK(ddl(proc("cond", "n INTEGER", "BEGIN IF n THEN NULL; END IF; END")) == "ran");
  CHECK(call("cond", {i(1)}) == "42804");
  CHECK(ddl(proc("nowhere", "n INTEGER", "BEGIN SELECT v FROM t; END")) == "ran");
  CHECK(call("nowhere", {i(1)}) == "42601");
  // Arguments: a procedure that does not exist, the wrong count or type, a value outside the type.
  CHECK(call("nope", {}) == "42883");
  CHECK(call("one", {i(1)}) == "08P01");
  CHECK(call("one", {Value{sql_test::bytes("1")}, Value{}}) == "08P01");
  CHECK(call("big", {i(40000)}) == "22003");
  // WHILE and LOOP end on their own, and on EXIT and CONTINUE.
  CHECK(ddl(proc("loops", "INOUT n INTEGER, INOUT c INTEGER",
                 "BEGIN c := 0; WHILE n > 0 LOOP n := n - 1; CONTINUE WHEN n = 2; c := c + 1; END LOOP; "
                 "LOOP c := c + 10; EXIT WHEN c > 30; END LOOP; END")) == "ran");
  CHECK(db->call("loops", {i(5), Value{}}).rows == Grid{{i(0), i(34)}});
  // A DDL since the last call: the procedure is compiled again.
  (void)db->execute("ALTER TABLE t ADD COLUMN w INTEGER");
  CHECK(db->call("one", {i(1), Value{}}).rows == Grid{{i(10)}});
  (void)db->execute("DROP TABLE t");
  CHECK(call("one", {i(1), Value{}}) == "42P01");
}

// docs/catalog_design.md, *Stages of schemas*, before stage 1: a variable's
// type is read at the call (PostgreSQL reads it at CREATE PROCEDURE), with
// PostgreSQL 17's SQLSTATE and message.
TEST_CASE("embedded API: a procedure variable's qualified type is refused at the call as PostgreSQL refuses it",
          "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_qualified_type"};
  auto db = Database::open(dir.path());
  auto declare = [&](std::string_view name, std::string_view decl, std::string_view body) {
    (void)db->execute("CREATE PROCEDURE " + std::string{name} + "(INOUT r BIGINT) LANGUAGE plpgsql AS $$ DECLARE " +
                      std::string{decl} + "; BEGIN " + std::string{body} + " END $$");
  };
  auto refusal = [&](std::string_view name) -> std::pair<std::string, std::string> {
    try {
      (void)db->call(name, {Value{}});
    } catch (const std::exception& e) {
      return {std::string{sqlstate(e)}, e.what()};
    }
    return {"ran", ""};
  };
  declare("in_foo", "x foo.int4", "x := 1;");
  CHECK(refusal("in_foo") == std::pair<std::string, std::string>{"3F000", "schema \"foo\" does not exist"});
  declare("in_public", "x public.int4", "x := 1;");
  CHECK(refusal("in_public") == std::pair<std::string, std::string>{"42704", "type \"public.int4\" does not exist"});
  // pg_catalog holds no type under the grammar's SQL spellings, quoted or not.
  declare("alias", "x pg_catalog.integer", "x := 1;");
  CHECK(refusal("alias") == std::pair<std::string, std::string>{"42704", "type \"pg_catalog.integer\" does not exist"});
  declare("quoted", "x pg_catalog.\"boolean\"", "x := true;");
  CHECK(refusal("quoted").first == "42704");
  declare("bare", "x \"integer\"", "x := 1;");
  CHECK(refusal("bare").first != "ran");
  // pg_catalog's own names stay, as does a FOR loop's own integer variable.
  declare("sum", "x pg_catalog.int8 := 0", "FOR i IN 1..3 LOOP x := x + i; END LOOP; r := x;");
  CHECK(db->call("sum", {Value{}}).rows == Grid{{i(6)}});
}

TEST_CASE("embedded API: a CALL that conflicts is run again from its arguments, as an autocommit statement is",
          "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_retry"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE c (id INTEGER PRIMARY KEY, n INTEGER)");
  (void)db->execute("INSERT INTO c VALUES (1, 0)");
  (void)db->execute(R"(CREATE PROCEDURE bump(k INTEGER, INOUT seen INTEGER) LANGUAGE plpgsql AS $$
BEGIN
  SELECT n INTO seen FROM c WHERE id = k;
  UPDATE c SET n = n + 1 WHERE id = k;
END $$)");
  // Another writer commits the row after the procedure's snapshot, `races` times.
  auto races = 0;
  auto racing = 0;
  db->after_txn_pin_for_testing([&] {
    if (racing >= races) return;
    ++racing;
    (void)db->execute("UPDATE c SET n = n + 100 WHERE id = 1");
  });
  races = 1;
  // The first run conflicts; the second reads the other writer's row.
  CHECK(db->call("bump", {i(1), Value{}}).rows == Grid{{i(100)}});
  CHECK(db->execute("SELECT n FROM c").rows == Grid{{i(101)}});
  // Conflicting on every attempt (the hook runs at each of its two statements): 40001, with
  // nothing of the procedure written.
  racing = 0;
  races = 6;
  try {
    (void)db->call("bump", {i(1), Value{}});
    FAIL("no conflict");
  } catch (const SerializationFailure& e) {
    CHECK(sqlstate(e) == "40001");
  }
  CHECK(racing == 6);
  CHECK(db->execute("SELECT n FROM c").rows == Grid{{i(701)}});
  // Inside a transaction the conflict is the client's: no rerun.
  racing = 0;
  races = 1;
  auto t = db->begin();
  CHECK(t.call("bump", {i(1), Value{}}).rows == Grid{{i(701)}});
  CHECK_THROWS_AS(t.commit(), SerializationFailure);
  CHECK(racing == 1);
  db->after_txn_pin_for_testing({});
}

TEST_CASE("embedded API: transaction_counts counts each transaction once, an autocommit statement as one",
          "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_transaction_counts"};
  auto db = Database::open(dir.path());
  auto before = db->transaction_counts();
  CHECK(before.committed == 0);
  CHECK(before.rolled_back == 0);
  (void)db->execute("CREATE TABLE n (id INTEGER PRIMARY KEY)");      // committed
  (void)db->execute("INSERT INTO n VALUES (1)");                     // committed
  CHECK_THROWS(db->execute("INSERT INTO n VALUES (1)"));             // rolled back: 23505
  {
    auto t = db->begin();
    (void)t.execute("INSERT INTO n VALUES (2)");
    (void)t.commit();                                                // committed
  }
  {
    auto t = db->begin();
    (void)t.execute("INSERT INTO n VALUES (3)");
    t.rollback();                                                    // rolled back
  }
  { auto t = db->begin(); }                                          // destroyed open: rolled back
  auto after = db->transaction_counts();
  CHECK(after.committed == 3);
  CHECK(after.rolled_back == 3);
}

TEST_CASE("embedded API: FOR targets IN query runs the body per row of the query as it ran", "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_for_query"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  (void)db->execute("CREATE TABLE customer (c_id INTEGER PRIMARY KEY, c_last VARCHAR(16), c_first VARCHAR(16), c_balance INTEGER)");
  for (auto [id, last, first] : {std::tuple{1, "BAR", "dan"}, {2, "BAR", "ann"}, {3, "OUGHT", "zed"}, {4, "BAR", "cat"},
                                 {5, "BAR", "bob"}, {6, "BAR", "eve"}}) {
    (void)db->execute("INSERT INTO customer VALUES ($1, $2, $3, 0)", {i(id), text(last), text(first)});
  }
  // TPROC-C's customer by last name: the one at position ceil(n / 2) by c_first.
  (void)db->execute(R"(CREATE PROCEDURE middle(name VARCHAR(16), INOUT pick INTEGER, INOUT seen INTEGER, INOUT f BOOLEAN)
LANGUAGE plpgsql AS $$
DECLARE
  n INTEGER;
  k INTEGER := 0;
  id INTEGER;
  first VARCHAR(16);
BEGIN
  SELECT count(c_id) INTO n FROM customer WHERE c_last = name;
  seen := 0;
  FOR id, first IN SELECT c_id, c_first FROM customer WHERE c_last = name ORDER BY c_first LOOP
    k := k + 1;
    UPDATE customer SET c_balance = c_balance + 1 WHERE c_id = id;  -- the loop's rows stay as they ran
    seen := seen + 1;
    IF k = (n + 1) / 2 THEN
      pick := id;
      EXIT;
    END IF;
  END LOOP;
  f := FOUND;
END $$)");
  // BAR by c_first: ann (2), bob (5), cat (4), dan (1), eve (6); n = 5, position 3: cat.
  CHECK(db->call("middle", {text("BAR"), Value{}, Value{}, Value{}}).rows == Grid{{i(4), i(3), Value{true}}});
  CHECK(db->execute("SELECT c_id FROM customer WHERE c_balance = 1 ORDER BY c_id").rows == Grid{{i(2)}, {i(4)}, {i(5)}});
  CHECK(db->call("middle", {text("OUGHT"), Value{}, Value{}, Value{}}).rows == Grid{{i(3), i(1), Value{true}}});
  // No row: the body never runs, and FOUND is false.
  CHECK(db->call("middle", {text("NONE"), Value{}, Value{}, Value{}}).rows == Grid{{Value{}, i(0), Value{false}}});
  // A FOR over a statement that returns no rows is refused at the call.
  (void)db->execute("CREATE PROCEDURE bad() LANGUAGE plpgsql AS $$ DECLARE x INTEGER; BEGIN "
                    "FOR x IN UPDATE customer SET c_balance = 0 WHERE c_id = 1 LOOP NULL; END LOOP; END $$");
  try {
    (void)db->call("bad", {});
    FAIL("ran");
  } catch (const ast::SqlError& e) {
    CHECK(std::string{e.sqlstate()} == "42601");
  }
}

TEST_CASE("embedded API: concatenation and SUBSTRING of columns build text per row, as PostgreSQL's do", "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_string_columns"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  (void)db->execute("CREATE TABLE s (id INTEGER PRIMARY KEY, name VARCHAR(10), amount DECIMAL(8,2), big BIGINT, ok BOOLEAN)");
  (void)db->execute("INSERT INTO s VALUES (1, 'héllo', 12.5, -7, TRUE)");
  (void)db->execute("INSERT INTO s VALUES (2, NULL, -0.05, 9000000000, FALSE)");
  auto rows = db->execute("SELECT id || ' ' || name, name || amount, substring(name FROM 2 FOR 3), substr(name, 4), "
                          "big || '|' || amount FROM s ORDER BY id");
  CHECK(rows.columns == std::vector<std::string>{"?column?", "?column?", "substring", "substr", "?column?"});
  CHECK(rows.rows == Grid{{text("1 héllo"), text("héllo12.50"), text("éll"), text("lo"), text("-7|12.50")},
                          {Value{}, Value{}, Value{}, Value{}, text("9000000000|-0.05")}});
  // A start before the first character counts toward the length; past the end, nothing.
  CHECK(db->execute("SELECT substring(name FROM -1 FOR 4), substring(name FROM 9) FROM s WHERE id = 1").rows ==
        Grid{{text("hé"), text("")}});
  CHECK(db->execute("SELECT substring(name FROM 1 FOR 0) FROM s WHERE id = 1").rows == Grid{{text("")}});
  CHECK(state("SELECT substring(name FROM 1 FOR -1) FROM s WHERE id = 1") == "22011");
  CHECK(state("SELECT id || big FROM s") == "42883");          // no text operand
  CHECK(state("SELECT name || ok FROM s") == "42601");         // a boolean's text: not built
  CHECK(state("SELECT overlay(name PLACING 'x' FROM 1) FROM s") == "42601");  // OVERLAY of a column: #671
  CHECK(state("SELECT substring(name FROM big) FROM s") == "42883");  // a bigint start
}

TEST_CASE("embedded API: UPPER, LOWER, TRIM, POSITION and the lengths of columns are PostgreSQL's, per row", "[sql][api]") {
  sql_test::TempDir dir{"api_text_functions"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  (void)db->execute("CREATE TABLE s (id INTEGER PRIMARY KEY, v VARCHAR(10), w TEXT, n INTEGER)");
  (void)db->execute("INSERT INTO s VALUES (1, ' héLlo ', 'l', 1)");
  (void)db->execute("INSERT INTO s VALUES (2, NULL, 'x', 2)");
  auto rows = db->execute("SELECT upper(v), lower(v), char_length(v), character_length(v), octet_length(v) FROM s ORDER BY id");
  CHECK(rows.columns == std::vector<std::string>{"upper", "lower", "char_length", "character_length", "octet_length"});
  CHECK(rows.rows == Grid{{text(" HéLLO "), text(" héllo "), i(7), i(7), i(8)}, {Value{}, Value{}, Value{}, Value{}, Value{}}});
  CHECK(db->execute("SELECT trim(v), trim(leading from v), trim(trailing from v), trim(both ' o' from v), position(w in v), "
                    "position('' in v), position('z' in v) FROM s WHERE id = 1")
            .rows == Grid{{text("héLlo"), text("héLlo "), text(" héLlo"), text("héLl"), i(5), i(1), i(0)}});
  CHECK(db->execute("SELECT position(w in v), trim(w from v), upper(v) || '!' FROM s WHERE id = 2").rows ==
        Grid{{Value{}, Value{}, Value{}}});
  CHECK(db->execute("SELECT char_length(v) + n FROM s WHERE id = 1").rows == Grid{{i(8)}});
  // A $n only inside a call is the statement's own: a constant beside it takes a slot after it.
  CHECK(db->execute("SELECT position('b' in $1), $1 || 'q', upper($1) FROM s WHERE id = 1", {sql_test::bytes("xbz")}).rows ==
        Grid{{i(2), text("xbzq"), text("XBZ")}});
  CHECK(state("SELECT upper(n) FROM s") == "42883");
  CHECK(state("SELECT position(n in v) FROM s") == "42883");
}

TEST_CASE("embedded API: ILIKE folds ASCII letters, and LIKE's ESCAPE may be a parameter", "[sql][api]") {
  sql_test::TempDir dir{"api_ilike"};
  auto db = Database::open(dir.path());
  auto state = [&](std::string_view sql, std::vector<Value> params = {}) -> std::string {
    try {
      (void)db->execute(sql, params);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  auto ids = [&](std::string_view sql, std::vector<Value> params) {
    std::vector<std::int64_t> out;
    for (const auto& r : db->execute(sql, params).rows) out.push_back(std::get<std::int64_t>(r[0]));
    return out;
  };
  using V = std::vector<std::int64_t>;
  (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR(20))");
  (void)db->execute("INSERT INTO t VALUES (1, 'apple'), (2, 'Apple'), (3, 'banana'), (4, 'a_b'), (5, NULL), (6, 'a%b')");
  CHECK(ids("SELECT id FROM t WHERE v ILIKE $1 ORDER BY id", {sql_test::bytes("APP%")}) == V{1, 2});
  CHECK(ids("SELECT id FROM t WHERE v NOT ILIKE $1 ORDER BY id", {sql_test::bytes("a%")}) == V{3});  // NULL is neither
  CHECK(ids("SELECT id FROM t WHERE v LIKE $1 ESCAPE $2 ORDER BY id", {sql_test::bytes("a#_b"), sql_test::bytes("#")}) == V{4});
  CHECK(ids("SELECT id FROM t WHERE v LIKE $1 ESCAPE $2 ORDER BY id", {sql_test::bytes("a_b"), sql_test::bytes("")}) == V{4, 6});
  CHECK(ids("SELECT id FROM t WHERE v LIKE $1 ESCAPE $2 ORDER BY id", {sql_test::bytes("a_b"), Value{}}).empty());
  CHECK(state("SELECT id FROM t WHERE v LIKE $1 ESCAPE $2", {sql_test::bytes("a_b"), sql_test::bytes("ab")}) == "22025");
  CHECK(state("SELECT id FROM t WHERE id ILIKE 'a'") == "42883");
}

TEST_CASE("embedded API: RETURNING gives a row per row written, as inserted, updated or deleted", "[sql][api]") {
  sql_test::TempDir dir{"api_returning"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  (void)db->execute("CREATE TABLE r (id INTEGER PRIMARY KEY, v INTEGER, s TEXT DEFAULT 'd')");
  auto ins = db->execute("INSERT INTO r VALUES ($1, $2), (2, NULL) RETURNING *", {i(1), i(10)});
  CHECK(ins.columns == std::vector<std::string>{"id", "v", "s"});
  CHECK(ins.affected == 2);
  CHECK(ins.rows == Grid{{i(1), i(10), text("d")}, {i(2), Value{}, text("d")}});
  // An UPDATE's values are the row as updated, a moved key included.
  auto upd = db->execute("UPDATE r SET v = v + 1, id = id + 10 WHERE id = 1 RETURNING id AS k, v * 2, upper(s)");
  CHECK(upd.columns == std::vector<std::string>{"k", "?column?", "upper"});
  CHECK(upd.rows == Grid{{i(11), i(22), text("D")}});
  // A DELETE's are the row as it was; no row written is no row returned.
  CHECK(db->execute("DELETE FROM r WHERE id = 2 RETURNING s, v").rows == Grid{{text("d"), Value{}}});
  auto none = db->execute("DELETE FROM r WHERE id = 99 RETURNING id");
  CHECK(none.rows.empty());
  CHECK(none.affected == 0);
  CHECK(db->execute("SELECT id FROM r").rows == Grid{{i(11)}});
  CHECK(state("INSERT INTO r VALUES (5, 1) RETURNING count(*)") == "42803");
  CHECK(state("INSERT INTO r VALUES (5, 1) RETURNING nope") == "42703");
  CHECK(db->execute("SELECT id FROM r").rows == Grid{{i(11)}});  // the refused statements wrote nothing
  // In a transaction the rows come back before the commit.
  auto txn = db->begin();
  CHECK(txn.execute("INSERT INTO r VALUES (6, 6) RETURNING v").rows == Grid{{i(6)}});
  txn.rollback();
  CHECK(db->execute("SELECT id FROM r").rows == Grid{{i(11)}});
  // A procedure's statement with RETURNING is refused when the procedure is first planned.
  (void)db->execute("CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ BEGIN DELETE FROM r WHERE id = 11 RETURNING id; END $$");
  CHECK(state("CALL p()") == "0A000");
  CHECK(db->execute("SELECT id FROM r").rows == Grid{{i(11)}});
}

TEST_CASE("embedded API: a procedure builds text from its variables, as TPROC-C's Payment does", "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_strings"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  (void)db->execute("CREATE TABLE customer (c_id INTEGER PRIMARY KEY, c_credit CHAR(2), c_data VARCHAR(500))");
  (void)db->execute("CREATE TABLE history (h_id INTEGER PRIMARY KEY, h_data VARCHAR(24))");
  (void)db->execute("INSERT INTO customer VALUES (1, 'BC', 'old data')");
  (void)db->execute("INSERT INTO customer VALUES (2, 'GC', 'kept')");
  (void)db->execute(R"(CREATE PROCEDURE pay(k INTEGER, d INTEGER, w INTEGER, amount NUMERIC(6,2), w_name VARCHAR(10),
                         d_name VARCHAR(10), INOUT out_data VARCHAR(500))
LANGUAGE plpgsql AS $$
DECLARE
  credit CHAR(2);
  data VARCHAR(500);
  h VARCHAR(24);
BEGIN
  h := w_name || '    ' || d_name;
  INSERT INTO history VALUES (k, h);
  SELECT c_credit, c_data INTO credit, data FROM customer WHERE c_id = k;
  IF credit = 'BC' THEN
    data := substr(k || ' ' || d || ' ' || w || ' ' || amount || ' | ' || data, 1, 20);
    UPDATE customer SET c_data = data WHERE c_id = k;
  END IF;
  out_data := data;
END $$)");
  CHECK(db->call("pay", {i(1), i(3), i(7), Value{std::int64_t{1250}}, text("wh"), text("dist"), Value{}}).rows ==
        Grid{{text("1 3 7 12.50 | old da")}});  // cut at 20 characters
  CHECK(db->execute("SELECT c_data FROM customer WHERE c_id = 1").rows == Grid{{text("1 3 7 12.50 | old da")}});
  CHECK(db->execute("SELECT h_data FROM history WHERE h_id = 1").rows == Grid{{text("wh    dist")}});
  CHECK(db->call("pay", {i(2), i(3), i(7), Value{std::int64_t{100}}, text("wh"), text("dist"), Value{}}).rows ==
        Grid{{text("kept")}});
}

TEST_CASE("embedded API: a procedure's assignment brings a number to the variable's scale, rounding half away from zero",
          "[sql][api][procedure]") {
  sql_test::TempDir dir{"api_procedure_rescale"};
  auto db = Database::open(dir.path());
  (void)db->execute(R"(CREATE PROCEDURE r(a NUMERIC(6, 3), n INTEGER, INOUT down NUMERIC(6, 2), INOUT up NUMERIC(8, 2),
                         INOUT pick INTEGER) LANGUAGE plpgsql AS $$
BEGIN
  down := a * 1;
  up := n;
  pick := random(3, 3);
END $$)");
  auto call = [&](std::int64_t thousandths, std::int64_t n) {
    return db->call("r", {Value{thousandths}, Value{n}, Value{}, Value{}, Value{}}).rows.at(0);
  };
  CHECK(call(1005, 7) == std::vector<Value>{Value{std::int64_t{101}}, Value{std::int64_t{700}}, Value{std::int64_t{3}}});
  CHECK(call(-1005, -7).at(0) == Value{std::int64_t{-101}});  // -1.005 is -1.01
  CHECK(call(1004, 0).at(0) == Value{std::int64_t{100}});
  try {
    (void)call(1000, 1'000'000);  // 1000000.00 is past NUMERIC(8, 2)
    FAIL("ran");
  } catch (const std::exception& e) {
    CHECK(std::string{sqlstate(e)} == "22003");
  }
  (void)db->execute("CREATE PROCEDURE bad(INOUT x INTEGER) LANGUAGE plpgsql AS $$ BEGIN x := random(5, 1); END $$");
  try {
    (void)db->call("bad", {Value{}});
    FAIL("ran");
  } catch (const std::exception& e) {
    CHECK(std::string{sqlstate(e)} == "22023");
  }
}

TEST_CASE("embedded API: CREATE TABLE AS and SELECT … INTO make a table of a query's columns and rows",
          "[sql][api][ctas]") {
  sql_test::TempDir dir{"api_ctas"};
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto state = [](Database& db, std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
    try {
      (void)db.execute(sql, params);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE src (a INTEGER PRIMARY KEY, b TEXT, c VARCHAR(5), d CHAR(3), e NUMERIC(5,2), f VARCHAR)");
    (void)db->execute("INSERT INTO src VALUES (1, 'x', 'ab', 'a', 1.5, 'q'), (2, 'y', NULL, 'bb', NULL, NULL)");
    CHECK(db->prepare("CREATE TABLE c AS SELECT 1").command() == Command::CreateTableAs);
    CHECK(db->prepare("SELECT 1 INTO c").command() == Command::SelectInto);
    CHECK(db->prepare("SELECT 1 INTO c").is_ddl());
    CHECK(db->prepare("SELECT 1 INTO c").columns().empty());  // it returns no rows

    // The rows written are the count; the columns keep the query's types.
    auto r = db->execute("CREATE TABLE c1 AS SELECT * FROM src");
    CHECK(r.affected == 2);
    CHECK(r.notices.empty());
    const auto* t = db->catalog()->table({kPublicSchema, "c1"});
    REQUIRE(t != nullptr);
    REQUIRE(t->columns.size() == 7);  // and the hidden row id
    CHECK(t->columns[0].sql == SqlType{SqlKind::Integer});
    CHECK(t->columns[2].sql == SqlType{SqlKind::Varchar, 5});
    CHECK(t->columns[3].sql == SqlType{SqlKind::Char, 3});
    CHECK(t->columns[4].sql == SqlType{SqlKind::Decimal, 0, 5, 2});
    CHECK(t->columns[5].sql == SqlType{SqlKind::Text});  // VARCHAR of no length, as a column declared so
    CHECK(t->columns[6].hidden);
    // CASE and COALESCE of one VARCHAR(n) or CHAR(n) keep its length; now()
    // is named now.
    (void)db->execute("CREATE TABLE c9 AS SELECT COALESCE(c, c) AS x, CASE WHEN a = 1 THEN d ELSE d END AS y, "
                      "COALESCE(c, 'z') AS z, now() FROM src");
    const auto* t9 = db->catalog()->table({kPublicSchema, "c9"});
    REQUIRE(t9 != nullptr);
    CHECK(t9->columns[0].sql == SqlType{SqlKind::Varchar, 5});
    CHECK(t9->columns[1].sql == SqlType{SqlKind::Char, 3});
    CHECK(t9->columns[2].sql == SqlType{SqlKind::Text});
    CHECK(t9->columns[3].name == "now");
    CHECK(db->execute("SELECT a, b, d FROM c1 ORDER BY a").rows ==
          Grid{{i(1), text("x"), text("a  ")}, {i(2), text("y"), text("bb ")}});
    CHECK(state(*db, "INSERT INTO c1 (c) VALUES ('toolong')") == "22001");  // VARCHAR(5) kept
    // Row ids continue after the rows written.
    (void)db->execute("INSERT INTO c1 (a) VALUES (3)");
    CHECK(db->execute("SELECT COUNT(*) FROM c1").rows == Grid{{i(3)}});

    CHECK(db->execute("SELECT a AS z INTO c2 FROM src WHERE a = 2").affected == 1);
    CHECK(db->execute("SELECT z FROM c2").rows == Grid{{i(2)}});
    // Fewer names than columns rename the first ones.
    (void)db->execute("CREATE TABLE c3 (x) AS SELECT a, b FROM src WHERE a = 1");
    CHECK(db->execute("SELECT * FROM c3").columns == std::vector<std::string>{"x", "b"});
    // A set operation, a view, an aggregate and a parameter as the query.
    CHECK(db->execute("CREATE TABLE c4 AS SELECT a FROM src UNION SELECT 7").affected == 3);
    (void)db->execute("CREATE VIEW v AS SELECT b FROM src");
    CHECK(db->execute("CREATE TABLE c5 AS SELECT COUNT(*) AS n FROM v").affected == 1);
    CHECK(db->execute("SELECT n FROM c5").rows == Grid{{i(2)}});
    const auto& with_param = db->prepare("CREATE TABLE c6 AS SELECT a FROM src WHERE a = $1");
    REQUIRE(with_param.params().size() == 1);
    CHECK(db->execute(with_param, {i(2)}).affected == 1);
    CHECK(db->execute("SELECT a FROM c6").rows == Grid{{i(2)}});

    // WITH NO DATA: the table and no rows; the query is not run.
    r = db->execute("CREATE TABLE c7 AS SELECT a FROM src WITH NO DATA");
    CHECK(r.affected == -1);
    CHECK(db->execute("SELECT a FROM c7").rows.empty());

    // A relation of the name is found first, before the query's own errors
    // at run (1/0, folded when PostgreSQL plans it), and IF NOT EXISTS skips.
    CHECK(state(*db, "CREATE TABLE c1 AS SELECT 1") == "42P07");
    CHECK(state(*db, "SELECT 1/0 INTO c1") == "42P07");
    CHECK(state(*db, "SELECT 1/0 INTO c8") == "22012");
    CHECK(state(*db, "CREATE TABLE v AS SELECT 1") == "42P07");
    r = db->execute("CREATE TABLE IF NOT EXISTS c1 AS SELECT 1/0");
    CHECK(r.affected == -1);
    REQUIRE(r.notices.size() == 1);
    CHECK(r.notices[0].sqlstate == "42P07");
    CHECK(r.notices[0].message == "relation \"c1\" already exists, skipping");
    // A literal's input error is the analysis's: before the name check.
    CHECK(state(*db, "CREATE TABLE c1 AS SELECT 'abc'::int") == "22P02");
    CHECK(state(*db, "CREATE TABLE IF NOT EXISTS c1 AS SELECT 'abc'::int") == "22P02");
    CHECK(state(*db, "CREATE TABLE c1 AS SELECT 40000::smallint") == "42P07");
    // A parameter beside a constant that fails to fold is still the statement's.
    const auto& failing = db->prepare("CREATE TABLE c1 AS SELECT 1/0 AS a, $1::int AS b");
    REQUIRE(failing.params().size() == 1);
    CHECK(state(*db, "CREATE TABLE c1 AS SELECT 1/0 AS a, $1::int AS b", {i(5)}) == "42P07");
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT 1/0 AS a, $1::int AS b", {i(5)}) == "22012");
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT a FROM src WHERE a > 1/0 AND a = $1", {i(5)}) == "22012");
    CHECK(db->execute("CREATE TABLE IF NOT EXISTS c1 AS SELECT 1/0 AS a, $1::int AS b", {i(5)}).notices.size() == 1);
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT 1/0 AS x WITH NO DATA") == "0A000");
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT nope FROM src") == "42703");  // bound at prepare
    CHECK(state(*db, "CREATE TABLE c8 (x, y, z) AS SELECT 1, 2") == "42601");
    CHECK(state(*db, "CREATE TABLE c8 (x, x) AS SELECT 1, 2") == "42701");
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT 1 AS q, 2 AS q") == "42701");
    // Not built yet: a NUMERIC of no precision, and a table of no columns.
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT 1.5") == "0A000");
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT AVG(a) FROM src") == "0A000");
    CHECK(state(*db, "CREATE TABLE c8 AS SELECT") == "0A000");
    CHECK(db->catalog()->table({kPublicSchema, "c8"}) == nullptr);
    // In a transaction too (tests/api_txn_ddl_test.cpp): rolled back, nothing.
    auto txn = db->begin();
    CHECK(txn.execute("SELECT 1 AS x INTO c8").affected == 1);
    txn.rollback();
    CHECK(db->catalog()->table({kPublicSchema, "c8"}) == nullptr);
    db->close();
  }
  {
    // Kept: the table, its rows and its next row id.
    auto db = Database::open(dir.path());
    CHECK(db->execute("SELECT COUNT(*) FROM c1").rows == Grid{{i(3)}});
    (void)db->execute("INSERT INTO c4 VALUES (8)");
    CHECK(db->execute("SELECT a FROM c4 ORDER BY a").rows == Grid{{i(1)}, {i(2)}, {i(7)}, {i(8)}});
    db->close();
  }
}

namespace {
// The rows of every table the engine holds, read from the engine.
auto row_keys(bytecask::DB& engine) -> std::size_t {
  auto ns = row_prefix(0);
  ns.resize(1);  // the rows' namespace
  std::size_t n = 0;
  for (const auto& e : engine.iter_from({}, ns)) {
    if (!has_prefix(e.key, ns)) break;
    ++n;
  }
  return n;
}
}  // namespace

TEST_CASE("embedded API: CREATE TABLE AS writes its table and rows in one batch, within the statement's budget",
          "[sql][api][ctas]") {
  sql_test::TempDir dir{"api_ctas_atomic"};
  {
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE src (a INTEGER PRIMARY KEY, b TEXT)");
    for (std::int64_t k = 1; k <= 50; ++k) (void)db->execute("INSERT INTO src VALUES ($1, 'some text')", {i(k)});
    // Held mid-commit, before the batch reaches the engine: neither the table
    // nor any of its rows is there, in the catalog or the engine (only src's
    // 50 rows are), so the rows were not written in a batch of their own
    // before the table's.
    auto version = db->catalog()->version();
    CHECK(row_keys(db->engine_for_testing()) == 50);
    std::string seen;
    std::size_t rows_seen = 0;
    db->live_catalog_for_testing().set_commit_hook_for_testing([&] {
      std::thread reader{[&] {
        seen = db->catalog()->table({kPublicSchema, "c"}) == nullptr && db->catalog()->version() == version ? "absent" : "present";
        rows_seen = row_keys(db->engine_for_testing());
      }};
      reader.join();
    });
    CHECK(db->execute("CREATE TABLE c AS SELECT a, b FROM src").affected == 50);
    db->live_catalog_for_testing().set_commit_hook_for_testing({});
    CHECK(seen == "absent");
    CHECK(rows_seen == 50);
    CHECK(db->catalog()->version() == version + 1);
    CHECK(db->execute("SELECT COUNT(*) FROM c").rows == Grid{{i(50)}});
    CHECK(row_keys(db->engine_for_testing()) == 100);  // the table's rows came with it

    // A crash before the batch reaches the engine (the hook throws, and the
    // database is closed and reopened, below): no table and no row.
    db->live_catalog_for_testing().set_commit_hook_for_testing([] { throw std::runtime_error{"crash"}; });
    CHECK_THROWS(db->execute("CREATE TABLE crashed AS SELECT a, b FROM src"));
    db->live_catalog_for_testing().set_commit_hook_for_testing({});

    // The batch's limit refuses it whole (54000), nothing written.
    db->live_catalog_for_testing().set_backfill_limit_for_testing(512);
    try {
      (void)db->execute("CREATE TABLE d AS SELECT a, b FROM src");
      FAIL("ran");
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "54000");
    }
    db->live_catalog_for_testing().set_backfill_limit_for_testing(LiveCatalog::kMaxBackfillBytes);
    CHECK(db->catalog()->table({kPublicSchema, "d"}) == nullptr);
    CHECK(db->catalog()->version() == version + 1);
    db->close();
  }
  {
    // The statement's memory budget bounds the query's rows first: 54000,
    // before any batch is built.
    auto db = Database::open(dir.path(), DatabaseOptions{.statement_memory_bytes = 512});
    try {
      (void)db->execute("CREATE TABLE d AS SELECT a, b FROM src");
      FAIL("ran");
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "54000");
    }
    CHECK(db->catalog()->table({kPublicSchema, "d"}) == nullptr);
    CHECK(db->execute("SELECT COUNT(*) FROM c").rows == Grid{{i(50)}});  // after a reopen
    CHECK(db->catalog()->table({kPublicSchema, "crashed"}) == nullptr);
    CHECK(row_keys(db->engine_for_testing()) == 100);
    db->close();
  }
}

TEST_CASE("embedded API: INSERT … SELECT writes each of a query's rows as an INSERT of it would be", "[sql][api][insert]") {
  sql_test::TempDir dir{"api_insert_select"};
  auto db = Database::open(dir.path());
  auto text = [](std::string_view v) { return Value{sql_test::bytes(v)}; };
  auto state = [&](std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
    try {
      (void)db->execute(sql, params);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  (void)db->execute("CREATE TABLE src (a INTEGER PRIMARY KEY, b BIGINT, c VARCHAR(10), d DOUBLE PRECISION)");
  (void)db->execute("INSERT INTO src VALUES (1, 10, 'one', 1.5), (2, NULL, 'two', NULL), (3, 30, NULL, 3.5)");
  (void)db->execute("CREATE TABLE dst (k INTEGER DEFAULT 7, v BIGINT DEFAULT 9, s TEXT, f DOUBLE PRECISION, r REAL, "
                    "n VARCHAR(3), at DATE DEFAULT CURRENT_DATE)");
  CHECK(db->prepare("INSERT INTO dst SELECT a FROM src").command() == Command::Insert);

  // The leading columns, in order; the rest take their defaults, the clock's
  // read by the statement.
  CHECK(db->execute("INSERT INTO dst SELECT a, b FROM src WHERE a <= 2").affected == 2);
  CHECK(db->execute("SELECT k, v, s, at IS NOT NULL FROM dst ORDER BY k").rows ==
        Grid{{i(1), i(10), kNull, Value{true}}, {i(2), kNull, kNull, Value{true}}});
  // A column list in any order; a NULL is written as NULL, not the column's
  // default; an integer meets a DOUBLE or REAL column converted.
  (void)db->execute("DELETE FROM dst");
  CHECK(db->execute("INSERT INTO dst (s, v, k, f, r) SELECT c, a, b, a, b FROM src ORDER BY a").affected == 3);
  CHECK(db->execute("SELECT k, v, s, f, r FROM dst ORDER BY v").rows ==
        Grid{{i(10), i(1), text("one"), Value{1.0}, Value{10.0}},
             {kNull, i(2), text("two"), Value{2.0}, kNull},
             {i(30), i(3), kNull, Value{3.0}, Value{30.0}}});
  // A literal of no type, NULL and a parameter take the column's type, as a
  // VALUES item does.
  (void)db->execute("DELETE FROM dst");
  CHECK(db->execute("INSERT INTO dst (k, v, s, n) SELECT '5', NULL, $1, 'ab' FROM src WHERE a = $2", {text("p"), i(1)}).affected == 1);
  CHECK(db->execute("SELECT k, v, s, n FROM dst").rows == Grid{{i(5), kNull, text("p"), text("ab")}});
  CHECK(db->execute("INSERT INTO dst (k) SELECT 1").affected == 1);  // without FROM
  // No row, LIMIT 0 and an OFFSET past the rows write nothing.
  CHECK(db->execute("INSERT INTO dst (k) SELECT a FROM src WHERE a > 100").affected == 0);
  CHECK(db->execute("INSERT INTO dst (k) SELECT a FROM src WHERE FALSE").affected == 0);
  CHECK(db->execute("INSERT INTO dst (k) SELECT a FROM src LIMIT 0").affected == 0);
  CHECK(db->execute("INSERT INTO dst (k) SELECT a FROM src ORDER BY a OFFSET 2 LIMIT 5").affected == 1);
  // The plan writes each row the query emits, a NULL as NULL, and counts
  // them apart from a LIMIT's counter.
  const auto plan = db->prepare("INSERT INTO dst (k, v) SELECT a, b FROM src ORDER BY a LIMIT 2").plan_source();
  CHECK(plan.find("bc_row_set_null(cx, r, ") != std::string::npos);
  CHECK(plan.find("int64_t n = 0;") != std::string::npos);
  CHECK(plan.find("bc_emit_count(cx, inserted);") != std::string::npos);
  CHECK(db->execute("INSERT INTO dst (k, v) SELECT a, b FROM src ORDER BY a LIMIT 2").affected == 2);
  // RETURNING gives each row as written.
  CHECK(db->execute("INSERT INTO dst (k) SELECT a FROM src WHERE a = 2 RETURNING k, v").rows == Grid{{i(2), i(9)}});
  // A view and a join as the query.
  (void)db->execute("CREATE VIEW sv AS SELECT a, c FROM src WHERE a < 3");
  CHECK(db->execute("INSERT INTO dst (k, s) SELECT a, c FROM sv").affected == 2);
  CHECK(db->execute("INSERT INTO dst (k) SELECT x.a FROM src x JOIN src y ON x.a = y.a").affected == 3);

  CHECK(db->execute("INSERT INTO dst (n) SELECT c FROM src").affected == 3);  // each fits VARCHAR(3)
  // Data errors are the row builder's, and the statement writes nothing.
  const auto before = db->execute("SELECT COUNT(*) FROM dst").rows;
  CHECK(state("INSERT INTO dst (n) SELECT c || 'xx' FROM src") == "22001");
  CHECK(state("INSERT INTO dst (k) SELECT b * 1000000000 FROM src") == "22003");
  CHECK(state("INSERT INTO dst (k) SELECT 'x'") == "22P02");
  CHECK(db->execute("SELECT COUNT(*) FROM dst").rows == before);

  // PostgreSQL's analysis errors.
  CHECK(state("INSERT INTO dst (k) SELECT a, b FROM src") == "42601");
  CHECK(state("INSERT INTO dst (k, v) SELECT a FROM src") == "42601");
  CHECK(state("INSERT INTO dst SELECT a, a, c, d, d, c, a, a FROM src") == "42601");
  CHECK(state("INSERT INTO dst (k, k) SELECT a, a FROM src") == "42701");
  CHECK(state("INSERT INTO dst (zz) SELECT a FROM src") == "42703");
  CHECK(state("INSERT INTO dst (k) SELECT zz FROM src") == "42703");
  CHECK(state("INSERT INTO nope SELECT a FROM src") == "42P01");
  CHECK(state("INSERT INTO dst (k) SELECT c FROM src") == "42804");  // VARCHAR into INTEGER
  CHECK(state("INSERT INTO dst (at) SELECT a FROM src") == "42804");
  CHECK(state("INSERT INTO dst (k) SELECT $1::text") == "42804");
  // Not yet: an assignment cast the plan does not make (#824), and a query
  // whose rows come through no Emit (#682).
  CHECK(state("INSERT INTO dst (s) SELECT a FROM src") == "0A000");
  CHECK(state("INSERT INTO dst (k) SELECT d FROM src") == "0A000");
  CHECK(state("INSERT INTO dst (k) SELECT COUNT(*) FROM src") == "0A000");
  CHECK(state("INSERT INTO dst (k) SELECT a FROM src GROUP BY a") == "0A000");
  CHECK(state("INSERT INTO dst (k) SELECT DISTINCT a FROM src") == "0A000");
  CHECK(state("INSERT INTO dst (k) SELECT a FROM src UNION SELECT 1") == "0A000");
  CHECK(db->execute("SELECT COUNT(*) FROM dst").rows == before);
}

TEST_CASE("embedded API: INSERT … SELECT reads the snapshot its statement began on, never its own rows",
          "[sql][api][insert]") {
  sql_test::TempDir dir{"api_insert_select_snapshot"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (a INTEGER, b TEXT)");
  (void)db->execute("INSERT INTO t VALUES (1, 'x'), (2, 'y')");
  // Each run doubles the table once: the scan does not see the rows written.
  CHECK(db->execute("INSERT INTO t SELECT * FROM t").affected == 2);
  CHECK(db->execute("INSERT INTO t SELECT * FROM t").affected == 4);
  CHECK(db->execute("SELECT COUNT(*) FROM t").rows == Grid{{i(8)}});
  CHECK(db->execute("SELECT COUNT(*) FROM t WHERE a = 2").rows == Grid{{i(4)}});
  // A table with a key: its own rows again are duplicates, and nothing is written.
  (void)db->execute("CREATE TABLE k (a INTEGER PRIMARY KEY)");
  (void)db->execute("INSERT INTO k VALUES (1), (2)");
  try {
    (void)db->execute("INSERT INTO k SELECT a FROM k");
    FAIL("a duplicate key was written");
  } catch (const std::exception& e) {
    CHECK(sqlstate(e) == "23505");
  }
  CHECK(db->execute("INSERT INTO k SELECT a + 2 FROM k").affected == 2);
  CHECK(db->execute("SELECT a FROM k ORDER BY a").rows == Grid{{i(1)}, {i(2)}, {i(3)}, {i(4)}});
}

TEST_CASE("embedded API: a LEFT JOIN sorted by its right side, its lookup by key repeated above the sort", "[sql][api]") {
  // #838: the lookup repeated above the sort compares the left row's column
  // in its ON, so a sorter holds that row; and the ON's equalities with
  // constants leave a NULL-extended row's columns NULL, so they stay sort keys.
  sql_test::TempDir dir{"api_left_join_sort"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t1 (c0 BIGINT, c1 BIGINT, c2 INTEGER, PRIMARY KEY (c2, c0))");
  (void)db->execute("CREATE TABLE t3 (c0 BIGINT PRIMARY KEY, c1 TEXT)");
  const auto& by_right =
      db->prepare("SELECT t1.c0, t3.c0 FROM t3 LEFT JOIN t1 ON t1.c2 = 7 AND t1.c0 = 5 AND t1.c1 = t3.c0 ORDER BY t1.c1, t3.c0");
  const auto& first = db->prepare(
      "SELECT t1.c0, t1.c1, t1.c2 FROM t3 LEFT JOIN t1 ON t1.c2 = $1 AND t1.c0 = $2 AND t1.c1 = t3.c0 "
      "ORDER BY t1.c2 NULLS LAST, t1.c1 NULLS FIRST LIMIT 1");
  const auto& by_left =
      db->prepare("SELECT t3.c0, t1.c0 FROM t3 LEFT JOIN t1 ON t1.c2 = $1 AND t1.c0 = $2 AND t1.c1 = t3.c0 ORDER BY t3.c1, t3.c0");
  CHECK(db->execute(by_right, {}).rows.empty());
  CHECK(db->execute(first, {i(7), i(5)}).rows.empty());
  CHECK(db->execute(by_left, {i(7), i(5)}).rows.empty());
  (void)db->execute("INSERT INTO t3 VALUES (0, 'z'), (1, 'y'), (2, 'x')");
  (void)db->execute("INSERT INTO t1 VALUES (5, 1, 7), (6, 2, 7), (5, 3, 8)");
  CHECK(db->execute(by_right, {}).rows == Grid{{i(5), i(1)}, {kNull, i(0)}, {kNull, i(2)}});
  CHECK(db->execute(first, {i(7), i(5)}).rows == Grid{{i(5), i(1), i(7)}});
  CHECK(db->execute(first, {i(8), i(5)}).rows == Grid{{kNull, kNull, kNull}});
  CHECK(db->execute(by_left, {i(7), i(6)}).rows == Grid{{i(2), i(6)}, {i(1), kNull}, {i(0), kNull}});
  CHECK(db->execute("SELECT t1.c2, count(*) FROM t3 LEFT JOIN t1 ON t1.c2 = 7 AND t1.c1 = t3.c0 GROUP BY t1.c2 ORDER BY 1")
            .rows == Grid{{i(7), i(2)}, {kNull, i(1)}});
}

TEST_CASE("embedded API: INSERT … ON CONFLICT DO UPDATE updates the row that holds the key", "[sql][api]") {
  sql_test::TempDir dir{"api_on_conflict_update"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE ou (id INTEGER PRIMARY KEY, v INTEGER, u TEXT UNIQUE, w BIGINT DEFAULT 7, CHECK (v < 100))");
  (void)db->execute("INSERT INTO ou VALUES (1, 10, 'a'), (2, 20, 'b')");
  auto state = [&](const std::string& sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  auto all = [&] { return db->execute("SELECT id, v, u, w FROM ou ORDER BY id").rows; };
  auto t = [](const char* s) -> Value { return bytes(s); };
  constexpr auto kUpsert = "INSERT INTO ou VALUES ($1, $2, $3) ON CONFLICT (id) DO UPDATE SET v = excluded.v";
  // A taken key updates its row from EXCLUDED; a free one inserts. Both count.
  CHECK(db->execute(kUpsert, {i(1), i(11), t("x")}).affected == 1);
  CHECK(db->execute(kUpsert, {i(3), i(30), t("c")}).affected == 1);
  CHECK(all() == Grid{{i(1), i(11), t("a"), i(7)}, {i(2), i(20), t("b"), i(7)}, {i(3), i(30), t("c"), i(7)}});
  // RETURNING gives the row as updated, and as inserted; the holder's columns, qualified, and constants.
  auto r = db->execute(
      "INSERT INTO ou VALUES (2, 0, 'q'), (4, 40, 'd') ON CONFLICT (id) DO UPDATE SET v = ou.v + 1, w = 8 RETURNING id, v, w");
  CHECK(r.affected == 2);
  CHECK(r.rows == Grid{{i(2), i(21), i(8)}, {i(4), i(40), i(7)}});
  // A unique index as the arbiter; EXCLUDED of a column the row leaves out is its default.
  CHECK(db->execute("INSERT INTO ou (id, v, u) VALUES (9, 1, 'a') ON CONFLICT (u) DO UPDATE SET w = excluded.w, v = excluded.id")
            .affected == 1);
  CHECK(db->execute("SELECT v, w FROM ou WHERE id = 1").rows == Grid{{i(9), i(7)}});
  // A row the statement inserted or updated, hit again: 21000, and nothing is written.
  CHECK(state("INSERT INTO ou VALUES (5, 1, 'e'), (5, 2, 'f') ON CONFLICT (id) DO UPDATE SET v = excluded.v") == "21000");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'e'), (1, 2, 'f') ON CONFLICT (id) DO UPDATE SET v = excluded.v") == "21000");
  CHECK(state("INSERT INTO ou VALUES (6, 1, 'a'), (7, 2, 'a') ON CONFLICT (u) DO UPDATE SET v = excluded.v") == "21000");
  // PostgreSQL's hint goes with it.
  try {
    (void)db->execute("INSERT INTO ou VALUES (5, 1, 'e'), (5, 2, 'f') ON CONFLICT (id) DO UPDATE SET v = excluded.v");
    FAIL("no 21000");
  } catch (const std::exception& e) {
    CHECK(error_hint(e) ==
          "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.");
  }
  // CHECK on the updated row (23514), and a SET that moves the key onto another row's (23505).
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = 200") == "23514");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = excluded.v + 0") == "42601");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET id = 2") == "23505");
  CHECK(all() == Grid{{i(1), i(9), t("a"), i(7)}, {i(2), i(21), t("b"), i(8)}, {i(3), i(30), t("c"), i(7)}, {i(4), i(40), t("d"), i(7)}});
  // The key a SET vacated is free: the statement's later row with it is inserted.
  r = db->execute("INSERT INTO ou VALUES (1, 50, 'y'), (1, 51, 'z') ON CONFLICT (id) DO UPDATE SET id = 10 RETURNING id, v");
  CHECK(r.rows == Grid{{i(10), i(9)}, {i(1), i(51)}});
  // A unique prefix a SET took from its row is free too, in autocommit and in a transaction; one
  // the updated row kept is a second hit.
  r = db->execute("INSERT INTO ou VALUES (11, 0, 'a'), (12, 1, 'a') ON CONFLICT (u) DO UPDATE SET u = 'aa' RETURNING id, u");
  CHECK(r.rows == Grid{{i(10), t("aa")}, {i(12), t("a")}});
  {
    auto tx = db->begin();
    r = tx.execute("INSERT INTO ou VALUES (13, 0, 'b'), (14, 1, 'b') ON CONFLICT (u) DO UPDATE SET u = 'bb' RETURNING id, u");
    CHECK(r.rows == Grid{{i(2), t("bb")}, {i(14), t("b")}});
    CHECK(tx.commit() == CommitOutcome::Committed);
  }
  CHECK(state("INSERT INTO ou VALUES (15, 0, 'c'), (16, 1, 'c') ON CONFLICT (u) DO UPDATE SET v = 0") == "21000");
  CHECK(state("INSERT INTO ou VALUES (15, 0, 'c'), (16, 1, 'c') ON CONFLICT (u) DO UPDATE SET id = 17") == "21000");
  // Binding: unqualified is ambiguous, an unknown column, a type PostgreSQL does not assign, no target.
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = v") == "42702");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = excluded.zz") == "42703");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET zz = 1") == "42703");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = excluded.u") == "42804");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = 1, v = 2") == "42601");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT (id) DO UPDATE SET v = zz.v") == "42P01");
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x') ON CONFLICT DO UPDATE SET v = 1") == "42601");
  CHECK(state("INSERT INTO ou SELECT id, v, u FROM ou ON CONFLICT (id) DO UPDATE SET v = 1") == "42601");
  // Integers assign across their types, the value checked against the column's.
  CHECK(db->execute("INSERT INTO ou VALUES (1, 1, 'x', 5) ON CONFLICT (id) DO UPDATE SET v = excluded.w").affected == 1);
  CHECK(state("INSERT INTO ou VALUES (1, 1, 'x', 5000000000) ON CONFLICT (id) DO UPDATE SET v = excluded.w") == "22003");
  CHECK(db->execute("SELECT v FROM ou WHERE id = 1").rows == Grid{{i(5)}});
}

TEST_CASE("embedded API: INSERT … ON CONFLICT DO NOTHING skips a row whose arbiter key is taken", "[sql][api]") {
  sql_test::TempDir dir{"api_on_conflict"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE oc (id INTEGER PRIMARY KEY, v INTEGER UNIQUE, w INTEGER NOT NULL, CHECK (w > 0))");
  (void)db->execute("INSERT INTO oc VALUES ($1, $2, $3)", {i(1), i(10), i(1)});
  auto affected = [&](const std::string& sql) { return db->execute(sql).affected; };
  auto state = [&](const std::string& sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ran";
  };
  // No target: any unique key.
  CHECK(affected("INSERT INTO oc VALUES (1, 11, 1) ON CONFLICT DO NOTHING") == 0);
  CHECK(affected("INSERT INTO oc VALUES (2, 10, 1) ON CONFLICT DO NOTHING") == 0);
  // A target names the arbiter, as a set of columns; another key's duplicate is 23505.
  CHECK(affected("INSERT INTO oc VALUES (1, 11, 1) ON CONFLICT (id) DO NOTHING") == 0);
  CHECK(affected("INSERT INTO oc VALUES (2, 10, 1) ON CONFLICT (v) DO NOTHING") == 0);
  CHECK(affected("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT (id, id) DO NOTHING") == 0);
  CHECK(state("INSERT INTO oc VALUES (2, 10, 1) ON CONFLICT (id) DO NOTHING") == "23505");
  CHECK(state("INSERT INTO oc VALUES (1, 11, 1) ON CONFLICT (v) DO NOTHING") == "23505");
  // NOT NULL and CHECK come first.
  CHECK(state("INSERT INTO oc VALUES (1, 10, NULL) ON CONFLICT DO NOTHING") == "23502");
  CHECK(state("INSERT INTO oc VALUES (1, 10, -1) ON CONFLICT DO NOTHING") == "23514");
  // The count and RETURNING are the rows written; a later row of the
  // statement skips on an earlier one's key.
  auto r = db->execute("INSERT INTO oc VALUES (5, 50, 1), (5, 51, 1), (6, 50, 1) ON CONFLICT DO NOTHING RETURNING id, v");
  CHECK(r.affected == 1);
  CHECK(r.rows == Grid{{i(5), i(50)}});
  CHECK(affected("INSERT INTO oc SELECT id + 100, v, w FROM oc ON CONFLICT (v) DO NOTHING") == 0);
  CHECK(affected("INSERT INTO oc SELECT id + 100, v + 100, w FROM oc ON CONFLICT DO NOTHING") == 2);
  CHECK(ids(db->execute("SELECT id FROM oc ORDER BY id")) == std::vector<std::int64_t>{1, 5, 101, 105});
  // Parameters, through a cached plan.
  for (auto v : {std::int64_t{7}, std::int64_t{7}}) {
    (void)db->execute("INSERT INTO oc VALUES ($1, $2, $3) ON CONFLICT DO NOTHING", {i(v), i(v), i(1)});
  }
  CHECK(db->execute("INSERT INTO oc VALUES ($1, $2, $3) ON CONFLICT DO NOTHING", {i(7), i(70), i(1)}).affected == 0);
  // Refused: no constraint matches (42P10), an unknown column (42703), and
  // the forms not built yet (0A000).
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT (w) DO NOTHING") == "42P10");
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT (id, v) DO NOTHING") == "42P10");
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT (zz) DO NOTHING") == "42703");
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT ON CONSTRAINT oc_pkey DO NOTHING") == "0A000");
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT (id) WHERE id > 0 DO NOTHING") == "0A000");
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT ((id + 1)) DO NOTHING") == "0A000");
  CHECK(state("INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT (id) DO UPDATE SET v = 1 WHERE oc.v > 0") == "0A000");
  (void)db->execute("CREATE VIEW ocv AS SELECT id, v, w FROM oc");
  CHECK(state("INSERT INTO ocv VALUES (1, 10, 1) ON CONFLICT DO NOTHING") == "0A000");
  (void)db->execute("CREATE PROCEDURE ocp() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO oc VALUES (1, 10, 1) ON CONFLICT DO "
                    "NOTHING; END $$");
  try {
    (void)db->call("ocp", {});
    FAIL("ON CONFLICT ran in a procedure");
  } catch (const std::exception& e) {
    CHECK(sqlstate(e) == "0A000");
  }
  // A table without a primary key: its unique index is the only arbiter.
  (void)db->execute("CREATE TABLE ocn (a INTEGER, b INTEGER)");
  CHECK(affected("INSERT INTO ocn VALUES (1, 1) ON CONFLICT DO NOTHING") == 1);
  CHECK(affected("INSERT INTO ocn VALUES (1, 1) ON CONFLICT DO NOTHING") == 1);
  CHECK(state("INSERT INTO ocn VALUES (1, 1) ON CONFLICT (a) DO NOTHING") == "42P10");
  (void)db->execute("DELETE FROM ocn WHERE b = 1");
  (void)db->execute("CREATE UNIQUE INDEX ocn_a ON ocn (a)");
  CHECK(affected("INSERT INTO ocn VALUES (1, 1), (1, 2) ON CONFLICT (a) DO NOTHING") == 1);
}

TEST_CASE("embedded API: a NAME column cuts a value to 63 bytes at a character boundary, keys and compares as text",
          "[sql][api][name]") {
  sql_test::TempDir dir{"api_name"};
  auto state = [](Database& d, std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
    try {
      (void)d.execute(std::string{sql}, params);
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ok";
  };
  auto text = [](Database& d, std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::string> out;
    auto r = d.execute(std::string{sql}, params);
    for (const auto& row : r.rows) out.push_back(to_text(row.at(0), r.types.at(0)));
    return out;
  };
  using V = std::vector<std::string>;
  auto repeat = [](std::string_view s, int n) {
    std::string out;
    for (int k = 0; k < n; ++k) out += s;
    return out;
  };
  const auto a70 = repeat("a", 70);
  const auto a63 = repeat("a", 63);
  const auto e40 = repeat("\xC3\xA9", 40);  // é: 2 bytes, cut to 31 of them (62 bytes)
  {
    auto db = Database::open(dir.path());
    CHECK(state(*db, "CREATE TABLE t (k NAME PRIMARY KEY, v INTEGER, s TEXT)") == "ok");
    CHECK(db->execute("SELECT k FROM t").types == std::vector<SqlType>{{SqlKind::Name}});
    CHECK(state(*db, "INSERT INTO t VALUES ('" + a70 + "', 1, '" + a70 + "')") == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES ($1, 2, 'x')", {bytes(e40)}) == "ok");
    CHECK(state(*db, "INSERT INTO t VALUES ('bob', 3, 'bob')") == "ok");
    CHECK(text(*db, "SELECT k FROM t WHERE v = 1") == V{a63});
    CHECK(text(*db, "SELECT k FROM t WHERE v = 2") == V{repeat("\xC3\xA9", 31)});
    // The key is the cut value: a longer one with its 63 bytes is taken.
    CHECK(state(*db, "INSERT INTO t VALUES ($1, 4, 'x')", {bytes(a70 + "zz")}) == "23505");
    // A literal or a $n compared with it is a NAME, cut; a TEXT is not.
    CHECK(text(*db, "SELECT v FROM t WHERE k = '" + a70 + "b'") == V{"1"});
    CHECK(text(*db, "SELECT v FROM t WHERE k = $1", {bytes(a70 + "c")}) == V{"1"});
    CHECK(text(*db, "SELECT v FROM t WHERE k = s ORDER BY v") == V{"3"});
    // Bytewise order, GROUP BY, DISTINCT, LIKE, || and COUNT, as text.
    CHECK(text(*db, "SELECT v FROM t WHERE v > 0 ORDER BY k") == V{"1", "3", "2"});
    CHECK(text(*db, "SELECT k FROM t WHERE k LIKE 'b%'") == V{"bob"});
    CHECK(text(*db, "SELECT k || '!' FROM t WHERE v = 3") == V{"bob!"});
    CHECK(text(*db, "SELECT COUNT(DISTINCT k) FROM t") == V{"3"});
    CHECK(text(*db, "SELECT 'x'::name::text") == V{"x"});
    CHECK(text(*db, "SELECT k::text FROM t WHERE v = 3") == V{"bob"});
    CHECK(text(*db, "SELECT $1::name", {bytes(a70)}) == V{a63});
    // A typed string cast to name is cut as an unknown literal is.
    CHECK(text(*db, "SELECT '" + a70 + "'::text::name") == V{a63});
    CHECK(text(*db, "SELECT ('" + a70 + "'::varchar)::name") == V{a63});
    CHECK(text(*db, "SELECT CAST(CAST('" + a70 + "' AS text) AS name) = CAST('" + a70 + "' AS name)") == V{"t"});
    CHECK(text(*db, "SELECT DISTINCT k FROM t WHERE v = 3") == V{"bob"});
    // PostgreSQL's errors: no operator with a number, no SUM of it.
    CHECK(state(*db, "SELECT v FROM t WHERE k = 1") == "42883");
    CHECK(state(*db, "SELECT SUM(k) FROM t") == "42883");
    // A NAME from another string type, which PostgreSQL cuts: refused.
    CHECK(state(*db, "SELECT COALESCE(k, s) FROM t") == "0A000");
    CHECK(text(*db, "SELECT COALESCE(s, k) FROM t WHERE v = 3") == V{"bob"});
    CHECK(state(*db, "SELECT k FROM t UNION SELECT s FROM t") == "0A000");
    // A DEFAULT literal is cut too.
    CHECK(state(*db, "CREATE TABLE d (n NAME DEFAULT '" + a70 + "', i INTEGER)") == "ok");
    CHECK(state(*db, "INSERT INTO d (i) VALUES (1)") == "ok");
    CHECK(text(*db, "SELECT n FROM d") == V{a63});
  }
  auto db = Database::open(dir.path());
  CHECK(db->execute("SELECT k FROM t").types.at(0) == SqlType{SqlKind::Name});
  CHECK(text(*db, "SELECT v FROM t WHERE k = 'bob'") == V{"3"});
}

TEST_CASE("embedded API: CURRENT_USER and its kin, CURRENT_CATALOG and CURRENT_SCHEMA are NAMEs read on each run",
          "[sql][api][name]") {
  sql_test::TempDir dir{"api_session_fns"};
  auto text = [](Database& d, std::string_view sql, const ExecutionOptions& opts = {}) {
    std::vector<std::string> out;
    auto r = d.execute(std::string{sql}, {}, opts);
    for (const auto& row : r.rows) {
      for (std::size_t c = 0; c < row.size(); ++c) out.push_back(to_text(row.at(c), r.types.at(c)));
    }
    return out;
  };
  auto state = [](Database& d, std::string_view sql) -> std::string {
    try {
      (void)d.execute(std::string{sql}, {});
    } catch (const std::exception& e) {
      return std::string{sqlstate(e)};
    }
    return "ok";
  };
  using V = std::vector<std::string>;
  DatabaseOptions o;
  o.name = "shop";
  auto db = Database::open(dir.path(), o);
  const auto sql = "SELECT CURRENT_USER, CURRENT_ROLE, USER, SESSION_USER, CURRENT_CATALOG, CURRENT_SCHEMA";
  auto r = db->execute(sql);
  CHECK(r.columns == std::vector<std::string>{"current_user", "current_role", "user", "session_user", "current_catalog",
                                              "current_schema"});
  CHECK(r.types == std::vector<SqlType>(6, SqlType{SqlKind::Name}));
  CHECK(text(*db, sql) == V{"bytecask", "bytecask", "bytecask", "bytecask", "shop", "public"});
  // The same cached plan serves another session's user: bound per run.
  ExecutionOptions ann;
  ann.user = "ann";
  CHECK(text(*db, sql, ann) == V{"ann", "ann", "ann", "ann", "shop", "public"});
  CHECK(text(*db, "SELECT CURRENT_USER || '!', CURRENT_USER = 'bytecask'") == V{"bytecask!", "t"});
  // A DEFAULT is the reading of the statement that inserts the row, in a
  // transaction its user's.
  CHECK(state(*db, "CREATE TABLE t (id INTEGER, u NAME DEFAULT CURRENT_USER, s TEXT DEFAULT SESSION_USER, "
                   "c NAME DEFAULT CURRENT_CATALOG, sc VARCHAR(10) DEFAULT CURRENT_SCHEMA)") == "ok");
  (void)db->execute("INSERT INTO t (id) VALUES (1)", {}, ann);
  {
    TransactionOptions to;
    to.user = "bo";
    auto txn = db->begin(to);
    (void)txn.execute("INSERT INTO t (id) VALUES (2)", {});
    (void)txn.execute("INSERT INTO t VALUES (3, DEFAULT, DEFAULT, DEFAULT, DEFAULT)", {});
    CHECK(txn.commit() == CommitOutcome::Committed);
  }
  CHECK(text(*db, "SELECT id, u, s, c, sc FROM t ORDER BY id") ==
        V{"1", "ann", "ann", "shop", "public", "2", "bo", "bo", "shop", "public", "3", "bo", "bo", "shop", "public"});
  CHECK(text(*db, "SELECT id FROM t WHERE u = CURRENT_USER", ann) == V{"1"});
  CHECK(text(*db, "SELECT id FROM t WHERE CURRENT_CATALOG = c ORDER BY id") == V{"1", "2", "3"});
  // PostgreSQL's 42804 for a type no NAME is assigned to.
  CHECK(state(*db, "CREATE TABLE x (a INTEGER DEFAULT CURRENT_USER)") == "42804");
  CHECK(state(*db, "CREATE TABLE x (a DATE DEFAULT CURRENT_SCHEMA)") == "42804");
  // A NAME too long for a VARCHAR(n) is 22001 where it is written.
  CHECK(state(*db, "CREATE TABLE y (a VARCHAR(3) DEFAULT CURRENT_CATALOG, b INTEGER)") == "ok");
  CHECK(state(*db, "INSERT INTO y (b) VALUES (1)") == "22001");
  // current_database() and current_schema() are CURRENT_CATALOG and
  // CURRENT_SCHEMA spelled as calls, named by the call.
  CHECK(text(*db, "SELECT current_database(), pg_catalog.current_schema()") == V{"shop", "public"});
  CHECK(state(*db, "CREATE TABLE w (id INTEGER, c NAME DEFAULT current_database())") == "ok");
  // Refused as gaps of PostgreSQL's dialect (0A000): compared in WHERE with
  // no column, in a CHECK, an aggregated select list, ADD COLUMN's DEFAULT.
  CHECK(state(*db, "SELECT id FROM t WHERE CURRENT_USER = 'bytecask'") == "0A000");
  CHECK(state(*db, "CREATE TABLE z (a NAME CHECK (a = CURRENT_USER))") == "0A000");
  CHECK(state(*db, "SELECT COUNT(*), CURRENT_USER FROM t") == "0A000");
  CHECK(state(*db, "ALTER TABLE t ADD COLUMN w NAME DEFAULT CURRENT_USER") == "0A000");
  // The clock's date on a NAME column has PostgreSQL's I/O cast, not built.
  CHECK(state(*db, "CREATE TABLE z (a NAME DEFAULT CURRENT_DATE)") == "42601");
  // A user name past 63 bytes is cut, as a NAME.
  ExecutionOptions longer;
  const auto long_name = std::string(70, 'u');
  longer.user = long_name;
  CHECK(text(*db, "SELECT CURRENT_USER", longer) == V{std::string(63, 'u')});
}
