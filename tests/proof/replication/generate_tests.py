#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_replication.cpp from the replication scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/replication/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

_project_root = Path(__file__).resolve().parent.parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.replication.expected_delta import (
    IngestDelta,
    ManifestDelta,
    ingest_expected,
    manifest_expected,
)
from tests.proof.replication.fault_point_resolver import (
    FaultConfig,
    resolve_ingest_fault,
    resolve_manifest_fault,
)
from tests.proof.replication.scenario_matrix import (
    IngestFailureClass,
    ManifestFailureClass,
    OpsShape,
    StateShape,
    generate_ingest_matrix,
    generate_manifest_matrix,
)

# ---------------------------------------------------------------------------
# Leader workload generation
# ---------------------------------------------------------------------------


def gen_leader_workload(state: StateShape) -> str:
    """Generate C++ to create leader DB and apply the workload."""
    opts = ""
    if state.max_file_bytes is not None:
        opts = f", {{.max_file_bytes = {state.max_file_bytes}}}"

    lines: List[str] = []
    lines.append(f"    auto leader = bytecask::DB::open(leader_dir{opts});")

    if state.label == "single_key":
        lines.append('    leader.put({}, to_bytes("k1"), to_bytes("v1"));')

    elif state.label == "multi_key":
        for i in range(1, 6):
            lines.append(
                f'    leader.put({{}}, to_bytes("k{i}"), to_bytes("v{i}"));'
            )

    elif state.label == "overwrites":
        lines.append('    leader.put({}, to_bytes("k1"), to_bytes("old"));')
        lines.append('    leader.put({}, to_bytes("k1"), to_bytes("new"));')

    elif state.label == "deletes":
        lines.append('    leader.put({}, to_bytes("k1"), to_bytes("v1"));')
        lines.append('    (void)leader.del({}, to_bytes("k1"));')

    elif state.label == "range_deletes":
        for i in range(1, 6):
            lines.append(
                f'    leader.put({{}}, to_bytes("k{i}"), to_bytes("v{i}"));'
            )
        lines.append(
            '    leader.del_range({}, to_bytes("k2"), to_bytes("k5"));'
        )

    elif state.label == "batches":
        lines.append("    {")
        lines.append("      bytecask::WritePlan plan;")
        lines.append('      plan.put(to_bytes("b1"), to_bytes("v1"));')
        lines.append('      plan.put(to_bytes("b2"), to_bytes("v2"));')
        lines.append('      plan.put(to_bytes("b3"), to_bytes("v3"));')
        lines.append(
            "      (void)leader.apply_batch({}, std::move(plan));"
        )
        lines.append("    }")

    elif state.label == "multi_file":
        lines.append("    for (int i = 0; i < 50; ++i) {")
        lines.append('      auto key = std::format("k{:04d}", i);')
        lines.append('      auto val = std::format("v{:04d}", i);')
        lines.append("      leader.put({}, to_bytes(key), to_bytes(val));")
        lines.append("    }")

    elif state.label == "mixed_sync_nosync":
        lines.append(
            '    leader.put({.sync = false}, to_bytes("ns1"), to_bytes("v1"));'
        )
        lines.append(
            '    leader.put({.sync = false}, to_bytes("ns2"), to_bytes("v2"));'
        )
        lines.append(
            '    leader.put({.sync = false}, to_bytes("ns3"), to_bytes("v3"));'
        )
        lines.append(
            '    leader.put({.sync = true}, to_bytes("s1"), to_bytes("v4"));'
        )

    elif state.label == "nosync_only":
        lines.append("    for (int i = 0; i < 50; ++i) {")
        lines.append('      auto key = std::format("k{:04d}", i);')
        lines.append('      auto val = std::format("v{:04d}", i);')
        lines.append(
            "      leader.put({.sync = false}, to_bytes(key), to_bytes(val));"
        )
        lines.append("    }")

    elif state.label == "nosync_then_sync":
        lines.append(
            '    leader.put({.sync = false}, to_bytes("ns1"), to_bytes("v1"));'
        )
        lines.append(
            '    leader.put({.sync = false}, to_bytes("ns2"), to_bytes("v2"));'
        )
        lines.append(
            '    leader.put({.sync = false}, to_bytes("ns3"), to_bytes("v3"));'
        )
        lines.append(
            '    leader.put({.sync = true}, to_bytes("s1"), to_bytes("v4"));'
        )

    elif state.label == "vacuumed_batches":
        # Write individual puts to fill a file, then a batch in a new file,
        # then vacuum the batch file.
        lines.append("    for (int i = 0; i < 10; ++i) {")
        lines.append('      auto key = std::format("pre{:02d}", i);')
        lines.append("      leader.put({}, to_bytes(key), to_bytes(key));")
        lines.append("    }")
        lines.append("    {")
        lines.append("      bytecask::WritePlan plan;")
        lines.append('      plan.put(to_bytes("b1"), to_bytes("v1"));')
        lines.append('      plan.put(to_bytes("b2"), to_bytes("v2"));')
        lines.append('      plan.put(to_bytes("b3"), to_bytes("v3"));')
        lines.append(
            "      (void)leader.apply_batch({}, std::move(plan));"
        )
        lines.append("    }")
        # Overwrite pre-keys to create fragmentation, then vacuum.
        lines.append("    for (int i = 0; i < 10; ++i) {")
        lines.append('      auto key = std::format("pre{:02d}", i);')
        lines.append(
            '      leader.put({}, to_bytes(key), to_bytes("updated"));'
        )
        lines.append("    }")
        lines.append(
            "    (void)leader.vacuum({.fragmentation_threshold = 0.0});"
        )

    else:
        raise ValueError(f"Unknown state label: {state.label}")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Fault injector generation
