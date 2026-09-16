# Buffer Pool Design — V0

> **Status: proposal.** No code exists. This describes the smallest buffer pool worth building: a third `DataFile` back-end alongside `pread` and `mmap`. Alternatives considered and deferred are listed in §10.

---

## 1. Why

**The engine's foundational bet is that all keys live in RAM.** The key directory is not a cache — it is mandatory. At roughly 50 bytes per key that is ~5 GB at 100 M keys and ~50 GB at 1 B. If any of it is not resident, a lookup becomes a page fault, and the flat sub-microsecond latency that is the whole premise degrades into disk I/O *to read the index* — worse than the B-tree engines the design exists to improve on.

**Nothing currently guarantees the key directory that memory.** The page cache is an unbounded competitor for the same RAM, and the kernel arbitrates between them knowing only that one is anonymous and the other is file-backed — not that one is load-bearing and the other is discretionary.

The failure is usually not dramatic. Without swap, page cache is reclaimed before anonymous memory, so the tree mostly survives; what you get is reclaim churn and **direct-reclaim stalls in the allocation path**. And the key directory grows: every `put` of a new key allocates radix tree nodes, so when the page cache holds all otherwise-free memory, a write can stall synchronously inside reclaim — a latency spike with no cause visible from inside the engine. Where swap exists, the sharper failure is available too.

**This is the inversion worth noticing.** A B-tree engine's buffer pool caches index and data together and can evict either under pressure. Here the index cannot be evicted at all, so bounding the value cache is the only way to protect it. This engine needs an explicit memory budget *more* than InnoDB does, not less.

A buffer pool makes the division explicit: the key directory takes what it needs, values get a fixed budget, and the total is known rather than negotiated by the kernel.

Three secondary things follow from owning the cache rather than borrowing it:

- **Granularity.** The kernel caches 4 KiB pages with its own readahead. We already fight this with `POSIX_FADV_RANDOM` / `MADV_RANDOM`.
- **Content.** The page cache stores raw blocks, so it also caches superseded entries — overwritten and tombstoned records that stay in a file until vacuum rewrites it.
- **Visibility.** `stats()` cannot distinguish a read served by the page cache from one that hit the device. There is no hit ratio to size against or alert on.

### `O_DIRECT` is the mechanism, not an optimization

A pool filled through buffered `pread` **bounds nothing** — it puts a bounded cache in front of an unbounded one, and the page cache goes on growing and competing with the key directory. Only with `O_DIRECT` is the pool the sole consumer of memory for file data, which is what makes the total `key directory + pool + slack`.

Phase 1 (§11) measures the pool's own cost with buffered fills, which is worth doing on its own. It does not deliver the goal. Phase 2 does.

### What makes this tractable

An append-only store does not need a real buffer pool, only a cache. Pages are never modified in place, so there is no dirty bit, no flush list, no background writer, no checkpoint, no WAL ordering constraint on eviction, and no torn-page protection. What remains is a hash table over fixed frames, an eviction policy, and a fill path.

## 2. Fit with the `DataFile` contract

The contract is unchanged. Of the five methods:

- **`read_value`** copies into `out`. Every existing back-end already does this, including mmap. The pool copies out of frames instead. This is `DB::get` — the dominant path.
- **`scan`** returns an owning `DataEntry`. Nothing to resolve.
- **`size`** is bookkeeping.
- **`read_entry` / `read_entry_unverified`** return `DataEntryView` spans. mmap points them into its mapping; `pread` points them into the caller's `io_buf`. **A pool frame can be recycled while a caller still holds a span into it** — and not only by another thread: an `EntryIterator` span stays live across the user's loop body, which can itself issue a read that misses and evicts that very frame.

  **The pool copies into the caller's `io_buf`**, exactly like `ReadOnlyPosixDataFile`. The documented rule — *"spans are valid until the next `operator++()`"* — holds unchanged. The copy is tens of nanoseconds against a saved syscall or device read.

### Blast radius

