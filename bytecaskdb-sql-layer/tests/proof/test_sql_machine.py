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

The fixed cases at the end call the same steps in a set order.
"""

from __future__ import annotations

import dataclasses
import shutil
import tempfile

import bytecask_sql
import hypothesis.strategies as st
from hypothesis import note
from hypothesis.control import currently_in_test_context
from hypothesis.stateful import RuleBasedStateMachine, invariant, precondition, rule, run_state_machine_as_test

import oracle as o
from conftest import Oracle

TYPES = [o.BIGINT, o.INTEGER, o.SMALLINT, o.DOUBLE, o.REAL, o.BOOLEAN, o.TEXT, o.varchar(4), o.BYTEA]
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
class Table:
    name: str
    columns: list[Column]
    pk: list[str]
    # Where the primary key is declared: "table", PRIMARY KEY (…); "column",
    # on each of its columns; "both", on its first column and the table.
    # Only "table", or "column" with one key column, declare one key.
    pk_style: str = "table"

    def column(self, name: str) -> Column:
        return next(c for c in self.columns if c.name == name)

    def ddl(self, pg: bool) -> str:
        on_column = set(self.pk if self.pk_style == "column" else self.pk[:1] if self.pk_style == "both" else [])
        parts = [c.ddl(pg, c.name in on_column) for c in self.columns]
        if self.pk and self.pk_style != "column":
            parts.append(f"PRIMARY KEY ({', '.join(self.pk)})")
        return f"CREATE TABLE {self.name} ({', '.join(parts)})"

    @property
    def select_all(self) -> str:
        return f"SELECT {', '.join(c.name for c in self.columns)} FROM {self.name}"


def values(t: o.Type, lookup: bool = False) -> st.SearchStrategy:
    """A value of the type's physical kind: mostly inside its declared
    range, sometimes outside it (a BIGINT for an INTEGER, a VARCHAR(n) of
    more than n characters), which both sides refuse. A lookup's REAL stays
    inside float's range: PostgreSQL refuses $n::real out of it, where a
    lookup in the layer finds nothing."""
    if t.kind == "int":
        edges = st.sampled_from([t.low, t.high, t.low - 1, t.high + 1]).filter(lambda v: BIGINT_LOW <= v <= BIGINT_HIGH)
        return st.one_of(st.integers(-3, 3), st.integers(t.low, t.high), edges, st.integers(BIGINT_LOW, BIGINT_HIGH))
    if t.kind == "float":
        return st.floats(allow_nan=False, allow_infinity=False)
    if t.kind == "real":
        # Floats, doubles that round to one, and, for writes, doubles past
        # float's range at either end (22003 on both).
        inside = st.one_of(st.floats(width=32, allow_nan=False, allow_infinity=False),
                           st.floats(-1e38, 1e38).filter(lambda v: v == 0 or abs(v) > 1e-37))
        return inside if lookup else st.one_of(inside, st.sampled_from([1e39, -1e39, 1e-50, -1e-50]))
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
    if t.kind == "bool":
        return st.booleans()
    printable = st.characters(min_codepoint=0x20, max_codepoint=0x7E, blacklist_characters="\\")
    text = st.text(printable, max_size=t.length or 4)
    return text.map(str.encode) if t.kind == "bytes" else text


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

    def teardown(self):
        # Every step runs, whichever fails, and the first failure is the one raised.
        try:
            self.db.close()
        finally:
            try:
                self.pg.close()
            finally:
                shutil.rmtree(self.dir, ignore_errors=True)

    # The two sides --------------------------------------------------------

    def both(self, sql: str, params=(), pg_sql: str | None = None) -> o.Outcome:
        """Runs a statement on both sides; they must agree. Returns the layer's outcome."""
        layer = o.run_layer(self.db, sql, params)
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
        return layer

    def existing_keys(self, t: Table) -> list[tuple]:
        out = o.run_pg(self.pg.conn, f"SELECT {', '.join(t.pk)} FROM {t.name}")
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
        return self.both(sql, params)

    def lookup(self, name: str, key: tuple) -> o.Outcome:
        t = self.tables[name]
        where = " AND ".join(f"{c} = ${i + 1}" for i, c in enumerate(t.pk))
        # The layer types $n by its column, so a REAL key is a real; psycopg
        # sends a float as double precision, so PostgreSQL is told.
        pg_where = " AND ".join(f"{c} = ${i + 1}" + ("::real" if t.column(c).type.kind == "real" else "")
                                for i, c in enumerate(t.pk))
        return self.both(f"{t.select_all} WHERE {where}", key, pg_sql=f"{t.select_all} WHERE {pg_where}")

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

    # Inserts are twice as likely as any other step: two rules run them.
    insert_one = precondition(lambda self: self.tables)(rule(data=st.data())(insert_row))
    insert_another = precondition(lambda self: self.tables)(rule(data=st.data())(insert_row))

    @precondition(lambda self: self.tables)
    @rule(data=st.data())
    def lookup_by_pk(self, data):
        t = self.tables[data.draw(st.sampled_from(sorted(self.tables)), label="table")]
        keys = self.existing_keys(t)
        if keys and data.draw(st.booleans(), label="existing key"):
            key = data.draw(st.sampled_from(keys), label="key")
        else:
            key = tuple(data.draw(st.none() | values(t.column(c).type, lookup=True), label=c) for c in t.pk)
        self.lookup(t.name, key)

    @rule()
    def reopen_layer(self):
        self.reopen()

    @invariant()
    def tables_agree(self):
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
        assert m.lookup("t0", (2**40,)).rows == ()  # a lookup is not a write: no refusal
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
    for params in ("1", b"\x01"):  # a str or bytes is not a sequence of parameters
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
