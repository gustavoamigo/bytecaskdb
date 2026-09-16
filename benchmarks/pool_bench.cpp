// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — buffer pool sweep over pool_bytes / dataset_bytes.
//
// This is the benchmark the buffer pool design asks for, and it is deliberately
// not an addition to engine_bench. Every benchmark there fits in RAM, which is
// the regime where the pool should be switched off; measuring it there measures
// overhead. The axis that means anything is how much of the dataset the pool
// can hold, swept from far too little to more than enough, reported as p50 and
// p99 rather than mean throughput — tenet 3 makes the tail the deciding number.
//
// WHAT THIS CAN AND CANNOT ANSWER
//
// Phase 1 fills through the page cache, so both the pool and the pread baseline
// read the same bytes from the same place. That makes this a fair measurement
// of the pool's own overhead and hit ratio — Phase 1's question — and NOT a
// measurement of what bypassing the page cache buys, which is Phase 2's and
// needs O_DIRECT plus a dataset larger than RAM.
//
// The pool runs twice per ratio: direct_io=true (Phase 2: O_DIRECT fills, the
// file's page-cache residency dropped at open) and direct_io=false (Phase 1:
// fills through the page cache). The difference is the price of bypassing the
// page cache — which is real device I/O on a miss, against a pread/mmap
// baseline that is still warm. That asymmetry is the point, not a flaw: it is
// what the pool costs when it is doing its actual job.
//
// Nothing here drops the page cache for the baselines (it needs root), so
// their "misses" are page-cache hits. Their numbers are a floor.
//
// Output is CSV on stdout; scripts/run_pool_bench.py records it.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import bytecask;

namespace {

auto to_bytes(std::string_view sv) -> bytecask::BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

struct Config {
  std::size_t keys = 200'000;
  std::size_t value_bytes = 512;
  std::size_t ops = 200'000;
  double zipf_s = 0.99;
  std::uint64_t max_file_bytes = 4ULL * 1024 * 1024;
  std::vector<double> ratios = {0.1, 0.25, 0.5, 1.0, 2.0};
  // Thread counts for the multi-reader arm (§8's GetMT criterion). Empty
  // skips it. Runs at ratio 1.0 — fully resident — because the question is
  // whether the lock-free read path scales, not what a miss costs.
  std::vector<unsigned> mt_threads;
  // Read-your-own-writes arm (§11 Phase 3's question): N put/get pairs on
  // fresh keys, so every get is of the newest bytes in the active file.
  // 0 skips it. Only the gets are timed.
  std::size_t ryow_pairs = 0;
  std::filesystem::path dir = ".tmp/pool_bench";
};

auto key_for(std::size_t i) -> std::string { return std::format("key{:09d}", i); }

// Zipf by precomputed CDF: exact, and the setup cost is irrelevant next to the
// measured loop. A rejection sampler would be cheaper to build and harder to
// trust.
class Zipf {
public:
  Zipf(std::size_t n, double s) : cdf_(n) {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i + 1), s);
      cdf_[i] = sum;
    }
    for (auto &c : cdf_) c /= sum;
  }

  auto operator()(std::mt19937_64 &rng) const -> std::size_t {
    const auto u = std::uniform_real_distribution<double>{0.0, 1.0}(rng);
    return static_cast<std::size_t>(
        std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
  }

private:
  std::vector<double> cdf_;
};

auto dataset_bytes(const std::filesystem::path &dir) -> std::uint64_t {
  std::uint64_t total = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() == ".data") total += e.file_size();
  }
  return total;
}

void build_dataset(const Config &cfg) {
  std::filesystem::remove_all(cfg.dir);
  std::filesystem::create_directories(cfg.dir);
  auto db = bytecask::DB::open(cfg.dir, {.max_file_bytes = cfg.max_file_bytes});
  const std::string value(cfg.value_bytes, 'v');
  for (std::size_t i = 0; i < cfg.keys; ++i) {
    db.put({.sync = false}, to_bytes(key_for(i)), to_bytes(value));
  }
}

