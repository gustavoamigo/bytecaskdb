# Plan language: physical plans written as functions

Status: **proposed**, not approved; nothing built. It names decisions for the
owner (*Decisions to make*) and changes none of the planner's.

A text language whose programs are physical plans: each call is one node of
`physical::Plan` (`plan/physical.cppm`), and nothing between the text and
the plan chooses anything. It is a second way into the pipeline the SQL
planner already ends in:

```
SQL  → parser → binder → logical plan → rewrites → search ─┐
                                                           ├→ physical::Plan → check() → codegen → bc_plan.h
plan text → parser → type checker → lowering ──────────────┘
```

`check()`, codegen, the plan API and the runtime are unchanged.

## Why a language for physical plans

- **A plan reads more easily than its SQL.** It is a tree of small
  operators in dataflow order. SQL's fixed clause order (`SELECT` first,
  `HAVING` for a filter after grouping) hides that order.
- **A written plan runs as written.** Its cost grows with the data, in plain
  view, and changes only when someone changes the text. A planned statement
  can change plan when its parameters or the data cross a choice point's
  threshold. For an OLTP statement, a plan that is steadily what it is
  beats a better average with occasional surprises: the same priority as the
  repository's *Predictable latency over peak throughput*.
- **OLTP plans are small.** Point lookups, short range scans, a join or two
  by key. The search earns its keep on many-table joins; a person writes
  these plans as well as it does.

SQL stays for everything else. The language does not replace the planner;
`EXPLAIN` prints the planner's choice in this language, so a planned
statement can be read, checked in and run as written (*EXPLAIN prints the
language*).

The language is functional, and the generated C stays imperative: a program
is one expression, each operator a function from relations to a relation;
codegen turns the tree into the nested loops it writes today.

## The language's syntax

A program is calls, a pipe, and `let`.

```
let recent(since) =
  scan(orders, index: orders_by_date, from: since)

let big_regions(since, min_total) =
  recent(since)
  | hash_join(build: scan(customers), on: customers.id == customer_id)
  | group(by: [customers.region], aggs: [total: sum(amount)])
  | filter(total > min_total)
  | sort(by: [region])
  | emit(region, total)

big_regions($1, $2)
```

- **`f(a, b, name: c)`** calls an operator or a `let` function. Positional
  arguments come first, then named ones, written `name: value`.
- **`x | f(a)`** is `f(x, a)`: the pipe passes its input as the first
  argument. An operator's second input is a named argument (`build:`), so
  which input is which is always written.
- **`let x = e`** names an expression; **`let f(p, q) = e`** defines a
  function. A program is any number of `let`s, then one expression: the
  plan. A name is defined before it is used, so nothing recurses.
- **Expressions are infix** (`total > min_total`). `==` compares and `=`
  only binds, so neither has two meanings. `$n` is the statement's
  parameter `n`.
- **A bare name in an operator's arguments is a column of its input**;
  `customers.region` qualifies one that two inputs share.

The grammar, LL(1):

```
program  = { let } expr
let      = "let" ident [ "(" [ ident { "," ident } ] ")" ] "=" expr
expr     = or { "|" call }
or       = and { "or" and }
and      = not { "and" not }
not      = "not" not | cmp
cmp      = add [ ("=="|"!="|"<"|"<="|">"|">=") add ]
add      = mul { ("+"|"-"|"||") mul }
mul      = unary { ("*"|"/") unary }
unary    = "-" unary | primary
primary  = literal | "$" int | "null" | name [ args ] | "(" expr ")" | list
name     = ident [ "." ident ]
call     = name args
args     = "(" [ arg { "," arg } ] ")"
list     = "[" [ arg { "," arg } ] "]"
arg      = [ ident ":" ] expr
```

