// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Vacuum under the SQL layer (docs/session_design.md, *Background vacuum*):
// the server's thread, driven by a pass the test scripts, and Database::vacuum
// over rows wide enough to fill the engine's 64 MiB files. An open transaction
// or hold cursor reads on across a vacuum; the engine's own guarantees are its
// tests'.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.tests.util;
import bytecask.sql.vacuum_thread;

using namespace bytecask::sql;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kRowBytes = 1u << 20;
constexpr std::int64_t kRows = 32;  // 32 MiB a round: half of one data file
constexpr auto kHour = std::chrono::milliseconds{1h};

auto wide(int round, std::int64_t id) -> Value {
  auto text = std::string(kRowBytes, static_cast<char>('a' + round % 26));
  text.replace(0, 8, std::to_string(100'000 + id).substr(0, 8));
  return sql_test::bytes(text);
}

auto open_wide(const sql_test::TempDir& dir) -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path(), {.sync = SyncMode::AtFileRotation});
  db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v BYTEA NOT NULL)");
  return db;
}

void insert_rows(Database& db, std::int64_t first, int round) {
  for (auto id = first; id < first + kRows; ++id) db.execute("INSERT INTO t VALUES ($1, $2)", {id, wide(round, id)});
}

void update_rows(Database& db, std::int64_t first, int round) {
  for (auto id = first; id < first + kRows; ++id) db.execute("UPDATE t SET v = $2 WHERE id = $1", {id, wide(round, id)});
}

auto holds(const Rows& rows, std::int64_t first, int round) -> bool {
  if (std::ssize(rows.rows) != kRows) return false;
  for (std::int64_t i = 0; i < kRows; ++i) {
    const auto& row = rows.rows[static_cast<std::size_t>(i)];
    if (std::get<std::int64_t>(row[0]) != first + i) return false;
    if (std::get<Bytes>(row[1]) != std::get<Bytes>(wide(round, first + i))) return false;
  }
  return true;
}

auto scan(Database& db) -> Rows { return db.execute("SELECT id, v FROM t WHERE id < 1000 ORDER BY id"); }

auto data_bytes(const sql_test::TempDir& dir) -> std::uintmax_t {
  std::uintmax_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator{dir.path()}) {
    std::error_code ec;  // vacuum may unlink it between the listing and the stat
    const auto size = entry.file_size(ec);
    if (!ec && entry.path().extension() == ".data") total += size;
  }
  return total;
}

// Open descriptors of this process on data files already unlinked: what a
// snapshot keeps of a file vacuum reclaimed, which du no longer counts.
auto unlinked_held(const sql_test::TempDir& dir) -> int {
  auto held = 0;
  for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/fd"}) {
    std::error_code ec;
    const auto target = std::filesystem::read_symlink(entry.path(), ec).string();
    if (!ec && target.starts_with(dir.path().string()) && target.ends_with(".data (deleted)")) ++held;
  }
  return held;
}

auto eventually(const std::function<bool()>& done, std::chrono::seconds limit = 60s) -> bool {
  const auto until = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(10ms);
  }
  return true;
}

}  // namespace

TEST_CASE("vacuum thread: stopping interrupts a pause rather than waiting it out", "[sql][server]") {
  std::atomic<int> passes{0};
  const auto started = std::chrono::steady_clock::now();
  {
    VacuumThread thread{[&] {
                          ++passes;
                          return false;
                        },
                        {.busy = kHour, .idle = kHour}};
    REQUIRE(eventually([&] { return passes.load() == 1; }));
  }
  CHECK(std::chrono::steady_clock::now() - started < 30s);
  CHECK(passes.load() == 1);
}

TEST_CASE("vacuum thread: a pass that reclaimed waits the busy pause, one that found nothing the idle pause",
          "[sql][server]") {
  std::atomic<int> passes{0};
  {
    VacuumThread thread{[&] { return ++passes < 5; }, {.busy = 1ms, .idle = kHour}};
    REQUIRE(eventually([&] { return passes.load() == 5; }));
    std::this_thread::sleep_for(50ms);
  }
  CHECK(passes.load() == 5);
}

TEST_CASE("vacuum thread: a failed pass waits the idle pause and is retried", "[sql][server]") {
  SECTION("retried") {
    std::atomic<int> passes{0};
    VacuumThread thread{[&]() -> bool {
                          if (++passes < 3) throw std::runtime_error("no space left");
                          return false;
                        },
                        {.busy = kHour, .idle = 1ms}};
    CHECK(eventually([&] { return passes.load() >= 3; }));
  }
  SECTION("not at the busy pause") {
    std::atomic<int> passes{0};
    {
      VacuumThread thread{[&]() -> bool {
                            ++passes;
                            throw std::runtime_error("no space left");
                          },
                          {.busy = 1ms, .idle = kHour}};
      REQUIRE(eventually([&] { return passes.load() == 1; }));
      std::this_thread::sleep_for(50ms);
    }
    CHECK(passes.load() == 1);
  }
}

