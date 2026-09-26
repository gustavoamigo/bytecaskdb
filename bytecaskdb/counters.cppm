// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — operational counters for pull-based metrics (Prometheus-style).
//
// Counters are internal to DB and accessed via DB::stats(). All atomic
// counters use relaxed ordering — sufficient for monotonic counters where
// cross-counter consistency is not required.

module;
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

export module bytecask.counters;

namespace bytecask {

// ---------------------------------------------------------------------------
// StripedCounter — a monotonic counter every reader thread bumps.
//
// A single atomic bumped on every get is one cache line that every core
// fights over, and at 32 readers that line, not the read, sets the
// throughput: the line can absorb roughly 50 M read-modify-writes a second
// however cheap the reads are. Striping the count across cache lines keyed by
// thread keeps each increment on a line only a couple of threads touch; the
// value is the sum of the stripes, read at stats() time.
// ---------------------------------------------------------------------------
export class StripedCounter {
public:
  void add(std::int64_t n) noexcept {
    stripes_[stripe_index()].value.fetch_add(n, std::memory_order_relaxed);
  }

  // A sum of relaxed loads: exact once writers are quiescent, approximate
  // while they are not, which is what a monotonic metric needs.
  [[nodiscard]] auto load() const noexcept -> std::int64_t {
    std::int64_t total = 0;
    for (const auto &s : stripes_) {
      total += s.value.load(std::memory_order_relaxed);
    }
    return total;
  }

private:
  static constexpr std::size_t kStripes = 16;
  static constexpr std::size_t kCacheLineBytes = 64;
  static_assert((kStripes & (kStripes - 1)) == 0, "kStripes must be a power of two");

  struct alignas(kCacheLineBytes) Stripe {
    std::atomic<std::int64_t> value{0};
  };

  // Threads take stripes round-robin on first use; more than kStripes threads
  // share stripes, which is contention between a few threads rather than all.
  static auto stripe_index() noexcept -> std::size_t {
    static std::atomic<std::size_t> next{0};
    thread_local const std::size_t id =
        next.fetch_add(1, std::memory_order_relaxed) & (kStripes - 1);
    return id;
  }

  std::array<Stripe, kStripes> stripes_{};
};

// ---------------------------------------------------------------------------
// Counters — per-DB-instance operational counters.
//
// Write-path counters are incremented under write_mu_ (zero contention).
// Read-path counters are bumped by every reader on every get and are striped
// (StripedCounter) so that concurrent readers do not serialise on one line.
// Recovery counters are plain int64_t — set once during construction, then
// immutable for the lifetime of the DB.
// ---------------------------------------------------------------------------
export struct Counters {
  // -- Write path --
  std::atomic<std::int64_t> bytes_written{0};
  std::atomic<std::int64_t> group_writer_batches{0};
  std::atomic<std::int64_t> group_writer_coalesced{0};
  std::atomic<std::int64_t> file_rotations{0};
  std::atomic<std::int64_t> fsyncs{0};
  // Writers that slept in commit_wait behind an in-flight flush. Zero for
  // a lone writer, which always flushes on its own thread.
  std::atomic<std::int64_t> commit_wait_blocked{0};

  // -- Read path --
  StripedCounter disk_reads;
  StripedCounter disk_read_bytes;

  // -- Buffer pool: see PoolCounters in bytecask.buffer_pool. They live with
  // the pool because the pool can outlive the DB — an iterator holds the
  // data file, the file holds the pool — and stats() reads them through it.

  // -- Vacuum --
  std::atomic<std::int64_t> vacuum_bytes_reclaimed{0};
  std::atomic<std::int64_t> vacuum_files_unlinked{0};
  std::atomic<std::int64_t> vacuum_tombstones_dropped{0};

  // -- Recovery (set once at open, then read-only) --
  std::int64_t recovery_files{0};
  std::int64_t recovery_keys{0};
  std::int64_t recovery_duration_us{0};

  // -- Files --
  std::atomic<std::int64_t> files_opened{0};

  // -- Hint backlog: rotations that waited for the background worker, and
  // for how long in total (Options::max_hint_backlog). --
  std::atomic<std::int64_t> hint_backpressure_stalls{0};
  std::atomic<std::int64_t> hint_backpressure_stall_us{0};

  // -- Errors --
  std::atomic<std::int64_t> crc_failures{0};
  std::atomic<std::int64_t> io_errors{0};
  std::atomic<std::int64_t> degraded_transitions{0};
};

} // namespace bytecask
