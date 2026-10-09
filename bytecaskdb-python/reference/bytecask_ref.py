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
   publishes it as the head. A snapshot is a reference to a tree, and costs
   nothing.

Writers take one lock; readers never do. A write is appended (and synced, if
asked) before its tree is published, so nothing becomes visible before it is
on disk.

The interface is the engine's (README, API Reference; CONTRACT.md)::

    with DB.open("my_db") as db:
        db.put(b"a", b"1")
        plan = WritePlan()
        plan.put(b"b", b"2")
        plan.put(b"c", b"3")
        db.apply_batch(plan)

leaves one data file holding, as ``dump("my_db")`` prints it::

    data_..._V01.data
           0  seq 1  PUT         b'a' b'1'
          21  seq 2  BULK_BEGIN
          40  seq 3  PUT         b'b' b'2'
          61  seq 4  PUT         b'c' b'3'
          82  seq 5  BULK_END

and a key directory mapping a, b and c to (file 1, offset 0, seq 1),
(file 1, 40, 3) and (file 1, 61, 4). The Pythonic interface (``db[k]``,
``with db.transaction()``) is bytecaskdb/ext.py, which runs on this module as
on the engine: ``bytecaskdb.DB.open(path, backend=bytecask_ref)``.

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
import fcntl
import os
import secrets
import struct
import sys
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import NamedTuple

from checksum import crc32c
from persistent_tree import PersistentTree

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


class Placed(NamedTuple):
    """An entry and the offset it starts at in its data file."""

    offset: int
    entry: Entry


def scan_committed(buf: bytes) -> tuple[list[Placed], int]:
    """The committed entries of a data file, and the offset where they end.

    A file is a sequence of units; a batch is committed once its BULK_END is
    read, so one a crash tore (a BULK_BEGIN without its end) is not::

        file := unit*
        unit := entry | BULK_BEGIN entry* BULK_END

    The scan stops at the first entry that does not parse or does not fit.
    """
    committed: list[Placed] = []
    batch: list[Placed] | None = None  # the open batch, if any
    end = offset = 0
    while (parsed := Entry.decode(buf, offset)) is not None:
        entry, next_offset = parsed
        match entry.type:
            case EntryType.BULK_BEGIN if batch is None:
                batch = [Placed(offset, entry)]
            case EntryType.BULK_END if batch is not None:
                committed += [*batch, Placed(offset, entry)]
                batch = None
                end = next_offset
            case EntryType.BULK_BEGIN | EntryType.BULK_END:
                break  # a marker out of place: damage
            case _ if batch is not None:
                batch.append(Placed(offset, entry))
            case _:
                committed.append(Placed(offset, entry))
                end = next_offset
        offset = next_offset
    return committed, end


def dump(path: str | os.PathLike[str]) -> None:
    """Prints the committed entries of every data file in a database directory."""
    for file in sorted(Path(path).glob("*.data")):
        print(file.name)
        for offset, e in scan_committed(file.read_bytes())[0]:
            match e.type:
                case EntryType.PUT | EntryType.RANGE_DELETE:
                    fields = f"{e.key!r} {e.value!r}"
                case EntryType.DELETE:
                    fields = f"{e.key!r}"
                case EntryType.BULK_BEGIN | EntryType.BULK_END:
                    fields = ""
            print(f"{offset:8}  seq {e.sequence}  {e.type.name:<12}{fields}".rstrip())


# ── The key directory ────────────────────────────────────────────────────────
#
# An immutable sorted map from each key to the location of its newest record.
# Every write publishes a new version, the head; a snapshot keeps one.


class Location(NamedTuple):
    file_id: int
    offset: int
    sequence: int


KeyDir = PersistentTree[bytes, Location]


def apply_entry(keydir: KeyDir, file_id: int, placed: Placed) -> KeyDir:
    """The key directory after an entry written in file_id.

    Both the write path and recovery go through here: opening a database is
    replaying its writes.
    """
    offset, entry = placed
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


def _key_changed(head: KeyDir, snapshot: KeyDir, key: bytes) -> bool:
    """Whether key was written, or erased, since the snapshot.

    A key created and erased since is absent from both, and does not count.
    """
    return _sequence(head.get(key)) != _sequence(snapshot.get(key))


