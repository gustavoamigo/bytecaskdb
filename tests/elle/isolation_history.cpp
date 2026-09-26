// SPDX-License-Identifier: MIT
// Isolation history generator: runs concurrent list-append transactions
// against one DB and writes the history as JSON for Elle (elle-cli) to check.
// See docs/isolation_checking_design.md.
//
// Each key holds a list of integers, stored as its value. A transaction is
// 1-4 micro-operations: [r k] reads the list from the transaction's snapshot,
// [append k e] extends it by a unique element and puts the whole list back.
// The emulated append is a true append only if the implicit W-W check on
// write keys holds, so the model tests that check rather than assuming it.
//
// Configurations (--config):
//   guarded    WritePlan(snap), ensure_unchanged on every key read but not
//              written. Expected strict-serializable.
//   unguarded  WritePlan(snap), no read guards. Expected snapshot isolation,
//              with write skew (G2-item) as the only serializability anomaly.
//   blind      db.get reads, snapshot-less WritePlan(). Expected to lose
//              appends: it shows the checker sees anomalies at all.
//
// Nemeses, drawn from the seed: a vacuum thread over small files, and a
// client that fails its own fdatasync through the fault injector, degrading
// the engine, then calls resume(). Every exception is recorded as :info: a
// write whose fdatasync failed can still be replayed by resume().
//
// Usage:
//   isolation_history --config guarded|unguarded|blind --out history.json
//                     [--seed S] [--threads N] [--txns N] [--dir PATH]
//                     [--no-vacuum] [--no-degrade]

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "fault_injector.h"

import bytecask;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

enum class Mode : std::uint8_t { Guarded, Unguarded, Blind };

auto mode_name(Mode m) -> std::string_view {
  switch (m) {
  case Mode::Guarded:
    return "guarded";
  case Mode::Unguarded:
    return "unguarded";
  case Mode::Blind:
    return "blind";
  }
  return "?";
}