No separator is needed between `let`s: an expression never continues with
`let`. `|` is the pipe and `||` concatenation; the lexer takes the longest
token. Everything else, `LIKE`, `IS NULL`, `COALESCE`, `CASE`, is a call:
`like(name, $1)`, `is_null(x)`, `coalesce(a, b)`, `case(c1, v1, c2, v2,
else)`.

## The operators, one per physical plan node

Each operator lowers to the node of the same row; its arguments are the
node's fields. The rules in the third column are the node's own
(`plan/physical.cppm`), which the type checker enforces before lowering.

| Operator | Node | Its rules |
|---|---|---|
| `nothing()` | `Empty` | |
| `one_row()` | `OneRow` | |
| `get(t, key: [a, b])` | `PointGet` | the whole primary key, by equality |
| `scan(t, index:, eq: [...], from:/after:, to:/before:, reverse:, as:)` | `Scan` | `eq` an equality prefix of the key, in order; bounds on the next part |
| `scan(…, nulls: last)` | `NullSplitScan` | the first ordered part nullable and ASC |
| `count_range(scan(…))` | `CountRange` | |
| `key_aggregates(scan(…), aggs: [...])` | `KeyAggregates` | `COUNT(*)`, and `MIN`/`MAX` of the part after the prefix |
| `lookup_join(outer, t, key: [...], left:, match:)` | `IndexJoin` | `key` t's whole primary key, of outer columns and parameters |
| `loop_join(outer, inner:, left:)` | `NestedLoop` | `inner` reads parameters alone |
| `hash_join(probe, build:, on:)` | `HashJoin` | `build` one table in one loop; `on` equalities of build and probe columns, never `DOUBLE` |
| `choose(count:, threshold:, below:, otherwise:)` | `Choice` | see *Decisions to make* |
| `filter(input, p, q, …)` | `Filter` | each argument a conjunct |
| `sort(input, by: [a, desc(b), nulls_first(c)], keep:)` | `Sort` | keys columns; over groups, GROUP BY columns |
| `limit(input, n)` | `Limit` | under `emit` |
| `aggregate(input, a)` | `Aggregate` | `COUNT(*)`, or `COUNT`, `COUNT(DISTINCT)` or `SUM` of an integer |
| `group(input, by:, aggs:)` | `GroupAggregate` | |
| `stream_group(input, by:, aggs:)` | `GroupAggregate`, `streamed` | input ordered by a prefix covering `by` |
| `emit(input, a, b, …)` over rows | `Emit` | |
| `emit(input, a, b, …)` over groups | `GroupEmit` | GROUP BY columns and aggregates |
| `insert(t, col: e, …)` | `Insert` | |
| `update(input, t, set: [col: e, …])` | `Update` | |
| `delete(input, t)` | `Delete` | |

Bounds: `from: x` and `to: x` include x, `after: x` and `before: x` exclude
it (`KeyBound::successor`). `from: null` starts at the part's NULLs and
`after: null` past them (`KeyBound::NullPart`). A scan with `for_update:
true` guards each row it returns (`Scan::for_update`).

`as:` names a second use of a table: `scan(orders, as: o2)`. The binder
already makes each use of a table its own instance (`logical.cppm`,
`ColumnRef`), so `o2.amount` lowers to a column of that instance.

## Fields the lowering derives

Some fields of a node are consequences of the plan, not choices: the
planner computes them, and so does the lowering, from the operators and
the catalog. A program never writes them.

- `Scan::covering`: every column read above the scan is in the index's key.
- `HashJoin::held`: the build side's table.
- `Sort::payloads`, `Sort::not_null`, `Sort::groups`: the loops below the
  sort, a hash join's probe key, and whether its input is groups.
