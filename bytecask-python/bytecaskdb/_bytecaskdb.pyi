# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Type stubs for the _bytecaskdb C extension module (nanobind).

from __future__ import annotations

import os
from typing import Iterator, overload

class DbDegraded(RuntimeError):
    """Raised by write operations when the engine is in a degraded state.

    Reads remain available. Call ``DB.resume()`` to attempt recovery.
    """

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

class Options:
    """Configuration for ``DB.open()``."""

    max_file_bytes: int
    """Active file rotation threshold in bytes (default 64 MiB)."""

    recovery_threads: int
    """Number of threads for parallel hint-file replay at open (default 4)."""

    fail_recovery_on_crc_errors: bool
    """If True (default), any CRC error during recovery raises.
    If False, corrupt entries are skipped and a warning is printed."""

    def __init__(self) -> None: ...

class WriteOptions:
    """Per-write options passed to ``put``, ``del_``, ``apply_batch``, etc."""

    sync: bool
    """If True (default), call fdatasync after write."""

    solo: bool
    """If True, bypass group commit and route to the solo writer."""

    def __init__(self) -> None: ...

class ReadOptions:
    """Per-read options passed to ``get``, ``iter_from``, etc."""

    verify_checksums: bool
    """If True, CRC-verify each value read from disk (default False)."""

    def __init__(self) -> None: ...

class VacuumOptions:
    """Options for ``DB.vacuum()``."""

    fragmentation_threshold: float
    """Minimum fragmentation ratio [0.0, 1.0] for a file to be eligible."""

    absorb_threshold: int
    """Max live bytes for absorption into active file (default 1 MiB)."""

    def __init__(self) -> None: ...

# ---------------------------------------------------------------------------
# Iterators
# ---------------------------------------------------------------------------

class EntryIterator:
    """Forward iterator yielding ``(key, value)`` tuples in ascending key order."""

    def __iter__(self) -> EntryIterator: ...
    def __next__(self) -> tuple[bytes, bytes]: ...

class KeyIterator:
    """Forward iterator yielding keys in ascending order. No disk I/O."""

    def __iter__(self) -> KeyIterator: ...
    def __next__(self) -> bytes: ...

class ReverseEntryIterator:
    """Reverse iterator yielding ``(key, value)`` tuples in descending key order."""

    def __iter__(self) -> ReverseEntryIterator: ...
    def __next__(self) -> tuple[bytes, bytes]: ...

class ReverseKeyIterator:
    """Reverse iterator yielding keys in descending order. No disk I/O."""

    def __iter__(self) -> ReverseKeyIterator: ...
    def __next__(self) -> bytes: ...

# ---------------------------------------------------------------------------
# Snapshot
# ---------------------------------------------------------------------------

class Snapshot:
    """Frozen, read-only view of the database at a point in time.

    Holds open any referenced data files until released. Supports use
    as a context manager (``with db.snapshot() as snap:``).
    """

    def get(self, key: bytes) -> bytes | None:
        """Return the value for *key*, or ``None`` if not found."""
        ...

    def contains_key(self, key: bytes) -> bool:
        """Return ``True`` if *key* exists. Pure in-memory, no disk I/O."""
        ...

    def iter_from(self, from_key: bytes = b"") -> EntryIterator:
        """Return an iterator over ``(key, value)`` pairs in ascending order,
        starting at the first key >= *from_key*."""
        ...

    def keys_from(self, from_key: bytes = b"") -> KeyIterator:
        """Return an iterator over keys in ascending order. No disk I/O."""
        ...

    def riter_from(self, from_key: bytes = b"") -> ReverseEntryIterator:
        """Return an iterator over ``(key, value)`` pairs in descending order,
        starting at the last key <= *from_key*."""
        ...

    def rkeys_from(self, from_key: bytes = b"") -> ReverseKeyIterator:
        """Return an iterator over keys in descending order. No disk I/O."""
        ...

    def __enter__(self) -> Snapshot: ...
    def __exit__(self, *args: object) -> None: ...

# ---------------------------------------------------------------------------
# WritePlan
# ---------------------------------------------------------------------------

