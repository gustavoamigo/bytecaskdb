#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_vacuum_compact.cpp from the vacuum_compact scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/vacuum_compact/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

_project_root = Path(__file__).resolve().parent.parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.vacuum_compact.expected_delta import (
    VacuumCompactDelta,
    vacuum_compact_delta,
)
from tests.proof.vacuum_compact.fault_point_resolver import (
    CompactFault,
    resolve_compact_fault,
)
from tests.proof.vacuum_compact.scenario_matrix import (
    CompactStateShape,
    VacuumCompactFailureClass,
    generate_matrix,
)

# ---------------------------------------------------------------------------
# C++ code generation
# ---------------------------------------------------------------------------


def _build_open_opts(state: CompactStateShape) -> str:
    """Build the C++ designated-initializer list for Options from state fields."""
    parts = [f".max_file_bytes = {state.max_file_bytes}"]
    if state.io_backend == "mmap":
        parts.append(".io_backend = bytecask::IoBackend::Mmap")
    elif state.io_backend == "buffer_pool":
        parts.append(".io_backend = bytecask::IoBackend::BufferPool")
        parts.append(".buffer_pool = {.capacity_bytes = 1048576}")
    return ", ".join(parts)


def gen_setup(state: CompactStateShape) -> str:
    """Generate C++ to create the initial DB state with a sealed file."""
    lines: List[str] = []
    lines.append(
        f"    // Setup: write {state.sealed_keys} to file_0, trigger rotation to seal it."
    )
    opts = _build_open_opts(state)
    lines.append(
        f"    auto db = bytecask::DB::open(dir, {{{opts}}});"
    )
    if state.batched:
        lines.append("    {")
        lines.append("      bytecask::WritePlan plan;")
        for key in state.sealed_keys:
            lines.append(
                f'      plan.put(to_bytes("{key}"), to_bytes("v_{key}"));'
            )
        lines.append("      (void)db.apply_batch({.sync = false}, std::move(plan));")
        lines.append("    }")
    else:
        for key in state.sealed_keys:
            lines.append(
                f'    db.put({{.sync = false}}, to_bytes("{key}"), to_bytes("v_{key}"));'
            )
    if state.range_del:
        lo, hi = state.range_del
        lines.append(
            f"    // Range tombstone inside the file being compacted: it deletes"
            f' {state.deleted_keys},'
        )
        lines.append(
            "    // and compaction has to carry it across even though it is not key data."
        )
        lines.append(
            f'    db.del_range({{.sync = false}}, to_bytes("{lo}"), to_bytes("{hi}"));'
        )
    elif state.deleted_keys:
        lines.append(
            f"    // Delete {state.deleted_keys} to create dead entries in file_0."
        )
        for key in state.deleted_keys:
            lines.append(
                f'    (void)db.del({{.sync = false}}, to_bytes("{key}"));'
            )
    lines.append("    // The baseline a power cut cannot take: everything so far is")
    lines.append("    // durable before the state vacuum will judge the file by is made.")
    lines.append("    make_durable(db);")
    lines.append("    durable_before = key_values(db);")
    if state.overwritten_keys:
        lines.append(
            f"    // Overwrite {list(state.overwritten_keys)} without a sync (#245):"
        )
        lines.append(
            "    // the sealed file's entries are dead only by writes a power cut"
        )
        lines.append("    // can lose.")
        # 2-byte values: a 23-byte entry, so the overwrites stay under the
        # 50-byte limit and no rotation syncs them (a rotation would).
        for key in state.overwritten_keys:
            lines.append(
                f'    db.put({{.sync = false}}, to_bytes("{key}"), to_bytes("n{key[1:]}"));'
            )
    return "\n".join(lines)


def gen_vacuum_call(
    fault: CompactFault | None, failure: VacuumCompactFailureClass
) -> str:
    """Generate the vacuum() call, wrapped in fault injector if needed."""
    # fragmentation_threshold=0.0 ensures any sealed file qualifies for vacuum.
    opts = "{.fragmentation_threshold = 0.0}"

    vc4_comment = (
        "\n    // VC4: fails just before the rename — the copy is synced and\n"
        "    // shrunk under .data.tmp and vacuum_commit never ran. Old file\n"
        "    // remains in state, and vacuum removes the staging copy (#235)."
        if failure == VacuumCompactFailureClass.VC4
        else "\n    // VC5: committed, source not unlinked — the kill inside vacuum's\n"
        "    // publish window. In-memory outcome is success; on disk the source\n"
        "    // and its compacted copy both exist with the same entries under\n"
        "    // the same sequences. Recovery deletes the copy, undoing the\n"
        "    // vacuum; assert_vacuum_recoverable proves the directory opens."
        if failure == VacuumCompactFailureClass.VC5
        else "\n    // VC6: renamed, not committed (#104 M3) — the compacted copy is on\n"
        "    // disk under its final name and the published state does not\n"
        "    // reference it. Vacuum removes it on the way out (#304)."
        if failure == VacuumCompactFailureClass.VC6
        else "\n    // VC7: the fdatasync that makes the superseding sync = false writes\n"
        "    // durable fails (#261). The engine degrades and vacuum throws before\n"
        "    // committing; the old file stays."
        if failure == VacuumCompactFailureClass.VC7
        else ""
    )

    if fault is None:
        return f"    REQUIRE(db.vacuum({opts}));"

    nth = (
        f"\n      fi.inj.fail_on_nth_match = {fault.nth};" if fault.nth else ""
    )
    return (
        f"    {{{vc4_comment}\n"
        f'      bytecask::testing::ScopedFaultInjector fi{{"{fault.name}"}};{nth}\n'
        f"      REQUIRE_THROWS_AS(db.vacuum({opts}), std::system_error);\n"
        f"    }}"
    )


