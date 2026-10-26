# The planner: a logical algebra, a search, and compiled pipelines

Status: **built** (design approved by the owner, 2026-10-24). Slice by
slice: the pipeline (#571); System R's search and the cost model (#574);
the memory budget and the lifetime rule, with #551 (PRs #570, #573, #578
and #584; `plan_api_design.md`, revision 7); the choice points (#582); the plan
API's sorters, hash tables, aggregate slots and sort keys (#577, #581);
full scans and sorts (#591, closing #533); hash joins (#600) and `GROUP
BY` (#609); then the closing PR, which fixed #609's last review findings
and wrote this document as built. The first of the post-MVP designs (#487,
*Next*). The parser, the plan API and TinyCC are unchanged by it; the
binder's output changed. The rest of the dialect is #534's (*Slices*).

## Why

The MVP's planner matched a bound statement against a fixed set of
physical shapes, each printed by one template, with no logical algebra, no
rewrite and no search. The shapes didn't compose: three tables needed a
new join shape, an `ORDER BY` an index couldn't give had nowhere to put a
sort (#533), a choice between two selective indexes was a rule a person
wrote, and a join held every inner row and key until the statement ended
(#551), because per-row storage had no owner.

## Standing rules

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

| Stage | Module |
|---|---|
| Binder | `sql/binder.cppm`: DDL to the catalog's statement values, DML to the logical plan |
| Logical plan | `plan/logical.cppm` |
| Rewrites | `plan/rewrite.cppm` |
| Search | `plan/search.cppm` |
| Physical plan and `check()` | `plan/physical.cppm` |
| Codegen | `plan/codegen.cppm` |

## The logical plan

A tree of relational operators over typed expressions. It says what to
compute, not how.

| Operator | Meaning |
|---|---|
| `Get(table)` | every row of a table |
| `OneRow` | one row of no columns: a `SELECT` without `FROM` |
| `Filter(input, predicate)` | the rows for which the predicate is true |
| `Project(input, exprs)` | one output row per input row, of these expressions |
| `Join(left, right, predicate)` | inner; outer joins with #534 |
| `Aggregate(input, group_by, aggs)` | `COUNT`, `COUNT(DISTINCT)`, `SUM`, `MIN`, `MAX`, `AVG`; no `group_by` means one row |
| `Sort(input, keys)` | each key with its direction and its NULL placement |
| `Limit(input, n)` | the first n rows |
| `Insert`, `Update`, `Delete` | writes, over the rows their input gives |

Each expression carries its SQL type, which the binder resolves. The
dialect's expressions are columns, parameters, integer constants, `+` and
`-`, comparisons, their conjunction and aggregate calls. A literal is a
parameter by then: the binder folds it and the API binds it after the
statement's own (`frontend_design.md`, *Literals*). A predicate (`And`,
`Or`, `Not` of comparisons, and `IsNull` of a column) is a conjunct of its
own: the search keeps a table's predicates apart from its comparisons
(`Rel::preds`), no access path takes one, and each is a filter after the
path's, refused (`no plan:`) where it compares a REAL or DOUBLE column, as a
comparison is. Codegen prints one as two C conditions, true and false, SQL's
three-valued logic with unknown neither; `*` and `/` come with the dialect
that accepts them (#534). The
binder's own rules are the frontend's: one SQL type per `$n`, and the
errors it raises.

**What the binder builds.**
- A `SELECT` is `Limit? → Project → Sort? → Filter? → the tables`, the
  tables a `Get`, or a `Join` of two `Get`s. A `Project`'s outputs are
  columns and constants' parameters. A `SELECT` without `FROM` is `Limit?
  → Project → OneRow`.
- An aggregated `SELECT` is `Limit? → Sort? → Filter? → Aggregate →
  Filter? → the tables`: the `Filter` above the `Aggregate` is its
  `HAVING`, and the `Sort` orders the groups. The `Aggregate`'s outputs are
  the select list, grouped columns and aggregate calls in its order; its
  `group_by` holds each column of the `GROUP BY` once, as written. A column
  not grouped whose table's whole primary key is grouped, which every row
  of a group holds the same value of, is in `dependent`
  (`frontend_design.md`, *GROUP BY and HAVING*): a group gives it, and it
  never keys one.
- Every `WHERE` conjunct, a join's `ON` among them, is in one `Filter`
  above the tables, in the order written. Pushdown moves it.
- `UPDATE` and `DELETE` are over `Filter? → Get`; `INSERT` is one row of
  parameters.
- A `Sort` key takes `NULLS FIRST` or `NULLS LAST` as written, else
  PostgreSQL's default for its direction: `ASC NULLS LAST`, `DESC NULLS
  FIRST`.
- `FOR UPDATE` is a flag on the `Get` it locks, carried to the physical
  access path, which guards each row it returns.

## Rewrites

A fixed list, applied in order, each to a fixpoint. None needs a cost.
1. **Constant folding.** An expression of constants becomes a constant; a
   conjunct that is always true is dropped. Two constants compare by their
   SQL types: exact numbers at the larger of their scales, a `BOOLEAN`,
   `DATE`, `TIMESTAMP` or `TIME` only with one of its own type, anything
   else not at all; two add only when both are in the result's
   representation. A conjunct always false replaces its `Filter`, and what
   it reads, with an `Empty` naming the tables it replaced; a `Join` with an
   `Empty` side is `Empty`. The search plans it as no rows, read from
   nothing. Today's dialect rarely gives it anything to fold: a `WHERE`
   compares a column with a parameter.
2. **Predicate pushdown.** A conjunct moves to the lowest operator whose
   columns it reads: onto the table it reads, or onto the `Join` when it
   compares the two sides. Two `Filter`s in a row become one, the lower
   one's conjuncts first, so a `Get` has at most one.
3. **Projection pruning.** Each operator keeps only the columns something
   above it reads. A read is covering when every column the operators above
   take is in the key; a column only a key bound reads need not be.

Subquery decorrelation joins the list when #534 brings subqueries. The two
rewrites of TPROC-C statements (`testing_design.md`, *The TPROC-C
statements*) are made by hand in the statement text.

## The search

System R-style bottom-up dynamic programming over access paths and join
orders (owner, 2026-10-23), on the plans' costs (*The cost model*).

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
- **A point lookup** when the whole primary key is compared for equality.
  It gives any order and any grouping: it returns at most one row.
- **A scan** of the primary key and of each index whose first part is
  compared: the equality prefix, then a range on the next part, forward or
  reverse. Of two bounds on one side, the first in `WHERE` order is the key
  bound and the other a residual. It covers when no filter reads its rows
  and its key holds every column read from them: result and aggregate
  columns, `ORDER BY` columns a sort key builds from the entry, and join
  columns that a key built from the row or a filter on the pair reads. A
  covered column is read from the entry's key wherever a row's would be.
  A scan of the whole primary key by equality is the point lookup, not
  built twice.
- **A full scan** of the primary key when nothing bounds the table, costed
  at the table's rows: ordinary costing, not a refusal (#533). A full scan
  of an index only where its order gives the `ORDER BY` (its NULLs placed
  as asked) or the `GROUP BY`'s groups. Each is budgeted as every read,
  guards each row under `FOR UPDATE`, and serves `UPDATE` and `DELETE`.
  The plan text is `bc_scan_open(cx, TBL_n, bc_empty, bc_empty, …)`.
- **Answered from the keys**, a plan beside the loop over the same scan,
  costs deciding: `COUNT(*)` or `COUNT` of a column that can't be NULL in
  the range, over a scan with no filter, is `bc_count_range`; `COUNT(*)`,
  `MIN` and `MAX` of one group are `KeyAggregates` (*Aggregates*).

**Joins.** Left-deep, bottom up over sets of tables: each set of k tables
from each set of k − 1 and one more table, read one of four ways:
- **by its whole primary key**, from parameters and the columns of the
  tables already read: an index nested loop (`IndexJoin`), one lookup per
  outer row. Only an equality keys it;
- **by its own scan, once**, from parameters alone, when the tables
  already read give at most one row (a point lookup, then lookups by key):
  a `NestedLoop` that reads it once, and not at all when that row is
  missing;
- **by its own scan, once per outer row**: a `NestedLoop` of two scans,
  whose inner cursor is `c<d>`. Costs make it the plan only over an outer
  side of a row or two, or where nothing else joins (no equality between
  the sides);
- **by a hash join** (*Hash joins*), where the table is joined by an
  equality between the sides, no key reaches it from the other rows, and
  the outer side is not at most one row.

The join's conjuncts between the new table and those already read filter
its rows, or the pairs, refused on a DOUBLE or REAL column alike. Bushy
trees and merge joins wait for a workload that needs them. Sets are bit
masks of up to 32 tables; the binder takes two.

The search keeps the cheapest plan per set of tables, per row count (at
most one, or more), per order (the `ORDER BY`'s, or none) and per grouping
(rows that come in the `GROUP BY`'s groups, or not), the first of equals,
so `FROM` order breaks ties. The whole statement's plans are all finished,
emitted under `LIMIT`, aggregated or sorted, before they are compared,
since a `LIMIT` or a sort changes which is cheaper. Where the statement
sorts a plan not in its order, the candidate is kept and pruned at the cost
of the plan it becomes: each loop a sorter holds reading rows, at 2 reads
per entry of an index.

**Interesting orders.** A plan whose output is ordered as a `Sort` above it
needs is kept even when it costs more, because the sort it saves may cost
more than the difference. That matters here more than usual: an index
order is often the cheapest way to answer `ORDER BY … LIMIT`.
- A plan's order is its first table's scan's; lookups by key keep it, and
  a hash join gives its probe side's (*Hash joins*). A point lookup gives
  any order.
- The order includes NULL placement (*NULLs in an ordered part*).
- A plan not in the order is sorted (*Sorts*).

**Groups.** A plan's rows come in groups (`Access::grouped`,
`Candidate::streams`) when its first table's scan has the `GROUP BY`'s
columns, but those its `WHERE` makes constant, as its parts after its
equality prefix, in any order and direction, NULLs split or not, and each
table after it is a lookup by key; or when it has at most one row. With no
`GROUP BY` there is no such order: every plan gives the one group, and no
full scan is built for it.

**Where filters run.** Each conjunct the key expresses becomes a bound.
Every other conjunct is a residual, run on the row as soon as its loop
reaches it, before anything is counted, emitted, guarded, held or written:
S2's `s_quantity < $4` runs on each `stock` row the join fetches.

**Writes inside a statement.** An `UPDATE` or `DELETE` of many rows writes
each row as its scan reaches it. The statement reads the transaction's view
as it stood at the statement's start (`transaction_design.md`,
*Statements inside a transaction*), so its own writes never feed back into
its scan.

### NULLs in an ordered part

The key codec sorts the NULL marker first (`types_design.md`), so an `ASC`
index part gives `NULLS FIRST` and a `DESC` part `NULLS LAST`, the other way
round in a reverse scan. PostgreSQL's defaults are the reverse. A part
whose requested placement is its key order needs nothing. A part the
`WHERE` compares, by equality or a range, holds no NULLs in the rows it
can match, and counts as `NOT NULL`. The other placement is given without
a sort only for one nullable part, the first ordered part after the
equality prefix, an `ASC` index part, with every later ordered part `NOT
NULL`: `NullSplitScan`, the present values and the NULL part as two
ranges in PostgreSQL's order, with one `LIMIT` across both. Every other
placement is sorted.

TPROC-C's ordered columns are `c_first` (P6, O2), `o_id` (O4) and
`no_o_id` (D1). `c_first` is nullable in the fixture, as in HammerDB's
MySQL schema, so P6's and O2's golden plans show the two-range form;
HammerDB's default PostgreSQL schema declares it `NOT NULL`, and the plan
doesn't rely on either. No TPROC-C index has a `DESC` part.

### The cost model

The unit is the one the work budget meters: record reads plus cursor steps
(`bc_budget::max_reads`). A plan's cost predicts what it will charge.

| Plan | Cost |
|---|---|
| A point lookup | 1 |
| A scan | its entries; twice that for an index scan that fetches each row |
| `COUNT` by `bc_count_range` | 2 (`kCountCost`) |
| `KeyAggregates` | `kCountCost` with a `COUNT(*)`, and 1 per `MIN` or `MAX`, 2 where the scan does not cover |
| A join by key | the outer plan's cost, plus 1 per outer row |
| A join read once | the outer plan's cost, plus the inner scan's |
| A nested loop of two scans | the outer plan's cost, plus the outer rows × the inner scan's |
| A hash join | the build side's cost and its rows held × (1 + 0.3), the probe side's cost, and 1 per pair (`bc_hash_next`) |
| A sort | its input's cost, unreduced by a `LIMIT`, plus each row held × (1 + 0.5), plus 1 per lookup by key repeated above it |
| A sort of several loops | 1 + 0.5 per pair in its first sorter, 0.5 per pair in each further one (`bc_sorter_nth` steps no cursor) |
| `COUNT(DISTINCT)` | its input's, plus 1 per row |
| A structure of groups | its input's cost, each row held × (1 + 0.3), then 1 per group |
| A streamed `GROUP BY` | its input's cost |
| A sort of the groups | each group × (1 + 0.5) |
| Under `LIMIT n` | the cost × n / the rows (or groups kept), when there are more than n; a hash build, a sorted probe side and a structure are not cut |
| A choice point | the plan's, as compiled, plus `kCountCost` for the count |

- **Entries.** A scan reaches the table's rows × 1/10 per equality-compared
  part × 1/3 per range bound (`kEqSelectivity`, `kRangeSelectivity`). It
  reaches at most 1 when the prefix covers a unique index's columns, and at
  least 1.
- **Rows.** A plan's entries, or 1 for a point lookup, × 1/10 per filter by
  `=` and 1/3 per other filter. A join's rows are the outer plan's × the
  inner side's (1 for a lookup by key), its join conjuncts counted as
  filters; a hash join's pairs are the two sides' rows × 1/10 per key
  equality.
- **Groups.** The rows × 1/10 per key the `WHERE` does not make constant,
  at least 1; 1 with no key. A `HAVING` keeps them at a filter's
  selectivity per conjunct.
- **A table's rows** are counted when the plan is compiled: `Database`
  counts each table's primary key on one snapshot, as `bc_count_range`
  does, at most two record reads each. Every `WHERE` operand in today's
  dialect is `$n` or `$n ± k`, so the whole table is the only constant
  bound; bounds that depend on `$n` are chosen at run time (*Choosing at
  run time*). A plan compiled with no database, as the golden plans are,
  costs a table at 1,000,000 rows (`kUncountedRows`), large enough that a
  key read beats a loop. Counts read at compile time age as tables grow:
  that's accepted (*Decisions*, 10), since the choices that matter in OLTP
  depend on `$n`.

**The constants, measured** (*Decisions*, 9), on the harness's
two-warehouse load.
- **Selectivities.** The fraction of a table's rows each TPROC-C key bound
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

  An equality keeps 0.087 per conjunct by the geometric mean, close to
  1/10; a range far less than 1/3 (0.081 per bound, 0.056 for the filter).
  The constants stay System R's 1/10 and 1/3: with the measured values in
  their place, no golden plan moves. A range's 1/3 is the number to revisit
  when a workload's choice turns on it.
- **A sort's weight, 0.5** (`kSortWeight`). `order_line` by `(ol_w_id,
  ol_d_id)`, 20 ranges of about 30,000 rows, each statement run seven
  times, median per run: one unit (a cursor step and its row read, as
  `SELECT sum(ol_quantity)` spends it) 700 to 750 ns; a row through a sort
  (`ORDER BY ol_amount, ol_o_id` against the same `SELECT` unsorted) 920 to
  1,100 ns; a `VARCHAR(24)` key, `ORDER BY ol_dist_info DESC`, 1,070 to
  1,150 ns. That is 1.23 to 1.58 units per row held, its `bc_sorter_next`
  step included: a weight of 0.23 to 0.58 beyond it.
- **A hash build's weight, 0.3** (`kHashWeight`). The same ranges, six
  runs: one unit 730 to 830 ns; a row held (the range built, probed by
  `stock`'s two rows of `s_i_id < 2` on `ol_i_id = s_i_id`) 1.09 to 1.46
  units beyond the `SELECT sum`, median 1.26; a row probed (`district`'s ten
  rows built, probed by the range on `ol_o_id = d_next_o_id`, which no row
  matches) 0.16 to 0.36 units. That is a weight of 0.09 to 0.46 beyond the
  one unit per row held. The cost counts a pair, not a probe: a probe with
  no pair costs about a quarter of the unit that already counts its row,
  and leaving it out favours a hash join over a nested loop by that much
  per probe row. A structure of groups takes the same weight.

### Choosing at run time

The owner's choice of three options (2026-10-23); the others were a generic
plan from heuristics, or a custom plan per execution as PostgreSQL makes
for a statement's first runs. A plan is a program, so it can contain two
alternatives and pick one with `bc_count_range` when it runs: one compiled
plan per statement, nothing re-planned, and nothing in the plan cache
depending on the parameters. A choice point is `Choice(counted, threshold,
below, at_or_above)` in the physical plan, chosen by `choose` in the
search. There are two kinds, by costs and by the memory budget (*Memory*),
at most 4 per plan (`kMaxChoices`, *Decisions*, 11); past that, the search
keeps its compile-time choice.

**By costs.** Every bound in today's dialect depends on `$n`, so every
scan's count is unknown at compile time.
- **Where.** At each table a plan reads by a scan whose bounds depend on
  `$n`: the first table's access, or a table read once under one row. Never
  at a point lookup, a lookup by key, a table read once per outer row, a
  scan whose bounds name at most one row (a unique index's whole key),
  either side of a hash join, or a full scan, whose count no parameter
  changes; and no full scan is a second plan. One per table.
- **The two plans.** The plan's access path is the first plan; the second
  is another range of the same table, from its access paths, the cheapest
  at the estimates of those whose costs cross the first one's. A plan in
  the `ORDER BY`'s order takes a second plan in the order; one streaming
  groups, a second that gives them too. In a sorted plan, the second plan of
  a loop a sorter holds reads rows, as its first does.
- **The threshold.** Each plan's cost is the whole statement's, as the cost
  model finishes it, with the first plan's count as the variable: the
  threshold is the least count, from 1 to the table's rows, from which the
  first plan costs more than the second. The two ranges' counts are related
  where their bounds are: a range whose bounds take only conjuncts the
  other's take holds the other's rows. Costs that only meet, cross only past
  the table's rows, or at a count of 1, get no choice point: under `LIMIT 1`
  a scan with no filter costs its first entry whatever its count.
  `crossing()` finds the threshold by binary search and checks, in every
  build, that the first plan costs more from one count on, at counts spread
  over the range (1, 2, 3, 4, 6, 9, … and the table's rows); a sorted cost
  and the clamps on entries can cross twice, and such a pair gets no choice
  point.
- `WHERE a = $1 AND b = $2`, indexes on `a` and on `b`, 10,000 rows: each
  scan reaches 1,000 entries and fetches each row, 2,000. Past 1,000 of
  `a`'s entries its scan costs more than `b`'s: threshold 1,001.

**The plan text.** The count is of the first plan's range, its key built
once and the first plan's scan opened on it; for a NULL split, its whole
prefix, or with no prefix its whole index, a `Range` of two open bounds. It
is limited to the threshold, so it costs no more than it must, and its two
record reads are charged like any read. The consumer, from a join's lookup
to the sink, is printed once per branch. Each plan's parameters are checked
for NULL and its `$n ± k` computed before anything is read.

```c
bc_view lo0 = …;                      /* the first plan's range */
bc_view hi0 = …;
int64_t count0 = bc_count_range(cx, IDX_6, lo0, hi0, 50001);
if (count0 < 50001) {
  /* the first plan's scan, opened on lo0 and hi0, its filters, then the consumer */
} else {
  /* the second plan's, then the consumer again */
}
```

None of TPROC-C's golden plans has one: O1 and P5 count `customer`'s range
by `bc_count_range`, whose cost is 2 at any count, and O4's two costs under
`LIMIT 1` never cross. The fixture's `choice_pk_or_index`,
`choice_read_once` and `choice_update` show one alone, read once under a
point lookup, and in an `UPDATE`; `no_choice_under_limit` the `LIMIT` case.

## The physical plan and codegen

The physical plan's operators:
- `Empty`, no rows and no read: the sink reports what it gives for none;
- `OneRow`, one row of no columns and no read: its consumer runs once. A
  plan of `Emit` over it is the one that names no table, so it has no
  constants;
- the access paths `PointGet`, `Scan` and `NullSplitScan`;
- `CountRange` and `KeyAggregates`, access paths that also produce the
  aggregate;
- the joins `IndexJoin`, `NestedLoop` and `HashJoin`;
- `Filter`, `Sort`, `Limit` and `Choice`;
- `Aggregate` (one row of `COUNT`, `COUNT(DISTINCT)` or `SUM`) and
  `GroupAggregate`;
- the sinks: `Emit`, `GroupEmit`, `Insert`, `Update` and `Delete`. An
  `Emit` target is a column or a parameter, a constant's, the same in
  every row.

The plan also lists the parameters a NULL of which ends it before it reads
(`null_checks`), the `$n ± k` values computed before anything is read, and
its result columns as PostgreSQL names and types them. The plan's own
columns for a result column are `physical::plan_columns` of its SQL type,
the one place they are derived: a `NUMERIC` (an `AVG`) is two `I64`, its
sum and its count, which the API divides; any other is one column of its
physical type.

**`check()`** enforces what the types can't express. A failure is a
compiler bug, a `std::logic_error`, never a plan. It checks each
combination of the plan's choice-point branches as the plan it runs, and
refuses:
- a key part or bound whose parameter the plan doesn't check for NULL, or
  past the statement's parameters, or a `$n ± k` it does not compute;
- a scan with neither a prefix nor a range; a covering scan under `FOR
  UPDATE`; a NULL split whose values don't start past the NULL part;
- a `DOUBLE` read from a key, wherever codegen reads one: a result column,
  an index join's inner key, a filter's operands, an aggregate's argument,
  a sort key part, a `KeyAggregates` value;
- a `LIMIT` not under the rows emitted; a write that sets no column, or
  sets one from another type;
- a choice point without two plans, with a negative threshold, at a table
  read once per outer row, or that counts another range than its first
  plan's (its object, prefix parameters, bounds and direction; a NULL
  split's whole prefix), each plan a scan of the counted table; at a hash
  join, one that counts another range than its build side's, or whose
  other plan reads other tables; at a grouped aggregate, one whose plans
  are not a structure and a stream of the same rows, counting the
  structure's input's range;
- a nested loop whose outer side is neither one row nor a loop, or whose
  inner side is not a scan (or a choice of two); an inner filter that
  compares a column outside the inner table, reads a covering scan's row,
  or reads a column outside the outer side;
- a hash join with no key, whose build side is not one loop over the table
  held, holds a covering scan's entry, or holds a table its probe side
  reads; a key that is not a column of the held table and one of the probe
  side, of one type, never `DOUBLE`;
- a sort with no key, over a covering scan, keeping a negative number of
  rows, or whose sorters hold other rows than its loops' (a sorter per loop
  of its input read above it, outer first, each over a row read); a column
  read above it from a loop no sorter holds; a sort key or NULL test
  outside its input's tables;
- an `Aggregate` of other than `COUNT` or `SUM`;
- `KeyAggregates` whose outputs are other than `COUNT(*)`, or `MIN` or
  `MAX` of its one argument; whose argument is not held as `I64`; whose
  values are read by a reverse scan, from another object or prefix than the
  one counted, or, the argument nullable, from a range whose low end is
  open or the NULL part itself (it would start at the NULLs);
- a structure of groups with no key, or that holds a covering scan's entry
  (a stream may read an I64 or BOOLEAN key from one); a streamed key of a
  type no local holds; a key or argument outside its input's tables; a
  dependent column that is a key, or of a group of no key; a `HAVING`, a
  sort of the groups or an output that names other than the groups' columns
  and aggregates; a sort of groups not over a structure of them.

**Codegen is produce/consume** (Neumann, "Efficiently Compiling Efficient
Query Plans for Modern Hardware", VLDB 2011; HyPer, then Umbra), each
consume passed to the input's produce as a continuation. A leaf's produce
prints the loop over its cursor, or the one `bc_get`, and runs its
parent's consume inside. A filter's consume prints an `if` that
`continue`s; a join's prints the inner probe. Everything between two
pipeline breakers becomes one nested loop, with the row's values in
locals. A row that fails a `Filter`, or a key that names no row, goes to
the next row in a loop; on a single row it ends the plan with what the sink
gives for no rows. Under `LIMIT 0` codegen prints an empty plan, the
computed `$n ± k` aside.

A pipeline breaker (`Sort`, a hash build, a structure of groups) consumes
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
execution batches of rows; neither can be written in the subset without
widening it. Produce/consume generates what the subset allows: nested
loops over API cursors, composed per operator. Its costs: two codegen
methods per operator; plan text, and TinyCC's compile time, grow with the
query, which is fine at OLTP sizes; and a choice point prints its consumer
once per branch. The generated C stays readable, and it is `EXPLAIN`'s
output.

**How each construct maps to the plan API.** Every pattern uses calls
`bc_plan.h` declares.

| Construct | Physical operator | Plan text |
|---|---|---|
| A whole primary key by equality | `PointGet` | `bc_key_params` (consecutive parameters) or `bc_key_begin` / `bc_key_*` / `bc_key_end`; `bc_get` |
| An equality prefix | `Scan`, its `prefix` | `bc_scan_open(lo = hi = prefix, BC_SCAN_PREFIX)` |
| A prefix and a range | `Scan`, its `rest` a `Range` | bounds from `bc_key_*`; an exclusive `lo` or an inclusive `hi` through `bc_key_successor` (`bc_plan.h`, *bounds*); with no lower bound on a nullable `ASC` part, `lo` past the NULL part |
| A full scan | `Scan`, its `rest` a `Range` of two open bounds | `bc_scan_open(bc_empty, bc_empty)` |
| `DESC` order | `Scan::reverse` | `BC_SCAN_REVERSE` |
| A nullable ordered part, its NULLs placed as PostgreSQL places them | `NullSplitScan` | two `bc_scan_open`s. Present values: `lo = bc_key_successor(prefix + NULL)`, `hi = bc_key_successor(prefix)`, no `BC_SCAN_PREFIX`. NULL part: `bc_scan_open(prefix + NULL, prefix + NULL, BC_SCAN_PREFIX)`, `prefix + NULL` the prefix's key with `bc_key_null` appended. `ASC`: present values forward, then the NULL part; `DESC`: the NULL part reversed, then the present values reversed. One loop body, one `LIMIT` counter |
| A covering read | `Scan::covering` | `bc_key_part_*` on `bc_scan_key`; otherwise `bc_scan_row` |
| A residual filter | `Filter` | `bc_col_*` on the row, compared in an `if` that `continue`s; a NULL operand rejects the row |
| `+` / `-` | an `Add` or `Sub` expression | `bc_add` / `bc_sub`; compared, computed once before anything is read and range-checked (below) |
| `SET col = col + $1` | `Update::set` | `bc_row_from`, `bc_row_set_*` with the sum, `bc_update`; on a BIGINT read, the sum by `bc_add_bigint` / `bc_sub_bigint`, which raise "bigint out of range" |
| UPDATE or DELETE of many rows | `Update`, `Delete` over a `Scan` | the scan, with `bc_update` / `bc_delete` per row; `bc_emit_count` with the rows written |
| `ORDER BY` an access path gives | none | the search picks a path in that order |
| Any other `ORDER BY` | `Sort` | `bc_sorter_*` (*Sorts*) |
| `LIMIT` | `Limit` | an `int64_t` counter in the loop's condition, so the cursor never steps past the limit |
| `COUNT(*)`, `COUNT(col NOT NULL)`, no residual | `CountRange` | `bc_count_range` over the access path's range |
| `COUNT`, `SUM` otherwise | `Aggregate` | an accumulator local, `bc_add`; a "seen" `bool` for SUM's NULL. Over at most one row no accumulator: its input's misses are the condition its row is emitted under |
| `COUNT(DISTINCT col)`, one row | `Aggregate` | `bc_agg_group` keyed by the column, then a count of `bc_agg_next` |
| `COUNT(*)`, `MIN`, `MAX` from the keys | `KeyAggregates` | `bc_count_range`, and one step per `MIN` or `MAX` (*Aggregates*) |
| `MIN`, `MAX`, `AVG`, several aggregates, `GROUP BY` | `GroupAggregate` | a structure (`bc_agg_*`) or a stream (locals) (*Aggregates*) |
| A join by key | `IndexJoin` | the inner key built from the outer row: `bc_key_*` of `bc_col_*`; nested in the outer loop |
| A join read once, or once per outer row | `NestedLoop` | the inner scan nested in the outer loop, opened once when the outer side is at most one row |
| A hash join | `HashJoin` | `bc_hash_*` (*Hash joins*) |
| `FOR UPDATE` | the access path's `for_update`, from the `Get`'s | `bc_guard_row` on each returned row: `bc_guard_unchanged` on its primary key |
| Targets | `Emit`, `GroupEmit`, or the aggregate's row | `bc_emit_col`, `bc_emit_key_part`; a constant's parameter, `bc_emit_<type>` of `bc_param_<type>` or `bc_emit_null`; for an aggregate, `bc_emit_i64` (every aggregate is held as an `int64_t`), and `bc_emit_null` for NULL |

A NULL that a key needs means no row can match, since `=` and the range
operators reject NULL: a parameter ends the plan before any read
(`null_checks`); an outer row's column in a join's inner key skips that
outer row.

**The range check on computed values.** `ol_o_id >= $3 - 20` computes an
`integer` that never reaches a row builder, and PostgreSQL raises 22003 when
such an operation leaves its type. `bc_add` / `bc_sub` check only `int64_t`,
and the row builder checks only what it stores, so the plan calls
`bc_check_int` with the value and the SQL type the compiler resolved, which
sets `BC_C_RANGE` when the value is outside it. Which type an operation is
in is the compiler's decision, so it shows in the plan text as one call;
the type's bounds are the runtime's, with the row builder's.
- Assignments need no call: the row builder checks the stored value
  (`CONTRACT.md`, values outside their declared type).
- `SET col = col ± $n` on a BIGINT: the sum itself can leave `int64_t`, so
  the plan adds with `bc_add_bigint` / `bc_sub_bigint`, which raise
  "bigint out of range". The type is the column read's, which the binder
  gives the `Add` or `Sub`, as PostgreSQL's operator takes its result type
  from its operands. The binder takes a BIGINT and a `DECIMAL` of scale 0
  as one type: a `DECIMAL(p, 0)` read cannot leave `int64_t`, its operand
  held to 18 digits, so its overflow is the assigned column's ("numeric
  field overflow") (`test_column_plus_parameter_across_bigint_and_scale_0_decimal`).
- `$n ± k` in a `WHERE`: `$n` is checked as an `INTEGER` before the plan
  runs, so `bc_add` cannot overflow `int64_t`
  (`test_a_computed_bound_past_integer_is_22003_before_the_plan_runs`).

### Sorts

A plan whose rows are not in the `ORDER BY`'s order is wrapped in a `Sort`
over `bc_sorter_*`, and compared with the plans in the order.
- The sort key is built with `bc_sort_key_*` per part, in its direction and
  NULL placement (`plan_api_design.md`, *Sort keys*). A part read from a
  covering scan's entry is `bc_sort_key_<type>` of `bc_key_part_<type>`,
  `bc_sort_key_null` where the entry's part is NULL; never a `DOUBLE`,
  which the key does not hold exactly.
- `ORDER BY … LIMIT n` opens the sorter with `keep = n`, and its loop keeps
  the `LIMIT`'s counter. `LIMIT 0` needs no order and sorts nothing.
- A sorter holds one view per row, so there is a sorter per loop of the
  plan whose row is read above the sort, by the result or by a lookup by key
  repeated there, each holding that loop's table's row, all fed the same
  sort key, built once per row. A sorter is stable and a top-n sorter keeps
  the earliest of equal keys, so sorters fed the same keys in the same order
  hold the rows of the same pairs, in the same order, and under `LIMIT n`
  all keep n. The sorter loop steps the first with `bc_sorter_next` and
  reads each other at the same place with `bc_sorter_nth`. A hash join's
  loops are its probe side's and the one over the rows it holds.
- A loop a sorter holds reads rows, never a covering scan's entries. A loop
  whose row nothing above the sort reads has no sorter: it is read only
  below it, and its access may cover. Where no loop's row is read above
  it, one sorter holds the keys alone, its payload `bc_empty`.
- Rows of tables read once before the first loop stay in hand. Tables
  looked up by key after the first loop are looked up again above the sort,
  from the row held: the same key on the same snapshot, so the same row,
  its filters held already.

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

### Hash joins

**Where.** A candidate where the table a plan adds is joined to it by an
equality between the sides, no key reaches it from the other rows, and the
outer side is not at most one row. The inner side is one table's access
path from parameters alone, read once: the tree stays left-deep. The
join's equalities key the hash table; its other conjuncts, which the binder
does not take yet, would filter the pairs. The nested loop reading the
table once per outer row is a candidate beside it, and the costs pick.

**The build side.** Either side is costed as the one the hash table holds:
the inner table, or the outer one when the plan so far is that table's
access alone. A row held costs more than a row probed, so the cheaper plan
builds on the smaller side by the cost model's estimate. Where an `ORDER
BY` of one side's columns sorts that side before it probes, the other side
is built, larger or not. The plan text names the side: `bc_hash h0 =
bc_hash_open(cx);  // built on TBL_1's rows`.

**The key.** Each row's key is a sort key of its join columns' values in
order, each `bc_sort_key_<type>` of the value read, ascending. A join
compares columns of one type (the binder), so equal SQL values are equal
physical values, and a part of one physical type gives equal values equal
bytes, read from a row or from a covering scan's entry. A row with a NULL
in any join column is neither put nor probed, since `=` never holds for
NULL; a probe side sorted before it probes drops it before the sorter, so
it is neither copied nor charged. A join column of type DOUBLE PRECISION or
REAL is refused, as a nested loop's is.

**Codegen.** The build side's loop, in a block of its own, builds each
row's key and puts the row. Then the probe side's loop builds its key,
finds, and loops over the rows held under it, the held one bound as
`b<i>`. A `LIMIT`'s counter is the probe loop's and the hash loop's, never
the build's.

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
row's pairs in the order their rows were put, so it gives the `ORDER BY`
where its probe side's access does. Otherwise, where every `ORDER BY`
column but those the `WHERE` makes constant is the probe side's table's,
the probe side can be sorted before it probes, every row kept (the join may
drop any of them), the `LIMIT` counted above. That is decided before any
loop is made to read rows: the probe side's sorter holds its row, so its
loop reads rows, and nothing else does for it. Any plan can instead be
sorted above the join (*Sorts*).

### Aggregates

**One row of `COUNT` or `SUM`.** A lone `COUNT`, `COUNT(DISTINCT)` or
`SUM`, with no `GROUP BY` or `HAVING`, is an `Aggregate` over its input: a
`CountRange` where the keys answer it, else the loop with an accumulator.
Everything else aggregated, a lone `MIN` or `MAX` among it, is a
`GroupAggregate`, one route, and where its aggregates allow it also
answered from the keys.

**Answered from the keys.** One group of every row, no `HAVING`, whose
outputs are each `COUNT(*)`, or `MIN` or `MAX` of one column held as
`I64`, the `ASC` part after the scan's equality prefix, with nothing
filtering a row after the range: `KeyAggregates(counted, values, arg,
outputs)`. `COUNT(*)` is `bc_count_range` of `counted`. `MIN` is the first
entry of `values`, which is `counted` started past the part's NULLs
wherever it starts at them (`physical::starts_at_nulls`: its low end open,
with no range on the part, an upper bound alone or a full scan, or the NULL
part as is) and the argument is nullable; `MAX` is its last, read as
the first entry of `values` scanned in reverse. Each `MIN` or `MAX` is read
once into `kv<i>`, NULL while `kn<i>`, by one step in a block of its own,
the cursor closed after it; then the row is emitted. A `DESC` part, another
aggregate, a second column, a residual filter, a `GROUP BY` or a `HAVING`
reads every row.

```c
/* SELECT min(b) FROM p WHERE a = $1 */
int64_t kv0 = 0;
bool kn0 = true;
{
  bc_view lo = bc_key_params(cx, TBL_4, 0, 1);
  bc_cursor c = bc_scan_open(cx, TBL_4, lo, lo, BC_SCAN_PREFIX);
  if (bc_scan_next(cx, c)) {
    kv0 = bc_key_part_i64(cx, TBL_4, bc_scan_key(cx, c), PART_4_1);
    kn0 = false;
  }
  bc_scan_close(cx, c);
}
bc_emit_begin(cx);
if (kn0) {
  bc_emit_null(cx);
} else {
  bc_emit_i64(cx, kv0);
}
bc_emit_end(cx);
```

A `MAX` is the same with `BC_SCAN_REVERSE`; with `COUNT(*)` beside them,
`int64_t n = bc_count_range(…)` comes first (`aggregates_from_keys`).

**`GroupAggregate(input, keys, aggs, streamed, dependent)`**, its rows the
groups; above it a `Filter` of its `HAVING`, a `Sort` of its groups
(holding each group's key), a `Limit`, and the sink, `GroupEmit`, whose
outputs are its keys, dependent columns and aggregates. Its columns are its
keys, then its dependent columns: a `HAVING`, a sort and the outputs may
name any of them, but only the keys make the group's key or start a
streamed group. A `HAVING` is an `if` block in either form. A `LIMIT`'s
counter is the loop over the groups or the sorter, never the input's.

**In a structure** (`streamed` false): each input row's group is found by
its key, a sort key of its key columns (`bc_sort_key_col`, ascending, NULLs
first, so equal values, `-0.0` and `0.0`, and NULLs give equal bytes), and
its values folded into the group's slots; then a loop over the groups in
the order they first appeared. Where it keeps what it gives for a group
(`GroupLayout`):
- slot 0 counts the group's rows, `COUNT(*)` among them; on the group's
  first row `bc_agg_null` of slot 0 is true;
- a key or dependent column held as `I64` has a slot, folded once, on the
  group's first row, by `bc_agg_min_i64`, NULL as `bc_agg_null`;
- any other (BOOLEAN, DOUBLE, a string) is read from the group's first row
  of its table, put in a hash table of the table's under the group's key:
  the plan API has no slot for it, and a group key's bytes decode as no
  table's. So that table's loop reads rows, and a choice point's second
  plan there reads rows too;
- each aggregate a slot: `COUNT` by `bc_agg_count`, `SUM` by
  `bc_agg_add_i64`, `MIN` and `MAX` by `bc_agg_min_i64` and
  `bc_agg_max_i64`, `AVG` two, its sum and its count; NULL inputs skipped,
  and `SUM`, `MIN`, `MAX` and `AVG` NULL where `bc_agg_null`;
- a `COUNT(DISTINCT col)` keeps an aggregate of its own, one slot, keyed by
  the group's key and the value: a pair new to it counts in the group.

A group's handle is found again by its key (`bc_agg_group` finds an
existing group and charges nothing), since a plan's locals are initialized
where declared and no call gives an empty group. A `Sort` of the groups
adds each one's key as its payload, and its loop finds the group again by
it.

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

**Streamed** (`streamed` true), where the input's rows come with each
group's together (*The search*, *Groups*): the group is kept in locals,
each key's value and NULL and each aggregate's accumulator, and given to
the consumer inside the input's loop when a row of another group starts
the next, then after the loop for the last. A dependent column is read from
each row and kept beside the keys. Its columns are I64 and BOOLEAN, the
types a local holds, read from a row or from a covering scan's entry: a
stream holds no row, so it may cover where a structure may not. Nothing is
held but the group in hand's `COUNT(DISTINCT)` values: each argument's
aggregate is keyed by the value alone and cleared (`bc_agg_clear`) as each
group ends, so a stream's memory does not grow with its groups. With no key
it is one group, given after the input, over no rows too, and its no-rows
form (a NULL parameter, a point lookup that misses) is the row of no rows,
each `COUNT` 0 and every other aggregate NULL, under its `HAVING`.

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

**Which form.** Every plan of the input may hold its groups in a
structure, sorted above where it does not give the `ORDER BY`; a plan whose
rows come in groups may also stream them, and only in the `ORDER BY`'s
order, since nothing holds its groups to sort them. A streamed plan gives
its groups in its rows' order and a structure in the order of their first
rows, which is also its rows' order, so either gives an `ORDER BY` of
grouped columns where its rows come in that order. Each plan is finished
as any plan is: choice points by costs (a streamed plan's second plan also
giving the groups), and at a hash join the choice by the budget at its
build. The cheapest plan without a hash join is that choice's second plan,
reading rows where the structure holds them; where the hash join gives the
`ORDER BY` by its order, that plan sorts its rows by it, every row kept,
its sorters holding the rows of each table the groups read, and it is made
ready under its own copy of the query's sort columns, which its build
reads. `SELECT a, count(*) FROM g GROUP BY a`, 1,000 rows, an index on a:
streamed in its order, covering, 1,000; the structure over any scan costs
more by 1,000 × 1.3 + 100.

**AVG** is PostgreSQL's `numeric`: `numeric_div` of the sum by the count
at the scale `select_div_scale` chooses, 16 significant digits or more,
rounded half away from zero, which can pass 64 bits. So the plan emits the
sum and the count, two `I64` columns for one result column
(`plan_columns`), and the API divides them as PostgreSQL does
(`average_text`). Its type is a kind of its own, `SqlKind::Numeric`,
PostgreSQL's numeric of no declared precision, which no column has; a
result row carries its value as its text, and every reader of `Rows` (the
Python binding, `pg_text`, the harness) decides by the kind. Its sum is an
integer sum, as `SUM`'s, with its 64-bit limit (`frontend_design.md`,
*Known differences*).

## Memory

**One memory budget per statement, charged the way the work budget is**
(owner, 2026-10-23), 64 MiB by default and configurable per database
(*Decisions*, 7). It replaced the arena's size and the byte bound each
blocking structure declared at open. PostgreSQL's `work_mem` is per
operator, so a plan with several sorts can use several times the setting;
one number per statement is simpler to state and to enforce.

**What is charged.** Every byte the plan API allocates for the statement:
keys and rows it builds or reads; the contents of sorters, hash tables and
aggregate groups; pinned values; the result rows it buffers; and the write
set, what the statement keeps until commit, or in a transaction until it
ends, when it moves to the transaction's own bound (`plan_api_design.md`,
*The write set is statement memory*). So a bulk write is bounded by the
budget like any other statement. It counts fixed amounts, not what the
allocator reports, so the accounting is deterministic and the
allocation-fault sweep can test it: scratch copies, results and structure
entries are charged the bytes requested plus a fixed amount per node; the
write set a conservative bound, each buffer at its capacity plus
`kAllocBytes`, each slot at twice its size, each node in full
(`plan_api_design.md`).

