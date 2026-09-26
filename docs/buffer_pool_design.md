# Buffer Pool Design

> **Status: implemented** as `IoBackend::BufferPool` (`bytecaskdb/buffer_pool.cppm`, `ReadOnlyBufferPoolDataFile` in `bytecaskdb/data_file.cppm`). Alternatives considered and not taken are in §9.

## Goal

**This exists for memory-constrained deployments** — a container with a `memory.max`, a shared host, a tuned appliance — where the dataset is far larger than RAM and the memory the engine uses has to be **bounded and configurable**.

The engine's foundational bet is that all keys live in RAM. The key directory is not a cache: at roughly 50 bytes per key it is ~5 GB at 100 M keys, and if any of it is not resident a lookup becomes a page fault. Nothing guarantees it that memory today. The page cache is an unbounded competitor for the same RAM, and the kernel arbitrates between them knowing only that one allocation is anonymous and the other file-backed — not that one is load-bearing and the other discretionary. Without swap the usual failure is not dramatic: reclaim churn and direct-reclaim stalls inside the write path, since every `put` of a new key allocates tree nodes. With swap, the sharper failure is available too.

A B-tree engine's buffer pool caches index and data together and can evict either. Here the index cannot be evicted at all, so bounding the value cache is the only way to protect it. The deliverable is one number the operator sets and the engine honours: `capacity_bytes`, meaning total footprint.

Two things follow from owning the cache rather than borrowing it: a **hit ratio** (`disk_reads` cannot tell a page-cache hit from a device round trip; `pool_hits` / `pool_misses` can), and **`O_DIRECT`**, which is the mechanism rather than an optimisation — a pool filled through buffered `pread` bounds nothing but itself, because the page cache goes on growing behind it.

**Non-goals.** Making a dataset that already fits in RAM faster: on a resident hit the pool is ~15 ns behind `mmap` and cannot beat it (§8). Where memory is not constrained, leave it off.

An append-only store does not need a real buffer pool, only a cache. Pages are never modified in place, so there is no dirty bit, no flush list, no checkpoint and no torn-page protection. What remains is an index over fixed frames, an eviction policy and a fill path.

## 1. Shape

```
Arena        one allocation of 4 KiB frames, plain bytes
Index        one open-addressed table of 16-byte slots; lock-free reads
Slot         key (file_id, frame_index) · frame number + SIEVE visited bit · seqlock version
Pins         one 32-bit count per frame; a frame is immutable while any reader holds one
Eviction     SIEVE over a per-frame admission-order list; test-then-set visited bit on a hit;
             frames admitted unvisited; skips pinned frames
Writers      one mutex for every change to the index or a frame's bytes
Active file  fully resident, inserted by the writer on append, never evicted
Sealed files O_DIRECT fills through a second descriptor
Fills        while frames are free, a miss reads the file-aligned 128 KiB block around it; once full, only the frames it needs
CRC          unchanged — verified per entry, per read
Scans        bypass the pool
Oversize     an entry over capacity/8 is read straight to the caller, never admitted
```

4 KiB frames because that is the `O_DIRECT` alignment unit and the device transfer minimum: the pool caches, evicts and serves hits a frame at a time. It does not *fill* a frame at a time while it is warming: until every frame is in use, a miss reads the 128 KiB block of the file around it — the kernel's default readahead, the rate the page-cache back-ends warm at — because frame-sized fills leave a cold pool warming one 4 KiB read per miss (§8). Once the pool is full, a miss reads only the frames it needs, as before. An entry spanning several frames is resolved as several independent lookups; each run of missing frames is one read, widened to the blocks it touches.

**The slot is the only metadata a hit touches.** Key, frame number, visited bit and version sit in one 16-byte slot, so a hit touches one index cache line and then the frame. An earlier version kept a separate per-frame metadata array with the version and reference bit; that was a second dependent cache miss on every hit for nothing the slot could not hold. The eviction list (§4) is per-frame metadata too — the frame's key and its two neighbours, 16 bytes — but only a miss, under the mutex, ever reads it.

