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

TEST_CASE("embedded API: an allocation failure in an autocommit statement never changes its outcome",
          "[sql][api][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{100}});
  const auto& insert = db->prepare("INSERT INTO acct VALUES ($1, $2)");
  const auto& update = db->prepare("UPDATE acct SET bal = $2 WHERE id = $1");
  // A duplicate key, a conflict on every retry, and a new row: every
  // allocation of the call failed in turn. The outcome is the call's own,
  // BC_E_MEMORY with nothing written, or, exactly when the failure left the
  // engine degraded after the batch may have reached it, 08007.
  // std::bad_alloc only from the call's first allocations, the parameters'
  // copy, before anything runs: never once the plan has run, when it would
  // hide the outcome.
  enum class Case { Duplicate, Conflict, Insert };
  for (auto which : {Case::Duplicate, Case::Conflict, Case::Insert}) {
    db->conflict_on_every_commit_for_testing(which == Case::Conflict);
    auto ran = false;
    auto unknowns = 0;
    for (long nth = 1;; ++nth) {
      // Built before the faults are armed; each insert's row its own.
      const std::vector<Value> params{which == Case::Insert ? std::int64_t{1000} + nth : std::int64_t{1},
                                      std::int64_t{7}};
      std::string outcome;
      auto fired = false;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        try {
          (void)db->execute(which == Case::Conflict ? update : insert, params);
          outcome = "ran";
        } catch (const TransactionResolutionUnknown&) {
          outcome = "unknown";
        } catch (const ExecError& e) {
          outcome = e.conflict()                     ? "conflict"
                    : e.code() == BC_E_MEMORY        ? "memory"
                    : e.code() == BC_E_CONSTRAINT    ? "duplicate"
                    : e.code() == BC_E_DEGRADED      ? "degraded"
                                                     : "error";
        } catch (const std::bad_alloc&) {
          outcome = "bad_alloc";
        }
        fired = faults.report().fired;
      }
      INFO("case " << static_cast<int>(which) << ", allocation " << nth << ": " << outcome);
      auto degraded = db->engine_for_testing().is_degraded();
      if (outcome == "bad_alloc") {
        CHECK_FALSE(ran);
      } else {
        ran = true;
        switch (which) {
          case Case::Duplicate:
            CHECK((outcome == "duplicate" || outcome == "memory"));
            CHECK_FALSE(degraded);
            break;
          case Case::Conflict:
            // The test's own conflict hook writes before the commit: a
            // failed allocation there can degrade the engine, and the
            // commit is then refused, with nothing written; never unknown.
            CHECK((outcome == "conflict" || outcome == "memory" || (degraded && outcome == "degraded")));
            break;
          case Case::Insert:
            CHECK((outcome == "ran" || outcome == "memory" || outcome == "unknown"));
            CHECK(degraded == (outcome == "unknown"));
            if (outcome == "memory") CHECK(balance(*db, std::int64_t{1000} + nth) == std::nullopt);
            if (outcome == "unknown") ++unknowns;
            break;
        }
      }
      if (degraded) db->engine_for_testing().resume();
      if (!fired) break;
    }
    if (which == Case::Insert) CHECK(unknowns > 0);  // the sweep reaches failures after the append
  }
  db->conflict_on_every_commit_for_testing(false);
  CHECK(balance(*db, 1) == 100);
}

TEST_CASE("embedded API: a DDL whose commit may have reached the engine is 08007", "[sql][api][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation faults in this build");
  auto unknowns = 0;
  for (long nth = 1;; ++nth) {
    sql_test::TempDir dir{"api_ddl_unknown"};
    std::string outcome;
    auto fired = false;
    {
      auto db = open_db(dir);
      for (std::int64_t i = 0; i < 8; ++i) db->execute("INSERT INTO acct VALUES ($1, $2)", {i, i});
      const auto& ddl = db->prepare("CREATE INDEX acct_bal ON acct (bal)");
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        try {
          (void)db->execute(ddl, {});
          outcome = "ran";
        } catch (const TransactionResolutionUnknown&) {
          outcome = "unknown";
        } catch (const std::bad_alloc&) {
          outcome = "failed";
        } catch (const std::exception&) {
          outcome = "failed";
        }
        fired = faults.report().fired;
      }
      INFO("allocation " << nth << ": " << outcome);
      CHECK(db->engine_for_testing().is_degraded() == (outcome == "unknown"));
      if (outcome == "unknown") ++unknowns;
    }
    // Whichever way it went, it went whole: the index is there or not, the
    // rows as they were. A failure that was not 08007 wrote nothing.
    auto db = Database::open(dir.path());
    auto has_index = db->catalog()->index("acct_bal").second != nullptr;
    if (outcome == "ran") CHECK(has_index);
    if (outcome == "failed") CHECK_FALSE(has_index);
    for (std::int64_t i = 0; i < 8; ++i) CHECK(balance(*db, i) == i);
    if (!fired) break;
  }
  CHECK(unknowns > 0);
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

