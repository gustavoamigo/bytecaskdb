# SparseIndexView Proposal

> **Status: proposal / design exploration.** Not a committed plan. This document explores a two-tier key layout that trades cold-key read latency for dramatically reduced radix tree overhead, lifting the RAM ceiling for workloads with clear hot/cold access patterns.

---

## 1. Problem

ByteCaskDB's radix tree keeps all keys in memory. Per-key overhead is ~50–70 bytes depending on key length and prefix structure. At scale:

| Keys | RAM (key directory only) |
|---:|---:|
| 10M | ~500 MB–700 MB |
| 100M | ~5–7 GB |
| 1B | ~50–70 GB |

This is the source of flat, predictable read latency — but it imposes a hard RAM ceiling on dataset size.

Not all workloads need sub-microsecond reads on every key. Log sinks, event stores, audit trails, and any system that accumulates keys over time share a common pattern: recent data is hot, old data is cold. Paying full radix tree overhead for keys that are rarely or never read individually is wasteful.

---

## 2. Core Idea

Two tiers within a single DB instance, distinguished by key prefix:

- **Hot tier**: regular ByteCaskDB keys, individually indexed in the radix tree. Full read performance. This is where new writes land.
- **Cold tier**: groups of keys packed into blocks stored as single DB values. The radix tree indexes one entry per block, not per key.

At 1B cold keys packed into blocks of 1000: **1M radix tree entries** (~50 MB) instead of 1B entries (~50 GB).

The boundary between hot and cold is **caller-driven**. SparseIndexView provides a `compact()` method that moves a range of hot keys into a cold block. The caller decides when to invoke it — time-based, count-based, or external trigger.

Built entirely on the public `DB` API (`put`, `get`, `del`, `del_range`, `iter_from`, `keys_from`, `apply_batch`, `snapshot`). No engine internals required. Same pattern as UnorderedView.

---

## 3. Storage Layout

### 3.1 Key naming

```
<ns>:hot:<user_key>                  → user value
<ns>:cold:<block_start_key>          → packed block (index + keys + values)
<ns>:cold:<block_start_key>:meta     → block metadata
<ns>:__meta__                        → view-level metadata
```

`<ns>` is a constructor argument (namespace), allowing multiple independent views in one DB. Double-underscore keys are internal control keys.

Block start keys use big-endian encoding for any numeric components to preserve lexicographic ordering in the radix tree.

### 3.2 Block format

Each cold block is a single DB value containing a sorted index followed by key and value data:

```
┌─────────────────────────────────────────────────────┐
│  num_entries     u32 LE                              │  ← 4-byte header
├─────────────────────────────────────────────────────┤
│  key_ends[]      u32 LE × (N+1)                     │  ← cumulative key end offsets
├─────────────────────────────────────────────────────┤
│  value_ends[]    u32 LE × (N+1)                     │  ← cumulative value end offsets
├─────────────────────────────────────────────────────┤
│  key_0 | key_1 | ... | key_N-1                      │  ← key region (sorted, contiguous)
├─────────────────────────────────────────────────────┤
│  val_0 | val_1 | ... | val_N-1                      │  ← value region
└─────────────────────────────────────────────────────┘
```

`num_entries` is stored in the block header so the read path can determine the size of the `key_ends` array without reading the metadata key. Keys are sorted and stored contiguously in the key region.

The layout is struct-of-arrays: key routing data (`key_ends`) is separated from value data (`value_ends` + value region). Binary search only touches the `key_ends` array (~4 KB for 1000 entries, fits in L1 cache) and ~10 key slices in the key region. The value offsets and value region are never accessed until a match is found.

Key `i` occupies `key_region[key_ends[i-1]..key_ends[i]]` (with `key_ends[-1]` defined as 0). Value `i` occupies `value_region[value_ends[i-1]..value_ends[i]]`.

### 3.3 Block metadata

Stored at `<ns>:cold:<block_start_key>:meta`:

```
num_entries        u32 LE   4 bytes
end_key_len        u16 LE   2 bytes
end_key            bytes    (end_key_len)
block_version      u32 LE   4 bytes
```

The start key is implicit from the block key itself. The end key enables routing: given a query key, find the block whose range `[start_key, end_key]` contains it.

### 3.4 View-level metadata

Stored at `<ns>:__meta__`:

```
version            u32 LE   4 bytes   (CAS sentinel)
block_size_target  u32 LE   4 bytes   (entries per block)
```

The `version` field enables optimistic concurrency via `ensure_unchanged` on the metadata key.

---

## 4. Read Path

### 4.1 Hot read (fast path)

```
get(user_key):
    found = db.get(<ns>:hot:<user_key>, out)
    if found: return out
    → fall through to cold path
```

One radix tree lookup + one disk read. Same latency as a bare `db.get()`.

### 4.2 Cold read

