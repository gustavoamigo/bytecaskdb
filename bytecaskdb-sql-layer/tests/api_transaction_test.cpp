// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The embedded API's transactions (docs/transaction_design.md, *Shape*):
// Database::begin's handle runs statements that read the transaction's own
// earlier writes, commits them as one batch or nothing, aborts on a failed
// statement with 25P02 after it, reports a conflict at commit as a
// serialization failure (40001), bounds its buffer (54000), rolls back when
// destroyed and is ended by close(); and handles on different threads run
// concurrently.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

namespace {

auto open_db(const sql_test::TempDir& dir) -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE acct (id BIGINT PRIMARY KEY, bal BIGINT NOT NULL)");
  return db;
}

auto balance(Database& db, std::int64_t id) -> std::optional<std::int64_t> {
  auto r = db.execute("SELECT bal FROM acct WHERE id = $1", {id});
  if (r.rows.empty()) return std::nullopt;
  return std::get<std::int64_t>(r.rows[0][0]);
}

auto txn_balance(Transaction& t, std::int64_t id) -> std::optional<std::int64_t> {
  auto r = t.execute("SELECT bal FROM acct WHERE id = $1", {id});
  if (r.rows.empty()) return std::nullopt;
  return std::get<std::int64_t>(r.rows[0][0]);
}

}  // namespace

TEST_CASE("embedded API: a transaction reads its own writes and commits them as one batch", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  auto t = db->begin();
  t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{0}});
  t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{60}});
  t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{2}, std::int64_t{40}});
  CHECK(txn_balance(t, 1) == 60);
  CHECK(txn_balance(t, 2) == 40);
  // Nothing is visible outside before the commit.
  CHECK(balance(*db, 1) == 100);
  CHECK(balance(*db, 2) == std::nullopt);
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK_FALSE(t.is_open());
  CHECK(balance(*db, 1) == 60);
  CHECK(balance(*db, 2) == 40);
  CHECK_THROWS_AS(t.execute("SELECT bal FROM acct WHERE id = $1", {std::int64_t{1}}), std::logic_error);
}

TEST_CASE("embedded API: a failed statement aborts the transaction: 25P02 until it ends, nothing written",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  auto t = db->begin();
  t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{1}});
  CHECK_THROWS_AS(t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{2}}), ExecError);
  CHECK(t.is_aborted());
  CHECK_THROWS_AS(t.execute("SELECT bal FROM acct WHERE id = $1", {std::int64_t{1}}), TransactionAborted);
  // commit() rolls it back, as PostgreSQL's COMMIT does, and says so.
  CHECK(t.commit() == CommitOutcome::RolledBack);
  CHECK_FALSE(t.is_open());
  CHECK(balance(*db, 1) == std::nullopt);
  // A statement refused before it ran aborts it too.
  auto u = db->begin();
  CHECK_THROWS_AS(u.execute("SELECT bal FROM acct WHERE id = $1", std::vector<Value>{sql_test::bytes("x")}), ParamError);
  CHECK_THROWS_AS(u.execute("SELECT bal FROM acct WHERE id = $1", {std::int64_t{1}}), TransactionAborted);
  u.rollback();
  CHECK_FALSE(u.is_open());
}

TEST_CASE("embedded API: a statement cached before a DDL is recompiled for a transaction", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  CHECK(balance(*db, 1) == 100);  // cached for this catalog version
  db->execute("DROP TABLE acct");
  db->execute("CREATE TABLE acct (id BIGINT PRIMARY KEY, bal BIGINT NOT NULL)");
  auto t = db->begin();
  t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{7}});
  CHECK(txn_balance(t, 1) == 7);
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(balance(*db, 1) == 7);
}

