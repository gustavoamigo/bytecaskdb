# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for the recovery / hint-generation proof.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional

from .scenario_matrix import RecoveryFailureClass, RecoveryStateShape


class HintReadPhase(Enum):
    # The two phases a read of a hint file lands in. They answer differently:
    # which one a failure lands in decides what DB::open does about it.
    VERIFIED = "while_verified"  # the pass that opens and checks a hint
    SCAN = "during_scan"         # the scan of a hint that verified


@dataclass(frozen=True)
class RecoveryDelta:
    # Where a hint read failed; None for a failure that is not a read.
    phase: Optional[HintReadPhase]
    # Did DB::open throw? recovery_prepare_files calls flush_hints_for
    # without a catch, so a failure regenerating a hint propagates out of
    # open. A read failure is answered by its phase: one while a hint is
    # verified costs the hint nothing — it is an index, so it is rebuilt from
    # its data file and the open succeeds with every key in it; one during the
    # scan of a hint that verified comes after entries were applied, so the
    # open throws and the next one recovers.
    open_throws: bool
    # After the fault clears, every key written before the damage is back.
    # This is the same in every cell, which is the point: a hint is an index,
    # so a failure generating or reading one costs recovery time and never
    # keys.
    all_keys_recovered: bool = True


def recovery_delta(
    state: RecoveryStateShape, failure: RecoveryFailureClass
) -> RecoveryDelta:
    if failure == RecoveryFailureClass.RC4:
        return RecoveryDelta(phase=HintReadPhase.VERIFIED, open_throws=False)
    if failure == RecoveryFailureClass.RC5:
        return RecoveryDelta(phase=HintReadPhase.SCAN, open_throws=True)
    return RecoveryDelta(
        phase=None, open_throws=failure != RecoveryFailureClass.SUCCESS
    )