# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo

"""ByteCaskDB in one file: a reference implementation of the model.

The whole engine is two ideas:

1. **The data file is the only record.** Every write is an append of
   CRC-checked entries to the active data file. A batch is framed by a
   BulkBegin and a BulkEnd entry, so it is on disk whole or not at all.
   Opening a database replays the data files in sequence order.
2. **The key directory is an immutable tree in memory.** It maps each key to
   the location of its newest record (file, offset, sequence). A write builds
   a new tree that shares every untouched node with the old one, then
   publishes it. A snapshot is a reference to a tree, and costs nothing.

Writers take one lock; readers never do. A write is appended (and synced, if
asked) before its tree is published, so nothing becomes visible before it is
on disk.

Files are the engine's V01 format (docs/file_format.md): the C++ engine opens
a database written here, and this opens one the engine wrote.

Left out, because none of it changes what a read returns: hint files, vacuum,
group commit, the buffer pool, preallocation, replication, degraded mode and
the directory lock. The tree here is a plain unbalanced BST that stores keys;
the engine's is a B+ tree whose leaves hold no key bytes. Sorted inserts make
it a list, so it is for reading and testing, not for loads.
"""

from __future__ import annotations

import enum
import os
import secrets
import struct
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, NamedTuple

# ---------------------------------------------------------------------------
# CRC-32C (Castagnoli), table-driven. The standard library has only CRC-32.
# ---------------------------------------------------------------------------


def _crc32c_table() -> list[int]:
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC_TABLE = _crc32c_table()


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Data file entries
#
#   sequence u64 | type u8 | key_size u16 | value_size u32 | key | value | crc32c u32
#
# all little-endian; the CRC covers everything before it.
# ---------------------------------------------------------------------------


class EntryType(enum.IntEnum):
    Put = 1
    Delete = 2
    BulkBegin = 3
    BulkEnd = 4
    RangeDel = 5  # key = range start, value = range end (exclusive)


_HEADER = struct.Struct("<QBHI")
_CRC = struct.Struct("<I")


class Entry(NamedTuple):
    sequence: int
    type: EntryType
    key: bytes
    value: bytes


def encode_entry(e: Entry) -> bytes:
    body = _HEADER.pack(e.sequence, e.type, len(e.key), len(e.value)) + e.key + e.value
    return body + _CRC.pack(crc32c(body))


def decode_entry(buf: bytes, off: int, verify: bool = True) -> tuple[Entry, int] | None:
    """The entry at off and the offset past it, or None if none parses there."""
    if off + _HEADER.size > len(buf):
        return None
    seq, typ, key_size, value_size = _HEADER.unpack_from(buf, off)
    if typ not in EntryType._value2member_map_:
        return None  # 0 is never a valid type: zeros are unwritten space
    key_at = off + _HEADER.size
    value_at = key_at + key_size
    end = value_at + value_size + _CRC.size
    if end > len(buf):
        return None
    if verify:
        (crc,) = _CRC.unpack_from(buf, end - _CRC.size)
        if crc != crc32c(buf[off : end - _CRC.size]):
            return None
    return Entry(seq, EntryType(typ), bytes(buf[key_at:value_at]), bytes(buf[value_at : end - _CRC.size])), end


def scan_committed(buf: bytes) -> tuple[list[tuple[int, Entry]], int]:
    """The committed entries of a data file, with their offsets, and the
    offset where committed data ends.

    A batch counts only once its BulkEnd is read: a crash that tore it leaves
    a BulkBegin without one, and none of the batch survives.
    """
    committed: list[tuple[int, Entry]] = []
    batch: list[tuple[int, Entry]] | None = None
    end = off = 0
    while (parsed := decode_entry(buf, off)) is not None:
        entry, nxt = parsed
        if entry.type == EntryType.BulkBegin:
            if batch is not None:
                break
            batch = [(off, entry)]
        elif entry.type == EntryType.BulkEnd:
            if batch is None:
                break
            committed += batch + [(off, entry)]
            batch = None
            end = nxt
        elif batch is not None:
            batch.append((off, entry))
        else:
            committed.append((off, entry))
            end = nxt
        off = nxt
    return committed, end


