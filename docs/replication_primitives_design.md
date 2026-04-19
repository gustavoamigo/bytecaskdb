# ByteCaskDB Replication Primitives Design

## Purpose

This document describes the minimal set of API primitives that ByteCaskDB needs to expose so that leader-follower replication can be built **on top of** the engine, without embedding any distributed systems logic inside it. ByteCaskDB remains a passive, embedded key-value store — an external coordinator handles topology, failure detection, and promotion.

Canonical location: `docs/replication_primitives_design.md`.

---

## Terminology

ByteCaskDB uses the term **sequence** for the globally monotonic `u64` counter assigned to every data entry. This is the same concept that other databases call a **log sequence number (LSN)**. We use `sequence` throughout the codebase and on-disk format; this document follows that convention.

---

## Design Principles

1. **ByteCaskDB is passive.** It provides primitives; coordination lives outside.
2. **No callbacks, no pub/sub.** Long-polling unifies push and pull into one API.
3. **Sequences already exist.** Every data entry carries a globally monotonic `u64 sequence`. The primitives surface what the engine already tracks.
4. **The data file is the replication log.** No separate WAL or changelog — the append-only data files contain every put, delete, and range delete with its sequence.

---

## Primitives

### 1. `current_sequence(timeout)`

Returns the current committed sequence. With `timeout=0` it returns immediately (poll mode). With `timeout>0` it blocks until a new commit lands or the timeout expires (long-poll mode).

Internally, the write path calls `cv.notify_all()` after `fdatasync` + `state_.store()`. The long-poll blocks on that condition variable — nanosecond overhead on the write path.

```cpp
auto current_sequence(std::chrono::milliseconds timeout = 0ms) const -> uint64_t;
```

**Use cases:**
- Replication loop: sleep until there's work to do.
- Monitoring: check replication lag (leader sequence vs follower sequence).
- Health checks: immediate poll with `timeout=0`.

### 2. `Snapshot::files()` — file manifest

`Snapshot` exposes the set of sealed data/hint files it references. While the snapshot is held, those files are pinned against vacuum. The active file is excluded — it may have partial or unsynced writes and is being appended to.

```cpp
class Snapshot {
public:
    auto files() const -> std::vector<FileInfo>;  // file_id, path, min_sequence, max_sequence
    // ... existing methods ...
};
```

**Use case:** Bootstrap a new follower — ship the snapshot's sealed files, open them on the follower, run recovery to rebuild the key directory, then start tailing from `follower.current_sequence()`.

### 3. `file_stats` with `min_sequence` / `max_sequence`

Each file's stats track the minimum and maximum sequence of entries it contains. This allows `changes_since` to skip files that have no entries newer than the target sequence.

Vacuum and file rotation are infrequent events. On each, a secondary index (`vector<file_id>` sorted by `min_sequence`) can be rebuilt cheaply for fast range filtering — but this is an optimization for later.

**Why this matters:** Vacuum rewrites entries into new files, breaking file-level sequence ordering. Without per-file sequence bounds, `changes_since` would need to scan every file.

### 4. `changes_since(snap, from_sequence)` — iterator

Returns an iterator that yields raw entries (sequence, entry_type, key, value) for all committed entries with `sequence > from_sequence`, **in ascending sequence order**.

Implementation:

1. Filter files where `max_sequence > from_sequence` (using file_stats).
2. Open a cursor per qualifying **hint file**. Hint files contain the entry metadata (sequence, entry_type, key, value_size, file_offset) — no data file reads needed for the scan itself. Within each hint file, skip entries with `sequence <= from_sequence`.
3. **Merge by sequence:** maintain a min-heap over hint file cursors, yielding the entry with the lowest sequence at each step. This produces a globally ordered stream even when vacuum has rewritten entries into new files in key order rather than sequence order. This is the same fan-in merge pattern that recovery already uses.
4. **Lazy value fetch:** the actual value bytes are read from the data file via `pread` only when the caller consumes the entry. Deletes and range deletes require no data file read at all.
5. **Incomplete batch filtering:** within each file, track `BulkBegin`/`BulkCommit`/`BulkPrepare`/`Bulk2PCCommit` markers. If a `BulkBegin` is seen without a matching `BulkCommit` or `BulkPrepare` before end of file, all entries since that `BulkBegin` are discarded — they belong to an uncommitted batch. Prepared batches (`BulkPrepare` without `Bulk2PCCommit`) are also discarded — the follower only ingests fully committed entries. All batch markers are stripped from the output.

The iterator holds a reference to the `Snapshot`, which pins all data files alive for the duration of the scan — vacuum cannot reclaim them mid-iteration.

