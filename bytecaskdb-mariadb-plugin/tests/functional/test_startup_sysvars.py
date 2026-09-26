# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_startup_sysvars.py — a dynamic sysvar set in the option file or on the
# command line takes effect, not just SET GLOBAL. MariaDB assigns startup
# values before the plugin initialises, without running the update callbacks.

import pymysql
import pytest

from conftest import MariaDBServer

ROWS = 2000


def _counter(cur, name):
    cur.execute("SHOW ENGINE BYTECASKDB STATUS")
    # One row; its Status column holds a "name: value" line per counter.
    for line in cur.fetchone()[2].splitlines():
        key, _, value = line.partition(": ")
        if key == name:
            return int(value)
    raise AssertionError(f"{name} missing from SHOW ENGINE BYTECASKDB STATUS")


@pytest.fixture(scope="module")
def server_with_small_copy_batches():
    server = MariaDBServer(
        dir_name=".mariadb_functional_test_startup_sysvars",
        port=3310,
        extra_args=["--bytecaskdb-bulk-copy-flush-bytes=4096"],
    )
    server.start()
    yield server
    server.stop()


def test_startup_bulk_copy_flush_bytes_takes_effect(server_with_small_copy_batches):
    conn = pymysql.connect(
        unix_socket=server_with_small_copy_batches.socket_path,
        user="root", password="", autocommit=True,
    )
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT @@global.bytecaskdb_bulk_copy_flush_bytes")
            assert cur.fetchone() == (4096,)
            cur.execute("CREATE DATABASE ssv")
            cur.execute("USE ssv")  # seq_1_to_N needs a current database
            cur.execute(
                "CREATE TABLE ssv.t (id INT PRIMARY KEY, v VARCHAR(64) NOT NULL) "
                "ENGINE=bytecaskdb"
            )
            cur.execute(
                "INSERT INTO ssv.t SELECT seq, REPEAT('x', 40) FROM seq_1_to_%d" % ROWS
            )

            before = _counter(cur, "bytecask.group_writer_batches")
            cur.execute("ALTER TABLE ssv.t FORCE")
            batches = _counter(cur, "bytecask.group_writer_batches") - before

            # ~100 KB of rows in 4 KiB flushes. The 64 MiB default would copy
            # them in one batch.
            assert batches >= 10, batches
            cur.execute("SELECT COUNT(*) FROM ssv.t")
            assert cur.fetchone() == (ROWS,)
    finally:
        conn.close()
