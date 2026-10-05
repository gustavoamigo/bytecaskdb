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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
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
using bytecask::testing::make_durable;
using bytecask::testing::OwnedEntries;
using bytecask::testing::ScopedSyscallFaults;
using bytecask::testing::SyscallFault;
using bytecask::testing::SyscallFaultReport;
using bytecask::testing::to_bytes;

using KeyValues = std::map<std::string, Bytes>;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "sweep_test_{}_{}",
                 std::chrono::system_clock::now().time_since_epoch().count(),
                 next_id())} {
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }

 private:
  static auto next_id() -> unsigned {
    static unsigned counter = 0;
    return counter++;
  }
};

auto bytes(std::string_view sv) -> Bytes {
  const auto bv = to_bytes(sv);
  return {bv.begin(), bv.end()};
}

// DB is neither copyable nor movable; the sweep needs one it can drop.
auto open_db(const std::filesystem::path &dir, const Options &opts)
    -> std::unique_ptr<DB> {
  return std::unique_ptr<DB>{new DB(DB::open(dir, opts))};
}

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

// The hint worker's calls are counted too. Waiting for it before arming, and
// again before disarming, keeps an operation's calls the same set on every
// run; only their order against the caller's can move.
void wait_hints_idle(const DB &db) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (db.stats().at("bytecask.hint_backlog") != 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      FAIL("the hint worker did not go idle");
    }
    std::this_thread::sleep_for(std::chrono::microseconds{200});
  }
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

// ---- Operations on an open DB ---------------------------------------------

struct Operation {
  std::string name;
  Options opts{};
  // Builds the starting state and makes it durable.
  std::function<void(DB &)> setup;
  // The operation under fault. Returns whether it did its work, which a run
  // the fault never reached must.
  std::function<bool(DB &)> run;
  // The baseline plus the whole transition.
  std::function<KeyValues(KeyValues)> transition;
  // False for a follower, which takes no writes but ingest.
  bool takes_writes{true};
  // States short of the whole transition that resume() or a power cut may
  // leave after a failure: an operation that puts several atomic units in
  // the file can fail with the first of them complete. Empty for an
  // operation that writes one unit.
  std::function<std::vector<KeyValues>(const KeyValues &)> partial{};
  // Runs the operation again after the fault, as a caller that got the error
  // would. Whatever the failure left, the whole transition must follow.
  std::function<void(DB &)> retry{};
};

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

// Six keys, a delete and a batch in one file, all durable.
void seed(DB &db) {
  for (int i = 0; i < 6; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{}", i)),
           to_bytes(std::format("v{}", i)));
  }
  (void)db.del({.sync = false}, to_bytes("k5"));
  WritePlan plan;
  plan.put(to_bytes("b0"), to_bytes("x"));
  plan.put(to_bytes("b1"), to_bytes("y"));
  (void)db.apply_batch({.sync = false}, std::move(plan));
  make_durable(db);
}

// A batch of every write type.
auto mixed_plan() -> WritePlan {
  WritePlan plan;
  plan.put(to_bytes("n0"), to_bytes("new"));
  plan.put(to_bytes("k0"), to_bytes("over"));
  plan.del(to_bytes("b0"));
  plan.del_range(to_bytes("k2"), to_bytes("k4"));
  return plan;
}

auto mixed_transition(KeyValues kv) -> KeyValues {
  kv["n0"] = bytes("new");
  kv["k0"] = bytes("over");
  kv.erase("b0");
  kv.erase("k2");
  kv.erase("k3");
  return kv;
}

