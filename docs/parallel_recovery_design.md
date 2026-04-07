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

The serial `recovery_load_serial` loop is fundamentally single-threaded: one
`TransientRadixTree` is updated one entry at a time across all hint files.
`recovery_load_parallel` replaces that loop with **file-level parallel builds**
followed by an **LSN-based sequential accumulator merge**.

See `docs/recovery_time_estimates.md` for the full baseline measurements.

---

## 2. Design

### 2.1. Key insight: partition by file, merge with LSN resolution

Assign subsets of hint files (round-robin) to worker threads. Each worker
independently builds a `RecoveryResult` from its files. Because a key can
appear in multiple files (older value in file A, newer value in file B),
workers' trees may overlap. Correctness is restored in the merge step via
LSN-based conflict resolution and cross-tombstone application.

This gives two properties:

1. **Zero synchronisation** during the build phase — workers touch disjoint
   file sets and write into independent data structures.
2. **Correct tombstone coverage** — a `Delete "foo"` in worker A's files
   suppresses a stale `Put "foo"` in worker B's files via the cross-tombstone
   pass in `recovery_merge_results`.

### 2.2. Architecture overview

```
    +-------------------------------------------------------------------+
    |  recovery_prepare_files (serial)                                  |
    |  Directory scan, remove .tmp, open+seal DataFiles, assign         |
    |  file_ids, generate any missing hint files.                       |
    +-------------------------------------------------------------------+
                                    |
                                    v  F RecoveredFile objects
    +------------------------+  +------------------------+  +----------+
    | Worker 0               |  | Worker 1               |  | ...      |
    | recovery_build_from_   |  | recovery_build_from_   |  |          |
    |   hints(files[0,W,2W…])|  |   hints(files[1,W+1…]) |  |          |
    | → RecoveryResult       |  | → RecoveryResult       |  |          |
    +------------------------+  +------------------------+  +----------+
              |                          |
              |      (via queue + cv)    |
              v                          v
    +-------------------------------------------------------------------+
    |  Main thread: sequential accumulator merge                        |
    |  acc = merge_results(acc, next_from_queue)  [as workers finish]   |
    +-------------------------------------------------------------------+
                                    |
                                    v
    +-------------------------------------------------------------------+
    |  Phase 4: single-pass live_bytes recomputation (iterator walk)    |
    +-------------------------------------------------------------------+
                                    |
                                    v
    +-------------------------------------------------------------------+
    |  Phase 5: EngineState assembly                                    |
    +-------------------------------------------------------------------+
```

### 2.3. Serial pre-pass — `recovery_prepare_files`

Before any worker thread launches, `recovery_prepare_files(EngineState &s)`:

1. Removes all `*.hint.tmp` and `*.data.tmp` files (incomplete writes from a
   prior crash).
2. Iterates `dir_` for `*.data` files; for each: assigns `file_id`,
   constructs and seals a `DataFile`, registers it in `s.files`.
3. Calls `flush_hints_for` for any data file lacking a companion `.hint` —
   this scans the raw data file and writes a complete hint file atomically via
   the temp-then-rename protocol.
4. Returns `vector<RecoveredFile>` (file_id, DataFile*, hint_path, total_bytes).

The resulting file registry is read-only from this point forward.
All workers reference it without synchronisation.

### 2.4. Worker threads — `recovery_build_from_hints`

W workers (`W = min(files.count, recovery_threads)`) are assigned files via
round-robin: worker `i` gets `files[i], files[i+W], files[i+2W], …`

Each worker calls `recovery_build_from_hints(span<RecoveredFile>)` which:

1. Initialises `file_stats[file_id].total_bytes = rf.total_bytes` for each
   assigned file (live_bytes is left at 0 — deferred to Phase 4).
2. Opens each hint file via `HintFile::OpenForRead` and iterates entries
   with `scanner.next()`.
3. For each `Put` entry: checks tombstone map; if no superseding tombstone,
   calls `t.upsert(key, entry, lsn_wins)` where `lsn_wins` keeps the entry
   with the higher sequence number.
4. For each `Delete` entry: records `tombstones[key] = max(tombs[key], seq)`;
   erases any tree entry with a lower sequence.
5. Returns `RecoveryResult{frozen_tree, tombstones, max_lsn, file_stats}`.

Workers push their result into a shared queue (under `queue_mu`) and notify
the main thread via a condition variable.

### 2.5. Sequential accumulator merge — `recovery_merge_results`

The main thread pulls `RecoveryResult` objects from the queue as workers
finish and merges each into an accumulator:

```cpp
acc = recovery_merge_results(std::move(acc), std::move(incoming));
```

`recovery_merge_results(a, b)`:

1. **File stats**: union `a.file_stats` and `b.file_stats` (disjoint by
   construction — each file is owned by exactly one worker).
2. **Tree merge**: `merged = PersistentRadixTree::merge(a.key_dir, b.key_dir,
   lsn_resolver)` where `lsn_resolver(x, y)` returns the entry with the
   higher sequence number.
3. **Cross-tombstone pass A→merged**: for each `(key, tomb_seq)` in
   `b.tombstones`, if `merged` contains that key with a lower sequence,
   erase it from `merged`. Repeat symmetrically for `a.tombstones`.
