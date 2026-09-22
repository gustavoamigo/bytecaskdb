# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for the group-commit correctness proof.
#
# Every other matrix in this framework drives one thread. Group commit is the
# write path's defining mechanism — a writer that finds no leader becomes one
# and executes every pending write under a single lock hold with a single
# fdatasync — and no generated cell faulted it with more than one thread in
# play.
#
# #118 proposed tagging each slot with its own fault, on the premise that a
# follower's I/O could be failed while the leader's succeeded. The write path
# does not work that way: execute_slot is pure in-memory (it contains no
# append or sync call at all), and execute_slots issues ONE append_entries for
# every slot's entries combined and ONE fdatasync. There is no per-slot I/O
# boundary to fail, so "fail writer B's slot while A leads" is not a state the
# engine can be in.
#
# What is left is group-wide, and the thread-local injector reaches it exactly
# as it stands: the leader is the thread that performs all of the group's I/O,
# so a fault armed on the leader's thread fires for the whole group. The
# question these cells ask is the one that actually matters — when the group's
# I/O fails, does every writer in it learn, and does the group land all or
# nothing?

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, Optional, Tuple


class GroupFailureClass(Enum):
    SUCCESS = "success"
    B1 = "group_append_fails"
    B2 = "group_append_partial_write"
    F = "group_commit_sync_fails"
    G = "group_rotation_sync_fails"
    H = "group_rotation_file_creation_fails"


@dataclass(frozen=True)
class GroupShape:
    label: str
    # Writers in the batch, the leader included. The leader blocks in
    # on_leader_start_ until this many slots are queued, so the batch shape is
    # deterministic rather than raced for.
    group_size: int
    # Each writer submits a 2-op plan, so the group's combined append carries a
    # BulkBegin/BulkEnd pair per writer and the all-or-nothing question is
    # asked of batches rather than single entries.
    multi_op: bool = False
    max_file_bytes: Optional[int] = None

    @property
    def at_rotation(self) -> bool:
        return self.max_file_bytes is not None


GROUP_SHAPES = [
    GroupShape("group_of_2", group_size=2),
    GroupShape("group_of_4", group_size=4),
    GroupShape("group_of_2_batched", group_size=2, multi_op=True),
    GroupShape("group_of_4_batched", group_size=4, multi_op=True),
    GroupShape("group_of_2_rotation", group_size=2, max_file_bytes=1),
    GroupShape("group_of_4_rotation", group_size=4, max_file_bytes=1),
]

GROUP_FAILURE_CLASSES = list(GroupFailureClass)

ROTATION_ONLY = {GroupFailureClass.G, GroupFailureClass.H}


def is_valid_combination(shape: GroupShape, failure: GroupFailureClass) -> bool:
    # G and H fire during the post-write rotation, which only happens when the
    # active file crosses the threshold.
    if failure in ROTATION_ONLY and not shape.at_rotation:
        return False
    # The rotation shapes exist for G and H. Crossing them with the append and
    # commit-sync classes would re-ask what the non-rotation shapes already
    # ask, against a fault point that fires at a different call in the
    # sequence — resolving which needs count-based injection that says nothing
    # new about grouping.
    if shape.at_rotation and failure in (
        GroupFailureClass.B1,
        GroupFailureClass.B2,
        GroupFailureClass.F,
    ):
        return False
    return True


def generate_matrix() -> Generator[Tuple[GroupShape, GroupFailureClass], None, None]:
    for shape in GROUP_SHAPES:
        for failure in GROUP_FAILURE_CLASSES:
            if is_valid_combination(shape, failure):
                yield (shape, failure)
