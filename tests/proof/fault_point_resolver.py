# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps failure classes to ScopedFaultInjector C++ constructor parameters.

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

from .scenario_matrix import FailureClass, PlanShape, StateShape


@dataclass
class FaultConfig:
    """Describes how to construct ScopedFaultInjector in generated C++."""

    name: Optional[str] = None
    fail_at: Optional[int] = None
    skip_names: List[str] = field(default_factory=list)
    post_write_mode: Optional[str] = None  # "short_write" or "throw_after"
    short_write_bytes: int = 0
    use_sync_false: bool = False

    @property
    def is_noop(self) -> bool:
        return (
            self.name is None
            and self.fail_at is None
            and self.post_write_mode is None
        )


def resolve_fault(
    state: StateShape, plan: PlanShape, failure: FailureClass
) -> FaultConfig:
    """Maps (state, plan, failure_class) to ScopedFaultInjector params."""
    n = plan.write_count
    multi = plan.is_multi_entry
    n_appends = n + (2 if multi else 0)

    if failure in (FailureClass.SUCCESS, FailureClass.A):
        return FaultConfig()

    if failure == FailureClass.B1:
        return FaultConfig(name="io_data_file_append")

    if failure == FailureClass.B2:
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode="short_write",
            short_write_bytes=5,
        )

    if failure == FailureClass.B3:
        return FaultConfig(
            name="io_data_file_append_partial",
            post_write_mode="throw_after",
        )

    if failure == FailureClass.C:
        # Fail on BulkEnd — the last append checkpoint.
        return FaultConfig(fail_at=n_appends - 1)

    if failure == FailureClass.F:
        if state.at_rotation:
            # On rotation_threshold the rotation sync fires first at the same
            # checkpoint name. Use count-based to skip rotation + file creation
            # and hit only the commit sync.
            # Sequence: appends(1..n_appends), rotation_sync(+1),
            #           file_creation(+2), commit_sync(+3 — fails here).
            return FaultConfig(fail_at=n_appends + 2)
        return FaultConfig(name="io_data_file_sync")

    if failure == FailureClass.G:
        # Rotation sync fails. Use sync=false so the commit sync checkpoint
        # is never reached — the only io_data_file_sync is the rotation sync.
        return FaultConfig(name="io_data_file_sync", use_sync_false=True)

    if failure == FailureClass.H:
        return FaultConfig(name="io_rotate_file_creation")

    raise ValueError(f"Unhandled failure class: {failure}")