// Leaves the engine degraded with a write the caller never saw succeed.
// `mode` chooses what is in the file: the whole entry, unsynced (a failed
// fdatasync), or the first bytes of it (a short append).
void degrade(DB &db, bytecask::testing::PostWriteMode mode) {
  using bytecask::testing::PostWriteMode;
  using bytecask::testing::ScopedFaultInjector;
  if (mode == PostWriteMode::none) {
    ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(
        db.put({.sync = true}, to_bytes("pending"), to_bytes("p")),
        std::system_error);
  } else {
    ScopedFaultInjector fi{"io_data_file_append_partial", mode, 7};
    REQUIRE_THROWS_AS(
        db.put({.sync = true}, to_bytes("pending"), to_bytes("p")),
        std::system_error);
  }
  REQUIRE(db.is_degraded());
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

TEST_CASE("fault sweep: put", "[fault_sweep]") {
  sweep({.name = "put",
         .setup = seed,
         .run =
             [](DB &db) {
               return db.put({.sync = true}, to_bytes("k1"), to_bytes("over"))
                   .durable;
             },
         .transition = [](KeyValues kv) {
           kv["k1"] = bytes("over");
           return kv;
         }});
}

TEST_CASE("fault sweep: del", "[fault_sweep]") {
  sweep({.name = "del",
         .setup = seed,
         .run =
             [](DB &db) {
               const auto r = db.del({.sync = true}, to_bytes("k1"));
               return r && r->durable;
             },
         .transition = [](KeyValues kv) {
           kv.erase("k1");
           return kv;
         }});
}

TEST_CASE("fault sweep: del_range", "[fault_sweep]") {
  sweep({.name = "del_range",
         .setup = seed,
         .run =
             [](DB &db) {
               return db
                   .del_range({.sync = true}, to_bytes("k1"), to_bytes("k3"))
                   .durable;
             },
         .transition = [](KeyValues kv) {
           kv.erase("k1");
           kv.erase("k2");
           return kv;
         }});
}

TEST_CASE("fault sweep: apply_batch", "[fault_sweep]") {
  sweep({.name = "apply_batch",
         .setup = seed,
         .run =
             [](DB &db) {
               const auto r = db.apply_batch({.sync = true}, mixed_plan());
               return r && r->durable;
             },
         .transition = mixed_transition});
}

// A write that fills the active file: the append, the rotation it triggers,
// and the sealed file's hint, written by the worker.
TEST_CASE("fault sweep: write across a rotation", "[fault_sweep]") {
  sweep({.name = "rotation-crossing write",
         .opts = {.max_file_bytes = 512},
         .setup = seed,
         .run =
             [](DB &db) {
               const std::string big(600, 'x');
               const auto files = db.file_stats().size();
               const auto r =
                   db.put({.sync = true}, to_bytes("big"), to_bytes(big));
               return r.durable && db.file_stats().size() > files;
             },
         .transition = [](KeyValues kv) {
           kv["big"] = bytes(std::string(600, 'x'));
           return kv;
         }});
}

TEST_CASE("fault sweep: vacuum", "[fault_sweep]") {
  sweep({.name = "vacuum",
         .opts = {.max_file_bytes = 50},
         .setup =
             [](DB &db) {
               db.put({.sync = false}, to_bytes("k0"), to_bytes("v_k0"));
               db.put({.sync = false}, to_bytes("k1"), to_bytes("v_k1"));
               (void)db.del({.sync = false}, to_bytes("k1"));
               make_durable(db);
             },
         .run = [](DB &db) {
           return db.vacuum({.fragmentation_threshold = 0.0});
         },
         .transition = [](KeyValues kv) { return kv; }});
}

TEST_CASE("fault sweep: create_manifest", "[fault_sweep]") {
  sweep({.name = "create_manifest",
         .setup = seed,
         .run = [](DB &db) { return !db.create_manifest().files.empty(); },
         .transition = [](KeyValues kv) { return kv; }});
}

TEST_CASE("fault sweep: ingest", "[fault_sweep]") {
  // The leader's stream in two slices: the follower's baseline, then the
  // slice ingested under fault, a put and a batch. A slice is published in
  // one step, and a failed ingest publishes none of it; but on disk only each
  // unit is atomic, so resume() may replay the put without the batch.
  // Delivering the slice again then completes it (CONTRACT.md, ingest).
  OwnedEntries first;
  OwnedEntries second;
  {
    TempDir td;
    auto leader = DB::open(td.path / "leader");
    seed(leader);
    const auto upto = leader.durable_sequence();
    {
      const auto snap = leader.snapshot();
      first = bytecask::testing::collect_changes(leader.changes_since(snap, 0));
    }
    leader.put({.sync = true}, to_bytes("k1"), to_bytes("over"));
    REQUIRE(leader.apply_batch({.sync = true}, mixed_plan()));
    const auto snap = leader.snapshot();
    second = bytecask::testing::collect_changes(leader.changes_since(snap, upto));
  }
  REQUIRE_FALSE(first.entries.empty());
  REQUIRE_FALSE(second.entries.empty());

  sweep({.name = "ingest",
         .opts = {.initial_mode = bytecask::Mode::Follower},
         .setup = [&](DB &db) { db.ingest(first.views()); },
         .run =
             [&](DB &db) {
               db.ingest(second.views());
               return db.durable_sequence() == second.entries.back().sequence;
             },
         .transition =
             [&](KeyValues kv) {
               for (const auto &e : second.entries) {
                 bytecask::testing::apply_replicated(kv, e.entry_type, e.key,
                                                     e.value);
               }
               return kv;
             },
         .takes_writes = false,
         .partial =
             [](const KeyValues &baseline) {
               auto put_only = baseline;
               put_only["k1"] = bytes("over");
               return std::vector<KeyValues>{put_only};
             },
         .retry = [&](DB &db) { db.ingest(second.views()); }});
}

// resume() itself under fault, from each state a failed write leaves: the
// whole entry in the file but unsynced, and a torn one.
TEST_CASE("fault sweep: resume after a failed sync", "[fault_sweep]") {
  sweep({.name = "resume (unsynced entry)",
         .setup =
             [](DB &db) {
               seed(db);
               degrade(db, bytecask::testing::PostWriteMode::none);
             },
         .run =
             [](DB &db) {
               db.resume();
               return !db.is_degraded();
             },
         .transition = [](KeyValues kv) {
           kv["pending"] = bytes("p");
           return kv;
         }});
}

TEST_CASE("fault sweep: resume after a torn append", "[fault_sweep]") {
  sweep({.name = "resume (torn entry)",
         .setup =
             [](DB &db) {
               seed(db);
               degrade(db, bytecask::testing::PostWriteMode::short_write);
             },
         .run =
             [](DB &db) {
               db.resume();
               return !db.is_degraded();
             },
         .transition = [](KeyValues kv) { return kv; }});
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

// Three sealed files and an active one, with every write type in them.
auto fill_files(DB &db) -> KeyValues {
  seed(db);
  for (int i = 0; i < 12; ++i) {
    db.put({.sync = false}, to_bytes(std::format("f{:02}", i)),
           to_bytes(std::string(90, static_cast<char>('a' + i))));
  }
  REQUIRE(db.apply_batch({.sync = false}, mixed_plan()));
  make_durable(db);
  wait_hints_idle(db);
  REQUIRE(db.file_stats().size() >= 3);
  return key_values(db);
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
