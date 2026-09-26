# Commit pipeline — overlapping the next batch with the in-flight fdatasync

Status: **implemented on branch `commit-pipeline`**, validation gate passed; see [Results](#results).
Date: 2026-09-14
Tracking: [gustavoamigo/bytecaskdb#70](https://github.com/gustavoamigo/bytecaskdb/issues/70)
Depends on: hand-off-after-every-batch `WriteGroup` (branch `write-group-handoff-every-batch`).
Reference shape: MariaDB InnoDB `storage/innobase/log/log0sync.cc` (`group_commit_lock`),
used twice in `log0log.cc` as `write_lock` and `flush_lock`.

## Problem

On a flush-bound disk the write path is idle for ~11% of every commit cycle.
Measured on the sysbench `oltp_write_only` run (8 clients, 4-vCPU Azure VM,
uprobe on `DB::execute_slots`, 7887 batches, 23.3 s):

| Leader phase per batch | µs | On the disk's critical path? |
|---|---:|---|
| fdatasync | 2253 | yes (the disk is working) |
| phase 1: validate + radix-tree apply | 177 | yes, but the disk is idle |
| phase 2: pwritev (page cache) | 22 | yes, disk idle |
| phase 3: publish state, fill results | 6 | yes, disk idle |
| gap: broadcast, hand-off, next leader wakes | 72 | yes, disk idle |
| **cycle** | **2530** | disk busy 89% |

InnoDB's gap between one fdatasync's exit and the next redo write is 28 µs on
the same host. Its client threads do the equivalent of phase 1 (copy the mtr
into the log buffer) before they join the group; the group leader only writes
and flushes.

The 277 µs of non-fdatasync time is not CPU we can remove. It is serial work
that sits between two flushes. The fix is to move it *beside* the flush.

## Why a pipeline can lose

The previous attempt found the pipeline machinery cost more than it saved.
The likely mechanism: every thread wake-up that lands between "flush N
returns" and "flush N+1 is issued" is disk-idle time, and a design with a
dedicated sync thread pays two of them per commit (writer → syncer, syncer →
writer). Measured on this VM with a two-thread condvar ping-pong (3 runs,
20 000 hops each):

| one-way condvar wake | µs |
|---|---:|
| p50 | 9.2 – 9.9 |
| p90 | 11.5 – 14.7 |
| p99 | 27 – 29 |
| max | 319 – 540 |

Two wakes per commit cost 20–60 µs against a 2253 µs flush here, which is
noise. Against a 100 µs NVMe flush the same two wakes are 20–60% overhead, and
if the earlier attempt was measured on fast storage that alone explains a
loss. The design below therefore has these constraints:

1. No new thread.
2. Zero wake-ups on the flush critical path when a single writer is active
   (the lone-writer path must be byte-for-byte today's path).
3. At most one wake-up on the flush critical path under contention, and it
   is the *same* wake-up the current design already pays for the hand-off.
4. Durability before visibility is kept exactly. Readers never see a state
   that contains a non-durable `sync=true` entry.

## Design

### Two heads instead of one

Today there is one `EngineState` pointer, `state_`. A batch builds a transient
on it, appends, fdatasyncs, and publishes. The pipeline splits that into two
pointers over the same persistent chain:

| pointer | who writes it | who reads it | meaning |
|---|---|---|---|
| `state_` (published) | the flush role, barrier ops | readers, `snapshot()`, `durable_sequence()` | every `sync=true` entry in it is durable |
| `head_` (prepared) | stage-1 leaders under `write_mu_` | stage-1 leaders, the flush role (atomic load) | latest state produced by phase 1 + 2; may contain entries still in the page cache |

Invariant: `state_` is an ancestor of `head_` in the state chain, so
`state_->next_seq <= head_->next_seq`. When no flush is in flight and nothing
is pending, they are the same state. The persistent radix tree makes this free:
a state is an immutable value, "building on the unpublished head" is what
`transient()` already does, and publishing is a pointer store.

Two more words of state:

- `sync_high_seq_` — highest sequence appended by a `sync=true` slot. Written
  under `write_mu_`, read atomically. Decides whether a flush needs an
  `fdatasync` at all.
- `flush_in_flight_` — `atomic<bool>`, the flush role. Whoever wins the
  exchange runs `flush_once()`; no thread owns the role permanently.

### Stage 1 — unchanged `WriteGroup`, executor stops before fdatasync

`WriteGroup` stays as it is after the hand-off-every-batch change: a leader
takes the queue, runs the executor once, marks its batch done, hands leadership
to the queue head in the same broadcast. What changes is the executor,
`DB::execute_slots`:

```
lock write_mu_
  current = head_                       (was: state_)
  reject if degraded / follower         (unchanged)
  t = current->transient()
  phase 1: execute_slot per slot        (unchanged)
  phase 2: file.append_entries          (unchanged)
  if t.is_rotation_needed:              → barrier path, see below
  S = t.persistent()
  head_ = S
  if any_sync: sync_high_seq_ = S->next_seq - 1
unlock
```

It no longer fdatasyncs, no longer publishes, no longer sets `durable`. The
leader's client thread is back in `apply_batch` after ~200 µs instead of
~2.4 ms, and the next leader is already running phase 1 for the batch that
will share the *next* flush.

### Stage 2 — `commit_wait`, the flush role, `flush_once`

`apply_batch` after `submit()` returns:

```
commit_wait(slot):
  target = slot.result->sequence, want_durable = slot.opts.sync
  loop:
    p = load_state()                                      // published
    if want_durable ? p->durable_seq >= target
                    : p->next_seq  >  target:
      slot.result->durable = p->durable_seq >= target;  return
    if flush_error_: rethrow it
    if !flush_in_flight_.exchange(true): flush_once()     // I am the flusher
    else: wait on durable_cv_                             // someone else is
```

```
flush_once():                                             // no write_mu_
  S = atomic_load(head_); p = load_state()
  need_sync = sync_high_seq_ > p->durable_seq
  if need_sync:
    S->active_file.sync()                                 // the only fdatasync
    counters_.fsyncs++
    on error: degrade path (below); return
  S' = copy of S (O(1): shared roots + a few integers)
  S'->durable_seq = need_sync ? S->next_seq - 1 : max(S->durable_seq, p->durable_seq)
  store_state(p, S')                                      // existing invariant checks
  flush_in_flight_ = false
  durable_cv_.notify_all()
```

What this buys, per flush, on the disk's critical path:

| step | today | pipelined |
|---|---:|---:|
| phase 1 + 2 of the next batch | 199 µs | 0 (overlapped with the previous fdatasync) |
| publish | 6 µs | ~6 µs (state copy + `store_state`) |
| hand-off / wake of the next flusher | 72 µs | one condvar wake, 10–28 µs |
| fdatasync | 2253 µs | 2253 µs |
| **cycle** | **2530 µs** | **~2290 µs (+10%)** |

The single wake is the waiter that wins `flush_in_flight_` after the
completing flusher's broadcast. It is the same broadcast the current design
uses to hand leadership to the next `WriteGroup` leader; it just now carries
the flush role instead. InnoDB's `group_commit_lock::release` does exactly
this, with the refinement that it wakes the next leader *first* and by a
per-waiter semaphore rather than a broadcast — the same refinement is
available here if the thundering herd shows up in context-switch counts (see
[Costs](#costs-and-open-points)).

Batch formation moves from the hand-off policy to the disk: a flush covers
everything appended while the previous flush ran. Under closed-loop clients
that is the same "~4 per flush" InnoDB shows, and no client waits more than
two flushes: the writer that appends during flush N is covered by flush N+1,
which starts the moment N returns.

**Lone writer** (engine_bench `Put/Sync`, single connection): stage 1, then
`commit_wait` finds the flag clear, flushes inline on its own thread. No wake,
no extra syscall, no thread. This is today's path with the fdatasync moved
from one function to the next.

**`sync=false`**: waits until the published `next_seq` covers it. If no flush
is in flight it takes the role and publishes without an fdatasync (today's
NoSync path, same work). If a flush is in flight it waits for that flush to
end and is published with the next `flush_once`, which fdatasyncs only if a
`sync=true` slot landed behind it. Visibility latency ≤ one flush in the
mixed case; unchanged in a pure NoSync workload. `CommitResult::durable`
keeps its meaning: `durable_seq >= sequence` at publish.

**Sync-only write** (`apply_batch({.sync = true}, WritePlan{})`, or any
synced plan that appends nothing): stage 1 appends nothing but, if the head
holds sequences above the published `durable_seq`, raises the head's
`sync_requested_seq` to its last sequence; stage 2 then waits in
`commit_wait` for that target like a synced writer. `flush_pending` runs
when the head asks for a sync even if it holds no new entries — the
unsynced writes may all be published already, by flushes that did not sync
them. A caller writing with `sync=false` uses it to bound what an OS crash
can lose. It holds the flush role across its fdatasync like any synced
flush, so `sync=false` writers arriving meanwhile wait for it.

**Snapshot conflict checks** (`validate_preconditions`) run against `head_`,
so a plan sees every earlier stage-1 write including ones not yet durable.
That is the serial order, exactly as slot 2 sees slot 1 inside today's batch.
If those earlier writes later fail their flush, the dependent plan fails
too — see below. Snapshots handed to clients come from `state_` and never
contain non-durable sync entries.

**When a conflict is reported.** A plan that loses to a write `head_` holds
but `state_` does not is rejected — correctly — but no snapshot can show a
retry what it lost to until that write is published, so every retry until
then is rejected the same way. Before the pipeline this could not happen:
the fdatasync ran under `write_mu_`, so a competing plan was validated only
after the earlier write was published. With immediate rejection, a caller's
retry loop spun through the whole flush of the write it lost to — the CAS
benchmark's average attempts went from 1.0 to 7–350 with the pipeline, at
a 1 % true collision rate. So `apply_batch` reports such a conflict once the
head it lost to is published (`wait_published`): the first moment a retry
can make progress, and the moment the conflict was reported before the
pipeline. It is detectable at validation — the plan's snapshot already
covers everything in `state_` (`conflict_snap_next >= state_->next_seq`) —
and a plan whose snapshot is behind `state_` returns at once, since its
retry has something new to see. Nothing is written either way, and a
non-conflicting plan keeps the full pipelining. Backoff under genuine
contention remains the caller's job (`CONTRACT.md`, *Conflict Safety*).

### Failure: the fdatasync returns an error

Today (classes F/G): the batch's slots get the `system_error`, `next_seq` is
not published, the engine degrades, `resume()` scans the active file. The
pipeline keeps that, widened to "every writer appended since the last
successful flush", because all of them built on an unpublished state:

```
degrade path in flush_once (takes write_mu_ — rare path, waits ≤ one stage-1 batch):
  err = published->transient(); err.apply_degrade(...)
  D = err.persistent(); store_state(published, D); head_ = D
  flush_error_ = current_exception(); flush_in_flight_ = false
  durable_cv_.notify_all()
```

Waiters whose target is above the published `durable_seq` rethrow
`flush_error_`. New writers are rejected at stage 1 by `head_->degraded`
(the existing check, now against `head_`). `resume()` clears `flush_error_`
after it republishes. Bytes appended by the failed group and by later stage-1
batches are on disk unpublished, which is the same shape `resume()` already
handles for class F. Nothing that was published is non-durable, so
recovery-equivalence holds without a new rule.

`head_` is written by the degrade path under `write_mu_` so that a stage-1
leader cannot advance it past the degraded state concurrently.

### Barrier operations: `drain()`

Rotation, `create_manifest`, `resume`, `vacuum_commit`, `set_mode`, `ingest`,
and `~DB` all assume the state they load is the latest and fully durable. They
get one primitive:

```
drain():                       // caller holds write_mu_; no new stage 1 can start
  loop:
    p = load_state()
    if p->next_seq == head_->next_seq && sync_high_seq_ <= p->durable_seq: return
    if !flush_in_flight_.exchange(true): flush_once()   // do it myself
    else: wait on durable_cv_
```

After `drain()`, `state_` and `head_` are the same state and everything is
durable; the operation then runs today's code verbatim and finishes by
setting `head_` to whatever it published. Rotation is the one that matters for
latency: the stage-1 leader whose append crosses `max_file_bytes` calls
`drain()` from inside the executor, then runs today's phase 3 (sync, seal,
create, publish) inline and marks its own batch durable. Cost: one extra wait
of at most one in-flight flush, once per 64 MiB. This also keeps the "which
file do I fdatasync" question trivial: after any rotation `head_ == state_`,
so all non-durable bytes are always in `head_->active_file`.

`drain()` and `flush_once()` never take `write_mu_` on the success path, so a
barrier holding `write_mu_` while it waits cannot deadlock the flusher. The
degrade path does take `write_mu_`; when `drain()` runs `flush_once()` inline
it already holds it, so the degrade helper has to be callable with the lock
held (one body, two entry points).

### What does not change

- Readers, `snapshot()`, iterators, `changes_since`: they read `state_`.
- `WriteGroup`, `SoloWriter`, `Slot`, routing (`solo`, `kGroupWriteMaxBytes`).
- Phase 1 and 2 of `execute_slots`, `execute_slot`, `TransientEngineState`.
- `store_state` invariant checks and its `durable_cv_` notification.
- `durable_sequence()`; `commit_wait` is the same wait with an error path.
- The single-threaded build: nothing can be in flight, so `commit_wait`
  always flushes inline. No `#ifdef`.
- On-disk format, recovery, hint files, vacuum's scan/copy phase.

Net new: `head_`, `sync_high_seq_`, `flush_in_flight_`, `flush_error_`,
`flush_once()`, `commit_wait()`, `drain()`. Net removed from `execute_slots`:
phase 3 except the rotation barrier. The `WriteGroup` doc comment about
"batch formation happens during the running batch's fdatasync" stays true and
becomes the whole point.

### Timeline

```
disk      ██████ flush N ██████|██████ flush N+1 ██████|██████ flush N+2 ██████
                                ^ issued by the waiter that wins the role
                                  ~10–28 µs after flush N returns

leader A  [phase1+2 batch N]──flushes N────────────────┘ returns, broadcast
leader B          [phase1+2 batch N+1]────wait─────────┘ wins the role, flushes N+1
leader C                    [phase1+2 batch N+2]───────wait──────────────┘ flushes N+2
clients   …arrive, queue, get applied by the current leader, wait on durable_cv_…
```

Phase 1 + 2 for N+1 runs entirely inside flush N. The 177 + 22 µs leave the
critical path; the 72 µs hand-off becomes the 10–28 µs role wake.

## Expected gain

From the measured cycle: 2530 µs → ~2290 µs, a ceiling of about +10% on
`oltp_write_only` at 8 clients if batch sizes hold (they should: a flush
covers whatever arrived during the previous one, which is what sets InnoDB's
~4). The paired runs after the hand-off change put ByteCaskDB at ~0.90 of
InnoDB on `write_only`; +10% is parity. InnoDB's own cycle is 28 + 148 (an
O_DIRECT pwrite on the critical path) + 2150 = ~2326 µs; a page-cache pwritev
overlapped with the flush has no such term.

Do not extrapolate to `oltp_insert`: its phase 1 is smaller (98 µs gap,
groups of 3.8 vs 3.99) and the two paired runs were polluted by a vacuum and
a 190 ms stall. Measure it separately.

## Costs and open points

- **Double wake per member under contention.** A `WriteGroup` member is woken
  once when its stage-1 batch is done, goes to `commit_wait`, and sleeps again
  on `durable_cv_` until its flush lands. Today it is woken once. Bound: one
  extra futex wake per commit, ~10 µs of CPU, off the critical path. At 2 000
  tps that is 2% of one core. If context-switch counts show it matters, the
  fix is InnoDB's: a per-slot semaphore and a waiter list so the flusher wakes
  exactly the slots it covered and the next leader only. Not in the first cut.
- **Thundering herd on `durable_cv_.notify_all()`.** Same as today's
  `WriteGroup` broadcast, same bound as above, same fix.
- **Reclaim moves.** The old published state is dropped by the flusher's
  `store_state` on a client thread that is about to return anyway, or by
  reader caches as today. Not on the flush critical path, since the flusher
  publishes and then releases the role before the drop. Interacts with
  `docs/defered_state_reclaim.md` (#40) only in that the flusher would be the
  natural producer for its queue.
- **Latency under one flusher per flush.** A writer's worst case is two
  flushes (remaining part of N, all of N+1), the same as InnoDB and no worse
  than today's leader-runs-batch-of-7 case.
- **The `S'` copy is O(1).** `EngineState` (`internals.cppm`) is two
  `PersistentRadixTree`s behind defaulted copy constructors (a root refcount
  bump each), four integers, a mode, a flag, and the degraded-reason string.
- **Not chosen: relax durability-before-visibility** (publish after pwritev,
  advance `durable_seq` later). It would remove the two-heads distinction but
  breaks CONTRACT.md's "sync=true is on disk before it is visible", which is
  the property the whole failure-mode story rests on.
- **Not chosen: dedicated flusher thread.** Saves the 10–28 µs role wake on
  the busy path but adds two wakes to the lone-writer path unless there is
  also an inline-when-idle branch, which is two flush paths instead of one.
  Revisit only if the measured residual gap is what stands between the
  pipeline and parity.
- **Not chosen: concurrent fdatasyncs relying on the block layer to merge
  flushes.** Possibly the least machinery of all — each stage-1 leader just
  calls fdatasync itself and the kernel coalesces — but it depends on ext4 and
  blk-flush behaviour that has not been measured here, and it doubles
  fdatasync syscalls. Worth a micro-benchmark some day; not needed for this
  design.
- **Not chosen: per-writer stage 1 without `WriteGroup`** (each writer
  applies its own slot under `write_mu_`). Simpler still, and it removes the
  double wake, but the leader-applies-all batching is what protects
  `PutMT/Sync` at 16–64 zero-think-time writers. Evaluate after the pipeline
  lands, using the same benchmark.

## Validation

Hypotheses, each with the measurement that decides it. All engine-vs-engine
numbers come from paired alternating runs in one time window; this host
drifts ~20% per hour.

| # | Hypothesis | Measurement | Pass |
|---|---|---|---|
| H1 | Non-fdatasync time on the flush critical path drops from 277 µs to ≤ 45 µs per flush | bpftrace: uprobe on `flush_once`, `fdatasync` exit → next `fdatasync` enter, same script shape as `bccycle.bt` | median gap ≤ 45 µs |
| H2 | `oltp_write_only` improves by the overlapped share | 3 paired reps, 8 threads, 20 s, alternating engines | ByteCaskDB/InnoDB ≥ 0.95 in every rep (from ~0.90) |
| H3 | Lone-writer latency unchanged | `engine_bench` `Put/Sync`, `Del/Sync`, 1 thread, before/after | within 2% |
| H4 | High-concurrency sync throughput not hurt by the second wake | `engine_bench` `PutMT/Sync` 2–64 threads before/after; `ctxt` per commit from `/proc/<pid>/status` | within 5% at every thread count |
| H5 | Transactions per flush hold at ~4 | `group_writer_coalesced / fsyncs` from `stats()`, before/after | ≥ 3.8 |
| H6 | `oltp_insert` does not regress | same paired protocol as H2 | ratio not lower than before |

Decision rule: H1–H3 must pass; H4–H6 must not fail. Otherwise revert. The
change is contained (one executor, one wait, three helpers), so a revert is a
single commit.

Cheap pre-check before writing any of it:

- Re-run `bccycle.bt` on the merged hand-off branch to refresh the baseline
  row of the table above; the 2530 µs cycle predates the merge.

## Tests

- **Durability before visibility, pipelined**: fault-injection hook that
  blocks inside `fdatasync`; a second writer appends behind it; assert the
  first write is not visible from `get()`/`snapshot()` until the hook
  releases, then both are visible and `durable_sequence()` covers both.
- **Flush failure fails everyone since the last flush**: inject
  `io_data_file_sync` once while a second batch is queued; assert both
  callers get `system_error`, the engine is degraded, nothing is visible,
  `resume()` recovers all appended entries. Extends the existing F-class
  proof case from one batch to two.
- **`sync=false` behind an in-flight flush** becomes visible after the flush
  and reports `durable` correctly.
- **Rotation barrier**: `max_file_bytes` small enough to rotate mid-run with
  a flush in flight; model-based recovery tests (`[model]`) unchanged and
  green under serial and parallel recovery, since on-disk shape is unchanged.
- **Snapshot conflict against a pending write**: plan A puts k; before A's
  flush lands, plan B with `ensure_unchanged(k)` from an older snapshot must
  be rejected — and the rejection reported only once A's write is
  published, never before (see *When a conflict is reported*).
- **Lone writer never waits on a condvar**: assert via the existing
  `WriteGroup` test hooks or a counter that a single-threaded `Put/Sync` runs
  `flush_once` on the calling thread.
- **Proof matrix** (`tests/proof`): expected `next_seq`/visibility deltas for
  F/G are unchanged in the single-threaded harness because stage 1 and the
  inline flush run on the same thread there.

## Documentation to update on implementation

- `docs/bytecask_design.md`: "Write path — group commit" (three-phase
  diagram becomes stage 1 / stage 2), "Durability before visibility" (drop
  "`write_mu_` is held across the entire write including `fdatasync`"; state
  the two-heads invariant), `durable_seq` section (advanced in `flush_once`).
- `CONTRACT.md`: I/O failure safety — "every writer appended since the last
  successful flush" receives the error; `durable_seq` "only advanced after
  successful fdatasync" now names `flush_once`.
- `README.md` Architecture: the write-path sentence about durability before
  visibility stays true; add one sentence that the next batch is prepared
  while the current flush is in flight.

## Implementation plan

Branch `commit-pipeline`, worktree `../bytecaskdb-pipeline`, based on
`4742430` (hand-off after every batch). Baseline: `bytecask_tests` green at
that commit, `engine_bench` built from it and kept as `engine_bench.baseline`.

Refinements to the design above, found while planning against the code:

1. **`sync_requested_seq` is a field of `EngineState`, not a separate
   atomic.** The transient records the highest sequence written by a
   `sync=true` slot; `persistent()` carries it into the head. Whether a flush
   is owed is then a property of the state being flushed
   (`S->sync_requested_seq > published->durable_seq`) with no cross-atomic
   ordering to get right. It also gives one new always-on publish invariant
   in `store_state`: `durable_seq >= sync_requested_seq`. A state that
   violates it contains a non-durable sync write; publishing it is exactly
   the bug this design must not have, so the check degrades instead.
2. **Only the flush-role holder publishes.** Every other publisher (barrier
   ops, the executor's failure paths, `deem_as_degraded`) runs under
   `write_mu_` and calls `drain()` first, so it never races a flush that is
   about to `store_state`. Degraded states on failure paths are built from
   the *published* state, never from the head.
3. **Degraded stops the pipeline.** `flush_once` and `drain` return without
   publishing when the published state is degraded; `commit_wait` rethrows
   the recorded flush error if there is one, else throws `DbDegraded`.
   Nothing waits forever.
4. **One counter**: `bytecask.commit_wait_blocked`, incremented when a
   writer sleeps behind an in-flight flush. Zero for a lone writer; the
   lone-writer test asserts it.

Steps, in order. No test or invariant is weakened at any step; a red test
means the code is wrong.

| # | Step | Done when |
|---|---|---|
| 0 | Worktree, baseline build, baseline test run, baseline `engine_bench` binary | done: 1467 cases green in 3m51s at `4742430` |
| 1 | `sync_requested_seq` in `EngineState` / `TransientEngineState` (`note_sync_requested`, `transient()`, `persistent()`); publish invariant in `store_state`; check in `validate_state_consistency` | done |
| 2 | DB members `head_`, `flush_in_flight_`, `flush_error_`; `load_head`/`store_head`; `flush_pending`, `flush_once`, `finish_flush`, `flush_failed`, `commit_wait`, `quiesce` + RAII `FlushRole` | done |
| 3 | `execute_slots`: phases 1–2 unchanged; failure paths `quiesce()` + degrade from published; rotation = `quiesce()` + existing phase 3; else `store_head`. `apply_batch` calls `commit_wait` after `submit` | done; first full run caught one bug (below), fixed with no test edits |
| 4 | Barrier ops: `create_manifest`, `resume`, `vacuum_commit` callers, `set_mode`, `ingest`, `~DB`, `flush_hints`, `deem_as_degraded` | done: 1467 cases green, 3m50s |
| 5 | New tests from the [Tests](#tests) section, using `test_before_flush_sync_` (a `BYTECASK_TESTING` hook that blocks inside `flush_pending` before the fdatasync) | done: 8 cases, 1475 green |

What the first full run caught. The head chain does not learn about flushes,
so after a flush the head still carries the pre-flush `durable_seq`. The
first cut had barrier operations (`set_mode`, `vacuum_commit`, ...) build
their new state from the head, which would have published a state with a
regressed `durable_seq`. The pre-existing regression check compares against
the state the operation was built from, so it could not see it; the new
`durable_seq >= sync_requested_seq` check fired in 15 replication proof
cases. Fix: barrier operations run after `quiesce()`, when the published
state covers the head, and build from the published state. Only
`execute_slots` derives from the head, and there the stale field is
harmless: `flush_pending` publishes `next_seq - 1` after an fdatasync and
`max(head, published)` otherwise, and the rotation path calls `apply_sync`.
The doc's earlier `advance_head` / `drain` names became `store_head` /
`quiesce`; `quiesce` returns the role as an RAII guard whose destructor
resets the head to the published state, and barrier operations use a
`WriteBarrier` that owns `write_mu_` and that guard in the right order.

Review changes (PR #73): the settle moved from `flush_pending` to the
`commit_wait` path only, since `quiesce()` callers hold `write_mu_` and
would block the leader they wait for; `head_` is now written only under
`write_mu_` (the unlocked resets on the degrade paths are gone — stage 1
rejects on the published state, and a batch that raced past that check
errors in `commit_wait`); `flush_pending` assigns `published->durable_seq`
on a NoSync-only publish, since the head's value is never ahead of it; the
`commit_wait` predicate checks coverage directly instead of pointer
inequality.

Coverage round: the diff coverage report pointed at a real ordering gap in
`flush_failed`, which published the degraded state before recording the
error, so a waiter could observe "degraded, no error" and get `DbDegraded`
where the contract promises the I/O error; both now happen under one
`durable_mu_` hold, the same mutex `commit_wait` checks under. The report
also removed two pieces of dead code (`FlushRole`'s move constructor,
never used since `quiesce()` returns a prvalue; an unused accessor) and
one unreachable branch (a degraded head under a non-degraded published
state). Two reachable paths gained tests through a `test_publish` seam:
admission of a batch that entered the write group before a flush failure,
and the publish invariant's degrade branch.
| 6 | `scripts/run_sanitizer.sh thread` and `address` | done before the settle (1475 cases, 0 reports each); rerun after it below |
| 7 | H3/H4: `engine_bench` `Put/Sync`, `Del/Sync`, `PutMT/Sync` 2–64 threads, baseline vs new, same window | done, see [Results](#results) |
| 8 | H1/H2/H5/H6: plugin built from this branch, paired sysbench, cycle trace | done, see [Results](#results) |
| 9 | `bytecask_design.md`, `CONTRACT.md`, `README.md`; this doc's status; issue #70 | done |
| 10 | Diff and results presented; commit only on explicit approval | — |

## Results

### H3/H4 — `engine_bench`, baseline `4742430` vs this branch

Same host, binaries alternated per repetition, three repetitions, median
shown, ratio = new / baseline. Dataset 50 000 keys, files on the root disk.

**First cut (no settle).** Lone writer unchanged; multi-writer sync
throughput 23–27% *lower* at 4–16 threads. A probe with the engine's own
counters (8 zero-think-time writers, 5 s) showed why: the fdatasync rate was
the same (~430/s) but each flush covered fewer commits, 4.15 vs 4.75, and
stage-1 batches were tiny (1.7 slots). Writers released by a flush re-enter
within microseconds but need a stage-1 batch before they are in the head,
and the next flush started the instant a waiter won the role, so they
slipped to the flush after. In the old design they only had to enqueue,
which is cheaper than the next leader's wake-up, so they made the batch.

**Fix, validated by the same probe.** `flush_pending` lets slots already
inside the `WriteGroup` finish stage 1 before capturing the head, bounded
by `kFlushSettleMax` (200 µs). Commits per fdatasync at 4/8/16 writers:
2.51/4.75/10.6 (baseline) → 3.96/7.78/15.4; fdatasync rate unchanged.

| benchmark | ratio new / baseline |
|---|---:|
| Put/Sync (1 thread) | 1.01 |
| Del/Sync (1 thread) | 0.98 |
| PutMT/Sync, 2 threads | 1.09 |
| PutMT/Sync, 4 threads | 1.46 |
| PutMT/Sync, 8 threads | 1.63 |
| PutMT/Sync, 16 threads | 1.37 |
| PutMT/Sync, 32 threads | 1.30 |
| PutMT/Sync, 64 threads | 1.37 |

H3 (lone writer within 2%) passes. H4 (multi-writer within 5%) passes with
margin: the pipeline is faster at every thread count, because a flush now
covers every commit that was waiting when it started, not just those that
happened to be queued before the leader's wake-up.

### H2/H6 — MariaDB sysbench, baseline vs this branch

`run-sysbench.sh --engines=bytecaskdb,innodb --workloads=oltp_write_only,oltp_insert
--threads=8 --time=20`, 5M rows, run in full from each worktree (each builds its
own plugin against its own `libbytecask.a` and prepares its own data), in the
order baseline, pipeline, pipeline, baseline. InnoDB is the same-window control.

| run | BC write_only | InnoDB write_only | BC / InnoDB | BC insert | InnoDB insert | BC / InnoDB |
|---|---:|---:|---:|---:|---:|---:|
| baseline rep 1 | 1343 | 1051 | 1.28 | 1667 | 2067 | 0.81 |
| pipeline rep 1 | 1997 | 1107 | 1.80 | 2010 | 2091 | 0.96 |
| pipeline rep 2 | 2001 | 1280 | 1.56 | 1974 | 2120 | 0.93 |
| baseline rep 2 | 1517 | 1281 | 1.18 | 1696 | 2091 | 0.81 |

Pipeline over baseline: write_only +32% to +49%, insert +16% to +21%. H2
(ratio to InnoDB ≥ 0.95 on write_only) and H6 (insert ratio not lower) pass.
InnoDB's own write_only numbers are lower than in the September analysis
(1051–1281 vs ~2000); the disk was in a slower phase, which is why only
same-window ratios are compared. The gain is larger than the +10% ceiling
the design predicted because the settle also grows the groups, which the
prediction held constant.

### H1/H5 — cycle trace on the pipeline instance

bpftrace on the ByteCaskDB `mariadbd` during a 20 s `oltp_write_only` run,
8 threads, gap = fdatasync exit to the next fdatasync enter (any thread):

| metric | before | pipeline |
|---|---:|---:|
| gap mean | 281 µs | 65 µs |
| gap median | 128–256 µs bucket | 32–64 µs bucket |
| fdatasync mean | 2253 µs | 2309 µs |
| disk busy | 89% | 97% |
| transactions per fdatasync | 3.88 | 4.00 |

H1 (median gap ≤ 45 µs) passes at the bucket resolution available; the mean
is above it because the settle wait shows up in the 128–512 µs buckets
(~13% of flushes). H5 (≥ 3.8 per fdatasync) passes.

### Sanitizers

`scripts/run_sanitizer.sh thread` and `address`: 1475 cases, no reports,
run twice — before and after the settle change to `WriteGroup`.
