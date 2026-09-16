# Buffer Pool Design — V0

> **Status: proposal.** No code exists. This describes the smallest buffer pool worth building: a third `DataFile` back-end alongside `pread` and `mmap`. Alternatives considered and deferred are listed in §10.

## Goal

**This exists for memory-constrained deployments** — a container with a `memory.max`, a shared host, a tuned appliance — where the dataset is far larger than available RAM and the memory the engine uses has to be **bounded and configurable**.

In those setups the operator needs to divide RAM explicitly between two things with very different requirements: the **key directory, which must be fully resident** and is not a cache, and the **value cache, which must not be allowed to grow without limit**. Today the kernel makes that division, using a page cache that is an unbounded competitor for the same memory, and it makes it without knowing which of the two is load-bearing. §1 works through what that costs.

So the deliverable is a single number the operator sets and the engine honours: `capacity_bytes`, meaning total footprint (§3). Everything else here — frame size, eviction policy, `O_DIRECT`, the pinned active file — exists to make that number meaningful.

**Non-goals.**

- **Making a dataset that already fits in RAM faster.** It cannot: the pool loses to `mmap` on a resident hit, structurally (§9). Where memory is not constrained, leave it off.
- **Improving throughput on the current benchmark suite.** Every benchmark today runs in the regime where the pool should be disabled. Measuring it there measures overhead — see the benchmarking note in §9.

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

### What the contract does not provide

The five methods are enough to *serve* a read from the pool. They are not enough to *build* one, because two things it needs are held by the engine rather than by the file.

**The frame key.** Frames need a per-file identifier, and the engine's `file_id` is not available where files are opened: `openDataFileForRead` takes neither an id nor a pool handle, four of its five call sites have the id in hand, and **vacuum does not** — it opens the compacted file before `apply_vacuum` mints the id. That left a choice between reordering vacuum and giving a shared, lock-free-read type a mutable field.

*Resolved — reserve before open.* Frames are keyed by the engine's `file_id`, and vacuum reserves its destination id before opening the compacted file, via `TransientEngineState::reserve_file_id()` under a short write barrier; `apply_vacuum` then consumes the reserved id instead of minting one. The barrier is what keeps the reservation disjoint from the id rotation mints on the same counter — reading `next_file_id_` outside it would race. A reserved id that is never used leaves a gap, which is harmless: ids are monotonic within a process and are reassigned from scratch at recovery.

A pool-local cache id was tried first and rejected. It avoided the extra barrier, but it made the frame key a second identity for the same thing, and §10's liveness-biased eviction wants to pick victims by `file_stats`, which is keyed by `file_id` — a pool-local key would need a side map to get back there. One identifier is worth one barrier on a vacuum path that already takes one.

The cost is honest: vacuum now takes two write barriers instead of one, so it quiesces writers twice per compaction.

**A shared owner.** The pool is per-DB state, so the factory has to carry a pointer to it, which makes `openDataFileForRead` a member or a DB-bound factory rather than the free function it is today.

Neither is deep, but together they mean the pool is not a drop-in third back-end: the seam has to move before there is anything to put behind it. That is Phase 0 work, and it is worth landing while the answer is still "there is no pool".

---

## 3. Shape

```
Arena        one aligned allocation, fixed 4 KiB frames, free list
Index        one shared open-addressed table; lock-free reads (acquire loads)
             striped locks (~64) on the fill/evict path only
Clock        one global hand, advanced by CAS; never touched by a reader
             reference bit in-band in the control byte, test-then-set
Key          (file_id, frame_index)
Eviction     CLOCK, test-then-set reference bit
Active file  fully resident and never evictable (§5)
Sealed files O_DIRECT fills (§6)
CRC          unchanged — verified per entry, per read (§4)
Scans        bypass (§7)
Disabled     capacity_bytes == 0 behaves exactly as today
```

4 KiB because it is the `O_DIRECT` alignment unit and the device transfer minimum, so read amplification over a bare `pread` is approximately zero — the kernel already reads a page minimum today.

An entry spanning several frames is resolved as several independent lookups; contiguous misses coalesce into one fill. There is no size threshold and no bypass, with one exception: an entry whose extent exceeds a fixed fraction of the pool (proposed: one eighth) is read directly into `out` and never admitted, because admitting it would evict the working set to hold a single value. That is an invariant derived from capacity, not a tuning knob, and a counter surfaces it.

