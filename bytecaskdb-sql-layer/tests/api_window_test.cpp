// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The replay runner (docs/cursor_design.md, *The replay cursor*):
// Transaction::execute_window drops the first `skip` rows of a read, keeps
// the next `limit` and stops. Determinism is the phase's gate: every golden
// SELECT, run twice on one transaction's view with the same parameters,
// gives the same rows, and replaying it in pages gives the run to completion
// sliced.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.tests.util;
import bytecask.sql.tprocc;

using namespace bytecask::sql;

namespace {

constexpr auto kAll = std::numeric_limits<std::uint64_t>::max();

auto read_file(const std::filesystem::path& p) -> std::string {
  std::ifstream in{p, std::ios::binary};
  REQUIRE(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

auto statements(const std::string& text) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::istringstream in{text};
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && line.back() == ';') line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

// The fixture's schema and the golden one, with rows that repeat in every
// indexed column, so equal keys (ties) are common.
auto populated(const sql_test::TempDir& dir) -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT)");
  db->execute("CREATE INDEX t_by_name ON t (name)");
  for (const auto& ddl : statements(read_file(std::filesystem::path{SQL_LAYER_GOLDEN_DIR} / "schema.sql"))) {
    db->execute(ddl);
  }
  for (std::int64_t i = 1; i <= 60; ++i) {
    db->execute("INSERT INTO t VALUES ($1, $2, $3)", {i, sql_test::bytes("n" + std::to_string(i % 6)), i % 10});
    db->execute("INSERT INTO p VALUES ($1, $2, $3, $4)", {i / 10, i % 10, i, i % 7});
    db->execute("INSERT INTO r VALUES ($1, $2, $3)", {i, i % 5, static_cast<double>(i % 9) / 2.0});
  }
  return db;
}

// Parameters of the statement's types, small values that hit the rows above.
auto params_for(const Statement& s) -> std::vector<Value> {
  std::vector<Value> out;
  const std::vector<std::int64_t> ints{2, 6, 1, 8};
  for (const auto& p : s.params()) {
    auto n = out.size();
    switch (phys_type(p.type)) {
      case PhysType::I64: out.emplace_back(ints[n % ints.size()]); break;
      case PhysType::F64: out.emplace_back(1.5 + static_cast<double>(n)); break;
      case PhysType::Bool: out.emplace_back(n % 2 == 0); break;
      case PhysType::Bytes: out.emplace_back(sql_test::bytes(n % 2 == 0 ? "n2" : "n4")); break;
    }
  }
  return out;
}

using RowVec = std::vector<std::vector<Value>>;

struct GateCounts {
  std::size_t selects{0}, multi_row{0}, resumable{0};
};

// Every SELECT in `dir`, run twice on the transaction's view and replayed in
// pages: the same rows each time, and the pages the run's rows sliced.
auto replay_gate(Database& db, Transaction& txn, const std::filesystem::path& dir) -> GateCounts {
  GateCounts counts;
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == ".sql" && e.path().stem() != "schema") files.push_back(e.path());
  }
  std::ranges::sort(files);
  for (const auto& f : files) {
    INFO(f.filename().string());
    auto sql = read_file(f);
    while (!sql.empty() && (sql.back() == '\n' || sql.back() == ';')) sql.pop_back();
    const Statement* stmt = nullptr;
    try {
      stmt = &db.prepare(sql);
    } catch (const std::exception&) {
      continue;  // a plan of another schema
    }
    if (stmt->command() != Command::Select) continue;
    ++counts.selects;
    counts.resumable += stmt->is_resumable() ? 1u : 0u;
    auto params = params_for(*stmt);

    auto full = txn.execute_window(*stmt, params, {0, kAll});
    auto again = txn.execute_window(*stmt, params, {0, kAll});
    CHECK(full.rows.rows == again.rows.rows);
    CHECK(full.end);
    CHECK(full.position == full.rows.rows.size());
    CHECK(full.rows.rows == txn.execute(*stmt, params).rows);
    const auto n = full.rows.rows.size();
    if (n >= 2) ++counts.multi_row;

    for (std::uint64_t k : {1u, 3u, 7u}) {
      RowVec got;
      for (std::uint64_t pos = 0;;) {
        auto page = txn.execute_window(*stmt, params, {pos, k});
        REQUIRE(page.rows.rows.size() <= k);
        got.insert(got.end(), page.rows.rows.begin(), page.rows.rows.end());
        pos += page.rows.rows.size();
        if (page.end || page.rows.rows.empty()) {
          CHECK(page.position == n);
          break;
        }
        CHECK(page.position == pos);
      }
      CHECK(got == full.rows.rows);
    }
  }
  return counts;
}

}  // namespace

TEST_CASE("replay runner: every golden SELECT is deterministic and a window is a slice of the run", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_window"};
  auto db = populated(dir);
  auto txn = db->begin();
  // The overlay is part of the view: a replay sees the statement's own writes every time.
  txn.execute("INSERT INTO t VALUES ($1, $2, $3)", {std::int64_t{100}, sql_test::bytes("n2"), std::int64_t{3}});
  txn.execute("UPDATE p SET v = v + 1 WHERE a = 1");
  auto c = replay_gate(*db, txn, SQL_LAYER_GOLDEN_DIR);
  // The gate must have looked at plans with something to order.
  CHECK(c.selects >= 40);
  CHECK(c.multi_row >= c.selects / 3);
  CHECK(c.resumable >= 5);
  txn.rollback();
}

