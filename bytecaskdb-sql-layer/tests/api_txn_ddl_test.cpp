// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Stages 2 and 4 of transactional DDL (docs/transaction_design.md, *Stages of
// transactional DDL*): DDL in an embedded Transaction. Later statements see
// it; ROLLBACK, an abort or a ROLLBACK TO before it undoes it; COMMIT
// publishes it with the transaction's writes, or nothing; other sessions see
// nothing of it until then and never wait; any two DDL commits conflict, and
// the second to commit fails.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

// The message `f` fails with.
template <typename F>
auto message(F&& f) -> std::string {
  try {
    f();
    return "";
  } catch (const std::exception& e) {
    return e.what();
  }
}

using Names = std::vector<std::string>;
using Grid = std::vector<std::vector<Value>>;

auto names(const Rows& r) -> Names { return r.columns; }

// The entries the engine holds under an index id: what a commit wrote of it.
auto index_entries(Database& db, std::uint32_t index_id) -> std::size_t {
  Bytes prefix;
  key_put_prefix(prefix, kNsIndex, index_id);
  auto snap = db.engine_for_testing().snapshot();
  std::size_t n = 0;
  for_each_in_prefix(snap, prefix, [&](BytesView, BytesView) { ++n; });
  return n;
}

auto index_id(Database& db, std::string_view name) -> std::uint32_t {
  const auto [table, ix] = db.catalog()->index({kPublicSchema, std::string{name}});
  REQUIRE(ix != nullptr);
  return ix->id;
}

auto open(const sql_test::TempDir& dir) -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path());
  (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO t VALUES (1, 10)");
  return db;
}

}  // namespace

TEST_CASE("txn DDL: CREATE TABLE, INSERT, CREATE INDEX and SELECT in one transaction; ROLLBACK leaves nothing, COMMIT all",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_life"};
  auto db = open(dir);
  const auto version = db->catalog()->version();
  for (const auto commit : {false, true}) {
    INFO((commit ? "COMMIT" : "ROLLBACK"));
    auto t = db->begin();
    (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY, b BIGINT)");
    (void)t.execute("INSERT INTO x VALUES (1, 10)");
    (void)t.execute("INSERT INTO x VALUES (2, 20)");
    (void)t.execute("CREATE INDEX x_b ON x (b)");
    CHECK(t.execute("SELECT k FROM x WHERE b = 20").rows == Grid{{i(2)}});
    // Another session sees nothing of it, and a text the transaction
    // compiled for its table is 42P01 there, run or prepared.
    CHECK(state([&] { (void)db->execute("SELECT k FROM x WHERE b = 20"); }) == "42P01");
    CHECK(state([&] { (void)db->prepare("SELECT k FROM x WHERE b = 20"); }) == "42P01");
    CHECK(db->catalog()->version() == version);
    if (commit) {
      CHECK(t.commit() == CommitOutcome::Committed);
    } else {
      t.rollback();
    }
  }
  CHECK(db->execute("SELECT k, b FROM x ORDER BY k").rows == Grid{{i(1), i(10)}, {i(2), i(20)}});
  CHECK(db->execute("SELECT k FROM x WHERE b = 20").rows == Grid{{i(2)}});
  CHECK(index_entries(*db, index_id(*db, "x_b")) == 2);
  // The committed catalog's version: the base plus one per DDL of the transaction.
  CHECK(db->catalog()->version() == version + 2);
}

TEST_CASE("txn DDL: DROP VIEW and DROP TABLE rolled back bring both back, with the rows", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_drop"};
  auto db = open(dir);
  (void)db->execute("CREATE VIEW v AS SELECT k, a FROM t");
  {
    auto t = db->begin();
    (void)t.execute("DROP VIEW v");
    (void)t.execute("DROP TABLE t");
    CHECK(state([&] { (void)t.execute("SELECT k FROM t"); }) == "42P01");
    t.rollback();
  }
  CHECK(db->execute("SELECT k, a FROM v").rows == Grid{{i(1), i(10)}});
  CHECK(db->execute("SELECT k, a FROM t").rows == Grid{{i(1), i(10)}});
  // DROP TABLE is RESTRICT: a table a view reads is 2BP01, and aborts.
  auto t = db->begin();
  CHECK(state([&] { (void)t.execute("DROP TABLE t"); }) == "2BP01");
  CHECK(t.is_aborted());
}

TEST_CASE("txn DDL: a backfill reads the transaction's own writes", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_backfill"};
  auto db = open(dir);
  auto t = db->begin();
  (void)t.execute("INSERT INTO t VALUES (2, 20)");
  (void)t.execute("DELETE FROM t WHERE k = 1");
  (void)t.execute("INSERT INTO t VALUES (3, 30)");
  // ADD COLUMN … DEFAULT gives the rows the transaction wrote the default.
  (void)t.execute("ALTER TABLE t ADD COLUMN c BIGINT DEFAULT 7");
  CHECK(t.execute("SELECT k, c FROM t ORDER BY k").rows == Grid{{i(2), i(7)}, {i(3), i(7)}});
  // CREATE INDEX indexes the rows the transaction inserted, and not the one it deleted.
  (void)t.execute("CREATE INDEX t_a ON t (a)");
  CHECK(t.execute("SELECT k FROM t WHERE a = 30").rows == Grid{{i(3)}});
  CHECK(t.execute("SELECT k FROM t WHERE a = 10").rows.empty());
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(index_entries(*db, index_id(*db, "t_a")) == 2);
  CHECK(db->execute("SELECT k, a, c FROM t ORDER BY k").rows == Grid{{i(2), i(20), i(7)}, {i(3), i(30), i(7)}});
}

TEST_CASE("txn DDL: a name dropped and created again in one transaction commits as the new table",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_recreate"};
  auto db = open(dir);
  auto t = db->begin();
  (void)t.execute("DROP TABLE t");
  (void)t.execute("CREATE TABLE t (k BIGINT PRIMARY KEY, z TEXT)");
  (void)t.execute("INSERT INTO t VALUES (9, 'nine')");
  // A cursor over the name reads the new table.
  auto c = t.open_cursor("SELECT * FROM t", ResultCheck::none(), {});
  auto rows = c.fetch(10);
  CHECK(names(rows) == Names{"k", "z"});
  CHECK(rows.rows.size() == 1);
  c.close();
  CHECK(t.commit() == CommitOutcome::Committed);
  auto out = db->execute("SELECT * FROM t");
  CHECK(names(out) == Names{"k", "z"});
  CHECK(out.rows.size() == 1);
  CHECK(out.rows[0][0] == i(9));
}

TEST_CASE("txn DDL: ROLLBACK TO undoes the DDL since the savepoint, RELEASE keeps it", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_savepoint"};
  auto db = open(dir);
  auto t = db->begin();
  (void)t.execute("INSERT INTO t VALUES (2, 20)");
  t.savepoint("s");
  (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
  (void)t.execute("ALTER TABLE t ADD COLUMN b BIGINT");
  (void)t.execute("CREATE INDEX t_a ON t (a)");
  (void)t.execute("INSERT INTO t VALUES (3, 30, 300)");
  t.rollback_to("s");
  // The rows before the savepoint stay; the DDL and the row after it go.
  auto star = t.execute("SELECT * FROM t ORDER BY k");
  CHECK(names(star) == Names{"k", "a"});
  CHECK(star.rows == Grid{{i(1), i(10)}, {i(2), i(20)}});
  CHECK(state([&] { (void)t.execute("SELECT k FROM x"); }) == "42P01");
  t.rollback_to("s");  // the transaction is open again
  // RELEASE keeps it.
  t.savepoint("r");
  (void)t.execute("CREATE TABLE y (k BIGINT PRIMARY KEY)");
  t.release("r");
  (void)t.execute("INSERT INTO y VALUES (1)");
  // A DDL that fails aborts the transaction, and ROLLBACK TO reopens it.
  t.savepoint("d");
  CHECK(state([&] { (void)t.execute("CREATE TABLE y (k BIGINT PRIMARY KEY)"); }) == "42P07");
  CHECK(t.is_aborted());
  t.rollback_to("d");
  // An abort with marks goes back to the newest mark's catalog.
  t.savepoint("a");
  (void)t.execute("CREATE TABLE z1 (k BIGINT PRIMARY KEY)");
  t.savepoint("b");
  (void)t.execute("CREATE TABLE z2 (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)t.execute("SELECT 1 / 0"); }) == "22012");
  t.rollback_to("b");
  CHECK(t.execute("SELECT k FROM z1").rows.empty());
  CHECK(state([&] { (void)t.execute("SELECT k FROM z2"); }) == "42P01");
  t.rollback_to("b");
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(db->catalog()->table({kPublicSchema, "x"}) == nullptr);
  CHECK(db->catalog()->table({kPublicSchema, "y"}) != nullptr);
  CHECK(db->catalog()->table({kPublicSchema, "z1"}) != nullptr);
  CHECK(db->catalog()->table({kPublicSchema, "z2"}) == nullptr);
  CHECK(db->catalog()->index({kPublicSchema, "t_a"}).second == nullptr);
  CHECK(db->execute("SELECT k FROM y").rows == Grid{{i(1)}});
  CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a"});
}

TEST_CASE("txn DDL: an abort that restores the catalog stops index maintenance for the index it undid",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_abort_index"};
  auto db = open(dir);
  // The index the transaction creates takes the next object id.
  const auto ix = static_cast<std::uint32_t>(db->catalog()->next_object_id());
  SECTION("an abort with marks") {
    auto t = db->begin();
    t.savepoint("s");
    (void)t.execute("CREATE INDEX i ON t (a)");
    (void)t.execute("INSERT INTO t VALUES ($1, $2)", {i(2), i(20)});
    CHECK(t.execute("SELECT k FROM t WHERE a = 20").rows == Grid{{i(2)}});
    CHECK(state([&] { (void)t.execute("SELECT 1 / 0"); }) == "22012");
    t.rollback_to("s");
    (void)t.execute("INSERT INTO t VALUES ($1, $2)", {i(2), i(20)});
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  SECTION("ROLLBACK TO, with an INSERT prepared after the CREATE INDEX") {
    auto t = db->begin();
    t.savepoint("s");
    (void)t.execute("CREATE INDEX i ON t (a)");
    const auto& insert = db->prepare("INSERT INTO t VALUES ($1, $2)");
    (void)t.execute(insert, {i(2), i(20)});
    t.rollback_to("s");
    (void)t.execute(insert, {i(2), i(20)});
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  CHECK(db->catalog()->index({kPublicSchema, "i"}).second == nullptr);
  CHECK(index_entries(*db, ix) == 0);
  CHECK(db->execute("SELECT a FROM t WHERE k = 2").rows == Grid{{i(20)}});
}

TEST_CASE("txn DDL: a statement compiled before a CREATE INDEX of the transaction maintains it after",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_maintain"};
  auto db = open(dir);
  const auto& insert = db->prepare("INSERT INTO t VALUES ($1, $2)");
  auto t = db->begin();
  (void)t.execute(insert, {i(2), i(20)});
  (void)t.execute("CREATE INDEX t_a ON t (a)");
  (void)t.execute(insert, {i(3), i(30)});
  CHECK(t.execute("SELECT k FROM t WHERE a = 30").rows == Grid{{i(3)}});
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(index_entries(*db, index_id(*db, "t_a")) == 3);
  // The shared statement never saw the transaction's catalog.
  CHECK(db->execute(insert, {i(4), i(40)}).affected == 1);
  CHECK(index_entries(*db, index_id(*db, "t_a")) == 4);
}

TEST_CASE("txn DDL: after its own DDL a transaction reports its own catalog's binder errors", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_binder"};
  auto db = open(dir);
  (void)db->execute("CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ BEGIN NULL; END $$");
  (void)db->execute("ALTER TABLE t ADD COLUMN c BIGINT");
  {
    auto t = db->begin();
    (void)t.execute("DROP PROCEDURE p");
    CHECK(state([&] { (void)t.call("p", {}); }) == "42883");
  }
  {
    auto t = db->begin();
    (void)t.execute("DROP TABLE t");
    (void)t.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)");
    CHECK(state([&] { (void)t.execute("SELECT c FROM t"); }) == "42703");
  }
}

TEST_CASE("txn DDL: a duplicate name is 42P07 and aborts; a skipped IF NOT EXISTS is no DDL of the transaction",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_dup"};
  auto db = open(dir);
  {
    // Two primary keys is parse analysis's 42P16, in an open transaction as in autocommit.
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute("CREATE TABLE w (a INT PRIMARY KEY, PRIMARY KEY (a))"); }) == "42P16");
    CHECK(state([&] { (void)db->execute("CREATE TABLE w (a INT PRIMARY KEY, PRIMARY KEY (a))"); }) == "42P16");
  }
  {
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)"); }) == "42P07");
    CHECK(t.is_aborted());
    // As PostgreSQL: once aborted, only a syntax error is reported before
    // 25P02; what parse analysis refuses (two primary keys, 42P16) is not.
    CHECK(state([&] { (void)t.execute("CREATE TABLE w (a INT PRIMARY KEY, PRIMARY KEY (a))"); }) == "25P02");
    CHECK(state([&] { (void)t.execute("CREATE TABL w (a INT)"); }) == "42601");
  }
  auto t = db->begin();
  auto out = t.execute("CREATE TABLE IF NOT EXISTS t (k BIGINT PRIMARY KEY)");
  REQUIRE(out.notices.size() == 1);
  CHECK(out.notices[0].sqlstate == "42P07");
  // Another DDL publishes: a transaction that ran none is not doomed.
  (void)db->execute("CREATE TABLE other (k BIGINT PRIMARY KEY)");
  CHECK(t.execute("SELECT a FROM t").rows == Grid{{i(10)}});
  (void)t.execute("INSERT INTO t VALUES (2, 20)");
  CHECK(t.commit() == CommitOutcome::Committed);
}