def gen_assertions(delta: VacuumCompactDelta) -> str:
    degraded = "true" if delta.degraded else "false"
    if delta.file_removed:
        return (
            "    assert_vacuum_success(db, before, vacuumed_file_id);\n"
            f"    CHECK(db.is_degraded() == {degraded});"
        )
    return (
        "    assert_vacuum_no_change(db, before, vacuumed_file_id);\n"
        f"    CHECK(db.is_degraded() == {degraded});"
    )


def gen_test(
    state: CompactStateShape, failure: VacuumCompactFailureClass
) -> str:
    """Generate one complete TEST_CASE."""
    delta = vacuum_compact_delta(state, failure)
    fault = resolve_compact_fault(state, failure)
    name = f"prove_vacuum_compact__{state.label}__{failure.value}"

    parts: List[str] = []
    if state.io_backend != "pread":
        # WASM/Emscripten builds reject every non-pread back-end (see
        # DB::open); these buffered/mmap variants only make sense natively.
        parts.append("#ifndef __EMSCRIPTEN__")
    parts.append(f'TEST_CASE("{name}", "[prove_vacuum_compact]") {{')
    parts.append("  TempDir td;")
    parts.append('  auto dir = td.path / "db";')
    # Power loss (#265): the cell runs under the page cache model, and the
    # directory is copied as the device holds it before ~DB syncs it.
    parts.append("  bytecask::testing::ScopedPageCacheModel cache;")
    parts.append('  auto cut = td.path / "cut";')
    parts.append("  std::map<std::string, bytecask::Bytes> durable_before;")
    parts.append("  std::uint64_t watermark = 0;")
    parts.append("  bytecask::testing::VacuumBaseline before;")
    parts.append("  {")
    parts.append(gen_setup(state))
    parts.append("")
    parts.append("    before = capture_vacuum_baseline(db);")
    parts.append("    auto vacuumed_file_id = find_vacuum_target(db);")
    parts.append("")
    parts.append(gen_vacuum_call(fault, failure))
    parts.append("")
    parts.append(gen_assertions(delta))
    # Every cell, thrown or not: no staging copy outlives the vacuum call.
    parts.append("    CHECK(staging_data_files(dir).empty());")
    # Every cell but VC5, whose source stays on disk after the commit: no
    # copy outlives an uncommitted vacuum either (#304).
    if failure != VacuumCompactFailureClass.VC5:
        parts.append("    CHECK(unreferenced_files(db, dir).empty());")
    parts.append("    watermark = durable_watermark(db);")
    parts.append("    cache.model.copy_device(dir, cut);  // power cut")
    parts.append("  }")
    parts.append("  assert_hints_durable(cache.model);")
    opts = _build_open_opts(state)
    parts.append(f"  assert_vacuum_recoverable(dir, before, {{{opts}}});")
    # The cut copy: the durable baseline, alone or with every overwrite, and
    # with them once the watermark covers them. A key gone is #245.
    last_seq = (
        "before.keys.next_seq - 1" if state.has_unsynced_superseder else "0"
    )
    parts.append("  assert_power_loss_outcome(")
    parts.append("      cut,")
    parts.append("      {.baseline = durable_before,")
    parts.append("       .after = before.keys.key_values,")
    parts.append(f"       .transition_last_seq = {last_seq},")
    parts.append(f"       .watermark = watermark}},")
    parts.append(f"      {{{opts}}});")
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
// AUTO-GENERATED by tests/proof/vacuum_compact/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for vacuum_compact_file(). Each test creates a
// DB with a sealed file, optionally injects a fault, calls vacuum(), and
// verifies the file was correctly compacted or that the DB is clean on failure.
// Every cell checks that no .data.tmp staging copy outlives vacuum(), whether it
// returned or threw (#235).
// VC5 verifies that a directory holding a compacted file and its source — a kill
// after the commit, before the unlink — recovers, with the vacuum undone.
// VC6 (#104 M3) verifies that a copy renamed but never committed is removed by
// the vacuum that placed it (#304): every cell but VC5 checks that no data or
// hint file outlives vacuum() unless the published state references it.
// VC7 (#261) fails the fdatasync vacuum issues before dropping entries that
// only sync = false writes supersede. Every cell cuts the power after the
// vacuum (#265) and recovers the directory as the device held it: the durable
// baseline must be there, with every overwrite or with none.

#include <cstdint>
#include <map>
#include <string>
#include <system_error>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::assert_hints_durable;
using bytecask::testing::assert_power_loss_outcome;
using bytecask::testing::assert_vacuum_no_change;
using bytecask::testing::durable_watermark;
using bytecask::testing::key_values;
using bytecask::testing::make_durable;
using bytecask::testing::assert_vacuum_recoverable;
using bytecask::testing::unreferenced_files;
using bytecask::testing::assert_vacuum_success;
using bytecask::testing::capture_vacuum_baseline;
using bytecask::testing::find_vacuum_target;
using bytecask::testing::staging_data_files;
using bytecask::testing::to_bytes;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_vac_compact_{}_{}",
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
    tests: List[str] = []
    for state, failure in generate_matrix():
        tests.append(gen_test(state, failure))
    return FILE_HEADER + "\n\n".join(tests) + "\n"


def main() -> None:
    output_dir = Path(__file__).resolve().parent.parent / "generated"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / "prove_vacuum_compact.cpp"
    content = generate_file()
    output_path.write_text(content)
    count = sum(1 for _ in generate_matrix())
    print(f"Generated {count} tests → {output_path}")


if __name__ == "__main__":
    main()
