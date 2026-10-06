# ByteCaskDB Transaction Design

**Status: implemented.** `DB::snapshot()`, `Snapshot`, `WritePlan` with its guards, and the conditional `DB::apply_batch(opts, plan)` are in `bytecaskdb/bytecask.cppm`. A higher-level `Transaction` type (buffered writes, read-your-own-writes, merged iterators, read-set tracking) was considered and not built; callers that need one, such as the MariaDB plugin (`bytecaskdb-mariadb-plugin/bytecaskdb_txn.cc`), build it on the primitives described here.

## Purpose

This document describes how ByteCaskDB supports transactions: a consistent read view (`Snapshot`), a write set with preconditions (`WritePlan`), and a compare-and-swap commit (`apply_batch`). It covers the API, conflict detection, the isolation levels the primitives give, and resource management.

---

## Overview

Transactions are built from two methods on `DB`:

- `snapshot()` returns a frozen, read-only view of the database.
- `apply_batch(opts, plan)` applies a `WritePlan` atomically, only if every precondition in it holds.

There is no transaction object, no wrapper around `DB`, and no lock beyond the write path's own serialisation. A caller uses as much as it needs:

| Need | Use |
|---|---|
| Unconditional single-key write | `put` / `del` / `del_range` |
| Unconditional multi-key atomic write | `apply_batch(opts, WritePlan{})` |
| Consistent read-only view | `db.snapshot()` |
| Conflict-safe read-modify-write | `db.snapshot()`, then `apply_batch(opts, WritePlan{std::move(snap)})` |

`put`, `del` and `del_range` build a `WritePlan` and call `apply_batch`. `del` adds an `ensure_present` guard, which is why it returns `nullopt` when the key is absent.

---

## `snapshot() → Snapshot`

```cpp
// On DB:
[[nodiscard]] auto snapshot() const -> Snapshot;
```

A `Snapshot` is a move-only value wrapping `shared_ptr<const EngineState>` — the same published state pointer readers use internally. It freezes the key directory, the file registry and the open data files at that instant. Reads on it acquire no mutex.

```cpp
export class Snapshot {
public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) noexcept = default;
  Snapshot& operator=(Snapshot&&) noexcept = default;

  [[nodiscard]] auto get(const ReadOptions& opts, BytesView key, Bytes& out) const -> bool;
  [[nodiscard]] auto contains_key(const ReadOptions& opts, BytesView key) const -> bool;
  [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;
  [[nodiscard]] auto riter_from(const ReadOptions& opts, BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto rkeys_from(const ReadOptions& opts, BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;
  // min(live keys in [from, to), limit). At most two record reads.
  [[nodiscard]] auto count_keys(BytesView from, BytesView to,
                                std::size_t limit) const -> std::size_t;

private:
  std::shared_ptr<const EngineState> state_;
  friend class DB;        // DB::snapshot() constructs it
  friend class WritePlan; // guards compare against state_
  // ...
};
```

Standalone use:

```cpp
auto snap = db.snapshot();
Bytes out;
snap.get({}, key, out);
for (auto& [k, v] : snap.iter_from({})) { ... }
// snap destructs: vacuum may now reclaim the files it held
```

**Why a first-class type rather than `ReadOptions::snapshot`.** LevelDB passes a raw `const Snapshot*` through `ReadOptions` and requires a manual `ReleaseSnapshot()`. Here reads are called on the snapshot itself and its lifetime is RAII. The same object is what a `WritePlan` carries as its reference point for conflict checks.

---

## `WritePlan`

`WritePlan` is the type `apply_batch` consumes. It carries **writes** (`put`, `del`, `del_range`) and **guards** (`ensure_present`, `ensure_absent`, `ensure_unchanged`, `ensure_range_unchanged`). Guards are preconditions checked atomically with the apply; if any fails, nothing in the plan is written.

A plan built with `WritePlan()` has no snapshot. It is an unconditional batch that may also carry `ensure_present` and `ensure_absent`, which test the current state. A plan built with `WritePlan(Snapshot)` adds `ensure_unchanged`, `ensure_range_unchanged` and the implicit write-write check on every key it writes.

