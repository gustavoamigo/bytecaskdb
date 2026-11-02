// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The operations the counted sweeps run under fault, with their starting
// states and the transition each makes: the I/O sweep (fault_sweep_test.cpp)
// and the allocation-failure sweep (alloc_sweep_test.cpp) run the same ones.
//
// Requires the including translation unit to `import bytecask;` and include
// proof/invariants.h before this header.

#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fault_injector.h"
#include <catch2/catch_test_macros.hpp>

namespace bytecask::testing::sweep_ops {

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
  TempDir(const TempDir &) = delete;
  auto operator=(const TempDir &) -> TempDir & = delete;

 private:
  static auto next_id() -> unsigned {
    static unsigned counter = 0;
    return counter++;
  }
};

inline auto bytes(std::string_view sv) -> Bytes {
  const auto bv = to_bytes(sv);
  return {bv.begin(), bv.end()};
}

// DB is neither copyable nor movable; the sweep needs one it can drop.
inline auto open_db(const std::filesystem::path &dir, const Options &opts)
    -> std::unique_ptr<DB> {
  return std::unique_ptr<DB>{new DB(DB::open(dir, opts))};
}

// The hint worker's calls and allocations are counted too. Waiting for it
// before arming, and again before disarming, keeps an operation's set the
// same on every run; only their order against the caller's can move. Its own
// polling (stats() builds a map) is not counted.
inline void wait_hints_idle(const DB &db) {
  const SuspendSyscallFaults polling;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (db.stats().at("bytecask.hint_backlog") != 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      FAIL("the hint worker did not go idle");
    }
    std::this_thread::sleep_for(std::chrono::microseconds{200});
  }
}

struct Operation {
  std::string name;
  Options opts{};
  // Builds the starting state and makes it durable.
  std::function<void(DB &)> setup;
  // The operation under fault. Returns whether it did its work, which a run
  // the fault never reached must. Builds its inputs under
  // SuspendSyscallFaults, so only the engine's calls and allocations count.
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
  // Checks what a run that returned produced, given the whole transition,
  // once the fault is disarmed: a check run under it would have its own
  // calls counted and failed.
  std::function<void(const KeyValues &)> returned{};
  // An I/O failure injected while `run` runs, with the allocation fault: the
  // handler for it allocates too. An engine whose injected failure was
  // thrown must refuse writes after, whatever else failed.
  std::function<std::unique_ptr<ScopedFaultInjector>()> io_fault{};
  // The injected failure comes once the operation's own write is durable:
  // a run where it was thrown may show the whole transition (CONTRACT.md,
  // a write whose rotation failed after it was synced), or the baseline when
  // there was no memory to publish it. Otherwise only the baseline.
  bool io_fault_after_durable{false};
};

