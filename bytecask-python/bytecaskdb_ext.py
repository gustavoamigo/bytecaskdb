"""Pythonic wrapper around the bytecaskdb C extension.

Adds:
  - Dict-like access  (db[key], db[key]=v, del db[key], key in db)
  - Named iteration   (db.items(), db.keys(), db.ritems(), db.rkeys(),
                       db.prefix(), db.rprefix())
  - Keyword-argument options instead of option objects
  - Batch context manager  (db.batch())
  - Transaction context manager with conflict detection  (db.transaction())
  - Pythonic snapshot wrapper  (db.snapshot())
  - Clean exception hierarchy  (ConflictError, DegradedError)
"""

from __future__ import annotations

import bytecaskdb as _bc

__all__ = [
    "DB",
    "Snapshot",
    "ConflictError",
    "DegradedError",
]

# ── Exceptions ───────────────────────────────────────────────────────────────

class ByteCaskError(Exception):
    """Base for all bytecaskdb_ext errors."""


class ConflictError(ByteCaskError):
    """Raised by a Transaction when apply_batch detects a write conflict."""


class DegradedError(ByteCaskError):
    """Raised when the engine enters a degraded state."""


# ── Internal helpers ─────────────────────────────────────────────────────────

def _write_opts(sync: bool, solo: bool) -> _bc.WriteOptions:
    o = _bc.WriteOptions()
    o.sync = sync
    o.solo = solo
    return o


def _read_opts(verify_checksums: bool) -> _bc.ReadOptions | None:
    if not verify_checksums:
        return None
    o = _bc.ReadOptions()
    o.verify_checksums = True
    return o


def _prefix_upper(prefix: bytes) -> bytes:
    """Return the smallest bytes value strictly greater than all keys that
    start with *prefix*, or b'' if prefix is all 0xFF bytes."""
    for i in range(len(prefix) - 1, -1, -1):
        if prefix[i] < 0xFF:
            return prefix[:i] + bytes([prefix[i] + 1])
    return b""


# ── Snapshot wrapper ──────────────────────────────────────────────────────────

class Snapshot:
    """Read-only frozen view of the database.

    Supports the same dict-like reads and iteration as DB.
    Use as a context manager to ensure resources are released promptly.
    """

    def __init__(self, inner: _bc.Snapshot) -> None:
        self._snap = inner

    def __enter__(self) -> "Snapshot":
        self._snap.__enter__()
        return self

    def __exit__(self, *args) -> None:
        self._snap.__exit__(*args)

    # ── Reads ─────────────────────────────────────────────────────────────────

    def __getitem__(self, key: bytes) -> bytes:
        v = self._snap.get(key)
        if v is None:
            raise KeyError(key)
        return v

    def __contains__(self, key: bytes) -> bool:
        return self._snap.contains_key(key)

    def get(self, key: bytes, default: bytes | None = None) -> bytes | None:
        v = self._snap.get(key)
        return v if v is not None else default

    # ── Iteration ─────────────────────────────────────────────────────────────

    def items(self, start: bytes = b""):
        """Iterate (key, value) pairs in ascending order from *start*."""
        return self._snap.iter_from(start)

    def keys(self, start: bytes = b""):
        """Iterate keys in ascending order from *start*. No disk I/O."""
        return self._snap.keys_from(start)

    def ritems(self, start: bytes = b""):
        """Iterate (key, value) pairs in descending order from *start*.
        When *start* is b'' (default), begins at the last key."""
        return self._snap.riter_from(start)

    def rkeys(self, start: bytes = b""):
        """Iterate keys in descending order from *start*. No disk I/O."""
        return self._snap.rkeys_from(start)

    def prefix(self, pfx: bytes):
        """Iterate (key, value) pairs whose key starts with *pfx*,
        ascending."""
        for key, value in self._snap.iter_from(pfx):
            if not key.startswith(pfx):
                break
            yield key, value

    def rprefix(self, pfx: bytes):
        """Iterate (key, value) pairs whose key starts with *pfx*,
        descending."""
        upper = _prefix_upper(pfx)
        for key, value in self._snap.riter_from(upper if upper else b""):
            if key >= upper and upper:
                continue
            if not key.startswith(pfx):
                break
            yield key, value


