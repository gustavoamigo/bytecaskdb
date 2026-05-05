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
    single_update_no_index_change = auto()
    single_update_pk_change = auto()
    multi_row_insert = auto()


class IndexShape(Enum):
    pk_only = auto()
    one_nonunique = auto()
    one_unique = auto()


class TxnShape(Enum):
    autocommit = auto()
    multi_statement = auto()
    with_savepoint = auto()


class PluginFailureClass(Enum):
    SUCCESS = auto()
    OCC_CONFLICT = auto()
    ENGINE_DEGRADED = auto()
    ENGINE_IO_FAIL = auto()
    PLUGIN_INDEX_HALF_BUFFERED = auto()


Scenario = Tuple[DMLShape, IndexShape, TxnShape, PluginFailureClass]


def is_valid_combination(dml: DMLShape, index: IndexShape,
                         txn: TxnShape, failure: PluginFailureClass) -> bool:
    # OCC_CONFLICT only makes sense for ops on existing keys.
    if failure == PluginFailureClass.OCC_CONFLICT:
        if dml == DMLShape.single_insert:
            return False
        if dml == DMLShape.multi_row_insert:
            return False

    # PLUGIN_INDEX_HALF_BUFFERED requires at least one secondary index.
    if failure == PluginFailureClass.PLUGIN_INDEX_HALF_BUFFERED:
        if index == IndexShape.pk_only:
            return False

    # single_update_no_index_change with pk_only is trivial (no index to
    # leave unchanged) — still valid but tests pure row-value update.

    # single_update_pk_change doesn't apply to multi_statement or
    # with_savepoint in this slice (keep combinatorics manageable).
    if dml == DMLShape.single_update_pk_change:
        if txn != TxnShape.autocommit:
            return False

    # multi_row_insert only with autocommit and multi_statement.
    if dml == DMLShape.multi_row_insert:
        if txn == TxnShape.with_savepoint:
            return False

    return True


def generate_matrix() -> Generator[Scenario, None, None]:
    for dml in DMLShape:
        for index in IndexShape:
            for txn in TxnShape:
                for failure in PluginFailureClass:
                    if is_valid_combination(dml, index, txn, failure):
                        yield (dml, index, txn, failure)
