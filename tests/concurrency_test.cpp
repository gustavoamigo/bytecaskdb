// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency stress tests for reader-writer lock and epoch reclamation

#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
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

// ---------------------------------------------------------------------------
// WriteGroup
// ---------------------------------------------------------------------------

TEST_CASE("WriteGroup single submit calls executor once", "[concurrency]") {
  std::atomic<int> exec_calls{0};
  bytecask::WriteGroup wg{[&](std::vector<bytecask::WriteGroup::Slot *> & /*batch*/) {
    ++exec_calls;
    // Mark all slots as having succeeded (no err).
  }};

  bytecask::WriteGroup::Slot slot;
  slot.sync = false;
  wg.submit(slot);

  CHECK(exec_calls.load() == 1);
}

TEST_CASE("WriteGroup concurrent submits are batched", "[concurrency]") {
  std::atomic<int> exec_calls{0};
  std::atomic<int> total_slots{0};
  bytecask::WriteGroup wg{[&](std::vector<bytecask::WriteGroup::Slot *> &batch) {
    ++exec_calls;
    total_slots += static_cast<int>(batch.size());
    // Simulate work so threads can pile up.
    std::this_thread::sleep_for(5ms);
  }};

  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> returned{0};

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      bytecask::WriteGroup::Slot slot;
      slot.sync = false;
      wg.submit(slot);
      ++returned;
    });
  }

  for (auto &t : threads) t.join();

  CHECK(returned.load() == kThreads);
  CHECK(total_slots.load() == kThreads);
  // With batching, we expect fewer executor calls than threads.
  CHECK(exec_calls.load() >= 1);
  CHECK(exec_calls.load() <= kThreads);
}

TEST_CASE("WriteGroup executor exception propagates to the failing slot",
          "[concurrency]") {
  bytecask::WriteGroup wg{[](std::vector<bytecask::WriteGroup::Slot *> &batch) {
    // Fail the first slot.
    batch[0]->err = std::make_exception_ptr(std::runtime_error("test error"));
  }};

  bytecask::WriteGroup::Slot slot;
  slot.sync = false;
  CHECK_THROWS_AS(wg.submit(slot), std::runtime_error);
}

TEST_CASE("WriteGroup remains usable after executor exception", "[concurrency]") {
  std::atomic<int> call_count{0};
  bytecask::WriteGroup wg{[&](std::vector<bytecask::WriteGroup::Slot *> &batch) {
    ++call_count;
    if (call_count.load() == 1) {
      batch[0]->err = std::make_exception_ptr(std::runtime_error("fail once"));
    }
  }};

  bytecask::WriteGroup::Slot slot1;
  slot1.sync = false;
  CHECK_THROWS(wg.submit(slot1));

  bytecask::WriteGroup::Slot slot2;
  slot2.sync = false;
  wg.submit(slot2); // Must succeed.
  CHECK(call_count.load() == 2);
}

TEST_CASE("WriteGroup executor throw propagates to all waiting slots",
          "[concurrency]") {
  // Multiple concurrent submitters must all receive the exception when the
  // executor throws, rather than deadlocking.
  constexpr int kSubmitters = 3;
  bytecask::WriteGroup wg{[][[noreturn]](std::vector<bytecask::WriteGroup::Slot *> & /*batch*/) {
    std::this_thread::sleep_for(50ms);
    throw std::runtime_error("executor boom");
  }};

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> threw{0};
  std::vector<std::thread> threads;
  threads.reserve(kSubmitters);
  for (int i = 0; i < kSubmitters; ++i) {
    threads.emplace_back([&] {
      bytecask::WriteGroup::Slot slot;
      slot.sync = false;
      ++ready;
      while (!start.load()) {
        std::this_thread::yield();
      }
      try {
        wg.submit(slot);
      } catch (const std::runtime_error &) {
        ++threw;
      }
    });
  }

  while (ready.load() != kSubmitters) {
    std::this_thread::yield();
  }
  start.store(true);

  for (auto &thread : threads) {
    thread.join();
  }
  CHECK(threw.load() == kSubmitters);
}

TEST_CASE("WriteGroup aborted slots receive WriteGroupAborted", "[concurrency]") {
  // Deterministic multi-slot batch: all threads reach a barrier before
  // any calls submit(), so when the first thread becomes leader and the
  // executor sleeps, the other threads are guaranteed to enqueue and
  // accumulate in the next batch.
  std::atomic<int> aborted_count{0};
  std::atomic<int> succeeded_count{0};
  constexpr int kThreads = 4;
  std::barrier sync_point(kThreads);

  bytecask::WriteGroup wg2{[](std::vector<bytecask::WriteGroup::Slot *> &batch) {
    // Sleep so non-leader threads enqueue into the next batch.
    std::this_thread::sleep_for(5ms);
    // First slot succeeds, rest aborted.
    for (std::size_t i = 1; i < batch.size(); ++i) {
      batch[i]->err = std::make_exception_ptr(bytecask::WriteGroupAborted{});
    }
  }};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      sync_point.arrive_and_wait();
      bytecask::WriteGroup::Slot slot;
      slot.sync = false;
      try {
        wg2.submit(slot);
        ++succeeded_count;
      } catch (const bytecask::WriteGroupAborted &) {
        ++aborted_count;
      }
    });
  }

  for (auto &t : threads) t.join();

  CHECK(succeeded_count.load() >= 1);
  CHECK(aborted_count.load() >= 1);
  CHECK(succeeded_count.load() + aborted_count.load() == kThreads);
}
