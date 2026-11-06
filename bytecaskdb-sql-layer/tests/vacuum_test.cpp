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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

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

// Rows nothing overwrites, written first: the one data file so far holds them,
// and once the rest go dead around them, vacuum can reclaim that file only by
// moving them.
constexpr std::int64_t kKept = 1000;
constexpr std::int64_t kKeptRows = 8;

void insert_rows(Database& db, std::int64_t first, int round, std::int64_t rows = kRows) {
  for (auto id = first; id < first + rows; ++id) db.execute("INSERT INTO t VALUES ($1, $2)", {id, wide(round, id)});
}

void update_rows(Database& db, std::int64_t first, int round) {
  for (auto id = first; id < first + kRows; ++id) db.execute("UPDATE t SET v = $2 WHERE id = $1", {id, wide(round, id)});
}

auto holds(const Rows& rows, std::int64_t first, int round, std::int64_t count = kRows) -> bool {
  if (std::ssize(rows.rows) != count) return false;
  for (std::int64_t i = 0; i < count; ++i) {
    const auto& row = rows.rows[static_cast<std::size_t>(i)];
    if (std::get<std::int64_t>(row[0]) != first + i) return false;
    if (std::get<Bytes>(row[1]) != std::get<Bytes>(wide(round, first + i))) return false;
  }
  return true;
}

auto scan(Database& db) -> Rows { return db.execute("SELECT id, v FROM t WHERE id < 1000 ORDER BY id"); }

auto scan_kept(Database& db) -> Rows { return db.execute("SELECT id, v FROM t WHERE id >= 1000 ORDER BY id"); }

auto only_data_file(const sql_test::TempDir& dir) -> std::filesystem::path {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator{dir.path()}) {
    if (entry.path().extension() == ".data") files.push_back(entry.path());
  }
  REQUIRE(files.size() == 1);
  return files[0];
}

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

auto healthy() -> bool { return false; }

// A VacuumLog is a plain function, so what it records is the test's one log.
struct Logged {
  std::string what;
  std::uint64_t failures;
  bool stops;
};
std::mutex logged_mu;

auto logged() -> std::vector<Logged>& {
  static auto* entries = new std::vector<Logged>;
  return *entries;
}

void record(const char* what, std::uint64_t failures, bool stops) {
  const std::lock_guard<std::mutex> lock{logged_mu};
  logged().push_back({what, failures, stops});
}

auto taken_log() -> std::vector<Logged> {
  const std::lock_guard<std::mutex> lock{logged_mu};
  return std::exchange(logged(), {});
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
                        healthy,
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
    VacuumThread thread{[&] { return ++passes < 5; }, healthy, {.busy = 1ms, .idle = kHour}};
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
                        healthy,
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
                          healthy,
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
    VacuumThread thread{pass, healthy, {.busy = 1ms, .idle = 1ms}};
    REQUIRE(eventually([&] { return passes.load() == 1; }));
    std::this_thread::sleep_for(50ms);
  }
  CHECK(passes.load() == 1);
}

TEST_CASE("vacuum thread: a failure that left the engine degraded ends it at once, naming the failure",
          "[sql][server]") {
  (void)taken_log();
  std::atomic<int> passes{0};
  {
    // A failed fdatasync: the I/O error, with the engine degraded by then.
    VacuumThread thread{[&]() -> bool {
                          ++passes;
                          throw std::runtime_error("fdatasync failed");
                        },
                        [] { return true; },
                        {.busy = 1ms, .idle = 1ms},
                        record};
    REQUIRE(eventually([&] { return passes.load() == 1; }));
    std::this_thread::sleep_for(50ms);
  }
  CHECK(passes.load() == 1);
  const auto log = taken_log();
  REQUIRE(log.size() == 1);
  CHECK(log[0].what == "fdatasync failed");
  CHECK(log[0].stops);
}

TEST_CASE("vacuum thread: a lasting failure is logged again, and a success starts the count over", "[sql][server]") {
  (void)taken_log();
  std::atomic<int> passes{0};
  {
    // 21 failures, a success, then one more failure.
    VacuumThread thread{[&]() -> bool {
                          const auto n = ++passes;
                          if (n == 22) return true;
                          if (n > 23) return false;
                          throw std::runtime_error("no space left");
                        },
                        healthy,
                        {.busy = 1ms, .idle = 1ms},
                        record};
    REQUIRE(eventually([&] { return passes.load() > 23; }));
  }
  std::vector<std::uint64_t> failures;
  for (const auto& entry : taken_log()) {
    CHECK_FALSE(entry.stops);
    failures.push_back(entry.failures);
  }
  CHECK(failures == std::vector<std::uint64_t>{1, 11, 21, 1});
}

TEST_CASE("vacuum thread: an exception of any type is reported and retried", "[sql][server]") {
  (void)taken_log();
  std::atomic<int> passes{0};
  {
    VacuumThread thread{[&]() -> bool {
                          if (++passes < 3) throw 42;
                          return false;
                        },
                        healthy,
                        {.busy = 1ms, .idle = 1ms},
                        record};
    REQUIRE(eventually([&] { return passes.load() >= 3; }));
  }
  const auto log = taken_log();
  REQUIRE_FALSE(log.empty());
  CHECK(log[0].what == "an exception of unknown type");
}

