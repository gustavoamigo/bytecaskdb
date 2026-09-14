// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitives: background worker, solo writer,
// group write batching.

module;
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

    if (!leader_active_) {
      leader_active_ = true;
      slot.lead = true;
    } else {
      cv_.wait(lk, [&] { return slot.done || slot.lead; });
    }
    if (slot.lead) lead(lk);

    if (slot.err) std::rethrow_exception(slot.err);
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
    std::vector<Slot *> batch;
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
    for (auto *s : batch) s->done = true;
    if (queue_.empty()) {
      leader_active_ = false;
    } else {
      queue_.front()->lead = true;
    }
    lk.unlock();
    cv_.notify_all();
  }

  std::function<void(std::vector<Slot *> &)> executor_;
  std::mutex queue_mu_;
  std::vector<Slot *> queue_;
  bool leader_active_{false};
  std::condition_variable cv_;
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

  std::mutex mu_;
  std::condition_variable cv_task_;
  std::condition_variable cv_idle_;
  std::queue<std::function<void()>> queue_;
  std::size_t active_{0};
  bool stop_{false};
  std::thread thread_;
};

#endif

} // namespace bytecask
