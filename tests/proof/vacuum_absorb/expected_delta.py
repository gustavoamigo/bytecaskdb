# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for vacuum_absorb correctness proof.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import AbsorbStateShape, VacuumAbsorbFailureClass


@dataclass(frozen=True)
class VacuumAbsorbDelta:
    # Did vacuum() throw?
    threw: bool
    # Was the vacuumed file removed from engine state?
    file_removed: bool


def vacuum_absorb_delta(
    state: AbsorbStateShape, failure: VacuumAbsorbFailureClass
) -> VacuumAbsorbDelta:
    """
    Reference model for vacuum_absorb.

    All-dead files (live_bytes == 0) take the vacuum_remove fast path — no I/O,
    no fault points reachable. Always succeeds regardless of injected fault.

    SUCCESS: vacuum completes — sealed file absorbed into active, old file removed.
    VA1/VA2: vacuum throws — old file remains, DB operational, not degraded.
    """
    # All-dead files bypass scan/copy — faults never fire.
    if not state.live_keys:
        return VacuumAbsorbDelta(threw=False, file_removed=True)
    if failure == VacuumAbsorbFailureClass.SUCCESS:
        return VacuumAbsorbDelta(threw=False, file_removed=True)
    return VacuumAbsorbDelta(threw=True, file_removed=False)
