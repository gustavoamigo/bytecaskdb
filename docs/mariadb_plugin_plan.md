# MariaDB Plugin — Build-Out Plan

> Elaborates a full plan to evolve the Phase-1 POC in `mariadb/` into a
> production-grade `ha_bytecaskdb` storage engine. Reads alongside
> `docs/mariadb_engine_design.md`, `docs/transaction_design.md`,
> `.notes/mariadb_engine_guide.md`, and `.notes/del_range.md`.
>
> **Deliverable for this round: plan only.** No code yet.

---

## 0. Ground rules locked up front

These are non-negotiable constraints the plan is built around.

0. **Secondary index entries: key-only, empty value.** The secondary
   index key is `[0x03 | tid | iid | sec_packed | pk_packed]` with
   an empty value. The PK suffix in the key serves as both the
   uniqueness tie-breaker and the back-pointer to the primary row.
   Phase E.3 (covering indexes) adds covered-column bytes to the
   value — additive, no key-format migration.
1. **One global ByteCaskDB instance per MariaDB server.** Opened once in
   `bytecaskdb_init()`, closed in `bytecaskdb_deinit()`. All schemas,
   tables, indexes, and the catalog live inside this one DB, namespaced
   exclusively by key prefix.
2. **MariaDB-only Layer 2.** The public `Transaction` described in
   `docs/transaction_design.md` is **not** implemented yet and is **not**
   exposed. The plugin ships its own `MariaDBTxn` internal to
   `mariadb/`, built directly on the already-public
   `DB::snapshot()` + `DB::apply_batch_if(WritePlan)` (Layer 1) via the
   C API (`bytecask_c.h`). The public Transaction in the main project
   is deferred indefinitely — nothing in the plugin should depend on it.
3. **`del_range` is planned but not yet in the engine.** The plan
   assumes `DB::del_range(from, to)` (and the matching
   `WritePlan::del_range`) will land per `.notes/del_range.md`. The
   plugin uses it where it is the only reasonable implementation
   (`DROP TABLE`, `TRUNCATE TABLE`, partition/index drop). Until then,
   those operations use a fallback (noted per feature) instead of
   blocking the rest of the roadmap.
4. **Vacuum and `resume()` are deferred to a later milestone**, but the
   plan reserves the integration points (background worker, SQL-visible
   status, degraded-state handling) so that landing them later is a
   drop-in change rather than a rewrite.
5. **No changes to core ByteCaskDB for MariaDB-specific reasons** other
   than additions already motivated by the engine's own roadmap
   (`del_range`, eventually tombstone GC). All MariaDB-specific
   machinery lives under `mariadb/`.

---

## 1. Global key-space layout

The entire MariaDB instance is encoded as a single ordered key space
inside one ByteCaskDB. Every key has a fixed 1-byte **namespace tag**
as its first byte so catalog state, user data, and index data never
collide and a single `del_range` can wipe a table cleanly.

```
byte 0          rest
─────────────────────────────────────────────────────────────────
0x01            catalog entries (see §2)
0x02 + [tid:4]  user-table primary row space
0x03 + [tid:4] + [iid:4]   secondary-index entries (see §5)
0x04            administrative / runtime state (counters, LSN marks)
0x05 … 0xFE     reserved
0xFF            sentinel (upper bound helper)
```

All multi-byte integers encoded in the key are **big-endian** so that
memcmp ordering matches numeric ordering — required for range scans
and `del_range` boundary semantics.

### 1.1 Why a namespace byte rather than a fixed low table_id range

- Uniform prefix length for `del_range(prefix, prefix + 1)` on any
  logical "thing" (a whole table, a whole index, the whole catalog).
- Catalog is well-separated from user data: recovery and tooling can
  iterate `[0x01, 0x02)` without touching user tables.
- Future-proof: adding new namespaces (e.g. a `binlog-pos` key, XA
  prepared-txn registry) is a byte allocation, not a migration.

### 1.2 Primary-row key

```
0x02 | table_id(BE,4) | pk_packed
```

`pk_packed` is MariaDB's internal byte-comparable key, produced by
`key_copy()` over the primary-key column(s). This is the same encoding
as Phase 1 but with the explicit namespace byte prepended.

### 1.3 Secondary-index key (Phase E)

```
0x03 | table_id(BE,4) | index_id(BE,4) | sec_key_packed | pk_packed
```

The PK suffix makes entries unique when the secondary key is
non-unique, and serves as the back-pointer to the primary row. Value
is empty, or carries covered columns if we promote the index to
`HA_KEYREAD_ONLY` (Phase E.2).

### 1.4 Catalog key (Phase A)

```
0x01 | subspace | key
```

With the following subspaces (all single-byte):

