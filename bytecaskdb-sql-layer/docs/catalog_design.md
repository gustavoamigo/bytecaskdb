# The SQL layer's catalog

Status: **built** (#489): `bytecaskdb-sql-layer/catalog/`, with its tests in
`tests/catalog_test.cpp` and the TPROC-C schema in `tests/tprocc_test.cpp`.
This covers what the catalog records, how it is stored under namespace
`0x01`, the in-memory form plans resolve against and how readers share it,
what makes a compiled plan compatible with the live catalog, and how each
DDL statement writes. Nothing in the engine changes for it.

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
| layouts | Each layout generation: its object id, its generation number, `bitmap_bytes`, and its offset table (column id → byte offset or variable slot). The last is current. Each table has one until `REBUILD` exists, which appends one (#503). |

A default is a constant of the column's physical type, or none (null).
DDL accepts integer, decimal, string and boolean literals, folded at
bind time to the stored value (a `DECIMAL(p, s)` default is stored scaled);
`TIMESTAMP`, `DATE` and `TIME` defaults, and expressions such as
`CURRENT_TIMESTAMP`, are refused: a plan cannot see the clock, and literal
parsing of those types isn't built.

## Serialization

Little-endian, every list counted, every string a `u16` length and its
bytes:

```
u16 format                   ; kRecordFormat, 4
u32 id, str name, u32 version, u32 next_col_id
u16 ncols   { u32 len, u32 id, str name, u8 kind, u32 length, u8 precision, u8 scale,
              u8 nullable, u8 default_tag, default, u8 hidden }
u16 npk     { u32 col_id }
u16 nidx    { u32 len, u32 id, str name, u8 unique, u16 nparts { u32 col_id, u8 desc } }
u16 nlayout { u32 len, u32 layout_id, u16 generation, u16 bitmap_bytes, u16 var_table, u16 nvar,
              u16 nslots { u32 col_id, u32 pos } }
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
too. A build reads every format from 3 to its own, taking an absent field
at its default; it refuses a newer format, whose added fields it may not be
able to ignore safely, and an older one: format 1 holds the earliest build's row
positions, format 2 (#490's) no SQL types. Both predate any database worth
reading. Format 4 added `hidden` at the end of a column entry; a format-3
record reads with no hidden column, as every table then had a declared
primary key. The record format also names the row format its positions are
for; formats 2, 3 and 4 share the 12-byte-header row format.

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
| `01 03 <name>` | Name entry: kind (1 table, 2 index) and id (4, big-endian). |

**The counters are 8 bytes**, and the next id at most one past
`kMaxObjectId`; any other value is refused at load. **One id counter** gives every table, index and layout its id, so an id
names one object for ever, and `bc_key_begin(table_or_index)` and
`bc_raise`'s object id need no kind. Ids stay at or below `kMaxObjectId`,
`0x00FFFFFF`: `bc_error_detail` carries one in 24 bits. A DDL that would
pass it is refused.

**The name entry** makes a name a key, so a `CREATE` guards its name with
`ensure_absent` and a rename is one key changed. Tables and indexes share
one namespace, as relations do in PostgreSQL. At load every name entry must
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

So adding a column invalidates nothing: no plan names the new column, and
rows that plans compiled before it write get its default from
`bc_row_begin`. The binder keeps that true for statement text too: an
`INSERT` without a column list fills the leading columns and gives the rest
their defaults, as PostgreSQL does, so the same text binds the same way
before and after the column is added. Adding an index invalidates nothing
either: a write plan compiled before it maintains it, and a read plan does
not use it until it is recompiled for another reason. Dropping an index
invalidates the plans that scan it. Dropping a table invalidates every plan
on it, and recompiling one fails at bind. An incompatible plan is
`BC_E_LAYOUT` and does not run; the embedded API recompiles it from its
text and runs it again.

## DDL

Every DDL statement is one atomic batch: the table record, the name entries
and the counters, plus the rows a backfill writes or the ranges a drop
deletes. It is built on a snapshot and guarded by `ensure_unchanged` on the
catalog version, so a second writer of the catalog makes it a conflict
rather than a lost update. The in-memory version is published only after
the batch commits; a failure before or in the commit leaves the catalog, on
disk and in memory, as it was.

| Statement | Batch |
|---|---|
| `CREATE TABLE` | Record, name entry (`ensure_absent`), counters. Two ids: the table and its first layout. The bitmap is sized as the row format says. Without a primary key, the record holds the hidden row id (*Tables without a primary key*). Each `UNIQUE` constraint is a unique index in the same record and batch, with its own name entry (*UNIQUE constraints*). A table of no columns is refused (0A000); PostgreSQL takes one. |
| `CREATE [UNIQUE] INDEX … (col [DESC], …)` | Record with the index, name entry (`ensure_absent`), counters, an entry per existing row, `ensure_range_unchanged` over the table's rows. A unique index whose rows already hold a duplicate is refused. |
| `DROP TABLE` | Record and name entries deleted; `del_range` over `02 <table_id>` and over `03 <index_id>` per index. |
| `DROP INDEX` | Record without the index, name entry deleted, `del_range` over `03 <index_id>`. |
| `ALTER TABLE … ADD COLUMN` | Record with the column appended at the next id and the next position of its region; the current generation's offsets do not move. With a default, every existing row rewritten with it, and `ensure_range_unchanged` over the rows. |

`IF EXISTS` on the drops; nothing else. A drop deletes in O(1) appends
whatever the table holds; the engine's `del_range` removes the keys from the
key directory as it applies. A `NOT NULL` column added without a default is
refused, as is a column whose id has no bit in the layout's bitmap (it needs
a `REBUILD`, which doesn't exist yet, #503). Ids taken by a DDL are not reused, even when
it is dropped later.

**Backfill is one batch, bounded by the engine's 1 GiB plan limit.**
`CREATE INDEX` and `ADD COLUMN … DEFAULT` on a populated table read every row
from the snapshot and write in the same batch as the catalog record, so the
index or column exists with all its entries or not at all, with no state in
between for a plan to see. The range guard makes a write to the table that
commits during the backfill a conflict; the DDL is retried up to three
times on a fresh snapshot and then fails. A backfill counts the bytes it
gathers and is refused, with an error that says so, as soon as they pass the
1 GiB limit, before it has read the rest of the table into memory. The alternative, batches written
while the index is invisible to reads but maintained by writes, needs an
index state in the record ("write-only") and plans that respect it; it is
deferred (#505), and the schema is created before the data until then. The
TPROC-C schema is.

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
`ADD COLUMN` and `CREATE INDEX` keep every cached plan, a drop refuses at
recompile; unique indexes and ranges on `DESC` parts, nullable and not.
`tests/tcc_test.cpp`: a unique insert raced by another commit conflicts,
and its retry raises the duplicate. `tests/tprocc_test.cpp`: the
nine TPROC-C tables and their indexes, created by SQL DDL, reopened and
checked.