# ---------------------------------------------------------------------------


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
        lines = ["        using PW = bytecask::testing::PostWriteMode;"]
        if config.post_write_mode == "short_write":
            lines.append(
                f"        bytecask::testing::ScopedFaultInjector fi{{"
                f'"{config.name}", {mode}, {config.short_write_bytes}}};'
            )
        else:
            lines.append(
                f"        bytecask::testing::ScopedFaultInjector fi{{"
                f'"{config.name}", {mode}}};'
            )
        return "\n".join(lines)

    if config.name:
        return (
            f"        bytecask::testing::ScopedFaultInjector "
            f'fi{{"{config.name}"}};'
        )

    if config.fail_at is not None:
        return (
            f"        bytecask::testing::ScopedFaultInjector "
            f"fi{{{config.fail_at}}};"
        )

    return ""


# ---------------------------------------------------------------------------
# Ingest test generation
# ---------------------------------------------------------------------------


def gen_ingest_call(throwing: bool, var: str = "views") -> str:
    """Generate the ingest call with or without REQUIRE_THROWS."""
    if throwing:
        return (
            "        REQUIRE_THROWS_AS(\n"
            f"            follower.ingest({var}),\n"
            "            std::system_error);"
        )
    return f"        follower.ingest({var});"


def gen_assertions_success(state: StateShape) -> str:
    """Generate assertions for the SUCCESS case."""
    lines: List[str] = []
    lines.append("      assert_replication_match(leader_bl, follower);")
    lines.append("      CHECK_FALSE(follower.is_degraded());")
    return "\n".join(lines)


def gen_assertions_failure(delta: IngestDelta) -> str:
    """Generate assertions for a failure case."""
    lines: List[str] = []
    if delta.partial_committed:
        # I_H: some entries committed. We can't match leader exactly
        # but we can check degraded + structural consistency.
        lines.append("      CHECK(follower.is_degraded());")
        lines.append("      assert_consistent(follower);")
    else:
        lines.append("      assert_replication_no_change(follower_bl, follower);")
        lines.append("      CHECK(follower.is_degraded());")
    lines.append("      assert_resumable(follower);")
    return "\n".join(lines)


def gen_recovery_check(delta: IngestDelta, failure: IngestFailureClass) -> str:
    """Generate recovery check after db scope closes."""
    # For I_F/I_G: sync failed — page-cache bytes may or may not survive.
    # Recovery outcome is non-deterministic; skip recovery check.
    if failure in (IngestFailureClass.I_F, IngestFailureClass.I_G):
        return (
            "    // Recovery skipped: sync failed — page-cache bytes may survive\n"
            "    // to resume(). assert_resumable covers in-process recovery."
        )
    if delta.keys_match:
        return "    assert_replication_recovery(follower_dir, leader_bl);"
    # For failures that are resumable, after resume the follower is consistent
    # but may not match leader. Just check structural consistency.
    return (
        "    {\n"
        "      auto recovered = bytecask::DB::open(follower_dir);\n"
        "      assert_consistent(recovered);\n"
        "    }"
    )


