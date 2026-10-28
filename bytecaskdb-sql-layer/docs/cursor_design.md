# Cursors in the SQL layer

Status: **design, not approved** (written 2026-10-28 for #663, sqltest
E121). Nothing is built. The owner's direction: a cursor is evaluated
lazily; holding the whole result in memory is not the design. The decisions
that need the owner are listed under *Owner decisions*; the phases, with
their issues, under *Phases*.

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
use server-side cursors (psycopg, JDBC fetch size), which phases 2 to 6
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

## Ways to pause a plan

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
all of `Gen` (2,400 lines) and `SetGen`. It serves every shape, holds only
the saved locals, and fits the pool. Cost: the largest change on the list,
in the part of the system with the most tests, and every later operator
must be written in two forms. Not additive for codegen. The plan API gains
the slot calls (additive).

**(b) Restart from a saved key (keyset resume).** Keep the snapshot and the
last scan key of each loop; a FETCH re-runs the plan with each loop's lower
bound moved past its key. This is the owner's picture exactly: a cursor is a
snapshot and a position, memory is a few keys, and no stack is held. It is
valid only where output order is a loop nest's key order and the rows come
straight from the scans: a single scan with filters and limit, index joins
and nested loops of scans (one saved key per loop level, because a FETCH can
stop mid-way through an outer row's inner loop). It is not valid after a
`Sort`, a hash join's build, a group structure, a set operation or a
subquery set: their rows come from a structure, so there is no key to resume
from, and re-running one per FETCH is quadratic. Codegen change: every
scan's bound printing takes an optional start key and the `LIMIT` counter an
initial value (additive plan text, plan-API unchanged, but the planner must
prove a plan is resumable and every future operator must be classified).
The prologue (computed bounds, subqueries, choice-point counts) re-runs per
FETCH; deterministic on a frozen snapshot, but a subquery re-reads. Two
mechanisms (resume for some shapes, something else for the rest) and a rule
that correctness depends on every new operator being classified: against the
design philosophy's simplicity and "no discipline-only invariants".

**(c) The plan on a parked thread.** The cursor owns a thread that runs the
unchanged plan; `bc_emit_end` blocks on a condition variable after the
batch, and FETCH wakes it. No codegen or plan-API change, every shape, blocking
operators materialise and then stream, as PostgreSQL's do. It holds a
thread per started cursor, which is the thread-per-connection cost #642
removed, now per cursor; it needs a global thread bound; shutdown must join
them; every FETCH is two hand-offs. It works under every sanitizer.

**(c′) The plan on its own stack (a stackful coroutine).** The same as (c)
without the thread: the plan runs on a stack of its own (`makecontext` and
`swapcontext`, or the equivalent) allocated at the cursor's first FETCH;
`bc_emit_end` swaps back to the caller when the batch is full; the next
FETCH swaps in, on whichever worker thread asked. No codegen or plan-visible
API change. The runtime gains three calls the server makes and a plan never
does. A cursor between FETCHes holds a stack and its context, no thread and
no worker. A batch of n rows is one switch, not n.

**(d) Anything else.** Run to completion and slice the buffer: the option 1
of #663, rejected by the owner. Re-run the statement with `OFFSET k` per
FETCH: quadratic. A pull-based executor beside the compiled plans: a second
engine to keep equal to the first. None is worth more than a line.

| | Codegen change | Shapes | Holds between FETCHes | Worker pool |
|---|---|---|---|---|
| (a) state machine | all of `Gen`, `SetGen` | all | saved locals, context | fits |
| (b) keyset resume | scan bounds, `LIMIT` | pipelines of scans only | snapshot, a key per loop | fits |
| (c) parked thread | none | all | a thread, context | a thread per cursor beside the pool |
| (c′) coroutine | none | all | a stack, context | fits |

**Recommendation: (c′), the plan on its own stack.** It is the only option
that serves every plan shape with no change to codegen or the plan API, so
the 2,400 lines of codegen and every operator written later stay as they are,
and what a cursor means for a shape is the same code path for all of them. It
keeps the property the owner asked for: execution is lazy. A scan produces a
row per resume; the engine's iterator keeps its place; values are read from
disk when the plan steps. What it costs, honestly:
- A stack per started cursor: sized by measurement (phase 0, I expect tens of
  KiB used out of a 256 KiB reservation with a guard page; the pages
  untouched are never resident), charged to the statement memory as a fixed
  amount.
- Hazards that threads do not have: a frame that lives across a suspend must
  hold no `thread_local` address, since the next resume may run on another
  thread; ASan and TSan need fiber annotations and MSan has no equivalent, so
  the MSan job may have to run the cursor tests on the fallback; `ucontext` is
  glibc and macOS, not musl.
- The engine iterator of a suspended scan may keep a buffer-pool frame
  leased (engine design, *L — Frame lease*): one 4 KiB frame per open scan.

Phase 0 is a spike to settle these before anything is built, with a
fallback that costs one file: (c), behind the same `open`/`resume`/`close`.
If the spike fails, (c) is the answer, not (a). (b) is not discarded: it is
the way to serve `FETCH LAST` and `BACKWARD` without buffering (below), for
the shapes where it is valid.

## The runner

Three runtime calls, in the section of `bc_plan.h` that the server calls
and a plan never does:
- `open(cx, plan, schema_ref)`: verifies the schema reference as
  `bc_ctx_run` does, allocates the stack, does not start the plan;
- `resume(cx, max_rows)`: runs the plan until it has emitted `max_rows` more
  rows, or it ends, or it fails; the work budget and deadline are armed anew
  for this call, so the statement timeout and a cancel apply per FETCH;
- `close(cx)`: frees the stack without resuming it. The frames on a
  suspended stack are the plan's (C, no destructors) and the entry
  trampoline's, which holds nothing that needs one, so there is nothing to
  unwind. A failed run is unwound the usual way, through its sticky error.

`bc_emit_end` suspends after the row that fills the batch; nothing else
suspends. The rows go to the result buffer as now; `fetch` converts and
releases the buffer, and the memory it held, before the next resume.

**What a FETCH does, on the session:**
1. A worker runs the session's turn; the message is `FETCH c FORWARD 100`.
2. The cursor's context resumes on that worker's thread, for up to 100 rows,
   reading the transaction's snapshot and the overlay frozen at DECLARE.
3. The rows become `DataRow`s; the plan is suspended; the turn ends and the
   worker is free.

**What a suspended cursor holds:**

| Holds | Bounded by |
|---|---|
| The transaction's snapshot (shared with the transaction; no extra) | the transaction |
| The frozen overlay | its byte count, reserved at DECLARE |
| The context: handle tables, one arena chunk (64 KiB), pins | the statement memory |
| Sorter, hash table and group contents, if the plan has them | the statement memory, still charged |
| The coroutine stack | a fixed reservation per started cursor |
| One leased buffer-pool frame per open engine iterator | one frame each |

A `DECLARE` runs nothing: it binds, takes the overlay and reserves memory.
The plan starts at the first `FETCH` or `MOVE`. As PostgreSQL, a runtime
error (`22003` in the 7,000th row, `54000`, a timeout) arrives at the
`FETCH` that reaches it, aborts the transaction and closes the cursor.

## Blocking operators and the memory budget

A pipeline breaker runs to completion before it produces its first row. The
cursor's first `FETCH` pays for it, and the structure stays charged to the
statement memory for the cursor's whole life, since rows stream out of it.
There is no spill (`planner_design.md`, *Memory*): past the statement's
budget (64 MiB by default) the first `FETCH` fails with `54000`, and the
cursor closes.

| Plan shape | Before the first row | Held while open |
|---|---|---|
| `PointGet`, `Scan`, `NullSplitScan`, `Filter`, `Limit`, `IndexJoin`, `NestedLoop` | nothing: **fully lazy** | one step per loop level |
| `GroupAggregate` as a stream (input in group order) | the first group | the group in hand |
| `Sort` the order is not given by an index | the whole input | sorter: rows and keys (`ORDER BY … LIMIT n` keeps n) |
| `HashJoin` | the build side | the hash table; the probe side is lazy |
| `GroupAggregate` as a structure, `COUNT(DISTINCT)`, `UNION`/`INTERSECT`/`EXCEPT` (distinct) | the whole input | groups or the distinct keys |
| `Aggregate` (scalar) | the whole input, in constant memory | a few locals |
| An uncorrelated `IN` subquery's set | the subquery | the set |
| `UNION ALL` | nothing | one arm at a time |

Which planner choice yields a lazy plan matters now: the search already
avoids a sort or a hash where an index gives the order and the budget is
short (`planner_design.md`, *The planner uses the budget*); for a cursor the
statement's budget is the whole cursor's, so no change is needed. A cost
model that prefers time-to-first-row for a cursor, as PostgreSQL's
`cursor_tuple_fraction` does, is not proposed; it is a planner change for
when a measurement asks for it.

## What a cursor sees

PostgreSQL's cursors are insensitive: they see the data as it stood at
`DECLARE`, including the transaction's own earlier writes, and nothing
after. The layer's view is the transaction's snapshot plus the overlay of
its earlier statements' writes (`transaction_design.md`, *The transaction's
view*). The snapshot never moves for a transaction, so the first half is
free. The overlay moves.

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
rows it started with. The plan's schema reference is verified once, at
`open`, not at each resume.

