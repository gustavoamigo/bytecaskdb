# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Maps GroupFailureClass to ScopedFaultInjector parameters.
#
# Which thread performs the group's I/O is not one answer, and the difference
# decides where a fault has to be armed. active_injector is thread_local.
#
#   append (B1, B2)          — the leader, inside execute_slots under
#                              write_mu_. Followers never append.
#   rotation sync/create     — the leader, inline: the rotation branch takes
#   (G, H)                     the flush role itself and syncs before sealing.
#   commit fdatasync (F)     — NOT the leader. On the non-rotation path
#                              execute_slots ends at store_head() and returns,
#                              leaving the flush to flush_pending(), run by
#                              whichever writer holds the FlushRole. Arming
#                              only the leader made this cell pass or fail by
#                              race. The class is armed on every writer thread
#                              instead, which is also the truer question: when
#                              the group's fdatasync fails, every writer in it
#                              must learn, whoever ran it.

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from .scenario_matrix import GroupFailureClass


@dataclass
class GroupFaultConfig:
    name: Optional[str] = None
    post_write_mode: Optional[str] = None
    short_write_bytes: int = 0
    # G fires on the pre-rotation fdatasync. Using sync=false for every writer
    # keeps the commit sync from being reached at all, so the only
    # io_data_file_sync checkpoint in the sequence is the rotation's.
    use_sync_false: bool = False
    # Arm on every writer's thread rather than the leader's alone — see the
    # note above on who performs which call.
    arm_on_all_writers: bool = False

    @property
    def is_noop(self) -> bool:
        return self.name is None


_NAMES = {
    GroupFailureClass.B1: "io_data_file_append",
    GroupFailureClass.F: "io_data_file_sync",
    GroupFailureClass.H: "io_rotate_file_creation",
}


def resolve_group_fault(failure: GroupFailureClass) -> GroupFaultConfig:
    if failure == GroupFailureClass.SUCCESS:
        return GroupFaultConfig()
    if failure == GroupFailureClass.B2:
        return GroupFaultConfig(
            name="io_data_file_append_partial",
            post_write_mode="short_write",
            short_write_bytes=5,
        )
    if failure == GroupFailureClass.G:
        return GroupFaultConfig(name="io_data_file_sync", use_sync_false=True)
    if failure == GroupFailureClass.F:
        return GroupFaultConfig(
            name="io_data_file_sync", arm_on_all_writers=True
        )
    return GroupFaultConfig(name=_NAMES[failure])
