// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitives: background worker, solo writer,
// group write batching.

module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
  std::exception_ptr err;
  // WriteGroup only: the word a queued slot's owner waits on. See
  // WriteGroup::release for why it has three values rather than two.
  std::atomic<std::uint32_t> state{0};
};

// Spin-wait hint: lets a sibling hyperthread run and saves power while a
// thread polls a word another core will change.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

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
// WriteGroup — group commit through one serial section (Template Method).
//
// Every write passes through the executor one batch at a time. A writer that
// finds no batch running runs one itself, inline: it takes everything queued,
// its own slot included, as one batch, then returns to its caller — a lone
// writer never sleeps and never wakes another thread. Slots queued while a
// batch runs are taken by the committer thread, which runs batch after batch
// until the queue is empty and then waits for the next hand-over.
//
// Why a committer rather than handing the next batch to a queued writer: the
// next batch cannot start until the thread that runs it is on a CPU. A
// queued writer is asleep, and waking it put a scheduler round trip —
// measured at ~300 µs per batch on a busy host — between every two batches.
// The committer is already running when a queue forms (it spins briefly
// before sleeping), so under load batches run back to back. Fairness is
// unchanged: an inline writer still runs exactly one batch before returning,
// and the committer has no caller of its own to keep waiting.
//
// The domain-specific batch execution logic is injected via a BatchExecutor
// callback at construction time. submit() is non-template — it takes a Slot&.
// Under BYTECASK_SINGLE_THREADED there is no committer: with one thread, no
// slot is ever queued behind a running batch.
// ---------------------------------------------------------------------------
export class WriteGroup {
public:
  explicit WriteGroup(
      std::function<void(std::vector<Slot *> &)> executor)
      : executor_{std::move(executor)} {
#ifndef BYTECASK_SINGLE_THREADED
    committer_ = std::thread{[this] { committer_loop(); }};
#endif
  }

  // Stops the committer. No submit may be in flight.
  ~WriteGroup() {
#ifndef BYTECASK_SINGLE_THREADED
    {
      std::lock_guard<std::mutex> lk{queue_mu_};
      stop_ = true;
    }
    committer_cv_.notify_one();
    committer_.join();
#endif
  }

  WriteGroup(const WriteGroup &) = delete;
  WriteGroup &operator=(const WriteGroup &) = delete;

#ifdef BYTECASK_TESTING
  // Test-only hook: called on the thread about to run a batch (an inline
  // writer or the committer) before it takes the queue. Allows a second
  // thread to enqueue its slot deterministically into the same batch.
  std::function<void()> on_batch_start_;

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

