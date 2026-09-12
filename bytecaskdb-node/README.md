# ByteCaskDB for Node.js

ByteCaskDB embedded key-value store for Node.js. Ships two backends behind the
identical TypeScript API: a **WASM backend** (Emscripten + Embind, single-threaded)
and a **native backend** (node-addon-api, links the real multi-threaded engine).
Both implement the same `ByteCaskFactory` contract — pick a backend and the rest
of your code is unchanged.

## Install (from source)

```bash
cd bytecaskdb-node
npm install
npm run build          # builds both backends (build:wasm + build:native)
```

## Usage

```js
import { createWasmBackend, createNativeBackend } from 'bytecaskdb';

// WASM backend (portable, single-threaded, sandboxed via NODEFS)
const wasm = await createWasmBackend();

// Native backend (multi-threaded engine, direct syscalls)
const native = await createNativeBackend();

const db = native.open('/tmp/mydb');
db.put('hello', 'world');
const val = db.get('hello');   // Uint8Array | null
db.close();
```

See [`wasm/API.md`](wasm/API.md) for the full JavaScript API specification —
it applies identically to both backends.

## Choosing a backend

| | Native (N-API) | WASM (Embind) |
|---|---|---|
| Threads | Multi-threaded (group commit, parallel recovery) | Single-threaded (`BYTECASK_SINGLE_THREADED`) |
| I/O | Direct syscalls | NODEFS (Node.js fs passthrough) |
| Build requirement | Clang + xmake (see [Building](../README.md#building)) | Emscripten SDK |
| Portability | Platform-specific native addon | Runs anywhere Node.js runs, including sandboxed environments without a native toolchain |
| Exceptions | Native C++ | WASM exceptions (`-fwasm-exceptions`) |
| Return type for `get()`/iterators | `Uint8Array` | `Uint8Array` |

Both backends default write methods to `sync: true` and accept `{ sync: false }`
for unsynced writes. Async (non-blocking) variants of fsync-bound calls are not
yet implemented on either backend — see `docs/native_node_binding_design.md`
for the design and follow-up plan.

## Native Backend

Links the real ByteCaskDB engine directly through
[node-addon-api](https://github.com/nodejs/node-addon-api) — no WASM sandbox,
full multi-threaded recovery and group commit.

### Build

```bash
cd bytecaskdb-node && npm run build:native
```

Builds the `bytecaskdb_node` xmake target (a shared library linking the
prebuilt `libbytecask.a` engine) and copies the resulting `.node` file to
`native/bytecask.node`. Requires the same Clang/xmake toolchain used to build
the core engine — no Emscripten SDK needed.

### Smoke test

```bash
npm run test:smoke:native
```

## WASM Backend

Cross-compiles ByteCaskDB to WebAssembly and runs under Node.js using NODEFS for real file I/O. Single-threaded only (no pthreads).

## Prerequisites

- Emscripten SDK activated (`source emsdk_env.sh`)
- Internet access on first run (clones `google/crc32c` and `google/benchmark`)

## Build

```bash
cd bytecaskdb-node/wasm && bash build.sh   # first run only — builds WASM deps
cd ../..
xmake build wasm_embind
```

`build.sh` cross-compiles **crc32c**, **Google Benchmark**, and **Catch2** to
WASM (cached after first run — a no-op on subsequent runs). It does not build
ByteCaskDB itself; the actual WASM targets are xmake targets, built from the
repository root:

| Target | Output | Description |
|--------|--------|--------------|
| `wasm_embind` | `build/bytecask.mjs` | Embind JS module |
| `wasm_smoke_test` | `build/wasm_smoke_test.js` | smoke test |
| `wasm_engine_bench` | `build/engine_bench_nodefs.js` | engine benchmarks |
| `wasm_tests` | `build/bytecask_tests.js` | Catch2 test suite |

## JavaScript API

The Embind module exposes ByteCaskDB as a JS-callable API. See [`wasm/API.md`](wasm/API.md) for the full specification.

```js
import createByteCask from './build/bytecask.mjs';
import { applyDisposeWiring } from '../dist/dispose.js';

const Module = await createByteCask();
// Wires Symbol.dispose/Symbol.iterator onto the raw Embind classes below —
// createWasmBackend() does this for you; needed here only for direct use of
// the raw module.
applyDisposeWiring(Module);
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
db.del('hello');                             // CommitResult, or null if key was absent

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
db.applyBatch(plan);  // CommitResult (committed) or null (conflict)

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
  // conflict (null) — price changed, retry
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
node wasm/build/engine_bench_nodefs.js

# Custom dataset size
BC_DATASET_SIZE=100000 node wasm/build/engine_bench_nodefs.js

# Filter to a single benchmark
node wasm/build/engine_bench_nodefs.js --benchmark_filter="ByteCaskDB/Get"
```

`engine_bench_nodefs.js` benchmarks the core C++ engine cross-compiled to
WASM and run under NODEFS — it never calls into the Embind binding layer, so
it says nothing about JS-facing call overhead.

### Comparing the native and WASM backends from JS

`scripts/bench.mjs` drives both backends through the identical
`ByteCaskFactory` interface — the one benchmark that actually measures the
N-API vs. Embind call overhead as seen from JS, not just the underlying
engine. Uses [tinybench](https://github.com/tinylibs/tinybench) directly
(Vitest wrapped this same library as `bench()` up through v4, but removed
that in-file API entirely in v5 with no replacement).

```bash
npm run bench                                 # both backends, all operations
node scripts/bench.mjs --filter=Get           # filter to one operation
BC_BENCH_DATASET_SIZE=100000 npm run bench    # custom dataset size (default 20k)
BC_BENCH_TIME_MS=5000 npm run bench           # run each benchmark longer
                                               # (tinybench default: 500ms)
```

Each operation (`Put/NoSync`, `Put/Sync`, `Get`, `Del/Sync`, `Range50`,
`MixedBatch/Sync`) reports both backends side by side plus a relative
speedup.

## Smoke test

```bash
node wasm/build/wasm_smoke_test.js
```

## Benchmark coverage differences (native vs WASM)

The `engine_bench` benchmarks (C++, not the Node bindings) compare backends —
this table describes those, not the Node.js `createNativeBackend`/`createWasmBackend` split above.

| | Native benchmark build | WASM benchmark build |
|---|---|---|
| Benchmarked engines | ByteCaskDB, RocksDB | ByteCaskDB only |
| Multi-threaded benchmarks | Yes | Disabled (`BENCH_NO_MT`) |

## Files

| File | Description |
|------|-------------|
| `native/bytecask_napi.cpp` | N-API binding layer — exposes DB, Snapshot, WritePlan, iterators, FileManifest to JS |
| `native/smoke_test.cjs` | Minimal Node.js smoke test for the compiled native addon |
| `wasm/build.sh` | Cross-compiles WASM dependencies (crc32c, Google Benchmark, Catch2); the WASM targets themselves are built by xmake — see the WASM Backend "Build" section above |
| `wasm/bytecask_embind.cpp` | Embind binding layer — exposes DB, Snapshot, WritePlan, iterators to JS |
| `wasm/test_node.cpp` | Minimal C++ smoke test: write, read, recovery |
| `wasm/pre.js` | Emscripten pre-run hook: env propagation, memory-usage reporting |
| `wasm/run.sh` | Helper to run built binaries with env propagation |
| `wasm/API.md` | Full JavaScript API specification |
| `src/types.ts` | Shared TypeScript interfaces (ByteCaskDB, Snapshot, WritePlan, iterators) |
| `src/wasm-backend.ts` | WASM backend factory |
| `src/native-backend.ts` | Native backend factory |
| `src/dispose.ts` | Shared `Symbol.dispose`/`Symbol.iterator` wiring used by both backends |
| `src/index.ts` | Package entry point |

## Clean rebuild

```bash
rm -rf wasm/build && cd wasm && bash build.sh   # rebuilds WASM deps from scratch
cd ../..
xmake build wasm_embind
```
