// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitive tests: BackgroundWorker, SoloWriter,
// WriteGroup

#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
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
// SoloWriter
// ---------------------------------------------------------------------------

TEST_CASE("SoloWriter submit calls executor once", "[concurrency]") {
  std::atomic<int> exec_calls{0};
  bytecask::SoloWriter sw{
      [&](std::vector<bytecask::Slot *> & /*batch*/) { ++exec_calls; }};

  bytecask::Slot slot;
  sw.submit(slot);
  CHECK(exec_calls.load() == 1);
}

TEST_CASE("SoloWriter serialises when executor locks", "[concurrency]") {
  std::mutex mu;
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};
  bytecask::SoloWriter sw{
      [&](std::vector<bytecask::Slot *> & /*batch*/) {
        std::lock_guard<std::mutex> lk{mu};
        auto c = ++concurrent;
        auto m = max_concurrent.load();
        while (c > m && !max_concurrent.compare_exchange_weak(m, c)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        --concurrent;
      }};

  constexpr int kThreads = 4;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      bytecask::Slot slot;
      sw.submit(slot);
    });
  }
  for (auto &t : threads) t.join();

  CHECK(max_concurrent.load() == 1);
}

TEST_CASE("SoloWriter executor exception propagates to caller", "[concurrency]") {
  bytecask::SoloWriter sw{
      [][[noreturn]](std::vector<bytecask::Slot *> & /*batch*/) {
        throw std::runtime_error("solo fail");
      }};

  bytecask::Slot slot;
  CHECK_THROWS_AS(sw.submit(slot), std::runtime_error);
}

TEST_CASE("SoloWriter remains usable after exception", "[concurrency]") {
  std::atomic<int> call_count{0};
  bytecask::SoloWriter sw{
      [&](std::vector<bytecask::Slot *> & /*batch*/) {
        if (++call_count == 1) throw std::runtime_error("fail once");
      }};

  bytecask::Slot slot1;
  CHECK_THROWS(sw.submit(slot1));

  bytecask::Slot slot2;
  sw.submit(slot2);
  CHECK(call_count.load() == 2);
}

// ---------------------------------------------------------------------------
// WriteGroup
// ---------------------------------------------------------------------------

TEST_CASE("WriteGroup single submit calls executor once", "[concurrency]") {
  std::atomic<int> exec_calls{0};
  bytecask::WriteGroup wg{[&](std::vector<bytecask::Slot *> & /*batch*/) {
    ++exec_calls;
  }};

  bytecask::Slot slot;
  wg.submit(slot);
  CHECK(exec_calls.load() == 1);
}

TEST_CASE("WriteGroup concurrent submits are batched", "[concurrency]") {
  std::atomic<int> exec_calls{0};
  std::atomic<int> total_slots{0};
  bytecask::WriteGroup wg{[&](std::vector<bytecask::Slot *> &batch) {
    ++exec_calls;
    total_slots += static_cast<int>(batch.size());
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }};

  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> returned{0};

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      bytecask::Slot slot;
      wg.submit(slot);
      ++returned;
    });
  }

  for (auto &t : threads) t.join();

  CHECK(returned.load() == kThreads);
  CHECK(total_slots.load() == kThreads);
  CHECK(exec_calls.load() >= 1);
  CHECK(exec_calls.load() <= kThreads);
}

TEST_CASE("WriteGroup executor exception propagates to the failing slot",
          "[concurrency]") {
  bytecask::WriteGroup wg{[](std::vector<bytecask::Slot *> &batch) {
    batch[0]->err = std::make_exception_ptr(std::runtime_error("test error"));
  }};

  bytecask::Slot slot;
  CHECK_THROWS_AS(wg.submit(slot), std::runtime_error);
}

TEST_CASE("WriteGroup remains usable after executor exception", "[concurrency]") {
  std::atomic<int> call_count{0};
  bytecask::WriteGroup wg{[&](std::vector<bytecask::Slot *> &batch) {
    ++call_count;
    if (call_count.load() == 1) {
      batch[0]->err = std::make_exception_ptr(std::runtime_error("fail once"));
    }
  }};

  bytecask::Slot slot1;
  CHECK_THROWS(wg.submit(slot1));

  bytecask::Slot slot2;
  wg.submit(slot2);
  CHECK(call_count.load() == 2);
}

