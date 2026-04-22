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
    expected_values: dict  # key -> value string (for value verification)
    seq_advance: int
    degraded: bool
    threw: bool


def _full_delta(
    plan: PlanShape,
    key_labels: List[str],
    existing_keys: List[str],
    degraded: bool = False,
    threw: bool = False,
) -> Delta:
    """Compute delta for a fully-persisted transition.

    Tracks the net effect per key: when multiple operations target the same
    key (causality shapes), only the final operation determines the outcome.
    """
    n = plan.write_count
    seq_advance = n + (2 if n > 1 else 0)

    # Track final state per key: ("put", value_index) or "deleted".
    final_state: dict = {}
    for i, (op, label) in enumerate(zip(plan.ops, key_labels)):
        if op == OpType.PUT:
            final_state[label] = ("put", i)
        elif op == OpType.DELETE:
            final_state[label] = "deleted"

    keys_added = [k for k, s in final_state.items() if s != "deleted"]
    keys_removed = [k for k, s in final_state.items() if s == "deleted"]
    expected_values = {
        k: f"new{s[1]}" for k, s in final_state.items() if s != "deleted"
    }

    return Delta(
        keys_added=keys_added,
        keys_removed=keys_removed,
        expected_values=expected_values,
        seq_advance=seq_advance,
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
        return Delta([], [], {}, 0, degraded=False, threw=False)

    if failure == FailureClass.B1:
        # Append failed before or after bytes hit disk — engine cannot tell.
        # next_seq stays at pre-failure value; resume() re-derives from disk.
        return Delta([], [], {}, 0, degraded=True, threw=True)

    if failure == FailureClass.C:
        # Append failed mid-batch (on BulkEnd). Prior entries (BulkBegin, data
        # entries) may be on disk. next_seq stays at pre-failure value;
        # resume() truncates the orphaned batch and sets next_seq from disk.
        return Delta([], [], {}, 0, degraded=True, threw=True)

    if failure == FailureClass.B2:
        return Delta([], [], {}, 0, degraded=True, threw=True)

    if failure == FailureClass.B3:
        # Any append error — including a full write that returned an error —
        # degrades unconditionally. resume() replays valid on-disk entries.
        return Delta([], [], {}, 0, degraded=True, threw=True)

    if failure == FailureClass.F:
        # Commit fdatasync failed — bytes in page cache but not confirmed
        # durable. Key changes are NOT published in-session. next_seq stays
        # at pre-failure value; resume() re-derives from disk.
        return Delta([], [], {}, 0, degraded=True, threw=True)

    if failure == FailureClass.G:
        # Rotation fdatasync failed — same contract as F: bytes in page cache,
        # key changes unpublished, next_seq stays at pre-failure value.
        return Delta([], [], {}, 0, degraded=True, threw=True)

    if failure == FailureClass.H:
        return _full_delta(
            plan, key_labels, existing_keys, degraded=True, threw=True
        )

    raise ValueError(f"Unhandled failure class: {failure}")
