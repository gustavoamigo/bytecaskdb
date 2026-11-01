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
  without a primary key*). `UNIQUE` on a column or on the table, named or
  not, is a unique index created with the table (`catalog_design.md`,
  *UNIQUE constraints*). `REFERENCES` on a column and `FOREIGN KEY … REFERENCES`
  on the table, named or not, `ON DELETE` and `ON UPDATE` `NO ACTION` or
  `RESTRICT`, are checked as PostgreSQL checks them when it creates them:
  the referenced table and columns exist, and are its primary key (the
  default) or a unique index's columns in any order, of types it compares
  (an INTEGER references a DECIMAL or a REAL key, not the reverse), with
  its SQLSTATE where they are not. Then nothing is kept: the constraint is
  not enforced (*Known differences*). `CASCADE`, `SET NULL`, `SET DEFAULT`,
  `MATCH FULL` and `PARTIAL`, `DEFERRABLE` and `NOT VALID` are refused.
- `INSERT … VALUES` of one row, with or without a column list, of
  parameters and constants.
- `SELECT` targets: columns, `*` and `table.*` (below), constants (*Literals*), `COUNT(*)`,
  `COUNT(col)`, `COUNT(DISTINCT col)`, `SUM(col)`, `MIN(col)`, `MAX(col)` and
  `AVG(col)`, several of them in any order; a constant not beside an
  aggregate or a `GROUP BY`.
- `SELECT *` and `table.*` are expanded by the binder, before it binds
  anything else, into the columns they stand for: each FROM table's in
  table order, hidden row ids left out, each named by its column's name.
  Over a `USING` or `NATURAL` join `*` gives the `USING` columns first,
  each once and named alone (so the preserved side's, as a column named
  alone is), then the left table's other columns, then the right's; a
  `table.*` is that table's columns, `USING` ones included. The
  expansion is a select list of ordinary columns, so everything after it
  (aggregation, `DISTINCT`, `ORDER BY`) treats it as the columns written
  out. `*` without `FROM` is 42601, `x.*` for a name not in `FROM` 42P01,
  `x.*` of a table an alias hides 42P01; `*` anywhere but the select list
  is 42601, `* AS (c, d)` is not PostgreSQL and is a syntax error.
- `GROUP BY` of columns, and `HAVING`, below. A query with a `GROUP BY`, a
  `HAVING` or an aggregate is aggregated; without `GROUP BY` it returns
  exactly one row, or none where its `HAVING` is false.
- `FROM`: one table, or two joined by a comma, `[INNER] JOIN … ON`, `JOIN
  … USING (cols) [AS alias]` or `NATURAL JOIN`. Columns may be qualified by
  the table's name. `USING` and `NATURAL` (every column name both tables
  share) compare their columns by `=`, as `ON` would, and name each alone
  as one column, the left table's, equal to the right's in every pair; the
  `USING` alias qualifies those columns only. A name missing from a side is
  42703, one named twice 42701, as in PostgreSQL. `LEFT [OUTER] JOIN` and
  `RIGHT [OUTER] JOIN` take each of these forms: every row of the preserved
  table, joined to each row of the other that `ON` matches, or to a row of
  NULLs where it matches none. `ON` decides the pairs and `WHERE` filters
  the joined rows, so `WHERE r.k IS NULL` keeps the rows nothing matched; a
  `USING` column named alone is the preserved table's. `FULL JOIN` is
  refused. A table takes an alias, `t [AS] x`: the front end rewrites the
  qualifier `x` to the table in every clause of that SELECT, so the binder
  never sees an alias, and the table's own name qualifies nothing while it is
  aliased (42P01, as PostgreSQL), nor may two FROM items share a name
  (42712). A select-list item takes `[AS] name`, which names its output
  column; an unqualified `ORDER BY name` that is such a name orders by that
  item (PostgreSQL looks at output names first there, at input columns
  first in `GROUP BY`, which takes no output name here). A subquery
  resolves its own aliases; it cannot name the outer query's (correlation
  is refused). `t AS x (c1, c2)`, an alias of a join, and a table joined
  with itself under two aliases are refused. Without `FROM`, a
  `SELECT` of constants returns one row of them, or none under `LIMIT 0`;
  it takes no `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY` or `FOR UPDATE`
  yet.
