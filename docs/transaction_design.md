# ByteCaskDB Transaction Design

## Purpose

This document describes the design for adding first-class transaction support to ByteCaskDB. It covers the three-layer architecture, the two new `DB` primitives that underpin it, the optional `Transaction` ergonomics layer, isolation models, conflict detection, resource management, and implementation strategy.

Canonical location: `docs/transaction_design.md`.

---

## Background: Where ByteCaskDB stands today

ByteCaskDB already provides atomic multi-operation writes via `apply_batch()`. A `Batch` groups an arbitrary number of `put` and `del` operations wrapped in `BulkBegin`/`BulkEnd` markers that are recovered atomically on restart.

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
│  apply_batch_if(snap, plan)     (CAS multi-key write)           │
├─────────────────────────────────────────────────────────────────┤
│  Layer 0 — Existing DB                                          │
│  put, del, apply_batch(Batch)   (no conflict detection — fast)  │
└─────────────────────────────────────────────────────────────────┘
```

A developer who wants exactly one capability can use exactly one layer:

- **Consistent read-only view**: call `db.snapshot()` — no `Transaction` needed.
- **Conflict-safe single-round write**: call `db.snapshot()` then `db.apply_batch_if(snap, plan)` — no `Transaction` needed.
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

LevelDB threads snapshots through `ReadOptions` as a raw `const Snapshot*` with manual `ReleaseSnapshot()`. The snapshot is not the object you call — it is a parameter you carry, with manual lifetime. ByteCaskDB's `Snapshot` inverts the model: reads are called directly on it, and lifetime is automatic (value semantics + RAII). It is also composable — `Transaction` is built on top of `Snapshot` rather than reimplementing state capture independently.

### `apply_batch_if(snap, opts, plan)` — compare-and-swap multi-key write

Applies the writes in `plan` atomically, but only if all guards in `plan` pass and no written key was modified since `snap` was taken. Guard checks and the apply all run under `write_mu_`, so they are serialized with all other writers — no external lock needed.

```cpp
// On DB:
// Applies plan atomically iff all guards pass and no written key was modified since snap.
// Returns true if committed, false on conflict (W-W or guard violation).
// Throws std::system_error on I/O failure.
// Returns true (no-op) if plan has no writes and no guards.
[[nodiscard]] auto apply_batch_if(const Snapshot& snap, WriteOptions opts, WritePlan plan) -> bool;
```

`WritePlan` is the type consumed by `apply_batch_if`. It separates cleanly from `Batch`: `Batch` is the unconditional bulk-write primitive (Layer 0); `WritePlan` is the conditional, snapshot-relative primitive (Layer 1). See the **`WritePlan`** section below for the full type definition.

Conflict detection pseudocode (runs under `write_mu_`):

```
lock write_mu_

// 1. Evaluate point guards
for each guard G in plan:
    current_entry = current state_->key_dir.find(G.key)
    snap_entry    = snap.state_->key_dir.find(G.key)

    if G.precondition == MustExist and !current_entry:
        → conflict (key must exist but is absent)
    if G.precondition == MustBeAbsent and current_entry:
        → conflict (key must be absent but exists)
    if G.precondition == MustBeUnchanged:
        snap_seq    = snap_entry ? snap_entry->sequence : 0
        current_seq = current_entry ? current_entry->sequence : 0
        if current_seq != snap_seq → conflict

// 2. Evaluate range guards
for each range guard R in plan:
    scan current key_dir from lower_bound(R.from) to R.to
    for each key K in range:
        snap_entry = snap.state_->key_dir.find(K)
        snap_seq   = snap_entry ? snap_entry->sequence : 0
        if K.sequence > snap_seq → conflict (key inserted or modified since snapshot)
    scan snap key_dir from lower_bound(R.from) to R.to
    for each key K in snap range:
        if !current key_dir contains K → conflict (key deleted since snapshot)

// 3. Implicit W-W check on all write keys
for each write key K in plan:
    snap_entry    = snap.state_->key_dir.find(K)
    current_entry = current state_->key_dir.find(K)

    if !snap_entry and current_entry:
        → conflict (key did not exist at snapshot, now does)
    if snap_entry and current_entry and
       current_entry.sequence != snap_entry.sequence:
        → conflict (key was modified after snapshot was taken)

on first conflict:
    unlock write_mu_
    return false

