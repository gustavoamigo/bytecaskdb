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
    # The staging copy and the placed copy are still on disk after vacuum()
    # threw, for the next open to remove.
    copy_left: bool = False


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
    In memory nothing changed, as for VC1–VC4, and on disk too: vacuum
    removes the copy it placed under its final name, and the hint it may
    have written, on the way out (#304). Left there, a vacuum retried under
    a persistent fault placed one copy per attempt, each costing the next
    open a recovery pass. A kill in the window still leaves the copy, which
    recovery deletes; VC5 and the recovery tests cover that pair.

    VC1–VC4 fail before the rename: vacuum removes its .data.tmp staging copy
    on the way out (#235), so a retry under a persistent fault does not leave
    a copy per attempt. Every cell checks no staging copy remains.

    VC7: the fdatasync that makes the superseding sync = false writes durable
    fails (#261). The engine degrades — a sync after a failed one proves
    nothing — and vacuum throws before committing, so the old file stays in
    the published state and on disk, as for VC1–VC4.

    VC8: the staging copy's shrink_to_fit fails (#258). As for VC1-VC4: vacuum
    throws, the old file stays and the staging copy is removed.

    VC9: renameDataFileExclusive's link() + unlink() fallback placed the copy
    and the unlink() of the staged name failed (#258). The function takes the
    placed name back before it throws, so vacuum's cleanup, which removes
    only the staged name, leaves nothing: the outcome is VC4's. Without the
    take-back the placed copy stayed until the next open, one per retry.

    VC10: the same under a fault that fails every unlink and remove: the
    take-back and the cleanup fail too, and both names stay (copy_left). The
    next open deletes the staged name and the placed copy beside its source,
    and assert_vacuum_recoverable proves it opens with every key.

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
    if failure == VacuumCompactFailureClass.VC10:
        return VacuumCompactDelta(threw=True, file_removed=False, copy_left=True)
    return VacuumCompactDelta(threw=True, file_removed=False)
