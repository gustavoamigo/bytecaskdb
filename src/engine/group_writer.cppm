module;
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.group_writer;

import bytecask.engine;

namespace bytecask {

// ---------------------------------------------------------------------------
// GroupWriter — group-commit wrapper for concurrent writers.
//
// Intent: N threads writing concurrently each pay one fdatasync today (the
// SWMR write_mu_ serialises them, so they queue up and each fires a separate
// fdatasync). GroupWriter coalesces those concurrent writes so that all writes
// that accumulate while one fdatasync is in flight share the next fdatasync.
//
// Protocol:
//   1. Each caller enqueues its operation and blocks on a future.
//   2. The first thread to find an empty queue becomes the leader for this
//      group. Subsequent arrivals become followers and wait.
//   3. The leader drains the queue — calling the engine write ops with
//      sync=false (each acquires write_mu_ normally) — then issues one
//      engine.sync() for the group, then fulfills all pending futures.
//   4. If more ops arrived while the leader was draining, the leader loops
//      and processes them as the next group before releasing the leader role.
//
// Thread safety: put/del/apply_batch are all thread-safe and may be called
// concurrently from any number of threads.
//
// Correctness under rotation: rotate_active_file() calls fdatasync before
// sealing, so any sync=false writes that landed in the pre-rotation file are
// already durable. engine.sync() only needs to cover the current active file.
// ---------------------------------------------------------------------------
// Default leader yield window: 100 µs, matching RocksDB's
// write_thread_max_yield_usec default. The leader spins/yields for up to this
// long before draining so that concurrent writers can join the same group and
// share the single fdatasync cost. Set to 0 to disable yielding entirely.
export inline constexpr std::uint32_t kDefaultGroupYieldUs = 100;

export class GroupWriter {
public:
  // max_yield_us: how long the elected leader waits for more writers to join
  // before draining. 0 = drain immediately (no coalescing benefit for solo
  // writers, but minimal latency overhead).
  explicit GroupWriter(Bytecask &engine,
                       std::uint32_t max_yield_us = kDefaultGroupYieldUs)
      : engine_{engine}, max_yield_us_{max_yield_us} {}

  GroupWriter(const GroupWriter &) = delete;
  GroupWriter &operator=(const GroupWriter &) = delete;

  // Submits a put and blocks until durability is confirmed by the leader's
  // fdatasync. Throws std::system_error on I/O failure.
  void put(BytesView key, BytesView value) {
    auto prom = std::make_shared<std::promise<void>>();
    auto fut = prom->get_future();
    const bool is_leader = enqueue({PutOp{Bytes{key.begin(), key.end()},
                                          Bytes{value.begin(), value.end()}},
                                    [prom](std::exception_ptr ex, bool) {
                                      if (ex)
                                        prom->set_exception(ex);
                                      else
                                        prom->set_value();
                                    }});
    if (is_leader)
      run_leader();
    fut.get();
  }

  // Submits a del and blocks until durability is confirmed (or absence
  // confirmed). Returns true if the key existed and was removed.
  // Throws std::system_error on I/O failure.
  [[nodiscard]] bool del(BytesView key) {
    auto prom = std::make_shared<std::promise<bool>>();
    auto fut = prom->get_future();
    const bool is_leader = enqueue({DelOp{Bytes{key.begin(), key.end()}},
                                    [prom](std::exception_ptr ex, bool found) {
                                      if (ex)
                                        prom->set_exception(ex);
                                      else
                                        prom->set_value(found);
                                    }});
    if (is_leader)
      run_leader();
    return fut.get();
  }

  // Submits a batch and blocks until durability is confirmed.
  // Per-batch BulkBegin/BulkEnd atomicity is preserved: each submission
  // keeps its own fence entries in the data file.
  // Throws std::system_error on I/O failure.
  void apply_batch(Batch batch) {
    auto prom = std::make_shared<std::promise<void>>();
    auto fut = prom->get_future();
    const bool is_leader = enqueue(
        {BatchOp{std::move(batch)}, [prom](std::exception_ptr ex, bool) {
           if (ex)
             prom->set_exception(ex);
           else
             prom->set_value();
         }});
    if (is_leader)
      run_leader();
    fut.get();
  }

private:
  struct PutOp {
    Bytes key;
    Bytes value;
  };

  struct DelOp {
    Bytes key;
  };

  struct BatchOp {
    Batch batch;
  };