# ---------------------------------------------------------------------------
# The key directory: a persistent (immutable) binary search tree
#
# A node is never changed after it is built. Inserting or erasing copies the
# nodes on the path from the root to the change and shares everything else,
# so every older root still describes the tree as it was.
# ---------------------------------------------------------------------------


class Location(NamedTuple):
    file_id: int
    offset: int
    sequence: int


class Node:
    __slots__ = ("key", "loc", "left", "right")

    def __init__(self, key: bytes, loc: Location, left: Node | None, right: Node | None):
        self.key = key
        self.loc = loc
        self.left = left
        self.right = right


Path_ = list[tuple[Node, bool]]  # (node, went_left) from the root down


def _rebuild(path: Path_, child: Node | None) -> Node | None:
    """Copies the path bottom-up, hanging child where the path ended."""
    for node, went_left in reversed(path):
        if went_left:
            child = Node(node.key, node.loc, child, node.right)
        else:
            child = Node(node.key, node.loc, node.left, child)
    return child


def tree_get(root: Node | None, key: bytes) -> Location | None:
    node = root
    while node is not None:
        if key == node.key:
            return node.loc
        node = node.left if key < node.key else node.right
    return None


def tree_put(root: Node | None, key: bytes, loc: Location) -> Node | None:
    path: Path_ = []
    node = root
    while node is not None and key != node.key:
        went_left = key < node.key
        path.append((node, went_left))
        node = node.left if went_left else node.right
    if node is None:
        return _rebuild(path, Node(key, loc, None, None))
    return _rebuild(path, Node(key, loc, node.left, node.right))


def tree_erase(root: Node | None, key: bytes) -> Node | None:
    path: Path_ = []
    node = root
    while node is not None and key != node.key:
        went_left = key < node.key
        path.append((node, went_left))
        node = node.left if went_left else node.right
    if node is None:
        return root  # absent: the same tree
    if node.left is None:
        return _rebuild(path, node.right)
    if node.right is None:
        return _rebuild(path, node.left)
    # Two children: the successor (leftmost of the right subtree) takes its place.
    succ_path: Path_ = []
    succ = node.right
    while succ.left is not None:
        succ_path.append((succ, True))
        succ = succ.left
    right = _rebuild(succ_path, succ.right)
    return _rebuild(path, Node(succ.key, succ.loc, node.left, right))


def tree_ascend(root: Node | None, start: bytes = b"") -> Iterator[tuple[bytes, Location]]:
    """Keys >= start, ascending."""
    stack: list[Node] = []
    node = root
    while node is not None:  # the path to start; nodes below start are skipped
        if node.key >= start:
            stack.append(node)
            node = node.left
        else:
            node = node.right
    while stack:
        node = stack.pop()
        yield node.key, node.loc
        node = node.right
        while node is not None:
            stack.append(node)
            node = node.left


def tree_descend(root: Node | None, start: bytes | None = None) -> Iterator[tuple[bytes, Location]]:
    """Keys <= start, descending; every key when start is None."""
    stack: list[Node] = []
    node = root
    while node is not None:
        if start is None or node.key <= start:
            stack.append(node)
            node = node.right
        else:
            node = node.left
    while stack:
        node = stack.pop()
        yield node.key, node.loc
        node = node.left
        while node is not None:
            stack.append(node)
            node = node.right


def tree_range(root: Node | None, start: bytes, end: bytes) -> Iterator[tuple[bytes, Location]]:
    """Keys in [start, end)."""
    for key, loc in tree_ascend(root, start):
        if key >= end:
            return
        yield key, loc


def apply_entry(root: Node | None, file_id: int, offset: int, e: Entry) -> Node | None:
    """The key directory after entry e, written at (file_id, offset).

    Both the write path and recovery go through here: opening a database is
    replaying its writes.
    """
    if e.type == EntryType.Put:
        return tree_put(root, e.key, Location(file_id, offset, e.sequence))
    if e.type == EntryType.Delete:
        return tree_erase(root, e.key)
    if e.type == EntryType.RangeDel:
        for key in [k for k, _ in tree_range(root, e.key, e.value)]:
            root = tree_erase(root, key)
    return root  # BulkBegin / BulkEnd: structure only


# ---------------------------------------------------------------------------
# Public API — mirrors bytecaskdb._bytecaskdb (bytecaskdb-python)
# ---------------------------------------------------------------------------


