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

The Embind module exposes ByteCaskDB as a JS-callable API. See [`API.md`](API.md) for the full specification.

```js
import createByteCask from './build/bytecask.mjs';

const Module = await createByteCask();
const { ByteCaskDB, WritePlan } = Module;

// Open a database (creates the directory if needed)
const db = ByteCaskDB.open('/tmp/mydb');
const db2 = ByteCaskDB.open('/tmp/mydb2', { maxFileBytes: 128 * 1024 * 1024 });

// Put / Get / Del — keys and values are strings
db.put('hello', 'world');                    // durable write (sync=true default)
db.put('hello', 'world', { sync: false });   // async write (no fsync)

const val = db.get('hello');                 // Uint8Array | null
Buffer.from(val).toString();                 // "world"

db.containsKey('hello');                     // true
db.del('hello');                             // returns true if key existed

// Range deletion — all keys in [from, to)
db.delRange('session:', 'session:~');

// Lazy iteration — JS iterator protocol (for...of, break, spread)
for (const { key, value } of db.entries('user:')) {
  console.log(Buffer.from(key).toString(), Buffer.from(value).toString());
  if (/* enough */) break;  // lazy — stops fetching from WASM
}

// Key-only iteration
for (const key of db.keys('user:')) { /* ... */ }

// Reverse iteration
for (const { key, value } of db.entriesReverse('user:~')) { /* ... */ }
for (const key of db.keysReverse('user:~')) { /* ... */ }

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
for (const { key } of snap.entries('')) { /* frozen view */ }

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

### Notes

- Keys and values are passed as UTF-8 strings. Binary keys are not yet supported.
- Scan methods (`entries`, `keys`, `entriesReverse`, `keysReverse`) return lazy JS iterators. Use `for...of` and `break` for bounded scans.
- Iterator objects hold C++ state. Close them when done, or consume to exhaustion, or use `using` declarations. All iterator and resource types support `Symbol.dispose` (Node.js 22+).
- Call `.close()` on DB, Snapshot, WritePlan, and iterators when done to free C++ memory. There is no garbage collection integration.
- C++ exceptions (I/O errors, CRC mismatches) are thrown as JS `Error` objects.
- All write methods default to `sync: true`. Pass `{ sync: false }` for async writes.

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
| `bytecask_embind.cpp` | Embind binding layer — exposes DB, Snapshot, WritePlan, iterators to JS |
| `test_node.cpp` | Minimal C++ smoke test: write, read, recovery |
| `pre.js` | Emscripten pre-run hook: env propagation, Symbol.dispose, Symbol.iterator wiring |
| `run.sh` | Helper to run built binaries with env propagation |
| `API.md` | Full JavaScript API specification |

## Clean rebuild

```bash
rm -rf build && bash build.sh
```
