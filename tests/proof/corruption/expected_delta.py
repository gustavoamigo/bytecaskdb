# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for the corruption axis.
#
# Damage inside published bytes has no recovery contract: the engine cannot
# know what state the damage left it in, so it makes no promise about what the
# file still holds. What it does promise is to stop rather than make the
# damage worse — resume() refuses, stays degraded and truncates nothing, and a
# cold open refuses too wherever the format lets it see the damage. The delta
# is therefore the same shape in every cell.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import ENTRY_BYTES, CorruptField, CorruptionShape


@dataclass(frozen=True)
class CorruptionDelta:
    # Bytes that must survive every refusal untouched: the published extent,
    # plus the unpublished entry the failed write appended after it.
    guarded_bytes: int
    # Whether the cold open that follows must refuse as well.
    open_refuses: bool


def corruption_delta(
    shape: CorruptionShape, field: CorruptField
) -> CorruptionDelta:
    return CorruptionDelta(
        guarded_bytes=shape.published_extent + ENTRY_BYTES,
        open_refuses=field.detected_at_open,
    )
