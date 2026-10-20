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
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

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
