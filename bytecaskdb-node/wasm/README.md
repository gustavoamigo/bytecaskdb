# Emscripten / WebAssembly Build

Cross-compiles ByteCaskDB to WebAssembly and runs under Node.js using NODEFS for real file I/O. Single-threaded only (no pthreads).

## Prerequisites

- Emscripten SDK activated (`source emsdk_env.sh`)
- Internet access on first run (clones `google/crc32c` and `google/benchmark`)

## Build

```bash
cd emscripten && bash build.sh
```

The build script:

1. Cross-compiles **crc32c** and **Google Benchmark** to WASM (cached after first run)
2. Precompiles all C++23 modules in dependency order
3. Compiles and links three targets:
   - `build/bytecask_node.js` — smoke test
   - `build/engine_bench_nodefs.js` — engine benchmarks
   - `build/bytecask.mjs` — Embind JS module

## JavaScript API

The Embind module exposes ByteCaskDB as a JS-callable API.

```js
import createByteCask from './build/bytecask.mjs';

const Module = await createByteCask();
const { ByteCaskDB, WritePlan } = Module;

// Open a database (creates the directory if needed)
const db = ByteCaskDB.open('/tmp/mydb');

// Put / Get / Del — keys and values are strings
db.put('hello', 'world');          // async write (no fsync)
db.putSync('hello', 'world');      // durable write (fsync)

const val = db.get('hello');       // Uint8Array | null
Buffer.from(val).toString();       // "world"

db.containsKey('hello');           // true
db.del('hello');                   // returns true if key existed

// Range deletion — all keys in [from, to)
db.delRange('session:', 'session:~');

// Ordered iteration — returns arrays (materialized, not lazy)
const entries = db.entries('user:', 100);  // [{key: Uint8Array, value: Uint8Array}, ...]
const keys = db.keys('user:', 100);       // [Uint8Array, ...]

// Reverse iteration
const last = db.entriesReverse('user:~', 10);
const lastKeys = db.keysReverse('user:~', 10);

// Atomic batch
const plan = new WritePlan();
plan.put('a', '1');
plan.put('b', '2');
plan.del('c');
db.applyBatch(plan);  // true (committed)

// Snapshot isolation
const snap = db.snapshot();
snap.get('a');              // Uint8Array
snap.containsKey('b');      // true
snap.entries('', 100);      // read from frozen point-in-time view

// Optimistic concurrency (snapshot + guards)
const snap2 = db.snapshot();
const guarded = WritePlan.withSnapshot(snap2);
guarded.ensureUnchanged('price');  // reject if price changed since snapshot
guarded.put('order:99', 'total');
if (!db.applyBatch(guarded)) {
  // conflict — price changed, retry
}

// Cleanup — call close() or use Symbol.dispose (Node.js 22+)
db.close();
snap.close();

// With explicit resource management (TypeScript 5.2+ / Node.js 22+):
{
  using db = ByteCaskDB.open('/tmp/mydb');
  db.put('x', 'y');
} // db[Symbol.dispose]() called automatically
```

### API Reference

**ByteCaskDB**

| Method | Returns | Description |
|--------|---------|-------------|
| `ByteCaskDB.open(path)` | `ByteCaskDB` | Open or create a database at `path` |
| `.get(key)` | `Uint8Array \| null` | Read a value |
| `.put(key, value)` | `void` | Write (no fsync) |
| `.putSync(key, value)` | `void` | Write with fsync |
| `.del(key)` | `boolean` | Delete; returns true if key existed |
| `.delSync(key)` | `boolean` | Delete with fsync |
| `.delRange(from, to)` | `void` | Delete all keys in [from, to) |
| `.containsKey(key)` | `boolean` | Check existence |
| `.snapshot()` | `Snapshot` | Frozen point-in-time view |
| `.applyBatch(plan)` | `boolean` | Atomic batch; false on conflict |
| `.applyBatchNoSync(plan)` | `boolean` | Atomic batch without fsync |
| `.entries(from, limit)` | `Array<{key, value}>` | Forward scan from key |
| `.keys(from, limit)` | `Array<Uint8Array>` | Forward key-only scan |
| `.entriesReverse(from, limit)` | `Array<{key, value}>` | Reverse scan |
| `.keysReverse(from, limit)` | `Array<Uint8Array>` | Reverse key-only scan |
| `.vacuum()` | `boolean` | Reclaim space; true if a file was vacuumed |
| `.isDegraded()` | `boolean` | Check for degraded state |
| `.degradedReason()` | `string` | Reason for degraded state |
| `.resume()` | `void` | Recover from degraded state |
| `.close()` | `void` | Close the database |

