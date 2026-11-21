// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Stage 0 of transactional DDL (docs/transaction_design.md, *Stages of
// transactional DDL*): a cached plan binds again after every DDL, and a
// statement's result type follows PostgreSQL 17 (a run by text returns its
// catalog's columns, a prepared statement whose columns change is 0A000); a
// transaction binds only against the catalog it pinned, with the rule's
// 40001s where that catalog lacks what a statement needs. T is a transaction
// pinned at v1; B is another session's DDL, published as v2.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

namespace {

auto i(std::int64_t v) -> Value { return Value{v}; }

// The SQLSTATE `f` fails with, "ok" when it runs.
template <typename F>
auto state(F&& f) -> std::string {
  try {
    f();
    return "ok";
  } catch (const std::exception& e) {
    return std::string{sqlstate(e)};
  }
}

auto names(const Rows& r) -> std::vector<std::string> { return r.columns; }

auto names(const std::vector<ResultColumn>& cols) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& c : cols) out.push_back(c.name);
  return out;
}

using Names = std::vector<std::string>;
using Grid = std::vector<std::vector<Value>>;

// A transaction whose first statement has pinned the catalog as it is now.
auto pinned(Database& db) -> Transaction {
  auto t = db.begin();
  (void)t.execute("SELECT 1");
  return t;
}

}  // namespace

TEST_CASE("rebind: by text a statement returns its catalog's columns; a held one whose columns change is 0A000",
          "[sql][api][rebind]") {
  sql_test::TempDir dir{"rebind_shape"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  const auto& star = db->prepare("SELECT * FROM t");
  const auto& t_star = db->prepare("SELECT t.* FROM t");
  const auto& returning = db->prepare("INSERT INTO t VALUES ($1, $2) RETURNING *");
  CHECK(names(db->execute(star, {})) == Names{"k", "a"});
  (void)db->execute("ALTER TABLE t ADD COLUMN b BIGINT");

  // By text: `*` and t.* expand again; an INSERT without a column list fills
  // the leading columns.
  CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a", "b"});
  CHECK(names(db->execute("SELECT t.* FROM t")) == Names{"k", "a", "b"});
  CHECK(names(db->execute("INSERT INTO t VALUES ($1, $2) RETURNING *", {i(2), i(20)})) == Names{"k", "a", "b"});
  // Held: 0A000 at execute and refit, and in a transaction at execute and open_cursor.
  for (const auto* held : {&star, &t_star}) {
    CHECK(state([&] { (void)db->execute(*held, {}); }) == "0A000");
    CHECK(state([&] { db->refit(*held); }) == "0A000");
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute(*held, {}); }) == "0A000");
    CHECK(t.is_aborted());
    auto u = db->begin();
    CHECK(state([&] { (void)u.open_cursor(*held, {}); }) == "0A000");
  }
  CHECK(state([&] { (void)db->execute(returning, {i(3), i(30)}); }) == "0A000");
  try {
    (void)db->execute(star, {});
  } catch (const FeatureNotSupported& e) {
    CHECK(std::string{e.what()} == "cached plan must not change result type");
  }
  // The held statement keeps what it fixed; a prepare of the text again
  // returns the new statement, as PostgreSQL's DEALLOCATE and PREPARE do.
  CHECK(names(star.columns()) == Names{"k", "a"});
  const auto& again = db->prepare("SELECT * FROM t");
  CHECK(&again != &star);
  CHECK(names(again.columns()) == Names{"k", "a", "b"});
  CHECK(db->execute(again, {}).rows.size() == 2);

  // A name a new column makes ambiguous is 42702, held and by text.
  (void)db->execute("CREATE TABLE u (j BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO u VALUES (1)");
  const auto& pick = db->prepare("SELECT a FROM t, u");
  CHECK(db->execute(pick, {}).rows.size() == 2);
  (void)db->execute("ALTER TABLE u ADD COLUMN a BIGINT");
  CHECK(state([&] { (void)db->execute(pick, {}); }) == "42702");
  CHECK(state([&] { (void)db->execute("SELECT a FROM t, u"); }) == "42702");

  // NATURAL and USING resolve again: v gains a, which NATURAL then joins on.
  (void)db->execute("CREATE TABLE v (k BIGINT PRIMARY KEY, x BIGINT)");
  (void)db->execute("INSERT INTO v VALUES (1, 5)");
  const auto& natural = db->prepare("SELECT k FROM t NATURAL JOIN v");
  const auto& using_k = db->prepare("SELECT * FROM t JOIN v USING (k)");
  CHECK(db->execute(natural, {}).rows == Grid{{i(1)}});
  CHECK(names(db->execute(using_k, {})) == Names{"k", "a", "b", "x"});
  (void)db->execute("ALTER TABLE v ADD COLUMN a BIGINT");
  CHECK(db->execute(natural, {}).rows.empty());  // a = 10 does not match a NULL a
  CHECK(names(db->execute("SELECT * FROM t JOIN v USING (k)")) == Names{"k", "a", "b", "x", "a"});
  CHECK(state([&] { (void)db->execute(using_k, {}); }) == "0A000");

  // A table dropped and created again with another column type: the new
  // type by text, 0A000 held.
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY, c BIGINT)");
  const auto& w_c = db->prepare("SELECT c FROM w");
  (void)db->execute(w_c, {});
  (void)db->execute("DROP TABLE w");
  (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY, c TEXT)");
  CHECK(db->execute("SELECT c FROM w").types == std::vector<SqlType>{SqlType{SqlKind::Text}});
  CHECK(state([&] { (void)db->execute(w_c, {}); }) == "0A000");

  // A view over a named column keeps its columns; one over `*` is refused
  // when read once its table has other columns, until it is created again.
  (void)db->execute("CREATE VIEW va AS SELECT a FROM t");
  (void)db->execute("CREATE VIEW vs AS SELECT * FROM t");
  (void)db->execute("ALTER TABLE t ADD COLUMN c BIGINT");
  CHECK(names(db->execute("SELECT * FROM va")) == Names{"a"});
  CHECK(state([&] { (void)db->execute("SELECT * FROM vs"); }) == "0A000");
}

