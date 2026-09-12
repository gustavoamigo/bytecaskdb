# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_concurrency.py — Concurrent-connection tests for ha_bytecaskdb.
#
# These tests cannot be expressed in YAML because they require multiple
# simultaneous connections with coordinated timing. Each test opens its
# own connections via make_connection() and uses threading.Barrier /
# threading.Event to synchronise threads at critical points.

import threading
import pytest
import pymysql


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _setup(make_connection, *sqls):
    conn = make_connection()
    with conn.cursor() as cur:
        for sql in sqls:
            cur.execute(sql)
    conn.close()


def _teardown(make_connection, *sqls):
    conn = make_connection()
    with conn.cursor() as cur:
        for sql in sqls:
            try:
                cur.execute(sql)
            except Exception:
                pass
    conn.close()


# ---------------------------------------------------------------------------
# Test 1 — Concurrent reads scale without error
# ---------------------------------------------------------------------------

def test_concurrent_reads(make_connection):
    """8 reader threads each scan 100 rows; all complete without error."""
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS conc_read",
        "CREATE DATABASE conc_read",
        "CREATE TABLE conc_read.t (id INT PRIMARY KEY, v INT) ENGINE=bytecaskdb",
        "INSERT INTO conc_read.t VALUES "
        + ", ".join(f"({i}, {i * 10})" for i in range(1, 101)),
    )

    errors = []

    def reader():
        try:
            conn = make_connection()
            with conn.cursor() as cur:
                cur.execute("SELECT id, v FROM conc_read.t ORDER BY id")
                rows = cur.fetchall()
                assert len(rows) == 100
                for i, (rid, v) in enumerate(rows, start=1):
                    assert rid == i and v == i * 10
            conn.close()
        except Exception as e:
            errors.append(e)

    threads = [threading.Thread(target=reader) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    _teardown(make_connection, "DROP DATABASE IF EXISTS conc_read")
    assert not errors, f"Reader errors: {errors}"


# ---------------------------------------------------------------------------
# Test 2 — Write-write conflict: exactly one of two concurrent updaters wins
# ---------------------------------------------------------------------------

def test_ww_conflict(make_connection):
    """
    Two threads both read the same row, then both try to update it.
    Exactly one should succeed; the other should get error 1213 (deadlock).
    """
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS conc_ww",
        "CREATE DATABASE conc_ww",
        "CREATE TABLE conc_ww.counter (id INT PRIMARY KEY, v INT) ENGINE=bytecaskdb",
        "INSERT INTO conc_ww.counter VALUES (1, 0)",
    )

    successes = []
    conflicts = []
    other_errors = []
    # Barrier ensures both threads have a snapshot before either commits.
    barrier = threading.Barrier(2)

    def updater():
        conn = make_connection()
        try:
            with conn.cursor() as cur:
                cur.execute("BEGIN")
                cur.execute("SELECT v FROM conc_ww.counter WHERE id = 1")
                cur.fetchall()
                barrier.wait()  # both hold a snapshot of v=0; now race to commit
                try:
                    cur.execute("UPDATE conc_ww.counter SET v = v + 1 WHERE id = 1")
                    cur.execute("COMMIT")
                    successes.append(True)
                except pymysql.err.OperationalError as e:
                    if e.args[0] in (1180, 1213):
                        conflicts.append(True)
                        cur.execute("ROLLBACK")
                    else:
                        other_errors.append(e)
                        cur.execute("ROLLBACK")
        finally:
            conn.close()

    t1 = threading.Thread(target=updater)
    t2 = threading.Thread(target=updater)
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    _teardown(make_connection, "DROP DATABASE IF EXISTS conc_ww")

    assert not other_errors, f"Unexpected errors: {other_errors}"
    assert len(successes) == 1, f"Expected 1 success, got {len(successes)}"
    assert len(conflicts) == 1, f"Expected 1 conflict (1213), got {len(conflicts)}"


# ---------------------------------------------------------------------------
# Test 3 — Read-while-writing: batch inserts are atomic
# ---------------------------------------------------------------------------