// No conflict — apply writes, publish new EngineState
write entries, publish new EngineState
unlock write_mu_
return true
```

The `sequence` comparison is an integer comparison between two in-memory fields — no I/O.

**Direct use** — no `Transaction` involved:

```cpp
auto snap = db.snapshot();
Bytes out;
snap.get(to_bytes("balance"), out);
auto new_balance = compute(out);

WritePlan plan;
plan.ensure_unchanged(to_bytes("balance"));
plan.put(to_bytes("balance"), new_balance);
auto ok = db.apply_batch_if(snap, {}, std::move(plan));
// ok == false if "balance" was written concurrently
```

### Single-operation optimization

Both `apply_batch()` and `apply_batch_if()` wrap entries in `BulkBegin`/`BulkEnd` markers for multi-entry atomic recovery. A single-entry write does not need these markers: a single data entry is already atomic on disk — if the write is incomplete, the CRC check on recovery rejects it. When the write set contains exactly one entry, the marker writes are skipped and the entry is written directly, exactly as `put()` and `del()` do today.

This optimization is **purely internal** — no API change. A caller using `apply_batch_if()` for a single CAS write pays no marker overhead.

| Path | Markers written |
|---|---|
| `put` / `del` | Never (single entry by definition) |
| `apply_batch` with `size() == 1` | No (optimization) |
| `apply_batch` with `size() > 1` | Yes (BulkBegin + BulkEnd required) |
| `apply_batch_if` with 1 write | No (same optimization) |
| `apply_batch_if` with > 1 writes | Yes |

---

## `WritePlan` — the `apply_batch_if` vocabulary type

`WritePlan` is the type consumed exclusively by `apply_batch_if`. It carries both **writes** (`put`, `del`) and **guards** (`ensure_present`, `ensure_absent`, `ensure_unchanged`, `ensure_range_unchanged`). Guards are preconditions checked atomically under `write_mu_` at commit time — if any guard fails, the entire plan is rejected and `apply_batch_if` returns `false`.

### Why a separate type from `Batch`

`Batch` is the unconditional atomic write primitive for `apply_batch` (Layer 0). Its vocabulary is `put` and `del` — nothing else. Mixing conditional guards into `Batch` would create a type that is only half-valid depending on which method consumes it: `apply_batch` would need to reject guards at runtime, and the type system would not prevent the mistake.

`WritePlan` is a distinct type that carries the full conditional vocabulary. Compile-time separation:

- You cannot pass a `WritePlan` to `apply_batch` — wrong type.
- You cannot pass a `Batch` to `apply_batch_if` — wrong type.
- `Batch` stays dead simple for the fast unconditional path.

### User-facing API

The API decomposes writes and preconditions into orthogonal primitives. Users compose them freely to express any transactional intent.

```cpp
export class WritePlan {
public:
  WritePlan() = default;
  WritePlan(const WritePlan&) = delete;
  WritePlan& operator=(const WritePlan&) = delete;
  WritePlan(WritePlan&&) noexcept = default;
  WritePlan& operator=(WritePlan&&) noexcept = default;

  // --- Writes (unconditional) ---

  // Upsert: write key regardless of current state.
  void put(BytesView key, BytesView value);

  // Delete: remove key regardless of current state.
  void del(BytesView key);

  // --- Point guards (preconditions checked at commit under write_mu_) ---

  // Conflict if key is absent in the current state at commit time.
  void ensure_present(BytesView key);

  // Conflict if key is present in the current state at commit time.
  void ensure_absent(BytesView key);

  // Conflict if key's sequence differs from the snapshot
  // (key was written, created, or deleted since snapshot).
  void ensure_unchanged(BytesView key);

  // --- Range guards ---

  // Conflict if any key in [from, to) was inserted, modified,
  // or deleted since the snapshot. The range is half-open:
  // `from` is inclusive, `to` is exclusive — same convention as
  // std::ranges and iterator pairs throughout the codebase.
  void ensure_range_unchanged(BytesView from, BytesView to);

  [[nodiscard]] auto empty() const noexcept -> bool;

private:
  // Per-key merged representation (see Internal Representation below).
  struct KeyAction { ... };
  std::map<Bytes, KeyAction> actions_;

  struct RangeGuard { Bytes from; Bytes to; };
  std::vector<RangeGuard> range_guards_;

