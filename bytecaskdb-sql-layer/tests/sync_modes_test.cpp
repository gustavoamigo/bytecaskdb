// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// DatabaseOptions (docs/server_design.md, *Durability* and *Reads*): when a
// commit reaches the disk, and how sealed files are read. Durability is read
// from the engine's durable_sequence(), the highest sequence fdatasync has
// confirmed. What an OS crash would lose cannot be staged here, so each mode
// is also checked to keep every commit across close and reopen.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;

namespace {

auto open_with(const sql_test::TempDir& dir, SyncMode mode, std::uint32_t interval_ms = 1000)
    -> std::unique_ptr<Database> {
  auto db = Database::open(dir.path(), {.sync = mode, .sync_interval_ms = interval_ms});
  db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT NOT NULL)");
  return db;
}

auto durable(Database& db) -> std::uint64_t { return db.engine_for_testing().durable_sequence(); }

// The engine numbers writes in commit order, so an unsynced write made right
// after a commit takes a sequence above every one the commit took. Its key is
// outside the layer's namespaces (catalog, rows and indexes: 0x01 to 0x03),
// so no scan of the layer's reads it.
auto sequence_after_commits(Database& db) -> std::uint64_t {
  constexpr std::array marker{std::byte{0xF0}};
  return db.engine_for_testing().put({.sync = false}, marker, marker).sequence;
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
  write_rows(*db, 1);
  auto marker = sequence_after_commits(*db);
  CHECK(durable(*db) >= marker - 1);  // every commit before the marker
  CHECK(durable(*db) < marker);       // and not the unsynced marker: the sync was the commits'
}

TEST_CASE("sync modes: AT_FILE_ROTATION syncs no commit, and close syncs them all", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_rotation"};
  {
    auto db = open_with(dir, SyncMode::AtFileRotation);
    db->execute("CREATE TABLE u (id BIGINT PRIMARY KEY)");  // a DDL is synced in every mode
    CHECK(durable(*db) >= sequence_after_commits(*db) - 1);
    auto before = durable(*db);
    write_rows(*db, 1);
    CHECK(durable(*db) == before);
    db->close();
  }
  auto again = Database::open(dir.path());
  CHECK(row_count(*again) == 3);
}

TEST_CASE("sync modes: AT_INTERVAL syncs in the background, not at commit", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_interval"};
  {
    auto db = open_with(dir, SyncMode::AtInterval, 60'000);  // no tick comes within the test
    auto before = durable(*db);
    write_rows(*db, 1);
    CHECK(durable(*db) == before);
  }
  auto again = Database::open(dir.path());
  CHECK(row_count(*again) == 3);

  // A background sync makes every write before it durable. The deadline only
  // bounds a broken flusher; a working one meets it within an interval.
  sql_test::TempDir fast_dir{"sync_interval_fast"};
  auto db = open_with(fast_dir, SyncMode::AtInterval, 20);
  write_rows(*db, 1);
  auto marker = sequence_after_commits(*db);
  CHECK(db->engine_for_testing().durable_sequence(marker, std::chrono::seconds{60}) >= marker);
}

TEST_CASE("sync modes: an interval outside 1 to 60000, or an unknown mode, is refused", "[sql][api][sync]") {
  sql_test::TempDir dir{"sync_bad_options"};
  CHECK_THROWS_AS(Database::open(dir.path(), {.sync_interval_ms = 0}), std::invalid_argument);
  CHECK_THROWS_AS(Database::open(dir.path(), {.sync_interval_ms = 60'001}), std::invalid_argument);
  CHECK_THROWS_AS(Database::open(dir.path(), {.sync = static_cast<SyncMode>(7)}), std::invalid_argument);
  CHECK(Database::open(dir.path(), {.sync_interval_ms = 60'000}) != nullptr);
}

TEST_CASE("io backend: every backend reads what another wrote, and a pool too small is refused", "[sql][api][io]") {
  sql_test::TempDir dir{"io_backend"};
  constexpr std::size_t kPool = std::size_t{256} << 20;
  {
    auto db = Database::open(dir.path());  // pread
    db->execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT NOT NULL)");
    write_rows(*db, 1);
  }
  {
    auto db = Database::open(dir.path(), {.io_backend = bytecask::IoBackend::BufferPool,
                                          .buffer_pool = {.capacity_bytes = kPool}});
    CHECK(row_count(*db) == 3);
    write_rows(*db, 10);
  }
  {
    auto db = Database::open(dir.path(), {.io_backend = bytecask::IoBackend::BufferPool,
                                          .buffer_pool = {.capacity_bytes = kPool, .direct_io = false}});
    CHECK(row_count(*db) == 6);
  }
  {
    auto db = Database::open(dir.path(), {.io_backend = bytecask::IoBackend::Mmap});
    CHECK(row_count(*db) == 6);
  }
  CHECK_THROWS_AS(Database::open(dir.path(), {.io_backend = bytecask::IoBackend::BufferPool,
                                              .buffer_pool = {.capacity_bytes = 1}}),
                  std::invalid_argument);
}
