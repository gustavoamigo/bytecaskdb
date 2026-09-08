# ByteCaskDB Native Node.js Binding Design

> Design for BC-236. Adds a native N-API backend (node-addon-api) alongside the
> existing WASM/Embind backend in `bytecaskdb-node/`, exposing the identical
> JavaScript API.

## Purpose

`bytecaskdb-node/` currently ships a single backend: ByteCaskDB cross-compiled
to WebAssembly and driven through Emscripten Embind
(`bytecaskdb-node/wasm/bytecask_embind.cpp`). WASM is single-threaded
(`BYTECASK_SINGLE_THREADED`) and runs the engine on top of NODEFS.

This document specifies a second backend: a **native** Node.js addon built with
[node-addon-api](https://github.com/nodejs/node-addon-api) that links the real
multi-threaded engine directly, keeping parallel recovery and the background
hint-file writer running on their own threads. Group commit is present but
only coalesces when writers contend, which a synchronous single-caller binding
does not produce — see [Concurrency model](#concurrency-model).
The two backends present the **same** JavaScript surface — the one already
declared in `bytecaskdb-node/src/types.ts` — so caller code is backend-agnostic.

Canonical location: `docs/native_node_binding_design.md`.

---

## Design decisions (BC-236)

| Decision | Choice | Rationale |
| --- | --- | --- |
| Build toolchain | **xmake target** `bytecaskdb_node` | Reuses the Clang C++23-modules toolchain that already builds the engine. Mirrors the Python `bytecaskdb_python` nanobind target, which links the same `bytecask` static library. node-gyp/cmake-js cannot drive Clang C++23 modules without significant custom work. |
| Call model | **Fully synchronous** | Exact parity with the shared `ByteCaskFactory` interface and the WASM backend. Matches the `better-sqlite3` model: an embedded store's ops are in-memory lookups plus one `pread`/`pwrite`, so synchronous calls do not meaningfully stall the event loop. The single exception — a blocking `durableSequence` — is documented, not hidden. |
| Scope | **Full API parity** | Everything the Embind backend exposes: `get`/`put`/`del`/`delRange`, `snapshot`, `applyBatch`, all four iterators, `changesSince`/`ingest`, `createManifest`, `stats`, mode control, and degraded-state handling. |

---

## Architecture

The package keeps `types.ts` as the single source of truth. Each backend is a
thin adapter that returns a `ByteCaskFactory`:

```
bytecaskdb-node/
├── src/
│   ├── types.ts            # unchanged — the ByteCaskFactory contract
│   ├── wasm-backend.ts     # unchanged — createWasmBackend()
│   ├── native-backend.ts   # NEW — createNativeBackend()
│   ├── dispose.ts          # NEW — shared Symbol.dispose / Symbol.iterator wiring
│   └── index.ts            # exports both factories
├── native/
│   ├── bytecask_napi.cpp   # NEW — node-addon-api binding (mirrors embind)
│   └── README.md           # updated
└── package.json            # + node-addon-api devDep, build:native script
```

```
  JS/TS caller
      │  (ByteCaskFactory — identical for both)
      ├──────────────────────────────┐
      ▼                              ▼
  native-backend.ts             wasm-backend.ts
      │  require('bytecask.node')     │  import bytecask.mjs
      ▼                              ▼
  bytecask_napi.cpp             bytecask_embind.cpp
      │  import bytecask;             │  import bytecask;
      ▼                              ▼
  libbytecask.a (native)        bytecask.wasm (Emscripten)
   multi-threaded                single-threaded
```

Both C++ binding layers `import bytecask;` and translate the same engine types.
The native layer is a near line-for-line analogue of the Embind layer — the same
wrapper structs, the same consumption guards, the same options-object parsing.

---

## C++ binding — `native/bytecask_napi.cpp`

The Embind file registers each engine type as an Embind `class_<T>`. The N-API
file registers each as a `Napi::ObjectWrap<T>` subclass. The mapping is
one-to-one:

| Embind class | N-API class | Wrapped engine type |
| --- | --- | --- |
| `ByteCaskDB` | `NapiDB` | `bytecask::DB` |
| `Snapshot` | `NapiSnapshot` | `std::optional<bytecask::Snapshot>` |
| `WritePlan` | `NapiWritePlan` | `std::optional<bytecask::WritePlan>` |
| `EntryIterator` | `NapiEntryIterator` | forward `EntryIterator` subrange |
| `KeyIterator` | `NapiKeyIterator` | forward `KeyIterator` subrange |
| `ReverseEntryIterator` | `NapiReverseEntryIterator` | `ReverseEntryIterator` subrange |
| `ReverseKeyIterator` | `NapiReverseKeyIterator` | `ReverseKeyIterator` subrange |
| `ChangeIterator` | `NapiChangeIterator` | `ChangeIterator` subrange |
| `FileManifest` | `NapiFileManifest` | `FileManifest` (snapshot + files) |

### Consumption guards (unchanged from Embind)

`Snapshot` and `WritePlan` are move-only and single-use. Both wrappers hold the
engine object in a `std::optional` and `check()` before every access:

- Constructing a `WritePlan` from a `Snapshot` moves the snapshot out and resets
  the source; further use throws `"Snapshot consumed by WritePlan"`.
- `applyBatch` moves the plan into the engine and resets it; further use throws
  `"WritePlan already applied"`.

These are the same runtime guards the Embind layer uses (`JsSnapshot::check`,
`JsWritePlan::check`); the engine API cannot express them at compile time across
the language boundary.

### Byte marshalling

The rule from the Embind layer holds verbatim: **every byte payload is copied
across the boundary.** A `std::span`/`Bytes` into engine-owned memory must never
be handed to JS as a view — it dangles the moment the C++ buffer is destroyed.

| Direction | Embind | N-API |
| --- | --- | --- |
| Key/value **in** | `std::string` param | `Napi::String` / `Napi::Buffer<uint8_t>` → owned `std::string` |
| Value/key **out** | copy into `Uint8Array` | copy into `Napi::Buffer<uint8_t>` (owns its bytes) |

Keys and values are passed **in** as UTF-8 strings, matching the current shared
contract (`put(key: string, value: string)`); binary keys remain unsupported on
both backends until `types.ts` changes. Reads return `Uint8Array`
(`Napi::Buffer`) as they do today.

### 64-bit sequences

Sequence numbers are `uint64_t` and must survive the boundary exactly — a JS
`number` (double) cannot represent every `u64`. The WASM backend achieves this
with `-sWASM_BIGINT` (a `uint64_t` becomes a JS `BigInt`). The native backend
uses `Napi::BigInt::New` / `Napi::BigInt::Uint64Value` directly. `CommitResult`
marshals to `{ sequence: bigint, durable: boolean }` on both backends.

### Options objects

Every method that takes engine options accepts an optional JS object as its last
argument. The native layer reads properties with the same names the Embind layer
uses (`sync`, `verifyChecksums`, `maxFileBytes`, `failOnCrcErrors`,
`maxKeyBytes`, `maxValueBytes`, `initialMode`) and maps `"leader"`/`"follower"`
strings to `bytecask::Mode`. A missing or `undefined`/`null` object yields
engine defaults.

### Error translation

C++ exceptions become JS `Error`s via `Napi::Error::ThrowAsJavaScriptException`
(or `NAPI_CPP_EXCEPTIONS`), mirroring the WASM behavior where I/O errors and CRC
mismatches surface as JS `Error`s. `nullopt` returns (from `del` and
`applyBatch`) become JS `null`, not exceptions — a conflict is an expected
outcome, not an error.

### Iterator protocol

Each iterator wrapper exposes `next()` returning `{ value, done }` and a
`close()` that frees the C++ state, exactly as the Embind iterators do. Laziness
is preserved: the wrapper advances the underlying engine iterator one step per
`next()` and never materializes the full range. `Symbol.iterator` and
`Symbol.dispose` are attached in the shared JS layer (below), not in C++.

---

## Build target — xmake `bytecaskdb_node`

Modeled on the existing `bytecaskdb_python` target. Key points:

- `set_kind("shared")`, `add_deps("bytecask")` — links the prebuilt
  `libbytecask.a` (built from the C++23 modules), so the binding TU is the only
  file compiled against the engine module interface.
- Resolve include paths at configure time via `on_load`, the way the Python
  target resolves nanobind/Python paths — here using Node's own headers and the
  `node-addon-api` package:
  - Node C headers (the `node-api-headers` package, or the headers shipped with
    the Node install).
  - `node-addon-api` include dir: `node -p "require('node-addon-api').include_dir"`.
- Output naming: `basename` `bytecask`, empty `prefixname`, extension `.node`,
  `targetdir` `bytecaskdb-node/native/` (or a `build/` subdir) so
  `native-backend.ts` can `require` it by a stable path.
- Undefined N-API symbols are resolved by the host Node process at load time —
  the same "unresolved host symbols" pattern the Python target documents for the
  Python C API. Do **not** pass `-Wl,--no-undefined`; on macOS pass
  `-undefined dynamic_lookup`.
- Third-party warning suppressions for `node-addon-api` headers under
  `-Weverything`, following the Python target's `-Wno-*` block.

`package.json` gains a `build:native` script (`cd .. && xmake build
bytecaskdb_node`) alongside the existing `build:wasm`, and `node-addon-api` as a
devDependency.

---

## Shared JS layer

### `native-backend.ts`

Mirrors `wasm-backend.ts`. Loads the `.node` addon and returns a
`ByteCaskFactory`:

```ts
export async function createNativeBackend(): Promise<ByteCaskFactory> {
  const addon = require("../native/bytecask.node");
  applyDisposeWiring(addon);            // shared with wasm-backend
  return { open: (path, opts) => addon.ByteCaskDB.open(path, opts ?? {}),
           WritePlan: addon.WritePlan };
}
```

There is no async module-init step (unlike Emscripten's `await createByteCask()`),
but the factory stays `async` to keep both backends interface-compatible and to
leave room for future lazy loading.

### `dispose.ts`

The `Symbol.dispose` and `Symbol.iterator` wiring currently lives in
`bytecaskdb-node/wasm/pre.js` and runs in Emscripten's `onRuntimeInitialized`.
It is lifted into a backend-neutral `applyDisposeWiring(module)` helper that both
backends call, so disposal and iteration behave identically regardless of
backend. `pre.js` is reduced to WASM-only concerns (env propagation, memory
report).

Embind retains ownership metadata in its JavaScript `ClassHandle`. Its `close()`
therefore delegates to Embind's idempotent `delete()` method rather than deleting
the C++ object from a bound method. Deleting from C++ leaves the live handle with
a dangling pointer; a later explicit delete or finalizer can then double-delete
it and corrupt the WASM runtime. N-API classes keep their native `close()`
implementation because they do not use Embind `ClassHandle`s.

### `index.ts`

Exports both factories. The default export can select a backend (native when the
addon is present, WASM otherwise), or callers import the one they want
explicitly. Type exports are unchanged.

---

## Concurrency model

The engine is multi-threaded; this binding, as scoped for BC-236, is a
**synchronous single-caller** API on top of it. Those two facts do not cancel
out, but they do not compound the way the word "multi-threaded" might suggest.
Be precise about what each layer provides.

**What is genuinely concurrent (engine-internal, active regardless of the
binding):**

- **Parallel recovery** — `DB::open` rebuilds the key directory across multiple
  threads. This runs on every native open.
- **Background hint-file writer** — after each file rotation, a background thread
  writes the hint file off the write path. This runs on every native rotation.

**What is *not* reachable from JS with the synchronous binding:**

- **Concurrent user operations.** Every `get`/`put`/`del`/`applyBatch` call runs
  on the Node **main event-loop thread and blocks it**. From a single JS thread,
  user operations are serialized — exactly one is in the engine at a time.
- **Group-commit coalescing.** Group commit amortizes `fdatasync` across writers
  that contend for the write mutex. A synchronous binding on one event-loop
  thread never produces two in-flight writers, so group commit degenerates to
  one writer per commit — it never batches. The engine machinery is present but
  idle.
- **Useful `durableSequence` blocking.** With `sync: true`, a write's
  `fdatasync` completes inline before `put` returns, so the durable sequence is
  already at the target — `durableSequence(min)` returns immediately. The
  blocking long-poll form only advances if *another* thread writes meanwhile;
  under a single-threaded caller nothing does, so a positive timeout just stalls
  the event loop until it expires. Treat it as a poll, not a wake-up, in this
  binding.

**Node Worker Threads do not lift this by themselves.** Workers are separate V8
isolates in the same process. Two blockers:

- N-API `ObjectWrap` objects are bound to one `Napi::Env`/isolate — a wrapped
  `DB` **cannot be passed between workers**. Sharing one engine instance means
  sharing the underlying C++ pointer out-of-band (e.g. `Napi::External` through
  `workerData`) with manual lifetime management. Not part of BC-236.
- The naive "each worker calls `open()` on the same directory" fails: the
  directory `flock(LOCK_EX | LOCK_NB)` (BC-170) conflicts across open file
  descriptions *within the same process*, so the second worker gets
  `std::system_error`. The lock exists precisely to prevent double-open.

**Future paths to real user-op concurrency (out of scope for BC-236):**

1. **Async variants** via `Napi::AsyncWorker` — each op runs on a libuv
   threadpool thread, so concurrent async calls from one JS thread genuinely
   contend in the engine and group commit begins to coalesce. Would live
   *beside* the synchronous methods.
2. **Shared native `DB` handle across workers** — one `open`, its pointer shared
   to sibling workers, each wrapping it in its own isolate. Engine access is
   already thread-safe; the work is the cross-isolate handle plumbing and
   lifetime/ownership rules.

---

## Semantic differences (behavior, not interface)

The interface is identical; the runtime characteristics differ. Callers do not
change code, but operators should know:

| Aspect | Native | WASM |
| --- | --- | --- |
| Engine threads | Parallel recovery + background hint writer run on their own threads | Single-threaded (`BYTECASK_SINGLE_THREADED`) |
| I/O | Direct syscalls | NODEFS passthrough |
| `durableSequence(min, timeout>0)` | Blocks the **event loop** until the durable sequence reaches `min` or the timeout expires | Blocks the single thread; nothing advances the sequence meanwhile |
| 64-bit sequences | `Napi::BigInt` | `-sWASM_BIGINT` |
| User-op concurrency | Serialized on the Node event loop, one caller at a time; group commit present but does not coalesce (see Concurrency model) | Serialized on the single thread |

`durableSequence` with a positive timeout is the one call that can stall the
Node event loop. It stays synchronous for parity and is documented as
event-loop-blocking; an async (`AsyncWorker`-based) variant is a possible
follow-up but is out of scope for BC-236 and would live **beside** the
synchronous method, not replace it.

---

## Testing

The strongest parity proof is running the **existing** vitest integration suite
(`bytecaskdb-node/test/integration/*`) against the native backend as well as the
WASM one. The suite is written against the `ByteCaskFactory` interface, so it is
backend-agnostic:

- Parameterize the test harness / `global-setup.ts` over a backend factory
  (`createNativeBackend` vs `createWasmBackend`) so every existing test runs
  twice.
- The same assertions (basic ops, ranges, transactions, iterators, stats,
  boundaries, error paths, lifecycle) must pass on both.
- Native-only coverage worth adding: multi-threaded (parallel) recovery on
  reopen and background hint-file generation after rotation — engine-internal
  concurrency WASM cannot exercise. Note: the synchronous single-caller API
  cannot itself produce contending writers, so group-commit coalescing is not
  reachable from JS without the async variants listed under Concurrency model.

Follows the repo testing rule: narrowest coverage that proves the change, reusing
the existing seam rather than building a new one.

---

## Implementation status (BC-236)

All four phases are complete and merged on branch `bc-236-native-node-binding`.

| Phase | Scope | Result |
| --- | --- | --- |
| P1 — Native addon scaffold | `bytecaskdb_node` xmake target, `native/bytecask_napi.cpp` module init, `build:native` npm script, `node-addon-api`/`node-api-headers` dependency wiring. | Done. `xmake build bytecaskdb_node` builds cleanly; `require("../native/bytecask.node")` loads without crashing. |
| P2 — API parity wrapper set | `NapiDB`, `NapiSnapshot`, `NapiWritePlan`, all iterator wrappers (`BC_DEFINE_ITERATOR` macro), `NapiFileManifest`, option parsing, BigInt sequence handling, `nullopt`→`null` semantics. | Done. Shared integration suite passes 106/106 against the native backend (see Testing below). |
| P3 — Shared JS backend selection | `src/native-backend.ts`, `src/dispose.ts` (shared disposal/iterator wiring extracted from `wasm/pre.js`), both factories exported from `src/index.ts`. | Done. `BC_TEST_BACKEND=native` switches the whole integration suite to the native backend with no test-source changes. |
| P4 — Validation + docs sync | Native/WASM test matrix, native-only recovery/rotation checks, README + project plan updates. | Done. `test/integration/native-recovery.test.ts` (native-only, `describe.skipIf`) covers reopen-recovers-all-keys and reopen-after-rotation-recovers-sealed-file-keys. |

### Implementation notes not anticipated in the original design

- **`NODE_ADDON_API_CPP_EXCEPTIONS_ALL` is required, not optional.**
  node-addon-api's `WrapCallback` only catches `Napi::Error` under
  `NAPI_CPP_EXCEPTIONS` alone. ByteCaskDB throws plain `std::exception`
  subclasses (`std::system_error`, `std::runtime_error`,
  `std::invalid_argument`, `std::filesystem::filesystem_error`) — never
  `Napi::Error`. Without also defining `NODE_ADDON_API_CPP_EXCEPTIONS_ALL`,
  any such exception escaping a bound method calls `std::terminate()` and
  kills the whole Node process (not catchable from JS `try`/`catch` — the
  process is already gone). Both defines are set on the `bytecaskdb_node`
  xmake target.
- **`bytecask::DB` is non-copyable and non-moveable.** `NapiDB` stores it as
  `std::unique_ptr<bytecask::DB>`, constructed via `new bytecask::DB(bytecask::DB::open(...))`
  (not `std::make_unique`, which would materialize an intermediate temporary
  requiring a move). `Close()` calls `db.reset()` for deterministic release
  of the directory lock — relying on GC to eventually collect the wrapper
  would leave the lock held indefinitely, which broke reopen-after-close in
  testing (`std::system_error`: directory locked).
- **Buffers, not `Uint8Array`, would have broken parity.** `get()`/iterators
  return `Napi::Uint8Array` constructed from a fresh `Napi::ArrayBuffer` +
  `memcpy`, not `Napi::Buffer<uint8_t>::Copy`. A Node `Buffer` is a `Uint8Array`
  subclass with extra prototype methods; tests asserting
  `result.constructor.name === 'Uint8Array'` (matching what Embind/WASM
  returns) fail against a `Buffer`. Input parsing uses `IsTypedArray()`
  (not `IsBuffer()`) so both `Buffer` and plain `Uint8Array` are accepted.
- **Embind disposal must use `ClassHandle.delete()`.** The original bound
  `close()` methods used `delete &self`. That freed C++ storage without marking
  the owning JavaScript `ClassHandle` deleted, so later explicit disposal or
  finalization double-deleted it and caused `table index is out of bounds` and
  `memory access out of bounds` failures. The shared disposal helper now detects
  Embind classes and implements idempotent `close()` through `delete()`.

## Non-goals (BC-236)

- Binary keys — blocked on a shared `types.ts` change affecting both backends.
- Async/Promise variants of write or fsync-bound methods.
- Prebuilt binary distribution via npm (`prebuildify`/`node-pre-gyp`) — the
  first pass builds from source through xmake, as the Python binding does.
- Replacing the WASM backend — both ship; callers choose.

---

## Implementation plan (BC-236)

> All four phases below are complete — see
> [Implementation status (BC-236)](#implementation-status-bc-236) for the
> as-built result and notes on where the implementation diverged from plan.

Track BC-236 as four small phases so each step is verifiable before the next.

| Phase | Scope | Exit criteria |
| --- | --- | --- |
| P1 — Native addon scaffold | Add `bytecaskdb_node` xmake target, `native/bytecask_napi.cpp` module init, `build:native` npm script, and `node-addon-api` dependency wiring. | `xmake build bytecaskdb_node` succeeds and `require("../native/bytecask.node")` loads in Node without crashing. |
| P2 — API parity wrapper set | Implement `NapiDB`, `NapiSnapshot`, `NapiWritePlan`, all iterator wrappers, `NapiFileManifest`, plus identical option parsing, BigInt sequence handling, and nullopt→`null` semantics. | Shared integration tests pass for CRUD, batch, snapshots, iterators, replication primitives, and error paths against native backend. |
| P3 — Shared JS backend selection | Add `src/native-backend.ts`, extract shared disposal/iterator wiring to `src/dispose.ts`, and export both factories from `src/index.ts`. | Existing TypeScript call sites compile unchanged and can switch backend by factory import only. |
| P4 — Validation + docs sync | Run native and WASM test matrix, add native-only recovery/rotation checks, and update README + project plan with status and caveats. | Test matrix is green (or failures are documented as pre-existing) and docs describe backend selection and sync-call blocking behavior. |

### Validation checklist

- Reuse the existing `ByteCaskFactory` integration seam in
  `bytecaskdb-node/test/integration/`.
- Execute the same suite for both backends, not separate bespoke tests.
- Add narrow native-only checks for:
  - parallel recovery on reopen (`Options.recovery_threads > 1`);
  - background hint generation after file rotation.
- Keep `durableSequence(min, timeout)` synchronous and explicitly documented as
  event-loop-blocking in native mode.

### Follow-up backlog after BC-236

Tracked as BC-237 in `docs/bytecask_project_plan.md`:

- Async Promise-based variants for fsync-bound calls (`AsyncWorker`), kept
  beside synchronous APIs.
- Cross-worker shared-handle attach flow (Appendix A) with token registry and
  explicit ownership semantics.
- Optional prebuilt binary distribution once API and ABI settle.

---

## Related documents

- `docs/engine_api_design.md` — the C++ engine API these bindings wrap.
- `docs/replication_primitives_design.md` — `durableSequence`, `changesSince`,
  `ingest`, `createManifest` semantics surfaced by both bindings.
- `bytecaskdb-node/wasm/API.md` — the JavaScript API spec both backends satisfy.
- `bytecaskdb-python/` — the nanobind binding and `bytecaskdb_python` xmake
  target this design mirrors for toolchain and linking.

---

## Appendix A — Cross-worker DB sharing (future work)

Out of scope for BC-236, captured here so the design exists. This is the second
path from the [Concurrency model](#concurrency-model) to real user-op
concurrency: one `open`, its engine shared to sibling Node Worker Threads, each
wrapping it in its own isolate. It works because Worker Threads are **threads in
one process**, not separate processes — they share the address space, so a
native pointer created in one worker is dereferenceable in every other. The only
thing that cannot cross a worker boundary is a V8/N-API object, so the shared
thing stays purely native and workers exchange a small validated token.

### A.1 Own the engine with a `shared_ptr` behind a process-global registry

The engine is non-moveable, but it is already heap-allocated inside a wrapper
(the Embind layer does `new JsDB{...}` with `bytecask::DB db;` mem-initialized
from `DB::open` — guaranteed copy elision, the DB is never moved). Do the same
into a shared control block:

```cpp
struct EngineHolder {
  bytecask::DB db;
  EngineHolder(std::filesystem::path dir, bytecask::Options o)
      : db{bytecask::DB::open(std::move(dir), std::move(o))} {}  // flock taken once, here
};

// The ONE piece of deliberately process-shared state. The .node is dlopen'd
// once per process, so this static is shared across all workers. Guard it.
static std::mutex g_reg_mu;
static std::unordered_map<std::string, std::shared_ptr<EngineHolder>> g_registry;
```

- `ByteCaskDB.open(path, opts)` (static): `std::make_shared<EngineHolder>(...)`,
  insert under a fresh **opaque token** (random 128-bit hex, or a monotonic
  counter), and return a `NapiDB` holding that `shared_ptr`. Expose
  `db.handle()` → the token.
- `ByteCaskDB.attach(token)` (static): lock, look up the token, copy the
  `shared_ptr` into a **new** `NapiDB` in *this* worker's isolate. Unknown
  token → clean `Napi::Error`, never a wild dereference.

### A.2 Cross the boundary with the token, not the object

```js
// main thread
const db = ByteCaskDB.open('/data/mydb');   // takes the directory flock ONCE
const token = db.handle();                  // opaque string, structured-cloneable
for (let i = 0; i < 4; i++)
  new Worker('./worker.js', { workerData: { token } });

// worker.js
const db = ByteCaskDB.attach(workerData.token);  // NO second open, NO second flock
db.put('worker' + threadId, 'x');                // 4 workers → 4 OS threads → engine
```

Why a token and not the raw pointer as a BigInt? Both dereference in the same
address space, but a raw `uintptr_t` is a wild pointer waiting to happen (a
stale or garbage number is memory corruption). The registry gives the project's
runtime-safety tier: a bad token is a lookup miss → exception, not UB.
`Napi::External` cannot help here — it is an isolate-bound JS value and
`postMessage` / structured clone rejects it, so it cannot cross workers anyway.

### A.3 Lifetime / ownership — the hard part, enforced by `shared_ptr`

- **The registry holds one strong ref** (the "primary", created by `open`).
  **Each attached worker's `NapiDB` holds its own strong ref.** The
  `bytecask::DB` destructor runs only when the *last* ref drops — after the last
  worker finishes. → no use-after-free, and **exactly one close**.
- **`close()` on the primary** erases the registry entry (drops the registry's
  ref). If workers are still attached, the engine stays alive until they
  release; if none are, it closes immediately.
- **The directory lock is sidestepped entirely.** Only `open` calls `flock`;
  `attach` never does. This avoids the BC-170 double-open collision that blocks
  the naive per-worker-`open` approach (flock conflicts across open file
  descriptions even within one process).
- **Only the DB handle crosses workers.** `Snapshot`, `WritePlan`, and iterators
  are per-isolate `ObjectWrap`s holding engine-internal cursors — they stay
  worker-local. This is a hard rule, not a suggestion.
- **The registry mutex guards insert/lookup/erase only.** Engine *operations*
  need no extra lock — the engine is already lock-free-read / single-writer, so
  concurrent `NapiDB` calls from different workers are safe as-is.

Residual leak to document, not a crash: if a worker is `worker.terminate()`'d,
its finalizer may not run, so its ref leaks and the engine stays open until
process exit. Safe (process teardown reclaims), just not eager.

### A.4 Build requirement — the addon must be context-aware

A worker that `require`s a non-context-aware addon errors. node-addon-api's
standard `NODE_API_MODULE(addon, Init)` is context-aware: `Init(env, exports)`
runs **once per worker**, defining the `ByteCaskDB` constructor fresh in each
isolate. Per-isolate class functions live in instance data
(`env.SetInstanceData` / `Napi::Addon`); the registry is the sanctioned
exception — global on purpose, and synchronized.

### A.5 The payoff

Once several workers on several OS threads call `put` concurrently, they contend
on the write mutex and **group commit coalesces their `fdatasync`s** — the exact
multi-threaded behavior the WASM backend can never reach and the synchronous
single-caller binding leaves idle. This is what turns "the engine is
multi-threaded" into "callers get multi-threaded throughput."
