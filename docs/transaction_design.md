# ByteCask Transaction Design

## Purpose

This document describes the design for adding first-class transaction support to ByteCask. It covers the three-layer architecture, the two new `DB` primitives that underpin it, the optional `Transaction` ergonomics layer, isolation models, conflict detection, resource management, and implementation strategy.

Canonical location: `docs/transaction_design.md`.

---

## Background: Where ByteCask stands today

ByteCask already provides atomic multi-operation writes via `apply_batch()`. A `Batch` groups an arbitrary number of `put` and `del` operations wrapped in `BulkBegin`/`BulkEnd` markers that are recovered atomically on restart.

What `apply_batch()` does **not** provide:

- A way to **read before writing** from a consistent view of the database.
- **Conflict detection**: two concurrent callers building batches can overwrite each other's keys without either knowing.
- **Read-your-own-writes**: buffered operations are not visible to reads before commit.
- **Rollback**: a `Batch` that has been applied cannot be undone.

---

## Design: Three layers

The transaction design is explicitly layered. Each layer is independently usable — developers are never forced to adopt the higher layers.

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 2 — Transaction (optional ergonomics)                    │
│  Accumulates writes, read-your-own-writes, iter_from/keys_from  │
│  Calls apply_batch_if() at commit — adds no new mechanism       │
├─────────────────────────────────────────────────────────────────┤
│  Layer 1 — Two new primitives on DB                             │
│  snapshot() → Snapshot          (consistent read-only view)     │
│  apply_batch_if(snap, batch)    (CAS multi-key write)           │
├─────────────────────────────────────────────────────────────────┤
│  Layer 0 — Existing DB                                          │
│  put, del, apply_batch          (no conflict detection — fast)  │
└─────────────────────────────────────────────────────────────────┘
```

A developer who wants exactly one capability can use exactly one layer:

- **Consistent read-only view**: call `db.snapshot()` — no `Transaction` needed.
- **Conflict-safe single-round write**: call `db.snapshot()` then `db.apply_batch_if(snap, batch)` — no `Transaction` needed.
- **Full transaction with buffered read/write and rollback**: use `Transaction`.

`DB` is unchanged from the caller's perspective except for two new methods. There is no mandatory wrapper type, no ownership transfer, and no `txn_mu_` — the existing `write_mu_` inside `apply_batch_if()` provides all serialization needed.

---

## Layer 1: `snapshot()` and `apply_batch_if()`

These are the only two additions to `DB`'s public interface. Everything in the transaction design is built on top of them.

### `snapshot() → Snapshot`

Returns a named, move-only, read-only view of the database as it exists at this instant. Wraps a `shared_ptr<const EngineState>` — the same atomic state pointer readers already use internally — and exposes the full read API.

```cpp
// On DB:
[[nodiscard]] auto snapshot() const -> Snapshot;
```

The `Snapshot` type:

```cpp
export class Snapshot {
public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) noexcept = default;
  Snapshot& operator=(Snapshot&&) noexcept = default;

  // Reads from the frozen state at snapshot time. No mutex acquired.
  [[nodiscard]] auto get(BytesView key, Bytes& out) const -> bool;
  [[nodiscard]] auto contains_key(BytesView key) const -> bool;
  [[nodiscard]] auto iter_from(BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto keys_from(BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

private:
  explicit Snapshot(std::shared_ptr<const EngineState> state);
  std::shared_ptr<const EngineState> state_;
  friend class DB; // DB::snapshot() constructs; apply_batch_if() reads state_
};
```

Implementation is a one-liner: `return Snapshot{state_.load()};`.

**Standalone use** — no `Transaction` involved:

```cpp
auto snap = db.snapshot();
snap.get(key, out);
for (auto& [k, v] : snap.iter_from()) { ... }
// snap destructs: vacuum may now reclaim its files
```

**Why `Snapshot` as a first-class type, not `ReadOptions::snapshot`**:

LevelDB threads snapshots through `ReadOptions` as a raw `const Snapshot*` with manual `ReleaseSnapshot()`. The snapshot is not the object you call — it is a parameter you carry, with manual lifetime. ByteCask's `Snapshot` inverts the model: reads are called directly on it, and lifetime is automatic (value semantics + RAII). It is also composable — `Transaction` is built on top of `Snapshot` rather than reimplementing state capture independently.

### `apply_batch_if(snap, opts, batch)` — compare-and-swap multi-key write

Applies `batch` atomically, but only if no key in the batch was modified since `snap` was taken. The conflict check and the apply both run under `write_mu_`, so they are serialized with all other writers — no external lock needed.

```cpp
// On DB:
// Applies batch atomically iff no key in batch was modified since snap.
// Throws BatchConflict on W-W conflict.
// Throws std::system_error on I/O failure.
// No-op if batch is empty.
void apply_batch_if(const Snapshot& snap, WriteOptions opts, Batch batch);
```

Conflict detection pseudocode (runs under `write_mu_`):

```
lock write_mu_

for each key K in batch:
    snap_entry    = snap.state_->key_dir.find(K)
    current_entry = current state_.load()->key_dir.find(K)

    if !snap_entry and current_entry:
        → conflict (key did not exist at snapshot, now does)
    if snap_entry and current_entry and
       current_entry.sequence != snap_entry.sequence:
        → conflict (key was modified after snapshot was taken)

on first conflict:
    unlock write_mu_
    throw BatchConflict{}

// No conflict — apply the batch (single-op optimization: see below)
write entries, publish new EngineState
unlock write_mu_
```

The `sequence` comparison is an integer comparison between two in-memory fields — no I/O.

**Direct use** — no `Transaction` involved:

```cpp
auto snap = db.snapshot();
Bytes out;
snap.get(to_bytes("balance"), out);
auto new_balance = compute(out);

Batch b;
b.put(to_bytes("balance"), new_balance);
db.apply_batch_if(snap, {}, std::move(b));
// throws BatchConflict if "balance" was written concurrently
```

### Single-operation optimization

Both `apply_batch()` and `apply_batch_if()` wrap entries in `BulkBegin`/`BulkEnd` markers for multi-entry atomic recovery. A single-entry batch does not need these markers: a single data entry is already atomic on disk — if the write is incomplete, the CRC check on recovery rejects it. When `batch.size() == 1`, the marker writes are skipped and the entry is written directly, exactly as `put()` and `del()` do today.

This optimization is **purely internal** — no API change. A caller using `apply_batch_if()` for a single CAS write pays no marker overhead.

| Path | Markers written |
|---|---|
| `put` / `del` | Never (single entry by definition) |
| `apply_batch` with `size() == 1` | No (optimization) |
| `apply_batch` with `size() > 1` | Yes (BulkBegin + BulkEnd required) |
| `apply_batch_if` with `size() == 1` | No (same optimization) |
| `apply_batch_if` with `size() > 1` | Yes |

---

## Layer 2: `Transaction` — optional ergonomics

`Transaction` is an accumulator built entirely on top of `snapshot()` and `apply_batch_if()`. Use it when you need:

- **Read-your-own-writes**: reads within the transaction check the buffered write set first.
- **Iterators over the merged state**: `iter_from()` and `keys_from()` merge the snapshot and the write set.
- **Deferred commit with rollback**: the write set is only materialized to disk on `commit()`.

It adds **no new mechanism**. `commit()` builds a `Batch` from the write set and calls `db_.apply_batch_if(snapshot_, opts, std::move(batch))`. Conflict detection, serialization, and durability all come from `apply_batch_if()`.

`Transaction` holds a non-owning `DB*`. `DB` is not modified to support `Transaction` beyond the two Layer 1 primitives.

### `Transaction` class

```cpp
export class Transaction {
public:
  // Constructed directly from a DB reference.
  // Captures a snapshot at construction time via db.snapshot().
  explicit Transaction(DB& db,
                       IsolationLevel level = IsolationLevel::Snapshot);

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) noexcept = default;
  Transaction& operator=(Transaction&&) noexcept = default;

  // RAII: calls rollback() if not yet committed or rolled back.
  // Moved-from transactions silently no-op.
  ~Transaction();

  // Reads key: checks write set first (read-your-own-writes),
  // then delegates to the held Snapshot.
  // For Serializable: records the key and its snapshot sequence in the read set.
  [[nodiscard]] auto get(BytesView key, Bytes& out) const -> bool;

  // Buffers a put. Not written to disk until commit().
  void put(BytesView key, BytesView value);

  // Buffers a delete. Not written to disk until commit().
  void del(BytesView key);

  // Merge-iterates snapshot + write set (write set wins on overlap).
  // No disk I/O until each entry is dereferenced.
  [[nodiscard]] auto iter_from(BytesView from = {}) const
      -> std::ranges::subrange<TxnEntryIterator, std::default_sentinel_t>;

  // Same merge, keys only — pure in-memory, no disk I/O.
  // Deleted keys in the write set are suppressed.
  [[nodiscard]] auto keys_from(BytesView from = {}) const
      -> std::ranges::subrange<TxnKeyIterator, std::default_sentinel_t>;

  // Builds a Batch from the write set; calls db_.apply_batch_if(snapshot_, batch).
  // For Serializable: runs R-W read-set check first, before calling apply_batch_if.
  // Throws BatchConflict on conflict.
  // Throws std::system_error on I/O failure.
  // No-op if write set is empty.
  void commit();

  // Discards write set. No I/O. Always succeeds.
  void rollback() noexcept;

  [[nodiscard]] auto isolation_level() const noexcept -> IsolationLevel;

private:
  DB* db_;                                         // non-owning; DB must outlive Transaction
  Snapshot snapshot_;
  std::map<Bytes, std::optional<Bytes>> write_set_;
  IsolationLevel level_;
  bool committed_{false};
};
```

**Usage**:

```cpp
auto txn = Transaction{db};
Bytes out;
txn.get(to_bytes("k"), out);          // reads from snapshot
txn.put(to_bytes("k"), new_value);    // buffered
txn.get(to_bytes("k"), out);          // returns new_value (write set wins)
txn.commit();  // calls db.apply_batch_if(snapshot_, {}, batch)
               // throws BatchConflict if "k" was modified concurrently
