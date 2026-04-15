# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps VacuumAbsorbFailureClass to fault injection parameters.

from __future__ import annotations

from typing import Optional

from .scenario_matrix import VacuumAbsorbFailureClass

_ABSORB_FAULT_NAMES = {
    VacuumAbsorbFailureClass.VA1: "io_data_file_append",
    VacuumAbsorbFailureClass.VA2: "io_vacuum_absorb_sync",
}


def resolve_absorb_fault(failure: VacuumAbsorbFailureClass) -> Optional[str]:
    """Returns the fault point name, or None for SUCCESS."""
    return _ABSORB_FAULT_NAMES.get(failure)
