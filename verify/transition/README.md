# Transition-table checks of the commit path

Executable form of the technique
[`docs/single_wait_commit_design.md`](../../docs/single_wait_commit_design.md)
§ *Tests* asked for — a transition table, invariants checked after every
transition, and mutants shown to fail the check — applied to the two
coordination protocols that *do* run, not to the design that will not
([#589](https://github.com/gustavoamigo/bytecaskdb/issues/589); the design it
was written for is `not planned`, [#83](https://github.com/gustavoamigo/bytecaskdb/issues/83)).

Every interleaving of N writers is enumerated — plus the committer thread, in
the write group's model — each model's invariants are checked after every
transition, and a state with no enabled step while a writer has not returned
is reported as a hang. The four mutants of #589 are built in, and the checker
requires the check to fail on each; `ci.yml` runs that self-check.

```
$ xmake build transition_check && build/linux/x86_64/release/transition_check
```

Sub-second: 0.13 s for the whole self-check (9 reference runs, 4 mutants) on
the machine this was written on. Other modes: `--mutant=NAME` runs one mutant
on its own and exits non-zero when the check fails on it; `--trace` prints the
counterexample of every run that violates (a mutant prints it regardless).

Not `tests/*.cpp`, which is globbed into `bytecask_tests` and where a
standalone `main` breaks the build. This is standalone and single-threaded: no
engine, no test framework, no sanitiser sensitivity.

## Model 1 — the write group's slot hand-over

`WriteGroup::submit` (bytecaskdb/concurrency.cppm:157-203) turns a queued slot
over to the committer through three flags and a queue — `running_`,
`hand_over_`, `committer_sleeping_`, `queue_` — plus `committer_cv_`. Each row
of the model is one step of one actor and one hold (or one release) of
`queue_mu_` in the code:

| step | actor | where | what |
|---|---|---|---|
| `submit` | a writer | concurrency.cppm:157-181 | the slot enters `queue_`; a writer that finds `running_` clear takes the whole queue as a batch and runs it, otherwise it waits for its slot's release |
| `run` | the batch's owner | `run_batch`, concurrency.cppm:232-262 | the executor runs the batch; the slots are done and released to their owners |
| `end` | the inline writer | concurrency.cppm:187-197 | `more` = the queue is not empty; if `more`, `hand_over_ = true` and the wake decision `more && committer_sleeping_`; otherwise `running_ = false` |
| `notify` | the inline writer | concurrency.cppm:197 | `committer_cv_.notify_one()`, after the unlock: it reaches the committer only if the committer is already inside `wait()` |
| `gate` | the committer | `committer_loop`, concurrency.cppm:299-319 | one hold of `queue_mu_` around the decision: take the queue (clearing `hand_over_` **at handover**) or, in that same hold, set `committer_sleeping_` and enter `wait()` |
| `wake` | the committer | concurrency.cppm:313 | `wait()` returns: `committer_sleeping_ = false`, back to the gate |
| `end` | the committer | concurrency.cppm:322-330 | the drain: take the next queue if it is non-empty, otherwise `running_ = false` |

**The invariant** is #589's sentence, read as the code states it:

> A non-empty queue implies a batch is running or the committer has been woken
> to run one.

checked as *the queue is non-empty ⟹ a batch is running (its owner has not
decided yet) ∨ `hand_over_` ∧ (the committer is awake ∨ a wake has reached it ∨
a wake is owed to it)*. `running_` is checked with it: a queued slot with the
flag clear could be handed to nobody.

`wait_released` and `Slot::state` are not part of this protocol: that wait is
an atomic wait on a word the releaser writes, which rechecks the value and so
cannot lose a wake. Only `committer_cv_` can, which is why it is the notify
that is modelled as a step.

## Model 2 — the fdatasync role

`commit_wait`, `flush_once`, `flush_threw`, `finish_flush` and the published
state (bytecaskdb/db_commit.cpp:397-558) give the writers one flush role,
`flush_in_flight_`, and one condition variable, `durable_cv_`:

| step | where | what |
|---|---|---|
| `commit` | `execute_slots`, then `commit_wait` | the write appends at the next sequence and remembers its target: `durable_seq` for a sync write, the published state for an unsynced one |
| `check` | db_commit.cpp:516-556 | covered → return; `flush_error_` or the degraded engine → throw; otherwise take the role (`flush_in_flight_.exchange`) or park on `durable_cv_` |
| `flush` | db_commit.cpp:359-461 | capture the head, fdatasync if the head owes one, publish what was captured — or throw where the counted allocation-failure sweep puts `std::bad_alloc` (#364) |
| `flush(throws)` | `flush_threw`, db_commit.cpp:463-475 | record `flush_error_`, refuse writes; `flush_once` still calls `finish_flush` |
| `finish_flush` | db_commit.cpp:415-419 | release the role and notify `durable_cv_` |

**The invariant** is #589's sentence, with the one word made precise:

> A parked writer whose target is uncovered implies a flush is in flight, an
> error is recorded, or the engine is degraded.

where *a flush is in flight* is the promise the flag stands for — a thread is
holding the role between taking and releasing it — not the bare flag. The two
are checked together (`flush_in_flight_` is set exactly while a thread holds
it), which is what turns the #364 shape into a state violation: a role kept
with nobody holding it claims cover for parked writers that no flush will ever
deliver.

Both models also report a state with no enabled step while some writer has not
returned as a hang.

## Mutants

Each mutant breaks one line of the code the way #589 describes it, and each
carries the guard that catches it. The `Guarded-by:` headers name the check
below, not a Catch2 spec: `scripts/check_guarded_by.py` indexes engine
mutation patches against `bytecask_tests`, and these mutants are variants of
the model. What replaces the discipline is the self-check — the checker runs
every mutant on every run and fails unless each is caught, so a guard that has
gone stale breaks `ci.yml` the way a stale spec does.

| mutant | the break | Guarded by | the counterexample (2 writers) |
|---|---|---|---|
| `sleeping-read-early` | `committer_sleeping_` read before the queue lock is released: the wake decision sees the flag as it was, not as it is (#589's "a wake can be sent to a thread that has not slept yet") | the hand-over invariant | `w0.submit w0.run w1.submit c.gate w0.end` — w0 read the flag before the committer slept and sends no wake |
| `hand-over-cleared-at-wake` | `hand_over_` cleared after the wake instead of at handover (concurrency.cppm:317 moved into the wake path) | the hand-over invariant | `w0.submit w0.run w1.submit c.gate w0.end w0.notify c.wake` — the wake clears the hand-over the committer was woken to serve |
| `wake-skipped` | the wake skipped when `more` is true (concurrency.cppm:197) | the hand-over invariant | `w0.submit w0.run w1.submit c.gate w0.end` — the committer sleeps with the queue non-empty |
| `flush-role-kept-on-throw` | `finish_flush` skipped on the throw path: a `flush_pending` that throws escapes `flush_once` with the role (#364's shape) | the flush-role invariant | `w0.commit w0.check w0.flush w0.finish_flush w1.commit w1.check w1.flush(throws)` — `flush_in_flight_` is set with nobody holding the role |

`flush-role-kept-on-throw` is the same break as
`tests/durability_mutations/flush_role_kept_on_throw.patch` (and
`barrier_flush_role_kept_on_throw.patch` for `quiesce`), whose engine test —
`Guarded-by: "refuse writes: a flush that throws releases the flush role"` —
catches it by hanging. The model catches it in 10 states and 0 ms, as a state
violation, and says which state rather than how long the run took to die.

The honest limit of `sleeping-read-early`: the pair "wake sent to a thread
that has not slept yet / wake withheld from a thread that has" is one defect
— a read that is not serialized with the committer's sleep transition — and
the search finds the second member of the pair. The first is the phrase #589
gives the same mutant; the model does not claim to have a witness for it,
because with the write group's locking, `committer_sleeping_ == true` read in
a critical section means the committer has already released `queue_mu_` inside
`wait()` and the notify reaches it.

## Results

| model | N = 6 | N = 5 | N = 4 | N = 3 | N = 2 |
|---|---|---|---|---|---|
| write group, reference | 142 045 states (36 ms) | 24 157 | 3 975 | 619 | 85 |
| flush role, reference | — | 287 047 states (79 ms) | 15 271 | 959 | 73 |

Every reference run is clean: no invariant violated, no state that leaves a
writer with nothing that could wake it, every size reaching states where all
writers have returned (`done`). The flush model stops at 5 writers — it
explores a flush that lands *and* one that throws at every flush — and the
whole self-check is 0.13 s. `kWGSizes` and `kFlushSizes` in `main.cpp` raise
the sizes for a deeper local run; `RunWGAt` and `RunFlushAt` instantiate what
those lists name.

## Out of scope

Deliberate, and each is a property of the code that is not coordination
between these flags:

- **One submit per writer.** The hand-over is per batch; a writer that submits
  twice only adds a second lap of the same protocol.
- **Barriers.** `quiesce`, `barrier_waiters_` and `FlushRole` are not modelled:
  they take the same role with the same release. The mutation set keeps a
  separate guard for the barrier's throw path.
- **The settle wait and `CommitDelay`** (db_commit.cpp:421-452,
  concurrency.cppm:377): timing heuristics that change which slots a flush
  covers, never who is waiting or who wakes them.
- **Spurious wakeups.** A spurious wake would let a lost notify be rescued by
  luck, and the checks are about the protocol itself. That is also why the
  `--mutant` runs must fail rather than be likely to fail.
- **Where the release is folded.** `run_batch` marks a slot done under
  `queue_mu_` and `release` notifies its owner outside it; the model does both
  at `run`, since between them the owner does nothing either invariant can
  see. `Slot::state`'s `kReleasing → notify → kDone` order exists for the
  owner's lifetime, not for the hand-over.
- **The memory model beyond the locks.** Each model step is atomic, which is
  what holding `queue_mu_` (or `durable_mu_`) gives the code; the atomics'
  orderings are what keep each flag inside its critical section, and the
  mutants above are the places where they must.
- **The transcription.** These are models of the protocol, not the engine
  driven through an adapter: a defect in the transcription would hide here and
  show up in `tests/concurrency_test.cpp`. The six behaviour tests of
  `docs/single_wait_commit_design.md` § *Tests*, and the existing pipeline
  suite, remain the check on the code itself.

## Files

| file | what |
|---|---|
| `explorer.h` | the exhaustive search: every interleaving, states deduplicated by key, invariants after every transition, a hang reported as a state with no enabled step |
| `writegroup_model.h` | model 1 and its three mutants |
| `flush_model.h` | model 2 and its one mutant |
| `main.cpp` | the self-check: the reference must be clean at every size, every mutant must be caught |