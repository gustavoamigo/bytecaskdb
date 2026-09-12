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
