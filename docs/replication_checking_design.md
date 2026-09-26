# Replication Checking with Elle

Status: step 1 (tailing, fixed leader) implemented and running nightly;
step 2 (topology) designed. Tracks [#178](https://github.com/gustavoamigo/bytecaskdb/issues/178).
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
| Unplanned promotion | stop tailing into the target; `target.set_mode(Leader)`; switch the view; abandon the old leader (clients get `:info` for writes in flight); re-bootstrap it later from the new leader | the promoted node's history is a prefix of the old leader's durable history |
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
| `cluster-vacuum` | on | all, with lag | **detects #168**: a follower that resumes `changes_since` below a compacted file receives a batch with entries missing |

`cluster-vacuum` plays the role `blind` plays in #94: it shows that the
harness finds the failure it is aimed at. It is an expected failure until
#168 is fixed, and a regression check after that.

## Likely findings

**Re-targeting after an unplanned promotion can fork silently.** Take a
follower `n2` that is ahead of the promoted node `n1`, because it tailed the
old leader further. When `n2` re-targets `n1`, `n1` assigns fresh writes the
sequences `n2` already holds. `ingest` skips entries at or below
`durable_sequence()` as duplicates, so `n2` would drop `n1`'s new writes and
keep the old leader's, with no error. The design says only "other followers
re-target the new leader". The harness draws the promotion target at random,
so it will reach this case. The convergence check should catch it. Unless a
guard already prevents it, it becomes an issue: either promote the
most-advanced follower, or have a follower refuse a source whose
`durable_sequence()` is behind its own and re-bootstrap instead.

## Open questions

1. **Mid-batch `ingest` slices.** `ingest` publishes whatever slice it is
   given in one step, and only its rotation chunking respects batch
   markers. A caller that cuts a slice between `BulkBegin` and `BulkEnd`
   publishes half a batch on the follower. The proof tests cut at batch
   boundaries themselves (`gen_incremental_test`), and no contract says the
   caller must. Two options:
   - document it as a caller obligation in `CONTRACT.md`, have the harness
     cut at batch boundaries, and add a harness mode that cuts anywhere to
     show the consequence; or
   - make `ingest` hold back or refuse a trailing incomplete batch, which is
     a behaviour change and belongs in its own issue.

   Recommendation: the first for this harness, and an issue for the second.
2. **#168's fix shape** decides the flipped expectation for
   `cluster-vacuum`. If `changes_since` refuses to resume below a lower
   bound, the harness must treat that as "re-bootstrap this follower", not
   as a failure. Bootstrap is already an event, so the path exists.
3. **Promotion target.** Random, which reaches the fork above, or
   most-advanced, which is what an operator should do? Recommendation:
   random, since the harness should look for trouble. The fork finding
   decides what the protocol document says.

## Implementation

Two steps, so each can land green:

1. **Tailing**, implemented. Bootstrap, replicate, lag, duplicates, follower
   restart, follower vacuum and follower reads, with a fixed leader.
2. **Topology**, not yet implemented. Planned transfer, unplanned promotion,
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

The manifest-boundary check is done at bootstrap, not on the first tailed
entry. A failed write consumes sequences, so a gap after `through_sequence`
is legitimate. What must hold is that the follower opens at exactly
`through_sequence`, with exactly the snapshot's contents.

## Findings

**#168 reproduces.** `cluster-vacuum` reports prefix and session violations
in about half its rounds (3 of 6 in the first local run). `cluster`, with
leader vacuum off, stayed clean in all 6 rounds, including the rounds with
the degrade nemesis.

A typical violation: a follower read shows one key at sequence S but lacks
a committed append at or below S on another key. Or a follower whose
`durable_sequence()` has passed W lacks an append committed at or below W.
Vacuum dropped the dead entry before the lagging follower's `changes_since`
reached it. The follower shows the gap until the newer version arrives.

Step 2 is still to come, including the fork case above.

## Acceptance

- `cluster` passes nightly in release and under ASan, or every failure it
  finds is filed and fixed.
- `cluster-vacuum` detects #168 on the current engine, and the nightly
  asserts that it does.
- Seeds are printed, and a failure uploads the histories and the event log.
- `replication_primitives_design.md` cites the checked guarantees, the
  #168 result and whatever the fork case settles.