def _range_changed(head: KeyDir, snapshot: KeyDir, start: bytes, stop: bytes) -> bool:
    """Whether any key in [start, stop) was written, or erased, since the snapshot."""
    keys = {k for k, _ in head.ascending(start, stop)} | {k for k, _ in snapshot.ascending(start, stop)}
    return any(_key_changed(head, snapshot, k) for k in keys)


# ── Public interface: the engine's ───────────────────────────────────────────


class ByteCaskError(Exception):
    """Base of the engine's own errors: a state the database is in, not a
    failed system call (OSError) or a bad argument (ValueError)."""


class DbClosed(ByteCaskError, ValueError):
    """Raised by every operation after close()."""


class DbDegraded(ByteCaskError, RuntimeError):
    """Raised by writes after a write failed with an I/O error. Reads go on;
    reopen the database to write again."""


# Hard ceilings. Options above them are refused at open. A value is limited
# by the engine's packed in-memory entry, not the u32 on disk; one write (its
# entries and batch markers) and the file size are capped so that every entry
# starts below 2^32 bytes into its file.
MAX_KEY_BYTES = 65535
MAX_VALUE_BYTES = (1 << 28) - 1
MAX_FILE_BYTES = 3 << 30
MAX_BATCH_BYTES = 1 << 30


def _check_range(from_key: bytes, to_key: bytes) -> None:
    """A range [from, to) must hold a possible key. from >= to is refused, not
    treated as nothing to do: it is almost always swapped bounds."""
    if from_key >= to_key:
        raise ValueError("range [from, to) is empty: from must sort before to")


@dataclass
class Options:
    max_file_bytes: int = 64 * 1024 * 1024
    max_key_bytes: int = 4096
    max_value_bytes: int = 4 * 1024 * 1024

    def _check(self) -> None:
        for name, ceiling in (("max_file_bytes", MAX_FILE_BYTES),
                              ("max_key_bytes", MAX_KEY_BYTES),
                              ("max_value_bytes", MAX_VALUE_BYTES)):
            if getattr(self, name) > ceiling:
                raise ValueError(f"{name} = {getattr(self, name)} exceeds the hard ceiling of {ceiling}")


@dataclass
class WriteOptions:
    sync: bool = True
    solo: bool = False  # the engine's group-commit bypass; there is no group commit here


@dataclass
class ReadOptions:
    verify_checksums: bool = True


@dataclass(frozen=True)
class CommitResult:
    sequence: int  # the highest sequence the write was given; 0 if it wrote nothing
    durable: bool  # fdatasync confirmed it before return


