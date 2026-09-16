# Buffer Pool Design — Proposal

> **Status: proposal / design exploration.** Not a committed plan. No code exists. This document works through the choices for a third `DataFile` back-end — a user-space buffer pool — and lays them out so each one can be settled independently and A/B tested against the two back-ends we already have.

---

## 1. Why this is worth exploring

ByteCaskDB currently has two read back-ends behind the `DataFile` contract, selected by `Options::use_mmap`:

| Back-end | Read path | Memory behaviour |
|---|---|---|
| `ReadOnlyPosixDataFile` / `WritablePosixDataFile` (default) | `pread` per read | Bytes live in the OS page cache; we pay a copy out of it |
| `ReadOnlyMmapDataFile` / `WritableMmapDataFile` | memcpy from the mapping | Bytes live in the OS page cache; mapped into our address space |

Both delegate caching to the kernel. That is a good default and it is free. It has three properties we do not control:

1. **Residency is not ours.** Under memory pressure — especially a cgroup memory limit, where page cache is charged to us — the kernel reclaims our hot pages on its own schedule. Tenet 3 is *predictable latency*; page-cache reclaim is the opposite of predictable.
2. **Granularity is not ours.** The kernel caches in 4 KiB pages with its own readahead heuristics. We already fight this with `POSIX_FADV_RANDOM` and `MADV_RANDOM`.
3. **We cannot see it.** `stats()` reports `disk_reads` and `disk_read_bytes`, but a "disk read" that the page cache served is indistinguishable from one that hit the device. There is no hit ratio to reason about, tune, or alert on.

A buffer pool fixes all three. The reason it is *tractable* here — and the reason this is worth doing in ByteCaskDB specifically rather than in a generic engine — is the observation in the request: **an append-only store does not need a real buffer pool.** It needs a cache.

### 1.1 What append-only removes

A classic buffer pool (InnoDB, Postgres, WiredTiger) spends most of its complexity on write-back:

| Classic buffer pool machinery | Needed here? | Why not |
|---|---|---|
| Dirty bit, flush list, LRU/flush-list split | No | Pages are never modified in place. A frame is either a copy of immutable on-disk bytes or absent. |
| Background writer / checkpointer | No | Nothing to write back. |
| WAL/LSN ordering constraint on eviction | No | The data file *is* the log. |
| Torn-page protection, double-write buffer | No | We never overwrite a page. CRC is per entry, not per page. |
| Page latch, latch upgrade (shared → exclusive) | No | No in-place mutation, so no writer latch. |
| Pool recovery / warm restart | No | Cold start is correct, just slower. |
| Frame allocation, hash table, eviction policy, pin/unpin | **Yes** | This is the whole subsystem. |

What is left is roughly the right-hand column: a sharded hash table over fixed frames, an eviction policy, and a fill path. That is a subsystem we can hold in our heads, which is the bar tenet 2 sets.

### 1.2 What it costs

Being honest up front, because this determines whether the project is worth starting:

- **On a dataset that fits in RAM, it can only lose.** It adds a hash lookup, a policy update and a copy in front of something the kernel already did for free. The current benchmark suite (50 k and 1 M keys) is entirely in that regime. If we A/B on today's benchmarks we will measure the overhead and none of the benefit.
- **It threatens the headline number.** Lock-free reads scaling to 14.0 Mops/s at 32 threads is the strongest result in the README. A buffer pool introduces shared mutable state on the read path. Section 6 is entirely about not losing that, and it has to be measured at 32 threads before anything ships.
- **A miss is a raw device read.** With `O_DIRECT` there is no page cache underneath to absorb a mistake. p99 on a miss gets worse, not better. Tenet 3 says measure the tail, not the mean.

The bet is: **datasets larger than RAM, and memory-limited deployments where residency must be bounded and observable.** Not the 1 M-key benchmark.

---

## 2. Fit with the existing `DataFile` contract

The contract, from `bytecaskdb/data_file.cppm`:

```cpp
scan(Offset) -> optional<pair<DataEntry, Offset>>
read_value(offset, key_size, value_size, verify, io_buf, out)
read_entry(offset, value_size, io_buf) -> DataEntryView
read_entry_unverified(offset, value_size, io_buf) -> DataEntryView
size() -> Offset
```

Three of the five are a clean fit:

- **`scan`** returns an owning `DataEntry`. Nothing to resolve.
- **`read_value`** copies into `out`. Every existing implementation already copies — including the mmap one, which does `out.assign(base, base + value_size)`. A pool-backed implementation copies out of a frame instead of out of a mapping. **`DB::get` and `Snapshot::get` cost exactly the same shape as today.** This is the dominant read path.
- **`size`** is bookkeeping.

### 2.1 The one point of friction: `read_entry` returns spans

`read_entry` / `read_entry_unverified` return a `DataEntryView` whose `key` and `value` are non-owning spans. The two existing implementations satisfy this differently:

- mmap: spans point into the mapping, valid for the lifetime of the `DataFile` — with one exception, noted below.
- pread: spans point into the caller's `io_buf`, valid until the next call with that buffer. `EntryIterator` documents exactly this: *"Spans are valid until the next `operator++()`"*.

**Why mmap gets its guarantee for free, and a pool cannot.** `ReadOnlyMmapDataFile` maps the whole file once: the byte at file offset `N` is always at `mmap_base_ + N`, for the entire life of the object, and the registry's `shared_ptr<DataFile>` keeps that object alive as long as any iterator holds it. Nothing moves or reuses that address. The guarantee is not maintained by the implementation — it is a consequence of a 1:1, address-stable mapping that never recycles.

