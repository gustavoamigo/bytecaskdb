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

Write a key-value pair. Cannot conflict.

- **key** `string`
- **value** `string`
- **opts** `WriteOptions?`
- **returns** `CommitResult` — the assigned sequence and whether it was durable before return
- **throws** on I/O failure or `DbDegraded`

**WriteOptions**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `sync` | `boolean` | `true` | Call `fdatasync` after write |

**CommitResult**

| Field | Type | Description |
|-------|------|-------------|
| `sequence` | `bigint` | Highest sequence assigned to this write. `0n` means nothing was written. |
| `durable` | `boolean` | `true` if `fdatasync` confirmed the write before return. |

---

### `.del(key, opts?)`

Delete a key.

- **key** `string`
- **opts** `WriteOptions?`
- **returns** `CommitResult | null` — `null` if the key was absent (nothing written)
- **throws** on I/O failure or `DbDegraded`

---

### `.delRange(from, to, opts?)`

Delete all keys in `[from, to)` with a single disk append. Cannot conflict.

- **from** `string`
- **to** `string`
- **opts** `WriteOptions?`
- **returns** `CommitResult` — `{ sequence: 0n, durable: true }` without writing if `from >= to`
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

Atomically apply all operations in a `WritePlan`. Returns `null` on conflict when the plan has a snapshot or guards.

- **plan** `WritePlan` — consumed by this call
- **opts** `WriteOptions?`
- **returns** `CommitResult | null` — `null` on conflict; an empty or guard-only plan that passes commits as a no-op (`{ sequence: 0n, durable: true }`)
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

### `.mode()`

Returns the current engine mode.

- **returns** `string` — `'leader'` or `'follower'`

---

### `.setMode(mode)`

Switch engine mode.

- **mode** `string` — `'leader'` or `'follower'`

---

### `.durableSequence(minSequence?, timeoutMs?)`

The single sequence primitive — replaces `currentSequence` (removed, no alias). Returns the highest sequence confirmed durable by `fdatasync`.

- **minSequence** `bigint?` — default `0n`. An already-reached target (or `0n`) returns immediately without blocking.
- **timeoutMs** `number?` — `0` (default): non-blocking poll. `> 0`: blocks until the durable sequence reaches `minSequence` or the timeout expires. **A positive `timeoutMs` blocks the calling thread — in Node.js, the event loop — for the whole wait; nothing else on that event loop can run until it returns.**
- **returns** `bigint` — the durable sequence at return (may be below `minSequence` if the timeout expired)

Covers three use cases with one call: polling (`db.durableSequence()`), replication wake-up (`leader.durableSequence(follower.durableSequence() + 1n, timeoutMs)`), and read-your-own-writes waits (`follower.durableSequence(result.sequence, timeoutMs)` using a `CommitResult` from a leader write).

---

### `.stats()`

Returns all operational counters and gauges as a flat object.

- **returns** `Record<string, number>` — monotonic counters (bytes_written, fsyncs, vacuum_bytes_reclaimed, crc_failures, io_errors, degraded_transitions) and gauges (degraded, open_files)

```js
const s = db.stats();
console.log(s.bytes_written, s.fsyncs, s.open_files);
```

---

### `.createManifest()`

Rotates the active file, waits for all hint files, and returns a manifest of sealed files with a snapshot.

- **returns** `FileManifest`

---

### `.changesSince(snap, fromSeq)`

Returns a lazy iterator over data entries with sequence > `fromSeq`.

- **snap** `Snapshot`
- **fromSeq** `number`
- **returns** `ChangeIterator`

---

### `.ingest(entries)`

Applies pre-sequenced entries from a leader. Follower mode only.

- **entries** `DataEntry[]`
- **throws** if not in follower mode or engine is degraded

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

### `WritePlan.withLimits(opts)`

Create an unguarded batch with key/value size limits. Useful when you want size validation without a snapshot.

- **opts** `{ maxKeyBytes?: number, maxValueBytes?: number }`

```js
const plan = WritePlan.withLimits({ maxKeyBytes: 256, maxValueBytes: 1024 });
plan.put('k', 'v');
db.applyBatch(plan);
```

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

### `.hasSnapshot()`

Check whether this plan was constructed with a snapshot.

- **returns** `boolean`

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
