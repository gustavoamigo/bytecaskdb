# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo

"""ByteCaskDB in one file: a reference implementation of the model.

The whole engine is two ideas:

1. **The data file is the only record.** Every write is an append of
   CRC-checked entries to the active data file. A batch is framed by a
   BULK_BEGIN and a BULK_END entry, so it is on disk whole or not at all.
   Opening a database replays the data files in sequence order.
2. **The key directory is an immutable tree in memory.** It maps each key to
   the location of its newest record (file, offset, sequence). A write builds
   a new tree that shares every untouched node with the old one, then
   publishes it. A snapshot is a reference to a tree, and costs nothing.

Writers take one lock; readers never do. A write is appended (and synced, if
asked) before its tree is published, so nothing becomes visible before it is
on disk.

The API is the one ``bytecaskdb.DB`` offers (bytecaskdb/ext.py)::

    with DB.open("my_db") as db:
        db[b"user:1"] = b"alice"
        with db.transaction() as txn:          # raises ConflictError on conflict
            txn[b"user:2"] = txn[b"user:1"]
        for key, value in db.prefix(b"user:"):
            ...

Files are the engine's V01 format (docs/file_format.md): the C++ engine opens
a database written here, and this opens one the engine wrote.

Left out, because none of it changes what a read returns: hint files, vacuum,
group commit, the buffer pool, preallocation, replication and resume(). After
an I/O error on a write, writes stop until the database is reopened.

What the engine guarantees and this does not:

- Exclusive access. There is no directory lock: two processes can open one
  database and corrupt it.
- Snapshots that outlive close(). Here reads after close() raise DbClosed.
- Flat latency. There is no group commit, so every sync write pays for its
  own fdatasync. The tree (persistent_tree.py) is unbalanced, and sorted
  inserts make it a list. Range deletes and range guards take time linear in
  the keys they cover, and opening reads every file whole.
- Fault testing. The engine's crash, chaos and fault-injection rigs have
  never run against this; its crash behaviour has unit tests only.

The engine's key directory is a B+ tree whose leaves hold no key bytes; this
tree stores the keys. It is for reading and testing, not for loads.
"""

from __future__ import annotations

import contextlib
import enum
import os
import secrets
import struct
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import NamedTuple

from checksum import crc32c
from persistent_tree import PersistentTree

MAX_KEY_BYTES = 4096
MAX_VALUE_BYTES = 4 * 1024 * 1024


# ── Data file entries ────────────────────────────────────────────────────────
#
#   sequence u64 | type u8 | key_size u16 | value_size u32 | key | value | crc32c u32
#
# all little-endian; the CRC covers everything before it.


class EntryType(enum.IntEnum):
    PUT = 1
    DELETE = 2
    BULK_BEGIN = 3
    BULK_END = 4
    RANGE_DELETE = 5  # key = range start, value = range end (exclusive)


_HEADER = struct.Struct("<QBHI")
_CRC = struct.Struct("<I")


class Entry(NamedTuple):
    sequence: int
    type: EntryType
    key: bytes = b""
    value: bytes = b""

    def encode(self) -> bytes:
        body = _HEADER.pack(self.sequence, self.type, len(self.key), len(self.value))
        body += self.key + self.value
        return body + _CRC.pack(crc32c(body))

    @classmethod
    def decode(cls, buf: bytes, offset: int, verify: bool = True) -> tuple[Entry, int] | None:
        """The entry at offset and the offset past it, or None if none parses there."""
        if offset + _HEADER.size > len(buf):
            return None
        sequence, raw_type, key_size, value_size = _HEADER.unpack_from(buf, offset)
        try:
            entry_type = EntryType(raw_type)
        except ValueError:  # 0 is never a type: zeros are unwritten space
            return None
        key_at = offset + _HEADER.size
        value_at = key_at + key_size
        crc_at = value_at + value_size
        end = crc_at + _CRC.size
        if end > len(buf):
            return None
        if verify and _CRC.unpack_from(buf, crc_at)[0] != crc32c(buf[offset:crc_at]):
            return None
        entry = cls(sequence, entry_type, bytes(buf[key_at:value_at]), bytes(buf[value_at:crc_at]))
        return entry, end


