// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Counted fault sweep (#317): for each operation, fail its 1st, 2nd, …, n-th
// I/O call in turn, until a run completes without reaching the fault. The
// calls are counted below the engine (syscall_faults.h), so no checkpoint has
// to name them. After each failure only generic invariants are checked: the
// state is the baseline or the baseline plus the whole transition, a degraded
// engine resumes, a reopen recovers the same state serial and parallel, and
// what a power cut leaves obeys the watermark rule. Each operation is swept
// once per I/O back-end.
// See docs/correctness_validation.md, "Counted fault sweep".

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "fault_injector.h"
#include "syscall_faults.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

import bytecask;

#include "proof/invariants.h"
#include "sweep_operations.h"

namespace {

using bytecask::Bytes;
using bytecask::DB;
using bytecask::Options;
using bytecask::WritePlan;
using bytecask::testing::assert_consistent;
using bytecask::testing::assert_hints_durable;
using bytecask::testing::assert_power_loss_outcome;
using bytecask::testing::assert_resumable;
using bytecask::testing::check_key_values;
using bytecask::testing::durable_watermark;
using bytecask::testing::EngineFingerprint;
using bytecask::testing::fingerprint;
using bytecask::testing::key_values;
using bytecask::testing::ScopedSyscallFaults;
using bytecask::testing::SyscallFault;
using bytecask::testing::SyscallFaultReport;
using bytecask::testing::to_bytes;

using namespace bytecask::testing::sweep_ops;  // NOLINT(google-build-using-namespace)

// The I/O back-ends make different calls on the same operation: the buffer
// pool opens a sealed file O_DIRECT, falls back, and fills frames with its
// own pread; the mmap back-end maps the active file and each sealed one.
struct Backend {
  const char *name;
  bytecask::IoBackend io;
};

constexpr Backend kBackends[] = {
    {"pread", bytecask::IoBackend::Pread},
    {"buffer pool", bytecask::IoBackend::BufferPool},
    {"mmap", bytecask::IoBackend::Mmap},
};

// The pool must hold 2 x max_file_bytes. An operation on the default file
// size gets a small one, so the pool, and each run, stays cheap; none of
// them comes near it.
constexpr std::uint64_t kPoolFileBytes = 64 * 1024;
constexpr std::size_t kPoolBytes = 1024 * 1024;

auto on_backend(Options opts, const Backend &backend) -> Options {
  opts.io_backend = backend.io;
  if (backend.io == bytecask::IoBackend::BufferPool) {
    opts.max_file_bytes = std::min(opts.max_file_bytes, kPoolFileBytes);
    opts.buffer_pool.capacity_bytes = kPoolBytes;
  }
  return opts;
}

auto with_backend(std::string_view name, const Backend &backend)
    -> std::string {
  return std::format("{} ({})", name, backend.name);
}

// The pass a sweep makes over an operation's calls.
struct Pass {
  const char *name;
  SyscallFault mode;
  bool cascade;
};

constexpr Pass kPasses[] = {
    {"before", SyscallFault::before, false},
    {"after", SyscallFault::after, false},
    {"short", SyscallFault::short_io, false},
    {"before, and every call after it", SyscallFault::before, true},
};

// No operation here makes more calls than this; a sweep that gets there is
// not converging.
constexpr int kMaxCalls = 2000;

// BYTECASK_SWEEP_TRACE=1 prints each failed call: what the sweep covered.
void trace(std::string_view op, const Pass &pass, int n,
           const SyscallFaultReport &rep, bool threw) {
  static const bool on = std::getenv("BYTECASK_SWEEP_TRACE") != nullptr;
  if (!on) return;
  std::fprintf(stderr, "sweep %.*s [%s] N=%d %s%s\n",
               static_cast<int>(op.size()), op.data(), pass.name, n,
               rep.fired ? rep.what.c_str() : "(not reached)",
               threw ? " -> threw" : "");
}

auto same_stats(const bytecask::FileStats &a, const bytecask::FileStats &b)
    -> bool {
  return a.live_bytes == b.live_bytes && a.total_bytes == b.total_bytes &&
         a.min_sequence == b.min_sequence && a.max_sequence == b.max_sequence &&
         a.tombstone_bytes == b.tombstone_bytes &&
         a.marker_bytes == b.marker_bytes;
}

// Opens two copies of what a clean close left in `dir`, one with serial
// recovery and one with parallel, and holds both to `expected` and to each
// other, file stats included. Each open adds an active file of its own, so
// only the files both saw are compared.
void assert_reopens(const std::filesystem::path &dir, const KeyValues &expected,
                    std::uint64_t expected_next_seq, Options opts) {
  const auto twin = dir.parent_path() / (dir.filename().string() + "_parallel");
  std::filesystem::remove_all(twin);
  std::filesystem::copy(dir, twin);

  auto recover = [&](const std::filesystem::path &d, unsigned threads) {
    opts.recovery_threads = threads;
    auto db = DB::open(d, opts);
    assert_consistent(db);
    return fingerprint(db);
  };
  const auto serial = recover(dir, 1);
  const auto parallel = recover(twin, 4);

  check_key_values(serial.key_values, expected, "serial recovery");
  check_key_values(parallel.key_values, expected, "parallel recovery");
  if (expected_next_seq != 0) {
    INFO("next_seq must survive recovery");
    CHECK(serial.next_seq == expected_next_seq);
  }
  CHECK(parallel.next_seq == serial.next_seq);
  for (const auto &[stem, fs] : serial.file_stats) {
    const auto it = parallel.file_stats.find(stem);
    if (it == parallel.file_stats.end()) continue;
    INFO("file_stats for " << stem << " must match, serial and parallel");
    CHECK(same_stats(fs, it->second));
  }
}

auto one_of(const KeyValues &got, const std::vector<KeyValues> &states)
    -> bool {
  return std::ranges::find(states, got) != states.end();
}

// One run of `op` with its n-th call failing. Returns false once the
// operation completes without reaching the fault.
auto sweep_step(const Operation &op, const Pass &pass, int n) -> bool {
  TempDir td;
  const auto dir = td.path / "db";
  const auto cut = td.path / "cut";
  const auto cut_resumed = td.path / "cut_resumed";
  bytecask::testing::ScopedPageCacheModel cache;

  SyscallFaultReport rep;
  KeyValues baseline;
  KeyValues after;
  KeyValues final_state;
  std::vector<KeyValues> partial_states;
  std::uint64_t final_next_seq = 0;
  bytecask::testing::PowerLossExpectation at_cut;
  {
    auto db = open_db(dir, op.opts);
    op.setup(*db);
    wait_hints_idle(*db);
    baseline = key_values(*db);
    after = op.transition(baseline);
    if (op.partial) partial_states = op.partial(baseline);

    bool threw = false;
    bool did_work = false;
    {
      ScopedSyscallFaults faults{dir, pass.mode, n, pass.cascade};
      try {
        did_work = op.run(*db);
      } catch (const std::exception &) {
        threw = true;
      }
      wait_hints_idle(*db);
      rep = faults.report();
    }
    trace(op.name, pass, n, rep, threw);
    INFO(op.name << ", fault " << pass.name << ", N = " << n << ": "
                 << (rep.fired ? rep.what : "not reached"));
    if (!threw && op.returned) op.returned(after);
    if (!rep.fired) {
      REQUIRE_FALSE(threw);
      REQUIRE(did_work);
      REQUIRE_FALSE(db->is_degraded());
    }

    // The baseline or the whole transition, and the transition if the
    // operation returned.
    const auto live = key_values(*db);
    if (threw && live == baseline) {
      // Nothing published.
    } else {
      check_key_values(live, after, "the operation's whole transition");
    }
    assert_consistent(*db);

    const bool changed = !threw && after != baseline;
    at_cut = {.baseline = baseline,
              .after = after,
              .transition_last_seq =
                  changed ? db->engine_state()->next_seq - 1 : 0,
              .watermark = durable_watermark(*db)};
    if (changed) {
      INFO("a sync operation that returned must be durable");
      CHECK(at_cut.watermark >= at_cut.transition_last_seq);
    }
    cache.model.copy_device(dir, cut);  // power cut

    if (db->is_degraded()) {
      assert_resumable(*db);
      const auto resumed = key_values(*db);
      if (live == baseline &&
          (resumed == baseline || one_of(resumed, partial_states))) {
        // The transition never reached the file whole.
      } else {
        check_key_values(resumed, after, "resume() keeps or completes the transition");
      }
    }

    if (op.retry) {
      op.retry(*db);
      check_key_values(key_values(*db), after, "the operation, run again");
      assert_consistent(*db);
    }

    // The engine goes on: a write after the fault lands and recovers.
    if (op.takes_writes) {
      const auto r = db->put({.sync = true}, to_bytes("~probe"), to_bytes("p"));
      CHECK(r.durable);
    }
    final_state = key_values(*db);
    final_next_seq = db->engine_state()->next_seq;
    cache.model.copy_device(dir, cut_resumed);  // power cut
  }
  INFO(op.name << ", fault " << pass.name << ", N = " << n << ": "
               << (rep.fired ? rep.what : "not reached"));
  assert_hints_durable(cache.model);
  assert_reopens(dir, final_state, final_next_seq, op.opts);
  {
    INFO("the cut taken after the fault, before resume()");
    const auto durable =
        at_cut.transition_last_seq > 0 &&
        at_cut.watermark >= at_cut.transition_last_seq;
    if (!durable && !partial_states.empty() &&
        one_of(key_values(DB::open(cut, op.opts)), partial_states)) {
      // The device holds the transition's first units, whole.
    } else {
      assert_power_loss_outcome(cut, at_cut, op.opts);
    }
  }
  {
    // Everything published by then was synced, or resume() made it durable.
    INFO("the cut taken at the end");
    auto recovered = DB::open(cut_resumed, op.opts);
    check_key_values(key_values(recovered), final_state, "the device at the end");
    assert_consistent(recovered);
  }
  return rep.fired;
}

void sweep(Operation op) {
  const auto backend = GENERATE(from_range(kBackends));
  op.name = with_backend(op.name, backend);
  op.opts = on_backend(op.opts, backend);
  for (const auto &pass : kPasses) {
    int n = 1;
    while (sweep_step(op, pass, n)) {
      ++n;
      REQUIRE(n <= kMaxCalls);
    }
  }
}

}  // namespace

