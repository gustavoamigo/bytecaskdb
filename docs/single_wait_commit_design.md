# Single-wait commit — one sleep and one targeted wake per transaction

Status: implementation in progress on branch `single-wait`. Tracking: #83.
Working document; folded into `docs/commit_pipeline_design.md` once the
validation gate passes, then deleted.

Baseline: September 15 2026 analysis on `/mnt/test-data` (fdatasync
≈ 155 µs), MariaDB sysbench `oltp_insert`, 8 clients, 5M rows, 4 vCPUs.

## Problem

After the commit pipeline, ByteCaskDB and InnoDB pay the same disk cost per
commit (155 µs fdatasync vs 148 µs O_DIRECT log write) and ByteCaskDB packs
more transactions into each one (4.8 vs 3.2). ByteCaskDB still commits 12.6K
inserts/s against InnoDB's 17.6K. With 8 closed-loop clients throughput is
`8 / latency`, and the missing 180 µs of latency per transaction is
coordination, not I/O:

| per transaction                        | ByteCaskDB | InnoDB |
|----------------------------------------|-----------:|-------:|
| sleeps (futex waits)                   | 2.0        | ~1.0   |
| threads woken                          | 1.9        | ~1.0   |
| futex wakes that wake nobody           | 2.0        | ~0     |
| kernel CPU in futex / wake / scheduler | 53 µs      | 16 µs  |
| voluntary context switches             | 3.2        | 2.2    |

A sync writer today sleeps in `WriteGroup::submit` until the leader's
broadcast, wakes, then sleeps again in `commit_wait` until the flusher's
broadcast, which also wakes every waiter whose flush has not landed. The
first sleep exists only so the writer can learn its stage 1 outcome before
waiting for durability; for a sync write nothing is done with it. Two
experiments ruled out the alternatives that move or trade work instead of
removing it: a bounded spin before each sleep lost 9 to 30% (CPU is the
scarce resource on that host), and releasing old `EngineState`s on the
background worker was neutral.

## Goals and non-goals

- A sync writer sleeps at most once, only while its bytes are not durable.
- Every wake targets one thread that has something to do with the event.
- No mutex on the wait or wake path of a transaction.
- Unchanged: durability before visibility, the publish invariant
  `durable_seq >= sync_requested_seq`, failure classes F/G/H, one batch per
  leader, the settle wait, `CommitResult` and when a caller gets it, and
  read-your-own-writes on return for nosync writes (they wait for
  publication, not for durability).
- Not in scope: the settle-cap rule; spinning (measured negative).

## The slot is the wait object

`Slot::word` is a 32-bit atomic: low bits the phase, the rest flags.

| phase / flag | meaning |
|---|---|
| `kSlotQueued` < `kSlotApplied` < `kSlotVisible` < `kSlotDurable` | stage 1 ran → its state is published → its fdatasync returned |
| `release_at` (atomic, set by the executor) | phase at which `submit` returns the slot: `Applied` for a conflict, error or empty plan; `Visible` for a nosync write; `Durable` for a sync write |
| `kSlotLead` | the owner is the next `WriteGroup` leader |
| `kSlotFlushNext` | the owner holds the flush role |
| `kSlotWaiting` | the owner is, or is about to be, asleep |

- `slot_wait`: set `kSlotWaiting`, sleep on the futex only while the word
  still holds the exact value seen. A wake between the load and the sleep
  fails the kernel's value check and the owner re-reads; no wake is lost.
- `slot_advance(phase)`: raise the phase (never lower), keep the flags, wake
  the owner only if that releases it and it is waiting. One syscall, one
  thread, none for an owner that is not asleep.
- `slot_hand(flag)` / `slot_take(flag)`: give a role to the owner, waking
  it if asleep; the owner clears the flag when it acts on it.
- `slot_done(w)`: phase ≥ `release_at`. Only then may the owner read `err`
  and `result`.

Linux uses `FUTEX_WAIT_PRIVATE` / `FUTEX_WAKE_PRIVATE` on the word; other
platforms `std::atomic::wait` / `notify_one`; the single-threaded build
aborts on a wait, which it can never reach.

