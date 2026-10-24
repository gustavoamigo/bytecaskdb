# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps RecoveryFailureClass to the hint faults, which mirror what the data
# file already has: write, sync, rename. The read side has one fault point and
# two phases, so a read class also says which read fails.

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from .scenario_matrix import RecoveryFailureClass


@dataclass(frozen=True)
class RecoveryFault:
    # The FAULT_INJECTION name to arm.
    name: str
    # A C++ expression for FaultInjector::fail_on_nth_match: which matching
    # checkpoint fails. None keeps the default — fail every checkpoint of the
    # name — which is right where there is one call of it, the three hint
    # writes. A hint is read many times, so a read class names the read that
    # lands in the phase it models. The generator computes the expression
    # into a local before it arms the injector.
    fail_on_nth: Optional[str] = None


_FAULTS = {
    RecoveryFailureClass.RC1: RecoveryFault("io_hint_write"),
    RecoveryFailureClass.RC2: RecoveryFault("io_hint_sync"),
    RecoveryFailureClass.RC3: RecoveryFault("io_hint_rename"),
    # A hint's first read is the one in the pass that verifies it.
    RecoveryFailureClass.RC4: RecoveryFault("io_hint_read", "1"),
    # The read after those that verify the hint the first data file's scan
    # reads is the first read of that scan.
    RecoveryFailureClass.RC5: RecoveryFault(
        "io_hint_read", "first_hint_scan_read(dir)"
    ),
}


def resolve_recovery_fault(failure: RecoveryFailureClass) -> Optional[RecoveryFault]:
    return _FAULTS.get(failure)