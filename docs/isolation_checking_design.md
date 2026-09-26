# Isolation Checking with Elle

Status: design. Tracks [#94](https://github.com/gustavoamigo/bytecaskdb/issues/94).
Follow-ups that extend this harness: [#178](https://github.com/gustavoamigo/bytecaskdb/issues/178)
(replication), [#176](https://github.com/gustavoamigo/bytecaskdb/issues/176) (SIGKILL).

## Purpose

The README claims snapshot isolation for reads through a `Snapshot`, and
serializable conflict detection for a `WritePlan` that guards what it read.
The tests behind those claims are pairwise: two threads, one conflict, assert
`nullopt`. That checks the mechanism. It does not check the claim, because the
anomalies that break isolation (G-single, G2-item, lost update, write skew)
show up as cycles across many transactions, not as one bad conflict.

This harness records a concurrent history of many transactions and hands it
to [Elle](https://github.com/jepsen-io/elle), through
[`elle-cli`](https://github.com/ligurio/elle-cli), which searches the history
for dependency cycles and names the anomaly each one is.

It complements the other validation layers in
[`correctness_validation.md`](correctness_validation.md): the proof matrix
checks the write path one failure at a time, the crash harness checks the
durable prefix after SIGKILL, and this checks isolation under concurrency.

## Two corrections to the issue

**The commit sequence cannot be the value.** #94 proposed writing each value
as its own `CommitResult.sequence`, so that Elle would not have to infer
version order. The sequence is assigned at commit, after the value is already
in the plan, so this is not possible. The harness uses Elle's `list-append`
model instead, where every read returns the whole list and so carries its own
version order. The sequence is still useful, as a cross-check (below).

**A write that throws is not a failed write.** #94 proposed recording writes
that throw `DbDegraded` as `:fail`, and asserting that they are never visible.
That holds for a write refused at entry by an engine that is already
degraded. It does not hold for the write whose `fdatasync` failed and caused
the degrade: its entries are in the active file, and `resume()` replays valid
committed entries (see *Sync failure degrade-then-rethrow* in
`correctness_validation.md`). A writer caught in the same group can be in the
same position. The harness cannot tell these cases apart from the exception,
so every exception is recorded as `:info` (outcome unknown). Only `nullopt`
is `:fail`. Elle accepts `:info` operations whether or not their writes later
appear.

## Model: `list-append` over byte values

Each key holds a list of integers, stored as its value: decimal, comma-
separated. A transaction is 1–4 micro-operations over a small set of active
keys:

- `[:r k nil]` — `snap.get(k)`, recorded with the list read.
- `[:append k id]` — read `k` from the same snapshot, append a unique
  integer `id`, and `plan.put(k, list + [id])`.

The append is emulated with a read and a put. It is a true append only if no
other transaction wrote `k` between this transaction's snapshot and its
commit. The implicit W-W check on write keys is what makes that true. If that
check is broken, a committed append goes missing from later reads, and Elle
reports lost update or incompatible order. The harness therefore checks the
implicit W-W check through the model itself, not beside it.

Inside a transaction, reads and appends on the same key see the
transaction's own earlier appends. The harness keeps a per-transaction
buffer for this, as `Transaction` (Layer 2) would.

A transaction with no appends does not call `apply_batch`. It completes
`:ok` with the reads it made.

**Key rotation.** Contention needs few keys. Cycle search and value size
need short lists. There are 8 active keys, and a key retires after 64
appends, when a fresh key takes its slot. This is the same scheme Jepsen's
`list-append` generator uses.

**Not modelled in this pass.** `del`, `del_range`, and
`ensure_range_unchanged` have no counterpart in `list-append`, and Elle
checks items, not predicates. Phantoms and range guards remain covered only
by the pairwise tests. That gap is stated here rather than worked around.

## Configurations

Each run records three histories from one seed. They differ only in how the
plan is built:

| Config | Plan | Expected |
|---|---|---|
| `guarded` | `WritePlan(snap)`, `ensure_unchanged` on every key read but not appended | Valid under `strict-serializable` |
| `unguarded` | `WritePlan(snap)`, no `ensure_unchanged` | Valid under `snapshot-isolation`. Under `strict-serializable`, the only reported anomalies are `G2-item` / `G2-item-realtime` |
| `blind` | Read with `db.get`, write with a snapshot-less `WritePlan()` | Reports `lost-update` |

`guarded` is the claim under test. `unguarded` and `blind` show that the
harness can see anomalies. A harness that reports nothing in every
configuration is not evidence of anything. `unguarded` also pins the
boundary: a G1 or G-single there is a bug in the implicit W-W check, not
expected write skew. Before trusting a run, the harness requires `unguarded`
to report `G2-item` at least once over the run.

`strict-serializable` rather than plain `serializable`: once `apply_batch`
returns, a snapshot taken later must see the write (`state_.store()` happens
before return). Real-time edges check that too. If they prove too strict for
a legitimate reason, that becomes a documented finding, and the check drops
to `serializable`.

## Commit-sequence cross-check

Elle infers each key's version order from the lists it reads. The engine
also knows that order: the `CommitResult.sequence` of each `:ok` appending
transaction. Before calling Elle, the harness checks that, for every key, the
longest list read is ordered by ascending commit sequence of the transactions
that appended its elements, with `:info` elements placed wherever they
appear. This is a direct check with no inference involved. It also guards
the harness against an encoding bug that Elle would otherwise read as an
anomaly.

## Nemeses

Each run draws from these per seed:

- **Concurrent vacuum.** A thread calls `vacuum()` in a loop, with
  `max_file_bytes` small (16 KiB–1 MiB) so that rotation and compaction run
  under the workload.
- **Degrade and resume.** A nemesis thread arms a `ScopedFaultInjector` at
  `io_data_file_sync` and issues its own append with `WriteOptions::solo`.
  The injector is thread-local, and `solo` keeps that write's I/O on the
  nemesis thread rather than a group leader's. The engine degrades. Clients
  record `:info` until the nemesis calls `resume()` after a random delay.
  Whether the solo path's `fdatasync` runs on the calling thread, given the
  pipelined commit, has to be confirmed in implementation. If it does not,
  the nemesis fails `io_data_file_append` instead.
- **I/O backend.** `Pread`, `Mmap`, or `BufferPool`, drawn per run.

Replication and SIGKILL are not nemeses here. #178 and #176 add them.

## Implementation

**The generator is C++, not Python.** #94 proposed the Python binding. Three
things rule it out:

- The fault injector exists only under `BYTECASK_TESTING`, which the wheel
  does not define.
- Python work between calls runs under the GIL. The binding releases the GIL
  inside engine calls, but the clients' own read-modify-write logic would be
  serialized, and client interleaving would be coarser than a real workload.
- #176 needs to fork and kill a child. `crash_consistency` already has that
  machinery in C++.

Files:

- `tests/elle/isolation_history.cpp` — xmake target `isolation_history`,
  built like `crash_consistency` (`BYTECASK_TESTING`, sanitizer-aware). It
  runs one configuration and writes the history as JSON, which `elle-cli`
  accepts directly.
- `scripts/run_isolation_check.py` — runs the binary for each configuration,
  runs the commit-sequence cross-check, calls `elle-cli` with the models
  above, reads the anomaly types Elle reports, and fails on anything outside
  the expected set. It prints the seed first.

**History recording.** Each client thread is an Elle `:process`. A global
atomic counter stamps `:index` at invoke (before `snapshot()`) and at
completion (after `apply_batch` returns or throws). Events go into
per-thread buffers and are merged by index at the end. The recording never
takes a shared lock on the hot path. `:time` is `steady_clock` nanoseconds.

**Reproducibility.** The seed fixes each thread's workload and the nemesis
schedule. Thread interleaving still varies, as kill timing does in the
crash harness. A failure uploads the history and Elle's output directory,
which is what a reproduction needs.

**Scale.** 8–32 client threads, 50k transactions per configuration to start.
The budget is the runtime of `elle-cli`, not of the engine. Raise it once
nightly timings are known.

**CI.** A new nightly workflow, `isolation-nightly.yml`, runs `release` and
`asan` jobs, on the same pattern as `crash-nightly.yml`. `elle-cli` has no
published releases. The job installs Java 21 and Leiningen, builds the
uberjar at a pinned `elle-cli` commit, and caches the jar keyed on that
commit. `ci.yml` only compiles `isolation_history`, as it does
`crash_consistency`.

## Acceptance

- `guarded` is valid under `strict-serializable` on every nightly run.
- `unguarded` reports only `G2-item` / `G2-item-realtime` (at least once per
  run), and `blind` reports `lost-update`.
- The commit-sequence cross-check passes in all three configurations.
- The seed is printed, and a failure uploads the history.
- The README's isolation paragraph and `transaction_design.md` cite this
  check.