```
get(user_key) continued:
    block_key = rkeys_from(<ns>:cold:<user_key>).first()   // nearest block ≤ key
    view = non_owning_view(block_key)    // span into mmap'd region, no copy
    N = read_u32(view[0..4])             // num_entries from block header
    return binary_search(view, N, user_key)
```

The block value is accessed through a non-owning view (a span into the mmap'd sealed file), not copied into a buffer. No metadata key read is needed — `num_entries` is in the block header, and if the key isn't in the block, the binary search returns not-found directly.

**Binary search within a block:**

1. Read `key_ends[mid]` and `key_ends[mid-1]` from the offset array at the front of the block. This array is contiguous and small (~4 KB for 1000 entries) — fits in L1 cache after first access.
2. Slice into the key region at `key_ends[mid-1]..key_ends[mid]` to read the key bytes.
3. Compare with the search key.
4. Repeat ~10 times for a 1000-entry block.
5. On match at position `found`: read `value_ends[found-1]` and `value_ends[found]` to locate the value in the value region. This is the first and only access to value data.

Total bytes touched during a cold read: ~4 KB (key offset array) + ~10 key slices (a few hundred bytes) + one value slice. The rest of the block — all other values and the value offset array — is never read.

### 4.3 Hot shadows cold

If a key exists in both tiers (hot write to a key that was already compacted), the hot version wins. The cold copy is stale until the next compaction of that range.

---

## 5. Write Path

All writes go to the hot tier:

```
put(user_key, value):
    db.put(<ns>:hot:<user_key>, value)

del(user_key):
    db.del(<ns>:hot:<user_key>)
```

SparseIndexView never writes directly to cold blocks during normal operation. Moving data from hot to cold is a separate compaction step.

---

## 6. Compaction (Hot → Cold)

### 6.1 Trigger

Caller-driven. SparseIndexView exposes:

```cpp
// Compact hot keys in [from, to) into cold blocks.
void compact(BytesView from, BytesView to);

// Compact the oldest N hot keys.
void compact_oldest(std::size_t n);
```

The caller decides policy: time-based (compact keys older than T), count-based (when hot tier exceeds N keys), or manual.

### 6.2 Mechanics

1. Take a snapshot.
2. Scan hot keys in the target range via `snap.iter_from(<ns>:hot:<from>)`.
3. Collect entries in groups of `block_size_target`.
4. For each group:
   - Sort entries by key (should already be sorted if time-ordered).
   - Build the packed block value (header + index + keys + values).
5. Build a `WritePlan` with the snapshot:
   - `put(<ns>:cold:<first_key>, packed_block)` — write the block.
   - `put(<ns>:cold:<first_key>:meta, metadata)` — write block metadata.
   - `del(<ns>:hot:<key>)` for each compacted hot key — remove from hot tier.
   - `ensure_unchanged(<ns>:__meta__)` — CAS guard.
6. `db.apply_batch(opts, std::move(plan))` — atomic commit.

If any hot key was modified between the snapshot and commit, the batch is rejected. Retry.

### 6.3 Atomicity

The `WritePlan` guarantees that the block write and hot-key deletions are atomic. At no point can a reader see a state where hot keys are deleted but the cold block is not yet visible (or vice versa).

---

## 7. Retention

Cold blocks are stored under a lexicographically ordered prefix. Deleting a time range of cold data:

```cpp
// Delete all cold blocks with start keys before cutoff.
db.del_range({}, to_bytes("<ns>:cold:"), to_bytes("<ns>:cold:" + cutoff));
```

One append to the data file, regardless of how many blocks fall in the range. The radix tree walk is in-memory.

Retention granularity is **per-block**, not per-key. If blocks contain 1000 entries spanning 1 hour each, the minimum retention granularity is 1 hour. Larger blocks trade finer retention for more RAM savings.

---

## 8. Range Queries

A range query over user keys merges results from both tiers:

1. Open an iterator on `<ns>:hot:` for hot keys in the range.
2. Open an iterator on `<ns>:cold:` for cold blocks overlapping the range.
3. For each cold block: read it, iterate its sorted entries.
4. Merge-join the two sorted streams by key order.

Hot entries shadow cold entries with the same key (same rule as point reads).

Cross-tier range queries are slower than single-tier queries due to block I/O. For workloads that scan only recent (hot) data, this cost is never paid.

---

## 9. Cold Deletes

### P0: not supported

Individual deletes from cold blocks are not supported. Retention is the intended deletion mechanism: `del_range` on the block prefix removes entire blocks.

### P1: tombstone bucket

A tombstone bucket stores individual cold-key deletions without rewriting blocks:

```
<ns>:cold:<block_start_key>:tomb → packed set of deleted keys within this block
```

The read path checks the tombstone bucket after finding a key in a cold block. If the key appears in the tombstone set, the read returns not-found.

On the next compaction pass over a range with tombstone buckets, the compactor rebuilds the block without the tombstoned entries and deletes the tombstone key — a background cleanup, not a hot-path cost.

---

## 10. Memory Analysis

**Baseline (all keys in radix tree):**

| Keys | Per-key overhead | RAM |
|---:|---:|---:|
| 1B | ~50 B | ~50 GB |

**With SparseIndexView (1B total, 1M hot, 999M cold, block_size=1000):**

| Component | Entries | Per-entry | RAM |
|---|---:|---:|---:|
| Hot keys | 1M | ~50 B | ~50 MB |
| Cold block keys | 999K | ~50 B | ~50 MB |
| Cold metadata keys (if kept) | 999K | ~50 B | ~50 MB |
| **Total (with meta)** | | | **~150 MB** |
| **Total (without meta)** | | | **~100 MB** |

RAM reduction: **~330x–500x** (100–150 MB vs 50 GB).

The per-entry overhead depends on key length and prefix compression. These figures use the ~50 B/key baseline for well-structured keys (from the radix tree memory profile). Actual overhead varies.

---

## 11. Expected Characteristics

| Operation | Hot tier | Cold tier |
|---|---|---|
| Point read | 1 tree lookup + 1 disk read | 1 reverse tree lookup + binary search over mmap'd view |
| Point write | 1 `db.put` | N/A (writes always go hot) |
| Delete (individual) | 1 `db.del` | Not directly supported |
| Delete (range/retention) | `del_range` | `del_range` on block prefix |
| Range scan | Standard `iter_from` | Block reads + merge |
| Compaction | — | I/O-bound: read N values, write 1 block, delete N hot keys |

Cold reads add one metadata read over hot reads. The binary search within the block operates on a non-owning mmap'd view — only the key offset array (~4 KB) and ~10 key slices are touched. The value region is read only once, only for the matched key.

---

## 12. Trade-offs and Limitations

1. **Cold reads are slower.** One reverse tree lookup to find the block, then a binary search over a mmap'd view. With warm OS page cache, only a few KB of the block are touched. With cold pages, the OS faults in only the accessed pages — not the entire block.

2. **Cold individual deletes are deferred to P1.** P0 relies on range-based retention. P1 adds a tombstone bucket per block (see §9) to support individual cold-key deletes without rewriting blocks.

3. **Hot shadows cold.** If a key exists in both tiers, the hot copy wins. The cold copy becomes stale until the block is rebuilt. The read path must check hot first.

4. **Retention granularity is per-block.** Larger blocks save more RAM but coarsen the retention boundary.

5. **Compaction generates write amplification.** Each compaction writes tombstones for every deleted hot key. Vacuum reclaims this space over time.

6. **Value size limits constrain block size.** Default `max_value_bytes` is 4 MiB. At 256-byte values: ~15,000 entries per block. At 4 KB values: ~1,000 entries per block. At 64 KB values: ~60 entries per block.

7. **Cross-tier range queries require merge logic.** More complex than a single `iter_from` call, and each cold block incurs I/O.

8. **No automatic compaction.** The caller drives compaction policy. This is deliberate — the view cannot know the workload's hot/cold boundary.

---

## 13. Open Questions

1. **Block assignment policy.** For time-ordered keys (timestamps, sequence numbers), blocks map naturally to contiguous ranges — compaction collects the next N hot keys and they form a block. For non-incremental keys (UUIDs, hash-prefixed keys), there is no natural contiguous range. A block assignment policy decides how keys are grouped into blocks: by insertion order, by key prefix, or by some other criterion. This affects both block locality (do keys within a block share prefix structure?) and cold-read routing (can you find the right block without scanning all metadata?).

2. **Tombstone bucket GC.** When should tombstone buckets trigger a block rewrite? Options: (a) at the next compaction pass that touches the range, (b) when the tombstone count exceeds a fraction of the block's entry count, (c) never — let retention delete the whole block eventually. The answer likely depends on the workload.

3. **Metadata key role.** The read path no longer uses the `:meta` key — `num_entries` is in the block header and key lookup is resolved by binary search. The metadata key still stores `end_key` and `block_version` for administrative use (retention boundary checks, range queries, format migration). Should it be kept, or can `end_key` be derived from the block's last key entry and the version embedded in the block key?

---

## 14. Use Cases

**Log sink**: keys are timestamps or sequence numbers, values are log entries (100 B–4 KB). Hot tier holds recent logs (minutes to hours). Compaction runs periodically to move old logs to cold blocks. Retention deletes blocks older than the configured window.

**Event store**: keys are `<stream>:<sequence>`, values are event payloads. Hot tier holds the active write window. Cold blocks group events by stream prefix. Range queries over a stream's history merge hot and cold tiers.

**Audit trail**: append-only, rarely queried by individual key, range-queried by time window. Cold tier dominates. Hot tier is a thin buffer before the next compaction run.

**General case**: any workload where key count grows without bound and access patterns have temporal or prefix-based locality. The caller defines what "cold" means for their domain.