class DbClosed(ValueError):
    """Raised by every operation after close()."""


@dataclass
class Options:
    max_file_bytes: int = 64 * 1024 * 1024
    max_key_bytes: int = 4096
    max_value_bytes: int = 4 * 1024 * 1024


@dataclass
class WriteOptions:
    sync: bool = True


@dataclass
class ReadOptions:
    verify_checksums: bool = True


@dataclass(frozen=True)
class CommitResult:
    sequence: int  # the highest sequence the write was given; 0 if it wrote nothing
    durable: bool  # fdatasync confirmed it before return


class Snapshot:
    """A read-only view of the database at one instant: a key directory root.

    Unlike the native binding's, it is not consumed by WritePlan; an immutable
    tree can be shared freely.
    """

    def __init__(self, db: DB, root: Node | None):
        self._db = db
        self._root = root

    def get(self, key: bytes, opts: ReadOptions | None = None) -> bytes | None:
        loc = tree_get(self._root, key)
        return None if loc is None else self._db._read_value(key, loc, opts)

    def contains_key(self, key: bytes, opts: ReadOptions | None = None) -> bool:
        return tree_get(self._root, key) is not None

    def iter_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        for key, loc in tree_ascend(self._root, from_key):
            yield key, self._db._read_value(key, loc, opts)

    def keys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        for key, _ in tree_ascend(self._root, from_key):
            yield key

    def riter_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        for key, loc in tree_descend(self._root, from_key or None):
            yield key, self._db._read_value(key, loc, opts)

    def rkeys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        for key, _ in tree_descend(self._root, from_key or None):
            yield key

    def __enter__(self) -> Snapshot:
        return self

    def __exit__(self, *args: object) -> None:
        pass


class WritePlan:
    """Writes applied atomically by DB.apply_batch, under preconditions.

    With a snapshot, every key the plan writes must be unchanged since that
    snapshot, or the plan conflicts.
    """

    def __init__(self, snapshot: Snapshot | None = None):
        self._snap = snapshot
        self._writes: list[tuple[EntryType, bytes, bytes]] = []
        self._guards: dict[bytes, str] = {}
        self._range_guards: list[tuple[bytes, bytes]] = []

    @property
    def has_snapshot(self) -> bool:
        return self._snap is not None

    def put(self, key: bytes, value: bytes) -> None:
        self._writes.append((EntryType.Put, bytes(key), bytes(value)))

    def del_(self, key: bytes) -> None:
        self._writes.append((EntryType.Delete, bytes(key), b""))

    def del_range(self, from_key: bytes, to_key: bytes) -> None:
        self._writes.append((EntryType.RangeDel, bytes(from_key), bytes(to_key)))

    def ensure_present(self, key: bytes) -> None:
        self._guard(key, "present")

    def ensure_absent(self, key: bytes) -> None:
        self._guard(key, "absent")

    def ensure_unchanged(self, key: bytes) -> None:
        if self._snap is None:
            raise ValueError("WritePlan::ensure_unchanged requires a snapshot")
        self._guard(key, "unchanged")

    def ensure_range_unchanged(self, from_key: bytes, to_key: bytes) -> None:
        if self._snap is None:
            raise ValueError("WritePlan::ensure_range_unchanged requires a snapshot")
        self._range_guards.append((bytes(from_key), bytes(to_key)))

    def _guard(self, key: bytes, kind: str) -> None:
        if self._guards.setdefault(bytes(key), kind) != kind:
            raise ValueError("WritePlan: contradictory guards on same key")


def _seq(loc: Location | None) -> int:
    return 0 if loc is None else loc.sequence


def _range_changed(now: Node | None, then: Node | None, start: bytes, end: bytes) -> bool:
    """Whether any key in [start, end) was written, or erased, between two trees.

    A key created and erased in between is absent from both, and does not count.
    """
    if any(loc.sequence != _seq(tree_get(then, k)) for k, loc in tree_range(now, start, end)):
        return True
    return any(tree_get(now, k) is None for k, _ in tree_range(then, start, end))