| Path | Uses the pool? |
|---|---|
| `DB::get`, `Snapshot::get` | Yes, via `read_value` |
| `iter_from` / `riter_from` | Yes, via `read_entry` |
| `keys_from`, `rkeys_from`, `contains_key` | No — pure radix tree |
| `get` on a tombstone or empty value | No — short-circuits before the registry |
| Vacuum, `create_manifest`, `scan_committed` | Via `scan` — **bypasses the pool** (§7) |
| Recovery | No — reads hint files |

---

## 3. Shape

```
Arena        one aligned allocation, fixed 4 KiB frames, free list
Index        shards; shard = hash(file_id, frame_index) % N
             each shard owns its table, free list, clock hand and lock
Key          (file_id, frame_index)
Validity     per-frame (file_id, generation); stale generation == miss
Eviction     CLOCK, test-then-set reference bit
Active file  fully resident and never evictable (§5)
Sealed files O_DIRECT fills (§6)
CRC          unchanged — verified per entry, per read (§4)
Scans        bypass (§7)
Disabled     buffer_pool_bytes == 0 behaves exactly as today
```

4 KiB because it is the `O_DIRECT` alignment unit and the device transfer minimum, so read amplification over a bare `pread` is approximately zero — the kernel already reads a page minimum today.

An entry spanning several frames is resolved as several independent lookups; contiguous misses coalesce into one fill. There is no size threshold and no bypass, with one exception: an entry whose extent exceeds a fixed fraction of the pool (proposed: one eighth) is read directly into `out` and never admitted, because admitting it would evict the working set to hold a single value. That is an invariant derived from capacity, not a tuning knob, and a counter surfaces it.

Per-frame metadata is roughly 32 bytes — `file_id`, `frame_index`, `generation`, seqlock version, reference byte, flags — about 1 % over a 4 KiB frame.

---

## 4. CRC — unchanged in V0

Entries are CRC-verified per read, exactly as today. `ReadOptions::verify_checksums` keeps its current meaning.

Verifying once on the disk→RAM transfer instead is attractive — a CRC over a ~139-byte entry does not reach the vectorized path, so it costs 50–100 ns, comparable to the lookup and memcpy that make up the rest of a hit. But it cannot be done at fill time: the CRC is per entry, entries straddle frames, and entry starts cannot be located inside an arbitrary frame. Doing it properly needs per-frame verified-extent bookkeeping and a rule that corruption in an entry nobody asked for must not fail the caller's read. That is real complexity for a second-order win, and it changes what `verify_checksums` means. Deferred to §10.

---

## 5. The active file — resident and pinned

**The active file is always created empty at startup**, so every byte in it was written by this process. The writer already holds those bytes after `pwritev`. So it populates the cache as it appends, and **the whole active file stays resident until the file is sealed.**

Three things follow.

**Reads of the active file never touch disk.** Full residency is an invariant, not best-effort. This removes the `O_DIRECT` coherency hazard entirely: the one file being written with buffered `pwritev` is also the one file we never read from disk, so buffered writes and direct reads never meet.

**The active file warms the cache for free.** Recently written data is the hottest data in most workloads — read-your-own-writes, session data, event streams. It arrives in the cache without a single read I/O, and without a CRC, since the writer computed those CRCs when it constructed the entries.

**Sealing is the release.** CLOCK skips any frame whose `file_id` is the current active file. On rotation, `active_file_id` changes and the previous file's frames become evictable immediately — no sweep, no bookkeeping, one comparison in the victim check.

Two details:

- **The tail frame is admitted while still being appended into.** This is safe without a latch: appended bytes are never modified, and a reader can only address an offset that has been published, which happens after the bytes are written (`state_.store()` follows `fdatasync`). Reader and writer never touch the same bytes.
- **Pinned bytes are the active file's *current* size**, growing from zero to `max_file_bytes` and released at seal — so the average pinned cost is about half the rotation threshold.

