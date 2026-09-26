# ByteCaskDB

> **Status: early development.** The core engine works and is well-tested, but the API and on-disk format may change before a stable release. Not recommended for production use yet.

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/gustavoamigo/bytecaskdb)
[![CI](https://github.com/gustavoamigo/bytecaskdb/actions/workflows/ci.yml/badge.svg)](https://github.com/gustavoamigo/bytecaskdb/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/gustavoamigo/bytecaskdb/branch/main/graph/badge.svg)](https://app.codecov.io/gh/gustavoamigo/bytecaskdb)

Test Analytics dashboard: https://app.codecov.io/gh/gustavoamigo/bytecaskdb/tests/new

CI uploads JUnit test results with per-test source file/line metadata so Test Analytics can show source context instead of a flat list.
CI also publishes JUnit results directly in GitHub Checks for PR-native test summaries.

**ByteCaskDB** is a fast, predictable embedded key-value store written in C++. Reads and writes have flat, predictable latency from thousands of keys to hundreds of millions.

All keys in memory at all times — a deliberate design choice that removes an entire class of complexity that exists solely to minimise disk access and makes every point lookup O(1) with flat, predictable latency. The key directory holds no key bytes, so a key costs 14–19 bytes of RAM whatever its length (measured at 1M keys, structured to random): 128 GB holds on the order of six billion keys. Very few moving parts — an in-memory key directory and an append-only data file — is what keeps that latency flat whether you have 1,000 records or 100 million. 

Built on the [Bitcask](https://riak.com/assets/bitcask-intro.pdf) append-only foundation, ByteCaskDB replaces the original hash-table key directory with a **[persistent B+ tree with blind leaves](docs/blind_leaf_btree_design.md)** — inner nodes carry separators, each leaf entry is a crit bit, a fingerprint and a record location, and the key bytes live only in the data file. It gives ordered range queries, prefix scans, and prefix compaction, while keeping the simplicity that makes Bitcask fast. Snapshots are O(1) — just a root pointer copy. Full MVCC and serializable conflict detection are supported with no separate transaction type required.

## Features

- **Sequential write path** — all I/O is sequential appends; no random writes. Every `put` and `del` is one append. `apply_batch` with N operations appends a begin marker, N entries, and an end marker in a single `pwritev` — still no WAL, no random writes. Because the key directory stores no key bytes, a `put` or `del` also reads one record (the key's own, or a neighbour's on an insert) to place the key. The whole record is read and CRC-checked, value included, so its cost grows with that record's value size; it is served from the buffer pool or page cache, and if it fails the operation fails before anything is appended.
- **Ordered range iteration** — scan from any key prefix in key order. Keys are read from their records as the iterator advances, values lazily. Bidirectional: scan forward with `iter_from`/`keys_from` or backward with `riter_from`/`rkeys_from`.
- **Range deletion** — `del_range(opts, from, to)` deletes all keys in `[from, to)` with a single data file append, whatever the size of the range. Removing the keys from the key directory reads each of them back from its data file (twice, today), so that part grows with the number of keys removed. Available on `DB` and `WritePlan`.
- **Atomic writes** — every `put`, `del`, and `del_range` is atomic. `apply_batch` makes multiple puts, deletes, and range deletes atomic as a group.
- **MVCC transactions** — `snapshot` captures a consistent point-in-time read-only view; `apply_batch(opts, plan)` applies a `WritePlan` atomically only when every precondition holds (**key present / absent / unchanged**, **range unchanged**), returning `nullopt` on conflict. The snapshot is embedded in the `WritePlan` at construction time. When a snapshot is present, every key in the write set is automatically checked for concurrent modification — no explicit guard needed on keys you write. Use `ensure_unchanged` for keys you read but don't write, and range guards for serializable conflict detection. Together they cover the full isolation spectrum: read from a `Snapshot` for **snapshot isolation**, add guards for **serializable** conflict detection, or use bare `put`/`del` for **read-uncommitted** fast paths. Each precondition check is a key directory lookup plus one record read for the key's sequence — no separate transaction type required. Both levels are checked every night with [Elle](https://github.com/jepsen-io/elle) against concurrent transaction histories, with vacuum and injected `fdatasync` failures running: guarded plans come out strict-serializable, and unguarded ones snapshot-isolated, with write skew as their only anomaly. The check covers point reads and writes; range guards and range deletes are not yet part of it ([`docs/isolation_checking_design.md`](docs/isolation_checking_design.md)).
- **Fast recovery** — parallelised index reconstruction from zstd-compressed hint files; from a cold start, with nothing in the page cache, 10 M keys recover in 0.33 s on a SATA SSD.
- **Bounded close and restart** — hint files are written in the background, and the number waiting is capped (`max_hint_backlog`, default 4). When the writer falls behind, writes wait for it rather than letting the backlog grow, so a clean close has at most that many hint files to write, and an open after a crash at most one more to rebuild. The cap can be turned off for bulk loads.
- **Vacuum** — vacuum process to reclaim unused space from overwritten or deleted keys; query performance does not degrade as the database grows. Delete tombstones are reclaimed too: when vacuum compacts a file, it drops each tombstone that no longer hides an older value in another file. Which ones qualify is decided when the database is opened, so a tombstone written since the last open stays until the next one. Vacuum removes history `changes_since` would stream, so under replication it takes `retain_after` from the replication service: the lowest position a follower still needs, above which nothing is dropped. The default restricts nothing.
- **Lock-free multi-reader, single-writer** — reads are lock-free and scale to millions of operations per second. Writes are serialised under a single mutex for their in-memory phase, with group commit: concurrent sync writers share a single `fdatasync` call, amortising the dominant cost. The commit is pipelined: while one flush is in flight, the next batch is validated, applied and appended, so the disk never waits on in-memory work. On the success path, `state_.store()` happens after `fdatasync`, guaranteeing durability before visibility.
- **Crash safety** — CRC-verified entries, atomic hint file generation (`write → fdatasync → rename`), and append-only data files as the primary durable store. Hint files are an index, not the record: one that fails its CRC is rebuilt from its data file at recovery rather than dropped, so a damaged index costs the time to rebuild it and not the keys behind it. A process killed in the middle of a vacuum leaves the file being compacted and its compacted copy side by side; the next open deletes the copy once it has checked that every entry in it is also in the original, and refuses to open on any other pair of files that share sequence numbers. On unrecoverable write-path failures (e.g. isolation rotation fails), the engine enters a degraded state: reads remain available, all writes throw `DbDegraded`, and the service calls `resume()` to recover without a restart. Corruption of data already acknowledged is refused rather than repaired: `resume()` throws instead of cutting the file back to the last entry it can parse, and `open` refuses a data file holding an entry that fails its CRC.
- **Bounded value cache** — `IoBackend::BufferPool` serves data files from a frame cache whose size the operator sets, filled with `O_DIRECT`, for deployments where the dataset far exceeds RAM and the footprint has to be a number rather than whatever the kernel's page cache settles on. Reads stay lock-free; the active file is resident from the moment its bytes are written, and until it is full a miss fills the 128 KiB around it, so a cold pool warms at readahead speed. `capacity_bytes` is the total footprint, and `stats()` reports a hit ratio to size against — something a page cache cannot give. On a resident dataset a hit is ~15 ns behind `mmap` and scales with reader threads the same way; it is off by default, and worth turning on when there is a memory budget to enforce. In the WASM build it is also the fast read path: every `pread` there is a call into Node's `fs`, and a hit skips it.
- **Operational counters** — `stats()` returns a flat `map<string, int64_t>` of monotonic counters (bytes written, fsyncs, group writer batches, vacuum bytes reclaimed, tombstones dropped, hint backpressure stalls, CRC failures, I/O errors, degraded transitions) and gauges (degraded state, open files, hint backlog, live key count, buffer pool hit/miss and residency, key directory versions alive and retired nodes they pin, tombstones vacuum must keep). Designed for pull-based scraping (Prometheus, logging). Counters only track what the engine can see internally — request counts and latency are the caller's responsibility.
- **Replication transport in Python** — Python bindings expose a `DataEntry(sequence, entry_type, key, value)` constructor accepting bytes-like payloads, so `changes_since()` output can be serialized over the wire and reconstructed before `ingest()`.

## Performance

Benchmarked at 1 M keys with [RocksDB](https://rocksdb.org/) as a reference point. The tables below include both engines for context.

- **Point reads** reach 1.29 Mops/s at 1 M keys with flat, sub-microsecond latency (p50 728 ns, p99 1.01 µs). Latency stays flat as the dataset grows because every lookup is an in-memory key directory traversal followed by a single read at a known offset, which confirms the key and returns the value.
- **Concurrent reads scale linearly** — lock-free snapshots with no shared mutex. 14.0 Mops/s at 32 threads.
- **Sequential writes** sustain 134 Kops/s (NoSync) and 139 ops/s (Sync), limited by `fdatasync` round-trip latency. No write amplification from compaction.
- **Concurrent sync writes scale via group commit** — writers share a single `fdatasync` call. 4.9 Kops/s at 64 threads.
- **Range scans over values** fetch each value individually from disk. LSM-based engines pack values contiguously in sorted runs and perform better here. Key-only iteration (`keys_from`) reads each key's record header and skips the value.
- **Recovery is fast and parallel** — compressed hint files replayed across all cores with per-file CRC verification. From a cold start: 1 M keys in 37 ms, 10 M in 0.33 s at 16 threads.

See [`docs/bytecask_benchmark_showcase.md`](docs/bytecask_benchmark_showcase.md) for the full benchmark report with all thread counts, dataset sizes, and hardware details.

---

### Single-Threaded Throughput (1M keys)

> Per-value CRC verification is disabled for read benchmarks; hint file CRC is always verified during recovery.

| Operation | ByteCaskDB | RocksDB | Notes |
|-----------|----------|---------|-------|
| Put (NoSync) | 134 Kops/s | 163 Kops/s | Sequential append on both sides |
| Put (Sync) | 139 ops/s | 148 ops/s | Disk-bound — limited by `fdatasync` round-trip latency |
| Get | 1.29 Mops/s | 431 Kops/s | In-memory key directory lookup; flat latency regardless of dataset size |
| Del (Sync) | 184 ops/s | 2 ops/s | Single tombstone append; no compaction write amplification |
| Range-50 | 28 K scans/s | 68 K scans/s | LSM sorted runs favour sequential value scans |
| MixedBatch (Sync) | 14 Kops/s | 13 Kops/s | Atomic batch with single `pwritev` + `fdatasync` |

At small dataset sizes (50 k keys), all keys fit in RocksDB's block cache and reads are fast on both engines. From 500 k keys onward, block cache misses begin to dominate and the in-memory key directory approach shows its advantage.

### Get Latency (1M keys, CRC disabled)

| Percentile | ByteCaskDB | RocksDB |
|-----------|---------|----------|
| p50 | 728 ns | 2.10 µs |
| p99 | 1.01 µs | 5.37 µs |

Latency stays flat as the dataset grows: every read resolves to a known file offset via the in-memory key directory, so there is no metadata amplification from multiple levels or bloom filter checks.

### Concurrent Reads — `GetMT` (1M keys, CRC disabled)

> Reads are lock-free; each thread holds an immutable snapshot of the engine state.

| Threads | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.34 Mops/s | 749 Kops/s |
| 4 | 4.18 Mops/s | 1.49 Mops/s |
| 8 | 6.25 Mops/s | 3.24 Mops/s |
| 16 | 8.76 Mops/s | 4.79 Mops/s |
| 32 | 14.01 Mops/s | 7.33 Mops/s |

### Concurrent Sync Writes — `PutMT/Sync` (1M keys)

> Group commit: concurrent sync writers share a single `fdatasync` call, amortising the dominant cost. The benefit grows with concurrency — more contending writers means larger batches and fewer `fdatasync` calls per write.

| Threads | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 145 ops/s | 257 ops/s |
| 4 | 314 ops/s | 439 ops/s |
| 8 | 617 ops/s | 741 ops/s |
| 16 | 1.3 Kops/s | 816 ops/s |
| 32 | 2.9 Kops/s | 1.7 Kops/s |
| 64 | 4.9 Kops/s | 3.3 Kops/s |

### Read-While-Writing (1M keys, 1 writer + N readers, Sync, CRC disabled)

> Lock-free readers are unaffected by concurrent writes. Read throughput scales the same whether the database is idle or under write load.

| Readers | ByteCaskDB | RocksDB |
|---:|---:|---:|
| 2 | 2.43 Mops/s | 720 Kops/s |
| 4 | 4.39 Mops/s | 1.32 Mops/s |
| 8 | 6.52 Mops/s | 2.91 Mops/s |
| 16 | 9.40 Mops/s | 4.48 Mops/s |
| 32 | 14.93 Mops/s | 7.20 Mops/s |

### Recovery

Recovery runs when ByteCaskDB opens an existing database: it rebuilds the in-memory key directory by reading compact hint files from disk. Each hint file holds its entries in zstd frames and is verified by a file-level CRC-32C trailer before parsing. This is parallelised across all available CPU cores — each core processes a disjoint set of hint files independently, and the results are merged before the database becomes available.

Measured from a cold start: every file of the database is evicted from the page cache before each open, so the hint files come off the SSD, as after a reboot or on a new node.

| Keys | Threads | Recovery Time | Speedup vs 1T |
|---:|---:|---:|---:|
| 1M | 1 | 206 ms | — |
| 1M | 4 | 63 ms | 3.3× |
| 1M | 8 | 42 ms | 4.9× |
| 1M | 16 | 37 ms | 5.6× |
| 10M | 1 | 2.24 s | — |
| 10M | 4 | 0.63 s | 3.6× |
| 10M | 8 | 0.37 s | 6.1× |
| 10M | 16 | 0.33 s | 6.9× |

The benchmark's keys — prefixed UUID text written in order, with 1-byte values — compress 14×: at 10M keys the hint files hold 675 MB of entries in 47 MB. Keys with less in common compress less (2.4× for random 16-hex-digit ids with realistic values, 1.6× for random binary keys), and a cold start then reads more; [`docs/hint_compression_design.md`](docs/hint_compression_design.md) has the measurements.

---

_Tested on AMD Ryzen 7 3700X (8C/16T), Samsung SSD 860 EVO SATA (463 MiB/s read), 31 GiB RAM. Each result is the mean of 5 runs. Benchmark source: [`benchmarks/engine_bench.cpp`](benchmarks/engine_bench.cpp)._

_The recovery figures were measured on the default blind-leaf key directory, on the machine above, in September 2026. The other figures were measured on the radix key directory, before the B+ tree became the default, and on the `mmap` read path, before `engine_bench` switched its default to the buffer pool. A head-to-head run of both trees on one engine put the B+ tree ahead on reads and batched writes and behind on unsynced single puts, but that run was on different hardware, so the absolute numbers above have not been re-measured on this machine and are not restated here. The blind-leaf tree, now the default, was then measured against the keyed B+ tree on one machine at 1M keys: level on `Get` and `GetMT`, 1.25–1.3× on random 16-byte keys, and 0.89× on unsynced single puts, with the key directory 2–7× smaller ([`docs/blind_leaf_btree_design.md`](docs/blind_leaf_btree_design.md)). Reproduce the keyed trees with [`scripts/compare_engine_bench.py`](scripts/compare_engine_bench.py)._

## Quick Start

```cpp
import bytecask;
using namespace bytecask;

// Open (or create) a database directory.
auto db = DB::open("my_db");

// Single-key operations.
db.put({}, to_bytes("user:1"), to_bytes("alice"));

Bytes out;
bool found = db.get({}, to_bytes("user:1"), out);   // true; value in out
auto del_result = db.del({}, to_bytes("user:1"));    // nullopt if key was absent

// Range deletion — delete all keys in [from, to) with a single disk append.
db.del_range({}, to_bytes("session:"), to_bytes("session:~"));

// Atomic batch — all operations land atomically. Every write returns a
// CommitResult (or nullopt on conflict) carrying the assigned sequence and
// whether fdatasync confirmed it durable before return.
WritePlan plan;
plan.put(to_bytes("user:2"), to_bytes("bob"));
plan.put(to_bytes("user:3"), to_bytes("carol"));
plan.del(to_bytes("user:1"));
(void)db.apply_batch({}, std::move(plan));

// Decrement stock — write keys are checked for conflicts automatically.
auto snap = db.snapshot();
Bytes stock_out;
snap.get({}, to_bytes("stock:widget"), stock_out);
// ... decrement stock count ...
WritePlan plan2{std::move(snap)};
plan2.put(to_bytes("stock:widget"), new_stock);
if (!db.apply_batch({}, std::move(plan2))) {
    // another writer changed stock:widget since our snapshot — retry
}

// Place order at current price — ensure_unchanged guards keys you read
// but don't write. Write keys are checked automatically.
auto snap2 = db.snapshot();
Bytes price_out;
snap2.get({}, to_bytes("price:widget"), price_out);
// ... compute order_total from price ...
WritePlan order{std::move(snap2)};
order.ensure_unchanged(to_bytes("price:widget"));  // reject if price changed
order.put(to_bytes("order:99"), order_total);
if (!db.apply_batch({}, std::move(order))) {
    // price changed since snapshot — re-read price and recompute
}

// Prefix scan — keys and values fetched lazily from disk as it advances.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only prefix scan — reads each key's record header, skips the value.
for (auto& key : db.keys_from({}, to_bytes("user:"))) { ... }

// Reverse scan — descending key order. Starts at last key <= "user:~".
for (auto& [key, value] : db.riter_from({}, to_bytes("user:~"))) { ... }

// Reverse keys-only — descending order.
for (auto& key : db.rkeys_from({}, to_bytes("user:~"))) { ... }
```

> `to_bytes` is a small helper that converts a `std::string_view` to `BytesView`:
> ```cpp
> auto to_bytes(std::string_view sv) -> BytesView {
>     return std::as_bytes(std::span{sv.data(), sv.size()});
> }
> ```

## API Reference

```cpp
namespace bytecask {

struct Options {
    uint64_t max_file_bytes{64 * 1024 * 1024};  // active file rotation threshold (default 64 MiB, hard ceiling: 4 GiB)
    unsigned recovery_threads{4};                // parallelism for hint-file replay at open
    // A hint file is a rebuildable index: one that fails its CRC is
    // regenerated from its data file in both modes and costs no keys. This
    // setting governs what is left — a data file that cannot be rescanned.
    // When true (default): DB::open throws. When false: that file is skipped
    // and the DB opens with the keys recovered from the rest, with a warning
    // on stderr for each one.
    bool fail_recovery_on_crc_errors{true};
    Mode initial_mode{Mode::Leader};             // leader allows normal writes; follower allows ingest
    uint32_t max_key_bytes{4096};                // max key size (hard ceiling: 65,535 — u16 wire format)
    uint32_t max_value_bytes{4 * 1024 * 1024};   // max value size (hard ceiling: 256 MiB — packed KeyDirEntry)
    IoBackend io_backend{IoBackend::Pread};      // how sealed files are read
    BufferPoolOptions buffer_pool{};             // only read when io_backend == BufferPool
    // Most sealed files allowed to wait for their hint file. A rotation that
    // would exceed it waits for the background writer, stalling writes (not
    // reads), so the work left for close — and for the next open after a
    // crash — stays bounded. 0 turns the bound off.
    uint32_t max_hint_backlog{4};
};

// Selects how sealed data files are read. The active file is written the same
// way in every mode.
enum class IoBackend {
    Pread,       // pread(2) per read (default)
    Mmap,        // sealed files memory-mapped; zero-copy reads
    BufferPool,  // sealed files served from a bounded, engine-owned cache
};

struct BufferPoolOptions {
    size_t capacity_bytes{0};          // TOTAL pool footprint, frames plus index.
                                       // Must be >= 2 x max_file_bytes.
    bool direct_io{true};              // O_DIRECT fills: the pool, not the page cache,
                                       // holds sealed-file data. Falls back per file
                                       // where the filesystem refuses.
};

struct WriteOptions {
    bool sync{true};      // call fdatasync after write (default true)
    bool solo{false};     // bypass group commit — route to solo writer (for benchmarking)
};

// A read sees every write that returned before it began, and every write any
// other read has already seen, on any thread.
struct ReadOptions {
    bool verify_checksums{true}; // CRC-verify each value read from disk (default true)
};

struct VacuumOptions {
    double fragmentation_threshold{0.5}; // a file is eligible above this share of dead bytes
    // Nothing above this sequence is dropped, so changes_since from any
    // sequence >= retain_after stays complete. Set by the replication service
    // to the lowest position a follower still needs; kNoRetention (-1), the
    // default, restricts nothing.
    uint64_t retain_after{kNoRetention};
};

enum class Mode { Leader, Follower };

// Outcome of a committed write: the highest sequence assigned (0 = nothing
// written) and whether fdatasync confirmed it durable before return. A
// reader — local or follower — whose durable_sequence() >= sequence is
// guaranteed to see every entry of this write.
struct CommitResult {
    uint64_t sequence{0};
    bool durable{false};
};

class DB {
public:
    // DB is non-copyable and non-moveable; open() relies on mandatory copy elision.
    [[nodiscard]] static auto open(std::filesystem::path dir,
                                   Options opts = {}) -> DB;

    // Writes value for key into out, reusing its capacity. Returns true if found.
    // Throws std::system_error on I/O failure, std::runtime_error on CRC mismatch,
    // or DbDegraded if the engine is degraded.
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;

    // Writes key → value. Overwrites any existing value. Cannot conflict.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    auto put(const WriteOptions& opts, BytesView key, BytesView value) -> CommitResult;

    // Writes a tombstone for key. nullopt if the key was absent — nothing written.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto del(const WriteOptions& opts, BytesView key) -> std::optional<CommitResult>;

    // Deletes all keys in [from, to) with a single data file append. Cannot conflict.
    // Returns {sequence = 0, durable = true} without writing if from >= to.
    // Throws std::system_error on I/O failure or DbDegraded.
    auto del_range(const WriteOptions& opts, BytesView from, BytesView to) -> CommitResult;

    [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                    BytesView key) const -> bool;

    // Atomically applies all operations in plan. nullopt on conflict (guard
    // failure or implicit W-W check) — nothing was written. An empty or
    // guard-only plan that passes commits as a no-op: {sequence = 0, durable = true}.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto apply_batch(WriteOptions opts,
                                   WritePlan plan) -> std::optional<CommitResult>;

    // Returns a frozen, move-only, read-only view of the DB at this instant.
    // Holds open referenced data files until destroyed — vacuum deferred automatically.
    [[nodiscard]] auto snapshot() const -> Snapshot;

    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    [[nodiscard]] auto riter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;

    [[nodiscard]] auto rkeys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

    // Returns true if a file was reclaimed. A file qualifies by the share of
    // its bytes that are overwritten or deleted values; tombstones and batch
    // markers count towards neither side. False when no file qualified or
    // none could be made smaller.
    [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;

    // The single sequence primitive — returns the highest sequence confirmed
    // durable by fdatasync. min_sequence = 0, an already-reached target, or a
    // nonpositive timeout return immediately without blocking. Otherwise blocks
    // until durable_seq >= min_sequence or timeout expires, then returns the
    // watermark. Covers polling (default args), replication wake-up
    // (follower.durable_sequence() + 1), and read-your-own-writes waits
    // (a CommitResult's sequence).
    [[nodiscard]] auto durable_sequence(
        std::uint64_t min_sequence = 0,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) const
        -> std::uint64_t;

    // Rotates the active file, waits for all hint files, and returns a
    // manifest of sealed files with a snapshot. Forces file rotation.
    // Vacuum must not run between create_manifest() and file transfer
    // completion (caller responsibility).
    [[nodiscard]] auto create_manifest() -> FileManifest;

    // Returns the current engine mode (Leader or Follower).
    [[nodiscard]] auto mode() const noexcept -> Mode;
    // Switches mode under the write mutex. No in-flight write straddles the transition.
    // A leader stepping down fdatasyncs first: every write it acknowledged,
    // sync or not, is durable and so shippable by changes_since.
    void set_mode(Mode mode);

    // Applies pre-sequenced entries from a leader. Follower mode only.
    // Idempotent: entries with sequence <= durable_sequence() are skipped.
    // Publishes the slice in one step: cut slices at batch boundaries, or
    // part of an atomic batch becomes visible.
    // Throws std::logic_error if not in follower mode, DbDegraded if degraded.
    void ingest(std::span<const DataEntryView> entries);

    // True if the engine has entered a degraded state from a write-path failure.
    // Reads remain available; all write operations throw DbDegraded.
    [[nodiscard]] auto is_degraded() const noexcept -> bool;
    [[nodiscard]] auto degraded_reason() const noexcept -> const std::string&;

    // Attempts to recover from a degraded state. Scans the active file to find
    // the last valid committed offset, truncates garbage bytes, syncs, seals
    // the file, and opens a new active file. On success, clears the degraded
    // flag. On failure, the engine stays degraded and the caller may retry.
    // It only trims what a failed write left behind: damage found in data
    // already acknowledged throws std::runtime_error and leaves the file
    // untouched — corrupt acknowledged data is refused, not repaired.
    // No-op if the engine is not degraded.
    void resume();

    // Returns all operational counters and gauges as a flat map.
    // Monotonic counters (bytes_written, fsyncs, …) and current-state
    // gauges (degraded, open_files, hint_backlog, keydir_versions_live,
    // keydir_nodes_parked). Designed for pull-based scraping.
    [[nodiscard]] auto stats() const -> std::map<std::string, std::int64_t>;
};

// Frozen, move-only, read-only view of DB state at a point in time.
// Holds open any referenced data files until destroyed.
class Snapshot {
public:
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;
    [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                    BytesView key) const -> bool;
    [[nodiscard]] auto iter_from(const ReadOptions& opts,
                                 BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
    [[nodiscard]] auto keys_from(const ReadOptions& opts,
                                 BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;
    [[nodiscard]] auto riter_from(const ReadOptions& opts,
                                  BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;
    [[nodiscard]] auto rkeys_from(const ReadOptions& opts,
                                  BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;
};

// Sealed file descriptor returned by create_manifest().
struct FileInfo {
    std::uint32_t file_id;
    std::filesystem::path data_path;
    std::filesystem::path hint_path;
};

// Manifest of sealed files with a point-in-time snapshot.
// Returned by DB::create_manifest().
struct FileManifest {
    Snapshot snap;                       // frozen read-only view
    std::vector<FileInfo> files;         // sealed data + hint files
    std::uint64_t through_sequence{0};   // last sequence covered
};

// Write plan for apply_batch. Groups multiple operations into a single atomic write.
// Construct with WritePlan(snap) to enable ensure_unchanged / ensure_range_unchanged guards;
// those methods throw std::logic_error if called on a snapshot-less WritePlan().
// When a snapshot is present, apply_batch automatically rejects the plan if any
// write key (put or del) changed since the snapshot — no explicit guard needed on
// keys in the write set. Use ensure_unchanged for read-only dependencies: keys whose
// value influenced the plan but that the plan does not modify.
// A guardless, snapshot-less WritePlan always commits successfully (no conflict possible).
class WritePlan {
public:
    WritePlan();                         // snapshot-less: only ensure_present/ensure_absent available
    explicit WritePlan(Snapshot snap);   // snapshot embedded; all guards available

    void put(BytesView key, BytesView value);
    void del(BytesView key);
    void del_range(BytesView from, BytesView to);  // range delete: [from, to)

    void ensure_present(BytesView key);                         // guard: key must exist
    void ensure_absent(BytesView key);                          // guard: key must be absent
    void ensure_unchanged(BytesView key);                       // guard: key unchanged since snapshot
    void ensure_range_unchanged(BytesView from, BytesView to);  // guard: no key change in [from, to)

    [[nodiscard]] auto has_snapshot() const noexcept -> bool;
};

// Non-owning view of a data entry. Used by changes_since (read path) and
// ingest (write path). The owning counterpart is DataEntry.
struct DataEntryView {
    std::uint64_t sequence;
    EntryType entry_type;
    std::span<const std::byte> key;
    std::span<const std::byte> value;
};

// Thrown by write operations when the engine is in a degraded state.
class DbDegraded : public std::runtime_error { /* ... */ };

// Thrown by put/del/del_range/apply_batch when the engine is in follower mode.
class DbFollowerMode : public std::runtime_error { /* ... */ };

} // namespace bytecask
```

Error handling follows the throw-on-failure convention used by the C++ standard library: I/O failures throw `std::system_error`; data corruption throws `std::runtime_error`; an internal invariant violation that cannot be continued through safely — currently only a reused data file name, which would silently drop writes at recovery — prints to stderr and aborts the process rather than throwing, since the write path would otherwise catch it and retry into the same corrupt state; write operations on a degraded engine throw `DbDegraded` (a `std::runtime_error` subclass, catchable separately); normal writes in follower mode throw `DbFollowerMode`. Key-not-found is signalled by `get` returning `false`; `apply_batch` (and `del`) return `nullopt` on precondition or W-W conflict — conflicts are expected outcomes, not exceptional errors. Every committed write returns a `CommitResult{sequence, durable}` — a wait-friendly token for read-your-own-writes across replication (see `durable_sequence` above).


## Architecture

### Design Principles

ByteCaskDB is designed around four core tenets, in priority order:

1. **Correctness** — data integrity above all else.
2. **Simplicity** — few moving parts; the design is easy to understand and maintain.
3. **Predictable latency over peak throughput** — bounded, flat write latency at every scale. A steady 1 ms per write is preferable to an average 0.1 ms with occasional 500 ms spikes.
4. **Performance** — optimisations require a real use case. Without one, correctness and simplicity take priority.

### Components

```
  ByteCaskDB
  ├── Key Directory  PersistentBlindBTree               (every key, in memory, 12 bytes per leaf entry)
  ├── File Registry  map<file_id, DataFile>             (open file descriptors)
  ├── Active File    append-only .data file             (current writes)
  └── Sealed Files   read-only .data + .hint files      (older segments)
```

**Write path**: all writes route through a single coordinator (`apply_batch`). Concurrent sync writers are batched via group commit — a writer that finds no leader becomes one and executes every pending write under one lock hold with a single `fdatasync`, then returns to its caller and hands leadership to the next waiting writer, so no client's commit is held to serve others. Each write appends CRC-32-verified, length-prefixed records to the active data file, then applies pure in-memory state transitions via `TransientEngineState`. The active file is zero-filled 4 MiB ahead of the write cursor, so each commit's `fdatasync` is a data-only flush with no filesystem journal traffic; the commit that crosses into a new 4 MiB chunk pays for flushing it. The key directory builder used for those transitions is single-use: it freezes into the published immutable snapshot and fails fast on accidental reuse. Durability before visibility: `state_.store()` happens after `fdatasync`.

**Read path**: readers obtain an immutable snapshot of the engine state, look up the key in the key directory to find its file and offset, then read the record: one read confirms the key (the leaf holds only its fingerprint) and returns the value. Reads are lock-free and scale linearly across cores.

**Recovery**: on `open`, the engine generates a hint file for any data file that lacks one (including the most recent active file), then replays all hint files in parallel to rebuild the key directory. Hint files are compact per-file indexes, sorted by key and compressed with zstd, written atomically (`write → fdatasync → rename`) by a background worker after each file rotation and synchronously at engine close. No raw data-file scan is performed — recovery reads only hint files.

Start with [`docs/bytecask_intro.md`](docs/bytecask_intro.md) for a first-pass happy-path overview, then use [`docs/bytecask_design.md`](docs/bytecask_design.md) as the full design reference.

## Building

ByteCaskDB requires **Clang** (with C++23 modules support), [xmake](https://xmake.io), and Python tooling (`nanobind`) when building the Python extension target.

```bash
# Build and run the test suite.
xmake build 
xmake run bytecask_tests

# Build benchmarks (optional; requires RocksDB).
python ./scripts/run_engine_bench.py

# Benchmark the MariaDB plugin with a selected sysbench workload.
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh \
  --engines=bytecaskdb,innodb --workloads=oltp_insert --threads=1,16

# TPROC-C (TPC-C derived) through HammerDB, ByteCaskDB vs InnoDB.
./bytecaskdb-mariadb-plugin/benchmarks/run-hammerdb.sh --warehouses=20 --vus=8,16
```

The key directory is chosen at build time. The default is the blind-leaf B+ tree; `BYTECASK_KEYDIR=btree` builds the engine on the B+ tree that keeps key bytes in its leaves (larger, and it never reads a record to place or enumerate a key), and `BYTECASK_KEYDIR=radix` on the radix tree. All three pass the same engine suite in CI, and the on-disk format is the same, so a database opens under any of them.

A ready-to-use development environment is provided via the included [Dev Container](.devcontainer) (Fedora 43, Clang, xmake, LLVM tooling, and `nanobind` pre-installed).

## Want to hack on it?

ByteCaskDB is early-stage and there's plenty of room to explore — new features, performance ideas, test coverage, documentation, or just poking around the internals. All of it is welcome.

See [CONTRIBUTING.md](CONTRIBUTING.md) to get started. The fastest path is to open it directly in GitHub Codespaces — no local setup required.

If you want to take it in a different direction and fork it into your own thing, go for it — that's what the MIT license is for.

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/bytecask_design.md`](docs/bytecask_design.md) | Living design reference: architecture, concurrency model, file format, vacuum, recovery |
| [`docs/file_format.md`](docs/file_format.md) | On-disk file format reference: data file entries, hint file entries, CRC, byte order, naming |
| [`docs/engine_api_design.md`](docs/engine_api_design.md) | Public API specification with usage examples |
| [`docs/parallel_recovery_design.md`](docs/parallel_recovery_design.md) | Parallel recovery algorithm and fan-in merge strategy |
| [`docs/hint_compression_design.md`](docs/hint_compression_design.md) | zstd-framed hint files: why cold-start recovery is bound by the SSD, the frame layout, compatibility, and measurements |
| [`docs/blind_leaf_btree_design.md`](docs/blind_leaf_btree_design.md) | Blind-leaf B+ tree design — the key directory: leaves without key bytes, fingerprint lookups, measurements |
| [`docs/persistent_btree_design.md`](docs/persistent_btree_design.md) | Persistent B+ tree design — the inner nodes of the key directory, and the keyed tree selectable with `BYTECASK_KEYDIR=btree` |
| [`docs/persistent_radix_tree_design.md`](docs/persistent_radix_tree_design.md) | Persistent radix tree data structure design — the alternate key directory (`BYTECASK_KEYDIR=radix`) |
| [`docs/correctness_validation.md`](docs/correctness_validation.md) | Write-path correctness validation: failure classes, proof test matrix, fault injection framework |
| [`docs/failure_mode_comparison.md`](docs/failure_mode_comparison.md) | Write-path failure mode comparison: ByteCaskDB vs RocksDB, LevelDB, SQLite WAL, LMDB, WiredTiger |
| [`docs/replication_primitives_design.md`](docs/replication_primitives_design.md) | Replication primitives: minimal API surface for building leader-follower replication on top of ByteCaskDB |
| [`docs/buffer_pool_design.md`](docs/buffer_pool_design.md) | Buffer pool: bounded, `O_DIRECT`-filled value cache for memory-constrained deployments, with measurements |
| [`docs/xa_support_design.md`](docs/xa_support_design.md) | XA / two-phase commit: generic 2PC primitives (`BulkPrepare`, `Bulk2PCCommit`, `Bulk2PCRollback`) for external coordinators |
| [`bytecaskdb-node/`](bytecaskdb-node/) | Node.js package: WASM (Embind) and native (N-API) backends behind one TypeScript API |
| [`CONTRACT.md`](CONTRACT.md) | Per-function behavioral contracts: atomicity, durability, I/O failure safety, sequence invariants |

## License

ByteCaskDB is dual-licensed:

- **MIT** — core engine (`src/`, `include/`, `tests/`, `benchmarks/`). See [`LICENSE`](LICENSE).
- **GPL-2.0-only** — MariaDB plugin (`bytecaskdb-mariadb-plugin/`). Required by the MariaDB plugin API.

See [`docs/project_organization.md`](docs/project_organization.md) for the full license boundary and rationale.
