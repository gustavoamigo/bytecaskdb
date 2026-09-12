# MariaDB plugin — steady-state validation plan

Date: 2026-09-12. Status: **active**. Supersedes feature work until the
decision in [§5](#5-decision-after-the-run) is taken.

## 1. Decision

Park new SQL features. Fix three correctness gaps found in review, then run a
long, larger-than-buffer-pool comparison against InnoDB and let that data pick
the next engine task.

The 60-second sysbench runs in `benchmarks/sysbench_results.csv` (500k rows,
2 vCPU codespace, InnoDB at `innodb_flush_log_at_trx_commit=1`) do not support
"close to InnoDB" once more than one client thread is involved:

| Workload | 1 thread | 2 threads | 4 threads |
|---|---|---|---|
| oltp_point_select | +3% | +16% | +13% |
| oltp_read_only | -11% | -8% | -12% |
| oltp_write_only | -7% | -38% | -42% |
| oltp_insert | -36% | -47% | -48% |
| oltp_read_write | +24% | -38% | -50% |

Two signals in that table matter more than the percentages. Write throughput
*drops* from one thread to two, which points at group commit never batching at
low concurrency. And a 60-second run on a table that fits in RAM cannot show
the failure modes either engine is expected to have over hours: InnoDB
checkpoint and purge stalls, ByteCaskDB vacuum interference and memory growth.

PR #28 (deferred `EngineState` reclamation) is closed, branch kept. Its own
benchmark gate was never met and BC-245 removed most of its premise. Reopen
only if a fresh `oltp_insert` profile on current main still shows
`Node::release` above ~10% of the client thread.

## 2. Blockers — fix before the long run

Each is small, plugin-local, and lands with a functional test. Without them the
long run measures bugs.

### 2.1 Autocommit reads run before the OCC snapshot exists

`MariaDBTxn::begin_if_needed` only takes the snapshot for explicit
transactions. In autocommit, `MariaDBTxn::get` and `MariaDBTxn::exists` fall
through to the live DB and the snapshot is taken later by the first buffered
write. Two clients running `UPDATE t SET v = v + 1 WHERE id = 1` can both read
v, one commits, the other snapshots *after* that commit, and its write passes
the engine's write-write check. One increment is lost. The same ordering
applies to `update_row`'s PK-change duplicate probe and to INSERTs on tables
with a UNIQUE secondary index (PR #29 covers only plain INSERTs on
PK-only-unique tables).

Fix: take the snapshot in `get` and `exists` when none is held, as
`iter_prefix` already does. The engine treats "key appeared since snapshot" as
a conflict, so the late writer then gets error 1213 instead of silently
overwriting.

Test: `tests/functional/test_concurrency.py`, two connections in autocommit
racing the increment above; assert the final value is 2 or one connection got
1213.

### 2.2 Statement rollback inside a transaction discards the whole buffer

`MariaDBTxn::rollback(all=false)` clears `ops_` and `lookup_` entirely. If the
third statement of a `BEGIN` block fails, statements one and two vanish and
the later `COMMIT` returns success as a no-op.

Fix: record `ops_.size()` at statement start (the `!registered_stmt_` branch
of `begin_if_needed`) and truncate to it on statement rollback, reusing the
rebuild in `savepoint_rollback`. Revert only that statement's row-count
deltas.

Test: `cases/transactions.yaml` — BEGIN, two inserts, a third that hits a
duplicate key, COMMIT; assert the first two rows exist.

### 2.3 Reverse scans with buffered writes

`MergeIterator` walks the write buffer forward in reverse mode and its compare
assumes ascending order, so `BEGIN; INSERT ...; SELECT ... ORDER BY id DESC`
returns uncommitted rows out of order. Separately, `index_read_map` sends
`HA_READ_BEFORE_KEY` and `HA_READ_KEY_OR_PREV` down the forward-iterator
path, which is what the optimizer issues for descending range scans.

Fix: walk the buffer with a reverse iterator when `reverse_` is set and invert
the compare; route the two find flags to `riter_prefix` /
`riter_index_prefix`.

Test: `cases/index_scan_completeness.yaml` — inside a transaction with
uncommitted rows, `WHERE id < 10 ORDER BY id DESC` on PK and on a secondary
index.

## 3. Semantics to state plainly

Half a day, no engine change.

- Drop `HTON_SUPPORTS_FOREIGN_KEYS` from `bytecaskdb_init`. FK metadata is
  kept for DDL only; nothing is enforced.
- Add a "Differences from InnoDB" section to the plugin README:
  `SELECT ... FOR UPDATE` is a plain snapshot read (no `ensure_unchanged`
  guards are emitted); write-write conflicts surface at COMMIT as error 1213
  and are not retried by the server; whole transactions are buffered in RAM
  until commit; long-lived snapshots (`mysqldump --single-transaction`)
  defer vacuum.
- Fix the `run-sysbench.sh` comment that says the secondary index is off by
  default while the flag turns it on.

## 4. Steady-state benchmark protocol

Machine: at least 4 physical cores, local SSD, nothing else running. The
2 vCPU codespace is not usable for the concurrency rows.

| Parameter | Value |
|---|---|
| Table | 1 table, 5M rows (~1 GB) |
| InnoDB buffer pool | 1G, so the working set does not fit |
| Durability | `innodb_flush_log_at_trx_commit=1`; ByteCaskDB `sync=true` (default) |
| Workloads | `oltp_write_only`, `oltp_read_write`, secondary index on |
| Threads | 8 |
| Duration | 45 minutes per workload per engine |
| Sampling | `--report-interval=10`; keep p95, p99 and max per interval |
| Engine counters | `SHOW ENGINE BYTECASKDB STATUS` every 60 s: vacuum bytes reclaimed, fsyncs, open files; RSS of `mariadbd` |
| InnoDB counters | `Innodb_buffer_pool_wait_free`, `Innodb_log_waits`, `Innodb_history_list_length` every 60 s |

Report per engine: tps over time, p99 over time, max latency per interval,
RSS over time. The interesting result is the shape of the curves, not the
final average.

## 5. Decision after the run

- ByteCaskDB flat while InnoDB degrades: the design claim holds. Next task is
  the low-concurrency group-commit fix (2-thread write throughput below
  1-thread), then re-run.
- Both flat: the write-path CPU gap is the priority. Continue BC-246/BC-247
  (radix-tree clone cost) and re-profile `oltp_insert` on current main.
- ByteCaskDB degrades: find out why before anything else. Likely suspects are
  vacuum contending with the writer and in-memory transaction buffering.

No new SQL features until one of these branches is taken.

## 6. Backlog from the review (not blocking)

| Item | Note |
|---|---|
| Savepoint row-count drift | `savepoint_rollback` truncates `ops_` but not `row_count_deltas_`; `stats.records` drifts after `ROLLBACK TO SAVEPOINT`. |
| Catalog pointer escapes its mutex | `catalog_lookup_meta` returns a pointer into a locked map; `update_row`, `delete_row`, `check` use it unlocked. The in-place `DROP FOREIGN KEY` ALTER reassigns the entry under `HA_ALTER_INPLACE_NO_LOCK`. Extend the BC-241 handler-side cache (`indexes_`) to those paths. |
| Deferred-INSERT error message | Commit-time duplicate prints `Duplicate entry ''` because `apply_batch` does not say which guard failed. |
| Triggers on deferred INSERT | A trigger that writes another table in a deferred autocommit INSERT gets no snapshot. Add a trigger functional test or exclude `table->triggers` from the deferred path. |
| `records_in_range` | Returns a constant tenth of the table; the optimizer cannot rank ranges. |
| Plugin guide | Says the server retries on deadlock; it does not. |

## 7. Status

| Step | Status |
|---|---|
| PR #28 closed, branch kept | pending |
| PR #29 merged | done (d8f5f1f) |
| 2.1 autocommit snapshot | open |
| 2.2 statement rollback | open |
| 2.3 reverse merge scan | open |
| 3 semantics / FK flag / README | open |
| 4 steady-state run | blocked on 2.x |
