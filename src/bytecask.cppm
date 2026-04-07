module;
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

export module bytecask;

export import :internals;
import bytecask.concurrency;
import bytecask.data_file;
import bytecask.radix_tree;
import bytecask.types;
import bytecask.util;

namespace bytecask {

// ---------------------------------------------------------------------------
// Type aliases — public API surface
// ---------------------------------------------------------------------------

// Owned byte buffer — for return values and batch storage.
export using Bytes = std::vector<std::byte>;

// Non-owning view — used for all input parameters to avoid copies.
export using BytesView = std::span<const std::byte>;

// ---------------------------------------------------------------------------
// VacuumOptions — controls vacuum file selection.
// ---------------------------------------------------------------------------
export struct VacuumOptions {
  // Minimum fragmentation ratio (1 − live_bytes / total_bytes) a sealed file
  // must exceed to be eligible for vacuum. Range [0.0, 1.0].
  double fragmentation_threshold{0.5};
  // Maximum live bytes a sealed file may contain to be absorbed into the
  // active file instead of being compacted into a new sealed file.
  // Files above this threshold are always compacted. Default: 1 MiB.
  std::uint64_t absorb_threshold{1ULL * 1024 * 1024};
};

// ---------------------------------------------------------------------------
// Batch
// ---------------------------------------------------------------------------

export struct BatchInsert {
  Bytes key;
  Bytes value;
};

export struct BatchRemove {
  Bytes key;
};

export using BatchOperation = std::variant<BatchInsert, BatchRemove>;

// Move-only, single-use container of operations submitted atomically.
// Consumed by DB::apply_batch().
export class Batch {
public:
  Batch() = default;
  Batch(const Batch &) = delete;
  Batch &operator=(const Batch &) = delete;
  Batch(Batch &&) noexcept = default;
  Batch &operator=(Batch &&) noexcept = default;

  void put(BytesView key, BytesView value) {
    operations_.emplace_back(BatchInsert{Bytes{key.begin(), key.end()},
                                         Bytes{value.begin(), value.end()}});
  }

  void del(BytesView key) {
    operations_.emplace_back(BatchRemove{Bytes{key.begin(), key.end()}});
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    return operations_.empty();
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return operations_.size();
  }

private:
  std::vector<BatchOperation> operations_;
  friend class DB;
};

// Default active-file size threshold: 64 MiB.
export inline constexpr std::uint64_t kDefaultRotationThreshold =
    64ULL * 1024 * 1024;

// ---------------------------------------------------------------------------
// WriteOptions / ReadOptions — modelled after LevelDB / RocksDB.
// ---------------------------------------------------------------------------

// Controls durability behaviour for write operations (put, del, apply_batch).
export struct WriteOptions {
  // When true (default), fdatasync is called after the write completes.
  // Set to false to skip the sync for higher throughput at the cost of
  // durability: data is in the OS page cache but not guaranteed on disk until
  // the next explicit sync or clean engine shutdown.
  bool sync{true};

  // When false (default), the write lock is acquired with a blocking wait.
  // When true, a non-blocking try_lock is attempted; if the lock is already
  // held, throws std::system_error with errc::resource_unavailable_try_again.
  bool try_lock{false};
};

// Controls consistency behaviour for read operations (get, contains_key).
// Two modes:
//   Session (default, staleness_tolerance = 0): read-your-writes guaranteed.
//     Thread-local snapshot refreshes whenever any write occurs.
//   Bounded staleness (staleness_tolerance > 0): snapshot may be up to
//     staleness_tolerance old. Same-thread put→get may return stale data.
//     Use for write-heavy workloads where read throughput matters more than
//     freshness.
export struct ReadOptions {
  // Maximum age of the cached snapshot before the reader refreshes it.
  // 0 (default): refresh on every write — session consistency.
  // > 0: refresh only when the last write is older than this value —
  //      bounded staleness.
  // The writer timestamps each state publication with steady_clock::now();
  // the reader compares that timestamp via a cheap relaxed load, never
  // calling the clock itself. The hot path is a single relaxed load of an
  // int64_t (plain MOV on x86) — no refcount traffic, no locked
  // instructions, no clock read on the reader side.
  std::chrono::milliseconds staleness_tolerance{0};

  // When true, all data read from underlying storage is verified
  // against its CRC32 checksum. When set to false (default) for higher read throughput at
  // the cost of silent corruption detection.
  bool verify_checksums{false};
};

// Options passed to DB::open().
export struct Options {
  // Active-file rotation threshold in bytes (default 64 MiB). When the active
  // file reaches this size it is sealed and a new one is opened.
  std::uint64_t max_file_bytes{kDefaultRotationThreshold};
  // Number of threads used to rebuild the key directory at open time.
  // 1 selects the serial path; >1 uses file-level fan-in parallelism.
  unsigned recovery_threads{4};
};

// ---------------------------------------------------------------------------
// KeyIterator — walks the key directory in ascending key order.
//
// In-memory only: no data file I/O. Satisfies std::input_iterator.
// ---------------------------------------------------------------------------
export class KeyIterator {
public:
  using value_type = Key;
  using difference_type = std::ptrdiff_t;

