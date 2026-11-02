// SPDX-License-Identifier: MIT
// Isolation history generator: runs concurrent transactions against one DB
// and writes the history as JSON for Elle (elle-cli) and the script's
// history checker. See docs/isolation_checking_design.md.
//
// Two workloads (--workload):
//
//   append  Each key holds a list of integers, stored as its value. A
//           transaction is 1-4 micro-operations: [r k] reads the list from the
//           transaction's snapshot, [append k e] extends it by a unique
//           element and puts the whole list back. The emulated append is a
//           true append only if the implicit W-W check on write keys holds, so
//           the model tests that check rather than assuming it. Checked by
//           Elle and the commit-sequence cross-check.
//   range   Point reads, scans, puts, dels and single-key range deletes over
//           32 fixed-width keys, each write a fresh value. Ranges overlap, so
//           a guard has to lose to another thread's in-flight write, and the
//           invariant (a range that held a key keeps one) makes write skew a
//           lost key. Checked by the script's history checker (#302).
//
// Configurations (--config):
//   guarded    WritePlan(snap), ensure_unchanged on every key read but not
//              written, ensure_range_unchanged on every range scanned but not
//              deleted. Expected strict-serializable.
//   unguarded  WritePlan(snap), no read guards. Expected snapshot isolation,
//              with write skew (G2-item) as the only serializability anomaly.
//   blind      db.get reads, snapshot-less WritePlan(). Expected to lose
//              appends: it shows the checker sees anomalies at all. Append
//              workload only.
//   presence   Range workload only (#676): guarded transactions mixed with
//              snapshot-less plans that rest on one ensure_present or
//              ensure_absent guard (db.del among them), and head reads of
//              the same few keys. A guard's outcome is a read of the key's
//              presence, a nullopt included. Expected strict-serializable.
//              The degrade nemesis runs often, and stalls the failing flush
//              between its error and the degrade, the window of #672.
//
// Nemeses, drawn from the seed: a vacuum thread over small files, and a
// client that fails its own fdatasync through the fault injector, degrading
// the engine, then calls resume(). Every exception is recorded as :info: a
// write whose fdatasync failed can still be replayed by resume().
//
// Usage:
//   isolation_history --config guarded|unguarded|blind|presence --out history.json
//                     [--workload append|range]
//                     [--seed S] [--threads N] [--txns N] [--dir PATH]
//                     [--no-vacuum] [--force-vacuum] [--no-degrade]
//                     [--followers N] [--follower-readers N] [--lag]
//                     [--kill-epochs N]
//   isolation_history --chaos-child --config C --seed S --threads N --dir D
//                     --fd N --process-base P --key-base K
//   isolation_history --absorb MANIFEST --config C --seed S --threads N
//                     --dir D --out history.json
//
// With --followers N (#178, docs/replication_checking_design.md) the leader
// gains N followers, each bootstrapped from a manifest under load and tailed
// through changes_since -> ingest, with reader threads of their own. Every
// operation carries a "node" (0 = leader); a session read carries "wait",
// the durable_sequence it waited for. Bootstraps and nemesis counts go to
// <out>.cluster.json.
//
// With --kill-epochs N (#176) the writers run in a child process that the
// parent SIGKILLs at a random point of its workload, N times, each child
// reopening what the last left, before one final read of every key. See
// "Kill nemesis" below. Kill counts go to <out>.kill.json.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
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
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fault_injector.h"

import bytecask;

extern char **environ; // NOLINT(readability-redundant-declaration)

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

enum class Mode : std::uint8_t { Guarded, Unguarded, Blind, Presence };

auto mode_name(Mode m) -> std::string_view {
  switch (m) {
  case Mode::Guarded:
    return "guarded";
  case Mode::Unguarded:
    return "unguarded";
  case Mode::Blind:
    return "blind";
  case Mode::Presence:
    return "presence";
  }
  return "?";
}

enum class Workload : std::uint8_t { Append, Range };

auto workload_name(Workload w) -> std::string_view {
  switch (w) {
  case Workload::Append:
    return "append";
  case Workload::Range:
    return "range";
  }
  return "?";
}

