# Parallel Recovery Design

## 1. Motivation

ByteCask recovery rebuilds the in-memory key directory by scanning hint files
(or raw data files) from disk. At large key counts the bottleneck is radix tree
insertion, not I/O:

| Keys | WithHints (serial) | Bottleneck |
|------|--------------------|------------|
| 1M   | 277 ms  | Tree insertion: ~95% |
| 10M  | 3,080 ms | Tree insertion: ~95% |
| 100M | ~33 s   | Tree insertion: ~95% |
| 1.84B (2 TB) | ~12 min | Tree insertion: ~95% |

The current `recover_existing_files` loop is fundamentally serial: a single
`TransientRadixTree` is updated one entry at a time across all hint files.
This design proposal replaces that loop with a **key-partitioned parallel
build** followed by a **disjoint-tree merge**, achieving ~11x speedup at the
2 TB scale on a 16-core machine.

See `docs/recovery_time_estimates.md` for the full baseline measurements.

---

## 2. Design

### 2.1. Key insight: partition by key, not by file

A natural parallelisation would be to assign each file to a different thread.
That creates a cross-shard correctness problem: a `DELETE "foo"` in file 3
must suppress a `PUT "foo"` in file 1 regardless of which shard processes
which file, requiring a shared tombstone map and synchronisation.

The better partitioning is by **key**: `hash(key) % P`. Because a key always
routes to the same partition, the `PUT` and its superseding `DELETE` always
land in the same partition. Each partition owns its tombstone map with no
cross-partition interaction.

This gives two properties that unlock efficient parallelism:

1. **Zero cross-partition synchronisation** during the build phase.
2. **Provably disjoint trees** after the build phase — the merge can adopt
   entire subtrees by pointer copy with no key comparisons.

### 2.2. Architecture overview

```
    +-------------------------------------------------------------------+
    |  Serial pre-pass: directory scan, assign file_ids, build          |
    |  file registry (DataFile objects). No tree work yet.              |
    +-------------------------------------------------------------------+
                                    |
                                    v  F files, IDs 0..F-1 known
    +-------------------------------------------------------------------+
    |  Reader thread pool (R threads)                                   |
    |  Scan hint files via scan_view(). For each entry compute          |
    |  p = hash(key) % P and push into queue[p].                        |
    +-------------------------------------------------------------------+
        | queue[0]           | queue[1]   ...    | queue[P-1]
        v                    v                   v
    +-----------+        +-----------+        +-----------+
    | Builder 0 |        | Builder 1 |  ...   |Builder P-1|
    | TransTree |        | TransTree |        | TransTree |
    | tombstones|        | tombstones|        | tombstones|
    +-----------+        +-----------+        +-----------+
        |                    |                     |
        +--------+-----------+                     |
                    v merge(T0,T1)  merge(T2,T3)   |
                [T01]      [T23]  ...              |
                    v merge(T01,T23) ...           |
                            final persistent tree <-+
```

Merge rounds 1 through `log2(P)-1` run in parallel. The final merge is serial.

### 2.3. Serial pre-pass

Before any thread launches:

1. Collect all `.data` paths, assign sequential `file_id`s.
2. Construct and seal `DataFile` objects; populate `s.files`.
3. Remove stale `.hint.tmp` files.

The resulting `file_id -> DataFile` registry is read-only from this point.
All builder threads reference it without synchronisation.

### 2.4. Reader threads

R reader threads (default: `min(hardware_concurrency, num_hint_files)`) each:

1. Open their assigned hint files via `HintFile::OpenForRead`.
2. Scan with `scan_view()` — zero heap allocation per entry.
3. For each entry compute `p = fnv1a(key) % P`.
4. Push the entry into `queues[p]`.

If a hint file is absent, the reader falls back to raw `DataFile::scan`.
Readers and builders run concurrently; builders drain queues as entries arrive.

### 2.5. Builder threads

P builder threads each own:

