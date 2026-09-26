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
  bool topology{false};     // planned transfers, promotions, re-bootstraps
  bool promote_least{false}; // unplanned promotion takes the least advanced
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

// Who ran an operation: a client writing to the leader, a reader of one
// node, or the final read of every key on one node.
enum class Role : std::uint8_t { Leader, Reader, Final };

auto role_name(Role r) -> std::string_view {
  switch (r) {
  case Role::Leader:
    return "leader";
  case Role::Reader:
    return "reader";
  case Role::Final:
    return "final";
  }
  return "?";
}

struct Event {
  std::uint64_t index{0};
  int process{0};
  int node{0};               // the node the operation ran on
  int epoch{0};              // cluster epoch when it was invoked
  Role role{Role::Leader};
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
    line += std::format(R"({{"index":{},"process":{},"node":{},"epoch":{},)"
                        R"("role":"{}","type":"{}","f":"txn","time":{},)"
                        R"("value":[)",
                        e.index, e.process, e.node, e.epoch, role_name(e.role),
                        event_type_name(e.type), e.time_ns);
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
// Where a transaction runs, for the history. wait_seq is set on a session
// read.
struct Placement {
  int process{0};
  int node{0};
  int epoch{0};
  Role role{Role::Leader};
  std::uint64_t wait_seq{0};
};

auto run_one(bytecask::DB &db, Mode mode, Placement at, Recorder &rec,
             std::vector<Event> &events, std::vector<Mop> mops,
             bytecask::WriteOptions wo, bool inject_sync, Totals &totals)
    -> std::uint64_t {
  Event inv;
  inv.process = at.process;
  inv.node = at.node;
  inv.epoch = at.epoch;
  inv.role = at.role;
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
  } catch (const bytecask::DbFollowerMode &e) {
    // Refused at admission, before anything is appended: the node was
    // demoted under the client. If the write ever shows up, it is G1a.
    outcome = {.type = EventType::Fail, .sequence = 0, .error = e.what()};
  } catch (const std::exception &e) {
    outcome = {.type = EventType::Info, .sequence = 0, .error = e.what()};
  }

