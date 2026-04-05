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
      std::lock_guard lk{mu};
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

  w.dispatch([] { throw std::runtime_error("oops"); });
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
// SyncGroup
// ---------------------------------------------------------------------------

TEST_CASE("SyncGroup single caller invokes sync exactly once", "[concurrency]") {
  bytecask::SyncGroup sg;
  std::atomic<int> calls{0};

  sg.sync([&] { ++calls; });

  CHECK(calls.load() == 1);
}

TEST_CASE("SyncGroup concurrent callers: sync called at least once, all return",
          "[concurrency]") {
  bytecask::SyncGroup sg;
  std::atomic<int> sync_calls{0};
  std::atomic<int> returned{0};
  constexpr int kThreads = 8;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      sg.sync([&] {
        ++sync_calls;
        // Simulate a brief sync cost so threads can pile up.
        std::this_thread::sleep_for(5ms);
      });
      ++returned;
    });
  }

  for (auto &t : threads) t.join();

  // Every caller must have returned.
  CHECK(returned.load() == kThreads);
  // The sync callable must have been called at least once.
  CHECK(sync_calls.load() >= 1);
  // With amortisation, we expect fewer calls than threads most of the time.
  // (Not a hard invariant, but a sanity check: at most kThreads calls.)
  CHECK(sync_calls.load() <= kThreads);
}

TEST_CASE("SyncGroup sync exception propagates to at most one caller",
          "[concurrency]") {
  // Single-threaded: the one caller (the leader) must see the exception.
  bytecask::SyncGroup sg;
  CHECK_THROWS_AS(sg.sync([] { throw std::runtime_error("sync failed"); }),
                  std::runtime_error);
}

TEST_CASE("SyncGroup remains usable after a sync exception", "[concurrency]") {
  bytecask::SyncGroup sg;
  std::atomic<int> calls{0};

  // First sync throws.
  CHECK_THROWS(sg.sync([] { throw std::runtime_error("transient"); }));

  // Subsequent sync must succeed.
  sg.sync([&] { ++calls; });
  CHECK(calls.load() == 1);
}
