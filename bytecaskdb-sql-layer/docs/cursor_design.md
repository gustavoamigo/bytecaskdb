# Cursors in the SQL layer

Status: **design, revised to the owner's decisions of 2026-10-28** (written
for #663, sqltest E121). Phases 1 (the replay runner, #717), 2 (pinned state,
#718), 3 (`DECLARE`, `FETCH` forward, `MOVE`, `CLOSE` in a transaction
block, #719), 4 (`WITH HOLD` by holding the snapshot, `DECLARE` outside a
block, #720) and 5 (every `FETCH` direction, `SCROLL` and `NO SCROLL`, #721)
are built; nothing else is. The owner's direction: a cursor is
evaluated lazily; holding the whole result in memory is not the design. The
first phases build a **replay cursor** (the snapshot and a position; each
`FETCH` re-runs the plan to its position, with no checkpoint or other
optimisation until a measurement asks, #731), and a resumable run replaces it
later where replay is too slow. The decisions still open are listed under
*Owner decisions*; the phases, with their issues, under *Phases*.

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
| e121_06_01_01, e121_07_01_01, e121_07_01_02 | `UPDATE`/`DELETE … WHERE CURRENT OF c` | The cursor was never fetched, so it is not positioned on a row: `24000` in PostgreSQL. `DELETE FROM ONLY ( t )` is in PostgreSQL's grammar (`relation_expr`), and parses to the tree of `DELETE FROM t` (phase 3's parse test); it is not the obstacle. |

(This table is from PostgreSQL's documented rules and grammar. Phase 3's
cases in `tests/functional/cases/postgresql/cursors.yaml` are recorded from a
real PostgreSQL, and the ones it makes true are listed in *As built (phase 3)*.)

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
  included, so no new accounting is needed. The number is decision 8, set
  after phase 1 measures reads per second;