struct Result {
  std::string backend;
  bool direct_io = false;      // meaningful only for buffer_pool
  unsigned threads = 1;
  bool ryow = false;           // this row is the read-your-own-writes arm
  double ratio = 0.0;          // 0 for the non-pool baselines
  std::uint64_t pool_bytes = 0;
  double ops_per_sec = 0.0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
  double hit_ratio = -1.0;     // -1 where the back-end has no pool
  double u = -1.0;             // fraction of an evicted frame that was read
  std::int64_t evictions = 0;
  std::int64_t retries = 0;
};

auto percentile(std::vector<std::uint64_t> &v, double p) -> std::uint64_t {
  if (v.empty()) return 0;
  const auto idx = static_cast<std::size_t>(
      static_cast<double>(v.size() - 1) * p);
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx),
                   v.end());
  return v[idx];
}

auto measure(const Config &cfg, bytecask::IoBackend backend,
             std::uint64_t pool_bytes, double ratio, bool direct_io) -> Result {
  bytecask::Options opts{.max_file_bytes = cfg.max_file_bytes,
                         .io_backend = backend};
  if (backend == bytecask::IoBackend::BufferPool) {
    opts.buffer_pool.capacity_bytes = static_cast<std::size_t>(pool_bytes);
    opts.buffer_pool.direct_io = direct_io;
  }
  // A fresh open gives every configuration a cold pool.
  auto db = bytecask::DB::open(cfg.dir, opts);

  const Zipf zipf{cfg.keys, cfg.zipf_s};
  std::mt19937_64 rng{42};

  // Warm to the steady state: a cold-cache transient is a different
  // measurement from the one this sweep is about.
  bytecask::Bytes out;
  for (std::size_t i = 0; i < cfg.ops / 10; ++i) {
    (void)db.get({}, to_bytes(key_for(zipf(rng))), out);
  }

  const auto before = db.stats();
  std::vector<std::uint64_t> lat;
  lat.reserve(cfg.ops);
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < cfg.ops; ++i) {
    const auto key = key_for(zipf(rng));
    const auto t0 = std::chrono::steady_clock::now();
    (void)db.get({}, to_bytes(key), out);
    const auto t1 = std::chrono::steady_clock::now();
    lat.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto after = db.stats();

  Result r;
  r.backend = backend == bytecask::IoBackend::Pread    ? "pread"
              : backend == bytecask::IoBackend::Mmap   ? "mmap"
                                                       : "buffer_pool";
  r.direct_io = direct_io;
  r.ratio = ratio;
  r.pool_bytes = pool_bytes;
  const auto secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
  r.ops_per_sec = secs > 0 ? static_cast<double>(cfg.ops) / secs : 0.0;
  r.p50_ns = percentile(lat, 0.50);
  r.p99_ns = percentile(lat, 0.99);
  r.p999_ns = percentile(lat, 0.999);

  if (backend == bytecask::IoBackend::BufferPool) {
    const auto hits = after.at("bytecask.pool_hits") -
                      before.at("bytecask.pool_hits");
    const auto misses = after.at("bytecask.pool_misses") -
                        before.at("bytecask.pool_misses");
    const auto total = hits + misses;
    r.hit_ratio = total > 0 ? static_cast<double>(hits) /
                                  static_cast<double>(total)
                            : 0.0;
    r.evictions = after.at("bytecask.pool_evictions") -
                  before.at("bytecask.pool_evictions");
    const auto touched = after.at("bytecask.pool_evicted_bytes_touched") -
                         before.at("bytecask.pool_evicted_bytes_touched");
    r.u = r.evictions > 0
              ? static_cast<double>(touched) /
                    (static_cast<double>(r.evictions) * 4096.0)
              : -1.0;  // nothing evicted: u is undefined, not zero
    r.retries = after.at("bytecask.pool_optimistic_retries") -
                before.at("bytecask.pool_optimistic_retries");
  }
  return r;
}

