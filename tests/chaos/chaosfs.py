#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""chaosfs: an in-memory FUSE filesystem that injects the faults a POSIX
filesystem is allowed to produce, and models power loss.

See docs/chaos_testing_design.md. Every file keeps two images:

  volatile  what reads return (the page cache, in effect)
  durable   what survives a power cut

and every directory keeps a volatile and a durable entry set plus the log of
entry changes since its last fsync. fsync/fdatasync copy a file's dirty pages
to the durable image; fsync on a directory makes its entry changes durable.
Nothing else does, apart from background writeback, which the orchestrator
triggers explicitly.

A failed fdatasync follows Linux (errseq_t, 4.13+): the dirty pages are
marked clean without reaching the durable image, the error is reported once
per open file, and a later fdatasync succeeds without writing them. Reads keep
returning the lost bytes until an eviction reverts them to the durable image.

Power loss (`crash`): each dirty page reaches the durable image whole, not at
all, or torn at 512-byte sectors; the size is the durable one or the volatile
one; each directory applies a prefix of its pending entry changes (a random
subset in `subset` mode), a rename either whole or not at all.

The model (ChaosModel) has no FUSE dependency; ChaosFuse adapts it. The
control socket takes one JSON object per line and answers with one:

  {"cmd": "faults", "set": {...}, "seed": N}   replace the fault config
  {"cmd": "clear"}                              no faults
  {"cmd": "freeze"}                             block every operation
  {"cmd": "crash", "seed": N, "subset": bool}   power loss; unfreeze, fenced
  {"cmd": "unfence"}                            accept requests again
  {"cmd": "evict"}                              failed pages revert to disk
  {"cmd": "writeback", "seed": N, "fail": bool} background writeback
  {"cmd": "stats"} / {"cmd": "log", "path": P} / {"cmd": "ping"}