def scan_committed(buf: bytes) -> tuple[list[tuple[int, Entry]], int]:
    """The committed entries of a data file, with their offsets, and the
    offset where committed data ends.

    A batch counts only once its BULK_END is read: a crash that tore it leaves
    a BULK_BEGIN without one, and none of the batch survives.
    """
    committed: list[tuple[int, Entry]] = []
    batch: list[tuple[int, Entry]] | None = None
    end = offset = 0
    while (parsed := Entry.decode(buf, offset)) is not None:
        entry, next_offset = parsed
        match entry.type:
            case EntryType.BULK_BEGIN if batch is None:
                batch = [(offset, entry)]
            case EntryType.BULK_END if batch is not None:
                committed += [*batch, (offset, entry)]
                batch = None
                end = next_offset
            case EntryType.BULK_BEGIN | EntryType.BULK_END:
                break  # a marker out of place: damage
            case _ if batch is not None:
                batch.append((offset, entry))
            case _:
                committed.append((offset, entry))
                end = next_offset
        offset = next_offset
    return committed, end


# ── The key directory ────────────────────────────────────────────────────────
#
# An immutable sorted map from each key to the location of its newest record.
# Every write publishes a new version; a snapshot keeps one.


class Location(NamedTuple):
    file_id: int
    offset: int
    sequence: int


KeyDir = PersistentTree[bytes, Location]


def apply_entry(keydir: KeyDir, file_id: int, offset: int, entry: Entry) -> KeyDir:
    """The key directory after entry, written at (file_id, offset).

    Both the write path and recovery go through here: opening a database is
    replaying its writes.
    """
    match entry.type:
        case EntryType.PUT:
            return keydir.set(entry.key, Location(file_id, offset, entry.sequence))
        case EntryType.DELETE:
            return keydir.discard(entry.key)
        case EntryType.RANGE_DELETE:
            for key, _ in list(keydir.ascending(entry.key, entry.value)):
                keydir = keydir.remove(key)
            return keydir
        case EntryType.BULK_BEGIN | EntryType.BULK_END:
            return keydir


def _sequence(location: Location | None) -> int:
    return 0 if location is None else location.sequence


def _range_changed(now: KeyDir, then: KeyDir, start: bytes, stop: bytes) -> bool:
    """Whether any key in [start, stop) was written, or erased, between two versions.

    A key created and erased in between is absent from both, and does not count.
    """
    if any(loc.sequence != _sequence(then.get(k)) for k, loc in now.ascending(start, stop)):
        return True
    return any(k not in now for k, _ in then.ascending(start, stop))


# ── Plans: writes and the preconditions they commit under ────────────────────


class _Guard(enum.Enum):
    PRESENT = enum.auto()
    ABSENT = enum.auto()
    UNCHANGED = enum.auto()  # since the plan's snapshot


@dataclass
class _Plan:
    """Writes to apply atomically, if every guard holds.

    With a snapshot, every key the plan writes must also be unchanged since it.
    """

    snapshot: KeyDir | None = None
    writes: list[Entry] = field(default_factory=list)  # sequences assigned at commit
    guards: dict[bytes, _Guard] = field(default_factory=dict)
    range_guards: list[tuple[bytes, bytes]] = field(default_factory=list)

    def guard(self, key: bytes, guard: _Guard) -> None:
        if self.guards.setdefault(key, guard) is not guard:
            raise ValueError("contradictory guards on the same key")

    def holds(self, head: KeyDir) -> bool:
        snap = self.snapshot if self.snapshot is not None else KeyDir()
        for key, guard in self.guards.items():
            location = head.get(key)
            match guard:
                case _Guard.PRESENT if location is None:
                    return False
                case _Guard.ABSENT if location is not None:
                    return False
                case _Guard.UNCHANGED if _sequence(location) != _sequence(snap.get(key)):
                    return False
        if any(_range_changed(head, snap, start, stop) for start, stop in self.range_guards):
            return False
        if self.snapshot is None:
            return True
        for write in self.writes:
            if write.type is EntryType.RANGE_DELETE:
                if _range_changed(head, snap, write.key, write.value):
                    return False
            elif _sequence(head.get(write.key)) != _sequence(snap.get(write.key)):
                return False
        return True


# ── Public API: the one bytecaskdb.DB offers ─────────────────────────────────


class ByteCaskError(Exception):
    """Base for this module's errors."""


class ConflictError(ByteCaskError):
    """A transaction lost to a concurrent write; nothing was written."""