## Ownership: who may touch a slot, and when

This is the rule the first implementation broke. A slot is owned by exactly
one party at a time, and hand-overs happen at two points, both on the
leader's thread:

1. **Owner → `WriteGroup`**, at `submit`. From the push onto the queue until
   the group has finished with the batch, only the `WriteGroup` and the
   executor it calls (both on the leader thread) touch the slot. The group
   finishes with a batch when `lead` has advanced every slot to `Applied`
   and handed leadership on. The owner sleeps and reads nothing.
2. **`WriteGroup` → flush role**, after that point, through the group's
   `after_batch` hook, on the leader thread: the engine pushes the slots
   that still wait (those with a sequence) onto `pending_`. From the push
   on, only `complete_covered` and `fail_pending` advance them, and only
   they pop.
3. **Flush role → owner**, at completion: the slot is popped and advanced to
   `Visible` or `Durable` (or `Durable` with `err` set) in one step, which
   is the owner's one wake. The owner reads `result` and `err` after it
   sees `slot_done`, and returns. A slot lives on its owner's stack, so it
   is destroyed only after step 3, and nobody holds a pointer to it by then.

Two consequences that must hold in code:

- The executor never pushes to `pending_`. If it did, the flush role could
  complete a member before `lead` finished its post-batch loop, and the
  loop would advance a slot whose owner had already returned and destroyed
  it (the crash found under sysbench in the first implementation).
- `pending_` is not ordered by sequence: leaders push after their batch, and
  two leaders may push out of order. `complete_covered` scans the whole
  list; the hand-off picks the lowest sequence. The list is at most one
  entry per waiting writer.

## Stage 1 — the leader wakes only who needs waking

`WriteGroup::submit`: enqueue; become leader if none is active, else
`slot_wait` on the slot's own word. `lead` runs the executor once for the
batch, then, still owning the slots: advance each to `Applied` (waking the
owners whose `release_at` is `Applied`: conflict, error, empty plan; sync
and nosync members keep sleeping), hand leadership to the queue head with
`kSlotLead`, then call `after_batch(batch)` and return. `SoloWriter` does
the same for its one slot. Neither reads a slot after the hook.

`DB::execute_slots` (the executor) is unchanged except that it sets
`release_at` per slot with a sequence and no longer touches `pending_`. The
rotation path completes its slots inline as before and leaves `release_at`
at `Applied`.

`DB::pend_batch` (the hook), on the leader thread with no lock held:

```
push every slot with a sequence onto pending_          (under pending_mu_)
complete_covered(load_state())
```

The second line closes the window in which a flush published the batch's
state before the push and found nothing to complete. Popping is safe for a
non-holder: every popper pops only slots the published state covers, and
completes what it pops, under `pending_mu_`.

## Stage 2 — one wait per writer

`apply_batch`, after `submit` returns:

```
if the slot has a sequence: commit_wait(slot)
else if pending_ is not empty and the role is free: flush_once()
```

The second line is the liveness rule for a leader with nothing of its own to
wait for (conflict, error, empty plan): its members may be pending with no
flush in flight and nobody else to start one. It costs a mutex check on a
path that returns without I/O anyway.

```
commit_wait(slot):
  loop:
    slot_done?                                 rethrow err if set; return
    holds kSlotFlushNext, or the role is free? flush_once(); continue
    slot_wait(slot)                            — the one sleep
```

A member woken by completion returns from `submit` and never enters the
loop; a member woken with `kSlotFlushNext` returns from `submit`, enters the
loop, and flushes.

```
flush_once (role holder):
  settle ≤ kFlushSettleMax while the WriteGroup is busy
  flush_pending                                (fdatasync if owed; publish)
  release_role:
    complete_covered(published)                (one targeted wake per writer)
    published degraded?  fail_pending(flush_error_ or DbDegraded)
    a barrier is waiting in quiesce()?  clear the flag, notify durable_cv_
    else pending_ not empty?  slot_hand(lowest sequence, kSlotFlushNext)
                              — the flag stays set across the hand-off
    else clear the flag
```

