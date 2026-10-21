# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""TPROC-C's Payment and Order-Status through SQL, against PostgreSQL
(docs/frontend_design.md, *Tests*, slice 1).

A small fixture (one warehouse, two districts, thirty customers each, a few
orders per customer) is loaded on both sides. Then a fixed seed runs a mix
of the two transactions on one thread: each statement of
docs/frontend_design.md's P1-P11 and O1-O5 goes to the layer in a
`Transaction` and to PostgreSQL in a REPEATABLE READ transaction, with the
client's logic (the customer by last name, the new balance and c_data)
between them, as HammerDB's driver keeps it. Every statement's result must
agree, and so must every table after the run.

The layer takes a DECIMAL parameter as its scaled integer and a TIMESTAMP
as microseconds since 1970; PostgreSQL takes decimal.Decimal and datetime.
"""

from __future__ import annotations

import datetime
import decimal
import pathlib
import random
import re
import shutil
import tempfile

import bytecask_sql

import oracle as o
from conftest import Oracle

EPOCH = datetime.datetime(1970, 1, 1)
SCHEMA_SOURCE = pathlib.Path(__file__).resolve().parents[1] / "tpcc_schema.cppm"


def schema() -> list[str]:
    """The fixture's DDL, read from tests/tpcc_schema.cppm, the one
    definition of it: each element of kTpccSchema, its adjacent string
    literals joined."""
    body = SCHEMA_SOURCE.read_text().split("kTpccSchema{", 1)[1].split("};", 1)[0]
    out, current = [], ""
    for literal, comma in re.findall(r'"((?:[^"\\]|\\.)*)"|(,)\s*\n', body):
        if comma:
            out.append(current)
            current = ""
        else:
            current += literal
    return out + ([current] if current else [])


def pg_ddl(ddl: str) -> str:
    """Text compares as bytes on PostgreSQL's side too."""
    return re.sub(r"(VARCHAR\(\d+\))", r'\1 COLLATE "C"', ddl)


# A parameter's kind: how each side takes it.
INT, TEXT, DEC2, DEC4, TS = "int", "text", "dec2", "dec4", "ts"


def to_layer(v, kind: str):
    if v is None:
        return None
    if kind in (DEC2, DEC4):
        return int(v.scaleb(2 if kind == DEC2 else 4))
    if kind == TS:
        return (v - EPOCH) // datetime.timedelta(microseconds=1)
    return v


def from_pg(v):
    """PostgreSQL's value as the layer returns it: a TIMESTAMP as its
    microseconds."""
    if isinstance(v, datetime.datetime):
        return (v - EPOCH) // datetime.timedelta(microseconds=1)
    return v


class Pair:
    """The two sides, one statement at a time: the layer's transaction and
    PostgreSQL's connection."""

    def __init__(self, dsn: str):
        self.pg = Oracle(dsn)
        self.dir = tempfile.mkdtemp(prefix="bytecask_tpcc_")
        self.db = bytecask_sql.open(self.dir)
        self.txn = None
        self.statements = 0

    def close(self):
        try:
            self.db.close()
        finally:
            self.pg.close()
            shutil.rmtree(self.dir, ignore_errors=True)

    def run(self, sql: str, params=(), kinds=(), key=None) -> tuple:
        """Runs one statement on both sides and returns the layer's rows.
        `key`: the ORDER BY key of a row, whose ties may come back in any
        order."""
        layer = o.run_layer(self.txn or self.db, sql, [to_layer(v, k) for v, k in zip(params, kinds)])
        pg = o.run_pg(self.pg.conn, sql, params)
        pg_rows = tuple(tuple(from_pg(v) for v in row) for row in pg.rows)
        assert layer.error is None and pg.error is None, f"{sql} {params!r}: layer {layer.detail}, PostgreSQL {pg.detail}"
        if key is None:
            assert o.strict(layer.rows) == o.strict(pg_rows), f"{sql} {params!r}: the layer {layer.rows}, PostgreSQL {pg_rows}"
        else:
            why = o.same_rows(layer.rows, pg_rows, key, None)
            assert why is None, f"{sql} {params!r}: {why}: the layer {layer.rows}, PostgreSQL {pg_rows}"
        assert layer.affected == pg.affected, f"{sql} {params!r}: the layer changed {layer.affected}, PostgreSQL {pg.affected}"
        self.statements += 1
        return layer.rows

    def begin(self):
        self.txn = self.db.begin()
        assert o.run_pg(self.pg.conn, "BEGIN ISOLATION LEVEL REPEATABLE READ").error is None

    def commit(self):
        assert self.txn.commit() == "COMMIT"
        self.txn = None
        assert o.run_pg(self.pg.conn, "COMMIT").error is None