class Snapshot:
    """A read-only view of the database at one instant: one key directory.

    WritePlan(snapshot) consumes it, as in the engine; it cannot be read after.
    """

    def __init__(self, db: DB, keydir: KeyDir) -> None:
        self._db = db
        self._keydir: KeyDir | None = keydir

    def _view(self) -> KeyDir:
        if self._keydir is None:
            raise RuntimeError("Snapshot already consumed by WritePlan")
        return self._keydir

    def _consume(self) -> KeyDir:
        keydir = self._view()
        self._keydir = None
        return keydir

    def get(self, key: bytes, opts: ReadOptions | None = None) -> bytes | None:
        location = self._view().get(key)
        return None if location is None else self._db._read_value(key, location, opts)

    def contains_key(self, key: bytes, opts: ReadOptions | None = None) -> bool:
        return key in self._view()

    def iter_from(self, from_key: bytes = b"",
                  opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        """(key, value) pairs from from_key, ascending. Values are read as the scan reaches them."""
        for key, location in self._view().ascending(from_key):
            yield key, self._db._read_value(key, location, opts)

    def keys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        for key, _ in self._view().ascending(from_key):
            yield key

    def riter_from(self, from_key: bytes = b"",
                   opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        """(key, value) pairs at or below from_key, descending; from the last key if it is b""."""
        for key, location in self._view().descending(from_key or None):
            yield key, self._db._read_value(key, location, opts)

    def rkeys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        for key, _ in self._view().descending(from_key or None):
            yield key

    def __enter__(self) -> Snapshot:
        return self

    def __exit__(self, *exc: object) -> None:
        pass


class _Guard(enum.Enum):
    PRESENT = enum.auto()
    ABSENT = enum.auto()
    UNCHANGED = enum.auto()  # since the plan's snapshot


class WritePlan:
    """Writes that DB.apply_batch applies atomically, if every guard holds.

    With a snapshot, every key the plan writes must also be unchanged since it.
    """

    def __init__(self, snapshot: Snapshot | None = None) -> None:
        self._snapshot = snapshot._consume() if snapshot is not None else None
        self._writes: list[Entry] = []  # sequences assigned at commit
        self._guards: dict[bytes, _Guard] = {}
        self._range_guards: list[tuple[bytes, bytes]] = []
        self._applied = False

    @property
    def has_snapshot(self) -> bool:
        return self._snapshot is not None

    def put(self, key: bytes, value: bytes) -> None:
        self._add(Entry(0, EntryType.PUT, bytes(key), bytes(value)))

    def del_(self, key: bytes) -> None:
        self._add(Entry(0, EntryType.DELETE, bytes(key)))

    def del_range(self, from_key: bytes, to_key: bytes) -> None:
        _check_range(from_key, to_key)
        self._add(Entry(0, EntryType.RANGE_DELETE, bytes(from_key), bytes(to_key)))

    def ensure_present(self, key: bytes) -> None:
        self._guard(key, _Guard.PRESENT)

    def ensure_absent(self, key: bytes) -> None:
        self._guard(key, _Guard.ABSENT)

    def ensure_unchanged(self, key: bytes) -> None:
        if self._snapshot is None:
            raise ValueError("WritePlan::ensure_unchanged requires a snapshot")
        self._guard(key, _Guard.UNCHANGED)

    def ensure_range_unchanged(self, from_key: bytes, to_key: bytes) -> None:
        _check_range(from_key, to_key)
        if self._snapshot is None:
            raise ValueError("WritePlan::ensure_range_unchanged requires a snapshot")
        self._check_not_applied()
        self._range_guards.append((bytes(from_key), bytes(to_key)))

    def _add(self, write: Entry) -> None:
        self._check_not_applied()
        self._writes.append(write)

    def _guard(self, key: bytes, guard: _Guard) -> None:
        self._check_not_applied()
        if self._guards.setdefault(bytes(key), guard) is not guard:
            raise ValueError("WritePlan: contradictory guards on the same key")

    def _check_not_applied(self) -> None:
        if self._applied:
            raise RuntimeError("WritePlan already applied")

    def _holds(self, head: KeyDir) -> bool:
        guards = self._guards.items()
        if any(key not in head for key, g in guards if g is _Guard.PRESENT):
            return False
        if any(key in head for key, g in guards if g is _Guard.ABSENT):
            return False
        if self._snapshot is None:
            return True  # nothing to compare with: no other guard is possible
        unchanged_keys = [key for key, g in guards if g is _Guard.UNCHANGED]
        unchanged_ranges = list(self._range_guards)
        for write in self._writes:
            if write.type is EntryType.RANGE_DELETE:
                unchanged_ranges.append((write.key, write.value))
            else:
                unchanged_keys.append(write.key)
        return (not any(_key_changed(head, self._snapshot, k) for k in unchanged_keys)
                and not any(_range_changed(head, self._snapshot, a, b) for a, b in unchanged_ranges))


class DB:
    """A database directory. Open it with DB.open."""

    def __init__(self, path: Path, opts: Options) -> None:
        self._dir = path
        self._opts = opts
        self._lock = threading.Lock()  # writers only
        self._fds: dict[int, int] = {}  # file_id -> descriptor
        self._head = KeyDir()  # the published key directory; replaced, never changed
        self._next_sequence = 1
        self._active_id = 0
        self._active_size = 0
        self._closed = False
        self._failure: str | None = None  # why writes stopped, after an I/O error

    @classmethod
    def open(cls, path: str | os.PathLike[str], opts: Options | None = None) -> DB:
        """Opens or creates the database at path: replays every data file in
        sequence order, then starts a new active file."""
        opts = opts or Options()
        opts._check()
        db = cls(Path(path), opts)
        db._dir.mkdir(parents=True, exist_ok=True)
        db._recover()
        db._start_active_file()
        return db

    def __enter__(self) -> DB:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ── Reads: lock-free, from the head as it is when they start ─────────────

    def snapshot(self) -> Snapshot:
        self._check_open()
        return Snapshot(self, self._head)

    def get(self, key: bytes, opts: ReadOptions | None = None) -> bytes | None:
        return self.snapshot().get(key, opts)

    def contains_key(self, key: bytes, opts: ReadOptions | None = None) -> bool:
        return self.snapshot().contains_key(key, opts)

    def iter_from(self, from_key: bytes = b"",
                  opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        return self.snapshot().iter_from(from_key, opts)

    def keys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        return self.snapshot().keys_from(from_key, opts)

    def riter_from(self, from_key: bytes = b"",
                   opts: ReadOptions | None = None) -> Iterator[tuple[bytes, bytes]]:
        return self.snapshot().riter_from(from_key, opts)

    def rkeys_from(self, from_key: bytes = b"", opts: ReadOptions | None = None) -> Iterator[bytes]:
        return self.snapshot().rkeys_from(from_key, opts)

    def _read_value(self, key: bytes, location: Location, opts: ReadOptions | None) -> bytes:
        """Reads the record at location: its header for its size, then the record."""
        self._check_open()
        fd = self._fds[location.file_id]
        _, _, key_size, value_size = _HEADER.unpack(os.pread(fd, _HEADER.size, location.offset))
        size = _HEADER.size + key_size + value_size + _CRC.size
        verify = opts is None or opts.verify_checksums
        parsed = Entry.decode(os.pread(fd, size, location.offset), 0, verify)
        if parsed is None or parsed[0].key != key or parsed[0].sequence != location.sequence:
            raise RuntimeError(f"corrupt record in file {location.file_id} at {location.offset}")
        return parsed[0].value

    # ── Writes: one at a time, under the lock ────────────────────────────────

    def put(self, key: bytes, value: bytes, opts: WriteOptions | None = None) -> CommitResult:
        plan = WritePlan()
        plan.put(key, value)
        result = self.apply_batch(plan, opts)
        assert result is not None  # an unguarded plan cannot conflict
        return result

    def del_(self, key: bytes, opts: WriteOptions | None = None) -> CommitResult | None:
        """None if the key was absent: nothing is written."""
        plan = WritePlan()
        plan.ensure_present(key)
        plan.del_(key)
        return self.apply_batch(plan, opts)

    def del_range(self, from_key: bytes, to_key: bytes,
                  opts: WriteOptions | None = None) -> CommitResult:
        """Deletes [from_key, to_key) with one entry, however many keys it holds."""
        self._check_sizes([Entry(0, EntryType.RANGE_DELETE, from_key, to_key)])
        plan = WritePlan()
        plan.del_range(from_key, to_key)
        result = self.apply_batch(plan, opts)
        assert result is not None
        return result

    def apply_batch(self, plan: WritePlan, opts: WriteOptions | None = None) -> CommitResult | None:
        """Applies every write in plan atomically, or none: None if a guard failed.

        Check, frame, append, sync, publish.
        """
        plan._check_not_applied()
        plan._applied = True
        sync = (opts or WriteOptions()).sync
        self._check_sizes(plan._writes)
        with self._lock, self._stop_writes_on_io_error():
            self._check_open()
            if self._failure is not None:
                raise DbDegraded(f"writes stopped after an I/O error: {self._failure}")

            # 1. Check the guards against the head.
            if not plan._holds(self._head):
                return None
            if not plan._writes:
                if sync:
                    _datasync(self._fds[self._active_id])
                return CommitResult(0, True)

            # 2. Frame: more than one write is wrapped in markers, so the batch
            #    lands whole. Every entry takes a sequence, the markers too.
            writes = plan._writes
            if len(writes) > 1:
                writes = [Entry(0, EntryType.BULK_BEGIN), *writes, Entry(0, EntryType.BULK_END)]
            entries = [w._replace(sequence=self._next_sequence + i) for i, w in enumerate(writes)]

            # 3. Append, in one write.
            placed = self._append(entries)

            # 4. Sync, if asked.
            if sync:
                _datasync(self._fds[self._active_id])

            # 5. Publish the next key directory: durable before visible.
            head = self._head
            for p in placed:
                head = apply_entry(head, self._active_id, p)
            self._head = head

            if self._active_size >= self._opts.max_file_bytes:
                self._rotate()
            return CommitResult(entries[-1].sequence, sync)

    def _append(self, entries: list[Entry]) -> list[Placed]:
        encoded = [entry.encode() for entry in entries]
        placed = []
        offset = self._active_size
        for entry, buf in zip(entries, encoded):
            placed.append(Placed(offset, entry))
            offset += len(buf)
        _pwrite_all(self._fds[self._active_id], b"".join(encoded), self._active_size)
        self._active_size = offset
        self._next_sequence = entries[-1].sequence + 1
        return placed

    def _check_sizes(self, writes: list[Entry]) -> None:
        for write in writes:
            if len(write.key) > self._opts.max_key_bytes:
                raise ValueError(f"key size {len(write.key)} exceeds limit {self._opts.max_key_bytes}")
            limit = (self._opts.max_key_bytes if write.type is EntryType.RANGE_DELETE
                     else self._opts.max_value_bytes)
            if len(write.value) > limit:
                raise ValueError(f"value size {len(write.value)} exceeds limit {limit}")
        framed = sum(_HEADER.size + len(w.key) + len(w.value) + _CRC.size for w in writes)
        if len(writes) > 1:
            framed += 2 * (_HEADER.size + _CRC.size)
        if framed > MAX_BATCH_BYTES:
            raise ValueError(f"write plan of {framed} bytes exceeds the limit of {MAX_BATCH_BYTES} bytes per write")

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
                    _datasync(self._fds[self._active_id])
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
        _datasync(self._fds[self._active_id])
        self._start_active_file()

    def _recover(self) -> None:
        """Rebuilds the key directory by replaying every data file.

        Files never share a sequence, so replaying them in order of their
        first sequence replays every write in the order it was made.
        """
        paths = sorted(self._dir.glob("*.data"))
        files = [_ScannedFile.read(file_id, path) for file_id, path in enumerate(paths, start=1)]
        newest = max((f.first_sequence for f in files), default=0)
        # A crash tears only the file being written, so two files with data
        # after their committed entries are damage.
        torn = [f.path.name for f in files if f.has_data_past_end]
        if len(torn) > 1:
            raise RuntimeError(f"corrupt data files {', '.join(torn)}: more than one holds data past its end")
        for f in sorted(files, key=lambda f: f.first_sequence):
            # Every file but the newest was synced whole before the next one
            # was started. After its committed entries, a file may hold:
            #   - nothing, or zeros (unwritten space): cut;
            #   - a write a crash tore, in the newest file: cut. A file with
            #     no first sequence may be the newest, its first page lost at
            #     a power cut and a later one kept, so it is cut too;
            #   - anything else: damage to acknowledged data, refused.
            is_newest = f.first_sequence == newest
            has_no_sequence = f.first_sequence == 0
            if f.has_data_past_end and not (is_newest or has_no_sequence):
                raise RuntimeError(f"corrupt data file {f.path.name} past offset {f.end}")
            # The newest may also hold bytes the last process never got to disk.
            if is_newest or f.end < f.size:
                _rewrite_durably(f.path, f.end)
            self._fds[f.file_id] = os.open(f.path, os.O_RDWR)
            for p in f.committed:
                self._head = apply_entry(self._head, f.file_id, p)
                self._next_sequence = max(self._next_sequence, p.entry.sequence + 1)


@dataclass
class _ScannedFile:
    file_id: int
    path: Path
    first_sequence: int  # read without its CRC; 0 if the file is shorter than a header
    committed: list[Placed]
    end: int  # where committed data ends
    size: int
    has_data_past_end: bool  # anything but zeros after end

    @classmethod
    def read(cls, file_id: int, path: Path) -> _ScannedFile:
        buf = path.read_bytes()
        committed, end = scan_committed(buf)
        first = _HEADER.unpack_from(buf)[0] if len(buf) >= _HEADER.size else 0
        return cls(file_id, path, first, committed, end, len(buf), any(buf[end:]))


if sys.platform == "darwin":
    # macOS has no fdatasync, and its fsync does not flush the drive's cache.
    def _datasync(fd: int) -> None:
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)

    _fullsync = _datasync
else:
    def _datasync(fd: int) -> None:
        os.fdatasync(fd)

    def _fullsync(fd: int) -> None:
        """Syncs data and metadata: a truncate changes the file's size."""
        os.fsync(fd)


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
        _fullsync(fd)
    finally:
        os.close(fd)


def _sync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
