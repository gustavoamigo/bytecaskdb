// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitives: background worker, solo writer,
// group write batching.

module;
#include <algorithm>
#include <chrono>
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
      std::move_only_function<void(std::vector<Slot *> &)> executor)
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
  std::move_only_function<void(std::vector<Slot *> &)> executor_;
};

// ---------------------------------------------------------------------------
// WriteGroupOpts — tuning parameters for WriteGroup's adaptive wait window.
// ---------------------------------------------------------------------------
export struct WriteGroupOpts {
  std::chrono::microseconds max_wait{200};
  std::size_t target_batch{8};
};

// ---------------------------------------------------------------------------
// WriteGroup — leader-applies-all write batching (Template Method pattern).
//
// The algorithm skeleton lives here: enqueue → elect leader → drain queue →
// call executor → mark done → wake → loop until empty. The domain-specific
// batch execution logic is injected via a BatchExecutor callback at
// construction time.
//
// On the first drain (leader just elected, queue typically has 1 slot), the
// leader waits up to an adaptive window for more writers to arrive. The wait
// exits early when the queue reaches target_batch. An EMA of batch sizes
// tracks natural concurrency: when batches fill naturally, the wait shrinks
// to zero.
//
// submit() is non-template — it takes a Slot&.
// ---------------------------------------------------------------------------
export class WriteGroup {
public:
  explicit WriteGroup(
      std::move_only_function<void(std::vector<Slot *> &)> executor,
      WriteGroupOpts opts = {})
      : executor_{std::move(executor)}, opts_{opts} {}

  WriteGroup(const WriteGroup &) = delete;
  WriteGroup &operator=(const WriteGroup &) = delete;

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
    } else {
      leader_cv_.notify_one();
    }

    cv_.wait(lk, [&] { return slot.done; });

    if (slot.err) std::rethrow_exception(slot.err);
  }

private:
  auto effective_wait() const -> std::chrono::microseconds {
    auto scale = std::clamp(
        1.0 - ema_batch_ / static_cast<double>(opts_.target_batch), 0.0, 1.0);
    return std::chrono::microseconds{static_cast<long long>(
        static_cast<double>(opts_.max_wait.count()) * scale)};
  }

  void leader_loop() {
    bool first_drain = true;
    while (true) {
      std::vector<Slot *> batch;
      {
        std::unique_lock<std::mutex> lk{queue_mu_};
        if (queue_.empty()) {
          leader_active_ = false;
          return;
        }

        if (first_drain && queue_.size() < opts_.target_batch) {
          auto wait = effective_wait();
          if (wait > std::chrono::microseconds::zero()) {
            leader_cv_.wait_for(lk, wait, [&] {
              return queue_.size() >= opts_.target_batch;
            });
          }
          first_drain = false;
        }

        batch.swap(queue_);
      }

      try {
        executor_(batch);
      } catch (...) {
        auto ex = std::current_exception();
        for (auto *s : batch) {
          if (!s->err) s->err = ex;
        }
      }

      ema_batch_ = 0.25 * static_cast<double>(batch.size())
                  + 0.75 * ema_batch_;

      {
        std::unique_lock<std::mutex> lk{queue_mu_};
        for (auto *s : batch) s->done = true;
      }
      cv_.notify_all();
    }
  }

  std::move_only_function<void(std::vector<Slot *> &)> executor_;
  WriteGroupOpts opts_;
  std::mutex queue_mu_;
  std::vector<Slot *> queue_;
  bool leader_active_{false};
  double ema_batch_{1.0};
  std::condition_variable cv_;
  std::condition_variable leader_cv_;
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
