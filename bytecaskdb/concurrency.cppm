// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — reader-writer lock and epoch-based memory reclamation

module;
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
// WriteGroupAborted — thrown to callers whose lambda was not executed
// because a prior lambda in the same batch failed.
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
// WriteGroup — leader-applies-all write batching (Template Method pattern).
//
// The algorithm skeleton lives here: enqueue → elect leader → drain queue →
// call executor → mark done → wake → loop until empty. The domain-specific
// batch execution logic is injected via a BatchExecutor callback at
// construction time.
//
// Slot is a minimal base struct carrying only sync/done/err. The engine
// extends it (e.g. EngineSlot) with a typed lambda; the executor
// static_cast's Slot* to the derived type.
//
// submit() is non-template — it takes a Slot&.
// ---------------------------------------------------------------------------
export class WriteGroup {
public:
  struct Slot {
    bool sync{false};
    bool done{false};
    std::exception_ptr err;
  };

  using BatchExecutor = std::move_only_function<void(std::vector<Slot *> &)>;

  explicit WriteGroup(BatchExecutor executor)
      : executor_{std::move(executor)} {}

  WriteGroup(const WriteGroup &) = delete;
  WriteGroup &operator=(const WriteGroup &) = delete;

  // Enqueue a slot and block until the batch containing it has been committed.
  // Rethrows the caller's own exception or WriteGroupAborted.
  void submit(Slot &slot) {
    std::unique_lock<std::mutex> lk{queue_mu_};
    slot.done = false;
    slot.err = nullptr;
    queue_.push_back(&slot);

    if (!leader_active_) {
      leader_active_ = true;
      lk.unlock();
      leader_loop();
      lk.lock();
    }

    cv_.wait(lk, [&] { return slot.done; });

    if (slot.err) std::rethrow_exception(slot.err);
  }

private:
  void leader_loop() {
    while (true) {
      std::vector<Slot *> batch;
      {
        std::unique_lock<std::mutex> lk{queue_mu_};
        if (queue_.empty()) {
          leader_active_ = false;
          return;
        }
        batch.swap(queue_);
      }

      try {
        executor_(batch);
      } catch (...) {
        // Executor threw without setting per-slot errors.
        // Record the exception on every slot that has no error yet.
        auto ex = std::current_exception();
        for (auto *s : batch) {
          if (!s->err) s->err = ex;
        }
      }

      {
        std::unique_lock<std::mutex> lk{queue_mu_};
        for (auto *s : batch) s->done = true;
      }
      cv_.notify_all();
    }
  }

  BatchExecutor executor_;
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
// Declare BackgroundWorker as the LAST member of any owning class so that
// it destructs first, ensuring the background thread joins before any other
// member is destroyed.
// ---------------------------------------------------------------------------
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

} // namespace bytecask