Per-frame metadata is roughly 32 bytes — `file_id`, `frame_index`, seqlock version, reference byte, flags — about 1 % over a 4 KiB frame.

**Frames hold atomic words, not plain bytes.** A reader copying out of a frame runs concurrently with a fill writing into it. The seqlock decides whether the bytes the reader got are *usable*, but two plain `memcpy`s racing is a data race and therefore UB whatever the version check later proves — tenet 1 counts that as a correctness failure, not a theoretical one. The copy is done with relaxed atomic 64-bit loads and stores, which are well-defined and compile to plain `mov`s on x86. ThreadSanitizer flagged the original `memcpy` pair; this is the fix. There is no `generation` stamp: §7 shows file ids are never reused within a process, so there is nothing to invalidate.

### The bound is the contract

`capacity_bytes` is the whole point of the subsystem, so it means **total pool footprint**, not the frame bytes with overhead added on afterwards. Configure 1 GiB, observe 1 GiB of resident memory. `innodb_buffer_pool_size` is widely criticised for the opposite behaviour, where actual RSS exceeds what the operator asked for.

Frame count is therefore derived from the budget rather than the budget from the frame count:

```
frames = capacity_bytes / (frame_bytes + per_frame_metadata + table_share)
```

Everything the pool owns comes out of that number:

| Inside the bound | Notes |
|---|---|
| Frame arena | the bulk |
| Per-frame metadata | ~32 B per frame |
| Hash table | open-addressed, ~`frames / 0.7` entries |
| Per-shard free lists, clock hands | small, but counted |
| **The pinned active file** | up to `max_file_bytes` — see below |

**The pinned active file is inside the budget, not additional to it.** §5 keeps it fully resident and non-evictable, which could be read as a separate allocation. It is not: as the active file grows, the cache available for sealed data shrinks, and at rotation it is released. If it were outside, the true total would be `capacity_bytes + max_file_bytes` and §1's argument would not hold.

That is what makes the sizing check mandatory rather than advisory: `DB::open` rejects `capacity_bytes < 2 × max_file_bytes`, because below that the active file alone consumes half the pool.

The floor scales with the *configured* `max_file_bytes`, not with the rotation ceiling: at the 64 MiB default it is 128 MiB, and only a database configured at the 4 GiB ceiling needs 8 GiB. The two knobs are coupled — an operator who wants a small pool lowers `max_file_bytes` to match — so the rejection message must name both values and say which one to change, rather than only reporting that the pool is too small.

**What is not inside the bound**, stated plainly so the accounting is honest: the caller's `out` buffer and `EntryIterator::io_buf_`, each sized to the largest value that caller has read. Both are caller-owned and caller-controlled. So the real equation is

```
key directory  +  pool (capacity_bytes)  +  caller buffers  +  slack
```

`DB::get`'s `thread_local io_buf` does **not** grow on the pool path — the pool copies straight into `out` and never touches it.

For the MariaDB plugin this surfaces as a `bytecaskdb_buffer_pool_size` sysvar, which is the knob operators already know how to reason about.

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

**This is a write-path insert, not a back-end choice.** The active file is a `WritableDataFile`, and it serves reads through the same registry as every sealed file. So none of the above follows from selecting a back-end at open: `WritableFileOps::append_entries` (`data_file.cppm:207`) gains a pool insert after the `pwritev`, and the writable file's `read_value` / `read_entry` have to consult the pool. That is why this is Phase 3 and why it is the only phase that touches the write path — Phases 1 and 2 leave it untouched. It is also why the residency invariant has to be proved, not assumed: if any append path can skip the insert, a read of the active file falls through to a file that was never opened for reading.

Two details:

- **The tail frame is admitted while still being appended into.** This is safe without a latch: appended bytes are never modified, and a reader can only address an offset that has been published, which happens after the bytes are written (`state_.store()` follows `fdatasync`). Reader and writer never touch the same bytes.
- **Pinned bytes are the active file's *current* size**, growing from zero to `max_file_bytes` and released at seal — so the average pinned cost is about half the rotation threshold.

**Sizing constraint:** the pool must be meaningfully larger than `max_file_bytes`, or the active file alone consumes it. This is the `capacity_bytes < 2 × max_file_bytes` rejection in §3, and it is the reason that check is mandatory rather than advisory.

