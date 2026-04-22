# Plan: Reorganize Node.js code into bytecaskdb-node/

## Context

The WASM/Emscripten code currently lives flat in `emscripten/`. We want to reorganize it into `bytecaskdb-node/` with a structure that supports both WASM and a future native N-API backend, with a shared TypeScript API layer.

**xmake limitation discovered:** xmake's C++23 module rules do not recognize `emcc` as a clang variant (`/usr/share/xmake/rules/c++/modules/support.lua` only matches `clang`/`clangxx`/`clang_cl`). This means `set_policy("build.c++.modules", true)` + `set_toolchains("emcc")` will fail. The WASM build must stay as a shell script.

## Target structure

```
bytecaskdb-node/
  package.json          # npm metadata + scripts (build:wasm, build:ts, test:smoke)
  tsconfig.json         # TypeScript config
  README.md             # moved from emscripten/README.md, updated
  native/
    README.md           # placeholder for future N-API binding
  wasm/
    build.sh            # migrated from emscripten/build.sh (ROOT path adjusted)
    run.sh              # migrated from emscripten/run.sh
    pre.js              # migrated from emscripten/pre.js
    bytecask_embind.cpp # migrated from emscripten/bytecask_embind.cpp
    test_node.cpp       # migrated
    test_nodefs.cpp     # migrated
    test_wasmfs.cpp     # migrated
    bench_main.cpp      # migrated
    catch2_stringmakers.{h,cpp}  # migrated
    API.md              # migrated
    .gitignore          # migrated from emscripten/.gitignore
    build/              # output artifacts (gitignored)
  src/
    index.ts            # entry point — re-exports types + WASM backend
    types.ts            # shared ByteCaskDB/Snapshot/WritePlan interfaces
    wasm-backend.ts     # async factory wrapping the Embind module
  dist/                 # compiled JS output (gitignored)
```

## Steps

### 1. Create directory structure and move files

- `mkdir -p bytecaskdb-node/{wasm,native,src}`
- `git mv` all files from `emscripten/` to `bytecaskdb-node/wasm/` (see file map below)
- `git mv emscripten/README.md bytecaskdb-node/README.md`
- Remove the now-empty `emscripten/` directory

**File map:**

| From | To |
|------|-----|
| `emscripten/build.sh` | `bytecaskdb-node/wasm/build.sh` |
| `emscripten/run.sh` | `bytecaskdb-node/wasm/run.sh` |
| `emscripten/pre.js` | `bytecaskdb-node/wasm/pre.js` |
| `emscripten/bytecask_embind.cpp` | `bytecaskdb-node/wasm/bytecask_embind.cpp` |
| `emscripten/test_node.cpp` | `bytecaskdb-node/wasm/test_node.cpp` |
| `emscripten/test_nodefs.cpp` | `bytecaskdb-node/wasm/test_nodefs.cpp` |
| `emscripten/test_wasmfs.cpp` | `bytecaskdb-node/wasm/test_wasmfs.cpp` |
| `emscripten/bench_main.cpp` | `bytecaskdb-node/wasm/bench_main.cpp` |
| `emscripten/catch2_stringmakers.h` | `bytecaskdb-node/wasm/catch2_stringmakers.h` |
| `emscripten/catch2_stringmakers.cpp` | `bytecaskdb-node/wasm/catch2_stringmakers.cpp` |
| `emscripten/API.md` | `bytecaskdb-node/wasm/API.md` |
| `emscripten/.gitignore` | `bytecaskdb-node/wasm/.gitignore` |
| `emscripten/README.md` | `bytecaskdb-node/README.md` |

### 2. Fix build.sh ROOT path

Single change: `ROOT="$SCRIPT_DIR/.."` → `ROOT="$SCRIPT_DIR/../.."` (now two levels up from `bytecaskdb-node/wasm/`).

### 3. Add MIT license headers

Add the project's standard header to all new files:
```
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
```

Also verify the moved `.cpp`/`.h` files already have it (they do — `bytecask_embind.cpp` already has the header).

### 4. Create TypeScript wrapper (src/)

**`src/types.ts`** — Interfaces matching the Embind API in `API.md`:
- `OpenOptions`, `WriteOptions`, `ReadOptions`
- `Entry` (`{ key: Uint8Array, value: Uint8Array }`)
- `CloseableIterator<T>` (extends `Disposable`, has `next()`, `close()`, `Symbol.iterator`)
- `Snapshot`, `WritePlan`, `ByteCaskDB` (all extend `Disposable`)
- `ByteCaskFactory` (`{ open(), WritePlan }`) — the common interface both backends will implement

**`src/wasm-backend.ts`** — `createWasmBackend(): Promise<ByteCaskFactory>`
- Dynamically imports `../wasm/build/bytecask.mjs`
- Returns a `ByteCaskFactory` wrapping the Embind module

**`src/index.ts`** — Re-exports all types + `createWasmBackend` as default export

### 5. Create package.json and tsconfig.json

**`package.json`:**
- `name: "bytecaskdb"`, `type: "module"`, `main: "dist/index.js"`, `types: "dist/index.d.ts"`
- Scripts: `build:wasm` (runs `wasm/build.sh`), `build:ts` (runs `tsc`), `test:smoke` (runs node smoke test)
- `devDependencies: { "typescript": "^5.4" }`
- `files`: `dist/`, `wasm/build/bytecask.mjs`, `wasm/build/bytecask.wasm`

**`tsconfig.json`:** ES2022 target, Node16 module resolution, strict, declaration output to `dist/`

### 6. Create native/ placeholder

`bytecaskdb-node/native/README.md` — short note explaining this is for future N-API binding.

### 7. Update root README.md

Add a reference to `bytecaskdb-node/` in the project documentation table and mention Node.js/WASM support.

### 8. Update docs/bytecask_project_plan.md

Track this reorganization task.

### 9. Delete emscripten/build/ artifacts

The `emscripten/build/` directory contains compiled artifacts (~15MB). Delete it since the new build path is `bytecaskdb-node/wasm/build/`.

## Key files to modify

- `emscripten/build.sh` → `bytecaskdb-node/wasm/build.sh` (ROOT path fix)
- `bytecaskdb-node/README.md` (update paths in existing content)
- `README.md` (root — add reference to bytecaskdb-node)
- `docs/bytecask_project_plan.md` (track task)

## New files to create

- `bytecaskdb-node/package.json`
- `bytecaskdb-node/tsconfig.json`
- `bytecaskdb-node/src/index.ts`
- `bytecaskdb-node/src/types.ts`
- `bytecaskdb-node/src/wasm-backend.ts`
- `bytecaskdb-node/native/README.md`

## Verification

1. `cd bytecaskdb-node/wasm && bash build.sh` — WASM compilation succeeds
2. `node bytecaskdb-node/wasm/build/bytecask_node.js` — smoke test passes
3. `cd bytecaskdb-node && npx tsc --noEmit` — TypeScript types compile without errors
