# ByteCaskDB API Reference

Usage examples and the public C++ interface, with its error-handling rules. The source of truth is [`include/bytecask.hpp`](../include/bytecask.hpp); [`engine_api_design.md`](engine_api_design.md) records the design decisions behind it.

## Usage examples

```cpp
import bytecask;
using namespace bytecask;

// Open (or create) a database directory.
auto db = DB::open("my_db");

// Single-key operations.
db.put({}, to_bytes("user:1"), to_bytes("alice"));

Bytes out;
bool found = db.get({}, to_bytes("user:1"), out);   // true; value in out
auto del_result = db.del({}, to_bytes("user:1"));    // nullopt if key was absent

// Range deletion — delete all keys in [from, to) with a single disk append.
db.del_range({}, to_bytes("session:"), to_bytes("session:~"));

// Atomic batch — all operations land atomically. Every write returns a
// CommitResult (or nullopt on conflict) carrying the assigned sequence and
// whether fdatasync confirmed it durable before return.
WritePlan plan;
plan.put(to_bytes("user:2"), to_bytes("bob"));
plan.put(to_bytes("user:3"), to_bytes("carol"));
plan.del(to_bytes("user:1"));
(void)db.apply_batch({}, std::move(plan));

// Decrement stock — write keys are checked for conflicts automatically.
auto snap = db.snapshot();
Bytes stock_out;
snap.get({}, to_bytes("stock:widget"), stock_out);
// ... decrement stock count ...
WritePlan plan2{std::move(snap)};
plan2.put(to_bytes("stock:widget"), new_stock);
if (!db.apply_batch({}, std::move(plan2))) {
    // another writer changed stock:widget since our snapshot — retry
}

// Place order at current price — ensure_unchanged guards keys you read
// but don't write. Write keys are checked automatically.
auto snap2 = db.snapshot();
Bytes price_out;
snap2.get({}, to_bytes("price:widget"), price_out);
// ... compute order_total from price ...
WritePlan order{std::move(snap2)};
order.ensure_unchanged(to_bytes("price:widget"));  // reject if price changed
order.put(to_bytes("order:99"), order_total);
if (!db.apply_batch({}, std::move(order))) {
    // price changed since snapshot — re-read price and recompute
}

// Prefix scan — keys and values fetched lazily from disk as it advances.
for (auto& [key, value] : db.iter_from({}, to_bytes("user:"))) {
    // Iterates all keys >= "user:" in ascending order.
}

// Keys-only prefix scan — reads each key's record header, skips the value.
for (auto& key : db.keys_from({}, to_bytes("user:"))) { ... }

// Reverse scan — descending key order. Starts at last key <= "user:~".
for (auto& [key, value] : db.riter_from({}, to_bytes("user:~"))) { ... }

// Reverse keys-only — descending order.
for (auto& key : db.rkeys_from({}, to_bytes("user:~"))) { ... }

// Close — makes every write durable, and throws if it could not.
// The destructor closes too, but has to swallow the error.
db.close();
```

> `to_bytes` is a small helper that converts a `std::string_view` to `BytesView`:
> ```cpp
> auto to_bytes(std::string_view sv) -> BytesView {
>     return std::as_bytes(std::span{sv.data(), sv.size()});
> }
> ```

## Interface

