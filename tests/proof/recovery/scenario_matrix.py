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
#
# The read side is one fault point, io_hint_read, over two phases of a hint
# file's life, and the phase decides what the open does: a read that fails
# while a hint is verified is answered by a rebuild from its data file, so the
# open succeeds; one that fails while a hint that verified is scanned comes
# after entries were applied, so the open throws. The two phases are two
# failure classes, and they run at one thread only: the injector is
# thread-local and at one thread recovery reads on the thread that opens.
# Arming it across recovery's workers, and with that the parallel half of
# this answer, is #599.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, Optional, Tuple


class RecoveryFailureClass(Enum):
    SUCCESS = "success"
    RC1 = "hint_write_fails"    # io_hint_write
    RC2 = "hint_sync_fails"     # io_hint_sync
    RC3 = "hint_rename_fails"   # io_hint_rename
    # io_hint_read, one per phase a hint read lands in: the expected open is
    # an outcome of the phase, not of the fault point.
    RC4 = "hint_read_fails_while_verified"  # the pass that opens a hint
    RC5 = "hint_read_fails_during_scan"     # the scan of a hint that verified


class Damage(Enum):
    # The shape a crash produces: the file active when the process died has
    # no hint, and recovery_prepare_files regenerates it. A clean close
    # writes that hint, so the test removes it.
    DROP_NEWEST_HINT = "drop_newest_hint"
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
    RecoveryStateShape("crash_hintless", Damage.DROP_NEWEST_HINT),
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
    RecoveryStateShape("hintless_batched", Damage.DROP_NEWEST_HINT, has_batches=True),
    # A range tombstone has to survive it too — recovery's suppression loop
    # reads it back out of the regenerated hint.
    RecoveryStateShape("hintless_range_del", Damage.DROP_NEWEST_HINT, has_range_del=True),
]

RECOVERY_FAILURE_CLASSES = list(RecoveryFailureClass)

# The classes that fail a read of a hint file. They run at one thread only.
READ_FAILURE_CLASSES = {
    RecoveryFailureClass.RC4,
    RecoveryFailureClass.RC5,
}

# Serial and parallel recovery diverging is the specific risk the [model]
# tests were built around, so every shape is recovered both ways.
RECOVERY_THREAD_COUNTS = [1, 4]


def is_valid_combination(
    state: RecoveryStateShape, failure: RecoveryFailureClass, threads: int
) -> bool:
    # A hint that is present and valid is never regenerated, so there is no
    # hint write to fault. Every shape here damages something, so all the
    # write-failure combinations are reachable. The read failures are the
    # exception, and only for a shape with more than one data file: recovery's
    # workers are min(files, threads) and one of them runs on the thread that
    # opens, so a one-file shape reads where the injector — thread-local — can
    # see it whatever the thread count, while a multi-file one at threads > 1
    # reads on workers that have no injector. Arming it there is #599.
    if failure in READ_FAILURE_CLASSES and threads != 1:
        return state.max_file_bytes is None  # the writes fit one data file
    return True


def generate_matrix() -> Generator[
    Tuple[RecoveryStateShape, RecoveryFailureClass, int], None, None
]:
    for state in RECOVERY_STATE_SHAPES:
        for failure in RECOVERY_FAILURE_CLASSES:
            for threads in RECOVERY_THREAD_COUNTS:
                if is_valid_combination(state, failure, threads):
                    yield (state, failure, threads)
