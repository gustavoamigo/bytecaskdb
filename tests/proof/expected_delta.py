# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model: pure function that computes the expected delta for a
# (PlanShape, FailureClass) pair. Independent of the C++ implementation.

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from .scenario_matrix import FailureClass, OpType, PlanShape


@dataclass(frozen=True)
class Delta:
    keys_added: List[str]
    keys_removed: List[str]
    lsn_advance: int
    degraded: bool
    threw: bool


def _full_delta(
    plan: PlanShape,
    key_labels: List[str],
    existing_keys: List[str],
    degraded: bool = False,
    threw: bool = False,
) -> Delta:
    """Compute delta for a fully-persisted transition."""
    n = plan.write_count
    lsn_advance = n + (2 if n > 1 else 0)

    keys_added = []
    keys_removed = []
    for op, label in zip(plan.ops, key_labels):
        if op == OpType.PUT:
            keys_added.append(label)
        elif op == OpType.DELETE:
            if label in existing_keys:
                keys_removed.append(label)

    return Delta(
        keys_added=keys_added,
        keys_removed=keys_removed,
        lsn_advance=lsn_advance,
        degraded=degraded,
        threw=threw,
    )


def expected_delta(
    plan: PlanShape,
    failure: FailureClass,
    key_labels: List[str],
    existing_keys: List[str],
) -> Delta:
    """
    The reference model. Computes what transition(S1, P, F) must produce.

    key_labels:    the key name assigned to each op in the plan
    existing_keys: which of those keys exist in the pre-transition state
    """
    if failure == FailureClass.SUCCESS:
        return _full_delta(plan, key_labels, existing_keys)

    if failure == FailureClass.A:
        return Delta([], [], 0, degraded=False, threw=False)

    if failure == FailureClass.B1:
        # Advance conservatively: the engine cannot determine from userspace
        # whether bytes reached disk (POSIX does not guarantee writev=-1 means
        # no bytes written). Gaps are safe; reuse is not.
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=True, threw=True)

    if failure == FailureClass.C:
        # Append failed mid-batch (on BulkEnd). Prior entries (BulkBegin, data
        # entries) may be on disk. Advance conservatively past all consumed LSNs.
        # resume() truncates the orphaned batch and sets next_lsn from disk.
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=True, threw=True)

    if failure == FailureClass.B2:
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=True, threw=True)

    if failure == FailureClass.B3:
        # Any append error — including a full write that returned an error —
        # degrades unconditionally. resume() replays valid on-disk entries.
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=True, threw=True)

    if failure == FailureClass.F:
        # Commit fdatasync failed — bytes in page cache but not confirmed
        # durable. Key changes are NOT published in-session. LSN advances to
        # prevent reuse. Engine degrades (BC-164); resume() restores writes.
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=True, threw=True)

    if failure == FailureClass.G:
        # Rotation fdatasync failed — same contract as F: bytes in page cache,
        # key changes unpublished, LSN advances, engine degrades (BC-164).
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=True, threw=True)

    if failure == FailureClass.H:
        return _full_delta(
            plan, key_labels, existing_keys, degraded=True, threw=True
        )

    raise ValueError(f"Unhandled failure class: {failure}")
