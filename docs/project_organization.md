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
| `include/bytecask.hpp` | Public C++ header for out-of-tree consumers (no module imports) |
| `bytecaskdb/bytecask_hpp.cpp` | Its implementation, bridging to the modules (no MariaDB headers; MIT-clean) |

These files contain **no MariaDB headers** and impose no GPL obligations on
callers. A third party can embed them, link them, or ship them under their
own MIT-compatible terms.

### MIT — SQL Layer (`bytecaskdb-sql-layer/`)

A SQL layer on the engine's public API (#487), MIT like the core. It links
`libbytecask.a` through `include/bytecask.hpp`, as the plugin does, and
includes no MariaDB header: the plugin may depend on it, never the reverse,
and CI builds it with no MariaDB development package installed. Today it
holds the end-to-end prototype of #497; files marked `// prototype:` are
disposable and will be replaced by the designed layers.

Its C++ is organized as C++23 modules, as the engine's is: one interface unit
per module, named `bytecask.sql.<name>`, with the exported declarations, and
an implementation unit beside it (`<name>.cpp`, `module bytecask.sql.<name>;`)
with the definitions, so a body edit recompiles no importer.

| Module | File | What it is |
|--------|------|------------|
| `bytecask.sql.key_codec` | `codec/key_codec.cppm` | Mem-comparable key encoding |
| `bytecask.sql.row_codec` | `codec/row_codec.cppm` | Row format |
| `bytecask.sql.catalog` | `catalog/catalog.cppm` | Tables, permanent column ids, indexes, persisted under namespace `0x01` |
| `bytecask.sql.runtime` | `algebra/runtime.cppm` | The C++ side of the plan API: the types `bc_plan.h` leaves opaque, and the symbol table a run-time loader links plans against |
| `bytecask.sql.ast` | `sql/ast.cppm` | The AST the parser hands the binder |
| `bytecask.sql.parser` | `sql/parser.cppm` | libpg_query behind `parse()`; `parser.cpp` is the only unit that sees its headers |
| `bytecask.sql.binder` | `sql/binder.cppm` | Names to ids, parameter types from use |
| `bytecask.sql.codegen` | `plan/codegen.cppm` | Planner and code generator |
| `bytecask.sql.tcc_loader` | `plan/tcc_loader.cppm` | TinyCC in process; `tcc_loader.cpp` is the only unit that sees `libtcc.h` |
| `bytecask.sql.api` | `api/database.cppm` | The embedded API |

One header: `algebra/bc_plan.h`, the C contract, which generated plans,
TinyCC and clang-as-C read. Its `extern "C"` definitions are
`algebra/plan_api.cpp`, a plain translation unit that includes the header and
imports the modules, standard headers first, for the reason
`bytecaskdb/bytecask_hpp.cpp` gives. The hand-written plans in `plans/*.c`
are C. The tests import the modules; the fixtures they share are modules too
(`bytecask.sql.tests.*`, under `tests/`). Two more headers of the same kind
are planned, each at a boundary another toolchain consumes: the C ABI for the
bindings (#494) and a header bridge for the plugin, the counterpart of
`include/bytecask.hpp` (#488).

Its third-party dependencies are fetched at build time, not vendored:
libpg_query (PostgreSQL licence, with its bundled protobuf-c, MIT/BSD) and
TinyCC (LGPL-2.1). The xmake targets that use them are not built by default;
the `build-and-test` job in `ci.yml` builds and runs `sql_layer_tests` with
the engine suite.

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

`bytecaskdb/bytecask_hpp.cpp` is compiled by xmake into `libbytecask.a` (it
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
│   └── bytecask_hpp.cpp   # bytecask.hpp's implementation (compiled by xmake into libbytecask.a)
│
├── include/              # MIT — public headers for out-of-tree consumers
│   └── bytecask.hpp      # PIMPL C++ header (no module imports, no MariaDB types)
│
├── bytecaskdb-mariadb-plugin/  # GPL-2.0 — MariaDB storage engine plugin
│   ├── CMakeLists.txt    # Standalone CMake build
│   ├── ha_bytecaskdb.h/cc
│   ├── bytecaskdb_plugin.cc
│   ├── key_encoding.h/cc
│   └── row_encoding.h/cc
│
├── bytecaskdb-sql-layer/ # MIT — SQL layer on the public API (#487)
│   ├── algebra/          # bc_plan.h (the one header), plan_api.cpp, runtime.cppm
│   ├── codec/ catalog/ sql/ plan/ api/   # bytecask.sql.* interface (.cppm) and implementation (.cpp) units
│   ├── plans/            # Hand-written plans, C
│   ├── tests/            # Catch2 tests; shared fixtures as bytecask.sql.tests.* modules
│   └── xmake.lua         # sql_layer, sql_layer_tests; libpg_query and TinyCC package rules
│
├── tests/                # MIT — engine tests (no MariaDB)
├── benchmarks/           # MIT — engine benchmarks
├── docs/                 # Documentation
├── scripts/              # Build and benchmark helper scripts
└── xmake.lua             # xmake build — bytecaskdb/, tests/, benchmarks/; includes bytecaskdb-sql-layer/xmake.lua
```

`xmake.lua` builds `bytecaskdb/bytecask_hpp.cpp` into `libbytecask.a`. The
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
| `sql_layer` | static lib | `libsql_layer.a` — the `bytecask.sql.*` modules and `plan_api.cpp`, from `bytecaskdb-sql-layer/xmake.lua` |
| `sql_layer_tests` | binary | SQL layer tests (Catch2), from `bytecaskdb-sql-layer/xmake.lua` |

The `bytecask` static target compiles `bytecaskdb/` sources plus
`bytecaskdb/bytecask_hpp.cpp` (the header's bridge to the modules). Tests
link against their own engine objects without it.

Build:
```sh
xmake                         # builds tests (default)
xmake build bytecask          # produces libbytecask.a
```

### CMake — MariaDB Plugin

Builds the GPL storage engine plugin. Consumes `libbytecask.a` and
`include/bytecask.hpp`.

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

## The Header Bridge (`bytecaskdb/bytecask_hpp.cpp`)

`bytecask_hpp.cpp` sits at the boundary between the two license zones:

- It `#include`s only `include/bytecask.hpp` and `import`s only `bytecask`
- It contains **zero** MariaDB headers → it is MIT-clean
- It is compiled by xmake into `libbytecask.a` (it imports the C++23 module)

This means the GPL obligation comes only from MariaDB headers present in other
`bytecaskdb-mariadb-plugin/` files — not from `bytecask_hpp.cpp` itself. The file is MIT, but it
ends up inside the GPL-licensed shared object because `libbytecask.a` is linked
into the plugin.