- A `TransientRadixTree<KeyDirEntry>` — initially empty.
- A `std::unordered_map<Key, uint64_t> tombstones`.

On each entry received:

```
if entry_type == Put:
    if tombstones[key] >= seq: skip       // superseded by higher-seq Delete
    if existing = tree.get(key):
        if existing.sequence >= seq: skip
    tree.set(key, KeyDirEntry{seq, file_id, file_offset, val_size})
    max_lsn = max(max_lsn, seq)

if entry_type == Delete:
    tombstones[key] = max(tombstones[key], seq)
    if existing = tree.get(key):
        if existing.sequence < seq: tree.erase(key)
    max_lsn = max(max_lsn, seq)
```

This is identical to today's `apply_put`/`apply_del` — just partitioned.
On EOF the builder calls `transient.persistent()` and reports
`(frozen_tree, max_lsn)`.

### 2.6. Disjoint tree merge

Because all keys in partition i are disjoint from all keys in partition j,
the merge of any two partition trees involves no key conflicts.

The merge walks both trees in DFS lockstep:

```
merge_disjoint(node_a, node_b):
    if node_a is null: return node_b    // adopt B's entire subtree — O(1)
    if node_b is null: return node_a    // adopt A's entire subtree — O(1)

    // Both non-null: merge child by child
    for each child byte c:
        result.child[c] = merge_disjoint(node_a.child[c], node_b.child[c])
```

Whenever one side is null for a given child byte, the other side's subtree
is adopted via a single `IntrusivePtr` copy — O(1), no recursion, no
allocation, no comparisons. Only the shared upper spine of the tree (bytes
common across partitions, e.g. the `user::`, `order::` namespace prefixes
in the benchmark key format — roughly 15 nodes deep) requires traversal.
Below that, every child belongs to exactly one partition and is adopted
wholesale.

The merge rounds are themselves parallel: `merge(T0, T1)` and `merge(T2, T3)`
are independent and run concurrently, giving `O(log2 P)` sequential depth.

The new primitive needed on `PersistentRadixTree<V>`:

```cpp
// Merges two provably-disjoint trees into one. Undefined behaviour if any
// key appears in both. Cost: O(shared-spine-depth x alphabet) for traversal;
// O(1) per disjoint subtree adoption via IntrusivePtr copy.
[[nodiscard]] static auto merge_disjoint(PersistentRadixTree a,
                                         PersistentRadixTree b)
    -> PersistentRadixTree;
```

`max_lsn` is resolved with a `std::max` fold over all partition values before
the final `EngineState` is assembled.

---

## 3. What needs to be built

| Component | Location | Description |
|-----------|----------|-------------|
| `PersistentRadixTree::merge_disjoint` | `radix_tree.cppm` | Core merge primitive |
| Partition orchestration | `bytecask.cppm` | Pre-pass, reader pool, builder pool, merge rounds |
| MPSC queue per partition | new utility | Entry dispatch from readers to builder |
| Hash function | utility | Fast key->partition routing (FNV-1a or xxHash) |
| `BM_RecoveryParallel` | `engine_bench.cpp` | Parallel recovery benchmark |
| `BM_MergeDisjoint` | `map_bench.cpp` | Merge primitive micro-benchmark |

The serial path is kept unchanged as the fallback for `P=1` and
`hardware_concurrency == 1`. No existing public interface changes.

---

## 4. Correctness properties

| Property | How it holds |
|----------|-------------|
| No key in two partitions | `hash(key) % P` is deterministic — same key always routes to same partition |
| Tombstone coverage complete | `PUT` and its superseding `DELETE` always land in the same shard's tombstone map |
| `file_id` values stable | Registry built before any thread launches; read-only thereafter |
| LSN ordering within a partition | Scan order preserves per-file append order; cross-file ordering handled by sequence-number comparison in `apply_put`/`apply_del`, same as today |
| Merge correctness | Disjoint by construction — `merge_disjoint` produces the union of two disjoint key sets; no conflict resolution needed |

