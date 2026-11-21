#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Unit tests of sql_isolation_check.range_check on hand-made range histories.

Run: python3 -m unittest bytecaskdb-sql-layer/scripts/test_sql_isolation_check.py -v
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sql_isolation_check as sic  # noqa: E402

# Two ranges: 0 holds keys 0 and 1, 100 holds 100 and 101.
BUCKETS = [{"lo": 0, "hi": 99, "initial": [0, 1]}, {"lo": 100, "hi": 199, "initial": [100, 101]}]


def txn(start: int, end: int, *, bucket: int = 0, type: str = "ok", insert: int | None = None,
        insert_applied: bool | None = False, scan: list[int] | None = None, delete: int | None = None,
        delete_applied: bool | None = False) -> dict:
    return {"process": 0, "type": type, "start": start, "end": end, "bucket": bucket, "insert": insert,
            "insert_applied": insert_applied if insert is not None else False, "scan": scan,
            "scan_start": start + 1, "scan_end": start + 2, "delete": delete,
            "delete_applied": delete_applied if delete is not None else False}


def history(txns: list[dict], final: list[int], config: str = "guarded") -> dict:
    return {"workload": "range", "config": config, "buckets": BUCKETS, "txns": txns, "final": final}


class RangeCheckTest(unittest.TestCase):
    def test_a_serial_history_is_clean(self):
        h = history([
            txn(10, 20, scan=[0, 1], delete=0, delete_applied=True),
            txn(30, 40, insert=5, insert_applied=True, scan=[1, 5], delete=1, delete_applied=True),
            txn(50, 60, scan=[5]),
        ], final=[5, 100, 101])
        self.assertEqual(sic.range_check(h), ([], []))

    def test_write_skew_empties_a_range_and_is_reported_as_broken(self):
        h = history([
            txn(10, 20, scan=[0, 1], delete=0, delete_applied=True),
            txn(11, 21, scan=[0, 1], delete=1, delete_applied=True),
            txn(30, 40, scan=[]),
        ], final=[100, 101], config="unguarded")
        violations, broken = sic.range_check(h)
        self.assertEqual(violations, [])
        self.assertEqual(len(broken), 2)  # the later scan and the final read

    def test_a_failed_transactions_insert_seen_is_a_violation(self):
        h = history([
            txn(10, 20, type="fail", insert=7, insert_applied=False, scan=[0, 1, 7]),
            txn(30, 40, scan=[0, 1, 7]),
        ], final=[0, 1, 100, 101])
        violations, _ = sic.range_check(h)
        self.assertIn("never committed", violations[0])

    def test_a_scan_missing_a_key_committed_before_it_is_a_violation(self):
        h = history([
            txn(10, 20, insert=7, insert_applied=True, scan=[0, 1, 7]),
            txn(30, 40, scan=[0, 1]),
        ], final=[0, 1, 7, 100, 101])
        violations, _ = sic.range_check(h)
        self.assertEqual(len(violations), 1)
        self.assertIn("missed 7", violations[0])

    def test_a_key_deleted_concurrently_may_be_missed(self):
        h = history([
            txn(10, 40, scan=[0, 1], delete=0, delete_applied=True),
            txn(20, 30, scan=[1]),
        ], final=[1, 100, 101])
        self.assertEqual(sic.range_check(h), ([], []))

    def test_a_read_of_a_key_deleted_before_it_began_is_a_violation(self):
        h = history([
            txn(10, 20, scan=[0, 1], delete=0, delete_applied=True),
            txn(30, 40, scan=[0, 1]),
        ], final=[1, 100, 101])
        violations, _ = sic.range_check(h)
        self.assertIn("deleted before it began", violations[0])

    def test_a_lost_delete_and_a_lost_insert_in_the_final_read_are_violations(self):
        h = history([
            txn(10, 20, scan=[0, 1], delete=0, delete_applied=True),
            txn(30, 40, bucket=1, insert=150, insert_applied=True, scan=[100, 101, 150]),
        ], final=[0, 1, 100, 101])
        violations, _ = sic.range_check(h)
        self.assertEqual(len(violations), 2)
        self.assertTrue(any("final read: 0 is there" in v for v in violations))
        self.assertTrue(any("final read: 150 is missing" in v for v in violations))

    def test_a_write_of_unknown_outcome_is_not_judged_in_the_final_read(self):
        h = history([
            txn(10, 20, type="info", scan=[0, 1], delete=0, delete_applied=None),
        ], final=[0, 1, 100, 101])
        self.assertEqual(sic.range_check(h), ([], []))

    def test_a_blind_scan_is_held_to_its_own_statement(self):
        # A delete that committed inside the scanner's window, before its
        # scan statement: a transaction's snapshot may predate it, a blind
        # statement's cannot.
        def run(config: str) -> list[str]:
            scanner = txn(10, 60, scan=[0, 1])
            scanner["scan_start"], scanner["scan_end"] = 50, 55
            deleter = txn(20, 30, scan=[0, 1], delete=0, delete_applied=True)
            return sic.range_check(history([scanner, deleter], final=[1, 100, 101], config=config))[0]
        self.assertEqual(run("guarded"), [])
        self.assertIn("deleted before it began", run("blind")[0])


if __name__ == "__main__":
    unittest.main()
