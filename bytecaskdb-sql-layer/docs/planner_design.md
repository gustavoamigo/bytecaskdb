# The planner: a logical algebra, a search, and compiled pipelines

Status: **design, approved (owner, 2026-10-24); the statement memory
budget of *Memory* is built (`plan_api_design.md`, revision 6), the
lifetime rule and the rest are not.** The first of
the post-MVP designs (#487, *Next*). It replaces the planner
`frontend_design.md` describes under *The current planner*. The parser,
the plan API and TinyCC are unchanged; the binder's output changes.

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

The bound tree has the same problem one level up: `bound::Select` is one
table with `col op $n` conjuncts, and `bound::Join` is exactly two.

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
`ir::check` does today. A failure is a compiler bug, never a plan.

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
- the result rows it buffers.

It counts bytes requested, not allocator overhead, so the accounting is
deterministic and the allocation-fault sweep can test it.

**Ownership decides what is held.**
- A view returned inside a loop lives until that loop's next step, unless
  it is pinned or added to a structure. A key built for one probe, an
  inner row and a group key all go when the outer row moves on. That is
  the fix for #551.
- What outlives a step: pins, the contents of blocking structures, and
  result rows.

How the API releases a step's storage (a mark on the arena per loop level,
or storage held with the cursor, as `bc_col_key`'s is today) is the
implementation PR's to choose. This rule is the contract. Streaming
operators therefore use memory bounded by the depth of the pipeline, not
by the number of rows, and only pipeline breakers and results grow.

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
