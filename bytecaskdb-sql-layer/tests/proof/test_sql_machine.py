# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The SQL layer against PostgreSQL: one Hypothesis state machine (#515).

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
join of two tables, each by its whole primary key. A model of the planner's rules (`served`) keeps the
machine to statements the layer plans; the rest must be refused with "no
plan".

The fixed cases at the end call the same steps in a set order.
"""

from __future__ import annotations

import array
import collections
import dataclasses
import decimal
import shutil
import tempfile

import bytecask_sql
import hypothesis.strategies as st
from hypothesis import note
from hypothesis.control import currently_in_test_context
from hypothesis.stateful import RuleBasedStateMachine, invariant, precondition, rule, run_state_machine_as_test

import oracle as o
from conftest import Oracle

TYPES = [o.BIGINT, o.INTEGER, o.SMALLINT, o.DOUBLE, o.REAL, o.numeric(6, 2), o.BOOLEAN, o.TEXT, o.varchar(4), o.BYTEA]
# Types a column + parameter is supported on, and how PostgreSQL types that
# parameter: as the column's type (numeric without the modifier).
ARITHMETIC = {"smallint": "smallint", "integer": "integer", "numeric(6,2)": "numeric"}
TABLE_NAMES = ["t0", "t1", "t2", "t3"]
BIGINT_LOW, BIGINT_HIGH = o.BIGINT.low, o.BIGINT.high


@dataclasses.dataclass
class Column:
    name: str
    type: o.Type
    nullable: bool = True
    default: object = None  # None: no DEFAULT clause

    def ddl(self, pg: bool, primary_key: bool = False) -> str:
        out = f"{self.name} {self.type.pg if pg else self.type.sql}"
        if primary_key:
            out += " PRIMARY KEY"
        if not self.nullable:
            out += " NOT NULL"
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
    if t.kind == "float":
        return st.floats(allow_nan=False, allow_infinity=False)
    if t.kind == "real":
        # Floats, doubles that round to one, and doubles past float's range
        # at either end (22003 on both).
        inside = st.one_of(st.floats(width=32, allow_nan=False, allow_infinity=False),
                           st.floats(-1e38, 1e38).filter(lambda v: v == 0 or abs(v) > 1e-37))
        return st.one_of(inside, st.sampled_from([1e39, -1e39, 1e-50, -1e-50]))
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
        # layer refuses it (docs/testing_design.md, *Known differences*).
        n = t.length
        return st.text(chars, max_size=n + 2).filter(lambda v: len(v) <= n or v[n:].strip(" "))
    return st.binary(max_size=6)


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


# What the planner serves (docs/frontend_design.md, *The planner's choices*,
# as slice 1 builds it), so the machine generates only statements it plans,
# and checks that the layer refuses the rest with "no plan". -------------


def key_paths(t: Table) -> list[tuple[bool, list[tuple[str, bool]]]]:
    """The primary key, then each index in creation order (so in object id
    order): whether it is the primary key, and its key's parts (an index's,
    then every part of the primary key, one the index names among them)."""
    out = [(True, [(c, False) for c in t.pk])]
    for ix in t.indexes:
        out.append((False, list(ix.columns) + [(c, False) for c in t.pk]))
    return out


def served(t: Table, conjuncts: list[tuple[str, str]], order: list[tuple[str, bool]], used: list[str]) -> bool:
    """Whether the planner plans a SELECT with these WHERE conjuncts
    (column, operator), ORDER BY items (column, DESC) and columns read."""
    eq = {c for c, op in conjuncts if op == "="}
    if all(c in eq for c in t.pk):  # a point lookup: any other conjunct is a residual
        return all(op == "=" and c in t.pk for c, op in conjuncts)
    ordered: list[tuple[str, bool]] = []
    for c, desc in order:
        if c not in eq and c not in [q for q, _ in ordered]:
            ordered.append((c, desc))
    candidates = []
    for oid, (is_pk, parts) in enumerate(key_paths(t)):
        run = 0
        while run < len(parts) and parts[run][0] in eq:
            run += 1
        ranged = run < len(parts) and any(c == parts[run][0] and op != "=" for c, op in conjuncts)
        if run == 0 and not ranged:
            continue
        serves, reverse = run + len(ordered) <= len(parts), False
        for i, (c, desc) in enumerate(ordered if serves else []):
            pc, pdesc = parts[run + i]
            rev = desc != pdesc
            if pc != c or (i > 0 and rev != reverse):
                serves = False
                break
            reverse = rev
        in_key = {c for c, _ in parts}
        covering = all(c in in_key and t.column(c).type.kind not in ("float", "real") for c in used)
        candidates.append(((run, ranged, is_pk, covering, -oid), serves, run, parts))
    candidates = [c for c in candidates if c[1]]
    if not candidates:
        return False
    _, _, run, parts = max(candidates, key=lambda c: c[0])
    run_cols = {c for c, _ in parts[:run]}
    for c, op in conjuncts:
        if op == "=" and c not in run_cols:
            return False  # a residual
        if op != "=" and (run == len(parts) or c != parts[run][0]):
            return False
    for i, (c, _) in enumerate(ordered):
        if not t.nullable(c) or any(q == c for q, _ in conjuncts):
            continue
        later = any(t.nullable(q) for q, _ in ordered[i + 1:])
        return i == 0 and not parts[run][1] and not later  # the two-range scan, or a refusal
    return True


class SqlMachine(RuleBasedStateMachine):
    dsn: str = ""  # set by the test that runs the machine

    def __init__(self):
        super().__init__()
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
        self.tables: dict[str, Table] = {}
        self.txn = None  # the layer's open transaction, if any

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
                shutil.rmtree(self.dir, ignore_errors=True)

    # The two sides --------------------------------------------------------

    def both(self, sql: str, params=(), pg_sql: str | None = None, types=None) -> o.Outcome:
        """Runs a statement on both sides; they must agree. Returns the layer's
        outcome. `types`: each parameter's column type, for the DECIMALs the
        layer takes scaled (o.to_layer)."""
        layer_params = [o.to_layer(v, t) for v, t in zip(params, types)] if types else params
        layer = o.run_layer(self.db if self.txn is None else self.txn, sql, layer_params)
        pg = o.run_pg(self.pg.conn, pg_sql or sql, params)
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

    def pg_aborted(self) -> bool:
        import psycopg

        return self.pg.conn.info.transaction_status == psycopg.pq.TransactionStatus.INERROR

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

    def create_without_pk(self, t: Table) -> None:
        # Known difference: the layer needs a primary key; PostgreSQL does not.
        out = o.run_layer(self.db, t.ddl(pg=False))
        assert out.error == "refused", f"{t.ddl(pg=False)}: a table without a primary key was accepted"

    def drop(self, name: str, if_exists: bool) -> o.Outcome:
        out = self.both(f"DROP TABLE {'IF EXISTS ' if if_exists else ''}{name}")
        if out.error is None:
            self.tables.pop(name, None)
        return out

    def insert(self, name: str, row: dict[str, object], all_columns: bool) -> o.Outcome:
        cols = list(row)
        params = [row[c] for c in cols]
        marks = ", ".join(f"${i + 1}" for i in range(len(cols)))
        target = "" if all_columns else f" ({', '.join(cols)})"
        sql = f"INSERT INTO {name}{target} VALUES ({marks})"
        t = self.tables.get(name)
        types = [t.column(c).type for c in cols] if t is not None else None
        return self.both(sql, params, types=types)

    def lookup(self, name: str, key: tuple, for_update: bool = False) -> o.Outcome:
        t = self.tables[name]
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

    def select(self, name: str, targets: list[str], where: list[tuple[str, str, object]],
               order: list[tuple[str, bool]] = (), limit: int | None = None, count: str | None = None) -> None:
        """SELECT targets (or COUNT(count), "*" for COUNT(*)) WHERE each
        (column, operator, value) ... ORDER BY ... LIMIT ...: on both sides
        when the planner serves it, else the layer must refuse it with no
        plan."""
        t = self.tables[name]
        cols = [c for c, _, _ in where]
        used = targets + ([count] if count not in (None, "*") else [])
        select = f"count({count})" if count else ", ".join(targets)
        conds = [f"{c} {op} ${i + 1}" for i, (c, op, _) in enumerate(where)]
        pg_conds = [f"{c} {op} {typed(t, c, i + 1)}" for i, (c, op, _) in enumerate(where)]
        tail = (" ORDER BY " + ", ".join(c + (" DESC" if d else "") for c, d in order) if order else "") + (
            f" LIMIT {limit}" if limit is not None else "")
        sql = f"SELECT {select} FROM {name} WHERE {' AND '.join(conds)}{tail}"
        params = [v for _, _, v in where]
        types = [t.column(c).type for c in cols]
        if not served(t, [(c, op) for c, op, _ in where], list(order), used):
            out = o.run_layer(self.db if self.txn is None else self.txn, sql, [o.to_layer(v, ty) for v, ty in zip(params, types)])
            if out.error == o.ABORTED:
                return  # the transaction was aborted before: nothing was planned
            assert out.error == "refused" and out.detail.startswith("no plan:"), f"{sql}: planned, expected no plan ({out})"
            if self.txn is not None and not self.pg_aborted():
                # A refused statement aborts the transaction on both sides, as
                # any statement PostgreSQL refuses does.
                assert self.txn.is_aborted
                assert o.run_pg(self.pg.conn, "SELECT no_such_column FROM " + name).error == "refused"
            return
        pg_sql = f"SELECT {select} FROM {name} WHERE {' AND '.join(pg_conds)}{tail}"
        if count:
            self.both(sql, params, pg_sql=pg_sql, types=types)  # one row, or none under LIMIT 0
            return
        layer = o.run_layer(self.db if self.txn is None else self.txn, sql, [o.to_layer(v, ty) for v, ty in zip(params, types)])
        full = o.run_pg(self.pg.conn, pg_sql.removesuffix(f" LIMIT {limit}" if limit is not None else ""), params)
        assert layer.error == full.error, f"{sql} {params!r}: the layer {layer.error or 'ran it'} ({layer.detail}), PostgreSQL {full.error or 'ran it'} ({full.detail})"
        if layer.error is not None:
            return
        key = (lambda r: tuple(r[targets.index(c)] for c, _ in order)) if order else None
        why = o.same_rows(layer.rows, full.rows, key, limit)
        assert why is None, f"{sql} {params!r}: {why}: the layer returned {layer.rows}, PostgreSQL {full.rows}"

    def add(self, name: str, col: str, op: str, operand, key: tuple, literal: bool = False) -> o.Outcome:
        """UPDATE … SET col = col op $1 WHERE the primary key; with `literal`,
        the operand is an integer literal in the text, typed by both sides
        as PostgreSQL types it."""
        t = self.tables[name]
        first = 1 if literal else 2
        where = " AND ".join(f"{c} = ${i + first}" for i, c in enumerate(t.pk))
        pg_where = " AND ".join(f"{c} = {typed(t, c, i + first)}" for i, c in enumerate(t.pk))
        key_types = [t.column(c).type for c in t.pk]
        if literal:
            return self.both(f"UPDATE {name} SET {col} = {col} {op} {operand} WHERE {where}", key,
                             pg_sql=f"UPDATE {name} SET {col} = {col} {op} {operand} WHERE {pg_where}", types=key_types)
        cast = ARITHMETIC[t.column(col).type.sql]
        return self.both(f"UPDATE {name} SET {col} = {col} {op} $1 WHERE {where}", (operand, *key),
                         pg_sql=f"UPDATE {name} SET {col} = {col} {op} $1::{cast} WHERE {pg_where}",
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
        return self.both(text(conds), params, pg_sql=text(pg_conds),
                         types=[a.column(c).type for c in a.pk] + [b.column(c).type for c in b.pk])

    def begin(self) -> None:
        # PostgreSQL's REPEATABLE READ, like the layer's, takes its snapshot
        # at the first statement, not at BEGIN.
        self.txn = self.db.begin()
        assert o.run_pg(self.pg.conn, "BEGIN ISOLATION LEVEL REPEATABLE READ").error is None

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
        return layer

    def rollback(self) -> None:
        self.txn.rollback()
        self.txn = None
        assert o.run_pg(self.pg.conn, "ROLLBACK").error is None

    def reopen(self) -> None:
        self.db.close()
        self.db = bytecask_sql.open(self.dir)

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
        columns = []
        for i in range(n):
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
            t.pk = []
            self.create_without_pk(t)
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
        self.insert(name, row, all_columns)

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
        self.lookup(t.name, key, for_update=data.draw(st.booleans(), label="FOR UPDATE"))

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
        existing row, in shuffled order. Returns them, the path's parts and
        the prefix's length."""
        is_pk, parts = data.draw(st.sampled_from(key_paths(t)), label="path")
        rows = self.existing_rows(t)
        row = data.draw(st.sampled_from(rows), label="row") if rows and data.draw(st.integers(0, 3), label="from a row?") else None
        names = [c.name for c in t.columns]
        def value(c: str):
            if row is not None and data.draw(st.integers(0, 4), label=f"{c} from the row?"):
                return row[names.index(c)]
            return data.draw(st.none() | values(t.column(c).type), label=c)
        run = data.draw(st.integers(0, len(parts)), label="prefix")
        where = []
        for c, _ in parts[:run]:
            if c not in [q for q, _, _ in where]:  # a key repeats a primary key column its index names
                where.append((c, "=", value(c)))
        if run < len(parts) and parts[run][0] in [c for c, _, _ in where]:
            pass  # a column the prefix compares, repeated in the key: no range on it
        elif run < len(parts) and (run == 0 or data.draw(st.booleans(), label="range?")):
            c = parts[run][0]
            bounds = data.draw(st.sampled_from([(">",), (">=",), ("<",), ("<=",), (">", "<"), (">=", "<="), (">=", "<")]),
                               label="bounds")
            where += [(c, op, value(c)) for op in bounds]
        return data.draw(st.permutations(where), label="where order"), parts, run

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def select_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, parts, run = self.where_on_a_path(data, t)
        if not where:
            return
        order: list[tuple[str, bool]] = []
        if data.draw(st.booleans(), label="order?"):
            # The parts after the prefix, each with or against its direction;
            # an equality-compared column now and then, which orders nothing.
            reverse = data.draw(st.booleans(), label="reverse")
            m = data.draw(st.integers(min(1, len(parts) - run), len(parts) - run), label="ordered parts")
            order = [(c, desc != reverse) for c, desc in parts[run:run + m]]
            if run and data.draw(st.integers(0, 3), label="an equal column?") == 0:
                order.insert(data.draw(st.integers(0, len(order)), label="at"), (parts[0][0], data.draw(st.booleans())))
            if order and data.draw(st.integers(0, 3), label="flip one?") == 0:
                i = data.draw(st.integers(0, len(order) - 1), label="flipped")
                order[i] = (order[i][0], not order[i][1])  # mostly unserved: the layer refuses it
        names = [c.name for c in t.columns]
        targets = data.draw(st.lists(st.sampled_from(names), min_size=1, max_size=3, unique=True), label="targets")
        targets += [c for c, _ in order if c not in targets]
        limit = data.draw(st.none() | st.sampled_from([0, 1, 2, 5]), label="limit")
        self.select(t.name, targets, where, order, limit)

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def count_by_key(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        where, _, _ = self.where_on_a_path(data, t)
        if not where:
            return
        arg = data.draw(st.sampled_from(["*"] + [c.name for c in t.columns]), label="count of")
        limit = data.draw(st.none() | st.sampled_from([0, 1]), label="limit")
        self.select(t.name, [], where, limit=limit, count=arg)

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
        operand = data.draw(st.none() | small | values(ty), label="operand")
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
        # ones at and past SMALLINT's and INTEGER's edges.
        literal = data.draw(st.one_of(st.integers(-5, 5), st.sampled_from([32767, 32768, -32768, 2**31 - 1, 2**31,
                                                                           2**40])), label="literal")
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
        assert m.insert("t9", {"c0": 3}, all_columns=False).error == "refused"
        assert m.lookup("t0", (1,)).error == "25P02"
        m.rollback()
    finally:
        m.teardown()


def test_create_and_drop_refusals(dsn):
    m = fixed(dsn)
    try:
        t = Table("t0", [Column("c0", o.SMALLINT)], ["c0"])
        assert m.create(t).error is None
        assert m.create(t).error == "refused"  # already exists
        assert m.create(Table("t1", [Column("c0", o.BIGINT), Column("c0", o.TEXT)], ["c0"])).error == "refused"
        assert m.create(Table("t1", [Column("c0", o.BIGINT)], ["zz"])).error == "refused"
        m.create_without_pk(Table("t1", [Column("c0", o.BIGINT)], []))
        assert m.drop("t2", if_exists=False).error == "refused"
        assert m.drop("t2", if_exists=True).error is None
        assert m.drop("t0", if_exists=False).error is None
        assert m.insert("t0", {"c0": 1}, all_columns=False).error == "refused"
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
    # Known difference. PostgreSQL gets the value as two float8 parameters
    # (pg_params gives each $1 its own %s) and returns no row; the layer
    # refuses a $n compared with columns of two SQL types, where it would
    # have keyed 1e39 for the REAL column as infinity and returned the inf row.
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
    """Each range refusal the layer makes, and the SQLSTATE the oracle gives
    it: every entry of RANGE_SQLSTATE is produced, so a message reworded in
    the layer fails here."""
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
        seen = set()
        for col, value, state in cases:
            params = [1] + [value if c == col else None for c in cols]
            marks = ", ".join(f"${n + 1}" for n in range(len(params)))
            try:
                db.execute(f"INSERT INTO r VALUES ({marks})", tuple(params))
            except bytecask_sql.ExecError as e:
                assert e.constraint == "range", (col, value, str(e))
                assert o.layer_sqlstate(e) == state, (col, value, str(e))
                seen.update(start for start in o.RANGE_SQLSTATE if str(e).startswith(start))
            else:
                raise AssertionError(f"{col} = {value!r} was written")
        assert seen == set(o.RANGE_SQLSTATE), set(o.RANGE_SQLSTATE) - seen
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
    assert o.pg_params("SELECT $2, $1", ("a", "b")) == ("SELECT %s, %s", ["b", "a"])
    # A % is escaped, and a $n inside a string literal is text.
    assert o.pg_params("SELECT '50%', '$1 it''s', $1", (7,)) == ("SELECT '50%%', '$1 it''s', %s", [7])


def test_rows_compare_by_type():
    assert o.strict([(True,)]) != o.strict([(1,)])
    assert o.strict([(1,)]) != o.strict([(1.0,)])
    assert o.strict([(-0.0,)]) != o.strict([(0.0,)])
    assert o.strict([("a", b"a", None)]) == o.strict([("a", b"a", None)])


def test_order_by_places_nulls_as_postgresql(dsn):
    """A nullable ordered column after an equality prefix: the two ranges
    put its NULLs last under ASC and first under DESC, one LIMIT across
    both. A nullable DESC index part is refused (no plan)."""
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
                m.select("t0", ["c1", "c0"], [("c2", "=", 1)], [("c1", desc)], limit)
                m.select("t0", ["c1", "c0"], [("c2", "=", 1)], [("c1", desc), ("c0", desc)], limit)
        # Only t0_ix1 leads with c1; its part is DESC.
        m.select("t0", ["c1"], [("c1", ">", "a")], [("c1", False)])
        assert not served(m.tables["t0"], [("c0", ">")], [("c1", False)], ["c1"])
        out = o.run_layer(m.db, "SELECT c1 FROM t0 WHERE c0 > $1 ORDER BY c1", (0,))
        assert out.error == "refused" and out.detail.startswith("no plan:"), out
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
    """SET col = col ± $1 on SMALLINT, INTEGER and NUMERIC(6, 2): a result
    outside the column's type is 22003 on both sides, NULL stays NULL."""
    m = fixed(dsn)
    try:
        m.create(Table("t0", [Column("c0", o.BIGINT, nullable=False), Column("c1", o.SMALLINT), Column("c2", o.INTEGER),
                              Column("c3", o.numeric(6, 2))], ["c0"]))
        m.insert("t0", {"c0": 1, "c1": 32760, "c2": -2147483640, "c3": decimal.Decimal("9999.00")}, all_columns=True)
        m.insert("t0", {"c0": 2, "c1": None, "c2": None, "c3": None}, all_columns=True)
        for key in (1, 2, 3):
            assert m.add("t0", "c1", "+", 7, (key,)).error is None
            assert m.add("t0", "c1", "+", 1, (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c2", "-", 9, (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c3", "+", decimal.Decimal("0.99"), (key,)).error is None
            assert m.add("t0", "c3", "+", decimal.Decimal("0.01"), (key,)).error == ("22003" if key == 1 else None)
            assert m.add("t0", "c3", "-", None, (key,)).error is None
        assert m.add("t0", "c2", "+", 2**31, (2,)).error == "22003"  # the operand is an INTEGER, on both
        # An operand of 18 digits or more is 22003 on the layer's side, before
        # the plan runs; PostgreSQL's NUMERIC takes it, and the sum overflows
        # NUMERIC(6, 2).
        m.insert("t0", {"c0": 4, "c1": 0, "c2": 0, "c3": decimal.Decimal("1.00")}, all_columns=True)
        assert m.add("t0", "c3", "+", decimal.Decimal(10**16), (4,)).error == "22003"
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

