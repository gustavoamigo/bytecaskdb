// SPDX-License-Identifier: MIT
// Chaos worker: the engine side of the chaos rig. See
// docs/chaos_testing_design.md; tests/chaos/run_chaos.py drives it.
//
// `run` opens a database (on a chaosfs mount) and runs the crash harness's
// single-writer workload with a vacuum thread and reader threads, streaming
// the history to fd N. Unlike the crash child it expects failures: a write
// that throws is recorded as Throw (outcome unknown) or Rejected (DbDegraded,
// nothing written), and the writer then recovers — resume() while the engine
// is degraded — and sends a View of the whole database before writing again,
// so every unknown outcome is settled by the next View or is the last write.
// SIGTERM ends it cleanly: resume if degraded, destroy the DB, send Closed.
//
// `check` reads one process life's history, recovers a copy of the directory
// the life left (serially and in parallel), and checks, against the state
// the previous life's check recovered:
//
//   - every View: the database the running process served equals the model
//     after some prefix of the history covering the durable watermark. A View
//     taken without a degrade in between must cover every committed write.
//   - the recovered contents: a prefix covering the watermark, the recovered
//     durable_sequence at or above it, and after a clean close every write.
//   - the life's own open agreed with the previous check's recovery.
//   - no Violation frame (a reader saw a value it could not have, or an engine
//     error that is never legitimate).
//
// On success it rewrites the state file with what it recovered. The View and
// recovery checks are the same whether the engine recovered through resume()
// or a restart: design invariants I1–I7.
//
// Exit codes of `run`: 0 closed cleanly, 4 DB::open failed (reported in an
// OpenFailed frame), 5 std::bad_alloc (the rig's address-space limit).
//
// Usage:
//   chaos_worker run   --dir D --seed S --fd N
//                      [--as-headroom BYTES] [--nofile-headroom N] [--no-vacuum]
//   chaos_worker check --dir CRASHED --work W --history H --state F --seed S
//                      --terminator power|sigkill|clean|exit [--evicted]
//                      [--report R]

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "../crash/crash_model.h"