```cpp
export class WritePlan {
public:
  WritePlan();                         // no snapshot: only ensure_present / ensure_absent
  explicit WritePlan(Snapshot snap);   // snapshot embedded: all guards, implicit W-W check

  WritePlan(const WritePlan&) = delete;
  WritePlan& operator=(const WritePlan&) = delete;
  WritePlan(WritePlan&&) noexcept = default;
  WritePlan& operator=(WritePlan&&) noexcept = default;

  // --- Writes (kept in call order) ---
  void put(BytesView key, BytesView value);
  void del(BytesView key);
  void del_range(BytesView from, BytesView to);   // [from, to)

  // --- Point guards ---
  // Conflict if key is absent from the current state at commit.
  void ensure_present(BytesView key);
  // Conflict if key is present in the current state at commit.
  void ensure_absent(BytesView key);
  // Conflict if key's sequence differs from the snapshot's (written,
  // created or deleted since). Throws std::logic_error without a snapshot.
  void ensure_unchanged(BytesView key);

  // --- Range guard ---
  // Conflict if any key in [from, to) was inserted, modified or deleted
  // since the snapshot. Throws std::logic_error without a snapshot.
  void ensure_range_unchanged(BytesView from, BytesView to);

  [[nodiscard]] auto has_snapshot() const noexcept -> bool;

private:
  std::optional<Snapshot> snap_;
  std::vector<WriteOp> writes_;            // PointPut | PointDel | RangeDel
  std::map<Bytes, KeyGuard> guards_;       // one precondition per key
  std::vector<RangeGuard> range_guards_;
  // ...
};
```

Writes and point guards are stored separately: writes in a vector, in the order they were added; point guards in a map holding one precondition per key. Setting a second, different precondition on a key that already has one (`ensure_present` then `ensure_absent`, or `ensure_present` then `ensure_unchanged`) throws `std::logic_error` at build time, not at commit. Repeating the same guard is allowed. Key and value sizes are checked against the database's limits as each call is made.

### Composing intent

| Intent | Calls |
|---|---|
| Upsert | `put(k, v)` |
| Insert, fail if the key exists | `ensure_absent(k)` + `put(k, v)` |
| Update, fail if the key is missing | `ensure_present(k)` + `put(k, v)` |
| Delete, fail if the key is missing | `ensure_present(k)` + `del(k)` |
| Unconditional delete | `del(k)` |
| Read dependency (no write) | `ensure_unchanged(k)` |
| Range read dependency | `ensure_range_unchanged(from, to)` |
| Unique key | `ensure_absent(k)`, or `ensure_range_unchanged` over a prefix |

---

## `apply_batch(opts, plan)`

```cpp
// On DB:
// Applies plan atomically iff every guard passes and, when the plan carries
// a snapshot, no key it writes changed since. nullopt on conflict — nothing
// was written. An empty or guard-only plan that passes writes nothing and
// returns {sequence = 0, durable = true}. Throws std::system_error on I/O
// failure, DbDegraded if degraded, DbFollowerMode in follower mode.
[[nodiscard]] auto apply_batch(WriteOptions opts, WritePlan plan)
    -> std::optional<CommitResult>;
```

`CommitResult{sequence, durable}` carries the highest sequence assigned to the write and whether `fdatasync` confirmed it before return. With `sync = true`, an empty or guard-only plan first makes every earlier `sync = false` write durable. See `docs/commit_result_api_design.md`.

### Conflict check

Before the plan joins a batch, `apply_batch` looks up each written key in the plan's snapshot on the caller's thread (`WritePlan::resolve_snapshot_entries`). The check itself runs in the write path's serial section under `write_mu_`, against the prepared head: every write already accepted, including those whose `fdatasync` is still in flight. Plans in one batch are checked one after another, so each sees the writes of the plans before it.

```
// Under write_mu_. "head" is the prepared state; "snap" the plan's snapshot.

// 1. Point guards
for each (key, precondition) in plan.guards_:
    cur = head.find(key)
    MustExist:       if !cur → conflict
    MustBeAbsent:    if cur  → conflict
    MustBeUnchanged: if seq(cur) != seq(snap.find(key)) → conflict   // absent = 0

// 2. Range guards
for each [from, to) in plan.range_guards_:
    range_changed(from, to)

// 3. Implicit W-W check (only when the plan has a snapshot)
for each write in plan.writes_:
    put / del key K:
        s = snap entry for K (resolved earlier)
        if s and head still maps K to s's record location → no conflict
        cur = head.find(K)
        appeared (!s and cur), deleted (s and !cur), or
        modified (s and cur and seq differs) → conflict
    del_range [from, to):
        range_changed(from, to)

range_changed(from, to):
    for each K in head over [from, to):
        if seq(K) != seq(snap.find(K)) → changed     // inserted or modified
    for each K in snap over [from, to):
        if !head.find(K) → changed                   // deleted, by del or a range tombstone

on conflict: result = nullopt, nothing appended
otherwise:   append entries, apply them to the head
```

