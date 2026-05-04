# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# expected_delta.py — Reference model for expected state changes.

from dataclasses import dataclass, field
from typing import Dict, List

from .scenario_matrix import DMLShape, IndexShape, TxnShape, PluginFailureClass


@dataclass(frozen=True)
class PluginDelta:
    pk_keys_added: List[str] = field(default_factory=list)
    pk_keys_removed: List[str] = field(default_factory=list)
    sec_keys_added: Dict[int, List[str]] = field(default_factory=dict)
    sec_keys_removed: Dict[int, List[str]] = field(default_factory=dict)
    row_count_delta: int = 0
    degraded: bool = False
    threw: bool = False
    error_code: int = 0  # 0 = success


# HA error codes matching the C defines.
HA_ERR_FOUND_DUPP_KEY = 121
HA_ERR_LOCK_DEADLOCK = 1213


def expected_delta(dml: DMLShape, index: IndexShape,
                   txn: TxnShape, failure: PluginFailureClass) -> PluginDelta:
    if failure == PluginFailureClass.ENGINE_DEGRADED:
        return PluginDelta(threw=True, degraded=True, row_count_delta=0)

    if failure == PluginFailureClass.OCC_CONFLICT:
        return PluginDelta(threw=True, error_code=HA_ERR_LOCK_DEADLOCK,
                           row_count_delta=0)

    # SUCCESS path.
    if dml == DMLShape.single_insert:
        return PluginDelta(row_count_delta=1)
    elif dml == DMLShape.single_delete:
        return PluginDelta(row_count_delta=-1)
    elif dml == DMLShape.single_update_index_change:
        return PluginDelta(row_count_delta=0)

    return PluginDelta()
