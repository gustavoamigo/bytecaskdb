// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitives: background worker, solo writer,
// group write batching.

module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

export module bytecask.concurrency;
#ifndef BYTECASK_EXP_TARGETED_WAKE
#define BYTECASK_EXP_TARGETED_WAKE 0
#endif

// EXPERIMENT ONLY: timing of the group-commit path.
export struct ExpTimers {
  static inline std::atomic<std::int64_t> exec_ns{0};
  static inline std::atomic<std::int64_t> handoff_ns{0};
  static inline std::atomic<std::int64_t> handoffs{0};
  static inline std::atomic<std::int64_t> mu_wait_ns{0};
  static inline std::atomic<std::int64_t> append_ns{0};
  static inline std::atomic<std::int64_t> rotate_ns{0};
  static inline std::atomic<std::int64_t> rotate_n{0};
};
export inline auto exp_now_ns() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace bytecask {

// ---------------------------------------------------------------------------
// WriteGroupAborted — thrown to callers whose slot was not executed
// because a prior slot in the same batch failed.
// ---------------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
export class WriteGroupAborted : public std::runtime_error {
public:
  WriteGroupAborted()
      : std::runtime_error(
            "bytecask: write group aborted — operation was not attempted") {}
};
#pragma clang diagnostic pop

// ---------------------------------------------------------------------------
// Slot — base type for writer submit interfaces.
//
// Carries sync/done/err fields. The engine extends it (e.g. EngineSlot) with
// domain-specific data; executors static_cast Slot* to the derived type.
// ---------------------------------------------------------------------------
export struct Slot {
  bool sync{false};
  bool done{false};
  bool lead{false};  // set by WriteGroup when this slot is handed leadership
  std::exception_ptr err;
  // EXPERIMENT E1: 0 waiting, 1 leads, 2 done — the word the owner sleeps on.
  std::atomic<std::uint32_t> wake{0};
};

// ---------------------------------------------------------------------------
// SoloWriter — single-slot writer with the same submit() interface as
// WriteGroup.
//
// Wraps the single slot in a one-element vector and calls the same executor
// signature as WriteGroup. No batching, no internal mutex — the executor is
// responsible for its own serialisation (e.g. acquiring write_mu_). Provides
// a uniform interface so the engine can share a single executor implementation.
// ---------------------------------------------------------------------------
export class SoloWriter {
public:
  explicit SoloWriter(
      std::function<void(std::vector<Slot *> &)> executor)
      : executor_{std::move(executor)} {}

  SoloWriter(const SoloWriter &) = delete;
  SoloWriter &operator=(const SoloWriter &) = delete;

  void submit(Slot &slot) {
    slot.done = false;
    slot.err = nullptr;
    std::vector<Slot *> batch{&slot};
    try {
      executor_(batch);
    } catch (...) {
      slot.err = std::current_exception();
    }
    slot.done = true;
    if (slot.err) std::rethrow_exception(slot.err);
  }

private:
  std::function<void(std::vector<Slot *> &)> executor_;
};

// ---------------------------------------------------------------------------
// WriteGroup — leader-applies-all write batching (Template Method pattern).
//
// The algorithm skeleton lives here: enqueue → elect leader → take the queue
// as one batch → call executor → mark done → hand leadership to the head of
// the queue → wake. The domain-specific batch execution logic is injected via
// a BatchExecutor callback at construction time.
//
// submit() is non-template — it takes a Slot&.
// ---------------------------------------------------------------------------
export class WriteGroup {
public:
  explicit WriteGroup(
      std::function<void(std::vector<Slot *> &)> executor)
      : executor_{std::move(executor)} {}

  WriteGroup(const WriteGroup &) = delete;
  WriteGroup &operator=(const WriteGroup &) = delete;

#ifdef BYTECASK_TESTING
  // Test-only hook: called after the leader is elected but before it takes
  // the queue as its batch. Allows a second thread to enqueue its slot
  // deterministically into the same batch.
  std::function<void()> on_leader_start_;

  // Block until the internal queue has at least n entries.
  void wait_for_queue_size(std::size_t n) {
    while (true) {
      {
        std::lock_guard<std::mutex> lk{queue_mu_};
        if (queue_.size() >= n) return;
      }
      std::this_thread::yield();
    }
  }
#endif

  // Enqueues slot and blocks until it is done. A submitter that finds no
  // leader becomes one and runs a single batch — everything queued at that
  // moment, its own slot included — then returns to its caller. If slots
  // were queued while the batch ran, leadership passes to the head of the
  // queue in the same broadcast that releases the batch.
  //
  // One batch per leader, not a run of them. The leader's own slot is done
  // after its batch, so every further batch it ran was time its caller
  // waited — and under closed-loop clients that batch released every other
  // writer at once, leaving the queue empty for the round trip each needed
  // before resubmitting. Measured on MariaDB sysbench oltp_write_only, 8
  // clients, a leader allowed 8 consecutive batches alternated batches of 1
  // and 7 with ~220 µs between batches; a hand-off after every batch gave
  // uniform batches of ~4 and ~70 µs.
  void submit(Slot &slot) {
    std::unique_lock<std::mutex> lk{queue_mu_};
    slot.done = false;
    slot.lead = false;
    slot.err = nullptr;
    queue_.push_back(&slot);
    inflight_.fetch_add(1, std::memory_order_relaxed);
    if (slot.sync) inflight_sync_.fetch_add(1, std::memory_order_relaxed);

    slot.wake.store(0, std::memory_order_relaxed);
    if (!leader_active_) {
      leader_active_ = true;
      slot.lead = true;
    } else if (kTargetedWake) {
      lk.unlock();
      std::uint32_t w;
      while ((w = slot.wake.load(std::memory_order_acquire)) == 0)
        slot.wake.wait(0, std::memory_order_acquire);
      if (w == 1) {
        lk.lock();
        slot.lead = true;
      }
    } else {
      cv_.wait(lk, [&] { return slot.done || slot.lead; });
    }
    if (slot.lead) lead(lk);

    if (slot.err) std::rethrow_exception(slot.err);
  }

  // True while any submitted slot has not been executed yet: queued, or in
  // the batch the leader is running. Lock-free so the commit pipeline's
  // flusher can let an in-progress batch land before it captures the head.
  [[nodiscard]] auto busy() const noexcept -> bool {
    return inflight_.load(std::memory_order_acquire) > 0;
  }

  // True while a submitted slot that asked for a sync has not been executed
  // yet: the next flush will fdatasync for it. Lock-free, like busy().
  [[nodiscard]] auto sync_busy() const noexcept -> bool {
    return inflight_sync_.load(std::memory_order_acquire) > 0;
  }

private:
  // Runs one batch. Called with lk held and leader_active_ set; drops lk
  // around the executor call and returns with lk released. One broadcast
  // wakes the finished batch and the next leader together — a wake per slot
  // measured 2× slower at 32 writers.
  void lead(std::unique_lock<std::mutex> &lk) {
#ifdef BYTECASK_TESTING
    if (on_leader_start_) {
      lk.unlock();
      on_leader_start_();
      lk.lock();
    }
#endif
    const auto t_start = exp_now_ns();
    if (handoff_at_ != 0) {
      ExpTimers::handoff_ns.fetch_add(t_start - handoff_at_, std::memory_order_relaxed);
      ExpTimers::handoffs.fetch_add(1, std::memory_order_relaxed);
      handoff_at_ = 0;
    }
    std::vector<Slot *> batch;
    batch.swap(queue_);
    lk.unlock();

    struct ExecTimer {
      std::int64_t t0 = exp_now_ns();
      ~ExecTimer() { ExpTimers::exec_ns.fetch_add(exp_now_ns() - t0, std::memory_order_relaxed); }
    };
    try {
      ExecTimer et;
      executor_(batch);
    } catch (...) {
      auto ex = std::current_exception();
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
    }

    lk.lock();
    int synced = 0;
    for (auto *s : batch) {
      synced += s->sync ? 1 : 0;
      s->done = true;
    }
    inflight_.fetch_sub(static_cast<int>(batch.size()),
                        std::memory_order_release);
    inflight_sync_.fetch_sub(synced, std::memory_order_release);
    Slot *next = nullptr;
    if (queue_.empty()) {
      leader_active_ = false;
    } else {
      next = queue_.front();
      if (!kTargetedWake) next->lead = true;
      handoff_at_ = exp_now_ns();
    }
    lk.unlock();
    if (!kTargetedWake) {
      cv_.notify_all();
      return;
    }
    // The next leader first: it is on the critical path, the batch is not.
    if (next) {
      next->wake.store(1, std::memory_order_release);
      next->wake.notify_one();
    }
    for (auto *s : batch) {
      if (s->lead) continue;  // this thread's own slot
      s->wake.store(2, std::memory_order_release);
      s->wake.notify_one();
    }
  }
  static constexpr bool kTargetedWake = BYTECASK_EXP_TARGETED_WAKE;

  std::function<void(std::vector<Slot *> &)> executor_;
  std::mutex queue_mu_;
  std::vector<Slot *> queue_;
  bool leader_active_{false};
  std::int64_t handoff_at_{0};
  std::condition_variable cv_;
  std::atomic<int> inflight_{0};
  std::atomic<int> inflight_sync_{0};  // the subset of inflight_ with sync
};

// ---------------------------------------------------------------------------
// BackgroundWorker — single persistent background thread for deferred work.
//
// Tasks are enqueued via dispatch() and executed in FIFO order. dispatch() is
// non-blocking; the caller returns immediately after enqueuing. Exceptions
// thrown by tasks are caught, logged to stderr, and swallowed — hint file
// writes are correctness-safe to drop (recovery falls back to raw data scan).
//
// Lifecycle: the thread starts at construction and joins at destruction.
// drain() blocks until the queue is empty and the last task has finished.
//
// When BYTECASK_SINGLE_THREADED is defined, no thread is spawned. dispatch()
// runs tasks immediately on the calling thread. Intended for environments
// without thread support (e.g. WebAssembly without pthreads).
//
// Declare BackgroundWorker as the LAST member of any owning class so that
// it destructs first, ensuring the background thread joins before any other
// member is destroyed.
// ---------------------------------------------------------------------------

#ifdef BYTECASK_SINGLE_THREADED

export class BackgroundWorker {
public:
  BackgroundWorker() = default;
  ~BackgroundWorker() = default;

  BackgroundWorker(const BackgroundWorker &) = delete;
  BackgroundWorker &operator=(const BackgroundWorker &) = delete;

  void dispatch(std::function<void()> task) {
    try {
      task();
    } catch (const std::exception &e) {
      std::cerr << "bytecask: background worker exception: " << e.what()
                << "\n";
    } catch (...) {
      std::cerr << "bytecask: background worker: unknown exception\n";
    }
  }

  void drain() {}

  // Tasks run inline, so nothing is ever pending.
  [[nodiscard]] auto pending() const noexcept -> std::size_t { return 0; }
  void wait_pending_below(std::size_t) {}
};

#else

export class BackgroundWorker {
public:
  BackgroundWorker() : thread_{[this] { run(); }} {}

  ~BackgroundWorker() {
    {
      std::unique_lock<std::mutex> lk{mu_};
      stop_ = true;
    }
    cv_task_.notify_one();
    thread_.join();
  }

  BackgroundWorker(const BackgroundWorker &) = delete;
  BackgroundWorker &operator=(const BackgroundWorker &) = delete;

  // Enqueue a task. Non-blocking; returns immediately.
  void dispatch(std::function<void()> task) {
    {
      std::unique_lock<std::mutex> lk{mu_};
      queue_.push(std::move(task));
    }
    cv_task_.notify_one();
  }

  // Block until the queue is empty and the running task (if any) has finished.
  void drain() {
    std::unique_lock<std::mutex> lk{mu_};
    cv_idle_.wait(lk, [this] { return queue_.empty() && active_ == 0; });
  }

  // Tasks queued plus the one running, if any.
  [[nodiscard]] auto pending() const -> std::size_t {
    std::unique_lock<std::mutex> lk{mu_};
    return queue_.size() + active_;
  }

  // Block until fewer than n tasks are queued or running. n == 0 would never
  // return, so it is treated as 1: wait for the worker to go idle.
  void wait_pending_below(std::size_t n) {
    std::unique_lock<std::mutex> lk{mu_};
    cv_idle_.wait(lk, [this, n] {
      return queue_.size() + active_ < std::max<std::size_t>(n, 1);
    });
  }

private:
  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk{mu_};
        cv_task_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty())
          return;
        task = std::move(queue_.front());
        queue_.pop();
        ++active_;
      }
      try {
        task();
      } catch (const std::exception &e) {
        std::cerr << "bytecask: background worker exception: " << e.what()
                  << "\n";
      } catch (...) {
        std::cerr << "bytecask: background worker: unknown exception\n";
      }
      {
        std::unique_lock<std::mutex> lk{mu_};
        --active_;
      }
      cv_idle_.notify_all();
    }
  }

  mutable std::mutex mu_;
  std::condition_variable cv_task_;
  std::condition_variable cv_idle_;
  std::queue<std::function<void()>> queue_;
  std::size_t active_{0};
  bool stop_{false};
  std::thread thread_;
};

#endif

} // namespace bytecask
