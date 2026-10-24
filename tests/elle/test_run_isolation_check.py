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


def append_txn(index: int, process: int, kind: str, key: int, element: int,
               seq: int | None = None) -> list[dict]:
    """An append to key, seen from its invoke to its completion."""
    value = [["append", key, element]]
    done = {"index": index + 1, "process": process, "type": kind,
            "value": value}
    if seq is not None:
        done["sequence"] = seq
    return [{"index": index, "process": process, "type": "invoke",
             "value": value}, done]


def read_txn(index: int, process: int, key: int, lst: list) -> list[dict]:
    return [{"index": index, "process": process, "type": "invoke",
             "value": [["r", key, None]]},
            {"index": index + 1, "process": process, "type": "ok",
             "value": [["r", key, lst]]}]


class FailedElementKeyTest(unittest.TestCase):
    """An element id names a write on one key. A process counter restarts
    with each epoch, so the same id appears on another key in another life:
    that is a different write, not the failed one."""

    def test_the_same_id_on_another_key_throughout(self):
        history = (append_txn(0, 0, "fail", 1, 7)
                   + append_txn(2, 0, "ok", 2, 7, 10)
                   + read_txn(4, 1, 2, [7]))
        self.assertEqual(ric.cross_check(history), [])

    def test_a_failed_element_read_on_its_own_key_fails(self):
        history = (append_txn(0, 0, "fail", 1, 7)
                   + read_txn(2, 1, 1, [7]))
        problems = ric.cross_check(history)
        self.assertTrue(any("element 7 of a failed txn read" in p
                            for p in problems), problems)

    def test_an_element_never_appended_on_its_key_fails(self):
        history = (append_txn(0, 0, "ok", 2, 7, 10)
                   + read_txn(2, 1, 2, [7])
                   + read_txn(4, 1, 1, [7]))
        problems = ric.cross_check(history)
        self.assertTrue(any("key 1: element 7 was never appended" in p
                            for p in problems), problems)


class NoSequenceTest(unittest.TestCase):
    def test_a_missing_sequence_fails_by_default(self):
        problems = ric.cross_check(sql_history([20, 10]))
        self.assertTrue(any("carry no commit sequence" in p for p in problems), problems)

    def test_commit_order_is_not_inferred_from_elements_when_opted_out(self):
        self.assertEqual(ric.cross_check(sql_history([20, 10]), require_sequence=False), [])

    def test_a_missing_append_is_still_found_when_opted_out(self):
        problems = ric.cross_check(sql_history([20]), require_sequence=False)
        self.assertTrue(any("missing from final read" in p for p in problems), problems)


def txn(index: int, process: int, seq: int | None, puts: dict[int, int],
        reads: dict[int, list] | None = None, kind: str = "ok",
        deletes: list[int] | None = None) -> list[dict]:
    """A range workload transaction: a put per (key, write id), then a point
    read and a scan per start key in reads, the value being the
    (key, version) pairs it returned. `index` is the invoke's index; the
    completion follows it."""
    value = [["put", k, wid] for k, wid in puts.items()]
    for start, pairs in (reads or {}).items():
        value.append([["r", start, pairs], ["scan", start, start + 4, pairs]])
    value += [["del", k] for k in (deletes or [])]
    inv = {"index": index, "process": process, "type": "invoke", "epoch": 0,
           "value": [["put", k, wid] for k, wid in puts.items()]
           + [["del", k] for k in (deletes or [])]
           + [[["r", start, []], ["scan", start, start + 4, []]]
              for start in (reads or {})]}
    done = {"index": index + 1, "process": process, "type": kind, "epoch": 0,
            "value": value}
    if kind == "ok":
        done["sequence"] = seq
    return [inv, done]


def final_read(index: int, values: dict[int, str]) -> list[dict]:
    """The read every client stopped for, holding one key's value per entry.
    [["r", k, null], ["scan", k, k + 1, null]] going out, as the generator
    writes it, and the values coming back."""
    value = [[["r", k, None], ["scan", k, k + 1, None]] for k in values]
    done = [[["r", k, [[k, v]]], ["scan", k, k + 1, [[k, v]]]]
            for k, v in values.items()]
    return [{"index": index, "process": 9, "type": "invoke", "epoch": 0,
             "role": "final", "value": value},
            {"index": index + 1, "process": 9, "type": "ok", "epoch": 0,
             "role": "final", "value": done}]


