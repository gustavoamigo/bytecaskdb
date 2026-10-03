// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — commit_probe: the serial section under many writers.
//
// Closed-loop transactions shaped like a TPROC-C commit through the MariaDB
// plugin: a snapshot, point reads, then updates and inserts in one WritePlan
// committed with sync=false, while a background thread commits an empty
// sync=true plan every interval (the plugin's bytecaskdb_sync = AT_INTERVAL).
// Between transactions each thread spins (server CPU) and sleeps (client
// round trips). Reports throughput, the shape of the group-commit batches,
// and how busy the serial section is (bytecask.group_writer_busy_us): once
// it nears 100%, commits/s is 1 / (serial µs per commit) whatever the thread
// count, and that cost is the number a change to the write path moves.
//
//   commit_probe <dir> <threads> <seconds> [key=value ...]
//
// Keys (defaults): keys=2000000 reads=10 updates=10 inserts=12 vsize=120
// spin_us=300 sleep_us=300 file_mb=64 sync_ms=1000 warm_s=2 pool_mb=4096.
// With spin_us=0 sleep_us=0 reads=0 inserts=0 updates=1 it measures a bare
// unsynced write, where a lone writer's commit latency dominates.
//
// Put <dir> on tmpfs to take the disk out (rotation fdatasyncs then cost
// ~nothing), and preload jemalloc to match the MariaDB benchmarks. Runs
// repeat within ~0.5% there; compare variants by alternating runs.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

import bytecask;

