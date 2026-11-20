#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Elle through SQL (docs/testing_design.md, *The TPROC-C harness*).

Records list-append histories through the SQL layer with `sql_layer_tprocc
elle`, and checks them with the engine's tooling: run_elle and cross_check
from scripts/run_isolation_check.py, the same rules as the engine's check
(docs/isolation_checking_design.md) but the commit-order one, which needs a
commit sequence a SQL commit does not report.

Each round runs the three configurations from one seed:

    guarded    reads of keys not appended take FOR UPDATE. Must pass the
               cross-check and be valid under strict-serializable.
    unguarded  plain reads. Must pass the cross-check and be valid under
               snapshot-isolation; under strict-serializable it may report
               only write skew (G2-item), and must at least once over the run.
    blind      every statement in autocommit. Must fail: the cross-check or
               Elle has to find a lost append.
    serializable
               every transaction at Isolation::Serializable, plain reads, no
               FOR UPDATE (docs/transaction_design.md, *Serializable
               isolation*). Must pass the cross-check and be valid under
               strict-serializable.
    serializable-ro
               the same with a third of the transactions read-only, begun
               READ ONLY: they record nothing and are placed at their
               snapshot (*Read-only serializable transactions*). Must be
               valid under strict-serializable.

The range workload (`sql_layer_tprocc elle --workload range`) runs scans and
deletes over fixed ranges of keys, which Elle has no model for, and is
checked here by range_check, which needs no commit order. Its invariant is
that a range that held a key keeps one: guarded, serializable and
serializable-ro must keep it every round,
unguarded and blind must each break it at least once over the run. In all
three every read must agree with the writes that committed around it, and
the final read with what committed.

Usage:
    python3 bytecaskdb-sql-layer/scripts/sql_isolation_check.py \\
        --binary build/linux/x86_64/releasedbg/sql_layer_tprocc [--elle-jar elle-cli.jar] \\
        [--workloads point,range] [--rounds N] [--txns N] [--threads N] [--seed S] [--out DIR]

Without --elle-jar the point workload runs only the cross-check.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import run_isolation_check as ric  # noqa: E402


def lost_appends(problems: list[str]) -> list[str]:
    """The cross-check's findings that show a lost append: missing from the
    final read, or a read the final value does not extend."""
    return [p for p in problems if "missing from final read" in p or "is not a prefix" in p]


CONFIGS = ric.CONFIGS + ("serializable", "serializable-ro")
# The configurations that must be strict-serializable and keep the range
# invariant.
SERIAL = ("guarded", "serializable", "serializable-ro")


def record(args: argparse.Namespace, workload: str, config: str, seed: int, round_dir: Path) -> Path:
    """Runs sql_layer_tprocc elle once and returns the history's path."""
    history_path = round_dir / f"{workload}-{config}.json"
    db = round_dir / f"db-{workload}-{config}"
    flags = ["--config", "serializable", "--read-only-third", "yes"] if config == "serializable-ro" else ["--config", config]
    subprocess.run([str(args.binary), "elle", "--dir", str(db), *flags, "--workload", workload,
                    "--threads", str(args.threads), "--txns", str(args.txns), "--seed", str(seed), "--out",
                    str(history_path)], check=True)
    shutil.rmtree(db, ignore_errors=True)
    return history_path


