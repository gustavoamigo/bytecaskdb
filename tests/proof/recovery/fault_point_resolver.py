# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps RecoveryFailureClass to the hint-write fault points, which mirror what
# the data file already has: write, sync, rename.

from __future__ import annotations

from typing import Optional

from .scenario_matrix import RecoveryFailureClass

_NAMES = {
    RecoveryFailureClass.RC1: "io_hint_write",
    RecoveryFailureClass.RC2: "io_hint_sync",
    RecoveryFailureClass.RC3: "io_hint_rename",
}


def resolve_recovery_fault(failure: RecoveryFailureClass) -> Optional[str]:
    return _NAMES.get(failure)