```

### `commit()` implementation

```cpp
void Transaction::commit() {
  if (write_set_.empty()) { committed_ = true; return; }

  // Serializable only: R-W conflict check before building the batch.
  if (level_ == IsolationLevel::Serializable) {
    auto current = db_->snapshot();
    std::vector<Bytes> conflicts;
    for (const auto& [key, snap_seq] : read_set_) {
      auto entry = current.state_->key_dir.find(BytesView{key});
      std::uint64_t cur_seq = entry ? entry->sequence : 0;
      if (cur_seq > snap_seq) conflicts.push_back(key);
    }
    if (cur_seq > snap_seq) throw BatchConflict{};
  }

  Batch batch;
  for (auto& [key, val] : write_set_) {
    if (val) batch.put(key, *val);
    else     (void)batch.del(key);
  }
  db_->apply_batch_if(snapshot_, WriteOptions{}, std::move(batch));
  committed_ = true;
}
```

No `txn_mu_`. No wrapper class. `apply_batch_if()` handles W-W serialization.

---

## Isolation levels

### AutoCommit (Layer 0 — already exists)

`put`, `del`, `apply_batch` — no conflict detection, maximum throughput.

### Snapshot isolation (Layer 1 and Layer 2)

Available directly via `apply_batch_if()` or via `Transaction`. The snapshot is captured once; at commit, each key in the batch is compared against the current key directory by `sequence`. First-committer-wins.

Prevents: dirty reads, non-repeatable reads, phantom reads (consistent snapshot). Does **not** prevent: write skew.

### Serializable isolation (later milestone — Layer 2 only)

Extends Snapshot isolation by tracking the **read set** inside `Transaction::get()` and performing a **read-write conflict check** in `Transaction::commit()` before calling `apply_batch_if()`. Prevents write skew.

`apply_batch_if()` does not change for Serializable — the R-W check is purely in `Transaction`, using `sequence` fields already present in `KeyDirEntry`. No new engine mechanism is required.

### ReadUncommitted / ReadCommitted

Not meaningful in ByteCask's SWMR model. Writes are only visible after `state_.store()` completes — there are no uncommitted writes visible to other readers. Not worth implementing.

---

## `BatchConflict` error

```cpp
export struct BatchConflict : std::exception {
  const char* what() const noexcept override { return "batch conflict"; }
};
```

Thrown by `apply_batch_if()` (W-W), and therefore by `Transaction::commit()`. For Serializable, `Transaction::commit()` may also throw it before `apply_batch_if()` is called (R-W).

---

## Comparison with RocksDB

| Concept | RocksDB | ByteCask |
|---|---|---|
| CAS write | Not directly available; requires `TransactionDB` | `db.apply_batch_if(snap, opts, batch)` — on plain `DB` |
| Snapshot | `db->GetSnapshot()` + manual `ReleaseSnapshot()` | `db.snapshot()` → `Snapshot` with RAII lifetime |
| Begin transaction | `txn_db->BeginTransaction(...)` — via mandatory wrapper | `Transaction{db, level}` — direct construction; no wrapper |
| Mandatory wrapper | Yes: `TransactionDB::Open()` takes ownership of `DB` | No: `Transaction` holds `DB&`; `DB` is unchanged |
| Read within txn | `txn->Get(read_opts, key, &value)` | `txn.get(key, out)` |
| Write within txn | `txn->Put(key, value)` — deferred | `txn.put(key, value)` — deferred |
| Commit | `txn->Commit()` — conflict check + flush | `txn.commit()` → `db.apply_batch_if()` |
| Rollback | `txn->Rollback()` | `txn.rollback()` — discard write set, no I/O |
| Conflict serialization | Separate lock manager or per-key locks | `write_mu_` inside `apply_batch_if()` — no extra infrastructure |
| File retention | SST files are immutable + ref-counted | `Snapshot` holds `shared_ptr<EngineState>` → vacuum deferred automatically |

The key structural difference: RocksDB's transaction support requires a mandatory `TransactionDB` wrapper that takes ownership of the underlying `DB`. ByteCask's Layer 1 primitives are available directly on `DB`, so any caller can use conflict-safe writes without adopting a wrapper type or changing how they manage the database object.

---

## Write-set-aware iteration (`TxnEntryIterator`, `TxnKeyIterator`)

`Transaction::iter_from()` and `Transaction::keys_from()` both merge the frozen snapshot state with the local write set using a two-pointer merge.

### Merge strategy (shared by both iterators)

```
Advance both:
  A — snapshot iterator (KeyIterator or EntryIterator over snap->key_dir)
  B — write_set_.begin() (std::map<Bytes, std::optional<Bytes>>)