def range_check(doc: dict) -> tuple[list[str], list[str]]:
    """Checks a range history with no commit order.

    Every key is born once, initial or inserted fresh, and dies at most once,
    so each read can be held to the writes that completed before its
    transaction began and to those it could not yet have seen. Returns
    (violations, broken): violations are reads or a final state no
    interleaving of the committed writes explains, a failure in every
    configuration; broken are the reads and final ranges where a range that
    held a key is empty, the invariant only serializable histories keep.
    """
    buckets = doc["buckets"]
    txns = doc["txns"]
    blind = doc["config"] == "blind"
    initial = {k for b in buckets for k in b["initial"]}
    # Per key: the insert that could have made it (applied or unknown), and
    # the deletes that did or may have removed it.
    inserts: dict[int, dict] = {}
    deletes: dict[int, list[dict]] = defaultdict(list)
    never: set[int] = set()  # keys whose only insert did not take effect
    for t in txns:
        if t["insert"] is not None:
            if t["insert_applied"] is False:
                never.add(t["insert"])
            else:
                inserts[t["insert"]] = t
        if t["delete"] is not None and t["delete_applied"] is not False:
            deletes[t["delete"]].append(t)

    def bucket_of(k: int) -> int | None:
        for i, b in enumerate(buckets):
            if b["lo"] <= k <= b["hi"]:
                return i
        return None

    def surely_live(k: int, after: int, before: int) -> bool:
        """Born by a write that completed before `after`, and no delete of it
        had begun by `before`."""
        born = k in initial or (k in inserts and inserts[k]["insert_applied"] is True and inserts[k]["end"] < after)
        return born and not any(d["start"] < before for d in deletes[k])

    violations: list[str] = []
    broken: list[str] = []
    for t in txns:
        # A transaction's scan is committed when it committed; a blind scan
        # is its own committed statement.
        if t["scan"] is None or not (t["type"] == "ok" or blind):
            continue
        # The scan reads a state taken between these two times: from the
        # transaction's start (its snapshot is taken at or after it), or the
        # blind statement's own call, to the scan's return.
        after = t["scan_start"] if blind else t["start"]
        before = t["scan_end"]
        b = t["bucket"]
        seen = set(t["scan"])
        for k in seen:
            if bucket_of(k) != b:
                violations.append(f"txn at {t['start']}: scan of range {b} returned {k}, outside it")
            elif k in never or (k not in initial and k not in inserts):
                violations.append(f"txn at {t['start']}: scan of range {b} returned {k}, never committed")
            elif any(d["end"] < after and d["delete_applied"] is True for d in deletes[k]):
                violations.append(f"txn at {t['start']}: scan of range {b} returned {k}, deleted before it began")
            elif k in inserts and inserts[k]["start"] > before:
                violations.append(f"txn at {t['start']}: scan of range {b} returned {k}, inserted after it ended")
        for k in sorted(initial | inserts.keys()):
            if bucket_of(k) == b and k not in seen and surely_live(k, after, before):
                violations.append(f"txn at {t['start']}: scan of range {b} missed {k}, committed before it began")
        if not seen - {t["insert"]}:
            broken.append(f"txn at {t['start']}: range {b} held no key")

    final = set(doc["final"])
    for k in sorted(initial | inserts.keys() | never | final):
        if k in never:
            if k in final:
                violations.append(f"final read: {k} is there, its insert did not take effect")
            continue
        born = k in initial or k in inserts
        if not born:
            violations.append(f"final read: {k} is there, never inserted")
            continue
        unknown = (k in inserts and inserts[k]["insert_applied"] is None) or any(
            d["delete_applied"] is None for d in deletes[k])
        if unknown:
            continue
        applied = sum(1 for d in deletes[k] if d["delete_applied"] is True)
        expected = 1 - applied
        if expected not in (0, 1):
            violations.append(f"key {k}: deleted {applied} times")
        elif (k in final) != (expected == 1):
            violations.append(f"final read: {k} is {'there' if k in final else 'missing'}, "
                              f"the committed writes leave it {'live' if expected else 'deleted'}")
    for i, b in enumerate(buckets):
        if not any(b["lo"] <= k <= b["hi"] for k in final):
            broken.append(f"final read: range {i} holds no key")
    return violations, broken


def check_range_round(args: argparse.Namespace, seed: int, round_dir: Path) -> set[str]:
    """Returns the configurations whose history broke the range invariant."""
    broke: set[str] = set()
    for config in CONFIGS:
        doc = json.loads(record(args, "range", config, seed, round_dir).read_text())
        violations, broken = range_check(doc)
        print(f"  range {config}: {len(doc['txns'])} txns, {len(violations)} violations, "
              f"{len(broken)} empty ranges seen")
        for v in violations[:10]:
            print(f"    {v}")
        for v in broken[:3]:
            print(f"    {v}")
        if violations:
            raise ric.CheckFailed(f"range {config}: reads or final state no committed writes explain")
        if config in SERIAL and broken:
            raise ric.CheckFailed(f"range {config}: a range that held a key lost it")
        if broken:
            broke.add(config)
    return broke