// The interposers must see the calls the engine makes: if a toolchain spells
// one differently (pread64, a fortified variant), nothing is counted and the
// sweep proves nothing.
TEST_CASE("fault sweep: the interposers count the engine's calls",
          "[fault_sweep]") {
  TempDir td;
  const auto dir = td.path / "db";

  ScopedSyscallFaults count_open{dir, SyscallFault::none, 0};
  auto db = open_db(dir, {});
  CHECK(count_open.report().calls > 0);

  {
    ScopedSyscallFaults count_put{dir, SyscallFault::after, 0};
    db->put({.sync = true}, to_bytes("k"), to_bytes("v"));
    // At least the append and its fdatasync.
    CHECK(count_put.report().calls >= 2);
  }
  {
    ScopedSyscallFaults fail_sync{dir, SyscallFault::before, 1, true};
    CHECK_THROWS_AS(db->put({.sync = true}, to_bytes("k"), to_bytes("w")),
                    std::system_error);
    const auto rep = fail_sync.report();
    CHECK(rep.fired);
    INFO(rep.what);
    CHECK(rep.what.find(dir.string()) != std::string::npos);
  }
  // Calls outside the directory are not the sweep's.
  {
    ScopedSyscallFaults elsewhere{td.path / "other", SyscallFault::before, 1,
                                  true};
    CHECK_NOTHROW(db->resume());
    CHECK(elsewhere.report().calls == 0);
  }
}

