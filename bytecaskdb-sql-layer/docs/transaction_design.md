# The SQL layer's transaction

Status: implemented in `bytecaskdb-sql-layer/txn/` (#488), moved from the
MariaDB plugin, where it was `MariaDBTxn`. Tests in
`tests/transaction_test.cpp`. The plugin derives its per-session adapter
from it; nothing in the layer uses it yet. Wiring it into `bc_ctx`, and
`BEGIN`/`COMMIT` in SQL, are #491's.

`bytecask::sql::Transaction` buffers a session's writes and reads from an
engine snapshot. It gives statement atomicity, multi-statement transactions
with statement rollback and savepoints, read-your-own-writes, and optimistic
conflict detection at commit. It is built on the engine's public API alone:
`Snapshot`, `WritePlan` and `apply_batch`.

## Header, not module

The layer is C++23 modules, but the plugin is built by CMake with MariaDB's
toolchain, and a module unit's compiled interface does not cross
toolchains. `txn/transaction.h`, `txn/transaction.cpp` and
`common/bytecask_view.h` are therefore plain C++20 on `include/bytecask.hpp`:
the plugin's CMake compiles `transaction.cpp` from here, and xmake compiles it
into `sql_layer`. They include no MariaDB header and import no module.

## State

- **Snapshot.** Taken at the statement's first read or write, or at
  `begin_if_needed(true)` for a multi-statement transaction, and held to the
  commit. Reads and the commit's write-write check use the same snapshot, so
  an autocommit read-modify-write conflicts with a commit made after its
  read instead of overwriting it.
- **Write buffer.** Two structures: `ops_`, the ordered log replayed into the
  `WritePlan` at commit, which keeps the order of writes to different keys;
  and `lookup_`, a sorted map from key to value or tombstone, for
  read-your-own-writes.
- **Merge iterators.** `iter_entries`, `riter_entries`, `iter_keys` and
  `riter_keys` walk the snapshot and `lookup_` together, forward or reverse,
  entries or keys only. On a tie the buffer wins, and a tombstone hides the
  snapshot's key. Each takes a namespace prefix, and emits only keys that
  start with it: the plugin passes a table's or an index's prefix.
- **Row counters.** `track_row_count_delta` applies a delta to a caller's
  counter and records it, so a rollback, a failed commit, a statement
  rollback or a savepoint rollback can undo it.

## Lifecycle

`begin_if_needed(multi)` starts a statement if none is open and, with
`multi`, a transaction. It returns which it started, so a caller with a
transaction coordinator registers each.

`commit(all, sync)` returns a `CommitOutcome`:

| Status | When | Then |
|---|---|---|
| `Committed` | The plan applied, or there was nothing to apply, or a statement commit inside a transaction kept the buffer | — |
| `Conflict` | The snapshot's write-write check or a guard refused the plan | Nothing written |
| `DuplicateKey` | A deferred insert's `ensure_absent` guard refused it | Nothing written; `duplicate` names the first guarded key that exists and its row |
| `EngineError` | The engine threw (`error` holds the exception), or refused a plan that cannot conflict | Nothing of the buffer written |

On every status but `Committed` the row-count deltas are reverted and the
transaction is reset. The caller maps the status to its own error: the
plugin maps them to `HA_ERR_LOCK_DEADLOCK`, `HA_ERR_FOUND_DUPP_KEY` (with the
server's duplicate-key message, rendered from `duplicate`) and
`HA_ERR_INTERNAL_ERROR`, and aborts the server when the engine is degraded.

`rollback(all)` discards the transaction, or, for a statement inside a
transaction, truncates `ops_` to the mark taken when the statement began,
rebuilds `lookup_` from what remains, and restores the row counters to the
same point.

A savepoint is an index into a stack of (log mark, row counters).
`savepoint_rollback` restores both and keeps the savepoint;
`savepoint_release` drops it and every later one.

## Deferred inserts

A plain autocommit insert whose only unique constraint is the primary key
need not read the snapshot to check for a duplicate.
`begin_deferred_insert()` puts the statement in a mode that takes no
snapshot: each put carries `guard_absent`, which commit turns into
`ensure_absent` on a snapshot-less plan, and a duplicate within the
statement is caught by `buffered_key_present`. A conflict on such a plan can
only be a guard, so it is `DuplicateKey`.

## Bulk copy

A copy into a table no other transaction can see yet (MariaDB's
`ALTER TABLE ... ALGORITHM=COPY` target) does not go through the buffer.
`begin_bulk_copy` sends puts to a separate snapshot-less plan, flushed with
`bulk_flush` whenever it passes a byte threshold, so memory stays bounded by
the threshold. The buffer, the snapshot and any merge iterator open over
them are untouched, since the copy's source scan runs in the same
transaction. Duplicate probes check the unflushed batch and then the DB.
A commit flushes the batch's tail; a rollback drops it.

## Not thread-safe

A transaction belongs to one session, and every call comes from that
session's thread.