- the statement timeout and cancel (#684) apply to each run;
- no cap on the position itself: a cursor over a million-row table is legal
  if its shape is resumable.

**Blocking operators under replay.** Every non-resumable FETCH pays `B`
again. A bounded **result cache** would remove that where the result is small
(phase 8, **parked** by the owner, 2026-10-28; nothing else depends on it,
and `WITH HOLD` no longer shares its store): the encoded result rows from the front of the result are kept in
the cursor, up to a byte budget inside the statement memory (proposed 4
MiB). A `FETCH` inside the cached prefix runs nothing. One that
passes it runs, skipping the cached rows, and appends rows while room is
left. A cursor whose whole result fits is complete after one run: it never
runs again. It does not help a large result over a sort: the cache is
the output, not the sorter, and the sorter is rebuilt for every FETCH past
the prefix. That case is what the resumable run is for.

**`FOR UPDATE`.** A replay passes the rows already fetched again. Their
guards were merged into the transaction at their first FETCH; the merge
dedups by key so a replay does not charge them again. A row's guard is taken
when the row is emitted, not when it is skipped past; skipped rows are not
guarded twice, and a guard is never lost.

**Directions.** Replay makes every direction a position computation, with no
extra structure (*`FETCH`, `MOVE` and `SCROLL`*).

**As built (phase 1).** `bc_ctx_window(cx, skip, limit)` sets the window
after `bc_ctx_begin*` and before `bc_ctx_run`; `bc_emit_end` of the top-level
plan drops the first `skip` rows unencoded and uncharged and keeps `limit`,
and the last kept row unwinds the plan as a failed context does, except that
it is not an error. `bc_ctx_emitted` and `bc_ctx_stopped` report where it
stopped. `Transaction::execute_window(stmt, params, RowWindow)` runs a read
over the transaction's view and returns `WindowRows`: the rows, the
`position` (rows the plan ended, skipped ones included) and `end` (the plan
ran out before the window was full; a window that ends exactly at the last
row has not seen that yet). `Statement::is_resumable()` is the checkpoint
whitelist above, computed at code generation (`GeneratedPlan::resumable`);
nothing reads it yet. The run is always from the first row: the checkpoint is
#731, and the interface leaves room for it.

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

**As built (phase 2).** `Transaction::open_cursor(stmt, params)` returns a
`Cursor` (`api/database.cppm`) over a SELECT; the transaction owns it, and the
handle is a view of it. It runs nothing at open but verifies the plan against
the transaction's pin, as a run does, pinning the snapshot and catalog if no
statement has. It keeps: the `Compiled` plan, the plan's parameter vector
(the caller's, the constants, the clock slots filled once from
`started_us`), and a `bc_overlay` pin of the transaction's overlay
(`transaction_design.md`, *The transaction's view*, *Limits*). `fetch(k)` runs
`execute_window`'s machinery with `skip = position`, `limit = k` on the pinned
plan, parameters and overlay, then `position += rows returned`; `at_end()`
once the plan ran out; `close()`. A cursor is closed by commit, rollback, an
abort (including its own failed `fetch`: the transaction aborts and so do all
its cursors, `25P02` after) and the database's close; a closed cursor's
`fetch` throws `std::logic_error`, `TransactionAborted` after an abort,
`DbClosed` after the database closed. No cursor timer, cap or setting exists:
the pin's reservation against `max_bytes` is the only limit, and a held
cursor is a held transaction. The memory reservation is the transaction's
bound (`BC_E_LIMIT` at open), not the statement budget or the server's global
cap, which a session adds in phase 7.

## `WITH HOLD`

Phase 4. A holdable cursor outlives its transaction, so it cannot read the
transaction's snapshot and overlay as they are at `COMMIT`. Decided
(2026-10-28): it is **lazy**. It holds the snapshot, never a materialised
result.

- **Hold the snapshot (decided).** The cursor keeps what `DECLARE` pinned: the
  snapshot, the overlay copy of the transaction's writes at `DECLARE`
  (phase 2's shared copy-on-write overlay, which survives the commit), the
  parameters, the clocks and the plan. A `FETCH` after `COMMIT` replays
  against that view, as any `FETCH` does, so it returns the rows a run at
  `DECLARE` would have returned, whatever has been committed since. The
  engine's snapshot is a handle on the published tree and the open files.
  While it lives, every node a write retires that it reaches is parked, and
  every unlinked data file keeps its disk blocks (engine design, *Read path*,
  `keydir_nodes_parked`; measured on the MariaDB plugin at 1.1 GB retained
  for 20 M keys under a write load that touched the whole key space). A hold
  cursor can live for days, so this is the cost, and it is real. `Snapshot`
  is move-only, so the hold cursor needs a snapshot of its own or a clone
  that outlives the transaction's; phase 4 settles which.
- **Materialising is rejected.** Reading the whole result into a store at
  `DECLARE` (or at `COMMIT`, as PostgreSQL does) is not lazy: it costs the
  whole result's memory and time up front, and a result over the statement
  budget could not be declared at all. The owner's direction is that holding
  the result in memory is not the design.
- **Refuse `WITH HOLD`: rejected.** It loses a feature psycopg and JDBC use
  for `commit()` in a loop.

Who bounds the snapshots held is not this document's to set. How many
snapshots may be pinned, for how long, and what happens at the limit are
one policy for the session design (#642), covering a long transaction, a
streaming read and a hold cursor alike. A hold cursor adds no timer and no
cap of its own; it is a pinned snapshot like any other and falls under that
policy. `DECLARE ... WITH HOLD` is phase 4 (*As built (phase 4)*).

A hold cursor declared in a block fetches before and after `COMMIT`. One
declared in a transaction that rolls back is dropped with it, as PostgreSQL
drops it; one declared in a transaction that commits survives, and so does
one declared earlier. It closes at `CLOSE`, disconnect, or the server's
shutdown. `DECLARE` outside a block is accepted in phase 4 as a hold cursor
(*Owner decisions*, 1): it declares in a transaction of its own that
commits at once, and the cursor holds the snapshot of that moment.

### As built (phase 4)

**Who owns the snapshot.** `Snapshot` is move-only, and a transaction's
commit moves its own into the `WritePlan` that validates the writes. A hold
cursor therefore has a handle of its own: `Snapshot::clone()` (engine, a
reference-count increment on the `EngineState` the snapshot already holds;
decided by the owner, 2026-10-28, over a sequence-based validation that would
need no handle and is a possible future change, not part of this work). Both
handles reach one state, so a clone retains nothing a hold cursor would not
retain with the original.

`bc_txn_hold` (plan API) takes, from a transaction with a live overlay pin, the
clone, the pinned catalog and the pin's overlay (`shared_ptr`) into a `bc_hold`
that the cursor owns. A hold cursor runs on it, in a context of the database's
pool begun with `bc_ctx_begin_hold`, whether its transaction is open or not: one
code path, `Database::run_held`, reads nothing of the transaction. It takes no
`FOR UPDATE` guard: the session refuses `WITH HOLD ... FOR UPDATE` (`0A000`, as
PostgreSQL).

**Lifetime.** `Transaction::open_cursor(stmt, params, Holdability::Hold)`.
`commit()` that commits detaches the hold cursors (releases the overlay pin's
charge to the transaction; the view stays in the hold) and closes the others.
A rollback, an abort, a commit that does not commit (a conflict, an engine
error) and a destroyed transaction close every cursor declared in it, hold
included. A hold cursor declared in an earlier transaction is untouched. A hold
cursor ends at `close()` / `CLOSE`, at `Database::close()` (the database keeps
a registry of its open hold cursors for this) and at the session's end
(disconnect, `Terminate`, shutdown). A dropped `Cursor` handle leaves it open.
A failed `FETCH` of a hold cursor closes that cursor; the session fails the
block as for any error.

**What it pins, and for how long.** The engine's snapshot: every tree node a
later write retires that it reaches is parked, and every data file vacuum
unlinks keeps its disk blocks (`keydir_versions_live`, `keydir_nodes_parked`;
1.1 GB retained for 20 M keys under a write load that touched the whole key
space, measured on the MariaDB plugin). **This phase adds no timer, cap, TTL or
setting.** A hold cursor lives until `CLOSE`, disconnect or server shutdown;
the limit on pinned snapshots is the session design's (#642). The overlay copy
is charged to the declaring transaction's bound while that transaction lasts
(`BC_E_LIMIT`, `54000`, at `DECLARE`); after its commit the copy is held
uncharged, until the session design's accounting (phase 7) charges what a
session holds.

**`DECLARE` outside a block** (owner decision 1) opens a transaction of its
own, declares a hold cursor in it and commits; `WITH HOLD` or not, and
`FOR UPDATE` too, which takes no guard there: there is no transaction left to
hold it. In an extended-protocol implicit block it is declared in that block
and kept by its commit.

**Session.** `SessionCursor::hold`. `end_transaction` drops the block's plain
cursors and keeps the hold ones that are still open; a name whose cursor a
rollback closed is forgotten when next named.

## Limits, lifetime and failure

| Event | What happens to open cursors |
|---|---|
| `COMMIT`, `ROLLBACK` | Non-hold cursors are dropped first (they are data under replay; a resumable run is freed), then the snapshot is released. |
| A statement or `FETCH` fails | The transaction aborts (`25P02` to follow); its cursors close at the abort, so the snapshot is released at once (`bc_txn_add`'s abort already does). |
| Idle-in-transaction timeout (`25P03`) | Not part of this stack: it is in the separate session stack (#675, #644, #684, #688), and cursors neither build nor depend on it. When it ends a transaction, that is an end like any other and its cursors close with it. |
| Disconnect, `Terminate`, shutdown | Rolled back with the transaction; hold cursors closed by the session. A suspended run, in phase 10, is freed, not resumed. |
| Statement timeout, `CancelRequest` | Apply to the `FETCH` running; the cursor is closed with the failed statement. A cancel between FETCHes is a no-op, as for any idle session. |
| DDL on a table a cursor reads | In a block: DDL is `0A000` (`server_design.md`). In autocommit, DDL commits and a lazy cursor cannot exist outside a block. A hold cursor holds its snapshot and the catalog pinned at `DECLARE`, so a later DDL changes nothing for it. So no case arises, and the pinned catalog already keeps the layout stable. |
| `DROP TABLE` by another session | The cursor's snapshot still reads the table's rows; the cursor is unaffected. |

Limits (numbers are decision 8, #724):
- **Cursors add no timers or caps of their own.** How many snapshots may be
  pinned, and for how long, is one policy for the session design (#642), the
  same for a long transaction, a cursor and a streaming read (decision 7). A
  cursor is held to it, not given a limit beside it;
- the replay cap on reads per run (*Bounding it*), a work budget like any
  statement's, and, in phase 10, the suspended runs in the server, bounding
  stacks as `--max-connections` bounds sessions;
- the memory a cursor holds (its overlay copy) is charged to the owning
  transaction's bound when the cursor is opened and released when it closes
  (phase 2), and counts against the server's `--max-memory-bytes` (`53200`,
  #688) as everything else the session holds does.

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
planner's result description, so it is decision 6. The cursor keeps the key of its last returned row with its position; a replay recomputes nothing for it.

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
decision 7. Neither is built here.

## As built (phase 3)

**The parse test.** What `libpg_query` (PostgreSQL 17's parser) gives, read
from the tree and not assumed:

| Text | Tree |
|---|---|
| `DECLARE c … CURSOR [WITH \| WITHOUT HOLD] FOR select` | `DeclareCursorStmt{portalname, options, query: SelectStmt}`. `options` is PostgreSQL's `CURSOR_OPT_*` bits: `BINARY` 0x1, `SCROLL` 0x2, `NO_SCROLL` 0x4, `INSENSITIVE` 0x8, `ASENSITIVE` 0x10, `HOLD` 0x20; 0x100 (fast plan) is always set. The tree has no location for the query, so the text of it is found from the scanner's tokens: after `CURSOR`, an optional `WITH`/`WITHOUT HOLD`, then `FOR`. |
| `… FOR UPDATE [OF t]` | The `SelectStmt` has a `lockingClause` (`LCS_FORUPDATE`, `lockedRels` for `OF`). |
| `… FOR READ ONLY` | **Nothing**: the clause is dropped by the grammar and the `SelectStmt` is the one without it. The layer cannot see it and does not need to. |
| `FETCH [NEXT] [FROM\|IN] c`, `FETCH 4 c`, `FETCH FORWARD [n] c` | `FetchStmt{direction: FORWARD, howMany, portalname, ismove}`. `NEXT`, a bare `FORWARD` and no direction are all `howMany` 1. |
| `FETCH ALL c`, `FETCH FORWARD ALL c` | FORWARD with `howMany` = `INT64_MAX`. |
| `FETCH PRIOR c`, `BACKWARD n`, `BACKWARD ALL` | BACKWARD, 1 for `PRIOR`, `INT64_MAX` for `ALL`. |
| `FETCH FIRST c`, `LAST c`, `ABSOLUTE n c`, `RELATIVE n c` | ABSOLUTE/RELATIVE; `FIRST` is ABSOLUTE 1 and `LAST` ABSOLUTE −1. |
| `FETCH -2 c` | FORWARD with `howMany` −2: PostgreSQL itself turns that into a backward fetch. |
| `FETCH 0 c` | FORWARD, `howMany` 0: the current row again. |
| `MOVE …` | The same `FetchStmt` with `ismove`. |
| `CLOSE c`, `CLOSE ALL` | `ClosePortalStmt{portalname}`; empty for `ALL`. |
| `DELETE FROM ONLY ( t )` | The tree of `DELETE FROM ONLY t`: `inh` false on the relation, which the layer already ignores. |

**Statement kind.** A cursor statement names the session's cursor and carries
no plan, so it is not an `ast::Statement`: the dialect has `ast::parse_cursor`,
which returns a `DeclareCursor`, `FetchCursor` or `CloseCursor` or nothing,
and the session runs it, as it runs `BEGIN`. A `DECLARE`'s query is returned
as text with everything before it blanked (same length, so an error's
position into the query is a position into the statement) and is prepared as
the `SELECT` it is, with its parameters. No binder, planner or `Statement`
change was needed.

**The session** keeps `cursors_`, a map of the block's `Cursor`s
(`Transaction::open_cursor`). A cursor shares the portals' namespace: a
`DECLARE` or a `Bind` of a used name is `42P03`. They close at `COMMIT`,
`ROLLBACK` and an abort (the transaction closes them), and at disconnect. The
rules, in PostgreSQL's order:

| Statement | Result |
|---|---|
| `DECLARE` in a failed block | `25P02` |
| `DECLARE … WITH HOLD … FOR UPDATE` | `0A000`, as PostgreSQL |
| `DECLARE` outside a block | a hold cursor (phase 4; PostgreSQL says `25P01`) |
| `DECLARE BINARY` | `0A000` |
| `DECLARE SCROLL … FOR UPDATE` | `0A000`, as PostgreSQL |
| a name in use | `42P03`, the transaction aborts |
| `FETCH`/`MOVE` of an unknown name, `CLOSE` of one | `34000` |
| `FETCH`/`MOVE` forward, a count or `ALL` | the next rows; `FETCH n` and `MOVE n` count the rows |
| `FETCH 0`, `ABSOLUTE`, `RELATIVE`, `FIRST`, `LAST`, `BACKWARD`, a negative count | phase 5 (*As built (phase 5)*) |
| `CLOSE name`, `CLOSE ALL` | `CLOSE CURSOR`, `CLOSE CURSOR ALL` |

A `FETCH` returns its `RowDescription` even with no row, as PostgreSQL does.
In the extended protocol a `Describe` of a `FETCH` gives the cursor's columns
if the cursor exists then, and `NoData` otherwise; a `DECLARE` may carry
parameters through `Bind`. `Execute` of a `FETCH` sends the rows and the tag
and, as for any portal, a row limit is still `0A000`.

**`FOR UPDATE [OF t]`** is accepted in the dialect for any `SELECT`:
`OF t` must name a `FROM` item (`42P01` otherwise) and is `FOR UPDATE`. A
cursor's guards are taken as its rows are fetched (*The replay cursor*,
`FOR UPDATE`). **`WHERE CURRENT OF`** is phase 6: today the dialect's `WHERE` rejects
it (`42601`, "WHERE must compare a column"), so a `FOR UPDATE` cursor can be declared and fetched but
no positioned `UPDATE` or `DELETE` can name it.

**Embedded API.** The embedded API has no SQL `DECLARE`: `Transaction::execute`
takes the dialect's `Statement`s, and a cursor statement is the session's. Its
surface is `Transaction::open_cursor` and `Cursor::fetch`, which the session
calls. The statement kinds are the server's.

**Conformance.** No sqltest id moves: the harness runs outside a block, so
`DECLARE` is `25P01` there (*What the sqltest ids need*).

## As built (phase 5)

**Position.** `Cursor::position()` is 0 before the first row, the row's number
on a row, and `rows + 1` after the last. The size `rows` is known once a run
has reached the plan's end; `Cursor::at_end()` is "after the last row". A
`FETCH` moves the cursor as PostgreSQL's does: it stops on the last row it
returned (so `BACKWARD 2` from row 3 returns 2 and 1 and stands on 1), and
asking for more rows than remain returns the rest and leaves it before the
first row or after the last.

**One fetch.** `CursorMove` is the direction and PostgreSQL's `howMany`. The
cursor turns it into a window of rows `first … first + count − 1` and the new
position, then makes one `execute_window` run with `skip = first − 1`, `limit
= count` (none when the window is empty). A `BACKWARD` window is returned
last row first by reversing the decoded rows in place. Once the size is
known, a window past it is trimmed and no run is made.

| Move, from position `p` | Window | New position |
|---|---|---|
| `FORWARD n`, `n` (`ALL`: no limit) | `p+1 … p+n` | `p+n`, or `rows+1` if fewer remained |
| `BACKWARD n` (`ALL`) | `p−1` down to `p−n` | `p−n`, or `0` |
| `FETCH 0`, `RELATIVE 0` | `p`, if it is a row | `p` |
| `ABSOLUTE n` | `n` | `n`, or `rows+1` past the end |
| `ABSOLUTE −n` | `rows−n+1` | that row, or `0` if none |
| `RELATIVE n` | `p+n` | as `ABSOLUTE` |
| `RELATIVE −n` | `p−n` | that row, or `0` |
| `ABSOLUTE 0` | none | `0` |

A negative `FORWARD` count is `BACKWARD` and the reverse, as PostgreSQL does.
`FIRST` is `ABSOLUTE 1`, `LAST` is `ABSOLUTE −1`, `PRIOR` is `BACKWARD 1`.
`MOVE` is the same run with the rows dropped and the tag `MOVE n`.

**Cost.** Every move is one replay from the first row (no checkpoint, #731): a
move whose window ends at row `r` reads `B + c·r` (*Cost of replay*), whatever
the direction. `BACKWARD n` buffers `min(n, p−1)` rows: the run charges them to
the statement's memory budget like any result, so a window that does not fit
is `54000` and the transaction aborts; `BACKWARD ALL` buffers `p−1` rows.
`LAST` and `ABSOLUTE −n` first learn the size, with a run that skips every row
and keeps none (unencoded and uncharged, a full run's reads and no memory),
then run the one row: two replays the first time, one after the size is known
(`FETCH ALL` and any move that reaches the end also learn it). A reverse run
for them is #725.

**`SCROLL`.** The layer follows the design's choice: `SCROLL` and no keyword
allow every direction; `NO SCROLL` refuses with `55000` ("cursor can only
scan forward") what PostgreSQL's `NO SCROLL` refuses, checked before anything
runs and leaving the cursor and the transaction usable through the embedded
API (`CursorForwardOnly`), failing the block in the session as any error does:

| On `NO SCROLL` | Refused (`55000`) | Allowed |
|---|---|---|
| `BACKWARD` or `PRIOR`, any count, `BACKWARD 0`, a negative `FORWARD` count | always | |
| `FETCH 0`, `RELATIVE 0`, `FORWARD 0` | when the cursor is on a row | before the first row and after the last |
| `RELATIVE −n` | always | `RELATIVE n`, `n > 0` |
| `ABSOLUTE n`, `n > 0` | `n` ≤ the current row (`rows` when after the end) | a row further on, or past the end |
| `ABSOLUTE 0` | on or after a row | before the first row |
| `ABSOLUTE −n`, `LAST` | always | |

These were checked against a PostgreSQL 16 server (the one at hand; PostgreSQL
17 is the recording target, and the rules are the same in both).

**Describe.** `Cursor::describe()` gives a cursor's columns without running or
moving, which the extended protocol's `Describe` of any `FETCH` form uses;
`fetch(0)` on the embedded API is the same (no rows, no movement).

**Embedded API.** `Cursor::fetch(CursorMove)`; `Transaction::open_cursor(…,
Scrolling)`, `Scrolling::ForwardOnly` for `NO SCROLL`. The earlier
`position()` after a drain was `rows`; it is now `rows + 1`.

**Tests.** `tests/api_cursor_test.cpp`: a model of PostgreSQL's `FETCH` that
steps one row at a time (so it shares no arithmetic with the code) against
every direction and count, from every position of results of 0, 1, 2, 3 and 6
rows (empty cursor, one row, first, last, before the start, after the end),
for plain, hold, `SCROLL` and `NO SCROLL` cursors and `MOVE`'s rows; random
sequences of directions over five plans (a scan, a sort, a group, a join)
compared with slicing the full result; a table of `NO SCROLL` refusals; later
writes ignored in every direction; hold cursors scrolling after the commit;
the allocation-fault sweep over a `BACKWARD` fetch; the statement budget.
`tests/functional/cases/postgresql/cursors.yaml`: 27 cases of directions,
`MOVE`, empty and one-row cursors, `NO SCROLL` and hold, recorded from
PostgreSQL 16. `tests/server/test_wire.py`: tags, row framing, `Describe` and
`55000` over the wire.

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
| 1 | #717 | **Done.** Replay runner in the runtime: skip count, row limit, stop; resumable-plan flag; determinism and differential tests. The start-key checkpoint is not built (owner direction, 2026-10-28): #731 | none | |
| 2 | #718 | **Done.** Copy-on-write overlay; pinned parameters, clocks and plan; forward-only `Cursor` in `Transaction` | none | |
| 3 | #719 | **Done.** `DECLARE`, `FETCH` forward, `MOVE`, `CLOSE`, `FOR READ ONLY`, `FOR UPDATE [OF t]` in the dialect and session, inside a block | none alone | decided |
| 4 | #720 | **Done.** `WITH HOLD` by holding the snapshot (lazy, on `Snapshot::clone`); `DECLARE` outside a block, accepted as a hold cursor | 7 of 8 ids; e121_03_01_01 needs `ORDER BY` of an expression | decided (1, 3) |
| 5 | #721 | **Done.** Every `FETCH` direction, `SCROLL` and `NO SCROLL`, `55000` | none (all eight E121-10 ids write `FETCH … INTO`, which stays refused) | |
| 6 | #722 | `WHERE CURRENT OF` | none as written | **5** |
| 7 | #724 | Replay cap, flags, accounting; no cursor-own timers or caps | none | **7** |
| 8 | #729 | **Parked** (owner, 2026-10-28). Result cache for small blocking cursors | none | |
| 9 | #716 | Spike: the plan on its own stack | none | **2** |
| 10 | #730 | Resumable run replaces replay | none | after 9 |
| 11 | #723 | `Execute` row limit at checkpoint cost, `PortalSuspended`, streamed results | none | **6** |

Phase 4's eight ids are e121_01_01_01 to _04, e121_02_01_01, e121_03_01_01,
e121_17_01_01 and _02; they need phase 3's syntax and phase 4's hold and
`DECLARE` outside a block. Seven pass. e121_03_01_01 declares
`ORDER BY A + 5`, which the dialect refuses with or without a cursor (a gap
in `ORDER BY`, not in cursors), so it stays marked. Phase 3 moves no sqltest id: the harness runs
outside a block. Phases 3 to 7 need phase 2; 5 also needs phase 1's replay;
11 needs 10.

Risks, by phase: **1** determinism is a property of today's operators, so
the test is the phase's gate; the start-key substitution must handle prefix
and reverse scans (a key inside `[lo, hi)`, not past it); **2** the
transaction's `Overlay` becomes shared, so every place that mutates it goes
through the clone, and the allocation-fault sweep covers it; **3**
`libpg_query`'s parse tree for the cursor statements was assumed; phase 3
read it (*As built (phase 3)*); **4** the snapshot a hold cursor retains,
against the session's policy for pinned snapshots (read, in *As built (phase 4)*); **6** the planner's result description
gains hidden columns, which `check()` and the schema reference must carry;
**9** the spike can fail, in which case (c) takes (c′)'s place.

## Owner decisions

Decided on 2026-10-28 (recorded on PR #728):

1. **`DECLARE` outside a transaction block** will be accepted, as a `WITH
   HOLD` cursor, a documented known difference; that is phase 4. Until then
   it stays PostgreSQL's `25P01`. 8 sqltest ids instead of 1.
2. **Replay first, no optimisation.** No checkpoint and no key-directory
   replay until a measurement asks (#731); resumable execution (phase 9) is
   taken when a measurement, a streaming need (#487) or a large sorted cursor
   asks, and not as codegen re-entrancy.
3. **`WITH HOLD` is lazy.** It holds the snapshot and is never materialised
   (*`WITH HOLD`*). Phase 4. The bound on pinned snapshots is the session
   design's one policy (#642), not a cursor limit.
4. **No syntax PostgreSQL does not have.** `OPEN c` (2 ids: e121_04_01_01,
   e121_08_01_01) and `FETCH … INTO` (8 ids, e121_10) stay syntax errors; the
   10 ids stay marked. `WHERE CURRENT OF` on a cursor never fetched stays
   `24000` (3 ids).
5. **Phase 8, the result cache, is parked.**

Still open:

6. **Hidden key columns in a cursor's plan** for `WHERE CURRENT OF`: a
   change to the planner's result description and the schema reference.
   (Phase 6.)
7. **How long a streaming read may pin a snapshot.** (Phase 11.) It belongs
   to the same pinned-snapshot policy as decision 3.
8. **The numbers:** the replay cap on reads per `FETCH` (set after phase 1
   measures reads per second); and, for phase 10, the suspended runs per
   server and the stack reservation (proposed 256 KiB). The open-cursors
   bound and the result cache size of earlier drafts are gone: cursors take
   no cap of their own, and the cache is parked. (Phase 7.)

## Deferred

- `FETCH LAST` and `BACKWARD` by a reverse run, without replay: #725.
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
- `FETCH LAST`, a negative `ABSOLUTE` and the first move that needs the size
  read the whole result once more; a reverse run would not (#725).
- A sort or hash a cursor needs is built within the statement budget at
  every run that reaches it, never spilled.
- A `WITH HOLD` cursor holds its snapshot, where PostgreSQL copies the rows
  at `COMMIT`: it pins tree nodes and files until closed (no timer or cap).
- `DECLARE` outside a block opens a hold cursor, where PostgreSQL says
  `25P01` (decision 1), and takes no `FOR UPDATE` guard.
- `FOR UPDATE OF t` locks the one table the query reads, which is all
  `FOR UPDATE` of a single-table `SELECT` can name.
- `SCROLL` with `FOR UPDATE` is `0A000`, as PostgreSQL's.