def _preconditions_hold(plan: WritePlan, root: Node | None) -> bool:
    snap = plan._snap._root if plan._snap is not None else None
    for key, kind in plan._guards.items():
        loc = tree_get(root, key)
        if kind == "present" and loc is None:
            return False
        if kind == "absent" and loc is not None:
            return False
        if kind == "unchanged" and _seq(loc) != _seq(tree_get(snap, key)):
            return False
    for start, end in plan._range_guards:
        if _range_changed(root, snap, start, end):
            return False
    if plan._snap is None:
        return True
    # Every key the plan writes must be as the snapshot saw it.
    for typ, key, value in plan._writes:
        if typ == EntryType.RangeDel:
            if _range_changed(root, snap, key, value):
                return False
        elif _seq(tree_get(root, key)) != _seq(tree_get(snap, key)):
            return False
    return True


class DB:
    def __init__(self, path: Path, opts: Options):
        """Use DB.open."""
        self._dir = path
        self._opts = opts
        self._lock = threading.Lock()  # writers only
        self._fds: dict[int, int] = {}  # file_id -> descriptor, for reads
        self._root: Node | None = None  # the published key directory
        self._next_seq = 1
        self._active_id = 0
        self._active_size = 0
        self._closed = False

    @staticmethod
    def open(path: str | os.PathLike[str], opts: Options | None = None) -> DB:
        """Opens or creates the database at path: replays every data file in
        sequence order, then starts a new active file."""
        db = DB(Path(path), opts or Options())
        db._dir.mkdir(parents=True, exist_ok=True)
        db._recover()
        db._start_active_file()
        return db

    # --- Reads: lock-free, from whatever root is published -----------------

    def snapshot(self) -> Snapshot:
        self._check_open()
        return Snapshot(self, self._root)

    def get(self, key: bytes, opts: ReadOptions | None = None) -> bytes | None:
        return self.snapshot().get(key, opts)

    def contains_key(self, key: bytes, opts: ReadOptions | None = None) -> bool:
        return self.snapshot().contains_key(key, opts)

    def iter_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        return self.snapshot().iter_from(from_key, opts)

    def keys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        return self.snapshot().keys_from(from_key, opts)

    def riter_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        return self.snapshot().riter_from(from_key, opts)

    def rkeys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        return self.snapshot().rkeys_from(from_key, opts)

    def _read_value(self, key: bytes, loc: Location, opts: ReadOptions | None) -> bytes:
        """Reads the record at loc: one read returns the value."""
        self._check_open()
        fd = self._fds[loc.file_id]
        header = os.pread(fd, _HEADER.size, loc.offset)
        _, _, key_size, value_size = _HEADER.unpack(header)
        size = _HEADER.size + key_size + value_size + _CRC.size
        verify = opts is None or opts.verify_checksums
        parsed = decode_entry(os.pread(fd, size, loc.offset), 0, verify)
        if parsed is None or parsed[0].key != key or parsed[0].sequence != loc.sequence:
            raise RuntimeError(f"corrupt record at file {loc.file_id} offset {loc.offset}")
        return parsed[0].value

    # --- Writes: one at a time, under the lock -------------------------------

    def put(self, key: bytes, value: bytes, opts: WriteOptions | None = None) -> CommitResult:
        plan = WritePlan()
        plan.put(key, value)
        result = self.apply_batch(plan, opts)
        assert result is not None
        return result

    def del_(self, key: bytes, opts: WriteOptions | None = None) -> CommitResult | None:
        """None if the key was absent: nothing is written."""
        plan = WritePlan()
        plan.ensure_present(key)
        plan.del_(key)
        return self.apply_batch(plan, opts)

    def del_range(self, from_key: bytes, to_key: bytes, opts: WriteOptions | None = None) -> CommitResult:
        """Deletes [from_key, to_key) with one entry, however many keys it holds."""
        self._check_sizes([(EntryType.RangeDel, from_key, to_key)])
        if from_key >= to_key:
            return CommitResult(0, True)
        plan = WritePlan()
        plan.del_range(from_key, to_key)
        result = self.apply_batch(plan, opts)
        assert result is not None
        return result

    def apply_batch(self, plan: WritePlan, opts: WriteOptions | None = None) -> CommitResult | None:
        """Applies every write in plan atomically, or none: None if a
        precondition failed."""
        sync = (opts or WriteOptions()).sync
        self._check_sizes(plan._writes)
        with self._lock:
            self._check_open()
            if not _preconditions_hold(plan, self._root):
                return None
            if not plan._writes:
                if sync:
                    os.fdatasync(self._fds[self._active_id])
                return CommitResult(0, True)

            # A batch of more than one write is framed, so it lands whole.
            framed = list(plan._writes)
            if len(framed) > 1:
                framed = [(EntryType.BulkBegin, b"", b""), *framed, (EntryType.BulkEnd, b"", b"")]
            entries = [Entry(self._next_seq + i, t, k, v) for i, (t, k, v) in enumerate(framed)]

            # 1. Append, in one write.
            encoded = [encode_entry(e) for e in entries]
            offsets = []
            at = self._active_size
            for buf in encoded:
                offsets.append(at)
                at += len(buf)
            fd = self._fds[self._active_id]
            _pwrite_all(fd, b"".join(encoded), self._active_size)
            if sync:
                os.fdatasync(fd)
            self._active_size = at
            self._next_seq += len(entries)

            # 2. Build the new key directory, then publish it: durable before visible.
            root = self._root
            for e, off in zip(entries, offsets):
                root = apply_entry(root, self._active_id, off, e)
            self._root = root

            if self._active_size >= self._opts.max_file_bytes:
                self._rotate()
            return CommitResult(entries[-1].sequence, sync)

    def close(self) -> None:
        """Makes every write durable and closes the files. Idempotent."""
        with self._lock:
            if self._closed:
                return
            self._closed = True
            os.fdatasync(self._fds[self._active_id])
            for fd in self._fds.values():
                os.close(fd)
            self._fds.clear()

    def __enter__(self) -> DB:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def _check_open(self) -> None:
        if self._closed:
            raise DbClosed("database is closed")

    def _check_sizes(self, writes: list[tuple[EntryType, bytes, bytes]]) -> None:
        for typ, key, value in writes:
            if len(key) > self._opts.max_key_bytes:
                raise ValueError(f"key size {len(key)} exceeds limit {self._opts.max_key_bytes}")
            limit = self._opts.max_key_bytes if typ == EntryType.RangeDel else self._opts.max_value_bytes
            if len(value) > limit:
                raise ValueError(f"value size {len(value)} exceeds limit {limit}")

    # --- Files ---------------------------------------------------------------

    def _start_active_file(self) -> None:
        stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime())
        path = self._dir / f"data_{stamp}_{secrets.token_hex(8)}_V01.data"
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o644)
        _sync_dir(self._dir)  # the file's name is durable before anything is written to it
        self._active_id = max(self._fds, default=0) + 1
        self._fds[self._active_id] = fd
        self._active_size = 0

    def _rotate(self) -> None:
        """Seals the active file, synced whole, and starts the next one."""
        os.fdatasync(self._fds[self._active_id])
        self._start_active_file()

    def _recover(self) -> None:
        """Rebuilds the key directory by replaying every data file.

        Files never share a sequence, so replaying them in order of their
        first sequence replays every write in the order it was made.
        """
        scanned = []
        for file_id, path in enumerate(sorted(self._dir.glob("*.data")), start=1):
            buf = path.read_bytes()
            committed, end = scan_committed(buf)
            first = _HEADER.unpack_from(buf)[0] if len(buf) >= _HEADER.size else 0
            scanned.append((first, file_id, path, committed, end, buf))

        newest = max((first for first, *_ in scanned), default=0)
        for first, file_id, path, committed, end, buf in sorted(scanned, key=lambda s: s[0]):
            if end < len(buf) and any(buf[end:]):
                # Only the file written last can hold a write a crash tore;
                # damage anywhere else is damage to acknowledged data.
                if first != newest:
                    raise RuntimeError(f"corrupt data file {path.name} past offset {end}")
            if end < len(buf):
                os.truncate(path, end)
            self._fds[file_id] = os.open(path, os.O_RDWR)
            for off, entry in committed:
                self._root = apply_entry(self._root, file_id, off, entry)
                self._next_seq = max(self._next_seq, entry.sequence + 1)


def _pwrite_all(fd: int, data: bytes, offset: int) -> None:
    view = memoryview(data)
    while view:
        n = os.pwrite(fd, view, offset)
        view = view[n:]
        offset += n


def _sync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
