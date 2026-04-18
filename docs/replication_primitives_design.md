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

### 2. `Snapshot::sequence()` and file manifest

`Snapshot` exposes the sequence at which it was taken and the set of data/hint files it references. While the snapshot is held, those files are pinned against vacuum.

```cpp
class Snapshot {
public:
    auto sequence() const -> uint64_t;
    auto files() const -> std::vector<FileInfo>;  // file_id, path, min_sequence, max_sequence
    // ... existing methods ...
};
```

**Use case:** Bootstrap a new follower — ship the snapshot's files, open them on the follower, then start tailing from `snapshot.sequence()`.

### 3. `file_stats` with `min_sequence` / `max_sequence`

Each file's stats track the minimum and maximum sequence of entries it contains. This allows `changes_since` to skip files that have no entries newer than the target sequence.

Vacuum and file rotation are infrequent events. On each, a secondary index (`vector<file_id>` sorted by `min_sequence`) can be rebuilt cheaply for fast range filtering — but this is an optimization for later.

**Why this matters:** Vacuum rewrites entries into new files, breaking file-level sequence ordering. Without per-file sequence bounds, `changes_since` would need to scan every file.

### 4. `changes_since(from_sequence, out, max_entries)`

Fills `out` with raw entries (sequence, entry_type, key, value) for entries with `sequence > from_sequence`, up to `max_entries`. The caller owns the buffer and reuses it across calls — after the first call sizes up, subsequent calls in the replication loop do zero heap allocations for the vector itself.

Implementation:

1. Filter files where `max_sequence > from_sequence` (using file_stats).
2. Scan those files sequentially, emitting entries where `sequence > from_sequence`.
3. **Incomplete batch filtering:** within each file, track `BulkBegin`/`BulkEnd` markers. If a `BulkBegin` is seen without a matching `BulkEnd` before end of file, all entries since that `BulkBegin` are discarded — they belong to an uncommitted `apply_batch` (e.g. leader crashed mid-write). This is the same logic recovery already uses. `BulkBegin`/`BulkEnd` markers themselves are stripped from the output — the follower's `ingest` does not need them.
4. Stop after `max_entries` entries.

The scan is performed against an internal snapshot so it sees a consistent view and terminates — it does not tail indefinitely.

```cpp
void changes_since(uint64_t from_sequence,
                   std::vector<RawEntry>& out,
                   std::size_t max_entries = 1024) const;
```

Entries include puts, deletes, and range deletes — the full committed changelog. Incomplete batches are excluded. No information is lost to tombstone removal because the data files are the source, not the key directory.

**Note on ordering:** entries are returned in file-scan order, not sequence order. This is safe because the follower uses two-phase ingest (see below): entries are applied to a working copy with sequence-wins resolution, and state is only published to readers once all entries up to a target sequence have been applied.

### 5. `ingest(entries)`, `ingest_commit()`, and `current_sequence()` on the follower

Ingestion on the follower is two-phase:

**`ingest(entries)`** applies raw entries to a working copy of the key directory using sequence-wins resolution (higher sequence always wins for the same key). The working copy is **not visible to readers** until `ingest_commit()` is called. This allows entries to arrive in any order without exposing intermediate states.

**`ingest_commit()`** atomically publishes the working copy as the new `EngineState`, making all ingested changes visible to readers in a single state swap.

**`current_sequence()`** returns the last committed sequence — identical semantics on both leader and follower. On the leader it reflects the latest write; on the follower it reflects the last `ingest_commit()`. External code does not need to know which mode it's talking to. The orchestrator compares `leader.current_sequence()` and `follower.current_sequence()` to measure replication lag.

```cpp
void ingest(std::span<const RawEntry> entries);  // applies to working copy
void ingest_commit();                             // publishes to readers
auto current_sequence() const -> uint64_t;        // last committed sequence
```

After `ingest_commit()`, `next_sequence` reflects `max(ingested sequences) + 1` so that a promoted follower assigns sequences without gaps.

**Constraints:**
- `ingest` and `ingest_commit` are only callable in `Mode::Follower`.
- `ingest` appends to data files and updates the key directory, same as the normal write path, but skips guards and sequence assignment.
- `ingest_commit()` should only be called when all entries up to a known target sequence have been ingested — the orchestrator owns this decision.

### 6. `Mode::Follower`

An engine mode that blocks `put`, `del`, `apply_batch` and allows only `ingest`/`ingest_commit` plus all read operations. The mode can be changed online — no restart required.

```cpp
enum class Mode { Leader, Follower };

void set_mode(Mode mode);
auto mode() const -> Mode;
```

**Promotion:** switch from `Follower` to `Leader`. The engine's `next_sequence` is already correct (advanced by `ingest`), so writes pick up where the old leader left off.

---

## Replication Flow

### Bootstrap

```
1. Follower calls leader: "give me a snapshot"
2. Leader takes snapshot → returns sequence + file manifest
3. Leader ships data files + hint files to follower
   (snapshot pins files against vacuum during transfer)
4. Follower opens ByteCaskDB with Mode::Follower over received files
5. Recovery rebuilds key directory from hint files
6. Follower records bootstrap_seq = snapshot.sequence()
```

### Steady-State Replication Loop

```
std::vector<RawEntry> buf;          // allocated once, reused forever
my_seq = bootstrap_seq
loop:
    target_seq = leader.current_sequence(timeout=30s)
    while my_seq < target_seq:
        leader.changes_since(my_seq, buf, 4096)
        if buf.empty(): break
        follower.ingest(buf)           // applies to working copy, NOT visible to readers
        my_seq = max sequence in buf
    follower.commit_state()            // atomic state swap — readers see all changes at once
```

Under high write load, `current_sequence` returns immediately and `changes_since` naturally batches entries per round trip. Under low load, the long-poll avoids busy-waiting. When the follower is far behind, the inner loop drains the backlog in bounded chunks without unbounded memory growth.

Entries arrive in file-scan order, not sequence order — this is safe because `ingest` uses sequence-wins resolution and nothing is visible until `commit_state()` at the target sequence. At that point, every entry up to `target_seq` has been applied.

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

## What ByteCaskDB Does NOT Do

- **Leader election** — external coordinator.
- **Failure detection** — external coordinator.
- **Network transport** — the replication loop is the user's code; ByteCaskDB provides the data.
- **Conflict resolution on split-brain** — out of scope for single-leader replication.
- **Synchronous replication** — `current_sequence` with long-poll gives near-zero lag. True synchronous replication (writer blocks until follower confirms) would require the writer to wait on a follower ack, adding follower-side `fdatasync` latency to every write. This could be added later as an opt-in `WriteOptions` flag without changing the primitive set.
