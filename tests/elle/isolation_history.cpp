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
//                     [--no-vacuum] [--force-vacuum] [--no-degrade]
//                     [--followers N] [--follower-readers N] [--lag]
//
// With --followers N (#178, docs/replication_checking_design.md) the leader
// gains N followers, each bootstrapped from a manifest under load and tailed
// through changes_since -> ingest, with reader threads of their own. Every
// operation carries a "node" (0 = leader); a session read carries "wait",
// the durable_sequence it waited for. Bootstraps and nemesis counts go to
// <out>.cluster.json.

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
#include <shared_mutex>
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
  int followers{0};        // 0: leader only (#94); N: a cluster (#178)
  int follower_readers{4}; // reader threads per follower
  bool lag{false};         // pause replication threads at random
  bool force_vacuum{false}; // leader vacuum on whatever the seed draws
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
  int node{0};               // 0 = leader, 1.. = follower
  std::uint64_t wait_seq{0}; // session read: the durable_sequence waited for
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
    line += std::format(R"({{"index":{},"process":{},"node":{},"type":"{}",)"
                        R"("f":"txn","time":{},"value":[)",
                        e.index, e.process, e.node, event_type_name(e.type),
                        e.time_ns);
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
    if (e.wait_seq != 0) line += std::format(R"(,"wait":{})", e.wait_seq);
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
// Where a transaction runs, for the history: node 0 is the leader. wait_seq
// is set on a follower's session read.
struct Placement {
  int process{0};
  int node{0};
  std::uint64_t wait_seq{0};
};

auto run_one(bytecask::DB &db, Mode mode, Placement at, Recorder &rec,
             std::vector<Event> &events, std::vector<Mop> mops,
             bytecask::WriteOptions wo, bool inject_sync, Totals &totals)
    -> std::uint64_t {
  Event inv;
  inv.process = at.process;
  inv.node = at.node;
  inv.wait_seq = at.wait_seq;
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
  done.process = at.process;
  done.node = at.node;
  done.wait_seq = at.wait_seq;
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
  return outcome.type == EventType::Ok ? outcome.sequence : 0;
}

// ---------------------------------------------------------------------------
// Cluster (#178): followers bootstrapped from the leader's manifest and fed
// by changes_since -> ingest. See docs/replication_checking_design.md.
// ---------------------------------------------------------------------------

// DB is neither copyable nor movable; the holder lets a follower be closed and
// reopened in place.
struct DbHolder {
  bytecask::DB db;
  DbHolder(const fs::path &dir, const bytecask::Options &opts)
      : db{bytecask::DB::open(dir, opts)} {}
};

// A follower. Readers and the replication thread hold gate shared while
// they use db; a restart takes it exclusively. glibc's rwlock prefers
// readers, so readers back off while restarting is set, or a restart would
// wait behind them forever.
struct FollowerNode {
  int node{0};
  fs::path dir;
  bytecask::Options opts;
  std::shared_mutex gate;
  std::atomic<bool> restarting{false};
  std::unique_ptr<DbHolder> holder; // null until bootstrapped
};

struct Bootstrap {
  int node{0};
  std::uint64_t through_sequence{0};
  std::uint64_t durable_after_open{0};
  std::int64_t keys_compared{0};
  std::int64_t mismatches{0};
};