class DbClosed(ValueError):
    """Raised by every operation after close()."""


class DbDegraded(RuntimeError):
    """Raised by writes after a write failed with an I/O error. Reads go on;
    reopen the database to write again."""


@dataclass(frozen=True)
class CommitResult:
    sequence: int  # the highest sequence the write was given; 0 if it wrote nothing
    durable: bool  # fdatasync confirmed it before return


class _Reads:
    """Dict-like reads and ordered scans over one version of the key directory."""

    def _version(self) -> tuple[DB, KeyDir]:
        raise NotImplementedError

    def __getitem__(self, key: bytes) -> bytes:
        value = self.get(key)
        if value is None:
            raise KeyError(key)
        return value

    def __contains__(self, key: bytes) -> bool:
        return key in self._version()[1]

    def get(self, key: bytes, default: bytes | None = None, *,
            verify_checksums: bool = False) -> bytes | None:
        db, keydir = self._version()
        location = keydir.get(key)
        if location is None:
            return default
        return db._read_value(key, location, verify_checksums)

    def items(self, start: bytes = b"", *,
              verify_checksums: bool = False) -> Iterator[tuple[bytes, bytes]]:
        """(key, value) pairs from start, ascending. Values are read as the scan reaches them."""
        db, keydir = self._version()
        for key, location in keydir.ascending(start):
            yield key, db._read_value(key, location, verify_checksums)

    def keys(self, start: bytes = b"") -> Iterator[bytes]:
        for key, _ in self._version()[1].ascending(start):
            yield key

    def ritems(self, start: bytes = b"", *,
               verify_checksums: bool = False) -> Iterator[tuple[bytes, bytes]]:
        """(key, value) pairs at or below start, descending; from the last key if start is b""."""
        db, keydir = self._version()
        for key, location in keydir.descending(start or None):
            yield key, db._read_value(key, location, verify_checksums)

    def rkeys(self, start: bytes = b"") -> Iterator[bytes]:
        for key, _ in self._version()[1].descending(start or None):
            yield key

    def prefix(self, pfx: bytes, *,
               verify_checksums: bool = False) -> Iterator[tuple[bytes, bytes]]:
        for key, value in self.items(pfx, verify_checksums=verify_checksums):
            if not key.startswith(pfx):
                return
            yield key, value

    def rprefix(self, pfx: bytes, *,
                verify_checksums: bool = False) -> Iterator[tuple[bytes, bytes]]:
        upper = _prefix_upper(pfx)
        for key, value in self.ritems(upper, verify_checksums=verify_checksums):
            if upper and key >= upper:
                continue
            if not key.startswith(pfx):
                return
            yield key, value


def _prefix_upper(prefix: bytes) -> bytes:
    """The least key above every key starting with prefix; b"" if there is none."""
    stripped = prefix.rstrip(b"\xff")
    return stripped[:-1] + bytes([stripped[-1] + 1]) if stripped else b""


class Snapshot(_Reads):
    """A read-only view of the database at one instant: one key directory."""

    def __init__(self, db: DB, keydir: KeyDir) -> None:
        self._db = db
        self._keydir = keydir

    def _version(self) -> tuple[DB, KeyDir]:
        return self._db, self._keydir

    def __enter__(self) -> Snapshot:
        return self

    def __exit__(self, *exc: object) -> None:
        pass


class Batch:
    """Writes committed atomically when the ``with db.batch()`` block exits."""

    def __init__(self) -> None:
        self._plan = _Plan()
        self.result: CommitResult | None = None

    def __setitem__(self, key: bytes, value: bytes) -> None:
        self.put(key, value)

    def __delitem__(self, key: bytes) -> None:
        self.delete(key)

    def put(self, key: bytes, value: bytes) -> None:
        self._plan.writes.append(Entry(0, EntryType.PUT, bytes(key), bytes(value)))

    def delete(self, key: bytes) -> None:
        self._plan.writes.append(Entry(0, EntryType.DELETE, bytes(key)))

    def delete_range(self, from_key: bytes, to_key: bytes) -> None:
        self._plan.writes.append(
            Entry(0, EntryType.RANGE_DELETE, bytes(from_key), bytes(to_key)))


