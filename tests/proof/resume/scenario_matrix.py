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


@dataclass(frozen=True)
class DegradeShape:
    label: str
    degrade_via: DegradeVia


class ResumeFailureClass(Enum):
    SUCCESS = "success"
    R1 = "truncate_fails"        # io_resume_truncate
    R2 = "sync_fails"            # io_resume_sync
    R3 = "file_creation_fails"   # io_resume_file_creation


DEGRADE_SHAPES = [
    DegradeShape("degrade_H", DegradeVia.H),
    DegradeShape("degrade_C", DegradeVia.C),
]

RESUME_FAILURE_CLASSES = list(ResumeFailureClass)


def is_valid_combination(
    degrade: DegradeShape, failure: ResumeFailureClass
) -> bool:
    # R1 requires orphaned bytes in the active file so that the truncation
    # branch in resume() is actually reached. degrade_H has no orphaned bytes
    # (write committed cleanly, valid_offset == file.size()) so the guard
    # `if (file.size() != valid_offset)` skips truncate entirely — the fault
    # point is unreachable and the test would pass vacuously.
    if failure == ResumeFailureClass.R1 and degrade.degrade_via == DegradeVia.H:
        return False
    return True


def generate_matrix() -> Generator[
    Tuple[DegradeShape, ResumeFailureClass], None, None
]:
    for degrade in DEGRADE_SHAPES:
        for failure in RESUME_FAILURE_CLASSES:
            if is_valid_combination(degrade, failure):
                yield (degrade, failure)
