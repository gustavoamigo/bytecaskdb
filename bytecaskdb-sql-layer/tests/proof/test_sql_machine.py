# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The SQL layer against PostgreSQL: Hypothesis state machines (#515).

Each rule runs one statement on both sides and checks that they agree:
both accept it, changing as many rows and returning the same rows, values
of the same types; or both refuse it, a constraint violation with the same
SQLSTATE. After every step each table must hold the same rows on both
sides, the layer's read from the engine (`table_rows`) and PostgreSQL's in
primary-key order. PostgreSQL holds the expected data; the machine holds
only the expected catalog.

Increment 1: CREATE TABLE (a few types, composite primary keys, nullable
columns, defaults, and refused variants), DROP TABLE [IF EXISTS], INSERT
weighted towards duplicate keys, NULLs, left-out NOT NULL columns and
values outside their declared types, a point lookup by the full primary
key, and a reopen of the layer's database (docs/testing_design.md).

Transactions (#491): BEGIN, the same inserts and lookups in it, which read
its own writes, then COMMIT or ROLLBACK; PostgreSQL runs it in a real
REPEATABLE READ transaction on its connection. A statement that fails
aborts both: every statement after it is 25P02, and COMMIT rolls back. The
tables are compared while no transaction is open; DDL and the reopen run
only then.

Slice 1 of the front end (#492): NUMERIC columns, CREATE INDEX, SELECT by
an equality prefix and a range of any key, with ORDER BY and LIMIT, COUNT,
and SET col = col ± $n. Slice 3: SET col = col ± an integer literal, and a
join of two tables, each by its whole primary key. Slice 4: SUM and MIN by
a key, and UPDATE and DELETE of every row a key's prefix or range reaches.
Slice 5: filters on the columns a key does not take, bounds of $n ± k on
INTEGER columns, COUNT(DISTINCT), and the join of a table reached by its
parameters to one read by its primary key from the first one's columns.
Planner slice 2: every plan the search costs agrees (alternatives agree),
and a join of a table named by its key in parameters to one scanned by
parameters, linked by an equality of columns neither key takes, or of the
scanned table's primary-key columns. Slice 4: full scans, with filters on
columns no key holds, UPDATE and DELETE over them, and ORDER BY on any
columns, several parts, ASC or DESC, NULLS FIRST or LAST, under LIMIT, on
one table and on joins; each plan of every combination of choice-point
branches agrees.

The wire target (docs/server_design.md, *Tests*): with BYTECASK_SQL_SERVER
set, every statement the layer runs also goes, as the same text with its
$n, to sql_layer_server on a database of its own, through psycopg with
text-format parameters. The server must agree with the layer exactly
(the same SQLSTATE, rows, count and block status), and its rows' text must
be PostgreSQL's, byte for byte, with the same type OIDs.
Hash joins: a join no key reaches, linked by equalities of any columns or
by none (a cross join), each table scanned by parameters or in full, with
NULLs and duplicates in the join columns (rows inserted with another
table's value), under ORDER BY of either table's columns or both, and
LIMIT.

A model of the planner's rules (`served`, `join_served`) keeps the machine
to statements the layer plans; the rest must be refused with "no plan".
What is left of it is the dialect's own limit, a filter on a DOUBLE or REAL
column no key takes.

Tables without a primary key: a second machine, KeylessMachine, as this
one reaches a table by its key. Inserts weighted towards copies of rows
the table holds, UPDATE, DELETE and SELECT by full scans, COUNT(*) per
group, and a reopen, each table and result compared as a bag of rows.

The fixed cases at the end call the same steps in a set order.
"""

from __future__ import annotations

import array
import collections
import dataclasses
import decimal
import math
import os
import shutil
import sys
import tempfile

import bytecask_sql
import hypothesis.strategies as st
import pytest
from hypothesis import note
from hypothesis.control import currently_in_test_context
from hypothesis.stateful import RuleBasedStateMachine, invariant, precondition, rule, run_state_machine_as_test

import oracle as o
from conftest import Oracle

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "server"))
from pgserver import Server, server_binary  # noqa: E402

TYPES = [o.BIGINT, o.INTEGER, o.SMALLINT, o.DOUBLE, o.REAL, o.numeric(6, 2), o.numeric(18, 0), o.BOOLEAN, o.TEXT,
         o.varchar(4), o.BYTEA]
# Types a column + parameter is supported on, and how PostgreSQL types that
# parameter: as the column's type (numeric without the modifier).
ARITHMETIC = {"bigint": "bigint", "smallint": "smallint", "integer": "integer", "numeric(6,2)": "numeric",
              "numeric(18,0)": "numeric"}
# The columns SUM and MIN are generated over: the types the layer takes, but
# BIGINT and NUMERIC(18, 0) for SUM, whose sum leaves int64
# (docs/frontend_design.md, *Known differences*).
SUMMED = ("smallint", "integer", "numeric(6,2)")
MINIMISED = SUMMED + ("bigint", "numeric(18,0)")
TABLE_NAMES = ["t0", "t1", "t2", "t3"]
BIGINT_LOW, BIGINT_HIGH = o.BIGINT.low, o.BIGINT.high


@dataclasses.dataclass
class Column:
    name: str
    type: o.Type
    nullable: bool = True
    default: object = None  # None: no DEFAULT clause
    unique: bool = False  # a UNIQUE column constraint

    def ddl(self, pg: bool, primary_key: bool = False) -> str:
        out = f"{self.name} {self.type.pg if pg else self.type.sql}"
        if primary_key:
            out += " PRIMARY KEY"
        if not self.nullable:
            out += " NOT NULL"
        if self.unique:
            out += " UNIQUE"
        if self.default is not None:
            out += " DEFAULT " + o.literal(self.type, self.default)
        return out


@dataclasses.dataclass
class Index:
    name: str
    columns: list[tuple[str, bool]]  # (column, DESC)
    unique: bool = False

    def ddl(self, table: str) -> str:
        cols = ", ".join(c + (" DESC" if desc else "") for c, desc in self.columns)
        return f"CREATE {'UNIQUE ' if self.unique else ''}INDEX {self.name} ON {table} ({cols})"


@dataclasses.dataclass
class Table:
    name: str
    columns: list[Column]
    pk: list[str]
    # Where the primary key is declared: "table", PRIMARY KEY (…); "column",
    # on each of its columns; "both", on its first column and the table.
    # Only "table", or "column" with one key column, declare one key.
    pk_style: str = "table"
    indexes: list[Index] = dataclasses.field(default_factory=list)  # in creation order

    def column(self, name: str) -> Column:
        return next(c for c in self.columns if c.name == name)

    def nullable(self, name: str) -> bool:
        return self.column(name).nullable and name not in self.pk

    def ddl(self, pg: bool) -> str:
        on_column = set(self.pk if self.pk_style == "column" else self.pk[:1] if self.pk_style == "both" else [])
        parts = [c.ddl(pg, c.name in on_column) for c in self.columns]
        if self.pk and self.pk_style != "column":
            parts.append(f"PRIMARY KEY ({', '.join(self.pk)})")
        return f"CREATE TABLE {self.name} ({', '.join(parts)})"

    @property
    def select_all(self) -> str:
        return f"SELECT {', '.join(c.name for c in self.columns)} FROM {self.name}"


def join_served(a: Table, conj_a: list[tuple], b: Table, conj_b: list[tuple], on: list[tuple[str, str]],
                outer: bool = False) -> bool:
    """Whether the planner plans a join of a and b on `on` (a's column, b's)
    with these conjuncts: when some order reads the outer table by any
    access path and the inner one by its whole primary key from parameters
    and outer columns, or by its own scan: under an outer table named by its
    key, read once; under any other, read once per outer row or hash joined.
    Every other conjunct a filter, none on a DOUBLE or REAL. Any ORDER BY is
    served: a sort holds a row of each loop in a sorter of its own. `outer`:
    a LEFT JOIN of b, which keeps a the outer table and never hashes, the
    same conditions in its one order."""
    sides = [(a, conj_a, [x for x, _ in on]), (b, conj_b, [y for _, y in on])]
    for o in ((0,) if outer else (0, 1)):
        (ot, oconj, ocols), (it, iconj, icols) = sides[o], sides[1 - o]
        if not served(ot, oconj):
            continue
        norm = [(c[0], c[1], len(c) > 2 and c[2]) for c in iconj]
        keyed_by_param = [next((i for i, (q, op, calc) in enumerate(norm) if q == k and op == "=" and not calc), None)
                          for k in it.pk]
        inner_floats = any(is_float(it, c) for c in icols) or any(is_float(ot, c) for c in ocols)
        if not any(i is None and k not in icols for i, k in zip(keyed_by_param, it.pk)):
            inner_filters = [c for i, (c, _, _) in enumerate(norm) if i not in keyed_by_param]
            # A key part one link names keys it; a second link on the part
            # is a filter.
            unused_on = list(icols)
            for i, k in zip(keyed_by_param, it.pk):
                if i is None:
                    unused_on.remove(k)
            if not any(is_float(it, c) for c in inner_filters + unused_on):
                return True
        # The inner table by its own scan, the join's conjuncts filtering it.
        if served(it, iconj) and not inner_floats:
            return True
    return False


def typed(t: Table, c: str, n: int) -> str:
    """$n compared with column c, as PostgreSQL's side spells it. The layer
    types $n by its column, as PostgreSQL types a $n it is not told the type
    of; psycopg tells it int8 or float8, so one compared with a SMALLINT,
    INTEGER or REAL casts to the column's type, which also refuses one
    outside it (22003)."""
    ty = t.column(c).type
    return f"${n}::{ty.sql}" if ty.sql in ("smallint", "integer", "real") else f"${n}"


def values(t: o.Type) -> st.SearchStrategy:
    """A value of the type's physical kind: mostly inside its declared
    range, sometimes outside it (a BIGINT for an INTEGER, a REAL past
    float's range, a VARCHAR(n) of more than n characters). Written, both
    sides refuse it; as a lookup key, both refuse a value outside the type's
    range and find nothing for one longer than a VARCHAR(n)."""
    if t.kind == "int":
        edges = st.sampled_from([t.low, t.high, t.low - 1, t.high + 1]).filter(lambda v: BIGINT_LOW <= v <= BIGINT_HIGH)
        return st.one_of(st.integers(-3, 3), st.integers(t.low, t.high), edges, st.integers(BIGINT_LOW, BIGINT_HIGH))
    # NaN, which PostgreSQL orders above every other value and equal to
    # itself, and the infinities, in either type.
    special = st.sampled_from([math.nan, math.inf, -math.inf])
    if t.kind == "float":
        return st.one_of(st.floats(allow_nan=False, allow_infinity=False), special)
    if t.kind == "real":
        # Floats, doubles that round to one, and doubles past float's range
        # at either end (22003 on both).
        inside = st.one_of(st.floats(width=32, allow_nan=False, allow_infinity=False),
                           st.floats(-1e38, 1e38).filter(lambda v: v == 0 or abs(v) > 1e-37))
        return st.one_of(inside, st.sampled_from([1e39, -1e39, 1e-50, -1e-50]), special)
    if t.kind == "decimal":
        # At its scale, so both sides hold it exactly; past its precision now
        # and then (22003 on both).
        scaled = st.one_of(st.integers(-300, 300), st.integers(t.low, t.high),
                           st.sampled_from([t.low, t.high, t.low - 1, t.high + 1]))
        return scaled.map(lambda n: decimal.Decimal(n).scaleb(-t.scale))
    if t.kind == "bool":
        return st.booleans()
    if t.kind == "text":
        chars = st.characters(blacklist_categories=("Cs",), blacklist_characters="\x00")
        if not t.length:
            return st.text(chars, max_size=6)
        # PostgreSQL truncates a VARCHAR(n) whose excess is only spaces; the
        # layer refuses it (docs/frontend_design.md, *Known differences*).
        n = t.length
        return st.text(chars, max_size=n + 2).filter(lambda v: len(v) <= n or v[n:].strip(" "))
    return st.binary(max_size=6)


def operands(t: o.Type) -> st.SearchStrategy:
    """An operand of col ± $n: a value as values(t) draws it, a DECIMAL's
    within 18 digits at its scale. Past them the layer refuses it before the
    plan runs, where PostgreSQL may complete (docs/frontend_design.md, *Known
    differences*); NUMERIC(18, 0)'s edges past its precision are 19 digits."""
    if t.kind != "decimal":
        return values(t)
    return values(t).filter(lambda v: abs(v.scaleb(t.scale)) < 10**18)


def default_values(t: o.Type) -> st.SearchStrategy:
    """A DEFAULT both sides read the same way from its literal."""
    if t.kind == "int":
        return st.integers(-3, 3)
    if t.kind in ("float", "real"):
        return st.sampled_from([0.0, 1.5, -2.25])  # exact in a float too
    if t.kind == "decimal":
        return st.sampled_from([decimal.Decimal("0.00"), decimal.Decimal("1.25"), decimal.Decimal("-2.50")])
    if t.kind == "bool":
        return st.booleans()
    printable = st.characters(min_codepoint=0x20, max_codepoint=0x7E, blacklist_characters="\\")
    text = st.text(printable, max_size=t.length or 4)
    return text.map(str.encode) if t.kind == "bytes" else text


# What the planner serves (docs/planner_design.md, *The search*),
# so the machine generates only statements it plans, and checks that the
# layer refuses the rest with "no plan". ----------------------------------


def key_paths(t: Table) -> list[tuple[bool, list[tuple[str, bool]]]]:
    """The primary key, then each index in creation order (so in object id
    order): whether it is the primary key, and its key's parts (an index's,
    then every part of the primary key, one the index names among them)."""
    out = [(True, [(c, False) for c in t.pk])]
    for ix in t.indexes:
        out.append((False, list(ix.columns) + [(c, False) for c in t.pk]))
    return out


@dataclasses.dataclass(frozen=True)
class Calc:
    """$n + k as a comparison's operand, on an INTEGER column: the
    parameter's value and k."""

    value: object
    k: int


@dataclasses.dataclass(frozen=True)
class Reuse:
    """A HAVING conjunct's operand that is the WHERE's $n, typed there, as
    PostgreSQL types a parameter by its first use."""

    n: int


# Integer types of scale 0, a DECIMAL of scale 0 among them: a $n of one
# compares exactly with a key or an aggregate of another
# (docs/frontend_design.md, *GROUP BY and HAVING*).
INTEGRAL = ("smallint", "integer", "bigint", "numeric(18,0)")


def param_value(v):
    return v.value if isinstance(v, Calc) else v


def compared(t: Table, c: str, op: str, v, n: int, pg: bool, qualified: bool = False) -> str:
    """One conjunct's text: column op $n, or op $n ± k for a Calc, which
    PostgreSQL's side types integer, as it types an unknown $n beside an
    integer literal."""
    col = f"{t.name}.{c}" if qualified else c
    if isinstance(v, Calc):
        return f"{col} {op} ${n}{'::integer' if pg else ''} {'+' if v.k >= 0 else '-'} {abs(v.k)}"
    return f"{col} {op} {typed(t, c, n) if pg else f'${n}'}"


def is_float(t: Table, c: str) -> bool:
    return t.column(c).type.kind in ("float", "real")


def conjuncts(where) -> list[tuple[str, str, bool]]:
    """(column, operator, computed) of each WHERE conjunct."""
    return [(c, op, isinstance(v, Calc)) for c, op, v in where]


def served(t: Table, conj: list[tuple], order: list = (), used: list[str] = ()) -> bool:
    """Whether the planner plans a statement of one table with these WHERE
    conjuncts (column, operator[, computed]): whether some access path takes
    every conjunct on a DOUBLE or REAL column into its key, as a filter on
    one is not supported yet (docs/planner_design.md, *The search*). A path
    is the point lookup on the whole primary key, a key whose first part is
    compared, an equality prefix and a range after it, or a full scan, which
    takes none. Any ORDER BY is served, by a path's order or a sort; `order`
    and `used` are not consulted. An equality with $n ± k is always a
    filter."""
    conj = [(c[0], c[1], len(c) > 2 and c[2]) for c in conj]
    if not any(is_float(t, c) for c, _, _ in conj):
        return True
    eq = {c for c, op, calc in conj if op == "=" and not calc}
    def no_float_filter(keyed: list[int]) -> bool:
        return not any(is_float(t, c) for i, (c, _, _) in enumerate(conj) if i not in keyed)
    def first(c, ops):
        return next((i for i, (q, op, calc) in enumerate(conj) if q == c and op in ops and (ops != ("=",) or not calc)), None)
    if all(c in eq for c in t.pk) and no_float_filter([first(c, ("=",)) for c in t.pk]):
        return True  # a point lookup: every other conjunct a filter
    for _, parts in key_paths(t):
        run = 0
        while run < len(parts) and parts[run][0] in eq:
            run += 1
        keyed = [first(c, ("=",)) for c, _ in parts[:run]]
        if run < len(parts):
            keyed += [first(parts[run][0], (">", ">=")), first(parts[run][0], ("<", "<="))]
        if no_float_filter(keyed):
            return True
    return False


def order_text(order) -> str:
    """ORDER BY items (column, DESC[, NULLS FIRST: True, LAST: False, None
    for the default]) as SQL."""
    def item(o):
        c, desc = o[0], o[1]
        nulls = o[2] if len(o) > 2 else None
        return c + (" DESC" if desc else "") + ("" if nulls is None else " NULLS FIRST" if nulls else " NULLS LAST")
    return ", ".join(item(x) for x in order)


def draw_order(data, columns: list[str], label: str) -> list[tuple]:
    """One to three ORDER BY items of `columns`, each ASC or DESC, with
    NULLS FIRST, LAST or the default."""
    cols = data.draw(st.lists(st.sampled_from(columns), min_size=1, max_size=3, unique=True), label=f"{label} columns")
    return [(c, data.draw(st.booleans(), label=f"{label} {c} desc"),
             data.draw(st.sampled_from([None, True, False]), label=f"{label} {c} nulls")) for c in cols]


class Sides:
    """The two sides a machine runs each statement on, the layer (and the
    server, when the wire target runs) and PostgreSQL, and the checks that
    they agree. A state machine mixes it in and calls open_sides() first."""

    dsn: str = ""  # set by the test that runs the machine

    def open_sides(self):
        # PostgreSQL first: if it cannot connect, nothing else is open yet.
        self.pg = Oracle(self.dsn)
        try:
            self.dir = tempfile.mkdtemp(prefix="bytecask_sql_proof_")
            self.db = bytecask_sql.open(self.dir)
        except BaseException:
            self.pg.close()
            if hasattr(self, "dir"):
                shutil.rmtree(self.dir, ignore_errors=True)
            raise
        self.txn = None  # the layer's open transaction, if any
        self.wire = None  # the server's connection, when the wire target runs
        if server_binary():
            try:
                self.wire_dir = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
                self.connect_wire(Server(self.wire_dir))
            except BaseException:
                self.teardown()
                raise

    def connect_wire(self, server: Server) -> None:
        import psycopg

        self.wire_server = server
        self.wire = psycopg.connect(server.dsn(), autocommit=True, prepare_threshold=None)

    def stop_wire(self) -> None:
        if self.wire is not None:
            self.wire.close()
            self.wire = None
            assert self.wire_server.stop() == 0, "".join(self.wire_server.output)

    def teardown(self):
        # Every step runs, whichever fails, and the first failure is the one raised.
        try:
            if self.txn is not None:
                self.txn.rollback()
                o.run_pg(self.pg.conn, "ROLLBACK")
            self.db.close()
        finally:
            try:
                self.pg.close()
            finally:
                try:
                    self.stop_wire()
                finally:
                    shutil.rmtree(self.dir, ignore_errors=True)
                    if hasattr(self, "wire_dir"):
                        shutil.rmtree(self.wire_dir, ignore_errors=True)

    # The two sides --------------------------------------------------------

    def wire_aborted(self) -> bool:
        import psycopg

        return self.wire.info.transaction_status == psycopg.pq.TransactionStatus.INERROR

    def layer(self, sql: str, params=(), types=None) -> o.Outcome:
        """Runs a statement on the layer, and on the server when the wire
        target runs, which must agree with it exactly. The layer's outcome,
        with the server's text. `types`: each parameter's column type, for
        the DECIMALs the layer takes scaled (o.to_layer)."""
        layer_params = [o.to_layer(v, t) for v, t in zip(params, types)] if types else params
        out = o.run_layer(self.db if self.txn is None else self.txn, sql, layer_params)
        if self.wire is None:
            return out
        wire = o.run_wire(self.wire, sql, params)
        what = f"{sql} {params!r} on the wire"
        assert wire.sqlstate == out.sqlstate, (
            f"{what}: the server {wire.sqlstate or 'ran it'} ({wire.detail}), the layer {out.sqlstate or 'ran it'} "
            f"({out.detail})")
        assert o.strict(wire.rows) == o.strict(out.rows), f"{what}: the server returned {wire.rows}, the layer {out.rows}"
        assert wire.affected == out.affected, f"{what}: the server changed {wire.affected} rows, the layer {out.affected}"
        if self.txn is not None:
            assert self.wire_aborted() == self.txn.is_aborted, f"{what}: the server's block status differs"
        return dataclasses.replace(out, text=wire.text)

    def both(self, sql: str, params=(), pg_sql: str | None = None, types=None) -> o.Outcome:
        """Runs a statement on both sides; they must agree. Returns the layer's
        outcome. `types`: each parameter's column type, for the DECIMALs the
        layer takes scaled (o.to_layer)."""
        layer = self.served(sql, params, self.layer(sql, params, types))
        pg = o.run_pg(self.pg.conn, pg_sql or sql, params)
        if layer.text and pg.text:
            assert layer.text == pg.text, f"{sql} {params!r}: the server sent {layer.text}, PostgreSQL {pg.text}"
        if currently_in_test_context():  # the fixed cases call the steps outside Hypothesis
            note(f"{sql} {params!r} -> layer {layer.error or 'ok'}, postgres {pg.error or 'ok'}")
        assert layer.error == pg.error, (
            f"{sql} {params!r}: the layer {layer.error or 'accepted it'} ({layer.detail}), "
            f"PostgreSQL {pg.error or 'accepted it'} ({pg.detail})"
        )
        assert o.strict(layer.rows) == o.strict(pg.rows), (
            f"{sql} {params!r}: the layer returned {layer.rows}, PostgreSQL {pg.rows}"
        )
        assert layer.affected == pg.affected, (
            f"{sql} {params!r}: the layer changed {layer.affected} rows, PostgreSQL {pg.affected}"
        )
        if self.txn is not None:
            assert self.txn.is_aborted == self.pg_aborted(), (
                f"{sql} {params!r}: the layer's transaction is {'' if self.txn.is_aborted else 'not '}aborted, "
                f"PostgreSQL's {'' if self.pg_aborted() else 'not '}"
            )
        return layer

    @staticmethod
    def served(sql: str, params, out: o.Outcome) -> o.Outcome:
        """`out`, the layer's outcome of a statement the machine predicts the
        planner serves: a planner refusal fails the step, whatever
        PostgreSQL does with the statement."""
        assert not o.is_no_plan(out), f"{sql} {params!r}: predicted served, the planner refused it ({out.detail})"
        return out

    def alternatives_agree(self, sql: str, params, types, full: tuple, key, limit: int | None) -> None:
        """Every plan the search costed for a SELECT returns a correct answer
        on the same data: PostgreSQL's rows `full`, given without the LIMIT,
        as o.same_rows checks them (docs/planner_design.md, *Tests*,
        alternatives agree). Autocommit only: each plan runs on its own
        snapshot, which a transaction's writes are not in."""
        if self.txn is not None:
            return
        layer_params = tuple(o.to_layer(v, t) for v, t in zip(params, types))
        for i, alt in enumerate(self.db._alternatives(sql, layer_params)):
            why = o.same_rows(tuple(alt["rows"]), full, key, limit)
            assert why is None, f"{sql} {params!r}: plan {i}: {why}: it returned {alt['rows']}, PostgreSQL {full}"

    def pg_aborted(self) -> bool:
        import psycopg

        return self.pg.conn.info.transaction_status == psycopg.pq.TransactionStatus.INERROR

    def reopen(self) -> None:
        self.db.close()
        self.db = bytecask_sql.open(self.dir)
        if self.wire is not None:  # the server stopped by SIGTERM and started again, on its data
            self.stop_wire()
            self.connect_wire(Server(self.wire_dir))


class SqlMachine(Sides, RuleBasedStateMachine):
    def __init__(self):
        super().__init__()
        self.tables: dict[str, Table] = {}
        self.open_sides()

    def existing_keys(self, t: Table) -> list[tuple]:
        if self.txn is not None and self.txn.is_aborted:
            return []  # PostgreSQL answers nothing until the rollback
        # Ordered, so a draw from it is reproducible from its seed; in a
        # transaction, the keys it sees.
        out = o.run_pg(
            self.pg.conn, f"SELECT {', '.join(t.pk)} FROM {t.name} ORDER BY {', '.join(t.pk)}"
        )
        assert out.error is None, out.detail
        return list(out.rows)

    def existing_rows(self, t: Table) -> list[tuple]:
        """Every row, in select_all's column order, as the transaction sees it."""
        if self.txn is not None and self.txn.is_aborted:
            return []
        out = o.run_pg(self.pg.conn, f"{t.select_all} ORDER BY {', '.join(t.pk)}")
        assert out.error is None, out.detail
        return list(out.rows)

    # The steps, callable in a set order by the fixed cases ------------------

    def create(self, t: Table) -> o.Outcome:
        out = self.both(t.ddl(pg=False), pg_sql=t.ddl(pg=True))
        if out.error is None:
            self.tables[t.name] = t
        return out

    def drop(self, name: str, if_exists: bool) -> o.Outcome:
        out = self.both(f"DROP TABLE {'IF EXISTS ' if if_exists else ''}{name}")
        if out.error is None:
            self.tables.pop(name, None)
        return out

    def insert(self, name: str, row: dict[str, object], all_columns: bool, literal: bool = False) -> o.Outcome:
        """`literal`: the values written into the text as literals
        (o.sql_literal), not sent as parameters."""
        cols = list(row)
        params = [row[c] for c in cols]
        target = "" if all_columns else f" ({', '.join(cols)})"
        t = self.tables.get(name)
        types = [t.column(c).type for c in cols] if t is not None else None
        if literal and t is not None:
            values = ", ".join(o.sql_literal(t.column(c).type, row[c]) for c in cols)
            return self.both(f"INSERT INTO {name}{target} VALUES ({values})")
        marks = ", ".join(f"${i + 1}" for i in range(len(cols)))
        return self.both(f"INSERT INTO {name}{target} VALUES ({marks})", params, types=types)

    def lookup(self, name: str, key: tuple, for_update: bool = False, literal: bool = False) -> o.Outcome | None:
        """`literal`: the key written into the text as literals; one its
        column's type does not hold exactly is the layer's refusal
        (o.exact_literal), which PostgreSQL does not have."""
        t = self.tables[name]
        if literal:
            lock = " FOR UPDATE" if for_update else ""
            types = [t.column(c).type for c in t.pk]
            sql = f"{t.select_all} WHERE " + " AND ".join(
                f"{c} = {o.sql_literal(ty, v)}" for c, ty, v in zip(t.pk, types, key)) + lock
            if not all(o.exact_literal(ty, v) for ty, v in zip(types, key)):
                out = self.layer(sql)
                if out.error == o.ABORTED:
                    return None  # the transaction was aborted before
                assert out.error == "refused", f"{sql}: a literal its column does not hold was {out.error or 'accepted'}"
                if self.txn is not None and not self.pg_aborted():
                    # A refused statement aborts the transaction on both sides,
                    # as any statement PostgreSQL refuses does.
                    assert self.txn.is_aborted
                    assert o.run_pg(self.pg.conn, "SELECT no_such_column FROM " + name).error == "42703"
                return None
            return self.both(sql)
        where = " AND ".join(f"{c} = ${i + 1}" for i, c in enumerate(t.pk))
        pg_where = " AND ".join(f"{c} = {typed(t, c, i + 1)}" for i, c in enumerate(t.pk))
        # FOR UPDATE returns what the plain SELECT does: on one connection
        # nothing changes a row it guards (the layer) or locks (PostgreSQL).
        lock = " FOR UPDATE" if for_update else ""
        return self.both(f"{t.select_all} WHERE {where}{lock}", key, pg_sql=f"{t.select_all} WHERE {pg_where}{lock}",
                         types=[t.column(c).type for c in t.pk])

    def create_index(self, name: str, ix: Index) -> o.Outcome:
        out = self.both(ix.ddl(name))
        if out.error is None:
            self.tables[name].indexes.append(ix)
        return out

    @staticmethod
    def where_text(conds: list[str]) -> str:
        return f" WHERE {' AND '.join(conds)}" if conds else ""

    def no_plan(self, name: str, sql: str, params: list, types: list, out: o.Outcome | None = None) -> None:
        """The layer must refuse the statement with no plan; PostgreSQL is not
        asked. `out`: the layer's outcome, when it has run already."""
        out = out or self.layer(sql, params, types)
        if out.error == o.ABORTED:
            return  # the transaction was aborted before: nothing was planned
        assert o.is_no_plan(out), f"{sql}: planned, expected no plan ({out})"
        if self.txn is not None and not self.pg_aborted():
            # A refused statement aborts the transaction on both sides, as
            # any statement PostgreSQL refuses does.
            assert self.txn.is_aborted
            assert o.run_pg(self.pg.conn, "SELECT no_such_column FROM " + name).error == "42703"

    def select(self, name: str, targets: list[str], where: list[tuple[str, str, object]],
               order: list[tuple] = (), limit: int | None = None, count: str | None = None,
               fn: str = "count", distinct: bool = False, for_update: bool = False) -> None:
        """SELECT targets (or the aggregate fn(count), "*" for COUNT(*))
        WHERE each (column, operator, value), none for a full scan, ORDER BY
        each (column, DESC[, NULLS FIRST]) ... LIMIT ...: on both sides when
        the planner serves it, else the layer must refuse it with no plan."""
        t = self.tables[name]
        cols = [c for c, _, _ in where]
        used = targets + ([count] if count not in (None, "*") else [])
        select = f"{fn}({'DISTINCT ' if distinct else ''}{count})" if count else ", ".join(targets)
        conds = [compared(t, c, op, v, i + 1, False) for i, (c, op, v) in enumerate(where)]
        pg_conds = [compared(t, c, op, v, i + 1, True) for i, (c, op, v) in enumerate(where)]
        tail = (" ORDER BY " + order_text(order) if order else "") + (f" LIMIT {limit}" if limit is not None else "") + (
            " FOR UPDATE" if for_update else "")
        sql = f"SELECT {select} FROM {name}{self.where_text(conds)}{tail}"
        params = [param_value(v) for _, _, v in where]
        types = [t.column(c).type for c in cols]
        if not served(t, conjuncts(where), list(order), used):
            self.no_plan(name, sql, params, types)
            return
        pg_sql = f"SELECT {select} FROM {name}{self.where_text(pg_conds)}{tail}"
        if count:
            out = self.both(sql, params, pg_sql=pg_sql, types=types)  # one row, or none under LIMIT 0
            if out.error is None:
                self.alternatives_agree(sql, params, types, out.rows, None, None)
            return
        layer = self.served(sql, params, self.layer(sql, params, types))
        full = o.run_pg(self.pg.conn, pg_sql.removesuffix(" FOR UPDATE").removesuffix(f" LIMIT {limit}" if limit is not None else ""), params)
        assert layer.error == full.error, f"{sql} {params!r}: the layer {layer.error or 'ran it'} ({layer.detail}), PostgreSQL {full.error or 'ran it'} ({full.detail})"
        if layer.error is not None:
            return
        key = (lambda r: tuple(r[targets.index(item[0])] for item in order)) if order else None
        why = o.same_rows(layer.rows, full.rows, key, limit)
        assert why is None, f"{sql} {params!r}: {why}: the layer returned {layer.rows}, PostgreSQL {full.rows}"
        self.alternatives_agree(sql, params, types, full.rows, key, limit)

    def write(self, name: str, where: list[tuple[str, str, object]], set_to: tuple[str, str, object] | None = None
              ) -> o.Outcome | None:
        """UPDATE name SET col = $n (op "=") or col = col op $n WHERE each
        (column, operator, value), with set_to = (col, op, value); DELETE
        FROM name WHERE … without; no WHERE for every row. Every row the
        WHERE matches, on both sides when the planner serves it, else the
        layer must refuse it with no plan."""
        t = self.tables[name]
        conds = [compared(t, c, op, v, i + 1, False) for i, (c, op, v) in enumerate(where)]
        pg_conds = [compared(t, c, op, v, i + 1, True) for i, (c, op, v) in enumerate(where)]
        params = [param_value(v) for _, _, v in where]
        types = [t.column(c).type for c, _, _ in where]
        if set_to is None:
            head = pg_head = f"DELETE FROM {name}"
        else:
            col, op, value = set_to
            n = len(where) + 1
            if op == "=":
                # Assigned, as INSERT assigns: PostgreSQL casts $n to the column.
                head = pg_head = f"UPDATE {name} SET {col} = ${n}"
            else:
                head = f"UPDATE {name} SET {col} = {col} {op} ${n}"
                pg_head = f"UPDATE {name} SET {col} = {col} {op} ${n}::{ARITHMETIC[t.column(col).type.sql]}"
            params.append(value)
            types.append(t.column(col).type)
        sql = f"{head}{self.where_text(conds)}"
        if not served(t, conjuncts(where)):
            self.no_plan(name, sql, params, types)
            return None
        return self.both(sql, params, pg_sql=f"{pg_head}{self.where_text(pg_conds)}", types=types)

    def add(self, name: str, col: str, op: str, operand, key: tuple, literal: bool = False,
            target: str | None = None) -> o.Outcome:
        """UPDATE … SET target = col op $1 WHERE the primary key (`target`
        defaults to col); with `literal`, the operand is an integer literal in
        the text, typed by both sides as PostgreSQL types it. The sum is in
        col's type; `target` only checks it on assignment."""
        t = self.tables[name]
        target = target or col
        first = 1 if literal else 2
        where = " AND ".join(f"{c} = ${i + first}" for i, c in enumerate(t.pk))
        pg_where = " AND ".join(f"{c} = {typed(t, c, i + first)}" for i, c in enumerate(t.pk))
        key_types = [t.column(c).type for c in t.pk]
        if literal:
            return self.both(f"UPDATE {name} SET {target} = {col} {op} {operand} WHERE {where}", key,
                             pg_sql=f"UPDATE {name} SET {target} = {col} {op} {operand} WHERE {pg_where}", types=key_types)
        cast = ARITHMETIC[t.column(col).type.sql]
        return self.both(f"UPDATE {name} SET {target} = {col} {op} $1 WHERE {where}", (operand, *key),
                         pg_sql=f"UPDATE {name} SET {target} = {col} {op} $1::{cast} WHERE {pg_where}",
                         types=[t.column(col).type] + key_types)

    def join(self, names: tuple[str, str], targets: list[tuple[str, str]], keys: tuple[tuple, tuple],
             on: bool = False) -> o.Outcome:
        """SELECT targets (table, column) FROM two tables, each named by its
        whole primary key: FROM a, b WHERE …, or FROM a JOIN b ON (b's key)
        WHERE (a's key)."""
        a, b = (self.tables[n] for n in names)
        conds = [[], []]
        pg_conds = [[], []]
        n = 0
        for side, t in enumerate((a, b)):
            for c in t.pk:
                n += 1
                conds[side].append(f"{t.name}.{c} = ${n}")
                pg_conds[side].append(f"{t.name}.{c} = {typed(t, c, n)}")
        select = ", ".join(f"{t}.{c}" for t, c in targets)
        def text(cs):
            if on:
                return f"SELECT {select} FROM {a.name} JOIN {b.name} ON {' AND '.join(cs[1])} WHERE {' AND '.join(cs[0])}"
            return f"SELECT {select} FROM {a.name}, {b.name} WHERE {' AND '.join(cs[0] + cs[1])}"
        params = [*keys[0], *keys[1]]
        types = [a.column(c).type for c in a.pk] + [b.column(c).type for c in b.pk]
        out = self.both(text(conds), params, pg_sql=text(pg_conds), types=types)
        if out.error is None:
            self.alternatives_agree(text(conds), params, types, out.rows, None, None)
        return out

    def join_on(self, a_name: str, b_name: str, where_a: list[tuple[str, str, object]],
                keys_b: list[tuple[str, tuple[str, object]]], filters_b: list[tuple[str, str, object]],
                targets: list[tuple[str, str]], limit: int | None = None, distinct: tuple[str, str] | None = None,
                links: list[tuple[str, str]] = (), order: list[tuple] = (), outer: str | None = None,
                anti: bool = False) -> None:
        """SELECT targets (or COUNT(DISTINCT table.col)) FROM a, b WHERE a's
        conjuncts, each of b's key parts equal to a column of a ("col", name)
        or to a parameter ("param", value), b's filters, and each link (b's
        column, a's) an equality of a column b's key does not hold with a
        column of a, ORDER BY each (table, column, DESC, NULLS FIRST): on both
        sides when the planner serves it, else the layer must refuse it with
        no plan. `outer` "LEFT" or "RIGHT": a outer joined to b, b's
        conditions and the links its ON, a's its WHERE; `anti`: the WHERE
        also takes the rows b matched none of, by b's first key column IS
        NULL."""
        a, b = self.tables[a_name], self.tables[b_name]
        conds, pg_conds, params, types = [], [], [], []
        def add(t, c, op, v):
            n = len(params) + 1
            conds.append(compared(t, c, op, v, n, False, True))
            pg_conds.append(compared(t, c, op, v, n, True, True))
            params.append(param_value(v))
            types.append(t.column(c).type)
        for c, op, v in where_a:
            add(a, c, op, v)
        n_a = len(conds)
        on = []
        for c, (kind, v) in keys_b:
            if kind == "col":
                conds.append(f"{b_name}.{c} = {a_name}.{v}")
                pg_conds.append(conds[-1])
                on.append((v, c))
            else:
                add(b, c, "=", v)
        for c, op, v in filters_b:
            add(b, c, op, v)
        for c, v in links:
            conds.append(f"{b_name}.{c} = {a_name}.{v}")
            pg_conds.append(conds[-1])
            on.append((v, c))
        select = f"count(DISTINCT {distinct[0]}.{distinct[1]})" if distinct else ", ".join(f"{t}.{c}" for t, c in targets)
        ordered = " ORDER BY " + order_text([(f"{t}.{c}", *rest) for t, c, *rest in order]) if order else ""
        tail = f" LIMIT {limit}" if limit is not None else ""
        if outer and len(conds) > n_a:
            def text(cs):
                where = cs[:n_a] + ([f"{b_name}.{b.pk[0]} IS NULL"] if anti else [])
                joined = (f"{a_name} LEFT JOIN {b_name}" if outer == "LEFT" else f"{b_name} RIGHT JOIN {a_name}")
                return f"SELECT {select} FROM {joined} ON {' AND '.join(cs[n_a:])}{self.where_text(where)}"
            sql, pg_sql = text(conds) + ordered + tail, text(pg_conds) + ordered
        else:
            outer = None
            sql = f"SELECT {select} FROM {a_name}, {b_name}{self.where_text(conds)}{ordered}{tail}"
            pg_sql = f"SELECT {select} FROM {a_name}, {b_name}{self.where_text(pg_conds)}{ordered}"
        conj_b = [(c, "=", False) for c, (kind, _) in keys_b if kind == "param"] + conjuncts(filters_b)
        if not join_served(a, conjuncts(where_a), b, conj_b, on, outer=outer is not None):
            self.no_plan(a_name, sql, params, types)
            return
        out = None
        if distinct:
            out = self.both(sql, params, pg_sql=pg_sql + tail, types=types)
            if out.error is None:
                self.alternatives_agree(sql, params, types, out.rows, None, None)
            return
        layer = self.served(sql, params, out or self.layer(sql, params, types))
        full = o.run_pg(self.pg.conn, pg_sql, params)
        assert layer.error == full.error, f"{sql} {params!r}: the layer {layer.error or 'ran it'} ({layer.detail}), PostgreSQL {full.error or 'ran it'} ({full.detail})"
        if layer.error is not None:
            return
        key = (lambda r: tuple(r[targets.index((t, c))] for t, c, *_ in order)) if order else None
        why = o.same_rows(layer.rows, full.rows, key, limit)
        assert why is None, f"{sql} {params!r}: {why}: the layer returned {layer.rows}, PostgreSQL {full.rows}"
        self.alternatives_agree(sql, params, types, full.rows, key, limit)

    def grouped(self, names: list[str], where: list[tuple[str, str, str, object]], group: list[tuple[str, str]],
                aggs: list[tuple], having: list[tuple] = (), order: list[tuple] = (), limit: int | None = None,
                links: list[tuple[str, str]] = ()) -> None:
        """SELECT the GROUP BY's columns (table, column), then each aggregate
        (fn, table, column or None for COUNT(*), DISTINCT), FROM one table or
        two, the second's column equal to the first's in each link, WHERE each
        (table, column, op, value), GROUP BY `group`, HAVING each (("col",
        table, column) or ("agg", aggregate), op, value), ORDER BY each
        (table, column, DESC, NULLS FIRST), LIMIT: on both sides when the
        planner serves it, else the layer must refuse it with no plan. A
        group of a DOUBLE or REAL key holding -0.0 and 0.0 shows either, as
        PostgreSQL's does by its plan: both compare as 0.0."""
        tables = [self.tables[n] for n in names]
        by_name = {t.name: t for t in tables}
        conds, pg_conds, params, types = [], [], [], []
        def add(text, pg_text, v, ty):
            conds.append(text)
            pg_conds.append(pg_text)
            params.append(param_value(v))
            types.append(ty)
        for tn, c, op, v in where:
            t = by_name[tn]
            n = len(params) + 1
            add(compared(t, c, op, v, n, False, True), compared(t, c, op, v, n, True, True), v, t.column(c).type)
        link_text = [f"{names[1]}.{c} = {names[0]}.{v}" for c, v in links]
        def agg_text(a):
            fn, tn, c, distinct = a
            return f"{fn}(*)" if c is None else f"{fn}({'DISTINCT ' if distinct else ''}{tn}.{c})"
        hav, pg_hav = [], []
        for what, op, v in having:
            n = len(params) + 1
            if isinstance(v, Reuse):
                lhs = f"{what[1]}.{what[2]}" if what[0] == "col" else agg_text(what[1])
                hav.append(f"{lhs} {op} ${v.n}")
                pg_hav.append(f"{lhs} {op} ${v.n}")
                continue
            if what[0] == "col":
                t = by_name[what[1]]
                hav.append(compared(t, what[2], op, v, n, False, True))
                pg_hav.append(compared(t, what[2], op, v, n, True, True))
                params.append(v)
                types.append(t.column(what[2]).type)
                continue
            fn, tn, c, _ = what[1]
            ty = o.BIGINT
            pg_param = f"${n}"
            if fn == "sum" and by_name[tn].column(c).type.kind == "decimal":
                ty = by_name[tn].column(c).type
            elif fn in ("min", "max"):
                ty = by_name[tn].column(c).type
                pg_param = typed(by_name[tn], c, n)
            hav.append(f"{agg_text(what[1])} {op} ${n}")
            pg_hav.append(f"{agg_text(what[1])} {op} {pg_param}")
            params.append(v)
            types.append(ty)
        targets = [f"{tn}.{c}" for tn, c in group] + [agg_text(a) for a in aggs]
        if not targets:
            return
        def text(cs, hs):
            w = cs + link_text
            out = f"SELECT {', '.join(targets)} FROM {', '.join(names)}" + (f" WHERE {' AND '.join(w)}" if w else "")
            if group:
                out += " GROUP BY " + ", ".join(f"{tn}.{c}" for tn, c in group)
            if hs:
                out += " HAVING " + " AND ".join(hs)
            if order:
                out += " ORDER BY " + order_text([(f"{tn}.{c}", *rest) for tn, c, *rest in order])
            return out
        tail = f" LIMIT {limit}" if limit is not None else ""
        sql = text(conds, hav) + tail
        pg_sql = text(pg_conds, pg_hav)
        conj = {n: [(c, op, isinstance(v, Calc)) for tn, c, op, v in where if tn == n] for n in names}
        if len(tables) == 1:
            ok = served(tables[0], conj[names[0]])
        else:
            ok = join_served(tables[0], conj[names[0]], tables[1], conj[names[1]], [(v, c) for c, v in links])
        # A HAVING on a DOUBLE or REAL key is a filter on one.
        ok = ok and not any(w[0] == "col" and is_float(by_name[w[1]], w[2]) for w, _, _ in having)
        if not ok:
            self.no_plan(names[0], sql, params, types)
            return
        layer = self.layer(sql, params, types)
        full = o.run_pg(self.pg.conn, pg_sql, params)
        assert layer.error == full.error, (
            f"{sql} {params!r}: the layer {layer.error or 'ran it'} ({layer.detail}), "
            f"PostgreSQL {full.error or 'ran it'} ({full.detail})")
        if layer.error is not None:
            return
        floats = [i for i, (tn, c) in enumerate(group) if is_float(by_name[tn], c)]
        def norm(rows):
            return tuple(tuple(0.0 if i in floats and v == 0 else v for i, v in enumerate(r)) for r in rows)
        key = (lambda r: tuple(r[group.index((tn, c))] for tn, c, *_ in order)) if order else None
        want = norm(full.rows)
        why = o.same_rows(norm(layer.rows), want, key, limit)
        assert why is None, f"{sql} {params!r}: {why}: the layer returned {layer.rows}, PostgreSQL {full.rows}"
        if layer.text and full.text and limit is None and not floats:
            # The server's text, an AVG's digits among it, is PostgreSQL's.
            assert layer.text[0] == full.text[0], f"{sql}: type OIDs {layer.text[0]}, PostgreSQL {full.text[0]}"
            assert collections.Counter(layer.text[1]) == collections.Counter(full.text[1]), (
                f"{sql} {params!r}: the server sent {layer.text[1]}, PostgreSQL {full.text[1]}")
        if self.txn is None:
            layer_params = tuple(o.to_layer(v, t) for v, t in zip(params, types))
            for i, alt in enumerate(self.db._alternatives(sql, layer_params)):
                why = o.same_rows(norm(tuple(alt["rows"])), want, key, limit)
                assert why is None, f"{sql} {params!r}: plan {i}: {why}: it returned {alt['rows']}, PostgreSQL {full.rows}"

    def draw_aggs(self, data, tables: list[Table], most: int) -> list[tuple]:
        """Up to `most` aggregates of the tables' columns: COUNT(*), COUNT and
        COUNT(DISTINCT) of any column, SUM and AVG of the SUMMED types (a
        BIGINT's total leaves int64), MIN and MAX of the MINIMISED ones."""
        choices = [("count", None, None, False)]
        for t in tables:
            for c in t.columns:
                choices += [("count", t.name, c.name, False), ("count", t.name, c.name, True)]
                if c.type.sql in SUMMED:
                    choices += [("sum", t.name, c.name, False), ("avg", t.name, c.name, False)]
                if c.type.sql in MINIMISED:
                    choices += [("min", t.name, c.name, False), ("max", t.name, c.name, False)]
        return data.draw(st.lists(st.sampled_from(choices), max_size=most, unique=True), label="aggregates")

    def draw_having(self, data, group: list[tuple[str, str]], aggs: list[tuple]) -> list[tuple]:
        """Up to two HAVING conjuncts: a key, COUNT, SUM, MIN or MAX compared
        with a parameter, of a value of its type, often from a row."""
        options = [("col", tn, c) for tn, c in group] + [("agg", a) for a in aggs if a[0] != "avg"]
        if not options:
            return []
        out = []
        for i in range(data.draw(st.integers(0, 2), label="HAVING conjuncts")):
            what = data.draw(st.sampled_from(options), label=f"having {i}")
            op = data.draw(st.sampled_from(["=", "<", "<=", ">", ">="]), label=f"having {i} op")
            if what[0] == "col":
                ty = self.tables[what[1]].column(what[2]).type
                rows = self.existing_rows(self.tables[what[1]])
                at = [c.name for c in self.tables[what[1]].columns].index(what[2])
                v = data.draw((st.sampled_from([r[at] for r in rows]) if rows else st.nothing()) | st.none() | values(ty),
                              label=f"having {i} value")
            else:
                fn, tn, c, _ = what[1]
                ty = self.tables[tn].column(c).type if c is not None else o.BIGINT
                if fn == "count":
                    v = data.draw(st.integers(-1, 6), label=f"having {i} count")
                elif fn == "sum" and ty.kind == "decimal":
                    v = data.draw(st.integers(-2000, 2000).map(lambda n: decimal.Decimal(n).scaleb(-ty.scale)), label="sum")
                elif fn == "sum":
                    v = data.draw(st.integers(-200000, 200000), label=f"having {i} sum")
                else:
                    v = data.draw(values(ty), label=f"having {i} value")
            out.append((what, op, v))
        return out

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def group_by(self, data):
        # GROUP BY one or two columns of one table, any of them, under a key
        # path's conjuncts or a full scan: in a structure, or streamed where
        # an index orders the groups; with HAVING, ORDER BY of the keys and a
        # LIMIT. Or no GROUP BY: several aggregates, one row.
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, parts, run = self.where_on_a_path(data, t)
        names = [c.name for c in t.columns]
        if data.draw(st.integers(0, 4), label="grouped?"):
            # Mostly a path's parts after its prefix: an order that streams.
            if parts[run:] and data.draw(st.booleans(), label="the path's parts?"):
                cols = [c for c, _ in parts[run:run + data.draw(st.integers(1, 2), label="keys")]]
                cols = list(dict.fromkeys(cols))
            else:
                cols = data.draw(st.lists(st.sampled_from(names), min_size=1, max_size=2, unique=True), label="keys")
        else:
            cols = []
        group = [(t.name, c) for c in data.draw(st.permutations(cols), label="key order")]
        aggs = self.draw_aggs(data, [t], 3 if group else 4)
        if not group and not aggs:
            aggs = [("count", None, None, False)]
        having = self.draw_having(data, group, aggs)
        order = []
        if group and data.draw(st.booleans(), label="ORDER BY?"):
            keys = data.draw(st.lists(st.sampled_from(group), min_size=1, unique=True), label="order")
            order = [(tn, c, data.draw(st.booleans(), label="desc"), data.draw(st.sampled_from([None, True, False]),
                                                                              label="nulls")) for tn, c in keys]
        limit = data.draw(st.none() | st.sampled_from([0, 1, 2, 5]), label="limit")
        self.grouped([t.name], [(t.name, c, op, v) for c, op, v in where], group, aggs, having, order, limit)

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def having_reuses_a_parameter(self, data):
        # A HAVING that compares a key or an aggregate with a $n the WHERE
        # compares an integer column with: typed by its first use, the
        # WHERE's, on both sides.
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, _, _ = self.where_on_a_path(data, t)
        reusable = [i for i, (c, _, v) in enumerate(where) if t.column(c).type.sql in INTEGRAL and not isinstance(v, Calc)]
        if not reusable:
            return
        at = data.draw(st.sampled_from(reusable), label="reused")
        names = [c.name for c in t.columns]
        group = [(t.name, c) for c in data.draw(st.lists(st.sampled_from(names), max_size=2, unique=True), label="keys")]
        aggs = self.draw_aggs(data, [t], 2)
        options = [("col", tn, c) for tn, c in group if t.column(c).type.sql in INTEGRAL]
        options += [("agg", a) for a in aggs + [("count", None, None, False)]
                    if a[0] == "count" or (a[0] in ("sum", "min", "max") and t.column(a[2]).type.sql in INTEGRAL)]
        what = data.draw(st.sampled_from(options), label="HAVING") if options else ("agg", ("count", None, None, False))
        if what[0] == "agg" and what[1] not in aggs:
            aggs = aggs + [what[1]]
        op = data.draw(st.sampled_from(["=", "<", "<=", ">", ">="]), label="HAVING op")
        self.grouped([t.name], [(t.name, c, o_, v) for c, o_, v in where], group, aggs, [(what, op, Reuse(at + 1))])

    @precondition(lambda self: len(self.tables) >= 2)
    @rule(data=st.data())
    def group_by_join(self, data):
        # A join no key reaches, or one by a key, linked by an equality of
        # columns of one type, grouped by a column of either table.
        a_name, b_name = data.draw(st.permutations(sorted(self.tables)), label="tables")[:2]
        a, b = self.tables[a_name], self.tables[b_name]
        pairs = [(x.name, y.name) for x in b.columns for y in a.columns if y.type.sql == x.type.sql]
        if not pairs:
            return
        links = data.draw(st.lists(st.sampled_from(pairs), min_size=1, max_size=2, unique=True), label="links")
        where_a, _, _ = self.where_on_a_path(data, a)
        where_b, _, _ = self.where_on_a_path(data, b)
        columns = [(t.name, x.name) for t in (a, b) for x in t.columns]
        group = data.draw(st.lists(st.sampled_from(columns), max_size=2, unique=True), label="keys")
        aggs = self.draw_aggs(data, [a, b], 3)
        if not group and not aggs:
            aggs = [("count", None, None, False)]
        having = self.draw_having(data, group, aggs)
        order = []
        if group and data.draw(st.booleans(), label="ORDER BY?"):
            order = [(tn, c, data.draw(st.booleans(), label="desc"), None) for tn, c in group[:1]]
        where = [(a_name, c, op, v) for c, op, v in where_a] + [(b_name, c, op, v) for c, op, v in where_b]
        self.grouped([a_name, b_name], where, group, aggs, having, order,
                     data.draw(st.none() | st.sampled_from([0, 2]), label="limit"), links=links)

    def begin(self) -> None:
        # PostgreSQL's REPEATABLE READ, like the layer's, takes its snapshot
        # at the first statement, not at BEGIN.
        self.txn = self.db.begin()
        assert o.run_pg(self.pg.conn, "BEGIN ISOLATION LEVEL REPEATABLE READ").error is None
        if self.wire is not None:
            assert o.run_wire(self.wire, "BEGIN").error is None

    def commit(self) -> str:
        """COMMIT on both; returns the outcome, as PostgreSQL's command tag says it.

        In an aborted transaction both roll back, without an error. One
        connection: nothing commits in between, so nothing conflicts.
        """
        layer = self.txn.commit()
        self.txn = None
        with self.pg.conn.cursor() as cur:
            cur.execute("COMMIT")
            pg = cur.statusmessage
        assert layer == pg, f"COMMIT: the layer {layer}, PostgreSQL {pg}"
        if self.wire is not None:
            with self.wire.cursor() as cur:
                cur.execute("COMMIT")
                assert cur.statusmessage == layer, f"COMMIT: the server {cur.statusmessage}, the layer {layer}"
        return layer

    def rollback(self) -> None:
        self.txn.rollback()
        self.txn = None
        assert o.run_pg(self.pg.conn, "ROLLBACK").error is None
        if self.wire is not None:
            assert o.run_wire(self.wire, "ROLLBACK").error is None

    def check(self) -> None:
        names = sorted(t["name"] for t in self.db.catalog())
        assert names == sorted(self.tables), f"the layer's tables are {names}, expected {sorted(self.tables)}"
        for t in self.tables.values():
            layer = self.db.table_rows(t.name)["rows"]
            pg = o.run_pg(self.pg.conn, f"{t.select_all} ORDER BY {', '.join(t.pk)}")
            assert pg.error is None, pg.detail
            assert o.strict(layer) == o.strict(pg.rows), (
                f"table {t.name}: the layer holds {layer}, PostgreSQL {list(pg.rows)}"
            )

    # Rules ----------------------------------------------------------------

    @precondition(lambda self: self.txn is None)
    @rule(data=st.data())
    def create_table(self, data):
        free = [n for n in TABLE_NAMES if n not in self.tables]
        # Mostly a free name; sometimes one that exists, which both refuse.
        pool = free if free and data.draw(st.integers(0, 4), label="free name?") else TABLE_NAMES
        name = data.draw(st.sampled_from(pool), label="name")
        n = data.draw(st.integers(1, 4), label="columns")
        # Now and then a type another table has, so joins find columns to
        # compare.
        taken = sorted({c.type.sql: c.type for t in self.tables.values() for c in t.columns}.items())
        columns = []
        for i in range(n):
            if taken and data.draw(st.integers(0, 2), label=f"c{i} type taken?") == 0:
                ty = data.draw(st.sampled_from(taken), label=f"c{i} type")[1]
            else:
                ty = data.draw(st.sampled_from(TYPES), label=f"c{i} type")
            nullable = data.draw(st.booleans(), label=f"c{i} nullable")
            default = data.draw(st.none() | default_values(ty), label=f"c{i} default")
            columns.append(Column(f"c{i}", ty, nullable, default))
        pk_count = data.draw(st.integers(1, min(2, n)), label="pk columns")
        pk = data.draw(st.permutations([c.name for c in columns]), label="pk order")[:pk_count]
        for c in columns:
            if c.name in pk:
                c.default = None
        t = Table(name, columns, list(pk))
        variant = data.draw(st.sampled_from(["ok"] * 6 + ["duplicate column", "unknown pk column", "no pk",
                                                          "column pk", "column and table pk"]))
        if variant == "column pk":
            t.pk_style = "column"  # two key columns: two PRIMARY KEYs, which both refuse
        elif variant == "column and table pk":
            t.pk_style = "both"
        elif variant == "duplicate column" and n > 1:
            t.columns[-1].name = t.columns[0].name
            t.pk = [t.columns[0].name]
        elif variant == "unknown pk column":
            t.pk = ["zz"]
        elif variant == "no pk":
            # Both take it; this machine's steps reach a table by its key, so
            # it is dropped again. KeylessMachine runs the tables without one.
            t.pk = []
            if self.both(t.ddl(pg=False), pg_sql=t.ddl(pg=True)).error is None:
                assert self.both(f"DROP TABLE {t.name}").error is None
            return
        self.create(t)

    @precondition(lambda self: self.txn is None)
    @rule(data=st.data())
    def drop_table(self, data):
        self.drop(data.draw(st.sampled_from(TABLE_NAMES), label="name"), data.draw(st.booleans(), label="if exists"))

    def insert_row(self, data):
        missing = [n for n in TABLE_NAMES if n not in self.tables]
        if missing and data.draw(st.integers(0, 9), label="missing table?") == 0:
            self.insert(data.draw(st.sampled_from(missing)), {"c0": 1}, all_columns=False)  # both refuse
            return
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        name = t.name
        all_columns = data.draw(st.booleans(), label="no column list")
        keys = self.existing_keys(t)
        duplicate = keys and data.draw(st.integers(0, 2), label="duplicate?") == 0
        dup = data.draw(st.sampled_from(keys)) if duplicate else None
        row = {}
        for c in t.columns:
            # Leave out a column now and then: one with a default or nullable
            # often, a NOT NULL one without a default, a key column among
            # them, rarely (both refuse it, 23502).
            optional = (c.nullable or c.default is not None) and c.name not in t.pk
            if not all_columns and data.draw(st.integers(0, 3 if optional else 11), label=f"omit {c.name}") == 0:
                continue
            if dup is not None and c.name in t.pk:
                row[c.name] = dup[t.pk.index(c.name)]
            elif data.draw(st.integers(0, 4), label=f"{c.name} null?") == 0:
                row[c.name] = None
            else:
                row[c.name] = data.draw(values(c.type), label=c.name)
        self.insert(name, row, all_columns, literal=data.draw(st.integers(0, 3), label="literals?") == 0)

    # Inserts are three times as likely as any other step: three rules run
    # them, so the reads find rows.
    insert_one = precondition(lambda self: self.tables)(rule(data=st.data())(insert_row))
    insert_another = precondition(lambda self: self.tables)(rule(data=st.data())(insert_row))
    insert_a_third = precondition(lambda self: self.tables)(rule(data=st.data())(insert_row))

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def lookup_by_pk(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        keys = self.existing_keys(t)
        if keys and data.draw(st.booleans(), label="existing key"):
            key = data.draw(st.sampled_from(keys), label="key")
        else:
            key = tuple(data.draw(st.none() | values(t.column(c).type), label=c) for c in t.pk)
        self.lookup(t.name, key, for_update=data.draw(st.booleans(), label="FOR UPDATE"),
                    literal=data.draw(st.integers(0, 3), label="literal key?") == 0)

    @precondition(lambda self: self.tables and self.txn is None)
    @rule(data=st.data())
    def create_index_rule(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        n = data.draw(st.integers(1, min(2, len(t.columns))), label="index columns")
        cols = data.draw(st.permutations([c.name for c in t.columns]), label="index order")[:n]
        ix = Index(f"{t.name}_ix{len(t.indexes)}", [(c, data.draw(st.booleans(), label=f"{c} desc")) for c in cols],
                   unique=data.draw(st.integers(0, 3), label="unique?") == 0)
        self.create_index(t.name, ix)

    def where_on_a_path(self, data, t: Table) -> tuple[list[tuple[str, str, object]], list[tuple[str, bool]], int]:
        """Conjuncts a key path expresses: an equality prefix of one of the
        table's keys and a range on the part after it, mostly from an
        existing row, in shuffled order; now and then none, for a full scan.
        Returns them, the path's parts and the prefix's length."""
        is_pk, parts = data.draw(st.sampled_from(key_paths(t)), label="path")
        rows = self.existing_rows(t)
        row = data.draw(st.sampled_from(rows), label="row") if rows and data.draw(st.integers(0, 3), label="from a row?") else None
        names = [c.name for c in t.columns]
        def value(c: str):
            if row is not None and data.draw(st.integers(0, 4), label=f"{c} from the row?"):
                return row[names.index(c)]
            return data.draw(st.none() | values(t.column(c).type), label=c)
        full = data.draw(st.integers(0, 4), label="full scan?") == 0
        run = 0 if full else data.draw(st.integers(0, len(parts)), label="prefix")
        where = []
        for c, _ in parts[:run]:
            if c not in [q for q, _, _ in where]:  # a key repeats a primary key column its index names
                where.append((c, "=", value(c)))
        if full:
            pass  # filters alone
        elif run < len(parts) and parts[run][0] in [c for c, _, _ in where]:
            pass  # a column the prefix compares, repeated in the key: no range on it
        elif run < len(parts) and (run == 0 or data.draw(st.booleans(), label="range?")):
            c = parts[run][0]
            bounds = data.draw(st.sampled_from([(">",), (">=",), ("<",), ("<=",), (">", "<"), (">=", "<="), (">=", "<")]),
                               label="bounds")
            where += [(c, op, value(c)) for op in bounds]
        # Now and then an INTEGER conjunct's operand is $n + k, mostly the
        # same value; and a filter or two on any column.
        for i, (c, op, v) in enumerate(where):
            if t.column(c).type.sql == "integer" and isinstance(v, int) and data.draw(st.integers(0, 5), label=f"{c} $n + k?") == 0:
                k = data.draw(st.integers(-30, 30), label="k")
                where[i] = (c, op, Calc(v - k if data.draw(st.integers(0, 3), label="same value?") else v, k))
        for i in range(data.draw(st.integers(0, 2), label="filters")):
            c = data.draw(st.sampled_from(names), label=f"filter {i}")
            op = data.draw(st.sampled_from(["=", "<", "<=", ">", ">="]), label=f"filter {i} op")
            where.append((c, op, value(c)))
        return data.draw(st.permutations(where), label="where order"), parts, run

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def select_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, parts, run = self.where_on_a_path(data, t)
        names = [c.name for c in t.columns]
        order: list[tuple] = []
        how = data.draw(st.sampled_from(["none", "path", "any"]), label="order?")
        if how == "any":
            # Any columns: mostly no path's order, so a sort.
            order = draw_order(data, names, "order")
        elif how == "path":
            # The parts after the prefix, each with or against its direction;
            # an equality-compared column now and then, which orders nothing.
            reverse = data.draw(st.booleans(), label="reverse")
            m = data.draw(st.integers(min(1, len(parts) - run), len(parts) - run), label="ordered parts")
            order = [(c, desc != reverse) for c, desc in parts[run:run + m]]
            if run and data.draw(st.integers(0, 3), label="an equal column?") == 0:
                order.insert(data.draw(st.integers(0, len(order)), label="at"), (parts[0][0], data.draw(st.booleans())))
            if order and data.draw(st.integers(0, 3), label="flip one?") == 0:
                i = data.draw(st.integers(0, len(order) - 1), label="flipped")
                order[i] = (order[i][0], not order[i][1])  # mostly no path's order: a sort
            if order and data.draw(st.integers(0, 2), label="NULLS?") == 0:
                i = data.draw(st.integers(0, len(order) - 1), label="placed")
                order[i] = (order[i][0], order[i][1], data.draw(st.booleans(), label="NULLS FIRST"))
        targets = data.draw(st.lists(st.sampled_from(names), min_size=1, max_size=3, unique=True), label="targets")
        targets += [c for c, *_ in order if c not in targets]
        limit = data.draw(st.none() | st.sampled_from([0, 1, 2, 5]), label="limit")
        self.select(t.name, targets, where, order, limit)

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def count_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, _, _ = self.where_on_a_path(data, t)
        arg = data.draw(st.sampled_from(["*"] + [c.name for c in t.columns]), label="count of")
        distinct = arg != "*" and data.draw(st.booleans(), label="DISTINCT")
        limit = data.draw(st.none() | st.sampled_from([0, 1]), label="limit")
        self.select(t.name, [], where, limit=limit, count=arg, distinct=distinct)

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def sum_or_min_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        fn = data.draw(st.sampled_from(["sum", "min"]), label="fn")
        cols = [c.name for c in t.columns if c.type.sql in (SUMMED if fn == "sum" else MINIMISED)]
        if not cols:
            return
        where, _, _ = self.where_on_a_path(data, t)
        arg = data.draw(st.sampled_from(cols), label=f"{fn} of")
        limit = data.draw(st.none() | st.sampled_from([0, 1]), label="limit")
        self.select(t.name, [], where, limit=limit, count=arg, fn=fn)

    @precondition(lambda self: any(c.name not in t.pk for t in self.tables.values() for c in t.columns))
    @rule(data=st.data())
    def update_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        cols = [c for c in t.columns if c.name not in t.pk]
        if not cols:
            return
        where, _, _ = self.where_on_a_path(data, t)
        col = data.draw(st.sampled_from(cols), label="set")
        # col ± $n on a column no unique index holds: one that moves a unique
        # value from row to row is a known difference.
        unique = any(ix.unique and col.name in [c for c, _ in ix.columns] for ix in t.indexes)
        op = "="
        if col.type.sql in ARITHMETIC and not unique:
            op = data.draw(st.sampled_from(["=", "+", "-"]), label="op")
        small = st.integers(-5, 5).map(lambda n: decimal.Decimal(n) if col.type.kind == "decimal" else n)
        value = data.draw(st.none() | (small if op != "=" else st.nothing()) |
                          (values(col.type) if op == "=" else operands(col.type)), label="value")
        self.write(t.name, where, (col.name, op, value))

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def delete_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, _, _ = self.where_on_a_path(data, t)
        self.write(t.name, where)

    @precondition(lambda self: any(c.type.sql in ARITHMETIC and c.name not in t.pk
                                   for t in self.tables.values() for c in t.columns))
    @rule(data=st.data())
    def add_to_column(self, data):
        choices = [(t.name, c.name) for t in self.tables.values() for c in t.columns
                   if c.type.sql in ARITHMETIC and c.name not in t.pk]
        name, col = data.draw(st.sampled_from(sorted(choices)), label="column")
        t = self.tables[name]
        keys = self.existing_keys(t)
        if keys and data.draw(st.integers(0, 4), label="existing key?"):
            key = data.draw(st.sampled_from(keys), label="key")
        else:
            key = tuple(data.draw(st.none() | values(t.column(c).type), label=c) for c in t.pk)
        ty = t.column(col).type
        small = st.integers(-5, 5).map(lambda n: decimal.Decimal(n) if ty.kind == "decimal" else n)
        operand = data.draw(st.none() | small | operands(ty), label="operand")
        self.add(name, col, data.draw(st.sampled_from(["+", "-"]), label="op"), operand, key)

    @precondition(lambda self: any(c.type.sql in ARITHMETIC and c.name not in t.pk
                                   for t in self.tables.values() for c in t.columns))
    @rule(data=st.data())
    def add_a_literal(self, data):
        choices = [(t.name, c.name) for t in self.tables.values() for c in t.columns
                   if c.type.sql in ARITHMETIC and c.name not in t.pk]
        name, col = data.draw(st.sampled_from(sorted(choices)), label="column")
        t = self.tables[name]
        keys = self.existing_keys(t)
        if keys and data.draw(st.integers(0, 4), label="existing key?"):
            key = data.draw(st.sampled_from(keys), label="key")
        else:
            key = tuple(data.draw(st.none() | values(t.column(c).type), label=c) for c in t.pk)
        # Integer literals, typed integer or, past it, bigint: small ones, and
        # ones at and past SMALLINT's and INTEGER's edges. 2^63 - 1 only on a
        # BIGINT: a SMALLINT, INTEGER or DECIMAL column refuses it before the
        # plan runs.
        edges = [32767, 32768, -32768, 2**31 - 1, 2**31, 2**40]
        if t.column(col).type is o.BIGINT:
            edges.append(2**63 - 1)
        literal = data.draw(st.one_of(st.integers(-5, 5), st.sampled_from(edges)), label="literal")
        self.add(name, col, data.draw(st.sampled_from(["+", "-"]), label="op"), literal, key, literal=True)

    @precondition(lambda self: len(self.tables) >= 2)
    @rule(data=st.data())
    def join_by_keys(self, data):
        # Two tables that hold rows, when there are two, so the pair exists.
        filled = [n for n in sorted(self.tables) if self.existing_keys(self.tables[n])]
        pool = filled if len(filled) >= 2 and data.draw(st.integers(0, 3), label="filled tables?") else sorted(self.tables)
        names = tuple(data.draw(st.permutations(pool), label="tables")[:2])
        sides = [self.tables[n] for n in names]
        keys = []
        for t in sides:
            existing = self.existing_keys(t)
            if existing and data.draw(st.integers(0, 4), label=f"{t.name} existing key?"):
                keys.append(data.draw(st.sampled_from(existing), label=f"{t.name} key"))
            else:
                keys.append(tuple(data.draw(st.none() | values(t.column(c).type), label=c) for c in t.pk))
        columns = [(t.name, c.name) for t in sides for c in t.columns]
        targets = data.draw(st.lists(st.sampled_from(columns), min_size=1, max_size=4, unique=True), label="targets")
        self.join(names, targets, (keys[0], keys[1]), on=data.draw(st.booleans(), label="JOIN ON"))

    @precondition(lambda self: len(self.tables) >= 2)
    @rule(data=st.data())
    def join_on_a_column(self, data):
        # b's key parts each from a column of a of its type, or a parameter.
        a_name, b_name = data.draw(st.permutations(sorted(self.tables)), label="tables")[:2]
        a, b = self.tables[a_name], self.tables[b_name]
        rows_b = self.existing_rows(b)
        row_b = data.draw(st.sampled_from(rows_b), label="b row") if rows_b and data.draw(st.booleans(), label="b row?") else None
        keys_b = []
        for c in b.pk:
            same = [x.name for x in a.columns if x.type.sql == b.column(c).type.sql]
            if same and data.draw(st.integers(0, 3), label=f"{c} joined?"):
                keys_b.append((c, ("col", data.draw(st.sampled_from(same), label=f"{c} on"))))
            else:
                v = row_b[[x.name for x in b.columns].index(c)] if row_b is not None else data.draw(values(b.column(c).type), label=c)
                keys_b.append((c, ("param", v)))
        if not any(kind == "col" for _, (kind, _) in keys_b):
            return
        where_a, _, _ = self.where_on_a_path(data, a)
        filters_b = []
        if data.draw(st.integers(0, 2), label="b filter?") == 0:
            c = data.draw(st.sampled_from([x.name for x in b.columns]), label="b filter")
            filters_b.append((c, data.draw(st.sampled_from(["=", "<", ">="]), label="b op"),
                              data.draw(st.none() | values(b.column(c).type), label="b value")))
        if data.draw(st.integers(0, 3), label="COUNT(DISTINCT)?") == 0:
            t = data.draw(st.sampled_from([a, b]), label="distinct of")
            self.join_on(a_name, b_name, where_a, keys_b, filters_b, [],
                         distinct=(t.name, data.draw(st.sampled_from([x.name for x in t.columns]), label="column")),
                         **self.draw_outer(data, b))
            return
        columns = [(t.name, x.name) for t in (a, b) for x in t.columns]
        targets = data.draw(st.lists(st.sampled_from(columns), min_size=1, max_size=3, unique=True), label="targets")
        order = self.join_order(data, columns, targets)
        self.join_on(a_name, b_name, where_a, keys_b, filters_b, targets,
                     limit=data.draw(st.none() | st.sampled_from([0, 1, 3]), label="limit"), order=order,
                     **self.draw_outer(data, b))

    @staticmethod
    def draw_outer(data, b: Table) -> dict:
        """Now and then the join outer: LEFT, or RIGHT with its sides
        written the other way round, the rows b matches none of alone now
        and then."""
        outer = data.draw(st.sampled_from([None, None, "LEFT", "RIGHT"]), label="outer join")
        return {"outer": outer, "anti": outer is not None and data.draw(st.integers(0, 3), label="anti join?") == 0}

    @staticmethod
    def join_order(data, columns: list[tuple[str, str]], targets: list[tuple[str, str]]) -> list[tuple]:
        """Now and then an ORDER BY of a join's columns, each of them added
        to the targets."""
        if data.draw(st.integers(0, 2), label="join ORDER BY?") != 0:
            return []
        picked = data.draw(st.lists(st.sampled_from(columns), min_size=1, max_size=2, unique=True), label="join order")
        order = [(t, c, data.draw(st.booleans(), label=f"{t}.{c} desc"),
                  data.draw(st.sampled_from([None, True, False]), label=f"{t}.{c} nulls")) for t, c in picked]
        targets += [p for p in picked if p not in targets]
        return order

    @precondition(lambda self: len(self.tables) >= 2)
    @rule(data=st.data())
    def join_read_once(self, data):
        # b named by its whole primary key in parameters, a scanned by
        # parameters, and an equality of columns neither key takes linking
        # them: b read first and a's scan once, or a's scan with a lookup of
        # b per row.
        a_name, b_name = data.draw(st.permutations(sorted(self.tables)), label="tables")[:2]
        a, b = self.tables[a_name], self.tables[b_name]
        pairs = [(x.name, y.name) for x in b.columns if x.name not in b.pk
                 for y in a.columns if y.name not in a.pk and y.type.sql == x.type.sql]
        self.read_once_linked(data, a, b, pairs)

    @precondition(lambda self: len(self.tables) >= 2)
    @rule(data=st.data())
    def join_read_once_on_keys(self, data):
        # As join_read_once, but linked on the scanned table's primary-key
        # columns, which an index of it holds as its key's suffix: a covering
        # read of them, when nothing else is read from its rows.
        a_name, b_name = data.draw(st.permutations(sorted(self.tables)), label="tables")[:2]
        a, b = self.tables[a_name], self.tables[b_name]
        pairs = [(x.name, y) for x in b.columns if x.name not in b.pk
                 for y in a.pk if a.column(y).type.sql == x.type.sql]
        self.read_once_linked(data, a, b, pairs)

    def read_once_linked(self, data, a: Table, b: Table, pairs: list[tuple[str, str]]) -> None:
        """b named by its whole primary key in parameters, a scanned by
        parameters, linked by equalities of b's columns with a's, each drawn
        from `pairs` (b's column, a's)."""
        a_name, b_name = a.name, b.name
        if not pairs:
            return
        links = data.draw(st.lists(st.sampled_from(pairs), min_size=1, max_size=2, unique=True), label="links")
        rows_b = self.existing_rows(b)
        row_b = data.draw(st.sampled_from(rows_b), label="b row") if rows_b and data.draw(st.integers(0, 3), label="b row?") else None
        keys_b = []
        for c in b.pk:
            v = row_b[[x.name for x in b.columns].index(c)] if row_b is not None else data.draw(values(b.column(c).type), label=c)
            keys_b.append((c, ("param", v)))
        where_a, _, _ = self.where_on_a_path(data, a)
        columns = [(t.name, x.name) for t in (a, b) for x in t.columns]
        if data.draw(st.integers(0, 3), label="COUNT(DISTINCT)?") == 0:
            t = data.draw(st.sampled_from([a, b]), label="distinct of")
            self.join_on(a_name, b_name, where_a, keys_b, [], [], links=links,
                         distinct=(t.name, data.draw(st.sampled_from([x.name for x in t.columns]), label="column")),
                         **self.draw_outer(data, b))
            return
        targets = data.draw(st.lists(st.sampled_from(columns), min_size=1, max_size=3, unique=True), label="targets")
        order = self.join_order(data, columns, targets)
        self.join_on(a_name, b_name, where_a, keys_b, [], targets, links=links,
                     limit=data.draw(st.none() | st.sampled_from([0, 1, 3]), label="limit"), order=order,
                     **self.draw_outer(data, b))

    @precondition(lambda self: len(self.tables) >= 2)
    @rule(data=st.data())
    def join_no_key(self, data):
        # Two tables, each scanned by parameters or in full, linked by up to
        # two equalities of columns of one type, key columns or not: mostly
        # no key reaches either, so the join is hashed, built on either side,
        # or one table read once per row of the other; with none, a cross
        # join, always the latter.
        a_name, b_name = data.draw(st.permutations(sorted(self.tables)), label="tables")[:2]
        a, b = self.tables[a_name], self.tables[b_name]
        pairs = [(x.name, y.name) for x in b.columns for y in a.columns if y.type.sql == x.type.sql]
        links = data.draw(st.lists(st.sampled_from(pairs), min_size=0, max_size=2, unique=True), label="links") if pairs else []
        where_a, _, _ = self.where_on_a_path(data, a)
        where_b, _, _ = self.where_on_a_path(data, b)
        columns = [(t.name, x.name) for t in (a, b) for x in t.columns]
        if data.draw(st.integers(0, 3), label="COUNT(DISTINCT)?") == 0:
            t = data.draw(st.sampled_from([a, b]), label="distinct of")
            self.join_on(a_name, b_name, where_a, [], where_b, [], links=links,
                         distinct=(t.name, data.draw(st.sampled_from([x.name for x in t.columns]), label="column")),
                         **self.draw_outer(data, b))
            return
        targets = data.draw(st.lists(st.sampled_from(columns), min_size=1, max_size=3, unique=True), label="targets")
        order = self.join_order(data, columns, targets)
        self.join_on(a_name, b_name, where_a, [], where_b, targets, links=links,
                     limit=data.draw(st.none() | st.sampled_from([0, 1, 3]), label="limit"), order=order,
                     **self.draw_outer(data, b))

    @precondition(lambda self: len(self.tables) >= 2 and self.txn is None)
    @rule(data=st.data())
    def insert_a_match(self, data):
        # A row of b whose column holds a value of a column of a, of its type,
        # from one of a's rows: duplicate join keys on both sides.
        a_name, b_name = data.draw(st.permutations(sorted(self.tables)), label="tables")[:2]
        a, b = self.tables[a_name], self.tables[b_name]
        rows = self.existing_rows(a)
        pairs = [(x.name, y) for x in b.columns for y, ya in enumerate(a.columns) if ya.type.sql == x.type.sql]
        if not rows or not pairs:
            return
        col, at = data.draw(st.sampled_from(pairs), label="column")
        row = {}
        for c in b.columns:
            if c.name == col:
                row[c.name] = data.draw(st.sampled_from(rows), label="a row")[at]
            elif b.nullable(c.name) and data.draw(st.integers(0, 4), label=f"{c.name} null?") == 0:
                row[c.name] = None
            else:
                row[c.name] = data.draw(values(c.type), label=c.name)
        self.insert(b_name, row, all_columns=True)

    @precondition(lambda self: self.txn is None)
    @rule()
    def reopen_layer(self):
        self.reopen()

    @precondition(lambda self: self.tables and self.txn is None)
    @rule()
    def begin_transaction(self):
        self.begin()

    @precondition(lambda self: self.txn is not None)
    @rule()
    def commit_transaction(self):
        self.commit()

    @precondition(lambda self: self.txn is not None)
    @rule()
    def rollback_transaction(self):
        self.rollback()

    @invariant()
    def tables_agree(self):
        if self.txn is None:  # a transaction's writes are its own until it commits
            self.check()


def machine(dsn: str) -> type[SqlMachine]:
    return type("SqlMachineWithOracle", (SqlMachine,), {"dsn": dsn})


def test_sql_machine(dsn):
    run_state_machine_as_test(machine(dsn))


# Tables without a primary key ------------------------------------------------

KEYLESS_NAMES = ["k0", "k1"]


class KeylessMachine(Sides, RuleBasedStateMachine):
    """Tables declared without a primary key, which PostgreSQL and the layer
    both take: every row inserted is kept, duplicates included, but where a
    UNIQUE column refuses them (23505), and UPDATE,
    DELETE, SELECT and GROUP BY reach them by full scans. Rows have no key
    to order them by, so a table, and each result, is compared as a bag of
    rows. Reopening the layer keeps the rows, and the inserts after it add
    rows rather than overwrite one."""

    def __init__(self):
        super().__init__()
        self.tables: dict[str, Table] = {}
        self.open_sides()

    def rows(self, t: Table) -> list[tuple]:
        """PostgreSQL's rows, in an order fixed by their values, so a draw
        from them is reproducible from its seed."""
        out = o.run_pg(self.pg.conn, f"{t.select_all} ORDER BY {', '.join(c.name for c in t.columns)}")
        assert out.error is None, out.detail
        return list(out.rows)

    def bag(self, sql: str, params, pg_sql: str, types) -> None:
        """A SELECT on both sides: the same rows, in any order."""
        layer = self.served(sql, params, self.layer(sql, params, types))
        pg = o.run_pg(self.pg.conn, pg_sql, params)
        assert layer.error == pg.error, f"{sql} {params!r}: the layer {layer.error} ({layer.detail}), PostgreSQL {pg.error}"
        assert collections.Counter(o.strict(layer.rows)) == collections.Counter(o.strict(pg.rows)), (
            f"{sql} {params!r}: the layer returned {layer.rows}, PostgreSQL {pg.rows}")

    @invariant()
    def check(self) -> None:
        catalog = {t["name"]: t for t in self.db.catalog()}
        assert sorted(catalog) == sorted(self.tables), f"the layer's tables are {sorted(catalog)}, expected {sorted(self.tables)}"
        for t in self.tables.values():
            # Its columns, and no primary key: the row id is no column.
            assert [c["name"] for c in catalog[t.name]["columns"]] == [c.name for c in t.columns]
            assert catalog[t.name]["primary_key"] == []
            layer = self.db.table_rows(t.name)["rows"]
            pg = o.run_pg(self.pg.conn, t.select_all)
            assert collections.Counter(o.strict(layer)) == collections.Counter(o.strict(pg.rows)), (
                f"table {t.name}: the layer holds {layer}, PostgreSQL {list(pg.rows)}")

    def draw_table(self, data) -> Table:
        return self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]

    @staticmethod
    def filterable(t: Table) -> list[str]:
        """The columns a filter may compare: not a DOUBLE or REAL, which the
        planner does not filter on (docs/frontend_design.md, *Known
        differences*)."""
        return [c.name for c in t.columns if c.type.kind not in ("float", "real")]

    def draw_where(self, data, t: Table, first: int) -> tuple[str, str, list, list] | None:
        """No WHERE, or col = $first, or col = a literal, on a filterable
        column, its value often one a row holds: the layer's text,
        PostgreSQL's, the parameters and their types. None for a literal
        the column's type does not hold exactly, which the layer refuses
        (o.exact_literal), having checked it does."""
        cols = self.filterable(t)
        if not cols or data.draw(st.integers(0, 3), label="no where?") == 0:
            return "", "", [], []
        c = data.draw(st.sampled_from(cols), label="where column")
        held = [r[[x.name for x in t.columns].index(c)] for r in self.rows(t)]
        v = data.draw(st.sampled_from(held) if held and data.draw(st.booleans(), label="held?") else values(t.column(c).type),
                      label="where value")
        ty = t.column(c).type
        if data.draw(st.integers(0, 2), label="predicate?") == 0:
            pred = self.draw_predicate(data, t, depth=2)
            return f" WHERE {pred}", f" WHERE {pred}", [], []
        if data.draw(st.booleans(), label="literal?"):
            where = f" WHERE {c} = {o.sql_literal(ty, v)}"
            if not o.exact_literal(ty, v):
                out = self.layer(f"SELECT {c} FROM {t.name}{where}")
                assert out.error == "refused", f"{where}: a literal its column does not hold was {out.error or 'accepted'}"
                return None
            return where, where, [], []
        return f" WHERE {c} = ${first}", f" WHERE {c} = {typed(t, c, first)}", [v], [ty]

    @staticmethod
    def draw_pattern(data, held: list[str]) -> str:
        """A LIKE pattern, mostly made from a value a row holds: the whole of
        it, a prefix, a suffix or a middle with % around, now and then a
        character as _; its own %, _ and backslash escaped. Sometimes any
        text, % and _ among it."""
        if not held or data.draw(st.integers(0, 4), label="free pattern?") == 0:
            return data.draw(st.text(alphabet="ab%_\\é", max_size=5), label="pattern")
        v = data.draw(st.sampled_from(held), label="pattern source")
        i = data.draw(st.integers(0, len(v)), label="cut from")
        j = data.draw(st.integers(i, len(v)), label="cut to")
        piece = "".join("\\" + ch if ch in "%_\\" else ch for ch in v[i:j])
        if piece and data.draw(st.booleans(), label="one as _?"):
            k = data.draw(st.integers(0, len(v[i:j]) - 1), label="which _")
            chars = ["\\" + ch if ch in "%_\\" else ch for ch in v[i:j]]
            chars[k] = "_"
            piece = "".join(chars)
        return ("%" if i > 0 else "") + piece + ("%" if j < len(v) else "")

    def draw_predicate(self, data, t: Table, depth: int) -> str:
        """A predicate on filterable columns, as SQL both sides read alike:
        AND, OR and NOT over =, <>, IS [NOT] NULL, [NOT] BETWEEN and [NOT]
        IN, of literals the table holds or NULL, so each is one its column
        holds exactly."""
        if depth > 0 and data.draw(st.integers(0, 2), label="combine?") == 0:
            op = data.draw(st.sampled_from(["AND", "OR", "NOT"]), label="combinator")
            if op == "NOT":
                return f"NOT ({self.draw_predicate(data, t, depth - 1)})"
            return f"({self.draw_predicate(data, t, depth - 1)}) {op} ({self.draw_predicate(data, t, depth - 1)})"
        c = data.draw(st.sampled_from(self.filterable(t)), label="predicate column")
        ty = t.column(c).type
        held = [r[[x.name for x in t.columns].index(c)] for r in self.rows(t)]
        value = st.sampled_from(held + [None]) if held else st.just(None)
        lit = lambda label: o.sql_literal(ty, data.draw(value, label=label))  # noqa: E731
        forms = ["=", "<>", "IS NULL", "IS NOT NULL", "BETWEEN", "NOT BETWEEN", "IN", "NOT IN"]
        if ty.kind == "text":
            forms += ["LIKE", "NOT LIKE"]
        form = data.draw(st.sampled_from(forms), label="predicate form")
        if form in ("LIKE", "NOT LIKE"):
            return f"{c} {form} {o.sql_literal(ty, self.draw_pattern(data, [h for h in held if h is not None]))}"
        if form in ("IS NULL", "IS NOT NULL"):
            return f"{c} {form}"
        if form in ("BETWEEN", "NOT BETWEEN"):
            sym = " SYMMETRIC" if data.draw(st.booleans(), label="symmetric?") else ""
            return f"{c} {form}{sym} {lit('lo')} AND {lit('hi')}"
        if form in ("IN", "NOT IN"):
            n = data.draw(st.integers(1, 3), label="in size")
            return f"{c} {form} ({', '.join(lit(f'in {i}') for i in range(n))})"
        return f"{c} {form} {lit('operand')}"

    @precondition(lambda self: len(self.tables) < len(KEYLESS_NAMES))
    @rule(data=st.data())
    def create_table(self, data):
        free = [n for n in KEYLESS_NAMES if n not in self.tables]
        n = data.draw(st.integers(1, 3), label="columns")
        # Now and then UNIQUE: inserts copying a row, and UPDATEs giving rows
        # one value, are then 23505 on both, NULLs never.
        cols = [Column(f"c{i}", data.draw(st.sampled_from(TYPES), label=f"c{i} type"),
                       nullable=data.draw(st.booleans(), label=f"c{i} nullable"),
                       unique=data.draw(st.integers(0, 3), label=f"c{i} unique") == 0) for i in range(n)]
        t = Table(data.draw(st.sampled_from(free), label="name"), cols, [])
        if self.both(t.ddl(pg=False), pg_sql=t.ddl(pg=True)).error is None:
            self.tables[t.name] = t

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def drop_table(self, data):
        t = self.draw_table(data)
        assert self.both(f"DROP TABLE {t.name}").error is None
        del self.tables[t.name]

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def insert(self, data):
        """A row, often a copy of one the table holds; all its columns, or
        some, the rest NULL."""
        t = self.draw_table(data)
        held = self.rows(t)
        if held and data.draw(st.integers(0, 2), label="duplicate?") == 0:
            row = dict(zip((c.name for c in t.columns), data.draw(st.sampled_from(held), label="copied row")))
        else:
            row = {c.name: data.draw(st.none() | values(c.type), label=c.name) for c in t.columns}
        all_columns = data.draw(st.booleans(), label="all columns?")
        if not all_columns:
            keep = data.draw(st.lists(st.sampled_from(list(row)), min_size=1, unique=True), label="given")
            row = {c: v for c, v in row.items() if c in keep}
        cols = list(row)
        target = "" if all_columns else f" ({', '.join(cols)})"
        if data.draw(st.booleans(), label="literals?"):
            values_text = ", ".join(o.sql_literal(t.column(c).type, row[c]) for c in cols)
            self.both(f"INSERT INTO {t.name}{target} VALUES ({values_text})")
            return
        marks = ", ".join(f"${i + 1}" for i in range(len(cols)))
        self.both(f"INSERT INTO {t.name}{target} VALUES ({marks})", [row[c] for c in cols],
                  types=[t.column(c).type for c in cols])

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def update(self, data):
        t = self.draw_table(data)
        c = data.draw(st.sampled_from([x.name for x in t.columns]), label="set column")
        v = data.draw(st.none() | values(t.column(c).type), label="set value")
        drawn = self.draw_where(data, t, 2)
        if drawn is None:
            return
        where, pg_where, params, types = drawn
        if data.draw(st.booleans(), label="literal set?"):
            # The WHERE's parameter, if any, is $2: renumbered as $1.
            lit = o.sql_literal(t.column(c).type, v)
            self.both(f"UPDATE {t.name} SET {c} = {lit}{where.replace('$2', '$1')}", params,
                      pg_sql=f"UPDATE {t.name} SET {c} = {lit}{pg_where.replace('$2', '$1')}", types=types)
            return
        self.both(f"UPDATE {t.name} SET {c} = $1{where}", [v, *params], pg_sql=f"UPDATE {t.name} SET {c} = $1{pg_where}",
                  types=[t.column(c).type, *types])

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def delete(self, data):
        t = self.draw_table(data)
        drawn = self.draw_where(data, t, 1)
        if drawn is None:
            return
        where, pg_where, params, types = drawn
        self.both(f"DELETE FROM {t.name}{where}", params, pg_sql=f"DELETE FROM {t.name}{pg_where}", types=types)

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def select(self, data):
        t = self.draw_table(data)
        cols = data.draw(st.lists(st.sampled_from([c.name for c in t.columns]), min_size=1, unique=True), label="selected")
        drawn = self.draw_where(data, t, 1)
        if drawn is None:
            return
        where, pg_where, params, types = drawn
        # DISTINCT now and then: one row per distinct combination, NULL one
        # value. Not of a float column, whose group shows -0.0 or 0.0, as
        # each side happens to meet them.
        floats = any(t.column(c).type.kind in ("float", "real") for c in cols)
        distinct = "DISTINCT " if not floats and data.draw(st.integers(0, 2), label="distinct?") == 0 else ""
        self.bag(f"SELECT {distinct}{', '.join(cols)} FROM {t.name}{where}", params,
                 f"SELECT {distinct}{', '.join(cols)} FROM {t.name}{pg_where}", types)

    @precondition(lambda self: any(KeylessMachine.filterable(t) for t in self.tables.values()))
    @rule(data=st.data())
    def count_groups(self, data):
        """COUNT(*) per value of a column: duplicates are counted, never merged."""
        t = self.tables[data.draw(st.sampled_from(sorted(n for n, t in self.tables.items() if self.filterable(t))),
                                  label="table")]
        c = data.draw(st.sampled_from(self.filterable(t)), label="group column")
        sql = f"SELECT {c}, COUNT(*) FROM {t.name} GROUP BY {c}"
        self.bag(sql, [], sql, [])

    @rule()
    def reopen_layer(self):
        self.reopen()


def keyless_machine(dsn: str) -> type[KeylessMachine]:
    return type("KeylessMachineWithOracle", (KeylessMachine,), {"dsn": dsn})


def test_keyless_machine(dsn):
    run_state_machine_as_test(keyless_machine(dsn))


def test_a_unique_column_refuses_a_duplicate_and_takes_nulls(dsn):
    """A UNIQUE constraint is a unique index: 23505 on both for a value a
    row holds, by INSERT or UPDATE; any number of NULLs; -0.0 is 0.0."""
    m = keyless_machine(dsn)()
    try:
        t = Table("k0", [Column("c0", o.INTEGER, unique=True), Column("c1", o.DOUBLE, unique=True)], [])
        assert m.both(t.ddl(pg=False), pg_sql=t.ddl(pg=True)).error is None
        m.tables[t.name] = t
        ins = "INSERT INTO k0 VALUES ($1, $2)"
        types = [o.INTEGER, o.DOUBLE]
        assert m.both(ins, [1, 0.0], types=types).error is None
        assert m.both(ins, [1, 1.0], types=types).error == "23505"
        assert m.both(ins, [2, -0.0], types=types).error == "23505"
        assert m.both(ins, [None, None], types=types).error is None
        assert m.both(ins, [None, None], types=types).error is None
        assert m.both(ins, [3, 3.0], types=types).error is None
        assert m.both("UPDATE k0 SET c0 = 1 WHERE c0 = 3").error == "23505"
        m.reopen()
        assert m.both("INSERT INTO k0 VALUES (3, 4.0)").error == "23505"
        m.check()
    finally:
        m.teardown()


def test_like_agrees_with_postgresql_on_utf8_and_escapes(dsn):
    """LIKE and NOT LIKE as PostgreSQL matches them: _ one character, not one
    byte; % any run, backtracking; the escape, default and given; NULL
    unknown."""
    m = keyless_machine(dsn)()
    try:
        t = Table("k0", [Column("c0", o.TEXT), Column("c1", o.varchar(8))], [])
        assert m.both(t.ddl(pg=False), pg_sql=t.ddl(pg=True)).error is None
        m.tables[t.name] = t
        for row in (["h\u00e9llo", "\u00e9t\u00e9"], ["hello", "ete"], ["a_b", "100%"], ["a\\b", "x"],
                    ["mississippi", "\u00fc\u00fc"], [None, None], ["", ""]):
            assert m.both("INSERT INTO k0 VALUES ($1, $2)", row, types=[o.TEXT, o.varchar(8)]).error is None
        for where in ("c0 LIKE 'h_llo'", "c0 LIKE 'h__llo'", "c0 LIKE '%\u00e9%'", "c1 LIKE '_t_'",
                      "c1 LIKE '\u00e9%'", "c1 LIKE '__'", "c0 LIKE 'm%iss%ppi'", "c0 LIKE '%s%s%s%s%'",
                      "c0 LIKE 'a\\_b'", "c1 LIKE '100\\%'", "c1 LIKE '100!%' ESCAPE '!'",
                      "c0 LIKE 'a\\b' ESCAPE ''", "c0 LIKE ''", "c0 NOT LIKE '%l%'", "c0 LIKE 'x\\'",
                      "c0 LIKE '%' OR c1 IS NULL"):
            sql = f"SELECT c0, c1 FROM k0 WHERE {where}"
            m.bag(sql, [], sql, [])
        m.check()
    finally:
        m.teardown()


def test_a_table_without_a_primary_key_keeps_duplicates_across_a_reopen(dsn):
    """Row ids after a reopen start past the rows the data holds: an insert
    then adds a row, never collides with one (23505) or replaces it."""
    m = keyless_machine(dsn)()
    try:
        t = Table("k0", [Column("c0", o.INTEGER), Column("c1", o.TEXT)], [])
        assert m.both(t.ddl(pg=False), pg_sql=t.ddl(pg=True)).error is None
        m.tables[t.name] = t
        for row in ([1, "a"], [1, "a"], [None, "b"]):
            assert m.both("INSERT INTO k0 VALUES ($1, $2)", row, types=[o.INTEGER, o.TEXT]).error is None
        m.reopen()
        for row in ([1, "a"], [2, None]):
            assert m.both("INSERT INTO k0 VALUES ($1, $2)", row, types=[o.INTEGER, o.TEXT]).error is None
        assert m.both("DELETE FROM k0 WHERE c0 = $1", [1], types=[o.INTEGER]).affected == 3
        m.check()
    finally:
        m.teardown()


# Fixed cases: the machine's steps in a set order. ---------------------------


def fixed(dsn) -> SqlMachine:
    return machine(dsn)()


def test_duplicate_primary_key_is_23505(dsn):
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.TEXT)], ["c0"]))
        assert m.insert("t0", {"c0": 1, "c1": "a"}, all_columns=False).error is None
        assert m.insert("t0", {"c0": 1, "c1": "b"}, all_columns=False).error == "23505"
        m.check()
    finally:
        m.teardown()


def test_null_for_not_null_is_23502_and_defaults_fill_the_rest(dsn):
    m = fixed(dsn)
    try:
        cols = [Column("c0", o.INTEGER), Column("c1", o.varchar(4), nullable=False, default="ab"),
                Column("c2", o.BOOLEAN, nullable=False)]
        m.create(Table("t0", cols, ["c0"]))
        assert m.insert("t0", {"c0": 1, "c1": None, "c2": True}, all_columns=False).error == "23502"
        assert m.insert("t0", {"c0": 1, "c2": False}, all_columns=False).error is None
        # c2 left unset, NOT NULL with no default: 23502 on both.
        assert m.insert("t0", {"c0": 2}, all_columns=False).error == "23502"
        m.check()
    finally:
        m.teardown()


def test_composite_key_lookup_and_reopen(dsn):
    m = fixed(dsn)
    try:
        cols = [Column("c0", o.TEXT), Column("c1", o.BYTEA), Column("c2", o.DOUBLE)]
        m.create(Table("t1", cols, ["c1", "c0"]))
        m.insert("t1", {"c0": "é", "c1": b"\x00", "c2": 0.5}, all_columns=True)
        m.insert("t1", {"c0": "e", "c1": b"\x00", "c2": None}, all_columns=True)
        assert len(m.lookup("t1", (b"\x00", "é")).rows) == 1
        assert m.lookup("t1", (None, "é")).rows == ()
        m.reopen()
        m.check()
    finally:
        m.teardown()


def test_a_transaction_reads_its_writes_and_commits_them(dsn):
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.TEXT)], ["c0"]))
        m.insert("t0", {"c0": 1, "c1": "a"}, all_columns=False)
        m.begin()
        assert m.insert("t0", {"c0": 2, "c1": "b"}, all_columns=False).error is None
        assert m.lookup("t0", (2,)).rows == ((2, "b"),)
        assert m.insert("t0", {"c0": 2, "c1": "c"}, all_columns=False).error == "23505"
        m.rollback()
        m.check()
        assert m.lookup("t0", (2,)).rows == ()
        m.begin()
        m.insert("t0", {"c0": 3, "c1": "c"}, all_columns=False)
        m.insert("t0", {"c0": 4, "c1": None}, all_columns=False)
        assert m.commit() == "COMMIT"
        m.check()
    finally:
        m.teardown()