A concurrent transaction's commit is invisible to the cursor, as to the
transaction.

## `WITH HOLD`

A holdable cursor outlives its transaction, so it cannot read the
transaction's snapshot or overlay. Options:

- **Materialise (recommended), at `DECLARE`.** Run the plan to the end into
  an encoded row store, charged to the statement budget (`54000` past it,
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
| `COMMIT`, `ROLLBACK` | Non-hold cursors close first, their runs freed, then the snapshot is released. |
| A statement or `FETCH` fails | The transaction aborts (`25P02` to follow); its cursors close at the abort, so the snapshot is released at once (`bc_txn_add`'s abort already does). |
| Idle-in-transaction timeout (`25P03`, #642) | The transaction ends and its cursors with it. This bounds how long a lazy cursor pins a snapshot, with no new timer. |
| Disconnect, `Terminate`, shutdown | Rolled back with the transaction; hold cursors closed by the session. Suspended stacks are freed, not resumed. |
| Statement timeout, `CancelRequest` | Apply to the `FETCH` running; the cursor is closed with the failed statement. A cancel between FETCHes is a no-op, as for any idle session. |
| DDL on a table a cursor reads | In a block: DDL is `0A000` (`server_design.md`). In autocommit, DDL commits and a lazy cursor cannot exist outside a block. A hold cursor holds rows, not a table. So no case arises, and the pinned catalog already keeps the layout stable. |
| `DROP TABLE` by another session | The cursor's snapshot still reads the table's rows; the cursor is unaffected. |

Limits (numbers are decision 7, #724):
- open cursors per session; PostgreSQL has no limit, the layer needs one
  because a started cursor holds a stack and a context;
- suspended runs in the server, bounding stacks as `--max-connections` bounds
  sessions;
- every held byte (overlay copies, context memory, replay buffers, hold
  stores) counts against the server's `--max-memory-bytes` (`53200`, #688),
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

The layer, cheapest correct equivalent:

| Declared | Backward movement |
|---|---|
| `NO SCROLL`, or neither | `55000` "cursor can only scan forward". A known difference for the "neither" case, where PostgreSQL would allow it for a simple scan. |
| `SCROLL` | A replay buffer: the encoded rows already fetched, kept in memory and charged to the statement budget. `PRIOR`, `FIRST`, `ABSOLUTE n` and `RELATIVE` backward are served from it; a forward fetch beyond it resumes the run; `LAST` and `ABSOLUTE -n` run to the end first. `54000` past the budget. |

The buffer grows with how far the cursor has moved, not with the result at
`DECLARE`, so a scrollable cursor that is read near the front stays small. It
is the honest limit that a `LAST` over a table larger than the budget fails
with `54000` when PostgreSQL succeeds.

**Reverse scans, when they serve.** The planner can scan any index or table
backward (`Scan::reverse`), and for a plan that is one pipeline of scans whose
order is the access path's (the fully lazy rows of the table above), `FETCH
LAST`, `BACKWARD` and `ABSOLUTE -n` could cost only the rows read: compile a
twin plan with every scan reversed and start its loops past the current row's
keys, which is (b) above. It does not apply after a sort, a hash, a group
structure or a set operation, where the buffer is the only answer. It needs a
spike for a join, whose position is one key per loop level. Deferred, #725.

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
the write (it is insensitive); the phase-1b copy-on-write is what makes this
safe. Phase 5 records the cases from PostgreSQL 17. It changes the
planner's result description, so it is decision 5.

## Protocol fit

The same runner gives the extended protocol what it lacks:
- **`Execute` with a row limit** resumes the portal's run for that many rows
  and sends `PortalSuspended` if the run is not finished. The portal keeps
  its run, as a cursor does, and no worker. Today a limit above 0 is
  `0A000`. A portal with a limit must outlive the `Execute`, so the
  implicit-transaction rule treats it as a block of several statements: it
  opens the implicit `Transaction` instead of autocommitting
  (`server_design.md`, *Extended query*), which is what PostgreSQL does.
- **Streaming (#487).** A `Query` or an `Execute` with no limit runs the same
  run in batches, writes the `DataRow`s of each batch, and checks the
  output bound (`max_unsent_bytes`, #688) between batches. A client that
  stops reading leaves a suspended run and a session that is "output
  blocked" and holds no worker; memory is the batch plus the run, not the
  result. The result is no longer built whole before the first `DataRow`
  (`server_design.md`, *Known differences*).

Streaming has a cost cursors do not: an autocommit `SELECT` keeps its
snapshot for as long as the client takes to read. The memory the snapshot
pins is the same as a long transaction's. The statement timeout bounds the
read time only if it counts the whole statement; how long a slow reader may
pin a snapshot is decision 6. Neither is built here.

## Tests

- Each phase adds cases to `tests/functional/cases/` with expected rows
  recorded from PostgreSQL 17 (`functional_testing_design.md`, *Recording
  expected results*), inside `BEGIN … COMMIT`.
- The runner: every codegen test plan run in batches of 1, 7 and all gives a
  run to completion's rows; a close at every suspension point; the
  allocation-failure sweep over `open` and `resume`; TSan with resumes on
  different threads.
- The frozen view: a cursor ignores an `INSERT`, `UPDATE`, `DELETE` and a
  `DELETE FROM t` after `DECLARE`, and a commit by another connection;
  accounting of the overlay copy.
- Lifecycle: cursors close at commit, rollback, abort, idle timeout,
  disconnect and shutdown, with the snapshot released (the engine's
  `keydir_versions_live` returns to 1).
- The sqltest ids: remove their marks from
  `tests/functional/not_implemented/sqltest.txt` as each phase makes them
  pass, and refresh the conformance table in `README.md` from a
  `--conformance` run.

## Phases

Each is sized for one session of 60 to 90 minutes. A phase marked
*decision* cannot start before the owner answers.

| Phase | Issue | What | Unlocks | Decision |
|---|---|---|---|---|
| 0 | #716 | Spike: the plan on its own stack, TLS, sanitizers, stack size, cost | none | **2** |
| 1a | #717 | `open`/`resume`/`close` in the runtime | none | after 0 |
| 1b | #718 | Copy-on-write overlay; forward-only `Cursor` in `Transaction` | none | |
| 2 | #719 | `DECLARE`, `FETCH` forward, `MOVE`, `CLOSE`, `FOR UPDATE` in the dialect and session | none alone | **1** |
| 3 | #720 | `WITH HOLD`; `DECLARE` outside a block | 8 ids, or 1 | **1, 3** |
| 4 | #721 | `SCROLL`, every direction | none | |
| 5 | #722 | `WHERE CURRENT OF` | none as written | **5** |
| 6 | #723 | `Execute` row limit, `PortalSuspended`, streamed results | none | **6** |
| 7 | #724 | Limits, flags, accounting | none | **7** |

Phase 6 depends on 1b only; phases 4, 5 and 7 on 2. Phase 3's eight ids are
e121_01_01_01 to _04, e121_02_01_01, e121_03_01_01, e121_17_01_01 and
_02; they need phase 2's syntax and phase 3's hold.

Risks, by phase: **0** the spike can fail, in which case (c) takes its
place; **1a** the TLS and sanitizer hazards, which the spike must have
found; **1b** the transaction's `Overlay` becomes shared, so every place that
mutates it must go through the clone; the allocation-fault sweep covers it;
**2** `libpg_query`'s parse tree for `FOR READ ONLY` and `ONLY ( t )` is
assumed, not read, and the first thing the phase does is a parse test;
**3** the hold store's size against the budget; **5** the planner's result
description gains hidden columns, which `check()` and the schema reference
must carry.

## Owner decisions

1. **`DECLARE` outside a transaction block.** PostgreSQL: `25P01` unless
   `WITH HOLD`. Recommended: accept it and make the cursor `WITH HOLD`, a
   documented known difference. 8 sqltest ids instead of 1. The alternative
   keeps PostgreSQL exactly and leaves 20 ids marked. (Phases 2, 3.)
2. **The pausing mechanism.** Recommended: the plan on its own stack, after
   the phase 0 spike, with the parked thread as the fallback behind the same
   interface. Not codegen re-entrancy. (Phase 0.)
3. **`WITH HOLD` policy.** Recommended: materialise at `DECLARE`, bounded by
   the statement budget. Alternatives: hold the snapshot with caps (#726),
   or refuse. (Phase 3.)
4. **Syntax PostgreSQL does not have.** `OPEN c` (2 ids: e121_04_01_01,
   e121_08_01_01) and `FETCH … INTO` (8 ids, e121_10) would be accepted only
   as extensions. Recommended: neither; leave the 10 ids marked. `WHERE
   CURRENT OF` on a cursor never fetched stays `24000` (3 ids).
5. **Hidden key columns in a cursor's plan** for `WHERE CURRENT OF`: a
   change to the planner's result description and the schema reference.
   (Phase 5.)
6. **How long a streaming read may pin a snapshot.** (Phase 6.)
7. **The limits' numbers:** open cursors per session (proposed 16), suspended
   runs per server, stack reservation (proposed 256 KiB, set by phase 0), the
   per-session memory bound. (Phase 7.)

## Deferred

- `FETCH LAST` and `BACKWARD` by a reverse run, without buffering: #725.
- Lazy `WITH HOLD` by holding a snapshot: #726.
- Cursors under user `SAVEPOINT` / `ROLLBACK TO`: #727.
- Spilling a cursor's sort or hash to disk: part of the layer-wide spill
  design (`planner_design.md`, *Memory*), not cursor-specific.
- `pg_cursors` and `SHOW` wait on the system catalogs (#496).

## Known differences from PostgreSQL

- A cursor without `SCROLL` never scans backward, where PostgreSQL does for a
  simple plan.
- A `SCROLL` cursor buffers the rows it has fetched, and `LAST` over a
  result past the statement budget is `54000`.
- A sort or hash a cursor needs is built at its first `FETCH` within the
  statement budget, never spilled.
- `WITH HOLD` reads the whole result at `DECLARE`, not at `COMMIT`.
- `DECLARE` outside a block opens a hold cursor, if decision 1 goes as
  recommended.
- A bound on cursors per session.
