# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for vacuum_absorb correctness proof.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, List, Tuple


@dataclass(frozen=True)
class AbsorbStateShape:
    label: str
    # Keys written to the sealed file (file_0).
    sealed_keys: List[str]
    # Keys deleted after sealing (in active file as tombstones).
    deleted_keys: List[str]
    # max_file_bytes to use when opening the DB — controls rotation timing.
    max_file_bytes: int

    @property
    def live_keys(self) -> List[str]:
        return [k for k in self.sealed_keys if k not in self.deleted_keys]


class VacuumAbsorbFailureClass(Enum):
    SUCCESS = "success"
    VA1 = "append_fails"   # io_data_file_append during scan/copy
    VA2 = "sync_fails"     # io_vacuum_absorb_sync after copy


# max_file_bytes=50: fits exactly 2 entries before rotation.
# k0 written (25B), k1 written (50B), del k1 triggers rotation (50>=50):
# file_0 sealed with {k0, k1}, file_1 becomes active with k1 tombstone.
#
# max_file_bytes=160: fits all 6 entries (150B < 160B). k0..k5 written,
# rotation on first delete (150+21=171>=160): file_0 sealed with {k0..k5},
# file_1 active with tombstones. Only one sealed file — the vacuum target
# always has live_bytes > 0 (k0 survives).
#
# all_dead: max_file_bytes=50 fits 2 entries. k0, k1 written (50B), both
# deleted: file_0 sealed with {k0, k1}, live_bytes = 0.
ABSORB_STATE_SHAPES = [
    AbsorbStateShape(
        "low_fragmentation",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k1"],
        max_file_bytes=50,
    ),
    AbsorbStateShape(
        "mostly_dead",
        sealed_keys=["k0", "k1", "k2", "k3", "k4", "k5"],
        deleted_keys=["k1", "k2", "k3", "k4", "k5"],
        max_file_bytes=160,
    ),
    AbsorbStateShape(
        "all_dead",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k0", "k1"],
        max_file_bytes=50,
    ),
]

VACUUM_ABSORB_FAILURE_CLASSES = list(VacuumAbsorbFailureClass)


def generate_matrix() -> Generator[
    Tuple[AbsorbStateShape, VacuumAbsorbFailureClass], None, None
]:
    for state in ABSORB_STATE_SHAPES:
        for failure in VACUUM_ABSORB_FAILURE_CLASSES:
            yield (state, failure)
