# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for vacuum_compact correctness proof.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, List, Tuple


@dataclass(frozen=True)
class CompactStateShape:
    label: str
    sealed_keys: List[str]
    deleted_keys: List[str]
    max_file_bytes: int

    @property
    def live_keys(self) -> List[str]:
        return [k for k in self.sealed_keys if k not in self.deleted_keys]


class VacuumCompactFailureClass(Enum):
    SUCCESS = "success"
    VC1 = "tmp_create_fails"  # io_vacuum_compact_tmp_create
    VC2 = "append_fails"      # io_data_file_append during scan/copy to tmp
    VC3 = "sync_fails"        # io_data_file_sync on tmp file
    VC4 = "rename_fails"      # io_vacuum_compact_rename (synced tmp on disk)


# Same state shapes as absorb but VacuumOptions::absorb_threshold=0 forces compact.
# For mostly_dead, max_file_bytes=150 packs all 6 keys into file_0 before
# rotation (6×25B=150B; del k1 at 150B triggers rotation), ensuring exactly
# one sealed file with live_bytes>0 so absorb_threshold=0 forces compact.
COMPACT_STATE_SHAPES = [
    CompactStateShape(
        "low_fragmentation",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k1"],
        max_file_bytes=50,
    ),
    CompactStateShape(
        "mostly_dead",
        sealed_keys=["k0", "k1", "k2", "k3", "k4", "k5"],
        deleted_keys=["k1", "k2", "k3", "k4", "k5"],
        max_file_bytes=150,
    ),
]

VACUUM_COMPACT_FAILURE_CLASSES = list(VacuumCompactFailureClass)


def generate_matrix() -> Generator[
    Tuple[CompactStateShape, VacuumCompactFailureClass], None, None
]:
    for state in COMPACT_STATE_SHAPES:
        for failure in VACUUM_COMPACT_FAILURE_CLASSES:
            yield (state, failure)
