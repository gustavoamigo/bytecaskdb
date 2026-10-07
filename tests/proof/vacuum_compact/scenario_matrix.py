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
    # Keys overwritten with sync = false once the baseline is durable (#245).
    # Their entries in the sealed file are dead only by writes a power cut can
    # lose, so vacuum must make those writes durable before it drops them.
    overwritten_keys: Tuple[str, ...] = ()

    @property
    def live_keys(self) -> List[str]:
        return [
            k
            for k in self.sealed_keys
            if k not in self.deleted_keys and k not in self.overwritten_keys
        ]

    @property
    def has_unsynced_superseder(self) -> bool:
        return bool(self.overwritten_keys)

    # No live entry and no tombstone: vacuum drops the file whole, with no
    # staging copy (vacuum_remove_file).
    @property
    def whole_file(self) -> bool:
        return not self.live_keys


class VacuumCompactFailureClass(Enum):
    SUCCESS = "success"
    VC1 = "tmp_create_fails"  # io_vacuum_compact_tmp_create
    VC2 = "append_fails"      # io_data_file_append during scan/copy to tmp
    VC3 = "sync_fails"        # io_data_file_sync on tmp file
    VC4 = "rename_fails"      # io_vacuum_compact_rename (synced tmp on disk)
    VC5 = "unlink_fails"      # io_vacuum_compact_unlink (committed; source left on disk)
    # The other end of VC5's window (#104 M3): the rename completed and the
    # process did not get to confirm it. Nothing is committed, so the copy on
    # disk is an orphan the published state does not reference.
    VC6 = "post_rename"       # io_vacuum_compact_post_rename (renamed; not committed)
    # The fdatasync vacuum_commit issues before it drops entries superseded
    # only by sync = false writes (#261): io_data_file_sync, the second one on
    # the compaction path (the first is the staging copy's), the first on the
    # whole-file path. Vacuum degrades and throws without committing.
    VC7 = "durability_sync_fails"
    # The staging copy's shrink_to_fit, between its sync and the rename
    # (#258): io_vacuum_compact_shrink.
    VC8 = "shrink_fails"
    # renameDataFileExclusive's fallback, for a filesystem without
    # RENAME_NOREPLACE (#258): renameat2 fails with EINVAL, link() places the
    # copy, and the unlink() of the staged name fails. The function takes the
    # placed name back before it throws, so nothing is left. Injected below
    # the engine (ScopedSyscallFaults), so Linux only.
    VC9 = "fallback_unlink_fails"
    # The same under a fault that fails every unlink and remove in the
    # directory: the take-back and vacuum's cleanup fail too, both names stay,
    # and the next open removes them.
    VC10 = "fallback_unlink_persistent"


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
    # #245: the sealed file's dead entries are superseded only by sync = false
    # writes. Two 25-byte puts fill max_file_bytes = 50 and seal file_0; the
    # 23-byte overwrites (2-byte values) land in the next file and stay under
    # the limit, so nothing but vacuum syncs them — a rotation would, and did
    # when the values were 4 bytes. Deleting instead would hide the bug: a
    # lost delete and a dropped Put leave the state the history ends in.
    CompactStateShape(
        "unsynced_overwrite",
        sealed_keys=["k0", "k1"],
        deleted_keys=[],
        max_file_bytes=50,
        overwritten_keys=("k1",),
    ),
    # Every key overwritten: no live entry and no tombstone, so vacuum drops
    # the file whole, without a staging copy — the other path through
    # vacuum_commit, and the one #245 was found on.
    CompactStateShape(
        "unsynced_overwrite_whole_file",
        sealed_keys=["k0", "k1"],
        deleted_keys=[],
        max_file_bytes=50,
        overwritten_keys=("k0", "k1"),
    ),
]

VACUUM_COMPACT_FAILURE_CLASSES = list(VacuumCompactFailureClass)


def is_valid_combination(
    state: CompactStateShape, failure: VacuumCompactFailureClass
) -> bool:
    # vacuum_commit syncs only when the state it judged the file by holds
    # writes no fdatasync has covered.
    if failure == VacuumCompactFailureClass.VC7:
        return state.has_unsynced_superseder
    # The whole-file path writes no staging copy, so it never makes the calls
    # VC1-VC4, VC6 and VC8-VC10 fault. It commits and unlinks, so VC5 applies.
    if state.whole_file:
        return failure in (
            VacuumCompactFailureClass.SUCCESS,
            VacuumCompactFailureClass.VC5,
        )
    return True


def generate_matrix() -> Generator[
    Tuple[CompactStateShape, VacuumCompactFailureClass], None, None
]:
    for state in COMPACT_STATE_SHAPES:
        for failure in VACUUM_COMPACT_FAILURE_CLASSES:
            if is_valid_combination(state, failure):
                yield (state, failure)
