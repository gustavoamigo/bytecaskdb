# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""PostgreSQL as the oracle (#515): the statements as PostgreSQL takes them,
its values as the layer's Python values, and both sides' outcomes in one
form, so a test compares them.

PostgreSQL holds the expected data. Text compares under COLLATE "C", byte
order, which is the order of the layer's keys. Values compare as values,
never as printed text, and by type: True is not 1, and -0.0 is not 0.0.
"""

from __future__ import annotations

import dataclasses
import re
import struct

import bytecask_sql

# The layer's constraint kinds and the SQLSTATE PostgreSQL reports for each.
SQLSTATE = {
    "unique": "23505",
    "not_null": "23502",
}

# A value outside its declared type is one kind, "range", in the layer; its
# message, PostgreSQL's words, says which. The one mapping from those
# messages, by how they start, to PostgreSQL's SQLSTATE.
# test_every_range_message_maps_to_its_sqlstate produces every one, so a
# reworded message fails there, not as a silent mismatch.
RANGE_SQLSTATE = {
    "smallint out of range": "22003",
    "integer out of range": "22003",
    "value out of range: overflow": "22003",  # REAL
    "value out of range: underflow": "22003",  # REAL
    "numeric field overflow": "22003",  # DECIMAL(p, s)
    "value too long for type character varying": "22001",
    "invalid byte sequence for encoding": "22021",  # TEXT, VARCHAR
    "date out of range": "22008",
    "timestamp out of range": "22008",
    "time out of range": "22008",
}
RANGE_STATES = frozenset(RANGE_SQLSTATE.values())


def layer_sqlstate(e: bytecask_sql.ExecError) -> str:
    """The SQLSTATE PostgreSQL gives the layer's refusal, or "refused"."""
    if e.constraint == "range":
        message = str(e)
        states = [state for start, state in RANGE_SQLSTATE.items() if message.startswith(start)]
        assert len(states) == 1, f"range refusal with no SQLSTATE in RANGE_SQLSTATE: {message}"
        return states[0]
    return SQLSTATE.get(e.constraint, "refused")


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
REAL = Type("real", "real")  # a float: stored as its float rounding on both sides
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
    other PostgreSQL error. `affected` is the rows an INSERT, UPDATE or
    DELETE changed, None for any other statement.
    """

    error: str | None = None
    rows: tuple = ()
    affected: int | None = None
    detail: str = ""


def strict(rows) -> tuple:
    """Rows in a form that compares by type as well as value: each value as
    (type name, value), a float as its exact hex, so True != 1 != 1.0 and
    -0.0 != 0.0."""

    def one(v):
        return (type(v).__name__, v.hex() if isinstance(v, float) else v)

    return tuple(tuple(one(v) for v in row) for row in rows)


def changes_rows(sql: str) -> bool:
    """Whether the statement reports the rows it changed."""
    return sql.split(None, 1)[0].upper() in ("INSERT", "UPDATE", "DELETE")


def literal(t: Type, v) -> str:
    """A constant as SQL text, for DEFAULT: both sides read it the same way."""
    if v is None:
        return "NULL"
    if t.kind == "bool":
        return "TRUE" if v else "FALSE"
    if t.kind in ("int", "float", "real"):
        return repr(v)
    if t.kind == "bytes":
        v = v.decode("ascii")  # printable ASCII only: no escapes to agree on
    return "'" + v.replace("'", "''") + "'"


# Outside a literal: a quoted string ('' escapes a quote), a $n
# placeholder, a %, or any other character.
TOKEN = re.compile(r"'(?:[^']|'')*'|\$(\d+)|%|[^'$%]+|.", re.S)


def pg_params(sql: str, params) -> tuple[str, list | None]:
    """$n placeholders to psycopg's %s, the parameters in their order. With
    parameters, psycopg reads every % as a placeholder's, so a literal %
    becomes %%; a $n inside a string literal is text. A statement without
    parameters goes as written."""
    if not params:
        return sql, None
    out, order = [], []
    for m in TOKEN.finditer(sql):
        token = m.group(0)
        if m.group(1) is not None:
            order.append(int(m.group(1)) - 1)
            out.append("%s")
        else:
            out.append(token.replace("%", "%%"))
    return "".join(out), [params[i] for i in order]


FLOAT4_OID = 700


def from_pg(v, type_oid: int = 0):
    """A PostgreSQL value as the layer returns it. A real arrives as the
    shortest text that names its float, which Python reads as the nearest
    double: packed as a float, it is the float exactly, as the layer stores
    it."""
    if isinstance(v, memoryview):
        return bytes(v)
    if type_oid == FLOAT4_OID and isinstance(v, float):
        return struct.unpack("f", struct.pack("f", v))[0]
    return v


def run_layer(db, sql: str, params=()) -> Outcome:
    try:
        r = db.execute(sql, tuple(params))
    except bytecask_sql.ExecError as e:
        # A constraint violation is an outcome to compare. Any other failure
        # of a plan that ran (corrupt, I/O, a handle, a layout, a limit,
        # memory, a conflict) is the layer failing: the test fails on it.
        if e.constraint is None:
            raise
        return Outcome(error=layer_sqlstate(e), detail=str(e))
    except bytecask_sql.SqlError as e:
        return Outcome(error="refused", detail=str(e))
    return Outcome(rows=tuple(r["rows"]), affected=r["affected"] if changes_rows(sql) else None)


def run_pg(conn, sql: str, params=()) -> Outcome:
    import psycopg

    text, values = pg_params(sql, params)
    try:
        with conn.cursor() as cur:
            cur.execute(text, values)
            rows = cur.fetchall() if cur.description else []
            oids = [d.type_code for d in cur.description] if cur.description else []
            affected = cur.rowcount if changes_rows(sql) else None
    except (psycopg.OperationalError, psycopg.InterfaceError):
        raise  # the connection or the driver failed: not an outcome
    except psycopg.Error as e:
        if not e.sqlstate:
            raise  # no SQLSTATE: not PostgreSQL refusing the statement
        state = e.sqlstate
        known = state in SQLSTATE.values() or state in RANGE_STATES
        return Outcome(error=state if known else "refused", detail=f"{state} {e}")
    return Outcome(rows=tuple(tuple(from_pg(v, oid) for v, oid in zip(row, oids)) for row in rows), affected=affected)