**Sizing constraint:** the pool must be meaningfully larger than `max_file_bytes`, or the active file alone consumes it. `DB::open` should reject `buffer_pool_bytes < 2 × max_file_bytes` with a clear error rather than degrade silently.

---

## 6. Sealed files — `O_DIRECT`

Sealed files are immutable, so a cached frame from one can never be stale and no invalidation is needed at the seal transition — the frames already hold what was written. The file is simply reopened with `O_DIRECT` for later misses.

Alignment falls out of the frame size: the arena is page-aligned, frames are 4 KiB, offsets are `frame_index × 4096`. Discover the real requirement with `statx(STATX_DIOALIGN)` where available, falling back to the logical block size.

`posix_fadvise(POSIX_FADV_DONTNEED)` on a file after it is sealed — therefore after `fdatasync`, so its pages are clean — releases its page-cache residency. That recovers most of the memory benefit without touching the write path.

**`O_DIRECT` is never applied to the write path.** Appends are `pwritev` at arbitrary offsets gathering unaligned caller buffers; aligning them would need a bounce buffer, destroying the zero-copy gather, and would fight `ensure_zeroed`. It is also not a durability primitive — `fdatasync` is still required.

**Portability:** unavailable on macOS (`F_NOCACHE` is the analogue), unavailable under Emscripten, and **rejected by tmpfs with `EINVAL`**, which matters because CI often runs on it. The back-end must detect this at open and fall back to buffered fills, recording that it did.

---

## 7. Everything else

**Scans.** `scan()`, `scan_committed()` and `create_manifest()` sweep whole files once. Admitting those frames would flush the working set on every vacuum pass. Scan paths bypass the pool and use buffered sequential `preadv` with `FADV_SEQUENTIAL` ahead and `FADV_DONTNEED` behind, leaving neither cache polluted.

**Vacuum.** Unlinking a file bumps its `file_id` generation; its frames become lazy misses and are recycled on contact. O(1), no sweep.

**`truncate()`** — called by `resume()` — bumps the generation the same way.

**Snapshots and rotation.** Snapshots hold files open, so nothing they reference can be unlinked. `file_id` is stable across registry snapshots, so rotation invalidates nothing.

**Recovery** reads hint files and is untouched.

**Emscripten.** No `O_DIRECT`, no page cache to bypass — MEMFS is already memory. The pool is rejected at open there, the way `use_mmap` is today, rather than silently ignored.

---

## 8. Not losing the read scaling

14.0 Mops/s at 32 threads is the strongest number in the README and the thing most at risk, since the pool puts shared mutable state on a lock-free read path. Three rules:

1. **Shard everything.** One pool-wide lock, free list or clock hand is a serialisation point. Each shard owns its own.
2. **Optimistic reads, not pin counts.** A refcount makes 32 readers of one hot frame take that cacheline exclusive, serialising them however short the critical section. Instead: read the frame's version, memcpy out, re-read the version; on mismatch retry, or fall back to a direct `pread`, which is always correct. This is why §2 chose copy-out — the two decisions are one.
3. **No unconditional writes on a hit.** Test-then-set the reference bit, so a hot frame writes nothing after the first touch.

These are hypotheses, not claims. They have to be measured with `GetMT` at 2–32 threads against both existing back-ends, with a working set that does not fit the pool.

---

## 9. Scope — when to turn it on

This is opt-in, and the default stays `pread` / `mmap`.

**The pool cannot beat `mmap` on a resident hit, structurally.** `ReadOnlyMmapDataFile` resolves a read to `mmap_base_ + offset` — an addition and a memcpy. The pool computes a frame index, hashes it, probes a table, checks the generation and takes a seqlock reading first: two or three extra cache misses on every hit. Against a resident dataset with no memory pressure, the pool is slower than what already ships. It beats `pread` (no syscall) and loses to `mmap`.

That is not an argument against building it — it is the same trade every buffer pool makes, and the reason they are all sized explicitly rather than enabled by default. What it buys is what a buffer pool is for:

