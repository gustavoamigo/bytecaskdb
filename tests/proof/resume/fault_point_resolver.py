# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps ResumeFailureClass to fault injection parameters.

from __future__ import annotations

from typing import Optional

from .scenario_matrix import ResumeFailureClass

# Named fault point for each resume failure class.
_RESUME_FAULT_NAMES = {
    ResumeFailureClass.R1: "io_resume_truncate",
    ResumeFailureClass.R2: "io_resume_sync",
    ResumeFailureClass.R3: "io_resume_file_creation",
}


def resolve_resume_fault(failure: ResumeFailureClass) -> Optional[str]:
    """Returns the fault point name, or None for SUCCESS (no injection)."""
    return _RESUME_FAULT_NAMES.get(failure)