TEST_CASE("txn DDL: DDL on a follower is 25006 at the statement", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_follower"};
  auto db = open(dir);
  db->engine_for_testing().set_mode(bytecask::Mode::Follower);
  auto t = db->begin();
  (void)t.execute("SELECT 1");
  CHECK(message([&] { (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)"); }) ==
        "cannot execute CREATE TABLE in a read-only transaction");
  CHECK(t.is_aborted());
  auto u = db->begin();
  CHECK(state([&] { (void)u.execute("DROP TABLE t"); }) == "25006");
  db->engine_for_testing().set_mode(bytecask::Mode::Leader);
}

TEST_CASE("txn DDL: DDL on what an open cursor of the transaction reads is 55006", "[sql][api][txn][ddl][cursor]") {
  sql_test::TempDir dir{"txn_ddl_in_use"};
  auto db = open(dir);
  (void)db->execute("CREATE INDEX t_a ON t (a)");
  (void)db->execute("CREATE TABLE u (k BIGINT PRIMARY KEY)");
  constexpr auto kInUse = " because it is being used by active queries in this session";
  struct Case {
    const char* ddl;
    std::string message;
  };
  for (const auto& c : std::vector<Case>{{"DROP TABLE t", "cannot DROP TABLE \"t\""},
                                         {"CREATE INDEX t_k ON t (k)", "cannot CREATE INDEX \"t\""},
                                         {"ALTER TABLE t ADD COLUMN b BIGINT", "cannot ALTER TABLE \"t\""}}) {
    INFO(c.ddl);
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM t", ResultCheck::none(), {});
    CHECK(cursor.fetch(10).rows.size() == 1);  // fetched to the end: still open
    CHECK(state([&] { (void)t.execute(c.ddl); }) == "55006");
    CHECK(message([&] {
            auto u = db->begin();
            auto cur = u.open_cursor("SELECT k FROM t", ResultCheck::none(), {});
            (void)u.execute(c.ddl);
          }) == c.message + kInUse);
    CHECK(t.is_aborted());
  }
  {
    // As PostgreSQL checks dependencies first: DROP TABLE of a table a view
    // depends on is 2BP01, not 55006, while a cursor reads the table.
    (void)db->execute("CREATE VIEW tv AS SELECT k FROM t");
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM t", ResultCheck::none(), {});
    CHECK(state([&] { (void)t.execute("DROP TABLE t"); }) == "2BP01");
    CHECK(t.is_aborted());
    t.rollback();
    (void)db->execute("DROP VIEW tv");
  }
  {
    // None for an unrelated table, for CREATE VIEW, or for DROP INDEX of an
    // index the cursor does not scan.
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k, a FROM t", ResultCheck::none(), {});
    (void)t.execute("DROP TABLE u");
    (void)t.execute("CREATE VIEW v AS SELECT k FROM t");
    (void)t.execute("DROP INDEX t_a");
    CHECK(cursor.fetch(10).rows.size() == 1);
    t.rollback();
  }
  {
    // 55006 for DROP INDEX of one it scans: on a table large enough that
    // the plan reads the index.
    for (std::int64_t k = 2; k <= 500; ++k) (void)db->execute("INSERT INTO t VALUES ($1, $1)", {i(k)});
    REQUIRE(db->prepare("SELECT k FROM t WHERE a = $1").plan_source().find("IDX_") != std::string::npos);
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM t WHERE a = $1", ResultCheck::none(), {i(10)});
    CHECK(message([&] { (void)t.execute("DROP INDEX t_a"); }) == std::string{"cannot DROP INDEX \"t_a\""} + kInUse);
  }
  {
    // A WITH HOLD cursor of an earlier transaction is not counted: it reads its own view.
    auto t = db->begin();
    auto held = t.open_cursor("SELECT k FROM t WHERE k = 1", ResultCheck::none(), {}, Holdability::Hold);
    CHECK(t.commit() == CommitOutcome::Committed);
    auto u = db->begin();
    (void)u.execute("DROP TABLE t");
    CHECK(u.commit() == CommitOutcome::Committed);
    CHECK(held.fetch(10).rows == Grid{{i(1)}});
  }
}

TEST_CASE("txn DDL: a portal bound in the transaction counts for 55006 while it is held",
          "[sql][api][txn][ddl][server]") {
  // The server's Bind (Transaction::bind_portal): PostgreSQL's PortalStart
  // opens what the portal reads, and a DDL on it is 55006 until the portal goes.
  sql_test::TempDir dir{"txn_ddl_portal"};
  auto db = open(dir);
  (void)db->execute("CREATE TABLE u (k BIGINT PRIMARY KEY)");
  auto t = db->begin();
  auto [record, reads] = t.bind_portal("SELECT k, a FROM t", ResultCheck::none());
  REQUIRE(reads != nullptr);
  CHECK(record.columns.size() == 2);
  auto [ddl_record, ddl_reads] = t.bind_portal("DROP TABLE u", ResultCheck::none());
  CHECK(ddl_record.ddl);
  CHECK(ddl_reads == nullptr);  // a DDL text reads nothing until it runs
  (void)t.execute("DROP TABLE u");  // a table no portal reads
  CHECK(message([&] { (void)t.execute("ALTER TABLE t ADD COLUMN b BIGINT"); }) ==
        "cannot ALTER TABLE \"t\" because it is being used by active queries in this session");
  CHECK(t.is_aborted());
  t.rollback();
  auto v = db->begin();
  {
    auto held = v.bind_portal("SELECT k FROM t", ResultCheck::none()).second;
    CHECK(state([&] { (void)v.execute("CREATE INDEX t_k ON t (k)"); }) == "55006");
  }
  v.rollback();
  auto w = db->begin();
  (void)w.bind_portal("SELECT k FROM t", ResultCheck::none());  // dropped at once: holds nothing
  (void)w.execute("CREATE INDEX t_k ON t (k)");
  CHECK(w.commit() == CommitOutcome::Committed);
}

TEST_CASE("txn DDL: a WITH HOLD cursor over a table the transaction created reads it after COMMIT",
          "[sql][api][txn][ddl][cursor]") {
  sql_test::TempDir dir{"txn_ddl_hold"};
  auto db = open(dir);
  auto t = db->begin();
  (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY, b TEXT)");
  (void)t.execute("INSERT INTO x VALUES (1, 'one')");
  auto held = t.open_cursor("SELECT k FROM x", ResultCheck::none(), {}, Holdability::Hold);
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(held.fetch(10).rows == Grid{{i(1)}});
  held.close();
}

TEST_CASE("txn DDL: SELECT * by text returns the transaction's columns; a held one whose columns change is 0A000",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_shape"};
  auto db = open(dir);
  const auto& star = db->prepare("SELECT * FROM t");
  auto t = db->begin();
  CHECK(names(t.execute("SELECT * FROM t")) == Names{"k", "a"});
  (void)t.execute("ALTER TABLE t ADD COLUMN b BIGINT DEFAULT 5");
  CHECK(t.execute("SELECT * FROM t").rows == Grid{{i(1), i(10), i(5)}});
  // The shared statement is unchanged for everyone else.
  CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a"});
  CHECK(state([&] { (void)t.execute(star, {}); }) == "0A000");
  CHECK(t.is_aborted());
  CHECK(t.commit() == CommitOutcome::RolledBack);
  // Committed, every session sees it.
  auto u = db->begin();
  (void)u.execute("ALTER TABLE t ADD COLUMN b BIGINT DEFAULT 5");
  CHECK(u.commit() == CommitOutcome::Committed);
  CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a", "b"});
}

TEST_CASE("txn DDL: a TxnStatement resolves its text where it runs", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_txnstatement"};
  auto db = open(dir);
  const auto cached = db->cached_plans();
  SECTION("over a table its transaction created and rolled back: 42P01") {
    auto t = db->begin();
    (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    auto s = t.prepare("SELECT k FROM x");
    CHECK(s.record().private_catalog);
    CHECK(names(t.execute(s.sql(), s.check(), {})) == Names{"k"});
    t.rollback();
    CHECK(state([&] { (void)db->execute(s.sql(), s.check(), {}); }) == "42P01");
  }
  SECTION("fixed at a pin behind another session's ADD COLUMN, run after COMMIT in autocommit: 0A000") {
    auto t = db->begin();
    (void)t.execute("SELECT 1");
    auto s = t.prepare("SELECT * FROM t");
    CHECK(db->cached_plans() == cached + 1);  // SELECT 1's; prepare puts nothing in the cache
    CHECK_FALSE(s.record().private_catalog);
    (void)db->execute("ALTER TABLE t ADD COLUMN b BIGINT");
    CHECK(names(t.execute(s.sql(), s.check(), {})) == Names{"k", "a"});
    CHECK(t.commit() == CommitOutcome::Committed);
    CHECK(state([&] { (void)db->execute(s.sql(), s.check(), {}); }) == "0A000");
  }
  SECTION("fixed after another transaction's ADD COLUMN, run in a transaction at that base") {
    auto t1 = db->begin();
    (void)t1.execute("SELECT 1");
    auto t2 = db->begin();
    (void)t2.execute("ALTER TABLE t ADD COLUMN b BIGINT");
    auto s = t2.prepare("SELECT * FROM t");
    CHECK(s.record().private_catalog);
    CHECK(s.record().columns.size() == 3);
    SECTION("committed: 40001, and its retry returns the new column") {
      CHECK(t2.commit() == CommitOutcome::Committed);
      CHECK(state([&] { (void)t1.execute(s.sql(), s.check(), {}); }) == "40001");
      CHECK_FALSE(t1.is_open());
      auto retry = db->begin();
      CHECK(names(retry.execute(s.sql(), s.check(), {})) == Names{"k", "a", "b"});
    }
    SECTION("rolled back: 0A000 at that base, the live catalog still there") {
      t2.rollback();
      CHECK(state([&] { (void)t1.execute(s.sql(), s.check(), {}); }) == "0A000");
    }
  }
}

TEST_CASE("txn DDL: the second of two DDL commits fails, and the loser writes nothing", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_conflicts"};
  auto db = open(dir);
  const auto rows = [&] { return db->execute("SELECT k FROM t ORDER BY k").rows; };
  SECTION("T1 runs DDL that depends on t's rows; T2 writes t and commits first: T1 is 40001") {
    for (const auto* ddl : {"CREATE INDEX t_a ON t (a)", "ALTER TABLE t ADD COLUMN b BIGINT DEFAULT 1", "DROP TABLE t"}) {
      INFO(ddl);
      auto t1 = db->begin();
      (void)t1.execute(ddl);
      (void)db->execute("INSERT INTO t VALUES (100, 1)");
      CHECK(state([&] { (void)t1.commit(); }) == "40001");
      CHECK(db->catalog()->index({kPublicSchema, "t_a"}).second == nullptr);
      CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a"});
      (void)db->execute("DELETE FROM t WHERE k = 100");
    }
    // DROP INDEX against a concurrent insert, whose entry it would range-delete.
    (void)db->execute("CREATE INDEX t_a ON t (a)");
    auto t1 = db->begin();
    (void)t1.execute("DROP INDEX t_a");
    (void)db->execute("INSERT INTO t VALUES (100, 1)");
    CHECK(state([&] { (void)t1.commit(); }) == "40001");
    CHECK(db->catalog()->index({kPublicSchema, "t_a"}).second != nullptr);
    CHECK(index_entries(*db, index_id(*db, "t_a")) == 2);
  }
  SECTION("ADD COLUMN with no default and CREATE VIEW do not depend on the rows: T1 commits") {
    auto t1 = db->begin();
    (void)t1.execute("ALTER TABLE t ADD COLUMN b BIGINT");
    (void)t1.execute("CREATE VIEW v AS SELECT k FROM t");
    (void)db->execute("INSERT INTO t VALUES (2, 20)");
    CHECK(t1.commit() == CommitOutcome::Committed);
    CHECK(db->execute("SELECT k, b FROM t ORDER BY k").rows == Grid{{i(1), Value{}}, {i(2), Value{}}});
    CHECK(db->execute("SELECT k FROM v ORDER BY k").rows == Grid{{i(1)}, {i(2)}});
  }
  SECTION("T1 commits DDL on t; T2 wrote t from an older pin: T2 is 40001; one that only read t commits") {
    auto wrote = db->begin();
    auto read = db->begin();
    (void)wrote.execute("INSERT INTO t VALUES (2, 20)");
    CHECK(read.execute("SELECT a FROM t").rows == Grid{{i(10)}});
    auto t1 = db->begin();
    (void)t1.execute("CREATE INDEX t_a ON t (a)");
    CHECK(t1.commit() == CommitOutcome::Committed);
    CHECK(state([&] { (void)wrote.commit(); }) == "40001");
    CHECK(read.commit() == CommitOutcome::Committed);
    CHECK(rows() == Grid{{i(1)}});
  }
  SECTION("T1 and T2 both create x: the first to commit wins; the other is 40001, and its retry 42P07") {
    auto t1 = db->begin();
    auto t2 = db->begin();
    (void)t1.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    (void)t2.execute("CREATE TABLE x (k BIGINT PRIMARY KEY, other TEXT)");
    (void)t1.execute("INSERT INTO x VALUES (1)");
    CHECK(t1.commit() == CommitOutcome::Committed);
    // At its next statement…
    CHECK(state([&] { (void)t2.execute("SELECT 1"); }) == "40001");
    CHECK(t2.is_aborted());
    // …or at its commit.
    auto t3 = db->begin();
    (void)t3.execute("SELECT 1");
    auto t4 = db->begin();
    (void)t4.execute("CREATE TABLE y (k BIGINT PRIMARY KEY)");
    (void)db->execute("CREATE TABLE y (k BIGINT PRIMARY KEY, other TEXT)");
    CHECK(state([&] { (void)t4.commit(); }) == "40001");
    CHECK(names(db->execute("SELECT * FROM y")) == Names{"k", "other"});
    auto retry = db->begin();
    CHECK(state([&] { (void)retry.execute("CREATE TABLE x (k BIGINT PRIMARY KEY, other TEXT)"); }) == "42P07");
    CHECK(names(db->execute("SELECT * FROM x")) == Names{"k"});
  }
  SECTION("any two DDL commits conflict, GRANT and CREATE ROLE included") {
    auto t1 = db->begin();
    (void)t1.execute("CREATE ROLE reader");
    (void)t1.execute("GRANT SELECT ON t TO reader");
    (void)db->execute("CREATE TABLE elsewhere (k BIGINT PRIMARY KEY)");
    CHECK(state([&] { (void)t1.commit(); }) == "40001");
    CHECK(db->catalog()->role("reader") == nullptr);
  }
  SECTION("an autocommit DDL in flight at T1's commit: 40001 at once") {
    auto t1 = db->begin();
    (void)t1.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    std::string outcome;
    db->live_catalog_for_testing().set_commit_hook_for_testing([&] {
      std::thread other{[&] { outcome = state([&] { (void)t1.commit(); }); }};
      other.join();
    });
    (void)db->execute("CREATE TABLE other (k BIGINT PRIMARY KEY)");
    db->live_catalog_for_testing().set_commit_hook_for_testing({});
    CHECK(outcome == "40001");
    CHECK(db->catalog()->table({kPublicSchema, "x"}) == nullptr);
  }
}

TEST_CASE("txn DDL: a transaction that ran DDL is 40001 at its next statement once another DDL commits",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_doomed"};
  auto db = open(dir);
  auto t = db->begin();
  (void)t.execute("INSERT INTO t VALUES (2, 20)");
  t.savepoint("before");
  (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
  t.savepoint("after");
  (void)db->execute("CREATE TABLE unrelated (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)t.execute("SELECT 1"); }) == "40001");
  CHECK(t.is_aborted());
  CHECK(t.is_open());
  // A mark after its first DDL leaves it doomed.
  t.rollback_to("after");
  CHECK(state([&] { (void)t.execute("SELECT 1"); }) == "40001");
  // A mark before it lifts the doom: nothing left to conflict.
  t.rollback_to("before");
  CHECK(t.execute("SELECT k FROM t ORDER BY k").rows == Grid{{i(1)}, {i(2)}});
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(db->catalog()->table({kPublicSchema, "x"}) == nullptr);
  CHECK(db->execute("SELECT k FROM t ORDER BY k").rows == Grid{{i(1)}, {i(2)}});
  // Without a savepoint, the doomed 40001 aborts it, and COMMIT rolls it back.
  auto u = db->begin();
  (void)u.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE TABLE unrelated2 (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { u.savepoint("s"); }) == "40001");
  CHECK(u.commit() == CommitOutcome::RolledBack);
}

TEST_CASE("txn DDL: a first statement that is a DDL re-pins; a later DDL at a stale pin is 40001",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_repin"};
  auto db = open(dir);
  auto raced = 0;
  db->after_txn_pin_for_testing([&] {
    if (raced++ > 0) return;
    (void)db->execute("CREATE TABLE other (k BIGINT PRIMARY KEY)");
  });
  {
    auto t = db->begin();
    (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    CHECK(raced == 2);
    CHECK(t.commit() == CommitOutcome::Committed);
    CHECK(db->catalog()->table({kPublicSchema, "x"}) != nullptr);
  }
  db->after_txn_pin_for_testing({});
  auto t = db->begin();
  (void)t.execute("SELECT 1");
  (void)db->execute("CREATE TABLE another (k BIGINT PRIMARY KEY)");
  CHECK(state([&] { (void)t.execute("CREATE TABLE y (k BIGINT PRIMARY KEY)"); }) == "40001");
  CHECK_FALSE(t.is_open());  // it had run no DDL: the 40001 ends it
}

TEST_CASE("txn DDL: a conflict with a DDL published since is 40001; with none, a taken key is 23505",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_classify"};
  auto db = open(dir);
  {
    // Two transactions take the same private id, and insert the same key.
    auto t1 = db->begin();
    auto t2 = db->begin();
    (void)t1.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    (void)t2.execute("CREATE TABLE y (k BIGINT PRIMARY KEY)");
    (void)t1.execute("INSERT INTO x VALUES (1)");
    (void)t2.execute("INSERT INTO y VALUES (1)");
    CHECK(t1.commit() == CommitOutcome::Committed);
    CHECK(state([&] { (void)t2.commit(); }) == "40001");
    CHECK(db->catalog()->table({kPublicSchema, "y"}) == nullptr);
  }
  {
    // No DDL published since: the key a concurrent insert took is a duplicate.
    auto t = db->begin();
    (void)t.execute("CREATE TABLE z (k BIGINT PRIMARY KEY)");
    (void)t.execute("INSERT INTO t VALUES (5, 50)");
    (void)db->execute("INSERT INTO t VALUES (5, 55)");
    CHECK(state([&] { (void)t.commit(); }) == "23505");
    CHECK(db->catalog()->table({kPublicSchema, "z"}) == nullptr);
    CHECK(db->execute("SELECT a FROM t WHERE k = 5").rows == Grid{{i(55)}});
  }
}

TEST_CASE("txn DDL: tables with hidden row ids under one private id never share a row id", "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_row_ids"};
  auto db = open(dir);
  auto t1 = db->begin();
  auto t2 = db->begin();
  (void)t1.execute("CREATE TABLE h (v BIGINT)");
  (void)t2.execute("CREATE TABLE h2 (v BIGINT)");
  (void)t1.execute("INSERT INTO h VALUES (1)");
  (void)t2.execute("INSERT INTO h2 VALUES (2)");
  CHECK(t2.commit() == CommitOutcome::Committed);
  CHECK(state([&] { (void)t1.commit(); }) == "40001");
  // A later table that takes the rolled-back id draws unique row ids.
  auto t3 = db->begin();
  (void)t3.execute("CREATE TABLE h3 (v BIGINT)");
  (void)t3.execute("INSERT INTO h3 VALUES (3)");
  (void)t3.execute("INSERT INTO h3 VALUES (4)");
  CHECK(t3.commit() == CommitOutcome::Committed);
  CHECK(db->execute("SELECT v FROM h3 ORDER BY v").rows == Grid{{i(3)}, {i(4)}});
  CHECK(db->execute("SELECT v FROM h2").rows == Grid{{i(2)}});
  // A table under an id a rolled-back transaction drew row ids for: the
  // next transaction takes the same id, and the counter goes on.
  {
    auto t4 = db->begin();
    (void)t4.execute("CREATE TABLE h4 (v BIGINT)");
    (void)t4.execute("INSERT INTO h4 VALUES (5)");
    t4.rollback();
  }
  auto t5 = db->begin();
  (void)t5.execute("CREATE TABLE h5 (v BIGINT)");
  (void)t5.execute("INSERT INTO h5 VALUES (6)");
  (void)t5.execute("INSERT INTO h5 VALUES (7)");
  CHECK(t5.commit() == CommitOutcome::Committed);
  CHECK(db->execute("SELECT v FROM h5 ORDER BY v").rows == Grid{{i(6)}, {i(7)}});
}

TEST_CASE("txn DDL: max_bytes bounds a transaction's DDL; Catalog::estimated_bytes counts by its constants",
          "[sql][api][txn][ddl]") {
  sql_test::TempDir dir{"txn_ddl_bound"};
  auto db = open(dir);
  for (std::int64_t k = 2; k <= 200; ++k) (void)db->execute("INSERT INTO t VALUES ($1, $1)", {i(k)});
  {
    // A backfill past max_bytes.
    auto t = db->begin(TransactionOptions{4u << 10});
    CHECK(message([&] { (void)t.execute("CREATE INDEX t_a ON t (a)"); }).ends_with("run the DDL outside a transaction"));
    CHECK(t.is_aborted());
  }
  {
    // A max_bytes above the engine's batch limit: the batch limit bounds it (the seam stands for 1 GiB).
    db->live_catalog_for_testing().set_backfill_limit_for_testing(1024);
    auto t = db->begin(TransactionOptions{std::uint64_t{4} << 30});
    CHECK(state([&] { (void)t.execute("ALTER TABLE t ADD COLUMN c BIGINT DEFAULT 1"); }) == "54000");
    db->live_catalog_for_testing().set_backfill_limit_for_testing(LiveCatalog::kMaxBackfillBytes);
  }
  {
    // Many DDL statements, each behind a SAVEPOINT: each mark keeps a catalog.
    auto t = db->begin(TransactionOptions{256u << 10});
    std::string outcome = "ok";
    for (int n = 0; n < 200 && outcome == "ok"; ++n) {
      t.savepoint("s" + std::to_string(n));
      outcome = state([&] { (void)t.execute("CREATE TABLE many" + std::to_string(n) + " (k BIGINT PRIMARY KEY)"); });
    }
    CHECK(outcome == "54000");
  }
  // The estimate: the base, then each table, column, index and object's constant and its names.
  auto cat = db->catalog();
  const auto* table = cat->table({kPublicSchema, "t"});
  REQUIRE(table != nullptr);
  std::uint64_t expected = Catalog::kEstimateBase + Catalog::kEstimateTable + table->name.size();
  for (const auto& c : table->columns) expected += Catalog::kEstimateColumn + c.name.size();
  CHECK(cat->estimated_bytes() == expected);
  (void)db->execute("CREATE INDEX t_a ON t (a)");
  CHECK(db->catalog()->estimated_bytes() == expected + Catalog::kEstimateIndex + 3);
}

TEST_CASE("txn DDL: a commit that may have reached the engine is 08007; reopened, catalog and rows agree",
          "[sql][api][txn][ddl][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  auto unknowns = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 200000);
    sql_test::TempDir dir{"txn_ddl_unknown"};
    std::string outcome;
    auto fired = false;
    {
      auto db = open(dir);
      auto t = db->begin();
      (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY, b BIGINT)");
      (void)t.execute("INSERT INTO x VALUES (1, 10)");
      (void)t.execute("CREATE INDEX x_b ON x (b)");
      (void)t.execute("INSERT INTO t VALUES (2, 20)");
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        outcome = state([&] { (void)t.commit(); });
        fired = faults.report().fired;
      }
      INFO("allocation " << nth << ": " << outcome);
      CHECK(db->engine_for_testing().is_degraded() == (outcome == "08007"));
      if (outcome == "08007") ++unknowns;
    }
    // Both or neither: the table, its index and rows, and the other table's row.
    auto db = Database::open(dir.path());
    const auto has = db->catalog()->table({kPublicSchema, "x"}) != nullptr;
    if (outcome == "ok") CHECK(has);
    if (outcome != "ok" && outcome != "08007") CHECK_FALSE(has);
    CHECK(db->execute("SELECT k FROM t WHERE k = 2").rows.size() == (has ? 1u : 0u));
    if (has) {
      CHECK(db->execute("SELECT k FROM x WHERE b = 10").rows == Grid{{i(1)}});
      CHECK(index_entries(*db, index_id(*db, "x_b")) == 1);
    }
    if (!fired) break;
  }
  CHECK(unknowns > 0);
}

TEST_CASE("txn DDL: reopened after a commit held at the engine, applied or failed, catalog and rows agree",
          "[sql][api][txn][ddl]") {
  for (const auto fail : {false, true}) {
    INFO((fail ? "failed" : "applied"));
    sql_test::TempDir dir{"txn_ddl_hook"};
    {
      auto db = open(dir);
      auto t = db->begin();
      (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
      (void)t.execute("INSERT INTO x VALUES (1)");
      (void)t.execute("INSERT INTO t VALUES (2, 20)");
      std::string seen;
      db->live_catalog_for_testing().set_commit_hook_for_testing([&] {
        // Mid-commit: no other session sees the table yet.
        std::thread reader{[&] { seen = db->catalog()->table({kPublicSchema, "x"}) == nullptr ? "absent" : "present"; }};
        reader.join();
        if (fail) throw std::runtime_error{"crash"};
      });
      const auto outcome = state([&] { (void)t.commit(); });
      CHECK((outcome == "ok") == !fail);
      db->live_catalog_for_testing().set_commit_hook_for_testing({});
      CHECK(seen == "absent");
    }
    auto db = Database::open(dir.path());
    CHECK((db->catalog()->table({kPublicSchema, "x"}) != nullptr) == !fail);
    CHECK(db->execute("SELECT k FROM t WHERE k = 2").rows.size() == (fail ? 0u : 1u));
    if (!fail) CHECK(db->execute("SELECT k FROM x").rows == Grid{{i(1)}});
  }
}

TEST_CASE("txn DDL: an allocation failure in a DDL step, a compile after it, ROLLBACK TO or the commit is a failed "
          "statement",
          "[sql][api][txn][ddl][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"txn_ddl_alloc"};
  auto db = open(dir);
  for (std::int64_t k = 2; k <= 20; ++k) (void)db->execute("INSERT INTO t VALUES ($1, $1)", {i(k)});
  struct Op {
    const char* what;
    std::function<void(Transaction&)> setup;
    std::function<void(Transaction&)> run;
  };
  const std::vector<Value> one{i(1)};
  const std::vector<Op> ops{
      {"CREATE INDEX (the clone, the overlay writes, the backfill, the charge)", [](Transaction&) {},
       [](Transaction& t) { (void)t.execute("CREATE INDEX t_a ON t (a)"); }},
      {"ADD COLUMN … DEFAULT", [](Transaction&) {},
       [](Transaction& t) { (void)t.execute("ALTER TABLE t ADD COLUMN c BIGINT DEFAULT 3"); }},
      {"a private compile after a DDL",
       [](Transaction& t) { (void)t.execute("CREATE INDEX t_a ON t (a)"); },
       [&](Transaction& t) { (void)t.execute("SELECT k FROM t WHERE a = $1", one); }},
  };
  for (const auto& op : ops) {
    INFO(op.what);
    auto failed = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 200000);
      auto t = db->begin();
      (void)t.execute("SELECT 1");
      t.savepoint("s");
      op.setup(t);
      t.savepoint("m");
      const auto holds = t.cap_holds_for_testing();
      std::string outcome = "ok";
      bool fired = false;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        outcome = state([&] { op.run(t); });
        fired = faults.report().fired;
      }
      INFO("allocation " << nth << ": " << outcome);
      if (outcome != "ok") {
        CHECK((outcome == "53200" || outcome == "XX000"));
        CHECK(t.is_aborted());
        // ROLLBACK TO restores the mark's catalog and charge, and the step runs.
        t.rollback_to("m");
        CHECK(t.cap_holds_for_testing() - t.plan_bytes_for_testing() <= holds);
        op.run(t);
      }
      CHECK(t.commit() == CommitOutcome::Committed);
      // Undo what the run committed, for the next allocation.
      if (db->catalog()->index({kPublicSchema, "t_a"}).second != nullptr) (void)db->execute("DROP INDEX t_a");
      if (db->catalog()->table({kPublicSchema, "t"})->column("c") != nullptr) {
        (void)db->execute("DROP TABLE t");
        (void)db->execute("CREATE TABLE t (k BIGINT PRIMARY KEY, a BIGINT)");
        for (std::int64_t k = 1; k <= 20; ++k) (void)db->execute("INSERT INTO t VALUES ($1, $1)", {i(k)});
      }
      if (!fired) break;
      ++failed;
    }
    CHECK(failed > 0);
  }
  // ROLLBACK TO across a DDL, and the commit of a DDL transaction.
  for (const auto commit : {false, true}) {
    INFO((commit ? "the commit" : "ROLLBACK TO"));
    auto failed = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 200000);
      auto t = db->begin();
      t.savepoint("s");
      (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
      (void)t.execute("INSERT INTO x VALUES (1)");
      std::string outcome = "ok";
      bool fired = false;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        outcome = state([&] {
          if (commit) {
            (void)t.commit();
          } else {
            t.rollback_to("s");
          }
        });
        fired = faults.report().fired;
      }
      INFO("allocation " << nth << ": " << outcome);
      if (commit) {
        // Committed whole or not at all.
        const auto has = db->catalog()->table({kPublicSchema, "x"}) != nullptr;
        CHECK(has == (outcome == "ok"));
        if (has) {
          CHECK(db->execute("SELECT k FROM x").rows == Grid{{i(1)}});
          (void)db->execute("DROP TABLE x");
        }
      } else {
        CHECK(outcome == "ok");  // a rollback to a mark allocates nothing
        CHECK(state([&] { (void)t.execute("SELECT k FROM x"); }) == "42P01");
        t.rollback();
      }
      if (!fired) break;
      ++failed;
    }
    if (commit) CHECK(failed > 0);
  }
}

