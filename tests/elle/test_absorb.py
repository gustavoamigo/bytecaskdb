#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""isolation_history --absorb on hand-written chaos lives.

Run: python3 -m unittest tests/elle/test_absorb.py -v
The binary is $ISOLATION_HISTORY, else the first one built under build/.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent


def find_binary() -> Path | None:
    if os.environ.get("ISOLATION_HISTORY"):
        return Path(os.environ["ISOLATION_HISTORY"])
    for mode in ("release", "releasedbg", "debug"):
        p = REPO / "build" / "linux" / "x86_64" / mode / "isolation_history"
        if p.exists():
            return p
    return None


# Life 0: process 0 appends 5 to key 0 with sync = false, acknowledged and not
# known durable when the life is killed; process 1 reads it. Life 1 opens, and
# its process 2 reads key 0: [5] if element 5 survived.
LIFE0 = """O
I 0 100 0 1 A 0 5
C 0 101 ok 1 0 1 A 0 5
I 1 102 0 1 N 0
C 1 103 ok 0 0 1 R 0 1 5
"""
LIFE1 = """O
I 2 400 0 1 N 0
C 2 401 ok 0 0 1 R 0 1 5
"""


class AbsorbTest(unittest.TestCase):
    def setUp(self):
        self.binary = find_binary()
        if self.binary is None:
            self.skipTest("isolation_history is not built")
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def absorb(self, life1_flags: str) -> list[dict]:
        """life1_flags: <cache_lost> <lost_before_open> <opened> <closed_ok>."""
        (self.root / "epoch0.txt").write_text(LIFE0)
        (self.root / "epoch1.txt").write_text(LIFE1)
        (self.root / "manifest.txt").write_text(
            "start 0\n"
            f"epoch 0 200 0 0 1 0 {self.root / 'epoch0.txt'}\n"
            f"epoch 1 300 {life1_flags} {self.root / 'epoch1.txt'}\n")
        out = self.root / "history.json"
        subprocess.run(
            [str(self.binary), "--absorb", str(self.root / "manifest.txt"),
             "--config", "unguarded", "--seed", "1", "--threads", "2",
             "--dir", str(self.root / "final_db"), "--out", str(out)],
            check=True, capture_output=True, text=True)
        return json.loads(out.read_text())

    def reader(self, history: list[dict], process: int = 1) -> dict:
        return next(op for op in history
                    if op["process"] == process and op["type"] != "invoke")

    def test_eviction_during_open_loses_the_last_lives_writes(self):
        # #437: the eviction came before the open made element 5 durable, so
        # the read of it is not one the database had to keep.
        op = self.reader(self.absorb("1 1 1 1"))
        self.assertEqual(op["type"], "info")
        self.assertEqual(op["error"], "read a write the page cache lost")
        self.assertEqual(op["value"], [["r", 0, None]])

    def test_eviction_during_open_keeps_the_life_s_own_reads(self):
        # Process 2 read after the open: what it saw, the open recovered and
        # made durable.
        op = self.reader(self.absorb("1 1 1 1"), process=2)
        self.assertEqual(op["type"], "ok")
        self.assertEqual(op["value"], [["r", 0, [5]]])

    def test_eviction_after_open_keeps_the_read(self):
        # The open made element 5 durable; a later eviction cannot take it.
        op = self.reader(self.absorb("1 0 1 1"))
        self.assertEqual(op["type"], "ok")
        self.assertEqual(op["value"], [["r", 0, [5]]])

    def test_failed_open_carries_the_writes_to_the_life_s_loss(self):
        # No open made element 5 durable, so the life's eviction takes it
        # whenever it came.
        op = self.reader(self.absorb("1 0 0 0"))
        self.assertEqual(op["type"], "info")


if __name__ == "__main__":
    unittest.main()
