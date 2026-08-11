# ByteCaskDB Replication Primitives Design

> Implemented in BC-197. Proof test suite (211 tests) in BC-204.
> `current_sequence(timeout)` renamed to the target-based `durable_sequence(min_sequence, timeout)` in BC-231 — see [`commit_result_api_design.md`](commit_result_api_design.md).

## Purpose

This document describes the minimal set of API primitives that ByteCaskDB exposes so that leader-follower replication is built **on top of** the engine, without embedding any distributed systems logic inside it. ByteCaskDB remains a passive, embedded key-value store — an external coordinator handles topology, failure detection, and promotion.

Canonical location: `docs/replication_primitives_design.md`.

---

## Terminology

ByteCaskDB uses the term **sequence** for the globally monotonic `u64` counter assigned to every data entry. This is the same concept that other databases call a **log sequence number (LSN)**. We use `sequence` throughout the codebase and on-disk format; this document follows that convention.

---

## Design Principles

1. **ByteCaskDB is passive.** It provides primitives; coordination lives outside.
2. **No user-registered callbacks, no server-pushed events.** Long-polling unifies push and pull into one API. The internal condvar is an implementation detail — the external contract is a blocking call that returns when data is available.
3. **Sequences already exist.** Every data entry carries a globally monotonic `u64 sequence`. The primitives surface what the engine already tracks.
4. **The data file is the replication log.** No separate WAL or changelog — the append-only data files contain every put, delete, range delete, and batch marker with its sequence.

---

## Primitives

### 1. `durable_sequence(min_sequence, timeout)`

The single sequence primitive — target-based, not advance-based (BC-231; renamed from `current_sequence`). Blocks until the durable sequence (the highest sequence confirmed by `fdatasync`) reaches at least `min_sequence`, then returns it. `min_sequence = 0`, an already-reached target, or a nonpositive `timeout` all return immediately without blocking — this is the poll mode. Otherwise it blocks until a `fdatasync` advances the durable sequence to `min_sequence` or the timeout expires — this is the long-poll mode.

Internally, the write path calls `cv.notify_all()` only when `durable_sequence` advances — that is, after a successful `fdatasync` (sync write, rotation sync, or resume). NoSync-only write batches publish state via `state_.store()` but do not notify — the replication loop stays asleep until durability is confirmed. The long-poll blocks on that condition variable — nanosecond overhead on the write path.

```cpp
auto durable_sequence(uint64_t min_sequence = 0,
                      std::chrono::milliseconds timeout = 0ms) const -> uint64_t;
```

**Use cases (one primitive, three targets):**
- Poll / lag monitoring: `min_sequence = 0` — always reached, returns immediately.
- Replication loop wake-up: `min_sequence = follower.durable_sequence() + 1` — "the leader has something the follower lacks".
- Read-your-own-writes: `min_sequence = r.sequence` from a `CommitResult` returned by a write — see [`commit_result_api_design.md`](commit_result_api_design.md).

**Why target-based fixes the idle-leader wart:** the previous advance-based `current_sequence(timeout)` blocked until the sequence moved *past its value at call entry* — a proxy for "the leader has new data," but wrong when the leader is already ahead of the follower (e.g. right after bootstrap, or after a follower restart). On an idle leader, that first call would block for the full timeout even though the leader already had entries the follower lacked. The target-based form has no such wart: `leader.durable_sequence(follower.durable_sequence() + 1, timeout)` returns immediately whenever the leader is already ahead, and only blocks when there is genuinely nothing new to replicate.

### 2. `create_manifest()` — sealed file manifest

Rotates the active file, waits for hint generation to complete, and returns a self-contained manifest of all sealed data and hint files along with a snapshot. The manifest covers every committed entry — no active file gap.

```cpp
struct FileManifest {
    Snapshot snap;                      // frozen key directory
    std::vector<FileInfo> files;        // sealed data + hint files
    uint64_t through_sequence;          // last sequence covered
};

auto create_manifest() -> FileManifest;
```

**Lifecycle:**

1. **Rotate** — the current active file is sealed and a new active file opens. Writes are not blocked (rotation is fast).
2. **Wait for hint generation** — hint files are generated outside the write lock, so no write stall. The function blocks until all hint files are ready.
3. **Return** — the caller receives everything needed to ship a consistent copy.

**Vacuum coordination:** the caller must not invoke `vacuum()` between `create_manifest()` and the completion of the file transfer/copy. Vacuum unlinks files from the filesystem; once unlinked, any external tool reading by path (`rsync`, `scp`, `cp`, a custom transfer loop) will see `ENOENT`. Since vacuum is caller-triggered, this is a simple serialization constraint — the coordinator driving the backup or bootstrap is the same code that decides when vacuum runs.

