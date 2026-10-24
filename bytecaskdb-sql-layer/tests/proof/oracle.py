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

import collections
import dataclasses
import decimal
import math
import re
import struct

import bytecask_sql

# The layer's constraint kinds and the SQLSTATE PostgreSQL reports for each.
SQLSTATE = {
    "unique": "23505",
    "not_null": "23502",
}

# A value outside its declared type is one kind, "range", in the layer; its
# message, PostgreSQL's words, says which SQLSTATE, and the layer reports it
# (the API's sqlstate(), as `sqlstate` on each exception). These are every
# such SQLSTATE: test_every_range_message_maps_to_its_sqlstate produces each
# message the layer maps.
RANGE_STATES = frozenset(state for _, state in bytecask_sql._range_messages())

# A statement in a transaction an earlier statement failed in.
ABORTED = "25P02"

# A statement the layer does not support.
NOT_SUPPORTED = "0A000"

# The kind of the planner's refusal (planner_design.md, *What the search
# refuses*): the layer raises bytecask_sql.FeatureNotSupported for it.
# PostgreSQL never reports this kind, so a planner refusal never agrees
# with PostgreSQL, whatever PostgreSQL did with the statement.
NO_PLAN = "no plan"


def is_no_plan(out: "Outcome") -> bool:
    """Whether the layer refused the statement as the planner refuses one."""
    return out.no_plan


# The SQLSTATEs of the binder's and the catalog's refusals that PostgreSQL
# gives the same statement (SqlError.sqlstate): a statement both refuse must
# be refused with the same one. The layer claims one only where PostgreSQL
# refuses the statement with it; a form the layer does not support, which
# PostgreSQL may well run, is 42601, and any other state is "refused".
BINDER_STATES = frozenset(
    {
        "42P01", "42P07", "42703", "42704", "42804", "42P18", "42883", "42702", "42701", "42803", "42809",
        "0A000",  # FOR UPDATE with an aggregate; but see layer_only
        "22003",  # a literal operand past DECIMAL's digits, as a stored value past its column
    }
)

# 0A000 is also one of the layer's own rules, which PostgreSQL does not
# have: a table needs a PRIMARY KEY. The state alone cannot tell it from FOR
# UPDATE with an aggregate, so the statement does: a CREATE TABLE refused
# with 0A000 is the layer's rule, kind "refused", never compared. The
# planner's refusal, also 0A000, is told by its class (run_layer: NO_PLAN).
def layer_only(state: str, sql: str) -> bool:
    """Whether a refusal with this SQLSTATE is one of the layer's own rules."""
    return state == NOT_SUPPORTED and sql.lstrip().upper().startswith("CREATE TABLE")


def error_kind(state: str, sql: str) -> str:
    """The comparable kind of a refusal of `sql` with this SQLSTATE: the
    SQLSTATE where both sides name it, "refused" for any other and for one
    of the layer's own rules (layer_only)."""
    if layer_only(state, sql):
        return "refused"
    known = state in SQLSTATE.values() or state in RANGE_STATES or state in BINDER_STATES or state == ABORTED
    return state if known else "refused"


def layer_sqlstate(e: bytecask_sql.ExecError) -> str:
    """The SQLSTATE PostgreSQL gives the layer's refusal, or "refused"."""
    if e.constraint == "range":
        assert e.sqlstate in RANGE_STATES, f"range refusal with SQLSTATE {e.sqlstate}: {e}"
        return e.sqlstate
    return SQLSTATE.get(e.constraint, "refused")


@dataclasses.dataclass(frozen=True)
class Type:
    """A column type, as both sides spell it."""

    sql: str  # the layer's spelling: "bigint", "varchar(4)", ...
    kind: str  # "int", "float", "real", "decimal", "bool", "text", "bytes"
    length: int = 0  # VARCHAR(n)
    low: int = 0  # integers: the declared range
    high: int = 0
    scale: int = 0  # NUMERIC(p, s): s; its range is in low and high, scaled

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


def numeric(p: int, s: int) -> Type:
    """NUMERIC(p, s): decimal.Decimal on both sides; the layer takes a
    parameter as its integer scaled by 10^s (to_layer)."""
    return Type(f"numeric({p},{s})", "decimal", low=-(10**p - 1), high=10**p - 1, scale=s)


