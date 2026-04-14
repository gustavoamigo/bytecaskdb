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
        return Delta([], [], 0, degraded=False, threw=True)

    if failure == FailureClass.C:
        return Delta([], [], 0, degraded=True, threw=True)

    if failure == FailureClass.B2:
        return Delta([], [], 0, degraded=plan.is_single_entry, threw=True)

    if failure == FailureClass.B3:
        if plan.is_single_entry:
            # BC-156: read-back confirms CRC valid → publish normally, no throw.
            return _full_delta(plan, key_labels, existing_keys)
        # Multi-entry B3: batch framing discards the orphaned entry; no degrade.
        return Delta([], [], 0, degraded=False, threw=True)

    if failure in (FailureClass.D, FailureClass.E):
        return Delta([], [], 0, degraded=True, threw=True)

    if failure == FailureClass.F:
        # Sync failed after writev — bytes in page cache but not confirmed
        # durable. Key changes are NOT published in-session (BC-155). LSN
        # still advances to prevent reuse of sequence numbers now on disk.
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=False, threw=True)

    if failure == FailureClass.G:
        # Rotation sync failed — same contract as F: key changes unpublished,
        # LSN advances past consumed values.
        n = plan.write_count
        return Delta([], [], n + (2 if n > 1 else 0), degraded=False, threw=True)

    if failure == FailureClass.H:
        return _full_delta(
            plan, key_labels, existing_keys, degraded=True, threw=True
        )

    raise ValueError(f"Unhandled failure class: {failure}")
