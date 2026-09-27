# Unsynced commits: one sleep, and less work under the lock

Status: D1 and D3 implemented; D2 and D4 implemented, measured and not
shipped (see *Results*). Built on `docs/write_path_investigation.md`, which
has the measurements; its targets are numbered T1–T4 below.

## Problem

An unsynced commit — `apply_batch` with `sync = false`, the MariaDB plugin's
`bytecaskdb_sync = AT_INTERVAL` — takes 450 µs through MariaDB at 16 clients
against InnoDB's 30 µs, and 16 clients reach 24% less throughput than
InnoDB. The time goes to:

- **T1, the settle wait.** The flusher yields up to 200 µs so a flush covers
  writers still in stage 1. That pays for synced flushes, which share one
  `fdatasync`; an unsynced flush has nothing to share, yet waits all the
  same. Removing it: `commit_wait` p50 86 → 3 µs, +19% throughput.
- **T2, ~35 µs of serial work per commit.** The batch runs under `write_mu_`.
  A third of it is conflict validation; half of that looks keys up in the
  transaction's immutable snapshot. Two thirds is updating the key
  directory, where each put and erase reads a record — under blind leaves,
  to find or place the key.
- **T3, two sleeps per commit.** A writer sleeps in the write group until
  its batch is applied, then again in `commit_wait` until a flush publishes
  it — even when there is nothing to flush.
- **T4, the state load.** `std::atomic_load` of a `shared_ptr` takes a mutex
  from libstdc++'s global pool; 6.9% of `engine_bench`'s CPU.

The synced path is not the target: group commit already makes it 2–7×
RocksDB. Nothing here may slow it.

## Design

Four changes, each measurable alone and each a separate PR, in this order.

### D1 — Settle only when a sync is owed (T1)

`flush_once` settles only if the flush it is about to run will `fdatasync`:
the head's `sync_requested_seq` is above the published `durable_seq`, or a
synced writer is still in stage 1. For the second, the write group keeps a
count of in-flight synced slots beside its existing `inflight_` count, so
`flush_once` can test it lock-free, as it tests `busy()` today.

An unsynced flush publishes immediately. Synced flushes keep the settle
exactly as measured. Nothing about ordering changes: settling only delays
capturing the head.

### D2 — Publication that only moves forward; the leader publishes an unsynced batch (T3)

*Not shipped* — measured within noise; see *Results*.

**The rule.** A leader whose batch appended only unsynced writes makes it
visible and returns. Its callers asked for visibility, not durability, so
nothing is theirs to wait for. The one thing that can stop it is order:
publishing the head makes visible everything appended before the batch, so
if an earlier synced write in the head is not durable yet, the batch cannot
become visible before it. Then — and only then — the batch rides the flush
in progress, as today.

**Why not a second publisher.** A first version of this change had the
leader take the flush role and publish, beside the flusher. It was rejected:
the two paths would publish independently, and keeping them in order would
rest on each checking the right flags at the right instant. The flusher, for
one, publishes the head it captured *before* its `fdatasync`; published
after a leader's newer head, it would move the state backwards. The fix is
not better coordination between publishers but an operation that cannot go
backwards whoever calls it.

**`publish` — one forward-only operation.** `state_` becomes an
`std::atomic<std::shared_ptr<const EngineState>>` (D4), and every
publication is a compare-and-swap loop against the state currently
published, `cur`:

- **A newer head** (`head->next_seq > cur->next_seq`) replaces `cur`, with
  `durable_seq = max(head's, cur's)`, if it exposes no unconfirmed sync
  (`sync_requested_seq <= durable_seq`) and `cur` is not degraded.
  Otherwise `publish` declines and the caller falls back: the leader leaves
  the batch to `commit_wait`; the flusher cannot hit this case (it publishes
  only after its `fdatasync`).
- **A head no newer than `cur`** — the flusher's pre-`fdatasync` capture,
  when a leader published past it meanwhile — is not published. Only its
  durability is kept: `cur` is replaced by a copy of itself with
  `durable_seq = max(cur's, the flush's)`. The flush made everything up to
  that sequence durable; nothing newer is exposed, and nothing goes back.
- **Degrading** (a failed `fdatasync`) replaces `cur` with a degraded copy of
  itself. `degraded` is sticky: a head published later never clears it,
  because a newer head is declined while `cur` is degraded.

If the CAS loses to a concurrent publish, `publish` re-reads `cur` and
re-decides. Every outcome keeps `next_seq`, `durable_seq`, the file ids and
`degraded` monotonic, so the order of concurrent publishers no longer
matters. `store_state`'s invariant checks, its read-cache generation bump
and its notifications move into `publish`, in one place as today.