def test_every_statement_after_a_failed_one_is_25P02(dsn):
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.BIGINT, nullable=False)], ["c0"]))
        m.begin()
        m.insert("t0", {"c0": 1, "c1": 1}, all_columns=False)
        assert m.insert("t0", {"c0": 2}, all_columns=False).error == "23502"
        assert m.lookup("t0", (1,)).error == "25P02"
        assert m.insert("t0", {"c0": 3, "c1": 3}, all_columns=False).error == "25P02"
        assert m.insert("t9", {"c0": 3}, all_columns=False).error == "25P02"  # refused or not, it is not run
        assert m.commit() == "ROLLBACK"  # as PostgreSQL's COMMIT of an aborted transaction
        m.check()
        # A statement refused before it runs aborts a transaction too.
        m.begin()
        assert m.insert("t9", {"c0": 3}, all_columns=False).error == "42P01"
        assert m.lookup("t0", (1,)).error == "25P02"
        m.rollback()
    finally:
        m.teardown()


def test_join_using_and_natural_join(dsn):
    """USING and NATURAL: the pairs equal on the named columns, NULL never
    equal, each named alone the left side's."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.INTEGER), Column("c2", o.TEXT)], ["c0"]))
        m.create(Table("t1", [Column("c3", o.BIGINT), Column("c1", o.INTEGER), Column("c2", o.TEXT)], ["c3"]))
        for row in ({"c0": 1, "c1": 1, "c2": "x"}, {"c0": 2, "c1": 2, "c2": "y"}, {"c0": 3, "c1": None, "c2": "z"}):
            assert m.insert("t0", row, all_columns=True).error is None
        for row in ({"c3": 10, "c1": 1, "c2": "x"}, {"c3": 11, "c1": 2, "c2": "n"}, {"c3": 12, "c1": None, "c2": "z"},
                    {"c3": 13, "c1": 1, "c2": "x"}):
            assert m.insert("t1", row, all_columns=True).error is None
        for sql in ("SELECT c1, t0.c0, t1.c3 FROM t0 JOIN t1 USING (c1) ORDER BY t0.c0, t1.c3",
                    "SELECT c1, c2, t1.c3 FROM t0 INNER JOIN t1 USING (c2, c1) ORDER BY t1.c3",
                    "SELECT t0.c0, t1.c3 FROM t0 NATURAL JOIN t1 ORDER BY t0.c0, t1.c3",
                    "SELECT j.c1 FROM t0 JOIN t1 USING (c1) AS j WHERE c1 = 2",
                    "SELECT c1, COUNT(*) FROM t0 JOIN t1 USING (c1) GROUP BY c1 ORDER BY c1"):
            assert m.both(sql).error is None
        assert m.both("SELECT c2 FROM t0 JOIN t1 USING (c1)").error == "42702"
        assert m.both("SELECT c0 FROM t0 JOIN t1 USING (c3)").error == "42703"
        m.check()
    finally:
        m.teardown()


def test_left_and_right_join_null_extend_the_rows_nothing_matches(dsn):
    """LEFT and RIGHT JOIN: each row of the preserved side, NULL-extended
    where ON matches nothing; WHERE filters the joined rows; a USING
    column alone is the preserved side's."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.INTEGER), Column("c2", o.TEXT)], ["c0"]))
        m.create(Table("t1", [Column("c3", o.BIGINT), Column("c1", o.INTEGER, nullable=False), Column("c2", o.TEXT)],
                       ["c3"]))
        for row in ({"c0": 1, "c1": 1, "c2": "x"}, {"c0": 2, "c1": 2, "c2": "y"}, {"c0": 3, "c1": None, "c2": "z"},
                    {"c0": 4, "c1": 4, "c2": None}):
            assert m.insert("t0", row, all_columns=True).error is None
        for row in ({"c3": 10, "c1": 1, "c2": "x"}, {"c3": 11, "c1": 2, "c2": "n"}, {"c3": 12, "c1": 5, "c2": "z"},
                    {"c3": 13, "c1": 1, "c2": "x"}):
            assert m.insert("t1", row, all_columns=True).error is None
        for sql in ("SELECT t0.c0, t1.c3 FROM t0 LEFT JOIN t1 ON t0.c1 = t1.c1 ORDER BY t0.c0, t1.c3",
                    "SELECT t0.c0, t1.c3 FROM t1 RIGHT JOIN t0 ON t0.c1 = t1.c1 AND t1.c2 = 'x' ORDER BY t0.c0, t1.c3",
                    "SELECT t0.c0, t1.c3 FROM t0 LEFT JOIN t1 ON t0.c1 = t1.c1 AND t0.c2 = 'x' ORDER BY t0.c0, t1.c3",
                    "SELECT t0.c0 FROM t0 LEFT JOIN t1 ON t0.c1 = t1.c1 WHERE t1.c3 IS NULL ORDER BY t0.c0",
                    "SELECT t0.c0, t1.c3 FROM t0 LEFT JOIN t1 ON t0.c1 = t1.c1 WHERE t1.c2 = 'x' OR t0.c0 > 2 ORDER BY t0.c0, t1.c3",
                    "SELECT c1, t0.c0, t1.c3 FROM t0 LEFT JOIN t1 USING (c1) ORDER BY t0.c0, t1.c3",
                    "SELECT c1, t0.c0, t1.c3 FROM t0 RIGHT JOIN t1 USING (c1) ORDER BY t1.c3",
                    "SELECT t0.c0, t1.c3 FROM t0 NATURAL LEFT JOIN t1 ORDER BY t0.c0",
                    "SELECT t0.c0, COUNT(t1.c1), COUNT(*) FROM t0 LEFT JOIN t1 ON t0.c1 = t1.c1 GROUP BY t0.c0 ORDER BY t0.c0",
                    "SELECT t1.c1, COUNT(*) FROM t0 LEFT JOIN t1 ON t0.c1 = t1.c1 GROUP BY t1.c1 ORDER BY t1.c1"):
            assert m.both(sql).error is None
        m.check()
    finally:
        m.teardown()