- **Bounded, enforced residency.** The pool decides what stays. Under a cgroup limit, where page cache is charged to us and reclaimed on the kernel's schedule, nothing else here can make that promise.
- **A hit ratio.** `disk_reads` cannot distinguish a page-cache hit from a device round trip, so there is currently no number to size against or alert on. A pool knows when it misses.

Two secondary effects push the same way at scale: a 100 GiB mapping needs roughly 25 M page-table entries and lives in TLB-miss territory under random access, where a huge-page arena needs 512× fewer; and for the MariaDB plugin, an explicit pool size is the knob operators already know how to reason about.

**Set `buffer_pool_bytes` when there is a memory budget to enforce. Leave it at zero otherwise.**

### Real risks, to be measured

- **Miss latency has no floor.** Today a "miss" often still hits the page cache. With `O_DIRECT` it is a device round trip. Mean throughput can improve while p99 regresses, which tenet 3 counts as a regression.
- **32-thread read scaling.** 14.0 Mops/s is the strongest number in the README, and §8 is a set of hypotheses about not losing it, not a set of claims.
- **Density.** A block cache stores headers, keys and superseded entries, exactly as the page cache does. §10 quantifies what a format-aware cache would save.
- **Sharding costs hit ratio.** Capacity cannot move between shards — a deliberate trade of hit ratio for scaling, and both should be measured.

### Benchmarking consequence

Comparing the pool against `mmap` on the current benchmarks measures the regime where it should be switched off, and will show it losing. The only comparison that means anything runs under memory pressure: the `pool_bytes / dataset_bytes` sweep in Phase 0, with p50 and p99, is the benchmark — not an addition to it.

## 10. Deferred — the A/B backlog

Each of these is a real option that V0 does not take. They are listed so that the choice is recorded, not rediscovered.

**Entry cache instead of a block cache.** Cache `(file_id, offset) → entry`, not file blocks. Makes CRC-on-fill trivial, holds no superseded entries, and avoids storing headers. Not taken because the density case does not survive the allocator: counting ~11 % internal fragmentation on a size-class ladder and ~20 B per-object metadata, a 16 B/16 B entry costs 55.5 bytes against the block cache's 51 — *worse*, and by the same margin at 1 KiB values. What survives is the neighbour term: with `u` the fraction of a frame's entries read before eviction, an entry cache has roughly `0.9 / u` of the block cache's effective capacity, crossing over near 90 % utilization. That favours it at low `u` — but size classes also fragment the eviction policy into orderings that cannot be compared (memcached's documented pathology), and that cost has no number attached. **Block cache wins on failure-mode asymmetry: its downside is a constant factor recoverable with RAM; slab calcification is a pathology.** It is also the instrument that measures `u`, which is the number that would justify replacing it. Revisit if measured `u ≤ 0.3`.

**CRC verified on fill.** Per-frame `verified_from` / `verified_to`, with a forward walk on fill (never on the hit path — that would put ~1 µs of speculative CRC on a ~150 ns operation). Worth ~25–40 % of hit latency. Deferred for complexity and because it changes the meaning of `verify_checksums`.

**Extent chaining.** Group the frames of one multi-frame entry so they are admitted and evicted together. Partial residency of a large entry saves little, since any missing frame still costs a round trip. V0 treats frames independently; most entries occupy one or two.

**Batched fills and `io_uring`.** The fill interface should take a batch from day one — a single fill is a batch of one — so `preadv`, `O_DIRECT` and `io_uring` are implementations of one seam. `io_uring` pays only with many reads in flight, which happens in exactly one place: `iter_from` knows its next N keys with no I/O, so a scan can issue N fills at once. That is a direct attack on Range-50, the one benchmark we lose to RocksDB.

**Other eviction policies.** S3-FIFO (scan-resistant, FIFO insertion) and W-TinyLFU (best hit ratios, most metadata). LRU only as a reference baseline — it needs list surgery under a lock on every hit, which is the worst possible fit for a lock-free read path.