def gen_full_stream_test(
    state: StateShape,
    failure: IngestFailureClass,
    delta: IngestDelta,
    config: FaultConfig,
) -> str:
    """Generate a full_stream ingest test."""
    parts: List[str] = []

    # Leader setup.
    parts.append(gen_leader_workload(state))
    parts.append("    leader_bl = capture_replication_baseline(leader);")
    parts.append("    auto snap = leader.snapshot();")
    parts.append("    auto owned = collect_changes(leader.changes_since(snap, 0));")
    parts.append("    auto views = owned.views();")
    parts.append("")

    # Follower setup.
    follower_opts = ""
    if state.max_file_bytes is not None:
        follower_opts = f".max_file_bytes = {state.max_file_bytes}, "
    parts.append(
        f"    auto follower = bytecask::DB::open(follower_dir,"
        f"\n        {{{follower_opts}.initial_mode = bytecask::Mode::Follower}});"
    )
    if not delta.keys_match:
        parts.append("    auto follower_bl = capture_baseline(follower);")
    parts.append("")

    # Ingest with optional fault injection.
    fi_code = gen_fault_injector(config)
    if fi_code:
        parts.append("      {")
        parts.append(fi_code)
        parts.append(gen_ingest_call(delta.threw))
        parts.append("      }")
    else:
        parts.append("      {")
        parts.append(gen_ingest_call(delta.threw))
        parts.append("      }")

    parts.append("")

    # Assertions.
    if delta.keys_match:
        parts.append(gen_assertions_success(state))
    else:
        parts.append(gen_assertions_failure(delta))

    return "\n".join(parts)


def gen_incremental_test(
    state: StateShape,
    failure: IngestFailureClass,
    delta: IngestDelta,
    config: FaultConfig,
) -> str:
    """Generate an incremental ingest test (split into chunks)."""
    parts: List[str] = []

    # Leader setup.
    parts.append(gen_leader_workload(state))
    parts.append("    leader_bl = capture_replication_baseline(leader);")
    parts.append("    auto snap = leader.snapshot();")
    parts.append("    auto owned = collect_changes(leader.changes_since(snap, 0));")
    parts.append("    auto views = owned.views();")
    parts.append("")

    # Follower setup.
    follower_opts = ""
    if state.max_file_bytes is not None:
        follower_opts = f".max_file_bytes = {state.max_file_bytes}, "
    parts.append(
        f"    auto follower = bytecask::DB::open(follower_dir,"
        f"\n        {{{follower_opts}.initial_mode = bytecask::Mode::Follower}});"
    )
    parts.append("")

    # Split into two chunks. Find a safe split point (not inside a batch).
    parts.append("    // Split views into two chunks at a safe boundary.")
    parts.append("    std::size_t split = views.size() / 2;")
    parts.append("    // Walk forward to find a non-batch boundary.")
    parts.append("    {")
    parts.append("      bool in_batch = false;")
    parts.append("      for (std::size_t i = 0; i < split; ++i) {")
    parts.append(
        "        if (views[i].entry_type == bytecask::EntryType::BulkBegin) in_batch = true;"
    )
    parts.append(
        "        else if (views[i].entry_type == bytecask::EntryType::BulkEnd) in_batch = false;"
    )
    parts.append("      }")
    parts.append("      if (in_batch) {")
    parts.append("        // Move split past BulkEnd.")
    parts.append("        while (split < views.size() &&")
    parts.append(
        "               views[split].entry_type != bytecask::EntryType::BulkEnd) ++split;"
    )
    parts.append("        if (split < views.size()) ++split; // past BulkEnd")
    parts.append("      }")
    parts.append("    }")
    parts.append(
        "    auto chunk1 = std::span<const bytecask::DataEntryView>{views.data(), split};"
    )
    parts.append(
        "    auto chunk2 = std::span<const bytecask::DataEntryView>{views.data() + split, views.size() - split};"
    )
    parts.append("")

    # First chunk always succeeds.
    parts.append("    // First chunk: always succeeds.")
    parts.append("    if (!chunk1.empty()) follower.ingest(chunk1);")
    parts.append("")

    # Capture follower baseline AFTER chunk 1 so that failure assertions
    # compare against the state that includes chunk 1's committed entries.
    if not delta.keys_match:
        parts.append("    auto follower_bl = capture_baseline(follower);")
        parts.append("")

    # Second chunk: fault injection if non-SUCCESS.
    fi_code = gen_fault_injector(config)
    if fi_code:
        parts.append("    // Second chunk: inject fault.")
        parts.append("    if (!chunk2.empty()) {")
        parts.append(fi_code)
        parts.append(gen_ingest_call(delta.threw))
        parts.append("    }")
    else:
        parts.append("    // Second chunk.")
        parts.append("    if (!chunk2.empty()) follower.ingest(chunk2);")

    parts.append("")

    # Assertions.
    if delta.keys_match:
        parts.append(gen_assertions_success(state))
    else:
        # When batch boundary adjustment pushes all entries into chunk1,
        # chunk2 is empty and the fault never fires. Handle both cases.
        parts.append("    if (chunk2.empty()) {")
        parts.append("      // All entries landed in chunk1 (batch boundary adjustment).")
        parts.append("      // No fault fired — verify success behavior.")
        parts.append("      assert_replication_match(leader_bl, follower);")
        parts.append("      CHECK_FALSE(follower.is_degraded());")
        parts.append("    } else {")
        parts.append(gen_assertions_failure(delta))
        parts.append("    }")

    return "\n".join(parts)


