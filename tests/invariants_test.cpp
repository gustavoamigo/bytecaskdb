// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Tests for the invariant-checking helpers (Phase 2 of correctness validation).

#include <chrono>
#include <filesystem>
#include <format>
#include <system_error>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::assert_delta;
using bytecask::testing::assert_recoverable;
using bytecask::testing::assert_resumable;
using bytecask::testing::capture_baseline;
using bytecask::testing::ExpectedDelta;
using bytecask::testing::to_bytes;

// Temporary directory RAII helper (same pattern as bytecask_test.cpp).
struct TempDir {
  std::filesystem::path path;

  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "inv_test_{}_{}",
                 std::chrono::system_clock::now().time_since_epoch().count(),
                 next_id())} {
    std::filesystem::create_directories(path);
  }

  ~TempDir() { std::filesystem::remove_all(path); }

 private:
  static auto next_id() -> unsigned {
    static unsigned counter = 0;
    return counter++;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// assert_consistent tests
// ---------------------------------------------------------------------------

TEST_CASE("assert_consistent on empty DB", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  assert_consistent(db);
}

TEST_CASE("assert_consistent on populated DB", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));
  db.put({}, to_bytes("d"), to_bytes("4"));
  db.put({}, to_bytes("e"), to_bytes("5"));
  assert_consistent(db);
}

TEST_CASE("assert_consistent after deletes", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));
  (void)db.del({}, to_bytes("b"));
  assert_consistent(db);
}

TEST_CASE("assert_consistent after overwrites", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v1"));
  db.put({}, to_bytes("k"), to_bytes("v2_longer"));
  assert_consistent(db);
}

TEST_CASE("assert_consistent after batch", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  bytecask::WritePlan plan;
  plan.put(to_bytes("x"), to_bytes("10"));
  plan.put(to_bytes("y"), to_bytes("20"));
  plan.put(to_bytes("z"), to_bytes("30"));
  (void)db.apply_batch({}, std::move(plan));
  assert_consistent(db);
}

// ---------------------------------------------------------------------------
// assert_delta tests
// ---------------------------------------------------------------------------

TEST_CASE("assert_delta on successful single put", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("pre"), to_bytes("existing"));

  auto before = capture_baseline(db);
  db.put({}, to_bytes("new_key"), to_bytes("value"));

  assert_delta(before, db, ExpectedDelta{
      .keys_added = {"new_key"},
      .keys_removed = {},
      .expected_values = {{"new_key", "value"}},
      .seq_advance = 1,
      .degraded = false,
  });
}

TEST_CASE("assert_delta on successful single delete", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("val"));

  auto before = capture_baseline(db);
  (void)db.del({}, to_bytes("k"));

  assert_delta(before, db, ExpectedDelta{
      .keys_added = {},
      .keys_removed = {"k"},
      .expected_values = {},
      .seq_advance = 1,
      .degraded = false,
  });
}

TEST_CASE("assert_delta on successful batch", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto before = capture_baseline(db);
  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("1"));
  plan.put(to_bytes("b"), to_bytes("2"));
  (void)db.apply_batch({}, std::move(plan));

  // 2 puts + BulkBegin + BulkEnd = 4 sequence slots
  assert_delta(before, db, ExpectedDelta{
      .keys_added = {"a", "b"},
      .keys_removed = {},
      .expected_values = {{"a", "1"}, {"b", "2"}},
      .seq_advance = 4,
      .degraded = false,
  });
}

TEST_CASE("assert_delta detects degraded", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = false}, to_bytes("pre"), to_bytes("existing"));

  auto before = capture_baseline(db);

  // Mid-batch append failure: fi{2} fires on the 3rd checkpoint (op2 in the
  // 2-op batch), after BulkBegin and op1 have already reached disk. The engine
  // degrades and next_seq advances past all consumed sequences (4 = 2 ops + 2 markers).
  {
    bytecask::testing::ScopedFaultInjector fi{2};
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("v1"));
    plan.put(to_bytes("b"), to_bytes("v2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
                      std::system_error);
  }

  assert_delta(before, db, ExpectedDelta{
      .keys_added = {},
      .keys_removed = {},
      .expected_values = {},
      .seq_advance = 4,
      .degraded = true,
  });
  assert_resumable(db);
}