def test_create_and_drop_refusals(dsn):
    m = fixed(dsn)
    try:
        t = Table("t0", [Column("c0", o.SMALLINT)], ["c0"])
        assert m.create(t).error is None
        assert m.create(t).error == "42P07"  # already exists
        assert m.create(Table("t1", [Column("c0", o.BIGINT), Column("c0", o.TEXT)], ["c0"])).error == "42701"
        assert m.create(Table("t1", [Column("c0", o.BIGINT)], ["zz"])).error == "42703"
        assert m.both(Table("t1", [Column("c0", o.BIGINT)], []).ddl(pg=False)).error is None  # no key: a row id
        assert m.both("DROP TABLE t1").error is None
        assert m.drop("t2", if_exists=False).error == "42P01"
        assert m.drop("t2", if_exists=True).error is None
        assert m.drop("t0", if_exists=False).error is None
        assert m.insert("t0", {"c0": 1}, all_columns=False).error == "42P01"
        m.check()
    finally:
        m.teardown()


def test_insert_without_a_primary_key_column(dsn):
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.BIGINT)], ["c0", "c1"]))
        assert m.insert("t0", {"c0": 1}, all_columns=False).error == "23502"
        m.check()
    finally:
        m.teardown()


def test_values_outside_their_declared_type_are_22003_and_22001(dsn):
    m = fixed(dsn)
    try:
        cols = [Column("c0", o.BIGINT), Column("c1", o.SMALLINT), Column("c2", o.INTEGER), Column("c3", o.varchar(4)),
                Column("c4", o.BOOLEAN, nullable=False, default=False)]
        m.create(Table("t0", cols, ["c0"]))
        # A row also NULL in a NOT NULL column: the type check comes first on both.
        assert m.insert("t0", {"c0": 1, "c1": 2**15, "c4": None}, all_columns=False).error == "22003"
        assert m.insert("t0", {"c0": None, "c3": "abcde"}, all_columns=False).error == "22001"
        for col, value, state in [("c1", 2**15, "22003"), ("c1", -(2**15) - 1, "22003"), ("c2", 2**31, "22003"),
                                  ("c3", "abcde", "22001"), ("c3", "ééééé", "22001")]:
            assert m.insert("t0", {"c0": 1, col: value}, all_columns=False).error == state, (col, value)
        assert m.insert("t0", {"c0": 1, "c1": 2**15 - 1, "c2": -(2**31), "c3": "éééé"}, all_columns=False).error is None
        assert m.lookup("t0", (2**40,)).rows == ()  # a BIGINT key: inside its type, found or not
        m.check()
    finally:
        m.teardown()


