# UnorderedView Design

UnorderedView is a linear hashing layer over ByteCaskDB that maps arbitrary key distributions (UUIDv4, SHA-256, random binary) into sequential bucket keys that the radix tree handles efficiently. Built entirely on the public `DB` API — no engine internals required.

**Status**: benchmark prototype (`benchmarks/unordered_view.h`), not part of the library yet.

## Problem

ByteCaskDB's radix tree compresses keys that share common prefixes. Time-ordered keys (UUIDv7, incrementing IDs) compress well; high-entropy keys (UUIDv4, SHA-256) share no prefixes and consume ~50–60 bytes of key directory per key. UnorderedView reduces this overhead by mapping arbitrary keys to short, structured bucket keys.

## Storage layout

```
DB Key:    <ns>/b/<4B bucket_id BE><2B hash16 BE>     (11 bytes for ns="uv")
DB Value:  [{key_len:4B}{original_key}{val_len:4B}{user_value}]+
```

- **Slot key**: namespace prefix + 4-byte big-endian bucket ID + 2-byte big-endian fingerprint. Fixed-size, lexicographically ordered.
- **Chain value**: concatenated entries. Multiple entries in a single chain arise only on hash16 collisions (expected rate: ~1/65536 per bucket slot). `val_len == 0` signals a tombstone.

Metadata keys:
- `<ns>/__meta__` — 16 bytes: `initial_size(4) | split_pointer(4) | round(4) | version(4)`.
- `<ns>/__bloom__` — collision filter bitmap (see below).

## Hashing

Two-stage hash via FNV-1a + Fibonacci multiplication:

1. **FNV-1a** hashes arbitrary byte sequences to 64 bits.
2. **Fibonacci multiplication** (`h * 0x9E3779B97F4A7C15`) mixes into the desired width:
   - `fib_hash32`: upper 32 bits → bucket routing.
   - `fib_hash16`: upper 16 bits → fingerprint for sub-bucket slot addressing.

## Routing (linear hashing)

```
route(key):
    h  = fib_hash32(fnv1a(key))
    n  = initial_size * 2^round
    b  = h % n
    if b < split_pointer:
        b = h % (2n)
    return b
```

The address space grows one bucket at a time. When `entry_count > num_buckets * bucket_capacity * load_factor`, the bucket at `split_pointer` is split: entries are re-routed to either the original bucket or the new bucket at `split_pointer + n`. The split pointer advances; when it reaches `n`, the round increments and the pointer resets to 0.

Splits are atomic: a `WritePlan` with a snapshot guard on `__meta__` deletes old slot keys, writes new ones, and updates metadata in a single `apply_batch`.

## Write path

Three paths, fastest first:

| Path | Condition | Cost |
|------|-----------|------|
| **Append** | Slot absent (`contains_key` = false) | 1 write, 0 reads |
| **Bloom skip** | Slot exists, bloom says no collision | 1 read + 1 write |
| **RMW** | Slot exists, bloom says maybe collision | 1 read + 1 write (+ bloom write on new collision) |

### Path 1 — Append (common case)

The slot key doesn't exist. Encode the chain entry and write it directly. No disk reads.

### Path 2 — Bloom skip

The slot exists but the collision filter says this fp16 slot has never had a hash collision. Read the chain to verify: if the existing key matches, overwrite directly. If it doesn't match, this is the first collision — set the bloom bit and do a full RMW. The bloom + chain are persisted atomically via `apply_batch`.

### Path 3 — RMW

The bloom says this slot may have a collision. Read the chain, scan for the key, rebuild the chain with the new/updated entry. If the key is new to the slot (true collision), update the bloom atomically.

### Split trigger

After each put, if `entry_count > num_buckets * bucket_capacity * load_factor`, the bucket at `split_pointer` is split. The split:

1. Scans all slot keys for the bucket (keys-only, in-memory).
2. Reads each slot chain, deduplicates (last-write-wins), GCs tombstones.
3. Re-routes entries to new buckets under `h % (2n)`.
4. Atomically deletes old slots, writes new slots, and updates metadata.

## Read path

```
get(key):
    bucket = route(key)
    fp     = hash16(key)
    chain  = db.get("<ns>/b/<bucket><fp>")
    return find_in_chain(chain, key)
```

One point-lookup, one disk read. No scan, no iteration.

## Collision filter (bloom)