struct RunOptions {
  Mode mode{Mode::Guarded};
  Workload workload{Workload::Append};
  std::uint64_t seed{0};
  int threads{16};
  int txns{50'000};
  fs::path dir{"isolation_history_db"};
  fs::path out;
  bool no_vacuum{false};
  bool no_retention{false}; // vacuum ignores where followers are (#168)
  bool no_degrade{false};
  int followers{0};        // 0: leader only (#94); N: a cluster (#178)
  int follower_readers{4}; // reader threads per follower
  bool lag{false};         // pause replication threads at random
  bool force_vacuum{false}; // leader vacuum on whatever the seed draws
  bool topology{false};     // planned transfers, promotions, re-bootstraps
  bool promote_least{false}; // unplanned promotion takes the least advanced
  // --kill-epochs N (#176): N child processes in turn, each SIGKILLed at a
  // random point of its workload, the next one reopening what the last left.
  int kill_epochs{0};
  // Set on the child only, by the parent.
  bool kill_child{false};
  bool chaos_child{false}; // #232: driven by run_chaos.py --workload elle
  fs::path absorb;         // #232: a chaos episode's manifest
  int child_fd{-1};
  int process_base{0};
  std::int64_t key_base{0};
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

// The range workload's keys: fixed width, so bytewise order is numeric order.
constexpr int kRangeKeys = 32;
// The keys the presence configuration's snapshot-less plans and head reads
// use: few, so a retry lands on a key whose last write is still in flight.
constexpr int kPresenceKeys = 8;
constexpr int kRangeWidth = 2; // digits in a range key id

auto key_bytes(std::int64_t k) -> std::string { return std::format("k{}", k); }
auto range_key_bytes(std::int64_t k) -> std::string {
  return std::format("k{:0{}d}", k, kRangeWidth);
}

auto as_view(std::string_view s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

auto as_string(std::span<const std::byte> b) -> std::string {
  return {reinterpret_cast<const char *>(b.data()), b.size()};
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

  // Slots start at first_key: a process that continues a killed one's
  // history starts on the keys it was last appending to.
  explicit KeyPool(std::int64_t first_key = 0) : next_key_{first_key} {
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

// A range workload read or scan returns (key, value) pairs, sorted by key. A
// value names its writer: "w<session>.<count>".
using Pairs = std::vector<std::pair<std::int64_t, std::string>>;

// One micro-operation:
//
//   Read      point read of key, or a scan of [key, to) when to > key
//   Append    append element to the append workload's list at key
//   Put       put a fresh value at key, named write_id
//   Del       del key
//   RangeDel  del_range [key, to)
//   Guard     ensure_present (want_present) or ensure_absent of key in a
//             snapshot-less plan; observed is what its outcome says the key
//             was: want_present on a commit, the opposite on a nullopt
//   Get       point read of key through the head, after the invoke, into
//             pairs (empty: absent)
//
// A range workload read or scan fills pairs, a point read of the append
// workload fills read; both are set on completion, and empty when the
// transaction failed. Write ids are distinct, so two versions of a key never
// compare equal and the checker can place every version without seeing its
// bytes.
struct Mop {
  enum class Kind : std::uint8_t { Read, Append, Put, Del, RangeDel, Guard,
                                   Get };
  Kind kind{Kind::Read};
  std::int64_t key{0};
  std::int64_t element{0};  // Append: the appended list element
  std::int64_t to{0};       // point read and RangeDel: the exclusive bound
  std::uint64_t write_id{0}; // Put: the value this write stores
  std::optional<List> read;  // Read, append workload
  Pairs pairs;               // Read, range workload
  bool is_scan{false};       // Read: a range [key, to), not one key
  bool read_done{false};     // Read, Get: it ran; false when the transaction did not
  bool want_present{false};  // Guard: ensure_present rather than ensure_absent
  bool api_del{false};       // Guard: run as db.del, not as a plan
  std::optional<bool> observed; // Guard: unset when the outcome is unknown
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
  int life{0};               // the node's bootstrap count at the time
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

// One micro-operation as JSON. A scan is one group holding its point read
// and its scan, whichever op indexes the group is the read.
auto mop_json(const Mop &m) -> std::string {
  std::string result;
  if (m.read) {
    write_list_json(result, *m.read);
  } else if (m.kind == Mop::Kind::Read && !m.read_done) {
    result = "null";
  } else {
    result.push_back('[');
    for (std::size_t p = 0; p < m.pairs.size(); ++p) {
      if (p != 0) result.push_back(',');
      result += std::format(R"([{},"{}"])", m.pairs[p].first,
                            json_escape(m.pairs[p].second));
    }
    result.push_back(']');
  }
  switch (m.kind) {
  case Mop::Kind::Append:
    return std::format(R"(["append",{},{}])", m.key, m.element);
  case Mop::Kind::Read:
    if (!m.is_scan) return std::format(R"(["r",{},{}])", m.key, result);
    return std::format(R"([["r",{},{}],["scan",{},{},{}]])", m.key, result,
                       m.key, m.to, result);
  case Mop::Kind::Put:
    return std::format(R"(["put",{},{}])", m.key, m.write_id);
  case Mop::Kind::Del:
    return std::format(R"(["del",{}])", m.key);
  case Mop::Kind::RangeDel:
    return std::format(R"(["del",{},{}])", m.key, m.to);
  case Mop::Kind::Guard: {
    const auto want = m.want_present ? "present" : "absent";
    if (!m.observed) return std::format(R"(["guard",{},"{}"])", m.key, want);
    return std::format(R"(["guard",{},"{}","{}"])", m.key, want,
                       *m.observed ? "present" : "absent");
  }
  case Mop::Kind::Get:
    if (!m.read_done) return std::format(R"(["get",{}])", m.key);
    if (m.pairs.empty()) return std::format(R"(["get",{},null])", m.key);
    return std::format(R"(["get",{},"{}"])", m.key,
                       json_escape(m.pairs.front().second));
  }
  return "[]";
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
                        R"("life":{},"role":"{}","type":"{}","f":"txn",)"
                        R"("time":{},"value":[)",
                        e.index, e.process, e.node, e.epoch, e.life,
                        role_name(e.role), event_type_name(e.type), e.time_ns);
    for (std::size_t j = 0; j < e.value.size(); ++j) {
      const auto &m = e.value[j];
      if (j != 0) line.push_back(',');
      line += mop_json(m);
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

// The keys an append-workload transaction picks: one to four.
auto append_keys(std::mt19937_64 &rng, KeyPool &keys)
    -> std::vector<std::int64_t> {
  const auto n = 1 + static_cast<int>(rng() % 4);
  std::vector<std::int64_t> out(static_cast<std::size_t>(n));
  for (auto &k : out) k = keys.pick(rng, true);
  return out;
}

// A thread's write block: kElementsPerThread values, far more than any
// thread's transaction count, so a block never runs out. A block is the
// thread's whole allocation whether or not its process survived, so the
// history's values are unique without the parent tracking the highest one.
constexpr std::int64_t kElementsPerThread = 1 << 22;

auto block_base(int process) -> std::int64_t {
  return static_cast<std::int64_t>(process) * kElementsPerThread;
}

// One writer thread's transactions: appends from the key pool, or the range
// workload's from its generator. `next_element` is the thread's own write
// block (see block_base). Defined after RangeGenerator.
class RangeGenerator;
auto make_txns(Workload w, std::mt19937_64 &rng, KeyPool &keys,
               RangeGenerator &ranges, std::int64_t &next_element)
    -> std::vector<Mop>;

auto append_txn(std::mt19937_64 &rng, KeyPool &keys, std::int64_t &next_element)
    -> std::vector<Mop> {
  std::vector<Mop> mops;
  auto append = [&](std::int64_t key) {
    Mop m;
    m.kind = Mop::Kind::Append;
    m.key = key;
    m.element = next_element++;
    mops.push_back(m);
  };
  for (const auto key : append_keys(rng, keys)) {
    if (rng() % 2 == 0) {
      append(key);
    } else {
      Mop m;
      m.kind = Mop::Kind::Read;
      m.key = key;
      mops.push_back(m);
    }
  }
  return mops;
}

// A write's identity: "w<serial>.<n>", in hex so a value stays short.
auto write_value(std::uint64_t write_id) -> std::string {
  return std::format("w{:x}.{:x}", write_id >> 32, write_id & 0xffffffffU);
}

auto write_id(std::uint64_t serial, std::uint64_t count) -> std::uint64_t {
  return (serial << 32) | (count & 0xffffffffU);
}

// The range workload (#302). Keys are 0..31; a range is fixed width, so the
// ranges overlap. The invariant: a range that held a key at the
// transaction's snapshot still holds one after it commits. A transaction
// scans such a range, and when it holds two or more keys, deletes one,
// alternating between del and a single-key del_range; now and then it first
// inserts a fresh key into the range. Two transactions on overlapping ranges
// that each delete a key the other scanned are write skew, and lose a key on
// a run without range guards.
class RangeGenerator {
public:
  // serial names the generator: every writer thread and every process of a
  // kill or a chaos episode has its own, so a write's value names it uniquely
  // for the whole history ("w<serial>.<n>"), and a later read names the
  // transaction that wrote what it saw without being told.
  explicit RangeGenerator(std::uint64_t serial) : serial_{serial} {}

  auto next(std::mt19937_64 &rng) -> std::vector<Mop> {
    Mop scan;
    scan.kind = Mop::Kind::Read;
    scan.is_scan = true;
    const auto width = 2 + static_cast<int>(rng() % 6); // 2..7 keys
    scan.key = static_cast<std::int64_t>(
        rng() % static_cast<std::uint64_t>(kRangeKeys - width + 1));
    scan.to = scan.key + width; // within [0, kRangeKeys)
    std::vector<Mop> mops;
    if (rng() % 4 == 0) {
      Mop put;
      put.kind = Mop::Kind::Put;
      put.key = scan.key + static_cast<std::int64_t>(
                                rng() % static_cast<std::uint64_t>(width));
      put.write_id = write_id(serial_, next_count_++);
      mops.push_back(put);
    }
    mops.push_back(scan);
    // Which key the delete takes, the scan decides: the executor settles it
    // on the first key the scan returned, when the scan holds at least two,
    // and drops the delete when it holds fewer.
    Mop del;
    del.kind = rng() % 2 == 0 ? Mop::Kind::Del : Mop::Kind::RangeDel;
    mops.push_back(del);
    return mops;
  }

  // A presence transaction (#676), on one of the first kPresenceKeys keys: a
  // head read, or a snapshot-less plan resting on one guard, alone or with a
  // write of the guarded key. One guard, so a nullopt says which key it read.
  auto presence(std::mt19937_64 &rng) -> std::vector<Mop> {
    const auto key = static_cast<std::int64_t>(rng() % kPresenceKeys);
    Mop first;
    first.key = key;
    const auto r = rng() % 20;
    if (r >= 16) {
      first.kind = Mop::Kind::Get;
      return {first};
    }
    first.kind = Mop::Kind::Guard;
    // 0-3 db.del, 4-5 ensure_present + del, 6-9 ensure_absent + put (an
    // insert), 10-11 ensure_present + put, 12-15 a guard alone.
    first.want_present = r < 6 || (r >= 10 && r < 14);
    first.api_del = r < 4;
    std::vector<Mop> mops{first};
    if (r < 6) {
      Mop del;
      del.kind = Mop::Kind::Del;
      del.key = key;
      del.to = key;
      mops.push_back(del);
    } else if (r < 12) {
      Mop put;
      put.kind = Mop::Kind::Put;
      put.key = key;
      put.write_id = write_id(serial_, next_count_++);
      mops.push_back(put);
    }
    return mops;
  }

private:
  std::uint64_t serial_;
  std::uint64_t next_count_{0}; // one thread owns a generator
};

auto make_txns(Workload w, std::mt19937_64 &rng, KeyPool &keys,
               RangeGenerator &ranges, std::int64_t &next_element)
    -> std::vector<Mop> {
  if (w == Workload::Range) return ranges.next(rng);
  return append_txn(rng, keys, next_element);
}

// Points a delete at the first key the scan returned, as a del or a one-key
// del_range, when the scan holds two live keys other than the one this
// transaction inserted. A plan that put and deleted one key would leave
// nothing to say which it meant. Returns false when the scan holds fewer,
// and the transaction writes nothing: its guard would fail for the key it
// could not find, an empty del_range over a live range would not.
auto choose_delete(Mop &del, const Pairs &seen, std::int64_t inserted) -> bool {
  std::vector<std::int64_t> keys;
  for (const auto &[k, v] : seen)
    if (k != inserted) keys.push_back(k);
  if (keys.size() < 2) return false;
  del.key = keys.front();
  del.to = del.key + 1; // a del_range covers the one key; a del names it alone
  return true;
}

// The range workload's starting state: every key written once, so the first
// transactions scan ranges that hold keys. Runs on a fresh directory, or one
// a killed child left, and is idempotent: a key already present is left as it
// is, since a later life's writes are the ones the history records.
auto seed_range_workload(bytecask::DB &db) -> void {
  bytecask::Bytes buf;
  for (auto k = 0; k < kRangeKeys; ++k) {
    const auto kb = range_key_bytes(k);
    if (db.get({}, as_view(kb), buf)) continue;
    db.put({}, as_view(kb), as_view(std::format("s{}", k)));
  }
}

struct TxnOutcome {
  EventType type{EventType::Ok};
  std::uint64_t sequence{0};
  std::string error;
  bool durable{false}; // Ok: CommitResult.durable
};

// The (key, value) pairs of key ids in [from, to), read through the
// snapshot, or the head when there is none.
auto scan_pairs(const bytecask::DB &db, const bytecask::Snapshot *snap,
                std::int64_t from, std::int64_t to) -> Pairs {
  Pairs out;
  const auto lo = range_key_bytes(from);
  const auto hi = range_key_bytes(to);
  auto visit = [&](const bytecask::EntryView &e) {
    const auto k = std::string_view{
        reinterpret_cast<const char *>(e.key.data()), e.key.size()};
    if (k >= hi) return false;
    out.emplace_back(std::stoll(std::string{k.substr(1)}), as_string(e.value));
    return true;
  };
  if (snap) {
    for (const auto &e : snap->iter_from({}, as_view(lo)))
      if (!visit(e)) break;
  } else {
    for (const auto &e : db.iter_from({}, as_view(lo)))
      if (!visit(e)) break;
  }
  return out;
}

// Reads a range workload transaction's scan and settles each delete on a key
// the scan returned, in mops. Takes the snapshot the plan will be built on, so
// the history records the plan the transaction actually runs.
//
// The guarded configuration reads point keys and ranges from the snapshot;
// unguarded reads the head with no guards and decides on a second, later scan
// of the head, which puts the delete decision behind the plan it goes into.
// That is what makes write skew visible.
auto plan_range_txn(bytecask::DB &db, Mode mode, std::vector<Mop> &mops)
    -> std::optional<bytecask::Snapshot> {
  std::optional<bytecask::Snapshot> snap;
  if (mode != Mode::Blind) snap.emplace(db.snapshot());
  const auto *view = snap ? &*snap : nullptr;

  for (auto &m : mops) {
    switch (m.kind) {
    case Mop::Kind::Read:
      m.pairs = scan_pairs(db, view, m.key, m.to);
      m.read_done = true;
      break;
    case Mop::Kind::Del:
    case Mop::Kind::RangeDel: {
      const auto scan = std::ranges::find_if(
          mops, [](const Mop &x) { return x.kind == Mop::Kind::Read; });
      // The transaction decides on what it sees: its own scan, or under the
      // unguarded configuration a scan of the head taken after the snapshot.
      // That later scan is what puts its read set behind its plan.
      const auto seen =
          mode == Mode::Guarded
              ? scan->pairs
              : scan_pairs(db, nullptr, scan->key, scan->to);
      const auto put = std::ranges::find_if(
          mops, [](const Mop &x) { return x.kind == Mop::Kind::Put; });
      m.key = -1; // dropped below unless choose_delete decides it
      (void)choose_delete(m, seen, put == mops.end() ? -1 : put->key);
      // A del covers one row: choose_delete's one-key end is for a del_range.
      if (m.kind == Mop::Kind::Del) m.to = m.key;
      break;
    }
    case Mop::Kind::Put:
    case Mop::Kind::Append:
    case Mop::Kind::Guard:
    case Mop::Kind::Get:
      break;
    }
  }
  // A delete the scan did not decide writes nothing, and an operation that
  // writes nothing has no sequence, so it is not part of the transaction.
  std::erase_if(mops, [](const Mop &m) {
    return (m.kind == Mop::Kind::Del || m.kind == Mop::Kind::RangeDel) &&
           m.key < 0;
  });
  return snap;
}

// Builds the plan for a transaction plan_range_txn read, and applies it. The
// guarded configuration guards every key and range it read but did not write,
// and lets the engine lose the transaction when another thread changed what it
// read: that is the claim under test.
auto apply_range_txn(bytecask::DB &db, Mode mode, const std::vector<Mop> &mops,
                     std::optional<bytecask::Snapshot> snap,
                     bytecask::WriteOptions wo) -> TxnOutcome {
  // The plan holds the writes, in key order: a put, or a del or one-key
  // del_range of a key the transaction scanned.
  std::map<std::int64_t, const Mop *> writes;
  for (const auto &m : mops)
    if (m.kind == Mop::Kind::Put || m.kind == Mop::Kind::Del ||
        m.kind == Mop::Kind::RangeDel)
      writes[m.key] = &m;
  if (writes.empty()) return {};

  auto plan = snap ? bytecask::WritePlan{std::move(*snap)}
                   : bytecask::WritePlan{};
  for (const auto &[k, m] : writes) {
    const auto kb = range_key_bytes(k);
    switch (m->kind) {
    case Mop::Kind::Put: {
      const auto v = write_value(m->write_id);
      plan.put(as_view(kb), as_view(v));
      break;
    }
    case Mop::Kind::Del:
      plan.del(as_view(kb));
      break;
    case Mop::Kind::RangeDel:
      plan.del_range(as_view(kb), as_view(range_key_bytes(m->to)));
      break;
    case Mop::Kind::Read:
    case Mop::Kind::Append:
    case Mop::Kind::Guard:
    case Mop::Kind::Get:
      break;
    }
  }
  if (mode == Mode::Guarded) {
    // Guards: every key read but not written, every range scanned but not
    // deleted. A range a one-key del_range only partly covers was still read.
    for (const auto &m : mops) {
      if (m.kind != Mop::Kind::Read) continue;
      const auto kb = range_key_bytes(m.key);
      if (m.to == m.key) {
        if (!writes.contains(m.key)) plan.ensure_unchanged(as_view(kb));
      } else if (std::ranges::none_of(mops, [&](const Mop &x) {
                   return x.kind == Mop::Kind::RangeDel && x.key == m.key &&
                          x.to == m.to;
                 })) {
        plan.ensure_range_unchanged(as_view(range_key_bytes(m.key)),
                                    as_view(range_key_bytes(m.to)));
      }
    }
  }
  const auto r = db.apply_batch(wo, std::move(plan));
  if (!r) return {.type = EventType::Fail, .sequence = 0, .error = {}, .durable = false};
  return {.type = EventType::Ok,
          .sequence = r->sequence,
          .error = {},
          .durable = r->durable};
}

// Drops every read result in mops: an :info transaction's reads say nothing
// about what the database held, and Elle would otherwise order by them.
auto clear_reads(std::vector<Mop> &mops) -> void {
  for (auto &m : mops) {
    if (m.kind != Mop::Kind::Read && m.kind != Mop::Kind::Get &&
        m.kind != Mop::Kind::Guard)
      continue;
    m.read.reset();
    m.pairs.clear();
    m.read_done = false;
    m.observed.reset();
  }
}

auto is_presence_txn(const std::vector<Mop> &mops) -> bool {
  return !mops.empty() && (mops.front().kind == Mop::Kind::Guard ||
                           mops.front().kind == Mop::Kind::Get);
}

// Runs a presence transaction (#676) and records what its outcome says the
// key was. A plan that rests on its guard alone appends nothing, so its answer
// rests on the head entry its guard read; with sync = false it does not drive
// the flush that publishes that entry, which is the path of #672.
auto apply_presence_txn(bytecask::DB &db, std::vector<Mop> &mops,
                        bytecask::WriteOptions wo) -> TxnOutcome {
  auto &first = mops.front();
  const auto kb = range_key_bytes(first.key);
  if (first.kind == Mop::Kind::Get) {
    bytecask::Bytes buf;
    if (db.get({}, as_view(kb), buf))
      first.pairs = {{first.key, as_string(buf)}};
    first.read_done = true;
    return {};
  }
  std::optional<bytecask::CommitResult> r;
  if (first.api_del) {
    r = db.del(wo, as_view(kb));
  } else {
    bytecask::WritePlan plan;
    if (first.want_present) {
      plan.ensure_present(as_view(kb));
    } else {
      plan.ensure_absent(as_view(kb));
    }
    for (const auto &m : mops) {
      if (m.kind == Mop::Kind::Put) {
        const auto v = write_value(m.write_id);
        plan.put(as_view(kb), as_view(v));
      } else if (m.kind == Mop::Kind::Del) {
        plan.del(as_view(kb));
      }
    }
    r = db.apply_batch(wo, std::move(plan));
  }
  first.observed = r ? first.want_present : !first.want_present;
  if (!r) return {.type = EventType::Fail, .sequence = 0, .error = {}, .durable = false};
  return {.type = EventType::Ok,
          .sequence = r->sequence,
          .error = {},
          .durable = r->durable};
}

// Runs mops as one transaction. Fills each read mop's result. Keys the
// transaction touches are loaded once; later mops see its own earlier
// appends, as Transaction (Layer 2) would.
auto execute_txn(bytecask::DB &db, Mode mode, Workload workload,
                 std::vector<Mop> &mops, bytecask::WriteOptions wo)
    -> TxnOutcome {
  if (workload == Workload::Range)
    return apply_range_txn(db, mode, mops, plan_range_txn(db, mode, mops), wo);

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
    if (m.kind == Mop::Kind::Append) {
      l.push_back(m.element);
      written[m.key] = true;
    } else {
      m.read = l;
      m.read_done = true;
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
  if (!r) return {.type = EventType::Fail, .sequence = 0, .error = {}, .durable = false};
  return {.type = EventType::Ok,
          .sequence = r->sequence,
          .error = {},
          .durable = r->durable};
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
  int life{0};
  Role role{Role::Leader};
  std::uint64_t wait_seq{0};
};

auto run_one(bytecask::DB &db, Mode mode, Workload workload, Placement at,
             Recorder &rec, std::vector<Event> &events, std::vector<Mop> mops,
             bytecask::WriteOptions wo, bool inject_sync, Totals &totals)
    -> std::uint64_t {
  Event inv;
  inv.process = at.process;
  inv.node = at.node;
  inv.epoch = at.epoch;
  inv.life = at.life;
  inv.role = at.role;
  inv.wait_seq = at.wait_seq;
  // The presence configuration's guarded transactions are the guarded
  // configuration's; its presence transactions run apart.
  const auto presence = is_presence_txn(mops);
  if (mode == Mode::Presence) mode = Mode::Guarded;
  // A range transaction's scan and delete decide its plan, so it is read
  // before the invoke is recorded: what the history names is what it ran. A
  // presence transaction reads after the invoke, so its window holds its read.
  auto snap = workload == Workload::Range && !presence
                  ? plan_range_txn(db, mode, mops)
                  : std::optional<bytecask::Snapshot>{};
  inv.value = mops;
  rec.stamp(inv);

  TxnOutcome outcome;
  try {
    const auto run = [&] {
      if (presence) return apply_presence_txn(db, mops, wo);
      return workload == Workload::Range
                 ? apply_range_txn(db, mode, mops, std::move(snap), wo)
                 : execute_txn(db, mode, workload, mops, wo);
    };
    if (inject_sync) {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      outcome = run();
    } else {
      outcome = run();
    }
  } catch (const bytecask::DbFollowerMode &e) {
    // Refused at admission, before anything is appended: the node was
    // demoted under the client. If the write ever shows up, it is G1a.
    outcome = {.type = EventType::Fail, .sequence = 0, .error = e.what(), .durable = false};
  } catch (const std::exception &e) {
    outcome = {.type = EventType::Info, .sequence = 0, .error = e.what(), .durable = false};
  }

  Event done;
  done.process = at.process;
  done.node = at.node;
  done.epoch = at.epoch;
  done.life = at.life;
  done.role = at.role;
  done.wait_seq = at.wait_seq;
  done.type = outcome.type;
  // A transaction reports the plan it ran: the range workload's executor
  // settles each delete's key before appending, so mops, not inv.value, names
  // what it really tried. Only an :ok carries read results; a failed or
  // indeterminate one reports the same plan with no read result. A presence
  // transaction's nullopt is an answer: its guard keeps what it observed.
  done.value = std::move(mops);
  if (outcome.type == EventType::Info ||
      (outcome.type == EventType::Fail && !presence))
    clear_reads(done.value);
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
  // Set by bootstrap, under the vacuum gate, to the manifest's
  // through_sequence: the node resumes there once installed, so vacuum must
  // keep what lies above it before the node is serving.
  std::atomic<bool> joining{false};
  std::atomic<std::uint64_t> joining_at{0};
  // Where replicate() last gave up on an iteration, for the stuck report.
  std::atomic<int> repl_stall{0};
  // How many times the node was bootstrapped; every operation records the
  // life it ran in, so what a node read of a branch it later turned out to
  // be on can be told from what it read once re-bootstrapped.
  std::atomic<int> life{0};
  // The source and view epoch of the node's last completed pull: an ingest
  // that returned, or one the engine refused as a fork. A pull at a new pair
  // runs even when the node is not behind, since an empty slice is how the
  // engine compares the histories of a node ahead of its source (#397).
  // Reset by bootstrap.
  std::atomic<int> pulled_src{-1};
  std::atomic<int> pulled_epoch{-1};
  // The last epoch whose leader's history the node is known to hold a
  // prefix of: the epoch it was bootstrapped from the leader in, last
  // ingested from the leader in, or led itself. Under --promote-least a node
  // ahead of a promotion keeps its old branch until its first pull is
  // refused; promoted before that, it leads from that branch. Node 0 starts
  // as the leader of epoch 0; any other node sets it at bootstrap.
  std::atomic<int> held_epoch{0};
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

// A topology event, for the summary. kind is "planned", "unplanned",
// "retarget" or "fork"; durable is the promoted node's durable_sequence()
// when it stopped tailing (for a retarget, the retargeted node's; for a
// fork, the position the refused node pulled from). held, for an
// unplanned promotion, is the epoch whose leader's history the promoted
// node held a prefix of: the old leader's epoch when it tailed it, an
// earlier one when it leads from another branch.
struct TopologyEvent {
  std::string kind;
  int epoch{0};
  int from{0}; // fork: the source
  int to{0};   // fork: the refused node
  std::uint64_t durable{0};
  std::uint64_t other_durable{0}; // planned: the old leader's; retarget, fork: the source's
  int life{0};                    // fork: the refused node's life
  int held{-1};                   // unplanned: see above
};

struct ClusterStats {
  std::mutex mu;
  std::vector<Bootstrap> bootstraps;  // under mu
  std::vector<TopologyEvent> events;  // under mu
  std::vector<std::string> errors;    // under mu
  std::atomic<int> ingests{0};
  std::atomic<int> ingest_errors{0};
  std::atomic<int> forks_detected{0}; // slices the engine refused as forks
  std::atomic<int> restarts{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> lag_pauses{0};
  std::atomic<int> follower_vacuums{0};
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

auto ingest_owned(bytecask::DB &follower, const bytecask::ChangeHeader &header,
                  const std::vector<OwnedEntry> &buf) -> void {
  std::vector<bytecask::DataEntryView> views;
  views.reserve(buf.size());
  for (const auto &e : buf) {
    views.push_back({e.sequence, e.entry_type, e.key, e.value});
  }
  follower.ingest(header, views);
}

// What max_bytes asks of changes_since for 1-64 entries of this workload.
// Its records are small — a short key and a list of a few elements — so this
// errs low: a slice that comes out shorter than 1-64 entries publishes more
// intermediate states, which is what the replication checks read. At 256
// the slices grew several times, and the unretained configuration stopped
// seeing #168 in most rounds. The engine cuts after the unit that passes
// the budget, so a batch comes through whole.
constexpr std::size_t kNominalEntryBytes = 48;

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
        n.joining_at.store(m.through_sequence, std::memory_order_release);
        n.joining.store(true, std::memory_order_release);
        fs::remove_all(n.dir);
        fs::create_directories(n.dir);
        for (const auto &fi : m.files) {
          fs::copy_file(fi.data_path, n.dir / fi.data_path.filename());
          // A hint the leader failed to write is rebuilt by the open.
          if (fs::exists(fi.hint_path))
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
      n.life.fetch_add(1, std::memory_order_acq_rel);
      n.pulled_src.store(-1, std::memory_order_release);
      n.pulled_epoch.store(-1, std::memory_order_release);
      return;
    } catch (const std::exception &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

// Phase 2 for node n, from whichever node the view says it tails: wake on
// the source's durable sequence, then drain what one snapshot of it holds
// above n's durable_sequence() as changes_since slices, each cut by the
// engine at a random max_bytes and given to one ingest. The first pull from
// a new source or epoch skips the wake: a node that is not behind still
// delivers an empty slice, which is how the engine finds one ahead of its
// source on another history. A slice the engine refuses as a fork (#397)
// takes n out of service; the orchestrator re-bootstraps it like a node an
// unplanned promotion abandoned. Nemeses, once per drain: lag pauses,
// duplicate delivery, vacuum on n and restarts of n. The view is read with
// n's gate held, so a promotion (which changes n's source while holding n
// exclusively) never races an ingest into n.
// The retain_after a vacuum on node `self` must use: the lowest position a
// node could resume changes_since from, over every node that is serving or
// joining, `self` aside. This is the replication service's decision, not
// the engine's. A node whose position cannot be read right now (restarting,
// being installed) counts as 0, which keeps everything. Without retention
// (the sensitivity configuration) vacuum drops what it can.
auto retention_point(std::vector<std::unique_ptr<Node>> &nodes,
                     const ClusterView &view, int self, bool retain)
    -> std::uint64_t {
  if (!retain) return bytecask::kNoRetention;
  const auto v = view.get();
  auto point = bytecask::kNoRetention;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    auto &node = *nodes[i];
    if (node.id == self) continue;
    if (node.joining.load(std::memory_order_acquire)) {
      point = std::min(point, node.joining_at.load(std::memory_order_acquire));
      continue;
    }
    if (!v.serving[i]) continue;
    std::uint64_t at = 0;
    if (auto g = enter(node); g.owns_lock()) at = node.holder->db.durable_sequence();
    point = std::min(point, at);
  }
  return point;
}

auto replicate(std::vector<std::unique_ptr<Node>> &nodes, Node &n,
               ClusterView &view, bool lag, bool retain, std::uint64_t seed,
               const std::atomic<bool> &stop, ClusterStats &stats) -> void {
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
    const auto first_pull =
        n.pulled_src.load(std::memory_order_acquire) != src ||
        n.pulled_epoch.load(std::memory_order_acquire) != v.epoch;
    std::uint64_t from = 0;
    try {
      if (rng() % 50 == 0) {
        const auto threshold = static_cast<double>(rng() % 60) / 100.0;
        const auto retain_after = retention_point(nodes, view, n.id, retain);
        if (fdb.vacuum({.fragmentation_threshold = threshold,
                        .retain_after = retain_after}))
          stats.follower_vacuums.fetch_add(1, std::memory_order_relaxed);
      }
      from = fdb.durable_sequence();
      if (!first_pull &&
          leader.durable_sequence(from + 1, std::chrono::milliseconds(20)) <=
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
      // The drain ends on an empty slice, which is also what a node that
      // is not behind delivers: a slice the engine checks, and that writes
      // nothing.
      do {
        from = buf.empty() ? from : buf.back().sequence;
        buf.clear();
        auto batch = leader.changes_since(
            snap, from, (1 + rng() % 64) * kNominalEntryBytes);
        for (const auto &e : batch.entries) {
          buf.push_back({e.sequence, e.entry_type,
                         bytecask::Bytes{e.key.begin(), e.key.end()},
                         bytecask::Bytes{e.value.begin(), e.value.end()}});
        }
        ingest_owned(fdb, batch.header, buf);
        stats.ingests.fetch_add(1, std::memory_order_relaxed);
        n.pulled_src.store(src, std::memory_order_release);
        n.pulled_epoch.store(v.epoch, std::memory_order_release);
      } while (!buf.empty() && !stop.load(std::memory_order_relaxed));
      // Only a drain that ended on an empty slice has had the engine compare
      // the histories at or above n's own position: a duplicate slice
      // rewound below it, accepted, proves nothing about what n holds above.
      if (buf.empty() && src == v.leader)
        n.held_epoch.store(v.epoch, std::memory_order_release);
    } catch (const bytecask::DbChangeMarkerMismatch &e) {
      // n's history diverged from its source's at a promotion, and the
      // engine refused the slice before writing anything. n stops serving
      // and tailing here; rebootstrap_abandoned() rebuilds it from the
      // leader's manifest, the remedy for a fork.
      stats.forks_detected.fetch_add(1, std::memory_order_relaxed);
      stats.event({.kind = "fork", .epoch = v.epoch, .from = src, .to = n.id,
                   .durable = from, .other_durable = leader.durable_sequence(),
                   .life = n.life.load(std::memory_order_acquire)});
      std::fprintf(stderr, "node %d: slice from node %d at %llu refused: %s\n",
                   n.id, src, static_cast<unsigned long long>(from), e.what());
      view.update([&](View &w) {
        w.serving[static_cast<std::size_t>(n.id)] = false;
        at(w.source, n.id) = -1;
      });
      n.pulled_src.store(src, std::memory_order_release);
      n.pulled_epoch.store(v.epoch, std::memory_order_release);
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
    tn.held_epoch.store(epoch, std::memory_order_release);
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
// protocol with the rule inverted: a follower ahead of the target forks, and
// the engine refuses its first slice from the new leader (#397).
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
  // A node out of service, abandoned or refused as a fork and waiting for
  // its re-bootstrap, is not a candidate.
  const auto serving = view.get().serving;
  std::vector<int> candidates;
  std::vector<std::uint64_t> durables;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto id = static_cast<int>(i);
    if (id == old || !serving[i]) continue;
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
  int held = 0;
  {
    NodeExclusive x{tn};
    promoted_at = tn.holder->db.durable_sequence();
    held = tn.held_epoch.load(std::memory_order_acquire);
    tn.holder->db.set_mode(bytecask::Mode::Leader);
    view.update([&](View &v) {
      v.leader = target;
      epoch = ++v.epoch;
      tn.held_epoch.store(epoch, std::memory_order_release);
      for (std::size_t i = 0; i < v.source.size(); ++i) {
        if (static_cast<int>(i) != target && v.serving[i])
          v.source[i] = target;
      }
    });
  }
  stats.event({.kind = "unplanned", .epoch = epoch, .from = old, .to = target,
               .durable = promoted_at, .other_durable = 0, .held = held});
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
                       R"("durable":{},"other_durable":{},"life":{},)"
                       R"("held":{}}})",
                       i == 0 ? "" : ",", e.kind, e.epoch, e.from, e.to,
                       e.durable, e.other_durable, e.life, e.held);
  }
  out << "],\"errors\":[";
  for (std::size_t i = 0; i < stats.errors.size(); ++i) {
    out << (i == 0 ? "" : ",") << '"' << json_escape(stats.errors[i]) << '"';
  }
  out << std::format(
      R"(],"final_leader":{},"ingests":{},"ingest_errors":{},)"
      R"("forks_detected":{},"restarts":{},)"
      R"("duplicates":{},"lag_pauses":{},"follower_vacuums":{},)"
      R"("session_reads":{}}})",
      final_leader, stats.ingests.load(), stats.ingest_errors.load(),
      stats.forks_detected.load(), stats.restarts.load(),
      stats.duplicates.load(),
      stats.lag_pauses.load(), stats.follower_vacuums.load(),
      stats.session_reads.load());
  out << "\n";
}

// ---------------------------------------------------------------------------
// Kill nemesis (#176): SIGKILL the process under concurrent group-commit
// writers, reopen, go on. See docs/isolation_checking_design.md.
//
// A SIGKILL takes the history recorded in memory with it, so the workload
// runs in a child process that streams every operation to the parent as it
// happens, one line each:
//   I <process> <time_ns> <sync> <n> <mop>...           before the call
//   C <process> <time_ns> <type> <sequence> <durable> <n> <mop>...  after it
//   D <sequence>                                         durable_sequence()
// with <mop> = A <key> <element> or R <key> <len> <element>... (a read's
// result, on C only; N <key> for a read with none). Lines are written whole,
// under a mutex; a line the kill cut short is discarded. Times are
// CLOCK_MONOTONIC, the same clock in every process.
// ---------------------------------------------------------------------------

auto monotonic_ns() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

// The kill configuration: the seed's engine configuration, with enough sync
// writers that several share each fdatasync, and no degrade nemesis.
auto kill_config(std::uint64_t seed) -> Config {
  auto c = config_for(seed);
  c.sync_percent = c.sync_percent < 50 ? 50 : 90;
  c.degrade = false;
  return c;
}

// The final read of a kill or chaos history: every key once, from the
// snapshot, as one read-only transaction finishing last. The range workload's
// final value is recorded in pairs, the append workload's in read.
auto append_final_read(std::vector<Event> &events, std::uint64_t &next_index,
                       std::int64_t start_ns, Workload workload,
                       const bytecask::Snapshot &snap, Event inv) -> void {
  const auto last = workload == Workload::Range ? kRangeKeys - 1 : INT64_MAX;
  // The keys come from the snapshot, not from a key id: a started process
  // spends ids without ever writing them, so the ids run far above the keys
  // a final read could enumerate between 0 and the highest.
  std::vector<Mop> mops;
  for (const auto &e : snap.iter_from({}, {})) {
    const auto key = std::string_view{
        reinterpret_cast<const char *>(e.key.data()), e.key.size()};
    const auto prefix = workload == Workload::Range ? 'r' : 'k';
    if (key.empty() || key.front() != prefix) continue;
    Mop m;
    m.key = std::stoll(std::string{key.substr(1)});
    if (m.key > last) continue;
    m.to = m.key;
    m.read_done = true;
    if (workload == Workload::Range) {
      m.pairs.emplace_back(m.key, std::string{reinterpret_cast<const char *>(
                                          e.value.data()),
                                      e.value.size()});
    } else {
      m.read = decode_list(e.value);
    }
    mops.push_back(std::move(m));
  }
  std::ranges::sort(mops, {}, &Mop::key);
  inv.value = std::move(mops);
  inv.index = next_index++;
  inv.time_ns = monotonic_ns() - start_ns;
  inv.type = EventType::Invoke;
  auto done = inv;
  done.type = EventType::Ok;
  done.index = next_index++;
  done.time_ns = monotonic_ns() - start_ns;
  events.push_back(std::move(inv));
  events.push_back(std::move(done));
}

class LineWriter {
public:
  explicit LineWriter(int fd) : fd_{fd} {}

  auto write(const std::string &line) -> void {
    std::lock_guard<std::mutex> g{mu_};
    std::size_t done = 0;
    while (done < line.size()) {
      const auto n = ::write(fd_, line.data() + done, line.size() - done);
      if (n < 0) {
        if (errno == EINTR) continue;
        std::_Exit(3); // the parent is gone
      }
      done += static_cast<std::size_t>(n);
    }
  }

private:
  std::mutex mu_;
  int fd_;
};

// One mop per token group, then its result when with_reads:
//   A <key> <element>                 append
//   R <from> <to> <n> [<value>...]    point read (from == to) or scan, the
//                                     values as key:value pairs, space-free
//   N <from> <to>                     the same without a result
//   P <key> <write id>                put
//   D <from> <to>                     del (from == to) or del_range
auto append_mops(std::string &line, const std::vector<Mop> &mops,
                 bool with_reads) -> void {
  line += std::format(" {}", mops.size());
  for (const auto &m : mops) {
    switch (m.kind) {
    case Mop::Kind::Append:
      line += std::format(" A {} {}", m.key, m.element);
      break;
    case Mop::Kind::Put:
      line += std::format(" P {} {}", m.key, m.write_id);
      break;
    case Mop::Kind::Del:
    case Mop::Kind::RangeDel:
      line += std::format(" D {} {}", m.key, m.to);
      break;
    case Mop::Kind::Guard:
    case Mop::Kind::Get:
      // The presence configuration runs in-process only (parse_args).
      throw std::logic_error{"presence mops have no kill wire format"};
    case Mop::Kind::Read:
      if (with_reads && m.read) {
        line += std::format(" R {} {} {}", m.key, m.to, m.read->size());
        for (const auto e : *m.read) line += std::format(" {}", e);
      } else if (with_reads) {
        line += std::format(" R {} {} {}", m.key, m.to, m.pairs.size());
        for (const auto &[k, v] : m.pairs) line += std::format(" {}:{}", k, v);
      } else {
        line += std::format(" N {} {}", m.key, m.to);
      }
      break;
    }
  }
}

// The child: opens the directory (recovering from the last kill), runs the
// writers, the vacuum nemesis and a durable_sequence() watcher, and streams
// everything until the parent kills it. After its share of transactions it
// idles, so a late kill does not grow the history without bound.
auto run_kill_child(const RunOptions &o) -> int {
  const auto cfg = kill_config(o.seed);
  auto db = bytecask::DB::open(o.dir, db_options(cfg));
  if (o.workload == Workload::Range) seed_range_workload(db);
  LineWriter out{o.child_fd};
  KeyPool keys{o.key_base};
  std::atomic<int> remaining{o.txns};
  std::atomic<bool> stop{false};

  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&] {
      std::mt19937_64 vrng{o.seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        try {
          std::ignore = db.vacuum({.fragmentation_threshold = threshold});
        } catch (const std::exception &) {
        }
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }
  std::jthread durable_thread{[&] {
    std::uint64_t last = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const auto d = db.durable_sequence(last + 1, std::chrono::milliseconds(5));
      if (d > last) {
        last = d;
        out.write(std::format("D {}\n", d));
      }
    }
  }};

  std::vector<std::jthread> writers;
  for (int t = 0; t < o.threads; ++t) {
    writers.emplace_back([&, t] {
      std::mt19937_64 rng{o.seed + 100 + static_cast<std::uint64_t>(t)};
      const auto process = o.process_base + t;
      // The process counts every thread of every earlier process too, so a
      // write's value names it uniquely across the whole history.
      RangeGenerator ranges{static_cast<std::uint64_t>(process)};
      // This thread's writes come from its own block (block_base above).
      std::int64_t next_element = block_base(process);
      while (remaining.fetch_sub(1, std::memory_order_relaxed) > 0) {
        auto mops = make_txns(o.workload, rng, keys, ranges, next_element);
        const auto sync =
            static_cast<int>(rng() % 100) < cfg.sync_percent;
        TxnOutcome r;
        std::string line;
        bool started = false;
        // What a range transaction will run is settled by its scan, so it is
        // read before the invoke goes out: the stream names the plan it ran,
        // which is what a killed operation is judged on. Both steps sit in
        // one try, so an I/O fault during the scan fails the transaction
        // rather than the child.
        try {
          auto snap = o.workload == Workload::Range
                          ? plan_range_txn(db, o.mode, mops)
                          : std::optional<bytecask::Snapshot>{};
          line = std::format("I {} {} {}", process, monotonic_ns(),
                             sync ? 1 : 0);
          append_mops(line, mops, false);
          out.write(line + "\n");
          started = true;
          r = o.workload == Workload::Range
                  ? apply_range_txn(db, o.mode, mops, std::move(snap),
                                    {.sync = sync})
                  : execute_txn(db, o.mode, o.workload, mops, {.sync = sync});
        } catch (const std::exception &e) {
          r = {.type = EventType::Info, .sequence = 0, .error = e.what(),
               .durable = false};
        }
        if (!started) continue;
        line = std::format("C {} {} {} {} {}", process, monotonic_ns(),
                           event_type_name(r.type), r.sequence,
                           r.durable ? 1 : 0);
        append_mops(line, mops, r.type == EventType::Ok);
        out.write(line + "\n");
      }
    });
  }
  for (auto &w : writers) w.join();
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

auto spawn_kill_child(const RunOptions &o, int epoch, int write_fd) -> pid_t {
  const auto self = fs::read_symlink("/proc/self/exe").string();
  std::vector<std::string> args{
      self,
      "--kill-child",
      "--config", std::string{mode_name(o.mode)},
      "--workload", std::string{workload_name(o.workload)},
      "--seed", std::to_string(o.seed + static_cast<std::uint64_t>(epoch)),
      "--threads", std::to_string(o.threads),
      "--txns", std::to_string(std::max(1, o.txns / o.kill_epochs)),
      "--dir", o.dir.string(),
      "--fd", std::to_string(write_fd),
      "--process-base", std::to_string(o.process_base),
      "--key-base", std::to_string(o.key_base)};
  std::vector<char *> argv;
  for (auto &a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (const auto rc = posix_spawn(&pid, self.c_str(), nullptr, nullptr,
                                  argv.data(), environ);
      rc != 0) {
    throw std::system_error{rc, std::generic_category(), "posix_spawn"};
  }
  return pid;
}

// Runs one child until it has reported `kill_after` completions, SIGKILLs
// it, and returns what it streamed and when it was killed. Counting
// completions rather than time puts the kill inside the workload however
// fast the machine is; the time limit only guards against a stuck child.
// Throws if the child exited on its own.
auto run_and_kill(const RunOptions &o, int epoch, int kill_after)
    -> std::pair<std::string, std::int64_t> {
  int fds[2];
  if (::pipe(fds) != 0)
    throw std::system_error{errno, std::generic_category(), "pipe"};
  const auto pid = spawn_kill_child(o, epoch, fds[1]);
  ::close(fds[1]);

  std::string stream;
  std::array<char, 1 << 16> buf{};
  const auto deadline = Clock::now() + std::chrono::minutes(2);
  std::int64_t killed_at = 0;
  auto completions = 0;
  std::size_t scanned = 0;
  auto eof = false;
  while (!eof) {
    int timeout_ms = -1;
    if (killed_at == 0) {
      for (; scanned < stream.size(); ++scanned) {
        if (stream[scanned] == 'C' &&
            (scanned == 0 || stream[scanned - 1] == '\n'))
          ++completions;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now());
      if (completions >= kill_after || left.count() <= 0) {
        ::kill(pid, SIGKILL);
        killed_at = monotonic_ns();
      } else {
        timeout_ms = static_cast<int>(left.count());
      }
    }
    pollfd pfd{.fd = fds[0], .events = POLLIN, .revents = 0};
    const auto ready =
        ::poll(&pfd, 1, killed_at != 0 ? -1 : std::max(timeout_ms, 1));
    if (ready < 0 && errno != EINTR)
      throw std::system_error{errno, std::generic_category(), "poll"};
    if (ready <= 0) continue;
    const auto n = ::read(fds[0], buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error{errno, std::generic_category(), "read"};
    }
    if (n == 0) {
      eof = true;
    } else {
      stream.append(buf.data(), static_cast<std::size_t>(n));
    }
  }
  ::close(fds[0]);
  if (killed_at == 0) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (killed_at == 0 || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    throw std::runtime_error{std::format(
        "epoch {}: child exited before the kill (status {:#x}); see its "
        "stderr above",
        epoch, status)};
  }
  return {std::move(stream), killed_at};
}

struct KillStats {
  int epochs{0};
  int in_flight{0};        // invoked, not completed, at a kill
  int downgraded{0};       // acknowledged, not durable at a kill
  int group_kills{0};      // kills with two or more sync writers in flight
  std::int64_t ok{0};
  std::int64_t fail{0};
};

// Turns one child's stream into history events: completions as they were
// reported, except that an acknowledged write not known durable at the kill
// becomes :info (a crash may lose it), and every operation still in flight
// gets an :info completion at the kill.
// not_durable, when given, receives every element appended by an operation
// that ends up :info: a power cut may still take it (#232).
//
// power_cut, set by a chaos life, drops a completion to :info whatever its
// process lived to do: the life's terminator is drawn after it ends, so one
// that ends in a power cut keeps only what durable says. A SIGKILL, which
// the kill mode always ends in, keeps the page cache, so there an
// acknowledged write that is not durable is still :ok.
auto absorb_epoch(std::string_view stream, int epoch, std::int64_t killed_at,
                  std::int64_t start_ns, std::vector<Event> &events,
                  std::uint64_t &next_index, std::int64_t &max_key,
                  bool range_workload, bool power_cut, KillStats &stats,
                  std::vector<std::int64_t> *not_durable = nullptr) -> void {
  struct Pending {
    std::vector<Mop> mops;
    bool sync{false};
  };
  std::map<int, Pending> pending;
  struct Done {
    Event event;
    std::vector<Mop> invoked;
    bool durable{false};
  };
  std::vector<Done> completions;
  std::uint64_t durable = 0;

  auto parse_mops = [&](std::istringstream &in,
                        bool with_reads) -> std::vector<Mop> {
    std::size_t n = 0;
    in >> n;
    std::vector<Mop> mops(n);
    for (auto &m : mops) {
      char kind = 0;
      in >> kind;
      if (kind == 'A') {
        m.kind = Mop::Kind::Append;
        in >> m.key >> m.element;
      } else if (kind == 'P') {
        m.kind = Mop::Kind::Put;
        in >> m.key >> m.write_id;
      } else if (kind == 'D') {
        in >> m.key >> m.to;
        m.kind = m.to == m.key ? Mop::Kind::Del : Mop::Kind::RangeDel;
      } else {
        m.kind = Mop::Kind::Read;
        in >> m.key >> m.to;
        m.is_scan = m.to > m.key;
        m.read_done = kind == 'R';
        if (kind == 'N' || !with_reads) continue;
        std::size_t len = 0;
        in >> len;
        if (range_workload) {
          m.pairs.reserve(len);
          for (std::size_t i = 0; i < len; ++i) {
            std::string pair;
            in >> pair;
            const auto colon = pair.find(':');
            if (colon == std::string::npos)
              throw std::runtime_error{"malformed pair from the child"};
            m.pairs.emplace_back(std::stoll(pair.substr(0, colon)),
                                 pair.substr(colon + 1));
          }
        } else {
          List l(len);
          for (auto &e : l) in >> e;
          m.read = std::move(l);
        }
      }
      max_key = std::max(max_key, m.key);
    }
    if (!in) throw std::runtime_error{"malformed line from the child"};
    return mops;
  };

  std::size_t pos = 0;
  while (true) {
    const auto end = stream.find('\n', pos);
    if (end == std::string_view::npos) break; // cut short by the kill
    std::istringstream in{std::string{stream.substr(pos, end - pos)}};
    pos = end + 1;
    char tag = 0;
    in >> tag;
    if (tag == 'D') {
      std::uint64_t d = 0;
      in >> d;
      durable = std::max(durable, d);
      continue;
    }
    // The chaos child's open, open-failure, reopen-required and close lines
    // (#232) carry nothing the history needs.
    if (tag == 'O' || tag == 'F' || tag == 'R' || tag == 'X') continue;
    Event e;
    std::int64_t t = 0;
    in >> e.process >> t;
    e.time_ns = t - start_ns;
    e.epoch = epoch;
    e.index = next_index++;
    if (tag == 'I') {
      int sync = 0;
      in >> sync;
      e.type = EventType::Invoke;
      e.value = parse_mops(in, false);
      pending[e.process] = {e.value, sync != 0};
      events.push_back(std::move(e));
    } else if (tag == 'C') {
      std::string type;
      int dur = 0;
      in >> type >> e.sequence >> dur;
      e.type = type == "ok" ? EventType::Ok
               : type == "fail" ? EventType::Fail
                                : EventType::Info;
      e.value = parse_mops(in, true);
      auto it = pending.find(e.process);
      if (it == pending.end())
        throw std::runtime_error{"completion without an invoke"};
      completions.push_back({std::move(e), std::move(it->second.mops), dur != 0});
      pending.erase(it);
    } else {
      throw std::runtime_error{std::format("unknown line tag {}", tag)};
    }
  }

  auto note_not_durable = [&](const std::vector<Mop> &mops) {
    if (!not_durable) return;
    for (const auto &m : mops)
      if (m.kind == Mop::Kind::Append) not_durable->push_back(m.element);
  };
  for (auto &[event, invoked, dur] : completions) {
    const auto lost = event.type == EventType::Ok && event.sequence != 0 &&
                      !dur && (power_cut || event.sequence > durable);
    if (lost) {
      event.type = EventType::Info;
      event.value = invoked;
      event.sequence = 0;
      event.error = power_cut ? "acknowledged, not durable at the power cut"
                              : "acknowledged, not durable at the kill";
      ++stats.downgraded;
    }
    if (event.type == EventType::Info) note_not_durable(invoked);
    if (event.type == EventType::Ok) ++stats.ok;
    if (event.type == EventType::Fail) ++stats.fail;
    events.push_back(std::move(event));
  }
  int sync_in_flight = 0;
  for (auto &[process, p] : pending) {
    Event e;
    e.index = next_index++;
    e.process = process;
    e.epoch = epoch;
    e.type = EventType::Info;
    e.time_ns = killed_at - start_ns;
    note_not_durable(p.mops);
    e.value = std::move(p.mops);
    e.error = "in flight at the kill";
    events.push_back(std::move(e));
    ++stats.in_flight;
    if (p.sync) ++sync_in_flight;
  }
  if (sync_in_flight >= 2) ++stats.group_kills;
  ++stats.epochs;
}

// The parent: kill_epochs children in turn on one directory, then a final
// read of every key after one more recovery, in this process.
auto run_kill(const RunOptions &o) -> int {
  const auto cfg = kill_config(o.seed);
  std::printf("isolation_history: config=%s seed=%llu threads=%d txns=%d "
              "backend=%s max_file_bytes=%llu sync%%=%d vacuum=%d "
              "kill_epochs=%d\n",
              mode_name(o.mode).data(),
              static_cast<unsigned long long>(o.seed), o.threads, o.txns,
              backend_name(cfg.backend).data(),
              static_cast<unsigned long long>(cfg.max_file_bytes),
              cfg.sync_percent, cfg.vacuum ? 1 : 0, o.kill_epochs);
  std::fflush(stdout);

  fs::remove_all(o.dir);
  std::mt19937_64 rng{o.seed + 7};
  const auto start_ns = monotonic_ns();
  std::vector<Event> events;
  std::uint64_t next_index = 0;
  std::int64_t max_key = -1;
  KillStats stats;
  auto child = o;
  for (int epoch = 0; epoch < o.kill_epochs; ++epoch) {
    // A process that saw an :info cannot be reused, so every child gets
    // fresh process ids, hence fresh element blocks, and the keys the last
    // one was on.
    child.process_base = epoch * o.threads;
    child.key_base = std::max<std::int64_t>(0, max_key + 1 - KeyPool::kSlots);
    // Somewhere between 10% and 95% of the child's transactions.
    const auto share = std::max(1, o.txns / o.kill_epochs);
    const auto kill_after =
        share / 10 +
        static_cast<int>(rng() % static_cast<std::uint64_t>(
                                     share * 85 / 100 + 1));
    const auto [stream, killed_at] = run_and_kill(child, epoch, kill_after);
    absorb_epoch(stream, epoch, killed_at, start_ns, events, next_index,
                 max_key, o.workload == Workload::Range, false, stats);
  }

  // One more recovery, then every key in one read-only transaction.
  {
    auto db = bytecask::DB::open(o.dir, db_options(cfg));
    Event inv;
    inv.process = o.kill_epochs * o.threads;
    inv.epoch = o.kill_epochs;
    auto snap = db.snapshot();
    append_final_read(events, next_index, start_ns, o.workload, snap,
                      inv);
  }

  write_history(o.out, std::move(events));
  auto summary = o.out;
  summary += ".kill.json";
  std::ofstream{summary} << std::format(
      R"({{"epochs":{},"in_flight":{},"downgraded":{},"group_kills":{},)"
      R"("ok":{},"fail":{}}})"
      "\n",
      stats.epochs, stats.in_flight, stats.downgraded, stats.group_kills,
      stats.ok, stats.fail);
  std::printf("  kill: epochs=%d ok=%lld fail=%lld in_flight=%d "
              "downgraded=%d group_kills=%d -> %s\n",
              stats.epochs, static_cast<long long>(stats.ok),
              static_cast<long long>(stats.fail), stats.in_flight,
              stats.downgraded, stats.group_kills, o.out.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// Chaos child and absorb (#232): the kill child's workload on a chaosfs mount,
// driven by tests/chaos/run_chaos.py --workload elle, which cuts the power,
// fails I/O and ends each process life; --absorb then turns the lives it
// saved into one history. See docs/isolation_checking_design.md, "Chaos".
//
// Beyond the kill child's lines the chaos child writes
//   O              the database opened
//   F <error>      DB::open threw; the child exits 4
//   R              resume() asks for a reopen (#240); the child exits 6
//   X ok | X fail <error>   the result of close() at a clean stop (SIGTERM)
// ---------------------------------------------------------------------------

std::atomic<bool> g_chaos_stop{false};

extern "C" void on_chaos_stop(int /*sig*/) { g_chaos_stop.store(true); }

auto one_line(std::string_view s) -> std::string {
  std::string out{s};
  std::ranges::replace(out, '\n', ' ');
  return out;
}

auto chaos_child_body(bytecask::DB &db, const RunOptions &o, const Config &cfg,
                      LineWriter &out) -> int {
  out.write("O\n");
  KeyPool keys{o.key_base};
  std::atomic<std::uint64_t> max_ok{0};
  std::atomic<bool> stop{false};

  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&] {
      std::mt19937_64 vrng{o.seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        try {
          std::ignore = db.vacuum({.fragmentation_threshold = threshold});
        } catch (const std::exception &) {
        }
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }
  std::jthread durable_thread{[&] {
    std::uint64_t last = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const auto d = db.durable_sequence(last + 1, std::chrono::milliseconds(5));
      if (d > last) {
        last = d;
        out.write(std::format("D {}\n", d));
      }
    }
  }};
  // Faults degrade the engine; the child recovers it as a service would. A
  // resume() that refuses and asks for a reopen ends the life: the next one
  // reopens the directory.
  std::jthread recovery_thread{[&] {
    std::mt19937_64 rrng{o.seed + 3};
    while (!stop.load(std::memory_order_relaxed)) {
      if (db.is_degraded()) {
        try {
          db.resume();
        } catch (const std::exception &e) {
          if (std::string_view{e.what()}.find("Reopen the database") !=
              std::string_view::npos) {
            out.write("R\n");
            std::_Exit(6);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10 + rrng() % 100));
          continue;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }};

  std::vector<std::jthread> writers;
  for (int t = 0; t < o.threads; ++t) {
    writers.emplace_back([&, t] {
      std::mt19937_64 rng{o.seed + 100 + static_cast<std::uint64_t>(t)};
      const auto process = o.process_base + t;
      RangeGenerator ranges{static_cast<std::uint64_t>(process)};
      // This thread's writes come from its own block (block_base above).
      std::int64_t next_element = block_base(process);
      while (!stop.load(std::memory_order_relaxed)) {
        if (db.is_degraded()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
        auto mops = make_txns(o.workload, rng, keys, ranges, next_element);
        const auto sync = static_cast<int>(rng() % 100) < cfg.sync_percent;
        TxnOutcome r;
        std::string line;
        bool started = false;
        // The range plan is settled before the invoke, so the invoke names
        // what the transaction ran. Both steps sit in one try: an injected
        // I/O fault during the scan is a failed transaction, not a fatal one.
        try {
          auto snap = o.workload == Workload::Range
                          ? plan_range_txn(db, o.mode, mops)
                          : std::optional<bytecask::Snapshot>{};
          line = std::format("I {} {} {}", process, monotonic_ns(),
                             sync ? 1 : 0);
          append_mops(line, mops, false);
          out.write(line + "\n");
          started = true;
          r = o.workload == Workload::Range
                  ? apply_range_txn(db, o.mode, mops, std::move(snap),
                                    {.sync = sync})
                  : execute_txn(db, o.mode, o.workload, mops, {.sync = sync});
        } catch (const std::exception &e) {
          r = {.type = EventType::Info, .sequence = 0, .error = e.what(),
               .durable = false};
        }
        // A fault before the invoke went out leaves no transaction to
        // complete: writing one would be a completion without an invoke.
        if (!started) continue;
        if (r.type == EventType::Ok) {
          auto seen = max_ok.load();
          while (r.sequence > seen && !max_ok.compare_exchange_weak(seen, r.sequence)) {
          }
        }
        line = std::format("C {} {} {} {} {}", process, monotonic_ns(),
                           event_type_name(r.type), r.sequence,
                           r.durable ? 1 : 0);
        append_mops(line, mops, r.type == EventType::Ok);
        out.write(line + "\n");
      }
    });
  }

  while (!g_chaos_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // A clean stop: the orchestrator lifted every hazard before SIGTERM. The
  // writers finish their transactions, then close() says whether everything
  // acknowledged is durable; if so, every sequence up to the last is.
  stop.store(true);
  writers.clear();
  recovery_thread = {};
  durable_thread = {};
  vacuum_thread = {};
  try {
    db.close();
    out.write(std::format("D {}\n", max_ok.load()));
    out.write("X ok\n");
  } catch (const std::exception &e) {
    out.write(std::format("X fail {}\n", one_line(e.what())));
  }
  return 0;
}

auto run_chaos_child(const RunOptions &o) -> int {
  std::signal(SIGTERM, on_chaos_stop);
  auto cfg = kill_config(o.seed);
  if (o.no_vacuum) cfg.vacuum = false;
  // chaosfs serves every read itself (FUSE direct_io), and the kernel refuses
  // a shared mapping of such a file: the Mmap backend cannot open there.
  if (cfg.backend == bytecask::IoBackend::Mmap)
    cfg.backend = bytecask::IoBackend::Pread;
  LineWriter out{o.child_fd};
  auto opened = false;
  try {
    // DB is neither copyable nor movable: it lives in this scope.
    auto db = bytecask::DB::open(o.dir, db_options(cfg));
    // The range workload's starting state is part of opening the database:
    // an I/O error here is a life that has no database, not a crash.
    if (o.workload == Workload::Range) seed_range_workload(db);
    opened = true;
    return chaos_child_body(db, o, cfg, out);
  } catch (const std::exception &e) {
    if (opened) throw;
    out.write(std::format("F {}\n", one_line(e.what())));
    return 4;
  }
}

// One life of a chaos episode, as the manifest lists it.
struct ChaosEpoch {
  int epoch{0};
  std::int64_t ended_at{0}; // CLOCK_MONOTONIC ns when the life was ended
  bool cache_lost{false};   // a power cut, or an eviction that dropped pages
  // An eviction dropped pages before the life's DB::open was seen to
  // complete: before the open made the earlier lives' writes durable (#437).
  bool lost_before_open{false};
  bool opened{false};       // the life's DB::open completed
  bool closed_ok{false};    // close() returned at a clean stop
  fs::path stream;
};

// The manifest: "start <ns>", then one line per life:
//   epoch <n> <ended_at_ns> <cache_lost> <lost_before_open> <opened> <closed_ok>
//         <stream path>
auto read_manifest(const fs::path &path, std::int64_t &start_ns)
    -> std::vector<ChaosEpoch> {
  std::ifstream in{path};
  std::vector<ChaosEpoch> epochs;
  std::string tag;
  while (in >> tag) {
    if (tag == "start") {
      in >> start_ns;
    } else if (tag == "epoch") {
      ChaosEpoch e;
      int lost = 0;
      int lost_before_open = 0;
      int opened = 0;
      int closed = 0;
      in >> e.epoch >> e.ended_at >> lost >> lost_before_open >> opened >>
          closed >> std::ws;
      std::string p;
      std::getline(in, p);
      e.cache_lost = lost != 0;
      e.lost_before_open = lost_before_open != 0;
      e.opened = opened != 0;
      e.closed_ok = closed != 0;
      e.stream = p;
      epochs.push_back(std::move(e));
    } else {
      throw std::runtime_error{std::format("manifest: unknown line {}", tag)};
    }
  }
  return epochs;
}

auto read_whole(const fs::path &p) -> std::string {
  std::ifstream in{p, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, {}};
}

// Builds one history from a chaos episode's lives, then reads every key once
// from --dir (a copy of the directory the last life left).
//
// The kill mode's rules hold per life: operations in flight at the end are
// :info, and so is every acknowledged write that is not durable, since the
// life's terminator (drawn only after it ended) may be a power cut. Further
// rules cover lives whose page cache lost data. Such a loss takes writes that
// were visible but never durable, so a read that returned one of them is
// :info: its result is not one the database had to keep. A write stays "not
// durable on the mount" until a later life's open completes (open makes what
// it trusts durable, #240), or a close() returns. An eviction while that open
// runs comes first: it takes what the earlier lives left (#437), and it may
// drop a write from an earlier life too, so a read of one is then judged only
// on how it ends.
auto run_absorb(const RunOptions &o) -> int {
  std::int64_t start_ns = 0;
  const auto epochs = read_manifest(o.absorb, start_ns);
  std::vector<Event> events;
  std::uint64_t next_index = 0;
  std::int64_t max_key = -1;
  KillStats stats;
  std::map<std::int64_t, std::vector<std::size_t>> readers_of;
  std::set<std::int64_t> unsynced;
  // :info elements from this life's operations: a power cut may take any of
  // them, so reads of them are judged only on how they end below.
  std::set<std::int64_t> undecided;
  std::int64_t lost_reads = 0;
  int cache_losses = 0;

  // Every :ok read of an element at risk, among events before `end`, becomes
  // :info: it saw a write the next recovery may not have.
  auto lose_reads_of = [&](const std::set<std::int64_t> &at_risk,
                           std::size_t end, std::string_view why) -> int {
    auto lost = 0;
    for (const auto e : at_risk) {
      const auto it = readers_of.find(e);
      if (it == readers_of.end()) continue;
      for (const auto i : it->second) {
        if (i >= end || events[i].type != EventType::Ok) continue;
        events[i].type = EventType::Info;
        events[i].sequence = 0;
        events[i].error = why;
        clear_reads(events[i].value);
        ++lost;
      }
    }
    return lost;
  };

  for (const auto &ep : epochs) {
    const auto first = events.size();
    std::vector<std::int64_t> not_durable;
    // A life may end in a power cut, so nothing it acknowledged is trusted
    // beyond its durable flag; a read of one becomes :info below.
    absorb_epoch(read_whole(ep.stream), ep.epoch, ep.ended_at, start_ns, events,
                 next_index, max_key, o.workload == Workload::Range, true, stats,
                 &not_durable);
    for (auto i = first; i < events.size(); ++i) {
      if (events[i].type != EventType::Ok) continue;
      for (const auto &m : events[i].value)
        if (m.read)
          for (const auto e : *m.read) readers_of[e].push_back(i);
    }
    // The open below, or the eviction before it, settles what the earlier
    // lives acknowledged but had not flushed. This life's reads came after
    // it: what they saw, the open recovered and made durable.
    if (ep.lost_before_open) {
      lost_reads += lose_reads_of(unsynced, first,
                                  "read a write the page cache lost");
      lost_reads += lose_reads_of(undecided, first,
                                  "read a write a power cut may take");
    }
    if (ep.opened) {
      unsynced.clear();
      undecided.clear();
    }
    unsynced.insert(not_durable.begin(), not_durable.end());
    if (ep.cache_lost) {
      ++cache_losses;
      lost_reads += lose_reads_of(unsynced, events.size(),
                                  "read a write the page cache lost");
      // Unless an eviction took the pages since, the next open found what
      // this life wrote and made it durable. One that took them may have
      // dropped an unflushed write too, which clear_reads accepts.
      for (const auto e : not_durable) {
        const auto it = readers_of.find(e);
        if (it == readers_of.end()) continue;
        for (const auto i : it->second) {
          if (i < first || events[i].type != EventType::Ok) continue;
          events[i].value.clear();
          events[i].error = "read a write a power cut may take";
          ++lost_reads;
        }
      }
    } else {
      undecided.insert(not_durable.begin(), not_durable.end());
    }
    if (ep.closed_ok) {
      unsynced.clear();
      undecided.clear();
    }
  }

  {
    auto db = bytecask::DB::open(o.dir, db_options(kill_config(o.seed)));
    Event inv;
    inv.process = static_cast<int>(epochs.size()) * o.threads;
    inv.epoch = static_cast<int>(epochs.size());
    auto snap = db.snapshot();
    append_final_read(events, next_index, start_ns, o.workload, snap,
                      inv);
  }

  write_history(o.out, std::move(events));
  auto summary = o.out;
  summary += ".chaos.json";
  std::ofstream{summary} << std::format(
      R"({{"epochs":{},"in_flight":{},"downgraded":{},"group_kills":{},)"
      R"("ok":{},"fail":{},"cache_losses":{},"lost_reads":{}}})"
      "\n",
      stats.epochs, stats.in_flight, stats.downgraded, stats.group_kills,
      stats.ok, stats.fail, cache_losses, lost_reads);
  return 0;
}

auto parse_mode(std::string_view s) -> Mode {
  if (s == "guarded") return Mode::Guarded;
  if (s == "unguarded") return Mode::Unguarded;
  if (s == "blind") return Mode::Blind;
  if (s == "presence") return Mode::Presence;
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
    } else if (a == "--no-retention") {
      o.no_retention = true;
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
    } else if (a == "--kill-epochs") {
      o.kill_epochs = std::stoi(std::string{next()});
    } else if (a == "--kill-child") {
      o.kill_child = true;
    } else if (a == "--chaos-child") {
      o.chaos_child = true;
    } else if (a == "--absorb") {
      o.absorb = next();
    } else if (a == "--fd") {
      o.child_fd = std::stoi(std::string{next()});
    } else if (a == "--process-base") {
      o.process_base = std::stoi(std::string{next()});
    } else if (a == "--key-base") {
      o.key_base = std::stoll(std::string{next()});
    } else if (a == "--workload") {
      const auto w = next();
      if (w == "append") {
        o.workload = Workload::Append;
      } else if (w == "range") {
        o.workload = Workload::Range;
      } else {
        throw std::runtime_error{std::format("unknown --workload {}", w)};
      }
    } else {
      throw std::runtime_error{std::format("unknown argument {}", a)};
    }
  }
  if (!have_config || (o.out.empty() && !o.kill_child && !o.chaos_child))
    throw std::runtime_error{"--config and --out are required"};
  if (o.kill_epochs < 0)
    throw std::runtime_error{"--kill-epochs must be >= 0"};
  if (o.kill_epochs > 0 && o.followers > 0)
    throw std::runtime_error{"--kill-epochs runs a single node"};
  if (o.threads < 1 || o.txns < 1)
    throw std::runtime_error{"--threads and --txns must be positive"};
  if (o.followers < 0 || o.follower_readers < 1)
    throw std::runtime_error{"--followers must be >= 0, --follower-readers >= 1"};
  // blind's lost appends are a list-append anomaly; the cluster checks are
  // list-shaped.
  if (o.workload == Workload::Range && (o.mode == Mode::Blind || o.followers > 0))
    throw std::runtime_error{
        "--workload range runs guarded, unguarded or presence, on one node"};
  // Kill and chaos lives of the presence configuration: a follow-up (#676).
  if (o.mode == Mode::Presence &&
      (o.workload != Workload::Range || o.kill_epochs > 0 || o.kill_child ||
       o.chaos_child || !o.absorb.empty()))
    throw std::runtime_error{
        "--config presence runs the range workload, in-process"};
  return o;
}

auto run(const RunOptions &o) -> int {
  auto cfg = config_for(o.seed);
  if (o.no_vacuum) cfg.vacuum = false;
  if (o.force_vacuum) cfg.vacuum = true;
  // The presence configuration is for plans whose answer rests on a write
  // whose flush fails, so it always runs the degrade nemesis.
  if (o.mode == Mode::Presence) cfg.degrade = true;
  if (o.no_degrade || o.topology) cfg.degrade = false;

  std::printf("isolation_history: config=%s workload=%s seed=%llu threads=%d "
              "txns=%d backend=%s max_file_bytes=%llu sync%%=%d vacuum=%d "
              "degrade=%d followers=%d lag=%d topology=%d promote_least=%d\n",
              mode_name(o.mode).data(), workload_name(o.workload).data(),
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
    if (o.workload == Workload::Range) seed_range_workload(nodes[0]->holder->db);
  }
  ClusterView view{node_count};
  auto node = [&](int id) -> Node & {
    return *nodes[static_cast<std::size_t>(id)];
  };

  const auto start = Clock::now();
  Recorder rec{start};
  KeyPool keys;
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
            // Under the gate, so a bootstrap between its manifest and its
            // install is counted as joining.
            std::lock_guard<std::mutex> vg{vacuum_gate};
            const auto retain_after =
                retention_point(nodes, view, ln.id, !o.no_retention);
            if (ln.holder->db.vacuum({.fragmentation_threshold = threshold,
                                      .retain_after = retain_after}))
              totals.vacuums.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (const std::exception &) {
          // Vacuum refuses to run on a degraded engine; the nemesis resumes it.
        }
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }

  // Presence: hold the failing flush between recording its error and
  // publishing the degrade, as a descheduled flush thread would. Writers that
  // pass admission meanwhile append behind the failed write, and a plan that
  // appends nothing can answer from their entries (#672). Set before any
  // thread starts and cleared once they stop.
  if (o.mode == Mode::Presence && cfg.degrade)
    node(0).holder->db.test_before_degrade_publish_ = [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

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
      const auto presence = o.mode == Mode::Presence;
      // One generator for the thread's whole run: a value names its write,
      // and a fresh generator per degrade cycle would number them again.
      RangeGenerator ranges{static_cast<std::uint64_t>(process)};
      std::int64_t next_element = block_base(process);
      while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            presence ? 1 + nrng() % 20 : 20 + nrng() % 180));
        if (stop.load(std::memory_order_relaxed)) break;
        // Retry until this thread holds the flush role for its own write.
        for (int attempt = 0; attempt < 50 && !db.is_degraded(); ++attempt) {
          auto mops = make_txns(o.workload, nrng, keys, ranges, next_element);
          if (std::ranges::none_of(mops, [](const Mop &m) {
                return m.kind == Mop::Kind::Append || m.kind == Mop::Kind::Put;
              }))
            continue;
          store_max(last_acked,
                    run_one(db, o.mode, o.workload, {.process = process}, rec,
                            events, std::move(mops),
                            {.sync = true, .solo = true}, true, totals));
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
            node(i).held_epoch.store(v.epoch, std::memory_order_release);
          });
          node(i).joining.store(false, std::memory_order_release);
        }
        replicate(nodes, node(i), view, o.lag, !o.no_retention, brng(),
                  repl_stop, cluster);
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
            for (int j = 0; j < k; ++j) {
              Mop m;
              m.key = keys.pick(rrng, false);
              m.to = m.key;
              mops.push_back(m);
            }
            Placement pl{.process = process, .node = i, .epoch = v.epoch,
                         .life = n.life.load(std::memory_order_acquire),
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
            run_one(db, Mode::Guarded, Workload::Append, pl, rec, events,
                    std::move(mops), {}, false, totals);
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
  // re-bootstrap of any node out of service: one an unplanned promotion
  // abandoned, or one the engine refused as a fork.
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
        node(i).held_epoch.store(w.epoch, std::memory_order_release);
      });
      node(i).joining.store(false, std::memory_order_release);
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
        // promote_least exists to fork a follower ahead of the new leader,
        // and only an unplanned promotion can; a short run must not spend
        // its few events on planned transfers.
        if (!o.promote_least && orng() % 2 == 0) {
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
        RangeGenerator ranges{static_cast<std::uint64_t>(p)};
        std::int64_t next_element = block_base(p);
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
            auto sync = static_cast<int>(crng() % 100) < cfg.sync_percent;
            // Presence: half the transactions are snapshot-less, with
            // sync = false so a guard-only plan does not drive its own flush.
            const auto presence =
                o.mode == Mode::Presence && crng() % 2 == 0;
            if (presence) sync = false;
            store_max(last_acked,
                      run_one(ln.holder->db, o.mode, o.workload,
                              {.process = p, .node = v.leader,
                               .epoch = v.epoch,
                               .life = ln.life.load(std::memory_order_acquire),
                               .role = Role::Leader, .wait_seq = 0},
                              rec, events,
                              presence ? ranges.presence(crng)
                                       : make_txns(o.workload, crng, keys,
                                                   ranges, next_element),
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
  node(0).holder->db.test_before_degrade_publish_ = nullptr;
  const auto final_leader = view.get().leader;
  auto &leader_db = node(final_leader).holder->db;
  if (leader_db.is_degraded()) leader_db.resume();

  // Convergence: a sync write makes every leader entry durable, so
  // changes_since can deliver all of it; every other node must then reach
  // the leader's durable sequence, and have pulled from the final leader in
  // the final epoch, since a node ahead of it on another history reaches the
  // sequence without holding the writes. A node the engine refuses during
  // the wait is re-bootstrapped and the wait repeated.
  if (o.followers > 0) {
    leader_db.put({.sync = true}, as_view("fence"), as_view(""));
    const auto target = leader_db.durable_sequence();
    for (int pass = 0;; ++pass) {
      if (o.topology) rebootstrap_abandoned();
      const auto epoch = view.get().epoch;
      for (int i = 0; i < node_count; ++i) {
        if (i == final_leader) continue;
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (durable_of(node(i)).value_or(0) < target ||
               node(i).pulled_epoch.load(std::memory_order_acquire) != epoch) {
          // Refused as a fork meanwhile: the next pass re-bootstraps it.
          if (!view.get().serving[static_cast<std::size_t>(i)]) break;
          if (Clock::now() >= deadline) {
            // What the leader would send it: the first entries past where it
            // stands, to tell a stream that is empty from one that is refused.
            const auto at_seq = durable_of(node(i)).value_or(0);
            std::string head;
            std::int64_t count = 0;
            auto snap = leader_db.snapshot();
            for (const auto &e :
                 leader_db.changes_since(snap, at_seq).entries) {
              if (count < 8)
                head += std::format(" {}:{}", e.sequence,
                                    static_cast<int>(e.entry_type));
              ++count;
            }
            auto v = view.get();
            cluster.error(std::format(
                "node {}: stuck at durable sequence {} below the leader's {} "
                "(node {}) after 20s; changes_since yields {} entries:{}; "
                "source {} serving {} stall {} wanted {} leader-wanted {} "
                "pulled from {} in epoch {} of {}",
                i, at_seq, target, final_leader, count, head,
                at(v.source, i),
                v.serving[static_cast<std::size_t>(i)] ? 1 : 0,
                node(i).repl_stall.load(), node(i).exclusive_wanted.load(),
                node(final_leader).exclusive_wanted.load(),
                node(i).pulled_src.load(), node(i).pulled_epoch.load(), epoch));
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      const auto serving = view.get().serving;
      if (!o.topology || pass >= node_count ||
          std::ranges::all_of(serving, [](bool b) { return b; }))
        break;
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
    const auto key_count =
        o.workload == Workload::Range ? kRangeKeys : keys.key_count();
    for (std::int64_t k0 = 0; k0 < key_count; k0 += 32) {
      std::vector<Mop> mops;
      for (auto k = k0; k < std::min(k0 + 32, key_count); ++k) {
        Mop m;
        m.key = k;
        m.to = k;
        mops.push_back(m);
      }
      run_one(n.holder->db, o.followers > 0 ? Mode::Guarded : o.mode,
              o.workload,
              {.process = process, .node = i, .epoch = final_epoch,
               .life = n.life.load(std::memory_order_acquire),
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
                "ingest_errors=%d forks_detected=%d restarts=%d "
                "duplicates=%d lag_pauses=%d follower_vacuums=%d "
                "session_reads=%d errors=%zu final_leader=%d\n",
                cluster.bootstraps.size(), cluster.events.size(),
                cluster.ingests.load(), cluster.ingest_errors.load(),
                cluster.forks_detected.load(), cluster.restarts.load(),
                cluster.duplicates.load(),
                cluster.lag_pauses.load(), cluster.follower_vacuums.load(),
                cluster.session_reads.load(), cluster.errors.size(),
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
              static_cast<long long>(o.workload == Workload::Range
                                         ? kRangeKeys
                                         : keys.key_count()),
              totals.degrades.load(),
              totals.vacuums.load(), o.out.c_str());
  return 0;
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    const auto o = parse_args(argc, argv);
    if (o.kill_child) return run_kill_child(o);
    if (o.chaos_child) return run_chaos_child(o);
    if (!o.absorb.empty()) return run_absorb(o);
    if (o.kill_epochs > 0) return run_kill(o);
    return run(o);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "isolation_history: %s\n", e.what());
    return 2;
  }
}