// Same workload as measure(), split across threads that share one DB and
// one Zipf sampler. Reported throughput is aggregate over wall time; the
// percentiles are over every thread's samples pooled.
auto measure_mt(const Config &cfg, bytecask::IoBackend backend,
                std::uint64_t pool_bytes, unsigned threads) -> Result {
  bytecask::Options opts{.max_file_bytes = cfg.max_file_bytes,
                         .io_backend = backend};
  if (backend == bytecask::IoBackend::BufferPool) {
    opts.buffer_pool.capacity_bytes = static_cast<std::size_t>(pool_bytes);
    opts.buffer_pool.direct_io = false;
  }
  auto db = bytecask::DB::open(cfg.dir, opts);
  const Zipf zipf{cfg.keys, cfg.zipf_s};
  const auto per_thread = cfg.ops / threads;

  // Warm on one thread so every thread starts against the same resident set.
  {
    std::mt19937_64 rng{42};
    bytecask::Bytes out;
    for (std::size_t i = 0; i < cfg.ops / 10; ++i) {
      (void)db.get({}, to_bytes(key_for(zipf(rng))), out);
    }
  }

  const auto before = db.stats();
  std::vector<std::vector<std::uint64_t>> lats(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);
  const auto start = std::chrono::steady_clock::now();
  for (unsigned t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng{1000 + t};
      bytecask::Bytes out;
      auto &lat = lats[t];
      lat.reserve(per_thread);
      for (std::size_t i = 0; i < per_thread; ++i) {
        const auto key = key_for(zipf(rng));
        const auto t0 = std::chrono::steady_clock::now();
        (void)db.get({}, to_bytes(key), out);
        const auto t1 = std::chrono::steady_clock::now();
        lat.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count()));
      }
    });
  }
  for (auto &w : workers) w.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto after = db.stats();

  std::vector<std::uint64_t> all;
  all.reserve(per_thread * threads);
  for (auto &l : lats) all.insert(all.end(), l.begin(), l.end());

  Result r;
  r.backend = backend == bytecask::IoBackend::Pread    ? "pread"
              : backend == bytecask::IoBackend::Mmap   ? "mmap"
                                                       : "buffer_pool";
  r.threads = threads;
  r.ratio = backend == bytecask::IoBackend::BufferPool ? 1.0 : 0.0;
  r.pool_bytes = pool_bytes;
  const auto secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
  r.ops_per_sec = secs > 0 ? static_cast<double>(all.size()) / secs : 0.0;
  r.p50_ns = percentile(all, 0.50);
  r.p99_ns = percentile(all, 0.99);
  r.p999_ns = percentile(all, 0.999);
  if (backend == bytecask::IoBackend::BufferPool) {
    const auto hits = after.at("bytecask.pool_hits") - before.at("bytecask.pool_hits");
    const auto misses = after.at("bytecask.pool_misses") - before.at("bytecask.pool_misses");
    r.hit_ratio = hits + misses > 0
                      ? static_cast<double>(hits) / static_cast<double>(hits + misses)
                      : 0.0;
    r.retries = after.at("bytecask.pool_optimistic_retries") -
                before.at("bytecask.pool_optimistic_retries");
  }
  return r;
}

