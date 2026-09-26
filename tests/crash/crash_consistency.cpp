// SPDX-License-Identifier: MIT
// Crash-consistency harness: SIGKILL a writer process at a random point and
// verify the durable prefix on reopen. See docs/correctness_validation.md,
// "Process-crash harness".
//
// One binary, two roles. The parent re-executes itself as a child that opens
// the database, runs a random single-writer workload (plus a vacuum thread),
// and streams every operation to the parent over a pipe: an Intent frame
// before the call, a Commit or Abort frame after it returns. The parent
// SIGKILLs the child after a random delay, then reopens copies of the
// directory and checks the recovered contents against its model:
//
//   - The recovered key/value set equals the model after applying some prefix
//     of the child's operations, in commit order. Batches are one operation,
//     so this also checks batch atomicity. No value from nowhere, no rollback.
//   - That prefix covers every operation at or below the durable watermark:
//     the highest of every durable_sequence() the child reported and every
//     CommitResult with durable == true.
//   - DB::open with default options (fail_recovery_on_crc_errors = true)
//     succeeds, and serial and parallel recovery agree on contents,
//     file_stats and the key count.
//
// The next child reopens the killed directory itself, so every iteration
// after the first also starts from a crash. The directory is wiped every
// --reset-every iterations to bound its size.
//
// The kernel page cache survives SIGKILL, so this checks the process-crash
// contract, not power loss.
//
// Usage:
//   crash_consistency [--iterations N] [--seed S] [--max-delay-ms MS]
//                     [--reset-every N] [--dir PATH] [--keep] [--verbose]
//                     [--no-vacuum]

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

import bytecask;

extern char **environ; // NOLINT(readability-redundant-declaration)

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Workload model
// ---------------------------------------------------------------------------

constexpr int kKeySpace = 512;
constexpr std::size_t kMaxValueBytes = 600;

enum class OpKind : std::uint8_t { Put = 1, Del = 2, DelRange = 3, Batch = 4 };
enum class ItemKind : std::uint8_t { Put = 1, Del = 2 };
enum class GuardKind : std::uint8_t { Present = 1, Absent = 2 };

struct BatchItem {
  ItemKind kind{ItemKind::Put};
  std::string key;
  std::string value;
};

struct Guard {
  GuardKind kind{GuardKind::Present};
  std::string key;
};

struct Op {
  OpKind kind{OpKind::Put};
  bool sync{false};
  std::string key;   // Put, Del; range start for DelRange
  std::string value; // Put; range end (exclusive) for DelRange
  std::vector<BatchItem> items;
  std::vector<Guard> guards;
};

using State = std::map<std::string, std::string>;

auto key_name(int i) -> std::string { return std::format("k{:04}", i); }

