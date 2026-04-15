# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps VacuumCompactFailureClass to fault injection parameters.

from __future__ import annotations

from typing import Optional

from .scenario_matrix import VacuumCompactFailureClass

_COMPACT_FAULT_NAMES = {
    VacuumCompactFailureClass.VC1: "io_vacuum_compact_tmp_create",
    VacuumCompactFailureClass.VC2: "io_data_file_append",
    VacuumCompactFailureClass.VC3: "io_data_file_sync",
    VacuumCompactFailureClass.VC4: "io_vacuum_compact_rename",
}


def resolve_compact_fault(failure: VacuumCompactFailureClass) -> Optional[str]:
    """Returns the fault point name, or None for SUCCESS."""
    return _COMPACT_FAULT_NAMES.get(failure)
