# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for vacuum_compact correctness proof.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, List, Optional, Tuple


@dataclass(frozen=True)
class CompactStateShape:
    label: str
    sealed_keys: List[str]
    deleted_keys: List[str]
    max_file_bytes: int
    io_backend: str = "pread"  # pread | mmap | buffer_pool
    # Write sealed_keys as one atomic batch, so the sealed file being
    # compacted carries a BulkBegin/BulkEnd pair around its live entries.
    batched: bool = False
    # A range delete, written into the sealed file after sealed_keys, so the
    # file being compacted carries a range tombstone. The keys it covers are
    # named in deleted_keys.
    range_del: Optional[Tuple[str, str]] = None

    @property
    def live_keys(self) -> List[str]:
        return [k for k in self.sealed_keys if k not in self.deleted_keys]


class VacuumCompactFailureClass(Enum):
    SUCCESS = "success"
    VC1 = "tmp_create_fails"  # io_vacuum_compact_tmp_create
    VC2 = "append_fails"      # io_data_file_append during scan/copy to tmp
    VC3 = "sync_fails"        # io_data_file_sync on tmp file
    VC4 = "rename_fails"      # io_vacuum_compact_rename (synced tmp on disk)


# Compact path is now always used for files with live_bytes > 0.
# For mostly_dead, max_file_bytes=150 packs all 6 keys into file_0 before
# rotation (6×25B=150B; del k1 at 150B triggers rotation), ensuring exactly
# one sealed file with live_bytes>0.
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
    CompactStateShape(
        "low_fragmentation_mmap",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k1"],
        max_file_bytes=50,
        io_backend="mmap",
    ),
    CompactStateShape(
        "mostly_dead_mmap",
        sealed_keys=["k0", "k1", "k2", "k3", "k4", "k5"],
        deleted_keys=["k1", "k2", "k3", "k4", "k5"],
        max_file_bytes=150,
        io_backend="mmap",
    ),
    CompactStateShape(
        "low_fragmentation_pool",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k1"],
        max_file_bytes=50,
        io_backend="buffer_pool",
    ),
    CompactStateShape(
        "mostly_dead_pool",
        sealed_keys=["k0", "k1", "k2", "k3", "k4", "k5"],
        deleted_keys=["k1", "k2", "k3", "k4", "k5"],
        max_file_bytes=150,
        io_backend="buffer_pool",
    ),
    # The two structural shapes. Compaction has to preserve the batch markers
    # and the range tombstone it copies — they are not key data, so nothing in
    # the key/value assertions notices if a retried compaction drops one, and
    # recovery would not notice either. assert_vacuum_success compares the
    # entry stream, which is what makes these cells say something.
    #
    # Sizing: an entry is 19 + key + value bytes and a marker is 19. The batch
    # is 19 + 25 + 25 + 19 = 88, so max_file_bytes=88 seals file_0 with the
    # whole batch in it and sends the delete to the next file.
    CompactStateShape(
        "batched_file",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k1"],
        max_file_bytes=88,
        batched=True,
    ),
    # 25 + 25 for the two puts, plus 19 + 2 + 2 for the range tombstone = 73.
    CompactStateShape(
        "range_tombstone_file",
        sealed_keys=["k0", "k1"],
        deleted_keys=["k1"],
        max_file_bytes=73,
        range_del=("k1", "k2"),
    ),
]

VACUUM_COMPACT_FAILURE_CLASSES = list(VacuumCompactFailureClass)


def generate_matrix() -> Generator[
    Tuple[CompactStateShape, VacuumCompactFailureClass], None, None
]:
    for state in COMPACT_STATE_SHAPES:
        for failure in VACUUM_COMPACT_FAILURE_CLASSES:
            yield (state, failure)