At each step:
  if A.key < B.key  → emit A
  if A.key == B.key → write set wins: emit B if not deleted; skip both
  if A.key > B.key  → emit B if not deleted
  nullopt in write set → deleted; skip
```

### `TxnKeyIterator` — keys only, no disk I/O

Used by `Transaction::keys_from()`. Walks `snap->key_dir` (in-memory) and the write-set keys. No `pread`. Satisfies `std::input_iterator`.

### `TxnEntryIterator` — keys + values

Used by `Transaction::iter_from()`. Same merge over `snap->key_dir`; values are read lazily from `snap->files` on dereference. Satisfies `std::input_iterator`.

For Serializable: each key emitted from the snapshot side is recorded in the transaction's read set as it is yielded by the iterator.

---

## Resource management

### `Transaction` lifetime

`Transaction` holds `DB*` (non-owning). `DB` must outlive all `Transaction`s opened from it — documented precondition. If the destructor runs while the transaction is active (neither committed nor rolled back), it calls `rollback()`. Moved-from transactions silently no-op.

### `Snapshot` lifetime and vacuum interaction

`Snapshot` holds `shared_ptr<const EngineState>`. The reference chain `EngineState → FileRegistry → DataFile` keeps all referenced data files open. Vacuum's `use_count() == 1` guard defers physical deletion until all snapshots referencing a file are destroyed. **No additional code is needed** — this is the same mechanism already protecting in-flight readers.

Practical implication: a long-lived `Transaction` (or standalone `Snapshot`) delays vacuum file reclamation. No correctness risk — only a space efficiency risk.

Mitigation options (to decide during implementation):
- Document the behaviour and leave it to the caller.
- Add a `Transaction::snapshot_age()` accessor so callers can detect and abort long-lived transactions.
- Add a `VacuumOptions::stale_snapshot_warn_ms` threshold.

---

## What Serializable requires

Snapshot isolation prevents dirty reads and phantom reads but allows **write skew**: two concurrent transactions each read a key the other later writes, with neither seeing the other's write.

Serializable requires both the R-W check and the apply to be atomic under `write_mu_`. Performing the R-W check in `Transaction::commit()` before calling `apply_batch_if()` — as a naive design would — is **unsound**: another writer can commit between the check and the apply, invalidating the result.

The correct design is for the R-W check to run *inside* `apply_batch_if()`, under `write_mu_`, alongside the existing W-W check. `Batch` carries the preconditions:

```cpp
batch.put(key, value);                          // write
batch.del(key);                                 // write
batch.ensure_key_unchanged(key);                // precondition: key not modified since snap
batch.ensure_range_unchanged(from, to);         // precondition: no key in [from, to) inserted/removed since snap
```

`apply_batch_if()` then processes all three operation types under `write_mu_`:

```
lock write_mu_