TEST_CASE("embedded API: a key a transaction inserted that a concurrent commit took is 23505 at commit unless an earlier statement's row changed",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("CREATE TABLE person (id BIGINT PRIMARY KEY, email VARCHAR(32))");
  db->execute("CREATE UNIQUE INDEX person_email ON person (email)");
  auto cat = db->catalog();
  auto acct_id = cat->table("acct")->id;
  auto email_id = cat->index("person_email").second->id;
  auto duplicate = [](Transaction& t) -> std::optional<ConstraintDetail> {
    try {
      (void)t.commit();
    } catch (const SerializationFailure&) {
      return std::nullopt;
    } catch (const ExecError& e) {
      CHECK_FALSE(e.conflict());
      CHECK(e.code() == BC_E_CONSTRAINT);
      CHECK_FALSE(t.is_open());
      return e.constraint();
    }
    FAIL("the second commit did not fail");
    return std::nullopt;
  };
  SECTION("a primary key: the error names the table") {
    auto a = db->begin();
    auto b = db->begin();
    a.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
    b.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{20}});
    CHECK(b.commit() == CommitOutcome::Committed);
    auto d = duplicate(a);
    REQUIRE(d);
    CHECK(d->constraint == BC_C_UNIQUE);
    CHECK(d->object_id == acct_id);
    CHECK(balance(*db, 1) == 20);
  }
  SECTION("a unique index value, under another key: the error names the index") {
    auto a = db->begin();
    auto b = db->begin();
    a.execute("INSERT INTO person VALUES ($1, $2)", {std::int64_t{1}, sql_test::bytes("x@y")});
    b.execute("INSERT INTO person VALUES ($1, $2)", {std::int64_t{2}, sql_test::bytes("x@y")});
    CHECK(b.commit() == CommitOutcome::Committed);
    auto d = duplicate(a);
    REQUIRE(d);
    CHECK(d->constraint == BC_C_UNIQUE);
    CHECK(d->object_id == email_id);
    CHECK(db->table_rows("person").rows.size() == 1);
  }
  SECTION("an autocommit insert raced the same way reports the same") {
    auto b = db->begin();
    b.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{20}});
    CHECK(b.commit() == CommitOutcome::Committed);
    try {
      (void)db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
      FAIL("the duplicate committed");
    } catch (const ExecError& e) {
      REQUIRE(e.constraint());
      CHECK(e.constraint()->constraint == BC_C_UNIQUE);
      CHECK(e.constraint()->object_id == acct_id);
    }
  }
  SECTION("a conflict on a row the transaction updated, not inserted, is 40001") {
    db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
    auto a = db->begin();
    a.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{11}});
    a.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{0}});
    db->execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{12}});
    CHECK_FALSE(duplicate(a));
    CHECK(balance(*db, 2) == std::nullopt);
  }
  // New-Order's race (#552): PostgreSQL's second transaction stops at the
  // row the first changed, before its INSERT, and never reports 23505.
  auto take = [&](Transaction& t, std::int64_t key, bool for_update) {
    if (for_update) (void)t.execute("SELECT bal FROM acct WHERE id = $1 FOR UPDATE", {std::int64_t{1}});
    t.execute("UPDATE acct SET bal = bal + $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{1}});
    t.execute("INSERT INTO acct VALUES ($1, $2)", {key, std::int64_t{0}});
  };
  SECTION("a row an earlier statement locked or wrote, changed by the commit that took the key: 40001") {
    for (auto for_update : {true, false}) {
      db->execute("DELETE FROM acct WHERE id = $1", {std::int64_t{2}});
      db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
      auto a = db->begin();
      auto b = db->begin();
      take(a, 2, for_update);
      take(b, 2, for_update);
      CHECK(b.commit() == CommitOutcome::Committed);
      CHECK_FALSE(duplicate(a));
      CHECK(balance(*db, 1) == 11);
      db->execute("DELETE FROM acct WHERE id = $1", {std::int64_t{1}});
    }
  }
  SECTION("an earlier locked row a concurrent commit left alone: still 23505") {
    db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
    auto a = db->begin();
    (void)a.execute("SELECT bal FROM acct WHERE id = $1 FOR UPDATE", {std::int64_t{1}});
    a.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{0}});
    db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{5}});
    auto d = duplicate(a);
    REQUIRE(d);
    CHECK(d->object_id == acct_id);
  }
  SECTION("an insert before the changed row: the duplicate comes first, 23505") {
    db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
    auto a = db->begin();
    a.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{0}});
    a.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{11}});
    auto b = db->begin();
    take(b, 2, false);
    CHECK(b.commit() == CommitOutcome::Committed);
    auto d = duplicate(a);
    REQUIRE(d);
    CHECK(d->object_id == acct_id);
  }
  SECTION("a unique value a later statement moved away claims nothing: not 23505") {
    auto a = db->begin();
    a.execute("INSERT INTO person VALUES ($1, $2)", {std::int64_t{1}, sql_test::bytes("x@y")});
    a.execute("UPDATE person SET email = $2 WHERE id = $1", {std::int64_t{1}, sql_test::bytes("z@w")});
    db->execute("INSERT INTO person VALUES ($1, $2)", {std::int64_t{2}, sql_test::bytes("x@y")});
    // The first statement's check of "x@y" still guards it, so the commit
    // conflicts (#537); what it is not is a duplicate.
    CHECK_FALSE(duplicate(a));
  }
  SECTION("two updates of one row to one unique value: the conflict on the row, 40001") {
    db->execute("INSERT INTO person VALUES ($1, $2)", {std::int64_t{1}, sql_test::bytes("a@b")});
    auto a = db->begin();
    a.execute("UPDATE person SET email = $2 WHERE id = $1", {std::int64_t{1}, sql_test::bytes("x@y")});
    db->execute("UPDATE person SET email = $2 WHERE id = $1", {std::int64_t{1}, sql_test::bytes("x@y")});
    // The entry that took "x@y" is the row's own: not a duplicate.
    CHECK_FALSE(duplicate(a));
  }
  SECTION("deleting and reinserting a row a concurrent commit updates is 40001") {
    db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
    auto a = db->begin();
    a.execute("DELETE FROM acct WHERE id = $1", {std::int64_t{1}});
    a.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{99}});
    db->execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{12}});
    CHECK_FALSE(duplicate(a));
    CHECK(balance(*db, 1) == 12);
  }
}