TEST_CASE("rebind: a held statement keeps the parameter types it fixed; by text they are inferred again",
          "[sql][api][rebind]") {
  sql_test::TempDir dir{"rebind_params"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE pt (a INTEGER PRIMARY KEY)");
  constexpr auto kOne = "SELECT 1 FROM pt WHERE a = $1";
  const auto& one = db->prepare(kOne);
  const auto& col = db->prepare("SELECT a FROM pt WHERE a = $1");
  (void)db->execute(one, {i(5)});
  (void)db->execute(col, {i(5)});
  // B: a is BIGINT now.
  (void)db->execute("DROP TABLE pt");
  (void)db->execute("CREATE TABLE pt (a BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO pt VALUES (5000000000), (5)");

  // In a transaction pinned after B, the held statement runs at INTEGER from
  // a private plan; nothing is swapped into it, or into the cache.
  {
    auto t = db->begin();
    CHECK(t.execute(one, {i(5)}).rows.size() == 1);
    CHECK(t.private_plans_for_testing() == 1);
    CHECK(one.params().at(0).type == SqlType{SqlKind::Integer});
    // Its result column changed: 0A000, the record no later than the pin.
    CHECK(state([&] { (void)t.execute(col, {i(5)}); }) == "0A000");
  }
  // By text, $1 is BIGINT: the cache then holds a statement of BIGINT $1,
  // in autocommit and in a transaction.
  CHECK(db->execute(kOne, {i(5'000'000'000)}).rows.size() == 1);
  CHECK(db->prepare(kOne).params().at(0).type == SqlType{SqlKind::BigInt});
  CHECK(&db->prepare(kOne) != &one);
  {
    auto t = db->begin();
    CHECK(t.execute(kOne, {i(5'000'000'000)}).rows.size() == 1);
    CHECK(t.private_plans_for_testing() == 0);
  }
  // In autocommit the held statement is bound at INTEGER and swapped into it.
  auto compiled = db->compilations();
  CHECK(db->execute(one, {i(5)}).rows.size() == 1);
  CHECK(db->execute(one, {i(5)}).rows.size() == 1);
  CHECK(db->compilations() == compiled + 1);
  CHECK(one.params().at(0).type == SqlType{SqlKind::Integer});
  CHECK(state([&] { (void)db->execute(one, {i(5'000'000'000)}); }) == "22003");
  CHECK(state([&] { (void)db->execute(col, {i(5)}); }) == "0A000");
}

TEST_CASE("rebind: a record's fixed parameter type that no longer fits is the binder's error, 40001 at a stale pin",
          "[sql][api][rebind]") {
  sql_test::TempDir dir{"rebind_given"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE pt (a INTEGER PRIMARY KEY)");
  constexpr auto kOne = "SELECT 1 FROM pt WHERE a = $1";
  // The server's record of a Parse: $1 int4.
  auto record = db->refit(kOne, ResultCheck::none());
  REQUIRE(record.params.at(0).type == SqlType{SqlKind::Integer});
  // B: a is a DATE; an autocommit run by text puts a $1 date statement in the cache.
  (void)db->execute("DROP TABLE pt");
  (void)db->execute("CREATE TABLE pt (a DATE PRIMARY KEY)");
  CHECK(db->execute(kOne, {i(1)}).rows.empty());
  CHECK(db->prepare(kOne).params().at(0).type == SqlType{SqlKind::Date});
  const auto check = ResultCheck::given(record);
  auto compiled = db->compilations();
  CHECK(state([&] { (void)db->refit(kOne, check); }) == "42883");
  CHECK(state([&] { (void)db->execute(kOne, check, {i(1)}); }) == "42883");
  CHECK(db->compilations() == compiled);  // the binder's error: nothing compiled, nothing cached
  CHECK(db->prepare(kOne).params().at(0).type == SqlType{SqlKind::Date});
  // In a transaction pinned after B with another DDL published since: 40001,
  // and its retry 42883.
  auto t = pinned(*db);
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)t.execute(kOne, check, {i(1)}); }) == "40001");
  CHECK_FALSE(t.is_open());
  auto retry = db->begin();
  CHECK(state([&] { (void)retry.execute(kOne, check, {i(1)}); }) == "42883");
}

TEST_CASE("rebind: a fixed INTEGER parameter assigned to a column now BIGINT is range-checked as INTEGER",
          "[sql][api][rebind]") {
  sql_test::TempDir dir{"rebind_assigned_width"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE pw (k INTEGER PRIMARY KEY, a INTEGER)");
  const auto& ins = db->prepare("INSERT INTO pw VALUES ($1, $2)");
  const auto& upd = db->prepare("UPDATE pw SET a = $1 WHERE k = 1");
  (void)db->execute("DROP TABLE pw");
  (void)db->execute("CREATE TABLE pw (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO pw VALUES (1, 1)");
  // PostgreSQL's Bind reads each value as int4 first: 22003, nothing written.
  CHECK(state([&] { (void)db->execute(ins, {i(5'000'000'000), i(7)}); }) == "22003");
  CHECK(state([&] { (void)db->execute(ins, {i(8), i(5'000'000'000)}); }) == "22003");
  CHECK(state([&] { (void)db->execute(upd, {i(5'000'000'000)}); }) == "22003");
  CHECK(ins.params().at(0).type == SqlType{SqlKind::Integer});
  {
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute(ins, {i(6'000'000'000), i(7)}); }) == "22003");
  }
  {
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute(upd, {i(6'000'000'000)}); }) == "22003");
  }
  // Inside INTEGER, both run.
  CHECK(db->execute(ins, {i(2), i(7)}).affected == 1);
  CHECK(db->execute(upd, {i(9)}).affected == 1);
  CHECK(db->execute("SELECT k, a FROM pw ORDER BY k").rows == Grid{{i(1), i(9)}, {i(2), i(7)}});
}

TEST_CASE("rebind: a fixed parameter type that no longer fits: PostgreSQL's SQLSTATE where it refuses too, 42601 where it "
          "converts",
          "[sql][api][rebind]") {
  sql_test::TempDir dir{"rebind_misfit"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE pm (k INTEGER PRIMARY KEY, a INTEGER)");
  const auto& cmp = db->prepare("SELECT k FROM pm WHERE a = $1");
  const auto& ins = db->prepare("INSERT INTO pm VALUES ($1, $2)");
  const auto& upd = db->prepare("UPDATE pm SET a = $1 WHERE k = $2");
  auto retype = [&](const char* type) {
    (void)db->execute("DROP TABLE pm");
    (void)db->execute(std::string{"CREATE TABLE pm (k INTEGER PRIMARY KEY, a "} + type + ")");
  };
  auto message = [](auto&& f) -> std::string {
    try {
      f();
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  };
  // PostgreSQL converts the int4 value (int4 = float8, an assignment cast to
  // float8 or text); the layer does not yet: 42601, naming SQL types.
  retype("DOUBLE PRECISION");
  CHECK(state([&] { (void)db->execute(cmp, {i(5)}); }) == "42601");
  CHECK(message([&] { (void)db->execute(cmp, {i(5)}); }).find("$1 of type integer used with a (double precision)") !=
        std::string::npos);
  CHECK(state([&] { (void)db->execute(ins, {i(1), i(5)}); }) == "42601");
  CHECK(state([&] { (void)db->execute(upd, {i(5), i(1)}); }) == "42601");
  retype("TEXT");
  CHECK(state([&] { (void)db->execute(ins, {i(1), i(5)}); }) == "42601");
  CHECK(message([&] { (void)db->execute(ins, {i(1), i(5)}); }).find("$2 of type integer assigned to a (text)") !=
        std::string::npos);
  // PostgreSQL has no operator text = integer either: 42883.
  CHECK(state([&] { (void)db->execute(cmp, {i(5)}); }) == "42883");
  // Nor date = integer (42883), nor an assignment cast integer to date (42804).
  retype("DATE");
  CHECK(state([&] { (void)db->execute(cmp, {i(5)}); }) == "42883");
  CHECK(message([&] { (void)db->execute(cmp, {i(5)}); }).find("$1 of type integer cannot be used with a (date)") !=
        std::string::npos);
  CHECK(state([&] { (void)db->execute(ins, {i(1), i(5)}); }) == "42804");
  CHECK(message([&] { (void)db->execute(ins, {i(1), i(5)}); }) ==
        "column \"a\" is of type date but expression is of type integer");
}

TEST_CASE("rebind: B adds a column; T reads t with the columns of its pin", "[sql][api][rebind][txn]") {
  sql_test::TempDir dir{"rebind_add_column"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  const auto& before = db->prepare("SELECT * FROM t");
  auto t = pinned(*db);
  auto writer = pinned(*db);
  (void)db->execute("ALTER TABLE t ADD COLUMN b BIGINT DEFAULT 7");  // B
  CHECK(names(t.execute("SELECT * FROM t")) == Names{"k", "a"});
  CHECK(names(t.execute(before, {})) == Names{"k", "a"});
  auto record = t.refit("SELECT * FROM t", ResultCheck::none());
  CHECK(names(record.columns) == Names{"k", "a"});
  CHECK(names(t.execute("SELECT * FROM t", ResultCheck::given(record), {})) == Names{"k", "a"});
  // T's INSERT without a column list runs at the pin; its commit is 40001
  // (the table's record changed), and writes nothing.
  CHECK(writer.execute("INSERT INTO t VALUES ($1, $2)", {i(2), i(20)}).affected == 1);
  CHECK_THROWS_AS(writer.commit(), SerializationFailure);
  CHECK(db->execute("SELECT k FROM t").rows == Grid{{i(1)}});
  // A statement prepared after B fixed (k, a, b): 40001 in T, which ends.
  const auto& after = db->prepare("SELECT * FROM t");
  CHECK(names(after.columns()) == Names{"k", "a", "b"});
  CHECK(state([&] { (void)t.execute(after, {}); }) == "40001");
  CHECK_FALSE(t.is_open());
  // Its retry pins the live catalog and runs.
  auto retry = db->begin();
  CHECK(names(retry.execute(after, {})) == Names{"k", "a", "b"});
}

TEST_CASE("rebind: B drops t, or drops it and creates it again; T reads the old t, and a T that wrote it fails at commit",
          "[sql][api][rebind][txn]") {
  for (auto recreate : {false, true}) {
    INFO((recreate ? "dropped and created again" : "dropped"));
    sql_test::TempDir dir{"rebind_drop"};
    auto db = Database::open(dir.path());
    (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
    (void)db->execute("INSERT INTO t VALUES (1, 10), (2, 20)");
    const auto& held = db->prepare("SELECT a FROM t WHERE k = $1");
    auto t = pinned(*db);
    auto reader = pinned(*db);
    (void)db->execute("DROP TABLE t");  // B
    if (recreate) {
      (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a TEXT)");
      (void)db->execute("INSERT INTO t VALUES (1, 'x')");
    }
    CHECK(t.execute("SELECT a FROM t WHERE k = $1", {i(1)}).rows == Grid{{i(10)}});
    CHECK(t.execute(held, {i(2)}).rows == Grid{{i(20)}});
    auto c = t.open_cursor(held, {i(1)});
    CHECK(c.fetch(10).rows == Grid{{i(10)}});
    auto scan = t.open_cursor("SELECT k, a FROM t", ResultCheck::none(), {});
    CHECK(scan.fetch(1).rows == Grid{{i(1), i(10)}});
    CHECK(t.private_plans_for_testing() >= 2);
    CHECK(t.execute_current_of("UPDATE t SET a = $1 WHERE CURRENT OF c", ResultCheck::none(), {i(99)}, scan)
              .affected == 1);
    CHECK_THROWS_AS(t.commit(), SerializationFailure);
    CHECK(reader.execute("SELECT a FROM t WHERE k = $1", {i(2)}).rows == Grid{{i(20)}});
    CHECK(reader.commit() == CommitOutcome::Committed);
    if (recreate) {
      CHECK(db->execute("SELECT k, a FROM t").rows == Grid{{i(1), Value{sql_test::bytes("x")}}});
    } else {
      CHECK(state([&] { (void)db->execute("SELECT a FROM t"); }) == "42P01");
    }
  }
}

TEST_CASE("rebind: a name B created is 40001 in T, and its retry runs; a typo is 40001, then 42P01",
          "[sql][api][rebind][txn]") {
  sql_test::TempDir dir{"rebind_created"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE base (k BIGINT PRIMARY KEY)");
  (void)db->execute("INSERT INTO base VALUES (1), (7)");
  struct Case {
    std::string ddl;
    std::string sql;
  };
  for (const auto& c : std::vector<Case>{
           {"CREATE TABLE x (k BIGINT PRIMARY KEY)", "SELECT k FROM x"},
           {"CREATE VIEW w AS SELECT k FROM base", "SELECT k FROM w"},
       }) {
    INFO(c.ddl);
    auto t = pinned(*db);
    (void)db->execute(c.ddl);
    CHECK(state([&] { (void)t.execute(c.sql); }) == "40001");
    CHECK_FALSE(t.is_open());
    auto retry = db->begin();
    CHECK(state([&] { (void)retry.execute(c.sql); }) == "ok");
  }
  {
    auto t = pinned(*db);
    (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
    CHECK(state([&] { (void)t.execute("SELECT k FROM nosuch"); }) == "40001");
    auto retry = db->begin();
    CHECK(state([&] { (void)retry.execute("SELECT k FROM nosuch"); }) == "42P01");
  }
  // A view dropped and created again over the same table is read as at the pin.
  (void)db->execute("CREATE VIEW vv AS SELECT k FROM base WHERE k < 5");
  auto t = pinned(*db);
  (void)db->execute("DROP VIEW vv");
  (void)db->execute("CREATE VIEW vv AS SELECT k FROM base WHERE k >= 5");
  CHECK(t.execute("SELECT k FROM vv").rows == Grid{{i(1)}});
  CHECK(db->execute("SELECT k FROM vv").rows == Grid{{i(7)}});
}

TEST_CASE("rebind: binder errors at a stale pin are 40001; at a live pin they are reported as they are",
          "[sql][api][rebind][txn]") {
  sql_test::TempDir dir{"rebind_binder"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE oc (k BIGINT PRIMARY KEY, b BIGINT)");
  (void)db->execute("CREATE TABLE other (k BIGINT PRIMARY KEY)");
  auto stale = [&](const std::string& ddl, const std::string& sql) {
    auto t = pinned(*db);
    (void)db->execute(ddl);
    auto first = state([&] { (void)t.execute(sql); });
    auto again = db->begin();
    auto retry = state([&] { (void)again.execute(sql); });
    if (retry == "ok") CHECK(again.commit() == CommitOutcome::Committed);
    return std::pair{first, retry};
  };
  // An ON CONFLICT arbiter index created after the pin.
  CHECK(stale("CREATE UNIQUE INDEX oc_b ON oc (b)", "INSERT INTO oc VALUES (1, 1) ON CONFLICT (b) DO NOTHING") ==
        std::pair<std::string, std::string>{"40001", "ok"});
  // An INSERT of the new arity after ADD COLUMN.
  CHECK(stale("ALTER TABLE oc ADD COLUMN c BIGINT", "INSERT INTO oc VALUES (2, 2, 2)") ==
        std::pair<std::string, std::string>{"40001", "ok"});
  // A column only another table gained.
  CHECK(stale("ALTER TABLE other ADD COLUMN z BIGINT", "SELECT z FROM oc") ==
        std::pair<std::string, std::string>{"40001", "42703"});
  // A live pin: a typo is 42P01.
  auto t = pinned(*db);
  CHECK(state([&] { (void)t.execute("SELECT k FROM nosuch"); }) == "42P01");
  CHECK(t.is_aborted());
}

TEST_CASE("rebind: T starts after B; a DDL racing T's first statement re-pins it, past kTxnPlanAttempts it runs",
          "[sql][api][rebind][txn]") {
  sql_test::TempDir dir{"rebind_race"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  const auto& star = db->prepare("SELECT * FROM t");
  {
    // Begun before B, its first statement after: it sees B.
    auto t = db->begin();
    (void)db->execute("ALTER TABLE t ADD COLUMN b BIGINT");
    CHECK(names(t.execute("SELECT * FROM t")) == Names{"k", "a", "b"});
    // A statement held from before B, whose columns changed: 0A000.
    CHECK(state([&] { (void)t.execute(star, {}); }) == "0A000");
  }
  // B creates x right after T's first statement pinned: T re-pins and reads x.
  auto raced = 0;
  db->after_txn_pin_for_testing([&] {
    if (raced++ > 0) return;
    (void)db->execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    (void)db->execute("INSERT INTO x VALUES (3)");
  });
  {
    auto t = db->begin();
    CHECK(t.execute("SELECT k FROM x").rows == Grid{{i(3)}});
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  // A DDL at every pin: past kTxnPlanAttempts re-pins the statement runs at its last pin.
  auto n = 0;
  db->after_txn_pin_for_testing([&] { (void)db->execute("CREATE TABLE r" + std::to_string(n++) + " (k BIGINT PRIMARY KEY)"); });
  {
    auto t = db->begin();
    CHECK(t.execute("SELECT a FROM t WHERE k = $1", {i(1)}).rows == Grid{{i(10)}});
    db->after_txn_pin_for_testing({});
    CHECK(n == 4);
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  db->after_txn_pin_for_testing({});
}

TEST_CASE("rebind: CALL in T runs the procedure as T's catalog has it", "[sql][api][rebind][txn][procedure]") {
  sql_test::TempDir dir{"rebind_call"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE c (id INTEGER PRIMARY KEY, n INTEGER)");
  (void)db->execute("INSERT INTO c VALUES (1, 5)");
  (void)db->execute(R"(CREATE PROCEDURE p(k INTEGER, INOUT seen INTEGER) LANGUAGE plpgsql AS $$
BEGIN
  SELECT n INTO seen FROM c WHERE id = k;
END $$)");
  // A CALL that is T's first statement pins first.
  {
    auto t = db->begin();
    CHECK(t.call("p", {i(1), Value{}}).rows == Grid{{i(5)}});
  }
  // B drops p and creates it with another body, and adds a column to c.
  auto t = pinned(*db);
  (void)db->execute("DROP PROCEDURE p");
  (void)db->execute(R"(CREATE PROCEDURE p(k INTEGER, INOUT seen INTEGER) LANGUAGE plpgsql AS $$
BEGIN
  seen := k + 100;
END $$)");
  (void)db->execute("ALTER TABLE c ADD COLUMN extra INTEGER");
  CHECK(t.call("p", {i(1), Value{}}).rows == Grid{{i(5)}});
  auto compiled = db->compilations();
  CHECK(t.call("p", {i(1), Value{}}).rows == Grid{{i(5)}});
  CHECK(db->compilations() == compiled);
  CHECK(t.procedure("p") != nullptr);
  // A procedure created after the pin: 40001, as is one that exists nowhere,
  // whose retry is 42883.
  (void)db->execute(R"(CREATE PROCEDURE q(INOUT r INTEGER) LANGUAGE plpgsql AS $$
BEGIN
  r := 1;
END $$)");
  CHECK(state([&] { (void)t.call("q", {Value{}}); }) == "40001");
  auto u = pinned(*db);
  (void)db->execute("CREATE TABLE moved (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)u.call("nowhere", {}); }) == "40001");
  auto v = pinned(*db);
  (void)db->execute("CREATE TABLE moved2 (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)v.procedure("nowhere"); }) == "40001");
  auto w = db->begin();
  CHECK(w.procedure("nowhere") == nullptr);
  CHECK(state([&] { (void)w.call("nowhere", {}); }) == "42883");
  CHECK(db->call("p", {i(1), Value{}}).rows == Grid{{i(101)}});
}

TEST_CASE("rebind: the transaction's plan map: one compile per text, charged, kept across ROLLBACK TO, freed",
          "[sql][api][rebind][txn][savepoint]") {
  sql_test::TempDir dir{"rebind_map"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  constexpr auto kText = "SELECT a FROM t WHERE k >= $1";
  {
    auto t = pinned(*db);
    (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");  // the pin is behind now
    t.savepoint("s");
    auto compiled = db->compilations();
    CHECK(t.execute(kText, {i(0)}).rows == Grid{{i(10)}});
    CHECK(db->compilations() == compiled + 1);
    CHECK(t.private_plans_for_testing() == 1);
    const auto charge = t.plan_bytes_for_testing();
    CHECK(charge > 0);
    CHECK(t.execute(kText, {i(0)}).rows == Grid{{i(10)}});  // a second run compiles nothing
    t.rollback_to("s");
    CHECK(t.plan_bytes_for_testing() == charge);  // no mark records it
    CHECK(t.execute(kText, {i(0)}).rows == Grid{{i(10)}});
    CHECK(db->compilations() == compiled + 1);
    // A failed statement with a mark keeps the map; a rollback to it, too.
    CHECK(state([&] { (void)t.execute("SELECT 1 / (a - a) FROM t"); }) == "22012");
    CHECK(t.plan_bytes_for_testing() >= charge);
    t.rollback_to("s");
    CHECK(t.execute(kText, {i(0)}).rows == Grid{{i(10)}});
    CHECK(db->compilations() == compiled + 2);  // the failed statement's own compile
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  // A re-pin drops the plans and their charge.
  auto raced = false;
  db->after_txn_pin_for_testing([&] {
    if (std::exchange(raced, true)) return;
    (void)db->execute("CREATE TABLE elsewhere2 (k BIGINT PRIMARY KEY)");
  });
  {
    auto t = db->begin();
    CHECK(t.execute("SELECT a FROM t WHERE k <= $1", {i(5)}).rows == Grid{{i(10)}});
    db->after_txn_pin_for_testing({});
    CHECK(t.private_plans_for_testing() == 0);
    CHECK(t.plan_bytes_for_testing() == 0);
  }
  // Private plans past max_bytes: 54000.
  {
    auto t = db->begin(TransactionOptions{40u << 10});
    (void)t.execute("SELECT 1");
    (void)db->execute("CREATE TABLE elsewhere3 (k BIGINT PRIMARY KEY)");
    try {
      for (int k = 0; k < 8; ++k) (void)t.execute("SELECT a FROM t WHERE k = " + std::to_string(k));
      FAIL("every plan fits");
    } catch (const ExecError& e) {
      CHECK(sqlstate(e) == "54000");
    }
  }
}

TEST_CASE("rebind: the shared cache serves a transaction only at exactly its pin, and never holds its plans",
          "[sql][api][rebind][txn]") {
  sql_test::TempDir dir{"rebind_shared"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  constexpr auto kText = "SELECT a FROM t WHERE k = $1";
  (void)db->execute(kText, {i(1)});
  // After a GRANT a transaction with a live pin compiles the text privately,
  // until an autocommit run binds the shared plan again.
  (void)db->execute("CREATE ROLE r");
  (void)db->execute("GRANT SELECT ON t TO r");
  auto compiled = db->compilations();
  {
    auto t = db->begin();
    CHECK(t.execute(kText, {i(1)}).rows == Grid{{i(10)}});
    CHECK(t.execute(kText, {i(1)}).rows == Grid{{i(10)}});
    CHECK(t.private_plans_for_testing() == 1);
    CHECK(db->compilations() == compiled + 1);
  }
  (void)db->execute(kText, {i(1)});
  CHECK(db->compilations() == compiled + 2);
  {
    auto t = db->begin();
    CHECK(t.execute(kText, {i(1)}).rows == Grid{{i(10)}});
    CHECK(t.private_plans_for_testing() == 0);
    CHECK(db->compilations() == compiled + 2);
  }
  // A private plan never reaches the shared cache, and a transaction whose
  // pin is behind adds nothing to it.
  auto t = pinned(*db);
  (void)db->execute("ALTER TABLE t ADD COLUMN b BIGINT");
  CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a", "b"});
  const auto cached = db->cached_plans();
  CHECK(names(t.execute("SELECT * FROM t")) == Names{"k", "a"});
  CHECK(names(t.execute("SELECT k FROM t WHERE a = 10")) == Names{"k"});
  CHECK(db->cached_plans() == cached);
  CHECK(names(db->prepare("SELECT * FROM t").columns()) == Names{"k", "a", "b"});
  // A DDL text Described in T and never run leaves T open; a DDL run is
  // 0A000, CREATE TABLE AS too.
  auto record = t.refit("CREATE TABLE nope (k BIGINT PRIMARY KEY)", ResultCheck::none());
  CHECK(record.ddl);
  CHECK(record.command == Command::CreateTable);
  CHECK(t.is_open());
  CHECK_FALSE(t.is_aborted());
  CHECK(state([&] { (void)t.execute("CREATE TABLE nope AS SELECT k FROM t"); }) == "0A000");
}

TEST_CASE("rebind: cursors hold their plan, private or shared", "[sql][api][rebind][txn][cursor]") {
  sql_test::TempDir dir{"rebind_cursor"};
  auto db = Database::open(dir.path(), {.max_memory_bytes = 1ull << 30});
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10), (2, 20)");
  const auto& star = db->prepare("SELECT * FROM t");
  auto t = pinned(*db);
  (void)db->execute("ALTER TABLE t ADD COLUMN b BIGINT");  // B
  (void)db->execute("INSERT INTO t VALUES (3, 30, 3)");
  // A held statement from before B opens with the pin's columns.
  auto c = t.open_cursor(star, {});
  CHECK(names(c.describe()) == Names{"k", "a"});
  CHECK(c.fetch(10).rows.size() == 2);
  // A cursor on a private plan reserves its estimate with its pin, and
  // gives it back when it closes: what is left is the map's charge.
  const auto holds = t.cap_holds_for_testing();
  const auto plans = t.plan_bytes_for_testing();
  auto priv = t.open_cursor("SELECT k FROM t WHERE a > $1", ResultCheck::none(), {i(0)});
  CHECK(t.cap_holds_for_testing() > holds + (t.plan_bytes_for_testing() - plans));
  priv.close();
  CHECK(t.cap_holds_for_testing() == holds + (t.plan_bytes_for_testing() - plans));
  // A WITH HOLD cursor on a private plan reads the pin's rows after COMMIT,
  // and keeps the plan's estimate reserved until it closes.
  auto held = t.open_cursor("SELECT a FROM t WHERE k > $1", ResultCheck::none(), {i(0)}, Holdability::Hold);
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(db->memory_used_for_testing() >= 16u << 10);  // the private plan's estimate, at least
  CHECK(held.fetch(10).rows == Grid{{i(10)}, {i(20)}});
  CHECK(names(held.describe()) == Names{"a"});
  held.close();
  CHECK(db->memory_used_for_testing() == 0);
  // A 54000 from charging the keyed twin's private plan fails the open.
  (void)db->execute("SELECT 1");  // bound again at the live catalog: shared, uncharged
  auto small = db->begin(TransactionOptions{8u << 10});
  (void)small.execute("SELECT 1");
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)small.open_cursor("SELECT k FROM t", ResultCheck::none(), {}); }) == "54000");
  CHECK(small.is_aborted());
}

TEST_CASE("rebind: an allocation failure in a private compile or a rebind leaves the caches as they were",
          "[sql][api][rebind][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"rebind_alloc"};
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  (void)db->execute(R"(CREATE PROCEDURE p(id BIGINT, INOUT seen BIGINT) LANGUAGE plpgsql AS $$
BEGIN
  SELECT a INTO seen FROM t WHERE k = id;
END $$)");
  constexpr auto kText = "SELECT a FROM t WHERE k = $1";
  constexpr auto kScan = "SELECT k, a FROM t";
  // Shared plans of each, and of the procedure, then a DDL: each is behind
  // the live catalog, so a transaction compiles it privately.
  (void)db->execute(kText, {i(1)});
  (void)db->call("p", {i(1), Value{}});
  {
    auto t = db->begin();
    auto c = t.open_cursor(kScan, ResultCheck::none(), {});
    (void)t.commit();
  }
  (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
  struct Op {
    const char* what;
    std::function<void(Transaction&)> run;
  };
  // The arguments are built outside the faults: only the API's allocations are failed.
  const std::vector<Value> one{i(1)};
  const std::vector<Value> args{i(1), Value{}};
  const std::vector<Value> none;
  const auto by_text = ResultCheck::none();
  const std::vector<Op> ops{
      {"a private compile", [&](Transaction& t) { (void)t.execute(kText, one); }},
      {"a private procedure compile", [&](Transaction& t) { (void)t.call("p", args); }},
      {"a cursor's private twin", [&](Transaction& t) { (void)t.open_cursor(kScan, by_text, none); }},
  };
  for (const auto& op : ops) {
    INFO(op.what);
    auto failed = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 200000);
      auto t = db->begin();
      (void)t.execute("SELECT 1");
      t.savepoint("s");
      const auto cached = db->cached_plans();
      std::string outcome = "ok";
      bool fired = false;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        try {
          op.run(t);
        } catch (const std::exception& e) {
          outcome = std::string{sqlstate(e)};
        }
        fired = faults.report().fired;
      }
      INFO("allocation " << nth << ": " << outcome);
      CHECK(db->cached_plans() == cached);
      if (outcome == "ok") {
        CHECK(t.private_plans_for_testing() == 1);
      } else {
        // A failed statement: the transaction aborted, its map holding only
        // what it charged (a plan compiled before a later failure stays, valid).
        CHECK((outcome == "53200" || outcome == "XX000"));
        CHECK(t.is_aborted());
        CHECK(t.private_plans_for_testing() <= 1);
        CHECK((t.private_plans_for_testing() == 0) == (t.plan_bytes_for_testing() == 0));
        t.rollback_to("s");
        op.run(t);  // and it runs once rolled back
        std::string keys;
        for (const auto& k : t.private_plan_keys_for_testing()) keys += "[" + k + "]";
        INFO(keys);
        CHECK(t.private_plans_for_testing() == 1);
      }
      t.rollback();
      if (!fired) break;
      ++failed;
    }
    CHECK(failed > 0);
  }
  // An autocommit rebind that replaces the statement in the cache: the
  // table alternates between two shapes, so each run by text rebinds.
  (void)db->execute("CREATE TABLE r (k BIGINT PRIMARY KEY)");
  (void)db->execute("SELECT * FROM r");
  auto failed = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 200000);
    const auto wide = nth % 2 == 1;
    (void)db->execute("DROP TABLE r");
    (void)db->execute(wide ? "CREATE TABLE r (k BIGINT PRIMARY KEY, v BIGINT)" : "CREATE TABLE r (k BIGINT PRIMARY KEY)");
    std::string outcome = "ok";
    bool fired = false;
    const auto cached = db->cached_plans();
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      try {
        (void)db->execute("SELECT * FROM r");
      } catch (const std::exception& e) {
        outcome = std::string{sqlstate(e)};
      }
      fired = faults.report().fired;
    }
    INFO("allocation " << nth << ": " << outcome);
    CHECK(db->cached_plans() == cached);
    if (outcome != "ok") CHECK((outcome == "53200" || outcome == "XX000"));
    // Whatever failed, the text runs, and returns the live catalog's columns.
    CHECK(db->execute("SELECT * FROM r").columns.size() == (wide ? 2u : 1u));
    if (!fired) break;
    ++failed;
  }
  CHECK(failed > 0);
}