def test_real_is_stored_and_keyed_as_its_float(dsn):
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.REAL), Column("c1", o.REAL)], ["c0"]))
        assert m.insert("t0", {"c0": 0.1, "c1": 0.1}, all_columns=False).error is None
        assert m.insert("t0", {"c0": float(0.1), "c1": 1e39}, all_columns=False).error == "22003"
        assert len(m.lookup("t0", (0.1,)).rows) == 1  # found by the value it was written with
        assert m.insert("t0", {"c0": 0.1}, all_columns=False).error == "23505"
        m.check()
    finally:
        m.teardown()


def test_a_key_outside_its_type_is_refused(dsn):
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER), Column("c1", o.varchar(2))], ["c0", "c1"]))
        assert m.insert("t0", {"c0": 1, "c1": "ab"}, all_columns=False).error is None
        assert m.lookup("t0", (2**31, "ab")).error == "22003"  # not an INTEGER: refused, not "no row"
        assert m.lookup("t0", (1, "abc")).rows == ()  # longer than VARCHAR(2): a modifier, so no row
        m.create(Table("t1", [Column("c0", o.REAL)], ["c0"]))
        assert m.insert("t1", {"c0": 0.0}, all_columns=False).error is None
        assert m.lookup("t1", (1e-50,)).error == "22003"  # would round to the 0.0 row
        m.check()
    finally:
        m.teardown()