- `GroupAggregate::dependent`: columns the keys determine.
- `Insert::checks`, `Update::checks`: the table's constraints.
- On `Plan`: `param_types` (each parameter's type is the column it is
  compared with or written to), `null_checks` (each parameter in a key
  part), `computed` (each `$n ± k`), `result`, `constants`,
  `catalog_version`, and `cost` (the cost model's, for `EXPLAIN`).

A literal in the text becomes a parameter, as the binder makes a constant
one (`ParamTarget`), so a plan's text and its parameters stay apart.

## The type checker

Nothing is annotated. Types come from the catalog and from each operator's
signature. A relation's type is one of two:

- **Rows**: its columns (`ColumnRef`: instance, column, physical type,
  nullable), its order (a list of `SortPart`s, or none), and whether it has
  at most one row (`PointGet`, a lookup by key under one).
- **Groups**: its GROUP BY columns, its aggregates by name, and its order.

The kind decides what a program may do next: `emit` over groups is
`GroupEmit`, `filter` over groups is `HAVING`, a `sort` over groups keys on
GROUP BY columns. `stream_group` requires its input's order to cover `by`;
`lookup_join` requires `key` to be the inner table's whole primary key;
`hash_join` requires `build` to be one table's scan under filters. Each
error names the operator, the rule, and where the program says otherwise.

A `let` function is checked where it is called: the call is replaced by its
body with the arguments substituted, then checked. No generics, no
inference algorithm; nothing recurses, so expansion ends. An error in a
function names the call that reached it. A name used twice is expanded
twice, and its plan runs twice: the plan is a tree, as
`physical::NodePtr` is.

**`check()` stays a compiler-bug check.** Today a failure there is a
`std::logic_error`, never a refusal (`planner_design.md`, *The physical
plan and codegen*). Each of its rules is either a type rule here or true of
every lowered plan by construction, so a program that type-checks never
fails it. A plan that does is a bug in the checker, as one from the search
is a bug in the search. The proof: run every rule of `check()` against a
lowered program in the tests (*Tests*).

## EXPLAIN prints the language

A printer turns any `physical::Plan` into this language, one operator per
node, derived fields left out. Parsing and lowering the printed text gives
back an equal plan. That makes three things one:

- `EXPLAIN` output, read by a person;
- a pinned plan: printed once, checked in, run as written;
- the test corpus (*Tests*).

The text names tables, indexes and columns by name, not by id, so a pinned
plan survives a catalog change that keeps its names: it is lowered again
against the new catalog. One that names something dropped is refused when
it is lowered, before it runs.

## Tests

- **Round trip.** For every statement the functional suite plans, print
  the plan, parse and lower the text, and compare with the planner's plan.
  That covers every operator the planner writes, from the corpus the layer
  already has.
- **Same rows.** The lowered plan runs, and its rows match the planner's
  plan's, as *alternatives agree* does for choice points.
- **Refusals.** One case per type rule, each a program that breaks it.
- **`check()` unreachable.** Every program in the tests that type-checks
  passes `check()`.

## What the language leaves out

Subqueries (`Plan::subs`) and set operations (`SetPlan`) are left out of
the first version; each would be an operator over whole plans. A statement
that needs them stays SQL.

## Decisions to make

1. **Plan text from users.** *Plans are only ever ours* (owner decision,
   2026-10-20, `plan_api_design.md`) is about the C subset: the runtime runs
   only plans its compiler generates. That holds: a program is lowered to
   `physical::Plan` and compiled by codegen, never run as C. But the
   lowering takes text from users, which the decision did not foresee.
   Confirm that it holds as read.
2. **Choice points in a written plan.** Refuse `choose` (a written plan
   never switches at run time), or allow it, so a pinned plan can hold the
   planner's choice point. The printer needs one or the other for a plan
   that has one.
3. **The operators' names.** The table above is a proposal.
4. **How a program reaches the layer.** A call on the embedded API beside
   `SQL`, a wire-protocol statement, or both.
5. **`aggregate` and `group` without `by`.** `Aggregate` covers a few
   aggregates and `GroupAggregate` with no keys the rest. Keep both
   operators, as the node list does, or one, `group(aggs:)`, that the
   lowering maps to whichever fits.