| subspace | key format                         | value                                     |
|----------|------------------------------------|-------------------------------------------|
| 0x01     | u8 counter-id                      | u64 BE counter value (table_id, index_id) |
| 0x02     | `db\0table\0` (null-terminated)    | TableMeta (see §2.2)                      |
| 0x03     | table_id(BE,4) \| index_id(BE,4)   | IndexMeta (see §2.2)                      |
| 0x04     | u8 format-version                  | ServerMeta (engine version, schema rev)   |

The `db\0table\0` form normalises what MariaDB passes as
`./<db>/<table>` into a canonical byte sequence: `db`, null,
`table`, null — avoids ambiguity when a database or table name
contains `/`.

---

## 2. Catalog, schema metadata, id allocation

Phase 1 assigns `table_id` from an in-memory counter that resets on
server restart. That is incorrect the moment we persist user data,
because the second run would remap a different table to the same
prefix.

### 2.1 Persistent id allocator

- One catalog key per counter: `0x01|0x01|counter_id`.
- Counter-ids: `1 = table_id`, `2 = index_id`, `3 = xid` (for XA prepare).
- Allocation runs as a conflict-safe CAS using
  `DB::apply_batch_if(snapshot, WritePlan)` against the counter key
  with `ensure_unchanged`. Two concurrent `CREATE TABLE` statements
  are serialised by MDL at the MariaDB layer, so contention is low,
  but the engine stays correct under any interleaving.

### 2.2 Metadata records

```cpp
struct TableMeta {
  uint32_t table_id;
  uint32_t schema_version;   // bumped on ALTER
  std::string full_name;     // "db.table"
  uint16_t reclength;        // for the Phase-1 raw row format
  uint16_t null_bytes;
  uint32_t pk_parts;
  // Per-column: type, length, nullable, charset-id
  std::vector<ColumnMeta> columns;
  // Per-index: index_id, column-ordinals, unique/non-unique
  std::vector<IndexMeta> indexes;
};
```

Persisted in `0x01|0x02|db\0table\0`. Loaded in
`ha_bytecaskdb::open()` and cached on the `TABLE_SHARE` side-object.
Updated in `create()`, `delete_table()`, `rename_table()`, and ALTER.

### 2.3 Replacing the Phase-1 map

- Drop `s_table_ids` / `s_next_table_id` from `bytecaskdb_plugin.cc`.
- Replace with a thin in-memory cache in front of the catalog keyed by
  full name, invalidated by DDL.
- `get_or_assign_table_id()` becomes a catalog read; absent entries
  trigger `CREATE`-time allocation, not implicit on-open allocation.

---

## 3. Phase breakdown

Each phase below lists: scope, new handler methods, ByteCaskDB
primitives used, tests to add, and the concrete acceptance gate
(something you can run and observe). The phases compose; each leaves
the plugin in a usable, testable state.

### Phase A — Persistent catalog + DDL hygiene

**Scope:** replace in-memory table-id map with the catalog in §2.
Handle `create()`, `delete_table()`, and `rename_table()` correctly.
Introduce a versioned row format from day one so that ALTER TABLE
does not break existing rows.

- **Versioned row format** — replace the Phase-1 raw `reclength`
  memcpy with a self-describing envelope:
  - Byte 0: format version (currently `0x01`).
  - Bytes 1–2: `schema_version` (LE u16, matches `TableMeta`).
  - Remaining: column data (Phase A keeps the raw `reclength` payload
    for now, but the envelope is always present).
  On read, if `schema_version` < current, the decoder applies
  defaults for added columns / skips dropped columns. This ensures
  any data written from Phase A onward survives future ALTERs
  without a full rewrite.

- `create()` — CAS-allocate a `table_id`, write `TableMeta`, publish.
- `delete_table()` — **temporary**: best-effort point-delete scan
  because `del_range` is not available yet. Implementation:
  1. Iterate `[0x02|tid, 0x02|tid+1)` using `iter_from`, batch up to
     ~4k keys, apply `Batch` of `del` operations; repeat until empty.
  2. Delete secondary-index key range the same way.
  3. Remove `TableMeta`.
  This is O(rows) in I/O and not atomic — acceptable until Phase G.
- `rename_table()` — rewrite `TableMeta` key under the new name. No
  data movement; `table_id` is stable across renames.
- `open()` — look up `TableMeta` by name; fail cleanly if absent.

**Tests:** MTR suite — `create`, `drop`, `rename`, restart-and-reopen.
Unit tests: catalog encode/decode, id allocator CAS under concurrent
workers.

**Gate:** restart server; created tables survive; dropped tables stay
dropped; renamed tables keep their rows.

### Phase B — Full CRUD on primary key

**Scope:** complete the handler methods MariaDB dispatches for
single-row operations over the primary key.