`durable_cv_` serves only `durable_sequence()` and `quiesce()`; each
registers in a waiter counter, and `store_state` / `release_role` notify
only when a counter is non-zero. A waiting barrier takes the role ahead of
pending writers, or a steady stream of writers could starve it; the
barrier's `~FlushRole` completes every pending slot (after a barrier
everything is published and durable) and then hands off or clears as above.

## Failure

`flush_pending` degrades on the published state, records `flush_error_`,
publishes the degraded state. `release_role` then completes every pending
slot with that error, so each writer appended since the last successful
flush rethrows the `std::system_error` on its own thread (classes F and G).
Later writers are rejected at stage 1 by the degraded published state
(`DbDegraded`). `resume()` clears `flush_error_` after it republishes;
`pending_` is empty by then.

## Counters, and the one wake that stays

`bytecask.group_writer_waits` (submitters that slept in `submit`) is new;
`bytecask.commit_wait_blocked` keeps its meaning (leaders that slept in
`commit_wait`). A member sleeps once, a first leader at most once. The
writer handed leadership after a batch sleeps once to be woken as leader
and once more for durability: that wake is what hand-off after every batch
costs, the previous design paid it too, and it happens for at most one
writer per batch. Per write the sum of the two counters is therefore at
most `1 + batches / writes`, measured in the stress test as
`waits + blocked <= submissions + group_writer_batches`.

## Defects found in the first implementation, and their tests

1. `release_at` was a plain field written by the executor and read by the
   sleeping member (TSAN). Fixed: atomic, relaxed; a queued slot sleeps for
   any value, and the acquire load of the word orders the rest.
2. `submit` read `err` right after `lead` for a leader whose slot was
   already pending, racing the flusher's `fail_pending` (TSAN). Fixed: `err`
   is read only after `slot_done`.
3. The executor pushed slots to `pending_` before `lead` had finished with
   the batch; a member completed by the flusher returned and freed its slot
   while `lead` was still advancing the batch (segfault under sysbench).
   Fixed by the ownership rule above: the push moves to `after_batch`.
4. A leader with no sequence never entered `commit_wait`, so with no flush in
   flight its sync members had no flusher (hang). Fixed by the liveness rule
   in `apply_batch`.
5. A pending slot could be handed the flush role and, in the same instant,
   be completed by a leader's catch-up `complete_covered` (its state was
   published just before it was pushed). Its owner woke with both "done" and
   "holds the role"; `commit_wait` returned on "done" first and the role was
   never exercised again (hang, found by the many-writers test). Fixed:
   `commit_wait` exercises a handed role before the done check, and the
   hand-off prefers a slot the publication does not cover.

Tests added for 3 and 4: a conflicting leader over a sync member with the
role free (must complete, deterministic through `on_leader_start_`), and a
stress test with 8 threads mixing sync, nosync and conflicting plans over a
small file size for a bounded time, run under TSAN and ASAN.

## Validation gate

- H1: `group_writer_waits + commit_wait_blocked` ≤ writes + batches
  (engine counters, sysbench `oltp_insert`, 8 clients); with the
  measured ~2 writes per batch, about 1.5 sleeps per write against 2.0.
- H2: futex wake syscalls per transaction ≤ 1.2 and wakes hitting nobody
  ≈ 0 (bpftrace; baseline 2.6 and 2.0).
- H3: sysbench `oltp_insert` 8 clients on the fast disk ≥ +8% tps, two
  interleaved rounds against the baseline build.
- H4: `engine_bench` `Put/Sync`, `Del/Sync`, `PutMT/Sync` 2 to 64 threads
  within ±3% or better.
- H5: sysbench `oltp_write_only` on the 2.2 ms disk not worse than baseline.
- H6: TSAN and ASAN suites clean, including the stress test.
- H7: every existing test passes unchanged; the 12× repeat of the
  `[pipeline]` and `[concurrency]` tags is clean.

## Documentation on completion

Fold into `docs/commit_pipeline_design.md` as the single explanation of the
pipelined group commit; update the stage 2 section of
`docs/bytecask_design.md`, the `durable_sequence` paragraph of
`CONTRACT.md`, and the two group-commit sentences of `README.md`.
