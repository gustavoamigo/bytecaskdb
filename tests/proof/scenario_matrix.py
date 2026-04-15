# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for correctness validation: state shapes, plan shapes,
# failure classes, and the validity filter.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, List, Optional, Tuple


class OpType(Enum):
    PUT = "put"
    DELETE = "delete"


class FailureClass(Enum):
    SUCCESS = "success"
    A = "before_any_io"
    B1 = "append_fails_nothing_written"
    B2 = "append_fails_partial_write"
    B3 = "append_fails_after_full_write"
    C = "on_bulk_end_append"
    F = "commit_sync_fails"
    G = "rotation_sync_fails"
    H = "rotation_file_creation_fails"


@dataclass(frozen=True)
class StateShape:
    label: str
    num_keys: int
    max_file_bytes: Optional[int] = None

    @property
    def at_rotation(self) -> bool:
        return self.max_file_bytes is not None


@dataclass(frozen=True)
class PlanShape:
    label: str
    ops: Tuple[OpType, ...]
    has_guards: bool = False
    is_conflicting: bool = False

    @property
    def is_single_entry(self) -> bool:
        return len(self.ops) == 1

    @property
    def is_multi_entry(self) -> bool:
        return len(self.ops) > 1

    @property
    def write_count(self) -> int:
        return len(self.ops)


# ---- Concrete instances ----

STATE_SHAPES = [
    StateShape("empty_db", num_keys=0),
    StateShape("single_key", num_keys=1),
    StateShape("populated_db", num_keys=10),
    StateShape("rotation_threshold", num_keys=1, max_file_bytes=1),
]

PLAN_SHAPES = [
    PlanShape("single_put", (OpType.PUT,)),
    PlanShape("single_delete", (OpType.DELETE,)),
    PlanShape("multi_put", (OpType.PUT, OpType.PUT)),
    PlanShape("mixed_batch", (OpType.PUT, OpType.DELETE)),
    PlanShape("large_batch", (OpType.PUT, OpType.PUT, OpType.PUT)),
    PlanShape("single_put_with_guards", (OpType.PUT,), has_guards=True),
    PlanShape("conflicting_plan", (OpType.PUT,), is_conflicting=True),
]

FAILURE_CLASSES = list(FailureClass)

MULTI_ENTRY_ONLY_CLASSES = {FailureClass.C}
ROTATION_ONLY_CLASSES = {FailureClass.G, FailureClass.H}


def is_valid_combination(
    state: StateShape, plan: PlanShape, failure: FailureClass
) -> bool:
    """Returns True if this (state, plan, failure) triple is a valid test."""
    # C, D, E require multi-entry batches
    if plan.is_single_entry and failure in MULTI_ENTRY_ONLY_CLASSES:
        return False
    # Class A only valid for conflicting_plan
    if failure == FailureClass.A and not plan.is_conflicting:
        return False
    # conflicting_plan only valid for class A
    if plan.is_conflicting and failure != FailureClass.A:
        return False
    # G, H only valid for rotation_threshold state
    if failure in ROTATION_ONLY_CLASSES and not state.at_rotation:
        return False
    return True


def generate_matrix() -> Generator[
    Tuple[StateShape, PlanShape, FailureClass], None, None
]:
    """Yields all valid (StateShape, PlanShape, FailureClass) triples."""
    for state in STATE_SHAPES:
        for plan in PLAN_SHAPES:
            for failure in FAILURE_CLASSES:
                if is_valid_combination(state, plan, failure):
                    yield (state, plan, failure)
