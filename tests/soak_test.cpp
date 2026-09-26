// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — chaos soak (#92).
//
// The proof matrix is single-threaded and mints a handful of files per cell.
// This covers what it cannot enumerate: readers, writers and lifecycle
// transitions (vacuum, set_mode, create_manifest, degrade + resume, close +
// reopen) running against one DB at the same time, for long enough that
// state accumulates. It is meant to run under ASan and TSan, which only
// report what actually executes.
//
// Hidden from the default run. Select it explicitly:
//
//   BYTECASK_SOAK_SECONDS=60 BYTECASK_SOAK_SEED=1234 bytecask_tests "[soak]"
//
// BYTECASK_SOAK_SECONDS  total wall-clock budget (default 20)
// BYTECASK_SOAK_SEED     run seed (default: random, printed)
// BYTECASK_SOAK_EPOCH    first epoch to run (default 0); with the seed, jumps
//                        straight to the epoch a failure was reported in
// BYTECASK_SOAK_KEEP     1 keeps a failed epoch's directory for inspection
// BYTECASK_SOAK_LIFECYCLE  comma-separated lifecycle operations to run
//                        (vacuum,snapshot,mode,manifest,degrade,stats,write;
//                        default all), for bisecting a failure
//
// The seed fixes each epoch's configuration and every thread's operation
// sequence. It cannot fix the interleaving — that is the thing under test —
// so a seed reproduces a failure's shape, and usually the failure itself
// after a few runs, not the exact schedule.
//
// Oracle. Each writer thread owns a disjoint key range, so it knows what each
// of its keys should hold, except after a write that threw an I/O-shaped
// error (its bytes may or may not surface after resume()): such a key is
// "unknown" until the writer's next confirmed write to it. Values are
// self-describing — header plus a payload derived from (writer, slot,
// counter) — so any thread can check any value it reads without
// coordination. What is checked:
//
//   every value      — well-formed, belongs to the key it was read under,
//                      and was actually attempted by its writer
//   writer read-back — a confirmed write reads back exactly (session reads)
//   writer outcomes  — del / guarded apply_batch return what the model says
//   durability       — a sync write's result is durable, and no reader ever
//                      sees a sync write's value while durable_sequence() is
//                      still below that write's sequence
//   monotonicity     — per reader thread, DB::get never goes back in time
//   ordering         — every iterator yields strictly ordered keys
//   held spans       — an EntryView held across a loop body keeps its bytes
//   snapshots        — two reads of a snapshot agree; an iterator outlives
//                      its snapshot; a snapshot outlives its DB
//   health           — the engine is never degraded except by an injected
//                      fault, and resume() always recovers from one
//   recovery         — reopen yields exactly what was there before close

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/resource.h>

import bytecask;

