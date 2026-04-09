# MariaDB Storage Engine Integration Design

## Purpose

This document describes the design for integrating ByteCaskDB as a MariaDB pluggable storage engine (`ha_bytecaskdb`). The integration builds ByteCaskDB as a shared library and loads it into MariaDB at runtime via `INSTALL PLUGIN` — **no MariaDB source tree checkout required**.

Canonical location: `docs/mariadb_engine_design.md`.

---

## Architecture Overview

```
┌───────────────────────────────────────────────────┐
│              MariaDB Server Process                │
│                                                    │
│  SQL Parser → Optimizer → Executor                 │
│       ↕                                            │
│  handler API (store_lock, external_lock,           │
│               write_row, rnd_next, index_read …)   │
│       ↕                                            │
│  ┌─────────────────────────────────────────────┐   │
│  │  ha_bytecaskdb.so (MariaDB storage plugin)   │   │
│  │                                              │   │
│  │  ha_bytecaskdb : public handler               │   │
│  │    ↕                                         │   │
│  │  MariaDB ↔ ByteCaskDB Adapter Layer           │   │
│  │    - Row encoding / decoding                 │   │
│  │    - Key encoding (PK + secondary indexes)   │   │
│  │    - L2 Transaction (internal)               │   │
│  │    ↕                                         │   │
│  │  libbytecask.so (or statically linked)       │   │
│  │    - DB, Snapshot, WritePlan, apply_batch_if  │   │
│  └─────────────────────────────────────────────┘   │
│                                                    │
│  Data directory: /var/lib/mysql/<db>/<table>/       │
│    ├── *.data   (ByteCask data files)              │
│    ├── *.hint   (ByteCask hint files)              │
│    └── .meta    (table metadata)                   │
└───────────────────────────────────────────────────┘
```

### Build Strategy — No MariaDB Clone Required

MariaDB, like MySQL, ships development headers and the `mysql_config` / `mariadb_config` tool. The plugin can be built out-of-tree:

1. **Install MariaDB development packages**: `mariadb-devel` (Fedora) / `libmariadb-dev` (Debian) provides `handler.h`, `sql_class.h`, and the handlerton types.
2. **Build `ha_bytecaskdb.so`** using CMake (or xmake), linking against the MariaDB headers and ByteCaskDB.
3. **Load at runtime**: `INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';`

This is the same model used by third-party engines like TokuDB (before inclusion), Spider, and ColumnStore when developed externally.

```
bytecask/
├── src/                        # existing engine source
├── mariadb/                    # NEW: MariaDB plugin source
│   ├── CMakeLists.txt          # builds ha_bytecaskdb.so
│   ├── ha_bytecaskdb.h
│   ├── ha_bytecaskdb.cc
│   ├── bytecaskdb_plugin.cc    # plugin init/deinit, handlerton
│   ├── row_encoding.h          # MariaDB row ↔ KV encoding
│   ├── row_encoding.cc
│   ├── key_encoding.h          # PK + secondary index key encoding
│   ├── key_encoding.cc
│   ├── bytecaskdb_txn.h        # L2 Transaction (internal to plugin)
│   └── bytecaskdb_txn.cc
├── docs/
│   └── mariadb_engine_design.md  # this file
└── ...
```

The `mariadb/` directory is a self-contained CMake project. It finds MariaDB headers via `mariadb_config --include` and links against the ByteCaskDB static library (built by the existing xmake system or a separate CMake target).

---

## Phased Implementation Plan

### Phase 1 — POC: Plug both projects together (Minimal)

**Goal**: A `ha_bytecaskdb.so` that MariaDB loads, with `CREATE TABLE ... ENGINE=bytecaskdb` creating a ByteCaskDB instance, and basic `INSERT` / `SELECT * FROM` working. Hardcoded single-table, no secondary indexes, no transactions.