// Put a fresh key, read it straight back, repeat. Under the pool the read
// should be a hit on a frame the writer just filled; under pread and mmap it
// is a page-cache read of bytes just written. Only the gets are timed.
auto measure_ryow(const Config &cfg, bytecask::IoBackend backend,
                  std::uint64_t pool_bytes) -> Result {
  bytecask::Options opts{.max_file_bytes = cfg.max_file_bytes,
                         .io_backend = backend};
  if (backend == bytecask::IoBackend::BufferPool) {
    opts.buffer_pool.capacity_bytes = static_cast<std::size_t>(pool_bytes);
    opts.buffer_pool.direct_io = false;
  }
  auto db = bytecask::DB::open(cfg.dir, opts);
  const std::string value(cfg.value_bytes, 'w');
  bytecask::Bytes out;
  std::vector<std::uint64_t> lat;
  lat.reserve(cfg.ryow_pairs);
  const auto before = db.stats();
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < cfg.ryow_pairs; ++i) {
    const auto key = std::format("ryow{:09d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes(value));
    const auto t0 = std::chrono::steady_clock::now();
    (void)db.get({}, to_bytes(key), out);
    const auto t1 = std::chrono::steady_clock::now();
    lat.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto after = db.stats();
  Result r;
  r.backend = backend == bytecask::IoBackend::Pread    ? "pread"
              : backend == bytecask::IoBackend::Mmap   ? "mmap"
                                                       : "buffer_pool";
  r.ryow = true;
  r.ratio = backend == bytecask::IoBackend::BufferPool ? 1.0 : 0.0;
  r.pool_bytes = pool_bytes;
  const auto secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
  r.ops_per_sec = secs > 0 ? static_cast<double>(cfg.ryow_pairs) / secs : 0.0;
  r.p50_ns = percentile(lat, 0.50);
  r.p99_ns = percentile(lat, 0.99);
  r.p999_ns = percentile(lat, 0.999);
  if (backend == bytecask::IoBackend::BufferPool) {
    const auto hits = after.at("bytecask.pool_hits") - before.at("bytecask.pool_hits");
    const auto misses = after.at("bytecask.pool_misses") - before.at("bytecask.pool_misses");
    r.hit_ratio = hits + misses > 0
                      ? static_cast<double>(hits) / static_cast<double>(hits + misses)
                      : 0.0;
  }
  return r;
}

auto parse_size(std::string_view s) -> std::size_t {
  std::size_t v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

} // namespace

auto main(int argc, char **argv) -> int {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    const auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view{argv[++i]} : std::string_view{};
    };
    if (a == "--keys") cfg.keys = parse_size(next());
    else if (a == "--value-bytes") cfg.value_bytes = parse_size(next());
    else if (a == "--ops") cfg.ops = parse_size(next());
    else if (a == "--max-file-bytes") cfg.max_file_bytes = parse_size(next());
    else if (a == "--dir") cfg.dir = std::string{next()};
    else if (a == "--zipf") cfg.zipf_s = std::stod(std::string{next()});
    else if (a == "--ratios") {
      cfg.ratios.clear();
      std::string list{next()};
      std::size_t pos = 0;
      while (pos <= list.size()) {
        const auto comma = list.find(',', pos);
        const auto tok = list.substr(pos, comma - pos);
        if (!tok.empty()) cfg.ratios.push_back(std::stod(tok));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (a == "--ryow") {
      cfg.ryow_pairs = parse_size(next());
    } else if (a == "--mt-threads") {
      cfg.mt_threads.clear();
      std::string list{next()};
      std::size_t pos = 0;
      while (pos <= list.size()) {
        const auto comma = list.find(',', pos);
        const auto tok = list.substr(pos, comma - pos);
        if (!tok.empty()) cfg.mt_threads.push_back(static_cast<unsigned>(std::stoul(tok)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (a == "--help") {
      std::puts(
          "pool_bench [--keys N] [--value-bytes N] [--ops N] [--zipf S]\n"
          "           [--max-file-bytes N] [--ratios a,b,c] [--dir PATH]\n"
          "           [--mt-threads a,b,c]  (multi-reader arm at ratio 1.0)\n"
          "           [--ryow N]            (N put/get pairs; gets timed)");
      return 0;
    }
  }

  std::fprintf(stderr, "building dataset: %zu keys x %zu B\n", cfg.keys,
               cfg.value_bytes);
  build_dataset(cfg);
  const auto bytes = dataset_bytes(cfg.dir);
  std::fprintf(stderr, "dataset on disk: %.1f MiB\n",
               static_cast<double>(bytes) / (1024.0 * 1024.0));

  std::vector<Result> results;
  // Baselines first, so a regression in the pool is read against them rather
  // than against an absolute number that says nothing on its own.
  results.push_back(measure(cfg, bytecask::IoBackend::Pread, 0, 0.0, false));
  results.push_back(measure(cfg, bytecask::IoBackend::Mmap, 0, 0.0, false));

  // Read-your-own-writes arm. Before any direct arm, like the one below.
  if (cfg.ryow_pairs > 0) {
    results.push_back(measure_ryow(cfg, bytecask::IoBackend::Pread, 0));
    results.push_back(measure_ryow(cfg, bytecask::IoBackend::Mmap, 0));
    results.push_back(measure_ryow(cfg, bytecask::IoBackend::BufferPool,
                                   bytes + 2 * cfg.max_file_bytes));
  }

  // Multi-reader arm: is the pool's lock-free read path still lock-free
  // under contention? mmap is the bar; pread is context. Fully resident,
  // and run here — before any direct arm — because a direct open drops
  // the page cache and every row in this arm depends on it being warm.
  for (const auto threads : cfg.mt_threads) {
    if (threads == 0) continue;
    results.push_back(measure_mt(cfg, bytecask::IoBackend::Pread, 0, threads));
    results.push_back(measure_mt(cfg, bytecask::IoBackend::Mmap, 0, threads));
    results.push_back(measure_mt(cfg, bytecask::IoBackend::BufferPool,
                                 bytes + 2 * cfg.max_file_bytes, threads));
  }

  // Every buffered arm runs before any direct arm. A direct open drops the
  // file's page-cache residency (FADV_DONTNEED), and a buffered run that
  // followed it would start cold and report disk latency as pool cost. In
  // this order the buffered arms and the baselines all see the page cache
  // the dataset build left warm, and the direct arms — which never use it —
  // can drop it freely.
  for (const bool direct_io : {false, true}) {
    for (const auto ratio : cfg.ratios) {
      auto pool_bytes = static_cast<std::uint64_t>(
          static_cast<double>(bytes) * ratio);
      // DB::open rejects a pool below 2 x max_file_bytes; a ratio that lands
      // under the floor is reported rather than silently clamped.
      if (pool_bytes < 2 * cfg.max_file_bytes) {
        std::fprintf(stderr,
                     "ratio %.2f -> %llu B is below 2 x max_file_bytes "
                     "(%llu B); skipped\n",
                     ratio, static_cast<unsigned long long>(pool_bytes),
                     static_cast<unsigned long long>(2 * cfg.max_file_bytes));
        continue;
      }
      results.push_back(measure(cfg, bytecask::IoBackend::BufferPool,
                                pool_bytes, ratio, direct_io));
    }
  }

  std::printf(
      "backend,direct_io,threads,ryow,ratio,pool_bytes,dataset_bytes,keys,"
      "value_bytes,ops,zipf_s,ops_per_sec,p50_ns,p99_ns,p999_ns,hit_ratio,u,"
      "evictions,optimistic_retries\n");
  for (const auto &r : results) {
    std::printf("%s,%d,%u,%d,%.3f,%llu,%llu,%zu,%zu,%zu,%.3f,%.1f,%llu,%llu,%llu,",
                r.backend.c_str(), r.direct_io ? 1 : 0, r.threads, r.ryow ? 1 : 0,
                r.ratio,
                static_cast<unsigned long long>(r.pool_bytes),
                static_cast<unsigned long long>(bytes), cfg.keys,
                cfg.value_bytes, cfg.ops, cfg.zipf_s, r.ops_per_sec,
                static_cast<unsigned long long>(r.p50_ns),
                static_cast<unsigned long long>(r.p99_ns),
                static_cast<unsigned long long>(r.p999_ns));
    if (r.hit_ratio < 0.0) {
      std::printf(",,,\n");  // no pool: these are absent, not zero
    } else {
      std::printf("%.4f,", r.hit_ratio);
      if (r.u < 0.0) std::printf(",");  // undefined without an eviction
      else std::printf("%.4f,", r.u);
      std::printf("%lld,%lld\n", static_cast<long long>(r.evictions),
                  static_cast<long long>(r.retries));
    }
  }

  std::filesystem::remove_all(cfg.dir);
  return 0;
}
