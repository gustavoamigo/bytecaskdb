# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_records_in_range.py — the optimizer's row estimate for a key range is
# an exact count for selective ranges (PK and secondary index), including
# rows buffered in the current transaction.

import pymysql


def _explain_rows(cur, sql):
    cur.execute("EXPLAIN " + sql)
    row = cur.fetchone()
    # id, select_type, table, type, possible_keys, key, key_len, ref, rows, Extra
    return row[3], int(row[8])


def test_range_estimates_are_exact_for_selective_ranges(make_connection):
    conn = make_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("DROP DATABASE IF EXISTS rir")
            cur.execute("CREATE DATABASE rir")
            cur.execute(
                "CREATE TABLE rir.t (id INT PRIMARY KEY, k INT NOT NULL, INDEX idx_k (k)) "
                "ENGINE=bytecaskdb"
            )
            cur.execute(
                "INSERT INTO rir.t VALUES "
                + ",".join(f"({i}, {i % 50})" for i in range(1, 501))
            )
            cur.execute("ANALYZE TABLE rir.t")
            cur.fetchall()

            # PK ranges: inclusive, exclusive, and open-ended.
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t WHERE id BETWEEN 10 AND 14")
            assert typ == "range" and rows == 5, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t WHERE id > 490")
            assert rows == 10, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t WHERE id < 4")
            assert rows == 3, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t WHERE id >= 100 AND id < 103")
            assert rows == 3, (typ, rows)

            # Secondary index: each k value has 10 rows.
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t FORCE INDEX (idx_k) WHERE k = 7")
            assert rows == 10, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t FORCE INDEX (idx_k) WHERE k BETWEEN 7 AND 8")
            assert rows == 20, (typ, rows)

            # Uncommitted rows in this transaction count too.
            cur.execute("BEGIN")
            cur.execute("INSERT INTO rir.t VALUES (1001, 7), (1002, 7)")
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t FORCE INDEX (idx_k) WHERE k = 7")
            assert rows == 12, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir.t WHERE id > 1000")
            assert rows == 2, (typ, rows)
            cur.execute("ROLLBACK")

            cur.execute("DROP DATABASE IF EXISTS rir")
    finally:
        conn.close()


def test_range_estimates_count_this_transactions_deletes_and_updates(make_connection):
    conn = make_connection()
    try:
        with conn.cursor() as cur:
            cur.execute("DROP DATABASE IF EXISTS rir2")
            cur.execute("CREATE DATABASE rir2")
            cur.execute(
                "CREATE TABLE rir2.t (id INT PRIMARY KEY, k INT NOT NULL, INDEX idx_k (k)) "
                "ENGINE=bytecaskdb"
            )
            cur.execute(
                "INSERT INTO rir2.t VALUES "
                + ",".join(f"({i}, {i % 50})" for i in range(1, 501))
            )

            cur.execute("BEGIN")
            # Committed rows deleted, and new ones inserted, inside the range.
            cur.execute("DELETE FROM rir2.t WHERE id BETWEEN 10 AND 14")
            cur.execute("INSERT INTO rir2.t VALUES (1001, 3), (1002, 3)")
            typ, rows = _explain_rows(cur, "SELECT id FROM rir2.t WHERE id BETWEEN 10 AND 20")
            assert rows == 6, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir2.t WHERE id > 999")
            assert rows == 2, (typ, rows)

            # An update moves a row between index values: k = 7 loses one,
            # k = 8 gains one.
            cur.execute("UPDATE rir2.t SET k = 8 WHERE id = 7")
            typ, rows = _explain_rows(cur, "SELECT id FROM rir2.t FORCE INDEX (idx_k) WHERE k = 7")
            assert rows == 9, (typ, rows)
            typ, rows = _explain_rows(cur, "SELECT id FROM rir2.t FORCE INDEX (idx_k) WHERE k = 8")
            assert rows == 11, (typ, rows)

            # Every row of a non-empty range deleted: the estimate is 1, never
            # 0, which the server would take as "certainly empty".
            cur.execute("DELETE FROM rir2.t WHERE id BETWEEN 100 AND 104")
            typ, rows = _explain_rows(cur, "SELECT id FROM rir2.t WHERE id BETWEEN 100 AND 104")
            assert rows == 1, (typ, rows)

            # More buffered rows in one range than the 1,024-key cap: the count
            # stops at the cap.
            cur.execute(
                "INSERT INTO rir2.t VALUES "
                + ",".join(f"({i}, 1)" for i in range(5001, 6101))
            )
            typ, rows = _explain_rows(cur, "SELECT id FROM rir2.t WHERE id > 5000")
            assert rows >= 1024, (typ, rows)
            cur.execute("ROLLBACK")

            cur.execute("DROP DATABASE IF EXISTS rir2")
    finally:
        conn.close()
