// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The forward-only cursor (docs/cursor_design.md, *What a cursor sees*):
// Transaction::open_cursor pins the snapshot, the transaction's writes as of
// the call, the parameters, the clocks and the plan; fetch() returns exactly
// the rows of a full run in order, whatever the page size and whatever the
// transaction writes meanwhile.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

namespace {

using RowVec = std::vector<std::vector<Value>>;

constexpr std::int64_t kRows = 25;

auto populated(const sql_test::TempDir& dir) -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT)");
  db->execute("CREATE INDEX t_by_name ON t (name)");
  for (std::int64_t i = 1; i <= kRows; ++i) {
    db->execute("INSERT INTO t VALUES ($1, $2, $3)", {i, sql_test::bytes("n" + std::to_string(i % 4)), i % 5});
  }
  return db;
}

// Every remaining row, `k` at a time.
auto drain(Cursor& c, std::uint64_t k) -> RowVec {
  RowVec out;
  for (;;) {
    auto page = c.fetch(k);
    REQUIRE(page.rows.size() <= k);
    out.insert(out.end(), page.rows.begin(), page.rows.end());
    if (page.rows.empty()) break;
  }
  return out;
}

void insert(Transaction& txn, std::int64_t id) {
  txn.execute("INSERT INTO t VALUES ($1, $2, $3)", {id, sql_test::bytes("n0"), std::int64_t{1}});
}

}  // namespace

TEST_CASE("cursor: fetch returns the rows of a full run, in order, for every page size", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_pages"};
  auto db = populated(dir);
  auto txn = db->begin();
  for (std::string_view sql : {"SELECT id, qty FROM t", "SELECT id FROM t WHERE qty > 1 ORDER BY qty, id",
                               "SELECT name, COUNT(*) FROM t GROUP BY name", "SELECT id FROM t ORDER BY name DESC, id",
                               "SELECT a.id, b.id FROM t a JOIN t b ON a.qty = b.id WHERE a.id < 12"}) {
    INFO(sql);
    const auto& stmt = db->prepare(sql);
    auto full = txn.execute(stmt, {}).rows;
    REQUIRE_FALSE(full.empty());
    for (std::uint64_t k : {1u, 2u, 3u, 7u, 25u, 1000u}) {
      INFO("page size " << k);
      auto c = txn.open_cursor(stmt, {});
      CHECK(c.position() == 0);
      CHECK_FALSE(c.at_end());
      CHECK(drain(c, k) == full);
      CHECK(c.position() == full.size());
      CHECK(c.at_end());
      CHECK(c.fetch(k).rows.empty());
      c.close();
    }
  }
  SECTION("columns come with every page, an empty one included") {
    auto c = txn.open_cursor(db->prepare("SELECT id, name FROM t"), {});
    CHECK(c.fetch(0).columns == std::vector<std::string>{"id", "name"});
    CHECK(c.position() == 0);
    (void)drain(c, 100);
    CHECK(c.fetch(5).columns == std::vector<std::string>{"id", "name"});
  }
}

