# ByteCaskDB — Project Organization

## License Boundaries

ByteCaskDB is **dual-licensed** by design: the storage engine is MIT, and the
MariaDB integration is GPL-2.0. The boundary is strict and physical — it maps
directly to directory boundaries in the repository.

Every source file carries an SPDX identifier on its first line:

```
// SPDX-License-Identifier: MIT          ← engine, tests, benchmarks, C bridge
// SPDX-License-Identifier: GPL-2.0-only ← bytecaskdb-mariadb-plugin/ only
```

This is machine-readable by GitHub's license detector, `licensee`, the REUSE
tool, and most IDE license-compliance scanners. `GPL-2.0-only` is used (not the
ambiguous `GPL-2.0`) because MariaDB's plugin API does not grant "or later"
permissions.

### MIT — Core Engine (`bytecaskdb/`, `include/`)

Everything needed to build or embed ByteCaskDB as a library is MIT:

| Path | What it is |
|------|------------|
| `bytecaskdb/*.cppm` | C++23 module sources — the engine |
| `bytecaskdb/bytecask.cpp` | Engine entry point |
| `include/bytecask_c.h` | Public C API declaration |
| `bytecaskdb/bytecask_c.cpp` | C ABI bridge (no MariaDB headers; MIT-clean) |

These files contain **no MariaDB headers** and impose no GPL obligations on
callers. A third party can embed them, link them, or ship them under their
own MIT-compatible terms.

### GPL-2.0 — MariaDB Integration (`bytecaskdb-mariadb-plugin/`)

Files under `bytecaskdb-mariadb-plugin/` `#include` MariaDB server headers (`handler.h`,
`ha_proto.h`, etc.). Including those headers creates a GPL-2.0 boundary.
Every `.cc`/`.h` file in this directory is GPL-2.0 as required by the MariaDB
plugin API.

| Path | What it is |
|------|------------|
| `bytecaskdb-mariadb-plugin/ha_bytecaskdb.h` | Storage engine handler (MariaDB C++ API) |
| `bytecaskdb-mariadb-plugin/ha_bytecaskdb.cc` | Handler implementation |
| `bytecaskdb-mariadb-plugin/bytecaskdb_plugin.cc` | Plugin declaration (`maria_declare_plugin`) |
| `bytecaskdb-mariadb-plugin/key_encoding.{h,cc}` | MariaDB key → bytecask key encoding |
| `bytecaskdb-mariadb-plugin/row_encoding.{h,cc}` | MariaDB row → bytecask value encoding |
| `bytecaskdb-mariadb-plugin/CMakeLists.txt` | CMake build for the plugin |

`bytecaskdb/bytecask_c.cpp` is compiled by xmake into `libbytecask.a` (it
imports the C++23 `bytecask` module and must be built with the same toolchain
that produced the BMIs). The file is MIT-licensed — it contains no MariaDB
headers.

---

## Directory Layout

```
bytecask/
│
├── bytecaskdb/           # MIT — C++23 engine modules
│   ├── *.cppm            # C++23 module sources (import bytecask;)
│   └── bytecask.cpp      # Engine definition (module implementation unit)
│   └── bytecask_c.cpp     # C ABI bridge (compiled by xmake into libbytecask.a)
│
├── include/              # MIT — public headers for out-of-tree consumers
│   └── bytecask_c.h      # Stable C API (no C++ types, no MariaDB types)
│
├── bytecaskdb-mariadb-plugin/  # GPL-2.0 — MariaDB storage engine plugin
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

`xmake.lua` builds `bytecaskdb/bytecask_c.cpp` into `libbytecask.a`. The
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

The `bytecask` static target compiles `bytecaskdb/` sources plus
`bytecaskdb/bytecask_c.cpp` (the C ABI bridge). Tests link
against their own engine objects without the C bridge.

Build:
```sh
xmake                         # builds tests (default)
xmake build bytecask          # produces libbytecask.a
```

### CMake — MariaDB Plugin

Builds the GPL storage engine plugin. Consumes `libbytecask.a` and
`include/bytecask_c.h`.

```sh
xmake build bytecask          # prerequisite: build the static lib first
cmake -S bytecaskdb-mariadb-plugin -B bytecaskdb-mariadb-plugin/build
cmake --build bytecaskdb-mariadb-plugin/build
```

Produces `ha_bytecaskdb.so`, installable via:
```sql
INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so';
```

---

## The C API Bridge (`bytecaskdb/bytecask_c.cpp`)

`bytecask_c.cpp` sits at the boundary between the two license zones:

- It `#include`s only `include/bytecask_c.h` and `import`s only `bytecask`
- It contains **zero** MariaDB headers → it is MIT-clean
- It is compiled by xmake into `libbytecask.a` (it imports the C++23 module)

This means the GPL obligation comes only from MariaDB headers present in other
`bytecaskdb-mariadb-plugin/` files — not from `bytecask_c.cpp` itself. The file is MIT, but it
ends up inside the GPL-licensed shared object because `libbytecask.a` is linked
into the plugin.
