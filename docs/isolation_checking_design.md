# Isolation Checking with Elle

Status: implemented, runs nightly. Tracks [#94](https://github.com/gustavoamigo/bytecaskdb/issues/94).
Follow-ups that extend this harness, both implemented: [#178](https://github.com/gustavoamigo/bytecaskdb/issues/178)
(replication) and [#176](https://github.com/gustavoamigo/bytecaskdb/issues/176) (SIGKILL, see *Kill*).

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
buffer for this; the engine has no transaction type that would.

A transaction with no appends does not call `apply_batch`. It completes
`:ok` with the reads it made.

**Key rotation.** Contention needs few keys. Cycle search and value size
need short lists. There are 8 active keys, and a key retires after 64
appends, when a fresh key takes its slot. This is the same scheme Jepsen's
`list-append` generator uses.

**Not modelled by `list-append`.** `del`, `del_range` and
`ensure_range_unchanged` have no counterpart in Elle's models, which check
items and not predicates. The history checker has no model to put them in
either, and a read through a range guard is not a list append. They get a
workload of their own, below.

## Range workload

`--workload range` (#302) runs the operations Elle cannot model: scans, point
reads, puts, dels and range deletes, with range guards, over keys narrow
enough that the ranges overlap. The invariant is a range that held a key
keeps one.

**The key space.** 32 keys, `k00`–`k31`, of fixed width, so byte order is key
order and a range is a pair of ids. Every write stores a value unique to it,
`w<session>.<n>`, where the session is the writer's thread in the in-process
run, or its process id in a kill's or a chaos episode's lives. A later read
therefore names exactly the version it saw without the checker having to know
which transaction wrote it.

**A transaction.** It scans a range of 2–7 keys and, when the scan holds two
or more live keys, deletes one: a `del` or a one-key `del_range`, alternating.
Now and then it first inserts a fresh key into the range. Two transactions on
overlapping ranges that each delete a key the other scanned, and each only
read the other's key, are write skew: with no range guard both commit and the
range loses a key.

**The two configurations.**

- `guarded` reads through the snapshot, adds `ensure_unchanged` for every key
  read and not written and `ensure_range_unchanged` for every range scanned
  and not deleted, and commits a snapshot plan. The engine refuses the
  transaction whose read set another thread changed, in the same serial
  section as the head it validates.
- `unguarded` reads through the head with no guards, and takes its delete
  decision on a scan of the head taken after its snapshot, which puts its read
  set behind its plan. Snapshot isolation admits it; the range guards are what
  a serializable plan adds.

**The check.** `history_check` in `run_isolation_check.py` replays the
history against the order the engine enforced. A committed transaction carries
the highest sequence it was assigned, and the engine assigns sequences in
commit order, so sorting the committed transactions by it is the order their
writes reached the database. Each is applied to a map of the key space, and
every read and scan is held to that map:

- `guarded`: the version a read returns is the one at its key, and a scan
  shows every key its range holds, at the transaction's own position.
- `unguarded`: a read or scan may be behind its own position, but only to a
  value the key has held: reading a value the order never held is a
  violation, not an exposure. At least one transaction has to have read a
  state behind its own, or the run is not shown to reach write skew at all.

A final read, taken after every client stopped, is held to the end of the
replayed order. This is what catches a write that was acknowledged and then
lost, when no later read happened to touch its key.

Two things the history cannot pin down, and how the check handles them:

- An `:info` transaction may or may not have committed. Each key its write
  named is placed only where a committed read named that key's exact value,
  just before that reader: placement is per key, so an `:info` placed by one
  key's value leaves its other keys unknown. A key whose last unplaced
  `:info` write is still the newest thing the history knows about it is not
  judged; a committed write of the key makes it judgeable again. This is
  what a kill leaves behind; a kill-mode round is about a hundred live
  `:info` transactions.
- A plan's entries span sequences below the one it reports, so another
  transaction's write can fall inside that span. A read is held to the state
  at any point across its own transaction's window, which is as wide as the
  plan has entries.
- A transaction that committed nothing carries no sequence, and the value it
  read can carry one above its own completion: the span above is why. Its
  reads are counted but not judged. A read-only transaction is roughly half
  the history, so this is the check's largest gap.

The state the database started with, `s<k>`, is the value at every key before
the first write of the history, and a read of it is judged against that. A
kill may lose it and the re-seed that follows is not a transaction, so after
the first epoch only versions this history wrote are judged.

The kill mode and the chaos rig run the same workload, which is where most of
the `:info` transactions come from, and so does the nemesis mix of vacuum and
degrade.

**Mutations.** Three breaks of the range check, each applied by hand and
caught by a `guarded` history in a run of 6,000 transactions on 16 threads,
with the mutation in place (20 or more violations each; the `unguarded`
history of the same seed keeps showing its exposures):

| Mutation | What the guarded history reports |
|---|---|
| skip the "deleted since the snapshot" pass of `range_changed` | reads of a state the order has already left: a delete went unseen |
| skip the "changed or inserted" pass | reads of a state the order has left, and scans that miss a key the order has live in the range |
| treat a write above the snapshot's sequence as unchanged, as validating against the published state does | both: scans that miss live keys, and reads of replaced versions |

The last is the effect of validating against the published state rather than
the head; replacing the head with the published state in `execute_group`
hangs the run instead, since the head then disagrees with the file it writes
to.

## Configurations

Each run records three histories from one seed. They differ only in how the
plan is built:

| Config | Plan | Expected |
|---|---|---|
| `guarded` | `WritePlan(snap)`, `ensure_unchanged` on every key read but not appended | Valid under `strict-serializable` |
| `unguarded` | `WritePlan(snap)`, no `ensure_unchanged` | Valid under `snapshot-isolation`. Under `strict-serializable`, the only reported anomalies are `G2-item` variants |
| `blind` | Read with `db.get`, write with a snapshot-less `WritePlan()` | Invalid: committed appends go missing |

`guarded` is the claim under test. `unguarded` and `blind` show that the
harness can see anomalies. `blind` has no W-W check, so two appends that
read the same list overwrite each other. Elle names that in several ways
(incompatible order, G-single, lost update, depending on which reads
witness it), so the script requires only that the history is invalid; the
cross-check below reports the missing appends directly. A harness that reports nothing in every
configuration is not evidence of anything. `unguarded` also pins the
boundary: a G1 or G-single there is a bug in the implicit W-W check, not
expected write skew. Before trusting a run, the harness requires `unguarded`
to report `G2-item` at least once over the run.

`strict-serializable` rather than plain `serializable`: once `apply_batch`
returns, a snapshot taken later must see the write (`state_.store()`
happens before return). Real-time edges check that too. `unguarded` gets
the same real-time check from its `strict-serializable` pass, which admits
only `G2-item` variants, so its snapshot-isolation pass uses the plain
model. If real-time edges prove too strict for a legitimate reason, that
becomes a documented finding, and the check drops to the non-real-time
model.

## Commit-sequence cross-check

Elle infers each key's version order from the lists it reads. The engine
also knows that order: the `CommitResult.sequence` of each `:ok` appending
transaction. After all clients finish, the generator reads every key once
more, so the longest read of a key is its final value. Before calling Elle,
the script checks, per key:

- every read is a prefix of the final value;
- the final value holds every `:ok` append, in ascending commit sequence
  (`:info` elements may appear anywhere, or not at all);
- no element comes from a `:fail` transaction, none is duplicated, and
  every element was invoked. Both hold per key: an element id is unique to
  a write within one process, and a process counter restarts with each
  epoch, so the same id names another key's write in another life;
- a read sees every append whose transaction completed `:ok` before the
  reading transaction was invoked (real-time visibility). The first CI run
  showed why this belongs here as well as in Elle: see *Findings*.

This is a direct check with no inference involved. It also guards the
harness against an encoding bug that Elle would otherwise read as an
anomaly, and it runs without a JVM.

## Nemeses

Each run draws from these per seed:

- **Concurrent vacuum.** A thread calls `vacuum()` in a loop, with
  `max_file_bytes` small (16 KiB–1 MiB) so that rotation and compaction run
  under the workload.
- **Degrade and resume.** Every 20–200 ms, a nemesis client arms a
  `ScopedFaultInjector` at `io_data_file_sync` and runs append transactions
  with `WriteOptions{.sync = true, .solo = true}`. The injector is
  thread-local. `solo` puts the append on the nemesis thread, but with the
  pipelined commit the `fdatasync` runs on whichever thread holds the flush
  role, so the nemesis retries (up to 50 transactions) until its own flush
  fails and the engine degrades. It then calls `resume()` after 1–20 ms.
  Its transactions are ordinary history entries. Other clients back off
  while the engine is degraded, so the history is not flooded with `:info`
  entries for writes refused at entry.
- **I/O backend.** `Pread`, `Mmap`, or `BufferPool`, drawn per run.

Replication is not a nemesis here: #178 adds it, in
[`replication_checking_design.md`](replication_checking_design.md). SIGKILL
has its own mode, below.

## Kill

`crash_consistency` kills a single writer and checks an exact prefix of its
commit order. It never kills the process while several sync writers share
one `fdatasync` through group commit, with the pipelined next batch already
appended: there the parent does not know which writes were in flight or in
what order. Inferring that order is Elle's job, so the kill is a mode of this
harness (#176): `isolation_history --kill-epochs N`, and
`run_isolation_check.py --kill` for guarded and unguarded.

**A child per epoch.** A SIGKILL takes the history in memory with it, so the
writers run in a child process, as in `crash_consistency`, and stream every
operation to the parent over a pipe as it happens: an invoke line before the
call, a completion line after it (type, reads, `CommitResult.sequence` and
`durable`), and `durable_sequence()` from a watcher thread. Lines are
written whole under a mutex; one the kill cut short is dropped. The parent
kills the child once it has reported a random 10–95% of its share of
transactions, which puts the kill inside the workload on any machine, then
starts the next child on the same directory, so every epoch after the first
begins with a recovery. After the last, the parent reopens the directory
itself and reads every key as one final transaction.

**Encoding.**

- Invoked, not completed at the kill: `:info`.
- Acknowledged with `durable`, or with a sequence at or below a
  `durable_sequence()` the child reported: `:ok`.
- Acknowledged, not known durable: `:info`. A crash may lose it; SIGKILL in
  practice does not, since the page cache survives, and `:info` allows both.
- `nullopt`: `:fail`.

Every epoch uses fresh Elle process ids (a process with an `:info` cannot
go on), elements unique across the run, and the keys the last epoch was
appending to, so reads after a recovery see what the kill left. Times are
`CLOCK_MONOTONIC`, one clock for every process, so real-time order holds
across kills.

**Configuration.** Engine settings come from the seed as usual, with the
sync share raised to 50–90% so writers do share flushes, and no degrade
nemesis. The vacuum nemesis runs in the child, so kills also land inside a
compaction.

**Reaching the case.** SIGKILL cannot lose an acknowledged write (the page
cache survives it), so there is no failure to provoke on purpose, as `blind`
does for lost updates. The run instead has to show that it reached the
window it is for: at least one kill with two or more sync writers in flight.
Each child writes `<out>.kill.json` with that count.

**Result.** 3 rounds, 10 epochs each, 16 threads: guarded strict-serializable
and unguarded snapshot-isolated (write skew only) in every round, with the
cross-check clean. Every kill had about 15 transactions in flight and caught
two or more sync writers; one landed inside a compaction and the next open
removed the half-done copy.

## Chaos

The kill mode's SIGKILL keeps the page cache, so it never loses an
acknowledged write, and it never fails an I/O. The chaos mode (#232) runs the
same writers under the chaos rig's hazards
([`chaos_testing_design.md`](chaos_testing_design.md)): power loss, failed
`fdatasync` with Linux semantics, eviction, failed writeback, full disks,
read-only remounts, metadata errors, latency and stalls. It is
`run_chaos.py --workload elle --config guarded|unguarded` with
`isolation_history` as the worker.

**Who drives.** The chaos orchestrator owns the mount, the fault timeline and
how each process life ends: a power cut (freeze, SIGKILL, crash, fence), a
SIGKILL, or a clean stop with the hazards lifted. A life is an epoch. The
child, `isolation_history --chaos-child`, is the kill child plus what a
service would do under faults:

- `O` once `DB::open` succeeds, or `F <error>` and exit 4 if it fails, so the
  orchestrator can require the next open, with every hazard lifted, to work.
- A recovery thread that calls `resume()` when the engine degrades, with
  backoff. If `resume()` refuses and asks for a reopen (#240), the child
  writes `R` and exits 6, which ends the life; the orchestrator accepts that
  only after an eviction dropped pages.
- On SIGTERM the writers finish, then `close()`. If it returns, a
  `durable_sequence` line at the last committed sequence follows, so every
  acknowledged write counts as durable; `X ok` or `X fail <error>` records the
  result.
- The `Mmap` backend is mapped to `Pread`, which chaosfs's `direct_io` mount
  cannot map.

At the end of an episode (N lives on one directory) `isolation_history
--absorb` turns the saved lives into one history with the kill mode's
`absorb_epoch`, then reads every key from a copy of the directory the last
life left. The history must pass the cross-check, and Elle: guarded
strict-serializable, unguarded snapshot-isolated.

**What a power cut may take.** The kill encoding holds per life. One rule is
added for lives whose page cache lost data (a power cut, or an eviction that
dropped pages): a read that returned an element not durable on the mount is
`:info`, with its result cleared as the kill mode's downgrades are. Such a
read saw a write the contract lets a crash take, and without the rule a
later read that no longer sees it would count as a lost write. An element
stays "not durable on the mount" from the life that wrote it until a later
life's `DB::open` completes (open makes what it trusts durable, #240) or a
`close()` returns. An eviction while that open runs, before it has read the
pages, takes what the earlier lives left there, and the open truncates it as
a torn tail (#437). So the orchestrator records, per life, whether pages were
evicted before it saw the open complete, and `--absorb` applies that loss to
the earlier lives' reads before the open clears them. The life's own reads
came after the open and are not touched by it. The orchestrator samples
chaosfs's count only once it sees the `O` line, so a late sample can only
count an eviction after the open as one before it. Reads of durable data are
checked in full.

**Sensitivity.** With `commit_skips_fdatasync` from `tests/chaos_mutations/`
applied (the commit flush skips its `fdatasync` and still reports durable),
the first guarded episode fails the cross-check: committed appends are
missing from the final read, and reads miss acknowledged appends.

**Result.** Guarded, 3 minutes: 92 lives, about 489,000 acknowledged
transactions, 48 lives ended with two or more sync writers in flight, 46 lost
page-cache data; every episode clean and strict-serializable. Unguarded,
3 minutes: 60 lives, every episode snapshot-isolated.

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
  the expected set. It prints the seed first. With `--workload range` it
  skips Elle and checks the history itself instead (`history_check`).

**History recording.** Each client thread is an Elle `:process`. A global
atomic counter stamps `:index` at invoke (before `snapshot()`) and at
completion (after `apply_batch` returns or throws). Events go into
per-thread buffers and are merged by index at the end. The recording never
takes a shared lock on the hot path. `:time` is `steady_clock` nanoseconds.

**Reproducibility.** The seed fixes each thread's workload and the nemesis
schedule. Thread interleaving still varies, as kill timing does in the
crash harness. A failure uploads the history and Elle's output directory,
which is what a reproduction needs.

**Scale.** 16 client threads by default and 50k transactions per
configuration. The engine generates a history in about two seconds, so the
budget is the runtime of `elle-cli`, not of the engine. Raise it once
nightly timings are known.

**CI.** `isolation-nightly.yml` runs `release` (50k transactions) and
`asan` (20k) jobs, 4 rounds each, on the same pattern as
`crash-nightly.yml`. `elle-cli` has no published releases. The job installs
Java 21 and Leiningen, builds the uberjar at a pinned `elle-cli` commit, and
caches the jar keyed on that commit. `ci.yml` only compiles
`isolation_history`, as it does `crash_consistency`.

Run locally:

```
xmake build isolation_history
python3 scripts/run_isolation_check.py \
  --binary build/linux/x86_64/releasedbg/isolation_history \
  --elle-jar elle-cli.jar --rounds 4
```

Without `--elle-jar` only the cross-check runs. The range workload needs no
jar:

```
python3 scripts/run_isolation_check.py \
  --binary build/linux/x86_64/releasedbg/isolation_history \
  --workload range --kill --rounds 4
```

## Findings

**A rotation after a failed flush cleared the degrade and hung the engine.**
The first runs with vacuum and degrade together were on a base without
#170. They hung in about one run in six. Every client was blocked in
`wait_published`, waiting for a conflicting write to be published. The
published state was not degraded, but `flush_error_` was still set.

A batch had passed admission on the healthy state and crossed the rotation
threshold. It waited in the rotation barrier's `quiesce()` for the
nemesis's flush, which failed and degraded the engine. The barrier did not
re-check the published state, so it published a state built on the
pre-failure head and cleared the degrade. From then on, every `commit_wait`
rethrew the stale `flush_error_` without flushing, and nothing was
published again.

The chaos soak had found the same bug independently, and #170 fixed it
while this harness was being written ("pipeline: rotation behind a failed
flush stays degraded" in `tests/bytecask_test.cpp`). On a base that
includes #170, the failing seed completes in every run. Two harnesses
built differently reaching the same failure is some evidence that each of
them reaches this part of the engine.

**An acknowledged write was invisible to other threads for a moment.** The
first CI run with Elle reported `G-single-item-realtime` in a `guarded`
history, which was otherwise valid. A transaction invoked after another had
returned `:ok` read the key from before that write.

Reads go through a per-thread cache that refreshes when `state_time_`
changes, and a publication stores `state_` and then `state_time_`. With the
commit pipeline, the thread that publishes is whichever holds the flush
role. A writer polling in `commit_wait` could see its write covered in
`state_` and return between those two stores. Until the publisher stored
`state_time_`, every thread's cached state predated the write. The engine
already covered the writer's own thread by seeding its cache, but not any
other thread. `CONTRACT.md` promises that a successful write is visible to
subsequent reads.

`commit_wait` now advances `state_time_` before it returns a covered write,
which replaces the seeding. The regression test is "pipeline: a write
another thread published is visible to every thread once put returns, even
before state_time_ is stored". It failed 3 of 3 runs without the fix. The
cross-check gained the real-time rule above. With it, the pre-fix binary
failed in round 3 of a local run, and the fixed one passed 16 rounds on the
same seed.

The replication check later found the reader-to-reader version of the same
gap: one reader saw an in-flight write, and a later reader did not. Session
reads now use a publication counter and a generation instead of the
timestamp, which also replaces the `state_time_` advance in `commit_wait`
(see [`replication_checking_design.md`](replication_checking_design.md),
*Findings*).

## Acceptance

- `guarded` is valid under `strict-serializable` on every nightly run.
- `unguarded` is valid under `snapshot-isolation` and reports only
  `G2-item` variants under `strict-serializable`, at least once per run.
  `blind` is invalid.
- The commit-sequence cross-check passes in all three configurations.
- `guarded` range histories pass `history_check` on every nightly run, and
  `unguarded` ones show at least one stale read per run.
- The seed is printed, and a failure uploads the history.
- The README's isolation paragraph and `transaction_design.md` cite this
  check.