// Applies op to state the way the engine does. Returns false when the op is a
// no-op the engine reports as nullopt (del of an absent key, failed guard).
auto apply(State &s, const Op &op, std::vector<std::string> *touched) -> bool {
  auto touch = [&](const std::string &k) {
    if (touched) touched->push_back(k);
  };
  switch (op.kind) {
  case OpKind::Put:
    s[op.key] = op.value;
    touch(op.key);
    return true;
  case OpKind::Del:
    if (s.erase(op.key) == 0) return false;
    touch(op.key);
    return true;
  case OpKind::DelRange: {
    auto first = s.lower_bound(op.key);
    auto last = s.lower_bound(op.value);
    for (auto it = first; it != last; ++it) touch(it->first);
    s.erase(first, last);
    return true;
  }
  case OpKind::Batch:
    for (const auto &g : op.guards) {
      const bool present = s.contains(g.key);
      if (present != (g.kind == GuardKind::Present)) return false;
    }
    for (const auto &item : op.items) {
      switch (item.kind) {
      case ItemKind::Put:
        s[item.key] = item.value;
        break;
      case ItemKind::Del:
        s.erase(item.key);
        break;
      }
      touch(item.key);
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Per-iteration configuration, derived from the iteration seed so parent and
// child agree without passing more than the seed.
// ---------------------------------------------------------------------------

struct Config {
  bytecask::IoBackend backend{bytecask::IoBackend::Pread};
  std::uint64_t max_file_bytes{0};
  int sync_percent{0};
  bool vacuum{false};
  unsigned recovery_threads{1};
};

auto config_for(std::uint64_t seed) -> Config {
  std::mt19937_64 rng{seed ^ 0x9e3779b97f4a7c15ULL};
  Config c;
  constexpr std::array backends{bytecask::IoBackend::Pread,
                                bytecask::IoBackend::Mmap,
                                bytecask::IoBackend::BufferPool};
  c.backend = backends[rng() % backends.size()];
  // Small files rotate often, so rotation, sealing and the background hint
  // worker are in flight when the kill lands.
  constexpr std::array sizes{std::uint64_t{16} << 10, std::uint64_t{64} << 10,
                             std::uint64_t{256} << 10, std::uint64_t{1} << 20};
  c.max_file_bytes = sizes[rng() % sizes.size()];
  constexpr std::array sync_percents{0, 5, 50, 100};
  c.sync_percent = sync_percents[rng() % sync_percents.size()];
  c.vacuum = rng() % 4 != 0;
  c.recovery_threads = 1 + static_cast<unsigned>(rng() % 4);
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
  o.recovery_threads = c.recovery_threads;
  o.io_backend = c.backend;
  if (c.backend == bytecask::IoBackend::BufferPool)
    o.buffer_pool.capacity_bytes = 4 * c.max_file_bytes + (1 << 20);
  return o;
}

auto as_view(std::string_view s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

auto as_string(std::span<const std::byte> b) -> std::string {
  std::string s(b.size(), '\0');
  if (!b.empty()) std::memcpy(s.data(), b.data(), b.size());
  return s;
}

// ---------------------------------------------------------------------------
// Wire format: [u32 payload length][payload]. The payload starts with a
// FrameType byte. A frame cut short by the kill is discarded.
// ---------------------------------------------------------------------------

enum class FrameType : std::uint8_t {
  Opened = 1,    // u64 durable_sequence at open, u64 keydir_keys at open
  Intent = 2,    // encoded Op, written before the call
  Commit = 3,    // u64 sequence, u8 durable
  Abort = 4,     // call returned nullopt
  Watermark = 5, // u64 durable_sequence()
};

class Writer {
public:
  void u8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void str(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    buf_.append(s);
  }
  [[nodiscard]] auto data() const -> const std::string & { return buf_; }

private:
  std::string buf_;
};

class Reader {
public:
  explicit Reader(std::string_view s) : s_{s} {}
  auto u8() -> std::uint8_t {
    need(1);
    return static_cast<std::uint8_t>(s_[pos_++]);
  }
  auto u32() -> std::uint32_t {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t{u8()} << (8 * i);
    return v;
  }
  auto u64() -> std::uint64_t {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t{u8()} << (8 * i);
    return v;
  }
  auto str() -> std::string {
    const auto n = u32();
    need(n);
    std::string out{s_.substr(pos_, n)};
    pos_ += n;
    return out;
  }

private:
  void need(std::size_t n) const {
    if (pos_ + n > s_.size()) throw std::runtime_error{"truncated frame"};
  }
  std::string_view s_;
  std::size_t pos_{0};
};

void encode_op(Writer &w, const Op &op) {
  w.u8(static_cast<std::uint8_t>(op.kind));
  w.u8(op.sync ? 1 : 0);
  w.str(op.key);
  w.str(op.value);
  w.u32(static_cast<std::uint32_t>(op.items.size()));
  for (const auto &item : op.items) {
    w.u8(static_cast<std::uint8_t>(item.kind));
    w.str(item.key);
    w.str(item.value);
  }
  w.u32(static_cast<std::uint32_t>(op.guards.size()));
  for (const auto &g : op.guards) {
    w.u8(static_cast<std::uint8_t>(g.kind));
    w.str(g.key);
  }
}

auto decode_op(Reader &r) -> Op {
  Op op;
  op.kind = static_cast<OpKind>(r.u8());
  op.sync = r.u8() != 0;
  op.key = r.str();
  op.value = r.str();
  const auto n_items = r.u32();
  for (std::uint32_t i = 0; i < n_items; ++i) {
    BatchItem item;
    item.kind = static_cast<ItemKind>(r.u8());
    item.key = r.str();
    item.value = r.str();
    op.items.push_back(std::move(item));
  }
  const auto n_guards = r.u32();
  for (std::uint32_t i = 0; i < n_guards; ++i) {
    Guard g;
    g.kind = static_cast<GuardKind>(r.u8());
    g.key = r.str();
    op.guards.push_back(std::move(g));
  }
  return op;
}

void write_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      // The parent is gone; nothing left to report to.
      std::_Exit(3);
    }
    bytes.remove_prefix(static_cast<std::size_t>(n));
  }
}

void send_frame(int fd, const Writer &payload) {
  Writer frame;
  frame.u32(static_cast<std::uint32_t>(payload.data().size()));
  write_all(fd, frame.data());
  write_all(fd, payload.data());
}

// ---------------------------------------------------------------------------
// Child
// ---------------------------------------------------------------------------

auto random_op(std::mt19937_64 &rng, const Config &cfg, std::uint64_t seed,
               std::uint64_t &value_id) -> Op {
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };
  auto value = [&] {
    // Unique per write, so a recovered value names exactly one operation:
    // the child's seed and the write's ordinal within that child.
    auto v = std::format("{:016x}.{}:", seed, value_id++);
    v.resize(v.size() + static_cast<std::size_t>(pick(kMaxValueBytes)), 'x');
    return v;
  };
  Op op;
  op.sync = pick(100) < cfg.sync_percent;
  const auto r = pick(100);
  if (r < 55) {
    op.kind = OpKind::Put;
    op.key = key_name(pick(kKeySpace));
    op.value = value();
  } else if (r < 75) {
    op.kind = OpKind::Del;
    op.key = key_name(pick(kKeySpace));
  } else if (r < 80) {
    op.kind = OpKind::DelRange;
    const auto from = pick(kKeySpace);
    op.key = key_name(from);
    op.value = key_name(from + 1 + pick(16));
  } else {
    op.kind = OpKind::Batch;
    // Distinct keys: the model does not need to decide in-batch ordering.
    std::vector<int> keys;
    const auto n = 2 + pick(8);
    while (std::ssize(keys) < n) {
      const auto k = pick(kKeySpace);
      if (std::ranges::find(keys, k) == keys.end()) keys.push_back(k);
    }
    for (const auto k : keys) {
      BatchItem item;
      item.key = key_name(k);
      if (pick(4) == 0) {
        item.kind = ItemKind::Del;
      } else {
        item.kind = ItemKind::Put;
        item.value = value();
      }
      op.items.push_back(std::move(item));
    }
    if (pick(3) == 0) {
      Guard g;
      g.kind = pick(2) == 0 ? GuardKind::Present : GuardKind::Absent;
      g.key = key_name(pick(kKeySpace));
      op.guards.push_back(std::move(g));
    }
  }
  return op;
}

