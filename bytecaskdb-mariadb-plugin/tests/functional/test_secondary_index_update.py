"""Diagnostic test to identify the stale secondary index root cause."""
import pytest


def test_update_int_index_stale(make_connection):
    """Single-row UPDATE on INT index — does old entry survive?"""
    conn = make_connection()
    cur = conn.cursor()

    cur.execute('DROP DATABASE IF EXISTS diag1')
    cur.execute('CREATE DATABASE diag1')
    cur.execute(
        'CREATE TABLE diag1.t (id INT PRIMARY KEY, v INT, INDEX idx_v(v)) ENGINE=bytecaskdb'
    )
    cur.execute('INSERT INTO diag1.t VALUES (1, 100)')
    cur.execute('INSERT INTO diag1.t VALUES (2, 200)')
    cur.execute('INSERT INTO diag1.t VALUES (3, 300)')

    # Verify index before update
    cur.execute('SELECT id FROM diag1.t WHERE v = 200')
    before = cur.fetchall()
    print(f"\nBefore UPDATE: v=200 -> {before}")
    assert before == ((2,),)

    # Single-row update
    cur.execute('UPDATE diag1.t SET v = 999 WHERE id = 2')

    # Check row data via full scan
    cur.execute('SELECT id, v FROM diag1.t ORDER BY id')
    rows = cur.fetchall()
    print(f"After UPDATE full scan: {rows}")

    # Check using FORCE INDEX to ensure index path is used
    cur.execute('SELECT id FROM diag1.t FORCE INDEX (idx_v) WHERE v = 200')
    stale_forced = cur.fetchall()
    print(f"After UPDATE FORCE INDEX: v=200 -> {stale_forced}")

    # Check without index (table scan + filter)
    cur.execute('SELECT id FROM diag1.t IGNORE INDEX (idx_v) WHERE v = 200')
    scan_result = cur.fetchall()
    print(f"After UPDATE IGNORE INDEX (table scan): v=200 -> {scan_result}")

    # Check old index entry (normal — optimizer may choose either path)
    cur.execute('SELECT id FROM diag1.t WHERE v = 200')
    stale = cur.fetchall()
    print(f"After UPDATE: v=200 -> {stale} (should be empty)")

    # EXPLAIN
    cur.execute('EXPLAIN SELECT id FROM diag1.t WHERE v = 200')
    explain = cur.fetchall()
    print(f"EXPLAIN: {explain}")

    # Check new index entry
    cur.execute('SELECT id FROM diag1.t WHERE v = 999')
    new_entry = cur.fetchall()
    print(f"After UPDATE: v=999 -> {new_entry}")

    # Run CHECK TABLE to see if it detects inconsistency
    cur.execute('CHECK TABLE diag1.t')
    check_result = cur.fetchall()
    print(f"CHECK TABLE: {check_result}")

    cur.execute('DROP DATABASE diag1')
    conn.close()

    assert stale == (), f"Stale entry found: v=200 returns {stale}"


def test_update_varchar_index_not_stale(make_connection):
    """VARCHAR UPDATE works — why does INT fail but VARCHAR pass?"""
    conn = make_connection()
    cur = conn.cursor()

    cur.execute('DROP DATABASE IF EXISTS diag2')
    cur.execute('CREATE DATABASE diag2')
    cur.execute(
        'CREATE TABLE diag2.t (id INT PRIMARY KEY, name VARCHAR(64), INDEX idx_name(name)) ENGINE=bytecaskdb'
    )
    cur.execute("INSERT INTO diag2.t VALUES (1, 'alice')")
    cur.execute("INSERT INTO diag2.t VALUES (2, 'bob')")
    cur.execute("INSERT INTO diag2.t VALUES (3, 'carol')")

    cur.execute("UPDATE diag2.t SET name = 'dave' WHERE id = 2")

    cur.execute("SELECT id FROM diag2.t WHERE name = 'bob'")
    stale = cur.fetchall()
    print(f"\nVARCHAR: name='bob' -> {stale} (should be empty)")

    cur.execute("SELECT id FROM diag2.t WHERE name = 'dave'")
    new_entry = cur.fetchall()
    print(f"VARCHAR: name='dave' -> {new_entry}")

    cur.execute('DROP DATABASE diag2')
    conn.close()

    assert stale == (), f"Stale entry found: name='bob' returns {stale}"
