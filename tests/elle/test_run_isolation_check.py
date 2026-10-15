# SPDX-License-Identifier: MIT
"""Unit tests for the lost-write accounting in run_isolation_check.py.

Run: python3 -m unittest tests/elle/test_run_isolation_check.py -v
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import run_isolation_check as ric  # noqa: E402


def topology_history(tailed: bool) -> tuple[list[dict], dict]:
    """Node 0 leads from sequence 100 (epoch 1) and acknowledges an append
    at 120. An unplanned promotion then makes node 1 leader at its
    durable_sequence() 150. When node 1 never tailed node 0's term, its 150
    is on node 2's older branch, so the append is lost and the final read
    on node 1 does not show it."""
    history = [
        {"index": 0, "process": 0, "type": "invoke", "node": 0, "epoch": 1,
         "life": 1, "role": "leader", "value": [["append", 1, 10]]},
        {"index": 1, "process": 0, "type": "ok", "node": 0, "epoch": 1,
         "life": 1, "role": "leader", "sequence": 120,
         "value": [["append", 1, 10]]},
        {"index": 2, "process": 9, "type": "invoke", "node": 1, "epoch": 2,
         "life": 1, "role": "final", "value": [["r", 1, None]]},
        {"index": 3, "process": 9, "type": "ok", "node": 1, "epoch": 2,
         "life": 1, "role": "final", "value": [["r", 1, []]]},
    ]
    summary = {
        "final_leader": 1,
        "events": [
            {"kind": "unplanned", "epoch": 1, "from": 2, "to": 0,
             "durable": 100, "other_durable": 0, "life": 0, "tailed": True},
            {"kind": "unplanned", "epoch": 2, "from": 0, "to": 1,
             "durable": 150, "other_durable": 0, "life": 0, "tailed": tailed},
        ],
    }
    return history, summary


def leader_problems(history: list[dict], summary: dict) -> list[str]:
    promotions = ric.lost_by_promotion(history, summary)
    ric.relabel_lost_writes(history, promotions)
    ric.relabel_forked_reads(history, summary, promotions)
    return ric.cross_check(ric.leader_history(history, summary["final_leader"]))


class LostByPromotionTest(unittest.TestCase):
    def test_target_that_never_tailed_the_term_loses_all_of_it(self):
        history, summary = topology_history(tailed=False)
        self.assertEqual(leader_problems(history, summary), [])
        self.assertEqual(history[1]["type"], "info")

    def test_target_that_tailed_the_term_keeps_up_to_its_durable(self):
        history, summary = topology_history(tailed=True)
        problems = leader_problems(history, summary)
        self.assertTrue(any("missing from final read" in p for p in problems),
                        problems)


if __name__ == "__main__":
    unittest.main()