def gen_restart_midstream_test(
    state: StateShape,
    failure: IngestFailureClass,
    delta: IngestDelta,
    config: FaultConfig,
) -> str:
    """Generate a restart_midstream ingest test."""
    parts: List[str] = []

    # Leader setup.
    parts.append(gen_leader_workload(state))
    parts.append("    leader_bl = capture_replication_baseline(leader);")
    parts.append("    auto snap = leader.snapshot();")
    parts.append("    auto owned = collect_changes(leader.changes_since(snap, 0));")
    parts.append("    auto views = owned.views();")
    parts.append("")

    # Split at safe boundary (same logic as incremental).
    parts.append("    std::size_t split = views.size() / 2;")
    parts.append("    {")
    parts.append("      bool in_batch = false;")
    parts.append("      for (std::size_t i = 0; i < split; ++i) {")
    parts.append(
        "        if (views[i].entry_type == bytecask::EntryType::BulkBegin) in_batch = true;"
    )
    parts.append(
        "        else if (views[i].entry_type == bytecask::EntryType::BulkEnd) in_batch = false;"
    )
    parts.append("      }")
    parts.append("      if (in_batch) {")
    parts.append("        while (split < views.size() &&")
    parts.append(
        "               views[split].entry_type != bytecask::EntryType::BulkEnd) ++split;"
    )
    parts.append("        if (split < views.size()) ++split;")
    parts.append("      }")
    parts.append("    }")
    parts.append(
        "    auto chunk1 = std::span<const bytecask::DataEntryView>{views.data(), split};"
    )
    parts.append("")

    follower_opts = ""
    if state.max_file_bytes is not None:
        follower_opts = f".max_file_bytes = {state.max_file_bytes}, "

    # First pass: ingest first half, close follower.
    parts.append("    // First pass: ingest first half, close follower.")
    parts.append("    {")
    parts.append(
        f"      auto follower = bytecask::DB::open(follower_dir,"
        f"\n          {{{follower_opts}.initial_mode = bytecask::Mode::Follower}});"
    )
    parts.append("      if (!chunk1.empty()) follower.ingest(chunk1);")
    parts.append("    }")
    parts.append("")

    # Second pass: reopen, get remainder via changes_since from follower's seq.
    parts.append("    // Second pass: reopen, ingest remainder.")
    parts.append("    {")
    parts.append(
        f"      auto follower = bytecask::DB::open(follower_dir,"
        f"\n          {{{follower_opts}.initial_mode = bytecask::Mode::Follower}});"
    )
    parts.append("      auto from_seq = follower.current_sequence();")
    parts.append("      auto snap2 = leader.snapshot();")
    parts.append(
        "      auto owned2 = collect_changes(leader.changes_since(snap2, from_seq));"
    )
    parts.append("      auto views2 = owned2.views();")
    if not delta.keys_match:
        parts.append("      auto follower_bl = capture_baseline(follower);")
    parts.append("")

    fi_code = gen_fault_injector(config)
    if fi_code:
        # The second pass receives only the remainder after the first pass.
        # For rotation faults (I_H, I_G), the remainder may be too small to
        # trigger rotation — the fault never fires. Use try-catch to handle
        # both outcomes.
        parts.append("      bool threw = false;")
        parts.append("      if (!views2.empty()) {")
        parts.append(fi_code)
        parts.append("        try {")
        parts.append(f"          follower.ingest(views2);")
        parts.append("        } catch (const std::system_error&) {")
        parts.append("          threw = true;")
        parts.append("        }")
        parts.append("      }")
    else:
        parts.append("      if (!views2.empty()) follower.ingest(views2);")

    parts.append("")

    if delta.keys_match:
        parts.append(gen_assertions_success(state))
    else:
        # If the fault didn't fire (views2 empty or too small to trigger
        # rotation), verify success behavior instead.
        parts.append("      if (!threw) {")
        parts.append("        assert_replication_match(leader_bl, follower);")
        parts.append("        CHECK_FALSE(follower.is_degraded());")
        parts.append("      } else {")
        parts.append(gen_assertions_failure(delta))
        parts.append("      }")

    parts.append("    }")

    return "\n".join(parts)


