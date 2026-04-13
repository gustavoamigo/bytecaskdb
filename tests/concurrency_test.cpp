// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency stress tests for reader-writer lock and epoch reclamation

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>
import bytecask.concurrency;

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// BackgroundWorker
// ---------------------------------------------------------------------------

TEST_CASE("BackgroundWorker dispatched tasks execute", "[concurrency]") {
  bytecask::BackgroundWorker w;
  std::atomic<int> count{0};

  w.dispatch([&] { ++count; });
  w.dispatch([&] { ++count; });
  w.dispatch([&] { ++count; });
  w.drain();

  CHECK(count.load() == 3);
}

TEST_CASE("BackgroundWorker tasks execute in FIFO order", "[concurrency]") {
  bytecask::BackgroundWorker w;
  std::vector<int> order;
  std::mutex mu;

  for (int i = 0; i < 5; ++i) {
    w.dispatch([&, i] {
      std::lock_guard<std::mutex> lk{mu};
      order.push_back(i);
    });
  }
  w.drain();

  REQUIRE(order.size() == 5U);
  for (int i = 0; i < 5; ++i)
    CHECK(order[static_cast<std::size_t>(i)] == i);
}

TEST_CASE("BackgroundWorker exception in task is swallowed", "[concurrency]") {
  bytecask::BackgroundWorker w;
  std::atomic<int> after{0};

  w.dispatch([][[noreturn]] { throw std::runtime_error("oops"); });
  w.dispatch([&] { ++after; });
  w.drain();

  // The worker thread must still be alive and process the second task.
  CHECK(after.load() == 1);
}

TEST_CASE("BackgroundWorker drain returns immediately when idle", "[concurrency]") {
  bytecask::BackgroundWorker w;
  // No tasks dispatched — drain must return quickly.
  w.drain();
}

TEST_CASE("BackgroundWorker can dispatch after drain", "[concurrency]") {
  bytecask::BackgroundWorker w;
  std::atomic<int> count{0};

  w.dispatch([&] { ++count; });
  w.drain();
  CHECK(count.load() == 1);

  w.dispatch([&] { ++count; });
  w.drain();
  CHECK(count.load() == 2);
}