**Use cases:**
- **Bootstrap** — ship the manifest's files to a new follower, open them, run recovery, then start tailing from `follower.durable_sequence()`.
- **Backup** — copy the manifest's files to durable storage for point-in-time restore.
- **Federation** — publish a consistent snapshot of the database to an external system.

**Cost:** forces a file rotation even if the active file is nearly empty. For bootstrap and backup this is acceptable (infrequent operations). Callers doing periodic backups should decide whether the rotation overhead fits their use case.

### 3. `file_stats` with `min_sequence` / `max_sequence`

Each file's stats track the minimum and maximum sequence of entries it contains. This allows `changes_since` to skip files that have no entries newer than the target sequence.

**Per-file sequence monotonicity invariant:** every data file (active or sealed) contains entries in strictly ascending sequence order. This is naturally true for the active file (sequences are assigned monotonically) and for sealed files produced by rotation. `vacuum_compact_file` preserves it — it scans the source file sequentially, skipping dead entries, so surviving entries retain their original order. `vacuum_remove_file` deletes the file entirely — no ordering concern. `vacuum_absorb_file` is not used — it would interleave old sequences into the active file, violating this invariant.

This invariant makes `changes_since` efficient: each file is a sorted run, so the min-heap merge is optimal. It also enables **binary search within a file** using hint files (each hint entry carries the sequence), allowing `changes_since` to seek directly to the first entry with `sequence > from_sequence` rather than scanning from byte 0.

### 4. `changes_since(snap, from_sequence)` — iterator

Returns an iterator that yields raw entries (sequence, entry_type, key, value) for all committed, durable entries with `sequence > from_sequence`, **in ascending sequence order**. The upper bound is `min(snap.sequence(), durable_sequence)` — entries visible in the snapshot but not yet `fdatasync`'d are excluded (see [Durable Sequence](#durable-sequence)).

Implementation:

1. Filter files where `max_sequence > from_sequence` (using file_stats).
2. Sort qualifying files by `min_sequence` in ascending order. The sequence-disjoint files invariant guarantees that files have non-overlapping sequence ranges.
3. **Linear file scan:** iterate through files in sequence order. Within each file, skip entries with `sequence <= from_sequence`. **No merge needed** — files are naturally ordered by sequence range.
4. **Lazy value fetch:** the actual value bytes are read from the data file via `pread` only when the caller consumes the entry. Deletes and range deletes require no data file read at all.
5. **Committed entry scanning:** each per-file cursor uses `CommittedEntryIterator`, the same iterator used by `flush_hints_for` and `vacuum_scan_and_copy`. Entries between `BulkBegin` and `BulkEnd` are buffered internally; the entries (with markers) are only yielded when `BulkEnd` is seen. If the file ends without `BulkEnd`, the buffered entries are discarded — the batch was incomplete (crash during `apply_batch`). All data file scanners in the engine handle incomplete batches identically.

The iterator holds a reference to the `Snapshot`, which keeps file descriptors open for the duration of the scan. Vacuum may unlink files from the filesystem mid-iteration, but POSIX guarantees `pread` succeeds on unlinked files as long as the fd is open. Unlike `create_manifest()` — which ships files by path and requires the caller to serialize with vacuum — `changes_since` is safe to run concurrently with vacuum because it never accesses files by path.

```cpp
auto changes_since(const Snapshot& snap, uint64_t from_sequence) const -> ChangeIterator;
```

When the iterator is exhausted, all committed durable entries up to `min(snap.sequence(), durable_sequence)` have been delivered in order.

Because entries are sequence-ordered, every prefix of the stream is a valid state. The follower can `ingest()` at any point — not just at iterator exhaustion — and the result is always a consistent, sequence-ordered prefix of the leader's history.

**Failure handling:** on failure mid-iteration, restart Phase 2 with a fresh snapshot and iterator from `follower.durable_sequence()`. Since entries are in sequence order, the follower's recovered state after a crash is a valid prefix — `durable_sequence()` is trustworthy.

### 5. `ingest(entries)` and `durable_sequence()` on the follower

**`ingest(entries)`** applies raw entries to the key directory and publishes the updated state to readers immediately. Because `changes_since` delivers entries in ascending sequence order, every `ingest` call produces a valid, consistent prefix of the leader's history — no separate commit step is needed.

**`durable_sequence()`** returns the last ingested sequence — identical semantics on both leader and follower. On the leader it reflects the latest write; on the follower it reflects the last `ingest`. External code does not need to know which mode it's talking to. The orchestrator compares `leader.durable_sequence()` and `follower.durable_sequence()` to measure replication lag.

```cpp
void ingest(std::span<const DataEntryView> entries);  // applies and publishes
auto durable_sequence() const -> uint64_t;        // last committed sequence
```

