# Deferred state reclamation — design

Status: **final**. Ready for implementation, gated by the benchmark decision
rule in [Plan](#plan).
Date: 2026-09-10 (proposed), 2026-09-10 (reviewed and finalised)
Tracking: BC-244. Prerequisite: BC-243 (see `docs/bytecask_project_plan.md`).

## Problem

`perf` profiling of the MariaDB plugin under `sysbench oltp_insert` (see
`bytecaskdb-mariadb-plugin/docs/sysbench_perf_gap_analysis.md`) shows that
**~34% of on-CPU time on the client thread is spent recursively freeing the
previous radix-tree generation**, and a further slice of the write path is the
mirror allocation cost.

Mechanism: every committed write path-copies the spine from the radix root to
the touched leaf, producing a new `EngineState`. When that new state is
published and the previous `EngineState`'s last `shared_ptr` owner drops it,
the destructor chain runs synchronously on whatever thread did the drop:

```
~EngineState → ~PersistentRadixTree → IntrusivePtr<Node>::~IntrusivePtr()
             → Node::release()   // iterative, but still:
                                 //   - one atomic fetch_sub per child slot
                                 //     of every superseded spine node
                                 //   - free() per superseded node
                                 //   - cache misses walking cold child vectors
```

`perf annotate` of `Node::release()` splits the cost roughly: `free()` ~40%,
child-vector pointer-chasing ~24%, atomic refcount + dependent branch ~11%.

Where the last reference is dropped today, in the single-connection
`oltp_insert` case (one client thread is both writer and reader):

1. **The writer**, at the end of `execute_slots`, when its local `current`
   copy goes out of scope — *not* inside `store_state`, because `current` is
   still alive when the new state is published.
2. **The reader**, inside `load_state_for_read`, when the thread-local cache
   is refreshed (`staleness_tolerance == 0` → refresh on every write) and
   that thread's cache held the last reference. This is the dominant site in
   the profile.

The work is intrinsic — one generation's superseded spine must be freed per
committed write — but it does not have to run on the latency-critical thread.

### Caveat on the measurement

The profile was taken on a linuxkit/arm64 VM where atomics are slow, which
inflates `release()`. On x86 the free is cheaper and the handoff described
below (mutex + possible futex wake) is a larger fraction of it. **Whether this
change is a net win on x86 is an open empirical question**; the benchmark
gate in [Plan](#plan) decides it. The design is written so that a negative
result is cheap to revert.

## Goal

Move `EngineState` destruction (dominated by the radix-tree spine free) off
the writer and reader critical paths onto a dedicated background thread,
bounded so a write burst cannot grow memory without limit.

Non-goals: reducing the *total* CPU cost of the free (ChildStore / refcount
rework, tracked separately), changing the on-disk format, or changing the
radix tree, `IntrusivePtr`, or `Node::release()`.

Consistent with design principle #3 (predictable latency over peak
throughput).

## Decisions

All decisions below are final. Rows marked **revised** changed from the
original proposal as a result of review against the code.

| # | Decision | Final | Notes |
|---|----------|-------|-------|
| D1 | New `StateReclaimer` class vs reuse `BackgroundWorker` | **new class** | allocation-free enqueue, bounded queue, no exception plumbing, isolated from hint I/O |
| D2 | Dedicated thread vs share `worker_` | **dedicated thread** | a multi-ms hint `fdatasync` must not stall retirements (→ memory), nor vice versa |
| D3 | Backpressure when the queue is full | **destroy inline on the caller** | bounded memory, non-blocking; degrades to today's behaviour exactly when deferral cannot keep up |
| D4 | `kMaxQueue` | **256**, compile-time constant — *revised from 1024* | the queue absorbs jitter, not a sustained backlog; if the reclaimer is hundreds of generations behind, deferral has already stopped helping. The cap bounds *count*, not bytes (see D4 note) |
| D5 | Handoff predicate | **`dead.use_count() == 1`** | sole-owner check is a relaxed atomic load; cheaper than the work it gates |
| D6 | Hook `~Snapshot` | **no** | `Snapshot` is a movable value with no back-pointer to `DB`; its generation is nearly always co-held by the creating thread's cache, which is the real last releaser (hook 2) |
| D7 | Reclaimer scope | **one per `DB`** | simplest ownership/shutdown. `EngineState` is self-contained, so destroying a state after its `DB` is gone is memory-safe — a process-wide reclaimer would also be safe if ever wanted |
| D8 | `drain()` call sites | **`~DB` only** | the reclaimer touches no files and no shared on-disk state; only shutdown ordering and test determinism need it |
| D9 | What is retired | **the whole `EngineState`** | also moves the last `DataFile` `shared_ptr` drop (`::close(fd)`, `munmap`) off the critical path |
| D10 | Consumer wake policy — *new* | **notify only if the consumer is asleep**; no spinning | a spinning background thread burns a core, which worsens the write-saturation risk this design already carries. See D10 note |
| D11 | Publish API — *new* | **single publish point: `store_state(std::shared_ptr<const EngineState>&& old, std::shared_ptr<EngineState> new)`**; the one-arg overload is removed | the rvalue-reference parameter makes "caller forgot to hand over its copy" a compile error, not a silent inline free. See Hook 1 |
| D12 | Observability — *new* | **two counters**: `states_retired`, `states_retired_inline`, exposed via `stats()` | makes the test plan deterministic; replaces RSS sampling |

### D4 note — the cap bounds count, not bytes

A generation's retained set is the superseded spine — for a single-key put a
handful of nodes, well under a KB. But a `WritePlan` touching 100 k keys
supersedes a large fraction of the tree, so 256 such generations could be
hundreds of MB. This is acceptable for v1 because the reclaimer only falls
that far behind if the writer sustainably outruns a whole core of freeing,
and the inline fallback then caps growth. A byte-based bound is a follow-up
if `states_retired_inline` ever fires under a realistic workload.

### D10 note — wake policy

`retire()` pushes under the mutex and reads a `sleeping_` flag set by the
consumer around its `cv.wait`. It calls `notify_one` only when the consumer
is asleep. Under a sustained write rate the consumer is awake and draining,
so the futex syscall mostly disappears; at low rates the syscall is noise
next to `fdatasync`. The regime that can lose is the mid-rate single
`NoSync` writer, where each commit finds the consumer asleep — that is what
`Put/NoSync` measures. Spinning or a timed wait is an alternative only if
that row shows a regression.

## Prerequisite — BC-243: thread-local state cache is shared across `DB` instances

Status: **done**, landed ahead of the reclaimer.

`load_state_for_read` caches the current generation in a function-local
`thread_local` keyed by nothing. Two `DB` instances read from the same thread
cross-contaminate: after `b.get(k)` then `a.get(k)`, `a` returns **B's value**
if B's last publish is newer than A's (verified with a scratch test, then
with a permanent regression test — see below). It also means a closed DB's
final `EngineState` — and its `DataFile` fds/mmaps — stays alive in whichever
thread read it last, past `~DB`.

This is a correctness bug independent of this design, but hook 2 lands on
exactly this code, and without the fix `retire_state` on DB A could receive
DB B's generation. Landed as its own commit, ahead of the reclaimer:

```cpp
struct TlState {
  const DB *owner{nullptr};
  std::shared_ptr<const EngineState> snapshot;
  std::int64_t last_write_time{0};
};
// ...
if (tl.owner != this) {
  tl.snapshot.reset();      // another DB's generation: free inline, never cross-retire
  tl.owner = this;
  tl.last_write_time = 0;
}
```

Regression test: `tests/bytecask_test.cpp` "BC-243: thread-local read cache
does not leak across DB instances" (`[tl-cache]`) — opens two `DB`s in the
same directory tree root, writes a shared key with different values plus a
key exclusive to one DB, interleaves reads on one thread, and asserts each DB
reports only its own data. Confirmed to fail without the fix (`from_b` where
`from_a` was expected, and `contains_key` returning true for a key that
exists only in the other DB) and to pass with it. Full suite green: 1,445
test cases, 19,777,209 assertions.


Single-DB hot path gains one pointer compare. Known benign residue: if a `DB`
is destroyed and a new one is allocated at the same address, the stale entry
is refreshed on first read because `state_time_` is `steady_clock`-based and
strictly newer. `~DB` does not clear other threads' caches; that is
pre-existing and unchanged.

Test: the two-DB interleaved read scenario above, plus `a.contains_key(k)`
for a key only in B.

## Design

### `StateReclaimer`

A small class in `bytecask.concurrency`, modelled on `BackgroundWorker`'s
lifecycle but purpose-built:

```cpp
export class StateReclaimer {
public:
  StateReclaimer();               // starts the thread
  ~StateReclaimer();              // stop, drain remaining queue on the worker
                                  // thread, join
  StateReclaimer(const StateReclaimer&) = delete;
  StateReclaimer& operator=(const StateReclaimer&) = delete;

  // Hand a retired state to the background thread for destruction.
  // Returns true if deferred. If the queue is at capacity, destroys `dead`
  // inline on the caller (outside the lock) and returns false.
  // Precondition: dead.use_count() == 1 (checked by DB::retire_state).
  [[nodiscard]] auto retire(std::shared_ptr<const EngineState> dead) noexcept -> bool;

  // Block until the queue is empty and the in-flight destruction (if any)
  // has finished. For ~DB ordering and tests.
  void drain();

private:
  void run();

  std::mutex mu_;
  std::condition_variable cv_task_;
  std::condition_variable cv_idle_;
  std::deque<std::shared_ptr<const EngineState>> queue_;
  bool sleeping_{false};   // consumer is in cv_task_.wait — retire() notifies only then
  bool busy_{false};       // consumer is destroying an element outside the lock
  bool stop_{false};
  static constexpr std::size_t kMaxQueue = 256;
  std::thread thread_;
};
```

Why not a second `BackgroundWorker`:

- **Allocation-free enqueue.** `BackgroundWorker` queues `std::function`;
  each `dispatch` may heap-allocate. A typed deque never allocates per
  retire beyond its own chunking.
- **Backpressure.** `BackgroundWorker`'s queue is unbounded (fine for rare
  hint flushes); retirement is once per commit.