- `update_row(old, new)` — atomic via `WritePlan`:
  - If PK did not change: one `put(new_pk, new_row)`.
  - If PK changed: `del(old_pk) + put(new_pk, new_row)` in a single
    plan. Guarded by `ensure_absent(new_pk)` when the new PK is not
    the same as the old.
- `delete_row(buf)` — `plan.del(encode_pk(buf))`.
- `index_read_map()` on `table->s->primary_key` — `DB::get()` with PK
  point key.
- `index_next()`, `index_prev()` — forward / reverse iterator bounded
  by `[0x02|tid, 0x02|tid+1)`.
- `index_first()`, `index_last()` — seek to the prefix edge.

**Tests:** MTR `update.test`, `delete.test`, concurrent PK point
lookups, reverse PK range scan.

**Gate:** full CRUD + PK point / range queries produce correct
results, including under moderate concurrency.

### Phase C — MariaDB-internal Layer 2 (`MariaDBTxn`) ✓

**Status: Done.** Implemented with dual-structure write buffer (`ops_` log + `lookup_` map) preserving cross-key causality. 24 smoke tests pass (5 new transaction tests: BEGIN/COMMIT, BEGIN/ROLLBACK, RYOW, dup key within txn, autocommit).

**Scope:** add statement-level and session-level atomicity using a
custom, MariaDB-specific L2 built on top of the Layer 1 C API
(`bytecask_snapshot`, `bytecask_write_plan_*`,
`bytecask_apply_batch_if`). See §4 for the full design.

- New files: `mariadb/bytecaskdb_txn.h`, `.cc`.
- Per-THD instance stored via `thd_get_ha_data` / `thd_set_ha_data`.
- `external_lock(thd, F_RDLCK|F_WRLCK)` — `txn->begin(thd)`.
  Registers with the MariaDB transaction coordinator via
  `trans_register_ha(thd, all=FALSE, hton)` for statement-level and,
  if inside a multi-statement transaction, `all=TRUE` for
  session-level.
- `external_lock(thd, F_UNLCK)` — on autocommit: commit; otherwise
  release read snapshot (if read-only statement).
- `hton->commit(thd, all)` — `txn->commit(all)`.
- `hton->rollback(thd, all)` — `txn->rollback(all)`.
- All writes (`write_row`, `update_row`, `delete_row`) route through
  `txn->buffer_put / buffer_del`, not directly to the DB.
- All reads (`index_read`, `rnd_next`, `rnd_pos`) route through
  `txn->get` / `txn->iter_from` so the write buffer is visible to
  subsequent reads in the same statement (read-your-own-writes).

**Tests:**
- Single-statement multi-row atomicity (INSERT … SELECT that fails
  midway leaves no partial rows).
- Explicit `BEGIN; INSERT; UPDATE; COMMIT;` — all or nothing.
- `BEGIN; INSERT; ROLLBACK;` — nothing persisted.
- Two concurrent sessions updating the same row — one succeeds, the
  other returns `HA_ERR_LOCK_DEADLOCK`.
- RYOW: `INSERT; SELECT` inside same statement sees the inserted row.

**Gate:** standard MTR transaction tests pass; concurrency test
returns deterministic deadlock errors.

### Phase D — Handlerton flags, MVCC posture, store_lock

**Scope:** promote the engine from "no transactions" to "MVCC-aware,
no-lock-manager".

- `hton->flags |= HTON_SUPPORTS_ONLINE_BACKUP` (Phase H).
- `table_flags()` — remove `HA_NO_TRANSACTIONS`; add `HA_MVCC`,
  `HA_NO_LOCK_MANAGER`, `HA_PARTIAL_COLUMN_READ`,
  `HA_PRIMARY_KEY_REQUIRED_FOR_POSITION`,
  `HA_PRIMARY_KEY_REQUIRED_FOR_DELETE`, `HA_BINLOG_ROW_CAPABLE`.
- `store_lock()` — confirmed no-op (already is).
- Correct `start_consistent_snapshot` hook — pin a snapshot inside the
  THD's `MariaDBTxn` for the session.

**Tests:** `mysqldump --single-transaction` on a busy DB returns a
consistent cut; confirmed by inserting between dump start and dump
read and observing the dump is unaffected.

### Phase E — Secondary indexes

**Scope:** full secondary index support behind `WritePlan`.

E.1 — Encoding and write path:

- `[0x03 | tid | iid | sec_packed | pk_packed] → empty`.
- Every `write_row` / `update_row` / `delete_row` atomically emits
  PK row entry + one secondary-index entry per index into a single
  `WritePlan`. `update_row` only touches indexes whose covered
  columns changed.
- Uniqueness: `UNIQUE KEY` emits an `ensure_absent` on the secondary
  prefix (without PK suffix — dedicated "uniqueness probe key") so
  the conflict is detected by the L1 guard, not by a secondary
  lookup beforehand.

E.2 — Read path:

