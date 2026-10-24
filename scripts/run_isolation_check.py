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
and restarts, and read from), in three configurations:

    cluster         leader vacuum off. The leader's operations must pass the
                    guarded checks above; the whole history must be
                    serializable (follower reads may be stale, not
                    inconsistent) and pass the replication checks: prefix,
                    session, monotonic reads, convergence and bootstrap.
    cluster-vacuum  leader vacuum on, with retain_after set to the lowest
                    position a follower could resume from, as a replication
                    service must. Must pass everything cluster does.
    cluster-vacuum-unretained
                    the same without retain_after. Expected to detect #168
                    at least once over the run: a follower resuming below a
                    compacted file receives a batch with entries missing.

With --topology each round runs the same cluster while leadership moves:
planned transfers to a random node, unplanned promotions, re-targeting of
every follower to the new leader, and re-bootstrap of the abandoned one. The
old leader's writes that an unplanned promotion lost are relabelled :info.

    topology         an unplanned promotion takes the most advanced follower.
                     Must pass everything the cluster configuration does, and
                     the engine must refuse no slice as a fork
                     (forks_detected = 0 in every round).
    topology-behind  an unplanned promotion takes the least advanced
                     follower. A follower ahead of the new leader holds writes
                     the leader lost: its first slice from the new leader is a
                     fork, which the engine refuses (#397); the harness takes
                     the node out of service and re-bootstraps it. At least
                     one round over the run has to see a refusal
                     (forks_detected >= 1), and every other check passes:
                     what the forked node read of the lost writes before the
                     refusal is relabelled :info, as the old leader's are.

With --kill each round runs guarded and unguarded as above, but the writers
run in a child process that is SIGKILLed at a random point of its workload
and replaced,
--kill-epochs times, each child recovering what the last left; a final read
of every key follows one more recovery. Operations in flight at a kill, and
acknowledged writes not known durable then, are :info. At least one kill
over the run has to land with two or more sync writers in flight.

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
KILL_CONFIGS = ("guarded", "unguarded")
CLUSTER_CONFIGS = ("cluster", "cluster-vacuum", "cluster-vacuum-unretained")
TOPOLOGY_CONFIGS = ("topology", "topology-behind")

# Anomalies write skew may show up as under strict-serializable. Anything
# else in the unguarded configuration is a bug in the implicit W-W check or
# in snapshot reads.
WRITE_SKEW = {"G2-item", "G2-item-process", "G2-item-realtime"}


ELLE_LOG_LINE = re.compile(
    r"(?:TRACE|DEBUG|INFO|WARN|ERROR) \[\d{4}-\d\d-\d\d [^\]]*\] [^\n]*\n")


class CheckFailed(Exception):
    pass


def cross_check(history: list[dict], require_sequence: bool = True) -> list[str]:
    """Checks the history against the commit order the engine reported.

    Every :ok transaction with appends must carry its commit sequence.
    require_sequence=False is for a history through SQL, whose commit reports
    none: the commit-order rule then skips those transactions, and every
    other rule still applies.

    Returns a list of violations (empty when the history is clean). Relies on
    the generator's final read of every key after all clients finish, so the
    longest read of a key is its final value.

    Also checks real-time visibility: a read must see every append whose
    transaction completed :ok before the reading transaction was invoked.
    """
    ok_appends: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    # Per key: an element id is unique to a write within one process, and a
    # process counter restarts with each epoch, so the same id names another
    # key's write in another life.
    failed: dict[int, set[int]] = defaultdict(set)
    invoked: dict[int, set[int]] = defaultdict(set)
    reads: dict[int, list[list[int]]] = defaultdict(list)
    # (invoke index, list read) per key, and (completion index, element) per
    # key for the real-time check.
    timed_reads: dict[int, list[tuple[int, list[int]]]] = defaultdict(list)
    acked: dict[int, list[tuple[int, int]]] = defaultdict(list)
    invoke_index: dict[int, int] = {}
    missing_sequence: list[int] = []

    for op in history:
        appends = [m for m in op["value"] if m[0] == "append"]
        if op["type"] == "invoke":
            for m in appends:
                invoked[m[1]].add(m[2])
            invoke_index[op["process"]] = op["index"]
        elif op["type"] == "ok":
            seq = op.get("sequence")
            if seq is None and appends and require_sequence:
                missing_sequence.append(op["index"])
            for pos, m in enumerate(appends):
                ok_appends[m[1]].append((seq, pos, m[2]))
                acked[m[1]].append((op["index"], m[2]))
            for m in op["value"]:
                if m[0] == "r":
                    reads[m[1]].append(m[2])
                    timed_reads[m[1]].append(
                        (invoke_index[op["process"]], m[2]))
        elif op["type"] == "fail":
            for m in appends:
                failed[m[1]].add(m[2])

    problems: list[str] = []
    if missing_sequence:
        problems.append(f"{len(missing_sequence)} :ok appending transactions carry no commit sequence, "
                        f"first at index {missing_sequence[0]}")
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
            if e in failed[key]:
                problems.append(f"key {key}: element {e} of a failed txn read")
            elif e not in invoked[key]:
                problems.append(f"key {key}: element {e} was never appended")
        committed = sorted(ok_appends[key], key=lambda a: (a[0] or 0, a[1], a[2]))
        missing = [e for _, _, e in committed if e not in where]
        if missing:
            problems.append(
                f"key {key}: committed appends {missing[:8]} missing from "
                f"final read ({len(missing)} total)")
        present = [e for seq, _, e in committed if e in where and seq is not None]
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


# ---------------------------------------------------------------------------
# Range workload history check (#302)
#
# A range history's transactions read point keys and ranges, and write puts,
# dels and one-key range deletes. Every put's value is unique to it,
# "w<session>.<n>", so a later read names the exact version it saw without
# this script having to know which transaction wrote it, even when that
# transaction is :info.
#
# The engine assigns a monotonic commit sequence to every write that
# commits. So the committed transactions of a history, sorted by sequence, are
# the order in which their writes reached the database; the check replays them
# against a map of the key space and holds every read to that map. See
# docs/isolation_checking_design.md, *Range workload*.
# ---------------------------------------------------------------------------

VERSION = re.compile(r"^w([0-9a-f]+)\.([0-9a-f]+)$")


class Op:
    """One transaction: its invoke and its completion."""

    __slots__ = ("process", "invoke", "index", "type", "value", "sequence",
                 "epoch", "role")

    def __init__(self, invoke: dict, done: dict):
        self.process = invoke["process"]
        self.invoke = invoke["index"]
        self.index = done["index"]
        self.type = done["type"]
        self.value = done["value"]
        self.sequence = done.get("sequence")
        self.epoch = done.get("epoch", 0)
        self.role = done.get("role", "leader")

    def writes(self):
        """[(key, version)] this transaction invokes, a put's version unique
        to it and a del's None. A key written twice in one transaction holds
        its last write: the plan applies its mops in order."""
        out = {}
        for m in self.value:
            if m[0] == "put":
                out[m[1]] = f"w{m[2] >> 32:x}.{m[2] & 0xffffffff:x}"
            elif m[0] == "del":
                # ["del", key] is a point delete; ["del", from, to) a range,
                # `to` exclusive as the engine's bounds are.
                for k in range(m[1], m[2] if len(m) > 2 else m[1] + 1):
                    out[k] = None
        return list(out.items())

    def scans(self):
        """[(from, to, [(key, version)])]: the scans this transaction ran."""
        out = []
        for m in self.value:
            parts = m if isinstance(m[0], list) else [m]
            for part in parts:
                if part[0] == "scan":
                    out.append((part[1], part[2], [tuple(p) for p in part[3]]))
        return out

    def reads(self):
        """[(from, (key, version))]: a point read, then each pair a scan
        returned, with the range it was read in. A scan is one group holding
        its point read and its scan (`[["r", k, v], ["scan", pairs]]`), so
        the group is flattened."""
        def pairs(m):
            # A range workload read's result is a list of [key, version];
            # the append workload's is a list of integers, which this check
            # has nothing to say about.
            result = m[2] if len(m) > 2 else None
            if (m[0] in ("r", "scan") and isinstance(result, list)
                    and all(isinstance(p, list) for p in result)):
                return [(m[1], tuple(p)) for p in result]
            return []

        out = []
        for m in self.value:
            if isinstance(m[0], list):
                for part in m:
                    out.extend(pairs(part))
            else:
                out.extend(pairs(m))
        return out


def ops_from_history(history: list[dict]) -> list[Op]:
    """The transactions: invoke and completion matched by process, which a
    client runs one transaction at a time, in invoke order. A kill's or a
    chaos episode's lives reuse a process id, but a killed transaction has no
    completion, so the invokes and completions stay paired."""
    pending: dict[int, dict] = {}
    ops = []
    for e in sorted(history, key=lambda e: e["index"]):
        key = e["process"]
        if e["type"] == "invoke":
            pending[key] = e
        elif e["type"] in ("ok", "info", "fail"):
            ops.append(Op(pending.pop(key), e))
    return ops


class RangeModel:
    """The key space as a map from key to version."""

    def __init__(self):
        self.state: dict[int, str | None] = {}

    def apply(self, op: Op) -> None:
        for k, v in op.writes():
            self.state[k] = v


def history_check(history: list[dict], config: str) -> tuple[list[str], dict]:
    """Replays a range history against the commit order the engine
    enforced (#302), and returns the violations and a summary.

    A committed transaction carries the highest sequence it was assigned, and
    the engine assigns sequences in commit order, so sorting the committed
    transactions by it is the order their writes reached the database. The
    check replays them onto a map of the key space and holds every read to a
    state of that map.

    Two things the history cannot pin down:

    -   An :info transaction may or may not have committed; a crash may lose
        it and resume() may replay it. Each key its write named is placed
        only where a committed read named that key's exact value, just
        before that reader. A key whose last unplaced :info write is still
        the newest thing the history knows about it is unknown, and reads
        of it are not judged: the database may or may not hold it. A
        committed write of the key settles it again.
    -   A plan's entries span sequences below the one it reports, and another
        transaction's write can fall inside that span. A read is therefore
        held to the state at any point across its own transaction's window.

    guarded    every read and scan matches the order at the reading
               transaction's position.
    unguarded  every read and scan matches the order at its position or
               earlier, and at least one transaction read a state behind its
               own position: that is the exposure the range guards close. A
               run in which no read is exposed proves nothing.

    The state the database started from, s<k>, is the value at every key
    before the first write, and a read of it is judged against that. A kill
    may lose it, and the re-seed that follows one is not a transaction, so
    after the first epoch only a version this history wrote is judged.

    A final read, taken after every client stopped, must hold the state the
    order ends in: this is what catches an acknowledged write that was then
    lost, when no later read happened to touch its key.
    """
    ops = ops_from_history(history)
    # A transaction that wrote nothing committed nothing and carries no
    # sequence. Its snapshot may be anywhere, so it is not placed; it is
    # counted only.
    committed = sorted((op for op in ops
                        if op.type == "ok" and op.sequence is not None),
                       key=lambda op: op.sequence)
    if not committed:
        return (["no transaction committed"], {})

    info = [op for op in ops if op.type == "info"]
    # key -> version -> the first committed transaction that read it.
    first_reader: dict[int, dict[str, Op]] = defaultdict(dict)
    for c in committed:
        for _, (k, v) in c.reads():
            if isinstance(v, str) and VERSION.match(v):
                first_reader[k].setdefault(v, c)
    info_before: dict[int, dict[int, str]] = defaultdict(dict)
    # (the :info, the key) a committed read placed, by the value it wrote.
    placed_infos: set[tuple[int, int]] = set()
    unplaced = 0
    for iw in info:
        for k, v in iw.writes():
            if v is None:
                continue
            reader = first_reader.get(k, {}).get(v)
            if reader is None:
                unplaced += 1
            else:
                info_before[id(reader)][k] = v
                placed_infos.add((id(iw), k))

    # The generator writes s<k> to every key before the run, so each key the
    # history names starts there. A read that shows another s value names it.
    # A kill may lose a seed, so after the first epoch only a write of this
    # history is judged.
    seed: dict[int, str] = {}
    for op in ops:
        for k, _ in op.writes():
            seed.setdefault(k, f"s{k}")
        for _, (k, _) in op.reads():
            seed.setdefault(k, f"s{k}")
        for _, (k, v) in op.reads():
            if isinstance(v, str) and v.startswith("s"):
                seed[k] = v

    problems: list[str] = []
    exposures = 0
    placed_info = 0
    model = RangeModel()
    model.state.update(seed)
    # key -> the invoke index of the last :info transaction that wrote it,
    # when no committed read has named that write's value.
    info_wrote: dict[int, int] = {}
    # key -> every version the order has held for it, so an unguarded read
    # that names none of them is a violation, not an exposure.
    seen: dict[int, set[str]] = defaultdict(set)
    for k, v in seed.items():
        seen[k].add(v)
    # The last few states of the order, oldest first: what a transaction
    # whose plan spans several sequences may have seen.
    recent: list[dict[int, str | None]] = [dict(seed)]
    for c in committed:
        for k, v in info_before[id(c)].items():
            model.state[k] = v
            placed_info += 1
        before = dict(model.state)
        model.apply(c)
        after = dict(model.state)
        # Every version the order has held, including one written and replaced
        # within a transaction's own plan.
        for k, v in c.writes():
            seen[k].add(v)
            info_wrote.pop(k, None)
        for iw in info:
            if iw.invoke < c.index:
                for k, v in iw.writes():
                    seen[k].add(v)
        for op in info:
            if op.invoke >= c.index:
                continue
            for k, _ in op.writes():
                if (id(op), k) not in placed_infos:
                    info_wrote[k] = op.invoke
        # A key whose last unplaced :info write is still the newest thing the
        # history knows about it is unknown: that :info may or may not have
        # committed.
        unknown = set(info_wrote)
        # What this transaction writes is not in its own snapshot.
        settles = {k for k, _ in c.writes()}
        # The plan's entries span sequences below the one it reports: one per
        # write, and the markers a plan of more than one write is framed by.
        span = len(c.writes())
        if span > 1:
            span += 2
        states = recent[-span:] + [after]

        for _, (k, v) in c.reads():
            if not isinstance(v, str):
                continue
            if k in unknown or k in settles:
                continue
            # A seed value is only judgeable in the first epoch: a kill may
            # lose it, and the re-seed that follows one is not a transaction.
            if v.startswith("s") and c.epoch != 0:
                continue
            if any(state.get(k) == v for state in states):
                continue
            # Unguarded: a read may be behind its own position, but only to a
            # value the order has held for the key.
            if config == "unguarded" and v in seen.get(k, ()):
                exposures += 1
                continue
            if v in seen.get(k, ()):
                problems.append(
                    f"process {c.process} (sequence {c.sequence}) read key {k} "
                    f"= {v}, a state the order has already left "
                    f"({before.get(k)} there)")
            else:
                problems.append(
                    f"process {c.process} (sequence {c.sequence}) read key {k} "
                    f"= {v}, which the order never holds there "
                    f"({before.get(k)})")
        for from_, to_, pairs in c.scans():
            shown = {k for k, _ in pairs}
            # Only a version this history wrote can be judged: a key holding
            # its seed may have survived a kill or not, and the re-seed that
            # follows one is not a transaction of the history.
            held = {k for k, v in before.items()
                    if from_ <= k < to_ and isinstance(v, str)
                    and VERSION.match(v) and k not in settles and k not in unknown}
            missed = sorted(held - shown)
            if not missed:
                continue
            if config == "guarded":
                problems.append(
                    f"process {c.process} (sequence {c.sequence}) scanned "
                    f"[{from_}, {to_}) and missed keys {missed[:8]} the order "
                    f"has live there")
            else:
                exposures += 1
        recent.append(after)
        if len(recent) > 8:
            del recent[0]

    # The final read, taken after every client stopped, is the state the
    # database kept: every write the order holds has to be in it, and nothing
    # else. It is what catches a lost acknowledged write no later read
    # happened to touch.
    # A seed value counts here: a key the order ends with deleted reads as
    # its seed again only where the delete was lost. Every key the final read
    # names a value for is compared, seed or version.
    unknown = set(info_wrote)
    for fin in (op for op in ops if op.role == "final" and op.type == "ok"):
        for _, (k, v) in fin.reads():
            if not isinstance(v, str):
                continue
            if k in unknown and not v.startswith("s"):
                continue
            if model.state.get(k) != v:
                problems.append(
                    f"final read (process {fin.process}) has key {k} = {v}, "
                    f"but the order ends with {model.state.get(k)} there")

    # A transaction that committed nothing carries no sequence, so the
    # engine's order does not place it. Its snapshot is the state of every
    # committed transaction whose completion preceded its invoke: a read of
    # anything the order held by then is valid, and beyond it is not.
    # A transaction that committed nothing is not judged: it carries no
    # sequence, and a value it read can carry one above its own completion
    # time, since a plan's entries span the sequences below the one it
    # reports. The history cannot place it, so it is counted only.

    reads_only = [op for op in ops if op.type == "ok" and op.sequence is None]
    summary = {
        "committed": len(committed),
        "read_only": len(reads_only),
        "info": len(info),
        "info_placed": placed_info,
        "info_unplaced": unplaced,
        "reads": sum(len(c.reads()) for c in committed),
        "exposures": exposures,
    }
    if config == "unguarded" and exposures == 0:
        problems.append(
            "unguarded: no read or scan saw a state behind its own commit; "
            "the run is not shown to reach write skew")
    return problems[:20], summary


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


def relabel_lost(ops: list[dict], lost: set[int], cap: int) -> int:
    """Relabels the :ok operations in ops that touched a lost element as
    :info with their reads cleared, which Elle and the cross-checks accept
    either way, and caps their session waits at cap: a wait above it was
    on the lost branch, whose sequences the new leader reassigned, and what
    it waited for still holds up to cap. Returns how many were relabelled."""
    changed = 0
    for op in ops:
        if op.get("wait", 0) > cap:
            op["wait"] = cap
        wrote_lost = any(m[0] == "append" and m[2] in lost
                         for m in op["value"])
        read_lost = any(m[0] == "r" and set(m[2] or []) & lost
                        for m in op["value"])
        if wrote_lost or read_lost:
            op["type"] = "info"
            op["value"] = [[m[0], m[1], None] if m[0] == "r" else m
                           for m in op["value"]]
            op.pop("sequence", None)
            changed += 1
    return changed


def term_lineage(events: list[dict]) -> tuple[dict, dict, dict]:
    """Each leader term's lineage: term k's history is term parent[k]'s up to
    branch[k], then the writes of leader[k] in k. A planned transfer's
    target caught up with the old leader; an unplanned promotion's target
    holds the history of the term it last tailed ("held"), which is an
    earlier one when it was left ahead of a promotion under --promote-least
    and promoted again before its first pull was refused."""
    parent: dict[int, int] = {}
    branch = {0: 0}
    leader = {0: 0}
    for ev in events:
        if ev["kind"] not in ("planned", "unplanned"):
            continue
        k = ev["epoch"]
        parent[k] = ev.get("held", k - 1) if ev["kind"] == "unplanned" else k - 1
        branch[k] = ev["durable"]
        leader[k] = ev["to"]
    return parent, branch, leader


def common_prefix(a: int, b: int, parent: dict, branch: dict) -> float:
    """The sequence up to which the histories of terms a and b agree."""
    limit = float("inf")
    while a != b:
        if a > b:
            limit, a = min(limit, branch[a]), parent[a]
        else:
            limit, b = min(limit, branch[b]), parent[b]
    return limit


def lost_by_promotion(history: list[dict],
                      summary: dict) -> list[tuple[dict, set[int]]]:
    """Each unplanned promotion with the elements it lost: those in the old
    leader's history, its own term's and any it inherited from an earlier
    branch, above the sequence where the promoted node's history departs
    from it. The returned event carries that sequence as "kept"."""
    events = summary.get("events", [])
    parent, branch, leader = term_lineage(events)
    writes: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for op in history:
        t = op.get("epoch")
        if (op["type"] == "ok" and op.get("role") == "leader"
                and leader.get(t) == op.get("node")):
            writes[t].extend((op.get("sequence", 0), m[2])
                             for m in op["value"] if m[0] == "append")
    out = []
    for ev in events:
        if ev["kind"] != "unplanned":
            continue
        old = ev["epoch"] - 1
        kept = min(ev["durable"],
                   common_prefix(old, parent[ev["epoch"]], parent, branch))
        lost: set[int] = set()
        term, cutoff = old, float("inf")
        while cutoff > kept:
            lost.update(e for seq, e in writes[term] if kept < seq <= cutoff)
            if term == 0:
                break
            cutoff, term = min(cutoff, branch[term]), parent[term]
        out.append(({**ev, "kept": kept}, lost))
    return out


def relabel_lost_writes(history: list[dict],
                        promotions: list[tuple[dict, set[int]]]) -> int:
    """Marks what an unplanned promotion lost as indeterminate.

    Replication is asynchronous: when a follower is promoted without the old
    leader, the old leader's writes in its last term above what the
    promoted node kept of it are lost by design, and so is
    anything its clients read of them. The old leader is abandoned and
    re-bootstrapped, so what it read of the lost elements in the bootstrap
    life it led in, as a follower before its term included, is relabelled
    too. A lost write that shows up on another node, or in a later life, is
    still caught. Returns how many.
    """
    changed = 0
    for ev, lost in promotions:
        lives = {op.get("life") for op in history
                 if op.get("node") == ev["from"]
                 and op.get("epoch") == ev["epoch"] - 1
                 and op.get("role") == "leader"}
        tenure = [op for op in history
                  if op.get("node") == ev["from"] and op["type"] == "ok"
                  and (op.get("life") in lives if lives
                       else op.get("epoch") == ev["epoch"] - 1)]
        changed += relabel_lost(tenure, lost, ev["kept"])
    return changed


def relabel_forked_reads(history: list[dict], summary: dict,
                         promotions: list[tuple[dict, set[int]]]) -> int:
    """Marks what a forked node read of a lost branch as indeterminate.

    A follower ahead of an unplanned promotion's target (topology-behind)
    holds the old leader's lost writes and serves reads of them until the
    engine refuses its first slice from the new leader as a fork and the
    harness re-bootstraps it. Those reads are the consequence the promotion
    rule prevents, not a finding: on the node a fork event names, in the
    life it names, operations that read a lost element become :info and
    session waits are capped, as the old leader's are. The branch point is
    one of the promotions up to the fork's epoch, so every one of them
    counts: a lower cap only weakens the session check on that node's
    reads. Reads in its next life, and on every other node, stay as they
    are. Returns how many.
    """
    changed = 0
    for ev in summary.get("events", []):
        if ev["kind"] != "fork":
            continue
        before = [(p, lost) for p, lost in promotions
                  if p["epoch"] <= ev["epoch"]]
        if not before:
            continue
        lost = set().union(*(l for _, l in before))
        cap = min(p["kept"] for p, _ in before)
        life = [op for op in history
                if op.get("node") == ev["to"] and op.get("life") == ev["life"]
                and op["type"] == "ok"]
        changed += relabel_lost(life, lost, cap)
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
        cmd.append("--force-vacuum" if config.startswith("cluster-vacuum")
                   else "--no-vacuum")
        if config == "cluster-vacuum-unretained":
            cmd.append("--no-retention")
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
        # Computed before either relabelling turns the lost appends :info.
        promotions = lost_by_promotion(history, summary)
        relabelled = relabel_lost_writes(history, promotions)
        forked_reads = relabel_forked_reads(history, summary, promotions)
        if relabelled or forked_reads:
            history_path.write_text(json.dumps(history))
        forks = summary.get("forks_detected", 0)
        print(f"  {config}: events {describe_events(summary)}; "
              f"{relabelled} operations lost to unplanned promotions; "
              f"forks_detected={forks}, {forked_reads} reads of a lost "
              f"branch on a forked node")
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
        if config == "cluster-vacuum-unretained":
            if leader_problems or not leader_valid:
                raise CheckFailed(f"{config}: leader checks failed")
            args.v168_seen |= bool(problems) or elle_found
            continue
        if leader_problems or not leader_valid or problems or elle_found:
            raise CheckFailed(f"{config}: checks failed")
        if config == "topology-behind":
            # The engine refuses the forked follower's slice and the harness
            # re-bootstraps it, so the histories stay valid; the refusal
            # itself is the detection.
            args.fork_seen |= forks >= 1
        elif forks:
            raise CheckFailed(
                f"{config}: the engine refused {forks} slices as forks; "
                f"with the most advanced follower promoted, no node is "
                f"ahead of its source")


def check_range_round(args: argparse.Namespace, seed: int,
                      round_dir: Path) -> None:
    """The range workload (#302): guarded and unguarded histories checked
    against the commit order, by history_check. Elle's models have no
    counterpart for a scan, a del or a range delete, so it does not run."""
    for config in KILL_CONFIGS:
        history_path = round_dir / f"{config}.json"
        cmd = [str(args.binary), "--config", config, "--workload", "range",
               "--seed", str(seed), "--txns", str(args.txns),
               "--threads", str(args.threads),
               "--dir", str(round_dir / f"db-{config}"),
               "--out", str(history_path)]
        if args.no_vacuum:
            cmd.append("--no-vacuum")
        if args.no_degrade:
            cmd.append("--no-degrade")
        if args.kill:
            cmd += ["--kill-epochs", str(args.kill_epochs)]
        subprocess.run(cmd, check=True)
        shutil.rmtree(round_dir / f"db-{config}", ignore_errors=True)
        if args.kill:
            kill = json.loads(
                Path(str(history_path) + ".kill.json").read_text())
            args.group_kills += kill["group_kills"]

        history = json.loads(history_path.read_text())
        problems, summary = history_check(history, config)
        verdict = "clean" if not problems else f"{len(problems)} violations"
        print(f"  {config}: history {summary}; {verdict}")
        for p in problems[:10]:
            print(f"    {p}")
        if config == "unguarded":
            args.exposed_seen |= summary.get("exposures", 0) > 0
        if problems:
            raise CheckFailed(f"{config}: range history check failed")


def check_round(args: argparse.Namespace, seed: int, round_dir: Path) -> None:
    if args.workload == "range":
        return check_range_round(args, seed, round_dir)
    for config in KILL_CONFIGS if args.kill else CONFIGS:
        history_path = round_dir / f"{config}.json"
        cmd = [str(args.binary), "--config", config, "--seed", str(seed),
               "--txns", str(args.txns), "--threads", str(args.threads),
               "--dir", str(round_dir / f"db-{config}"),
               "--out", str(history_path)]
        if args.no_vacuum:
            cmd.append("--no-vacuum")
        if args.no_degrade:
            cmd.append("--no-degrade")
        if args.kill:
            cmd += ["--kill-epochs", str(args.kill_epochs)]
        subprocess.run(cmd, check=True)
        shutil.rmtree(round_dir / f"db-{config}", ignore_errors=True)
        if args.kill:
            kill = json.loads(
                Path(str(history_path) + ".kill.json").read_text())
            args.group_kills += kill["group_kills"]

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
    parser.add_argument("--kill", action="store_true",
                        help="SIGKILL the writer process and reopen, "
                             "--kill-epochs times per history (#176)")
    parser.add_argument("--kill-epochs", type=int, default=10)
    parser.add_argument("--workload", choices=("append", "range"),
                        default="append",
                        help="range: point reads, scans, puts, dels and "
                             "one-key range deletes, checked against the "
                             "commit order (#302)")
    args = parser.parse_args()
    if args.kill and (args.cluster or args.topology):
        parser.error("--kill runs a single node")
    if args.workload == "range" and (args.cluster or args.topology):
        parser.error("--workload range runs a single node")

    seed = args.seed if args.seed is not None else random.SystemRandom().getrandbits(63)
    print(f"run_isolation_check: seed={seed} rounds={args.rounds} "
          f"txns={args.txns} threads={args.threads} "
          f"elle={'yes' if args.elle_jar else 'no (cross-check only)'}")
    print(f"  rerun: {' '.join(sys.argv[:1])} --binary {args.binary} "
          f"--seed {seed} --rounds {args.rounds} --txns {args.txns} "
          f"--threads {args.threads}"
          + (f" --elle-jar {args.elle_jar}" if args.elle_jar else "")
          + (" --workload range" if args.workload == "range" else "")
          + (" --cluster" if args.cluster else "")
          + (" --topology" if args.topology else "")
          + (f" --kill --kill-epochs {args.kill_epochs}"
             if args.kill else ""))
    sys.stdout.flush()

    shutil.rmtree(args.out, ignore_errors=True)
    args.write_skew_seen = False
    args.exposed_seen = False
    args.v168_seen = False
    args.fork_seen = False
    args.group_kills = 0
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
        if args.cluster and not args.topology and not args.v168_seen:
            raise CheckFailed(
                "cluster-vacuum-unretained: no round detected #168; the "
                "harness is not shown to be sensitive to gaps in "
                "changes_since")
        if args.topology and not args.fork_seen:
            raise CheckFailed(
                "topology-behind: the engine refused no slice as a fork in "
                "any round; the run is not shown to put a follower ahead of "
                "its leader")
        if args.kill and args.group_kills == 0:
            raise CheckFailed(
                "kill: no kill landed with two or more sync writers in "
                "flight; the run never reached a shared fdatasync")
        if (args.workload == "range" and not args.exposed_seen):
            raise CheckFailed(
                "unguarded: no range transaction read a state behind its own "
                "commit in any round; the run is not shown to reach write "
                "skew")
        if (args.elle_jar and not args.cluster and not args.topology
                and args.workload != "range"
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