**Scope**:
- Plugin skeleton: `handlerton` registration, `create()` → `ha_bytecaskdb` handler.
- `ha_bytecaskdb::create()` — registers the table in the global `DB` instance (one per server), assigns a `table_id`.
- `ha_bytecaskdb::open()` / `close()` — acquire/release a reference to the global `DB` instance.
- `ha_bytecaskdb::write_row()` — encode `[table_id][PK]` from `table->record[0]`, encode full row as value, call `db.put()`.
- `ha_bytecaskdb::rnd_init()` / `rnd_next()` / `rnd_end()` — full table scan via `db.iter_from()`.
- `ha_bytecaskdb::info()` — return estimated row count (key count from key directory).
- `ha_bytecaskdb::position()` / `rnd_pos()` — store/retrieve by PK.
- Row encoding: simple format — serialize all columns into a byte buffer using MariaDB's internal field formats.
- Key encoding: `[table_id][PK]`. Encode PK columns into a comparable byte sequence, prefixed with a 4-byte table identifier.
- No `delete_row`, no `update_row`, no indexes, no transactions.
- `table_flags()`: `HA_NO_TRANSACTIONS | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION`.

**Deliverables**:
- Working `ha_bytecaskdb.so` loadable in MariaDB.
- `CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(100)) ENGINE=bytecaskdb;`
- `INSERT INTO t VALUES (1, 'hello');`
- `SELECT * FROM t;` — returns the inserted row.
- `DROP TABLE t;` — removes the ByteCaskDB data directory.

**Not in Phase 1**: `UPDATE`, `DELETE`, secondary indexes, transactions, `WHERE` clauses on non-PK columns, any MVCC/locking.

### Phase 2 — Basic CRUD + Primary Key Lookups

**Goal**: Full single-row CRUD and PK-based point queries.

**Scope**:
- `ha_bytecaskdb::delete_row()` — `db.del(pk)`.
- `ha_bytecaskdb::update_row()` — `db.del(old_pk)` + `db.put(new_pk, new_row)` via `apply_batch()`.
- `ha_bytecaskdb::index_read()` on the primary key — `db.get(pk)`.
- `ha_bytecaskdb::index_next()` / `index_prev()` — range scan on PK via iterators.
- `store_lock()` — no-op (return `to` unchanged).
- `external_lock()` — no-op initially.

### Phase 3 — L2 Transaction (Internal) + Statement Atomicity

**Goal**: Statement-level atomicity using a custom L2 Transaction built on Layer 1 primitives. This transaction is internal to the plugin — not exposed as a public ByteCaskDB API.

**Scope**:
- `bytecaskdb_txn.h/.cc` — a lightweight `MariaDBTxn` class:
  - Captures a `Snapshot` at statement start (via `external_lock(F_RDLCK/F_WRLCK)`).
  - Buffers writes (`put`/`del`) from `write_row`/`update_row`/`delete_row`.
  - On `external_lock(F_UNLCK)` (autocommit) or explicit `COMMIT`:
    builds a `WritePlan` from the buffered writes and calls `db.apply_batch_if(snap, opts, plan)`.
  - On `ROLLBACK`: discards the write set.
  - Read-your-own-writes: `get()` checks the write buffer first, then falls through to the snapshot.