**Liveness-biased eviction.** Prefer victims from files with a low live ratio, which `file_stats` already tracks. Reclaims superseded-entry waste after the fact rather than never admitting it.

**Process-wide pool.** V0 is per-DB, matching `Options`. A shared pool matters for the MariaDB plugin, where every table would otherwise carve out its own arena.

---

## 11. Options, counters, phases

### Options

`Options::use_mmap` is a `bool` at its limit. Replace it with `enum class IoBackend { Pread, Mmap, BufferPool }` — mechanical, but it touches the C ABI shim, the C++ header shim, the Python and Node bindings, the MariaDB sysvar, and the tests that `GENERATE(false, true)`. Worth doing before there is a third value.

```cpp
struct BufferPoolOptions {
  std::size_t capacity_bytes;          // 0 = disabled
  std::size_t frame_bytes;             // default 4096
  unsigned    shards;
  unsigned    oversize_guard_divisor;  // entry > capacity/N is never admitted
  bool        direct_io;
  bool        huge_pages;
};
```

### Counters

Onto the existing `Counters`, surfaced by `stats()`:

```
pool_hits, pool_misses, pool_fills, pool_fill_bytes,
pool_evictions, pool_oversize_reads, pool_multi_frame_reads,
pool_frames_resident, pool_frames_pinned, pool_frames_total,
keydir_bytes_estimate,
pool_optimistic_retries, pool_direct_io_fallbacks
```

Hit ratio is the primary A/B metric. `keydir_bytes_estimate` is what lets an operator size the pool as *total − key directory − slack*, which §1 makes the point of the exercise; without it the budget cannot be computed from outside. `pool_optimistic_retries` is the early warning for §8.2. `pool_direct_io_fallbacks` catches tmpfs degrading silently in CI.

### Correctness

`tests/data_file_test.cpp` and `tests/bytecask_test.cpp` already `GENERATE(false, true)` over `use_mmap`; widening that to the three back-ends runs every existing test body against the pool unchanged. This is the main reason §2 keeps the contract byte-identical. Add a differential test — random reads against a pool-backed file compared byte-for-byte with `ReadOnlyPosixDataFile` as oracle, at pool sizes small enough to force constant eviction. The `[model]` recovery tests need extending if anything here changes what a recovered DB contains; as designed it does not, but the active-file insert-on-write path and `truncate()` invalidation sit close enough to the write path to warrant coverage.

### Phases

| Phase | Content | Answers |
|---|---|---|
| 0 | Back-end enum, counters, `u` and `L` instrumentation, a benchmark sweep whose working set exceeds RAM | What is the baseline, and can we even see the regime where a cache matters? |
| 1 | Pool with buffered `preadv` fills, CLOCK, sharded, copy-out | Does it beat the page cache while still using it? Does 32-thread scaling survive? |
| 2 | `O_DIRECT` on sealed files, `FADV_DONTNEED` after seal | What does bypassing the page cache buy, and cost at p99? |
| 3 | Active-file residency and pinning | Does read-your-own-writes improve on a write-heavy mix? |

The benchmark sweep in Phase 0 is not optional. Every current benchmark fits in RAM, where a cache can only lose; the interesting axis is `pool_bytes / dataset_bytes` at 0.1×–2.0×, with p50 and p99, not throughput alone.

**Phase 0 is worth building whether or not the pool follows.** The missing hit-ratio visibility and the missing out-of-RAM benchmark are gaps today.

---

## 12. Open

1. **What is the acceptance bar for Phase 1?** It should be a hit ratio and a p99 at a stated working-set ratio, not a throughput number on benchmarks that cannot see the regime. Agree it before Phase 1, not after.
2. **Does measured `u` reopen Axis A?** See §10. `u ≥ 0.9` closes it permanently; `u ≤ 0.3` reopens the storage layer, and only the storage layer.
3. ~~Is the `resume()` / `truncate` mmap span hazard real?~~ Filed as [#87](https://github.com/gustavoamigo/bytecaskdb/issues/87). Pre-existing, independent of this work.
