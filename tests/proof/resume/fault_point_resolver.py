# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps ResumeFailureClass to fault injection parameters.

from __future__ import annotations

from typing import Optional, Sequence, Union

from .scenario_matrix import ResumeFailureClass

# Named fault point for each resume failure class.
_RESUME_FAULT_NAMES = {
    ResumeFailureClass.R1: "io_resume_truncate",
    ResumeFailureClass.R2: "io_resume_sync",
    ResumeFailureClass.R3: "io_resume_file_creation",
}

# CASCADE injects R2 then R3 sequentially.
_CASCADE_FAULTS = ("io_resume_sync", "io_resume_file_creation")


def resolve_resume_fault(
    failure: ResumeFailureClass,
) -> Union[None, str, Sequence[str]]:
    """Returns the fault point name(s), or None for SUCCESS/DOUBLE (no injection).

    For CASCADE, returns a sequence of two fault names to inject in order.
    """
    if failure == ResumeFailureClass.CASCADE:
        return _CASCADE_FAULTS
    return _RESUME_FAULT_NAMES.get(failure)