TEST_CASE("embedded API: in an aborted transaction a statement that does not parse is a syntax error, not 25P02",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  auto t = db->begin();
  CHECK_THROWS_AS(t.execute("SELECT bal FROM nope WHERE id = $1", {std::int64_t{1}}), ast::SqlError);
  REQUIRE(t.is_aborted());
  CHECK_THROWS_AS(t.execute("SELEC bal"), ast::SqlError);
  CHECK_THROWS_AS(t.execute("SELECT bal FROM nope WHERE id = $1", {std::int64_t{1}}), TransactionAborted);
  CHECK(t.is_aborted());
}

TEST_CASE("embedded API: a conflict at commit is a serialization failure, and nothing is written", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  auto a = db->begin();
  auto b = db->begin();
  a.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{90}});
  a.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{10}});
  b.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{50}});
  CHECK(b.commit() == CommitOutcome::Committed);
  try {
    (void)a.commit();
    FAIL("the second commit did not conflict");
  } catch (const SerializationFailure& e) {
    CHECK(e.conflict());
  }
  CHECK_FALSE(a.is_open());
  CHECK(balance(*db, 1) == 50);
  CHECK(balance(*db, 2) == std::nullopt);
}

TEST_CASE("embedded API: a transaction past its buffer bound is BC_E_LIMIT and aborted", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  auto t = db->begin({.max_bytes = 512});
  auto refused = false;
  for (std::int64_t i = 0; i < 100 && !refused; ++i) {
    try {
      t.execute("INSERT INTO acct VALUES ($1, $2)", {i, i});
    } catch (const ExecError& e) {
      CHECK(e.code() == BC_E_LIMIT);
      refused = true;
    }
  }
  CHECK(refused);
  CHECK(t.is_aborted());
  t.rollback();
  CHECK(balance(*db, 0) == std::nullopt);
}

TEST_CASE("embedded API: destroying an open transaction rolls it back; close() ends the open ones",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  {
    auto t = db->begin();
    t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{1}});
  }
  CHECK(balance(*db, 1) == std::nullopt);
  auto t = db->begin();
  t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{1}});
  auto moved = std::move(t);  // the handle moves; the transaction is the same
  CHECK_FALSE(t.is_open());
  CHECK(moved.is_open());
  db->close();
  CHECK_FALSE(moved.is_open());
  CHECK_THROWS_AS((void)moved.commit(), DbClosed);
  CHECK_THROWS_AS(db->begin(), DbClosed);
  moved.rollback();  // ended already: nothing to do
  auto again = Database::open(dir.path());
  CHECK(balance(*again, 1) == std::nullopt);
}

