#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_group_commit.cpp from the group-commit scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/group_commit/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

_project_root = Path(__file__).resolve().parent.parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.group_commit.expected_delta import GroupDelta, group_delta
from tests.proof.group_commit.fault_point_resolver import (
    GroupFaultConfig,
    resolve_group_fault,
)
from tests.proof.group_commit.scenario_matrix import (
    GroupFailureClass,
    GroupShape,
    generate_matrix,
)


def _open_opts(shape: GroupShape) -> str:
    if shape.max_file_bytes is not None:
        return f"{{.max_file_bytes = {shape.max_file_bytes}}}"
    return ""


def gen_injector(config: GroupFaultConfig) -> List[str]:
    """The injector, armed inside the leader's thread body."""
    if config.is_noop:
        return []
    if config.post_write_mode == "short_write":
        return [
            "      using PW = bytecask::testing::PostWriteMode;",
            "      bytecask::testing::ScopedFaultInjector fi{",
            f'          "{config.name}", PW::short_write, {config.short_write_bytes}}};',
        ]
    return [f'      bytecask::testing::ScopedFaultInjector fi{{"{config.name}"}};']


def gen_plan(shape: GroupShape, var: str) -> List[str]:
    """A writer's plan. Writer i writes g{i} (and h{i} when multi_op)."""
    lines = ["      bytecask::WritePlan plan;"]
    lines.append(
        f'      plan.put(to_bytes("g" + {var}), to_bytes("gv" + {var}));'
    )
    if shape.multi_op:
        lines.append(
            f'      plan.put(to_bytes("h" + {var}), to_bytes("hv" + {var}));'
        )
    return lines


