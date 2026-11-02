# ByteCaskDB

> **Status: early development.** The core engine works and is well-tested, but the API and on-disk format may change before a stable release. Not recommended for production use yet.

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/gustavoamigo/bytecaskdb)
[![CI](https://github.com/gustavoamigo/bytecaskdb/actions/workflows/ci.yml/badge.svg)](https://github.com/gustavoamigo/bytecaskdb/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/gustavoamigo/bytecaskdb/branch/main/graph/badge.svg)](https://app.codecov.io/gh/gustavoamigo/bytecaskdb)

**ByteCaskDB** is a fast, predictable embedded key-value store written in C++, designed for flat read and write latency as the dataset grows.

Every key has an entry in memory at all times — a deliberate design choice that removes an entire class of complexity that exists to minimise disk access, and gives every point lookup an in-memory traversal followed by one read at a known offset. The entry holds no key bytes, which stay in the data file, so a key costs 14–19 bytes of RAM whatever its length (measured at 1M keys, structured to random): 100 million keys come to 1.4–1.9 GB. 

Built on the [Bitcask](https://riak.com/assets/bitcask-intro.pdf) append-only foundation, ByteCaskDB replaces the original hash-table key directory with a **[persistent B+ tree with blind leaves](docs/blind_leaf_btree_design.md)**. It gives ordered range queries, prefix scans and O(1) snapshots while keeping the simplicity that makes Bitcask fast. Full MVCC and serializable conflict detection need no separate transaction type.

## Features

- **Flat read latency** — lock-free reads, one record read per lookup: p50 172 ns, p99 378 ns at 1M keys, 62.9 Mops/s at 32 threads.
- **Small, predictable memory** — 14–19 bytes of RAM per key, independent of key length.
- **Ordered iteration** — forward and reverse prefix scans, key-only scans, O(1) snapshots, and `del_range` as a single append.
- **Transactions** — atomic batches, snapshot isolation, and serializable conflict detection through read and range guards.
- **Durable by construction** — append-only, CRC-checked records, group commit for synchronous writers, and online vacuum; `close()` throws if any acknowledged write is not durable.
- **Fast recovery** — parallel rebuild from compressed hint files: 10M keys in 0.47 s from a cold start on a SATA SSD.
- **Failure-tested correctness** — model-based, bounded exhaustive testing of every failure class of each write-path operation. Guarantees are stated explicitly in [`CONTRACT.md`](CONTRACT.md), each row naming the tests that prove it, alongside fault sweeps that fail every I/O call and allocation in turn, a chaos rig that cuts the power, and [Elle](https://github.com/jepsen-io/elle) checking isolation. [How it is validated](docs/correctness_validation.md).

Also: a bounded `O_DIRECT` buffer pool, replication primitives with fork detection, and operational counters. The complete list, with mechanisms, is in [`docs/bytecask_design.md`](docs/bytecask_design.md#features); the guarantees, each with the test that proves it, are in [`CONTRACT.md`](CONTRACT.md).

## Performance

1M keys, 245-byte values, mean of 5 runs. Per-value CRC verification is off for reads; hint file CRC is always verified during recovery.

| Operation | Result |
|-----------|-------:|
| Get | 4.72 Mops/s (p50 172 ns, p99 378 ns) |
| Get, 32 threads | 62.9 Mops/s |
| Put (NoSync) | 99.0 Kops/s |
| Put (Sync) | 161.8 ops/s |
| Put (Sync), 64 threads | 8.4 Kops/s |
| Del (Sync) | 157.8 ops/s |
| Range-50 | 441.4 K scans/s |
| MixedBatch (Sync) | 14.6 Kops/s |
| Recovery, 10M keys, cold start, 8 threads | 0.47 s |

Sync writes are bound by `fdatasync` latency; concurrent writers share one (group commit). Unsynced puts slow down as the dataset grows: 149.7 Kops/s at 50k keys, 99.0 Kops/s at 1M. A scan reads each value from its own record, and the benchmark dataset fits in memory, so scans over larger data were not measured. Key-only iteration (`keys_from`) skips the value.

_AMD Ryzen 7 3700X (8C/16T), SATA SSD, 31 GiB RAM; run of 2026-10-07 at commit `e62d83f`. Other thread counts, dataset sizes and the recovery table: [`docs/performance.md`](docs/performance.md)._

## Quick Start

```cpp
import bytecask;
using namespace bytecask;

auto db = DB::open("my_db");                          // open or create

db.put({}, to_bytes("user:1"), to_bytes("alice"));

Bytes out;
bool found = db.get({}, to_bytes("user:1"), out);     // true; value in out
db.del({}, to_bytes("user:1"));

// Atomic batch: every operation lands, or none does.
WritePlan plan;
plan.put(to_bytes("user:2"), to_bytes("bob"));
plan.del(to_bytes("user:1"));
db.apply_batch({}, std::move(plan));

// Optimistic transaction: keys you write are checked against the snapshot.
auto snap = db.snapshot();
Bytes stock;
snap.get({}, to_bytes("stock:widget"), stock);
WritePlan txn{std::move(snap)};
txn.put(to_bytes("stock:widget"), new_stock);
if (!db.apply_batch({}, std::move(txn))) { /* conflict: retry */ }

// Prefix scan, in key order; values are read lazily.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) { /* ... */ }

db.close();   // makes every write durable; throws if it could not
```

More examples (guards, range deletes, reverse scans) and the full interface are in [`docs/api_reference.md`](docs/api_reference.md); the source of truth is [`include/bytecask.hpp`](include/bytecask.hpp).

## Architecture

```
  ByteCaskDB
  ├── Key Directory  PersistentBlindBTree               (an entry per key, in memory; no key bytes)
  ├── File Registry  map<file_id, DataFile>             (open file descriptors)
  ├── Active File    append-only .data file             (current writes)
  └── Sealed Files   read-only .data + .hint files      (older segments)
```

- **Write**: every write goes through one coordinator (`apply_batch`) that appends CRC-checked records to the active file. Concurrent sync writers share one `fdatasync` (group commit).
- **Read**: a reader takes an immutable snapshot, finds the key's file and offset in the key directory, and reads the record. No locks.
- **Recovery**: `open` replays hint files in parallel to rebuild the key directory; a data file without a hint is indexed first. Hints are a rebuildable index, never the source of truth.

Design principles, in priority order: correctness, simplicity, predictable latency over peak throughput, performance. [`docs/bytecask_intro.md`](docs/bytecask_intro.md) is a five-minute overview; [`docs/bytecask_design.md`](docs/bytecask_design.md) is the full reference. [`bytecaskdb-python/reference/bytecask_ref.py`](bytecaskdb-python/reference/bytecask_ref.py) is the whole model in one plain-Python file, checked against the engine in CI.

## Building

Requires **Clang** with C++23 modules, [xmake](https://xmake.io), and `nanobind` for the Python extension. A [Dev Container](.devcontainer) (Fedora 43) has everything installed.

```bash
xmake build bytecask_tests && xmake run bytecask_tests   # build and test
python ./scripts/run_engine_bench.py                     # engine benchmark (needs RocksDB)
```

The key directory is chosen at build time: the blind-leaf B+ tree by default, or `BYTECASK_KEYDIR=btree` for the B+ tree that keeps key bytes in its leaves. Both pass the same suite and share the on-disk format. MariaDB plugin benchmarks are in [`bytecaskdb-mariadb-plugin/README.md`](bytecaskdb-mariadb-plugin/README.md).

## Documentation

| Start here | |
|---|---|
| [`docs/bytecask_intro.md`](docs/bytecask_intro.md) | Five-minute overview |
| [`docs/bytecask_design.md`](docs/bytecask_design.md) | Living design reference: features, architecture, concurrency, vacuum, recovery |
| [`docs/api_reference.md`](docs/api_reference.md) | Usage examples and the public interface |
| [`CONTRACT.md`](CONTRACT.md) | What the engine guarantees, each row with the test that proves it |
| [`docs/file_format.md`](docs/file_format.md) | On-disk data and hint file formats |
| [`docs/performance.md`](docs/performance.md) | Benchmark tables and methodology |
| [`docs/correctness_validation.md`](docs/correctness_validation.md) | Failure classes, fault injection, MC/DC coverage |
| [`docs/project_organization.md`](docs/project_organization.md) | Repository layout, build systems, license boundaries |

Component designs (key directory, buffer pool, replication, recovery, hints, transactions) are the other files in [`docs/`](docs/). Language bindings: [`bytecaskdb-python/`](bytecaskdb-python/), [`bytecaskdb-node/`](bytecaskdb-node/).

## Contributing

ByteCaskDB is early-stage and contributions of every size are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md); the fastest start is GitHub Codespaces. Forks are fine — that's what the MIT license is for.

## License

**MIT** for the core engine (`bytecaskdb/`, `include/`, `tests/`, `benchmarks/`; see [`LICENSE`](LICENSE)); **GPL-2.0-only** for the MariaDB plugin (`bytecaskdb-mariadb-plugin/`), as its plugin API requires. Boundaries: [`docs/project_organization.md`](docs/project_organization.md).
