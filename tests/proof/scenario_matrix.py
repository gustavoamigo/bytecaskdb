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
    delete_after_create: bool = False  # create keys then delete them (tombstone state)
    use_mmap: bool = False

    @property
    def at_rotation(self) -> bool:
        return self.max_file_bytes is not None


@dataclass(frozen=True)
class PlanShape:
    label: str
    ops: Tuple[OpType, ...]
    has_guards: bool = False
    is_conflicting: bool = False
    causality_key: Optional[str] = None  # all ops target this key
    use_solo: bool = False  # route through solo writer instead of group commit

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
    StateShape("deleted_key", num_keys=1, delete_after_create=True),
    StateShape("single_key_buffered", num_keys=1, use_mmap=True),
    StateShape("populated_db_buffered", num_keys=10, use_mmap=True),
    StateShape(
        "rotation_threshold_buffered",
        num_keys=1,
        max_file_bytes=1,
        use_mmap=True,
    ),
]

PLAN_SHAPES = [
    PlanShape("single_put", (OpType.PUT,)),
    PlanShape("single_delete", (OpType.DELETE,)),
    PlanShape("multi_put", (OpType.PUT, OpType.PUT)),
    PlanShape("mixed_batch", (OpType.PUT, OpType.DELETE)),
    PlanShape("large_batch", (OpType.PUT, OpType.PUT, OpType.PUT)),
    PlanShape("single_put_with_guards", (OpType.PUT,), has_guards=True),
    PlanShape("conflicting_plan", (OpType.PUT,), is_conflicting=True),
    PlanShape("causality_overwrite", (OpType.PUT, OpType.PUT), causality_key="c0"),
    PlanShape("causality_put_del", (OpType.PUT, OpType.DELETE), causality_key="c0"),
    PlanShape("causality_del_put", (OpType.DELETE, OpType.PUT), causality_key="k0"),
    PlanShape(
        "causality_put_del_put",
        (OpType.PUT, OpType.DELETE, OpType.PUT),
        causality_key="c0",
    ),
    PlanShape(
        "solo_causality_overwrite",
        (OpType.PUT, OpType.PUT),
        causality_key="c0",
        use_solo=True,
    ),
    PlanShape(
        "solo_causality_put_del",
        (OpType.PUT, OpType.DELETE),
        causality_key="c0",
        use_solo=True,
    ),
    PlanShape(
        "solo_causality_del_put",
        (OpType.DELETE, OpType.PUT),
        causality_key="k0",
        use_solo=True,
    ),
    PlanShape(
        "solo_causality_put_del_put",
        (OpType.PUT, OpType.DELETE, OpType.PUT),
        causality_key="c0",
        use_solo=True,
    ),
    PlanShape("sequential_overwrite", (OpType.PUT,), causality_key="k0"),
    PlanShape("solo_sequential_overwrite", (OpType.PUT,), causality_key="k0", use_solo=True),
]

FAILURE_CLASSES = list(FailureClass)

MULTI_ENTRY_ONLY_CLASSES = {FailureClass.C}
ROTATION_ONLY_CLASSES = {FailureClass.G, FailureClass.H}

# Failure classes that leave the engine degraded, so the cell reaches
# assert_resumable() and therefore resume()'s truncate.
DEGRADING_CLASSES = {
    FailureClass.B1,
    FailureClass.B2,
    FailureClass.B3,
    FailureClass.C,
    FailureClass.F,
    FailureClass.G,
    FailureClass.H,
}


class Observer(Enum):
    """A view acquired before the transition and checked after it.

    The read path appears in the matrix only as the oracle's instrument —
    every assertion reads the DB *after* the transition. An observer makes
    the read path the subject: it takes a view first, then asks whether the
    transition left it valid. See "View and span lifetimes" in CONTRACT.md
    for what each view is owed.
    """

    NONE = "none"
    HELD_VALUE = "held_value"            # Bytes filled by get(), kept alive
    HELD_ITER_SPAN = "held_iter_span"    # span from iter_from(), held across
    HELD_RITER_SPAN = "held_riter_span"  # span from riter_from(), held across
    HELD_SNAPSHOT = "held_snapshot"      # db.snapshot() kept open
    SECOND_INSTANCE = "second_instance"  # a second DB open on this thread