class Transaction(Batch, Snapshot):
    """A batch read from, and checked against, the snapshot taken when it began.

    Point reads see the transaction's own writes; scans read the snapshot only.
    It commits when the ``with db.transaction()`` block exits, and raises
    ConflictError instead if a key it writes, or guards, changed meanwhile.
    """

    def __init__(self, db: DB, keydir: KeyDir) -> None:
        Batch.__init__(self)
        Snapshot.__init__(self, db, keydir)
        self._plan.snapshot = keydir
        self._pending: dict[bytes, bytes | None] = {}  # None: deleted here
        self._deleted_ranges: list[tuple[bytes, bytes]] = []

    def _own_write(self, key: bytes) -> tuple[bool, bytes | None]:
        """Whether this transaction wrote key, and the value it left (None: deleted)."""
        if key in self._pending:
            return True, self._pending[key]
        if any(lo <= key < hi for lo, hi in self._deleted_ranges):
            return True, None
        return False, None

    def get(self, key: bytes, default: bytes | None = None, *,
            verify_checksums: bool = False) -> bytes | None:
        written, value = self._own_write(key)
        if written:
            return default if value is None else value
        return super().get(key, default, verify_checksums=verify_checksums)

    def __contains__(self, key: bytes) -> bool:
        written, value = self._own_write(key)
        return value is not None if written else super().__contains__(key)

    def put(self, key: bytes, value: bytes) -> None:
        super().put(key, value)
        self._pending[bytes(key)] = bytes(value)

    def delete(self, key: bytes) -> None:
        super().delete(key)
        self._pending[bytes(key)] = None

    def delete_range(self, from_key: bytes, to_key: bytes) -> None:
        super().delete_range(from_key, to_key)
        self._deleted_ranges.append((from_key, to_key))
        for key in self._pending:
            if from_key <= key < to_key:
                self._pending[key] = None

    def ensure_present(self, key: bytes) -> None:
        self._plan.guard(bytes(key), _Guard.PRESENT)

    def ensure_absent(self, key: bytes) -> None:
        self._plan.guard(bytes(key), _Guard.ABSENT)

    def ensure_unchanged(self, key: bytes) -> None:
        self._plan.guard(bytes(key), _Guard.UNCHANGED)

    def ensure_range_unchanged(self, from_key: bytes, to_key: bytes) -> None:
        self._plan.range_guards.append((bytes(from_key), bytes(to_key)))

    @property
    def has_snapshot(self) -> bool:
        return True


class _Commit:
    """The context manager behind db.batch() and db.transaction()."""

    def __init__(self, db: DB, batch: Batch, sync: bool) -> None:
        self._db = db
        self._batch = batch
        self._sync = sync

    def __enter__(self) -> Batch:
        return self._batch

    def __exit__(self, exc_type: type[BaseException] | None, *exc: object) -> None:
        if exc_type is not None:
            return  # the block raised: nothing is written
        self._batch.result = self._db._commit(self._batch._plan, self._sync)
        if self._batch.result is None:
            raise ConflictError("Transaction aborted: concurrent modification detected")


