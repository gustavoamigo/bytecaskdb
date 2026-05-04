# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# scenario_matrix.py — DML/Index/Txn/Failure shape enums and valid combo filter.

from enum import Enum, auto
from typing import Generator, Tuple


class DMLShape(Enum):
    single_insert = auto()
    single_delete = auto()
    single_update_index_change = auto()


class IndexShape(Enum):
    pk_only = auto()
    one_nonunique = auto()
    one_unique = auto()


class TxnShape(Enum):
    autocommit = auto()


class PluginFailureClass(Enum):
    SUCCESS = auto()
    OCC_CONFLICT = auto()
    ENGINE_DEGRADED = auto()


Scenario = Tuple[DMLShape, IndexShape, TxnShape, PluginFailureClass]


def is_valid_combination(dml: DMLShape, index: IndexShape,
                         txn: TxnShape, failure: PluginFailureClass) -> bool:
    # OCC_CONFLICT only makes sense for updates (conflict on existing key).
    if failure == PluginFailureClass.OCC_CONFLICT:
        if dml == DMLShape.single_insert:
            return False

    # Delete requires an existing row, which works with all index shapes.
    # No further elimination needed for the first slice.
    return True


def generate_matrix() -> Generator[Scenario, None, None]:
    for dml in DMLShape:
        for index in IndexShape:
            for txn in TxnShape:
                for failure in PluginFailureClass:
                    if is_valid_combination(dml, index, txn, failure):
                        yield (dml, index, txn, failure)