// The C++ standard library is linked statically so that std::filesystem's
// calls reach the interposers too (xmake.lua). Linked against the shared
// libstdc++, none of these would be counted.
TEST_CASE("fault sweep: the interposers count std::filesystem's calls",
          "[fault_sweep]") {
  TempDir td;
  const auto dir = td.path / "db";
  std::filesystem::create_directories(dir);
  const auto from = dir / "a.tmp";
  const auto to = dir / "a";
  { std::ofstream{from} << "x"; }

  // names: the calls the standard library may make for it, any of which the
  // interposers must fail.
  auto fails = [&](SyscallFault mode, auto &&call,
                   std::initializer_list<std::string_view> names) {
    ScopedSyscallFaults faults{dir, mode, 1};
    CHECK_THROWS_AS(call(), std::filesystem::filesystem_error);
    const auto rep = faults.report();
    INFO(rep.what);
    CHECK(std::ranges::any_of(names, [&](std::string_view name) {
      return rep.what.starts_with(name);
    }));
  };
  // Before glibc 2.33, stat is an inline wrapper around __xstat, which
  // --wrap=stat does not see (xmake.lua, fault_sweep_link_guard): there the
  // call goes uncounted and nothing fails.
  auto stat_fails = [&](auto &&call) {
    {
      ScopedSyscallFaults probe{dir, SyscallFault::none, 0};
      (void)call();
      if (probe.report().calls == 0) {
        WARN("stat is not interposed on this libc");
        return;
      }
    }
    fails(SyscallFault::before, call, {"stat("});
  };
  stat_fails([&] { return std::filesystem::file_size(from); });
  stat_fails([&] { return std::filesystem::exists(from); });
  // libstdc++ opens the directory with openat, libc++ (the MemorySanitizer
  // build) with opendir.
  fails(SyscallFault::before,
        [&] { (void)std::filesystem::directory_iterator{dir}; },
        {"openat(", "opendir("});
  fails(SyscallFault::before,
        [&] { (void)std::filesystem::create_directory(dir / "sub"); },
        {"mkdir("});
  // Made, then reported failed: the rename landed.
  fails(SyscallFault::after, [&] { std::filesystem::rename(from, to); },
        {"rename("});
  CHECK(std::filesystem::exists(to));
  fails(SyscallFault::before, [&] { (void)std::filesystem::remove(to); },
        {"remove("});
  CHECK(std::filesystem::exists(to));
}

