# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Reference model for the group-commit correctness proof.
#
# Derived from execute_slots, not from observation: every failure path ends in
#     for (auto *s : batch) { if (!s->err) s->err = ex; }
# so the group's error reaches every writer in it, and submit() rethrows it on
# each writer's own thread. Which side of the publish the failure falls on is
# what decides whether the group's keys are visible:
#
#   append fails (B1, B2)   — caught before any publish. Nothing lands.
#   commit sync fails (F)   — store_state(degraded_copy) only. Nothing lands.
#   rotation sync fails (G) — same. Nothing lands.
#   rotation create fails (H) — store_state(published, t.persistent()) runs
#                               first, so the whole group lands, then degrades.

from __future__ import annotations

from dataclasses import dataclass

from .scenario_matrix import GroupFailureClass, GroupShape


@dataclass(frozen=True)
class GroupDelta:
    # Did every writer in the group see the failure? The engine has no way to
    # tell one writer its write landed and another that it did not: they share
    # one append and one fdatasync.
    all_threw: bool
    # Are the group's keys visible afterwards? All of them, or none — a mix is
    # the failure these cells exist to catch.
    keys_visible: bool
    degraded: bool


def group_delta(shape: GroupShape, failure: GroupFailureClass) -> GroupDelta:
    if failure == GroupFailureClass.SUCCESS:
        return GroupDelta(all_threw=False, keys_visible=True, degraded=False)
    if failure == GroupFailureClass.H:
        # The one class where the transition is persisted and the engine still
        # degrades: the active file is sealed and cannot take another append.
        return GroupDelta(all_threw=True, keys_visible=True, degraded=True)
    return GroupDelta(all_threw=True, keys_visible=False, degraded=True)
