#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_resume.cpp from the resume scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/resume/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

_project_root = Path(__file__).resolve().parent.parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.resume.expected_delta import ResumeDelta, resume_delta
from tests.proof.resume.fault_point_resolver import resolve_resume_fault
from tests.proof.resume.scenario_matrix import (
    DegradeShape,
    DegradeVia,
    ResumeFailureClass,
    generate_matrix,
)

# ---------------------------------------------------------------------------
# C++ code generation
# ---------------------------------------------------------------------------


def gen_degrade_setup(degrade: DegradeShape) -> str:
    """Generate C++ to establish the degraded state."""
    if degrade.degrade_via == DegradeVia.H:
        return (
            "    // Establish degrade_H: write k0, then fault on rotation after p0.\n"
            "    auto db = bytecask::DB::open(dir, {.max_file_bytes = 30});\n"
            '    db.put({.sync = false}, to_bytes("k0"), to_bytes("v0"));\n'
            "    {\n"
            '      bytecask::testing::ScopedFaultInjector fi_degrade{"io_rotate_file_creation"};\n'
            "      REQUIRE_THROWS_AS(\n"
            '          db.put({.sync = true}, to_bytes("p0"), to_bytes("new0")),\n'
            "          std::system_error);\n"
            "    }\n"
            "    REQUIRE(db.is_degraded());"
        )
    else:  # DegradeVia.C
        return (
            "    // Establish degrade_C: k0 committed; 2-op batch fails at BulkEnd\n"
            "    // (fail_at=3 cascades: BulkEnd + isolation sync + rotation all fail).\n"
            "    // Orphaned BulkBegin+p0+p1 remain in active file — truncation needed.\n"
            "    auto db = bytecask::DB::open(dir);\n"
            '    db.put({.sync = false}, to_bytes("k0"), to_bytes("v0"));\n'
            "    {\n"
            "      bytecask::Batch batch;\n"
            '      batch.put(to_bytes("p0"), to_bytes("new0"));\n'
            '      batch.put(to_bytes("p1"), to_bytes("new1"));\n'
            "      bytecask::testing::ScopedFaultInjector fi_degrade{3};\n"
            "      REQUIRE_THROWS_AS(\n"
            "          db.apply_batch({.sync = true}, std::move(batch)),\n"
            "          std::system_error);\n"
            "    }\n"
            "    REQUIRE(db.is_degraded());"
        )


def gen_fault_phase(fault_name: str) -> str:
    """Generate Phase 2: inject resume fault → stays degraded."""
    return (
        f"    // Phase 2: inject resume fault → resume() throws, stays degraded.\n"
        f"    {{\n"
        f'      bytecask::testing::ScopedFaultInjector fi_resume{{"{fault_name}"}};\n'
        f"      REQUIRE_THROWS_AS(db.resume(), std::system_error);\n"
        f"    }}\n"
        f"    REQUIRE(db.is_degraded());"
    )


def gen_clean_resume_and_checks(delta: ResumeDelta) -> str:
    """Generate Phase 3: clean resume + key visibility checks."""
    lines: List[str] = []
    lines.append(
        "    // Phase 3: clean resume → clears degraded flag."
        if delta.first_threw
        else "    // resume() succeeds on first attempt."
    )
    lines.append("    REQUIRE_NOTHROW(db.resume());")
    lines.append("    REQUIRE_FALSE(db.is_degraded());")
    for key in delta.keys_present:
        lines.append(f'    CHECK(db.contains_key(to_bytes("{key}")));')
    for key in delta.keys_absent:
        lines.append(f'    CHECK_FALSE(db.contains_key(to_bytes("{key}")));')
    lines.append("    assert_consistent(db);")
    return "\n".join(lines)


def gen_recovery_check(delta: ResumeDelta) -> str:
    """Generate assert_keys_recoverable call after db scope closes."""
    present = "{" + ", ".join(f'"{k}"' for k in delta.keys_present) + "}"
    absent = "{" + ", ".join(f'"{k}"' for k in delta.keys_absent) + "}"
    return f"  assert_keys_recoverable(dir, {present}, {absent});"


def gen_test(degrade: DegradeShape, failure: ResumeFailureClass) -> str:
    """Generate one complete TEST_CASE."""
    delta = resume_delta(degrade, failure)
    fault_name = resolve_resume_fault(failure)
    name = f"prove_resume__{degrade.label}__{failure.value}"

    parts: List[str] = []
    parts.append(f'TEST_CASE("{name}", "[prove_resume]") {{')
    parts.append("  TempDir td;")
    parts.append('  auto dir = td.path / "db";')
    parts.append("  {")
    parts.append(gen_degrade_setup(degrade))
    parts.append("")

    if fault_name:
        parts.append(gen_fault_phase(fault_name))
        parts.append("")

    parts.append(gen_clean_resume_and_checks(delta))
    parts.append("  }")
    parts.append(gen_recovery_check(delta))
    parts.append("}")
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# File generation
# ---------------------------------------------------------------------------

FILE_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// AUTO-GENERATED by tests/proof/resume/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for resume(). Each test establishes a degraded
// state, optionally injects a fault inside resume() (verifying it stays
// degraded), then performs a clean resume() and verifies recovery.

#include <system_error>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::assert_consistent;
using bytecask::testing::assert_keys_recoverable;
using bytecask::testing::to_bytes;

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_resume_{}",
                 std::chrono::system_clock::now().time_since_epoch().count())} {
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

"""


def generate_file() -> str:
    tests: List[str] = []
    for degrade, failure in generate_matrix():
        tests.append(gen_test(degrade, failure))
    return FILE_HEADER + "\n\n".join(tests) + "\n"


def main() -> None:
    output_dir = Path(__file__).resolve().parent.parent / "generated"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / "prove_resume.cpp"
    content = generate_file()
    output_path.write_text(content)
    count = sum(1 for _ in generate_matrix())
    print(f"Generated {count} tests → {output_path}")


if __name__ == "__main__":
    main()
