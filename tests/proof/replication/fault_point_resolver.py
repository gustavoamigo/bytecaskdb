# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps IngestFailureClass / ManifestFailureClass to ScopedFaultInjector
# configuration for C++ code generation.

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from .scenario_matrix import IngestFailureClass, ManifestFailureClass, OpsShape


@dataclass(frozen=True)
class FaultConfig:
    name: Optional[str] = None
    fail_at: Optional[int] = None
    post_write_mode: Optional[str] = None
    short_write_bytes: int = 5
    skip_names: tuple = ()

    @property
    def is_noop(self) -> bool:
        return self.name is None and self.fail_at is None


def resolve_ingest_fault(
    failure: IngestFailureClass,
    ops: OpsShape,
) -> FaultConfig:
    """Return the ScopedFaultInjector config for an ingest failure class.

    For INCREMENTAL/RESTART_MIDSTREAM ops, the fault targets only the
    final chunk — the generator wraps only the last ingest call with the
    ScopedFaultInjector block.
    """
    if failure == IngestFailureClass.SUCCESS:
        return FaultConfig()

    if failure == IngestFailureClass.I_B1:
        return FaultConfig(name="io_data_file_append")

    if failure == IngestFailureClass.I_B2:
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode="short_write",
            short_write_bytes=5,
        )

    if failure == IngestFailureClass.I_F:
        return FaultConfig(name="io_data_file_sync")

    if failure == IngestFailureClass.I_C:
        # Count-based: fail on the 2nd io_data_file_append checkpoint.
        # In a batch ingest: BulkBegin(append#1), entries(append#2..N),
        # BulkEnd(append#N+1). Failing at checkpoint 2 leaves an orphaned
        # BulkBegin.
        return FaultConfig(fail_at=2)

    if failure == IngestFailureClass.I_G:
        # Rotation sync: fires on io_data_file_sync during rotation.
        # For rotation states, the rotation sync is the first sync checkpoint.
        return FaultConfig(name="io_data_file_sync")

    if failure == IngestFailureClass.I_H:
        return FaultConfig(name="io_rotate_file_creation")

    raise ValueError(f"Unhandled failure class: {failure}")


def resolve_manifest_fault(failure: ManifestFailureClass) -> FaultConfig:
    """Return the ScopedFaultInjector config for a manifest failure class."""
    if failure == ManifestFailureClass.SUCCESS:
        return FaultConfig()

    if failure == ManifestFailureClass.M_R:
        return FaultConfig(name="io_rotate_file_creation")

    if failure == ManifestFailureClass.M_H:
        # Hint file sync goes through io_data_file_sync on the hint DataFile.
        # The manifest waits for hint generation; faulting the sync path
        # causes create_manifest to throw during worker_.drain().
        return FaultConfig(name="io_data_file_sync")

    raise ValueError(f"Unhandled manifest failure class: {failure}")
