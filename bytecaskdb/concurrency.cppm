// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — concurrency primitives: background worker, solo writer,
// group write batching.

module;
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#if defined(__linux__) && !defined(BYTECASK_SINGLE_THREADED)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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
// Slot — base type for writer submit interfaces, and the object its owner
// waits on.
//
// The engine extends it (EngineSlot) with domain data; executors static_cast
// Slot* to the derived type. `word` is the slot's progress: the low bits are
// the phase, the rest are flags. The owner sleeps on the word itself, one
// futex per slot, so a wake reaches exactly the thread that needs it and no
// mutex is held on the wait or wake path. A sync writer sleeps once, until
// its bytes are durable; nothing wakes it earlier. See slot_wait,
// slot_advance and slot_hand.
// ---------------------------------------------------------------------------
export constexpr std::uint32_t kSlotQueued = 0;   // enqueued, not executed
export constexpr std::uint32_t kSlotApplied = 1;  // stage 1 ran (err says how)
export constexpr std::uint32_t kSlotVisible = 2;  // its state is published
export constexpr std::uint32_t kSlotDurable = 3;  // its fdatasync returned
export constexpr std::uint32_t kSlotPhaseMask = 3;
export constexpr std::uint32_t kSlotLead = 1u << 2;       // owner leads next
export constexpr std::uint32_t kSlotFlushNext = 1u << 3;  // owner holds the flush role
export constexpr std::uint32_t kSlotWaiting = 1u << 31;   // owner is (about to be) asleep

export struct Slot {
  bool sync{false};
  std::atomic<std::uint32_t> word{kSlotQueued};
  // Phase at which submit() returns the slot to its owner. Written by the
  // executor before the leader advances the word: kSlotApplied when there
  // is nothing to wait for (conflict, error, empty plan), kSlotVisible for
  // a nosync write, kSlotDurable for a sync write. Relaxed: the owner reads
  // it after an acquire load of the word, and a queued slot sleeps for any
  // value, so the order it becomes visible in does not matter.
  std::atomic<std::uint32_t> release_at{kSlotApplied};
  std::exception_ptr err;
};

static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

namespace detail {
// slot_futex_wait returns true if the thread slept (was woken by a wake),
// false if the word had already changed and it never blocked.
#if defined(BYTECASK_SINGLE_THREADED)
[[noreturn]] inline auto slot_futex_wait(std::atomic<std::uint32_t> &,
                                         std::uint32_t) -> bool {
  // One thread: nothing could ever advance the word this thread waits on.
  std::cerr << "bytecask: slot wait in a single-threaded build\n";
  std::abort();
}
inline void slot_futex_wake(std::atomic<std::uint32_t> &) {}
#elif defined(__linux__)
// FUTEX_WAIT returns at once (EAGAIN) when the word no longer holds
// `expected`; that check, inside the kernel, is what makes load-then-wait
// lose-free.
inline auto slot_futex_wait(std::atomic<std::uint32_t> &w,
                            std::uint32_t expected) -> bool {
  return ::syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&w),
                   FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0) == 0;
}
inline void slot_futex_wake(std::atomic<std::uint32_t> &w) {
  (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&w),
                  FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}
#else
inline auto slot_futex_wait(std::atomic<std::uint32_t> &w,
                            std::uint32_t expected) -> bool {
  w.wait(expected, std::memory_order_acquire);
  return true;
}
inline void slot_futex_wake(std::atomic<std::uint32_t> &w) { w.notify_one(); }
#endif
}  // namespace detail

// The slot is finished for its owner: its phase reached release_at. Only
// then may the owner read err and result — before that, the flush role may
// still be writing them.
export [[nodiscard]] inline auto slot_done(const Slot &s,
                                           std::uint32_t w) noexcept -> bool {
  return (w & kSlotPhaseMask) >= s.release_at.load(std::memory_order_relaxed);
}

// The owner may return from its wait: the slot is done, or a role was
// handed to it.
export [[nodiscard]] inline auto slot_released(const Slot &s,
                                               std::uint32_t w) noexcept
    -> bool {
  return slot_done(s, w) || (w & (kSlotLead | kSlotFlushNext)) != 0;
}

