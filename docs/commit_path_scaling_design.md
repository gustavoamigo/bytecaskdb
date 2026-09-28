# Commit path scaling: the serial section at 48 cores

Status: investigation done, plan proposed. Phase 1 is a series of changes to
`main`; phase 2, moving the append out of the serial section, is an epic on a
long-lived branch. The experiments behind every number here are on branch
`exp/commit-probe` (compile-time toggles, not for merging).

Reader: `docs/commit_pipeline_design.md` describes the two stages this document
changes (stage 1 under `write_mu_`, stage 2 the flush role), and
`docs/write_path_investigation.md` the earlier 16-thread investigation this one
continues.

## Problem

HammerDB TPROC-C, 96 warehouses, `--profile=fast`, on a c6id.12xlarge (48
vCPUs, 24 cores, MariaDB 10.5.29). ByteCaskDB and InnoDB are level at 14 users
and ByteCaskDB stops scaling after that:

| Users | ByteCaskDB NOPM | InnoDB NOPM |
|---:|---:|---:|
| 14 | 359,701 | 364,874 |
| 24 | 401,754 | 466,221 |
| 48 | 424,518 | 542,094 |

At 48 users the host is 62% idle. Connection threads are busy 34% of the
time, against 56% at 24 users: doubling the users adds threads that wait, not
work.

## Findings

### 1. The serial section is saturated

Timers added to the group-commit path (`exp.*` in `stats()`, branch
`exp/commit-probe`) measured it directly, with ByteCaskDB's data on a RAM disk
so the disk takes no part:

| Variant, 48 users | NOPM | Commits/s | Commits per batch | Executor busy | Handoff gap |
|---|---:|---:|---:|---:|---:|
| A: `main` (leader hand-off, `notify_all`) | 426,984 | 15,248 | 17.1 | 93% | 7% (80 µs per batch) |
| B: targeted wake of the next leader | 428,466 | 15,354 | 15.9 | 94% | 6% (66 µs) |
| C: dedicated committer thread | **451,202** | 16,144 | 14.6 | **97%** | 0 |

The executor, stage 1 under `write_mu_`, is busy 93–97% of wall time at
**~60 µs per commit** in all three variants. One section running one commit at
a time at 60 µs caps commits at ~16,600/s, which is what `Com_commit` shows
(16,707–17,686/s). Throughput is `1 / serial time per commit`; nothing else
moves it until that falls.

An earlier reading of the first capture's on-CPU profile put the executor at
half occupancy. That was wrong: perf copies 8 KiB of user stack per sample,
MariaDB's connection threads are deeper than that, and samples inside
`WriteGroup::lead` lost their frames. The committer thread in variant C has a
shallow stack, so its profile is the reliable one; the timers agree with it.

### 2. What the 60 µs are

On-CPU profile of variant C's committer thread, which does all the serial work
(98.8% CPU):

| Part | Share of serial time |
|---|---:|
| Engine code | 49% |
| &nbsp;&nbsp;`apply_writes`: inserts (`kd_put`) 12%, updates by location (`kd_put_at`) 7% | 24% |
| &nbsp;&nbsp;`validate_preconditions`: `kd_holds` 8.6%, `kd_get` 4.7% | 14% |
| &nbsp;&nbsp;CRC-32C of each record | 3.6% |
| &nbsp;&nbsp;`BufferPool::sieve_victim` (eviction during the append) | 2.2% |
| libc: `memmove` 9% (node and record copies), `memcmp` 8% (key comparisons), mutex 3.5% | 22% |
| jemalloc | 16% |
| kernel: the `pwritev` copy (~8%), wakeups | 13% |

Samples in libc, jemalloc and the kernel lost their callers (the host's libc
is stripped), but they belong to the thread that only runs the executor.

The same executor costs ~45 µs per commit in `commit_probe` (below) with 2M
keys. The box's key directory holds 65M: each descent is deeper and misses
cache more, which is why search and comparison weigh more there.

### 3. What each lever is worth

