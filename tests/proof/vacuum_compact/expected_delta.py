# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for vacuum_compact correctness proof.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import CompactStateShape, VacuumCompactFailureClass


@dataclass(frozen=True)
class VacuumCompactDelta:
    threw: bool
    file_removed: bool


def vacuum_compact_delta(
    state: CompactStateShape, failure: VacuumCompactFailureClass
) -> VacuumCompactDelta:
    """
    Reference model for vacuum_compact.

    SUCCESS: old file replaced by new compacted sealed file.
    VC1–VC4: vacuum throws — old file remains, DB operational, not degraded.
    VC5: vacuum throws after the commit — the compacted file is in state and
    the source is gone from it, exactly as on success, but the source is
    still on disk. That is the kill inside vacuum's publish window: both
    files hold the same entries under the same sequences. Recovery undoes
    the vacuum by deleting the compacted file, and assert_vacuum_recoverable
    proves the directory opens with every key and sequence-disjoint files.

    VC4 note: after a successful rename the tmp file is valid on disk but
    unreferenced. The .data.tmp extension is not scanned by recovery, so it
    is a harmless disk orphan — assert_vacuum_recoverable confirms this.
    """
    if failure == VacuumCompactFailureClass.SUCCESS:
        return VacuumCompactDelta(threw=False, file_removed=True)
    if failure == VacuumCompactFailureClass.VC5:
        return VacuumCompactDelta(threw=True, file_removed=True)
    return VacuumCompactDelta(threw=True, file_removed=False)
