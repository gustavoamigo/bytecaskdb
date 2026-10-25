# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Runs a case over the wire as a client does, through libpq's
extended protocol (psycopg's `pq` binding), so each value is the text the
server sent: PostgreSQL's text form for its type."""

from __future__ import annotations

import dataclasses
from typing import Protocol

from psycopg import pq

from casemodel import Case, Step, Value

_FAILED = (pq.ExecStatus.FATAL_ERROR, pq.ExecStatus.BAD_RESPONSE, pq.ExecStatus.NONFATAL_ERROR)


@dataclasses.dataclass(frozen=True)
class Outcome:
    rows: tuple[tuple[Value, ...], ...] | None = None  # None: the statement returned no result set
    rowcount: int | None = None
    sqlstate: str | None = None  # set when the statement failed
    message: str = ""

    @property
    def failed(self) -> bool:
        return bool(self.message)


class Server(Protocol):
    def dsn(self) -> str: ...
    def restart(self) -> None: ...


def connect(dsn: str) -> pq.PGconn:
    conn = pq.PGconn.connect(dsn.encode())
    if conn.status != pq.ConnStatus.OK:
        message = conn.error_message.decode(errors="replace")
        conn.finish()
        raise ConnectionError(message)
    return conn


def send(conn: pq.PGconn, sql: str, params: tuple[Value, ...] | None = None) -> Outcome:
    """One statement, with `$1`... bound to `params`. A refusal is an
    Outcome with its SQLSTATE, a lost connection one without."""
    try:
        res = conn.exec_params(sql.encode(), [None if p is None else p.encode() for p in params or ()])
    except Exception as e:  # the connection is gone: libpq raises, it has no result to return
        return Outcome(message=f"{type(e).__name__}: {e}")
    if res.status in _FAILED:
        state = res.error_field(pq.DiagnosticField.SQLSTATE)
        return Outcome(sqlstate=state.decode() if state else None,
                       message=res.error_message.decode(errors="replace").strip() or "no message")
    if res.status == pq.ExecStatus.TUPLES_OK:
        rows = tuple(
            tuple(None if (v := res.get_value(r, c)) is None else v.decode()
                  for c in range(res.nfields))
            for r in range(res.ntuples))
        return Outcome(rows=rows, rowcount=res.command_tuples)
    if res.status in (pq.ExecStatus.COMMAND_OK, pq.ExecStatus.EMPTY_QUERY):
        return Outcome(rowcount=res.command_tuples)
    return Outcome(message=f"unexpected result status {res.status.name}")


def sort_rows(rows: tuple[tuple[Value, ...], ...]) -> list[tuple[Value, ...]]:
    """SQL gives no order without ORDER BY: NULL first, then by text."""
    return sorted(rows, key=lambda row: tuple((v is not None, v or "") for v in row))


def _check(case: Case, step: Step, out: Outcome) -> None:
    head = f"[{case.id}] {step.sql!r}"
    if step.error is not None:
        assert out.failed, f"{head}: expected error {step.error}, but it succeeded"
        assert out.sqlstate == step.error, f"{head}: expected error {step.error}, got {out.sqlstate}: {out.message}"
        return
    assert not out.failed, f"{head}: failed, {out.sqlstate}: {out.message}"
    if step.rowcount is not None:
        assert out.rowcount == step.rowcount, f"{head}: expected rowcount {step.rowcount}, got {out.rowcount}"
    if step.expected is not None:
        assert out.rows is not None, f"{head}: expected rows, the statement returned none"
        want, got = list(step.expected), list(out.rows)
        if not step.ordered:
            want, got = sort_rows(step.expected), sort_rows(out.rows)
        assert got == want, f"{head}: rows differ\n  expected: {want}\n  actual:   {got}"


def _teardown(case: Case, server: Server) -> None:
    """Always runs; its errors are ignored."""
    if not case.teardown:
        return
    try:
        conn = connect(server.dsn())
    except ConnectionError:
        return
    try:
        for sql in case.teardown:
            send(conn, sql)
    finally:
        conn.finish()


def run_case(case: Case, server: Server) -> None:
    conn = connect(server.dsn())
    try:
        for sql in case.setup:
            out = send(conn, sql)
            assert not out.failed, f"[{case.id}] setup {sql!r}: {out.sqlstate}: {out.message}"
        for step in case.steps:
            if step.restart:
                conn.finish()
                server.restart()
                conn = connect(server.dsn())
            else:
                _check(case, step, send(conn, step.sql, step.params))
    finally:
        # Closed first: the server rolls back what the case left open.
        conn.finish()
        _teardown(case, server)