struct ClusterStats {
  std::mutex mu;
  std::vector<Bootstrap> bootstraps; // under mu
  std::vector<std::string> errors;   // under mu
  std::atomic<int> ingests{0};
  std::atomic<int> ingest_errors{0};
  std::atomic<int> restarts{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> lag_pauses{0};
  std::atomic<int> follower_vacuums{0};
  std::atomic<int> session_reads{0};

  void error(std::string what) {
    std::lock_guard<std::mutex> g{mu};
    errors.push_back(std::move(what));
  }
};

// Owned copy of a changes_since entry: the iterator's views are valid only
// until it advances.
struct OwnedEntry {
  std::uint64_t sequence{0};
  bytecask::EntryType entry_type{bytecask::EntryType::Put};
  bytecask::Bytes key;
  bytecask::Bytes value;
};

auto ingest_owned(bytecask::DB &follower, const std::vector<OwnedEntry> &buf)
    -> void {
  std::vector<bytecask::DataEntryView> views;
  views.reserve(buf.size());
  for (const auto &e : buf) {
    views.push_back({e.sequence, e.entry_type, e.key, e.value});
  }
  follower.ingest(views);
}

// Phase 1 of the protocol, under load: manifest, copy, open as a follower.
// Leader vacuum is held off from the manifest to the end of the copy, as the
// protocol requires. The follower's state at open must equal the manifest
// snapshot's, key for key.
auto bootstrap(bytecask::DB &leader, FollowerNode &f, std::mutex &vacuum_gate,
               const KeyPool &keys, ClusterStats &stats) -> void {
  for (;;) {
    try {
      std::unique_lock<std::mutex> vl{vacuum_gate};
      auto m = leader.create_manifest();
      fs::remove_all(f.dir);
      fs::create_directories(f.dir);
      for (const auto &fi : m.files) {
        fs::copy_file(fi.data_path, f.dir / fi.data_path.filename());
        fs::copy_file(fi.hint_path, f.dir / fi.hint_path.filename());
      }
      vl.unlock();
      auto h = std::make_unique<DbHolder>(f.dir, f.opts);

      Bootstrap b{.node = f.node,
                  .through_sequence = m.through_sequence,
                  .durable_after_open = h->db.durable_sequence(),
                  .keys_compared = keys.key_count(),
                  .mismatches = 0};
      bytecask::Bytes lv;
      bytecask::Bytes fv;
      for (std::int64_t k = 0; k < b.keys_compared; ++k) {
        const auto kb = key_bytes(k);
        const auto lf = m.snap.get({}, as_view(kb), lv);
        const auto ff = h->db.get({}, as_view(kb), fv);
        if (lf != ff || (lf && lv != fv)) ++b.mismatches;
      }
      {
        std::lock_guard<std::mutex> g{stats.mu};
        stats.bootstraps.push_back(b);
      }
      std::unique_lock<std::shared_mutex> g{f.gate};
      f.holder = std::move(h);
      return;
    } catch (const std::exception &) {
      // create_manifest refuses a degraded leader; the nemesis resumes it.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

// Phase 2: wake on the leader's durable sequence, stream changes_since from
// the follower's durable_sequence(), ingest in slices cut at batch
// boundaries. Nemeses: lag pauses, duplicate delivery, follower vacuum and
// follower restarts.
auto replicate(bytecask::DB &leader, FollowerNode &f, bool lag,
               std::uint64_t seed, const std::atomic<bool> &stop,
               ClusterStats &stats) -> void {
  std::mt19937_64 rng{seed};
  // A run lasts a few seconds, so the nemeses fire every few hundred ms.
  auto next_restart =
      Clock::now() + std::chrono::milliseconds(100 + rng() % 400);
  while (!stop.load(std::memory_order_relaxed)) {
    if (lag && rng() % 10 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20 + rng() % 180));
      stats.lag_pauses.fetch_add(1, std::memory_order_relaxed);
    }
    if (Clock::now() >= next_restart) {
      f.restarting.store(true, std::memory_order_release);
      std::unique_lock<std::shared_mutex> g{f.gate};
      f.holder.reset();
      f.holder = std::make_unique<DbHolder>(f.dir, f.opts);
      f.restarting.store(false, std::memory_order_release);
      stats.restarts.fetch_add(1, std::memory_order_relaxed);
      next_restart =
          Clock::now() + std::chrono::milliseconds(100 + rng() % 400);
    }
    std::shared_lock<std::shared_mutex> g{f.gate};
    auto &fdb = f.holder->db;
    try {
      if (rng() % 50 == 0) {
        const auto threshold = static_cast<double>(rng() % 60) / 100.0;
        if (fdb.vacuum({.fragmentation_threshold = threshold}))
          stats.follower_vacuums.fetch_add(1, std::memory_order_relaxed);
      }
      auto from = fdb.durable_sequence();
      if (leader.durable_sequence(from + 1, std::chrono::milliseconds(20)) <=
          from)
        continue;
      if (rng() % 10 == 0 && from > 0) {
        from -= std::min<std::uint64_t>(from, 1 + rng() % 32);
        stats.duplicates.fetch_add(1, std::memory_order_relaxed);
      }
      auto snap = leader.snapshot();
      std::vector<OwnedEntry> buf;
      auto target = 1 + rng() % 64;
      auto in_batch = false;
      for (const auto &e : leader.changes_since(snap, from)) {
        buf.push_back({e.sequence, e.entry_type,
                       bytecask::Bytes{e.key.begin(), e.key.end()},
                       bytecask::Bytes{e.value.begin(), e.value.end()}});
        if (e.entry_type == bytecask::EntryType::BulkBegin) in_batch = true;
        if (e.entry_type == bytecask::EntryType::BulkEnd) in_batch = false;
        // Slices end at batch boundaries: ingest publishes a slice in one
        // step, and a slice cut inside a batch would publish part of it.
        if (!in_batch && buf.size() >= target) {
          ingest_owned(fdb, buf);
          stats.ingests.fetch_add(1, std::memory_order_relaxed);
          buf.clear();
          target = 1 + rng() % 64;
        }
      }
      if (in_batch) {
        stats.error(std::format(
            "node {}: changes_since ended inside a batch after {}", f.node,
            buf.empty() ? 0 : buf.back().sequence));
      } else if (!buf.empty()) {
        ingest_owned(fdb, buf);
        stats.ingests.fetch_add(1, std::memory_order_relaxed);
      }
    } catch (const std::exception &) {
      // Restart from the follower's durable_sequence(), as the protocol
      // says. A degraded leader or follower recovers through resume().
      stats.ingest_errors.fetch_add(1, std::memory_order_relaxed);
      if (fdb.is_degraded()) {
        try {
          fdb.resume();
        } catch (const std::exception &) {
        }
      }
    }
  }
}

auto store_max(std::atomic<std::uint64_t> &a, std::uint64_t v) -> void {
  auto cur = a.load(std::memory_order_relaxed);
  while (cur < v &&
         !a.compare_exchange_weak(cur, v, std::memory_order_acq_rel,
                                  std::memory_order_relaxed)) {
  }
}

auto write_cluster_summary(const fs::path &path, ClusterStats &stats)
    -> void {
  std::ofstream out{path};
  if (!out) throw std::runtime_error{"cannot open " + path.string()};
  std::lock_guard<std::mutex> g{stats.mu};
  out << "{\"bootstraps\":[";
  for (std::size_t i = 0; i < stats.bootstraps.size(); ++i) {
    const auto &b = stats.bootstraps[i];
    out << std::format(R"({}{{"node":{},"through_sequence":{},)"
                       R"("durable_after_open":{},"keys_compared":{},)"
                       R"("mismatches":{}}})",
                       i == 0 ? "" : ",", b.node, b.through_sequence,
                       b.durable_after_open, b.keys_compared, b.mismatches);
  }
  out << "],\"errors\":[";
  for (std::size_t i = 0; i < stats.errors.size(); ++i) {
    out << (i == 0 ? "" : ",") << '"' << json_escape(stats.errors[i]) << '"';
  }
  out << std::format(
      R"(],"ingests":{},"ingest_errors":{},"restarts":{},"duplicates":{},)"
      R"("lag_pauses":{},"follower_vacuums":{},"session_reads":{}}})",
      stats.ingests.load(), stats.ingest_errors.load(),
      stats.restarts.load(), stats.duplicates.load(),
      stats.lag_pauses.load(), stats.follower_vacuums.load(),
      stats.session_reads.load());
  out << "\n";
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
    } else if (a == "--followers") {
      o.followers = std::stoi(std::string{next()});
    } else if (a == "--follower-readers") {
      o.follower_readers = std::stoi(std::string{next()});
    } else if (a == "--lag") {
      o.lag = true;
    } else if (a == "--force-vacuum") {
      o.force_vacuum = true;
    } else {
      throw std::runtime_error{std::format("unknown argument {}", a)};
    }
  }
  if (!have_config || o.out.empty())
    throw std::runtime_error{"--config and --out are required"};
  if (o.threads < 1 || o.txns < 1)
    throw std::runtime_error{"--threads and --txns must be positive"};
  if (o.followers < 0 || o.follower_readers < 1)
    throw std::runtime_error{"--followers must be >= 0, --follower-readers >= 1"};
  return o;
}

