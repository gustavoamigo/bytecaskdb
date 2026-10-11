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
inline auto ingest_operation() -> Operation {
  auto first = std::make_shared<OwnedEntries>();
  auto second = std::make_shared<OwnedEntries>();
  {
    TempDir td;
    auto leader = DB::open(td.path / "leader");
    seed(leader);
    const auto upto = leader.durable_sequence();
    {
      const auto snap = leader.snapshot();
      *first = collect_changes(leader.changes_since(snap, 0));
    }
    leader.put({.sync = true}, to_bytes("k1"), to_bytes("over"));
    REQUIRE(leader.apply_batch({.sync = true}, mixed_plan()));
    const auto snap = leader.snapshot();
    *second = collect_changes(leader.changes_since(snap, upto));
  }
  REQUIRE_FALSE(first->entries.empty());
  REQUIRE_FALSE(second->entries.empty());

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

}  // namespace bytecask::testing::sweep_ops