TEST_CASE("embedded API: DDL given to a transaction is 0A000 and aborts it", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  auto t = db->begin();
  t.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
  CHECK_THROWS_AS(t.execute("CREATE INDEX acct_bal ON acct (bal)"), FeatureNotSupported);
  CHECK(t.is_aborted());
  CHECK(db->catalog()->index("acct_bal").second == nullptr);
  CHECK(t.commit() == CommitOutcome::RolledBack);
  CHECK(balance(*db, 1) == std::nullopt);
}

TEST_CASE("embedded API: a DDL on a table an open transaction wrote makes its commit 40001; one it read does not",
          "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
  db->execute("CREATE TABLE other (id BIGINT PRIMARY KEY)");
  auto wrote = db->begin();
  auto read = db->begin();
  wrote.execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{20}});
  CHECK(txn_balance(read, 1) == 10);
  read.execute("INSERT INTO other VALUES ($1)", {std::int64_t{1}});
  // The DDL commits at once, with both transactions open.
  db->execute("CREATE INDEX acct_bal ON acct (bal)");
  CHECK_THROWS_AS(wrote.commit(), SerializationFailure);
  CHECK(read.commit() == CommitOutcome::Committed);
  // A reader that took FOR UPDATE rows a DDL then rewrote conflicts:
  // PostgreSQL would make the DDL wait for its row locks; never waiting, the
  // reader gets 40001 instead.
  auto locked = db->begin();
  CHECK(locked.execute("SELECT bal FROM acct WHERE id = $1 FOR UPDATE", {std::int64_t{1}}).rows.size() == 1);
  db->execute("ALTER TABLE acct ADD COLUMN tag BIGINT DEFAULT 5");
  CHECK_THROWS_AS(locked.commit(), SerializationFailure);
  CHECK(balance(*db, 2) == std::nullopt);
  CHECK(db->table_rows("other").rows.size() == 1);
  // The index holds every row: the one the conflicting transaction wrote
  // never reached the table without its entry.
  CHECK(db->execute("SELECT id FROM acct WHERE bal = $1", {std::int64_t{10}}).rows.size() == 1);
}