// Runs op against the engine. nullopt when the engine reported a no-op.
auto execute(bytecask::DB &db, const Op &op)
    -> std::optional<bytecask::CommitResult> {
  const bytecask::WriteOptions wo{.sync = op.sync};
  switch (op.kind) {
  case OpKind::Put:
    return db.put(wo, as_view(op.key), as_view(op.value));
  case OpKind::Del:
    return db.del(wo, as_view(op.key));
  case OpKind::DelRange:
    return db.del_range(wo, as_view(op.key), as_view(op.value));
  case OpKind::Batch: {
    bytecask::WritePlan plan;
    for (const auto &g : op.guards) {
      switch (g.kind) {
      case GuardKind::Present:
        plan.ensure_present(as_view(g.key));
        break;
      case GuardKind::Absent:
        plan.ensure_absent(as_view(g.key));
        break;
      }
    }
    for (const auto &item : op.items) {
      switch (item.kind) {
      case ItemKind::Put:
        plan.put(as_view(item.key), as_view(item.value));
        break;
      case ItemKind::Del:
        plan.del(as_view(item.key));
        break;
      }
    }
    return db.apply_batch(wo, std::move(plan));
  }
  }
  return std::nullopt;
}

auto run_child(const fs::path &dir, std::uint64_t seed, bool no_vacuum, int fd)
    -> int {
  auto cfg = config_for(seed);
  if (no_vacuum) cfg.vacuum = false;
  std::mt19937_64 rng{seed};
  auto db = bytecask::DB::open(dir, db_options(cfg));
  {
    Writer w;
    w.u8(static_cast<std::uint8_t>(FrameType::Opened));
    w.u64(db.durable_sequence());
    w.u64(static_cast<std::uint64_t>(db.stats().at("bytecask.keydir_keys")));
    send_frame(fd, w);
  }

  std::atomic<bool> stop{false};
  std::jthread vacuum_thread;
  if (cfg.vacuum) {
    vacuum_thread = std::jthread{[&db, &stop, seed] {
      std::mt19937_64 vrng{seed + 1};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto threshold = static_cast<double>(vrng() % 60) / 100.0;
        (void)db.vacuum({.fragmentation_threshold = threshold});
        std::this_thread::sleep_for(std::chrono::microseconds(vrng() % 5000));
      }
    }};
  }

  std::uint64_t value_id = 0;
  // Bounded so a slow kill cannot fill the disk; the child then idles until
  // the parent kills it.
  constexpr int kMaxOps = 200'000;
  for (int i = 0; i < kMaxOps; ++i) {
    const auto op = random_op(rng, cfg, seed, value_id);
    {
      Writer w;
      w.u8(static_cast<std::uint8_t>(FrameType::Intent));
      encode_op(w, op);
      send_frame(fd, w);
    }
    const auto result = execute(db, op);
    {
      Writer w;
      if (result) {
        w.u8(static_cast<std::uint8_t>(FrameType::Commit));
        w.u64(result->sequence);
        w.u8(result->durable ? 1 : 0);
      } else {
        w.u8(static_cast<std::uint8_t>(FrameType::Abort));
      }
      send_frame(fd, w);
    }
    if (rng() % 16 == 0) {
      Writer w;
      w.u8(static_cast<std::uint8_t>(FrameType::Watermark));
      w.u64(db.durable_sequence());
      send_frame(fd, w);
    }
  }
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ---------------------------------------------------------------------------
// Parent
// ---------------------------------------------------------------------------