  friend class DB;
};
```

### Composing intent from primitives

The two write verbs (`put`, `del`) and four guard primitives compose freely to express any standard KV transactional operation:

| Intent | Calls |
|---|---|
| Upsert (unconditional) | `put(k, v)` |
| INSERT (fail if exists) | `ensure_absent(k)` + `put(k, v)` |
| UPDATE (fail if missing) | `ensure_present(k)` + `put(k, v)` |
| DELETE (fail if missing) | `ensure_present(k)` + `del(k)` |
| Unconditional delete | `del(k)` |
| Read guard (no write) | `ensure_unchanged(k)` |
| Range read guard | `ensure_range_unchanged(from, to)` |
| Unique constraint | `ensure_absent(k)` (or `ensure_range_unchanged` for prefix uniqueness) |

### Internal representation

Guards and writes on the same key are merged into a single `KeyAction`. The engine processes one flat map — no pairing logic needed at commit time.

```cpp
struct KeyAction {
  enum class Precondition { None, MustExist, MustBeAbsent, MustBeUnchanged };
  enum class Write { None, Put, Del };

  Precondition precondition{Precondition::None};
  Write write{Write::None};
  Bytes value;  // meaningful only when write == Put
};
```

When a user calls:
```cpp
plan.ensure_present(key);
plan.del(key);
```

The `WritePlan` merges this into `KeyAction{MustExist, Del, {}}` — a single entry, checked and applied in one pass.

**Build-time validation**: calling contradictory guards on the same key (e.g. `ensure_present(k)` then `ensure_absent(k)`) throws `std::logic_error` immediately at build time — not deferred to commit.

### Completeness verification

This section verifies that the `WritePlan` vocabulary is sufficient to implement all standard W-W and R-W transactional guarantees for a KV store.

#### W-W conflict patterns (Snapshot Isolation)

**1. Blind write conflict — two writers update the same key**

Writer A: `put(k, v1)` — Writer B: `put(k, v2)`.

`apply_batch_if` performs an implicit W-W check on every write key: it compares the snapshot sequence against the current sequence. Second committer sees mismatch → `false`. No guard needed — built into `apply_batch_if` for all write keys. ✅

**2. Delete-delete conflict**

Writer A: `del(k)` — Writer B: `del(k)`.

Same implicit W-W check on the `del` key. ✅

**3. Insert-insert conflict — two writers insert the same new key**

Writer A: `ensure_absent(k) + put(k, v1)` — Writer B: `ensure_absent(k) + put(k, v2)`.

First committer succeeds. Second: `ensure_absent` fails (key now exists). The implicit W-W check would also catch it, but the guard makes the intent explicit. ✅

**4. Conditional update — update only if key exists**

`ensure_present(k) + put(k, v)`.

If key was deleted between snapshot and commit → `ensure_present` fails. ✅

**5. Conditional delete — delete only if key exists**

`ensure_present(k) + del(k)`.

If key was already deleted → `ensure_present` fails. ✅

#### R-W conflict patterns (Serializable)

**6. Write skew (two on-call doctors)**

Both transactions read `doctor_1` and `doctor_2` (both on-call). Each removes themselves.

```cpp
// Txn A:
plan.ensure_unchanged(to_bytes("doctor_2"));  // guard the key we read but don't write
plan.del(to_bytes("doctor_1"));

// Txn B:
plan.ensure_unchanged(to_bytes("doctor_1"));  // guard the key we read but don't write
plan.del(to_bytes("doctor_2"));
```

First committer succeeds. Second: `ensure_unchanged` detects the other doctor's key was modified. ✅

**7. Read-then-write on different keys with external dependency**

Transaction reads `exchange_rate`, `balance_a`, `balance_b`; writes adjusted balances.

```cpp
auto snap = db.snapshot();
Bytes rate, a, b;
snap.get(to_bytes("exchange_rate"), rate);
snap.get(to_bytes("balance_a"), a);
snap.get(to_bytes("balance_b"), b);