A pool exists precisely to put more file behind fewer bytes of memory: frame slot 4711 holds `(file 3, frame 900)` now and `(file 7, frame 12)` later. Address stability is the thing a cache trades away in order to be a cache. So a span into a frame is valid only until that frame is recycled, and the recycling decision belongs to whoever next takes a miss:

```
Thread A: for (auto& [k, v] : db.iter_from({}, prefix))   // v spans frame 4711
              ...user code...                              // window is user-controlled
Thread B: db.get(other_key)  -> miss -> pool full -> victim search picks 4711
                             -> refills it with unrelated file content
Thread A: reads v                                          // wrong bytes
```

Two things about this are worth stating precisely, because both are easy to get wrong:

**It does not require a second thread.** `EntryIterator::operator*` hands out a span; the user's loop body calls `db.get(k)`; that read misses, needs a victim, and can select the very frame the live span points into. A single-threaded program is enough.

**A sanitiser will not catch it.** This is not the BC-122 failure mode, where the dangling span pointed at freed heap and ASan reported a heap-use-after-free. Here the arena is still mapped and readable — the memory is valid, the *contents* belong to a different file. And because a frame holds real entry bytes, the garbage is structurally plausible: right shape, plausible lengths, wrong data. The result is a silently wrong value returned to the caller, with no crash and no sanitiser report. That is strictly harder to detect than the bug the coding guidelines hold up as the cautionary tale, which is reason enough to design the hazard out rather than manage it.