- `index_read_map` / `index_next` / `index_prev`,
  `index_first` / `index_last`, `read_range_first` / `_next`.
- Visibility: Option B from the guide — don't version secondary
  entries; fetch the primary row and rely on `MariaDBTxn`'s snapshot
  for visibility. This is simpler and correct under the SWMR model.
  (Option A is deferred; we don't have per-row version chains and
  we don't want them.)
- Covering indexes: `HA_KEYREAD_ONLY` once we start writing
  covered-column bytes into the secondary value. Initially out of
  scope — add after E.1 + E.2 are stable.

E.3 — Optimiser feedback:

- All statistics are **computed on demand** by walking the in-memory
  radix tree. No counters, no persisted stats, no hotspot keys.
  The tree holds every key in memory, so a `keys_from` prefix walk
  is a pure in-memory DFS — zero disk I/O, microseconds per key.
  The walk is **lock-free**: it operates on an immutable snapshot of
  the tree (`shared_ptr<const EngineState>`), so concurrent writers
  are never blocked. `ANALYZE TABLE` on a 10M-row table is a
  millisecond-scale walk with zero impact on read or write traffic.
  MariaDB caches the result on the `TABLE_SHARE` and refreshes on
  `ANALYZE TABLE` or table reopen, so the walk is infrequent.
- `info(HA_STATUS_VARIABLE)`:
  - `stats.records` — count keys under `[0x02|tid, 0x02|tid+1)`.
  - `stats.mean_rec_length` — sum `KeyDirEntry.value_size` during
    the same walk, divide by count.
  - `stats.data_file_length` — `mean_rec_length * records`
    (approximate; all tables share the same data files).
- `info(HA_STATUS_CONST)`:
  - `rec_per_key[i]` — walk `[0x03|tid|iid, 0x03|tid|iid+1)`,
    count distinct secondary-key prefixes (strip the PK suffix).
    Exact cardinality, computed per index.
- If the on-demand walk ever becomes a bottleneck, add caching as
  a pure optimisation — no design change needed.

**Tests:** MTR `index.test` style coverage — unique violations,
multi-column indexes, range scans, `ORDER BY index`, index-based
`WHERE`.

**Gate:** optimiser picks the right plan on indexed columns;
cardinality plausible.

### Phase F — MVCC isolation tightening

**Scope:** formalise that the engine delivers Snapshot Isolation.
Serializable is a later milestone (same boundary as in
`transaction_design.md`).

- Document the contract: reads inside a txn are SI; writes are
  checked at commit via `apply_batch_if`.
- Map `tx_isolation` to behaviour:
  - `READ COMMITTED` — snapshot is re-acquired at each statement
    start (release + re-begin in `external_lock`).
  - `REPEATABLE READ` — snapshot held for the whole transaction
    (default).
  - `SERIALIZABLE` — Phase F.2: emit `ensure_unchanged` for every
    read key; emit `ensure_range_unchanged` for every scanned range.
    Requires range tracking on the iterators. Can land when the
    public-API Serializable story matures.

**Tests:** write-skew test fails under RR (documented, expected),
passes once SERIALIZABLE is implemented in F.2.

### Phase G — `del_range`-backed DDL

**Scope:** once `.notes/del_range.md` is implemented, use it where
appropriate. This phase depends on that engine feature.

- `delete_table()` — replace the per-row loop with
  `WritePlan plan;
   plan.del_range({0x02 | tid}, {0x02 | tid+1});
   plan.del_range({0x03 | tid}, {0x03 | tid+1});
   plan.del(catalog_key);
   db.apply_batch_if({}, std::move(plan));` — single atomic
  operation, O(1) disk writes.
- `truncate()` — same as `delete_table` but preserves the
  catalog entry.
- `ALTER TABLE … DROP INDEX` — `del_range` the index's key subspace.
- Partition drop (future) — `del_range` each partition's subspace.

**Tests:** drop a 10 M-row table; verify disk I/O is a single entry,
and a subsequent full scan returns empty.

**Gate:** DROP is constant-time on disk regardless of table size.

### Phase H — Vacuum, resume, backup, replication

Everything below is **deferred** until the engine is CRUD-complete and
transaction-correct. The plan reserves the hooks so landing them
later is additive.

H.1 — Vacuum:

- Background thread launched in `bytecaskdb_init()`.
- Tunable via plugin system variables
  (`bytecaskdb_vacuum_interval_seconds`,
  `bytecaskdb_vacuum_min_dead_ratio`).
- Calls `DB::vacuum()` repeatedly; exits on plugin shutdown.
- Status variable `bytecaskdb_last_vacuum_ms`, `_files_reclaimed`.

H.2 — Resume from degraded:

- Wrap every write path (`write_row`, `update_row`, `delete_row`,
  txn commit) in a `DbDegraded` catch. On first catch:
  1. Log to the MariaDB error log.
  2. Attempt `db.resume()` on a background worker (not the
     user thread).
  3. Return `HA_ERR_GENERIC` to the current statement; retry is the
     app's responsibility.
- Expose `bytecaskdb_degraded` (bool) and `bytecaskdb_degraded_reason`
  (string) status variables so DBAs can see it from SQL.

H.3 — Replication hooks:

- `position()` / `rnd_pos()` — already correct (we store the full
  encoded PK in `ref`). Audit for correctness under RBR once we have
  a test harness.

- **Near-term: split-batch 2PC.** Map to ByteCaskDB's existing batch
  format: `prepare()` writes `BulkBegin + entries` and fdatasyncs;
  `commit()` writes `BulkEnd` and fdatasyncs. On crash before
  `commit`, recovery discards the incomplete batch (no BulkEnd) —
  automatic rollback. To support the crash-after-binlog-write case
  (`recover()` / `commit_by_xid()`), add an XID marker entry type
  inside the batch so recovery can identify prepared-but-uncommitted
  batches and MariaDB can re-drive the commit.

  Sufficient for non-replicated deployments and for replication when
  the engine is the only transactional participant.

- **Long-term: outbox-based replication (dedicated design doc).**
  The 2PC exists because there are two durable stores (engine data
  files + binlog file) that must agree. The better architecture is
  one source of truth: the engine writes binlog events atomically
  with the data as outbox entries (e.g., `0x04 | lsn → event`),
  and a background drainer produces standard binlog files from them.
  One fdatasync per transaction instead of 2–3. No 2PC, no crash
  gap, consistency by construction.

  This pattern is proven outside the MySQL ecosystem (TiKV, CockroachDB,
  FoundationDB all use a single log as both WAL and replication stream).
  No MariaDB/MySQL engine has done it — the server assumes synchronous
  binlog files. The integration challenge is MariaDB server internals
  (binlog handler, `SHOW MASTER STATUS`, replica I/O thread), not the
  engine. Worth a dedicated `.notes/` design doc when the time comes.
  The engine-side work (writing outbox entries in the batch) is trivial.

H.4 — Backup:

- `hton->backup_stage` — `HA_BACKUP_STAGE_BLOCK_COMMIT` takes the
  engine's write mutex (via a "quiesce writes" call to the engine —
  another Phase-H engine addition) and captures
  `db.snapshot()`-equivalent marker; the data files are consistent
  on disk because every sealed file is immutable. Backup tool copies
  `*.data` and `*.hint` files.

  Initial (minimal) approach: we do **not** block commits; we rely
  on the append-only property and hint files to make a live copy
  consistent up to the last sealed file, and accept that the active
  file may be truncated on the backup side. Users who need exact
  consistency run `FLUSH TABLES WITH READ LOCK` before the copy.

