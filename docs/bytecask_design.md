# ByteCaskDB Design

## Purpose

ByteCaskDB is a [Bitcask](https://riak.com/assets/bitcask-intro.pdf) implementation with a key architectural difference: it uses an immutable **persistent B+ tree** for the Key Directory instead of a Hash Table. This design choice enables efficient **range queries** and **prefix searches** while maintaining Bitcask's core strengths of fast writes and simple recovery. The name "ByteCaskDB" reflects this hybrid approach: **Bitcask algorithm** + **tree index** = **ByteCaskDB**.

**Core design choice**: all keys live in memory at all times. This eliminates disk-bound index lookups entirely — every point read resolves in the key directory, every range scan is a pure in-memory walk. Database size is bounded by available RAM: at ~70 bytes per unique key (key data + metadata + tree structure overhead), 10 million keys require around 700 MB.

This document is the living design reference for the repository. It should track the current implementation state, the intended architecture, and important constraints.

Canonical location: `docs/bytecask_design.md`.

For a first-pass overview focused on the main write loop, see `docs/bytecask_intro.md`.

## Repository Tooling Notes

`scripts/sys-info.sh` reports host hardware characteristics for the current execution context. The memory section is intentionally minimal and privilege-free: it reports the machine's installed RAM capacity from `/proc/meminfo` instead of attempting detailed DIMM inventory. The disk section resolves the filesystem mounted at an optional target path argument (default `./.tmp`) via `findmnt --target`, strips any bracketed subvolume suffix from the reported source, and then maps partitions/LVM-style block devices back to their parent physical disk with `lsblk -no PKNAME`. This keeps the output focused on the disk that backs the benchmark data directory instead of listing every block device on the host. The target path is also where the `fio` sequential read/write probes run, and it is echoed as `Measured path` in the output. `benchmark_showcase.py` passes its `--tmpdir` here: the reported hardware must be the disk the benchmarks actually wrote to, not the one holding the repository.

The main CI workflow (`.github/workflows/ci.yml`) includes a dedicated `coverage` job. It runs `scripts/run_coverage.sh` in a Fedora + Clang environment, executes the C++ test binaries (`bytecask_tests`, and `bytecask_tests` again built with `BYTECASK_KEYDIR=btree`, so the keyed tree's engine paths — its recovery among them — count alongside the default blind tree's) with LLVM profile instrumentation, merges profiles with `llvm-profdata`, generates HTML coverage (`coverage/html`), exports `lcov.info`, uploads both artifacts to GitHub Actions, and publishes `lcov.info` to Codecov for repository coverage tracking and badge rendering. The same builds record MC/DC (`-fcoverage-mcdc`): `scripts/mcdc_report.py` merges one export per build and fails the job when a condition in `data_file.cppm`, `hint_file.cppm` or `bytecask.cppm` is neither shown independent by a test nor marked `mcdc-exempt:` at its site, or when the exemptions outnumber the cap in `run_coverage.sh`. See *MC/DC coverage* in `docs/correctness_validation.md`.

A change that touches only documentation (`**.md`, `docs/**`) does not run `ci.yml` or `build-wheels.yml`, as a pull request or as a push to `main`: both ignore those paths, since no job reads Markdown. A change that also touches anything else runs both in full. A docs-only commit on `main` therefore has no coverage upload; Codecov compares the next pull request against the nearest commit that has one, whose code is the same. The nightly workflows run on a pull request only when their own paths change, so docs never trigger them either.

Codecov policy is configured in `.codecov.yml`: project coverage status uses the `cpp` flag with an 85% target (1% threshold), patch coverage status uses the same flag with an 80% target (1% threshold), and CI pass is required before Codecov reports success.

The `sanitizers` CI job (`.github/workflows/sanitizers.yml`, called from `ci.yml`) matrixes `bytecask_tests` over AddressSanitizer, ThreadSanitizer, MemorySanitizer, and UndefinedBehaviorSanitizer (`--sanitizer=address|thread|memory|undefined`) and over the key directory (`BYTECASK_KEYDIR`): a pull request, and the push to `main` that merges it, run ASan, TSan and UBSan on the default blind tree; a nightly schedule runs all four on all three trees. The UBSan leg on the blind tree also runs `btree_tests`, and is built with `-fno-sanitize-recover=undefined` so any report fails the run. MSan additionally requires an MSan-instrumented `libc++`/`libc++abi` — the system `libcxx-devel` package used by the other two is not instrumented and produces false positives if linked into an MSan binary — built by `scripts/build_msan_libcxx.sh` from the matching `llvm-project` release branch and cached across CI runs (`actions/cache`, keyed on the script's contents). Because the prebuilt `catch2` package is compiled against the system's default libstdc++ and can't link against a `-stdlib=libc++` binary, the MSan configuration of `bytecask_tests` compiles Catch2 from source instead, from the vendored amalgamated distribution in `third_party/catch2_amalgamated/`; every other configuration keeps using the normal `catch2` package. The MSan build is compiled at `-O1` with frame pointers, over the debug mode's `-O0`, keeps `-fsanitize-memory-track-origins=2`, and has a two-hour job timeout. The `zstd` package is likewise built with `-fsanitize=memory` in that configuration: uninstrumented, its writes into the compressed output go unseen by MSan and every hint frame reads as uninitialized. Its flags carry `--target=$CLANG_TARGET_TRIPLE` like the project's own targets: xmake's package toolchain adds a generic `--target=x86_64-linux-gnu`, under which Clang cannot find Fedora's MSan runtime (installed only under `x86_64-redhat-linux-gnu`). See `docs/correctness_validation.md` for the full rationale and known limitations.

The `build-and-test` CI job also exports Catch2 JUnit XML reports for all three C++ test binaries. It publishes them to GitHub Checks via `EnricoMi/publish-unit-test-result-action@v2` for PR-native test summaries, and uploads them to Codecov via `codecov/codecov-action@v5` with `report_type: test_results` (OIDC), enabling the Codecov Test Analytics dashboard (`/tests/new`). Before upload, `scripts/enrich_junit_sources.py` joins each JUnit file with Catch2 `--list-tests --reporter xml` metadata and injects `file`/`line` attributes per testcase so dashboards have source context. Both coverage and test-results uploads set `slug: gustavoamigo/bytecaskdb` explicitly to avoid repository auto-detection issues, include `if: ${{ always() && !cancelled() }}` guards, and pass `${{ secrets.CODECOV_TOKEN }}` as a fallback for environments where OIDC repository mapping is not yet active. The raw XML files are retained as a GitHub Actions artifact (`junit-test-results`) for debugging failed test uploads.

## Goals

- Provide a clean, minimal API surface for key-value operations.
- Support atomic multi-operation batches.
- Support ordered range iteration (enabled by the ordered key directory).
- Be idiomatic C++23: no raw pointers, no stringly-typed errors, move-only ownership.

## Non-Goals (for now)

- Multi-writer access, MVCC, or full transaction isolation. ByteCaskDB uses a SWMR model. Snapshot isolation via `snapshot()` and `apply_batch(WritePlan)` is available; there is no separate transaction type.
- TTL or expiry.
- Async I/O.
- Background (auto) vacuum. Vacuum is called explicitly by the user.

## Integration: MariaDB Storage Engine

ByteCaskDB is being integrated as a MariaDB pluggable storage engine (`ha_bytecask`). The plugin builds out-of-tree against MariaDB development headers and loads via `INSTALL PLUGIN`. The integration uses a MariaDB-internal transaction object built directly on `snapshot()` + `apply_batch(WritePlan)` (`transaction_design.md`); the engine has no public transaction type. Full design: `docs/mariadb_engine_design.md`.

The plugin consumes the engine through the PIMPL C++ header `include/bytecask.hpp` (typed `bytecask::DB`, `Snapshot`, `WritePlan`, RAII iterators, throwing errors). The implementation lives in `bytecaskdb/bytecask_hpp.cpp` and is compiled into `libbytecask.a`, which the plugin links statically.

The MariaDB handler keeps per-open-table hot-path state local to the handler:
the schema version, secondary-index metadata, row-count counter, and
AUTO_INCREMENT counter. `write_row()` and ALTER-copy `write_row()` reuse that
cached state instead of resolving catalog maps per row. Row-count deltas are
still tracked in the per-THD transaction object so rollback and failed commit
can restore the counters exactly.

### C API / Shared Library Boundary

ByteCaskDB uses C++23 modules internally, which are not portable across compilation unit boundaries when linking external code. To cross this boundary (e.g. the MariaDB plugin), a stable `extern "C"` API is provided:

- **`include/bytecask_c.h`**: flat C header with opaque `bytecask_db_t*` / `bytecask_iter_t*` / `bytecask_snapshot_t*` / `bytecask_write_plan_t*` handles. No C++ types, no module imports. Covers: open/close, put/del/get, forward iteration, snapshots, conditional atomic writes (`apply_batch` via `WritePlan`), and vacuum.
- **`bytecaskdb/bytecask_c.cpp`**: implementation that imports `bytecask` (the C++23 module) and forwards calls through the C API. Compiled into `libbytecask.a`.
- **`xmake.lua` `bytecask` target**: static library combining all engine module objects plus `bytecask_c.cpp`.

Any out-of-tree consumer (not just the MariaDB plugin) should use this C API boundary rather than importing the C++23 modules directly.

### C++ Public Header (`include/bytecask.hpp`)

For C++ consumers that want the full typed API without importing C++23 modules, a PIMPL header is provided:

- **`include/bytecask.hpp`**: standard `#pragma once` header. Defines all public types (`WriteOptions`, `ReadOptions`, `Mode`, `Options`, `Snapshot`, `WritePlan`, `DB`, all iterator types, `DbDegraded`, `DbFollowerMode`) in namespace `bytecask::internal`, with `using` aliases in `namespace bytecask` at the bottom. Depends only on the C++ standard library — no module imports.
- **`bytecaskdb/bytecask_hpp.cpp`**: the only translation unit that `import bytecask;`. Contains all `Impl` struct definitions and out-of-line method bodies. `to_module()`/`from_module()` helpers in an anonymous namespace convert between `bytecask::internal::` (header-defined, plain mangling) and `bytecask::` (module-imported, module-attached mangling) types. `translate_exceptions()` re-throws `bytecask::DbDegraded` and `bytecask::DbFollowerMode` (module-attached) as the header-defined equivalents so callers that include only the header can catch them correctly.

**C++23 module type attachment**: types defined in a module's purview get a `@modulename` suffix in their mangled symbol names. This makes `bytecask::WriteOptions` (from `import bytecask;`) and a nominally identical `WriteOptions` in the header different types at link time. The header resolves this by putting all plain types in `bytecask::internal` namespace (unattached mangling), and using `BYTECASK_HPP_IMPL_MODE` to suppress the `namespace bytecask` aliases in the one TU that imports the module.

### Python Bindings

`bytecaskdb-python/` provides Python bindings via [nanobind](https://github.com/wjakob/nanobind). The extension includes `include/bytecask.hpp` and links against `libbytecask.a` — it does not import the C++23 module directly. The extension exposes DB, Snapshot, WritePlan, all iterator types, and Options. The GIL is released on all I/O paths so multiple Python threads can perform concurrent reads.

**Free-threaded Python (PEP 703)**: the bindings support free-threaded Python 3.13+ (`Py_GIL_DISABLED=1`). The build system auto-detects free-threading via `sysconfig.get_config_var('Py_GIL_DISABLED')` and defines `NB_FREE_THREADED`, which declares `Py_mod_gil = Py_MOD_GIL_NOT_USED` and activates nanobind's locking primitives.

`DataEntry` is constructible from Python (`DataEntry(sequence, entry_type, key, value)`) with bytes-like key/value inputs. This enables network replication transports to deserialize wire payloads back into `DataEntry` objects before calling `ingest()`.

The locking strategy respects the engine's existing thread model:

- **DB reads and snapshot reads are unlocked** — the C++ engine provides lock-free reads via immutable snapshots; adding Python-level locks would destroy read scaling.
- **`PySnapshot::take()`** is protected by `nb::ft_mutex` — the move-out operation is not atomic and concurrent double-move would be UB.
- **Iterator `__next__`** uses `nb::lock_self()` — mutable cursor state must be serialized per-instance.
- **WritePlan mutation methods** use `nb::lock_self()` — the check-then-mutate pattern is not atomic.

Under GIL Python, all nanobind locking primitives (`nb::ft_mutex`, `nb::lock_self()`) are no-ops — zero overhead.

### Python Reference Implementation

`bytecaskdb-python/reference/bytecask_ref.py` is the engine's model in one file of plain Python, written to be read. It imports two general-purpose modules: `persistent_tree.py`, an immutable sorted map, and `checksum.py`, CRC-32C.

Its interface is the engine's, as the native binding (`bytecaskdb._bytecaskdb`) exposes it and `CONTRACT.md` specifies it:
- `apply_batch(WritePlan)`, with every guard;
- `put`/`del_`/`del_range`, `get`/`contains_key`, and the four iterators;
- `Snapshot`, which a `WritePlan` consumes;
- `CommitResult`, and option objects.

The Pythonic interface (`db[k]`, `with db.transaction()`) stays in `bytecaskdb/ext.py`. A test can run `ext.py` on the reference through `bytecaskdb.DB.open(path, backend=bytecask_ref)`. `ext.py` builds options and plans through the backend module it was opened with, and the default is the native extension. This is a test seam: `reference/` is not part of the `bytecaskdb` package and must be on `sys.path`.

The docstring shows, with `dump(path)`, what a put followed by a two-write batch leaves on disk. `dump` prints the committed entries of any database, the engine's included. The reference contains only what decides what a read returns:

- **Write path.** One lock. `apply_batch` runs five steps: check, frame, append, sync, publish.
  - **Check:** the plan's guards are checked against the head, the published key directory.
  - **Frame:** BULK_BEGIN/BULK_END wrap more than one write, and every entry takes a sequence, markers included.
  - **Append:** in one write, which returns each entry with its offset.
  - **Sync:** if asked.
  - **Publish:** the next key directory is built from those placed entries and published.
- **Key directory.** A `PersistentTree[bytes, Location]`. `PersistentTree` is an unbalanced binary search tree built by path copying and knows nothing of the engine. It is a `collections.abc.Mapping` whose `set`, `remove` and `discard` return a new tree, with ordered `ascending`/`descending` scans. A snapshot is a tree. A `Location` is a record's file, offset and sequence. A value is read from its record, and the record's CRC, key and sequence are checked.
- **Recovery.** Every data file is replayed in order of its first sequence, through the same `apply_entry` the write path uses. A batch counts once its BULK_END is read. A file whose committed entries stop before its end falls into one of three cases:
  - zeros follow: they are cut;
  - data follows, in the newest file or in a file whose first sequence reads 0: it is cut, as in the engine's `recovery_check_tail`;
  - data follows in more than one file: open refuses, since a crash tears only the file being written;
  - data follows, anywhere else: open refuses.
  - Before replaying, the newest file's committed bytes are written back and synced, and so is any cut. This is the engine's `rewrite_durably`/`truncate_durably`. Without it, a failed sync could leave those bytes only in the page cache, and a cut that was never synced could bring a torn tail back into a file that is no longer the newest.
- **I/O errors.** A write or sync that fails with an I/O error stops all writes: they raise `DbDegraded`, while reads go on. `close()` raises `DbDegraded` too. Reopening is the only way back, and it rewrites the file as above. The engine also offers `resume()`; the reference does not.
- **Guards.** These match `validate_preconditions`. With a snapshot, the plan collects the keys and ranges that must be unchanged: explicit guards plus the keys it writes, and range guards plus its range deletes. It then checks each one the same way. A key is unchanged if its sequence, or its absence, is the same in the snapshot and the head. A key created and then deleted after the snapshot is absent from both, so it does not conflict.

It writes the V01 format, so the engine opens a database it wrote (generating the hints) and it opens one the engine wrote (ignoring the hints). Hints, vacuum, group commit, preallocation, the buffer pool, replication and `resume()` are left out. A database the engine left mid-vacuum, with a compacted copy beside the original, is for the engine to open first.

It does not give the engine's other guarantees:
- **Exclusive access.** There is no directory lock.
- **Snapshots that outlive `close()`.**
- **Flat latency.** There is no group commit, the tree is unbalanced, range deletes and range guards are linear in the keys they cover, and opening reads every file whole.
- **Fault testing.** The crash, chaos and fault-injection rigs have never run against it; its crash and I/O-error handling have unit tests only.

`bytecaskdb-python/tests/test_persistent_tree.py` checks the tree and the CRC on their own. `bytecaskdb-python/tests/test_reference.py` runs the reference and the native binding through the same calls at the engine's interface. A seeded workload runs on both: puts, deletes, range deletes, and guarded plans whose snapshots were taken several writes earlier. Snapshots are held across writes, rotation, reopens and native vacuum. Every commit or conflict must agree, and so must every sequence and every `get` and scan.

A second test runs one `ext.py` script on both backends and compares the results. Because `ext.py` is shared, the differential test cannot see a bug in it; `ext.py`'s own tests (`test_safety.py`) cover it. File-format tests cover both directions. It all runs in `ci.yml`.

## Design Principles

The design follows these core tenets in order of priority:

1. **Correctness**: Data integrity is paramount. All design decisions prioritize correctness over performance.
2. **Simplicity**: The architecture is kept simple to facilitate understanding and maintainability.
3. **Predictable latency over peak throughput**: Write-path operations must have bounded, predictable latency. Work that can be deferred without compromising correctness must be deferred. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes. This directly influences decisions like deferring hint file writes out of the rotation path.
4. **Performance**: Optimizations require a real use case. Without one, correctness and simplicity take priority.

## System Architecture

### Key Directory

ByteCaskDB uses `PersistentBlindBTree<kBlindLeafBytes>` as the in-memory key directory: a B+ tree whose leaves hold no key bytes (below). All keys reside in memory at all times.

The engine names the tree only through the aliases in `bytecaskdb/internals.cppm` (`KeyDirTree`, `KeyDirTransient`, `KeyDirIter`, …) and reaches it only through the `kd_*` functions there: `kd_get`, `kd_contains`, `kd_put` and `kd_erase` (each returning what it displaced, so a put is one descent), `kd_lower_bound`, `kd_upper_bound`, `kd_begin`, `kd_end`, and the value iterators. Each takes a `KeyDirCtx`: the file registry of the version the tree belongs to, and on the write path the records the batch being built has placed but not yet written. Trees that store their keys ignore it. `BYTECASK_KEYDIR=btree` builds the engine on the B+ tree that keeps key bytes in its leaves; CI runs the full engine suite on both. Any other value is refused at configure time. The switch stays so a new key directory can be tested against the same engine suite.

The keyed build recovers into a `RecoveryKeyDirTree`, a keyed B+ tree, and `key_dir_from_recovered()` hands it to the engine unchanged; the blind build recovers straight from the hint files, as described below.

The key directory is a persistent (immutable) B+ tree. Values live only in leaves, inner nodes hold suffix-truncated separators, and there are no sibling links — a sibling pointer would force a leaf update to touch its neighbour, which is exactly what structural sharing cannot afford. One slotted node layout serves leaves and inner nodes alike: a 32-byte header, the prefix every key in the node shares stored once, a `u64` slot array growing up from the front and an entry heap growing down from the end. Each slot packs the first four suffix bytes into its high half, so a lower bound within a node is a branch-free count over the slot array that never touches the heap for most keys; ties fall back to a full compare. Nodes are 4 KiB, which is three levels at 1M keys and four at 100M for 36-byte keys — a node exceeds that only to hold a single oversized key, and then holds exactly that one entry. Deletion is lazy: no rebalancing, an emptied node is unlinked and freed, and the root collapses when it has one child. The tree provides get/set/erase/upsert, `lower_bound()` / `upper_bound()`, forward and reverse ordered iteration over `(key, value)` or values alone, and a `transient()` / `persistent()` API for batch mutations. Iterators hold a stack of `(node, index)` sized to the tree height plus a key buffer rebuilt on each advance, so a dereferenced key is a span valid until the next advance. Implemented in `bytecask.btree` (`bytecaskdb/btree.cppm`); `docs/persistent_btree_design.md` is the full design, including the node layout, the reclamation windows and the measurements against the radix tree.

The trees' transients are intentionally single-use. `persistent() &&` retires the builder, and any later read or write on a consumed or moved-from transient throws `std::logic_error` in release builds instead of relying on debug-only assertions.

Tree nodes carry no reference count. Lifetime is decided per version instead of per pointer: the session that builds a new version retires the base nodes it makes unreachable, and the version chain frees a retired node once no live version can still reach it. A `Snapshot` therefore holds exactly the nodes it can reach, as it did under reference counting. Versions form a chain — a state may be derived only from the end of it — which the commit pipeline satisfies by construction and `resume()` restores by dropping the unpublished heads of a failed flush before it derives the resumed state.

That reclaimer is `VersionChain<Traits>` in `bytecask.version_chain` (`bytecaskdb/version_chain.cppm`). `Traits` is all it needs of a node: its version tag, its children, how to destroy it, and the accounting hook the memory tests read — `btree_detail::ChainTraits` for the B+ tree, whose inner nodes the blind tree shares. It began as the retired radix tree's own class; see `docs/radix_tree_epoch_reclamation_design.md` for its design and `docs/persistent_btree_design.md` §Versions for where a B+ node becomes garbage.

`upsert(key, val, should_replace)`, on the transient, performs a single-traversal conditional insert-or-replace: it walks from root to leaf once, inserting if the key is absent, or replacing if `should_replace(existing, incoming)` returns true. Returns the displaced old value when a replacement occurs, enabling callers to track side-effects (e.g. file_stats adjustment). `kd_put` uses it, so a put that displaces an entry is one descent. It was introduced when profiling (perf, Recovery/Parallel/16) showed separate `get()` + `set()` traversals consuming ~49% of recovery time.

Keys are stored as byte sequences inside the tree's nodes. The tree APIs accept `std::span<const std::byte>` for all key parameters — no intermediate `Key` wrapper is needed for internal operations. The public `Key` class (backed by `std::vector<std::byte>`) is retained for the external iterator API (`KeyIterator`, `EntryIterator`) and for the recovery tombstone tracking map. `Key` provides `operator<=>` (lexicographic over raw byte values) and `begin()`/`end()`/`size()` accessors. Keys have a hard upper bound of 65 535 bytes (the `u16 key_size` field in the data file header).

#### History

The original key directory was `PersistentOrderedMap<Key, KeyDirEntry>`, backed by `immer::flex_vector<Entry>`. A persistent radix tree replaced it (BC-030) and was in turn replaced as the default by the B+ tree, which measured faster on point reads, range scans and batched writes, and rebuilt the key directory from sorted hint files faster as well; the B+ tree with blind leaves then replaced that as the default (below). The radix tree has since been removed; `docs/persistent_radix_tree_design.md` records its design and measurements, and [`70612bc`](https://github.com/gustavoamigo/bytecaskdb/tree/70612bcef60dd41136f7c01f8c593034b3833cc6) is the last commit that has its code. The keyed B+ tree stays selectable with `BYTECASK_KEYDIR=btree` and passes the same engine suite in CI. `docs/persistent_btree_design.md` records the head-to-head measurements, including where the B+ tree was behind: unsynced single puts.

#### The blind-leaf key directory (the default)

A B+ tree whose leaves store no key bytes: each entry is a crit bit, a 24-bit fingerprint and the record's location, 12 bytes in all. A point lookup compares the leaf's fingerprints with the query's in vector registers and reads the record behind each match to confirm the key; an insert walks the leaf's crit bits to one candidate and reads that record to place the key. The key directory's size no longer depends on key length (13.7–19 B/key at 1M keys, against 33–133 for the B+ tree). Inner nodes, path copying and reclamation are the B+ tree's. Implemented in `bytecask.blind_btree` (`bytecaskdb/blind_btree.cppm`); `docs/blind_leaf_btree_design.md` is the design and has the measurements.

In the engine it changes what reads the data files:

- The tree reads keys through a `KeyReader` over the `KeyDirCtx`, with `DataFile::lend_record`: the whole entry, sizes from its own header, one frame lookup on the buffer pool, CRC-checked unless `ReadOptions::verify_checksums` is off. A record the current batch has placed but not written is answered from `TransientEngineState::pending_`, filled as each put is applied and discarded with the transient.
- The read that confirms a key also yields its sequence, so `kd_get` returns a full `KeyDirEntry` and the conflict checks, vacuum's remap and `resume()`'s replay are unchanged. `DB::get` and `Snapshot::get` take the value from that same read.
- Reads the other trees never do: every put and erase reads one record (the candidate's, which may be another key's), plus one when a leaf splits; `contains_key` and every step of `keys_from` read one record. A read that fails — I/O error or CRC mismatch, including on a neighbouring key's record — fails the operation before anything is written.
- Iterators over a published state hold their own handle on its file registry, so `keys_from` can outlive the call that made it.
- `Snapshot::count_keys(from, to, limit)` counts a range without walking its keys: `lower_bound` places each end within its leaf (one read each), and `btree_detail::count_entries` adds leaf sizes between the two iterator stacks, stopping at `limit`. The keyed B+ tree counts the same way with no reads. The MariaDB plugin's `records_in_range` uses it: walking the range instead read one record per key and, in HammerDB TPROC-C, made up most of the engine's record reads (`docs/range_count_design.md`).
- Recovery (`recovery_load_streams`) merges the sorted hint files directly: per file, a fence at the start of every frame and every 4 KiB inside one; splitters from the pooled fences; per range, a k-way merge of every file's slice straight into blind leaves, which are then concatenated. No intermediate tree is built, and at 1M keys it recovers faster than the B+ tree's `recovery_load_ranged`. Leaves are loaded between 60% and 100% full, spread so they do not all split on the first random writes after open.

### Size Limits

The on-disk entry header limits keys to 65,535 bytes (u16 `key_size` field). Its `value_size` field is 32 bits wide, but values are limited further by the in-memory packing below. Neither can be raised without a format or packing change.

The in-memory `KeyDirEntry` is bit-packed into two 64-bit words (16 bytes) to keep the key directory small. Field limits enforced by the packing:

| Field | Bits | Max value |
|---|---|---|
| sequence | 48 | 2^48 − 1, 281 trillion (~8.9 years at 1M ops/sec) |
| file_id | 20 | 1,048,575 (split across word0 and word1) |
| file_offset | 32 | 2^32 − 1, where an entry may start |
| value_size | 28 | 268,435,455 (2^28 − 1) per value |

`KeyDirEntry::make` checks each field and throws past it. The engine does not rely on that check: every limit is enforced before the write it would break reaches a data file, so reaching one never degrades the engine. `CONTRACT.md`, *Hard limits*, states the behaviour; the `[limits]` tests prove it, exactly at each limit and one past.

- **Sequence.** `ingest` rejects an entry above 2^48 − 1, of any type — a tombstone or marker would otherwise reach the file unchecked, since only Puts are packed. `execute_slot` refuses a plan whose last sequence would pass the limit, before `apply_writes`, so it fails alone and the rest of its group commits.
- **File offset.** `Options::max_file_bytes` is capped at `kMaxFileBytes` (3 GiB) and one write — a plan, or an atomic batch in an `ingest` slice — at `kMaxBatchBytes` (1 GiB). A file is rotated once it reaches `max_file_bytes` and a write never spans two files, so the first write of a group starts below 3 GiB and ends below 4 GiB. Writers that queue together could still carry the file further, so `execute_group` ends a group before a slot that would take the file past 2^32 bytes; `execute_slots` runs the rest as the next group, after the rotation. A group splits only that close to the offset limit — at the default 64 MiB it would take gigabytes queued at once — so a group is still one append with one outcome. Test builds can lower that 2^32 (`test_max_group_file_bytes`), which is how a test splits a group without writing 4 GiB. `ingest` needs no split: it already rotates between batches. A `static_assert` ties the two constants to the packed field.
- **File id.** Ids are given out in name order at each `open` and taken by every rotation, vacuum compaction and `resume()`, never reused within a process. `execute_group` checks for a free id before phase 1; without one, a slot that would take the active file to the threshold is refused (`execute_slot`, with `can_rotate` false) and the file stays under it, so no rotation is attempted after sealing. `ingest` counts the rotations a slice needs with the same chunking rule it writes with (`ingest_chunk`) and refuses the slice if there are fewer ids. `vacuum_compact_file` and `resume()` check before touching a file. The error tells the operator to reopen, which renumbers; `open` refuses a directory with more data files than there are ids.
- **Bytes per `pwritev`.** `append_entries` splits a write into calls of at most `kMaxEntriesPerWritev` entries (`IOV_MAX / 4`) and `kMaxBytesPerWritev` bytes (1 GiB), with at least one entry per call. Linux transfers at most 0x7ffff000 bytes per call and returns short above it, which `check_write` reports as a failed write: without the byte cap, a legal plan of nine 256 MiB values degraded the engine. Test builds lower the entry count to 2 so ordinary tests cross chunk boundaries; `test_writev_limits` lets a test set the production values or a small byte cap.

Configurable limits are enforced at the API boundary — before any data is copied into a `WritePlan` or written to disk:

| Limit | Default | Hard ceiling | Rationale |
|-------|---------|-------------|-----------|
| `Options::max_key_bytes` | 4,096 (4 KiB) | 65,535 | Keys live in memory (key directory). Large keys bloat RAM and slow traversal. |
| `Options::max_value_bytes` | 4,194,304 (4 MiB) | 268,435,455 | Values go to disk. Oversized values cause pathological file rotation. |
| `Options::max_file_bytes` | 64 MiB | 3 GiB (`kMaxFileBytes`) | Leaves room for one `kMaxBatchBytes` write under the 32-bit offset. 0 is valid and seals a file after every write. |

An option above its ceiling, or `recovery_threads = 0`, makes `open` throw `std::invalid_argument` before the directory is created or locked; it is not lowered to the ceiling, so the engine never runs with a limit other than the one configured. Violations of the size limits throw `std::invalid_argument`. `WritePlan` carries the limits from `Snapshot` (which inherits them from `DB`) or uses the defaults when constructed without a snapshot. `DB::put`, `DB::del`, `DB::del_range`, and `DB::ingest` all validate before proceeding.

### Concurrency Model

ByteCaskDB follows a **single-writer / multiple-reader (SWMR)** model:

- Exactly one writer may operate at a time.
- Multiple readers may operate concurrently.
- MVCC and snapshot isolation are not supported.

#### Directory lock

A single process may hold a database directory open at a time. `DB::open()` acquires an exclusive advisory lock (`flock(LOCK_EX | LOCK_NB)`) on `dir/.lock` before recovery begins. A second process attempting to open the same directory receives a `std::system_error`. The lock is released when the `DB` is destroyed. The `.lock` file remains on disk as a harmless sentinel.

#### Concurrency strategy

ByteCaskDB's read path is designed so that **readers never acquire the write mutex**. The strategy combines two ideas:

1. **A single writer mutex** (`write_mu_`) that serialises mutations — readers are completely unaffected by it.
2. **An immutable, copy-on-write snapshot** (`EngineState`) published via `std::atomic<std::shared_ptr<EngineState>>` — readers capture the current snapshot without blocking the writer.

##### State publication via std::atomic<shared_ptr>

The engine state is published through `std::atomic<std::shared_ptr<EngineState>>`. Writers call `state_.store()` to publish a new immutable snapshot; readers call `state_.load()` to obtain a reference-counted copy. The atomic `shared_ptr` guarantees that `load()` always returns a valid, self-consistent snapshot. Old snapshots stay alive as long as any reader holds a reference.

`Bytecask` uses `std::atomic<std::shared_ptr<EngineState>>` for its published state. The `write_mu_` mutex serialises writers; `state_.load()` is the readers' only access point.

##### Shared state layout

```
  Bytecask object
  ┌─────────────────────────────────────────────────────────────┐
  │  write_mu_      std::mutex (heap-allocated)                 │  ← writers only
  │                                                             │
  │  state_         atomic<shared_ptr<EngineState>>             │  ← writer stores,
  │                                                             │    readers load (no write_mu_)
  └─────────────────────────────────────────────────────────────┘

  EngineState  (heap, reference-counted, never mutated in place)
  ┌──────────────────────────────────────────────────┐
  │  key_dir         PersistentBTree<KeyDirEntry>     │  key → (file_id, offset, seq)
  │  files           shared_ptr<FileMap>              │  file_id → open DataFile fd
  │  file_stats      map<uint32_t, FileStats>         │  per-file live/total bytes
  │  active_file_id  uint32_t                         │
  │  next_file_id    uint32_t                         │  writer-only; monotonic file counter
  │  next_seq         uint64_t                         │  writer-only; monotonic sequence counter
  │  durable_seq      uint64_t                         │  highest sequence confirmed by fdatasync
  └──────────────────────────────────────────────────┘
```

`EngineState` bundles all engine state into a single immutable value. Writers never mutate an `EngineState` in place; they create a `TransientEngineState` working copy, apply mutations, and publish the result via `persistent()`. The old state stays alive as long as any reader holds a `shared_ptr` reference.

`file_stats` lives inside `EngineState` so that the transient/persistent discipline covers all mutable state uniformly. The map is shallow-copied into `TransientEngineState` on each write — acceptable because the number of open files is small (typically < 100).

##### Write path — group commit

All writes (`put`, `del`, `apply_batch`) route through a single coordinator: `DB::apply_batch`. The public methods `put` and `del` are thin wrappers that build a `WritePlan` and delegate. Each write is packaged into an `EngineSlot` and submitted to either `SoloWriter` (single-slot, no batching) or `WriteGroup` (leader-applies-all batching).

`EngineSlot::result` is `std::optional<CommitResult>` (BC-231): `execute_slot` sets it to `nullopt` on a validation/conflict failure, or to `CommitResult{.sequence = entries.back().sequence}` on success (`{.sequence = 0}` for an empty or guard-only plan). `durable` is filled in at the end of Phase 3 once the batch's sync/rotation outcome is known: `slot->result->durable = (t.durable_seq() >= slot->result->sequence)` for every committed slot — this naturally covers group-coalesced and rotation syncs. A batch made of only empty/guard-only plans produces no entries at all and returns from `execute_slots` right after Phase 1, before Phase 3 ever runs; that early-return path explicitly sets `durable = true` on every committed slot whose result has nothing to wait for. The exception is a zero-entry slot with `sync = true` while earlier `sync = false` writes are not yet durable: it keeps `sequence = 0`, records the head's last sequence in `EngineSlot::sync_through`, raises the head's `sync_requested_seq` to it, and waits in `commit_wait` for the flush that covers it. `flush_pending` therefore runs whenever the head asks for a sync, not only when it holds new entries. This makes `apply_batch({.sync = true}, WritePlan{})` a flush of every earlier unsynced write, coalesced with group commit. See `docs/commit_result_api_design.md` for the full `CommitResult` contract, including the C/Python/Node bindings.

**Routing**: a write goes to `SoloWriter` when `WriteOptions::solo` is set (benchmarking) or when the plan's byte size exceeds `kGroupWriteMaxBytes` (256 KiB). All other writes go to `WriteGroup`.

**WriteGroup**: every write passes through the executor one batch at a time — the serial section whose busy time `bytecask.group_writer_busy_us` measures. A writer that arrives while no batch is running runs one itself, inline: it takes every queued slot (its own included) as one batch, executes them all under one lock hold, and returns to its caller. N concurrent sync writes share a single `fdatasync` instead of paying N separate calls — fdatasync is ~1–2 ms; pwritev is ~microseconds. A lone writer never sleeps and never wakes another thread.

Slots queued while a batch runs are taken by the **committer thread**, which runs batch after batch until the queue is empty, then polls for the next hand-over for 50 µs before sleeping. It replaced handing the next batch to the queued writer at the head of the queue: that writer was asleep, so every batch waited for a scheduler round trip before it could start — ~7% of the serial section's time on an idle 48-vCPU host, and ~300 µs per batch, a quarter of the section's time, when the host's cores were busy. The committer is already running when a queue forms, so under load batches run back to back. `commit_probe` (tmpfs, jemalloc), per thread count, before → after: bare unsynced writes 254K → 254K commits/s at 1 thread, 212K → 269K at 2, 209K → 251K at 16; TPROC-C-shaped transactions 10.4K → 10.4K at 8 threads, 18.0K → 21.0K at 48, with the serial section 79% → 95% busy. HammerDB TPROC-C, 96 warehouses, 48 users, data on a RAM disk: +5.7% NOPM.

The committer thread allocates almost every key-directory node, since each batch path-copies the nodes it changes, and readers free them when they drop the last version that pins them. A general-purpose allocator does not give those frees back to the allocating thread, so the key directory's nodes go through `NodePool` (`docs/persistent_btree_design.md`, *Node pool*): freed nodes go onto a shared list and come back to the next allocation, whichever thread freed them. On `commit_probe` it takes 2–3 µs per commit out of the serial section under glibc (+4% to +5% commits/s) and under jemalloc's defaults (`docs/commit_pipeline_design.md`, *Node allocation*).

A finished slot goes back to its owner through its own word, `Slot::state` — a targeted wake per waiter, not a broadcast. An unsynced owner polls it for 10 µs before sleeping on it, which covers a short batch without a scheduler round trip. A synced owner sleeps on it at once: released by a poll, it would reach `commit_wait` before the flush that covers its entries had published, and sleep there instead, paying a wake-up the flush would otherwise have absorbed. Measured on tmpfs at 4 synced writers (`PutMT/Sync`), polling took the share of commits already durable on reaching `commit_wait` from 71% to 34% and the commit from 16.9 to 19.7 µs; unsynced writers keep the poll, which is worth +30–40% at 2 threads. `kDone` is the last write the releasing thread makes to a slot: the owner returns only on it and may free the slot at once, so the notify happens under `kReleasing`, while the owner is still bound to wait.

One batch per inline writer is a fairness rule, kept from the leader hand-off it replaced: the writer's own slot is done after its batch, and the committer, which runs every later one, has no caller of its own. The leader's own slot is done after its batch, so every further batch it ran was time its caller waited. Two earlier policies were measured against it. Draining until the queue was empty made one sysbench client a permanent writer whose commit did not return for a 13-second run. Letting a leader run up to 8 consecutive batches ("hot leader") kept the disk busy under closed-loop writers but had two costs. With two writers it serialised to one put per fdatasync: the leader kept serving the other thread's resubmissions while its own caller waited (engine_bench `PutMT/Sync`, 2 threads: 477 ops/s, the same as one thread; 695 ops/s with a hand-off after every batch). With eight MariaDB clients the batch that released everyone else was followed by an empty queue for the length of each client's round trip, so batch sizes alternated between 1 and 7 with ~220 µs of idle disk between batches; a hand-off after every batch gave uniform batches of ~4 and ~70 µs, and sysbench `oltp_write_only`/`oltp_insert` throughput rose 6–8% with lower p95 in every paired run.

The hot leader wins one regime: many closed-loop writers with no think time on an oversubscribed machine. At 32 threads on 4 vCPUs it reacquired the queue lock behind the whole wave of resubmitting members and collected batches of 31 (255 of 483 batches), where a freshly woken leader takes the queue at a random point in the wave and gets 13–31; the commit cycle itself is no slower (fsync 2.1 ms and inter-fsync gap 146 µs vs 190 µs), but fewer puts share each fsync, so `PutMT/Sync` throughput is 15–25% lower at 16–64 threads. That regime has no client latency floor and is the least representative of a database server, so fairness and predictable latency (principle 3) take precedence.

**SoloWriter**: same `submit(Slot&)` interface, no internal batching. The executor acquires `write_mu_` and processes the single slot. Provides a uniform interface for routing.

##### Commit pipeline: stage 1 under `write_mu_`, stage 2 under the flush role

Design: `docs/commit_pipeline_design.md`. The executor (stage 1) runs under `write_mu_` and stops after the append; the fdatasync and the publication (stage 2) run under a separate *flush role*, outside `write_mu_`, so stage 1 of the next batch overlaps the fdatasync of the previous one. Plans are validated against the head, so a plan can lose to a write that is applied but not yet published; such a conflict is reported once that write is published (`wait_published`), since no retry can succeed before — see *When a conflict is reported* in the pipeline design. On a flush-bound disk this took ~280 µs of serial in-memory work per commit cycle off the disk's critical path.

Two state pointers over the same persistent chain:

| pointer | written by | read by | meaning |
|---|---|---|---|
| `state_` (published) | the flush-role holder | readers, `snapshot()`, `durable_sequence()`, barrier ops | every `sync=true` entry in it is durable |
| `head_` (prepared) | `execute_slots` under `write_mu_` | `execute_slots`, `flush_pending` | latest stage-1 output; may hold entries still in the page cache |

`state_` is always an ancestor of `head_`; they agree whenever no flush is in flight and no batch is pending. `EngineState::sync_requested_seq` — the highest sequence written by a `sync=true` slot — travels with each state, so whether a state owes an fdatasync is a property of the state itself (`sync_requested_seq > published durable_seq`), and `store_state` enforces `durable_seq >= sync_requested_seq` on every publication.

```
  Stage 1 — execute_slots, under write_mu_ (per batch)
  ────────────────────────────────────────────────────
  t = head_->transient()
  Phase 1: per slot, sequential
    1. validate_preconditions(plan)   — against the prepared head, so a
       plan sees every earlier stage-1 write, published or not
    2. prepare_write(plan) → vector<DataEntryView>
    3. pre-compute offsets from running_offset
    4. apply_writes(plan, offsets)
    5. collect entries into all_entries
  Phase 2: file.append_entries(all_entries)   — one pwritev, page cache
  note_sync_requested(batch_max_seq) if any slot is sync=true
  head_ = t.persistent()                      — no fdatasync, no publish
  (rotation needed? → quiesce(), then sync / seal / rotate / publish inline)

  Stage 2 — commit_wait, on the writer's own thread, no write_mu_
  ────────────────────────────────────────────────────────────────
  loop:
    covered?  sync=true:  state_->durable_seq >= my sequence
              sync=false: state_->next_seq   >  my sequence     → return
    flush error recorded / engine degraded                       → throw
    flush role free?  take it, flush_pending(), release, wake all → loop
    else               sleep on durable_cv_ until the flush lands  → loop

  flush_pending (the role holder)
    head = head_;  published = state_
    if head->sync_requested_seq > published->durable_seq: fdatasync(head active file)
    publish copy of head with durable_seq = head->next_seq - 1 (or unchanged)
```

The sequential per-slot processing in Phase 1 preserves the serial correctness model exactly. Each slot's `validate_preconditions` sees the key_dir after all previous slots' writes. A `del` with `ensure_present` in slot 2 correctly sees slot 1's `put`.

Properties of stage 2:

- **No new thread, no leader tenure.** The flush role is taken by whichever waiter finds it free after the previous flush's broadcast; a writer flushes at most the head as it stands when it wins. A lone writer takes the role immediately and flushes on its own thread: its path is the old one with the fdatasync moved from one function to the next, zero wake-ups (`bytecask.commit_wait_blocked` stays 0).
- **Batch formation is set by the disk.** A flush covers everything appended while the previous flush ran. A writer's worst case is two flushes.
- **Settle before capture.** On the `commit_wait` path (`flush_once`, never in `quiesce()`, whose callers hold `write_mu_` and would block the leader they wait for), the role holder waits for slots already inside the `WriteGroup` to finish stage 1 before capturing the head, bounded by `kFlushSettleMax` (200 µs). A throughput heuristic; nothing depends on it for correctness. Writers released by the previous flush re-enter within microseconds but need a stage-1 batch before they are in the head; a flush that starts the instant the role is won leaves them for the flush after. With 8 zero-think-time writers the settle took commits per fdatasync from 4.15 to 7.78 at the same fdatasync rate. The bound only binds under continuous arrivals, where it is ~8% of a flush, and it is what keeps a stalled stage-1 leader from holding the disk. Only a flush that owes an fdatasync settles — the head already asks for a sync the published state has not confirmed, or a synced writer is still in stage 1 (`WriteGroup::sync_busy`): an unsynced flush has nothing to share, and under continuous unsynced writers the settle delayed every publication by the whole bound (+19% `oltp_write_only` at 16 clients without it; `docs/write_path_investigation.md`). `bytecask.flush_settles` counts the flushes that settled.
- **Commit delay before the settle.** A flush that owes an fdatasync first waits for the commits the last fdatasync released (`CommitDelay`, `bytecask.concurrency`): until as many synced commits have finished stage 1 since that fdatasync returned as it covered, or until min(F, 2·R), whichever is first. F is the running average of fdatasync time; R is the running average of a client's round trip, from a synced commit returning to the same thread's next synced commit, per DB, with samples of F or longer dropped. With the disk busy with fdatasync back to back, throughput is commits per fdatasync, and the settle alone only waits for slots already in the write group, not for clients a round trip away. A lone writer never waits (the last fdatasync covered one commit), nor does anything before F and R are measured, an unsynced flush, a barrier (`quiesce()` never calls `flush_once`), or a single-threaded build. The wait spins with `yield`, like the settle. It changes when an fdatasync starts, nothing else: a commit still returns only once an fdatasync covering it completed. Reasoning and measurements: `docs/commit_pipeline_design.md`, *Commit delay*.
- **`sync=false` behind a flush** waits for that flush and is published by the next `flush_pending`, which fdatasyncs only if a `sync=true` slot landed behind it. Visibility latency ≤ one flush in the mixed case; unchanged in a pure NoSync workload.
- **Barrier operations** — `create_manifest`, `resume`, `vacuum_commit`, `set_mode`, `ingest` — construct a `WriteBarrier`: it takes `write_mu_`, calls `quiesce()` (take the role, flush and publish the head on the calling thread), and on destruction resets `head_` to `state_`, releases the role, then unlocks, in that order by member layout. Rotation, already under `write_mu_` inside `execute_slots`, calls `quiesce()` directly — and checks the published state afterwards: if the flush it waited on (its own or another writer's) failed, the batch fails with that flush's error and publishes nothing, since its transient was built on the head the failed flush left behind. Publishing it would clear the degrade without `resume()` while `flush_error_` stayed set, so every later writer got the stale error back and a conflicting `apply_batch` waited forever for entries nothing would publish (found by the chaos soak, #92; `pipeline: rotation behind a failed flush stays degraded`). Every publication happens under the role, so a flush that is landing can never overwrite a barrier's or a failure path's state. `resume()` is the one barrier that runs on a degraded state, where `quiesce()` publishes nothing and `head_` still holds the failed flush's heads; it resets `head_` to `state_` itself before deriving, since the key directory derives a version only from the end of its chain.
- **`head_` is written only under `write_mu_`**, by `execute_slots` and by `~FlushRole`. A flush failure publishes the degraded state without touching the head; stage 1 refuses to start on a degraded published state, and a batch that raced past that check ends in `commit_wait` with the flush error, its bytes handled by `resume()` like the failed flush's. The head's `durable_seq` is not meaningful — the head chain never learns about flushes — so only `execute_slots` derives a state from `head_`, and `flush_pending` assigns `durable_seq` at publish: `next_seq - 1` after an fdatasync, the published value otherwise.

##### Range deletion (`del_range`)

`del_range(opts, from, to)` deletes all keys in `[from, to)` with a single data file append. The on-disk entry reuses the standard layout: `entry_type = RangeDel (0x05)`, `key = start_key`, `value = end_key`. No new header fields.

A range must have `from < to`. `check_range` refuses `from >= to` with `std::invalid_argument` in `WritePlan::del_range` (which `DB::del_range` goes through), `ensure_range_unchanged` and `Snapshot::count_keys`, before anything is written or read. It used to be a silent no-op — `{sequence = 0}` from `DB::del_range`, an entry that deleted nothing from a plan — and an empty or swapped range is almost always a caller's swapped bounds, which the silence hid. The MariaDB plugin's `count_range` answers `lo >= hi`, which contradictory predicates produce, with 0 before calling `count_keys`. The rule is on the API only: `ingest` and recovery still apply a `RangeDel` with `from >= to` written before it, as a range that deletes nothing.

On the write path, `prepare_write` emits one `DataEntryView` per range delete. `apply_writes` iterates the key directory from `lower_bound(from)` to the first key `>= to`, decrements `live_bytes` on each affected file, and erases the keys. The RangeDel entry itself contributes only to `total_bytes` (same as point Delete — tombstones are not live).

Range deletes are supported on `DB::del_range` and `WritePlan::del_range`. Inside a batch, they are framed by `BulkBegin`/`BulkEnd` like other operations. Existing guards (`ensure_unchanged`, `ensure_range_unchanged`, implicit W-W check) detect concurrent range deletes without changes — erased keys produce sequence mismatches.

A range guard and the implicit W-W check of a planned `del_range` are one check, `TransientEngineState::range_changed(snap, from, to)`: `[from, to)` has changed when a key in the head carries a sequence the snapshot does not hold for it (changed or inserted), or a key in the snapshot is absent from the head (deleted, by `del` or a range tombstone). Two passes are needed because a deletion leaves no entry to carry a sequence. A key vacuum relocated keeps its sequence and is not a change; a key inserted and deleted again since the snapshot is not one either. The check runs against the pipeline head, so it sees writes of earlier slots in the same group that are not yet published. `tests/range_conflict_test.cpp` checks it against a `std::map` model of the range, with the intervening writes published, in the same group commit, followed by vacuum, or replayed by `resume()`.

##### TransientEngineState

`TransientEngineState` is the mutable working copy for all write-path state transitions. It follows the same `transient()` / `persistent()` pattern as the key directory tree.

The coordinator (step 5 above) only performs IO. It never touches `key_dir`, `file_stats`, or sequence directly. The transient owns all state logic:

- `validate_preconditions(plan)` — reads snapshot + current state, returns bool
- `prepare_write(plan)` — assigns sequences, returns `vector<DataEntryView>` for the IO loop
- `apply_writes(plan, io_result)` — updates key_dir, file_stats, advances sequence
- `apply_rotate_file(new_file)` — registers new file, updates active_file_id
- `apply_vacuum(old_file_id, scan, new_file)` — remaps keys, updates registry + stats
- `apply_sync(batch_max_seq)` — records that fdatasync confirmed durability up to batch_max_seq
- `persistent() &&` — produces `shared_ptr<EngineState>` for publishing

**Three-phase discipline**: Phase 1 is pure in-memory (validate, prepare, compute offsets, apply_writes). Phase 2 is a single IO call. Phase 3 is sync/rotate/publish. If IO throws, the transient's state mutations are already applied but never published — the transient is discarded and the engine degrades.

##### Rotate on batch failure

If `append()` throws mid-batch after `BulkBegin` has been written, the active file contains an orphaned `BulkBegin` with no matching `BulkEnd`. Without intervention, subsequent writes to the same file extend the region that `flush_hints_for` (and `vacuum_scan_and_copy`) treat as part of the incomplete batch — those entries would be silently discarded on recovery.

The fix: the IO loop (step 5) is wrapped in `try/catch`. When the batch is multi-entry and IO fails, the catch block attempts to sync the tainted file and rotate to a fresh active file. If the isolation sync fails, the orphaned batch markers may not be durable — a crash could leave an unresolvable `BulkBegin` — so the engine is poisoned immediately instead of proceeding. If sync succeeds but the isolation rotation fails, the engine is also poisoned (see below). When isolation succeeds, the rotated state is published immediately so subsequent writes go to the clean file, and the exception is rethrown to the caller.

##### Degraded state (`DbDegraded`) and `resume()`

If the isolation sync or rotation after an orphaned `BulkBegin` fails (e.g. `fdatasync` error on the tainted file, or filesystem error creating the new data file), the active file may retain the orphaned marker in a non-durable or inconsistent state. Any subsequent write to that file would appear to succeed in-process but be silently discarded by recovery.

When this happens, the engine calls `deem_as_degraded(reason)` with a diagnostic string identifying the failed file, then rethrows the original exception. From that point:

- **Writes blocked**: `put`, `del`, `apply_batch`, and `vacuum` throw `DbDegraded` immediately. The reason string is available via `degraded_reason()`.
- **Reads available**: `get`, `contains_key`, `snapshot`, `iter_from`, `keys_from`, `riter_from`, `rkeys_from` continue to work. The in-memory state was correctly rolled back (the transient was never persisted), so reads reflect the last successfully committed state.
- **Recovery via `resume()`**: the degraded flag is in-memory only. The service calls `resume()` to attempt in-process recovery without a restart. `resume()` runs a universal recovery process:
  1. Acquires the write lock and drops the heads the failed flush left unpublished (`head_ = state_`), so the resumed state is derived from the published one with those heads already reclaimed.
  2. Rewrites the active file and syncs it (`rewrite_durably`, see *A failed `fdatasync` (fsyncgate)* below), so every byte the scan reads is on the device.
  3. Scans the active file using `CommittedEntryIterator` to find the last valid committed offset. Orphaned `BulkBegin` batches are excluded — if a `BulkBegin` has no matching `BulkEnd`, `committed_offset` is reset to before the batch start. A corrupt entry ends the scan where a CRC failure throws out of the iterator, so `valid_offset` is recorded as the scan advances rather than after it completes: the entries collected for replay and the offset the file is truncated to must describe the same prefix, or step 4 replays key directory entries addressing bytes step 5 removes. `CommittedEntryIterator` defers parsing the entry after a committed one until the caller advances past it, so the throw leaves the `operator++` that steps past an entry already counted, never the one about to yield it; it never catches, because the scan that compacts a file in vacuum relies on the throw to refuse rather than trim. The scan that indexes a hint-less file at open runs with `OnDamage::Stop` instead, and rules on the tail itself (*Recovering a Hint-less File*). An I/O error is rethrown as-is — it says nothing about the bytes. A parse failure below the **published extent** (the active file's `total_bytes` in the published state) is not a failed write's leftovers: it is damage in acknowledged data, or `sync=false` writes a failed `fdatasync` left off the device and the kernel evicted before step 2 read them. Either way readers were served bytes the file no longer holds, and the key directory keeps no older version of the keys they overwrote, so `resume()` throws `std::runtime_error` before truncating, leaving the file exactly as found and the engine degraded. A reopen rebuilds from the hints and recovers what the device holds. There is no recovery contract for damaged published data — the promise is only not to make it worse. The same check is what keeps every published offset inside the file in release builds, where `validate_state_consistency`'s extent walk does not run.
  4. Replays valid committed entries into the key directory using sequence-wins resolution. The scan collects **every** entry type, markers included: a `BulkBegin`/`BulkEnd` pair moves no key and no live byte, but it does consume sequences, and the file's `min_sequence`/`max_sequence` have to count them. Hint files carry markers for exactly this reason, so filtering them here made `resume()` report a `min_sequence` above a sequence the file really contains while a cold open of the same bytes reported the true one — and `ChangeIterator` orders its file queue by `min_sequence`. For each Put whose sequence exceeds the current key_dir entry (or the key is absent), update key_dir and file_stats; for each Delete whose sequence exceeds the current entry, erase the key; for each RangeDel, erase every key in `[from, to)` carrying a lower sequence — the same suppression rule hint replay applies at recovery, because resume and a cold open have to agree on what the same bytes mean. The scan carries the range tombstone's exclusive upper bound through in `ResumeEntry::range_end`; it lives in the entry's value, which the rest of the replay does not read. The switch over `EntryType` is exhaustive rather than an if-chain: an entry type that falls through here is silently dropped, and the resumed key directory then holds keys a fresh open would not. This step recovers entries that were written to the data file but never published to EngineState (e.g. sync-failure paths where only next_seq was advanced, or degraded-state transitions that occurred between IO and state publication). Also advances `next_seq` past the highest sequence seen on disk.
  5. Calls `ftruncate` to remove garbage bytes and orphaned batch markers up to `valid_offset`. In mmap mode this leaves the mapping untouched — readers are not quiesced and may hold spans into it (see *DataFile mmap*, and *View and span lifetimes* in [`CONTRACT.md`](../CONTRACT.md) for what a reader is owed across this call). The file's logical end (and, in mmap mode, `mmap_end_`) is lowered to `valid_offset` *before* the `ftruncate`, not after it: `ftruncate` can fail after it has already cut the file — ext4 sets the new size and drops the page cache, then returns the error of freeing the blocks — and a logical end left above the real end of file sends every record read that over-reads up to it past EOF, failing reads of published data while the engine stays degraded (#236). Only bytes no reader needs lie past `valid_offset`, since the step 3 check keeps it at or above the published extent, so lowering first takes nothing away; if the cut then fails without cutting, the bytes past the lowered end are garbage either way and the next `resume()` cuts them. A reader that loaded the old logical end just before it was lowered still over-reads past the new end; that is why `fetch_record`'s first, speculative read may come back short at end of file, as long as it holds the record's own bytes (#246).
  6. Calls `fdatasync` to persist the truncation.
  7. Seals the active file and dispatches hint generation (both idempotent).
  8. Creates a new active file and publishes the new engine state, with `durable_seq` covering every entry replayed.
  9. Clears the `degraded_` flag.
  If any step throws, the engine stays degraded and the caller may retry. Recovery is idempotent.

##### A failed `fdatasync` (fsyncgate)

When `fdatasync` fails, Linux (since 4.13, `errseq_t`) marks the dirty pages it covered clean and keeps them cached. It reports the error once, and a later `fdatasync` on the file returns 0 without writing them. Reads return the new bytes until the pages are evicted, then the bytes on disk (#231; Rebello et al., *Can Applications Recover from fsync Failures?*, USENIX ATC 2020). The failed range is everything appended since the last successful sync: the failing group, and every `sync=false` write published since by flushes that skipped the `fdatasync`. A process killed before its sync leaves the same shape to the next one, in dirty pages instead of clean ones.

Engines with a second copy treat a failed `fsync` as fatal and recover from it (PostgreSQL replays WAL, InnoDB its redo log). The data file is the only record here, so a restart would just read the same cached pages. Instead, the file is made durable as it reads now, before anything is built from it. `rewrite_durably(path)` opens its own buffered descriptor, reads the file in 1 MiB chunks, writes each chunk back to the offset it came from — `write()` dirties a page even when its bytes are unchanged — and `fdatasync`s. What was read is what the device holds, relying on nothing beyond the trust every commit already puts in `fdatasync`. If the pages were evicted before the read, the device's bytes are written back and the scan stops at them, which is correct: those writes were never durable. It is called at the two places that build state from bytes whose durability is unknown:

- **`resume()`**, on the active file before its scan (step 2 above), up to its logical end, where the scan stops; past it lie only the zero-filled preallocation and the bytes of an append that failed. All of it, not the range since the last successful sync: resume is rare, writes are stalled while degraded, and it needs no new engine state.
- **`DB::open`**, on every hint-less file before it is read (*Recovering a Hint-less File*), whole: a new process cannot know which bytes an earlier one synced, or whether it saw an error, and no writer says where its entries end.

The scan reads through `read_raw`, a `pread` of the page cache on every back-end. Under the buffer pool the active file's point reads come from the frames the writer filled, which hold what was appended, not what the rewrite made durable; the sweep never reads them. If the device holds less than was published, the scan stops below the published extent and resume refuses, so no frame above the scan's end is ever addressed.

`sync=false` writes in a failed range can be lost without a crash, as they would be at a power loss: when their pages are evicted before resume reads them back. [`CONTRACT.md`](../CONTRACT.md) says so.

An `O_DIRECT` read of the tail was rejected: it bypasses clean cached pages but is evidence, not proof. The device's volatile write cache can hold what it returns, and with nothing dirty after the error, whether the next `fdatasync` sends a cache flush depends on the filesystem. Some filesystems honour the flag and still serve reads from a cache (ZFS before 2.3, NFS).

Implementation: `degraded_` is an `atomic<bool>` (release on write, acquire on read). The non-atomic `degraded_reason_` string is safely published via the release/acquire pair — the string is written before `degraded_.store(true, release)` and read after `degraded_.load(acquire)`.

##### Runtime invariant enforcement

The engine validates structural invariants at runtime before publishing state, not just in tests. `store_state` compares old and new `EngineState` on every publication: `next_seq`, `active_file_id`, `next_file_id`, and `durable_seq` must never regress, and the new state's `durable_seq` must cover its `sync_requested_seq` (durability before visibility). On violation the engine degrades (nothing published, writes blocked, reads remain available). Cost: four integer comparisons per write — unmeasurable against `pwritev` + `fdatasync`. Debug builds add a full `next_seq > max(key_dir sequences)` walk. When `durable_seq` advances, `store_state` notifies `durable_cv_` — the condvar used by `durable_sequence(min_sequence, timeout)` for long-poll.

On cold paths (`DB::open()`, `resume()`), `validate_state_consistency` runs the full O(n) structural check: active file in registry, no dangling file references, `next_seq` ahead of all sequences, `file_stats` covers all files, `live_bytes` matches `key_dir`. On violation it throws — the DB does not open or `resume()` fails.

##### Short writes

A `pwritev` on a regular file returns short when the file runs out of room part way (a full disk, `RLIMIT_FSIZE`), and sets no `errno`. Every data and hint file write checks its result with `check_write` (`bytecask.util`): -1 is reported with its `errno`, and a short count as `EIO` naming the bytes the file took, never as whatever an earlier call left in `errno` (#221). The write is not continued: the bytes that landed are past the logical end, the engine degrades as for any append failure, and `resume()` cuts what lies past the last committed record.

##### Durable sequence tracking

`durable_seq` is a field on `EngineState` that tracks the highest sequence number confirmed by `fdatasync`. Its companion `sync_requested_seq` is the highest sequence written by a `sync=true` slot (`TransientEngineState::note_sync_requested`); a state may only be published when `durable_seq >= sync_requested_seq`, checked by `store_state`.

On the common path `flush_pending` advances it: after a successful fdatasync of the prepared head it publishes a copy of that head with `durable_seq = next_seq - 1` (every entry appended before the fdatasync started). On the rotation barrier and in `ingest`, `TransientEngineState::apply_sync(batch_max_seq)` is called after each successful `file.sync()` as before. If a sync fails, neither path advances it — the published state carries the previous `durable_seq` unchanged. The monotonicity guard in `apply_sync` makes repeated calls idempotent.

`durable_sequence(min_sequence, timeout)` exposes `durable_seq` to callers (renamed from `current_sequence` — BC-231, since "current" was ambiguous between the highest *allocated* and highest *durable* sequence). It is the single sequence primitive: `min_sequence = 0`, an already-reached target, or a nonpositive `timeout` all return the current watermark immediately without blocking. Otherwise it blocks on `durable_cv_` (notified by `store_state` when `durable_seq` advances) until `durable_seq >= min_sequence` or the timeout expires, then returns the current watermark. The condvar notification is centralized in `store_state` — one place, one check. This single target-based primitive covers polling (`min_sequence = 0`), the replication wake-up (`min_sequence = follower.durable_sequence() + 1`), and RYOW waits (`min_sequence = result.sequence` from a `CommitResult`) — see `docs/commit_result_api_design.md` and `docs/replication_primitives_design.md`.

After recovery (`DB::open`, `resume`), `durable_seq` is set to `next_seq - 1`: every recovered entry is durable. Sealed files were synced whole before they were sealed, and every file whose durability is unknown — a hint-less file at open, the active file at resume — is rewritten and synced before it is read (*A failed `fdatasync` (fsyncgate)*).

##### Post-write rotation failure

After all appends succeed and mutations are applied, the engine may rotate the active file if it exceeds the size threshold. Rotation syncs the file, seals it, and creates a new active file. Two distinct failures can occur:

- **Sync fails before seal**: the file is not sealed, and the engine degrades without publishing key changes. A later `fdatasync` would return 0 without writing the pages the failed one left clean (*A failed `fdatasync` (fsyncgate)*), so nothing is retried on trust; `resume()` rewrites and syncs the file.
- **File creation fails after seal**: `rotate_active_file` calls `seal()` before creating the new file. If creation fails, the active file is sealed and cannot accept further appends. The engine degrades the DB and publishes state. `resume()` creates a fresh active file. A failed sync of the directory after the create (see *Directory Sync*) takes the same path: the file exists, its name is not known to be durable, and no write is acknowledged into it.

##### Sync failure: advance sequence, discard key changes

If `fdatasync` fails (in `flush_pending`, or the rotation sync), the write is not
confirmed durable. Key-directory changes are not published — the written key is not
visible to callers. `next_seq` is advanced past the consumed sequence numbers to
prevent sequence reuse for bytes now in the page cache. The caller receives the
exception and must retry. This matches the contract of every other peer engine.

With the pipeline the failed flush is wider than one batch: every writer whose
entries were appended since the last successful flush built on unpublished
state, so all of them receive the same `std::system_error` (`flush_error_`,
rethrown by `commit_wait`), the engine degrades, `head_` is reset to the
degraded published state, and later writers get `DbDegraded` at stage 1.
`resume()` clears `flush_error_` after it republishes; its active-file scan
replays those entries as before.

##### Durability before visibility

`state_.store()` happens **after** `fdatasync` in all cases. A write is never
visible to readers until it is durable on disk (or explicitly chosen as
`sync=false` by the caller). The pipeline keeps this exactly: readers only ever
load `state_`; the prepared head that may contain non-durable entries is
private to the write path, and `store_state` rejects any state with
`sync_requested_seq > durable_seq`.

Consequences:
- `write_mu_` is held for stage 1 only (validate, apply, `pwritev`). The `fdatasync` runs under the flush role, outside `write_mu_`, so the next batch's stage 1 overlaps it. Concurrent writers are still serialised in the order their stage 1 ran.
- `sync = false` writes still have no durability guarantee. Visibility is immediate when no flush is in flight; behind an in-flight flush it is deferred until that flush lands, because the write was built on state the flush has not yet published.
- After recovery, in-memory state is consistent with what is on disk.

##### Read path (no lock, no mutex)

```
  Reader thread N

  1. snap = load_state(opts)  // const ref to thread-local shared_ptr
     └─ no refcount bump — returns a const& to the TL cache.
        The shared_ptr stays alive until the same thread
        calls load_state again.

  2. tree lookup via raw const Node* pointers
     └─ no atomic traffic of any kind.
        Safe: snap pins the tree version → keeps all
        descendants alive. The write path clones shared
        nodes before mutating — old nodes stay intact.

  3. pread(fd, ...) for value retrieval
     └─ stateless, no synchronisation.
```

The read path never acquires `write_mu_`. `load_state` returns a `const&` to a thread-local `shared_ptr<const EngineState>`, avoiding the refcount increment/decrement that `atomic<shared_ptr>::load()` would impose on every read. `get` and `contains_key` bind that reference rather than copying it: a copy is a read-modify-write on a control block every reader thread shares, and that one line capped concurrent gets at ~25 M/s on 32 threads before the read itself did (measured; see *Operational Counters* for the other line). The key directory lookup walks raw `const Node*` pointers and the nodes themselves hold no counters, so a lookup does no atomic writes at all. Both are safe because the thread-local snapshot pins the tree version for the duration of the `get()` call. Iterators do take a copy, because they outlive the call.

**A thread that stops reading must not keep its version alive.** The cache is refreshed only by its own thread's next read, so on its own a thread that reads once and then idles would keep the key directory version it saw alive for as long as it idles. Reclamation is per node — a retired node is freed once no live version reaches it (see *Persistent B+ tree*) — so under a write load that touches the whole key space, one version held long enough comes to hold a whole retained copy of the tree: ~1.1 GB at 20 M keys, measured on the MariaDB plugin, where the thread that ran plugin initialisation and threads parked in the server's thread cache did exactly that. The engine therefore takes an idle cache away without its thread's help, the way RocksDB scrapes its per-thread `SuperVersion`: every thread's cache is a registered slot (`ReadCacheSlot`); a read claims its own slot with one uncontended `exchange` to `kInUse`, uses the entry, and stores it back; and the writer, on publishing — rate-limited to ten scrapes a second, and also from `vacuum()` and `stats()` so that a pin left by the last write is released when writes stop — swaps any slot idle for ~1 s to `kObsolete` and frees its entry. A claim in flight makes the swap fail, so an entry is freed only when no reader can hold it; a reader that finds the sentinel re-acquires the current state. "Recently used" is a scrape epoch the reader copies from a counter, not a clock, so the read path gains one `exchange` on its own cache line (measured at 1–5 ns) and nothing shared. Snapshots and iterators are explicit objects the caller holds and are outside this: they pin what they can reach until destroyed, by design. Closing a DB uses the same swap to take every entry that DB owns, from every slot, rather than waiting for idleness (see *`DB::close()`*). `stats()` exposes the two numbers that make all of this visible: `keydir_versions_live` (1 = only the published state) and `keydir_nodes_parked` (retired nodes older versions still pin); the second climbing towards the node count while the first stays above one names a holder, not a leak.

**Same-thread guarantee**: a `put` followed by a `get` on the **same thread** always observes the put — the `store()` in step [4] is sequenced before the `load()` in the subsequent `get()`.

#### Read-scaling behaviour

Benchmarks on a 22-vCPU instance (50k keys, 1 KiB random values):

| Threads | Before (mutex only) | After (atomic shared_ptr) | Improvement |
|---------|---------------------|--------------------------|-------------|
| 2       | 1.66 M ops/s        | 2.16 M ops/s             | +30%        |
| 4       | 1.73 M ops/s        | 3.01 M ops/s             | +74%        |
| 8       | 1.74 M ops/s        | 3.57 M ops/s             | +105%       |
| 16      | 1.91 M ops/s        | 3.67 M ops/s             | +92%        |

Throughput scales near-linearly with thread count for read-heavy workloads.

#### Read consistency

A read (`get`, `contains_key`, the iterators, `snapshot()`) sees every write that returned before the read began, and every write another read has already seen, on any thread. There is no option to relax this. `ReadOptions` carries only `verify_checksums`.

Reads go through a per-thread cache of the engine state, so a read that finds nothing new costs two plain loads and no reference-count traffic on the shared state. The cache entry is keyed by the owning `DB` instance (a raw pointer comparison). A thread that reads from more than one `DB` in the same process switches cache targets transparently: the first read against a different instance always refreshes (BC-243). This costs one pointer compare on the hot path, and only matters in practice for processes that open several `DB`s and read from them on one thread, such as tests or a service that shards across directories.

##### Mechanism

Every publication passes through one raw store, which brackets the store of the new state:

```
  Writer:  publishing_.fetch_add(1)                  ← a publication is in progress
           state_.store(S1, seq_cst)                  ← new immutable snapshot
           state_gen_.store(next_state_gen(), release) ← process-wide unique generation
           publishing_.fetch_sub(1, release)

  Reader:
           busy = publishing_.load(acquire); gen = state_gen_.load(acquire)   ← two MOVs on x86
           if busy != 0 or gen != tl.gen:
               tl.snapshot = state_.load()            ← refresh (refcount bump)
               tl.gen = busy ? none : gen
           return tl.snapshot
```

No clock call on the reader side, no locked instruction, no refcount traffic until a refresh is needed.

##### Why a timestamp was not enough

The cache used to refresh when a timestamp, `state_time_`, stored after each publication, changed. A timestamp stored after the state cannot tell a reader that a publication it has not yet seen finish is already visible elsewhere. Two failures followed from that, and the Elle isolation check found both:

- **A writer returned inside a publication.** With the commit pipeline, the thread that publishes `S1` is whichever thread holds the flush role. A writer polling `state_` in `commit_wait` could see its write covered and return before the publisher stored `state_time_`. Reads that started after the return, on other threads, then served their cached pre-write state. This was fixed in #180 by advancing `state_time_` in `commit_wait`.
- **A reader saw a publication another reader then missed.** A reader with nothing cached loads `state_` directly and sees `S1`. A reader on another thread, which starts after the first one has finished, compares timestamps, finds nothing new, and serves its cached `S0`. No writer has returned, so nothing on the write side can close this gap. Elle reported it as `G-single-item-realtime` on the leader in the replication check ([`replication_checking_design.md`](replication_checking_design.md)).

`publishing_` closes both. It is raised before `state_` is stored. So anyone who has seen `S1`, whether a reader that loaded it or a writer whose commit it covers, happens-after the raise. A read that starts after them therefore finds `publishing_` raised, or finds `state_gen_` moved on because the publication has finished, and reloads. This subsumes #180's fix, which is gone. What remains is the ordinary overlap: a read that runs concurrently with a publication may see either state, as it may under any ordering.

Generations come from a single process-wide counter. A read cache knows its DB only by address, and a new DB can reuse the address of one that was destroyed. A per-DB counter would restart at 0 and could hand the new DB a generation that the cache still holds for the old DB. The cache would then serve the old DB's state. A test sequence that opens a DB at the same stack address in every case reproduced exactly that.

##### Bounded staleness, removed

`ReadOptions::staleness_tolerance` used to let a reader keep its cached state for up to that long after a write, trading freshness for throughput. That included writes made by the same thread. Measured on a 22-vCPU machine against one background writer, it was worth +42% read throughput at 8 threads and +188% at 16 (1.39 M → 1.98 M and 846 k → 2.44 M ops/s), because session readers contend on the spinlock inside `atomic<shared_ptr>` on every refresh.

It was removed. By design it gave up read-your-writes, and the guarantees above are the ones the isolation and replication checks verify. It was also the only reason for a publication timestamp beside the publication counter. A workload that needs the throughput can hold a `Snapshot` for as long as it is willing to read stale data.

`engine_bench` compares ByteCaskDB against LevelDB and RocksDB across Put, Get, Del, Range50, Mixed, MixedBatch, PutMT, and MixedMT benchmarks at both NoSync and Sync durability levels. RocksDB compression is disabled (`kNoCompression`) and values are 1 KiB of random (incompressible) bytes so neither LevelDB nor RocksDB gains an advantage from Snappy/block-cache effects.

### File Registry

The engine maps each monotonic `uint32_t` file ID to its open `DataFile` (`EngineState::files`), and keeps per-file `FileStats` the same way (`EngineState::file_stats`). Both are copy-on-write maps from `bytecaskdb/u32_map.cppm`: a copy is an O(1) snapshot, and a write goes through `transient()` / `persistent() &&` like the key directory, so a reader holding an old `EngineState` keeps the registry it started with, and every `DataFile` in it open, without locking.

The module has two implementations of one interface (the `PersistentU32MapOf` / `TransientU32MapOf` concepts), because the two maps are used in opposite ways:

- **`files` is a `PersistentU32Table`**, a direct-addressing table: one slot per ID between the lowest and highest ID held, so a lookup is an index. It is read on every record access — a range scan on the blind key directory looks a file up both to read each key and to read its value — and written only at rotation, vacuum and open. A transient shares its base table until its first write, which copies it, so the per-batch transient the write path takes costs nothing. IDs are minted in sequence and capped at `KeyDirEntry::kMaxFileId` (2^20 − 1); erasing the lowest or highest ID trims the table, so it spans the IDs held. A file that is never vacuumed keeps every slot above it, one per rotation since.
- **`file_stats` is a `PersistentU32Map`**, over the keyed `PersistentBTree`: it is updated by every commit and read only by vacuum and `stats()`, so a write must not copy the map.

Neither is built on the key directory's tree. The blind-leaf tree stores no key bytes and reads each key back from its record, and a file ID is in no record. Measured with `engine_bench` at 50k keys against the radix tree `files` was on before: `Range50` 4.44 → 3.83 µs, `Get` 323 → 303 ns, writes unchanged. A one-entry lookup costs about 0.75 ns in the table, 6.8 ns in the radix tree and 8.9 ns in the keyed B+ tree, and the gap widens with the number of files.

### Data File Lifecycle

Each data file transitions through three phases in sequence:

| Phase | Description |
|-------|-------------|
| **Active** | The current append target; accepts all writes. |
| **Rotating** | Sealed (`fdatasync` + `seal()`); a companion `.hint` file is being written by the background worker. |
| **Immutable** | The `.hint` file exists; the data file is sealed and read-only. |

A new active data file is always created on engine startup.

### Vacuum

(This project uses the PostgreSQL term *vacuum*; other systems call it *compaction* or *merge*.)

ByteeCask implements a **conservative online vacuum**: the engine continues to serve reads and writes while vacuum rewrites sealed data files. *Conservative* means the design prioritises write-path non-interference and correctness over maximum space reclamation efficiency.

#### Constraints

- Only sealed (immutable) data files are considered — the active file is never touched.
- Only files whose fragmentation exceeds a configurable threshold are processed; files below the threshold are left alone.
- One file is processed per `vacuum()` call. Callers that want to process multiple files call in a loop.
- A file that compaction cannot shrink is declined: the staged copy is discarded and `vacuum()` returns `false`. Tombstones and batch markers are counted as kept bytes (`tombstone_bytes`, `marker_bytes`), so they never make a file look reclaimable: a selected file always has dead Puts to drop, and a vacuum-to-convergence loop terminates.
- A tombstone is dropped only when recovery found that no other file holds an older Put it hides, and only in a file compacted for its dead Puts (see **Tombstone handling** below).
- A new compacted file is fully written and `fdatasync`-ed before any old file is removed.
- **Sequence-disjoint files**: vacuum must preserve the invariant that all data files have non-overlapping sequence ranges. Compacted files maintain disjoint sequence ranges from other files.

#### Fragmentation

The fragmentation of a sealed data file is the fraction of disk space compaction is sure to reclaim — its dead Puts:

```
fragmentation = 1 − (live_bytes + tombstone_bytes + marker_bytes) / total_bytes
```

- `total_bytes` — physical file size: all appended bytes, including dead puts, tombstones, and BulkBegin/BulkEnd markers.
- `live_bytes` — sum of entry sizes (`kHeaderSize + key_size + value_size + kCrcSize`) for Put entries currently referenced by the key directory.
- `tombstone_bytes` — sum of entry sizes for Delete and RangeDel entries in the file. Tombstones are never live and count as kept: compaction copies every one recovery found still needed (see **Tombstone handling**), and the ones it may drop are not what a file is compacted for. A file holding nothing but tombstones has fragmentation 0 and is never selected.
- `marker_bytes` — `kHeaderSize + kCrcSize` per BulkBegin/BulkEnd marker. Compaction keeps every marker, so they count as kept too; without that, a file whose only dead bytes were markers stayed eligible, and every `vacuum()` call staged an identical copy of it and gave up (#169).

Tombstones and markers count as neither live nor dead. Counting them as dead (the formula was `1 − live_bytes / total_bytes`) made a file of tombstones measure 100% fragmented however often it was compacted: every `vacuum()` call picked it, staged an identical copy and discarded it (#169). Tombstones that recovery found droppable are not counted as reclaimable either — a file is never compacted just to drop tombstones; they go when the file is compacted for its dead Puts.

A file qualifies for vacuum when `fragmentation > VacuumOptions::fragmentation_threshold` (default `0.5`).

#### Live fragmentation tracking

Rather than computing `live_bytes` at vacuum time (which would require a full key-directory traversal — O(total_keys) — or scanning sealed data files), the engine maintains per-file stats updated incrementally.

```cpp
struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};  // lowest sequence in this file (0 = no entries)
  std::uint64_t max_sequence{0};  // highest sequence in this file (0 = no entries)
  std::uint64_t tombstone_bytes{0};  // Delete + RangeDel entries: not live
  std::uint64_t marker_bytes{0};     // BulkBegin + BulkEnd markers: not live
};
```

`file_stats` is a `std::map<uint32_t, FileStats>` inside `EngineState`. It is copied into `TransientEngineState` on each write and updated as part of the state transition. This keeps all mutable state under the transient/persistent discipline.

The helper `entry_size(key_size, value_size)` returns `kHeaderSize + key_size + value_size + kCrcSize` and is used everywhere stats are updated.

##### Write-path updates

All stats updates happen inside `TransientEngineState::apply_writes`:

- **On Put**: if the key already exists (overwrite), subtract `entry_size(key.size(), old_entry.value_size)` from `file_stats[old_entry.file_id].live_bytes`. Add `entry_size(key.size(), value.size())` to `file_stats[active_file_id].live_bytes` and to `.total_bytes`.
- **On Del**: if the key exists, subtract `entry_size(key.size(), old_entry.value_size)` from `file_stats[old_entry.file_id].live_bytes`. Add the tombstone size (`kHeaderSize + key.size() + kCrcSize`) to `file_stats[active_file_id].total_bytes` and `.tombstone_bytes`. The tombstone is never added to `live_bytes` — tombstones are never referenced by the key directory.
- **On DelRange**: subtract each erased key's entry size from its file's `live_bytes`, and add the range tombstone's size (`entry_size(from.size(), to.size())`) to the active file's `total_bytes` and `tombstone_bytes`.
- **BulkBegin/BulkEnd markers**: each add `kHeaderSize + kCrcSize` to the active file's `total_bytes` and `marker_bytes` — they are never referenced by the key directory.
- **On rotation**: `apply_rotate_file` inserts `FileStats{0, 0}` for the new active file.

At vacuum time fragmentation is an O(1) integer division per file — no scanning, no I/O, no additional lock contention.

The active file's `live_bytes` may be non-zero (it holds the current live writes), but the active file is never a vacuum candidate, so its fragmentation is never evaluated.

##### Recovery stats reconstruction

`file_stats` must be rebuilt on startup. Both `total_bytes` and `live_bytes` are reconstructed without scanning data files or traversing the key directory:

- **`total_bytes`**: computed via `std::filesystem::file_size(path)` per sealed file in `open_and_prepare_files()`. Exact for append-only files. O(1) per file, no I/O beyond a `stat` call.
- **`tombstone_bytes`, `marker_bytes`**: summed per file from its hint file's Delete and RangeDel entries and its BulkBegin/BulkEnd markers, in every recovery path, as the sequence bounds are. Hint files carry all of them, so this needs no data file read. `apply_resume` rebuilds both for the resumed file from the committed entries it scans.
- **`live_bytes`**: reconstructed as a side-effect of the existing hint-file recovery pass. The hint entry carries `key_size` (from `key.size()`) and `value_size`, so `entry_size(key_size, value_size)` is computable without touching the data file.

The displacement logic mirrors write-path updates:

```
for each hint entry h (processed in arbitrary file order, sequence wins):
  if h.type == Put and h wins over existing entry o:
    file_stats[o.file_id].live_bytes -= entry_size(o.key_size, o.value_size)
    file_stats[h.file_id].live_bytes += entry_size(h.key_size, h.value_size)

  if h.type == Delete and h wins over existing entry o:
    file_stats[o.file_id].live_bytes -= entry_size(o.key_size, o.value_size)
    // tombstone never enters live_bytes
```

Processing order across files does not matter — the canonical key-ownership comparator always picks the same winner regardless of merge order, so `live_bytes` converges to the right values.

##### Canonical key-ownership comparator

Sequence numbers are unique per logical write. When two `KeyDirEntry` values claim the same key, the one with the higher sequence wins. If two entries share the same sequence number, they must point to the same physical record (`file_id`, `file_offset`). Two offsets in one file are corruption and throw `std::runtime_error`. Two files throw `SequenceOverlap`, which `recovery_open` catches: that is the shape an interrupted vacuum leaves, and it is resolved or rejected as described under *Vacuum → Crash safety*. This comparator is commutative, so merge results are independent of worker count, completion order, or file iteration order. Both serial and parallel recovery use this comparator.

Recovery assigns file ids in name order (`recovery_prepare_files` sorts the `.data` paths), so ids are a function of the directory's contents rather than of the order the filesystem lists it in. A failure in any phase of the ranged merge is an exception from `DB::open`: its `parallel_for` catches on each thread and rethrows on the caller's after joining, where an exception leaving a thread used to be `std::terminate`.

##### Sequence bounds

`min_sequence` and `max_sequence` track the range of sequence numbers written to each file. They enable `changes_since` to skip irrelevant files during replication and are maintained alongside `live_bytes`/`total_bytes` in every write path:

- **`apply_writes`**: captures the batch's start and end sequence once per write batch.
- **Vacuum**: `vacuum_scan_and_copy` tracks sequences of all copied entries (including batch markers). `apply_vacuum` propagates bounds to the compacted file.
- **`apply_resume`**: resets bounds on truncation, rebuilds from committed entries.
- **Recovery**: both serial and parallel paths track min/max per file during hint replay.

Invariant: both are zero (no entries yet) or both are non-zero with `min_sequence <= max_sequence`. Enforced by `validate_state_consistency`.

**Parallel recovery**: each worker tracks bounds for its own files. Bounds carry through the Phase 3 merge unchanged (file IDs are disjoint across workers). Phase 4 does not modify sequence bounds.: `RecoveryResult` includes a `file_stats` map alongside `key_dir`, `tombstones`, and `max_seq`. Each worker builds its own `file_stats` during Phase 2 using the algorithm above. During Phase 3 sequential accumulator merge, `file_stats` maps are unioned (file IDs are disjoint across workers due to round-robin). The merge does **not** recompute `live_bytes` — instead, a single `live_bytes` pass runs once after the final merge in Phase 4 (assembly), iterating the fully-merged tree exactly once.

#### Vacuum primitives

Vacuum uses two self-contained, independently testable paths. The `vacuum()` caller picks exactly one per target file based on whether the file holds anything that may need keeping: live entries or tombstones.

##### `vacuum_compact_file(file_id)` — rewrite a sealed file, dropping dead entries

Used when the file's live data is too large to fit into the active file. Produces a new, sealed, compacted file.

1. **Snapshot the key directory** — call `state_.load()` to obtain the current `EngineState`. This is the authoritative view of which entries are live.
2. **Rewrite the target file** — open a new data file at a `.data.tmp` path for writing (new timestamp stem, same directory). Scan the old data file entry by entry using `CommittedEntryIterator` (see below). For each emitted entry:
   - *Put entry*: check whether the snapshot's key directory entry for that key points to the old file at this offset with the same sequence number. If yes (live), write it to the new file at its new offset, recording `(key → new_file_id, new_offset)`. If no (dead), skip.
   - *Delete or RangeDel entry*: dropped if `NeededTombstones::droppable(sequence)`, otherwise copied verbatim (same sequence number, same key). See **Tombstone handling** below.
   - *BulkBegin/BulkEnd marker*: always copied.
   If nothing at all is left to copy — every Put dead, every tombstone droppable — the staged file is discarded and the source is removed with `vacuum_remove_file` instead of publishing an empty file.
3. **Seal and durability** — `fdatasync` the tmp file, close it. Rename `.data.tmp` → `.data` atomically. Open a new `DataFile` at the final path and seal it. Write a hint file by scanning the compacted file (no batches in the output), using the temp-then-rename protocol (`.hint.tmp` → `.hint`).
4. **Atomic commit** (under `write_mu_`):
   0. If `durable_seq` is below the last sequence of the step 1 snapshot, `fdatasync` the active file and advance `durable_seq` first. See **What supersedes must be durable** below.
   a. Take a transient of the current key directory.
   b. For each live Put entry copied to the new file, look up the key in the current key directory. If the sequence number still matches (no concurrent write superseded it), update `KeyDirEntry` to the new `file_id` and `file_offset`. If the sequence number differs, skip — the concurrent writer's version takes precedence.
   c. Call `persistent()` to obtain the new immutable key directory.
   d. Build an updated `FileRegistry`: add the new compacted file, remove the old file.
   e. Update `file_stats_`: remove the old file's entry. Insert a new entry for the compacted file using the exact `compacted_live_bytes` tracked during step 2 — for each entry whose key-dir sequence no longer matched in step 4b (concurrent write won), its `entry_size` was subtracted from the running total. `total_bytes` is the physical size of the new file; `tombstone_bytes` and `marker_bytes` count the tombstones and markers copied. (Note: `compacted_live_bytes` may be less than `total_bytes` because the new file also contains tombstones and markers that are never counted as live, and entries superseded by concurrent writes during the I/O phase.)
   f. Publish the new `EngineState` via `state_.store()`.
5. **Release `write_mu_`**.
6. **Unlink old file** — the old file is removed from the registry and its `.data` and `.hint` files are unlinked from the filesystem immediately after the commit. Existing readers continue via their open file descriptors — POSIX guarantees that `pread` on an unlinked file succeeds as long as the fd is open. Disk blocks are freed when the last `shared_ptr<DataFile>` is destroyed, closing the fd.

##### `vacuum_remove_file(file_id)` — delete a file with nothing to keep

Used when `live_bytes == 0` and `tombstone_bytes == 0` — the file holds only superseded puts and batch markers. No I/O is required. Also used by `vacuum_compact_file` when its scan left nothing to copy — every Put dead and every tombstone droppable — instead of publishing an empty file.

A file with no live entries but with tombstones goes through `vacuum_compact_file` instead, which keeps the tombstones still needed and drops the rest. Dropping it whole would drop them all, and a Put they shadow in an older file would come back at the next open (#166).

1. **Snapshot the key directory** — call `state_.load()` to obtain the current `EngineState`.
2. **Commit the removal** under `write_mu_`:
   0. Make every write in the snapshot durable, as in `vacuum_compact_file` step 4.
   a. Remove the file from the registry.
   b. Remove the file's entry from `file_stats_`.
   c. Publish the updated `EngineState`.
3. **Unlink the files** — remove the `.data` and `.hint` files from the filesystem.

This is a pure metadata operation — no scanning, no copying, no syncing. The file contained no live data and no tombstone that could still matter, so removing it has no effect on readable keys, now or after reopen.

#### Committed entry scanning

`vacuum_compact_file` uses `CommittedEntryIterator` (`scan_committed`) when reading the sealed data file. The iterator yields entries one at a time, including `BulkBegin`/`BulkEnd` markers. Internally, entries between `BulkBegin` and `BulkEnd` are buffered and only yielded when `BulkEnd` is reached. If the file ends mid-batch (crash during `apply_batch`), the buffered entries are silently discarded — they were never committed.

**Why**: without committed-entry filtering, vacuum would copy uncommitted entries (including tombstones from incomplete batches) into the output. Those tombstones could incorrectly shadow live Puts in other files, causing data loss on recovery. This matches `flush_hints_for`'s hint generation.

(`vacuum_remove_file` performs no scanning — it simply removes file metadata.)

#### Crash safety

`vacuum_compact_file` writes the new data file to `.data.tmp` and renames it atomically to `.data` after `fdatasync`. If the engine crashes mid-write, recovery ignores `.data.tmp` files (it only processes `.data` extensions) and cleans them up in `open_and_prepare_files`. The hint file uses the same `.hint.tmp` → `.hint` protocol.

Only a crash leaves vacuum's copy behind. A vacuum that returns or throws before its commit removes the copy on the way out: before the rename the staging file — nothing to reclaim, or a failed scan, copy, sync, shrink or rename (#235) — and after it the copy under its final name and the hint written for it — a failed directory sync, open, hint write or commit (#304). Until the commit the published state holds the source and nothing reads the copy, so removing it loses nothing. A caller retrying vacuum under a persistent fault such as ENOSPC or EMFILE therefore does not leave a full copy per attempt; the chaos rig found 58 left that way, the next open running one recovery pass per copy. A removal that fails under the same fault, or that a power cut loses, is ignored and left to recovery; it never replaces the exception in flight. The final name joins the cleanup only once the rename has returned: before that it may be a file a concurrent rotation minted.

The rename is followed by a sync of the directory before vacuum commits (#199). Without it, a power loss could persist the later unlink of the source and drop the rename: the next open would find only `.data.tmp`, delete it as staging, and lose every live entry the source held. A failed sync throws before the commit, and vacuum removes the renamed copy on the way out; if that removal is lost, the copy and its source are both on disk, which is the interrupted-vacuum shape below.

After the rename there is a second window. Vacuum scans the compacted file C to write its hint, commits, and only then unlinks the source S. A kill anywhere in there, or a failed unlink after the commit, leaves C and S both on disk, holding the same entries under the same sequences — seconds for a 64 MiB file, and a cgroup OOM kill under a write-heavy load found it. Vacuum is committed on disk only once S is unlinked, so recovery undoes it (`DB::recovery_open`, design in [`vacuum_crash_recovery_design.md`](vacuum_crash_recovery_design.md)):

1. Recovery stops at the first sign of two files sharing sequences: `kde_newer` throws `SequenceOverlap` when two files claim one key under one sequence, and `find_sequence_overlap` checks the per-file sequence ranges once recovery is done, for a pair that shares no key. The ranges check is over files, not keys, and is the only cost a healthy open pays.
2. `recovery_undo_interrupted_vacuum` takes the smaller file as C and reads both data files, checking that every committed entry of C appears in S in order with the same sequence, type, key and value.
3. If it does, C's hint and then C are deleted, with a line on stderr, and recovery runs again from scratch (with a fresh buffer pool, since ids are reassigned). If it does not, `DB::open` throws: the two files are not a compaction pair, and nothing is deleted.

Deleting C is safe on exactly what step 2 checks: everything in C is also in S. Deleting S instead would also assume that S's other entries are dead, which nothing checks; a copy of a prefix of a file passes step 2, and deleting the larger file there would lose every write after the copy. The cost is that the next vacuum redoes the compaction. After reopening, files are sequence-disjoint again (D18), so `changes_since` yields each entry once.

`vacuum_remove_file` has no crash safety concerns — it performs no I/O, only removes file references from engine state.

##### What supersedes must be durable

The snapshot a file is judged by is the published state, and it includes `sync = false` writes that no `fdatasync` has covered yet. A Put superseded only by such a write looks dead, and dropping it is durable once the source is unlinked. A power cut then loses the new write, which `sync = false` allows, and the old value with it, which it does not: the key is gone, a state no prefix of the history produces (#245, found by the chaos rig).

So both primitives, in `vacuum_commit` and before the source can be unlinked, make every write up to the snapshot's last sequence durable: an `fdatasync` of the active file under the write barrier, advancing `durable_seq` as a commit would. It is skipped when `durable_seq` already covers the snapshot, which is the case whenever the writes are synced. A failed sync degrades the engine, like any failed `fdatasync` on the write path, and vacuum throws before committing, leaving the source in place. A degraded engine cannot trust a later sync (#231), so vacuum refuses there too. The alternative, dropping a record only when its superseder is at or below `durable_seq`, adds no sync but leaves the rest for a later vacuum that has to find them again.

#### Vacuum caller (`vacuum()`)

The public `vacuum()` method orchestrates file selection and dispatches to exactly one primitive:

1. **Acquire `vacuum_mu_`** — prevents two `vacuum()` calls from running concurrently.
2. **Order the candidates** — copy `file_stats_` under a brief `write_mu_` acquisition (O(sealed files), then release). Iterate sealed files, compute `fragmentation` (see **Fragmentation**; O(1) per file, no I/O), and keep those above `fragmentation_threshold`, most fragmented first. A file whose `min_sequence` is above `retain_after` is left out: everything in it is kept (see **Retention**). If no file qualifies, return immediately.
3. **Branch**, for each candidate in turn:
   - If `live_bytes == 0`, `tombstone_bytes == 0` and `max_sequence <= retain_after` → call `vacuum_remove_file(target)` (fast path, no I/O) and return.
   - Otherwise → call `vacuum_compact_file(target)` (sealed→sealed compaction). If it made the file smaller, return; if not (every dead entry in it may be above `retain_after`), try the next candidate. Every `true` still removes bytes, so a `while (vacuum(...))` loop terminates.
4. **Release `vacuum_mu_`**.

#### Tombstone handling

Tombstone entries (Delete and RangeDel) record that keys were explicitly removed. A tombstone matters only while it hides something: if another data file still holds an older Put of a key it deletes, recovery would see that Put and resurrect the key unless the tombstone survives to overrule it. Every other tombstone can go.

A tombstone `(K, s)` in file T is **needed** when some file other than T holds a `Put(K)` with a sequence below `s` (for a RangeDel, any key in its range). Everything else is droppable:

- **Superseded by a newer Put**: the newer Put wins on its own.
- **Not the newest tombstone for its key**: the newest one covers the same Puts.
- **The older Put is in T itself**: compacting T drops that dead Put in the same pass that drops the tombstone. If a crash leaves T and its compacted copy side by side, recovery deletes the copy and keeps T, Put and tombstone together.
- **No older Put anywhere**: there is nothing to hide.

**Recovery decides; compaction applies.** Recovery already reads every hint entry, and hint files are not deduplicated — dead Puts included. It collects the sequences of needed tombstones into `NeededTombstones` (a sorted `vector<uint64_t>`, `DB::needed_tombstones_`), together with `horizon`, the highest sequence it saw. No extra pass: the marking happens where each path already resolves a key.

- *Streams (blind) and sorted (B+ tree) paths* see, per key, each file's newest entry for that key (`recovery_mark_needed`). A Delete among them is needed if a Put from another file with a lower sequence is also among them; a range tombstone is needed if such a Put falls inside its range. A Put a file hides under its own newer entry needs nothing: that entry is a newer Put, or a Delete in the same file that compaction drops together with it. The streams path merges every file in each key range, so this is the whole picture for the key. The ranged path does it per worker in `recovery_build_sorted`, then again in its range merge between each worker's surviving Put and the tombstones pooled from all workers.

A lenient open (`fail_recovery_on_crc_errors = false`) that skips a data file it cannot read never sees that file's Puts, so it cannot show any tombstone unneeded: it builds an empty set with horizon 0, and every tombstone is kept until an open that reads every file.

Every entry has its own sequence, so a sequence names one tombstone exactly: the set needs no hashing and costs 8 bytes per needed tombstone, nothing for the droppable ones. `vacuum_compact_file` drops a tombstone when `seq <= horizon` and `seq` is not in the set.

**Why a decision made at open stays valid.** Every write, `ingest` and `resume()` after open carries a higher sequence, so no older Put can appear, and vacuum only removes entries — a droppable tombstone stays droppable. The reverse is not tracked: a needed tombstone may become droppable once vacuum compacts away the Put it hid, and that is noticed only at the next open. Tombstones above `horizon` — written since the open — were never examined and are always kept. An interrupted vacuum does not disturb the decision either: the file it would bring back was on disk at open, so its Puts were counted.

**Tombstones do not drive selection.** A file is compacted for its dead Puts (see **Fragmentation**); droppable tombstones go with it when it is, and stay on disk when it is not. A file made only of tombstones is never selected.

The original sequence number of every copied tombstone is preserved verbatim so recovery's sequence comparison still works correctly on the compacted file.

**Replication.** A dropped tombstone is no longer in the `changes_since` stream, the same way a dropped dead Put is not. A follower that already holds `Put(K)` and resumed from a sequence before the delete would never learn about it. Retention prevents that (next section).

#### Retention

Vacuum removes history, and a follower resuming `changes_since` below what it removed would get a stream with gaps: a batch missing its dead Puts, a delete it never sees (#168). Whether a follower still needs that history is a replication decision, not a storage one: only the replication service knows which followers count and where they are. So the engine takes the decision as an argument and keeps no state for it.

`VacuumOptions::retain_after` is a sequence. Vacuum drops nothing above it:

- A dead Put above it is copied into the compacted file instead of dropped. It is counted in `total_bytes` and not in `live_bytes`, so it stays reclaimable, and a later vacuum with a higher `retain_after` drops it. It has no key-directory mapping; recovery resolves it by sequence like any overwritten Put.
- A tombstone above it is kept even when recovery marked it droppable.
- A file is removed whole only when its `max_sequence` is at or below it; a file whose `min_sequence` is above it is not a candidate.
- Batch markers are always kept, as before.

`changes_since(snap, from)` is then complete for any `from >= retain_after` of every vacuum since `from`. The replication service passes the lowest `durable_sequence()` among the followers it counts on, on every node, since a follower can become leader, and counts a node it is bootstrapping from the manifest's `through_sequence` on. A follower it leaves out has to be re-bootstrapped. The default, `kNoRetention` (−1 as an unsigned sequence), restricts nothing, so a single-node database vacuums as before.

The alternative was an engine-side record of the highest sequence vacuum dropped, with `changes_since` refusing to resume below it. It detects the gap but decides availability on its own: a follower below the record has to re-bootstrap, and a node bootstrapped from copied files, which lack the record, has to assume the worst and push every follower behind it into a re-bootstrap at once. Retention avoids the gap instead of reporting it.

#### Space accounting

For each deleted key in the vacuumed file:
- **Reclaimed**: `kHeaderSize + key_size + value_size + kCrcSize` (the Put entry), plus the tombstone when it is droppable.
- **Residue**: `kHeaderSize + key_size + kCrcSize` (a needed tombstone, copied; `value_size = 0`).

For 1 KiB values a needed tombstone (`~19 + key_size` bytes) is negligible relative to the value reclaimed. The residue matters for delete-heavy workloads that write small values, which is what dropping unneeded tombstones is for.

#### Concurrency guarantee

Vacuum never holds `write_mu_` during file I/O on the file it compacts. `write_mu_` is held for the atomic commit step, which is in-memory (transient tree update + pointer swaps) except for one `fdatasync` of the active file when `sync = false` writes are not yet durable (see **What supersedes must be durable**). Writers wait for that sync as they would for a group commit's. Write-path latency is unaffected by the size of the file being vacuumed.

## Data File Format (.data)

### Entry Structure

```
+------------------+
| Leading Header   | 15 bytes
+------------------+
| Key Data         | key_size bytes
+------------------+
| Value Data       | value_size bytes (0 for Delete/BulkBegin/BulkEnd)
+------------------+
| CRC32            | 4 bytes (trailing)
+------------------+
```

### Leading Header (15 bytes)

| Offset | Size | Field      | Type   | Description                                    |
|--------|------|------------|--------|------------------------------------------------|
| 0      | 8    | Sequence   | u64 LE | Monotonic sequence number                      |
| 8      | 1    | EntryType  | u8     | Entry kind (see EntryType enum)                |
| 9      | 2    | Key Size   | u16 LE | Key length in bytes (0 for BulkBegin/BulkEnd)  |
| 11     | 4    | Value Size | u32 LE | Value length in bytes (0 for Delete/Bulk*)     |

### EntryType Enum

```cpp
enum class EntryType : uint8_t {
    Put       = 0x01, // Standard key-value pair
    Delete    = 0x02, // Tombstone — key present, value empty
    BulkBegin = 0x03, // Start of atomic batch — key and value empty
    BulkEnd   = 0x04, // End of atomic batch   — key and value empty
};
```

A zero byte in the `EntryType` field is unambiguous corruption or an uninitialized write (no valid type maps to 0).

### Trailing CRC (4 bytes)

| Offset from start of entry      | Size | Field | Type   | Description                     |
|---------------------------------|------|-------|--------|---------------------------------|
| 15 + key_size + value_size      | 4    | CRC32 | u32 LE | Checksum of all preceding bytes |

CRC is at the **end** of the entry so both write and read can be done in a single pass: write all fields and accumulate CRC in one loop, then append the checksum.

### Size constants

- `kHeaderSize = 15` — fixed leading fields (sequence + entry_type + key_size + value_size)
- `kCrcSize = 4` — trailing CRC
- Total entry size: `kHeaderSize + key_size + value_size + kCrcSize`

### Serialization

- Little-endian byte order throughout.
- Serialization uses an internal cursor-based API (`ByteWriter` / `ByteReader` in `serialization.cppm`). Both wrap the low-level `write_le` / `read_le` helpers with an auto-advancing offset so callers never compute byte positions by hand. `ByteWriter` optionally accepts a `Crc32*`; when non-null every `put()` / `put_bytes()` call also feeds the written bytes into the CRC accumulator, giving one-pass write + checksum with zero ceremony.
- CRC-32C uses the Castagnoli polynomial `0x1EDC6F41` via the [google/crc32c](https://github.com/google/crc32c) library, which auto-detects hardware acceleration at runtime (SSE4.2 on x86-64, CRC instructions on AArch64) and falls back to a software implementation when neither is available.
- CRC32 is computed over **all bytes of the entry except the trailing CRC field itself** (i.e., the leading header + key data + value data).

### Sequence Number

The sequence is a **globally monotonic** counter across all data files and all engine sessions — not a per-file counter. This is a correctness invariant: recovery determines which of two entries for the same key is fresher by comparing sequences from potentially different files. Any per-file counter reset would silently allow stale data to overwrite live data.

- The engine owns and increments the global sequence. `DataFile` is a passive consumer: the caller passes `sequence` to every `append` call.
- `DataFile` does not start its own counter; it does not know or care what value the sequence starts at.
- On startup, the engine scans all hint files and the active data file to find `max_seq`, then seeds the new active `DataFile` at `max_seq + 1`.

### Log-Structured Naming Convention

`make_data_file_stem()` builds the stem `data_{YYYYMMDDHHmmss}_{salt}_V01`: a UTC
timestamp at second precision, a 64-bit random salt in hex, and the file format
version. The full layout is in [`file_format.md`](file_format.md). The timestamp
is a debug aid — entry sequence numbers, not filenames, are the authoritative
ordering. File IDs are a separate monotonic `std::uint32_t` assigned at
rotation, not derived from the name.

**A stem is used once, and reuse is fatal.** Reusing one used to be silent:
`open(O_CREAT)` without `O_EXCL` reopened the sealed file and adopted its
length, so the "new" active file started non-empty, and every entry appended to
it was invisible to recovery, because `flush_hints_for` skips a file whose
`.hint` already exists.

Two independent things keep that from happening, and the split matters: the
salt makes a reuse negligible, and the guards make it fatal rather than silent
if one ever occurs.

The timestamp is only second-granular, so within one second the salt separates
every file alone, and the collision rate grows with the square of how many are
minted in that second. A small `Options::max_file_bytes` mints thousands: at
2,000 files a 32-bit salt repeats about once in 2,100 runs, which is frequent
enough to have surfaced on `main`. A 64-bit salt puts the same case at ~1e-13.
The salt is drawn per file from `std::random_device` rather than from a seeded
PRNG — seeding `mt19937` from one 32-bit draw would leave a process only 2^32
possible salt sequences however wide each output was, which is the property
that had to improve.

`createDataFileForWrite(dir, stem, suffix, capacity, io_backend)` is the only way
the engine creates a data file — `DB::open`, `rotate_active_file`, `resume()`,
and vacuum's staging copy all route through it. It refuses a stem that is
already in use on either half of the name:

- `<stem><suffix>` exists — rejected by `O_EXCL`, which is race-free against a
  concurrent creator.
- `<stem>.hint` exists — the stem is sealed, even if the data file itself was
  vacuumed away.

Vacuum's final placement goes through `renameDataFileExclusive`, which claims
the target name atomically (`renameat2(RENAME_NOREPLACE)`, falling back to
`link` + `unlink`) instead of replacing it. Checking the target and then calling
`std::filesystem::rename` is not equivalent, and was the first form of this
guard: vacuum stages its compacted copy holding only `vacuum_mu_`, so a
concurrent rotation can mint the same stem in the window between the check and
the rename — its own `O_EXCL` succeeds, because vacuum holds only
`<stem>.data.tmp` and has not written a `.hint` yet — and the rename then
replaces a live active file. A crash between `link` and `unlink` leaves a
`.data.tmp` behind, which `recovery_prepare_files` removes at open. If the
`unlink` fails, the function removes the name its `link` placed before it
throws. A throw then means nothing was placed, as when `renameat2` fails,
and vacuum's cleanup, which removes only the staged name, leaves no copy
behind. If that removal fails too, both names stay, and the next open
deletes the `.data.tmp` and the placed copy beside its source (#258).

All three checks `panic()` — they abort the process rather than throw. An
exception would be caught by the write path's `catch (...)`, turned into a
degraded state, and cleared by `resume()` minting a fresh stem: recovery from
the symptom, while the broken generator that caused it stays broken. Given the
salt width above, reaching any of these checks means exactly that. See *Fatal
invariants* in [`CONTRACT.md`](../CONTRACT.md).

### DataFile API

`DataFile` (in `bytecask.data_file` module) is the abstract base for all data file types. It defines the read interface (`read_raw`, `read_value`, `lend_record`, `size`). Concrete writable implementations extend `WritableDataFile`, which adds `append_entry`, `append_entries`, `sync`, `truncate`, and `shrink_to_fit` as pure virtual methods. One writable implementation exists per `IoBackend`, mirroring the read side (`ReadOnlyPosixDataFile`, `ReadOnlyMmapDataFile`, `ReadOnlyBufferPoolDataFile`):

- **`WritableMmapDataFile`**: Maps the file at the rotation threshold with `mmap(PROT_READ, MAP_SHARED)`; the file itself is zero-filled in chunks ahead of the write cursor (`WritableFileOps::ensure_zeroed`, see *Zero-fill ahead of the write cursor* below), so pages past the zeroed end are mapped but never touched. Reads within the mapped region are zero-syscall memcpy from the mmap region. Reads beyond the mmap region (rare: overflow past pre-allocated size) fall back to `pread`. Writes go through `pwritev` at a tracked `offset_`. MAP_SHARED is required so that `pwritev` writes through the fd are visible to mmap readers.
- **`WritablePosixDataFile`**: Pure `pread`-based reads, `pwritev` writes. Same chunked zero-fill as the mmap variant. This is the default, and the fallback for platforms without mmap support.
- **`WritableBufferPoolDataFile`**: `pwritev` writes that also insert the appended bytes into the buffer pool, and point reads served from it. See *Buffer pool* below.

**Write-path I/O policies.** The last two are the same class template, `WritablePosixFile<Io>`, instantiated on a policy that supplies the three things a back-end varies: what to do with bytes as they are appended (`publish`), how to answer a point read of the active file (`fetch_upto`: up to the requested length, short only at end of file), and whether the bytes are already in memory (`kResident`, which decides whether `read_value` and `lend_record` try the resident bytes before reading the file). A point read of the active file is bounded by the file's logical end (`WritableFileOps::offset_`), and `fetch_record`'s first read goes past the record it wants, up to that bound. That read may meet the end of the file below the bound — `resume()` cuts the file under readers that loaded the end before it was lowered — and only the record's own bytes have to be there; the read of the rest of a longer record is exact (#246); under the buffer pool those extra bytes are frame memory the appender wrote. `offset_` is therefore stored with release after the append's frame copy and loaded with acquire, so everything below a loaded end happens-before the read. It was relaxed, on the reasoning that a reader only asks for offsets a published state names; the over-read broke that, and the chaos soak (#92) reported the race. The writable `mmap` file serves a read from its mapping only when the whole record lies below both the mapping and the logical end: the mapping spans the zero-filled capacity, and bounded by it alone a read just below the end took zeros for the rest of a record instead of failing as every other back-end does.

```
WritablePosixDataFile      = WritablePosixFile<PreadIo>   // stateless; publish() compiles away
WritableBufferPoolDataFile = WritablePosixFile<PoolIo>    // holds BufferPool& + file_id
```

The choice is made once, when the factory maps the runtime `IoBackend` onto a type. Past that point nothing branches on which back-end is in use: the pool back-end's state lives in its own policy rather than as a nullable member of the shared `WritableFileOps`, so the pread and mmap back-ends neither carry it nor pay a test against it on the write and read paths. `WritableFileOps` holds a policy only for `publish`; `WritableMmapDataFile` serves its own reads from the mapping and uses the stateless one.

`createDataFileForWrite(dir, stem, suffix, capacity, io_backend)` selects the implementation and is how the engine creates every file it writes to: the file must not already exist, so it adds `O_EXCL` and panics on a reused stem (see *Log-Structured Naming Convention*). It opens with `O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC` — no `O_APPEND`, since pre-allocated files require positioned writes. Tests that reopen a file they wrote call a concrete type's `create(path, capacity)`, which adopts the file's current length. `renameDataFileExclusive(from, to)` completes the set: it moves a staged file onto its final name without replacing an existing target, and is how vacuum publishes a compacted file. `createDataFileForWrite`, like `openDataFileForRead`, *requires* a pool for `IoBackend::BufferPool` and throw `std::logic_error` without one — falling back to `pread` would make the configured bound quietly meaningless. Vacuum's staging copy is the one file that legitimately wants no pool, because its `file_id` is reserved only once the copy is complete and a pool-backed file keys its frames by one from construction; it says so by passing `stagingBackend(io_backend)`, which maps `BufferPool` to `Pread` and leaves the others alone. The staged file enters the pool when it is reopened for read under its final name.

A read-only file's size is taken once, by `fstat`, when it is opened, and bounds every read of it. If that `fstat` fails the open throws (`sealed_file_size`): a size of zero would seal a file whose records cannot be read and whose hint indexes nothing.

- **`append_entry(sequence, entry_type, key, value) -> Offset`**: Serializes a new entry with the given sequence number and `EntryType`, writes it via `pwritev()` at the tracked `offset_`, and returns the byte offset where the entry starts. `BulkBegin`/`BulkEnd` entries pass empty key and value spans. Does **not** guarantee durability on its own. The 15-byte header and 4-byte CRC are serialized into a fixed member buffer (`hdr_crc_buf_`); the key and value spans are passed directly as iovecs — no heap allocation and no copy of key/value data occurs on the write path.
- **`sync()`**: Calls `::fdatasync()` to flush all pending writes to physical storage. Must be called explicitly to guarantee crash-safety. Decoupled from `append_entry()` to enable Group Commit: callers can batch multiple `append_entry()` calls before a single `sync()`.
- **`shrink_to_fit()`**: `ftruncate` to `size()` plus `fdatasync`. Called once, when the file is sealed, to give the zero-filled tail back. Unlike `truncate()` it never touches a mapping, so it is safe while readers hold snapshots of the file — every published offset lies below `size()`.
- **`lend_record(offset, value_size_hint, verify, io_buf, lease)`**: The record at `offset`, its key and value sizes taken from its own header: spans into the mapping, into a pool frame pinned by `lease`, or into `io_buf` after one speculative `pread` (two for a record longer than the guess). Used by iterators and the blind key directory.
- **`read_raw(offset, dst) -> size_t`**: The sweep primitive. Copies the bytes at `offset` into `dst`, stopping at `size()` — `file_size` for a sealed file, the write cursor `offset_` for an active one — and returns how many it copied. It never goes through the buffer pool (see *Buffer pool*: a sweep must not flush the working set); under direct I/O the pool-backed file drops each chunk's page cache as soon as it is copied out.
- **`DataFileIterator`**: How every whole-file sweep reads entries — hint generation, vacuum, `create_manifest`, `resume()`, `changes_since`. It reads `DataFileIterator::kChunkBytes` (1 MiB) at a time through `read_raw` and frames entries out of that buffer, verifying each CRC; an entry that crosses the end of the buffer refills from that entry, and one larger than the buffer grows it to fit (bounded by `max_value_bytes`). The yielded entry reuses its key and value storage. A 64 MiB file costs about 64 reads rather than two `pread`s and three allocations per entry, which is what made hint generation syscall-bound — 13 s a file on virtiofs (#146); on local disk a 300k-entry file went from ~360 ms to ~40 ms. The sweep ends at a short header, a zero header (`sequence == 0`) or an entry whose computed end lies past `size()`. Because the CRC covers the key and the value, `key_size` and `value_size` cannot be validated before the bytes they describe are read, and a `value_size` is 32 bits wide: a single corrupt byte can name a 4 GiB entry. Bounding the entry's end against the readable extent, rather than sizing a read buffer from a number not yet verified, is what keeps that from turning into a 4 GiB allocation. An entry that ends past anything ever written is corrupt by inspection; treating it as the end of the file is what lets `resume()` truncate to the last good offset.
- **`read_value(offset, key_size, value_size, io_buf, out)`**: High-level read primitive. Reads the entry into `io_buf`, CRC-verifies it when `verify` is set, and copies only the value into `out`. Both `io_buf` (scratch) and `out` reuse existing capacity across calls. Used by `Bytecask::get()` and `EntryIterator`.
- Key and value are accepted as `std::span<const std::byte>` for binary safety.

### I/O Back-end Rationale

- **POSIX over `std::ofstream`**: `::pwritev()` issues a single syscall per entry using scatter-gather I/O at a specified offset, skipping the buffering layers and locale state overhead of C++ streams.
- **`fdatasync` over `fflush`/`flush()`**: `fdatasync` syncs data to physical media while skipping inode metadata updates (access time etc.), making it faster than `fsync` for a pure append-only log.
- **Group Commit pattern**: Separating `append_entry()` (writes to page cache) from `sync()` (forces to disk) lets future code batch hundreds of writes before a single expensive `fdatasync`, which is the primary lever for high write throughput on NVMe hardware (see `io_uring` paper reference).
- **Zero-copy write path**: `append_entry()` builds only the 15-byte header and 4-byte CRC in a fixed member buffer, then calls `::pwritev()` with four iovecs — `[header(15), key, value, crc(4)]`. The kernel gathers the scattered buffers into one atomic write without any intermediate heap allocation or memcpy of key/value data. For 1 KiB values this eliminates ~250 MB/s of unnecessary copying at 244k puts/s.
- **Zero-fill ahead of the write cursor**: before an append, `WritableFileOps::ensure_zeroed` writes zeros from the current physical end up to the next `kZeroFillChunkBytes` (4 MiB) boundary, never past `max_file_bytes`; a fresh file gets its first chunk at creation. Allocation alone is not enough: `fallocate` reserves *unwritten* extents, and the first write into each block converts one — a journaled metadata change that the next `fdatasync` must wait for. On ext4 (`data=ordered`) that turns a commit into data write → journal write → journal commit → flush, roughly 2× the cost of a data-only flush; measured on an Azure SSD, 2.9 ms per `fdatasync` versus 1.6 ms once the extents are written, and MariaDB sysbench `oltp_write_only` went from 1.3k to 2.3k tps. Writing zeros converts a whole chunk at once: the zeros are not synced separately, so the commit that crosses into a new chunk flushes it (4 MiB of writes and one journal commit, ~30 ms on that disk), and every other commit in the chunk is a pure data flush — the profile of a preallocated redo log. Chunks rather than the whole file up front because a full 64 MiB fill made creating an empty database a 64 MiB write and every rotation a ~0.5 s stall on the group-commit leader; sixteen ~30 ms bumps per file is the better latency shape, and it is one integer (`zeroed_end_`) and one `if` on the write path, with nothing in the background. Emscripten builds zero-fill too: every WASM target runs under `NODERAWFS`, so data files are host files with the same journal behavior. A capacity of 0 turns the fill off — tests and tooling that reopen files they wrote expect exact sizes.
- **Sealed files are exact-size**: the zero tail is not a format concern — every sweep stops at a zero header (`sequence == 0`), which no real entry can carry — but `file_size` seeds `total_bytes` at recovery, and a zero tail on a half-full file would make vacuum see it as partly garbage. So the tail is given back whenever a file is sealed: `rotate_active_file` and vacuum's staging copy call `shrink_to_fit()` before the file is reopened read-only, `DB::close()` calls it on the active file, and `recovery_prepare_files` truncates a hint-less data file to the committed end that `flush_hints_for` returns from its scan, once *Recovering a Hint-less File* has ruled that what lies past it may go. Files sealed by earlier builds in mmap mode keep their 64 MiB `fallocate` size and an existing hint; recovery seeds an inflated `total_bytes` for them once, and vacuum rewrites them early.

### Source Code Module Architecture

We use fine-grained C++20 modules:
- `bytecask.util`: CRC-32C accumulator (`Crc32`, backed by google/crc32c) and checked `narrow<To>(From)` conversion.
- `bytecask.serialization`: Core serialization primitives (`ByteWriter`, `ByteReader`, `read_le`, `write_le`) and re-exports `bytecask.util`.
- `bytecask.data_entry`: Logical entry definition, `write_header_and_crc()` (fills a fixed 19-byte buffer with LE header + CRC for zero-copy I/O), `serialize_entry()` (complete in-memory entry for tests/recovery), `parse_header_and_verify()` / `deserialize_entry()` — CRC verification is factored into `parse_header_and_verify()`, which the data file's read paths share.
- `bytecask.data_file`: Disk I/O — `DataFile` (abstract base), `WritableDataFile` (pure interface), `WritablePosixFile<Io>` with the `PreadIo` / `PoolIo` policies (aliased `WritablePosixDataFile` / `WritableBufferPoolDataFile`), `WritableMmapDataFile`, `ReadOnlyMmapDataFile`, `ReadOnlyPosixDataFile`, `ReadOnlyBufferPoolDataFile`, `Offset`.
- `bytecask.hint_entry`: `HintEntry`, `serialize_entry()`, `deserialize_entry()` — symmetric read/write for hint entries.
- `bytecask.hint_file`: Hint file writer and reader (`HintFile`).
- `bytecask.persistent_ordered_map`: Immutable sorted map (`PersistentOrderedMap<K,V>`, `OrderedMapTransient<K,V>`) backed by `immer::flex_vector`; retained for benchmarking.
- `bytecask.btree`: Persistent B+ tree (`PersistentBTree<V>`, `TransientBTree<V>`, `BTreeIterator<V>`) with one slotted node layout, suffix-truncated separators and version-based node reclamation; the key directory.
- `bytecask.version_chain`: `VersionChain<Traits>` — per-version node reclamation shared by both trees.
- `bytecask.engine`: Public engine API (`Bytecask`, `EngineState`, `KeyIterator`, `EntryIterator`, `FileRegistry`, type aliases).

### Current scope boundaries

- `EntryType` is written and read back on `DataFile::read()`; atomic bulk semantics are enforced at a layer above `DataFile`.
- No file rotation or size limits.
- No read path for the key directory — append-only for now.

### DataFile fd mode after rotation

Writable data files open with `O_RDWR | O_CREAT | O_CLOEXEC`. After a data file is rotated it is logically immutable — no new entries should be appended. The engine enforces this at a higher level; the fd mode is not downgraded to `O_RDONLY` after rotation. Rationale:

- `DataFile` is an internal class; the engine exclusively controls when `append()` is called.
- Re-opening the fd purely for semantic enforcement adds syscall overhead and complexity without improving correctness for the production path.
- A `sealed_` flag with `assert(!sealed_)` at the top of `append()` is sufficient: it catches programming errors in debug builds at zero production cost.
- The one way a sealed file could be reopened for write — a reused filename — is now refused outright by `createDataFileForWrite`, in release builds as well as debug.

Contrast with `HintFile`, which uses `OpenForWrite` / `OpenForRead` factory functions. That split models externally visible, non-overlapping lifecycles at different call sites: one site writes during `flush_hints()`, a completely separate site reads during recovery. Encoding that distinction in the type prevents mixing them up. `DataFile` has no equivalent external semantic split.

### DataFile mmap

Two mmap strategies are used depending on the file's lifecycle. What a
reader is owed across each of them — and across every other event that
moves a file under a live view — is stated in *View and span lifetimes*
in [`CONTRACT.md`](../CONTRACT.md); this section covers the mechanism.

**Buffer pool — `ReadOnlyBufferPoolDataFile` / `WritableBufferPoolDataFile`**: When `Options::io_backend` is `IoBackend::BufferPool`, sealed files are served from a bounded frame cache owned by the DB (`BufferPool`, `bytecaskdb/buffer_pool.cppm`), and the writable active file inserts every byte it appends into the same pool so it is resident without a read. The cache is an arena of 4 KiB frames, one open-addressed index of 16-byte slots — key `(file_id, frame_index)`, frame number with the visited bit, seqlock version — a pin count per frame, and a list of resident frames in admission order that SIEVE eviction walks; a hit only sets the visited bit, and a frame filled by a read miss is admitted unvisited, so a frame read once is evicted on the hand's first pass. A hit is one lock-free probe, a pin on the frame, a version re-check that proves the slot still maps the key to that frame, and a plain `memcpy`: a frame is immutable while any pin is held, because eviction claims a frame by moving its pin count from zero to a dead marker with one CAS and skips any frame it cannot claim. Iterators are lent spans straight into the frame under a `FrameLease` held until they advance (`DataFile::lend_record`), so a scan over the pool copies nothing for an entry that sits inside one frame. Every change to the index or a frame happens under one mutex, released across device reads, with each slot change bracketed by its version going odd then even; deletion is a backward shift, not a tombstone. Vacuum and rotation reserve the new file's `file_id` before opening it (`TransientEngineState::reserve_file_id`) because a file keys its frames by it from construction. While the pool has free frames, a miss reads the file-aligned 128 KiB block around it (`kPoolFillBlockBytes`) and admits every whole frame in it, so a cold pool warms at readahead speed rather than one 4 KiB read per miss; a full pool fills only the frames a miss needs, since a block's neighbours would evict frames that earned their place. Sealed frames are filled through a second descriptor whose reads bypass the page cache, so the pool — not the kernel — holds sealed-file data; a filesystem that refuses falls back to buffered fills for that file, counted in `pool_direct_io_fallbacks`. `BufferPool::open_uncached` is the one place that names a platform mechanism for this (`O_DIRECT` on Linux, `F_NOCACHE` on macOS, neither elsewhere) and the only platform `#if` left in the engine; it proves the descriptor with one aligned read before returning it, because some filesystems accept the flag at open and fail at the first read. The engine and the tests both ask it, so they cannot disagree about a mount. The active file is never opened this way — it is written through the page cache and read back from the pool frames the writer filled. Scans bypass the pool and oversize reads (over capacity/8) are never admitted. When vacuum deletes a file it frees that file's frames (`BufferPool::release_file`, claimed as eviction claims a victim, a held frame left to the hand), rather than leaving them to the hand's next pass. `get` copies into the caller's buffer — assigned straight from the lent frame when the value lies in one resident frame; a span into a frame is handed out only under a lease. `Options::buffer_pool.capacity_bytes` is the total footprint — frames and index both come out of it — and `DB::open` rejects a pool smaller than `2 x max_file_bytes`, since the pinned active file lives inside the budget. See [`buffer_pool_design.md`](buffer_pool_design.md).

**Active (writable) file — `WritableMmapDataFile`**: When `Options::io_backend` is `IoBackend::Mmap`, the active file is mapped at the rotation threshold with `mmap(PROT_READ, MAP_SHARED)` and zero-filled in chunks ahead of the write cursor (see *Zero-fill ahead of the write cursor*). MAP_SHARED is required so that `pwritev` writes through the fd update the same pages that mmap readers see. Reads within the mapped region are zero-syscall memcpy; reads beyond (rare: file grew past pre-allocated size) fall back to `pread`.

The mapping is established once, in the constructor, and released once, in the destructor: its address is stable for the object's whole life. This is a requirement, not an implementation detail. `truncate()` is called by `resume()`, which excludes writers but not readers — reads stay lock-free and remain available while the engine is degraded — and an `EntryIterator` hands out spans into the mapping that stay valid until the next `operator++()`, i.e. across the body of the caller's range-for. Unmapping and remapping under a live reader would leave that span addressing memory the process no longer owns (SIGSEGV), or, once the address is reused by the next mapping, silently reading unrelated bytes.

So `truncate()` calls `ftruncate` and moves one field: `mmap_end_`, the prefix of the mapping still backed by the file. It moves it before the `ftruncate`, with the logical end, so an `ftruncate` that cuts and then fails cannot leave pages past the new end of file reachable through the mapping. Every published `KeyDirEntry` points below the truncation point by construction, so no reader loses access; anything at or past it takes the existing `pread` fallback, which fails as a clean short read instead of faulting on a mapped page beyond EOF. `mmap_end_` is `atomic<size_t>` (release on write, acquire on read) because the lock-free read path reads it while `truncate()` lowers it. `shrink_to_fit()` lowers it the same way when the file is sealed, so every path that shrinks the file lowers the bound with it — `truncate` is no longer the odd one out. The bound is not a claim about the file's length in general: a fresh mapping spans the whole capacity while the file is only zero-filled a chunk ahead of the write cursor, which is the existing steady state. What it guarantees is narrower and is the part that matters here — nothing that shrinks the file leaves the bound above the new end.

**Sealed (read-only) files**: When `seal()` is called, the file is memory-mapped with `mmap(PROT_READ, MAP_PRIVATE)` and `MADV_RANDOM`. Subsequent `lend_record()` and `read_value()` serve directly from the mapped region, eliminating `pread` syscalls on the hot read path. If `mmap` fails (e.g. address space exhaustion), the class silently falls back to `pread`. The mapping is released by `munmap` in the destructor.

A read through a mapping has no way to return an error. When the kernel cannot fill a mapped page — a media error, `EIO` from a network or FUSE filesystem, a file truncated underneath — it delivers `SIGBUS`, which kills the host process unless the host handles it. Every other read in the engine throws `std::system_error`. That is a property of `IoBackend::Mmap`, taken on with it; the `Pread` and `BufferPool` back-ends, and hint files under every back-end, read with `pread`.

On WASM/Emscripten builds, mmap is disabled (`#ifndef __EMSCRIPTEN__`). Emscripten's mmap emulation allocates a heap buffer and copies the file contents into it — functionally identical to `pread` but with doubled memory consumption. WASM builds use the `pread` fallback exclusively; tests that request mmap assert that fallback rather than treating it as a failed fast path. `DB::open` rejects `IoBackend::Mmap` outright on Emscripten builds (throws `std::invalid_argument`), rather than silently ignoring the option — the sealed-file read path and the active-file write path never take the mmap branch on this platform, so an accepted-but-ignored option would be a silent behavior mismatch with native. `IoBackend::BufferPool` is accepted: under `NODERAWFS` each `pread` is a call into Node's `fs`, and a pool hit avoids it. The pool fills without `O_DIRECT` there (`open_uncached` returns -1 on Emscripten), so `direct_io` has no effect. Emscripten stubs out `link()`, which the exclusive rename that installs a vacuumed file depends on; `bytecaskdb-node/wasm/node_linkat.c` replaces the stub with a hard link through Node's `fs`, keeping `EEXIST` so a live file is never replaced. Correctness proof tests generated with `io_backend = IoBackend::Mmap` (in `tests/proof/generated/`) are compiled out under `#ifndef __EMSCRIPTEN__` for the same reason.

### HintFile I/O model

Write and read modes have different I/O strategies.

- **`OpenForWrite(path)`** — opens the file and writes its 16-byte header. Each `append()` serializes one entry into the open frame; once the frame holds `kHintFrameBytes` (16 KiB) it is compressed with zstd in one call and written to the fd, updating a running CRC-32C accumulator. `close()` writes the last frame and the inverted CRC trailer, calls `fdatasync()`, and closes the fd. If the HintFile is destroyed without calling `close()` (e.g. exception path), the fd is closed without writing the CRC — the `.hint.tmp` file is cleaned up on next startup.
- **`OpenForRead(path)`** — opens the file and reads all of it once, front to back, in 256 KiB chunks with `pread`: each chunk feeds the CRC, and the pass records where each *unit* — a zstd frame — starts. It checks the header, every frame's header and decompressed size, and at the end the trailer. Only then does it return, and it closes the fd before it does, keeping the path and where the units are. Scanners read one unit at a time: each read opens the file, `pread`s the unit and closes it again. Scanners share the verified file's map with the `HintFile`, so a scanner can outlive it.

Hint files are never memory-mapped. Recovery used to map them for its merges, and a page the kernel could not fill was a `SIGBUS` that killed the process (#237). With `pread` a failed read is a `std::system_error`, like every other read.

The reader holds, per open file, the path and one 8-byte offset per unit (one per ~16 KiB of entries before compression), and per scanner one decoded unit. The compressed bytes of the frame being decoded go into a per-thread scratch buffer, shared by the thread's scanners. The open pass holds one chunk while it runs. Nothing holds a whole file.

The pass reads every byte before a scan reads any, so a scan reads a file a second time. That is the pattern the mapping had — the CRC pass, then the scan — and on a cold start the first pass brings the file into the page cache for the second.

A merge holds every hint file at once — the keyed tree's cursor merge, and each range of the blind tree's stream merge — so a descriptor per hint would double what an open needs beside the data files, and a database with more files than half of `RLIMIT_NOFILE` would not open (#251). Opening per read bounds it: during recovery the process holds a descriptor per data file, plus one per recovery thread for the hint unit it is reading. The price is an `open` and a `close` per unit, one per ~16 KiB of entries; on a cold start the open pass has already brought the file into the page cache, so neither waits on the disk. Nothing in the engine replaces a hint under a scanner — recovery holds the directory lock, and a rebuild happens before the file's scanners exist — so a scan reads the file it verified.

The reader reads only the zstd-framed layout. A hint in the uncompressed layout written before compression fails the magic check and is rebuilt from its data file, like any damaged hint (see [Hint File Format](#hint-file-format-hint)).

`HintEntry.key` is a `std::span<const std::byte>` with no allocation per entry. In a framed file it points into the frame the scanner has decoded, so it is valid only until the scanner's next `next()` or `seek()`. A reader that keeps an entry longer copies it into a `HintRecord`, which owns its key and reuses its capacity: the merge cursors of `recovery_build_sorted` and `recovery_load_streams` hold their current entry and lookahead that way, and a blind fence owns its key. Testing builds give every decoded frame a fresh allocation, so a reader that breaks the rule reads freed memory, and ASan reports it instead of the reader silently seeing the next frame's bytes.

## Hint File Format (.hint)

### Purpose

Hint files are compact companion files to sealed (rotated) data files. Each hint entry summarises one data file entry — just enough metadata and the full key — so that the in-memory Key Directory can be rebuilt at startup by scanning the smaller hint files instead of the raw data files. Only sealed data files have a corresponding hint file; the active data file is recovered by scanning its raw bytes if needed.

### File Structure

A hint file is a 16-byte header, the entries in zstd frames, and a 4-byte trailer:

```
+------------------+
| Header           | 16 bytes: magic "BCHINTZ" 0x81, version, codec
+------------------+
| zstd frame       | ~16 KiB of entries before compression
+------------------+
     ...repeated...
+------------------+
| ~CRC32C          | 4 bytes (file trailer)
+------------------+
```

A frame holds whole entries and records its decompressed size, so a reader walks frames one after another without an index. Decompressed back to back, the frames are the entries below. zstd level 1: on the recovery benchmark's keys, level 3 compressed no better. Compressed, a hint file is 1.6–9× smaller than the entries it holds, depending on how much the keys have in common, and a cold start reads that many fewer bytes; see [`hint_compression_design.md`](hint_compression_design.md) for the measurements. The byte-level layout is in [`file_format.md`](file_format.md).

Files written before compression hold the entries back to back and a plain CRC-32C, and are still read. Their first 8 bytes are the first entry's sequence, below 2^63; the magic sets the top bit, so the layouts cannot be confused. The framed trailer is inverted so that a reader that knows only the old layout fails its CRC and rebuilds the hint from the data file instead of parsing frames as entries: downgrading costs one hint rebuild per file.

`flush_hints_for()` writes the `BulkBegin`/`BulkEnd` markers (keyless) and the range tombstones first in data-file append order, then the Put and Delete entries sorted by key. See [Sorted Hint Files](#sorted-hint-files).

Each entry is a 23-byte header followed by the key:

### Hint Header (23 bytes)

| Offset | Size | Field        | Type   | Description                                    |
|--------|------|--------------|--------|------------------------------------------------|
| 0      | 8    | Sequence     | u64 LE | Entry sequence number                          |
| 8      | 1    | EntryType    | u8     | Entry kind: `Put` (0x01), `Delete` (0x02), or `RangeDel` (0x05) |
| 9      | 8    | File Offset  | u64 LE | Byte offset of the entry in the data file      |
| 17     | 4    | Value Size   | u32 LE | Value length in bytes                          |
| 21     | 2    | Key Len      | u16 LE | Length of the key bytes that follow             |

`Value Size` is stored so the reader can compute the full on-disk entry size in the data file without reading it.

### File Trailer (4 bytes)

| Offset from file start | Size | Field  | Type   | Description                                       |
|------------------------|------|--------|--------|---------------------------------------------------|
| end - 4                | 4    | CRC32  | u32 LE | Bitwise NOT of the CRC-32C (Castagnoli) over all preceding bytes, header and frames |

The file CRC is the only integrity check. zstd detects little on its own — a flipped bit in a frame of real hint entries decoded to wrong bytes without error 83–93% of the time — and its per-frame checksum is not enabled: it would catch the same flips, but not a missing frame, and only partway through decoding.

`OpenForRead` verifies the trailer CRC before it returns, and so before any entry is parsed, rejecting the whole file with `std::runtime_error` on a mismatch or a frame header that does not hold, and with `std::system_error` when a read fails. Verifying at open is what makes the file's replacement safe: the throw lands before any entry has been applied to the key directory, so the rebuild below has no partial state to undo.

A hint file is a derived index, not the records it points at, so a CRC failure in one says the index is damaged and not that the data file behind it is — and a read that fails on it says nothing about the data file either. `open_hint_or_rebuild` therefore treats a failure that says the hint's bytes are bad the same way whichever it is: it removes the hint, regenerates it from its data file through `flush_hints_for` — the same scan a missing hint already takes — and opens the result, in both recovery modes. Bad bytes are a check the file fails (`std::runtime_error`: CRC, header, frame, entry) or a read the device fails (`std::system_error` with `EIO`, which a file that ends early is reported as). Any other error — `EMFILE`, `ENFILE`, `ENOMEM`, `EACCES`, `std::bad_alloc` — says nothing about the hint, and a rebuild on it would remove a good hint and then fail to write it back; it is thrown with the hint untouched. `fail_recovery_on_crc_errors` governs only what is left after that, through `open_hint_or_skip`: a data file whose own entries fail their CRCs cannot be rescanned, and the file is then thrown on (strict) or skipped (lenient). The same distinction applies there: an error that says nothing about the file's bytes is thrown in both modes, since a skip would cost the file's keys. Skipping is not free — a skipped file keeps its `total_bytes` while contributing no `live_bytes`, so the next vacuum sees it as entirely garbage and unlinks it, which is why a rebuildable hint must never reach that path.

A read can also fail after the open pass succeeded: a scan reads the file again, and the second read can fail where the first did not. By then some of the file's entries have been applied, so neither a rebuild nor a skip is safe, and in both modes `DB::open` throws the `std::system_error`. Nothing has been written by then; the next open recovers. The lenient skip covers only a file that could not be opened, in every recovery path. The skip happens inside the worker that opens the file (`open_hint_or_skip`), so an error that escapes a recovery worker is one no mode answers, and every recovery path rethrows it whatever the mode.

### Size Constants

- `kHintHeaderSize = 23` — fixed header fields (no per-entry CRC)
- Total entry size: `kHintHeaderSize + key_len` (variable), before compression
- `kHintFrameBytes = 16 KiB` — entry bytes after which a frame is closed
- File overhead: 16-byte header, 4-byte file CRC trailer

### Scanner API

`HintFile::make_scanner()` returns a `Scanner` object that iterates over entries sequentially, reading and decoding one unit at a time into a buffer it owns; each thread shares one zstd decompression context, and one buffer for compressed bytes, among all its scanners. A failed read throws `std::system_error`. `HintEntry.key` is valid until the scanner's next `next()` or `seek()`.

```cpp
auto scanner = hint.make_scanner();
while (auto he = scanner.next()) { /* use he->key, he->sequence, … */ }
```

`position()` names where the next entry starts — the index of its unit and its offset in the decoded unit — and `seek()` returns there. Positions order like the entries they name. The blind recovery path records its fences as positions.

### Recovery

On engine startup:

1. Discard any `.hint.tmp` files — incomplete hint files from a crash mid-rotation.
2. Open all `.data` files and seal them. For any data file without a companion `.hint` — and, lazily, for any whose `.hint` will not open — generate one via `flush_hints_for()` (uses `CommittedEntryIterator`: buffers entries between BulkBegin/BulkEnd, discards incomplete batches, logs a warning). `BulkBegin`/`BulkEnd` markers are written to hint files with their sequence numbers (for accurate `next_seq` computation). Other hint entries are sorted by key (for prefix compression). Recovery's sequence-aware upsert handles duplicate keys across entries.
3. Recover exclusively from hint files. For each hint entry:
   - `Put`: insert `(key → {sequence, file_id, file_offset, value_size})` only if `entry.sequence > dir[key].sequence` (skip if a fresher entry is already present).
   - `Delete`: remove the key from the tree if `entry.sequence > dir[key].sequence`; otherwise skip.
4. Record `max_seq` — the largest sequence number seen across all hint entries.
5. Build the needed-tombstone set as a side effect of step 3: the sequences of the tombstones seen beating a Put from another file, and `max_seq` as the set's horizon (see *Vacuum → Tombstone handling*). Count each file's tombstone and marker bytes into its `FileStats` as the entries go by.
6. Create a new active data file seeded at `max_seq + 1`.

This is a single code path: `flush_hints_for()` is the same function used by rotation and background hint writes. No raw-scan recovery logic exists — recovery always goes through hints.

### Recovering a Hint-less File

Open scans every data file that has no hint: after a crash, the file that was active and any sealed file still waiting for its hint; after a close whose final sync failed, or that found the engine degraded, the active file. A clean close writes the active file's hint. Before anything reads a hint-less file, `rewrite_durably` makes the bytes a read returns durable: the active file's last bytes may be in the page cache and not on the device, and a hint built from them would outlive them at a power loss (*A failed `fdatasync` (fsyncgate)*). No file says which one was active, so every hint-less file is rewritten; the cost is bounded by `max_hint_backlog + 1` files of at most `max_file_bytes`. `engine_bench`'s `ByteCaskDB/OpenHintless` measures it: a cold open of a database whose newest file, 60 MB, has had its hint removed as a crash leaves it. On a SATA SSD it went from 527 ms to 835 ms with the rewrite. The scan runs with `OnDamage::Stop`: it ends at the first record that does not parse, whatever the reason — a zeroed header, a record running past the end of the file, or a CRC mismatch. Before the hint is renamed into place, `recovery_check_tail` rules on everything past the end of the last committed record (#138):

| Past the last committed record | File | Outcome |
|---|---|---|
| zeros only | any | trimmed, and the new length synced: the preallocated tail of the active file. Sealing truncates it and `fdatasync`s the truncate, so a sealed file does not normally carry one; one that does (sealed by an earlier build, say) is trimmed the same way |
| anything else | no other file starts at a higher sequence | truncated: a torn tail |
| anything else | another file starts at a higher sequence | refused in every mode; the file is left as found and no hint is written |
| anything else | another hint-less file also holds something other than zeros past its last committed record | refused in every mode, before either file is cut: a crash tears one file |

The rule rests on one invariant: only the file written last can hold a record a crash tore, because rotation, vacuum's copy and `resume()` all `fdatasync` a file before sealing it. "Written last" is read off sequences, which no two files share, so each file's first header orders them. A file whose first header is zero has no sequence to compare, and goes whole: it may be the newest file, never synced, whose first page a power cut lost while a later page reached the disk, and that open must succeed. Nothing outside the data files records how much of a file was synced, so in the newest file damage in synced data is indistinguishable from a torn tail and is truncated with it — the choice PostgreSQL makes for its WAL and RocksDB for its default recovery mode. `resume()` does not have that ambiguity: it knows the published extent and refuses below it.

What that leaves unrefused, besides damage in the newest file:
- damage in a sealed hint-less file that zeroes it from a record boundary to its end, which reads as a leftover preallocated tail;
- damage in a just-sealed file when a crash follows the rotation before the new file's first write, since the empty new file has no sequence and the sealed one reads as newest;
- damage to the first header of a sealed file that leaves a sequence higher than any other file's;
- damage that zeroes the sequence in the first header of a sealed hint-less file, which reads as a newest file whose first page was lost.

None of these is a state a crash produces: each needs storage to lose bytes it reported synced. The engine relies on storage to keep that promise, and refuses a database that shows it broken rather than open what is left of it. These are the shapes it cannot refuse, because each leaves exactly what a crash would. The last one is refused when the newest file is torn as well, since a crash tears one file; with a clean newest file it is not visible (#303).

Before this, a torn record that failed its CRC refused the open in both modes, so an ordinary power cut during a write could leave the database unopenable, while a zeroed header or an oversized `value_size` in any hint-less file was trimmed silently.

### Parallel Recovery

`DB::open` takes `recovery_threads` (default 4). One code path serves every thread count: with one worker (`recovery_threads == 1`, or one data file) the same algorithm runs on the calling thread without spawning one, so an open with one recovery thread keeps the caller's thread-local state, which the fault-injection tests rely on.

1. **Phase 1 (serial)**: open the data files and generate any missing hint file.
2. **Phase 2 (parallel)**: rebuild the key directory from the sorted hint files, partitioned by key range. The blind tree recovers through `recovery_load_streams` (see *The blind-leaf key directory*); the keyed B+ tree through `recovery_load_ranged` (below). Neither has a fan-in: each worker owns one key range.
3. **Phase 3 (serial assembly)**: concatenate the ranges and set `next_seq` past the highest sequence seen.

Recovery used to partition by *file* and merge the per-worker trees in a fan-in (`recovery_load_parallel`), which suited the radix tree it was built for; `docs/parallel_recovery_design.md` records that design. [`hint_compression_design.md`](hint_compression_design.md) has the current cold-start measurements (10M keys at 16 threads: 0.33 s).

### Range-Partitioned Recovery (B+ tree)

Splitting the work by *file* pays for it at the fan-in: every worker's tree must be merged into one. That merge was cheap on the radix tree, which could adopt a disjoint subtree by pointer, and is not cheap on a B+ tree, which has to move every key. `recovery_load_ranged`, the keyed B+ tree's recovery, replaces the fold with a partition; it is coupled to the tree it builds, not a tree-agnostic engine phase.

1. **Sample** separators from each file's hint run, and merge the samples to pick `W - 1` boundaries that cut the key space into W roughly equal ranges.
2. **Build** each range in parallel. A worker k-way merges the slice of every hint run that falls in its range straight into a `BulkLoader`, which packs full leaves in key order — no descent, no split, no slot shift per key.
3. **Concat**. The runs are disjoint and already in order, so assembly is a spine built over them, not a merge.

There is no fan-in: worker k's keys belong to worker k and to nobody else. Step 2 is the reason hint files are written sorted, and `recovery_build_sorted` throws if it is handed an unsorted one.

Two invariants matter in step 3. The merge collapses duplicate keys within a run to the highest sequence, because hint files are no longer deduplicated and one key can repeat inside a run. And `concat` publishes the assembled tree under the maximum of the session tag and every run's tag: the version chain frees a dead version by tag interval, so a node tagged above the published version would later be taken for a *different* version's garbage and freed while still reachable.

Cursors are held in a heap rather than scanned linearly. A worker in the ranged path has one cursor per hint file — not one per recovery thread — so at 10M keys across many files the linear scan dominated: 8.37s against 2.44s for the heap at one thread.

### Sorted Hint Files

`flush_hints_for` writes each hint file's Put and Delete entries sorted by key ascending, sequence descending. Range tombstones and `BulkBegin`/`BulkEnd` markers are written first, in scan order: a range tombstone's key is a range *bound*, not a key of the file, so it must not join the sorted run, and recovery's tombstone handling is order independent anyway.

Sorting costs one buffer per data file — bounded by `max_file_bytes`, not by the database — and is what lets recovery bulk-load a B+ tree instead of inserting key by key. Order is not part of the contract: every recovery path resolves entries by sequence, never by position, so a sorted hint file and an unsorted one describe the same key directory and either reader accepts either file. The one asymmetry runs the other way — `recovery_build_sorted` *requires* sorted input and throws on anything else, so it is reachable only from `recovery_load_ranged`.

Unlike BC-088, the sorted writer does **not** deduplicate. `file_stats.min_sequence` / `max_sequence` are rebuilt at recovery from the entries the hint file still holds, and `ChangeIterator` selects data files by that range; dropping an entry can raise `min_sequence` above a sequence the data file really contains, and replication would then skip the file.

### Incomplete Batch Recovery

If the engine crashes after writing a `BulkBegin` but before the matching `BulkEnd`, the data file contains an incomplete batch. `flush_hints_for()` detects this (BulkBegin with no matching BulkEnd), discards the buffered entries, and logs a warning. No partial-batch entries appear in the generated hint file, so they are never inserted into the key directory.

### Relationship to Data Files

| Property | Data file | Hint file |
|---|---|---|
| Extension | `.data` | `.hint` |
| Contents | Full key + value | Key + location metadata only |
| Created | At engine open / rotation | When a data file is sealed (rotated), or on startup for files missing hints |
| Read at startup | Never directly — hints are generated first if absent | For all sealed files |

One hint file corresponds to exactly one data file (same timestamp stem, different extension).

**Hint files are a correctness-carrying artifact for recovery.** On startup, any data file without a companion `.hint` has one generated via `flush_hints_for()` (using `CommittedEntryIterator`). Recovery then reads only hint files — there is no separate raw-scan fallback path.

Hint files are written **deferred**: never inline on the write path. During normal operation, `rotate_active_file()` dispatches hint writes to the `BackgroundWorker`. At engine close, `DB::close()` seals the active file and calls `flush_hints()`, ensuring every data file has a companion `.hint` after a clean shutdown. This keeps write-path latency flat and bounded. The cost is that a crash before shutdown causes recovery to generate missing hint files on startup, which is always correct and only slower. `close()` also writes the active file's hint once its final `fdatasync` and `shrink_to_fit()` have succeeded, unless the engine is degraded or the file is empty, so a clean restart finds no hint-less file to rewrite. A degraded close skips it: a sync that failed earlier left pages clean that no later sync writes (see *`DB::close()`*).

**The backlog is bounded (#146).** Deferring the work moves it, it does not delete it: whatever the worker has not written when the engine closes, `close()` writes synchronously, and whatever a crash leaves unwritten, the next `open` rebuilds serially before it can serve. Under sustained writes a worker slower than the rotation rate grows the queue without limit, so close and open had no time bound — a supervisor with a stop timeout kills the close, the next open inherits the backlog, and a startup probe kills that. `Options::max_hint_backlog` (default 4) caps it: every path that seals a file (`rotate_active_file`, `resume()`) first calls `wait_for_hint_backlog()`, which blocks while the worker has that many tasks queued or running. The wait runs on the writer with the write path held, so writes stall and reads do not. It happens before the file is sealed, so the backlog never exceeds the limit and a kill at any point leaves at most `max_hint_backlog + 1` data files without a hint (the backlog plus the active file) — the bound on the next open, just as `max_hint_backlog` bounds what close has to write. It cannot deadlock: a hint task takes no lock a writer holds (`flush_hints_for` is static and reads outside the buffer pool). `max_hint_backlog = 0` turns it off, restoring the unbounded behaviour for bulk loads that prefer throughput to restart time. `stats()` reports the backlog (`hint_backlog`) and the stalls (`hint_backpressure_stalls`, `hint_backpressure_stall_us`). With the chunked sweep, a stall only happens when the disk cannot read back what was just written.

### Hint File Atomicity

To guarantee hint files are either complete or absent, writing uses a temp-then-rename protocol:

1. Write the complete hint file to `data_{timestamp}.hint.tmp`.
2. Call `fdatasync` to flush all bytes to physical storage.
3. Atomically `rename(2)` to `data_{timestamp}.hint` — POSIX guarantees this rename is atomic on the same filesystem.
4. Sync the directory (see *Directory Sync*). Only restart time depends on it: a lost rename leaves the data file hint-less, and the next open rebuilds the hint from it.

Any `.hint.tmp` file found at startup is discarded (it represents an incomplete write interrupted by a crash). Recovery will re-scan the corresponding `.data` file instead.

Because hint file writes are deferred (see above), this protocol is exercised at engine close or during an explicit `flush_hints()` call — not inside the rotation critical path.

### Directory Sync

`fdatasync` makes a file's bytes durable, not the directory entry that names it, and POSIX lets a power loss keep a later directory operation while dropping an earlier one. Every create or rename that something later depends on is therefore followed by `sync_directory(dir)` (`fsync` on the directory, #199), before the thing that depends on it:

| Entry | Synced | Before |
|---|---|---|
| directories `DB::open` creates | each parent, deepest first | anything is written in them |
| a new active file (`DB::open`, `rotate_active_file`, `resume()`, all through `create_active_file`) | `dir_` | any write into it is acknowledged |
| vacuum's compacted file, renamed from `.data.tmp` | `dir_` | the commit, and so the unlink of the source |
| a hint file, renamed from `.hint.tmp` | `dir_` | nothing; a lost rename only costs a rebuild |

Unlinks are not synced. A lost unlink brings back a file the engine already handles: a vacuum source next to its compacted copy is undone at recovery, and a vacuumed-away file that reappears, or an uncommitted copy whose removal was lost, is the same pair. Any later directory sync — a rotation, a hint, the next vacuum's rename — makes the unlink durable, so a power cut leaves at most the pairs vacuum created since the last one. Vacuum's staging create is not synced on its own; the sync after the rename covers the entry.

A filesystem that cannot sync a directory returns `EINVAL`, which is taken as done, as PostgreSQL does. The WASM build has nothing to sync. The rotation sync runs under the write path, once per `max_file_bytes`, where rotation already waits on the hint backlog; the commit path does not change.

On ext4 and XFS, metadata is journaled in order, so none of these entries is likely to be lost without the sync. The engine does not depend on that. Each site passes its own fault injection checkpoint to `sync_directory` (`io_dir_sync_*`), and the `[dir_sync]` tests fail each one: each shows its site still syncs, and that nothing the sync guards goes ahead when it fails.

### Module Plan

`HintFile` lives in the `bytecask.hint_file` C++20 module (`bytecaskdb/hint_file.cppm`), symmetric with `DataFile`:

Construction uses named static factory functions to make intent explicit at the call site:

- **`HintFile::OpenForWrite(path) -> HintFile`** — opens the file immediately with `O_WRONLY | O_CREAT | O_TRUNC`. Each `append()` serializes one entry and writes it directly to the fd, updating a running CRC-32C accumulator.
- **`HintFile::OpenForRead(path) -> HintFile`** — reads the whole file once in chunks, verifies the file-level CRC and records where each unit starts, and holds no descriptor afterwards: scanners open the file for each unit they read (see *HintFile I/O model*).

Write API:
- **`append(sequence, entry_type, file_offset, key, value_size) -> void`**: Serializes one hint entry and writes it to the fd. Only `Put` and `Delete` are valid entry types; passing `BulkBegin` or `BulkEnd` is a programming error.
- **`close() -> void`**: Writes the 4-byte CRC-32C trailer, calls `fdatasync()`, and closes the fd. If the HintFile is destroyed without calling `close()`, the fd is closed without writing the CRC — the `.hint.tmp` file is cleaned up on next startup.

Read API:
- **`make_scanner() -> Scanner`**: Returns a forward-only scanner. `HintEntry.key` is a `span<const byte>` into the scanner's decoded unit — valid until its next `next()` or `seek()`.

`HintEntry` is a plain struct holding `{uint64_t sequence, EntryType entry_type, uint64_t file_offset, std::span<const std::byte> key, uint32_t value_size}`.

## Range Scan: `iter_from`

`iter_from` returns a lazy input range of `(Key, Bytes)` pairs in ascending key order. Each dereference issues a single `pread` via `DataFile::read_value_into()` — the caller-supplied `key_size` and `value_size` (from `KeyDirEntry`) let the engine compute the total entry size upfront, where a whole-file sweep, which does not know sizes in advance, reads 1 MiB chunks instead (`DataFileIterator`). The iterator reuses an internal I/O buffer and the cached value vector across advances, so sequential scans incur zero allocations after the first dereference.

Results are always in ascending key order (key directory iteration order).


### Type Aliases

```cpp
// Owned byte buffer — used for return values and batch storage.
using Bytes = std::vector<std::byte>;

// Owned key — semantically distinct from a generic byte buffer.
// Keys have an upper bound of 65 535 bytes (u16 key_size in the data file header).
// Starting as an alias for Bytes; may be refined to enforce the size invariant.
using Key = Bytes;

// Non-owning view — used for all input parameters.
using BytesView = std::span<const std::byte>;
```

`std::byte` makes the intent clear (raw bytes, not text) and prevents accidental arithmetic. `Key` is kept distinct from `Bytes` so the key directory type (`PersistentOrderedMap<Key, KeyDirEntry>`) reads as its intent and provides a single point of change if the key type needs to evolve. `BytesView` as the universal input type avoids copies at call sites and accepts any contiguous range.

### WritePlan

`WritePlan` is the unified type for all write operations submitted to `apply_batch`. It carries both **writes** (`put`, `del`, `del_range`) and optional **guards** (`ensure_present`, `ensure_absent`, `ensure_unchanged`, `ensure_range_unchanged`). When constructed with a `Snapshot`, guards and implicit W-W checks are available; without a snapshot, only unconditional writes and `ensure_present`/`ensure_absent` guards are available.

`WritePlan` is move-only and single-use; `DB::apply_batch` consumes it by move. See *Snapshots and conditional writes* below and `docs/transaction_design.md` for the full type definition and guard semantics.

### Iterators

Both `KeyIterator` and `EntryIterator` satisfy `std::bidirectional_iterator`. They yield entries in ascending key order when advanced with `operator++` and descending order with `operator--`. This matches the key directory's iterators, which are themselves bidirectional. Backward traversal steps back through the iterator's stack of `(node, index)` frames — O(1) amortized per step, matching forward iteration.

Forward scans use `iter_from` / `keys_from` (keys >= `from`); reverse scans use `riter_from` / `rkeys_from` (keys <= `from` in descending order). Both are available on `DB` and `Snapshot`. RocksDB equivalents: `Seek` + `Next` ↔ `iter_from`; `SeekForPrev` + `Prev` ↔ `riter_from`.

Reverse iteration is provided by a generic `ReverseIterator<Iter>` template that wraps `KeyIterator` or `EntryIterator`. `std::reverse_iterator` cannot be used because its `operator*` dereferences a temporary copy of the underlying iterator — when the underlying iterator caches its result internally (as both `KeyIterator` and `EntryIterator` do), the returned reference dangles. `ReverseIterator` holds the inner iterator directly and pre-decrements once in the constructor, so the reference remains valid.

```cpp
// Yields (key, value) pairs — bidirectional.
class EntryIterator {
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type      = std::pair<Bytes, Bytes>;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> EntryIterator&;
    auto operator--() -> EntryIterator&;
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};

// Yields keys only (no value I/O) — bidirectional.
class KeyIterator {
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type      = Bytes;
    using difference_type = std::ptrdiff_t;

    auto operator++() -> KeyIterator&;
    auto operator--() -> KeyIterator&;
    auto operator*() const -> const value_type&;
    auto operator==(std::default_sentinel_t) const noexcept -> bool;
};
```

Both integrate with `std::ranges::subrange` so callers can use range-for directly:

```cpp
// Forward scan (ascending).
for (auto& [key, value] : db.iter_from(opts, start_key)) { ... }
for (auto& key : db.keys_from(opts, prefix))              { ... }

// Reverse scan (descending).
for (auto& [key, value] : db.riter_from(opts, start_key)) { ... }
for (auto& key : db.rkeys_from(opts, prefix))              { ... }
```

- **Lazy**: each dereference reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries.
- **`KeyIterator` is in-memory only**: walks the in-memory key directory without touching any data file.
- **Error handling**: throws `std::system_error` on I/O failure.
- **Self-anchored**: `EntryIterator` and `ReverseEntryIterator` each hold their own `shared_ptr<const EngineState>`, and `KeyIterator` holds the key-directory root it was built from. An iterator therefore keeps every data file it can reach open, and the subtree it walks immutable, independently of the `DB` and of the `Snapshot` it came from — which is why a span may outlive that `Snapshot` but never the iterator. The full per-event table is *View and span lifetimes* in [`CONTRACT.md`](../CONTRACT.md).
- **Move-only (entry iterators)**: `EntryIterator` and `ReverseEntryIterator` cache the spans `operator*` returns, and on the `pread` path those spans address the iterator's own `io_buf_`. Copying would deep-copy the buffer while carrying the spans unchanged, leaving the copy pointing into the source's storage — so the copy operations are deleted and the hazard is a compile error rather than a comment. Moving is safe: the buffer travels with the spans. `ChangeIterator` is move-only for the same reason. `KeyIterator` materializes an owning `Key` and stays copyable, which `ReverseIterator<KeyIterator>` requires.

### WriteOptions and ReadOptions

Modelled after LevelDB/RocksDB. All write operations accept a `WriteOptions`; all read operations accept a `ReadOptions`. Both default-construct to the same behaviour as the old bare signatures.

```cpp
struct WriteOptions {
    // When true (default), fdatasync is called after every write.
    // Set to false to skip the sync for higher throughput at the cost of
    // durability on crash: data is in the OS page cache but not on disk.
    bool sync{true};
};

struct ReadOptions {
    // Placeholder for future read-path knobs (e.g. verify_checksums).
};
```

**`sync` default of `true`**: preserves the pre-existing crash-safe behaviour. Callers that deliberately trade durability for throughput (e.g., bulk import, benchmarks) opt out explicitly by setting `sync = false`. `close()` (which the destructor calls) syncs before it returns, so `sync = false` on individual writes does not risk losing data on clean shutdown — only on an OS/power failure between the last write and the close. If that sync fails, `close()` throws, so the caller learns the writes are not durable.

### Bytecask

```cpp
class Bytecask {
public:
    // Opens or creates a database rooted at `dir`.
    // Throws std::system_error if the directory cannot be opened or recovery fails.
    [[nodiscard]] static auto open(std::filesystem::path dir) -> Bytecask;

    Bytecask(const Bytecask&)            = delete;
    Bytecask& operator=(const Bytecask&) = delete;
    Bytecask(Bytecask&&) noexcept        = default;
    Bytecask& operator=(Bytecask&&) noexcept = default;
    ~Bytecask();  // calls close() if it was not called, swallowing its errors

    // Makes every write durable, writes the hint files and releases the
    // directory lock. Throws if an acknowledged write is not durable or the
    // shutdown failed; closed either way. Idempotent.
    void close();

    // ── Primary operations ────────────────────────────────────────────────

    // Returns the value for `key`, or std::nullopt if the key does not exist.
    // Throws std::system_error on I/O failure or std::runtime_error on corruption.
    [[nodiscard]] auto get(const ReadOptions& opts, BytesView key) const
        -> std::optional<Bytes>;

    // Output-parameter variant: writes the value into `out`, reusing its
    // existing capacity to amortize allocation across calls. Returns true
    // if the key was found, false otherwise.
    [[nodiscard]] auto get(const ReadOptions& opts, BytesView key,
                           Bytes& out) const -> bool;

    // Writes `key` → `value`. Overwrites any existing value.
    // Throws std::system_error on I/O failure.
    void put(const WriteOptions& opts, BytesView key, BytesView value);

    // Writes a tombstone for `key`.
    // Returns true if the key existed and was removed, false if it was absent.
    // Throws std::system_error on I/O failure.
    [[nodiscard]] bool del(const WriteOptions& opts, BytesView key);

    // Returns true if `key` exists in the index (no disk I/O).
    [[nodiscard]] auto contains_key(BytesView key) const -> bool;

    // ── Batch ─────────────────────────────────────────────────────────────

    // Atomically applies all operations in `plan` wrapped in BulkBegin/BulkEnd entries.
    // `plan` is consumed (move-only). No-op if plan is empty and has no guards.
    // When the plan carries a snapshot, guards and implicit W-W checks are evaluated.
    // Returns true if committed, false on conflict.
    // Throws std::system_error on I/O failure or DbDegraded if degraded.
    [[nodiscard]] auto apply_batch(const WriteOptions& opts, WritePlan plan) -> bool;

    // ── Range iteration ───────────────────────────────────────────────────

    // Forward: keys >= `from` in ascending order.
    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    // Reverse: keys <= `from` in descending order.
    // Empty `from` starts from the last key.
    [[nodiscard]] auto riter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;
    [[nodiscard]] auto rkeys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

    // ── Manifest ──────────────────────────────────────────────────────────

    // Rotates the active file, waits for all hint files, and returns a
    // manifest of sealed files with a snapshot. Forces file rotation. A
    // listed hint may be missing, if its write failed; opening the copied
    // files rebuilds it.
    // Vacuum must not run between create_manifest() and file transfer
    // completion (caller responsibility).
    [[nodiscard]] auto create_manifest() -> FileManifest;

private:
    explicit Bytecask(std::filesystem::path dir);
};
```

### Usage Examples

```cpp
// Open (or create) a database.
auto db = Bytecask::open("my_db");

// Single-key operations.
db.put(as_bytes("user:1"), as_bytes("alice"));

auto val = db.get(as_bytes("user:1"));
if (val) { /* use *val */ }

bool existed = db.del(as_bytes("user:1")); // false if key was absent

// Atomic batch.
WritePlan plan;
plan.put(as_bytes("user:2"), as_bytes("bob"));
plan.put(as_bytes("user:3"), as_bytes("carol"));
plan.del(as_bytes("user:1"));
db.apply_batch({}, std::move(plan));

// Range scan (forward).
for (auto& [key, value] : db.iter_from(as_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only scan (no disk I/O — key directory walk only).
for (auto& key : db.keys_from(as_bytes("user:"))) { ... }

// Reverse scan (descending).
for (auto& [key, value] : db.riter_from(as_bytes("user:~"))) {
    // Iterates all keys <= "user:~" in descending order.
}
for (auto& key : db.rkeys_from(as_bytes("user:~"))) { ... }
```

> `as_bytes` is a small helper that converts a string literal or `std::string_view` to `BytesView`. Its exact form is TBD.

## Background Worker

`BackgroundWorker` is a non-exported internal class in `bytecask.engine`. It maintains a single persistent background thread that processes tasks in FIFO order.

### API

```cpp
class BackgroundWorker {
public:
  BackgroundWorker();
  ~BackgroundWorker();                        // drains, then joins thread
  void dispatch(std::function<void()> task);  // non-blocking enqueue
  void drain();                               // block until idle
  std::size_t pending() const;                // queued + running
  void wait_pending_below(std::size_t n);     // block until pending() < n
};
```

### Lifetime invariant

`BackgroundWorker` is declared as the **last member** of `Bytecask`. C++ destruction order is reverse of declaration order, so `worker_` destructs first — its destructor sets `stop_ = true`, notifies the worker thread, and joins it. This guarantees the background thread has finished all tasks before any other `Bytecask` member (`dir_`, `state_`, `write_mu_`, etc.) is destroyed. There is no risk of the background thread accessing freed memory.

### Exception handling

Exceptions thrown by background tasks are caught per-task, logged to `stderr`, and swallowed. Hint file writes are correctness-safe to drop: recovery always falls back to scanning the raw `.data` file if no `.hint` companion exists.

### BC-026: deferred hint file writes

After `rotate_active_file()` seals a data file, it captures a `shared_ptr<DataFile>` to the sealed file and dispatches `flush_hints_for(file, dir)` to the worker through `dispatch_hint()`. Before sealing it calls `wait_for_hint_backlog()`, which holds the rotation until the worker has fewer than `max_hint_backlog` tasks (see *Relationship to Data Files*). The `shared_ptr` keeps the `DataFile` fd alive for the duration of the background task regardless of what the engine state does subsequently.

The synchronous path in `flush_hints(EngineState&)` (called by the public `flush_hints()` method) reuses the same `flush_hints_for` helper, guaranteeing consistent hint-writing behaviour between the background and synchronous paths.

`DB::close()` syncs the active file, shrinks it, writes its hint when the engine is not degraded, then calls `flush_hints()` which drains the background worker and writes hint files for all sealed files. This guarantees that after a clean shutdown every data file has a companion `.hint` — the next `DB::open()` recovers purely from hint files with no raw `.data` scanning.

### `DB::close()` (#257)

`close()` is the shutdown. It exists because a destructor cannot report that the final `fdatasync` failed: a caller that only destroyed the `DB` could not tell a clean shutdown from one that lost its unsynced writes. `~DB()` still calls `close()` when the caller did not, and swallows what it throws.

It takes `vacuum_mu_`, then the write barrier — the order `vacuum()` takes them in — so a running vacuum finishes, and every write whose entries stage 1 has appended is flushed and published before the close reads the state. A writer that arrives later finds the closed state at admission and throws `DbClosed`. Then:

1. **Healthy engine:** `fdatasync` the active file, mark every published sequence durable, `shrink_to_fit()`, and write the active file's hint if it holds anything. **Degraded engine:** no sync. One that failed earlier left pages clean that a later one does not write (#231), so what was durable when the engine degraded is what is durable. The trim is best effort.
2. `flush_hints()`: drain the worker and write the sealed files' hints. Those files were synced whole when they were sealed, so this runs whether or not the engine is degraded.
3. Publish `closed_copy()` of the state through the checked `store_state`, which wakes `durable_sequence` and `wait_published` waiters.
4. Take this DB's entries out of every thread's read cache, then release the directory lock.
5. Rethrow the first error.

`close()` returns normally only if every acknowledged write is durable and the shutdown completed. On a healthy engine a failed sync, trim or hint write throws `std::system_error`. On a degraded one it throws `DbDegraded` when `durable_seq` is below the last published sequence — `sync = false` writes acknowledged after the last good sync. A degraded engine whose acknowledged writes are all durable closes normally: the batch whose sync failed was never acknowledged. The DB is closed either way. A retry cannot help, so a second `close()` returns at once without reporting again.

The closed state keeps the sequences, mode and degraded reason, and drops the files and the key directory, so the DB stops holding them. Snapshots and iterators keep the versions they already hold; read caches do not. A cache is reclaimed by idleness and knows its DB only by address, so without step 4 every thread that had read the DB — the closing one included — kept the last state it saw until it read another DB or went idle under some other DB's writes. That state holds the data files and, with `IoBackend::BufferPool`, the pool and all its frames. Step 4 uses the scrape's protocol: a slot holding an entry of this DB is swapped to `kObsolete` and its entry freed. A slot found claimed may be a read of this DB in flight that will put an entry back, so the pass repeats until it finds no slot claimed, by a read of any DB; a claim lasts one read, so the wait is bounded by the reads in flight. A read on another thread that races an explicit `close()` is not ordered after it and could cache a state again, so `~DB` repeats the pass: by then every call on the DB has returned, and nothing of it is left in any cache. Every operation checks `closed` on the state it loads: readers in `load_state_for_read`, one branch on the state already loaded; writers at admission, through `is_write_allowed()` and `is_ingestion_allowed()`. The `noexcept` accessors — `mode()`, `is_degraded()`, `degraded_reason()` — keep answering from the closed state. `DbClosed` is a `std::logic_error`: using a closed handle is a caller bug, not an I/O condition.

A failed close leaves on disk what a crash would, and the next open handles it the same way: the active file is hint-less, so it is rewritten durably and scanned (*Recovering a Hint-less File*).

The bindings report it too. C: `bytecask_close` returns `-1` with `bytecask_errmsg()` set, and frees the handle either way. Python: `DB.close()`, and `DB` is a context manager; `DbClosed` is a `ValueError`, as Python raises for a closed file. Node: `close()` throws once the handle is released; under WASM the bound `closeDb` returns the message, because `-fwasm-exceptions` hides `what()` from JS. MariaDB: `bytecaskdb_deinit` logs a failed close with `sql_print_error` and returns `1`.

---

## Fault Injection Test Seam

To directly test correctness paths that only activate on IO failures (BC-131, BC-133), `bytecask.data_file` exposes a thread-local fault injection API compiled exclusively when `BYTECASK_TESTING` is defined.

### API

```cpp
// Fail the Nth next append() call. n=0 fails immediately; n=1 lets one through, etc.
void fault_inject_nth_append(int n) noexcept;

// Fail the Nth next sync() call. n=0 fails immediately.
void fault_inject_nth_sync(int n) noexcept;

// Reset all fault state to disabled. Call after each test (or use FaultGuard).
void fault_inject_reset() noexcept;
```

Fault state is per-thread (`thread_local FaultHooks`), so tests on different threads don't interfere. Both functions throw `std::system_error(EINVAL)` when the countdown reaches zero, identical in type to a real IO failure.

### Faults by count, below the engine

The checkpoints above fail the calls someone named. `bytecask_tests` also fails calls nobody named: it is linked with `-Wl,--wrap` for each I/O call it references, and the `[fault_sweep]` tests fail the 1st, 2nd, …, n-th call of each operation in turn, on each I/O back-end, and check generic invariants after each. It links the C++ standard library statically, so the calls the engine makes through `std::filesystem` (the hint's rename, vacuum's removes, `file_size`, `exists`, the directory scan at open) are counted like its own. A new I/O call is covered the day it is added, from the engine or from the standard library; one whose spelling has no interposer fails the link, except on glibc older than 2.33 (the manylinux wheels), where that guard is off. The sweep is Linux-only (Apple's linker has no `--wrap`). See `docs/correctness_validation.md`, *Counted fault sweep*.

### Intended use

The `FaultGuard` RAII helper in `bytecask_test.cpp` calls `fault_inject_reset()` on destruction, so the fault state is always cleared even when a test assertion fails.

Tests enabled by this seam:
- **`[fault_inject]` mid-batch append failure** (BC-131): confirms that a failed append after `BulkBegin` triggers rotation and that the orphaned batch is discarded by recovery.
- **`[f_visibility]`/`[g_visibility]`** (BC-155): confirms that key changes are not published on sync failure — written key is absent after F or G failure, engine not degraded.

### Page cache model

`PageCacheModel` (`fault_injector.h`, activated by `ScopedPageCacheModel`) models which bytes of a data file the device holds, so a test can cut the power after a failed `fdatasync` with Linux semantics. Per 4 KiB page it keeps the device's bytes while the cache holds others: a data file write records the page as it read before the write and marks it dirty, a successful sync forgets the dirty pages' images, and an injected sync failure (`FAULT_INJECTION_SYNC`) only clears their dirty marks, so later syncs keep them too. `restore_device(dir)` writes every image back — a power cut, or the kernel evicting the pages — and `mark_unsynced(path)` models a process killed before its sync; both are for a directory no engine has open. `copy_device(dir, dst)` is the cut for a live engine: it copies the directory and overlays the images on the copy, leaving the engine and its files untouched, so the proof generators cut the power after every cell and recover the copy against the durable watermark ([`correctness_validation.md`](correctness_validation.md), *Power-loss axis*). Hint files are not modelled; what they depend on is checked instead. A hint may only index bytes the device holds, which the engine keeps by ordering — a file is synced before it is sealed and hinted, a hint-less file is rewritten and synced before open hints it — and `hint_written`, called as each hint's trailer is written, records a violation if a page of the hint's data file has an image that differs from its current bytes (an image equal to them is a rewrite of durable data in flight, which the device holds either way). The `[fsyncgate]` tests and the generators use it.

### Design notes

The seam is intentionally minimal:
- Zero production overhead: the `#ifdef BYTECASK_TESTING` blocks compile away entirely in non-test builds.
- Thread-local (not global) state: each thread has an independent countdown, avoiding cross-test interference in multi-threaded test runners.
- No virtual dispatch, no interface changes: `DataFile` remains a concrete class.

---

## Snapshots and conditional writes: `snapshot()` and `apply_batch(WritePlan)`

Two primitives added to `DB` in BC-103 providing snapshot isolation without any mandatory transaction wrapper.

### `DB::snapshot() → Snapshot`

Returns a `Snapshot` — a move-only, read-only value wrapping a `shared_ptr<const EngineState>`. The entire state at that moment is frozen: the key directory, the file registry, and open file descriptors. Reads on `Snapshot` are lock-free; no mutex is ever acquired.

The `shared_ptr` keeps all data files referenced at snapshot time alive — their file descriptors remain open. Vacuum unlinks files immediately, but POSIX guarantees that `pread` on an unlinked file succeeds as long as the fd is open. Disk blocks are freed when the last `shared_ptr<DataFile>` is destroyed, closing the fd.

`Snapshot` exposes the same read API as `DB`: `get`, `contains_key`, `iter_from`, `keys_from`, `riter_from`, `rkeys_from`.

### `DB::apply_batch(opts, plan) -> std::optional<CommitResult>`

Applies `plan` atomically only if all guards pass and no key in the write set was modified since the plan's snapshot was taken (when the plan carries a snapshot). Both the conflict check and the apply run under `write_mu_`, serialised with all other writers.

Conflict is detected by comparing `KeyDirEntry::sequence` between the snapshot state and the current state for each key in the write set. Three conflict cases are detected:

1. Key absent in snapshot but present now (key appeared after snapshot).
2. Key present in snapshot but absent now (key deleted after snapshot).
3. Key present in both but with a different `sequence` (key modified after snapshot).

On the first conflict detected, `apply_batch` returns `nullopt` before any I/O is performed. If no conflict is found, the writes are applied atomically.

#### Implicit W-W check on write keys

When a `WritePlan` carries a snapshot, `apply_batch` automatically checks every key in the write set (put or del) for concurrent modification — the caller does not need to call `ensure_unchanged` on keys they intend to write. This closes a common concurrency hole: without it, a plan could read a key, compute a new value, and write it back without noticing a concurrent writer already changed that key.

**Checked and applied by location.** A record location, `(file_id, offset)`, names one immutable record for the life of the process — file ids are never reused and an append-only file holds one record per offset — and a record holds one key. So a key whose entry in the head still points at the record the snapshot had is that key, unchanged. `apply_batch` looks up each written key in the snapshot on the caller's thread, before the plan joins a batch (`WritePlan::resolve_snapshot_entries`); under the write lock the W-W check then asks the head for that location, which on the blind key directory reads no record (the leaf's fingerprint scan finds the candidates and the location picks the key's entry), and the update or erase replaces or removes that entry in place, without the read that places a key. Only a key that no longer points at the snapshot's record is looked up by key: to tell a changed key from one vacuum relocated (same sequence, new location), and to place it. A put of a key the snapshot lacked — an insert — still reads a neighbour to place it. In `oltp_write_only` three of four written rows are updates or a delete, so their record reads leave the serial section; the snapshot lookups they replace run in parallel across writers. The keyed trees read no record to find a key and go by key as before; plans without a snapshot, range deletes and the guards are unchanged.

`ensure_unchanged` is for read-only dependencies: keys whose value influenced the plan's decisions but that the plan does not modify. For example, reading a price to compute an order total — the price key is a read dependency but not in the write set, so it needs an explicit guard.

### Conflict signalling

`apply_batch` returns `std::optional<CommitResult>`: engaged with the assigned `CommitResult{sequence, durable}` if committed, `nullopt` on conflict (W-W or guard violation) — nothing was written, no sequence assigned. Conflicts are expected outcomes in concurrent workloads — not errors. I/O failures remain exceptions (`std::system_error`). `put` and `del_range` cannot conflict (no guards, no snapshot) and return `CommitResult` directly; `del` returns `nullopt` if the key was absent. See `docs/commit_result_api_design.md` for the full `CommitResult` contract (BC-231).

### Single-entry batch optimization

When the write set contains exactly one operation, `apply_batch` skips the `BulkBegin`/`BulkEnd` marker writes entirely. A single data entry is CRC-protected and self-describing — the markers add no recovery benefit. See D14.

## Operational Counters

`DB::stats()` returns a `std::map<std::string, std::int64_t>` containing all operational counters and gauges. Designed for pull-based metrics integration (Prometheus, logging, debugging).

**Monotonic counters** (increment only, reset on restart):

| Counter | Path | Description |
|---------|------|-------------|
| `bytecask.bytes_written` | Write | On-disk bytes appended (header + key + value + CRC) |
| `bytecask.group_writer_batches` | Write | `execute_slots()` calls (one per batch) |
| `bytecask.group_writer_coalesced` | Write | Total writers coalesced across all batches |
| `bytecask.group_writer_busy_us` | Write | Wall time `execute_slots()` held `write_mu_`, in microseconds: the serial section every write passes through. Over elapsed time, how busy it is; over `group_writer_coalesced`, its cost per write. When it nears 100%, write throughput is `1 / cost per write` whatever the thread count |
| `bytecask.commit_delay_waits` | Write | Flushes that waited for the commits the last fdatasync released before capturing the head (see *Commit delay before the settle*). |
| `bytecask.commit_delay_us` | Write | Total time those waits took, in microseconds. Over `commit_delay_waits`, the average wait. |
| `bytecask.commit_delay_fsync_us` | Write (gauge) | The commit delay's current estimate of one fdatasync (F), in microseconds. |
| `bytecask.commit_delay_round_trip_us` | Write (gauge) | The commit delay's current estimate of a client's round trip (R), in microseconds. |
| `bytecask.file_rotations` | Write | Active file rotations |
| `bytecask.fsyncs` | Write | `fdatasync` calls |
| `bytecask.disk_reads` | Read | `pread` calls from `get()` |
| `bytecask.keydir_versions_live` | Gauge | Key directory versions alive: 1 is the published state alone; each further one is a snapshot, an iterator or a thread's read cache holding an older tree |
| `bytecask.keydir_nodes_parked` | Gauge | Retired key directory nodes that older live versions still reach and so cannot be freed (see *Read path*) |
| `bytecask.keydir_pool_bytes` | Gauge | Process-wide: freed key directory nodes held for reuse on the node pool's shared list, at most 64 MiB; thread caches (at most 256 KiB per thread) not counted |
| `bytecask.disk_read_bytes` | Read | Bytes read from disk |
| `bytecask.vacuum_bytes_reclaimed` | Vacuum | Bytes freed by vacuum |
| `bytecask.vacuum_files_unlinked` | Vacuum | Data files physically removed |
| `bytecask.vacuum_tombstones_dropped` | Vacuum | Tombstones compaction left out because recovery found them no longer needed |
| `bytecask.tombstones_needed` | Gauge | Size of the needed-tombstone set recovery built at open (8 bytes each) |
| `bytecask.files_opened` | Lifecycle | `DataFile` opens (recovery, rotation, vacuum) |
| `bytecask.hint_backpressure_stalls` | Lifecycle | Rotations that waited for the hint worker (`max_hint_backlog`) |
| `bytecask.hint_backpressure_stall_us` | Lifecycle | Total time those rotations waited, in microseconds |
| `bytecask.crc_failures` | Error | CRC mismatches on the read path |
| `bytecask.io_errors` | Error | `std::system_error` from I/O operations |
| `bytecask.degraded_transitions` | Error | Times the engine entered degraded state, by any path — counted where every publication passes (the raw `store_state`), since the failure paths publish their degraded copy directly |

**Recovery counters** (set once at open, immutable for DB lifetime):

| Counter | Description |
|---------|-------------|
| `bytecask.recovery_files` | Data files replayed during recovery |
| `bytecask.recovery_keys` | Keys recovered into the key directory |
| `bytecask.recovery_duration_us` | Wall-clock recovery time in microseconds |

**Gauges** (current state, read from `EngineState` at call time):

| Gauge | Description |
|-------|-------------|
| `bytecask.degraded` | 1 if degraded, 0 otherwise |
| `bytecask.open_files` | Number of data files in the file registry |
| `bytecask.hint_backlog` | Sealed files whose hint is queued or being written: what close still has to write, and what an open after a crash would rebuild |

All atomic counters use `std::memory_order_relaxed` — sufficient for monotonic counters where cross-counter consistency is not required. Write-path counters are incremented under the existing `write_mu_` (zero contention).

Read-path counters (`disk_reads`, `disk_read_bytes`, `pool_hits`, `pool_misses`) are bumped by every reader on every `get`, and a single atomic for each is one cache line every core fights over. Measured on 32 reader threads, that line absorbs about 50 M read-modify-writes a second and sets the throughput on its own: two RMWs per `get` (`mmap`) gave ~25 M gets/s, three (the buffer pool adds `pool_hits`) gave ~16 M. They are therefore `StripedCounter`s: 16 cache-line-sized stripes, each thread bumping the stripe it drew round-robin on first use, `load()` summing them at `stats()` time. Every increment lands whichever stripe a thread draws, so the count is exact; a `load()` racing with writers is a sum of relaxed loads, which is what a monotonic metric needs. With the counters and the state copy (above) off the shared lines, both back-ends scale with reader threads at their single-thread ratio.

Counters are per-DB instance (`Counters` struct owned by `DB`). Two open databases have independent counter sets.

## Design Decisions

| # | Decision |
|---|----------|
| D1 | **Error handling**: Throw (`std::system_error` for I/O, `std::runtime_error` for corruption). These are panic-level events the caller cannot meaningfully recover from inline. `std::optional` covers the key-not-found case for `get`. No `std::expected` at this boundary — there are no anticipated recoverable error conditions in normal operation. |
| D2 | **Config**: Deferred — removed from the initial API scope. |
| D3 | **WritePlan ownership**: `WritePlan` is move-only (copy constructor and copy assignment deleted). Single-use by design. |
| D4 | **WritePlan size limit**: `kMaxBatchBytes` (1 GiB) on disk, markers included — `apply_batch` throws `std::invalid_argument` above it (see *Size Limits*). |
| D5 | **Iterator strategy**: Lazy — each `operator++` reads one value from disk on demand. Early-termination scans pay no I/O cost for unvisited entries. |
| D6 | **`KeyIterator` source**: In-memory only — walks the B-Tree key directory without opening any data file. |
| D7 | **`del` on missing key**: Returns `bool` — `true` if the key existed and was removed, `false` if it was absent. Consistent with `std::set::erase` returning a count. |
| D8 | **Error handling during iteration**: Throw `std::system_error` on I/O failure (consistent with D1 and standard C++ practice). |
| D9 | **Concurrency model**: SWMR — exactly one writer at a time; reads are concurrent. MVCC and snapshot isolation are not provided. |
| D10 | **Vacuum**: Two independently testable paths — `vacuum_compact_file` (rewrite sealed file into a new sealed file, dropping dead entries) and `vacuum_remove_file` (delete files with no live entries and no tombstones, no I/O required). `vacuum()` selects a target file above `fragmentation_threshold`, then branches: `vacuum_remove_file` if `live_bytes == 0 && tombstone_bytes == 0`, otherwise `vacuum_compact_file`. Returns `true` if a file was processed, `false` if nothing qualified. No compound paths. All vacuum-related identifiers use a `vacuum_` prefix. One sealed file per `vacuum()` call. Engine continues serving reads and writes. For `vacuum_compact_file`, `write_mu_` is held only for the commit step (I/O writes to a private temp file). For `vacuum_remove_file`, `write_mu_` is held only for the brief metadata update. `vacuum_commit` itself does not acquire `write_mu_` — the caller is responsible for holding it. A tombstone is dropped only when recovery found it no longer hides a Put in another file (`NeededTombstones`); tombstones written since the open are always copied. File selection uses `fragmentation > fragmentation_threshold`, with fragmentation `1 − (live_bytes + tombstone_bytes + marker_bytes) / total_bytes`, computed from incrementally maintained `FileStats` — O(1) per file. Stats are reconstructed during recovery as a side-effect of the hint-file pass. |
| D11 | **File naming**: `data_{YYYYMMDDHHmmss}_{RRRRRRRR}_V{XX}`. Timestamp is UTC second precision — a human-readable creation-time hint, not content age (compaction produces new files with old entries). `RRRRRRRR` is a 4-byte random hex salt for collision avoidance. `V{XX}` is the file format version (`V01` initially). Filename ordering carries no semantic meaning; entry sequence numbers are authoritative. |
| D12 | **Hint file atomicity**: Write to `*.hint.tmp`, `fdatasync`, then atomically `rename(2)` to `*.hint`, then sync the directory. A `.hint.tmp` file found at startup is discarded. |
| D13 | **Incomplete batch recovery**: An unmatched `BulkBegin` in the active data file scan causes the partial batch to be discarded with a logged warning. No partial-batch entries enter the key directory. |
| D14 | **Single-entry batch optimization**: When `apply_batch` is called with exactly one operation, the `BulkBegin`/`BulkEnd` marker writes are skipped. A single data entry is self-describing and CRC-protected, so the markers add no recovery benefit for a write set of size 1. |
| D15 | **C ABI / shared-library link constraint**: `libbytecask.a` is compiled with `-fPIC` so it can be linked into a shared object (e.g. `ha_bytecaskdb.so`). Without `-fPIC`, clang emits `R_X86_64_TPOFF32`/`R_X86_64_32S` relocations illegal in a DSO. xmake syntax: `add_cxxflags("-fPIC", {force = true})` on the `bytecask` static target. |
| D16 | **MariaDB plugin header ordering**: Server-internal headers require `server/my_global.h` before `handler.h`. The client-side stub does not define `MY_GLOBAL_INCLUDED`/`uchar`/`unlikely()`. Fedora layout: base `/usr/include/mysql`, server `/usr/include/mysql/server`, private `/usr/include/mysql/server/private`. CMake include order must be `server/private` → `server` → base. `-DMYSQL_SERVER` is required. `handlerton::state` does not exist in this MariaDB ABI; use `PLUGIN_LICENSE_GPL` (no MIT constant). |
| D17 | **Directory locking**: One process per directory, enforced by `flock()` on `dir/.lock`. Advisory only — does not protect against uncooperative processes that bypass `DB::open()`. |
| D18 | **Sequence-disjoint files**: All data files must have non-overlapping sequence ranges — no two files contain entries with the same sequence number. Active file rotation naturally preserves this (sealed files have contiguous sequence ranges). Vacuum compact must ensure compacted files maintain disjoint ranges; the one exception, a compacted file whose source outlived a kill, is removed at the next open (see *Vacuum → Crash safety*), and recovery refuses any other overlap. This invariant enables efficient replication (linear scan instead of min-heap merge), supports future file merging operations, and allows skipping entire files based on sequence bounds. |
| D19 | **MariaDB insert hot path**: Handler-open state caches secondary-index metadata and direct catalog counter pointers. Per-row INSERT processing must not take catalog map locks for stable table metadata, row-count increments, or AUTO_INCREMENT reservations once the handler has opened the table. |

## Replication Primitives

ByteCaskDB exposes a minimal set of primitives for building leader-follower replication on top of the engine. The design avoids embedding a replication protocol — instead it provides the read and write building blocks that an external coordinator composes.

See [`replication_primitives_design.md`](replication_primitives_design.md) for the full design reference.

### Mode

```cpp
enum class Mode { Leader, Follower };
```

`Mode` controls which write paths are available:

- **Leader** (default): normal writes (`put`, `del`, `del_range`, `apply_batch`) are allowed; `ingest` is rejected.
- **Follower**: normal writes throw `DbFollowerMode`; only `ingest` is allowed. Reads, snapshots, vacuum, and `resume()` work in both modes.

`set_mode(Mode)` acquires the write mutex to ensure no in-flight write straddles the transition. `mode()` is a lock-free atomic read (acquire semantics), same pattern as `is_degraded()`.

A leader stepping down (`set_mode(Mode::Follower)` from `Leader`) calls `fdatasync` on the active file before it publishes the new mode, and raises `durable_seq` to the last assigned sequence. `changes_since` ships only up to `durable_seq`, so without the sync a write acknowledged with `sync = false` stays unshippable: a planned transfer would complete without it, and the new leader would reuse its sequence (found by the topology replication check, #178). The sync is skipped when nothing is above `durable_seq`, which is always the case for a follower: `ingest` syncs before it publishes, so the test does not look at the mode being left. It is skipped too on a degraded engine, where a sync after a failed one proves nothing (#231): a degraded leader steps down with its unsynced writes still above `durable_seq`. A failed `fdatasync` degrades the engine, publishes nothing else, and rethrows; the mode stays `Leader`, and `resume()` recovers as after any failed commit sync.

Every sync of the active file degrades the engine when it fails, whatever called it: the commit flush, a rotation, `set_mode`, vacuum, `create_manifest` and `ingest`. A caller that only threw would leave a healthy engine whose next `fdatasync` returns 0 without writing the failed pages, and `durable_seq` would rise over writes the device does not hold. `create_manifest` did exactly that until #281.

### Leader-side: `durable_sequence`, `create_manifest`, `changes_since`

- `durable_sequence(min_sequence, timeout)` — the single sequence primitive (renamed from `current_sequence` — BC-231). Blocks until the durable sequence reaches at least `min_sequence` or the timeout expires, then returns the durable sequence; `min_sequence = 0`/an already-reached target/a nonpositive timeout return immediately without blocking (useful for polling replicas or waking a replication loop only when the leader is genuinely ahead).
- `create_manifest()` — rotates the active file, waits for all hint files, and returns a `FileManifest` of sealed files with a snapshot. Used for initial bootstrap. Every listed data file exists; a hint may not, since the background worker only logs a failed hint write. That is safe because a hint is a rebuildable index: the receiver's open writes the hint of any data file that lacks one, at the cost of one scan of that file. The manifest builds `hint_path` from the file's name rather than checking it, and callers copy a hint only when it exists (#349).
- `changes_since(seq, snap)` — returns a lazy `ChangeIterator` that walks sealed files in sequence order, yielding `DataEntryView` entries with `sequence > seq`. Constant memory — scans one entry at a time.

### Follower-side: `ingest`

```cpp
void ingest(std::span<const DataEntryView> entries);
```

`ingest` applies pre-sequenced entries from a leader to the follower's storage. Key properties:

- **Idempotent**: entries with `sequence <= durable_seq` are silently skipped.
- **Batch-safe rotation**: `BulkBegin`/`BulkEnd` pairs always land in the same data file. Rotation only occurs at boundaries where no batch is open.
- **Chunked I/O**: entries are written in chunks separated by rotation boundaries — one `pwritev` + one `fdatasync` per chunk, mirroring the leader's group-commit batching.
- **Durability before visibility**: `store_state` (publishing to readers) happens only after the final `fdatasync`.
- **Degraded-state on failure**: same pattern as the normal write path — on I/O failure, the engine goes degraded and `resume()` recovers.

Correctness is validated by 211 generated proof tests (178 ingest + 33 manifest) covering the full (StateShape × OpsShape × FailureClass) matrix. See [`correctness_validation.md`](correctness_validation.md) for the proof framework and [`replication_primitives_design.md`](replication_primitives_design.md) for the invariants.

## Working agreement

For each repository change, this file should be updated when the change affects one of the following:

- architecture
- behavior
- build or test workflow
- important implementation constraints