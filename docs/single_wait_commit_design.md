# Single-wait commit — one sleep and one targeted wake per writer

Status: **not planned**; kept for the slot state machine and the transition
table. Tracking: [#83](https://github.com/gustavoamigo/bytecaskdb/issues/83),
closed as not planned on 2026-10-08 — the design was implemented on branch
`single-wait` (89cd2d5), met H1/H2/H4, missed H3, and was parked as not
mergeable. This document is the record of it: not a plan, and none of it is
implemented on `main`. What is worth keeping is below — the slot state machine (§ *The
slot is the wait object*) and the transition table (§ *The transition table*),
whose test-before-the-code routine is now `verify/transition/`, retargeted to
the protocol that runs. Supersedes the design on branch `single-wait` (commit
89cd2d5), which worked and was parked: see
[Why the first implementation is not the base](#why-the-first-implementation-is-not-the-base).

Reader: `docs/commit_pipeline_design.md` describes stage 2 as it is on main —
a `flush_in_flight_` token, `flush_pending`, and writers parked on
`durable_cv_`. This document proposed to change only who sleeps and who is
woken; the pipeline it compares against has moved since it was written, which
is what
[Relationship to the current pipeline](#relationship-to-the-current-pipeline)
is for. Durability before visibility, the publish invariant
`durable_seq >= sync_requested_seq`, failure classes F/G/H, one batch per
leader, the settle wait, `CommitResult` and `wait_published` are unchanged.

## Relationship to the current pipeline

What runs instead, and where: stage 1 is `WriteGroup`
(bytecaskdb/concurrency.cppm:112), which hands a queued slot to the committer
thread that runs the batches which queue behind a running one
([#224](https://github.com/gustavoamigo/bytecaskdb/pull/224)) — `submit`
(concurrency.cppm:157-203) crosses `running_`, `hand_over_`,
`committer_sleeping_` and `queue_`, and its `Slot::state`
(concurrency.cppm:269-296) is a three-value release word, not a state machine.
Stage 2 is `DB::commit_wait`, `DB::flush_once`, `DB::finish_flush` and
`DB::flush_threw` (bytecaskdb/db_commit.cpp:415-558): one `flush_in_flight_`
token, one `durable_cv_`, `flush_error_`, the published state — and a flush
that owes an `fdatasync` waits first for the commits it would otherwise miss,
`CommitDelay` ([#336](https://github.com/gustavoamigo/bytecaskdb/pull/336)),
a timing heuristic inside that flush.

Still relevant if the wait is ever measured on the critical path again: the
slot as the wait object with one state and one load (§ *The slot is the wait
object*) — `Slot::state` is what that idea shrunk to one release; the single
invariant and its single hand-off, "an uncovered slot in the queue implies the
role is held", which the shipped `WriteGroup` maintains in its own form (three
flags and one wake decision) and which `verify/transition/` checks transition
by transition; the ownership rule (§ *Ownership*) that the transition out of
`Queued` is the executor's last touch of a slot; and the transition table as a
test written before the code (§ *Tests*). What does not carry over is
`CommitQueue` itself, the `durable_cv_` waiter counter (the shipped publishes
guard their own notifies), and the validation gate in § *Validation gate*,
which was written to decide this design.

## Problem

A sync writer sleeps twice per commit. Once in `WriteGroup::submit`, waiting
for the leader's `cv_.notify_all()` to tell it stage 1 ran — which, for a sync
write, it does nothing with. Then again in `commit_wait`, on `durable_cv_`,
which is a broadcast that also wakes every writer whose flush has not landed.
Measured on sysbench `oltp_insert`, 8 clients, fdatasync ≈ 155 µs: 2.0 sleeps,
1.9 threads woken and 2.0 futex wakes that reach nobody per transaction,
53 µs/tx of kernel futex and scheduler CPU against InnoDB's 16 µs.

Two alternatives were measured and rejected on the first attempt: a bounded
spin before each sleep (−9% to −30%; CPU is the scarce resource on that host)
and releasing old `EngineState`s on the background worker (neutral).

## Goals

- A sync writer sleeps at most once, and only while its bytes are not durable.
- Every wake targets one thread that has work to do because of that event.
- No mutex on a writer's wake path.
- **A writer learns its entire situation from one atomic load of one word.**
  This is the goal the first attempt missed, and the one that makes the rest
  auditable.

Not in scope: the settle-cap rule, spinning.

## The slot is the wait object, and it has exactly one state

`Slot::state` is a single atomic enum. There are no flags, no second atomic,
and no predicate computed from more than this one value. A writer that wakes
loads it once and knows what to do.

| state | meaning |
|---|---|
| `Queued` | submitted, stage 1 has not released it |
| `Leading` | nominated to run stage 1 for the write group |
| `Pending` | stage 1 appended its entries; waiting for the flush that covers it |
| `Nominated` | `Pending`, and nominated to run that flush |
| `Done` | released to its owner, which may now read `result` and `err` |

A slot is never "waiting and leading", or "done and holding the role", because
those states do not exist. The first attempt spent a 32-bit word on a 2-bit
phase plus `Lead`, `FlushNext` and `Waiting` bits and a second atomic
(`release_at`) saying which phase counted as finished; "is this slot done"
was a function of two atomics and three helpers, and every defect it hit was
an unhandled combination of those bits at one instant.

`release_at` is gone. The owner does not evaluate coverage — it waits for the
literal value `Done`. Coverage is evaluated once, by the thread that completes
the slot, from `result->sequence` and `opts.sync`: plain fields written by the
executor while it exclusively owned the slot.

### The transition table

Every transition is one compare-exchange from a named state to a named state,
with one actor. Nothing does a read-modify-write of flags.

| from | to | actor | under | may fail? |
|---|---|---|---|---|
| — | `Queued` | owner, at `submit` | — | — |
| `Queued` | `Leading` | the outgoing leader | `queue_mu_` | no |
| `Leading` | `Queued` | owner, taking leadership | — | no |
| `Queued` | `Done` | the executor, for a slot with nothing to wait for | — | no |
| `Queued` | `Pending` | the executor, for a slot that appended | `commit_mu_` | no |
| `Pending` | `Nominated` | `CommitQueue::hand_off`, handing the flush role | `commit_mu_` | no |
| `Nominated` | `Pending` | owner, taking the flush role | — | no |
| `Pending` | `Done` | `CommitQueue::complete` / `fail_all` | `commit_mu_` | **yes** |

The single "may fail" entry is the whole race, named and contained: a
`Nominated` slot is skipped by completion. Its owner returns it to `Pending`
and the flush it then runs completes it. That one sentence replaces the first
attempt's "a handed role is exercised before the done check so it can never be
lost".

An attempt to leave a state other than the one named is a programming error,
not a case to handle: `move(from, to)` asserts, `try_move(from, to)` is used
only where the table says the transition may fail.

### Waiting and waking

```cpp
// Parks until the state is no longer `expect`. Returns the state seen.
auto park_while(SlotState expect) -> SlotState;
```

`std::atomic<SlotState>::wait` / `notify_one`: one address per slot, so a wake
reaches exactly one thread, and libc++ skips the syscall when nothing is
parked on that address. Same effect as the first attempt's `kSlotWaiting` bit
and raw `FUTEX_WAIT_PRIVATE`, without a bit in the state word or a Linux
`#ifdef`. If measurement later shows libc++'s contention table costs more than
a hand-rolled futex, that is a two-function change behind this interface; it
is not the part that was fragile.

A single-threaded (`BYTECASK_SINGLE_THREADED`) build aborts on a park: with
one thread a park can only be a bug, and the design never reaches one there
(the sole writer nominates itself).

## `CommitQueue` — one object for all of stage 2

Everything a writer waits on lives in one class with one mutex. On main and on
the parked branch this state is spread over DB members: `flush_in_flight_`,
`pending_`, `pending_mu_`, `barrier_waiters_`, `durable_waiters_` and
`durable_cv_`, read and updated by `commit_wait`, `flush_once`, `release_role`,
`complete_covered`, `fail_pending`, `pend_batch`, `drive_pending` and
`quiesce`.

```cpp
class CommitQueue {
 public:
  // What the published state covers. Extracted from EngineState by the
  // caller, so CommitQueue is unit-testable without a DB.
  struct Coverage {
    std::uint64_t next_seq;      // published writes are < next_seq
    std::uint64_t durable_seq;   // fdatasync confirmed through here
    bool degraded;
  };

  // Stage 1, under write_mu_: hand the slot over and return whether the
  // caller must run the flush that will cover it.
  void push(EngineSlot& slot);

  // Flush role.
  [[nodiscard]] auto try_acquire_role() -> bool;   // writers
  void acquire_role_blocking();                    // barriers (quiesce)
  void release_role(Coverage published, std::exception_ptr flush_error);

 private:
  std::mutex mu_;
  EngineSlot* head_{nullptr};   // intrusive FIFO, ascending sequence
  EngineSlot* tail_{nullptr};
  bool role_held_{false};
  int barrier_waiters_{0};
  std::condition_variable role_cv_;   // barrier waiters only
};
```

The waiting set is an intrusive singly-linked FIFO through a `next` field on
`EngineSlot` — no allocation on the commit path, and in ascending sequence
order because `execute_slots` pushes under `write_mu_`, which serialises every
batch of both writers. `release_role` is one pass over it: unlink and complete
each covered slot, remember the first uncovered one, nominate it.

The first attempt's list was a `std::vector` pushed from an `after_batch` hook
outside `write_mu_`, so it was not in sequence order, completion was an
`erase_if` over the whole list and the hand-off was a separate scan for the
lowest uncovered sequence under a second lock hold.

### The one invariant

> While the queue holds a slot the published state does not cover, the role is
> held: by a thread running a flush, or by the owner of the one `Nominated`
> slot.

Every function that can leave the queue non-empty re-establishes it by calling
one private `hand_off()` before releasing `mu_`. It is checkable in a test and
it is the only liveness argument in the design.

On the parked branch the same property was three separate rules: `pend_batch`'s
catch-up `complete_covered`, a `drive_pending` special case in `apply_batch`
for a leader with nothing of its own to wait for, and the "prefer a slot the
publication does not cover" hand-off heuristic. `drive_pending` disappears: a
leader whose own plan conflicted has already nominated one of its members
inside `push`.

### Ownership: one local rule instead of a hook

> **The transition out of `Queued` is the last time the executor may touch
> that slot.**

`push` takes `mu_`, links the slot while it is still `Queued` — invisible to
completion, which only acts on `Pending` — then moves it to `Pending`. From
that CAS on, the slot belongs to the queue, and its owner may be released and
its stack frame reused at any moment.

This is what the parked branch needed `WriteGroup::after_batch` and a
three-step ownership argument for. The hook is not needed at all, and
`WriteGroup` keeps the shape it has on main: it never touches a slot after the
executor returns normally. Two contract lines make that safe:

- The executor transitions **every** slot out of `Queued` before returning
  normally. `WriteGroup`'s post-batch loop is only on the exception path.
- If the executor throws, it has transitioned **no** slot, so `WriteGroup` may
  set `err` and move each slot `Queued → Done`. `execute_slots` already does
  all its fallible work before the release pass.

`submit` also stops reading `slot.err` for a slot it did not release: it loads
the state once, and reads `err` only if that load says `Done`. Reading `err`
on a still-`Pending` slot was TSAN defect 2 of the first attempt.

### Lifetime of the notify

A completer writes `result`, unlinks the slot, moves it to `Done`, and then
notifies. Between the move and the notify the owner may already have returned
from `apply_batch`. This is safe and must stay documented: `notify_one` uses
the object's address as a key and never dereferences it, and a `Slot` lives in
the frame of the thread being notified, so the address stays mapped.
`EngineSlot` must therefore stay stack-allocated — a heap-allocated slot would
make this a use-after-free.

## Barriers and long-polls

`quiesce()` calls `acquire_role_blocking()`, which registers in
`barrier_waiters_` and waits on `role_cv_`. `hand_off` checks
`barrier_waiters_` first and frees the role rather than nominating, so a steady
stream of writers cannot starve a rotation, a vacuum or a `create_manifest`.
The barrier's `~FlushRole` calls `release_role`, which completes every pending
slot — after a barrier everything is published and durable — and then hands off
as usual.

`durable_cv_` is left to `durable_sequence()` and `wait_published()` alone,
notified by `store_state`, and only when a waiter counter says someone is
there. That counter must cover `wait_published` as well as `durable_sequence`;
the parked branch counted only the latter, which predated `wait_published`.

## Failure

Unchanged in effect. `flush_pending` degrades on the published state and
records `flush_error_`; `release_role` is called with that error and, when the
published state is degraded, moves every queued slot to `Done` with it set, so
each writer appended since the last successful flush rethrows the
`std::system_error` on its own thread (classes F and G). Later writers are
rejected at stage 1 by the degraded published state. `resume()` clears
`flush_error_`; the queue is empty by then.

## Counters

`bytecask.commit_wait_blocked` keeps its meaning: writers that parked waiting
for their flush. `bytecask.group_writer_waits` is new: submitters that parked
in `submit`. Per write the sum is at most `1 + batches / writes` — a member
sleeps once, and the writer handed `Leading` after a batch sleeps once to be
woken as leader and once more for durability, which is what hand-off after
every batch costs and which the current design pays too.

## Tests

1. **Transition table.** A unit test over `CommitQueue` with a fake
   `Coverage`, no DB: every legal transition, and every illegal one asserting.
   Written before the code, as the issue's post-mortem asks. *Written as
   `verify/transition/`, retargeted: `CommitQueue` is not what runs, so the
   table drives the shipped hand-over (`WriteGroup::submit`) and the flush
   role (`commit_wait` / `flush_once` / `finish_flush`), with #589's mutants
   required to fail the check.*
2. **The invariant.** After every `CommitQueue` operation in that test, assert
   that an uncovered slot implies the role is held. *The same assertion is
   there, in the form each shipped protocol states it: a non-empty queue
   implies a batch is running or the committer has been woken to run one, and
   a parked writer implies a flush in flight, an error, or a degraded engine.*
3. The six behaviour tests from the parked branch, which cover the defects it
   found: one sleep per writer; an fdatasync failure reaching a member that was
   never woken; a barrier not starved behind a flush; a `durable_sequence`
   waiter woken; a leader whose own plan conflicts still giving its sync
   members a flusher; and an 8-thread stress mix of sync, nosync and
   conflicting writers over rotations.
4. TSAN and ASAN on all of the above, plus the 12× repeat of `[pipeline]` and
   `[concurrency]`.

## Validation gate

Carried over from the first attempt, except H3.

- H1: `group_writer_waits + commit_wait_blocked` ≤ writes + batches.
- H2: futex wake syscalls per transaction ≤ 1.2, wakes hitting nobody ≈ 0.
- H4: `engine_bench` `Put/Sync`, `Del/Sync`, `PutMT/Sync` at 2–64 threads
  within ±3% or better. The first attempt measured +5% to +32% on
  `PutMT/Sync`; this design does strictly less work per commit, so a
  regression against it is a bug.
- H5: sysbench `oltp_write_only` on the 2.2 ms disk not worse.
- H6: TSAN and ASAN clean. H7: every existing test passes unchanged.

H3 (sysbench `oltp_insert` +8%) is dropped as a gate. The first attempt met
H1, H2 and H4 and moved sysbench not at all: the freed coordination time went
into a longer flush cycle (inter-flush gap 206 → 267 µs, 4.6 → 5.3 tx per
fsync), and the MariaDB critical path had moved to key directory refcount
traffic ([#84](https://github.com/gustavoamigo/bytecaskdb/issues/84)). This
work is justified by H1/H2/H4 and by the code being simpler than what it
replaces; it should not be re-attempted as a sysbench win.

## Why the first implementation is not the base

Branch `single-wait` (89cd2d5) passes everything but H3. Its defects were not
coincidences — each was an unhandled combination of bits that the model
allowed to exist:

| defect | cause | why it cannot recur here |
|---|---|---|
| use-after-free under sysbench | the executor pushed slots to `pending_` before `lead` had finished with the batch | the transition out of `Queued` is the executor's last touch; there is no post-batch loop |
| lost flush role at the hand-off instant | a slot held `FlushNext` and was completed in the same instant; `commit_wait` returned on done first | `Nominated` and `Done` are different states; completion skips `Nominated` |
| hang: a conflicting leader left its members with no flusher | liveness was a special case in `apply_batch` | `push` re-establishes the invariant before it returns |
| `err` read while the flusher was writing it (TSAN) | `submit` read `err` on a slot it had not released | `err` is read only in the `Done` state |
| double-counted sleeps | the counter was withdrawn on a failed futex check | `park_while` counts after it returns from a real park |

The branch and its numbers are kept as the record.
