# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_desc_autoinc.py — after a restart, the AUTO_INCREMENT counter is seeded
# from the largest value of the column, wherever its index keeps it: the
# first key of a DESC primary key (#456), every group of a composite key whose
# leading column is not the counter's (#460), a secondary index, and a table
# with no primary key.

import os

from test_backup import _StandaloneMariaDB, _find_bytecask_root


def test_desc_pk_autoinc_seeds_after_restart():
    root = _find_bytecask_root()
    server = _StandaloneMariaDB(
        root, os.path.join(root, ".mariadb_desc_autoinc_test"), port=3313)
    try:
        server.init_and_start()
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("CREATE DATABASE desc_ai")
            cur.execute(
                "CREATE TABLE desc_ai.t (id INT NOT NULL AUTO_INCREMENT, v INT,"
                " PRIMARY KEY (id DESC)) ENGINE=bytecaskdb")
            cur.execute("INSERT INTO desc_ai.t (v) VALUES (1), (2), (3)")
            cur.execute(
                "CREATE TABLE desc_ai.c (a INT NOT NULL,"
                " b INT NOT NULL AUTO_INCREMENT, PRIMARY KEY (a DESC, b))"
                " ENGINE=bytecaskdb")
            cur.execute("INSERT INTO desc_ai.c (a) VALUES (1), (1), (1)")
            # The largest b is in the smallest a's group: (2,1),(1,2),(1,3),(1,4).
            cur.execute(
                "CREATE TABLE desc_ai.g (a INT NOT NULL,"
                " b INT NOT NULL AUTO_INCREMENT, PRIMARY KEY (a, b))"
                " ENGINE=bytecaskdb")
            cur.execute("INSERT INTO desc_ai.g (a) VALUES (2), (1), (1), (1)")
            # Counter in a secondary index, under a primary key it is not in.
            cur.execute(
                "CREATE TABLE desc_ai.s (k INT NOT NULL PRIMARY KEY,"
                " n INT NOT NULL AUTO_INCREMENT, KEY (n)) ENGINE=bytecaskdb")
            cur.execute("INSERT INTO desc_ai.s (k) VALUES (30), (20), (10)")
            # No primary key at all.
            cur.execute(
                "CREATE TABLE desc_ai.n (n INT NOT NULL AUTO_INCREMENT, v INT,"
                " KEY (n)) ENGINE=bytecaskdb")
            cur.execute("INSERT INTO desc_ai.n (v) VALUES (1), (2), (3)")
        conn.close()

        server.restart()

        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("INSERT INTO desc_ai.t (v) VALUES (4)")
            cur.execute("SELECT id, v FROM desc_ai.t ORDER BY id")
            assert cur.fetchall() == ((1, 1), (2, 2), (3, 3), (4, 4))
            cur.execute("INSERT INTO desc_ai.c (a) VALUES (1)")
            cur.execute("SELECT a, b FROM desc_ai.c ORDER BY b")
            assert cur.fetchall() == ((1, 1), (1, 2), (1, 3), (1, 4))
            cur.execute("INSERT INTO desc_ai.g (a) VALUES (1)")
            cur.execute("SELECT MAX(b) FROM desc_ai.g")
            assert cur.fetchall() == ((5,),)
            cur.execute("INSERT INTO desc_ai.s (k) VALUES (40)")
            cur.execute("SELECT n FROM desc_ai.s WHERE k = 40")
            assert cur.fetchall() == ((4,),)
            cur.execute("INSERT INTO desc_ai.n (v) VALUES (4)")
            cur.execute("SELECT n, v FROM desc_ai.n ORDER BY n")
            assert cur.fetchall() == ((1, 1), (2, 2), (3, 3), (4, 4))
        conn.close()
    finally:
        server.cleanup()