TEST_CASE("vacuum thread: gives the disk back while rows keep their values, also after a reopen", "[sql][server]") {
  sql_test::TempDir dir{"vacuum_thread"};
  auto db = open_wide(dir);
  constexpr auto kRounds = 6;
  insert_rows(*db, kKept, 0, kKeptRows);
  const auto first_file = only_data_file(dir);
  insert_rows(*db, 0, 0);
  {
    VacuumThread thread{[&] { return db->vacuum(); }, [&] { return db->is_degraded(); }, {.busy = 10ms, .idle = 100ms}};
    for (auto round = 1; round < kRounds; ++round) update_rows(*db, 0, round);
    // 200 MiB written, 40 MiB of it live. Sealed files end at most half dead,
    // and the active file holds up to 64 MiB more.
    CHECK(eventually([&] { return data_bytes(dir) <= 144u << 20; }));
    REQUIRE(eventually([&] { return !std::filesystem::exists(first_file); }));
  }
  CHECK(holds(scan(*db), 0, kRounds - 1));
  CHECK(holds(scan_kept(*db), kKept, 0, kKeptRows));
  db->close();
  db = Database::open(dir.path());
  CHECK(holds(scan(*db), 0, kRounds - 1));
  CHECK(holds(scan_kept(*db), kKept, 0, kKeptRows));
}

TEST_CASE("Database::vacuum: returns false with nothing to reclaim and throws DbClosed after close()", "[sql][api]") {
  sql_test::TempDir dir{"vacuum_api"};
  auto db = open_wide(dir);
  CHECK_FALSE(db->vacuum());
  db->close();
  CHECK_THROWS_AS(db->vacuum(), DbClosed);
  CHECK_FALSE(db->is_degraded());
}

TEST_CASE("Database::vacuum: a threshold of 1.0 reclaims nothing, not even a fully dead file", "[sql][api]") {
  sql_test::TempDir dir{"vacuum_threshold"};
  auto db = open_wide(dir);
  insert_rows(*db, 0, 0);
  for (auto round = 1; round < 6; ++round) update_rows(*db, 0, round);
  CHECK_FALSE(db->vacuum({.fragmentation_threshold = 1.0}));
  CHECK(db->vacuum());
}

TEST_CASE("Database::vacuum: throws DbDegraded on a degraded engine, which ends the thread", "[sql][api][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("no allocation fault seam in this build");
  sql_test::TempDir dir{"vacuum_degraded"};
  auto db = open_wide(dir);
  CHECK_FALSE(db->is_degraded());
  // The first allocation whose failure leaves a commit's outcome unknown.
  for (std::int64_t nth = 1; !db->engine_for_testing().is_degraded(); ++nth) {
    // Built outside the faults: only the statement's own allocations are failed.
    const std::vector<Value> params{nth, sql_test::bytes("v")};
    bytecask::testing::ScopedAllocFaults faults{nth, false};
    try {
      db->execute("INSERT INTO t VALUES ($1, $2)", params);
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the loop reads the outcome
    }
    REQUIRE(faults.report().fired);
  }
  CHECK(db->is_degraded());
  CHECK_THROWS_AS(db->vacuum(), DbDegraded);

  std::atomic<int> passes{0};
  {
    VacuumThread thread{[&] {
                          ++passes;
                          return db->vacuum();
                        },
                        healthy,
                        {.busy = 1ms, .idle = 1ms}};
    REQUIRE(eventually([&] { return passes.load() == 1; }));
    std::this_thread::sleep_for(50ms);
  }
  CHECK(passes.load() == 1);
  db->engine_for_testing().resume();
}

TEST_CASE("Database::vacuum: a transaction open across it reads its snapshot and commits with no conflict",
          "[sql][api]") {
  sql_test::TempDir dir{"vacuum_txn"};
  auto db = open_wide(dir);
  insert_rows(*db, kKept, 0, kKeptRows);
  const auto first_file = only_data_file(dir);
  insert_rows(*db, 0, 0);
  auto txn = db->begin();
  CHECK(holds(txn.execute("SELECT id, v FROM t WHERE id < 1000 ORDER BY id"), 0, 0));
  for (auto round = 1; round < 6; ++round) update_rows(*db, 0, round);

  while (db->vacuum()) {
  }
  // The kept rows were only in this file, so they have moved.
  REQUIRE_FALSE(std::filesystem::exists(first_file));
  CHECK(holds(scan_kept(*db), kKept, 0, kKeptRows));

  CHECK(holds(txn.execute("SELECT id, v FROM t WHERE id < 1000 ORDER BY id"), 0, 0));
  CHECK(holds(txn.execute("SELECT id, v FROM t WHERE id >= 1000 ORDER BY id"), kKept, 0, kKeptRows));
  for (auto id = kKept; id < kKept + kKeptRows; ++id) {
    txn.execute("UPDATE t SET v = $2 WHERE id = $1", {id, wide(7, id)});
  }
  CHECK(txn.commit() == CommitOutcome::Committed);
  CHECK(holds(scan_kept(*db), kKept, 7, kKeptRows));
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
