// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — reader-writer lock and epoch-based memory reclamation

module;
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

export module bytecask.concurrency;

namespace bytecask {

// ---------------------------------------------------------------------------
// SyncGroup — amortises fdatasync across concurrent writers.
//
// fdatasync is expensive (~2 ms). When N writers finish their writev at
// roughly the same time, one fdatasync can cover all of them. SyncGroup
// batches those writers so only one actually calls the sync callable
// while the rest wait and piggyback on its result.
//
// Precondition: callers must have completed their writev before entering
// sync(). The callable flushes data already in the page cache.
//
// Invariant: sync() does not return to a caller until a sync that started
// *after* that caller's writev has completed successfully.
// ---------------------------------------------------------------------------
export class SyncGroup {
public:
  // Amortises a sync operation across concurrent callers. do_sync() is called
  // by exactly one leader per batch; all others piggyback on its result.
  void sync(std::invocable auto do_sync) {
    std::unique_lock<std::mutex> lk{mu_};

    // Phase 1: take a ticket — our writev is done, data is in page cache.
    const auto my_ticket = next_ticket_++;

    // Phase 2: wait until covered by a completed sync, or become leader.
    cv_.wait(lk, [&] {
      return current_synced_ticket_ >= my_ticket || !syncing_;
    });

    // A sync that started after our writev already covered us.
    if (current_synced_ticket_ >= my_ticket) return;

    // Phase 3: we are the leader — snapshot the watermark, sync, notify.
    syncing_ = true;
    const auto batch_end = next_ticket_ - 1;
    lk.unlock();

    try {
      do_sync();
    } catch (...) {
      // If sync fails, reset the syncing flag and wake up waiters so they can
      // retry (or handle their own failure). Do not advance the synced ticket.
      lk.lock();
      syncing_ = false;
      lk.unlock();
      cv_.notify_all();
      throw;
    }

    lk.lock();
    current_synced_ticket_ = batch_end;
    syncing_ = false;
    lk.unlock();
    cv_.notify_all();
  }

private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::uint64_t next_ticket_{1};           // next ticket to hand out
  std::uint64_t current_synced_ticket_{0}; // highest ticket on disk
  bool syncing_{false};                    // is a sync in flight?
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