// Stage 4: CREATE TABLE AS in a transaction.

TEST_CASE("txn DDL: CREATE TABLE AS reads the transaction's view; ROLLBACK leaves nothing, COMMIT the table and rows",
          "[sql][api][txn][ddl][ctas]") {
  sql_test::TempDir dir{"txn_ddl_ctas"};
  auto db = open(dir);
  const auto version = db->catalog()->version();
  for (const auto commit : {false, true}) {
    INFO((commit ? "COMMIT" : "ROLLBACK"));
    auto t = db->begin();
    (void)t.execute("INSERT INTO t VALUES (2, 20)");
    (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY, b BIGINT)");
    (void)t.execute("INSERT INTO x VALUES (3, 30)");
    // Its query reads the transaction's own rows and its own table.
    auto out = t.execute("CREATE TABLE c AS SELECT k, a FROM t UNION ALL SELECT k, b FROM x");
    CHECK(out.affected == 3);
    CHECK(t.execute("SELECT k, a FROM c ORDER BY k").rows == Grid{{i(1), i(10)}, {i(2), i(20)}, {i(3), i(30)}});
    (void)t.execute("SELECT k * 2 AS d INTO s FROM c WHERE k > 1");
    CHECK(t.execute("SELECT d FROM s ORDER BY d").rows == Grid{{i(4)}, {i(6)}});
    out = t.execute("CREATE TABLE e AS SELECT k FROM c WITH NO DATA");
    CHECK(out.affected == -1);
    CHECK(t.execute("SELECT k FROM e").rows.empty());
    // Another session sees nothing of it.
    CHECK(state([&] { (void)db->execute("SELECT k FROM c"); }) == "42P01");
    CHECK(db->catalog()->version() == version);
    if (commit) {
      CHECK(t.commit() == CommitOutcome::Committed);
    } else {
      t.rollback();
      CHECK(db->catalog()->table({kPublicSchema, "c"}) == nullptr);
      CHECK(db->catalog()->table({kPublicSchema, "s"}) == nullptr);
    }
  }
  CHECK(db->execute("SELECT k, a FROM c ORDER BY k").rows == Grid{{i(1), i(10)}, {i(2), i(20)}, {i(3), i(30)}});
  CHECK(db->execute("SELECT d FROM s ORDER BY d").rows == Grid{{i(4)}, {i(6)}});
  CHECK(db->execute("SELECT k FROM e").rows.empty());
  // A table, a CREATE TABLE AS, a SELECT … INTO and a WITH NO DATA: four DDL.
  CHECK(db->catalog()->version() == version + 4);
}