Usage: chaosfs.py MOUNTPOINT --control SOCKET [--seed N]
"""

from __future__ import annotations

import argparse
import errno
import functools
import json
import os
import random
import socketserver
import stat
import sys
import threading
import time
from collections import deque

PAGE = 4096
SECTOR = 512

# Probabilities are per operation of the named class; 0 disables.
DEFAULT_FAULTS = {
    "read_eio": 0.0,       # read fails, nothing read
    "write_eio": 0.0,      # write fails after landing a random prefix (maybe none)
    "write_short": 0.0,    # write lands a prefix and returns its length
    "fsync_eio": 0.0,      # fdatasync fails with Linux semantics
    "meta_eio": 0.0,       # create/mkdir/rename/unlink/truncate/fsyncdir fail;
                           # the change may or may not have happened
    "erofs": False,        # every mutation fails with EROFS
    "capacity": None,      # bytes; None = unlimited. ENOSPC past it.
    "cow": False,          # copy-on-write allocation: any write needs free space
    "odirect_refused": False,  # open(O_DIRECT) fails with EINVAL
    "latency_p": 0.0,      # share of operations delayed...
    "latency_ms": 0.0,     # ...by an exponential delay with this mean
    "stall_ms": 0.0,       # every matching operation blocks up to this long
    "stall_ops": "all",    # "all" or "fsync"
}

MUTATING = {"write", "create", "mkdir", "rename", "unlink", "rmdir",
            "truncate", "fsync", "fsyncdir"}


def _pages(start: int, end: int) -> range:
    """Page indices covering bytes [start, end)."""
    if end <= start:
        return range(0)
    return range(start // PAGE, (end - 1) // PAGE + 1)


def _npages(size: int) -> int:
    return (size + PAGE - 1) // PAGE


def _fail(code: int):
    raise OSError(code, os.strerror(code))


class Inode:
    __slots__ = ("ino", "is_dir", "mode", "data", "durable", "dirty", "failed",
                 "err_seq", "nlink", "opens", "entries", "dentries", "pending",
                 "mtime")

    def __init__(self, ino: int, is_dir: bool, mode: int):
        self.ino = ino
        self.is_dir = is_dir
        self.mode = mode
        self.nlink = 0
        self.opens = 0
        self.mtime = time.time()
        # Files.
        self.data = bytearray()
        self.durable = bytearray()
        self.dirty: set[int] = set()
        self.failed: set[int] = set()
        self.err_seq = 0
        # Directories.
        self.entries: dict[str, int] = {}
        self.dentries: dict[str, int] = {}
        self.pending: list[tuple] = []


class Handle:
    __slots__ = ("inode", "flags", "seen_err")

    def __init__(self, inode: Inode, flags: int):
        self.inode = inode
        self.flags = flags
        self.seen_err = inode.err_seq


class ChaosModel:
    """The filesystem state and fault logic. Thread-safe: every public
    operation holds one lock, and blocks while frozen."""

    def __init__(self, seed: int = 0):
        self.cond = threading.Condition()
        self.frozen = False
        self.fenced = False
        self.epoch = 0
        self.faults = dict(DEFAULT_FAULTS)
        self.rng = random.Random(seed)
        self.lat_rng = random.Random(seed ^ 0x5DEECE66D)
        self.inodes: dict[int, Inode] = {}
        self.handles: dict[int, Handle] = {}
        self.next_ino = 1
        self.next_fh = 1
        self.log: deque = deque(maxlen=20000)
        self.counts: dict[str, int] = {}
        self.root = self._new_inode(True, 0o755)
        self.root.nlink = 1

    # -- infrastructure -----------------------------------------------------

    def _new_inode(self, is_dir: bool, mode: int) -> Inode:
        ino = Inode(self.next_ino, is_dir, mode)
        self.next_ino += 1
        self.inodes[ino.ino] = ino
        return ino

    def _note(self, op: str, what: str, path: str = ""):
        # Counted by operation and kind; the detail goes to the log only.
        key = f"{op}:{what.split('(')[0].split('=')[0]}"
        self.counts[key] = self.counts.get(key, 0) + 1
        self.log.append((round(time.time(), 6), op, path, what))

    def _delay(self, op: str):
        f = self.faults
        wait = 0.0
        if f["stall_ms"] > 0 and (f["stall_ops"] == "all" or op == "fsync"):
            wait += self.lat_rng.uniform(0, f["stall_ms"]) / 1000.0
        if f["latency_p"] > 0 and self.lat_rng.random() < f["latency_p"]:
            wait += self.lat_rng.expovariate(1.0 / max(f["latency_ms"], 0.001)) / 1000.0
        if wait > 0:
            time.sleep(wait)

    def _arrive(self, op: str) -> int:
        """Called on arrival, before the lock. Returns the epoch the request
        belongs to, then applies any injected delay."""
        epoch = self.epoch
        self._delay(op)
        return epoch

    def _enter(self, op: str, epoch: int | None = None) -> int:
        """Called with the lock held. Waits out a freeze. A request that
        arrived before a power cut never happens, even if a stall or the
        freeze held it until after."""
        if epoch is None:
            epoch = self.epoch
        while self.frozen:
            self.cond.wait()
        if self.epoch != epoch or self.fenced:
            _fail(errno.EIO)
        if self.faults["erofs"] and op in MUTATING:
            self._note(op, "erofs")
            _fail(errno.EROFS)
        return epoch

    def _chance(self, name: str) -> bool:
        p = self.faults[name]
        return p > 0 and self.rng.random() < p

    def _lookup(self, path: str) -> Inode:
        node = self.root
        for part in [p for p in path.split("/") if p]:
            if not node.is_dir:
                _fail(errno.ENOTDIR)
            ino = node.entries.get(part)
            if ino is None:
                _fail(errno.ENOENT)
            node = self.inodes[ino]
        return node

    def _parent(self, path: str) -> tuple[Inode, str]:
        head, _, name = path.rstrip("/").rpartition("/")
        parent = self._lookup(head or "/")
        if not parent.is_dir:
            _fail(errno.ENOTDIR)
        return parent, name

    def _node(self, path: str | None, fh: int | None) -> Inode:
        if fh is not None and fh in self.handles:
            return self.handles[fh].inode
        if path is None:
            _fail(errno.EBADF)
        return self._lookup(path)

    def _used(self) -> int:
        return sum(_npages(len(i.data)) * PAGE + PAGE for i in self.inodes.values())

    def _free(self) -> int | None:
        cap = self.faults["capacity"]
        return None if cap is None else max(0, cap - self._used())

    def _gc(self, ino: Inode):
        if ino.nlink <= 0 and ino.opens <= 0 and ino is not self.root:
            self.inodes.pop(ino.ino, None)

    def _meta_fault(self, op: str, path: str) -> bool | None:
        """None: no fault. True/False: fail with EIO after (not) applying."""
        if self._chance("meta_eio"):
            applied = self.rng.random() < 0.5
            self._note(op, f"eio(applied={applied})", path)
            return applied
        return None

    def _need_space(self, op: str, path: str):
        free = self._free()
        if free is not None and free < PAGE:
            self._note(op, "enospc", path)
            _fail(errno.ENOSPC)

    # -- operations ---------------------------------------------------------

    def getattr(self, path: str | None, fh: int | None = None) -> dict:
        with self.cond:
            self._enter("getattr")
            n = self._node(path, fh)
            kind = stat.S_IFDIR if n.is_dir else stat.S_IFREG
            size = 4096 if n.is_dir else len(n.data)
            return {
                "st_mode": kind | n.mode, "st_nlink": max(n.nlink, 1),
                "st_size": size, "st_blocks": _npages(size) * (PAGE // 512),
                "st_blksize": PAGE, "st_uid": os.getuid(), "st_gid": os.getgid(),
                "st_mtime": int(n.mtime * 1e9), "st_ctime": int(n.mtime * 1e9),
                "st_atime": int(n.mtime * 1e9),
            }

    def readdir(self, path: str | None, fh: int | None = None) -> list[str]:
        with self.cond:
            self._enter("readdir")
            n = self._node(path, fh)
            if not n.is_dir:
                _fail(errno.ENOTDIR)
            return [".", ".."] + list(n.entries)

    def mkdir(self, path: str, mode: int):
        epoch = self._arrive("mkdir")
        with self.cond:
            self._enter("mkdir", epoch)
            parent, name = self._parent(path)
            if name in parent.entries:
                _fail(errno.EEXIST)
            self._need_space("mkdir", path)
            fault = self._meta_fault("mkdir", path)
            if fault is not False:
                d = self._new_inode(True, mode & 0o7777)
                d.nlink = 1
                parent.entries[name] = d.ino
                parent.pending.append(("link", name, d.ino))
            if fault is not None:
                _fail(errno.EIO)

    def create(self, path: str, mode: int, flags: int) -> int:
        epoch = self._arrive("create")
        with self.cond:
            self._enter("create", epoch)
            if flags & os.O_DIRECT and self.faults["odirect_refused"]:
                self._note("create", "odirect_refused", path)
                _fail(errno.EINVAL)
            parent, name = self._parent(path)
            if name in parent.entries:
                if flags & os.O_EXCL:
                    _fail(errno.EEXIST)
                n = self.inodes[parent.entries[name]]
                if flags & os.O_TRUNC:
                    self._truncate(n, 0)
                return self._open_handle(n, flags)
            self._need_space("create", path)
            fault = self._meta_fault("create", path)
            if fault is not None:
                if fault:
                    n = self._new_inode(False, mode & 0o7777)
                    n.nlink = 1
                    parent.entries[name] = n.ino
                    parent.pending.append(("link", name, n.ino))
                _fail(errno.EIO)
            n = self._new_inode(False, mode & 0o7777)
            n.nlink = 1
            parent.entries[name] = n.ino
            parent.pending.append(("link", name, n.ino))
            return self._open_handle(n, flags)

    def _open_handle(self, n: Inode, flags: int) -> int:
        fh = self.next_fh
        self.next_fh += 1
        self.handles[fh] = Handle(n, flags)
        n.opens += 1
        return fh

    def open(self, path: str, flags: int) -> int:
        epoch = self._arrive("open")
        with self.cond:
            self._enter("open", epoch)
            if flags & os.O_DIRECT and self.faults["odirect_refused"]:
                self._note("open", "odirect_refused", path)
                _fail(errno.EINVAL)
            n = self._lookup(path)
            if flags & os.O_TRUNC and not n.is_dir:
                self._truncate(n, 0)
            return self._open_handle(n, flags)

    def release(self, fh: int):
        with self.cond:
            h = self.handles.pop(fh, None)
            if h is not None:
                h.inode.opens -= 1
                self._gc(h.inode)

    def read(self, path: str | None, size: int, offset: int, fh: int) -> bytes:
        epoch = self._arrive("read")
        with self.cond:
            self._enter("read", epoch)
            n = self._node(path, fh)
            if self._chance("read_eio"):
                self._note("read", "eio", path or "")
                _fail(errno.EIO)
            return bytes(n.data[offset:offset + size])

    def write(self, path: str | None, data: bytes, offset: int, fh: int) -> int:
        epoch = self._arrive("write")
        with self.cond:
            self._enter("write", epoch)
            n = self._node(path, fh)
            size = len(data)
            if self._chance("write_eio"):
                landed = self.rng.randrange(0, size + 1) if size else 0
                self._write(n, data[:landed], offset)
                self._note("write", f"eio(landed={landed}/{size})", path or "")
                _fail(errno.EIO)
            free = self._free()
            if free is not None:
                if self.faults["cow"]:
                    if free < len(_pages(offset, offset + size)) * PAGE:
                        self._note("write", "enospc(cow)", path or "")
                        _fail(errno.ENOSPC)
                else:
                    old = _npages(len(n.data))
                    need = _npages(max(len(n.data), offset + size)) - old
                    if need * PAGE > free:
                        fit = (old + free // PAGE) * PAGE - offset
                        if fit <= 0:
                            self._note("write", "enospc", path or "")
                            _fail(errno.ENOSPC)
                        self._note("write", f"enospc(short {fit}/{size})", path or "")
                        size = fit
            if size > 1 and self._chance("write_short"):
                size = self.rng.randrange(1, size)
                self._note("write", f"short({size}/{len(data)})", path or "")
            self._write(n, data[:size], offset)
            return size

    def _write(self, n: Inode, data: bytes, offset: int):
        if not data:
            return
        end = offset + len(data)
        if len(n.data) < offset:
            n.data.extend(bytes(offset - len(n.data)))
        n.data[offset:end] = data
        for p in _pages(offset, end):
            n.dirty.add(p)
            n.failed.discard(p)
        n.mtime = time.time()

    def truncate(self, path: str | None, length: int, fh: int | None = None):
        epoch = self._arrive("truncate")
        with self.cond:
            self._enter("truncate", epoch)
            n = self._node(path, fh)
            fault = self._meta_fault("truncate", path or "")
            if fault is None or fault:
                self._truncate(n, length)
            if fault is not None:
                _fail(errno.EIO)

    def _truncate(self, n: Inode, length: int):
        old = len(n.data)
        if length < old:
            del n.data[length:]
            n.dirty = {p for p in n.dirty if p * PAGE < length}
            n.failed = {p for p in n.failed if p * PAGE < length}
            if length % PAGE:
                n.dirty.add(length // PAGE)
        elif length > old:
            n.data.extend(bytes(length - old))
            # A hole: nothing to write back, only the size. The last partial
            # page's tail changed from "past EOF" to zeros.
            if old % PAGE:
                n.dirty.add(old // PAGE)
        n.mtime = time.time()

    def fsync(self, path: str | None, datasync: int, fh: int):
        epoch = self._arrive("fsync")
        with self.cond:
            self._enter("fsync", epoch)
            h = self.handles.get(fh)
            n = h.inode if h else self._node(path, None)
            if n.is_dir:
                return self._fsyncdir(n, path or "")
            if self._chance("fsync_eio"):
                # Writeback fails part-way: some pages may have reached the
                # disk, the rest are marked clean without being written.
                done = [p for p in n.dirty if self.rng.random() < 0.3]
                self._persist(n, done, resize=False)
                n.failed |= n.dirty - set(done)
                n.dirty.clear()
                n.err_seq += 1
                if h:
                    h.seen_err = n.err_seq
                self._note("fsync", f"eio(lost_pages={len(n.failed)})", path or "")
                _fail(errno.EIO)
            if h and h.seen_err < n.err_seq:
                h.seen_err = n.err_seq
                self._note("fsync", "eio(reported)", path or "")
                _fail(errno.EIO)
            self._persist(n, list(n.dirty), resize=True)
            n.dirty.clear()

    def _persist(self, n: Inode, pages, resize: bool):
        if resize:
            if len(n.durable) > len(n.data):
                del n.durable[len(n.data):]
            elif len(n.durable) < len(n.data):
                n.durable.extend(bytes(len(n.data) - len(n.durable)))
        for p in pages:
            start = p * PAGE
            end = min(start + PAGE, len(n.data))
            if start >= end:
                continue
            if len(n.durable) < end:
                n.durable.extend(bytes(end - len(n.durable)))
            n.durable[start:end] = n.data[start:end]

    def fsyncdir(self, path: str | None, fh: int | None):
        epoch = self._arrive("fsyncdir")
        with self.cond:
            self._enter("fsyncdir", epoch)
            return self._fsyncdir(self._node(path, fh), path or "")

    def _fsyncdir(self, d: Inode, path: str):
        if self._chance("meta_eio"):
            self._note("fsyncdir", "eio", path)
            _fail(errno.EIO)
        d.dentries = dict(d.entries)
        d.pending.clear()

    def rename(self, old: str, new: str):
        epoch = self._arrive("rename")
        with self.cond:
            self._enter("rename", epoch)
            sp, sname = self._parent(old)
            dp, dname = self._parent(new)
            if sname not in sp.entries:
                _fail(errno.ENOENT)
            fault = self._meta_fault("rename", f"{old} -> {new}")
            if fault is None or fault:
                ino = sp.entries.pop(sname)
                replaced = dp.entries.get(dname)
                dp.entries[dname] = ino
                if sp is dp:
                    sp.pending.append(("rename", sname, dname, ino))
                else:
                    sp.pending.append(("unlink", sname))
                    dp.pending.append(("link", dname, ino))
                if replaced is not None and replaced != ino:
                    r = self.inodes[replaced]
                    r.nlink -= 1
                    self._gc(r)
            if fault is not None:
                _fail(errno.EIO)

    def unlink(self, path: str, is_dir: bool = False):
        op = "rmdir" if is_dir else "unlink"
        epoch = self._arrive(op)
        with self.cond:
            self._enter(op, epoch)
            parent, name = self._parent(path)
            if name not in parent.entries:
                _fail(errno.ENOENT)
            n = self.inodes[parent.entries[name]]
            if is_dir and n.entries:
                _fail(errno.ENOTEMPTY)
            fault = self._meta_fault(op, path)
            if fault is None or fault:
                del parent.entries[name]
                parent.pending.append(("unlink", name))
                n.nlink -= 1
                self._gc(n)
            if fault is not None:
                _fail(errno.EIO)

    def statfs(self) -> dict:
        with self.cond:
            cap = self.faults["capacity"]
            used = self._used()
            total = cap if cap is not None else max(used * 4, 1 << 34)
            free = max(0, total - used)
            return {"f_bsize": PAGE, "f_frsize": PAGE, "f_blocks": total // PAGE,
                    "f_bfree": free // PAGE, "f_bavail": free // PAGE,
                    "f_files": 1 << 20, "f_ffree": 1 << 20, "f_namemax": 255}

    # -- control ------------------------------------------------------------

    def set_faults(self, faults: dict, seed: int | None):
        with self.cond:
            cfg = dict(DEFAULT_FAULTS)
            unknown = set(faults) - set(cfg)
            if unknown:
                raise ValueError(f"unknown faults: {sorted(unknown)}")
            cfg.update(faults)
            self.faults = cfg
            if seed is not None:
                self.rng.seed(seed)
                self.lat_rng.seed(seed ^ 0x5DEECE66D)

    def freeze(self):
        with self.cond:
            self.frozen = True

    def evict(self) -> int:
        """Pages whose writeback failed leave the cache: reads now return
        what the disk holds."""
        with self.cond:
            reverted = 0
            for n in self.inodes.values():
                for p in n.failed:
                    start = p * PAGE
                    end = min(start + PAGE, len(n.data))
                    if start >= end:
                        continue
                    disk = bytes(n.durable[start:end])
                    n.data[start:end] = disk + bytes(end - start - len(disk))
                    reverted += 1
                n.failed.clear()
            self._note("evict", f"reverted(pages={reverted})")
            return reverted

    def writeback(self, seed: int, fail: bool) -> int:
        """Background writeback of a random share of dirty pages, as the
        kernel does between syncs. With fail, those pages are lost instead and
        the next fsync of each file reports EIO."""
        with self.cond:
            rng = random.Random(seed)
            touched = 0
            for n in self.inodes.values():
                if n.is_dir or not n.dirty:
                    continue
                pages = [p for p in sorted(n.dirty) if rng.random() < 0.5]
                if not pages:
                    continue
                touched += len(pages)
                if fail:
                    n.failed |= set(pages)
                    n.err_seq += 1
                else:
                    self._persist(n, pages, resize=False)
                n.dirty -= set(pages)
            self._note("writeback", f"{'failed' if fail else 'written'}(pages={touched})")
            return touched

    def crash(self, seed: int, subset: bool) -> dict:
        """Power loss. Called while frozen, after SIGKILL was sent: the dying
        process cannot exit while one of its requests is held here, so the
        crash is applied first, and the mount stays fenced (every request
        fails) until `unfence`, once the process is gone."""
        with self.cond:
            rng = random.Random(seed)
            summary = {"pages_kept": 0, "pages_lost": 0, "pages_torn": 0,
                       "dir_changes_kept": 0, "dir_changes_lost": 0}
            for n in self.inodes.values():
                if n.is_dir:
                    self._crash_dir(n, rng, subset, summary)
                else:
                    self._crash_file(n, rng, summary)
            # Only what the durable namespace reaches survives.
            reachable: dict[int, int] = {self.root.ino: 1}
            stack = [self.root]
            while stack:
                d = stack.pop()
                for ino in d.entries.values():
                    if ino not in self.inodes:
                        continue
                    first = ino not in reachable
                    reachable[ino] = reachable.get(ino, 0) + 1
                    if first and self.inodes[ino].is_dir:
                        stack.append(self.inodes[ino])
            for ino in list(self.inodes):
                if ino not in reachable:
                    del self.inodes[ino]
                else:
                    self.inodes[ino].nlink = reachable[ino]
                    self.inodes[ino].opens = 0
            # Entries to inodes that no longer exist cannot happen: an inode is
            # dropped only when unreachable. Guard anyway.
            for n in self.inodes.values():
                if n.is_dir:
                    n.entries = {k: v for k, v in n.entries.items() if v in self.inodes}
                    n.dentries = dict(n.entries)
            self.handles.clear()
            self.epoch += 1
            self.fenced = True
            self.frozen = False
            self.cond.notify_all()
            self.log.append((round(time.time(), 6), "crash", "", json.dumps(summary)))
            return summary

    def _crash_file(self, n: Inode, rng: random.Random, summary: dict):
        size = len(n.durable) if rng.random() < 0.5 else len(n.data)
        img = bytearray(n.durable[:size])
        if len(img) < size:
            img.extend(bytes(size - len(img)))
        for p in sorted(n.dirty):
            start = p * PAGE
            end = min(start + PAGE, size, len(n.data))
            if start >= end:
                continue
            r = rng.random()
            if r < 0.4:
                summary["pages_lost"] += 1
            elif r < 0.8:
                img[start:end] = n.data[start:end]
                summary["pages_kept"] += 1
            else:
                for s in range(start, end, SECTOR):
                    if rng.random() < 0.5:
                        e = min(s + SECTOR, end)
                        img[s:e] = n.data[s:e]
                summary["pages_torn"] += 1
        summary["pages_lost"] += len(n.failed)
        n.data = img
        n.durable = bytearray(img)
        n.dirty = set()
        n.failed = set()
        n.err_seq = 0

    def _crash_dir(self, d: Inode, rng: random.Random, subset: bool, summary: dict):
        ops = d.pending
        if subset:
            chosen = [op for op in ops if rng.random() < 0.5]
        else:
            chosen = ops[:rng.randint(0, len(ops))]
        entries = dict(d.dentries)
        for op in chosen:
            if op[0] == "link":
                entries[op[1]] = op[2]
            elif op[0] == "unlink":
                entries.pop(op[1], None)
            elif op[0] == "rename":
                if entries.get(op[1]) == op[3]:
                    del entries[op[1]]
                entries[op[2]] = op[3]
        summary["dir_changes_kept"] += len(chosen)
        summary["dir_changes_lost"] += len(ops) - len(chosen)
        d.entries = entries
        d.dentries = dict(entries)
        d.pending = []

    def unfence(self):
        with self.cond:
            self.fenced = False

    def stats(self) -> dict:
        with self.cond:
            return {"counts": dict(self.counts), "used": self._used(),
                    "inodes": len(self.inodes), "frozen": self.frozen,
                    "epoch": self.epoch,
                    "dirty_pages": sum(len(n.dirty) for n in self.inodes.values()),
                    "failed_pages": sum(len(n.failed) for n in self.inodes.values())}

    def dump_log(self, path: str):
        with self.cond:
            entries = list(self.log)
        with open(path, "w") as f:
            for e in entries:
                f.write("\t".join(str(x) for x in e) + "\n")


# ---------------------------------------------------------------------------
# FUSE adapter
# ---------------------------------------------------------------------------

def make_fuse_ops(model: ChaosModel):
    import mfusepy as fuse

    def guard(fn):
        # functools.wraps keeps the signature: mfusepy picks create()'s
        # calling convention from its parameter count.
        @functools.wraps(fn)
        def wrapped(*args, **kwargs):
            try:
                return fn(*args, **kwargs)
            except OSError as e:
                raise fuse.FuseOSError(e.errno) from None
        return wrapped

    class ChaosFuse(fuse.Operations):
        # Unlinked files stay readable through their open handles (vacuum
        # removes files snapshots still read), so operations with a handle
        # may come without a path.
        flag_nopath = True
        flag_nullpath_ok = True
        use_ns = True

        @guard
        def getattr(self, path, fh=None):
            return model.getattr(path, fh)

        @guard
        def readdir(self, path, fh):
            return model.readdir(path, fh)

        @guard
        def mkdir(self, path, mode):
            model.mkdir(path, mode)
            return 0

        @guard
        def rmdir(self, path):
            model.unlink(path, is_dir=True)
            return 0

        @guard
        def create(self, path, mode, flags):
            return model.create(path, mode, flags)

        @guard
        def open(self, path, flags):
            return model.open(path, flags)

        @guard
        def opendir(self, path):
            return model.open(path, os.O_RDONLY)

        def releasedir(self, path, fh):
            model.release(fh)
            return 0

        @guard
        def read(self, path, size, offset, fh):
            return model.read(path, size, offset, fh)

        @guard
        def write(self, path, data, offset, fh):
            return model.write(path, data, offset, fh)

        @guard
        def truncate(self, path, length, fh=None):
            model.truncate(path, length, fh)
            return 0

        @guard
        def fsync(self, path, datasync, fh):
            model.fsync(path, datasync, fh)
            return 0

        @guard
        def fsyncdir(self, path, datasync, fh):
            model.fsyncdir(path, fh)
            return 0

        @guard
        def rename(self, old, new):
            model.rename(old, new)
            return 0

        @guard
        def unlink(self, path):
            model.unlink(path)
            return 0

        def release(self, path, fh):
            model.release(fh)
            return 0

        def flush(self, path, fh):
            return 0

        def statfs(self, path):
            return model.statfs()

        def utimens(self, path, times=None):
            return 0

        def chmod(self, path, mode):
            return 0

        def chown(self, path, uid, gid):
            return 0

        def access(self, path, amode):
            return 0

    return ChaosFuse(), fuse


def serve_control(model: ChaosModel, sock_path: str):
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            for line in self.rfile:
                try:
                    req = json.loads(line)
                    reply = {"ok": True, "result": dispatch(req)}
                except Exception as e:  # reported to the orchestrator
                    reply = {"ok": False, "error": f"{type(e).__name__}: {e}"}
                self.wfile.write((json.dumps(reply) + "\n").encode())
                self.wfile.flush()

    def dispatch(req: dict):
        cmd = req["cmd"]
        if cmd == "ping":
            return "pong"
        if cmd == "faults":
            model.set_faults(req.get("set", {}), req.get("seed"))
            return None
        if cmd == "clear":
            model.set_faults({}, None)
            return None
        if cmd == "freeze":
            model.freeze()
            return None
        if cmd == "crash":
            return model.crash(req.get("seed", 0), bool(req.get("subset")))
        if cmd == "unfence":
            model.unfence()
            return None
        if cmd == "evict":
            return model.evict()
        if cmd == "writeback":
            return model.writeback(req.get("seed", 0), bool(req.get("fail")))
        if cmd == "stats":
            return model.stats()
        if cmd == "log":
            model.dump_log(req["path"])
            return None
        raise ValueError(f"unknown command {cmd}")

    if os.path.exists(sock_path):
        os.unlink(sock_path)
    server = socketserver.ThreadingUnixStreamServer(sock_path, Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mountpoint")
    ap.add_argument("--control", required=True, help="control socket path")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    model = ChaosModel(args.seed)
    serve_control(model, args.control)
    ops, fuse = make_fuse_ops(model)
    # direct_io: every read and write reaches the model, so the model is the
    # page cache. Zero timeouts: after a crash the kernel must not answer
    # from cached names or sizes. hard_remove: an unlinked open file keeps
    # working through its handle instead of being renamed to .fuse_hidden.
    fuse.FUSE(ops, args.mountpoint, foreground=True, nothreads=False,
              direct_io=True, hard_remove=True, attr_timeout=0,
              entry_timeout=0, negative_timeout=0, auto_unmount=True,
              fsname="chaosfs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
