# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_desc_autoinc.py — the AUTO_INCREMENT counter is seeded from the largest
# stored id after a restart when the primary key is DESC (#456): its keys are
# stored largest first, so the seed reads the first key, not the last.

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
        conn.close()

        server.restart()

        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("INSERT INTO desc_ai.t (v) VALUES (4)")
            cur.execute("SELECT id, v FROM desc_ai.t ORDER BY id")
            assert cur.fetchall() == ((1, 1), (2, 2), (3, 3), (4, 4))
        conn.close()
    finally:
        server.cleanup()
