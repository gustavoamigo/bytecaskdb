#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_apply_batch.cpp from the scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

# Allow running as a script from the project root.
_project_root = Path(__file__).resolve().parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.expected_delta import Delta, expected_delta
from tests.proof.fault_point_resolver import FaultConfig, resolve_fault
from tests.proof.scenario_matrix import (
    FailureClass,
    Observer,
    OpType,
    PlanShape,
    StateShape,
    generate_matrix,
)

# ---------------------------------------------------------------------------
# Key label assignment
# ---------------------------------------------------------------------------


def key_labels_for(plan: PlanShape) -> List[str]:
    """Return the key name assigned to each op in the plan."""
    if plan.op_keys:
        return list(plan.op_keys)
    if plan.causality_key:
        return [plan.causality_key] * len(plan.ops)
    labels: List[str] = []
    put_idx = 0
    for op in plan.ops:
        if op == OpType.PUT:
            if plan.is_conflicting:
                labels.append("k0")
            else:
                labels.append(f"p{put_idx}")
                put_idx += 1
        elif op == OpType.DELETE:
            labels.append("k0")
    return labels


def existing_keys(state: StateShape) -> List[str]:
    """List of key names that exist in the pre-transition state."""
    return [f"k{i}" for i in range(state.num_keys)]


# ---------------------------------------------------------------------------
# C++ code generation
# ---------------------------------------------------------------------------


def _build_open_opts(state: StateShape) -> str:
    """Build the C++ designated-initializer list for Options from state fields."""
    parts: List[str] = []
    if state.max_file_bytes is not None:
        parts.append(f".max_file_bytes = {state.max_file_bytes}")
    if state.io_backend == "mmap":
        parts.append(".io_backend = bytecask::IoBackend::Mmap")
    elif state.io_backend == "buffer_pool":
        parts.append(".io_backend = bytecask::IoBackend::BufferPool")
        parts.append(".buffer_pool = {.capacity_bytes = 1048576}")
    return ", ".join(parts)


def gen_setup(state: StateShape) -> str:
    """Generate C++ to create initial DB state."""
    lines: List[str] = []
    opts = _build_open_opts(state)
    if opts:
        lines.append(
            f"    auto db = bytecask::DB::open(dir, {{{opts}}});"
        )
    else:
        lines.append("    auto db = bytecask::DB::open(dir);")
    for i in range(state.num_keys):
        lines.append(
            f'    db.put({{.sync = false}}, to_bytes("k{i}"), to_bytes("v{i}"));'
        )
    if state.delete_after_create:
        for i in range(state.num_keys):
            lines.append(
                f'    (void)db.del({{.sync = false}}, to_bytes("k{i}"));'
            )
    return "\n".join(lines)


def gen_pre_baseline(plan: PlanShape) -> str:
    """Generate pre-baseline setup (snapshot + conflicting write)."""
    if not plan.is_conflicting and not plan.has_guards:
        return ""
    lines: List[str] = []
    lines.append("    auto snap = db.snapshot();")
    if plan.is_conflicting:
        lines.append(
            '    db.put({.sync = false}, to_bytes("k0"), '
            'to_bytes("conflict"));'
        )
    return "\n".join(lines)


def gen_plan(plan: PlanShape, state: StateShape, labels: List[str]) -> str:
    """Generate C++ to construct a WritePlan (after baseline capture)."""
    lines: List[str] = []

    if plan.is_conflicting or plan.has_guards:
        lines.append("    bytecask::WritePlan plan{std::move(snap)};")
        if plan.has_guards and state.num_keys > 0:
            lines.append('    plan.ensure_unchanged(to_bytes("k0"));')
        if plan.is_conflicting:
            lines.append('    plan.ensure_unchanged(to_bytes("k0"));')
    else:
        lines.append("    bytecask::WritePlan plan;")

    for i, (op, label) in enumerate(zip(plan.ops, labels)):
        if op == OpType.PUT:
            lines.append(
                f'    plan.put(to_bytes("{label}"), to_bytes("new{i}"));'
            )
        elif op == OpType.DELETE:
            lines.append(f'    plan.del(to_bytes("{label}"));')
        elif op == OpType.RANGE_DEL:
            lines.append(
                f'    plan.del_range(to_bytes("{plan.range_from}"), '
                f'to_bytes("{plan.range_to}"));'
            )

    return "\n".join(lines)