class DB(_Reads):
    """A database directory. Open it with DB.open."""

    def __init__(self, path: Path, max_file_bytes: int) -> None:
        self._dir = path
        self._max_file_bytes = max_file_bytes
        self._lock = threading.Lock()  # writers only
        self._fds: dict[int, int] = {}  # file_id -> descriptor
        self._keydir = KeyDir()  # the published version; replaced, never changed
        self._next_sequence = 1
        self._active_id = 0
        self._active_size = 0
        self._closed = False
        self._failure: str | None = None  # why writes stopped, after an I/O error

    @classmethod
    def open(cls, path: str | os.PathLike[str], *,
             max_file_bytes: int = 64 * 1024 * 1024) -> DB:
        """Opens or creates the database at path: replays every data file in
        sequence order, then starts a new active file."""
        db = cls(Path(path), max_file_bytes)
        db._dir.mkdir(parents=True, exist_ok=True)
        db._recover()
        db._start_active_file()
        return db

    def __enter__(self) -> DB:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ── Reads: lock-free, from whichever version is published ───────────────

    def _version(self) -> tuple[DB, KeyDir]:
        self._check_open()
        return self, self._keydir

    def snapshot(self) -> Snapshot:
        return Snapshot(*self._version())

    def _read_value(self, key: bytes, location: Location, verify: bool) -> bytes:
        """Reads the record at location: one read returns the value."""
        self._check_open()
        fd = self._fds[location.file_id]
        header = os.pread(fd, _HEADER.size, location.offset)
        _, _, key_size, value_size = _HEADER.unpack(header)
        size = _HEADER.size + key_size + value_size + _CRC.size
        parsed = Entry.decode(os.pread(fd, size, location.offset), 0, verify)
        if parsed is None or parsed[0].key != key or parsed[0].sequence != location.sequence:
            raise RuntimeError(f"corrupt record in file {location.file_id} at {location.offset}")
        return parsed[0].value

    # ── Writes: one at a time, under the lock ────────────────────────────────

    def __setitem__(self, key: bytes, value: bytes) -> None:
        self.put(key, value)

    def __delitem__(self, key: bytes) -> None:
        """Deletes key; nothing happens if it is absent."""
        self.delete(key)

    def put(self, key: bytes, value: bytes, *, sync: bool = True) -> CommitResult:
        plan = _Plan(writes=[Entry(0, EntryType.PUT, bytes(key), bytes(value))])
        result = self._commit(plan, sync)
        assert result is not None  # an unguarded plan cannot conflict
        return result

    def delete(self, key: bytes, *, sync: bool = True) -> CommitResult | None:
        """None if the key was absent: nothing is written."""
        plan = _Plan(writes=[Entry(0, EntryType.DELETE, bytes(key))])
        plan.guard(bytes(key), _Guard.PRESENT)
        return self._commit(plan, sync)

    def delete_range(self, from_key: bytes, to_key: bytes, *,
                     sync: bool = True) -> CommitResult:
        """Deletes [from_key, to_key) with one entry, however many keys it holds."""
        write = Entry(0, EntryType.RANGE_DELETE, bytes(from_key), bytes(to_key))
        _check_sizes([write])
        if from_key >= to_key:
            return CommitResult(0, True)
        result = self._commit(_Plan(writes=[write]), sync)
        assert result is not None
        return result

    def batch(self, *, sync: bool = True) -> _Commit:
        """An atomic batch of writes, committed when the block exits::

            with db.batch() as b:
                b[b"k1"] = b"v1"
                del b[b"k2"]
        """
        return _Commit(self, Batch(), sync)

    def transaction(self, *, sync: bool = True) -> _Commit:
        """A snapshot-backed transaction, committed when the block exits::

            with db.transaction() as txn:
                txn[b"stock"] = str(int(txn[b"stock"]) - 1).encode()
        """
        return _Commit(self, Transaction(*self._version()), sync)

    def _commit(self, plan: _Plan, sync: bool) -> CommitResult | None:
        """Applies every write in plan atomically, or none: None if a guard failed."""
        _check_sizes(plan.writes)
        with self._lock, self._stop_writes_on_io_error():
            self._check_open()
            if self._failure is not None:
                raise DbDegraded(f"writes stopped after an I/O error: {self._failure}")
            if not plan.holds(self._keydir):
                return None
            if not plan.writes:
                if sync:
                    os.fdatasync(self._fds[self._active_id])
                return CommitResult(0, True)

            # More than one write is framed, so the batch lands whole. Every
            # entry takes a sequence, the markers too.
            writes = plan.writes
            if len(writes) > 1:
                writes = [Entry(0, EntryType.BULK_BEGIN), *writes, Entry(0, EntryType.BULK_END)]
            entries = [w._replace(sequence=self._next_sequence + i) for i, w in enumerate(writes)]

            # 1. Append, in one write; sync if asked.
            encoded = [entry.encode() for entry in entries]
            offsets = []
            end = self._active_size
            for buf in encoded:
                offsets.append(end)
                end += len(buf)
            fd = self._fds[self._active_id]
            _pwrite_all(fd, b"".join(encoded), self._active_size)
            if sync:
                os.fdatasync(fd)
            self._active_size = end
            self._next_sequence += len(entries)

            # 2. Build the next version of the key directory, then publish it:
            #    durable before visible.
            keydir = self._keydir
            for entry, offset in zip(entries, offsets):
                keydir = apply_entry(keydir, self._active_id, offset, entry)
            self._keydir = keydir

            if self._active_size >= self._max_file_bytes:
                self._rotate()
            return CommitResult(entries[-1].sequence, sync)

    @contextlib.contextmanager
    def _stop_writes_on_io_error(self) -> Iterator[None]:
        """After a failed write or sync, no write is accepted again.

        A failed fdatasync may leave the pages it could not write clean in the
        page cache, so a later fdatasync reports success without writing them:
        nothing written before the failure can be trusted to reach the disk,
        however often it is synced. A failed write may also leave bytes past
        the end of the active file. Reopening scans the file and rewrites it
        (see _recover), so the next process starts from what is really there.
        """
        try:
            yield
        except OSError as e:
            self._failure = str(e)
            raise

    @property
    def is_degraded(self) -> bool:
        return self._failure is not None

    @property
    def degraded_reason(self) -> str:
        return self._failure or ""

    def close(self) -> None:
        """Makes every write durable and closes the files. Idempotent.

        Raises DbDegraded, after closing, if writes stopped on an I/O error:
        writes acknowledged without sync may then not be durable.
        """
        with self._lock:
            if self._closed:
                return
            self._closed = True
            try:
                if self._failure is None:
                    os.fdatasync(self._fds[self._active_id])
            finally:
                for fd in self._fds.values():
                    os.close(fd)
                self._fds.clear()
            if self._failure is not None:
                raise DbDegraded(f"closed after an I/O error: {self._failure}")

    def _check_open(self) -> None:
        if self._closed:
            raise DbClosed("database is closed")

    # ── Files ────────────────────────────────────────────────────────────────

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
        paths = sorted(self._dir.glob("*.data"))
        files = [_ScannedFile.read(file_id, path) for file_id, path in enumerate(paths, start=1)]
        newest = max((f.first_sequence for f in files), default=0)
        for f in sorted(files, key=lambda f: f.first_sequence):
            # Only the file written last can hold a write a crash tore;
            # damage anywhere else is damage to acknowledged data.
            if f.has_data_past_end and f.first_sequence != newest:
                raise RuntimeError(f"corrupt data file {f.path.name} past offset {f.end}")
            # Every other file was synced whole before the next was started.
            # The newest may hold bytes the last process never got to disk.
            if f.first_sequence == newest or f.end < f.size:
                _rewrite_durably(f.path, f.end)
            self._fds[f.file_id] = os.open(f.path, os.O_RDWR)
            for offset, entry in f.committed:
                self._keydir = apply_entry(self._keydir, f.file_id, offset, entry)
                self._next_sequence = max(self._next_sequence, entry.sequence + 1)


