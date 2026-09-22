# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for the corruption axis.
#
# The delta is the same in every cell, which is what makes the axis cheap to
# assert: everything below the first damaged entry survives and stays
# READABLE, everything from it onward is truncated away, and the engine ends
# non-degraded. Presence is not a sufficient check — the bug this axis exists
# to catch leaves the key directory intact while the bytes behind it are gone,
# so each surviving key is read back by value.

from __future__ import annotations

from dataclasses import dataclass
from typing import List

from .scenario_matrix import CorruptField, CorruptionShape


@dataclass(frozen=True)
class CorruptionDelta:
    keys_present: dict      # key -> value it must still read back as
    keys_absent: List[str]
    valid_offset: int       # what the file must be truncated to
    degraded_after_resume: bool = False


def corruption_delta(
    shape: CorruptionShape, field: CorruptField
) -> CorruptionDelta:
    present = {k: f"v{k[1:]}" for k in shape.surviving_keys}
    return CorruptionDelta(
        keys_present=present,
        keys_absent=shape.lost_keys,
        valid_offset=shape.valid_offset,
    )