TEST_CASE("fault sweep: put", "[fault_sweep]") {
  sweep(put_operation());
}

TEST_CASE("fault sweep: del", "[fault_sweep]") {
  sweep(del_operation());
}

TEST_CASE("fault sweep: del_range", "[fault_sweep]") {
  sweep(del_range_operation());
}

TEST_CASE("fault sweep: apply_batch", "[fault_sweep]") {
  sweep(apply_batch_operation());
}

TEST_CASE("fault sweep: write across a rotation", "[fault_sweep]") {
  sweep(rotation_operation());
}

TEST_CASE("fault sweep: vacuum", "[fault_sweep]") {
  sweep(vacuum_operation());
}

TEST_CASE("fault sweep: create_manifest", "[fault_sweep]") {
  sweep(manifest_operation());
}

TEST_CASE("fault sweep: ingest", "[fault_sweep]") {
  sweep(ingest_operation());
}

TEST_CASE("fault sweep: resume after a failed sync", "[fault_sweep]") {
  sweep(resume_unsynced_operation());
}

TEST_CASE("fault sweep: resume after a torn append", "[fault_sweep]") {
  sweep(resume_torn_operation());
}

// close() with an unsynced batch behind it. It returns only if every
// acknowledged write is durable; it is closed either way; and a reopen in the
// same process, where the page cache still holds the batch, recovers it.
TEST_CASE("fault sweep: close", "[fault_sweep]") {
  const auto backend = GENERATE(from_range(kBackends));
  const auto name = with_backend("close", backend);
  const auto opts = on_backend({.max_file_bytes = 512}, backend);
  for (const auto &pass : kPasses) {
    for (int n = 1;; ++n) {
      REQUIRE(n <= kMaxCalls);
      TempDir td;
      const auto dir = td.path / "db";
      const auto cut = td.path / "cut";
      bytecask::testing::ScopedPageCacheModel cache;

      SyscallFaultReport rep;
      KeyValues baseline;
      KeyValues full;
      bool threw = false;
      {
        auto db = open_db(dir, opts);
        seed(*db);
        baseline = key_values(*db);
        REQUIRE(db->apply_batch({.sync = false}, mixed_plan()));
        wait_hints_idle(*db);
        full = key_values(*db);
        {
          ScopedSyscallFaults faults{dir, pass.mode, n, pass.cascade};
          try {
            db->close();
          } catch (const std::exception &) {
            threw = true;
          }
          rep = faults.report();
        }
        trace(name, pass, n, rep, threw);
        INFO(name << ", fault " << pass.name << ", N = " << n << ": "
                             << (rep.fired ? rep.what : "not reached"));
        if (!rep.fired) REQUIRE_FALSE(threw);
        CHECK_THROWS_AS(db->put({}, to_bytes("late"), to_bytes("x")),
                        bytecask::DbClosed);
        cache.model.copy_device(dir, cut);  // power cut
      }
      INFO(name << ", fault " << pass.name << ", N = " << n << ": "
                           << (rep.fired ? rep.what : "not reached"));
      assert_hints_durable(cache.model);
      {
        auto recovered = DB::open(cut, opts);
        const auto got = key_values(recovered);
        if (threw && got == baseline) {
          // close() reported that the batch may not be durable.
        } else {
          check_key_values(got, full, "a close that returned made it durable");
        }
        assert_consistent(recovered);
      }
      assert_reopens(dir, full, 0, opts);
      if (!rep.fired) break;
    }
  }
}