```cpp
namespace bytecask {

struct Options {
    uint64_t max_file_bytes{64 * 1024 * 1024};  // active file rotation threshold (default 64 MiB, hard ceiling: 3 GiB; 0 seals a file after every write)
    unsigned recovery_threads{4};                // parallelism for hint-file replay at open (0 is refused)
    // A hint file is a rebuildable index: one that fails its CRC, or that a
    // read fails on, is regenerated from its data file in both modes and
    // costs no keys. This
    // setting governs what is left — a data file that cannot be rescanned.
    // When true (default): DB::open throws. When false: that file is skipped
    // and the DB opens with the keys recovered from the rest, with a warning
    // on stderr for each one. It does not reach a data file's tail: a torn
    // tail in the newest file is truncated, and damage in any other file
    // without a hint is refused, in both modes.
    bool fail_recovery_on_crc_errors{true};
    Mode initial_mode{Mode::Leader};             // leader allows normal writes; follower allows ingest
    // A setting above its hard ceiling makes open throw std::invalid_argument.
    uint32_t max_key_bytes{4096};                // max key size (hard ceiling: 65,535 — u16 wire format)
    uint32_t max_value_bytes{4 * 1024 * 1024};   // max value size (hard ceiling: 2^28 - 1, 256 MiB less one byte)
    IoBackend io_backend{IoBackend::Pread};      // how sealed files are read
    BufferPoolOptions buffer_pool{};             // only read when io_backend == BufferPool
    // Most sealed files allowed to wait for their hint file. A rotation that
    // would exceed it waits for the background writer, stalling writes (not
    // reads), so the work left for close — and for the next open after a
    // crash — stays bounded. 0 turns the bound off.
    uint32_t max_hint_backlog{4};
    size_t thread_scratch_bytes{1024 * 1024};   // most a thread's record buffers keep after a call (at least 256 KiB)
};

// Selects how sealed data files are read. The active file is written the same
// way in every mode.
enum class IoBackend {
    Pread,       // pread(2) per read (default)
    Mmap,        // sealed files memory-mapped; zero-copy reads. On Linux a read
                 // the kernel cannot complete is SIGBUS, not std::system_error;
                 // on macOS a file truncated underneath can serve its old bytes
    BufferPool,  // sealed files served from a bounded, engine-owned cache
};

struct BufferPoolOptions {
    size_t capacity_bytes{0};          // TOTAL pool footprint, frames plus index.
                                       // Must be >= 2 x max_file_bytes.
    bool direct_io{true};              // O_DIRECT fills: the pool, not the page cache,
                                       // holds sealed-file data. Falls back per file
                                       // where the filesystem refuses.
};

struct WriteOptions {
    bool sync{true};      // call fdatasync after write (default true)
    bool solo{false};     // bypass group commit — route to solo writer (for benchmarking)
};

// A read sees every write that returned before it began, and every write any
// other read has already seen, on any thread.
struct ReadOptions {
    bool verify_checksums{true}; // CRC-verify each value read from disk (default true)
};

struct VacuumOptions {
    double fragmentation_threshold{0.5}; // a file is eligible above this share of dead bytes
    // Nothing above this sequence is dropped, so changes_since from any
    // sequence >= retain_after stays complete. Set by the replication service
    // to the lowest position a follower still needs; kNoRetention (-1), the
    // default, restricts nothing.
    uint64_t retain_after{kNoRetention};
};

enum class Mode { Leader, Follower };

// Outcome of a committed write: the highest sequence assigned (0 = nothing
// written) and whether fdatasync confirmed it durable before return. A
// reader — local or follower — whose durable_sequence() >= sequence is
// guaranteed to see every entry of this write.
struct CommitResult {
    uint64_t sequence{0};
    bool durable{false};
};

class DB {
public:
    // DB is non-copyable and non-moveable; open() relies on mandatory copy elision.
    [[nodiscard]] static auto open(std::filesystem::path dir,
                                   Options opts = {}) -> DB;

    // Writes value for key into out, reusing its capacity. Returns true if found.
    // Throws std::system_error on I/O failure, std::runtime_error on CRC mismatch,
    // or DbDegraded if the engine is degraded.
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;

    // Writes key → value. Overwrites any existing value. Cannot conflict.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    auto put(const WriteOptions& opts, BytesView key, BytesView value) -> CommitResult;

    // Writes a tombstone for key. nullopt if the key was absent — nothing written.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto del(const WriteOptions& opts, BytesView key) -> std::optional<CommitResult>;

    // Deletes all keys in [from, to) with a single data file append. Cannot conflict.
    // Throws std::invalid_argument, before anything is written, if from >= to.
    // Throws std::system_error on I/O failure or DbDegraded.
    auto del_range(const WriteOptions& opts, BytesView from, BytesView to) -> CommitResult;

    [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                    BytesView key) const -> bool;

    // Atomically applies all operations in plan. nullopt on conflict (guard
    // failure or implicit W-W check) — nothing was written. An empty or
    // guard-only plan that passes writes nothing and returns {sequence = 0,
    // durable = true}. With sync it first makes every earlier write durable, so
    // apply_batch({.sync = true}, WritePlan{}) flushes earlier sync=false writes.
    // A plan over 1 GiB on disk, batch markers included, throws
    // std::invalid_argument before anything is written.
    // Throws std::system_error on I/O failure or DbDegraded if the engine is degraded.
    [[nodiscard]] auto apply_batch(WriteOptions opts,
                                   WritePlan plan) -> std::optional<CommitResult>;

    // Returns a frozen, move-only, read-only view of the DB at this instant.
    // Holds open referenced data files until destroyed — vacuum deferred automatically.
    [[nodiscard]] auto snapshot() const -> Snapshot;

    [[nodiscard]] auto iter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

    [[nodiscard]] auto keys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

    [[nodiscard]] auto riter_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;

    [[nodiscard]] auto rkeys_from(const ReadOptions& opts, BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

    // Returns true if a file was reclaimed. A file qualifies by the share of
    // its bytes that are overwritten or deleted values; tombstones and batch
    // markers count towards neither side. False when no file qualified or
    // none could be made smaller.
    [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;

    // The single sequence primitive — returns the highest sequence confirmed
    // durable by fdatasync. min_sequence = 0, an already-reached target, or a
    // nonpositive timeout return immediately without blocking. Otherwise blocks
    // until durable_seq >= min_sequence or timeout expires, then returns the
    // watermark; milliseconds::max() waits with no deadline. Covers polling (default args), replication wake-up
    // (follower.durable_sequence() + 1), and read-your-own-writes waits
    // (a CommitResult's sequence).
    [[nodiscard]] auto durable_sequence(
        std::uint64_t min_sequence = 0,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) const
        -> std::uint64_t;

    // Rotates the active file, waits for all hint files, and returns a
    // manifest of sealed files with a snapshot. Forces file rotation. A
    // listed hint may be missing, if its write failed; opening the copied
    // files rebuilds it.
    // Vacuum must not run between create_manifest() and file transfer
    // completion (caller responsibility).
    [[nodiscard]] auto create_manifest() -> FileManifest;

    // Returns the current engine mode (Leader or Follower).
    [[nodiscard]] auto mode() const noexcept -> Mode;
    // Switches mode under the write mutex. No in-flight write straddles the transition.
    // A leader stepping down fdatasyncs first: every write it acknowledged,
    // sync or not, is durable and so shippable by changes_since. A follower
    // promoted appends a change marker at the next sequence and fdatasyncs it
    // before the mode changes; the first write takes the sequence after it.
    // A failed append or sync degrades and leaves the mode. Leader -> Leader
    // writes nothing; opening in Leader mode is not a promotion.
    void set_mode(Mode mode);

    // One slice of this node's history for ingest: a header naming this
    // node's marker at from_sequence, and the whole units above it that were
    // durable at snap, cut after the unit that passes max_bytes. The entries
    // are read lazily.
    [[nodiscard]] auto changes_since(const Snapshot& snap, std::uint64_t from_sequence,
                                     std::size_t max_bytes = kUnlimitedBytes) const
        -> ChangeBatch;

    // Applies one slice, as changes_since produced it. Follower mode only.
    // Idempotent: entries with sequence <= durable_sequence() are skipped.
    // Before anything is written: a slice whose from_sequence is past
    // durable_sequence() is a gap (std::invalid_argument); one whose marker
    // at the comparison point is not this node's comes from a history that
    // diverged at a promotion (DbChangeMarkerMismatch). An empty slice is
    // checked too. Throws std::logic_error if not in follower mode,
    // DbDegraded if degraded, std::invalid_argument for a slice that ends
    // inside an atomic batch, is not strictly increasing, holds a malformed
    // marker, a sequence above 2^48 - 1 or an atomic batch over 1 GiB.
    void ingest(const ChangeHeader& header, std::span<const DataEntryView> entries);

    // True if the engine has entered a degraded state from a write-path failure.
    // Reads remain available; all write operations throw DbDegraded.
    [[nodiscard]] auto is_degraded() const noexcept -> bool;
    // Why the engine is degraded, or empty. Returned by value: may throw
    // std::bad_alloc.
    [[nodiscard]] auto degraded_reason() const -> std::string;

    // Attempts to recover from a degraded state. Rewrites and syncs the
    // active file, so nothing a failed fdatasync left only in the page cache
    // is trusted, then scans it to find the last valid committed offset,
    // truncates garbage bytes, syncs, seals the file, and opens a new active
    // file. On success, clears the degraded
    // flag. On failure, the engine stays degraded and the caller may retry.
    // It only trims what a failed write left behind: damage found in data
    // already acknowledged throws std::runtime_error and leaves the file
    // untouched — corrupt acknowledged data is refused, not repaired.
    // No-op if the engine is not degraded.
    void resume();

    // Returns all operational counters and gauges as a flat map.
    // Monotonic counters (bytes_written, fsyncs, …) and current-state
    // gauges (degraded, open_files, hint_backlog, keydir_versions_live,
    // keydir_nodes_parked). Designed for pull-based scraping.
    [[nodiscard]] auto stats() const -> std::map<std::string, std::int64_t>;

    // Makes every write durable, sync = false ones included, writes the hint
    // files and releases the directory lock. Waits for in-flight writes and a
    // running vacuum. Returns only if every acknowledged write is durable and
    // the shutdown completed; otherwise throws std::system_error (a failed
    // fdatasync or hint write) or DbDegraded (a degraded engine holding
    // acknowledged writes that are not durable). Closed either way: every
    // later call throws DbClosed, except mode(), is_degraded() and
    // degraded_reason(). A second close() returns at once. ~DB calls close()
    // if it was not called, and swallows its errors.
    void close();
};

// Frozen, move-only, read-only view of DB state at a point in time.
// Holds open any referenced data files until destroyed.
class Snapshot {
public:
    [[nodiscard]] auto get(const ReadOptions& opts,
                           BytesView key, Bytes& out) const -> bool;
    [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                    BytesView key) const -> bool;
    [[nodiscard]] auto iter_from(const ReadOptions& opts,
                                 BytesView from = {}) const
        -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
    [[nodiscard]] auto keys_from(const ReadOptions& opts,
                                 BytesView from = {}) const
        -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;
    [[nodiscard]] auto riter_from(const ReadOptions& opts,
                                  BytesView from = {}) const
        -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;
    [[nodiscard]] auto rkeys_from(const ReadOptions& opts,
                                  BytesView from = {}) const
        -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;
    // Live keys in [from, to), counted no further than limit: returns
    // min(count, limit). At most two record reads, whatever the range holds.
    // Throws std::invalid_argument if from >= to.
    [[nodiscard]] auto count_keys(BytesView from, BytesView to,
                                  std::size_t limit) const -> std::size_t;
};

// Sealed file descriptor returned by create_manifest().
struct FileInfo {
    std::uint32_t file_id;
    std::filesystem::path data_path;
    std::filesystem::path hint_path;
};

// Manifest of sealed files with a point-in-time snapshot.
// Returned by DB::create_manifest().
struct FileManifest {
    Snapshot snap;                       // frozen read-only view
    std::vector<FileInfo> files;         // sealed data + hint files
    std::uint64_t through_sequence{0};   // last sequence covered
};

// Write plan for apply_batch. Groups multiple operations into a single atomic write.
// Construct with WritePlan(snap) to enable ensure_unchanged / ensure_range_unchanged guards;
// those methods throw std::logic_error if called on a snapshot-less WritePlan().
// When a snapshot is present, apply_batch automatically rejects the plan if any
// write key (put or del) changed since the snapshot — no explicit guard needed on
// keys in the write set. Use ensure_unchanged for read-only dependencies: keys whose
// value influenced the plan but that the plan does not modify.
// A guardless, snapshot-less WritePlan always commits successfully (no conflict possible).
class WritePlan {
public:
    WritePlan();                         // snapshot-less: only ensure_present/ensure_absent available
    explicit WritePlan(Snapshot snap);   // snapshot embedded; all guards available

    void put(BytesView key, BytesView value);
    void del(BytesView key);
    void del_range(BytesView from, BytesView to);  // range delete: [from, to); from >= to throws

    void ensure_present(BytesView key);                         // guard: key must exist
    void ensure_absent(BytesView key);                          // guard: key must be absent
    void ensure_unchanged(BytesView key);                       // guard: key unchanged since snapshot
    void ensure_range_unchanged(BytesView from, BytesView to);  // guard: no key change in [from, to)

    [[nodiscard]] auto has_snapshot() const noexcept -> bool;
};

// The identity of a history from a promotion on: the marker entry's own
// sequence and a random id. A node's marker at a sequence is its last marker
// at or below it, kOriginMarker ({0, 0}) if it holds none.
struct ChangeMarker {
    std::uint64_t since_sequence;
    std::uint64_t id;
};

// Where a slice starts: the source's marker at from_sequence, and from_sequence.
struct ChangeHeader {
    ChangeMarker marker;
    std::uint64_t from_sequence;
};

// One changes_since result, which is one ingest call.
struct ChangeBatch {
    ChangeHeader header;
    std::ranges::subrange<ChangeIterator, std::default_sentinel_t> entries;  // lazy
};

// Non-owning view of a data entry. Used by changes_since (read path) and
// ingest (write path). The owning counterpart is DataEntry.
struct DataEntryView {
    std::uint64_t sequence;
    EntryType entry_type;
    std::span<const std::byte> key;
    std::span<const std::byte> value;
};

// Thrown by write operations when the engine is in a degraded state.
class DbDegraded : public std::runtime_error { /* ... */ };

// Thrown by put/del/del_range/apply_batch when the engine is in follower mode.
class DbFollowerMode : public std::runtime_error { /* ... */ };

// Thrown by ingest for a slice of a history that diverged from this node's
// at a promotion. Nothing was written; re-bootstrap from a manifest.
class DbChangeMarkerMismatch : public std::runtime_error { /* ... */ };

// Thrown by every DB operation after close().
class DbClosed : public std::logic_error { /* ... */ };

} // namespace bytecask
```

Error handling follows the throw-on-failure convention used by the C++ standard library: a key, value or plan over its limit, or an option over its hard ceiling at `open`, throws `std::invalid_argument` before anything is written; I/O failures throw `std::system_error`; data corruption throws `std::runtime_error`; an internal invariant violation that cannot be continued through safely — currently only a reused data file name, which would silently drop writes at recovery — prints to stderr and aborts the process rather than throwing, since the write path would otherwise catch it and retry into the same corrupt state; write operations on a degraded engine throw `DbDegraded` (a `std::runtime_error` subclass, catchable separately); normal writes in follower mode throw `DbFollowerMode`; an `ingest` from a history that diverged at a promotion throws `DbChangeMarkerMismatch`; any call on a closed `DB` throws `DbClosed` (a `std::logic_error`). Key-not-found is signalled by `get` returning `false`; `apply_batch` (and `del`) return `nullopt` on precondition or W-W conflict — conflicts are expected outcomes, not exceptional errors. Every committed write returns a `CommitResult{sequence, durable}` — a wait-friendly token for read-your-own-writes across replication (see `durable_sequence` above).