// ---------------------------------------------------------------------------
// assert_recoverable tests
// ---------------------------------------------------------------------------

TEST_CASE("assert_recoverable after successful write", "[invariants]") {
  TempDir td;
  auto dir = td.path / "db";

  bytecask::testing::Baseline before;
  {
    auto db = bytecask::DB::open(dir);
    db.put({.sync = true}, to_bytes("pre"), to_bytes("existing"));
    before = capture_baseline(db);
    db.put({.sync = true}, to_bytes("new_key"), to_bytes("value"));
  } // DB closes, hints flushed

  assert_recoverable(dir, before, ExpectedDelta{
      .keys_added = {"new_key"},
      .keys_removed = {},
      .expected_values = {{"new_key", "value"}},
      .seq_advance = 1,
      .degraded = false,
  });
}

TEST_CASE("assert_recoverable when transition not persisted",
          "[invariants]") {
  TempDir td;
  auto dir = td.path / "db";

  bytecask::testing::Baseline before;
  {
    auto db = bytecask::DB::open(dir);
    db.put({.sync = true}, to_bytes("pre"), to_bytes("existing"));
    before = capture_baseline(db);

    // Class B1: append fails before any bytes are written.
    {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_append"};
      REQUIRE_THROWS_AS(
          db.put({.sync = false}, to_bytes("new"), to_bytes("val")),
          std::system_error);
    }
  } // DB closes

  assert_recoverable(dir, before, ExpectedDelta{
      .keys_added = {},
      .keys_removed = {},
      .expected_values = {},
      .seq_advance = 0,
      .degraded = false,
  });
}

// ---------------------------------------------------------------------------
// capture_baseline test
// ---------------------------------------------------------------------------

TEST_CASE("capture_baseline captures correct state", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto bl = capture_baseline(db);
  CHECK(bl.key_values.size() == 2);
  CHECK(bl.key_values.contains("a"));
  CHECK(bl.key_values.contains("b"));
  CHECK(bytecask::testing::to_string(bl.key_values.at("a")) == "1");
  CHECK(bytecask::testing::to_string(bl.key_values.at("b")) == "2");
  CHECK(bl.next_seq == db.engine_state()->next_seq);
}

// ---------------------------------------------------------------------------
// validate_state_consistency tests
// ---------------------------------------------------------------------------

TEST_CASE("validate_state_consistency passes on valid state", "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  auto state = db.engine_state();
  REQUIRE_NOTHROW(db.test_validate_state_consistency(*state));
}

TEST_CASE("validate_state_consistency throws on regressed next_seq",
          "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  auto state = db.engine_state();

  // Construct a bad state with next_seq set to 0.
  auto bad = *state;
  bad.next_seq = 0;
  REQUIRE_THROWS_AS(db.test_validate_state_consistency(bad),
                    std::runtime_error);
}

TEST_CASE("validate_state_consistency throws on dangling file ref",
          "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  auto state = db.engine_state();

  // Construct a bad state: remove the active file from the registry
  // but keep key_dir pointing to it. We clear the active_file_id to
  // a non-existent file to trigger the "active file missing" check.
  auto bad = *state;
  bad.active_file_id = 999;
  REQUIRE_THROWS_AS(db.test_validate_state_consistency(bad),
                    std::runtime_error);
}

TEST_CASE("validate_state_consistency throws on live_bytes mismatch",
          "[invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  auto state = db.engine_state();

  // Corrupt live_bytes in file_stats.
  auto bad = *state;
  auto fstats_t = bad.file_stats.transient();
  fstats_t.update(bad.active_file_id, [](bytecask::FileStats &fs) {
    fs.live_bytes = 999999;
  });
  bad.file_stats = std::move(fstats_t).persistent();
  REQUIRE_THROWS_AS(db.test_validate_state_consistency(bad),
                    std::runtime_error);
}
