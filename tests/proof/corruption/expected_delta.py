# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for the corruption axis.
#
# Damage inside published bytes has no recovery contract: the engine cannot
# know what state the damage left it in, so it makes no promise about what the
# file still holds. What it does promise is to stop rather than make the
# damage worse — resume() refuses, stays degraded and truncates nothing. A cold
# open cannot do the same: the damaged file is the newest, where a crash can
# leave a torn tail, and nothing on disk tells the two apart. It truncates at
# the first bad record, as for a torn tail (#138). The delta is therefore the
# same shape in every cell.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import ENTRY_BYTES, CorruptField, CorruptionShape


@dataclass(frozen=True)
class CorruptionDelta:
    # Bytes that must survive every refusal untouched: the published extent,
    # plus the unpublished entry the failed write appended after it.
    guarded_bytes: int
    # Where the cold open that follows truncates the file.
    open_truncates_to: int


def corruption_delta(
    shape: CorruptionShape, field: CorruptField
) -> CorruptionDelta:
    return CorruptionDelta(
        guarded_bytes=shape.published_extent + ENTRY_BYTES,
        open_truncates_to=shape.committed_before,
    )