**Frames are immutable while readable, so a reader uses `memcpy` — or no copy.** A frame's bytes change only when eviction has *claimed* it, and eviction claims a frame by moving its pin count from zero to a dead marker with one CAS, which fails while any reader holds a pin. A reader pins before it reads and unpins after, so at the moment of its `memcpy` no store to those bytes is possible and there is no data race to argue about. The first version instead copied under the seqlock with relaxed 8-byte atomic loads — race-free, but a scalar loop the compiler cannot vectorise (~14 ns for 245 B against `memmove`'s ~4, ~0.5 ms on 2 MiB) and, being a copy-then-validate scheme, unable to lend a span. Two pin RMWs per hit are what immutability costs (~8 ns; the classic buffer-pool price, RocksDB's HyperClockCache pays the same), and on a uniform and on a Zipf(0.99) read mix they measured cheaper than the copy they replace at every thread count (§8). The one write a pinned frame still sees is the active file's append extending it past the size every reader is bounded by: disjoint bytes.

## 2. The bound is the contract

`capacity_bytes` means **total pool footprint**: the index table is a power of two sized for at most 70 % load, and the arena takes what is left of the budget — never the other way round. Configure 1 GiB, observe 1 GiB.

```
table  = bit_ceil(capacity / (4096 + 16) × 10 / 7)     slots
frames = min((capacity − table × 16) / 4096, table × 0.7)
```

The pinned active file is *inside* the budget: as it grows, the cache available for sealed data shrinks, and at rotation it is released. That is what makes the sizing check mandatory rather than advisory — `DB::open` rejects `capacity_bytes < 2 × max_file_bytes`, because below that the active file alone consumes half the pool. The two knobs are coupled; the rejection message names both and says which to change.

Not inside the bound, stated plainly: the caller's `out` buffer and `EntryIterator::io_buf_`, each sized to the largest value that caller has read. So the real equation is `key directory + pool + caller buffers + slack`. `stats()` reports `keydir_keys` for the first term; multiply by the bytes/key your key shape measures (`scripts/run_memory_profile.py`, about 50 for typical keys).

For the MariaDB plugin this is `bytecaskdb_buffer_pool_size`, the knob operators already know how to reason about.

## 3. The read path

`read_value` copies into `out` — assigned straight from the lent frame when the bytes lie in one resident frame, which is one allocation and one copy like the `mmap` path, and through `read_at` otherwise — and `read_entry` / `read_entry_unverified` return spans into the caller's `io_buf`, exactly as `ReadOnlyPosixDataFile` does. `lend_record` is the third way and the one iterators and the blind key directory use: it takes the key and value sizes from the entry's own header, and when the whole entry — header through CRC — lies inside one resident frame, it returns spans **into the frame** after one lookup and parks the pin in the caller's `FrameLease`; the frame cannot be evicted or refilled until the lease is reset, which `EntryIterator` does on advance. An entry that straddles a frame boundary (about 7 % at 300 B) or whose frame is not resident is copied into `io_buf` as before. The lease lives in the iterator next to `io_buf`, so the span and the pin that backs it cannot come apart; it is released before `io_buf` is destroyed. Every other back-end implements `lend_record` by name rather than through a base default, so `mmap` pays one virtual call per entry and not two: `mmap` returns spans into the mapping, and `pread` reads the header, a key budget and the expected value (or the rest of the page when the caller has no size) in one call, and the rest of a longer entry in a second.

**A hit** takes no lock. Probe the index from `hash(key)` with linear probing: load the slot's version (acquire), then its key; an empty key ends the probe as a miss. On a match with an even version, load the frame number and **pin the frame** (`fetch_add` on its pin word); a dead marker in the old value means eviction owns it, so unpin and retry. Then reload the slot's version: the slot named this frame for this key when we read it, but the frame could have been claimed and refilled between that load and the pin — doing so would have erased the slot, a slot's version only ever moves, so the version being what it was proves the slot still maps the key to the frame, and the pin now held proves the frame stays put. Copy with `memcpy` (or lend the span) and unpin. After eight lost races fall back to a device read, which is always correct. Besides the pins, the only write on a hit is a test-then-set of the visited bit.

**A miss** in a pool with free frames reads the file-aligned `kPoolFillBlockBytes` (128 KiB) blocks covering the run of missing frames into a thread-local, page-aligned buffer — `O_DIRECT` where the file has a direct descriptor, buffered otherwise — copies the caller's bytes out of that buffer, then takes the mutex and admits each whole frame the read covered. Frames of the block that were already resident are read again and not re-admitted; blocks are clipped to the file. The device read happens outside the lock. A short tail frame (one that would extend past EOF) is served but never admitted, which is what lets frames be fixed-length with no per-frame valid-length field. A read larger than `capacity / 8` bypasses the pool entirely: admitting it would evict the working set to hold one value. A full pool reads just the missing run, frame-aligned: there every neighbour a block admitted would evict a frame that earned its place, and on a Zipf read mix with the pool a tenth of the dataset that cost 5× in throughput (§8). The resident-frame count that decides is read without the lock — it only grows until the pool is full, and a stale value picks a fill size, not an outcome. The oversize bound also applies: a pool so small that a block is over `capacity / 8` fills frames only, or one miss would evict the frames it had just admitted; `DB::open` requires `capacity ≥ 2 × max_file_bytes`, so at any real file size that never binds.

## 4. Changes under one mutex

Every change to the index or to a frame's contents happens under `mu_`; every change to a slot is bracketed by its version going odd and then even, and every write to a frame happens while its pin word holds the dead marker. **That is the whole invariant**: a reader that pinned a frame and then saw its slot's version unchanged holds bytes nothing can write. Three operations:

- **`insert(key, frame, bytes, visited)`** takes a dead frame — claimed by eviction, or fresh and named by no slot — finds the first empty slot on the key's probe chain, marks it odd, stores frame and key, writes the frame's bytes, marks it even, and clears the dead bit last, so a reader that pins it then finds the slot as it is. The clear is an RMW (`fetch_and`), not a store of zero: a reader that loaded the victim's old slot can be between the `fetch_add` of a pin that will fail and the `fetch_sub` that drops it, and a store would wipe that +1, so the drop would wrap the pin word below zero — a frame nothing could pin or claim again. It then links the frame at the newest end of the eviction list.
- **`erase(slot)`** is a backward shift, not a tombstone. Each later entry in the cluster whose probe path crosses the hole moves back into it (odd → store → even on the destination), and the last hole is cleared. Readers see a moved entry at its new slot before its old slot changes, so the only transient state is a duplicate. Tombstones were tried first: a linear-probing table that only ever adds tombstones gets slower with every eviction and never recovers — §8 has the numbers.
- **`sieve_victim()`** is SIEVE (Zhang et al., NSDI '24). Every resident frame is on a list in admission order, newest at one end; the hand walks from where it last stopped toward the newest frame, wrapping to the oldest, clearing visited bits, until it reaches an unvisited frame of a sealed file that it can claim — a CAS of the pin word from zero to dead. The victim is unlinked and the hand rests on the frame after it. A pinned frame is passed over, never waited for: a lease can be held across a caller's loop body. The hand is touched only by a thread that has already missed and needs a frame, so at ~100 ns against a 10–100 µs device read its contention is noise.

**Why SIEVE, and why frames are admitted unvisited.** A hit sets a bit and moves nothing, exactly as CLOCK's did, so the lock-free hit path is unchanged. What differs is the order the hand meets frames in. Survivors keep their place on the list and new frames go in at the newest end, between the hand and the frames it has already passed, so a frame read once reaches the hand after a fraction of a sweep; under CLOCK it waited a whole one. Most frames are read once — the misses of a skewed workload are overwhelmingly one-off reads — so evicting those early is where SIEVE's hits come from. That only works if a new frame starts unvisited: the miss that brought it in is its first read, and what earns it a place is a second. Admitted visited, as CLOCK's frames were, a frame read once survives a full pass and SIEVE degrades to CLOCK (offline replay: 0.618 hit ratio against SIEVE's 0.671 and CLOCK's 0.615 on the Zipf trace of §8). The writer's appends are the exception and are admitted visited: they are the bytes likeliest to be read next, and the hand skips the active file until it rotates anyway. The list lives beside the index rather than in it because the index's order is hash order, and backward-shift erase moves entries across the hand; the old CLOCK's hand walked slots, and so walked frames in an order unrelated to their age.

Admission is `erase(victim)` then `insert(key, victim's frame, bytes, visited)`. No slot is ever odd outside the mutex, so the lock holder never has to skip a mid-change slot, and since every used frame is indexed by exactly one slot at all times, `pool_frames_resident` is simply the count of frames ever claimed.

The seqlock writer stores the odd version and then a release fence, so the changes that follow cannot become visible before the odd version does; the even version is a release store. The reader issues an acquire fence between its key and frame loads and the version re-read: an acquire load, and the acquiring pin RMW, order only what follows them, so without the fence those loads could sink below the re-read. The frame's bytes are published by the release RMW that clears the dead bit, which the reader's acquiring `fetch_add` synchronises with — so a reader whose pin succeeds sees the finished bytes.

## 5. The active file

The active file is always created empty at startup, so every byte in it was written by this process, and the writer already holds those bytes after `pwritev`. The active file is its own implementation, `WritableBufferPoolDataFile` — `WritablePosixFile` instantiated on the `PoolIo` policy — so the pool's descriptor and `file_id` are that class's state rather than a nullable member every writable file carries. Its policy hands each iovec the writer just wrote to `append_resident`, which extends a resident frame in place with relaxed word stores and **no version bump** — an append never modifies existing bytes, a boundary word is observed old-or-new atomically, and a reader only asks for bytes below the published file size — or admits a frame whose written prefix starts at its first byte. The writable file's point reads then go through the pool like a sealed file's, with its logical end as the file size.

This gives read-your-own-writes without a disk read or a page-cache read, warms the cache with the hottest data for free, and removes the `O_DIRECT` coherency hazard: the one file being written with buffered `pwritev` is the one file never read from disk. The hand skips any frame whose key names the active file; at rotation the engine moves that id (under the write lock) and the previous file's frames become ordinary, no sweep needed.

Residency is reliable rather than an invariant: a frame the writer cannot claim (every slot pinned, which the 2× floor makes unreachable in practice) or an append that does not start at a frame boundary of a non-resident frame stays on disk, and a read of it takes the buffered fallback inside `read_at`, which is coherent because the active file is never opened `O_DIRECT`. The test asserts zero misses across 650 read-your-own-writes plus a batch.

The new active file's `file_id` is reserved (`TransientEngineState::reserve_file_id`) *before* the file is created, because the writable file keys its frames by it from construction. Vacuum does the same for its compacted file under a short write barrier, which is what keeps the reservation disjoint from the ids rotation mints on the same counter; that costs vacuum a second barrier per compaction. A reserved id that goes unused leaves a gap, which is harmless: ids are monotonic within a process and reassigned at recovery.

## 6. Sealed files — `O_DIRECT`

Each pool-backed sealed file opens a second descriptor for frame fills through `open_uncached` and keeps the buffered one for everything else. That function is the only place in the engine that names a platform mechanism for uncached reads, and the only platform `#if` left in it; the engine and the tests both ask it, so the two cannot disagree about whether a mount serves one. The descriptor is **proved usable with one aligned read at open** — some filesystems accept the flag and fail at read — and a file whose filesystem refuses fills buffered, counted in `pool_direct_io_fallbacks` so a CI mount cannot silently measure the wrong thing. Alignment falls out of the frame size: every fill is a frame-aligned offset, a rounded-up length and a 4 KiB-aligned buffer, and a read past EOF comes back short, which is allowed.

`POSIX_FADV_DONTNEED` runs at open, releasing the residency the file built up while it was the active file (it was `fdatasync`'d before it was sealed, so the pages are clean), and after every scan sweep, since under direct I/O the page cache a sweep pulls in serves nothing afterwards. On macOS there is no `O_DIRECT`; `open_uncached` sets `F_NOCACHE` instead, and there is no `posix_fadvise` to call. Where neither exists it returns -1 and every file takes the buffered fallback.

`O_DIRECT` is never applied to the write path: appends gather unaligned caller buffers with `pwritev`, and aligning them would need a bounce buffer.

## 7. Everything else

**Scans bypass the pool.** `scan()` sweeps whole files once — vacuum, hint generation, `create_manifest` — and admitting those frames would flush the working set on every vacuum pass. The bypass is an explicit `Source::Bypass` argument on the pool-backed file's read helpers, so a new read path has to choose rather than inherit a default; a test asserts `pool_fills` is unchanged across a vacuum.

**Vacuum needs no invalidation.** File ids are strictly monotonic and never reused within a process, so when vacuum unlinks a file nothing will ever look up its frames again. They are orphans, not stale entries, and the hand reclaims them on its next pass since their visited bits stay clear. *If file ids ever become reusable, this reasoning breaks* and a per-frame generation stamp becomes a correctness requirement.

**`truncate()`** (from `resume()`) needs nothing either: every published entry lies below the truncation point, and the file is sealed immediately afterwards. **Snapshots** hold files open, so nothing they reference can be unlinked. **Recovery** reads hint files and is untouched. **Emscripten** accepts the back-end without `O_DIRECT`: `open_uncached` reports no uncached read there, so every file fills through the buffered path. The Node builds use `NODERAWFS`, where every `pread` is a call out to Node's `fs`; a hit skips it, which is the reason to run the pool on WASM. At 100k keys the WASM `Get` went from 468 K to 1.25 M ops/s, and `Range50` from 16 K to 214 K scans/s. WASM linear memory never shrinks, so the pool's frames stay reserved until the process exits.

| Path | Uses the pool? |
|---|---|
| `DB::get`, `Snapshot::get` | Yes, via `read_value` |
| `iter_from` / `riter_from` | Yes, via `read_entry` |
| `keys_from`, `rkeys_from`, `contains_key` | No — pure radix tree |
| Vacuum, `create_manifest`, `scan_committed` | Via `scan` — bypasses |
| Recovery | No — hint files |

**Options and counters.**

```cpp
enum class IoBackend { Pread, Mmap, BufferPool };   // one writable + one read-only implementation each
struct BufferPoolOptions {
  std::size_t capacity_bytes;   // total footprint; >= 2 x max_file_bytes
  bool direct_io{true};
};
```

`stats()` adds `keydir_keys`, `pool_hits`, `pool_misses`, `pool_fills` (frames admitted by a read miss; the writer's inserts cost no I/O and are not counted), `pool_evictions`, `pool_frames_total`, `pool_frames_resident` and `pool_direct_io_fallbacks`. Hit ratio is the number to size against.

## 8. Measurements

`benchmarks/pool_bench.cpp` via `python3 scripts/run_pool_bench.py`, recorded in `benchmarks/pool_bench_results.csv`. Unless stated: 60 k keys × 512 B (32.5 MiB on disk), Zipf(0.99), 4 MiB files, 200 k reads after a 20 k warm-up, a 4-vCPU Azure VM with a ~160 µs disk. Read the columns against each other, not against the README's numbers from other hardware.

**Resident, buffered fills, ratio 1.0** (pool = dataset bytes, so ~2 % of frames do not fit):

| Back-end | threads | ops/s | p50 | p99 | hit ratio |
|---|---:|---:|---:|---:|---:|
| pread | 1 | 767 K | 912 ns | 1.57 µs | — |
| mmap | 1 | 1.68 M | 240 ns | 861 ns | — |
| buffer pool | 1 | 1.44 M | 301 ns | 2.10 µs | 0.981 |
| mmap | 2 | 3.09 M | 290 ns | 882 ns | — |
| buffer pool | 2 | 2.81 M | 321 ns | 2.12 µs | 0.981 |
| mmap | 4 | 4.59 M | 380 ns | 1.01 µs | — |
| buffer pool | 4 | 3.94 M | 471 ns | 2.99 µs | 0.981 |

**Why the pool is not as fast as `mmap` on a hit, and why it cannot be.** `mmap` resolves a read to `base + offset` and a `memcpy`: one data access, no index. The pool hashes the key, probes one index line, and copies with 8-byte atomic loads. Isolated on one hot key (`--hit-probe`, everything in L1) both cost the same, 221 ns per `DB::get`; the ~60–80 ns gap in the Zipf sweep is the index line missing cache on a spread-out working set plus the atomic copy. That is the structural floor of any cache with an index. What the pool loses at p50 it does not lose at scale: 1→4 threads scales 2.7× against `mmap`'s 2.7×, with the fill mutex leaving no signature.

**Hit path against `mmap` on the engine benchmark.** `engine_bench`'s ByteCaskDB rows are the pool (its default adapter) and `ByteCaskDB_Mmap/*` the comparison: 1 M keys × 245 B, ~330 MB on disk, a pool sized at 512 B per key and read through once before the timed loop so the rows measure hits (`pool_hit_ratio` = 1.0 on every row), Ryzen 7 3700X, medians of 3. The rows below were taken while the pool adapter was still the labelled one, at 1 GiB; the size does not change a hit.

| | mmap | pool | pool ÷ mmap |
|---|---:|---:|:---:|
| Get | 206 ns | 257 ns | 0.80 |
| GetMT × 8 | 34.3 M/s | 27.5 M/s | 0.80 |
| GetMT × 16 | 45.7 M/s | 36.7 M/s | 0.80 |
| GetMT × 32 | 62.1 M/s | 54.1 M/s | 0.87 |
| Range-50 | 1.51 µs | 4.86 µs | 0.31 |

Before this table the pool did 16.1 M/s at 32 threads against `mmap`'s 28.6, and `perf` on `DB::get` said why: every get did a `fetch_add` on `disk_reads` and `disk_read_bytes` — one `Counters` cache line for every reader — the pool added `pool_hits` on the same line, and `get` copied the thread-local state `shared_ptr`, two more read-modify-writes on a control block every reader shares. At 32 threads those lines absorb about 50 M RMWs a second and nothing else counts: two RMWs per get gave `mmap` ~25 M/s, three gave the pool ~16 M. Striping the read-path counters per thread (`StripedCounter`) and binding the state by reference took both back-ends to where the serial cost sets the rate, and the pool now scales with readers at its single-thread ratio.

The ~50 ns single-thread gap, from `perf annotate` on the hit path: ~9 ns loading the slot (the index line, an L3 hit on a spread-out working set), ~10 ns of hash, probe and seqlock, ~14 ns for the word-at-a-time copy of 245 B against `memmove`'s ~4, ~5 ns for the hit counter, and ~8 ns of `memset` from `out.resize()` that `mmap`'s `assign` does not pay when the caller hands over an empty buffer (a reused buffer of the same size costs neither). The slot line and the atomic copy were the floor of that design; inlining the sub-word head and tail copies measured as a loss and was not kept. Range-50 was a different shape: `mmap` returns spans into the mapping with no copy at all, and the pool's `read_entry_unverified` did two lookups per entry — header, then body — and copied both.

**Pinned frames.** Replacing the seqlock copy with pins, `memcpy` and lent spans (§1, §3), same benchmark, medians of 3 (Range-50 of 5):

| | mmap | pool, seqlock copy | pool, pinned | pinned ÷ mmap |
|---|---:|---:|---:|:---:|
| Get | 209 ns | 257 ns | **224 ns** | 0.93 |
| GetMT × 8 | 35.8 M/s | 27.5 M/s | **30.7 M/s** | 0.86 |
| GetMT × 16 | 47.8 M/s | 36.7 M/s | **40.5 M/s** | 0.85 |
| GetMT × 32 | 60.0 M/s | 54.1 M/s | **57.0 M/s** | 0.95 |
| Range-50 | 1.67 µs | 4.86 µs | **2.34 µs** | 0.71 |

The Get row is two steps: pins and `memcpy` took 257 to 242 ns, and letting `read_value` assign into `out` straight from the lent frame — one allocation and one copy, the shape of the `mmap` path, instead of `resize()`'s zero-fill followed by `read_at`'s run bookkeeping — took it to 224. The CRC-verifying read, which is the default, verifies over the frame span in place the same way.

And on `pool_bench`'s Zipf(0.99) multi-reader arm (60 k × 512 B, resident), where hot keys would show pin contention if there were any: 1 thread 1.96 → 2.15 M/s (mmap 2.34), 8 threads 11.2 → 13.1 M/s (mmap 15.4), 16 threads 18.4 → 19.8 M/s (mmap 23.0); p50 280 → 230 ns. The pins cost less than the copy they replace at every point measured.

`mmap`'s own Range-50 moved 1.50 → 1.67 µs: `EntryIterator` now goes through `lend_entry` (since renamed `lend_record`), and the lease reset on advance plus the `verify` branch inside it are ~20 instructions an entry that the mapping does not need. A base-class default for it cost a second virtual call on top (1.75 µs) and was replaced by per-back-end implementations.

What is left of the pool's hit, from `perf annotate` on the pinned copy path: ~8 ns loading the slot — the index's one dependent cache miss — ~3 ns of hash, probe and version check, ~8 ns for the two pin RMWs, ~4 ns for the hit counter, and the copy itself, now the same as `mmap`'s copy into `out`. That is the ~15 ns a `get` is behind `mmap`, and it is what an indexed, bounded cache with immutable frames is. Two things that did not help and were not kept: prefetching the slot from `DB::get` before the file-registry walk (the window is too short to hide the miss; 224 → 224 ns), and inlining the sub-word head and tail of the old atomic copy (a loss).

**p99 is the hit ratio, not the hit cost.** At 0.98 the 99th percentile *is* a miss — a buffered `pread` plus admission, ~2 µs — and at ratio 2.0 (everything fits, hit ratio 0.994) p99 drops to 0.95 µs. Under real memory pressure a miss is the device, for every back-end alike.

**Index ageing, before and after backward-shift deletion.** Ratio 0.25 (hit ratio 0.81), 1 MiB files, run for 100 k and then 4 M reads so evictions accumulate. The tombstoned table got slower with age and never recovered; the shifted one does not move.

| Index | reads | evictions | ops/s | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| tombstones | 100 k | 21 k | 1.02 M | 2.69 µs | 3.72 µs |
| tombstones | 4 M | 844 k | 926 K | 4.42 µs | 6.93 µs |
| backward shift | 100 k | 20 k | 1.04 M | 2.79 µs | 5.52 µs |
| backward shift | 4 M | 832 k | 1.02 M | 2.88 µs | 5.00 µs |

**Read-your-own-writes** (`--ryow 20000`: put a fresh 512 B value, read it back, gets timed): the pool serves the read from the frame the writer filled at **752 ns p50 / 922 ns p99**, against `pread`'s page-cache read of the bytes it just wrote at 1.31 µs / 2.15 µs, and 151 K pairs/s against 136 K — the insert on append costs less than the page-cache read it saves.

**Cold start and page-cache residency** (`--cold`: every data file fsync'd and `FADV_DONTNEED`'d before each run; each run on its own thread so the engine's thread-local snapshot cannot carry an `mmap` mapping across runs). After a `pread`, `mmap` or buffered-pool run the whole dataset is in the page cache; after a direct-I/O run it holds the active file and nothing else. That column is the deliverable. The cost was readahead: `pread` pulls a cold dataset in 128 KiB reads, and a direct miss was one 4 KiB frame, so the pool took ~40× more device reads to warm. Block fills (below) close that.

**Block fills** (`kPoolFillBlockBytes`, §3). `pool_bench --cold`, `O_DIRECT` rows, Zipf(0.99), 60 k × 512 B, on a SATA SSD (Samsung 860 EVO); ops/s, with p99:

| pool / dataset | frame fills | block fills always | block fills while frames are free |
|---|---:|---:|---:|
| 0.10 | 32.6 K (113 µs) | 6.5 K (409 µs) | 34.4 K (111 µs) |
| 0.25 | 48.5 K (113 µs) | 10.3 K (409 µs) | 53.5 K (112 µs) |
| 0.50 | 75.8 K (103 µs) | 18.1 K (401 µs) | 100.7 K (98 µs) |
| 1.00 | 134 K (97 µs) | 847 K (0.7 µs) | 1.57 M (0.7 µs) |
| 2.00 | 132 K (99 µs) | 1.82 M (0.6 µs) | 1.83 M (0.6 µs) |

Where the pool can hold the working set, frame fills never finish warming it inside the run and block fills do: hit ratio 0.93 → 1.00. Where it cannot, filling blocks unconditionally admits 31 cold neighbours per miss and evicts the hot set (evictions 20 K → 579 K at 0.10); stopping at the first full pool leaves that regime where it was.

MariaDB sysbench `oltp_read_write`, 5 M rows (2.8 GB of data files), 1 GiB pool, 60 s warm-up then 60 s measured, same SATA disk, where `fdatasync` costs ~6.5 ms. With frame fills the pool was still ~85 % empty at the end of every run: 24–28 K misses per run, each queued behind a commit's cache flush (1.9 ms per read against 0.19 ms with no flush in flight), inflating every transaction and stretching the flushes themselves (6.6 → 8.7 ms). With block fills misses fall to hundreds:

| threads | frame fills | block fills |
|---:|---:|---:|
| 1 | 118–134 tps | 135 tps |
| 4 | 211 tps | 313 tps |
| 8 | 377–451 tps (p95 34–45 ms) | 578–654 tps (p95 14–15 ms) |
| 16 | 881 tps (p95 36 ms) | 1144–1596 tps (p95 14–16 ms) |

**RocksDB as a reference.** RocksDB 11.11 built from source, `HyperClockCache` (LRU is the same), 1 GiB cache, bloom filters, no compression, fully warmed, identical keys, values and Zipf stream: 503 K gets/s at 1 thread, p50 1.44 µs, p99 2.93 µs; 1.26 M/s at 4 threads. A `Get` there walks memtable, index and filter blocks before the data block, which is where the difference lives; the block cache itself is fine.

**MariaDB plugin, with and without a memory limit** (`bytecaskdb-mariadb-plugin/benchmarks/memory-pressure/run-memory-pressure.sh`). sysbench on a 10 M-row `sbtest1` (3.2 GB of data files, 20 M keys, ~1.1 GB of key directory), 20 s warm-up then 40 s measured, default `special` (hot-spot) distribution, `verify_checksums=OFF`, sync commits. The limited rows run `mariadbd` in a cgroup with `memory.max = 2.5 GiB`, so the page cache has about 1.2 GB for 3.2 GB of files; the pool is 512 MiB there and 4 GiB (everything resident) without a limit. Page cache dropped before every start. InnoDB is the reference with the same limit and a 1.5 GiB buffer pool (`O_DIRECT`).

| back-end | limit | point select 8 thr | 16 thr | read-write 8 thr | 16 thr (p95) | pool hit |
|---|---|---:|---:|---:|---:|---:|
| pread | none | 55.5 K/s | 60.5 K/s | 876 tps | 917 (25 ms) | — |
| mmap | none | 55.3 K/s | 62.1 K/s | 1102 tps | 1265 (18 ms) | — |
| buffer pool 4 GiB | none | 53.8 K/s | 61.5 K/s | 1046 tps | 1180 (19 ms) | 97–100 % |
| pread | 2.5 GiB | 39.9 K/s | 43.7 K/s | 580 tps | 460 (68 ms) | — |
| mmap | 2.5 GiB | 37.5 K/s | 43.4 K/s | 538 tps | 193 (117 ms) | — |
| buffer pool 512 MiB | 2.5 GiB | 45.3 K/s | 48.6 K/s | 928 tps | 1042 (22 ms) | 93 % |
| InnoDB, 8 GiB buffer pool | none | 54.2 K/s | 57.6 K/s | 852 tps | 1115 (45 ms) | — |
| InnoDB, 1.5 GiB buffer pool | 2.5 GiB | 47.3 K/s | 46.9 K/s | 768 tps | 993 (44 ms) | — |

Without a limit the three back-ends are within a few percent of each other: MariaDB's SQL layer, not the value read, sets the pace on four vCPUs. Under the limit the page cache is the thing being reclaimed, and both `pread` and `mmap` lose 20–30 % on point selects and fall apart on the read-write mix, where the same memory also has to absorb the writes; `mmap` at 16 threads is a sixth of its unlimited throughput with a 117 ms p95. The pool keeps 84–90 % of its unlimited throughput, because the memory it uses is the memory it was given and the kernel has nothing of its to reclaim — the same reason InnoDB with its own buffer pool keeps 87 % of its throughput under the limit. Against InnoDB the pool-backed plugin is level on point selects and ahead on the write mix, mostly in the tail (p95 22 ms against 44 ms). Read-write is `fdatasync`-bound at ~1 ms per commit on this disk, which is why its absolute numbers are small everywhere.

*These rows flatter ByteCaskDB against InnoDB* (#153). The harness then loaded ByteCaskDB's table with its own batched `INSERT ... SELECT`, and `AUTO_INCREMENT`'s doubling reservations for a statement of unknown length left ~24 % of the ids in 1..10 M unused. sysbench draws ids from that whole range, so 13–27 % of point selects, depending on the distribution, asked for a missing row and read nothing; range selects returned fewer rows and updates of missing rows wrote nothing. InnoDB, prepared by sysbench, had no gaps. The harness now prepares both engines with sysbench; the comparison under the limit has to be re-run before these rows are quoted.

**Large values.** `scripts/run_pool_bench_cgroup.sh`: 5 000 keys × 2 MiB (10 GB on disk), so the key directory is ~250 KB and trivially resident, run inside a cgroup with `memory.max = 512 MiB` and no swap, every back-end started cold, Zipf(0.99), 4 000 gets. The pool rows are `O_DIRECT` fills only; a pool filled through the page cache under a limit is two caches and says nothing about the pool. Raw rows in `benchmarks/pool_bench_cgroup_results.csv`.

The first version of `read_at` lost this comparison, and the reason is worth keeping. A 2 MiB value is 512 frames, eviction takes frames rather than values, and a value that had lost one frame was treated as a full miss: the whole 2 MiB was read again and all 512 frames re-admitted, evicting 511 more frames to make room for bytes the pool already held. `read_at` now serves every resident frame and reads only the runs that are missing; a one-frame loss is one 4 KiB read. Same dataset, same limit, before and after:

| back-end | cache | ops/s | p50 | p99 | hit ratio |
|---|---:|---:|---:|---:|---:|
| `pread` | ~500 MiB page cache | 378 | 0.5–1.1 ms | 34 ms | — |
| pool, whole-value refill | 448 MiB | 280 | 1.02 ms | 50 ms | 31 % |
| **pool, missing-runs refill** | 448 MiB | **399** | 1.17 ms | **18.7 ms** | 44 % |
| pool, missing-runs refill | 256 MiB | 341 | 1.18 ms | 24.3 ms | 37 % |
| pool, missing-runs refill | 128 MiB | 293 | 1.21 ms | 33.9 ms | 28 % |
| `mmap` | ~500 MiB page cache | 26 | 68.6 ms | 84 ms | — |

At equal memory the pool now leads `pread` by 5 % on throughput — within this VM's run-to-run noise (`pread`'s own p50 moved 2× between runs) — and by 45 % on the tail, which is not noise. Three things remain on the pool's side of the ledger:

1. **Eviction still takes frames, not values.** (Measured under CLOCK; SIEVE has not been run on this dataset.) 448 MiB holds 224 values and a value-granular cache of that size would hit about 60 % on this Zipf; the pool hits 44 %, because a miss's 512 admissions evict frames scattered across other values in hash order. §9's extent chaining, or a frame size set near the value size, is what closes the rest.
2. **A single 2 MiB `O_DIRECT` read misses readahead pipelining** on this virtual disk. (The per-word atomic copy that was the other item here — ~0.5 ms on 2 MiB — is gone with pinned frames; the table above predates that and has not been re-run.)

`mmap` is unusable here at 26 ops/s: every 4 KiB page of every value faults separately under pressure.

**Eviction policy: SIEVE over CLOCK** (issue #148). The question was whether a different policy would earn its complexity, and it was answered offline before the hit path was touched: the pool's demand accesses were recorded — one record per logical read, whether it hit or not — and replayed through a simulator of the pool's own CLOCK and fill rules, and through libCacheSim's LRU, CLOCK, GCLOCK, S3-FIFO, SIEVE, W-TinyLFU, ARC, 2Q, LIRS and Belady. The tooling is on the `pool-trace-sim` branch. The demand stream does not depend on the cache, so one trace replays at every size. The simulator's replay of the pool matched measured hit ratios to four decimals, twice at sizes predicted before they were measured, which is what licenses reading its other rows as what the pool would do.

Frame hit ratio at a pool of 10 % of each trace's footprint (distinct frames read):

| trace | CLOCK (pool) | admit unreferenced | GCLOCK | SIEVE | S3-FIFO | W-TinyLFU | Belady |
|---|---:|---:|---:|---:|---:|---:|---:|
| `pool_bench` Zipf(0.99), keys scattered | 0.615 | 0.634 | 0.638 | 0.671 | 0.678 | 0.687 | 0.775 |
| sysbench point select, `pareto` | 0.649 | 0.666 | 0.673 | 0.707 | 0.713 | 0.722 | 0.794 |
| sysbench point select, `special` | 0.778 | 0.779 | 0.778 | 0.783 | 0.783 | 0.787 | 0.861 |
| `pool_bench` uniform (control) | 0.100 | 0.100 | 0.100 | 0.101 | 0.101 | 0.102 | 0.412 |

CLOCK was 15 points under Belady on skewed traces; SIEVE recovers most of what practical policies can, within about half a point of S3-FIFO and a point of W-TinyLFU, with a hit path identical to CLOCK's. Its gain depends on admitting frames unvisited (§4); `special`'s hot 1 % fits at this size and its uniform tail cannot be cached by any policy, so everything ties there — at 2 % of its footprint, where the hot set does not fit, SIEVE takes it from 0.546 to 0.749. On `oltp_read_write` (98 % iterator reads) SIEVE's gain is small, 6–9 % fewer misses, because a scan reads nearly everything once.

Measured, before and after, same host, same key stream. `pool_bench` Zipf(0.99) `--scramble`, 2 M × 512 B (1.04 GiB), 16 MiB files, `O_DIRECT`, 5 M reads:

| pool / data | hit ratio | evictions | ops/s | p50 | p99 |
|---:|---|---|---|---|---|
| 0.05 | 0.550 → **0.619** | 2.53 M → 2.15 M | 8.4 K → **10.6 K** (+27 %) | 1.35 → 1.14 µs | 250 → 261 µs |
| 0.10 | 0.617 → **0.671** | 2.14 M → 1.84 M | 10.6 K → **12.4 K** (+16 %) | 1.21 → 1.11 µs | 242 → 239 µs |
| 0.25 | 0.727 → **0.760** | 1.50 M → 1.32 M | 14.9 K → **17.0 K** (+14 %) | 1.09 → 1.04 µs | 233 → 232 µs |

p99 is a device read under both policies; SIEVE makes it rarer, not faster. MariaDB sysbench, 10 M rows, `--rand-type=pareto`, 16 threads, 512 MiB pool, 60 s warm-up and 120 s measured, no memory limit (fills are `O_DIRECT` either way), one run each:

| workload | hit ratio | tps | p95 |
|---|---|---|---|
| point select | 0.790 → **0.820** | 22.3 K → **26.1 K** (+17 %) | 1.10 → 0.95 ms |
| read-write | 0.989 → 0.990 | 369 → 385 | 104.8 → 103.0 ms |

Read-write is commit-bound and was already at 0.99; its change is inside this VM's noise. The hit path is unchanged — `engine_bench` with every read a hit (Get 186 → 185 ns, GetMT × 32 40.3 → 41.0 M/s, Range-50 2.74 → 2.70 µs; the `mmap` rows, which the change does not touch, moved as much).

## 9. Not built

Each of these is a real option; they are listed so the choice is recorded, not rediscovered.

- **Batched fills / `io_uring` / prefetch on miss.** `iter_from` knows its next N keys with no I/O and could issue N fills at once. (Readahead-sized fills, the other half of this item, are built: §3.)
- **Prewarming at open**, newest files first. Measured on sysbench (§8) and not taken: it fills a 1 GiB pool in 4.4 s when idle, but under load it competes with the reads it is meant to spare, and in that workload hotness follows the key, not the file's age — the pool held the newest 1 GiB and still missed 15× more than block fills alone.
- **Entry cache instead of a block cache.** Caches `(file_id, offset) → entry`, holds no headers or superseded entries. Per-object allocator overhead makes it *denser* only when frames are poorly used; measured on the Zipf workload, an evicted frame had served about 20 % of its bytes, which reopens the question without deciding it. Slab calcification has no number attached and is the reason the block cache won.
- **Reads across frames served from views** (#158). A record or value that straddles a frame boundary (about 7 % at 300 B) goes through `read_at`. Serving it instead from `view`s of the frames — one lookup per frame, keeping the pin the first `view` took, one copy, no zero-fill — was built and measured on `engine_bench` `Get` (1M keys, not pinned, medians of five interleaved runs): the B+ tree 0.4 % faster, within noise, the blind tree 1.4 % slower. A straddling read's cost is the per-frame lookups, pins and copies, which any correct path keeps; the change removes one redundant lookup, and the setup it adds to every lend costs the blind tree more than that. The tests it came with stay: `tests/data_file_test.cpp` reads records with the boundary in the header, key, value or CRC, and across several frames.
- **CRC verified on fill** instead of per read. Worth ~25–40 % of hit latency, but entries straddle frames and it changes what `verify_checksums` means.
- **S3-FIFO and W-TinyLFU.** Replayed offline against the same traces as SIEVE (§8): S3-FIFO adds about half a point of hit ratio over SIEVE for two queues and a ghost set; W-TinyLFU about one point, for a frequency sketch written on every hit — a shared write on the path that is lock-free today. Neither pays for itself on these traces. **Liveness-biased eviction** using `file_stats` is still open.
- **Striped fill locks.** One mutex, released across the device read, shows no contention up to the core count measured; striping is available if a 32-thread run shows it.
- **Process-wide pool** for the MariaDB plugin, **online resize**, **huge pages** for the arena (THP already applies where enabled system-wide), **`FADV_SEQUENTIAL`** on scans.
