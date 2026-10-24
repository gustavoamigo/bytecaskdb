# The planner: a logical algebra, a search, and compiled pipelines

Status: **design, approved (owner, 2026-10-24); slice 1 built** (*Slice 1
as built*): the pipeline, at parity with the MVP's rule-based planner,
which it replaced. *Memory* is built too: the statement budget and the
lifetime rule (`plan_api_design.md`, revision 7), and #551 with it; slices
2 to 5 are not. The first of the post-MVP designs (#487, *Next*). The
parser, the plan API and TinyCC are unchanged; the binder's output changed.

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
| `Aggregate(input, group_by, aggs)` | `COUNT`, `COUNT(DISTINCT)`, `SUM`, `MIN`, `MAX`; no `group_by` means one row |
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
NULLs placed by its keys. `ORDER BY … LIMIT n` keeps only the first n.

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
  stated in this document.

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

It counts bytes requested, not allocator overhead, so the accounting is
deterministic and the allocation-fault sweep can test it.

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

**The planner uses the budget.** A sort or hash build whose known size is
past the budget isn't chosen when another plan avoids it, such as an index
that gives the order. A size that depends on `$n` falls under *Choosing at
run time*.

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
  - working `bc_sorter_*` and `bc_hash_*` calls, which `bc_plan.h`
    declares and which set `BC_E_HANDLE` today.

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
| `SET col = col + $1` | `Update::set` | `bc_row_from`, `bc_row_set_*` with the sum, `bc_update` |
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

`COUNT(DISTINCT)` uses the aggregate structure `bc_plan.h` declares
(`bc_agg_open` with no slots, `bc_agg_group`, `bc_agg_next`).

### The search's choices

The MVP planner's rules, which slice 1's search keeps: no cost model,
chosen so every TPROC-C statement gets the plan a person would write.

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
   remain. None remaining: refused (*What the search refuses*, row 5).
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

#### What the search refuses

The search refuses a statement inside the subset that it can't answer
from an index scan in PostgreSQL's order (#533). It checks in this order, and the first refusal is the
statement's only one:

| # | Refused | Message |
|---|---|---|
| 1 | A join with an `ORDER BY` | `no plan: ORDER BY on a join` |
| 2 | A join where neither table is reachable from parameters alone | `no plan: a full scan of <first table in FROM>` |
| 3 | A join whose inner table no key reaches from the outer row and the parameters | `no plan: no index of <inner table> joins on <columns>` |
| 4 | A single table with no usable path | `no plan: a full scan of <table>` |
| 5 | A single table whose usable paths don't give its `ORDER BY` (a point lookup on the whole primary key always does) | `no plan: no index of <table> gives ORDER BY <columns>` |
| 6 | A single table whose chosen path can't place the NULLs of its `ORDER BY` as PostgreSQL does (*NULLs in an ordered part*) | `no plan: ORDER BY <column> would need its NULLs reordered` |

The refusal is a `FeatureNotSupported`, SQLSTATE 0A000, whose message
starts `no plan:`, raised after binding and before anything runs. It
carries no position: the search works on the logical plan, which keeps
none. A logical plan of a shape the search does not plan, which today's
binder never builds, is refused the same way, never with another
exception: an operator where the search plans another (`no plan: a Join
where the search plans a table`), or a conjunct that does not compare one
of its table's columns (`no plan: a conjunct on <table> the search does not
plan yet`). PostgreSQL runs each of these; it is the one entry of
`frontend_design.md`'s *Known differences* that is a feature gap.

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
   in the slice that adds the cost model.
10. **No recompiling when counts age**, until a workload shows a plan going
    stale.
11. **At most 4 choice points per plan.**