TEST_CASE("txn DDL: CREATE TABLE AS of a taken name is 42P07, or skipped with IF NOT EXISTS, before its query runs",
          "[sql][api][txn][ddl][ctas]") {
  sql_test::TempDir dir{"txn_ddl_ctas_dup"};
  auto db = open(dir);
  {
    auto t = db->begin();
    (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    // The name is looked for first, as PostgreSQL does: the query's 1/0 never runs.
    CHECK(state([&] { (void)t.execute("CREATE TABLE x AS SELECT 1/0 AS v"); }) == "42P07");
    CHECK(t.is_aborted());
  }
  {
    auto t = db->begin();
    (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
    auto out = t.execute("CREATE TABLE IF NOT EXISTS x AS SELECT 1/0 AS v");
    REQUIRE(out.notices.size() == 1);
    CHECK(out.notices[0].sqlstate == "42P07");
    CHECK(state([&] { (void)t.execute("CREATE TABLE y AS SELECT 1/0 AS v"); }) == "22012");
    CHECK(t.is_aborted());
  }
  {
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute("CREATE TABLE y AS SELECT 1/0 AS v WITH NO DATA"); }) == "0A000");
  }
  {
    // A skipped IF NOT EXISTS is no DDL of the transaction: another DDL does not doom it.
    auto t = db->begin();
    (void)t.execute("CREATE TABLE IF NOT EXISTS t AS SELECT 1 AS v");
    (void)db->execute("CREATE TABLE other (k BIGINT PRIMARY KEY)");
    CHECK(t.execute("SELECT a FROM t").rows == Grid{{i(10)}});
    CHECK(t.commit() == CommitOutcome::Committed);
  }
}