WritePlan plan;
plan.ensure_unchanged(to_bytes("exchange_rate"));  // guard read dependency
plan.put(to_bytes("balance_a"), new_a);
plan.put(to_bytes("balance_b"), new_b);
db.apply_batch_if(snap, {}, std::move(plan));
```

If `exchange_rate` was modified concurrently → conflict. The written keys (`balance_a`, `balance_b`) are covered by the implicit W-W check. ✅

**8. Phantom prevention — range read then write**

Transaction iterates `[user:100:, user:200:)`, computes aggregate, writes result.

```cpp
WritePlan plan;
plan.ensure_range_unchanged(to_bytes("user:100:"), to_bytes("user:200:"));
plan.put(to_bytes("aggregate"), result);
db.apply_batch_if(snap, {}, std::move(plan));
```

Any insert, delete, or modification in the range since snapshot → conflict. ✅

**9. Unique constraint via range guard**

Insert `user:150:` only if the key prefix range is empty.

```cpp
WritePlan plan;
plan.ensure_range_unchanged(to_bytes("user:150:"), to_bytes("user:151:"));
plan.ensure_absent(to_bytes("user:150:"));
plan.put(to_bytes("user:150:"), value);
db.apply_batch_if(snap, {}, std::move(plan));
```

`ensure_range_unchanged` catches concurrent inserts anywhere in the prefix. `ensure_absent` catches pre-existing key. ✅

**10. Read-only transaction validation — no writes**

Verify that multiple keys were from a consistent cut.

```cpp
WritePlan plan;
plan.ensure_unchanged(to_bytes("k1"));
plan.ensure_unchanged(to_bytes("k2"));
plan.ensure_unchanged(to_bytes("k3"));
db.apply_batch_if(snap, {}, std::move(plan));
// no writes — guards only. No disk I/O on success.
```

✅

#### Edge cases

**11. Guard on a key absent at snapshot time and still absent**: `ensure_unchanged(k)` — snapshot sequence = 0, current sequence = 0. No conflict. ✅

**12. Guard on a key absent at snapshot time, now present**: `ensure_unchanged(k)` — snapshot sequence = 0, current sequence > 0. Conflict. ✅

**13. Guard on a key present at snapshot time, now deleted**: `ensure_unchanged(k)` — snapshot sequence N, current absent. Conflict. ✅

**14. Multiple compatible guards on same key**: `ensure_present(k) + ensure_unchanged(k)` — merged into `KeyAction` with strongest precondition. ✅

**15. Contradictory guards**: `ensure_present(k) + ensure_absent(k)` — throws `std::logic_error` at build time. ✅

#### Completeness matrix

| Guarantee | Mechanism | Sufficient? |
|---|---|---|
| W-W on written keys | Implicit in `apply_batch_if` (sequence check on all writes) | ✅ |
| W-W conditional insert | `ensure_absent` + `put` | ✅ |
| W-W conditional update | `ensure_present` + `put` | ✅ |
| W-W conditional delete | `ensure_present` + `del` | ✅ |
| R-W point (write skew) | `ensure_unchanged` on read-but-not-written keys | ✅ |
| R-W range (phantoms) | `ensure_range_unchanged` | ✅ |
| Read-only validation | Guards only, no writes | ✅ |
| Unique constraint | `ensure_absent` or `ensure_range_unchanged` | ✅ |

The six primitives cover all standard W-W and R-W transactional guarantees for a KV store. The snapshot provides the read context; `ensure_unchanged` is strictly stronger than a value-level check because it detects *any* write to the key, even an ABA write that restores the same value.

---

## Layer 2: `Transaction` — optional ergonomics

`Transaction` is an accumulator built entirely on top of `snapshot()` and `apply_batch_if()`. Use it when you need:

- **Read-your-own-writes**: reads within the transaction check the buffered write set first.
- **Iterators over the merged state**: `iter_from()` and `keys_from()` merge the snapshot and the write set.
- **Deferred commit with rollback**: the write set is only materialized to disk on `commit()`.

It adds **no new mechanism**. `commit()` builds a `WritePlan` from the write set (and read set for Serializable) and calls `db_.apply_batch_if(snapshot_, opts, std::move(plan))`. Conflict detection, serialization, and durability all come from `apply_batch_if()`.

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

  // Builds a WritePlan from the write set; calls db_.apply_batch_if(snapshot_, plan).
  // For Serializable: emits ensure_unchanged / ensure_range_unchanged guards
  // into the WritePlan for read-set keys, checked atomically under write_mu_.
  // Returns true if committed, false on conflict.
  // Throws std::system_error on I/O failure.
  // Returns true (no-op) if write set is empty.
  [[nodiscard]] auto commit() -> bool;

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
auto ok = txn.commit();  // calls db.apply_batch_if(snapshot_, {}, plan)
                         // ok == false if "k" was modified concurrently
```