Comparisons are by `KeyDirEntry::sequence`. Vacuum moves a record but keeps its sequence, so a relocated key is not a conflict. A record location names one immutable record for the life of the process, so a written key whose head entry still points at the snapshot's record is unchanged and needs no further lookup. On the blind-leaf key directory that check reads no record; `docs/bytecask_design.md` (*Implicit W-W check on write keys*) has the detail.

A plan that conflicts with a write the head holds but that is not yet published returns once that write is published. Until then a retry from a fresh snapshot would see the same state and lose again. A plan whose snapshot is already behind the published state returns at once.

### Single-entry plans

A plan with more than one write is framed by `BulkBegin`/`BulkEnd` markers so recovery applies it all or not at all. A plan with exactly one write skips the markers: a single CRC-checked entry is already atomic on disk. A guarded single-key CAS therefore costs the same append as `put`.

| Path | Markers written |
|---|---|
| `put` / `del` / `del_range` | Never |
| `apply_batch` with 1 write | No |
| `apply_batch` with > 1 writes | Yes |

---

## Isolation levels

### Read-uncommitted fast path

`put`, `del`, `del_range`, and `apply_batch` with a snapshot-less, guardless plan. No conflict detection; concurrent read-modify-write cycles can lose updates. The name is nominal: no reader ever sees an unpublished write.

### Snapshot isolation

Read from a `Snapshot`, write through `WritePlan{std::move(snap)}`. The implicit W-W check makes the first committer win on every written key. It does not prevent write skew: two plans that each read a key the other writes can both commit.

### Serializable

Add `ensure_unchanged` for every key read but not written, and `ensure_range_unchanged` for every range scanned. Because the guards are checked in the same serial section as the W-W check and the apply, no writer can commit between check and apply. `ensure_unchanged` compares sequences, not values, so it also catches a write that restored the same value (ABA).

### What is checked

The isolation check ([`isolation_checking_design.md`](isolation_checking_design.md)) records concurrent histories of `WritePlan` transactions and runs Elle over them every night, with vacuum and injected `fdatasync` failures running, and with the process SIGKILLed and reopened under concurrent group-commit writers:

- A plan built on a snapshot, with `ensure_unchanged` on every key it read but did not write, is **strict-serializable**.
- The same plan without read guards is **snapshot-isolated**. Its only serializability anomaly is write skew (G2-item).
- Plans without a snapshot lose updates, as expected.

The check covers point reads and writes. `ensure_range_unchanged`, `del_range` and phantoms are not part of it yet.

### Read committed

Not a separate level. Writes are visible only after `state_.store()` publishes them, so no reader sees an uncommitted write. A caller that wants read-committed reads calls `DB::get` instead of reading from a held snapshot.

---

## Guarantee coverage

### Write-write

1. **Two writers update the same key.** The second committer's implicit W-W check sees a different sequence → `nullopt`.
2. **Two writers delete the same key.** Same check on the `del` key.
3. **Two writers insert the same new key.** `ensure_absent(k) + put(k, v)` on both; the second fails the guard. The implicit check would also catch it.
4. **Conditional update.** `ensure_present(k) + put(k, v)` fails if the key was deleted before commit.
5. **Conditional delete.** `ensure_present(k) + del(k)` fails if the key is already gone.

### Read-write

6. **Write skew (two on-call doctors).** Each plan guards the key it read but does not write:

   ```cpp
   // Plan A:
   plan.ensure_unchanged(to_bytes("doctor_2"));
   plan.del(to_bytes("doctor_1"));
   // Plan B:
   plan.ensure_unchanged(to_bytes("doctor_1"));
   plan.del(to_bytes("doctor_2"));
   ```

   The first committer succeeds; the second fails its guard.

7. **Read dependency on another key.**

   ```cpp
   auto snap = db.snapshot();
   Bytes rate, a, b;
   (void)snap.get({}, to_bytes("exchange_rate"), rate);
   (void)snap.get({}, to_bytes("balance_a"), a);
   (void)snap.get({}, to_bytes("balance_b"), b);

   WritePlan plan{std::move(snap)};
   plan.ensure_unchanged(to_bytes("exchange_rate"));  // read, not written
   plan.put(to_bytes("balance_a"), new_a);            // covered by the W-W check
   plan.put(to_bytes("balance_b"), new_b);
   auto result = db.apply_batch({}, std::move(plan));
   ```

