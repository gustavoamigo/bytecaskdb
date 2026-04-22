# ByteCaskDB JavaScript API Specification

API for the ByteCaskDB WebAssembly module. All methods accept an optional options object as the last parameter for forward-compatible extensibility.

## Module Initialization

```js
import createByteCask from './build/bytecask.mjs';

const Module = await createByteCask();
const { ByteCaskDB, WritePlan } = Module;
```

## ByteCaskDB

### `ByteCaskDB.open(path, opts?)`

Open or create a database at `path`.

- **path** `string` — directory path
- **opts** `OpenOptions?` — optional configuration
- **returns** `ByteCaskDB`
- **throws** on I/O failure or CRC error during recovery

```js
const db = ByteCaskDB.open('/tmp/mydb');
const db = ByteCaskDB.open('/tmp/mydb', { maxFileBytes: 128 * 1024 * 1024 });
```

**OpenOptions**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `maxFileBytes` | `number` | `67108864` (64 MiB) | Active file rotation threshold |
| `failOnCrcErrors` | `boolean` | `true` | Throw on CRC errors during recovery. When `false`, corrupt entries are skipped. |

---

### `.get(key, opts?)`

Read a value by key.

- **key** `string`
- **opts** `ReadOptions?`
- **returns** `Uint8Array | null`
- **throws** on I/O failure, CRC mismatch (when checksums enabled), or `DbDegraded`

**ReadOptions**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `verifyChecksums` | `boolean` | `false` | CRC-verify the value read from disk |

---

### `.put(key, value, opts?)`

Write a key-value pair.

- **key** `string`
- **value** `string`
- **opts** `WriteOptions?`
- **throws** on I/O failure or `DbDegraded`

**WriteOptions**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `sync` | `boolean` | `true` | Call `fdatasync` after write |

---

### `.del(key, opts?)`

Delete a key.

- **key** `string`
- **opts** `WriteOptions?`
- **returns** `boolean` — `true` if the key existed
- **throws** on I/O failure or `DbDegraded`

---

### `.delRange(from, to, opts?)`

Delete all keys in `[from, to)` with a single disk append.

- **from** `string`
- **to** `string`
- **opts** `WriteOptions?`
- **throws** on I/O failure or `DbDegraded`

---

### `.containsKey(key)`

Check whether a key exists. Pure in-memory lookup.

- **key** `string`
- **returns** `boolean`

---

### `.snapshot()`

Capture a frozen, read-only view of the database at this instant.

- **returns** `Snapshot`

---

### `.applyBatch(plan, opts?)`

Atomically apply all operations in a `WritePlan`. Returns `false` on conflict when the plan has a snapshot or guards.

- **plan** `WritePlan` — consumed by this call
- **opts** `WriteOptions?`
- **returns** `boolean`
- **throws** on I/O failure or `DbDegraded`

---

### `.entries(from, opts?)`

Forward scan starting from `from`. Returns a lazy JS iterator of `{ key: Uint8Array, value: Uint8Array }`.

- **from** `string` — start key (inclusive)
- **opts** `ReadOptions?`
- **returns** `EntryIterator` — implements JS iterator protocol

```js
for (const { key, value } of db.entries('user:')) {
  if (/* done */) break;
}
```

---

### `.keys(from, opts?)`

Forward key-only scan. Returns a lazy JS iterator of `Uint8Array`.

- **from** `string`
- **opts** `ReadOptions?`
- **returns** `KeyIterator`

---

### `.entriesReverse(from, opts?)`

Reverse scan starting from the last key <= `from`.

- **from** `string`
- **opts** `ReadOptions?`
- **returns** `ReverseEntryIterator`

---

### `.keysReverse(from, opts?)`

Reverse key-only scan.

- **from** `string`
- **opts** `ReadOptions?`
- **returns** `ReverseKeyIterator`

---

### `.vacuum()`

Reclaim space from overwritten or deleted keys.

- **returns** `boolean` — `true` if a file was vacuumed