### `commit()` implementation

```cpp
auto Transaction::commit() -> bool {
  if (write_set_.empty() && read_set_.empty()) { committed_ = true; return true; }

  WritePlan plan;

  // Serializable: emit guards for read-set keys (R-W protection).
  if (level_ == IsolationLevel::Serializable) {
    for (const auto& [key, _] : read_set_points_)
      plan.ensure_unchanged(key);
    for (const auto& [from, to] : read_set_ranges_)
      plan.ensure_range_unchanged(from, to);
  }

  // Emit writes.
  for (auto& [key, val] : write_set_) {
    if (val) plan.put(key, *val);
    else     plan.del(key);
  }

  if (!db_->apply_batch_if(snapshot_, WriteOptions{}, std::move(plan)))
    return false;
  committed_ = true;
  return true;
}
```

No `txn_mu_`. No wrapper class. `apply_batch_if()` handles all conflict checks atomically under `write_mu_` — both the explicit guards and the implicit W-W check on write keys. Conflicts are signalled by return value (`false`), not exceptions — they are expected outcomes in concurrent workloads, not errors. I/O failures remain exceptions (`std::system_error`).

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

Not meaningful in ByteCaskDB's SWMR model. Writes are only visible after `state_.store()` completes — there are no uncommitted writes visible to other readers. Not worth implementing.

---

## Conflict signalling

Conflicts are expected outcomes in concurrent workloads — they are not errors. Both `apply_batch_if()` and `Transaction::commit()` return `bool`: `true` if committed, `false` on conflict (W-W or guard violation). The caller's only response is retry or abort.

I/O failures remain exceptions (`std::system_error`) — those are genuinely unexpected.

This follows C++ Core Guidelines E.3 ("Use exceptions for error handling only") and avoids exception-based retry loops:

```cpp
// Clean retry loop — no try/catch for expected control flow.
while (true) {
  auto snap = db.snapshot();
  Bytes out;
  snap.get(to_bytes("counter"), out);
  WritePlan plan;
  plan.ensure_unchanged(to_bytes("counter"));
  plan.put(to_bytes("counter"), increment(out));
  if (db.apply_batch_if(snap, {}, std::move(plan))) break;
}
```

---

## Comparison with RocksDB

| Concept | RocksDB | ByteCaskDB |
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

The key structural difference: RocksDB's transaction support requires a mandatory `TransactionDB` wrapper that takes ownership of the underlying `DB`. ByteCaskDB's Layer 1 primitives are available directly on `DB`, so any caller can use conflict-safe writes without adopting a wrapper type or changing how they manage the database object.

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

Serializable requires both the R-W check and the apply to be atomic under `write_mu_`. Performing the R-W check outside `write_mu_` — as a naive design would — is **unsound**: another writer can commit between the check and the apply, invalidating the result.

The correct design is for the R-W check to run *inside* `apply_batch_if()`, under `write_mu_`, alongside the existing W-W check. `WritePlan` carries both writes and guards, and `apply_batch_if` processes all of them atomically:

```
lock write_mu_

// 1. Point guards: ensure_present, ensure_absent, ensure_unchanged
for each guard → check against current state

// 2. Range guards: ensure_range_unchanged
for each range → scan current key_dir, compare sequences

// 3. Implicit W-W check on all write keys
for each write key → compare snapshot sequence vs current sequence

if any check fails: return false
apply writes
unlock write_mu_
```

`Transaction::commit()` for Serializable translates its internal state into `WritePlan` operations:

```cpp
// for each key in read_set_points_:    plan.ensure_unchanged(key)
// for each (from, to) in read_set_ranges_: plan.ensure_range_unchanged(from, to)
// for each (key, val) in write_set_:   plan.put / plan.del
db_->apply_batch_if(snapshot_, opts, std::move(plan));
```

No separate overload on `DB` is needed. `WritePlan` is the extension point — `apply_batch_if` signature stays `apply_batch_if(snap, opts, plan)` for all isolation levels.

### Range Serializability (later milestone)

Point-read Serializability (above) is straightforward. Range Serializability — detecting phantoms when a transaction iterates a range — requires tracking range predicates in the read set.

