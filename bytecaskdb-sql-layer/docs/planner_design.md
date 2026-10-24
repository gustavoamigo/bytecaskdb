# The planner: a logical algebra, a search, and compiled pipelines

Status: **design, approved (owner, 2026-10-24); slices 1 to 4 built, and
slice 5's hash joins and GROUP BY** (*Slice 1 as built*, *Slice 2 as
built*, *Slice 4 as built*, *Hash joins as built*, *GROUP BY as built*):
the pipeline, then System R's search with the cost model and the choice
points (*Choice points as built*), slice 3, *Memory*: the statement budget
and the lifetime rule (`plan_api_design.md`, revision 7), and #551 with
it, slice 4, full scans and sorts (#533), hash joins and `GROUP BY` (#534).
The rest of slice 5 is not built.
The first of the post-MVP designs (#487, *Next*). The parser, the plan API
and TinyCC are unchanged; the binder's output changed.

## Why

After binding, the MVP's planner matches a statement against a fixed set of
physical shapes: `PointGet`, `Scan`, `NullSplitScan` and `PointJoin` to
reach rows, and `Rows`, `Aggregate`, `CountFromKeys` and `MinFromFirstKey`
to produce them. Codegen prints each shape with one template. There is no
logical algebra, no rewrite and no search. Each front-end slice added a
shape, and the shapes don't compose:
- three tables need a new join shape;
- an `ORDER BY` an index can't give has nowhere to put a sort (#533);
- a choice between two selective indexes is a rule a person writes;
- a join holds every inner row and key until the statement ends (#551),
  because per-row storage has no owner.

The bound tree had the same problem one level up: `bound::Select` was one
table with `col op $n` conjuncts, and `bound::Join` exactly two.

## Standing rules

These are unchanged.
- **Plans are ours.** The compiler generates every plan as a program in
  the C subset of `plan_api_design.md` (*The language*), and only those
  plans are supported.
- **PostgreSQL decides semantics, not design.** Results, errors and NULL
  placement follow PostgreSQL 17, and the proof machine compares them.
  How the planner is built does not follow PostgreSQL.
- **Never wait.** No plan blocks on a lock or on memory.
- **Refuse rather than guess.** A statement the planner can't plan is
  refused with 0A000 before anything runs.

## The pipeline

```
SQL ─▶ libpg_query ─▶ AST ─▶ binder ─▶ logical plan ─▶ rewrites ─▶ search ─▶ physical plan ─▶ codegen ─▶ C ─▶ TinyCC
                                  (names, types)          (fixed set)  (System R)              (produce/consume)
```

Each stage has one job and one test seam:
- **The binder** resolves names and types and builds the logical plan
  directly. There is no separate bound tree.
- **Rewrites** turn one logical plan into an equivalent one, the same plan
  every time.
- **The search** picks a physical plan using costs.
- **Codegen** prints a physical plan, with no choices of its own.

## The logical plan

A tree of relational operators over typed expressions. It says what to
compute, not how.

| Operator | Meaning |
|---|---|
| `Get(table)` | every row of a table |
| `Filter(input, predicate)` | the rows for which the predicate is true |
| `Project(input, exprs)` | one output row per input row, of these expressions |
| `Join(left, right, predicate, kind)` | inner first; outer joins with #534 |
| `Aggregate(input, group_by, aggs)` | `COUNT`, `COUNT(DISTINCT)`, `SUM`, `MIN`, `MAX`, `AVG`; no `group_by` means one row |
| `Sort(input, keys)` | each key with its direction and its NULL placement |
| `Limit(input, n)` | the first n rows |
| `Insert`, `Update`, `Delete` | writes, over the rows their input gives |

**Expressions:**
- column references, parameters and constants;
- `+ - * /` with their types' overflow rules;
- comparisons, `AND`, `OR`, `NOT`, `IS NULL`;
- aggregate calls, valid only under `Aggregate`.

Each expression carries its SQL type, which the binder resolves. The
binder also resolves which conjuncts belong to which table. The binder's
own rules don't change: one SQL type per `$n`, and the errors it raises.

`FOR UPDATE` is a flag on the `Get` it locks, carried through to the
physical scan, which guards each row it returns.

## Rewrites

A fixed list, applied in order, each to a fixpoint. None needs a cost.
1. **Constant folding.** An expression of constants becomes a constant; a
   conjunct that is always true is dropped. One that is always false makes
   the input empty.
2. **Predicate pushdown.** A conjunct moves down to the lowest operator
   whose columns it reads: to one `Get` if it reads one table, to a `Join`
   if it reads both sides. A join predicate that is an equality between the
   sides is what the search uses to reach the inner side by key.
3. **Projection pruning.** Each operator keeps only the columns something
   above it reads. That is what makes a covering index read possible.

Subquery decorrelation joins the list when #534 brings subqueries.

The two rewrites of TPROC-C statements (`testing_design.md`, *The TPROC-C
statements*) were made by hand in the statement text, and stay that way.

## The search

System R-style bottom-up dynamic programming over access paths and join
orders (owner, 2026-10-23). It enumerates the plans for one table, then for
every set of two tables, then three, keeping the cheapest plan per set and
per *interesting order*.

**Access paths.** For each `Get` and the conjuncts pushed onto it:
- **A point lookup** when the whole primary key is compared for
  equality.
- **A scan** of the primary key or of an index:
  - the bounds come from an equality prefix plus a range on the next part;
  - the scan is forward or reverse;
  - it is covering when every column read is in the key.
- **A full scan** of the primary key when nothing bounds the table.
  Choosing it is ordinary costing, not a refusal (#533).
- **Answered from the keys alone:**
  - `COUNT` over a scan with no residual conjunct is `bc_count_range`;
  - `MIN` over an `ASC` part after the prefix, with no residual, is the
    scan's first entry.
  
  These are access paths that also produce the aggregate. Today's
  `CountFromKeys` and `MinFromFirstKey` become two of them.

**Interesting orders.** A plan whose output is ordered as a `Sort` above it
needs is kept even when it costs more, because the sort it saves may cost
more than the difference. This is System R's mechanism, and it matters
here more than usual: an index order is often the cheapest way to answer
`ORDER BY … LIMIT`.
- The order includes NULL placement. The key codec sorts NULLs first,
  while PostgreSQL's `ASC` default is last, so a nullable ordered part
  gives the order PostgreSQL asks for only in the two-range form (today's
  `NullSplitScan`).
- Every other placement goes through a sort.

**Joins.**
- Left-deep trees only, since a nested-loop join reads its inner side once
  per outer row.
- **Index nested loop** where the inner table is reachable by key from the
  outer row and the parameters.
- **Hash join** where it isn't (#534), building on the smaller side, with
  that side named in the plan text.
- Bushy trees and merge joins wait for a workload that needs them.

**Sort.** Where no path gives the order, a `Sort` over `bc_sorter_*`, with
NULLs placed by its keys, which `bc_sort_key_*` builds with each part's
direction and NULL placement (`plan_api_design.md`, *Sort keys*). `ORDER BY … LIMIT n` keeps only the first n.

### The cost model

The unit is the one the work budget meters: record reads plus cursor steps
(`bc_budget::max_reads`). So the cost of a plan is a prediction of what it
will charge.
- **A point lookup:** 1.
- **A scan:** 1 per entry it reaches, plus 1 per row it fetches when it
  isn't covering.
- **A join:** the outer cost, plus the outer rows times the inner cost per
  probe.
- **A sort or hash build:** the rows it holds, plus a weight per row for
  the CPU and the memory. The weight is a constant, measured once and
  stated in this document: 0.5 for a sort (*Slice 4 as built*), 0.3 for a
  hash build (*Hash joins as built*).

**Cardinalities** come from `bc_count_range`, which is exact and costs at
most two record reads whatever the range holds.
- **Bounds that are constants** are counted when the plan is compiled.
- **Bounds that depend on `$n`** are unknown at compile time. The plan
  carries the choice instead (*Choosing at run time*).
- **Where neither works,** for example a filter on a column no key holds,
  a fixed selectivity applies. It starts at System R's 1/3 for a range
  comparison and 1/10 for an equality, and is checked on the harness with
  the weights (*Decisions*, 9).

A plan is compiled once per statement and kept until the catalog changes,
as today. Counts read at compile time age as tables grow. That's accepted:
the choices that matter in OLTP depend on `$n`, and those are made at run
time.

### Choosing at run time

The owner's choice of the three options (2026-10-23). The others were a
generic plan from heuristics, or a custom plan per execution as PostgreSQL
makes for a statement's first runs.

A plan is a program, so it can contain two alternatives and pick one with
`bc_count_range` when it runs:

```c
int64_t n = bc_count_range(cx, IDX_ORDERS_I2, lo, hi, 1000);
if (n < 1000) {
  /* index path: scan orders_i2 [lo, hi), fetch each row */
} else {
  /* primary-key path */
}
```

**The rules:**
- **Only where it matters.** A choice point goes where the search finds
  two plans for one table whose costs cross within the range of the
  unknown count. A full primary-key lookup never gets one.
- **Two alternatives per choice point.** The threshold is where their
  costs cross, computed by the search from the cost model, and the count
  is limited to the threshold so it costs no more than it must.
- **Choices don't multiply.** One choice point per table. Past a small
  fixed number in one plan, the search takes its compile-time choice
  instead, so plan size stays bounded.
- **The plan text shows both branches and the threshold.** `EXPLAIN` is
  the plan text, so it shows the rule and not one branch.
- **The cost is charged like any read.** The count's two record reads
  come out of the statement's budget.

The result is one compiled plan per statement: nothing is re-planned, and
nothing in the plan cache depends on the parameters.

## The physical plan and codegen

The physical plan is a tree of operators: the access paths above, `Filter`,
`Project`, the joins, `Aggregate`, `Sort`, `Limit`, the choice points, and
the sinks (`Emit` for result rows, `Insert`, `Update`, `Delete`). The
search builds it, and `check()` enforces what the types can't express, as
the MVP's `ir::check` did. A failure is a compiler bug, never a plan.

**Codegen is produce/consume** (Neumann, "Efficiently Compiling Efficient
Query Plans for Modern Hardware", VLDB 2011; HyPer, then Umbra). Each
operator has two methods:
- `produce()` asks its input to emit rows;
- `consume(row)` prints the code that runs on each row its input emits.

A scan's `produce` prints `while (bc_scan_next(...)) {`, then calls its
parent's `consume` inside the loop. A filter's `consume` prints an `if`
that `continue`s, then calls its parent's. A join's `consume` prints the
inner probe, a nested loop. Everything between two pipeline breakers
becomes one nested loop, with the row's values in locals.

A pipeline breaker (`Sort`, a hash build, `Aggregate` with groups) consumes
by adding to its structure, then produces by looping over it:

```c
while (bc_scan_next(cx, c)) {           /* order_line, bounded by the prefix */
  /* filter, project */
  bc_sorter_add(cx, s, sort_key, payload);
}
while (bc_sorter_next(cx, s, &p)) {     /* the sort's produce */
  /* limit counter; emit */
}
```

**Why produce/consume.** The plan language has no structs, arrays or
pointers, and a `while` condition is exactly one of the API's iteration
calls. Volcano iterators would need iterator objects, and vectorized
execution would need batches of rows. Neither can be written in the subset
without widening it. Produce/consume generates what the subset already
allows: nested loops over API cursors, which is what today's codegen
prints for each fixed shape. The redesign makes those loops compose per
operator instead of per shape.

**The costs.**
- Two codegen methods per operator, which is harder to write than an
  iterator.
- Plan text, and TinyCC's compile time, grow with the query. This is fine
  at OLTP sizes: TinyCC compiles today's plans in about 1 ms, two thirds of
  it fixed cost.
- A choice point prints its consumer once per branch.

The generated C stays readable, and it stays `EXPLAIN`'s output.

## Memory

**One memory budget per statement, charged the way the work budget is**
(owner, 2026-10-23). It replaces two things the plan API has today:
- **The arena's size.** Today a statement's arena holds every key, row and
  result, and is reset only after the statement.
- **The byte bound each blocking structure declares at open.** These are
  `bc_sorter_open`'s, `bc_hash_open`'s and `bc_agg_open`'s `bytes`
  arguments.

PostgreSQL's `work_mem` is per operator, so a plan with several sorts can
use several times the setting. One number per statement is simpler to
state and to enforce.

**What is charged.** Every byte the plan API allocates for the statement:
- keys and rows it builds or reads;
- the contents of sorters, hash tables and aggregate groups;
- pinned values;
- the result rows it buffers;
- the write set: what the statement keeps until commit, or in a
  transaction until it ends, when it moves to the transaction's own bound
  (`plan_api_design.md`, *The write set is statement memory*). So a bulk
  write is bounded by the budget like any other statement.

It counts fixed amounts, not what the allocator reports, so the accounting
is deterministic and the allocation-fault sweep can test it. Scratch
copies, results and structure entries are charged the bytes requested plus
a fixed amount per node. The write set is charged more, a conservative
bound: each buffer at its capacity plus `kAllocBytes` for the allocator's
header, each slot at twice its size, and each node in full, as
`plan_api_design.md` lists.

**Ownership decides what is held.**
- A view returned inside a loop lives until that loop's next step, unless
  it is pinned or added to a structure. A key built for one probe, an
  inner row and a group key all go when the outer row moves on. That is
  the fix for #551.
- What outlives a step: pins, the contents of blocking structures, result
  rows and the write set.

How the API releases a step's storage (a mark on the arena per loop level,
or storage held with the cursor) is the implementation PR's to choose; it
marks the arena (`plan_api_design.md`, *A loop's step owns what it
allocates*). This rule is the contract. Streaming
operators therefore use memory bounded by the depth of the pipeline, not
by the number of rows, and only pipeline breakers, results and writes grow.

**Past the budget, an error, never a spill.** This is the plan API's
existing guarantee (*Bounded in memory*), kept.
- The error is SQLSTATE 54000 (`program_limit_exceeded`), since it is a
  limit we set, not the machine running out.
- PostgreSQL spills sorts and hashes to disk instead. That is a known
  difference, listed in the contract.
- Spilling (an external sort, a partitioned hash join) is a separate design
  for when a workload needs it.

**The planner uses the budget.** Where another plan avoids a sort or a
hash build, such as an index that gives the order, the structure's size is
an unknown count, and the two plans are a choice point (*Choosing at run
time*), within its caps. The count is of the range the structure's input
reads, a full scan's being the table's, by `bc_count_range` when the plan
runs, limited to the least count at which the structure would pass the
budget: below it the structure, from it the other plan. The count bounds
the rows held whatever the filters keep, since they can't be fewer than
none nor more than the range holds. No count read at compile time decides
it: a plan is kept while its tables grow.

**Not covered.** Memory across statements and sessions, a global cap,
belongs with the thread-model design. So does the result buffer. Results
are buffered until the plan returns, because plans are effect-free until
commit, so a large result set counts against the budget. Streaming results
to the client is part of the transport design.

## What the plan API gains and loses

- **Loses:**
  - the `bytes` argument of `bc_sorter_open`, `bc_hash_open` and
    `bc_agg_open`;
  - `bc_ctx_new`'s `arena_bytes`, which becomes a memory field in
    `bc_budget`, beside `max_reads`.
- **Gains:**
  - the lifetime rule above, stated in `plan_api_design.md`;
  - working `bc_sorter_*` and `bc_hash_*` calls and aggregate slots, which
    `bc_plan.h` declared and which set `BC_E_HANDLE` until the plan-API half
    of slices 4 and 5 built them (`plan_api_design.md`, revision 8);
  - a `keep` argument for `bc_sorter_open`, where 0 keeps every entry: a
    top-n sorter holds at most the first n in sort order, so
    `ORDER BY … LIMIT n` (*The search*, Sort) holds n entries whatever the
    input, and an entry that sorts after the n held is not copied.

Plans are only ever ours and are regenerated from SQL, so changing these
signatures breaks no stored plan.

## Tests

- **Golden plans.** Every TPROC-C statement keeps its golden plan. The
  rewrite changes the text, so the first slice reviews each diff by hand,
  and the C may differ while the plan's reads must be the same. After that
  slice, a golden diff shows every plan a later change moves.
- **The proof machine against PostgreSQL** generates statements beyond what
  the MVP planner serves, as each operator lands. Its `served` model, which
  keeps the machine to what the planner plans, shrinks until only the
  dialect's own limits remain.
- **Alternatives agree.** For a statement, the search can be asked for
  every plan it costed, not only the cheapest. The proof suite runs all of
  them on the same data and requires the same rows. This catches a wrong
  rewrite or a wrong access path without a reference result, and it runs
  both branches of every choice point.
- **Choices.** Unit tests give the search fixed counts and check the plan
  it picks and the thresholds it prints.
- **Memory.** A statement that holds more than the budget fails with 54000
  and leaves nothing behind. A join over many outer rows stays within a
  small budget, which is #551's test. The allocation-fault sweep covers the
  new allocations.
- **The HammerDB gate and the in-process harness** run unchanged, and their
  numbers before and after go in the PR that switches the planner.
- **The benchmark for comparing versions** (#487, *Next*) is built alongside
  this design. sysbench's `oltp_*` workloads are the acceptance tests for
  the operators they need: a secondary index, `ORDER BY` on a non-key
  column, `DISTINCT`.

## Slices

Each slice is one PR with its tests.
1. **The new pipeline at parity.**
   - The logical plan, the rewrites, a search with only the MVP's paths,
     the physical plan and produce/consume codegen.
   - Every MVP statement plans as it does today: the golden diffs are
     reviewed, and the proof machine and the HammerDB smoke pass.
   - The old IR, planner and codegen are removed in the same PR, so only
     one planner exists.
2. **Costs and choices.** The cost model, cardinalities from
   `bc_count_range`, interesting orders and choice points.
3. **Memory.** The statement budget, the lifetime rule, and #551.
4. **Sorts and full scans.** `bc_sorter_*`, and the refusals of #533
   lifted.
5. **The dialect.** Hash joins, `GROUP BY`, three or more tables, and the
   rest of #534, each with its proof-machine rules and a sysbench workload
   where one exists.

## Slice 1 as built

The pipeline of this design with the MVP planner's choices. Every golden
plan regenerated unchanged, byte for byte, so each statement makes the
same API calls and reads as before.

| Stage | Module |
|---|---|
| Binder | `sql/binder.cppm`: DDL to the catalog's statement values, DML to the logical plan |
| Logical plan | `plan/logical.cppm` |
| Rewrites | `plan/rewrite.cppm` |
| Search | `plan/search.cppm` |
| Physical plan and `check()` | `plan/physical.cppm` |
| Codegen | `plan/codegen.cppm` |

**The logical plan the binder builds.**
- A `SELECT` is `Limit? → Project | Aggregate → Sort? → Filter? → Get`,
  with a `Join` of two `Get`s in place of the `Get` for two tables.
- Every `WHERE` conjunct, a join's `ON` among them, is in one `Filter`
  above the tables, in the order written. Pushdown moves it.
- `UPDATE` and `DELETE` are over `Filter? → Get`; `INSERT` is one row of
  parameters.
- The expressions are the dialect's: columns, parameters, integer
  constants, `+` and `-`, comparisons, their conjunction and aggregate
  calls. `*`, `/`, `OR`, `NOT` and `IS NULL` come with the dialect that
  accepts them (#534).
- A `Sort` key's NULLs go where PostgreSQL's default for its direction
  puts them. The dialect has no `NULLS FIRST` or `NULLS LAST`.

**The rewrites.**
- Constant folding finds nothing to fold in today's dialect: a `WHERE`
  compares a column with a parameter, and a literal follows a column or a
  parameter. It compares two constants by their SQL types: exact numbers
  at the larger of their scales, a `BOOLEAN`, `DATE`, `TIMESTAMP` or `TIME`
  only with one of its own type, and anything else not at all. It adds two
  only when both are in the result's representation.
- A conjunct always false replaces its `Filter`, and what it reads, with an
  `Empty` naming the tables it replaced; a `Join` with an `Empty` side is
  `Empty`. The search plans it as no rows, read from nothing.
- Pushdown moves a join's conjuncts onto the table each reads, and its
  column comparisons onto the `Join`. Two `Filter`s in a row become one, the
  lower one's conjuncts first, so a `Get` has at most one.
- Pruning sets each operator's output columns. A read is covering when
  every column the `Project` or `Aggregate` takes is in the key, so a
  column only a key bound reads need not be.

**The physical plan.** Its operators:
- `Empty`, no rows and no read: the sink reports what it gives for none;
- the access paths `PointGet`, `Scan` and `NullSplitScan`;
- `CountRange` and `MinFirstEntry`, the access paths that also produce the
  aggregate;
- `IndexJoin`, the index nested loop;
- `Filter` and `Limit`;
- the sinks: `Emit`, `Aggregate`, `Insert`, `Update` and `Delete`.

The plan also lists the parameters a NULL of which ends it before it reads,
and the `$n ± k` values computed before anything is read. `check()` refuses,
as a compiler bug:
- a key part or bound whose parameter the plan doesn't check for NULL;
- a scan with neither a prefix nor a range;
- a covering scan under `FOR UPDATE`, or emitting a `DOUBLE`;
- a NULL split whose values don't start past the NULL part;
- `MIN` from the first entry of a reverse scan;
- a `LIMIT` anywhere but under `Emit`;
- a write that sets no column, or sets one from another type.

**Codegen.** Produce/consume, with each consume passed to the input's
produce as a continuation.
- A leaf's produce prints the loop over its cursor, or the one `bc_get`,
  and runs its parent's consume inside.
- A row that fails a `Filter`, or a key that names no row, goes to the next
  row in a loop. On a single row it ends the plan with what the sink gives
  for no rows.
- An `Aggregate` over at most one row keeps no accumulator. Its input's
  misses become the condition its row is emitted under.

**How each construct maps to the plan API.** No new loop or structure is
needed; every pattern below uses calls `bc_plan.h` declares.

| Construct | Physical operator | Plan text |
|---|---|---|
| A whole primary key by equality | `PointGet` | `bc_key_params` (consecutive parameters) or `bc_key_begin` / `bc_key_*` / `bc_key_end`; `bc_get` |
| An equality prefix | `Scan`, its `prefix` | `bc_scan_open(lo = hi = prefix, BC_SCAN_PREFIX)` |
| A prefix and a range | `Scan`, its `rest` a `Range` | bounds from `bc_key_*`; an exclusive `lo` or an inclusive `hi` through `bc_key_successor` (`bc_plan.h`, *bounds*) |
| `DESC` order | `Scan::reverse` | `BC_SCAN_REVERSE` |
| A nullable ordered part, its NULLs placed as PostgreSQL places them | `NullSplitScan` | two `bc_scan_open`s. Present values: `bc_scan_open(lo = bc_key_successor(prefix + NULL), hi = bc_key_successor(prefix))`, no `BC_SCAN_PREFIX`. NULL part: `bc_scan_open(prefix + NULL, prefix + NULL, BC_SCAN_PREFIX)`, where `prefix + NULL` is the prefix's key with `bc_key_null` appended. `ASC`: present values forward, then the NULL part; `DESC`: the NULL part reversed, then the present values reversed. One loop body, one `LIMIT` counter |
| A covering read | `Scan::covering` | `bc_key_part_*` on `bc_scan_key`; otherwise `bc_scan_row` |
| A residual filter | `Filter` | `bc_col_*` on the row, compared in an `if` that `continue`s; a NULL operand rejects the row |
| `+` / `-` | an `Add` or `Sub` expression | `bc_add` / `bc_sub`; compared, computed once before anything is read and range-checked (below) |
| `SET col = col + $1` | `Update::set` | `bc_row_from`, `bc_row_set_*` with the sum, `bc_update`; on a BIGINT read, the sum by `bc_add_bigint` / `bc_sub_bigint` (the `Add`'s or `Sub`'s type), which raise "bigint out of range" on the table |
| UPDATE or DELETE of many rows | `Update`, `Delete` over a `Scan` | the scan, with `bc_update` / `bc_delete` per row; `bc_emit_count` with the rows written |
| `ORDER BY` | the access path's order | none: the search picks a path in that order |
| `LIMIT` | `Limit` | an `int64_t` counter in the scan loop's condition, so the cursor never steps past the limit |
| `COUNT(*)`, `COUNT(col NOT NULL)`, no residual | `CountRange` | `bc_count_range` over the access path's range |
| `COUNT`, `SUM` otherwise | `Aggregate` | an accumulator local, `bc_add`; a "seen" `bool` for SUM's NULL |
| `MIN` otherwise | `Aggregate` | a local holding the least non-NULL value, kept by comparison: `<` on integers, on doubles `<` with NaN taken as greater than every other value (`docs/types_design.md`, *Double arithmetic*); a "seen" `bool` for the NULL. Never a bytes column (`frontend_design.md`, *The dialect*) |
| `MIN(col)`, `col` an `ASC` part directly after the equality prefix, no residual | `MinFirstEntry`, its `Scan` forward, its range from past the NULL part when `col` is nullable | the first entry of the scan, which starts past the NULL part when `col` is nullable. A `DESC` part (its first entry is the maximum) takes the `Aggregate` row |
| `COUNT(DISTINCT col)` | `Aggregate` | `bc_agg_group` keyed by the column, then a count of `bc_agg_next` |
| A join | `IndexJoin` | the inner key built from the outer row: `bc_key_*` of `bc_col_*`; nested in the outer loop |
| `FOR UPDATE` | the access path's `for_update`, from the `Get`'s | `bc_guard_row` on each returned row: `bc_guard_unchanged` on its primary key |
| Targets | `Emit`, or the aggregate's row | `bc_emit_col`, `bc_emit_key_part`; for an aggregate, `bc_emit_i64` (`COUNT`, `SUM`, and `MIN` of an integer, `DECIMAL`, `DATE`, `TIMESTAMP` or `TIME`), `bc_emit_f64` (`MIN` of a `REAL` or `DOUBLE PRECISION`), and `bc_emit_null` over no rows |

A NULL that a key needs means no row can match, since `=` and the range
operators reject NULL:
- **a parameter:** the plan ends early, before any read (the physical
  plan's `null_checks`);
- **an outer row's column, in a join's inner key:** that outer row is
  skipped (`continue`), and the loop goes on to the next one.

**The range check on computed values.** `ol_o_id >= $3 - 20` computes an
`integer` that never reaches a row builder, and PostgreSQL raises 22003 when
such an operation leaves its type. `bc_add` / `bc_sub` check only `int64_t`,
and the row builder checks only what it stores, so the plan calls
`bc_check_int` with the value and the SQL type the compiler resolved, and it
sets `BC_C_RANGE` when the value is outside it.
  - *The rule:* which type an operation is in is a decision, the
    compiler's operator resolution, so it shows in the plan text as one
    call. What the type's bounds are is mechanics, in the runtime with the
    row builder's.
  - Assignments need no call: the row builder checks the stored value
    already (`CONTRACT.md`, the row on values outside their declared type).
  - *`SET col = col ± $n` on a BIGINT:* the sum itself can leave `int64_t`,
    so the plan adds with `bc_add_bigint` / `bc_sub_bigint`, which raise
    `BC_C_RANGE`, "bigint out of range". The type is the SQL type of the
    column read, which the binder gives the `Add` or `Sub`, as PostgreSQL's
    operator takes its result type from its operands; the assigned column
    only checks the stored sum. The binder takes a BIGINT and a `DECIMAL` of
    scale 0 as one type, the only crossing: a `DECIMAL(p, 0)` read cannot
    leave `int64_t`, its operand held to 18 digits, so its overflow is the
    assigned column's ("numeric field overflow").
    `test_column_plus_parameter_across_bigint_and_scale_0_decimal` runs both
    crossings against PostgreSQL.
  - *`$n ± k` in a `WHERE`:* `$n` is checked as an `INTEGER` before the plan
    runs, so `bc_add` cannot overflow `int64_t`
    (`test_a_computed_bound_past_integer_is_22003_before_the_plan_runs`).

`COUNT(DISTINCT)` uses the aggregate structure `bc_plan.h` declares
(`bc_agg_open` with no slots, `bc_agg_group`, `bc_agg_next`).

### The search's choices

The MVP planner's rules, which slice 1's search kept: no cost model,
chosen so every TPROC-C statement gets the plan a person would write.
Slice 2 replaced them with costs (*Slice 2 as built*), and its refusals
are there.

**Access path, per table.** A path is the primary key or one of the
table's indexes. It is *usable* when its first part is compared for
equality or bounded by a range. It *serves* an `ORDER BY` when the
ordered columns are its parts after its equality-compared ones, in its
order, all `ASC` (a forward scan) or all `DESC` (a reverse one); ordered
columns compared for equality may be left out. Where the NULLs of the
ordered columns fall is checked after the path is chosen (step 4).

A point lookup on the whole primary key serves any `ORDER BY`: it returns
at most one row.

1. Every primary-key column compared for equality: a point lookup.
2. Otherwise, with an `ORDER BY`, only the usable paths that serve it
   remain. None remaining: refused (*What the search refused*).
3. Of those, the one with the longest leading run of equality-compared
   parts wins; at equal runs, one with a range part after the run beats
   one without. Remaining ties go to the primary key, then to the covering
   path, then to the lower id.
4. Then the chosen path's NULLs are checked against PostgreSQL's
   placement (below). One it can't place is refused (row 6); the search
   doesn't go back to try another path. This is deliberately naive, until
   slice 2's costs.

So `WHERE a = $1 AND b = $2 ORDER BY d`, on a primary key `(a, b, c)` and
an index `(a, d)`, takes the index: one equality part and
`b = $2` as a residual, rather than the longer primary-key prefix, which
can't give the order.

**NULLs in an ordered part.** An index's order puts NULLs where PostgreSQL
doesn't: on an `ASC` index part the key codec sorts the NULL marker first
(`docs/types_design.md`), so a scan gives `NULLS FIRST` for `ASC` and
`NULLS LAST` for `DESC`, and PostgreSQL's defaults are the reverse. A
part the `WHERE` compares, by equality or a range, holds no NULLs in the
rows it can match, and counts as `NOT NULL` here. The chosen path places
NULLs as PostgreSQL does when:
- every ordered part is `NOT NULL`: one range; or
- exactly one is nullable, it is the first ordered part after the
  equality prefix, and it is an `ASC` index part: the two ranges of
  `NullSplitScan`, the present values and the NULL part, in PostgreSQL's
  order, with one `LIMIT` across both. Ordered parts
  after it are `NOT NULL`, so within each range the index order is
  PostgreSQL's.

Anything else is row 6: two nullable ordered parts would need a range per
combination of NULLs; a nullable part after another ordered part, its
NULLs placed within each value of that part; a nullable `DESC` index part
(its NULL marker and values inverted), other bounds. All are #533.

Take `WHERE c_w_id = $1 AND c_d_id = $2 ORDER BY c_first`. No usable path
of `customer` serves it: `customer_i2` is `(c_w_id, c_d_id, c_last,
c_first, c_id)`, and `c_last` lies between the prefix and `c_first`. So
it is row 5, before nullability is looked at. P6's `WHERE c_w_id = $1 AND
c_d_id = $2 AND c_last = $3 ORDER BY c_first` takes `customer_i2` with a
three-part prefix; `c_first` is nullable and directly after it, so the
plan is a `NullSplitScan`. `ORDER BY c_first, c_id` plans the same way,
`c_id` being `NOT NULL`.

TPROC-C's ordered columns are `c_first` (P6, O2), `o_id` (O4) and `no_o_id`
(D1). `o_id` and `no_o_id` are `NOT NULL` in the schema fixture. `c_first`
is nullable there, as in HammerDB's MySQL schema, so P6's and O2's golden
plans show the two-range form. HammerDB's default PostgreSQL schema
declares it `NOT NULL`; the fixture keeps the MySQL types and the plan
doesn't rely on either. No TPROC-C index has a `DESC` part.

`MIN` taken from the first entry needs an `ASC` part and no residual, and
starts past the NULL part. Otherwise the scan runs to its
end and a local keeps the minimum.

`bc_count_range` cardinalities are not consulted. With TPROC-C's schema
every statement has a full key or a selective prefix, so a cost model
changes no plan.

**Where filters run.** Each conjunct the key expresses becomes a bound.
Every other conjunct is a residual, run on the row as soon as its loop
reaches it, before anything is counted, emitted, guarded or written: S2's
`s_quantity < $4` runs on each `stock` row the join fetches. Of two bounds
on one side, the first in `WHERE` order is the key bound and the other a
residual.

**`ORDER BY`** is served only by the access path's order, forward or
reverse, on a single table: the logical `Sort` becomes no physical
operator. Sorters (`bc_sorter_*`) are slice 4 (#533).

**Joins.** Index nested loop only, two tables only.
- The outer table is the one reachable from parameters alone, a point
  lookup over a scan. The inner one must be reachable by its whole primary
  key from the outer row's columns and parameters. Only an equality keys
  it; every other comparison of the two tables' columns is a filter on the
  pair, with its own operator.
- An `ORDER BY` on a join is refused. No TPROC-C join has one.
- N1: both are point lookups; the order is the `FROM` order.
- S2: `order_line` is outer (a prefix and a range from parameters), and
  `stock` is a point lookup by `(ol_i_id, $1)`.
- Hash joins wait for a statement that needs one.

**Writes inside a statement.** An `UPDATE` or `DELETE` of many rows writes
each row as its scan reaches it. The statement reads the transaction's view
as it stood at the statement's start (`docs/transaction_design.md`,
*Statements inside a transaction*), so its own writes never feed back into
its scan.

#### What the search refused

Slice 1 refused, in this order: an `ORDER BY` on a join; a join where
neither table is reachable from parameters alone, or whose inner table no
key reaches from the outer row; a table with no usable path; usable paths
none of which gives the `ORDER BY`; and a chosen path that can't place its
`ORDER BY`'s NULLs as PostgreSQL does. *Slice 2 as built* has what
replaced it.

## Slice 2 as built

System R's search and the cost model. It was built in two steps: the
search and the cost model first, a bound that depends on `$n` taking the
fixed selectivities; then the choice points (*Choice points as built*).
Through both, every TPROC-C golden plan regenerated unchanged, byte for
byte, so no TPROC-C statement's reads changed. The TPROC-C statements
compiled on the harness's loaded database, with its counts, give their
golden plans too.

**The source.** The search flattens what the statement reads into its
tables, in `FROM` order, each with the conjuncts on it, and the join's
conjuncts between two of them. One validator, `use_of`, decides what the
search can use, for a table's `Filter` and a `Join`'s predicate alike: a
column of the table compared with a parameter or with `$n ± k`, or with a
column of another table only in a `Join`'s predicate and only when that
table is on the `Join`'s other side. A predicate conjunct that compares a
column with a parameter goes to its table, as pushdown would put it.
Anything else is refused (*What the search refuses*).

**Access paths, per table.**
- The point lookup, when the whole primary key is compared for equality.
- A scan of the primary key and of each index whose first part is
  compared: the equality prefix, the range on the next part, forward or
  reverse. It covers when no filter reads its rows and its key holds every
  column read from them: the table's result and aggregate columns, and
  its join conjuncts' columns, which a key built from the row or a filter
  on the pair reads. One rule for every table, in a join or alone; a
  covered column is read from the entry's key wherever a row's would be.
  A table read once under a join conjunct is filtered by it, so that scan
  reads its rows. A scan of the whole primary key by equality is the point
  lookup, not built twice.
- No full scan (slice 4).

**Joins.** Left-deep, bottom up over sets of tables: each set of k tables
from each set of k − 1 and one more table, read one of two ways:
- by its whole primary key, from parameters and the columns of the tables
  already read: an index nested loop, one lookup per outer row;
- by its own scan, from parameters alone, when the tables already read give
  at most one row (a point lookup, then lookups by key): a nested loop
  (`NestedLoop`) that reads it once, and not at all when that row is
  missing. The join's conjuncts between it and the tables already read
  filter its rows, as they filter a lookup's, refused on a DOUBLE or REAL
  column alike.

The search keeps the cheapest plan per set, the first of equals, so `FROM`
order breaks ties, and the cheapest of at most one row apart, which the
second way extends. The whole statement's plans are all finished, emitted under
`LIMIT` or aggregated, before they are compared, since a `LIMIT` changes
which is cheaper. Sets are bit masks of up to 32 tables; the dialect has
two.

**Interesting orders.** No sort exists yet, so the `ORDER BY`'s order is
the only interesting one, and a path that doesn't give it is not built:
the cheapest plan per set is the cheapest per set and order.
- A plan's order is its first table's scan's, and a key lookup per row
  keeps it. So an `ORDER BY` on a join is planned when the first table's
  scan gives it, on that table's columns.
- The order includes NULL placement. A nullable ordered part gives
  PostgreSQL's order only in the two-range form: first after the prefix,
  `ASC`, every later ordered part `NOT NULL`.
- A point lookup gives any order.

**Answered from the keys.** `COUNT` over a scan with no filter is
`bc_count_range`; `MIN` over a forward scan whose next part is the
argument's `ASC` part, with no filter, is the scan's first entry. Each is
a plan beside the loop over the same scan, and costs decide between them.

### The cost model as built

| Plan | Cost |
|---|---|
| A point lookup | 1 |
| A scan | its entries; twice that for an index scan that fetches each row |
| `COUNT` by `bc_count_range` | 2 |
| `MIN` from the first entry | 1; 2 when it fetches the row |
| A join by key | the outer plan's cost, plus 1 per outer row |
| A join read once | the outer plan's cost, plus the inner scan's |
| `COUNT(DISTINCT)` | its input's, plus 1 per row (`bc_agg_next`) |
| Under `LIMIT n` | the cost × n / the rows, when there are more than n |
| A choice point | the plan's, as compiled, plus 2 for the count |

- **Entries.** A scan reaches the table's rows × 1/10 per
  equality-compared part × 1/3 per range bound. It reaches at most 1 when
  the prefix covers a unique index's columns, and at least 1.
- **Rows.** A plan's entries, or 1 for a point lookup, × 1/10 per filter by
  `=` and 1/3 per other filter. A join's rows are the outer plan's × the
  inner side's (1 for a lookup by key), its join conjuncts counted as
  filters, whether the inner side is looked up or read once.
- **A table's rows** are counted when the plan is compiled: `Database`
  counts each table's primary key on one snapshot, as `bc_count_range`
  does, at most two record reads each. Every `WHERE` operand in today's
  dialect is `$n` or `$n ± k`, so the whole table is the only constant
  bound. A plan compiled with no database, as the golden plans are, costs a
  table at 1,000,000 rows. That is large enough that a key read beats a
  loop, as it does on a table where the choice matters.

**The selectivities, measured** (*Decisions*, 9). On the harness's
two-warehouse load, the fraction of a table's rows each TPROC-C key bound
keeps, its parameter drawn as a row's own value:

| Bound | Statements | Rows | Kept | Per conjunct |
|---|---|---:|---:|---:|
| `new_order (w, d)` | D1, D2 | 18,000 | 5.0e-2 | 0.224 |
| `orders (w, d)` | O4 | 60,000 | 5.0e-2 | 0.224 |
| `order_line (w, d)` | S2 | 600,251 | 5.0e-2 | 0.224 |
| `customer (w, d, c_last)` | O1, O2, P5, P6 | 60,000 | 1.6e-4 | 0.055 |
| `order_line (w, d, o_id)` | D5, D6, O5 | 600,251 | 1.8e-5 | 0.026 |
| `orders (w, d, o_c_id)` | O4 | 60,000 | 1.7e-5 | 0.026 |
| `ol_o_id` in `[next − 20, next)`, within `(w, d)` | S2 | 600,251 | 6.6e-3 | 0.081 per bound |
| `s_quantity < 10..20` | S2's filter | 200,000 | 5.6e-2 | 0.056 |

- An equality keeps 0.026 to 0.224 per conjunct, 0.087 by their geometric
  mean: close to 1/10.
- A range keeps far less than 1/3: 0.081 per bound, 0.056 for the filter.
- The constants stay System R's. With the measured 0.087 and 0.081 in
  their place, no golden plan moves.
- A range's 1/3 is the number to revisit when a workload's choice turns on
  it.
- The per-row weight of a sort is slice 4's (*Slice 4 as built*), and of
  a hash build the hash joins' (*Hash joins as built*).

### Choice points as built

The design's *Choosing at run time*, in the search (`choose`), the physical
plan (`Choice`) and codegen. Every bound in today's dialect depends on
`$n`, so every scan's count is unknown when the plan is compiled.

**Where they go.** At each table a plan reads by a scan, outer first: the
first table's access, or the table read once under one row. Never at a
point lookup or a lookup by key, nor at a scan whose bounds name at most
one row (a unique index's whole key). The plan's access path is the first
plan; the second is another range of the same table, from its access
paths. Of those whose costs cross the first one's, the cheapest at the
estimates is taken, so each choice point has exactly two plans. One per
table, and at most 4 per plan (*Decisions*, 11): past that, the search
keeps its compile-time choice. A plan of today's dialect holds at most
one, since a table is read once only under a point lookup.

**The threshold.** Each plan's cost is the whole statement's, as the cost
model finishes it (a join's lookups, `LIMIT`, `COUNT(DISTINCT)`), with the
first plan's count as the variable: the threshold is the least count, from
1 to the table's rows, at which the first plan costs more than the second.
The two ranges' counts are related where their bounds are: a range whose
bounds take only conjuncts the other's take holds the other's rows, so its
count is at least the other's; one whose bounds take more holds at most
the other's. Costs that only meet, as where one range holds the other's
rows, never cross, and get no choice point. Neither does a pair whose
costs cross only past the table's rows, or at a count of 1. So under
`LIMIT 1` a scan with no filter costs its first entry whatever its count,
and gets none.
- `WHERE a = $1 AND b = $2` with indexes on `a` and on `b`, 10,000 rows:
  each scan reaches 1,000 entries and fetches each row, 2,000. Past 1,000
  of `a`'s entries its scan costs more than `b`'s: threshold 1,001.
- The plan's cost is its first plan's plus the count's 2.
- `crossing()` finds the threshold by binary search, so it assumes the
  difference of the two costs changes sign at most once between 1 and the
  table's rows. Both costs grow with the count, one of them linearly, the
  other at most so where a `LIMIT` or a sort's top-n flattens it, and that
  holds for every pair the search forms. A debug build checks it at counts
  spread over the range (1, 2, 3, 4, 6, 9, … and the table's rows), and a
  pair that crosses twice is a `std::logic_error`, a compiler bug.
  Superseded in #591's review: a sorted cost and `entries_beside`'s clamps
  can cross twice, so such a pair gets no choice point, in every build
  (*Slice 4 as built*).

**The plan text.**

```c
bc_view lo0 = …;                      /* the first plan's range, a NULL split's whole prefix */
bc_view hi0 = …;
int64_t count0 = bc_count_range(cx, IDX_6, lo0, hi0, 50001);
if (count0 < 50001) {
  /* the first plan's scan, opened on lo0 and hi0, its filters, then the consumer */
} else {
  /* the second plan's, then the consumer again */
}
```

The count is limited to the threshold, and its two record reads are
charged like any read. The consumer, from a join's lookup to the sink, is
printed once per branch. Each plan's parameters are checked for NULL and
its `$n ± k` computed before anything is read, as any plan's.

**What `check()` adds.** A choice point counts exactly its first plan's
range: its object, its prefix's parameters, its bounds and its direction,
or for a NULL split its whole prefix. Its threshold is 0 or more, and each
plan is a scan of the counted table. The counted range's key is built once:
the first plan's scan opens on it (a NULL split builds its own two
ranges).

**Golden plans.** None of TPROC-C's gains one. Three statements have two
ranges of one table: O1 and P5 count `customer`'s by `bc_count_range`,
whose cost is 2 at any count, and O4's two costs under `LIMIT 1` never
cross (`orders_i2`'s first entry ends its scan, and the primary key's
range holds its rows). Every other statement has one range per table, or
a key lookup. The fixture's
`choice_pk_or_index`, `choice_read_once` and `choice_update` show one,
alone, read once under a point lookup, and in an `UPDATE`;
`no_choice_under_limit` shows the `LIMIT` case.

### What the search refuses

A statement is refused only when no plan computes it. Slice 1 chose one
path and refused the statement when that path failed; slice 2 tries each.
So these are planned now, where slice 1 refused them:
- a table whose first-choice path left a DOUBLE filter or could not place
  its NULLs, but which another path serves;
- an `ORDER BY` on a join that the first table's scan gives;
- a join whose second table is keyed by parameters alone, the first one
  scanned: that table is read once, first, and the other's scan runs only
  if its row is there.

| # | Refused | Message |
|---|---|---|
| 1 | A table no key reaches; a join none of whose tables a key reaches | `no plan: a full scan of <table, or the first table in FROM>` |
| 2 | A join where no order reads a table by its whole primary key | `no plan: no equality names <table>'s key`, or `no plan: no equality names <columns> of <table>'s key` |
| 3 | No access path in the `ORDER BY`'s order | `no plan: no index of <table> gives ORDER BY <columns>` |
| 4 | Every path in that order puts its NULLs where PostgreSQL doesn't (*NULLs in an ordered part*) | `no plan: ORDER BY <column> would need its NULLs reordered` |
| 5 | Every plan leaves a filter on a DOUBLE or REAL column | `no plan: a filter on column "<column>", a REAL or DOUBLE PRECISION, is not supported yet` |

Where plans fail for different reasons, the refusal is the first one of
the furthest stage a plan reached: its tables (rows 1 and 2), then the
order (3), its NULLs (4), and its filters (5). Each failure is recorded
where it happens, with the plan it stops: an access path, or a step that
adds a table to a set. A plan that fails is carried on as far as its tables
reach, at the earliest stage of its parts' failures, so a failure counts
only when its plan reaches every table: a scan of one table not in the
`ORDER BY`'s order is not the reason a join that never reaches the other
table is refused. When no plan reaches every table, row 2 names the first
table in `FROM` order that the largest set planned lacks, and the key
columns no equality names.

The refusal is a `FeatureNotSupported`, SQLSTATE 0A000, whose message
starts `no plan:`, raised after binding and before anything runs. It
carries no position: the search works on the logical plan, which keeps
none. A logical plan the search can't use, which today's binder never
builds, is refused the same way, never with another exception and never
with uncompilable C:
- an operator where the search plans another (`no plan: a Filter over a
  Filter where the search plans a table`, `an Empty where …`);
- a conjunct `use_of` refuses (`no plan: a conjunct on <table> that does
  not compare one of its columns`, `… compares two columns outside a
  join`, `a join conjunct that does not compare a column of each side`,
  `a conjunct that reads no column`);
- a result, aggregate or `ORDER BY` column that is not a column of the
  statement's tables.

PostgreSQL runs each of these; it is the one entry of `frontend_design.md`'s
*Known differences* that is a feature gap.

### Tests

- **Alternatives agree.** `search_all` returns every plan the search
  costed, none pruned, cheapest first. `generate_alternatives` prints each,
  and a plan with choice points twice, its thresholds overridden
  (`physical::override_thresholds`): INT64_MAX takes every first plan, 0
  every second. `Database::alternatives_for_testing` (the test build) runs
  each on its own snapshot. Slice 4 forces each choice point on its own
  (`override_thresholds` takes one threshold per choice point), so a plan
  of k choice points is run once per combination of branches, 2^k times,
  k at most 4. The proof machine runs
  them after every `SELECT`, aggregate and join it compares with
  PostgreSQL, in autocommit, and checks each plan's rows as it checks the
  layer's. An embedded-API test does the same in C++. The machine
  generates the join where both ways apply: a table named by its key in
  parameters, the other scanned by parameters, and an equality of columns
  neither key takes linking them (`join_read_once`, its links on neither
  table's key); now and then a new table takes a column type another has,
  so joins find columns to compare. A fixed test runs both plans of a
  choice point, alone, read once and in an `UPDATE`
  (`test_both_plans_of_a_choice_point_agree`).
- **Choices** (`search_test`). Fixed counts choose the join order; a
  range's count chooses `COUNT` from the keys or by a loop; an `ORDER BY`
  keeps the plan in its order where another costs less, and a `LIMIT`
  cuts its cost; a point lookup beats a unique index's one entry, and a
  covering scan a fetching one; a table keyed by parameters alone is read
  once, before the other's scan, covering when nothing reads it, and the
  join's conjuncts filter it and its rows; a refusal names the failure of a
  plan of every table. Choice points: fixed counts give the threshold the
  plan prints, at a table alone and read once; none where one range lies
  inside the other, under a `LIMIT` its scan's first entry ends, or at a
  unique index or a point lookup.
- **`physical::check`** (`physical_test`): a nested loop's outer side is
  one row, its inner side a scan, or a choice of two, and an inner filter
  compares a column of the inner table, read from its row, so never over
  a covering scan, with no column but the outer side's; a choice point
  counts its first plan's range, and each plan is a scan of its table.
- **The search's model** in the proof machine (`served`, `join_served`)
  says whether some plan serves a statement, as the search now does.
- **The harness**, 2 warehouses, 4 threads, 30 s: 237,014 TPM, 106,092
  NOPM, and the consistency conditions hold after the run and in `check`.

## Slice 4 as built

Full scans and sorts, and #533's refusals lifted. Every TPROC-C golden plan
regenerated unchanged, so no TPROC-C statement's reads changed.

**Full scans.** A full scan of the primary key is an access path when
nothing bounds the table, costed at the table's rows; it is chosen when it
is cheapest or nothing else is usable. A full scan of an index is one only
when its order gives the `ORDER BY`, its NULLs placed as asked: the "index
that gives the order" of *Memory*. Each is budgeted as every read, guards
each row it returns under `FOR UPDATE`, and serves `UPDATE` and `DELETE`
too. The plan text is `bc_scan_open(cx, TBL_n, bc_empty, bc_empty, …)`.
No choice point by costs is at a full scan, whose count no parameter
changes, and none takes a full scan as its second plan. The choice by the
budget is at one, counting the table, and may take one.

**Sorts.** A plan whose rows are not in the `ORDER BY`'s order is wrapped in
a `Sort` over `bc_sorter_*`, and compared with the plans in the order.
- The sort key is built with `bc_sort_key_col` per part, in its direction
  and NULL placement. The dialect takes `NULLS FIRST` and `NULLS LAST` in
  `ORDER BY` (not in `CREATE INDEX`); without them, PostgreSQL's defaults:
  `ASC NULLS LAST`, `DESC NULLS FIRST`.
- The order includes NULL placement. An index part's key order puts NULLs
  first for an `ASC` part and last for a `DESC` one, the other way round in
  a reverse scan. A part whose requested placement is that key order needs
  nothing. The two-range form gives the other placement for the first
  ordered part after the prefix, an `ASC` one. Every other placement is
  sorted.
- `ORDER BY … LIMIT n` opens the sorter with `keep = n`, and its loop keeps
  the `LIMIT`'s counter. `LIMIT 0` needs no order and sorts nothing.
- A sorter holds one view per row, so a sorted plan reads one table in a
  loop, the `Sort`'s payload: the sorter holds that row, as `bc_pin` would,
  and never a covering scan's entry, so the loop's scan reads rows. Rows of
  tables read once before the loop stay in hand. A table looked up by key
  after it is looked up again above the `Sort`, from the row held: the same
  key on the same snapshot, so the same row, its filters held already. A
  plan of two loops, one inside the other, cannot be sorted, and is refused
  when a sort is all that would give its order. Hash joins lifted that
  refusal with a sorter per loop (*Hash joins as built*, *Order*).
- Codegen prints the pipeline breaker as *The physical plan and codegen*
  shows: the input's loop builds each row's sort key and calls
  `bc_sorter_add`; the sort's produce is `while (bc_sorter_next(cx, s0,
  &p0))`, the consumer inside.

```c
bc_sorter s0 = bc_sorter_open(cx, 10);
/* the input's loop */
  bc_sort_key sk = bc_sort_key_begin(cx);
  bc_sort_key_col(cx, sk, row, COL_1_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
  bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);
bc_view p0 = bc_empty(cx);
while (n < 10 && bc_sorter_next(cx, s0, &p0)) {
  /* lookups by key again, from p0; emit */
}
```

**Joins.** A table no key reaches from the other's rows is read by its own
scan, the join's conjuncts filtering its rows: once under at most one outer
row, as before, and now also once per outer row, a nested loop of two
scans whose inner cursor is `c<d>`. That plan costs the outer plan's plus
the outer rows times the inner scan's, and it is refused where a hash join
(slice 5) would cost less: the inner scan once, plus the smaller side's
rows at the sort's weight, plus the larger side's probes. So it is planned
only over an outer side of a row or two. A refusal names the table and the
hash join. No choice point counts a table read once per outer row. The
refusal is gone: the search costs a hash join beside it (*Hash joins as
built*).

**The weight, measured** (*Decisions*, 9). On the harness's two-warehouse
load, `order_line` by `(ol_w_id, ol_d_id)`, 20 ranges of about 30,000 rows,
600,251 in all, each statement run seven times, median per run:
- one unit, a cursor step and its row read, as `SELECT sum(ol_quantity)`
  spends it: 700 to 750 ns;
- a row through a sort, `ORDER BY ol_amount, ol_o_id` against the same
  `SELECT` unsorted, both emitting every row: 920 to 1,100 ns;
- `ORDER BY ol_dist_info DESC`, a `VARCHAR(24)` key: 1,070 to 1,150 ns.

That is 1.23 to 1.58 units per row held, its `bc_sorter_next` step
included: a weight of 0.23 to 0.58 beyond it, over three runs. The search
takes 0.5 (`kSortWeight`). A sort costs its input's cost, unreduced by a
`LIMIT`, plus each row it holds × (1 + 0.5), plus one per lookup by key
repeated above it.

**The planner uses the budget.** The search is given the statement's
memory budget (`bc_budget::max_bytes`, `Database`'s statement memory), and
builds *Memory*'s rule so:
- A sorter of n entries holds at least `kSorterBytes` + n ×
  `kSortEntryBytes` (192 and 128 bytes), so the least count past the
  budget is (`max_bytes` − 192) / 128 + 1; 0 for a budget under 192.
- A sorted plan of one table, where another plan of it gives the `ORDER
  BY`, gets a choice point by the budget, within the cap of 4: the count of
  the sorted range, a full scan's the whole table (`bc_count_range` over
  `bc_empty` and `bc_empty`), limited to that least count. Below it, the
  sort; from it, the cheapest plan that gives the order, which may be a
  full scan of an index, and may cover. The first plan's branch holds the
  `Sort`. Its filters only make the count an upper bound on the rows held.
- None under a `LIMIT` the budget holds, and none where nothing else gives
  the order: that sort runs, and past the budget fails with 54000.
- No count read at compile time decides it. Slice 4 first left out a sort
  of a whole table whose rows, counted at compile time, passed the budget,
  and only counted a range at run time where its count depended on `$n`:
  a plan compiled while its table was small then failed with 54000 on
  every run once the table had grown, though an index scan in the order
  would have run (#591's review).
- `SELECT k FROM w WHERE y >= $1 ORDER BY x`, an index on x, compiled at
  10 rows with a 100,000-byte budget: the full scan, filtered and sorted,
  costs 10 + 3.3 × 1.5, less than the index's 2 × 10, and the plan counts
  `w` against 780 when it runs.

**Interesting orders.** The search keeps the cheapest plan per set of
tables, per row count (at most one, or more) and per order (the `ORDER
BY`'s, or none). A plan in the order keeps its choice points among plans in
the order; a sorted one takes any range of its loop's table, under the
sort, which reads rows.

**Carried over from #582's review.**
1. `physical::check` compares a choice point's whole counted range with
   its first plan's: prefix parameters, bounds, direction, and a NULL
   split's whole prefix.
2. It refuses a `DOUBLE` read from a key wherever codegen reads one: a
   result column, an index join's inner key, a filter's operands, an
   aggregate's argument, `MIN` from the first entry.
3. Codegen builds a choice point's counted range once; its first plan's
   scan opens on it.
4. The proof machine's `join_read_once_on_keys` links the scanned table's
   primary-key columns, which an index holds as its key suffix, beside
   `join_read_once`.
5. `choose()` takes each table's access paths as an argument, from
   `candidates()`'s result.
6. `override_thresholds` takes a threshold per choice point; alternatives
   agree runs every combination.
7. `crossing()`'s assumption is stated above (*Choice points as built*),
   with its debug check.

**Carried over from #591's review.**
1. `physical::check` checks each combination of a plan's choice-point
   branches as the plan it runs: a covering scan in one branch no longer
   refuses a sort in the other, as the choice by the budget forms (a sort
   of `ix_a`'s range, or `ix_ab`'s covering scan in the order).
2. The choice by the budget counts a full scan's table, filtered or not,
   and no count read at compile time decides a sort (*The planner uses the
   budget*, above).
3. `crossing()` checks, in every build, that the first plan costs more from
   one count on, at the counts it samples; where two costs cross more than
   once it returns none, and the search keeps its compile-time choice.
4. The proof machine generates NaN, `Infinity` and `-Infinity` for DOUBLE
   and REAL, and `oracle.same_rows` compares sort keys as PostgreSQL orders
   them, every NaN equal to every other.
5. Where the statement sorts a plan not in its order, the search keeps and
   prunes each candidate at the cost of the plan it becomes: its loop's
   access uncovered, at 2 reads per entry of an index.
6. A plan that reaches every table and passes every filter but needs a
   hash join is refused at a stage past the filters, so the refusal names
   the hash join, not another plan's DOUBLE filter. Nothing produced the
   order stage, which is gone.
7. Codegen passes a choice point's counted range to the scan of its first
   plan by that scan, not to whichever scan opens next.
8. `logical::Expr` and `physical::KeyBound` have a defaulted `operator==`,
   which `check` uses.

### What the search refuses

| # | Refused | Message |
|---|---|---|
| 1 | Every plan leaves a filter on a DOUBLE or REAL column | `no plan: a filter on column "<column>", a REAL or DOUBLE PRECISION, is not supported yet` |
| 2 | A join that no key reaches either way, where every plan reading a table once per row of the other costs more than a hash join | `no plan: no key of <table> is reached from the other table's rows, and reading it once per row costs more than a hash join, which is not planned yet` |
| 3 | An `ORDER BY` only a sort gives, over plans of two loops | `no plan: ORDER BY <columns> over a nested loop of two scans: a sort holds one table's rows` |

Where plans fail for different reasons, the refusal is the first one of
the furthest stage a plan of every table reached: its tables, its filters
(row 1), then the hash join it lacks (row 2), which only a plan past every
filter can. Row 3 is the refusal when every complete plan needs it. Rows 2
and 3 changed with hash joins (*Hash joins as built*, *What the search
refuses*). A table, an `ORDER BY` no index gives,
an `ORDER BY` on a join and a NULL placement no index gives are no longer
refused. The shapes the search does not plan are refused as before (*What
the search refuses*, slice 2).

### Tests

- **Golden plans.** `choice_pk_or_index`, `choice_read_once` and
  `choice_update` moved: the counted range is built once and the first
  branch opens on it. The count's two reads and the scan's are the same; two
  key builds fewer. New: `full_scan`, `full_scan_count`,
  `full_scan_for_update`, `update_full_scan`, `delete_full_scan`,
  `order_full_index` (a full index scan in the order, its NULLs by two
  ranges), `order_read_once`, `sort_parts`, `sort_top_n`,
  `sort_full_scan`, `sort_double`, `sort_join` (the lookup repeated above
  the sort) and `sort_read_once`.
- **Search** (`search_test`): a sorted plan beside the one in the order, by
  cost and under `LIMIT`; full scans; a table read once per outer row
  against a hash join, and its refusal, past another plan's DOUBLE filter;
  the budget by a choice point, a range's and a full scan's compiled while
  the table was small, its counted range opening only its first plan's
  scan; a choice between a sort and a covering scan in the order, which
  `check` passes; an unordered plan of a join kept at its cost uncovered;
  `crossing()` where two costs cross more than once.
- **The budget at run time** (`api_transaction_test`): a sort compiled at
  10 rows under a 64 KiB budget, run at 1,000, takes the index in its
  order by the count, with no recompile.
- **`physical::check`** (`physical_test`): sorts, full scans, the counted
  range and `DOUBLE` reads from a key; a sort in one branch of a choice
  point beside a covering scan in the other.
- **The allocation-fault sweep** fails each allocation of four compiled
  sorted plans in turn (`embedded API: an allocation failure in a sorted
  plan is BC_E_MEMORY or its rows`).
- **The proof machine** generates full scans, `UPDATE` and `DELETE` over
  them, `ORDER BY` on any columns, several parts, mixed directions, `NULLS
  FIRST`/`LAST` and `LIMIT`, and `ORDER BY` on joins. Its `served` model is
  down to the `DOUBLE` filter, and `join_served` says "either" where the
  cost model decides between a nested loop and a refusal.

## Hash joins as built

The first part of slice 5: hash joins (#534). Every golden plan,
TPROC-C's and the fixture's, regenerated unchanged, so no statement planned
before changed its reads.

**Where.** A hash join is a candidate where the table a plan adds is joined
to it by an equality between the sides and no key reaches it from the other
rows, so the index nested loop can't read it. It is not one under an outer
side of at most one row: reading the table once there costs less. The
inner side is one table's access path from parameters alone, read once:
the tree stays left-deep. The join's equalities key the hash table; its
other conjuncts, which the binder does not take yet, would filter the
pairs. The nested loop that reads the table once per outer row is a
candidate beside it, no longer refused (slice 4's row 2), and the costs
pick.

**The build side.** Either side is costed as the one the hash table holds:
the inner table, or the outer one when the plan so far is that table's
access alone. A row held costs more than a row probed, so where both serve,
the cheaper plan builds on the smaller side by the cost model's estimate.
Where an `ORDER BY` of one side's columns sorts that side before it
probes (*Order*, below), the other side is built, larger or not. The plan text names the side:
`bc_hash h0 = bc_hash_open(cx);  // built on TBL_1's rows`.

**The key.** Each row's key is a sort key (`plan_api_design.md`, *Sort
keys*) of its join columns' values in order, each `bc_sort_key_<type>` of
the value read, ascending. A join compares columns of one type (the
binder), so equal SQL values are equal physical values, and a part of one
physical type gives equal values equal bytes, read from a row or from a
covering scan's entry. A row with a NULL in any join column is neither put
nor probed: its loop goes to the next row, since `=` never holds for NULL.
A probe side sorted before it probes drops such a row before it is added
to the sorter, so it is neither copied nor charged.
A join column of type DOUBLE PRECISION or REAL is refused, as a nested
loop's is (row 1).

**Codegen** prints it as a pipeline breaker. The build side's loop, in a
block of its own, builds each row's key and puts the row. Then the probe
side's loop builds its key, finds, and loops over the rows held under it,
the held one bound as `b<i>`. A `LIMIT`'s counter is the probe loop's and
the hash loop's, never the build's.

```c
bc_hash h0 = bc_hash_open(cx);  // built on TBL_1's rows
{
  /* the build side's loop */
    if (bc_col_null(cx, row, COL_1_2)) continue;
    bc_sort_key hk0 = bc_sort_key_begin(cx);
    bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_1_2), BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    bc_hash_put(cx, h0, bc_sort_key_end(cx, hk0), row);
}
/* the probe side's loop */
  bc_sort_key hk0 = bc_sort_key_begin(cx);
  bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_4_3), BC_SORT_ASC, BC_SORT_NULLS_FIRST);
  bc_hash_iter q0 = bc_hash_find(cx, h0, bc_sort_key_end(cx, hk0));
  bc_view b0 = bc_empty(cx);
  while (bc_hash_next(cx, q0, &b0)) {
    /* the consumer */
  }
```

**Order.** A hash join's rows come in its probe side's order, each probe
row's pairs in the order their rows were put. So it gives the `ORDER BY`
where its probe side's access does. Otherwise, where every `ORDER BY`
column, but those the `WHERE` makes constant, is the probe side's table's,
its probe side can be sorted before it probes, every row kept, since the
join may drop any of them, and the `LIMIT` counted above. That is decided
before any loop is made to read rows: the probe side's sorter holds its
row, read for the hash keys above it, so its loop reads rows, and nothing
else does for it.

Any plan can instead be sorted above it, a hash join's or a nested loop
of two scans', whatever the `ORDER BY`'s columns. A sorter holds one view
per entry, so there is a sorter per loop of the plan whose row is read
above the sort, by the result or by a lookup by key repeated there, each
holding that loop's table's row, all fed the same sort key, built once per
row; a hash join's loops are its probe side's and the one over the rows it
holds. A sorter is stable and a top-n sorter keeps the earliest of equal
keys (`plan_api_design.md`, *Sort keys*), so sorters fed the same keys in
the same order hold the rows of the same pairs, in the same order, and
under `LIMIT n` all keep n. The sorter loop steps the first with
`bc_sorter_next` and reads each other at the same place with
`bc_sorter_nth`. Tables looked up by key after the first loop are looked
up again above the sort, as for one loop.

A loop whose row nothing above the sort reads has no sorter: its row is
read only below it, for the sort key, its filters and the join, and its
access is free to cover. Each `ORDER BY` column is then among the columns a
covering scan's index must hold, and a sort key part read from its entry is
`bc_sort_key_<type>` of `bc_key_part_<type>`, `bc_sort_key_null` where the
entry's part is NULL; never a `DOUBLE`, which the key does not hold
exactly. Where no loop's row is read above it, one sorter holds the keys
alone, its payload `bc_empty`, and the rows read once before the first loop
stay in hand. A choice point's second plan at a loop a sorter holds reads
rows, as its first does, and at a loop none holds may cover. With the two
tables the binder takes, a choice point is only ever at the loop that
drives the plan: a table read once per outer row takes none.

```c
/* SELECT t.id, p.a … ORDER BY p.v: both loops' rows read above the sort */
bc_sorter s0 = bc_sorter_open(cx, 0);
bc_sorter s1 = bc_sorter_open(cx, 0);
/* the outer loop, then the inner one */
    bc_sort_key sk = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, sk, row1, COL_4_2, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_view skv = bc_sort_key_end(cx, sk);
    bc_sorter_add(cx, s0, skv, row);
    bc_sorter_add(cx, s1, skv, row1);
bc_view p0 = bc_empty(cx);
int64_t r1 = 0;
while (bc_sorter_next(cx, s0, &p0)) {
  bc_view p1 = bc_sorter_nth(cx, s1, r1);
  ++r1;
  /* lookups by key again; emit */
}
```

Under `LIMIT 0` no plan reads anything: codegen prints an empty plan, the
computed `$n ± k` aside, as it already printed nothing for an aggregate
under `LIMIT 0`. Before, a hash join under `LIMIT 0` ran its whole build.

**Cost.** The build side's cost and its rows held, each at 1 + 0.3
(`kHashWeight`); the probe side's cost; and one per pair, a step of
`bc_hash_next`. The pairs are the two sides' rows × 1/10 per key equality,
as a nested loop counts its join conjuncts. A sorted probe side adds its
rows × (1 + 0.5), the sort's. A sort above a plan of several loops costs
the sort's per pair held in its first sorter, and 0.5 per pair in each
further one, the weight of its copy: `bc_sorter_nth` steps no cursor. Each
loop a sorter holds is costed reading rows, at 2 reads per entry of an
index, and each other one at its access's cost, covering or not; a probe
side sorted before it probes, at its own loop's rows read. The search keeps
and prunes each candidate at that cost, so it compares plans at the cost
they run at. Under `LIMIT n` the build, and a sorted
probe side, are not cut: the plan costs them, plus the rest × n / the
rows. No choice point by costs is at either side of a hash join.

**The weight, measured** (*Decisions*, 9). On the harness's two-warehouse
load, `order_line` by `(ol_w_id, ol_d_id)`, 20 ranges of 30,013 rows on
average, each statement run seven times over all 20, median per run, six
runs:
- one unit, a cursor step and its row read, as `SELECT sum(ol_quantity)`
  spends it: 730 to 830 ns;
- a row held: the same range built, probed by `stock`'s two rows of
  `s_i_id < 2` on `ol_i_id = s_i_id`, against the `SELECT sum`: 860 to
  1,090 ns beyond it, 1.09 to 1.46 units, median 1.26;
- a row probed, for comparison: `district`'s ten rows of a warehouse
  built, probed by the range on `ol_o_id = d_next_o_id`, which no row
  matches: 130 to 270 ns beyond the scan, 0.16 to 0.36 units.

That is a weight of 0.09 to 0.46 beyond the one unit per row held; the
search takes 0.3 (`kHashWeight`). The cost counts a pair, not a probe: a
probe with no pair, its key built and found, costs about a quarter of the
unit that already counts the probe side's row. Leaving it out favours a
hash join over a nested loop by that much per probe row.

**The planner uses the budget** (*Memory*). A hash table of n rows holds
`kHashBytes` (192 bytes), and per row `kHashEntryBytes` (240) with the
row, and each distinct key's bytes once (`bc_hash_put`). A row holds at
least its header, bitmap and fixed columns, in the smallest layout its
table's rows may have, and a key at least a marker and the value of each
part: 8 bytes for an integer, 1 for a BOOLEAN, 2 for an empty byte string.
The threshold is (`max_bytes` − 192) / (240 + row + key) + 1, by the
formula the sorter's uses (`least_past`): a build under it fits whenever
its rows and keys are no larger than those bounds. It charges a key's
bytes per row, as if no two rows shared a key, so it is conservative, not
the least count past the budget: where rows share keys, it switches to the
other plan at fewer rows than would fail, never at more. Counting the entries alone, as this slice
first did, let a build under the threshold fail with 54000 where the
other plan would have answered.
- Where another plan of the statement avoids the hash table, the hash join
  is the first plan of a choice point by the budget: the count of its build
  side's range, a full scan's the whole table, limited to that threshold.
  Below it, the hash join; from it, the cheapest plan without one, which
  for the joins planned today is a nested loop reading a table once per row
  of the other, sorted where the statement's order needs it.
  The choice point is over the whole join, and checked as reading the same
  tables in both branches.
- A build known not to fit, a budget under 192 bytes, or a range of at
  most one row, a unique index's whole key, where one row may not fit, is
  left out where another plan avoids it. One known to fit gets no choice
  point.
- Where nothing avoids it, the hash join runs, and past the budget fails
  with 54000.
- A sorted probe side, or a sort above the join, is not a choice by the
  budget: past it, it fails with 54000.
- The cheapest plan without a hash join is found once per statement, and
  only where some plan has a hash join, and is the second plan of every
  hash join's choice point.
- `SELECT u.k FROM t, u WHERE t.name = $1 AND u.v = t.qty`, t at 40 rows
  and u at 100, under a 10,000-byte budget: the hash join built on t's
  range costs 8 + 4 × 1.3 + 100 + 40. A row of t holds at least 36 bytes
  (12 of header, 8 of bitmap, two fixed columns) and a key 9, so the plan
  counts that range against 35 when it runs; under 35 it hashes, from 35 it
  reads u once per row of t. Where t's rows share a `qty`, 35 rows hold
  fewer bytes than that, and the switch comes early.

### What the search refuses

| # | Refused | Message |
|---|---|---|
| 1 | Every plan leaves a filter on a DOUBLE or REAL column | `no plan: a filter on column "<column>", a REAL or DOUBLE PRECISION, is not supported yet` |

Slice 4's refusal of a join no key reaches, where reading a table once per
row of the other would cost more than a hash join, is gone, and with it
the stage it was refused at. So is its row 3, an `ORDER BY` only a sort
gives over plans of two loops, of one table's columns or both tables':
a sorter per loop sorts any plan (*Order*). This slice first refused it
when the columns were of both tables, and for a nested loop of two scans
even of one table's, as in a join with no equality between the sides
whose tables no key reaches.

### Tests

- **Golden plans.** None moved. New: `hash_join` (built on the outer
  table's range, a full scan probing it), `hash_join_two_keys` (two
  equalities, one column of the build side in both, its NULL test once),
  `hash_join_limit` (the counter on the probe and hash loops, not the
  build's), `hash_join_sorted` (the probe side sorted, `keep` 0, before it
  probes), `hash_join_distinct` (`COUNT(DISTINCT)` of the probe side's
  column) and `nested_loop_limit` (under `LIMIT 10` the nested loop
  reading the inner table once per outer row costs less than the hash
  join's build, which a `LIMIT` does not cut). Then, with the review's
  fixes: `sort_cross_join` (a join with no equality between the sides,
  a nested loop of two scans, sorted by one table's column: a sorter per
  loop, read in step), `hash_join_sorted_both` (sorted above the join by
  both tables' columns, `keep` 5 on both sorters),
  `hash_join_sorted_null_key` (a nullable probe key, its NULLs dropped
  before the sort and not tested again after it) and `hash_join_limit_zero`
  (an empty plan). None of the others moved. With the second review's
  fixes, `sort_cross_join` moved: only t's `id` is read above the sort, so
  it has one sorter, of t's rows, and no `bc_sorter_nth`; p's loop still
  reads its rows, since `p_w` holds no `v`, the sort key. Its reads are the
  same, and each pair holds one row in a sorter, not two. No other plan
  moved. New: `sort_keys_alone` (t by its key, p's loop sorted by p's
  column, nothing of p read above the sort: one sorter of the keys alone,
  `bc_empty` its payload, t's row in hand above it).
- **Search** (`search_test`): fixed counts choose a hash join over the
  nested loop and the other way round; the build side follows the counts,
  and under a `LIMIT`; an `ORDER BY` of one side's columns probes that
  side, sorted or in its order; a build that may pass the budget is a
  choice point counting its range at a threshold that counts each row's and
  key's least bytes, one known not to fit is left out; a nested loop of two
  scans and a hash join sorted by one table's columns or both, a sorter per
  loop, under a `LIMIT` both keeping n; under `LIMIT 0` no plan reads
  anything. Then, with the second review's fixes: a choice point at a loop
  a sorter holds reads rows in both plans, though `p_w` covers, and every
  plan costed passes `check` in every combination of its branches; not read
  above the sort, the same loop covers and builds the sort key from its
  entry; a nested loop of two scans sorted by p's column, only t's read
  above it, has one sorter and p's covering index, a nullable key part
  read from the entry, at one sorter's cost; a hash join sorting its probe
  side holds its rows though nothing above reads one, and is costed so;
  two keys on one probe column test it for NULL once before the sort.
- **The budget at run time** (`api_transaction_test`): a hash join compiled
  at 20 rows of its build side under a 64 KiB budget, its rows at least 92
  bytes, counts its range against 192, not the 273 of its entries alone; at
  230 rows it reads the other table once per row and answers, and the hash
  join's branch, forced, fails with 54000.
- **`physical::check`** (`physical_test`): the build side is one loop over
  the table held, never a covering scan's entry; each key is a column of
  the held table and one of the probe side, of one type, never DOUBLE; a
  choice point at a hash join counts its build side's range, and its other
  plan reads the same tables, a table looked up again above a sort counted
  once; a sort has a sorter per loop of its input read above it, outer
  first, each over a row read, and tests for NULL only columns of its
  input's tables; a column read above it, by the result or a lookup by key,
  is never of a loop no sorter holds; a sort key part read from a covering
  scan's entry is never a `DOUBLE`.
- **The allocation-fault sweep** fails each allocation of four compiled
  hash join plans in turn (`embedded API: an allocation failure in a hash
  join plan is BC_E_MEMORY or its rows`): alone, sorted on its probe side,
  under `COUNT(DISTINCT)` and under a `LIMIT`; and one sorted above the
  join by both tables' columns, and a join of two scans sorted (`an
  allocation failure in a sorted plan`), each a sorter per loop.
- **The proof machine** generates joins no key reaches, linked by up to
  two equalities of any columns of one type, key columns or not, or by
  none, each table scanned by parameters or in full (`join_no_key`), and
  inserts rows that
  take another table's join value (`insert_a_match`), so join columns hold
  NULLs and duplicates on both sides; with targets, `ORDER BY` of either
  table's columns or both, `LIMIT` and `COUNT(DISTINCT)`. Alternatives
  agree runs every costed plan, both build sides and the nested loop among
  them, and every combination of choice-point branches. `join_served` no
  longer accepts a refusal for the hash join or a sort: what it does not
  serve is the `DOUBLE` filter. `test_hash_joins` runs the fixed cases,
  joins with no equality and `ORDER BY`s of both tables' columns among
  them.
- **The harness**, 2 warehouses, 4 threads, 30 s: 166,898 TPM, 74,620
  NOPM, and the consistency conditions hold after the run and in `check`.
  Its statements' plans are unchanged; the machine is a 4-core container,
  not slice 2's. After the review's fixes, the same run: 184,190 TPM, 82,280 NOPM,
  the conditions holding after it and in `check`; no plan of its
  statements moved. After the second review's fixes: 149,504 TPM, 66,800
  NOPM, the conditions holding after it and in `check`; again no plan of
  its statements moved, so the difference is the container's, not the
  plans'.

## GROUP BY as built

The second part of slice 5 (#534): `GROUP BY` of columns, `HAVING`, `MAX`
and `AVG`, and several aggregates in one select list. Every golden plan,
TPROC-C's and the fixture's, regenerated unchanged.

**The logical plan.** An aggregated `SELECT` is `Limit? → Sort? → Filter?
→ Aggregate → Filter? → the tables`: the `Filter` above the `Aggregate` is
its `HAVING`, and the `Sort` orders the groups. The `Aggregate`'s outputs
are the select list, grouped columns and aggregate calls in its order; its
`group_by` holds each column of the `GROUP BY` once, as written. A column
not grouped whose table's whole primary key is, which every row of a group
holds the same value of, is in `dependent` (`frontend_design.md`, *GROUP
BY and HAVING*): a group gives it, and it never keys one.
Slice 1's one-row aggregate (one `COUNT`, `SUM` or `MIN`, no `GROUP BY`
or `HAVING`) keeps its plans: `CountRange`, `MinFirstEntry` and the loop
with an accumulator. Everything else is a `GroupAggregate`.

**The physical plan.** `GroupAggregate(input, keys, aggs, streamed,
dependent)`, its rows the groups; above it a `Filter` of its `HAVING`, a
`Sort` of its groups (`groups`: it holds each group's key), a `Limit`, and
the sink, `GroupEmit`, whose outputs are its keys, dependent columns and
aggregates. Its columns are its keys, then its dependent columns: a
`HAVING`, a sort and the outputs may name any of them, and each is kept as
below, but only the keys make the group's key or start a streamed group.

**In a structure** (`streamed` false), the pipeline breaker of *The
physical plan and codegen*: each input row's group is found by its key, a
sort key of its key columns (`bc_sort_key_col`, ascending, NULLs first, so
equal values, `-0.0` and `0.0`, and NULLs give equal bytes), and its values
folded into the group's slots; then a loop over the groups in the order
they first appeared. Where it keeps what it gives for a group
(`GroupLayout`):
- slot 0 counts the group's rows, `COUNT(*)` among them; on the group's
  first row, `bc_agg_null` of slot 0 is true;
- a column of physical type I64, a key or dependent, has a slot, its value
  folded once, on the group's first row, by `bc_agg_min_i64`, NULL as
  `bc_agg_null`;
- any other (BOOLEAN, DOUBLE, a string) is read from the group's first
  row of its table, put on that row in a hash table of the table's
  (`bc_hash_*`) under the group's key: the plan API has no slot for it,
  and a group key's bytes decode as no table's. So that table's loop reads
  rows, never a covering scan's entries, and a choice point's second plan
  there reads rows too;
- each aggregate a slot: `COUNT` by `bc_agg_count`, `SUM` by
  `bc_agg_add_i64`, `MIN` and `MAX` by `bc_agg_min_i64` and
  `bc_agg_max_i64`, `AVG` two, its sum and its count; NULL inputs skipped,
  and `SUM`, `MIN`, `MAX` and `AVG` NULL where `bc_agg_null`;
- a `COUNT(DISTINCT col)` keeps an aggregate of its own, one slot, keyed by
  the group's key and the value: a pair new to it counts in the group.

```c
bc_agg a0 = bc_agg_open(cx, 3);
{
  /* the input's loop */
    bc_sort_key gb0 = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, gb0, row, COL_1_2, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    bc_view gr0 = bc_sort_key_end(cx, gb0);
    bc_group gg0 = bc_agg_group(cx, a0, gr0);
    if (bc_agg_null(cx, gg0, 0)) {
      if (!bc_col_null(cx, row, COL_1_2)) bc_agg_min_i64(cx, gg0, 1, bc_col_i64(cx, row, COL_1_2));
    }
    bc_agg_count(cx, gg0, 0);
    bc_agg_max_i64(cx, gg0, 2, bc_col_i64(cx, row, COL_1_0));
}
bc_view gk0 = bc_empty(cx);
while (bc_agg_next(cx, a0, &gk0, 0)) {
  bc_group g0 = bc_agg_group(cx, a0, gk0);
  /* HAVING, then the consumer: emit, or a sort of the groups */
}
```

A group's handle is found again by its key (`bc_agg_group` finds an
existing group and charges nothing), since a plan's locals are initialized
where declared and no call gives an empty group. A `Sort` of the groups
adds each one's key as its payload, and its loop finds the group again by
the key it holds. A `LIMIT`'s counter is the loop over the groups or the
sorter, never the input's.

**Streamed** (`streamed` true), where the input's rows come with each
group's together: the group is kept in locals, a key's value and NULL and
each aggregate's accumulator, and given to the consumer inside the input's
loop when a row of another group starts the next, then after the loop for
the last. A dependent column is read from each row and kept beside the
keys. Nothing is held but the group in hand's `COUNT(DISTINCT)` values:
each argument's aggregate is keyed by the value alone and cleared
(`bc_agg_clear`, `plan_api_design.md`) as each group ends, so a stream's
memory does not grow with its groups. Before, it was keyed by every
(group, value) pair and held them all to the end. With no key it is one
group, given after the input, over no rows too, and its no-rows form (a
NULL parameter, a point lookup that misses) is the row of no rows under its
`HAVING`. A `HAVING` is an `if` block in either form, since a streamed
group is given inside its input's loop. Its columns are I64 and BOOLEAN,
the types a local holds, read from a row or from a covering scan's entry,
as `bc_key_part_i64` or `bc_key_part_bool`: a stream holds no row, so it
may cover where a structure may not.

```c
bool st0 = false;
int64_t kv0_0 = 0;
int64_t x0_0 = 0;
bc_agg d0_0 = bc_agg_open(cx, 1);  // COUNT(DISTINCT) values seen per group
/* the input's loop, in a's order */
    int64_t rk0_0 = bc_col_i64(cx, row, COL_4_0);
    if (st0 && (rk0_0 != kv0_0)) {
      /* the group given */
      x0_0 = 0;
      bc_agg_clear(cx, d0_0);
    }
    kv0_0 = rk0_0;
    st0 = true;
    /* v's key alone, new to d0_0: ++x0_0 */
```

**Interesting orders.** A plan's rows come in groups (`Access::grouped`,
`Candidate::streams`) when its first table's scan has the `GROUP BY`'s
columns, but those its `WHERE` makes constant, as its parts after its
equality prefix, in any order and direction, NULLs split or not, and each
table after it is a lookup by key; or when it has at most one row. A full
scan of an index is built for that order too; with no `GROUP BY` there is
no such order, every plan gives the one group, and no full scan is built
for it. The search keeps the
cheapest plan per set of tables and per that property, as per the `ORDER
BY`'s order. A streamed plan gives its groups in its rows' order, and a
structure's groups come in the order of their first rows, which is also
its rows' order: so either gives an `ORDER BY` of grouped columns where its
rows come in that order, and a structure sorts its groups otherwise. A
streamed plan that does not give it is not built.

**Cost.**
- Groups: the rows × 1/10 per key the `WHERE` does not make constant, at
  least 1; 1 with none.
- A structure: its input's cost, each row held at 1 + 0.3 (`kHashWeight`,
  as a hash build), then a step per group; a `LIMIT` does not cut the
  structure.
- Streamed: its input's cost; under `LIMIT n` × n / the groups kept.
- A `HAVING` keeps the groups at a filter's selectivity per conjunct; a
  `COUNT(DISTINCT)` costs one per row; a sort of the groups, each at 1 +
  0.5.
- `SELECT a, count(*) FROM g GROUP BY a`, 1,000 rows, an index on a:
  streamed in its order, covering, 1,000; the structure over any scan
  costs more by 1,000 × 1.3 + 100.

Each plan is finished as an ungrouped one is (`finish_prepared`). Choice
points by costs are chosen as for any plan (*Choice points as built*), a
streamed plan's second plan also giving the groups. A hash join takes the
choice by the budget at its build, the cheapest plan without a hash join
(of rows read where the structure holds them) its second plan, and none by
costs at either side (*Hash joins as built*); one known not to fit is left
out where another plan avoids it. So `SELECT t0.c2, count(*) FROM t0 JOIN
t1 ON t0.c1 = t1.c1 GROUP BY t0.c2` under a small budget switches at the
count the join alone does. Before, a grouped plan took its choice points
by costs only, and a hash join under it ran past the budget.

Every choice point counts its first plan's range as one rule says
(`choice`, checked by `counts_first`): a NULL split counts its whole
prefix, with no prefix its whole index, as a `Range` of two open bounds,
since a scan of no prefix needs a range. A hash join built on a NULL split
of no prefix used to count its prefix with none, which `check` refused.

**The planner uses the budget** (*Memory*). The rows are at most the
count of the input's range, so where a streamed plan of the statement
avoids the structure, the structure over one table's range is the first
plan of a choice point by the budget: the count of that range, a full
scan's the whole table, limited to a count past which it may not fit.
Below it the structure; from it the cheapest streamed plan. The count is
`least_past` of what the structure holds, as a hash build's is: fixed,
`kAggBytes` and a byte per slot, `kHashBytes` per table whose rows its
groups hold, `kAggBytes` + 1 per `COUNT(DISTINCT)`, and `kSorterBytes`
where it sorts its groups; and per row, as if each started a group and
gave each `COUNT(DISTINCT)` a new value, what the plan API charges
(`bc_agg_group`, `bc_hash_put`, `bc_sorter_add`):
- the group: its key, `kGroupEntryBytes` and `kAggSlotBytes` per slot;
- per held table: its row and `kHashEntryBytes`, and the group's key,
  new to the hash table;
- per `COUNT(DISTINCT)`: a key of the group's key, as a byte-string part,
  and the value's part, `kGroupEntryBytes` and one slot;
- in a sort: the sort key, the group's key as payload, `kSortEntryBytes`.

A key part is at least its marker where it may be NULL, else the marker
and its least value (`least_part_bytes`: 8 bytes for an integer, 1 for a
BOOLEAN, 2 for an empty string), and a row at least `least_row_bytes`. So
a structure under the count fits whenever its rows and key parts are no
larger than those bounds. Counting each row a group and a new value is
conservative, not the least count past the budget: where rows share groups
or values, it switches to the stream at fewer rows than would fail, never
at more. Counting the fixed amounts alone, as this slice first did, let a
structure under the count fail with 54000 where the stream would have
answered. One known to pass the budget, or a budget under the empty
aggregate's, is left out there; where nothing streams, it runs, and past
the budget fails with 54000.
- `SELECT a, count(*) FROM g WHERE k < $1 GROUP BY a` under 10,000 bytes:
  322 fixed and 233 per row (a's key at least its NULL marker), so k's
  range counts against 42 when it runs.
- `SELECT a, count(DISTINCT v) FROM g WHERE k < $1 GROUP BY a`: 644 fixed,
  and per row 281 for the group and 195 for a value, against 20; the fixed
  amounts alone gave 34.

**AVG** is PostgreSQL's `numeric`: `numeric_div` of the sum by the count
at the scale `select_div_scale` chooses, 16 significant digits or more,
rounded half away from zero, which can pass 64 bits. So the plan emits the
sum and the count, two `I64` result columns for one SQL column
(`ResultColumn::quotient`), and the API divides them as PostgreSQL does
(`average_text`); a result row carries the value as its text. Its sum is
an integer sum, as `SUM`'s, with its 64-bit limit (*Known differences*).

### What the search refuses

| # | Refused | Message |
|---|---|---|
| 1 | A `HAVING` comparison on a grouped DOUBLE or REAL column | `no plan: a filter on column "<column>", a REAL or DOUBLE PRECISION, is not supported yet` |

Hash joins' refusals stand. A shape the search does not plan, which the
binder never builds (an output or `ORDER BY` key not grouped, a `HAVING`
that compares no key or aggregate), is refused as before (*What the
search refuses*, slice 2).

### Tests

- **Golden plans.** None moved. New: `group_by_hash` (a structure, a
  nullable key in a slot), `group_by_stream` (streamed in the primary
  key's order), `group_by_stream_limit` (a key the `WHERE` makes constant,
  and the `LIMIT` on the input loop and the last group), `group_by_held_row`
  (a BYTEA key read from the group's first row), `group_by_having`
  (`SUM`, `AVG`'s two columns, `HAVING count(*) > $2`), `group_by_sorted`
  (the groups sorted, `keep` 3), `group_by_distinct`, `group_by_join`
  (over a hash join), `group_by_double` and `aggregates_ungrouped` (one
  group over no rows under its `HAVING`).
- **Search** (`search_test`): fixed counts choose the stream in an index's
  order or the structure; an `ORDER BY` the stream's order gives, reversed
  or not, or a sort of the groups, and a `LIMIT` that makes the stream
  cheaper; a key the `WHERE` makes constant; the choice by the budget, its
  threshold, left out under the empty aggregate's bytes, and none where
  nothing streams.
- **`physical::check`** (`physical_test`): a structure needs a key, a
  streamed key a local's type, a key read from a held row never a covering
  scan's entry; a `HAVING`, a sort and the outputs name the groups' keys
  and aggregates.
- **The budget at run time** (`api_transaction_test`): compiled at 10 rows
  under 64 KiB, run at 2,000 groups, the count takes the stream; grouped by
  a column no index orders, 54000.
- **AVG**: `average_text` against PostgreSQL 16's digits, and through the
  API.
- **The allocation-fault sweep** fails each allocation of four grouped
  plans in turn (`embedded API: an allocation failure in a grouped plan is
  BC_E_MEMORY or its rows`): a structure with a nullable key and `AVG`, a
  held row with `COUNT(DISTINCT)`, `HAVING`, a sort and a `LIMIT`, one over
  a hash join, and a stream.
- **The proof machine** generates `GROUP BY` of one or two columns of any
  type, mostly a key path's parts so that plans stream, with every
  aggregate, `HAVING` of keys and aggregates, `ORDER BY` of the keys,
  NULLS FIRST or LAST, and `LIMIT`; no `GROUP BY` with several
  aggregates; and over joins (`group_by`, `group_by_join`). NULLs and
  duplicates in keys and arguments come from the inserts, empty inputs from
  the bounds. Alternatives agree runs every costed plan, both forms and
  both branches of a choice point. The server's text, `AVG`'s digits
  among it, must be PostgreSQL's. `test_group_by` runs the fixed cases.
  Its model `join_served` now counts a second link on a key part as a
  filter, which a DOUBLE refuses.
- **The harness**, 2 warehouses, 4 threads, 30 s: 116,556 to 124,372 TPM
  and 51,982 to 55,546 NOPM over three runs, and the consistency
  conditions hold after each run and in `check`; the branch before it gave
  124,922 TPM on the same container. Its statements' plans are unchanged.
- **With the review's fixes**, after #600's final form was merged in:
  - golden plans: `group_by_distinct` moved: its stream keys each value
    alone and clears its values' aggregate as each group ends
    (`bc_agg_clear`); its reads are the same. No other plan moved. New:
    `group_by_dependent` (grouped by the primary key, a dependent integer
    kept in locals, streamed in the key's order) and
    `group_by_dependent_held` (a dependent string read from the group's
    held row, the groups sorted by it, the group's key the primary key's
    alone);
  - search: a grouped query over a hash join takes the build's choice by
    the budget at the join's threshold, and leaves a build known not to
    fit out; a hash join built on a NULL split of no prefix counts the
    whole index, and every plan passes `check`; a dependent column streams
    by the primary key, and in a structure is a slot outside the group's
    key; a BOOLEAN key streams from a covering entry, and the structure
    over the same scan reads rows; with no `GROUP BY`, no index's full
    scan is built for the groups; a key the `WHERE` makes constant is not
    counted in the groups; a structure's threshold counts its keys', held
    rows' and distinct values' bytes;
  - `physical::check`: a stream may read a BOOLEAN key from a covering
    entry, a structure may not; a dependent column is never a key, never
    of a group of no key, and a sort, a `HAVING` and the outputs may name
    it;
  - plan API: `bc_agg_clear` drops every group, releases its bytes (held
    flat across 200 groups) and makes their handles stale, and is refused
    after `bc_agg_next`;
  - the budget at run time: a streamed `COUNT(DISTINCT)` of 2,000 groups
    runs under 64 KiB, which every group's values held would pass, and at
    200 rows the count, at 137 by the values' bytes, takes the stream where
    the fixed amounts' 232 would have taken the structure and failed;
  - the harness, 2 warehouses, 4 threads, 30 s: 147,774 TPM and 65,848
    NOPM, the consistency conditions holding after the run and in `check`;
    no plan of its statements moved.

## Proposed `CONTRACT.md` changes

- **Added:** a statement that holds more memory than its budget fails with
  54000 before anything is committed. The `Known differences` row says that
  PostgreSQL spills instead.
- **Widened, slice by slice:** the planner's refusals (0A000) shrink as
  full scans, sorts and joins land. Each PR names what it widened.
- **Unchanged:** plans stay in the subset, results and errors match
  PostgreSQL, and nothing waits.

## Decisions

The owner's answers in the planner discussion (2026-10-23):
1. A logical algebra after binding: the operators above.
2. A small fixed set of rewrites: constant folding, predicate pushdown,
   projection pruning.
3. System R-style bottom-up search with interesting orders.
4. Produce/consume code generation (Neumann, VLDB 2011).
5. Choosing at run time with `bc_count_range` for choices that depend on
   `$n`: one compiled plan per statement, with no re-planning.
6. One memory budget per statement, with per-row storage released per loop
   step, and an error past the budget rather than a spill.

The owner's answers to the open questions (2026-10-24):
7. **The default memory budget** is 64 MiB per statement, configurable per
   database.
8. **Past the budget, SQLSTATE 54000** (`program_limit_exceeded`): a limit
   the layer sets, not the machine running out of memory.
9. **The cost weights** (the per-row weight of a sort or hash build, and the
   fixed selectivities) are measured once on the harness and stated here,
   in the slice that adds the cost model: the selectivities in *Slice 2 as
   built*, a sort's 0.5 in *Slice 4 as built*, a hash build's 0.3 in *Hash
   joins as built*.
10. **No recompiling when counts age**, until a workload shows a plan going
    stale.
11. **At most 4 choice points per plan.**