# ── Batch context manager ────────────────────────────────────────────────────

class _Batch:
    """Accumulates writes/deletes for atomic commit.

    Do not construct directly — use ``db.batch()``.
    """

    def __init__(self, db: _bc.DB, write_opts: _bc.WriteOptions) -> None:
        self._db = db
        self._plan = _bc.WritePlan()
        self._write_opts = write_opts

    def __setitem__(self, key: bytes, value: bytes) -> None:
        self._plan.put(key, value)

    def __delitem__(self, key: bytes) -> None:
        self._plan.del_(key)

    def put(self, key: bytes, value: bytes) -> None:
        self._plan.put(key, value)

    def delete(self, key: bytes) -> None:
        self._plan.del_(key)

    def delete_range(self, from_key: bytes, to_key: bytes) -> None:
        self._plan.del_range(from_key, to_key)

    def _commit(self) -> bool:
        return self._db.apply_batch(self._plan, self._write_opts)


class _BatchContext:
    def __init__(self, db: _bc.DB, write_opts: _bc.WriteOptions) -> None:
        self._db = db
        self._write_opts = write_opts
        self._batch: _Batch | None = None

    def __enter__(self) -> _Batch:
        self._batch = _Batch(self._db, self._write_opts)
        return self._batch

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is None and self._batch is not None:
            self._batch._commit()


# ── Transaction context manager (snapshot-backed, raises ConflictError) ──────

class _Transaction:
    """Snapshot-backed write plan.  Reads come from the snapshot; writes are
    staged and committed atomically.  Guards (ensure_*) can be added.

    Operations are staged in Python lists and replayed into a WritePlan at
    commit time.  This avoids consuming the snapshot (via C++ move) until
    reads are finished.

    Do not construct directly — use ``db.transaction()``.
    """

    def __init__(self, db: _bc.DB, raw_snap: _bc.Snapshot,
                 write_opts: _bc.WriteOptions) -> None:
        self._db = db
        self._raw_snap = raw_snap
        self._snap = Snapshot(raw_snap)
        self._ops: list[tuple[str, tuple]] = []
        self._guards: list[tuple[str, tuple]] = []
        self._write_opts = write_opts

    # ── Reads from snapshot ───────────────────────────────────────────────────

    def __getitem__(self, key: bytes) -> bytes:
        return self._snap[key]

    def __contains__(self, key: bytes) -> bool:
        return key in self._snap

    def get(self, key: bytes, default: bytes | None = None) -> bytes | None:
        return self._snap.get(key, default)

    def items(self, start: bytes = b""):
        return self._snap.items(start)

    def keys(self, start: bytes = b""):
        return self._snap.keys(start)

    def ritems(self, start: bytes = b""):
        return self._snap.ritems(start)

    def rkeys(self, start: bytes = b""):
        return self._snap.rkeys(start)

    def prefix(self, pfx: bytes):
        return self._snap.prefix(pfx)

    def rprefix(self, pfx: bytes):
        return self._snap.rprefix(pfx)

    # ── Staged writes ─────────────────────────────────────────────────────────

    def __setitem__(self, key: bytes, value: bytes) -> None:
        self._ops.append(("put", (key, value)))

    def __delitem__(self, key: bytes) -> None:
        self._ops.append(("del_", (key,)))

    def put(self, key: bytes, value: bytes) -> None:
        self._ops.append(("put", (key, value)))

    def delete(self, key: bytes) -> None:
        self._ops.append(("del_", (key,)))

    def delete_range(self, from_key: bytes, to_key: bytes) -> None:
        self._ops.append(("del_range", (from_key, to_key)))

    # ── Guards ────────────────────────────────────────────────────────────────

    def ensure_unchanged(self, key: bytes) -> None:
        self._guards.append(("ensure_unchanged", (key,)))

    def ensure_absent(self, key: bytes) -> None:
        self._guards.append(("ensure_absent", (key,)))

    def ensure_present(self, key: bytes) -> None:
        self._guards.append(("ensure_present", (key,)))

    def ensure_range_unchanged(self, from_key: bytes, to_key: bytes) -> None:
        self._guards.append(("ensure_range_unchanged", (from_key, to_key)))

    @property
    def has_snapshot(self) -> bool:
        return True

    def _commit(self) -> bool:
        plan = _bc.WritePlan(self._raw_snap)  # moves snapshot — reads are done
        for method, args in self._guards:
            getattr(plan, method)(*args)
        for method, args in self._ops:
            getattr(plan, method)(*args)
        return self._db.apply_batch(plan, self._write_opts)