for each ensure_key_unchanged(K):
    snap_seq    = snap.state_->key_dir.find(K) → sequence (0 if absent)
    current_seq = current state_->key_dir.find(K) → sequence (0 if absent)
    if current_seq != snap_seq → R-W conflict on K

for each ensure_range_unchanged(from, to):
    scan current key_dir from lower_bound(from) to to
    if any key has sequence > snap_seq → R-W conflict

for each write key K:
    // W-W check (as today)

if any conflicts: throw BatchConflict{}
apply batch
unlock write_mu_
```

`Transaction::commit()` for Serializable translates its internal state into batch operations:

```cpp
// for each (key, snap_seq) in read_set_points_:  batch.ensure_key_unchanged(key)
// for each (from, to) in read_set_ranges_:        batch.ensure_range_unchanged(from, to)
// for each (key, val) in write_set_:              batch.put / batch.del
db_->apply_batch_if(snapshot_, opts, std::move(batch));
```

No separate overload on `DB` is needed. `Batch` is the extension point — `apply_batch_if()` signature stays `apply_batch_if(snap, opts, batch)` for all isolation levels.

### Range Serializability (later milestone)

Point-read Serializability (above) is straightforward. Range Serializability — detecting phantoms when a transaction iterates a range — requires tracking range predicates in the read set.

`Transaction::iter_from(from)` would record a `RangeRead{from, {}}` into `read_set_ranges_` as it advances; `Transaction::commit()` then emits `batch.ensure_range_unchanged(from, to)` for each tracked range. `apply_batch_if()` performs the bounded scan of the current key directory — O(keys in range), no disk I/O.

`PersistentRadixTree` already has `lower_bound()` and DFS iteration, so the scan is feasible. The new piece is plumbing range records through the iterator into `Transaction::read_set_ranges_`. Until implemented, `Transaction::iter_from()` on a `Serializable` transaction throws `std::logic_error`, documenting the limitation rather than silently degrading the isolation guarantee.

### Engine changes Serializable does NOT require

- No new on-disk format changes.
- No new `EntryType` values.
- No changes to recovery, vacuum, or `EngineState`.
- No per-key locking infrastructure.

---

## Impact on `DB`

Two additions to `DB`'s public interface:

```cpp
// Returns a consistent read-only view of the DB at this instant.
[[nodiscard]] auto snapshot() const -> Snapshot;