struct OpRecord {
  Op op;
  enum class Outcome { InFlight, Committed, Aborted } outcome{Outcome::InFlight};
  std::uint64_t sequence{0};
  bool durable{false};
};

struct History {
  bool opened{false};
  std::uint64_t opened_durable{0};
  std::uint64_t opened_keys{0};
  std::vector<OpRecord> ops;
  std::uint64_t watermark{0};
};

auto parse_history(std::string_view stream) -> History {
  History h;
  std::size_t pos = 0;
  while (stream.size() - pos >= 4) {
    Reader len_reader{stream.substr(pos, 4)};
    const auto len = len_reader.u32();
    if (stream.size() - pos - 4 < len) break; // cut short by the kill
    Reader r{stream.substr(pos + 4, len)};
    pos += 4 + len;
    switch (static_cast<FrameType>(r.u8())) {
    case FrameType::Opened:
      h.opened = true;
      h.opened_durable = r.u64();
      h.opened_keys = r.u64();
      h.watermark = h.opened_durable;
      break;
    case FrameType::Intent:
      h.ops.push_back({.op = decode_op(r)});
      break;
    case FrameType::Commit:
      h.ops.back().outcome = OpRecord::Outcome::Committed;
      h.ops.back().sequence = r.u64();
      h.ops.back().durable = r.u8() != 0;
      if (h.ops.back().durable)
        h.watermark = std::max(h.watermark, h.ops.back().sequence);
      break;
    case FrameType::Abort:
      h.ops.back().outcome = OpRecord::Outcome::Aborted;
      break;
    case FrameType::Watermark:
      h.watermark = std::max(h.watermark, r.u64());
      break;
    }
  }
  return h;
}

struct Failure {
  std::string what;
};

struct Recovered {
  State contents;
  std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                         std::uint64_t>>
      file_stats;
  std::int64_t keydir_keys{0};
  std::uint64_t durable_sequence{0};
};

auto open_and_collect(const fs::path &dir, const Config &cfg, unsigned threads,
                      bool fail_on_crc) -> Recovered {
  auto opts = db_options(cfg);
  opts.recovery_threads = threads;
  opts.fail_recovery_on_crc_errors = fail_on_crc;
  auto db = bytecask::DB::open(dir, opts);
  Recovered r;
  for (auto &entry : db.iter_from({})) {
    r.contents.emplace(as_string(entry.key), as_string(entry.value));
  }
  for (const auto &[id, fs] : db.file_stats()) {
    r.file_stats.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                              fs.max_sequence);
  }
  std::ranges::sort(r.file_stats);
  r.keydir_keys = db.stats().at("bytecask.keydir_keys");
  r.durable_sequence = db.durable_sequence();
  return r;
}

struct PrefixResult {
  std::size_t applied{0};   // operations in the matching prefix
  std::size_t committed{0}; // operations whose call returned
};

// A value's identity: "<child seed>.<ordinal>", without the padding.
auto brief(const std::string &v) -> std::string { return v.substr(0, v.find(':')); }

auto touches(const Op &op, const std::string &k) -> bool {
  switch (op.kind) {
  case OpKind::Put:
  case OpKind::Del:
    return op.key == k;
  case OpKind::DelRange:
    return op.key <= k && k < op.value;
  case OpKind::Batch:
    return std::ranges::any_of(op.items, [&](const auto &i) { return i.key == k; }) ||
           std::ranges::any_of(op.guards, [&](const auto &g) { return g.key == k; });
  }
  return false;
}