```cpp
auto changes_since(const Snapshot& snap, uint64_t from_sequence) const -> ChangeIterator;
```

When the iterator is exhausted, all committed entries up to the snapshot's sequence have been delivered in order.

Because entries are sequence-ordered, every prefix of the stream is a valid state. The follower can `ingest()` at any point — not just at iterator exhaustion — and the result is always a consistent, sequence-ordered prefix of the leader's history.

**Failure handling:** on failure mid-iteration, restart Phase 2 with a fresh snapshot and iterator from `follower.current_sequence()`. Since entries are in sequence order, the follower's recovered state after a crash is a valid prefix — `current_sequence()` is trustworthy.

### 5. `ingest(entries)` and `current_sequence()` on the follower

**`ingest(entries)`** applies raw entries to the key directory and publishes the updated state to readers immediately. Because `changes_since` delivers entries in ascending sequence order, every `ingest` call produces a valid, consistent prefix of the leader's history — no separate commit step is needed.

**`current_sequence()`** returns the last ingested sequence — identical semantics on both leader and follower. On the leader it reflects the latest write; on the follower it reflects the last `ingest`. External code does not need to know which mode it's talking to. The orchestrator compares `leader.current_sequence()` and `follower.current_sequence()` to measure replication lag.

```cpp
void ingest(std::span<const RawEntry> entries);  // applies and publishes
auto current_sequence() const -> uint64_t;        // last committed sequence
```

After each `ingest`, `next_sequence` reflects `max(ingested sequences) + 1` so that a promoted follower assigns sequences without gaps.

**Constraints:**
- `ingest` is only callable in `Mode::Follower`.
- Appends to data files and updates the key directory, same as the normal write path, but skips guards and sequence assignment.

### 6. `Mode::Follower`

An engine mode that blocks `put`, `del`, `apply_batch` and allows only `ingest` plus all read operations. The mode can be changed online — no restart required.

```cpp
enum class Mode { Leader, Follower };

void set_mode(Mode mode);
auto mode() const -> Mode;
```

**Promotion:** switch from `Follower` to `Leader`. The engine's `next_sequence` is already correct (advanced by `ingest`), so writes pick up where the old leader left off.

---

## Replication Flow

### Phase 1 — Bootstrap (new follower only)

```
1. Follower calls leader: "give me a snapshot"
2. Leader takes snapshot → returns file manifest
3. Leader ships sealed data files + hint files to follower
   (snapshot pins files against vacuum during transfer;
    active file excluded — only sealed files are shipped)
4. Follower opens ByteCaskDB with Mode::Follower over received files
5. Recovery rebuilds key directory from hint files
6. Proceed to Phase 2
```

### Phase 2 — Replicate (loop forever, restart on any failure)

Because `changes_since` delivers entries in sequence order, every prefix of the stream is a valid state. After a crash, `follower.current_sequence()` reflects a consistent prefix — it is trustworthy and the orchestrator does not need to persist its own `from_seq`.

```
loop:
    target_seq = leader.current_sequence(timeout=30s)
    snap = leader.snapshot()
    it = leader.changes_since(snap, follower.current_sequence())
    for entry in it:
        follower.ingest(entry)
    // on failure at any point: loop restarts from follower.current_sequence()
```

Under high write load, `current_sequence` returns immediately and the iterator yields many entries per iteration. Under low load, the long-poll avoids busy-waiting.

### Follower Promotion

```
1. Detect leader failure (external coordinator)
2. Follower stops replication loop
3. follower.set_mode(Mode::Leader)
4. Writes resume — next_sequence continues from last ingested sequence
5. Other followers re-target the new leader
```

No sequence reset, no gap. The promoted follower's sequence space is a strict continuation of the old leader's.

---

## Other Use Cases

The same primitives that power leader-follower replication also enable:

- **Change Data Capture (CDC)** — tail `changes_since` to transform entries and push to Kafka, Pulsar, or any event bus. The sequence number is an exactly-once cursor.
- **Outbox pattern** — write the domain event and the state change in a single `apply_batch`, then a separate process tails `changes_since` to publish events. No dual-write problem because both are in the same atomic append.

ByteCaskDB does not care who is consuming or why. It surfaces the ordered stream that already exists in its data files.

---

## What ByteCaskDB Does NOT Do

- **Leader election** — external coordinator.
- **Failure detection** — external coordinator.
- **Network transport** — the replication loop is the user's code; ByteCaskDB provides the data.
- **Conflict resolution on split-brain** — out of scope for single-leader replication.
- **Synchronous replication** — not needed and not planned. The client decides whether to wait for follower convergence by polling `follower.current_sequence()` after a write — same pattern as Kafka's producer acks. The durability-vs-latency tradeoff belongs to the client, not the engine.
