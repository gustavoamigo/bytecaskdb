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

  // -- Read path --
  std::atomic<std::int64_t> disk_reads{0};
  std::atomic<std::int64_t> disk_read_bytes{0};

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