**Who publishes.**

| Caller | Holds | Publishes | Can race with |
|---|---|---|---|
| Leader of an unsynced batch (new) | `write_mu_` | its head, via `publish` | the flusher |
| Flusher (`flush_pending`) | flush role | its captured head, or only its durability, via `publish` | leaders |
| Flush failure (`flush_failed`) | flush role | a degraded copy of `cur`, via `publish` | leaders |
| Barriers: rotation, vacuum, `set_mode`, `resume`, ingest | `write_mu_` + flush role | their state, via `publish` | no one: leaders need `write_mu_`, the flusher needs the role |

The barriers keep their exclusion; they go through `publish` only so there
is one place that publishes.

**Waking waiters.** A writer in `commit_wait` for an unsynced write is
covered once `next_seq` passes it. Today only `finish_flush` wakes it; a
leader's publish must wake it too, so `publish` notifies on `next_seq`
advancing as well as on `durable_seq`.

**What a commit costs afterwards.** An unsynced writer sleeps once, in the
write group, and not at all if it leads. Its `commit_wait` finds its write
published on the first check. A synced writer is unchanged. The leader's
publish is a state copy and a CAS under `write_mu_`: O(1), paid by the next
batch's wait for the mutex.

**Invariants kept.** Durability before visibility (checked in `publish`, as
in `store_state` today); `next_seq`, `durable_seq`, `active_file_id` and
`next_file_id` never regress (now by construction, still checked); a
degraded engine stays degraded until `resume`; publication follows append
order, because heads are built under `write_mu_` in that order and
`publish` never lets an older one replace a newer.

The per-slot state machine #83 recommended would also remove the first
sleep, the wait for the leader. While the batch is serial that wait is the
batch itself, and the serial work (D3) bounds throughput; the state machine
stays the next step if handoffs measure as the limit after D1–D4.

### D3 — Record locations as version tokens (T2)

A key's record location, `(file_id, offset)`, names one immutable record for
the life of the process: file ids are never reused (`CONTRACT.md`) and an
offset in an append-only file holds one record. Two states that map a key to
the same location map it to the same version.

**Resolve outside the lock.** Before submitting, on the writer's own thread,
`apply_batch` looks up each written key in the plan's snapshot, keeping its
location (or "absent"). Under blind leaves each lookup reads the key's
record — the reads validation does today, now in parallel across writers.

**Validate by location inside the lock.** The implicit write-write check
asks whether the key's entry in the head is the snapshot's. With a location
in hand the blind tree answers without a read: the leaf's fingerprint scan
yields the candidates, and one whose location equals the snapshot's *is* the
key, unchanged. Only when none matches — the key changed, was deleted, or
was absent in the snapshot — does it read, as today, to find which.

**Apply by location.** For a put of a key unchanged since the snapshot, and
for an erase, the entry's position is already known from the check: replace
its location in place (the crit bit and fingerprint do not change for the
same key) or remove it, without reading the record. A put of a key the
snapshot lacked — an insert — still reads a neighbour to place it; that read
is inherent to blind leaves.

In `oltp_write_only` three of four written rows are updates or a delete, so
most key-directory reads leave the serial section. For a plan without a
snapshot (plain `put`/`del`), nothing changes.

Scope: blind key directory only; the keyed trees read nothing to validate or
place a key and keep today's code. Point writes only; range deletes and
range guards keep today's checks.

### D4 — Publish the state without the global mutex pool (T4; D2's foundation)

*Not shipped.* `std::atomic<std::shared_ptr>` is not available in libc++, so
the version built mirrored the published `next_seq` and `durable_seq` into
two atomics that `commit_wait` read instead. It measured neutral (see
*Results*).

Replace `std::atomic_load`/`std::atomic_store` on `std::shared_ptr` for
`state_` and `head_` with `std::atomic<std::shared_ptr<EngineState>>`
(C++20). D2's compare-and-swap publication is built on it. libstdc++ implements it with a lock bit in the object itself, not a
process-wide pool shared with every other `shared_ptr`. If measurement shows
that is not enough, the fallback is the reader-side pattern the engine
already uses for reads (per-thread cached state, validated by generation).

## What does not change

The on-disk format, the public API, WriteGroup's batching and hand-off, the
flush role, conflict semantics (every conflict detected today is detected;
D3 only avoids reads when there is none), and the synced path's settle.

## Tests

The changes sit in the concurrency core, so a test that exercises each
mechanism once in a fixed interleaving is only the first layer. The
repository already has the harnesses that matter here; every change runs
through all of them, and each correctness risk gets a seeded bug that a
layer must catch before the change is trusted.

### Layer 1 — mechanism tests (`tests/bytecask_test.cpp`, `[pipeline]`)

