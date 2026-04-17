# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for resume() correctness proof.

from __future__ import annotations

from dataclasses import dataclass
from typing import List

from .scenario_matrix import DegradeShape, DegradeVia, ResumeFailureClass


@dataclass(frozen=True)
class ResumeDelta:
    # Keys that must be present after all resume() calls complete.
    keys_present: List[str]
    # Keys that must be absent after all resume() calls complete.
    keys_absent: List[str]
    # Did the fault-injected resume() throw? (False for SUCCESS.)
    first_threw: bool


def resume_delta(degrade: DegradeShape, failure: ResumeFailureClass) -> ResumeDelta:
    """
    Reference model: computes the expected observable state after resume()
    completes (including a clean retry for R1/R2/R3/CASCADE failure cases).

    degrade_H: k0 was committed before degradation; p0 was also committed
               (it triggered the rotation fault after the write succeeded).
               Both survive resume().

    degrade_C: k0 was committed; the 2-op batch (p0, p1) failed at BulkEnd
               and all isolation attempts also failed (cascade from fail_at=3).
               Orphaned BulkBegin+p0+p1 bytes remain in the active file.
               resume() truncates back to after k0. Only k0 survives.

    degrade_F: k0 was committed (sync=false); p0 was appended but commit
               sync (fdatasync) failed. Bytes are in the page cache. resume()
               scans the active file, finds p0 as a valid committed entry,
               replays it. Both k0 and p0 survive.

    degrade_G: k0 was committed (sync=false); p0 was appended with sync=false
               on a small max_file_bytes DB. The pre-rotation sync failed.
               Bytes are in the page cache. resume() scans and replays p0.
               Both k0 and p0 survive.
    """
    if degrade.degrade_via == DegradeVia.H:
        keys_present = ["k0", "p0"]
        keys_absent: List[str] = []
    elif degrade.degrade_via == DegradeVia.C:
        keys_present = ["k0"]
        keys_absent = ["p0", "p1"]
    elif degrade.degrade_via == DegradeVia.F:
        keys_present = ["k0", "p0"]
        keys_absent: List[str] = []
    else:  # DegradeVia.G
        keys_present = ["k0", "p0"]
        keys_absent: List[str] = []

    first_threw = failure not in (
        ResumeFailureClass.SUCCESS,
        ResumeFailureClass.DOUBLE,
    )
    return ResumeDelta(keys_present, keys_absent, first_threw)