TEST_CASE("replay runner: TPROC-C's golden SELECTs are deterministic too", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_window_tprocc"};
  auto db = Database::open(dir.path());
  tprocc::load(*db, {.warehouses = 1, .scale = {.items = 200, .customers = 30, .orders = 30, .new_orders = 9}});
  auto txn = db->begin();
  auto c = replay_gate(*db, txn, std::filesystem::path{SQL_LAYER_GOLDEN_DIR} / "tprocc");
  CHECK(c.selects >= 10);
  txn.rollback();
}

TEST_CASE("replay runner: skip and limit at the edges", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_window_edges"};
  auto db = populated(dir);
  auto txn = db->begin();
  const auto& stmt = db->prepare("SELECT id FROM t ORDER BY id");
  auto full = txn.execute_window(stmt, {}, {0, kAll}).rows.rows;
  REQUIRE(full.size() == 60);

  auto at = [&](RowWindow w) { return txn.execute_window(stmt, {}, w); };

  SECTION("skip 0, limit k keeps the first k and does not know it is the end") {
    auto r = at({0, 5});
    CHECK(r.rows.rows == RowVec(full.begin(), full.begin() + 5));
    CHECK_FALSE(r.end);
    CHECK(r.position == 5);
  }
  SECTION("limit 0 returns no row and reads none") {
    auto r = at({7, 0});
    CHECK(r.rows.rows.empty());
    CHECK_FALSE(r.end);
    CHECK(r.rows.columns.size() == 1);
  }
  SECTION("skip past the end is empty and at the end") {
    auto r = at({61, 5});
    CHECK(r.rows.rows.empty());
    CHECK(r.end);
    CHECK(r.position == 60);
    auto huge = at({kAll, 5});
    CHECK(huge.rows.rows.empty());
    CHECK(huge.end);
    CHECK(huge.position == 60);
  }
  SECTION("skip to the last row") {
    auto r = at({59, 5});
    CHECK(r.rows.rows == RowVec(full.begin() + 59, full.end()));
    CHECK(r.end);
  }
  SECTION("a window ending exactly at the end has not yet seen it") {
    auto r = at({55, 5});
    CHECK(r.rows.rows.size() == 5);
    CHECK_FALSE(r.end);
    CHECK(at({60, 5}).end);
  }
  SECTION("a large skip and a large limit") {
    auto r = at({30, kAll});
    CHECK(r.rows.rows == RowVec(full.begin() + 30, full.end()));
    CHECK(r.end);
  }
  SECTION("a statement that returns no rows is refused and the transaction is intact") {
    const auto& ins = db->prepare("INSERT INTO t VALUES ($1, $2, $3)");
    CHECK_THROWS_AS(txn.execute_window(ins, {std::int64_t{200}, sql_test::bytes("x"), std::int64_t{1}}, {0, 1}),
                    std::invalid_argument);
    CHECK(txn.is_open());
    CHECK_FALSE(txn.is_aborted());
  }
  SECTION("a window leaves the transaction usable and its writes unchanged") {
    (void)at({0, 3});
    txn.execute("INSERT INTO t VALUES ($1, $2, $3)", {std::int64_t{300}, sql_test::bytes("z"), std::int64_t{1}});
    CHECK(at({0, kAll}).rows.rows.size() == 61);
    CHECK(txn.commit() == CommitOutcome::Committed);
    CHECK(db->execute("SELECT id FROM t").rows.size() == 61);
  }
}

TEST_CASE("replay runner: a failing plan fails the window the same way every time", "[sql][api][txn][cursor]") {
  sql_test::TempDir dir{"api_window_error"};
  auto db = populated(dir);
  const auto& stmt = db->prepare("SELECT 100 / (qty - 5) FROM t ORDER BY id");
  for (int i = 0; i < 2; ++i) {
    auto txn = db->begin();
    // The error is at the row where qty = 5, the fifth row (id 5): a window that stops before it is fine.
    CHECK(txn.execute_window(stmt, {}, {0, 4}).rows.rows.size() == 4);
    CHECK_THROWS_AS(txn.execute_window(stmt, {}, {0, 6}), ExecError);
  }
}

TEST_CASE("resumable flag: a whitelist of plan shapes", "[sql][api][cursor]") {
  sql_test::TempDir dir{"api_resumable"};
  auto db = populated(dir);
  auto resumable = [&](std::string_view sql) { return db->prepare(sql).is_resumable(); };
  CHECK(resumable("SELECT id, qty FROM t"));
  CHECK(resumable("SELECT id FROM t WHERE qty > 3"));
  CHECK(resumable("SELECT id FROM t LIMIT 5"));
  CHECK_FALSE(resumable("SELECT id FROM t ORDER BY qty, id"));
  CHECK_FALSE(resumable("SELECT COUNT(*) FROM t"));
  CHECK_FALSE(resumable("SELECT qty, COUNT(*) FROM t GROUP BY qty"));
  CHECK_FALSE(resumable("SELECT id FROM t UNION SELECT id FROM r"));
  CHECK_FALSE(resumable("INSERT INTO t VALUES ($1, $2, $3)"));
}