H.5 — Observability:

- `INFORMATION_SCHEMA.BYTECASKDB_STATS` — one row per table with
  `table_id`, `rows_est`, `live_bytes`, `total_bytes`, `dead_ratio`.
- `INFORMATION_SCHEMA.BYTECASKDB_FILES` — one row per sealed file
  with path, size, live bytes, age.

These require a few read-only accessors on `DB` that do not exist
today but are trivial.

---

## 4. MariaDB-internal L2 Transaction (`MariaDBTxn`) design

Specialised, MariaDB-only. **Does not ship in the public engine
surface.** Entirely under `mariadb/`. Built on the Layer 1 C API.

### 4.1 Requirements the public `Transaction` would not meet

1. MariaDB drives the lifecycle (external_lock, commit, rollback),
   not the caller.
2. Operates on encoded `uchar*` record buffers, not `BytesView`.
3. Tracks MariaDB-specific state: THD pointer, statement vs.
   session scope, isolation level, binlog-order slot.
4. Must support **nested scopes** — a statement rollback inside a
   larger transaction (MariaDB's `savepoint`) — which the public
   `Transaction` does not address.
5. Must be cheap to allocate per-THD and per-statement — potentially
   millions of times over a server's lifetime.

### 4.2 Class sketch

```cpp
// mariadb/bytecaskdb_txn.h
class MariaDBTxn {
public:
  explicit MariaDBTxn(bytecask_db_t* db);
  ~MariaDBTxn();

  // Statement / session scope. `all` follows MariaDB convention:
  //   all == false → statement-level
  //   all == true  → whole session
  void begin_if_needed(THD* thd, bool all);

  // Writes — buffered, not yet applied.
  void buffer_put(const uint8_t* key, size_t klen,
                  const uint8_t* val, size_t vlen);
  void buffer_del(const uint8_t* key, size_t klen);

  // Reads — write-buffer-first, then snapshot.
  // Returns: 1 found, 0 not found, <0 error (error is fatal).
  int get(const uint8_t* key, size_t klen,
          std::vector<uint8_t>& out) const;

  // Snapshot-aware iterator over a prefix range, merged with the
  // write buffer. Used by rnd_next and index_next.
  // The iterator must honour "delete tombstone in buffer suppresses
  // the snapshot's key".
  class PrefixIter {
    /* forward-only, lazy, no heap copies per step */
  };
  PrefixIter iter_prefix(const uint8_t* lo, size_t lolen,
                         const uint8_t* hi, size_t hilen) const;

  // Commit/rollback — apply buffered writes at once.
  int commit(bool all);       // 0=ok, HA_ERR_LOCK_DEADLOCK on conflict
  void rollback(bool all);

  // Savepoints (Phase C.2 — after baseline C works).
  void savepoint_set(const char* name);
  int  savepoint_release(const char* name);   // drops marker
  int  savepoint_rollback(const char* name);  // rewinds buffer

  bool is_active() const noexcept;
  bool has_snapshot() const noexcept;
private:
  bytecask_db_t*                              db_;
  bytecask_snapshot_t*                        snap_{nullptr};
  // Ordered so we can merge-iterate against the snapshot.
  // std::optional<value> — nullopt encodes a buffered tombstone.
  std::map<std::vector<uint8_t>,
           std::optional<std::vector<uint8_t>>> buf_;
  // Savepoint marks: name → size of `buf_` at mark time + extra
  // undo log for entries that were present before the mark.
  struct SavepointMark { /* ... */ };
  std::vector<SavepointMark> savepoints_;
  IsolationMode isolation_;
  bool stmt_active_{false};
  bool sess_active_{false};
};
```

### 4.3 Lifecycle wiring

```
MariaDB entry point            MariaDBTxn action
────────────────────           ─────────────────────────────
external_lock(thd, F_RDLCK)    begin_if_needed(thd, all=depends)
external_lock(thd, F_WRLCK)    begin_if_needed(thd, all=depends)
                               trans_register_ha(thd, false, hton)
                               [if thd is in multi-stmt txn]
                               trans_register_ha(thd, true, hton)

write_row(buf)                 buffer_put(pk(buf), row(buf))
                               + per-secondary-index put (Phase E)
update_row(old,new)            buffer_del + buffer_put
                               (for each changed index, two ops)
delete_row(buf)                buffer_del(pk(buf)) + index dels

external_lock(thd, F_UNLCK)    on autocommit  → commit(all=true)
                               on read-only stmt w/ no multi-stmt
                                              → release snapshot, no-op

hton->commit(thd, all)         commit(all)
hton->rollback(thd, all)       rollback(all)

hton->savepoint_set            savepoint_set(name)
hton->savepoint_rollback       savepoint_rollback(name)
hton->savepoint_release        savepoint_release(name)
```

### 4.4 Commit algorithm (C API form)

```
if buf_ is empty:              return 0

plan = bytecask_write_plan_new_with_snapshot(snap_)   // consumes snap_
for (key, val) in buf_:
    if val.has_value():        bytecask_write_plan_put(plan, key, val)
    else:                      bytecask_write_plan_del(plan, key)

// Phase F.2 — Serializable:
for each read-set key:         bytecask_write_plan_ensure_unchanged(..)
for each read-set range:       bytecask_write_plan_ensure_range_unchanged(..)

rc = bytecask_apply_batch_if(db_, plan, /*sync=*/1)
if rc == 1: return 0
if rc == 0: return HA_ERR_LOCK_DEADLOCK    // conflict
return HA_ERR_GENERIC                       // I/O error
```

`rc == 0` matters — `apply_batch_if` returning false is the engine's
"you need to retry" signal. Map to `HA_ERR_LOCK_DEADLOCK` so MariaDB's
statement retry machinery picks it up (matches MyRocks). Do **not**
treat it as an internal error.

### 4.5 Read-your-own-writes

```
txn.get(key):
  it = buf_.find(key)
  if it != end:
      return it->second.has_value() ? *it->second : NOT_FOUND

  return bytecask_snapshot_get(snap_, key, …)
```

### 4.6 Iterators

`PrefixIter` merge-walks the snapshot iterator and `buf_`'s
lower_bound(lo). At each step it returns the smaller of the two keys
and, on tie, the buffered value (buffer wins). Buffered tombstones
suppress the corresponding snapshot key. Implementation lives in
`bytecaskdb_txn.cc`; no C API change needed since the plan treats the
snapshot iterator's keys/values as opaque byte buffers.

### 4.7 Savepoints (Phase C.2)

Implemented by recording, at savepoint set, the prior value of every
key the txn subsequently modifies inside that scope. On rollback,
restore them; on release, discard. Only needed once we want
`SAVEPOINT` SQL to work.

### 4.8 What's explicitly out

- No pessimistic locking initially. OCC via Layer 1 is the
  correctness mechanism. A pessimistic lock layer (in-memory lock
  table to reduce conflict frequency) can be added later on top of
  OCC without engine or format changes — strictly additive.
- No internal retry loop — MariaDB is the one that retries deadlocked
  statements. Plugin just surfaces the error.
- No thread-local or global txn state outside the THD slot.

---

## 5. Secondary indexes — concrete plan

Keeping this short since the guide covers the MariaDB side.

1. On `create()` — iterate `table->key_info`; for every index with
   `i != table->s->primary_key`:
   - Allocate `index_id` (CAS in the catalog).
   - Record in `TableMeta.indexes`.
2. `encode_sec_key(i, buf, pk_bytes)`:
   - `[0x03 | tid | iid | key_copy(key_info[i], buf) | pk_bytes]`
3. `write_row`:
   - For each non-PK index `i`: `plan.put(sec_key_i, empty)`.
   - For unique indexes: `plan.ensure_absent(dedup_probe_key_i)`
     where the probe key is the secondary without the PK suffix.
4. `update_row`:
   - For each index whose covered columns changed: delete old,
     insert new (with unique-probe for unique indexes).
5. `delete_row`: delete every secondary entry.
6. `index_read`/`_next`/`_prev`: seek on the encoded secondary
   prefix; on each entry, extract `pk_bytes` suffix, fetch primary
   row via `txn.get`, decode into the record buffer.
7. Range-bounded: the caller (MariaDB) gives us `end_range`; stop
   when the secondary key goes past it.
8. Covering: Phase E.3 when we add covered-column bytes into the
   secondary value. Until then, every secondary read re-reads the
   primary — correct, just slower.

---

## 6. DDL correctness with and without `del_range`

| DDL                       | Without `del_range` (Phase A–F)                       | With `del_range` (Phase G)                                                                      |
|---------------------------|-------------------------------------------------------|-------------------------------------------------------------------------------------------------|
| CREATE TABLE              | Allocate table_id, put `TableMeta`, index_ids.         | Same.                                                                                            |
| DROP TABLE                | ~~Paginated point-delete loop~~ **Implemented:** atomic `del_range` + catalog delete in single `WritePlan`. O(1) I/O. | Single `WritePlan` with two `del_range` + `TableMeta` delete. Atomic, O(1) I/O.                  |
| TRUNCATE TABLE            | Same paginated loop, `TableMeta` preserved.            | Two `del_range` calls; O(1) disk.                                                                |
| DROP INDEX                | Paginated point-delete on index prefix.                | Single `del_range` on `[0x03|tid|iid, 0x03|tid|iid+1)`.                                          |
| ALTER TABLE … ADD COLUMN  | Increment `schema_version` in `TableMeta`; decoding branches on version. No row rewrite if column has a default. | Same — no del_range needed.                                                                      |
| ALTER TABLE … DROP COLUMN | Schema-version bump; readers skip the column.          | Same.                                                                                            |
| RENAME TABLE              | Swap `TableMeta` keys. No data movement.               | Same.                                                                                            |

Phase G is a drop-in; the plan never blocks on `del_range` being
there before it.

---

## 7. Concurrency model

| Concern                     | Who handles it                     | Notes                                                               |
|-----------------------------|------------------------------------|---------------------------------------------------------------------|
| MDL (schema locks)          | MariaDB                            | Untouched.                                                          |
| Table / row locks           | No one (HTON_NO_LOCK_MANAGER)      | `store_lock()` is a no-op.                                          |
| Txn isolation               | `MariaDBTxn` + Layer 1 SWMR        | SI by construction.                                                 |
| W-W conflict detection      | `apply_batch_if` implicit W-W      | Each buffered write key is checked against the snapshot sequence.   |
| R-W conflict (Serializable) | `MariaDBTxn` emits `ensure_*`      | Phase F.2.                                                          |
| Group commit                | Already done by engine             | Per-THD `apply_batch_if` calls participate in the engine's group.   |
| Catalog contention          | MariaDB MDL + CAS on counter keys  | DDL is rare; CAS contention is essentially zero.                    |

There is **no new mutex** in the plugin beyond MariaDB's own THR_LOCK
infrastructure and whatever is needed to protect the per-server
in-memory catalog cache.

---

## 8. Engine-side additions the plan assumes

These are tracked as dependencies; none are MariaDB-specific and all
make sense for the engine on their own merits.

| Addition                                   | Required by    | Source of truth                          |
|--------------------------------------------|----------------|------------------------------------------|
| `DB::del_range`, `WritePlan::del_range`    | Phase G        | `.notes/del_range.md`                    |
| Per-table live-row / byte counters on L1   | Phase E / H    | Small extension to the key directory.    |
| `DB` read accessor for file stats          | Phase H.5      | Already exists internally; expose safely.|
| C API wrappers for the above               | All MariaDB    | Extend `include/bytecask_c.h`.           |

No engine feature listed as "current public API" needs to change shape
to fit the plugin — Layer 1 is already sufficient for Phases A–F.

---

## 9. Testing strategy

1. **MariaDB Test Runner (MTR) suite** under `mariadb/mysql-test/`.
   - `ddl.test` — CREATE / DROP / RENAME / ALTER across restart.
   - `crud.test` — INSERT / UPDATE / DELETE / SELECT over PK.
   - `index.test` — secondary index reads, unique constraints.
   - `txn.test` — autocommit, explicit, rollback, nested savepoint.
   - `concurrency.test` — contended writes return deadlock, retry
     succeeds.
   - `recovery.test` — kill -9 mid-workload, restart, verify state.
2. **Unit tests** under `mariadb/tests/` for the pure helpers:
   `key_encoding`, `row_encoding`, `bytecaskdb_txn` buffer semantics,
   iterator merge, catalog encode/decode. Driven by Catch2 and run
   with `xmake run mariadb_unit_tests`.
3. **Fault injection** (reuses engine's fault-injection harness) —
   degraded-state paths once Phase H.2 lands.
4. **Benchmarks** — `sysbench --db-driver=mysql --storage-engine=bytecaskdb`
   point-read, point-write, range, update. Baseline against InnoDB
   and MyRocks on the same box. Target: parity on reads, ≥ 70% on
   writes at the first public milestone.

---

## 10. Risks and open questions

1. **Row format stability.** Phase 1 stores `table->s->reclength`
   bytes raw. That breaks on any ALTER. Phase E will introduce a
   versioned row format; we should do it as part of E.1, not defer
   to Phase H.
3. **Replication strategy.** Near-term: split-batch 2PC (BulkBegin at
   prepare, BulkEnd at commit) works for single-engine deployments.
   Long-term: outbox-based replication eliminates 2PC entirely —
   binlog events stored atomically with data, drained to standard
   binlog files by a background thread. The outbox approach is
   architecturally superior (one source of truth, one fdatasync) but
   requires significant MariaDB server-side integration work.
   Dedicated design doc when the time comes.
4. **Schema catalog migrations.** Once the catalog schema
   (`ServerMeta` format-version) evolves, we need an upgrade path.
   Reserve a `format_version` key now so a future `open()` can
   detect and migrate.
5. **Running two MariaDB instances against the same data dir.** The
   engine does not currently detect or prevent this. A lock file in
   the DB dir is a trivial add but belongs to the engine roadmap.
6. **Character set awareness in key encoding.** MariaDB's
   `key_copy()` already produces byte-comparable keys for most
   charsets, but collation-sensitive indexes need a careful audit
   before Phase E ships.

---

## 11. Milestone order (recommended)

```
A  Persistent catalog + DDL hygiene
B  Full PK CRUD
C  MariaDBTxn (statement-level) — no savepoints
C.2 Savepoints
D  Handlerton MVCC flags, store_lock no-op confirmed,
   start_consistent_snapshot
E.1 Secondary index write path (+ versioned row format)
E.2 Secondary index read path
E.3 Covering indexes
F  Isolation formalised (SI default, READ COMMITTED, REPEATABLE READ)
F.2 SERIALIZABLE (read-set tracking + ensure_* emission)
G  del_range-backed DDL (depends on engine feature)
H.1 Vacuum background worker
H.2 Resume-from-degraded
H.3 Replication / 2PC hooks (revisit engine prepared-write story)
H.4 Backup hooks
H.5 Information_schema views
```

A and B are prerequisites for anything real. C is the correctness
gate for production use. E is the optimiser-relevance gate. G is
nice-to-have until you have tables bigger than a few million rows.
H is operations.

---

## 12. What ships when

| Milestone      | User-visible capability                                         |
|----------------|------------------------------------------------------------------|
| After A + B    | Durable single-threaded CRUD on PK. OK for experiments.         |
| After C        | Multi-statement transactions, autocommit. OK for demos.         |
| After D + E.1  | Optimiser picks indexes. Standard OLTP workloads functional.    |
| After E.2 + F  | Full OLTP; standard isolation levels. First "real user" cut.    |
| After G        | Fast schema ops. Production-shaped.                             |
| After H.*      | Operable (backup, replication posture, degraded recovery).      |

---

## 13. Out of scope for this plan

- MariaDB cluster / Galera integration.
- XA with external resource managers.
- Full-text indexes.
- Spatial indexes.
- Compression at the engine level (ByteCaskDB doesn't compress).
- Row-level locking SQL (`SELECT … FOR UPDATE`) — would require a
  lock manager; conflicts with the lockless design.