TEST_CASE("txn DDL: CREATE TABLE AS takes parameters, prepared in the transaction or held",
          "[sql][api][txn][ddl][ctas]") {
  sql_test::TempDir dir{"txn_ddl_ctas_params"};
  auto db = open(dir);
  auto t = db->begin();
  (void)t.execute("CREATE TABLE x (k BIGINT PRIMARY KEY)");
  (void)t.execute("INSERT INTO x VALUES (5)");
  // Prepared against the transaction's catalog: its query names its own table.
  auto p = t.prepare("CREATE TABLE c AS SELECT k FROM x WHERE k > $1");
  CHECK(p.record().ddl);
  REQUIRE(p.record().params.size() == 1);
  CHECK(t.execute(p.sql(), p.check(), {i(1)}).affected == 1);
  CHECK(t.execute("SELECT k FROM c").rows == Grid{{i(5)}});
  // A held Statement, prepared in autocommit.
  const auto& held = db->prepare("CREATE TABLE h AS SELECT a FROM t WHERE k = $1");
  CHECK(t.execute(held, {i(1)}).affected == 1);
  CHECK(t.execute("SELECT a FROM h").rows == Grid{{i(10)}});
  // A binder error of its query is the statement's when it is prepared.
  CHECK(state([&] { (void)t.prepare("CREATE TABLE d AS SELECT nope FROM x"); }) == "42703");
  CHECK(t.is_aborted());
  // A constant that fails to fold: the parameters are still the statement's,
  // read from a stand-in that never runs. The run folds the query itself.
  auto u = db->begin();
  (void)u.execute("CREATE TABLE y (k BIGINT PRIMARY KEY)");
  auto f = u.prepare("CREATE TABLE z AS SELECT 1/0 AS v, k FROM y WHERE k > $1");
  CHECK(f.record().ddl);
  CHECK(f.record().params.size() == 1);
  CHECK(u.refit(f.sql(), f.check()).params.size() == 1);
  CHECK(state([&] { (void)u.execute(f.sql(), f.check(), {i(1)}); }) == "22012");
  CHECK(u.is_aborted());
}

TEST_CASE("txn DDL: rows inserted into a CREATE TABLE AS table take row ids past its own",
          "[sql][api][txn][ddl][ctas]") {
  sql_test::TempDir dir{"txn_ddl_ctas_ids"};
  auto db = open(dir);
  (void)db->execute("INSERT INTO t VALUES (2, 20)");
  {
    auto t = db->begin();
    (void)t.execute("CREATE TABLE c AS SELECT a FROM t");
    (void)t.execute("INSERT INTO c VALUES (30)");
    CHECK(t.execute("SELECT a FROM c ORDER BY a").rows == Grid{{i(10)}, {i(20)}, {i(30)}});
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  (void)db->execute("INSERT INTO c VALUES (40)");
  CHECK(db->execute("SELECT a FROM c ORDER BY a").rows == Grid{{i(10)}, {i(20)}, {i(30)}, {i(40)}});
  // A rolled-back table drew row ids under the id the next table takes: a
  // CREATE TABLE AS of that id, autocommit or not, writes its own ids, and
  // an insert draws past them.
  for (const auto in_txn : {false, true}) {
    INFO((in_txn ? "in a transaction" : "autocommit"));
    const auto name = std::string{in_txn ? "d2" : "d1"};
    {
      auto r = db->begin();
      (void)r.execute("CREATE TABLE gone (v BIGINT)");
      (void)r.execute("INSERT INTO gone VALUES (1)");
      r.rollback();
    }
    if (!in_txn) (void)db->execute("CREATE TABLE " + name + " AS SELECT k FROM t");
    auto t = db->begin();
    if (in_txn) (void)t.execute("CREATE TABLE " + name + " AS SELECT k FROM t");
    (void)t.execute("INSERT INTO " + name + " VALUES (3)");
    CHECK(t.commit() == CommitOutcome::Committed);
    CHECK(db->execute("SELECT k FROM " + name + " ORDER BY k").rows == Grid{{i(1)}, {i(2)}, {i(3)}});
  }
}

TEST_CASE("txn DDL: ROLLBACK TO undoes a CREATE TABLE AS; max_bytes bounds its rows", "[sql][api][txn][ddl][ctas]") {
  sql_test::TempDir dir{"txn_ddl_ctas_marks"};
  auto db = open(dir);
  {
    auto fill = db->begin();
    for (std::int64_t k = 2; k <= 2000; ++k) (void)fill.execute("INSERT INTO t VALUES ($1, $1)", {i(k)});
    CHECK(fill.commit() == CommitOutcome::Committed);
  }
  {
    auto t = db->begin();
    t.savepoint("s");
    (void)t.execute("CREATE TABLE c AS SELECT k FROM t");
    t.rollback_to("s");
    CHECK(state([&] { (void)t.execute("SELECT k FROM c"); }) == "42P01");
    t.rollback_to("s");
    (void)t.execute("CREATE TABLE c AS SELECT k FROM t WHERE k < 3");
    CHECK(t.commit() == CommitOutcome::Committed);
    CHECK(db->execute("SELECT k FROM c ORDER BY k").rows == Grid{{i(1)}, {i(2)}});
  }
  // The rows the query returns fit; the table they make does not.
  auto t = db->begin(TransactionOptions{32u << 10});
  const auto why = message([&] { (void)t.execute("CREATE TABLE big AS SELECT k, a FROM t"); });
  INFO(why);
  CHECK(why.ends_with("run the DDL outside a transaction"));
  CHECK(t.is_aborted());
}

// docs/catalog_design.md, *Schema DDL batches and transactional DDL*: CREATE
// SCHEMA and DROP SCHEMA are steps of a transaction like any DDL.

TEST_CASE("txn DDL: CREATE SCHEMA is invisible to others until COMMIT, gone after ROLLBACK; two creating one, 40001",
          "[sql][api][txn][ddl][schemas]") {
  sql_test::TempDir dir{"txn_ddl_schema"};
  auto db = open(dir);
  for (const auto commit : {false, true}) {
    INFO((commit ? "COMMIT" : "ROLLBACK"));
    auto t = db->begin();
    (void)t.execute("CREATE SCHEMA s");
    (void)t.execute("CREATE TABLE s.t (k BIGINT PRIMARY KEY)");
    (void)t.execute("INSERT INTO s.t VALUES (7)");
    CHECK(t.execute("SELECT k FROM s.t").rows == Grid{{i(7)}});
    CHECK(t.execute("SELECT k FROM t").rows == Grid{{i(1)}});  // public's t, still
    CHECK(state([&] { (void)db->execute("SELECT k FROM s.t"); }) == "42P01");
    CHECK(db->catalog()->schema("s") == nullptr);
    if (commit) {
      CHECK(t.commit() == CommitOutcome::Committed);
    } else {
      t.rollback();
      CHECK(db->catalog()->schema("s") == nullptr);
    }
  }
  CHECK(db->execute("SELECT k FROM s.t").rows == Grid{{i(7)}});
  // Two transactions create one schema: the second to commit is 40001 and
  // writes nothing.
  auto t1 = db->begin();
  auto t2 = db->begin();
  (void)t1.execute("CREATE SCHEMA u");
  (void)t2.execute("CREATE SCHEMA u");
  CHECK(t1.commit() == CommitOutcome::Committed);
  CHECK(state([&] { (void)t2.commit(); }) == "40001");
  CHECK(db->catalog()->schema("u") != nullptr);
  // A schema dropped in a transaction, then a table created in it: 3F000.
  auto t3 = db->begin();
  (void)t3.execute("DROP SCHEMA u");
  CHECK(state([&] { (void)t3.execute("CREATE TABLE u.t (k BIGINT PRIMARY KEY)"); }) == "3F000");
  t3.rollback();
  // In a transaction, CALL names a procedure of a schema the transaction created.
  auto t4 = db->begin();
  (void)t4.execute("CREATE SCHEMA w");
  (void)t4.execute("CREATE PROCEDURE w.p(INOUT x BIGINT) LANGUAGE plpgsql AS $$ BEGIN x := x + 1; END $$");
  auto def = t4.procedure("w", "p");
  REQUIRE(def != nullptr);
  CHECK(t4.call(QualifiedName{def->schema, "p"}, {i(41)}).rows == Grid{{i(42)}});
  CHECK(t4.procedure("p") == nullptr);  // public has none of the name
  CHECK(state([&] { (void)t4.procedure("nope", "p"); }) == "3F000");
  t4.rollback();
}

TEST_CASE("schemas: a statement prepared on t keeps reading public's t after CREATE TABLE s.t",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_plan_cache"};
  auto db = open(dir);
  const auto& st = db->prepare("SELECT a FROM t");
  CHECK(db->execute(st, {}).rows == Grid{{i(10)}});
  (void)db->execute("CREATE SCHEMA s");
  (void)db->execute("CREATE TABLE s.t (k BIGINT PRIMARY KEY, a BIGINT)");
  (void)db->execute("INSERT INTO s.t VALUES (1, 99)");
  CHECK(db->execute(st, {}).rows == Grid{{i(10)}});
  CHECK(db->execute("SELECT a FROM t").rows == Grid{{i(10)}});
  CHECK(db->execute("SELECT a FROM s.t").rows == Grid{{i(99)}});
  // A view created over an unqualified t stores public's, and reads it after
  // a reopen whatever s holds.
  (void)db->execute("CREATE VIEW s.v AS SELECT a FROM t");
  CHECK(db->catalog()->view({db->catalog()->schema("s")->id, "v"})->names ==
        std::vector<ViewName>{{ViewName::Kind::Relation, "t", kPublicSchema}});
  db->close();
  db = Database::open(dir.path());
  CHECK(db->execute("SELECT a FROM s.v").rows == Grid{{i(10)}});
}

TEST_CASE("schemas: messages name a missing object as s.x, and DROP SCHEMA and DROP ROLE list as PostgreSQL 17 does",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_messages"};
  auto db = open(dir);
  auto notice = [&](const char* sql) { return db->execute(sql).notices.at(0).message; };
  auto detail = [&](const char* sql) -> std::string {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return std::string{error_detail(e)};
    }
    return "ran";
  };
  (void)db->execute("CREATE SCHEMA q");
  (void)db->execute("CREATE ROLE r");
  // Every expectation as PostgreSQL 17 gives it.
  CHECK(notice("DROP DOMAIN IF EXISTS q.nod") == "type \"q.nod\" does not exist, skipping");
  CHECK(notice("DROP TYPE IF EXISTS q.nod") == "type \"q.nod\" does not exist, skipping");
  CHECK(notice("DROP PROCEDURE IF EXISTS q.p2()") == "procedure q.p2() does not exist, skipping");
  CHECK(notice("DROP PROCEDURE IF EXISTS q.p2(INTEGER)") == "procedure q.p2(pg_catalog.int4) does not exist, skipping");
  CHECK(message([&] { (void)db->execute("DROP TYPE q.nod"); }) == "type \"q.nod\" does not exist");
  CHECK(message([&] { (void)db->execute("GRANT USAGE ON TYPE q.nod TO r"); }) == "type \"q.nod\" does not exist");
  CHECK(message([&] { (void)db->execute("REVOKE USAGE ON DOMAIN q.nod FROM r"); }) == "type \"q.nod\" does not exist");
  CHECK(message([&] { (void)db->execute("GRANT SELECT ON q.nt TO r"); }) == "relation \"q.nt\" does not exist");
  CHECK(message([&] { (void)db->execute("GRANT USAGE ON SEQUENCE q.ns TO r"); }) == "relation \"q.ns\" does not exist");
  CHECK(message([&] { (void)db->execute("DROP PROCEDURE q.nop()"); }) == "procedure q.nop() does not exist");
  CHECK(message([&] { (void)db->execute("DROP PROCEDURE q.nop(INT, TEXT)"); }) ==
        "procedure q.nop(integer, text) does not exist");
  CHECK(message([&] { (void)db->execute("DROP PROCEDURE q.nop"); }) == "could not find a procedure named \"q.nop\"");
  CHECK(message([&] { (void)db->execute("DROP PROCEDURE nop(INT)"); }) == "procedure nop(integer) does not exist");
  CHECK(state([&] { (void)db->execute("DROP PROCEDURE q.nop()"); }) == "42883");
  // A schema's name is bare in DROP SCHEMA's message and DETAIL; a relation's is quoted.
  (void)db->execute("CREATE SCHEMA \"R Q\"");
  (void)db->execute("CREATE TABLE \"R Q\".\"T x\" (a INTEGER)");
  CHECK(message([&] { (void)db->execute("DROP SCHEMA \"R Q\""); }) ==
        "cannot drop schema R Q because other objects depend on it");
  CHECK(detail("DROP SCHEMA \"R Q\"") == "table \"R Q\".\"T x\" depends on schema R Q");
  (void)db->execute("DROP TABLE \"R Q\".\"T x\"");
  // RESTRICT reports 100 objects, then how many more: "object" for one.
  (void)db->execute("CREATE SCHEMA big");
  std::string expected;
  for (int k = 1; k <= 101; ++k) {
    (void)db->execute("CREATE TABLE big.t" + std::to_string(k) + " (a INTEGER)");
    if (k <= 100) expected += (k == 1 ? "" : "\n") + ("table big.t" + std::to_string(k) + " depends on schema big");
  }
  CHECK(detail("DROP SCHEMA big") == expected + "\nand 1 other object (see server log for list)");
  (void)db->execute("CREATE TABLE big.t102 (a INTEGER)");
  CHECK(detail("DROP SCHEMA big") == expected + "\nand 2 other objects (see server log for list)");
  // DROP ROLE lists every schema the role owns.
  (void)db->execute("CREATE ROLE r4");
  (void)db->execute("CREATE SCHEMA s4a AUTHORIZATION r4");
  (void)db->execute("CREATE SCHEMA s4b AUTHORIZATION r4");
  CHECK(state([&] { (void)db->execute("DROP ROLE r4"); }) == "2BP01");
  CHECK(detail("DROP ROLE r4") == "owner of schema s4a\nowner of schema s4b");
  // Privileges on a schema are not built (#873).
  for (const auto* sql : {"GRANT USAGE ON SCHEMA q TO PUBLIC", "REVOKE USAGE ON SCHEMA q FROM PUBLIC",
                          "GRANT SELECT ON ALL TABLES IN SCHEMA q TO r", "REVOKE SELECT ON ALL TABLES IN SCHEMA q FROM r"}) {
    CHECK(state([&] { (void)db->execute(sql); }) == "0A000");
  }
  // A qualified column in a subquery that names no item of the subquery's
  // FROM may name the enclosing query's (PostgreSQL reads it there): 0A000,
  // never the 42P01 an item of the same table name would give.
  (void)db->execute("CREATE TABLE q.t (a BIGINT)");
  CHECK(state([&] { (void)db->execute("SELECT a FROM q.t WHERE EXISTS (SELECT 1 FROM t WHERE t.a = q.t.a)"); }) == "0A000");
  CHECK(state([&] { (void)db->execute("SELECT a FROM t WHERE EXISTS (SELECT 1 FROM q.t WHERE q.t.a = public.t.a)"); }) ==
        "0A000");
  CHECK(state([&] { (void)db->execute("SELECT a FROM t WHERE EXISTS (SELECT 1 FROM q.t WHERE q.t.a = t.a)"); }) == "ok");
}

