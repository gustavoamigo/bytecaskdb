# ByteCaskDB MariaDB Plugin — Code Guide

This document explains how the plugin is organized and how it works. Read it
when you open this directory for the first time or return after a break.

---

## What this is

A MariaDB pluggable storage engine. MariaDB loads `ha_bytecaskdb.so` at
runtime and routes `INSERT`, `SELECT`, `BEGIN`, `COMMIT`, etc. to it. The
plugin translates those calls into ByteCaskDB operations.

```
MariaDB server
  └── handler API  (write_row, rnd_next, index_read, external_lock, …)
        └── ha_bytecaskdb : public handler
              └── bytecask::DB  (one global instance for the whole server)
```

No MariaDB source tree is required to build it — just the development headers
(`mariadb-devel` on Fedora, `libmariadb-dev` on Debian) and the ByteCaskDB
static library built by the main xmake project.

---

## File map

```
bytecaskdb-mariadb-plugin/
├── bytecaskdb_plugin.cc   Plugin entry point. Opens the global DB,
│                          wires the handlerton callbacks, owns the
│                          catalog in-memory cache.
│
├── ha_bytecaskdb.h        Handler class declaration + global extern
│   ha_bytecaskdb.cc       declarations. Contains the full handler class.
│
├── bytecaskdb_txn.h       Per-connection transaction state (MariaDBTxn).
│   bytecaskdb_txn.cc      Buffers writes, holds a snapshot, commits via
│                          WritePlan, implements read-your-own-writes.
│
├── catalog.h              Key-space encoding for the persistent catalog:
│   catalog.cc             key builders, TableMeta serialization,
│                          ColumnMeta, IndexMeta.
│
├── key_encoding.h         Primary key and secondary index key encoding.
│   key_encoding.cc        Wraps MariaDB's key_copy() + the namespace prefix.
│
├── row_encoding.h         Row serialization: MariaDB record buffer → bytes.
│   row_encoding.cc        Adds a 3-byte versioned envelope (format + schema
│                          version) so ALTER TABLE can add/drop columns.
│
├── bytecask_view.h        Inline adapters between MariaDB's uint8_t*+size_t
│                          and bytecask's BytesView (std::span<const std::byte>).
│
├── integration_test.cpp   Catch2 unit tests for the pure helpers (key
│                          encoding, row encoding, catalog, txn buffer).
│
└── CMakeLists.txt         Out-of-tree build; finds MariaDB headers via
                           mariadb_config, links against libbytecask.a.
```

---

## Three core design decisions

### 1. One global ByteCaskDB instance for the entire server

`bytecaskdb_init()` opens a single `bytecask::DB` at `datadir/bytecaskdb/`.
Every schema, table, and index on the server shares it. Keys are namespaced
by a prefix byte so they never collide.

This is the same model as MyRocks. The benefit: a `WritePlan` with a single
`apply_batch()` can atomically commit writes to multiple tables and indexes
without any 2PC or distributed coordination.

The global DB pointer lives in `bytecaskdb_plugin.cc` and is declared
`extern` in `ha_bytecaskdb.h` so the handler can reach it.

### 2. Key-space layout

Every key stored in the DB starts with a 1-byte namespace tag:

```
0x01  Catalog   — TableMeta records and ID counters
0x02  Row data  — [table_id:4 BE][pk_packed]
0x03  Indexes   — [table_id:4 BE][index_id:4 BE][sec_key_packed][pk_packed]
0x04  Admin     — reserved
```

All multi-byte integers in keys are big-endian so `memcmp` order matches
numeric order — required for range scans and `del_range` boundaries.

This means `del_range({0x02,tid}, {0x02,tid+1})` atomically drops all rows
for a table in a single disk append, regardless of how many rows it has.

Key encoding lives in `key_encoding.h/cc` (primary and secondary keys) and
`catalog.h/cc` (catalog keys and metadata serialization).

### 3. MariaDBTxn — the per-connection transaction

MariaDB calls `external_lock(F_WRLCK)` when a statement begins and
`external_lock(F_UNLCK)` when it ends (for autocommit). Explicit `BEGIN` /
`COMMIT` / `ROLLBACK` come through `hton->commit` / `hton->rollback`.

`MariaDBTxn` sits in the per-THD slot (`thd_get_ha_data` / `thd_set_ha_data`)
and handles all of this:

- **Snapshot** — captured once at `begin_if_needed()`, held for the life of
  the statement (READ COMMITTED) or the transaction (REPEATABLE READ).
- **Write buffer** — `write_row`, `update_row`, `delete_row` all call
  `buffer_put` / `buffer_del` rather than writing directly to the DB.
- **Read-your-own-writes** — `get()` checks the buffer first, then the
  snapshot. `MergeIterator` merge-walks buffer + snapshot so scans also
  see buffered writes.
- **Commit** — builds a `WritePlan` from the buffer in insertion order and
  calls `db->apply_batch()`. If that returns `false` (W-W conflict), commit
  returns `HA_ERR_LOCK_DEADLOCK` and MariaDB retries the statement.
- **Rollback** — discards the buffer; snapshot released.

The dual-structure buffer (`ops_` ordered log + `lookup_` sorted map) is
the core of `bytecaskdb_txn.h`. `ops_` preserves causality for commit;
`lookup_` gives O(log n) RYOW lookups.

---

## Request traces

### INSERT INTO t VALUES (1, 'alice')