Deterministic interleavings with the existing hooks (`FlushGate`,
`test_before_flush_sync_`, `on_leader_start_`), one per behaviour:

- **D1:** a flush with only unsynced writers in flight publishes without
  settling; one with a synced writer in stage 1 still settles; a synced write
  that arrives during an unsynced flush is covered by the next flush, with
  one `fdatasync`.
- **D2, `publish` itself:** each of its cases in isolation — a newer head
  replaces `cur`; a head no newer than `cur` only raises `durable_seq`; a
  head exposing an unconfirmed sync is declined; a newer head is declined
  while `cur` is degraded; a degrade replaces `cur` with a degraded copy —
  and each race between two of them, driven with hooks between the load of
  `cur` and the CAS: whichever order they land in, the published state is
  the same.
- **D2, the leader:** a leader publishes its unsynced batch and its
  followers return without blocking in `commit_wait` (`commit_wait_blocked`
  unchanged); a leader whose head holds a synced write that is not yet
  durable does not publish, and the batch stays invisible until the
  `fdatasync` returns (the existing "invisible until its fdatasync returns"
  test, with unsynced batches appended behind it); the flusher's
  pre-`fdatasync` capture landing after a leader's newer publish raises
  `durable_seq` and leaves the newer head published; a failed `fdatasync`
  racing a leader's publish leaves the engine degraded and fails every writer
  appended since the last successful flush; a writer already in
  `commit_wait` for an unsynced write is woken by a leader's publish.
- **D3:** for each shape of write — put of an unchanged key, put of a key
  changed/deleted/re-created since the snapshot, put of a key absent from the
  snapshot, erase, erase of an already-erased key, del-then-put of one key in
  one plan — the conflict outcome and the resulting key directory equal the
  current code's. Record reads (buffer-pool hits) for validating and applying
  an unchanged key are 0.

### Layer 2 — differential and model tests

- **D3 differential:** a randomised generator of plans (snapshots taken at
  random points, puts, deletes, range deletes, guards, keys shared between
  plans) applied through the location path and through today's path on two
  databases fed the same operations; after every commit, the outcome (commit
  or conflict, and `lost_to`) and the full key/value contents must match.
  Thousands of operations per seed, several seeds, all three key directories
  (the keyed ones exercise the fallback).
- **Lost updates under concurrency:** threads doing snapshot read-modify-write
  increments on a small hot key set, mixing `sync = true` and `false`; the
  final sum of every counter equals the number of committed increments.
- **Model-based recovery tests** (`[model]`): unchanged, and they must pass.

### Layer 3 — the isolation checker (Elle)

`tests/elle/isolation_history.cpp` via `scripts/run_isolation_check.py`:
concurrent list-append histories in its `guarded` (strict serializable) and
`unguarded` (snapshot isolation) configurations, its sync mixes (0%, 5%,
50%), vacuum and fault-injection nemeses, and the SIGKILL-and-reopen mode.
D3 changes exactly the write-write check the checker's model rests on, and
D2 changes when a batch becomes visible; a lost update, a dirty or
non-repeatable read, or a history that is not a prefix after a kill fails
it. Run for many seeds before and after each change, with results in the
PR.

### Layer 4 — crash consistency

`tests/crash/crash_consistency.cpp` (SIGKILL at a random point, recovered
contents a prefix of commit order covering the durable watermark), with its
sync mixes including 0% and 100%. D2 must never publish an unsynced batch
ahead of an earlier synced one that is not yet durable; the durable-prefix
check is what catches it after a kill. The harness is single-writer today;
D2 is a multi-writer interleaving, so it gains a mode with concurrent
writers (the Elle harness's kill mode covers isolation, this covers the
durable prefix).

### Layer 5 — chaos soak under sanitizers

The `[soak]` test (readers, writers, vacuum, `set_mode`, degrade and
`resume`, close and reopen at once) under TSan and ASan, for longer runs
than CI's nightly budget, before each PR.

### Seeded bugs

In the style of `tests/soak_mutations/`, a patch per failure the design
could introduce, each of which must be caught by at least one layer above —
otherwise that layer is extended first:

| Patch | The bug | Expected to be caught by |
|---|---|---|
| `publish_owing_sync` | `publish` accepts a head that exposes an unconfirmed sync | the invariant check (degrade), layers 1, 4, 5 |
| `publish_older_head` | `publish` lets a head no newer than `cur` replace it | the invariant check (`next_seq` regress), layers 1, 3 |
| `publish_clears_degraded` | a newer head is published over a degraded `cur` | layers 1, 5 |
| `publish_no_wake` | `publish` does not notify on `next_seq` advancing | layer 1 (a waiter hangs), layer 5 |
| `location_trusts_fingerprint` | D3 treats a fingerprint match as unchanged without comparing locations | layers 1, 2, 3 |
| `location_skips_absent` | D3 skips the head check for keys absent from the snapshot | layers 1, 2, 3 |
| `settle_skips_synced` | D1 skips the settle for synced flushes too | the D1 settle test (a throughput, not a correctness, bug) |

