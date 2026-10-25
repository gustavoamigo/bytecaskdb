# The SQL front end: parsing, the dialect and binding

Status: **built.** This covers what turns SQL text into a statement the
planner can take: the parser, the dialect the layer accepts, and the binder,
which resolves names and types. It also covers the layer's known
differences from PostgreSQL in what SQL means. The planner is
`planner_design.md`. The TPROC-C statements the layer runs are in
`testing_design.md`, *The TPROC-C statements*.

## Standing rules

- **Do what PostgreSQL does.** Every accepted construct has PostgreSQL 17's
  semantics, and the proof machine compares it with PostgreSQL.
- **Never wait.** Nothing here blocks. `FOR UPDATE` is a guard, checked at
  commit (`transaction_design.md`).
- **Only the compiler's plans are supported.** The planner and the code
  generator are the only source of plan text.
- **Refuse rather than guess.** A statement outside the dialect is refused
  at parse or bind, with a message and a position. One inside it that the
  planner can't plan is refused by the planner (`planner_design.md`,
  *What the search refuses*). Either way, nothing has run.

## Parsing

libpg_query 17 parses the text into PostgreSQL's parse tree, read as
protobuf-c, and a transformer turns it into the layer's AST
(`sql/parser.cppm`, `sql/ast.cppm`). The dialect is PostgreSQL's. A node
kind the transformer doesn't take is refused with its position.

## The dialect

**Statements.**
- `CREATE TABLE`, `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX` and
  `ALTER TABLE … ADD COLUMN` (`catalog_design.md`, *DDL*). A table
  declared without a primary key keeps every row, duplicates included,
  under a hidden row id no statement sees (`catalog_design.md`, *Tables
  without a primary key*).
- `INSERT … VALUES` of one row, with or without a column list.
- `SELECT` targets: columns, `COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT
  col)`, `SUM(col)`, `MIN(col)`, `MAX(col)` and `AVG(col)`, several of
  them in any order.
- `GROUP BY` of columns, and `HAVING`, below. A query with a `GROUP BY`, a
  `HAVING` or an aggregate is aggregated; without `GROUP BY` it returns
  exactly one row, or none where its `HAVING` is false.
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
- `ORDER BY` takes a list of columns, each `ASC` or `DESC`, and `NULLS
  FIRST` or `NULLS LAST`; without them NULLs sort as PostgreSQL's
  defaults put them, last under `ASC` and first under `DESC`. Ties come
  back in any order, as PostgreSQL allows. `CREATE INDEX` takes neither.
- `LIMIT` takes an integer literal or a parameter. `LIMIT NULL` is no
  limit, and a negative limit is refused (2201W), as in PostgreSQL.

**Aggregates**, with PostgreSQL's results:
- `COUNT(*)` counts rows, `COUNT(col)` non-NULL values, `COUNT(DISTINCT
  col)` distinct non-NULL values. All three are `bigint`, and 0 over no
  rows.
- `SUM` over `SMALLINT` or `INTEGER` is `bigint`. Over `BIGINT` or
  `DECIMAL` it is `numeric`, kept at the input's scale. It is NULL over no
  rows.
- `AVG` over an integer or `DECIMAL` column is `numeric`: PostgreSQL's
  `numeric_div` of the sum by the count, at the scale its
  `select_div_scale` chooses (at least 16 significant digits), rounded
  half away from zero. Its digits can pass 64 bits, so the plan emits the
  sum and the count and the API divides them (`average_text`); a result
  row carries the value as its text. NULL over no rows.
- `MIN` and `MAX` are of the column's type, and NULL over no rows. They
  take a numeric or temporal column.
  - Of a `TEXT`, `VARCHAR` or `BYTEA` column they are refused (#534).
    Keeping the least value seen needs a buffer the plan API doesn't have.
  - Of a `REAL` or `DOUBLE PRECISION` column they are refused too (*Known
    differences*), as are `SUM` and `AVG` of one (42601).

**`GROUP BY` and `HAVING`** (`planner_design.md`, *Aggregates*):
- `GROUP BY` takes a list of columns of the statement's tables, of any type.
  NULLs form one group, and a DOUBLE's `-0.0` and `0.0` one (its row shows
  the value of the group's first row, as PostgreSQL's hashed aggregate
  does). No expressions, `GROUPING SETS`, `ROLLUP`, `CUBE` or `GROUP BY
  DISTINCT`.
- A column the select list, `ORDER BY` or `HAVING` names must be grouped,
  or of a table whose whole primary key is grouped: PostgreSQL's
  functional dependency. Every row of a group holds the same value of such
  a column, so the binder lists it apart (`Aggregate::dependent`), read
  from the group's rows, and the `GROUP BY` stays as written, so an
  index's order can still stream the groups. Anything else is 42803,
  PostgreSQL's message.