Measured with `benchmarks/commit_probe.cpp` (branch `exp/commit-probe`): 48
closed-loop threads, each transaction a snapshot, 10 reads, 10 updates and 12
inserts committed with `sync=false`, plus an empty `sync=true` commit every
second (the plugin's interval sync). Data on tmpfs, jemalloc preloaded, runs
repeat within 0.5%.

| Change | Commits/s | Serial µs per commit | Notes |
|---|---:|---:|---|
| `main` | 18,259 | 43.8 | handoff 20% of wall time, 251 µs per batch |
| Targeted wake of the next leader | 21,082 | 46.9 | handoff 10 µs |
| Committer thread | 21,246 | 45.0 | no handoff |
| + node pool | 22,491 (+5.7%) | 42.4 | |
| + both W-W checks skipped (**unsafe upper bound**) | 25,861 (+15%) | 36.5 | the ceiling for a fused descent |
| Reusing the per-batch vectors | no change | | dropped |
| jemalloc thread-cache tuning (`MALLOC_CONF`) | +0.7% | | dropped |

The append (`pwritev`, the buffer-pool copy, CRC) is 17–23% of serial time by
its timer, on the probe and on the box.

Through the full stack on the development machine (HammerDB, 70 warehouses,
35 users), the committer thread gave +6% (227,140 → 241,039 NOPM) and the
targeted wake nothing: with every core busy, even one wake of the next leader
waited ~300 µs for a CPU. The committer removes that wake from the critical
path instead of making it cheaper, which is why it is the variant to build.

### 4. Rotation stalls depend on the disk

A rotation runs two `fdatasync`s under `write_mu_`. On tmpfs a rotation costs
~2 ms; on the development machine's NVMe drive, 22 ms to 1.2 s depending on
pending writeback, up to 41% of wall time in one run. The box ran 64 MiB data
files, about one rotation a second. 512 MiB files cut the count eightfold.

## Plan

The serial section is the ceiling, so every change below is judged by one
number: serial time per commit. Variant C reached 451,202 NOPM; InnoDB 542,094.
Closing that takes about 20% less serial time per commit.

### Phase 1: on `main`, one change at a time

Each is its own PR, measured with `commit_probe` before and after and with
`engine_bench` as the repository requires; the box confirms them together in
one session.

**1.1 Committer thread.** One thread runs every batch back to back; writers
queue their slot and sleep until the batch holding it is done. It replaces the
leader hand-off in `WriteGroup`. Stage 2 (`commit_wait`, the flush role) is
unchanged.

- *Slot lifetime.* The experiment wakes a slot with `atomic::notify_one` after
  storing its final state, and the owner may already have returned: undefined
  behaviour. Either the committer owns slot storage (a ring it recycles) or
  the slot is `thread_local` to its writer, which cannot exit while waiting.
  The single-state-word design of issue #83 applies as is.
- *Idle cost.* The committer spins briefly on an empty queue before sleeping
  (the experiment: 50 µs). Measure CPU at low load and latency for a single
  writer; the 14-user run showed no regression.
- *Paths that bypass the group:* `SoloWriter`, barriers, shutdown order (the
  thread must stop before the state it executes against).

**1.2 Node pool.** B+ tree nodes come in a few sizes: blind leaves are
always 1 KiB, inner nodes one size unless a long prefix needs more
(`Node::allocate`). Retired nodes are freed on whichever thread drops the last
version that pins them, usually a reader, so jemalloc's per-thread cache on the
committer never gets them back. The experiment keeps a mutex-guarded free list
per exact capacity that any thread frees into; the allocating thread takes the
whole list when its cache is empty.

- *Sanitizers.* Recycled memory hides use-after-free from ASan. The pool must
  be off in sanitizer builds, and the memory tests that count `operator new`
  must still see node allocations.
- *Bounds.* A cap, and trimming when the pool stays idle.
- *Subtree frees* (`free_node_subtree_if`) should hand over a batch per lock,
  not a node.

**1.3 Fused validate-and-apply descent.** Today each written key is descended
twice under the lock: once to check it (`kd_holds` for an update, `kd_get` for
an insert), once to change it (`kd_put_at`, `kd_put`). `replace_at` already
changes an entry only if it still points at the snapshot's record, and the
insert descent already finds whether the key exists; the check can be the
result of the apply.

- *Atomicity.* A plan conflicts as a whole. Applying while checking means
  undoing the keys already applied when a later key conflicts: an undo list
  per plan (restore the location, erase the insert). Conflicts are rare (tens
  in a 5-minute run), so the undo path costs little on average, and it is
  where the bugs would be.
- *Tests.* A differential test like D3's (`test_resolve_by_key_`): one seeded
  workload of snapshot plans, conflicts and vacuums on two databases, fused
  and two-pass, requiring every commit, conflict, key and file stat to match.
  Then the model tests and the Elle checks.
- Range guards and plans without a snapshot keep today's checks.

**1.4 Small items.**

- CRC-32C of key and value on the caller's thread, combined with the header's
  under the lock (`crc32c_combine`): ~3.6%.
- File-stats updates accumulated per batch and applied once: ~3.4% on the
  probe.
- 512 MiB data files in `bytecaskdb-fast.cnf`.
- The group-commit timers as permanent counters in `stats()`: executor time,
  batch count and commits per batch already exist; executor busy time would
  have answered this investigation's first question at once.
- Issue #221: a short `pwrite` reports a stale `errno`.

### Phase 2: the append out of the serial section (epic)

On branch `epic/append-pipeline`, kept alive until measurements and tests say
whether it comes in.

**Why it is possible.** Stage 1 computes every record's offset from its size
(`running_offset` in `execute_slot`) before writing anything, and
`apply_writes` points the key directory at those offsets. The key directory
does not need the bytes written, only written before anyone reads them.

**Who could read them early.**

1. *Readers* see only published states. If the stage that publishes a batch
   writes its bytes first, readers are covered.
2. *The next batch's stage 1* reads records to place inserts and to check keys
   that moved. Within a batch the engine already serves unwritten records from
   `pending_` (`note_pending`). The change carries pending records from one
   head to the next until their bytes are written, then drops them.
3. *The bytes themselves* point into the writers' `WritePlan`s; a writer is
   blocked until its batch is published, so they outlive the write.

**The pipeline.**

```
committer (stage 1): validate + apply + offsets → hand batch N over → batch N+1
flush stage (2):     pwritev batch N, CRC, buffer-pool copy → fdatasync if owed → publish
```

**Invariants to keep.**

- Batches are written in sequence order, by one writer, so a later record is
  never on disk ahead of an earlier one. Recovery truncates the newest file at
  the first record that does not parse, and nothing past a gap was ever
  acknowledged.
- Durability before visibility: publish only after the batch's bytes are
  written, and after `fdatasync` when one is owed.
- A sealed file has no pending appends: the rotation barrier waits for the
  flush stage, as it already waits for a flush in flight.

**Open questions.**

- *Append failure.* Today a failed `pwritev` fails its own batch in stage 1.
  Moved, it fails in stage 2 with later batches already applied on top. The
  failed-flush path already degrades the engine, fails every write appended
  behind it and lets `resume()` trim their bytes; the append failure has to
  join that path, with fault-injection tests for it.
- *Where pending records live* between heads, and how reads find them without
  a lock (the head chain is immutable per version).
- *Buffer pool.* The active file's frames are filled at append time; reads of
  a record between apply and write must come from pending, not the pool.
- *Stage 2 as the new bottleneck.* It takes ~20% of today's serial work plus
  the `fdatasync`; one thread may need to overlap the write of batch N+1 with
  the sync of batch N.

**Tests.** The model-based recovery tests (`[model]`) with the flush stage
lagging deliberately; fault injection on the moved append; crash tests at
every point between apply and publish; the Elle isolation checks; TSan.

**Go / no-go.** It comes in if it removes at least 15% of serial time per
commit on the box, every test layer above passes, and the failure and
recovery semantics in `CONTRACT.md` are unchanged. Otherwise the branch stays
as a record.

## Measuring

- **Engine:** `commit_probe` on tmpfs with jemalloc preloaded, 48 threads,
  alternating runs of the two variants. Serial µs per commit and executor busy
  share are the numbers that matter; commits/s follows while the executor is
  saturated.
- **Full stack on the development machine:** HammerDB with `--rampup=1
  --duration=2`, ~4 minutes a cell. Its cores are all busy, so it confirms a
  change does not regress, not how much it gains at 48 cores.
- **The box:** batch every change into one session. ByteCaskDB's data on a
  RAM disk isolates the serial section from the disk; size it for the run's
  growth, not the schema (96 warehouses: 9 GB pristine, 9 GB working copy,
  ~24 GB of appends in 7 minutes), and keep the capture's working files off
  it. `--capture`'s status snapshots carry the timers.
- Profiles on the box lose frames: MariaDB's stacks outrun perf's 8 KiB copy
  and the host's libc is stripped. Trust the timers, and profile a thread with
  a shallow stack when a breakdown is needed.