def gen_fault_injector(config: FaultConfig) -> str:
    """Generate C++ ScopedFaultInjector declaration."""
    if config.is_noop:
        return ""

    if config.post_write_mode:
        mode = (
            "PW::short_write"
            if config.post_write_mode == "short_write"
            else "PW::throw_after"
        )
        lines = ["      using PW = bytecask::testing::PostWriteMode;"]
        if config.post_write_mode == "short_write":
            lines.append(
                f"      bytecask::testing::ScopedFaultInjector fi{{"
                f'"{config.name}", {mode}, {config.short_write_bytes}}};'
            )
        else:
            lines.append(
                f"      bytecask::testing::ScopedFaultInjector fi{{"
                f'"{config.name}", {mode}}};'
            )
        return "\n".join(lines)

    if config.name:
        return (
            f"      bytecask::testing::ScopedFaultInjector "
            f'fi{{"{config.name}"}};'
        )

    if config.fail_at is not None and config.skip_names:
        skip = ", ".join(f'"{s}"' for s in config.skip_names)
        return (
            f"      bytecask::testing::ScopedFaultInjector "
            f"fi{{{config.fail_at}, {{{skip}}}}};"
        )

    if config.fail_at is not None:
        return (
            f"      bytecask::testing::ScopedFaultInjector "
            f"fi{{{config.fail_at}}};"
        )

    return ""


def gen_observer_acquire(observer: Observer) -> str:
    """C++ that takes a view before the fault-injected call."""
    if observer == Observer.NONE:
        return ""
    if observer == Observer.HELD_VALUE:
        return "\n".join([
            "    bytecask::Bytes obs_value;",
            '    REQUIRE(db.get({}, to_bytes("k0"), obs_value));',
            "    const bytecask::Bytes obs_value_at_acquire = obs_value;",
        ])
    if observer == Observer.HELD_ITER_SPAN:
        return "\n".join([
            "    auto obs_range = db.iter_from({});",
            "    auto obs_it = obs_range.begin();",
            "    REQUIRE_FALSE(obs_it == std::default_sentinel);",
            "    const auto obs_span = (*obs_it).value;",
            "    const bytecask::Bytes obs_span_at_acquire{",
            "        obs_span.begin(), obs_span.end()};",
        ])
    if observer == Observer.HELD_RITER_SPAN:
        return "\n".join([
            "    auto obs_rrange = db.riter_from({});",
            "    auto obs_rit = obs_rrange.begin();",
            "    REQUIRE_FALSE(obs_rit == std::default_sentinel);",
            "    const auto obs_rspan = (*obs_rit).value;",
            "    const bytecask::Bytes obs_rspan_at_acquire{",
            "        obs_rspan.begin(), obs_rspan.end()};",
        ])
    if observer == Observer.HELD_SNAPSHOT:
        return "\n".join([
            "    auto obs_snap = db.snapshot();",
            "    bytecask::Bytes obs_snap_value;",
            '    REQUIRE(obs_snap.get({}, to_bytes("k0"), obs_snap_value));',
            "    const bytecask::Bytes obs_snap_at_acquire = obs_snap_value;",
        ])
    if observer == Observer.SECOND_INSTANCE:
        return "\n".join([
            "    TempDir obs_td;",
            '    auto obs_db = bytecask::DB::open(obs_td.path / "other");',
            '    obs_db.put({.sync = false}, to_bytes("other"),',
            '               to_bytes("other_value"));',
            "    bytecask::Bytes obs_other;",
            '    REQUIRE(obs_db.get({}, to_bytes("other"), obs_other));',
            '    REQUIRE(to_string(obs_other) == "other_value");',
        ])
    raise AssertionError(f"unhandled observer: {observer}")


