# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for replication proof tests: state shapes, ops shapes,
# failure classes, and the validity filter.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, Optional, Tuple


# ---- State shapes -----------------------------------------------------------
# The leader workload that produces the state to replicate.


@dataclass(frozen=True)
class StateShape:
    label: str
    max_file_bytes: Optional[int] = None
    has_batches: bool = False
    needs_vacuum: bool = False
    has_nosync: bool = False

    @property
    def triggers_rotation(self) -> bool:
        return self.max_file_bytes is not None


STATE_SHAPES = [
    StateShape("single_key"),
    StateShape("multi_key"),
    StateShape("overwrites"),
    StateShape("deletes"),
    StateShape("range_deletes"),
    StateShape("batches", has_batches=True),
    StateShape("multi_file", max_file_bytes=256),
    StateShape("mixed_sync_nosync", has_nosync=True),
    StateShape("nosync_only", max_file_bytes=256, has_nosync=True),
    StateShape("nosync_then_sync", has_nosync=True),
    StateShape("vacuumed_batches", max_file_bytes=256, has_batches=True, needs_vacuum=True),
]


# ---- Ops shapes -------------------------------------------------------------
# How entries are delivered to the follower.


class OpsShape(Enum):
    FULL_STREAM = "full_stream"
    INCREMENTAL = "incremental"
    RESTART_MIDSTREAM = "restart_midstream"
    DUPLICATE_DELIVERY = "duplicate_delivery"
    PLANNED_PROMOTION = "planned_promotion"


OPS_SHAPES = list(OpsShape)


# ---- Ingest failure classes -------------------------------------------------


class IngestFailureClass(Enum):
    SUCCESS = "success"
    I_B1 = "append_fails_nothing_written"
    I_B2 = "append_fails_partial_write"
    I_F = "sync_fails"
    I_C = "crash_mid_batch"
    I_G = "rotation_sync_fails"
    I_H = "rotation_file_creation_fails"


INGEST_FAILURE_CLASSES = list(IngestFailureClass)

# Failure classes that only apply to states with batch markers.
BATCH_ONLY_FAILURES = {IngestFailureClass.I_C}

# Failure classes that only apply to states that trigger file rotation.
ROTATION_ONLY_FAILURES = {IngestFailureClass.I_G, IngestFailureClass.I_H}


# ---- Manifest failure classes -----------------------------------------------


class ManifestFailureClass(Enum):
    SUCCESS = "success"
    M_R = "rotation_fails"
    M_H = "hint_gen_fails"


MANIFEST_FAILURE_CLASSES = list(ManifestFailureClass)


# ---- Validity filters -------------------------------------------------------


def is_valid_ingest_combination(
    state: StateShape, ops: OpsShape, failure: IngestFailureClass
) -> bool:
    """Returns True if this (state, ops, failure) triple is a valid test."""
    # duplicate_delivery skips entries before I/O — only SUCCESS applies.
    if ops == OpsShape.DUPLICATE_DELIVERY and failure != IngestFailureClass.SUCCESS:
        return False
    # planned_promotion is a flag flip — only SUCCESS applies.
    if ops == OpsShape.PLANNED_PROMOTION and failure != IngestFailureClass.SUCCESS:
        return False
    # I_C requires batch markers (BulkBegin/BulkEnd) to orphan.
    if failure in BATCH_ONLY_FAILURES and not state.has_batches:
        return False
    # I_G / I_H require rotation to trigger.
    if failure in ROTATION_ONLY_FAILURES and not state.triggers_rotation:
        return False
    return True


# ---- Matrix generators ------------------------------------------------------


def generate_ingest_matrix() -> Generator[
    Tuple[StateShape, OpsShape, IngestFailureClass], None, None
]:
    """Yields all valid (StateShape, OpsShape, IngestFailureClass) triples."""
    for state in STATE_SHAPES:
        for ops in OPS_SHAPES:
            for failure in INGEST_FAILURE_CLASSES:
                if is_valid_ingest_combination(state, ops, failure):
                    yield (state, ops, failure)


def generate_manifest_matrix() -> Generator[
    Tuple[StateShape, ManifestFailureClass], None, None
]:
    """Yields all valid (StateShape, ManifestFailureClass) pairs."""
    for state in STATE_SHAPES:
        for failure in MANIFEST_FAILURE_CLASSES:
            yield (state, failure)