*As built.* `WritableFileOps` hands the bytes of every successful `pwritev` to `BufferPool::append_resident` — gathered from the same iovecs it just wrote, one memcpy of the entry, never a re-read — and the writable file's point reads go through `read_at` like a sealed file's, with its logical end as the file size. CLOCK skips any frame whose key names the active file, and the engine moves that id at rotation and resume, under the write lock; the new active file gets its id from `reserve_file_id()` *before* it is created, the same ordering vacuum needed. The tail frame is admitted with its written prefix and **extended in place by later appends with plain relaxed word stores and no version bump**: an append never modifies existing bytes, a boundary word is observed old-or-new atomically, and a reader only uses bytes below the published size — which is what makes this section's "safe without a latch" true, and it is true only because frames are atomic words.

Two departures from the text above. Residency is **reliable, not an invariant**: a frame the writer cannot claim (the pool entirely pinned or mid-fill, which the 2x floor makes unreachable in practice) stays on disk, and a read of it takes the buffered `pread` fallback inside `read_at` — coherent, because the active file is never opened `O_DIRECT`. The test asserts zero misses across 650 read-your-own-writes, which is the property that matters, rather than asserting a frame count. And a frame not resident whose next append does not start at its first byte is skipped rather than assembled from a partial re-read; a later read miss admits it whole. `pool_frames_pinned` is computed by a walk over frame metadata; `pool_writer_inserts` counts frames the writer placed, distinct from `pool_fills`, which a read miss caused.

---

## 6. Sealed files — `O_DIRECT`

Sealed files are immutable, so a cached frame from one can never be stale and no invalidation is needed at the seal transition — the frames already hold what was written. The file is simply reopened with `O_DIRECT` for later misses.

Alignment falls out of the frame size: the arena is page-aligned, frames are 4 KiB, offsets are `frame_index × 4096`. Discover the real requirement with `statx(STATX_DIOALIGN)` where available, falling back to the logical block size.

`posix_fadvise(POSIX_FADV_DONTNEED)` on a file after it is sealed — therefore after `fdatasync`, so its pages are clean — releases its page-cache residency. That recovers most of the memory benefit without touching the write path.

**`O_DIRECT` is never applied to the write path.** Appends are `pwritev` at arbitrary offsets gathering unaligned caller buffers; aligning them would need a bounce buffer, destroying the zero-copy gather, and would fight `ensure_zeroed`. It is also not a durability primitive — `fdatasync` is still required.

**Portability:** unavailable on macOS (`F_NOCACHE` is the analogue), unavailable under Emscripten, and refused by some filesystems — older tmpfs, some FUSE and overlay mounts — which matters because CI can run on them. The back-end must detect this at open and fall back to buffered fills, recording that it did.

*As built.* Each pool-backed file opens a second descriptor with `O_DIRECT` for frame fills and keeps the buffered one for everything else — scans, oversize reads, and the fallback. The direct descriptor is **proved usable with one aligned read at open** before it is trusted, because some filesystems accept the flag at `open` and fail at `read`; refusal counts in `pool_direct_io_fallbacks` and that file fills buffered. Alignment is not probed with `statx`: every fill is a frame-aligned offset, a rounded-up length and a 4 KiB-aligned scratch, and 4 KiB is a multiple of every real block size — a device wanting more answers `EINVAL` on the read and takes the same per-read fallback. A direct read that runs past EOF comes back short, which is allowed, so the file's length need not be aligned.