def to_layer(v, t: Type | None):
    """A parameter as the layer takes it: a DECIMAL as its scaled integer,
    anything else as it is. Only Decimals with at most s fractional digits
    are given, so the scaling is exact."""
    if t is not None and t.kind == "decimal" and isinstance(v, decimal.Decimal):
        return int(v.scaleb(t.scale))
    return v


@dataclasses.dataclass(frozen=True)
class Outcome:
    """What one statement did: rows and the affected count, or a refusal.

    `error` is None when it ran, a SQLSTATE for a constraint both sides
    name (a constraint's, a binder refusal's: BINDER_STATES), NO_PLAN for
    the planner's refusal, which nothing on PostgreSQL's side equals, or
    "refused" for any other error: the layer's other SqlErrors and its own
    rules (layer_only), and any other PostgreSQL error. `affected` is the
    rows an INSERT, UPDATE or DELETE changed, None for any other statement.
    """

    error: str | None = None
    rows: tuple = ()
    affected: int | None = None
    detail: str = ""
    # The exact SQLSTATE of a refusal, where the side reports one: the
    # layer's (the API's sqlstate()), the server's and PostgreSQL's.
    sqlstate: str | None = None
    # The rows as their text format's bytes, with each column's type OID:
    # what PostgreSQL and the server send. Empty from the embedded layer.
    text: tuple = ()
    # The layer raised bytecask_sql.FeatureNotSupported: the planner found
    # no plan. Set by run_layer only.
    no_plan: bool = False


def strict(rows) -> tuple:
    """Rows in a form that compares by type as well as value: each value as
    (type name, value), a float as its exact hex, so True != 1 != 1.0 and
    -0.0 != 0.0."""

    def one(v):
        return (type(v).__name__, v.hex() if isinstance(v, float) else v)

    return tuple(tuple(one(v) for v in row) for row in rows)


# A NaN in a sort key: PostgreSQL orders every NaN equal to every other.
_NAN = ("NaN",)


def ordering(k: tuple) -> tuple:
    """A sort key as PostgreSQL compares it: a NaN equal to any other NaN,
    -0.0 equal to 0.0 as in Python."""
    return tuple(_NAN if isinstance(v, float) and math.isnan(v) else v for v in k)


def same_rows(layer: tuple, full: tuple, key, limit: int | None) -> str | None:
    """Whether the layer's rows are a correct answer, given PostgreSQL's
    rows without the LIMIT: in order of `key` (None: no ORDER BY), each run
    of equal keys, as PostgreSQL orders them, the same multiset, and the run
    LIMIT cuts drawn from PostgreSQL's. A reason when they are not."""
    if key is not None:
        key = (lambda k: lambda r: ordering(k(r)))(key)
    want = len(full) if limit is None else min(limit, len(full))
    if len(layer) != want:
        return f"{len(layer)} rows, expected {want}"
    def bag(rows):
        return collections.Counter(strict(rows))
    if key is None:
        if limit is None:
            return None if bag(layer) == bag(full) else "different rows"
        return None if not bag(layer) - bag(full) else "rows PostgreSQL does not hold"
    if [key(r) for r in layer] != [key(r) for r in full[:want]]:
        return "a different order"
    i = 0
    while i < want:
        j = i
        while j < want and key(layer[j]) == key(layer[i]):
            j += 1
        whole = [r for r in full if key(r) == key(layer[i])]
        if j < want or len(whole) == j - i:  # a run the LIMIT did not cut
            if bag(layer[i:j]) != bag(whole):
                return "different rows in a run of equal keys"
        elif bag(layer[i:j]) - bag(whole):
            return "rows PostgreSQL does not hold"
        i = j
    return None


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
    if t.kind == "decimal":
        return str(v)
    if t.kind == "bytes":
        v = v.decode("ascii")  # printable ASCII only: no escapes to agree on
    return "'" + v.replace("'", "''") + "'"


# Outside a literal: a quoted string ('' escapes a quote), a $n
# placeholder, a %, or any other character.
TOKEN = re.compile(r"'(?:[^']|'')*'|\$(\d+)|%|[^'$%]+|.", re.S)