def check_round(args: argparse.Namespace, seed: int, round_dir: Path) -> bool:
    """Returns whether the unguarded history showed write skew."""
    skew = False
    for config in CONFIGS:
        history_path = record(args, "point", config, seed, round_dir)
        problems = ric.cross_check(json.loads(history_path.read_text()), require_sequence=False)
        print(f"  {config}: cross-check {'clean' if not problems else f'{len(problems)} violations'}")
        for p in problems[:3 if config == "blind" else 10]:
            print(f"    {p}")
        if config != "blind" and problems:
            raise ric.CheckFailed(f"{config}: cross-check failed")
        elle_found = None
        if args.elle_jar:
            if config in SERIAL:
                a = ric.run_elle(args.elle_jar, history_path, "strict-serializable", round_dir / f"elle-{config}")
                print(f"  {config}: strict-serializable {ric.describe(a)}")
                if a.get("valid?") is not True:
                    raise ric.CheckFailed(f"{config}: not strict-serializable")
            elif config == "unguarded":
                a = ric.run_elle(args.elle_jar, history_path, "snapshot-isolation", round_dir / "elle-unguarded-si")
                print(f"  unguarded: snapshot-isolation {ric.describe(a)}")
                if a.get("valid?") is not True:
                    raise ric.CheckFailed("unguarded: not snapshot isolation")
                a = ric.run_elle(args.elle_jar, history_path, "strict-serializable", round_dir / "elle-unguarded-ser")
                print(f"  unguarded: strict-serializable {ric.describe(a)}")
                found = set(a.get("anomaly-types", []))
                if found - ric.WRITE_SKEW:
                    raise ric.CheckFailed(f"unguarded: anomalies beyond write skew: {sorted(found - ric.WRITE_SKEW)}")
                skew |= bool(found & ric.WRITE_SKEW)
            elif not lost_appends(problems):
                # Only when the cross-check found nothing: a blind history is
                # large and invalid, and Elle's search and plots of it are
                # slow, so the cross-check's finding stands on its own.
                a = ric.run_elle(args.elle_jar, history_path, "strict-serializable", round_dir / "elle-blind")
                print(f"  blind: strict-serializable {ric.describe(a)}")
                elle_found = a.get("valid?") is False
        if config == "blind" and not lost_appends(problems) and not elle_found:
            raise ric.CheckFailed("blind: neither check found an anomaly; the check is not sensitive to lost updates")
    return skew


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--elle-jar", type=Path)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--txns", type=int, default=5_000)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--out", type=Path, default=Path("sql_isolation_check"))
    parser.add_argument("--workloads", default="point,range",
                        help="comma-separated: point (list-append, Elle), range (scans and deletes)")
    args = parser.parse_args()
    workloads = args.workloads.split(",")
    if not workloads or set(workloads) - {"point", "range"}:
        parser.error("--workloads takes point, range or both")
    seed = args.seed if args.seed is not None else random.randrange(1 << 31)
    print(f"seed {seed}")
    skew = False
    broke: set[str] = set()
    try:
        for r in range(args.rounds):
            round_dir = args.out / f"round-{r}"
            shutil.rmtree(round_dir, ignore_errors=True)
            round_dir.mkdir(parents=True)
            print(f"round {r} (seed {seed + r})")
            if "point" in workloads:
                skew |= check_round(args, seed + r, round_dir)
            if "range" in workloads:
                broke |= check_range_round(args, seed + r, round_dir)
        if "point" in workloads and args.elle_jar and not skew:
            raise ric.CheckFailed("unguarded: no write skew over the run; the check cannot see what the guards prevent")
        if "range" in workloads:
            for config in ("unguarded", "blind"):
                if config not in broke:
                    raise ric.CheckFailed(f"range {config}: no range lost its keys over the run; "
                                          "the check cannot see what the guards prevent")
    except ric.CheckFailed as e:
        print(f"FAILED: {e}")
        return 1
    print("passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