// One line per operation. With only_key set, batch items for other keys are
// left out.
auto describe(const OpRecord &rec, const std::string *only_key) -> std::string {
  const auto *outcome = rec.outcome == OpRecord::Outcome::Committed ? "committed"
                        : rec.outcome == OpRecord::Outcome::Aborted ? "aborted"
                                                                    : "in-flight";
  auto line = std::format("seq {:>8} {:<9} sync={:d} durable={:d} ", rec.sequence,
                          outcome, rec.op.sync, rec.durable);
  switch (rec.op.kind) {
  case OpKind::Put:
    line += std::format("put {} {}", rec.op.key, brief(rec.op.value));
    break;
  case OpKind::Del:
    line += std::format("del {}", rec.op.key);
    break;
  case OpKind::DelRange:
    line += std::format("del_range [{}, {})", rec.op.key, rec.op.value);
    break;
  case OpKind::Batch:
    line += "batch";
    for (const auto &g : rec.op.guards)
      if (!only_key || g.key == *only_key)
        line += std::format(" ensure_{}({})",
                            g.kind == GuardKind::Present ? "present" : "absent",
                            g.key);
    for (const auto &i : rec.op.items)
      if (!only_key || i.key == *only_key)
        line += i.kind == ItemKind::Put
                    ? std::format(" put {} {}", i.key, brief(i.value))
                    : std::format(" del {}", i.key);
    break;
  }
  return line;
}

// Finds the prefix of `ops` (in commit order, aborted ops skipped) whose
// application to `base` yields `recovered`, starting at the shortest prefix
// that covers the watermark. Throws Failure when none does.
auto match_prefix(const State &base, const History &h,
                  const State &recovered) -> PrefixResult {
  std::vector<const OpRecord *> applied_ops;
  std::uint64_t last_seq = h.opened_durable;
  State running = base;
  for (const auto &rec : h.ops) {
    switch (rec.outcome) {
    case OpRecord::Outcome::Committed: {
      if (rec.sequence <= last_seq)
        throw Failure{std::format("sequence {} not above previous {}",
                                  rec.sequence, last_seq)};
      last_seq = rec.sequence;
      // The engine said it wrote something; the model must agree it was not
      // a no-op. This is checked on the live history, before any crash.
      if (!apply(running, rec.op, nullptr) && rec.op.kind != OpKind::DelRange)
        throw Failure{std::format(
            "seq {}: engine committed an op the model says is a no-op",
            rec.sequence)};
      applied_ops.push_back(&rec);
      break;
    }
    case OpRecord::Outcome::Aborted: {
      auto copy = running;
      if (apply(copy, rec.op, nullptr))
        throw Failure{"engine returned nullopt for an op the model says "
                      "should commit"};
      break;
    }
    case OpRecord::Outcome::InFlight:
      // Only the last op can be in flight: the child has a single writer.
      applied_ops.push_back(&rec);
      break;
    }
  }

  std::size_t min_prefix = 0;
  std::size_t committed = 0;
  for (std::size_t i = 0; i < applied_ops.size(); ++i) {
    const auto *rec = applied_ops[i];
    if (rec->outcome != OpRecord::Outcome::Committed) continue;
    ++committed;
    if (rec->sequence <= h.watermark) min_prefix = i + 1;
  }

  State model = base;
  for (std::size_t i = 0; i < min_prefix; ++i)
    (void)apply(model, applied_ops[i]->op, nullptr);

  auto differs = [&](const std::string &k) {
    const auto m = model.find(k);
    const auto r = recovered.find(k);
    if (m == model.end() || r == recovered.end())
      return (m == model.end()) != (r == recovered.end());
    return m->second != r->second;
  };
  std::map<std::string, bool> mismatched;
  std::size_t mismatches = 0;
  auto recheck = [&](const std::string &k) {
    const auto now = differs(k);
    auto &was = mismatched[k];
    if (was != now) {
      mismatches = now ? mismatches + 1 : mismatches - 1;
      was = now;
    }
  };
  for (const auto &[k, v] : model) recheck(k);
  for (const auto &[k, v] : recovered) recheck(k);

  // Operations that change nothing (a del_range over no keys, a batch of
  // deletes on absent keys) make several prefixes match. The longest one is
  // reported, so `applied` does not undercount what survived.
  std::optional<std::size_t> longest;
  std::size_t best = mismatches;
  std::size_t best_at = min_prefix;
  for (std::size_t k = min_prefix;; ++k) {
    if (mismatches == 0) longest = k;
    if (mismatches < best) {
      best = mismatches;
      best_at = k;
    }
    if (k == applied_ops.size()) break;
    std::vector<std::string> touched;
    (void)apply(model, applied_ops[k]->op, &touched);
    for (const auto &key : touched) recheck(key);
  }
  if (longest) return {.applied = *longest, .committed = committed};

  // Describe the closest prefix to make the report actionable.
  State closest = base;
  for (std::size_t i = 0; i < best_at; ++i)
    (void)apply(closest, applied_ops[i]->op, nullptr);
  std::string sample;
  int shown = 0;
  auto show = [&](const std::string &k) {
    if (shown++ >= 5) return;
    const auto m = closest.find(k);
    const auto r = recovered.find(k);
    const auto b = base.find(k);
    sample += std::format("\n    {}: model={} recovered={} (at open: {})", k,
                          m == closest.end() ? "<absent>" : brief(m->second),
                          r == recovered.end() ? "<absent>" : brief(r->second),
                          b == base.end() ? "<absent>" : brief(b->second));
    for (const auto &rec : h.ops)
      if (touches(rec.op, k)) sample += "\n      " + describe(rec, &k);
  };
  std::map<std::string, bool> keys;
  for (const auto &[k, v] : closest) keys[k];
  for (const auto &[k, v] : recovered) keys[k];
  for (const auto &[k, unused] : keys) {
    const auto m = closest.find(k);
    const auto r = recovered.find(k);
    if ((m == closest.end()) != (r == recovered.end()) ||
        (m != closest.end() && m->second != r->second))
      show(k);
  }
  throw Failure{std::format(
      "recovered state matches no prefix of the history at or above the "
      "durable watermark {} (ops {}, watermark prefix {}, closest prefix {} "
      "with {} mismatched keys):{}",
      h.watermark, applied_ops.size(), min_prefix, best_at, best, sample)};
}