class WritePlan:
    """Atomic write plan for ``DB.apply_batch()``.

    Construct without arguments for a simple unconditional batch.
    Construct with a ``Snapshot`` to enable ``ensure_unchanged`` /
    ``ensure_range_unchanged`` and automatic write-write conflict detection.

    After apply, all further calls raise ``RuntimeError``.
    """

    @overload
    def __init__(self) -> None: ...
    @overload
    def __init__(self, snapshot: Snapshot) -> None: ...
    def __init__(self, snapshot: Snapshot | None = None) -> None: ...

    def put(self, key: bytes, value: bytes) -> None:
        """Stage a key-value write."""
        ...

    def del_(self, key: bytes) -> None:
        """Stage a key deletion."""
        ...

    def del_range(self, from_key: bytes, to_key: bytes) -> None:
        """Stage a range deletion: all keys in ``[from_key, to_key)``."""
        ...

    def ensure_present(self, key: bytes) -> None:
        """Guard: *key* must exist at commit time."""
        ...

    def ensure_absent(self, key: bytes) -> None:
        """Guard: *key* must be absent at commit time."""
        ...

    def ensure_unchanged(self, key: bytes) -> None:
        """Guard: *key* must not have changed since the plan's snapshot.

        Raises ``ValueError`` if the plan has no snapshot.
        """
        ...

    def ensure_range_unchanged(self, from_key: bytes, to_key: bytes) -> None:
        """Guard: no key in ``[from_key, to_key)`` changed since the snapshot.

        Raises ``ValueError`` if the plan has no snapshot.
        """
        ...

    @property
    def has_snapshot(self) -> bool:
        """``True`` if this plan was constructed with a snapshot."""
        ...

# ---------------------------------------------------------------------------
# DB
# ---------------------------------------------------------------------------

class DB:
    """ByteCaskDB database handle.

    Open or create a database with ``DB.open(path)``. Non-copyable
    and non-moveable — one handle per database directory.
    """

    @staticmethod
    def open(
        path: str | os.PathLike[str],
        opts: Options | None = None,
    ) -> DB:
        """Open or create a database at *path*."""
        ...

    def get(
        self, key: bytes, opts: ReadOptions | None = None
    ) -> bytes | None:
        """Return the value for *key*, or ``None`` if not found."""
        ...

    def put(
        self,
        key: bytes,
        value: bytes,
        opts: WriteOptions | None = None,
    ) -> None:
        """Write *key* → *value*. Overwrites any existing value."""
        ...

    def del_(
        self, key: bytes, opts: WriteOptions | None = None
    ) -> bool:
        """Delete *key*. Return ``True`` if it existed."""
        ...

    def del_range(
        self,
        from_key: bytes,
        to_key: bytes,
        opts: WriteOptions | None = None,
    ) -> None:
        """Delete all keys in ``[from_key, to_key)`` with a single disk append."""
        ...

    def contains_key(self, key: bytes) -> bool:
        """Return ``True`` if *key* exists. Pure in-memory, no disk I/O."""
        ...

    def apply_batch(
        self, plan: WritePlan, opts: WriteOptions | None = None
    ) -> bool:
        """Atomically apply *plan*. Consumes *plan*.

        Return ``True`` if committed, ``False`` on conflict.
        """
        ...

    def snapshot(self) -> Snapshot:
        """Return a frozen, read-only view of the database at this instant."""
        ...

    def iter_from(
        self,
        from_key: bytes = b"",
        opts: ReadOptions | None = None,
    ) -> EntryIterator:
        """Return an iterator over ``(key, value)`` pairs in ascending order."""
        ...

    def keys_from(
        self,
        from_key: bytes = b"",
        opts: ReadOptions | None = None,
    ) -> KeyIterator:
        """Return an iterator over keys in ascending order. No disk I/O."""
        ...

    def riter_from(
        self,
        from_key: bytes = b"",
        opts: ReadOptions | None = None,
    ) -> ReverseEntryIterator:
        """Return an iterator over ``(key, value)`` pairs in descending order."""
        ...

    def rkeys_from(
        self,
        from_key: bytes = b"",
        opts: ReadOptions | None = None,
    ) -> ReverseKeyIterator:
        """Return an iterator over keys in descending order. No disk I/O."""
        ...

    def vacuum(self, opts: VacuumOptions | None = None) -> bool:
        """Run one vacuum pass. Return ``True`` if a file was vacuumed."""
        ...

    def resume(self) -> None:
        """Attempt recovery from a degraded state.

        Scans the active file, truncates garbage, replays valid entries,
        and opens a fresh active file. No-op if not degraded.
        """
        ...

    @property
    def is_degraded(self) -> bool:
        """``True`` if the engine is in a degraded state."""
        ...

    @property
    def degraded_reason(self) -> str:
        """Diagnostic string describing why the engine degraded, or empty."""
        ...
