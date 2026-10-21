# SPDX-License-Identifier: MIT
"""Unit tests for the lost-write accounting in run_isolation_check.py.

Run: python3 -m unittest tests/elle/test_run_isolation_check.py -v
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import run_isolation_check as ric  # noqa: E402


def topology_history(held: int) -> tuple[list[dict], dict]:
    """Node 0 leads from sequence 100 (epoch 1) and acknowledges an append
    at 120. An unplanned promotion then makes node 1 leader at its
    durable_sequence() 150. When node 1 holds epoch 0's history rather than
    node 0's term, its 150 is on node 2's older branch, so the append is
    lost and the final read on node 1 does not show it."""
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
             "durable": 100, "other_durable": 0, "life": 0, "held": 0},
            {"kind": "unplanned", "epoch": 2, "from": 0, "to": 1,
             "durable": 150, "other_durable": 0, "life": 0, "held": held},
        ],
    }
    return history, summary


def op(index, process, node, epoch, role, value, sequence=None, life=1):
    """An invoke and its :ok completion."""
    o = {"index": index, "process": process, "type": "ok", "node": node,
         "epoch": epoch, "life": life, "role": role, "value": value}
    if sequence is not None:
        o["sequence"] = sequence
    inv = {**o, "type": "invoke", "index": index - 1,
           "value": [[m[0], m[1], None] if m[0] == "r" else m for m in value]}
    inv.pop("sequence", None)
    return [inv, o]


CHAIN_EVENTS = [
    {"kind": "unplanned", "epoch": 1, "from": 0, "to": 1, "durable": 100,
     "held": 0},
    {"kind": "unplanned", "epoch": 2, "from": 1, "to": 2, "durable": 150,
     "held": 0},
    {"kind": "unplanned", "epoch": 3, "from": 2, "to": 0, "durable": 200,
     "held": 1},
]


def leader_problems(history: list[dict], summary: dict) -> list[str]:
    promotions = ric.lost_by_promotion(history, summary)
    ric.relabel_lost_writes(history, promotions)
    ric.relabel_forked_reads(history, summary, promotions)
    return ric.cross_check(ric.leader_history(history, summary["final_leader"]))


class LostByPromotionTest(unittest.TestCase):
    def test_target_that_never_tailed_the_term_loses_all_of_it(self):
        history, summary = topology_history(held=0)
        self.assertEqual(leader_problems(history, summary), [])
        self.assertEqual(history[1]["type"], "info")

    def test_target_that_tailed_the_term_keeps_up_to_its_durable(self):
        history, summary = topology_history(held=1)
        problems = leader_problems(history, summary)
        self.assertTrue(any("missing from final read" in p for p in problems),
                        problems)

    def test_inherited_branch_lost_with_the_term_that_led_from_it(self):
        # Epoch 0: node 0 (A) appends 10 at 120. Epoch 1: node 1 (B),
        # promoted at 100, appends 20 at 180; A is re-bootstrapped from B.
        # Epoch 2: node 2 (C), left ahead on A's branch at 150, is promoted
        # (held 0) and reads [10]. Epoch 3: A is promoted holding B's
        # history (held 1); its final read is [20]. C's read of A's branch
        # must count as lost with C's term.
        history = (op(1, 0, 0, 0, "leader", [["append", 1, 10]], 120)
                   + op(3, 1, 1, 1, "leader", [["append", 1, 20]], 180)
                   + op(5, 2, 2, 2, "leader", [["r", 1, [10]]])
                   + op(7, 9, 0, 3, "final", [["r", 1, [20]]]))
        summary = {"final_leader": 0, "events": CHAIN_EVENTS}
        promotions = ric.lost_by_promotion(history, summary)
        self.assertEqual([lost for _, lost in promotions],
                         [{10}, {20}, {10}])
        self.assertEqual(leader_problems(history, summary), [])
        self.assertEqual(history[5]["type"], "info")

    def test_stale_leader_follower_reads_lost_in_its_life(self):
        # The chain above, with a second append (11 at 130) on A's branch.
        # C reads [10, 11] as a follower in epoch 1 before its term, in the
        # life it leads in, and is re-bootstrapped after epoch 3 (life 2)
        # with [20]. Its life-1 reads count as lost; the life-2 read stays.
        history = (op(1, 0, 0, 0, "leader", [["append", 1, 10]], 120)
                   + op(3, 0, 0, 0, "leader", [["append", 1, 11]], 130)
                   + op(5, 1, 1, 1, "leader", [["append", 1, 20]], 180)
                   + op(7, 2, 2, 1, "reader", [["r", 1, [10, 11]]])
                   + op(9, 3, 2, 2, "leader", [["r", 1, [10, 11]]])
                   + op(11, 9, 0, 3, "final", [["r", 1, [20]]])
                   + op(13, 8, 2, 3, "final", [["r", 1, [20]]], life=2))
        summary = {"final_leader": 0, "events": CHAIN_EVENTS}
        self.assertEqual(leader_problems(history, summary), [])
        self.assertEqual(ric.replication_checks(history, 0), [])
        self.assertEqual(history[7]["type"], "info")
        self.assertEqual(history[13]["type"], "ok")



def sql_history(final: list[int]) -> list[dict]:
    """Two appends to key 1 committed in the order 20, then 10, as a history
    through SQL records them: with no commit sequence. Then a final read."""
    ops = []
    for i, e in enumerate((20, 10)):
        ops += [{"index": 2 * i, "process": i, "type": "invoke", "value": [["append", 1, e]]},
                {"index": 2 * i + 1, "process": i, "type": "ok", "value": [["append", 1, e]]}]
    ops += [{"index": 4, "process": 9, "type": "invoke", "value": [["r", 1, None]]},
            {"index": 5, "process": 9, "type": "ok", "value": [["r", 1, final]]}]
    return ops


class NoSequenceTest(unittest.TestCase):
    def test_commit_order_is_not_inferred_from_elements(self):
        self.assertEqual(ric.cross_check(sql_history([20, 10])), [])

    def test_a_missing_append_is_still_found(self):
        problems = ric.cross_check(sql_history([20]))
        self.assertTrue(any("missing from final read" in p for p in problems), problems)


if __name__ == "__main__":
    unittest.main()