TEST_CASE("embedded API: transactions on different threads run concurrently and lose no update", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  constexpr int kThreads = 8, kEach = 25;
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{0}, std::int64_t{0}});
  for (std::int64_t i = 1; i <= kThreads; ++i) db->execute("INSERT INTO acct VALUES ($1, $2)", {i, std::int64_t{0}});
  // Prepared once, before the threads: a transaction's statements only read
  // the prepared statement.
  const auto& read = db->prepare("SELECT bal FROM acct WHERE id = $1");
  const auto& write = db->prepare("UPDATE acct SET bal = $2 WHERE id = $1");
  std::atomic<int> conflicts{0};
  std::vector<std::thread> threads;
  for (int n = 1; n <= kThreads; ++n) {
    threads.emplace_back([&, n] {
      for (int done = 0; done < kEach;) {
        auto t = db->begin();
        // Each moves one unit from the shared row 0's counter onto its own.
        auto shared = std::get<std::int64_t>(t.execute(read, {std::int64_t{0}}).rows.at(0).at(0));
        auto own = std::get<std::int64_t>(t.execute(read, {std::int64_t{n}}).rows.at(0).at(0));
        t.execute(write, {std::int64_t{0}, shared + 1});
        t.execute(write, {std::int64_t{n}, own + 1});
        try {
          if (t.commit() == CommitOutcome::Committed) ++done;
        } catch (const SerializationFailure&) {
          ++conflicts;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(balance(*db, 0) == kThreads * kEach);
  for (std::int64_t i = 1; i <= kThreads; ++i) CHECK(balance(*db, i) == kEach);
  INFO("conflicts " << conflicts.load());
}

TEST_CASE("embedded API: a moved-from transaction has ended", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  auto t = db->begin();
  auto u = std::move(t);
  CHECK_FALSE(t.is_open());  // NOLINT(bugprone-use-after-move): what a moved-from handle does is the test
  CHECK_FALSE(t.is_aborted());
  CHECK_THROWS_AS(t.execute("SELECT bal FROM acct WHERE id = $1", {std::int64_t{1}}), std::logic_error);
  CHECK_THROWS_AS((void)t.commit(), std::logic_error);
  t.rollback();
  u.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{1}});
  t = std::move(u);  // u is moved from now, t the open one
  CHECK(t.commit() == CommitOutcome::Committed);
  CHECK(balance(*db, 1) == 1);
}

TEST_CASE("embedded API: a transaction may outlive its database, and close() racing it ends it", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  std::optional<Transaction> t;
  {
    auto db = open_db(dir);
    t.emplace(db->begin());
    t->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{1}});
  }  // the database is destroyed, and closed, with the transaction open
  CHECK_FALSE(t->is_open());
  CHECK_THROWS_AS(t->execute("SELECT bal FROM acct WHERE id = $1", {std::int64_t{1}}), DbClosed);
  t->rollback();
  t.reset();
  auto db = Database::open(dir.path());
  CHECK(balance(*db, 1) == std::nullopt);
  // close() from another thread while a transaction runs statements: each
  // statement either runs or is refused, and the transaction ends, never
  // left looking open.
  auto u = db->begin();
  std::atomic<bool> started{false};
  std::thread worker{[&] {
    for (std::int64_t i = 0;; ++i) {
      try {
        (void)u.execute("SELECT bal FROM acct WHERE id = $1", {i});
        started = true;
      } catch (const DbClosed&) {
        return;
      } catch (const std::logic_error&) {
        return;
      }
    }
  }};
  while (!started) std::this_thread::yield();
  db->close();
  worker.join();
  CHECK_FALSE(u.is_open());
}

TEST_CASE("embedded API: a statement that no longer fits the catalog a transaction pinned is 40001",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  auto t = db->begin();
  CHECK(txn_balance(t, 1) == 100);  // the first statement pins the catalog
  db->execute("DROP TABLE acct");
  db->execute("CREATE TABLE acct (id BIGINT PRIMARY KEY, bal BIGINT NOT NULL)");
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{7}});
  CHECK(balance(*db, 1) == 7);  // the shared statement, recompiled for the live catalog
  // The pinned catalog is older than the statement's plan: the transaction
  // fails as its commit would, and ends; the statement is not recompiled
  // back for it.
  CHECK_THROWS_AS(t.execute("SELECT bal FROM acct WHERE id = $1", {std::int64_t{1}}), SerializationFailure);
  CHECK_FALSE(t.is_open());
  auto compiled = db->compilations();
  auto u = db->begin();
  CHECK(txn_balance(u, 1) == 7);
  CHECK(u.commit() == CommitOutcome::Committed);
  CHECK(db->compilations() == compiled);
}

TEST_CASE("embedded API: transactions commit concurrently while other threads prepare and run autocommit statements",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  constexpr int kCommitters = 4, kEach = 30, kPrepares = 40;
  for (std::int64_t i = 0; i < kCommitters; ++i) db->execute("INSERT INTO acct VALUES ($1, $2)", {i, std::int64_t{0}});
  std::atomic<int> done{0};
  std::vector<std::thread> threads;
  for (std::int64_t n = 0; n < kCommitters; ++n) {
    threads.emplace_back([&, n] {
      for (int i = 0; i < kEach; ++i) {
        auto t = db->begin();
        auto bal = txn_balance(t, n).value();
        t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {n, bal + 1});
        // Each writes its own row: none conflicts.
        CHECK(t.commit() == CommitOutcome::Committed);
      }
      ++done;
    });
  }
  threads.emplace_back([&] {
    // Distinct texts: each prepare is a cache miss and a compile.
    for (int i = 0; i < kPrepares; ++i) {
      (void)db->prepare("SELECT bal FROM acct WHERE id = $1" + std::string(static_cast<std::size_t>(i) + 1, ' '));
    }
  });
  threads.emplace_back([&] {
    while (done.load() < kCommitters) (void)balance(*db, 0);
  });
  for (auto& th : threads) th.join();
  for (std::int64_t i = 0; i < kCommitters; ++i) CHECK(balance(*db, i) == kEach);
}