`Transaction::iter_from(from)` would record a `RangeRead{from, {}}` into `read_set_ranges_` as it advances; `Transaction::commit()` then emits `plan.ensure_range_unchanged(from, to)` for each tracked range. `apply_batch_if()` performs the bounded scan of the current key directory — O(keys in range), no disk I/O.

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

// Applies plan atomically iff all guards pass and no written key was modified since snap.
// Returns true if committed, false on conflict. Throws std::system_error on I/O failure.
[[nodiscard]] auto apply_batch_if(const Snapshot& snap, WriteOptions opts, WritePlan plan) -> bool;
```

Everything else — `Transaction`, `TxnEntryIterator`, `TxnKeyIterator`, `IsolationLevel` — lives in `src/transactions.cpp`. `Snapshot` and `WritePlan` are exported from `bytecask.cppm`.

No `TransactionalDB`. No `txn_mu_`. No `friend` declarations added to `DB`. No internal restructuring of `DB`.

---

## Module exports — net additions only

| Name | Status |
|---|---|
| `DB` | Two additions: `snapshot()`, `apply_batch_if()` |
| `Batch`, `BatchInsert`, `BatchRemove` | Unchanged (Layer 0 only) |
| `ReadOptions`, `WriteOptions`, `Options`, `VacuumOptions` | Unchanged |
| `KeyIterator`, `EntryIterator` | Unchanged |
| `WritePlan` | **New** — conditional write + guard vocabulary for `apply_batch_if` |
| `Snapshot` | **New** |
| `Transaction` | **New** |
| `IsolationLevel` | **New** |
| `TxnKeyIterator` | **New** |
| `TxnEntryIterator` | **New** |

`EngineState`, `KeyDirEntry`, and `FileRegistry` remain internal and are not exported.

---

## Interaction with existing `Batch`

`Batch` remains the unconditional atomic write primitive (Layer 0). It carries only `put` and `del` — no guards, no snapshot-relative semantics. `apply_batch(Batch)` is the fast path for callers who do not need conflict detection.

`WritePlan` is the conditional, snapshot-relative primitive (Layer 1). It carries writes *and* guards. `apply_batch_if(Snapshot, WritePlan)` checks all guards and W-W constraints atomically before applying.

The two types are distinct — you cannot pass one where the other is expected. The layers are complementary:

- Use `put` / `del` / `apply_batch(Batch)` when you are the sole writer or do not need conflict safety.
- Use `db.snapshot()` + `db.apply_batch_if(snap, plan)` when you need conflict-safe conditional writes.
- Use `Transaction` when you need read-your-own-writes, deferred writes, rollback, or iteration over the merged state.

---

## What is genuinely new work

| Item | Scope |
|---|---|
| `IsolationLevel` enum | New export, trivial |
| `DB::snapshot()` | One-liner: `return Snapshot{state_.load()}` |
| `Snapshot` class | New export; wraps `shared_ptr<const EngineState>`; full read-only API |
| `WritePlan` class | New export; per-key `KeyAction` map + range guards; build-time validation |
| `DB::apply_batch_if()` | New method on `DB`: guard check + W-W sequence check under `write_mu_` + existing apply path |
| Single-op optimization | Skip `BulkBegin`/`BulkEnd` when write count == 1 (both `apply_batch` and `apply_batch_if`) |
| `Transaction` class | New; holds `DB*` + `Snapshot` + write set; `commit()` builds `WritePlan` and calls `apply_batch_if()` |
| W-W conflict check | ~15 lines inside `apply_batch_if()` |
| Guard checks (point + range) | ~25 lines inside `apply_batch_if()`, under `write_mu_` |
| `TxnKeyIterator` | New: two-pointer merge of snapshot `KeyIterator` + write-set keys |
| `TxnEntryIterator` | New: same merge; values read lazily from snapshot files |
| Range R-W tracking for Serializable | Later milestone: iterator records ranges into `Transaction::read_set_ranges_` |

---

## Implementation strategy

### Files

| File | Change | Content |
|---|---|---|
| `src/bytecask.cppm` | Modified | Forward-declare `Snapshot` before `DB`; add `snapshot()` and `apply_batch_if()` to `DB`; append full definitions of `WritePlan`, `Snapshot` after `DB`; `IsolationLevel`, `TxnKeyIterator`, `TxnEntryIterator`, `Transaction` in `src/transactions.cpp` |
| `src/bytecask.cpp` | Modified | `DB::snapshot()` and `DB::apply_batch_if()` implementations; single-op optimization in both `apply_batch` and `apply_batch_if` |
| `src/transactions.cpp` | **New** | `Snapshot` method bodies; `TxnKeyIterator`; `TxnEntryIterator`; `Transaction` method bodies |
| `xmake.lua` | Modified | `src/bytecask.cpp` → `src/*.cpp` to pick up the new translation unit (all three targets) |
| `tests/bytecask_test.cpp` | Modified | New `[transaction]` TEST_CASEs |

### Declaration order in `bytecask.cppm`

`DB::snapshot()` returns `Snapshot`, which is a new type. A forward declaration before `DB` resolves the dependency — an incomplete return type is valid in a function declaration:

```cpp
// Forward declaration before DB:
export class Snapshot;
export class WritePlan;

// DB class with two new public methods:
export class DB { ... };

// Full definitions in dependency order after DB:
// WritePlan → Snapshot (full)
```

### Key field name

`KeyDirEntry::sequence` is the actual field. The design doc uses "lsn"/"sequence" as interchangeable concept names; the implementation uses `sequence` throughout.

### Access to `Snapshot::state_` in `apply_batch_if`

`apply_batch_if` is a method on `DB`. `Snapshot` declares `friend class DB`, so `DB`'s methods can read `snap.state_` directly. `WritePlan::actions_` and `WritePlan::range_guards_` are accessible to `DB` via `friend class DB` on `WritePlan`.

### Test coverage

| Test | What it proves |
|---|---|
| `DB::snapshot()` standalone | Read-only view consistent after concurrent writes |
| `apply_batch_if` no conflict | Applies when no concurrent write occurred |
| `apply_batch_if` W-W conflict | Returns `false` when key modified after snapshot |
| `apply_batch_if` `ensure_present` | Returns `false` when guarded key is absent |
| `apply_batch_if` `ensure_absent` | Returns `false` when guarded key is present |
| `apply_batch_if` `ensure_unchanged` | Returns `false` when guarded key was modified since snapshot |
| `apply_batch_if` `ensure_range_unchanged` | Returns `false` when a key in the guarded range was modified |
| `WritePlan` contradictory guards | Throws `std::logic_error` at build time for `ensure_present` + `ensure_absent` on same key |
| Single-op optimization | `apply_batch` / `apply_batch_if` with 1 write writes no markers (verify via `file_stats` byte counts) |
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
- **`Batch`** — unchanged. Remains the unconditional Layer 0 primitive. Never absorbs conditional operations.
- **`apply_batch_if`** — signature never changes. `WritePlan` absorbs new guard types; `apply_batch_if` processes whatever is in the plan.
- **`WritePlan`** is the extension point. New guard types (e.g. `ensure_value_equals` if ever needed) are added to it; nothing upstream changes.
- **`Transaction`** — when range Serializability arrives, `iter_from()` starts populating `read_set_ranges_` and emitting `ensure_range_unchanged` into the `WritePlan` at commit. No change to `apply_batch_if`, no change to `DB`.

Each milestone is purely additive. No existing call site needs modification when a higher isolation level is introduced.

---

## Open questions

1. **Moved-from `Transaction` destructor**: silently no-op (like `std::unique_ptr`) or assert. `std::unique_ptr` precedent is strong — silent no-op.

2. **Max open duration / observability**: long-lived `Snapshot` or `Transaction` delays vacuum file reclamation. Minimum response is documentation. Future option: `Transaction::snapshot_age()` or `VacuumOptions::stale_snapshot_warn_ms`.

3. **Range Serializability milestone boundary**: `Transaction::iter_from()` on a `Serializable` transaction throws `std::logic_error` until range tracking is implemented. Silently degrading the isolation guarantee without the caller knowing is worse than a clear error.

4. **R-W check scope**: all guard checks (`ensure_present`, `ensure_absent`, `ensure_unchanged`, `ensure_range_unchanged`) run inside `apply_batch_if` under `write_mu_`, alongside the implicit W-W check. This makes `apply_batch_if` a single, unified conflict-checking primitive. Callers who need only W-W protection simply build a `WritePlan` with writes only — no guards. Callers who need R-W protection add guards. `Transaction::commit()` emits guards for its read set automatically.