- **No exception plumbing.** `~EngineState` is `noexcept`.
- **Isolation from hint I/O.** One FIFO would let a hint `fdatasync` stall a
  retirement backlog and vice versa.

Returning `bool` keeps the reclaimer free of a dependency on
`bytecask.counters`; `DB::retire_state` bumps the counter.

Consumer loop sketch:

```
lock
loop:
  while queue empty and not stop: sleeping_ = true; wait; sleeping_ = false
  if queue empty and stop: return                 // drained before exit
  s = move(front); pop; busy_ = true
  unlock; s.reset(); lock                         // destruction outside the lock
  busy_ = false
  if queue empty: cv_idle_.notify_all()
```

`drain()` waits for `queue_.empty() && !busy_`.

### Hook points

Both in `bytecask.cppm`, routed through one helper:

```cpp
mutable StateReclaimer reclaimer_;   // mutable: hook 2 runs from const read paths

void retire_state(std::shared_ptr<const EngineState> dead) const noexcept {
  if (!dead || dead.use_count() != 1) return;   // another owner drops it later
  if (reclaimer_.retire(std::move(dead)))
    counters_.states_retired.fetch_add(1, std::memory_order_relaxed);
  else
    counters_.states_retired_inline.fetch_add(1, std::memory_order_relaxed);
}
```