After each `ingest`, `next_sequence` reflects `max(next_sequence, max(ingested sequences) + 1)` — it never decreases, preserving the runtime invariant enforced by `store_state`.

**Idempotency:** entries with `sequence <= durable_sequence()` are silently skipped. This makes `ingest` safe for restart-on-failure semantics — the orchestrator restarting from `follower.durable_sequence()` may occasionally re-deliver the boundary entry, and the follower handles it correctly.

**Constraints:**
- `ingest` is only callable in `Mode::Follower`.
- Appends to data files and updates the key directory, same as the normal write path, but skips guards and sequence assignment.
- `RangeDel` entries walk the follower's key directory over `[from, to)`, same as the normal write path. If the follower never received the keys in that range (e.g. it started tailing after they were written), the walk is a no-op — the range is simply empty on the follower.

### 6. `Mode::Follower`

An engine mode that blocks `put`, `del`, `apply_batch` and allows only `ingest` plus all read operations. The mode can be changed online — no restart required.

`vacuum()` is allowed in Follower mode — followers accumulate fragmentation from `ingest` writes and need to reclaim space. Vacuum does not assign new sequences; it preserves original sequences verbatim. `resume()` is also allowed if the follower enters a degraded state.

```cpp
enum class Mode { Leader, Follower };

void set_mode(Mode mode);
auto mode() const -> Mode;
```

**Promotion:** switch from `Follower` to `Leader`. The engine's `next_sequence` is already correct (advanced by `ingest`), so writes pick up where the old leader left off.

---

## Durable Sequence

A NoSync write (`WriteOptions.sync = false`) publishes state via `state_.store()` without calling `fdatasync`. The entry is visible to local readers immediately, but it is only in the OS page cache — not durable. If the leader crashes before the next `fdatasync`, the entry is lost.

Replicating unsync'd entries would create a divergence: the follower has data the leader would lose on crash. The follower's state would not be a valid prefix of the leader's *durable* history.

**The rule**: `changes_since` yields only entries whose durability has been confirmed by `fdatasync`. The replication boundary is the durable sequence, not the visible sequence.

### Tracking

The engine maintains a `durable_sequence` alongside `next_seq`:

- **`next_seq`** — highest allocated sequence. Includes unsync'd entries. Used for sequence assignment.
- **`durable_sequence`** — highest sequence confirmed by `fdatasync`. Only increases. Each sync point sets `durable_sequence = max(durable_sequence, highest_sequence_in_synced_range)`. This is trivially satisfied because sequences are assigned monotonically and each `fdatasync` covers all entries written to the file so far.

**Sync points that advance `durable_sequence`** (these are the only points at which `durable_sequence` can move):

1. **Sync write** — `file.sync()` when any writer in the group commit batch requested `sync=true`. Covers all entries in the batch, including those from NoSync writers that rode along.
2. **Rotation sync** — `file.sync()` before sealing the active file. Covers all entries written to the file, including NoSync entries. This is the mechanism that bounds replication lag on NoSync-only workloads.
3. **Resume sync** — after `resume()` truncates garbage bytes and syncs the recovered file.

The error-path `file.sync()` after a failed `append_entries` does **not** advance `durable_sequence`. That sync is a best-effort flush of bytes previously in the page cache — but `batch_max_seq` includes sequences from the failed append, which may be corrupt. Advancing `durable_sequence` to those sequences would claim durability for garbage. The engine degrades after this path; `resume()` sets `durable_sequence` correctly from the recovered state.

Any future code that adds a new `file.sync()` call should also advance `durable_sequence`, or replication will stall / skip entries depending on direction.

`changes_since(snap, from_sequence)` uses `min(snap.sequence(), durable_sequence)` as its upper bound. Entries with `sequence > durable_sequence` are excluded even if present in the snapshot's key directory.

`durable_sequence(min_sequence, timeout)` exposes this field to callers. The long-poll condition variable fires when `durable_sequence` advances. This ensures the replication loop only wakes up when there are durable entries to replicate.

### Group commit interaction

When a batch contains mixed sync/nosync writers, `fdatasync` covers the entire batch. `durable_sequence` advances past all entries in the batch, including those from nosync writers that rode along. This is the existing group commit behavior — no change required.

### NoSync-only workloads

File rotation calls `fdatasync` before sealing the active file (required for crash safety of the sealed file). This means `durable_sequence` advances at least once per `max_file_bytes` of writes, even on a pure NoSync workload. Replication lag on a NoSync-only leader is bounded by the rotation threshold — not unbounded.

Between rotations, NoSync entries are in the page cache and not replicable. Once rotation syncs the file, all entries in it become durable and `durable_sequence` catches up to the highest sequence in the sealed file. An explicit sync write or `fdatasync` call has the same effect at any point.

---

## Vacuum: Preserving Batch Markers