namespace {

using bytecask::Bytes;
using bytecask::BytesView;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------

constexpr auto splitmix64(std::uint64_t x) noexcept -> std::uint64_t {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

class Rng {
public:
  explicit Rng(std::uint64_t seed) : gen_{splitmix64(seed)} {}

  // Uniform in [lo, hi].
  auto range(std::uint64_t lo, std::uint64_t hi) -> std::uint64_t {
    return std::uniform_int_distribution<std::uint64_t>{lo, hi}(gen_);
  }
  auto chance(double p) -> bool {
    return std::bernoulli_distribution{p}(gen_);
  }
  template <typename T> auto pick(std::span<const T> xs) -> T {
    return xs[static_cast<std::size_t>(range(0, xs.size() - 1))];
  }

private:
  std::mt19937_64 gen_;
};

auto env_u64(const char *name) -> std::optional<std::uint64_t> {
  const char *v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  if (v == nullptr || *v == '\0') return std::nullopt;
  return std::stoull(v);
}

// ---------------------------------------------------------------------------
// Configuration — one per epoch, derived from (run seed, epoch index).
// ---------------------------------------------------------------------------

struct SoakConfig {
  std::uint64_t seed{0};
  unsigned epoch{0};
  std::chrono::milliseconds duration{0};
  unsigned writers{1};
  unsigned readers{1};
  unsigned slots_per_writer{64};
  bytecask::IoBackend io_backend{bytecask::IoBackend::Pread};
  std::uint64_t max_file_bytes{0};
  std::size_t pool_bytes{0};
  std::uint32_t max_key_bytes{0};
  std::uint32_t max_value_bytes{0};
  bool verify_checksums{true};
  unsigned recovery_threads{1};

  [[nodiscard]] auto options() const -> bytecask::Options {
    bytecask::Options o;
    o.max_file_bytes = max_file_bytes;
    o.recovery_threads = recovery_threads;
    o.max_key_bytes = max_key_bytes;
    o.max_value_bytes = max_value_bytes;
    o.io_backend = io_backend;
    o.buffer_pool.capacity_bytes = pool_bytes;
    return o;
  }

  [[nodiscard]] auto describe() const -> std::string {
    const auto backend = io_backend == bytecask::IoBackend::Pread ? "pread"
                         : io_backend == bytecask::IoBackend::Mmap
                             ? "mmap"
                             : "buffer_pool";
    return std::format(
        "seed={} epoch={} duration_ms={} writers={} readers={} slots={} "
        "io_backend={} max_file_bytes={} pool_bytes={} max_key_bytes={} "
        "max_value_bytes={} verify_checksums={} recovery_threads={}",
        seed, epoch, duration.count(), writers, readers, slots_per_writer,
        backend, max_file_bytes, pool_bytes, max_key_bytes, max_value_bytes,
        verify_checksums, recovery_threads);
  }
};

auto make_config(std::uint64_t seed, unsigned epoch,
                 std::chrono::milliseconds remaining) -> SoakConfig {
  Rng r{seed ^ splitmix64(epoch + 1)};
  SoakConfig c;
  c.seed = seed;
  c.epoch = epoch;
  c.duration = std::min(
      remaining, std::chrono::milliseconds{r.range(1000, 6000)});
  c.writers = static_cast<unsigned>(r.range(1, 4));
  c.readers = static_cast<unsigned>(r.range(1, 4));
  c.slots_per_writer = static_cast<unsigned>(r.pick<std::uint64_t>(
      std::array<std::uint64_t, 4>{8, 64, 256, 1024}));
  c.io_backend = r.pick<bytecask::IoBackend>(std::array{
      bytecask::IoBackend::Pread, bytecask::IoBackend::Mmap,
      bytecask::IoBackend::BufferPool});
  // 1 rotates on every write; the rest span one to many writes per file.
  c.max_file_bytes = r.pick<std::uint64_t>(std::array<std::uint64_t, 5>{
      1, 4096, 64 * 1024, 1024 * 1024, 8 * 1024 * 1024});
  // The pool is kept small relative to the data so eviction runs.
  c.pool_bytes = std::max<std::size_t>(
      2 * c.max_file_bytes,
      r.pick<std::uint64_t>(std::array<std::uint64_t, 3>{
          256 * 1024, 2 * 1024 * 1024, 32 * 1024 * 1024}));
  c.max_key_bytes = static_cast<std::uint32_t>(r.pick<std::uint64_t>(
      std::array<std::uint64_t, 3>{32, 256, 4096}));
  c.max_value_bytes = static_cast<std::uint32_t>(r.pick<std::uint64_t>(
      std::array<std::uint64_t, 3>{256, 64 * 1024, 1024 * 1024}));
  c.verify_checksums = r.chance(0.5);
  c.recovery_threads = static_cast<unsigned>(r.range(1, 8));
  return c;
}

// ---------------------------------------------------------------------------
// Failure log — worker threads cannot use Catch2 assertions (they are not
// thread-safe), so they record here and the main thread reports.
// ---------------------------------------------------------------------------

class Failures {
public:
  void add(std::string msg) {
    std::lock_guard<std::mutex> lk{mu_};
    if (msgs_.size() < 32) msgs_.push_back(std::move(msg));
    any_.store(true, std::memory_order_release);
  }
  [[nodiscard]] auto any() const noexcept -> bool {
    return any_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto text() const -> std::string {
    std::lock_guard<std::mutex> lk{mu_};
    std::string out;
    for (const auto &m : msgs_) out += "  - " + m + "\n";
    return out;
  }

private:
  mutable std::mutex mu_;
  std::vector<std::string> msgs_;
  std::atomic<bool> any_{false};
};

// ---------------------------------------------------------------------------
// Keys and values
//
// Key:   "<owner>/<slot:06>/" + filler.  owner = "w<id:02>" or "lc".
//        The filler length is a fixed function of (seed, owner, slot), so a
//        slot always maps to the same key; now and then it is the maximum.
// Value: magic u32 | owner u16 | flags u16 | slot u32 | len u32 |
//        counter u64 | payload, payload[i] a function of the header.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMagic = 0x5341'4B31;  // "SAK1"
constexpr std::size_t kValueHeader = 24;
constexpr std::uint16_t kLifecycleOwner = 0xFFFF;
constexpr std::uint16_t kFlagSync = 1;
constexpr std::size_t kKeyPrefix = 11;  // "w00/000000/"

auto owner_prefix(std::uint16_t owner) -> std::string {
  return owner == kLifecycleOwner ? std::string{"lc"}
                                  : std::format("w{:02}", owner);
}

auto slot_prefix(std::uint16_t owner, std::uint32_t slot) -> std::string {
  return std::format("{}/{:06}/", owner_prefix(owner), slot);
}

auto make_key(const SoakConfig &c, std::uint16_t owner, std::uint32_t slot)
    -> Bytes {
  auto s = slot_prefix(owner, slot);
  const auto h = splitmix64(c.seed ^ (std::uint64_t{owner} << 32) ^ slot);
  const auto room = c.max_key_bytes - s.size();
  const auto filler = h % 50 == 0 ? room : std::min<std::size_t>(h % 17, room);
  for (std::size_t i = 0; i < filler; ++i)
    s.push_back(static_cast<char>('a' + (h >> (i % 48)) % 26));
  const auto b = std::as_bytes(std::span{s});
  return {b.begin(), b.end()};
}

struct KeyId {
  std::uint16_t owner;
  std::uint32_t slot;
};

auto parse_key(BytesView key) -> std::optional<KeyId> {
  if (key.size() < kKeyPrefix - 1) return std::nullopt;
  const std::string_view s{reinterpret_cast<const char *>(key.data()),
                           key.size()};
  std::uint16_t owner = 0;
  std::size_t pos = 0;
  if (s.starts_with("lc/")) {
    owner = kLifecycleOwner;
    pos = 3;
  } else if (s.size() >= kKeyPrefix && s[0] == 'w' && s[3] == '/') {
    owner = static_cast<std::uint16_t>((s[1] - '0') * 10 + (s[2] - '0'));
    pos = 4;
  } else {
    return std::nullopt;
  }
  if (s.size() < pos + 7 || s[pos + 6] != '/') return std::nullopt;
  std::uint32_t slot = 0;
  for (std::size_t i = pos; i < pos + 6; ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    slot = slot * 10 + static_cast<std::uint32_t>(s[i] - '0');
  }
  return KeyId{owner, slot};
}

template <typename T> void put_le(std::span<std::byte> out, T v) {
  for (std::size_t i = 0; i < sizeof(T); ++i)
    out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}
template <typename T> auto get_le(BytesView in) -> T {
  T v = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    v |= static_cast<T>(std::to_integer<std::uint64_t>(in[i]) << (8 * i));
  return v;
}

auto payload_byte(std::uint64_t h, std::size_t i) -> std::byte {
  return static_cast<std::byte>(splitmix64(h + i / 8) >> (8 * (i % 8)));
}

auto payload_seed(std::uint16_t owner, std::uint32_t slot,
                  std::uint64_t counter) -> std::uint64_t {
  return splitmix64((std::uint64_t{owner} << 48) ^ (std::uint64_t{slot} << 20)
                    ^ splitmix64(counter));
}

auto make_value(std::uint16_t owner, std::uint32_t slot, std::uint64_t counter,
                bool sync, std::size_t len) -> Bytes {
  len = std::max(len, kValueHeader);
  Bytes v(len);
  const std::span<std::byte> s{v};
  put_le<std::uint32_t>(s.subspan(0), kMagic);
  put_le<std::uint16_t>(s.subspan(4), owner);
  put_le<std::uint16_t>(s.subspan(6), sync ? kFlagSync : 0);
  put_le<std::uint32_t>(s.subspan(8), slot);
  put_le<std::uint32_t>(s.subspan(12), static_cast<std::uint32_t>(len));
  put_le<std::uint64_t>(s.subspan(16), counter);
  const auto h = payload_seed(owner, slot, counter);
  for (std::size_t i = kValueHeader; i < len; ++i) v[i] = payload_byte(h, i);
  return v;
}

struct ValueId {
  std::uint16_t owner;
  std::uint32_t slot;
  std::uint64_t counter;
  bool sync;
};

// Checks a value against the key it was read under. Returns the decoded
// identity or a description of what is wrong.
auto check_value(BytesView key, BytesView value)
    -> std::pair<std::optional<ValueId>, std::string> {
  const auto kid = parse_key(key);
  if (!kid) return {std::nullopt, "key does not parse"};
  if (value.size() < kValueHeader)
    return {std::nullopt, std::format("value too short ({} bytes)",
                                      value.size())};
  if (get_le<std::uint32_t>(value) != kMagic)
    return {std::nullopt, "bad magic"};
  ValueId id{get_le<std::uint16_t>(value.subspan(4)),
             get_le<std::uint32_t>(value.subspan(8)),
             get_le<std::uint64_t>(value.subspan(16)),
             (get_le<std::uint16_t>(value.subspan(6)) & kFlagSync) != 0};
  if (id.owner != kid->owner || id.slot != kid->slot)
    return {std::nullopt,
            std::format("value belongs to owner {} slot {}, read under "
                        "owner {} slot {}",
                        id.owner, id.slot, kid->owner, kid->slot)};
  if (get_le<std::uint32_t>(value.subspan(12)) != value.size())
    return {std::nullopt, "length field disagrees with value size"};
  const auto h = payload_seed(id.owner, id.slot, id.counter);
  for (std::size_t i = kValueHeader; i < value.size(); ++i) {
    if (value[i] != payload_byte(h, i))
      return {std::nullopt, std::format("payload mismatch at byte {}", i)};
  }
  return {id, {}};
}

auto key_str(BytesView key) -> std::string {
  std::string s{reinterpret_cast<const char *>(key.data()),
                std::min<std::size_t>(key.size(), 40)};
  if (key.size() > 40) s += "...";
  return s;
}

auto key_less(BytesView a, BytesView b) -> bool {
  return std::ranges::lexicographical_compare(
      a, b, [](std::byte x, std::byte y) {
        return std::to_integer<unsigned>(x) < std::to_integer<unsigned>(y);
      });
}

// ---------------------------------------------------------------------------
// Shared per-writer state visible to readers.
// ---------------------------------------------------------------------------

struct WriterShared {
  // Highest counter this writer has handed to the engine. Published before
  // the write is issued, so no reader can see a value above it.
  std::atomic<std::uint64_t> attempted{0};
  // Every counter at or below this has returned to the writer.
  std::atomic<std::uint64_t> settled{0};

  // counter -> sequence, for committed sync=true writes still recent enough
  // for a reader to be holding an observation of them.
  void record_sync(std::uint64_t counter, std::uint64_t seq) {
    std::lock_guard<std::mutex> lk{mu_};
    sync_seq_[counter] = seq;
    if (sync_seq_.size() > 1U << 16) {
      const auto cutoff = counter - (1U << 15);
      std::erase_if(sync_seq_, [&](const auto &kv) { return kv.first < cutoff; });
    }
  }
  [[nodiscard]] auto sync_seq(std::uint64_t counter) const
      -> std::optional<std::uint64_t> {
    std::lock_guard<std::mutex> lk{mu_};
    if (auto it = sync_seq_.find(counter); it != sync_seq_.end())
      return it->second;
    return std::nullopt;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::uint64_t, std::uint64_t> sync_seq_;
};

// A writer's belief about one of its keys.
struct SlotModel {
  enum class State { Absent, Present, Unknown } state{State::Absent};
  std::uint64_t counter{0};  // valid when Present
};

// A writer's model, owned by the epoch driver so the end-of-epoch checks can
// read it after the writer's thread is gone.
struct WriterPersistent {
  std::vector<SlotModel> slots;
  std::uint64_t next_counter{1};
};

struct EpochShared {
  const SoakConfig &cfg;
  bytecask::DB &db;
  Failures &failures;
  std::vector<std::unique_ptr<WriterShared>> writers;  // + lifecycle, last
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> injected_degrades{0};

  [[nodiscard]] auto writer_for(std::uint16_t owner) const -> WriterShared * {
    if (owner == kLifecycleOwner) return writers.back().get();
    if (owner >= writers.size() - 1) return nullptr;
    return writers[owner].get();
  }

  void fail(std::string what) {
    failures.add(std::format("[{}] {}", cfg.describe(), what));
    stop.store(true, std::memory_order_release);
  }

  [[nodiscard]] auto stopping() const -> bool {
    return stop.load(std::memory_order_acquire) || failures.any();
  }

  // Common value validation: shape plus "was attempted".
  auto validate(BytesView key, BytesView value, std::string_view where)
      -> std::optional<ValueId> {
    auto [id, err] = check_value(key, value);
    if (!id) {
      fail(std::format("{}: key '{}': {}", where, key_str(key), err));
      return std::nullopt;
    }
    auto *w = writer_for(id->owner);
    if (w == nullptr) {
      fail(std::format("{}: key '{}' names unknown owner {}", where,
                       key_str(key), id->owner));
      return std::nullopt;
    }
    if (id->counter > w->attempted.load(std::memory_order_acquire)) {
      fail(std::format("{}: key '{}' holds counter {} never attempted",
                       where, key_str(key), id->counter));
      return std::nullopt;
    }
    return id;
  }
};

// A std::system_error from a write is the injected fault: nothing else
// throws one in a healthy run, and a real I/O error degrades the engine,
// which the lifecycle thread reports outside a fault window. The exception
// is classified by type alone, never inspected: the engine hands one failed
// flush's exception object to every writer it failed, and libstdc++ frees
// it under a reference count TSan cannot see (it lives in the uninstrumented
// runtime), so a read of it on one thread and its release on another is
// reported as a race that is not there.

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

class Writer {
public:
  Writer(EpochShared &sh, std::uint16_t id, WriterPersistent &p)
      : sh_{sh}, cfg_{sh.cfg}, id_{id}, p_{p}, me_{*sh.writers[id]},
        rng_{cfg_.seed ^ splitmix64((std::uint64_t{cfg_.epoch} << 16) | id)} {
    keys_.reserve(cfg_.slots_per_writer);
    for (std::uint32_t s = 0; s < cfg_.slots_per_writer; ++s)
      keys_.push_back(make_key(cfg_, id_, s));
  }

  void run() {
    while (!sh_.stopping()) {
      const auto op = rng_.range(0, 99);
      if (op < 45) {
        do_put();
      } else if (op < 60) {
        do_del();
      } else if (op < 65) {
        do_del_range();
      } else if (op < 90) {
        do_batch();
      } else {
        do_snapshot_read();
      }
    }
  }

private:
  [[nodiscard]] auto wopts() -> bytecask::WriteOptions {
    return {.sync = rng_.chance(0.3), .solo = rng_.chance(0.2)};
  }
  [[nodiscard]] auto ropts() const -> bytecask::ReadOptions {
    return {.verify_checksums = cfg_.verify_checksums};
  }

  auto value_len() -> std::size_t {
    const auto roll = rng_.range(0, 999);
    if (roll < 10) return cfg_.max_value_bytes;
    if (roll < 60)
      return static_cast<std::size_t>(
          rng_.range(kValueHeader, std::min<std::uint64_t>(
                                       cfg_.max_value_bytes, 8192)));
    return static_cast<std::size_t>(rng_.range(
        kValueHeader, std::min<std::uint64_t>(cfg_.max_value_bytes, 200)));
  }

  auto slot() -> std::uint32_t {
    return static_cast<std::uint32_t>(rng_.range(0, cfg_.slots_per_writer - 1));
  }

  auto new_counter() -> std::uint64_t {
    const auto c = p_.next_counter++;
    me_.attempted.store(c, std::memory_order_release);
    return c;
  }
  void settle(std::uint64_t c) {
    me_.settled.store(c, std::memory_order_release);
  }

  // Classifies an exception thrown by a write. Returns true if the write
  // was rejected before anything was appended.
  auto classify(std::string_view op) -> bool {
    try {
      throw;
    } catch (const bytecask::DbFollowerMode &) {
      return true;  // admission check, nothing appended
    } catch (const bytecask::DbDegraded &) {
      return false;  // may have been appended before the degrade
    } catch (const std::system_error &) {
      return false;  // the injected fault; see above
    } catch (const std::exception &e) {
      sh_.fail(std::format("writer {} {}: unexpected exception: {}", id_, op,
                           e.what()));
      return false;
    }
  }

  void check_commit(const bytecask::CommitResult &r, bool sync,
                    std::string_view op) {
    if (sync && !r.durable)
      sh_.fail(std::format("writer {} {}: sync write returned durable=false "
                           "(sequence {})", id_, op, r.sequence));
    if (r.durable && sh_.db.durable_sequence() < r.sequence)
      sh_.fail(std::format("writer {} {}: durable write at sequence {} but "
                           "durable_sequence() = {}",
                           id_, op, r.sequence, sh_.db.durable_sequence()));
  }

  void read_back(std::uint32_t s, std::string_view op) {
    const auto &m = p_.slots[s];
    if (m.state == SlotModel::State::Unknown) return;
    Bytes out;
    bool found = false;
    try {
      found = sh_.db.get(ropts(), keys_[s], out);
    } catch (const std::exception &e) {
      sh_.fail(std::format("writer {} read-back after {}: {}", id_, op,
                           e.what()));
      return;
    }
    if (m.state == SlotModel::State::Absent) {
      if (found) {
        const auto id = check_value(keys_[s], out).first;
        sh_.fail(std::format("writer {} read-back after {}: key '{}' should "
                             "be absent, holds counter {}",
                             id_, op, key_str(keys_[s]),
                             id ? std::to_string(id->counter) : "?"));
      }
      return;
    }
    if (!found) {
      sh_.fail(std::format("writer {} read-back after {}: key '{}' missing, "
                           "expected counter {}",
                           id_, op, key_str(keys_[s]), m.counter));
      return;
    }
    auto vid = sh_.validate(keys_[s], out, "writer read-back");
    if (vid && vid->counter != m.counter)
      sh_.fail(std::format("writer {} read-back after {}: key '{}' holds "
                           "counter {}, expected {}",
                           id_, op, key_str(keys_[s]), vid->counter,
                           m.counter));
  }

  void do_put() {
    const auto s = slot();
    const auto opts = wopts();
    const auto c = new_counter();
    const auto v = make_value(id_, s, c, opts.sync, value_len());
    try {
      const auto r = sh_.db.put(opts, keys_[s], v);
      check_commit(r, opts.sync, "put");
      if (opts.sync) me_.record_sync(c, r.sequence);
      p_.slots[s] = {SlotModel::State::Present, c};
      settle(c);
      read_back(s, "put");
    } catch (...) {
      if (!classify("put")) p_.slots[s].state = SlotModel::State::Unknown;
      settle(c);
    }
  }

  void do_del() {
    const auto s = slot();
    const auto opts = wopts();
    const auto before = p_.slots[s];
    try {
      const auto r = sh_.db.del(opts, keys_[s]);
      if (before.state == SlotModel::State::Present && !r)
        sh_.fail(std::format("writer {} del: key '{}' known present but del "
                             "reported absent", id_, key_str(keys_[s])));
      if (before.state == SlotModel::State::Absent && r)
        sh_.fail(std::format("writer {} del: key '{}' known absent but del "
                             "wrote a tombstone", id_, key_str(keys_[s])));
      if (r) check_commit(*r, opts.sync, "del");
      p_.slots[s].state = SlotModel::State::Absent;
      read_back(s, std::format("del (model {}, counter {}, del {})",
                               static_cast<int>(before.state), before.counter,
                               r ? std::format("wrote seq {}", r->sequence)
                                 : std::string{"found nothing"}));
    } catch (...) {
      if (!classify("del")) p_.slots[s].state = SlotModel::State::Unknown;
    }
  }

  // [from, to) over this writer's slots [a, b).
  auto range_keys(std::uint32_t a, std::uint32_t b)
      -> std::pair<std::string, std::string> {
    return {slot_prefix(id_, a), slot_prefix(id_, b)};
  }

  void do_del_range() {
    const auto a = slot();
    const auto b = static_cast<std::uint32_t>(
        rng_.range(a, std::min<std::uint64_t>(cfg_.slots_per_writer, a + 16)));
    const auto [from, to] = range_keys(a, b);
    const auto opts = wopts();
    try {
      const auto r = sh_.db.del_range(opts, std::as_bytes(std::span{from}),
                                      std::as_bytes(std::span{to}));
      if (a < b) check_commit(r, opts.sync, "del_range");
      for (auto s = a; s < b; ++s) p_.slots[s].state = SlotModel::State::Absent;
      if (a < b) read_back(a, "del_range");
    } catch (...) {
      if (!classify("del_range"))
        for (auto s = a; s < b; ++s)
          p_.slots[s].state = SlotModel::State::Unknown;
    }
  }

  void do_batch() {
    const bool with_snap = rng_.chance(0.5);
    std::optional<bytecask::WritePlan> plan;
    if (with_snap)
      plan.emplace(sh_.db.snapshot());
    else
      plan.emplace();
    const auto opts = wopts();

    // Model the plan on a copy; commit it only if the batch commits.
    auto model = p_.slots;
    bool all_known = true;
    bool guard_violated = false;
    std::vector<std::uint32_t> guarded;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> puts;  // counter, slot
    std::vector<std::uint32_t> touched;

    // Guards first, each on a distinct key, judged against the pre-state.
    const auto n_guards = rng_.range(0, 2);
    for (std::uint64_t g = 0; g < n_guards; ++g) {
      const auto s = slot();
      if (std::ranges::find(guarded, s) != guarded.end()) continue;
      guarded.push_back(s);
      const auto st = p_.slots[s].state;
      if (st == SlotModel::State::Unknown) all_known = false;
      const auto kind = rng_.range(0, with_snap ? 2 : 1);
      if (kind == 0) {
        plan->ensure_present(keys_[s]);
        if (st == SlotModel::State::Absent) guard_violated = true;
      } else if (kind == 1) {
        plan->ensure_absent(keys_[s]);
        if (st == SlotModel::State::Present) guard_violated = true;
      } else {
        plan->ensure_unchanged(keys_[s]);
      }
    }
    std::string shape = with_snap ? "snapshot" : "no snapshot";
    for (auto s : guarded)
      shape += std::format(" guard({}:{})", s,
                           static_cast<int>(p_.slots[s].state));
    if (with_snap && rng_.chance(0.2)) {
      const auto a = slot();
      const auto b = static_cast<std::uint32_t>(rng_.range(
          a, std::min<std::uint64_t>(cfg_.slots_per_writer, a + 8)));
      const auto [from, to] = range_keys(a, b);
      plan->ensure_range_unchanged(std::as_bytes(std::span{from}),
                                   std::as_bytes(std::span{to}));
      // An unknown key in the range may surface at resume() between the
      // snapshot and the commit, which is a real change to the range.
      for (auto s = a; s < b; ++s)
        if (p_.slots[s].state == SlotModel::State::Unknown) all_known = false;
      shape += std::format(" range_guard[{},{})", a, b);
    }

    const auto n_ops = rng_.range(1, 6);
    std::vector<Bytes> values;  // kept alive until apply_batch
    values.reserve(n_ops);
    for (std::uint64_t i = 0; i < n_ops; ++i) {
      const auto kind = rng_.range(0, 9);
      if (kind < 6) {
        const auto s = slot();
        const auto c = new_counter();
        values.push_back(make_value(id_, s, c, opts.sync, value_len()));
        plan->put(keys_[s], values.back());
        model[s] = {SlotModel::State::Present, c};
        puts.emplace_back(c, s);
        touched.push_back(s);
      } else if (kind < 9) {
        const auto s = slot();
        plan->del(keys_[s]);
        model[s].state = SlotModel::State::Absent;
        touched.push_back(s);
      } else {
        const auto a = slot();
        const auto b = static_cast<std::uint32_t>(rng_.range(
            a, std::min<std::uint64_t>(cfg_.slots_per_writer, a + 8)));
        const auto [from, to] = range_keys(a, b);
        plan->del_range(std::as_bytes(std::span{from}),
                        std::as_bytes(std::span{to}));
        for (auto s = a; s < b; ++s) {
          model[s].state = SlotModel::State::Absent;
          touched.push_back(s);
        }
      }
    }
    // A snapshot-backed plan conflicts if a written key moved since the
    // snapshot; this writer is the only one writing its keys, so that only
    // happens through an unknown outcome surfacing at resume().
    for (auto s : touched)
      if (p_.slots[s].state == SlotModel::State::Unknown) all_known = false;

    const auto last_counter = puts.empty() ? 0 : puts.back().first;
    try {
      const auto r = sh_.db.apply_batch(opts, std::move(*plan));
      if (r) {
        if (guard_violated && all_known)
          sh_.fail(std::format("writer {} apply_batch: committed although a "
                               "guard contradicts the known state", id_));
        check_commit(*r, opts.sync, "apply_batch");
        if (opts.sync)
          for (const auto &[c, s] : puts) me_.record_sync(c, r->sequence);
        p_.slots = std::move(model);
      } else if (all_known && !guard_violated) {
        std::string writes;
        for (auto s : touched) writes += std::format(" {}", s);
        sh_.fail(std::format("writer {} apply_batch: conflict reported with "
                             "every guard and write key in a known, "
                             "satisfied state ({}; writes to slots{})",
                             id_, shape, writes));
      }
      if (last_counter != 0) settle(last_counter);
      if (!touched.empty()) read_back(touched.back(), "apply_batch");
    } catch (...) {
      if (!classify("apply_batch"))
        for (auto s : touched) p_.slots[s].state = SlotModel::State::Unknown;
      if (last_counter != 0) settle(last_counter);
    }
  }

  // A snapshot must answer the same way twice, whatever happens between.
  void do_snapshot_read() {
    auto snap = sh_.db.snapshot();
    std::vector<std::uint32_t> ss;
    for (int i = 0; i < 4; ++i) ss.push_back(slot());
    std::vector<std::optional<Bytes>> first;
    for (auto s : ss) {
      Bytes out;
      first.push_back(snap.get(ropts(), keys_[s], out) ? std::optional{out}
                                                       : std::nullopt);
    }
    // This writer's own writes land between the two passes.
    do_put();
    for (std::size_t i = 0; i < ss.size(); ++i) {
      Bytes out;
      const bool found = snap.get(ropts(), keys_[ss[i]], out);
      if (found != first[i].has_value() || (found && out != *first[i]))
        sh_.fail(std::format("writer {}: snapshot read of '{}' changed "
                             "between two reads", id_, key_str(keys_[ss[i]])));
    }
  }

  EpochShared &sh_;
  const SoakConfig &cfg_;
  std::uint16_t id_;
  WriterPersistent &p_;
  WriterShared &me_;
  Rng rng_;
  std::vector<Bytes> keys_;
};

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

class Reader {
public:
  Reader(EpochShared &sh, unsigned id)
      : sh_{sh}, cfg_{sh.cfg},
        rng_{cfg_.seed ^ splitmix64((std::uint64_t{cfg_.epoch} << 16) | 0x100U
                                    | id)} {}

  void run() {
    while (!sh_.stopping()) {
      const auto op = rng_.range(0, 99);
      if (op < 45) {
        do_get();
      } else if (op < 55) {
        do_contains();
      } else if (op < 70) {
        do_iter(false);
      } else if (op < 80) {
        do_iter(true);
      } else if (op < 88) {
        do_keys(false);
      } else if (op < 93) {
        do_keys(true);
      } else if (op < 99 || !rng_.chance(0.01)) {
        do_snapshot();
      } else {
        do_idle();
      }
      resolve_pending();
    }
  }

private:
  [[nodiscard]] auto ropts() const -> bytecask::ReadOptions {
    return {.verify_checksums = cfg_.verify_checksums};
  }

  auto random_key() -> Bytes {
    const auto owner = rng_.chance(0.05)
        ? kLifecycleOwner
        : static_cast<std::uint16_t>(rng_.range(0, cfg_.writers - 1));
    const auto slot = static_cast<std::uint32_t>(
        rng_.range(0, owner == kLifecycleOwner ? 15 : cfg_.slots_per_writer - 1));
    return make_key(cfg_, owner, slot);
  }

  // A sync write becomes visible only once durable_sequence() covers it. The
  // sequence is known only when the writer's call returns, so remember what
  // durable_sequence() said right after the read and settle it later.
  void observe(const ValueId &id) {
    if (!id.sync || pending_.size() >= 4096) return;
    pending_.push_back({id.owner, id.counter, sh_.db.durable_sequence()});
  }

  void resolve_pending() {
    std::erase_if(pending_, [&](const Pending &p) {
      auto *w = sh_.writer_for(p.owner);
      if (auto seq = w->sync_seq(p.counter)) {
        if (p.durable_after_read < *seq)
          sh_.fail(std::format(
              "durability before visibility: a reader saw owner {} counter "
              "{} (sequence {}) while durable_sequence() was {}",
              p.owner, p.counter, *seq, p.durable_after_read));
        return true;
      }
      // Settled without a recorded sequence: the write failed or was
      // pruned. Nothing to check.
      return w->settled.load(std::memory_order_acquire) >= p.counter;
    });
  }

  void do_get() {
    const auto key = random_key();
    Bytes out;
    if (!sh_.db.get(ropts(), key, out)) return;
    auto id = sh_.validate(key, out, "get");
    if (!id) return;
    observe(*id);
    // Per-thread snapshots only ever move forward.
    auto &last = last_seen_[std::string{reinterpret_cast<const char *>(key.data()),
                                        key.size()}];
    if (id->counter < last)
      sh_.fail(std::format("get went back in time on '{}': counter {} after "
                           "{}", key_str(key), id->counter, last));
    last = id->counter;
  }

  void do_contains() {
    (void)sh_.db.contains_key(ropts(), random_key());
  }

  // Walks from a random key. The span of each entry is held across a loop
  // body that does other engine work, then compared to a copy.
  template <typename Range>
  void walk_entries(Range &&range, bool reverse, std::string_view where) {
    Bytes prev;
    bool have_prev = false;
    const auto limit = rng_.range(1, 64);
    std::uint64_t n = 0;
    for (auto it = range.begin(); it != range.end() && n < limit; ++it, ++n) {
      const auto &[key, value] = *it;
      const Bytes key_copy{key.begin(), key.end()};
      const Bytes value_copy{value.begin(), value.end()};
      if (have_prev && (reverse ? !key_less(key, prev) : !key_less(prev, key)))
        sh_.fail(std::format("{}: keys out of order: '{}' after '{}'", where,
                             key_str(key), key_str(prev)));
      if (auto id = sh_.validate(key, value, where)) observe(*id);
      // Loop body: let other threads move files underneath the span.
      if (rng_.chance(0.1)) {
        Bytes tmp;
        (void)sh_.db.get(ropts(), random_key(), tmp);
      }
      if (rng_.chance(0.02)) std::this_thread::sleep_for(1ms);
      else std::this_thread::yield();
      if (!std::ranges::equal(key, key_copy)
          || !std::ranges::equal(value, value_copy))
        sh_.fail(std::format("{}: held span for '{}' changed across the loop "
                             "body", where, key_str(key_copy)));
      prev = key_copy;
      have_prev = true;
      if (sh_.stopping()) break;
    }
  }

  template <typename Range>
  void walk_keys(Range &&range, bool reverse, std::string_view where) {
    Bytes prev;
    bool have_prev = false;
    const auto limit = rng_.range(1, 256);
    std::uint64_t n = 0;
    for (auto it = range.begin(); it != range.end() && n < limit; ++it, ++n) {
      const Bytes key{(*it).begin(), (*it).end()};
      if (!parse_key(key))
        sh_.fail(std::format("{}: malformed key '{}'", where, key_str(key)));
      if (have_prev && (reverse ? !key_less(key, prev) : !key_less(prev, key)))
        sh_.fail(std::format("{}: keys out of order: '{}' after '{}'", where,
                             key_str(key), key_str(prev)));
      prev = key;
      have_prev = true;
    }
  }

  void do_iter(bool reverse) {
    const auto from = rng_.chance(0.1) ? Bytes{} : random_key();
    if (reverse)
      walk_entries(sh_.db.riter_from(ropts(), from), true, "riter_from");
    else
      walk_entries(sh_.db.iter_from(ropts(), from), false, "iter_from");
  }

  void do_keys(bool reverse) {
    const auto from = rng_.chance(0.1) ? Bytes{} : random_key();
    if (reverse)
      walk_keys(sh_.db.rkeys_from(ropts(), from), true, "rkeys_from");
    else
      walk_keys(sh_.db.keys_from(ropts(), from), false, "keys_from");
  }

  // A thread that reads and then goes quiet: its cached engine state is
  // taken by the scrape (idle ~1 s) on another thread's publish, and the
  // read that wakes it races that scrape for the entry.
  void do_idle() {
    do_get();
    const auto until = Clock::now() + std::chrono::milliseconds{rng_.range(1200, 1600)};
    while (Clock::now() < until && !sh_.stopping())
      std::this_thread::sleep_for(20ms);
    do_get();
  }

  // Snapshot reads twice agree; iterators taken from a snapshot outlive it.
  void do_snapshot() {
    auto snap = std::make_unique<bytecask::Snapshot>(sh_.db.snapshot());
    std::vector<Bytes> keys;
    std::vector<std::optional<Bytes>> first;
    for (int i = 0; i < 6; ++i) {
      keys.push_back(random_key());
      Bytes out;
      first.push_back(snap->get(ropts(), keys.back(), out) ? std::optional{out}
                                                           : std::nullopt);
      if (first.back()) (void)sh_.validate(keys.back(), *first.back(), "snapshot get");
    }
    const auto from = random_key();
    auto fwd = snap->iter_from(ropts(), from);
    auto rev = snap->riter_from(ropts(), from);
    auto ks = snap->keys_from(ropts(), from);
    std::this_thread::sleep_for(std::chrono::microseconds{rng_.range(0, 2000)});
    for (std::size_t i = 0; i < keys.size(); ++i) {
      Bytes out;
      const bool found = snap->get(ropts(), keys[i], out);
      if (found != first[i].has_value() || (found && out != *first[i]))
        sh_.fail(std::format("snapshot read of '{}' changed between two "
                             "reads", key_str(keys[i])));
    }
    snap.reset();  // the iterators hold their own state
    walk_entries(fwd, false, "snapshot iter_from (snapshot destroyed)");
    walk_entries(rev, true, "snapshot riter_from (snapshot destroyed)");
    walk_keys(ks, false, "snapshot keys_from (snapshot destroyed)");
  }

  struct Pending {
    std::uint16_t owner;
    std::uint64_t counter;
    std::uint64_t durable_after_read;
  };

  EpochShared &sh_;
  const SoakConfig &cfg_;
  Rng rng_;
  std::vector<Pending> pending_;
  std::unordered_map<std::string, std::uint64_t> last_seen_;
};

// ---------------------------------------------------------------------------
// Lifecycle — the only thread that arms faults, so the only one that may
// see the engine degraded outside a window it opened.
// ---------------------------------------------------------------------------

class Lifecycle {
public:
  Lifecycle(EpochShared &sh, std::uint64_t &next_counter)
      : sh_{sh}, cfg_{sh.cfg}, next_counter_{next_counter},
        me_{*sh.writers.back()},
        rng_{cfg_.seed ^ splitmix64((std::uint64_t{cfg_.epoch} << 16) | 0x200U)} {}

  void run() {
    while (!sh_.stopping()) {
      if (sh_.db.is_degraded()) {
        sh_.fail(std::format("engine degraded outside an injected fault: {}",
                             sh_.db.degraded_reason()));
        return;
      }
      const auto op = rng_.range(0, 99);
      const auto *name = op < 25   ? "vacuum"
                         : op < 45 ? "snapshot"
                         : op < 55 ? "mode"
                         : op < 65 ? "manifest"
                         : op < 80 ? "degrade"
                         : op < 90 ? "stats"
                                   : "write";
      try {
        if (!enabled(name)) {
          // Skipped, but the draw is kept so the rest of the run is the
          // same sequence as with every operation on.
        } else if (op < 25) {
          do_vacuum();
        } else if (op < 45) {
          do_snapshot_churn();
        } else if (op < 55) {
          do_mode_round_trip();
        } else if (op < 65) {
          do_manifest();
        } else if (op < 80) {
          do_degrade_resume();
        } else if (op < 90) {
          (void)sh_.db.stats();
          (void)sh_.db.durable_sequence(sh_.db.durable_sequence() + 1, 1ms);
        } else {
          do_own_write();
        }
      } catch (const std::exception &e) {
        sh_.fail(std::format("lifecycle {}: unexpected exception: {}", name,
                             e.what()));
        return;
      }
      std::this_thread::sleep_for(std::chrono::microseconds{rng_.range(0, 3000)});
    }
    snaps_.clear();
  }

private:
  // BYTECASK_SOAK_LIFECYCLE, a comma-separated list, narrows the lifecycle
  // thread to the named operations when bisecting a failure.
  static auto enabled(std::string_view name) -> bool {
    const char *v = std::getenv("BYTECASK_SOAK_LIFECYCLE");  // NOLINT(concurrency-mt-unsafe)
    if (v == nullptr || *v == '\0') return true;
    const auto list = std::string{","} + v + ",";
    return list.find(std::string{","} + std::string{name} + ",")
           != std::string::npos;
  }

  auto new_counter() -> std::uint64_t {
    const auto c = next_counter_++;
    me_.attempted.store(c, std::memory_order_release);
    return c;
  }

  void do_vacuum() {
    const bytecask::VacuumOptions vo{
        .fragmentation_threshold =
            rng_.pick<double>(std::array{0.0, 0.1, 0.5})};
    for (auto n = rng_.range(1, 8); n > 0 && !sh_.stopping(); --n) {
      if (!sh_.db.vacuum(vo)) break;
    }
  }

  void do_snapshot_churn() {
    if (snaps_.size() < 8 && rng_.chance(0.6)) {
      snaps_.push_back(sh_.db.snapshot());
    } else if (!snaps_.empty()) {
      // Read through an old snapshot before letting it go: its files may
      // have been vacuumed since.
      const auto i = static_cast<std::size_t>(rng_.range(0, snaps_.size() - 1));
      std::uint64_t n = 0;
      for (const auto &[key, value] : snaps_[i].iter_from({})) {
        (void)sh_.validate(key, value, "held snapshot iter_from");
        if (++n > 32) break;
      }
      snaps_.erase(snaps_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }

  void do_mode_round_trip() {
    sh_.db.set_mode(bytecask::Mode::Follower);
    if (sh_.db.mode() != bytecask::Mode::Follower)
      sh_.fail("set_mode(Follower) did not take");
    std::this_thread::sleep_for(std::chrono::microseconds{rng_.range(0, 2000)});
    sh_.db.set_mode(bytecask::Mode::Leader);
    if (sh_.db.mode() != bytecask::Mode::Leader)
      sh_.fail("set_mode(Leader) did not take");
  }

  void do_manifest() {
    auto m = sh_.db.create_manifest();
    if (m.through_sequence > sh_.db.durable_sequence())
      sh_.fail(std::format("create_manifest: through_sequence {} above "
                           "durable_sequence() {}",
                           m.through_sequence, sh_.db.durable_sequence()));
    // Vacuum runs only on this thread, so every listed file still exists.
    for (const auto &f : m.files) {
      if (!std::filesystem::exists(f.data_path)
          || !std::filesystem::exists(f.hint_path))
        sh_.fail(std::format("create_manifest: file {} missing on disk",
                             f.file_id));
    }
  }

  void do_own_write() {
    const auto c = new_counter();
    const bool sync = rng_.chance(0.5);
    const auto slot = static_cast<std::uint32_t>(rng_.range(0, 15));
    const auto key = make_key(cfg_, kLifecycleOwner, slot);
    const auto val = make_value(kLifecycleOwner, slot, c, sync, 64);
    const auto r = sh_.db.put({.sync = sync}, key, val);
    if (sync) me_.record_sync(c, r.sequence);
    me_.settled.store(c, std::memory_order_release);
  }

  // Arms a fault on this thread, drives this thread's writes into it until
  // the engine degrades, holds it degraded briefly under load, resumes.
  void do_degrade_resume() {
    const auto point = rng_.chance(0.7) ? std::string{"io_data_file_sync"}
                                        : std::string{"io_rotate_file_creation"};
    {
#ifdef BYTECASK_TESTING
      bytecask::testing::ScopedFaultInjector fi{point};
#endif
      for (int attempt = 0; attempt < 64 && !sh_.db.is_degraded(); ++attempt) {
        const auto c = new_counter();
        const auto slot = static_cast<std::uint32_t>(rng_.range(0, 15));
        const auto val = make_value(kLifecycleOwner, slot, c, true, 64);
        try {
          const auto r = sh_.db.put({.sync = true, .solo = rng_.chance(0.5)},
                                    make_key(cfg_, kLifecycleOwner, slot), val);
          me_.record_sync(c, r.sequence);
        } catch (const bytecask::DbDegraded &) {
          // Another path of ours degraded it first.
        } catch (const std::system_error &) {
          // The fault this loop armed.
        }
        me_.settled.store(c, std::memory_order_release);
      }
    }
    if (!sh_.db.is_degraded()) return;  // the fault point never came up
    sh_.injected_degrades.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::microseconds{rng_.range(0, 5000)});
    sh_.db.resume();
    if (sh_.db.is_degraded())
      sh_.fail(std::format("resume() returned but the engine is still "
                           "degraded: {}", sh_.db.degraded_reason()));
  }

  EpochShared &sh_;
  const SoakConfig &cfg_;
  std::uint64_t &next_counter_;
  WriterShared &me_;
  Rng rng_;
  std::deque<bytecask::Snapshot> snaps_;
};

// ---------------------------------------------------------------------------
// Epoch driver
// ---------------------------------------------------------------------------

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(std::uint64_t seed)
      : path{std::filesystem::temp_directory_path()
             / std::format("bc_soak_{}_{}", seed,
                           Clock::now().time_since_epoch().count())} {
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    if (keep) {
      std::cerr << std::format("soak: kept {}\n", path.string());
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  bool keep{false};
  TempDir(const TempDir &) = delete;
  auto operator=(const TempDir &) -> TempDir & = delete;
};

// Key -> counter for everything in the DB, every value checked on the way.
auto dump(const bytecask::DB &db, Failures &failures, const SoakConfig &cfg,
          std::string_view where) -> std::map<Bytes, std::uint64_t> {
  std::map<Bytes, std::uint64_t> out;
  for (const auto &[key, value] : db.iter_from({})) {
    auto [id, err] = check_value(key, value);
    if (!id) {
      failures.add(std::format("[{}] {}: key '{}': {}", cfg.describe(), where,
                               key_str(key), err));
      continue;
    }
    out.emplace(Bytes{key.begin(), key.end()}, id->counter);
  }
  return out;
}

auto dump(const bytecask::Snapshot &snap, Failures &failures,
          const SoakConfig &cfg, std::string_view where)
    -> std::map<Bytes, std::uint64_t> {
  std::map<Bytes, std::uint64_t> out;
  for (const auto &[key, value] : snap.iter_from({})) {
    auto [id, err] = check_value(key, value);
    if (!id) {
      failures.add(std::format("[{}] {}: key '{}': {}", cfg.describe(), where,
                               key_str(key), err));
      continue;
    }
    out.emplace(Bytes{key.begin(), key.end()}, id->counter);
  }
  return out;
}

// First few differences between two dumps, for the failure message.
auto diff(const std::map<Bytes, std::uint64_t> &want,
          const std::map<Bytes, std::uint64_t> &got) -> std::string {
  std::string out;
  int n = 0;
  const auto note = [&](const Bytes &k, std::string what) {
    if (n++ < 8) out += std::format("\n      '{}': {}", key_str(k), what);
  };
  for (const auto &[k, c] : want) {
    auto it = got.find(k);
    if (it == got.end()) note(k, std::format("counter {} missing", c));
    else if (it->second != c)
      note(k, std::format("counter {} became {}", c, it->second));
  }
  for (const auto &[k, c] : got)
    if (!want.contains(k)) note(k, std::format("counter {} appeared", c));
  if (n > 8) out += std::format("\n      ... {} differences in all", n);
  return out;
}

void raise_fd_limit() {
  rlimit rl{};
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    (void)::setrlimit(RLIMIT_NOFILE, &rl);
  }
}

}  // namespace

TEST_CASE("chaos soak: concurrent readers, writers and lifecycle",
          "[.soak][soak]") {
  const auto seed = env_u64("BYTECASK_SOAK_SEED").value_or(std::random_device{}());
  const auto budget =
      std::chrono::seconds{env_u64("BYTECASK_SOAK_SECONDS").value_or(20)};
  std::cerr << std::format("soak: seed={} seconds={}\n", seed, budget.count());
  raise_fd_limit();

  Failures failures;
  const auto deadline = Clock::now() + budget;
  std::vector<WriterPersistent> writer_state;
  std::uint64_t lifecycle_counter = 1;
  // BYTECASK_SOAK_KEEP=1 leaves a failed epoch's directory on disk.
  const bool keep_failed = env_u64("BYTECASK_SOAK_KEEP").value_or(0) != 0;

  const auto first_epoch =
      static_cast<unsigned>(env_u64("BYTECASK_SOAK_EPOCH").value_or(0));
  for (unsigned epoch = first_epoch;
       Clock::now() < deadline && !failures.any(); ++epoch) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    auto cfg = make_config(seed, epoch, std::max(remaining, 200ms));
    std::cerr << std::format("soak: {}\n", cfg.describe());

    // Each epoch starts from an empty directory: its configuration (key and
    // value limits, file size) may not admit the previous epoch's data.
    TempDir dir{seed};
    dir.keep = keep_failed;  // cleared once the epoch passes
    writer_state.assign(cfg.writers, {});
    for (auto &w : writer_state)
      w.slots.assign(cfg.slots_per_writer, SlotModel{});

    std::map<Bytes, std::uint64_t> before_close;
    std::uint64_t injected = 0;
    std::optional<bytecask::Snapshot> survivor;
    {
      auto db = bytecask::DB::open(dir.path, cfg.options());
      EpochShared sh{cfg, db, failures, {}, {}, {}};
      for (unsigned i = 0; i <= cfg.writers; ++i)
        sh.writers.push_back(std::make_unique<WriterShared>());

      {
        std::vector<std::unique_ptr<Writer>> writers;
        std::vector<std::unique_ptr<Reader>> readers;
        for (unsigned i = 0; i < cfg.writers; ++i)
          writers.push_back(std::make_unique<Writer>(
              sh, static_cast<std::uint16_t>(i), writer_state[i]));
        for (unsigned i = 0; i < cfg.readers; ++i)
          readers.push_back(std::make_unique<Reader>(sh, i));
        Lifecycle lifecycle{sh, lifecycle_counter};
        // Declared last so it is destroyed — joined — before the objects
        // its threads run on.
        std::vector<std::jthread> threads;

        const auto guarded = [&](std::function<void()> fn) {
          return [&sh, body = std::move(fn)] {
            try {
              body();
            } catch (const std::exception &e) {
              sh.fail(std::format("thread escaped with: {}", e.what()));
            }
          };
        };
        for (auto &w : writers)
          threads.emplace_back(guarded([&w] { w->run(); }));
        for (auto &r : readers)
          threads.emplace_back(guarded([&r] { r->run(); }));
        threads.emplace_back(guarded([&lifecycle] { lifecycle.run(); }));

        const auto epoch_end = Clock::now() + cfg.duration;
        while (Clock::now() < epoch_end && !sh.stopping())
          std::this_thread::sleep_for(20ms);
        sh.stop.store(true, std::memory_order_release);
      }  // join

      if (failures.any()) break;
      injected = sh.injected_degrades.load();
      if (db.is_degraded())
        failures.add(std::format("[{}] degraded at epoch end: {}",
                                 cfg.describe(), db.degraded_reason()));

      // Every writer's known keys hold what the writer believes.
      for (unsigned w = 0; w < cfg.writers; ++w) {
        for (std::uint32_t s = 0; s < cfg.slots_per_writer; ++s) {
          const auto &m = writer_state[w].slots[s];
          if (m.state == SlotModel::State::Unknown) continue;
          const auto key = make_key(cfg, static_cast<std::uint16_t>(w), s);
          Bytes out;
          const bool found = db.get({}, key, out);
          const auto id = found ? check_value(key, out).first : std::nullopt;
          const bool ok = m.state == SlotModel::State::Absent
                              ? !found
                              : id && id->counter == m.counter;
          if (!ok)
            failures.add(std::format(
                "[{}] epoch end: key '{}' expected {}, found {}",
                cfg.describe(), key_str(key),
                m.state == SlotModel::State::Absent
                    ? std::string{"absent"}
                    : std::format("counter {}", m.counter),
                !found ? std::string{"absent"}
                : id   ? std::format("counter {}", id->counter)
                       : std::string{"a malformed value"}));
        }
      }
      const auto degraded_transitions =
          db.stats().at("bytecask.degraded_transitions");
      if (std::cmp_not_equal(degraded_transitions, injected))
        failures.add(std::format("[{}] {} degraded transitions for {} "
                                 "injected faults",
                                 cfg.describe(), degraded_transitions,
                                 injected));

      before_close = dump(db, failures, cfg, "dump before close");
      survivor.emplace(db.snapshot());
    }  // close

    // After a clean close every hint file indexes a data file that exists.
    for (const auto &e : std::filesystem::directory_iterator{dir.path}) {
      if (e.path().extension() != ".hint") continue;
      auto data = e.path();
      data.replace_extension(".data");
      if (!std::filesystem::exists(data))
        failures.add(std::format("[{}] after close: orphan hint file {}",
                                 cfg.describe(),
                                 e.path().filename().string()));
    }

    // A snapshot outlives the DB it came from.
    if (auto got = dump(*survivor, failures, cfg, "snapshot after close");
        got != before_close)
      failures.add(std::format("[{}] snapshot read after DB destruction "
                               "disagrees with the DB before close:{}",
                               cfg.describe(), diff(before_close, got)));

    // Reopen under a different recovery fan-out: same contents.
    auto reopen_opts = cfg.options();
    reopen_opts.recovery_threads = cfg.recovery_threads % 8 + 1;
    {
      auto db = bytecask::DB::open(dir.path, reopen_opts);
      if (auto got = dump(db, failures, cfg, "dump after reopen");
          got != before_close)
        failures.add(std::format("[{}] reopen with recovery_threads={} "
                                 "recovered different contents:{}",
                                 cfg.describe(), reopen_opts.recovery_threads,
                                 diff(before_close, got)));
    }
    survivor.reset();
    if (failures.any()) break;
    dir.keep = false;
    std::cerr << std::format("soak: epoch {} ok ({} keys, {} injected "
                             "degrades)\n",
                             epoch, before_close.size(), injected);
  }

  INFO("seed " << seed << " — rerun with BYTECASK_SOAK_SEED=" << seed);
  if (failures.any()) FAIL("soak failures:\n" << failures.text());
  SUCCEED();
}