8. **Phantoms.** A plan that scanned `[user:100:, user:200:)` adds `ensure_range_unchanged(user:100:, user:200:)`. Any insert, delete or modification in the range since the snapshot is a conflict.
9. **Unique prefix.** `ensure_range_unchanged(user:150:, user:151:)` + `ensure_absent(user:150:)` + `put(user:150:, v)`.
10. **Read-only validation.** A plan with only `ensure_unchanged` guards confirms several reads came from a still-current cut. It writes nothing.

### Edge cases

11. `ensure_unchanged(k)` on a key absent at snapshot and still absent: both sequences are 0, no conflict.
12. Absent at snapshot, present now: conflict.
13. Present at snapshot, deleted now: conflict.
14. `ensure_present(k)` and `ensure_unchanged(k)` on the same key: different preconditions, `std::logic_error` at build time. `ensure_unchanged` alone already fails if the key was deleted; if it must also exist, that is known from the snapshot read.
15. `ensure_present(k)` and `ensure_absent(k)`: `std::logic_error` at build time.

| Guarantee | Mechanism |
|---|---|
| W-W on written keys | Implicit sequence check when the plan has a snapshot |
| Conditional insert | `ensure_absent` + `put` |
| Conditional update / delete | `ensure_present` + `put` / `del` |
| R-W on points (write skew) | `ensure_unchanged` on keys read but not written |
| R-W on ranges (phantoms) | `ensure_range_unchanged` |
| Read-only validation | Guards only |
| Unique constraint | `ensure_absent` or `ensure_range_unchanged` |

---

## Conflict signalling

A conflict is an expected outcome, not an error: `apply_batch` returns `nullopt` and the caller retries or gives up. I/O failures are exceptions (`std::system_error`). This follows C++ Core Guidelines E.3 and keeps retry loops free of `try`/`catch`:

```cpp
while (true) {
  auto snap = db.snapshot();
  Bytes out;
  (void)snap.get({}, to_bytes("counter"), out);
  WritePlan plan{std::move(snap)};
  plan.put(to_bytes("counter"), increment(out));  // W-W check covers "counter"
  if (db.apply_batch({}, std::move(plan))) break;
}
```

---

## Snapshot lifetime and vacuum

`Snapshot` holds `shared_ptr<const EngineState>`. Through `EngineState → FileRegistry → DataFile`, that keeps every data file the snapshot references open. Vacuum unlinks a file it has compacted, but a held descriptor keeps `pread` working and the blocks allocated until the last reference goes. The same mechanism protects in-flight readers; snapshots need nothing extra.

A `WritePlan` holds its snapshot until `apply_batch` consumes it. A long-lived snapshot or plan therefore delays the reclamation of disk space. It is not a correctness risk. `stats()` reports `bytecask.keydir_versions_live` and `bytecask.keydir_nodes_parked`, which grow while old states are held.

---

## Comparison with RocksDB

| Concept | RocksDB | ByteCaskDB |
|---|---|---|
| CAS write | Requires `TransactionDB` or `OptimisticTransactionDB` | `db.apply_batch(opts, plan)` on plain `DB` |
| Snapshot | `db->GetSnapshot()` + manual `ReleaseSnapshot()` | `db.snapshot()` → `Snapshot`, RAII |
| Read dependency | `txn->GetForUpdate()` | `plan.ensure_unchanged(k)` |
| Conflict serialisation | Lock manager or optimistic validation | The write path's serial section, already there for every write |
| File retention | Ref-counted SST files | `Snapshot` holds `shared_ptr<EngineState>`; vacuum's deletes wait on it |

RocksDB's transactions need a wrapper type that owns the underlying `DB`. Here the primitives are on `DB` itself, so any caller can use conflict-safe writes without changing how it opens or holds the database.

---

## Extension point

`apply_batch(opts, plan)` does not change when the vocabulary grows. A new guard (for example a value-equality check, if a use case ever needs one) is a new `WritePlan` method plus a case in the conflict check. Guards and the implicit check need no on-disk format change, no new `EntryType`, and nothing in recovery or vacuum: they are evaluated against the in-memory key directory and leave no trace in the data files.