TEST_CASE("cursor: a write after open changes nothing the cursor returns", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_writes"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id, qty FROM t");

  auto run = [&](bool before_write, bool with_earlier_writes, std::uint64_t k) {
    auto txn = db->begin();
    if (with_earlier_writes) {  // the overlay is non-empty and shared when the cursor opens
      insert(txn, 100);
      txn.execute("UPDATE t SET qty = 9 WHERE id = 3");
      txn.execute("DELETE FROM t WHERE id = 4");
    }
    auto expected = txn.execute(scan, {}).rows;
    auto c = txn.open_cursor(scan, {});
    RowVec got;
    auto take = [&](std::uint64_t n) {
      auto page = c.fetch(n);
      got.insert(got.end(), page.rows.begin(), page.rows.end());
    };
    if (!before_write) take(k);
    insert(txn, 200);                                       // an insert, before or after the first fetch
    txn.execute("UPDATE t SET qty = qty + 100 WHERE id < 20");  // updates, in front of and behind the position
    txn.execute("DELETE FROM t WHERE id > 10");             // deletes, ahead of the position
    txn.execute("DELETE FROM t WHERE id = 1");              // and behind it
    insert(txn, 1);
    take(k);
    auto rest = drain(c, k);
    got.insert(got.end(), rest.begin(), rest.end());
    CHECK(got == expected);
    // The transaction sees its own writes, the cursor's pin did not hide them.
    CHECK(txn.execute(scan, {}).rows != expected);
    CHECK(txn.execute("SELECT COUNT(*) FROM t WHERE id = 1").rows.at(0).at(0) == Value{std::int64_t{1}});
    // Another cursor opened now sees all of them.
    auto after = txn.execute(scan, {}).rows;
    auto c2 = txn.open_cursor(scan, {});
    CHECK(drain(c2, k) == after);
    txn.rollback();  // each run starts from the same rows
  };
  for (auto before : {true, false}) {
    for (auto earlier : {false, true}) {
      for (std::uint64_t k : {1u, 4u, 100u}) {
        INFO("before first fetch " << before << ", earlier writes " << earlier << ", page " << k);
        run(before, earlier, k);
      }
    }
  }
}

TEST_CASE("cursor: a range delete and a write to an index after open leave the cursor's order alone",
          "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_index"};
  auto db = populated(dir);
  auto txn = db->begin();
  const auto& by_name = db->prepare("SELECT id FROM t ORDER BY name, id");
  auto expected = txn.execute(by_name, {}).rows;
  auto c = txn.open_cursor(by_name, {});
  RowVec got = c.fetch(5).rows;
  txn.execute("DELETE FROM t");
  insert(txn, 1000);
  CHECK(txn.execute(by_name, {}).rows.size() == 1);
  auto rest = drain(c, 3);
  got.insert(got.end(), rest.begin(), rest.end());
  CHECK(got == expected);
}