def gen_observer_verify(observer: Observer, moment: str) -> str:
    """C++ that re-checks the view. `moment` names the call site."""
    if observer == Observer.NONE:
        return ""
    lines = [f"    // observer: {observer.value} — {moment}", "    {"]
    if observer == Observer.HELD_VALUE:
        lines.append("      assert_view_stable(obs_value, obs_value_at_acquire);")
    elif observer == Observer.HELD_ITER_SPAN:
        lines.append("      assert_view_stable(obs_span, obs_span_at_acquire);")
    elif observer == Observer.HELD_RITER_SPAN:
        lines.append("      assert_view_stable(obs_rspan, obs_rspan_at_acquire);")
    elif observer == Observer.HELD_SNAPSHOT:
        lines += [
            "      bytecask::Bytes obs_snap_now;",
            '      REQUIRE(obs_snap.get({}, to_bytes("k0"), obs_snap_now));',
            "      CHECK(obs_snap_now == obs_snap_at_acquire);",
        ]
    elif observer == Observer.SECOND_INSTANCE:
        lines += [
            "      bytecask::Bytes obs_other_now;",
            '      REQUIRE(obs_db.get({}, to_bytes("other"), obs_other_now));',
            '      CHECK(to_string(obs_other_now) == "other_value");',
        ]
    else:
        raise AssertionError(f"unhandled observer: {observer}")
    lines.append("    }")
    return "\n".join(lines)


def gen_execute(
    plan: PlanShape, config: FaultConfig, delta: Delta
) -> str:
    """Generate C++ to execute the transition."""
    sync_val = "false" if config.use_sync_false else "true"
    solo_part = ", .solo = true" if plan.use_solo else ""

    if delta.threw:
        return (
            f"      REQUIRE_THROWS_AS(\n"
            f"          db.apply_batch({{.sync = {sync_val}{solo_part}}},\n"
            f"                            std::move(plan)),\n"
            f"          std::system_error);"
        )
    if plan.is_conflicting:
        return (
            f"      REQUIRE_FALSE(\n"
            f"          db.apply_batch({{.sync = {sync_val}{solo_part}}},\n"
            f"                            std::move(plan)));"
        )
    return (
        f"      REQUIRE(\n"
        f"          db.apply_batch({{.sync = {sync_val}{solo_part}}},\n"
        f"                            std::move(plan)));"
    )


def gen_delta_literal(delta: Delta) -> str:
    """Generate C++ ExpectedDelta aggregate initializer."""
    ka = "{" + ", ".join(f'"{k}"' for k in delta.keys_added) + "}"
    kr = "{" + ", ".join(f'"{k}"' for k in delta.keys_removed) + "}"
    if delta.expected_values:
        ev_pairs = ", ".join(
            f'{{"{k}", "{v}"}}' for k, v in delta.expected_values.items()
        )
        ev = "{" + ev_pairs + "}"
    else:
        ev = "{}"
    degraded = "true" if delta.degraded else "false"
    return (
        f"ExpectedDelta{{\n"
        f"        .keys_added = {ka},\n"
        f"        .keys_removed = {kr},\n"
        f"        .expected_values = {ev},\n"
        f"        .seq_advance = {delta.seq_advance},\n"
        f"        .degraded = {degraded},\n"
        f"    }}"
    )


def should_check_recovery(delta: Delta, failure: FailureClass) -> bool:
    """Recovery check: skipped for F/G — sync failed, page-cache bytes may
    survive to resume() and be committed then. assert_resumable covers this."""
    if failure in (FailureClass.F, FailureClass.G):
        return False
    return True  # resume() restores consistency for B1/B2/B3/C/H before db scope closes