// Blocks until the slot is released to its owner and returns the word seen.
// Sets kSlotWaiting before sleeping and sleeps only while the word is the
// exact value it saw, so a wake between the load and the sleep is never
// lost. Counts in *sleeps, if given, the first time it actually sleeps.
export auto slot_wait(Slot &s, std::atomic<std::int64_t> *sleeps = nullptr)
    -> std::uint32_t {
  bool counted = false;
  for (;;) {
    auto w = s.word.load(std::memory_order_acquire);
    if (slot_released(s, w)) return w;
    if ((w & kSlotWaiting) == 0) {
      if (!s.word.compare_exchange_weak(w, w | kSlotWaiting,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        continue;
      }
      w |= kSlotWaiting;
    }
    // Counted before the sleep so a waiter behind a held flush is visible
    // as blocked; withdrawn if the word had changed and it never slept.
    const bool first = sleeps != nullptr && !counted;
    if (first) {
      sleeps->fetch_add(1, std::memory_order_relaxed);
      counted = true;
    }
    if (!detail::slot_futex_wait(s.word, w) && first) {
      sleeps->fetch_sub(1, std::memory_order_relaxed);
      counted = false;
    }
  }
}

// Raises the phase to `phase` (never lowers it), keeping the flags. Wakes
// the owner only if that releases it and it is asleep: one syscall, one
// thread, none for an owner that is not waiting. Everything the owner reads
// after waking (result, err) must be written before this call.
export void slot_advance(Slot &s, std::uint32_t phase) {
  auto w = s.word.load(std::memory_order_relaxed);
  std::uint32_t n = 0;
  bool wake = false;
  do {
    const auto p = std::max(w & kSlotPhaseMask, phase);
    n = (w & ~kSlotPhaseMask) | p;
    wake = (w & kSlotWaiting) != 0
        && p >= s.release_at.load(std::memory_order_relaxed);
    if (wake) n &= ~kSlotWaiting;
  } while (!s.word.compare_exchange_weak(w, n, std::memory_order_acq_rel,
                                         std::memory_order_relaxed));
  if (wake) detail::slot_futex_wake(s.word);
}

// Hands a role (kSlotLead or kSlotFlushNext) to the owner, waking it if it
// is asleep. The owner takes the role with slot_take before acting on it.
export void slot_hand(Slot &s, std::uint32_t flag) {
  auto w = s.word.load(std::memory_order_relaxed);
  std::uint32_t n = 0;
  bool wake = false;
  do {
    n = w | flag;
    wake = (w & kSlotWaiting) != 0;
    if (wake) n &= ~kSlotWaiting;
  } while (!s.word.compare_exchange_weak(w, n, std::memory_order_acq_rel,
                                         std::memory_order_relaxed));
  if (wake) detail::slot_futex_wake(s.word);
}

export void slot_take(Slot &s, std::uint32_t flag) noexcept {
  s.word.fetch_and(~flag, std::memory_order_acq_rel);
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
  // after_batch, if given, runs on the submitting thread after the executor
  // returned normally and the slot was advanced to kSlotApplied, and only
  // if the slot still waits (release_at above kSlotApplied): the point at
  // which the writer is done with the slot and the engine may hand it on
  // (see WriteGroup).
  explicit SoloWriter(
      std::function<void(std::vector<Slot *> &)> executor,
      std::function<void(std::vector<Slot *> &)> after_batch = {})
      : executor_{std::move(executor)}, after_batch_{std::move(after_batch)} {}

  SoloWriter(const SoloWriter &) = delete;
  SoloWriter &operator=(const SoloWriter &) = delete;

  void submit(Slot &slot) {
    slot.word.store(kSlotQueued, std::memory_order_relaxed);
    slot.release_at.store(kSlotApplied, std::memory_order_relaxed);
    slot.err = nullptr;
    std::vector<Slot *> batch{&slot};
    bool ok = true;
    try {
      executor_(batch);
    } catch (...) {
      slot.err = std::current_exception();
      ok = false;
    }
    const bool waits =
        slot.release_at.load(std::memory_order_relaxed) > kSlotApplied;
    slot_advance(slot, kSlotApplied);
    if (ok && waits && after_batch_) after_batch_(batch);
    // After the hook the flush role may own the slot and write err; it is
    // read only once the slot is done (the caller's commit_wait otherwise).
    if (slot_done(slot, slot.word.load(std::memory_order_acquire))
        && slot.err) {
      std::rethrow_exception(slot.err);
    }
  }

private:
  std::function<void(std::vector<Slot *> &)> executor_;
  std::function<void(std::vector<Slot *> &)> after_batch_;
};

// ---------------------------------------------------------------------------
// WriteGroup — leader-applies-all write batching (Template Method pattern).
//
// The algorithm skeleton lives here: enqueue → elect leader → take the queue
// as one batch → call executor → advance each slot to kSlotApplied (waking
// only the owners that have nothing left to wait for) → hand leadership to
// the head of the queue with one wake. The domain-specific batch execution
// logic is injected via a BatchExecutor callback at construction time.
//
// Slot ownership. From submit() until lead() has advanced the batch and
// handed leadership on, a slot belongs to the group and the executor, both
// on the leader thread; its owner sleeps and reads nothing. Advancing to
// kSlotApplied releases the owners whose release_at is kSlotApplied, and a
// released owner may return and destroy its slot at once. The slots the
// executor marked as still waiting (release_at above kSlotApplied) stay
// asleep, and only those are passed, still on the leader thread, to
// after_batch: the point at which the group is done with them and the
// engine may hand them to its flush role. The group never touches a slot
// after the hook, and after_batch runs only when the executor returned
// normally.
//
// submit() is non-template — it takes a Slot&.
// ---------------------------------------------------------------------------
export class WriteGroup {
public:
  explicit WriteGroup(
      std::function<void(std::vector<Slot *> &)> executor,
      std::function<void(std::vector<Slot *> &)> after_batch = {})
      : executor_{std::move(executor)}, after_batch_{std::move(after_batch)} {}

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

  // Enqueues slot and blocks until the slot is released to its owner (see
  // Slot::release_at) or handed leadership. A submitter that finds no
  // leader becomes one and runs a single batch — everything queued at that
  // moment, its own slot included — then returns to its caller. If slots
  // were queued while the batch ran, leadership passes to the head of the
  // queue with one targeted wake.
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
    slot.word.store(kSlotQueued, std::memory_order_relaxed);
    slot.release_at.store(kSlotApplied, std::memory_order_relaxed);
    slot.err = nullptr;
    queue_.push_back(&slot);
    inflight_.fetch_add(1, std::memory_order_relaxed);

    bool lead_now = false;
    if (!leader_active_) {
      leader_active_ = true;
      lead_now = true;
    } else {
      lk.unlock();
      const auto w = slot_wait(slot, &waits_);
      if ((w & kSlotLead) != 0) {
        slot_take(slot, kSlotLead);
        lk.lock();
        lead_now = true;
      }
    }
    if (lead_now) lead(lk);

    // After the hook the flush role may own the slot and write err; it is
    // read only once the slot is done (commit_wait reads it otherwise).
    if (slot_done(slot, slot.word.load(std::memory_order_acquire))
        && slot.err) {
      std::rethrow_exception(slot.err);
    }
  }

  // Submitters that slept in submit: members waiting for their batch or
  // their flush, and the writer handed leadership after each batch (one
  // per batch at most, the one sleep leader hand-off costs).
  [[nodiscard]] auto waits() const noexcept -> std::int64_t {
    return waits_.load(std::memory_order_relaxed);
  }

  // True while any submitted slot has not been executed yet: queued, or in
  // the batch the leader is running. Lock-free so the commit pipeline's
  // flusher can let an in-progress batch land before it captures the head.
  [[nodiscard]] auto busy() const noexcept -> bool {
    return inflight_.load(std::memory_order_acquire) > 0;
  }

private:
  // Runs one batch. Called with lk held and leader_active_ set; drops lk
  // around the executor call and returns with lk released. Each slot is
  // advanced to kSlotApplied, which wakes only the owners with nothing left
  // to wait for; the next leader gets its own wake; then after_batch hands
  // the batch on. Nothing here reads a slot after that.
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

    bool ok = true;
    try {
      executor_(batch);
    } catch (...) {
      auto ex = std::current_exception();
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      ok = false;
    }
    // Decided before the advance: a released slot may be gone right after.
    std::vector<Slot *> waiting;
    if (ok && after_batch_) {
      for (auto *s : batch) {
        if (s->release_at.load(std::memory_order_relaxed) > kSlotApplied) {
          waiting.push_back(s);
        }
      }
    }

    lk.lock();
    inflight_.fetch_sub(static_cast<int>(batch.size()),
                        std::memory_order_release);
    Slot *next = nullptr;
    if (queue_.empty()) {
      leader_active_ = false;
    } else {
      next = queue_.front();
    }
    lk.unlock();
    // `next` stays queued until it leads: only the leader takes the queue,
    // and the next leader is `next` itself.
    for (auto *s : batch) slot_advance(*s, kSlotApplied);
    if (next != nullptr) slot_hand(*next, kSlotLead);
    if (!waiting.empty()) after_batch_(waiting);
  }

  std::function<void(std::vector<Slot *> &)> executor_;
  std::function<void(std::vector<Slot *> &)> after_batch_;
  std::mutex queue_mu_;
  std::vector<Slot *> queue_;
  bool leader_active_{false};
  std::atomic<int> inflight_{0};
  std::atomic<std::int64_t> waits_{0};
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