---

## 5. Performance estimates

**Config**: P = 16 builder partitions, R = 4 reader threads.
**Machine**: 16 x 4427 MHz, 16 MiB L3 (see `recovery_time_estimates.md`).

### 2 TB / 1.84B keys

| Phase | Serial | Parallel (P=16) |
|-------|--------|-----------------|
| Build | ~11 min | ~47s (16 builders concurrent) |
| Merge (4 rounds, rounds 1-3 parallel) | -- | ~20s |
| Hint I/O, NVMe warm cache | ~40s | ~20s (readers pipelined with builders) |
| **Total, NVMe warm** | **~12 min** | **~67s (~1 min)** |
| **Total, SATA cold** | **~15 min** | **~4-5 min** (I/O-bound: 120 GB / 500 MB/s) |

**Warm-cache speedup: ~11x.** The floor is the final merge round (merging
two ~920M-key trees serially, ~10s).

### Smaller sizes (NVMe warm cache, P=16)

| DB size | Keys | Serial | Parallel |
|---------|------|--------|----------|
| 10 GB   | 9.2M  | ~3.1s   | ~0.5s |
| 100 GB  | 92M   | ~35s    | ~5s   |
| 1 TB    | 922M  | ~6 min  | ~35s  |
| 2 TB    | 1.84B | ~12 min | ~67s  |

---

## 6. Choosing P

P should be a power of two (`hash & (P-1)` is a single AND instruction) and
tuned to available threads at startup:

```cpp
const auto P = std::bit_ceil(std::thread::hardware_concurrency());
```

Expected sweet spots: P = 16 on 16-core machines, P = 8 on 8-core machines.
Increasing P beyond `hardware_concurrency` makes each partition tree smaller
(better L3 fit per builder) but adds merge rounds with diminishing returns.

The implementation should fall back to the serial path when
`num_hint_files < 2` or estimated key count is below ~500k — at small scales
the parallel setup overhead exceeds any gain.

---

## 7. Relation to existing design

The parallel path is a drop-in replacement for `recover_existing_files`. All
`EngineState` invariants are preserved:

- `s.files` populated identically via the serial pre-pass.
- `s.key_dir` is `PersistentRadixTree<KeyDirEntry>` — same type, same
  content, different construction path.
- `s.next_lsn = max_lsn + 1` derived from the `std::max` fold over all
  partition `max_lsn` values.

The transient/persistent edit-tag cycle is still used per partition builder.
`merge_disjoint` operates on frozen (persistent) trees only — no edit-tag
interference between builders and the merge step is possible.

---

## 8. Merge benchmark findings

Run on 16 × 4427 MHz, Clang release. `PersistentRadixTree<int>`, generic `"key_N"` keys, 3 repetitions (mean reported).

### 8.1. Merge-only cost (N total keys, two pre-built N/2-key trees)

| Scenario | 1k (µs) | 10k (µs) | 100k (µs) |
|---|---|---|---|
| Disjoint (0% overlap) | 23 | 261 | 2,779 |
| Overlapping (50% overlap) | 44 | 454 | 6,208 |

Overlapping merges are ~2× more expensive — every shared key forces a node clone + resolver call.

### 8.2. Split-build-merge vs linear build (sequential)

End-to-end: `build(N/2) + build(N/2) + merge(N)` measured sequentially, compared to `TransientSet(N)`.

| Benchmark | 100k (µs) | vs linear |
|---|---|---|
| TransientSet (linear baseline) | 14,583 | 1.0× |
| SplitBuildMerge (disjoint) | 19,820 | 1.36× |
| SplitBuildMergeOverlapping (20%) | 24,744 | 1.70× |
| SplitBuildMergePrefixed (disjoint) | 17,468 | 1.20× |

### 8.3. Parallel projection (2 threads)

With 2 threads the build phases overlap, so wall-clock ≈ `build(N/2) + merge`.