auto copy_dir(const fs::path &from, const fs::path &to) -> void {
  fs::remove_all(to);
  // A kill before DB::open created the directory leaves nothing to copy.
  if (!fs::exists(from)) {
    fs::create_directories(to);
    return;
  }
  fs::copy(from, to, fs::copy_options::recursive);
}

struct RunOptions {
  int iterations{200};
  std::uint64_t seed{0};
  int max_delay_ms{1500};
  int reset_every{25};
  fs::path dir;
  bool keep{false};
  bool verbose{false};
  bool no_vacuum{false};
};

struct Totals {
  std::size_t committed{0};
  std::size_t durable_needed{0};
  std::size_t lost_after_return{0};
  int killed_before_open{0};
};

auto spawn_child(const fs::path &self, const fs::path &dir, std::uint64_t seed,
                 bool no_vacuum, int write_fd) -> pid_t {
  const auto fd_arg = std::to_string(write_fd);
  const auto seed_arg = std::to_string(seed);
  const auto dir_arg = dir.string();
  const auto self_arg = self.string();
  std::vector<char *> argv{const_cast<char *>(self_arg.c_str()),
                           const_cast<char *>("--child"),
                           const_cast<char *>(dir_arg.c_str()),
                           const_cast<char *>(seed_arg.c_str()),
                           const_cast<char *>(no_vacuum ? "1" : "0"),
                           const_cast<char *>(fd_arg.c_str()), nullptr};
  pid_t pid = 0;
  if (const auto rc = posix_spawn(&pid, self_arg.c_str(), nullptr, nullptr,
                                  argv.data(), environ);
      rc != 0) {
    throw std::system_error{rc, std::generic_category(), "posix_spawn"};
  }
  return pid;
}

// Runs one child until `delay`, SIGKILLs it, and returns everything it
// streamed. Throws Failure if the child exited on its own.
auto run_and_kill(const fs::path &self, const fs::path &dir, std::uint64_t seed,
                  bool no_vacuum, std::chrono::microseconds delay)
    -> std::string {
  int fds[2];
  if (::pipe(fds) != 0)
    throw std::system_error{errno, std::generic_category(), "pipe"};
  const auto pid = spawn_child(self, dir, seed, no_vacuum, fds[1]);
  ::close(fds[1]);

  std::string stream;
  char buf[1 << 16];
  const auto deadline = Clock::now() + delay;
  bool killed = false;
  bool eof = false;
  while (!eof) {
    int timeout_ms = -1;
    if (!killed) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now());
      if (left.count() <= 0) {
        ::kill(pid, SIGKILL);
        killed = true;
      } else {
        timeout_ms = static_cast<int>(left.count());
      }
    }
    pollfd pfd{.fd = fds[0], .events = POLLIN, .revents = 0};
    const auto ready = ::poll(&pfd, 1, killed ? -1 : std::max(timeout_ms, 1));
    if (ready < 0 && errno != EINTR)
      throw std::system_error{errno, std::generic_category(), "poll"};
    if (ready <= 0) continue;
    const auto n = ::read(fds[0], buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error{errno, std::generic_category(), "read"};
    }
    if (n == 0) {
      eof = true;
    } else {
      stream.append(buf, static_cast<std::size_t>(n));
    }
  }
  ::close(fds[0]);
  // A child that closed the pipe before the deadline exited on its own.
  if (!killed) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!killed || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    throw Failure{std::format(
        "child exited before the kill (status {:#x}); see its stderr above",
        status)};
  }
  return stream;
}