TEST_CASE("embedded API: SELECT … FOR UPDATE makes the commit 40001 if a row it returned changed", "[sql][api][txn]") {
  sql_test::TempDir dir{"api_txn"};
  auto db = open_db(dir);
  db->execute("CREATE INDEX acct_bal ON acct (bal)");
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{10}});
  db->execute("INSERT INTO acct VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{20}});
  constexpr auto kLock = "SELECT bal FROM acct WHERE id = $1 FOR UPDATE";
  // Write skew: each reads a row the other writes. Without FOR UPDATE both commit.
  SECTION("without FOR UPDATE, a read row changed by another commit is not a conflict") {
    auto t = db->begin();
    CHECK(txn_balance(t, 1) == 10);
    t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{2}, std::int64_t{21}});
    db->execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{11}});
    CHECK(t.commit() == CommitOutcome::Committed);
    CHECK(balance(*db, 2) == 21);
  }
  SECTION("with FOR UPDATE on the primary key, it is") {
    auto t = db->begin();
    CHECK(t.execute(kLock, {std::int64_t{1}}).rows.size() == 1);
    t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{2}, std::int64_t{21}});
    db->execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{11}});
    CHECK_THROWS_AS(t.commit(), SerializationFailure);
    CHECK(balance(*db, 2) == 20);
  }
  SECTION("with FOR UPDATE over an index range, a deleted row is a change too") {
    auto t = db->begin();
    CHECK(t.execute("SELECT id FROM acct WHERE bal >= $1 FOR UPDATE", {std::int64_t{15}}).rows.size() == 1);
    t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{11}});
    db->execute("DELETE FROM acct WHERE id = $1", {std::int64_t{2}});
    CHECK_THROWS_AS(t.commit(), SerializationFailure);
    CHECK(balance(*db, 1) == 10);
  }
  SECTION("a transaction that only read FOR UPDATE still conflicts on a row it returned") {
    // In PostgreSQL the other writer would wait for this one; never waiting,
    // the writer commits and this transaction gets the conflict.
    auto t = db->begin();
    CHECK(t.execute(kLock, {std::int64_t{1}}).rows.size() == 1);
    db->execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{11}});
    CHECK_THROWS_AS(t.commit(), SerializationFailure);
    // A transaction that read the row unchanged commits, writing nothing.
    auto u = db->begin();
    CHECK(u.execute(kLock, {std::int64_t{1}}).rows.size() == 1);
    CHECK(u.commit() == CommitOutcome::Committed);
    CHECK(balance(*db, 1) == 11);
  }
  SECTION("on a follower: an autocommit FOR UPDATE reads, a transaction that used one fails at commit") {
    db->engine_for_testing().set_mode(bytecask::Mode::Follower);
    // Autocommit: the statement's commit writes nothing and is not asked of
    // the engine.
    auto r = db->execute(kLock, {std::int64_t{2}});
    REQUIRE(r.rows.size() == 1);
    CHECK(std::get<std::int64_t>(r.rows[0][0]) == 20);
    // A read-only transaction without FOR UPDATE commits.
    auto plain = db->begin();
    CHECK(txn_balance(plain, 1) == 10);
    CHECK(plain.commit() == CommitOutcome::Committed);
    // With it, its guards need the engine's check, which a follower refuses,
    // as it refuses a write: nothing is written.
    auto t = db->begin();
    CHECK(t.execute(kLock, {std::int64_t{1}}).rows.size() == 1);
    try {
      (void)t.commit();
      FAIL("a follower committed a guarded transaction");
    } catch (const ExecError& e) {
      CHECK_FALSE(e.conflict());
      CHECK(e.code() == BC_E_DEGRADED);
    }
    db->engine_for_testing().set_mode(bytecask::Mode::Leader);
  }
  SECTION("a row FOR UPDATE returned that no one changed commits, and the guard blocks no writer") {
    auto t = db->begin();
    CHECK(t.execute(kLock, {std::int64_t{1}}).rows.size() == 1);
    db->execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{2}, std::int64_t{22}});
    t.execute("UPDATE acct SET bal = $2 WHERE id = $1", {std::int64_t{1}, std::int64_t{12}});
    CHECK(t.commit() == CommitOutcome::Committed);
    CHECK(balance(*db, 1) == 12);
    CHECK(balance(*db, 2) == 22);
  }
  SECTION("an autocommit SELECT … FOR UPDATE returns its rows") {
    auto r = db->execute(kLock, {std::int64_t{2}});
    REQUIRE(r.rows.size() == 1);
    CHECK(std::get<std::int64_t>(r.rows[0][0]) == 20);
  }
}