**Hook 1 — the single publish point.** Today every publisher holds a local
`current` (from `load_state_for_write()`) that outlives `store_state`, so
`use_count() >= 2` at publish and the last drop happens at scope exit on the
writer. The fix is to make publishing *consume* the caller's reference:

```cpp
// Sole publish point. Caller must hold write_mu_ and pass its own reference
// to the outgoing state, which is retired after the new one is visible.
void store_state(std::shared_ptr<const EngineState> &&old_state,
                 std::shared_ptr<EngineState> new_state) {
  // O(1) invariant checks unchanged ...
  assert(old_state == std::atomic_load(&state_));   // debug: caller passed the live generation
  std::atomic_store(&state_, std::move(new_state));
  state_time_.store(now_ns(), std::memory_order_release);
  // notify / counters unchanged ...
  retire_state(std::move(old_state));               // old_state is our only ref to prev
}
```

No `atomic_exchange` is needed: under `write_mu_` the caller's `old_state`
*is* the outgoing generation, so after the store its `use_count()` is 1 iff
no `Snapshot` or thread-local cache holds it. This also removes the open
question about a new deprecated-API surface.

Every call site becomes `store_state(std::move(current), …)`. Sites:
`execute_slots` (success and the three error paths), `vacuum_commit`,
`set_mode`, `deem_as_degraded`, `resume`, `create_manifest` (both),
`ingest` (error paths and final). The one-arg `store_state(new)` overload is
deleted; the error paths that used it with `err_t = current->transient()`
pass `std::move(current)` like everyone else (their invariant checks pass
trivially — the degraded state copies every checked field). `deem_as_degraded`
is invoked from inside `store_state` on invariant failure; it loads its own
`current` and publishes through the same function, with the outer frame's
`old_state` still alive → the inner retire skips (`use_count() > 1`) and the
outer `old_state` frees inline. That is a cold path and acceptable.
`store_initial_state` stays as is (no previous generation).