SYLLABLES = ["BAR", "OUGHT", "ABLE", "PRI", "PRES", "ESE", "ANTI", "CALLY", "ATION", "EING"]


def last_name(n: int) -> str:
    return "".join(SYLLABLES[int(d)] for d in f"{n:03d}")


def letters(rng: random.Random, lo: int, hi: int) -> str:
    return "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(rng.randint(lo, hi)))


def money(rng: random.Random, lo: int, hi: int) -> decimal.Decimal:
    return decimal.Decimal(rng.randint(lo * 100, hi * 100)).scaleb(-2)


def stamp(rng: random.Random) -> datetime.datetime:
    return datetime.datetime(2026, 1, 1) + datetime.timedelta(seconds=rng.randint(0, 10**7))


DISTRICTS, CUSTOMERS = 2, 30


def load(p: Pair, rng: random.Random) -> dict:
    """The fixture, in one transaction on each side. Returns the next order
    id per district."""
    for ddl in schema():
        assert o.run_layer(p.db, ddl).error is None
        assert o.run_pg(p.pg.conn, pg_ddl(ddl)).error is None
    p.begin()
    p.run("INSERT INTO warehouse (w_id, w_ytd, w_tax, w_name, w_street_1, w_street_2, w_city, w_state, w_zip) "
          "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
          (1, decimal.Decimal("300000.00"), decimal.Decimal("0.1000"), "WH1", "S1", "S2", "CITY", "ST", "123451111"),
          (INT, DEC2, DEC4, TEXT, TEXT, TEXT, TEXT, TEXT, TEXT))
    next_o_id = {}
    for d in range(1, DISTRICTS + 1):
        p.run("INSERT INTO district (d_id, d_w_id, d_ytd, d_tax, d_next_o_id, d_name, d_street_1, d_street_2, d_city, "
              "d_state, d_zip) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)",
              (d, 1, decimal.Decimal("30000.00"), decimal.Decimal("0.0500"), 1, f"D{d}", "S1", "S2", "CITY", "ST",
               "123451111"),
              (INT, INT, DEC2, DEC4, INT, TEXT, TEXT, TEXT, TEXT, TEXT, TEXT))
        for c in range(1, CUSTOMERS + 1):
            # Few last names, so a name holds several customers; some first
            # names NULL, which ORDER BY c_first puts last.
            first = None if rng.random() < 0.2 else letters(rng, 3, 6)
            p.run("INSERT INTO customer (c_id, c_d_id, c_w_id, c_first, c_middle, c_last, c_street_1, c_street_2, "
                  "c_city, c_state, c_zip, c_phone, c_since, c_credit, c_credit_lim, c_discount, c_balance, "
                  "c_ytd_payment, c_payment_cnt, c_delivery_cnt, c_data) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, "
                  "$10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21)",
                  (c, d, 1, first, "OE", last_name(rng.randint(0, 5)), "S1", "S2", "CITY", "ST", "123451111",
                   "0123456789", stamp(rng), rng.choice(["GC", "BC"]), decimal.Decimal("50000.00"),
                   decimal.Decimal(rng.randint(0, 5000)).scaleb(-4), decimal.Decimal("-10.00"),
                   decimal.Decimal("10.00"), 1, 0, letters(rng, 30, 60)),
                  (INT, INT, INT, TEXT, TEXT, TEXT, TEXT, TEXT, TEXT, TEXT, TEXT, TEXT, TS, TEXT, DEC2, DEC4, DEC2,
                   DEC2, INT, INT, TEXT))
        o_id = 1
        for c in range(1, CUSTOMERS + 1):
            for _ in range(rng.randint(0, 2)):
                lines = rng.randint(1, 4)
                p.run("INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local) "
                      "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                      (o_id, d, 1, c, stamp(rng), rng.choice([None, rng.randint(1, 10)]), lines, 1),
                      (INT, INT, INT, INT, TS, INT, INT, INT))
                for n in range(1, lines + 1):
                    p.run("INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, "
                          "ol_quantity, ol_amount, ol_delivery_d, ol_dist_info) "
                          "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                          (o_id, d, 1, n, rng.randint(1, 1000), 1, 5, money(rng, 0, 9999),
                           rng.choice([None, stamp(rng)]), letters(rng, 24, 24)),
                          (INT, INT, INT, INT, INT, INT, INT, DEC2, TS, TEXT))
                o_id += 1
        next_o_id[d] = o_id
    p.commit()
    return next_o_id


COUNT_BY_NAME = "SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3"


