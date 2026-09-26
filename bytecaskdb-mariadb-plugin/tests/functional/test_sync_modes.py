# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_sync_modes.py — bytecaskdb_sync decides when committed transactions
# reach disk: at every commit, on the background flusher's interval, or only
# at file rotation. Observed through the engine's fsync counter.

import time

import pymysql
import pytest

from conftest import MariaDBServer

N_COMMITS = 20


def _fsyncs(cur):
    cur.execute("SHOW ENGINE BYTECASKDB STATUS")
    # One row; its Status column holds a "name: value" line per counter.
    for line in cur.fetchone()[2].splitlines():
        name, _, value = line.partition(": ")
        if name == "bytecask.fsyncs":
            return int(value)
    raise AssertionError("bytecask.fsyncs missing from SHOW ENGINE BYTECASKDB STATUS")


def _commit_rows(cur, first_id):
    for i in range(first_id, first_id + N_COMMITS):
        cur.execute(f"INSERT INTO sm.t VALUES ({i}, 'v{i}')")
    return first_id + N_COMMITS


def _wait_for(pred, timeout=5.0):
    deadline = time.monotonic() + timeout
    while not pred():
        assert time.monotonic() < deadline, "condition not reached in time"
        time.sleep(0.02)


@pytest.fixture
def sm(make_connection):
    conn = make_connection()
    with conn.cursor() as cur:
        cur.execute("DROP DATABASE IF EXISTS sm")
        cur.execute("CREATE DATABASE sm")
        cur.execute(
            "CREATE TABLE sm.t (id INT PRIMARY KEY, v VARCHAR(16) NOT NULL) "
            "ENGINE=bytecaskdb"
        )
    yield conn
    with conn.cursor() as cur:
        cur.execute("SET GLOBAL bytecaskdb_sync = DEFAULT")
        cur.execute("SET GLOBAL bytecaskdb_sync_interval_ms = DEFAULT")
        cur.execute("DROP DATABASE IF EXISTS sm")
    conn.close()


def test_at_every_commit_syncs_each_commit(sm):
    with sm.cursor() as cur:
        before = _fsyncs(cur)
        _commit_rows(cur, 1)
        assert _fsyncs(cur) - before >= N_COMMITS


def test_at_interval_syncs_on_the_flusher_not_at_commit(sm):
    with sm.cursor() as cur:
        # A long interval first, so the flusher is asleep while we commit.
        cur.execute("SET GLOBAL bytecaskdb_sync_interval_ms = 60000")
        cur.execute("SET GLOBAL bytecaskdb_sync = AT_INTERVAL")
        time.sleep(0.2)  # let the tick the mode change triggered finish

        before = _fsyncs(cur)
        next_id = _commit_rows(cur, 1)
        assert _fsyncs(cur) - before == 0

        # Shortening the interval wakes the flusher: it syncs the commits
        # above without waiting out the 60 s pause.
        cur.execute("SET GLOBAL bytecaskdb_sync_interval_ms = 50")
        _wait_for(lambda: _fsyncs(cur) > before)

        # With nothing unsynced, its ticks cost no fdatasync.
        time.sleep(0.2)
        idle_before = _fsyncs(cur)
        time.sleep(0.5)  # ten ticks
        assert _fsyncs(cur) - idle_before == 0

        # And new commits are synced within an interval of being made.
        synced_before = _fsyncs(cur)
        _commit_rows(cur, next_id)
        _wait_for(lambda: _fsyncs(cur) > synced_before)


def test_at_file_rotation_never_syncs_on_a_timer(sm):
    with sm.cursor() as cur:
        cur.execute("SET GLOBAL bytecaskdb_sync_interval_ms = 50")
        cur.execute("SET GLOBAL bytecaskdb_sync = AT_FILE_ROTATION")
        time.sleep(0.2)

        before = _fsyncs(cur)
        next_id = _commit_rows(cur, 1)
        time.sleep(0.5)  # ten flusher intervals
        assert _fsyncs(cur) - before == 0

        # Switching back takes effect at the next commit.
        cur.execute("SET GLOBAL bytecaskdb_sync = AT_EVERY_COMMIT")
        before = _fsyncs(cur)
        _commit_rows(cur, next_id)
        assert _fsyncs(cur) - before >= N_COMMITS

        cur.execute("SELECT COUNT(*) FROM sm.t")
        assert cur.fetchone() == (2 * N_COMMITS,)


def test_switch_to_at_every_commit_syncs_earlier_commits_without_a_new_one(sm):
    # Commits made under AT_FILE_ROTATION must not wait for the next commit
    # to reach disk once the mode is back to AT_EVERY_COMMIT.
    with sm.cursor() as cur:
        cur.execute("SET GLOBAL bytecaskdb_sync_interval_ms = 50")
        cur.execute("SET GLOBAL bytecaskdb_sync = AT_FILE_ROTATION")
        time.sleep(0.2)
        before = _fsyncs(cur)
        _commit_rows(cur, 1)
        assert _fsyncs(cur) - before == 0

        cur.execute("SET GLOBAL bytecaskdb_sync = AT_EVERY_COMMIT")
        _wait_for(lambda: _fsyncs(cur) > before)


@pytest.fixture(scope="module")
def server_started_at_interval():
    # A mode set on the command line or in my.cnf, not by SET GLOBAL.
    server = MariaDBServer(
        dir_name=".mariadb_functional_test_sync_modes",
        port=3311,
        extra_args=["--bytecaskdb-sync=AT_INTERVAL",
                    "--bytecaskdb-sync-interval-ms=60000"],
    )
    server.start()
    yield server
    server.stop()


def test_mode_set_at_startup_takes_effect(server_started_at_interval):
    conn = pymysql.connect(
        unix_socket=server_started_at_interval.socket_path,
        user="root", password="", autocommit=True,
    )
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT @@global.bytecaskdb_sync, "
                        "@@global.bytecaskdb_sync_interval_ms")
            assert cur.fetchone() == ("AT_INTERVAL", 60000)
            cur.execute("CREATE DATABASE sm")
            cur.execute(
                "CREATE TABLE sm.t (id INT PRIMARY KEY, v VARCHAR(16) NOT NULL) "
                "ENGINE=bytecaskdb"
            )
            before = _fsyncs(cur)
            _commit_rows(cur, 1)
            assert _fsyncs(cur) - before == 0
    finally:
        conn.close()