  KeyIterator() = default;

  explicit KeyIterator(RadixTreeIterator<KeyDirEntry> cur)
      : cur_{std::move(cur)} {
    cache_key();
  }

  auto operator*() const -> const value_type & { return cached_key_; }

  auto operator++() -> KeyIterator & {
    ++cur_;
    cache_key();
    return *this;
  }

  void operator++(int) {
    ++cur_;
    cache_key();
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  void cache_key() {
    if (cur_ != std::default_sentinel) {
      auto [key_span, val] = *cur_;
      cached_key_ = Key{key_span};
    }
  }

  RadixTreeIterator<KeyDirEntry> cur_;
  Key cached_key_;
};

// ---------------------------------------------------------------------------
// EntryIterator — walks the key directory reading values lazily from disk.
//
// Satisfies std::input_iterator. Throws std::system_error on I/O failure.
// ---------------------------------------------------------------------------
export class EntryIterator {
public:
  using value_type = std::pair<Key, Bytes>;
  using difference_type = std::ptrdiff_t;

  EntryIterator() = default;

  EntryIterator(std::shared_ptr<const EngineState> state,
                RadixTreeIterator<KeyDirEntry> cur,
                bool verify_checksums = true)
      : state_{std::move(state)}, cur_{std::move(cur)},
        verify_checksums_{verify_checksums} {}

