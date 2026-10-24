# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps RecoveryFailureClass to the hint faults, which mirror what the data
# file already has: write, sync, rename. The read side has one fault point and
# two phases, so a read class also says which read of it fails.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional

from .scenario_matrix import RecoveryFailureClass


class FaultTarget(Enum):
    # Fail every checkpoint of the name: right where there is one call of it,
    # the three hint writes.
    EVERY_CALL = "every_call"
    # The first checkpoint of the name: a hint file's first read is the one in
    # the pass that verifies it.
    FIRST_CALL = "first_call"
    # The first read of a scan. Where that read falls is a property of the
    # directory and of the recovery code, so generate_tests.py asks for it at
    # run time rather than spelling a C++ expression here.
    FIRST_SCAN_READ = "first_scan_read"


@dataclass(frozen=True)
class RecoveryFault:
    # The FAULT_INJECTION name to arm.
    name: str
    # Which checkpoint of the name fails.
    target: FaultTarget = FaultTarget.EVERY_CALL


_FAULTS = {
    RecoveryFailureClass.RC1: RecoveryFault("io_hint_write"),
    RecoveryFailureClass.RC2: RecoveryFault("io_hint_sync"),
    RecoveryFailureClass.RC3: RecoveryFault("io_hint_rename"),
    # The two phases a hint read fails in: what DB::open does about the
    # failure is decided by the phase, so each phase takes its own cell.
    RecoveryFailureClass.RC4: RecoveryFault("io_hint_read",
                                            FaultTarget.FIRST_CALL),
    RecoveryFailureClass.RC5: RecoveryFault("io_hint_read",
                                            FaultTarget.FIRST_SCAN_READ),
}


def resolve_recovery_fault(failure: RecoveryFailureClass) -> Optional[RecoveryFault]:
    return _FAULTS.get(failure)