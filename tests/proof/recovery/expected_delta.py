# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for the recovery / hint-generation proof.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import RecoveryFailureClass, RecoveryStateShape


@dataclass(frozen=True)
class RecoveryDelta:
    # Did DB::open throw? recovery_prepare_files calls flush_hints_for without
    # a catch, so a failure regenerating a hint propagates out of open.
    open_throws: bool
    # After the fault clears, every key written before the damage is back.
    # This is the same in every cell, which is the point: a hint is an index,
    # so a failure generating one costs recovery time and never keys.
    all_keys_recovered: bool = True


def recovery_delta(
    state: RecoveryStateShape, failure: RecoveryFailureClass
) -> RecoveryDelta:
    return RecoveryDelta(open_throws=failure != RecoveryFailureClass.SUCCESS)
