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
    degraded: bool = False


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

    VC6: vacuum throws after the rename and before the commit — #104's M3.
    In memory nothing changed, as for VC1–VC4. On disk the compacted copy
    sits under its final name, unreferenced, beside the source it copies.
    Recovery must detect it and delete it: the cell records the orphan's
    path before closing and checks it is gone after the next open.

    VC1–VC4 fail before the rename: vacuum removes its .data.tmp staging copy
    on the way out (#235), so a retry under a persistent fault does not leave
    a copy per attempt. Every cell checks no staging copy remains.

    VC7: the fdatasync that makes the superseding sync = false writes durable
    fails (#261). The engine degrades — a sync after a failed one proves
    nothing — and vacuum throws before committing, so the old file stays in
    the published state and on disk, as for VC1–VC4.

    Every cell also cuts the power after the vacuum (#265). The recovered copy
    must hold the durable baseline alone, or with every overwrite: SUCCESS
    made the overwrites durable before dropping what they superseded, so they
    are required there; after VC7 nothing did, so either is allowed. A copy
    with a key missing — the old value dropped, the new one lost — fails
    both, which is #245.
    """
    if failure == VacuumCompactFailureClass.SUCCESS:
        return VacuumCompactDelta(threw=False, file_removed=True)
    if failure == VacuumCompactFailureClass.VC5:
        return VacuumCompactDelta(threw=True, file_removed=True)
    if failure == VacuumCompactFailureClass.VC7:
        return VacuumCompactDelta(threw=True, file_removed=False, degraded=True)
    return VacuumCompactDelta(threw=True, file_removed=False)