namespace {

using namespace crash_model;

// ---------------------------------------------------------------------------
// Per-life configuration, derived from the life's seed so run and check
// agree without passing more than the seed.
// ---------------------------------------------------------------------------

struct Config {
  bytecask::IoBackend backend{bytecask::IoBackend::Pread};
  std::uint64_t max_file_bytes{0};
  int sync_percent{0};
  bool vacuum{false};
  unsigned recovery_threads{1};
  std::uint32_t max_hint_backlog{4};
  int readers{0};
};

auto config_for(std::uint64_t seed) -> Config {
  std::mt19937_64 rng{seed ^ 0xc2b2ae3d27d4eb4fULL};
  Config c;
  // No Mmap: chaosfs serves every read itself (FUSE direct_io), and the
  // kernel refuses a shared mapping of such a file.
  c.backend = rng() % 2 == 0 ? bytecask::IoBackend::Pread
                             : bytecask::IoBackend::BufferPool;
  constexpr std::array sizes{std::uint64_t{16} << 10, std::uint64_t{64} << 10,
                             std::uint64_t{256} << 10, std::uint64_t{1} << 20};
  c.max_file_bytes = sizes[rng() % sizes.size()];
  constexpr std::array sync_percents{0, 5, 50, 100};
  c.sync_percent = sync_percents[rng() % sync_percents.size()];
  c.vacuum = rng() % 4 != 0;
  c.recovery_threads = 1 + static_cast<unsigned>(rng() % 4);
  constexpr std::array backlogs{0U, 1U, 4U};
  c.max_hint_backlog = backlogs[rng() % backlogs.size()];
  c.readers = static_cast<int>(rng() % 3);
  return c;
}

auto db_options(const Config &c) -> bytecask::Options {
  bytecask::Options o;
  o.max_file_bytes = c.max_file_bytes;
  o.recovery_threads = c.recovery_threads;
  o.io_backend = c.backend;
  o.max_hint_backlog = c.max_hint_backlog;
  if (c.backend == bytecask::IoBackend::BufferPool)
    o.buffer_pool.capacity_bytes = 4 * c.max_file_bytes + (1 << 20);
  return o;
}

auto describe_config(const Config &c) -> std::string {
  return std::format(
      "backend={} max_file_bytes={} sync%={} vacuum={} recovery_threads={} "
      "max_hint_backlog={} readers={}",
      c.backend == bytecask::IoBackend::Pread ? "pread" : "buffer_pool",
      c.max_file_bytes, c.sync_percent, c.vacuum, c.recovery_threads,
      c.max_hint_backlog, c.readers);
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

std::atomic<bool> g_stop{false};

extern "C" void on_sigterm(int /*sig*/) { g_stop.store(true); }

class Channel {
public:
  explicit Channel(int fd) : fd_{fd} {}
  void send(const Writer &w) {
    std::lock_guard<std::mutex> lk{mu_};
    send_frame(fd_, w);
  }
  void text(FrameType t, std::string_view s) {
    Writer w;
    w.u8(static_cast<std::uint8_t>(t));
    w.str(s);
    send(w);
  }
  void u64(FrameType t, std::uint64_t v) {
    Writer w;
    w.u8(static_cast<std::uint8_t>(t));
    w.u64(v);
    send(w);
  }

private:
  int fd_;
  std::mutex mu_;
};

auto scan(const bytecask::DB &db) -> State {
  State s;
  for (auto &entry : db.iter_from({}))
    s.emplace(as_string(entry.key), as_string(entry.value));
  return s;
}

void sleep_ms(std::uint64_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Brings the engine back to a writable state and sends a View. Returns false
// if asked to stop first. `resumed` records whether a degrade was cleared,
// which is what lets the checker accept lost unsynced writes in the View.
auto recover(bytecask::DB &db, Channel &ch, std::mt19937_64 &rng, bool on_stop)
    -> bool {
  bool resumed = false;
  std::string last_error;
  for (int attempt = 0;; ++attempt) {
    if (g_stop.load() && !on_stop) return false;
    if (on_stop && attempt >= 40) {
      ch.text(FrameType::Violation,
              std::format("engine still not recovered at clean close, with "
                          "the hazards lifted: {}",
                          last_error));
      return false;
    }
    try {
      if (db.is_degraded()) {
        db.resume();
        resumed = true;
      }
      const auto contents = scan(db);
      Writer w;
      w.u8(static_cast<std::uint8_t>(FrameType::View));
      w.u8(resumed ? 1 : 0);
      w.u64(db.durable_sequence());
      encode_state(w, contents);
      ch.send(w);
      return true;
    } catch (const std::exception &e) {
      last_error = e.what();
    }
    sleep_ms(10 + rng() % 100);
  }
}

void reader_loop(const bytecask::DB &db, Channel &ch, std::uint64_t seed,
                 const std::atomic<std::uint64_t> &issued, int id) {
  std::mt19937_64 rng{seed + 100 + static_cast<std::uint64_t>(id)};
  const auto own = std::format("{:016x}.", seed);
  bytecask::Bytes out;
  while (!g_stop.load(std::memory_order_relaxed)) {
    const auto key = key_name(static_cast<int>(rng() % kKeySpace));
    const auto issued_before = issued.load();
    try {
      if (db.get({}, as_view(key), out)) {
        const auto v = as_string(out);
        if (!value_matches_key(v, key)) {
          ch.text(FrameType::Violation,
                  std::format("get({}) returned a value that is not one "
                              "written to it: {}",
                              key, brief(v)));
        } else if (v.starts_with(own)) {
          const auto ordinal = std::stoull(v.substr(own.size()));
          if (ordinal >= issued.load())
            ch.text(FrameType::Violation,
                    std::format("get({}) returned value {} before it was "
                                "written (issued {})",
                                key, brief(v), issued_before));
        }
      }
    } catch (const bytecask::DbDegraded &) {
    } catch (const std::system_error &) {
      // An I/O error on a read is an honest failure.
    } catch (const std::exception &e) {
      // A CRC mismatch or a record that does not parse: the filesystem never
      // returns bytes that were not written, so the engine served a location
      // whose bytes it never made durable, or never wrote.
      ch.text(FrameType::Violation,
              std::format("get({}) failed on data it published: {}", key,
                          e.what()));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
  }
}

// Starts the vacuum and reader threads. Under the rig's address-space limit
// creating them can fail; that is the harness running out of memory, not the
// engine failing, so it exits as out of memory.
auto start_threads(bytecask::DB *db, Channel &ch, const Config &cfg,
                   std::uint64_t seed, const std::atomic<std::uint64_t> &issued)
    -> std::vector<std::jthread> {
  std::vector<std::jthread> threads;
  try {
    if (cfg.vacuum) {
      threads.emplace_back([db, seed] {
        std::mt19937_64 vrng{seed + 1};
        while (!g_stop.load(std::memory_order_relaxed)) {
          try {
            const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
            (void)db->vacuum({.fragmentation_threshold = threshold});
          } catch (const std::exception &) {
            // A vacuum that fails on I/O leaves the files as they were, or
            // degrades the engine; the writer recovers it.
          }
          std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
        }
      });
    }
    for (int i = 0; i < cfg.readers; ++i)
      threads.emplace_back([db, &ch, seed, &issued, i] {
        reader_loop(*db, ch, seed, issued, i);
      });
  } catch (const std::system_error &) {
    std::_Exit(5);
  }
  return threads;
}

// Limits applied once the database is open, so they land during the run:
// the current address-space size or descriptor count plus a headroom. Zero
// headroom means no limit.
struct RunLimits {
  std::uint64_t as_headroom{0};
  std::uint64_t nofile_headroom{0};
  bool no_vacuum{false}; // not a limit: leaves the vacuum thread out
};

auto current_vm_bytes() -> std::uint64_t {
  std::ifstream in{"/proc/self/status"};
  std::string line;
  while (std::getline(in, line))
    if (line.starts_with("VmSize:")) return std::stoull(line.substr(7)) * 1024;
  return 0;
}

// Soft limits only, below the current hard limit: raising a hard limit needs
// privileges.
void set_soft_limit(int resource, std::uint64_t value) {
  rlimit r{};
  ::getrlimit(resource, &r);
  r.rlim_cur = std::min<rlim_t>(value, r.rlim_max);
  ::setrlimit(resource, &r);
}

// The clean close runs with every hazard lifted, the worker's own limits
// included: the soft limits go back to the hard ones.
void lift_limits() {
  for (const int resource : {RLIMIT_AS, RLIMIT_NOFILE}) {
    rlimit r{};
    ::getrlimit(resource, &r);
    r.rlim_cur = r.rlim_max;
    ::setrlimit(resource, &r);
  }
}

void apply_run_limits(const RunLimits &l) {
  if (l.as_headroom > 0)
    set_soft_limit(RLIMIT_AS, current_vm_bytes() + l.as_headroom);
  if (l.nofile_headroom > 0) {
    const auto open_fds = static_cast<std::uint64_t>(std::distance(
        fs::directory_iterator{"/proc/self/fd"}, fs::directory_iterator{}));
    set_soft_limit(RLIMIT_NOFILE, open_fds + l.nofile_headroom);
  }
}

auto run_open(bytecask::DB &db_ref, Channel &ch, const Config &cfg,
              std::uint64_t seed, const RunLimits &limits)
    -> int;

auto run(const fs::path &dir, std::uint64_t seed, int fd,
         const RunLimits &limits) -> int {
  std::signal(SIGTERM, on_sigterm);
  Channel ch{fd};
  auto cfg = config_for(seed);
  if (limits.no_vacuum) cfg.vacuum = false;
  bool opened = false;
  try {
    int rc = 0;
    {
      // DB is neither copyable nor movable: it lives in this scope, and
      // Closed is sent once its destructor has run.
      auto db = bytecask::DB::open(dir, db_options(cfg));
      opened = true;
      rc = run_open(db, ch, cfg, seed, limits);
    }
    Writer w;
    w.u8(static_cast<std::uint8_t>(FrameType::Closed));
    ch.send(w);
    return rc;
  } catch (const std::exception &e) {
    if (opened) throw;
    ch.text(FrameType::OpenFailed, e.what());
    return 4;
  }
}

auto run_open(bytecask::DB &db_ref, Channel &ch, const Config &cfg,
              std::uint64_t seed, const RunLimits &limits) -> int {
  auto *db = &db_ref;
  std::mt19937_64 rng{seed};
  {
    Writer w;
    w.u8(static_cast<std::uint8_t>(FrameType::Opened));
    w.u64(db->durable_sequence());
    w.u64(static_cast<std::uint64_t>(db->stats().at("bytecask.keydir_keys")));
    ch.send(w);
  }

  std::atomic<std::uint64_t> issued{0};
  auto threads = start_threads(db, ch, cfg, seed, issued);
  apply_run_limits(limits);

  std::uint64_t value_id = 0;
  constexpr int kMaxOps = 200'000;
  for (int i = 0; i < kMaxOps && !g_stop.load(); ++i) {
    const auto op = random_op(rng, cfg.sync_percent, seed, value_id);
    issued.store(value_id);
    {
      Writer w;
      w.u8(static_cast<std::uint8_t>(FrameType::Intent));
      encode_op(w, op);
      ch.send(w);
    }
    bool needs_recovery = false;
    try {
      const auto result = execute(*db, op);
      Writer w;
      if (result) {
        w.u8(static_cast<std::uint8_t>(FrameType::Commit));
        w.u64(result->sequence);
        w.u8(result->durable ? 1 : 0);
      } else {
        w.u8(static_cast<std::uint8_t>(FrameType::Abort));
      }
      ch.send(w);
    } catch (const bytecask::DbDegraded &e) {
      ch.text(FrameType::Rejected, e.what());
      needs_recovery = true;
    } catch (const std::logic_error &e) {
      ch.text(FrameType::Throw, e.what());
      ch.text(FrameType::Violation,
              std::format("write threw std::logic_error: {}", e.what()));
      needs_recovery = true;
    } catch (const std::exception &e) {
      ch.text(FrameType::Throw, e.what());
      needs_recovery = true;
    }
    if (needs_recovery && !recover(*db, ch, rng, false)) break;
    if (rng() % 16 == 0) ch.u64(FrameType::Watermark, db->durable_sequence());
  }
  while (!g_stop.load()) sleep_ms(20);

  // Clean close: the orchestrator lifted every hazard before SIGTERM.
  lift_limits();
  if (db->is_degraded()) (void)recover(*db, ch, rng, true);
  threads.clear();
  ch.u64(FrameType::Watermark, db->durable_sequence());
  return 0;
}

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------

// What the previous check left for the next one. `contents` is what a
// completed DB::open of the directory would serve. It is durable on the mount
// only once an open there completes (open makes the bytes it trusts durable),
// so the check also keeps `floor`, the last state known durable on the mount,
// and `pending`, the writes of the last life that opened, on top of it. A
// power cut before the next open completes may lose any of `pending` above
// its watermark, and nothing else.
struct Base {
  State contents;
  std::uint64_t durable{0};
  State floor;
  std::uint64_t floor_seq{0};
  std::vector<OpRecord> pending;
  std::uint64_t pending_watermark{0};
};

auto read_file(const fs::path &p) -> std::string {
  std::ifstream in{p, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

void encode_record(Writer &w, const OpRecord &rec) {
  encode_op(w, rec.op);
  w.u8(static_cast<std::uint8_t>(rec.outcome));
  w.u64(rec.sequence);
  w.u8(rec.durable ? 1 : 0);
}

auto decode_record(Reader &r) -> OpRecord {
  OpRecord rec;
  rec.op = decode_op(r);
  rec.outcome = static_cast<OpRecord::Outcome>(r.u8());
  rec.sequence = r.u64();
  rec.durable = r.u8() != 0;
  return rec;
}

auto load_base(const fs::path &p) -> Base {
  if (!fs::exists(p)) return {};
  const auto bytes = read_file(p);
  Reader r{bytes};
  Base b;
  b.durable = r.u64();
  b.contents = decode_state(r);
  b.floor_seq = r.u64();
  b.floor = decode_state(r);
  b.pending_watermark = r.u64();
  const auto n = r.u32();
  for (std::uint32_t i = 0; i < n; ++i) b.pending.push_back(decode_record(r));
  return b;
}

void store_base(const fs::path &p, const Base &b) {
  Writer w;
  w.u64(b.durable);
  encode_state(w, b.contents);
  w.u64(b.floor_seq);
  encode_state(w, b.floor);
  w.u64(b.pending_watermark);
  w.u32(static_cast<std::uint32_t>(b.pending.size()));
  for (const auto &rec : b.pending) encode_record(w, rec);
  const auto tmp = fs::path{p}.concat(".tmp");
  {
    std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
    out << w.data();
  }
  fs::rename(tmp, p);
}

struct Life {
  bool opened{false};
  std::uint64_t opened_durable{0};
  std::uint64_t opened_keys{0};
  std::optional<std::string> open_failed;
  bool closed{false};
  std::uint64_t watermark{0};
  // Operations still unsettled by a View, in order. A View folds the prefix
  // it matched into `settled`.
  std::vector<OpRecord> ops;
  std::vector<OpRecord> all; // every operation, for the report
  std::vector<std::string> violations;
  std::vector<std::string> throws;
  int views{0};
  int resumed_views{0};
  std::size_t reads_of_lost_pages{0};
  std::size_t reopen_required{0};
  std::size_t lost_at_resume{0};
  std::size_t committed{0};
};

// Number of operations in ops that returned a result or were shown applied.
auto settled_count(std::span<const OpRecord> ops) -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(ops, [](const OpRecord &r) {
    return r.outcome == OpRecord::Outcome::Committed ||
           r.outcome == OpRecord::Outcome::Applied;
  }));
}

// Checks one View and settles the operations before it.
void apply_view(Life &life, const Base &base, bool resumed,
                std::uint64_t durable, const State &view) {
  using Outcome = OpRecord::Outcome;
  if (durable < life.watermark)
    throw Failure{std::format("durable_sequence went back from {} to {} "
                              "across a recovery",
                              life.watermark, durable)};
  PrefixResult r;
  try {
    r = match_prefix(base.contents, life.ops, life.opened_durable,
                     life.watermark, view);
  } catch (const Failure &f) {
    throw Failure{std::format("View {} ({}): the running database {}",
                              life.views, resumed ? "after resume()" : "after a failed write",
                              f.what)};
  }
  std::vector<OpRecord> effective;
  for (const auto &rec : life.ops)
    if (rec.outcome != Outcome::Aborted && rec.outcome != Outcome::Rejected)
      effective.push_back(rec);
  const auto certain = settled_count(effective);
  if (r.applied < certain) {
    if (!resumed)
      throw Failure{std::format(
          "View {} after a failed write, with no degrade: the running database "
          "lost {} acknowledged writes",
          life.views, certain - r.applied)};
    life.lost_at_resume += certain - r.applied;
  }
  effective.resize(std::min(effective.size(), r.applied));
  for (auto &rec : effective)
    if (rec.outcome == Outcome::Unknown || rec.outcome == Outcome::InFlight)
      rec.outcome = Outcome::Applied;
  life.ops = std::move(effective);
  life.watermark = std::max(life.watermark, durable);
  ++life.views;
  if (resumed) ++life.resumed_views;
}

// Fills `life` as it goes, so a failure report shows the history up to the
// frame that failed. `cache_lost`: an eviction dropped pages whose writeback
// had failed, while the process ran. Their published, non-durable writes are
// gone before the engine can know (it learns at its next fdatasync), so a
// View may lack writes above the watermark, and a read of such a write may
// fail. A read may still never return a value that was not written.
void parse_life(std::string_view stream, const Base &base, bool cache_lost,
                Life &life) {
  using Outcome = OpRecord::Outcome;
  for (const auto payload : split_frames(stream)) {
    Reader r{payload};
    switch (static_cast<FrameType>(r.u8())) {
    case FrameType::Opened:
      life.opened = true;
      life.opened_durable = r.u64();
      life.opened_keys = r.u64();
      life.watermark = life.opened_durable;
      break;
    case FrameType::OpenFailed:
      life.open_failed = r.str();
      break;
    case FrameType::Intent:
      life.ops.push_back({.op = decode_op(r)});
      life.all.push_back(life.ops.back());
      break;
    case FrameType::Commit:
      life.ops.back().outcome = Outcome::Committed;
      life.ops.back().sequence = r.u64();
      life.ops.back().durable = r.u8() != 0;
      if (life.ops.back().durable)
        life.watermark = std::max(life.watermark, life.ops.back().sequence);
      life.all.back() = life.ops.back();
      ++life.committed;
      break;
    case FrameType::Abort:
      life.ops.back().outcome = Outcome::Aborted;
      life.all.back() = life.ops.back();
      break;
    case FrameType::Throw:
      life.ops.back().outcome = Outcome::Unknown;
      life.all.back() = life.ops.back();
      life.throws.push_back(r.str());
      break;
    case FrameType::Rejected:
      life.ops.back().outcome = Outcome::Rejected;
      life.all.back() = life.ops.back();
      break;
    case FrameType::Watermark:
      life.watermark = std::max(life.watermark, r.u64());
      break;
    case FrameType::View: {
      const bool resumed = r.u8() != 0;
      const auto durable = r.u64();
      const auto view = decode_state(r);
      apply_view(life, base, resumed || cache_lost, durable, view);
      break;
    }
    case FrameType::Violation: {
      auto what = r.str();
      if (cache_lost && what.find("failed on data it published") != std::string::npos)
        ++life.reads_of_lost_pages;
      else if (cache_lost &&
               what.find("Reopen the database to recover") != std::string::npos)
        // resume() refuses to trim published data the eviction took, and
        // sends the caller to a reopen: the next life's open must succeed.
        ++life.reopen_required;
      else
        life.violations.push_back(std::move(what));
      break;
    }
    case FrameType::Closed:
      life.closed = true;
      break;
    }
  }
}

struct CheckOptions {
  fs::path dir;
  fs::path work;
  fs::path history;
  fs::path state;
  fs::path report;
  std::uint64_t seed{0};
  // How the life ended: power (a power cut), sigkill, clean (SIGTERM with
  // the hazards lifted) or exit (the worker exited on its own).
  std::string terminator;
  // An eviction dropped pages whose writeback had failed, during or after
  // the life: like a power cut, that loses what only the page cache held.
  bool evicted{false};
};

void write_report(const CheckOptions &o, const Config &cfg, const Life &life,
                  const std::string &what) {
  if (o.report.empty()) return;
  std::ofstream out{o.report};
  out << "FAIL " << what << "\n";
  out << "seed " << o.seed << " " << describe_config(cfg) << "\n";
  out << "watermark " << life.watermark << " opened_durable "
      << life.opened_durable << " views " << life.views << "\n";
  for (const auto &t : life.throws) out << "threw: " << t << "\n";
  for (const auto &v : life.violations) out << "violation: " << v << "\n";
  for (const auto &rec : life.all) out << describe(rec, nullptr) << "\n";
}

auto check(const CheckOptions &o) -> int {
  const auto cfg = config_for(o.seed);
  const auto base = load_base(o.state);
  const auto stream = read_file(o.history);
  Life life;
  try {
    parse_life(stream, base, o.evicted, life);
    if (!life.violations.empty())
      throw Failure{std::format("the worker saw {} violations; first: {}",
                                life.violations.size(), life.violations.front())};
    if (life.opened && (life.opened_durable != base.durable ||
                        life.opened_keys != base.contents.size()))
      throw Failure{std::format(
          "the worker's open disagrees with the previous recovery: "
          "durable_sequence {} vs {}, keys {} vs {}",
          life.opened_durable, base.durable, life.opened_keys,
          base.contents.size())};

    fs::create_directories(o.work);
    const auto recovered = recover_both(o.dir, o.work, db_options(cfg));

    std::size_t applied = 0;
    std::size_t total = 0;
    const bool power_cut = o.terminator == "power";
    if (!life.opened) {
      // Nothing was written. Unless the page cache lost something (a power
      // cut or an eviction), recovery reproduces the previous state; if it
      // did, it may lose what no completed open made durable: a prefix of the
      // pending writes covering their watermark.
      const auto why = life.open_failed.value_or("killed during open");
      if (!power_cut && !o.evicted) {
        if (recovered.contents != base.contents)
          throw Failure{std::format("the life never opened the database ({}), "
                                    "and the directory lost state",
                                    why)};
      } else {
        try {
          (void)match_prefix(base.floor, base.pending, base.floor_seq,
                             base.pending_watermark, recovered.contents);
        } catch (const Failure &f) {
          throw Failure{std::format("the life never opened the database ({}), "
                                    "and after the {} {}",
                                    why, power_cut ? "power cut" : "eviction",
                                    f.what)};
        }
      }
    } else {
      const auto r = match_prefix(base.contents, life.ops, life.opened_durable,
                                  life.watermark, recovered.contents);
      applied = r.applied;
      total = r.total;
      if (recovered.durable_sequence < life.watermark)
        throw Failure{std::format(
            "recovered durable_sequence {} below the watermark {}",
            recovered.durable_sequence, life.watermark)};
      // A clean close keeps every write, unless an eviction took some before
      // the engine could know.
      if (o.terminator == "clean" && life.closed && !o.evicted &&
          r.applied < settled_count(life.ops))
        throw Failure{std::format(
            "after a clean close, recovery lost {} acknowledged writes",
            settled_count(life.ops) - r.applied)};
    }

    Base next;
    next.contents = recovered.contents;
    next.durable = recovered.durable_sequence;
    if (power_cut || (o.terminator == "clean" && life.closed)) {
      // Nothing is left only in the page cache.
      next.floor = recovered.contents;
      next.floor_seq = recovered.durable_sequence;
    } else if (life.opened) {
      // The open made the state it started from durable; this life's own
      // writes may still be only in the page cache.
      next.floor = base.contents;
      next.floor_seq = life.opened_durable;
      next.pending = life.ops;
      next.pending_watermark = life.watermark;
    } else {
      next.floor = base.floor;
      next.floor_seq = base.floor_seq;
      next.pending = base.pending;
      next.pending_watermark = base.pending_watermark;
    }
    store_base(o.state, next);
    std::printf(
        "{\"ok\": true, \"opened\": %s, \"open_failed\": %s, \"closed\": %s, "
        "\"committed\": %zu, \"views\": %d, \"resumed_views\": %d, "
        "\"throws\": %zu, \"bad_alloc\": %zu, \"lost_at_resume\": %zu, "
        "\"tail_applied\": %zu, \"tail_total\": %zu, \"keys\": %zu}\n",
        life.opened ? "true" : "false", life.open_failed ? "true" : "false",
        life.closed ? "true" : "false", life.committed, life.views,
        life.resumed_views, life.throws.size(),
        static_cast<std::size_t>(std::ranges::count_if(
            life.throws,
            [](const std::string &t) { return t.find("bad_alloc") != std::string::npos; })),
        life.lost_at_resume, applied, total, recovered.contents.size());
    return 0;
  } catch (const Failure &f) {
    write_report(o, cfg, life, f.what);
    std::fprintf(stderr, "FAIL %s\n", f.what.c_str());
    return 1;
  }
}

auto parse_check(int argc, char **argv) -> CheckOptions {
  CheckOptions o;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        throw std::invalid_argument{std::format("{} needs a value", a)};
      return argv[++i];
    };
    if (a == "--dir") o.dir = next();
    else if (a == "--work") o.work = next();
    else if (a == "--history") o.history = next();
    else if (a == "--state") o.state = next();
    else if (a == "--report") o.report = next();
    else if (a == "--seed") o.seed = std::stoull(next());
    else if (a == "--terminator") o.terminator = next();
    else if (a == "--evicted") o.evicted = true;
    else throw std::invalid_argument{std::format("unknown argument {}", a)};
  }
  if (o.dir.empty() || o.work.empty() || o.history.empty() || o.state.empty() ||
      o.terminator.empty())
    throw std::invalid_argument{
        "check needs --dir, --work, --history, --state, --terminator"};
  return o;
}

} // namespace

// Under the rig's address-space limit, std::bad_alloc can escape from any
// thread, and terminate is the allowed way to die (design, "Invariants").
// The distinct exit code lets the orchestrator tell it from any other abort.
[[noreturn]] void on_terminate() {
  if (const auto ex = std::current_exception()) {
    try {
      std::rethrow_exception(ex);
    } catch (const std::bad_alloc &) {
      std::_Exit(5);
    } catch (...) {
    }
  }
  std::abort();
}

auto main(int argc, char **argv) -> int {
  std::set_terminate(on_terminate);
  try {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "run") {
      fs::path dir;
      std::uint64_t seed = 0;
      int fd = -1;
      RunLimits limits;
      for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string {
          if (i + 1 >= argc)
            throw std::invalid_argument{std::format("{} needs a value", a)};
          return argv[++i];
        };
        if (a == "--dir") dir = next();
        else if (a == "--seed") seed = std::stoull(next());
        else if (a == "--fd") fd = std::stoi(next());
        else if (a == "--as-headroom") limits.as_headroom = std::stoull(next());
        else if (a == "--nofile-headroom") limits.nofile_headroom = std::stoull(next());
        else if (a == "--no-vacuum") limits.no_vacuum = true;
        else throw std::invalid_argument{std::format("unknown argument {}", a)};
      }
      if (dir.empty() || fd < 0)
        throw std::invalid_argument{"run needs --dir and --fd"};
      return run(dir, seed, fd, limits);
    }
    if (mode == "check") return check(parse_check(argc, argv));
    if (mode == "config") {
      std::printf("%s\n", describe_config(config_for(std::stoull(argv[2]))).c_str());
      return 0;
    }
    std::fprintf(stderr, "usage: chaos_worker run|check|config ...\n");
    return 2;
  } catch (const std::bad_alloc &) {
    return 5;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "chaos_worker: %s\n", e.what());
    return 2;
  }
}
