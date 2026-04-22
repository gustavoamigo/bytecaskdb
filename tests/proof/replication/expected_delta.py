# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model: pure functions that compute the expected outcome for
# each (StateShape, OpsShape, IngestFailureClass) triple. Independent
# of the C++ implementation.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import (
    IngestFailureClass,
    ManifestFailureClass,
    OpsShape,
    StateShape,
)


@dataclass(frozen=True)
class IngestDelta:
    keys_match: bool       # follower key_dir == leader key_dir
    degraded: bool         # follower engine is degraded
    threw: bool            # ingest threw an exception
    partial_committed: bool  # some entries committed before failure (I_H)


@dataclass(frozen=True)
class ManifestDelta:
    manifest_produced: bool
    threw: bool
    degraded: bool  # engine should be degraded after failure


def ingest_expected(
    state: StateShape,
    ops: OpsShape,
    failure: IngestFailureClass,
) -> IngestDelta:
    """Compute the expected outcome of an ingest transition."""
    if failure == IngestFailureClass.SUCCESS:
        return IngestDelta(
            keys_match=True, degraded=False, threw=False, partial_committed=False
        )

    if ops == OpsShape.DUPLICATE_DELIVERY:
        # Duplicates are skipped before I/O — always SUCCESS.
        return IngestDelta(
            keys_match=True, degraded=False, threw=False, partial_committed=False
        )

    if ops == OpsShape.PLANNED_PROMOTION:
        # Mode switch is a flag flip — always SUCCESS.
        return IngestDelta(
            keys_match=True, degraded=False, threw=False, partial_committed=False
        )

    # I_H: rotation file creation fails AFTER sync succeeded.
    # The chunk that triggered rotation is durable. Partial prefix committed.
    if failure == IngestFailureClass.I_H:
        return IngestDelta(
            keys_match=False, degraded=True, threw=True, partial_committed=True
        )

    # I_G: rotation sync fails. Chunk in page cache, durability unconfirmed.
    # I_B1/I_B2/I_F/I_C: various write/sync failures.
    # All: no entries published, engine degrades.
    return IngestDelta(
        keys_match=False, degraded=True, threw=True, partial_committed=False
    )


def manifest_expected(failure: ManifestFailureClass) -> ManifestDelta:
    """Compute the expected outcome of a create_manifest call."""
    if failure == ManifestFailureClass.SUCCESS:
        return ManifestDelta(manifest_produced=True, threw=False, degraded=False)
    # M_R: rotation file creation fails after seal — engine must degrade.
    if failure == ManifestFailureClass.M_R:
        return ManifestDelta(manifest_produced=False, threw=True, degraded=True)
    # M_H: pre-rotation sync fails — active file not sealed, no degradation.
    return ManifestDelta(manifest_produced=False, threw=True, degraded=False)