**Vacuum strategy for replication:** only two vacuum paths exist: `vacuum_compact_file` (sealed→sealed, for files with live entries) and `vacuum_remove_file` (delete, for files with zero live entries). The `vacuum_absorb_file` path (sealed→active) is deprecated because it breaks per-file sequence monotonicity — absorbed entries would interleave old sequences into the active file, defeating file-level filtering and complicating the `changes_since` read fence (the active file's `durable_seq` boundary).

`vacuum_scan_and_copy` in `bytecaskdb/bytecask.cpp` rewrites live entries from a sealed source file into a new sealed destination file. The `emit_entry` lambda writes `BulkBegin` before the first entry in a batch and `BulkEnd` after the last. The outer scan loop tracks batch membership (`in_batch`, `pending` vector) and only calls `emit_entry` for batch entries after seeing `BulkEnd` — so the batch is known-committed at emit time. The markers are emitted alongside the entries:

1. Before iterating `pending`, write `BulkBegin` to the destination file with its original sequence.
2. Emit each pending entry.
3. After iterating `pending`, write `BulkEnd` to the destination file with its original sequence.

Original sequences are preserved verbatim — vacuum never assigns new sequences. The destination file contains the same `BulkBegin`...entries...`BulkEnd` framing as the source, with dead entries removed.

### Why no backward compatibility concern

This changes the on-disk layout of vacuumed files: they now contain `BulkBegin`/`BulkEnd` markers that were previously absent. However:

- **Hint files include batch markers.** Hint files write `BulkBegin`/`BulkEnd` markers (with their sequence numbers) so that recovery correctly computes `next_seq` and `durable_seq`. Recovery reads hint files, not data files, so the markers must be present for accurate sequence tracking.
- **The data file format already supports these markers.** `BulkBegin` and `BulkEnd` are existing `EntryType` values (0x03, 0x04). Any code that scans data files already handles them.
- **Existing vacuumed files without markers are still valid.** `changes_since` over a file without markers yields standalone entries — the same behavior as today. The change only affects files vacuumed after the fix.

### Impact on bootstrap

`create_manifest` ships sealed files to new followers. If those files were vacuumed before this fix, they contain no batch markers. The follower recovers from hint files (which include markers), so bootstrap is unaffected — recovery tracks sequences from hint entries regardless of whether the data file contains markers. Once the fix is in place, newly vacuumed files carry markers through, and `changes_since` over those files produces correctly framed batches.

---

## Replication Flow

### Phase 1 — Bootstrap (new follower only)

```
1. Follower calls leader: "give me a manifest"
2. Leader calls create_manifest() → rotates active file, waits for hints, returns FileManifest
3. Leader ships sealed data files + hint files to follower
   (vacuum must not run during transfer — see vacuum coordination above)
4. Follower opens ByteCaskDB with Mode::Follower over received files
   (DB::open() creates a new active file as usual — needed for ingest and eventual promotion)
5. Recovery rebuilds key directory from hint files
   (follower.durable_sequence() == manifest.through_sequence after recovery)
6. Proceed to Phase 2
```

### Phase 2 — Replicate (loop forever, restart on any failure)

Because `changes_since` delivers entries in sequence order, every prefix of the stream is a valid state. After a crash, `follower.durable_sequence()` reflects a consistent prefix — it is trustworthy and the orchestrator does not need to persist its own `from_seq`.

```
loop:
    leader.durable_sequence(follower.durable_sequence() + 1, timeout=30s)
        — target-based wake-up: returns immediately if the leader is
          already ahead of the follower; blocks only when there is
          genuinely nothing new to replicate
    snap = leader.snapshot()               — captures sequence boundary
    it = leader.changes_since(snap, follower.durable_sequence())
    for entry in it:
        follower.ingest(entry)
    // on failure at any point: loop restarts from follower.durable_sequence()
```

The snapshot determines the upper bound of the stream — `changes_since` yields entries up to `snap.sequence()`. No separate call is needed to define that upper bound; the snapshot is the target. The leading `durable_sequence(min_sequence, timeout)` call serves only as a wake-up: unlike the old advance-based form, a leader that is already ahead of the follower wakes the loop immediately — blocking happens only when there is genuinely nothing to replicate (see the idle-leader note under [Primitive 1](#1-durable_sequencemin_sequence-timeout)).

### Follower Promotion (unplanned)

```
1. Detect leader failure (external coordinator)
2. Follower stops replication loop
3. follower.set_mode(Mode::Leader)
4. Writes resume — next_sequence continues from last ingested sequence
5. Other followers re-target the new leader
```

No sequence reset, no gap. The promoted follower's sequence space is a strict continuation of the old leader's.

### Leadership Transfer (planned)

Graceful leadership transfer uses `set_mode(Mode::Follower)` on the old leader to stop writes, then waits for the target follower to catch up before promoting it.

```
1. old_leader.set_mode(Mode::Follower)        — writes stop immediately
2. Replication loop continues                  — changes_since and durable_sequence are reads
3. Wait: follower.durable_sequence() == old_leader.durable_sequence()
4. follower.set_mode(Mode::Leader)             — writes resume on new leader
5. Other followers re-target the new leader
```

No drain mode is needed. Writes are mutex-serialized — a write is either holding the lock and completes before the mode switch, or it doesn't hold the lock and the next attempt is rejected. There is no intermediate "in-flight" state, so the mode switch is a clean cut.

---

## Other Use Cases

The same primitives that power leader-follower replication also enable:

- **Change Data Capture (CDC)** — tail `changes_since` to transform entries and push to Kafka, Pulsar, or any event bus. The sequence number is an exactly-once cursor.
- **Outbox pattern** — write the domain event and the state change in a single `apply_batch`, then a separate process tails `changes_since` to publish events. No dual-write problem because both are in the same atomic append.

ByteCaskDB does not care who is consuming or why. It surfaces the ordered stream that already exists in its data files.

---

## Correctness Validation

The proof framework for replication primitives follows the same model used in [`docs/correctness_validation.md`](correctness_validation.md): StateShape × OpsShape × FailureClass → expected delta, validated against invariants. The validation proves the correctness guarantees expressed in [`CONTRACT.md`](../CONTRACT.md).

### Proof Framework

Replication correctness reduces to: the follower's `EngineState` after ingesting `changes_since(snap, 0)` from the leader is identical to the leader's `EngineState` at snapshot time.

```
Given:
  L   — a leader EngineState after some workload (StateShape)
  S   — snapshot of L
  I   — changes_since(S, 0) → stream of all committed entries
  FC  — an I/O failure class during ingest (one of SUCCESS, I_B1, I_B2, I_F, I_C, I_G, I_H)

Validate:
  follower.state_after_ingest(I, FC) satisfies all invariants
  and follower.key_dir == leader.key_dir              (on SUCCESS)
  and follower.durable_sequence() == leader.durable_sequence()  (on SUCCESS)
```

The binary question for every case is the same as the write-path model:

```
Was the ingest transition fully persisted?
  Yes → follower state matches leader state
  No  → follower state is unchanged (or a valid prefix on partial ingest)
```

### Foundational Invariant: `changes_since` and `ingest` Are a Paired Contract

For any leader state at snapshot time, `ingest(changes_since(snap, 0))` on an empty follower produces a key directory identical to the leader's *durable* state. This holds regardless of how either primitive is implemented — file boundaries and entry grouping are implementation concerns that do not leak through the contract.

`changes_since` yields only committed, durable entries — including `BulkBegin`/`BulkEnd` markers that frame atomic batches. Incomplete batches (orphaned `BulkBegin` without `BulkEnd`) are excluded by the snapshot's sequence bound. Unsync'd entries (visible in the snapshot but not yet `fdatasync`'d) are excluded — the replication boundary is the durable sequence. `ingest` writes entries atomically using the batch markers: a `BulkBegin`..`BulkEnd` sequence is written as a single `writev`, ensuring that a crash mid-ingest never leaves a partial batch visible on the follower.

This paired contract means the replication protocol has no translation layer and no per-entry-type handling. `Put`, `Delete`, `RangeDel`, and batch markers flow through unchanged. Any future entry type added to the data file format is automatically replicable as long as `changes_since` yields it and `ingest` applies it — neither primitive needs to know what the entry means.

### StateShapes

The leader workload that produces the state to replicate. Each shape exercises a different property of `changes_since` and `ingest`.

| Shape | Description | What it tests |
|-------|-------------|---------------|
| `single_key` | One put | Minimal replication payload — single entry through the full pipeline |
| `multi_key` | Multiple puts | Multi-entry ingest; changes_since yields multiple entries in sequence order |
| `overwrites` | Put then overwrite same key | changes_since yields both entries; ingest applies in sequence order so the overwrite wins |
| `deletes` | Put then delete | Tombstone replicates correctly — follower does not have the key after ingest |
| `range_deletes` | Put N keys then del_range | RangeDel entry walks the follower's key directory over [from, to) |
| `batches` | apply_batch with multiple ops | Batch markers preserved through changes_since; ingest uses them to write atomically on the follower |
| `multi_file` | Enough writes to trigger rotation | changes_since merges entries across multiple sealed files in ascending sequence order |
| `mixed_sync_nosync` | Interleave sync and nosync writes | changes_since yields only entries up to `durable_sequence`; unsync'd entries visible in the snapshot are excluded |
| `nosync_only` | All writes with sync=false, enough to trigger rotation | changes_since yields entries only after rotation triggers fdatasync; replication lag bounded by `max_file_bytes` |
| `nosync_then_sync` | Nosync writes followed by one sync write | The sync's `fdatasync` covers all prior nosync entries; `durable_sequence` catches up and changes_since yields everything |
| `vacuumed_batches` | apply_batch, then vacuum the sealed file containing the batch | changes_since over the vacuumed file yields BulkBegin/BulkEnd markers; ingest writes atomically on the follower |

### OpsShapes

How entries are delivered to the follower. Each shape exercises a different property of `ingest`.

| Shape | Description | What it tests |
|-------|-------------|---------------|
| `full_stream` | Ingest all entries from changes_since(snap, 0) in one call | Baseline correctness — full replication in a single pass |
| `incremental` | Ingest in chunks, simulating the replication loop | Each chunk produces a valid prefix; follower.durable_sequence() advances monotonically |
| `restart_midstream` | Ingest some entries, close follower, reopen, restart from follower.durable_sequence() | Recovery equivalence — reopened follower's durable_sequence() is trustworthy and the next changes_since picks up without gaps |
| `duplicate_delivery` | Re-deliver entries the follower already has | Idempotency — entries with sequence <= durable_sequence() are silently skipped, no state change |
| `planned_promotion` | Replicate NodeA (Leader) → NodeB (Follower) fully, transfer leadership to NodeB, write new entries on NodeB, replicate NodeB → NodeA (backward sync) | Sequence continuity on promoted NodeB; NodeA rejects writes after demotion; NodeA catches up from NodeB and its key_dir matches NodeB's state |

**Out of scope for the primitive proof:** unplanned promotion (old leader crashes, follower promotes, old leader later rejoins). A crashed leader may have locally-recovered entries past the follower's durable sequence, creating a log fork. Reconciling a fork is a coordinator-level concern, not a primitive-level guarantee. The required recovery protocol is simple: a crashed leader must be re-bootstrapped from the new leader via `create_manifest` before rejoining as a follower — never reopened in place.

### FailureClasses

`ingest` uses the same write path as `apply_batch` — append to data file, update key_dir, publish state — but without guards or conflict detection. The failure classes are a subset of those defined in `correctness_validation.md`.

```python
class IngestFailureClass(Enum):
    SUCCESS = "success"                    # no failure — all entries ingested
    I_B1 = "append_fails_nothing_written"  # writev returns -1
    I_B2 = "append_fails_partial_write"    # writev returns short, file tainted
    I_F  = "sync_fails"                   # fdatasync fails — bytes in page cache, not durable
    I_C  = "crash_mid_batch"              # crash after BulkBegin written, before BulkEnd
    I_G  = "rotation_sync_fails"          # pre-rotation fdatasync fails during ingest rotation
    I_H  = "rotation_file_creation_fails" # rotation file creation fails after seal — chunk is durable
```

| Class | Entries applied | key_dir changes | next_seq advances | Degraded |
|-------|----------------|-----------------|-------------------|----------|
| SUCCESS | All | Yes — full delta | Yes | No |
| I_B1 | None | No | Yes (prevent reuse) | Yes |
| I_B2 | None | No | Yes (prevent reuse) | Yes |
| I_F | None visible | No (not published) | Yes (prevent reuse) | Yes |
| I_C | None from the batch | No — recovery discards incomplete batch | Restored to pre-batch | No (recovery handles) |
| I_G | None visible | No (not published) | Yes (prevent reuse) | Yes |
| I_H | Chunk that triggered rotation | Yes — partial delta | Yes | Yes |

**next_seq advance on failure + idempotency — coordinator contract.** On I_B1/I_B2/I_F the follower advances `next_seq` past the failed entries and degrades. A naive re-deliver would hit the idempotency skip (`sequence <= durable_sequence()`) and silently drop data. The coordinator must call `resume()` before retrying: `resume()` truncates the post-durable tail and rebuilds `next_seq` from the recovered state ([`bytecask.cpp` `DB::resume`](../bytecaskdb/bytecask.cpp)), restoring the invariant `next_seq == max(applied seq) + 1`. Re-delivery then proceeds normally.

Class C (orphaned `BulkBegin`) is reachable through the replication pipeline — `changes_since` preserves batch markers, so `ingest` writes `BulkBegin`...entries...`BulkEnd` as a `writev`. If the follower crashes mid-`ingest` after writing `BulkBegin` but before `BulkEnd`, recovery discards the incomplete batch (same as the leader's recovery behavior). Classes I_G and I_H (rotation failures) apply when `ingest` triggers file rotation on the follower because the active file exceeds the size threshold. I_G fails the pre-rotation sync; I_H succeeds on the sync but fails to create the new active file after sealing. I_H is notable: the chunk that triggered rotation is fully durable (the sync succeeded), so the follower's key_dir reflects a partial delta — the entries from that chunk are committed.

### Expected Deltas

| StateShape × OpsShape | SUCCESS | I_B1 / I_B2 / I_F / I_G | I_H |
|----------------------|---------|--------------------------|-----|
| Any × `full_stream` | follower.key_dir == leader.key_dir | follower.key_dir unchanged from baseline | follower.key_dir reflects entries from the chunk that triggered rotation |
| Any × `incremental` | follower.key_dir == leader.key_dir after final chunk | follower.key_dir reflects entries ingested before the failing chunk | Same — partial delta from the durable chunk |
| Any × `restart_midstream` | follower.key_dir == leader.key_dir after second pass | follower.key_dir reflects entries ingested before the simulated crash | Same — partial delta from the durable chunk |
| Any × `duplicate_delivery` | follower.key_dir unchanged (all entries already applied) | N/A — duplicates are skipped before I/O | N/A |
| Any × `planned_promotion` | NodeA.key_dir == NodeB.key_dir after backward sync | N/A — SUCCESS only | N/A |

### create_manifest — FailureClasses

`create_manifest` involves file rotation and waiting for hint generation. Failure classes:

```python
class ManifestFailureClass(Enum):
    SUCCESS = "success"                # rotation + hints complete
    M_R = "rotation_fails"            # active file rotation throws
    M_H = "hint_generation_fails"     # hint file write fails
```

| Class | Manifest produced | Leader state | Expected delta |
|-------|-------------------|-------------|---------------|
| SUCCESS | Yes | Continues accepting writes | manifest.through_sequence == leader.durable_sequence() at rotation time; all sealed files listed |
| M_R | No — exception thrown | Unchanged, continues accepting writes | No state change — rotation is atomic |
| M_H | No — exception or timeout | Active file was rotated (sealed) | Leader has a new active file but no manifest returned |

### Invariants

Checked after every test case:

1. **Sequence continuity**: `follower.durable_sequence() == max(ingested sequences)` on SUCCESS; unchanged on failure.
2. **next_seq monotonicity**: `follower.next_seq` never decreases, even after failed ingest.
3. **Key-value equivalence** (SUCCESS): for every key in leader.key_dir at snapshot time, `follower.get(key)` returns the same value.
4. **No extra keys**: follower.key_dir contains no keys absent from leader.key_dir at snapshot time.
5. **Structural consistency**: same checks as `assert_consistent` in the write-path proof — live_bytes matches key_dir, no dangling file refs, next_seq ahead of all sequences.
6. **Recovery equivalence**: close follower, reopen from disk, verify recovered state matches pre-close state. Proves `ingest` writes are durable and recoverable.
7. **Idempotency**: re-ingesting entries with `sequence <= durable_sequence()` produces no state change — no key_dir delta, no next_seq change.
8. **Durable boundary**: `changes_since` never yields entries with `sequence > durable_sequence`. Validated by running a mixed sync/nosync workload on the leader, taking a snapshot (which includes unsync'd entries in the key directory), and verifying that `changes_since` excludes entries beyond the durable sequence.
9. **Batch atomicity through vacuum**: for the `vacuumed_batches` state shape, `changes_since` over vacuumed files yields `BulkBegin`/`BulkEnd` markers framing the batch entries. `ingest` writes the batch as a single `writev` on the follower. After a simulated crash mid-ingest (Class I_C), recovery discards the incomplete batch — no partial batch is visible.
9a. **Vacuum preserves original sequences**: for every entry in a vacuumed file, the sequence equals the entry's sequence in the pre-vacuum source file. Vacuum never re-numbers. This is load-bearing for replication (otherwise `changes_since` would yield inconsistent sequences after vacuum) and is currently incidental in [`vacuum_scan_and_copy`](../bytecaskdb/bytecask.cpp) — making it a test invariant prevents a future refactor from silently breaking it.
9b. **Per-file sequence monotonicity and sequence-disjoint files**: within every data file (active or sealed, including compacted files), entries appear in strictly ascending sequence order. Additionally, all data files have non-overlapping sequence ranges — no two files contain entries with the same sequence number. This holds naturally: the write path assigns sequences monotonically to the active file, `vacuum_compact_file` scans sequentially and preserves order while maintaining disjoint ranges, and `vacuum_remove_file` deletes the file entirely. This invariant enables linear file scanning for `changes_since` without min-heap merge.
10. **Sequence continuity after promotion**: after `set_mode(Leader)`, the first write on the promoted node gets `sequence == durable_sequence() + 1`. No gap, no reuse of existing sequences.
11. **Mode enforcement**: in `Mode::Follower`, `put`/`del`/`apply_batch` are rejected. In `Mode::Leader`, `ingest` is rejected. `set_mode` transitions are immediate — no in-flight writes straddle the boundary (writes are mutex-serialized).
12. **Backward sync after promotion**: after planned promotion, NodeA (now Follower) replicating from NodeB (now Leader) produces a key_dir identical to NodeB's. The old leader's stale entries are superseded by the new leader's writes through normal sequence ordering.

### E2E Test Structure

Each test follows this structure:

```
ingest tests:
  1. Create leader DB, apply workload (StateShape)
  2. leader.snapshot() → snap
  3. leader.changes_since(snap, 0) → stream
  4. Create follower DB in Mode::Follower
  5. Inject fault per FailureClass
  6. follower.ingest(stream) per OpsShape
  7. assert_replication_invariants(leader, follower, expected)
  8. For degraded cases: follower.resume(), then re-ingest remaining entries
  9. Close follower, reopen, assert_recovery_equivalent

create_manifest tests:
  1. Create leader DB, apply workload (StateShape)
  2. Inject fault per ManifestFailureClass
  3. leader.create_manifest() → manifest (or throws)
  4. On SUCCESS: assert manifest.through_sequence == leader.durable_sequence()
  5. On SUCCESS: assert all sealed files present in manifest.files
  6. Assert leader continues accepting writes regardless of failure class

planned_promotion tests:
  1. Create NodeA (Leader), apply workload (StateShape)
  2. Create NodeB (Follower)
  3. Replicate fully: NodeA.changes_since → NodeB.ingest until caught up
  4. NodeA.set_mode(Mode::Follower)
  5. assert NodeA rejects put/del/apply_batch
  6. NodeB.set_mode(Mode::Leader)
  7. Write new entries on NodeB
  8. assert NodeB.durable_sequence() continues without gap
  9. Replicate NodeB → NodeA: NodeB.changes_since → NodeA.ingest
  10. assert NodeA.key_dir == NodeB.key_dir (backward sync converged)
  11. Close both, reopen, assert_recovery_equivalent
```

### Scenario Matrix Size

11 state shapes × 5 ops shapes × 7 failure classes = 385 ingest tests (before elimination rules).

Elimination rules:
- `duplicate_delivery` × failure classes: duplicates are skipped before I/O, so I_B1/I_B2/I_F/I_C/I_G/I_H are unreachable. Only SUCCESS applies → eliminates 66 tests.
- `planned_promotion` × failure classes: a flag flip — only SUCCESS applies → eliminates 66 tests.
- I_C only applies to state shapes with batches (`batches`, `vacuumed_batches`). For state shapes without batch markers, I_C is unreachable → eliminates 45 tests.
- I_G / I_H only apply to state shapes that trigger file rotation (`multi_file`, `nosync_only`, `vacuumed_batches`). For state shapes without `max_file_bytes`, rotation is unreachable → eliminates 24 tests.
- `nosync_only`: rotation triggers fdatasync, so entries are replicable after rotation. All ops shapes and failure classes apply — no elimination.

11 state shapes × 3 manifest failure classes = **33 manifest tests**.

After elimination: **178 ingest tests** + **33 manifest tests** = **211 tests**.

### Edge Cases

Three edge cases required special handling in the test framework:

1. **Nosync baseline**: `capture_replication_baseline` builds the leader's expected key-value map by replaying `changes_since(snap, 0)` output, not by iterating the snapshot's key directory. This ensures the baseline respects the `durable_seq` boundary — unsync'd entries visible in the snapshot are excluded from the expected follower state.

2. **Batch boundary chunk splitting**: for state shapes with small entry counts and batch markers, the batch boundary adjustment in `gen_incremental_test` can push all entries into chunk 1, leaving chunk 2 empty. The generated tests handle this with a runtime `if (chunk2.empty())` guard that checks success behavior instead of attempting a second ingest.

3. **Restart midstream rotation edge case**: in `restart_midstream` tests, the second `changes_since` pass after recovery returns only the remainder entries. For rotation-class faults (I_H, I_G), this remainder may be too small to trigger file rotation — the fault never fires. The test framework uses try-catch with a `bool threw` tracking pattern to handle both outcomes: if the fault fires, assert failure behavior; if not, assert success behavior.

---

## What ByteCaskDB Does NOT Do

- **Leader election** — external coordinator.
- **Failure detection** — external coordinator.
- **Network transport** — the replication loop is the user's code; ByteCaskDB provides the data.
- **Conflict resolution on split-brain** — out of scope for single-leader replication.
- **Synchronous replication** — not needed and not planned. The client decides whether to wait for follower convergence by calling `follower.durable_sequence(r.sequence, timeout)` — where `r` is the `CommitResult` returned by the leader write — after a write, same pattern as Kafka's producer acks. The durability-vs-latency tradeoff belongs to the client, not the engine. See [`commit_result_api_design.md`](commit_result_api_design.md) for the full read-your-own-writes pattern.
