# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for resume() correctness proof.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, Tuple


class DegradeVia(Enum):
    H = "rotation_file_creation"  # io_rotate_file_creation: write committed, rotation fails
    C = "bulk_end_append"         # fail_at=3 on 2-op batch: BulkEnd+isolation all fail
    F = "commit_sync"             # io_data_file_sync on sync=true: bytes in page cache
    G = "rotation_sync"           # io_data_file_sync on sync=false + small max_file_bytes


@dataclass(frozen=True)
class DegradeShape:
    label: str
    degrade_via: DegradeVia
    use_mmap: bool = False
    use_write_buffer: bool = False


class ResumeFailureClass(Enum):
    SUCCESS = "success"
    R1 = "truncate_fails"        # io_resume_truncate
    R2 = "sync_fails"            # io_resume_sync
    R3 = "file_creation_fails"   # io_resume_file_creation
    DOUBLE = "double_resume"     # resume succeeds, then resume again (no-op)
    CASCADE = "cascade_r2_r3"    # R2 fails, R3 fails, clean succeeds


DEGRADE_SHAPES = [
    DegradeShape("degrade_H", DegradeVia.H),
    DegradeShape("degrade_C", DegradeVia.C),
    DegradeShape("degrade_F", DegradeVia.F),
    DegradeShape("degrade_G", DegradeVia.G),
    DegradeShape("degrade_H_buffered", DegradeVia.H, use_mmap=True, use_write_buffer=True),
    DegradeShape("degrade_C_buffered", DegradeVia.C, use_mmap=True, use_write_buffer=True),
]

RESUME_FAILURE_CLASSES = list(ResumeFailureClass)


def is_valid_combination(
    degrade: DegradeShape, failure: ResumeFailureClass
) -> bool:
    # R1 requires orphaned bytes in the active file so that the truncation
    # branch in resume() is actually reached. degrade_H, degrade_F, and
    # degrade_G have no orphaned bytes (writes committed cleanly or bytes
    # are valid entries in the page cache — valid_offset == file.size())
    # so the guard `if (file.size() != valid_offset)` skips truncate
    # entirely — the fault point is unreachable and the test would pass
    # vacuously. Only degrade_C (orphaned BulkBegin) triggers truncation.
    if failure == ResumeFailureClass.R1 and degrade.degrade_via != DegradeVia.C:
        return False
    # R2 (sync) and CASCADE (R2→R3) require the file to NOT be sealed so
    # that resume() enters the truncate/sync/seal block. degrade_H seals
    # the file during rotation before the fault fires, so resume() skips
    # that block entirely — the sync fault point is unreachable.
    if degrade.degrade_via == DegradeVia.H and failure in (
        ResumeFailureClass.R2,
        ResumeFailureClass.CASCADE,
    ):
        return False
    return True


def generate_matrix() -> Generator[
    Tuple[DegradeShape, ResumeFailureClass], None, None
]:
    for degrade in DEGRADE_SHAPES:
        for failure in RESUME_FAILURE_CLASSES:
            if is_valid_combination(degrade, failure):
                yield (degrade, failure)
