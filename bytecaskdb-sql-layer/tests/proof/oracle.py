# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""PostgreSQL as the oracle (#515): the statements as PostgreSQL takes them,
its values as the layer's Python values, and both sides' outcomes in one
form, so a test compares them.

PostgreSQL holds the expected data. Text compares under COLLATE "C", byte
order, which is the order of the layer's keys. Values compare as values,
never as printed text.
"""

from __future__ import annotations

import dataclasses
import re

import bytecask_sql

# The layer's constraint kinds and the SQLSTATE PostgreSQL reports for each.
SQLSTATE = {
    "unique": "23505",
    "not_null": "23502",
}


@dataclasses.dataclass(frozen=True)
class Type:
    """A column type, as both sides spell it."""

    sql: str  # the layer's spelling: "bigint", "varchar(4)", ...
    kind: str  # "int", "float", "bool", "text", "bytes"
    length: int = 0  # VARCHAR(n)
    low: int = 0  # integers: the declared range
    high: int = 0

    @property
    def pg(self) -> str:
        """PostgreSQL's spelling: text compares as bytes."""
        return f'{self.sql} COLLATE "C"' if self.kind == "text" else self.sql


BIGINT = Type("bigint", "int", low=-(2**63), high=2**63 - 1)
INTEGER = Type("integer", "int", low=-(2**31), high=2**31 - 1)
SMALLINT = Type("smallint", "int", low=-(2**15), high=2**15 - 1)
DOUBLE = Type("double precision", "float")
BOOLEAN = Type("boolean", "bool")
TEXT = Type("text", "text")
BYTEA = Type("bytea", "bytes")


def varchar(n: int) -> Type:
    return Type(f"varchar({n})", "text", length=n)


@dataclasses.dataclass(frozen=True)
class Outcome:
    """What one statement did: rows and the affected count, or a refusal.

    `error` is None when it ran, a SQLSTATE for a constraint both sides
    name, or "refused" for any other error: the layer's SqlError, and any
    other PostgreSQL error.
    """

    error: str | None = None
    rows: tuple = ()
    detail: str = ""


def literal(t: Type, v) -> str:
    """A constant as SQL text, for DEFAULT: both sides read it the same way."""
    if v is None:
        return "NULL"
    if t.kind == "bool":
        return "TRUE" if v else "FALSE"
    if t.kind in ("int", "float"):
        return repr(v)
    if t.kind == "bytes":
        v = v.decode("ascii")  # printable ASCII only: no escapes to agree on
    return "'" + v.replace("'", "''") + "'"


PARAM = re.compile(r"\$(\d+)")


def pg_params(sql: str, params) -> tuple[str, list | None]:
    """$n placeholders to psycopg's %s, the parameters in their order. A
    statement without parameters goes as written: its literals may hold "%"
    or "$1", which are text, not placeholders."""
    if not params:
        return sql, None
    order = [int(n) - 1 for n in PARAM.findall(sql)]
    return PARAM.sub("%s", sql), [params[i] for i in order]


def from_pg(v):
    """A PostgreSQL value as the layer returns it."""
    return bytes(v) if isinstance(v, memoryview) else v


def run_layer(db, sql: str, params=()) -> Outcome:
    try:
        r = db.execute(sql, tuple(params))
    except bytecask_sql.ExecError as e:
        return Outcome(error=SQLSTATE.get(e.constraint, "refused"), detail=str(e))
    except bytecask_sql.SqlError as e:
        return Outcome(error="refused", detail=str(e))
    return Outcome(rows=tuple(r["rows"]))


def run_pg(conn, sql: str, params=()) -> Outcome:
    import psycopg

    text, values = pg_params(sql, params)
    try:
        with conn.cursor() as cur:
            cur.execute(text, values)
            rows = cur.fetchall() if cur.description else []
    except psycopg.Error as e:
        state = e.sqlstate or ""
        return Outcome(error=state if state in SQLSTATE.values() else "refused", detail=f"{state} {e}")
    return Outcome(rows=tuple(tuple(from_pg(v) for v in row) for row in rows))