| Scenario | Estimated parallel (µs) | vs linear | Speedup |
|---|---|---|---|
| Disjoint, generic | 8,521 + 2,779 = 11,300 | 14,583 | **1.29×** |
| 20% overlap, generic | ~10,000 + ~4,744 = ~14,744 | 14,583 | ~1.0× (break-even) |
| Disjoint, prefixed | ~7,292 + ~2,885 = ~10,177 | 15,636* | **1.54×** |

\* Prefixed linear baseline = `TransientSetPrefixed/100000`.

### 8.4. Implications for the fan-in merge tree

In a log₂(P) fan-in with P workers:

- **Round 1** (P/2 merges): each merge combines two per-file trees. Keys are mostly disjoint — a key typically appears in only one hint file. Cost per merge ≈ disjoint rate.
- **Round 2+**: merged trees cover increasingly overlapping key ranges. In an update-heavy database, later rounds approach the 20% overlap case.
- **Final round**: single serial merge of two ~N/2 trees. Overlap depends on workload.

The build phase dominates total cost. Merge rounds 1–3 are parallel and cheap (disjoint subtree adoption). Only the final round is serial and potentially overlapping.

---

## 9. Strategy decision: hash-partition vs file-level split

The original design (§2) proposes `hash(key) % P` partitioning to guarantee **fully disjoint** trees, enabling `merge_disjoint` with zero conflict resolution.

An alternative is **file-level partitioning**: assign each hint file to a worker round-robin, each builds a full-key-range tree, and merge with a general `merge(a, b, resolve)` using LSN-based conflict resolution.

### 9.1. Hash-partition (original design)

| Pro | Con |
|---|---|
| Guaranteed disjoint — merge is O(shared spine) | Requires MPSC queues, hash dispatch |
| No resolver needed — simpler merge primitive | Reader → builder pipeline adds latency |
| Merge 2× faster at high overlap | Partition imbalance if key distribution skewed |
| `count_keys` can sum partition sizes instead of walking | More infrastructure to build |

### 9.2. File-level partition

| Pro | Con |
|---|---|
| Trivial to implement — each worker reads its files and builds | Later merge rounds face ~20% overlap |
| No queues, no hashing, no dispatch | Resolver needed (LSN comparison) |
| Work-stealing straightforward | Merge ~2× slower in overlapping rounds |
| Already have `merge(a, b, resolve)` implemented and tested | `count_keys` O(N) walk after each merge |

### 9.3. Benchmark-informed recommendation

The merge benchmarks show:

1. **Disjoint merge is 2× cheaper** than 50%-overlapping merge, but even the overlapping merge is only 34% of linear build cost at 100k keys. The merge phase is not the bottleneck — build is.
2. **At 2 threads, disjoint split+merge gives 29% speedup**, overlapping breaks even. At 4+ threads the build phase dominates enough that both strategies win.
3. **Prefixed keys** (realistic workload) show only +20% sequential overhead for split+merge, making parallel a clear win even at 2 threads.

Given that:
- `merge(a, b, resolve)` is **already implemented and tested** (9 test cases, 25k assertions, model-based oracle)
- File-level partitioning requires **zero new infrastructure** (no queues, no hash dispatch)
- The performance gap between disjoint and general merge is **dominated by build cost** and shrinks with more workers
- Real workloads are closer to the disjoint case in early rounds (most keys appear in one file)

**Recommendation: start with file-level partitioning** using the existing general `merge(a, b, resolve)` with an LSN resolver. If profiling of the merge phase at >1B keys shows it becoming a bottleneck, upgrade to hash-partitioned disjoint merge.

---

## 10. Implementation order

Tracked as **BC-068: Parallel recovery**.

