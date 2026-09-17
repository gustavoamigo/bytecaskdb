// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — operational counters for pull-based metrics (Prometheus-style).
//
// Counters are internal to DB and accessed via DB::stats(). All atomic
// counters use relaxed ordering — sufficient for monotonic counters where
// cross-counter consistency is not required.

module;
#include <atomic>
#include <cstdint>

export module bytecask.counters;

namespace bytecask {

// ---------------------------------------------------------------------------
// Counters — per-DB-instance operational counters.
//
// Write-path counters are incremented under write_mu_ (zero contention).
// Read-path counters use relaxed atomic fetch_add (~8 ns on x86, negligible
// next to the pread syscall).
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
  std::atomic<std::int64_t> disk_reads{0};
  std::atomic<std::int64_t> disk_read_bytes{0};

  // -- Buffer pool (all zero when IoBackend != BufferPool) --
  // hits/misses are the primary metric: unlike disk_reads, a miss here is
  // known to have left the pool, which the page cache cannot tell you.
  std::atomic<std::int64_t> pool_hits{0};
  std::atomic<std::int64_t> pool_misses{0};
  // Frames admitted by a read miss. The writer's inserts on append are not
  // fills: they cost no I/O.
  std::atomic<std::int64_t> pool_fills{0};
  std::atomic<std::int64_t> pool_evictions{0};
  std::int64_t pool_frames_total{0};
  // Gauge: frames currently holding a file's bytes. Resident / total is the
  // fill level an operator sizes against.
  std::atomic<std::int64_t> pool_frames_resident{0};
  // Files whose filesystem refused O_DIRECT and fill through the page cache
  // instead. Catches a CI mount that would otherwise measure the wrong thing.
  std::atomic<std::int64_t> pool_direct_io_fallbacks{0};

  // -- Vacuum --
  std::atomic<std::int64_t> vacuum_bytes_reclaimed{0};
  std::atomic<std::int64_t> vacuum_files_unlinked{0};

  // -- Recovery (set once at open, then read-only) --
  std::int64_t recovery_files{0};
  std::int64_t recovery_keys{0};
  std::int64_t recovery_duration_us{0};

  // -- Files --
  std::atomic<std::int64_t> files_opened{0};

  // -- Errors --
  std::atomic<std::int64_t> crc_failures{0};
  std::atomic<std::int64_t> io_errors{0};
  std::atomic<std::int64_t> degraded_transitions{0};
};

} // namespace bytecask
