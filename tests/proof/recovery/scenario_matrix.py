# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for the recovery / hint-generation correctness proof.
#
# hint_file.cppm had 19 syscall sites and no fault points, and DB::open had no
# matrix at all — recovery was proven by the [model] tests, every one of which
# runs the clean path. The README sells the hint write as a crash-safety
# mechanism ("write -> fdatasync -> rename") and nothing injected a failure
# into any of the three steps.
#
# What makes this cheap to assert is that the expected delta is the same in
# every cell. A hint file is a rebuildable index, not the record: a failure
# generating one may cost recovery time and must not cost keys. So each cell
# asks the same question of a different shape — does every key survive?
#
# Scope: the four synchronous flush_hints_for call sites are reachable, since
# the fault is armed on the thread that calls DB::open. The two post-rotation
# worker_.dispatch sites are not — active_injector is thread_local and the
# background worker has its own. That is recorded in the docs rather than
# worked around: a fault armed here would have to outlive the stack frame that
# set it, and the failure it models (a missing hint) is already the state the
# hintless_* shapes below start from.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, Optional, Tuple


class RecoveryFailureClass(Enum):
    SUCCESS = "success"
    RC1 = "hint_write_fails"    # io_hint_write
    RC2 = "hint_sync_fails"     # io_hint_sync
    RC3 = "hint_rename_fails"   # io_hint_rename


class Damage(Enum):
    # No damage needed. A clean close leaves the file that was active at
    # shutdown hint-less all by itself — flush_hints skips active_file_id — so
    # this is already the shape a crash produces, and the one
    # recovery_prepare_files regenerates from.
    NONE = "none"
    DROP_ALL_HINTS = "drop_all_hints"             # every sealed file's hint gone
    CORRUPT_NEWEST_HINT = "corrupt_newest_hint"   # newest hint's CRC no longer holds


@dataclass(frozen=True)
class RecoveryStateShape:
    label: str
    damage: Damage
    num_keys: int = 6
    max_file_bytes: Optional[int] = None
    has_batches: bool = False
    has_range_del: bool = False


RECOVERY_STATE_SHAPES = [
    # The shape a crash leaves behind: the file that was active at shutdown
    # never got a hint, and recovery_prepare_files regenerates it (and drops
    # its preallocated tail).
    RecoveryStateShape("crash_hintless", Damage.NONE),
    # Several sealed files, none with a hint — every one has to be rebuilt.
    RecoveryStateShape(
        "multi_file_hintless", Damage.DROP_ALL_HINTS, max_file_bytes=120
    ),
    # A hint that fails its CRC. open_hint_or_rebuild must discard it and
    # rebuild from the data file rather than drop the keys behind it.
    RecoveryStateShape(
        "damaged_hint", Damage.CORRUPT_NEWEST_HINT, max_file_bytes=120
    ),
    # Batch markers have to survive regeneration: hint files carry BulkBegin
    # and BulkEnd so recovery can compute durable_seq over a batch.
    RecoveryStateShape("hintless_batched", Damage.NONE, has_batches=True),
    # A range tombstone has to survive it too — recovery's suppression loop
    # reads it back out of the regenerated hint.
    RecoveryStateShape("hintless_range_del", Damage.NONE, has_range_del=True),
]

RECOVERY_FAILURE_CLASSES = list(RecoveryFailureClass)

# Serial and parallel recovery diverging is the specific risk the [model]
# tests were built around, so every shape is recovered both ways.
RECOVERY_THREAD_COUNTS = [1, 4]


def is_valid_combination(
    state: RecoveryStateShape, failure: RecoveryFailureClass, threads: int
) -> bool:
    # A hint that is present and valid is never regenerated, so there is no
    # hint write to fault. Every shape here damages something, so all
    # combinations are reachable.
    return True


def generate_matrix() -> Generator[
    Tuple[RecoveryStateShape, RecoveryFailureClass, int], None, None
]:
    for state in RECOVERY_STATE_SHAPES:
        for failure in RECOVERY_FAILURE_CLASSES:
            for threads in RECOVERY_THREAD_COUNTS:
                if is_valid_combination(state, failure, threads):
                    yield (state, failure, threads)