All of it runs under all three key directories where it applies, with the
full engine suite, the plugin's unit and functional suites, and CI's TSan
and ASan jobs.

## Measurement

For each of D1–D4, before and after:

- `engine_bench` in full (the repository's rule), with `PutMT/Sync` and
  `CasMT/Sync` as the regression guard for the synced path, and
  `PutMT/PeriodicSync` with and without its periodic sync as the target.
- sysbench `oltp_write_only` and `oltp_insert`, 1 and 16 threads,
  `--profile=fast`, against InnoDB: throughput, and the `COMMIT` latency by
  the same uprobes as the investigation.
- HammerDB TPROC-C, 70 warehouses, 14 users, `--profile=fast`.

Expected, from the investigation: D1 alone about +19% on `oltp_write_only`
at 16 threads. D2 and D3 are not measured in isolation yet; D3's ceiling is
the serial work it removes — the snapshot-side validation (~a fifth of the
section) plus the update and erase reads.

## Results

Measured on the machine of the investigation (Ryzen 7 3700X, SATA SSD),
each step a commit, with #202 and #197 applied for the harness. sysbench
`--profile=fast`, 60 s after 30 s of warm-up; HammerDB 70 warehouses, 14
users. One run each: steps meant to change nothing moved up to ±2.5%.

| Step (transactions/s; NOPM) | write_only 1t | write_only 16t | insert 1t | insert 16t | HammerDB |
|---|---:|---:|---:|---:|---:|
| base (`d2f62c9c`) | 6,033 | 21,544 | 22,172 | 57,557 | 198,497 |
| + D1 | 6,085 | 25,558 | 21,831 | 71,143 | 202,685 |
| + D4 | 6,050 | 25,450 | 21,887 | 70,465 | 202,911 |
| + D2a (forward-only publish) | 6,078 | 24,888 | 21,316 | 70,730 | 201,523 |
| + D2b (leader publishes) | 6,159 | 25,650 | 21,223 | 71,708 | 203,711 |
| + D3 | 6,197 | 28,358 | 21,929 | 71,338 | 218,757 |
| **D1 + D3 only (this branch)** | 6,220 | **28,318** | 22,056 | **70,490** | **220,016** |
| InnoDB, `flush_log_at_trx_commit = 2` | 5,816 | 27,742 | 21,175 | 67,636 | |

D1 and D3 carry the gain: `oltp_write_only` at 16 clients +31%, `oltp_insert`
+22%, both now ahead of InnoDB, HammerDB +11%. D4 and D2a were neutral, as
intended of a foundation and a refactor; D2b added 1–3%, within noise — with
the settle gone `commit_wait` is already ~3 µs at the median, so the second
sleep D2b removes is short. The concurrency D2 adds (two publishers, a
compare-and-swap publication) is not worth that; D4, measured on its own,
also slowed synced puts at 8 threads by ~11% until a follow-up fixed it.

`engine_bench`, speed-up over base, mean of two alternating runs, 50,000
keys:

| | D1 + D3 | with D2 and D4 too |
|---|---:|---:|
| `CasMT/NoSync`, 2 / 8 / 32 threads | 1.57× / 1.28× / 1.54× | 1.65× / 1.27× / 1.56× |
| `PutMT/PeriodicSync`, 2 / 8 / 32 / 64 threads | 1.33× / 1.08× / 0.91× / 1.16× | 1.40× / 1.15× / 0.97× / 1.21× |

At 32 threads `PeriodicSync` is noisy: an earlier pair of runs had D3 at
1.15× base. Synced puts are level from base to D3 in isolation (2 and 8
threads, every commit in between). A full-suite run is not a fair
comparison for them: their `fdatasync` latency depends on the writeback the
benchmarks before them leave, and it swung 3× between two full runs of the
same code. Unsynced plain puts at 8 threads — 0.44× RocksDB in the
investigation — gain only ~8%: they carry no snapshot for D3 to use.

## Rollout

1. **D1** — small, independent, the largest measured single effect.
2. **D4** — small; D2's compare-and-swap is built on it.
3. **D2** — `publish` first, with every existing publisher moved onto it and
   no behaviour change (measured neutral, all layers green); then the
   leader's publish of unsynced batches as its own change.
4. **D3** — the largest, with its differential test written before the
   change.