auto verify(const fs::path &crashed, const fs::path &work, const Config &cfg,
            const State &base, std::uint64_t base_durable, const History &h,
            Totals &totals) -> Recovered {
  if (h.opened) {
    // The child reopened the directory the previous iteration verified.
    if (h.opened_durable != base_durable ||
        h.opened_keys != base.size()) {
      throw Failure{std::format(
          "child's open disagrees with the verified reopen: durable_sequence "
          "{} vs {}, keys {} vs {}",
          h.opened_durable, base_durable, h.opened_keys, base.size())};
    }
  } else {
    ++totals.killed_before_open;
  }

  // Default options first: a directory left by a process crash must open
  // with fail_recovery_on_crc_errors = true. Serial recovery is the baseline.
  const auto serial_dir = work / "serial";
  copy_dir(crashed, serial_dir);
  Recovered serial;
  try {
    serial = open_and_collect(serial_dir, cfg, 1, true);
  } catch (const std::exception &e) {
    throw Failure{std::format("DB::open with default options refused the "
                              "crashed directory: {}",
                              e.what())};
  }

  const auto parallel_dir = work / "parallel";
  copy_dir(crashed, parallel_dir);
  const auto parallel = open_and_collect(parallel_dir, cfg, 4, false);

  if (serial.contents != parallel.contents)
    throw Failure{"serial and parallel recovery disagree on contents"};
  if (serial.file_stats != parallel.file_stats)
    throw Failure{"serial and parallel recovery disagree on file_stats"};
  if (serial.keydir_keys != parallel.keydir_keys ||
      serial.keydir_keys != std::ssize(serial.contents))
    throw Failure{"keydir_keys disagrees with the recovered contents"};
  if (serial.durable_sequence != parallel.durable_sequence)
    throw Failure{"serial and parallel recovery disagree on durable_sequence"};

  if (!h.opened) {
    // Killed during open: nothing was written, so recovery must reproduce the
    // previous verified state exactly.
    if (serial.contents != base)
      throw Failure{"killed during open, and the directory lost state"};
    return serial;
  }

  const auto prefix = match_prefix(base, h, serial.contents);
  if (serial.durable_sequence < h.watermark)
    throw Failure{std::format("recovered durable_sequence {} below watermark {}",
                              serial.durable_sequence, h.watermark)};
  totals.committed += prefix.committed;
  std::size_t needed = 0;
  for (const auto &rec : h.ops)
    if (rec.outcome == OpRecord::Outcome::Committed &&
        rec.sequence <= h.watermark)
      ++needed;
  totals.durable_needed += needed;
  if (prefix.applied < prefix.committed)
    totals.lost_after_return += prefix.committed - prefix.applied;
  return serial;
}

auto parse_args(int argc, char **argv) -> RunOptions {
  RunOptions o;
  o.seed = std::random_device{}();
  o.seed = (o.seed << 32) | std::random_device{}();
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string_view {
      if (i + 1 >= argc) throw std::invalid_argument{std::format("{} needs a value", a)};
      return argv[++i];
    };
    if (a == "--iterations") {
      o.iterations = std::stoi(std::string{next()});
    } else if (a == "--seed") {
      o.seed = std::stoull(std::string{next()});
    } else if (a == "--max-delay-ms") {
      o.max_delay_ms = std::stoi(std::string{next()});
    } else if (a == "--reset-every") {
      o.reset_every = std::max(1, std::stoi(std::string{next()}));
    } else if (a == "--dir") {
      o.dir = next();
    } else if (a == "--no-vacuum") {
      o.no_vacuum = true;
    } else if (a == "--verbose") {
      o.verbose = true;
    } else if (a == "--keep") {
      o.keep = true;
    } else {
      throw std::invalid_argument{std::format("unknown argument {}", a)};
    }
  }
  if (o.dir.empty())
    o.dir = fs::temp_directory_path() /
            std::format("bytecask_crash_{}", ::getpid());
  return o;
}