auto run(const RunOptions &o) -> int {
  auto cfg = config_for(o.seed);
  if (o.no_vacuum) cfg.vacuum = false;
  if (o.force_vacuum) cfg.vacuum = true;
  if (o.no_degrade) cfg.degrade = false;

  std::printf("isolation_history: config=%s seed=%llu threads=%d txns=%d "
              "backend=%s max_file_bytes=%llu sync%%=%d vacuum=%d degrade=%d "
              "followers=%d lag=%d\n",
              mode_name(o.mode).data(),
              static_cast<unsigned long long>(o.seed), o.threads, o.txns,
              backend_name(cfg.backend).data(),
              static_cast<unsigned long long>(cfg.max_file_bytes),
              cfg.sync_percent, cfg.vacuum ? 1 : 0, cfg.degrade ? 1 : 0,
              o.followers, o.lag ? 1 : 0);
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
  // Processes: leader clients, the degrade nemesis, the leader's final read,
  // then per follower its readers and its final read. One buffer each.
  const auto leader_processes = o.threads + 2;
  const auto per_follower = o.follower_readers + 1;
  auto follower_process = [&](int node, int r) {
    return leader_processes + (node - 1) * per_follower + r;
  };
  std::vector<std::vector<Event>> buffers(
      static_cast<std::size_t>(leader_processes + o.followers * per_follower));
  // Highest sequence a leader client has had acknowledged; session reads
  // wait for it on a follower.
  std::atomic<std::uint64_t> last_acked{0};
  // Held by each vacuum call on the leader, and by a bootstrap from its
  // manifest to the end of the file copy.
  std::mutex vacuum_gate;

  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&db, &stop, &totals, &vacuum_gate,
                                  seed = o.seed] {
      std::mt19937_64 vrng{seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        try {
          std::lock_guard<std::mutex> g{vacuum_gate};
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
          store_max(last_acked,
                    run_one(db, o.mode, {.process = process}, rec, events,
                            std::move(mops), {.sync = true, .solo = true},
                            true, totals));
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

  // Followers: each is bootstrapped from a manifest while the leader is
  // under load, then tailed; readers run on it once it exists.
  ClusterStats cluster;
  std::vector<std::unique_ptr<FollowerNode>> followers;
  std::atomic<bool> repl_stop{false};
  std::atomic<bool> readers_stop{false};
  std::vector<std::jthread> repl_threads;
  std::vector<std::jthread> reader_threads;
  for (int n = 1; n <= o.followers; ++n) {
    auto f = std::make_unique<FollowerNode>();
    f->node = n;
    f->dir = o.dir.string() + std::format("-f{}", n);
    f->opts = db_options(cfg);
    f->opts.initial_mode = bytecask::Mode::Follower;
    followers.push_back(std::move(f));
  }
  for (auto &fp : followers) {
    auto &f = *fp;
    repl_threads.emplace_back([&, fptr = &f] {
      std::mt19937_64 brng{o.seed + 200 + static_cast<std::uint64_t>(fptr->node)};
      std::this_thread::sleep_for(std::chrono::milliseconds(20 + brng() % 200));
      bootstrap(db, *fptr, vacuum_gate, keys, cluster);
      replicate(db, *fptr, o.lag, brng(), repl_stop, cluster);
    });
    for (int r = 0; r < o.follower_readers; ++r) {
      reader_threads.emplace_back([&, fptr = &f, r] {
        const auto process = follower_process(fptr->node, r);
        std::mt19937_64 rrng{o.seed + 1000 + static_cast<std::uint64_t>(process)};
        auto &events = buffers[static_cast<std::size_t>(process)];
        while (!readers_stop.load(std::memory_order_relaxed)) {
          if (fptr->restarting.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
          }
          std::shared_lock<std::shared_mutex> g{fptr->gate};
          if (!fptr->holder) {
            g.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
          }
          auto &fdb = fptr->holder->db;
          std::vector<Mop> mops;
          const auto n = 1 + static_cast<int>(rrng() % 4);
          for (int i = 0; i < n; ++i)
            mops.push_back({.append = false, .key = keys.pick(rrng, false),
                            .element = 0, .read = {}});
          Placement at{.process = process, .node = fptr->node, .wait_seq = 0};
          if (rrng() % 4 == 0) {
            const auto seq = last_acked.load(std::memory_order_acquire);
            if (seq != 0 &&
                fdb.durable_sequence(seq, std::chrono::milliseconds(200)) >=
                    seq) {
              at.wait_seq = seq;
              cluster.session_reads.fetch_add(1, std::memory_order_relaxed);
            }
          }
          run_one(fdb, Mode::Guarded, at, rec, events, std::move(mops), {},
                  false, totals);
          g.unlock();
          // Think time: reads are cheap, and unthrottled readers would
          // dominate the history Elle has to check.
          std::this_thread::sleep_for(
              std::chrono::microseconds(500 + rrng() % 1500));
        }
      });
    }
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
          store_max(last_acked,
                    run_one(db, o.mode, {.process = p}, rec, events,
                            random_txn(crng, keys, next_element),
                            {.sync = sync}, false, totals));
        }
      });
    }
  }
  stop.store(true, std::memory_order_relaxed);
  if (nemesis_thread.joinable()) nemesis_thread.join();
  if (vacuum_thread.joinable()) vacuum_thread.join();
  if (db.is_degraded()) db.resume();

  // Convergence: a sync write makes every leader entry durable, so
  // changes_since can deliver all of it; every follower must then reach the
  // leader's durable sequence.
  if (!followers.empty()) {
    db.put({.sync = true}, as_view("fence"), as_view(""));
    const auto target = db.durable_sequence();
    for (auto &fp : followers) {
      const auto deadline = Clock::now() + std::chrono::seconds(20);
      for (;;) {
        {
          std::shared_lock<std::shared_mutex> g{fp->gate};
          if (fp->holder && fp->holder->db.durable_sequence() >= target) break;
        }
        if (Clock::now() >= deadline) {
          std::shared_lock<std::shared_mutex> g{fp->gate};
          std::string detail = "not bootstrapped";
          if (fp->holder) {
            auto &fdb = fp->holder->db;
            const auto from = fdb.durable_sequence();
            auto snap = db.snapshot();
            std::uint64_t n = 0;
            std::uint64_t first = 0;
            for (const auto &e : db.changes_since(snap, from)) {
              if (n++ == 0) first = e.sequence;
            }
            detail = std::format(
                "follower durable {} degraded {} ({}); leader durable {}; "
                "changes_since yields {} entries from {}",
                from, fdb.is_degraded(), fdb.degraded_reason(),
                db.durable_sequence(), n, first);
          }
          cluster.error(std::format("node {}: did not reach durable sequence "
                                    "{} within 20s: {}",
                                    fp->node, target, detail));
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    readers_stop.store(true, std::memory_order_relaxed);
    reader_threads.clear();
    repl_stop.store(true, std::memory_order_relaxed);
    repl_threads.clear();
  }

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
      run_one(db, o.mode, {.process = process}, rec, events, std::move(mops),
              {}, false, totals);
    }
  }
  for (auto &fp : followers) {
    if (!fp->holder) continue;
    const auto process = follower_process(fp->node, o.follower_readers);
    auto &events = buffers[static_cast<std::size_t>(process)];
    const auto key_count = keys.key_count();
    for (std::int64_t k0 = 0; k0 < key_count; k0 += 32) {
      std::vector<Mop> mops;
      for (auto k = k0; k < std::min(k0 + 32, key_count); ++k)
        mops.push_back({.append = false, .key = k, .element = 0, .read = {}});
      run_one(fp->holder->db, Mode::Guarded,
              {.process = process, .node = fp->node, .wait_seq = 0}, rec,
              events, std::move(mops), {}, false, totals);
    }
  }

  std::vector<Event> all;
  for (auto &b : buffers) {
    std::ranges::move(b, std::back_inserter(all));
  }
  write_history(o.out, std::move(all));
  if (!followers.empty()) {
    auto summary = o.out;
    summary += ".cluster.json";
    write_cluster_summary(summary, cluster);
    std::printf("  cluster: bootstraps=%zu ingests=%d ingest_errors=%d "
                "restarts=%d duplicates=%d lag_pauses=%d follower_vacuums=%d "
                "session_reads=%d errors=%zu\n",
                cluster.bootstraps.size(), cluster.ingests.load(),
                cluster.ingest_errors.load(), cluster.restarts.load(),
                cluster.duplicates.load(), cluster.lag_pauses.load(),
                cluster.follower_vacuums.load(), cluster.session_reads.load(),
                cluster.errors.size());
    followers.clear();
    for (int n = 1; n <= o.followers; ++n)
      fs::remove_all(o.dir.string() + std::format("-f{}", n));
  }

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
