# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for resume() correctness proof.

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List

from .scenario_matrix import DegradeShape, DegradeVia, ResumeFailureClass


@dataclass(frozen=True)
class ResumeDelta:
    # Keys that must be present after all resume() calls complete, mapped to
    # the value they must read back as. A wrong truncation leaves the key
    # directory intact while the bytes behind it are gone, so presence alone
    # is not enough to assert — the cells read the value.
    keys_present: Dict[str, str]
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

    degrade_B2: k0 was committed; p0's writev returned short, leaving a torn
               entry whose CRC cannot hold. resume()'s scan stops at it and
               truncates back to after k0. Only k0 survives — and its value
               must still read back, which is what distinguishes a correct
               truncation from one that cut too far.

    degrade_B3: k0 was committed; p0's writev wrote every byte and then
               returned an error. offset_ never advanced, so the entry lies
               past the file's committed offset and resume() discards it.
               Only k0 survives.

    degrade_F_range: k0 and k1 were committed (sync=false); a del_range over
               [k, l) was appended but its commit sync failed, so the range
               tombstone reached the page cache and the key directory was
               never told. resume() replays the entry, and replaying a range
               tombstone means applying it: both keys go. A resume() that
               ignored the entry would leave the engine holding keys a fresh
               open would not.

    degrade_F_batch: a 2-op batch committed (sync=false), so the active file
               holds BulkBegin, p0, p1, BulkEnd below the failure point; then
               k0's commit sync fails. All three keys survive. What this shape
               is really for is the file's sequence bounds: the markers
               consume sequences 1 and 4, and a resume() that did not collect
               them reported min_sequence 2 for a file whose first entry is
               sequence 1 — a bound a cold open, whose hint file does carry
               markers, computes differently.
    """
    if degrade.degrade_via in (DegradeVia.H, DegradeVia.F, DegradeVia.G):
        keys_present = {"k0": "v0", "p0": "new0"}
        keys_absent: List[str] = []
    elif degrade.degrade_via == DegradeVia.C:
        keys_present = {"k0": "v0"}
        keys_absent = ["p0", "p1"]
    elif degrade.degrade_via in (DegradeVia.B2, DegradeVia.B3):
        keys_present = {"k0": "v0"}
        keys_absent = ["p0"]
    elif degrade.degrade_via == DegradeVia.F_RANGE:
        keys_present = {}
        keys_absent = ["k0", "k1"]
    else:  # DegradeVia.F_BATCH
        keys_present = {"p0": "new0", "p1": "new1", "k0": "v0"}
        keys_absent: List[str] = []

    first_threw = failure not in (
        ResumeFailureClass.SUCCESS,
        ResumeFailureClass.DOUBLE,
    )
    return ResumeDelta(keys_present, keys_absent, first_threw)