`POSIX_FADV_DONTNEED` runs twice: at open, releasing the residency the file built up while it was the active file (it was `fdatasync`'d before it was sealed, so the pages are clean), and at the end of every scan sweep, since under direct I/O the page cache a sweep pulls in serves nothing afterwards. Under the buffered fallback neither runs — there the page cache *is* the fill path. tmpfs on Linux 6.6+ accepts `O_DIRECT`, so it can no longer be relied on to exercise the fallback; the fallback detection is covered only where a mount refuses.

All fills go through one seam, `fill(PoolFile, span<FillRequest>)` — a batch of one today, so that `preadv`, `O_DIRECT` and `io_uring` are implementations of that call rather than of `read_at`, per §10.

---

## 7. Everything else

**Scans.** `scan()`, `scan_committed()` and `create_manifest()` sweep whole files once. Admitting those frames would flush the working set on every vacuum pass. Scan paths bypass the pool and use buffered sequential `preadv` with `FADV_SEQUENTIAL` ahead and `FADV_DONTNEED` behind, leaving neither cache polluted.

*As built:* the bypass is in, expressed as an explicit `Source::Direct` argument on the pool-backed file's read helpers, so a new read path has to choose rather than inherit whichever default is nearest — the first implementation routed `scan()` through the pool by omission, and a vacuum pass evicted the working set. A test asserts `pool_fills` is unchanged across a vacuum. The `FADV_SEQUENTIAL`/`FADV_DONTNEED` hints are **not** in yet; the bypass stops the pool being polluted, not the page cache.

**Vacuum needs no invalidation at all.** `active_file_id_ = next_file_id_++` (`bytecask.cppm:1864`) — file ids are strictly monotonic and never reused within a process. So when vacuum unlinks a file, no `KeyDirEntry` points at it any more and its id is never issued again, which means nothing will ever look up `(that file_id, any frame)`. Its frames are not stale, they are **orphans** — and orphans are what CLOCK evicts best, since they are never referenced again and their reference bits stay clear. They clean themselves up on the next hand pass.

This is worth stating because the obvious design is a per-frame `(file_id, generation)` stamp compared on every lookup, so that bumping a counter invalidates a whole file in O(1). That buys a hot-path comparison and four bytes per frame in exchange for reclaiming dead capacity one CLOCK sweep earlier than it happens anyway. **If file ids ever become reusable, this reasoning breaks and the generation stamp becomes a correctness requirement** — not an optimisation.

**`truncate()`** — called by `resume()` — needs nothing either. Every published entry lies below `valid_offset`, so frames above it hold bytes no reader addresses, and the file is sealed immediately afterwards.

**Snapshots and rotation.** Snapshots hold files open, so nothing they reference can be unlinked. `file_id` is stable across registry snapshots, so rotation invalidates nothing.

**Recovery** reads hint files and is untouched.

**Emscripten.** No `O_DIRECT`, no page cache to bypass — MEMFS is already memory. The pool is rejected at open there, the way `use_mmap` is today, rather than silently ignored.

---

## 8. Not losing the read scaling

14.0 Mops/s at 32 threads is the strongest number in the README and the thing most at risk, since the pool puts shared mutable state on a lock-free read path. Three rules:

**1. The read path takes no lock, and the clock hand is not on it.** The hand is advanced only by a thread that has already missed and needs a victim. Since a miss costs a device read of 10–100 µs, contention on a single global hand at ~100 ns is noise — so it does not need sharding either. Readers probe the table with acquire loads and never touch it.

Writers (fill and evict) take a striped lock, re-validate the slot after acquiring it, and publish **key and value before the control word**, so a reader that sees an occupied slot is guaranteed to see the matching contents.

**2. Striped locks, not partitioned sub-pools.** An earlier draft sharded the pool into independent sub-pools, each with its own table, free list, hand and lock. That was sized to defend a read path that turns out not to need defending. Striping locks over one shared table gives the same write-path concurrency **without fragmenting capacity between shards**, so the hit ratio previously written off as the price of scaling is not lost.

*As built, V0 takes one fill/evict mutex rather than ~64 stripes* — and releases it across the device read, so the hold is the victim choice and the table update, not the I/O. By this section's own argument that makes contention noise against a 10–100 µs read. One lock is materially easier to audit, and tenet 2 outranks tenet 4 without a measured case. Striping stays available if `GetMT` shows the single lock contended; the number this section exists to defend, lock-free reads, is unaffected either way.

**3. No unconditional writes on a hit.** Test-then-set the reference bit, so a hot frame writes nothing after the first touch. Keep the bit **in-band** — packed into the control word the probe already loaded, rather than in the frame header, which would be a second cache line touched per hit. Lost updates are harmless: a slightly-less-recently-used frame may be evicted a little sooner, which is within a cache's tolerance.

*As built:* the bit is its own byte, not packed into the version word — but it sits in the same 16-byte `FrameMeta` as that word, so a hit touches the one cache line this rule exists to protect. Packing it into `version` would make the seqlock's before/after compare mask the bit out and the fill's parity arithmetic step around it, for no cache line saved; tenet 2 says the separate byte stays until a measurement says otherwise.

**4. Optimistic reads, not pin counts.** A refcount makes 32 readers of one hot frame take that cache line exclusive, serialising them however short the critical section. Instead: read the frame's version, memcpy out, re-read the version; on mismatch retry, or fall back to a direct `pread`, which is always correct.

The version check is **not** redundant with the publish-last ordering in (1), and the difference is worth being precise about. Publish-last is sufficient for a cache of object references — a reader that races with eviction gets a reference to an object that is still valid, so the worst outcome is a logically-evicted but correct value. Our frames are **reused memory**: a reader racing with evict-then-refill gets bytes that are part old and part new, which is garbage rather than a stale-but-valid value. Ordering makes the *slot* consistent; only the version check makes the *bytes* consistent. This is the same reason §2 copies out instead of handing back spans.

These are hypotheses, not claims. They have to be measured with `GetMT` at 2–32 threads against both existing back-ends, with a working set that does not fit the pool.

---

## 9. Scope — when to turn it on

This is opt-in, and the default stays `pread` / `mmap`.

**The pool cannot beat `mmap` on a resident hit, structurally.** `ReadOnlyMmapDataFile` resolves a read to `mmap_base_ + offset` — an addition and a memcpy. The pool computes a frame index, hashes it, probes a table and takes a seqlock reading first: two or three extra cache misses on every hit. Against a resident dataset with no memory pressure, the pool is slower than what already ships. It beats `pread` (no syscall) and loses to `mmap`.

That is not an argument against building it — it is the same trade every buffer pool makes, and the reason they are all sized explicitly rather than enabled by default. What it buys is what a buffer pool is for:

- **Bounded, enforced residency.** The pool decides what stays. Under a cgroup limit, where page cache is charged to us and reclaimed on the kernel's schedule, nothing else here can make that promise.
- **A hit ratio.** `disk_reads` cannot distinguish a page-cache hit from a device round trip, so there is currently no number to size against or alert on. A pool knows when it misses.

Two secondary effects push the same way at scale: a 100 GiB mapping needs roughly 25 M page-table entries and lives in TLB-miss territory under random access, where a huge-page arena needs 512× fewer; and for the MariaDB plugin, an explicit pool size is the knob operators already know how to reason about.

**Set `capacity_bytes` when there is a memory budget to enforce. Leave it at zero otherwise.**

### Real risks, to be measured

- **Miss latency has no floor.** Today a "miss" often still hits the page cache. With `O_DIRECT` it is a device round trip. Mean throughput can improve while p99 regresses, which tenet 3 counts as a regression.
- **32-thread read scaling.** 14.0 Mops/s is the strongest number in the README, and §8 is a set of hypotheses about not losing it, not a set of claims.
- **Density.** A block cache stores headers, keys and superseded entries, exactly as the page cache does. §10 quantifies what a format-aware cache would save.

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

**Online resize.** `capacity_bytes` is fixed at open in V0. InnoDB supports resizing its pool without a restart, which operators expect; doing it here means growing or shrinking the arena and rebuilding the shard tables while serving reads.

---

## 11. Options, counters, phases

### Options

`Options::use_mmap` is a `bool` at its limit. The back-end is a three-way selection now, so the option becomes one:

```cpp
enum class IoBackend { Pread, Mmap, BufferPool };
```

**It selects the writable file too.** `use_mmap` is threaded into `createDataFileForWrite` and `openDataFileForWrite` as well as `openDataFileForRead`, so the enum has to answer what `BufferPool` means for the active file. It means **`Pread`**: §6 keeps `O_DIRECT` off the write path entirely, and §5 keeps the active file resident by inserting on append, not by changing how it is written. So `BufferPool` selects the pool for sealed files and builds the writable file exactly as `Pread` does today. Worth stating in the enum's own comment, because "buffer pool" naturally reads as covering both.

The project is pre-stable, so `use_mmap` is replaced rather than deprecated alongside the enum — one representation, no shim.

Blast radius, all mechanical: `Options` and its `include/bytecask.hpp` mirror, `bytecask_hpp.cpp`, the C ABI shim, the Python and Node bindings, the `bytecaskdb_use_mmap` sysvar and the plugin README table, `benchmarks/engine_bench.cpp`, and the tests. The test surface looks alarming at ~630 references but is not: 591 are `.use_mmap = true}` in `tests/proof/generated/`, regenerated from `tests/proof/*/generate_tests.py`, leaving two `GENERATE(false, true)` sites and about twenty literals to touch by hand.

```cpp
struct BufferPoolOptions {
  std::size_t capacity_bytes;          // 0 = disabled; TOTAL footprint, not frame bytes
  std::size_t frame_bytes;             // default 4096
  unsigned    lock_stripes;          // fill/evict path only; reads take no lock
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

Hit ratio is the primary A/B metric. `keydir_bytes_estimate` is what lets an operator size the pool as *total − key directory − slack*, which §1 makes the point of the exercise; without it the budget cannot be computed from outside. `pool_optimistic_retries` is the early warning for §8.2. `pool_direct_io_fallbacks` catches a mount refusing `O_DIRECT` and degrading silently in CI.

*As built:* `keydir_bytes_estimate` is `keydir_keys × 50`. The tree keeps no byte accounting, and adding it to the persistent clone/release path is not worth the risk for a gauge, so the constant comes from `scripts/run_memory_profile.py` on this tree: 47 B/key for 42-byte prefixed keys at 500 k (55 at 100 k), and 61–77 B/key for 8-byte binary keys, which share less prefix. That is an estimate within about ±30 % by key shape; `keydir_keys` is exact and sits beside it so an operator who has profiled their own keys can multiply for themselves. `pool_frames_resident` is a gauge; `pool_frames_pinned` waits on Phase 3.

### Correctness

`tests/data_file_test.cpp` and `tests/bytecask_test.cpp` already `GENERATE(false, true)` over `use_mmap`; widening that to the three back-ends runs every existing test body against the pool unchanged. This is the main reason §2 keeps the contract byte-identical. Add a differential test — random reads against a pool-backed file compared byte-for-byte with `ReadOnlyPosixDataFile` as oracle, at pool sizes small enough to force constant eviction. The `[model]` recovery tests need extending if anything here changes what a recovered DB contains; as designed it does not, but the active-file insert-on-write path and `truncate()` invalidation sit close enough to the write path to warrant coverage.

### Phases

| Phase | Content | Answers |
|---|---|---|
| 0 | Back-end enum, counters, `u` and `L` instrumentation, a benchmark sweep whose working set exceeds RAM | What is the baseline, and can we even see the regime where a cache matters? |
| 1 | Pool with buffered `preadv` fills, CLOCK, striped fill/evict locks, copy-out | Does it beat the page cache while still using it? Does 32-thread scaling survive? |
| 2 | `O_DIRECT` on sealed files, `FADV_DONTNEED` after seal | What does bypassing the page cache buy, and cost at p99? |
| 3 | Active-file residency and pinning | Does read-your-own-writes improve on a write-heavy mix? |

The benchmark sweep in Phase 0 is not optional. Every current benchmark fits in RAM, where a cache can only lose; the interesting axis is `pool_bytes / dataset_bytes` at 0.1×–2.0×, with p50 and p99, not throughput alone.

**Phase 0 is worth building whether or not the pool follows.** The missing hit-ratio visibility and the missing out-of-RAM benchmark are gaps today.

### Measurements

`benchmarks/pool_bench.cpp`, via `python3 scripts/run_pool_bench.py`; recorded in `benchmarks/pool_bench_results.csv`. 60 k keys x 512 B (31 MiB on disk), Zipf(0.99), 1 MiB files, one run each, dev container — read the columns against each other, not against the README. The pool runs each ratio twice: `direct_io=0` (Phase 1, fills through the page cache) and `direct_io=1` (Phase 2, `O_DIRECT` fills with the file's page-cache residency dropped at open).

| Back-end | direct | ratio | ops/s | p50 | p99 | hit ratio |
|---|:-:|---:|---:|---:|---:|---:|
| pread | — | — | 1.03 M | 552 ns | 1.65 µs | — |
| mmap | — | — | 1.61 M | 241 ns | 1.08 µs | — |
| buffer pool | 0 | 0.25 | 692 K | 700 ns | 3.99 µs | 0.808 |
| buffer pool | 0 | 1.00 | 980 K | 489 ns | 2.96 µs | 0.928 |
| buffer pool | 1 | 0.25 | 92 K | 812 ns | **63.0 µs** | 0.808 |
| buffer pool | 1 | 1.00 | 211 K | 722 ns | **52.7 µs** | 0.928 |

**What `O_DIRECT` does here is exactly what §9 warned it would.** Under `direct_io=1` a pool miss is a device read, and this VM's disk answers in ~50–75 µs. p99 is that number at every ratio, because even at 1.00 the residual 7 % of misses are compulsory first-touches inside the measurement window, and at 1 % of 60 k reads the tail is made entirely of them. Throughput falls 5–8x against the buffered pool. This is not a regression to fix: it is the cost of the pool doing its actual job, measured in the one regime — everything resident, page cache warm, no memory pressure — where the design says to switch it off. The `pread` and `mmap` rows are reading the same bytes from a warm page cache the machine cannot drop without root; their tails are a floor.

What this benchmark still cannot show is the pool's *benefit*: a dataset larger than RAM, or a cgroup limit, where the page cache is the thing evicting the key directory. Neither is set up here. On this evidence the pool is a bounded, measurable cost with no demonstrated upside — and that is an honest description of Phase 2 on a machine with spare RAM.

**The frame copy's cost, before and after.** Making the copy a data-race-free sequence of relaxed atomic word loads (the plain `memcpy` was UB) first landed as a generic loop — a modulo and two `memcpy`s per word — and buffered p50 at ratio 1.00 went from 270 ns to 489 ns, throughput 1.35 M to 980 K. Rewriting it as head / whole-words-to-`dst` / tail brings it to **324 ns and 1.18 M**. The ~50 ns that remains against the pre-atomic number is the price of eight-byte atomic loads over one vectorised `memcpy`, and it stays: correctness is not the thing to trade here. p99 is unaffected either way — it is made of misses.

Against §12.1's proposed Phase 1 bar (measured on the buffered rows, which is what Phase 1 is): hit ratio 0.808 at ratio 0.25 passes; optimistic retries were zero, passes; p99 3.99 µs is 3.7x `mmap` against a bar of 2x, fails; the 32-thread `GetMT` criterion is still unmeasured. The bar was written before any numbers existed, and Phase 1 reads through the page cache on both sides, so it cannot structurally match `mmap` there — whether that means the bar was wrong or the miss path is, is the question §12.1 exists to settle before Phase 2 is judged.

**`u` is 0.17–0.23 on this workload — under §12.2's threshold.** Measured at ratios 0.10 / 0.25 / 0.50 as 0.172 / 0.196 / 0.229, identical under buffered and direct fills, undefined at 1.00 and above where nothing is evicted. With 512-byte values a frame holds ~7.5 entries, so an evicted frame served about 1.3–1.7 of them before it went: the frames that get evicted are, by construction, the cold ones, filled for a single read whose neighbours were never asked for. Two things make the real figure lower still, not higher — the bitmap counts 32-byte slots, an overcount against entries, and this workload's Zipf rank *is* its disk order, so hot keys are physically adjacent, which is kinder to a block cache than a real key space would be. §12.2 says `u ≤ 0.3` reopens the storage layer, and only the storage layer. This measurement does that. It does not decide it: §10's estimate is that an entry cache has ~`0.9/u` of a block cache's effective capacity — about 4× here — against a slab-calcification failure mode that has no number attached, and one workload shape at one value size is not the evidence to settle that on. What it is, is the first number the question has ever had.

**Multi-reader arm (§8's hypotheses).** `--mt-threads`, fully resident (ratio 1.0, buffered fills), N threads sharing one DB and one Zipf sampler; 120 k reads split across them; a 4-core container, so 8 threads is 2x oversubscribed and §8's 32-thread criterion is not measurable here.

| threads | mmap | pool | pool/mmap | 1→N scaling, mmap / pool | pool p99 / mmap p99 | optimistic retries |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1.60 M | 1.16 M | 72 % | — | 2.7x | 0 |
| 2 | 1.77 M | 2.13 M | 120 % | 1.1x / 1.8x | 2.0x | 0 |
| 4 | 2.96 M | 2.62 M | 88 % | 1.85x / 2.26x | 2.5x | 0 |
| 8 | 3.77 M | 2.78 M | 74 % | 2.4x / 2.4x | 3.0x | 0 |

Retries at zero everywhere is the result that matters: the seqlock read never lost a race to eviction, so §8.4's optimistic copy-out is carrying no fallback load. The pool scales at least as well as `mmap` up to the core count — the throughput gap is the per-hit cost already visible single-threaded, not contention — and its p99 ratio to `mmap` does not widen with threads, so the single fill mutex (§8.2's deviation) leaves no signature at this concurrency. Against §8's "within 10 % of `mmap`" it is 12 % at four threads; the 2-thread row where the pool leads is one run and should be read as noise. None of this speaks to 32 threads.

**Read-your-own-writes (§11 Phase 3's question).** `--ryow N`: put a fresh 512-byte value, read it straight back, N = 20 000 times; only the gets are timed, at ratio 1.0 with buffered fills, one run.

| Back-end | pairs/s | get p50 | get p99 | hit ratio |
|---|---:|---:|---:|---:|
| pread | 148 K | 1.24 µs | 2.27 µs | — |
| mmap | 126 K | 1.52 µs | 3.50 µs | — |
| buffer pool | 120 K | 1.71 µs | 2.89 µs | **1.000** |

The mechanism works: every one of 20 000 reads of just-written bytes was served from the frame the writer filled, zero misses, no disk and no page cache. The answer to "does it improve on a write-heavy mix", on this machine, is **no**: `pread`'s read of bytes it just wrote is a warm page-cache hit at 1.2 µs, and the pool's is slower, while the put side pays about 19 % for the insert (after fixing the same naive word loop on the write side that had cost the read side; 25 % before). What residency buys — never touching the page cache — is invisible without memory pressure, which is the same sentence every other row in this section ends on.

One number here is not explained. A pool hit on the active file costs ~1.7 µs, against ~0.32 µs for a pool hit on a sealed file in the sweep above: same `read_at`, same copy-out, same frames. The get-after-put baseline (state refresh, the released snapshot's node reclamation) is shared by all three back-ends and cannot be it. It wants a profile before anyone reasons about it; it is left open rather than theorised.

The arena page-fault inversion found by the first version of this benchmark (p99 rising with pool size because the arena faulted in lazily on the read path) stays fixed: buffered p99 falls monotonically from 0.10 to 1.00.

---

## 12. Open

1. **What is the acceptance bar for Phase 1?** Proposed below — still to be agreed before Phase 1 starts, which is the point of asking. It is a hit ratio and a p99 at a stated working-set ratio, not a throughput number on benchmarks that cannot see the regime.

   Measured at `pool_bytes / dataset_bytes = 0.25` under a Zipfian read workload (s ≈ 0.99, the YCSB default) over a dataset that does not fit in RAM:

   - **Hit ratio ≥ 0.6.** CLOCK over Zipf(0.99) at a quarter of the working set should clear this comfortably. Below it, the fault is the policy or the admission rule, not the pool.
   - **p99 within 2× of the `mmap` back-end** on the same workload. Phase 1 still fills through the page cache, so both sides read the same bytes from the same place and the gap *is* the pool's own overhead — which is what this phase exists to measure. Tenet 3 makes p99 the deciding number, not mean throughput.
   - **`GetMT` at 32 threads within 10 % of the `mmap` back-end on a fully resident dataset.** This is §8 restated as pass/fail. The regression that matters is the one on the workload where the pool should be switched off but its code is still on the read path, and 14.0 Mops/s is the number being defended.
   - **`pool_optimistic_retries / pool_hits < 10⁻³`** at 32 threads. Higher means eviction is fighting readers and §8.4's `pread` fallback is carrying load it was not designed to carry.

   Failing the first is a policy problem and does not block Phase 2. Failing either of the last two is a design problem and does.

2. **Does measured `u` reopen Axis A?** See §10. `u ≥ 0.9` closes it permanently; `u ≤ 0.3` reopens the storage layer, and only the storage layer.

   *Instrumented.* Each frame carries a 128-bit bitmap of the 32-byte slots hits have copied out of, set test-then-set so a hot slot writes once; at eviction its popcount is added to `pool_evicted_bytes_touched`, and `u = pool_evicted_bytes_touched / (pool_evictions × 4096)`. `pool_bench` reports it per run. It measures bytes read, not entries — a block cache does not know entry boundaries — so it is a slight overcount of §10's definition at the 32-byte granularity, which is the conservative direction for the question it answers.
3. ~~Is the `resume()` / `truncate` mmap span hazard real?~~ Filed as [#87](https://github.com/gustavoamigo/bytecaskdb/issues/87). Pre-existing, independent of this work.