TEST_CASE("schemas: a view whose stored names lack one its query reads is XX000 at use, never a search",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_view_map"};
  {
    auto db = open(dir);
    (void)db->execute("CREATE TABLE u (k BIGINT PRIMARY KEY)");
    (void)db->execute("CREATE VIEW v AS SELECT a FROM t");
    db->close();
  }
  // Forged: the record names u, which the view depends on, but not t, which
  // its query reads. It passes load (u is in its dependencies).
  {
    auto engine = bytecask::DB::open(dir.path());
    LiveCatalog live{engine};
    auto view = *live.current()->view({kPublicSchema, "v"});
    view.names = {{ViewName::Kind::Relation, "u", kPublicSchema}};
    view.depends_on.push_back(live.current()->table({kPublicSchema, "u"})->id);
    engine.put({}, view_record_key(view.id), serialize_view(view));
    engine.close();
  }
  auto db = Database::open(dir.path());
  try {
    (void)db->execute("SELECT a FROM v");
    FAIL("ran");
  } catch (const std::exception& e) {
    CHECK(std::string{sqlstate(e)} == "XX000");
    CHECK(std::string{e.what()} == "view v has no stored resolution for t");
  }
}

TEST_CASE("schemas: an allocation failure in a transaction of CREATE SCHEMA and CREATE TABLE s.t commits all or nothing",
          "[sql][api][txn][ddl][schemas][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"schemas_alloc"};
  auto db = open(dir);
  auto failed = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 200000);
    std::string outcome = "ok";
    bool fired = false;
    {
      auto t = db->begin();
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      outcome = state([&] {
        (void)t.execute("CREATE SCHEMA s");
        (void)t.execute("CREATE TABLE s.t (k BIGINT PRIMARY KEY)");
        (void)t.execute("INSERT INTO s.t VALUES (1)");
        (void)t.commit();
      });
      fired = faults.report().fired;
    }
    INFO("allocation " << nth << ": " << outcome);
    const auto cat = db->catalog();
    const auto* s = cat->schema("s");
    CHECK((s != nullptr) == (outcome == "ok"));
    if (s != nullptr) {
      REQUIRE(cat->table({s->id, "t"}) != nullptr);
      CHECK(db->execute("SELECT k FROM s.t").rows == Grid{{i(1)}});
      (void)db->execute("DROP TABLE s.t");
      (void)db->execute("DROP SCHEMA s");
    }
    if (!fired) break;
    ++failed;
  }
  CHECK(failed > 0);
}

namespace {

// Whether the database holds schema `s`, or any object of it.
auto holds_schema(Database& db, std::string_view s) -> bool { return db.catalog()->schema(s) != nullptr; }

}  // namespace

TEST_CASE("schemas: CREATE SCHEMA with elements runs them in PostgreSQL's order, with the path (s, public)",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_elements"};
  auto db = open(dir);
  (void)db->execute("CREATE ROLE r");
  // An index written before its table, and the table its own schema's even
  // with a public table of the name.
  (void)db->execute("CREATE TABLE b (a BIGINT)");
  (void)db->execute("CREATE SCHEMA e1 CREATE INDEX i ON b (a) CREATE TABLE b (a BIGINT PRIMARY KEY)");
  auto cat = db->catalog();
  const auto e1 = cat->schema("e1")->id;
  CHECK(cat->index({e1, "i"}).first == cat->table({e1, "b"}));
  CHECK(cat->index({kPublicSchema, "i"}).first == nullptr);
  // A view written before its table reads it: tables run first.
  (void)db->execute(
      "CREATE SCHEMA e2 CREATE VIEW v AS SELECT x FROM u CREATE SEQUENCE q CREATE TABLE u (x BIGINT PRIMARY KEY) "
      "GRANT SELECT ON u, v TO r GRANT SELECT ON t TO r");
  cat = db->catalog();
  const auto e2 = cat->schema("e2")->id;
  REQUIRE(cat->view({e2, "v"}) != nullptr);
  CHECK(cat->view({e2, "v"})->names == std::vector<ViewName>{{ViewName::Kind::Relation, "u", e2}});
  CHECK(cat->sequence({e2, "q"}) != nullptr);
  const auto r = cat->role("r")->id;
  auto granted_to_r = [&](std::uint32_t object) {
    const auto acl = cat->acl(object);
    return std::ranges::any_of(acl, [&](const AclEntry& e) { return e.grantee == r; });
  };
  CHECK(granted_to_r(cat->table({e2, "u"})->id));
  CHECK(granted_to_r(cat->view({e2, "v"})->id));
  CHECK(granted_to_r(cat->table({kPublicSchema, "t"})->id));  // t is public's: e2 holds no t
  CHECK_FALSE(granted_to_r(cat->table({kPublicSchema, "b"})->id));
  // A view over a later-written view is 42P01: views run in written order.
  CHECK(state([&] {
          (void)db->execute("CREATE SCHEMA e3 CREATE VIEW v1 AS SELECT * FROM v2 CREATE VIEW v2 AS SELECT 1 AS x");
        }) == "42P01");
  CHECK_FALSE(holds_schema(*db, "e3"));
  // A table that references itself, unqualified, references its own schema's.
  (void)db->execute("CREATE SCHEMA e4 CREATE TABLE t (k BIGINT PRIMARY KEY, up BIGINT REFERENCES t)");
  CHECK(db->catalog()->table({db->catalog()->schema("e4")->id, "t"}) != nullptr);
}

TEST_CASE("schemas: an element placed elsewhere is 42P15, a temporary one 42P16, and CREATE TRIGGER 0A000",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_element_errors"};
  auto db = open(dir);
  CHECK(message([&] { (void)db->execute("CREATE SCHEMA s CREATE TABLE x (a BIGINT) CREATE TABLE o.y (a BIGINT)"); }) ==
        "CREATE specifies a schema (o) different from the one being created (s)");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s CREATE INDEX i ON o.t (a)"); }) == "42P15");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s CREATE TEMP TABLE x (a BIGINT)"); }) == "42P16");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s CREATE TEMP SEQUENCE q"); }) == "42P16");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s CREATE TEMP VIEW v AS SELECT 1"); }) == "42P16");
  // The schema's own errors come first, as in PostgreSQL 17.
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA public CREATE TABLE o.x (a BIGINT)"); }) == "42P06");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA public CREATE TEMP TABLE x (a BIGINT)"); }) == "42P06");
  CHECK(state([&] {
          (void)db->execute("CREATE SCHEMA s CREATE TRIGGER tr AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION f()");
        }) == "0A000");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s CREATE FUNCTION f() RETURNS int AS 'select 1' LANGUAGE sql"); }) ==
        "42601");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s REVOKE SELECT ON t FROM PUBLIC"); }) == "42601");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA IF NOT EXISTS s CREATE TABLE x (a BIGINT)"); }) == "0A000");
  CHECK(state([&] { (void)db->execute("CREATE SCHEMA s CREATE TABLE x (a BIGINT)", {i(1)}); }) == "08P01");
  CHECK_FALSE(holds_schema(*db, "s"));
}

