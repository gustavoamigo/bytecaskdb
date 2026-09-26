# Replication Checking with Elle

Status: step 1 (tailing, fixed leader) and step 2 (topology) implemented
and running nightly. Tracks [#178](https://github.com/gustavoamigo/bytecaskdb/issues/178).
Extends the isolation check in [`isolation_checking_design.md`](isolation_checking_design.md).

## Purpose

[`replication_primitives_design.md`](replication_primitives_design.md)
describes a whole protocol, not just a tailing loop:

1. **Bootstrap:** `create_manifest()`, copy the files, open a follower on
   them, then tail from `follower.durable_sequence()`.
2. **Replicate:** wake on `leader.durable_sequence(follower + 1, t)`, then
   `changes_since`, then `ingest`, and restart from
   `follower.durable_sequence()` on any failure.
3. **Planned transfer:** `old.set_mode(Follower)`, wait until the target
   has caught up, then `target.set_mode(Leader)`. The design says no drain
   mode is needed.
4. **Unplanned promotion:** a follower is promoted without the old leader.
   Its sequence space continues the old leader's.
5. **Re-targeting and re-bootstrap:** the other followers tail the new
   leader. A crashed old leader is re-bootstrapped from the new one, never
   reopened in place.

It promises the clients:

- **Prefix consistency.** A follower's state is always a prefix of the
  leader's durable commit order: no gaps, no reordering, and never part of a
  batch.
- **Read-your-writes.** Once `node.durable_sequence(r.sequence, t)`
  returns at least `r.sequence`, a read on that node sees `r` and everything
  committed before it.
- **Monotonic reads.** A node never moves backwards, whether it is caught
  up, lagging, or receiving an entry twice.
- **No lost acknowledged writes on a planned transfer**, and none beyond the
  promoted follower's `durable_sequence()` on an unplanned one.

## What the existing tests cover

| Suite | Covers | Leaves out |
|---|---|---|
| `[manifest]`, `[replication]` in `bytecask_test.cpp` | each primitive once: bootstrap by copying files, `changes_since` order, idempotency, batches, range delete, promotion continuity, one round trip | concurrency: every case is single-threaded on an idle leader |
| `prove_replication` (211 cases) | one `ingest` or `create_manifest` call per case, across failure classes | more than one step of the protocol |
| `bytecaskdb-python/tests/test_replication.py` | the same primitives through the binding | concurrency |
| chaos soak | `set_mode` and `create_manifest` under load on one DB | a follower |

No test runs two nodes at once. So none covers bootstrap while writes
continue, the manifest's `through_sequence` boundary against the tail that
follows it, a transfer while clients are writing, promotion of a lagging
follower, or a follower re-targeted to a new leader.

## Cluster

One process holds three nodes (`n0`, `n1`, `n2`). Each is a `DB` in its own
directory, behind a holder, so it can be closed and reopened. A shared
**cluster view** records which node leads, which node each follower tails,
and an epoch that moves on every topology change. Everything reads the
view under a shared lock, and topology changes take it exclusively.

- **Leader clients** (the isolation harness's `guarded` list-append
  transactions) write to whichever node the view names as leader.
  - `DbFollowerMode` means the node was demoted under the client. It is
    thrown at admission, before anything is appended, so it is recorded as
    `:fail`. If that write ever becomes visible, Elle reports it as G1a.
  - Any other exception is `:info`, as in #94.
- **Replication threads,** one per follower, run Phase 2 against the node
  the view names as that follower's source. `ingest` is always given
  slices that end at a batch boundary (see *Open questions*).
- **Follower clients** run read-only transactions on a follower. A quarter
  of them are **session reads**. The client takes the `CommitResult.sequence`
  of a transaction that has already returned `:ok`, waits for
  `node.durable_sequence(seq, 1s)`, then reads. The history records the node
  and `seq` with the read.
- **The orchestrator** drives the protocol. It applies one topology event
  every 0.5–2 s, drawn from the table below.

| Event | Steps | Must hold |
|---|---|---|
| Bootstrap | pause leader vacuum; `create_manifest()`; copy the manifest files; open a follower on them; resume vacuum; start tailing from its `durable_sequence()` | the first `changes_since` after the manifest continues at `through_sequence + 1`, with no gap and no overlap |
| Planned transfer | `leader.set_mode(Follower)`; wait until the target's `durable_sequence()` equals the old leader's; `target.set_mode(Leader)`; re-target the other followers; switch the view | every write acknowledged by the old leader is on the new one; writes refused by the old leader stay invisible |
| Unplanned promotion | cut every follower off the old leader and fence it (`set_mode(Follower)`); let any ingest in flight finish; promote the follower with the highest `durable_sequence()`; re-target the others; switch the view; re-bootstrap the old leader later from the new leader | the promoted node's history is a prefix of the old leader's durable history, and no follower is ahead of it |
| Follower restart | close a follower and reopen it; resume tailing from its `durable_sequence()` | monotonic reads across the restart |
| Lag | pause one replication thread for 50–500 ms | nothing, beyond the checks below |
| Duplicate delivery | restart one `changes_since` from `durable_sequence() - k` | `ingest` skips what it already holds |

**Losing acknowledged writes on an unplanned promotion.** Replication is
asynchronous, so the old leader's writes with sequence above the promoted
node's `durable_sequence()` are lost by design. At promotion, the harness
records that sequence. The script then relabels every leader `:ok` with a
higher sequence, acknowledged in the old epoch, as `:info`. Those writes may
or may not appear anywhere, and that is correct.

**A crashed node never rejoins in place.** An abandoned leader is only ever
re-bootstrapped: its directory is wiped and filled from a new manifest, as
the design requires. The in-process "crash" is abandonment: the node stops
being addressed, with no power loss. Killing the process belongs to #176.

## What is checked

| Check | Over | Catches |
|---|---|---|
| Elle `strict-serializable` | leader operations of each epoch | the #94 claim on every leader, including a promoted one |
| Elle `serializable` | whole history, all nodes and epochs, after relabelling | fractured or out-of-order follower reads, a refused write becoming visible (G1a), a lost write that was not relabelled |
| Cross-check: convergence | after the run, all nodes caught up to the final leader | each node's final read equals the final leader's |
| Cross-check: session | session reads | a read after a successful wait on `seq` that misses a committed append with sequence ≤ `seq` |
| Cross-check: monotonic | reads on one node within one node lifetime, in real-time order | a node read older than one that completed before it began |
| Cross-check: manifest boundary | each bootstrap | a gap or overlap between `through_sequence` and the first tailed entry |

The whole history is checked under `serializable` because a follower read
may be stale. Staleness is allowed. A read that is inconsistent with some
prefix is not. The real-time guarantees the design does promise (session,
monotonic) have their own direct checks, since Elle cannot express "stale
but not older than".

## Configurations

| Config | Leader vacuum | Events | Expected |
|---|---|---|---|
| `cluster` | off | all | every check passes |
| `cluster-vacuum` | on | all, with lag; every vacuum, leader or follower, passes `retain_after` = the lowest position a serving or joining node could resume from | every check passes |
| `cluster-vacuum-unretained` | on | as `cluster-vacuum`, without `retain_after` | **detects #168** in at least one round of the run: a follower that resumes `changes_since` below a compacted file receives a batch with entries missing |
| `topology` | off | tailing, planned transfers, unplanned promotions of the most advanced follower, re-bootstrap | every check passes |
| `topology-behind` | off | as `topology`, but an unplanned promotion takes the least advanced follower | **detects the fork** below in at least one round of the run |

`cluster-vacuum-unretained` plays the role `blind` plays in #94: it shows
that the harness finds the failure retention prevents. `topology-behind`
does the same for the promotion rule: without it, the checks must find a
fork.
Once a forked node leads in turn, the fork reaches the leader history too,
so any finding counts there.

## Open questions

1. **Mid-batch `ingest` slices.** Settled: `ingest` publishes whatever
   slice it is given in one step, so a slice cut between `BulkBegin` and
   `BulkEnd` publishes half a batch. `CONTRACT.md` now states that the
   caller must cut at batch boundaries, and the harness does.
   [#188](https://github.com/gustavoamigo/bytecaskdb/issues/188) tracks
   having `ingest` hold back or refuse a trailing incomplete batch.
2. **#168's fix shape.** Settled: the replication service owns
   retention. Every vacuum takes `retain_after`, the lowest position a
   follower it counts on could resume from, and drops nothing above it.
   The harness computes it over the nodes that are serving or joining: a
   bootstrap records the manifest's `through_sequence` while it still holds
   the vacuum gate, so a vacuum between the manifest and the install counts
   the new node. An engine-side record that refused resumes below what
   vacuum dropped was tried first and rejected: it detected the gap but
   forced re-bootstraps, all at once after a promotion of a bootstrapped
   node, which lacks the record.
3. **Promotion target.** Settled by the fork finding below: the protocol
   promotes the most advanced follower, and `topology-behind`, which
   promotes the least advanced, is the sensitivity check. A random target
   forked in only about 40% of rounds, too few for a nightly assertion.

## Implementation

Two steps, so each can land green:

1. **Tailing**, implemented. Bootstrap, replicate, lag, duplicates, follower
   restart, follower vacuum and follower reads, with a fixed leader.
2. **Topology**, implemented. Planned transfer, unplanned promotion,
   re-targeting and re-bootstrap.

Step 1 as built:

- `tests/elle/isolation_history.cpp` takes `--followers N`, `--lag` and
  `--force-vacuum`. Each follower:
  - is bootstrapped from `create_manifest()` 20–220 ms into the run, while
    the leader is under load. Leader vacuum is held off from the manifest to
    the end of the copy. The follower's state at open is compared key by key
    with the manifest's snapshot.
  - is tailed by its own thread, which cuts `ingest` slices at batch
    boundaries. That thread also runs the nemeses: lag pauses of 20–200 ms,
    duplicate delivery from up to 32 sequences back, follower vacuum, and a
    restart every 100–500 ms.
  - has 4 reader threads, with 0.5–2 ms of think time. A quarter of their
    reads are session reads, after a `durable_sequence` wait of up to
    200 ms.

  The nemesis timings are shorter than the table above, because a run lasts
  about two seconds. glibc's rwlock prefers readers, so a restart raises a
  flag that makes the readers back off first. Otherwise the restart waited
  behind them indefinitely and the follower stopped tailing.

  At the end, a sync write makes every leader entry durable, and every
  follower must reach the leader's durable sequence within 20 s. Then
  every node reads every key. Every operation carries `node`, and a session
  read carries `wait`. The bootstraps and the nemesis counts go to
  `<out>.cluster.json`.
- `scripts/run_isolation_check.py --cluster` runs the leader's operations
  through the #94 checks (cross-check and Elle `strict-serializable`). It
  runs the whole history through Elle `serializable`, and through the
  prefix, session, monotonic and convergence cross-checks. It also checks
  the bootstrap records.
- `isolation-nightly.yml` runs 8 cluster rounds in each job.

Step 2 as built:

- `isolation_history --topology` starts every node as in step 1, and gives
  every node, the leader included, a replication thread and readers, so a
  node can change role without new threads. Each node sits behind a gate:
  a topology change takes it exclusively, which waits for the ingest or
  read in flight on that node. The replication thread reads the view with
  the gate held, so a re-target never races an ingest. Readers back off
  while anyone waits for the gate exclusively, and that wait is a count,
  not a flag. With a flag, a drain releasing the gate cleared it while a
  follower's restart was still waiting. Readers came back in, each held the
  gate through a session wait on data only that follower would ingest, and
  glibc's reader-preferring rwlock starved the restart for good: about one
  run in 60 ended with a follower stuck below the final leader. The degrade nemesis
  is off: a leader that steps down or is promoted is not also degraded.
- Once every follower is up, an orchestrator applies an event every
  100–400 ms, planned or unplanned with equal odds. A planned transfer
  picks a random target. An unplanned promotion is described in the table above;
  `--promote-least` inverts its choice of target. A node the
  promotion abandoned is re-bootstrapped from the new leader before the
  next event.
- Every operation carries its `epoch` and `role` (`leader`, `reader`,
  `final`). The summary records each event with the promoted node's
  `durable_sequence()` and, for a re-target, the re-targeted node's.
- At the end, abandoned nodes are re-bootstrapped, a sync write goes to the
  final leader, every node must catch up to it, and every node reads every
  key.
- `run_isolation_check.py --topology` relabels what an unplanned promotion
  lost: an `:ok` operation on the old leader in its last epoch that wrote or
  read an element above the promoted node's `durable_sequence()` becomes
  `:info` with its reads cleared. A session read there that waited above
  that sequence waited on the lost branch, whose sequences the new leader
  reassigns, so its wait is capped at it. The leader history is every
  `leader` operation plus the final leader's final reads; it goes through
  the #94 checks. The replication checks run over every node, with
  convergence against the final leader.

The manifest-boundary check is done at bootstrap, not on the first tailed
entry. A failed write consumes sequences, so a gap after `through_sequence`
is legitimate. What must hold is that the follower opens at exactly
`through_sequence`, with exactly the snapshot's contents.

## Findings

**#168 reproduces.** Without retention, `cluster-vacuum` reported prefix and session violations
in about half its rounds (3 of 6 in the first local run). `cluster`, with
leader vacuum off, stayed clean in all 6 rounds, including the rounds with
the degrade nemesis.

A typical violation: a follower read shows one key at sequence S but lacks
a committed append at or below S on another key. Or a follower whose
`durable_sequence()` has passed W lacks an append committed at or below W.
Vacuum dropped the dead entry before the lagging follower's `changes_since`
reached it. The follower shows the gap until the newer version arrives.

**#168 fixed by retention.** With `retain_after` set to the lowest follower
position, `cluster-vacuum` was clean in all 6 local rounds (2–83 vacuums a
round), and `cluster-vacuum-unretained`, the same run without it, showed
#168 in all 6.

**A write one reader had seen was invisible to a later reader.** The first
local run with Elle reported `G-single-item-realtime` on the leader of a
`cluster` round, with leader vacuum off. The monotonic cross-check flagged
the same thing on node 0. Transaction T13 was still in flight. Reader T12
saw its append to one key. Reader T14, which began after T12 finished, did
not see T13's append to another key.

#180 had closed the gap for a writer that returns in the middle of another
thread's publication, but here no writer had returned. T12's thread loaded
the new state directly, while T14's thread served its cached state because
the publisher had not yet stored `state_time_`.

The fix replaces the timestamp in session mode with a publication counter
and a generation, described in `bytecask_design.md` under *Why session mode
needs more than a timestamp*. The regression test is "pipeline: a write one
reader has seen is visible to every later reader, while its publication is
still in progress". It fails on `main` 3 times out of 3.

**Re-targeting after an unplanned promotion forked silently.** With the
target drawn at random, a follower `n2` ahead of the promoted `n1`, because
it had tailed the old leader further, re-targeted `n1`. `n1` assigned its
new writes sequences `n2` already held, and `ingest` skipped them as
duplicates: `n2` kept the old leader's writes and dropped `n1`'s, with no
error. It showed as convergence and prefix violations in 5 of 6 seeds, and
Elle reported the whole history not serializable (G2-item). The leader
history stayed strict-serializable. The fix is in the protocol, not the
engine: cut every follower off the old leader, let the ingest in flight
finish, and promote the most advanced follower. No follower can then be
ahead of the new leader. `replication_primitives_design.md` now says so.

**A planned transfer lost acknowledged `sync=false` writes.** In a run
with only planned transfers, node 1 acknowledged an append at sequence
8664, and the transfer recorded node 1's `durable_sequence()` as 8663. The
append was missing from the final read. `set_mode(Follower)` drained the
commit pipeline but never called `fdatasync`, so a write acknowledged
without sync stayed above `durable_sequence()`, and `changes_since`, which
stops there, never shipped it. The new leader then reused its sequence.
`set_mode(Follower)` on a leader now `fdatasync`s first, so a leader that
steps down has made every write it acknowledged durable, and the transfer's
catch-up wait covers them all.

With both fixes, `topology` passed every local round.

## Acceptance

- `cluster` passes nightly in release and under ASan, or every failure it
  finds is filed and fixed.
- `cluster-vacuum` passes with retention, and `cluster-vacuum-unretained`
  detects #168 at least once per run.
- Seeds are printed, and a failure uploads the histories and the event log.
- `replication_primitives_design.md` cites the checked guarantees, the
  #168 result and whatever the fork case settles.
