# The SQL front end: parsing, the dialect and binding

Status: **built.** This covers what turns SQL text into a statement the
planner can take: the parser, the dialect the layer accepts, and the binder,
which resolves names and types. It also covers the layer's known
differences from PostgreSQL in what SQL means. The planner is
`planner_design.md`. The TPROC-C statements the layer runs are in
`testing_design.md`, *The TPROC-C statements*. *Expressions in casts,
GROUP BY, DISTINCT, SET and MIN/MAX of text (#904)* is approved (2026-11-22),
not built yet.

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
  `CHECK (…)` on a column or on the table, named or not, is tested on every
  `INSERT` and `UPDATE` of the table, over the row as written, and refuses
  a row it makes false with 23514 (`catalog_design.md`, *CHECK
  constraints*). Its expression is any BOOLEAN value the select list
  evaluates over the table's columns and constants (*Expressions over
  columns*); `NO INHERIT`, `NOT VALID`, `DEFERRABLE` and `ADD COLUMN …
  CHECK` are refused.
- `INSERT … VALUES` of one row or several, with or without a column list,
  of parameters, constants and `DEFAULT`. The rows are one statement: each
  is written in order, as an `INSERT` of it alone would be (its keys,
  `UNIQUE` and `CHECK` tested against the rows before it too), and a row
  that fails writes none of them. Rows of different lengths are 42601, as
  in PostgreSQL. The plan writes each row in a block of its own, so a
  statement's plan grows with its rows.