namespace {

// DB::open under fault, on copies of one directory. A failed open must lose
// nothing: the open that follows it, and what a power cut leaves of it,
// recover every key.
void sweep_open(const std::filesystem::path &shape, const KeyValues &expected,
                const Options &opts, std::string_view name) {
  for (const auto &pass : kPasses) {
    for (int n = 1;; ++n) {
      REQUIRE(n <= kMaxCalls);
      TempDir td;
      const auto dir = td.path / "db";
      const auto cut = td.path / "cut";
      std::filesystem::copy(shape, dir);
      bytecask::testing::ScopedPageCacheModel cache;

      SyscallFaultReport rep;
      bool threw = false;
      {
        std::unique_ptr<DB> db;
        {
          ScopedSyscallFaults faults{dir, pass.mode, n, pass.cascade};
          try {
            db = open_db(dir, opts);
          } catch (const std::exception &) {
            threw = true;
          }
          rep = faults.report();
        }
        trace(name, pass, n, rep, threw);
        INFO(name << ", fault " << pass.name << ", N = " << n << ": "
                  << (rep.fired ? rep.what : "not reached"));
        if (!rep.fired) REQUIRE_FALSE(threw);
        if (db) {
          check_key_values(key_values(*db), expected, "an open that returned");
          assert_consistent(*db);
          CHECK_FALSE(db->is_degraded());
        }
        cache.model.copy_device(dir, cut);  // power cut
      }
      INFO(name << ", fault " << pass.name << ", N = " << n << ": "
                << (rep.fired ? rep.what : "not reached"));
      assert_hints_durable(cache.model);
      {
        auto recovered = DB::open(cut, opts);
        check_key_values(key_values(recovered), expected, "the device after the open");
        assert_consistent(recovered);
      }
      assert_reopens(dir, expected, 0, opts);
      if (!rep.fired) break;
    }
  }
}

}  // namespace

TEST_CASE("fault sweep: open after a clean close", "[fault_sweep]") {
  const auto backend = GENERATE(from_range(kBackends));
  const Options opts{.max_file_bytes = 512};
  TempDir td;
  const auto shape = td.path / "shape";
  KeyValues expected;
  {
    auto db = DB::open(shape, opts);
    expected = fill_files(db);
  }
  auto parallel = on_backend(opts, backend);
  auto serial = parallel;
  serial.recovery_threads = 1;
  sweep_open(shape, expected, serial,
             with_backend("open (clean, serial)", backend));
  sweep_open(shape, expected, parallel,
             with_backend("open (clean, parallel)", backend));
}

// The directory a killed process leaves: the file that was active has no
// hint and a preallocated tail, so open rewrites it, syncs it, cuts the tail
// and generates the hint.
TEST_CASE("fault sweep: open after a crash", "[fault_sweep]") {
  const auto backend = GENERATE(from_range(kBackends));
  const Options opts{.max_file_bytes = 512};
  TempDir td;
  const auto shape = td.path / "shape";
  KeyValues expected;
  {
    auto db = DB::open(td.path / "live", opts);
    expected = fill_files(db);
    std::filesystem::copy(td.path / "live", shape);
  }
  auto parallel = on_backend(opts, backend);
  auto serial = parallel;
  serial.recovery_threads = 1;
  sweep_open(shape, expected, serial,
             with_backend("open (crashed, serial)", backend));
  sweep_open(shape, expected, parallel,
             with_backend("open (crashed, parallel)", backend));
}

// ---- Chained counted faults (#355) ---------------------------------------
//
// The sweep above starts every step from a fresh database: one fault, one
// resume, done. This keeps one database and applies a seeded sequence of
// faults to it, so the state a fault meets is the state earlier faults left:
// files sealed by resume(), sequences advanced through failures, file_stats
// built across truncations, hints of cut files, and a fault landing inside
// resume() itself.
//
// A cycle arms one fault (a mode and a gap of counted calls) and runs
// operations from the sweep's table until it fires. A cycle that leaves the
// engine degraded is not resumed until the next cycle, whose armed fault is
// therefore counted through resume(); a resume the fault interrupted is
// retried with the fault lifted, bounded. After each fault the live state
// must be the baseline or the whole transition, and every kChainReopenEvery-th
// a cut is checked against the watermark rule and its settled state reopened
// serial and parallel, file_stats included.
//
// The gap is log-uniform over 1..kMaxGap so a run reaches both faults inside
// one operation and faults several operations apart. The seed and fault
// count come from BYTECASK_SWEEP_CHAIN_SEED and
// BYTECASK_SWEEP_CHAIN_FAULTS; BYTECASK_SWEEP_CHAIN_TRACE=1 prints each
// fault. The (call, mode) histogram at the end shows what the run reached.