**Hook 2 — thread-local refresh** (on top of the BC-243 fix):

```cpp
if (wt - tl.last_write_time > tolerance) {
  auto dead = std::exchange(tl.snapshot, load_state());
  tl.last_write_time = wt;
  retire_state(std::move(dead));
}
```

The reference returned still points at `tl.snapshot`; the refresh completes
before any traversal, so `dead` is genuinely unreferenced by the caller.

**Not hooked: `~Snapshot`** (D6). A `Snapshot` that outlives many cache
refreshes — a long scan, a plugin multi-statement transaction — frees inline
on destruction. Not a hot loop; revisit if a read-heavy profile shows it.

### Lifecycle and shutdown

- `reclaimer_` is declared **after** `worker_` in `DB`, so it destructs and
  joins first. Neither depends on the other; both finish before `state_`,
  the file registry and `dir_` are torn down.
- `~StateReclaimer` sets `stop_`, notifies, and the loop drains the queue
  before exiting. Queued generations are freed on the reclaimer thread.
- The **final** generation (`state_`, the whole tree — the largest free of
  all) is freed by `~DB`'s member destruction, or later by a reader
  thread's cache (pre-existing). This design does not change that.

### Single-threaded / WASM builds

Under `BYTECASK_SINGLE_THREADED`, like `BackgroundWorker`, `StateReclaimer`
spawns no thread: `retire()` destroys inline and returns false; `drain()` is
a no-op. Behaviour identical to today except the `states_retired_inline`
counter advances.

## What does not change

- `Node::release()`, `IntrusivePtr`, `ChildStore`, node layout.
- On-disk format, hint files, recovery. A retired state is purely in-memory.
- Durability before visibility: `state_.store()` still happens after
  `fdatasync`; the retire of the *previous* state happens strictly after.
- Reader load path: readers still `atomic_load(&state_)` with no lock.
  **Precision:** the sole-owner refresh in hook 2 takes the reclaimer mutex
  once per generation on one reader thread. Reads themselves remain
  lock-free.

## Correctness considerations