// Six keys, a delete and a batch in one file, all durable.
inline void seed(DB &db) {
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
inline auto mixed_plan() -> WritePlan {
  WritePlan plan;
  plan.put(to_bytes("n0"), to_bytes("new"));
  plan.put(to_bytes("k0"), to_bytes("over"));
  plan.del(to_bytes("b0"));
  plan.del_range(to_bytes("k2"), to_bytes("k4"));
  return plan;
}

inline auto mixed_transition(KeyValues kv) -> KeyValues {
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
inline void degrade(DB &db, PostWriteMode mode) {
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

// Three sealed files and an active one, with every write type in them.
inline auto fill_files(DB &db) -> KeyValues {
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

// ---- The operations ------------------------------------------------------

inline auto put_operation() -> Operation {
  return {.name = "put",
          .setup = seed,
          .run =
              [](DB &db) {
                return db.put({.sync = true}, to_bytes("k1"), to_bytes("over"))
                    .durable;
              },
          .transition = [](KeyValues kv) {
            kv["k1"] = bytes("over");
            return kv;
          }};
}

// A put while a snapshot taken before it is held, and released while the
// fault is still armed: the put's retired nodes park on the snapshot's key
// directory version, and its release hands them on or frees them (#390).
inline auto snapshot_put_operation() -> Operation {
  return {.name = "put under a snapshot",
          .setup = seed,
          .run =
              [](DB &db) {
                std::optional<Snapshot> snap;
                {
                  const SuspendSyscallFaults inputs;
                  snap.emplace(db.snapshot());
                }
                const auto r =
                    db.put({.sync = true}, to_bytes("k1"), to_bytes("over"));
                snap.reset();
                return r.durable;
              },
          .transition = [](KeyValues kv) {
            kv["k1"] = bytes("over");
            return kv;
          }};
}

inline auto del_operation() -> Operation {
  return {.name = "del",
          .setup = seed,
          .run =
              [](DB &db) {
                const auto r = db.del({.sync = true}, to_bytes("k1"));
                return r && r->durable;
              },
          .transition = [](KeyValues kv) {
            kv.erase("k1");
            return kv;
          }};
}

inline auto del_range_operation() -> Operation {
  return {.name = "del_range",
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
          }};
}

inline auto apply_batch_operation() -> Operation {
  return {.name = "apply_batch",
          .setup = seed,
          .run =
              [](DB &db) {
                auto plan = [] {
                  const SuspendSyscallFaults inputs;
                  return mixed_plan();
                }();
                const auto r = db.apply_batch({.sync = true}, std::move(plan));
                return r && r->durable;
              },
          .transition = mixed_transition};
}

// A batch that splits nodes its own earlier writes made the transient's: the
// first put copies the seed's single leaf, and the puts after it fill that
// copy until it splits, so the split replaces a node the transient owns and
// the root over the halves is allocated after it. The seed alone is one
// leaf that never fills, which is why no other operation reaches it.
inline auto splitting_batch_operation() -> Operation {
  constexpr int kPuts = 80;  // the seed's 8 keys and these overfill a leaf
  auto key = [](int i) { return std::format("s{:02}", i * 37 % kPuts); };
  return {.name = "apply_batch that splits what it changed",
          .setup = seed,
          .run =
              [key](DB &db) {
                auto plan = [&] {
                  const SuspendSyscallFaults inputs;
                  WritePlan p;
                  for (int i = 0; i < kPuts; ++i)
                    p.put(to_bytes(key(i)), to_bytes("s"));
                  return p;
                }();
                const auto r = db.apply_batch({.sync = true}, std::move(plan));
                return r && r->durable;
              },
          .transition = [key](KeyValues kv) {
            for (int i = 0; i < kPuts; ++i) kv[key(i)] = bytes("s");
            return kv;
          }};
}

// A write that fills the active file: the append, the rotation it triggers,
// and the sealed file's hint, written by the worker.
inline auto rotation_operation() -> Operation {
  return {.name = "rotation-crossing write",
          .opts = {.max_file_bytes = 512},
          .setup = seed,
          .run =
              [](DB &db) {
                std::string big;
                std::size_t files = 0;
                {
                  const SuspendSyscallFaults inputs;
                  big.assign(600, 'x');
                  files = db.file_stats().size();
                }
                const auto r =
                    db.put({.sync = true}, to_bytes("big"), to_bytes(big));
                const SuspendSyscallFaults check;
                return r.durable && db.file_stats().size() > files;
              },
          .transition = [](KeyValues kv) {
            kv["big"] = bytes(std::string(600, 'x'));
            return kv;
          }};
}

inline auto vacuum_operation() -> Operation {
  return {.name = "vacuum",
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
          .transition = [](KeyValues kv) { return kv; }};
}

// The leader's stream in two slices: the follower's baseline, then the slice
// ingested under fault, a put and a batch. A slice is published in one step,
// and a failed ingest publishes none of it; but on disk only each unit is
// atomic, so resume() may replay the put without the batch. Delivering the
// slice again then completes it (CONTRACT.md, ingest).
struct IngestSlices {
  std::shared_ptr<OwnedEntries> first = std::make_shared<OwnedEntries>();
  std::shared_ptr<OwnedEntries> second = std::make_shared<OwnedEntries>();
};

inline auto ingest_slices() -> IngestSlices {
  IngestSlices slices;
  {
    TempDir td;
    auto leader = DB::open(td.path / "leader");
    seed(leader);
    const auto upto = leader.durable_sequence();
    {
      const auto snap = leader.snapshot();
      *slices.first = collect_changes(leader.changes_since(snap, 0));
    }
    leader.put({.sync = true}, to_bytes("k1"), to_bytes("over"));
    REQUIRE(leader.apply_batch({.sync = true}, mixed_plan()));
    const auto snap = leader.snapshot();
    *slices.second = collect_changes(leader.changes_since(snap, upto));
  }
  REQUIRE_FALSE(slices.first->entries.empty());
  REQUIRE_FALSE(slices.second->entries.empty());
  return slices;
}

inline auto ingest_operation() -> Operation {
  const auto [first, second] = ingest_slices();

  return {.name = "ingest",
          .opts = {.initial_mode = Mode::Follower},
          .setup = [first](DB &db) { db.ingest(first->header, first->views()); },
          .run =
              [second](DB &db) {
                auto views = [&] {
                  const SuspendSyscallFaults inputs;
                  return second->views();
                }();
                db.ingest(second->header, views);
                return db.durable_sequence() ==
                       second->entries.back().sequence;
              },
          .transition =
              [second](KeyValues kv) {
                for (const auto &e : second->entries) {
                  apply_replicated(kv, e.entry_type, e.key, e.value);
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
          .retry = [second](DB &db) { db.ingest(second->header, second->views()); }};
}

// An append that fails with its bytes in the file, as a failed writev does
// once it has written: the engine must refuse writes, with or without the
// memory to say why.
inline auto failed_append_fault() -> std::unique_ptr<ScopedFaultInjector> {
  return std::make_unique<ScopedFaultInjector>("io_data_file_append_partial",
                                               PostWriteMode::throw_after);
}

inline auto failed_sync_fault() -> std::unique_ptr<ScopedFaultInjector> {
  return std::make_unique<ScopedFaultInjector>("io_data_file_sync");
}

inline auto failed_rotation_fault() -> std::unique_ptr<ScopedFaultInjector> {
  return std::make_unique<ScopedFaultInjector>("io_rotate_file_creation");
}

// The second fdatasync fails: for a write that crosses a rotation, the one
// of the next file, after the rotation's own.
inline auto failed_second_sync_fault() -> std::unique_ptr<ScopedFaultInjector> {
  auto fi = std::make_unique<ScopedFaultInjector>("io_data_file_sync");
  fi->inj.fail_on_nth_match = 2;
  return fi;
}

// The group commit's fdatasync of a sync write fails (flush_failed).
inline auto put_failed_sync_operation() -> Operation {
  auto op = put_operation();
  op.name = "put whose fdatasync fails";
  op.io_fault = failed_sync_fault;
  return op;
}

// The rotation's fdatasync, before the file is sealed, fails.
inline auto rotation_failed_sync_operation() -> Operation {
  auto op = rotation_operation();
  op.name = "rotation-crossing write whose rotation fdatasync fails";
  op.io_fault = failed_sync_fault;
  return op;
}

// The write is synced and the file sealed; the next file cannot be created.
inline auto rotation_failed_creation_operation() -> Operation {
  auto op = rotation_operation();
  op.name = "rotation-crossing write whose next file cannot be created";
  op.io_fault = failed_rotation_fault;
  op.io_fault_after_durable = true;
  return op;
}

// The rotation succeeds; the sync that follows it fails.
inline auto rotation_failed_second_sync_operation() -> Operation {
  auto op = rotation_operation();
  op.name = "rotation-crossing write whose sync after the rotation fails";
  op.io_fault = failed_second_sync_fault;
  return op;
}

// After the fault, the follower is delivered the slice again and promoted,
// and a durable write follows: what an engine that took the failed ingest's
// bytes for its own would lose.
inline void promote_after_retry(Operation &op) {
  op.takes_writes = true;
  op.retry = [retry = op.retry](DB &db) {
    retry(db);
    db.set_mode(Mode::Leader);
  };
}

inline auto ingest_failed_append_operation() -> Operation {
  auto op = ingest_operation();
  op.name = "ingest whose append fails";
  op.io_fault = failed_append_fault;
  promote_after_retry(op);
  return op;
}

inline auto ingest_failed_sync_operation() -> Operation {
  auto op = ingest_operation();
  op.name = "ingest whose fdatasync fails";
  op.io_fault = failed_sync_fault;
  promote_after_retry(op);
  return op;
}

// A file small enough that the slice rotates it, and the rotation's new
// file cannot be created.
inline auto ingest_failed_rotation_operation() -> Operation {
  auto op = ingest_operation();
  op.name = "ingest whose rotation cannot create a file";
  op.opts.max_file_bytes = 300;
  op.io_fault = failed_rotation_fault;
  promote_after_retry(op);
  return op;
}

inline auto ingest_failed_rotation_sync_operation() -> Operation {
  auto op = ingest_failed_rotation_operation();
  op.name = "ingest whose rotation fdatasync fails";
  op.io_fault = failed_sync_fault;
  return op;
}

// A follower's promotion appends its ChangeMarker; the append fails.
inline auto promotion_failed_append_operation() -> Operation {
  const auto first = ingest_slices().first;
  return {.name = "set_mode(Leader) whose marker append fails",
          .opts = {.initial_mode = Mode::Follower},
          .setup = [first](DB &db) { db.ingest(first->header, first->views()); },
          .run =
              [](DB &db) {
                db.set_mode(Mode::Leader);
                return db.mode() == Mode::Leader;
              },
          .transition = [](KeyValues kv) { return kv; },
          .retry = [](DB &db) { db.set_mode(Mode::Leader); },
          .io_fault = failed_append_fault};
}

// A promotion whose marker takes the follower's file past its limit, and the
// next file cannot be created. The limit is one byte past what the slice
// leaves in the file, measured on a copy.
inline auto promotion_failed_rotation_operation() -> Operation {
  const auto first = ingest_slices().first;
  std::uint64_t slice_bytes = 0;
  {
    TempDir td;
    auto probe = DB::open(td.path / "probe", {.initial_mode = Mode::Follower});
    probe.ingest(first->header, first->views());
    for (const auto &[id, fs] : probe.file_stats())
      slice_bytes = std::max(slice_bytes, fs.total_bytes);
  }
  auto op = promotion_failed_append_operation();
  op.name = "set_mode(Leader) whose rotation cannot create a file";
  op.opts.max_file_bytes = slice_bytes + 1;
  op.io_fault = failed_rotation_fault;
  return op;
}

// A leader holding an unsynced write steps down, which syncs it; the sync
// fails.
inline auto step_down_failed_sync_operation() -> Operation {
  return {.name = "set_mode(Follower) whose fdatasync fails",
          .setup =
              [](DB &db) {
                seed(db);
                db.put({.sync = false}, to_bytes("u1"), to_bytes("unsynced"));
              },
          .run =
              [](DB &db) {
                db.set_mode(Mode::Follower);
                return db.mode() == Mode::Follower;
              },
          .transition = [](KeyValues kv) { return kv; },
          .takes_writes = false,
          .retry = [](DB &db) { db.set_mode(Mode::Follower); },
          .io_fault = failed_sync_fault};
}

// Vacuum makes the overwrites it judged by durable before it drops the old
// file; that fdatasync fails. Two durable values, then two unsynced
// overwrites: the whole-file path writes no staging copy, so the first data
// file sync is that one.
inline auto vacuum_failed_sync_operation() -> Operation {
  return {.name = "vacuum whose fdatasync fails",
          .opts = {.max_file_bytes = 50},
          .setup =
              [](DB &db) {
                db.put({.sync = true}, to_bytes("k0"), to_bytes("v_k0"));
                db.put({.sync = true}, to_bytes("k1"), to_bytes("v_k1"));
                db.put({.sync = false}, to_bytes("k0"), to_bytes("n0"));
                db.put({.sync = false}, to_bytes("k1"), to_bytes("n1"));
              },
          .run = [](DB &db) {
            return db.vacuum({.fragmentation_threshold = 0.0});
          },
          .transition = [](KeyValues kv) { return kv; },
          .io_fault = failed_sync_fault};
}

// resume() itself under fault, from each state a failed write leaves: the
// whole entry in the file but unsynced, and a torn one.
inline auto resume_unsynced_operation() -> Operation {
  return {.name = "resume (unsynced entry)",
          .setup =
              [](DB &db) {
                seed(db);
                degrade(db, PostWriteMode::none);
              },
          .run =
              [](DB &db) {
                db.resume();
                return !db.is_degraded();
              },
          .transition = [](KeyValues kv) {
            kv["pending"] = bytes("p");
            return kv;
          }};
}

inline auto resume_torn_operation() -> Operation {
  return {.name = "resume (torn entry)",
          .setup =
              [](DB &db) {
                seed(db);
                degrade(db, PostWriteMode::short_write);
              },
          .run =
              [](DB &db) {
                db.resume();
                return !db.is_degraded();
              },
          .transition = [](KeyValues kv) { return kv; }};
}

// Every data file a returned manifest lists exists. A hint may not, when the
// worker failed to write it (#349): copying what is there must still open to
// the manifest's state, the missing hint rebuilt by the open. Checked once
// the fault is disarmed, since the check's own calls would otherwise count.
inline auto manifest_operation() -> Operation {
  auto listed = std::make_shared<std::vector<bytecask::FileInfo>>();
  return {.name = "create_manifest",
          .setup = seed,
          .run =
              [listed](DB &db) {
                auto m = db.create_manifest();
                *listed = std::move(m.files);
                return !listed->empty();
              },
          .transition = [](KeyValues kv) { return kv; },
          .returned =
              [listed](const KeyValues &expected) {
                REQUIRE_FALSE(listed->empty());
                const auto dest =
                    listed->front().data_path.parent_path().parent_path() /
                    "from_manifest";
                std::filesystem::remove_all(dest);
                std::filesystem::create_directories(dest);
                for (const auto &f : *listed) {
                  INFO("manifest file " << f.file_id);
                  REQUIRE(std::filesystem::exists(f.data_path));
                  std::filesystem::copy_file(f.data_path,
                                             dest / f.data_path.filename());
                  if (std::filesystem::exists(f.hint_path))
                    std::filesystem::copy_file(f.hint_path,
                                               dest / f.hint_path.filename());
                }
                auto copy = DB::open(dest, {});
                check_key_values(key_values(copy), expected,
                                 "a DB opened from the manifest's files");
              }};
}

inline auto manifest_failed_rotation_operation() -> Operation {
  auto op = manifest_operation();
  op.name = "create_manifest whose rotation cannot create a file";
  op.io_fault = failed_rotation_fault;
  return op;
}

inline auto manifest_failed_sync_operation() -> Operation {
  auto op = manifest_operation();
  op.name = "create_manifest whose fdatasync fails";
  op.io_fault = failed_sync_fault;
  return op;
}

}  // namespace bytecask::testing::sweep_ops