- Register transaction with MariaDB coordinator via `trans_register_ha()`.
- `hton->commit = bytecaskdb_commit` / `hton->rollback = bytecaskdb_rollback`.
- Isolation: Snapshot isolation (Layer 1's default). Serializable deferred to later.
- Per-THD transaction state stored via `thd_get_ha_data()` / `thd_set_ha_data()`.

**Design detail — Why a separate L2, not the public `Transaction` from `transaction_design.md`**:

The public `Transaction` class (designed but not yet implemented) targets the ByteCaskDB C++ API consumer. The MariaDB integration has different requirements:
- MariaDB drives the lifecycle (`external_lock`, `commit`, `rollback`) — the plugin doesn't control when operations begin/end.
- MariaDB's `handler` API operates on `uchar*` record buffers, not `BytesView` — the adapter must encode/decode.
- The plugin needs to track MariaDB-specific state (THD pointer, table metadata, whether statement or session-level transaction).
- The write buffer stores encoded KV pairs, not the original `(BytesView, BytesView)`.

Building a thin `MariaDBTxn` directly on `snapshot()` + `apply_batch_if()` is simpler and more correct than trying to reuse a general-purpose `Transaction` class that would need MariaDB-specific hooks.

### Phase 4 — Secondary Indexes

**Scope**:
- Key encoding: `[table_id][index_id][sec_key_columns][pk]` → empty value. Same global DB — no cross-table or cross-schema coordination needed.
- Atomic primary + secondary writes via `apply_batch()` / `WritePlan`.
- `index_read()`, `index_next()`, `index_prev()` — seek + iterate on secondary key prefix.
- `index_first()`, `index_last()` — boundary scans.
- `update_row()` — delete old secondary entries, insert new ones for changed indexed columns.
- `info()` — cardinality estimates from key distribution sampling.

### Phase 5 — MVCC + Lockless Architecture

**Scope**:
- Handlerton flags: `HTON_MVCC | HTON_NO_LOCK_MANAGER`.
- `table_flags()`: `HA_MVCC | HA_NO_LOCK_MANAGER`.
- `store_lock()` — confirmed no-op.
- `external_lock()` — snapshot acquisition / release tied to statement boundaries.
- Write-write conflict detection via `apply_batch_if()` returning false → `HA_ERR_LOCK_DEADLOCK`.
- `start_consistent_snapshot()` — for `mysqldump --single-transaction`.

### Phase 6 — Replication + Backup Hooks

**Scope**:
- `position()` / `rnd_pos()` — correct stable row references for RBR.
- 2PC: `hton->prepare` flushes WAL before binlog write.
- XA recovery: `hton->recover`, `commit_by_xid`, `rollback_by_xid`.
- Backup stages: `hton->backup_stage` hooks.

---

## ByteCaskDB Feature Gap Analysis

What ByteCaskDB currently has vs. what the MariaDB integration needs:

### Already Implemented (sufficient for Phase 1-3)

| Feature | Status | Used by |
|---|---|---|
| `DB::open()` | ✅ | `ha_bytecaskdb::create/open` |
| `DB::put()` | ✅ | `write_row` |
| `DB::get()` | ✅ | `index_read` (PK) |
| `DB::del()` | ✅ | `delete_row` |
| `DB::contains_key()` | ✅ | uniqueness checks |
| `DB::apply_batch()` | ✅ | atomic multi-key writes |
| `DB::snapshot()` | ✅ | L2 Transaction reads |
| `DB::apply_batch_if()` | ✅ | L2 Transaction commit (OCC) |
| `WritePlan` with guards | ✅ | conflict detection at commit |
| Forward iterators (`iter_from`, `keys_from`) | ✅ | `rnd_next`, `index_next`, range scans |
| Reverse iterators (`riter_from`, `rkeys_from`) | ✅ | `index_prev`, descending scans |
| Atomic batch writes (`BulkBegin`/`BulkEnd`) | ✅ | multi-row statement atomicity |
| Vacuum | ✅ | background maintenance |
| Parallel recovery | ✅ | fast startup |

### Gaps — Needed for Full Integration

| Gap | Needed by | Severity | Notes |
|---|---|---|---|
| **Shared library build target** | Phase 1 | **Must have** | Currently only builds as object files compiled into each binary. Need a `libbytecask.a` or `.so` target. Straightforward xmake/CMake addition. |
| **C API wrapper** | Phase 1 | **Must have** | MariaDB plugins are C++ but link via C ABI boundaries. ByteCaskDB uses C++23 modules. Need a thin C++ wrapper with stable ABI (no module exports, no `std::span` in signatures). Can use `extern "C"` or a plain header with opaque types. |
| **Row count estimate** | Phase 2+ | **Nice to have** | `ha_bytecaskdb::info()` can return `HA_POS_ERROR` (unknown) for `stats.records` — the optimizer still works, just with less accurate cost estimates. Revisit if optimizer quality matters. |
| **Table-prefixed key encoding** | Phase 1 | **Must have** | Single DB instance per server (like MyRocks). All tables across all schemas share one ByteCaskDB, keys prefixed with `[table_id]`. Enables cross-table and cross-schema transactions via a single `apply_batch_if()` — no 2PC needed. |
| **`upper_bound()` / bounded iteration** | Phase 4 | **Should have** | Secondary index scans need bounded iteration (stop at index prefix boundary). Radix tree has `lower_bound()` and `upper_bound()` — need to verify the iterator stop condition works for prefix-bounded ranges. |
| **Table metadata persistence** | Phase 2 | **Should have** | MariaDB's `.frm` file tracks schema. ByteCaskDB needs to persist table-level metadata (column count, types, index definitions) for recovery. Can store as a reserved key (`__meta__`) or a side file. |
| **Configurable data directory** | Phase 1 | **Nice to have** | MariaDB expects data in `datadir/<db>/<table>/`. `DB::open()` already takes a path — just needs correct wiring. |

### Not Needed (ByteCaskDB is sufficient as-is)

| Concern | Why not needed |
|---|---|
| WAL / write-ahead log | ByteCaskDB's append-only data files with CRC + atomic batch markers provide crash recovery. No separate WAL needed. |
| MVCC versioning | `snapshot()` + `apply_batch_if()` provide snapshot isolation via the SWMR model. No per-row version chain needed — ByteCaskDB's `EngineState` snapshot IS the MVCC mechanism. |
| Per-key locking | OCC via `apply_batch_if()` replaces pessimistic locking. Conflicts return `false` → `HA_ERR_LOCK_DEADLOCK`. |
| Background compaction | Vacuum is caller-driven. MariaDB plugin can run it periodically via a background thread. |

---

## L2 Transaction Design (MariaDB-Internal)

The MariaDB L2 Transaction is a thin adapter between MariaDB's `handler` lifecycle and ByteCaskDB's Layer 1 primitives.

### `MariaDBTxn` Class

```cpp
// mariadb/bytecaskdb_txn.h
class MariaDBTxn {
public:
  explicit MariaDBTxn(bytecask::DB& db);

  // Called from external_lock(F_RDLCK/F_WRLCK) — captures snapshot.
  void begin();

  // Buffered writes — called from write_row/update_row/delete_row.
  void buffer_put(bytecask::BytesView key, bytecask::BytesView value);
  void buffer_del(bytecask::BytesView key);

  // Read-your-own-writes: checks write buffer, then snapshot.
  bool get(bytecask::BytesView key, bytecask::Bytes& out) const;

  // Called from hton->commit. Builds WritePlan, calls apply_batch_if.
  // Returns 0 on success, HA_ERR_LOCK_DEADLOCK on conflict.
  int commit(bool sync);

  // Called from hton->rollback. Discards write buffer.
  void rollback();

  bool is_active() const;

private:
  bytecask::DB& db_;
  std::optional<bytecask::Snapshot> snapshot_;
  // key → value (nullopt = delete)
  std::map<bytecask::Bytes, std::optional<bytecask::Bytes>> write_buffer_;
  bool active_{false};
};
```

### Lifecycle

```
MariaDB                                 MariaDBTxn
────────                                ──────────
external_lock(F_WRLCK)          →       begin()  [snapshot = db.snapshot()]
  write_row(buf)                →       buffer_put(encode_pk(buf), encode_row(buf))
  write_row(buf)                →       buffer_put(...)
  update_row(old, new)          →       buffer_del(encode_pk(old))
                                        buffer_put(encode_pk(new), encode_row(new))
external_lock(F_UNLCK)
  [autocommit]                  →       commit(sync=true)
                                          WritePlan plan;
                                          for each buffered write: plan.put/del(...)
                                          db.apply_batch_if(snapshot, opts, plan)
  [explicit COMMIT]             →       commit(sync=true)
  [ROLLBACK]                    →       rollback()
```

### Conflict Handling

When `apply_batch_if()` returns `false`, `commit()` returns `HA_ERR_LOCK_DEADLOCK`. MariaDB's retry logic (or the application) retries the statement. This is the standard pattern used by MyRocks.

### Per-THD Storage

```cpp
// In ha_bytecaskdb.cc
static MariaDBTxn* get_or_create_txn(THD* thd, bytecask::DB& db) {
  auto* txn = static_cast<MariaDBTxn*>(thd_get_ha_data(thd, bytecaskdb_hton));
  if (!txn) {
    txn = new MariaDBTxn(db);
    thd_set_ha_data(thd, bytecaskdb_hton, txn);
  }
  return txn;
}
```

---

## Row Encoding

MariaDB passes rows as `uchar*` buffers in its internal format (`table->record[0]`). The adapter must encode these into ByteCaskDB's `BytesView` values.

### Strategy: Use MariaDB's Native Row Format

For the POC (Phase 1), store MariaDB's row buffer directly as the value:
- `write_row(buf)`: value = `BytesView{buf, table->s->reclength}`.
- `rnd_next(buf)`: `memcpy(buf, value.data(), table->s->reclength)`.

This is the simplest approach and avoids custom serialization. The downside is the format is MariaDB-specific and tied to the table schema. Acceptable for initial phases.

### Future: Custom Row Encoding

For Phase 4+, a custom encoding may be needed for:
- Secondary index covering columns.
- Schema evolution (adding/dropping columns without rewriting all rows).
- Cross-platform portability.

---

## Key Encoding

### Primary Key Encoding

All keys are prefixed with a 4-byte big-endian `table_id` so that all tables across all schemas share one global DB instance without collisions. The encoding is: `[table_id: 4 bytes][pk_columns: variable]`.

For the POC: use MariaDB's `key_copy()` to extract the PK into a fixed-size buffer, prepend the table_id, and use that as the ByteCaskDB key. MariaDB's key format is already byte-comparable for most types.

### Secondary Index Encoding (Phase 4)

```
Key format:  [table_id: 4 bytes][index_id: 4 bytes][sec_key_columns: variable][pk: variable]
Value:       empty (or minimal unpack info for covering indexes)
```

The `[table_id][index_id]` prefix ensures secondary indexes occupy a separate key range from the primary data, from each other, and from other tables.

---

## Open Questions

1. **C++23 modules across shared library boundary**: MariaDB plugins are compiled separately. ByteCaskDB uses C++23 modules internally. The plugin will need a stable header-based API (no module imports). Options: (a) a thin `bytecask.h` C++ header wrapping the module types, (b) a C API with opaque handles.

2. ~~**One DB per table vs. one DB for the entire server**~~: **Resolved — single DB per server** (same approach as MyRocks). One `DB::open()` at plugin init, shared by all schemas and tables. Keys prefixed with `[table_id]`. This avoids 2PC for cross-table and cross-schema transactions — a single `apply_batch_if()` atomically commits writes across everything.

3. **MariaDB version target**: MariaDB 10.6+ (LTS) or 11.x. The `handler` API is stable across versions. Development headers from the distro package should suffice.

4. **`.frm` file handling**: MariaDB manages `.frm` files for table definitions. The engine should not interfere. On `create()`, just open the ByteCaskDB directory alongside the `.frm`. On `DROP TABLE`, MariaDB removes the `.frm`; we must remove the ByteCaskDB data directory via `delete_table()`.

5. **Thread safety of the global `DB` instance**: All tables across all schemas share one `DB` instance (opened once at plugin init). Multiple handler threads operate on it concurrently. ByteCaskDB's SWMR model allows this (single writer, multiple readers). The L2 Transaction's OCC handles write conflicts.

---

## Reference

- `.notes/mariadb_engine_guide.md` — detailed MariaDB handler API reference.
- `docs/transaction_design.md` — Layer 1/2/3 transaction architecture.
- `docs/bytecask_design.md` — ByteCaskDB core design and concurrency model.
- MariaDB `storage/example/` — minimal engine skeleton.
- MariaDB `storage/rocksdb/` (MyRocks) — production KV engine reference.
