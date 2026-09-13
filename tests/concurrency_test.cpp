// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitive tests: BackgroundWorker, SoloWriter,
// WriteGroup

#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
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

// A leader runs at most kMaxLeaderBatches consecutive batches, then returns
// to its caller and passes leadership to a queued slot. Under an unbounded
// drain-until-empty leader, A would also run batch K+1 before returning, and
// that batch's executor — which waits for A to have returned — times out.
TEST_CASE("WriteGroup leader hands off after kMaxLeaderBatches", "[concurrency]") {
  constexpr int kMax = bytecask::WriteGroup::kMaxLeaderBatches;
  std::mutex mu;
  std::condition_variable cv;
  int followers_queued = 0;   // written by main thread under mu
  bool a_returned = false;
  std::atomic<int> exec_calls{0};
  std::thread::id a_id;
  std::vector<std::thread::id> leaders;

  bytecask::WriteGroup wg{[&](std::vector<bytecask::Slot *> &batch) {
    const int call = ++exec_calls;
    std::unique_lock<std::mutex> lk{mu};
    leaders.push_back(std::this_thread::get_id());
    if (call <= kMax) {
      // Hold this batch open until one more follower is queued behind it,
      // so the leader always finds a non-empty queue at the batch end.
      cv.wait(lk, [&] { return followers_queued >= call; });
    } else {
      // Batch K+1 may only start once A is back with its caller.
      REQUIRE(cv.wait_for(lk, std::chrono::seconds{5},
                          [&] { return a_returned; }));
    }
    (void)batch;
  }};

  std::thread a([&] {
    a_id = std::this_thread::get_id();
    bytecask::Slot slot;
    wg.submit(slot);
    {
      std::lock_guard<std::mutex> lk{mu};
      a_returned = true;
    }
    cv.notify_all();
  });

  // One follower per batch: each is queued while the previous batch runs.
  std::vector<std::thread> followers;
  for (int i = 1; i <= kMax; ++i) {
    while (exec_calls.load() < i) std::this_thread::yield();
    followers.emplace_back([&] {
      bytecask::Slot slot;
      wg.submit(slot);
    });
    wg.wait_for_queue_size(1);
    {
      std::lock_guard<std::mutex> lk{mu};
      ++followers_queued;
    }
    cv.notify_all();
  }

  a.join();
  for (auto &t : followers) t.join();

  CHECK(exec_calls.load() == kMax + 1);
  REQUIRE(leaders.size() == static_cast<std::size_t>(kMax + 1));
  for (int i = 0; i < kMax; ++i) CHECK(leaders[static_cast<std::size_t>(i)] == a_id);
  CHECK(leaders.back() != a_id);
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