def gen_duplicate_delivery_test(state: StateShape) -> str:
    """Generate a duplicate_delivery test (always SUCCESS)."""
    parts: List[str] = []

    # Leader setup.
    parts.append(gen_leader_workload(state))
    parts.append("    leader_bl = capture_replication_baseline(leader);")
    parts.append("    auto snap = leader.snapshot();")
    parts.append("    auto owned = collect_changes(leader.changes_since(snap, 0));")
    parts.append("    auto views = owned.views();")
    parts.append("")

    follower_opts = ""
    if state.max_file_bytes is not None:
        follower_opts = f".max_file_bytes = {state.max_file_bytes}, "

    # Follower: ingest, then re-ingest.
    parts.append(
        f"    auto follower = bytecask::DB::open(follower_dir,"
        f"\n        {{{follower_opts}.initial_mode = bytecask::Mode::Follower}});"
    )
    parts.append("    follower.ingest(views);")
    parts.append("    auto seq_after = follower.current_sequence();")
    parts.append("")
    parts.append("    // Re-deliver same entries — must be a no-op.")
    parts.append("    follower.ingest(views);")
    parts.append("    CHECK(follower.current_sequence() == seq_after);")
    parts.append("    assert_replication_match(leader_bl, follower);")

    return "\n".join(parts)


