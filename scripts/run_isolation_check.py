#!/usr/bin/env python3
"""Generate list-append histories with isolation_history and check them with
Elle (elle-cli) and a direct commit-sequence cross-check.

See docs/isolation_checking_design.md, and for --cluster
docs/replication_checking_design.md.

Usage:
    python3 scripts/run_isolation_check.py --binary PATH [--elle-jar PATH]
        [--seed S] [--rounds N] [--txns N] [--threads N] [--out DIR]
        [--no-vacuum] [--no-degrade] [--cluster] [--topology]

Each round runs the three configurations from one seed:

    guarded    must pass the cross-check and be valid under
               strict-serializable.
    unguarded  must pass the cross-check and be valid under
               snapshot-isolation. Under strict-serializable it must be
               invalid with only write skew (G2-item) reported: that shows the
               checker can see the anomaly the guards exist to prevent.
    blind      must fail: the cross-check or Elle has to find a lost or
               out-of-order append.

With --cluster each round instead runs a leader with two followers
(bootstrapped from a manifest under load, tailed with lag, duplicate delivery
and restarts, and read from), in two configurations:

    cluster         leader vacuum off. The leader's operations must pass the
                    guarded checks above; the whole history must be
                    serializable (follower reads may be stale, not
                    inconsistent) and pass the replication checks: prefix,
                    session, monotonic reads, convergence and bootstrap.
    cluster-vacuum  leader vacuum on. Must pass everything cluster does.
                    A follower that falls behind what vacuum kept is refused
                    by changes_since (DbInvalidSequence) and re-bootstraps;
                    at least one round over the run has to show that, or
                    the configuration never reached the case (#168).

With --topology each round runs the same cluster while leadership moves:
planned transfers to a random node, unplanned promotions, re-targeting of
every follower to the new leader, and re-bootstrap of the abandoned one. The
old leader's writes that an unplanned promotion lost are relabelled :info.

    topology         an unplanned promotion takes the most advanced follower.
                     Must pass everything the cluster configuration does.
    topology-behind  an unplanned promotion takes the least advanced
                     follower. A follower ahead of the new leader keeps writes
                     the leader lost, and at least one round over the run has
                     to detect that fork. Once the forked node leads in turn, the fork
                     reaches the leader history, so any finding counts.

Without --elle-jar only the cross-check runs. Exit status is non-zero on any
failed expectation. Histories and Elle's output stay under --out.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

CONFIGS = ("guarded", "unguarded", "blind")
CLUSTER_CONFIGS = ("cluster", "cluster-vacuum")
TOPOLOGY_CONFIGS = ("topology", "topology-behind")

# Anomalies write skew may show up as under strict-serializable. Anything
# else in the unguarded configuration is a bug in the implicit W-W check or
# in snapshot reads.
WRITE_SKEW = {"G2-item", "G2-item-process", "G2-item-realtime"}


ELLE_LOG_LINE = re.compile(
    r"(?:TRACE|DEBUG|INFO|WARN|ERROR) \[\d{4}-\d\d-\d\d [^\]]*\] [^\n]*\n")


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
    # Elle's plotting threads log to stdout and can land mid-line in the
    # JSON ("INFO [...] elle.viz Skipping plot of N bytes").
    stdout = ELLE_LOG_LINE.sub("", proc.stdout)
    # elle-cli pretty-prints the analysis as the last JSON object on stdout.
    if stdout.startswith("{"):
        start = 0
    else:
        start = stdout.find("\n{")
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
        return json.loads(stdout[start:])
    except json.JSONDecodeError as e:
        raise CheckFailed(f"cannot parse elle-cli output in {out_dir}: {e}")


def describe(analysis: dict) -> str:
    return (f"valid?={analysis.get('valid?')} "
            f"anomalies={analysis.get('anomaly-types', [])} "
            f"not={analysis.get('not', [])}")


def relabel_lost_writes(history: list[dict], summary: dict) -> int:
    """Marks what an unplanned promotion lost as indeterminate.

    Replication is asynchronous: when a follower is promoted without the old
    leader, the old leader's writes above the promoted node's
    durable_sequence() in its last term are lost by design, and so is
    anything its clients read of them. Those :ok operations become :info
    with their reads cleared, which Elle and the cross-checks accept either
    way. A session read there that waited above that sequence waited on the
    lost branch, so its wait is capped at it. A lost write that shows up on
    another node is still caught: only operations on the old leader are
    relabelled. Returns how many.
    """
    changed = 0
    for ev in summary.get("events", []):
        if ev["kind"] != "unplanned":
            continue
        old, epoch, durable = ev["from"], ev["epoch"], ev["durable"]
        tenure = [op for op in history
                  if op.get("node") == old and op.get("epoch") == epoch - 1
                  and op["type"] == "ok"]
        lost_elements: set[int] = set()
        for op in tenure:
            if op.get("role") == "leader" and op.get("sequence", 0) > durable:
                lost_elements.update(
                    m[2] for m in op["value"] if m[0] == "append")
        for op in tenure:
            # A session read there waited on the lost branch: sequences above
            # durable were reassigned by the new leader. What it waited for
            # still holds up to durable.
            if op.get("wait", 0) > durable:
                op["wait"] = durable
            wrote_lost = any(m[0] == "append" and m[2] in lost_elements
                             for m in op["value"])
            read_lost = any(m[0] == "r" and set(m[2] or []) & lost_elements
                            for m in op["value"])
            if wrote_lost or read_lost:
                op["type"] = "info"
                op["value"] = [[m[0], m[1], None] if m[0] == "r" else m
                               for m in op["value"]]
                op.pop("sequence", None)
                changed += 1
    return changed


def leader_history(history: list[dict], final_leader: int) -> list[dict]:
    """The operations a leader served: client transactions, and the final
    read of the final leader."""
    return [op for op in history
            if op.get("role", "leader") == "leader"
            or (op.get("role") == "final" and op.get("node") == final_leader)]


def replication_checks(history: list[dict], final_leader: int) -> list[str]:
    """Checks every node's reads against the leaders' commit order.

    prefix       a read on a node that is not serving as leader is a prefix
                 of the leaders' history: if it shows an append committed at
                 sequence S, it shows every committed append at or below S
                 on the keys it read.
    session      a read made after durable_sequence(W) returned >= W shows
                 every committed append at or below W.
    monotonic    a read on a node shows no less of a key than any read on the
                 same node that completed before it was invoked.
    convergence  every node's final read of every key equals the final
                 leader's.
    """
    elem_seq: dict[int, int] = {}
    ok_by_key: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for op in history:
        if op["type"] == "ok" and op.get("role", "leader") == "leader":
            seq = op.get("sequence", 0)
            for m in op["value"]:
                if m[0] == "append":
                    elem_seq[m[2]] = seq
                    ok_by_key[m[1]].append((seq, m[2]))

    found: dict[str, list[str]] = defaultdict(list)
    longest: dict[tuple[int, int], int] = defaultdict(int)
    floors: dict[int, dict[int, int]] = {}
    final: dict[int, dict[int, list[int]]] = defaultdict(dict)
    for op in history:
        node = op.get("node", 0)
        role = op.get("role", "leader")
        reads = [m for m in op["value"] if m[0] == "r"]
        if op["type"] == "invoke":
            floors[op["process"]] = {m[1]: longest[(node, m[1])] for m in reads}
            continue
        floor = floors.pop(op["process"], {})
        if op["type"] != "ok":
            continue
        for m in reads:
            key, lst = m[1], m[2]
            if len(lst) < floor.get(key, 0):
                found["monotonic"].append(
                    f"node {node} key {key}: read {lst} after a completed read "
                    f"of length {floor[key]}")
            longest[(node, key)] = max(longest[(node, key)], len(lst))
            if role == "final":
                final[node][key] = lst
        if role == "leader" or not reads:
            continue
        cut = max((elem_seq[e] for m in reads for e in m[2] if e in elem_seq),
                  default=0)
        wait = op.get("wait", 0)
        for m in reads:
            have = set(m[2])
            gap = [e for s, e in ok_by_key[m[1]] if s <= cut and e not in have]
            if gap:
                found["prefix"].append(
                    f"node {node} key {m[1]}: read {m[2]} shows sequence {cut} "
                    f"elsewhere but misses committed appends {gap[:8]}")
            late = [e for s, e in ok_by_key[m[1]] if s <= wait and e not in have]
            if late:
                found["session"].append(
                    f"node {node} key {m[1]}: read {m[2]} after waiting for "
                    f"sequence {wait} misses committed appends {late[:8]}")
    for node in final:
        if node == final_leader:
            continue
        for key in sorted(set(final[final_leader]) | set(final[node])):
            if final[node].get(key, []) != final[final_leader].get(key, []):
                found["convergence"].append(
                    f"node {node} key {key}: final {final[node].get(key)} but "
                    f"leader (node {final_leader}) final "
                    f"{final[final_leader].get(key)}")

    problems = []
    for kind, items in sorted(found.items()):
        problems.append(f"{kind}: {len(items)} violations")
        problems.extend(f"  {i}" for i in items[:5])
    return problems


def cluster_summary_problems(summary: dict) -> list[str]:
    problems = [f"error: {e}" for e in summary["errors"]]
    for b in summary["bootstraps"]:
        if b["mismatches"] != 0:
            problems.append(
                f"bootstrap node {b['node']}: {b['mismatches']} of "
                f"{b['keys_compared']} keys differ from the manifest snapshot")
        if b["durable_after_open"] != b["through_sequence"]:
            problems.append(
                f"bootstrap node {b['node']}: durable_sequence() "
                f"{b['durable_after_open']} after open, manifest through "
                f"{b['through_sequence']}")
    if not summary["bootstraps"]:
        problems.append("no follower was bootstrapped")
    return problems


def describe_events(summary: dict) -> str:
    counts: dict[str, int] = defaultdict(int)
    forks = 0
    for ev in summary.get("events", []):
        counts[ev["kind"]] += 1
        if ev["kind"] == "retarget" and ev["durable"] > ev["other_durable"]:
            forks += 1
    parts = [f"{k}={v}" for k, v in sorted(counts.items())]
    if forks:
        parts.append(f"retargets-ahead-of-source={forks}")
    return " ".join(parts) or "none"


def check_cluster_round(args: argparse.Namespace, seed: int,
                        round_dir: Path) -> None:
    configs = TOPOLOGY_CONFIGS if args.topology else CLUSTER_CONFIGS
    for config in configs:
        history_path = round_dir / f"{config}.json"
        cmd = [str(args.binary), "--config", "guarded", "--seed", str(seed),
               "--txns", str(args.txns), "--threads", str(args.threads),
               "--followers", "2", "--lag",
               "--dir", str(round_dir / f"db-{config}"),
               "--out", str(history_path)]
        cmd.append("--force-vacuum" if config == "cluster-vacuum"
                   else "--no-vacuum")
        if config.startswith("topology"):
            cmd.append("--topology")
        if config == "topology-behind":
            cmd.append("--promote-least")
        if args.no_degrade:
            cmd.append("--no-degrade")
        subprocess.run(cmd, check=True)
        shutil.rmtree(round_dir / f"db-{config}", ignore_errors=True)

        history = json.loads(history_path.read_text())
        summary = json.loads(
            Path(str(history_path) + ".cluster.json").read_text())
        final_leader = summary.get("final_leader", 0)
        relabelled = relabel_lost_writes(history, summary)
        if relabelled:
            history_path.write_text(json.dumps(history))
        print(f"  {config}: events {describe_events(summary)}; "
              f"{relabelled} operations lost to unplanned promotions")
        setup = cluster_summary_problems(summary)
        if setup:
            for p in setup[:10]:
                print(f"    {p}")
            raise CheckFailed(f"{config}: bootstrap or replication errors")

        leader = leader_history(history, final_leader)
        leader_problems = cross_check(leader)
        print(f"  {config}: leader cross-check "
              f"{'clean' if not leader_problems else ''}")
        for p in leader_problems[:10]:
            print(f"    {p}")
        problems = replication_checks(history, final_leader)
        print(f"  {config}: replication checks "
              f"{'clean' if not problems else ''}")
        for p in problems[:12]:
            print(f"    {p}")
        elle_found = False
        leader_valid = True
        if args.elle_jar:
            leader_path = round_dir / f"{config}-leader.json"
            leader_path.write_text(json.dumps(leader))
            a = run_elle(args.elle_jar, leader_path, "strict-serializable",
                         round_dir / f"elle-{config}-leader")
            print(f"  {config}: leader strict-serializable {describe(a)}")
            leader_valid = a.get("valid?") is True
            a = run_elle(args.elle_jar, history_path, "serializable",
                         round_dir / f"elle-{config}-all")
            print(f"  {config}: all nodes serializable {describe(a)}")
            elle_found = a.get("valid?") is not True
        if config == "cluster-vacuum":
            args.history_rebootstraps += summary.get("history_rebootstraps", 0)
        if config == "topology-behind":
            # A forked follower can later lead, so the fork may show in the
            # leader history too: any finding counts.
            args.fork_seen |= (bool(leader_problems) or not leader_valid
                               or bool(problems) or elle_found)
        elif leader_problems or not leader_valid or problems or elle_found:
            raise CheckFailed(f"{config}: checks failed")


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
    parser.add_argument("--cluster", action="store_true")
    parser.add_argument("--topology", action="store_true",
                        help="cluster with planned transfers, unplanned "
                             "promotions, re-targeting and re-bootstrap")
    args = parser.parse_args()

    seed = args.seed if args.seed is not None else random.SystemRandom().getrandbits(63)
    print(f"run_isolation_check: seed={seed} rounds={args.rounds} "
          f"txns={args.txns} threads={args.threads} "
          f"elle={'yes' if args.elle_jar else 'no (cross-check only)'}")
    print(f"  rerun: {' '.join(sys.argv[:1])} --binary {args.binary} "
          f"--seed {seed} --rounds {args.rounds} --txns {args.txns} "
          f"--threads {args.threads}"
          + (f" --elle-jar {args.elle_jar}" if args.elle_jar else "")
          + (" --cluster" if args.cluster else "")
          + (" --topology" if args.topology else ""))
    sys.stdout.flush()

    shutil.rmtree(args.out, ignore_errors=True)
    args.write_skew_seen = False
    args.history_rebootstraps = 0
    args.fork_seen = False
    rng = random.Random(seed)
    try:
        for r in range(args.rounds):
            round_seed = rng.getrandbits(63)
            print(f"round {r + 1}/{args.rounds} seed={round_seed}")
            sys.stdout.flush()
            if args.cluster or args.topology:
                check_cluster_round(args, round_seed, args.out / f"round-{r}")
            else:
                check_round(args, round_seed, args.out / f"round-{r}")
        if (args.cluster and not args.topology
                and args.history_rebootstraps == 0):
            raise CheckFailed(
                "cluster-vacuum: no follower fell behind what vacuum kept; "
                "the run never reached the case min_resumable_sequence "
                "guards (#168)")
        if args.topology and not args.fork_seen:
            raise CheckFailed(
                "topology-behind: no round detected a fork; the harness is "
                "not shown to be sensitive to a follower ahead of its leader")
        if (args.elle_jar and not args.cluster and not args.topology
                and not args.write_skew_seen):
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