def customer_by_name(p: Pair, sql: str, params: tuple, kinds: tuple, first: int, c_last: str, d: int) -> tuple | None:
    """The customer a last name selects: the middle of those it names, by
    c_first (ceil(n / 2), 1-based), as TPC-C's profile has it. Customers
    with one c_first tie: the layer's row is the one taken, and PostgreSQL's
    run of ties must hold the same rows. `first`: c_first's position in the
    rows."""
    [(n,)] = p.run(COUNT_BY_NAME, (c_last, d, 1), (TEXT, INT, INT))
    rows = p.run(sql, params, kinds, key=lambda r: (r[first],))
    assert len(rows) == n
    return rows[(n + 1) // 2 - 1] if rows else None


def rollback(p: Pair) -> None:
    p.txn.rollback()
    p.txn = None
    assert o.run_pg(p.pg.conn, "ROLLBACK").error is None


def payment(p: Pair, rng: random.Random, h_id: int) -> None:
    d = rng.randint(1, DISTRICTS)
    amount = money(rng, 1, 5000)
    p.begin()
    p.run("UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2", (amount, 1), (DEC2, INT))
    [(*_, w_name)] = p.run("SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse "
                           "WHERE w_id = $1", (1,), (INT,))
    p.run("UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3", (amount, 1, d), (DEC2, INT, INT))
    [(*_, d_name)] = p.run("SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM district "
                           "WHERE d_w_id = $1 AND d_id = $2", (1, d), (INT, INT))
    if rng.random() < 0.6:
        c_last = last_name(rng.randint(0, 6))
        row = customer_by_name(
            p, "SELECT c_first, c_middle, c_id, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, "
            "c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 "
            "AND c_last = $3 ORDER BY c_first", (1, d, c_last), (INT, INT, TEXT), 0, c_last, d)
        if row is None:  # no customer of that name: the harness rolls back
            rollback(p)
            return
        c_id, credit, balance = row[2], row[9], row[12]
    else:
        c_id = rng.randint(1, CUSTOMERS)
        [row] = p.run("SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, "
                      "c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer "
                      "WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3", (1, d, c_id), (INT, INT, INT))
        credit, balance = row[9], row[12]
    balance -= amount
    if credit == "BC":
        [(c_data,)] = p.run("SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                            (1, d, c_id), (INT, INT, INT))
        c_data = f"{c_id} {d} 1 {d} 1 {amount} | {c_data}"[:500]
        p.run("UPDATE customer SET c_balance = $1, c_data = $2 WHERE c_w_id = $3 AND c_d_id = $4 AND c_id = $5",
              (balance, c_data, 1, d, c_id), (DEC2, TEXT, INT, INT, INT))
    else:
        p.run("UPDATE customer SET c_balance = $1 WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4",
              (balance, 1, d, c_id), (DEC2, INT, INT, INT))
    p.run("INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data) "
          "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
          (h_id, d, 1, c_id, d, 1, stamp(rng), amount, f"{w_name}    {d_name}"),
          (INT, INT, INT, INT, INT, INT, TS, DEC2, TEXT))
    p.commit()


def order_status(p: Pair, rng: random.Random) -> None:
    d = rng.randint(1, DISTRICTS)
    p.begin()
    if rng.random() < 0.6:
        c_last = last_name(rng.randint(0, 6))
        row = customer_by_name(
            p, "SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 "
            "AND c_w_id = $3 ORDER BY c_first", (c_last, d, 1), (TEXT, INT, INT), 1, c_last, d)
        c_id = row[3] if row is not None else None
    else:
        c_id = rng.randint(1, CUSTOMERS)
        p.run("SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_id = $1 AND c_d_id = $2 "
              "AND c_w_id = $3", (c_id, d, 1), (INT, INT, INT))
    if c_id is not None:
        orders = p.run("SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = $1 AND o_w_id = $2 "
                       "AND o_c_id = $3 ORDER BY o_id DESC LIMIT 1", (d, 1, c_id), (INT, INT, INT))
        if orders:
            p.run("SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line "
                  "WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3", (orders[0][0], d, 1), (INT, INT, INT),
                  key=lambda r: ())
    p.commit()


def tables_agree(p: Pair) -> None:
    for t in p.db.catalog():
        cols = ", ".join(c["name"] for c in t["columns"])
        layer = p.db.table_rows(t["name"])["rows"]
        pg = o.run_pg(p.pg.conn, f"SELECT {cols} FROM {t['name']} ORDER BY {', '.join(t['primary_key'])}")
        pg_rows = tuple(tuple(from_pg(v) for v in row) for row in pg.rows)
        assert o.strict(layer) == o.strict(pg_rows), f"table {t['name']}: the layer and PostgreSQL differ"


def test_payment_and_order_status_agree_with_postgresql(dsn):
    p = Pair(dsn)
    try:
        rng = random.Random(487)
        load(p, rng)
        for h_id in range(60):
            if rng.random() < 0.5:
                payment(p, rng, h_id)
            else:
                order_status(p, rng)
        tables_agree(p)
        assert p.statements > 400
    finally:
        p.close()