A bloom filter tracks which `(bucket_id, hash16)` slots have true hash collisions (>1 distinct key). This lets the write path skip the read-modify-write cycle for the vast majority of puts where a slot has exactly one key.

**Sizing**: expected collision count ≈ `N² / (2 * B * 65536)` where N = total keys, B = num buckets. The bloom is sized for 10× this estimate with a floor of 1024 items. For 1M keys pre-sized to 262K buckets, expected collisions ≈ 29, bloom capacity = 1024 items ≈ few KB.

**Persistence**: the bloom is stored as a single DB value at `<ns>/__bloom__`. On collision detection, the bloom is updated and persisted atomically with the chain update via `apply_batch`.

**False positives**: a bloom FP causes an unnecessary RMW read but no correctness issue. The configured FP rate (default 1%) applies to the collision slot population, not the total key count.

## Pre-sizing via `capacity`

When the expected key count is known, setting `Options.capacity` computes `initial_size = next_pow2(capacity / bucket_capacity / load_factor)`, eliminating all splits during population. This reduces write amplification from ~3× to ~1×.

The computation uses `std::bit_ceil` (C++20) and only applies to new views — on reopen, `initial_size` is restored from the persisted metadata.

## Options

| Field | Default | Description |
|-------|---------|-------------|
| `initial_size` | 8 | Starting number of buckets (power of 2). Overridden when `capacity > 0`. |
| `bucket_capacity` | 64 | Entries per bucket before a split is triggered. |
| `load_factor` | 0.75 | Fraction of capacity before split. |
| `bloom_fp_rate` | 0.01 | False-positive rate for the collision filter. |
| `capacity` | 0 | Expected key count. 0 = no pre-sizing. |

## Concurrency

- **Writes** are serialized under a `std::mutex`. `put`, `del`, and `split` hold the lock.
- **Reads** (`get`, `contains_key`) are lock-free — they go directly through `db.get()`.

## Memory profile (1M keys, 16-byte UUIDv4 binary keys, 245-byte values)

| Configuration | Heap | Splits | Entries moved | B/key overhead |
|---------------|------|--------|---------------|----------------|
| UnorderedView (capacity=1M) | 56.3 MiB | 0 | 0 | 42.8 B |
| UnorderedView (naive, init=8) | 57.9 MiB | 20,826 | 1,421,618 | ~24 B |
| Direct radix tree (UUIDv7) | 47.2 MiB | n/a | n/a | ~31 B |

Pre-sizing eliminates all split I/O. The naive configuration uses less heap because splits compact the key space over time, but incurs ~3× write amplification. Direct UUIDv7 achieves the lowest heap because time-sorted keys compress well in the radix tree.

### Running the memory profile

```bash
# Build
xmake build memory_profile

# Pre-sized (capacity=1M, 0 splits)
BC_DATASET_SIZE=1000000 BC_KEY_FORMAT=uuidv4_binary \
  BC_USE_UNORDERED_VIEW=1 BC_UV_CAPACITY=1000000 \
  xmake run memory_profile

# Naive (no pre-sizing, ~20K splits)
BC_DATASET_SIZE=1000000 BC_KEY_FORMAT=uuidv4_binary \
  BC_USE_UNORDERED_VIEW=1 \
  xmake run memory_profile

# Direct radix tree baseline
BC_DATASET_SIZE=1000000 BC_KEY_FORMAT=uuidv7_binary \
  xmake run memory_profile

# Scripted comparison
python3 scripts/run_memory_profile.py --unordered-view --key-formats=uuidv4_binary --sizes=1000000
```

### Write amplification simulation

```bash
# Byte-level write amplification model
python3 scripts/simulate_write_amp.py --keys 1000000

# Parameter sweep (bucket_capacity, load_factor, initial_size)
python3 scripts/unordered_view_sim.py
```

## Files

| File | Role |
|------|------|
| `benchmarks/unordered_view.h` | Implementation |
| `benchmarks/unordered_view_test.cpp` | Tests (8 test cases) |
| `benchmarks/memory_profile.cpp` | Heap and RSS measurement harness |
| `benchmarks/engine_bench.cpp` | Benchmark adapter (`BcUnorderedViewAdapter`) |
| `scripts/run_memory_profile.py` | Automated memory profile runner |
| `scripts/simulate_write_amp.py` | Byte-level write amplification simulator |
| `scripts/unordered_view_sim.py` | Parameter sweep simulator |