1. ~~`PersistentRadixTree::merge(a, b, resolve)` + unit tests~~ — **Done** (BC-068a)
2. ~~Merge benchmarks in `map_bench.cpp`~~ — **Done** (disjoint, overlapping, split-build-merge)
3. ~~Parallel `recover_existing_files` with file-level partitioning behind `recovery_threads` param (default 1 = serial)~~ — **Done** (BC-068)
4. `BM_RecoveryParallel` in `engine_bench.cpp`
5. Make parallel the default when `hardware_concurrency >= 4`
6. **(Optional)** If merge proves bottleneck at >1B keys: hash-partition variant with `merge_disjoint`

---

## 11. Parallel Recovery v1 — file-level partitioning with fan-in merge

This section describes the first parallel recovery algorithm. It uses
file-level partitioning with the existing general `merge(a, b, resolve)` and
a parallel fan-in reduction. The serial `recover_existing_files` serves as
the baseline; this v1 algorithm is the first parallel alternative.

### 11.1. Data structures

```cpp
struct RecoveryResult {
  PersistentRadixTree<KeyDirEntry> key_dir;
  std::map<Key, std::uint64_t> tombstones;  // key → highest DELETE seq
  std::uint64_t max_lsn{0};
};
```

Each worker produces a `RecoveryResult`. The fan-in merge consumes pairs of
`RecoveryResult`s and produces a single merged `RecoveryResult` at each step.

### 11.2. Algorithm

#### Phase 1 — serial pre-pass (unchanged)

Runs exactly as the current serial path:

1. Remove stale `.hint.tmp` files.
2. Directory scan: collect all `.data` paths.
3. For each data file: assign `file_id`, construct `DataFile`, seal, register
   in `s.files`.
4. Generate missing hint files via `flush_hints_for()` (batch-aware).
5. Build the `RecoveredFile` vector: `{file_id, data_file, hint_path}`.

The file registry is read-only from this point. All subsequent phases
reference it without synchronisation.

#### Phase 2 — parallel build (W workers)

Assign files to W workers via round-robin on the sorted `RecoveredFile` list:

```
worker[i] ← files where (file_index % W) == i
```

Each worker runs independently, building its own `RecoveryResult`:

```
for each assigned (file_id, hint_path):
    scan hint_path with HintFile::scan_view()
    for each hint entry:
        if Put:  apply_put(seq, file_id, file_off, val_size, key)
        if Del:  apply_del(seq, key)
    track max_lsn

persist the transient tree → RecoveryResult{tree, tombstones, max_lsn}
```

The `apply_put` / `apply_del` logic is identical to the serial path — check
tombstone map, compare sequence numbers, set/erase in transient tree.

W defaults to `std::thread::hardware_concurrency()`, clamped to the file
count. When W = 1, this degenerates to the serial path.

#### Phase 3 — parallel fan-in merge (log₂(W) rounds)

Merge the W `RecoveryResult`s pairwise in parallel rounds:

```
Round 1:  merge(R0,R1)  merge(R2,R3)  merge(R4,R5)  ...   → W/2 results
Round 2:  merge(R01,R23)  merge(R45,R67)  ...              → W/4 results
  ...
Round k:  merge(left, right)                               → 1 result
```

Each pair merge within a round is independent and runs on its own thread.
An odd result at the end of any round carries forward unpaired.

Total rounds: ⌈log₂(W)⌉. Threads per round: ⌊count/2⌋.
Threading uses `std::jthread` — join on scope exit.

#### Phase 4 — serial assembly

```cpp
s.key_dir   = final_result.key_dir;
s.next_lsn  = final_result.max_lsn + 1;
```

Tombstones are no longer needed after assembly — they are intermediate
bookkeeping only.

### 11.3. Tombstone cross-application in merge

This is the central correctness concern that distinguishes file-level
partitioning from hash-partitioning. With hash-partitioning every PUT and
its superseding DELETE land in the same partition; with file-level
partitioning they may be in different workers.

**The problem.** `merge(A, B, resolve)` fires the resolver only when the
same key exists in both trees. A DELETE removes the key from the tree, so
a stale PUT in the other tree has no counterpart to conflict with — the
merge adopts it unconditionally.

