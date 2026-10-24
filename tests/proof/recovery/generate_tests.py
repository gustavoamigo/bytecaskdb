#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_recovery.cpp from the recovery scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/recovery/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

_project_root = Path(__file__).resolve().parent.parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.recovery.expected_delta import (
    HintReadPhase,
    RecoveryDelta,
    recovery_delta,
)
from tests.proof.recovery.fault_point_resolver import (
    RecoveryFault,
    resolve_recovery_fault,
)
from tests.proof.recovery.scenario_matrix import (
    Damage,
    RecoveryFailureClass,
    RecoveryStateShape,
    generate_matrix,
)

_DAMAGE_CALL = {
    Damage.DROP_NEWEST_HINT: "REQUIRE(drop_newest_hint(dir));",
    Damage.DROP_ALL_HINTS: "REQUIRE(drop_all_hints(dir) > 0);",
    Damage.CORRUPT_NEWEST_HINT: "REQUIRE(corrupt_newest_hint(dir));",
}


def _write_opts(state: RecoveryStateShape) -> str:
    if state.max_file_bytes is not None:
        return f"{{.max_file_bytes = {state.max_file_bytes}}}"
    return ""


def _open_opts(state: RecoveryStateShape, threads: int) -> str:
    parts: List[str] = []
    if state.max_file_bytes is not None:
        parts.append(f".max_file_bytes = {state.max_file_bytes}")
    parts.append(f".recovery_threads = {threads}")
    return "{" + ", ".join(parts) + "}"


def gen_setup(state: RecoveryStateShape) -> List[str]:
    """Write the pre-crash state and record what must come back."""
    lines: List[str] = []
    opts = _write_opts(state)
    open_call = (
        f"bytecask::DB::open(dir, {opts})" if opts else "bytecask::DB::open(dir)"
    )
    lines.append("  {")
    lines.append(f"    auto db = {open_call};")
    if state.has_batches:
        lines.append("    // Markers have to survive hint regeneration: a hint file")
        lines.append("    // carries BulkBegin/BulkEnd so recovery can compute")
        lines.append("    // durable_seq across a batch.")
        lines.append("    {")
        lines.append("      bytecask::WritePlan plan;")
        for i in range(state.num_keys):
            lines.append(
                f'      plan.put(to_bytes("k{i}"), to_bytes("v{i}"));'
            )
        lines.append("      REQUIRE(db.apply_batch({.sync = true}, std::move(plan)));")
        lines.append("    }")
    else:
        for i in range(state.num_keys):
            lines.append(
                f'    db.put({{.sync = true}}, to_bytes("k{i}"), to_bytes("v{i}"));'
            )
    if state.has_range_del:
        lines.append("    // A range tombstone has to survive it too — recovery reads")
        lines.append("    // it back out of the regenerated hint to suppress the range.")
        lines.append(
            '    db.del_range({.sync = true}, to_bytes("k1"), to_bytes("k3"));'
        )
    lines.append("  }")
    return lines


def expected_keys(state: RecoveryStateShape) -> List[tuple]:
    present = [(f"k{i}", f"v{i}") for i in range(state.num_keys)]
    if state.has_range_del:
        # del_range over ["k1", "k3") removes k1 and k2.
        present = [(k, v) for k, v in present if not ("k1" <= k < "k3")]
    return present


def absent_keys(state: RecoveryStateShape) -> List[str]:
    if not state.has_range_del:
        return []
    return [f"k{i}" for i in range(state.num_keys) if "k1" <= f"k{i}" < "k3"]


def gen_key_checks(state: RecoveryStateShape) -> List[str]:
    """Every key that must be back, checked on an open DB."""
    p: List[str] = []
    for key, value in expected_keys(state):
        p.append("    {")
        p.append("      bytecask::Bytes out;")
        p.append(f'      INFO("key must survive recovery: {key}");')
        p.append(f'      CHECK(db.get({{}}, to_bytes("{key}"), out));')
        p.append(f'      CHECK(to_string(out) == "{value}");')
        p.append("    }")
    for key in absent_keys(state):
        p.append(
            f'    CHECK_FALSE(db.contains_key({{}}, to_bytes("{key}")));'
        )
    p.append("    assert_consistent(db);")
    return p


def gen_recovered_block(state: RecoveryStateShape, opts: str) -> List[str]:
    """An open with no fault running, and every key inside it."""
    p = [
        "  {",
        f"    auto db = bytecask::DB::open(dir, {opts});",
        "    CHECK_FALSE(db.is_degraded());",
    ]
    p.extend(gen_key_checks(state))
    p.append("  }")
    return p


