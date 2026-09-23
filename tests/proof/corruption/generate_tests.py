#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Generates prove_corruption.cpp from the corruption scenario matrix.
#
# Usage (from project root):
#     python3 tests/proof/corruption/generate_tests.py

from __future__ import annotations

import sys
from pathlib import Path
from typing import List

_project_root = Path(__file__).resolve().parent.parent.parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from tests.proof.corruption.expected_delta import corruption_delta
from tests.proof.corruption.scenario_matrix import (
    FIELD_OFFSET,
    CorruptField,
    CorruptionShape,
    generate_matrix,
)

_POKE = {
    CorruptField.CRC: "flip_byte_at",
    CorruptField.VALUE_SIZE: "poke_huge_value_size",
    CorruptField.ENTRY_TYPE: "poke_invalid_entry_type",
    CorruptField.SEQUENCE: "poke_zero_sequence",
}


def gen_setup(shape: CorruptionShape) -> List[str]:
    """Fixed-size entries, so every offset is arithmetic rather than parsed."""
    lines: List[str] = []
    lines.append("    auto db = bytecask::DB::open(dir);")
    if shape.batched:
        lines.append("    // k0, k1 standalone, then a batch — the corruption lands")
        lines.append("    // inside it, and an incomplete batch is discarded whole.")
        for i in range(2):
            lines.append(
                f'    db.put({{.sync = true}}, to_bytes("k{i}"), to_bytes("v{i}"));'
            )
        lines.append("    {")
        lines.append("      bytecask::WritePlan plan;")
        lines.append('      plan.put(to_bytes("b0"), to_bytes("w0"));')
        lines.append('      plan.put(to_bytes("b1"), to_bytes("w1"));')
        lines.append("      REQUIRE(db.apply_batch({.sync = true}, std::move(plan)));")
        lines.append("    }")
    else:
        lines.append("    // Six 23-byte entries: entry i begins at 23*i.")
        for i in range(6):
            lines.append(
                f'    db.put({{.sync = true}}, to_bytes("k{i}"), to_bytes("v{i}"));'
            )
    return lines


def gen_test(shape: CorruptionShape, field: CorruptField) -> str:
    delta = corruption_delta(shape, field)
    off = shape.corrupt_offset + FIELD_OFFSET[field]
    name = f"prove_corruption__{shape.label}__{field.value}"

    p: List[str] = []
    p.append(f'TEST_CASE("{name}", "[prove_corruption]") {{')
    p.append("  TempDir td;")
    p.append('  auto dir = td.path / "db";')
    p.append("  std::vector<char> damaged;")
    p.append("  {")
    p.extend(gen_setup(shape))
    p.append("")
    p.append("    // Every key above was appended, synced and published. The")
    p.append("    // damage lands afterwards, which is what separates this axis")
    p.append("    // from the syscall-failure ones: it sits inside bytes readers")
    p.append("    // have already been served, not in a tail nobody saw.")
    p.append("    {")
    p.append("      bytecask::testing::ScopedFaultInjector fi{\"io_data_file_sync\"};")
    p.append("      REQUIRE_THROWS_AS(")
    p.append('          db.put({.sync = true}, to_bytes("zz"), to_bytes("wz")),')
    p.append("          std::system_error);")
    p.append("    }")
    p.append("    REQUIRE(db.is_degraded());")
    p.append("")
    p.append(
        f"    // Damage the {field.value} field of the entry at byte"
        f" {shape.corrupt_offset}."
    )
    p.append(f"    {_POKE[field]}(dir, {off});")
    p.append(f"    damaged = data_file_prefix(dir, {delta.guarded_bytes});")
    p.append("")
    p.append("    // No consistent state to resume into: refuse, twice, and leave")
    p.append("    // every byte where it was — the published extent and the")
    p.append("    // unpublished entry after it alike.")
    p.append("    for (int attempt = 0; attempt < 2; ++attempt) {")
    p.append("      INFO(\"resume attempt \" << attempt);")
    p.append("      CHECK(resume_refuses_over_damage(db));")
    p.append("      CHECK(db.is_degraded());")
    p.append(f"      CHECK(data_file_prefix(dir, {delta.guarded_bytes}) == damaged);")
    p.append("    }")
    p.append("  }")
    if delta.open_refuses:
        p.append("  // A cold open reads the same damage and refuses the same way,")
        p.append("  // under the default fail_recovery_on_crc_errors, without")
        p.append("  // trimming the file to the point it could parse.")
        p.append("  REQUIRE_THROWS_AS(bytecask::DB::open(dir), std::runtime_error);")
        p.append(f"  CHECK(data_file_prefix(dir, {delta.guarded_bytes}) == damaged);")
    else:
        p.append(f"  // No cold-open assertion: a {field.value} damaged this way")
        p.append("  // reads as the end of written data, which is also what the")
        p.append("  // unwritten tail of a crashed active file looks like, and a")
        p.append("  // hint-less file records no committed length to tell the two")
        p.append("  // apart. resume() can only because the published state knows")
        p.append("  // the extent. See docs/correctness_validation.md, class M4.")
    p.append("}")
    return "\n".join(p)


FILE_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// AUTO-GENERATED by tests/proof/corruption/generate_tests.py — DO NOT EDIT.
//
// Correctness proof tests for the corruption axis (#104 class M4). Classes A
// through H model syscalls that fail; these model a read that succeeds and
// returns something wrong, so the axis is over the bytes rather than the
// calls. Each test publishes a known set of entries, damages one field of one
// entry at a computed offset, and verifies the engine fails stop: resume()
// refuses and stays degraded, a cold open refuses wherever the format can see
// the damage, and neither truncates a byte. There is no recovery contract for
// damaged published data — only a promise not to make it worse.

#include <string_view>
#include <system_error>
#include <vector>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>

import bytecask;

#include "proof/invariants.h"

namespace {

using bytecask::testing::data_file_prefix;
using bytecask::testing::flip_byte_at;
using bytecask::testing::poke_huge_value_size;
using bytecask::testing::poke_invalid_entry_type;
using bytecask::testing::poke_zero_sequence;
using bytecask::testing::to_bytes;

// resume() must refuse with the corruption it found — not an I/O error, which
// it rethrows as-is, and not by succeeding.
auto resume_refuses_over_damage(bytecask::DB& db) -> bool {
  try {
    db.resume();
  } catch (const std::system_error&) {
    return false;
  } catch (const std::runtime_error& e) {
    return std::string_view{e.what()}.find("refusing to truncate") !=
           std::string_view::npos;
  }
  return false;
}

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "prove_corrupt_{}_{}",
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
    out = out_dir / "prove_corruption.cpp"
    out.write_text(generate_file())
    print(f"Generated {sum(1 for _ in generate_matrix())} tests → {out}")


if __name__ == "__main__":
    main()