def gen_planned_promotion_test(state: StateShape) -> str:
    """Generate a planned_promotion test (always SUCCESS)."""
    parts: List[str] = []

    opts = ""
    if state.max_file_bytes is not None:
        opts = f", {{.max_file_bytes = {state.max_file_bytes}}}"
    follower_opts = ""
    if state.max_file_bytes is not None:
        follower_opts = f".max_file_bytes = {state.max_file_bytes}, "

    # NodeA (leader) applies workload.
    parts.append(gen_leader_workload(state))
    parts.append(
        "    auto init_leader_bl = capture_replication_baseline(leader);"
    )
    parts.append("    auto snap = leader.snapshot();")
    parts.append("    auto owned = collect_changes(leader.changes_since(snap, 0));")
    parts.append("    auto views = owned.views();")
    parts.append("")

    # NodeB (follower) ingests.
    parts.append(
        f"    auto follower = bytecask::DB::open(follower_dir,"
        f"\n        {{{follower_opts}.initial_mode = bytecask::Mode::Follower}});"
    )
    parts.append("    follower.ingest(views);")
    parts.append("    assert_replication_match(init_leader_bl, follower);")
    parts.append("")

    # Leadership transfer.
    # For nosync workloads, flush unsync'd entries before mode switch.
    # In a real deployment the operator would ensure all writes are durable
    # before transferring leadership.
    if state.has_nosync:
        parts.append("    // Flush nosync entries to durable state before leadership transfer.")
        parts.append('    leader.put({.sync = true}, to_bytes("__flush__"), to_bytes(""));')
        parts.append('    (void)leader.del({.sync = true}, to_bytes("__flush__"));')
        parts.append("    // Re-capture baseline after flush — all entries now durable.")
        parts.append("    init_leader_bl = capture_replication_baseline(leader);")
        parts.append("    snap = leader.snapshot();")
        parts.append("    owned = collect_changes(leader.changes_since(snap, 0));")
        parts.append("    views = owned.views();")
        parts.append("    follower.ingest(views);")
        parts.append("")
    parts.append("    // Leadership transfer: leader → follower, follower → leader.")
    parts.append("    leader.set_mode(bytecask::Mode::Follower);")
    parts.append("    CHECK_THROWS_AS(")
    parts.append('        leader.put({}, to_bytes("reject"), to_bytes("x")),')
    parts.append("        bytecask::DbFollowerMode);")
    parts.append("")
    parts.append("    follower.set_mode(bytecask::Mode::Leader);")
    parts.append("    auto seq_before = follower.current_sequence();")
    parts.append('    follower.put({}, to_bytes("promoted_key"), to_bytes("promoted_val"));')
    parts.append("    CHECK(follower.current_sequence() == seq_before + 1);")
    parts.append("")

    # Backward sync: follower → leader.
    parts.append("    // Backward sync: follower → leader.")
    parts.append("    auto snap2 = follower.snapshot();")
    parts.append(
        "    auto owned2 = collect_changes(follower.changes_since(snap2,"
        " leader.current_sequence()));"
    )
    parts.append("    auto views2 = owned2.views();")
    parts.append("    leader.ingest(views2);")
    parts.append("")

    # Verify convergence — set outer leader_bl to follower's final state
    # so the recovery check can verify the follower dir.
    parts.append("    auto follower_final = capture_replication_baseline(follower);")
    parts.append("    assert_replication_match(follower_final, leader);")
    parts.append("    leader_bl = follower_final;  // for recovery check")

    return "\n".join(parts)


# ---------------------------------------------------------------------------
# Top-level test case generation
# ---------------------------------------------------------------------------


def gen_ingest_test(
    state: StateShape, ops: OpsShape, failure: IngestFailureClass
) -> str:
    """Generate one complete ingest TEST_CASE."""
    delta = ingest_expected(state, ops, failure)
    config = resolve_ingest_fault(failure, ops)
    name = f"prove_repl__{state.label}__{ops.value}__{failure.value}"

    parts: List[str] = []
    parts.append(f'TEST_CASE("{name}", "[prove_repl]") {{')
    parts.append("  TempDir td;")
    parts.append('  auto leader_dir = td.path / "leader";')
    parts.append('  auto follower_dir = td.path / "follower";')
    # leader_bl declared outside scope so recovery check can use it.
    parts.append("  bytecask::testing::ReplicationBaseline leader_bl;")
    parts.append("  {")

    if ops == OpsShape.FULL_STREAM:
        parts.append(gen_full_stream_test(state, failure, delta, config))
    elif ops == OpsShape.INCREMENTAL:
        parts.append(gen_incremental_test(state, failure, delta, config))
    elif ops == OpsShape.RESTART_MIDSTREAM:
        parts.append(gen_restart_midstream_test(state, failure, delta, config))
    elif ops == OpsShape.DUPLICATE_DELIVERY:
        parts.append(gen_duplicate_delivery_test(state))
    elif ops == OpsShape.PLANNED_PROMOTION:
        parts.append(gen_planned_promotion_test(state))
    else:
        raise ValueError(f"Unknown ops shape: {ops}")

    parts.append("  }")

    # Recovery check (outside inner scope so DBs are closed).
    parts.append(gen_recovery_check(delta, failure))

    parts.append("}")
    return "\n".join(parts)