**Ownership decides what is held.** A view returned inside a loop lives
until that loop's next step, unless it is pinned or added to a structure:
a key built for one probe, an inner row and a group key all go when the
outer row moves on (#551). What outlives a step: pins, the contents of
blocking structures, result rows and the write set. The API marks the arena
per loop level (`plan_api_design.md`, *A loop's step owns what it
allocates*). Streaming operators therefore use memory bounded by the depth
of the pipeline, not by the number of rows; only pipeline breakers, results
and writes grow.

**Past the budget, an error, never a spill.** SQLSTATE 54000
(`program_limit_exceeded`), a limit the layer sets, not the machine running
out (*Decisions*, 8). PostgreSQL spills sorts and hashes to disk instead: a
known difference, listed in the contract. Spilling (an external sort, a
partitioned hash join) is a separate design for when a workload needs it.

**The planner uses the budget.** The search is given the statement's
budget (`bc_budget::max_bytes`). Where another plan avoids a sort, a hash
build or a structure of groups, the structure's size is an unknown count,
and the two plans are a choice point (*Choosing at run time*): the count of
the range the structure's input reads, a full scan's the whole table
(`bc_count_range` over `bc_empty` and `bc_empty`), limited to a count past
which the structure may not fit; below it the structure, from it the other
plan. The filters only make the count an upper bound on the rows held. No
count read at compile time decides it, so a plan compiled while its table
was small still answers once the table has grown. The first plan's branch
holds the structure; the choice point is over the whole plan, and checked
as reading the same tables in both branches.

Each threshold is `least_past(max_bytes, fixed, per row)`: (`max_bytes` −
fixed) / per row + 1, 0 for a budget under the fixed amount. Per row it
counts the least the API can charge: a row at least `least_row_bytes` (its
header, bitmap and fixed columns in the smallest layout its table's rows
may have), a key part at least `least_part_bytes` (its marker where it may
be NULL, else the marker and its least value: 8 bytes for an integer, 1 for
a BOOLEAN, 2 for an empty string). So a structure under the count fits
whenever its rows and keys are no larger than those bounds. Where it counts
a key per row, as if no two rows shared one, it is conservative: where rows
share keys, groups or values, it switches at fewer rows than would fail,
never at more.
- **A sort** of a plan of one table, where another plan of it gives the
  `ORDER BY`: fixed `kSorterBytes` (192), per row `kSortEntryBytes` (128).
  The other plan is the cheapest that gives the order, which may be a full
  scan of an index, and may cover. None under a `LIMIT` the budget holds.
  `SELECT k FROM w WHERE y >= $1 ORDER BY x`, an index on x, compiled at 10
  rows under 100,000 bytes: the full scan, filtered and sorted, costs 10 +
  3.3 × 1.5, less than the index's 2 × 10, and the plan counts `w` against
  780 when it runs.
- **A hash join** where another plan of the statement avoids the hash
  table: fixed `kHashBytes` (192), per row `kHashEntryBytes` (240), the row
  and its key. The other plan is the cheapest without a hash join, found
  once per statement and only where some plan has one, sorted where the
  statement's order needs it. `SELECT u.k FROM t, u WHERE t.name = $1 AND
  u.v = t.qty`, t at 40 rows and u at 100, under 10,000 bytes: the hash join
  built on t's range costs 8 + 4 × 1.3 + 100 + 40; a row of t holds at least
  36 bytes and a key 9, so the plan counts that range against 35. A sorted
  probe side, or a sort above the join, is not a choice by the budget.
- **A structure of groups** over one table's range, where a streamed plan
  avoids it: fixed `kAggBytes` and a byte per slot, `kHashBytes` per table
  whose rows its groups hold, `kAggBytes` + 1 per `COUNT(DISTINCT)`, and
  `kSorterBytes` where it sorts its groups; per row, as if each started a
  group and gave each `COUNT(DISTINCT)` a new value, what `bc_agg_group`,
  `bc_hash_put` and `bc_sorter_add` charge: the group's key,
  `kGroupEntryBytes` and `kAggSlotBytes` per slot; per held table its row,
  `kHashEntryBytes` and the group's key; per `COUNT(DISTINCT)` a key of the
  group's key, as a byte-string part, and the value's part,
  `kGroupEntryBytes` and one slot; in a sort the sort key, the group's key
  as payload and `kSortEntryBytes`. The other plan is the cheapest streamed
  plan that is not a hash join, compared by the cost of the plan the choice
  point builds, which takes no choice point of its own. `SELECT a,
  count(*) FROM g WHERE k < $1 GROUP BY a` under 10,000 bytes: 322 fixed
  and 233 per row, so k's range counts against 42; `SELECT a,
  count(DISTINCT v) …`: 644 fixed and per row 281 for the group and 195 for
  a value, against 20.

A structure known not to fit (a budget under its fixed amount) or a range
of at most one row, where one row may not fit, is left out where another
plan avoids it; one known to fit takes no choice point. Where nothing
avoids it, it runs, and past the budget fails with 54000.

**Not covered.** Memory across statements and sessions, a global cap,
belongs with the thread-model design, and so does the result buffer.
Results are buffered until the plan returns, because plans are effect-free
until commit, so a large result set counts against the budget. Streaming
results to the client is part of the transport design.

## What the plan API gained and lost

- **Lost:** the `bytes` argument of `bc_sorter_open`, `bc_hash_open` and
  `bc_agg_open`; `bc_ctx_new`'s `arena_bytes`, now a memory field in
  `bc_budget`, beside `max_reads`.
- **Gained:** the lifetime rule above; working `bc_sorter_*` and
  `bc_hash_*` calls and aggregate slots, which `bc_plan.h` declared and
  which set `BC_E_HANDLE` until slices 4 and 5 built them
  (`plan_api_design.md`, revision 8); a `keep` argument for
  `bc_sorter_open`, where 0 keeps every entry: a top-n sorter holds at most
  the first n in sort order, and an entry that sorts after the n held is
  not copied; `bc_sorter_nth`; and `bc_agg_clear`, which drops every group
  and releases its bytes, for a stream's `COUNT(DISTINCT)`.

Plans are only ever ours and are regenerated from SQL, so changing these
signatures broke no stored plan.

## What the search refuses

A statement is refused only when no plan computes it: the search tries
every plan, and records each failure where it happens, with the plan it
stops (an access path, or a step that adds a table to a set). A plan that
fails is carried on as far as its tables reach, so a failure counts only
when its plan reaches every table. Where plans fail for different reasons,
the refusal is the first one of the furthest stage a plan of every table
reached.

| # | Refused | Message |
|---|---|---|
| 1 | Every plan leaves a filter, a join conjunct or a hash key on a DOUBLE or REAL column | `no plan: a filter on column "<column>", a REAL or DOUBLE PRECISION, is not supported yet` |
| 2 | A `HAVING` comparison on a grouped DOUBLE or REAL column | the same message |

The refusal is a `FeatureNotSupported`, SQLSTATE 0A000, whose message
starts `no plan:`, raised after binding and before anything runs. It
carries no position: the search works on the logical plan, which keeps
none. A logical plan the search can't use, which today's binder never
builds, is refused the same way, never with another exception and never
with uncompilable C:
- an operator where the search plans another (`no plan: a Filter over a
  Filter where the search plans a table`, `an Empty where …`, `… where the
  search plans an Aggregate`);
- a conjunct `use_of` refuses (`no plan: a conjunct on <table> that does
  not compare one of its columns`, `… compares two columns outside a
  join`, `a join conjunct that does not compare a column of each side`,
  `a conjunct that reads no column`); a table joined with itself; more than
  32 tables;
- a result, aggregate, `GROUP BY` or `ORDER BY` column that is not a column
  of the statement's tables; an output or `ORDER BY` key not grouped; a
  `HAVING` conjunct that does not compare a grouped column or an aggregate
  with a parameter.

PostgreSQL runs each of these; they are the entry of `frontend_design.md`'s
*Known differences* that is a feature gap. A table no key reaches, an
`ORDER BY` no index gives, an `ORDER BY` on a join, any NULL placement, a
join no key reaches and an `ORDER BY` over a plan of two loops are planned.

## Tests

- **Golden plans** (`tests/golden/`, `golden_test`). Every TPROC-C
  statement and each fixture statement has its plan text; a golden diff
  shows every plan a change moves, and each is explained in its PR. Slice 1
  regenerated every MVP plan byte for byte, so the new pipeline started at
  the MVP's reads. The fixture covers choice points (`choice_*`,
  `no_choice_under_limit`), full scans (`full_scan*`, `update_full_scan`,
  `delete_full_scan`), orders and sorts (`order_full_index`,
  `order_read_once`, `sort_parts`, `sort_top_n`, `sort_full_scan`,
  `sort_double`, `sort_join`, `sort_read_once`, `sort_cross_join`,
  `sort_keys_alone`), hash joins (`hash_join`, `hash_join_two_keys`,
  `hash_join_limit`, `hash_join_limit_zero`, `hash_join_sorted`,
  `hash_join_sorted_both`, `hash_join_sorted_null_key`,
  `hash_join_distinct`, `nested_loop_limit`), aggregates from the keys
  (`min_first_key`, `max_last_key`, `aggregates_from_keys`,
  `min_loop_nullable`, `min_desc_covering`, `max_desc_part`) and `GROUP BY`
  (`group_by_hash`, `group_by_stream`, `group_by_stream_limit`,
  `group_by_held_row`, `group_by_having`, `group_by_sorted`,
  `group_by_distinct`, `group_by_join`, `group_by_double`,
  `group_by_dependent`, `group_by_dependent_held`, `aggregates_ungrouped`).
- **Alternatives agree.** `search_all` returns every plan the search
  costed, none pruned, cheapest first. `generate_alternatives` prints each
  once per combination of its choice points' branches, 2^k for k choice
  points, k at most 4, its thresholds overridden
  (`physical::override_thresholds`, one per choice point: INT64_MAX takes
  the first plan, 0 the second). `Database::alternatives_for_testing` (the
  test build) runs each on its own snapshot. The proof machine runs them
  after every `SELECT`, aggregate, join and `GROUP BY` it compares with
  PostgreSQL, in autocommit, and checks each plan's rows as it checks the
  layer's; an embedded-API test does the same in C++. This catches a wrong
  rewrite or a wrong access path without a reference result, and runs both
  branches of every choice point.
- **The search** (`search_test`) with fixed counts: the join order; `COUNT`
  from the keys or by a loop; a plan in the `ORDER BY`'s order against a
  cheaper one sorted, and a `LIMIT`; a point lookup against a unique
  index's entry, a covering scan against a fetching one; a table read once,
  covering, filtered by the join's conjuncts; a refusal naming the failure
  of a plan of every table; a choice point's threshold, and none where one
  range lies inside the other, under a `LIMIT` its first entry ends, at a
  unique index or a point lookup, or where two costs cross more than once;
  full scans; a hash join against the nested loop, its build side, a
  sorted probe side, sorters per loop and their access; a structure against
  a stream, a key the `WHERE` makes constant, a dependent column, a BOOLEAN
  key; the choice by the budget for a sort, a hash join and a structure, its
  threshold, left out where known not to fit, a grouped hash join's choice;
  `COUNT(*)`, `MIN` and `MAX` from the keys, a lone `MIN` and a lone `MAX`
  by one route, and a loop where they don't apply; under `LIMIT 0` no plan
  reads anything. Every plan the tests cost passes `check` in every
  combination of its branches.
- **`physical::check`** (`physical_test`): each refusal of *The physical
  plan and codegen*, with a plan that passes beside it; among them a
  `KeyAggregates` of a nullable argument whose range starts at the NULLs
  (no range, a full scan's, an upper bound alone, the NULL part itself),
  and of an argument not held as `I64`.
- **Codegen** (`codegen_test`): bound forms, writes of many rows, `MIN`
  from its first entry past the NULLs or by a loop as one group, `SUM` by a
  loop, `DOUBLE` columns read from rows, refusals with their messages, and
  `FOR UPDATE`.
- **The budget at run time** (`api_transaction_test`): a sort compiled at
  10 rows under 64 KiB, run at 1,000, takes the index in its order; a hash
  join compiled at 20 rows of its build side counts its range by its rows'
  and keys' least bytes, reads the other table once per row past it, and
  its forced branch fails with 54000; a structure compiled at 10 rows, run
  at 2,000 groups, takes the stream, and grouped by a column no index
  orders fails with 54000; a streamed `COUNT(DISTINCT)` of 2,000 groups
  runs under 64 KiB. A statement past the budget fails with 54000 and
  leaves nothing behind; a join over many outer rows stays within a small
  budget (#551).
- **The allocation-fault sweep** fails each allocation of compiled sorted,
  hash join and grouped plans in turn (`embedded API: an allocation failure
  in a sorted plan`, `… in a hash join plan`, `… in a grouped plan is
  BC_E_MEMORY or its rows`).
- **The proof machine against PostgreSQL** generates full scans, `UPDATE`
  and `DELETE` over them; `ORDER BY` on any columns, mixed directions,
  `NULLS FIRST`/`LAST` and `LIMIT`; NaN and infinities, compared as
  PostgreSQL orders them; joins by key, read once (`join_read_once`,
  `join_read_once_on_keys`) and with no key (`join_no_key`), with
  `insert_a_match` putting NULLs and duplicates in join columns; `GROUP BY`
  of one or two columns of any type, every aggregate, `HAVING` of keys and
  aggregates, a `HAVING` reusing the `WHERE`'s `$n` on an integral column,
  a `NUMERIC(18, 0)` among them (`having_reuses_a_parameter`); no `GROUP
  BY` with several aggregates; and grouped joins (`group_by`,
  `group_by_join`). Its models `served` and `join_served` say whether some
  plan serves a statement: what they don't serve is the `DOUBLE` filter.
  Fixed cases: `test_both_plans_of_a_choice_point_agree`,
  `test_hash_joins`, `test_group_by`,
  `test_a_grouped_hash_join_and_its_other_plan_both_order_the_groups`,
  `test_a_having_parameter_typed_by_the_where`.
- **The harness** (`sql_layer_tprocc`, 2 warehouses, 4 threads, 30 s): the
  consistency conditions hold after the run and in `check`, and its
  statements' plans are the golden ones.
- **`AVG`**: `average_text` against PostgreSQL's digits, and through the
  API and the server's text.

## Slices

Each slice was one PR, or a few, with its tests.
1. **The new pipeline at parity.** The logical plan, the rewrites, a search
   with only the MVP's paths, the physical plan and produce/consume
   codegen; every MVP plan unchanged; the old IR, planner and codegen
   removed in the same PR.
2. **Costs and choices.** The cost model, cardinalities from
   `bc_count_range`, interesting orders and choice points.
3. **Memory.** The statement budget, the lifetime rule, and #551.
4. **Sorts and full scans.** `bc_sorter_*`, and #533's refusals lifted.
5. **Hash joins and `GROUP BY`**, with `HAVING`, `MAX`, `AVG` and several
   aggregates in one select list.

The rest of the dialect stays in #534: three or more tables, aliases,
expressions, `OFFSET`, `RETURNING` and multi-row `VALUES`, subqueries and
outer joins, `FOR UPDATE` on joins, and `EXPLAIN`. The serializable guards,
the plan cache and procedures (#495) keep their own designs.

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
   fixed selectivities) are measured once on the harness and stated here:
   *The cost model*, *The constants, measured*.
10. **No recompiling when counts age**, until a workload shows a plan going
    stale.
11. **At most 4 choice points per plan.**