TEST_CASE("vacuum thread: a degraded or closed engine ends it", "[sql][server]") {
  std::atomic<int> passes{0};
  std::function<bool()> pass;
  SECTION("degraded") {
    pass = [&]() -> bool {
      ++passes;
      throw DbDegraded{"an fdatasync failed"};
    };
  }
  SECTION("closed") {
    pass = [&]() -> bool {
      ++passes;
      throw DbClosed{};
    };
  }
  {
    VacuumThread thread{pass, {.busy = 1ms, .idle = 1ms}};
    REQUIRE(eventually([&] { return passes.load() == 1; }));
    std::this_thread::sleep_for(50ms);
  }
  CHECK(passes.load() == 1);
}

TEST_CASE("vacuum thread: gives the disk back while rows keep their values, also after a reopen", "[sql][server]") {
  sql_test::TempDir dir{"vacuum_thread"};
  {
    auto db = open_wide(dir);
    constexpr auto kRounds = 6;
    insert_rows(*db, 0, 0);
    {
      VacuumThread thread{[&] { return db->vacuum(); }, {.busy = 10ms, .idle = 100ms}};
      for (auto round = 1; round < kRounds; ++round) update_rows(*db, 0, round);
      // 192 MiB written, 32 MiB of it live. Each sealed file ends at most half
      // dead, and the active file holds up to 64 MiB more.
      CHECK(eventually([&] { return data_bytes(dir) <= 128u << 20; }));
    }
    CHECK(holds(scan(*db), 0, kRounds - 1));
    db->close();
    db = Database::open(dir.path());
    CHECK(holds(scan(*db), 0, kRounds - 1));
  }
}

TEST_CASE("Database::vacuum: returns false with nothing to reclaim and throws DbClosed after close()", "[sql][api]") {
  sql_test::TempDir dir{"vacuum_api"};
  auto db = open_wide(dir);
  CHECK_FALSE(db->vacuum());
  db->close();
  CHECK_THROWS_AS(db->vacuum(), DbClosed);
}

TEST_CASE("Database::vacuum: a transaction open across it reads its snapshot and commits with no conflict",
          "[sql][api]") {
  sql_test::TempDir dir{"vacuum_txn"};
  auto db = open_wide(dir);
  // Rows the rest never overwrites, in the file vacuum will compact: it moves them.
  constexpr std::int64_t kKept = 1000;
  insert_rows(*db, kKept, 0);
  insert_rows(*db, 0, 0);
  auto txn = db->begin();
  CHECK(holds(txn.execute("SELECT id, v FROM t WHERE id < 1000 ORDER BY id"), 0, 0));
  for (auto round = 1; round < 6; ++round) update_rows(*db, 0, round);

  auto reclaimed = 0;
  while (db->vacuum()) ++reclaimed;
  REQUIRE(reclaimed > 0);

  CHECK(holds(txn.execute("SELECT id, v FROM t WHERE id < 1000 ORDER BY id"), 0, 0));
  CHECK(holds(txn.execute("SELECT id, v FROM t WHERE id >= 1000 ORDER BY id"), kKept, 0));
  // A few of them: a transaction holds at most 64 MiB of writes.
  constexpr std::int64_t kWritten = 4;
  for (auto id = kKept; id < kKept + kWritten; ++id) {
    txn.execute("UPDATE t SET v = $2 WHERE id = $1", {id, wide(7, id)});
  }
  CHECK(txn.commit() == CommitOutcome::Committed);
  const auto after = db->execute("SELECT id, v FROM t WHERE id >= 1000 ORDER BY id");
  REQUIRE(std::ssize(after.rows) == kRows);
  for (std::int64_t i = 0; i < kRows; ++i) {
    const auto id = kKept + i;
    CHECK(std::get<Bytes>(after.rows[static_cast<std::size_t>(i)][1]) ==
          std::get<Bytes>(wide(i < kWritten ? 7 : 0, id)));
  }
}

TEST_CASE("Database::vacuum: a hold cursor reads on across it and keeps the disk blocks until it closes",
          "[sql][api]") {
  sql_test::TempDir dir{"vacuum_hold"};
  auto db = open_wide(dir);
  insert_rows(*db, 0, 0);
  auto txn = db->begin();
  auto cursor = txn.open_cursor(db->prepare("SELECT id, v FROM t ORDER BY id"), {}, Holdability::Hold);
  CHECK(txn.commit() == CommitOutcome::Committed);
  for (auto round = 1; round < 6; ++round) update_rows(*db, 0, round);

  auto reclaimed = 0;
  while (db->vacuum()) ++reclaimed;
  REQUIRE(reclaimed > 0);
  CHECK(unlinked_held(dir) > 0);

  CHECK(holds(cursor.fetch(1000), 0, 0));

  cursor.close();
  // A thread's read cache may hold the state it last read until its next read.
  CHECK(eventually([&] {
    (void)scan(*db);
    return unlinked_held(dir) == 0;
  }));
}