TEST_CASE("cursor: parameters and the clocks are the ones at open", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_pinned"};
  auto db = populated(dir);
  constexpr std::int64_t kDay = 86'400'000'000;
  std::int64_t now = 20'000 * kDay + 5'000'000;
  db->clock_for_testing([&] { return now; });

  auto txn = db->begin();  // its time: now
  const auto& stmt = db->prepare("SELECT id, qty, LOCALTIMESTAMP FROM t WHERE qty >= $1 ORDER BY id");
  std::vector<Value> params{std::int64_t{3}};
  auto expected = txn.execute(stmt, params).rows;
  REQUIRE(expected.size() == 10);
  auto a = txn.open_cursor(stmt, params);
  params[0] = std::int64_t{4};  // the cursor kept its own copy
  auto b = txn.open_cursor(stmt, params);  // the same statement, other parameters, alongside
  now += 3 * kDay;                         // the clock moves on
  auto got = a.fetch(4).rows;
  for (const auto& row : got) CHECK(row.at(2) == Value{20'000 * kDay + 5'000'000});
  auto rest = drain(a, 3);
  got.insert(got.end(), rest.begin(), rest.end());
  CHECK(got == expected);
  auto other = drain(b, 6);
  CHECK(other.size() == 5);
  for (const auto& row : other) {
    CHECK(row.at(1) == Value{std::int64_t{4}});
    CHECK(row.at(2) == Value{20'000 * kDay + 5'000'000});
  }
  txn.rollback();
}

TEST_CASE("cursor: a recompile or a DDL cannot change a live cursor's plan", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_plan"};
  auto db = populated(dir);
  const auto& stmt = db->prepare("SELECT id FROM t WHERE name = $1");
  const std::vector<Value> params{sql_test::bytes("n1")};
  auto before = stmt.plan_source();
  REQUIRE(before.find("t_by_name") != std::string::npos);  // it reads the index
  auto txn = db->begin();
  auto expected = txn.execute(stmt, params).rows;
  REQUIRE(expected.size() > 4);
  auto c = txn.open_cursor(stmt, params);
  auto got = c.fetch(2).rows;
  // Another session drops the index; the statement is refitted to a plan without it.
  db->execute("DROP INDEX t_by_name");
  (void)db->execute(stmt, params);
  REQUIRE(stmt.plan_source() != before);
  auto rest = drain(c, 2);
  got.insert(got.end(), rest.begin(), rest.end());
  CHECK(got == expected);
  txn.rollback();
}

TEST_CASE("cursor: the transaction's end closes its cursors", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_life"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id FROM t");

  SECTION("commit") {
    auto txn = db->begin();
    auto c = txn.open_cursor(scan, {});
    (void)c.fetch(3);
    REQUIRE(c.is_open());
    CHECK(txn.commit() == CommitOutcome::Committed);
    CHECK_FALSE(c.is_open());
    CHECK_THROWS_AS(c.fetch(1), std::logic_error);
    c.close();  // does nothing
  }
  SECTION("rollback, and a destroyed transaction") {
    auto txn = std::make_unique<Transaction>(db->begin());
    auto c = txn->open_cursor(scan, {});
    txn->rollback();
    CHECK_FALSE(c.is_open());
    CHECK_THROWS_AS(c.fetch(1), std::logic_error);
    auto txn2 = std::make_unique<Transaction>(db->begin());
    auto c2 = txn2->open_cursor(scan, {});
    txn2.reset();
    CHECK_FALSE(c2.is_open());
    CHECK_THROWS_AS(c2.fetch(1), std::logic_error);
  }
  SECTION("the database's close") {
    auto txn = db->begin();
    auto c = txn.open_cursor(scan, {});
    db->close();
    CHECK_FALSE(c.is_open());
    CHECK_THROWS_AS(c.fetch(1), DbClosed);
    c.close();
  }
  SECTION("close() ends one cursor and leaves the others and the transaction") {
    auto txn = db->begin();
    auto a = txn.open_cursor(scan, {});
    auto b = txn.open_cursor(scan, {});
    (void)a.fetch(2);
    a.close();
    CHECK_FALSE(a.is_open());
    CHECK_THROWS_AS(a.fetch(1), std::logic_error);
    CHECK(b.fetch(100).rows.size() == static_cast<std::size_t>(kRows));
    CHECK(txn.is_open());
    insert(txn, 500);
    CHECK(txn.commit() == CommitOutcome::Committed);
  }
  SECTION("a moved cursor handle keeps the cursor, the moved-from one is empty") {
    auto txn = db->begin();
    auto a = txn.open_cursor(scan, {});
    auto b = std::move(a);
    CHECK(b.is_open());
    CHECK(b.fetch(2).rows.size() == 2);
    txn.rollback();
  }
}

TEST_CASE("cursor: an error mid-fetch aborts the transaction and closes its cursors", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_error"};
  auto db = populated(dir);
  auto txn = db->begin();
  const auto& fails = db->prepare("SELECT 100 / (qty - 3) FROM t ORDER BY id");  // at id 3
  auto c = txn.open_cursor(fails, {});
  auto other = txn.open_cursor(db->prepare("SELECT id FROM t"), {});
  CHECK(c.fetch(2).rows.size() == 2);
  CHECK_THROWS_AS(c.fetch(5), ExecError);
  CHECK(txn.is_aborted());
  CHECK_FALSE(c.is_open());
  CHECK_FALSE(other.is_open());
  CHECK_THROWS_AS(c.fetch(1), TransactionAborted);
  CHECK_THROWS_AS(other.fetch(1), TransactionAborted);
  CHECK_THROWS_AS(txn.open_cursor(db->prepare("SELECT id FROM t"), {}), TransactionAborted);
  CHECK(txn.commit() == CommitOutcome::RolledBack);
}

TEST_CASE("cursor: only a SELECT, in a usable transaction", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_refused"};
  auto db = populated(dir);
  auto txn = db->begin();
  CHECK_THROWS_AS(txn.open_cursor(db->prepare("DELETE FROM t"), {}), std::invalid_argument);
  CHECK_THROWS_AS(txn.open_cursor(db->prepare("CREATE TABLE z (a BIGINT PRIMARY KEY)"), {}), std::invalid_argument);
  CHECK(txn.is_open());
  CHECK_FALSE(txn.is_aborted());
  // Parameters of the wrong number or type are the statement's error, as at execute.
  CHECK_THROWS_AS(txn.open_cursor(db->prepare("SELECT id FROM t WHERE qty = $1"), {}), ParamError);
  CHECK(txn.is_aborted());
}

namespace {

void insert_wide(Transaction& txn, std::int64_t i) {
  txn.execute("INSERT INTO t VALUES ($1, $2, $3)", {1000 + i, sql_test::bytes(std::string(40, 'x')), i});
}

constexpr std::uint64_t kBound = 64u << 10;

// The most rows of insert_wide a transaction under kBound takes.
auto capacity(Database& db) -> std::int64_t {
  std::int64_t n = 0;
  for (;; ++n) {
    auto txn = db.begin({.max_bytes = kBound});
    try {
      for (std::int64_t i = 0; i <= n; ++i) insert_wide(txn, i);
    } catch (const ExecError&) {
      return n;
    }
    txn.rollback();
    if (n > 5000) FAIL("no bound reached");
  }
}

}  // namespace

TEST_CASE("cursor: its copy of the transaction's writes counts against the transaction's bound", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_cursor_bound"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id FROM t");
  const auto cap = capacity(*db);
  REQUIRE(cap >= 30);

  SECTION("a cursor that cannot reserve is 54000 and aborts") {
    auto txn = db->begin({.max_bytes = kBound});
    for (std::int64_t i = 0; i < cap; ++i) insert_wide(txn, i);  // the bound nearly spent
    try {
      (void)txn.open_cursor(scan, {});
      FAIL("expected a limit error");
    } catch (const ExecError& e) {
      CHECK(e.code() == BC_E_LIMIT);
    }
    CHECK(txn.is_aborted());
  }
  SECTION("an empty overlay reserves nothing, and a closed cursor gives back what it reserved") {
    auto txn = db->begin({.max_bytes = kBound});
    auto early = txn.open_cursor(scan, {});  // no writes yet: free
    for (std::int64_t i = 0; i < cap / 3; ++i) insert_wide(txn, i);
    auto c = txn.open_cursor(scan, {});  // reserves the third
    c.close();
    for (std::int64_t i = cap / 3; i < cap; ++i) insert_wide(txn, i);  // all of the bound again
    CHECK(early.fetch(1000).rows.size() == static_cast<std::size_t>(kRows));
    CHECK(txn.commit() == CommitOutcome::Committed);
  }
  SECTION("a write that would pass the bound because of a held copy fails at the write") {
    auto txn = db->begin({.max_bytes = kBound});
    for (std::int64_t i = 0; i < cap / 3; ++i) insert_wide(txn, i);
    auto c = txn.open_cursor(scan, {});
    auto failed = false;
    try {
      for (std::int64_t i = cap / 3; i < cap; ++i) insert_wide(txn, i);
    } catch (const ExecError& e) {
      failed = e.code() == BC_E_LIMIT;
    }
    CHECK(failed);
    CHECK(txn.is_aborted());
    CHECK_FALSE(c.is_open());
  }
}

// ---------------------------------------------------------------------------
// WITH HOLD (docs/cursor_design.md, *`WITH HOLD`*): a hold cursor owns a clone
// of the snapshot, so it outlives the transaction that declared it.
// ---------------------------------------------------------------------------

namespace {

auto ids(const Rows& r) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  for (const auto& row : r.rows) out.push_back(std::get<std::int64_t>(row[0]));
  return out;
}

auto first_n(std::int64_t n) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  for (std::int64_t i = 1; i <= n; ++i) out.push_back(i);
  return out;
}

// The key directory versions alive: 1 is the published state alone. A
// thread's idle read cache is scraped by stats(), so this waits for it.
auto versions_live(Database& db) -> std::int64_t {
  auto n = std::int64_t{0};
  for (int i = 0; i < 40; ++i) {
    n = db.engine_for_testing().stats().at("bytecask.keydir_versions_live");
    if (n <= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return n;
}

}  // namespace

TEST_CASE("hold cursor: survives the commit of its transaction and shows the DECLARE-time view",
          "[sql][api][txn][cursor][hold]") {
  sql_test::TempDir dir{"api_hold_commit"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id FROM t ORDER BY id");
  const auto all = first_n(kRows);

  SECTION("a read-only transaction") {
    auto txn = db->begin();
    auto c = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK(ids(c.fetch(3)) == first_n(3));
    CHECK(txn.commit() == CommitOutcome::Committed);
    REQUIRE(c.is_open());
    // Other sessions write and commit; the cursor shows none of it.
    db->execute("INSERT INTO t VALUES (100, 'x', 1)");
    db->execute("DELETE FROM t WHERE id = 4");
    db->execute("UPDATE t SET qty = 99 WHERE id = 5");
    auto rest = ids(c.fetch(1000));
    CHECK(rest == std::vector<std::int64_t>(all.begin() + 3, all.end()));
    CHECK(c.at_end());
  }
  SECTION("a transaction that wrote before and after the DECLARE: the overlay is the DECLARE-time one") {
    auto txn = db->begin();
    insert(txn, 200);  // before: in the view
    auto c = txn.open_cursor(scan, {}, Holdability::Hold);
    insert(txn, 300);  // after: not
    txn.execute("DELETE FROM t WHERE id = 1");
    CHECK(txn.commit() == CommitOutcome::Committed);  // moves the transaction's own snapshot into the commit
    auto expected = all;
    expected.push_back(200);
    CHECK(ids(c.fetch(1000)) == expected);
    // The committed writes are in the database, not in the cursor's view.
    CHECK(db->execute("SELECT COUNT(*) FROM t WHERE id IN (200, 300)").rows.size() == 1);
    CHECK(std::get<std::int64_t>(db->execute("SELECT COUNT(*) FROM t WHERE id IN (200, 300)").rows[0][0]) == 2);
  }
  SECTION("fetch in a later transaction and in autocommit") {
    auto txn = db->begin();
    auto c = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK(txn.commit() == CommitOutcome::Committed);
    CHECK(ids(c.fetch(2)) == first_n(2));  // no transaction at all
    auto later = db->begin();
    db->execute("INSERT INTO t VALUES (101, 'x', 1)");
    CHECK(ids(c.fetch(2)) == std::vector<std::int64_t>({3, 4}));
    later.rollback();
    CHECK(c.position() == 4);
  }
  SECTION("a plain cursor is still closed by the commit") {
    auto txn = db->begin();
    auto plain = txn.open_cursor(scan, {});
    auto held = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK(txn.commit() == CommitOutcome::Committed);
    CHECK_FALSE(plain.is_open());
    CHECK(held.is_open());
  }
}

TEST_CASE("hold cursor: parameters and clocks are the ones at DECLARE", "[sql][api][txn][cursor][hold]") {
  sql_test::TempDir dir{"api_hold_params"};
  auto db = populated(dir);
  const auto& stmt = db->prepare("SELECT id FROM t WHERE qty = $1 ORDER BY id");
  auto txn = db->begin();
  auto c = txn.open_cursor(stmt, {std::int64_t{2}}, Holdability::Hold);
  auto other = txn.open_cursor(stmt, {std::int64_t{3}}, Holdability::Hold);
  CHECK(txn.commit() == CommitOutcome::Committed);
  CHECK(ids(c.fetch(100)) == std::vector<std::int64_t>({2, 7, 12, 17, 22}));
  CHECK(ids(other.fetch(100)) == std::vector<std::int64_t>({3, 8, 13, 18, 23}));
}

TEST_CASE("hold cursor: a rollback or abort drops the ones declared in the transaction, not earlier ones",
          "[sql][api][txn][cursor][hold]") {
  sql_test::TempDir dir{"api_hold_rollback"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id FROM t ORDER BY id");

  auto first = db->begin();
  auto earlier = first.open_cursor(scan, {}, Holdability::Hold);
  CHECK(first.commit() == CommitOutcome::Committed);

  SECTION("rollback") {
    auto txn = db->begin();
    auto declared = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK(ids(declared.fetch(2)) == first_n(2));
    txn.rollback();
    CHECK_FALSE(declared.is_open());
    CHECK_THROWS_AS(declared.fetch(1), std::logic_error);
    CHECK(earlier.is_open());
    CHECK(ids(earlier.fetch(2)) == first_n(2));
  }
  SECTION("an abort, and a destroyed transaction") {
    auto txn = db->begin();
    auto declared = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK_THROWS(txn.execute("SELECT nope FROM t"));
    CHECK(txn.is_aborted());
    CHECK_FALSE(declared.is_open());
    CHECK_THROWS_AS(declared.fetch(1), TransactionAborted);
    CHECK(txn.commit() == CommitOutcome::RolledBack);
    CHECK(ids(earlier.fetch(1)) == first_n(1));
    auto gone = std::make_unique<Transaction>(db->begin());
    auto d2 = gone->open_cursor(scan, {}, Holdability::Hold);
    gone.reset();
    CHECK_FALSE(d2.is_open());
  }
  SECTION("an earlier hold cursor fetches while a later transaction is aborted") {
    auto txn = db->begin();
    CHECK_THROWS(txn.execute("SELECT nope FROM t"));
    CHECK(ids(earlier.fetch(1)) == first_n(1));
  }
}

TEST_CASE("hold cursor: close, the database's close and a failed fetch release it", "[sql][api][txn][cursor][hold]") {
  sql_test::TempDir dir{"api_hold_close"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id FROM t ORDER BY id");

  SECTION("close() releases the snapshot it pins") {
    REQUIRE(versions_live(*db) == 1);
    auto txn = db->begin();
    auto c = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK(txn.commit() == CommitOutcome::Committed);
    db->execute("INSERT INTO t VALUES (100, 'x', 1)");  // the published state moves on; the cursor's version stays
    CHECK(db->engine_for_testing().stats().at("bytecask.keydir_versions_live") >= 2);
    CHECK(versions_live(*db) >= 2);
    c.close();
    CHECK_FALSE(c.is_open());
    CHECK_THROWS_AS(c.fetch(1), std::logic_error);
    CHECK(versions_live(*db) == 1);
  }
  SECTION("the database's close releases it, and a fetch after is DbClosed") {
    auto txn = db->begin();
    auto c = txn.open_cursor(scan, {}, Holdability::Hold);
    CHECK(txn.commit() == CommitOutcome::Committed);
    db->close();
    CHECK_FALSE(c.is_open());
    CHECK_THROWS_AS(c.fetch(1), DbClosed);
    c.close();
  }
  SECTION("a dropped handle leaves the cursor open until the database closes") {
    {
      auto txn = db->begin();
      (void)txn.open_cursor(scan, {}, Holdability::Hold);
      CHECK(txn.commit() == CommitOutcome::Committed);
    }
    db->execute("INSERT INTO t VALUES (100, 'x', 1)");
    CHECK(versions_live(*db) >= 2);
    db->close();
  }
}

TEST_CASE("hold cursor: its overlay copy is charged to the transaction until it commits", "[sql][api][txn][cursor][hold]") {
  sql_test::TempDir dir{"api_hold_bound"};
  auto db = populated(dir);
  const auto& scan = db->prepare("SELECT id FROM t");
  const auto cap = capacity(*db);
  REQUIRE(cap >= 30);
  auto txn = db->begin({.max_bytes = kBound});
  for (std::int64_t i = 0; i < cap; ++i) insert_wide(txn, i);  // the bound nearly spent
  try {
    (void)txn.open_cursor(scan, {}, Holdability::Hold);
    FAIL("expected a limit error");
  } catch (const ExecError& e) {
    CHECK(e.code() == BC_E_LIMIT);
  }
  CHECK(txn.is_aborted());
}