**In fairness, the mmap guarantee has one hole.** `resume()` is the only `truncate()` call site, and `WritableMmapDataFile::truncate` performs `munmap` -> `ftruncate` -> `mmap` in place on a live `DataFile`. `WriteBarrier` excludes writers only; reads are lock-free and remain available while the engine is degraded. A reader holding a span from `read_entry` on the active mmap file across a `resume()` therefore holds a genuinely dangling pointer, and since `mmap(nullptr, ...)` may return the same address, it would often appear to work. This has not been shown reachable by a test, and it is pre-existing and orthogonal to this proposal — but the mmap guarantee should not be described as unconditional. Tracked as [#87](https://github.com/gustavoamigo/bytecaskdb/issues/87).

There are three ways out, not two:

**Option R1 — copy out into `io_buf` (recommended).** The pool-backed implementation copies the entry's bytes from its frames into the caller's `io_buf` and returns spans into that, exactly like `ReadOnlyPosixDataFile`. The contract is unchanged, byte for byte, and the caller-visible lifetime rule is the one already documented.

**Option R2 — return a pinned handle.** Change the signature to return an RAII `PinnedEntry` holding a pin on the frames. Zero-copy, but it changes the contract for all three back-ends, and puts a refcount on the hot read path (Section 6.2 explains why that is the expensive choice, not the cheap one). It is also more than "add a refcount": the pin lives as long as the caller holds the view, which for `EntryIterator` is an unbounded, user-controlled window, so a slow loop body can pin an arbitrary fraction of the pool — up to a miss finding no evictable victim at all.

**Option R3 — epoch-based reclamation (QSBR).** Readers publish an epoch on entry; evicted frames go to a retired list and are recycled only once every reader that could have observed them has quiesced. No per-read refcount, so the hot path is cheaper than R2. Rejected for the same unbounded window: an `EntryIterator` span stays live across the user's loop body, so a reader can sit in an epoch indefinitely and stall reclamation *pool-wide*, not just for the frame it is using. That is a worse failure than the one it fixes.

A fourth shape — hand back the span plus a version stamp and require the caller to re-validate before use — is rejected on principle: it makes correctness depend on caller discipline, which the coding guidelines rank below designs where incorrect usage is impossible.

**Decision: R1.** Settled — the pool copies into `io_buf`; R2 and R3 stay on the record as alternatives, not as work. The copy is a memcpy of typically a few hundred bytes (tens of nanoseconds) against a saved syscall or device read (microseconds to hundreds of microseconds). It keeps the new back-end a genuine drop-in, which is precisely what makes the A/B honest — the same test bodies and the same benchmark bodies run unmodified against all three. R2 is a later optimisation with a measurement behind it, not a starting point.

R1 is also the only one of the three in which the dangerous window does not exist, rather than being managed by additional machinery. And there is a second reason for it that is not about simplicity: copy-out is what lets us use optimistic reads instead of pin counts (Section 6.2). Handing out long-lived pointers forces refcounting; refcounting on a hot frame is a contended cacheline; a contended cacheline is how we lose the 32-thread scaling number.

### 2.2 Blast radius

The pool is only reachable from paths that actually touch disk:

| Path | Touches the pool? |
|---|---|
| `DB::get`, `Snapshot::get` (non-empty value) | Yes — `read_value` |
| `iter_from` / `riter_from` (`EntryIterator`) | Yes — `read_entry` / `read_entry_unverified` |
| `keys_from` / `rkeys_from`, `contains_key` | No — pure radix tree walk |
| `get` on a tombstone or empty value | No — short-circuits before the file registry |
| Vacuum, `create_manifest`, `scan_committed` | Via `scan` — see Section 7.4, these should bypass |
| Recovery | No — reads hint files, a separate path |
| Write path (`append_entry`, `append_entries`, `sync`) | Optional — see Section 5 |

---

## 3. The choice axes

Each of the following is an independent decision. Section 9 turns every one of them into a field on `BufferPoolOptions`, so none of them has to be settled by argument — they can be settled by measurement.

### Axis A — cache unit: entry or block?

**A1. Entry cache.** Key `(file_id, offset)`, value the entry bytes.

- CRC-on-fill is exactly right by construction: a cache unit *is* an entry.
- No block arithmetic, no straddling, no read amplification.
- Variable-size values means a slab or size-class allocator and fragmentation management.
- Metadata dominates for small values. A 20-byte value with a 48-byte header is 70 % overhead.
- No spatial locality benefit. Range scans over values — the one benchmark where we lose to RocksDB (28 K vs 68 K scans/s) — gain nothing.
- Under `O_DIRECT` we would read an aligned superset from the device and then throw the neighbours away. We pay for them and discard them.

**A2. Block cache, large frames (64 KiB).**

- Trivial allocator: one arena, fixed frames, free list, no fragmentation.
- Strong readahead for sequential access.
- Brutal read amplification for random point reads: a 100-byte value drags 64 KiB off the device. 640×. This is the workload the engine is *built* for.

**A3. Block cache, device-sized frames (4 KiB), multi-frame extents (recommended).**

- 4 KiB is the `O_DIRECT` alignment unit and the minimum the device will transfer anyway. Amplification over a bare `pread` is therefore approximately zero — the kernel already reads a page minimum today.
- Same trivial allocator as A2.
- Keeps neighbours, which matters: in an append-only file, physical adjacency *is* temporal adjacency. Keys written together are read together in most real workloads. A1 throws this away.
- **Every entry uses the same path regardless of size.** An entry occupying frames `F..F+N` is read by resolving each frame, filling the missing ones, and copying each frame's slice into `out` in order. A 100-byte value touches one or two frames; a 4 MiB value touches 1024. One mechanism, no threshold.

#### Why not a large-value bypass

An earlier draft of this document proposed reading entries above a size threshold directly with `pread`, skipping the pool. That was wrong, for three reasons:

1. **It makes the pool useless for the workload that most needs it.** A store of 1 MiB values would get a 0 % hit ratio by construction. The whole point of the subsystem is to cut I/O, and a bypass switches it off exactly where each avoided read is worth the most.
2. **It makes latency bimodal in value size.** Two entries differing by one byte across the threshold get different orders of magnitude of read latency, and a large value pays full disk cost on *every* read, forever, no matter how hot it is. Tenet 3 asks for latency that is predictable; a size cliff is the opposite.
3. **`bypass_above_bytes` is a knob with no defensible setting.** Nobody can pick it from first principles, and it would be tuned by whoever last got bitten.

Uniformity is the correct default. The bypass survives only in the degenerate form described under *Guard rail* below, where it stops being a performance threshold and becomes a structural invariant.

#### Frames are the unit of memory; extents are the unit of admission

A multi-frame entry does **not** need a linked list to be *found*. Frames are keyed by `(file_id, frame_index)`, so the frames covering an entry are N independent hash lookups, and a contiguous run of misses coalesces into a single batched fill — which the fill interface in Axis D already provides. That alone gives uniformity with no extra machinery.

Chaining earns its place for a different reason: **partial residency of a multi-frame entry is close to worthless.** If any frame of a 1024-frame value is missing, the read still pays a device round trip, and the latency of that round trip is dominated by the seek, not the transfer. Holding 512 of those frames is memory spent for approximately no I/O saved — the worst possible state for the pool to be in, and the state an unaware CLOCK hand will naturally produce.

So the two units are separated:

- **Frame (4 KiB)** — the unit of *memory*. Fixed size, trivial allocator, `O_DIRECT`-aligned.
- **Extent (the frames covering one entry)** — the unit of *admission, eviction and policy*. Admitted together, evicted together, carrying one reference bit rather than one per frame.

Concretely, the chain is policy metadata: frames in an extent link to their successor, the policy operates on extent heads, and evicting a head releases the whole chain. Lookup still goes through the hash table, so a single-frame entry — the overwhelmingly common case — is exactly one lookup and an extent of one, with no chain to walk.

This also fixes a second-order problem: with per-frame reference bits, a hot 1024-frame value sets 1024 bits and is 1024 separate eviction decisions. With one bit per extent, it is one object with one recency, which is what it actually is.

#### What a read touches, and what a frame stores

These are two different questions, and the answer differs.

**On the read path, the key is dead weight — and the pool can skip it entirely.** `read_value` receives `key_size` from the caller, because the caller already *has* the key; it reads the key off disk only to step over it and to feed it into the CRC. So today's `pread` path for a `get` does:

1. `pread` the whole entry, `19 + key_size + value_size` bytes, into `io_buf` — one syscall, one copy;
2. CRC over all of it;
3. `out.assign(...)` — a second copy, of the value alone.

A pool-backed `read_value`, with the CRC already discharged at fill time (Axis C), does:

1. resolve the frames covering `[offset + kHeaderSize + key_size, ... + value_size)` — the **value's** byte range, not the entry's;
2. memcpy those bytes straight into `out`.

The header, the key and the CRC trailer are never touched, never copied, and **`io_buf` is never written at all** on this path. It stays in the signature because the contract requires it, and a pool-backed `read_value` simply ignores it.

Worth keeping the size of this in proportion: the dominant saving is the avoided syscall and the avoided CRC, both measured in microseconds or hundreds of nanoseconds. Not copying the key saves one memcpy of tens of bytes — real, but second-order next to the I/O it replaces. The place the key genuinely costs something is *storage*, below.

**In the frames, the key is stored, and that is a deliberate trade.** A frame is a verbatim image of 4 KiB of file, so it holds headers, keys, CRC trailers and neighbouring entries alongside the values. Storing only values would be denser — the question is by how much, and what it costs:

| value | key | value as % of entry | density gain if values only |
|---:|---:|---:|---:|
| 16 B | 16 B | 31 % | 3.2× |
| 64 B | 16 B | 65 % | 1.6× |
| 256 B | 24 B | 86 % | 1.2× |
| 1 KiB | 24 B | 96 % | 1.04× |
| 4 KiB | 24 B | 99 % | 1.01× |

So a value-only cache is worth 1.5–3× the effective capacity for small values and rounds to nothing above about 256 bytes.

Against that, the device transfer is 4 KiB whether we want it or not — that is the `O_DIRECT` alignment unit and the kernel's minimum besides. At 51 bytes per entry, one aligned read brings in roughly **80 entries**. A block cache keeps all 80 and can serve any of them; a value-only cache keeps one and discards 79 it has already paid to transfer. Whether that matters is entirely a question of whether the workload has spatial locality — and in an append-only file, physical adjacency *is* write-time adjacency, which is exactly the correlation that batch writes, event streams and session data produce.

Density of 1.5–3× against up to 80 neighbours per I/O is not a trade that argument settles. It is Axis A's real content, and it belongs in the A/B rather than in this paragraph: A1 (entry/value cache) versus A3 (block cache) is precisely this question, and the counters in Section 9.2 are what decide it.

One coupling to note before anyone tries a values-only pool: `read_entry` / `read_entry_unverified` **do** need the key, because `EntryIterator` yields `EntryView{key, value}` and sources the key from disk deliberately — `bytecask.cppm:289` records that it "uses `ValueIterator` internally — no key reconstruction during tree walk". A values-only pool could not serve the iterator paths without either reconstructing keys from the radix tree (which `KeyIterator` already does, at a cost that comment was written to avoid) or falling back to disk for the key alone. A block cache serves both paths from the same bytes.

#### Costs, stated honestly

- **Eviction latency is no longer bounded.** Freeing one victim may release 1024 frames, and the thread that pays is the reader that took the miss. That is a tenet 3 problem, and it is the real price of dropping the bypass. Mitigations worth measuring: cap the work done per victim selection and continue the release incrementally, or hand large releases to the background worker. This needs a p99 measurement on a mixed small/large workload before it is called settled.
- **Admission needs N frames at once.** For the default 4 MiB `max_value_bytes` against any sane pool size this is a rounding error — 1024 frames out of a 1 GiB pool is 0.4 %. It only becomes real at the packed ceiling (2^28−1, ≈256 MiB) against a small pool.
- **Large values can still evict the working set**, bypass or not. That is now the eviction policy's problem rather than something special-cased away, which is where it belongs — see the scan-resistance discussion in Axis B and the scan bypass in Section 7.4.

#### Guard rail

One case genuinely cannot be served: an entry larger than the pool, or large enough that admitting it would evict most of the working set to cache a single value. That is not a tuning question, it is an invariant — so the limit is *derived from capacity*, not configured: an entry whose extent exceeds a fixed fraction of the pool (a starting proposal: one eighth) is read directly into `out` and never admitted.

The distinction from the rejected bypass matters. This is not "large values are slow", it is "one value may not evict the cache to hold itself". With the default 4 MiB ceiling it never fires unless the pool is smaller than about 32 MiB, and if it fires often that is a misconfiguration the counters should surface (`pool_oversize_reads`), not a tuning opportunity.

**Recommendation: A3**, frame size configurable with a default of 4096, extents as the admission and eviction unit, and no size-based bypass other than the capacity-derived guard rail.

### Axis B — eviction policy

The request asks for a policy "aligned with how bytecaskdb works". The engine offers four pieces of information a generic pool does not have:

1. **Sealed files are immutable.** A frame from a sealed file can never be stale. There is no invalidation problem to solve, ever — except for the active file (Section 5) and for files vacuum removes (Section 7.3).
2. **Vacuum deletes whole files.** When a file is unlinked, every frame it owns is garbage at once. That is a bulk invalidate, not a per-frame decision.
3. **Vacuum already knows liveness.** `file_stats` carries `live_bytes` / `total_bytes` per file. A file that is 10 % live is 90 % wasted pool. This is a genuinely engine-specific eviction signal.
4. **`file_id` is monotonic.** Newer files are, in most log-structured workloads, hotter. A free recency proxy with no per-frame bookkeeping.

Policy candidates:

| Policy | Hot-path cost | Scan resistance | Notes |
|---|---|---|---|
| **LRU** | List surgery under a lock on *every hit* | None | The reference point in the literature and the worst possible fit for a lock-free read path. Worth implementing **only** as an A/B baseline. |
| **CLOCK / second chance** | One byte, written only when it is currently 0 | Weak | Near-zero hot-path cost. Well understood. The safe default. |
| **S3-FIFO** | One capped counter increment | Good | Three FIFO queues; the small queue absorbs one-hit wonders. FIFO insertion is a tail bump, which is friendlier to concurrency than list surgery. Competitive hit ratios. |
| **W-TinyLFU** | Count-min sketch update | Strong | Best-in-class hit ratios. Most metadata and the most code. |

The hot-path cost column is the one that matters here, and it has a subtlety worth stating: **any unconditional write to a frame's metadata on a hit makes that cacheline exclusive, which serialises concurrent readers of a hot frame.** CLOCK's reference bit must be test-then-set (read, write only if 0) so that a hot frame does zero writes after the first touch. The same discipline applies to any counter in any policy. Getting this wrong is how the 32-thread number dies.

**Recommendation:** CLOCK for v1, behind a policy interface, with S3-FIFO as the first A/B alternative and LRU as a reference baseline. Layer two engine-specific rules on top, both of which are cold-path only:

- **Bulk invalidate by file.** Frames carry `(file_id, generation)`. Unlinking a file or truncating one bumps the generation; a lookup that finds a stale generation is a miss and recycles the frame. O(1), lazy, no scan of the pool.
- **Liveness-biased victim choice.** When the CLOCK hand has a choice, prefer a frame from a file with a low live ratio. One float per `file_id`, consulted only at eviction. Cheap, and a generic pool cannot do it.

A third rule — bias by `file_id` recency — is tempting but should be a tiebreaker at most, never a hard rule. Vacuum rewrites old data into *new* files, so `file_id` is a proxy for write recency, not for content age.

### Axis C — where the CRC happens

This is the axis the request is most specific about: verify on the disk→RAM transfer, not on every read.

Note first that this is a **semantic change to `ReadOptions::verify_checksums`**, and it should be documented as one. Today it means *verify on every read*. Under a buffer pool it means *verify every transfer from disk into memory*. That is the same trade Postgres makes with page checksums, and it is a defensible one — but it means a bit flip in pool memory is no longer caught on read. On ECC memory that is fine. Without ECC it is a real reduction in coverage, and we should say so rather than bury it.

The mechanical problem: **CRC is per entry, frames are per 4 KiB, and entries straddle frames.** You cannot verify a frame in isolation because you do not know where the entries in it begin.

**C1. Verify what you fetch, always.** Verify only the requested entry, on hits as well as misses. No semantic change, no new state — and no benefit. This is the do-nothing option, and it is the honest control arm for the A/B.

**C2. Verified-extent watermark per frame (recommended).** Every read arrives with an exact entry extent: the caller knows `offset`, `key_size` and `value_size`, so the entry occupies `[offset, offset + kHeaderSize + key_size + value_size + kCrcSize)`. Therefore **every read tells the pool about one known entry boundary.** Store two offsets in the frame header, `verified_from` and `verified_to`:

- On a fill triggered by a read at offset `O`, verify the requested entry, then walk forward from its end verifying each successive entry while it lies wholly inside the filled range — stopping at a zero header, which is the zero-fill tail and cannot be a real entry. Set `verified_from = O`, `verified_to =` the end of the last fully verified entry.
- A read of `[a, b)` skips the CRC iff `verified_from <= a && b <= verified_to`. Otherwise it verifies itself and extends the watermark if the new extent is contiguous with it.
- Straddling entries are covered naturally: a multi-frame fill verifies across the boundary, and the watermark on each frame records its own share.

Two `uint16_t` per frame. The forward walk costs CRC over the filled extent — with the hardware CRC-32C instruction, roughly a microsecond for 4 KiB, against a device read measured in tens to hundreds of microseconds. It is off the hot path by construction.

**C3. Re-checksum at frame granularity.** Compute our own CRC over the whole frame at fill time and verify it on access. Rejected: verifying 4 KiB on every read of a 100-byte value is strictly worse than verifying the entry. It has one legitimate use — a background scrubber for long-resident frames — which is a separate feature, not this one.

**Recommendation: C2**, with C1 kept as the A/B control and as the behaviour when the pool is disabled.

One consequence worth calling out: on the write path (Section 5), the writer has just computed every CRC it wrote. Frames inserted by the writer arrive **fully verified for free** — `verified_from` = frame start, `verified_to` = frame end, zero CRC work.

### Axis D — I/O mechanism

Three candidates, and they are not mutually exclusive.

**D1. Buffered `pread` / `preadv`.** What we do today. Fills go through the page cache, so bytes are resident twice — in the pool and in the page cache. Does not meet the "bypass the OS page cache" goal, but it is the right *first* implementation because it isolates the pool's own cost from the I/O mechanism's. If the pool cannot beat the page cache while *using* the page cache, `O_DIRECT` will not save it.

**D2. `O_DIRECT`.** Meets the goal directly.

- **Alignment falls out of Axis A3.** Buffer address, file offset and length must all be multiples of the logical block size. Allocate the arena with `posix_memalign` (or an anonymous `mmap`, which is page-aligned by construction); frames are 4 KiB and frame-aligned; offsets are `frame_index * 4096`. Every constraint is satisfied without special-casing. Discover the real requirement with `statx(STATX_DIOALIGN)` on Linux 6.1+, falling back to the logical block size.
- **It must not be applied to the write path.** Appends are `pwritev` at arbitrary offsets with arbitrary lengths, gathering caller-owned key and value buffers with no alignment. Making those `O_DIRECT` would mean staging every append through an aligned bounce buffer, destroying the zero-copy `pwritev` that the design notes call out explicitly, and it would interact badly with `ensure_zeroed`, which exists to tame ext4 journal behaviour. `O_DIRECT` is also not a durability primitive — `fdatasync` is still required.
- **Mixing `O_DIRECT` reads with buffered writes on the same file is the real hazard.** Linux permits it and the `open(2)` man page warns against it. For the *active* file this is a live correctness concern, not a theoretical one.
- **Portability:** no `O_DIRECT` on macOS (`F_NOCACHE` is the analogue), none under Emscripten, and — importantly for CI — **tmpfs rejects it with `EINVAL`**. The back-end must detect the failure at open and degrade to buffered silently, with a counter recording that it did.

The resolution for the mixing hazard is to **split by lifecycle**, which is exactly what the engine's file states already give us:

| File state | Read path | Write path |
|---|---|---|
| Active | Buffered `pread` on miss, or served from frames inserted by the writer (Section 5) | Buffered `pwritev`, unchanged |
| Sealed | `O_DIRECT` fills into the pool | None |

On seal, the frames already in the pool for that file are correct — sealed content is what was written — so **no invalidation is needed at the transition**. The file is simply reopened with `O_DIRECT` for future misses. Additionally, `posix_fadvise(POSIX_FADV_DONTNEED)` on a file after it is sealed (and therefore after `fdatasync`, so its pages are clean and droppable) releases its page-cache residency. That recovers most of the memory benefit of `O_DIRECT` for the write path without any of the write-path surgery.

**D3. `io_uring`.** The honest answer is that it does nothing for a single synchronous `get`: submit-plus-complete is more work than one `pread`, unless you burn a core on `SETUP_SQPOLL`. It wins only where there are many reads in flight at once, and the public API is synchronous and per-key, so that parallelism has to come from *inside* a single operation. There is exactly one place where it does:

**`iter_from` knows its next N keys without any I/O.** The radix tree walk is pure memory. So a range scan can look ahead N keys, resolve N `(file_id, offset)` pairs, and issue N fills as one batched submission. That is a direct attack on Range-50 — the one benchmark where we lose to RocksDB, and where the README's stated reason is precisely that we fetch each value individually.

That is a real use case, which is what tenet 4 requires before optimising. But `io_uring` in a library is operationally heavy: a ring per thread, registered files and buffers to get the benefit, kernel version gating, and hosts that disable it outright via seccomp — so a runtime fallback is mandatory regardless.

**Recommendation: phase it, and design the seam now.** The pool's fill interface should be a **batch** from day one:

```
fill(span<FillRequest>) -> void        // FillRequest = { file, frame_index, frame* }
```

A single-frame fill is a batch of one. `pread`, `preadv`, `O_DIRECT` and `io_uring` are then four implementations of one interface, switchable by option, with the pool and the policy untouched. Even with the `pread` back-end, the batch interface immediately enables scan readahead by coalescing adjacent frames into one `preadv`.

### Axis E — concurrency structure

See Section 6. Summarised as an axis: **shard count** and **pin discipline** (optimistic seqlock vs. refcount) are both A/B fields.

---

## 4. Recommended v1 shape

Pulling the recommendations together:

```
Frames        4 KiB, fixed, in one aligned arena allocated at open
              (anonymous mmap, optionally MADV_HUGEPAGE and mlock)
Indexing      N independent shards; shard = hash(file_id, frame_index) % N
              each shard owns its table, free list, clock hand and lock
Lookup        open-addressed table per shard, key (file_id, frame_index)
Validity      per-frame (file_id, generation); stale generation == miss
Read          copy out into the caller's io_buf (R1, decided), under an
              optimistic seqlock: read version, memcpy out, re-read version,
              retry or fall back to a direct pread on mismatch
Policy        CLOCK, test-then-set reference bit, liveness-biased victim choice
CRC           C2: verified_from / verified_to watermark per frame
Fill          batch interface; v1 backend = preadv, O_DIRECT on sealed files only
Active file   buffered; frames inserted by the writer as they complete (Section 5)
Entries       frames are memory, extents (frames covering one entry) are the
              unit of admission, eviction and policy; no size-based bypass,
              only a capacity-derived guard rail for entries that would
              evict the working set to hold themselves
Scans         bypass the pool (Section 7.4)
Disabled      buffer_pool_bytes == 0 falls back to today's back-ends exactly
```

Per-frame metadata is roughly 32–48 bytes: `file_id`, `frame_index`, `generation`, seqlock version, reference/frequency byte, flags, `verified_from`, `verified_to`. Against a 4 KiB frame that is about 1 % overhead.

---

## 5. The active file

This is the only mutable file, and therefore the only coherency problem.

The writer already holds `write_mu_` and, after `pwritev`, holds the exact bytes it wrote and the CRCs it computed. So it can populate pool frames directly — **insert on write**. Three points of care:

**Durability before visibility is preserved.** The engine's rule is that `state_.store()` happens after `fdatasync`. A frame populated before the store is not reachable: no published `KeyDirEntry` points at it yet. So the insert is unobservable until the state store makes it observable, which is the same ordering guarantee the write path already provides. Inserting after `pwritev` and before publishing is the natural place.

**Only complete frames are admitted.** The tail frame of the active file is still being appended into, so it is mutable and must not be cached. A frame becomes immutable the moment the write cursor passes its end — and because `ensure_zeroed` has already written zeros through the end of the current 4 MiB chunk, the frame's content at that point is final: real bytes up to the cursor, zeros after. So the rule is simply *admit frame `N` when the cursor passes `(N+1) * frame_bytes`*. The incomplete tail is served by buffered `pread` on the active fd, which is what happens today.

**Reads of the active file that miss fall back to buffered `pread`**, never `O_DIRECT`, for the mixing reason in Axis D.

The payoff is that read-your-own-writes on a write-heavy workload becomes a pool hit with no I/O and no CRC — the writer verified those bytes by constructing them.

`truncate()` (called by `resume()` out of a degraded state) must bump the file's generation, invalidating every frame it owns. `shrink_to_fit()` only releases the zero tail past `size()`, so it needs nothing — no published offset lies in that region.

---

## 6. Not losing the read scaling

14.0 Mops/s at 32 threads is the number most at risk. Three rules.

### 6.1 Shard everything

One pool-wide lock, one pool-wide free list, or one pool-wide clock hand is a serialisation point at 32 threads. Partition the pool into independent shards keyed by `hash(file_id, frame_index)`, each with its own table, free list, hand and lock. This costs a little hit ratio to capacity imbalance and buys scaling on every structure at once. Shard count is an A/B field; something like 4× hardware threads is a reasonable starting guess, to be measured rather than assumed.

### 6.2 Optimistic reads, not pin counts

A pin is `fetch_add` / `fetch_sub` on the frame header. Under copy-out the pin is held for perhaps 50 ns — but the cost is not the duration, it is that 32 threads reading *the same hot frame* are all taking that cacheline exclusive. That serialises them at 50–100 ns each regardless of how short the critical section is.

Use an optimistic seqlock instead: read the frame's version, memcpy the bytes out, re-read the version; if it changed, the frame was recycled underneath us — retry, or on repeated failure fall back to a direct `pread`, which is always correct. The version word is read-mostly, so the cacheline stays shared and readers scale. Eviction bumps the version, so a reader can never hand back bytes from a recycled frame.

This is why Section 2.1 chose copy-out. The two decisions are the same decision: optimistic reads require that we never hand out a pointer that outlives the check.

### 6.3 No unconditional writes on a hit

Covered in Axis B. Test-then-set the reference bit. Never write metadata on a hit unless the value actually changes.

**None of this is a claim.** All three are hypotheses to be measured with `GetMT` at 2/4/8/16/32 threads against both existing back-ends, with a working set that does not fit the pool — otherwise the contention is not exercised.

---

## 7. Interaction with the rest of the engine

### 7.1 Snapshots
A `Snapshot` holds `shared_ptr<DataFile>` for every file it references, so files stay open and vacuum is deferred. The pool changes nothing here: it caches file *contents*, and a file the snapshot holds open cannot be unlinked.

### 7.2 Rotation
The file registry is copy-on-write per rotation. The pool is keyed by `file_id`, which is stable across registry snapshots, so a rotation invalidates nothing. On seal, the sealed file's frames stay valid (Section 5), and it is reopened `O_DIRECT` for later misses.

### 7.3 Vacuum
Vacuum rewrites a sealed file into a new one and unlinks the old. Bump the old `file_id`'s generation at unlink; every frame it owns becomes a lazy miss. The new file starts cold. Vacuum's own read of the source file should bypass the pool (7.4) — it reads every byte exactly once and would otherwise flush the entire cache.

### 7.4 Scans
`scan()`, `scan_committed()` and `create_manifest()` sweep whole files sequentially, exactly once. Admitting those frames is the classic sequential-flooding failure: one vacuum pass evicts the entire working set. Scan paths should bypass the pool and use buffered sequential `preadv` with `POSIX_FADV_SEQUENTIAL` ahead and `POSIX_FADV_DONTNEED` behind, so they leave neither the pool nor the page cache polluted. Whether to bypass or to admit-at-the-cold-end is an A/B field; bypass is the recommended default because it is the one that cannot hurt.

### 7.5 Recovery
Recovery reads hint files, not data files, so it is untouched. A pool-backed `DataFile` does not change the 510 ms / 10 M-key result in either direction.

### 7.6 Emscripten / WASM
No `O_DIRECT`, no `mmap` worth having, and the whole point — bypassing the OS page cache — does not apply to MEMFS, which is already memory. The pool should be compiled out or refuse to enable there, the same way `use_mmap` is rejected today rather than silently ignored.

---

## 8. Honest assessment of where this loses

Stated plainly, because these determine whether to build it:

1. **In-RAM datasets.** Pure overhead. Every current benchmark is in this regime.
2. **The page cache is genuinely good.** It is shared across processes, it is free, it has had decades of tuning, and `O_DIRECT` gives up its ability to absorb a bad access pattern.
3. **Miss latency has no floor underneath it.** Today a "miss" often still hits the page cache. With `O_DIRECT` a miss is a device round trip. Mean throughput may improve while p99 gets worse. Tenet 3 says that is a regression, not a win.
4. **Complexity.** Even reduced to a cache, this is the most intricate subsystem we would have. Section 1.1 is the argument that the reduction is large enough to be worth it; that argument should be re-examined once there is a concrete frame-and-table design to look at, not accepted now.
5. **Sharding costs hit ratio.** A sharded pool of N shards has strictly worse hit ratio than one global pool of the same size, because capacity cannot move between shards. We are trading hit ratio for scaling, deliberately, and should measure both.

The conditions under which it wins are specific and should be the acceptance criteria, not a vague "faster": dataset larger than RAM; or a cgroup memory limit where residency must be bounded; or a deployment that needs an observable, tunable hit ratio.

---

## 9. Making it A/B testable

This is a first-class requirement, so it drives the shape rather than being bolted on.

### 9.1 One option switch, many sub-switches

`Options::use_mmap` is a `bool` and is already at its limit. Replace it with an enum, keeping the third back-end a one-field change:

```cpp
enum class IoBackend { Pread, Mmap, BufferPool };
```

`use_mmap` is pre-1.0 and documented as such; the migration touches `Options`, the C ABI shim, the C++ header shim, the Python and Node bindings, the MariaDB sysvar, and the tests that use `GENERATE(false, true)`. That is mechanical but not zero, and it is worth doing before there is a third value rather than after.

Every axis in Section 3 then becomes a field, so no axis is decided by argument:

```cpp
struct BufferPoolOptions {
  std::size_t capacity_bytes;          // 0 = disabled
  std::size_t frame_bytes;             // Axis A
  unsigned    oversize_guard_divisor;  // Axis A — entry > capacity/N is never admitted
  EvictionPolicy policy;               // Axis B: Clock | S3Fifo | Lru
  bool liveness_biased_eviction;       // Axis B
  CrcMode crc_mode;                    // Axis C: PerRead | OnFill
  bool direct_io;                      // Axis D
  FillBackend fill_backend;            // Axis D: Pread | Preadv | Uring
  std::size_t readahead_frames;        // Axis D — scan lookahead depth
  unsigned shards;                     // Axis E
  PinMode pin_mode;                    // Axis E: Optimistic | RefCount
  bool insert_on_write;                // Section 5
  bool admit_scans;                    // Section 7.4
  bool huge_pages;
};
```

The combination matrix is then machine-enumerable, which is the point.

### 9.2 Counters — the A/B is not meaningful without them

`stats()` already returns a flat `map<string, int64_t>`, so this is pure plumbing onto the existing `Counters` struct:

```
pool_hits, pool_misses, pool_fills, pool_fill_bytes,
pool_evictions, pool_oversize_reads, pool_multi_frame_reads,
pool_frames_released_per_eviction_max,
pool_crc_verifications, pool_crc_bytes,
pool_frames_resident, pool_frames_total,
pool_optimistic_retries, pool_direct_io_fallbacks
```

Hit ratio is the primary A/B metric, and `pool_optimistic_retries` is the early warning for Section 6.2 going wrong. `pool_direct_io_fallbacks` catches the tmpfs case silently degrading in CI. A stats-driven test — "N sequential reads of the same key produce 1 fill and N-1 hits" — is a cheap and precise regression test for the policy.

### 9.3 Benchmarks

The current harness (`scripts/run_engine_bench.py` → `benchmarks/engine_bench_results.csv`) does not parameterise by back-end; only the tests do. Three changes:

1. A back-end selector on `engine_bench` and a new `io_backend` CSV column, so `run_engine_bench.py` can sweep `{Pread, Mmap, BufferPool}` and the CSV stays longitudinally comparable.
2. **A working-set sweep, which is the part that actually matters.** The interesting axis is `pool_bytes / dataset_bytes` at 0.1×, 0.25×, 0.5×, 1.0×, 2.0×. Every existing benchmark runs at effectively ∞, where the result is predetermined. Without this, the A/B measures overhead and nothing else.
3. Latency percentiles on every read benchmark, not just throughput. The CSV already carries `lat_p50_ns` and `lat_p99_ns`; the `O_DIRECT` risk in Section 8.3 is invisible in a mean.

### 9.4 Correctness A/B

The strongest lever, and it is nearly free:

- `tests/data_file_test.cpp` and `tests/bytecask_test.cpp` already use `GENERATE(false, true)` over `use_mmap`. Widening that to `GENERATE(Pread, Mmap, BufferPool)` runs every existing test body against the new back-end unchanged. This is the single best reason to keep the contract byte-identical (Section 2.1).
- A differential test: random `(offset, key_size, value_size)` reads against a pool-backed file compared byte-for-byte with `ReadOnlyPosixDataFile` as the oracle, across pool sizes small enough to force constant eviction.
- The `[model]` recovery tests need extending per the repo's testing rules if any of this changes what a recovered DB contains or reports. As designed it does not — the pool is a read-side cache and the on-disk format is untouched — but the active-file insert-on-write path (Section 5) and `truncate()` invalidation are close enough to the write path to warrant coverage.
- Fault injection: the existing `FAULT_INJECTION` seam should cover fill failure, `O_DIRECT` open failure, and eviction racing a read.

### 9.5 Phasing

Each phase is independently benchmarkable and independently revertible. That is what makes the A/B real rather than a single all-or-nothing comparison.

| Phase | Content | What it answers |
|---|---|---|
| 0 | Back-end enum, counters, benchmark sweep with a working set that exceeds RAM. No pool. | What is the baseline, and can we even see the regime where a pool matters? |
| 1 | Pool with buffered `preadv` fills, CLOCK, sharded, copy-out, C2 CRC. | Does the pool beat the page cache while still using it? Does 32-thread scaling survive? |
| 2 | `O_DIRECT` on sealed files, `FADV_DONTNEED` after seal. | What does bypassing the page cache actually buy, and what does it cost at p99? |
| 3 | Insert-on-write for the active file. | Does read-your-own-writes improve on a write-heavy mix? |
| 4 | Batched fills for scan readahead, then `io_uring` behind the same fill interface. | Does Range-50 move? |

If Phase 1 does not show a win in the regime Phase 0 establishes, the honest outcome is to stop — and Phase 0 is cheap enough to be worth building regardless, because the missing hit-ratio visibility and the missing out-of-RAM benchmark are gaps today, pool or no pool.

---

## 10. Open questions

1. **Does `verify_checksums` keep its meaning?** Section 3 Axis C changes it from per-read to per-transfer. Is that acceptable as a silent change of meaning under a different back-end, or does it need a separate option so the two are not conflated?
2. **Is the entry cache (A1) really dismissed?** It is the only option that makes CRC-on-fill trivially correct with no per-frame state at all, and storing values without their keys and headers is worth 1.5–3× effective capacity below ~64-byte values (Axis A, *What a read touches*). Against that it discards ~79 of the ~80 entries every aligned device read already paid for, and it cannot serve the `read_entry` / `EntryIterator` paths without key reconstruction. Density versus locality is a measurement, not an argument — it should be settled by the A/B, not here.
3. **Should the pool be per-DB or process-wide?** Per-DB is simpler and matches `Options`. Process-wide matters for the MariaDB plugin, where many tables would otherwise each carve out their own fixed arena.
4. **Is `O_DIRECT` alone enough, or do we want `RWF_NOWAIT` / `preadv2` as a "hit the page cache or tell me you missed" probe?** That would let us keep the page cache as a free second tier without giving up control.
5. **What is the acceptance bar?** Section 8 argues it should be a hit ratio and a p99 at a stated working-set ratio, not a throughput number on the existing benchmarks. That bar should be agreed before Phase 1, not after.
6. ~~Is the `resume()` / `WritableMmapDataFile::truncate` span hazard (Section 2.1) reachable in practice?~~ Filed as [#87](https://github.com/gustavoamigo/bytecaskdb/issues/87). Pre-existing and independent of this proposal; not a dependency for any phase here.