namespace {

constexpr int kDefaultChainFaults = 60;
constexpr int kChainReopenEvery = 20;
constexpr int kMaxGap = 1000;
// The most operations one fault window runs before its gap is recorded as not
// reached, or the gap itself when that is larger.
constexpr int kMaxOpsPerFault = 256;
// A resume() a fault interrupted is retried with the fault lifted; this many
// failures in a row is a bug, not a retry.
constexpr int kMaxResumeRetries = 5;

// Unset is the default; a set value that is not an integer is a typo in a
// test knob, not a reason to run something else silently.
auto env_int(const char *name) -> std::optional<int> {
  const char *v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  if (v == nullptr || *v == '\0') return std::nullopt;
  std::size_t used = 0;
  int value = 0;
  try {
    value = std::stoi(v, &used);
  } catch (const std::exception &) {
    used = 0;
  }
  if (used == 0 || v[used] != '\0')
    throw std::invalid_argument{
        std::format("{} must be an integer, got '{}'", name, v)};
  return value;
}

// The call a report names: "fdatasync" from "fdatasync(/db/x.data)".
auto call_of(std::string_view what) -> std::string {
  return std::string{what.substr(0, what.find('('))};
}

auto mode_name(SyscallFault mode) -> const char * {
  switch (mode) {
    case SyscallFault::none:
      return "none";
    case SyscallFault::before:
      return "before";
    case SyscallFault::after:
      return "after";
    case SyscallFault::short_io:
      return "short";
  }
  return "none";
}

struct FaultPlan {
  SyscallFault mode;
  int gap;
};

// Log-uniform over 1..kMaxGap: every order of magnitude is equally likely, so
// a run reaches a fault on the next call and one a thousand calls away.
auto draw_fault(std::mt19937_64 &rng) -> FaultPlan {
  constexpr std::array<SyscallFault, 3> kModes = {
      SyscallFault::before, SyscallFault::after, SyscallFault::short_io};
  const auto mode = kModes[std::uniform_int_distribution<std::size_t>{
      0, kModes.size() - 1}(rng)];
  const auto exponent =
      std::uniform_real_distribution<double>{0.0, std::log2(kMaxGap)}(rng);
  const auto gap = static_cast<int>(std::llround(std::pow(2.0, exponent)));
  return {.mode = mode, .gap = std::clamp(gap, 1, kMaxGap)};
}

// The state is read through the engine, so its calls must not count against
// the armed fault: a check would otherwise consume the gap it measures.
auto suspended_key_values(const DB &db) -> KeyValues {
  const bytecask::testing::SuspendSyscallFaults uncounted;
  return key_values(db);
}

// One operation under the armed fault: what it started from, the whole
// transition it owes, and the last sequence it assigned.
struct Step {
  KeyValues before;
  KeyValues after;
  std::uint64_t last_seq{0};
  bool threw{false};
  const Operation *op{nullptr};
};

auto run_step(DB &db, const Operation &op, const KeyValues &model) -> Step {
  Step s{.before = model, .after = op.transition(model), .op = &op};
  const auto seq_before = db.engine_state()->next_seq;
  try {
    (void)op.run(db);
  } catch (const std::exception &) {
    s.threw = true;
  }
  const auto seq_after = db.engine_state()->next_seq;
  s.last_seq = seq_after > seq_before ? seq_after - 1 : 0;
  return s;
}

// What the engine may hold between cycles: the visible state, and the pair a
// degraded resume() may still settle it between.
struct ChainState {
  KeyValues model;
  bool pending{false};
  KeyValues pending_after;
  std::uint64_t pending_last_seq{0};
};

// Checks what a step left and narrows `state` to it. A degraded engine that
// still shows the pre-step state may complete the transition in resume(), so
// the pair stays allowed until then; a published transition stays required.
void settle(DB &db, const Step &s, ChainState &state) {
  const auto live = suspended_key_values(db);
  if (s.threw) {
    INFO("a faulted step leaves the baseline or the whole transition");
    REQUIRE((live == s.before || live == s.after));
  } else {
    check_key_values(live, s.after, "the operation's whole transition");
    REQUIRE(live == s.after);
  }
  state.model = live;
  state.pending = db.is_degraded() && live == s.before;
  if (state.pending) {
    state.pending_after = s.after;
    state.pending_last_seq = s.last_seq;
  }
}

// One armed fault window: the step it left, whether it interrupted resume(),
// and its report.
struct Window {
  Step step;
  bool fault_in_resume{false};
  SyscallFaultReport report;
};

// Arms one fault and runs operations until it fires. A degraded engine's
// resume() is the first operation, so the gap counts through it; a fault that
// interrupts it is retried by the caller with the fault lifted.
auto run_window(DB &db, const std::filesystem::path &dir,
                const std::vector<Operation> &ops, const FaultPlan &plan,
                bool degraded, ChainState &state, std::mt19937_64 &rng)
    -> Window {
  Window w;
  w.step = {.before = state.model,
            .after = state.pending ? state.pending_after : state.model,
            .last_seq = state.pending ? state.pending_last_seq : 0};
  ScopedSyscallFaults faults{dir, plan.mode, plan.gap};
  if (degraded) {
    try {
      db.resume();
    } catch (const std::exception &) {
      w.step.threw = true;
    }
    w.fault_in_resume = w.step.threw;
    if (w.step.threw && !faults.fired())
      FAIL("resume() threw without the armed fault firing");
    if (!w.step.threw) {
      const auto live = suspended_key_values(db);
      INFO("resume() keeps or completes the transition");
      REQUIRE((live == w.step.before || live == w.step.after));
      state.model = live;
      state.pending = false;
      w.step = {.before = state.model, .after = state.model, .last_seq = 0};
    }
  }
  if (!w.step.threw) {
    const auto max_ops = std::max(kMaxOpsPerFault, plan.gap);
    for (int i = 0; i < max_ops && !faults.fired(); ++i) {
      if (db.is_degraded()) break;
      const auto &op = ops[std::uniform_int_distribution<std::size_t>{
          0, ops.size() - 1}(rng)];
      w.step = run_step(db, op, state.model);
      if (faults.fired()) break;
      if (w.step.threw)
        FAIL("an operation threw without the armed fault firing");
      settle(db, w.step, state);
    }
  }
  wait_hints_idle(db);
  w.report = faults.report();
  return w;
}

// A write after the fault lands, and the sequence a dropped transition
// advanced is represented on disk: recovery reconstructs next_seq from the
// highest sequence it finds, so a probe at the current next_seq keeps the
// settled cut's next_seq equal to the live one. The hint worker must finish
// before the probe syncs the file a resume() sealed, or the probe's sync hides
// a hint written over pages the device did not hold.
void probe_after_fault(DB &db, ChainState &state,
                       bytecask::testing::PageCacheModel &cache) {
  wait_hints_idle(db);
  assert_hints_durable(cache);
  const auto r = db.put({.sync = true}, to_bytes("~probe"), to_bytes("p"));
  REQUIRE(r.durable);
  state.model["~probe"] = bytes("p");
  wait_hints_idle(db);
}

// A resume() the fault interrupted is retried with the fault lifted; one that
// still fails is a bug, not a retry.
void retry_resume(DB &db) {
  for (int attempt = 0; attempt < kMaxResumeRetries; ++attempt) {
    try {
      db.resume();
      return;
    } catch (const std::exception &) {
    }
  }
  FAIL("resume() did not recover with the fault lifted");
}

void run_chain(const Backend &backend, std::uint64_t seed, int faults) {
  TempDir td;
  const auto dir = td.path / "db";
  bytecask::testing::ScopedPageCacheModel cache;
  auto opts = on_backend(Options{.max_file_bytes = 512}, backend);
  std::mt19937_64 rng{seed};

  const std::vector<Operation> ops = {
      put_operation(),     del_operation(),     del_range_operation(),
      apply_batch_operation(), rotation_operation(), vacuum_operation(),
      manifest_operation()};

  std::map<std::string, int> histogram;
  int reached = 0;
  int missed = 0;
  int resume_faults = 0;
  ChainState state;
  KeyValues &model = state.model;
  std::uint64_t next_seq = 0;

  {
    auto db = open_db(dir, opts);
    bytecask::testing::sweep_ops::seed(*db);
    wait_hints_idle(*db);
    model = suspended_key_values(*db);
    next_seq = db->engine_state()->next_seq;

    for (int cycle = 0; reached < faults && cycle < faults * 8; ++cycle) {
      // A faulted write leaves the engine degraded for the next cycle, whose
      // resume is then the first operation the armed fault counts through.
      const bool degraded = db->is_degraded();

      const auto plan = draw_fault(rng);
      const auto window =
          run_window(*db, dir, ops, plan, degraded, state, rng);
      const Step &step = window.step;
      const bool fault_in_resume = window.fault_in_resume;
      const auto &rep = window.report;

      if (!rep.fired) {
        ++missed;
        INFO("fault [" << mode_name(plan.mode) << "] gap " << plan.gap
                       << " was not reached");
        CHECK_FALSE(db->is_degraded());
        continue;
      }

      ++reached;
      if (fault_in_resume) ++resume_faults;
      histogram[std::format("{} [{}]", call_of(rep.what),
                            mode_name(plan.mode))]++;
      if (std::getenv("BYTECASK_SWEEP_CHAIN_TRACE") != nullptr) {  // NOLINT
        std::fprintf(stderr, "chain %s fault %d [%s] gap=%d %s%s\n",
                     backend.name, reached, mode_name(plan.mode), plan.gap,
                     rep.what.c_str(), fault_in_resume ? " (resume)" : "");
      }

      // Every kChainReopenEvery-th fault, a cut taken before resume(): the
      // baseline or the whole transition, under the watermark rule.
      const bool sampled = reached % kChainReopenEvery == 0;
      if (sampled) {
        const auto cut = td.path / std::format("cut_{}", reached);
        cache.model.copy_device(dir, cut);
        INFO("fault " << reached << " [" << mode_name(plan.mode) << "] "
                      << rep.what);
        assert_power_loss_outcome(
            cut,
            {.baseline = step.before,
             .after = step.after,
             .transition_last_seq = step.last_seq,
             .watermark = durable_watermark(*db)},
            opts);
      }

      // What a returned operation produced, once its calls no longer count.
      if (step.op != nullptr && !step.threw && step.op->returned)
        step.op->returned(step.after);

      assert_consistent(*db);
      settle(*db, step, state);

      // A fault that interrupted resume() is retried with the fault lifted.
      // One that interrupted a write is not: the next cycle's resume runs
      // under a fresh fault, so a fault can land inside it from the state the
      // earlier faults built.
      if (fault_in_resume && db->is_degraded()) {
        retry_resume(*db);
        const auto live = suspended_key_values(*db);
        REQUIRE((live == model ||
                 (state.pending && live == state.pending_after)));
        model = live;
        state.pending = false;
      }

      // A write after the fault lands, and the sequence a dropped transition
      // advanced is represented on disk.
      if (!db->is_degraded()) probe_after_fault(*db, state, cache.model);
      next_seq = db->engine_state()->next_seq;

      if (sampled && !db->is_degraded()) {
        const auto settled = td.path / std::format("settled_{}", reached);
        cache.model.copy_device(dir, settled);
        INFO("the settled state after " << reached << " faults");
        assert_reopens(settled, model, next_seq, opts);
      }
    }

    if (db->is_degraded()) {
      INFO("the chain ends degraded; resume() must recover it");
      assert_resumable(*db);
      const auto live = suspended_key_values(*db);
      REQUIRE((live == model ||
               (state.pending && live == state.pending_after)));
      model = live;
      const auto r =
          db->put({.sync = true}, to_bytes("~probe"), to_bytes("p"));
      REQUIRE(r.durable);
      model["~probe"] = bytes("p");
    }
    wait_hints_idle(*db);
    next_seq = db->engine_state()->next_seq;

    // A power cut at the end, through the page-cache model, then the settled
    // state reopened serial and parallel.
    {
      const auto cut = td.path / "cut_end";
      cache.model.copy_device(dir, cut);
      assert_power_loss_outcome(cut, {.baseline = model, .after = model},
                                opts);
    }
    {
      const auto settled = td.path / "settled_end";
      cache.model.copy_device(dir, settled);
      assert_reopens(settled, model, next_seq, opts);
    }
  }

  assert_hints_durable(cache.model);
  assert_reopens(dir, model, next_seq, opts);
  CHECK(reached == faults);

  std::fprintf(stderr,
               "chained sweep (%s): seed=%llu faults=%d reached=%d missed=%d "
               "resume=%d\n",
               backend.name, static_cast<unsigned long long>(seed), faults,
               reached, missed, resume_faults);
  for (const auto &[what, n] : histogram)
    std::fprintf(stderr, "  %-24s %d\n", what.c_str(), n);
}

}  // namespace

TEST_CASE("fault sweep: a chained run of faults",
          "[fault_sweep][fault_sweep_chain]") {
  const auto seed = static_cast<std::uint64_t>(
      env_int("BYTECASK_SWEEP_CHAIN_SEED").value_or(355));
  const auto faults =
      env_int("BYTECASK_SWEEP_CHAIN_FAULTS").value_or(kDefaultChainFaults);
  REQUIRE(faults > 0);
  for (const auto &backend : kBackends) {
    INFO("backend " << backend.name);
    run_chain(backend, seed, faults);
  }
}