class _TransactionContext:
    def __init__(self, db: _bc.DB, write_opts: _bc.WriteOptions) -> None:
        self._db = db
        self._write_opts = write_opts
        self._txn: _Transaction | None = None

    def __enter__(self) -> _Transaction:
        self._txn = _Transaction(self._db, self._db.snapshot(), self._write_opts)
        return self._txn

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is None and self._txn is not None:
            if not self._txn._commit():
                raise ConflictError(
                    "Transaction aborted: concurrent modification detected"
                )


# ── DB ────────────────────────────────────────────────────────────────────────

class DB:
    """Pythonic wrapper around bytecaskdb.DB.

    Open with DB.open(path) or DB.open(path, max_file_bytes=..., ...).
    """

    def __init__(self, inner: _bc.DB) -> None:
        self._db = inner

    # ── Open ──────────────────────────────────────────────────────────────────

    @classmethod
    def open(
        cls,
        path: str,
        *,
        max_file_bytes: int | None = None,
        recovery_threads: int | None = None,
        fail_recovery_on_crc_errors: bool | None = None,
    ) -> "DB":
        opts = _bc.Options()
        if max_file_bytes is not None:
            opts.max_file_bytes = max_file_bytes
        if recovery_threads is not None:
            opts.recovery_threads = recovery_threads
        if fail_recovery_on_crc_errors is not None:
            opts.fail_recovery_on_crc_errors = fail_recovery_on_crc_errors
        return cls(_bc.DB.open(path, opts))

    # ── Dict-like interface ───────────────────────────────────────────────────

    def __getitem__(self, key: bytes) -> bytes:
        """Return value for *key*; raises KeyError if not found."""
        v = self._db.get(key)
        if v is None:
            raise KeyError(key)
        return v

    def __setitem__(self, key: bytes, value: bytes) -> None:
        """Write key → value with default options (sync=True)."""
        self._db.put(key, value)

    def __delitem__(self, key: bytes) -> None:
        """Delete *key*. Silently succeeds if the key does not exist."""
        self._db.del_(key)

    def __contains__(self, key: bytes) -> bool:
        """Return True if *key* exists. No disk I/O."""
        return self._db.contains_key(key)

    def get(
        self,
        key: bytes,
        default: bytes | None = None,
        *,
        verify_checksums: bool = False,
    ) -> bytes | None:
        """Return value for *key*, or *default* if not found."""
        v = self._db.get(key, _read_opts(verify_checksums))
        return v if v is not None else default

    # ── Writes ────────────────────────────────────────────────────────────────

    def put(
        self,
        key: bytes,
        value: bytes,
        *,
        sync: bool = True,
        solo: bool = False,
    ) -> None:
        """Write key → value."""
        self._db.put(key, value, _write_opts(sync, solo))

    def delete(
        self,
        key: bytes,
        *,
        sync: bool = True,
        solo: bool = False,
    ) -> bool:
        """Delete *key*. Returns True if the key existed."""
        return self._db.del_(key, _write_opts(sync, solo))

    def delete_range(
        self,
        from_key: bytes,
        to_key: bytes,
        *,
        sync: bool = True,
        solo: bool = False,
    ) -> None:
        """Delete all keys in [from_key, to_key) with a single disk append."""
        self._db.del_range(from_key, to_key, _write_opts(sync, solo))

    # ── Iteration ─────────────────────────────────────────────────────────────

    def items(self, start: bytes = b"", *, verify_checksums: bool = False):
        """Iterate (key, value) pairs in ascending order from *start*."""
        return self._db.iter_from(start, _read_opts(verify_checksums))

    def keys(self, start: bytes = b"", *, verify_checksums: bool = False):
        """Iterate keys in ascending order from *start*. No disk I/O."""
        return self._db.keys_from(start, _read_opts(verify_checksums))

    def ritems(self, start: bytes = b"", *, verify_checksums: bool = False):
        """Iterate (key, value) pairs in descending order from *start*.
        When *start* is b'' (default), begins at the last key."""
        return self._db.riter_from(start, _read_opts(verify_checksums))

    def rkeys(self, start: bytes = b"", *, verify_checksums: bool = False):
        """Iterate keys in descending order from *start*. No disk I/O."""
        return self._db.rkeys_from(start, _read_opts(verify_checksums))

    def prefix(self, pfx: bytes, *, verify_checksums: bool = False):
        """Iterate (key, value) pairs whose key starts with *pfx*,
        ascending."""
        ropts = _read_opts(verify_checksums)
        for key, value in self._db.iter_from(pfx, ropts):
            if not key.startswith(pfx):
                break
            yield key, value

    def rprefix(self, pfx: bytes, *, verify_checksums: bool = False):
        """Iterate (key, value) pairs whose key starts with *pfx*,
        descending."""
        upper = _prefix_upper(pfx)
        ropts = _read_opts(verify_checksums)
        for key, value in self._db.riter_from(upper if upper else b"", ropts):
            if upper and key >= upper:
                continue
            if not key.startswith(pfx):
                break
            yield key, value

    # ── Batch / Transaction ───────────────────────────────────────────────────

    def batch(self, *, sync: bool = True, solo: bool = False) -> _BatchContext:
        """Context manager for an atomic batch of writes/deletes.

        Changes are committed on ``__exit__``.  No conflict detection.

        Example::

            with db.batch() as b:
                b[b"key1"] = b"value1"
                del b[b"key2"]
                b.delete_range(b"log:001", b"log:010")
        """
        return _BatchContext(self._db, _write_opts(sync, solo))

    def transaction(
        self, *, sync: bool = True, solo: bool = False
    ) -> _TransactionContext:
        """Context manager for a snapshot-backed transaction.

        Reads come from a snapshot taken at entry.  Writes are staged and
        committed atomically on ``__exit__``.  Raises ``ConflictError`` if
        a concurrent modification was detected.

        Example::

            with db.transaction() as txn:
                stock = int(txn[b"stock"])
                txn[b"stock"] = str(stock - 1).encode()
        """
        return _TransactionContext(self._db, _write_opts(sync, solo))

    # ── Snapshot ──────────────────────────────────────────────────────────────

    def snapshot(self) -> Snapshot:
        """Return a frozen read-only view of the database at this instant."""
        return Snapshot(self._db.snapshot())

    # ── Maintenance ───────────────────────────────────────────────────────────

    def vacuum(
        self,
        *,
        fragmentation_threshold: float | None = None,
        absorb_threshold: int | None = None,
    ) -> bool:
        """Run one vacuum pass. Return True if a file was vacuumed.

        Call in a loop to compact all eligible files::

            while db.vacuum():
                pass
        """
        opts = _bc.VacuumOptions()
        if fragmentation_threshold is not None:
            opts.fragmentation_threshold = fragmentation_threshold
        if absorb_threshold is not None:
            opts.absorb_threshold = absorb_threshold
        return self._db.vacuum(opts)

    def resume(self) -> None:
        """Attempt recovery from a degraded state."""
        self._db.resume()

    # ── Properties ────────────────────────────────────────────────────────────

    @property
    def is_degraded(self) -> bool:
        return self._db.is_degraded

    @property
    def degraded_reason(self) -> str:
        return self._db.degraded_reason