def test_a_parameter_compared_with_two_types_is_refused(dsn):
    # Known difference. PostgreSQL gets the value as one float8 parameter
    # (pg_params maps the reused $1 to one named parameter), compares the
    # REAL column with it as a float8, where inf is not 1e39, and returns no
    # row; the layer refuses a $n compared with columns of two SQL types,
    # where it would have keyed 1e39 for the REAL column as infinity and
    # returned the inf row.
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.DOUBLE), Column("c1", o.REAL)], ["c0", "c1"]))
        assert m.insert("t0", {"c0": 1e39, "c1": float("inf")}, all_columns=False).error is None
        sql = "SELECT c0 FROM t0 WHERE c0 = $1 AND c1 = $1"
        assert o.run_pg(m.pg.conn, sql, (1e39,)).rows == ()
        assert o.run_layer(m.db, sql, (1e39,)).error == "refused"
        m.check()
    finally:
        m.teardown()


def test_a_planner_refusal_never_agrees_with_postgresql(dsn):
    # PostgreSQL fails the statement with a state the oracle calls
    # "refused" (22P02); the planner refuses it with no plan. Were both
    # "refused", a wrong "served" prediction would pass: the step fails.
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT), Column("c1", o.DOUBLE)], ["c0"]))
        sql = "SELECT c0 FROM t0 WHERE c1 = $1"
        pg_sql = "SELECT 'x'::bigint"
        assert o.run_pg(m.pg.conn, pg_sql).error == "refused"
        assert o.is_no_plan(o.run_layer(m.db, sql, (1.0,)))
        with pytest.raises(AssertionError, match="predicted served, the planner refused it"):
            m.both(sql, (1.0,), pg_sql=pg_sql)
    finally:
        m.teardown()


