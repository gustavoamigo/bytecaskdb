# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# runner.py — YAML test case executor.
#
# YAML case format:
#
#   - name: Human-readable test name
#     setup:                     # list of SQL statements, run before test
#       - SQL statement
#     query: SELECT ...          # single query whose rows are compared to expected
#     expected:                  # list of rows, each row is a list of values
#       - [col1, col2]
#     teardown:                  # list of SQL statements, always run after test
#       - SQL statement
#
# For multi-step tests (e.g. BEGIN / INSERT / COMMIT / SELECT) use `steps`
# instead of `query`+`expected`:
#
#   steps:
#     - sql: BEGIN
#     - sql: INSERT INTO t VALUES (1, 100)
#     - sql: COMMIT
#     - sql: SELECT v FROM t WHERE id = 1
#       expected: [[100]]        # only steps with `expected` assert row output
#     - sql: INSERT INTO t VALUES (1, 100)
#       error: 1062              # expect this MySQL error code; step must fail

import pymysql


def _rows_to_str(rows):
    """Normalize fetchall() output to list-of-list-of-str for comparison."""
    return [
        ["NULL" if v is None else str(v) for v in row]
        for row in rows
    ]


def _execute_step(cur, step, case_name):
    """Execute one step dict. Handles optional `expected` and `error` keys."""
    sql = step["sql"]
    if "error" in step:
        try:
            cur.execute(sql)
        except pymysql.err.MySQLError as e:
            got = e.args[0]
            want = step["error"]
            assert got == want, (
                f"[{case_name}] step '{sql}' expected error {want}, got {got}"
            )
            return
        raise AssertionError(
            f"[{case_name}] step '{sql}' expected error {step['error']} but succeeded"
        )

    cur.execute(sql)
    if "expected" in step:
        actual = _rows_to_str(cur.fetchall())
        expected = [["NULL" if v is None else str(v) for v in row] for row in step["expected"]]
        assert actual == expected, (
            f"[{case_name}] step '{sql}' row mismatch\n"
            f"  expected: {expected}\n"
            f"  actual:   {actual}"
        )


def run_case(case, make_connection):
    conn = make_connection()
    try:
        with conn.cursor() as cur:
            for sql in case.get("setup", []):
                cur.execute(sql)

        with conn.cursor() as cur:
            try:
                if "query" in case:
                    cur.execute(case["query"])
                    actual = _rows_to_str(cur.fetchall())
                    expected = [
                        ["NULL" if v is None else str(v) for v in row] for row in case.get("expected", [])
                    ]
                    assert actual == expected, (
                        f"[{case['name']}] row mismatch\n"
                        f"  expected: {expected}\n"
                        f"  actual:   {actual}"
                    )
                elif "steps" in case:
                    for step in case["steps"]:
                        _execute_step(cur, step, case["name"])
            finally:
                with conn.cursor() as cleanup:
                    for sql in case.get("teardown", []):
                        try:
                            cleanup.execute(sql)
                        except Exception:
                            pass
    finally:
        conn.close()