- `UPDATE` and `DELETE` of any number of rows.
- `ORDER BY`, `LIMIT` and `FOR UPDATE`, below.

**Literals and expressions.**
- **Literals** of every kind, typed and read as PostgreSQL types them
  (*Literals*, below).
- **`*` and `/`** fold constants only, as `+` and `-` do, and are refused
  on a column (42601, not supported yet): the select list holds no
  arithmetic of columns. Integers take PostgreSQL's types (`integer *
  integer` is `integer`, any wider operand makes `bigint`), the product
  of numerics is at the sum of the scales within 18 digits, and the
  result is range-checked (22003, `INT_MIN / -1` included). Integer `/`
  truncates toward zero. A zero divisor is 22012, unless the dividend is
  `NULL` (the operator is strict and never runs). A numeric quotient is
  refused: its scale is PostgreSQL's division rule (`select_div_scale`),
  not built yet. `%` is refused.
- **`CASE`, `COALESCE` and `NULLIF`** fold constants only (of a column,
  a parameter or a subquery they are refused, 42601, with `*` and `/`
  of columns, #667). The result type is PostgreSQL's union rule: unknown
  literals give way to the typed branches, which must be of one class
  (42804, `CASE types … cannot be matched`); integers widen to the
  widest; a numeric branch is `numeric` and keeps its own scale
  (`CASE WHEN true THEN 1 ELSE 2.50 END` is `1`); all unknown is `text`.
  An unknown literal that is not a value of the type is 22P02, in a
  branch not taken too, as PostgreSQL checks types before it runs. A
  `WHEN` of a searched `CASE` must be `boolean` (42804); `NULL` is not
  true. A simple `CASE x WHEN v` tests `x = v` as a comparison of
  constants does (42883 where PostgreSQL has no such operator). `NULLIF(a, b)`
  is `a`, of `a`'s type (an unknown `a` takes `b`'s), unless `a = b`.
  Conditions and results are evaluated in order and only as far as the
  answer needs: a `WHEN` after the first true one is never evaluated,
  and a condition or the taken result that raises (22012, 22003) raises.
  A result not taken that raises is refused (42601): its type is not
  known without evaluating it. Float branches are refused. The output
  column is named `case`, `coalesce` or `nullif`.
- **`||` and the string functions** fold constants only (of a column, a
  parameter or a subquery they are refused, 42601, with the other
  column-valued expressions, #667). The parser reads PostgreSQL's
  spellings as the calls the grammar makes of them: `TRIM([LEADING |
  TRAILING | BOTH] [chars FROM] s)` is `ltrim`, `rtrim` or `btrim` (spaces
  by default), `POSITION(sub IN s)` is `position(s, sub)`, `SUBSTRING(s
  FROM a [FOR n])` is `substring(s, a, n)`, `OVERLAY(s PLACING r FROM a
  [FOR n])` is `overlay`; `CHAR_LENGTH`, `CHARACTER_LENGTH`, `OCTET_LENGTH`,
  `UPPER`, `LOWER` and the function-call forms of the others are taken
  as written. Text is UTF-8 and positions and lengths count characters.
  - Arguments resolve as PostgreSQL's overloads do: an unknown literal
    is of the parameter's type (`SUBSTRING('abc' FROM '2')` reads `'2'`
    as an integer, 22P02 if it is not one), `smallint` and `integer` fit
    an `integer` parameter, and any other type matches no function
    (42883, `function upper(integer) does not exist`; so does a wrong
    number of arguments). A NULL argument gives NULL after the types are
    checked, as the functions are strict.
  - `a || b`: strings, or a string with any other type through its text
    (`'a' || 1` is `a1`, `'a' || true` is `atrue`); two values neither of
    which is a string match no operator (42883).
  - `UPPER` and `LOWER` fold ASCII letters only, the layer's one
    collation being `COLLATE "C"`.
  - `SUBSTRING(s FROM a FOR n)` is PostgreSQL's: a start before the first
    character takes from it, the characters before it counting toward
    `n`; `n < 0` is 22011; a start or length past `integer` is 42883
    (`bigint`) and `a + n` past 2^31 − 1 reads to the end. `OVERLAY` with
    a start below 1 is 22011, and a resume position past `integer` is
    22003.
  - `POSITION` of an empty substring is 1, of one not found 0.
  - The output column is named as PostgreSQL names it: `?column?` for
    `||`, otherwise the function (`btrim` for `TRIM`).
  - Refused (42601): `SUBSTRING(s FROM 'pattern' [FOR 'escape'])`, the
    regular-expression form PostgreSQL picks when no operand is an
    integer; `bytea` operands but for `bytea || bytea`; a float operand
    of `||`; a schema other than `pg_catalog`. `USING CHARACTERS` and
    `USING OCTETS` are not PostgreSQL syntax (42601).
- **`+` and `-`** on integers and on `DECIMAL`, with PostgreSQL's result
  type: `smallint + smallint` is `smallint`, `integer + bigint` is
  `bigint`, and so on.
  - `DECIMAL ± DECIMAL` needs equal scales; others are refused.
  - The result is checked against its type at the operation, as
    PostgreSQL's operators check it: `integer out of range`, `smallint out
    of range`, `numeric field overflow`, all SQLSTATE 22003.
  - An expression involving NULL is NULL.
- **Where expressions may appear:** `SET col = expr`, a comparison's
  operand (`ol_o_id >= $3 - 20`), and `VALUES`. A constant expression may
  also be a select-list target and a `HAVING` operand.

**`WHERE`** is a conjunction (`AND`) of comparisons (`=`, `<`, `<=`, `>`,
`>=`) of a column with a parameter, a constant or `$n ± k`, in any order,
on any columns, and of predicates over them.
- A comparison with NULL is not true, so it rejects the row, as in SQL's
  three-valued logic.
- **Predicates:** `AND`, `OR` and `NOT` of comparisons and of `col IS [NOT]
  NULL`, in SQL's three-valued logic: `NOT` of unknown is unknown, `AND` is
  false when one side is, `OR` true when one side is. The parser writes the
  rest with them: `a <> b` is `NOT (a = b)`; `a BETWEEN lo AND hi` is `a >=
  lo AND a <= hi`, `SYMMETRIC` either way round, `NOT BETWEEN` its `NOT`; `a
  IN (x, y)` is `a = x OR a = y` and `NOT IN` its `NOT`, so a NULL in the
  list makes `NOT IN` true of no row, as in PostgreSQL.
- The `AND`s at the top, a `BETWEEN`'s included, are separate conjuncts, so
  a key takes each comparison it can. A predicate (`OR`, `NOT`, `IS NULL`)
  is a filter, checked on each row a plan reads, never a key's bound
  (`planner_design.md`, *The search*).
- **`LIKE`**: `col [NOT] LIKE pattern [ESCAPE 'c']`, of a TEXT or VARCHAR
  column and a parameter or constant pattern, typed TEXT as PostgreSQL
  types it; a predicate leaf, matched by `bc_like` (`plan_api_design.md`):
  `%` any run, `_` one character, case-sensitive and bytewise (`COLLATE
  "C"`). The escape is a backslash, `ESCAPE`'s one character, or none for
  `ESCAPE ''`; a longer one is 22025, as is a pattern the match finds ending
  in its escape. A number column is 42883.
- **Subqueries**, uncorrelated, in a `SELECT`'s and in an `UPDATE`'s or
  `DELETE`'s `WHERE`: `col op (SELECT …)` of one value, `[NOT] EXISTS (SELECT
  …)`, `col [NOT] IN (SELECT …)` and `col op ANY | SOME | ALL (SELECT …)`; and
  `(SELECT …)` in a select list. Each runs once, before the statement's own
  plan, on its snapshot (`planner_design.md`, *Subqueries*). Its semantics
  are PostgreSQL's: a scalar subquery of no row is NULL, of two or more
  21000; `IN` is `= ANY` and `NOT IN` its `NOT`; `ANY` is true where a row
  makes the comparison true, else NULL where a row is NULL (or the column
  is, over some rows), else false, over no rows too; `ALL` is `NOT ANY` of
  the negated operator, so true over no rows. A subquery selects one column,
  but for `EXISTS` (42601 otherwise), of a type the column compares with:
  integers with integers, a DECIMAL of its scale, strings with strings, any
  other type its own. A scalar one is a parameter to the plan, so
  `id = (SELECT MAX(k) FROM u)` is a key lookup.
- Not yet: a predicate comparing a REAL or DOUBLE column (`no plan:`, as
  any filter on one), `$n ± k` inside one, one of both tables' columns in a
  join, in `HAVING`, `ILIKE` (its case folding is a locale's), `LIKE` of a
  BYTEA column, `IS DISTINCT FROM`; a subquery that reads a column of the
  query around it (correlated), one inside another, in `FROM`, `FOR UPDATE`
  in one, one of `AVG`, assigned by `SET` or `VALUES`, beside an aggregate,
  or compared with other than a column.
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

**`SELECT DISTINCT`** is a `GROUP BY` of the selected columns, so it
returns one row per distinct combination, NULL one value and a DOUBLE's
`-0.0` and `0.0` one, as the grouping below makes them. Its `ORDER BY` must
name selected columns (42P10) and it takes no `FOR UPDATE` (0A000), as in
PostgreSQL. Over a query already aggregated it is nothing more where every
`GROUP BY` column is selected, or where there is no `GROUP BY`; otherwise,
and with a constant selected or `DISTINCT ON`, it is not taken yet.

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
- `ILIKE`, `IS DISTINCT FROM`, a predicate over both tables of a join or
  in `HAVING`;
- correlated and nested subqueries and the others above, derived tables,
  set operations;
- `DISTINCT ON`, `DISTINCT` of a constant or over groups it does not select
  every key of, `GROUP BY` of an expression, and `HAVING` of anything but
  the comparisons above;
- `FULL JOIN`, an outer join's `ON` comparing an expression of columns,
  three or more tables, renaming a table's columns (`t AS x (a, b)`), an
  alias of a join, an output name in `GROUP BY` or in `ORDER BY` of other
  than a column;
- `OFFSET`, `RETURNING`, multi-row `VALUES`;
- `CASE`, `COALESCE` and `NULLIF` of columns, parameters and subqueries, a
  `CASE` result that is a float, a `WHEN` that is not a comparison
  (`AND`, `OR`, `IS NULL`, `BETWEEN` as a value), `GREATEST` and `LEAST`;
- `*` and `/` of columns, a numeric quotient, `%`, `||` and the string
  functions of columns and parameters, `LENGTH`, `CONCAT`, `REPLACE`, the
  regular-expression `SUBSTRING`, `bytea` string functions (#667, #671);
- a cast of anything but a string literal or `NULL`, a numeric literal of
  more than 18 digits, a string literal in arithmetic, unary `-` of
  anything but a constant, a comparison of a column as a value, and a
  constant beside an aggregate.

The list and its order of work are #534.

### Literals

A literal is a constant of the statement, folded when it is bound, so a
plan never holds one: each becomes a parameter the API binds after the
statement's own (`bound::Bound::fixed`), which no client is told of or
sends. A plan is compiled for its text, so the values are the text's; a
recompile after a DDL folds them again, to the types they meet then.

**Typed as PostgreSQL types them.**
- An integer literal is `integer`, or `bigint` past it. A number with a
  point or an exponent is `numeric` at the scale it is written with (`1.50`
  at 2, `.2E+2` at 0, `2.2E-2` at 3), held as a `DECIMAL` of no declared
  precision within 18 digits. `TRUE` and `FALSE` are `boolean`.
- A string literal and `NULL` have no type yet. PostgreSQL types one by
  where it is used and reads a string with that type's input function: the
  same reading a parameter's text gets on the wire, from one module
  (`sql/text.cppm`). Alone in a select list each is `text`.
- Unary `+` and `-`, and `+`, `-`, `*` and `/` of constants, fold with
  PostgreSQL's result types and range checks: `2147483647 + 1` is "integer out of
  range" (22003), `1 + 1.5` the numeric 2.5, `1 + NULL` an integer NULL.
- A comparison of constants (`=`, `<>`, `<`, `<=`, `>`, `>=`) folds to a
  `boolean`, NULL if either side is: numbers exactly whatever their
  scales, a float with NaN above every value, strings and bytes bytewise
  (*Known differences*), `FALSE` before `TRUE`, dates and times of one
  type by value. A string literal beside a typed constant is read as that
  constant's type (`1 < '2'`); two compare as text. Types PostgreSQL has no
  operator for are 42883; a date beside a timestamp is not taken yet.
- A cast of a string literal or `NULL`: `DATE '…'`, `'…'::date` and
  `CAST('…' AS date)` alike read the string with the type's input function,
  a `DECIMAL(p, s)` rounded to its scale and range-checked (22003), and are
  of that type. A cast of anything else, and to `VARCHAR(n)`, which
  PostgreSQL's cast truncates, are not taken yet.
- A select-list constant is named `?column?`, a cast by its type's name
  (`date`, `int4`, `numeric`), as PostgreSQL names them.

**Where it meets a column.**
- **Assigned** (`VALUES`, `SET`): PostgreSQL's assignment casts. A numeric
  is rounded half away from zero to an integer column or a `DECIMAL`'s
  scale (`2.5` stores 3), a number or a boolean assigned to a string column
  is its text (`2.2E-2` stores `0.022`), a REAL is read once as a float.
  A type PostgreSQL does not assign is 42804. The value's range is checked
  when the statement is compiled, as PostgreSQL checks it when it plans
  it, whether or not a row is written; of several out of range, the first
  in the statement's order is the error, as there (a too-long string
  before a later column's numeric overflow).
- **Compared** (`WHERE`, `HAVING`): only a constant the column's type holds
  exactly. `smallint_col = 7.0` is `= 7`; `smallint_col = 40000`,
  `dec2_col = 1.005` and `real_col = 0.1` are refused (*Known differences*).
  A string literal is read as the column's type: `int_col = '7'` is `= 7`,
  `int_col = '1.5'` is 22P02. An integer compared with a string column is
  42883, as in PostgreSQL.

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
- **A constant a column's type does not hold exactly is refused in a
  comparison** (42601): `smallint_col = 40000`, `int_col = 1.5`,
  `dec2_col = 1.005`, a REAL compared with a number that is no float, a
  `DECIMAL` compared with one past 18 digits at its scale. PostgreSQL
  compares in a wider type and answers. The layer compares each column in
  its own key encoding, so it would have to rewrite the comparison
  (`= 40000` is never true, `< 40000` is `IS NOT NULL`); it refuses rather
  than guess. The machine expects the refusal (`oracle.exact_literal`).
- **A UNIQUE constraint's index can be dropped.** The layer keeps no
  constraint object, so `DROP INDEX` of a `UNIQUE` constraint's index drops
  it, and the constraint with it. PostgreSQL refuses (2BP01: the constraint
  requires the index) and drops it only with its table or `ALTER TABLE …
  DROP CONSTRAINT`.
- **A FOREIGN KEY is not enforced.** It is checked when its table is
  created, then dropped: a row whose key no parent row holds is written,
  a parent row with children can be deleted or have its key changed, and
  the parent table or the index it references can be dropped. PostgreSQL
  refuses each (23503, 2BP01). The owner postponed enforcement, as several
  databases have shipped without it; its design is in #657.
- **Strings compare bytewise, as `COLLATE "C"`**, constants as columns
  do: `'B' < 'a'` is true. PostgreSQL orders them by the database's
  collation, under which `'a' < 'B'` may be true instead. The layer has one
  collation (`types_design.md`).
- **A constant's error is raised when the statement is compiled**: `SELECT
  2147483647 + 1` and `UPDATE … SET smallint_col = 40000` fail at prepare
  (the wire's Parse), with PostgreSQL's SQLSTATE. PostgreSQL folds the same
  constants when it plans the statement, at Bind for a statement Parse left
  unplanned.
- **A `CASE` or `COALESCE` result that would raise, and is not taken, is
  refused.** `SELECT CASE WHEN true THEN 1 ELSE 1 / 0 END` is 1 in
  PostgreSQL; here the folder evaluates a result to learn its type, so
  the statement is refused with 42601. A `WHEN` after the first true one
  is skipped, as there.

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
