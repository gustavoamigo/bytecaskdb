// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// DatabaseOptions::sync (docs/server_design.md, *Durability*): when a commit
// reaches the disk. The engine's fsyncs counter shows it; what an OS crash
// would lose cannot be staged here, so each mode is also checked to keep
// every commit across close and reopen.

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

namespace {

auto fsyncs(Database& db) -> std::int64_t {
  auto stats = db.engine_for_testing().stats();
  auto it = stats.find("bytecask.fsyncs");
  return it == stats.end() ? 0 : it->second;
}

auto open_with(const sql_test::TempDir& dir, SyncMode mode, std::uint32_t interval_ms = 1000)
    -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path(), {.sync = mode, .sync_interval_ms = interval_ms});
  db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT NOT NULL)");
  return db;
}

// One autocommit insert and one two-statement transaction.
void write_rows(Database& db, std::int64_t first) {
  db.execute("INSERT INTO t VALUES ($1, $2)", {first, std::int64_t{1}});
  auto txn = db.begin();
  txn.execute("INSERT INTO t VALUES ($1, $2)", {first + 1, std::int64_t{2}});
  txn.execute("INSERT INTO t VALUES ($1, $2)", {first + 2, std::int64_t{3}});
  (void)txn.commit();
}

auto row_count(Database& db) -> std::size_t {
  std::size_t n = 0;
  for (std::int64_t id = 0; id < 100; ++id) n += db.execute("SELECT v FROM t WHERE id = $1", {id}).rows.size();
  return n;
}

}  // namespace

TEST_CASE("sync modes: AT_EVERY_COMMIT syncs each commit before it returns", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_every"};
  auto db = open_with(dir, SyncMode::AtEveryCommit);
  auto before = fsyncs(*db);
  db->execute("INSERT INTO t VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{1}});
  CHECK(fsyncs(*db) > before);
  before = fsyncs(*db);
  auto txn = db->begin();
  txn.execute("INSERT INTO t VALUES ($1, $2)", {std::int64_t{2}, std::int64_t{2}});
  (void)txn.commit();
  CHECK(fsyncs(*db) > before);
}

TEST_CASE("sync modes: AT_FILE_ROTATION syncs no commit, and close syncs them all", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_rotation"};
  {
    auto db = open_with(dir, SyncMode::AtFileRotation);
    auto before = fsyncs(*db);
    write_rows(*db, 1);
    CHECK(fsyncs(*db) == before);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});  // no flusher runs
    CHECK(fsyncs(*db) == before);
    db->execute("CREATE TABLE u (id BIGINT PRIMARY KEY)");  // a DDL is synced in every mode
    CHECK(fsyncs(*db) > before);
    db->close();
  }
  auto again = Database::open(dir.path());
  CHECK(row_count(*again) == 3);
}

TEST_CASE("sync modes: AT_INTERVAL syncs in the background, not at commit", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_interval"};
  {
    auto db = open_with(dir, SyncMode::AtInterval, 60'000);  // no tick comes within the test
    auto before = fsyncs(*db);
    write_rows(*db, 1);
    CHECK(fsyncs(*db) == before);
  }
  auto again = Database::open(dir.path());
  CHECK(row_count(*again) == 3);

  sql_test::TempDir fast_dir{"sync_interval_fast"};
  auto db = open_with(fast_dir, SyncMode::AtInterval, 20);
  auto before = fsyncs(*db);
  write_rows(*db, 1);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (fsyncs(*db) == before && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  CHECK(fsyncs(*db) > before);
}

TEST_CASE("sync modes: an interval outside 1 to 60000 is refused", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_bad_interval"};
  CHECK_THROWS_AS(Database::open(dir.path(), {.sync_interval_ms = 0}), std::invalid_argument);
  CHECK_THROWS_AS(Database::open(dir.path(), {.sync_interval_ms = 60'001}), std::invalid_argument);
  CHECK(Database::open(dir.path(), {.sync_interval_ms = 60'000}) != nullptr);
}

TEST_CASE("io backend: a buffer pool serves reads after a reopen, and one too small is refused", "[sql][api][io]") {
  sql_test::TempDir dir{"io_backend"};
  constexpr std::size_t kPool = std::size_t{256} << 20;
  {
    auto db = Database::open(dir.path(), {.io_backend = bytecask::IoBackend::BufferPool, .buffer_pool = {.capacity_bytes = kPool}});
    db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT NOT NULL)");
    write_rows(*db, 1);
  }
  auto again = Database::open(dir.path(), {.io_backend = bytecask::IoBackend::BufferPool,
                                          .buffer_pool = {.capacity_bytes = kPool, .direct_io = false}});
  CHECK(row_count(*again) == 3);
  again->close();
  CHECK_THROWS_AS(Database::open(dir.path(), {.io_backend = bytecask::IoBackend::BufferPool, .buffer_pool = {.capacity_bytes = 1}}),
                  std::invalid_argument);
}
