# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The case model every suite's loader produces (docs/functional_testing_design.md,
*How a run works*): a case is a list of steps, each SQL with one expectation."""

from __future__ import annotations

import dataclasses

Value = str | None
Row = list[Value]


@dataclasses.dataclass(frozen=True)
class Step:
    """One statement (or a restart) and what it must do.

    With none of `expected`, `rowcount` and `error`, the statement must
    succeed. `expected` rows are PostgreSQL's text form, None for NULL.
    """

    sql: str = ""
    expected: tuple[tuple[Value, ...], ...] | None = None
    rowcount: int | None = None
    error: str | None = None
    params: tuple[Value, ...] | None = None
    ordered: bool = False
    restart: bool = False


@dataclasses.dataclass(frozen=True)
class Case:
    suite: str
    id: str
    features: tuple[str, ...]
    steps: tuple[Step, ...]
    setup: tuple[str, ...] = ()
    teardown: tuple[str, ...] = ()
    server_flags: tuple[str, ...] = ()
    unimplemented: bool = False
    # Cases of one group share a server: a file's cases.
    group: str = ""

    @property
    def own_server(self) -> bool:
        return bool(self.server_flags) or any(s.restart for s in self.steps)
