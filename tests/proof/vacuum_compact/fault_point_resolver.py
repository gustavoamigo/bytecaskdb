# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps VacuumCompactFailureClass to fault injection parameters.

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from .scenario_matrix import CompactStateShape, VacuumCompactFailureClass


@dataclass(frozen=True)
class CompactFault:
    name: str
    # Which checkpoint of that name fails: 0 the first, N the Nth
    # (FaultInjector::fail_on_nth_match).
    nth: int = 0

_COMPACT_FAULT_NAMES = {
    VacuumCompactFailureClass.VC1: "io_vacuum_compact_tmp_create",
    VacuumCompactFailureClass.VC2: "io_data_file_append",
    VacuumCompactFailureClass.VC3: "io_data_file_sync",
    VacuumCompactFailureClass.VC4: "io_vacuum_compact_rename",
    VacuumCompactFailureClass.VC5: "io_vacuum_compact_unlink",
    VacuumCompactFailureClass.VC6: "io_vacuum_compact_post_rename",
}


def resolve_compact_fault(
    state: CompactStateShape, failure: VacuumCompactFailureClass
) -> Optional[CompactFault]:
    """Returns the fault to arm, or None for SUCCESS."""
    if failure == VacuumCompactFailureClass.VC7:
        # The compaction path syncs its staging copy first; the whole-file path
        # writes none, so its first data file sync is the one.
        return CompactFault("io_data_file_sync", nth=1 if state.whole_file else 2)
    name = _COMPACT_FAULT_NAMES.get(failure)
    return CompactFault(name) if name else None