def pg_params(sql: str, params, placeholder: str = "%s") -> tuple[str, list | None]:
    """$n placeholders to psycopg's (%s, or %t for text format), the
    parameters in their order. With parameters, psycopg reads every % as a
    placeholder's, so a literal % becomes %%; a $n inside a string literal
    is text. A statement without parameters goes as written."""
    if not params:
        return sql, None
    out, order = [], []
    for m in TOKEN.finditer(sql):
        token = m.group(0)
        if m.group(1) is not None:
            order.append(int(m.group(1)) - 1)
            out.append(placeholder)
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
    """Runs a statement on a Database, or on a Transaction in its place."""
    try:
        r = db.execute(sql, tuple(params))
    except bytecask_sql.TransactionAborted as e:
        return Outcome(error=ABORTED, detail=str(e), sqlstate=e.sqlstate)
    except bytecask_sql.ExecError as e:
        # A constraint violation is an outcome to compare. Any other failure
        # of a plan that ran (corrupt, I/O, a handle, a layout, a limit,
        # memory, a conflict) is the layer failing: the test fails on it.
        if e.constraint is None:
            raise
        return Outcome(error=layer_sqlstate(e), detail=str(e), sqlstate=e.sqlstate)
    except bytecask_sql.FeatureNotSupported as e:
        return Outcome(error=NO_PLAN, detail=str(e), sqlstate=e.sqlstate, no_plan=True)
    except bytecask_sql.SqlError as e:
        return Outcome(error=error_kind(e.sqlstate, sql), detail=str(e), sqlstate=e.sqlstate)
    return Outcome(rows=tuple(r["rows"]), affected=r["affected"] if changes_rows(sql) else None)


def raw_text(cur) -> tuple:
    """The rows a cursor holds, as the bytes of their text format, and each
    column's type OID."""
    res = cur.pgresult
    if res is None or cur.description is None:
        return ()
    oids = tuple(res.ftype(c) for c in range(res.nfields))
    return (oids, tuple(tuple(res.get_value(r, c) for c in range(res.nfields)) for r in range(res.ntuples)))


def run_psycopg(conn, text: str, values, sql: str) -> Outcome:
    """A statement through psycopg: on PostgreSQL, or on the server."""
    import psycopg

    try:
        with conn.cursor() as cur:
            cur.execute(text, values)
            # A statement with no columns has an empty description, not None.
            rows = cur.fetchall() if cur.description is not None else []
            oids = [d.type_code for d in cur.description] if cur.description is not None else []
            affected = cur.rowcount if changes_rows(sql) else None
            raw = raw_text(cur)
    except (psycopg.OperationalError, psycopg.InterfaceError):
        raise  # the connection or the driver failed: not an outcome
    except psycopg.Error as e:
        if not e.sqlstate:
            raise  # no SQLSTATE: not the server refusing the statement
        state = e.sqlstate
        return Outcome(error=error_kind(state, sql), detail=f"{state} {e}", sqlstate=state)
    return Outcome(rows=tuple(tuple(from_pg(v, oid) for v, oid in zip(row, oids)) for row in rows), affected=affected,
                   text=raw)


def run_pg_numbered(conn, sql: str, params=()) -> Outcome:
    """A statement on PostgreSQL with its $n as written, every parameter
    untyped. psycopg numbers a placeholder by its position, so run_pg cannot
    send a statement whose $1 no use gives a type (42P18)."""
    import psycopg

    res = conn.pgconn.exec_params(sql.encode(), [None if v is None else str(v).encode() for v in params])
    if res.status == psycopg.pq.ExecStatus.FATAL_ERROR:
        state = res.error_field(psycopg.pq.DiagnosticField.SQLSTATE).decode()
        return Outcome(error=error_kind(state, sql), detail=f"{state} {res.error_message.decode()}", sqlstate=state)
    return Outcome(rows=(), affected=None)


def run_pg(conn, sql: str, params=()) -> Outcome:
    text, values = pg_params(sql, params)
    return run_psycopg(conn, text, values, sql)


def run_wire(conn, sql: str, params=()) -> Outcome:
    """A statement on sql_layer_server, as the layer's text with its $n: each
    parameter goes as text (%t), so the server reads it as the type it
    inferred, as it reads HammerDB's."""
    text, values = pg_params(sql, params, "%t")
    return run_psycopg(conn, text, values, sql)
