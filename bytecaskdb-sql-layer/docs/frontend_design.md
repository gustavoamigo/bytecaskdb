# The SQL front end: parsing, the dialect and binding

Status: **built.** This covers what turns SQL text into a statement the
planner can take: the parser, the dialect the layer accepts, and the binder,
which resolves names and types. It also covers the layer's known
differences from PostgreSQL in what SQL means. The planner is in
*The current planner* below, until `planner_design.md` replaces it. The
TPROC-C statements the layer runs are in `testing_design.md`, *The TPROC-C
statements*.

## Standing rules

- **Do what PostgreSQL does.** Every accepted construct has PostgreSQL 17's
  semantics, and the proof machine compares it with PostgreSQL.
- **Never wait.** Nothing here blocks. `FOR UPDATE` is a guard, checked at
  commit (`transaction_design.md`).
- **Only the compiler's plans are supported.** The planner and the code
  generator are the only source of plan text.
- **Refuse rather than guess.** A statement outside the dialect is refused
  at parse or bind, with a message and a position. One inside it that the
  planner can't plan is refused by the planner (*What the planner
  refuses*). Either way, nothing has run.

## Parsing

libpg_query 17 parses the text into PostgreSQL's parse tree, read as
protobuf-c, and a transformer turns it into the layer's AST
(`sql/parser.cppm`, `sql/ast.cppm`). The dialect is PostgreSQL's. A node
kind the transformer doesn't take is refused with its position.

## The dialect

**Statements.**
- `CREATE TABLE`, `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX` and
  `ALTER TABLE … ADD COLUMN` (`catalog_design.md`, *DDL*).
- `INSERT … VALUES` of one row, with or without a column list.
- `SELECT` targets: columns, `COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT
  col)`, `SUM(col)` and `MIN(col)`. There's no `GROUP BY`: an aggregate
  makes the query ungrouped and it returns exactly one row. A query mixing
  aggregates and bare columns is refused (42803), as in PostgreSQL.
- `FROM`: one table, or two joined by a comma or `[INNER] JOIN … ON`.
  Columns may be qualified by the table's name.
- `UPDATE` and `DELETE` of any number of rows.
- `ORDER BY`, `LIMIT` and `FOR UPDATE`, below.

**Literals and expressions.**
- **Integer literals**, typed as PostgreSQL types them: `integer` if they
  fit, else `bigint`, else refused. Other values travel as parameters.
