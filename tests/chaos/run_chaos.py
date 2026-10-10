#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Chaos rig orchestrator. See docs/chaos_testing_design.md.

Mounts chaosfs, then runs episodes: a fresh database directory and a chain of
process lives on it. Each life draws a worker configuration (from its seed,
inside chaos_worker), a hazard timeline and a terminator:

  windows     fsync EIO (Linux semantics), write/read/metadata EIO, short
              writes, ENOSPC (extent or copy-on-write), EROFS, latency, stalls
  events      eviction of pages whose writeback failed, background writeback
              (clean or failing)
  limits      RLIMIT_AS (std::bad_alloc anywhere) and RLIMIT_NOFILE
  terminator  power loss, SIGKILL, or a clean close with the hazards lifted

After each life it copies the directory the life left, and `chaos_worker
check` verifies the history against it. The next life reopens the same
directory, so every life after the first starts from a crash or a close.

On failure it keeps, under <work>/failure/: the directory before and after
the life, the history, the checker's report, the timeline, the worker's
output and chaosfs's fault log, and prints a rerun line. A seed fixes each
life's configuration, timeline and chaosfs decisions by operation count, not
thread interleaving, so it reproduces a failure's shape; the saved image
reproduces the recovery exactly:

  chaos_worker check --dir <failure>/after --work /tmp/w --history \\
      <failure>/history.bin --state <failure>/state.bin --seed <life seed>

Usage:
  run_chaos.py --worker build/linux/x86_64/release/chaos_worker --minutes 30
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import random
import re
import resource
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Frame types (tests/crash/crash_model.h, FrameType).
OPENED, INTENT, COMMIT, ABORT, WATERMARK, THROW, REJECTED, VIEW, OPEN_FAILED, \
    VIOLATION, CLOSED = range(1, 12)
PROGRESS = {COMMIT, ABORT, VIEW, WATERMARK}