  auto operator*() const -> const value_type & {
    if (!has_cached_) {
      auto [key_span, dir_entry] = *cur_;
      cached_.first = Key{key_span};
      state_->files->at(dir_entry.file_id)
          ->read_value(dir_entry.file_offset,
                      narrow<std::uint16_t>(key_span.size()),
                      dir_entry.value_size, verify_checksums_,
                      io_buf_, cached_.second);
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> EntryIterator & {
    ++cur_;
    has_cached_ = false;
    return *this;
  }

  void operator++(int) {
    ++cur_;
    has_cached_ = false;
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  std::shared_ptr<const EngineState> state_;
  RadixTreeIterator<KeyDirEntry> cur_;
  mutable value_type cached_;
  bool verify_checksums_{true};
  mutable Bytes io_buf_;
  mutable bool has_cached_{false};
};

// ---------------------------------------------------------------------------
// DB — SWMR key-value store
//
// Forward declaration — full definition after DB.
export class Snapshot;

// ---------------------------------------------------------------------------
// DB — SWMR key-value store
//
// Thread safety: write operations (put, del, apply_batch) are serialised by
// write_mu_. After producing a new EngineState, the writer publishes it via
// state_.store(). Readers call state_.load() without acquiring write_mu_.
// ---------------------------------------------------------------------------
export class DB {
public:
  // Opens or creates a database rooted at dir.
  // Always creates a new active data file.
  // Throws std::system_error if the directory cannot be prepared.
  [[nodiscard]] static auto open(std::filesystem::path dir,
                                 Options opts = {}) -> DB {
    return DB{std::move(dir), std::move(opts)};
  }

  DB(const DB &) = delete;
  DB &operator=(const DB &) = delete;
  DB(DB &&) = delete;
  DB &operator=(DB &&) = delete;

  ~DB();

  // Writes the value for key into out, reusing its existing capacity to
  // amortize allocation across calls. Returns true if the key was found,
  // false otherwise.
  // Throws std::system_error on I/O failure or std::runtime_error on CRC
  // mismatch.
  [[nodiscard]] auto get(const ReadOptions &opts, BytesView key,
                         Bytes &out) const -> bool;

  // Writes key → value. Overwrites any existing value.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  void put(const WriteOptions &opts, BytesView key, BytesView value);

  // Writes a tombstone for key.
  // Returns true if the key existed and was removed, false if it was absent.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  [[nodiscard]] auto del(const WriteOptions &opts, BytesView key) -> bool;

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(BytesView key) const -> bool;

#ifdef BYTECASK_TESTING
  // Returns a snapshot of per-file stats (under write_mu_).
  // Only available in test builds (BYTECASK_TESTING).
  [[nodiscard]] auto file_stats() const -> std::map<std::uint32_t, FileStats> {
    std::lock_guard<std::mutex> lk{*write_mu_};
    return file_stats_;
  }
#endif
  // Atomically applies all operations in batch, wrapped in BulkBegin/BulkEnd.
  // batch is consumed (move-only). No-op if batch.empty().
  // opts.sync controls whether a single fdatasync is issued at the end.
  // Rotates the active file after the sync if the threshold is reached.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  void apply_batch(const WriteOptions &opts, Batch batch);

  // Returns a frozen, move-only, read-only view of the DB at this instant.
  // Holds open any data files referenced at snapshot time until destroyed.
  [[nodiscard]] auto snapshot() const -> Snapshot;

  // Applies batch atomically iff no key in batch was modified since snap.
  // Throws BatchConflict on W-W conflict. Throws std::system_error on I/O failure.
  // No-op if batch is empty.
  void apply_batch_if(const Snapshot &snap, WriteOptions opts, Batch batch);

  // Returns an input range of (key, value) pairs with keys >= from.
  // Pass an empty span to start from the first key. Each dereference reads
  // one value from disk via a single pread (lazy). Results are in ascending
  // key order.
  // Throws std::system_error on I/O failure.
  [[nodiscard]] auto iter_from(const ReadOptions &opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

  // Returns an input range of keys >= from. Walks the in-memory key directory
  // only; no disk I/O.
  [[nodiscard]] auto keys_from(const ReadOptions &opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

  // Selects the highest-fragmentation sealed file above the threshold
  // and either absorbs it into the active file (if it fits) or compacts
  // it into a new sealed file.
  // Returns true if a file was vacuumed, false if no file qualified.
  //
  // Thread-safe: vacuum_mu_ serialises concurrent vacuum() calls independently
  // from write_mu_, so normal put/del/apply_batch calls are not blocked while
  // vacuum scans and rewrites data — only the brief commit step acquires
  // write_mu_. vacuum() is safe to call from a dedicated background thread
  // without any external synchronization.
  //
  // Typical background thread pattern:
  //
  //   while (!stop_requested) {
  //     sleep(1h);
  //     while (db.vacuum()) {
  //       sleep(2s);   // more files may still qualify; keep draining
  //     }
  //   }
  [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;

private:
  explicit DB(std::filesystem::path dir, Options opts);

  // Hint file management
  // Writes hint file via temp-then-rename. Batch-aware; idempotent if .hint exists.
  static void flush_hints_for(const std::shared_ptr<DataFile> &file,
                               const std::filesystem::path &dir);
  // Writes hint files for all sealed files in s.
  void flush_hints(const EngineState &s);
  // Drains background hint tasks then writes all sealed hint files.
  void flush_hints();
  

  // Vacuum helpers
  // Batch-aware scan: copies live Puts and tombstones from source_file into dest_file.
  static auto vacuum_scan_and_copy(
      const std::shared_ptr<const EngineState> &snap,
      const DataFile &source_file, DataFile &dest_file,
      std::uint32_t source_file_id) -> VacuumScanResult;
  // Purges stale files with no in-flight readers. Called at vacuum() start.
  void vacuum_purge_stale_files();
  // Remaps key_dir entries, updates file registry, publishes new state. Caller must hold write_mu_.
  void vacuum_commit(std::uint32_t old_file_id, const VacuumScanResult &scan,
                     std::shared_ptr<DataFile> new_sealed_file);
  // Defers the old data file for removal once no readers hold a reference to it.
  void vacuum_defer_old_file(const std::shared_ptr<const EngineState> &snap,
                             std::uint32_t file_id);
  // Rewrites a sealed file into a new sealed file containing only live entries.
  void vacuum_compact_file(std::uint32_t file_id);
  // Appends live entries from a sealed file into the active file, then removes the sealed file.
  void vacuum_absorb_file(std::uint32_t file_id);

  // FileStats helpers (called under write_mu_)
  // Marks an existing entry as dead in its file's stats.
  void stats_retire_entry(BytesView key, const KeyDirEntry &old);
  // Records a new Put entry: live + total on the active file.
  void stats_publish_put(std::uint32_t active_file_id, BytesView key,
                         BytesView value);
  // Records a tombstone: total only on the active file.
  void stats_publish_tombstone(std::uint32_t active_file_id, BytesView key);
  // Records a bulk marker (BulkBegin / BulkEnd): total only.
  void stats_publish_bulk_marker(std::uint32_t active_file_id);

  // State access
  // Returns a thread-local cached EngineState snapshot; refreshes on staleness.
  [[nodiscard]] auto load_state(const ReadOptions &opts) const
      -> const std::shared_ptr<const EngineState> &;
  // Acquires write_mu_; blocking or try-lock based on opts.try_lock.
  auto acquire_write_lock(const WriteOptions &opts)
      -> std::unique_lock<std::mutex>;

  // Recovery
  // Phase 1: opens all data files, seals them, generates missing hint files.
  auto recovery_prepare_files(EngineState &s) -> std::vector<RecoveredFile>;
  // Builds a RecoveryResult from a set of hint files; no shared mutable state.
  static auto recovery_build_from_hints(std::span<RecoveredFile> files)
      -> RecoveryResult;
  // Merges two RecoveryResults with LSN-based conflict resolution.
  static auto recovery_merge_results(RecoveryResult a, RecoveryResult b)
      -> RecoveryResult;
  // Reconstructs key_dir from hint files using a single thread.
  auto recovery_load_serial(EngineState s) -> EngineState;
  // Reconstructs key_dir using file-level fan-in parallelism.
  auto recovery_load_parallel(EngineState s, unsigned recovery_threads)
      -> EngineState;

  // File rotation
  // Seals active file, dispatches hint write to background, opens new active file.
  [[nodiscard]] auto rotate_active_file(EngineState s) -> EngineState;
  // Calls rotate_active_file if active file has reached rotation_threshold_.
  [[nodiscard]] auto rotate_if_needed(EngineState s) -> EngineState;

  // Shared implementation for apply_batch and apply_batch_if.
  // snap == nullptr: unconditional apply (apply_batch path).
  // snap != nullptr: W-W conflict check before applying (apply_batch_if path).
  void apply_batch_impl(const WriteOptions &opts, Batch batch,
                        const Snapshot *snap);

  // Member variables
  std::filesystem::path dir_;
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  // All mutable state — SWMR. Writers publish via state_.store()
  // under write_mu_; readers call state_.load() (never acquiring write_mu_).
  std::atomic<std::shared_ptr<EngineState>> state_;
  // Written (release) by every state_.store() with steady_clock::now().
  // Stale readers compare this against a thread-local timestamp with a
  // single relaxed load (plain MOV on x86) to decide whether to refresh.
  std::atomic<std::int64_t> state_time_{0};
  // Serialises writers (put, del, apply_batch). Readers never acquire this.
  std::unique_ptr<std::mutex> write_mu_{std::make_unique<std::mutex>()};
  // Group commit: batches concurrent fdatasync calls so one sync covers
  // multiple writers.
  SyncGroup sync_group_;
  // Serialises vacuum() calls. Separate from write_mu_ so vacuum I/O does
  // not block normal writes.
  std::unique_ptr<std::mutex> vacuum_mu_{std::make_unique<std::mutex>()};
  // Protected by vacuum_mu_.
  std::vector<StaleFile> stale_files_;
  // Per-file live/total byte counters for vacuum fragmentation tracking.
  std::map<std::uint32_t, FileStats> file_stats_;
  // Declared last so it destructs first, joining the background thread before
  // any other member is destroyed.
  mutable BackgroundWorker worker_;
};

// ---------------------------------------------------------------------------
// BatchConflict — thrown by apply_batch_if() when a W-W conflict is detected.
// At least one key in the batch was modified after the snapshot was taken.
// ---------------------------------------------------------------------------
export struct BatchConflict : std::exception {
  const char *what() const noexcept override;
};

// ---------------------------------------------------------------------------
// Snapshot — frozen, move-only, read-only view of DB state.
//
// Holds a shared_ptr<const EngineState> that keeps referenced data files open
// until the Snapshot is destroyed. Vacuum defers physical file deletion until
// all Snapshots referencing a file are gone.
// No mutex is acquired on any read method — reads are lock-free.
// ---------------------------------------------------------------------------
export class Snapshot {
public:
  Snapshot(const Snapshot &) = delete;
  Snapshot &operator=(const Snapshot &) = delete;
  Snapshot(Snapshot &&) noexcept = default;
  Snapshot &operator=(Snapshot &&) noexcept = default;

  // Returns true if key exists in this snapshot. No disk I/O.
  [[nodiscard]] auto contains_key(BytesView key) const -> bool;

  // Writes the value for key into out. Returns true if found, false if absent.
  // Throws std::system_error on I/O failure or std::runtime_error on CRC mismatch.
  [[nodiscard]] auto get(BytesView key, Bytes &out) const -> bool;

  // Returns an input range of (key, value) pairs with keys >= from.
  // Results are in ascending key order. Each dereference reads from disk (lazy).
  [[nodiscard]] auto iter_from(BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

  // Returns an input range of keys >= from. Pure in-memory — no disk I/O.
  [[nodiscard]] auto keys_from(BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

private:
  explicit Snapshot(std::shared_ptr<const EngineState> state)
      : state_{std::move(state)} {}
  std::shared_ptr<const EngineState> state_;
  friend class DB; // DB::snapshot() constructs; apply_batch_if() reads state_
};

} // namespace bytecask