auto run_parent(const RunOptions &o) -> int {
  const auto self = fs::read_symlink("/proc/self/exe");
  const auto db_dir = o.dir / "db";
  const auto work = o.dir / "work";
  fs::remove_all(o.dir);
  fs::create_directories(work);

  std::printf("crash_consistency: seed=%llu iterations=%d dir=%s\n",
              static_cast<unsigned long long>(o.seed), o.iterations,
              o.dir.c_str());
  std::printf("  rerun: crash_consistency --seed %llu --iterations %d "
              "--max-delay-ms %d --reset-every %d\n",
              static_cast<unsigned long long>(o.seed), o.iterations,
              o.max_delay_ms, o.reset_every);
  std::fflush(stdout);

  std::mt19937_64 rng{o.seed};
  State base;
  std::uint64_t base_durable = 0;
  Totals totals;
  const auto started = Clock::now();

  for (int iter = 0; iter < o.iterations; ++iter) {
    if (iter % o.reset_every == 0) {
      fs::remove_all(db_dir);
      base.clear();
      base_durable = 0;
    }
    const auto child_seed = rng();
    auto cfg = config_for(child_seed);
    if (o.no_vacuum) cfg.vacuum = false;
    // Log-uniform from 1 ms to max_delay_ms: many kills land early (during
    // open, recovery and the first rotations), some after long runs.
    std::uniform_real_distribution<double> u{0.0, 1.0};
    const auto lo = std::log(1000.0);
    const auto hi = std::log(static_cast<double>(o.max_delay_ms) * 1000.0);
    const auto delay = std::chrono::microseconds{
        static_cast<std::int64_t>(std::exp(lo + (hi - lo) * u(rng)))};

    auto iteration_desc = [&] {
      return std::format("iteration {} child_seed={} backend={} "
                         "max_file_bytes={} sync%={} vacuum={} delay={}us",
                         iter, child_seed, backend_name(cfg.backend),
                         cfg.max_file_bytes, cfg.sync_percent, cfg.vacuum,
                         delay.count());
    };

    // The directory as the child will find it, kept for a failure report.
    const auto before = work / "before";
    copy_dir(db_dir, before);
    History h;
    try {
      const auto stream =
          run_and_kill(self, db_dir, child_seed, o.no_vacuum, delay);
      h = parse_history(stream);
      const auto crashed = work / "crashed";
      copy_dir(db_dir, crashed);
      const auto recovered =
          verify(crashed, work, cfg, base, base_durable, h, totals);
      base = recovered.contents;
      base_durable = recovered.durable_sequence;
    } catch (const Failure &f) {
      // The parent never opens db_dir itself, so it is still exactly what the
      // kill left behind.
      const auto keep = o.dir / "failure";
      fs::remove_all(keep);
      fs::create_directories(keep);
      copy_dir(before, keep / "before_open");
      copy_dir(db_dir, keep / "after_kill");
      {
        std::ofstream out{keep / "history.txt"};
        out << iteration_desc() << "\nwatermark " << h.watermark << "\n";
        for (const auto &rec : h.ops) out << describe(rec, nullptr) << "\n";
      }
      std::fprintf(stderr,
                   "FAIL %s\n  %s\n  history: %zu ops, watermark %llu\n"
                   "  directories and history kept in %s\n",
                   iteration_desc().c_str(), f.what.c_str(), h.ops.size(),
                   static_cast<unsigned long long>(h.watermark), keep.c_str());
      return 1;
    }
    if (o.verbose || (iter + 1) % 25 == 0 || iter + 1 == o.iterations) {
      std::printf("  [%d/%d] %s ops=%zu\n", iter + 1, o.iterations,
                  iteration_desc().c_str(), h.ops.size());
      std::fflush(stdout);
    }
  }

  const auto secs = std::chrono::duration<double>(Clock::now() - started).count();
  std::printf("PASS %d iterations in %.1fs: %zu committed ops checked, %zu at "
              "or below the durable watermark, %d kills during open, %zu "
              "returned non-durable writes lost\n",
              o.iterations, secs, totals.committed, totals.durable_needed,
              totals.killed_before_open, totals.lost_after_return);
  if (!o.keep) fs::remove_all(o.dir);
  return 0;
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    if (argc == 6 && std::string_view{argv[1]} == "--child") {
      return run_child(argv[2], std::stoull(argv[3]),
                       std::string_view{argv[4]} == "1", std::stoi(argv[5]));
    }
    return run_parent(parse_args(argc, argv));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "crash_consistency: %s\n", e.what());
    return 2;
  }
}