def test_two_primary_keys_are_refused(dsn):
    m = fixed(dsn)
    try:
        cols = [Column("c0", o.BIGINT), Column("c1", o.BIGINT)]
        assert m.create(Table("t0", cols, ["c0", "c1"], pk_style="column")).error == "refused"
        assert m.create(Table("t0", cols, ["c0", "c1"], pk_style="both")).error == "refused"
        assert m.create(Table("t0", cols, ["c1"], pk_style="column")).error is None
        m.check()
    finally:
        m.teardown()


# The module and the comparison, without PostgreSQL. ------------------------


def test_calls_after_close_raise_closed(tmp_path):
    db = bytecask_sql.open(str(tmp_path))
    db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)")
    db.close()
    for call in (lambda: db.execute("SELECT k FROM t WHERE k = $1", (1,)), db.catalog, lambda: db.table_rows("t")):
        try:
            call()
        except bytecask_sql.Closed as e:
            assert isinstance(e, ValueError) and isinstance(e, bytecask_sql.Error)
        else:
            raise AssertionError("a call after close() was allowed")
    db.close()  # a second close does nothing


def test_a_conflict_on_every_retry_sets_conflict(tmp_path):
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)")
        db._conflict_on_every_commit(True)
        try:
            db.execute("INSERT INTO t VALUES ($1)", (1,))
        except bytecask_sql.ExecError as e:
            assert e.conflict and e.constraint is None
        else:
            raise AssertionError("a statement conflicting on every retry committed")
        db._conflict_on_every_commit(False)
        assert db.table_rows("t")["rows"] == []
        try:
            db.execute("INSERT INTO t VALUES ($1)", (1,))
            db.execute("INSERT INTO t VALUES ($1)", (1,))
        except bytecask_sql.ExecError as e:
            assert not e.conflict and e.constraint == "unique"
        else:
            raise AssertionError("a duplicate key was written")
    finally:
        db.close()


def test_text_that_is_not_utf8_is_refused_on_write(tmp_path):
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY, s TEXT)")
        try:
            db.execute("INSERT INTO t VALUES ($1, $2)", (1, b"\xff"))  # text is bytes to the plan
        except bytecask_sql.ExecError as e:
            assert e.constraint == "range" and o.layer_sqlstate(e) == "22021"
        else:
            raise AssertionError("bytes that are not UTF-8 were written as text")
        assert db.table_rows("t")["rows"] == []
    finally:
        db.close()


def test_stored_text_that_is_not_utf8_raises_on_read(tmp_path):
    """The read path's strict decode: text the layer would refuse, stored
    under it through the engine, raises rather than reads as text."""
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY, s TEXT)")
        db.execute("INSERT INTO t VALUES ($1, $2)", (1, "a"))
        [(key, row)] = db._engine_rows("t")
        assert row.endswith(b"a")  # the one variable column's bytes end the row
        db._engine_put(key, row[:-1] + b"\xff")
        for read in (lambda: db.table_rows("t"), lambda: db.execute("SELECT s FROM t WHERE k = $1", (1,))):
            try:
                read()
            except UnicodeDecodeError:
                pass
            else:
                raise AssertionError("stored bytes that are not UTF-8 read as text")
    finally:
        db.close()


def test_every_range_message_maps_to_its_sqlstate(tmp_path):
    """Each range refusal the layer makes, and the SQLSTATE it reports: every
    message start the layer maps (bytecask_sql._range_messages) is produced,
    so a message reworded in the layer fails here."""
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE r (k BIGINT PRIMARY KEY, s SMALLINT, i INTEGER, f REAL, d NUMERIC(4, 2), "
                   "v VARCHAR(2), x TEXT, dt DATE, ts TIMESTAMP, tm TIME)")
        cols = ["s", "i", "f", "d", "v", "x", "dt", "ts", "tm"]
        cases = [
            ("s", 2**15, "22003"),
            ("i", 2**31, "22003"),
            ("f", 1e39, "22003"),
            ("f", 1e-50, "22003"),
            ("d", 10_000, "22003"),
            ("v", "abc", "22001"),
            ("v", b"\xc3", "22021"),
            ("x", b"\x00", "22021"),
            ("dt", -2_440_589, "22008"),
            ("ts", -210_866_803_200_000_001, "22008"),
            ("tm", 86_400_000_001, "22008"),
        ]
        messages = bytecask_sql._range_messages()
        seen = set()
        for col, value, state in cases:
            params = [1] + [value if c == col else None for c in cols]
            marks = ", ".join(f"${n + 1}" for n in range(len(params)))
            try:
                db.execute(f"INSERT INTO r VALUES ({marks})", tuple(params))
            except bytecask_sql.ExecError as e:
                assert e.constraint == "range", (col, value, str(e))
                assert o.layer_sqlstate(e) == state, (col, value, str(e))
                seen.update(start for start, _ in messages if str(e).startswith(start))
            else:
                raise AssertionError(f"{col} = {value!r} was written")
        # A BIGINT leaves its range only in arithmetic, never in a stored value.
        db.execute("CREATE TABLE rb (k BIGINT PRIMARY KEY, b BIGINT)")
        db.execute("INSERT INTO rb VALUES ($1, $2)", (1, BIGINT_HIGH))
        try:
            db.execute("UPDATE rb SET b = b + $1 WHERE k = $2", (1, 1))
        except bytecask_sql.ExecError as e:
            assert e.constraint == "range" and o.layer_sqlstate(e) == "22003", str(e)
            seen.update(start for start, _ in messages if str(e).startswith(start))
            assert str(e).startswith("bigint out of range"), str(e)
        else:
            raise AssertionError("b + 1 past BIGINT's range was written")
        # bc_sub_bigint: the other direction, from the low edge.
        db.execute("INSERT INTO rb VALUES ($1, $2)", (2, BIGINT_LOW))
        try:
            db.execute("UPDATE rb SET b = b - $1 WHERE k = $2", (1, 2))
        except bytecask_sql.ExecError as e:
            assert e.constraint == "range" and o.layer_sqlstate(e) == "22003", str(e)
            assert str(e).startswith("bigint out of range"), str(e)
        else:
            raise AssertionError("b - 1 below BIGINT's range was written")
        assert seen == {start for start, _ in messages}, {start for start, _ in messages} - seen
    finally:
        db.close()


def test_parameters_and_tables_the_layer_refuses_are_sql_errors(tmp_path):
    """Refusals before a plan runs are bytecask_sql.SqlError, never a
    ValueError or TypeError: an int outside BIGINT, a parameter of a type the
    layer has none for, a table_rows of a table that does not exist."""
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)")
        for params in ((2**63,), (-(2**63) - 1,), ([1],)):
            try:
                db.execute("INSERT INTO t VALUES ($1)", params)
            except bytecask_sql.SqlError as e:
                assert "parameter $1" in str(e) and not isinstance(e, (ValueError, TypeError))
            else:
                raise AssertionError(f"{params!r} was taken")
        try:
            db.table_rows("nope")
        except bytecask_sql.SqlError as e:
            assert not isinstance(e, ValueError)
        else:
            raise AssertionError("table_rows of a missing table returned")
        assert db.table_rows("t")["rows"] == []
    finally:
        db.close()


def test_closed_and_misused_params(tmp_path):
    db = bytecask_sql.open(str(tmp_path))
    db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY)")
    # Only a list or tuple holds parameters: not a str, a buffer, a
    # UserString or an array.array (sequences of characters or ints), nor a
    # dict or set. A str with a lone surrogate has no UTF-8.
    for params in (
        "1",
        b"\x01",
        bytearray(b"\x01"),
        memoryview(b"\x01"),
        collections.UserString("1"),
        array.array("q", [1]),
        {1: 1},
        {1},
        ("\ud800",),
    ):
        try:
            db.execute("INSERT INTO t VALUES ($1)", params)
        except bytecask_sql.SqlError:
            pass
        else:
            raise AssertionError(f"{params!r} was taken as parameters")
    db.close()
    try:
        db.execute("INSERT INTO t VALUES ($1)", (object(),))  # closed comes first
    except bytecask_sql.Closed:
        pass
    else:
        raise AssertionError("a closed database converted parameters")


def test_decimal_is_exact_under_any_context(tmp_path):
    db = bytecask_sql.open(str(tmp_path))
    try:
        # DML takes no literals, so the values come from DEFAULTs.
        db.execute("CREATE TABLE t (k BIGINT PRIMARY KEY, d DECIMAL(18, 4) DEFAULT 12345678901234.5678)")
        db.execute("CREATE TABLE u (k BIGINT PRIMARY KEY, d DECIMAL(18, 4) DEFAULT -0.0001)")
        for t in ("t", "u"):
            db.execute(f"INSERT INTO {t} (k) VALUES ($1)", (1,))
        with decimal.localcontext() as ctx:
            ctx.prec = 2
            ctx.traps[decimal.Inexact] = True
            ctx.traps[decimal.Rounded] = True
            rows = [db.execute(f"SELECT d FROM {t} WHERE k = $1", (1,))["rows"][0] for t in ("t", "u")]
        assert rows == [(decimal.Decimal("12345678901234.5678"),), (decimal.Decimal("-0.0001"),)]
        assert rows[0][0].as_tuple().exponent == -4
    finally:
        db.close()


def test_placeholders_to_psycopg():
    assert o.pg_params("SELECT 1", ()) == ("SELECT 1", None)
    assert o.pg_params("SELECT $2, $1", ("a", "b")) == ("SELECT %(p2)s, %(p1)s", {"p1": "a", "p2": "b"})
    # A $n used twice is one parameter, as the statement wrote it.
    assert o.pg_params("SELECT $1 WHERE $1 > 0", (5,), "t") == ("SELECT %(p1)t WHERE %(p1)t > 0", {"p1": 5})
    # A % is escaped, and a $n inside a string literal is text.
    assert o.pg_params("SELECT '50%', '$1 it''s', $1", (7,)) == ("SELECT '50%%', '$1 it''s', %(p1)s", {"p1": 7})


def test_rows_compare_by_type():
    assert o.strict([(True,)]) != o.strict([(1,)])
    assert o.strict([(1,)]) != o.strict([(1.0,)])
    assert o.strict([(-0.0,)]) != o.strict([(0.0,)])
    assert o.strict([("a", b"a", None)]) == o.strict([("a", b"a", None)])


def test_order_by_places_nulls_as_postgresql(dsn):
    """A nullable ordered column after an equality prefix: the two ranges
    put its NULLs last under ASC and first under DESC, one LIMIT across
    both; the key's own order NULLS FIRST under ASC. A nullable DESC index
    part, and every other placement, through a sort."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.TEXT), Column("c2", o.BIGINT)],
                       ["c0"]))
        m.create_index("t0", Index("t0_ix0", [("c2", False), ("c1", False)]))
        m.create_index("t0", Index("t0_ix1", [("c1", True)]))
        for k, (c1, c2) in enumerate([("b", 1), (None, 1), ("a", 1), (None, 1), ("c", 2), ("a", 1)]):
            m.insert("t0", {"c0": k, "c1": c1, "c2": c2}, all_columns=True)
        for desc in (False, True):
            for limit in (None, 1, 3, 5):
                for nulls in (None, True, False):
                    m.select("t0", ["c1", "c0"], [("c2", "=", 1)], [("c1", desc, nulls)], limit)
                    m.select("t0", ["c1", "c0"], [("c2", "=", 1)], [("c1", desc, nulls), ("c0", desc)], limit)
                    m.select("t0", ["c1", "c0"], [("c0", ">", 0)], [("c1", desc, nulls)], limit)
                    m.select("t0", ["c1", "c2"], [], [("c2", not desc), ("c1", desc, nulls)], limit)
        # Only t0_ix1 leads with c1; its part is DESC.
        m.select("t0", ["c1"], [("c1", ">", "a")], [("c1", False)])
        m.check()
    finally:
        m.teardown()


def test_count_from_the_keys_and_by_row(dsn):
    """COUNT(*) and COUNT of a NOT NULL column count the key range; COUNT of
    a nullable column counts its values. 0 over no rows, and a NULL
    parameter matches none."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.TEXT)], ["c0", "c1"]))
        for k, c2 in enumerate(["x", None, "y", None]):
            m.insert("t0", {"c0": k % 2, "c1": k, "c2": c2}, all_columns=True)
        for arg in ("*", "c1", "c2"):
            for key in (0, 1, 7, None):
                m.select("t0", [], [("c0", "=", key)], count=arg)
            m.select("t0", [], [("c0", "=", 0), ("c1", ">=", 1)], count=arg)
            m.select("t0", [], [("c0", "=", 1), ("c1", "=", 1)], count=arg, limit=0)
    finally:
        m.teardown()


def test_column_plus_parameter_at_the_edges(dsn):
    """SET col = col ± $1 on BIGINT, SMALLINT, INTEGER and NUMERIC(6, 2): a
    result outside the column's type is 22003 on both sides, NULL stays NULL.
    BIGINT's is int64 itself, at both ends (#540)."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.SMALLINT), Column("c2", o.INTEGER),
                              Column("c3", o.numeric(6, 2)), Column("c4", o.BIGINT)], ["c0"]))
        m.insert("t0", {"c0": 1, "c1": 32760, "c2": -2147483640, "c3": decimal.Decimal("9999.00"), "c4": BIGINT_HIGH},
                 all_columns=True)
        m.insert("t0", {"c0": 2, "c1": None, "c2": None, "c3": None, "c4": None}, all_columns=True)
        m.insert("t0", {"c0": 5, "c1": 0, "c2": 0, "c3": decimal.Decimal("0.00"), "c4": BIGINT_LOW}, all_columns=True)
        for key in (1, 2, 3):
            assert m.add("t0", "c1", "+", 7, (key,)).error is None
            assert m.add("t0", "c1", "+", 1, (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c2", "-", 9, (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c3", "+", decimal.Decimal("0.99"), (key,)).error is None
            assert m.add("t0", "c3", "+", decimal.Decimal("0.01"), (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c3", "-", None, (key,)).error is None
            assert m.add("t0", "c4", "+", 1, (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c4", "-", 1, (key,)).error is None  # BIGINT_HIGH - 1; NULL stays NULL
        assert m.add("t0", "c4", "+", BIGINT_HIGH, (1,)).error == "22003"  # (2^63 - 2) + (2^63 - 1)
        # Row 5 starts at -2^63: nothing below it.
        assert m.add("t0", "c4", "-", 1, (5,)).error == "22003"
        assert m.add("t0", "c4", "+", -1, (5,)).error == "22003"
        assert m.add("t0", "c4", "+", BIGINT_HIGH, (5,)).error is None  # -1
        assert m.add("t0", "c4", "-", BIGINT_LOW, (5,)).error is None  # 2^63 - 1
        assert m.add("t0", "c4", "-", BIGINT_LOW, (5,)).error == "22003"
        assert m.add("t0", "c2", "+", 2**31, (2,)).error == "22003"  # the operand is an INTEGER, on both
        # An operand of 18 digits or more is 22003 on the layer's side, before
        # the plan runs; PostgreSQL's NUMERIC takes it, and the sum overflows
        # NUMERIC(6, 2).
        m.insert("t0", {"c0": 4, "c1": 0, "c2": 0, "c3": decimal.Decimal("1.00")}, all_columns=True)
        assert m.add("t0", "c3", "+", decimal.Decimal(10**16), (4,)).error == "22003"
        m.check()
    finally:
        m.teardown()


def test_column_plus_parameter_across_bigint_and_scale_0_decimal(dsn):
    """SET target = col ± $1: the sum is in col's type, as PostgreSQL's
    operator types it, and the target only checks it on assignment. A BIGINT
    col leaves int64 as "bigint out of range"; a NUMERIC(18, 0) col cannot,
    its operand being held to 18 digits, so its overflow is the target's.
    Both are 22003, a NUMERIC(18, 0) of scale 0 being a BIGINT to the layer
    (#540). A SMALLINT or INTEGER target of a BIGINT col is refused."""
    m = fixed(dsn)
    big = 10**18 - 1
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.BIGINT),
                              Column("c2", o.numeric(18, 0)), Column("c3", o.SMALLINT)], ["c0"]))
        m.insert("t0", {"c0": 1, "c1": BIGINT_HIGH, "c2": decimal.Decimal(big), "c3": 0}, all_columns=True)
        m.insert("t0", {"c0": 2, "c1": BIGINT_LOW, "c2": decimal.Decimal(-big), "c3": 0}, all_columns=True)
        m.insert("t0", {"c0": 3, "c1": 5, "c2": decimal.Decimal(0), "c3": 0}, all_columns=True)
        d = decimal.Decimal
        # A NUMERIC(18, 0) col and target: past 18 digits, though not past int64.
        assert m.add("t0", "c2", "+", d(1), (1,)).error == "22003"
        assert m.add("t0", "c2", "+", d(big), (1,)).error == "22003"
        assert m.add("t0", "c2", "-", d(1), (2,)).error == "22003"
        assert m.add("t0", "c2", "-", d(big), (1,)).error is None  # 0
        assert m.add("t0", "c2", "+", d(big), (1,)).error is None
        # A BIGINT col into a NUMERIC(18, 0) target: int64's edge first, then 18 digits.
        assert m.add("t0", "c1", "+", 1, (1,), target="c2").error == "22003"  # bigint out of range
        assert m.add("t0", "c1", "-", 1, (2,), target="c2").error == "22003"
        assert m.add("t0", "c1", "-", 1, (1,), target="c2").error == "22003"  # fits int64, not 18 digits
        assert m.add("t0", "c1", "+", 1, (3,), target="c2").error is None  # 6
        assert m.add("t0", "c1", "-", 7, (3,), target="c2").error is None  # -2
        # A NUMERIC(18, 0) col into a BIGINT target: the sum fits int64 whatever the operand.
        assert m.add("t0", "c2", "+", d(big), (1,), target="c1").error is None  # 2 * (10^18 - 1)
        assert m.add("t0", "c2", "-", d(big), (2,), target="c1").error is None
        assert m.add("t0", "c2", "+", None, (1,), target="c1").error is None
        sql = "UPDATE t0 SET c3 = c1 + $1 WHERE c0 = $2"
        out = o.run_layer(m.db, sql, (1, 3))
        assert out.error == "refused" and "assigned column's type" in out.detail, out
        m.check()
    finally:
        m.teardown()


def binder_machine(dsn) -> SqlMachine:
    """A machine with one table of every type a binder fault needs, and a row."""
    m = fixed(dsn)
    m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.TEXT), Column("c2", o.DOUBLE),
                          Column("c3", o.BOOLEAN), Column("c4", o.BYTEA), Column("c5", o.numeric(6, 2)),
                          Column("c6", o.BIGINT)], ["c0"]))
    m.create(Table("t1", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.TEXT)], ["c0"]))
    m.insert("t0", {"c0": 1, "c1": "a", "c2": 1.5, "c3": True, "c4": b"x", "c5": decimal.Decimal("1.50"), "c6": 7},
             all_columns=True)
    return m


def test_binder_refusals_carry_postgresqls_sqlstate(dsn):
    """A statement the binder refuses for a fault PostgreSQL also refuses gets
    PostgreSQL's SQLSTATE (#548). PostgreSQL gets each statement with its $n as
    written and untyped (run_pg_numbered), as the layer reads it: run_pg
    would send each typed as psycopg adapts it."""
    m = binder_machine(dsn)
    try:
        for sql, params, state in (
            ("SELECT c0 FROM nope WHERE c0 = $1", (1,), "42P01"),
            ("SELECT c0 FROM t0 WHERE t9.c0 = $1", (1,), "42P01"),
            ("SELECT zz FROM t0 WHERE c0 = $1", (1,), "42703"),
            ("UPDATE t0 SET zz = $1 WHERE c0 = $2", (1, 2), "42703"),
            ("SELECT c1 FROM t0, t1 WHERE t0.c0 = $1 AND t1.c0 = $2", (1, 2), "42702"),
            ("INSERT INTO t0 (c0, c0) VALUES ($1, $2)", (1, 2), "42701"),
            ("SELECT c1, count(*) FROM t0 WHERE c0 = $1", (1,), "42803"),
            ("UPDATE t0 SET c1 = count(*) WHERE c0 = $1", (1,), "42803"),
            # No aggregate of that type: 42883.
            ("SELECT sum(c1) FROM t0 WHERE c0 = $1", (1,), "42883"),
            ("SELECT sum(c3) FROM t0 WHERE c0 = $1", (1,), "42883"),
            ("SELECT min(c4) FROM t0 WHERE c0 = $1", (1,), "42883"),
            # A $n compared with two columns no operator relates: 42883.
            ("SELECT c0 FROM t0 WHERE c1 = $1 AND c0 = $1", ("a",), "42883"),
            ("SELECT c0 FROM t0 WHERE c0 = $1 AND c1 = $1", (1,), "42883"),
            ("SELECT c0 FROM t0 WHERE c3 = $1 AND c0 = $1", (True,), "42883"),
            ("SELECT c0 FROM t0 WHERE c2 = $1 AND c3 = $1", (1.5,), "42883"),
            # A column assigned from a column of a type that has no assignment cast to it: 42804.
            ("UPDATE t0 SET c6 = c1 WHERE c0 = $1", (1,), "42804"),
            ("UPDATE t0 SET c6 = c3 WHERE c0 = $1", (1,), "42804"),
            ("UPDATE t0 SET c3 = c6 WHERE c0 = $1", (1,), "42804"),
            ("UPDATE t0 SET c2 = c3 WHERE c0 = $1", (1,), "42804"),
            ("UPDATE t0 SET c4 = c6 WHERE c0 = $1", (1,), "42804"),
            # 0A000 and 22003.
            ("SELECT sum(c6) FROM t0 WHERE c0 = $1 FOR UPDATE", (1,), "0A000"),
            ("UPDATE t0 SET c5 = c5 + 100000000000000000 WHERE c0 = $1", (1,), "22003"),
        ):
            layer = o.run_layer(m.db, sql, params)
            pg = o.run_pg_numbered(m.pg.conn, sql, params)
            assert layer.error == pg.error == state, (sql, layer, pg)
        # 42P18, in the same loop: $1 has no use to type it.
        sql = "SELECT c0 FROM t0 WHERE c0 = $2"
        layer = o.run_layer(m.db, sql, (1, 1))
        pg = o.run_pg_numbered(m.pg.conn, sql, (1, 1))
        assert layer.error == pg.error == "42P18", (sql, layer, pg)
        m.check()
    finally:
        m.teardown()