4. **Tombstone union**: for each key, `merged_tombs[key] = max(a_seq, b_seq)`.
5. Returns `RecoveryResult{merged, merged_tombs, max(a.max_lsn, b.max_lsn),
   merged_stats}`.

### 2.6. Live-bytes recomputation — Phase 4

`recovery_build_from_hints` defers live_bytes tracking to avoid O(N × log₂ W)
redundant map lookups inside each worker (a key may be overwritten by a later
file in the same worker's set). After the final merge, one iterator pass
over the fully-merged tree reconstructs live_bytes correctly:

```cpp
for (auto it = final.key_dir.begin(); it != sentinel; ++it) {
    const auto &[key_span, kde] = *it;
    final.file_stats[kde.file_id].live_bytes +=
        entry_size(key_span.size(), kde.value_size);
}
```

### 2.7. EngineState assembly — Phase 5

```cpp
s.key_dir   = std::move(final.key_dir);
s.next_lsn  = final.max_lsn + 1;
file_stats_ = std::move(final.file_stats);
```

---

## 3. Key types

| Type | Location | Description |
|------|----------|-------------|
| `RecoveredFile` | `internals.cppm` | One data file seen during pre-pass: `file_id`, `DataFile*`, `hint_path`, `total_bytes` |
| `RecoveryResult` | `internals.cppm` | Output of one worker: `key_dir`, `tombstones`, `max_lsn`, `file_stats` |
| `recovery_prepare_files` | `bytecask.cpp` | Shared pre-pass for both serial and parallel paths |
| `recovery_build_from_hints` | `bytecask.cpp` | Builds a `RecoveryResult` from a subset of hint files (no shared state) |
| `recovery_merge_results` | `bytecask.cpp` | LSN-based merge of two `RecoveryResult`s |
| `recovery_load_serial` | `bytecask.cpp` | Single-thread path: all files, one transient tree |
| `recovery_load_parallel` | `bytecask.cpp` | Multi-thread path: W workers + accumulator merge |

---

## 4. Correctness properties

| Property | How it holds |
|----------|-------------|
| No lost writes | Every hint entry is processed by exactly one worker; all workers' results are merged into the accumulator before assembly |
| Stale Put suppressed by later Delete | Cross-tombstone pass in `recovery_merge_results` erases any merged-tree entry that is older than a tombstone from the other worker's files |
| Latest value wins across files | `PersistentRadixTree::merge` uses `lsn_resolver` which returns the entry with the higher sequence number |
| `file_id` values stable during build | Registry built in `recovery_prepare_files` before any worker launches; read-only thereafter |
| `file_stats.total_bytes` accurate | Set from `std::filesystem::file_size` in `recovery_prepare_files`; never modified during build/merge |
| `file_stats.live_bytes` accurate | Recomputed in a single iterator pass after the final merge, from the authoritative merged tree |
| Serial and parallel paths produce identical results | Model-based recovery tests (`[model]` in `bytecask_test.cpp`) verify key/value content and per-file `file_stats` match a serial baseline under 1, 2, 4, and 8 threads |

---

## 5. Serial path differences

`recovery_load_serial` uses a single `TransientRadixTree` and tracks
`live_bytes` inline (updating per-file stats as each entry is processed).
This avoids the Phase 4 recomputation pass but is fundamentally single-threaded.

The two paths are selected at open time via `Options::recovery_threads`:

```cpp
if (opts.recovery_threads <= 1)
    s = recovery_load_serial(std::move(s));
else
    s = recovery_load_parallel(std::move(s), opts.recovery_threads);
```

---

## 6. Merge benchmark findings

Run on 16 × 4427 MHz, Clang release. `PersistentRadixTree<int>`, generic `"key_N"` keys, 3 repetitions (mean reported).

### 6.1. Merge-only cost (N total keys, two pre-built N/2-key trees)

| Scenario | 1k (µs) | 10k (µs) | 100k (µs) |
|---|---|---|---|
| Disjoint (0% overlap) | 23 | 261 | 2,779 |
| Overlapping (50% overlap) | 44 | 454 | 6,208 |

Overlapping merges are ~2× more expensive — every shared key forces a node clone + resolver call.

### 6.2. Split-build-merge vs linear build (sequential)

End-to-end: `build(N/2) + build(N/2) + merge(N)` measured sequentially, compared to `TransientSet(N)`.

| Benchmark | 100k (µs) | vs linear |
|---|---|---|
| TransientSet (linear baseline) | 14,583 | 1.0× |
| SplitBuildMerge (disjoint) | 19,820 | 1.36× |
| SplitBuildMergeOverlapping (20%) | 24,744 | 1.70× |
| SplitBuildMergePrefixed (disjoint) | 17,468 | 1.20× |

### 6.3. Parallel projection (2 threads)

With 2 threads the build phases overlap, so wall-clock ≈ `build(N/2) + merge`.

| Scenario | Estimated parallel (µs) | vs linear | Speedup |
|---|---|---|---|
| Disjoint, generic | 8,521 + 2,779 = 11,300 | 14,583 | **1.29×** |
| 20% overlap, generic | ~10,000 + ~4,744 = ~14,744 | 14,583 | ~1.0× (break-even) |
| Disjoint, prefixed | ~7,292 + ~2,885 = ~10,177 | 15,636* | **1.54×** |

\* Prefixed linear baseline = `TransientSetPrefixed/100000`.