def gen_test(shape: GroupShape, failure: GroupFailureClass) -> str:
    delta = group_delta(shape, failure)
    config = resolve_group_fault(failure)
    name = f"prove_group__{shape.label}__{failure.value}"
    sync = "false" if config.use_sync_false else "true"
    n = shape.group_size
    opts = _open_opts(shape)
    open_call = (
        f"bytecask::DB::open(dir, {opts})" if opts else "bytecask::DB::open(dir)"
    )

    p: List[str] = []
    p.append(f'TEST_CASE("{name}", "[prove_group][concurrency]") {{')
    p.append("  TempDir td;")
    p.append('  auto dir = td.path / "db";')
    p.append("  bytecask::testing::EngineFingerprint fp;")
    p.append("  {")
    p.append(f"    auto db = {open_call};")
    p.append("")
    p.append(f"    // Force exactly {n} writers into one batch: the leader blocks in")
    p.append("    // on_leader_start_ until every follower's slot is queued, so the")
    p.append("    // group's shape is deterministic rather than raced for.")
    p.append("    std::mutex mu;")
    p.append("    std::condition_variable cv;")
    p.append("    bool leader_ready = false;")
    p.append("    db.test_write_group().on_leader_start_ = [&] {")
    p.append("      {")
    p.append("        std::lock_guard<std::mutex> lk{mu};")
    p.append("        leader_ready = true;")
    p.append("      }")
    p.append("      cv.notify_all();")
    p.append(f"      db.test_write_group().wait_for_queue_size({n});")
    p.append("    };")
    p.append("")
    p.append(f"    std::array<bool, {n}> threw{{}};")
    p.append(f"    std::array<std::optional<bytecask::CommitResult>, {n}> results{{}};")
    p.append("")
    p.append("    auto write_as = [&](int i, bool is_leader) {")
    p.append('      auto idx = std::to_string(i);')
    p.extend(gen_plan(shape, "idx"))
    p.append("      try {")
    p.append(
        f"        results[static_cast<std::size_t>(i)] ="
        f" db.apply_batch({{.sync = {sync}}}, std::move(plan));"
    )
    p.append("      } catch (const std::exception &) {")
    p.append("        threw[static_cast<std::size_t>(i)] = true;")
    p.append("      }")
    p.append("      (void)is_leader;")
    p.append("    };")
    p.append("")
    inj = gen_injector(config)
    p.append("    std::vector<std::thread> writers;")
    if config.arm_on_all_writers:
        p.append("    // The commit fdatasync is not the leader's: on the non-rotation")
        p.append("    // path execute_slots ends at store_head() and flush_pending()")
        p.append("    // does the sync, run by whichever writer holds the FlushRole.")
        p.append("    // Arming every writer makes the cell deterministic and asks the")
        p.append("    // truer question: whoever ran it, every writer must learn.")
    else:
        p.append("    // Writer 0 submits first and becomes the leader; it is the thread")
        p.append("    // that appends and rotates for the whole group, so the fault is")
        p.append("    // armed there.")
    p.append("    writers.emplace_back([&] {")
    if inj:
        p.append("      {")
        p.extend("  " + line for line in inj)
        p.append("        write_as(0, true);")
        p.append("      }")
    else:
        p.append("      write_as(0, true);")
    p.append("    });")
    p.append(f"    for (int i = 1; i < {n}; ++i) {{")
    p.append("      writers.emplace_back([&, i] {")
    p.append("        std::unique_lock<std::mutex> lk{mu};")
    p.append("        cv.wait(lk, [&] { return leader_ready; });")
    p.append("        lk.unlock();")
    if inj and config.arm_on_all_writers:
        p.append("        {")
        p.extend("  " + line for line in inj)
        p.append("          write_as(i, false);")
        p.append("        }")
    else:
        p.append("        write_as(i, false);")
    p.append("      });")
    p.append("    }")
    p.append("    for (auto &t : writers) t.join();")
    p.append("    db.test_write_group().on_leader_start_ = nullptr;")
    p.append("")
    p.append("    // The group shares one append and one fdatasync, so the engine has")
    p.append("    // no way to tell one writer its write landed and another that it")
    p.append("    // did not. Every writer must have seen the same outcome.")
    p.append(f"    for (std::size_t i = 0; i < {n}; ++i) {{")
    p.append('      INFO("writer " << i << " outcome must match the group");')
    p.append(
        f"      CHECK(threw[i] == {'true' if delta.all_threw else 'false'});"
    )
    p.append("    }")
    p.append("")
    p.append("    // ... and the group's keys are all visible or none are.")
    p.append(f"    for (int i = 0; i < {n}; ++i) {{")
    p.append('      auto idx = std::to_string(i);')
    p.append('      INFO("writer " << i << " key visibility must match the group");')
    visible = "true" if delta.keys_visible else "false"
    p.append(
        f'      CHECK(db.contains_key({{}}, to_bytes("g" + idx)) == {visible});'
    )
    if shape.multi_op:
        p.append(
            f'      CHECK(db.contains_key({{}}, to_bytes("h" + idx)) == {visible});'
        )
    p.append("    }")
    p.append("")
    p.append(
        f"    CHECK(db.is_degraded() == {'true' if delta.degraded else 'false'});"
    )
    if not delta.degraded and sync == "true":
        p.append("    // A committed group that was fdatasync'd reports durable.")
        p.append(f"    for (std::size_t i = 0; i < {n}; ++i) {{")
        p.append('      INFO("writer " << i << " CommitResult");')
        p.append("      REQUIRE(results[i].has_value());")
        p.append("      CHECK(results[i]->durable);")
        p.append("    }")
    p.append("    assert_consistent(db);")
    if delta.degraded:
        p.append("    assert_resumable(db);")
        p.append("    fp = fingerprint(db);")
    else:
        p.append("    fp = fingerprint(db);")
    p.append("  }")
    p.append("  // Whatever the group left on disk, a cold open must agree with it.")
    if opts:
        p.append(f"  assert_matches_recovery(dir, fp, {opts});")
    else:
        p.append("  assert_matches_recovery(dir, fp);")
    p.append("}")
    return "\n".join(p)


FILE_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// AUTO-GENERATED by tests/proof/group_commit/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for group commit under I/O failure. Each test forces
// a deterministic batch of N writers through one leader, faults the group's
// shared I/O on the leader's thread, and verifies that every writer in the
// group saw the same outcome and that the group landed all or nothing.

#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::assert_matches_recovery;
using bytecask::testing::assert_resumable;
using bytecask::testing::fingerprint;
using bytecask::testing::to_bytes;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_group_{}_{}",
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

} // namespace

"""


def generate_file() -> str:
    tests = [gen_test(s, f) for s, f in generate_matrix()]
    return FILE_HEADER + "\n\n".join(tests) + "\n"


def main() -> None:
    out_dir = Path(__file__).resolve().parent.parent / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "prove_group_commit.cpp"
    out.write_text(generate_file())
    print(f"Generated {sum(1 for _ in generate_matrix())} tests → {out}")


if __name__ == "__main__":
    main()