struct RunOptions {
  Mode mode{Mode::Guarded};
  std::uint64_t seed{0};
  int threads{16};
  int txns{50'000};
  fs::path dir{"isolation_history_db"};
  fs::path out;
  bool no_vacuum{false};
  bool no_degrade{false};
};

// Per-run engine and nemesis configuration, derived from the seed.
struct Config {
  bytecask::IoBackend backend{bytecask::IoBackend::Pread};
  std::uint64_t max_file_bytes{0};
  int sync_percent{0};
  bool vacuum{false};
  bool degrade{false};
};

auto config_for(std::uint64_t seed) -> Config {
  std::mt19937_64 rng{seed ^ 0x9e3779b97f4a7c15ULL};
  Config c;
  constexpr std::array backends{bytecask::IoBackend::Pread,
                                bytecask::IoBackend::Mmap,
                                bytecask::IoBackend::BufferPool};
  c.backend = backends[rng() % backends.size()];
  // Small files rotate often, so rotation and compaction run under the
  // workload.
  constexpr std::array sizes{std::uint64_t{16} << 10, std::uint64_t{64} << 10,
                             std::uint64_t{256} << 10, std::uint64_t{1} << 20};
  c.max_file_bytes = sizes[rng() % sizes.size()];
  constexpr std::array sync_percents{0, 5, 50};
  c.sync_percent = sync_percents[rng() % sync_percents.size()];
  c.vacuum = rng() % 4 != 0;
  c.degrade = rng() % 2 != 0;
  return c;
}

auto backend_name(bytecask::IoBackend b) -> std::string_view {
  switch (b) {
  case bytecask::IoBackend::Pread:
    return "pread";
  case bytecask::IoBackend::Mmap:
    return "mmap";
  case bytecask::IoBackend::BufferPool:
    return "buffer_pool";
  }
  return "?";
}

auto db_options(const Config &c) -> bytecask::Options {
  bytecask::Options o;
  o.max_file_bytes = c.max_file_bytes;
  o.io_backend = c.backend;
  if (c.backend == bytecask::IoBackend::BufferPool)
    o.buffer_pool.capacity_bytes = 4 * c.max_file_bytes + (1 << 20);
  return o;
}

// ---------------------------------------------------------------------------
// Keys and values
// ---------------------------------------------------------------------------

using List = std::vector<std::int64_t>;

auto key_bytes(std::int64_t k) -> std::string { return std::format("k{}", k); }

auto as_view(std::string_view s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

auto encode_list(const List &l) -> std::string {
  std::string s;
  for (const auto e : l) {
    if (!s.empty()) s.push_back(',');
    s += std::to_string(e);
  }
  return s;
}

auto decode_list(std::span<const std::byte> b) -> List {
  List l;
  std::int64_t cur = 0;
  auto any = false;
  for (const auto byte : b) {
    const auto c = std::to_integer<char>(byte);
    if (c == ',') {
      l.push_back(cur);
      cur = 0;
      any = false;
    } else if (c >= '0' && c <= '9') {
      cur = cur * 10 + (c - '0');
      any = true;
    } else {
      throw std::runtime_error{"undecodable list value"};
    }
  }
  if (any) l.push_back(cur);
  return l;
}

// Active key slots. Few keys give contention; retiring a key after
// kAppendsPerKey appends keeps lists short, so values and Elle's cycle search
// stay small. Jepsen's list-append generator uses the same scheme.
class KeyPool {
public:
  static constexpr int kSlots = 8;
  static constexpr int kAppendsPerKey = 64;

  KeyPool() {
    for (int i = 0; i < kSlots; ++i) {
      slots_[static_cast<std::size_t>(i)] = next_key_++;
    }
  }

  // Picks a key. When for_append is set, counts the append and retires the
  // key once it has had kAppendsPerKey of them.
  auto pick(std::mt19937_64 &rng, bool for_append) -> std::int64_t {
    std::lock_guard<std::mutex> g{mu_};
    auto &slot = slots_[rng() % slots_.size()];
    const auto k = slot;
    if (for_append && ++appends_[k] >= kAppendsPerKey) slot = next_key_++;
    return k;
  }

  [[nodiscard]] auto key_count() const -> std::int64_t {
    std::lock_guard<std::mutex> g{mu_};
    return next_key_;
  }

private:
  mutable std::mutex mu_;
  std::array<std::int64_t, kSlots> slots_{};
  std::map<std::int64_t, int> appends_;
  std::int64_t next_key_{0};
};

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

struct Mop {
  bool append{false};
  std::int64_t key{0};
  std::int64_t element{0};  // append only
  std::optional<List> read; // read only; set on completion
};

enum class EventType : std::uint8_t { Invoke, Ok, Fail, Info };

auto event_type_name(EventType t) -> std::string_view {
  switch (t) {
  case EventType::Invoke:
    return "invoke";
  case EventType::Ok:
    return "ok";
  case EventType::Fail:
    return "fail";
  case EventType::Info:
    return "info";
  }
  return "?";
}

struct Event {
  std::uint64_t index{0};
  int process{0};
  EventType type{EventType::Invoke};
  std::int64_t time_ns{0};
  std::vector<Mop> value;
  std::uint64_t sequence{0}; // Ok with appends: CommitResult.sequence
  std::string error;         // Info: what the call threw
};

// Stamps :index at invoke and at completion. Events land in per-thread
// buffers and are merged by index once the run ends, so recording takes no
// shared lock.
class Recorder {
public:
  explicit Recorder(Clock::time_point start) : start_{start} {}

  auto stamp(Event &e) -> void {
    e.index = next_index_.fetch_add(1, std::memory_order_acq_rel);
    e.time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - start_)
                    .count();
  }

private:
  Clock::time_point start_;
  std::atomic<std::uint64_t> next_index_{0};
};

auto write_list_json(std::string &out, const List &l) -> void {
  out.push_back('[');
  for (std::size_t i = 0; i < l.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += std::to_string(l[i]);
  }
  out.push_back(']');
}

auto json_escape(std::string_view s) -> std::string {
  std::string r;
  for (const auto c : s) {
    if (c == '"' || c == '\\') {
      r.push_back('\\');
      r.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      r += std::format("\\u{:04x}", static_cast<unsigned>(c));
    } else {
      r.push_back(c);
    }
  }
  return r;
}

auto write_history(const fs::path &path, std::vector<Event> events) -> void {
  std::ranges::sort(events, {}, &Event::index);
  std::ofstream out{path};
  if (!out) throw std::runtime_error{"cannot open " + path.string()};
  out << "[\n";
  std::string line;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto &e = events[i];
    line.clear();
    line += std::format(R"({{"index":{},"process":{},"type":"{}","f":"txn",)"
                        R"("time":{},"value":[)",
                        e.index, e.process, event_type_name(e.type), e.time_ns);
    for (std::size_t j = 0; j < e.value.size(); ++j) {
      const auto &m = e.value[j];
      if (j != 0) line.push_back(',');
      if (m.append) {
        line += std::format(R"(["append",{},{}])", m.key, m.element);
      } else {
        line += std::format(R"(["r",{},)", m.key);
        if (m.read) {
          write_list_json(line, *m.read);
        } else {
          line += "null";
        }
        line.push_back(']');
      }
    }
    line.push_back(']');
    if (e.type == EventType::Ok && e.sequence != 0)
      line += std::format(R"(,"sequence":{})", e.sequence);
    if (e.type == EventType::Info)
      line += std::format(R"(,"error":"{}")", json_escape(e.error));
    line.push_back('}');
    if (i + 1 != events.size()) line.push_back(',');
    line.push_back('\n');
    out << line;
  }
  out << "]\n";
  if (!out) throw std::runtime_error{"write failed: " + path.string()};
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------

auto random_txn(std::mt19937_64 &rng, KeyPool &keys,
                std::atomic<std::int64_t> &next_element) -> std::vector<Mop> {
  const auto n = 1 + static_cast<int>(rng() % 4);
  std::vector<Mop> mops;
  for (int i = 0; i < n; ++i) {
    Mop m;
    m.append = rng() % 2 == 0;
    m.key = keys.pick(rng, m.append);
    if (m.append)
      m.element = next_element.fetch_add(1, std::memory_order_relaxed);
    mops.push_back(m);
  }
  return mops;
}

struct TxnOutcome {
  EventType type{EventType::Ok};
  std::uint64_t sequence{0};
  std::string error;
};

// Runs mops as one transaction. Fills each read mop's result. Keys the
// transaction touches are loaded once; later mops see its own earlier
// appends, as Transaction (Layer 2) would.
auto execute_txn(bytecask::DB &db, Mode mode, std::vector<Mop> &mops,
                 bytecask::WriteOptions wo) -> TxnOutcome {
  std::map<std::int64_t, List> lists;
  std::map<std::int64_t, bool> written;
  bytecask::Bytes buf;

  std::optional<bytecask::Snapshot> snap;
  if (mode != Mode::Blind) snap.emplace(db.snapshot());

  auto load = [&](std::int64_t k) -> List & {
    auto it = lists.find(k);
    if (it != lists.end()) return it->second;
    const auto kb = key_bytes(k);
    const auto found = snap ? snap->get({}, as_view(kb), buf)
                            : db.get({}, as_view(kb), buf);
    return lists.emplace(k, found ? decode_list(buf) : List{}).first->second;
  };

  for (auto &m : mops) {
    auto &l = load(m.key);
    if (m.append) {
      l.push_back(m.element);
      written[m.key] = true;
    } else {
      m.read = l;
    }
  }
  if (written.empty()) return {};

  auto plan = snap ? bytecask::WritePlan{std::move(*snap)}
                   : bytecask::WritePlan{};
  for (const auto &[k, l] : lists) {
    const auto kb = key_bytes(k);
    if (written.contains(k)) {
      plan.put(as_view(kb), as_view(encode_list(l)));
    } else if (mode == Mode::Guarded) {
      plan.ensure_unchanged(as_view(kb));
    }
  }
  const auto r = db.apply_batch(wo, std::move(plan));
  if (!r) return {.type = EventType::Fail, .sequence = 0, .error = {}};
  return {.type = EventType::Ok, .sequence = r->sequence, .error = {}};
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

struct Totals {
  std::atomic<int> ok{0};
  std::atomic<int> fail{0};
  std::atomic<int> info{0};
  std::atomic<int> degrades{0};
  std::atomic<int> vacuums{0};
};

// One Elle process. A client thread records invoke, runs the transaction and
// records the completion. inject_sync arms a fault at io_data_file_sync
// around the call: the solo path appends on this thread, and the fdatasync
// fails here too when this thread holds the flush role.
auto run_one(bytecask::DB &db, Mode mode, int process, Recorder &rec,
             std::vector<Event> &events, std::vector<Mop> mops,
             bytecask::WriteOptions wo, bool inject_sync, Totals &totals)
    -> void {
  Event inv;
  inv.process = process;
  inv.value = mops;
  rec.stamp(inv);

  TxnOutcome outcome;
  try {
    if (inject_sync) {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      outcome = execute_txn(db, mode, mops, wo);
    } else {
      outcome = execute_txn(db, mode, mops, wo);
    }
  } catch (const std::exception &e) {
    outcome = {.type = EventType::Info, .sequence = 0, .error = e.what()};
  }

  Event done;
  done.process = process;
  done.type = outcome.type;
  // A failed or indeterminate transaction reports what it invoked: its reads
  // carry no result.
  done.value = outcome.type == EventType::Ok ? std::move(mops) : inv.value;
  done.sequence = outcome.sequence;
  done.error = std::move(outcome.error);
  rec.stamp(done);
  switch (outcome.type) {
  case EventType::Ok:
    totals.ok.fetch_add(1, std::memory_order_relaxed);
    break;
  case EventType::Fail:
    totals.fail.fetch_add(1, std::memory_order_relaxed);
    break;
  case EventType::Info:
    totals.info.fetch_add(1, std::memory_order_relaxed);
    break;
  case EventType::Invoke:
    break;
  }
  events.push_back(std::move(inv));
  events.push_back(std::move(done));
}

auto parse_mode(std::string_view s) -> Mode {
  if (s == "guarded") return Mode::Guarded;
  if (s == "unguarded") return Mode::Unguarded;
  if (s == "blind") return Mode::Blind;
  throw std::runtime_error{std::format("unknown --config {}", s)};
}

auto parse_args(int argc, char **argv) -> RunOptions {
  RunOptions o;
  o.seed = std::random_device{}();
  auto have_config = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    auto next = [&]() -> std::string_view {
      if (i + 1 >= argc) throw std::runtime_error{std::format("{} needs a value", a)};
      return argv[++i];
    };
    if (a == "--config") {
      o.mode = parse_mode(next());
      have_config = true;
    } else if (a == "--seed") {
      o.seed = std::stoull(std::string{next()});
    } else if (a == "--threads") {
      o.threads = std::stoi(std::string{next()});
    } else if (a == "--txns") {
      o.txns = std::stoi(std::string{next()});
    } else if (a == "--dir") {
      o.dir = next();
    } else if (a == "--out") {
      o.out = next();
    } else if (a == "--no-vacuum") {
      o.no_vacuum = true;
    } else if (a == "--no-degrade") {
      o.no_degrade = true;
    } else {
      throw std::runtime_error{std::format("unknown argument {}", a)};
    }
  }
  if (!have_config || o.out.empty())
    throw std::runtime_error{"--config and --out are required"};
  if (o.threads < 1 || o.txns < 1)
    throw std::runtime_error{"--threads and --txns must be positive"};
  return o;
}

auto run(const RunOptions &o) -> int {
  auto cfg = config_for(o.seed);
  if (o.no_vacuum) cfg.vacuum = false;
  if (o.no_degrade) cfg.degrade = false;

  std::printf("isolation_history: config=%s seed=%llu threads=%d txns=%d "
              "backend=%s max_file_bytes=%llu sync%%=%d vacuum=%d degrade=%d\n",
              mode_name(o.mode).data(),
              static_cast<unsigned long long>(o.seed), o.threads, o.txns,
              backend_name(cfg.backend).data(),
              static_cast<unsigned long long>(cfg.max_file_bytes),
              cfg.sync_percent, cfg.vacuum ? 1 : 0, cfg.degrade ? 1 : 0);
  std::fflush(stdout);

  fs::remove_all(o.dir);
  auto db = bytecask::DB::open(o.dir, db_options(cfg));

  const auto start = Clock::now();
  Recorder rec{start};
  KeyPool keys;
  std::atomic<std::int64_t> next_element{1};
  std::atomic<int> remaining{o.txns};
  std::atomic<bool> stop{false};
  Totals totals;
  // One buffer per client, plus one for the degrade nemesis and one for the
  // final read.
  std::vector<std::vector<Event>> buffers(static_cast<std::size_t>(o.threads) + 2);

  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&db, &stop, &totals, seed = o.seed] {
      std::mt19937_64 vrng{seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        try {
          if (db.vacuum({.fragmentation_threshold = threshold}))
            totals.vacuums.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception &) {
          // Vacuum refuses to run on a degraded engine; the nemesis resumes it.
        }
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }

  // The degrade nemesis is its own Elle process: its transactions are real
  // appends, and the one whose fdatasync fails is :info, since resume() may
  // replay it.
  std::jthread nemesis_thread;
  if (cfg.degrade) {
    const auto process = o.threads;
    nemesis_thread = std::jthread{[&, process] {
      std::mt19937_64 nrng{o.seed + 2};
      auto &events = buffers[static_cast<std::size_t>(process)];
      while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(20 + nrng() % 180));
        if (stop.load(std::memory_order_relaxed)) break;
        // Retry until this thread holds the flush role for its own write.
        for (int attempt = 0; attempt < 50 && !db.is_degraded(); ++attempt) {
          auto mops = random_txn(nrng, keys, next_element);
          if (std::ranges::none_of(mops, &Mop::append)) continue;
          run_one(db, o.mode, process, rec, events, std::move(mops),
                  {.sync = true, .solo = true}, true, totals);
        }
        if (!db.is_degraded()) continue;
        totals.degrades.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(1 + nrng() % 20));
        for (;;) {
          try {
            db.resume();
            break;
          } catch (const std::exception &e) {
            std::fprintf(stderr, "resume failed, retrying: %s\n", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }
      }
    }};
  }