- `HAVING` is a conjunction of comparisons of a grouped column or an
  aggregate with a parameter. The parameter takes the column's type, or the
  aggregate's: `COUNT`'s and an integer `SUM`'s `bigint`, a `SUM` of a
  BIGINT or DECIMAL its numeric, checked as a BIGINT at its scale, `MIN`'s
  and `MAX`'s their column's. A parameter an earlier use typed, in the
  `WHERE` or an earlier conjunct, keeps that type, as PostgreSQL types a
  `$n` by its first use: `WHERE a > $1 GROUP BY a HAVING count(*) > $1`,
  `a` an INTEGER, compares `count(*)` with an INTEGER `$1`. The layer runs
  it where both are integers of scale 0 (SMALLINT, INTEGER, BIGINT, a
  DECIMAL of scale 0), which compare exactly; any other pair is refused, as
  two uses of one `$n` are (*Known differences*). `HAVING` on `AVG` is
  refused (42601): its quotient is no type a plan holds.
- `AVG`'s result is PostgreSQL's numeric of no declared precision, a SQL
  type of its own (`SqlKind::Numeric`): no column has it, and a result row
  carries its value as its text.
- `ORDER BY` takes grouped columns; it sorts the groups, and `LIMIT` counts
  them.

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
- `DISTINCT` on the select list, `GROUP BY` of an expression, and `HAVING`
  of anything but the comparisons above;
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
position, and the SQLSTATE PostgreSQL refuses the same statement with, or
42601 for a form the layer does not support (`server_design.md`, *Errors*).
The planner's refusals are `FeatureNotSupported`, 0A000 (`planner_design.md`, *What the
search refuses*).

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
  their message and position (`binder_test`). Each row of
  `planner_design.md`'s *What the search refuses* is tested with its
  message.
- **Against PostgreSQL.** The proof machine generates the dialect's
  statements and compares them with PostgreSQL (`testing_design.md`). It
  generates only statements the planner serves, so it never compares a
  planner refusal with PostgreSQL. `test_tprocc.py` runs the five TPROC-C
  transactions against PostgreSQL statement by statement.

## Known differences from PostgreSQL

The proof machine expects each one, or doesn't generate it, so it doesn't
fail on them.

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
  runs both. A `HAVING` comparing an integer with a `$n` an earlier use
  typed as an integer runs, as PostgreSQL's (*GROUP BY and HAVING*); with
  a `$n` the `WHERE` typed as a `DECIMAL` of scale 2, `HAVING count(*) >
  $1` is refused, where PostgreSQL casts the count to numeric.
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
- **A SUM or an AVG whose running total leaves `int64_t` fails** (`BC_E_ARITH`). The
  layer adds in 64 bits, a BIGINT's or a DECIMAL's scaled integer, and the
  statement fails as 22003; PostgreSQL's `SUM` of a BIGINT or NUMERIC is an unbounded `numeric`. Below that the
  sums agree exactly (`test_sum_past_int64_is_an_error`). The machine sums
  no BIGINT column; no TPROC-C sum comes near.
- **MIN and MAX of a REAL or DOUBLE PRECISION column are refused.**
  PostgreSQL's `MIN` keeps the last of `-0.0` and `0.0` it meets, so its
  answer depends on the order it reads the rows. The machine takes no `MIN`
  or `MAX` of a float.
- **A HAVING parameter compared with the SUM of a BIGINT or DECIMAL is
  checked as a BIGINT** at the column's scale before the plan runs: a value
  past it is 22003, where PostgreSQL compares it with the numeric sum. The
  machine's values stay inside it.
- **A unique value an UPDATE moves from one row to another is a
  duplicate.** With a unique index on `u` and rows holding 1 and 2, `SET u
  = u + 1` over both is refused (23505), whatever order the rows are
  written in. Each row's new value is checked against the snapshot, where
  the other row still holds it. PostgreSQL checks each row as it writes it,
  so its outcome depends on the order it visits them. Two rows given one
  value are 23505 on both
  (`test_one_unique_value_given_to_two_rows_is_23505`). The machine adds to
  no column a unique index holds.
- **A filter on a DOUBLE is refused** (`planner_design.md`, *What the search refuses*): a filter on a REAL or DOUBLE
  PRECISION column no key takes. PostgreSQL runs it. Everything else in
  the dialect is planned: full scans, sorts and joins no key reaches
  included.
- **A sort, or a structure of groups, past the statement's memory budget
  fails** with 54000, where PostgreSQL spills it to disk
  (`planner_design.md`, *Memory*).
- **A filter on a grouped DOUBLE or REAL column in HAVING is refused**
  (`no plan:`), as a `WHERE`'s is.

Each is a refusal or an error, not a different answer. Everything else in
the dialect matches PostgreSQL, and the proof machine checks that it does.

## What the layer doesn't do yet

Gaps, not decisions: PostgreSQL runs each, and the layer refuses it until
it is built. The functional suite marks the cases that need one as not
implemented (`functional_testing_design.md`).

- **A table needs columns.** The layer refuses `CREATE TABLE t ()` (0A000);
  PostgreSQL accepts a table of no columns.

## Decisions

The owner's decisions on the front end (2026-10-21):
- **The range check on computed values** is in the plan API, raising 22003
  as PostgreSQL's operators do, so no key is built from a value its column
  can't hold (*The range check on computed values*).
- **The planner was rule-based, and refused full scans and sorts no index
  provided.** Superseded by `planner_design.md`: a cost-based search,
  which plans both.

The decisions on the TPROC-C statements are in `testing_design.md`, *The
TPROC-C statements*. The decisions on the gate are in `server_design.md`.