def test_a_computed_bound_past_integer_is_22003_before_the_plan_runs(dsn):
    """WHERE c > $1 - k: $n is an INTEGER, so a $n past INTEGER, int64's edge
    included, is 22003 before the plan runs and its addition (bc_add) never
    sees an int64 overflow. Only the layer is asked: the oracle's driver sends
    a $n past INTEGER as a bigint, which PostgreSQL then takes."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False)], ["c0"]))
        m.insert("t0", {"c0": 1}, all_columns=True)
        types = [o.INTEGER]
        for op in ("-", "+"):
            sql = f"SELECT c0 FROM t0 WHERE c0 > $1 {op} 15"
            for edge in (BIGINT_HIGH, BIGINT_LOW, 2**31, -(2**31) - 1):
                assert m.layer(sql, (edge,), types).error == "22003", (op, edge)
            assert m.both(sql, (0,), types=types).error is None
        m.check()
    finally:
        m.teardown()


def test_forms_the_layer_does_not_support_are_refused_without_a_sqlstate(dsn):
    """A statement PostgreSQL runs, which the layer refuses as a form it does not
    support, is 42601: the layer claims PostgreSQL's code only where
    PostgreSQL refuses the statement with it. No row matches a WHERE here, so
    PostgreSQL's run leaves the two sides alike."""
    m = binder_machine(dsn)
    try:
        for sql, params in (
            ("UPDATE t0 SET c2 = c6 WHERE c0 = $1", (99,)),  # bigint to double precision
            ("UPDATE t0 SET c6 = c2 WHERE c0 = $1", (99,)),
            ("UPDATE t0 SET c1 = c6 WHERE c0 = $1", (99,)),  # any type to text
            ("UPDATE t0 SET c1 = c3 WHERE c0 = $1", (99,)),
            ("SELECT c0 FROM t0 WHERE c5 = $1 AND c2 = $1", (1,)),  # numeric and double precision
            ("SELECT c0 FROM t0 WHERE c2 = $1 AND c6 = $1", (1.5,)),
            ("UPDATE t0 SET c1 = $1 WHERE c6 = $1 AND c0 = $2", (1, 99)),  # PostgreSQL types the WHERE's $1 first
            ("UPDATE t0 SET c6 = c6 + $1 WHERE c0 = $2 AND c2 = $1", (1, 99)),
            ("SELECT sum(c2) FROM t0 WHERE c0 = $1", (1,)),
            ("SELECT min(c2) FROM t0 WHERE c0 = $1", (1,)),
            ("SELECT min(c1) FROM t0 WHERE c0 = $1", (1,)),
        ):
            layer = o.run_layer(m.db, sql, params)
            pg = o.run_pg_numbered(m.pg.conn, sql, params)
            assert pg.error is None, (sql, pg)
            assert layer.error == "refused" and layer.sqlstate == "42601", (sql, layer)
        # PostgreSQL refuses an untyped $1 assigned to two columns of different types
        # (42P08, not 42804): the layer does not claim a code for it either.
        sql = "UPDATE t0 SET c2 = $1, c6 = $1 WHERE c0 = $2"
        layer = o.run_layer(m.db, sql, (2.0, 99))
        pg = o.run_pg_numbered(m.pg.conn, sql, (2.0, 99))
        assert layer.sqlstate == "42601" and pg.sqlstate == "42P08", (sql, layer, pg)
        m.check()
    finally:
        m.teardown()


def test_create_table_of_an_existing_name_refuses_in_postgresqls_order(dsn):
    """A repeated column or a missing key column is refused before an existing
    name, as PostgreSQL does. (A table without a key, or with a column of an
    unsupported type or default, never reaches the catalog: the binder refuses
    it first, 42601, where PostgreSQL says 42P07. The catalog's own order for
    those is its unit test's.)"""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False)], ["c0"]))
        assert m.create(Table("t0", [Column("c0", o.BIGINT), Column("c0", o.TEXT)], ["c0"])).error == "42701"
        assert m.create(Table("t0", [Column("c0", o.BIGINT)], ["zz"])).error == "42703"
        m.check()
    finally:
        m.teardown()


def test_a_parameter_used_with_two_types_is_refused(dsn):
    """SET c1 = $1, c2 = c2 + $1, with c1 BIGINT and c2 SMALLINT: the layer
    refuses a $n used with two SQL types, in either order (docs/testing_design.md,
    *Known differences*); PostgreSQL infers one and runs it. One parameter per
    column runs on both."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.BIGINT), Column("c2", o.SMALLINT)],
                       ["c0"]))
        m.insert("t0", {"c0": 1, "c1": 0, "c2": -30000}, all_columns=True)
        for sql in ("UPDATE t0 SET c1 = $1, c2 = c2 + $1 WHERE c0 = $2", "UPDATE t0 SET c2 = c2 + $1, c1 = $1 WHERE c0 = $2"):
            out = o.run_layer(m.db, sql, (40000, 1))
            assert out.error == "refused" and "parameter $1 is" in out.detail, out
        sql = "UPDATE t0 SET c1 = $1, c2 = c2 + $2 WHERE c0 = $3"
        assert m.both(sql, (40000, 32000, 1)).affected == 1
        assert m.both(sql, (40000, 32000, 1)).error == "22003"  # 2000 + 32000 leaves SMALLINT
        m.check()
    finally:
        m.teardown()


def test_a_join_of_two_point_lookups(dsn):
    """FROM a, b and a JOIN b ON …, each table by its whole primary key: the
    row pair, or none when either row is missing or a key is NULL."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.TEXT)], ["c0"]))
        m.create(Table("t1", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.BIGINT, nullable=False),
                              Column("c2", o.numeric(6, 2))], ["c0", "c1"]))
        m.insert("t0", {"c0": 1, "c1": "a"}, all_columns=True)
        m.insert("t1", {"c0": 1, "c1": 2, "c2": decimal.Decimal("1.50")}, all_columns=True)
        targets = [("t1", "c2"), ("t0", "c1"), ("t1", "c0")]
        for keys in (((1,), (1, 2)), ((2,), (1, 2)), ((1,), (1, 3)), ((None,), (1, 2)), ((1,), (None, 2))):
            for on in (False, True):
                out = m.join(("t0", "t1"), targets, keys, on=on)
                assert len(out.rows) == (1 if keys == ((1,), (1, 2)) else 0)
    finally:
        m.teardown()


def test_column_plus_an_integer_literal(dsn):
    """SET col = col ± n, n an integer literal: PostgreSQL types it integer,
    or bigint past integer; the stored value is checked as the column's
    type, 22003 on both sides when it leaves it."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.SMALLINT), Column("c2", o.INTEGER),
                              Column("c3", o.numeric(6, 2))], ["c0"]))
        m.insert("t0", {"c0": 1, "c1": 32760, "c2": 0, "c3": decimal.Decimal("9990.25")}, all_columns=True)
        m.insert("t0", {"c0": 2, "c1": None, "c2": None, "c3": None}, all_columns=True)
        for key in (1, 2):
            assert m.add("t0", "c1", "+", 7, (key,), literal=True).error is None
            assert m.add("t0", "c1", "+", 1, (key,), literal=True).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c2", "-", 2**31 + 1, (key,), literal=True).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c3", "+", 9, (key,), literal=True).error is None
            assert m.add("t0", "c3", "+", 1, (key,), literal=True).error == ("22003" if key == 1 else None)
            # Past BIGINT's half: the sum leaves the column, never int64.
            assert m.add("t0", "c1", "+", 2**63 - 1, (key,), literal=True).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c2", "-", 2**63 - 1, (key,), literal=True).error == ("22003" if key == 1 else None)
        m.check()
    finally:
        m.teardown()


def test_a_range_refusal_on_a_join_names_its_columns_table(tmp_path):
    """A parameter outside its column's type is refused before the plan runs,
    naming the table of the column it is compared with: on a join, the
    second table's for its key."""
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE a (k BIGINT PRIMARY KEY, v BIGINT)")
        db.execute("CREATE TABLE b (k SMALLINT PRIMARY KEY, w BIGINT)")
        ids = {t["name"]: t["id"] for t in db.catalog()}
        try:
            db.execute("SELECT v, w FROM a, b WHERE a.k = $1 AND b.k = $2", (1, 40000))
        except bytecask_sql.ExecError as e:
            assert (e.constraint, e.object_id) == ("range", ids["b"])
        else:
            raise AssertionError("a SMALLINT key of 40000 was accepted")
    finally:
        db.close()



def test_sum_and_min_over_rows_nulls_and_none(dsn):
    """SUM and MIN by a primary-key prefix and by an index: the value over
    the non-NULL ones, NULL over none or all NULL, and none under LIMIT 0.
    SUM of SMALLINT and INTEGER is a bigint, of NUMERIC a numeric at its
    scale; MIN is of the column's type."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.SMALLINT), Column("c3", o.numeric(6, 2)), Column("c4", o.BIGINT)],
                       ["c0", "c1"]))
        m.create_index("t0", Index("t0_ix0", [("c0", False), ("c4", False)]))
        rows = [(0, 1, 7, "1.25", 9), (0, 2, None, "-3.50", None), (0, 3, -2, None, -4), (1, 4, None, None, None),
                (1, 5, 32767, "9999.99", 2**62)]
        for c0, c1, c2, c3, c4 in rows:
            m.insert("t0", {"c0": c0, "c1": c1, "c2": c2, "c3": None if c3 is None else decimal.Decimal(c3), "c4": c4},
                     all_columns=True)
        m.insert("t0", {"c0": 1, "c1": 6, "c2": 32767, "c3": decimal.Decimal("9999.99"), "c4": 2**62},
                 all_columns=True)
        for fn, arg in (("sum", "c2"), ("sum", "c3"), ("min", "c2"), ("min", "c3"), ("min", "c4"), ("min", "c1")):
            for key in (0, 1, 2, None):
                m.select("t0", [], [("c0", "=", key)], count=arg, fn=fn)
            m.select("t0", [], [("c0", "=", 0), ("c1", ">", 1)], count=arg, fn=fn)
            m.select("t0", [], [("c0", "=", 0), ("c1", "=", 3)], count=arg, fn=fn)
            m.select("t0", [], [("c0", "=", 0)], count=arg, fn=fn, limit=0)
        # MIN of c4 from t0_ix0's first entry past its NULLs; of c1, a loop.
        for key in (0, 1, 2):
            m.select("t0", [], [("c0", "=", key), ("c4", "<", 10)], count="c4", fn="min")
    finally:
        m.teardown()


def test_sum_past_int64_is_an_error(tmp_path):
    """A SUM whose running total leaves int64, a BIGINT's or a DECIMAL's in its
    scaled integer, fails the statement as SQLSTATE 22003, where PostgreSQL's
    numeric would hold it (docs/frontend_design.md, *Known differences*)."""
    db = bytecask_sql.open(str(tmp_path))
    try:
        db.execute("CREATE TABLE t (k BIGINT, n BIGINT NOT NULL, v BIGINT, PRIMARY KEY (k, n))")
        for n, v in enumerate((2**62, 2**62 - 1, 2**62)):
            db.execute("INSERT INTO t VALUES ($1, $2, $3)", (1, n, v))
        assert db.execute("SELECT sum(v) FROM t WHERE k = $1 AND n < $2", (1, 2))["rows"] == [(decimal.Decimal(2**63 - 1),)]
        try:
            db.execute("SELECT sum(v) FROM t WHERE k = $1", (1,))
        except bytecask_sql.ExecError as e:
            assert e.constraint is None and "out of range" in str(e) and e.sqlstate == "22003", e
        else:
            raise AssertionError("a sum past int64 was returned")
        db.execute("CREATE TABLE d (g BIGINT, k BIGINT, v NUMERIC(18, 0), PRIMARY KEY (g, k))")
        for k in range(10):  # ten of 10^18 - 1: 9.99 * 10^18 > 2^63
            db.execute("INSERT INTO d VALUES ($1, $2, $3)", (1, k, 10**18 - 1))
        try:
            db.execute("SELECT sum(v) FROM d WHERE g = $1", (1,))
        except bytecask_sql.ExecError as e:
            assert e.constraint is None and e.sqlstate == "22003", e
        else:
            raise AssertionError("a DECIMAL sum past int64 was returned")
    finally:
        db.close()


def test_update_and_delete_every_row_a_key_reaches(dsn):
    """UPDATE and DELETE by a primary-key prefix, an index and a range: each
    row matched is written once, the count is the rows matched, and a NULL
    key matches none. In a transaction a later statement sees the earlier
    one's writes."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.SMALLINT), Column("c3", o.TEXT)], ["c0", "c1"]))
        m.create_index("t0", Index("t0_ix0", [("c2", True)]))
        for c1 in range(6):
            m.insert("t0", {"c0": c1 % 2, "c1": c1, "c2": c1 * 10 if c1 != 3 else None, "c3": f"r{c1}"},
                     all_columns=True)
        assert m.write("t0", [("c0", "=", 0)], ("c3", "=", "even")).affected == 3
        assert m.write("t0", [("c0", "=", None)], ("c3", "=", "none")).affected == 0
        # A SET parameter outside its column's type is refused before any row
        # is read, as PostgreSQL coerces it at bind.
        assert m.write("t0", [("c0", "=", None)], ("c2", "=", 40000)).error == "22003"
        assert m.write("t0", [("c2", ">=", 20)], ("c2", "+", 5)).affected == 3
        assert m.write("t0", [("c2", "<", 30)], ("c2", "-", 1)).affected == 3
        assert m.write("t0", [("c2", ">", 0)], ("c2", "+", 32767)).error == "22003"  # nothing written
        m.check()
        m.begin()
        assert m.write("t0", [("c0", "=", 1), ("c1", ">", 1)], ("c3", "=", "odd")).affected == 2
        m.select("t0", ["c1", "c3"], [("c0", "=", 1)])
        assert m.write("t0", [("c0", "=", 1), ("c1", "<=", 3)]).affected == 2
        m.select("t0", ["c1", "c3"], [("c0", "=", 1)])
        m.commit()
        assert m.write("t0", [("c2", "<", 100)]).affected == 4
        m.check()
    finally:
        m.teardown()



def test_one_unique_value_given_to_two_rows_is_23505(dsn):
    """An UPDATE that gives two rows one value of a unique index is refused
    (23505) on both sides, and writes nothing (#507)."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.INTEGER)], ["c0", "c1"]))
        m.create_index("t0", Index("t0_ix0", [("c2", False)], unique=True))
        for c1, c2 in ((1, 1), (2, 2), (3, None)):
            m.insert("t0", {"c0": 0, "c1": c1, "c2": c2}, all_columns=True)
        assert m.write("t0", [("c0", "=", 0), ("c1", ">=", 2)], ("c2", "=", 7)).error == "23505"
        assert m.write("t0", [("c0", "=", 0), ("c1", ">=", 2)], ("c2", "=", None)).affected == 2
        assert m.write("t0", [("c0", "=", 0)], ("c2", "=", 1)).error == "23505"
        m.check()
    finally:
        m.teardown()


def test_an_update_parameter_is_checked_against_its_narrowest_use(dsn):
    """$1 added to a NUMERIC(10, 2) and assigned to a NUMERIC(4, 2): a value
    only the wider holds is 22003 on both sides, in either order, even when
    no row matches; one both hold runs."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.numeric(4, 2)),
                              Column("c2", o.numeric(10, 2))], ["c0"]))
        m.insert("t0", {"c0": 1, "c1": decimal.Decimal("1.00"), "c2": decimal.Decimal("1.00")}, all_columns=True)
        types = [o.numeric(4, 2), o.BIGINT]
        for sql in ("UPDATE t0 SET c2 = c2 + $1, c1 = $1 WHERE c0 = $2", "UPDATE t0 SET c1 = $1, c2 = c2 + $1 WHERE c0 = $2"):
            for key in (1, 2):
                assert m.both(sql, (decimal.Decimal("123.45"), key), types=types).error == "22003"
            assert m.both(sql, (decimal.Decimal("12.34"), 1), types=types).affected == 1
        m.check()
    finally:
        m.teardown()


def test_filters_on_what_the_key_does_not_take(dsn):
    """A conjunct the key does not take filters each row: on a point lookup,
    a scan, an UPDATE and a DELETE, by every operator, NULL on either side
    rejecting the row. One on a DOUBLE column is refused with no plan."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.SMALLINT), Column("c3", o.TEXT), Column("c4", o.DOUBLE)], ["c0", "c1"]))
        for c1, c2, c3 in ((1, 5, "b"), (2, None, "a"), (3, 7, None), (4, 5, "c"), (5, 9, "ab")):
            m.insert("t0", {"c0": 0, "c1": c1, "c2": c2, "c3": c3, "c4": 1.5}, all_columns=True)
        for op in ("=", "<", "<=", ">", ">="):
            for v in (5, None):
                m.select("t0", ["c1", "c2"], [("c0", "=", 0), ("c2", op, v)])
                m.select("t0", ["c1"], [("c0", "=", 0), ("c1", "=", 4), ("c2", op, v)])
            m.select("t0", ["c1", "c3"], [("c0", "=", 0), ("c1", ">", 1), ("c3", op, "ab")], limit=2)
            m.select("t0", [], [("c0", "=", 0), ("c3", op, "b")], count="*")
        m.select("t0", ["c1"], [("c0", "=", 0), ("c4", "=", 1.5)])  # refused: a DOUBLE filter
        assert m.write("t0", [("c0", "=", 0), ("c2", "=", 5)], ("c3", "=", "five")).affected == 2
        assert m.write("t0", [("c0", "=", 0), ("c1", "=", 3), ("c2", ">", 8)], ("c3", "=", "x")).affected == 0
        assert m.write("t0", [("c0", "=", 0), ("c3", "<", "b")]).affected == 2
        m.check()
    finally:
        m.teardown()


def test_a_bound_of_a_parameter_plus_an_integer(dsn):
    """col op $n ± k on an INTEGER column: a range bound, or a filter for =;
    a NULL $n matches nothing, and a sum outside INTEGER is 22003 on both
    sides, before any row is read."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.INTEGER)], ["c0", "c1"]))
        for c1 in range(10):
            m.insert("t0", {"c0": 1, "c1": c1 * 10, "c2": c1}, all_columns=True)
        for k in (-20, 0, 5):
            m.select("t0", ["c1"], [("c0", "=", 1), ("c1", "<", 60), ("c1", ">=", Calc(60, k))])
            m.select("t0", ["c1"], [("c0", "=", 1), ("c1", "=", Calc(30 - k, k))])
            m.select("t0", ["c1"], [("c0", "=", 1), ("c2", "<", Calc(5 - k, k))])
        m.select("t0", ["c1"], [("c0", "=", 1), ("c1", ">=", Calc(None, -20))])
        m.select("t0", ["c1"], [("c0", "=", None), ("c1", ">=", Calc(-2**31, -20))])  # 22003, the key NULL
        m.select("t0", ["c1"], [("c0", "=", 1), ("c1", "<", Calc(2**31 - 1, 1))])  # 22003
        assert m.write("t0", [("c0", "=", 1), ("c1", ">", Calc(70, 10))]).affected == 1
        m.check()
    finally:
        m.teardown()


def test_a_join_on_a_column(dsn):
    """FROM a, b with each of b's key parts equal to a column of a or to a
    parameter: a scanned, b read by its key from each a row. A NULL join
    column or a missing b row joins nothing; filters on either side; LIMIT
    and COUNT(DISTINCT) over the pairs."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.INTEGER)], ["c0", "c1"]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.SMALLINT)], ["c0", "c1"]))
        for c1, c2 in ((1, 10), (2, 20), (3, None), (4, 30), (5, 10), (6, 40)):
            m.insert("t0", {"c0": 1, "c1": c1, "c2": c2}, all_columns=True)
        for c0, q in ((10, 5), (20, 50), (30, 7)):
            m.insert("t1", {"c0": c0, "c1": 1, "c2": q}, all_columns=True)
        keys = [("c0", ("col", "c2")), ("c1", ("param", 1))]
        for where in ([("c0", "=", 1)], [("c0", "=", 1), ("c1", "<", 6), ("c1", ">=", Calc(6, -4))],
                      [("c0", "=", 1), ("c1", "=", 1)], [("c0", "=", None)]):
            for filters in ([], [("c2", "<", 10)], [("c2", "<", None)]):
                m.join_on("t0", "t1", where, keys, filters, [("t0", "c1"), ("t1", "c2")])
                m.join_on("t0", "t1", where, keys, filters, [], distinct=("t1", "c0"))
            m.join_on("t0", "t1", where, keys, [], [("t1", "c0")], limit=2)
        # t0 scanned in full, filtered, and t1 looked up by key per row.
        m.join_on("t0", "t1", [("c2", "=", 1)], [("c0", ("col", "c2")), ("c1", ("col", "c1"))], [], [("t0", "c1")])
        # An outer value too long for any key joins nothing, as in PostgreSQL;
        # it still counts as a distinct value.
        m.create(Table("t2", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.TEXT)], ["c0"]))
        m.create(Table("t3", [Column("c0", o.TEXT, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        for c0, c1 in ((1, "a"), (2, "x" * 70000), (3, "b"), (4, "x" * 70000)):
            m.insert("t2", {"c0": c0, "c1": c1}, all_columns=True)
        for c0, c1 in (("a", 1), ("b", 2)):
            m.insert("t3", {"c0": c0, "c1": c1}, all_columns=True)
        m.join_on("t2", "t3", [("c0", ">=", 1)], [("c0", ("col", "c1"))], [], [("t2", "c0"), ("t3", "c1")])
        m.select("t2", [], [("c0", ">=", 1)], count="c1", distinct=True)
    finally:
        m.teardown()


def test_a_table_read_once_joined_on_columns_neither_key_takes(dsn):
    """FROM a, b with b named by its whole key in parameters, a scanned by
    parameters, and equalities of columns neither key takes linking them:
    b read once and a's scan under it, or a's scan with a lookup of b per
    row. Every plan applies the links; a missing b row or a NULL joins
    nothing; a link on DOUBLE columns has no plan."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER),
                              Column("c2", o.DOUBLE), Column("c3", o.INTEGER)], ["c0"]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.INTEGER), Column("c3", o.DOUBLE), Column("c4", o.INTEGER)], ["c0", "c1"]))
        for c0, c1, c3 in ((1, 10, 7), (2, 20, 8), (3, None, 7)):
            m.insert("t0", {"c0": c0, "c1": c1, "c2": 1.5, "c3": c3}, all_columns=True)
        for c1, c2, c4 in ((1, 10, 7), (2, 20, 7), (3, None, 7), (4, 10, 8), (5, 30, 7), (6, 10, 7)):
            m.insert("t1", {"c0": 1, "c1": c1, "c2": c2, "c3": 1.5, "c4": c4}, all_columns=True)
        targets = [("t1", "c1"), ("t0", "c1")]
        for key in (1, 2, 3, 9, None):
            keys = [("c0", ("param", key))]
            for where in ([("c0", "=", 1)], [("c0", "=", 1), ("c1", ">=", 2), ("c1", "<", 6)], [("c0", "=", None)]):
                m.join_on("t1", "t0", where, keys, [], targets, links=[("c1", "c2")])
                m.join_on("t1", "t0", where, keys, [], targets, links=[("c1", "c2"), ("c3", "c4")])
                m.join_on("t1", "t0", where, keys, [], [], links=[("c1", "c2")], distinct=("t1", "c1"))
                m.join_on("t1", "t0", where, keys, [], targets, links=[("c1", "c2")], limit=1)
            # On DOUBLE columns: no plan.
            m.join_on("t1", "t0", [("c0", "=", 1)], keys, [], targets, links=[("c2", "c3")])
    finally:
        m.teardown()