TEST_CASE("schemas: a failing element leaves no schema and no element, in autocommit and in a transaction",
          "[sql][api][txn][schemas]") {
  sql_test::TempDir dir{"schemas_element_atomic"};
  auto db = open(dir);
  const std::vector<std::string> failing{
      "CREATE SCHEMA s CREATE TABLE x (a BIGINT) CREATE SEQUENCE q CREATE SEQUENCE q",
      "CREATE SCHEMA s CREATE TABLE x (a BIGINT) CREATE TABLE x (b BIGINT)",
      "CREATE SCHEMA s CREATE TABLE x (a BIGINT) CREATE VIEW v AS SELECT * FROM nosuch",
      "CREATE SCHEMA s CREATE TABLE x (a BIGINT) CREATE INDEX i ON x (nosuch)",
      "CREATE SCHEMA s CREATE TABLE x (a BIGINT) GRANT SELECT ON x TO nobody",
  };
  for (const auto& sql : failing) {
    INFO(sql);
    CHECK(state([&] { (void)db->execute(sql); }) != "ok");
    CHECK_FALSE(holds_schema(*db, "s"));
    auto t = db->begin();
    (void)t.execute("INSERT INTO t VALUES (2, 20)");
    CHECK(state([&] { (void)t.execute(sql); }) != "ok");
    CHECK(t.is_aborted());
    CHECK(t.commit() == CommitOutcome::RolledBack);
    CHECK_FALSE(holds_schema(*db, "s"));
    CHECK(db->execute("SELECT k FROM t").rows == Grid{{i(1)}});
  }
  // In a transaction, one that works is the transaction's: others see it at its commit.
  auto t = db->begin();
  (void)t.execute("CREATE SCHEMA s CREATE TABLE x (a BIGINT) CREATE VIEW v AS SELECT a FROM x");
  (void)t.execute("INSERT INTO s.x VALUES (5)");
  CHECK(t.execute("SELECT a FROM s.v").rows == Grid{{i(5)}});
  CHECK_FALSE(holds_schema(*db, "s"));
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(db->execute("SELECT a FROM s.v").rows == Grid{{i(5)}});
}

TEST_CASE("schemas: a view created as an element keeps reading what it read at creation, through a reopen",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_element_view"};
  {
    auto db = open(dir);
    (void)db->execute("CREATE TABLE t1 (k BIGINT PRIMARY KEY, a BIGINT)");
    (void)db->execute("INSERT INTO t1 VALUES (1, 10), (2, 20)");
    (void)db->execute(
        "CREATE SCHEMA s "
        "CREATE VIEW v AS SELECT a FROM t1 "
        "CREATE VIEW vs AS SELECT k, (SELECT MAX(a) FROM t1) AS m FROM t "
        "CREATE VIEW vin AS SELECT k FROM t WHERE k IN (SELECT k FROM t1) "
        "CREATE VIEW vex AS SELECT k FROM t WHERE EXISTS (SELECT 1 FROM t1 WHERE t1.k = 2)");
    const auto s = db->catalog()->schema("s")->id;
    // The view stores its own text, an ordinary CREATE VIEW.
    CHECK(db->catalog()->view({s, "v"})->definition == "CREATE VIEW v AS SELECT a FROM t1");
    CHECK(db->catalog()->view({s, "vex"})->definition == "CREATE VIEW vex AS SELECT k FROM t WHERE EXISTS (SELECT 1 FROM t1 WHERE t1.k = 2)");
    (void)db->execute("CREATE TABLE s.t1 (k BIGINT PRIMARY KEY, a BIGINT)");
    (void)db->execute("CREATE TABLE s.t (k BIGINT PRIMARY KEY)");
    (void)db->execute("INSERT INTO s.t1 VALUES (9, 90)");
    CHECK(db->execute("SELECT a FROM s.v ORDER BY a").rows == Grid{{i(10)}, {i(20)}});
    db->close();
  }
  auto db = Database::open(dir.path());
  CHECK(db->execute("SELECT a FROM s.v ORDER BY a").rows == Grid{{i(10)}, {i(20)}});
  CHECK(db->execute("SELECT k, m FROM s.vs").rows == Grid{{i(1), i(20)}});
  CHECK(db->execute("SELECT k FROM s.vin").rows == Grid{{i(1)}});
  CHECK(db->execute("SELECT k FROM s.vex").rows == Grid{{i(1)}});
}

TEST_CASE("schemas: INSERT through an element view, and a view whose text cannot be cut out is 0A000",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_element_insert"};
  auto db = open(dir);
  // Comments and a closing semicolon are not the view's text.
  (void)db->execute(
      "CREATE SCHEMA s /* c */ CREATE TABLE x (k BIGINT PRIMARY KEY, a BIGINT) -- x\n"
      " CREATE VIEW v (kk) AS SELECT k /* create */ FROM x ;");
  const auto s = db->catalog()->schema("s")->id;
  CHECK(db->catalog()->view({s, "v"})->definition == "CREATE VIEW v (kk) AS SELECT k /* create */ FROM x");
  CHECK(db->execute("INSERT INTO s.v VALUES (3)").affected == 1);
  CHECK(db->execute("SELECT k FROM s.x").rows == Grid{{i(3)}});
  CHECK(message([&] { (void)db->execute("CREATE SCHEMA s2 CREATE VIEW v AS SELECT a AS create FROM t"); }) ==
        "CREATE VIEW as a schema element whose text cannot be cut out of the statement is not supported");
  CHECK(state([&] {
          (void)db->execute("CREATE SCHEMA s2 CREATE VIEW v AS SELECT a AS grant FROM t CREATE TABLE y (a BIGINT)");
        }) == "0A000");
  CHECK_FALSE(holds_schema(*db, "s2"));
  // The GRANT of WITH GRANT OPTION, and a privilege CREATE, start no element:
  // the view after them is cut out whole.
  (void)db->execute("CREATE ROLE r");
  (void)db->execute(
      "CREATE SCHEMA s3 GRANT SELECT ON t TO r WITH GRANT OPTION CREATE TABLE y (a BIGINT) "
      "GRANT SELECT, INSERT ON y TO r WITH GRANT OPTION CREATE VIEW w AS SELECT 2 AS z");
  const auto s3 = db->catalog()->schema("s3")->id;
  CHECK(db->catalog()->view({s3, "w"})->definition == "CREATE VIEW w AS SELECT 2 AS z");
  CHECK(db->execute("SELECT z FROM s3.w").rows == Grid{{i(2)}});
  // s.t.c where t is read unqualified: which schema's t is known only at bind.
  for (const auto* sql : {"CREATE SCHEMA s4 CREATE TABLE y (a BIGINT) CREATE VIEW v AS SELECT s4.y.a FROM y",
                          "CREATE SCHEMA s4 CREATE VIEW v AS SELECT public.t.k FROM t",
                          "CREATE SCHEMA s4 CREATE VIEW v AS SELECT k FROM t WHERE EXISTS (SELECT s4.y.* FROM y)"}) {
    INFO(sql);
    CHECK(state([&] { (void)db->execute(sql); }) == "0A000");
    CHECK_FALSE(holds_schema(*db, "s4"));
  }
  // Qualified in its FROM too, it is read as anywhere else.
  (void)db->execute("CREATE SCHEMA s4 CREATE TABLE y (a BIGINT) CREATE VIEW v AS SELECT s4.y.a FROM s4.y");
  (void)db->execute("INSERT INTO s4.y VALUES (7)");
  CHECK(db->execute("SELECT a FROM s4.v").rows == Grid{{i(7)}});
}

TEST_CASE("schemas: a statement prepared through a view rebinds after CREATE SCHEMA with elements",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_element_rebind"};
  auto db = open(dir);
  (void)db->execute("CREATE VIEW v AS SELECT a FROM t");
  const auto& st = db->prepare("SELECT a FROM v");
  const auto before = db->catalog()->version();
  (void)db->execute("CREATE SCHEMA s CREATE TABLE t (a BIGINT) CREATE VIEW v AS SELECT 99 AS a");
  CHECK(db->catalog()->version() > before);
  CHECK(db->execute(st, {}).rows == Grid{{i(10)}});
  CHECK(db->execute("SELECT a FROM s.v").rows == Grid{{i(99)}});
}

TEST_CASE("schemas: CREATE SCHEMA with elements that loses to another DDL is 40001 with nothing written, "
          "and a crash at its commit leaves nothing after reopen",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_element_race"};
  {
    auto db = open(dir);
    std::string outcome;
    db->live_catalog_for_testing().set_commit_hook_for_testing([&] {
      std::thread other{[&] { outcome = state([&] { (void)db->execute("CREATE SCHEMA s2 CREATE TABLE x (a BIGINT)"); }); }};
      other.join();
    });
    (void)db->execute("CREATE SCHEMA s1 CREATE TABLE x (a BIGINT)");
    db->live_catalog_for_testing().set_commit_hook_for_testing({});
    CHECK(outcome == "40001");
    CHECK(holds_schema(*db, "s1"));
    CHECK_FALSE(holds_schema(*db, "s2"));
    db->live_catalog_for_testing().set_commit_hook_for_testing([] { throw std::runtime_error{"crash"}; });
    CHECK_THROWS(db->execute("CREATE SCHEMA s3 CREATE TABLE x (a BIGINT) CREATE VIEW v AS SELECT a FROM x"));
    db->live_catalog_for_testing().set_commit_hook_for_testing({});
    db->close();
  }
  auto db = Database::open(dir.path());
  CHECK(holds_schema(*db, "s1"));
  CHECK_FALSE(holds_schema(*db, "s3"));
  CHECK(state([&] { (void)db->execute("SELECT a FROM s3.x"); }) == "42P01");
}

TEST_CASE("schemas: an allocation failure in CREATE SCHEMA with elements in autocommit writes all or nothing",
          "[sql][api][schemas][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"schemas_element_alloc"};
  auto db = open(dir);
  auto failed = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 200000);
    std::string outcome = "ok";
    bool fired = false;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      outcome = state([&] {
        (void)db->execute("CREATE SCHEMA s CREATE TABLE x (k BIGINT PRIMARY KEY) CREATE VIEW v AS SELECT k FROM x");
      });
      fired = faults.report().fired;
    }
    INFO("allocation " << nth << ": " << outcome);
    const auto cat = db->catalog();
    const auto* s = cat->schema("s");
    CHECK((s != nullptr) == (outcome == "ok"));
    if (s != nullptr) {
      REQUIRE(cat->table({s->id, "x"}) != nullptr);
      REQUIRE(cat->view({s->id, "v"}) != nullptr);
      (void)db->execute("DROP VIEW s.v");
      (void)db->execute("DROP TABLE s.x");
      (void)db->execute("DROP SCHEMA s");
    } else {
      CHECK(cat->schemas().empty());
    }
    if (!fired) break;
    ++failed;
  }
  CHECK(failed > 0);
}

namespace {

// Every key the engine holds: what a DROP that leaves nothing behind must
// restore exactly.
auto all_keys(Database& db) -> std::set<Bytes> {
  std::set<Bytes> out;
  auto snap = db.engine_for_testing().snapshot();
  for_each_in_prefix(snap, Bytes{}, [&](BytesView k, BytesView) { out.emplace(k.begin(), k.end()); });
  return out;
}

// A schema s of every kind of object DROP SCHEMA … CASCADE drops, each with
// an ACL where it takes one, and two views in public over its view.
void make_cascade_schema(Database& db) {
  for (const auto* sql : {"CREATE SCHEMA s", "CREATE TABLE s.x (k BIGINT PRIMARY KEY, b BIGINT)",
                          "INSERT INTO s.x VALUES (1, 10), (2, 20)", "CREATE INDEX xb ON s.x (b)",
                          "CREATE VIEW s.v AS SELECT k FROM s.x", "CREATE SEQUENCE s.q", "CREATE DOMAIN s.d AS INTEGER",
                          "CREATE TYPE s.sh", "CREATE PROCEDURE s.p(a INTEGER, b TEXT) LANGUAGE plpgsql AS $$ BEGIN NULL; END $$",
                          "GRANT SELECT ON s.x TO r", "GRANT SELECT ON s.v TO r", "GRANT USAGE ON SEQUENCE s.q TO r",
                          "GRANT USAGE ON DOMAIN s.d TO r", "CREATE VIEW pv AS SELECT k FROM s.v",
                          "CREATE VIEW pv2 AS SELECT k FROM pv"}) {
    (void)db.execute(sql);
  }
}

// Whether every object make_cascade_schema makes is there, or none is.
auto cascade_schema_state(Database& db) -> std::string {
  const auto cat = db.catalog();
  const auto* s = cat->schema("s");
  if (s == nullptr) {
    auto none = cat->view({kPublicSchema, "pv"}) == nullptr && cat->view({kPublicSchema, "pv2"}) == nullptr;
    return none ? "none" : "views left";
  }
  auto all = cat->table({s->id, "x"}) != nullptr && cat->index({s->id, "xb"}).second != nullptr &&
             cat->view({s->id, "v"}) != nullptr && cat->sequence({s->id, "q"}) != nullptr &&
             cat->type({s->id, "d"}) != nullptr && cat->type({s->id, "sh"}) != nullptr &&
             cat->procedure({s->id, "p"}) != nullptr && cat->view({kPublicSchema, "pv"}) != nullptr &&
             cat->view({kPublicSchema, "pv2"}) != nullptr;
  if (!all) return "partial";
  return db.execute("SELECT k FROM pv2 ORDER BY k").rows == Grid{{i(1)}, {i(2)}} ? "all" : "rows lost";
}

}  // namespace