```
MariaDB calls:
  external_lock(thd, F_WRLCK)
    → txn->begin_if_needed()       captures db->snapshot()
    → trans_register_ha(thd, ...)  registers with MariaDB coordinator

  write_row(record_buf)
    → encode_current_pk(buf)       [0x02 | table_id | key_copy(PK)]
    → encode_row(buf)              [version:1][schema_version:2][raw reclength bytes]
    → txn->buffer_put(pk, row)     added to ops_ + lookup_

  external_lock(thd, F_UNLCK)   [autocommit]
    → txn->commit()
        WritePlan plan{std::move(*snap_)};
        for op in ops_: plan.put(key, val)
        db->apply_batch({.sync=true}, std::move(plan))
```

### SELECT * FROM t WHERE id = 1

```
MariaDB calls:
  external_lock(thd, F_RDLCK)
    → txn->begin_if_needed()       captures snapshot (if not held)

  index_read_map(buf, key, ...)
    → encode index key from `key` param
    → txn->get(pk, out)
        1. lookup_.find(pk) → check buffer
        2. snap_->get({}, pk, out) → read from snapshot
    → decode_row(out, buf)         fills MariaDB record buffer

  external_lock(thd, F_UNLCK)
    → read-only, no buffer → no commit needed
```

### Full table scan (SELECT * FROM t)

```
  rnd_init(scan=true)
    → txn->iter_prefix(lo=[0x02,tid], hi=[0x02,tid+1], table_id)
         returns MergeIterator that two-pointer walks:
           snapshot iter (from db snapshot)
           buffer iter   (from lookup_ map, lower_bound lo)
         buffer wins on ties; tombstones suppress snapshot keys

  rnd_next(buf) repeatedly
    → merge_scan_->valid() ? advance and decode : HA_ERR_END_OF_FILE

  rnd_end()
    → merge_scan_.reset()
```

---

## Catalog

The catalog is stored inside the same ByteCaskDB instance under the `0x01`
prefix. At plugin init, `catalog_init()` scans `[0x01|0x02, 0x01|0x03)` and
rebuilds two in-memory maps:

- `s_name_to_id` — `"db\0table\0"` → `table_id`
- `s_id_to_meta` — `table_id` → `TableMeta`

Protected by `s_catalog_mu` (a plain mutex; catalog mutations are DDL, rare).

`catalog_alloc_table_id()` uses a snapshot + `WritePlan` CAS loop against
the counter key so two concurrent `CREATE TABLE` statements never get the
same ID. In practice MariaDB's MDL serializes DDL, so the CAS never retries.

The catalog functions are defined in `bytecaskdb_plugin.cc` (where the
in-memory caches live) and declared `extern` in `ha_bytecaskdb.h` so the
handler can call them from `create()`, `delete_table()`, `rename_table()`.

---

## Row format

Rows are stored as a 3-byte envelope followed by the raw MariaDB record
buffer (`table->s->reclength` bytes):

```
byte 0:   format version (currently 0x01)
byte 1-2: schema_version LE u16  (bumped on ALTER TABLE)
byte 3+:  raw MariaDB record buffer (reclength bytes)
```

The schema version lets the decoder apply defaults for added columns or skip
dropped columns on rows written before the ALTER. `row_encoding.h/cc`
handles all of this.

---

## Build

```bash
cd bytecaskdb-mariadb-plugin
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

CMake finds MariaDB headers via `mariadb_config --include` and expects
`libbytecask.a` to be built already by the parent xmake project.

To run the unit tests:

```bash
cd build && ctest --output-on-failure
```

See `SMOKE_TEST.md` for the end-to-end MariaDB test procedure.

---

## Implementation status

| Phase | Description | Status |
|-------|-------------|--------|
| A | Persistent catalog, DDL (CREATE/DROP/RENAME) | Done |
| B | Full PK CRUD (INSERT/UPDATE/DELETE, index scans) | Done |
| C | MariaDBTxn: BEGIN/COMMIT/ROLLBACK, RYOW, OCC | Done |
| D | Handlerton MVCC flags, store_lock no-op | Partial |
| E | Secondary indexes (write + read path) | Done |
| F | Isolation levels (RC, RR, Serializable) | Partial |
| G | `del_range`-backed DDL (O(1) DROP TABLE) | Done |
| H | Vacuum, resume, replication hooks, backup | Not started |

Phases A–E and G are functional. The engine handles standard OLTP workloads:
multi-statement transactions, secondary indexes, DDL that survives restart.
Phase H (operational features) is the remaining gap before production use.

---

## Key invariants

- **No writes bypass `MariaDBTxn`.** Every `write_row`, `update_row`,
  `delete_row` goes through `buffer_put` / `buffer_del`. Direct `db->put()`
  is only called from DDL paths (catalog writes from `create()`, `delete_table()`).

- **Snapshot held for the life of the statement, not per-row.** A snapshot
  is taken once in `begin_if_needed()`. All reads in that statement see the
  same consistent state.

- **`apply_batch` returning false is not a fatal error.** It means W-W
  conflict: two transactions modified the same key concurrently. The commit
  path maps it to `HA_ERR_LOCK_DEADLOCK`. The application must re-read the
  affected data and rebuild the transaction from scratch — a plain retry of
  the same writes will conflict again.

- **The catalog in-memory cache is the source of truth for reads; the DB is
  the source of truth for recovery.** At startup, `catalog_init()` rebuilds
  the cache from the DB. DDL updates both atomically (DB write first, then
  cache update under `s_catalog_mu`).