def test_hash_joins(dsn):
    """FROM a, b linked by equalities of columns no key takes, or by none,
    each table scanned by parameters or in full: a hash join, built on
    either side, or a table read once per row of the other. A NULL join
    column matches nothing; duplicate keys on both sides give every pair;
    under ORDER BY of either table's columns or both, LIMIT and
    COUNT(DISTINCT). Every plan agrees."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER), Column("c2", o.TEXT)],
                       ["c0"]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER), Column("c2", o.TEXT)],
                       ["c0"]))
        for c0, c1, c2 in ((1, 10, "a"), (2, 10, "b"), (3, None, "a"), (4, 20, None), (5, 30, "c"), (6, 10, "a")):
            m.insert("t0", {"c0": c0, "c1": c1, "c2": c2}, all_columns=True)
        for c0, c1, c2 in ((1, 10, "a"), (2, 10, "a"), (3, 20, "b"), (4, None, None), (5, 40, "a")):
            m.insert("t1", {"c0": c0, "c1": c1, "c2": c2}, all_columns=True)
        targets = [("t0", "c0"), ("t1", "c0")]
        for where_a in ([], [("c0", ">=", 2)], [("c0", "<", 6), ("c1", ">", 0)]):
            for where_b in ([], [("c0", "<=", 4)]):
                for links in ([("c1", "c1")], [("c1", "c1"), ("c2", "c2")], [("c2", "c2")], []):
                    m.join_on("t0", "t1", where_a, [], where_b, targets, links=links)
                    m.join_on("t0", "t1", where_a, [], where_b, targets, links=links, limit=2)
                    m.join_on("t0", "t1", where_a, [], where_b, targets + [("t1", "c2")], links=links, limit=3,
                              order=[("t1", "c2", True, None), ("t1", "c0", False, None)])
                    m.join_on("t0", "t1", where_a, [], where_b, targets + [("t0", "c2")], links=links,
                              order=[("t0", "c2", False, True)])
                    m.join_on("t0", "t1", where_a, [], where_b, [], links=links, distinct=("t1", "c2"))
                    # By both tables' columns: a sorter of each table's rows,
                    # read in step, under LIMIT each keeping the same pairs.
                    for limit in (None, 0, 2):
                        m.join_on("t0", "t1", where_a, [], where_b, targets + [("t1", "c2")], links=links,
                                  limit=limit, order=[("t1", "c2", False, None), ("t0", "c0", True, None),
                                                      ("t1", "c0", False, None)])
    finally:
        m.teardown()


def test_an_empty_select_list_returns_a_row_of_no_columns_per_match(dsn):
    """SELECT FROM t WHERE …: one empty row per match, none for no match, and
    one per row of a range."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.TEXT)], ["c0", "c1"]))
        for c1 in (1, 2, 3):
            m.insert("t0", {"c0": 1, "c1": c1, "c2": "a"}, all_columns=True)
        m.insert("t0", {"c0": 2, "c1": 1, "c2": None}, all_columns=True)
        for where in ([("c0", "=", 1), ("c1", "=", 2)], [("c0", "=", 1), ("c1", "=", 9)], [("c0", "=", 9)],
                      [("c0", "=", None)], [("c0", "=", 1)], [("c0", ">=", 1)]):
            m.select("t0", [], where)
        m.select("t0", [], [("c0", "=", 1)], limit=2)
        # Served from an index alone, with no row read.
        m.create_index("t0", Index("t0_ix0", [("c2", False)]))
        for where in ([("c2", "=", "a")], [("c2", "=", None)], [("c2", ">=", "a")]):
            m.select("t0", [], where)
        # FOR UPDATE locks what it returns; here, rows of no columns.
        for where in ([("c0", "=", 1), ("c1", "=", 2)], [("c0", "=", 1)], [("c0", "=", 9)]):
            m.select("t0", [], where, for_update=True)
        # A join of two point lookups.
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False)], ["c0"]))
        m.insert("t1", {"c0": 1}, all_columns=True)
        m.join(("t0", "t1"), [], ((1, 2), (1,)))
        m.join(("t0", "t1"), [], ((1, 2), (9,)))
        m.join(("t0", "t1"), [], ((1, 9), (1,)))
        m.join(("t0", "t1"), [], ((1, 2), (1,)), on=True)
    finally:
        m.teardown()


def test_count_distinct(dsn):
    """COUNT(DISTINCT col) counts distinct non-NULL values, -0.0 and 0.0 as
    one; 0 over no rows, and none under LIMIT 0."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.TEXT), Column("c3", o.DOUBLE)], ["c0", "c1"]))
        for c1, c2, c3 in ((1, "a", 0.0), (2, "b", -0.0), (3, None, 1.5), (4, "a", None), (5, "b", 1.5)):
            m.insert("t0", {"c0": 1, "c1": c1, "c2": c2, "c3": c3}, all_columns=True)
        for arg in ("c1", "c2", "c3"):
            for key in (1, 2, None):
                m.select("t0", [], [("c0", "=", key)], count=arg, distinct=True)
            m.select("t0", [], [("c0", "=", 1), ("c1", "=", 3)], count=arg, distinct=True)
            m.select("t0", [], [("c0", "=", 1)], count=arg, distinct=True, limit=0)
    finally:
        m.teardown()


def test_both_plans_of_a_choice_point_agree(dsn):
    """Two indexes of one table each reach the rows a WHERE asks for: the
    plan counts the first one's range when it runs, and scans it, or the
    other past the threshold (docs/planner_design.md, *Choosing at run
    time*). Each plan of the choice point, forced in turn, agrees with
    PostgreSQL, alone, read once under a join, and in an UPDATE."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER), Column("c2", o.INTEGER),
                              Column("c3", o.INTEGER)], ["c0"]))
        m.create_index("t0", Index("t0_c1", [("c1", False)]))
        m.create_index("t0", Index("t0_c2", [("c2", False)]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        # c1 = 0 on most rows, c2 spread: some ranges past the threshold, some below.
        for k in range(120):
            m.insert("t0", {"c0": k, "c1": None if k % 17 == 0 else (k % 5 if k % 4 == 0 else 0),
                            "c2": None if k % 13 == 0 else k % 6, "c3": k % 9}, all_columns=True)
        m.insert("t1", {"c0": 1, "c1": 3}, all_columns=True)
        sql = "SELECT c0, c3 FROM t0 WHERE c1 = $1 AND c2 = $2"
        for c1, c2 in ((0, 0), (4, 0), (0, 5), (3, 3), (None, 1), (0, None)):
            m.select("t0", ["c0", "c3"], [("c1", "=", c1), ("c2", "=", c2)])
            if c1 is not None and c2 is not None:
                # Each index's plan carries a choice point, run once per plan.
                assert len(m.db._alternatives(sql, (c1, c2))) >= 4
            m.select("t0", ["c0"], [("c1", "=", c1), ("c2", ">=", c2)])
            m.select("t0", [], [("c1", "=", c1), ("c2", "=", c2)], count="c3")
            m.join_on("t0", "t1", [("c1", "=", c1), ("c2", "=", c2)], [("c0", ("param", 1))], [],
                      [("t0", "c0"), ("t1", "c1")], links=[("c1", "c3")])
        for c1, c2, v in ((0, 0, 50), (4, 2, 60), (0, 4, 70)):
            m.write("t0", [("c1", "=", c1), ("c2", "=", c2)], set_to=("c3", "=", v))
        m.check()
    finally:
        m.teardown()


def test_a_join_ordered_by_its_first_tables_scan(dsn):
    """ORDER BY on a join: the first table's scan gives the order, and a key
    lookup per row keeps it; a nullable ordered column takes the two ranges,
    its NULLs where PostgreSQL puts them. And a join whose second table is
    keyed by parameters alone. Every plan the search costed agrees."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER, nullable=False),
                              Column("c2", o.INTEGER)], ["c0", "c1"]))
        m.create_index("t0", Index("t0_ix0", [("c0", False), ("c2", False)]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.TEXT)], ["c0"]))
        for c1, c2 in ((1, 3), (2, None), (3, 1), (4, 3), (5, None), (6, 2)):
            m.insert("t0", {"c0": 1, "c1": c1, "c2": c2}, all_columns=True)
        for c0, c1 in ((1, "a"), (2, "b"), (3, "c"), (4, None), (5, "e")):
            m.insert("t1", {"c0": c0, "c1": c1}, all_columns=True)

        def ordered(sql: str, params: list, key, limit):
            tail = f" LIMIT {limit}" if limit is not None else ""
            layer = m.layer(sql + tail, params)
            full = o.run_pg(m.pg.conn, sql, params)
            assert layer.error is None and full.error is None, (sql, layer, full)
            why = o.same_rows(layer.rows, full.rows, key, limit)
            assert why is None, f"{sql}{tail} {params!r}: {why}: the layer returned {layer.rows}, PostgreSQL {full.rows}"
            m.alternatives_agree(sql + tail, params, [o.INTEGER] * len(params), full.rows, key, limit)

        for desc in ("", " DESC"):
            for limit in (None, 1, 3):
                ordered(f"SELECT t0.c1, t1.c1 FROM t0, t1 WHERE t0.c0 = $1 AND t1.c0 = t0.c2 ORDER BY t0.c1{desc}",
                        [1], lambda r: (r[0],), limit)
                ordered(f"SELECT t0.c2, t1.c1 FROM t0, t1 WHERE t0.c0 = $1 AND t1.c0 = t0.c1 ORDER BY t0.c2{desc}",
                        [1], lambda r: (r[0],), limit)
        # t1 by its key from a parameter, once per t0 row.
        for key in (1, 4, 9):
            ordered("SELECT t0.c1, t1.c1 FROM t0, t1 WHERE t0.c0 = $1 AND t1.c0 = $2", [1, key], None, None)
        # An ORDER BY on the second table's column is in no scan's order: the
        # pairs sorted, t1 looked up again for each.
        for nulls in ("", " NULLS FIRST", " NULLS LAST"):
            for limit in (None, 2):
                ordered(f"SELECT t0.c1, t1.c1 FROM t0, t1 WHERE t0.c0 = $1 AND t1.c0 = t0.c2 ORDER BY t1.c1 DESC{nulls}, t0.c1",
                        [1], lambda r: (r[1], r[0]), limit)
        m.check()
    finally:
        m.teardown()


def test_same_rows_compares_sort_keys_as_postgresql_orders_them():
    """Two rows tied on NaN are one run, in either order; -0.0 ties 0.0."""
    nan = math.nan
    pg = ((1, 2.0), (2, float("nan")), (3, float("nan")))
    key = lambda r: (r[1],)  # noqa: E731
    assert o.same_rows(((1, 2.0), (3, float("nan")), (2, nan)), pg, key, None) is None
    assert o.same_rows(((1, 2.0), (3, nan)), pg, key, 2) is None
    assert o.same_rows(((2, nan), (1, 2.0), (3, nan)), pg, key, None) == "a different order"
    assert o.same_rows(((1, -0.0), (2, 0.0)), ((2, 0.0), (1, -0.0)), key, None) is None


def test_nan_and_infinities_order_as_postgresql_orders_them(dsn):
    """NaN, Infinity and -Infinity in DOUBLE and REAL columns, sorted, keyed
    and looked up: PostgreSQL orders NaN above every other value and every
    NaN equal to every other, so rows tied on NaN form one run of equal keys
    (oracle.same_rows)."""
    nan, inf = math.nan, math.inf
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.DOUBLE), Column("c2", o.REAL)],
                       ["c0"]))
        m.create(Table("t1", [Column("c0", o.DOUBLE, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        rows = [(nan, inf), (-inf, nan), (inf, -inf), (nan, nan), (0.0, 1.5), (None, inf), (-0.0, None), (nan, -inf)]
        for k, (c1, c2) in enumerate(rows):
            m.insert("t0", {"c0": k, "c1": c1, "c2": c2}, all_columns=True)
        for k, v in enumerate([nan, inf, -inf, 2.5, -0.0]):
            m.insert("t1", {"c0": v, "c1": k}, all_columns=True)
        assert m.insert("t1", {"c0": nan, "c1": 9}, all_columns=True).error == "23505"  # one NaN key
        for limit in (None, 1, 3):
            m.select("t0", ["c0", "c1"], [], [("c1", False, None)], limit)
            m.select("t0", ["c0", "c1", "c2"], [], [("c1", True, None), ("c2", False, True)], limit)
            m.select("t0", ["c0", "c2"], [("c0", ">=", 1)], [("c2", True, False)], limit)
            m.select("t1", ["c0", "c1"], [], [("c0", True, None)], limit)
        for v in (nan, inf, -inf):
            assert len(m.lookup("t1", (v,)).rows) == 1
            m.select("t1", ["c0", "c1"], [("c0", ">=", v)], [("c0", False, None)])
        m.check()
    finally:
        m.teardown()


def test_full_scans_and_sorts(dsn):
    """Full scans, filtered by columns no key holds, UPDATE and DELETE over
    them, and sorts by any columns, NULLS FIRST or LAST, under LIMIT; a sort
    of a join, and of a DOUBLE PRECISION column (-0.0 and 0.0 equal). Each
    plan of each statement agrees with PostgreSQL."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.TEXT), Column("c2", o.DOUBLE),
                              Column("c3", o.INTEGER)], ["c0"]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        rows = [("b", 1.5, 3), (None, -0.0, None), ("a", 0.0, 1), ("b", None, 3), ("c", -2.0, 2), (None, 1.5, 1)]
        for k, (c1, c2, c3) in enumerate(rows):
            m.insert("t0", {"c0": k, "c1": c1, "c2": c2, "c3": c3}, all_columns=True)
            m.insert("t1", {"c0": k, "c1": None if k == 2 else k % 3}, all_columns=True)
        for limit in (None, 0, 1, 4):
            m.select("t0", ["c0", "c1"], [], [("c1", False, None)], limit)
            m.select("t0", ["c0", "c1", "c3"], [("c3", ">=", 1)], [("c3", True, False), ("c1", False, True)], limit)
            m.select("t0", ["c0", "c2"], [("c0", "<", 5)], [("c2", True, None), ("c0", False)], limit)
            m.select("t0", ["c0", "c2"], [], [("c2", False, True), ("c0", True)], limit)
        m.select("t0", ["c0"], [("c3", "=", 3)])
        m.select("t0", [], [("c1", ">", "a")], count="*")
        m.join_on("t1", "t0", [("c1", ">=", 0)], [("c0", ("col", "c0"))], [], [("t0", "c1"), ("t1", "c1")],
                  order=[("t0", "c1", False, None), ("t1", "c1", True, None)])
        m.write("t0", [("c3", "=", 3)], ("c1", "=", "z"))
        m.write("t0", [("c1", "<", "b")])
        m.write("t1", [], ("c1", "=", 7))
        m.check()
    finally:
        m.teardown()


def test_group_by(dsn):
    """GROUP BY one or two columns, NULLs and duplicates in the keys and the
    aggregated columns, of every aggregate, with HAVING, ORDER BY of the keys
    and LIMIT; in a structure or streamed in an index's order; over no rows;
    without GROUP BY, one row; over a join. Every plan agrees."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER), Column("c2", o.TEXT),
                              Column("c3", o.numeric(6, 2)), Column("c4", o.DOUBLE)], ["c0"]))
        m.create_index("t0", Index("t0_c1", [("c1", False)]))
        rows = ((1, 10, "a", decimal.Decimal("1.25"), 0.0), (2, 10, "b", None, -0.0), (3, None, "a", decimal.Decimal("-3.10"), math.nan),
                (4, 20, None, decimal.Decimal("2.00"), 1.5), (5, None, "c", None, None), (6, 10, "a", decimal.Decimal("0.05"), 0.0))
        for c0, c1, c2, c3, c4 in rows:
            m.insert("t0", {"c0": c0, "c1": c1, "c2": c2, "c3": c3, "c4": c4}, all_columns=True)
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        for c0, c1 in ((1, 10), (2, 10), (3, None), (4, 20)):
            m.insert("t1", {"c0": c0, "c1": c1}, all_columns=True)
        every = [("count", None, None, False), ("count", "t0", "c2", False), ("count", "t0", "c2", True),
                 ("sum", "t0", "c3", False), ("avg", "t0", "c1", False), ("avg", "t0", "c3", False),
                 ("min", "t0", "c3", False), ("max", "t0", "c1", False)]
        for where in ([], [("t0", "c0", ">=", 2)], [("t0", "c0", ">", 100)], [("t0", "c1", "=", 10)]):
            for group in ([("t0", "c1")], [("t0", "c2")], [("t0", "c4")], [("t0", "c1"), ("t0", "c2")], []):
                m.grouped(["t0"], where, group, every)
                m.grouped(["t0"], where, group, every[:2], having=[(("agg", every[0]), ">", 1)])
                if group:
                    m.grouped(["t0"], where, group, every[3:5], order=[(*group[0], True, None)], limit=2)
                    m.grouped(["t0"], where, group, [], having=[(("agg", every[6]), "<", decimal.Decimal("2.00"))],
                              order=[(*g, False, True) for g in group])
        m.grouped(["t0"], [], [("t0", "c1")], every[:1], having=[(("col", "t0", "c1"), ">=", 10)])
        # A HAVING on a DOUBLE key is a filter on one: no plan.
        m.grouped(["t0"], [], [("t0", "c4")], every[:1], having=[(("col", "t0", "c4"), ">", 0.0)])
        for links in ([("c1", "c1")], [("c0", "c0")]):
            m.grouped(["t0", "t1"], [], [("t1", "c1")], every[:4], links=links)
            m.grouped(["t0", "t1"], [("t0", "c0", ">=", 2)], [("t0", "c2")], every[:2], links=links,
                      order=[("t0", "c2", False, None)], limit=1)
    finally:
        m.teardown()


def test_a_grouped_hash_join_and_its_other_plan_both_order_the_groups(dsn):
    """GROUP BY over a hash join no key reaches, ORDER BY a grouped column:
    the hash join probes t0 in its key's order, so its groups come in order,
    and its choice point by the budget takes, past the count, the nested
    loop, which reads t1 first. That branch sorts its rows too. Alternatives
    agree forces both branches."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        m.create(Table("t1", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.INTEGER)], ["c0"]))
        for k in range(40):
            m.insert("t0", {"c0": k * 7 % 40, "c1": k % 4}, all_columns=True)
        for k in range(3):
            m.insert("t1", {"c0": k, "c1": k}, all_columns=True)
        count = ("count", None, None, False)
        for desc in (False, True):
            for limit in (None, 3):
                m.grouped(["t0", "t1"], [], [("t0", "c0")], [count], order=[("t0", "c0", desc, None)], limit=limit,
                          links=[("c1", "c1")])
        assert len(m.db._alternatives(
            "SELECT t0.c0, count(*) FROM t0, t1 WHERE t1.c1 = t0.c1 GROUP BY t0.c0 ORDER BY t0.c0", ())) >= 2
    finally:
        m.teardown()


def test_a_having_parameter_typed_by_the_where(dsn):
    """$1 compared with an INTEGER, SMALLINT or NUMERIC(18, 0) column in the
    WHERE, then with COUNT(*), a SUM, a MAX or a key in the HAVING, a
    NUMERIC(18, 0) one among them: PostgreSQL types it by its first use, the
    WHERE's, and so does the layer, a DECIMAL of scale 0 being an integer."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.INTEGER, nullable=False), Column("c1", o.SMALLINT), Column("c2", o.BIGINT),
                              Column("c3", o.numeric(18, 0))], ["c0"]))
        for k in range(12):
            m.insert("t0", {"c0": k, "c1": k % 3, "c2": None if k % 5 == 0 else k * 10,
                            "c3": None if k % 4 == 0 else decimal.Decimal(k % 5)}, all_columns=True)
        count = ("count", None, None, False)
        havings = (("agg", count), ("agg", ("sum", "t0", "c1", False)), ("agg", ("max", "t0", "c2", False)),
                   ("agg", ("max", "t0", "c3", False)), ("agg", ("min", "t0", "c3", False)), ("col", "t0", "c1"))
        for col, op in (("c0", ">"), ("c1", "<="), ("c3", ">=")):
            for v in (0, 1, 2, None):
                for what in havings:
                    aggs = [count] + ([what[1]] if what[0] == "agg" and what[1] != count else [])
                    m.grouped(["t0"], [("t0", col, op, v)], [("t0", "c1")], aggs, [(what, ">", Reuse(1))])
                m.grouped(["t0"], [("t0", col, op, v)], [("t0", "c3")], [count], [(("col", "t0", "c3"), "<", Reuse(1))])
                m.grouped(["t0"], [("t0", col, op, v)], [], [count], [(("agg", count), ">", Reuse(1))])
    finally:
        m.teardown()