def load_isolation_checks():
    """scripts/run_isolation_check.py, for its cross_check and run_elle."""
    path = HERE.parent.parent / "scripts" / "run_isolation_check.py"
    spec = importlib.util.spec_from_file_location("run_isolation_check", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ISOLATION = None


class RigError(Exception):
    """The rig itself broke (chaosfs died, a tool is missing): not a finding."""


class Failure(Exception):
    pass


class Control:
    def __init__(self, path: str):
        self.path = path

    def __call__(self, cmd: str, **kw):
        req = dict(kw, cmd=cmd)
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(120)
            s.connect(self.path)
            s.sendall((json.dumps(req) + "\n").encode())
            line = s.makefile().readline()
        if not line:
            raise RigError(f"chaosfs closed the control socket on {cmd}")
        reply = json.loads(line)
        if not reply["ok"]:
            raise RigError(f"chaosfs {cmd}: {reply['error']}")
        return reply["result"]


class History:
    """Collects a worker's frames from its pipe, keeping the raw stream for
    the checker and the timing the orchestrator needs."""

    def __init__(self, fd: int):
        self.fd = fd
        self.raw = bytearray()
        self.opened_at: float | None = None
        self.open_failed = False
        self.closed = False
        self.last_progress = time.monotonic()
        self.frames = 0
        self._pos = 0
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        while True:
            chunk = os.read(self.fd, 1 << 16)
            if not chunk:
                break
            self.raw += chunk
            self._parse()
        os.close(self.fd)

    def _parse(self):
        while len(self.raw) - self._pos >= 5:
            (n,) = struct.unpack_from("<I", self.raw, self._pos)
            if len(self.raw) - self._pos - 4 < n:
                return
            kind = self.raw[self._pos + 4]
            self._pos += 4 + n
            self.frames += 1
            if kind == OPENED:
                self.opened_at = time.monotonic()
            elif kind == OPEN_FAILED:
                self.open_failed = True
            elif kind == CLOSED:
                self.closed = True
            if kind in PROGRESS:
                self.last_progress = time.monotonic()

    def join(self, timeout: float):
        self._thread.join(timeout)


class ElleStream:
    """Collects an isolation_history --chaos-child's lines (#232): I, C and D
    for the history, O when the database opened, F when DB::open failed, R
    when resume() asked for a reopen, X with close()'s result."""

    def __init__(self, fd: int):
        self.fd = fd
        self.raw = bytearray()
        self.opened_at: float | None = None
        self.open_failed = False
        self.reopen_required = False
        self.closed = False
        self.close_ok = False
        self.last_progress = time.monotonic()
        self._pos = 0
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        while True:
            chunk = os.read(self.fd, 1 << 16)
            if not chunk:
                break
            self.raw += chunk
            self._parse()
        os.close(self.fd)

    def _parse(self):
        while True:
            end = self.raw.find(b"\n", self._pos)
            if end < 0:
                return
            line = bytes(self.raw[self._pos:end])
            self._pos = end + 1
            tag = line[:1]
            if tag == b"O":
                self.opened_at = time.monotonic()
            elif tag == b"F":
                self.open_failed = True
            elif tag == b"R":
                self.reopen_required = True
            elif tag == b"X":
                self.closed = True
                self.close_ok = line.startswith(b"X ok")
            if tag in (b"C", b"D"):
                self.last_progress = time.monotonic()

    def join(self, timeout: float):
        self._thread.join(timeout)


def elle_bases(stream: bytes, max_element: int, max_key: int) -> tuple[int, int]:
    """The highest element and key an isolation_history child's stream names:
    the next life must use fresh elements and continue on its keys."""
    for line in stream.splitlines():
        parts = line.split()
        if not parts or parts[0] not in (b"I", b"C"):
            continue
        # I <proc> <t> <sync> <n> mops... ; C <proc> <t> <type> <seq> <dur> <n> mops...
        i = 5 if parts[0] == b"I" else 7
        try:
            while i < len(parts):
                kind, key = parts[i], int(parts[i + 1])
                max_key = max(max_key, key)
                if kind == b"A":
                    max_element = max(max_element, int(parts[i + 2]))
                    i += 3
                elif kind == b"R":
                    n = int(parts[i + 2])
                    i += 3 + n
                else:  # N <key>
                    i += 2
        except (IndexError, ValueError):
            continue  # a line cut short by the end of the life
    return max_element, max_key


def log_uniform(rng: random.Random, lo: float, hi: float) -> float:
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


WINDOWS = {"fsync_eio": 5, "write_eio": 2, "read_eio": 2, "meta_eio": 2,
           "short": 1, "enospc": 3, "erofs": 1, "latency": 2, "stall": 2}
TERMINATORS = {"power": 45, "sigkill": 35, "clean": 20}
EVENTS = {"evict", "writeback", "writeback_fail"}
# Per-life resource limits on the worker. RLIMIT_AS cannot run under ASan,
# which reserves terabytes of shadow memory: disable it there.
RESOURCES = {"rlimit_as", "rlimit_nofile"}
# Lines of worker.log a failure prints: enough for a sanitizer's stacks.
WORKER_LOG_TAIL = 80
# Not a hazard: `vacuum` in --disable leaves the worker's vacuum thread out.
WORKLOAD = {"vacuum"}
HAZARDS = set(WINDOWS) | set(TERMINATORS) | EVENTS | RESOURCES | WORKLOAD


def draw_window(rng: random.Random, disabled: set[str]) -> dict | None:
    kinds = [k for k in WINDOWS if k not in disabled]
    if not kinds:
        return None
    kind = rng.choices(kinds, weights=[WINDOWS[k] for k in kinds])[0]
    if kind == "fsync_eio":
        # Sometimes the pages a failed fdatasync lost are evicted at once, so
        # the next read or resume() already sees what the disk holds.
        p, evict = rng.choice([0.05, 0.3, 1.0]), rng.choice([0.0, 0.5])
        return {"fsync_eio": p, "evict_failed": 0.0 if "evict" in disabled else evict}
    if kind == "write_eio":
        return {"write_eio": rng.choice([0.01, 0.1])}
    if kind == "read_eio":
        return {"read_eio": rng.choice([0.01, 0.1])}
    if kind == "meta_eio":
        return {"meta_eio": rng.choice([0.05, 0.3])}
    if kind == "short":
        return {"write_short": 0.1}
    if kind == "enospc":
        # Resolved to a byte capacity when the window opens: what is in use
        # then, plus a little headroom.
        return {"capacity_headroom": rng.choice([0, 64 << 10, 1 << 20, 8 << 20])}
    if kind == "erofs":
        return {"erofs": True}
    if kind == "latency":
        return {"latency_p": 0.3, "latency_ms": rng.choice([1, 5, 20])}
    return {"stall_ms": rng.choice([200, 1000, 3000]),
            "stall_ops": rng.choice(["all", "fsync"])}


def draw_plan(rng: random.Random, max_life_s: float, disabled: set[str]) -> dict:
    # Every draw happens whatever is disabled, so disabling a hazard leaves
    # the rest of a seed's plans as they were.
    duration = log_uniform(rng, 0.05, max_life_s)
    events = []
    for wid in range(rng.choice([0, 1, 1, 2, 3])):
        start = rng.uniform(0, duration)
        length = log_uniform(rng, 0.02, 2.0)
        faults = draw_window(rng, disabled)
        if faults is not None:
            events.append({"t": start, "kind": "on", "id": wid, "faults": faults})
            events.append({"t": start + length, "kind": "off", "id": wid})
    evict_at = rng.uniform(0, duration) if rng.random() < 0.5 else None
    if evict_at is not None and "evict" not in disabled:
        events.append({"t": evict_at, "kind": "evict"})
    writeback_at = rng.uniform(0, duration) if rng.random() < 0.5 else None
    writeback_fail = rng.random() < 0.3
    if writeback_at is not None and not (
            "writeback" in disabled or (writeback_fail and "writeback_fail" in disabled)):
        events.append({"t": writeback_at, "kind": "writeback", "fail": writeback_fail})
    events.sort(key=lambda e: e["t"])
    terms = [t for t in TERMINATORS if t not in disabled]
    terminator = rng.choices(terms, weights=[TERMINATORS[t] for t in terms])[0]
    # A limit applies from the start (it lands during open: the worker
    # reserves 230-370 MiB of address space for thread stacks and malloc
    # arenas) or, set by the worker once the database is open, at its usage
    # then plus a small headroom (it lands during the run).
    limits = {}
    as_draw = (rng.random() < 0.15, rng.random() < 0.3,
               rng.choice([256, 320, 400]) << 20, rng.choice([0, 1, 4, 16]) << 20)
    if as_draw[0] and "rlimit_as" not in disabled:
        limits["as"] = as_draw[2] if as_draw[1] else ["run", max(as_draw[3], 1 << 16)]
    nofile_draw = (rng.random() < 0.15, rng.random() < 0.3,
                   rng.choice([32, 64, 128]), rng.choice([1, 2, 4, 16]))
    if nofile_draw[0] and "rlimit_nofile" not in disabled:
        limits["nofile"] = nofile_draw[2] if nofile_draw[1] else ["run", nofile_draw[3]]
    return {"duration": duration, "events": events, "terminator": terminator,
            "limits": limits,
            "subset": rng.random() < 0.25,
            "evict_after_kill": rng.random() < 0.3 and "evict" not in disabled,
            "crash_seed": rng.getrandbits(32)}


class Rig:
    def __init__(self, args):
        self.args = args
        self.work = Path(args.work).resolve()
        self.mnt = self.work / "mnt"
        self.sock = str(self.work / "chaosfs.sock")
        self.ctl = Control(self.sock)
        self.fs_proc: subprocess.Popen | None = None
        self.totals: dict[str, int] = {}
        self.open_errors: dict[str, int] = {}

    # -- chaosfs --------------------------------------------------------------

    def mount(self):
        self.mnt.mkdir(parents=True, exist_ok=True)
        subprocess.run(["fusermount3", "-u", "-q", str(self.mnt)], check=False)
        self.fs_log = open(self.work / "chaosfs.stderr", "w")
        self.fs_proc = subprocess.Popen(
            [sys.executable, str(HERE / "chaosfs.py"), str(self.mnt),
             "--control", self.sock, "--seed", str(self.args.seed)],
            stdout=self.fs_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.fs_proc.poll() is not None:
                raise RigError("chaosfs exited during mount; see chaosfs.stderr")
            if os.path.ismount(self.mnt) and os.path.exists(self.sock):
                try:
                    self.ctl("ping")
                    return
                except OSError:
                    pass
            time.sleep(0.05)
        raise RigError("chaosfs did not mount within 20 s")

    def unmount(self):
        subprocess.run(["fusermount3", "-u", "-q", str(self.mnt)], check=False)
        if self.fs_proc:
            try:
                self.fs_proc.wait(10)
            except subprocess.TimeoutExpired:
                self.fs_proc.kill()

    def check_fs_alive(self):
        if self.fs_proc.poll() is not None:
            raise RigError("chaosfs exited; see chaosfs.stderr")

    # -- one life -------------------------------------------------------------

    def run_life(self, life: dict) -> dict:
        """Runs the worker under the life's plan and ends it. Returns what the
        checker needs to know about how it ended."""
        plan = life["plan"]
        faults_base = {"odirect_refused": life["odirect_refused"]}
        self.ctl("faults", set=faults_base, seed=life["seed"])
        rfd, wfd = os.pipe()
        # After a failed open, the next open runs with every hazard lifted,
        # resource limits included.
        limits = {} if life["quiet_open"] else plan["limits"]
        start_limits = {k: v for k, v in limits.items() if not isinstance(v, list)}
        run_args = ["--no-vacuum"] if "vacuum" in self.args.disable else []
        # With nothing that can fail a read, a read's I/O error is the engine's.
        if {"read_eio", "evict", "rlimit_nofile"} <= self.args.disable:
            run_args.append("--strict-reads")
        for name, flag in (("as", "--as-headroom"), ("nofile", "--nofile-headroom")):
            if isinstance(limits.get(name), list):
                run_args += [flag, str(limits[name][1])]

        def apply_limits():
            # Soft limits only: raising a hard limit needs privileges.
            for name, which in (("as", resource.RLIMIT_AS),
                                ("nofile", resource.RLIMIT_NOFILE)):
                if name in start_limits:
                    hard = resource.getrlimit(which)[1]
                    resource.setrlimit(which, (start_limits[name], hard))

        env = dict(os.environ)
        if "as" in limits:
            # glibc otherwise serves allocations from arenas reserved at
            # startup, and the address space hardly grows after open: map
            # every allocation of a page or more, so the limit can bite.
            env["MALLOC_MMAP_THRESHOLD_"] = "4096"
        if self.args.workload == "elle":
            argv = self.elle_command(life, wfd)
        else:
            argv = [self.args.worker, "run", "--dir", str(life["db"]), "--seed",
                    str(life["seed"]), "--fd", str(wfd)] + run_args
        with open(life["dir"] / "worker.log", "w") as wlog:
            proc = subprocess.Popen(
                argv, pass_fds=(wfd,), stdout=wlog, stderr=subprocess.STDOUT,
                preexec_fn=apply_limits if start_limits else None, env=env)
        os.close(wfd)
        hist = ElleStream(rfd) if self.args.workload == "elle" else History(rfd)
        active: dict[int, dict] = {}
        pending = list(plan["events"])
        # Whether an eviction dropped pages during or after the life, from
        # chaosfs's own count: it evicts on its own too (evict_failed).
        evicted_before = self.ctl("stats")["pages_evicted"]
        # After a failed open, the next open must succeed with no hazard in
        # force (I5): its timeline starts once the database is open.
        start = time.monotonic()
        quiet_open = life["quiet_open"]
        exited_early = False
        while True:
            self.check_fs_alive()
            now = time.monotonic()
            if quiet_open:
                if hist.opened_at is None:
                    if proc.poll() is not None:
                        exited_early = True
                        break
                    if now - start > 60:
                        raise Failure("with every hazard lifted, DB::open did "
                                      "not finish within 60 s")
                    time.sleep(0.005)
                    continue
                start = hist.opened_at
                quiet_open = False
            t = now - start
            while pending and pending[0]["t"] <= t:
                self.apply_event(pending.pop(0), active, faults_base, life["cow"])
            if t >= plan["duration"]:
                break
            if proc.poll() is not None:
                exited_early = True
                break
            time.sleep(0.005)

        outcome = {"terminator": plan["terminator"], "exited_early": exited_early,
                   "limits": sorted(limits), "ended_ns": time.monotonic_ns()}
        if exited_early:
            self.ctl("clear")
        elif plan["terminator"] == "power":
            # The kernel keeps a killed process alive while one of its
            # requests is held in chaosfs, so the power is cut first and the
            # mount fenced until the process is gone.
            self.ctl("freeze")
            proc.kill()
            outcome["crash"] = self.ctl("crash", seed=plan["crash_seed"],
                                        subset=plan["subset"])
            proc.wait(60)
            self.ctl("unfence")
        elif plan["terminator"] == "sigkill":
            proc.kill()
            proc.wait(60)
            self.ctl("clear")
            if plan["evict_after_kill"]:
                self.ctl("evict")
        else:
            self.ctl("clear")
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(self.args.close_timeout)
            except subprocess.TimeoutExpired:
                stacks = self.stacks(proc.pid)
                proc.kill()
                proc.wait()
                (life["dir"] / "stacks.txt").write_text(stacks)
                hist.join(10)
                life["history"].write_bytes(bytes(hist.raw))
                raise Failure(f"clean close did not finish within "
                              f"{self.args.close_timeout} s with every hazard "
                              f"lifted (I9); stacks in stacks.txt")
        self.ctl("clear")
        outcome["evicted"] = self.ctl("stats")["pages_evicted"] > evicted_before
        hist.join(30)
        life["history"].write_bytes(bytes(hist.raw))
        rc = proc.returncode
        outcome["returncode"] = rc
        outcome["open_failed"] = hist.open_failed
        outcome["closed"] = hist.closed
        killed = rc == -signal.SIGKILL and not exited_early
        out_of_memory = rc == 5 and "as" in limits
        outcome["out_of_memory"] = out_of_memory
        # The Elle child exits 6 when resume() asks for a reopen (#240): the
        # documented recovery after the page cache lost published data.
        reopen = self.args.workload == "elle" and rc == 6
        if reopen and not outcome["evicted"]:
            raise Failure("resume() asked for a reopen with no page lost to "
                          "an eviction")
        if not killed and not out_of_memory and not reopen and rc not in (0, 4):
            # The Elle child never exits 1 itself; a sanitizer report, a leak
            # found at exit included, does (#398).
            sanitizer = self.args.workload == "elle" and rc == 1
            raise Failure(f"the worker died on its own (returncode {rc}"
                          f"{', a sanitizer report' if sanitizer else ''})")
        if rc == 4 and not hist.open_failed:
            raise Failure("worker exited 4 without reporting a failed open")
        if plan["terminator"] == "clean" and not exited_early and rc == 0 \
                and not hist.closed:
            raise Failure("clean close returned without the DB closing")
        return outcome

    def apply_event(self, ev: dict, active: dict, base: dict, cow: bool):
        kind = ev["kind"]
        if kind == "on":
            faults = dict(ev["faults"])
            if "capacity_headroom" in faults:
                used = self.ctl("stats")["used"]
                faults["capacity"] = used + faults.pop("capacity_headroom")
                faults["cow"] = cow
            active[ev["id"]] = faults
        elif kind == "off":
            active.pop(ev["id"], None)
        elif kind == "evict":
            self.ctl("evict")
            return
        elif kind == "writeback":
            self.ctl("writeback", seed=int(ev["t"] * 1e6), fail=ev["fail"])
            return
        merged = dict(base)
        for faults in active.values():
            merged.update(faults)
        self.ctl("faults", set=merged)

    @staticmethod
    def stacks(pid: int) -> str:
        for cmd in (["eu-stack", "-p", str(pid)],
                    ["gdb", "-p", str(pid), "-batch", "-ex", "thread apply all bt"]):
            if shutil.which(cmd[0]):
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
                return r.stdout + r.stderr
        return "no eu-stack or gdb to collect stacks"

    # -- check ----------------------------------------------------------------

    def check_life(self, life: dict, outcome: dict) -> dict:
        after = life["dir"] / "after"
        if after.exists():
            shutil.rmtree(after)
        if life["db"].exists():
            shutil.copytree(life["db"], after)
        else:
            after.mkdir()
        # Keep the state the checker starts from, for the failure record.
        if life["state"].exists():
            shutil.copy(life["state"], life["dir"] / "state.bin")
        cmd = [self.args.worker, "check", "--dir", str(after),
               "--work", str(life["dir"] / "check"),
               "--history", str(life["history"]), "--state", str(life["state"]),
               "--seed", str(life["seed"]), "--report", str(life["dir"] / "report.txt")]
        cmd += ["--terminator",
                "exit" if outcome["exited_early"] else outcome["terminator"]]
        if outcome["evicted"]:
            cmd.append("--evicted")
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if r.returncode == 1:
            raise Failure(r.stderr.strip())
        if r.returncode != 0:
            raise RigError(f"checker exited {r.returncode}: {r.stderr.strip()}")
        return json.loads(r.stdout.strip().splitlines()[-1])

    # -- the Elle workload (#232) ----------------------------------------------

    def elle_command(self, life: dict, wfd: int) -> list[str]:
        e = self.elle
        argv = [self.args.worker, "--chaos-child", "--config", self.args.config,
                "--seed", str(life["seed"]), "--threads", str(self.args.threads),
                "--dir", str(life["db"]), "--fd", str(wfd),
                "--process-base", str(e["epoch"] * self.args.threads),
                "--element-base", str(e["max_element"] + 1),
                # The KeyPool starts on the keys the last life was appending to.
                "--key-base", str(max(0, e["max_key"] + 1 - 8))]
        if "vacuum" in self.args.disable:
            argv.append("--no-vacuum")
        return argv

    def elle_start_episode(self, ep_root: Path):
        if ep_root.exists():
            shutil.rmtree(ep_root)
        ep_root.mkdir(parents=True)
        self.elle = {"epoch": 0, "max_element": -1, "max_key": -1,
                     "root": ep_root, "manifest": [f"start {time.monotonic_ns()}"]}

    def elle_after_life(self, life: dict, outcome: dict, hist_raw: bytes) -> dict:
        """Records the life as an epoch of the episode's history."""
        e = self.elle
        stream = e["root"] / f"epoch{e['epoch']}.txt"
        stream.write_bytes(hist_raw)
        e["max_element"], e["max_key"] = elle_bases(hist_raw, e["max_element"],
                                                    e["max_key"])
        power = outcome["terminator"] == "power" and not outcome["exited_early"]
        text = hist_raw.decode(errors="replace")
        opened = "\nO\n" in "\n" + text
        closed_ok = "\nX ok" in "\n" + text
        e["manifest"].append(
            f"epoch {e['epoch']} {outcome['ended_ns']} "
            f"{int(power or outcome['evicted'])} {int(opened)} {int(closed_ok)} {stream}")
        e["epoch"] += 1
        open_failed = "\nF " in "\n" + text
        if open_failed:
            # DB::open's error, without the paths that differ between runs.
            msg = text.split("\nF ", 1)[1].split("\n", 1)[0] if "\nF " in text \
                else text[2:].split("\n", 1)[0]
            msg = re.sub(r"'[^']*'", "'…'", msg)[:100]
            self.open_errors[msg] = self.open_errors.get(msg, 0) + 1
        return {"opened": opened, "open_failed": open_failed,
                "closed_ok": closed_ok, "cache_lost": power or outcome["evicted"]}

    def elle_end_episode(self, ep_dir: Path) -> dict:
        """Builds the episode's history and checks it: the commit-sequence
        cross-check always, Elle when a jar is given."""
        e = self.elle
        (e["root"] / "manifest.txt").write_text("\n".join(e["manifest"]) + "\n")
        final = e["root"] / "final_db"
        if (ep_dir / "db").exists():
            shutil.copytree(ep_dir / "db", final)
        else:
            final.mkdir()
        history_path = e["root"] / "history.json"
        r = subprocess.run(
            [self.args.worker, "--absorb", str(e["root"] / "manifest.txt"),
             "--config", self.args.config, "--seed", "1",
             "--threads", str(self.args.threads), "--dir", str(final),
             "--out", str(history_path)],
            capture_output=True, text=True, timeout=1200)
        if r.returncode != 0:
            raise Failure(f"--absorb failed: {r.stderr.strip()[-600:]}")
        summary = json.loads(Path(str(history_path) + ".chaos.json").read_text())
        history = json.loads(history_path.read_text())
        problems = ISOLATION.cross_check(history)
        if problems:
            raise Failure("cross-check: " + "; ".join(problems[:5]))
        verdict = "cross-check clean"
        if self.args.elle_jar:
            models = ("strict-serializable" if self.args.config == "guarded"
                      else "snapshot-isolation")
            a = ISOLATION.run_elle(Path(self.args.elle_jar), history_path, models,
                                   e["root"] / "elle")
            if a.get("valid?") is not True:
                raise Failure(f"Elle: not {models}: {ISOLATION.describe(a)}")
            verdict += f", {models}"
        summary["verdict"] = verdict
        # Kept until here: a failure's record holds the image the final read
        # used, so --absorb replays it exactly.
        shutil.rmtree(final, ignore_errors=True)
        return summary

    # -- driver ---------------------------------------------------------------

    def run(self) -> int:
        a = self.args
        rng = random.Random(a.seed)
        print(f"run_chaos: seed={a.seed} minutes={a.minutes} work={self.work}")
        print(f"  rerun: {sys.argv[0]} --worker {a.worker} --seed {a.seed} "
              f"--minutes {a.minutes} --lives {a.lives} --max-life-s {a.max_life_s}"
              + (f" --disable {','.join(sorted(a.disable))}" if a.disable else ""))
        sys.stdout.flush()
        self.mount()
        deadline = time.monotonic() + a.minutes * 60
        started = time.monotonic()
        lives_run = 0
        try:
            episode = 0
            while time.monotonic() < deadline:
                ep_dir = self.mnt / f"ep{episode}"
                ep_dir.mkdir()
                fd = os.open(self.mnt, os.O_RDONLY)
                os.fsync(fd)
                os.close(fd)
                state = self.work / f"state{episode}.bin"
                odirect_refused = rng.random() < 0.3
                cow = rng.random() < 0.3
                quiet_open = False
                prev_after: Path | None = None
                if a.workload == "elle":
                    self.elle_start_episode(self.work / f"elle_ep{episode}")
                for n in range(a.lives):
                    if time.monotonic() >= deadline:
                        break
                    life_dir = self.work / "life"
                    if life_dir.exists():
                        shutil.rmtree(life_dir)
                    life_dir.mkdir()
                    if prev_after is not None:
                        shutil.move(str(prev_after), life_dir / "before")
                    life = {
                        "episode": episode, "n": n, "seed": rng.getrandbits(63),
                        "plan": draw_plan(rng, a.max_life_s, a.disable),
                        "dir": life_dir, "db": ep_dir / "db", "state": state,
                        "history": life_dir / "history.bin",
                        "odirect_refused": odirect_refused, "cow": cow,
                        "quiet_open": quiet_open,
                    }
                    (life_dir / "plan.json").write_text(json.dumps(
                        {k: v for k, v in life.items()
                         if k in ("episode", "n", "seed", "plan", "odirect_refused",
                                  "cow", "quiet_open")}, indent=1))
                    try:
                        outcome = self.run_life(life)
                        if a.workload == "elle":
                            result = self.elle_after_life(
                                life, outcome, life["history"].read_bytes())
                        else:
                            result = self.check_life(life, outcome)
                    except Failure as f:
                        return self.fail(life, str(f))
                    lives_run += 1
                    if a.workload == "elle":
                        quiet_open = result["open_failed"]
                        self.tally_elle(outcome, result)
                        continue
                    quiet_open = result["open_failed"] or (outcome["out_of_memory"] \
                        and not result["opened"])
                    self.tally(outcome, result)
                    prev_after = self.work / "prev_after"
                    if prev_after.exists():
                        shutil.rmtree(prev_after)
                    shutil.move(str(life_dir / "after"), prev_after)
                    # The previous life's plan and history, for the record of
                    # a failure in the next.
                    prev_life = self.work / "prev_life"
                    if prev_life.exists():
                        shutil.rmtree(prev_life)
                    prev_life.mkdir()
                    for name in ("plan.json", "history.bin", "report.txt"):
                        if (life_dir / name).exists():
                            shutil.copy(life_dir / name, prev_life / name)
                    if a.verbose:
                        print(f"  ep {episode} life {n}: {outcome['terminator']} "
                              f"{json.dumps(result)}")
                if a.workload == "elle":
                    try:
                        summary = self.elle_end_episode(ep_dir)
                    except Failure as f:
                        return self.fail_episode(episode, str(f))
                    for k in ("ok", "in_flight", "downgraded", "group_kills",
                              "cache_losses", "lost_reads"):
                        self.totals[k] = self.totals.get(k, 0) + summary[k]
                    print(f"  episode {episode}: {summary['epochs']} lives, "
                          f"{summary['ok']} ok, {summary['verdict']}")
                    sys.stdout.flush()
                    shutil.rmtree(self.elle["root"], ignore_errors=True)
                if lives_run and lives_run % 20 == 0 or a.verbose:
                    print(f"  [{time.monotonic() - started:6.0f}s] episode {episode}: "
                          f"{lives_run} lives, {json.dumps(self.totals)}")
                    sys.stdout.flush()
                shutil.rmtree(ep_dir)
                state.unlink(missing_ok=True)
                episode += 1
            fs_stats = self.ctl("stats")
            print(f"PASS {lives_run} lives in {time.monotonic() - started:.0f}s: "
                  f"{json.dumps(self.totals)}")
            print(f"  chaosfs faults: {json.dumps(fs_stats['counts'])}")
            for msg, n in sorted(self.open_errors.items(), key=lambda kv: -kv[1])[:5]:
                print(f"  DB::open failed {n}x: {msg}")
            return 0
        finally:
            self.unmount()

    def tally(self, outcome: dict, result: dict):
        t = self.totals
        t[outcome["terminator"]] = t.get(outcome["terminator"], 0) + 1
        for k in ("committed", "views", "resumed_views", "throws", "bad_alloc",
                  "lost_at_resume"):
            t[k] = t.get(k, 0) + result[k]
        for name in outcome["limits"]:
            t[f"rlimit_{name}"] = t.get(f"rlimit_{name}", 0) + 1
        if result["open_failed"]:
            t["open_failed"] = t.get("open_failed", 0) + 1
        if result["close_failed"]:
            t["close_failed"] = t.get("close_failed", 0) + 1
        if outcome["out_of_memory"]:
            t["out_of_memory"] = t.get("out_of_memory", 0) + 1

    def tally_elle(self, outcome: dict, result: dict):
        t = self.totals
        t[outcome["terminator"]] = t.get(outcome["terminator"], 0) + 1
        for k in ("open_failed", "cache_lost"):
            if result[k]:
                t[k] = t.get(k, 0) + 1

    def fail_episode(self, episode: int, what: str) -> int:
        """An Elle episode failed its end-of-episode check: keep its lives'
        streams, manifest and history."""
        keep = self.work / "failure"
        if keep.exists():
            shutil.rmtree(keep)
        shutil.copytree(self.elle["root"], keep)
        try:
            self.ctl("log", path=str(keep / "chaosfs_faults.log"))
        except (RigError, OSError):
            pass
        print(f"FAIL episode {episode} ({self.args.config}): {what}\n"
              f"  kept in {keep}", file=sys.stderr)
        return 1

    def fail(self, life: dict, what: str) -> int:
        keep = self.work / "failure"
        if keep.exists():
            shutil.rmtree(keep)
        shutil.copytree(life["dir"], keep)
        if (self.work / "prev_life").exists():
            shutil.copytree(self.work / "prev_life", keep / "prev_life")
        try:
            self.ctl("log", path=str(keep / "chaosfs_faults.log"))
        except (RigError, OSError):
            pass
        print(f"FAIL episode {life['episode']} life {life['n']} seed {life['seed']} "
              f"terminator {life['plan']['terminator']}\n  {what}\n"
              f"  kept in {keep}", file=sys.stderr)
        # The cause is often in the worker's own output, a sanitizer report
        # above all, and the job log is all a reader may get (#398).
        log = keep / "worker.log"
        lines = log.read_text(errors="replace").splitlines() if log.exists() else []
        if lines:
            print(f"  last {min(len(lines), WORKER_LOG_TAIL)} lines of worker.log:",
                  file=sys.stderr)
            for line in lines[-WORKER_LOG_TAIL:]:
                print(f"    {line}", file=sys.stderr)
        return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--worker", required=True,
                    help="path to chaos_worker, or to isolation_history with "
                         "--workload elle")
    ap.add_argument("--workload", choices=("chaos", "elle"), default="chaos",
                    help="chaos: the single-writer workload, checked per life; "
                         "elle: isolation_history's concurrent writers, checked "
                         "per episode by the cross-check and Elle (#232)")
    ap.add_argument("--config", choices=("guarded", "unguarded"), default="guarded",
                    help="elle: which isolation configuration to run")
    ap.add_argument("--threads", type=int, default=16, help="elle: writer threads")
    ap.add_argument("--elle-jar", default=None,
                    help="elle: elle-cli jar; without it only the cross-check runs")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--lives", type=int, default=25, help="lives per episode")
    ap.add_argument("--max-life-s", type=float, default=4.0)
    ap.add_argument("--close-timeout", type=float, default=60.0)
    ap.add_argument("--work", default=None)
    ap.add_argument("--disable", default="",
                    help="comma-separated hazards to leave out: "
                         + ",".join(sorted(HAZARDS)))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    args.disable = {h for h in args.disable.split(",") if h}
    if args.workload == "elle":
        # The Elle child takes no resource limits.
        args.disable |= RESOURCES
    if args.disable - HAZARDS:
        ap.error(f"unknown hazards: {sorted(args.disable - HAZARDS)}")
    if set(TERMINATORS) <= args.disable:
        ap.error("at least one terminator must stay enabled")
    if args.seed is None:
        args.seed = random.SystemRandom().getrandbits(63)
    if args.work is None:
        args.work = tempfile.mkdtemp(prefix="bytecask_chaos_")
    args.worker = str(Path(args.worker).resolve())
    if args.workload == "elle":
        global ISOLATION
        ISOLATION = load_isolation_checks()
    try:
        return Rig(args).run()
    except RigError as e:
        print(f"run_chaos: rig error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
