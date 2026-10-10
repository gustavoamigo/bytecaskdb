# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_pk_range_read.py — primary-key ranges read in batches return what
# InnoDB returns for the same data and statements: range shapes over integer,
# composite, signed and temporal keys, ranges inside a transaction with its own
# writes, and statements that write the rows they are scanning. A key the
# batched path does not take (VARCHAR) is checked to go the other way.

import random

import pymysql
import pytest

DB = "pkrange"
ENGINES = ("bytecaskdb", "InnoDB")


def _batched_ranges(cur):
    cur.execute("SHOW ENGINE BYTECASKDB STATUS")
    status = cur.fetchone()[2]
    for line in status.splitlines():
        name, _, value = line.partition(": ")
        if name == "plugin.batched_pk_ranges":
            return int(value)
    raise AssertionError("plugin.batched_pk_ranges missing from engine status")


def _create(cur, name, ddl, rows):
    for engine in ENGINES:
        table = f"{DB}.{name}_{engine.lower()}"
        cur.execute(f"CREATE TABLE {table} ({ddl}) ENGINE={engine}")
        if rows:
            placeholders = ",".join(["%s"] * len(rows[0]))
            cur.executemany(f"INSERT INTO {table} VALUES ({placeholders})", rows)


def _same(cur, name, where, order="", extra=""):
    results = []
    for engine in ENGINES:
        table = f"{DB}.{name}_{engine.lower()}"
        cur.execute(f"SELECT * FROM {table} FORCE INDEX (PRIMARY) WHERE {where} {order} {extra}")
        results.append(list(cur.fetchall()))
    if not order:
        results = [sorted(r) for r in results]
    assert results[0] == results[1], (name, where, order, extra)
    return results[0]


def _table_contents(cur, name):
    out = []
    for engine in ENGINES:
        cur.execute(f"SELECT * FROM {DB}.{name}_{engine.lower()} ORDER BY 1, 2")
        out.append(list(cur.fetchall()))
    return out


@pytest.fixture
def cur(make_connection):
    conn = make_connection()
    with conn.cursor() as c:
        c.execute(f"DROP DATABASE IF EXISTS {DB}")
        c.execute(f"CREATE DATABASE {DB}")
        yield c
        c.execute(f"DROP DATABASE IF EXISTS {DB}")
    conn.close()


def test_integer_key_ranges_match_innodb(cur):
    rows = [(i * 3, f"v{i}") for i in range(1, 1500)]
    _create(cur, "t", "id INT NOT NULL PRIMARY KEY, v VARCHAR(20)", rows)
    rng = random.Random(1010)
    before = _batched_ranges(cur)
    for _ in range(150):
        a, b = sorted(rng.randrange(-20, 4600) for _ in range(2))
        lo = rng.choice((">=", ">"))
        hi = rng.choice(("<=", "<"))
        _same(cur, "t", f"id {lo} {a} AND id {hi} {b}")
        _same(cur, "t", f"id {lo} {a} AND id {hi} {b}", order="ORDER BY id")
        _same(cur, "t", f"id BETWEEN {a} AND {b}", order="ORDER BY id",
              extra=f"LIMIT {rng.randrange(1, 40)}")
    # Empty and single-row ranges.
    _same(cur, "t", "id > 30 AND id < 33")
    _same(cur, "t", "id >= 30 AND id <= 30 + 0")
    _same(cur, "t", "id BETWEEN 31 AND 32")
    assert _batched_ranges(cur) > before


def test_composite_signed_and_temporal_keys_match_innodb(cur):
    rng = random.Random(77)
    comp = sorted({(rng.randrange(0, 40), rng.randrange(-500, 500)) for _ in range(3000)})
    _create(cur, "c", "a INT NOT NULL, b BIGINT NOT NULL, v INT, PRIMARY KEY (a, b)",
            [(a, b, a * 1000 + b) for a, b in comp])
    signed = [(i * 7 - 5000, i) for i in range(1500)]
    _create(cur, "s", "id BIGINT NOT NULL PRIMARY KEY, v INT", signed)
    dts = [(f"2026-01-{1 + i % 28:02d} {i % 24:02d}:{i % 60:02d}:{i % 59:02d}.{i % 1000:03d}", i)
           for i in range(1200)]
    _create(cur, "d", "ts DATETIME(3) NOT NULL PRIMARY KEY, v INT",
            list({d: (d, v) for d, v in dts}.values()))

    before = _batched_ranges(cur)
    for _ in range(80):
        x, y = sorted(rng.randrange(-2, 42) for _ in range(2))
        p, q = sorted(rng.randrange(-520, 520) for _ in range(2))
        _same(cur, "c", f"a BETWEEN {x} AND {y}")
        _same(cur, "c", f"a > {x} AND a < {y}")
        _same(cur, "c", f"a = {x} AND b BETWEEN {p} AND {q}", order="ORDER BY a, b")
        _same(cur, "c", f"a = {x} AND b > {p} AND b < {q}")
        _same(cur, "c", f"(a, b) > ({x}, {p}) AND a <= {y}")
        s, t = sorted(rng.randrange(-5100, 5600) for _ in range(2))
        _same(cur, "s", f"id >= {s} AND id < {t}", order="ORDER BY id")
    _same(cur, "d", "ts BETWEEN '2026-01-03' AND '2026-01-09 12:00:00'",
          order="ORDER BY ts")
    _same(cur, "d", "ts > '2026-01-05 05:05:05.005' AND ts < '2026-01-20'")
    assert _batched_ranges(cur) > before


