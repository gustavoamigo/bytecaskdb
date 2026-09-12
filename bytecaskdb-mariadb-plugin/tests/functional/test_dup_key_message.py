# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_dup_key_message.py — duplicate-key errors carry the standard code and
# message on every INSERT path, including the deferred (commit-time) check
# used for plain autocommit INSERTs.

import pytest
import pymysql


def _run(make_connection, *sqls):
    conn = make_connection()
    try:
        with conn.cursor() as cur:
            for sql in sqls:
                cur.execute(sql)
    finally:
        conn.close()


def _expect_dup(cur, sql, value, key="PRIMARY"):
    with pytest.raises(pymysql.err.IntegrityError) as ei:
        cur.execute(sql)
    code, msg = ei.value.args[0], ei.value.args[1]
    assert code == 1062, f"expected ER_DUP_ENTRY (1062), got {code}: {msg}"
    assert f"Duplicate entry '{value}' for key '{key}'" in msg, msg


def test_duplicate_pk_message_on_all_insert_paths(make_connection):
    _run(
        make_connection,
        "DROP DATABASE IF EXISTS dupmsg",
        "CREATE DATABASE dupmsg",
        "CREATE TABLE dupmsg.t (id INT PRIMARY KEY, v VARCHAR(16)) ENGINE=bytecaskdb",
        "CREATE TABLE dupmsg.c (a INT NOT NULL, b VARCHAR(8) NOT NULL, v INT, PRIMARY KEY (a, b)) ENGINE=bytecaskdb",
        "INSERT INTO dupmsg.t VALUES (7, 'seven')",
        "INSERT INTO dupmsg.c VALUES (1, 'x', 0)",
    )
    conn = make_connection()
    try:
        with conn.cursor() as cur:
            # Plain autocommit INSERT: deferred check, reported at commit.
            _expect_dup(cur, "INSERT INTO dupmsg.t VALUES (7, 'again')", "7")
            # Multi-row: the duplicate is the second row.
            _expect_dup(cur, "INSERT INTO dupmsg.t VALUES (8, 'eight'), (7, 'again')", "7")
            # Composite PK renders as 'a-b'.
            _expect_dup(cur, "INSERT INTO dupmsg.c VALUES (1, 'x', 1)", "1-x")
            # Explicit transaction: eager check inside write_row.
            cur.execute("BEGIN")
            _expect_dup(cur, "INSERT INTO dupmsg.t VALUES (7, 'again')", "7")
            cur.execute("ROLLBACK")
            # Nothing leaked from the failed statements.
            cur.execute("SELECT id, v FROM dupmsg.t ORDER BY id")
            assert cur.fetchall() == ((7, "seven"),)
    finally:
        conn.close()
    _run(make_connection, "DROP DATABASE IF EXISTS dupmsg")