def test_read_while_writing(make_connection):
    """
    Writer inserts rows 10 at a time inside explicit transactions.
    Concurrent readers observe row counts that are always multiples of 10.
    """
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS conc_rw",
        "CREATE DATABASE conc_rw",
        "CREATE TABLE conc_rw.t (id INT PRIMARY KEY, v INT) ENGINE=bytecaskdb",
    )

    stop_event = threading.Event()
    bad_counts = []
    reader_errors = []

    def writer():
        conn = make_connection()
        with conn.cursor() as cur:
            for batch in range(10):
                cur.execute("BEGIN")
                for i in range(10):
                    row_id = batch * 10 + i + 1
                    cur.execute(f"INSERT INTO conc_rw.t VALUES ({row_id}, {row_id})")
                cur.execute("COMMIT")
        conn.close()
        stop_event.set()

    def reader():
        conn = make_connection()
        try:
            with conn.cursor() as cur:
                while not stop_event.is_set():
                    cur.execute("SELECT COUNT(*) FROM conc_rw.t")
                    (count,) = cur.fetchone()
                    if count % 10 != 0:
                        bad_counts.append(count)
        except Exception as e:
            reader_errors.append(e)
        finally:
            conn.close()

    readers = [threading.Thread(target=reader) for _ in range(4)]
    w = threading.Thread(target=writer)

    for r in readers:
        r.start()
    w.start()
    w.join()
    for r in readers:
        r.join()

    _teardown(make_connection, "DROP DATABASE IF EXISTS conc_rw")

    assert not reader_errors, f"Reader errors: {reader_errors}"
    assert not bad_counts, f"Saw non-multiple-of-10 counts: {bad_counts}"


# ---------------------------------------------------------------------------
# Test 4 — High-concurrency inserts: all rows land without collisions
# ---------------------------------------------------------------------------

def test_high_concurrency_inserts(make_connection):
    """16 threads each insert 100 rows with disjoint PKs; total == 1600."""
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS conc_ins",
        "CREATE DATABASE conc_ins",
        "CREATE TABLE conc_ins.t (id INT PRIMARY KEY, thread INT) ENGINE=bytecaskdb",
    )

    errors = []

    def inserter(thread_id):
        conn = make_connection()
        try:
            with conn.cursor() as cur:
                for i in range(100):
                    row_id = thread_id * 100 + i
                    cur.execute(
                        f"INSERT INTO conc_ins.t VALUES ({row_id}, {thread_id})"
                    )
        except Exception as e:
            errors.append(e)
        finally:
            conn.close()

    threads = [threading.Thread(target=inserter, args=(tid,)) for tid in range(16)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    if not errors:
        conn = make_connection()
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM conc_ins.t")
            (total,) = cur.fetchone()
        conn.close()
    else:
        total = None

    _teardown(make_connection, "DROP DATABASE IF EXISTS conc_ins")

    assert not errors, f"Insert errors: {errors}"
    assert total == 1600, f"Expected 1600 rows, got {total}"


# ---------------------------------------------------------------------------
# Test 5 — Autocommit read-modify-write must not lose updates
# ---------------------------------------------------------------------------

def test_autocommit_increment_no_lost_update(make_connection):
    """
    8 connections each run `UPDATE ... SET v = v + 1` 100 times in autocommit,
    retrying on 1213. Every increment must land: the statement's read and its
    write must be checked against the same snapshot, so a concurrent commit
    between the two surfaces as a conflict rather than being overwritten.
    """
    n_threads, n_iters = 8, 100
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS conc_rmw",
        "CREATE DATABASE conc_rmw",
        "CREATE TABLE conc_rmw.t (id INT PRIMARY KEY, v INT NOT NULL) ENGINE=bytecaskdb",
        "INSERT INTO conc_rmw.t VALUES (1, 0)",
    )

    errors = []
    barrier = threading.Barrier(n_threads)

    def incrementer():
        conn = make_connection()
        try:
            with conn.cursor() as cur:
                barrier.wait()
                for _ in range(n_iters):
                    while True:
                        try:
                            cur.execute("UPDATE conc_rmw.t SET v = v + 1 WHERE id = 1")
                            break
                        except pymysql.err.OperationalError as e:
                            if e.args[0] not in (1180, 1213):
                                raise
        except Exception as e:
            errors.append(e)
        finally:
            conn.close()

    threads = [threading.Thread(target=incrementer) for _ in range(n_threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    conn = make_connection()
    with conn.cursor() as cur:
        cur.execute("SELECT v FROM conc_rmw.t WHERE id = 1")
        (v,) = cur.fetchone()
    conn.close()

    _teardown(make_connection, "DROP DATABASE IF EXISTS conc_rmw")

    assert not errors, f"Unexpected errors: {errors}"
    assert v == n_threads * n_iters, f"Lost updates: expected {n_threads * n_iters}, got {v}"