- **`+` and `-`** on integers and on `DECIMAL`, with PostgreSQL's result
  type: `smallint + smallint` is `smallint`, `integer + bigint` is
  `bigint`, and so on.
  - `DECIMAL ± DECIMAL` needs equal scales; others are refused.
  - The result is checked against its type at the operation, as
    PostgreSQL's operators check it: `integer out of range`, `smallint out
    of range`, `numeric field overflow`, all SQLSTATE 22003.
  - An expression involving NULL is NULL.
- **Where expressions may appear:** `SET col = expr`, a comparison's
  operand (`ol_o_id >= $3 - 20`), and `VALUES`.

**`WHERE`** is a conjunction (`AND`) of comparisons (`=`, `<`, `<=`, `>`,
`>=`) between expressions, in any order, on any columns.
- A comparison with NULL is not true, so it rejects the row, as in SQL's
  three-valued logic.
- Comparing two columns of different SQL types is refused (*Known
  differences*).

**`ORDER BY` and `LIMIT`.**
- `ORDER BY` takes a list of columns, each `ASC` or `DESC`. Ties come back
  in any order, as PostgreSQL allows.
- `LIMIT` takes an integer literal or a parameter. `LIMIT NULL` is no
  limit, and a negative limit is refused (2201W), as in PostgreSQL.

**Aggregates**, with PostgreSQL's results:
- `COUNT(*)` counts rows, `COUNT(col)` non-NULL values, `COUNT(DISTINCT
  col)` distinct non-NULL values. All three are `bigint`, and 0 over no
  rows.
- `SUM` over `SMALLINT` or `INTEGER` is `bigint`. Over `BIGINT` or
  `DECIMAL` it is `numeric`, kept at the input's scale. It is NULL over no
  rows.
- `MIN` is of the column's type, and NULL over no rows. It takes a numeric
  or temporal column.
  - `MIN` of a `TEXT`, `VARCHAR` or `BYTEA` column is refused (#534).
    Keeping the least value seen needs a buffer the plan API doesn't have.
  - `MIN` of a `REAL` or `DOUBLE PRECISION` column is refused too (*Known
    differences*).

**`FOR UPDATE`**, on a single-table `SELECT`:
- every row returned is guarded with `bc_guard_unchanged`, and the commit
  fails with 40001 if another transaction committed a change to one first;
- with an aggregate it is refused (0A000, "FOR UPDATE is not allowed with
  aggregate functions"), as PostgreSQL refuses it;
- on a join, not yet (#534).

**Refused**, with a message and the node's position: everything not above.
That includes:
- `OR`, `NOT`, `IN`, `BETWEEN`, `IS NULL`;
- subqueries, derived tables, set operations;
- `GROUP BY`, `HAVING`, `DISTINCT` on the select list;
- outer joins, three or more tables, table aliases;
- `OFFSET`, `RETURNING`, multi-row `VALUES`;
- `*`, `/`, string concatenation and functions;
- literals other than integers.

The list and its order of work are #534.

## Binding

The binder (`sql/binder.cppm`) resolves names against the catalog version
the statement pinned, and types every expression by `types_design.md`'s
rules, as PostgreSQL does. The defaults of columns an `INSERT` leaves out
are the runtime's, filled by `bc_row_begin` (`catalog_design.md`).

**Parameters** are typed by their use, as PostgreSQL types `$n`:
- compared with a column, or combined with one in arithmetic: the column's
  type;
- assigned to a column: the column's type;
- combined only with an integer literal (`$3 - 20`, where `$3` has no
  other use): `integer`;
- otherwise: "could not determine data type of parameter $n" (42P18).

**One SQL type per parameter.** A parameter used with columns of two SQL
types is refused at bind, whether it is compared with them, assigned to
them or added to them. A `DECIMAL` of scale 0 counts as a `BIGINT` here.
PostgreSQL infers one type and runs the statement (*Known differences*).
A parameter's range is checked against the narrowest of its uses before
the plan runs, and a refusal names that use's table.

**Errors.** A parse or bind error is an `ast::SqlError` with a message and a
position. `sqlstate()` reports it as 42601 for now; PostgreSQL's own codes
for binder errors (42P01, 42703, 42804, 42P18) are #548. The planner's
refusals are `FeatureNotSupported`, 0A000.

## The current planner

This section describes the planner on `main`. `planner_design.md` replaces
it, and its first slice deletes this section.

### The statement IR

The planner builds a small tree, and the code generator prints each
node with one fixed pattern. A plan's text is a pure function of the IR,
so golden plans pin the planner's choices and the printing together.

```cpp
namespace bytecask::sql::ir {

// A value in scope: a parameter, a constant, or a column or key part of a
// row an outer loop holds. Typed: its SqlType decides its decoder, its
// NULL handling and its arithmetic.
struct Expr {
  enum class Kind { Param, Const, Column, KeyPart, Add, Sub } kind;
  SqlType type;
  std::uint32_t param{0}, loop{0}, col{0};  // Param; Column and KeyPart: which loop, which column
  std::int64_t constant{0};                 // Const
  std::unique_ptr<Expr> lhs, rhs;           // Add, Sub
};

struct Compare { ast::Compare::Op op; Expr lhs, rhs; };

// How one table is reached. Every id and type the code generator prints is
// here: it never reads the catalog.
struct PointGet {                    // the whole primary key, by equality
  std::uint32_t table;
  std::vector<Expr> key;
};

struct Bound { Expr value; bool inclusive; };
struct Range { std::optional<Bound> lo, hi; };   // on the part after the prefix
struct PresentOnly {};               // the part after the prefix, its NULL part skipped:
                                     // [successor(prefix + NULL), successor(prefix))

struct Scan {
  std::uint32_t table;
  std::uint32_t object;              // the table (its primary key) or one of its indexes
  std::vector<Expr> eq;              // the equality prefix
  std::variant<std::monostate, Range, PresentOnly> rest;
  bool reverse{false};
  bool covering{false};              // every column used comes from the key
};

// The two-range form (*The planner's choices*): the present values, then
// the NULL part (BC_SCAN_PREFIX on prefix + NULL), or the NULL part first.
struct NullSplitScan {
  Scan present;                      // its rest is PresentOnly
  bool nulls_first;                  // DESC
  std::vector<Compare> residual;
};

struct Loop {
  std::variant<PointGet, Scan> path;
  std::vector<Compare> residual;     // the conjuncts the key does not express, in WHERE order
};

struct Aggregate { enum class Fn { CountStar, Count, CountDistinct, Sum, Min } fn; Expr arg; };
struct CountFromKeys {};             // bc_count_range over the scan's range
struct MinFromFirstKey {};           // the scan's first entry; skips NULLs by its PresentOnly
struct Rows {
  std::vector<Expr> targets;
  std::optional<Expr> limit;
  bool for_update{false};
};

struct Select {
  // One table, ordered or not; or a join, which takes no ORDER BY.
  std::variant<Loop, NullSplitScan, std::pair<Loop, Loop>> from;   // a join: outer, inner
  std::variant<Rows, Aggregate, CountFromKeys, MinFromFirstKey> out;
};

struct Insert { std::uint32_t table; std::vector<std::optional<Expr>> values; };  // per column; nullopt: default
struct Update { Loop rows; std::vector<std::pair<std::uint32_t, Expr>> set; };  // no order, so never split
struct Delete { Loop rows; };

using Statement = std::variant<Select, Insert, Update, Delete>;

}  // namespace bytecask::sql::ir
```

The types exclude the combinations that can't be planned: a point lookup
has no range, no order and no NULL split; a join and an `UPDATE` or
`DELETE` never hold a `NullSplitScan`; a range and the NULL split are
alternatives of one `rest`. What the types can't express, `ir::check`
enforces on every IR before it is printed, and a failure is a compiler
bug, raised as an internal error, never a plan:
- a `NullSplitScan`'s scan has `rest` = `PresentOnly`;
- `MinFromFirstKey` reads a forward `Scan` in a `Loop`, with no residual,
  whose `rest` is `PresentOnly` or nothing;
- `CountFromKeys` reads a `Scan` in a `Loop` with no residual;
- `limit` and `for_update` are only on `Rows`.

**How each construct maps to the plan API.** No new loop or structure is
needed; every pattern below uses calls `bc_plan.h` declares.

| Construct | IR | Plan text |
|---|---|---|
| A whole primary key by equality | `PointGet` | `bc_key_params` (consecutive parameters) or `bc_key_begin` / `bc_key_*` / `bc_key_end`; `bc_get` |
| An equality prefix | `Scan{eq}` | `bc_scan_open(lo = hi = prefix, BC_SCAN_PREFIX)` |
| A prefix and a range | `Scan{eq, Range}` | bounds from `bc_key_*`; an exclusive `lo` or an inclusive `hi` through `bc_key_successor` (`bc_plan.h`, *bounds*) |
| `DESC` order | `Scan::reverse` | `BC_SCAN_REVERSE` |
| A nullable ordered part, its NULLs placed as PostgreSQL places them | `NullSplitScan` | two `bc_scan_open`s. Present values: `bc_scan_open(lo = bc_key_successor(prefix + NULL), hi = bc_key_successor(prefix))`, no `BC_SCAN_PREFIX`. NULL part: `bc_scan_open(prefix + NULL, prefix + NULL, BC_SCAN_PREFIX)`, where `prefix + NULL` is the prefix's key with `bc_key_null` appended. `ASC`: present values forward, then the NULL part; `DESC`: the NULL part reversed, then the present values reversed. One loop body, one `LIMIT` counter |
| A covering read | `Scan::covering` | `bc_key_part_*` on `bc_scan_key`; otherwise `bc_scan_row` |
| A residual filter | `Loop::residual` | `bc_col_*` or `bc_key_part_*`, compared in an `if` that `continue`s; a NULL operand rejects the row |
| `+` / `-` | `Expr{Add, Sub}` | `bc_add` / `bc_sub`, then the range check of its type (*Additions*); in a `SET` on a BIGINT read, `bc_add_bigint` / `bc_sub_bigint` (`Expr::bigint`), which raise "bigint out of range" on the table |
| `SET col = col + $1` | `Update{set}` | `bc_row_from`, `bc_row_set_*` with the sum, `bc_update` |
| UPDATE or DELETE of many rows | `Update{rows}`, `Delete{rows}` with a `Scan` | the scan, with `bc_update` / `bc_delete` per row; `bc_emit_count` with the rows written |
| `ORDER BY` | the access path's order | none: the planner picks a path in that order |
| `LIMIT` | `Rows::limit` | an `int64_t` counter; `break` at the limit |
| `COUNT(*)`, `COUNT(col NOT NULL)`, no residual | `CountFromKeys` | `bc_count_range` over the access path's range |
| `COUNT`, `SUM` otherwise | `Aggregate` | an accumulator local, `bc_add`; a "seen" `bool` for SUM's NULL |
| `MIN` otherwise | `Aggregate` | a local holding the least non-NULL value, kept by comparison: `<` on integers, on doubles `<` with NaN taken as greater than every other value (`docs/types_design.md`, *Double arithmetic*); a "seen" `bool` for the NULL. Never a bytes column (*The dialect*) |
| `MIN(col)`, `col` an `ASC` part directly after the equality prefix, no residual | `MinFromFirstKey`, its `Scan` forward with `rest` = `PresentOnly` when `col` is nullable | the first entry of the scan, which starts past the NULL part when it is `PresentOnly`, then `break`. A `DESC` part (its first entry is the maximum) takes the `Aggregate` row |
| `COUNT(DISTINCT col)` | `Aggregate` | `bc_agg_group` keyed by the column, then a count of `bc_agg_next` |
| A join | `from` = outer, inner `Loop`s | the inner key built from the outer row: `bc_key_*` of `bc_col_*`; nested in the outer loop |
| `FOR UPDATE` | `Rows::for_update` | `bc_guard_row` on each returned row: `bc_guard_unchanged` on its primary key |
| Targets | `Rows::targets`, or `out` | `bc_emit_col`, `bc_emit_key_part`; for an aggregate, `bc_emit_i64` (`COUNT`, `SUM`, and `MIN` of an integer, `DECIMAL`, `DATE`, `TIMESTAMP` or `TIME`), `bc_emit_f64` (`MIN` of a `REAL` or `DOUBLE PRECISION`), and `bc_emit_null` over no rows |

A NULL that a key needs means no row can match, since `=` and the range
operators reject NULL:
- **a parameter:** the plan ends early, as today, before any read;
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
    column read, as PostgreSQL's operator takes its result type from its
    operands; the assigned column only checks the stored sum. The binder takes
    a BIGINT and a `DECIMAL` of scale 0 as one type, the only crossing: a
    `DECIMAL(p, 0)` read cannot leave `int64_t`, its operand held to 18
    digits, so its overflow is the assigned column's ("numeric field
    overflow"). `test_column_plus_parameter_across_bigint_and_scale_0_decimal`
    runs both crossings against PostgreSQL.
  - *`$n ± k` in a `WHERE`:* `$n` is checked as an `INTEGER` before the plan
    runs, so `bc_add` cannot overflow `int64_t`
    (`test_a_computed_bound_past_integer_is_22003_before_the_plan_runs`).

`COUNT(DISTINCT)` uses the aggregate structure `bc_plan.h` declares
(`bc_agg_open` with no slots, `bc_agg_group`, `bc_agg_next`).

### The planner's choices

All deliberately naive: rules, no cost model, chosen so every TPROC-C
statement gets the plan a person would write.

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
   remain. None remaining: refused (*What the planner refuses*, row 5).
3. Of those, the one with the longest leading run of equality-compared
   parts wins; at equal runs, one with a range part after the run beats
   one without. Remaining ties go to the primary key, then to the covering
   path, then to the lower id.
4. Then the chosen path's NULLs are checked against PostgreSQL's
   placement (below). One it can't place is refused (row 6); the planner
   doesn't go back to try another path. This is deliberately naive.

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
  `NullSplitScan` (*The statement IR*), the present values and the NULL
  part, in PostgreSQL's order, with one `LIMIT` across both. Ordered parts
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
starts past the NULL part (`PresentOnly`). Otherwise the scan runs to its
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
reverse, on a single table. Sorters (`bc_sorter_*`) are #533.

**Joins.** Index nested loop only, two tables only.
- The outer table is the one reachable from parameters alone, a point
  lookup over a scan. The inner one must be reachable from the outer row's
  columns and parameters, by a point lookup or a prefix.
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

#### What the planner refuses

The planner refuses a statement inside the subset that it can't answer
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

The refusal is a SQL error with SQLSTATE 0A000 (`feature_not_supported`),
raised after binding and before anything runs. It carries no position: the
planner works on the bound statement, which keeps none. PostgreSQL runs
each of these; it is the one entry of *Known differences* that is a
feature gap.


## Tests

- **Golden plans.**
  - Statements are compiled against the schema fixture plus
    `tests/golden/schema.sql`, and each statement's text and plan are
    checked in as `tests/golden/<statement>.sql` and `.c`.
  - A test recompiles each one, fails on any difference, and compiles each
    plan with TinyCC.
  - A change to the code generator regenerates them with
    `SQL_LAYER_REGEN_GOLDEN=1` (`tests/golden_test.cpp`), and the PR shows
    the diff.
  - Every TPROC-C statement has its golden plan.
- **Refusals.** Refusals of what the dialect leaves out are tested with
  their message and position (`binder_test`). Each row of *What the
  planner refuses* is tested with its message.
- **Against PostgreSQL.** The proof machine generates the dialect's
  statements and compares them with PostgreSQL (`testing_design.md`). It
  generates only statements the planner serves, so it never compares a
  planner refusal with PostgreSQL. `test_tprocc.py` runs the five TPROC-C
  transactions against PostgreSQL statement by statement.

## Known differences from PostgreSQL

The proof machine expects each one, or doesn't generate it, so it doesn't
fail on them.

- **A table needs a primary key.** The layer refuses `CREATE TABLE` without
  one; PostgreSQL accepts it. The machine sends that variant to the layer
  only and expects the refusal.
- **A VARCHAR(n) value whose excess is only spaces is refused.**
  PostgreSQL truncates it to n characters and stores it; the layer refuses
  it as too long (22001). The machine doesn't generate one.
- **A DEFAULT outside its column's type is refused by the DDL.** The layer
  refuses `CREATE TABLE` (and `ADD COLUMN`) with, say, `SMALLINT DEFAULT
  100000`, `REAL DEFAULT 1e39` or `VARCHAR(2) DEFAULT 'abc'`. PostgreSQL
  accepts the DDL and refuses each INSERT that takes the default (22003,
  22001; checked on 16.15). A DEFAULT that is not UTF-8 never reaches
  either: PostgreSQL refuses the statement's text (22021). The machine's
  defaults stay inside their types.
- **Comparing columns of different SQL types is refused,** in a `WHERE` and
  in a join; PostgreSQL casts one to the other's. One key encoding serves
  each comparison. TPROC-C compares columns of one type only.
- **A parameter used with columns of two SQL types is refused** (*Binding*).
  `WHERE a = $1 AND b = $1`, with `a DOUBLE PRECISION` and `b REAL`,
  `INTEGER` and `BIGINT`, or `DECIMAL`s of two scales, is refused, and so
  is `SET b = $1, a = a + $1` with `b BIGINT` and `a SMALLINT`. PostgreSQL
  runs both.
  - Refusing is the safe choice. With `$1 = 0.1`, PostgreSQL compares
    `real::float8 = 0.1`, which is false, where a `REAL` key part keyed as
    the float of 0.1 would match. A range check per comparison wouldn't
    close that.
  - The machine gives each column its own parameter
    (`test_a_parameter_compared_with_two_types_is_refused`,
    `test_a_parameter_used_with_two_types_is_refused`; the DECIMAL scales,
    which the oracle doesn't model, in the embedded API's test).
- **A DECIMAL operand of `col ± $n` past 18 digits is 22003.** The layer's
  DECIMAL is a scaled 64-bit integer (`types_design.md`), so the operand is
  checked against 18 digits at the column's scale before the plan runs,
  even when the column is NULL. PostgreSQL's unconstrained `numeric` may
  still complete (NULL plus anything is NULL). The machine's operands stay
  inside 18 digits.
- **A DECIMAL literal operand of `col ± n` past 18 digits once scaled is
  refused at bind** ("numeric field overflow"), whatever the column holds.
  PostgreSQL computes the sum and fails only when it doesn't fit the
  column. The machine's literals stay inside 18 digits.
- **A SUM whose running total leaves `int64_t` fails** (`BC_E_ARITH`). The
  layer adds in 64 bits, a BIGINT's or a DECIMAL's scaled integer, and the
  statement fails as 22003; PostgreSQL's `SUM` of a BIGINT or NUMERIC is an unbounded `numeric`. Below that the
  sums agree exactly (`test_sum_past_int64_is_an_error`). The machine sums
  no BIGINT column; no TPROC-C sum comes near.
- **MIN of a REAL or DOUBLE PRECISION column is refused.** PostgreSQL's
  `MIN` keeps the last of `-0.0` and `0.0` it meets, so its answer depends
  on the order it reads the rows. The machine takes no `MIN` of a float.
- **A unique value an UPDATE moves from one row to another is a
  duplicate.** With a unique index on `u` and rows holding 1 and 2, `SET u
  = u + 1` over both is refused (23505), whatever order the rows are
  written in. Each row's new value is checked against the snapshot, where
  the other row still holds it. PostgreSQL checks each row as it writes it,
  so its outcome depends on the order it visits them. Two rows given one
  value are 23505 on both
  (`test_one_unique_value_given_to_two_rows_is_23505`). The machine adds to
  no column a unique index holds.
- **A statement no index scan answers in its order is refused** (*What the
  planner refuses*): a full scan, a sort of the rows read, an `ORDER BY` on
  a join, or one whose NULLs the chosen index can't place as PostgreSQL
  does. PostgreSQL runs it. This is the one feature gap in the list; #533
  closes it.

Each is a refusal or an error, not a different answer. Everything else in
the dialect matches PostgreSQL, and the proof machine checks that it does.

## Decisions

The owner's decisions on the front end (2026-10-21):
- **The range check on computed values** is in the plan API, raising 22003
  as PostgreSQL's operators do, so no key is built from a value its column
  can't hold (*The range check on computed values*).
- **The planner is rule-based, and refuses full scans and sorts no index
  provides.** Both are superseded by `planner_design.md`: a cost-based
  search, with sorts and full scans (#533).

The decisions on the TPROC-C statements are in `testing_design.md`, *The
TPROC-C statements*. The decisions on the gate are in `server_design.md`.
