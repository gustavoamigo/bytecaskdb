// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Stage 2 of transactional DDL (docs/transaction_design.md, *Stages of
// transactional DDL*): DDL in an embedded Transaction. Later statements see
// it; ROLLBACK, an abort or a ROLLBACK TO before it undoes it; COMMIT
// publishes it with the transaction's writes, or nothing; other sessions see
// nothing of it until then and never wait; any two DDL commits conflict, and
// the second to commit fails.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
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
  const auto [table, ix] = db.catalog()->index(name);
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
  CHECK(db->catalog()->table("x") == nullptr);
  CHECK(db->catalog()->table("y") != nullptr);
  CHECK(db->catalog()->table("z1") != nullptr);
  CHECK(db->catalog()->table("z2") == nullptr);
  CHECK(db->catalog()->index("t_a").second == nullptr);
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
  CHECK(db->catalog()->index("i").second == nullptr);
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
    auto t = db->begin();
    CHECK(state([&] { (void)t.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)"); }) == "42P07");
    CHECK(t.is_aborted());
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
      CHECK(db->catalog()->index("t_a").second == nullptr);
      CHECK(names(db->execute("SELECT * FROM t")) == Names{"k", "a"});
      (void)db->execute("DELETE FROM t WHERE k = 100");
    }
    // DROP INDEX against a concurrent insert, whose entry it would range-delete.
    (void)db->execute("CREATE INDEX t_a ON t (a)");
    auto t1 = db->begin();
    (void)t1.execute("DROP INDEX t_a");
    (void)db->execute("INSERT INTO t VALUES (100, 1)");
    CHECK(state([&] { (void)t1.commit(); }) == "40001");
    CHECK(db->catalog()->index("t_a").second != nullptr);
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
    CHECK(db->catalog()->table("x") == nullptr);
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
  CHECK(db->catalog()->table("x") == nullptr);
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
    CHECK(db->catalog()->table("x") != nullptr);
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
    CHECK(db->catalog()->table("y") == nullptr);
  }
  {
    // No DDL published since: the key a concurrent insert took is a duplicate.
    auto t = db->begin();
    (void)t.execute("CREATE TABLE z (k BIGINT PRIMARY KEY)");
    (void)t.execute("INSERT INTO t VALUES (5, 50)");
    (void)db->execute("INSERT INTO t VALUES (5, 55)");
    CHECK(state([&] { (void)t.commit(); }) == "23505");
    CHECK(db->catalog()->table("z") == nullptr);
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
  const auto* table = cat->table("t");
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
    const auto has = db->catalog()->table("x") != nullptr;
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
        std::thread reader{[&] { seen = db->catalog()->table("x") == nullptr ? "absent" : "present"; }};
        reader.join();
        if (fail) throw std::runtime_error{"crash"};
      });
      const auto outcome = state([&] { (void)t.commit(); });
      CHECK((outcome == "ok") == !fail);
      db->live_catalog_for_testing().set_commit_hook_for_testing({});
      CHECK(seen == "absent");
    }
    auto db = Database::open(dir.path());
    CHECK((db->catalog()->table("x") != nullptr) == !fail);
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
      if (db->catalog()->index("t_a").second != nullptr) (void)db->execute("DROP INDEX t_a");
      if (db->catalog()->table("t")->column("c") != nullptr) {
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
        const auto has = db->catalog()->table("x") != nullptr;
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