  // Enqueues slot and returns once the executor has run it. Rethrows the
  // slot's error, if it has one.
  void submit(Slot &slot) {
    slot.done = false;
    slot.err = nullptr;
    slot.state.store(kQueued, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk{queue_mu_};
    queue_.push_back(&slot);
    inflight_.fetch_add(1, std::memory_order_relaxed);
    if (slot.sync) inflight_sync_.fetch_add(1, std::memory_order_relaxed);

    if (running_) {
      lk.unlock();
      wait_released(slot);
    } else {
      running_ = true;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
      // Reused so an inline commit allocates nothing here.
      thread_local std::vector<Slot *> batch;
#pragma clang diagnostic pop
      run_batch(lk, batch);
#ifdef BYTECASK_SINGLE_THREADED
      while (!queue_.empty()) run_batch(lk, batch);
      running_ = false;
      lk.unlock();
#else
      // Slots queued while this batch ran go to the committer; running_
      // stays set so no inline writer overtakes them.
      const bool more = !queue_.empty();
      if (more) {
        hand_over_.store(true, std::memory_order_release);
      } else {
        running_ = false;
      }
      const bool wake = more && committer_sleeping_;
      lk.unlock();
      if (wake) committer_cv_.notify_one();
#endif
      release(batch, &slot);
    }
    if (slot.err) std::rethrow_exception(slot.err);
  }

  // True while any submitted slot has not been executed yet: queued, or in
  // the batch being run. Lock-free so the commit pipeline's flusher can let
  // an in-progress batch land before it captures the head.
  [[nodiscard]] auto busy() const noexcept -> bool {
    return inflight_.load(std::memory_order_acquire) > 0;
  }

  // True while a submitted slot that asked for a sync has not been executed
  // yet: the next flush will fdatasync for it. Lock-free, like busy().
  [[nodiscard]] auto sync_busy() const noexcept -> bool {
    return inflight_sync_.load(std::memory_order_acquire) > 0;
  }

private:
  static constexpr std::uint32_t kQueued = 0;
  static constexpr std::uint32_t kReleasing = 1;
  static constexpr std::uint32_t kDone = 2;
  // A queued unsynced writer polls its slot this long before sleeping: a
  // batch ahead of it that finishes within the window costs it no scheduler
  // round trip. A synced writer sleeps at once (see wait_released).
  static constexpr auto kWriterSpin = std::chrono::microseconds{10};
  // The committer polls for the next hand-over this long before sleeping, so
  // under steady load it is running when a queue forms.
  static constexpr auto kCommitterSpin = std::chrono::microseconds{50};

  // Takes the queue as one batch and runs it. Called and returns with lk
  // held and running_ set; drops lk around the executor.
  void run_batch(std::unique_lock<std::mutex> &lk, std::vector<Slot *> &batch) {
#ifdef BYTECASK_TESTING
    if (on_batch_start_) {
      lk.unlock();
      on_batch_start_();
      lk.lock();
    }
#endif
    batch.clear();
    batch.swap(queue_);
    lk.unlock();

    try {
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
  }

  // Returns every slot of a finished batch but `self` (the running thread's
  // own) to its owner. kDone is the last write to a slot: its owner returns
  // only on kDone, and may destroy the slot the moment it does, so the
  // notify — which needs the slot alive — happens under kReleasing, while
  // the owner is still bound to wait.
  static void release(const std::vector<Slot *> &batch, const Slot *self) {
    for (auto *s : batch) {
      if (s == self) continue;
      s->state.store(kReleasing, std::memory_order_release);
      s->state.notify_one();
      s->state.store(kDone, std::memory_order_release);
    }
  }

  static void wait_released(Slot &slot) {
    // A synced writer does not poll: released at once, it would reach the
    // flush wait before the flush covering it publishes, and sleep there.
    const auto spin_until = std::chrono::steady_clock::now() +
        (slot.sync ? std::chrono::microseconds{0} : kWriterSpin);
    bool spinning = true;
    for (;;) {
      const auto s = slot.state.load(std::memory_order_acquire);
      if (s == kDone) return;
      if (spinning && std::chrono::steady_clock::now() >= spin_until)
        spinning = false;
      if (s == kQueued && !spinning)
        slot.state.wait(kQueued, std::memory_order_acquire);
      else
        cpu_relax();  // spinning, or the releaser is between notify and kDone
    }
  }

#ifndef BYTECASK_SINGLE_THREADED
  void committer_loop() {
    std::vector<Slot *> batch;
    std::unique_lock<std::mutex> lk{queue_mu_};
    for (;;) {
      if (!hand_over_.load(std::memory_order_relaxed)) {
        lk.unlock();
        const auto spin_until = std::chrono::steady_clock::now() + kCommitterSpin;
        while (!hand_over_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < spin_until)
          cpu_relax();
        lk.lock();
        while (!hand_over_.load(std::memory_order_relaxed) && !stop_) {
          committer_sleeping_ = true;
          committer_cv_.wait(lk);
          committer_sleeping_ = false;
        }
        if (!hand_over_.load(std::memory_order_relaxed)) return;  // stop_
      }
      hand_over_.store(false, std::memory_order_relaxed);
      // running_ is this thread's until the queue is empty.
      for (;;) {
        run_batch(lk, batch);
        const bool more = !queue_.empty();
        if (!more) running_ = false;
        lk.unlock();
        release(batch, nullptr);
        lk.lock();
        if (!more) break;
      }
    }
  }
#endif

  std::function<void(std::vector<Slot *> &)> executor_;
  std::mutex queue_mu_;
  std::vector<Slot *> queue_;
  // A batch is running, or the queue has been handed to the committer.
  // Guarded by queue_mu_.
  bool running_{false};
  std::atomic<int> inflight_{0};
  std::atomic<int> inflight_sync_{0};  // the subset of inflight_ with sync
#ifndef BYTECASK_SINGLE_THREADED
  // Set, under queue_mu_, when an inline writer leaves slots queued behind
  // its batch; read without the lock by the spinning committer.
  std::atomic<bool> hand_over_{false};
  bool committer_sleeping_{false};  // guarded by queue_mu_
  bool stop_{false};                // guarded by queue_mu_
  std::condition_variable committer_cv_;
  std::thread committer_;
#endif
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
