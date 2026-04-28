// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — C++23 module interface: public API surface for the storage engine

module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask;

export import :internals;
import bytecask.batch_iterator;
import bytecask.concurrency;
export import bytecask.counters;
import bytecask.data_file;
import bytecask.radix_tree;
export import bytecask.types;
import bytecask.u32_map;
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
// Mode — defined in bytecask.types; re-exported via the types import.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VacuumOptions — controls vacuum file selection.
// ---------------------------------------------------------------------------
export struct VacuumOptions {
  // Minimum fragmentation ratio (1 − live_bytes / total_bytes) a sealed file
  // must exceed to be eligible for vacuum. Range [0.0, 1.0].
  double fragmentation_threshold{0.5};
};


// Default active-file size threshold: 64 MiB.
export inline constexpr std::uint64_t kDefaultRotationThreshold =
    64ULL * 1024 * 1024;

// Hard limits imposed by the on-disk entry header format and in-memory packing.
// key_size is u16 (2 bytes), value_size is u24 in packed KeyDirEntry (16 MiB).
export inline constexpr std::uint32_t kMaxKeySize = 65535;
export inline constexpr std::uint32_t kMaxValueSize = KeyDirEntry::kMaxValueSize;

// Sensible defaults — keys live in RAM (radix tree), values go to disk.
export inline constexpr std::uint32_t kDefaultMaxKeyBytes = 4096;
export inline constexpr std::uint32_t kDefaultMaxValueBytes =
    4U * 1024 * 1024; // 4 MiB

// Carried by Snapshot and WritePlan so size checks happen at the API boundary.
export struct SizeLimits {
  std::uint32_t max_key_bytes{kDefaultMaxKeyBytes};
  std::uint32_t max_value_bytes{kDefaultMaxValueBytes};
};

inline void check_key_size(std::size_t size, std::uint32_t limit) {
  if (size > limit) {
    throw std::invalid_argument{
        "key size " + std::to_string(size) +
        " exceeds limit " + std::to_string(limit)};
  }
}

inline void check_value_size(std::size_t size, std::uint32_t limit) {
  if (size > limit) {
    throw std::invalid_argument{
        "value size " + std::to_string(size) +
        " exceeds limit " + std::to_string(limit)};
  }
}

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

  // When true, bypasses the write group and executes the write alone under
  // write_mu_. Default false — writes go through the group commit path.
  // Set to true to benchmark solo vs group performance.
  bool solo{false};
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

  // When true (default), all data read from underlying storage is verified
  // against its CRC32 checksum. Set to false for higher read throughput at
  // the cost of silent corruption detection.
  bool verify_checksums{true};
};

// Options passed to DB::open().
export struct Options {
  // Active-file rotation threshold in bytes (default 64 MiB). When the active
  // file reaches this size it is sealed and a new one is opened.
  std::uint64_t max_file_bytes{kDefaultRotationThreshold};
  // Number of threads used to rebuild the key directory at open time.
  // 1 selects the serial path; >1 uses file-level fan-in parallelism.
#ifdef BYTECASK_SINGLE_THREADED
  unsigned recovery_threads{1};
#else
  unsigned recovery_threads{4};
#endif
  // When true (default): any CRC error during recovery causes DB::open to
  // throw std::runtime_error. When false: corrupt entries and hint files are
  // skipped; the DB opens with whatever was successfully recovered, and a
  // warning is printed to stderr for each skipped item.
  bool fail_recovery_on_crc_errors{true};
  // Initial engine mode. Leader (default) allows normal writes; Follower
  // blocks put/del/apply_batch and allows ingest().
  Mode initial_mode{Mode::Leader};
  // Maximum key size in bytes. Keys exceeding this limit are rejected with
  // std::invalid_argument. Hard ceiling: 65,535 (u16 wire format).
  std::uint32_t max_key_bytes{kDefaultMaxKeyBytes};
  // Maximum value size in bytes. Values exceeding this limit are rejected with
  // std::invalid_argument. Hard ceiling: 16,777,215 (24-bit packed KeyDirEntry).
  std::uint32_t max_value_bytes{kDefaultMaxValueBytes};
};

// ---------------------------------------------------------------------------
// KeyIterator — walks the key directory in ascending key order.
//
// In-memory only: no data file I/O. Satisfies std::input_iterator.
// ---------------------------------------------------------------------------
export class KeyIterator {
public:
  using iterator_concept = std::bidirectional_iterator_tag;
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

