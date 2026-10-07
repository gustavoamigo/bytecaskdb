# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps VacuumCompactFailureClass to fault injection parameters.

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from .scenario_matrix import CompactStateShape, VacuumCompactFailureClass


@dataclass(frozen=True)
class SyscallRule:
    """One bytecask::testing::SyscallFaultRule, in SyscallFault::before mode."""
    call: str
    nth: int = 1
    cascade: bool = False
    err: str = ""  # an errno constant; empty for the call's usual EIO


@dataclass(frozen=True)
class CompactFault:
    # A FAULT_INJECTION checkpoint, or empty for a fault below the engine.
    name: str = ""
    # Which checkpoint of that name fails: 0 the first, N the Nth
    # (FaultInjector::fail_on_nth_match).
    nth: int = 0
    # Faults below the engine (ScopedSyscallFaults), and the calls they must
    # fail first, in order, as prefixes and suffixes of the report's entries.
    syscall_rules: Tuple[SyscallRule, ...] = ()
    expected_failed: Tuple[Tuple[str, str], ...] = ()


# renameat2 reports EINVAL, as on a filesystem without RENAME_NOREPLACE, so
# renameDataFileExclusive takes its link() + unlink() fallback.
_NO_RENAME_NOREPLACE = SyscallRule("renameat2", cascade=True, err="EINVAL")
_FALLBACK_FAILED = (("renameat2(", ".data.tmp)"), ("unlink(", ".data.tmp)"))

_COMPACT_FAULT_NAMES = {
    VacuumCompactFailureClass.VC1: "io_vacuum_compact_tmp_create",
    VacuumCompactFailureClass.VC2: "io_data_file_append",
    VacuumCompactFailureClass.VC3: "io_data_file_sync",
    VacuumCompactFailureClass.VC4: "io_vacuum_compact_rename",
    VacuumCompactFailureClass.VC5: "io_vacuum_compact_unlink",
    VacuumCompactFailureClass.VC6: "io_vacuum_compact_post_rename",
    VacuumCompactFailureClass.VC8: "io_vacuum_compact_shrink",
}


def resolve_compact_fault(
    state: CompactStateShape, failure: VacuumCompactFailureClass
) -> Optional[CompactFault]:
    """Returns the fault to arm, or None for SUCCESS."""
    if failure == VacuumCompactFailureClass.VC7:
        # The compaction path syncs its staging copy first; the whole-file path
        # writes none, so its first data file sync is the one.
        return CompactFault("io_data_file_sync", nth=1 if state.whole_file else 2)
    if failure == VacuumCompactFailureClass.VC9:
        # The staged name's unlink fails once; the take-back succeeds.
        return CompactFault(
            syscall_rules=(_NO_RENAME_NOREPLACE, SyscallRule("unlink")),
            expected_failed=_FALLBACK_FAILED,
        )
    if failure == VacuumCompactFailureClass.VC10:
        # Every unlink and remove fails: the take-back's, and the cleanup's.
        return CompactFault(
            syscall_rules=(
                _NO_RENAME_NOREPLACE,
                SyscallRule("unlink", cascade=True),
                SyscallRule("remove", cascade=True),
            ),
            expected_failed=_FALLBACK_FAILED,
        )
    name = _COMPACT_FAULT_NAMES.get(failure)
    return CompactFault(name) if name else None