TEST_CASE("embedded API: an allocation failure in commit() never changes its outcome", "[sql][api][txn][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  // A conflicting commit, then one that would succeed: each allocation of the
  // commit failed in turn.
  for (auto conflicting : {true, false}) {
    INFO((conflicting ? "conflicting" : "succeeding") << " commit");
    auto unknowns = 0;
    for (long nth = 1;; ++nth) {
      sql_test::TempDir dir{"api_txn"};
      std::optional<std::int64_t> stored;
      std::string outcome;
      auto fired = false;
      {
        auto db = open_db(dir);
        db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
        auto a = db->begin();
        a.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{90}});
        if (conflicting) {
          auto b = db->begin();
          b.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{50}});
          REQUIRE(b.commit() == CommitOutcome::Committed);
        }
        {
          bytecask::testing::ScopedAllocFaults faults{nth, false};
          try {
            (void)a.commit();
            outcome = "committed";
          } catch (const SerializationFailure&) {
            outcome = "conflict";
          } catch (const TransactionResolutionUnknown&) {
            outcome = "unknown";
          } catch (const ExecError& e) {
            outcome = e.code() == BC_E_MEMORY ? "memory" : "error";
          } catch (const std::bad_alloc&) {
            outcome = "bad_alloc";
          }
          fired = faults.report().fired;
        }
        INFO("allocation " << nth << ": " << outcome);
        CHECK_FALSE(a.is_open());
        // Unknown exactly when the failure degraded the engine: the batch
        // may be in the data file.
        CHECK(db->engine_for_testing().is_degraded() == (outcome == "unknown"));
        if (outcome == "unknown") {
          CHECK_FALSE(conflicting);  // a conflict is found before anything is written
        } else {
          CHECK((outcome == (conflicting ? "conflict" : "committed") || outcome == "memory"));
          stored = balance(*db, 1);
        }
      }
      if (outcome == "unknown") {
        // Whichever way it went, it went whole, as a reopen finds it.
        auto db = Database::open(dir.path());
        auto bal = balance(*db, 1);
        CHECK((bal == 100 || bal == 90));
        ++unknowns;
      } else {
        CHECK(stored == (outcome == "committed" ? 90 : conflicting ? 50 : 100));
      }
      if (!fired) break;
    }
    // The sweep reaches failures after the batch is in the data file.
    if (!conflicting) CHECK(unknowns > 0);
  }
}