---

### `.isDegraded()`

Check whether the engine has entered a degraded state.

- **returns** `boolean`

---

### `.degradedReason()`

Get the reason for the degraded state.

- **returns** `string`

---

### `.resume()`

Attempt to recover from a degraded state.

---

### `.close()`

Close the database and free C++ memory. Also available via `Symbol.dispose`.

---

## Snapshot

Frozen, read-only view of the database at a point in time. Holds open any referenced data files until closed.

### `.get(key, opts?)`

Read from the snapshot. Same signature as `ByteCaskDB.get`.

### `.containsKey(key)`

Check existence in the snapshot.

### `.entries(from, opts?)`

Forward scan over the snapshot. Returns `EntryIterator`.

### `.keys(from, opts?)`

Forward key-only scan over the snapshot. Returns `KeyIterator`.

### `.entriesReverse(from, opts?)`

Reverse scan over the snapshot. Returns `ReverseEntryIterator`.

### `.keysReverse(from, opts?)`

Reverse key-only scan over the snapshot. Returns `ReverseKeyIterator`.

### `.close()`

Release the snapshot.

---

## WritePlan

Atomic batch builder. Groups multiple operations into a single atomic write.

### `new WritePlan()`

Create an unguarded batch (no snapshot, no conflict detection on write keys).

### `WritePlan.withSnapshot(snap)`

Create a guarded batch. Consumes the snapshot — the `Snapshot` object becomes invalid after this call. Write keys are automatically checked for concurrent modification. Use `ensureUnchanged` for read-only dependencies.

- **snap** `Snapshot` — consumed

### `.put(key, value)`

Add a put operation to the batch.

### `.del(key)`

Add a delete operation to the batch.

### `.delRange(from, to)`

Add a range delete `[from, to)` to the batch.

### `.ensurePresent(key)`

Guard: reject the batch if `key` does not exist at commit time.

### `.ensureAbsent(key)`

Guard: reject the batch if `key` exists at commit time.

### `.ensureUnchanged(key)`

Guard: reject the batch if `key` has changed since the snapshot. Requires a snapshot-backed plan.

### `.ensureRangeUnchanged(from, to)`

Guard: reject the batch if any key in `[from, to)` has changed since the snapshot. Requires a snapshot-backed plan.

### `.close()`

Release the plan.

---

## Iterators

All iterator types (`EntryIterator`, `KeyIterator`, `ReverseEntryIterator`, `ReverseKeyIterator`) implement the JS iterator protocol:

- **`next()`** — returns `{ value, done }`. For entry iterators, `value` is `{ key: Uint8Array, value: Uint8Array }`. For key iterators, `value` is `Uint8Array`.
- **`Symbol.iterator`** — returns `this`, enabling `for...of`.
- **`Symbol.dispose`** / **`close()`** — release C++ state.

Iterators are lazy: each `next()` call advances the underlying C++ iterator and copies one result across the WASM boundary. Use `break` to stop early without materializing the full result set.

```js
// Collect first 10 entries
const results = [];
for (const entry of db.entries('prefix:')) {
  results.push(entry);
  if (results.length >= 10) break;
}
```

**Important:** Iterator objects hold C++ state (open file descriptors, tree cursors). Always close them when done — either by consuming to exhaustion, calling `.close()`, or using `using` declarations.

---

## Options Reference

| Object | Field | Type | Default | Used by |
|--------|-------|------|---------|---------|
| `OpenOptions` | `maxFileBytes` | `number` | `67108864` | `open` |
| `OpenOptions` | `failOnCrcErrors` | `boolean` | `true` | `open` |
| `WriteOptions` | `sync` | `boolean` | `true` | `put`, `del`, `delRange`, `applyBatch` |
| `ReadOptions` | `verifyChecksums` | `boolean` | `false` | `get`, `entries`, `keys`, `entriesReverse`, `keysReverse` |

All options objects are optional. Omitting them uses the defaults shown above.