- `INSERT … SELECT`, with or without a column list: each row of the query
  is written as an `INSERT` of it alone would be, and a row that fails
  writes none of them. The query reads the snapshot the statement began
  on, so `INSERT INTO t SELECT * FROM t` doubles `t` once. Its select list
  fills the columns listed, or the table's leading columns; a column left
  out takes its default, and a NULL given is written as NULL. As in
  PostgreSQL the column list is checked first (42703, 42701), then the
  query, then the number of values (42601, "INSERT has more expressions
  than target columns" or "more target columns than expressions"), then
  each value against its column in order. A constant, an unknown literal,
  `NULL` and a `$n` are bound against their column as a `VALUES` item is
  (`'5'` is an INTEGER, `'x'` 22P02). Any other value must be of its
  column's type or family: an integer into an integer column, `TEXT` or
  `VARCHAR` into `TEXT` or `VARCHAR(n)`, a `DECIMAL` of the same scale,
  `CHAR(n)` of the same length, which the row builder checks as it checks
  a parameter (22003, 22001); a `REAL` or `DOUBLE PRECISION` into the
  other; an integer into either, which the plan converts. A pair PostgreSQL
  has no assignment cast for is 42804, its message PostgreSQL's (without
  its hint); every other assignment cast PostgreSQL makes is refused
  (0A000, #824). Refused too (0A000): a query that aggregates, groups or is
  `DISTINCT`, and a set operation, whose rows the plan gives through no
  `Emit` (#682); and `INSERT … SELECT` in a procedure. The plan is the
  query's, its rows sent to the write (`planner_design.md`, *INSERT …
  SELECT*).
- `INSERT … ON CONFLICT DO NOTHING`, of `VALUES` or a query: a row whose
  key under the arbiter is taken is skipped, and the rest are written. With
  no target the arbiter is every unique constraint, the primary key and each
  unique index; with a list of columns it is every one whose columns are
  the list's, as a set (`(b, a)` and `(a, a)` name what `(a, b)` and `(a)`
  do, and unique indexes on `(a, b)` and `(b, a)` are both arbiters of
  either), as PostgreSQL infers it: a column the table lacks is 42703, no
  matching constraint is 42P10 ("there is no unique or exclusion constraint
  matching the ON CONFLICT specification"), and so are `ASC`, `DESC` and
  `NULLS FIRST`/`LAST` on a target column. A key is taken when a row the
  statement wrote holds it (`VALUES (1, …), (1, …)` writes the first) or
  the statement's view does and the statement did not vacate it; a key
  with a NULL part conflicts with nothing. A duplicate under another unique
  constraint is 23505, as without `ON CONFLICT`. NOT NULL (23502) and
  `CHECK` (23514) are tested first, so a row that breaks one fails the
  statement even where it would have been skipped, as in PostgreSQL. The
  count and `RETURNING` are the rows written. The plan calls
  `bc_insert_on_conflict` per row (`plan_api_design.md`), which decides
  before it records anything, so a skip leaves no trace, and takes no
  insert claim on an arbiter key: a concurrent commit of that key makes the
  commit a conflict, retried in autocommit, where the retry reads the row
  and skips it, and 40001 in a transaction. A row skipped because the
  snapshot holds it stays skipped when a concurrent commit deletes it
  (*Known differences*). Refused (0A000): `ON CONSTRAINT`, a
  `WHERE` on the target, an expression, collation or operator class in it,
  `ON CONFLICT` into a view, and in a procedure.
- `INSERT … VALUES … ON CONFLICT (columns) DO UPDATE SET …`: a row whose
  key under the arbiter (inferred as for `DO NOTHING`) is taken updates the
  row that holds it instead, and the rest are inserted. `DO UPDATE` with no
  target is 42601 ("ON CONFLICT DO UPDATE requires inference specification
  or constraint name"), as in PostgreSQL. The `SET` binds over two
  relations, the target table and `excluded`, the row proposed: a column
  qualified by the table's name is the holder's, bound as `UPDATE`'s `SET`
  binds (a column, column ± a parameter or integer, a constant, a
  parameter, `DEFAULT`); `excluded.col` is the value the row proposed for
  `col`, its parameter, or `col`'s default where the row leaves it out. An
  unqualified column is 42702 ("column reference … is ambiguous"), an
  unknown `excluded` column 42703 ("column excluded.x does not exist"), an
  unknown target 42703 ("column … of relation … does not exist"), a column
  set twice 42601. `excluded.col` is assigned to a column of its type or of
  another integer type (the row builder checks the range); a type
  PostgreSQL does not assign from is 42804 ("column … is of type … but
  expression is of type …"), another is refused (42601, not yet), and so is
  `excluded` inside an expression (`excluded.v + 1`). The updated row is
  checked as `UPDATE` checks it: `CHECK` again (23514), a primary key moved
  onto another row's (23505), and a moved key leaves its old key free, so a
  later row of the statement with that key is inserted. A row the statement
  already inserted or updated, hit again, is 21000 ("ON CONFLICT DO UPDATE
  command cannot affect row a second time", with PostgreSQL's hint); so is
  a key under an arbiter index whose holder the statement updated and that
  kept it. A unique key an update of the statement took from its row is
  free, as a moved primary key is: a later row with it is inserted. The count is the rows inserted or updated, and `RETURNING` gives
  each as written: the inserted row, or the row as updated. The plan calls
  `bc_insert_or_existing` per row (`plan_api_design.md`), and on a taken
  key builds the new row from the holder and writes it with `bc_update`.
  Refused: `DO UPDATE … WHERE` (0A000), `INSERT … SELECT … ON CONFLICT DO
  UPDATE` (42601, not yet), and the refusals of `DO NOTHING`.
- `RETURNING` of an `INSERT`, `UPDATE` or `DELETE`: a result row per row
  written, its list bound as a select list over the one table (`*`,
  columns, constants, parameters, expressions, each named as a select
  list names it). An `INSERT`'s and an `UPDATE`'s values are of the row as
  written (defaults, a moved key and `SET`'s values included), a
  `DELETE`'s of the row as it was. The plan evaluates the list after the
  write, over the row it has in hand (`nr`, or `old` for a `DELETE`), and
  emits it; the statement still reports its count, so the tag is `INSERT 0
  n`, `UPDATE n` or `DELETE n` after the rows. An aggregate is 42803, as in
  PostgreSQL. Refused (0A000): `RETURNING` in a procedure's statement, and
  `RETURNING … INTO` with it.
- `SELECT` targets: columns, `*` and `table.*` (below), constants (*Literals*), `COUNT(*)`,
  `COUNT(col)`, `COUNT(DISTINCT col)`, `SUM(col)`, `MIN(col)`, `MAX(col)` and
  `AVG(col)`, several of them in any order; a constant not beside an
  aggregate or a `GROUP BY`.
- `SELECT *` and `table.*` are expanded by the binder, before it binds
  anything else, into the columns they stand for: each FROM table's in
  table order, hidden row ids left out, each named by its column's name.
  Over a join `*` gives the join's columns as PostgreSQL orders them: each
  join's `USING` or `NATURAL` columns first, each once and named alone (so
  the preserved side's, as a column named alone is), then its left side's
  other columns, then its right table's; so over `a JOIN b USING (k) JOIN
  c USING (j)`, `j`, then `k`, `a`'s and `b`'s others, then `c`'s. A
  `table.*` is that table's columns, `USING` ones included. The
  expansion is a select list of ordinary columns, so everything after it
  (aggregation, `DISTINCT`, `ORDER BY`) treats it as the columns written
  out. `*` without `FROM` is 42601, `x.*` for a name not in `FROM` 42P01,
  `x.*` of a table an alias hides 42P01; `*` anywhere but the select list
  is 42601, `* AS (c, d)` is not PostgreSQL and is a syntax error.
- `GROUP BY` of columns, and `HAVING`, below. A query with a `GROUP BY`, a
  `HAVING` or an aggregate is aggregated; without `GROUP BY` it returns
  exactly one row, or none where its `HAVING` is false.
- `FROM`: one table, or up to 32 joined by a comma list, `[INNER] JOIN …
  ON`, `CROSS JOIN` or `LEFT [OUTER] JOIN … ON`, one after the other
  (`a JOIN b ON … LEFT JOIN c ON …`, each join's right side a table and its
  left side the tables before it), `JOIN … USING (cols) [AS alias]` and
  `NATURAL JOIN` at any place in the chain, and for the first join `RIGHT
  [OUTER] JOIN`. An inner join's right side may be an inner join in
  parentheses (`a JOIN (b JOIN c ON …) ON …`), and a comma list's items
  joins (`a LEFT JOIN b ON …, c`, `a, b JOIN c ON …`). The parser flattens
  both into one list of FROM items (`join_items`): an inner join's rows are
  the same whatever order its tables are joined in, and a later item's
  join whose `ON` cannot read the items before it pairs their rows with
  its own whatever they are, so the left-deep chain returns PostgreSQL's
  rows. Each `ON` records the first item of its own join
  (`Compare::on_from`: the chain's first, else the first of the
  parenthesised join or of the comma item). Columns may be
  qualified by the table's name. Each `ON` sees the tables of its own join,
  up to its own: a later one is 42P01 (`missing FROM-clause entry`), one
  before its join too (`invalid reference to FROM-clause entry for table
  "a"`, with PostgreSQL's DETAIL `There is an entry for table "a", but it
  cannot be referenced from this part of the query.`, naming the item's
  alias where it has one, even when an item of the `ON`'s own join is the
  same table under an alias, which hides its name; the parser's
  `check_on_scope`), and a name alone
  only such a table has 42703 with PostgreSQL's DETAIL naming it, or, for
  several, saying so with the HINT `Try using a table-qualified name.`
  (the binder's `column`). A `LEFT` or `RIGHT` join in parentheses or of a
  join in parentheses would NULL-extend other rows than the chain's and is
  refused (42601), and so is `USING` or `NATURAL` in parentheses, of a
  join in parentheses or after a comma, whose left side is not every item
  before it. It compares columns of two tables by any of `=`,
  `<`, `<=`, `>`, `>=` and `<>`, or is a predicate (`AND`, `OR`, `NOT`, `IS
  NULL`) over columns of both, or a boolean constant (`ON TRUE` pairs every
  row; `ON FALSE` none); NULL is true of none. `USING` and `NATURAL`
  compare their columns by `=`, as `ON` would, and name each alone as one
  column. Their left side is the join before them, as in PostgreSQL: its
  columns are each earlier join's `USING` columns once, then the other
  columns of its tables (the binder's `JoinScope`, one list per prefix of
  the chain). `NATURAL` takes every name of those columns the right table
  has, in their order. A `USING` name must be one column of the left side:
  one two of its tables have, not merged by an earlier `USING`, is 42702
  (`common column name "k" appears more than once in left table`), one it
  lacks 42703 (`… does not exist in left table`); one the right table lacks
  is 42703, one named twice 42701, each checked name by name in
  PostgreSQL's order. A merged column is its left side's column, equal to
  the right's in every pair (the preserved table's for a `RIGHT JOIN`); a
  later join's `USING` compares that column with its
  right table's. A name alone resolves against the columns of the join up
  to where it is read (an `ON` sees its own prefix, the rest the whole
  join): two of them with the name are 42702, so a merged name is
  ambiguous once a later table without `USING` on it has the name too.
  Each `USING` alias qualifies its own join's columns only. `LEFT` and `RIGHT` give
  every row of the preserved table, joined to each row of the other that
  `ON` matches, or to a row of NULLs where it matches none. `ON` decides the
  pairs and `WHERE` filters the joined rows, so `WHERE r.k IS NULL` keeps
  the rows nothing matched; a `USING` column named alone is the preserved
  table's. In a chain, a left join NULL-extends its right table in the rows
  before it: a later `ON` reading that table sees NULLs, so an inner join
  after it drops those rows where it reads them, and a left join keeps
  them. `RIGHT JOIN` keeps every row of its table against the join before
  it, from the first item of its own join (after a comma, only its own
  join's items, as in PostgreSQL): those items are NULL-extended together
  where nothing matches, their own `ON`s deciding which rows they have
  (`planner_design.md`, *Outer joins*). A `RIGHT JOIN` of a join that has
  an outer join, `a LEFT JOIN b ON … RIGHT JOIN c ON …`, is refused (42601,
  `a RIGHT JOIN of a join that has an outer join is not supported yet`).
  `FULL [OUTER] JOIN` keeps the rows of both sides, each NULL-extended
  where the `ON` matches none, `WHERE` filtering the joined rows after the
  NULL-extension; a `USING` column named alone is the side's that has a
  row, `COALESCE(l.c, r.c)`. It is the whole `FROM`, of two tables (or
  views a query merges, or one view of such a join): one in a chain, in
  parentheses or after a comma is refused (42601, `a FULL JOIN that is not
  the whole FROM of two tables is not supported yet`, #712), and so is an
  aggregate, `GROUP BY`, `HAVING` or `DISTINCT` above it, an `OFFSET`, `INSERT
  … SELECT` of it, one in an arm of a set operation and a grouped view as
  either side (42601, `a grouped view in a FULL JOIN is not supported
  yet`). As PostgreSQL, its `ON` must be merge- or hash-joinable: a
  conjunct that reads columns is an equality of an expression of each
  side, or one such equality is among them, each `NOT` pushed down first
  as PostgreSQL does, so `NOT (a <> b)` is `a = b` (else 0A000, `FULL JOIN
  is only supported with merge-joinable or hash-joinable join
  conditions`), and `FOR UPDATE` over it is 0A000,
  `FOR UPDATE cannot be applied to the nullable side of an outer join`
  (`planner_design.md`, *FULL JOIN*). A table takes an alias, `t [AS]
  x`. A table named twice, `t JOIN t AS x`, is read twice, once under each
  name; the planner reads it as two tables (`planner_design.md`, *Joins of
  several tables*). The front end rewrites the qualifier `x` to the table
  in every clause of that `SELECT` where no table is named twice, so the
  binder never sees an alias, and keeps the aliases where one is, which are
  then what tell the two apart. The table's own name qualifies nothing while
  it is aliased (42P01, as PostgreSQL), nor may two `FROM` items share a name
  (42712, `FROM t, t` among them). A `USING (…) AS alias` is a `FROM` name
  too: one that matches another `USING` alias, a table's alias or the name of
  a table not aliased, earlier or later in the `FROM`, is 42712, as in
  PostgreSQL (`resolve_aliases`). A table's name that only its alias hides is
  free for one, and the aliases are then kept for the binder, so that the
  qualifier names the join, not the table. A select-list item takes `[AS] name`,
  which names its output column, as an unaliased column item's name names
  its own; an unqualified `ORDER BY name` that is such a name orders by that
  item (PostgreSQL looks at output names first there, at input columns first
  in `GROUP BY`, which takes no output name here). The parser resolves the
  name to a copy of the item's expression, a column or any other
  (`resolve_output_names`, before positions are resolved, so `ORDER BY 1`
  names item 1 whatever another item is called): the output name hides any
  input column of that name, so `SELECT -a AS a ... ORDER BY a` orders by
  `-a`. Two items of the name are 42702 unless they are one, as in
  PostgreSQL: one column named twice, qualified or not, or one expression
  written twice alike (`ast::same_expr`, PostgreSQL's `equal()`). A
  subquery resolves its own aliases; a qualifier in it that names the
  outer query's alias is the outer table's, as in the outer query (a
  correlated subquery, *Subqueries* below). One that names a table the
  subquery itself aliases, the outer query's table read again under an
  alias (`FROM t WHERE EXISTS (SELECT 1 FROM t AS x WHERE x.a = t.b)`), may
  name the outer table, as PostgreSQL reads it: the subquery then keeps its
  aliases and that qualifier as written (`Names::open`), and the binder
  resolves it against the outer query's tables, or, where none is named so,
  refuses it with 42P01 (PostgreSQL's SQLSTATE, the binder's "missing
  FROM-clause entry" wording). At the top level such a qualifier is 42P01
  in the parser, as before. An outer alias of a table the subquery also
  reads (`FROM t AS o WHERE EXISTS (SELECT 1 FROM t WHERE t.a = o.a)`) is
  42601, since it would name the subquery's own table (#836). `t AS x (c1, c2)` renames the table's first
  columns (the binder renames them in the alias's table instance, which is
  already a copy; the parser keeps the alias so qualifiers are resolved
  there). The old names are gone (42703), the unnamed columns keep theirs,
  more names than columns is 42P10, a repeated name 42701. `SELECT * AS (c,
  d)` is SQL standard only, not PostgreSQL, and is a syntax error (42601).
  An alias of a join and a view in a query that names a
  table twice are refused. Without `FROM`, a `SELECT` returns one row of
  its items, or none under `LIMIT 0`: each is any select-list expression
  (*Expressions over columns*) of constants, parameters, subqueries and the
  time; a column in it is of no table (42703, or 42P01 for `t.x`), as in
  PostgreSQL. It takes no `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY` or
  `FOR UPDATE` yet.
- `UPDATE` and `DELETE` of any number of rows.
- `ORDER BY`, `LIMIT` and `FOR UPDATE`, below.

**Literals and expressions.**
- **Literals** of every kind, typed and read as PostgreSQL types them
  (*Literals*, below).
- **`*` and `/`** of constants fold as `+` and `-` do. Integers take
  PostgreSQL's types (`integer * integer` is `integer`, any wider operand
  makes `bigint`), the product of numerics is at the sum of the scales
  within 18 digits, and the result is range-checked (22003, `INT_MIN / -1`
  included). Integer `/` truncates toward zero. A zero divisor is 22012,
  unless the dividend is `NULL` (the operator is strict and never runs).
  A numeric quotient has PostgreSQL's scale (`select_div_scale`: at least
  16 significant digits, so `5.0 / 2` is `2.5000000000000000`), rounded half
  away from zero; one whose scale or value passes 18 digits (`1.0 / 3` is
  twenty places) is refused (42601). Of columns, they are *Expressions over
  columns*, below.
- **`%`, unary `-` and `abs`** follow PostgreSQL's int and numeric
  operators, of constants and of columns alike.
  - `%` is the remainder of the quotient truncated toward zero, of the
    dividend's sign: integers of the wider type, numerics at the finer scale.
    A zero divisor is 22012, `INT64_MIN % -1` is 0, and a float has no `%`
    (42883).
  - Unary `-` and `abs` keep the operand's type: an integer type's least value
    is 22003 ("smallint out of range"), a float's is IEEE, and `abs(-0.0)` is
    0. `abs` of an unknown literal is a `double precision`, as PostgreSQL
    resolves it.
  - Of a parameter, both are refused, since PostgreSQL finds them ambiguous.
  - `%` is named `?column?` in a select list, `abs` `abs`.
- **`+`, `-`, `*`, `/`, comparisons, `IS [NOT] NULL`, `AND`, `OR` and
  `NOT` of columns** are values in a select list (*Expressions over
  columns*, below).
- **`DATE - DATE`** is PostgreSQL's `date_mi`: the `integer` count of days
  from the second to the first, of constants and columns alike (*Date and
  time arithmetic*, below).
- **`EXTRACT(unit FROM x)`** is PostgreSQL 17's `pg_catalog.extract(text,
  x)` of a `DATE`, `TIME`, `TIMESTAMP` or `TIMESTAMPTZ`, of a constant
  (folded), a column, a typed `$n`, the time or any value expression in a
  select list, `ORDER BY` or `RETURNING` (`Expr::Kind::Extract`, the field
  in `constant`; `bc_extract`). The constant fold and the plan API share
  one implementation (`extract_field`, `sql/text.cppm`).
  - The unit is a name or a string, read as PostgreSQL's `date_part` reads
    it: lower-cased, every spelling of its token tables (`years`, `yr`,
    `mon`, `msec`, `msecond`, `usecond`, `isodow`, `j`), a name longer than ten characters
    matched on its first ten. A unit no type has is 22023 (`unit "foo" not
    recognized for type date`), a reserved word other than `epoch`
    (`now`, `infinity`) or a unit the operand's type lacks 0A000 (`unit
    "hour" not supported for type date`), the type named as PostgreSQL
    names it.
  - The operand resolves as PostgreSQL's overloads do: one of no type, a
    bare `$n`, an unknown literal or `NULL`, fits several (42725,
    `function pg_catalog.extract(unknown, unknown) is not unique`), and
    any other type none (42883). A `NULL` value gives `NULL` without
    reading the unit, as the function is strict.
  - The result is a `numeric` of PostgreSQL's scale: 6 for `SECOND` and
    `EPOCH` of a `TIME`, 3 for `MILLISECONDS`, 0 for the rest
    (`MICROSECONDS`, `MINUTE`, `HOUR`, `DAY`, `MONTH`, `QUARTER`, `WEEK`,
    `YEAR`, `ISOYEAR`, `DECADE`, `CENTURY`, `MILLENNIUM`, `DOW`, `ISODOW`,
    `DOY`, `JULIAN` and `EPOCH` of a `DATE`, and `TIMEZONE`,
    `TIMEZONE_HOUR`, `TIMEZONE_MINUTE` of a `TIMESTAMPTZ`, all 0 in the
    layer's one zone, UTC). The calendar is the proleptic Gregorian one,
    with PostgreSQL's years before 1 AD (1 BC is `-1`; decade 0 is 9 BC
    to 1 BC); a `TIMESTAMPTZ`'s fields are its UTC ones.
  - Refused (42601): `EPOCH` of a `TIMESTAMP` or `TIMESTAMPTZ`, whose
    microseconds pass the 18 digits of the layer's `numeric` after year
    33658; `JULIAN` of one, a fraction of a day of twenty places; a unit
    other than a name or a string constant (`pg_catalog.extract($1, d)`).
    `bc_extract` refuses those two fields of a timestamp too (BC_E_HANDLE),
    as the physical check does (`date_field_built`), so a plan written by
    hand cannot reach a value the binder never asks for.
    `EXTRACT` in a `GROUP BY` is refused as any expression there is; in
    a `WHERE` it is a filter (stage P1a). `EXTRACT` of a `TIME WITH TIME ZONE` is not built (0A000).
- **`CASE`, `COALESCE` and `NULLIF`** of constants fold. Of a column, a
  parameter or a subquery they are values in a select list (*Expressions
  over columns*, below). The result type is PostgreSQL's union rule: unknown
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
  known without evaluating it (with a column among the branches this
  goes away: *Expressions over columns*). Float branches of constants are
  refused. The output column is named `case`, `coalesce` or `nullif`.
- **`||` and the string functions** fold constants. Of a column, a
  parameter or a procedure's variable, `||`, `SUBSTRING` (and `substr`,
  its call form), `UPPER`, `LOWER`, `TRIM`, `POSITION`, `CHAR_LENGTH`,
  `CHARACTER_LENGTH` and `OCTET_LENGTH` are select-list values built per
  row (`Expr::Kind::Concat`, `Substr`, `TextFn`; `bc_concat`,
  `bc_number_text`, `bc_substr`, `bc_text_case`, `bc_trim`,
  `bc_char_length`, `bc_octet_length`, `bc_position`): `||` of text with
  text, or with an integer or a `DECIMAL` through its text as PostgreSQL
  prints it (`12.50`); the others as below, the constant folds and the
  plan API sharing one implementation (`sql/text.cppm`). A `$n` beside `||`
  or as a string function's argument is `text`, as a start or count
  `integer`. `OVERLAY` of non-constants, and any of them in a `WHERE`, are
  refused (42601, #671). The parser reads PostgreSQL's
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
  - `random(lo, hi)` is PostgreSQL 17's: an integer drawn uniformly from
    `lo` to `hi`, both included, anew at each evaluation (never folded);
    `lo > hi` is 22023. `random()` of no arguments, a `double precision`,
    is refused (42601).
  - `TO_TIMESTAMP(s, 'YYYYMMDDHH24MISS')` is a `timestamptz` read in UTC,
    the form HammerDB's driver sends (`procedures_design.md`, *Wire
    protocol for `CALL`*); another format is refused (42601), and a string
    that is not fourteen digits is 22007.
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
- **Where expressions may appear:** `SET col = expr`, `VALUES`, a select
  list, and any operand of a `WHERE` or `ON` (*Expressions in WHERE, ON and
  HAVING (stage P)*, stage P1a). A constant expression may also be a
  `HAVING` operand.
- **`DEFAULT`** as a `VALUES` item of an `INSERT` or the value of a `SET`
  assignment is the column's default constant, NULL where it has none (a
  NOT NULL column without one is 23502). The binder replaces it with that
  constant, so it is no new plan shape. It is no expression: PostgreSQL's
  grammar allows it nowhere else.

**`WHERE`** is a conjunction (`AND`) of comparisons (`=`, `<`, `<=`, `>`,
`>=`) of a column with a parameter, a constant, `$n ± k` or another column,
in any order, on any columns, and of predicates over them; and, since stage
P1a, of comparisons, `IS NULL`, `LIKE`, `IN` and `BETWEEN` of any value
expression a select list takes, and of a BOOLEAN expression standing alone
(*Expressions in WHERE, ON and HAVING (stage P)*). Those are filters,
evaluated on each row a plan reads, never a key's bound.
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
- **A REAL or DOUBLE PRECISION column compared** in a `WHERE`, an `ON`, a
  predicate or a `HAVING` (a grouped REAL or DOUBLE PRECISION column, or
  `SUM`, `MIN` or `MAX` of a DOUBLE PRECISION one, its `$n` a `double precision`) compares in PostgreSQL's float8
  order: `-0.0` equals `0.0`, NaN equals NaN and is above every other
  value, `Infinity` included. A key and a filter agree (`planner_design.md`,
  *Filters on DOUBLE and REAL columns*). A REAL compares the float its
  operand rounds to, as PostgreSQL types it float4: `real_col = $1` with
  0.1 matches the row written with 0.1, and so does `real_col = '0.1'`.
- **Two columns of one table compared** (`WHERE i > f`, in an `ON`, an
  `UPDATE` or a `DELETE`, under `AND`, `OR` and `NOT`) filter its rows; they
  never bound a key (`planner_design.md`, *The pipeline*). The pair is of
  one compared type (`VARCHAR` or `CHAR(n)` with `TEXT`, a `DECIMAL` of
  scale 0 with a `BIGINT`), two integers of any width, two floats, or an
  integer with a `REAL` or `DOUBLE PRECISION`, compared as float8 as
  PostgreSQL casts the integer: a `BIGINT` past 2^53 rounds, NaN is above
  every number. Not yet (42601): a `DECIMAL` with another scale or another
  number (the comparison does not rescale), a `CHAR(n)` with a `VARCHAR`
  (PostgreSQL compares those as bpchar, both sides' trailing spaces
  ignored), a `DATE` with a timestamp of either kind, a `TIMESTAMP` with
  a `TIMESTAMPTZ`. A pair PostgreSQL has no operator for is its 42883,
  `operator does not exist: text = integer`, the operator as written.
- **A boolean constant:** `TRUE`, `FALSE` or `NULL` as a conjunct of a
  `WHERE` or an `ON`, or inside its `AND`, `OR` and `NOT`
  (`ast::Compare::Kind::Const`). The binder makes it a BOOLEAN constant,
  `NULL` as `FALSE`, which no filter tells apart from unknown but where a
  `NOT` is above it (refused there, 42601). Constant folding drops `TRUE`
  and settles `AND`, `OR` and `NOT` of constants; a `FALSE` left at the top
  empties its input, so an inner join `ON FALSE` and a `WHERE FALSE` read
  nothing (`planner_design.md`, *Rewrites*). In a left join's `ON` it pairs
  no row, so the search makes it a filter of the right table's rows and
  every left row is NULL-extended. A numeric constant is PostgreSQL's 42804
  (`argument of WHERE must be type boolean, not type integer`), naming as
  PostgreSQL does the `AND`, `OR` or `NOT` it is an argument of, else the
  clause (`JOIN/ON` for an `ON`), and its type: `integer` within int4,
  `bigint` within int8, else `numeric`. A string is read as a BOOLEAN, as
  PostgreSQL reads an unknown (`WHERE 'abc'` is 22P02).
- **`LIKE`** and **`ILIKE`**: `col [NOT] [I]LIKE pattern [ESCAPE e]`, of a
  TEXT or VARCHAR column and a parameter or constant pattern, typed TEXT as
  PostgreSQL types it (since stage P1a either side may be any text
  expression, evaluated); a predicate leaf, matched by `bc_like` or `bc_ilike`
  (`plan_api_design.md`): `%` any run, `_` one character, bytewise (`COLLATE
  "C"`); `LIKE` is case-sensitive, `ILIKE` matches the ASCII letters of the
  pattern's literals in either case, as `UPPER` and `LOWER` fold (`É`
  and `é` differ). The escape is a backslash, `ESCAPE`'s one character, or
  none for `ESCAPE ''`; a longer one is 22025, as is a pattern the match
  finds ending in its escape. `ESCAPE $n` is a TEXT parameter, checked at
  each row by `bc_like_escape`; a NULL one makes the predicate unknown, as
  PostgreSQL's strict `like_escape` does. The escape is read before letters
  fold, as `like_escape` runs before `ILIKE`'s lowering: under `ESCAPE 'A'`,
  `A` quotes and `a` is a literal. A number column is 42883.
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
  `id = (SELECT MAX(k) FROM u)` is a key lookup. A `$n` only a subquery
  names is one of the statement's parameters, as any `$n`.
- **Correlated subqueries**, a narrow slice (#714): `[NOT] EXISTS`, `[NOT]
  IN`, `ANY` and `ALL` as above, in the `WHERE` of a `SELECT`, `UPDATE` or
  `DELETE` of one table, the subquery itself of one table, where each
  column of the outer table it reads is compared with a column of its own
  in its `WHERE` (`b = a`, `t2.b < t1.a`, inside `AND`, `OR` and `NOT`
  too). PostgreSQL's semantics, run for each outer row that reaches the
  conjunct (`planner_design.md`, *Subqueries*): a NULL outer value compares
  as NULL, so `b = a` matches no row and `a NOT IN (…)` of that row is
  true. The two columns compare as a subquery's value does above (a pair
  outside those is 42601, its message naming the outer column), and a pair
  PostgreSQL has no operator for is its 42883, in the order written. The
  subquery may read the outer table again under an alias, so an `UPDATE` or
  `DELETE` may test the table it writes; each run reads it as it was before
  the statement.
  Its `SELECT` may aggregate (`COUNT`, `SUM`, `MIN`, `MAX`), group, order
  and limit as any subquery may. Refused, each with its own 42601 (#836): a
  correlated scalar subquery (`x = (SELECT …)`, or in a select list); one in
  a query of a join, outside a `WHERE`, in an arm of a set operation, or in
  a procedure's statement; one of a join, of a set operation or without
  `FROM`; an outer column anywhere else (its select list,
  `GROUP BY`, `HAVING`, `ORDER BY`, `IS NULL`, `LIKE`, arithmetic, compared
  with a constant or with another outer column); either column a `CHAR(n)`;
  and one where a view is read, around it or in it.
- Not yet: `$n ± k` inside one, one of both tables' columns in a
  join, in `HAVING`, `LIKE` of a BYTEA column, `IS DISTINCT FROM`, `NULL`
  under `NOT`, a string as a boolean, a constant in `HAVING`; a correlated
  subquery but the slice above, one inside another, in `FROM`, `FOR UPDATE`
  in one, one of `AVG`, assigned by `SET` or `VALUES`, beside an aggregate,
  or compared with other than a column.
- **Refused (42601): a `WITH` query** (a common table expression, `WITH
  RECURSIVE` too), in any statement and in a view's body: the parser takes
  no `WITH` clause yet ("SELECT clause not supported"), where PostgreSQL
  runs it.
- Comparing two columns of different SQL types in a join is refused
  (42601, *Known differences*), but for a pair PostgreSQL has no operator
  for, which is its 42883 as for two columns of one table (see above), in
  an `ON`, a `WHERE`, `USING` or `NATURAL`.

**`ORDER BY`, `LIMIT` and `OFFSET`.**
- `ORDER BY` takes a list of keys, each `ASC` or `DESC`, and `NULLS
  FIRST` or `NULLS LAST`; without them NULLs sort as PostgreSQL's
  defaults put them, last under `ASC` and first under `DESC`. Ties come
  back in any order, as PostgreSQL allows. `CREATE INDEX` takes neither.
- A key is a column, an output name or select-list position (above and
  below), or an expression of the `FROM` columns (`ORDER BY a + 5`,
  `ORDER BY coalesce(a, 0)`, `ORDER BY t.a + u.b` over a join): anything
  the select list takes, bound as a select item is (`plain_output`), and
  evaluated per row as the row is added to the sorter, so an error it
  raises is the statement's (`ORDER BY s * s` of a `SMALLINT` is 22003,
  `ORDER BY id / 0` 22012). A key with no column that folds to a constant
  or is a parameter orders nothing and is dropped (`ORDER BY 1 + 1`,
  `ORDER BY (SELECT 1)`); a bare `ORDER BY $n` that nothing before typed is
  text, as PostgreSQL types it there. No index gives an expression's order:
  the plan sorts (`docs/planner_design.md`, *Sorts*).
- Under `DISTINCT` a key must be a selected item: a column the list
  selects, or an expression written alike; any other is 42P10, as in
  PostgreSQL. On a set operation a key that is not an output column is
  0A000 (*Set operations*).
- *Refused (42601).* An expression key in a grouped or aggregated query
  (an aggregate in `ORDER BY`, `ORDER BY a + 1` under `GROUP BY a`),
  `ORDER BY random()`, which PostgreSQL draws once per row, and `ORDER BY
  t.*`, which PostgreSQL orders by the whole row.
- `LIMIT` takes an integer literal or a parameter. `LIMIT NULL` is no
  limit, and a negative limit is refused (2201W), as in PostgreSQL.
- `OFFSET` takes an integer literal: that many rows are skipped before
  `LIMIT` counts, after `ORDER BY`. `OFFSET NULL` is 0, a negative one is
  2201X. A query of one row (an aggregate without `GROUP BY`, a `SELECT`
  without `FROM`) gives none past `OFFSET 1`. It is taken in a query, a
  subquery and a cursor's query; on a set operation, in one of its arms and
  in a view it is refused (42601).

**Aggregates**, with PostgreSQL's results:
- `COUNT(*)` counts rows, `COUNT(col)` non-NULL values, `COUNT(DISTINCT
  col)` distinct non-NULL values. All three are `bigint`, and 0 over no
  rows.
- The argument of `COUNT`, `SUM`, `MIN`, `MAX` and `AVG` is a column or
  any expression of the `FROM` columns a select list takes (*Expressions
  over columns*): `SUM(v * 2)`, `COUNT(1)`, `MAX(CASE … END)`, a view
  column that is an expression. It is evaluated once per row, where the
  row is read, and folded as a column's value is: NULL is skipped, and the
  call is typed by the expression's type as by a column's (`SUM` of an
  `INTEGER` product is `bigint`). An error it raises is the statement's
  (22003 for an `INTEGER` product past its range, 22012), raised by a row
  whose operands are not NULL, as PostgreSQL evaluates it. An aggregate
  inside the argument is 42803, "aggregate function calls cannot be
  nested"; a bare `$n` nothing else types, or a bare NULL or quoted
  literal (no cast), is of unknown type: 42725 under `SUM` and `AVG`
  ("function sum(unknown) is not unique"), and 42P18 for the `$n` under
  `COUNT`, as PostgreSQL refuses them (`COUNT` of such a literal counts it). The parser keeps the argument whole
  (`ast::Expr::args`, `star` for `COUNT(*)`); the binder binds it as a
  select item's expression. The plans that answer an aggregate from the
  keys (`KeyAggregates`, `CountRange`) take a column argument only; any
  other is folded by a loop over the rows.
- Every aggregate over a column takes `ALL` (the default) or `DISTINCT`.
  `DISTINCT` folds each distinct non-NULL value of the column once: a
  `DOUBLE PRECISION`'s `-0.0` and `0.0` are one value and NaN is one, as
  `COUNT(DISTINCT)` already took them. `MIN(DISTINCT x)` and `MAX(DISTINCT
  x)` are `MIN(x)` and `MAX(x)`, bound as such. The values seen are held
  per group in the statement's memory, a key each, and a statement that
  passes its budget fails with 54000, never spilling (`planner_design.md`,
  *Memory*). With `DISTINCT` the argument is a column: `SUM(DISTINCT a +
  1)` is not taken yet (0A000), since the distinct values are keyed by the
  column's encoding.
- `SUM` over `SMALLINT` or `INTEGER` is `bigint`. Over `BIGINT` or
  `DECIMAL` it is `numeric`, kept at the input's scale. It is NULL over no
  rows.
- `SUM`, `AVG`, `MIN` and `MAX` over a `DOUBLE PRECISION` column (`FLOAT`)
  are `double precision`, as PostgreSQL's `float8` aggregates are. `SUM`
  adds in order and fails with 22003 where finite values overflow; an
  infinity or NaN carries through. `AVG` is the sum divided by the count.
  NaN is above every number, so it is the `MAX` and the `MIN` only of NaNs.
  Of `-0.0` and `0.0` the last read is the `MIN` and the `MAX`, as
  PostgreSQL keeps it (*Known differences*).
- `AVG` over an integer or `DECIMAL` column is `numeric`: PostgreSQL's
  `numeric_div` of the sum by the count, at the scale its
  `select_div_scale` chooses (at least 16 significant digits), rounded
  half away from zero. Its digits can pass 64 bits, so the plan divides
  them into the quotient's text (`bc_num_quotient`) and a result row
  carries the value as that text. NULL over no rows.
- `MIN` and `MAX` are of the column's type, and NULL over no rows. They
  take a numeric or temporal column.
  - Of a `TEXT`, `VARCHAR` or `BYTEA` column they are refused (#534).
    Keeping the least value seen needs a buffer the plan API doesn't have.
  - `SUM`, `AVG`, `MIN` and `MAX` of a `REAL` column are refused (42601):
    PostgreSQL's `SUM` and `MIN` of one are `real`, summed in single
    precision.

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
  aggregate with a parameter. An aggregate written as the select list
  writes it is the same call, which the planner finds by its bound
  expression and folds once; any other is folded alongside. The parameter takes the column's type, or the
  aggregate's: `COUNT`'s and an integer `SUM`'s `bigint`, a `SUM` of a
  BIGINT or DECIMAL its numeric, checked as a BIGINT at its scale, `MIN`'s
  and `MAX`'s their column's. A parameter an earlier use typed, in the
  `WHERE` or an earlier conjunct, keeps that type, as PostgreSQL types a
  `$n` by its first use: `WHERE a > $1 GROUP BY a HAVING count(*) > $1`,
  `a` an INTEGER, compares `count(*)` with an INTEGER `$1`. The layer runs
  it where both are integers of scale 0 (SMALLINT, INTEGER, BIGINT, a
  DECIMAL of scale 0), which compare exactly; any other pair is refused, as
  two uses of one `$n` are (*Known differences*). `HAVING` on `AVG` is
  refused (42601): its quotient is numeric text, which a plan does not
  compare. `HAVING` on `SUM`,
  `MIN` or `MAX` of a `DOUBLE PRECISION` column compares a `double
  precision`, in float8 order.
- `AVG`'s result is PostgreSQL's numeric of no declared precision, a SQL
  type of its own (`SqlKind::Numeric`), the type of a `NUMERIC` column
  declared without a precision; a result row carries its value as its
  text. A subquery of it is refused (0A000).
- `ORDER BY` takes grouped columns; it sorts the groups, and `LIMIT` counts
  them.

**`FOR UPDATE`**, on a single-table `SELECT`:
- every row returned is guarded with `bc_guard_unchanged`, and the commit
  fails with 40001 if another transaction committed a change to one first;
- with an aggregate it is refused (0A000, "FOR UPDATE is not allowed with
  aggregate functions"), as PostgreSQL refuses it;
- `FOR UPDATE OF t` is taken when `t` is a table the query reads (`42P01`
  otherwise), and is `FOR UPDATE`;
- on a join, not yet (#534).

**Cursor statements.** `ast::parse_cursor` reads `DECLARE`, `FETCH`, `MOVE` and
`CLOSE`, and returns nothing for any other statement. They are not an
`ast::Statement`: they name a session's cursor and carry no plan, so the
server session runs them (`server_design.md`, *Cursor statements*). A
`DECLARE` gives its name, `[NO] SCROLL`, `BINARY`, `WITH HOLD`, whether the
query has `FOR UPDATE`, and the query's text, which `parse` then reads as a
`SELECT` (`FOR READ ONLY` is dropped by libpg_query's grammar and needs
nothing). A `FETCH` or `MOVE` gives PostgreSQL's direction and count. Which
forms run is the session's and `cursor_design.md`'s; `OPEN` and `FETCH … INTO`
are syntax errors as in PostgreSQL, and `WHERE CURRENT OF name` is the whole
`WHERE` of an `UPDATE` or `DELETE` (`ast::Update::current_of`, `ast::Delete::current_of`;
`cursor_design.md`, *As built (phase 6)*); in a `SELECT` or beside another
condition it is a syntax error, as in PostgreSQL.

**Select-list positions in `ORDER BY`.** `ORDER BY 2` orders a plain `SELECT`
by its second select-list item. The parser resolves it to a copy of that
item's expression, a column or any other, so every later stage sees the key
itself; a position outside the list is 42P10 and any other constant 42601,
as in PostgreSQL. A
position after `*` is refused (42601): the binder expands stars, the parser
does not.

**Set operations**, `UNION`, `INTERSECT` and `EXCEPT` of two queries, each
`ALL` or `DISTINCT` (the default), as PostgreSQL's (`planner_design.md`,
*Set operations*):
- **Precedence:** `INTERSECT` binds tighter than `UNION` and `EXCEPT`,
  which group to the left; parentheses group. The parser builds the tree
  (`ast::SetOperation`, a `Select`'s `set`), so a set operation is a
  `SELECT` wherever one is taken: a statement, a subquery in `EXISTS`, `IN`,
  `ANY`, `ALL` or a comparison, and a view's query.
- **Arms** are plain `SELECT`s of columns, constants and parameters, a
  `WHERE`, a join, and in parentheses their own `ORDER BY` and `LIMIT`.
  Refused (42601): an arm that groups, aggregates or is `DISTINCT`. Refused
  (0A000, as PostgreSQL's): `FOR UPDATE` on or inside one. `CORRESPONDING` is
  not PostgreSQL's, and a syntax error.
- **Columns.** The arms give as many columns as each other (42601). A
  result column is named as the first arm names it. Its type resolves pair
  by pair, as PostgreSQL's `UNION` does: the same type; integers to the
  wider; an integer or `REAL` meeting `DOUBLE PRECISION` to it; `TEXT`
  meeting `VARCHAR` to `TEXT`; `DECIMAL` of one scale; classes that differ
  (a number and a string) are 42804, `UNION types integer and text cannot
  be matched`.
  Refused (42601) where the layer does not convert yet: `DECIMAL` of two
  scales or with an integer or a float, an integer with `REAL` (PostgreSQL
  rounds it to a float), `DATE` with `TIMESTAMP`, and a literal of no type
  (`NULL`, `'a'`) meeting a column of another type.
- **`LIMIT`** after the last arm limits the combined rows.
- **`ORDER BY`** after the last arm orders the combined rows, by the result
  columns' names (the first arm's) or by position, `ASC` or `DESC`, with
  `NULLS FIRST` or `LAST` (last under `ASC`, first under `DESC` by default),
  and a `LIMIT` after it applies to the sorted rows. The names are the
  output's alone, as in PostgreSQL: a name that is only an arm's column is
  42703, a name two columns share 42702, a qualified name 42P01 ("missing
  FROM-clause entry"), a position outside the columns 42P10, and an expression 0A000 ("invalid
  UNION/INTERSECT/EXCEPT ORDER BY clause", without PostgreSQL's detail and hint).
  The parser keeps a position (`ast::OrderItem::position`) for the binder
  to resolve against the outputs (`logical::SetOp::order`). `ORDER BY` inside
  a parenthesised set operation within another is refused (42601, "not
  supported yet"). *Select-list positions* below.
- **A view defined with a set operation** is created, with its columns and
  the tables it reads, and not read yet (42601), as one defined with
  `DISTINCT` or `LIMIT`: reading it needs a derived table.

**Views**, `CREATE VIEW name [(column, …)] AS SELECT …` and `DROP VIEW [IF
EXISTS] name`:
- **Expanded by the binder.** The catalog keeps the statement's text
  (`catalog_design.md`, *Views*). A statement that names a view has it
  parsed and bound again, and `ViewExpander` (`sql/binder.cppm`) rewrites
  the statement before the binder sees it: the view's `FROM` and `WHERE`
  replace the name, and each reference to one of its columns becomes the
  select-list item it is. Planner, plans and runtime see base tables
  only. A view of views is expanded through, to a depth of 16.
- **Where it is read:** alone, as the table of a `FROM`; inner, `LEFT` and
  `RIGHT` joined with a table or another view; in a subquery (in the
  select list, `WHERE` or `ORDER BY`), and in the subquery of an
  `UPDATE`'s or `DELETE`'s `WHERE`. A joined view merged
  into the query must be a plain `SELECT` of one table. Its columns are
  qualified by that table, so one the other side lacks never changes
  meaning, and one both have is 42702. A grouped view joined with a table
  is not merged: it is a derived relation (below), and the join is any
  join of two relations, `USING` and `NATURAL` included. Refused (42601):
  `USING` or `NATURAL` on a merged view, an outer join of a merged view
  with a `WHERE`, two views of one table (a table joined with itself), and
  a grouped view joined with another view.
- **Over a grouped view** (`GROUP BY`, `HAVING` or an aggregate) a query
  selects its columns, orders by its group columns, and its `WHERE` keeps
  groups: it becomes the view's `HAVING`. It may group again by exactly the
  view's group columns, selecting those alone: a view row is a group, so
  that is the view. A grouped view with no aggregate, whose `HAVING` tests
  group columns alone, has the base rows' values, one row per group, so a
  query whose aggregates are `MIN`, `MAX` and `COUNT(DISTINCT)`, which
  cannot tell a value once from many, reads the base rows (the view's
  `HAVING` joining its `WHERE`). Each of these is merged into the query.
- **A grouped view as a derived relation.** Any other query that
  aggregates, groups or selects `DISTINCT` over a grouped view (`SUM` or
  `COUNT` of its rows, an aggregate of its aggregate, `GROUP BY` one of its
  aggregates) reads it as PostgreSQL's subquery in `FROM` does: as rows of
  its own, one per group. `ViewExpander::derive` leaves the view in the
  query as a `FROM` item holding its query (`ast::FromItem::derived`, the
  view's `ORDER BY` dropped), decided before the query is changed. The
  binder binds that query as a SELECT of its own, the query around it out of
  its scope, and makes a table of its outputs (`Binder::derived_relation`,
  an id from `Table::kDerivedBase`, owned by the bound statement): each
  column named as the view names it, typed as its output, NULL-able as the
  output is (a grouped column as its table declares it, `COUNT` never, any
  other aggregate always). It has no primary key, so a column the query
  neither groups nor aggregates is 42803, as in PostgreSQL. The plan reads it
  as `logical::Derived` (`planner_design.md`, *Derived relations*). Refused
  (42601): a `GROUP BY` or `DISTINCT` of a column held other than as an
  integer or a boolean (text, bytea, a float) (#833), a view column that is
  an `AVG` of an integer or `DECIMAL` (PostgreSQL's `numeric`, numeric
  text the plan never keys or compares), and a view that itself reads one.
- **A grouped view joined with a table** is a derived relation too
  (`ViewExpander::merge_two`, `derived_item`): `SELECT a FROM v JOIN t
  USING (a)`, whatever the query selects. The binder binds it as above; a
  table the view's query reads, joined again, is read by an instance of its
  own (`Binder::instance`), since the view's query reads it by its own id
  in the loops the join runs inside. Its rows are read only as the join's
  outer rows, so refused (42601): the view on an outer join's
  NULL-extended side (`t LEFT JOIN v`, `v RIGHT JOIN t`), stricter than
  PostgreSQL (#834); a query that groups, aggregates or selects `DISTINCT`
  over the join; the view joined with another view; and a join of a view
  that aggregates over a grouped view, a derived relation inside a derived
  relation (#835). Refused (0A000) by the planner: `ORDER BY` over the
  join, whose sort would hold the view's rows (#834).
  Refused (0A000) by the planner: a view of one group of every row (no
  `GROUP BY`) whose every plan may end before it reads: a point lookup, or
  a comparison with NULL or with a subquery's value (#833). Its `WHERE` on
  other constants, a filter or a key's range, is read.
- A view defined with `DISTINCT`, `LIMIT` or a set operation is created
  and not read yet.
- **Columns** are named as the select list names them, or by the column
  list, which may name fewer than there are columns. Two with one name are
  42701, more names than columns 42601.
- **Created** by binding its query as a use would, so its errors are
  PostgreSQL's (42P01, 42703, …). It takes no parameter (42P02). Refused
  (42601): `OR REPLACE`, `TEMPORARY` and options (`WITH (check_option=…)`
  among them). `WITH [CASCADED | LOCAL] CHECK OPTION` is taken
  (docs/catalog_design.md, *WITH CHECK OPTION*). Each unqualified relation name of its query is stored
  with the schema it resolved to, and read there at every use
  (*Schemas*).
- **`INSERT` through a view** writes to its table, as PostgreSQL's
  automatically updatable views do (`ViewExpander::into_base_table`). The
  view, and each view it reads down to the table, must select from one
  table or view, with no `DISTINCT`, `GROUP BY`, `HAVING`, set operation,
  `LIMIT`, `OFFSET` or aggregate (a `LIMIT ALL`, `LIMIT NULL` or `OFFSET 0`
  that limits nothing counts, as in PostgreSQL): otherwise 55000, "cannot insert into view",
  with PostgreSQL's DETAIL and HINT naming the first view of the chain that
  fails. A `FROM` alias and its column names are read through. With no
  column list the row fills the view's first columns, as many as it has
  (more is 42601). Each column filled must be a column of the table: one
  that is an expression is 0A000 with PostgreSQL's DETAIL, and two view
  columns of one table column are 42601 ("multiple assignments to same
  column"). The table's defaults fill the columns left out. A view's
  `WHERE` is checked only where a check option asks: the row written is
  then refused (44000) where a checked `WHERE` is false or NULL
  (docs/catalog_design.md, *WITH CHECK OPTION*). `VALUES`, its
  subqueries and `INSERT … SELECT` read views as any query does.
  `RETURNING` gives the view's columns over the row written, `*` included.
  A view of views is checked a view at a time, from the outermost, so an
  error names the view PostgreSQL's does.
- **`UPDATE` and `DELETE` of a view are refused** (0A000, #678), as is any
  DDL written against it as a table.
- **Dropped with `RESTRICT`**, the default and only form: a table or view
  another view reads cannot be dropped (2BP01), whether the view reads it
  in `FROM` or in a subquery in any position, `ORDER BY` included (#870).
  What a view reads is what binding its query resolved, a view's own reads
  and not those of the views it reads (`catalog_design.md`, *Views resolve
  against what they were created against*). The error names one view that
  reads the object itself, in the message: PostgreSQL's names each
  dependent in a DETAIL, with a HINT. `DROP VIEW` of a table is
  42809, as `DROP TABLE` of a view. Refused (0A000, #872): `CASCADE` on
  `DROP TABLE`, `DROP VIEW` or any `DROP` but `DROP SCHEMA`, which
  PostgreSQL takes; a `DROP` of a list or `CONCURRENTLY` is 42601.
- **A plan through a view is rebuilt after any DDL**, since the runtime
  checks the tables a plan reads and not the views it came through
  (`Statement`'s plan records them, `bound::Bound::views`).

**`CREATE TABLE AS` and `SELECT … INTO`**, `CREATE TABLE [IF NOT EXISTS]
name [(column, …)] AS SELECT … [WITH [NO] DATA]` and `SELECT … INTO [TABLE]
name …`, one statement (`ast::CreateTableAs`) that remembers which form was
written:
- **A table of the query's columns.** Each column is named as the select
  list names it (`now()` as `now`), or by the column list, which may name
  fewer than there are columns; each has the query's result type (a
  `VARCHAR` of no length is `TEXT`, as a column declared so is). A `CASE`
  with an `ELSE` or a `COALESCE` whose branches are all one `VARCHAR(n)` or
  `CHAR(n)`, none a literal or a `$n`, keeps that length, as PostgreSQL's
  exprTypmod does; with a literal among them it has none. The table has no primary key, so
  its rows are under hidden row ids 1 to n, and no constraint or default.
- **Bound at prepare, run at execute.** The query is bound and planned when
  the statement is prepared, so its errors (42P01, 42703, …) come first,
  as PostgreSQL analyses it first. Its parameters are the statement's: over
  the extended protocol `$n` binds as in a `SELECT`; over the simple one it
  is 42P02, as in PostgreSQL. A literal's input error ('abc'::int is
  22P02, DATE '2020-13-01' 22008, '40000'::smallint 22003) is raised at
  prepare, before the name check, as PostgreSQL's analysis reads the
  literal (`ast::SqlError::at_analysis`). A data exception of folding a
  constant (1/0 is 22012, 40000::smallint 22003) is kept and raised at
  execute, after the name check, since PostgreSQL folds constants when it
  plans, after it looked for the table. The query is then compiled with
  that constant NULL, for the statement's parameters only, and never run.
  Only a constant in the select list, `GROUP BY`, `WHERE`, `ON` or `HAVING`
  of the query itself is replaced so; one in a set operation's arm or a
  subquery raises its error at prepare, before the name check.
- **PostgreSQL's order at execute.** A relation of the name is 42P07, or
  with `IF NOT EXISTS` a NOTICE and nothing else; then more names than
  columns is 42601 ("too many column names were specified"), a name twice
  42701; then the query runs, autocommit, and its rows and the table are
  one DDL: the table's record and every row in one batch
  (`catalog_design.md`, *CREATE TABLE AS*). A concurrent DDL that takes the
  name first makes it 42P07, or the NOTICE. In a transaction the query
  runs as its statement, through its view, and the table and rows are a
  DDL of the transaction (`transaction_design.md`, stage 4).
- **Tag:** `SELECT n`, the rows written; `CREATE TABLE AS` for `WITH NO
  DATA` (the query is not run) and for an `IF NOT EXISTS` skip. A read-only
  block refuses it as `SELECT INTO` or `CREATE TABLE AS`, as written
  (25006).
- **Refused (0A000):** `TEMP`, `UNLOGGED`, `WITH (…)`, `USING`,
  `TABLESPACE`, `CREATE MATERIALIZED VIEW` and `CREATE TABLE … AS EXECUTE`;
  a result column of a `NUMERIC` of no declared precision (an `AVG`, a
  literal `1.5`, a `NUMERIC` column), whose values the follow-up of
  `NUMERIC` columns will convert; a query of no columns; `INTO` in
  the first arm of a set operation, which PostgreSQL takes as the
  statement's. `ON COMMIT` is 42P16, as PostgreSQL refuses it on a table that
  is not temporary; `INTO` in another arm is 42601, as there, and in a
  subquery 42601. `WITH NO DATA` of a query whose constant raises an error
  (`SELECT 1/0 … WITH NO DATA`) is refused (0A000): PostgreSQL does not plan
  that query, so creates the table, but the stand-in lost the column's
  type.

**Procedures**, `CREATE [OR REPLACE] PROCEDURE name (args) LANGUAGE plpgsql
AS …` and `DROP PROCEDURE [IF EXISTS] name [(argtype, …)]`
(`procedures_design.md`):
- **Parsed in two passes.** `pg_query_parse` gives the name, the arguments
  and their modes; an argument's type is read as a column's is.
  `pg_query_parse_plpgsql` checks the body's syntax (42601). The SQL in the
  body is not bound, so it may name a table that does not exist yet.
- **Refused (0A000):** `CREATE FUNCTION`, a language other than `plpgsql`,
  `BEGIN ATOMIC`, any option, `VARIADIC`, an unnamed argument, a `DEFAULT`,
  and `CALL`, until the runtime runs procedures. No language is 42P13.
- **`DROP PROCEDURE`'s list** is read as libpg_query gives it: the types,
  without those marked `OUT`.

**Roles and privileges**, `CREATE ROLE`, `DROP ROLE [IF EXISTS]`, `GRANT`
and `REVOKE` on tables and views (#653, its first step):
- **No logins, and no enforcement yet.** Authentication is outside the
  layer, and every session is the bootstrap superuser (#653's default
  resolver), so a grant is recorded and nothing is denied, as PostgreSQL
  denies a superuser nothing. The resolver that names a session's role and
  the `Authorizer` that checks privileges (42501) are #653's next step.
- **`CREATE ROLE name`**, with no option. `public`, `none` and names
  starting `pg_` are reserved (42939), a name in use is 42710. Roles have
  a namespace of their own.
- **`GRANT` and `REVOKE`** of `SELECT`, `INSERT`, `UPDATE`, `DELETE`,
  `TRUNCATE`, `REFERENCES`, `TRIGGER` and `MAINTAIN`, or `ALL
  [PRIVILEGES]`, on `[TABLE]` tables and views, to or from roles and
  `PUBLIC`, with column lists (`SELECT`, `INSERT`, `UPDATE`, `REFERENCES`),
  `WITH GRANT OPTION`, `REVOKE GRANT OPTION FOR`, `GRANTED BY` the current
  user, and `CASCADE` or `RESTRICT`. The latter two are alike: every grant's
  grantor is the superuser, so no grant depends on another. Repeating one
  changes nothing.
- **PostgreSQL's semantics.** A table-level `REVOKE` takes the privilege
  from every column too, and a column-level one takes it from that column
  only. A grant option goes to roles only, not to `PUBLIC` (0LP01).
- **PostgreSQL's order of errors.** The objects come first: a missing one
  is 42P01, an index 42809. Then the grantees: a missing role is 42704,
  and a grantor that is another role is 0A000. Then the privileges: an
  unrecognized one is 42601, and one of another kind of object, or one a
  column lacks, is 0LP01. Then the columns: 42703.
- **`DROP ROLE`** of a role that holds a privilege is 2BP01, so is any role
  of a list with one, and nothing is dropped. A missing role is 42704; with
  `IF EXISTS`, a NOTICE per missing name. A drop of a table or view takes
  its privileges with it.
- **`GRANT` and `REVOKE` `ON SEQUENCE`, `ON DOMAIN` and `ON TYPE`**, of the
  privileges each takes (`catalog_design.md`, *Domains, shell types and
  sequences*), with PostgreSQL's errors: 42704 for a missing type or a
  shell one, 42P01 for a missing sequence, 42809 for an object of another
  kind, 0LP01 for a privilege the kind does not take.
- **Refused (42601):** `CREATE ROLE`'s options and `CREATE USER`, since the
  layer has no logins or role attributes. Also refused: `GRANT` on other
  kinds of objects (functions, procedures, …), `ON TABLE` of a
  sequence, role membership (`GRANT role TO
  role`), `ALTER DEFAULT PRIVILEGES`, and `CURRENT_USER`, `CURRENT_ROLE` or
  `SESSION_USER` as a grantee. `ON SCHEMA` and `ON ALL … IN SCHEMA` are
  0A000 (*Schemas*).

**Domains, shell types and sequences**: `CREATE DOMAIN name [AS] type`,
`CREATE TYPE name` (a shell type), `CREATE SEQUENCE [IF NOT EXISTS] name`,
and `DROP DOMAIN`, `DROP TYPE` and `DROP SEQUENCE [IF EXISTS]`. They are
stored and granted on, with PostgreSQL's namespaces and errors. None is
usable yet.
- Refused (42601): a domain's `DEFAULT`, `NOT NULL`, `CHECK` and
  `COLLATE`, a column of a domain's type, a sequence's options,
  `nextval`, `currval`, `setval` and `SERIAL`, and every other form of
  `CREATE TYPE`.

**Schemas** (`catalog_design.md`, *Schemas*, stage 1b): `CREATE SCHEMA
[IF NOT EXISTS] name [AUTHORIZATION role]`, `CREATE SCHEMA AUTHORIZATION
role`, and `DROP SCHEMA [IF EXISTS] name [, …] [RESTRICT | CASCADE]`, with
PostgreSQL 17's errors: 42P06 for a taken name (`public` and
`information_schema` too; `IF NOT EXISTS` gives the NOTICE), 42939 for a
`pg_` name, 42704 for a missing role, 3F000 for a missing schema (the
statement drops nothing), 2BP01 for `pg_catalog` and for a schema holding
any object under RESTRICT, with PostgreSQL's DETAIL and HINT. `CASCADE`
(stage 5) drops every object of the schemas and every view that reads
one, in one batch, with PostgreSQL 17's NOTICE `drop cascades to …`; in
a transaction, one that would drop a table an open cursor reads is
55006. A schema's owner is the
`AUTHORIZATION` role, or the bootstrap role; `DROP ROLE` of an owner is
2BP01. A relation, procedure or type is named alone (in `public`, the one
schema of the path) or `schema.name`, everywhere a name is resolved; a
column `s.t.c`. A missing schema is 42P01 where a statement reads
(`relation "s.t" does not exist`), 3F000 where it creates or drops, and the
`IF EXISTS` NOTICE `schema "s" does not exist, skipping`. A relation in
`pg_catalog` is 42501.
- `CREATE SCHEMA` with elements (stage 2): `CREATE TABLE`, `VIEW`,
  `INDEX`, `SEQUENCE` and `GRANT`, run in PostgreSQL 17's order
  (sequences, tables, views, indexes, grants) as one statement, all or
  nothing. What an element creates, and an index's table, is the new
  schema's; one qualified by another schema is 42P15, a temporary one
  42P16. A name an element reads resolves in the new schema, then
  `public`. A view element stores its own text, cut from the statement.
- Refused (0A000): `IF NOT EXISTS` with elements (as PostgreSQL), `CREATE
  TRIGGER` as an element, a view element whose text cannot be cut out of
  the statement (a `CREATE` or `GRANT` written inside it outside
  parentheses, as in `SELECT a AS create`, which PostgreSQL takes), a
  column `s.t.c` in a view element (`s` the new schema or `public`) whose
  query names `t` unqualified and unaliased, which PostgreSQL reads in the
  schema the path finds `t` in (qualifying the `FROM` item works), `DROP
  SCHEMA public` and `information_schema`, a domain, type or procedure in
  `pg_catalog` and anything in `information_schema` or `pg_temp`,
  `CREATE SCHEMA AUTHORIZATION CURRENT_USER` without a name, a name of
  three parts (a database), a schema-qualified column in a subquery that
  names no unaliased item of the subquery's own `FROM` (PostgreSQL may
  read it in the query around), and one table name from two schemas in one `FROM` with
  neither aliased, `GRANT`/`REVOKE … ON SCHEMA` and `ON ALL … IN SCHEMA`
  (#873), and `SET`, `SHOW` and `RESET search_path` (#871).

**Type names.** A type in a column definition, a cast, `CREATE DOMAIN`, a
procedure's argument or a procedure's variable is named alone or qualified
by `pg_catalog.` (`pg_catalog.int4`, as libpg_query spells `INTEGER`). No
other schema holds a type the layer can use, so any other qualifier is
refused as PostgreSQL 17 refuses it (`type_schema` in `sql/parser.cppm`,
then the binder): `public.`, `information_schema.` and `pg_toast.`,
schemas PostgreSQL always has, are 42704 `type "public.int4" does not
exist`; a user schema's is 42704 `type "s.d" does not exist` (0A000 when
`s.d` is a domain or shell type, which no column takes yet), and a schema
that does not exist 3F000 `schema "foo" does not exist`, before an array's
brackets are refused;
three parts are 0A000 `cross-database references are not implemented`
(*Known differences*), four or more 42601 `improper qualified name (too
many dotted names)`. `pg_catalog.` before a spelling the grammar turns
into a type (`integer`, `int`, `smallint`, `bigint`, `boolean`, `real`,
`float`, `double`, `decimal`, `dec`, `character`), quoted or not, is 42704
`type "pg_catalog.integer" does not exist`, as no type of pg_catalog has
that name. A `CREATE PROCEDURE` argument's type is named without quotes
(`type public.int4 does not exist`), as PostgreSQL names it there. A
procedure's variable is checked at its call, as the rest of its
declaration is (*Known differences*); an integer `FOR` loop's own
variable, which libpg_query writes as `pg_catalog."integer"`, is `int4`.
`CREATE TABLE IF NOT EXISTS` of a name in use skips the statement before
it reads the definition, as PostgreSQL does, so the parser keeps a
definition's refusal (`CreateTable::refusal`) for the binder to raise only
when the name is free (`catalog_design.md`, *Name resolution with
schemas*).

**NUMERIC without a precision.** `NUMERIC`, `DECIMAL` and `DEC` without a
precision declare a column of any digits, stored as PostgreSQL's
`numeric_out` text, the scale kept (`types_design.md`, *NUMERIC without a
precision*): written by `INSERT` (a number literal of any digits, a string
or a `$n` read as `numeric_in` reads it, an integer or `DECIMAL` constant
at its scale, another `NUMERIC` by `INSERT … SELECT`), `UPDATE`'s `SET` of
those or of another `NUMERIC` column, `DEFAULT` and `ADD COLUMN`; read,
returned by `RETURNING` and a view, tested by `IS [NOT] NULL` and `CHECK`
over that, counted by `COUNT`, and carried by `CASE` and `COALESCE` of
`NUMERIC`s and `UNION ALL`. Text that is no number is 22P02, past
numeric's format 22003, not UTF-8 22021; a boolean, string or date value
`INSERT` or `SET` assigns, or a boolean `DEFAULT`, is 42804, as in
PostgreSQL (a date `DEFAULT` is 42601, as any non-literal `DEFAULT` is). A
comparison with a boolean, string or date value is 42883, a string literal
that is no number 22P02, as in PostgreSQL. A Python `decimal.Decimal`
parameter is sent as its text, so a string column stores it where
PostgreSQL refuses it (42804). Refused (0A000) until it has an order and arithmetic: a key,
`UNIQUE`, index or `FOREIGN KEY` column of it; comparing it (a `WHERE`, an
`ON`, `USING`, `NATURAL`, `NULLIF`, `IN`, `BETWEEN`, a `$n` beside it);
`ORDER BY`, `GROUP BY`, `DISTINCT`, `UNION`, `INTERSECT`, `EXCEPT`; `+`,
`-`, `*`, `/`, `%`, unary `-`, `abs`, `||`; an aggregate but `COUNT`, and
`COUNT(DISTINCT …)`; a cast of it or to it (`'1.5'::numeric`,
`$1::numeric`), but a `$n` cast to it assigned to it; a `DOUBLE PRECISION` or `REAL` value assigned
to it, and it assigned to a column of another type; a subquery of it; a
domain or procedure argument of it.

**jsonb.** `jsonb` (also `pg_catalog.jsonb`) declares a column of any
JSON value, stored in its binary layout and returned as `jsonb_out`'s text
(`jsonb_design.md`, stage 1; `types_design.md`, *jsonb*): written by
`INSERT` (a string literal, read by `jsonb_in` when the statement is
prepared, so its 22P02 comes first with PostgreSQL's DETAIL and CONTEXT; a
`$n`, typed jsonb, read when it runs; `$n::jsonb`; another jsonb by
`INSERT … SELECT` and `CREATE TABLE AS`), `UPDATE`'s `SET` of those or of
another jsonb column, `DEFAULT` (a string, or a string cast to jsonb:
`DEFAULT '{}'::jsonb`, the one cast a `DEFAULT` takes) and `ADD COLUMN`;
read, returned by `RETURNING`, a view and `UNION ALL`, tested by `IS [NOT]
NULL` (JSON `null` is not SQL NULL), counted by `COUNT`, and carried by
`CASE` and `COALESCE` of jsonb values. A string value (a column, a
constant or a `$n` of a string type) cast to jsonb is read by `jsonb_in`
when it runs (`bc_jsonb_in`), and a jsonb cast to `text` or `varchar` of
no length is its text (`bc_jsonb_text`). PostgreSQL's errors come first: a
text value assigned to it, or a number or boolean `DEFAULT`, 42804 with its
HINT; a cast to it of another type, or of it to `bytea`, a date or a time,
42846; arithmetic of it but `-` with text or an integer, and `MIN`,
`MAX`, `SUM` and `AVG` of it, 42883; a comparison with another type,
42883. A jsonb constant assigned to a string column is its text, and one
with a typed text under `||` is text concatenation. Refused (0A000) until
stage 2: a comparison of it, of constants too (a literal beside it read
first, 22P02), `IN`,
`ORDER BY`, `GROUP BY`, `DISTINCT`, `COUNT(DISTINCT …)`, `UNION`,
`INTERSECT`, `EXCEPT`, and a subquery of it; until the next phase: a key,
`UNIQUE`, index or `FOREIGN KEY` column of it; until stage 4: the
operators PostgreSQL has for it but the accessors (`@>`, `<@`, `?`, `?|`,
`?&`, `#-`, `@?`, `@@`, refused by the parser whatever the operands; `||`
and `-` of a jsonb, by the binder) and the JSON functions but
`jsonb_typeof` and `jsonb_array_length` (named `jsonb_…` or `json_…`,
`to_json`, `to_jsonb`, `row_to_json`, `array_to_json`), its casts to
`numeric(p, s)`, and to `varchar(n)`, `char` and `name`, which cut or pad; a jsonb column's value
assigned to a string column (`INSERT … SELECT v::text` works); `json`
(stage 1c), a domain, a procedure argument and a PL/pgSQL variable of
jsonb. A form the parser takes for no type keeps its 42601 with jsonb:
`IN`, `BETWEEN`, `IS DISTINCT FROM` and `LIKE` in a select list, `COLLATE`
(PostgreSQL's is 42804), a cast in `UPDATE`'s `SET`.

**jsonb's accessors and casts out** (`jsonb_design.md`, stage 3). `j -> k`
and `j ->> k` with a text key or an integer index, `j #> p` and `j #>> p`
with a path, `jsonb_typeof(j)` and `jsonb_array_length(j)` (also
`pg_catalog.`-qualified), and casts of a jsonb to `boolean`, `smallint`,
`integer`, `bigint`, `real`, `double precision` and `numeric` of no
precision, of columns, `$n` and constants alike, wherever the select list
takes a value: the select list, `WHERE` and `ON` as evaluated filters
(*Expressions in WHERE, ON and HAVING (stage P)*), `ORDER BY` of an
expression, and the argument of an aggregate (`sum((j -> 0)::int)`). `->`
and `#>` give jsonb, the others text (`jsonb_array_length` an integer).
Operands resolve as PostgreSQL 17's `oper_select_candidate`
(`jsonb_design.md`, *Operator resolution for jsonb, with unknown literals
and parameters*): an unknown literal or `$n` on the right of `->` is a text
key, a `smallint` or `integer` an index, a string type (`varchar`, `name`,
`char`) a key; `bigint`, `numeric` and any other type 42883 (`operator does
not exist: jsonb -> bigint`), as is a left operand of another type (`text
-> unknown`); a left `$n` or unknown literal 42725 (`operator is not
unique: unknown -> unknown`). The path of `#>` (*Path arguments*) is a
string literal read as `array_in` reads a `text[]` when the statement is
prepared (22P02 `malformed array literal` with its DETAIL, 54000, 2202E),
`ARRAY[…]` of text constants, text values and `$n` (each typed text; an
empty `ARRAY[]` 42P18, an integer element 42883 `jsonb #> integer[]`), or a
`$n` typed `text[]` (OID 1009 in Describe), read at bind, so its error
comes whether a row reaches it or not; a typed `text` there is 42883. A
constant expression is folded when the statement is prepared, as
PostgreSQL folds it, its error then. The results are PostgreSQL's
`jsonb_object_field`, `jsonb_array_element` and `jsonb_get_element`: NULL
for a missing key, a key of a non-object, an index outside the array (from
the end when negative), and for `#>` at a scalar mid-path, a NULL element
or a non-integer index of an array; a scalar is an array of itself to `->
i` (`'5'::jsonb -> 0` is `5`), not to `#>`; `->>` of a JSON null is SQL
NULL and of a string its de-escaped text. Errors: 22023 `cannot get array
length of a scalar` (`of a non-array`), 22023 `cannot cast jsonb string to
type integer` and the rest, the target's 22003 range error
(`integer out of range`, `"1e400" is out of range for type double
precision`). A cast to an integer rounds half away from zero. An accessor
names its column `?column?`, a cast of one its type (`int4`), as
PostgreSQL does. Not built with them: `UPDATE … SET` of an accessor and a
`GROUP BY`, `DISTINCT` or `COUNT(DISTINCT …)` of one, the layer's limits
for any expression (42601, 0A000); `ORDER BY` of a jsonb result (stage 2);
`ARRAY[ARRAY[…]]` and a cast to `text[]` (42601); `MAX` and `MIN` of text,
which the layer has for no text value.

**NAME.** `NAME` declares a column of PostgreSQL's identifier type
(`types_design.md`, *NAME*): text of at most 63 bytes, a longer value cut
at a character boundary wherever it is written or read as a `name` (a
literal or a `$n` beside a `NAME` column, `'…'::name`, `$1::name`, a
`DEFAULT` literal). It is a string in every rule the dialect has for
`TEXT`: compared (a key, `UNIQUE` and index column too), ordered, grouped,
`DISTINCT`, `LIKE`, `||` and the string functions (giving `text`), `COUNT`,
and assigned to and from the other string types. A `NAME`, `TEXT` or
`VARCHAR` value cast to `text` or `varchar` of no length, or a `NAME` to
`name`, keeps its bytes; a constant or `$n` is cast as any is. Its results are `name` (OID 19), and
a `$n` compared with it is typed `name`. Its errors are PostgreSQL's: no
operator with a number (42883), no `SUM` or `AVG` (42883). `CASE`,
`COALESCE` and a set operation resolve a `NAME` with another string type
as PostgreSQL does, the first's type winning, so a `TEXT` first is
`text`; where the result would be a `name` made of another string's
values (`COALESCE(name_col, text_col)`, `SELECT name_col UNION SELECT
text_col`), which PostgreSQL cuts to 63 bytes, it is refused (0A000).

**Refused**, with a message and the node's position: everything not above.
That includes:
- a `NUMERIC` column without a precision compared, ordered, grouped,
  computed, aggregated (but by `COUNT`) or cast, or as a key (0A000,
  *NUMERIC without a precision*);
- a jsonb value compared, ordered, grouped, `DISTINCT`, or as a key, every
  operator and function of jsonb but its accessors, `jsonb_typeof` and
  `jsonb_array_length`, its casts to `numeric(p, s)`, `varchar(n)`, `char`
  and `name`, and the `json` type (0A000, *jsonb*, *jsonb's accessors and
  casts out*; `jsonb_design.md`, *Stages of jsonb*);
- `IS DISTINCT FROM`, and a predicate or a constant in `HAVING`; a
  `NULL` constant under `NOT`; in a `WHERE` or `ON`, `random()` (42601), an
  aggregate (42803), `expr IN (SELECT …)` of an expression, and `CURRENT_DATE`
  compared with other than a column (*Expressions in WHERE, ON and HAVING
  (stage P)*, *What stays refused*);
- correlated subqueries but the slice above (#836), nested subqueries and the others above, a subquery in `FROM`, and a derived relation but a grouped view aggregated over;
- `DISTINCT ON`, `DISTINCT` of a constant or over groups it does not select
  every key of, an aggregate of `DISTINCT` values of an expression (0A000),
  `GROUP BY` of an expression, and `HAVING` of anything but the
  comparisons above;
- a `FULL JOIN` that is not the whole `FROM` of two tables (#712), or under
  an aggregate, `GROUP BY`, `DISTINCT`, `OFFSET`, `INSERT … SELECT` or a set
  operation, or of a grouped view, an outer join's `ON` comparing an expression of columns,
  more than 32 tables (0A000, *Known differences*), a `RIGHT JOIN` of a join that has an outer join,
  an outer join in parentheses or of a join in parentheses, `USING` or `NATURAL` in
  parentheses or after a comma,
  `FOR UPDATE` of a join (#697), an alias of a join, an output name in `GROUP BY`;
- `RETURNING` in a procedure; `VALUES` outside `INSERT`; `OFFSET` on a set operation, in one of
  its arms or in a view;
- `ON CONFLICT ON CONSTRAINT`, a `WHERE`, an
  expression, a collation or an operator class in `ON CONFLICT`'s target,
  `DO UPDATE … WHERE`, and `ON CONFLICT` into a view or in a procedure
  (0A000); `INSERT … SELECT … ON CONFLICT DO UPDATE`, `excluded` inside an
  expression, and `excluded.col` into a column of another type PostgreSQL
  assigns to (42601, not yet);
- `INSERT … SELECT` of an aggregated, grouped or `DISTINCT` query or a set
  operation (#682), of a value outside its column's type or family that
  PostgreSQL assigns (a conversion through text, `CHAR(n)` into another
  string type or length, a `DECIMAL` of another scale or into or out of an
  integer or float, a date or time into another date or time type, #824),
  or in a procedure (0A000);
- `CASE` or `COALESCE` of a float beside another number, a `BETWEEN` or
  `IN` as a value, `GREATEST` and `LEAST`, `CASE` outside the plain select
  list (#685);
- the quotient of a numeric other than constants, arithmetic and
  comparison of `REAL` and `DOUBLE PRECISION` as values, an expression in
  `GROUP BY`, `SET`, an aggregated select list or the `ORDER BY` of a
  grouped or aggregated query (#685, #686), `ORDER BY random()` and
  `ORDER BY t.*`;
- `OVERLAY` of columns and parameters,
  `||` of a boolean, float or date value, `LENGTH`, `CONCAT`,
  `REPLACE`, the regular-expression `SUBSTRING`, `bytea` string functions
  (#671);
- a `CASE`, `COALESCE` or set operation whose result is a `NAME` made of
  another string type's values (0A000, *NAME*); `MIN` and `MAX` of a
  `NAME`, as of every string type;
- a cast of anything but a constant, a parameter, or a string value to
  `text` or `varchar` of no length (a `NAME` to `name`); a cast of a
  parameter to `DECIMAL`, `VARCHAR(n)` or `CHAR(n)`, to a type another use
  of it does not give it, or, in `VALUES` or `SET`, to other than its
  column's type; a cast parameter inside an expression in `SET` or
  `HAVING`, or in an aggregated select list; a numeric literal of more than 18 digits, a string literal in arithmetic (two string literals under one operator are
  PostgreSQL's 42725, operator is not unique), unary `-` or `abs`
  of a parameter, and a constant beside an aggregate;
- `EXTRACT(EPOCH FROM …)` and `EXTRACT(JULIAN FROM …)` of a timestamp,
  and `EXTRACT` of a unit given other than as a name or a string;
- date and time arithmetic other than `DATE - DATE`: `date ± integer`,
  `date + time`, any operator whose result is an `interval` (PostgreSQL has
  them; the layer has no `interval` type), and a `$n` or an unknown literal
  beside a date or time value under `+`, `*` or `/`.

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
  `DATE '2026-10-25' - DATE '2026-01-01'` is the integer 297, `DATE
  '2026-01-01' - NULL` an integer NULL.
- A comparison of constants (`=`, `<>`, `<`, `<=`, `>`, `>=`) folds to a
  `boolean`, NULL if either side is: numbers exactly whatever their
  scales, a float with NaN above every value, strings and bytes bytewise
  (*Known differences*), `FALSE` before `TRUE`, dates and times of one
  type by value. A string literal beside a typed constant is read as that
  constant's type (`1 < '2'`); two compare as text. Types PostgreSQL has no
  operator for are 42883. A timestamptz beside a timestamp or a date
  compares as timestamptz (the timestamp read as UTC, the date as midnight
  UTC); a time beside a time with time zone as a time with time zone (at
  offset 0); a time with time zone beside a date or a timestamp is 42883;
  a date beside a timestamp is not taken yet.
- A cast of a string literal or `NULL`: `DATE '…'`, `'…'::date` and
  `CAST('…' AS date)` alike read the string with the type's input function,
  a `DECIMAL(p, s)` rounded to its scale and range-checked (22003), and are
  of that type. A cast to `VARCHAR(n)` or `CHAR(n)`, which PostgreSQL's cast
  truncates or pads, is not taken yet.
- A cast of any other typed constant follows PostgreSQL's cast table
  (`pg_cast`), and its I/O casts to and from a string:
  - a number to any number type: an integer to a narrower integer range
    checked (22003); a numeric to an integer rounded half away from zero, a
    float to the nearest even (`rint`), NaN and infinity 22003; to a
    `DECIMAL(p, s)` through its text, rounded to the scale (a float at 15
    digits, 6 for `real`, as `float8_numeric` reads it); a float to `real`
    22003 past its range;
  - `boolean` to `integer` (1 or 0) and `integer` to `boolean` (non-zero is
    true), and no other integer type either way;
  - any type to `text` by its output function (`true`, not `t`); a float is
    not taken yet, since the layer's float text is not PostgreSQL's;
  - a typed string to any type by that type's input function;
  - every other pair is 42846, a NULL of it too (`NULL::integer::date`).
- Integer arithmetic of constants is checked against its result type's
  range: `smallint + smallint` is a `smallint` and 22003 past 32767.
- A cast of a date, time, timestamp or timestamptz constant: to text its
  text; to its own type itself; a timestamp or timestamptz to a date (the
  floor in UTC, so 1969-12-31 23:00 is 1969-12-31) or a time; a date to a
  timestamp or timestamptz (22008 if the instant passes 64 bits); a
  timestamp to a timestamptz and back (the same microseconds, the session
  being UTC); a time with time zone to a time (its offset dropped), and a
  time or a timestamptz to a time with time zone (the time, or the
  timestamptz's UTC time, at offset 0). A time to a date, timestamp or
  timestamptz, a date to a time, a time with time zone to a date or a
  timestamp, and a date or a timestamp to a time with time zone are 42846,
  as in PostgreSQL. A time with time zone constant assigned to a `TIME`
  column drops its offset, and a timestamptz constant assigned to a
  `TIME WITH TIME ZONE` column is its UTC time at offset 0, PostgreSQL's
  assignment casts; a date or time constant assigned to a column of a
  date or time type it has no cast to is 42804, `column "c" is of type …
  but expression is of type …`.
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

### The time

`CURRENT_DATE`, `CURRENT_TIME[(p)]`, `LOCALTIME[(p)]`, `LOCALTIMESTAMP[(p)]`, `CURRENT_TIMESTAMP[(p)]` and `now()` are the one
constant a plan cannot fold: a plan is cached by its text, so a value
folded at bind would be the time of the first run. The binder gives each
use a slot among the statement's constants (`bound::ClockSlot`), marked as
the time, and the API fills it on every run (`Database::plan_params`),
from one reading of the clock per statement, so two uses in a statement
are one instant.

- **Which time.** A statement in a transaction reads the time the
  transaction began, as PostgreSQL's `CURRENT_*` and `LOCALTIME*` do
  (`transaction_timestamp()`); an autocommit statement, the time it began.
- **The clock.** `Database` reads the system clock, microseconds since the
  Unix epoch, through one function a test replaces
  (`clock_for_testing`), so the tests fix the time.
- **The zone.** The layer has no time zone: the time is UTC, the date the
  UTC date. A PostgreSQL session set to another `TimeZone` differs.
- **Precision.** `(p)` rounds to p fractional digits, half up, as
  PostgreSQL does; above 6 it is 6 (PostgreSQL warns). A time can round to
  24:00:00, as there.
- **With a time zone.** `CURRENT_TIMESTAMP[(p)]` and `now()` are `timestamp
  with time zone`, as in PostgreSQL: the same instant as `LOCALTIMESTAMP`,
  written with `+00`. `CURRENT_TIME[(p)]` is `time with time zone`
  (`types_design.md`, *TIME WITH TIME ZONE*): `LOCALTIME`'s time at offset
  0, the session's zone, written with `+00` (`ClockSlot::Kind::TimeTz`).
- **Assigned, and as a column's `DEFAULT`.** `VALUES` and `SET` assign
  the time to a column by PostgreSQL's assignment cast:
  - a `DATE` takes the date of `CURRENT_DATE` or of a timestamp;
  - a `TIME` takes `LOCALTIME`, `CURRENT_TIME` (its offset dropped) or a
    timestamp's time, at the precision;
  - a `TIME WITH TIME ZONE` takes `CURRENT_TIME`, `LOCALTIME` or
    `CURRENT_TIMESTAMP`'s time, at offset 0;
  - a `TIMESTAMP` or `TIMESTAMPTZ` takes either timestamp.

  A type with no such cast is 42804 (`CURRENT_TIME` to a date or a
  timestamp, `CURRENT_DATE` or `LOCALTIMESTAMP` to a time with time zone), worded
  as PostgreSQL words it: `but default expression is of type` for a
  `DEFAULT`, `but expression is of type` for a value assigned. A date to a timestamp, which is
  midnight, is refused, and so is the time to a `TEXT`, `VARCHAR` or
  `NAME` column, which PostgreSQL assigns by the I/O cast (42601). A column's `DEFAULT` of the clock is the same
  assignment, made where the `INSERT` leaves the column out or writes
  `DEFAULT` (`catalog_design.md`, *What a table records*).
- **Compared in a `WHERE`.** A column compared with the time, on either
  side, inside `AND`, `OR` and `NOT`, in a `SELECT`'s, `UPDATE`'s,
  `DELETE`'s or subquery's `WHERE`, is a comparison with a parameter of
  the column's type that the API fills on every run, so the plan's key
  range is the run's. The pairs are:
  - a `DATE` with `CURRENT_DATE`, a `TIME` with `LOCALTIME`, a `TIME WITH
    TIME ZONE` with `CURRENT_TIME` or `LOCALTIME` (read as a time with
    time zone, as PostgreSQL casts it);
  - a `TIMESTAMP` or `TIMESTAMPTZ` with `CURRENT_TIMESTAMP`, `now()` or
    `LOCALTIMESTAMP`. PostgreSQL compares a timestamp with a timestamptz
    in the session's time zone; the layer's is UTC, so the two are the
    same instant and the comparison is the plain one.

  A pair PostgreSQL has no operator for (an integer or a text with any of
  them, a `TIME` with a date or a timestamp, a `DATE` with `LOCALTIME`)
  is 42883, its types named in the order written and its operator as
  written: the parser writes `<>` and `!=` as `NOT (=)`, and the
  comparison under that `NOT` remembers it was a `<>`
  (`ast::Compare::ne`), as a typed constant's 42883 does. A `DATE` with a
  timestamp, or a timestamp with `CURRENT_DATE`, compares the date's
  midnight, and a `TIME` with `CURRENT_TIME` compares the column as a time
  with time zone: both are refused (42601).
- **Compared in a select list.** A column or the time compared with the
  time of its own type, as a value; `LOCALTIME` beside a time with time
  zone is read as one (`Binder::clock_as_timetz`). Two other date and time
  types are not compared as a value yet (42601).
- **The placeholder.** The value a clock slot holds in the bound
  statement is 0, a placeholder: nothing folds it or checks it as the
  statement's value.
- **Refused.** The
  time inside an aggregated select list, in a value expression of a
  `WHERE` (`d < CURRENT_DATE - 1`), compared in a `WHERE` with anything
  but a column (a constant, a parameter or the time:
  `CURRENT_DATE > DATE '2020-01-01'`, `$1 < CURRENT_DATE`), in a
  procedure's statement (0A000 at the first call,
  `procedures_design.md`), and `ADD COLUMN` with a `DEFAULT` of the clock.

### Session functions

`CURRENT_USER`, `CURRENT_ROLE`, `USER`, `SESSION_USER`, `CURRENT_CATALOG`
and `CURRENT_SCHEMA`, the standard's niladic forms (no parentheses, as in
PostgreSQL), are `name` values (`types_design.md`, *NAME*), named by
the word, as PostgreSQL names them. `current_database()` and
`current_schema()`, PostgreSQL's call forms, are `CURRENT_CATALOG` and
`CURRENT_SCHEMA`, named by the call.
- **What they return.** Every session is the bootstrap superuser
  (*Roles and privileges*), so the four user functions return the same
  name: the session's user. Over the wire that is the startup packet's
  `user`, as `session_authorization` reports it; in the embedded API it is
  `ExecutionOptions::user` for a statement outside a transaction and
  `TransactionOptions::user` in one, and `bytecask` (`kBootstrapUser`)
  where none is given. `CURRENT_CATALOG` is the database's name
  (`DatabaseOptions::name`, the server's `--database`, by default the
  directory's last component), PostgreSQL's `current_database()`.
  `CURRENT_SCHEMA` is `public`, the one schema, until schemas exist; then
  it is the first schema of the search path. A name past 63 bytes is cut,
  as a `NAME` is.
- **Bound per run.** Each use is a slot among the statement's constants,
  as the time is (`bound::ClockSlot`, kinds `User`, `Catalog`, `Schema`),
  filled by `Database::plan_params` on every run, so one cached plan
  serves every session and user. The placeholder is an empty string.
- **Where they go.** In a select list, in value expressions (`||`, the
  string functions, `CASE`, `COALESCE`, a comparison as a value), cast to
  `text` or `name`, assigned in `VALUES` and `SET`, and compared in a
  `WHERE` with a column, on either side: a string column compares with
  them as text, any other is 42883 (`operator does not exist: integer =
  name`).
- **As a column's `DEFAULT`.** A `ClockDefault` of the session function
  (`catalog_design.md`), filled where the `INSERT` leaves the column out
  or writes `DEFAULT`, as the clock's, in a transaction or a server block
  too. A `NAME`, `TEXT`, `VARCHAR(n)` or `CHAR(n)` column takes one, by
  PostgreSQL's assignment cast; a value too long for a `VARCHAR(n)` is
  22001 when the row is written. Any other type is 42804 (`column "a" is
  of type integer but default expression is of type name`).
- **Refused,** each with 0A000 where PostgreSQL runs it: compared in a
  `WHERE` or `ON` with anything but a column (`WHERE CURRENT_USER = 'x'`:
  the planner plans no conjunct that compares no column); in a `CHECK`
  constraint, in a procedure's expression or statement (at the first
  call), in the select list of an aggregated query; `ADD COLUMN` with
  such a `DEFAULT`; `SET ROLE` and `SET SESSION AUTHORIZATION` (as every
  `SET` the layer lacks). `SYSTEM_USER` is not taken (42601).

### Expressions over columns

A select-list item that is not a column, a constant, an aggregate or a
subquery by itself is a *value expression*: a tree of columns, constants,
scalar subqueries, `$n`, `+ - * /`, the six comparisons, `IS [NOT] NULL`,
`AND`, `OR`, `NOT`, `CASE`, `COALESCE` and `NULLIF`. The binder types it
(`Binder::value`) and gives the plan one `Project` output; the search plans
it as a result column and codegen evaluates it for each row
(*planner_design.md*, *Expressions over columns*). `||` and the string
functions plug into the same tree (#671); they are refused over columns
until then.

- **Result types are PostgreSQL's, from one function (`arith_type`), for a
  constant and a column alike.** Two integers give the wider of the two
  (`smallint * smallint` is `smallint`, `smallint * integer` `integer`); with
  a `DECIMAL`, `+` and `-` are at the finer scale and `*` at the sum of the
  scales. A comparison, `IS NULL`, `AND`, `OR` and `NOT` are `boolean`. The
  output is named `?column?`, or its `AS` name.
- **NULL is SQL's.** An arithmetic operator or a comparison with a NULL
  operand is NULL and never runs; `IS [NOT] NULL` is never NULL; `AND` is
  false where an operand is false, else NULL where one is NULL; `OR` the
  mirror; `NOT` of NULL is NULL.
- **Errors.** A `smallint` or `integer` result outside its type, and a
  `bigint` one outside int64, is 22003 (a `bigint` product says "integer out
  of range" where PostgreSQL says "bigint"); a zero divisor is 22012 and is
  raised only for a row that evaluates it. `AND` and `OR` evaluate an
  operand only while no earlier one settles the result, as PostgreSQL does
  left to right, so `b <> 0 AND a / b > 1` raises nothing.
- **Comparisons** need operands of one class (42883 otherwise: `integer <
  text`): numbers meet at the finer scale, text and `bytea` compare bytewise
  (`COLLATE "C"`), booleans `FALSE` before `TRUE`, dates and times of one
  type by value. A subquery's value compares as a constant does; no row is
  NULL.
- **A `$n` takes its type from the column beside it** (`i / $1`, `i < $1`),
  as in a `WHERE`; one beside another parameter, a constant or an
  expression is refused (42601). An unknown literal or `NULL` takes the
  other operand's type (`i + '1'`, `z = NULL`).
- **Date and time arithmetic.** `DATE - DATE` is an `integer`, the days
  between them: a date is a count of days from 1970, so it is one
  subtraction, exact over the type's whole range (4714-11-24 BC to
  5874897-12-31, a difference of at most 2147483493). A `$n`, an unknown
  literal or `NULL` beside a `DATE` under `-` is read as a `DATE`, as
  PostgreSQL resolves it (`d - $1`, `d - '2026-01-01'`, `'2026-01-01' -
  DATE '2020-01-01'`). Under `+`, `*` or `/` the operator is decided as
  PostgreSQL resolves it for an operand of type unknown, before that
  operand is read as the other's type, so a literal that is not a valid
  date or time answers as one that is (`temporal_untyped_operator`):
  `date + unknown` is 42725 (PostgreSQL has `date + integer`, `+
  interval` and `+ time` and cannot choose), `date` or `timestamp` under `*`
  or `/`, and `unknown / time`, are 42883; the forms PostgreSQL runs (`time
  + unknown` and `timestamp + unknown` as an interval, a time scaled by
  a number) are refused (42601). Of the other operators over date and time
  types (`temporal_operator`), those PostgreSQL has, its implicit casts
  included, are refused (42601): `date ± integer`, `date + time`, a
  timestamp's or a time's difference (an `interval`), `date - timestamp`,
  `time * number`. A pair PostgreSQL has no operator for is 42883 (`date +
  date`, `date * date`, `date - numeric`, `date - bigint`, `integer -
  date`), and `time + time` is 42725, ambiguous there. A `TIME WITH TIME
  ZONE` has no `T - T`: its `-` reads an unknown as an `interval`, refused
  (42601) as the other interval forms are, and so are `timetz ± time` and
  `date + timetz` (a timestamptz); `timetz + unknown` is 42725, `unknown -
  timetz`, `timetz - timetz`, `timetz + timetz`, `timetz * number` and
  `timetz` with a timestamp are 42883.
- **Held within 18 digits.** A numeric operand brought to a finer scale
  (`d + i`, `d < i`) must stay inside the 18 digits a `DECIMAL` holds, by
  its type, or by its value for a constant; a `bigint` beside a `DECIMAL`
  that needs rescaling is refused. A product past int64 is 22003, where
  PostgreSQL's numeric is exact.
- **`CASE`, `COALESCE` and `NULLIF`** over columns, parameters and
  subqueries. A simple `CASE x WHEN v` is a searched one over `x = v`
  and `NULLIF(a, b)` is `CASE WHEN a = b THEN NULL ELSE a END`.
  - *Conditions.* A `WHEN` is any boolean expression the tree holds
    (`AND`, `OR`, `NOT`, `IS NULL`, a comparison, a subquery's value); one
    that is not `boolean` is 42804; NULL is not true.
  - *Result type.* The union rule of the constants' fold, over the branches'
    types: unknown literals and `NULL` give way; the typed branches must be of one
    class (42804 otherwise); integers widen; `text` and `varchar` are `text`;
    two floats are the wider float; all unknown is `text`. A `$n` among
    the results takes the type of the first plain column among them (none:
    refused, 42601). An unknown literal that is not a value of the type is
    22P02.
  - *Evaluation.* In order, and a branch only when it is taken: `CASE WHEN
    b <> 0 THEN a / b ELSE 0 END` raises nothing, as PostgreSQL's. So the
    refusal of a result that would raise and is not taken (constants
    only, above) does not apply once a column, parameter or subquery is
    among the branches. A branch of constants still folds when the
    statement is compiled, so `CASE WHEN a > 0 THEN a ELSE 1 / 0 END` is
    22012, as PostgreSQL's (it folds constants in untaken branches too).
  - *Known difference.* A numeric result has one scale, the finest of its
    branches (an integer is scale 0): `COALESCE(price, 0)` over a
    `DECIMAL(10,2)` is `0.00` where PostgreSQL's `numeric` keeps each
    value's own scale and gives `0`. The value is the same.
  - *Refused (42601).* A float beside another number, different date and
    time types, and a numeric result past 18 digits.
- **Refused (42601).** A numeric quotient with a column or an expression in
  it: PostgreSQL's scale follows the operands' values (`select_div_scale`
  reads their leading digits), so one column's quotient has several scales
  and the layer's result type has one. `REAL` and `DOUBLE PRECISION`
  arithmetic and comparison as a value (unary `-` and `abs` of one are
  taken). A cast, an
  aggregate inside an expression (an expression is taken inside an
  aggregate, *Aggregates*), an expression in the select list of an
  aggregated query or under `DISTINCT`, an expression in the `ORDER BY` of
  a grouped or aggregated query, in `GROUP BY`, `SET` or a `WHERE`.

### Expressions in WHERE, ON and HAVING (stage P)

Status: **P1a built; P1b, P2 and P3 design, approved by the owner on
2026-11-22 with every recommendation under *Owner's decisions on stage P*.**
P1a is the *Stages of stage P* table's first row: expression operands in a
`WHERE`, an `ON` and an `UPDATE`'s or `DELETE`'s `WHERE`, tried in written
order. `HAVING` (P3), PG 17's conjunct order (P1b) and an operand of no
column computed once (P2) are not built. It is a dependency of `jsonb_design.md` (*Expressions in WHERE
and ON*; stage 3's `->>` in a `WHERE`) and of Mattermost (#889). The
planner's part is `planner_design.md`, *Expression filters (stage P)*.
Behaviour observed on PostgreSQL 17.12 is cited as "PG 17"; every PG 17
claim was run with `\set VERBOSITY verbose`. Code citations are to
`gustavoamigo/sql-jsonb-type` at fd4fe482.

#### What a WHERE takes today, and the gap

A `WHERE` or `ON` conjunct compares a bare column with a parameter, a
`$n::T` of the column's type, a constant expression (folded), `$n ± k`
beside an INTEGER column, another column, a scalar subquery or the clock
(`Binder::compare`, `sql/binder.cppm:4363-4445`). `IS NULL` and `LIKE`
take a column only (:4285, :4223); a `HAVING` is one comparison of a
grouped column or an aggregate with a parameter or constant (:5708-5745).
Everything else is 42601 `WHERE must compare a column with a parameter or
a constant` (:4427-4429), or `ON must …`, `HAVING must …`, `IS NULL of
other than a column …`, `LIKE of other than a column …`. PG 17 runs all of
these:

| Statement | Layer today | PG 17 |
|---|---|---|
| `WHERE lower(name) = $1` | 42601 | rows |
| `WHERE a + 1 > $1`, `a * 2 < b + 1`, `abs(a) = 1` | 42601 | rows |
| `WHERE name \|\| 'x' = 'Alicex'`, `coalesce(a, 0) = $1`, `CASE … END = 1` | 42601 | rows |
| `WHERE lower(name) LIKE 'a%'`, `a + 1 IS NULL` | 42601 | rows |
| `WHERE a + 0 IN (1, 5)`, `a = $1 - 1 OR b = 2` | 42601 (`$n + k … inside AND, OR or NOT`) | rows |
| `JOIN … ON x.a + 1 = y.b`, `ON lower(x.s) = lower(y.s)` | 42601 | rows |
| `UPDATE … WHERE lower(name) = $1`, `DELETE … WHERE a + 1 > 100` | 42601 | rows |
| `HAVING sum(v) * 2 > 25`, `HAVING sum(v) IS NULL` | 42601 | groups |
| `WHERE bigint_col > $1 - 100` | 42601 (`… only compared with an INTEGER column`) | rows, `$1` integer |

The parser already holds any value expression on both sides of a
comparison (`ast::Compare`, `sql/ast.cppm:175-201`; `parser.cppm:630-639`)
and the select list already evaluates them (`Binder::value`,
:2974-3047; `Gen::eval`, `plan/codegen.cppm:1675-1717`). Stage P connects
the two: an operand that is not one of today's shapes is bound by the
select list's binder and evaluated per row, by the select list's
evaluator, as a filter.

#### Positions and forms stage P takes

- **Positions:** a `SELECT`'s `WHERE`, each `ON` (inner, `LEFT`, `RIGHT`,
  and both arms of a `FULL JOIN`, *FULL JOIN* in `planner_design.md`), the
  `WHERE` of `UPDATE` and `DELETE`, and (stage P3) `HAVING`. Wherever a
  conjunct may stand today, under `AND`, `OR` and `NOT` included.
- **Forms**, each over any value expression the select list takes (its
  operators, `CASE`, `COALESCE`, `NULLIF`, the string functions, `EXTRACT`,
  casts, scalar subqueries, the clock, and in stage 3 the jsonb
  operators):
  - `e1 op e2`, `op` one of `= <> < <= > >=`, either side or both an
    expression;
  - `e IS [NOT] NULL`;
  - `e [NOT] [I]LIKE p [ESCAPE c]`, `e` and `p` text expressions (`LOWER(x)
    LIKE LOWER($1)`, Mattermost's `property_field_store_listing.go:127`
    form), the escape a parameter or a constant as today;
  - `e [NOT] IN (x, y, …)` and `e [NOT] BETWEEN [SYMMETRIC] lo AND hi`, the
    parser's rewrites to `OR` of `=` and to `>= AND <=` (:640-704), each
    item any expression;
  - a BOOLEAN expression standing alone as a conjunct or an operand of
    `AND`, `OR` and `NOT`: `WHERE flag`, `WHERE NOT flag`, `WHERE CASE WHEN
    … THEN … ELSE … END`, `WHERE (a > 0) = b` (open question 2). A bare
    `$n` standing alone (`WHERE $1`) is typed BOOLEAN, as PG 17 types it
    (`pg_prepared_statements`: `{boolean}`);
  - in `HAVING` (stage P3), the same forms over grouped columns, aggregates,
    parameters and constants, under `AND`, `OR` and `NOT`.
- **A comparison of no column** (`WHERE $1 = $2`, `WHERE lower($1) = 'x'`)
  is a filter like any other (stage P1), then a test made once before the
  plan reads (stage P2, `planner_design.md`).

Today's shapes stay as they are where their types agree: a bare column
compared with a parameter, constant, other column, subquery or clock of
its own type binds exactly as it does now and may bound a key. A column
compared with an operand of another type (`d > $1::int`, `d` a
DECIMAL(10,2)) is evaluated, never a key (`planner_design.md`, *The search:
a filter, never a key's bound*). Only what today's gate refuses takes the
new path, so no statement the layer runs today changes its rows; its plan
changes only in the order of its filter conjuncts (*Evaluation order*).

#### What stays refused

Each keeps its SQLSTATE; positions are added where they were missing.
Each refusal of a clause names the clause as PG 17 does.

| Form | SQLSTATE | Why |
|---|---|---|
| An aggregate in `WHERE` (`a + count(*) > 1`) | 42803 `aggregate functions are not allowed in WHERE`, with its position | PG 17 (`parse_agg.c:606`) |
| An aggregate in `ON` | 42803 `aggregate functions are not allowed in JOIN conditions` | PG 17 (`parse_agg.c:592`) |
| A non-BOOLEAN expression as a conjunct | 42804 `argument of WHERE must be type boolean, not type integer`; `argument of JOIN/ON …`, `argument of HAVING …`; as an operand of `AND`, `OR` or `NOT`, `argument of AND …` (`OR`, `NOT`) at the operand's position | PG 17 (`parse_coerce.c:1180`) |
| A column of `HAVING` neither grouped nor in an aggregate (P3) | 42803 `column "t.v" must appear in the GROUP BY clause or be used in an aggregate function` | PG 17 (`parse_agg.c:1492`) |
| `random(lo, hi)` anywhere in a `WHERE`, `ON` or `HAVING` | 42601 `random() in a WHERE, ON or HAVING is not supported yet` | a replay cursor must read the same rows on each run (`cursor_design.md`, *The replay cursor*), as for `ORDER BY random()` (:5328); open question 3 |
| A comparison or arithmetic of REAL or DOUBLE PRECISION as an expression (`coalesce(dbl, 0) > $1`) | 42601, as in a select list today (`comparison_value`, :3527; `arithmetic_value`, :3496) | no float evaluator in `eval` yet; a bare float column compared as today still runs |
| `expr [NOT] IN (SELECT …)`, `expr op ANY/ALL (SELECT …)` of an expression | 42601 `IN, ANY or ALL of a subquery of other than a column` (:4240), as today | `any_of` types the set by a column (:2845-2866); a follow-up issue |
| A `CASE` or `COALESCE` arm that PG 17 drops without folding and whose fold raises (`CASE WHEN false THEN 1/0 …`, `COALESCE(1, 1/0)`) | 42601 `a branch of CASE (COALESCE) that raises an error and is not taken is not supported yet`, as in a select list today | *Constants and CASE arms* below |
| `GREATEST`, `LEAST` | 42601 `expression not supported`, now with its position | no evaluator yet; Mattermost's `SET … = GREATEST(…)` is open question 6 |
| `IS [NOT] DISTINCT FROM`, `= ANY($1)` of an array, `IN` of a row | 42601, now with the position | not a stage P form |
| A jsonb or NUMERIC comparison | as today (`numeric_compared`, :4597-4602) | `jsonb_design.md` stage 2 |
| A correlated subquery inside an expression | as today | #836 |
| `UPDATE … SET col = expr` beyond `col ± x` | as today (:4085-4175) | open question 6 |
| `ORDER BY`, `GROUP BY` of an aggregated query's expression | as today | not a filter |

#### Three-valued logic of an expression filter

A conjunct keeps a row only when it is true. An expression of a NULL is
NULL, so a comparison of it is unknown and rejects the row; `NOT` of
unknown is unknown; `AND` is false when one operand is, `OR` true when one
is; `IS [NOT] NULL` is never unknown. This is today's rule (`CONTRACT.md`,
the three-valued-logic row), now over any operand. PG 17, table `jz_t(a,
b)` = (0,1), (1,NULL), (2,3), (NULL,4), (-1,0):

- `WHERE a + NULL = 1`: no row. `WHERE a + b = a + b`: 3 rows, not the two
  with a NULL.
- `WHERE (a + b > 2) IS NULL`: (1,NULL), (NULL,4). `WHERE NOT (a + b > 2)`:
  (0,1), (-1,0).
- `WHERE NOT (b > 2 AND a = 1)`: (0,1), (2,3), (-1,0); (1,NULL) and
  (NULL,4) are unknown.
- `1 / NULL::int` is NULL, not 22012: the arithmetic helpers are called
  only for a row whose operands are not NULL (`planner_design.md`,
  *Expressions over columns*).

A row NULL-extended by an outer join reads NULL from the other side's
columns, so a `WHERE` above the join that is not strict (`WHERE 10 / v.y >
0 OR v.y IS NULL`, `WHERE v.y * 2 IS NULL`) raises nothing for it and
keeps or drops it by the same rules, as in PG 17 (5 and 4 rows). A strict
one is a difference (*Differences*, *One-table conjuncts of a join*).

#### Types of the operands, parameters and literals

The comparison's legality and its type are the select list's
(`comparison_value`, :3519-3551): what `SELECT e1 < e2` takes, `WHERE e1 <
e2` takes, with the same rescaling of numerics. What it refuses is refused
with the same SQLSTATE: `WHERE s + 1 > 0` is 42883 `operator does not
exist: text + integer`, and `WHERE lower(a) = 'x'` 42883 `function
lower(integer) does not exist` (PG 17). `LIKE` of a non-text expression is
42883 `operator does not exist: integer ~~ unknown` (PG 17). A comparison
or arithmetic of REAL or DOUBLE inside an expression stays 42601, as in a
select list (*What stays refused*).

An untyped operand takes its type from the other side, whatever that side
is, as PG 17 types an unknown:

- **A literal** beside an expression is read as the expression's type:
  `a + 1 = '1.5'` is 22P02 `invalid input syntax for type integer: "1.5"`
  at bind time, `a + 1 > 1.5` compares an integer with a numeric, as the
  select list does today (PG 17 agrees on all three).
- **A parameter** beside an expression is typed by it. Today only a column
  types a `$n` (`operands`/`peer_of`, :3360-3367); stage P lets any typed
  operand do it, in the `WHERE` and, with the same change, in the select
  list (`SELECT lower(name) = $1` is 42601 today). PG 17's types, from
  `pg_prepared_statements`: `lower($1) = s` text; `a + 1 = $1` integer; `a +
  $1 = 2` integer; `s || $1 = 'xa'` text; `a + 1 IN ($1, $2)` and `a + 1
  BETWEEN $1 AND $2` integer, integer; `lower(s) LIKE $1` text. A bad value
  bound later is 22P02 or 22003 at bind, as for a column's `$n` today
  (PG 17: `a + 1 = $1` with '1.5' is 22P02, CONTEXT `unnamed portal
  parameter $1 = '...'`).
- **`$n - k` beside a BIGINT column** types `$n` INTEGER, as PG 17 does:
  `c > $1 - 100` with `c` bigint is `{integer}`, and binding
  1700000000000 is 22003 `value "1700000000000" is out of range for type
  integer` in PG 17. `$1::bigint - 100` is `{bigint}`. Both run in stage P;
  the first is the shape Mattermost writes, and it is refused today
  (:4435-4437).
- **`$n::T` beside a column of another type** (`d > $1::int`) types `$n` as
  T and is evaluated as the select list evaluates `d > $1::int`, rescaled;
  it bounds no key.
- **Two untyped operands:** `$1 = $2` types both text, as PG 17 does; `$1 +
  $2 = a` is 42725 `operator is not unique: unknown + unknown`, PG 17's
  message and HINT. `abs($1) = a` types `$1` double precision in PG 17;
  the layer has no double `abs` of an unknown and refuses it (42601), a
  difference listed below.
- A `$n` an earlier use typed keeps that type, as today.

#### Evaluation order, and errors in rows a filter would reject

An expression can raise an error for a row: 22012 for `1 / a` where `a` is
0, 22003 for an overflow, 22P02 from a cast, 22011 from `substring`, 22025
from `LIKE`'s escape. Whether a row raises depends on which conjuncts were
tried on it first. PostgreSQL says the order is undefined (section
4.2.14). Stage P1a tries them in written order; stage P1b (open question
1) tries them as PG 17's planner orders them. Both rules share this:

- **A row stops at the first top-level conjunct that is not true.**
- **Inside `AND`, `OR`, `NOT`, `CASE` and `COALESCE`** an operand is tried
  only while the result is unsettled, in written order, as the select
  list's evaluator does already. PG 17: `a = 0 OR 1/a > 0` runs; `1/a > 0
  OR a = 0` is 22012; `CASE WHEN a <> 0 THEN 1/a > 0 ELSE false END` runs;
  `a = 1 OR (a + 1 > 0 AND b = 2)` is planned as written. P1b's
  normalisation (below) moves `NOT` and `IN`'s items before this rule
  applies.
- **A row no plan reads raises nothing.** A row a key excludes, a row past
  a `LIMIT` that stops the read, a pair of a join whose key does not
  match: none is evaluated. Which rows a plan reads depends on the key the
  planner chooses, so where a conjunct would raise on a row a key skips,
  the statement's error depends on the plan (*Differences*).
- **An error is the statement's.** A `SELECT` returns it in place of its
  rows (a cursor at the `FETCH` that reaches the row); an `UPDATE` or
  `DELETE` raising one has no effect, as for any run-time error (PG 17: a
  `DELETE` raising 22012 leaves the table unchanged). The SQLSTATE is the
  operator's, through `BC_E_ARITH` and `BC_E_DATA` (`jsonb_design.md`,
  stage 0).

**P1b: PG 17's order.** PG 17 normalises a qual, then sorts its top-level
conjuncts by cost (`order_qual_clauses`, a stable sort). The layer does
the same, in this order:

1. **`NOT` moves inward** (PG 17's `canonicalize_qual` and negator
   operators): `NOT (x OR y)` is `NOT x AND NOT y`, `NOT (x AND y)` is
   `NOT x OR NOT y`, `NOT NOT x` is `x`; `NOT` of a comparison is the
   negated comparison. PG 17: `NOT (1/a > 0 OR a = 0)` is planned `Filter:
   ((a <> 0) AND ((1 / a) <= 0))` and returns rows 2 and -1; `NOT (a IS
   NULL OR 1/a > 0)` is `(a IS NOT NULL) AND ((1 / a) <= 0)`. A marked `IN`
   list (step 3) is one unit: `NOT` of it is PG's `<> ALL`, one conjunct.
2. **Every `AND` reached from the top is flattened** into top-level
   conjuncts, a `BETWEEN`'s included: `1/a BETWEEN 0 AND 5 AND a + 0 <> 0`
   is 22012 in PG 17, and with `a <> 0` it is planned `(a <> 0) AND ((1/a)
   >= 0) AND ((1/a) <= 5)`. `NOT BETWEEN` becomes an `OR` (step 1), and
   `BETWEEN SYMMETRIC` an `OR` of two `AND`s, each one conjunct.
3. **An `IN` list's items are regrouped** as PG 17's `transformAExprIn`
   does: when two or more items read no column (constants, parameters,
   `$1 + 1`), those come first, as one marked unit (PG's `= ANY`), and the
   items that read a column follow in written order; otherwise the items
   stay in written order. PG 17: `a IN (1/b, 0, 5)` is planned `((a = ANY
   ('{0,5}')) OR (a = (1 / b)))` and returns (0,0) and (5,1), where `a =
   1/b OR a = 0 OR a = 5` is 22012; `a IN (1/b, 0)` stays `(a = (1 / b))
   OR (a = 0)`; `a NOT IN (1/b, 1)` is `(a <> 1) AND (a <> (1 / b))`. An
   explicit `OR` is never regrouped.
4. **Top-level conjuncts are sorted by cost, ties in written order.** The
   cost counts each operator and function call once (every built-in the
   layer has is `procost` 1), as PG 17's custom plan sees it:
   - columns, constants, `IS [NOT] NULL`, `AND`, `OR`, `NOT`, `CASE` and
     `COALESCE` cost nothing of their own;
   - **a subtree that reads no column, no clock and no `random()` costs
     nothing**: PG 17's custom plan, its default for a statement's first
     five runs and every simple-query statement, folds parameters to
     constants. `PREPARE q1(int) … WHERE 1/a > 0 AND a = $1 - 1` is planned
     `Filter: ((a = 1) AND ((1 / a) > 0))` and returns a row; `s =
     lower($1)` is costed as `s = 'x'`;
   - the clock costs one call (`now()` and `CURRENT_TIMESTAMP` are calls,
     `procost` 1, and are not folded): `ts < now() AND a + 1 > 0` ties at 2
     and keeps its order either way; `ts < now() - interval '1 day'` costs
     3;
   - a marked `IN` unit costs its left operand once plus half a call per
     item: `a + 0 IN (1, 5) AND a + b > 0` keeps its order (2 against 2);
     `coalesce(a, 0) + 1 IN (1, 2, 3) AND 10/a > 0` puts `10/a > 0` first
     (2 against 2.5); an explicit `a = 0 OR a = 5 OR a = 7` costs 3;
   - a jsonb to text or text to jsonb cast costs two (PG's I/O casts); the
     casts from jsonb to a number or BOOLEAN that stage 3 adds cost one
     (`pg_cast` method f): `(j->'n')::int > 0` costs 3;
   - a subquery: an uncorrelated `EXISTS` (`Truth`) costs nothing (PG's
     one-time filter); `IN (SELECT …)` under `NOT` or `OR` (`InSet`, PG's
     hashed SubPlan) costs one call; a top-level `IN (SELECT …)` or `op
     ANY/ALL (SELECT …)` comes after every other conjunct, as PG 17 runs it
     as a semi-join or a SubPlan after the scan's filter (`a IN (SELECT x
     FROM v) AND 10/a > 0` is a `Hash Semi Join` over `Filter: ((10 / a) >
     0)` and is 22012).

PG 17's plans for the examples:
- `1/a > 0 AND a <> 0` runs, no error: `Filter: ((a <> 0) AND ((1 / a) >
  0))`. The same with the conjuncts written the other way.
- `lower(s) = 'x' AND a = 1` is `Filter: ((a = 1) AND (lower(s) = 'x'))`;
  `a + b + 1 > 0 AND a * 2 > 0` puts `a * 2 > 0` first.
- `(1/a) IS NOT NULL AND a <> 0` keeps its order (one call each) and is
  22012.
- `CASE WHEN a <> 0 THEN 1/a ELSE 0 END > 0 AND b > 0` puts `b > 0` first
  (3 calls against 1); `COALESCE(a, 0) > 0 AND b > 0` keeps its order (1
  against 1).
- `a IN (1,2,3,4) AND a + 1 > 0` keeps its order (2 against 2).

Why PG 17's order and not the written one: a guard written after the
expression it guards (`1/a > 0 AND a <> 0`) is legal SQL that PG 17 runs,
and an error where PostgreSQL returns rows is the visible difference an
application meets. Either order returns the same rows whenever no error is
raised, so the choice never makes a result wrong; it decides only which
statements raise.

**`HAVING`** (stage P3). As PG 17's `subquery_planner` does, a `HAVING`
conjunct that reads no aggregate, no subquery and no `random()` moves to
the `WHERE` and is tried on every input row before grouping: `GROUP BY k
HAVING sum(v) > 100 AND 10/k > 0` is planned `HashAggregate Filter: (sum(v)
> 100) -> Seq Scan Filter: ((10 / k) > 0)` and is 22012 for the k = 0 row.
The rest are ordered as a `WHERE`'s: `100/sum(v) > 0 AND sum(v) <> 0` is
planned `(sum(v) <> 0) AND ((100 / sum(v)) > 0)` and runs (with P1b;
written order before it).

**Constants and CASE arms.** Constants are folded at bind time as today, so
`a <> 0 AND 1/0 > 0` is 22012 even on an empty table, as in PG 17. The
fold follows PG 17's `eval_const_expressions`, as the select list's does
today: a `CASE` arm whose `WHEN` is constant false is dropped, and every
arm after a constant-true `WHEN` or after a non-NULL constant `COALESCE`
argument; every other arm is folded, so a constant error in it raises at
bind, whether or not a row takes it. PG 17: `CASE WHEN a > 0 THEN 1/0 > 0
ELSE true END` and `COALESCE(a, 1/0) > 0` are 22012 on an empty table; the
layer's select list raises 22012 for both today. Where PG 17 drops an arm
whose fold raises (`CASE WHEN false THEN 1/0 > 0 ELSE true END`,
`COALESCE(1, 1/0)`), PG 17 runs and the layer keeps today's refusal, 42601
`a branch of CASE that raises an error and is not taken is not supported
yet` (*Differences*).

#### Binder changes

- **Parser** (`sql/parser.cppm`). A node `predicate()` does not know (a
  bare column, a `$n`, a `FuncCall`, a `CaseExpr`, any other value
  expression) becomes a new `ast::Compare::Kind::Value` whose expression is
  `lhs`, so the AST's walkers (`correlated()`, `correlation()`'s `none()`,
  the view merge, `has_subquery`) see it without a case of their own, where
  it is rejected today (:616, :715). The `OR` an `IN` list makes is marked
  (`ast::Compare::in_list`). `GREATEST`/`LEAST`'s refusal gets its position
  (:508).
- **`Binder::compare`** (:4363-4445). Today's shapes are tried first and
  bind as now. Each refusal below falls through to `value()` for both
  sides and `comparison_value` for the comparison, with the
  untyped-operand rule above, giving `logical::compare(op, lhs, rhs)` over
  any operands:
  - `WHERE`/`ON must compare a column with a parameter or a constant`
    (:4427-4429): falls through.
  - `$n + n and $n - n are supported only compared with an INTEGER column`
    (:4435-4437) and the literal past INTEGER (:4439-4441): fall through.
  - `uncast_param`'s `a cast of a parameter to other than its column's
    type` (:4319-4324, called at :4393): falls through; `value()` types
    `$n` as T, and the conjunct is evaluated (B1 in `planner_design.md`).
    A cast to DECIMAL, VARCHAR(n) or CHAR(n) stays refused by `value()`.
  - The clock compared with other than a column (:4416-4421): the
    `not_supported` falls through (`now() > $1`, `ts < now() - interval '1
    day'`); `CURRENT_DATE`'s `not_yet` stays.
  - A scalar subquery's `comparable` refusal: stays, as in a select list.
- **Inside `AND`, `OR` and `NOT`** (:4276). The guard goes. A comparison
  under a connective whose operand is not a bare parameter, constant,
  column, subquery or clock binds through `value()`, `$n ± k` included (a
  hidden parameter and a constant, evaluated): the `$n ± k` key shape is
  made only for a top-level conjunct, the only place `add_computed` and
  `Checker::predicate_of` look for it.
- **`IS NULL`, `LIKE` and `Value` conjuncts** bind their operand with
  `value()` when it is not a column (:4285, :4223); `LIKE`'s pattern may
  be any text expression, its escape stays a parameter or a constant. A
  `Value` conjunct must be BOOLEAN (42804 otherwise, the message naming
  the clause or the connective, *What stays refused*). A quantified
  subquery of an expression stays refused (:4240).
- **An `IN` list** (P1b) binds its marked `OR` as step 3 of *Evaluation
  order* regroups it: an `Or` whose first operand is the marked unit, an
  `Or` of the no-column items' `=` with `logical::Expr::in_list` set, then
  the column-reading items' `=`.
- **`checked_expr`** (:6494-6532), used only for `WITH CHECK OPTION`
  (:6482), gains a `Value` case (the expression cloned) and nothing else:
  `LIKE` and subqueries stay refused there, as today. A `CHECK`
  constraint binds from its own text (:3570-3641), and PG 17 refuses a
  subquery in one (0A000 `cannot use subquery in check constraint`).
- **Aggregates** in a `WHERE` or `ON` operand are 42803 with PG 17's
  message for that clause, found by the resolver `value()` is given.
- **`HAVING`** (stage P3): `having_conjunct` (:5708-5745) binds through the
  same path with a resolver that maps a grouped column to the group's key
  and an aggregate to the group's folded value (the call the select list
  folds when written alike), and a column neither grouped nor dependent to
  PG 17's 42803. Its `AND`, `OR`, `NOT`, `IS NULL`, `BETWEEN`, `IN` and
  constants follow (:5709's refusal goes).

#### Guarantees stage P adds to CONTRACT.md

`CONTRACT.md` states the layer's rule; how far it agrees with PG 17 is
this section's and *Differences*.

- **Widened** (P1a), the three-valued-logic row (`CONTRACT.md:127`): "A
  `WHERE`, `ON`, `UPDATE`'s or `DELETE`'s `WHERE` (and in P3 a `HAVING`)
  of AND, OR and NOT over comparisons, IS [NOT] NULL, LIKE, `<>`, BETWEEN
  and IN of a list, **whose operands are any value expression a select
  list takes, or a BOOLEAN expression standing alone**, returns exactly
  the rows for which it is true in SQL's three-valued logic …". Tests: the
  binder, search and codegen cases below, the functional file
  `where_expressions.yaml`, and `proof: test_keyless_machine`, `proof:
  test_sql_machine`.
- **Added** (P1a), errors of a filter: "An expression in a WHERE, ON or
  HAVING is evaluated only for rows the statement reads, and the rows a
  statement reads depend on its plan. A row a top-level conjunct rejects
  is not tried against the conjuncts after it; inside AND, OR, NOT, CASE
  and COALESCE an operand is evaluated only while the result is
  unsettled, in written order. An error raised is the statement's, with
  the operator's SQLSTATE (22003, 22012, 22P02, 22011, 22023, 22025), and
  an UPDATE or DELETE raising one has no effect." P1a adds "Top-level
  conjuncts are tried in written order"; P1b replaces it with "Top-level
  conjuncts, after NOT is moved inward, ANDs are flattened and an IN
  list's constant items are grouped first, are tried in order of their
  count of operator and function calls, ties in written order." Tests:
  `proof: test_a_cheaper_conjunct_guards_a_division` (P1b), `embedded API:
  an expression filter raises only for rows it evaluates`, the keyless
  machine.
- **Widened**, the `$n::T` row (`CONTRACT.md:125`): a cast parameter as an
  operand in WHERE (and in P3 HAVING) is no longer refused; beside a
  column of another type it is compared as the select list compares it.
- **Added** (stage P2): "A column compared with an expression of parameters,
  constants and the clock is compared with its value computed once before
  any row is read; an error it raises is the statement's even when no row
  is read." Tests: `search: a row-independent operand bounds a key`,
  `embedded API: a row-independent operand is computed once`.
- **Widened** (stage P3), the aggregate row's HAVING clause: a HAVING of
  expressions over grouped columns and aggregates under AND, OR and NOT,
  its conjuncts of no aggregate evaluated on each input row.

#### Tests of stage P (front end)

- `tests/binder_test.cpp`: each form above binds to a `Filter` conjunct of
  the expected tree and types; `$n` typed by an expression (`lower(s) =
  $1` text, `a + 1 IN ($1, $2)` integers, `c > $1 - 100` integer, `$1 =
  $2` text, text, `WHERE $1` boolean); `a = $1 - 1 OR b = 2` binds the
  inner comparison through `value()`; `d > $1::int` an evaluated compare;
  the refusals of the table above with their SQLSTATEs, messages per
  clause and positions; P1b: an `IN` list's regrouping and mark. Cases
  that assert today's refusals change: :198 (`id = $1 + qty` now binds),
  :1299 (the BIGINT `$n - k` now binds), :1355 (`ON x.a + 1 = y.b` now
  binds); `tests/api_test.cpp:2963` (`WHERE UPPER(v) = 'ABC'`) becomes a
  result.
- `tests/functional/cases/postgresql/where_expressions.yaml`, new, every
  case run on PG 17 first: the rows of the table in *What a WHERE takes
  today*; the three-valued-logic list above; `UPDATE` and `DELETE` with an
  expression `WHERE`, and one that raises 22012 leaving the table as it
  was; the type errors (42883, 42804 per clause, 42803 per clause, 42725,
  22P02); the outer-join and `FULL JOIN` cases of `planner_design.md`,
  *Expression filters*; the constant-fold cases of *Constants and CASE
  arms* (22012 for a folded arm, 42601 for a dropped one, marked as a
  difference); P1b: `1/a > 0 AND a <> 0` (rows) against `(1/a) IS NOT NULL
  AND a <> 0` and `1/a > 0 OR a = 0` (22012), `NOT (1/a > 0 OR a = 0)`
  (rows), the `BETWEEN` and `IN` cases of *Evaluation order*. Features
  E061-01, E061-02, E061-03, E061-04, E061-05, E061-06, F041-04, and
  E051-06 for `HAVING`. The header of `string_functions.yaml` (:1-3),
  which says column cases are in `strings.yaml` marked unimplemented (none
  is), is corrected and its column cases added.
- `tests/functional/not_implemented/sqltest.txt`: no sqltest id is marked
  for this gap today (every remaining id is a `not PostgreSQL:` line), so
  no mark changes; the run must stay green.

#### Stages of stage P, and their sizes

| Stage | Builds | Size (code + tests + docs) |
|---|---|---|
| P1a | expression operands of comparisons, `IS NULL`, `LIKE`, `IN` lists, `BETWEEN` and BOOLEAN conjuncts in `WHERE`, `ON`, `UPDATE`/`DELETE` `WHERE`; `$n` typed by an expression; `$n - k` beside any integer column; `$n::T` of another type, evaluated; written conjunct order; `eval` gains `LIKE`, `Truth`, `InSet`; the key shape checks (`is_computed`, typed `use_of`), `reads_none` of any conjunct, and every conjunct-test site printing per conjunct | ~1,150 lines: parser 60, binder 280, rewrite 15, search 100, physical 70, codegen 140, tests 380, docs 105 |
| P1b | PG 17's order: `NOT` moved inward and `AND`s flattened (`plan/rewrite.cppm`), the `IN` list's regrouping and mark, `filter_cost` and the stable sort in every filter | ~450 lines: binder 40, rewrite 60, search 90, tests 200, docs 60 |
| P2 | an operand of no column computed once before the plan, as `$n ± k` is (`physical::Computed`, generalised), and a key's bound; a conjunct of no column a test made once | ~300 lines |
| P3 | `HAVING` over expressions of grouped columns and aggregates, under `AND`, `OR`, `NOT`; its aggregate-free conjuncts moved to the `WHERE`; with open question 4, the same expressions in an aggregated select list | ~450 lines |

`jsonb_design.md` budgeted about 1,200 lines; stage P is about 2,350. Each
row is one PR; P1a alone unblocks jsonb stage 3's `->>` in a `WHERE`.

#### Differences from PG 17 left after stage P

- **Written order before P1b.** Until P1b, `1/a > 0 AND a <> 0` is 22012
  where PG 17 returns rows.
- **Conjunct order at the edges** (P1b). The cost is computed on the bound
  tree, so where the layer writes an expression with another node count
  than PG 17's (an implicit widening PG calls `int48` for, a relabel PG
  makes a call), a tie can break the other way. PG 17 also factors common
  terms out of an `OR` (`canonicalize_qual`), which the layer does not.
  The subquery costs are PG 17's rank, not its estimate: a hashed
  SubPlan's cost grows with its estimate in PG 17. Each can only move
  which of two conjuncts raises first.
- **The generic plan.** The cost counts a parameter expression as PG 17's
  custom plan does. Under `plan_cache_mode = force_generic_plan`, or after
  PG 17 switches a prepared statement to its generic plan, PG 17 counts it
  (`a = $1 - 1` costs 2): `1/a > 0 AND a = $1 - 1` is then 22012 there and
  runs in the layer. P1 before P2 is the same split: `a = 1/$1` with `$1` =
  0 on an empty table raises nothing in P1 (PG 17's generic plan) and
  22012 in P2 (its custom plan).
- **A key PG 17 does not use.** A conjunct that would come before a
  conjunct the layer's key takes is not tried on rows the key skips,
  where PG 17's sequential scan tries it and raises: `(1/a) IS NOT NULL
  AND b = 2` with a key on `b`, or, in P2, a conjunct of cost 1 beside a
  clock key `ts < now()` (cost 2). PG 17 with an index on `b` does the
  same as the layer. The same holds of a table a join probes: a filter
  on it runs only on the rows a probe reaches.
- **One-table conjuncts of a join.** PG 17 evaluates a conjunct that reads
  one table of a join on every row of that table's scan: an `ON` conjunct
  of any join (`t LEFT JOIN v ON t.a = v.x AND 10 / v.y > 0` is 22012
  from a `v` row no `t` row joins, under a hash, merge or nested-loop
  join, and even when `t` is empty), a `WHERE` conjunct of an inner join
  (`t JOIN v ON t.a = v.x WHERE 10 / v.y > 0`), and a strict `WHERE`
  above a left join, which PG 17 reduces to an inner join (`t LEFT JOIN v
  ON t.a = v.x WHERE 10 / v.y > 0` is a plain `Hash Join` and 22012). The
  layer evaluates it only on the rows its loop reaches, and keeps a
  strict `WHERE` above a left join (`push_down`, `plan/rewrite.cppm:347-400`),
  so it can return rows where PG 17 raises. The rows, when PG 17 returns
  any, are the same. Open question 8.
- **A NULL parameter of a strict operator.** PG 17's custom plan folds
  a NULL `$n` as it folds a NULL constant; the layer binds no parameter's
  value, so `10/a > $1 OR a = 0` with `$1` NULL evaluates `10/a` and is
  22012 where PG 17 returns the `a = 0` row. A NULL constant is folded
  (*What stage P1a built*). PG 17's generic plan is the layer's.
- **CASE and COALESCE arms PG 17 drops.** `CASE WHEN false THEN 1/0 > 0
  ELSE true END` and `COALESCE(1, 1/0)` run in PG 17 and stay 42601
  (*Constants and CASE arms*).
- **An `IN` list's left operand** is evaluated once per item it is
  compared with, where PG 17's `= ANY` evaluates it once. The rows and
  the first error are the same; only the work differs.
- **No expression is a key.** `lower(name) = $1` reads the whole table;
  PG 17 uses an expression index on `lower(name)` where there is one. The
  layer has no expression indexes. Results agree.
- `random()` in a filter, `abs($1)` of an untyped parameter, a float in an
  expression, `expr IN (SELECT …)`, `GREATEST`, `LEAST`, `IS DISTINCT
  FROM`, `= ANY(array)`: refused (above).

#### Owner's decisions on stage P

The owner approved every recommendation below on 2026-11-22. Question 8's
outer-join reduction is tracked as a planner improvement of its own.

1. **The order of conjuncts.** (a) PG 17's order (P1b) in the same PR as
   P1a; (b) P1a with written order, P1b as the next PR (recommended: P1a
   is correct on its own, and P1b's rules — the normalisation, the `IN`
   regrouping, a cost table that tracks PG 17 — are reviewed alone); (c)
   written order only, simplest to explain, but `1/a > 0 AND a <> 0` stays
   22012 where PG 17 returns rows; (d) today's order (key's leftovers,
   then predicates), which matches neither.
2. **A BOOLEAN expression standing alone as a conjunct** (`WHERE flag`,
   `WHERE CASE … END`) in P1a. Recommended yes: the parser change is the
   `Value` kind, the rest is the same path, and `WHERE flag` is common.
3. **`random()` in a filter.** Recommended refused (42601), as `ORDER BY
   random()` is: a replay cursor (`cursor_design.md`) recomputes its rows
   and would read others. The select list's `random()` in a cursor already
   replays different values; that is a separate issue to open.
4. **P3 and the aggregated select list.** `SELECT sum(v) * 2 … GROUP BY k`
   is 42601 today (`an expression in the select list of an aggregated query
   is not supported yet`); the leaf that evaluates an aggregate's folded
   value in a `HAVING` is the leaf that select list needs. Recommended: in
   P3, one more case in the Emit of a group.
5. **SQLSTATE of the refusals stage P adds** (`random()` in a filter).
   Recommended 42601 `… is not supported yet`, as the `WHERE` refusals it
   sits among are; `jsonb_design.md`'s 0A000 is for jsonb's own refusals.
6. **Expressions in `SET`** (Mattermost's `SET … = GREATEST(…)`, `SET a = a
   * 2`, `SET name = lower(name)`). Not a filter, and not stage P: `set()`
   (`codegen.cppm:3036-3075`) and `set_list` (`physical.cppm:731-741`) take
   `eval` and `value()` in a small addendum of its own with `GREATEST` and
   `LEAST`. Recommended: a follow-up issue, built after P1a.
7. **An expression as a join key** (`ON lower(x.s) = lower(y.s)` is a
   nested loop of every pair). Recommended: a follow-up issue for a hash
   join on expressions and an inner probe by an outer expression, after P2.
8. **One-table conjuncts of a join.** Matching PG 17 needs outer-join
   reduction in `push_down` and a full scan of a joined table with a
   raising filter, which changes the plans of today's joins. Recommended:
   not in stage P; the difference stays listed, and the keyless machine
   draws raising operators only where it does not apply.
9. **`expr IN (SELECT …)`.** Typing the set by an expression needs
   `any_of` and `InSet` over an expression's type (a synthetic column, as
   `like()` builds, and `eval`'s `InSet` by the expression's physical
   type). Recommended: a follow-up issue after P1a.

#### Risks of stage P

- **Errors where PG 17 has none, or the reverse.** Bounded by P1b's
  order and checked by the keyless proof machine, whose tables have no
  key so both sides scan, on single-table `WHERE`s; what is left is
  listed above.
- **Shape checks that trust a conjunct's form.** Today's key and `truth`
  paths read a conjunct by its shape; stage P makes new shapes that pass
  those checks (`planner_design.md`, B1-B3). Each check becomes one
  predicate shared by the search, the checker and codegen.
- **Full scans an application did not expect.** `lower(col) = $1` cannot
  use a key. Mattermost's expression-index queries will scan; the work
  budget bounds each one.
- **Plans of today's statements change order.** P1a's written order and
  P1b's cost sort can move a predicate ahead of a comparison in a printed
  plan; codegen tests that compare plan text change with it, and no
  result does.
- **Generated code size.** Each expression conjunct prints its locals;
  compile time grows with the expression, as the select list's does.

#### What stage P1a built, and where it differs from the plan above

P1a is built as the *Stages of stage P* table's first row, with these
points settled in the code:

- **Parser:** a node `predicate()` does not take is `ast::Compare::Kind::
  Value`, its expression in `lhs` and the 42804's argument name in
  `arg_of` (the clause, `JOIN/ON`, or the `AND`, `OR` or `NOT` around it);
  a string constant and a scalar subquery standing alone are `Value`s
  (`WHERE 'true'`, `WHERE (SELECT flag …)`). `GREATEST` and `LEAST` are
  refused with their position.
- **The clock:** only `CURRENT_DATE` compared with other than a column
  stays refused (42601); `now()`, `LOCALTIME` and the rest are values as in
  a select list. The old guard's `clock_words` named every time function
  `CURRENT_DATE, …`, so the line above that cites it meant this.
- **Untyped operands:** a typed constant types a `$n` beside it (`1 = $1`
  integer, `$1 + 1` in a select list), two untyped operands of a comparison
  are text (`$1 = 'x'`), of arithmetic 42725; `$n IS NULL` is PostgreSQL's
  42P18 `could not determine data type of parameter $1`.
- **Typing a conjunct:** `physical::column_compare` (a column against a
  parameter of its type, a wider integer's, `$n ± k` of an INTEGER, or a
  column), `physical::is_computed` and `physical::evaluated` are the one
  test the search's `use_of`, `add_computed`, the checker and codegen ask.
- **Written order:** the search records each conjunct's place
  (`Rel::written`) and returns the residual in it, and codegen keeps it:
  a conjunct testing a correlated subquery runs it at its written place,
  for a row the conjuncts before it kept (before P1a it came after the
  others of its Filter). PG 17 runs `EXISTS` as a semi- or anti-join, so
  whether `EXISTS (…) AND 10 / a > 0` raises there depends on its plan.
- **A strict operator of a NULL constant** is a NULL of its type
  (`Binder::strict_null`), as PG 17's `eval_const_expressions` folds it,
  in a select list as in a filter: `10/a > NULL` divides nothing and
  `(10/a + NULL) IS NULL` is true of every row. The operands are bound
  first, so their type errors stand. Arithmetic, comparisons, `NOT`,
  `LIKE`, `||`, `SUBSTRING`, the string functions, `EXTRACT`, `abs`, `%`
  and the jsonb casts are strict; `AND`, `OR`, `IS NULL`, `CASE`,
  `COALESCE` and `random()` are not.
- **A CHAR(n) column beside a TEXT constant** (`ch = 'ab ' || ''`,
  `ch = lower('AB ')`) is compared as the select list compares it, the
  column cast to text, so trailing spaces count, as PG 17 resolves
  `bpchar = text`. A string literal, and a constant cast to VARCHAR, is
  still folded to the column's type (PG's `bpchar = bpchar`).
- **Positions:** the 42804 of a non-BOOLEAN conjunct, or operand of
  `AND`, `OR` or `NOT`, is at the expression's leftmost token, as PG 17's
  `exprLocation` (`WHERE a + 1` at `a`).
- **Not built in P1a:** P1b's order, P2's computed operand, P3's `HAVING`,
  expressions in `SET` (#896), join keys on expressions (#897) and `expr IN
  (SELECT …)` (#898). PostgreSQL's HINT on a 42883 (`No operator matches …`)
  is not given, as for a select list's.

#### Edits to other documents once stage P is built

`frontend_design.md`'s *Where expressions may appear* (:461-463) and
**WHERE** rules (:470-534), its not-yet list (:1189, :1216-1225, the
string function, expression, `HAVING` items); `planner_design.md`'s *What
the search refuses* (:1686-1715); `README.md:63` (the "expression in … a
WHERE" and "string function in a WHERE" gaps); `jsonb_design.md`,
*Expressions in WHERE and ON*, pointing here.

### Expressions in casts, GROUP BY, DISTINCT, SET and MIN/MAX of text (#904)

Status: **approved** by the owner on 2026-11-22, with every recommendation
in *Open questions on #904*; tracking issue #904. Nothing here is built yet. It covers the six gaps #904 lists,
found while building jsonb stage 3: a cast of an expression, `GROUP BY`
of an expression, `SELECT DISTINCT` and `COUNT(DISTINCT …)` of an
expression, an expression in `UPDATE … SET` (#896), `MIN`/`MAX` of text,
and `ARRAY[ARRAY[…]]` with `::text[]`. This section is the dialect: what
each form means, its types, errors, names and refusals. How the planner
and codegen run them is `planner_design.md`, *Expressions in casts, group
keys, DISTINCT, SET and text MIN/MAX (#904)*. Every PG 17 claim was run on
PostgreSQL 17.12, database collation `C.UTF-8`, with `\set VERBOSITY
verbose`, and its tables dropped after. Code citations are to
`gustavoamigo/sql-jsonb-accessors` at bf22be05.

#### The gaps #904 lists, and what PG 17 returns

| Statement | Layer today | PG 17 |
|---|---|---|
| `SELECT (props->>'k')::int`, `WHERE (props->>'n')::int > 5`, `ORDER BY s::int` | 42601 `a cast of other than a constant or a parameter is not supported yet` (`bound_value`, `sql/binder.cppm:3365`) | values; `(j->>'t')::int` of `"ab"` is 22P02 `invalid input syntax for type integer: "ab"`, no position |
| `SELECT $1::text::int` | 42601, as above | Describe: parameter `text`, result `integer`; `'zz'` is 22P02 at execution |
| `SELECT … GROUP BY props->>'k'`, `GROUP BY lower(b)`, `GROUP BY 1` | 42601 `GROUP BY takes columns` (`sql/parser.cppm:1955-1960`) | groups |
| `SELECT DISTINCT props->>'k'`, `DISTINCT lower(b)` | 42601 `SELECT DISTINCT of other than columns …` (`binder.cppm:5643-5646`) | rows |
| `count(DISTINCT props->>'k')`, `sum(DISTINCT a + 1)` | 0A000 `an aggregate of DISTINCT values of an expression` (`binder.cppm:4506`) | values |
| `UPDATE t SET s = j->>'k'`, `SET n = (j->>'k')::int`, `SET t = upper(t)`, `SET n = n * 2`, `SET t = CASE … END` | 42601, each with its operator's message (`Binder::value(Table, …, target)`, `binder.cppm:4598-4687`) | rows written |
| `SELECT max(t)`, `min(v)`, `max(ch)`, `max(j->>'k')` | 42601 `MAX of a text column is not supported` (`aggregate_support`, `binder.cppm:4442-4445`) | values |
| `j #> ARRAY[ARRAY['a'],ARRAY['b']]`, `j #> '{a,b}'::text[]` | 42601 | `1`, both |

Each is refused for the layer's general limits on expressions, not for
jsonb, and PG 17 runs each. The sections below take them one at a time;
*Stages of #904* orders them as PRs.

#### Casts of an expression

**What changes.** A cast `x::T`, `CAST(x AS T)` or `T 'lit'` of any value
expression `x` the select list takes is a value computed per row, wherever
a value is taken: the select list, a `WHERE`, `ON` and (P3) `HAVING`
operand, `ORDER BY`, an aggregate's argument, `RETURNING`, and with the
later stages a `GROUP BY` key and a `SET` value. A procedure's expressions
are bound by the same `bound_value` and generated by the same `Gen::eval`
(`codegen.cppm:3735`), so C1 lifts casts there too (`x := (a || b)::int`),
under the added row below and a procedure test; it adds no separate code
path. A cast of a constant stays
folded at analysis (*Literals*, above), and `$n::T` still types `$n` as
`T`. The two now share one rule for which casts exist, so a constant and
a column of the same type cast alike or are refused alike.

**The cast table.** One function, `cast_kind(from, to)`, classifies each
pair of the layer's types, as `pg_cast` does:

- **relabel** (no work): text and varchar to text or to varchar without a
  length; name to text or varchar; a type to itself.
- **function cast**: integer to integer (narrowing range-checked), integer
  to float, integer or DECIMAL to `DECIMAL(p, s)`, DECIMAL to integer,
  float to integer, float to `DECIMAL(p, s)`, DECIMAL to float, real to
  double precision and back, `integer` to `boolean` and back (int4 only:
  `smallint`, `bigint` and DECIMAL with `boolean` are none, as PG 17's
  `5::smallint::bool`, `true::bigint` and `1.5::numeric::bool` are
  42846), date to timestamp(tz), timestamp(tz) to date or time, timestamp
  to timestamptz and back (PG's `timestamp_timestamptz`, cost 1; the
  session is UTC, so the value is unchanged), time to timetz, timetz to
  time, text or varchar to name (cut to 63 bytes, `length(repeat('x',70)
  ::name)` is 63), `CHAR(n)` to text, varchar or name (PG's `rtrim1`:
  trailing spaces stripped, `'ab '::char(3)::text || '|'` is `ab|`, never
  the padded output), and jsonb's casts out (stage 3's `JsonbCast`, which
  this subsumes).
- **I/O cast**: any other type to text or name by its output function,
  and text, varchar or name to any type by its input function, as PG's
  `CoerceViaIO`; text to jsonb by `jsonb_in`.
- **none**: every other pair, 42846 `cannot cast type date to integer`,
  at the cast's position, at analysis, whatever the rows (PG 17:
  `current_date::int`, `('{"a":1}'::jsonb)::text[]`, `5::text[]`).

A class is what the cast does, not what it costs: a function cast that
leaves the value unchanged (timestamp to timestamptz, CHAR(n) to text of
a value held without trailing spaces) still costs 1, as PG's `pg_cast`
says (stage P1b, *Cost*). The constant fold (`cast_typed`,
`cast_temporal`, `binder.cppm:1440-1500`) asks the same function, so its
inline pair list goes. `cast_kind` lives in `sql/text.cppm`
(`bytecask.sql.text`), beside `from_text`, which the binder, the checker
(`plan/physical.cppm`) and codegen already import.

**Results and errors, per pair** (PG 17, each run on 5499):

| Cast | Result | Error, raised for the row that reaches it |
|---|---|---|
| text to `smallint`/`integer`/`bigint` | `' 12 '` is 12, `'0x1F'` 31, `'1_000'` 1000 | 22P02 `invalid input syntax for type integer: "ab"` (`''`, `'1.5'` too); 22003 `value "3000000000" is out of range for type integer` |
| text to `real`/`double precision` | the input function's value; `real` read as `float4in` reads it (`strtof`, rounded once), not as a double narrowed | 22P02 `invalid input syntax for type double precision: "x"` (`real` for real); 22003 `"1e400" is out of range for type double precision`, `"1e39" is out of range for type real` (and `'1e-50'::real`); hexadecimal floats (`'0x1F'::float8` is 31 in PG 17) are 22P02, a difference |
| text to `boolean` | `'yes'` true | 22P02 `invalid input syntax for type boolean: "maybe"` |
| text to `DECIMAL(p, s)` | rounded half away from zero to `s` | 22P02 `… type numeric: "x"`; 22003 `numeric field overflow`, DETAIL `A field with precision 2, scale 1 must round to an absolute value less than 10^1.`; `'Infinity'` 22003 `numeric field overflow`, DETAIL `A field with precision 3, scale 0 cannot hold an infinite value.`; `'NaN'` is NaN in PG 17, an error in the layer (below) |
| text to `date`, `time`, `timestamp`, `timestamptz` | the input function's value | 22007 `invalid input syntax for type date: "x"`; 22008 `date/time field value out of range: "2024-02-30"` |
| text to `bytea`, `jsonb` | the input function's value | 22P02 with the input function's DETAIL |
| integer to narrower integer | itself | 22003 `integer out of range` / `smallint out of range` (`3000000000::bigint::int`, `70000::int::smallint`), never a wrapped value |
| date to timestamp(tz); timestamp(tz) to date or time; time to timetz and back | PG's value (midnight; the date or time part; UTC offset `+00`, dropped) | 22008 `date out of range for timestamp` (`'5874897-12-31'::date::timestamp`) |
| real to double precision | exact | none |
| text or varchar to name; CHAR(n) to text, varchar or name | cut to 63 bytes; trailing spaces stripped | none |
| DECIMAL to integer | rounded half away from zero: 2.50 is 3, -2.5 is -3, `(d*3)::int` of 3.50 is 11 | 22003 `integer out of range` |
| float to integer | `rint`, ties to even: 2.5 is 2, 3.5 is 4 | 22003 `integer out of range` (NaN, infinity) |
| float to `DECIMAL(p, s)` | through 15 significant digits (6 for `real`): `2.5::numeric(3,0)` is 3 | 22003 as above; an infinity 22003 `numeric field overflow`, DETAIL `A field with precision 3, scale 0 cannot hold an infinite value.`; NaN is `NaN` in PG 17 (`'NaN'::float8::numeric(3,0)`), which a DECIMAL cannot hold: the layer raises 22003 `numeric field overflow`, DETAIL `A field with precision 3, scale 0 cannot hold NaN.`, a difference |
| DECIMAL to float | the nearest double of its decimal text (`strtod`) | none |
| float8 to real | nearest | 22003 `value out of range: overflow` / `underflow` |
| integer, DECIMAL, boolean, date and time types, bytea, jsonb to text | the output function: `2.50`, `true`, `2024-03-01`, `\x6162`, `{"a": 1}` | none |
| `integer` to `boolean`, `boolean` to `integer` (int4 only) | non-zero true; 1 or 0 | none |
| jsonb to a scalar type | stage 3's rules (`jsonb_design.md`, *Casts to and from jsonb and json*) | 22023 `cannot cast jsonb string to type integer`, 22003 |

A NULL operand is NULL, and the cast does not run (it is strict, and
`strict_null` folds `NULL::text::int` to a NULL integer as it folds the
jsonb casts). A runtime error carries **no position**, as PG 17's does
(`execute p1('zz')` with `$1::text::int` is 22P02 with no `LINE`); a
42846, a 0A000 or 42601 refusal, and an error of a folded constant are
positioned at analysis.

**Which rows raise.** A cast in a filter raises only for rows the
statement evaluates it on, under stage P's rule (*Evaluation order, and
errors in rows a filter would reject*): `WHERE id = 2 AND s::int = 7` over
a table holding `'x'` in another row returns the row in PG 17 and in the
layer, since the key bound reads one row; `WHERE s::int = 7` alone raises
22P02 in both.

**Integer input, constants included.** `integer_from` (`sql/text.cppm:519`)
reads digits only. PG 17's `pg_strtoint*` also reads `0x`, `0o`, `0b` and
`_` between digits, and its range error names the value. The cast's
helper and the constant fold share the fixed `integer_from`, so
`'0x1F'::int` is 31 at analysis as per row (today 22P02), and
`'3000000000'::int` says `value "3000000000" is out of range for type
integer` (today `integer out of range`; same SQLSTATE). `integer_from` is
also how `from_text` reads a parameter's wire text (`server/pg_text.cppm:38`,
`pg_session.cppm:1995`) and a typed literal (`binder.cppm:1185`, :2340),
so a text-format parameter `'0x1F'` bound to an integer is 31 too: the
fix widens what parameters accept, not only constants. Two more input
fixes ride with it, for the same reason: `float_from` reads a `real` as
`float4in` does (`strtof`, and `"1e39" is out of range for type real`
where today it says `value out of range: overflow`), and `decimal_from`
reads `NaN` and the infinities, so a DECIMAL(p, s) of `'Infinity'` is
PG's 22003 with its DETAIL rather than 22P02 (NaN: the difference above).
Hexadecimal floats stay 22P02 (*Differences*).

**What stays refused.**

| Cast | SQLSTATE | Why |
|---|---|---|
| to or from `NUMERIC` of no precision, other than jsonb to `numeric` | 0A000, as today (`refuse_numeric`) | open question 2 |
| of a value to `VARCHAR(n)` or `CHAR(n)` | 42601, as a constant's is today | PG truncates and pads; open question 3 |
| of a REAL or DOUBLE PRECISION to text | 42601, as a constant's is today (`cast_typed`, :1460) | the layer's float text is not `float8out`'s; open question 4 |
| to `text[]`, `varchar[]` or any array | as today (*ARRAY[ARRAY[…]] and casts to text[]*) | not a type the layer has |
| of a value to a domain or enum | as today | no such types |

**Naming.** A cast's column is named as PG 17's `FigureColname`: the
operand's name when that name is strong (a column, a function other than
an operator, `coalesce`, `nullif`, an aggregate, a scalar subquery's column by its name: `(SELECT 1 AS
x)::text` is `x`, and `(SELECT 1)::text`, whose column is `?column?`, is
`?column?`, not `text`), else the target type's name (`int4`, `text`, `float8`, `varchar`, `bool`,
`numeric`), and a cast of a cast whose inner name is weak takes the outer
type's. PG 17: `a::text` is `a`, `upper(s)::text` is `upper`,
`coalesce(a,0)::text` is `coalesce`, `(a+1)::text` is `text`,
`(a*2)::bigint` is `int8`, `(j->>'k')::int` is `int4`, `(CASE …)::int` is
`int4`, `$1::text::int` is `int4`. `plain_output`'s cast branch
(`binder.cppm:6200-6230`) becomes that one rule, which also fixes
`(s || 'x')::text`, named `?column?` today where PG 17 says `text`.

**Describe.** A cast's result is its target type, with a `DECIMAL(p, s)`'s
typmod as constant casts report it today. `$1::text::int` describes `$1`
as text (25) and the result as `int4`, as PG 17's
`pg_prepared_statements` shows (`{text}`, `{integer}`).

**Keys.** A cast of a column, or of a parameter (`id = $1::text::int`), is
an operand, never a key's bound (`planner_design.md`, *The search: a
filter, never a key's bound*): the rows are right and the plan scans.
Computing a parameter-only cast once, so it bounds a key, is stage P2's
(open question 6).

#### GROUP BY of an expression

**What a GROUP BY item may be** (PG 17's `findTargetlistEntrySQL92`, each
case run):

- **Any value expression** of the `FROM`'s columns that the select list
  takes: `GROUP BY props->>'k'`, `lower(b)`, `a + 1`, `coalesce(a, 0)`,
  `(j->>'k')::int`, several of them, mixed with columns.
- **An integer constant** `k` is the select list's `k`-th item: `GROUP BY
  1`, and `GROUP BY (1)`. Past the list, 0 or negative: 42P10 `GROUP BY
  position 3 is not in select list` (`GROUP BY -1` says `position -1`), at
  the constant. An item that holds an aggregate: 42803 `aggregate
  functions are not allowed in GROUP BY`, at the aggregate. A non-integer
  constant (`GROUP BY 'x'`, `GROUP BY 1.5`): 42601 `non-integer constant in
  GROUP BY`. Only a bare integer literal is a position: `GROUP BY 1+1` and
  `GROUP BY 1::int` are constant expressions, so `SELECT a … GROUP BY
  1::int` is 42803 at `a`. With `*` in the select list a position is
  refused as `ORDER BY`'s is (`parser.cppm:1536-1537`).
- **A key that is the same for every row** (a constant expression,
  `GROUP BY $1`, `GROUP BY 'x'::text`, or a position naming a constant
  item, `SELECT 1, count(*) … GROUP BY 1`) still groups: one group over
  rows, **none over no rows**. PG 17, over an empty table: `SELECT
  count(*) … GROUP BY 1+1`, `… GROUP BY $1` and `SELECT DISTINCT 1` return
  no rows, while `SELECT count(*)` with no `GROUP BY` returns one row, 0.
  Such a key is never dropped, so the query never becomes a keyless
  aggregate (`planner_design.md`, *Group keys and DISTINCT values of an
  expression*).
- **A bare name** is an input column first: `SELECT a AS id … GROUP BY
  id` groups by the table's `id` (PG 17 returns one group per row). Only a
  name no `FROM` column has is an output name: `GROUP BY k` with `props->>'k'
  AS k`. Two items of that name that differ: 42702 `GROUP BY "x" is
  ambiguous`; alike (`a AS x, a AS x`), one key. **This is the opposite
  of `ORDER BY`'s rule**, so `GROUP BY` does not use `resolve_output_names`;
  the binder resolves it against the catalog.
- **An aggregate** in an item: 42803 `aggregate functions are not allowed
  in GROUP BY`, at it.
- Duplicate keys (`GROUP BY a, a+1, (a)` keeps `a` and `a+1`) are kept once
  each, as PG's `addTargetToGroupList` keeps them.

**Matching the select list, ORDER BY and HAVING to the keys.** PG 17
walks each select item, `HAVING` conjunct and `ORDER BY` key top-down: a
subtree `equal()` to a key is that key's value, an aggregate is its folded
value, a constant or parameter is itself, and any other column is 42803
`column "g904.b" must appear in the GROUP BY clause or be used in an
aggregate function`, at that column. PG 17, each run:

| Query | PG 17 |
|---|---|
| `SELECT lower(b), count(*) … GROUP BY lower(b)` | groups |
| `SELECT lower(b) \|\| 'z' … GROUP BY lower(b)`, `SELECT (a+1)*2 … GROUP BY a+1` | groups |
| `SELECT a+1 … GROUP BY a` | groups |
| `SELECT upper(b) … GROUP BY lower(b)`, `SELECT b … GROUP BY lower(b)` | 42803 at `b` |
| `SELECT a + 1 … GROUP BY 1+a` | 42803 at `a` (not `equal()`) |
| `SELECT a+1 AS b, count(*) FROM g904 GROUP BY b`, `g904` having a column `b` | 42803 at `a`: `GROUP BY b` is the input column (the rule above), so `a` is ungrouped |

PG 17 matches a constant by its typed value **and** its literal class:

| Select item, `GROUP BY a + 1` | PG 17 | Layer |
|---|---|---|
| `a + 01`, `a + (1)` | matches | matches |
| `a + 1.0` (numeric against integer), `a + 1.0` against `GROUP BY a + 1.00` (scale differs) | 42803 at `a` | 42803 |
| `a + '1'`, `a + 1::int` | matches (coerced to the same int4 constant) | 42803, a difference |

The binder matches on the AST, before binding, by extending
`ast::same_expr` (`ast.cppm:253`, already PG's `equal()` for DISTINCT's
42P10 and `resolve_output_names`): two column references are equal when
they resolve to the same column (`lower(t.b)` matches `lower(b)`); an
integer literal equals one of the same value (`01` and `1`); a numeric
literal one of the same digits **and scale** (`1.0` and `1.00` differ); a
string literal one of the same bytes; a literal never equals a literal of
another class or a cast. A cast already compares by the parser's
canonical type name (libpg_query's last name, `integer` and `int4` both
`int4`), so `t::integer` matches `GROUP BY t::int4`, as in PG 17. It cannot
match bound expressions, since every literal binds to its own hidden
parameter (`Binder::fixed`, `binder.cppm:2812-2819`), so `props->>'k'` bound
twice gives two trees that differ. Spellings PG's coercion makes equal and
the AST does not (`'k'` against `'k'::text`, `1` against `'1'` or
`1::int`) are 42803 where PG runs: listed in *Differences*.

**Columns a grouped primary key determines.** `Aggregate::dependent`
keeps working beside expression keys, for a primary key whose every
column is a **plain** key (a column, a position or output name naming a
column): PG 17 runs `SELECT id, b, lower(s) … GROUP BY 1, lower(s)` with
`id` the primary key. A key `id + 0` determines nothing (`SELECT id, b …
GROUP BY id + 0` is 42803 at `id`). An expression over a dependent column
(`SELECT lower(b) … GROUP BY id`, which PG 17 runs) is an expression over
the group's values, so it waits for P3 (42601), as below.

**What G1 takes, and what waits for P3.** A select item, `ORDER BY` key
or `HAVING` operand that **is** a key expression, written alike, is that
key's value. An expression **over** keys or aggregates (`lower(b) ||
'z'`, `count(*) * 2`) is owner decision 4 of stage P: the Emit of a group
evaluating an expression of its values, built with P3. Until P3 it stays
42601 `an expression in the select list of an aggregated query is not
supported yet`, but only after the binder has looked for an ungrouped
column outside matched keys and aggregates: `SELECT b … GROUP BY
lower(b)` is PG's 42803, never a 42601.

**Equality of keys.** Two rows are in one group when each key's values are
equal under the key's type, NULL equal to NULL: text bytewise (the layer's
one collation, `C`), a DOUBLE's `-0.0` equal to `0.0` and NaN to NaN (PG
17: `GROUP BY d*1` over `0.0`, `-0.0`, `NaN`, `NaN`, `1` gives three
groups), integers, booleans, dates and times by value, `DECIMAL(p, s)` at
its scale. The value a group shows is its first row's, as PG 17's hashed
aggregate shows it (`GROUP BY -d` over `0.0`, `-0.0` printed `-0`); text,
being bytewise-equal, shows exactly what every row of the group holds.

**What stays refused.**

| Key | SQLSTATE | Why |
|---|---|---|
| A jsonb expression (`GROUP BY props->'j'`) | 0A000 `GROUP BY of a jsonb value is not supported yet`, as a jsonb column's | jsonb equality (`1` = `1.0`) is `jsonb_design.md` stage 2 |
| A NUMERIC of no precision | 0A000, as a column's (`refuse_uncompared`) | no sort key for it |
| A CHAR(n)-typed expression (`coalesce(ch, ch)`) | 0A000 | open question 9 |
| `random()`, or anything holding it | 42601, as `ORDER BY random()` | a replay cursor reads the rows again |
| A subquery (`GROUP BY (SELECT 1)`) | 42601 `a subquery in GROUP BY is not supported yet` | PG 17 runs it; not needed |
| An expression key over a derived relation's columns | 42601 | its keys live in C locals (`planner_design.md`, *Derived relations*) |
| `GROUPING SETS`, `ROLLUP`, `CUBE`, `()`, `GROUP BY DISTINCT` | as today | not #904 |

**Plans.** A query grouped by an expression always holds its groups in a
structure: no index orders an expression, so its groups never stream, and
one with many groups fails with 54000 past its memory budget, as an
unordered `GROUP BY` of columns does today (`CONTRACT.md:311`).

#### SELECT DISTINCT and aggregates of DISTINCT values of an expression

**`SELECT DISTINCT`** of expressions is a `GROUP BY` of its select items,
as `SELECT DISTINCT` of columns is today (*Aggregates*, `SELECT DISTINCT`, above), with the key equality above: PG
17's `SELECT DISTINCT -d` over `0.0`, `-0.0` and `1` returns two rows, the
first `-0`. An `ORDER BY` under it must name a select item, by position,
output name or the same expression, else 42P10 `for SELECT DISTINCT, ORDER
BY expressions must appear in select list` (`binder.cppm:5627-5640`, as
today; PG 17: `SELECT DISTINCT lower(b) … ORDER BY b`). The refusals of
the table above hold for its items. `DISTINCT ON` stays as today.

**`COUNT`, `SUM` and `AVG` of `DISTINCT` values of an expression** fold
each distinct non-NULL value of the expression once, per group: PG 17,
`count(DISTINCT props->>'k')` is 2, `sum(DISTINCT a+1)` 9, `avg(DISTINCT
a*2)` 4.0000000000000000, `count(DISTINCT -d)` over `0.0`, `-0.0`, NaN,
NaN, 1 is 3. The expression is evaluated once per row; a row where it is
NULL is skipped; an error it raises is the statement's. `MIN` and `MAX`
of `DISTINCT` are those of all values, as today. The refusals of the
argument's type stay: an aggregate of jsonb (`count(DISTINCT props->'j')`
is 2 in PG 17, 0A000 here until stage 2) and of NUMERIC of no precision
are 0A000. The distinct values are held in the statement's memory; past
its budget, 54000.

#### Expressions in UPDATE … SET (#896)

**What a SET value may be.** Any value expression the select list takes,
over the updated table's columns, parameters and constants: `SET s =
j->>'k'`, `SET n = (j->>'k')::int`, `SET t = upper(t)`, `SET n = n * 2`,
`SET t = CASE WHEN n > 0 THEN 'p' ELSE 'q' END`, `SET n = coalesce(j->>'zz',
'0')::int`. Every value is computed from the row **before** the update:
PG 17's `SET n = n + 1, t = t || n` of `n = 5, t = 'x'` writes `6`, `x5`.
`RETURNING` sees the new row, as today. A `$n` inside is typed by its
operators, as in a select list; a bare `$n`, and `$n::T` where T is the
column's type, keep today's typing by the column. `$n::T` where T is not
the column's type is a cast like any other: `$n` is typed T and the
value is then assigned by the table below, per row, never typed by the
column. PG 17: `UPDATE p SET t = $1::int` describes `$1` as `{integer}`
and `execute u1('007')` writes `7`; `SET s = $1::int` (smallint) of 70000
is 22003 `smallint out of range` for the row; `SET n = $1::text` (n
integer) is 42804 at prepare. Typing it by the column would describe
`$1` as text and store `007`.

**The assignment.** The value is then converted to the column's type by
PG 17's assignment casts (`coerce_to_target_type`, assignment context):

| Value's type to column's | PG 17 | Layer after S1 |
|---|---|---|
| its own type; text, varchar, name among each other | itself | itself |
| an integer to a narrower integer | 22003 `smallint out of range` past it (`SET s = n + 100000`) | same, by the row builder (`catalog.cppm:1681`) |
| an integer to a float | its value | same |
| DECIMAL or float to an integer | DECIMAL half away from zero (2.50 is 3), float `rint` (2.5 is 2); 22003 | same, C1's helpers |
| an integer, DECIMAL or float to `DECIMAL(p, s)` | rounded to `s`; 22003 `numeric field overflow` with its DETAIL | same (float to DECIMAL as C1's cast) |
| any type to text or name | its output function: `SET t = bo` is `true`, `SET t = d` `2.50`, `SET t = j->'o'` `{"z": 1}`, `SET t = dt` `2024-03-01`, `SET t = by` `\x6162` | same, C1's output helpers; a float stays refused (open question 4) |
| to `VARCHAR(n)` or `CHAR(n)` | 22001 `value too long for type character varying(3)` past `n` characters, unless the excess is only spaces: cut to `n` (`SET v = t \|\| '    '` of `'x'` stores `'x  '` in a `varchar(3)`) | 22001 by the row builder (`catalog.cppm:1734-1750`); a CHAR(n) cuts the spaces as PG does and is stored without trailing spaces, as today; a VARCHAR(n) whose excess is only spaces is 22001, the existing *Known differences* entry |
| DECIMAL to a float; real to double precision | its value (DECIMAL as C1's cast) | same |
| double precision to real | nearest; 22003 `value out of range: overflow` / `underflow` | same, C1's `bc_f64_to_f32` |
| date to timestamp(tz); timestamp(tz) to date or time | C1's cast (`SET ts = dt` is midnight, `SET dt = ts` the date) | same, C1's helpers |
| to name | cut to 63 bytes | same, by the row builder |
| jsonb to jsonb | itself | same |
| text to an integer, boolean, date, jsonb or bytea; boolean to integer and back; integer to boolean; jsonb to anything but text | 42804 `column "n" is of type integer but expression is of type text`, HINT `You will need to rewrite or cast the expression.`, at the value's position | same (`assignment_mismatch`, `binder.cppm:520`) |
| to or from NUMERIC of no precision | runs | 0A000, as today |

The pairs follow `physical::assigns` and the binder's `assigns_to`
(INSERT … SELECT's rule, `binder.cppm:6178`), widened to this table and
renamed so their 0A000 messages name the statement, not `INSERT …
SELECT`. 22P02 never comes from an assignment: it needs an explicit cast
inside the value (`SET n = (j->>'txt')::int` is 22P02 `… "abcd"`).

**Errors and their order.** Each error is raised by the first row that
meets it, and a statement that raises writes nothing. Of several `SET`
items that would fail on one row, **PG 17 reports the one on the
lower-numbered column**, whatever the written order: its rewriter sorts
the target list by attribute number (`expand_targetlist`). PG 17: `SET n
= n/n, a = a + 1` and `SET a = a + 1, n = n/n` of `a = 32767, n = 0`, `a`
declared before `n`, both give 22003 `smallint out of range`, never 22012.
So the layer evaluates and stores the items in the column order of the
table, not as written (open question 10). PG 17 has two classes of
analysis-time error, and only one is in written order:

| Statement (`a smallint`, `n int`, `a` first) | PG 17 |
|---|---|
| `SET n = 'zz', a = 99999`, `SET t = 'a'::int::text, a = 99999` | 22P02 at `'zz'` (at `'a'`), with its `LINE`: written order |
| `SET n = 1/0, a = 99999`, `SET a = 99999, n = 1/0` | 22003 `smallint out of range`, no `LINE`: column order |

A literal's **input** error (22P02, 22007, 22008, raised while the parser
reads a string as its type) comes first, in written order, with its
position, as today (*Literals*). An error of **folding** (22012 of `1/0`,
22003 of a constant's arithmetic or of its assignment to the column) is
the planner's in PG and follows column order with no position, as a
runtime error does: the binder records it on its item rather than
raising, and the items are checked in column order, the first recorded
error raised. NOT NULL is checked when the row is complete, as today.

**What stays refused in SET.**

| Form | SQLSTATE | Why |
|---|---|---|
| A subquery in a `SET` value | 42601, as today | the plan's subqueries are not wired to writes; a follow-up |
| `EXCLUDED.c` inside an expression of `ON CONFLICT DO UPDATE SET` | 42601, as today (`conflict_update`, :8004) | open question 13 |
| `GREATEST`, `LEAST` | 42601, as everywhere today | open question 12 |
| An aggregate | 42803 `aggregate functions are not allowed in UPDATE`, PG 17's message | PG refuses it |
| `INSERT … VALUES` of a column expression | 42601 `INSERT values must be parameters or constants`, as today (`binder.cppm:8343-8346`) | `VALUES` has no row; the widened `value()` must keep this guard |

**`col + $n` and `col - $n` today.** The special branch (`binder.cppm:
4638-4660`, `literal()` at :4575-4592) and `Checker::set_list`'s Add/Sub
case are subsumed by the general path and go. What they guarantee stays:
22003 with the column type's message for a result outside it (`SET s = s
+ 1` at 32767 is `smallint out of range`, PG 17's int4 sum failing its
assignment), `bigint out of range` for a BIGINT, NULL for a NULL operand,
the range error raised as `BC_C_RANGE` **on the updated table**, and a
DECIMAL literal that would rescale past 18 digits refused at bind.

- **The object id.** The general `eval` raises a range error on object 0
  (`bc_add_bigint(…, 0)` and `bc_check_int(…, 0)`, `codegen.cppm:1833`,
  :1864); today's SET path names the table (:3190-3193), and the embedded
  API reports it. So while it generates a `SET` value, `eval` passes the
  table's id as the object id of every range error it raises (arithmetic,
  `bc_check_int`, the cast helpers), and `CONTRACT.md:137` keeps
  `BC_C_RANGE`.
- **The DECIMAL literal.** `literal()`'s 18-digit check moves with it:
  the binder applies it where the general path binds an integer or
  DECIMAL constant operand of a DECIMAL `+` or `-` at the sum's scale. It
  stays a bind-time refusal, so `CONTRACT.md:313` is unchanged, and its
  test stays.

#### MIN and MAX of text

`MIN` and `MAX` take a `TEXT`, `VARCHAR`, `NAME` or `CHAR(n)` argument, a
column or an expression (`max(j->>'k')`), per group or over all rows,
with `DISTINCT` (the same as without) and in `HAVING`. PG 17, each run:

- The result type is `text` (25) for text, varchar and name arguments
  (`pg_typeof(max(varchar_col))` is `text`: PG has no `max(varchar)`), and
  `character` (1042) **with no length** for `CHAR(n)`: a view over
  `max(ch)`, `ch char(4)`, has `atttypmod` -1 and `format_type` `bpchar`.
  Its value is still printed padded to the column's `n` (`max(ch)` of
  `'b'` is `b   `, `octet_length` 4), since PG's datum is the stored,
  padded one; it is compared without its trailing spaces (`max` over
  `'a'::char(3)`, `'a '::char(3)`, `'ab'` is `ab`). The layer holds a CHAR
  value without trailing spaces and pads it on output by the type's `n`;
  the call's type has no `n`, so the binder types it `SqlType{Char}` with
  no length (Describe: 1042, typmod -1, as `pg::type_info` gives an
  unsized CHAR) and records the argument column's `n` on the output as
  its display width, which the result writer pads to as it pads a CHAR(n)
  column. The value itself stays unpadded, so a `HAVING` comparison of it
  compares as a column's. Only a CHAR(n) **column** argument is taken; a
  CHAR-typed expression argument (`max(coalesce(ch, ch))`) is 0A000, for
  open question 9's reason.
- Values are compared as text is everywhere in the layer, bytewise, which
  is PG's order under `C` and `C.UTF-8`: `max` over `'a'` and `'B'` is
  `a`, over `'é'`, `'z'` and `'Z'` it is `é`. Under another database collation PG orders otherwise;
  *Known differences* already says so for `ORDER BY` and comparisons, and
  gains `MIN` and `MAX`.
- NULLs are skipped; over no rows, or only NULLs, the result is NULL.
- `MIN` and `MAX` of `bytea` and of jsonb stay 42883 `function max(bytea)
  does not exist`, as PG 17's; `SUM` and `AVG` of text stay 42883.

Each group holds at most one value per text `MIN` or `MAX`, as long as the
longest it has seen; it is held in the statement's memory, and past its
budget the statement fails with 54000.

#### ARRAY[ARRAY[…]] and casts to text[]

A `text[]` is a jsonb path only, never a value, column or result (owner's
decision Q2, `jsonb_design.md`, *Path arguments*). A general array type is
#889's tier-4 item and stays out of #904. PG 17, each run:

- `j #> '{{a},{b}}'`, `j #> ARRAY[ARRAY['a'],ARRAY['b']]` and `j #>
  ARRAY[['a'],['b']]` read the 2-D path flat: `1`.
- `j #> '{a,b}'::text[]` is `1`; `ARRAY[1]::text[]` coerces its elements.
- Sub-arrays of different lengths, and `ARRAY[NULL::text[], ARRAY['a']]`:
  2202E `multidimensional arrays must have array expressions with matching
  dimensions`. `ARRAY['a', ARRAY['b']]`: 22P02 `malformed array literal:
  "a"`. Seven levels: 54000 `number of array dimensions (7) exceeds the
  maximum allowed (6)`.
- `j #- '{{a}}'` is 2202E `wrong number of array subscripts`; `j -
  '{a}'::text[]` deletes key `a` while `j - '{a}'` deletes key `{a}`.
- `('{"a":1}'::jsonb)::text[]` and `5::text[]` are 42846; `j #>
  '{a}'::int[]` is 22P02 for the literal, `j #> '{1}'::int[]` 42883.

**Design: keep both refused**, with a message that says what is refused:
`ARRAY[ARRAY[…]]` in a path becomes 42601 `a multidimensional ARRAY is not
supported yet` (today's `an ARRAY other than as the path of #> or #>>` is
wrong, the outer `ARRAY` being that path). The SQLSTATE stays 42601, so
`CONTRACT.md:119` keeps its words. `::text[]` in a path position is needed
by jsonb stage 4's `- text[]` (`j - '{a}'::text[]` is the documented way
to pick that overload) and is designed with it, together with a path form
that carries its dimensions for stage 4's 2202E (open question 15).

#### Errors, NULLs and positions of #904

- Every new value is strict except where PG's is not: a cast, a `GROUP BY`
  key, a `SET` value and a `MIN`/`MAX` argument that is NULL raise nothing.
  A NULL key is a group of its own, as a NULL column key is.
- Errors raised per row (22P02, 22003, 22007, 22008, 22023, 22012, 22001)
  have no position, as PG 17's. Errors found at analysis (42846, 42803,
  42P10, 42702, 42601, 42804, 0A000) have the position PG 17 gives:
  42803 at the ungrouped column, 42P10 and 42601 at the `GROUP BY`
  constant, 42702 at the name, 42804 at the `SET` value, 42846 at the cast.
  A folded constant's error in `SET` (22012, 22003) is PG 17's planner's:
  column order, no position (*Errors and their order*).
- A statement that raises has no effect, an `UPDATE` included.
- Each error carries PG 17's SQLSTATE, message and DETAIL. A `TextError`
  raised in a plan keeps its SQLSTATE (`planner_design.md`, *Plan API
  calls for runtime casts*); none reports `BC_E_CORRUPT`.

#### Column names and Describe of #904

- A cast: *Casts of an expression*, **Naming** and **Describe**.
- A `GROUP BY` or `DISTINCT` expression item is named as in a select list
  of no grouping (`lower`, `?column?` for `props->>'k'`), typed as the
  expression (`props->>'k'` text, 25).
- `MIN` and `MAX` of text are `min` and `max`, of type `text` (25) or
  `character` (1042, typmod -1) as above.
- `UPDATE … RETURNING` names and types are as today.
- `$n` inside a `SET` value, a `GROUP BY` key or a cast is typed as in a
  select list, and Describe reports that type.

#### Guarantees #904 changes in CONTRACT.md

None is narrowed and none removed. Each is named in its stage's PR. Two
errors change but nothing that ran stops running: a folded constant's
error in `SET` moves from written order with a position to column order
without one (PG 17's), and `'Infinity'` or `'NaN'` to a DECIMAL(p, s)
moves from 22P02 to 22003.

- **Added** (C1), casts of expressions: "A cast of any value expression
  the select list takes is computed for each row it is evaluated for, by
  PostgreSQL 17's cast between the two types: a cast PostgreSQL does not
  have is 42846 at analysis; a NULL is NULL; an error is the input or
  conversion function's SQLSTATE, message and DETAIL (22P02, 22003, 22007,
  22008, 22023), with no position, and the statement's. A constant casts
  as a value of its type does." Tests: `binder: a cast of an expression
  binds by the cast table`, `embedded API: casts of expressions give
  PostgreSQL 17's values and errors`, `plan API: the cast helpers keep
  their input function's SQLSTATE`, `proof: test_sql_machine`,
  `tests/functional/cases/postgresql/casts.yaml`.
- **Widened** (C1), `CONTRACT.md:119`: the jsonb casts out are casts of
  any expression, as above; its `ARRAY[ARRAY[…]]` and text[] words stay.
- **Widened** (C0), `CONTRACT.md:111`: a string constant cast to an
  integer reads `0x`, `0o`, `0b` and `_` as PG 17, and its 22003 names the
  value; a `real` reads as `float4in` with its message; `'Infinity'` to a
  DECIMAL(p, s) is 22003 with PG's DETAIL.
- **Widened** (C0), the parameter rows (`CONTRACT.md:126` and the wire
  rows that read a text-format parameter by its type): a parameter's text
  bound to an integer reads `0x`, `0o`, `0b` and `_`, since the binder and
  the server read it through the same `from_text`.
- **Widened** (C1), the procedure rows (`CONTRACT.md:177`-178): a
  procedure's expressions take casts of expressions, as the added row says.
- **Widened** (C1 and S1), `CONTRACT.md:126`: `$n::T` as an operand of a
  cast (`$1::text::int`), and as an operand in `SET`; and (S1) `$n::T`
  assigned to a column of another type, today 42601, types `$n` as T and
  assigns it by the assignment table (42804 where PG has no assignment).
- **Widened** (C1, S1, G0, G1), `CONTRACT.md:144`: its refused list loses casts
  of expressions, `GROUP BY` and `SET`.
- **Widened** (S1), `CONTRACT.md:137` becomes the `SET` row: "`UPDATE …
  SET col = e` of any value expression the select list takes computes
  every `e` from the row before the update and assigns it with
  PostgreSQL 17's assignment cast: no assignment cast is 42804; a value
  outside the column's type is 22003 or 22001; of several items failing
  on one row, the error is the lower-numbered column's, a literal's input
  error excepted (written order, with its position); a value outside an
  integer or DECIMAL column is `BC_C_RANGE` on the table; a NULL operand
  gives NULL; the statement writes nothing when one raises." Tests:
  `proof: test_column_plus_parameter_at_the_edges`, `proof:
  test_sql_machine`, `embedded API: SET of an expression reads the row
  before the update`, `tests/functional/cases/postgresql/dml.yaml`.
- **Widened** (S1), `CONTRACT.md:250`, INSERT … SELECT: it shares S1's
  assignment table (`assigns_to`, `physical::assigns`, one store), so its
  0A000 list loses the conversions through text, a DECIMAL of another
  scale, into or out of an integer or float, and a date or time into
  another date or time type; what stays 0A000 is NUMERIC of no precision
  and a float into text. This is #824's work; S1's PR closes #824 (open,
  *INSERT … SELECT — the assignment casts it does not take yet*), and
  `assigns_to`'s comment stops citing it.
- **Widened** (S1), `CONTRACT.md:252`, ON CONFLICT DO UPDATE: `conflict_update`
  binds its SET through the same `Binder::value` (`binder.cppm:7986`), so
  "a column, or column ± a parameter or integer" becomes any value
  expression over the holder's columns qualified by the table's name;
  `EXCLUDED.c` inside an expression stays 42601 (open question 13).
- **Widened** (G0 and G1), `CONTRACT.md:133`: `GROUP BY` of select-list
  positions and of output names (an input column first; G0), and of
  expressions (G1), with
  the key equality above; a select item, `ORDER BY` key or `HAVING` operand
  written as a key is its value; a key the same for every row still
  groups, no group over no rows. **Widened** (G1), `CONTRACT.md:131`:
  `SELECT DISTINCT` of expressions. **Widened** (G1), `CONTRACT.md:311`: a
  `GROUP BY` or `DISTINCT` of an expression is among the structures that
  fail with 54000 past the budget.
- **Widened** (G2 and M1), `CONTRACT.md:132`: `COUNT`, `SUM` and `AVG` of
  `DISTINCT` values of an expression (the 0A000 clause goes); `MIN` and
  `MAX` take TEXT, VARCHAR, NAME and a CHAR(n) column, bytewise, of type
  text or character (no length, printed padded to the column's n).
- **Exclusions** (`CONTRACT.md` *Exclusions*): gains `MIN`/`MAX` under the
  one collation, and text-to-date/time casts of forms the layer does not
  read (*Differences*).

#### Tests of #904 (front end)

Expected values come from PG 17 runs, never from the layer's output.

- `tests/parser_test.cpp`: any value expression in `GROUP BY`; positions,
  the non-integer constant (42601) and `GROUPING SETS` refusal with their
  positions.
- `tests/binder_test.cpp`: the cast table (each class, 42846 with its
  position, the refusals); `strict_null` of a cast; cast naming, every row
  of **Naming** and `(s || 'x')::text`; `GROUP BY` positions (42P10 for 0
  and past the list, 42803 for an aggregate's position), the alias rule
  (input column first, 42702), the matching table above (42803 at the
  column), the refusals with SQLSTATEs; `DISTINCT` of expressions and 42P10;
  `COUNT(DISTINCT expr)` and its jsonb 0A000; the `SET` assignment table
  (42804 with HINT and position, 0A000), `INSERT VALUES` still refusing a
  column expression; `MIN`/`MAX` of text types and their result types,
  bytea 42883. Added by the reviews: `(SELECT 1)::text` named `?column?`
  and `(SELECT 1 AS x)::text` `x`; the int2/int8/DECIMAL with boolean
  pairs 42846; `GROUP BY -1` 42P10, `GROUP BY (1)` a position, `SELECT a …
  GROUP BY 1::int` 42803 at `a`; the constant-matching table (`a + 01`
  matches, `a + 1.0` and `a + 1.00` do not); a grouped primary key with an
  expression key (`SELECT id, b, lower(s) … GROUP BY 1, lower(s)` binds,
  `GROUP BY id + 0` 42803 at `id`, `SELECT lower(b) … GROUP BY id` 42601);
  `SET t = $1::int` typing `$1` as integer, `SET n = $1::text` 42804;
  `SET`'s two error classes (`SET n = 'zz', a = 99999` 22P02 with a
  position; `SET n = 1/0, a = 99999` and its mirror 22003 `smallint out of
  range`); `max(ch)` typed CHAR with no length and `max(coalesce(ch, ch))`
  0A000. Assertions that pin today's refusals change: :182 (the
  Add/Sub message), :409, :509, :1640, :1659, :2004.
- `tests/api_test.cpp`, `tests/jsonb_accessors_test.cpp`: the embedded
  results of each form, including `GROUP BY props->>'k'`, `DISTINCT
  props->>'k'`, `count(DISTINCT props->>'k')`, `UPDATE … SET s = v->>'k'
  RETURNING`, `max(j->>'k')`, a cast raising 22P02 only for an evaluated
  row, and a `SET` raising with nothing written. Over an **empty table**:
  `SELECT count(*) … GROUP BY 1+1`, `… GROUP BY $1`, `… GROUP BY
  'x'::text`, `SELECT 1, count(*) … GROUP BY 1` and `SELECT DISTINCT 1`
  return no rows, and `SELECT count(*)` one. Casts: `3000000000::bigint
  ::int` and `70000::int::smallint` of columns 22003 (`BC_C_RANGE`),
  `'5874897-12-31'` date to timestamp 22008, a 70-byte text to name 63
  bytes, a CHAR(3) `'ab'` to text `ab`, `'NaN'` and `'inf'` float to
  DECIMAL(3, 0) 22003 with their DETAILs, `'1e39'` to real. `UPDATE … SET
  s = s + 1` at 32767 raising `BC_C_RANGE` on the table's id; `SET v = t
  || '    '` into a `varchar(3)` 22001 (the known difference); `UPDATE …
  SET t = $1::int` of `'007'` writing `7`; `max(ch)` printed padded.
- `tests/pg_session_test.cpp`, `tests/server/test_wire.py`: Describe of
  `$1::text::int` (25 → 23), of `max(v)` (25) and `max(ch)` (1042 with
  typmod -1, and its value padded to the column's `n`), of `UPDATE … SET
  t = $1::int` (`$1` 23), of
  `numeric(5,2)` casts with their typmod.
- Functional cases, each recorded from PG 17 with
  `tests/functional/record.py --pg`: `casts.yaml` (a column case per pair
  of the cast table, runtime errors without a position, naming),
  `where_expressions.yaml` (casts in filters), `aggregates.yaml` (`GROUP
  BY` expressions, positions, aliases, `DISTINCT`, `COUNT(DISTINCT
  expr)`, `MIN`/`MAX` of text, varchar, char, of `j->>'k'`, per group,
  over no rows, in `HAVING`), `dml.yaml`, `returning.yaml` and
  `on_conflict.yaml` (`SET` expressions, the assignment table, the error
  order of two failing items), `jsonb.yaml` (the accessor forms of each).
  No sqltest id is marked for these gaps; the run stays green.

#### Stages of #904

Each row is one PR, in the recommended order: smallest that unblocks
something first, each correct on its own and each exercised by its own
tests.

| Stage | Builds | Unblocks (Mattermost-style) | Size (code + tests + docs) |
|---|---|---|---|
| M1 | `MIN`/`MAX` of text: the plan API's bytes slot, a stream's holder; depends on no other stage | `SELECT max(Id) …`, `min(Props->>'k')` | ~700 lines: binder 40, physical 40, codegen 120, plan API 180, tests 270, docs 50 |
| C0 | input fixes shared by constants, parameters and (later) casts: `integer_from` (`0x`, `0o`, `0b`, `_`, the value in its 22003), `float_from`'s `real` (`strtof`, its message), `decimal_from`'s `'Infinity'`; the `ARRAY[ARRAY]` message | `'0x1F'::int`, a hex integer parameter | ~300 lines: text 90, binder 10, tests 170, docs 30 |
| C1 | casts of expressions: `cast_kind`, the `Cast` kind replacing `JsonbCast`, the plan API's cast helpers, cast naming; casts in procedure expressions | `WHERE (Props->>'x')::bigint > $1`, `ORDER BY (Props->>'n')::int`, `sum((Props->>'n')::int)`, `$1::text::int` | ~1,150 lines: binder 190, physical 50, codegen 150, plan API 330, tests 370, docs 60 |
| S1 | expressions in `SET` with the assignment table, the store shared with `INSERT … SELECT` (closes #824), column-order evaluation, `$n::T` of another type; needs C1's output and rounding helpers | `UPDATE … SET Message = lower(Message)`, `SET n = (Props->>'n')::int`, `SET s = Props->>'k'`, `SET flag = CASE … END`; with jsonb stage 4, `SET Props = jsonb_set(Props, $2, $3)` | ~950 lines: binder 240, physical 60, codegen 130, tests 440, docs 80 |
| G0 | `GROUP BY` positions and output names over **column** items (the input-column-first rule, 42P10, 42702, 42601 for a non-integer constant); binder only, keys stay columns; a position naming an expression item stays 42601 until G1 | `SELECT UserId, count(*) … GROUP BY 1` | ~350 lines: parser 60, binder 110, tests 160, docs 20 |
| G1 | `GROUP BY` of expressions (positions naming one included), keys the same for every row; `SELECT DISTINCT` of expressions; `same_expr` extended; the grouped resolver's exact-key case; `GroupKey`; expression keys in the structure, never streamed | `SELECT Props->>'k', count(*) … GROUP BY 1`, `SELECT DISTINCT Props->>'type'` | ~1,050 lines: binder 190, logical 30, search 140, physical 80, codegen 180, tests 390, docs 40 |
| G2 | `COUNT`, `SUM`, `AVG` of `DISTINCT` values of an expression: `GroupLayout::distinct` as an `Expr`, the evaluated value appended as a key part, the one-row Aggregate's fold (`codegen.cppm:2271`) keying an expression by its value | `count(DISTINCT Props->>'user_id')` | ~550 lines: binder 40, physical 60, codegen 150, tests 260, docs 40 |

`GREATEST`/`LEAST` (Mattermost's `SET LastPostAt = GREATEST(LastPostAt,
$1)`) and P3's expressions over keys and aggregates are not #904's rows:
open question 12 and stage P.

#### Differences from PG 17 left after #904

- **Text to date and time types.** The layer's input functions read fewer
  forms than PG's `DateTime` parser: `'today'`, `'epoch'`, `'infinity'`,
  `'Jan 1 2024'` and `'20240101'` are values in PG 17 and 22007 in the
  layer (open question 5). A string the layer does read gives PG's value.
- **Float text.** A REAL or DOUBLE PRECISION cast, or assigned, to text
  stays refused (open question 4).
- **Hexadecimal floats.** `'0x1F'::float8` is 31 and `'0x1.8p1'::float8`
  3 in PG 17 (`float8in` is `strtod`); the layer's `float_from` reads
  decimal only (`from_chars`), so they are 22P02: an error where PG
  returns a value.
- **NaN into DECIMAL(p, s).** PG 17 stores NaN (`'NaN'::float8::numeric(3,0)`
  and `'NaN'::numeric(3,0)` are `NaN`); the layer's DECIMAL has no NaN, so
  it is 22003 `numeric field overflow`, DETAIL `A field with precision 3,
  scale 0 cannot hold NaN.`: an error where PG returns a value.
- **VARCHAR(n) assignments whose excess is spaces** stay 22001 in `SET`,
  as in `INSERT` (the existing entry under *Known differences from
  PostgreSQL*).
- **Group key spellings.** `GROUP BY lower(b)` with `SELECT lower(b::text)`
  is one expression to PG's `equal()` after coercion and two to the AST
  match: 42803 or 42601 where PG runs. So are a select item `a + '1'` or
  `a + 1::int` against `GROUP BY a + 1`, which PG 17 coerces to one int4
  constant. Columns are matched by what they
  resolve to, so qualifiers do not matter.
- **Parameter-only casts are not key bounds** until stage P2.
- **The collation** of `MIN`/`MAX` of text, as of comparisons.
- **Errors under `LIMIT`.** PG 17's sorted `GroupAggregate` can stop before
  a row whose key raises; the layer's structure evaluates every key. An
  error where PG returns rows, never the reverse.

#### Open questions on #904

1. **Order of the stages.** Recommended: M1, C0, C1, S1, G0, G1, G2, as
   the table. M1 depends on nothing and is the smallest real unblocker
   (`max(Id)`); C0 is small and fixes input the later stages share; C1 is
   the prerequisite of S1's text assignments and of `GROUP BY` of a cast,
   and alone unblocks typed comparisons of `->>`; G0 is binder-only and
   can go any time. Alternative: S1 first without I/O assignments, if
   `SET` is wanted sooner.
2. **NUMERIC of no precision in runtime casts.** Recommended: keep 0A000 in
   C1 (jsonb to `numeric` stays), a follow-up issue for numeric_in and
   numeric_out helpers. The layer's NUMERIC is new and has no evaluator
   of its own yet.
3. **Casts to `VARCHAR(n)` and `CHAR(n)`.** PG truncates (`'abcd'::varchar(3)`
   is `abc`) and pads. Recommended: keep 42601, as for constants, and a
   follow-up issue; no client in #889 needs it. Assignment to such a column
   is not a cast and is built (22001).
4. **Float to text.** Recommended: keep refused (42601) until the layer's
   float text is PG 17's `float8out` (shortest round-trip,
   `0.30000000000000004`, `1e+100`), then lift it for constants, casts,
   assignments and the wire together, under one follow-up issue.
5. **Text to date and time forms the layer does not read.** Recommended:
   build the casts and raise 22007 for them, listed as a difference: an
   error where PG returns a value, never another value. Alternative:
   refuse the cast from text to a date or time type (42601).
6. **`id = $1::text::int` as a key bound.** Recommended: a filter in C1;
   stage P2's computed operand makes it a bound with no work of #904's.
7. **How keys are matched.** Recommended: the AST, by extending
   `ast::same_expr`: columns by what they resolve to, constants by literal
   class and typed value (an integer and a numeric literal never match;
   numeric scale counts), before binding so positions survive. `a + '1'`
   and `a + 1::int` against `a + 1` stay 42803 where PG matches, listed.
   Matching bound trees fails on hidden parameters; teaching `==` to see
   through them couples binding to grouping.
8. **G1's select list.** Recommended: exact key items only, with PG's
   42803 found first; expressions over keys and aggregates in P3 (owner
   decision 4 of stage P), whose Emit leaf they share.
9. **CHAR(n)-typed keys, DISTINCT values and `MIN`/`MAX` arguments of an
   expression.** Recommended: 0A000 in G1 and M1 (a CHAR(n) column stays
   taken). A CHAR value is stored without trailing
   spaces, but whether every CHAR-typed expression's value is so
   normalised before it reaches a sort key is not checked; a key that kept
   them would split a group PG keeps whole.
10. **Order of `SET`'s errors.** Recommended: evaluate and store in column
    order, as PG 17 reports them, folded constants' errors included; only a
    literal's input error stays in written order with its position.
    Alternative: written order for everything, listed as a difference.
11. **S1's assignment pairs.** Recommended: PG 17's whole table above,
    minus NUMERIC of no precision and float to text (questions 2 and 4),
    shared with INSERT … SELECT, closing #824.
    The smaller slice (same type and family only) is simpler but leaves
    `SET t = n + 1` 0A000 where PG writes `'6'`.
12. **`GREATEST` and `LEAST`.** Mattermost's `SET LastPostAt =
    GREATEST(LastPostAt, $1)` needs them (PG 17: NULLs ignored,
    `greatest(NULL, 3)` is 3, all NULL is NULL). Recommended: a stage of
    its own after S1, under #896, in every value position at once.
13. **`EXCLUDED` inside an `ON CONFLICT DO UPDATE SET` expression** (`SET n
    = t.n + EXCLUDED.n`). Recommended: keep 42601 in S1 and open an issue;
    codegen has the proposed row in hand, but binding a second row source
    is its own change.
14. **Holding a text `MIN`/`MAX`.** Recommended: the plan API keeps the
    bytes beside the slot and hands out a copy in the step's arena
    (`planner_design.md`). A view into the slot would dangle when a later
    row replaces the value.
15. **`::text[]` in a path.** Recommended: design it with jsonb stage 4,
    with a path form that carries its dimensions, so the form changes once;
    `ARRAY[ARRAY[…]]` stays refused with the clearer message.
16. **Integer input of constants and parameters.** Recommended: fix
    `integer_from` in its own PR, C0, for constants, parameters and casts
    alike (`0x1F`, `1_000`, the value in the 22003 message), so they never
    disagree; it widens what constants and text-format parameters accept.
    `real`'s input and `'Infinity'` to DECIMAL ride with it.
17. **NaN into DECIMAL(p, s).** PG 17 stores NaN. Recommended: 22003 with a
    DETAIL in PG's form, listed as a difference, until the layer's DECIMAL
    has a NaN (no client in #889 needs one). Alternative: 0A000.
18. **The error class of a cast's range error.** Recommended: an integer
    or DECIMAL value outside its target type is `BC_E_CONSTRAINT`,
    `BC_C_RANGE` (as `bc_check_int`, `bc_jsonb_to_i64` and the row builder
    raise it today); every other cast error is `BC_E_DATA` with its
    SQLSTATE (`planner_design.md`, *Plan API calls for runtime casts*).

#### Edits to other documents once #904 is built

Each stage's PR: this section's status line; *What the layer doesn't do
yet* and the stage P refusal table's `SET` and `GROUP BY` rows;
*Aggregates* (:649-681, the text `MIN`/`MAX` and `DISTINCT` bullets) and
*SELECT DISTINCT and GROUP BY* (:683-720); *Literals*' cast bullets
(:1335-1373, the shared cast table); *Known differences from PostgreSQL* (the
collation bullet and the date forms); `jsonb_design.md` (the `JsonbCast`
rename, :1376; :1446's refusal); `planner_design.md`, *Expression filters*
(:1548-1556, whose claim that stage 3 gives `(props->>'n')::int > 5` holds
once C1 is built); `CONTRACT.md` as above; and `README.md`'s *Built* and
*Not built yet*.

## Binding

The binder (`sql/binder.cppm`) resolves names against the catalog version
the statement pinned, and types every expression by `types_design.md`'s
rules, as PostgreSQL does. The defaults of columns an `INSERT` leaves out
are the runtime's, filled by `bc_row_begin` (`catalog_design.md`).

**Parameters** are typed by their use, as PostgreSQL types `$n`:
- compared with a column, or combined with one in arithmetic: the column's
  type;
- assigned to a column: the column's type;
- cast, `$n::T` or `CAST($n AS T)`, and compared with or assigned to a
  column of type `T` (in a `WHERE`, `VALUES` or `SET`): `T`, which is the
  column's, so the cast changes nothing and the parameter binds as if
  bare. A cast to another type, or to `DECIMAL`, `VARCHAR(n)` or
  `CHAR(n)`, whose casts round, cut or pad, is not taken yet;
- cast in a select-list value of no aggregated query, with or without
  `FROM` (`SELECT $1::integer + 1`, `SELECT v + $2::bigint FROM t`): `T`, and
  its value is range-checked as a `T` before the plan runs (22003, 22P02
  for text that is no `T`). Alone in a select list it is named by its type
  (`int4`), as PostgreSQL names it; an operator of it is `?column?`; a
  cast of the cast (`$1::jsonb::int`) by the outer type (`int4`). A cast
  to `DECIMAL`, `VARCHAR(n)` or `CHAR(n)` is not taken yet, nor one to a
  type other than another use of the parameter gives it (*Known
  differences*). A cast parameter as an operand anywhere else (`SET n =
  $1::integer + 1`, `WHERE n > $1::integer + 1`, `HAVING`, beside an
  aggregate) is not taken yet;
- combined only with an integer literal (`$3 - 20`, where `$3` has no
  other use): `integer`;
- otherwise: "could not determine data type of parameter $n" (42P18).

**One SQL type per parameter.** A parameter used with columns of two SQL
types is refused at bind, whether it is compared with them, assigned to
them or added to them. A `DECIMAL` of scale 0 counts as a `BIGINT` here.
PostgreSQL infers one type and runs the statement (*Known differences*).
A parameter's range is checked against the narrowest of its uses before
the plan runs, and a refusal names that use's table.

**Fixed parameter types.** A prepared statement keeps the parameter types
it was first bound with, as PostgreSQL's does, and is bound again with them
after a DDL (`bind` with fixed types): each `$n` starts typed. A fixed
integer type compared with, or assigned to, an integer column of another
width is taken, as PostgreSQL's cross-type operators (`int8 = int4`) and
assignment casts take it, and its value is range-checked as the fixed type
before the plan runs (22003), as PostgreSQL's Bind reads it, so an `integer`
`$1` assigned to a column now `bigint` refuses 5000000000. Any other use the
type does not fit is the binder's error:

- where PostgreSQL has no operator or assignment cast either, its SQLSTATE:
  42883 for a comparison (an `integer` `$1` compared with a `date` or `text`
  column, "parameter $1 of type integer cannot be used with a (date)", not
  PostgreSQL's "operator does not exist: date = integer"), and 42804 for an
  assignment, in PostgreSQL's words;
- where PostgreSQL converts the value, refused (42601, *Known
  differences*): an integer `$n` compared with or assigned to a float or
  `DECIMAL` column, and any value assigned to a string column.

PostgreSQL reads a parameter's value as its fixed type at Bind, before it
revalidates the plan: a date sent for an `integer` `$1` is 22P02 there and
the binder's error here.

**Binding again after a DDL.** A cached statement is bound again at the
live catalog after any DDL (`catalog_design.md`, *Compatibility*), so `*`,
`t.*` and NATURAL expand to the columns the tables have then, USING resolves
again, and a name a new column makes ambiguous is 42702. A statement run by
text returns those columns; a prepared one whose columns change is 0A000.

**DDL is bound where it runs.** A query, `INSERT`, `UPDATE` or `DELETE` is
bound when it is prepared, so its binder errors are thrown by `prepare` and
at Parse. A DDL is not: `prepare` parses it and classifies it from its
syntax tree, and it is bound at execute, against the catalog it runs in, as
PostgreSQL analyses a utility statement only when it runs. Its binder errors
(a table, column or role that does not exist, a column name taken) are
thrown at execute, and a DDL prepared before the table it names is created
runs once it exists. A syntax error is the parse's, at prepare, for every
statement. `CREATE TABLE AS` and `SELECT … INTO` bind their query at
prepare, as before (`catalog_design.md`, *CREATE TABLE AS*).

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
  their message and position (`binder_test`). The planner's refusals of a
  logical plan it can't use, which the binder never builds, are tested with
  their messages (`search_test`).
- **Against PostgreSQL.** The proof machine generates the dialect's
  statements and compares them with PostgreSQL (`testing_design.md`). It
  generates only statements the planner serves, so it never compares a
  planner refusal with PostgreSQL. `test_tprocc.py` runs the five TPROC-C
  transactions against PostgreSQL statement by statement.

## Known differences from PostgreSQL

The proof machine expects each one, or doesn't generate it, so it doesn't
fail on them.

- **A `SELECT` joins at most 32 tables** (owner decision, 2026-11-18). A
  33rd is refused, 0A000 `joins of more than 32 tables are not supported`,
  at the 33rd table; PostgreSQL has no cap. The search holds a set of
  tables as a 32-bit mask, and past 12 tables joins them in `FROM` order
  (`planner_design.md`, *Joins of several tables*): there a table not
  joined to any table written before it is crossed with all of them, at a
  cost that grows with the product of their row counts (a 13-table star of
  4-row tables, its centre written last, takes 29 s; PostgreSQL reorders
  it). The machine joins fewer.

- **An `UPDATE` of a primary key checks each row's new key in the order
  it reads the rows**, key order for a scan, as PostgreSQL checks them in
  the order it reads them, heap order for a sequential scan. Either refuses
  a row moving into a key a row it has not moved yet holds (23505), so a
  statement that succeeds in one order can fail in the other, as one can
  in PostgreSQL between two physical layouts of the same rows.

- **A VARCHAR(n) value whose excess is only spaces is refused.** (A
  `CHAR(n)` cuts it, as PostgreSQL does: `types_design.md`, *CHAR(n)*.)
  PostgreSQL truncates it to n characters and stores it; the layer refuses
  it as too long (22001). The machine doesn't generate one.
- **A DEFAULT outside its column's type is refused by the DDL.** The layer
  refuses `CREATE TABLE` (and `ADD COLUMN`) with, say, `SMALLINT DEFAULT
  100000`, `REAL DEFAULT 1e39` or `VARCHAR(2) DEFAULT 'abc'`. PostgreSQL
  accepts the DDL and refuses each INSERT that takes the default (22003,
  22001; checked on 16.15). A DEFAULT that is not UTF-8 never reaches
  either: PostgreSQL refuses the statement's text (22021). The machine's
  defaults stay inside their types.
- **Comparing columns of two tables of different SQL types is refused,**
  in a `WHERE` and in a join, where PostgreSQL casts one to the other's
  (42601; a pair PostgreSQL has no operator for is its 42883). One key
  encoding serves each join comparison. A `CHAR(n)` with a `VARCHAR` is
  refused too, in a join and in one table: PostgreSQL compares the two as
  bpchar. Two columns of one table compare across the numeric types above
  (*`WHERE`*); joining them is #823. TPROC-C compares columns of one type
  only.
- **A prepared statement's fixed parameter type that PostgreSQL would
  convert is refused** (*Fixed parameter types*). After a DDL makes `a` a
  `DOUBLE PRECISION`, a `DECIMAL` or a `TEXT`, a statement prepared with
  `a INTEGER` and `WHERE a = $1`, `SET a = $1` or `VALUES ($1)` is 42601
  until it is prepared again; PostgreSQL converts the `int4` value and runs
  it. Integers of another width run, range-checked as the fixed type.
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
- **`$n::T` beside another use of `$n` of another type is refused.**
  PostgreSQL types `$n` by its first use in the statement's text and casts
  that value to `T`: `SELECT $1::bigint FROM t WHERE k = $1`, with `k
  INTEGER`, runs there. The layer types a parameter once, as `T` too, and
  refuses the pair (42601), as it refuses two column types (above). The
  same type in both uses runs.
- **`VARCHAR` without a length is `TEXT`, and described as `text` (OID
  25).** PostgreSQL describes it as `varchar` (OID 1043). The values and
  their comparisons are the same; only the type a client is told differs:
  a column declared `VARCHAR`, a constant cast to it (`'a'::varchar`), and
  a parameter cast to it (`SELECT $1::varchar`, the parameter and the
  column both 25). A `VARCHAR(n)` is described as PostgreSQL describes it.
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
- **A numeric quotient of columns is refused; one of constants is exact
  to 18 digits.** PostgreSQL's scale follows the operands' values, so
  `d / 2` has a scale per row; a result column has one type
  (*Expressions over columns*). A quotient of constants whose scale is
  past 18 (`1.0 / 3`, twenty places) is refused (42601). The machine
  divides integers only.
- **An expression over columns that leaves int64 is 22003.** A product of
  two large `DECIMAL`s, or a `bigint` beside a `DECIMAL` that needs
  rescaling, is where PostgreSQL's `numeric` stays exact. A `bigint`
  product or quotient out of range is "integer out of range", where
  PostgreSQL's message says "bigint"; the SQLSTATE is the same.
- **A SUM or an AVG whose running total leaves `int64_t` fails** (`BC_E_ARITH`). The
  layer adds in 64 bits, a BIGINT's or a DECIMAL's scaled integer, and the
  statement fails as 22003; PostgreSQL's `SUM` of a BIGINT or NUMERIC is an unbounded `numeric`. Below that the
  sums agree exactly (`test_sum_past_int64_is_an_error`). The machine sums
  no BIGINT column; no TPROC-C sum comes near.
- **MIN and MAX of a DOUBLE PRECISION column keep the last of `-0.0` and
  `0.0` read**, as PostgreSQL's do, so which of the two shows depends on the
  order the rows are read, which is the plan's and not the heap's. The two
  compare equal and differ only in sign. The machine takes no `MIN` or `MAX`
  of a float.
- **SUM and AVG of DISTINCT DOUBLE PRECISION values add them in the order
  they were first read**; PostgreSQL sorts the values and adds them
  ascending. Float addition is not associative, so the two can differ in the
  last digits (`0.1`, `0.2`, `1.5`, `2.5` and `4` sum to `8.3` ascending and
  to `8.299999999999999` added as 1.5, 2.5, 4, 0.1, 0.2). Without `DISTINCT` both add in
  the order the rows are read, which is the plan's and not the heap's. The
  machine's floats are exact binary fractions.
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
  no column a unique index holds. `ON CONFLICT DO UPDATE`'s `SET` checks a
  new unique value the same way; a proposed row whose arbiter key an
  earlier row's update left is inserted, as in PostgreSQL.
- **A sort, a structure of groups, or the keys a set operation tells
  duplicates apart by, past the statement's memory budget fails** with
  54000, where PostgreSQL spills it to disk (`planner_design.md`, *Memory*
  and *Set operations*).
- **A constant a column's type does not hold exactly is refused in a
  comparison** (42601): `smallint_col = 40000`, `int_col = 1.5`,
  `dec2_col = 1.005`, a REAL compared with a number that is not an integer
  within 2^24 (even 1.5, which a float holds), a REAL constant
  (`1.5::real`) compared with a REAL or DOUBLE PRECISION column, a
  `DECIMAL` compared with one past 18 digits at its scale. PostgreSQL
  compares in a wider type and answers. The REAL cases are a gap (#822):
  `real_col = '1.5'` and `real_col = $1` already compare. The layer compares each column in
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
- **A `CASE` or `COALESCE` of constants whose untaken result would raise
  is refused.** `SELECT CASE WHEN true THEN 1 ELSE 1 / 0 END` is 1 in
  PostgreSQL; here the folder evaluates a result to learn its type, so
  the statement is refused with 42601. A `WHEN` after the first true one
  is skipped, as there. With a column, parameter or subquery among the
  branches the result is typed without evaluating it and the branch is
  evaluated only when taken (*Expressions over columns*).
- **A numeric `CASE` or `COALESCE` over columns has one scale,** the
  finest of its branches; PostgreSQL's keeps each value's own.
- **A `CASE` of one `CHAR(n)` branch and no `ELSE` is a `CHAR(n)`.**
  PostgreSQL's is a `bpchar` of no length (the default NULL has none),
  unless its planner folds the `CASE` away (`CASE WHEN true THEN c END`), so
  a `CREATE TABLE AS` of it there takes a longer value the layer's table
  refuses (22001). The layer has no `CHAR` of no length.

- **`EXTRACT`'s unit is checked when the statement is bound,** so a unit
  the operand's type lacks (0A000) or no type has (22023) refuses the
  statement even where no row reaches it, an empty table or a column of
  only `NULL`s, or where the value is a typed parameter bound to `NULL`
  (`EXTRACT(HOUR FROM $1::date)` given `NULL` is 0A000; PostgreSQL gives
  `NULL`). PostgreSQL reads the unit at each call and never calls the
  function for those. A `NULL` constant folds to `NULL` before the unit is
  read, as in PostgreSQL. The error is the one PostgreSQL gives the first
  row it reads.

- **A view is bound again at each use,** so one its tables have outgrown
  is refused (0A000) until it is dropped and created again: a `SELECT *`
  view after `ALTER TABLE … ADD COLUMN`. PostgreSQL keeps the columns the
  star gave it.

- **`ON CONFLICT DO NOTHING` skips a row whose conflicting row a
  concurrent commit deletes** (owner decision, 2026-11-19). The check reads
  the statement's snapshot and guards nothing, so a row the snapshot holds
  under the arbiter is skipped even when another session's commit deletes
  it before this statement commits, and the statement commits with the row
  not written. PostgreSQL, in `READ COMMITTED` and in `REPEATABLE READ`,
  waits for the deleting transaction and then inserts the row. A guard on the row would make the
  commit a conflict, and the autocommit retry would insert it; that costs
  a guard per skipped row and was not taken.

Each is a refusal or an error, not a different answer, except the last.
Everything else in the dialect matches PostgreSQL, and the proof machine
checks that it does.

## What the layer doesn't do yet

Gaps, not decisions: PostgreSQL runs each, and the layer refuses it until
it is built. The functional suite marks the cases that need one as not
implemented (`functional_testing_design.md`).

- **`TIME WITH TIME ZONE` is partial.** The type, its order, keys,
  `CURRENT_TIME` and the casts of constants are built (`types_design.md`,
  *TIME WITH TIME ZONE*). Refused: a precision under 6
  (`TIME(3) WITH TIME ZONE`, 0A000), which would round on store; a zone
  name or abbreviation in its text (`'12:00 PST'`, 22007), as the layer
  has no time zone database; its arithmetic, all of which takes or gives
  an `interval` or a timestamptz (42601); `EXTRACT` of it (0A000); a
  `TIME` column compared with one (42601); a timestamptz constant compared
  with one (42601, where PostgreSQL has no operator, 42883); a bare time
  after ISO 8601's `T` (`'T12:00'`, 22007, for `TIME` too). Tracked in
  #866.

- **A type name of three parts is refused** (0A000 `cross-database
  references are not implemented`), the current database's too:
  PostgreSQL accepts `postgres.pg_catalog.int4` in the database
  `postgres` (owner decision Q5 (a), `catalog_design.md`, *Name
  resolution with schemas*).
- **Schemas are partial** (`catalog_design.md`, *Stages of schemas*):
  a view element whose text holds a
  `CREATE` or `GRANT` outside parentheses, `CREATE TRIGGER` as an
  element, privileges on a schema (#873), `SET search_path` (#871), and one
  table name from two schemas in one `FROM` with neither aliased, which
  PostgreSQL reads (an alias on either works here).
- **A procedure variable's type is read at the call.** A qualifier
  PostgreSQL refuses in a `DECLARE` (`x foo.int4`, `x pg_catalog.integer`)
  is refused by the layer's first `CALL` of the procedure, with
  PostgreSQL's SQLSTATE and message; PostgreSQL refuses the `CREATE
  PROCEDURE`.
- **A table needs columns.** The layer refuses `CREATE TABLE t ()` and a
  `CREATE TABLE AS` of a query of no columns (0A000); PostgreSQL accepts a
  table of no columns.
- **`CREATE TABLE AS` is narrower.** PostgreSQL creates temporary and
  unlogged tables, columns of a `NUMERIC` of no precision (the layer has
  the column type, not yet the conversion of a query's values to it), takes `SELECT … INTO` in a set operation's first
  arm, and creates the table of `WITH NO DATA` over a query whose constant
  raises an error; the layer refuses each (0A000).
- **Run-time parameters are the transaction modes only.** `SET`, `SHOW`
  and `RESET` are read by PostgreSQL's grammar (`ast::parse_variable`, a
  syntax error PostgreSQL's 42601), and the server keeps
  `transaction_isolation`, `transaction_read_only`,
  `transaction_deferrable`, their three `default_transaction_…` defaults
  and `client_encoding` (`server_design.md`, *Run-time parameters*). Any
  other parameter (`search_path`, #871, `TIME ZONE`, …), `SHOW ALL` and
  `SET … FROM CURRENT` are refused (0A000); PostgreSQL sets them.
- **Transactional DDL never waits.** A `Transaction`, and over the wire
  a `BEGIN` block or an extended-query block of several statements, runs
  DDL, `CREATE TABLE AS` and `SELECT … INTO` included, as a step of the
  transaction, published at its commit, as PostgreSQL does; where
  PostgreSQL makes one transaction wait for another's DDL, the second to
  commit fails with 40001 (`transaction_design.md`, *Transactional DDL*).
- **A set operation cannot be ordered, and its arms are plain.** PostgreSQL
  sorts the combined rows with `ORDER BY` and combines queries that group,
  aggregate or select `DISTINCT`. Here a set operation streams its arms
  into a structure of keys and holds no typed row, so the first waits for
  an object in the plan API that does (#681), and the second for the same
  streaming at a grouped query's emit (#682).
- **Views take `INSERT` only, and a query over a grouped view is limited.**
  PostgreSQL updates and deletes through a simple view (#678), checks the
  rows an `UPDATE` writes with `WITH CHECK OPTION`, replaces a view with `CREATE OR
  REPLACE VIEW`, and joins any view, or reads one with `DISTINCT`, `LIMIT`
  or a set operation, as a subquery in `FROM`. Here a view's query is
  merged into the statement, and only a grouped view that a query
  aggregates over, or that is joined with a table, is read as a derived
  relation, grouped by integer and boolean columns only (#833), and in a
  join only as the outer side of a join with a table, unsorted and
  ungrouped (#834, #835); the rest wait for updatable
  views and derived tables in general.

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
