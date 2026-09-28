// EXPERIMENT: closed-loop transactions shaped like a TPROC-C commit through
// the MariaDB plugin — snapshot, point reads, updates and inserts in one
// WritePlan, committed with sync=false while a background thread issues an
// empty sync=true commit every interval (the plugin's AT_INTERVAL sync).
// Between commits each thread spins (server CPU) and sleeps (client round
// trips). Reports throughput, batch shape and the group-commit timers.
//
//   commit_probe dir threads secs [key=value ...]

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

using namespace bytecask;
using Clock = std::chrono::steady_clock;

namespace {

auto bv(const std::string &s) -> BytesView {
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

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: commit_probe dir threads secs [k=v ...]\n");
    return 2;
  }
  const std::filesystem::path dir = argv[1];
  const int threads = std::atoi(argv[2]);
  const int secs = std::atoi(argv[3]);
  std::map<std::string, std::string> kv;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
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

  Options opts;
  opts.max_file_bytes = static_cast<std::uint64_t>(file_mb) << 20;
  opts.io_backend = IoBackend::BufferPool;
  opts.buffer_pool.capacity_bytes = std::size_t{4} << 30;
  opts.buffer_pool.direct_io = false;

  std::filesystem::remove_all(dir);
  auto db = DB::open(dir, opts);
  const std::string val(vsize, 'v');
  for (std::uint64_t i = 0; i < keys;) {
    WritePlan p;
    for (int j = 0; j < 1000 && i < keys; ++j, ++i) p.put(bv(key_of(i)), bv(val));
    (void)db.apply_batch({.sync = false}, std::move(p));
  }

  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> commits{0}, conflicts{0};
  std::thread syncer{[&] {
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sync_ms));
      (void)db.apply_batch({.sync = true}, WritePlan{});
    }
  }};
  std::vector<std::thread> ws;
  for (int t = 0; t < threads; ++t) {
    ws.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t) * 7919 + 1);
      std::uniform_int_distribution<std::uint64_t> pick(0, keys - 1);
      std::uint64_t next_insert = 0;
      Bytes out;
      const ReadOptions ro{.verify_checksums = false};
      while (!stop.load(std::memory_order_relaxed)) {
        spin_for(std::chrono::microseconds(spin_us));
        if (sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        auto snap = db.snapshot();
        for (long r = 0; r < reads; ++r) (void)snap.get(ro, bv(key_of(pick(rng))), out);
        WritePlan plan{std::move(snap)};
        for (long u = 0; u < updates; ++u) plan.put(bv(key_of(pick(rng))), bv(val));
        for (long n = 0; n < inserts; ++n)
          plan.put(bv(std::format("o:{:03}:{:012}", t, next_insert++)), bv(val));
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
  const auto t0 = Clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(secs));
  const auto s1 = db.stats();
  const auto c1 = commits.load();
  const double el = std::chrono::duration<double>(Clock::now() - t0).count();
  stop = true;
  for (auto &w : ws) w.join();
  syncer.join();

  auto d = [&](const char *k) {
    return static_cast<double>(s1.at(k) - s0.at(k));
  };
  const double n = static_cast<double>(c1 - c0);
  const double batches = d("bytecask.group_writer_batches");
  // Share of wall time: the leader executing, the handoff gap, etc.
  std::printf(
      "threads=%d spin=%ld sleep=%ld file_mb=%ld | commits/s %.0f  "
      "conflicts %lld | batch %.1f commits, %.0f/s | exec %.0f%%  handoff %.0f%% "
      "(%.1f us each)  mu_wait %.0f%%  append %.0f%%  rotate %.0f%% (%.0f x %.1f ms)"
      "  | exec/commit %.1f us\n",
      threads, spin_us, sleep_us, file_mb, n / el,
      static_cast<long long>(conflicts.load()), d("bytecask.group_writer_coalesced") / batches,
      batches / el, 100 * d("exp.exec_ns") / (el * 1e9),
      100 * d("exp.handoff_ns") / (el * 1e9),
      d("exp.handoffs") > 0 ? d("exp.handoff_ns") / d("exp.handoffs") / 1e3 : 0.0,
      100 * d("exp.mu_wait_ns") / (el * 1e9), 100 * d("exp.append_ns") / (el * 1e9),
      100 * d("exp.rotate_ns") / (el * 1e9), d("exp.rotate_n"),
      d("exp.rotate_n") > 0 ? d("exp.rotate_ns") / d("exp.rotate_n") / 1e6 : 0.0,
      d("exp.exec_ns") / d("bytecask.group_writer_coalesced") / 1e3);
  return 0;
}