TEST_CASE("embedded API: an allocation failure while an autocommit error is reported never changes it",
          "[sql][api][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  const auto& insert = db->prepare("INSERT INTO acct VALUES ($1, $2)");
  const auto& update = db->prepare("UPDATE acct SET bal = $2 WHERE id = $1");
  // A duplicate key, and a conflict on every retry: every allocation of the
  // call failed in turn. The error is the call's own, or BC_E_MEMORY, and
  // nothing is written. std::bad_alloc only from the call's first
  // allocations, the parameters' copy, before anything runs: never once the
  // plan has run, when it would hide the outcome.
  const std::vector<Value> params{std::int64_t{1}, std::int64_t{7}};  // built before the faults are armed
  for (auto conflict : {false, true}) {
    db->conflict_on_every_commit_for_testing(conflict);
    auto ran = false;
    for (long nth = 1;; ++nth) {
      std::string outcome;
      auto fired = false;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        try {
          (void)db->execute(conflict ? update : insert, params);
          outcome = "ran";
        } catch (const ExecError& e) {
          outcome = e.conflict() ? "conflict" : e.code() == BC_E_MEMORY ? "memory" : e.code() == BC_E_CONSTRAINT ? "duplicate" : "error";
        } catch (const std::bad_alloc&) {
          outcome = "bad_alloc";
        }
        fired = faults.report().fired;
      }
      INFO((conflict ? "conflict" : "duplicate") << ", allocation " << nth << ": " << outcome);
      if (db->engine_for_testing().is_degraded()) {
        // The test's own conflict hook writes; a failed allocation in that
        // write degrades the engine, which is not the call under test.
        db->engine_for_testing().resume();
        if (!fired) break;
        continue;
      }
      if (outcome == "bad_alloc") {
        CHECK_FALSE(ran);
      } else {
        ran = true;
        CHECK((outcome == (conflict ? "conflict" : "duplicate") || outcome == "memory"));
      }
      if (!fired) break;
    }
  }
  db->conflict_on_every_commit_for_testing(false);
  CHECK(balance(*db, 1) == 100);
}

TEST_CASE("embedded API: a statement runs only on the database that prepared it", "[sql][api][txn]") {
  sql_test::TempDir d1{"api_txn"}, d2{"api_txn"};
  auto db1 = open_db(d1);
  auto db2 = open_db(d2);
  const auto& stmt = db1->prepare("SELECT bal FROM acct WHERE id = $1");
  auto cached = db2->cached_plans();
  CHECK_THROWS_AS(db2->execute(stmt, {std::int64_t{1}}), std::logic_error);
  auto t = db2->begin();
  CHECK_THROWS_AS(t.execute(stmt, {std::int64_t{1}}), std::logic_error);
  CHECK(t.is_open());
  CHECK_FALSE(t.is_aborted());  // a misuse of the handle, not a failed statement
  CHECK(db2->cached_plans() == cached);
  CHECK(t.commit() == CommitOutcome::Committed);
}

TEST_CASE("embedded API: a DDL that finds another in progress is 40001 at once", "[sql][api]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  std::string second;
  // The first DDL is held mid-commit while a second runs on another thread.
  db->live_catalog_for_testing().set_commit_hook_for_testing([&] {
    std::thread other{[&] {
      try {
        db->execute("CREATE TABLE other (id BIGINT PRIMARY KEY)");
        second = "ran";
      } catch (const SerializationFailure&) {
        second = "40001";
      }
    }};
    other.join();
  });
  db->execute("CREATE INDEX acct_bal ON acct (bal)");
  db->live_catalog_for_testing().set_commit_hook_for_testing({});
  CHECK(second == "40001");
  CHECK(db->catalog()->table("other") == nullptr);
  db->execute("CREATE TABLE other (id BIGINT PRIMARY KEY)");  // retried, it runs
  CHECK(db->catalog()->table("other") != nullptr);
}

TEST_CASE("embedded API: a DDL racing a transaction's first statement re-pins it, not 40001", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  CHECK(balance(*db, 1) == 100);
  // Right after the first statement pins the catalog, a DDL replaces the
  // table and the shared statement is recompiled for it: the pin is older
  // than the plan, and the statement has read nothing.
  auto raced = false;
  db->after_txn_pin_for_testing([&] {
    if (raced) return;
    raced = true;
    std::thread other{[&] {
      db->execute("DROP TABLE acct");
      db->execute("CREATE TABLE acct (id BIGINT PRIMARY KEY, bal BIGINT NOT NULL)");
      db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{7}});
      CHECK(balance(*db, 1) == 7);
    }};
    other.join();
  });
  auto t = db->begin();
  CHECK(txn_balance(t, 1) == 7);  // pinned again, on the live catalog and a new snapshot
  db->after_txn_pin_for_testing({});
  CHECK(raced);
  CHECK(t.commit() == CommitOutcome::Committed);
}