def gen_fault_block(
    state: RecoveryStateShape,
    delta: RecoveryDelta,
    fault: RecoveryFault,
    opts: str,
) -> List[str]:
    """The open under the fault, and what the phase it lands in costs."""
    p: List[str] = []
    if delta.phase == HintReadPhase.VERIFIED:
        p.append("  // A read that fails while the first hint is verified has the")
        p.append("  // hint rebuilt around it: it is an index, so recovery throws it")
        p.append("  // away and indexes its data file again. The open succeeds and")
        p.append("  // every key is in it. A hint's first read is the one that")
        p.append("  // verifies it.")
    elif delta.phase == HintReadPhase.SCAN:
        p.append("  // A read that fails during the scan of a hint that verified is")
        p.append("  // not answered by a rebuild: the entries of a hint are applied as")
        p.append("  // it scans, so the open throws. Recovery opens and scans the hint")
        p.append("  // of one data file at a time, in name order, so the read after")
        p.append("  // those that open the first hint with anything to scan is a read")
        p.append("  // of its scan.")
    else:
        p.append("  // A hint cannot be regenerated. recovery_prepare_files calls")
        p.append("  // flush_hints_for without a catch, so open fails.")
    p.append("  {")
    if fault.fail_on_nth is not None:
        # Before the injector is armed: measuring the read takes reads too,
        # under an injector of its own.
        p.append(f"    const int fail_on = {fault.fail_on_nth};")
    p.append(f'    bytecask::testing::ScopedFaultInjector fi{{"{fault.name}"}};')
    if fault.fail_on_nth is not None:
        p.append("    fi.inj.fail_on_nth_match = fail_on;")
    if delta.phase == HintReadPhase.VERIFIED:
        p.append(f"    auto db = bytecask::DB::open(dir, {opts});")
        p.append("    // The reads after the failure are the rebuild re-reading the")
        p.append("    // hint it replaced: no file went unindexed behind its hint.")
        p.append("    REQUIRE(fi.inj.name_matches > fail_on);")
        p.append("    CHECK_FALSE(db.is_degraded());")
        p.extend(gen_key_checks(state))
        p.append("  }")
    elif delta.phase == HintReadPhase.SCAN:
        p.append(f"    REQUIRE_THROWS_AS(bytecask::DB::open(dir, {opts}),")
        p.append("                      std::system_error);")
        p.append("    REQUIRE(fi.inj.name_matches >= fail_on);")
        p.append("  }")
    else:
        p.append(f"    REQUIRE_THROWS(bytecask::DB::open(dir, {opts}));")
        p.append("  }")
    return p


def gen_test(
    state: RecoveryStateShape, failure: RecoveryFailureClass, threads: int
) -> str:
    delta = recovery_delta(state, failure)
    fault = resolve_recovery_fault(failure)
    name = f"prove_recovery__{state.label}__{failure.value}__t{threads}"
    opts = _open_opts(state, threads)

    p: List[str] = []
    p.append(f'TEST_CASE("{name}", "[prove_recovery]") {{')
    p.append("  TempDir td;")
    p.append('  auto dir = td.path / "db";')
    p.extend(gen_setup(state))
    p.append("")
    p.append(f"  // Damage: {state.damage.value}")
    p.append(f"  {_DAMAGE_CALL[state.damage]}")
    p.append("")
    if fault is not None:
        p.extend(gen_fault_block(state, delta, fault, opts))
        p.append("")
        p.append("  // The fault clears. A hint is an index, not the record: the")
        p.append("  // failure cost recovery time and must have cost no keys.")
    p.extend(gen_recovered_block(state, opts))
    p.append("}")
    return "\n".join(p)


FILE_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// AUTO-GENERATED by tests/proof/recovery/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for DB::open's recovery and hint generation. Each
// test writes a known state, damages the hint files the way a crash or bad
// hardware would, optionally faults one of the three steps of the hint write
// (write -> fdatasync -> rename) or a read of a hint file, and verifies that
// once the fault clears every key comes back. A hint file is a rebuildable
// index, not the record. One fault's outcome is an exception to that: a read
// failing while a hint is verified is answered by a rebuild, while one
// failing during the scan of a hint that verified fails the open.

#include <system_error>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;
import bytecask.hint_file;

#include "proof/hint_reads.h"
#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::corrupt_newest_hint;
using bytecask::testing::drop_all_hints;
using bytecask::testing::drop_newest_hint;
using bytecask::testing::first_hint_scan_read;
using bytecask::testing::to_bytes;
using bytecask::testing::to_string;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_recovery_{}_{}",
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
    tests = [gen_test(s, f, t) for s, f, t in generate_matrix()]
    return FILE_HEADER + "\n\n".join(tests) + "\n"


def main() -> None:
    out_dir = Path(__file__).resolve().parent.parent / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "prove_recovery.cpp"
    out.write_text(generate_file())
    print(f"Generated {sum(1 for _ in generate_matrix())} tests → {out}")


if __name__ == "__main__":
    main()
