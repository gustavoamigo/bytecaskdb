// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_count_test.cpp — Unit tests for the per-table cached row count counters.
//
// The counter API lives in bytecaskdb_plugin.cc but depends on MariaDB headers.
// This file re-implements the same atomic counter pattern in isolation to
// verify correctness of the increment/decrement/reset/drop lifecycle.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

static std::mutex s_mu;
static std::map<uint32_t, std::unique_ptr<std::atomic<int64_t>>> s_counters;

std::atomic<int64_t> &counter_for(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_mu};
  auto it = s_counters.find(table_id);
  if (it == s_counters.end()) {
    auto [ins, _] = s_counters.emplace(
        table_id, std::make_unique<std::atomic<int64_t>>(0));
    it = ins;
  }
  return *it->second;
}

int64_t row_count(uint32_t table_id) {
  return counter_for(table_id).load();
}

void row_count_add(uint32_t table_id, int64_t delta) {
  counter_for(table_id).fetch_add(delta);
}

void row_count_reset(uint32_t table_id) {
  counter_for(table_id).store(0);
}

void row_count_drop(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_mu};
  s_counters.erase(table_id);
}

struct CounterFixture {
  CounterFixture() {
    std::lock_guard<std::mutex> lk{s_mu};
    s_counters.clear();
  }
  ~CounterFixture() {
    std::lock_guard<std::mutex> lk{s_mu};
    s_counters.clear();
  }
};

} // namespace

TEST_CASE_METHOD(CounterFixture, "Row count starts at zero", "[row_count]") {
  REQUIRE(row_count(1) == 0);
}

TEST_CASE_METHOD(CounterFixture, "Increment and decrement", "[row_count]") {
  row_count_add(1, 1);
  row_count_add(1, 1);
  row_count_add(1, 1);
  REQUIRE(row_count(1) == 3);

  row_count_add(1, -1);
  REQUIRE(row_count(1) == 2);

  row_count_add(1, -2);
  REQUIRE(row_count(1) == 0);
}

TEST_CASE_METHOD(CounterFixture, "Reset sets count to zero", "[row_count]") {
  row_count_add(1, 100);
  REQUIRE(row_count(1) == 100);

  row_count_reset(1);
  REQUIRE(row_count(1) == 0);
}

TEST_CASE_METHOD(CounterFixture, "Drop removes counter entirely", "[row_count]") {
  row_count_add(1, 50);
  row_count_drop(1);
  REQUIRE(row_count(1) == 0);
}

TEST_CASE_METHOD(CounterFixture, "Independent per-table counters", "[row_count]") {
  row_count_add(1, 10);
  row_count_add(2, 20);
  row_count_add(3, 30);

  REQUIRE(row_count(1) == 10);
  REQUIRE(row_count(2) == 20);
  REQUIRE(row_count(3) == 30);

  row_count_add(2, -5);
  REQUIRE(row_count(1) == 10);
  REQUIRE(row_count(2) == 15);
  REQUIRE(row_count(3) == 30);
}

TEST_CASE_METHOD(CounterFixture, "Decrement below zero is clamped by caller", "[row_count]") {
  row_count_add(1, 2);
  row_count_add(1, -5);
  // Counter goes negative — caller (info()) uses std::max(0, ...) to clamp
  REQUIRE(row_count(1) == -3);
  REQUIRE(std::max(int64_t{0}, row_count(1)) == 0);
}

TEST_CASE_METHOD(CounterFixture, "Concurrent increments are atomic", "[row_count]") {
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 10000;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kOpsPerThread; ++i) {
        row_count_add(1, 1);
      }
    });
  }
  for (auto &t : threads) { t.join(); }

  REQUIRE(row_count(1) == kThreads * kOpsPerThread);
}

TEST_CASE_METHOD(CounterFixture, "Concurrent increment and decrement", "[row_count]") {
  constexpr int kOps = 10000;

  row_count_add(1, kOps);

  std::thread inserter([&] {
    for (int i = 0; i < kOps; ++i) { row_count_add(1, 1); }
  });
  std::thread deleter([&] {
    for (int i = 0; i < kOps; ++i) { row_count_add(1, -1); }
  });

  inserter.join();
  deleter.join();

  REQUIRE(row_count(1) == kOps);
}