@dataclass
class _ScannedFile:
    file_id: int
    path: Path
    first_sequence: int  # read without its CRC; 0 if the file has no header
    committed: list[tuple[int, Entry]]
    end: int  # where committed data ends
    size: int
    has_data_past_end: bool  # anything but zeros after end

    @classmethod
    def read(cls, file_id: int, path: Path) -> _ScannedFile:
        buf = path.read_bytes()
        committed, end = scan_committed(buf)
        first = _HEADER.unpack_from(buf)[0] if len(buf) >= _HEADER.size else 0
        return cls(file_id, path, first, committed, end, len(buf), any(buf[end:]))


def _check_sizes(writes: list[Entry]) -> None:
    for write in writes:
        if len(write.key) > MAX_KEY_BYTES:
            raise ValueError(f"key size {len(write.key)} exceeds limit {MAX_KEY_BYTES}")
        limit = MAX_KEY_BYTES if write.type is EntryType.RANGE_DELETE else MAX_VALUE_BYTES
        if len(write.value) > limit:
            raise ValueError(f"value size {len(write.value)} exceeds limit {limit}")


def _pwrite_all(fd: int, data: bytes, offset: int) -> None:
    view = memoryview(data)
    while view:
        written = os.pwrite(fd, view, offset)
        view = view[written:]
        offset += written


def _rewrite_durably(path: Path, end: int) -> None:
    """Makes the first end bytes of path durable, and cuts what follows.

    What was read may be only in the page cache: the last process was killed
    before it synced, or a sync failed and left the pages clean without
    writing them, where no later sync would. Writing the bytes back dirties
    them again, so the sync writes them. The cut is synced too, or a power
    loss could bring back the torn tail in a file that is no longer the newest.
    """
    fd = os.open(path, os.O_RDWR)
    try:
        _pwrite_all(fd, os.pread(fd, end, 0), 0)
        os.ftruncate(fd, end)
        os.fsync(fd)
    finally:
        os.close(fd)


def _sync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