def info(index: int, process: int, puts: dict[int, int],
         dels: list[list] | None = None) -> list[dict]:
    """A range workload transaction whose outcome is unknown (an exception:
    a failed fdatasync, a kill)."""
    value = [["put", k, wid] for k, wid in puts.items()] + (dels or [])
    return [{"index": index, "process": process, "type": "invoke", "epoch": 0,
             "value": value},
            {"index": index + 1, "process": process, "type": "info",
             "epoch": 0, "value": value}]


def version(session: int, n: int) -> str:
    """The version a value stores for write id (session, n)."""
    ident = (session << 32) | n
    return f"w{ident >> 32:x}.{ident & 0xffffffff:x}"


class HistoryCheckTest(unittest.TestCase):
    """history_check: the range workload's replay against the commit order
    (#302). Each test is a history that is right or wrong for one reason."""

    def check(self, history, config="guarded"):
        return ric.history_check(history, config)[0]

    def base(self) -> list[dict]:
        """Key 5 holds version (1, 0), committed at sequence 10."""
        return txn(0, 0, 10, {5: 1 << 32}, {5: [[5, "s5"]]})

    def test_a_consistent_history_passes(self):
        history = self.base() + txn(10, 1, 20, {6: 2 << 32},
                                    {5: [[5, version(1, 0)]]})
        self.assertEqual(self.check(history), [])

    def test_a_read_of_a_version_the_order_does_not_hold_fails(self):
        history = self.base() + txn(10, 1, 20, {6: 2 << 32},
                                    {5: [[5, version(3, 0)]]})
        problems = self.check(history)
        self.assertTrue(any("read key 5" in p for p in problems), problems)

    def test_a_stale_read_is_an_exposure_unguarded_and_a_violation_guarded(self):
        # Process 0 replaces key 5 at sequence 20; process 1 reads the version
        # from sequence 10: a read behind its own position.
        history = (self.base()
                   + txn(10, 0, 20, {5: 2 << 32}, {5: [[5, version(1, 0)]]})
                   + txn(20, 1, 30, {6: 3 << 32},
                         {5: [[5, version(1, 0)]]}))
        self.assertTrue(self.check(history, "guarded"))
        problems, summary = ric.history_check(history, "unguarded")
        self.assertEqual(problems, [])
        self.assertGreater(summary["exposures"], 0)

    def test_unguarded_needs_an_exposure_to_prove_anything(self):
        history = self.base() + txn(10, 1, 20, {6: 2 << 32},
                                    {6: [[6, version(2, 0)]]})
        problems = self.check(history, "unguarded")
        self.assertTrue(any("not shown to reach write skew" in p
                            for p in problems), problems)

    def test_a_scan_that_misses_a_live_key_fails(self):
        history = self.base() + txn(10, 1, 20, {6: 2 << 32},
                                    {5: [[6, version(2, 0)]]})
        problems = self.check(history)
        self.assertTrue(any("missed keys [5]" in p for p in problems),
                        problems)

    def test_a_scan_cannot_show_the_key_the_transaction_inserts(self):
        # The transaction puts key 5 and scans [5, 9) of the snapshot it made
        # before the put: its own insert is not there to show.
        history = self.base() + txn(10, 1, 20, {5: 2 << 32}, {5: []})
        self.assertEqual(self.check(history), [])

    def test_an_info_write_placed_by_a_later_read_stands_in_the_order(self):
        # The :info put of (9, 0) on key 5 was there before process 1 read it,
        # so it is placed just before that read, over the earlier write.
        history = (self.base() + info(10, 9, {5: 9 << 32})
                   + txn(20, 1, 20, {6: 1 << 32},
                         {5: [[5, version(9, 0)]]}))
        self.assertEqual(self.check(history), [])

    def test_a_key_an_unplaced_info_write_touched_is_not_judged(self):
        # No committed read named (9, 0): whether it committed is unknown, so
        # key 5 is unknown from the :info transaction on and a read of either
        # value is accepted.
        for read in (version(1, 0), version(9, 0)):
            history = (self.base() + info(10, 9, {5: 9 << 32})
                       + txn(20, 1, 20, {6: 1 << 32}, {5: [[5, read]]}))
            self.assertEqual(self.check(history), [])

    def test_an_unplaced_info_write_does_not_judge_the_read_after_it(self):
        # Only the :info's own value being read back places it (the test
        # above); until then a read of the key names a version no committed
        # transaction wrote, and is not judged either way.
        history = (self.base() + info(10, 9, {5: 9 << 32})
                   + txn(20, 1, 20, {5: 2 << 32})
                   + txn(30, 2, 30, {6: 3 << 32},
                         {5: [[5, version(4, 0)]]}))
        self.assertEqual(self.check(history), [])

    def test_a_committed_write_settles_a_key_an_info_touched(self):
        # Whether the :info at index 11 committed is unknown, but the write
        # at sequence 20 is a committed transaction of the order, so the key
        # has a known state from then on: a read of the :info's value is a
        # state the order has left.
        history = (self.base() + info(11, 9, {5: 9 << 32})
                   + txn(20, 1, 20, {6: 2 << 32}, {5: [[5, version(9, 0)]]},
                         deletes=[5])
                   + txn(30, 2, 30, {6: 3 << 32},
                         {5: [[5, version(9, 0)]]}))
        problems = self.check(history)
        self.assertTrue(any("read key 5" in p for p in problems), problems)

    def test_an_info_placed_by_one_key_leaves_its_other_keys_unknown(self):
        # The :info wrote keys 5 and 6; only key 5's value is read back. Key
        # 6 stays unknown, so a read of a version no transaction wrote there
        # is accepted rather than reported.
        history = (self.base() + info(11, 9, {5: 9 << 32, 6: 9 << 32})
                   + txn(20, 1, 20, {}, {5: [[5, version(9, 0)]]})
                   + txn(30, 2, 30, {7: 3 << 32},
                         {6: [[6, version(2, 0)]]}))
        self.assertEqual(self.check(history), [])

    def test_a_committed_read_of_the_seeded_state_is_judged(self):
        # Key 5 starts at s5 and the order deletes the seed at sequence 10:
        # a committed transaction reading s5 afterwards has left the order.
        history = (txn(0, 0, 10, {5: 1 << 32},
                       {5: [[5, version(1, 0)]]}, deletes=[5])
                   + txn(10, 1, 20, {7: 2 << 32}, {5: [[5, "s5"]]}))
        problems = self.check(history)
        self.assertTrue(any("read key 5" in p and "s5" in p
                            for p in problems), problems)

    def test_a_read_only_transaction_is_not_judged(self):
        # It carries no sequence, and a value it read can carry one above
        # its own completion time, so the history cannot place it.
        history = (self.base() + txn(10, 1, None, {}, {5: [[5, "s5"]]})
                   + txn(20, 2, 30, {7: 2 << 32}, {5: [[5, version(1, 0)]]}))
        self.assertEqual(self.check(history), [])

    def test_the_final_read_must_hold_the_end_of_the_order(self):
        # Process 0's put of (2, 0) is acknowledged and the order holds it,
        # but the final read shows the seeded value: a lost acknowledged
        # write, which no later read has to touch to be caught.
        history = (self.base() + txn(10, 1, 20, {5: 2 << 32})
                   + final_read(20, {5: "s5"}))
        problems = self.check(history)
        self.assertTrue(any("final read" in p for p in problems), problems)

    def test_the_final_read_matching_the_order_passes(self):
        history = (self.base()
                   + txn(10, 1, 20, {5: 2 << 32}, {5: [[5, version(1, 0)]]})
                   + final_read(20, {5: version(2, 0)}))
        self.assertEqual(self.check(history), [])

    def test_an_invoke_without_a_completion_is_ignored(self):
        history = self.base() + [{"index": 10, "process": 7, "type": "invoke",
                                  "epoch": 0, "value": [["put", 6, 7 << 32]]}]
        self.assertEqual(self.check(history), [])

    def test_the_lives_of_a_kill_share_process_ids(self):
        # A transaction killed at the end of epoch 0 has no completion; the
        # first completion of epoch 1 belongs to its own invoke.
        history = (self.base()
                   + [{"index": 10, "process": 1, "type": "invoke",
                       "epoch": 1, "value": [["put", 7, 2 << 32]]}]
                   + txn(20, 0, 20, {6: 1 << 32},
                         {5: [[5, version(1, 0)]]}))
        self.assertEqual(self.check(history), [])


if __name__ == "__main__":
    unittest.main()