  auto operator++(int) -> KeyIterator {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  auto operator--() -> KeyIterator & {
    --cur_;
    cache_key();
    return *this;
  }

  auto operator--(int) -> KeyIterator {
    auto tmp = *this;
    --*this;
    return tmp;
  }

  auto operator==(const KeyIterator &other) const noexcept -> bool {
    return cur_ == other.cur_;
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
  using iterator_concept = std::bidirectional_iterator_tag;
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
      if (dir_entry.value_size() == 0) {
        cached_.second.clear();
      } else {
        (*state_->files.get(dir_entry.file_id()))
            ->read_value(dir_entry.file_offset(),
                        narrow<std::uint16_t>(key_span.size()),
                        dir_entry.value_size(), verify_checksums_,
                        io_buf_, cached_.second);
      }
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> EntryIterator & {
    ++cur_;
    has_cached_ = false;
    return *this;
  }

  auto operator++(int) -> EntryIterator {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  auto operator--() -> EntryIterator & {
    --cur_;
    has_cached_ = false;
    return *this;
  }

  auto operator--(int) -> EntryIterator {
    auto tmp = *this;
    --*this;
    return tmp;
  }

  auto operator==(const EntryIterator &other) const noexcept -> bool {
    return cur_ == other.cur_;
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
// ReverseIterator<Iter> — generic reverse adapter for bidirectional iterators
// whose operator* returns a reference to internal cache.
//
// std::reverse_iterator cannot be used here: its operator* dereferences a
// temporary copy of the underlying iterator, which dangles when the iterator
// caches its result internally (KeyIterator, EntryIterator both do).
//
// This adapter holds the underlying iterator directly, pre-decrements once
// in the constructor, and advances by calling operator-- on the inner
// iterator. Because the inner iterator is long-lived, the reference returned
// by operator* remains valid.
// ---------------------------------------------------------------------------
export template <std::bidirectional_iterator Iter>
class ReverseIterator {
public:
  using iterator_concept = std::forward_iterator_tag;
  using value_type = std::iter_value_t<Iter>;
  using difference_type = std::ptrdiff_t;

  ReverseIterator() = default;

  explicit ReverseIterator(Iter past_pos) : cur_{std::move(past_pos)} {
    --cur_;
  }

  auto operator*() const -> std::iter_reference_t<Iter> { return *cur_; }

  auto operator++() -> ReverseIterator & {
    --cur_;
    return *this;
  }

  auto operator++(int) -> ReverseIterator {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  auto operator==(const ReverseIterator &other) const noexcept -> bool {
    return cur_ == other.cur_;
  }

private:
  Iter cur_;
};

export using ReverseKeyIterator = ReverseIterator<KeyIterator>;
export using ReverseEntryIterator = ReverseIterator<EntryIterator>;

// ---------------------------------------------------------------------------
// ChangeIterator — yields raw entries in ascending sequence order (lazy)
//
// Used by changes_since() for replication. Walks data files lazily in
// min_sequence order, scanning one entry at a time via CommittedEntryIterator.
// Yields entries with sequence > from_sequence.
// Holds a snapshot reference to keep file descriptors open during iteration.
// ---------------------------------------------------------------------------

export class ChangeIterator {
public:
  using iterator_category = std::input_iterator_tag;
  using value_type = DataEntryView;
  using difference_type = std::ptrdiff_t;

  ChangeIterator() = default;
  ~ChangeIterator(); // Defined in .cpp where Impl is complete

  // Move-only semantics
  ChangeIterator(const ChangeIterator&) = delete;
  ChangeIterator& operator=(const ChangeIterator&) = delete;
  ChangeIterator(ChangeIterator&&) noexcept; // Defined in .cpp where Impl is complete
  ChangeIterator& operator=(ChangeIterator&&) noexcept; // Defined in .cpp where Impl is complete

  // Constructor for implementation use - not part of public API
  explicit ChangeIterator(std::shared_ptr<const EngineState> state,
                          std::uint64_t from_sequence,
                          std::uint64_t durable_sequence);

  auto operator++() -> ChangeIterator &;
  void operator++(int);
  auto operator*() const -> const value_type &;
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  // Implementation details hidden from public interface
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// DB — SWMR key-value store
//
// Forward declarations — full definitions after DB.
export class Snapshot;
export class WritePlan;

// ---------------------------------------------------------------------------
// TransientEngineState — mutable working copy of engine state.
//
// Created from EngineState via transient(). Owns the mutation logic for all
// state transitions: writes, rotation, vacuum. The coordinator (DB) does IO
// and sequencing but never touches key_dir, file_stats, or sequence directly.
//
// Follows the same transient/persistent discipline as
// TransientRadixTree/PersistentRadixTree: mutations are batched on a
// mutable copy, then committed back to an immutable shared_ptr<EngineState>
// via persistent().
// ---------------------------------------------------------------------------
export class TransientEngineState {
public:
  TransientEngineState(const TransientEngineState &) = delete;
  auto operator=(const TransientEngineState &)
      -> TransientEngineState & = delete;
  TransientEngineState(TransientEngineState &&) noexcept = default;
  auto operator=(TransientEngineState &&) noexcept
      -> TransientEngineState & = default;

  // Precondition validation — pure reads, no mutations.
  // If the plan has a snapshot, checks point guards + range guards + W-W on
  // all write keys. Without a snapshot, checks only point guards that
  // don't need one (MustExist, MustBeAbsent).
  // Returns true if all guards pass.
  [[nodiscard]] auto validate_preconditions(const WritePlan &plan) const
      -> bool;

  // Prepare IO plan — pure read, no mutations.
  // Returns the exact append entries to write: sequence-assigned,
  // BulkBegin/BulkEnd included for multi-op batches.
  // Borrows key/value from the WritePlan; the plan must outlive the result.
  [[nodiscard]] auto prepare_write(const WritePlan &plan) const
      -> std::vector<DataEntryView>;

  // State transition: apply all writes from a plan using pre-computed offsets.
  // Cannot fail — pure in-memory mutations.
  void apply_writes(const WritePlan &plan,
                     std::span<const std::uint64_t> offsets);

  // State transition: register a new file as active after rotation.
  // Cannot fail.
  void apply_rotate_file(std::shared_ptr<DataFile> new_file);

  // State transition: remap keys after vacuum scan+copy.
  // Cannot fail.
  void apply_vacuum(std::uint32_t old_file_id, const VacuumScanResult &scan,
                    std::shared_ptr<DataFile> new_sealed_file);

  // State transition: replay valid committed entries from a resume() scan.
  // Uses sequence-wins resolution to update key_dir and file_stats. Advances
  // next_seq past the highest sequence in the entries. Resets file_stats
  // for file_id (total_bytes = valid_offset, min/max_sequence rebuilt from
  // entries) so the caller does not need mutable file_stats access.
  void apply_resume(std::uint32_t file_id,
                    const std::vector<ResumeEntry> &entries,
                    std::uint64_t valid_offset);

  // State transition: record that all sequences up to batch_max_seq
  // have been confirmed durable by fdatasync. Monotonic — silently
  // ignores a value <= current durable_seq. Cannot fail.
  void apply_sync(std::uint64_t batch_max_seq);

  // State transition: apply pre-sequenced entries from ingest (replication).
  // Like apply_writes but operates on DataEntryView span with leader-assigned
  // sequences. Advances next_seq past the highest ingested sequence.
  void apply_ingest(std::span<const DataEntryView> entries,
                    std::span<const std::uint64_t> offsets);

  // State transition: set engine mode (Leader/Follower).
  void apply_set_mode(Mode mode) { mode_ = mode; }

  // State transition: mark engine as degraded with a reason.
  void apply_degrade(std::string reason) {
    degraded_ = true;
    degraded_reason_ = std::move(reason);
  }

  // State transition: clear degraded state after successful resume().
  void apply_clear_degraded() {
    degraded_ = false;
    degraded_reason_.clear();
  }

  // Pure queries the coordinator needs for IO decisions.
  [[nodiscard]] auto active_file() -> DataFile &;
  [[nodiscard]] auto active_file_ptr() const -> std::shared_ptr<DataFile>;
  [[nodiscard]] auto active_file_id() const noexcept -> std::uint32_t;
  [[nodiscard]] auto is_rotation_needed(std::uint64_t threshold) const -> bool;

  // Returns the current next_seq value — used to capture the post-write sequence
  // before consuming the transient on sync failure (F/G).
  [[nodiscard]] auto next_seq() const noexcept -> std::uint64_t;

  [[nodiscard]] auto durable_seq() const noexcept -> std::uint64_t;

  [[nodiscard]] auto mode() const noexcept -> Mode { return mode_; }
  [[nodiscard]] auto is_degraded() const noexcept -> bool { return degraded_; }
  [[nodiscard]] auto degraded_reason() const noexcept -> const std::string & {
    return degraded_reason_;
  }

  // Returns a mutable reference to file_stats_. Used by resume() to update
  // total_bytes for a truncated active file before publishing state.
  [[nodiscard]] auto file_stats() noexcept -> TransientU32Map<FileStats> & {
    return file_stats_;
  }

  // Commit: consume the transient and produce a new immutable EngineState.
  [[nodiscard]] auto persistent() && -> std::shared_ptr<EngineState>;

private:
  friend class DB;
  friend struct EngineState;
  TransientEngineState(TransientRadixTree<KeyDirEntry> key_dir,
                       TransientU32Map<std::shared_ptr<DataFile>> files,
                       TransientU32Map<FileStats> file_stats,
                       std::uint32_t active_file_id,
                       std::uint32_t next_file_id,
                       std::uint64_t next_seq,
                       std::uint64_t durable_seq,
                       Mode mode,
                       bool degraded,
                       std::string degraded_reason);

  TransientRadixTree<KeyDirEntry> key_dir_;
  TransientU32Map<std::shared_ptr<DataFile>> files_;
  TransientU32Map<FileStats> file_stats_;
  std::uint32_t active_file_id_;
  std::uint32_t next_file_id_;
  std::uint64_t next_seq_;
  std::uint64_t durable_seq_;
  Mode mode_;
  bool degraded_;
  std::string degraded_reason_;
};

// ---------------------------------------------------------------------------
// DB — SWMR key-value store
// ---------------------------------------------------------------------------
// DbDegraded — thrown by write operations when the engine is degraded.
//
// The engine enters degraded state when a write-path failure leaves the
// active file in an inconsistent state. Writes are blocked; reads remain
// available. Call resume() to attempt in-process recovery without restart.
// ---------------------------------------------------------------------------
export class DbDegraded : public std::runtime_error {
public:
  explicit DbDegraded(const std::string &reason)
      : std::runtime_error(reason) {}
  DbDegraded(const DbDegraded &) = default;
  auto operator=(const DbDegraded &) -> DbDegraded & = default;
  ~DbDegraded() override;
};

// ---------------------------------------------------------------------------
// DbFollowerMode — thrown by write operations (put, del, apply_batch) when
// the engine is in Follower mode. Separate from DbDegraded because follower
// mode is intentional, not an error condition.
// ---------------------------------------------------------------------------
export class DbFollowerMode : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
  DbFollowerMode(const DbFollowerMode &) = default;
  auto operator=(const DbFollowerMode &) -> DbFollowerMode & = default;
  ~DbFollowerMode() override;
};

// Default group-write byte-size threshold: plans above this size are routed
// to the solo writer (a large batch would monopolize the group).
export inline constexpr std::uint64_t kGroupWriteMaxBytes = 256ULL * 1024;

// Forward declaration — defined after WritePlan.
export struct EngineSlot;
export struct FileInfo;
export struct FileManifest;

// ---------------------------------------------------------------------------
// DB — the public interface to a ByteCaskDB database.
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

  // Deletes all keys in [from, to). One append to the data file, one
  // optional fdatasync. No-op if from >= to.
  void del_range(const WriteOptions &opts, BytesView from, BytesView to);

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                  BytesView key) const -> bool;

  // Returns the current engine mode. Lock-free (reads published state).
  [[nodiscard]] auto mode() const noexcept -> Mode;

  // Switches the engine between Leader and Follower mode.
  // Acquires write_mu_ to ensure no in-flight write straddles the boundary.
  void set_mode(Mode mode);

  // Returns true if the engine has entered a degraded state. A degraded DB
  // refuses all writes but reads remain available. Call resume() to attempt
  // in-process recovery.
  [[nodiscard]] auto is_degraded() const noexcept -> bool;

  // Returns the reason the engine entered degraded state.
  [[nodiscard]] auto degraded_reason() const noexcept -> std::string;

  // Attempts to recover from a degraded state. If not degraded, returns
  // immediately. On success, clears the degraded flag and the engine accepts
  // writes again. If any step fails, the engine stays degraded and the caller
  // may retry. Recovery is idempotent.
  void resume();

#ifdef BYTECASK_TESTING
  // Returns a snapshot of per-file stats.
  // Only available in test builds (BYTECASK_TESTING).
  [[nodiscard]] auto file_stats() const -> std::map<std::uint32_t, FileStats> {
    auto s = load_state();
    std::map<std::uint32_t, FileStats> result;
    for (const auto [id, fs] : s->file_stats) result.emplace(id, fs);
    return result;
  }

  // Returns the current engine state for invariant checking.
  // Only available in test builds (BYTECASK_TESTING).
  [[nodiscard]] auto engine_state() const -> std::shared_ptr<const EngineState> {
    return load_state();
  }

  // Exposed for testing: validates structural consistency of an EngineState.
  void test_validate_state_consistency(const EngineState &s) const {
    validate_state_consistency(s);
  }
#endif
  // Returns a frozen, move-only, read-only view of the DB at this instant.
  // Holds open any data files referenced at snapshot time until destroyed.
  [[nodiscard]] auto snapshot() const -> Snapshot;

  // Applies plan atomically iff all guards pass and no written key was
  // modified since snap. Returns true if committed, false on conflict.
  // A guardless, snapshot-less plan always commits (returns true).
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  // Returns true (no-op) if plan has no writes and no guards.
  [[nodiscard]] auto apply_batch(WriteOptions opts,
                                 WritePlan plan) -> bool;

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

  // Returns a range of (key, value) pairs in descending key order.
  // When from is non-empty, starts at the last key <= from.
  // When from is empty, starts at the last key in the DB.
  [[nodiscard]] auto riter_from(const ReadOptions &opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;

  // Returns a range of keys in descending order. Pure in-memory — no disk I/O.
  [[nodiscard]] auto rkeys_from(const ReadOptions &opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

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

  // Returns the highest sequence confirmed durable by fdatasync.
  // timeout=0: non-blocking, returns current value.
  // timeout>0: blocks until durable_seq advances past the baseline
  // captured at entry, or timeout expires.
  [[nodiscard]] auto current_sequence(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) const
      -> std::uint64_t;

  // Rotates the active file, waits for all hint files, and returns a
  // manifest of sealed files with a snapshot. Forces file rotation.
  // Vacuum must not run between create_manifest() and file transfer
  // completion (caller responsibility).
  [[nodiscard]] auto create_manifest() -> FileManifest;

  // Returns an iterator that yields raw entries (sequence, entry_type, key, value)
  // for all committed, durable entries with sequence > from_sequence, in ascending
  // sequence order. Used for replication.
  // The upper bound is min(snap.sequence(), durable_sequence) — entries visible
  // in the snapshot but not yet fdatasync'd are excluded.
  [[nodiscard]] auto changes_since(const Snapshot& snap, std::uint64_t from_sequence) const
      -> std::ranges::subrange<ChangeIterator, std::default_sentinel_t>;

  // Applies pre-sequenced entries from a trusted leader. Only callable in
  // Follower mode (throws std::logic_error otherwise). Entries with
  // sequence <= current durable_seq are silently skipped (idempotency).
  // Always syncs; never splits a BulkBegin..BulkEnd across files.
  void ingest(std::span<const DataEntryView> entries);

  // Returns all operational counters and gauges as a flat map.
  // Copies atomic counters (relaxed load) and reads current gauges from
  // EngineState. Designed for pull-based scraping (Prometheus, logging).
  [[nodiscard]] auto stats() const -> std::map<std::string, std::int64_t>;

private:
  explicit DB(std::filesystem::path dir, Options opts);

  // File rotation
  // Seals active file, dispatches hint write to background, opens new active file.
  void rotate_active_file(TransientEngineState &t,
                          const std::shared_ptr<const EngineState> &current);

  // Degrade — sets the engine to write-blocked state with a reason.
  // Used by store_state invariant checks; error catch blocks use
  // apply_degrade() on the transient directly.
  void deem_as_degraded(std::string reason);

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
  // Remaps key_dir entries, updates file registry, publishes new state. Caller must hold write_mu_.
  void vacuum_commit(std::uint32_t old_file_id, const VacuumScanResult &scan,
                     std::shared_ptr<DataFile> new_sealed_file);
  // Unlinks the old data and hint files. Open fds survive (POSIX).
  void vacuum_unlink_old_file(const std::shared_ptr<const EngineState> &snap,
                              std::uint32_t file_id);
  // Rewrites a sealed file into a new sealed file containing only live entries.
  void vacuum_compact_file(std::uint32_t file_id);
  // Appends live entries from a sealed file into the active file, then removes the sealed file.
  void vacuum_remove_file(std::uint32_t file_id);

  // State access helpers — raw state_ / state_time_ access is confined here.
  // Read path: thread-local cached snapshot, may be slightly stale.
  [[nodiscard]] auto load_state_for_read(const ReadOptions &opts) const
      -> const std::shared_ptr<const EngineState> &;
  // Write path: authoritative load, always current. Caller must hold write_mu_.
  [[nodiscard]] auto load_state_for_write() const
      -> std::shared_ptr<EngineState>;
  // Publish new state + bump timestamp. Caller must hold write_mu_.
  // Runs O(1) invariant checks comparing old vs new; degrades on violation.
  void store_state(const std::shared_ptr<const EngineState> &old_state,
                   std::shared_ptr<EngineState> new_state);
  // Publish initial state during construction (no previous state to compare).
  void store_initial_state(std::shared_ptr<EngineState> s);
  // Validates structural consistency of published state. Throws on violation.
  // Called on cold paths only (open, resume).
  void validate_state_consistency(const EngineState &s) const;
  // Writer executors — called by SoloWriter / WriteGroup.
  // Prepares and applies one slot against the transient. Pure in-memory:
  // no I/O. Appends prepared entries to all_entries; running_offset is
  // advanced by the total byte size produced. Returns false on validation
  // failure (sets slot.result).
  auto execute_slot(TransientEngineState &t, EngineSlot &slot,
                    std::vector<DataEntryView> &all_entries,
                    std::uint64_t &running_offset) -> bool;
  // Executor callback shared by solo_writer_ and write_group_. Three phases:
  // (1) per-slot validate/prepare/apply in-memory, (2) one append_entries,
  // (3) sync/rotate/publish.
  void execute_slots(std::vector<Slot *> &batch);

  // Recovery
  // Phase 1: opens all data files, seals them, generates missing hint files.
  auto recovery_prepare_files(EngineState &s)
      -> std::vector<RecoveredFile>;
  // Builds a RecoveryResult from a set of hint files; no shared mutable state.
  static auto recovery_build_from_hints(std::span<RecoveredFile> files,
                                        bool strict) -> RecoveryResult;
  // Merges two RecoveryResults with sequence-based conflict resolution.
  static auto recovery_merge_results(RecoveryResult a, RecoveryResult b)
      -> RecoveryResult;
  // Reconstructs key_dir from hint files. Uses file-level fan-in parallelism
  // when recovery_threads > 1; single-threaded otherwise.
  auto recovery_load_parallel(EngineState s, unsigned recovery_threads,
                               bool strict) -> EngineState;

  // Portable atomic load/store for shared_ptr. The C++20 specialization
  // std::atomic<std::shared_ptr<T>> is not yet available in all libc++
  // versions (e.g. Homebrew LLVM). The C++11 free functions work everywhere
  // but are deprecated in libstdc++ — suppress the warning here once.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  auto load_state() const -> std::shared_ptr<EngineState> {
    return std::atomic_load(&state_);
  }
  void store_state(std::shared_ptr<EngineState> s) {
    std::atomic_store(&state_, std::move(s));
  }
#pragma clang diagnostic pop

  // Member variables
  std::filesystem::path dir_;
  int lock_fd_{-1};  // flock() on dir_/.lock; released by close() in ~DB()
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  SizeLimits size_limits_;
  mutable Counters counters_;
  // All mutable state — SWMR. Writers publish via atomic_store()
  // under write_mu_; readers call atomic_load() (never acquiring write_mu_).
  // Note: std::atomic<std::shared_ptr<T>> (C++20 P0718R2) is not yet
  // available in all libc++ versions (e.g. Homebrew LLVM). Use the C++11
  // free-function overloads instead.
  std::shared_ptr<EngineState> state_;
  // Written (release) by every state_.store() with steady_clock::now().
  // Stale readers compare this against a thread-local timestamp with a
  // single relaxed load (plain MOV on x86) to decide whether to refresh.
  std::atomic<std::int64_t> state_time_{0};
  // Long-poll condvar for durable_seq advances. Notified by store_state
  // when new_state->durable_seq > old_state->durable_seq.
  mutable std::mutex durable_mu_;
  mutable std::condition_variable durable_cv_;
  // Serialises writers (put, del, apply_batch). Readers never acquire this.
  std::unique_ptr<std::mutex> write_mu_{std::make_unique<std::mutex>()};
  // Serialises vacuum() calls. Separate from write_mu_ so vacuum I/O does
  // not block normal writes.
  std::unique_ptr<std::mutex> vacuum_mu_{std::make_unique<std::mutex>()};
  // Solo writer — single-slot execution under write_mu_. Same submit()
  // interface as WriteGroup. Used for large batches or opts.solo benchmarking.
  SoloWriter solo_writer_{[this](auto &b) { execute_slots(b); }};
  // Group writer — leader-applies-all batching. Amortises fdatasync across
  // concurrent writers. Default path for small writes.
  WriteGroup write_group_{[this](auto &b) { execute_slots(b); }};
  // Declared last so it destructs first, joining the background thread before
  // any other member is destroyed.
  mutable BackgroundWorker worker_;

#ifdef BYTECASK_TESTING
public:
  auto& test_write_group() { return write_group_; }
#endif
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
  [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                  BytesView key) const -> bool;

  // Writes the value for key into out. Returns true if found, false if absent.
  // Throws std::system_error on I/O failure or std::runtime_error on CRC mismatch.
  [[nodiscard]] auto get(const ReadOptions& opts, BytesView key,
                         Bytes &out) const -> bool;

  // Returns an input range of (key, value) pairs with keys >= from.
  // Results are in ascending key order. Each dereference reads from disk (lazy).
  [[nodiscard]] auto iter_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;

  // Returns an input range of keys >= from. Pure in-memory — no disk I/O.
  [[nodiscard]] auto keys_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

  // Returns a range of (key, value) pairs in descending key order.
  // When from is non-empty, starts at the last key <= from.
  // When from is empty, starts at the last key in the DB.
  [[nodiscard]] auto riter_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;

  // Returns a range of keys in descending order. Pure in-memory — no disk I/O.
  [[nodiscard]] auto rkeys_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

private:
  explicit Snapshot(std::shared_ptr<const EngineState> state,
                    SizeLimits limits = {})
      : state_{std::move(state)}, limits_{limits} {}
  std::shared_ptr<const EngineState> state_;
  SizeLimits limits_;
  friend class DB;
  friend class TransientEngineState;
  friend class WritePlan;

  // Private accessor for DB::changes_since
  auto state() const -> const std::shared_ptr<const EngineState>& { return state_; }
#ifdef BYTECASK_TESTING
public:
  static auto from_state(std::shared_ptr<const EngineState> s) -> Snapshot {
    return Snapshot{std::move(s)};
  }
#endif
};

// ---------------------------------------------------------------------------
// FileInfo / FileManifest — sealed file inventory for create_manifest().
// ---------------------------------------------------------------------------

export struct FileInfo {
  std::uint32_t file_id;
  std::filesystem::path data_path;
  std::filesystem::path hint_path;
};

export struct FileManifest {
  Snapshot snap;                       // point-in-time read-only view
  std::vector<FileInfo> files;         // sealed data + hint files
  std::uint64_t through_sequence{0};   // last sequence covered
};

// ---------------------------------------------------------------------------
// WritePlan — conditional write + guard vocabulary for apply_batch.
//
// Optionally carries a Snapshot that defines the reference point for
// ensure_unchanged / ensure_range_unchanged guards. When constructed
// without a snapshot, only ensure_present / ensure_absent guards are
// available — calling ensure_unchanged or ensure_range_unchanged on a
// snapshot-less plan throws std::logic_error (programming error).
//
// Implicit W-W check: when a snapshot is present, apply_batch
// automatically rejects the plan if any key in the write set (put or del)
// was modified since the snapshot. This means ensure_unchanged is only
// needed for read-only dependencies — keys whose value influenced the
// plan but that the plan does not modify.
//
// Guards and writes on the same key are merged. Contradictory guards
// throw std::logic_error at build time.
// ---------------------------------------------------------------------------

export class WritePlan {
public:
  enum class Precondition { None, MustExist, MustBeAbsent, MustBeUnchanged };

  struct KeyGuard {
    Precondition precondition{Precondition::None};
  };

  struct RangeGuard {
    Bytes from;
    Bytes to; // exclusive — [from, to)
  };

  struct PointPut {
    Bytes key;
    Bytes value;
  };

  struct PointDel {
    Bytes key;
  };

  struct RangeDel {
    Bytes from;
    Bytes to; // exclusive — [from, to)
  };

  using WriteOp = std::variant<PointPut, PointDel, RangeDel>;

  WritePlan() = default;
  explicit WritePlan(SizeLimits limits) : limits_{limits} {}
  explicit WritePlan(Snapshot snap)
      : snap_{std::move(snap)}, limits_{snap_ ? snap_->limits_ : SizeLimits{}} {}
  WritePlan(const WritePlan &) = delete;
  WritePlan &operator=(const WritePlan &) = delete;
  WritePlan(WritePlan &&) noexcept = default;
  WritePlan &operator=(WritePlan &&) noexcept = default;

  // --- Writes (unconditional) ---

  void put(BytesView key, BytesView value) {
    check_key_size(key.size(), limits_.max_key_bytes);
    check_value_size(value.size(), limits_.max_value_bytes);
    writes_.emplace_back(
        PointPut{Bytes{key.begin(), key.end()},
                 Bytes{value.begin(), value.end()}});
  }

  void del(BytesView key) {
    check_key_size(key.size(), limits_.max_key_bytes);
    writes_.emplace_back(PointDel{Bytes{key.begin(), key.end()}});
  }

  // --- Range writes ---

  void del_range(BytesView from, BytesView to) {
    check_key_size(from.size(), limits_.max_key_bytes);
    check_key_size(to.size(), limits_.max_key_bytes);
    writes_.emplace_back(
        RangeDel{Bytes{from.begin(), from.end()},
                 Bytes{to.begin(), to.end()}});
  }

  // --- Point guards ---

  void ensure_present(BytesView key) {
    check_key_size(key.size(), limits_.max_key_bytes);
    set_precondition(key, Precondition::MustExist);
  }

  void ensure_absent(BytesView key) {
    check_key_size(key.size(), limits_.max_key_bytes);
    set_precondition(key, Precondition::MustBeAbsent);
  }

  // Requires a snapshot — throws std::logic_error if constructed without one.
  void ensure_unchanged(BytesView key) {
    check_key_size(key.size(), limits_.max_key_bytes);
    if (!snap_) {
      throw std::logic_error{
          "WritePlan::ensure_unchanged requires a snapshot"};
    }
    set_precondition(key, Precondition::MustBeUnchanged);
  }

  // --- Range guards ---

  // Conflict if any key in [from, to) was inserted, modified,
  // or deleted since the snapshot. The range is half-open:
  // from is inclusive, to is exclusive.
  // Requires a snapshot — throws std::logic_error if constructed without one.
  void ensure_range_unchanged(BytesView from, BytesView to) {
    check_key_size(from.size(), limits_.max_key_bytes);
    check_key_size(to.size(), limits_.max_key_bytes);
    if (!snap_) {
      throw std::logic_error{
          "WritePlan::ensure_range_unchanged requires a snapshot"};
    }
    range_guards_.push_back(
        {Bytes{from.begin(), from.end()}, Bytes{to.begin(), to.end()}});
  }

  [[nodiscard]] auto has_snapshot() const noexcept -> bool {
    return snap_.has_value();
  }

private:
  [[nodiscard]] auto empty() const noexcept -> bool {
    return writes_.empty() && guards_.empty() && range_guards_.empty();
  }

  [[nodiscard]] auto write_count() const noexcept -> std::size_t {
    return writes_.size();
  }

  // Estimated total I/O bytes for all write operations, including
  // BulkBegin/BulkEnd markers for multi-op plans.
  [[nodiscard]] auto write_bytes() const noexcept -> std::uint64_t {
    std::uint64_t bytes = 0;
    for (const auto &w : writes_) {
      std::visit(
          [&bytes](const auto &op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, PointPut>) {
              bytes += entry_size(op.key.size(), op.value.size());
            } else if constexpr (std::is_same_v<T, PointDel>) {
              bytes += entry_size(op.key.size(), 0);
            } else {
              bytes += entry_size(op.from.size(), op.to.size());
            }
          },
          w);
    }
    if (writes_.size() > 1) bytes += 2 * (kHeaderSize + kCrcSize);
    return bytes;
  }

  auto guard_for(BytesView key) -> KeyGuard & {
    auto k = Bytes{key.begin(), key.end()};
    return guards_[std::move(k)];
  }

  void set_precondition(BytesView key, Precondition pre) {
    auto &g = guard_for(key);
    if (g.precondition != Precondition::None && g.precondition != pre) {
      throw std::logic_error{"WritePlan: contradictory guards on same key"};
    }
    g.precondition = pre;
  }

  std::optional<Snapshot> snap_;
  SizeLimits limits_;
  std::vector<WriteOp> writes_;
  std::map<Bytes, KeyGuard> guards_;
  std::vector<RangeGuard> range_guards_;
  friend class DB;
  friend class TransientEngineState;
};

// ---------------------------------------------------------------------------
// EngineSlot — extends Slot with domain-specific fields for the write path.
//
// Stack-allocated by each caller of apply_batch. Both SoloWriter and
// WriteGroup executors static_cast Slot* to EngineSlot*.
// ---------------------------------------------------------------------------
export struct EngineSlot : Slot {
  WritePlan plan;
  WriteOptions opts;
  bool result{true};
};

} // namespace bytecask
