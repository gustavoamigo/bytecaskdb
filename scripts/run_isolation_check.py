#!/usr/bin/env python3
"""Generate list-append histories with isolation_history and check them with
Elle (elle-cli) and a direct commit-sequence cross-check.

See docs/isolation_checking_design.md.

Usage:
    python3 scripts/run_isolation_check.py --binary PATH [--elle-jar PATH]
        [--seed S] [--rounds N] [--txns N] [--threads N] [--out DIR]
        [--no-vacuum] [--no-degrade]

Each round runs the three configurations from one seed:

    guarded    must pass the cross-check and be valid under
               strict-serializable.
    unguarded  must pass the cross-check and be valid under
               snapshot-isolation. Under strict-serializable it must be
               invalid with only write skew (G2-item) reported: that shows the
               checker can see the anomaly the guards exist to prevent.
    blind      must fail: the cross-check or Elle has to find a lost or
               out-of-order append.

Without --elle-jar only the cross-check runs. Exit status is non-zero on any
failed expectation. Histories and Elle's output stay under --out.
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

CONFIGS = ("guarded", "unguarded", "blind")

# Anomalies write skew may show up as under strict-serializable. Anything
# else in the unguarded configuration is a bug in the implicit W-W check or
# in snapshot reads.
WRITE_SKEW = {"G2-item", "G2-item-process", "G2-item-realtime"}


class CheckFailed(Exception):
    pass


def cross_check(history: list[dict]) -> list[str]:
    """Checks the history against the commit order the engine reported.

    Returns a list of violations (empty when the history is clean). Relies on
    the generator's final read of every key after all clients finish, so the
    longest read of a key is its final value.

    Also checks real-time visibility: a read must see every append whose
    transaction completed :ok before the reading transaction was invoked.
    """
    ok_appends: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    failed: set[int] = set()
    invoked: set[int] = set()
    reads: dict[int, list[list[int]]] = defaultdict(list)
    # (invoke index, list read) per key, and (completion index, element) per
    # key for the real-time check.
    timed_reads: dict[int, list[tuple[int, list[int]]]] = defaultdict(list)
    acked: dict[int, list[tuple[int, int]]] = defaultdict(list)
    invoke_index: dict[int, int] = {}

    for op in history:
        appends = [m for m in op["value"] if m[0] == "append"]
        if op["type"] == "invoke":
            invoked.update(m[2] for m in appends)
            invoke_index[op["process"]] = op["index"]
        elif op["type"] == "ok":
            seq = op.get("sequence", 0)
            for pos, m in enumerate(appends):
                ok_appends[m[1]].append((seq, pos, m[2]))
                acked[m[1]].append((op["index"], m[2]))
            for m in op["value"]:
                if m[0] == "r":
                    reads[m[1]].append(m[2])
                    timed_reads[m[1]].append(
                        (invoke_index[op["process"]], m[2]))
        elif op["type"] == "fail":
            failed.update(m[2] for m in appends)

    problems: list[str] = []
    for key in sorted(set(reads) | set(ok_appends)):
        final = max(reads[key], key=len, default=[])
        where = {e: i for i, e in enumerate(final)}
        if len(where) != len(final):
            problems.append(f"key {key}: duplicate element in {final}")
        for r in reads[key]:
            if final[: len(r)] != r:
                problems.append(
                    f"key {key}: read {r} is not a prefix of final {final}")
                break
        for e in final:
            if e in failed:
                problems.append(f"key {key}: element {e} of a failed txn read")
            elif e not in invoked:
                problems.append(f"key {key}: element {e} was never appended")
        committed = sorted(ok_appends[key])
        missing = [e for _, _, e in committed if e not in where]
        if missing:
            problems.append(
                f"key {key}: committed appends {missing[:8]} missing from "
                f"final read ({len(missing)} total)")
        present = [e for _, _, e in committed if e in where]
        positions = [where[e] for e in present]
        if positions != sorted(positions):
            problems.append(
                f"key {key}: final read {final} does not follow commit "
                f"sequence order of {present}")
        for started, r in timed_reads[key]:
            seen = set(r)
            stale = [e for done, e in acked[key] if done < started and e not in seen]
            if stale:
                problems.append(
                    f"key {key}: read {r} started after appends {stale[:8]} "
                    f"were acknowledged but does not see them")
                break
    return problems


def run_elle(jar: Path, history: Path, models: str, out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["java", "-jar", str(jar), "-m", "list-append", "-c", models,
           "-v", "json", "-d", str(out_dir), str(history)]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    (out_dir / "elle.stdout").write_text(proc.stdout)
    (out_dir / "elle.stderr").write_text(proc.stderr)
    # elle-cli pretty-prints the analysis as the last JSON object on stdout.
    if proc.stdout.startswith("{"):
        start = 0
    else:
        start = proc.stdout.find("\n{")
        start = start + 1 if start >= 0 else -1
    if start < 0 or proc.returncode not in (0, 1):
        for name, text in (("stdout", proc.stdout), ("stderr", proc.stderr)):
            tail = text.strip().splitlines()[-20:]
            if tail:
                print(f"  elle-cli {name} (last {len(tail)} lines):")
                for line in tail:
                    print(f"    {line}")
        raise CheckFailed(
            f"elle-cli failed (exit {proc.returncode}) on {history}; "
            f"see {out_dir}")
    try:
        return json.loads(proc.stdout[start:])
    except json.JSONDecodeError as e:
        raise CheckFailed(f"cannot parse elle-cli output in {out_dir}: {e}")


def describe(analysis: dict) -> str:
    return (f"valid?={analysis.get('valid?')} "
            f"anomalies={analysis.get('anomaly-types', [])} "
            f"not={analysis.get('not', [])}")


def check_round(args: argparse.Namespace, seed: int, round_dir: Path) -> None:
    for config in CONFIGS:
        history_path = round_dir / f"{config}.json"
        cmd = [str(args.binary), "--config", config, "--seed", str(seed),
               "--txns", str(args.txns), "--threads", str(args.threads),
               "--dir", str(round_dir / f"db-{config}"),
               "--out", str(history_path)]
        if args.no_vacuum:
            cmd.append("--no-vacuum")
        if args.no_degrade:
            cmd.append("--no-degrade")
        subprocess.run(cmd, check=True)
        shutil.rmtree(round_dir / f"db-{config}", ignore_errors=True)

        history = json.loads(history_path.read_text())
        problems = cross_check(history)
        verdict = "clean" if not problems else f"{len(problems)} violations"
        print(f"  {config}: cross-check {verdict}")
        for p in problems[: 3 if config == "blind" else 10]:
            print(f"    {p}")

        if config != "blind" and problems:
            raise CheckFailed(f"{config}: commit-sequence cross-check failed")

        elle_found = None
        if args.elle_jar:
            if config == "guarded":
                a = run_elle(args.elle_jar, history_path,
                             "strict-serializable", round_dir / "elle-guarded")
                print(f"  guarded: strict-serializable {describe(a)}")
                if a.get("valid?") is not True:
                    raise CheckFailed("guarded: not strict-serializable")
            elif config == "unguarded":
                a = run_elle(args.elle_jar, history_path,
                             "snapshot-isolation",
                             round_dir / "elle-unguarded-si")
                print(f"  unguarded: snapshot-isolation {describe(a)}")
                if a.get("valid?") is not True:
                    raise CheckFailed("unguarded: not snapshot isolation")
                a = run_elle(args.elle_jar, history_path,
                             "strict-serializable",
                             round_dir / "elle-unguarded-ser")
                print(f"  unguarded: strict-serializable {describe(a)}")
                found = set(a.get("anomaly-types", []))
                if found - WRITE_SKEW:
                    raise CheckFailed(
                        f"unguarded: anomalies beyond write skew: "
                        f"{sorted(found - WRITE_SKEW)}")
                args.write_skew_seen |= bool(found & WRITE_SKEW)
            else:
                a = run_elle(args.elle_jar, history_path,
                             "strict-serializable", round_dir / "elle-blind")
                print(f"  blind: strict-serializable {describe(a)}")
                elle_found = a.get("valid?") is False
        if config == "blind" and not problems and not elle_found:
            raise CheckFailed(
                "blind: neither check found an anomaly; the harness is not "
                "sensitive to lost updates")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--elle-jar", type=Path)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--txns", type=int, default=50_000)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--out", type=Path, default=Path("isolation_check"))
    parser.add_argument("--no-vacuum", action="store_true")
    parser.add_argument("--no-degrade", action="store_true")
    args = parser.parse_args()

    seed = args.seed if args.seed is not None else random.SystemRandom().getrandbits(63)
    print(f"run_isolation_check: seed={seed} rounds={args.rounds} "
          f"txns={args.txns} threads={args.threads} "
          f"elle={'yes' if args.elle_jar else 'no (cross-check only)'}")
    print(f"  rerun: {' '.join(sys.argv[:1])} --binary {args.binary} "
          f"--seed {seed} --rounds {args.rounds} --txns {args.txns} "
          f"--threads {args.threads}"
          + (f" --elle-jar {args.elle_jar}" if args.elle_jar else ""))
    sys.stdout.flush()

    shutil.rmtree(args.out, ignore_errors=True)
    args.write_skew_seen = False
    rng = random.Random(seed)
    try:
        for r in range(args.rounds):
            round_seed = rng.getrandbits(63)
            print(f"round {r + 1}/{args.rounds} seed={round_seed}")
            sys.stdout.flush()
            check_round(args, round_seed, args.out / f"round-{r}")
        if args.elle_jar and not args.write_skew_seen:
            raise CheckFailed(
                "unguarded: Elle reported no write skew in any round; the "
                "harness is not shown to be sensitive to it")
    except (CheckFailed, subprocess.CalledProcessError) as e:
        print(f"FAIL: {e}\n  histories and Elle output kept in {args.out}")
        return 1
    print(f"PASS {args.rounds} rounds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
