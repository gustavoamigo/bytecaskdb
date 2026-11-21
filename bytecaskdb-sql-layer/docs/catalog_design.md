# The SQL layer's catalog

Status: **built** (#489): `bytecaskdb-sql-layer/catalog/`, with its tests in
`tests/catalog_test.cpp` and the TPROC-C schema in `tests/tprocc_test.cpp`.
This covers what the catalog records, how it is stored under namespace
`0x01`, the in-memory form plans resolve against and how readers share it,
what makes a compiled plan compatible with the live catalog, and how each
DDL statement writes. Views are in *Views*; procedures in
`procedures_design.md`, *Catalog record*. Nothing in the engine changes
for it.

The catalog is written from scratch. The plugin's catalog is positional,
keyed by name and GPL-2.0, and nothing of it is copied. The plugin would
share this one only if it adopted the layer's formats (#488).

## What a table records

| Field | Meaning |
|---|---|
| `id` | Permanent object id, from the one counter below. Never reused. |
| `name` | As the parser hands it over: an unquoted identifier folded to lower case by PostgreSQL's rules, a quoted one exactly as written, so `"Foo"` and `foo` are two tables. Plans never paste a name into C: their constants are named from ids (`TBL_1`, `IDX_3`, `COL_1_2`, `PART_3_0`), so a name need not be an identifier and two objects' constants cannot collide. The plan's header lists what each constant stands for, in `//` lines whose user bytes outside printable ASCII, `\` and `?` are `\xNN` escapes. |
| `version` | Per-table counter, bumped by every DDL on the table. Not checked by plans (see *Compatibility*); kept for several schema versions (*Several schema versions*). |
| `next_col_id` | The next column id. Column ids are permanent and never reused, even after a column is dropped. |
| columns | Each: permanent `id`, `name`, SQL type (kind, `VARCHAR` length, `DECIMAL` precision and scale), nullability, default, and whether it is hidden: the row id of a table declared without a primary key (*Tables without a primary key*). The physical type follows from the SQL type (`types_design.md`, *Types*). |
| primary key | Column ids, in key order. Always ascending; a primary key column is never null. For a table declared without one, its hidden row id. |
| indexes | Each: permanent `id`, `name`, unique or not, parts as (column id, `ASC`/`DESC`). |
| checks | Each `CHECK` constraint: its name and the text of its expression (*CHECK constraints*). Empty for most tables. |
| layouts | Each layout generation: its object id, its generation number, `bitmap_bytes`, and its offset table (column id → byte offset or variable slot). The last is current. Each table has one until `REBUILD` exists, which appends one (#503). |

A default is a constant of the column's physical type, or none (null).
DDL accepts integer, decimal, string and boolean literals, folded at bind
time to the stored value (a `DECIMAL(p, s)` default is stored scaled).
`TIMESTAMP`, `DATE` and `TIME` literal defaults are refused, since literal
parsing of those types isn't built for defaults.

**A default of the clock** (`CURRENT_DATE`, `CURRENT_TIMESTAMP` and
`now()`, `LOCALTIME`, `LOCALTIMESTAMP`, `CURRENT_TIME`, with a precision) is a
`ClockDefault`: its function and precision. They are two `u8`s that trail
the column's entry, written only for one, so a record without one is
unchanged, and an older build refuses one with them (trailing bytes). The
column's stored constant default stays null, since the binder, not
`bc_row_begin`, fills the column.
- **How it is filled.** The binder gives an `INSERT` that leaves the column
  out, and a `DEFAULT` written in `VALUES` or `SET`, the statement's
  reading of the clock, a slot the API fills each run (`frontend_design.md`,
  *The time*).
- **Which types take one.** A column takes one only where PostgreSQL's
  assignment cast exists and the reading gives its value
  (`clock_assignable`): a `DATE` `CURRENT_DATE` or a timestamp's date, a
  `TIME` any of them but `CURRENT_DATE`, a `TIME WITH TIME ZONE`
  `CURRENT_TIME`, `LOCALTIME` or `CURRENT_TIMESTAMP` at offset 0, a
  `TIMESTAMP` or `TIMESTAMPTZ` either timestamp. A date to a timestamp,
  which is midnight and not the reading, is refused.
- **`ADD COLUMN`.** It is refused with one: plans compiled before it fill a
  new column from the stored default.
- **A session function** (`CURRENT_USER`, `CURRENT_ROLE` and `USER`, all
  `CurrentUser`; `SESSION_USER`; `CURRENT_CATALOG`; `CURRENT_SCHEMA`) is a
  `ClockDefault` too, functions 5 to 8 after the clock's 1 to 4
  (`CURRENT_TIME`, added later, is 9), its
  precision unused. Its value is a `name`, so a `NAME`, `TEXT`,
  `VARCHAR(n)` or `CHAR(n)` column takes one and no other; load refuses
  one on another type, as it refuses the clock's (`frontend_design.md`,
  *Session functions*).

## Serialization

Little-endian, every list counted, every string a `u16` length and its
bytes:

```
u16 format                   ; 7
u32 id, str name, u32 version, u32 next_col_id
u16 ncols   { u32 len, u32 id, str name, u8 kind, u32 length, u8 precision, u8 scale,
              u8 nullable, u8 default_tag, default, u8 hidden }
u16 npk     { u32 col_id }
u16 nidx    { u32 len, u32 id, str name, u8 unique, u16 nparts { u32 col_id, u8 desc } }
u16 nlayout { u32 len, u32 layout_id, u16 generation, u16 bitmap_bytes, u16 var_table, u16 nvar,
              u16 nslots { u32 col_id, u32 pos } }
u16 nchecks { u32 len, str name, blob expression }
```

A layout records where its fixed region ends (`var_table`) and its slot
count rather than deriving them from the positions, so a position a
dropped column leaves at the end of a region stays reserved.

`default_tag` is 0 for none, else the physical type code (`bc_type`),
followed by 8 bytes for `i64` and `f64`, one for `bool`, a `u32` length and
the bytes for `bytes`.

**The append-only rule.** A later format adds fields only at the end of the
record or of an entry, never reorders or reinterprets one. Columns, indexes
and layouts are length-prefixed entries, so a later format can extend them
too. A build refuses a newer format, whose added fields it may not be able
to ignore safely ("written by a later SQL-layer build").

**The record format names the row and key formats.** A layout's positions
are for one row format, and its table's keys are in one key format, so a
change to either is a new record format. Format 7 is the one of
`types_design.md`, *Row format* and *Key encoding*: fixed columns and
`SMALLINT`, `INTEGER` and `DATE` key parts at their declared widths, the
8-byte row header (#780). Every table record is written as 7. Formats 1 to
6 laid every fixed column and integer key part out as 8 bytes under a
12-byte row header; a build refuses them ("written by an earlier SQL-layer
build, whose row and key formats this build does not read"), so a database
from before #780 does not open and none of its rows or keys is decoded as
format 7's. There is no migration: such a database is reloaded.

**A record that does not parse is refused at load**, with the table's id:
a truncated field, a type kind or tag outside its set, a physical type that
disagrees with the SQL type, a key or index column that is not a column of
the table or appears twice, a layout that fails the row format's invariants, trailing bytes
after the last field, a hidden column that is not the whole primary key, a
BIGINT, nameless and without a default, or that an index names, or a second
one. The catalog never opens a database it cannot decode correctly.

## Storage

Namespace `0x01`, as the plugin's catalog is:

| Key | Value |
|---|---|
| `01 01 01` | Next object id, `u64` big-endian. |
| `01 01 02` | Catalog version, `u64` big-endian; bumped by every DDL. |
| `01 02 <id:4>` | The table record above, indexes and layouts included. |
| `01 03 <name>` | Name entry: kind (1 table, 2 index, 3 view) and id (4, big-endian). |
| `01 04 <id:4>` | The view record (*Views*). |
| `01 05 <id:4>` | The procedure record (`procedures_design.md`, *Catalog record*). |
| `01 06 <name>` | Procedure name entry: kind 4 and id (4, big-endian). Procedures have a namespace of their own. |
| `01 07 <id:4>` | The role record (*Roles and privileges*). |
| `01 08 <name>` | Role name entry: kind 5 and id (4, big-endian). Roles have a namespace of their own. |
| `01 09 <id:4>` | The privileges (ACL) of the table, view, sequence or type of that id (*Roles and privileges*). |
| `01 0A <id:4>` | A type record: a domain or a shell type (*Domains, shell types and sequences*). |
| `01 0B <name>` | Type name entry: kind 7 and id. Domains and shell types share this namespace, and a table's or view's name counts in it, as its row type does in PostgreSQL. |
| `01 0C <id:4>` | A sequence record. Its name entry is `01 03`, kind 6: sequences are relations, with no row type. |

**The counters are 8 bytes**, and the next id at most one past
`kMaxObjectId`; any other value is refused at load. **One id counter** gives every table, index and layout its id, so an id
names one object for ever, and `bc_key_begin(table_or_index)` and
`bc_raise`'s object id need no kind. Ids stay at or below `kMaxObjectId`,
`0x00FFFFFF`: `bc_error_detail` carries one in 24 bits. A DDL that would
pass it is refused.

**The name entry** makes a name a key, so a `CREATE` guards its name with
`ensure_absent` and a rename is one key changed. Tables, indexes and views
share one namespace, as relations do in PostgreSQL. At load every name entry must
name an object of that name and every object must have its entry; a
mismatch is refused as corruption.

## Secondary-key prefix: `03 <index_id>`

The open question on #487 was `03 <index_id>`, as the plan API had it, or
`03 <table_id> <index_id>`, as the plugin has it. **Decided: `03
<index_id>`.**

- **The table id adds nothing.** An index id is global and permanent, so it
  alone names the index. The plugin needs the table id because its index id
  is MariaDB's `key_info[]` position, unique only within a table; the
  plugin adopts this catalog and its ids with the shared key format.
- **Dropping a table's indexes costs the same.** `DROP TABLE` writes one
  `del_range` for the rows and one per index, in one batch: one append for
  the batch, whatever the rows, against two with the longer prefix. A table
  has a handful of indexes.
- **Every index entry is 4 bytes shorter.** The key directory holds no key
  bytes, so this is not RAM, but it is 4 bytes of every index record on
  disk and in every read, and of every key a plan builds.
- **One prefix shape.** Rows and index entries are both `ns id(4) parts…`;
  `bc_key_begin`, the scans and the key-part decoders handle one width.

## Unique indexes

A unique index has the same entries as any other: the index parts, then the
primary key. Two rows with a null in a unique part therefore never collide,
which is the null semantics the types document fixes. A duplicate is a
second entry under the same index-part prefix, so the check is a prefix,
not a key. The runtime makes it inside `bc_insert` and `bc_update`
(`docs/plan_api_design.md`, *Writes*), in three steps:

1. **Probe the snapshot.** The first entry at or after the new entry's
   index-part prefix is read; if it has the prefix, the row is a duplicate.
   One record read, charged to the plan's budget.
2. **Raise.** A duplicate is `BC_E_CONSTRAINT` with `BC_C_UNIQUE` and the
   index id, which the runtime never retries.
3. **Guard the race.** Otherwise an `ensure_range_unchanged` over the prefix
   and its successor goes into the write plan, so the engine refuses the
   commit if any entry under the prefix changed since the snapshot.

Point existence and `ensure_absent` name one key, and an entry under the
prefix has the other row's primary key after it, so neither could see it.
The check is skipped when any part of the new entry is null. The primary
key is checked by existence; its race needs no guard, since the key is in
the write plan and the engine checks every written key for a change since
the snapshot. An `UPDATE` checks a unique index only when the row's prefix
changed, so a row never collides with itself. The plugin checks its unique
indexes by the same prefix scan.

Keeping one entry shape, rather than storing a unique index as its parts
with the primary key in the value, keeps `bc_scan_row`, the covering reads
and `DESC` the same for every index, and needs no second shape for the
entries whose parts are null.

## Tables without a primary key

PostgreSQL takes a table without a primary key, and keeps every row
inserted into it, duplicates included. The layer's rows are keyed (`02
<table_id> <primary key>`), so such a table gets a key of its own: a
**hidden row id**, a `BIGINT NOT NULL` column after the declared ones,
which is the whole primary key.

- **No statement sees it.** It has no name, and `Table::column(name)` never
  returns it, so no statement can name, set, compare, group or select it,
  and `CREATE INDEX` cannot take it. An `INSERT` without a column list
  fills the declared columns, as PostgreSQL does. A `GROUP BY` takes no
  functional dependency from it, as PostgreSQL takes none from a table
  without a key. The plan API, the plans and their constants are those of
  any table; a plan's header names the column `(row id)`.
- **The runtime fills it.** `bc_row_begin`, which writes a column's
  default, also draws the table's next row id
  (`LiveCatalog::next_row_id`), so an `INSERT` plan never names it, and a
  retried plan draws another. `bc_row_from` copies it, so an `UPDATE`
  keeps a row's id.
- **Where an id comes from.** Each `LiveCatalog` holds, per table with a
  row id, the next id to hand out, under a mutex held only to advance it.
  A table enters at its first insert, seeded from the engine's latest
  state: one past the greatest id its row range holds (one reverse step,
  `rkeys_from`), or 1 for none. No id of this catalog's is out before the
  seed, so every id in use is then committed and below it. Nothing is
  persisted: the data file is the only record, and the row keys are the
  count's record. So:
  - concurrent inserts never draw the same id, and so never conflict with
    each other on the key, in autocommit or in transactions;
  - an insert that fails, conflicts or rolls back leaves a gap;
  - while a catalog lives, no id is handed out twice;
  - after a reopen, the ids of rows deleted from the end of the table, or
    drawn and never committed, may be handed out again, as PostgreSQL
    reuses a `ctid`. No row holds them then.
  - A second `LiveCatalog` on the same open database (only tests make one)
    counts on its own; an id both draw is refused by the engine's check of
    every key a commit writes, never written twice.
- **The rejected alternative** was a stored counter per table. Every
  insert would write it, so any two concurrent inserts into the table
  would conflict, and it would be state outside the rows that has to
  agree with them.

## UNIQUE constraints

A `UNIQUE` constraint, on a column or on the table, is a unique index on
its columns, created with the table in the `CREATE TABLE`'s batch, so the
table never exists without it. There is no constraint object: the index is
the constraint. The catalog takes the constraints as PostgreSQL's
`transformIndexConstraints` does:

- each column must be the table's and named once (42703 "column … named in
  key does not exist", 42701 "… appears twice in unique constraint"),
  checked before the table's own name is (42P07);
- one on the primary key's columns, in order, is the key, and makes no
  index; one on an earlier one's columns, in order, is that index, which
  takes its name if it had none;
- a named one's index has its name, refused (42P07) where a relation, the
  table itself or an earlier constraint has it;
- an unnamed one's is named as PostgreSQL's `ChooseIndexName` names it:
  `table_col1_col2_key`, within 63 bytes, the longer of the table and the
  column part shortened first and each clipped to a UTF-8 character, then
  `key1`, `key2`… where a relation, or an index named earlier in the
  statement, has the name.

`NULLS NOT DISTINCT`, `INCLUDE`, `DEFERRABLE` and `ADD COLUMN … UNIQUE` are
refused, not yet built. A constraint's index is an index like any other:
`DROP INDEX` drops it (*Known differences* in `frontend_design.md`).

## The in-memory form

`Catalog` is immutable: the tables by id and by name, each index's table,
the key layout of every table and index (the parts `bc_key_*` and the scans
read, `DESC` included). It is built at open by
reading every record under `01 02` and the counters, and by every DDL from
the catalog before it. Lookups are `const` and allocate nothing.

**Readers share it through immutable versions.** `LiveCatalog` holds the
current version in an `atomic<shared_ptr<const Catalog>>`; a DDL builds the
next version and publishes it by storing the pointer, and a reader loads it
without a lock. A context pins one version
for an execution and drops it at reset, so an idle context holds no
superseded version; nothing a reader holds changes under
it, and a reader never waits for a DDL's backfill. A lock taken by every
execution would be simpler to state but would put a shared cache line on the
read path the engine keeps lock-free, and would stop every query for the
length of a `CREATE INDEX`.

Two rules make a pinned version agree with the engine:

1. **A version and a snapshot are taken together.** `LiveCatalog::pin`
   loads the current version, takes a snapshot, and reads the catalog
   version the snapshot holds (one record read). Equal, they agree. The
   snapshot can only be ahead: either this catalog's own DDL has reached the
   engine and not yet swapped the version in, which a generation counter
   (odd from just before the batch reaches the engine until the swap) tells
   apart, and the pin waits for the swap; or another catalog on the same
   database moved it, and the pin reloads the catalog from the snapshot and
   publishes it. The engine publishes a batch only after its sync, so no
   reader waits through a backfill's write and sync, and no plan pairs a
   snapshot with a version it does not hold.
2. **A write commit guards the records of the tables it writes.**
   `bc_ctx_commit` adds `ensure_unchanged(01 02 <table_id>)` for every table
   whose rows or index entries the write plan touches, found from the keys
   themselves. A DDL on that table committed after the plan's snapshot
   changes the record, the commit is a conflict, and `bc_execute` retries on
   a fresh pin, where the runtime writes the index entries the new version
   has and the compatibility check sends an incompatible plan back to be
   recompiled. One record read per table written, the price of never
   committing rows, or index entries, shaped by a catalog the engine has
   moved past.

DDL statements are serialized by a mutex in `LiveCatalog`, held from the
prepare to the publication, so a DDL never conflicts with another of the
same catalog; the engine's directory lock keeps a second process out. A
second `LiveCatalog` on the same open database (only tests open one)
catches up at its next pin, so its DDL never builds on a stale version and
never hands out an id twice.

## CHECK constraints

A `CHECK` constraint, on a column or on the table, is kept in the table's
record, in the `CREATE TABLE`'s batch, so the table never exists with a
constraint missing. A table has no object id for it, and no name entry:
constraint names are the table's own (PostgreSQL's are too), so they are
unique within a table and never collide with a relation.

- **The text is the definition,** as a view's is (*Views*): the expression
  as written between the parentheses. A write to the table binds it again,
  so the binder and `Gen::eval` are the only evaluator and there is no
  second form to version. A table's constraints are bound at each `INSERT`
  and `UPDATE` of it, and the plan holds the result.
- **Names** are as written, or as PostgreSQL's `ChooseConstraintName`:
  `table_column_check` for a constraint written on a column,
  `table_check` for the table's, then `1`, `2`… where the table has the
  name. Named ones are taken first. A name used twice is 42710.
- **Checked at creation.** The binder binds each expression over the table
  as it will be, so a column the table lacks (42703) or a result that is
  not `BOOLEAN` (42804) is refused with the table. What a write cannot
  evaluate over the row it has built is refused (0A000 or its SQLSTATE): a
  parameter, a subquery, an aggregate, `CURRENT_DATE`, `LOCALTIME` and
  `LOCALTIMESTAMP`, and any expression the select list cannot evaluate.
- **Evaluated on the row as written.** The generated plan builds the row,
  evaluates each constraint over it, and raises `BC_C_CHECK` (23514) where
  the result is false, before `bc_insert` or `bc_update`: nothing is
  written, no index entry included. True and NULL pass. The row has its
  defaults, so a constraint sees the value an omitted column gets, and an
  `UPDATE` tests every constraint of the table, as PostgreSQL does.
  Its cost is one evaluation of the expression per written row, in the plan's
  C: column reads and comparisons, no allocation.
- **The error** carries the table's id as `bc_raise`'s object. Its message
  says "check constraint N violated", where PostgreSQL names the relation
  and the constraint; the SQLSTATE is the same.
- **Not built:** `ALTER TABLE ADD CONSTRAINT`, `ADD COLUMN … CHECK`,
  `NO INHERIT`, `NOT VALID` and `DEFERRABLE`.

## Roles and privileges

What #653's first step stores. The format is little-endian, as the other
records are.
- **A role record**: `u16` format (1), `u32` id, `str` name. Its id is
  drawn from the one counter. The bootstrap superuser every session is
  stays implicit: it is never written, has no name, and its id is
  `kBootstrapRole`, `0x01000000`, past every object id.
- **One ACL record per table or view** with a grant: `u16` format (1),
  `u32` object id, `u32` n, then n entries, each a `u32` grantee (0 for
  `PUBLIC`), a `u32` column, a `u16` of privileges, a `u16` of grant
  options and a `u32` grantor.
  - The column is a table's column id or a view's column position, or
    `kWholeObject`, `0xFFFFFFFF`, for the object itself.
  - The privileges are PostgreSQL's `arwdDxtm` as bits 1 to 128. A
    column's are `SELECT`, `INSERT`, `UPDATE` and `REFERENCES` only.
  - The grantor is the superuser while every session is one.
- **Entries are sorted by (column, grantee)**, one each, and each holds a
  privilege. A `REVOKE` that leaves an object none deletes the record, and
  a `DROP TABLE` or `DROP VIEW` deletes it.
- **Load checks** refuse as corruption any of the following:
  - a role or ACL record under another id's key;
  - a role without its name entry, or a name entry without its role;
  - an ACL of no table or view;
  - an entry of a missing column or role;
  - an entry with no privilege, an option it lacks, or a column privilege
    a column cannot take;
  - entries out of order.

  A later format is refused, as for the other records.

## Domains, shell types and sequences

Objects of their own, stored and granted on. None is usable yet: a domain
is not a column's type, a shell type is for nothing (as in PostgreSQL),
and a sequence gives no values.
- **A type record**: `u16` format (1), `u32` id, `str` name, `u8` kind (1
  shell, 2 domain). A domain adds its base type: `u8` kind, `u32` length,
  `u8` precision, `u8` scale.
- **A sequence record**: `u16` format (1), `u32` id, `str` name. Its
  options and its counter are not built.
- **Namespaces.**
  - A domain or a shell type is refused where a type, table or view holds
    the name (42710), and a `CREATE TABLE` or `CREATE VIEW` where a type
    does (42710).
  - A sequence is refused where any relation holds the name (42P07).
  - `CREATE DOMAIN` over a shell type fills it in: the domain takes the
    shell's id.
- **Privileges.** A domain or a type takes `USAGE`, a sequence `USAGE`,
  `SELECT` and `UPDATE`, in the ACL record a table's would be in. A shell
  type takes none: a grant on one is 42704 (*only a shell*), as in
  PostgreSQL.
- **Drops.** `DROP DOMAIN` drops a domain, `DROP TYPE` a domain or a shell
  type, `DROP SEQUENCE` a sequence, each with its ACL. A drop by the wrong
  statement is 42809.
- **Load checks** refuse as corruption:
  - a type or sequence record under another id's key, or a later format;
  - a missing, extra or mistyped name entry;
  - a type of a table's or view's name;
  - an ACL entry with a privilege its object's kind does not take.

## Views

A view is a stored query. The record is under `01 04 <id>`, its name entry
has kind 3, and the id comes from the one counter, so a view dropped and
created again is another object and nothing that named the first can mistake
it for the second.

```
u16 format                   ; kViewRecordFormat, 1
u32 id, str name
blob definition              ; the CREATE VIEW statement, as the client sent it
u16 ndeps { u32 id }         ; the tables and views the query names, each once
u16 ncols { str name }       ; the view's columns, as created
```

- **The text is the definition.** Nothing parsed or bound is stored: a
  parsed form would be a second format to version, and the binder already
  turns text into a statement. A view is read by parsing the text and
  binding it again (`frontend_design.md`, *The dialect*, *Views*), so a use
  sees the tables as they are then. `columns` is what the view returned
  when it was created, and a use whose columns differ is refused, so the
  catalog never gives a view columns it did not have.
- **`ndeps` makes `DROP` RESTRICT.** `DROP TABLE` and `DROP VIEW` look for a
  view whose list holds the object, and refuse it (2BP01). A view reads
  through the views it names, so the list holds what the query names
  directly, a subquery's in any position included, not what those read in
  turn. It is what binding the query resolved (*Views resolve against what
  they were created against*, rule 1), not a second walk of its text.
- **Format 1 is the first.** A build reads views of format 1 and refuses a
  later one, as it refuses a later table record. A build from before views
  refuses a database holding one: the view's name entry names no table or
  index, which that build refuses at open as corruption.
- **A record that does not parse is refused at load**, with the same
  checks as a table's: a truncated field, trailing bytes, a record under
  another id's key, an id used twice, a dependency that is neither a table
  nor a view, a name entry that disagrees.
- **Compatibility.** A plan names tables, never views, so the runtime's
  check (*Compatibility*) does not see one. The embedded API records the
  views each plan was expanded through and rebuilds it after any DDL
  (`frontend_design.md`, *Views*).

## Compatibility

`bc_schema_ref` lists what a plan depends on, and `bc_ctx_run` checks it
against the pinned version on every execution (`docs/plan_api_design.md`,
*Runtime*, states the cost). A plan is compatible when:

- **every table and index it names exists.** Ids are never reused, and a
  table's primary key and an index's parts never change, so existence is
  the same key layout;
- **every column it reads or sets exists in its table, with the type the
  plan was compiled with.** A column is named by its permanent id and the
  runtime finds its position, so neither a new column nor a new layout
  generation moves it; this fails only for a dropped column;
- **every table it writes exists.** The plan does not name the table's
  indexes: the runtime writes the entries of every index the pinned version
  has.

So adding a column leaves a plan compatible: no plan names the new column,
and rows that plans compiled before it write get its default from
`bc_row_begin`. Adding an index does too: a write plan compiled before it
maintains it. Dropping an index invalidates the plans that scan it.
Dropping a table invalidates every plan on it. An incompatible plan is
`BC_E_LAYOUT` and does not run.

Compatibility is what the runtime checks, for a DDL that races a run. It
does not decide when the embedded API binds a statement again: **a cached
plan bound at a catalog version behind the live one is bound again at its
next run, after any DDL**, GRANT and CREATE ROLE included
(docs/transaction_design.md, *Stages of transactional DDL*, stage 0). The
version comparison is the trigger, so a plan whose tables gained a column
binds again even though it stays compatible: `*`, `t.*` and NATURAL expand
again, USING resolves again, and a name a new column makes ambiguous is
42702, as PostgreSQL binds a statement against its latest catalog. An
`INSERT` without a column list fills the leading columns and gives the rest
their defaults, as PostgreSQL does, so it binds to the same plan after an
ADD COLUMN. A `*` over a view gives the view's recorded columns; a view
whose own `*` would now give other columns is refused when read (0A000),
where PostgreSQL keeps the columns of its CREATE VIEW.

The result type follows PostgreSQL 17. A statement run by text binds with
inferred parameter types and returns the live catalog's columns. A held
`Statement` keeps the parameter types and result columns it fixed: it binds
again with its own types, a type that no longer fits is the binder's error,
and other result columns are 0A000 "cached plan must not change result
type". A rebind whose parameter types and result columns are unchanged
swaps the new plan into the `Statement`; otherwise the new plan goes into a
new `Statement` that replaces it in the cache, the old one valid for its
holders until `close()`, and a run or a prepare of the text then uses the
new one. A caller's record (`ResultCheck`, the server's from Parse) binds
the same way against the record: the cache's plan when its parameter types
are the record's, else a plan for that run alone.

The cost is one compile per cached text after each DDL, of any object, and
not again until the next. Two runs pay per call instead: a held `Statement`
whose rebind keeps failing binds once per call until it is prepared again,
and a record whose types differ from the cache's binds a one-shot plan per
call. A replaced `Statement` stays in memory until `close()`; #549 bounds
them with the cache.

A transaction binds only against the catalog it pinned
(docs/transaction_design.md, *A transaction binds only against its own
catalog*): it runs the shared plan only when that plan was bound at exactly
its pin, and otherwise compiles the statement privately at the pin.

## DDL

A DDL statement is bound when it runs, against the catalog it runs in, not
when it is prepared (`frontend_design.md`, *Binding*). In autocommit that
is the live catalog, and the DDL commits at once; in a transaction it is
the transaction's catalog, and the DDL is a step of the transaction,
published at its commit (`transaction_design.md`, *Transactional DDL*). Every DDL statement
is one atomic batch: the table record, the name entries
and the counters, plus the rows a backfill writes or the ranges a drop
deletes. It is built on a snapshot and guarded by `ensure_unchanged` on the
catalog version, so a second writer of the catalog makes it a conflict
rather than a lost update. The in-memory version is published only after
the batch commits; a failure before or in the commit leaves the catalog, on
disk and in memory, as it was.

| Statement | Batch |
|---|---|
| `CREATE TABLE` | Record, name entry (`ensure_absent`), counters. Two ids: the table and its first layout. The bitmap is sized as the row format says. Without a primary key, the record holds the hidden row id (*Tables without a primary key*). Each `UNIQUE` constraint is a unique index in the same record and batch, with its own name entry (*UNIQUE constraints*). A table of no columns is refused (0A000); PostgreSQL takes one. |
| `CREATE TABLE AS`, `SELECT … INTO` | As `CREATE TABLE` of no primary key, plus every row of the query under row ids 1 to n (*CREATE TABLE AS*). |
| `CREATE [UNIQUE] INDEX … (col [DESC], …)` | Record with the index, name entry (`ensure_absent`), counters, an entry per existing row, `ensure_range_unchanged` over the table's rows. A unique index whose rows already hold a duplicate is refused. |
| `DROP TABLE` | Record and name entries deleted; `del_range` over `02 <table_id>` and over `03 <index_id>` per index. Refused (2BP01) while a view reads the table. |
| `DROP INDEX` | Record without the index, name entry deleted, `del_range` over `03 <index_id>`. |
| `CREATE VIEW` | View record, name entry (`ensure_absent`), counters. One id. |
| `DROP VIEW` | View record and name entry deleted. |
| `CREATE PROCEDURE` | Procedure record, procedure name entry (`ensure_absent`), counters. One id. `OR REPLACE` of a procedure of the same arguments puts its record again, under its id. |
| `DROP PROCEDURE` | Procedure record and procedure name entry deleted. |
| `ALTER TABLE … ADD COLUMN` | Record with the column appended at the next id and the next position of its region; the current generation's offsets do not move. With a default, every existing row rewritten with it, and `ensure_range_unchanged` over the rows. |

`IF EXISTS` on the drops and `IF NOT EXISTS` on `CREATE TABLE`; nothing
else. Either, when it applies, writes nothing and publishes no version:
`LiveCatalog::run` reports the DDL unchanged, and the API turns that into
PostgreSQL's notice (`Rows::notices`). `IF NOT EXISTS` is decided twice. The
binder, finding the name taken by a table, view or index in the catalog it
binds against, skips the definition unread, as PostgreSQL does, and the DDL
carries the name alone; the apply skips again when the name is taken then.
One the binder found taken and a DDL since has freed fails with 40001, as a
DDL that meets another does. A drop deletes in O(1) appends
whatever the table holds; the engine's `del_range` removes the keys from the
key directory as it applies. A `NOT NULL` column added without a default is
refused, as is a column whose id has no bit in the layout's bitmap (it needs
a `REBUILD`, which doesn't exist yet, #503). Ids taken by a DDL are not reused, even when
it is dropped later.

**Every catalog write bumps the version.** Every write to the catalog
namespace `01` goes through `Catalog::next_version`, and every
`next_version` that changes something guards (`ensure_unchanged`) and puts
the version key. A transaction's DDL relies on it: `bc_txn_ddl` asserts
both are in the batch it is given, then drops the name entries'
`ensure_absent` and the version guard, which a transaction that drops a
name and takes it again would fail at its commit, and the commit guards the
version key once instead. Any DDL that took a name, or changed a record,
since the transaction's snapshot moved the version, so that one guard
stands for them all. Autocommit DDL keeps its guards and does not rely on
it. The exceptions: the test seam `conflict_on_every_commit_for_testing`
rewrites table records unchanged with a plain put (test-only, and it
creates no name); and a sequence counter (`nextval`, not built) would write
namespace `01` without a DDL, and must then go around a transaction's
overlay and be named here.

**A version number names only a published catalog.** A transaction's DDL
makes a private catalog numbered its base plus one per DDL, and two open
transactions may each hold a different catalog of the same number. Only
`LiveCatalog` publishes one; a transaction's commit publishes its private
catalog through `LiveCatalog::commit`, whose version guard proved that none
was published since its base, so the number it publishes is above every one
published before. Versions are not contiguous. Every cache keyed by version
(the shared plan cache, the shared procedure map) is consulted with a
published catalog's version only: a transaction that has run DDL compiles
every statement privately.

**Backfill is one batch, bounded by the engine's 1 GiB plan limit.**
`CREATE INDEX` and `ADD COLUMN … DEFAULT` on a populated table read every row
from the snapshot and write in the same batch as the catalog record, so the
index or column exists with all its entries or not at all, with no state in
between for a plan to see. The range guard makes a write to the table that
commits during the backfill a conflict; the DDL is retried up to three
times on a fresh snapshot and then fails. `Catalog::next_version` reads
the rows through a `RowSource`, an ordered scan of one key prefix:
autocommit DDL gives it its snapshot (`SnapshotRows`), and a DDL in a
transaction gives it the transaction's view of its snapshot and its own
writes (`bc_txn_rows`; `transaction_design.md`, *Mechanism: the
transaction's private catalog*). A backfill counts the bytes it
gathers and is refused (`BackfillTooLarge`), with an error that says so, as
soon as they pass the 1 GiB limit, before it has read the rest of the table
into memory; in a transaction, as soon as they pass the transaction's
`max_bytes` if that is less (54000, "run the DDL outside a transaction"). The alternative, batches written
while the index is invisible to reads but maintained by writes, needs an
index state in the record ("write-only") and plans that respect it; it is
deferred (#505), and the schema is created before the data until then. The
TPROC-C schema is.

### CREATE TABLE AS

`CREATE TABLE … AS` and `SELECT … INTO` (`frontend_design.md`, *The
dialect*) run their query first, autocommit, on a snapshot of its own; the
API then hands the rows to the catalog in the `CreateTableDdl` (`rows`),
each value of its column's type, and the catalog writes the table's record,
its name entry and each row in one batch, row i under the hidden row id i.
So the table exists with all its rows or not at all: a reader, or a crash,
before the batch commits finds no table and no row (the test holds the
commit at `LiveCatalog`'s commit hook, where a reader sees no row key in the
engine, and throws there to stand for a crash, then reopens). The rows are the
query's snapshot: a write committed after the query ran and before the DDL
commits is not in them, as in any transaction whose writes (here, a new
table's keys alone) conflict with nothing. Once the DDL's next catalog is
built, `LiveCatalog::reserve_row_ids` moves the table's row id counter past
n, so its first `INSERT` draws n + 1 or later (*Tables without a primary
key*): seeding from the engine alone is not enough, since a transaction's
table is unpublished until its commit and a counter outlives a rolled-back
table of the same id.

In a transaction (`transaction_design.md`, stage 4) the query runs as a
statement of the transaction, through its view, and the table and its rows
are a DDL of it: they go into the transaction's overlay, bounded by its
`max_bytes` as a backfill is, and are published with its commit.

Two limits bound the rows, and the statement's memory budget comes first:
the query's result is gathered under `statement_memory_bytes` (64 MiB by
default), and a result past it is 54000 before any batch is built. The
rows written are charged to the backfill's 1 GiB limit, which a budget set
above it reaches; past it the statement is 54000 too (the API's
`ExecError` `BC_E_LIMIT`), with nothing written.

## Bootstrapping

An empty database has no `01 01` counters: the next id is 1, the version 0,
and there are no tables. The first DDL writes both counters in its batch,
its `ensure_unchanged` on the absent version key holding against a
concurrent first DDL. Nothing is written at open, so opening a database
read-only, or as a follower, does not change it. A database whose `0x01`
namespace holds a record or a name entry that does not parse is refused at
open.

## Several schema versions

Deferred.

Serving several schema versions at once (per-version column lists and
defaults, a session declaring the version it speaks, `DROP COLUMN` as a
compatible change, `REBUILD` as a new generation, retiring old versions) is
not built. Four choices made here keep it open:

1. **Permanent column ids, never reused**, in the record (`next_col_id`) and
   in the row's bitmap.
2. **The row header keeps the generation and the lengths**, so a row says
   which offset table decodes it, and the record keeps every generation's
   table.
3. **The per-table version counter** stays in the record, bumped by every
   DDL on the table, for a version to name.
4. **Append-only serialization**, so the record can grow per-version
   fields without migrating the records already written.

## Schemas

Status: **design, approved by the owner on 2026-11-20** (#713, option 1),
with every recommendation under *Open questions for the owner*: Q1 (a),
Q2 (a), Q3 (a), Q4 (a), Q5 (a), Q6 (a), Q7 (a), Q8 (a), Q9 (b). Built:
both fixes before stage 1 (qualified type names; a view's dependencies
from the binder's resolution), stage 1a (lookups keyed by (schema, name))
and stage 1b (the schema record, `CREATE SCHEMA` and `DROP SCHEMA` without
elements or `CASCADE`, qualified names; *Stages of schemas* says what was
adapted), stage 2 (`CREATE SCHEMA` with elements) and stage 3 (`WITH
CHECK OPTION`); stages 4 and 5 are not built. This section covers what
`CREATE SCHEMA` and `DROP SCHEMA` do, how a schema-qualified name resolves,
the schema record and the change to the name entries' keys, what an older
build does with a database that holds a schema, `WITH CHECK OPTION` (which
sqltest's F311-04 needs), and the stages that build it. Behaviour observed
on PostgreSQL 17.12 is cited as "PG 17". Line numbers are those of branch
`gustavoamigo/sql-txn-ddl-ctas-implicit`.

**The design in one paragraph.** A schema is a catalog record with an id
from the one counter, a name entry in a namespace of its own, and an ACL
record that exists as long as the schema does. Every name entry of a
relation, procedure or type outside `public` carries its schema's id in its
key; `public` is schema 0, implicit, never written, and holds every object
that exists today, so no existing key moves and nothing is migrated. Every
record this build writes is in a next format that names its schema; the
records written before it still read, as objects of `public`. A build from
before stage 1 refuses any database holding a record this build wrote, a
schema included, as `bytecaskdb-sql-layer/CONTRACT.md` already says of a
database a later build writes. A session's `search_path` is fixed at
`public`. A view stores, for each unqualified relation name of its query,
the schema that name resolved to when it was created, and resolves through
that map at every use, never through a path.

### What the sqltest cases need

The 8 F311 cases (`tests/functional/suites/sqltest/standards/2016/F/F311-0{1..5}.tests.yml`)
fail at the parser today and are listed in
`tests/functional/not_implemented/sqltest.txt:139-146`. sqltest has no
expected results: a case passes when every step succeeds
(`tests/functional/loaders/sqltest.py:5-7`).

| Case | Statement | Needs |
|---|---|---|
| f311_01_01_01 | `CREATE SCHEMA S` | stage 1b |
| f311_02_01_01 | `CREATE SCHEMA S CREATE TABLE T (A INT)` | stage 2 |
| f311_03_01_01 | `CREATE TABLE T1 (A INTEGER)`; `CREATE SCHEMA S CREATE VIEW V AS SELECT A FROM T1` | stage 2: `T1` resolves to `public` |
| f311_04_01_01..03 | as 03, with `WITH CASCADED CHECK OPTION`, `WITH CHECK OPTION`, `WITH LOCAL CHECK OPTION` | stages 2 and 3 |
| f311_05_01_01, 02 | `CREATE TABLE T`; `CREATE ROLE R`; `CREATE SCHEMA S GRANT SELECT ON [TABLE] T TO R` | stage 2 |

No case needs stage 4 (schema privileges) or stage 5 (`DROP SCHEMA …
CASCADE`); building them now is the owner's call (Q9).

### Schema semantics

**A schema is a namespace of relations, procedures and types.** Tables,
views, indexes and sequences share one namespace per schema, as relations
do in PostgreSQL; procedures have one per schema, and types (domains, shell
types, and the row type of each table and view) one per schema. Roles stay
global, as in PostgreSQL. Two objects of one name in two schemas are two
objects. Names fold case as other names do: `CREATE SCHEMA "DZ_Up"` then
`DROP SCHEMA dz_up` is 3F000 (PG 17).

**Schema 0, `public`.** Every object that exists today is in it, and an
unqualified `CREATE` puts the object there. It is implicit, as the
bootstrap role is: no record, no name entry.

**System schema names.** `pg_catalog` and `information_schema` exist in
PG 17; the layer holds no object in either.
- `CREATE SCHEMA [IF NOT EXISTS] name` checks, in PG 17's order: a name
  starting `pg_` is 42939 "unacceptable schema name "pg_x"", DETAIL "The
  prefix "pg_" is reserved for system schemas.", with or without `IF NOT
  EXISTS` (`pg_catalog` included); then `public` and `information_schema`
  are taken, though no name entry holds them: 42P06, or with `IF NOT
  EXISTS` the NOTICE below and nothing written; then the name entry (PG 17,
  each).
- `DROP SCHEMA pg_catalog`, `IF EXISTS` included, is 2BP01 "cannot drop
  schema pg_catalog because it is required by the database system"
  (PG 17). `DROP SCHEMA information_schema` is 0A000: PG 17's is 2BP01
  listing its ~100 objects, which the layer does not hold (*Differences*).
- Creating in a system schema: a table, view, index or sequence in
  `pg_catalog` is 42501 `permission denied to create "pg_catalog.x"`,
  DETAIL "System catalog modifications are currently disallowed." (PG 17).
  A domain, type or procedure in `pg_catalog`, and any object in
  `information_schema` or `pg_temp`, is 0A000: PG 17 creates them (as
  superuser; `pg_temp.x` a temporary table), the layer has no such
  schema to write in (*Differences*).

**`CREATE SCHEMA [IF NOT EXISTS] name [AUTHORIZATION role] [element …]`**
and `CREATE SCHEMA AUTHORIZATION role [element …]`, which names the schema
after the role (PG 17).
- A taken name is 42P06 `schema "s" already exists`. `IF NOT EXISTS` on a
  taken name writes nothing and gives NOTICE 42P06 `schema "s" already
  exists, skipping`, decided twice as `CREATE TABLE IF NOT EXISTS` is
  (*DDL*).
- `IF NOT EXISTS` with elements is 0A000 "CREATE SCHEMA IF NOT EXISTS
  cannot include schema elements" (PG 17 raises it in its grammar; the
  parser maps libpg_query's message to 0A000, not 42601).
- `AUTHORIZATION role`: the role must exist (42704, PG 17);
  `CURRENT_USER`, `CURRENT_ROLE` and `SESSION_USER` name the bootstrap role.
  The schema records the role as its owner (*Schema owner and
  privileges*).
- The catalog itself refuses an empty name or one holding a NUL byte, for
  a schema and for every other object, in each DDL's `next_version` and at
  decode (*Schema catalog record and name entry keys*).

**Schema elements.** PG 17's grammar takes `CREATE TABLE`, `CREATE VIEW`,
`CREATE INDEX`, `CREATE SEQUENCE`, `CREATE TRIGGER` and `GRANT` as elements;
`CREATE FUNCTION`, `CREATE DOMAIN`, `CREATE TYPE`, `CREATE TABLE … AS` and
`REVOKE` are 42601 there, and so here, since the layer's parser is PG's.
`CREATE TEMP TABLE` as an element is 42P16 "cannot create temporary
relation in non-temporary schema" (PG 17). `CREATE TRIGGER` is 0A000: the
layer has no triggers. The rest follow PG 17:
- **Order.** Elements run grouped by kind, sequences, then tables, then
  views, then indexes, then grants, each group in written order (PG 17's
  `transformCreateSchemaStmtElements`; observed: an index written before
  its table works, a view over a later-written view is 42P01).
- **Where each element's object goes.** An unqualified `CREATE TABLE`,
  `VIEW`, `SEQUENCE` or index's table is in the new schema; one qualified
  with the new schema is accepted; one qualified with another is 42P15
  "CREATE specifies a schema (a) different from the one being created
  (b)" (PG 17). An index is in its table's schema, so `CREATE INDEX i ON t`
  as an element indexes `s.t`, never `public.t` (PG 17: with `public.dz_b`
  present, `CREATE SCHEMA dz_e1 CREATE INDEX dz_i ON dz_b(a) CREATE TABLE
  dz_b(a int)` put `dz_i` on `dz_e1.dz_b`).
- **What an element reads.** Names an element reads (a view's query, a
  `GRANT`'s objects) resolve with the path (the new schema, `public`): the
  first schema holding a relation of the name, of any kind, wins, else
  42P01 (PG 17; *The path*).
- **One statement.** The schema and every element commit together or not
  at all; a failing element writes nothing, schema included (PG 17).

**`DROP SCHEMA [IF EXISTS] name [, …] [RESTRICT | CASCADE]`**
- A missing schema is 3F000 `schema "s" does not exist`, and the statement
  drops nothing even when other names exist; with `IF EXISTS`, NOTICE 00000
  `schema "s" does not exist, skipping` per missing name (PG 17).
- **RESTRICT**, the default: a schema that holds any object is 2BP01
  "cannot drop schema s because other objects depend on it" (with more
  than one schema named, "cannot drop desired object(s) because other
  objects depend on them"), HINT "Use DROP ... CASCADE to drop the
  dependent objects too." (PG 17). Its DETAIL, as PG 17 writes it:
  - one line per table, view, sequence, domain, shell type and procedure
    of the schema: `table s.t depends on schema s`; a domain and a shell
    type are both `type s.d`, a procedure `function s.p(integer)` with its
    argument types;
  - no line for an index, a row type, a constraint, or a sequence a column
    owns;
  - a view in another schema that reads one of them is listed as
    depending on what it reads, not on the schema: `view v depends on view
    s.v` (or `on table s.t`);
  - a name is unqualified when its schema is on the session's path
    (`view v`, not `view public.v`);
  - past 100 lines, the rest is one line, `and N other objects (see
    server log for list)`.
- **CASCADE** (stage 5) drops, in one batch: every table, view, index,
  sequence, procedure, domain and shell type of the schema; every view in
  any schema that reads one of them, and the views that read those, to the
  end of the chain; and the ACL record of each, the schema's included.
  NOTICE "drop cascades to N other objects" (or "drop cascades to table
  s.t" for one), DETAIL one `drop cascades to …` line per object the
  RESTRICT form lists, with the same naming and the same 100-line cap
  (PG 17: `DROP SCHEMA dz_r CASCADE` dropped `dz_r.x` and the view
  `public.dz_rv` over it). A procedure elsewhere whose body names a dropped
  table is not dropped: its body is not a dependency, as a PL/pgSQL body
  is not in PG 17 (`procedures_design.md`, *Catalog record*).
- **In use.** In a transaction, a `DROP SCHEMA` that would drop a table
  or index an open cursor or bound portal of the transaction reads is
  55006 `cannot DROP TABLE "t" because it is being used by active queries
  in this session`, as `Transaction::check_in_use`
  (`api/database.cppm:3315-3360`) gives for `DROP TABLE`. RESTRICT reports
  2BP01 first, since a schema holding the table is not empty; CASCADE
  checks each table and index of its closure (PG 17: a cursor over
  `dz_cu.t`, then `DROP SCHEMA dz_cu CASCADE`, gave its NOTICE, then this
  55006; without CASCADE, 2BP01).
- `DROP SCHEMA public` is 0A000 (*Differences from PostgreSQL 17*).

**`CURRENT_SCHEMA`** and `current_schema()` return `public`, the first
schema of the path (PR 28 builds it; if this lands first, stage 1b does).
`current_schemas(false)` is `{public}` and `current_schemas(true)`
`{pg_catalog,public}` (PG 17).

### Name resolution with schemas

**Qualified names are accepted everywhere a name is resolved**: a `FROM`
relation, `INSERT INTO`, `UPDATE`, `DELETE`, `CREATE TABLE`, `CREATE VIEW`,
`CREATE SEQUENCE`, `CREATE DOMAIN`, `CREATE TYPE`, `CREATE PROCEDURE`,
`CALL`, `ALTER TABLE`, `CREATE INDEX … ON s.t`, every `DROP`, `GRANT` and
`REVOKE`, a column's type, and a column reference `s.t.c`. Each refusal
the parser has today (`sql/parser.cppm:86`, `:840`, `:865`, `:910`,
`:919`, `:924`, `:934`, `:947`, `:1000`, `:1667`, `:1908`, `:185`) goes.
An index's own name in `CREATE INDEX` takes no qualifier: 42601, as in
PG 17. A function name keeps today's rule: `pg_catalog.` only. A name of
four parts or more is 42601 "improper qualified name (too many dotted
names): a.b.c.d" (PG 17).

**A type name is resolved whole.** The parser kept a type name's last
part, so `CREATE TABLE t (a foo.int4)` was accepted as `int4` where PG 17
gives 3F000. That bug is fixed on its own before stage 1 (*Stages*, built):
a qualifier other than `pg_catalog.` is refused (`check_type_schema` in
`sql/parser.cppm`; `frontend_design.md`, *Type names*). Stage 1b then widens it: `s.d` names the domain or shell type `d`
of `s` (which is not a column's type yet, *Domains, shell types and
sequences*). `pg_catalog.int4` and the other built-in types stay accepted;
`public.int4` is 42704 `type "public.int4" does not exist` (PG 17).

**The path.** An unqualified name resolves against a path of schemas. The
first schema that holds a relation of the name, of any kind (table, view,
index, sequence), wins, and the caller then checks the kind (42809, as
today), as PG 17's `RangeVarGetRelid` does; a lookup never skips an index
or sequence `s.x` to reach `public.x`.

| Where | Path |
|---|---|
| A session, embedded or wire | `public` |
| An element of `CREATE SCHEMA s` | `s`, `public` |
| A view's query, at every use | none: its stored map (below) |
| A procedure's body, at each call | the caller's, as PG 17 resolves a PL/pgSQL body's names when it runs |

Built-in functions and types are `pg_catalog`'s and are found before the
path, as in PG 17. The layer has no relation in `pg_catalog`, so the path
holds every relation it can name.

**Errors, as PG 17 gives them** (each observed). A name in a message is
written as PG 17 writes it: some forms qualify it, others do not.

| Statement | Missing schema | Missing object in an existing schema |
|---|---|---|
| `SELECT … FROM s.t`, `INSERT INTO`, `UPDATE`, `DELETE` `s.t` | 42P01 `relation "s.t" does not exist` | 42P01 `relation "s.t" does not exist` |
| `CREATE TABLE`, `VIEW`, `SEQUENCE`, `DOMAIN`, `TYPE`, `PROCEDURE` `s.x` | 3F000 `schema "s" does not exist` | (none) |
| `DROP TABLE`, `VIEW`, `SEQUENCE` `s.x` | 3F000 | 42P01 `table "x" does not exist` (`view "x"`, `sequence "x"`), unqualified; `DROP TABLE s.t, s.x` names `x` alone |
| `DROP INDEX s.x` | 3F000 | 42704 `index "x" does not exist` |
| `DROP TYPE`, `DROP DOMAIN` `s.x` | 3F000 | 42704 `type "s.x" does not exist` |
| `DROP PROCEDURE s.p()` | 3F000 | 42883 `procedure s.p() does not exist` |
| `CALL s.p()` | 3F000 | 42883 `procedure s.p() does not exist`, HINT "No procedure matches the given name and argument types. You might need to add explicit type casts." |
| `ALTER TABLE s.x`, `CREATE INDEX ON s.x`, `GRANT … ON s.x` | 3F000 | 42P01 `relation "s.x" does not exist` |
| A column type or a cast `s.d` (`1::s.d`) | 3F000 | 42704 `type "s.d" does not exist` |
| `DROP … IF EXISTS s.x` | NOTICE `schema "s" does not exist, skipping` | its notice, as today (`table "x" does not exist, skipping`) |
| `db.s.t`, a relation | 0A000 `cross-database references are not implemented: "db.s.t"` | |
| `db.s.t.c`, a column | 0A000 `cross-database references are not implemented: db.s.t.c` | |

**`FROM` and column references.** Two relations of one name in two schemas
may appear in one `FROM` unaliased (`FROM s.t, public.t`); `t.c` over them
is 42P09 `table reference "t" is ambiguous`, and `s.t.c` or `public.t.c`
picks one. The same name twice from one schema stays 42712. A qualified
column reference that matches no entry is 42P01 in PG 17's three forms:
- `x.t.c` with no relation `x.t` in `FROM`: "missing FROM-clause entry for
  table "t"";
- the relation is in `FROM` under an alias: "invalid reference to
  FROM-clause entry for table "t"", HINT "Perhaps you meant to reference
  the table alias "x".";
- `public.t.c` when `FROM` holds only `s.t`: the same message, DETAIL
  "There is an entry for table "t", but it cannot be referenced from this
  part of the query.".

`UPDATE s.t SET s.t.a = …` is 42703 `column "s" of relation "t" does not
exist` (PG 17).

What changes in the parser and binder for this:
- `ast::FromItem` carries its schema (a `QualifiedName`, *Stage 1a*).
- `resolve_aliases`' duplicate check (`parser.cppm:1406-1450`) keys an
  unaliased item on (schema, name), so `FROM s.t, public.t` passes where
  today it is 42712.
- Alias erasure stays suppressed whenever two items share a relation name
  in any schema: today's `names.tables[i] == names.tables[j]` compares bare
  names, and stays bare.
- A qualifier `requalify` rewrites carries the schema, so two `t` items
  from two schemas never collapse onto one name. Test: `FROM s.t x,
  public.t y WHERE x.c = y.c`.
- The binder's range-entry key for an unaliased relation becomes its
  (schema, name).

**Views resolve against what they were created against.** A view is stored
as text and bound again at every use (*Views*), so with a path of more than
one schema a later object could change what an unqualified name means: a
view created as an element of `CREATE SCHEMA s` over `public.t1` would read
`s.t1` once one is created. PG 17 binds the view once and keeps the object.
The layer keeps it too:
1. **The binder records what it resolved.** While it binds `CREATE VIEW`,
   the binder records each relation it resolves, in every position of the
   query (subqueries in targets, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`,
   CTE bodies), as it resolves it. `depends_on` and the map below are both
   built from that one record, not from a second walk of the AST.
2. **The view stores a map** from each unqualified relation name its query
   reads (a CTE's name excluded) to the schema that name resolved to:
   `u16 nnames { u8 kind, str name, u32 schema }` in its record (format 2,
   below), kind 1 for a relation. A qualified name needs no entry.
3. **At each use an unqualified relation name resolves through the map,
   never through a path.** A name the map lacks is an internal error
   (XX000, "view v has no stored resolution for t"), never a search: it
   can only arise from a damaged record or a binder bug, and an error is
   preferred to a guess.

The map gives the object of the creation: the entry names the schema the
name resolved to then, and that schema's object is still there, since it
is in `depends_on` and a view's dependencies cannot be dropped (RESTRICT;
CASCADE drops the view with them). Load checks that each entry's object is
in `depends_on`. A view written before stage 1 (format 1) has no map: its
names resolve in `public`, the only schema its path held.

**Rule 1 is built** (before stage 1, *Stages*; #870). `depends_on` came
from `ViewExpander::relations`, a second walk that resolved names with
`cat_.table(name)` on its own and, through `each_subquery`, missed a
subquery in `ORDER BY`: `CREATE VIEW v AS SELECT a FROM t ORDER BY (SELECT
max(b) FROM u)` let `DROP TABLE u` succeed, and `v` then failed. Now it is
the record of what binding the view's query resolved, in two parts, since
views are expanded before the binder runs:
- **Tables: the binder.** `Binder::resolved_relations()` lists each table
  the binder resolved by name in a `FROM`, in every position of the query
  (a subquery's in a select list, `WHERE`, `ORDER BY`, an arm of a set
  operation). A `FROM` item a view's body brought in by its merge is the
  view's read, not the query's: the expander marks each such item
  `ast::FromItem::of_view` (the body's subqueries' and a grouped view's
  derived query's too, a subquery shared by two places kept one), and the
  binder does not record it.
- **Views: the expander.** A view is resolved where the expander expands
  it; it records the views it expands in the query being created itself
  (depth 0), not those it expands inside another view's body.

`each_subquery` now also visits `ORDER BY`, so a view read by a subquery in
`ORDER BY` is expanded where it was passed to the binder unexpanded and
refused. `GROUP BY` and `HAVING` take no subquery yet and the dialect has
no `WITH` query (`frontend_design.md`, *Subqueries*: refused, 42601), so a view of one is refused at
`CREATE VIEW`: no view is created without what it reads. When those are
built, the binder resolves their relations as any other's and they enter
the record with no change here.

Only relations enter the map today. Views cannot name a sequence or a
domain yet (no `nextval`, and a domain is not a usable column type,
`frontend_design.md:1004-1007`); when either is built, a view's reference
to a type or sequence must enter the map under its own kind, and a
column default's too (*Risks*).

### Schema catalog record and name entry keys

New keys under namespace `0x01` (*Storage*); `0D` and `0E` are free today:

| Key | Value |
|---|---|
| `01 0D <id:4>` | The schema record. |
| `01 0E <name>` | Schema name entry: kind 8 and id (4, big-endian). Schemas have a namespace of their own. |
| `01 09 <id:4>` | The schema's ACL record, written with the schema and deleted with it (*Schema owner and privileges*). |

**The schema record**, little-endian as the others:

```
u16 format                   ; kSchemaRecordFormat, 1
u32 id, str name
u32 owner                    ; a role id, or kBootstrapRole
```

Its id comes from the one counter, so a schema dropped and created again is
another schema. `public` has no record and is id 0, which the counter never
gives.

**A name entry outside `public` carries its schema.** A name entry key is
today the sub-prefix and the name's bytes, with no terminator
(`name_entry_key`, `catalog.cppm:1649`). Outside `public` it gains a
marker byte `00` and the schema id:

```
01 03 <name>                 ; a relation in public: unchanged
01 03 00 <schema:4> <name>   ; a relation in schema `schema` (big-endian)
01 06 [00 <schema:4>] <name> ; a procedure
01 0B [00 <schema:4>] <name> ; a type
01 08 <name>                 ; a role: unchanged, roles are global
```

The two forms cannot collide because no name is empty or holds a NUL
byte, and from stage 1b the catalog itself enforces that: every DDL's
`next_version` refuses such a name (42602 `invalid name`, reachable only
through the `Catalog` API, since libpg_query's input is a C string and its
grammar refuses a zero-length identifier), and load refuses one in a record
or a name entry as corruption. Today nothing in the catalog checks it:
`name_entry_key` copies bytes raw and record decode takes any bytes, and
`utf8_code_points` (`catalog.cppm:206`) checks TEXT and VARCHAR values,
never a name. Every existing key is the first form, so no key moves and
nothing is migrated. An index's entry is under its table's schema.

**Every record this build writes names its schema.** Each record kind gets
a next format that appends its fields; this build writes only that format,
for an object in `public` too, and reads the earlier one as an object of
`public`:

| Record | Read | Written |
|---|---|---|
| Table (`01 02`) | 7, 8 | 8: format 7 and `u32 schema`. The row and key formats stay format 7's. |
| View (`01 04`) | 1, 2 | 2: format 1, `u32 schema`, `u8 check_option` (0 none, 1 local, 2 cascaded), `u16 nnames { u8 kind, str name, u32 schema }`. |
| Procedure (`01 05`), type (`01 0A`), sequence (`01 0C`) | 1, 2 | 2: format 1 and `u32 schema`. |
| ACL (`01 09`) | 1, 2 | 2: format 1 with `u8 is_default` after the object id; `is_default` 1 holds no entry and means the owner's default (PG's null `nspacl`), and only a schema's record may hold no entry. |

A record changes format only when a DDL writes it (an `ALTER TABLE`
rewrites its table's record); nothing is rewritten at open. A later format
of any of these kinds extends the one above: the schema is always there,
so no kind needs two variants again. View format 2 is defined once, in
stage 1b, with every field: `check_option` other than 0 was refused at load
until stage 3 built it; from stage 3 a value above 2 is.

So the name entry and the record each name the schema, as each names the
object's name today, and load checks that they agree. The in-memory
lookups (`names_`, `procedure_names_`, `type_names_`, `catalog.cppm:744`,
`:2368`) are keyed by (schema id, name), built from the records as
`rebuild_lookups` builds them today; a record decodes alone, so a
round-trip test needs no name entry.

**Load checks** refuse as corruption, as for the other records:
- a schema record under another id's key, cut short, with trailing bytes,
  or of a later format;
- a schema without its name entry or its ACL record, a name entry without
  its schema, two schemas of one name, a schema named `public` or
  `information_schema` or starting `pg_`, or an owner that is no role;
- a schema ID that another object also has: schema ids join the id set
  `use()` checks for uniqueness;
- an empty name, or one holding a NUL byte, in any record or name entry;
- a name entry with the `00` marker shorter than 5 bytes, of schema 0, or
  of a schema that does not exist;
- a record whose schema disagrees with its name entry's, or names a
  missing schema; a format-7 (or format-1) record whose name entry carries
  a schema; an index whose name entry is not in its table's schema;
- a view whose `check_option` is above 2; whose map names a schema twice
  for one name, a missing schema, or an object not in its `depends_on`;
- an ACL record of format 1 for a schema, of format 2 with `is_default`
  set and entries, with no entry for any object but a schema, or with a
  privilege its object's kind does not take.

### How an older build reacts to a database with schemas

`bytecaskdb-sql-layer/CONTRACT.md`'s row on earlier builds ("`Database::open`
of a database written by an SQL-layer build from before #780 …") already
says "A database this build writes cannot be opened by a build from before
it". This design keeps that and nothing more:
- **A record this build wrote is refused** by a build from before stage
  1b: its format is later ("written by a later SQL-layer build",
  `catalog.cppm:1787` and the parsers after it).
- **A database holding any schema is refused**, an empty one included:
  every schema has an ACL record, of format 2, which `parse_acl` refuses
  the same way (`catalog.cppm:1931-1937`). So an older build never sees a
  database where a schema exists and it can act without knowing it (a
  `DROP ROLE` of the schema's owner, which its 2BP01 check,
  `catalog.cppm:2915-2920`, knows only through ACLs, and which would leave
  this build refusing the database for an owner that is no role).
- **A database whose every record predates stage 1b opens** in the older
  build as before: this build moves no key and rewrites nothing it does
  not change.

### Schema owner and privileges

What #653's first step stores; nothing is enforced (#653).
- **Owner.** The record holds the owner: the `AUTHORIZATION` role, or
  `kBootstrapRole`. Objects have no owner in the layer, so one created in a
  schema `AUTHORIZATION r` has none, where PG 17 makes `r` its owner.
  `DROP ROLE` of a schema's owner is 2BP01 "role "r" cannot be dropped
  because some objects depend on it", DETAIL `owner of schema s` (PG 17).
- **The schema's ACL record** is written by `CREATE SCHEMA` with
  `is_default` set and no entry (PG's null `nspacl`, the owner's `USAGE`
  and `CREATE`), and deleted only by `DROP SCHEMA`. The first `GRANT` or
  `REVOKE` on the schema writes the default out (`r=UC/r`) and applies
  itself, as PG 17's `aclchk` does; a `REVOKE` that empties it keeps the
  record with no entry (PG 17: `REVOKE ALL ON SCHEMA dz_o FROM dz_r` left
  `{postgres=U/dz_r}`, no entry of the owner). An owner's own entries give
  `DROP ROLE` no `privileges for schema s` line; another grantee's do, as
  today for tables.
- **`GRANT` and `REVOKE` `{USAGE | CREATE | ALL} ON SCHEMA s`** (stage 4)
  edit that record, the column `kWholeObject`, as a sequence's are.
  `USAGE` is today's bit 256; `CREATE` is a new bit, 512. Another privilege
  is 0LP01 "invalid privilege type SELECT for schema"; a missing schema
  3F000 (PG 17). A schema takes no column privileges. On `public` both are
  0A000 (*Differences*).
- **`GRANT … ON ALL TABLES IN SCHEMA s [, …]`** (stage 4) grants on every
  table and view of each schema named at the time, as one `GrantDdl` of
  those objects (PG 17 accepts it; it writes no default for later tables,
  and covers no sequence). `IN SCHEMA public` is accepted; only `ON SCHEMA
  public` is 0A000. `ON ALL SEQUENCES`, `ALL FUNCTIONS`, `ALL PROCEDURES`
  and `ALL ROUTINES IN SCHEMA` stay 0A000.
- **`DROP SCHEMA`** deletes the schema's ACL record with it, and CASCADE
  each dropped object's.
- `GrantDdl::Target` (`catalog.cppm:595`) gains `Schema`.

### Schema DDL batches and transactional DDL

Each statement goes through `Catalog::next_version`, so it bumps the
catalog version, guards it, and every plan binds again after it, as after
any DDL (*Compatibility*). There is no new durability site: each is one
`WritePlan` and one `apply_batch` with `sync=true`
(`transaction_design.md`, *Crash atomicity*).

| Statement | Batch |
|---|---|
| `CREATE SCHEMA s` | Schema record, schema name entry (`ensure_absent`), its ACL record, counters. One id. |
| `CREATE SCHEMA s` with elements | The above and each element's batch, as one transaction's commit (below). |
| `DROP SCHEMA s` | Schema record, name entry and ACL record deleted. |
| `DROP SCHEMA s CASCADE` | The above, and each dropped object's batch as its own `DROP` writes it (`del_range` per table and index). |

**Transactional DDL applies unchanged.** `CREATE SCHEMA` and `DROP SCHEMA`
are DDL of the `Ddl` variant (`catalog.cppm:608`) like the others: in a
transaction each is a step bound against the transaction's catalog (rule
A), later statements see it, and the commit publishes it with the
transaction's writes or nothing. A skipped `IF [NOT] EXISTS` is not a DDL
of the transaction. `Transaction::check_in_use` gains `DropSchemaDdl`
(*DROP SCHEMA*, "In use"). Two transactions that each create `s` conflict
at the second commit (40001), as any two DDL commits do: a DDL never waits
for another (`transaction_design.md:477-502`), where PG 17 waits and then
raises 23505 or 42P06 (*Differences*).

**`CREATE SCHEMA` with elements is one transaction.** Each element must be
bound against the catalog the previous one made (a view over a table
created in the same statement), so the statement cannot be bound up front
against the live catalog, as `execute_ddl` binds a DDL today
(`api/database.cppm:2427-2437`). The API runs it as the steps of a
transaction: in a transaction, as steps of that one (`step_ddl`,
`:3382-3460`); in autocommit, as steps of an internal embedded
`Transaction` that it commits at the end, with the default
`TransactionOptions::max_bytes` (64 MiB), which the catalog's estimate of
what the elements add counts against (54000 past it, nothing written). The
first step creates the schema, then each element in PG 17's order, each
bound with the path (`s`, `public`). Transactional DDL already composes
several `step_ddl` calls into one commit, so this adds no mechanism.

It is not retried. Autocommit DDL is not retried today: `Database::run_ddl`
turns `DdlInProgress` into 40001 at once (`api/database.cppm:2437-2447`),
and the CONTRACT row "A DDL never waits for another …" says the caller may
retry. A `CREATE SCHEMA` with elements whose internal transaction loses at
commit to another DDL is 40001 the same way, with nothing written; the
caller reruns it. (The one internal retry loop that reruns from the start,
`Database::call`, `:2989-3006`, is for procedures and stays theirs.)

**A view created as an element stores its own text.** A view's
`definition` is the whole statement (`parser.cppm:1968`), which the binder
parses again at two sites and requires to be a `CREATE VIEW`
(`into_base_table`, `binder.cppm:5635-5636`, and `binder.cppm:5840`). An
element has no statement of its own, so the parser cuts one: the element's
byte range of the client's text, from its first token to the token before
the next element's first token (or the statement's end), found with
`pg_query_scan` (already used, `parser.cppm:1827`). The slice is kept only
if it parses alone to a single `CREATE VIEW` whose tree equals the
element's, locations aside; otherwise the statement is 0A000 "CREATE VIEW
as a schema element whose text cannot be cut out". The stored definition is
then an ordinary `CREATE VIEW`: both parse sites work unchanged, the text is
the client's (nothing is deparsed), each view stores only its own text, and
no view depends on its sibling elements' syntax. A routing helper is
therefore not needed; a test covers `INSERT` through an element view.

### WITH CHECK OPTION

F311-04 creates views `WITH [CASCADED | LOCAL] CHECK OPTION`, refused today
(42601, `parser.cppm:1146`). Built here because the cases need it, and
minimally: `INSERT` is the only write through a view the layer has
(`UPDATE` and `DELETE` of a view are 0A000, #678).
- **Stored** as the view record's `check_option` (format 2). `WITH CHECK
  OPTION` alone is `CASCADED`, as in PG 17. The other ways PG 17 sets it,
  `CREATE VIEW v WITH (check_option=local)`, `ALTER VIEW v SET
  (check_option=…)` and `CREATE OR REPLACE VIEW` (which can drop it), stay
  42601 as view options and `OR REPLACE` are today
  (`frontend_design.md:862`), the element forms included (*Differences*).
- **Created only on a view `INSERT` could write through.** The layer's
  rule for that (`ViewExpander::into_base_table`, `binder.cppm:5611`:
  one table or view, no `DISTINCT`, `GROUP BY`, `HAVING`, set operation,
  `LIMIT`, `OFFSET` or aggregate, down to the table) is checked at
  `CREATE VIEW`, through the same predicate (`not_updatable`), so a view
  is never created that `INSERT` then refuses. `CREATE VIEW` also requires
  at least one updatable column, as PG 17 does when it checks a check
  option: a view whose columns are all expressions is refused, one with
  some computed columns is accepted (PG 17). A refusal is 0A000 "WITH
  CHECK OPTION is supported only on automatically updatable views", HINT
  the reason for the first view of the chain that fails ("Views containing
  DISTINCT are not automatically updatable.", "Views that have no
  updatable columns are not automatically updatable.", PG 17). The reason
  is the HINT here, where `INSERT`'s 55000 puts it in DETAIL and gives the
  trigger HINT (`with_notes(why, "To enable inserting…")`), as PG 17 does.
- **A subquery in a checked `WHERE` is 0A000 at `CREATE VIEW`**, for the
  reason a `CHECK` constraint's is (*CHECK constraints*). It covers every
  `WHERE` the new view would check: its own, and under `CASCADED` those of
  the views below it. PG 17 accepts and enforces it (*Differences*).
- **Which views are checked.** Walking from the view written down to the
  table, a view's `WHERE` is checked if it has a check option or a view
  above it is `CASCADED`; `CASCADED` makes every view below it checked.
  So `LOCAL` checks its own `WHERE` and those below that have their own
  option, and a view with no option over one that has one still enforces
  that one (PG 17: `LOCAL` over a view with no option passed a row its
  `WHERE` refuses; a view with no option over `dz_cv1` gave 44000 naming
  `dz_cv1`).
- **Evaluated after the row is written, before it is returned.** The
  binder maps each checked `WHERE` to the table's columns through the
  views, as it maps the inserted columns, and the generated plan evaluates
  them over the row after `bc_insert`, innermost view first, and before
  the row's `RETURNING` output is emitted, so no row outside the view is
  streamed ahead of the error. A result that is false or NULL raises a new
  constraint code, `BC_C_CHECK_OPTION`, with the view's id; the API
  reports 44000 `new row violates check option for view "v"`, DETAIL
  `Failing row contains (…)`: the whole base-table row in the table's
  column order, defaults filled and NULL written `null`, the hidden row id
  of a table without a primary key left out (PG 17 gave `(-7, null, 0)`
  for a view showing two of three columns). After `bc_insert`, because
  PG 17 checks after the row and its index entries: a row that is both a
  duplicate and outside the view is 23505 (PG 17), and a `CHECK`
  constraint (23514) and `NOT NULL` (23502) come first too. The
  statement's error undoes its writes, as any error does. Foreign keys are
  not enforced yet; when #657 enforces them, 44000 must still come before
  23503 (PG 17: a row both outside the view and violating a foreign key
  gave 44000).
- **Its cost** is one evaluation of each checked `WHERE` per row written
  through the view.
- #678 must apply the same checks when it builds `UPDATE` of a view (PG 17
  checks an `UPDATE` that moves a row out).

As built (stage 3), with what the code made different:
- **The chain is walked once, by `ViewExpander`.** `into_base_table`
  keeps each view's definition as a `Level` (its query, the relation's
  columns as named and as aliased, its select list), and
  `checked_wheres` maps each checked conjunct to the table's columns
  through the levels below, into `ast::Insert::view_checks`, innermost
  view first. `CREATE VIEW` runs the same walk (`creation_checks`) over
  the new view and binds the result (`Binder::view_check`) and drops it,
  so a `WHERE` an `INSERT` could not check is refused when the view is
  created. Refusals at `CREATE VIEW` come after the query binds, as PG
  17's: its shape (`not_updatable`, top first), then its columns, then
  each view below, then the checked `WHERE`s.
- **A checked conjunct is a value, as a `CHECK` constraint's.** A
  `WHERE` conjunct is turned into an expression (`checked_expr`) and
  bound as one, which the plan evaluates over the row with the
  expression evaluator. `LIKE` has no such form yet: a checked `WHERE`
  holding one is 0A000 at `CREATE VIEW`, where PG 17 accepts it
  (*Differences*), as is a subquery (`EXISTS`, `IN (SELECT …)` or a
  scalar one). So is anything else a value cannot evaluate yet: a
  comparison of a `REAL` or `DOUBLE PRECISION` is 42601 at `CREATE VIEW`
  (*Differences*), though the same `WHERE` without a check option reads.
  A `NOT` (and so `<>`, `IS NOT NULL`, `NOT IN` and `NOT BETWEEN`, which
  the parser writes with one) keeps its operand in `args`, as the
  parser's does; a `NULL` conjunct (`WHERE NULL`, `WHERE b > 0 AND NULL`)
  is bound as a `BOOLEAN` NULL, so every row fails it with 44000, as in
  PG 17.
- **The DETAIL travels with the error.** The plan raises
  `bc_raise_row(cx, BC_C_CHECK_OPTION, view, table, row)` (plan API
  revision 19): the runtime writes PG 17's message and the failing row
  as the error's note (`bc_error_note`), and the API carries it as
  `ExecError::note`, which `error_detail` returns and the server sends
  as DETAIL. The row's values are their text output (`to_text`), NULL
  `null`, each cut to at most 64 bytes before a UTF-8 sequence the cut
  would split and followed by `...` where cut, and the DETAIL ends with a
  period, as PG 17's `ExecBuildSlotValueDescription` writes it (65 `x`
  gave 64 and `...`; 33 `é` gave 32 and `...`).
- **The view below a check option must itself take `INSERT`**, as the
  design says; PG 17 creates such a view (a check option over a
  `DISTINCT` view) and refuses only the `INSERT`. Listed in *Differences*.

### Differences from PostgreSQL 17

Each was observed on PG 17.12 unless it says otherwise.

| PG 17 | The layer | Why |
|---|---|---|
| `search_path` is `"$user", public`, settable with `SET`, shown by `SHOW` | Fixed `public`; `SET search_path`, `SHOW` and `RESET` stay 0A000 (`server/pg_session.cppm:401-428`) | Q1. A path per session would key the shared plan cache by path. `$user` resolves to nothing while no schema has the session user's name. |
| `DROP SCHEMA public` drops it when empty, or with CASCADE (PG 17's documentation; not run here, to leave the server's `public` alone) | 0A000 | Q2. `public` is implicit; dropping it needs state that says so. |
| `DROP SCHEMA information_schema` is 2BP01, DETAIL ~100 objects | 0A000 | The layer holds no object there to list. |
| `GRANT`/`REVOKE … ON SCHEMA public` change its stored ACL (`{pg_database_owner=UC/…,=U/…}`, observed; the grant itself not run here) | 0A000 | Its default ACL is not stored, and nothing is enforced (#653). |
| `CREATE DOMAIN`, `TYPE` or `PROCEDURE` in `pg_catalog`, and any `CREATE` in `information_schema`, succeed for a superuser; `CREATE TABLE pg_temp.x` makes a temporary table | 0A000 | The layer has no such schema to write in, nor temporary tables. A relation in `pg_catalog` is 42501 in both. |
| `postgres.s.t` reads `s.t` when `postgres` is the current database | 0A000 for every three-part name | Q5. An error, never another database's object. |
| `CREATE TRIGGER` as an element | 0A000 | No triggers. |
| Objects created under `AUTHORIZATION r` are owned by `r` | No owner | Objects have no owner in the layer. |
| Two sessions' `CREATE SCHEMA s` race: the second waits, then 23505 or 42P06 | The second to commit is 40001 | A DDL never waits for another (`transaction_design.md:477-502`). |
| `pg_namespace`, `pg_class`, `information_schema.schemata` answer | 42P01 (`information_schema.x`, `pg_catalog.x`) | No catalog views (`pg_session.cppm:449`); an error, not a wrong answer. psql's `\dn` fails as `\dt` does today. |
| `DROP SCHEMA … CASCADE` lists dropped objects in its order | The same objects; the DETAIL lines may be in another order | A notice, not a result. |
| `WITH CHECK OPTION` with a subquery in a checked `WHERE` is accepted and enforced | 0A000 at `CREATE VIEW` | As for a `CHECK` constraint's subquery. |
| `WITH CHECK OPTION` with `LIKE` in a checked `WHERE` is accepted and enforced | 0A000 at `CREATE VIEW` | A checked `WHERE` is evaluated as a value, which has no `LIKE` yet. |
| `WITH CHECK OPTION` with a comparison of a `REAL` or `DOUBLE PRECISION` in a checked `WHERE` is accepted and enforced | 42601 at `CREATE VIEW` | A checked `WHERE` is evaluated as a value, which compares no floating-point value yet; the same view without the option is created. |
| `WITH CHECK OPTION` over a view `INSERT` cannot write through (`DISTINCT`, `GROUP BY`, …) is created; an `INSERT` through it is 55000 | 0A000 at `CREATE VIEW`, the reason as HINT | A view is never created that `INSERT` then refuses. |
| `WITH (check_option=…)`, `ALTER VIEW … SET (check_option=…)`, `CREATE OR REPLACE VIEW` | 42601, as view options and `OR REPLACE` are today | Not built; the reloption form would set the same record field. |
| `WITH CHECK OPTION` also checks `UPDATE` through the view | `UPDATE` of a view is 0A000 (#678) | Not built. |

### Guarantees and CONTRACT rows

`bytecaskdb-sql-layer/CONTRACT.md`; each change is named in the PR that
makes it. Rows are named by their first words.

| Row | Change |
|---|---|
| "Each DDL statement is atomic" | **Widened** to `CREATE SCHEMA` (with its elements, as one statement) and `DROP SCHEMA`. Tests: stage 1b and 2's reopen and commit-hook tests. |
| "A DDL never waits for another" | **Widened** to `CREATE SCHEMA` with elements in autocommit: a conflict at its internal commit is 40001 with nothing written, not retried. Test: stage 2's "two `CREATE SCHEMA` with elements race; the loser writes nothing". |
| "A catalog the build cannot decode is refused at open" | **Widened** to the schema record, both name entry forms, the new formats, and a name that is empty or holds a NUL byte. Tests: stage 1b's `catalog_test` refusals. |
| "CREATE VIEW stores the view in the catalog" | **Widened**: the namespace is per schema, so one name may be a table in `public` and a view in `s`. Tests: stage 1b's functional cases. |
| "A table or view that a view reads cannot be dropped" | **Widened** by stage 5: `DROP SCHEMA … CASCADE` drops the views that read what it drops. DROP TABLE and DROP VIEW stay RESTRICT only. Before stage 1, **fixed** (built): the guarantee now also holds for a table read only by an `ORDER BY` subquery. Test: "embedded API: a view's dependencies are every relation its query reads, a subquery's in any position". |
| "`Database::open` of a database written by an SQL-layer build from before #780" | Unchanged in substance: a database holding a record this build wrote, any schema included, is refused by a build from before stage 1b. Proved by the older build's own "catalog: a damaged or foreign-format record is refused" and by this build's test that every record and ACL it writes is in the new format. |
| "A DDL statement given to a `Transaction`" | **Widened** to `CREATE SCHEMA` and `DROP SCHEMA`; a `CREATE SCHEMA`'s elements are steps of the transaction; `DROP SCHEMA` of what an open cursor reads is 55006. Tests: stage 1b and 5's `api_txn_ddl_test` cases. |
| New, stage 1b | **Added**: qualified names resolve to that schema's object or fail with PG 17's SQLSTATE (3F000, 42P01, 42704, 42883, 42501, 0A000, 42601); an unqualified name resolves in `public`; `CREATE SCHEMA`/`DROP SCHEMA` errors (42P06, 42939, 3F000, 2BP01). Tests (as built): "schemas: CREATE and DROP SCHEMA errors are PostgreSQL's", "schemas: messages name a missing object as s.x, and DROP SCHEMA and DROP ROLE list as PostgreSQL 17 does", "schemas: a schema is a record, a name entry and an ACL record, and its objects are its own through a reopen", "catalog: lookups are keyed by (schema, name), and a DDL creating in a schema that does not exist is refused", `tests/functional/cases/postgresql/schemas.yaml`, sqltest f311_01_01_01; the row in `CONTRACT.md` names the rest. |
| New, before stage 1 | **Narrowed**: a type name qualified by anything but `pg_catalog.` is refused (3F000, or 42704 for `public.`), where its last part was read alone. Test: "parser: a qualified type name other than pg_catalog's is refused". |
| New, stage 2 | **Added**: a view resolves each unqualified name to the object it was created against, whatever is created later. Tests: stage 2's shadowing test, through a reopen. |
| New, stage 3 | **Added** (built): a row written through a view with a check option that its checked `WHERE`s do not hold true is 44000 and writes nothing. Tests: "embedded API: a view WITH CHECK OPTION refuses a row its checked WHEREs do not hold true of", the view record's round trip in "schemas: a record of an earlier format reads as an object of public", `tests/functional/cases/postgresql/views.yaml`, sqltest f311_04_01_01..03. |
| New, stage 4 (if built, Q9) | **Added**: schema privileges are stored and survive a reopen; not enforced (#653). Tests: stage 4's reopen and refusal tests. |

The layer's `README.md` drops "schemas" from what is not built
(`README.md:53`) once stage 2 ships, and `frontend_design.md` loses each
refusal a stage removes (`:861-863`, `:994-995`).

### Stages of schemas

Each stage is one PR on the previous, with its tests, docs and contract
rows, built in the order below. None adds an engine durability site
(`fdatasync`, rename, unlink, a hint file), so `tests/CLAUDE.md`'s fault
sweeps do not apply; each new record kind gets the catalog's load-refusal
tests and a reopen test, as views, roles and types did
(`tests/catalog_test.cpp:263`, `:1020`, `:1133`, `:1315`, `:1483`, `:1648`,
`:592`), and each new DDL a crash at the commit hook
(`set_commit_hook_for_testing`, as `api_test.cpp:4531` holds a CTAS
crash).

**Before stage 1: two fixes of today's bugs, each its own PR.**
- **Qualified type names. Built.** `parser.cppm` kept a type name's last
  part; it now refuses any qualifier but `pg_catalog.` (3F000 `schema
  "foo" does not exist`, `public.int4` 42704, and so `information_schema.`
  and `pg_toast.`, which PG 17 always has), as PG 17; three parts are
  0A000 and four 42601, as Q5 and *Name resolution with schemas* give
  them. `pg_catalog.integer` and the grammar's other SQL spellings are
  42704, as in PG 17 (a procedure variable declared so ran as `int4`). A
  `CREATE TABLE IF NOT EXISTS` of a name in use skips before the
  qualifier is checked, as PG 17 does. Test as in the CONTRACT table, the
  embedded API test of a procedure variable, and
  `tests/functional/cases/postgresql/qualified_type_names.yaml`.
- **A view's dependencies from the binder's own resolution. Built**
  (*Views resolve against what they were created against*, rule 1), which
  closes the `ORDER BY` subquery gap (#870). Test: "embedded API: a view's
  dependencies are every relation its query reads, a subquery's in any
  position": a view with a subquery in the select list, `WHERE` (scalar,
  `IN`, `EXISTS`), `ORDER BY` (of a table, and of a view), and a set
  operation's arm blocks `DROP TABLE` or `DROP VIEW` of what it reads, as
  PG 17's 2BP01, through a reopen, and names the view that reads the
  relation itself; a subquery in `GROUP BY` or `HAVING` and a `WITH` query
  are refused at `CREATE VIEW` (42601). Also
  `tests/functional/cases/postgresql/views.yaml`.

**Stage 1a: lookups keyed by (schema, name), schema 0 only. Built.** No change in
behaviour. A `QualifiedName` type (schema id and name) replaces the bare
`std::string` of every relation, procedure and type reference: in each
`Ddl` struct (`DropTableDdl`, `CreateIndexDdl`, `AddColumnDdl`, …), in
`ast::FromItem` and the other AST relation references (~25 in
`sql/ast.cppm`), and in the catalog's lookup API, which has no overload by
name alone. A bare string no longer converts, so every site fails to
compile until it says which schema (binder 18/13, `plan/search.cppm` 28,
`api/database.cppm` 5, `catalog.cppm` 6 `.table(`/`.view(` calls).
Comparisons done on names rather than through lookups are changed to
compare `QualifiedName`s, each listed in the PR:
- the foreign key self-reference test `f.table == s.table`
  (`binder.cppm:6686`), which would otherwise take `CREATE TABLE s.t (…
  REFERENCES t)` as a self-reference when `t` is `public.t`;
- `relation_taken` (`api/database.cppm`, CTAS);
- the load check `cat.table(t.name)` for a type's row-type name;
- the names `check_in_use` reads from `DropTableDdl`, `CreateIndexDdl`,
  `AddColumnDdl` and `DropIndexDdl`.
Tests: the whole suite, unchanged.

As built: `QualifiedName` is `sql/names.cppm`, an aggregate of `schema` and
`name` with no constructor from a string, and `kPublicSchema` (0). The
parser gives every name it reads `kPublicSchema` (`in_public`), since it
still refuses a qualifier. The catalog's `names_`, `procedure_names_` and
`type_names_` are keyed by `QualifiedName`, built from the records in
`public`, since no record holds a schema until stage 1b; an index's own
name in `CreateIndexDdl` stays a string and is looked up in its table's
schema. `next_version` refuses a DDL that names any schema but `public`
(3F000), which only the `Catalog` API can build, rather than write it to
`public`. Name comparisons that stay bare, as the design says: alias
erasure in `resolve_aliases`, a column reference's qualifier, and the
binder's range-entry keys (stage 1b changes the last two). The embedded
API's `Database::call`, `Transaction::call`, `Transaction::procedure` and
`table_rows` still take a name as written and resolve it in `public`, the
session's path; stage 1b decides how they name a schema. Other comparisons
changed beyond the four above: an updatable view's base table is checked
by identity (`base == cat.table(name)`) rather than by name; the
procedure-variable check of a PL/pgSQL body keeps its scope as
`QualifiedName`s. Test: "catalog: lookups are keyed by (schema, name), and
a DDL naming a schema but public is refused".

**Stage 1b: the schema record and qualified names.** f311_01_01_01.
- Catalog: the record, its ACL record, both name entry forms, the record
  formats of *Schema catalog record and name entry keys* (view format 2
  with all its fields), the name check, the load checks.
- Parser and binder: qualified names everywhere (*Name resolution with
  schemas*), the parser's `FROM` changes, the path `public`, `s.d` type
  names, `CREATE SCHEMA` without elements (with elements 0A000 until
  stage 2), the system schema names, `AUTHORIZATION`, `DROP SCHEMA [IF
  EXISTS] list [RESTRICT]` (CASCADE 0A000 until stage 5), `DROP ROLE` of
  an owner, `CURRENT_SCHEMA` if PR 28 has not landed, the view map (rule 2
  and 3; every view this build writes stores one). SQLSTATEs 3F000, 42P06,
  42P09, 42602 and 42501 join `sql/sqlstate.cppm` where missing.
- Tests: `catalog_test`: the schema record cut at every byte, a later
  format, trailing bytes, under another id's key, a missing, extra or
  mistyped schema name entry, a schema without its ACL record, a `00`
  entry too short, of schema 0 or of a missing schema, a record whose
  schema disagrees with its entry, a format-7 table whose entry carries a
  schema, an index entry outside its table's schema, a schema id another
  object has, an empty name and a NUL in a name (record and entry), a view
  map naming an object outside `depends_on`, each refused; a format-7
  table, format-1 view and format-1 ACL still read, as objects of
  `public`; every record and ACL this build writes is of the new format;
  every schema DDL survives a reopen; a DDL the engine refuses (a
  follower) leaves nothing; a crash at the commit hook of `CREATE SCHEMA`
  and of `CREATE TABLE s.t` leaves neither after reopen, and one after
  the hook leaves both; a `Catalog` DDL with an empty or NUL name is
  refused. `api_txn_ddl_test`: `CREATE SCHEMA` in a transaction is
  invisible to others until commit and gone after rollback; two
  transactions creating one schema, 40001; a schema dropped in a
  transaction that then creates a table in it is 3F000. Plan cache: a
  statement prepared on `t` keeps reading `public.t` after `CREATE TABLE
  s.t`. A view whose stored map lacks a name its query reads (a forged
  record that passes load) is XX000 at use, with a mutation that restores
  a path fallback and must fail it. An allocation-fault sweep over a
  transaction of `CREATE SCHEMA` and `CREATE TABLE s.t` (as
  `api_rebind_test.cpp:666`). Functional cases: each error of the tables
  above, the system schema names, `FROM s.t x, public.t y WHERE x.c =
  y.c`, 42P09, 42712, the three 42P01 forms, `DROP SCHEMA` lists, the
  RESTRICT DETAIL rules (several kinds, a view in `public` over a view of
  the schema, more than 100 objects, two schemas named), a quoted name.
  `test_sql_machine` gains schema DDL in its model.

As built, with what the code made different:
- **The parser keeps a schema's name, the binder resolves it.** The
  parser cannot know a schema's id, and a prepared statement is bound
  again after every DDL, so a schema dropped and created again is another
  id. Every AST name is an `ast::ObjectName` (the schema as written, "" for
  none, and the name; `sql/ast.cppm`), and the binder turns it into a
  `QualifiedName` where it looks it up: `bound::lookup_name` for a read (a
  schema that does not exist is `kMissingSchema`, where nothing is, so a
  read is 42P01 naming `s.t`), `created_name` for a `CREATE` (3F000, or
  the system schema refusals), `dropped_name` for a `DROP` (3F000, or under
  `IF EXISTS` a `SkippedDdl`, a DDL that changes nothing and gives
  PostgreSQL's NOTICE `schema "s" does not exist, skipping`). The rule that
  a site does not compile until it says which schema still holds: an
  `ObjectName` does not convert to a `QualifiedName`.
- **System schemas are ids no record has**: `pg_catalog`,
  `information_schema` and `pg_toast` resolve to `kPgCatalogSchema` and its
  neighbours (`sql/names.cppm`), above every object and role id. A lookup
  there finds nothing; the catalog refuses a `CREATE` there (3F000) behind
  the binder's own refusals. `pg_temp` is no schema, as in a PostgreSQL
  session that has made no temporary object: a read of `pg_temp.x` is
  42P01, a `DROP` 3F000, a `CREATE` 0A000.
- **A user schema's type** (`s.d`, a column's type or a cast) is kept by
  the parser (`ColumnDefAst::type_schema`, `Expr::cast_schema`) and refused
  by the binder: 3F000 when the schema does not exist, 42704 when it holds
  no type of the name, 0A000 when it does (a domain is no usable type yet).
- **Only a `CREATE`'s schema is checked by `next_version`**; a `DROP`,
  `ALTER` or `GRANT` naming a schema that does not exist finds nothing, as
  a lookup does (42P01, 42704, 42883).
- **A qualified column reference** (`s.t.c`, `s.t.*`) is resolved by the
  parser against its query's own `FROM` (`resolve_schema_qualifier`): it
  names the unaliased item of table `t` and schema `s`, an unqualified
  item being public's (the path), and then reads as `t.c`. PostgreSQL's
  three 42P01 forms otherwise. In a subquery, one that names no unaliased
  item of the subquery's own `FROM` is 0A000 before any 42P01 form:
  PostgreSQL looks for it in the queries around, and reads it there when
  one has the item, even past an item of the same table name in the
  subquery (the functional case is marked unimplemented). Four parts are
  0A000 (`db.s.t.c`) and five 42601.
- **Not built: one table name from two schemas, neither aliased, in one
  `FROM`** (`FROM s.t, public.t`). The binder tells `FROM` items apart by
  name, so the parser refuses it, 0A000, where PostgreSQL reads both and
  gives 42P09 for an unqualified `t.c`; an alias on either works, and the
  functional case is marked unimplemented. `FROM t, public.t`, one table
  twice, stays 42712 as in PostgreSQL.
- **The view map.** `Binder::resolved_names` (each table a `FROM` resolved
  by an unqualified name, with the schema it was found in) and the
  expander's views read at depth 0 by an unqualified name make the stored
  names, from the one resolution that makes `depends_on`. At each use
  `view_definition` parses the view and gives each unqualified `FROM` name
  its stored schema (`stored_names`); a name the record lacks is XX000
  `view v has no stored resolution for t`. A view of format 1 has none and
  reads its names in public (`View::public_names`). The mutation
  `tests/schema_mutations/view_names_path_fallback.patch` restores a path
  fallback, and the XX000 test catches it.
- **The embedded API names a procedure's schema**:
  `Database::call` and `Transaction::call` take a `QualifiedName` (a bare
  name is public's), and `Transaction::procedure(schema, name)` resolves a
  schema as SQL writes it against the transaction's catalog (3F000).
  `CALL s.p()` over the wire uses them. `table_rows` still names a table
  of public.
- **`DdlRefused` carries a DETAIL and a HINT** (`with_notes`), which the
  server sends: `DROP SCHEMA`'s RESTRICT list and HINT, `CREATE SCHEMA
  pg_x`'s DETAIL, `DROP ROLE`'s `owner of schema s`, a line for each
  schema the role owns. As PostgreSQL 17 writes them, a schema's name in
  `DROP SCHEMA`'s message and DETAIL is bare (`cannot drop schema R Q`,
  `depends on schema R Q`) while a relation's is quoted, and past 100
  lines the DETAIL ends `and 1 other object` or `and N other objects`.
- **A missing object's name is written as PostgreSQL writes it**
  (`Catalog::written`): `s.x` for a schema other than public, bare in
  public, in `DROP TYPE`/`DROP DOMAIN`'s error and IF EXISTS NOTICE,
  `GRANT`/`REVOKE`'s, and `DROP PROCEDURE`'s, which reads `procedure
  s.p(integer, text) does not exist`, or `could not find a procedure named
  "s.p"` with no argument list. PostgreSQL keeps a written `public.` too
  (`type "public.d"`); a resolved name no longer carries it, so the layer
  writes `type "d"` there.
- **Tests**, beyond the unit tests the list above names: the
  `test_sql_machine` rule `schema_of_its_own` (a schema of its own on
  PostgreSQL, named after the oracle's, since the oracle's `search_path`
  is not public), and `tests/functional/cases/postgresql/schemas.yaml`.
  The RESTRICT DETAIL past 100 objects is tested at 101 and 102. Not built
  from the list: an allocation-fault sweep that runs here (the sweep is
  written; it runs where allocation faults are built in).

**Stage 2: schema elements.** f311_02_01_01, f311_03_01_01,
f311_05_01_01 and 02.
- Elements run as a transaction's steps, in PG 17's order, with the path
  (`s`, `public`); 42P15 joins `sql/sqlstate.cppm`; `IF NOT EXISTS` with
  elements 0A000; the element forms refused (42601, 42P16, 0A000); a view
  element's text cut from the statement.
- Tests: a failing element (each kind) leaves no schema and no element, in
  autocommit and in a transaction; a crash at the commit hook leaves none
  after reopen; an index or view written before its table works; a view
  over a later-written view is 42P01; a view created against `public.t1`
  as an element still reads `public.t1` after `s.t1` is created, through
  a reopen, with `public.t1` read from a subquery in each position too;
  `INSERT` through an element view; an element view whose text cannot be
  cut out is 0A000 (`SELECT a AS create …`, whose slice fails the
  tree check); a statement prepared through a view rebinds after `CREATE
  SCHEMA`; two `CREATE SCHEMA` with elements race and the loser is 40001
  with nothing written. An allocation-fault sweep over `CREATE SCHEMA`
  with elements in autocommit, the new internal-transaction path.

As built, with what the code made different:
- **The parser reads the elements and places them.** `ast::CreateSchema`
  holds its `elements` in written order, each an `ast::SchemaElement`:
  the element's statement (a `CreateSequence`, `CreateTable`,
  `CreateView`, `CreateIndex` or `Grant`, read by the parser's own
  converter for that statement), and two deferred errors. A name an
  element creates, and an index's table, is written qualified by the new
  schema's name as the statement gives it; one qualified by another
  schema keeps its name and records the 42P15 (`misplaced`), and a
  temporary table, view or sequence the 42P16 (`refusal`). `run_order`
  gives PG 17's order. `CREATE TRIGGER` is refused (0A000) as the
  statement is parsed, so before the schema's own errors (42P06, 42704),
  where PostgreSQL reports those first.
- **The path is a mark on the name, not a search in the binder.**
  `bound::bind_schema_element` copies the element and gives each
  unqualified name it reads that the new schema holds (a relation of any
  kind, or a type for `GRANT … ON DOMAIN | TYPE`) `ObjectName::path_schema`,
  which `lookup_name` honours; every other unqualified name stays
  public's. A view element's `FROM` items are marked in every position,
  subqueries and set operation arms included (`path_names`, as
  `stored_names` walks them), so the binder's `resolved_names`, which
  makes the view's stored names, records the schema each was found in.
  A table's `REFERENCES` its own unqualified name is the new schema's
  table, as PostgreSQL finds it once the table exists.
- **The API.** `Transaction::create_schema_elements` steps the schema,
  raises the first `misplaced` in written order, then binds and steps each
  element against the transaction's catalog of the moment, with
  `check_in_use`. Autocommit (`Database::execute`, by text or prepared)
  runs it through `Database::schema_with_elements`: a transaction begun
  with the session's user and the default options, the statement as its
  one step, then its commit, outside the call's lock (the transaction's
  calls take it). Its commit's 40001 is the caller's, as designed.
- **The cut.** `element_ranges` scans the statement's tokens: each element
  starts at a `CREATE` or `GRANT` at parenthesis depth 0 after the
  statement's own, and ends at the last token before the next start,
  comments and `;` left out. A `GRANT` after `WITH` (`WITH GRANT OPTION`)
  and a `CREATE` or `GRANT` after `GRANT` or a comma (a privilege, as in
  `GRANT CREATE ON …`) start no element. `view_element_text` parses the slice and
  compares its tree with the element's, every `location` zeroed
  (`zero_locations` walks protobuf-c's descriptors) and both packed.
- **Not built:** a schema-qualified column (`s.t.c` or `s.t.*`, `s` the
  new schema or `public`) in a view element whose query names `t`
  unqualified and unaliased. The parser resolves such a qualifier against
  the `FROM` items as written, an unqualified one taken as public's, but
  which schema an element's unqualified `t` is (the new one or `public`)
  is known only when the element binds: the parser would read the column
  against the wrong item or refuse it with a wrong 42P01. It is refused
  with 0A000 (`refuse_path_qualified_columns`, before the view is read),
  where PostgreSQL reads it. Qualifying the `FROM` item works.
- **Tests:** as listed above, in `tests/api_txn_ddl_test.cpp` (the
  `schemas:` cases of stage 2) and `tests/functional/cases/postgresql/schemas.yaml`.
  The allocation-fault sweep is written; it runs where allocation faults
  are built in.

**Stage 3: `WITH [LOCAL | CASCADED] CHECK OPTION`. Built** (*WITH CHECK
OPTION* says what was adapted). f311_04_01_01..03.
- The view record's `check_option` (values 1 and 2 accepted at load from
  here), the creation check, the plan's checks after `bc_insert` and before
  `RETURNING`, `BC_C_CHECK_OPTION` in the plan API
  (`docs/plan_api_design.md`), 44000 joins `sql/sqlstate.cppm`.
- Tests: each case of *WITH CHECK OPTION* against PG 17 (a violating row,
  a NULL, CASCADED and LOCAL over views with and without an option, the
  innermost failing view named, the `Failing row` DETAIL with a hidden and
  a defaulted column, 23505, 23514 and 23502 before 44000, `INSERT …
  SELECT`, `INSERT … RETURNING` that emits nothing before the error, a
  `$n` parameter, the 0A000 and its HINT per refusal, a view of only
  computed columns, a subquery in a checked `WHERE` below a `CASCADED`
  view); `INSERT … ON CONFLICT` into a view stays 0A000
  (`binder.cppm:5612`); the record with an option round-trips, is refused
  cut and with an option value of 3, and survives a reopen.

**Stage 4: schema privileges** (Q9 recommends deferring it to an issue
with #653; until then `GRANT`/`REVOKE … ON SCHEMA` and `ON ALL TABLES IN
SCHEMA` stay 0A000, and the ACL record holds only the default). `GRANT`/`REVOKE … ON SCHEMA`, `ON ALL
TABLES IN SCHEMA`, `DROP ROLE` of a grantee 2BP01. Tests: each error of
*Schema owner and privileges* against PG 17; the first `GRANT` writes the
default out; a `REVOKE` that empties the ACL keeps the record; the ACL
survives a reopen and is refused with a privilege a schema does not take;
`DROP SCHEMA` takes it.

**Stage 5: `DROP SCHEMA … CASCADE`** (Q9). The closure over `depends_on`,
the in-use check, the notices, one batch. Tests: a schema of a table, an
index, a view, a sequence, a domain, a shell type and a procedure, with a
view in `public` over its table and a view over that view, all dropped
with their ACLs and rows (no key under any dropped prefix), and nothing
else; the NOTICE and RESTRICT's DETAIL list the objects PG 17 lists, which
exclude indexes; an open cursor over a table of the schema makes it 55006
in a transaction; a crash at the commit hook leaves all of them after
reopen; in a transaction, rollback restores them. An allocation-fault
sweep over a `DROP SCHEMA … CASCADE` of several kinds.

**Recorded and hand-written cases.** The PG 17 oracle records each case in
its own schema with `search_path` set to it alone
(`bytecaskdb-sql-layer/tests/functional/record.py:124-130`), so a case
that names `public` reads PG's real `public`, not the case's. Cases that
name `public` (`FROM s.t, public.t`, a view element over `public.t1`, the
`DROP SCHEMA` DETAIL with a view in `public`) are hand-written, each with
the PG 17 probe that gave its expectation in a comment; every other case
is recorded. A case that creates a schema gives it a unique name and drops
it in `teardown`.

### Open questions for the owner

Each has a recommendation; the design above assumes it. Implementation
choices that were questions before (where the schema id lives, a view
element's text, stage order) are decided above.
- **Q1. `search_path`.** (a) Fixed `public` (and the element path).
  (b) `SET`/`SHOW`/`RESET search_path` per session: the shared plan cache
  keys by path, `CURRENT_SCHEMA` follows it, a path of no existing schema
  makes `CREATE` 3F000 "no schema has been selected to create in" (PG 17).
  (c) (b) with `"$user"`. **Recommended: (a) for these stages, (b) as a
  follow-up issue**; views already store their resolution, so (b) changes
  no record.
- **Q2. `DROP SCHEMA public`.** (a) 0A000. (b) A real, droppable `public`:
  a record written for it, and a marker once it is dropped, which every
  existing database would have to gain. **Recommended: (a).**
- **Q3. CASCADE beyond schemas.** (a) `DROP SCHEMA … CASCADE` only (stage
  5). (b) Also `DROP TABLE`/`VIEW … CASCADE`, which share the closure.
  (c) No CASCADE: 0A000. **Recommended: (a)**, (b) as a follow-up issue.
- **Q4. `AUTHORIZATION`.** (a) Accept and store the owner (stage 1b).
  (b) Check that the role exists but store nothing; `DROP ROLE` of it then
  succeeds, a listed difference. (c) 0A000 until objects have owners.
  **Recommended: (a)**: one `u32`, and `DROP ROLE` keeps PG 17's 2BP01;
  Q7(a) removes the older-build hazard it would otherwise carry.
- **Q5. Three-part names.** (a) 0A000 always. (b) Accept the current
  database's name (the wire's startup `database`; the embedded API has
  none). **Recommended: (a).**
- **Q6. Must a build from before schemas keep opening a public-only
  database this build has written?** (a) No: this build writes every
  record in its new format, so an older build refuses a database once
  this build has written any record to it; one this build only read still
  opens. (b) Yes: write a record in its new format only for an object
  outside `public`, keep two encodings per kind and a load check that an
  object has only the one its schema calls for, and prove the older-build
  rows with a fixture written by an older binary. **Recommended: (a)**:
  the CONTRACT already promises only that "A database this build writes
  cannot be opened by a build from before it", and (b) doubles every
  record kind for a property nobody is promised.
- **Q7. A schema in an older build.** A build from before stage 1b ignores
  `01 0D`/`01 0E`, so it would open a database whose schemas are empty and
  ungranted, and could then `DROP ROLE` a schema's owner, leaving a
  database this build refuses for an owner that is no role. (a) Every
  schema has an ACL record, of a format the older build refuses, so it
  refuses any database holding a schema. (b) Q4(b): store no owner, so
  there is nothing to break. (c) Have this build accept a missing owner
  with a fallback. **Recommended: (a)**: the ACL record is the schema's
  own (PG's `nspacl`), it needs no key outside the ACL namespace, and the
  older build gives its "later build" refusal.
- **Q8. Refusing keys under an unknown `0x01` sub-prefix.** `Catalog::load`
  ignores them today (`catalog.cppm:2155-2350`), so a record kind a later
  build adds would be ignored, not refused, by this one. Refusing them
  narrows what opens, and protects future builds, not schemas. (a) A
  separate PR, before or after these stages, with its own "Narrowed"
  CONTRACT entry: it spells out the known set exactly (the counters under
  `01 01` are read by key, not scanned, so a third counter `01 01 03` is
  refused like an unknown sub-prefix), a test that a fixture with only
  counters and every current sub-prefix still opens, and one refusal test
  per unknown byte. (b) Bundle it into stage 1b. (c) Leave it.
  **Recommended: (a).**
- **Q9. Scope: stages 4 and 5.** No F311 case needs either. Stage 4 stores
  privileges nothing enforces (#653); stage 5 is the layer's first CASCADE
  closure, with a batch bounded only by the schema's size. (a) Build both
  now, after stage 3. (b) Build stage 5 now, stage 4 as an issue with
  #653. (c) Both as issues: `GRANT … ON SCHEMA` and `DROP SCHEMA …
  CASCADE` stay 0A000. **Recommended: (b)**: `DROP SCHEMA … CASCADE` is
  how schemas are normally dropped (the oracle's own teardown uses it),
  while stored, unenforced privileges can wait for #653, which needs
  them; the ACL record exists from stage 1b either way.

### Risks of schemas

- **A lookup that forgets the schema** resolves in `public` silently, a
  wrong object. Stage 1a removes every name-only lookup and every bare
  name in a `Ddl` or AST reference, so a missed site does not compile;
  name comparisons outside lookups are listed and changed there.
- **The PG 17 oracle runs each case in its own schema** (*Recorded and
  hand-written cases*): a case that names `public` cannot be recorded
  faithfully and is hand-written.
- **Views bound at each use.** The map holds only if the binder records
  every relation it resolves, in every position; the stage 2 shadowing
  test reads `public.t1` from each position, and a name the map lacks is
  an error, never a search.
- **Type and sequence names bound at each use.** Only relations enter a
  view's map. When domains become column types, a cast to `::s.d` is
  allowed in a view, or `nextval('seq')` is built (`frontend_design.md:1006`),
  an unqualified type or sequence name in a view or a column default would
  resolve against the path at each use, where PG 17 binds the object at
  creation. Each must enter the stored resolution when it lands.
- **Relation names `pg_…` in `public`.** PG 17 searches `pg_catalog`
  first, so `SELECT * FROM pg_class` reads its catalog even with a
  `public.pg_class`; the layer reads the user's table. This is true today
  and unchanged here; a follow-up issue tracks it.
- **A large `DROP SCHEMA … CASCADE`** is one commit: `del_range` is one
  append, but the engine removes each key from its key directory by
  reading it back (`docs/bytecask_design.md:49`), so its latency grows
  with the rows and index entries of every table it drops, together. It
  holds the catalog's DDL slot for that time, so every other DDL fails
  with 40001 while it runs.

### Out of scope for schemas

`SET search_path` (Q1), `ALTER SCHEMA … RENAME` and `OWNER TO`, `ALTER …
SET SCHEMA`, catalog views (`pg_namespace`, `information_schema`),
temporary schemas (`pg_temp`), privilege enforcement (#653), `DROP TABLE`
and `DROP VIEW … CASCADE` (Q3), `UPDATE` and `DELETE` through a view with
a check option (#678), the view-option and `OR REPLACE` forms of a check
option, `CREATE TRIGGER` elements, schema-qualified operators and
functions other than `pg_catalog.`'s, refusing unknown `0x01`
sub-prefixes (Q8).

## Tests

`tests/catalog_test.cpp`: the record round-trips; a record cut at every
byte, with a bad type code, an unknown key column or trailing bytes, and a
format-2 record, are refused; each DDL survives a reopen; a DDL refused by
the engine (a follower) leaves nothing on disk or in memory; a drop leaves
no key under the table's or index's prefix; the name entries and the
records agree; a reader pinned while a DDL is held mid-commit returns at once
on the old version; concurrent DDL each get the version they published; a backfill raced by a write, and a DDL raced by another
catalog's, are conflicts. `tests/plan_api_test.cpp`: each compatibility
rule, `bc_ctx_check`, and a write committed across a DDL
on its table is a conflict. `tests/plan_writes_test.cpp`: `bc_insert`,
`bc_update`, `bc_delete` and `bc_del_range` keep every index exact,
unique, `DESC` and nullable parts included, and an index created after a
plan was compiled is maintained by it. `tests/api_test.cpp`: through SQL,
every cached plan binds again once after any DDL, `ADD COLUMN` and
`CREATE INDEX` included, and a statement over a dropped table is the
binder's error when it binds again; unique indexes and ranges on `DESC` parts, nullable and not.
`tests/tcc_test.cpp`: a unique insert raced by another commit conflicts,
and its retry raises the duplicate. `tests/tprocc_test.cpp`: the
nine TPROC-C tables and their indexes, created by SQL DDL, reopened and
checked. Views: `tests/catalog_test.cpp` has the record cut at every byte, a later
format, trailing bytes, a missing or mistyped name entry, the drops that
2BP01 refuses, and a reopen; `tests/api_test.cpp` reads views through SQL.