  {
    std::vector<std::jthread> clients;
    for (int p = 0; p < o.threads; ++p) {
      clients.emplace_back([&, p] {
        std::mt19937_64 crng{o.seed + 100 + static_cast<std::uint64_t>(p)};
        auto &events = buffers[static_cast<std::size_t>(p)];
        while (remaining.fetch_sub(1, std::memory_order_relaxed) > 0) {
          // A degraded engine refuses every write until the nemesis resumes
          // it; back off rather than fill the history with :info.
          while (db.is_degraded())
            std::this_thread::sleep_for(std::chrono::microseconds(200));
          const auto sync =
              static_cast<int>(crng() % 100) < cfg.sync_percent;
          run_one(db, o.mode, p, rec, events,
                  random_txn(crng, keys, next_element), {.sync = sync}, false,
                  totals);
        }
      });
    }
  }
  stop.store(true, std::memory_order_relaxed);
  if (nemesis_thread.joinable()) nemesis_thread.join();
  if (vacuum_thread.joinable()) vacuum_thread.join();
  if (db.is_degraded()) db.resume();

  // Final reads over every key, so an append lost after its last concurrent
  // read still shows up in the history.
  {
    const auto process = o.threads + 1;
    auto &events = buffers[static_cast<std::size_t>(process)];
    const auto key_count = keys.key_count();
    for (std::int64_t k0 = 0; k0 < key_count; k0 += 32) {
      std::vector<Mop> mops;
      for (auto k = k0; k < std::min(k0 + 32, key_count); ++k)
        mops.push_back({.append = false, .key = k, .element = 0, .read = {}});
      run_one(db, o.mode, process, rec, events, std::move(mops), {}, false,
              totals);
    }
  }

  std::vector<Event> all;
  for (auto &b : buffers) {
    std::ranges::move(b, std::back_inserter(all));
  }
  write_history(o.out, std::move(all));

  const auto secs =
      std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("  %.1fs: ok=%d fail=%d info=%d keys=%lld degrades=%d "
              "vacuums=%d -> %s\n",
              secs, totals.ok.load(), totals.fail.load(), totals.info.load(),
              static_cast<long long>(keys.key_count()), totals.degrades.load(),
              totals.vacuums.load(), o.out.c_str());
  return 0;
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    return run(parse_args(argc, argv));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "isolation_history: %s\n", e.what());
    return 2;
  }
}