TEST_CASE("schemas: DROP SCHEMA … CASCADE drops every object of the schema and the views that read them, and nothing else",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_cascade"};
  auto db = open(dir);
  (void)db->execute("CREATE ROLE r");
  (void)db->execute("CREATE TABLE keep (k BIGINT PRIMARY KEY)");
  (void)db->execute("CREATE VIEW keepv AS SELECT k FROM t");
  (void)db->execute("GRANT SELECT ON t TO r");
  const auto before = all_keys(*db);
  make_cascade_schema(*db);
  // A procedure elsewhere whose body names a dropped table is not dropped:
  // its body is no dependency, as a PL/pgSQL body is not in PostgreSQL 17.
  (void)db->execute("CREATE PROCEDURE pp() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO s.x VALUES (3, 30); END $$");
  REQUIRE(cascade_schema_state(*db) == "all");

  // RESTRICT lists what CASCADE drops, as PostgreSQL 17 lists it: no index.
  std::string restrict_detail;
  try {
    (void)db->execute("DROP SCHEMA s RESTRICT");
    FAIL("ran");
  } catch (const std::exception& e) {
    CHECK(std::string{sqlstate(e)} == "2BP01");
    restrict_detail = std::string{error_detail(e)};
  }
  CHECK(restrict_detail ==
        "table s.x depends on schema s\nview s.v depends on schema s\nview pv depends on view s.v\n"
        "view pv2 depends on view pv\nsequence s.q depends on schema s\ntype s.d depends on schema s\n"
        "type s.sh depends on schema s\nfunction s.p(integer,text) depends on schema s");

  const auto out = db->execute("DROP SCHEMA s CASCADE");
  REQUIRE(out.notices.size() == 1);
  CHECK(out.notices[0].sqlstate == "00000");
  CHECK(out.notices[0].message == "drop cascades to 8 other objects");
  CHECK(out.notices[0].detail ==
        "drop cascades to table s.x\ndrop cascades to view s.v\ndrop cascades to view pv\n"
        "drop cascades to view pv2\ndrop cascades to sequence s.q\ndrop cascades to type s.d\n"
        "drop cascades to type s.sh\ndrop cascades to function s.p(integer,text)");
  CHECK(cascade_schema_state(*db) == "none");
  CHECK(state([&] { (void)db->execute("SELECT k FROM pv"); }) == "42P01");
  // The rest stands: the public tables, the view over one, the role, the
  // procedure (which now fails when called, as in PostgreSQL 17).
  CHECK(db->execute("SELECT k FROM keepv").rows == Grid{{i(1)}});
  CHECK(db->catalog()->role("r") != nullptr);
  CHECK(state([&] { (void)db->call("pp", {}); }) == "42P01");
  (void)db->execute("DROP PROCEDURE pp");
  // No key under any dropped object's prefix, no record, entry or ACL left:
  // the engine holds exactly the keys it held before the schema.
  CHECK(all_keys(*db) == before);

  // The same through a reopen: nothing comes back.
  db->close();
  db = Database::open(dir.path());
  CHECK(cascade_schema_state(*db) == "none");
  CHECK(all_keys(*db) == before);
}

TEST_CASE("schemas: DROP SCHEMA … CASCADE gives PostgreSQL 17's NOTICE for one object, for none, and after IF EXISTS",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_cascade_notice"};
  auto db = open(dir);
  // Every expectation as PostgreSQL 17 gives it.
  (void)db->execute("CREATE SCHEMA a");
  CHECK(db->execute("DROP SCHEMA a CASCADE").notices.empty());  // an empty schema: no NOTICE
  (void)db->execute("CREATE SCHEMA a");
  (void)db->execute("CREATE TABLE a.t (k BIGINT)");
  auto one = db->execute("DROP SCHEMA a CASCADE");
  REQUIRE(one.notices.size() == 1);
  CHECK(one.notices[0].message == "drop cascades to table a.t");
  CHECK(one.notices[0].detail.empty());
  // Two schemas, a missing one skipped first; a view in one over the
  // other's table is the schema's object, not a reader.
  (void)db->execute("CREATE SCHEMA a");
  (void)db->execute("CREATE TABLE a.t (k BIGINT)");
  (void)db->execute("CREATE SCHEMA b");
  (void)db->execute("CREATE VIEW b.w AS SELECT k FROM a.t");
  auto two = db->execute("DROP SCHEMA IF EXISTS a, nope, b CASCADE");
  REQUIRE(two.notices.size() == 2);
  CHECK(two.notices[0].message == "schema \"nope\" does not exist, skipping");
  CHECK(two.notices[1].message == "drop cascades to 2 other objects");
  CHECK(two.notices[1].detail == "drop cascades to table a.t\ndrop cascades to view b.w");
  CHECK(db->catalog()->schemas().empty());
  // A missing name without IF EXISTS drops nothing; the system schemas are refused as RESTRICT refuses them.
  (void)db->execute("CREATE SCHEMA a");
  (void)db->execute("CREATE TABLE a.t (k BIGINT)");
  CHECK(state([&] { (void)db->execute("DROP SCHEMA a, nope CASCADE"); }) == "3F000");
  CHECK(state([&] { (void)db->execute("DROP SCHEMA a, pg_catalog CASCADE"); }) == "2BP01");
  CHECK(state([&] { (void)db->execute("DROP SCHEMA a, public CASCADE"); }) == "0A000");
  CHECK(db->catalog()->schema("a") != nullptr);
  // Past 100 objects the DETAIL ends with how many more, the count in the message is all of them.
  for (int k = 1; k <= 101; ++k) (void)db->execute("CREATE TABLE a.t" + std::to_string(k) + " (a INTEGER)");
  auto big = db->execute("DROP SCHEMA a CASCADE");
  REQUIRE(big.notices.size() == 1);
  CHECK(big.notices[0].message == "drop cascades to 102 other objects");
  CHECK(big.notices[0].detail.starts_with("drop cascades to table a.t\ndrop cascades to table a.t1\n"));
  CHECK(big.notices[0].detail.ends_with("\ndrop cascades to table a.t99\nand 2 other objects (see server log for list)"));
  // DROP TABLE and DROP VIEW … CASCADE stay refused, by the parser, as a feature
  // not built (#872).
  CHECK(state([&] { (void)db->execute("DROP TABLE t CASCADE"); }) == "0A000");
  CHECK(state([&] { (void)db->execute("DROP VIEW v CASCADE"); }) == "0A000");
}

TEST_CASE("schemas: DROP SCHEMA … CASCADE in a transaction: rolled back it restores all, and an open cursor makes it 55006",
          "[sql][api][txn][ddl][schemas][cursor]") {
  sql_test::TempDir dir{"schemas_cascade_txn"};
  auto db = open(dir);
  (void)db->execute("CREATE ROLE r");
  make_cascade_schema(*db);
  const auto keys = all_keys(*db);
  {
    auto t = db->begin();
    auto out = t.execute("DROP SCHEMA s CASCADE");
    CHECK(out.notices.at(0).message == "drop cascades to 8 other objects");
    // The transaction sees it gone; others see it all until the commit.
    CHECK(state([&] { (void)t.execute("SELECT k FROM pv2"); }) == "42P01");
    CHECK(cascade_schema_state(*db) == "all");
    t.rollback();
  }
  CHECK(cascade_schema_state(*db) == "all");
  CHECK(all_keys(*db) == keys);
  constexpr auto kInUse = " because it is being used by active queries in this session";
  {
    // As PostgreSQL 17: a cursor over a table of the schema is 55006 under
    // CASCADE, and RESTRICT is 2BP01 first.
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM s.x", ResultCheck::none(), {});
    CHECK(message([&] { (void)t.execute("DROP SCHEMA s CASCADE"); }) == std::string{"cannot DROP TABLE \"x\""} + kInUse);
    CHECK(t.is_aborted());
  }
  {
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM pv2", ResultCheck::none(), {});  // through the views
    CHECK(state([&] { (void)t.execute("DROP SCHEMA s"); }) == "2BP01");
  }
  {
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM pv2", ResultCheck::none(), {});
    CHECK(state([&] { (void)t.execute("DROP SCHEMA s CASCADE"); }) == "55006");
  }
  {
    // A cursor that scans the index holds the index, named as PostgreSQL 17 names it.
    for (std::int64_t k = 3; k <= 500; ++k) (void)db->execute("INSERT INTO s.x VALUES ($1, $1)", {i(k)});
    REQUIRE(db->prepare("SELECT k FROM s.x WHERE b = $1").plan_source().find("IDX_") != std::string::npos);
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM s.x WHERE b = $1", ResultCheck::none(), {i(10)});
    CHECK(message([&] { (void)t.execute("DROP SCHEMA s CASCADE"); }) == std::string{"cannot DROP INDEX \"xb\""} + kInUse);
  }
  {
    // None for a cursor over a table the drop leaves; committed, it is gone for all.
    auto t = db->begin();
    auto cursor = t.open_cursor("SELECT k FROM t", ResultCheck::none(), {});
    (void)t.execute("DROP SCHEMA s CASCADE");
    CHECK(cursor.fetch(10).rows == Grid{{i(1)}});
    CHECK(t.commit() == CommitOutcome::Committed);
  }
  CHECK(cascade_schema_state(*db) == "none");
}

TEST_CASE("schemas: a crash at the commit hook of DROP SCHEMA … CASCADE leaves every object after reopen",
          "[sql][api][schemas]") {
  sql_test::TempDir dir{"schemas_cascade_crash"};
  {
    auto db = open(dir);
    (void)db->execute("CREATE ROLE r");
    make_cascade_schema(*db);
    db->live_catalog_for_testing().set_commit_hook_for_testing([] { throw std::runtime_error{"crash"}; });
    CHECK_THROWS(db->execute("DROP SCHEMA s CASCADE"));
    db->live_catalog_for_testing().set_commit_hook_for_testing({});
    db->close();
  }
  auto db = Database::open(dir.path());
  CHECK(cascade_schema_state(*db) == "all");
  (void)db->execute("DROP SCHEMA s CASCADE");
  CHECK(cascade_schema_state(*db) == "none");
}

TEST_CASE("schemas: an allocation failure in DROP SCHEMA … CASCADE drops all or nothing",
          "[sql][api][schemas][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"schemas_cascade_alloc"};
  auto db = open(dir);
  (void)db->execute("CREATE ROLE r");
  auto failed = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 200000);
    if (db->catalog()->schema("s") == nullptr) make_cascade_schema(*db);
    std::string outcome = "ok";
    bool fired = false;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      outcome = state([&] { (void)db->execute("DROP SCHEMA s CASCADE"); });
      fired = faults.report().fired;
    }
    INFO("allocation " << nth << ": " << outcome);
    CHECK(cascade_schema_state(*db) == (outcome == "ok" ? "none" : "all"));
    if (!fired) break;
    ++failed;
  }
  CHECK(failed > 0);
}