TEST_CASE("WriteGroup executor throw propagates to all waiting slots",
          "[concurrency]") {
  constexpr int kSubmitters = 3;
  bytecask::WriteGroup wg{
      [][[noreturn]](std::vector<bytecask::Slot *> & /*batch*/) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        throw std::runtime_error("executor boom");
      }};

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> threw{0};
  std::vector<std::thread> threads;
  threads.reserve(kSubmitters);
  for (int i = 0; i < kSubmitters; ++i) {
    threads.emplace_back([&] {
      bytecask::Slot slot;
      ++ready;
      while (!start.load()) std::this_thread::yield();
      try {
        wg.submit(slot);
      } catch (const std::runtime_error &) {
        ++threw;
      }
    });
  }

  while (ready.load() != kSubmitters) std::this_thread::yield();
  start.store(true);

  for (auto &thread : threads) thread.join();
  CHECK(threw.load() == kSubmitters);
}

TEST_CASE("WriteGroup aborted slots receive WriteGroupAborted", "[concurrency]") {
  std::atomic<int> aborted_count{0};
  std::atomic<int> succeeded_count{0};
  constexpr int kThreads = 4;
  std::barrier<> sync_point(kThreads);

  bytecask::WriteGroup wg{[](std::vector<bytecask::Slot *> &batch) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    for (std::size_t i = 1; i < batch.size(); ++i) {
      batch[i]->err = std::make_exception_ptr(bytecask::WriteGroupAborted{});
    }
  }};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      sync_point.arrive_and_wait();
      bytecask::Slot slot;
      try {
        wg.submit(slot);
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

// ---------------------------------------------------------------------------
// WriteGroup — adaptive wait window
// ---------------------------------------------------------------------------

TEST_CASE("WriteGroup wait window collects concurrent submitters into one batch",
          "[concurrency]") {
  std::atomic<std::size_t> max_batch{0};
  std::atomic<int> exec_calls{0};
  constexpr int kThreads = 4;
  std::barrier<> sync_point(kThreads);

  bytecask::WriteGroup wg{
      [&](std::vector<bytecask::Slot *> &batch) {
        ++exec_calls;
        auto sz = batch.size();
        auto m = max_batch.load();
        while (sz > m && !max_batch.compare_exchange_weak(m, sz)) {
        }
      },
      {.max_wait = 50ms, .target_batch = 8}};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      sync_point.arrive_and_wait();
      bytecask::Slot slot;
      wg.submit(slot);
    });
  }
  for (auto &t : threads) t.join();

  CHECK(max_batch.load() >= 2);
  CHECK(exec_calls.load() <= kThreads);
}

TEST_CASE("WriteGroup wait exits early when target_batch is reached",
          "[concurrency]") {
  constexpr std::size_t kTarget = 3;
  constexpr int kThreads = static_cast<int>(kTarget);
  std::atomic<std::size_t> batch_size{0};
  std::barrier<> sync_point(kThreads);

  bytecask::WriteGroup wg{
      [&](std::vector<bytecask::Slot *> &batch) {
        batch_size.store(batch.size());
      },
      {.max_wait = 5s, .target_batch = kTarget}};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      sync_point.arrive_and_wait();
      bytecask::Slot slot;
      wg.submit(slot);
    });
  }

  auto start = std::chrono::steady_clock::now();
  for (auto &t : threads) t.join();
  auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(batch_size.load() == kTarget);
  CHECK(elapsed < 1s);
}

TEST_CASE("WriteGroup no wait under high load (EMA adapts)", "[concurrency]") {
  constexpr int kRounds = 20;
  constexpr int kThreads = 8;

  std::atomic<int> exec_calls{0};
  bytecask::WriteGroup wg{
      [&](std::vector<bytecask::Slot *> & /*batch*/) {
        ++exec_calls;
        std::this_thread::sleep_for(1ms);
      },
      {.max_wait = 500ms, .target_batch = 4}};

  for (int round = 0; round < kRounds; ++round) {
    std::barrier<> sync_point(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&] {
        sync_point.arrive_and_wait();
        bytecask::Slot slot;
        wg.submit(slot);
      });
    }
    for (auto &t : threads) t.join();
  }

  auto start = std::chrono::steady_clock::now();
  std::barrier<> final_sync(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      final_sync.arrive_and_wait();
      bytecask::Slot slot;
      wg.submit(slot);
    });
  }
  for (auto &t : threads) t.join();
  auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(elapsed < 100ms);
}