- **Sole-ownership handoff.** No `weak_ptr` exists in the codebase, and
  every way to hold a generation is a counted copy (`Snapshot`, `WritePlan`,
  `tl.snapshot`, a publisher's `current`). `load_state_for_read` returns a
  `const shared_ptr&` into the caller's own `thread_local`, so it cannot be
  a hidden owner on another thread. After the store, `state_` no longer
  points at the old generation, so nobody can acquire a new reference.
- **Concurrent refcount drop vs. `ensure_mutable`.** The reclaimer decrements
  a node shared with the live generation from 2→1 (`fetch_sub`, acq_rel)
  while the writer's transient may check `refcount_ == 1` (acquire) on the
  same node. This is safe: the tag check clones any node from a previous
  generation regardless, and if the tag matched, `refcount_ == 1` means sole
  ownership. **This race already exists today** — reader threads drop
  snapshots concurrently with the writer — so no new hazard is introduced.
- **Memory ordering.** The mutex in `retire()`/`run()` provides the
  happens-before edge between the retiring thread and the destruction. TSan
  run required.
- **Degraded / follower / vacuum paths.** All publish through the single
  `store_state`, so they are covered uniformly. Vacuum holds a `snap` for the
  duration of its I/O, so generations published during a vacuum pass have
  `use_count() > 1` and free inline when `snap` drops on the vacuum thread
  — cold, acceptable.
- **Allocator interaction.** Nodes allocated on the writer are now freed on
  another thread (jemalloc remote frees go to the owning arena, not the
  writer's tcache). The writer loses the immediate tcache reuse it gets
  today. This is part of why the benchmark, not the profile, decides.
- **Model-based recovery tests.** No on-disk change, so serial and parallel
  recovery cannot diverge. They still exercise the path (close after
  thousands of ops → drain in `~DB`); run them.

## Test plan

1. **BC-243 regression test** (`tests/bytecask_test.cpp`, `[tl-cache]`): two
   DBs, interleaved reads on one thread, values and `contains_key` correct.
2. **`StateReclaimer` unit tests** (`tests/concurrency_test.cpp`,
   `[reclaimer]`), using an instrumented `EngineState` destructor counter or
   a test-only value type:
   - `retire` past `kMaxQueue` returns false and destroys inline; queue
     never exceeds the cap.
   - `drain()` blocks until all queued states are destroyed.
   - Destructor drains the queue; destructor count equals enqueue count.
3. **Engine integration** (`tests/bytecask_test.cpp`):
   - N single-key puts with a read after each on the same thread:
     `stats()["bytecask.states_retired"] == N` after `drain()` (or after
     close and reopen for the observable equivalent),
     `states_retired_inline == 0`.
   - N puts with no reads (writer path only): same assertions — this is
     what proves hook 1 fires.
   - A `Snapshot` held across writes: those generations are **not**
     retired until the snapshot drops (counter unchanged while held).
   - `[model]` random / batch-heavy / delete-heavy suites under serial and
     parallel recovery, key/values and `file_stats` matching the serial
     baseline.
4. **Sanitizers**: `xmake f --sanitizer=thread` on `[concurrency]`,
   `[reclaimer]`, `[tl-cache]`; ASan on the retire/drain lifecycle.

RSS sampling and "live `Node` count returns to baseline after `close()`" are
dropped: the former is flaky by nature and the latter is defeated by
thread-local caches holding the final generation.

## Benchmark plan

Run on x86_64 (this codespace qualifies; `scripts/run_engine_bench.py`'s
hardcoded `x86_64` path is fine here). `--full` (1M keys). Before/after rows
into `benchmarks/engine_bench_results.csv`.

| Benchmark | What it isolates | Expectation / rule |
|---|---|---|
| `ByteCaskDB/Put/NoSync` | **hook 1 alone** (no reads) — handoff cost vs. freed work at mid-rate | the decision row; see rule below |
| `ByteCaskDB/Put/Sync` | hook 1 under real fsync | small win or neutral |
| `ByteCaskDB/PutMT/Sync` @ 2–64T | write saturation; reclaimer competes for a core | neutral low/mid T; 64T within noise (±3%) |
| `ByteCaskDB/Del/Sync`, `ByteCaskDB/MixedBatch/Sync` | other publishers | no regression |
| `ByteCaskDB/MixedMT`, `ByteCaskDB/ReadAndWriteLoad` | **hook 2** on reader threads | possible read win; no regression |

Then re-run the `oltp_insert` `perf` profile with the plugin unchanged to
see how much of the ~34% left the client thread.

**Decision rule.** Keep the change if `Put/NoSync` p50 and p99 improve or
are within noise **and** `PutMT/64T` is within noise **and** `oltp_insert`
throughput improves. If `Put/NoSync` regresses, the handoff costs more than
the free on x86: try the D10 alternative (timed wait) once; if it still
regresses, revert the reclaimer (keep BC-243 and D11 — both are
improvements on their own) and pursue the ChildStore/refcount rework
instead.

## Resolved questions

Formerly "open questions" in the proposal.

1. **Throughput at full write saturation.** Resolved by D10: no spinning, so
   the reclaimer's CPU cost is at most one core actively freeing, which is
   work the writer would otherwise do itself. At saturation the consumer is
   usually awake, so the per-commit cost is one uncontended mutex. Guarded
   by the `PutMT/64T` row with a ±3% noise band; no heuristic gating unless
   that row fails.
2. **`kMaxQueue` validation.** Resolved by D4 (256) and D12: the
   `states_retired_inline` counter shows deterministically whether the cap
   was ever hit, in tests and in benchmarks. Revisit the value only if it
   fires under a realistic single-writer load.
3. **Deprecated `shared_ptr` atomic API surface.** Dissolved: D11 needs no
   `atomic_exchange`. `use_count()` is not deprecated. The existing
   `atomic_load`/`atomic_store` remain under the existing suppression.

## Residual risks

- **Net effect on x86 is unmeasured.** Covered by the decision rule. The
  design is structured so a negative result reverts one class and two hooks,
  leaving BC-243 and D11 in place.
- **`~Snapshot` inline frees in snapshot-heavy read workloads** (D6).
  Revisit with a profile.
- **Count-based cap** (D4 note). Byte-based bound is a follow-up if the
  inline counter ever fires in practice.

## Plan

Each step is one commit, reviewed before committing. Steps 1–2 are
worthwhile independently of the outcome of step 4.

| Step | Work | Acceptance |
|---|---|---|
| 0 | **Baseline.** `python3 scripts/run_engine_bench.py --full` on this x86_64 codespace; `oltp_insert` sysbench run + `perf` profile with the current plugin. | Rows in `benchmarks/engine_bench_results.csv`; profile saved alongside the gap analysis. |
| 1 | **BC-243** — owner-keyed thread-local cache in `load_state_for_read`; `[tl-cache]` test. | ✅ **Done.** New test fails before, passes after; full `bytecask_tests` green (1,445 cases, 19,777,209 assertions). |
| 2 | **D11** — `store_state(&&old, new)` as the sole publish point; delete the one-arg overload; `std::move(current)` at every site; debug `assert(old_state == atomic_load(&state_))`. Pure refactor, no behaviour change. | Compiles with `-Weverything`; full tests + `[model]` green. |
| 3 | **BC-244** — `StateReclaimer` (D1–D5, D10) in `bytecask.concurrency` with the `BYTECASK_SINGLE_THREADED` variant; counters (D12) in `Counters` and `stats()`; `retire_state` + hooks 1 and 2; `reclaimer_` member after `worker_`; `[reclaimer]` unit tests; engine integration tests; `[model]` suites; TSan + ASan runs. | All tests green under release, TSan, ASan. `states_retired_inline == 0` in the single-writer integration test. |
| 4 | **Benchmark gate.** Re-run step 0; apply the decision rule. | Before/after CSV rows shown; `oltp_insert` profile re-taken. |
| 5 | **Docs** (with step 3 or 4): `docs/bytecask_design.md` (state publication, reclaimer, shutdown ordering, counters), `README.md` (counter list under Operational counters), `docs/bytecask_project_plan.md` (BC-243/BC-244 to Done; follow-ups to Backlog). | Docs match shipped behaviour. |
| 6 | **Follow-ups → Backlog.** Right-size the plugin change (P3) from the new profile; ChildStore/refcount rework; byte-based queue bound if warranted; `~Snapshot` hook if a read-heavy profile shows it. | Entries in `docs/bytecask_project_plan.md`. |