def gen_test(
    state: StateShape,
    plan: PlanShape,
    failure: FailureClass,
    observer: Observer = Observer.NONE,
) -> str:
    """Generate one complete TEST_CASE."""
    labels = key_labels_for(plan)
    existing = existing_keys(state)
    delta = expected_delta(plan, failure, labels, existing)
    config = resolve_fault(state, plan, failure)
    name = f"prove__{state.label}__{plan.label}__{failure.value}"
    if observer != Observer.NONE:
        # Observer-free cells keep their existing names.
        name += f"__{observer.value}"

    parts: List[str] = []
    if state.io_backend != "pread":
        # WASM/Emscripten builds reject every non-pread back-end (see
        # DB::open); these buffered/mmap variants only make sense natively.
        parts.append("#ifndef __EMSCRIPTEN__")
    parts.append(f'TEST_CASE("{name}", "[prove]") {{')
    parts.append("  TempDir td;")
    parts.append('  auto dir = td.path / "db";')
    parts.append(f"  auto expected = {gen_delta_literal(delta)};")
    parts.append("  Baseline before;")
    parts.append("  {")

    # Setup
    parts.append(gen_setup(state))
    parts.append("")

    # Pre-baseline (snapshot + conflicting write before baseline capture)
    pre = gen_pre_baseline(plan)
    if pre:
        parts.append(pre)
        parts.append("")

    parts.append("    before = capture_baseline(db);")
    parts.append("")

    # Plan (after baseline capture)
    parts.append(gen_plan(plan, state, labels))
    parts.append("")

    # Observer acquires its view last, so nothing between it and the
    # transition can refresh what it holds.
    acquire = gen_observer_acquire(observer)
    if acquire:
        parts.append(acquire)
        parts.append("")

    # Fault injection + execute
    fi_code = gen_fault_injector(config)
    if fi_code:
        parts.append("    {")
        parts.append(fi_code)
        parts.append(gen_execute(plan, config, delta))
        parts.append("    }")
    else:
        parts.append("    {")
        parts.append(gen_execute(plan, config, delta))
        parts.append("    }")

    parts.append("")

    # In-process validation
    parts.append("    assert_delta(before, db, expected);")
    verify = gen_observer_verify(observer, "after the transition")
    if verify:
        parts.append(verify)
    if delta.degraded:
        parts.append("    assert_resumable(db);")
        # resume() truncates the active file while readers are not quiesced,
        # so the view is checked across it as well as across the failure.
        verify_after_resume = gen_observer_verify(observer, "after resume()")
        if verify_after_resume:
            parts.append(verify_after_resume)
    parts.append("  }")

    # Recovery
    if should_check_recovery(delta, failure):
        opts = _build_open_opts(state)
        if opts:
            parts.append(f"  assert_recoverable(dir, before, expected, {{{opts}}});")
        else:
            parts.append("  assert_recoverable(dir, before, expected);")
    else:
        if failure in (FailureClass.F, FailureClass.G):
            parts.append(
                "  // Recovery skipped: sync failed — page-cache bytes may survive"
            )
            parts.append(
                "  // to resume() and be committed then. assert_resumable covers"
            )
            parts.append(
                "  // the in-process recovery path."
            )
        else:
            parts.append(
                "  // Recovery skipped: degraded state with unpersisted transition."
            )

    parts.append("}")
    if state.io_backend != "pread":
        parts.append("#endif  // __EMSCRIPTEN__")
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# File generation
# ---------------------------------------------------------------------------

FILE_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// AUTO-GENERATED by tests/proof/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for apply_batch. Each test exercises one
// (StateShape, PlanShape, FailureClass) combination from the scenario
// matrix, validates the transition delta against the reference model,
// and verifies recovery where applicable.

#include <system_error>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::assert_delta;
using bytecask::testing::assert_recoverable;
using bytecask::testing::assert_resumable;
using bytecask::testing::assert_view_stable;
using bytecask::testing::Baseline;
using bytecask::testing::capture_baseline;
using bytecask::testing::ExpectedDelta;
using bytecask::testing::to_bytes;
using bytecask::testing::to_string;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_test_{}_{}",
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
    """Generate the complete .cpp file."""
    tests: List[str] = []
    for state, plan, failure, observer in generate_matrix():
        tests.append(gen_test(state, plan, failure, observer))
    return FILE_HEADER + "\n\n".join(tests) + "\n"


def main() -> None:
    output_dir = Path(__file__).resolve().parent / "generated"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / "prove_apply_batch.cpp"
    content = generate_file()
    output_path.write_text(content)
    count = sum(1 for _ in generate_matrix())
    print(f"Generated {count} tests → {output_path}")


if __name__ == "__main__":
    main()