Example:
- Worker A processes file 1: `PUT "x" seq=5` → tree has `"x"`.
- Worker B processes file 2: `DEL "x" seq=8` → tree has no `"x"`,
  tombstones has `{"x": 8}`.
- `merge(A.tree, B.tree, resolve)` sees `"x"` only in A → adopts it.
  The stale PUT survives incorrectly.

**The fix.** After tree merge, iterate both tombstone maps and erase entries
whose sequence number is lower than the tombstone:

```
merge_results(A, B) → RecoveryResult:
    1. merged_tree = merge(A.key_dir, B.key_dir, lsn_resolver)

    2. for (key, tomb_seq) in B.tombstones:
           if entry = merged_tree.get(key):
               if entry.sequence < tomb_seq:
                   merged_tree = merged_tree.erase(key)

    3. for (key, tomb_seq) in A.tombstones:
           if entry = merged_tree.get(key):
               if entry.sequence < tomb_seq:
                   merged_tree = merged_tree.erase(key)

    4. merged_tombstones = union(A.tombstones, B.tombstones)
       // per-key: max(A.seq, B.seq)

    5. max_lsn = max(A.max_lsn, B.max_lsn)

    return {merged_tree, merged_tombstones, max_lsn}
```

Step 4 is essential: merged tombstones must propagate through subsequent
fan-in rounds. A tombstone from round 1 may need to suppress a PUT that
only enters the merge tree in round 3.

**Correctness argument.** Every DELETE in the database is recorded in
exactly one worker's tombstone map. The fan-in produces a running union of
all tombstone maps seen so far. At round k, the merged tombstone map
contains every DELETE from all 2^k input workers. The cross-application
step (2–3) erases any PUT with a lower sequence number. After the final
round, every DELETE has had a chance to suppress every PUT — identical
result to the serial path.

### 11.4. LSN resolver

The resolver passed to `merge()` picks the entry with the higher sequence
number (most recent write wins):

```cpp
auto lsn_resolver = [](const KeyDirEntry& a, const KeyDirEntry& b) {
    return (a.sequence >= b.sequence) ? a : b;
};
```

This handles the case where both workers have a PUT for the same key
(update-heavy workload, key written in multiple files).

### 11.5. Thread count selection

```cpp
auto W = std::min(
    static_cast<unsigned>(files.size()),
    std::thread::hardware_concurrency());
if (W == 0) W = 1;  // fallback
```

When `W == 1`, skip Phase 2/3 entirely and run the existing serial path.
This is the default (no regression risk) until parallel is validated.

A future `recovery_threads` parameter on `Bytecask::open()` will allow
explicit control. Default: 0 = auto (use hardware_concurrency).

### 11.6. Comparison with serial baseline

| Aspect | Serial (current) | Parallel v1 |
|--------|-------------------|-------------|
| Build | 1 thread, 1 transient tree | W threads, W transient trees |
| Merge | — | ⌈log₂(W)⌉ rounds, pairwise parallel |
| Tombstone handling | Single map, immediate | Per-worker map + cross-application at merge |
| Infrastructure | — | `RecoveryResult`, `merge_results()`, fan-in loop |
| Key conflicts | N/A (one tree) | Resolved by LSN in `merge()` |
| Tombstone conflicts | N/A (one map) | Cross-applied after each merge step |
| Correctness | Trivial (serial) | Proven equivalent via tombstone propagation |

### 11.7. Future improvements (not in v1)

- **Hash-partitioning (v2):** If merge-phase profiling at >1B keys shows
  it dominating, switch to `hash(key) % P` routing with `merge_disjoint`
  (zero conflict, zero tombstone cross-application). Requires MPSC queues.
- **Work stealing:** If file sizes are uneven, round-robin may leave some
  workers idle. A work-stealing queue could balance the load.
- **Streaming merge:** Overlap Phase 2 and Phase 3 by starting round 1
  merges as soon as the first two workers finish, rather than waiting for
  all W workers.