namespace {

using Clock = std::chrono::steady_clock;

auto as_view(const std::string &s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

auto key_of(std::uint64_t i) -> std::string { return std::format("s:{:012}", i); }

void spin_for(std::chrono::microseconds d) {
  const auto until = Clock::now() + d;
  while (Clock::now() < until) {
  }
}

auto arg(const std::map<std::string, std::string> &m, const std::string &k,
         long def) -> long {
  const auto it = m.find(k);
  return it == m.end() ? def : std::stol(it->second);
}

} // namespace

auto main(int argc, char **argv) -> int {
  if (argc < 4) {
    std::fprintf(stderr, "usage: commit_probe <dir> <threads> <seconds> [key=value ...]\n");
    return 2;
  }
  const std::filesystem::path dir = argv[1];
  const int threads = std::atoi(argv[2]);
  const int secs = std::atoi(argv[3]);
  std::map<std::string, std::string> kv;
  for (int i = 4; i < argc; ++i) {
    const std::string a = argv[i];
    const auto eq = a.find('=');
    if (eq != std::string::npos) kv[a.substr(0, eq)] = a.substr(eq + 1);
  }
  const auto keys = static_cast<std::uint64_t>(arg(kv, "keys", 2'000'000));
  const auto reads = arg(kv, "reads", 10);
  const auto updates = arg(kv, "updates", 10);
  const auto inserts = arg(kv, "inserts", 12);
  const auto vsize = static_cast<std::size_t>(arg(kv, "vsize", 120));
  const auto spin_us = arg(kv, "spin_us", 300);
  const auto sleep_us = arg(kv, "sleep_us", 300);
  const auto file_mb = arg(kv, "file_mb", 64);
  const auto sync_ms = arg(kv, "sync_ms", 1000);
  const auto warm_s = arg(kv, "warm_s", 2);
  const auto pool_mb = arg(kv, "pool_mb", 4096);

  bytecask::Options opts;
  opts.max_file_bytes = static_cast<std::uint64_t>(file_mb) << 20;
  opts.io_backend = bytecask::IoBackend::BufferPool;
  opts.buffer_pool.capacity_bytes = static_cast<std::size_t>(pool_mb) << 20;
  opts.buffer_pool.direct_io = false;

  std::filesystem::remove_all(dir);
  auto db = bytecask::DB::open(dir, opts);
  const std::string val(vsize, 'v');
  for (std::uint64_t i = 0; i < keys;) {
    bytecask::WritePlan p;
    for (int j = 0; j < 1000 && i < keys; ++j, ++i) p.put(as_view(key_of(i)), as_view(val));
    (void)db.apply_batch({.sync = false}, std::move(p));
  }

  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> commits{0};
  std::atomic<std::int64_t> conflicts{0};
  std::thread syncer{[&] {
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sync_ms));
      (void)db.apply_batch({.sync = true}, bytecask::WritePlan{});
    }
  }};
  std::vector<std::thread> writers;
  for (int t = 0; t < threads; ++t) {
    writers.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t) * 7919 + 1);
      std::uniform_int_distribution<std::uint64_t> pick(0, keys - 1);
      std::uint64_t next_insert = 0;
      bytecask::Bytes out;
      const bytecask::ReadOptions ro{.verify_checksums = false};
      while (!stop.load(std::memory_order_relaxed)) {
        if (spin_us > 0) spin_for(std::chrono::microseconds(spin_us));
        if (sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        auto snap = db.snapshot();
        for (long r = 0; r < reads; ++r) (void)snap.get(ro, as_view(key_of(pick(rng))), out);
        bytecask::WritePlan plan{std::move(snap)};
        for (long u = 0; u < updates; ++u) plan.put(as_view(key_of(pick(rng))), as_view(val));
        for (long n = 0; n < inserts; ++n)
          plan.put(as_view(std::format("o:{:03}:{:012}", t, next_insert++)), as_view(val));
        if (db.apply_batch({.sync = false}, std::move(plan)))
          commits.fetch_add(1, std::memory_order_relaxed);
        else
          conflicts.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(warm_s));
  const auto s0 = db.stats();
  const auto c0 = commits.load();
  const auto k0 = conflicts.load();
  const auto t0 = Clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(secs));
  const auto s1 = db.stats();
  const auto c1 = commits.load();
  const auto k1 = conflicts.load();
  const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
  stop = true;
  for (auto &w : writers) w.join();
  syncer.join();

  const auto delta = [&](const char *k) {
    return static_cast<double>(s1.at(k) - s0.at(k));
  };
  const double committed = static_cast<double>(c1 - c0);
  const double batches = delta("bytecask.group_writer_batches");
  const double writes = delta("bytecask.group_writer_coalesced");
  const double busy_us = delta("bytecask.group_writer_busy_us");
  std::printf("threads=%d commits/s=%.0f conflicts/s=%.0f batch=%.1f batches/s=%.0f "
              "serial_busy=%.1f%% serial_us_per_commit=%.1f\n",
              threads, committed / elapsed,
              static_cast<double>(k1 - k0) / elapsed,
              batches > 0 ? writes / batches : 0.0, batches / elapsed,
              100.0 * busy_us / (elapsed * 1e6),
              writes > 0 ? busy_us / writes : 0.0);
  // A key directory with a write buffer: how often, and how long, commits
  // waited for its merger.
  if (s1.contains("bytecask.keydir_buffer_stalls"))
    std::printf("  keydir buffer: stalls/s=%.0f inline_merges/s=%.0f stall_us_per_commit=%.2f\n",
                delta("bytecask.keydir_buffer_stalls") / elapsed,
                delta("bytecask.keydir_buffer_inline_merges") / elapsed,
                committed > 0 ? delta("bytecask.keydir_buffer_stall_us") / committed : 0.0);
  if (s1.contains("bytecask.keydir_buffer_merges")) {
    const auto merges = delta("bytecask.keydir_buffer_merges");
    const auto slots = delta("bytecask.keydir_buffer_merge_slots");
    const auto merge_us = delta("bytecask.keydir_buffer_merge_us");
    std::printf("  merger: busy=%.1f%% us/merge=%.0f ns/slot=%.0f by_location=%.1f%%\n",
                100.0 * merge_us / (elapsed * 1e6), merges > 0 ? merge_us / merges : 0.0,
                slots > 0 ? 1000.0 * merge_us / slots : 0.0,
                slots > 0 ? 100.0 * delta("bytecask.keydir_buffer_merge_by_location") / slots : 0.0);
  }
  return 0;
}