  using Op = std::variant<PutOp, DelOp, BatchOp>;
  // fulfill(exception_ptr, del_result): null ptr = success.
  using Fulfillment = std::function<void(std::exception_ptr, bool)>;

  struct QueueEntry {
    Op op;
    Fulfillment fulfill;
  };

  // Adds entry to the queue. Returns true iff this caller is the new leader.
  auto enqueue(QueueEntry entry) -> bool {
    std::lock_guard lk{mu_};
    queue_.push_back(std::move(entry));
    if (leader_active_)
      return false;
    leader_active_ = true;
    return true;
  }

  // Drain the queue in a loop until empty, then release the leader role.
  // Called only by the elected leader thread.
  //
  // Yield phase (when max_yield_us_ > 0): snapshot the current queue depth
  // first, then spin/sleep for up to max_yield_us_ waiting for that count to
  // grow (i.e., new writers joined). Exits early on the first check where
  // cur_size > last_size. First 3 iterations use yield() (cheap CPU spin,
  // matching RocksDB's write_thread_slow_yield_usec ≈ 3 µs boundary); after
  // that, 1 µs sleeps. last_size is updated each iteration so the exit fires
  // on the next new arrival, not just the first one after the snapshot.
  //
  // Critical: last_size must be initialised from the actual queue size, not
  // from 0. The leader's own operation is already enqueued when run_leader()
  // is called (queue_.size() >= 1). Initialising to 0 would cause cur_size
  // (= 1) > last_size (= 0) to fire on the very first check, exiting
  // immediately and defeating all coalescing.
  void run_leader() {
    if (max_yield_us_ > 0) {
      std::size_t last_size;
      {
        std::lock_guard lk{mu_};
        last_size = queue_.size();
      }
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::microseconds{max_yield_us_};
      for (std::uint32_t n = 0; std::chrono::steady_clock::now() < deadline;
           ++n) {
        if (n < 3)
          std::this_thread::yield();
        else
          std::this_thread::sleep_for(std::chrono::microseconds{1});
        std::size_t cur_size;
        {
          std::lock_guard lk{mu_};
          cur_size = queue_.size();
        }
        if (cur_size > last_size)
          break;
        last_size = cur_size;
      }
    }

    while (true) {
      std::vector<QueueEntry> batch;
      {
        std::lock_guard lk{mu_};
        if (queue_.empty()) {
          leader_active_ = false;
          return;
        }
        std::swap(batch, queue_);
      }
      process_batch(batch);
    }
  }

  // Issue all writes with sync=false, call sync() once, then fulfill all
  // promises. Writes that throw store their exception and are fulfilled with
  // the write error. If sync() throws, all promises that expected success are
  // fulfilled with the sync exception instead.
  void process_batch(std::vector<QueueEntry> &batch) {
    WriteOptions no_sync;
    no_sync.sync = false;

    struct WriteResult {
      std::exception_ptr ex;
      bool del_found{false};
    };

    std::vector<WriteResult> results;
    results.reserve(batch.size());
    bool any_write = false;

    for (auto &entry : batch) {
      try {
        const auto found = std::visit(
            [&](auto &op) -> bool {
              using T = std::decay_t<decltype(op)>;
              if constexpr (std::is_same_v<T, PutOp>) {
                engine_.put(no_sync, op.key, op.value);
                any_write = true;
                return false;
              } else if constexpr (std::is_same_v<T, DelOp>) {
                const auto f = engine_.del(no_sync, op.key);
                if (f)
                  any_write = true;
                return f;
              } else {
                engine_.apply_batch(no_sync, std::move(op.batch));
                any_write = true;
                return false;
              }
            },
            entry.op);
        results.push_back({nullptr, found});
      } catch (...) {
        results.push_back({std::current_exception(), false});
      }
    }

    // Single fdatasync covers all writes above.
    std::exception_ptr sync_ex;
    if (any_write) {
      try {
        engine_.sync();
      } catch (...) {
        sync_ex = std::current_exception();
      }
    }

    // Fulfill every promise. Write errors take precedence; if the write
    // succeeded but sync failed, the sync exception is reported.
    for (std::size_t i = 0; i < batch.size(); ++i) {
      const auto &[write_ex, del_found] = results[i];
      batch[i].fulfill(write_ex ? write_ex : sync_ex, del_found);
    }
  }

  Bytecask &engine_;
  std::uint32_t max_yield_us_;
  std::mutex mu_;
  std::vector<QueueEntry> queue_;
  bool leader_active_{false};
};

} // namespace bytecask