// Applies batch atomically iff no key in batch was modified since snap.
// Throws BatchConflict on W-W conflict. Throws std::system_error on I/O failure.
void apply_batch_if(const Snapshot& snap, WriteOptions opts, Batch batch);
```

Everything else — `Transaction`, `TxnEntryIterator`, `TxnKeyIterator`, `IsolationLevel` — lives in `src/transactions.cpp`. `BatchConflict` and `Snapshot` are exported from `bytecask.cppm`.

No `TransactionalDB`. No `txn_mu_`. No `friend` declarations added to `DB`. No internal restructuring of `DB`.

---

## Module exports — net additions only

| Name | Status |
|---|---|
| `DB` | Two additions: `snapshot()`, `apply_batch_if()` |
| `Batch`, `BatchInsert`, `BatchRemove` | Unchanged |
| `ReadOptions`, `WriteOptions`, `Options`, `VacuumOptions` | Unchanged |
| `KeyIterator`, `EntryIterator` | Unchanged |
| `Snapshot` | **New** |
| `BatchConflict` | **New** |
| `Transaction` | **New** |
| `IsolationLevel` | **New** |
| `TxnKeyIterator` | **New** |
| `TxnEntryIterator` | **New** |

`EngineState`, `KeyDirEntry`, and `FileRegistry` remain internal and are not exported.

---

## Interaction with existing `Batch`

`Batch` remains the unconditional atomic write primitive (Layer 0). `apply_batch_if` adds conflict detection on top of the same mechanism. The layers are complementary:

- Use `put` / `del` / `apply_batch` when you are the sole writer or do not need conflict safety.
- Use `db.snapshot()` + `db.apply_batch_if()` when you need conflict-safe CAS with no write-set overhead.
- Use `Transaction` when you need read-your-own-writes, deferred writes, rollback, or iteration over the merged state.

---

## What is genuinely new work

| Item | Scope |
|---|---|
| `IsolationLevel` enum | New export, trivial |
| `BatchConflict` | New export, trivial |
| `DB::snapshot()` | One-liner: `return Snapshot{state_.load()}` |
| `Snapshot` class | New export; wraps `shared_ptr<const EngineState>`; full read-only API |
| `DB::apply_batch_if()` | New method on `DB`: sequence check loop under `write_mu_` + existing apply path |
| Single-op optimization | Skip `BulkBegin`/`BulkEnd` when `batch.size() == 1` (both `apply_batch` and `apply_batch_if`) |
| `Transaction` class | New; holds `DB*` + `Snapshot` + write set; `commit()` calls `apply_batch_if()` |
| W-W conflict check | ~15 lines inside `apply_batch_if()` |
| R-W conflict check (Serializable, point) | ~10 lines in `Transaction::commit()` before `apply_batch_if()` |
| `TxnKeyIterator` | New: two-pointer merge of snapshot `KeyIterator` + write-set keys |
| `TxnEntryIterator` | New: same merge; values read lazily from snapshot files |
| Range R-W tracking for Serializable | Later milestone: iterator records ranges into `Transaction::read_set_` |

---

## Implementation strategy

### Files

| File | Change | Content |
|---|---|---|
| `src/bytecask.cppm` | Modified | Forward-declare `Snapshot` before `DB`; add `snapshot()` and `apply_batch_if()` to `DB`; append full definitions of `BatchConflict`, `Snapshot`, after `DB`; `IsolationLevel`, `TxnKeyIterator`, `TxnEntryIterator`, `Transaction` in `src/transactions.cpp` |
| `src/bytecask.cpp` | Modified | `DB::snapshot()` and `DB::apply_batch_if()` implementations; single-op optimization in both `apply_batch` and `apply_batch_if` |
| `src/transactions.cpp` | **New** | `Snapshot` method bodies; `TxnKeyIterator`; `TxnEntryIterator`; `Transaction` method bodies |
| `xmake.lua` | Modified | `src/bytecask.cpp` → `src/*.cpp` to pick up the new translation unit (all three targets) |
| `tests/bytecask_test.cpp` | Modified | New `[transaction]` TEST_CASEs |

### Declaration order in `bytecask.cppm`

`DB::snapshot()` returns `Snapshot`, which is a new type. A forward declaration before `DB` resolves the dependency — an incomplete return type is valid in a function declaration:

```cpp
// Forward declaration before DB:
export class Snapshot;

// DB class with two new public methods:
export class DB { ... };

// Full definitions in dependency order after DB:
// BatchConflict → Snapshot (full)
```

### Key field name

`KeyDirEntry::sequence` is the actual field. The design doc uses "lsn"/"sequence" as interchangeable concept names; the implementation uses `sequence` throughout.

### Access to `Snapshot::state_` in `apply_batch_if`

`apply_batch_if` is a method on `DB`. `Snapshot` declares `friend class DB`, so `DB`'s methods can read `snap.state_` directly. `Batch::operations_` is already accessible to `DB` via the existing `friend class DB` on `Batch`.

### Test coverage

| Test | What it proves |
|---|---|
| `DB::snapshot()` standalone | Read-only view consistent after concurrent writes |
| `apply_batch_if` no conflict | Applies when no concurrent write occurred |
| `apply_batch_if` W-W conflict | Throws `BatchConflict` when key modified after snapshot |
| Single-op optimization | `apply_batch` / `apply_batch_if` with 1 op writes no markers (verify via `file_stats` byte counts) |
| `Transaction` read-your-own-writes | `txn.put(k, v)` then `txn.get(k)` returns `v` before commit |
| `Transaction` snapshot read consistency | `txn.get(k)` returns snapshot value, not a later committed write |
| `Transaction` W-W conflict | Two concurrent transactions write same key; second commit throws |
| `Transaction` rollback | Write set discarded; key absent after rollback |
| RAII rollback | `Transaction` goes out of scope without `commit()` — no change to DB |
| `keys_from` merge | Deleted-in-write-set keys suppressed; new write-set keys appear |

---

## Open/Closed design principle

The layered design is explicitly closed for modification and open for extension at every seam:

- **`DB`** (`put`, `del`, `apply_batch`) — untouched when transactions are added.
- **`apply_batch_if`** — signature never changes. `Batch` absorbs new operation types (`ensure_key_unchanged`, `ensure_range_unchanged`); `apply_batch_if` processes whatever is in the batch.
- **`Batch`** is the extension point. New precondition types are added to it; nothing upstream changes.
- **`Transaction`** — when range Serializability arrives, `iter_from()` starts populating `read_set_ranges_` and emitting `ensure_range_unchanged` into the batch at commit. No change to `apply_batch_if`, no change to `DB`.

Each milestone is purely additive. No existing call site needs modification when a higher isolation level is introduced.

---

## Open questions

1. **Moved-from `Transaction` destructor**: silently no-op (like `std::unique_ptr`) or assert. `std::unique_ptr` precedent is strong — silent no-op.

2. **Max open duration / observability**: long-lived `Snapshot` or `Transaction` delays vacuum file reclamation. Minimum response is documentation. Future option: `Transaction::snapshot_age()` or `VacuumOptions::stale_snapshot_warn_ms`.

3. **Range Serializability milestone boundary**: `Transaction::iter_from()` on a `Serializable` transaction throws `std::logic_error` until range tracking is implemented. Silently degrading the isolation guarantee without the caller knowing is worse than a clear error.

4. **R-W check scope**: the W-W check in `apply_batch_if` is correct and useful for all callers at Layer 1. The R-W check is intentionally kept in `Transaction::commit()` only — `apply_batch_if` remains a clean CAS primitive. Callers who need R-W protection opt into `Transaction`; callers who only need W-W CAS use `apply_batch_if` directly.
