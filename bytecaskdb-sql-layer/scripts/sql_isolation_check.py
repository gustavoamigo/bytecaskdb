#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Elle through SQL (docs/testing_design.md, *The MVP gate harness*).

Records list-append histories through the SQL layer with `sql_layer_tpcc
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

Usage:
    python3 bytecaskdb-sql-layer/scripts/sql_isolation_check.py \\
        --binary build/linux/x86_64/releasedbg/sql_layer_tpcc [--elle-jar elle-cli.jar] \\
        [--rounds N] [--txns N] [--threads N] [--seed S] [--out DIR]

Without --elle-jar only the cross-check runs.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import run_isolation_check as ric  # noqa: E402


def lost_appends(problems: list[str]) -> list[str]:
    """The cross-check's findings that show a lost append: missing from the
    final read, or a read the final value does not extend."""
    return [p for p in problems if "missing from final read" in p or "is not a prefix" in p]


def check_round(args: argparse.Namespace, seed: int, round_dir: Path) -> bool:
    """Returns whether the unguarded history showed write skew."""
    skew = False
    for config in ric.CONFIGS:
        history_path = round_dir / f"{config}.json"
        db = round_dir / f"db-{config}"
        subprocess.run([str(args.binary), "elle", "--dir", str(db), "--config", config, "--threads",
                        str(args.threads), "--txns", str(args.txns), "--seed", str(seed), "--out",
                        str(history_path)], check=True)
        shutil.rmtree(db, ignore_errors=True)
        problems = ric.cross_check(json.loads(history_path.read_text()))
        print(f"  {config}: cross-check {'clean' if not problems else f'{len(problems)} violations'}")
        for p in problems[:3 if config == "blind" else 10]:
            print(f"    {p}")
        if config != "blind" and problems:
            raise ric.CheckFailed(f"{config}: cross-check failed")
        elle_found = None
        if args.elle_jar:
            if config == "guarded":
                a = ric.run_elle(args.elle_jar, history_path, "strict-serializable", round_dir / "elle-guarded")
                print(f"  guarded: strict-serializable {ric.describe(a)}")
                if a.get("valid?") is not True:
                    raise ric.CheckFailed("guarded: not strict-serializable")
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
    args = parser.parse_args()
    seed = args.seed if args.seed is not None else random.randrange(1 << 31)
    print(f"seed {seed}")
    skew = False
    try:
        for r in range(args.rounds):
            round_dir = args.out / f"round-{r}"
            shutil.rmtree(round_dir, ignore_errors=True)
            round_dir.mkdir(parents=True)
            print(f"round {r} (seed {seed + r})")
            skew |= check_round(args, seed + r, round_dir)
        if args.elle_jar and not skew:
            raise ric.CheckFailed("unguarded: no write skew over the run; the check cannot see what the guards prevent")
    except ric.CheckFailed as e:
        print(f"FAILED: {e}")
        return 1
    print("passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
