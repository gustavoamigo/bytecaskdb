# The SQL layer's catalog

Status: design, decided (#489); implemented in
`bytecaskdb-sql-layer/catalog/`, with the tests in
`bytecaskdb-sql-layer/tests/catalog_test.cpp` and the TPROC-C schema in
`tests/tpcc_test.cpp`. This document decides what the catalog records, how
it is stored under namespace `0x01`, the in-memory form plans resolve
against and how readers share it, what makes a compiled plan compatible
with the live catalog, and how each DDL statement of stage 1 writes. It
agrees with `docs/plan_api_design.md` (revision 4, which this work makes)
and `docs/sql_layer_types_design.md`; nothing in the engine changes.

The catalog is shared with the MariaDB plugin when the plugin adopts the
sql-layer's formats (#487, *Relationship to the MariaDB plugin*). It is
written from scratch: the plugin's catalog is positional, keyed by name and
GPL-2.0, and nothing of it is copied.

## What a table records

| Field | Meaning |
|---|---|
| `id` | Permanent object id, from the one counter below. Never reused. |
| `name` | Folded to lower case, as SQL folds an unquoted identifier. |
| `version` | Per-table counter, bumped by every DDL on the table. Not checked by plans (see *Compatibility*); kept for stage 2. |
| `next_col_id` | The next column id. Column ids are permanent and never reused, even after a column is dropped. |
| columns | Each: permanent `id`, `name`, SQL type (kind, `VARCHAR` length, `DECIMAL` precision and scale), nullability, default. The physical type follows from the SQL type (`docs/sql_layer_types_design.md`, *Stage-1 types*). |
| primary key | Column ids, in key order. Always ascending; a primary key column is never null. |
| indexes | Each: permanent `id`, `name`, unique or not, parts as (column id, `ASC`/`DESC`). |
| layouts | Each layout generation: its object id, its generation number, `bitmap_bytes`, and its offset table (column id → byte offset or variable slot). The last is current. Stage 1 has one per table; `REBUILD` appends one (#503). |

A default is a constant of the column's physical type, or none (null).
Stage 1 accepts integer, decimal, string and boolean literals, folded at
bind time to the stored value (a `DECIMAL(p, s)` default is stored scaled);
`TIMESTAMP`, `DATE` and `TIME` defaults, and expressions such as
`CURRENT_TIMESTAMP`, are refused: a plan cannot see the clock, and literal
parsing of those types is #492's.

## Serialization

Little-endian, every list counted, every string a `u16` length and its
bytes:

```
u16 format                   ; kRecordFormat, 3
u32 id, str name, u32 version, u32 next_col_id
u16 ncols   { u16 len, u32 id, str name, u8 kind, u32 length, u8 precision, u8 scale,
              u8 nullable, u8 default_tag, default }
u16 npk     { u32 col_id }
u16 nidx    { u16 len, u32 id, str name, u8 unique, u16 nparts { u32 col_id, u8 desc } }
u16 nlayout { u16 len, u32 layout_id, u16 generation, u16 bitmap_bytes, u16 nslots { u32 col_id, u32 pos } }
```

`default_tag` is 0 for none, else the physical type code (`bc_type`),
followed by 8 bytes for `i64` and `f64`, one for `bool`, a `u32` length and
the bytes for `bytes`.

**The append-only rule.** A later format adds fields only at the end of the
record or of an entry, never reorders or reinterprets one. Columns, indexes
and layouts are length-prefixed entries, so a later format can extend them
too. A build reads every format from 3 to its own, taking an absent field
at its default; it refuses a newer format, whose added fields it may not be
able to ignore safely, and an older one: format 1 holds the prototype's row
positions, format 2 (#490's) no SQL types. Both predate any database worth
reading. The record format also names the row format its positions are
for; formats 2 and 3 share the 12-byte-header row format.

**A record that does not parse is refused at load**, with the table's id:
a truncated field, a type kind or tag outside its set, a physical type that
disagrees with the SQL type, a key or index column that is not a column of
the table, a layout that fails the row format's invariants, trailing bytes
after the last field. The catalog never opens a database it cannot decode
correctly.

## Storage

Namespace `0x01`, as the plugin's catalog is:

| Key | Value |
|---|---|
| `01 01 01` | Next object id, `u64` big-endian. |
| `01 01 02` | Catalog version, `u64` big-endian; bumped by every DDL. |
| `01 02 <id:4>` | The table record above, indexes and layouts included. |
| `01 03 <name>` | Name entry: kind (1 table, 2 index) and id (4, big-endian). |

**One id counter** gives every table, index and layout its id, so an id
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
not a key:

```c
bc_key ku = bc_key_begin(cx, IDX_U);           /* the index parts only */
...
bc_view u = bc_key_end(cx, ku);
bc_cursor uc = bc_scan_open(cx, IDX_U, u, u, BC_SCAN_PREFIX);
if (bc_scan_next(cx, uc)) {                    /* one record read */
  bc_raise(cx, BC_C_UNIQUE, IDX_U);
  return;
}
bc_guard_range(cx, IDX_U, u, bc_key_successor(cx, u));   /* the race */
```

This is the plan API's `bc_exists` → `bc_raise` → `bc_guard_absent`
pattern with the key replaced by its prefix: `bc_exists` and
`bc_guard_absent` name one key, and an entry under the prefix has the other
row's primary key after it, so neither could see it. The scan costs the one
record read `bc_exists` would; the range guard makes the engine refuse the
commit if any entry under the prefix changed since the snapshot. The plan
skips both when any part of the new entry is null. The primary key keeps
`bc_exists` and `bc_guard_absent`. An `UPDATE` that changes a unique
index's columns checks the new prefix the same way, only when it differs
from the old one, so a row never collides with itself. The plugin checks
its unique indexes by the same prefix scan.

Keeping one entry shape, rather than storing a unique index as its parts
with the primary key in the value, keeps `bc_scan_pk`, the covering reads
and `DESC` the same for every index, and needs no second shape for the
entries whose parts are null.

## The in-memory form

`Catalog` is immutable: the tables by id and by name, each index's table,
the key layout of every table and index (the parts `bc_key_*` and the scans
read, `DESC` included), and each layout id's table. It is built at open by
reading every record under `01 02` and the counters, and by every DDL from
the catalog before it. Lookups are `const` and allocate nothing.

**Readers share it through immutable versions.** `LiveCatalog` holds the
current version as a `shared_ptr<const Catalog>`; a DDL builds the next
version and publishes it by swapping the pointer. A context pins one version
for an execution and drops it at reset; nothing a reader holds changes under
it, and a reader never waits for a DDL's backfill. A lock taken by every
execution would be simpler to state but would put a shared cache line on the
read path the engine keeps lock-free, and would stop every query for the
length of a `CREATE INDEX`.

Two rules make a pinned version agree with the engine:

1. **A version and a snapshot are taken together.** `LiveCatalog::pin`
   reads a generation counter, takes the version and the snapshot, and
   reads the counter again, retrying if it changed. A DDL makes the counter
   odd before its commit reaches the engine and even after it publishes, and
   a pin that finds it odd waits for it. So no plan pairs a snapshot that
   holds a DDL with the version before it, or the reverse.
2. **A write commit guards the records of the tables it writes.**
   `bc_ctx_commit` adds `ensure_unchanged(01 02 <table_id>)` for every table
   whose rows or index entries the write plan touches, found from the keys
   themselves. A DDL on that table committed after the plan's snapshot
   changes the record, the commit is a conflict, and `bc_execute` retries on
   a fresh pin, where the compatibility check sends an incompatible plan back
   to be recompiled. One record read per table written, the price of never
   committing rows shaped by a catalog the engine has moved past.

DDL statements are serialized by a mutex in `LiveCatalog`; the engine's
directory lock keeps a second process out.

## Compatibility

`bc_schema_ref` lists what a plan depends on, and `bc_ctx_run` checks it
against the pinned version. The catalog version is in the ref for messages
only; nothing is checked by version equality. A plan is compatible when:

- **every table and index it reads exists.** Ids are never reused, and a
  table's primary key and an index's parts never change, so existence is
  the same key layout;
- **every column it reads or sets is in the current layout of its table**,
  at the offset or slot and with the type the plan was compiled with. A
  column's position and type never change within a generation, so this
  fails only for a dropped column or a new generation;
- **every table it writes exists, and the indexes it maintains are exactly
  that table's indexes**: none dropped, none added.

So adding a column invalidates nothing: no plan names the new column, and
rows that plans compiled before it write get its default from
`bc_row_begin`. Adding an index invalidates the write plans of its table,
which must maintain it, and no read plan. Dropping an index invalidates the
plans that scan it and the write plans of its table. Dropping a table
invalidates every plan on it, and recompiling one fails at bind. An
incompatible plan is `BC_E_LAYOUT` and does not run; the embedded API
recompiles it from its text and runs it again.

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
| `CREATE TABLE` | Record, name entry (`ensure_absent`), counters. Two ids: the table and its first layout. The bitmap is sized as the row format says. |
| `CREATE [UNIQUE] INDEX … (col [DESC], …)` | Record with the index, name entry (`ensure_absent`), counters, an entry per existing row, `ensure_range_unchanged` over the table's rows. A unique index whose rows already hold a duplicate is refused. |
| `DROP TABLE` | Record and name entries deleted; `del_range` over `02 <table_id>` and over `03 <index_id>` per index. |
| `DROP INDEX` | Record without the index, name entry deleted, `del_range` over `03 <index_id>`. |
| `ALTER TABLE … ADD COLUMN` | Record with the column appended at the next id and the next position of its region; the current generation's offsets do not move. With a default, every existing row rewritten with it, and `ensure_range_unchanged` over the rows. |

`IF EXISTS` on the drops; nothing else. A drop deletes in O(1) appends
whatever the table holds; the engine's `del_range` removes the keys from the
key directory as it applies. A `NOT NULL` column added without a default is
refused, as is a column whose id has no bit in the layout's bitmap (it needs
a `REBUILD`, not in stage 1). Ids taken by a DDL are not reused, even when
it is dropped later.

**Backfill is one batch, bounded by the engine's 1 GiB plan limit.**
`CREATE INDEX` and `ADD COLUMN … DEFAULT` on a populated table read every row
from the snapshot and write in the same batch as the catalog record, so the
index or column exists with all its entries or not at all, with no state in
between for a plan to see. The range guard makes a write to the table that
commits during the backfill a conflict; the DDL is retried up to three
times on a fresh snapshot and then fails. A backfill past the 1 GiB limit
is refused with an error that says so. The alternative, batches written
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

## Stage 2: several schema versions, deferred

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

## Validation

`tests/catalog_test.cpp`: the record round-trips; a record cut at every
byte, with a bad type code, an unknown key column or trailing bytes, and a
format-2 record, are refused; each DDL survives a reopen; a DDL refused by
the engine (a follower) leaves nothing on disk or in memory; a drop leaves
no key under the table's or index's prefix; the name entries and the
records agree. `tests/api_test.cpp` and `tests/plan_api_test.cpp`: the
compatibility cases (`ADD COLUMN` keeps a plan, `CREATE INDEX` recompiles
a write plan and not a read plan, a drop recompiles or refuses), a write
committed across a DDL is a conflict, a backfill raced by a write is
retried, unique and `DESC` indexes through SQL. `tests/tpcc_test.cpp`: the
nine TPROC-C tables and their indexes, created by SQL DDL, reopened and
checked.