  Event done;
  done.process = at.process;
  done.node = at.node;
  done.epoch = at.epoch;
  done.role = at.role;
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
// Cluster (#178): nodes bootstrapped from the leader's manifest and fed by
// changes_since -> ingest, and with --topology, planned transfers, unplanned
// promotions, re-targeting and re-bootstrap. See
// docs/replication_checking_design.md.
// ---------------------------------------------------------------------------

// DB is neither copyable nor movable; the holder lets a node be closed and
// reopened in place.
struct DbHolder {
  bytecask::DB db;
  DbHolder(const fs::path &dir, const bytecask::Options &opts)
      : db{bytecask::DB::open(dir, opts)} {}
};

// One node. Everything that uses db holds gate shared; a restart, promotion
// or re-bootstrap takes it exclusively through NodeExclusive. glibc's rwlock
// prefers readers, so users back off while exclusive_wanted is set, or the
// exclusive holder would wait behind them forever.
struct Node {
  int id{0};
  fs::path dir;
  bytecask::Options opts; // follower options; node 0 first opens as leader
  std::shared_mutex gate;
  // NodeExclusive holders and waiters. A count, not a flag: a holder that
  // cleared a flag on release while another was still waiting let readers
  // back in, and glibc's reader-preferring rwlock then starved the waiter
  // for good, since the readers were waiting on data only it would ingest.
  std::atomic<int> exclusive_wanted{0};
  std::unique_ptr<DbHolder> holder; // null while not bootstrapped
  // Where replicate() last gave up on an iteration, for the stuck report.
  std::atomic<int> repl_stall{0};
};

// A shared lock on the node's gate if it has a DB and nobody is waiting for
// it exclusively; an empty lock otherwise.
auto enter(Node &n) -> std::shared_lock<std::shared_mutex> {
  if (n.exclusive_wanted.load(std::memory_order_acquire) > 0) return {};
  std::shared_lock<std::shared_mutex> g{n.gate};
  if (!n.holder) return {};
  return g;
}

class NodeExclusive {
public:
  explicit NodeExclusive(Node &n) : n_{n} {
    n_.exclusive_wanted.fetch_add(1, std::memory_order_acq_rel);
    lk_ = std::unique_lock<std::shared_mutex>{n_.gate};
  }
  ~NodeExclusive() {
    lk_.unlock();
    n_.exclusive_wanted.fetch_sub(1, std::memory_order_acq_rel);
  }
  NodeExclusive(const NodeExclusive &) = delete;
  auto operator=(const NodeExclusive &) -> NodeExclusive & = delete;

private:
  Node &n_;
  std::unique_lock<std::shared_mutex> lk_;
};

// Who leads, who tails whom, and which nodes serve reads. Changed only by
// the orchestrator (and bootstraps); a node's own entry changes only while
// the orchestrator holds that node exclusively.
struct View {
  int leader{0};
  int epoch{0};
  std::vector<int> source;   // node each node tails; -1 for none
  std::vector<bool> serving; // bootstrapped and not abandoned
};

class ClusterView {
public:
  explicit ClusterView(int nodes) {
    v_.source.assign(static_cast<std::size_t>(nodes), -1);
    v_.serving.assign(static_cast<std::size_t>(nodes), false);
    v_.serving[0] = true;
  }
  [[nodiscard]] auto get() const -> View {
    std::shared_lock<std::shared_mutex> g{mu_};
    return v_;
  }
  template <typename F> void update(F f) {
    std::unique_lock<std::shared_mutex> g{mu_};
    f(v_);
  }

private:
  mutable std::shared_mutex mu_;
  View v_;
};

auto at(std::vector<int> &v, int i) -> int & {
  return v[static_cast<std::size_t>(i)];
}

struct Bootstrap {
  int node{0};
  int source{0};
  std::uint64_t through_sequence{0};
  std::uint64_t durable_after_open{0};
  std::int64_t keys_compared{0};
  std::int64_t mismatches{0};
};

// A topology event, for the summary. kind is "planned", "unplanned" or
// "retarget"; durable is the promoted node's durable_sequence() when it
// stopped tailing (for a retarget, the retargeted node's).
struct TopologyEvent {
  std::string kind;
  int epoch{0};
  int from{0};
  int to{0};
  std::uint64_t durable{0};
  std::uint64_t other_durable{0}; // planned: the old leader's; retarget: the source's
};

struct ClusterStats {
  std::mutex mu;
  std::vector<Bootstrap> bootstraps;  // under mu
  std::vector<TopologyEvent> events;  // under mu
  std::vector<std::string> errors;    // under mu
  std::atomic<int> ingests{0};
  std::atomic<int> ingest_errors{0};
  std::atomic<int> restarts{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> lag_pauses{0};
  std::atomic<int> follower_vacuums{0};
  // Followers that re-bootstrapped because their source's vacuum had
  // dropped history they had not yet received.
  std::atomic<int> history_rebootstraps{0};
  std::atomic<int> session_reads{0};

  void error(std::string what) {
    std::lock_guard<std::mutex> g{mu};
    errors.push_back(std::move(what));
  }
  void event(TopologyEvent e) {
    std::lock_guard<std::mutex> g{mu};
    events.push_back(std::move(e));
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

// Phase 1 of the protocol, under load: manifest from source, copy, open n as
// a follower. The source's vacuum is held off from the manifest to the end
// of the copy, as the protocol requires. The opened node must equal the
// manifest's snapshot, key for key. Retries until the source can produce a
// manifest (a degraded or demoted source cannot).
auto bootstrap(Node &source, Node &n, std::mutex &vacuum_gate,
               const KeyPool &keys, ClusterStats &stats) -> void {
  for (;;) {
    try {
      std::unique_ptr<DbHolder> h;
      Bootstrap b{.node = n.id, .source = source.id};
      {
        auto sg = enter(source);
        if (!sg.owns_lock()) throw std::runtime_error{"source unavailable"};
        std::unique_lock<std::mutex> vl{vacuum_gate};
        auto m = source.holder->db.create_manifest();
        fs::remove_all(n.dir);
        fs::create_directories(n.dir);
        for (const auto &fi : m.files) {
          fs::copy_file(fi.data_path, n.dir / fi.data_path.filename());
          fs::copy_file(fi.hint_path, n.dir / fi.hint_path.filename());
        }
        vl.unlock();
        h = std::make_unique<DbHolder>(n.dir, n.opts);
        b.through_sequence = m.through_sequence;
        b.durable_after_open = h->db.durable_sequence();
        b.keys_compared = keys.key_count();
        bytecask::Bytes lv;
        bytecask::Bytes fv;
        for (std::int64_t k = 0; k < b.keys_compared; ++k) {
          const auto kb = key_bytes(k);
          const auto lf = m.snap.get({}, as_view(kb), lv);
          const auto ff = h->db.get({}, as_view(kb), fv);
          if (lf != ff || (lf && lv != fv)) ++b.mismatches;
        }
      }
      {
        std::lock_guard<std::mutex> g{stats.mu};
        stats.bootstraps.push_back(b);
      }
      NodeExclusive x{n};
      n.holder = std::move(h);
      return;
    } catch (const std::exception &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

// Phase 2 for node n, from whichever node the view says it tails: wake on
// the source's durable sequence, stream changes_since from n's
// durable_sequence(), ingest in slices cut at batch boundaries. Nemeses:
// lag pauses, duplicate delivery, vacuum on n and restarts of n. The view
// is read with n's gate held, so a promotion (which changes n's source
// while holding n exclusively) never races an ingest into n.
auto replicate(std::vector<std::unique_ptr<Node>> &nodes, Node &n,
               const ClusterView &view, bool lag, std::uint64_t seed,
               const std::atomic<bool> &stop, std::mutex &vacuum_gate,
               const KeyPool &keys, ClusterStats &stats) -> void {
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
      next_restart =
          Clock::now() + std::chrono::milliseconds(100 + rng() % 400);
      // Checked under the exclusive hold: a promotion also holds the node
      // exclusively, so the node cannot become leader between the check and
      // the reopen (which opens it as a follower).
      NodeExclusive x{n};
      const auto v = view.get();
      if (v.leader != n.id && v.serving[static_cast<std::size_t>(n.id)]) {
        if (n.holder) {
          n.holder.reset();
          n.holder = std::make_unique<DbHolder>(n.dir, n.opts);
          stats.restarts.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    auto g = enter(n);
    if (!g.owns_lock()) {
      n.repl_stall.store(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    auto v = view.get();
    const auto src = at(v.source, n.id);
    if (src < 0) {
      n.repl_stall.store(2, std::memory_order_relaxed);
      g.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    auto &sn = *nodes[static_cast<std::size_t>(src)];
    auto sg = enter(sn);
    if (!sg.owns_lock()) {
      n.repl_stall.store(3, std::memory_order_relaxed);
      continue;
    }
    auto &fdb = n.holder->db;
    auto &leader = sn.holder->db;
    auto behind_history = false;
    try {
      if (rng() % 50 == 0) {
        const auto threshold = static_cast<double>(rng() % 60) / 100.0;
        if (fdb.vacuum({.fragmentation_threshold = threshold}))
          stats.follower_vacuums.fetch_add(1, std::memory_order_relaxed);
      }
      auto from = fdb.durable_sequence();
      if (leader.durable_sequence(from + 1, std::chrono::milliseconds(20)) <=
          from) {
        n.repl_stall.store(4, std::memory_order_relaxed);
        continue;
      }
      n.repl_stall.store(5, std::memory_order_relaxed);
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
        // step, and a slice cut inside a batch would publish part of it
        // (CONTRACT.md, ingest).
        if (!in_batch && buf.size() >= target) {
          ingest_owned(fdb, buf);
          stats.ingests.fetch_add(1, std::memory_order_relaxed);
          buf.clear();
          target = 1 + rng() % 64;
        }
      }
      if (in_batch) {
        stats.error(std::format(
            "node {}: changes_since from node {} ended inside a batch after {}",
            n.id, src, buf.empty() ? 0 : buf.back().sequence));
      } else if (!buf.empty()) {
        ingest_owned(fdb, buf);
        stats.ingests.fetch_add(1, std::memory_order_relaxed);
      }
    } catch (const bytecask::DbInvalidSequence &) {
      // The source's vacuum dropped history n still needs (#168). A
      // duplicate-delivery rewind can land below the source's
      // min_resumable_sequence() too; that one just retries from where n is.
      behind_history =
          fdb.durable_sequence() < leader.min_resumable_sequence();
    } catch (const std::exception &) {
      // Restart from n's durable_sequence(), as the protocol says. A
      // degraded node recovers through resume().
      stats.ingest_errors.fetch_add(1, std::memory_order_relaxed);
      if (fdb.is_degraded()) {
        try {
          fdb.resume();
        } catch (const std::exception &) {
        }
      }
    }
    if (behind_history) {
      // Resuming is refused, so n starts over from a manifest.
      sg.unlock();
      g.unlock();
      stats.history_rebootstraps.fetch_add(1, std::memory_order_relaxed);
      {
        NodeExclusive x{n};
        n.holder.reset();
      }
      bootstrap(sn, n, vacuum_gate, keys, stats);
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

auto durable_of(Node &n) -> std::optional<std::uint64_t> {
  auto g = enter(n);
  if (!g.owns_lock()) return std::nullopt;
  return n.holder->db.durable_sequence();
}

// Planned transfer (replication_primitives_design.md, Leadership Transfer):
// stop writes on the leader, wait for the target to catch up, promote it,
// re-target everyone else, the old leader included.
auto planned_transfer(std::vector<std::unique_ptr<Node>> &nodes,
                      ClusterView &view, int target, ClusterStats &stats)
    -> void {
  const auto old = view.get().leader;
  auto &on = *nodes[static_cast<std::size_t>(old)];
  auto &tn = *nodes[static_cast<std::size_t>(target)];
  // Read under the same hold as the mode switch: a separate durable_of()
  // can find the gate taken and read 0, and the wait below then promotes a
  // target that has not caught up, losing the old leader's tail.
  std::uint64_t old_durable = 0;
  {
    auto g = enter(on);
    if (!g.owns_lock()) return;
    on.holder->db.set_mode(bytecask::Mode::Follower);
    old_durable = on.holder->db.durable_sequence();
  }
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (durable_of(tn).value_or(0) < old_durable) {
    if (Clock::now() >= deadline) {
      stats.error(std::format("planned transfer {} -> {}: target stuck at {} "
                              "below the old leader's {}",
                              old, target, durable_of(tn).value_or(0),
                              old_durable));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  NodeExclusive x{tn};
  const auto target_durable = tn.holder->db.durable_sequence();
  view.update([&](View &v) { at(v.source, target) = -1; });
  tn.holder->db.set_mode(bytecask::Mode::Leader);
  int epoch = 0;
  view.update([&](View &v) {
    v.leader = target;
    epoch = ++v.epoch;
    for (std::size_t i = 0; i < v.source.size(); ++i) {
      if (static_cast<int>(i) != target && v.serving[i])
        v.source[i] = target;
    }
  });
  stats.event({.kind = "planned", .epoch = epoch, .from = old, .to = target,
               .durable = target_durable, .other_durable = old_durable});
}

// Unplanned promotion (Follower Promotion). The old leader is taken as
// lost: every follower is cut off from it, it refuses writes from then on,
// and any ingest in flight is let finish; then the most advanced follower
// is promoted where it stands, so no follower holds an entry the new
// leader lacks. The old leader is abandoned and re-bootstrapped later;
// the writes it acknowledged above the promoted node's durable sequence
// are lost. promote_least takes the least advanced follower instead, the
// protocol with the rule inverted: a follower ahead of the target forks.
auto unplanned_promotion(std::vector<std::unique_ptr<Node>> &nodes,
                         ClusterView &view, bool promote_least,
                         ClusterStats &stats) -> void {
  const auto old = view.get().leader;
  auto &on = *nodes[static_cast<std::size_t>(old)];
  view.update([&](View &v) {
    v.serving[static_cast<std::size_t>(old)] = false;
    std::ranges::fill(v.source, -1);
  });
  // The old leader stops taking writes now, as a crashed one would. What it
  // acknowledged beyond what reached a follower is lost.
  {
    auto g = enter(on);
    if (g.owns_lock()) on.holder->db.set_mode(bytecask::Mode::Follower);
  }
  // replicate() reads the view under the node's gate: once the gate has
  // been held exclusively, no ingest from the old leader is left running.
  std::vector<int> candidates;
  std::vector<std::uint64_t> durables;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto id = static_cast<int>(i);
    if (id == old) continue;
    NodeExclusive x{*nodes[i]};
    if (!nodes[i]->holder) continue;
    candidates.push_back(id);
    durables.push_back(nodes[i]->holder->db.durable_sequence());
  }
  if (candidates.empty()) {
    // Nothing to promote: the old leader is still the only one up.
    {
      auto g = enter(on);
      if (g.owns_lock()) on.holder->db.set_mode(bytecask::Mode::Leader);
    }
    view.update([&](View &v) {
      v.serving[static_cast<std::size_t>(old)] = true;
      for (std::size_t i = 0; i < v.source.size(); ++i) {
        if (static_cast<int>(i) != old && v.serving[i]) v.source[i] = old;
      }
    });
    return;
  }
  std::size_t pick = 0;
  for (std::size_t i = 1; i < candidates.size(); ++i) {
    if (promote_least ? durables[i] < durables[pick]
                      : durables[i] > durables[pick])
      pick = i;
  }
  const auto target = candidates[pick];
  auto &tn = *nodes[static_cast<std::size_t>(target)];
  int epoch = 0;
  std::uint64_t promoted_at = 0;
  {
    NodeExclusive x{tn};
    promoted_at = tn.holder->db.durable_sequence();
    tn.holder->db.set_mode(bytecask::Mode::Leader);
    view.update([&](View &v) {
      v.leader = target;
      epoch = ++v.epoch;
      for (std::size_t i = 0; i < v.source.size(); ++i) {
        if (static_cast<int>(i) != target && v.serving[i])
          v.source[i] = target;
      }
    });
  }
  stats.event({.kind = "unplanned", .epoch = epoch, .from = old, .to = target,
               .durable = promoted_at, .other_durable = 0});
  const auto v = view.get();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto id = static_cast<int>(i);
    if (id == target || !v.serving[i]) continue;
    stats.event({.kind = "retarget", .epoch = epoch, .from = target, .to = id,
                 .durable = durable_of(*nodes[i]).value_or(0),
                 .other_durable = promoted_at});
  }
}

auto write_cluster_summary(const fs::path &path, ClusterStats &stats,
                           int final_leader) -> void {
  std::ofstream out{path};
  if (!out) throw std::runtime_error{"cannot open " + path.string()};
  std::lock_guard<std::mutex> g{stats.mu};
  out << "{\"bootstraps\":[";
  for (std::size_t i = 0; i < stats.bootstraps.size(); ++i) {
    const auto &b = stats.bootstraps[i];
    out << std::format(R"({}{{"node":{},"source":{},"through_sequence":{},)"
                       R"("durable_after_open":{},"keys_compared":{},)"
                       R"("mismatches":{}}})",
                       i == 0 ? "" : ",", b.node, b.source, b.through_sequence,
                       b.durable_after_open, b.keys_compared, b.mismatches);
  }
  out << "],\"events\":[";
  for (std::size_t i = 0; i < stats.events.size(); ++i) {
    const auto &e = stats.events[i];
    out << std::format(R"({}{{"kind":"{}","epoch":{},"from":{},"to":{},)"
                       R"("durable":{},"other_durable":{}}})",
                       i == 0 ? "" : ",", e.kind, e.epoch, e.from, e.to,
                       e.durable, e.other_durable);
  }
  out << "],\"errors\":[";
  for (std::size_t i = 0; i < stats.errors.size(); ++i) {
    out << (i == 0 ? "" : ",") << '"' << json_escape(stats.errors[i]) << '"';
  }
  out << std::format(
      R"(],"final_leader":{},"ingests":{},"ingest_errors":{},"restarts":{},)"
      R"("duplicates":{},"lag_pauses":{},"follower_vacuums":{},)"
      R"("session_reads":{},"history_rebootstraps":{}}})",
      final_leader, stats.ingests.load(), stats.ingest_errors.load(),
      stats.restarts.load(), stats.duplicates.load(),
      stats.lag_pauses.load(), stats.follower_vacuums.load(),
      stats.session_reads.load(), stats.history_rebootstraps.load());
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
    } else if (a == "--topology") {
      o.topology = true;
    } else if (a == "--promote-least") {
      o.promote_least = true;
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
  if (o.no_degrade || o.topology) cfg.degrade = false;

  std::printf("isolation_history: config=%s seed=%llu threads=%d txns=%d "
              "backend=%s max_file_bytes=%llu sync%%=%d vacuum=%d degrade=%d "
              "followers=%d lag=%d topology=%d promote_least=%d\n",
              mode_name(o.mode).data(),
              static_cast<unsigned long long>(o.seed), o.threads, o.txns,
              backend_name(cfg.backend).data(),
              static_cast<unsigned long long>(cfg.max_file_bytes),
              cfg.sync_percent, cfg.vacuum ? 1 : 0, cfg.degrade ? 1 : 0,
              o.followers, o.lag ? 1 : 0, o.topology ? 1 : 0,
              o.promote_least ? 1 : 0);
  std::fflush(stdout);

  // Node 0 opens as the leader; every other node is bootstrapped from the
  // leader as a follower.
  const auto node_count = o.followers + 1;
  std::vector<std::unique_ptr<Node>> nodes;
  for (int i = 0; i < node_count; ++i) {
    auto n = std::make_unique<Node>();
    n->id = i;
    n->dir = i == 0 ? o.dir : fs::path{o.dir.string() + std::format("-n{}", i)};
    n->opts = db_options(cfg);
    n->opts.initial_mode = bytecask::Mode::Follower;
    fs::remove_all(n->dir);
    nodes.push_back(std::move(n));
  }
  {
    auto leader_opts = db_options(cfg);
    nodes[0]->holder = std::make_unique<DbHolder>(nodes[0]->dir, leader_opts);
  }
  ClusterView view{node_count};
  auto node = [&](int id) -> Node & {
    return *nodes[static_cast<std::size_t>(id)];
  };

  const auto start = Clock::now();
  Recorder rec{start};
  KeyPool keys;
  std::atomic<std::int64_t> next_element{1};
  std::atomic<int> remaining{o.txns};
  std::atomic<bool> stop{false};
  Totals totals;
  // Processes: the leader clients, the degrade nemesis, then per node its
  // readers and its final read. One buffer each.
  const auto per_node = o.follower_readers + 1;
  auto node_process = [&](int id, int r) {
    return o.threads + 1 + id * per_node + r;
  };
  std::vector<std::vector<Event>> buffers(
      static_cast<std::size_t>(o.threads + 1 + node_count * per_node));
  // Highest sequence a leader client has had acknowledged; session reads
  // wait for it on their node.
  std::atomic<std::uint64_t> last_acked{0};
  // Held by each vacuum call on the leader, and by a bootstrap from its
  // manifest to the end of the file copy.
  std::mutex vacuum_gate;
  ClusterStats cluster;

  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&] {
      std::mt19937_64 vrng{o.seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        auto &ln = node(view.get().leader);
        try {
          auto g = enter(ln);
          if (g.owns_lock()) {
            std::lock_guard<std::mutex> vg{vacuum_gate};
            if (ln.holder->db.vacuum({.fragmentation_threshold = threshold}))
              totals.vacuums.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (const std::exception &) {
          // Vacuum refuses to run on a degraded engine; the nemesis resumes it.
        }
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }

  // The degrade nemesis is its own Elle process: its transactions are real
  // appends, and the one whose fdatasync fails is :info, since resume() may
  // replay it. The leader never moves while it runs (no --topology).
  std::jthread nemesis_thread;
  if (cfg.degrade) {
    const auto process = o.threads;
    nemesis_thread = std::jthread{[&, process] {
      std::mt19937_64 nrng{o.seed + 2};
      auto &events = buffers[static_cast<std::size_t>(process)];
      auto &db = node(0).holder->db;
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

  // Replication: every node has a thread that tails whatever the view says
  // it tails (nothing while it leads). Followers are first bootstrapped from
  // the leader while it is under load.
  std::atomic<bool> repl_stop{false};
  std::atomic<bool> readers_stop{false};
  std::vector<std::jthread> repl_threads;
  std::vector<std::jthread> reader_threads;
  std::jthread orchestrator;
  // Destroyed before the threads above are joined: if anything below throws,
  // every loop is told to stop, so the exception surfaces instead of the
  // unwind waiting on threads that never finish.
  struct StopAll {
    std::atomic<bool> &a, &b, &c;
    ~StopAll() {
      a.store(true);
      b.store(true);
      c.store(true);
    }
  } stop_all{stop, repl_stop, readers_stop};
  if (o.followers > 0) {
    for (int i = 0; i < node_count; ++i) {
      repl_threads.emplace_back([&, i] {
        std::mt19937_64 brng{o.seed + 200 + static_cast<std::uint64_t>(i)};
        if (i != 0) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(20 + brng() % 200));
          const auto leader = view.get().leader;
          bootstrap(node(leader), node(i), vacuum_gate, keys, cluster);
          view.update([&](View &v) {
            v.serving[static_cast<std::size_t>(i)] = true;
            at(v.source, i) = v.leader;
          });
        }
        replicate(nodes, node(i), view, o.lag, brng(), repl_stop, vacuum_gate,
                  keys, cluster);
      });
      for (int r = 0; r < o.follower_readers; ++r) {
        reader_threads.emplace_back([&, i, r] {
          const auto process = node_process(i, r);
          std::mt19937_64 rrng{o.seed + 1000 +
                               static_cast<std::uint64_t>(process)};
          auto &events = buffers[static_cast<std::size_t>(process)];
          auto &n = node(i);
          while (!readers_stop.load(std::memory_order_relaxed)) {
            const auto v = view.get();
            auto g = v.serving[static_cast<std::size_t>(i)]
                         ? enter(n)
                         : std::shared_lock<std::shared_mutex>{};
            if (!g.owns_lock()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
              continue;
            }
            auto &db = n.holder->db;
            std::vector<Mop> mops;
            const auto k = 1 + static_cast<int>(rrng() % 4);
            for (int j = 0; j < k; ++j)
              mops.push_back({.append = false, .key = keys.pick(rrng, false),
                              .element = 0, .read = {}});
            Placement pl{.process = process, .node = i, .epoch = v.epoch,
                         .role = Role::Reader, .wait_seq = 0};
            if (rrng() % 4 == 0) {
              const auto seq = last_acked.load(std::memory_order_acquire);
              if (seq != 0 &&
                  db.durable_sequence(seq, std::chrono::milliseconds(200)) >=
                      seq) {
                pl.wait_seq = seq;
                cluster.session_reads.fetch_add(1, std::memory_order_relaxed);
              }
            }
            run_one(db, Mode::Guarded, pl, rec, events, std::move(mops), {},
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
  }

  // Topology: once every follower is up, planned transfers to a random
  // node and unplanned promotions of the most advanced one, and
  // re-bootstrap of any node an unplanned promotion abandoned.
  auto rebootstrap_abandoned = [&] {
    const auto v = view.get();
    for (int i = 0; i < node_count; ++i) {
      if (i == v.leader || v.serving[static_cast<std::size_t>(i)]) continue;
      {
        NodeExclusive x{node(i)};
        node(i).holder.reset();
      }
      bootstrap(node(v.leader), node(i), vacuum_gate, keys, cluster);
      view.update([&](View &w) {
        w.serving[static_cast<std::size_t>(i)] = true;
        at(w.source, i) = w.leader;
      });
    }
  };
  if (o.topology && o.followers > 0) {
    orchestrator = std::jthread{[&] {
      std::mt19937_64 orng{o.seed + 3};
      const auto all_up = [&] {
        const auto v = view.get();
        return std::ranges::all_of(v.serving, [](bool b) { return b; });
      };
      while (!all_up() && !stop.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100 + orng() % 300));
        if (stop.load(std::memory_order_relaxed)) break;
        if (!all_up()) {
          rebootstrap_abandoned();
          continue;
        }
        if (orng() % 2 == 0) {
          const auto v = view.get();
          std::vector<int> candidates;
          for (int i = 0; i < node_count; ++i)
            if (i != v.leader) candidates.push_back(i);
          const auto target = candidates[orng() % candidates.size()];
          planned_transfer(nodes, view, target, cluster);
        } else {
          unplanned_promotion(nodes, view, o.promote_least, cluster);
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
          for (;;) {
            const auto v = view.get();
            auto &ln = node(v.leader);
            auto g = enter(ln);
            // A degraded or demoted leader refuses every write until the
            // nemesis resumes it or the view moves on; back off rather than
            // fill the history.
            if (!g.owns_lock() || ln.holder->db.is_degraded() ||
                ln.holder->db.mode() != bytecask::Mode::Leader) {
              if (g.owns_lock()) g.unlock();
              std::this_thread::sleep_for(std::chrono::microseconds(200));
              continue;
            }
            const auto sync =
                static_cast<int>(crng() % 100) < cfg.sync_percent;
            store_max(last_acked,
                      run_one(ln.holder->db, o.mode,
                              {.process = p, .node = v.leader,
                               .epoch = v.epoch, .role = Role::Leader,
                               .wait_seq = 0},
                              rec, events, random_txn(crng, keys, next_element),
                              {.sync = sync}, false, totals));
            break;
          }
        }
      });
    }
  }
  stop.store(true, std::memory_order_relaxed);
  if (orchestrator.joinable()) orchestrator.join();
  if (nemesis_thread.joinable()) nemesis_thread.join();
  if (vacuum_thread.joinable()) vacuum_thread.join();
  if (o.topology && o.followers > 0) rebootstrap_abandoned();
  const auto final_leader = view.get().leader;
  auto &leader_db = node(final_leader).holder->db;
  if (leader_db.is_degraded()) leader_db.resume();

  // Convergence: a sync write makes every leader entry durable, so
  // changes_since can deliver all of it; every other node must then reach
  // the leader's durable sequence.
  if (o.followers > 0) {
    leader_db.put({.sync = true}, as_view("fence"), as_view(""));
    const auto target = leader_db.durable_sequence();
    for (int i = 0; i < node_count; ++i) {
      if (i == final_leader) continue;
      const auto deadline = Clock::now() + std::chrono::seconds(20);
      while (durable_of(node(i)).value_or(0) < target) {
        if (Clock::now() >= deadline) {
          // What the leader would send it: the first entries past where it
          // stands, to tell a stream that is empty from one that is refused.
          const auto at_seq = durable_of(node(i)).value_or(0);
          std::string head;
          std::int64_t count = 0;
          auto snap = leader_db.snapshot();
          for (const auto &e : leader_db.changes_since(snap, at_seq)) {
            if (count < 8)
              head += std::format(" {}:{}", e.sequence,
                                  static_cast<int>(e.entry_type));
            ++count;
          }
          auto v = view.get();
          cluster.error(std::format(
              "node {}: stuck at durable sequence {} below the leader's {} "
              "(node {}) after 20s; changes_since yields {} entries:{}; "
              "source {} serving {} stall {} wanted {} leader-wanted {}",
              i, at_seq, target, final_leader, count, head,
              at(v.source, i),
              v.serving[static_cast<std::size_t>(i)] ? 1 : 0,
              node(i).repl_stall.load(), node(i).exclusive_wanted.load(),
              node(final_leader).exclusive_wanted.load()));
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

  // Final reads of every key on every node, so an append lost after its
  // last concurrent read still shows up, and convergence can be checked.
  const auto final_epoch = view.get().epoch;
  for (int i = 0; i < node_count; ++i) {
    auto &n = node(i);
    if (!n.holder) continue;
    const auto process = node_process(i, o.follower_readers);
    auto &events = buffers[static_cast<std::size_t>(process)];
    const auto key_count = keys.key_count();
    for (std::int64_t k0 = 0; k0 < key_count; k0 += 32) {
      std::vector<Mop> mops;
      for (auto k = k0; k < std::min(k0 + 32, key_count); ++k)
        mops.push_back({.append = false, .key = k, .element = 0, .read = {}});
      run_one(n.holder->db, o.followers > 0 ? Mode::Guarded : o.mode,
              {.process = process, .node = i, .epoch = final_epoch,
               .role = Role::Final, .wait_seq = 0},
              rec, events, std::move(mops), {}, false, totals);
    }
  }

  std::vector<Event> all;
  for (auto &b : buffers) {
    std::ranges::move(b, std::back_inserter(all));
  }
  write_history(o.out, std::move(all));
  if (o.followers > 0) {
    auto summary = o.out;
    summary += ".cluster.json";
    write_cluster_summary(summary, cluster, final_leader);
    std::printf("  cluster: bootstraps=%zu events=%zu ingests=%d "
                "ingest_errors=%d restarts=%d duplicates=%d lag_pauses=%d "
                "follower_vacuums=%d session_reads=%d "
                "history_rebootstraps=%d errors=%zu final_leader=%d\n",
                cluster.bootstraps.size(), cluster.events.size(),
                cluster.ingests.load(), cluster.ingest_errors.load(),
                cluster.restarts.load(), cluster.duplicates.load(),
                cluster.lag_pauses.load(), cluster.follower_vacuums.load(),
                cluster.session_reads.load(),
                cluster.history_rebootstraps.load(), cluster.errors.size(),
                final_leader);
  }
  nodes.clear();
  for (int i = 1; i < node_count; ++i)
    fs::remove_all(o.dir.string() + std::format("-n{}", i));

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