**Snapshot** — read-only, frozen view

| Method | Returns | Description |
|--------|---------|-------------|
| `.get(key)` | `Uint8Array \| null` | Read from snapshot |
| `.containsKey(key)` | `boolean` | Check existence in snapshot |
| `.entries(from, limit)` | `Array<{key, value}>` | Forward scan |
| `.keys(from, limit)` | `Array<Uint8Array>` | Key-only scan |
| `.close()` | `void` | Release the snapshot |

**WritePlan** — atomic batch builder

| Method | Returns | Description |
|--------|---------|-------------|
| `new WritePlan()` | `WritePlan` | Unguarded batch |
| `WritePlan.withSnapshot(snap)` | `WritePlan` | Guarded batch (consumes snapshot) |
| `.put(key, value)` | `void` | Add a put operation |
| `.del(key)` | `void` | Add a delete operation |
| `.delRange(from, to)` | `void` | Add a range delete |
| `.ensurePresent(key)` | `void` | Guard: key must exist |
| `.ensureAbsent(key)` | `void` | Guard: key must not exist |
| `.ensureUnchanged(key)` | `void` | Guard: key unchanged since snapshot |
| `.ensureRangeUnchanged(from, to)` | `void` | Guard: no changes in [from, to) |
| `.close()` | `void` | Release the plan |

### Notes

- Keys and values are passed as UTF-8 strings. Binary keys are not yet supported.
- `entries()` / `keys()` materialize results into arrays. Use the `limit` parameter for large datasets.
- Call `.close()` on DB, Snapshot, and WritePlan when done to free C++ memory. There is no garbage collection integration.
- All types support `Symbol.dispose` for use with `using` declarations (Node.js 22+).
- C++ exceptions (I/O errors, CRC mismatches) are thrown as JS `Error` objects.

## Benchmarks

```bash
# Default 50k keys
node build/engine_bench_nodefs.js

# Custom dataset size
BC_DATASET_SIZE=100000 node build/engine_bench_nodefs.js

# Filter to a single benchmark
node build/engine_bench_nodefs.js --benchmark_filter="ByteCaskDB/Get"
```

## Smoke test

```bash
node build/bytecask_node.js
```

## What's different from the native build

| | Native | WASM |
|---|---|---|
| Threads | Multi-threaded (group commit, parallel recovery) | Single-threaded (`BYTECASK_SINGLE_THREADED`) |
| I/O | Direct syscalls | NODEFS (Node.js fs passthrough) |
| Exceptions | Native C++ | WASM exceptions (`-fwasm-exceptions`) |
| Benchmarked engines | ByteCaskDB, RocksDB | ByteCaskDB only |
| Multi-threaded benchmarks | Yes | Disabled (`BENCH_NO_MT`) |

## Files

| File | Description |
|------|-------------|
| `build.sh` | Build script — compiles dependencies, modules, and links all targets |
| `bytecask_embind.cpp` | Embind binding layer — exposes DB, Snapshot, WritePlan to JS |
| `test_node.cpp` | Minimal C++ smoke test: write, read, recovery |
| `pre.js` | Emscripten pre-run hook: env propagation + Symbol.dispose wiring |
| `run.sh` | Helper to run built binaries with env propagation |

## Clean rebuild

```bash
rm -rf build && bash build.sh
```
