// SPDX-License-Identifier: MIT
// Workload model, history frames and the prefix check shared by the
// process-crash harness (tests/crash/crash_consistency.cpp) and the chaos
// worker (tests/chaos/chaos_worker.cpp).
//
// Include after the standard headers; the header imports bytecask itself.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <unistd.h>

import bytecask;

namespace crash_model {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Workload model
// ---------------------------------------------------------------------------

inline constexpr int kKeySpace = 512;
inline constexpr std::size_t kMaxValueBytes = 600;

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

inline auto key_name(int i) -> std::string { return std::format("k{:04}", i); }

// Applies op to state the way the engine does. Returns false when the op is a
// no-op the engine reports as nullopt (del of an absent key, failed guard).
inline auto apply(State &s, const Op &op, std::vector<std::string> *touched)
    -> bool {
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

inline auto as_view(std::string_view s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

inline auto as_string(std::span<const std::byte> b) -> std::string {
  std::string s(b.size(), '\0');
  if (!b.empty()) std::memcpy(s.data(), b.data(), b.size());
  return s;
}

// A value is "<writer seed>.<ordinal>.<key>:" padded with 'x'. Unique per
// write, so a recovered value names exactly one operation, and it carries its
// key, so a reader can tell a value served under the wrong key.
inline auto make_value(std::uint64_t seed, std::uint64_t id,
                       std::string_view key, std::size_t pad) -> std::string {
  auto v = std::format("{:016x}.{}.{}:", seed, id, key);
  v.resize(v.size() + pad, 'x');
  return v;
}

// True when v is well formed and belongs to key.
inline auto value_matches_key(std::string_view v, std::string_view key) -> bool {
  const auto colon = v.find(':');
  if (colon == std::string_view::npos) return false;
  const auto head = v.substr(0, colon);
  const auto last_dot = head.rfind('.');
  if (last_dot == std::string_view::npos || head.substr(last_dot + 1) != key)
    return false;
  return std::ranges::all_of(v.substr(colon + 1), [](char c) { return c == 'x'; });
}

// A value's identity, without the padding.
inline auto brief(const std::string &v) -> std::string {
  return v.substr(0, v.find(':'));
}

inline auto random_op(std::mt19937_64 &rng, int sync_percent,
                      std::uint64_t seed, std::uint64_t &value_id) -> Op {
  auto pick = [&](int n) {
    return static_cast<int>(rng() % static_cast<std::uint64_t>(n));
  };
  auto value = [&](const std::string &key) {
    return make_value(seed, value_id++, key,
                      static_cast<std::size_t>(pick(kMaxValueBytes)));
  };
  Op op;
  op.sync = pick(100) < sync_percent;
  const auto r = pick(100);
  if (r < 55) {
    op.kind = OpKind::Put;
    op.key = key_name(pick(kKeySpace));
    op.value = value(op.key);
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
        item.value = value(item.key);
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
inline auto execute(bytecask::DB &db, const Op &op)
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

// ---------------------------------------------------------------------------
// Wire format: [u32 payload length][payload]. The payload starts with a
// FrameType byte. A frame cut short by a kill is discarded.
// ---------------------------------------------------------------------------

enum class FrameType : std::uint8_t {
  Opened = 1,     // u64 durable_sequence at open, u64 keydir_keys at open;
                  // the chaos worker adds u8 has_contents and the contents
  Intent = 2,     // encoded Op, written before the call
  Commit = 3,     // u64 sequence, u8 durable
  Abort = 4,      // call returned nullopt
  Watermark = 5,  // u64 durable_sequence()
  Throw = 6,      // str what(): the call threw; its outcome is unknown
  Rejected = 7,   // the call threw DbDegraded: nothing was written
  View = 8,       // u8 resumed, u64 durable_sequence, u32 n, n x (key, value)
  OpenFailed = 9, // str what(): DB::open threw
  Violation = 10, // str: the worker saw an invariant break while running
  Closed = 11,    // the DB was destroyed cleanly
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

inline void encode_op(Writer &w, const Op &op) {
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

inline auto decode_op(Reader &r) -> Op {
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

inline void encode_state(Writer &w, const State &s) {
  w.u32(static_cast<std::uint32_t>(s.size()));
  for (const auto &[k, v] : s) {
    w.str(k);
    w.str(v);
  }
}

inline auto decode_state(Reader &r) -> State {
  State s;
  const auto n = r.u32();
  for (std::uint32_t i = 0; i < n; ++i) {
    auto k = r.str();
    s.emplace(std::move(k), r.str());
  }
  return s;
}

inline void write_all(int fd, std::string_view bytes) {
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

// One write per frame, so frames from several threads never interleave.
inline void send_frame(int fd, const Writer &payload) {
  Writer frame;
  frame.u32(static_cast<std::uint32_t>(payload.data().size()));
  write_all(fd, frame.data() + payload.data());
}

// Splits a frame stream into payloads. A frame cut short is dropped.
inline auto split_frames(std::string_view stream) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t pos = 0;
  while (stream.size() - pos >= 4) {
    Reader len_reader{stream.substr(pos, 4)};
    const auto len = len_reader.u32();
    if (stream.size() - pos - 4 < len) break;
    out.push_back(stream.substr(pos + 4, len));
    pos += 4 + len;
  }
  return out;
}

// ---------------------------------------------------------------------------
// History and the prefix check
// ---------------------------------------------------------------------------

struct OpRecord {
  Op op;
  enum class Outcome {
    InFlight,  // no answer: the process died during the call
    Committed, // returned a CommitResult
    Aborted,   // returned nullopt
    Unknown,   // threw an I/O-shaped error: applied or not
    Rejected,  // threw DbDegraded: not applied
    Applied,   // an Unknown a later view showed applied; sequence unknown
  } outcome{Outcome::InFlight};
  std::uint64_t sequence{0};
  bool durable{false};
};

struct Failure {
  std::string what;
};

struct PrefixResult {
  std::size_t applied{0};   // operations in the matching prefix
  std::size_t committed{0}; // operations whose call returned a result
  std::size_t total{0};     // operations that may have applied
};

inline auto touches(const Op &op, const std::string &k) -> bool {
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

inline auto outcome_name(OpRecord::Outcome o) -> const char * {
  switch (o) {
  case OpRecord::Outcome::InFlight:
    return "in-flight";
  case OpRecord::Outcome::Committed:
    return "committed";
  case OpRecord::Outcome::Aborted:
    return "aborted";
  case OpRecord::Outcome::Unknown:
    return "threw";
  case OpRecord::Outcome::Rejected:
    return "rejected";
  case OpRecord::Outcome::Applied:
    return "applied";
  }
  return "?";
}

// One line per operation. With only_key set, batch items for other keys are
// left out.
inline auto describe(const OpRecord &rec, const std::string *only_key)
    -> std::string {
  auto line = std::format("seq {:>8} {:<9} sync={:d} durable={:d} ", rec.sequence,
                          outcome_name(rec.outcome), rec.op.sync, rec.durable);
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

// Finds the prefix of `ops` (in commit order; aborted and rejected ops
// skipped) whose application to `base` yields `recovered`, starting at the
// shortest prefix that covers `watermark`. `first_seq` is the durable sequence
// `base` was opened at: every committed sequence must lie above it. An
// in-flight or unknown operation may be in the prefix or not; only the last
// operation that may have applied can be one. Throws Failure when no prefix
// matches.
inline auto match_prefix(const State &base, std::span<const OpRecord> ops,
                         std::uint64_t first_seq, std::uint64_t watermark,
                         const State &recovered) -> PrefixResult {
  using Outcome = OpRecord::Outcome;
  std::vector<const OpRecord *> applied_ops;
  std::uint64_t last_seq = first_seq;
  State running = base;
  for (const auto &rec : ops) {
    switch (rec.outcome) {
    case Outcome::Committed: {
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
    case Outcome::Applied:
      (void)apply(running, rec.op, nullptr);
      applied_ops.push_back(&rec);
      break;
    case Outcome::Aborted: {
      auto copy = running;
      if (apply(copy, rec.op, nullptr))
        throw Failure{"engine returned nullopt for an op the model says "
                      "should commit"};
      break;
    }
    case Outcome::Rejected:
      break;
    case Outcome::InFlight:
    case Outcome::Unknown:
      applied_ops.push_back(&rec);
      break;
    }
  }

  std::size_t min_prefix = 0;
  std::size_t committed = 0;
  for (std::size_t i = 0; i < applied_ops.size(); ++i) {
    const auto *rec = applied_ops[i];
    if (rec->outcome == Outcome::InFlight || rec->outcome == Outcome::Unknown) {
      if (i + 1 != applied_ops.size())
        throw Failure{"an operation with an unknown outcome is not the last "
                      "that may have applied; the history is malformed"};
      continue;
    }
    if (rec->outcome == Outcome::Committed) {
      ++committed;
      if (rec->sequence <= watermark) min_prefix = i + 1;
    }
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
  if (longest)
    return {.applied = *longest, .committed = committed, .total = applied_ops.size()};

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
    for (const auto &rec : ops)
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
      watermark, applied_ops.size(), min_prefix, best_at, best, sample)};
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

struct Recovered {
  State contents;
  std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                         std::uint64_t>>
      file_stats;
  std::int64_t keydir_keys{0};
  std::uint64_t durable_sequence{0};
};

inline auto open_and_collect(const fs::path &dir, const bytecask::Options &opts)
    -> Recovered {
  auto db = bytecask::DB::open(dir, opts);
  Recovered r;
  for (auto &entry : db.iter_from({})) {
    r.contents.emplace(as_string(entry.key), as_string(entry.value));
  }
  for (const auto &[id, st] : db.file_stats()) {
    r.file_stats.emplace_back(st.live_bytes, st.total_bytes, st.min_sequence,
                              st.max_sequence);
  }
  std::ranges::sort(r.file_stats);
  r.keydir_keys = db.stats().at("bytecask.keydir_keys");
  r.durable_sequence = db.durable_sequence();
  return r;
}

inline auto copy_dir(const fs::path &from, const fs::path &to) -> void {
  fs::remove_all(to);
  // A kill before DB::open created the directory leaves nothing to copy.
  if (!fs::exists(from)) {
    fs::create_directories(to);
    return;
  }
  fs::copy(from, to, fs::copy_options::recursive);
}

// Serial recovery with default options is the baseline; parallel recovery
// with fail_recovery_on_crc_errors = false must agree with it. `opts` carries
// the backend and file size; recovery settings are overridden here.
inline auto recover_both(const fs::path &crashed, const fs::path &work,
                         bytecask::Options opts) -> Recovered {
  const auto serial_dir = work / "serial";
  copy_dir(crashed, serial_dir);
  opts.recovery_threads = 1;
  opts.fail_recovery_on_crc_errors = true;
  Recovered serial;
  try {
    serial = open_and_collect(serial_dir, opts);
  } catch (const std::exception &e) {
    throw Failure{std::format("DB::open with default options refused the "
                              "crashed directory: {}",
                              e.what())};
  }

  const auto parallel_dir = work / "parallel";
  copy_dir(crashed, parallel_dir);
  opts.recovery_threads = 4;
  opts.fail_recovery_on_crc_errors = false;
  const auto parallel = open_and_collect(parallel_dir, opts);

  if (serial.contents != parallel.contents)
    throw Failure{"serial and parallel recovery disagree on contents"};
  if (serial.file_stats != parallel.file_stats)
    throw Failure{"serial and parallel recovery disagree on file_stats"};
  if (serial.keydir_keys != parallel.keydir_keys ||
      serial.keydir_keys != std::ssize(serial.contents))
    throw Failure{"keydir_keys disagrees with the recovered contents"};
  if (serial.durable_sequence != parallel.durable_sequence)
    throw Failure{"serial and parallel recovery disagree on durable_sequence"};
  return serial;
}

} // namespace crash_model