def gen_manifest_test(state: StateShape, failure: ManifestFailureClass) -> str:
    """Generate one complete manifest TEST_CASE."""
    delta = manifest_expected(failure)
    config = resolve_manifest_fault(failure)
    name = f"prove_manifest__{state.label}__{failure.value}"

    parts: List[str] = []
    parts.append(f'TEST_CASE("{name}", "[prove_manifest]") {{')
    parts.append("  TempDir td;")
    parts.append('  auto leader_dir = td.path / "leader";')
    parts.append("  {")

    # Leader workload.
    parts.append(gen_leader_workload(state))
    parts.append("")

    if delta.manifest_produced:
        parts.append("    auto manifest = leader.create_manifest();")
        parts.append("    CHECK(manifest.through_sequence == leader.current_sequence());")
        parts.append("    CHECK_FALSE(manifest.files.empty());")
        parts.append("")
        parts.append("    // Verify all files exist on disk.")
        parts.append("    for (const auto& f : manifest.files) {")
        parts.append("      CHECK(std::filesystem::exists(f.data_path));")
        parts.append("      CHECK(std::filesystem::exists(f.hint_path));")
        parts.append("    }")
    else:
        fi_code = gen_fault_injector(config)
        if fi_code:
            parts.append("    {")
            parts.append(fi_code)
            parts.append(
                "        REQUIRE_THROWS_AS(leader.create_manifest(), std::system_error);"
            )
            parts.append("    }")
        else:
            parts.append(
                "    REQUIRE_THROWS_AS(leader.create_manifest(), std::system_error);"
            )

        # Post-failure assertions.
        if delta.degraded:
            parts.append("")
            parts.append("    // Engine must be degraded — sealed active file is unusable.")
            parts.append("    CHECK(leader.is_degraded());")
            parts.append("    assert_consistent(leader);")
            parts.append("")
            parts.append("    // resume() must recover.")
            parts.append("    assert_resumable(leader);")
            parts.append("")
            parts.append("    // After resume, writes must succeed.")
            parts.append('    REQUIRE_NOTHROW(leader.put({}, to_bytes("post_resume"), to_bytes("ok")));')
        else:
            parts.append("")
            parts.append("    // Engine must NOT be degraded — failure was before seal.")
            parts.append("    CHECK_FALSE(leader.is_degraded());")
            parts.append("")
            parts.append("    // Writes must still succeed.")
            parts.append('    REQUIRE_NOTHROW(leader.put({}, to_bytes("post_fail"), to_bytes("ok")));')

    parts.append("  }")
    parts.append("}")
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# File generation
# ---------------------------------------------------------------------------

FILE_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// AUTO-GENERATED by tests/proof/replication/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for replication primitives. Each test exercises
// one (StateShape, OpsShape, IngestFailureClass) or
// (StateShape, ManifestFailureClass) combination from the scenario matrix,
// validates invariants, and verifies recovery where applicable.

#include <system_error>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::assert_replication_match;
using bytecask::testing::assert_replication_no_change;
using bytecask::testing::assert_replication_recovery;
using bytecask::testing::assert_resumable;
using bytecask::testing::Baseline;
using bytecask::testing::capture_baseline;
using bytecask::testing::capture_replication_baseline;
using bytecask::testing::collect_changes;
using bytecask::testing::to_bytes;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_repl_{}",
                 std::chrono::system_clock::now().time_since_epoch().count())} {
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

"""


def generate_file() -> str:
    """Generate the complete .cpp file."""
    tests: List[str] = []
    for state, ops, failure in generate_ingest_matrix():
        tests.append(gen_ingest_test(state, ops, failure))
    for state, failure in generate_manifest_matrix():
        tests.append(gen_manifest_test(state, failure))
    return FILE_HEADER + "\n\n".join(tests) + "\n"


def main() -> None:
    output_dir = Path(__file__).resolve().parent.parent / "generated"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / "prove_replication.cpp"
    content = generate_file()
    output_path.write_text(content)
    ingest_count = sum(1 for _ in generate_ingest_matrix())
    manifest_count = sum(1 for _ in generate_manifest_matrix())
    print(f"Generated {ingest_count} ingest + {manifest_count} manifest"
          f" = {ingest_count + manifest_count} tests → {output_path}")


if __name__ == "__main__":
    main()