def _handler_reads(cur):
    cur.execute("SHOW SESSION STATUS WHERE Variable_name IN "
                "('Handler_read_key', 'Handler_read_next')")
    return {name: int(value) for name, value in cur.fetchall()}


def test_range_reads_no_row_past_its_ends(cur):
    # The server re-checks the WHERE clause on every row it is handed, so a
    # row read past an end is filtered out and only shows in the counters:
    # one key read to open the range, then one next per row, the last one
    # returning end of range.
    rows = [(i, f"v{i}") for i in range(1, 400)]
    _create(cur, "e", "id INT NOT NULL PRIMARY KEY, v VARCHAR(20)", rows)
    table = f"{DB}.e_bytecaskdb"
    for where, n in [("id >= 100 AND id <= 150", 51), ("id > 100 AND id < 150", 49),
                     ("id >= 100 AND id < 150", 50), ("id > 100 AND id <= 150", 50),
                     ("id BETWEEN 1 AND 399", 399), ("id > 150 AND id < 151", 0)]:
        before = _handler_reads(cur)
        cur.execute(f"SELECT id FROM {table} FORCE INDEX (PRIMARY) WHERE {where}")
        assert len(cur.fetchall()) == n, where
        after = _handler_reads(cur)
        assert after["Handler_read_key"] - before["Handler_read_key"] == 1, where
        expected_next = n if n else 0
        assert after["Handler_read_next"] - before["Handler_read_next"] == expected_next, where


def test_string_key_takes_the_other_path(cur):
    rows = [(f"k{i:05d}", i) for i in range(500)]
    _create(cur, "str", "id VARCHAR(10) NOT NULL PRIMARY KEY, v INT", rows)
    before = _batched_ranges(cur)
    _same(cur, "str", "id BETWEEN 'k00100' AND 'k00200'", order="ORDER BY id")
    _same(cur, "str", "id > 'k00100' AND id < 'k00300'")
    assert _batched_ranges(cur) == before


def test_ranges_see_the_transactions_own_writes(cur):
    rows = [(i, f"v{i}") for i in range(1, 2000)]
    _create(cur, "tx", "id INT NOT NULL PRIMARY KEY, v VARCHAR(20)", rows)
    rng = random.Random(5)
    cur.execute("BEGIN")
    for engine in ENGINES:
        table = f"{DB}.tx_{engine.lower()}"
        cur.execute(f"INSERT INTO {table} VALUES (2500, 'new'), (-3, 'neg'), (1000000, 'far')")
        cur.execute(f"DELETE FROM {table} WHERE id BETWEEN 300 AND 340")
        cur.execute(f"UPDATE {table} SET v = 'upd' WHERE id IN (5, 500, 1500)")
    for _ in range(60):
        a, b = sorted(rng.randrange(-10, 2600) for _ in range(2))
        _same(cur, "tx", f"id BETWEEN {a} AND {b}", order="ORDER BY id")
    cur.execute("COMMIT")


@pytest.mark.parametrize("statement", [
    "UPDATE {t} SET v = CONCAT(v, 'u') WHERE id BETWEEN 100 AND 400",
    "UPDATE {t} SET id = id + 1000 WHERE id BETWEEN 100 AND 200",
    "UPDATE {t} SET id = id + 3 WHERE id BETWEEN 300 AND 350 ORDER BY id DESC",
    "UPDATE {t} SET id = id - 1 WHERE id BETWEEN 600 AND 700 ORDER BY id",
    "DELETE FROM {t} WHERE id BETWEEN 500 AND 600",
    "INSERT INTO {t} SELECT id + 5000, v FROM {t} WHERE id BETWEEN 10 AND 90",
])
@pytest.mark.parametrize("in_transaction", [False, True])
def test_writes_during_a_range_scan_match_innodb(cur, statement, in_transaction):
    rows = [(i * 2, f"v{i}") for i in range(1, 600)]
    _create(cur, "w", "id INT NOT NULL PRIMARY KEY, v VARCHAR(40)", rows)
    if in_transaction:
        cur.execute("BEGIN")
    for engine in ENGINES:
        table = f"{DB}.w_{engine.lower()}"
        if in_transaction:
            # Buffered rows inside the range the statement scans.
            cur.execute(f"INSERT INTO {table} VALUES (151, 'b'), (333, 'b'), (651, 'b')")
            cur.execute(f"DELETE FROM {table} WHERE id IN (120, 340, 680)")
        try:
            cur.execute(statement.format(t=table))
        except pymysql.err.IntegrityError:
            pass  # both engines must refuse it; compared below
    bcdb, innodb = _table_contents(cur, "w")
    assert bcdb == innodb, statement
    if in_transaction:
        cur.execute("COMMIT")
    bcdb, innodb = _table_contents(cur, "w")
    assert bcdb == innodb, statement
