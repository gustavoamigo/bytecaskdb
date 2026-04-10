# ByteCaskDB — Project Organization

## License Boundaries

ByteCaskDB is **dual-licensed** by design: the storage engine is MIT, and the
MariaDB integration is GPL-2.0. The boundary is strict and physical — it maps
directly to directory boundaries in the repository.

Every source file carries an SPDX identifier on its first line:

```
// SPDX-License-Identifier: MIT          ← engine, tests, benchmarks, C bridge
// SPDX-License-Identifier: GPL-2.0-only ← mariadb/ only
```

This is machine-readable by GitHub's license detector, `licensee`, the REUSE
tool, and most IDE license-compliance scanners. `GPL-2.0-only` is used (not the
ambiguous `GPL-2.0`) because MariaDB's plugin API does not grant "or later"
permissions.

### MIT — Core Engine (`bytecaskdb/`, `src/`, `include/`)

Everything needed to build or embed ByteCaskDB as a library is MIT:

| Path | What it is |
|------|------------|
| `bytecaskdb/*.cppm` | C++23 module sources — the engine |
| `bytecaskdb/bytecask.cpp` | Engine entry point |
| `src/bytecask_c.cpp` | C ABI bridge (no MariaDB headers; MIT-clean) |
| `include/bytecask_c.h` | Public C API declaration |

These files contain **no MariaDB headers** and impose no GPL obligations on
callers. A third party can embed them, link them, or ship them under their
own MIT-compatible terms.

### GPL-2.0 — MariaDB Integration (`mariadb/`)

Files under `mariadb/` `#include` MariaDB server headers (`handler.h`,
`ha_proto.h`, etc.). Including those headers creates a GPL-2.0 boundary.
Every `.cc`/`.h` file in this directory is GPL-2.0 as required by the MariaDB
plugin API.

| Path | What it is |
|------|------------|
| `mariadb/ha_bytecaskdb.h` | Storage engine handler (MariaDB C++ API) |
| `mariadb/ha_bytecaskdb.cc` | Handler implementation |
| `mariadb/bytecaskdb_plugin.cc` | Plugin declaration (`maria_declare_plugin`) |
| `mariadb/key_encoding.{h,cc}` | MariaDB key → bytecask key encoding |
| `mariadb/row_encoding.{h,cc}` | MariaDB row → bytecask value encoding |
| `mariadb/CMakeLists.txt` | CMake build for the plugin |

`src/bytecask_c.cpp` is compiled by `mariadb/CMakeLists.txt` alongside the
plugin sources. It is **not** compiled by xmake — this is intentional: it
keeps the xmake-built static library (`libbytecask.a`) free of any C ABI
symbols, so tests link against a pure C++23 engine with no C wrapper baggage.

---

## Directory Layout

```
bytecask/
│
├── bytecaskdb/           # MIT — C++23 engine modules
│   ├── *.cppm            # C++23 module sources (import bytecask;)
│   └── bytecask.cpp      # Engine definition (module implementation unit)
│
├── src/                  # MIT — C ABI bridge
│   └── bytecask_c.cpp    # C API implementation — compiled by MariaDB CMake, not xmake
│
├── include/              # MIT — public headers for out-of-tree consumers
│   └── bytecask_c.h      # Stable C API (no C++ types, no MariaDB types)
│
├── mariadb/              # GPL-2.0 — MariaDB storage engine plugin
│   ├── CMakeLists.txt    # Standalone CMake build
│   ├── ha_bytecaskdb.h/cc
│   ├── bytecaskdb_plugin.cc
│   ├── key_encoding.h/cc
│   └── row_encoding.h/cc
│
├── tests/                # MIT — engine tests (no C API, no MariaDB)
├── benchmarks/           # MIT — engine benchmarks
├── docs/                 # Documentation
├── scripts/              # Build and benchmark helper scripts
└── xmake.lua             # xmake build — covers bytecaskdb/, tests/, benchmarks/ only
```

`xmake.lua` has **no knowledge** of `mariadb/` or `src/bytecask_c.cpp`. The
plugin is built independently with CMake, which consumes `libbytecask.a`
produced by xmake.

---

## Build Systems

Two independent build systems, for two independent concerns:

### xmake — Core Engine

Builds the C++23 engine. Targets:

| Target | Kind | Description |
|--------|------|-------------|
| `bytecask_tests` | binary | Test suite (Catch2) |
| `engine_bench` | binary | Engine benchmarks vs RocksDB |
| `bytecask_bench` | binary | Map structure benchmarks |
| `bytecask` | static lib | `libbytecask.a` — consumed by MariaDB CMake |

The `bytecask` static target compiles only `bytecaskdb/` sources. It does
**not** include `src/bytecask_c.cpp`, so tests link against a pure C++23
engine with no C ABI symbols.

Build:
```sh
xmake                         # builds tests (default)
xmake build bytecask          # produces libbytecask.a
```

### CMake — MariaDB Plugin

Builds the GPL storage engine plugin. Consumes `libbytecask.a`,
`include/bytecask_c.h`, and `src/bytecask_c.cpp`.

```sh
xmake build bytecask          # prerequisite: build the static lib first
cmake -S mariadb -B mariadb/build
cmake --build mariadb/build
```

Produces `ha_bytecaskdb.so`, installable via:
```sql
INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';
```

---

## The C API Bridge (`src/bytecask_c.cpp`)

`bytecask_c.cpp` sits at the boundary between the two license zones:

- It `#include`s only `include/bytecask_c.h` and `import`s only `bytecask`
- It contains **zero** MariaDB headers → it is MIT-clean
- It is compiled by `mariadb/CMakeLists.txt` as part of the plugin, not by xmake

This means the GPL obligation comes only from MariaDB headers present in other
`mariadb/` files — not from `bytecask_c.cpp` itself. The file is MIT, but it
ends up inside the GPL-licensed shared object because it is compiled together
with the plugin.

The CMakeLists reaches it as `${BYTECASK_ROOT}/src/bytecask_c.cpp`.
