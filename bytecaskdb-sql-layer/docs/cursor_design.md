# Cursors in the SQL layer

Status: **design, not approved** (written 2026-10-28 for #663, sqltest
E121; revised the same day for the owner's replay proposal). Nothing is
built. The owner's direction: a cursor is evaluated lazily; holding the whole
result in memory is not the design. The first phases build a **replay
cursor** (the snapshot and a position; each `FETCH` re-runs the plan to its
position), and a resumable run replaces it later where replay is too slow.
The decisions that need the owner are listed under *Owner decisions*; the
phases, with their issues, under *Phases*.

## What a cursor is here

`DECLARE name [BINARY] [INSENSITIVE] [[NO] SCROLL] CURSOR [{WITH | WITHOUT}
HOLD] FOR query` names a query. `FETCH` and `MOVE` read it a few rows at a
time in a direction, `CLOSE` ends it. This is PostgreSQL's statement set, and
standing rule 1 of `transaction_design.md` applies: PostgreSQL decides the
behaviour, the layer decides how it is built.

A cursor is a named portal of the session (`server_design.md`, *A session*).
It shares the portal namespace: a used name is `42P03`, an unknown one
`34000`. It ends with its transaction, as a portal does, unless it is `WITH
HOLD`.

What is refused: `BINARY` (binary results are `0A000` already), and every
form the dialect does not support in the query. `INSENSITIVE` and
`ASENSITIVE` are accepted and change nothing: PostgreSQL's cursors are
insensitive, and so are these.

## What the sqltest ids need

The 21 E121 ids (`tests/functional/suites/sqltest/standards/2016/E/E121-*.tests.yml`)
are each a `CREATE TABLE` followed by one to four cursor statements, on an
empty table. The harness (`tests/functional/executor.py`, `loaders/sqltest.py`)
sends each statement as its own `exec_params` on one connection, with no
`BEGIN`, "as sqltest wrote it" (`functional_testing_design.md`, decision 13).
A case passes when every statement is accepted.

That decides most of them. Against PostgreSQL 17's rules:

| ids | Statements | Why they cannot pass as written |
|---|---|---|
| e121_01_01_01 to _04, e121_02_01_01, e121_03_01_01, e121_17_01_02 | `DECLARE` without `HOLD` | PostgreSQL: `25P01`, "DECLARE CURSOR can only be used in transaction blocks". The harness never opens a block. |
| e121_17_01_01 | `DECLARE … WITH HOLD` | Passes in PostgreSQL. |
| e121_04_01_01, e121_08_01_01 | `OPEN c` | Not PostgreSQL. It has no `OPEN` (a `DECLARE` opens), so it is a syntax error there. |
| e121_10_01_01 to _08 | `FETCH … INTO <table>` | Not PostgreSQL. SQL-level `FETCH` has no `INTO`; the standard's takes host variables, and sqltest names a table. |
| e121_06_01_01, e121_07_01_01, e121_07_01_02 | `UPDATE`/`DELETE … WHERE CURRENT OF c` | The cursor was never fetched, so it is not positioned on a row: `24000` in PostgreSQL. `DELETE FROM ONLY ( t )` is in PostgreSQL's grammar (`relation_expr`), so far as I know; it is not the obstacle, and phase 2 confirms it with a parse test. |

(I did not run the sequences against PostgreSQL, as #663 says; this is from
its documented rules and grammar. The phase that adds the syntax records each
case from PostgreSQL 17 before relying on it.)

So with PostgreSQL's semantics kept, **one of the 21 ids can pass**. The
rest need the layer to accept something PostgreSQL does not. The cheapest
extension that gives a useful result is to accept `DECLARE` outside a block
and make the cursor `WITH HOLD` (decision 1). That is a superset: it turns
an error into a working cursor, and no PostgreSQL program changes meaning.
With it, **eight ids pass**: e121_01_01_01 to _04, e121_02_01_01,
e121_03_01_01, e121_17_01_01 and _02. The other 13 stay marked, because
passing them means inventing syntax (`OPEN`, `INTO`) or a success
PostgreSQL does not give (`WHERE CURRENT OF` on an unpositioned cursor). The
ids are not why cursors are worth building: the real value is clients that
use server-side cursors (psycopg, JDBC fetch size), which phases 3 to 6 and 11
serve and the functional suite's own cases, recorded from PostgreSQL, test.

## Why a plan cannot pause today

- A plan is one C function, `void plan(bc_ctx *)`, compiled by TinyCC
  (`plan/codegen.cppm`, `print_plan`). It runs to its end inside
  `bc_ctx_run`.
- Codegen is produce/consume (`planner_design.md`, *The physical plan and
  codegen*): everything between two pipeline breakers is one nest of `while
  (bc_scan_next(...))` loops, the row's values in C locals, `bc_emit_*`
  deep inside the innermost loop. A pause has to keep those locals, the
  loops' positions, and the API handles they hold.
- `bc_emit_end` appends the row to the context's result buffer, charged to
  the statement memory. `Database::results` turns the buffer into `Rows`
  after the plan returns. Nothing leaves the plan before it ends.
- The context keeps everything else a pause must keep: the handle tables,
  the arena marked per loop step, the cursors' engine iterators
  (`ViewEntries`: the snapshot's iterator merged with the overlay's), the
  sorters, hash tables and aggregates. All of it is an index into tables of
  the context, valid until `bc_ctx_reset`.
- Errors are sticky and every call after one returns a neutral value, so a
  plan can be unwound from outside by failing its context.

The plan language has no structs, arrays or pointers, which is why produce/
consume was chosen over iterators. That matters below.

## Ways to run a plan a few rows at a time

Each is judged on what it changes, which plans it serves, what it holds
between FETCHes, and the worker pool (#642, #644): a cursor between FETCHes
must hold no worker.

**(a) Re-entrant codegen.** The plan becomes a state machine: `int
plan_next(bc_ctx *)` returns at each emit with a label number, and the next
call jumps back in. The locals must survive the return. Plan text cannot
make them `static` (one compiled plan is shared by every context running it),
and the subset has no struct to put them in, so each live local would be
saved into new per-context slots (`bc_save_*`/`bc_load_*`) at every emit and
restored at every resume label. Codegen's `Writer` is a line printer; it does
not track which locals are live where, so this means a scope model through
all of `Gen` (2,400 lines) and `SetGen`. It serves every shape and fits the
pool. Cost: the largest change on the list, in the part of the system with
the most tests, and every later operator must be written in two forms.

**(b) Restart from a saved key (keyset resume).** Keep the snapshot and the
last scan key of the outermost loop; a FETCH re-runs the plan with that
loop's start moved to the key. Memory is a few keys. Valid only where output
order is the outer scan's key order and nothing carries state from one outer
row to the next (*Checkpoints*, below). It is not a mechanism for every
plan; it is the optimisation that makes (e) cheap for the common cursor.

**(c) The plan on a parked thread.** The cursor owns a thread that runs the
unchanged plan; `bc_emit_end` blocks on a condition variable after the
batch, and FETCH wakes it. No codegen or plan-API change, every shape,
blocking operators materialise once and then stream. It holds a thread per
started cursor, which is the thread-per-connection cost #642 removed, now per
cursor; it needs a global thread bound; shutdown must join them. It works
under every sanitizer.

**(c′) The plan on its own stack (a stackful coroutine).** The same as (c)
without the thread: the plan runs on a stack of its own (`makecontext` and
`swapcontext`, or the equivalent) allocated at the cursor's first FETCH;
`bc_emit_end` swaps back to the caller when the batch is full; the next FETCH
swaps in, on whichever worker thread asked. No codegen change. A cursor
between FETCHes holds a stack and its context, no thread and no worker.
Hazards threads do not have: a frame that lives across a suspend must hold
no `thread_local` address, since the next resume may run on another thread;
ASan and TSan need fiber annotations and MSan has no equivalent; `ucontext`
is glibc and macOS, not musl; a suspended engine iterator may keep a
buffer-pool frame leased (engine design, *L — Frame lease*).

**(d) Run to completion and slice the buffer.** The option 1 of #663: the
whole result in memory. Rejected by the owner.

**(e) Replay against the pinned snapshot** (owner proposal, 2026-10-28).
A cursor keeps no execution state. Its state is the pinned view, the plan
and a position `p`, the number of rows already returned. A `FETCH` of k rows
runs the plan from its start against that same view, drops the first `p`
rows without encoding them, keeps the next `k`, and stops the run. Stopping
is a flag the runtime sets after the last wanted row; every API iteration
then reports its end, as the sticky error already makes them (*Model*,
`plan_api_design.md`), and the run returns without an error.
- *Changes:* codegen none; `bc_plan.h`'s plan-visible calls none. The runtime
  gains, for the server only, a run that takes a skip count and a row
  limit, and a start key for the outermost scan (*Checkpoints*).
- *Shapes:* all, because the plan is deterministic (below).
- *Holds:* the snapshot, the overlay copy, the bound parameters, the compiled
  plan and a counter: no stack, no thread, no iterator, no context between
  FETCHes (a context is taken from the session's, as a statement takes it).
- *Pool:* the best fit of all: a cursor between FETCHes is data.
- *Cost:* work proportional to the position, every FETCH (*Cost of
  replay*). Memory stays constant; time does not.

| | Codegen change | Shapes | Holds between FETCHes | Cost per FETCH | Worker pool |
|---|---|---|---|---|---|
| (a) state machine | all of `Gen`, `SetGen` | all | saved locals, context | O(k) | fits |
| (b) keyset resume | scan bounds | outer-scan pipelines | snapshot, a key | O(k) | fits |
| (c) parked thread | none | all | a thread, context | O(k) | a thread per cursor |
| (c′) coroutine | none | all | a stack, context | O(k) | fits |
| (e) replay | none | all | data only | O(p + k); O(k) with (b) | fits |

**Recommendation: (e) first, with (b) as its checkpoint; (c′) later.**
- Replay is the only option whose new machinery is a counter, a flag and a
  start key, all inside the runtime and none in a place a thread or a stack
  switch could hurt: no sanitizer, thread-local or stack-size question, no
  parked state to leak at disconnect. It is lazy in the owner's sense: rows
  are read from the snapshot as the cursor reaches them, and nothing is held
  but the view. It is correct by the one property below (determinism), which
  a test can check for every plan.
- Its cost is real: a cursor read sequentially is quadratic in the worst
  case. (b) removes that for the cursor most clients open (an ordered scan,
  an index join), a result cache (*Blocking operators*) for the small sorted
  one. What stays slow is a large cursor over a blocking operator, and
  streaming (#487), where each batch would redo the whole run.
- Those need a run that can pause: (c′) behind the same interface the replay
  cursor has (`open`, `fetch`, `close`), so it replaces replay for the shapes
  that need it without the session or the API changing. Which of (c), (c′) or
  (a) is a spike (phase 9), and it is taken when a measurement or #487 asks.

The determinism of plans, the one property replay rests on, comes first.

## The replay cursor

**Determinism.** A replay returns the rows the first run returned only if a
plan's output order is identical on every run against the same view,
parameters and clock values. Read from the runtime (`algebra/plan_api.cpp`):

| Source of order | Deterministic? |
|---|---|
| Scans, forward and reverse, tables and indexes | Yes: the snapshot's key order merged with the frozen overlay's. |
| `Sort` | Yes: `bc_sorter_*` returns equal keys in the order added (`CONTRACT.md`), and the order added is the input's. PostgreSQL promises no order among ties; replay promises its own earlier one. |
| Hash join | Yes: `bc_hash_next` walks a key's chain in the order put. The table's `unordered_map` is only ever looked up by key, never iterated. |
| Group structures, `DISTINCT`, set operations | Yes: `bc_agg_next` walks groups in first-seen order (a vector); the `std::map` is a lookup. |
| Choice points | Yes: they test `bc_count_range` of the frozen view against a threshold printed in the plan. |
| Time | The plan cannot see the clock; `CURRENT_DATE` and kin are hidden parameters filled before the run (`frontend_design.md`, *The time*). Pinned below. |
| Randomness, addresses, threads | None in a plan. |
| Errors | An error at row 7,000 recurs at row 7,000: the same rows are computed. A work budget or deadline is not a property of the data; a replay that exhausts one fails the `FETCH`, it never returns different rows. |

So no plan shape needs refusing or an order forced. The property is a fact
about today's operators, not a rule the code states, and the design does not
rely on discipline: phase 1 adds a test that runs every codegen test plan
twice on one view, and with the batch sizes of the cursor tests, and
compares the result bytes. An operator that iterates an unordered container
fails it.

**What `DECLARE` pins.** Everything the plan reads or is given:

| Pinned | How |
|---|---|
| The snapshot | The transaction's, which never moves while it lives. |
| The transaction's overlay as of `DECLARE` | Shared and copy-on-write (*What a cursor sees*). A replay must see the same overlay every time, so the guarantee is the same one. |
| The parameters | The bound values, kept with the cursor, plus the hidden parameters the API fills: `Statement::fixed` entries for the clocks (`bound::ClockSlot`, `api/database.cppm`) are computed once, at `DECLARE`, from the transaction's time (`started_us`) and never refilled at a `FETCH`. Within a transaction they are constant anyway; pinning them keeps a cursor right if a clock slot is ever read per statement (#700). |
| The catalog version | The transaction's pinned `Catalog`. |
| The compiled plan | The `Compiled` the `DECLARE` ran, held by the cursor, so a recompile (a refit after another session's DDL, a cache eviction) cannot change the plan or its order between FETCHes. |
| The memory and work budgets | The same values on every run, so a choice point and a `54000` fall the same way. The work budget is the cursor's replay cap (below). |

**One FETCH, in order.**
1. The runtime takes a context, begins it on the cursor's snapshot and
   overlay with the pinned parameters, and verifies the pinned plan's schema
   reference as `bc_ctx_run` does.
2. The run starts with a skip count `s` and a row limit `k`. `bc_emit_end`
   counts each emitted row; while the count is within `s` the row is dropped
   (no encoding, no memory charged); the next `k` rows are encoded to the
   result buffer; after the last the stop flag is set.
3. The rows become `DataRow`s or `Rows`, the position becomes `p + k`, and
   the context is reset.

`DECLARE` runs nothing. A stop reached at the plan's own end is the
cursor's end: its total `N` is then known, and a `FETCH` after it returns no
row without running.

**Checkpoints.** Replaying from the start is needed only because the runtime
does not know where in the plan's loops the first run stopped. For a plan
whose rows come from one outer scan in that scan's key order, it knows
enough: at the stop, the outermost loop's cursor is on an entry. Record its
key and the number `m` of rows already emitted for that outer entry. The
next run reopens the outermost scan *at that key* (the runtime moves the
start bound of the first cursor opened outside every loop; the plan text is
untouched) and sets the skip count to `m`. Rows between the previous
checkpoint and this one are not read again, filtered rows included.

Valid when every one of these holds, decided from the physical plan at
compile time and carried in the plan's metadata as one flag:
- the root is `Emit`, over `Filter`s, joins and at most one `Limit` at the
  top; the outermost loop is a `Scan`;
- the joins are `IndexJoin` and `NestedLoop` with a scan or a point lookup
  as the inner side, where the inner side is a pure function of the outer
  row;
- no `Sort`, `HashJoin`, `Aggregate`, `GroupAggregate`, set operation,
  `Choice`, or `NullSplitScan` (a second outermost scan); subqueries are
  allowed, and re-run at every FETCH;
- a top-level `Limit n` is enforced by the cursor from the plan's metadata,
  stopping at row n of the whole cursor, because the plan's own counter
  restarts at 0 on a resumed run.

Anything else is not resumable and replays from the start. The flag is a
whitelist: an operator added later is non-resumable until someone proves it
valid, so forgetting is slow, never wrong. A differential test pins this: for
every codegen test plan with the flag, FETCHing in pages of 1, 3 and all with
checkpoints gives the rows of a run to completion.

**Cost of replay.** Let `c` be the record reads and cursor steps the plan
makes per emitted row (1 for a full scan with no filter; 1/s for a filter of
selectivity s; more for a join), and `B` the reads before its first row (0
for a lazy plan; the whole input for a sort, a hash build or a group
structure). A `FETCH` of `k` rows at position `p` costs `B + c (p + k)`.
Reading all `N` rows in pages of `k` costs, summed over the `N/k` fetches:

- without a checkpoint: `(N/k) B + c (N²/(2k) + N/2)`. For a lazy plan,
  `c = 1`, `k = 100`: `N = 10⁵` costs 5×10⁷ reads, 500 times the 10⁵ a single
  run costs; `N = 10⁶` costs 5×10⁹. With a sort over a 10⁵-row input, a
  thousand fetches each redo the 10⁵ reads and the sort.
- with a checkpoint: `(N/k) S + c N`, where `S` is the setup a run repeats
  (its subqueries, its counts, its opens) and `m` is at most the fan-out of
  one outer row. Linear in `N`.

**Bounding it.** Latency per FETCH should be predictable (design philosophy,
principle 3), so the bound is per FETCH, not per position:
- the run's work budget (`bc_budget::max_reads`) is set for cursor runs from
  a per-server value, and a `FETCH` that needs more is `54000`, "cursor
  position too far to replay", failing the statement as any budget does.
  Every record read and cursor step is charged already, skipped rows
  included, so no new accounting is needed. The number is decision 7, set
  after phase 1 measures reads per second;
- the statement timeout and cancel (#684) apply to each run;
- no cap on the position itself: a cursor over a million-row table is legal
  if its shape is resumable.

**Blocking operators under replay.** Every non-resumable FETCH pays `B`
again. A bounded **result cache** removes that where the result is small
(phase 8): the encoded result rows from the front of the result are kept in
the cursor, up to a byte budget inside the statement memory (proposed 4
MiB; decision 7). A `FETCH` inside the cached prefix runs nothing. One that
passes it runs, skipping the cached rows, and appends rows while room is
left. A cursor whose whole result fits is complete after one run: it never
runs again, which is also exactly what `WITH HOLD` needs (below), so the two
share one store. It does not help a large result over a sort: the cache is
the output, not the sorter, and the sorter is rebuilt for every FETCH past
the prefix. That case is what the resumable run is for.

**`FOR UPDATE`.** A replay passes the rows already fetched again. Their
guards were merged into the transaction at their first FETCH; the merge
dedups by key so a replay does not charge them again. A row's guard is taken
when the row is emitted, not when it is skipped past; skipped rows are not
guarded twice, and a guard is never lost.

**Directions.** Replay makes every direction a position computation, with no
extra structure (*`FETCH`, `MOVE` and `SCROLL`*).

## Resumable execution (later)

When replay is too slow, a run that can pause replaces it behind the same
interface: `open` a run, `fetch(n)` resumes it for n more rows, `close`
frees it. The recommended mechanism is (c′), the plan on its own stack,
suspending in `bc_emit_end` after the batch; (c) is its fallback; (a) is not
proposed. It serves what replay cannot: a large result over a sort or hash
(the structure is built once and stays), a deep position in a non-resumable
plan, and streaming for #487 (each batch resumes rather than re-runs).

What a suspended run holds: the context (handle tables, one 64 KiB arena
chunk, pins), the sorters, hash tables and groups if the plan has them
(still charged to the statement memory), the stack, one leased buffer-pool
frame per open engine iterator, and the snapshot and overlay the cursor
already pins. Between FETCHes it holds no worker and no thread.

A spike (phase 9) settles the (c′) risks first: stack use across the
functional suite's plans, the switch cost against a thread hand-off, ASan,
TSan and MSan, a `thread_local` audit of every frame alive across a suspend
(`scripts/check_thread_local.py`), and glibc against musl. Its outcome is a
decision. Then the build (phase 10) and, for streaming, phase 11.

## Blocking operators and the memory budget

A pipeline breaker runs to completion before it produces its first row, and
under replay it runs again in every run that reaches past the cached prefix.
The structure lives for one run and is freed with it, so a replay cursor
holds no sorter between FETCHes; a resumable run (phases 9 and 10) would hold
it. There is no spill (`planner_design.md`, *Memory*): past the statement's
budget (64 MiB by default) the run fails with `54000`, and the `FETCH` with
it.

| Plan shape | Before the first row (`B`) | Replay checkpoint |
|---|---|---|
| `PointGet`, `Scan`, `Filter`, `Limit`, `IndexJoin`, `NestedLoop` | nothing: **fully lazy** | yes, if the outermost loop is a scan |
| `NullSplitScan` | nothing | no (two outermost scans) |
| `GroupAggregate` as a stream (input in group order) | the first group | no |
| `Sort` the order is not given by an index | the whole input | no |
| `HashJoin` | the build side | no |
| `GroupAggregate` as a structure, `COUNT(DISTINCT)`, `UNION`/`INTERSECT`/`EXCEPT` (distinct) | the whole input | no |
| `Aggregate` (scalar) | the whole input, in constant memory | no |
| An uncorrelated `IN` subquery's set | the subquery | allowed; re-run each FETCH |
| `UNION ALL` | nothing | no |

"Fully lazy" is a statement about work: such a cursor reads from the
snapshot only the rows it returns, plus those its filter drops on the way.
The others pay `B` per run, which the result cache removes for a small result
and only a resumable run removes for a large one. The search already avoids
a sort or a hash where an index gives the order and the budget is short
(`planner_design.md`, *The planner uses the budget*). A cost model that
prefers time to first row for a cursor, as PostgreSQL's
`cursor_tuple_fraction` does, is not proposed.

## What a cursor sees

PostgreSQL's cursors are insensitive: they see the data as it stood at
`DECLARE`, including the transaction's own earlier writes, and nothing
after. The layer's view is the transaction's snapshot plus the overlay of
its earlier statements' writes (`transaction_design.md`, *The transaction's
view*). The snapshot never moves for a transaction, so the first half is
free. The overlay moves. A replay must see the same overlay on every run, so the guarantee is exact, not approximate.

The overlay is a sorted map of point entries updated in place, and a set of
deleted ranges: there is no log with a position to cut at. A cutoff would
mean versioning every entry and the merge that reads it. Options:

- **Copy on write, shared (recommended).** The transaction holds its overlay
  by `shared_ptr`. `DECLARE` takes a reference; the next statement that adds
  effects clones the overlay first if anyone else holds it, and writes to its
  own copy. Cost: one clone of the overlay per `DECLARE` that is followed
  by a write, not per statement: after the clone the transaction's copy is
  unshared again. A fetch-and-update loop (`FETCH`, `UPDATE … WHERE CURRENT
  OF`) clones once. A transaction that has written nothing, the usual cursor
  transaction, shares an empty overlay and copies nothing.
- **Refuse `DECLARE` after writes.** PostgreSQL allows it; ETL code does it.
  Rejected.
- **Copy at every `DECLARE`.** Same bound, more copies. Rejected.

Memory: the cursor reserves the overlay's byte bound (`Overlay::bytes`, the
bound the transaction already keeps) when it declares, against the statement
budget and the server's global cap, and releases it at `CLOSE`. A `DECLARE`
that cannot is `54000` or `53200`, where it can be understood, not a later
`UPDATE` that fails because a cursor is open. The worst case is one overlay
copy per cursor declared between writes, each at most the transaction's
`max_bytes` (64 MiB).

The cursor also shares the transaction's pinned catalog version, so a DDL
committed meanwhile changes nothing it reads: it keeps the layout of the
rows it started with. The plan's schema reference is verified against that
version at each run, as `bc_ctx_run` does.

A concurrent transaction's commit is invisible to the cursor, as to the
transaction.

## `WITH HOLD`

A holdable cursor outlives its transaction, so it cannot read the
transaction's snapshot or overlay. Options:

- **Materialise (recommended), at `DECLARE`.** Run the plan to the end into
  the cursor's result store (the result cache of phase 8, with no byte
  limit but the statement budget), charged to the statement budget (`54000` past it,
  never a spill), release the snapshot, and serve every `FETCH` from the
  store. PostgreSQL does the same at `COMMIT` (it fills a tuplestore from the
  cursor). Doing it at `DECLARE` keeps a failure where the user can see it: at
  `COMMIT` a `54000` would end the transaction, and the commit would be a
  new place to fail. The cost is that `DECLARE … WITH HOLD` reads the whole
  result and is not lazy. This is the one cursor that is not lazy; it is
  bounded, and a snapshot is released at once.
- **Hold the snapshot**, with a cap on held snapshots, a time-to-live and a
  close at disconnect. The engine's snapshot is a handle on the published
  tree and the open files. While it lives, every node a write retires that it
  reaches is parked, and every unlinked data file keeps its disk blocks
  (engine design, *Read path*, `keydir_nodes_parked`; measured on the MariaDB
  plugin at 1.1 GB retained for 20 M keys under a write load that touched the
  whole key space). A hold cursor can live for days. The cap and the TTL are
  policy the layer would own, and `Snapshot` is move-only, so a hold cursor
  would need its own or a clone. Deferred, #726, for when a workload needs
  more than the budget.
  Under replay it is worse: a held snapshot would be replayed from at
  whatever position the cursor reaches, days later, over a database that
  has moved on. The recommendation stands.
- **Refuse** `WITH HOLD`: loses a feature psycopg and JDBC use for
  `commit()` in a loop. Rejected.

A hold cursor declared in a block is materialised at `DECLARE` and its
`FETCH`es work before and after `COMMIT`. A hold cursor declared in a transaction that rolls back is
dropped with it, as PostgreSQL drops it; one declared in a transaction that
commits survives, and so does one declared earlier. It counts against the
session's cursor limit and the memory cap, and it closes at `CLOSE`,
disconnect, or the server's shutdown.

## Limits, lifetime and failure

| Event | What happens to open cursors |
|---|---|
| `COMMIT`, `ROLLBACK` | Non-hold cursors are dropped first (they are data under replay; a resumable run is freed), then the snapshot is released. |
| A statement or `FETCH` fails | The transaction aborts (`25P02` to follow); its cursors close at the abort, so the snapshot is released at once (`bc_txn_add`'s abort already does). |
| Idle-in-transaction timeout (`25P03`, #642) | The transaction ends and its cursors with it. This bounds how long a lazy cursor pins a snapshot, with no new timer. |
| Disconnect, `Terminate`, shutdown | Rolled back with the transaction; hold cursors closed by the session. A suspended run, in phase 10, is freed, not resumed. |
| Statement timeout, `CancelRequest` | Apply to the `FETCH` running; the cursor is closed with the failed statement. A cancel between FETCHes is a no-op, as for any idle session. |
| DDL on a table a cursor reads | In a block: DDL is `0A000` (`server_design.md`). In autocommit, DDL commits and a lazy cursor cannot exist outside a block. A hold cursor holds rows, not a table. So no case arises, and the pinned catalog already keeps the layout stable. |
| `DROP TABLE` by another session | The cursor's snapshot still reads the table's rows; the cursor is unaffected. |

Limits (numbers are decision 7, #724):
- open cursors per session; PostgreSQL has no limit, the layer needs one
  because each cursor pins an overlay copy and a result cache;
- the replay cap on reads per run (*Bounding it*), and, in phase 10, the
  suspended runs in the server, bounding stacks as `--max-connections` bounds
  sessions;
- every held byte (overlay copies, result caches, hold stores) counts against the server's `--max-memory-bytes` (`53200`, #688),
  and the session's cursors against one per-session bound, charged when
  reserved and released at close.

## `FETCH`, `MOVE` and `SCROLL`

PostgreSQL: `FETCH [direction] [FROM|IN] cursor` with `NEXT`, `PRIOR`,
`FIRST`, `LAST`, `ABSOLUTE n`, `RELATIVE n`, `n`, `ALL`, `FORWARD [n|ALL]`,
`BACKWARD [n|ALL]`. A fetch past either end returns no row and leaves the
cursor before the first or after the last row. `MOVE` is `FETCH` with the
rows discarded and the tag `MOVE n`. `NO SCROLL` forbids backward movement;
`SCROLL` allows it; with neither, PostgreSQL allows backward movement when
the plan can run backward without storing rows, and otherwise refuses with
`55000`.

**Replay gives every direction for free.** The cursor's position `p` is the
number of rows returned (0 before the first row; `N + 1` after the last, once
`N` is known). A direction is arithmetic on it, then one run with a skip
count and a limit:

| Fetch | Rows returned | Cost beyond a forward fetch |
|---|---|---|
| `NEXT`, `FORWARD n`, `n` | `p+1 … p+n` | none |
| `FORWARD ALL` | `p+1 … N` | the rest of the result |
| `PRIOR`, `BACKWARD 1` | `p−1` | a run to `p−1`: the replay cost |
| `BACKWARD n` | `p−n … p−1`, returned in reverse | a run to `p−1`; the n rows are buffered for the reversal (charged, `54000` past the budget) |
| `BACKWARD ALL` | `1 … p−1`, reversed | buffers `p−1` rows |
| `FIRST`, `ABSOLUTE 1` | row 1 | a run of 1 row, cheap |
| `ABSOLUTE n`, `RELATIVE n` | row `n`, or `p+n` | a run to that row |
| `LAST`, `ABSOLUTE −n` | row `N`, or `N−n+1` | a run to the end, to learn `N` (once; `N` is kept) |
| count 0 | the current row again | a run to `p` |

A direction needs no buffer of the result and no `SCROLL` machinery beyond
the position and the batch being reversed. Each backward fetch costs the
replay cost `B + c·p` (a checkpoint does not help a backward fetch, which
starts again at the front, or at the result cache's prefix). That is the
price of "better than holding the result": a client that pages backward
through a large cursor is quadratic, as a forward one is without a
checkpoint.

**What the layer does with the keywords.** `NO SCROLL` refuses a backward
fetch with `55000`, as PostgreSQL. `SCROLL` allows every direction above.
With neither keyword the layer allows them too (PostgreSQL refuses some
plans), because it costs no memory: a superset of PostgreSQL, listed under
*Known differences*.

**What E121-10 gets.** Nothing in the sqltest count: all eight ids write
`FETCH … INTO <table>`, which PostgreSQL's `FETCH` does not have (decision
4). It gives the PostgreSQL behaviour the standard's `FETCH` orientations
describe: ABSOLUTE, RELATIVE, FIRST, LAST, PRIOR, NEXT, tested by cases
recorded from PostgreSQL.

**Reverse scans, when they serve.** The planner can scan any index or table
backward (`Scan::reverse`). For a resumable plan (*Checkpoints*), `FETCH
LAST`, `BACKWARD` and `ABSOLUTE −n` could cost only the rows read: compile
a twin plan with every scan reversed and start its outermost loop at the
cursor's checkpoint key. It does not apply after a sort, a hash, a group
structure or a set operation. It needs a spike for a join, whose position is
one key per loop level. Deferred, #725.

## `WHERE CURRENT OF`

The statement names the cursor's current row. Today the embedded API cannot:
`Rows` is `{columns, types, rows of Value, affected}`, with no row identity
(`api/database.cppm`), and the plan emits only the select list.

The smallest additive change: a cursor over one table with no join, group,
aggregate, `DISTINCT` or set operation is compiled with the table's primary
key as **hidden trailing result columns** (`bc_row_key` of the row in hand,
or `bc_emit_key_part`, which the plan API has; it rides a sorter's payload
as the row does). `Rows` and `RowDescription` do not show them. The cursor
keeps the last fetched row's key. `UPDATE t SET … WHERE CURRENT OF c` binds
as `WHERE pk = <the key>`: a `PointGet`, the plan the layer already has, with
the cursor's key values as parameters. No new plan operator and no change to
`bc_plan.h`. A table with no primary key has the hidden row id as its key
(`catalog_design.md`), and works the same.

Errors, as PostgreSQL's: an unknown cursor `34000`; not positioned on a row
(never fetched, or fetched past either end) `24000`; not a simple scan of
that table `24000`. The update sees the cursor's row as the transaction
sees it, writes to the overlay as any statement, and the cursor does not see
the write (it is insensitive); the phase 2 copy-on-write is what makes this
safe. Phase 6 records the cases from PostgreSQL 17. It changes the
planner's result description, so it is decision 5. The cursor keeps the key of its last returned row with its position; a replay recomputes nothing for it.

## Protocol fit

The cursor interface (`open`, `fetch`, `close`) is the portal interface the
extended protocol lacks:
- **`Execute` with a row limit** fetches that many rows of the portal and
  sends `PortalSuspended` if the portal is not finished. Today a limit above
  0 is `0A000`. The portal is a cursor with a position and no running state,
  so it holds no worker. A portal with a limit must outlive the `Execute`,
  so the implicit-transaction rule treats it as a block of several
  statements: it opens the implicit `Transaction` instead of autocommitting
  (`server_design.md`, *Extended query*), which is what PostgreSQL does.
  Under replay it is right and correct, and as slow as a cursor: a client
  that pages a large portal with `fetchSize` pays the cursor cost above. A
  portal over a resumable plan pays only the checkpoint's. This is phase 3's
  work with phase 11's mechanism behind it.
- **Streaming (#487).** A `Query` or an `Execute` with no limit should send
  rows as the run produces them and check the output bound
  (`max_unsent_bytes`, #688) between batches. A client that stops reading
  would leave a session that is "output blocked" and holds no worker, with
  memory of one batch plus the run, not the result. **Replay does not
  serve this**: every batch would redo the run from its start, and a simple
  `Query` has no checkpoint to lean on, so a million-row `SELECT` would cost
  quadratic work to stream. Streaming needs the resumable run (phase 10), so
  phase 11 depends on it and not on replay. Until then a result is built
  whole, as `server_design.md`, *Known differences* says.

Streaming has a cost cursors do not: an autocommit `SELECT` keeps its
snapshot for as long as the client takes to read, pinning what a long
transaction pins. The statement timeout bounds the read time only if it
counts the whole statement; how long a slow reader may pin a snapshot is
decision 6. Neither is built here.

## Tests

- Each phase adds cases to `tests/functional/cases/` with expected rows
  recorded from PostgreSQL 17 (`functional_testing_design.md`, *Recording
  expected results*), inside `BEGIN … COMMIT`.
- **Determinism** (phase 1, the gate for everything after): every codegen
  test plan run twice on one view gives identical result bytes, and run in
  pages of 1, 7 and all with replay gives the rows of a run to completion.
- **Checkpoints** (phase 1): for every plan the whitelist marks resumable,
  pages of 1, 3 and all with checkpoints equal a run to completion; for plans
  it does not mark, a negative test shows the flag is false (a sort, a hash
  join, a group, a set operation, a `Choice`, a `NullSplitScan`).
- The runtime: the allocation-failure sweep over a skipped-and-stopped run;
  a stop at every row of every shape leaves no handle, arena mark or memory
  charge behind; the replay cap fails a `FETCH` with `54000`.
- The frozen view: a cursor ignores an `INSERT`, `UPDATE`, `DELETE` and a
  `DELETE FROM t` after `DECLARE`, and a commit by another connection;
  accounting of the overlay copy.
- Lifecycle: cursors close at commit, rollback, abort, idle timeout,
  disconnect and shutdown, with the snapshot released (the engine's
  `keydir_versions_live` returns to 1).
- Resumable execution (phases 9 and 10): the replay cursor's own tests, run
  against the new mechanism unchanged.
- The sqltest ids: remove their marks from
  `tests/functional/not_implemented/sqltest.txt` as each phase makes them
  pass, and refresh the conformance table in `README.md` from a
  `--conformance` run.

## Phases

Each is sized for one session of 60 to 90 minutes. A phase marked
*decision* cannot start before the owner answers. The replay cursor is
phases 1 to 8; a resumable run replaces it from phase 9, behind the same
interface, so phases 3 to 7 do not change when it lands.

| Phase | Issue | What | Unlocks | Decision |
|---|---|---|---|---|
| 1 | #717 | Replay runner in the runtime: skip count, row limit, stop, start key for the outermost scan; resumable-plan flag; determinism and differential tests | none | |
| 2 | #718 | Copy-on-write overlay; pinned parameters, clocks and plan; forward-only `Cursor` in `Transaction` | none | |
| 3 | #719 | `DECLARE`, `FETCH` forward, `MOVE`, `CLOSE`, `FOR UPDATE` in the dialect and session | none alone | **1** |
| 4 | #720 | `WITH HOLD` (result store); `DECLARE` outside a block | 8 ids, or 1 | **1, 3** |
| 5 | #721 | Every `FETCH` direction, `SCROLL`, `55000` | none | |
| 6 | #722 | `WHERE CURRENT OF` | none as written | **5** |
| 7 | #724 | Limits, replay cap, flags, accounting | none | **7** |
| 8 | #729 | Result cache for small blocking cursors | none | **7** |
| 9 | #716 | Spike: the plan on its own stack | none | **2** |
| 10 | #730 | Resumable run replaces replay | none | after 9 |
| 11 | #723 | `Execute` row limit at checkpoint cost, `PortalSuspended`, streamed results | none | **6** |

Phase 4's eight ids are e121_01_01_01 to _04, e121_02_01_01, e121_03_01_01,
e121_17_01_01 and _02; they need phase 3's syntax and phase 4's hold.
Phases 3 to 8 need phase 2; 5 and 8 also need phase 1's replay; 11 needs 10.

Risks, by phase: **1** determinism is a property of today's operators, so
the test is the phase's gate; the start-key substitution must handle prefix
and reverse scans (a key inside `[lo, hi)`, not past it); **2** the
transaction's `Overlay` becomes shared, so every place that mutates it goes
through the clone, and the allocation-fault sweep covers it; **3**
`libpg_query`'s parse tree for `FOR READ ONLY` and `ONLY ( t )` is assumed,
not read, and the first thing the phase does is a parse test; **4** the hold
store's size against the budget; **6** the planner's result description
gains hidden columns, which `check()` and the schema reference must carry;
**9** the spike can fail, in which case (c) takes (c′)'s place.

## Owner decisions

1. **`DECLARE` outside a transaction block.** PostgreSQL: `25P01` unless
   `WITH HOLD`. Recommended: accept it and make the cursor `WITH HOLD`, a
   documented known difference. 8 sqltest ids instead of 1. The alternative
   keeps PostgreSQL exactly and leaves 20 ids marked. (Phases 3, 4.)
2. **When to build resumable execution.** The owner's order is replay first.
   Recommended: build phases 1 to 8, measure real cursors, and take phase 9
   when a measurement, a streaming need (#487) or a large sorted cursor asks.
   Not codegen re-entrancy. (Phase 9.)
3. **`WITH HOLD` policy.** Recommended: materialise at `DECLARE`, bounded by
   the statement budget. Alternatives: hold the snapshot with caps (#726),
   or refuse. (Phase 4.)
4. **Syntax PostgreSQL does not have.** `OPEN c` (2 ids: e121_04_01_01,
   e121_08_01_01) and `FETCH … INTO` (8 ids, e121_10) would be accepted only
   as extensions. Recommended: neither; leave the 10 ids marked. `WHERE
   CURRENT OF` on a cursor never fetched stays `24000` (3 ids).
5. **Hidden key columns in a cursor's plan** for `WHERE CURRENT OF`: a
   change to the planner's result description and the schema reference.
   (Phase 6.)
6. **How long a streaming read may pin a snapshot.** (Phase 11.)
7. **The numbers:** open cursors per session (proposed 16); the replay cap on
   reads per `FETCH` (set after phase 1 measures reads per second); the
   result cache size per cursor (proposed 4 MiB); and, for phase 10, the
   suspended runs per server and the stack reservation (proposed 256 KiB).
   (Phases 7, 8.)

## Deferred

- `FETCH LAST` and `BACKWARD` by a reverse run, without replay: #725.
- Lazy `WITH HOLD` by holding a snapshot: #726.
- Cursors under user `SAVEPOINT` / `ROLLBACK TO`: #727.
- Spilling a cursor's sort or hash to disk: part of the layer-wide spill
  design (`planner_design.md`, *Memory*), not cursor-specific.
- `pg_cursors` and `SHOW` wait on the system catalogs (#496).

## Known differences from PostgreSQL

- A cursor re-runs its plan to its position, so a `FETCH` costs work
  proportional to the position unless the plan is resumable (an outer scan
  in key order), and a `FETCH` past the replay cap is `54000`.
- A cursor without `SCROLL` and `NO SCROLL` allows backward fetches for
  every plan; PostgreSQL refuses some plans.
- `BACKWARD n` and `BACKWARD ALL` buffer the rows they return, within the
  statement budget.
- A sort or hash a cursor needs is built within the statement budget at
  every run that reaches it, never spilled.
- `WITH HOLD` reads the whole result at `DECLARE`, not at `COMMIT`.
- `DECLARE` outside a block opens a hold cursor, if decision 1 goes as
  recommended.
- A bound on cursors per session.