OBSERVERS = list(Observer)

# Observers that read an existing key at acquire time.
_NEEDS_A_LIVE_KEY = {
    Observer.HELD_VALUE,
    Observer.HELD_ITER_SPAN,
    Observer.HELD_RITER_SPAN,
    Observer.HELD_SNAPSHOT,
}


# Plan shapes elected to carry observers, in preference order. Two questions
# are distinct to a held view, so each gets a representative:
#
#   disjoint  — the transition writes keys the observer is not looking at
#               (multi_put writes p0/p1, and emits the BulkBegin/BulkEnd pair
#               that #87 was found in)
#   colliding — the transition overwrites k0, the key every observer reads.
#               This is what makes a held Snapshot assert isolation rather
#               than mere survival, and what asserts that a superseded entry's
#               bytes stay put on an append-only file.
#
# sequential_overwrite is a single put targeting k0; causality_del_put is the
# multi-entry (del k0, put k0) fallback for classes that need 2+ operations.
_DISJOINT_REPS = ("multi_put",)
_COLLIDING_REPS = ("sequential_overwrite", "causality_del_put")


def _first_valid(
    labels: Tuple[str, ...], state: StateShape, failure: FailureClass
) -> Optional[PlanShape]:
    for label in labels:
        plan = next((p for p in PLAN_SHAPES if p.label == label), None)
        if plan is not None and is_valid_combination(state, plan, failure):
            return plan
    return None


def observer_plans_for(
    state: StateShape, failure: FailureClass
) -> List[PlanShape]:
    """The plan shapes that carry observers for this (state, failure).

    What a transition does to a lent view is decided by the failure class and
    the state shape — which file is written, whether it is mapped, whether
    resume() truncates it — plus whether the write set touches the key being
    observed. Everything else a plan shape varies (entry count, solo vs group
    commit, guard vocabulary) is invisible to a held view, so crossing all 17
    with every observer would multiply the matrix without asking a new
    question.
    """
    plans = [
        p
        for p in (
            _first_valid(_DISJOINT_REPS, state, failure),
            _first_valid(_COLLIDING_REPS, state, failure),
        )
        if p is not None
    ]
    if plans:
        return plans
    fallback = next(
        (p for p in PLAN_SHAPES if is_valid_combination(state, p, failure)), None
    )
    return [fallback] if fallback is not None else []


def is_valid_observer(
    observer: Observer,
    state: StateShape,
    plan: PlanShape,
    failure: FailureClass,
) -> bool:
    """Returns True if this observer is worth crossing with this cell.

    A full cross product is not affordable, so an observer is crossed only
    where the transition can actually disturb a view: the failure degrades
    (so the cell reaches resume(), which truncates the active file under a
    live reader), or the state maps that file. Class A returns before any
    I/O and cannot invalidate anything. Within that, one representative plan
    shapes carry the observers, one disjoint from the observed key and
    one colliding with it — see observer_plans_for.
    """
    if observer == Observer.NONE:
        return True
    if failure == FailureClass.A:
        return False
    if not (failure in DEGRADING_CLASSES or state.use_mmap):
        return False
    if plan not in observer_plans_for(state, failure):
        return False
    if observer in _NEEDS_A_LIVE_KEY:
        # Nothing to observe without a key that survives setup.
        return state.num_keys > 0 and not state.delete_after_create
    return True


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
    Tuple[StateShape, PlanShape, FailureClass, Observer], None, None
]:
    """Yields all valid (StateShape, PlanShape, FailureClass, Observer) cells."""
    for state in STATE_SHAPES:
        for plan in PLAN_SHAPES:
            for failure in FAILURE_CLASSES:
                if not is_valid_combination(state, plan, failure):
                    continue
                for observer in OBSERVERS:
                    if is_valid_observer(observer, state, plan, failure):
                        yield (state, plan, failure, observer)
