// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Behavioral tests for the stable C API (include/bytecask_c.h), exercised
// through C++ so Catch2 can drive them. Validates status codes, nullable
// write-options/result out-params, and CommitResult/durable_sequence
// semantics across the C ABI boundary (BC-231 Phase 3).

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>

#include "../include/bytecask_c.h"

namespace {

struct TempDir {
  std::filesystem::path path;

  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "bc_c_test_{}_{}",
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

auto bytes_of(std::string_view sv) -> const uint8_t * {
  return reinterpret_cast<const uint8_t *>(sv.data());
}

} // namespace

TEST_CASE("bytecask_put with null opts/out succeeds", "[c_api]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  int rc = bytecask_put(db, bytes_of("k"), 1, bytes_of("v"), 1, nullptr, nullptr);
  CHECK(rc == 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_put fills CommitResult with a positive sequence",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  bytecask_commit_result_t result{.sequence = 999, .durable = 0};
  int rc = bytecask_put(db, bytes_of("k"), 1, bytes_of("v"), 1, nullptr, &result);
  CHECK(rc == 0);
  CHECK(result.sequence > 0);
  CHECK(result.durable != 0); // default opts: sync = true

  bytecask_close(db);
}

TEST_CASE("bytecask_put with explicit nosync options reports not durable",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  bytecask_write_options_t opts{.sync = 0, .solo = 1};
  bytecask_commit_result_t result{};
  int rc = bytecask_put(db, bytes_of("k"), 1, bytes_of("v"), 1, &opts, &result);
  CHECK(rc == 0);
  CHECK(result.sequence > 0);
  CHECK(result.durable == 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_del of absent key returns 0 and does not fill out",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  bytecask_commit_result_t result{.sequence = 12345, .durable = 1};
  int rc = bytecask_del(db, bytes_of("nope"), 4, nullptr, &result);
  CHECK(rc == 0);
  // out is cleared to {0, 0} on entry regardless of outcome.
  CHECK(result.sequence == 0);
  CHECK(result.durable == 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_del of existing key returns 1 and fills out",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  REQUIRE(bytecask_put(db, bytes_of("k"), 1, bytes_of("v"), 1, nullptr, nullptr) == 0);

  bytecask_commit_result_t result{};
  int rc = bytecask_del(db, bytes_of("k"), 1, nullptr, &result);
  CHECK(rc == 1);
  CHECK(result.sequence > 0);
  CHECK(result.durable != 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_del_range with from >= to returns durable zero sequence",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  bytecask_commit_result_t result{.sequence = 7, .durable = 0};
  int rc = bytecask_del_range(db, bytes_of("z"), 1, bytes_of("a"), 1, nullptr, &result);
  CHECK(rc == 0);
  CHECK(result.sequence == 0);
  CHECK(result.durable != 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_apply_batch commits and fills out on success",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  auto *plan = bytecask_write_plan_new();
  REQUIRE(plan != nullptr);
  bytecask_write_plan_put(plan, bytes_of("k1"), 2, bytes_of("v1"), 2);
  bytecask_write_plan_put(plan, bytes_of("k2"), 2, bytes_of("v2"), 2);

  bytecask_commit_result_t result{};
  int rc = bytecask_apply_batch(db, plan, nullptr, &result);
  CHECK(rc == 1);
  CHECK(result.sequence > 0);
  CHECK(result.durable != 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_apply_batch conflict returns 0 and does not fill out",
          "[c_api][commit_result]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  REQUIRE(bytecask_put(db, bytes_of("k"), 1, bytes_of("v0"), 2, nullptr, nullptr) == 0);
  auto *snap = bytecask_snapshot(db);
  REQUIRE(snap != nullptr);

  // Modify "k" after the snapshot was taken.
  REQUIRE(bytecask_put(db, bytes_of("k"), 1, bytes_of("v1"), 2, nullptr, nullptr) == 0);

  auto *plan = bytecask_write_plan_new_with_snapshot(snap);
  REQUIRE(plan != nullptr);
  REQUIRE(bytecask_write_plan_ensure_unchanged(plan, bytes_of("k"), 1) == 0);
  bytecask_write_plan_put(plan, bytes_of("k"), 1, bytes_of("v2"), 2);

  bytecask_commit_result_t result{.sequence = 42, .durable = 1};
  int rc = bytecask_apply_batch(db, plan, nullptr, &result);
  CHECK(rc == 0);
  CHECK(result.sequence == 0);
  CHECK(result.durable == 0);

  bytecask_close(db);
}

TEST_CASE("bytecask_durable_sequence returns immediately for target 0",
          "[c_api][durable_seq]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  CHECK(bytecask_durable_sequence(db, 0, 0) == 0);

  bytecask_commit_result_t result{};
  REQUIRE(bytecask_put(db, bytes_of("k"), 1, bytes_of("v"), 1, nullptr, &result) == 0);
  CHECK(bytecask_durable_sequence(db, 0, 0) == result.sequence);
  CHECK(bytecask_durable_sequence(db, result.sequence, 5000) == result.sequence);

  bytecask_close(db);
}

TEST_CASE("bytecask_durable_sequence times out on an unreached target",
          "[c_api][durable_seq]") {
  TempDir td;
  auto *db = bytecask_open(td.path.string().c_str(), 0);
  REQUIRE(db != nullptr);

  uint64_t seq = bytecask_durable_sequence(db, 1, 50);
  CHECK(seq < 1);

  bytecask_close(db);
}

TEST_CASE("bytecask_durable_sequence on null db returns 0 and sets error",
          "[c_api][durable_seq]") {
  CHECK(bytecask_durable_sequence(nullptr, 0, 0) == 0);
  CHECK(bytecask_errmsg() != nullptr);
}
