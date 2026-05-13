// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — C++23 module: public API surface and engine implementation

module;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <system_error>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module bytecask;

export import :internals;
import bytecask.batch_iterator;
import bytecask.concurrency;
export import bytecask.counters;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_file;
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
  // When true, sealed files are memory-mapped for zero-copy reads.
  // When false (default), reads use pread(2), avoiding virtual address space
  // pressure under memory contention.
  bool use_mmap{false};
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
// EntryIterator — zero-copy forward iterator over key/value spans.
//
// Yields EntryView with non-owning spans into mmap (sealed files) or an
// internal io_buf (active file). Spans are valid until the next operator++().
// Uses ValueIterator internally — no key reconstruction during tree walk.
// ---------------------------------------------------------------------------
export class EntryIterator {
public:
  using iterator_concept = std::input_iterator_tag;
  using value_type = EntryView;
  using difference_type = std::ptrdiff_t;

  EntryIterator() = default;

  EntryIterator(std::shared_ptr<const EngineState> state,
                ValueIterator<KeyDirEntry> cur,
                bool verify_checksums = true)
      : state_{std::move(state)}, cur_{std::move(cur)},
        verify_checksums_{verify_checksums} {}

  auto operator*() const -> const EntryView & {
    if (!has_cached_) {
      auto &dir_entry = *cur_;
      auto &file = *(*state_->files.get(dir_entry.file_id()));
      if (verify_checksums_) {
        raw_cached_ = file.read_entry(dir_entry.file_offset(),
                                      dir_entry.value_size(), io_buf_);
      } else {
        raw_cached_ = file.read_entry_unverified(dir_entry.file_offset(),
                                                 dir_entry.value_size(), io_buf_);
      }
      cached_ = EntryView{.key = raw_cached_.key, .value = raw_cached_.value};
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> EntryIterator & {
    ++cur_;
    has_cached_ = false;
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  std::shared_ptr<const EngineState> state_;
  ValueIterator<KeyDirEntry> cur_;
  bool verify_checksums_{true};
  mutable DataEntryView raw_cached_;
  mutable EntryView cached_;
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

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  Iter cur_;
};

export using ReverseKeyIterator = ReverseIterator<KeyIterator>;

// ---------------------------------------------------------------------------
// ReverseEntryIterator — zero-copy reverse iterator over key/value spans.
// ---------------------------------------------------------------------------
export class ReverseEntryIterator {
public:
  using iterator_concept = std::input_iterator_tag;
  using value_type = EntryView;
  using difference_type = std::ptrdiff_t;

  ReverseEntryIterator() = default;

  ReverseEntryIterator(std::shared_ptr<const EngineState> state,
                       ReverseValueIterator<KeyDirEntry> cur,
                       bool verify_checksums = true)
      : state_{std::move(state)}, cur_{std::move(cur)},
        verify_checksums_{verify_checksums} {}

  auto operator*() const -> const EntryView & {
    if (!has_cached_) {
      auto &dir_entry = *cur_;
      auto &file = *(*state_->files.get(dir_entry.file_id()));
      if (verify_checksums_) {
        raw_cached_ = file.read_entry(dir_entry.file_offset(),
                                      dir_entry.value_size(), io_buf_);
      } else {
        raw_cached_ = file.read_entry_unverified(dir_entry.file_offset(),
                                                 dir_entry.value_size(), io_buf_);
      }
      cached_ = EntryView{.key = raw_cached_.key, .value = raw_cached_.value};
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> ReverseEntryIterator & {
    ++cur_;
    has_cached_ = false;
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  std::shared_ptr<const EngineState> state_;
  ReverseValueIterator<KeyDirEntry> cur_;
  bool verify_checksums_{true};
  mutable DataEntryView raw_cached_;
  mutable EntryView cached_;
  mutable Bytes io_buf_;
  mutable bool has_cached_{false};
};

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

  // State transition: register a sealed read-only file in place of the old
  // active, then register a new writable file as the new active.
  // Cannot fail.
  void apply_rotate_file(std::shared_ptr<DataFile> sealed_old,
                         std::shared_ptr<DataFile> new_file);

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
  [[nodiscard]] auto active_file() -> WritableDataFile &;
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
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;

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

    // Drains background hint tasks then writes all sealed hint files.
    // temporary in public for memoery profile - TODO: Move it back to private:
  void flush_hints();
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

  

  // Vacuum helpers
  // Batch-aware scan: copies live Puts and tombstones from source_file into dest_file.
  static auto vacuum_scan_and_copy(
      const std::shared_ptr<const EngineState> &snap,
      const DataFile &source_file, WritableDataFile &dest_file,
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
  bool use_mmap_{false};
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
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;

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


DbDegraded::~DbDegraded() = default;
DbFollowerMode::~DbFollowerMode() = default;

#pragma region Internal helpers

namespace {

// Generates a unique data file stem.
// Format: "data_{YYYYMMDDHHmmss}_{RRRRRRRR}_V01"
//   - Timestamp: UTC second precision, human-readable creation time (debug hint
//     only — does not reflect content age after compaction).
//   - RRRRRRRR: 4-byte random hex salt for collision avoidance.
//   - V01: file format version.
auto make_data_file_stem() -> std::string {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  static thread_local std::mt19937 rng{std::random_device{}()};
#pragma clang diagnostic pop
  const auto salt = std::uniform_int_distribution<std::uint32_t>{0, 0xFFFF'FFFF}(rng);

  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  ::gmtime_r(&tt, &tm_buf);

  return std::format("data_{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}_{:08x}_V01",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, salt);
}

// Nanoseconds since steady_clock epoch. Timestamps state publications;
// readers compare against this with a single relaxed load (plain MOV on x86).
auto now_ns() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

#pragma endregion

#pragma region TransientEngineState

TransientEngineState::TransientEngineState(
    TransientRadixTree<KeyDirEntry> key_dir,
    TransientU32Map<std::shared_ptr<DataFile>> files,
    TransientU32Map<FileStats> file_stats,
    std::uint32_t active_file_id, std::uint32_t next_file_id,
    std::uint64_t next_seq, std::uint64_t durable_seq,
    Mode mode, bool degraded, std::string degraded_reason)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_file_id_{next_file_id}, next_seq_{next_seq},
      durable_seq_{durable_seq}, mode_{mode}, degraded_{degraded},
      degraded_reason_{std::move(degraded_reason)} {}

auto EngineState::transient() const -> TransientEngineState {
  return TransientEngineState{
      key_dir.transient(), files.transient(), file_stats.transient(),
      active_file_id, next_file_id, next_seq, durable_seq,
      mode, degraded, degraded_reason};
}

auto TransientEngineState::validate_preconditions(const WritePlan &plan) const
    -> bool {
  const auto *snap_state =
      plan.snap_ ? plan.snap_->state_.get() : nullptr;

  // 1. Point guards.
  for (const auto &[key, guard] : plan.guards_) {
    const std::span<const std::byte> key_span{key};
    const auto cur_entry = key_dir_.get(key_span);

    switch (guard.precondition) {
    case WritePlan::Precondition::MustExist:
      if (!cur_entry) return false;
      break;
    case WritePlan::Precondition::MustBeAbsent:
      if (cur_entry) return false;
      break;
    case WritePlan::Precondition::MustBeUnchanged: {
      // ensure_unchanged already enforced snap_ is present at build time.
      const auto snap_entry = snap_state->key_dir.get(key_span);
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
      const std::uint64_t cur_seq = cur_entry ? cur_entry->sequence() : 0;
      if (cur_seq != snap_seq) return false;
      break;
    }
    case WritePlan::Precondition::None:
      break;
    }
  }

  // 2. Range guards (only present when snap_ is set — enforced at build time).
  for (const auto &rg : plan.range_guards_) {
    const std::span<const std::byte> from_span{rg.from};
    const std::span<const std::byte> to_span{rg.to};

    // Check current state for keys modified since snapshot.
    for (auto it = key_dir_.lower_bound(from_span);
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (Key{key_span} >= Key{to_span}) break;
      const auto snap_entry = snap_state->key_dir.get(key_span);
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
      if (entry.sequence() != snap_seq) return false;
    }

    // Check snapshot for keys deleted since snapshot.
    for (auto it = snap_state->key_dir.lower_bound(from_span);
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (Key{key_span} >= Key{to_span}) break;
      if (!key_dir_.get(key_span)) return false;
    }
  }

  // 3. Implicit W-W check on all write keys (only when snapshot present).
  if (snap_state) {
    for (const auto &w : plan.writes_) {
      bool has_conflict = false;
      std::visit(
          [&](const auto &op) -> void {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
              const std::span<const std::byte> key_span{op.key};
              const auto snap_entry = snap_state->key_dir.get(key_span);
              const auto cur_entry = key_dir_.get(key_span);
              const bool appeared = !snap_entry && cur_entry;
              const bool deleted = snap_entry && !cur_entry;
              const bool modified =
                  snap_entry && cur_entry &&
                  cur_entry->sequence() != snap_entry->sequence();
              if (appeared || deleted || modified) has_conflict = true;
            } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
              const std::span<const std::byte> key_span{op.key};
              const auto snap_entry = snap_state->key_dir.get(key_span);
              const auto cur_entry = key_dir_.get(key_span);
              const bool appeared = !snap_entry && cur_entry;
              const bool deleted = snap_entry && !cur_entry;
              const bool modified =
                  snap_entry && cur_entry &&
                  cur_entry->sequence() != snap_entry->sequence();
              if (appeared || deleted || modified) has_conflict = true;
            } else if constexpr (std::is_same_v<T, WritePlan::RangeDel>) {
              // Range conflict check: verify no keys in [from, to) changed since snapshot
              const std::span<const std::byte> from_span{op.from};
              const std::span<const std::byte> to_span{op.to};

              // Check current state for keys modified since snapshot.
              for (auto it = key_dir_.lower_bound(from_span);
                   it != std::default_sentinel && !has_conflict; ++it) {
                auto [key_span, entry] = *it;
                if (Key{key_span} >= Key{to_span}) break;
                const auto snap_entry = snap_state->key_dir.get(key_span);
                const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
                if (entry.sequence() != snap_seq) has_conflict = true;
              }

              // Check snapshot for keys deleted since snapshot.
              for (auto it = snap_state->key_dir.lower_bound(from_span);
                   it != std::default_sentinel && !has_conflict; ++it) {
                auto [key_span, entry] = *it;
                if (Key{key_span} >= Key{to_span}) break;
                if (!key_dir_.get(key_span)) has_conflict = true;
              }
            }
          },
          w);
      if (has_conflict) return false;
    }
  }

  return true;
}

auto TransientEngineState::prepare_write(const WritePlan &plan) const
    -> std::vector<DataEntryView> {
  std::vector<DataEntryView> entries;
  const auto wc = plan.write_count();
  if (wc == 0) return entries;

  const bool multi = wc > 1;
  entries.reserve(multi ? wc + 2 : 1);

  auto seq = next_seq_;

  if (multi) {
    entries.push_back({seq++, EntryType::BulkBegin, {}, {}});
  }

  for (const auto &w : plan.writes_) {
    std::visit(
        [&entries, &seq](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
            entries.push_back(
                {seq++, EntryType::Put,
                 std::span<const std::byte>{op.key},
                 std::span<const std::byte>{op.value}});
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            entries.push_back(
                {seq++, EntryType::Delete,
                 std::span<const std::byte>{op.key}, {}});
          } else {
            entries.push_back(
                {seq++, EntryType::RangeDel,
                 std::span<const std::byte>{op.from},
                 std::span<const std::byte>{op.to}});
          }
        },
        w);
  }

  if (multi) {
    entries.push_back({seq++, EntryType::BulkEnd, {}, {}});
  }

  return entries;
}

void TransientEngineState::apply_writes(
    const WritePlan &plan, std::span<const std::uint64_t> offsets) {
  std::size_t io_idx = 0;
  const auto wc = plan.write_count();
  const bool multi = wc > 1;
  const auto batch_start_seq = next_seq_;

  // Account for BulkBegin marker.
  if (multi) {
    file_stats_.update(active_file_id_, [](FileStats &fs) {
      fs.total_bytes += kHeaderSize + kCrcSize;
    });
    ++next_seq_;
    ++io_idx;
  }

  for (const auto &w : plan.writes_) {
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
            const std::span<const std::byte> key_span{op.key};
            const auto existing = key_dir_.get(key_span);
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              const auto ef = existing->file_id();
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
            }
            const auto val_size = narrow<std::uint32_t>(op.value.size());
            const auto sz = entry_size(key_span.size(), val_size);
            file_stats_.update(active_file_id_, [sz](FileStats &fs) {
              fs.live_bytes += sz;
              fs.total_bytes += sz;
            });
            key_dir_.set(key_span, KeyDirEntry::make(next_seq_, offsets[io_idx],
                                                      active_file_id_, val_size));
            ++next_seq_;
            ++io_idx;
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            const std::span<const std::byte> key_span{op.key};
            const auto existing = key_dir_.get(key_span);
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              const auto ef = existing->file_id();
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
            }
            const auto del_sz = entry_size(key_span.size(), 0);
            file_stats_.update(active_file_id_, [del_sz](FileStats &fs) {
              fs.total_bytes += del_sz;
            });
            key_dir_.erase(key_span);
            ++next_seq_;
            ++io_idx;
          } else {
            // RangeDel: iterate key_dir in [from, to), decrement live_bytes
            // on each affected file, erase from key_dir, then account for
            // the entry itself (total_bytes only — tombstones are not live).
            const std::span<const std::byte> from_span{op.from};
            const std::span<const std::byte> to_span{op.to};

            // Collect keys to erase — cannot erase during iteration.
            std::vector<Key> to_erase;
            for (auto it = key_dir_.lower_bound(from_span);
                 it != std::default_sentinel; ++it) {
              auto [key_span, entry] = *it;
              if (Key{key_span} >= Key{to_span}) break;
              const auto dec =
                  entry_size(key_span.size(), entry.value_size());
              const auto ef = entry.file_id();
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
              to_erase.emplace_back(key_span);
            }
            for (const auto &k : to_erase) {
              key_dir_.erase(std::span<const std::byte>{k});
            }

            const auto rd_sz = entry_size(op.from.size(), op.to.size());
            file_stats_.update(active_file_id_, [rd_sz](FileStats &fs) {
              fs.total_bytes += rd_sz;
            });
            ++next_seq_;
            ++io_idx;
          }
        },
        w);
  }

  // Account for BulkEnd marker.
  if (multi) {
    file_stats_.update(active_file_id_, [](FileStats &fs) {
      fs.total_bytes += kHeaderSize + kCrcSize;
    });
    ++next_seq_;
    ++io_idx;
  }

  // Track per-file sequence bounds.
  const auto batch_end_seq = next_seq_ - 1;
  file_stats_.update(
      active_file_id_,
      [batch_start_seq, batch_end_seq](FileStats &fs) {
        if (fs.min_sequence == 0 || batch_start_seq < fs.min_sequence)
          fs.min_sequence = batch_start_seq;
        if (batch_end_seq > fs.max_sequence)
          fs.max_sequence = batch_end_seq;
      });
}

void TransientEngineState::apply_ingest(
    std::span<const DataEntryView> entries,
    std::span<const std::uint64_t> offsets) {
  assert(entries.size() == offsets.size());
  if (entries.empty()) return;

  const auto batch_start_seq = entries.front().sequence;
  std::uint64_t max_seq = 0;

  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto &e = entries[i];
    const auto offset = offsets[i];
    if (e.sequence > max_seq) max_seq = e.sequence;

    switch (e.entry_type) {
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      file_stats_.update(active_file_id_, [](FileStats &fs) {
        fs.total_bytes += kHeaderSize + kCrcSize;
      });
      break;

    case EntryType::Put: {
      const auto existing = key_dir_.get(e.key);
      if (existing) {
        const auto dec =
            entry_size(e.key.size(), existing->value_size());
        const auto ef = existing->file_id();
        file_stats_.update(
            ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
      }
      const auto val_size = narrow<std::uint32_t>(e.value.size());
      const auto sz = entry_size(e.key.size(), val_size);
      file_stats_.update(active_file_id_, [sz](FileStats &fs) {
        fs.live_bytes += sz;
        fs.total_bytes += sz;
      });
      key_dir_.set(e.key, KeyDirEntry::make(e.sequence, offset,
                                             active_file_id_, val_size));
      break;
    }

    case EntryType::Delete: {
      const auto existing = key_dir_.get(e.key);
      if (existing) {
        const auto dec =
            entry_size(e.key.size(), existing->value_size());
        const auto ef = existing->file_id();
        file_stats_.update(
            ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
      }
      const auto del_sz = entry_size(e.key.size(), 0);
      file_stats_.update(active_file_id_, [del_sz](FileStats &fs) {
        fs.total_bytes += del_sz;
      });
      key_dir_.erase(e.key);
      break;
    }

    case EntryType::RangeDel: {
      // key = from, value = to
      std::vector<Key> to_erase;
      for (auto it = key_dir_.lower_bound(e.key);
           it != std::default_sentinel; ++it) {
        auto [key_span, entry] = *it;
        if (Key{key_span} >= Key{e.value}) break;
        const auto dec =
            entry_size(key_span.size(), entry.value_size());
        const auto ef = entry.file_id();
        file_stats_.update(
            ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
        to_erase.emplace_back(key_span);
      }
      for (const auto &k : to_erase) {
        key_dir_.erase(std::span<const std::byte>{k});
      }
      const auto rd_sz = entry_size(e.key.size(), e.value.size());
      file_stats_.update(active_file_id_, [rd_sz](FileStats &fs) {
        fs.total_bytes += rd_sz;
      });
      break;
    }
    }
  }

  // Advance next_seq past the highest ingested sequence.
  if (max_seq >= next_seq_) next_seq_ = max_seq + 1;

  // Track per-file sequence bounds.
  const auto batch_end_seq = max_seq;
  file_stats_.update(
      active_file_id_,
      [batch_start_seq, batch_end_seq](FileStats &fs) {
        if (fs.min_sequence == 0 || batch_start_seq < fs.min_sequence)
          fs.min_sequence = batch_start_seq;
        if (batch_end_seq > fs.max_sequence)
          fs.max_sequence = batch_end_seq;
      });
}

void TransientEngineState::apply_rotate_file(
    std::shared_ptr<DataFile> sealed_old,
    std::shared_ptr<DataFile> new_file) {
  files_.set(active_file_id_, std::move(sealed_old));
  KeyDirEntry::check_file_id(next_file_id_);
  active_file_id_ = next_file_id_++;
  files_.set(active_file_id_, std::move(new_file));
  file_stats_.set(active_file_id_, FileStats{});
}

void TransientEngineState::apply_vacuum(
    std::uint32_t old_file_id, const VacuumScanResult &scan,
    std::shared_ptr<DataFile> new_sealed_file) {
  const auto dest_file_id =
      new_sealed_file ? next_file_id_++ : active_file_id_;

  auto actual_live_bytes = scan.live_bytes;
  for (const auto &m : scan.mappings) {
    const std::span<const std::byte> key_span{m.key};
    const auto cur = key_dir_.get(key_span);
    if (cur && cur->sequence() == m.sequence) {
      key_dir_.set(key_span,
                   KeyDirEntry::make(m.sequence, m.new_offset, dest_file_id,
                                     m.value_size));
    } else {
      actual_live_bytes -= entry_size(m.key.size(), m.value_size);
    }
  }

  files_.erase(old_file_id);
  if (new_sealed_file) {
    files_.set(dest_file_id, std::move(new_sealed_file));
  }

  file_stats_.erase(old_file_id);
  if (dest_file_id != active_file_id_) {
    file_stats_.set(dest_file_id,
                    FileStats{actual_live_bytes, scan.total_bytes,
                              scan.min_sequence, scan.max_sequence});
  } else {
    file_stats_.update(dest_file_id, [actual_live_bytes,
                                      total = scan.total_bytes,
                                      smin = scan.min_sequence,
                                      smax = scan.max_sequence](FileStats &fs) {
      fs.live_bytes += actual_live_bytes;
      fs.total_bytes += total;
      if (smin > 0 && (fs.min_sequence == 0 || smin < fs.min_sequence))
        fs.min_sequence = smin;
      if (smax > fs.max_sequence) fs.max_sequence = smax;
    });
  }
}

void TransientEngineState::apply_resume(
    std::uint32_t file_id, const std::vector<ResumeEntry> &entries,
    std::uint64_t valid_offset) {
  // Reset file stats for the truncated file — apply_resume owns the
  // full stats lifecycle so callers don't need mutable file_stats access.
  file_stats_.update(file_id, [valid_offset](FileStats &fs) {
    fs.total_bytes = valid_offset;
    fs.min_sequence = 0;
    fs.max_sequence = 0;
  });

  std::uint64_t max_seq = 0;
  std::uint64_t seq_min = 0;
  std::uint64_t seq_max = 0;
  for (const auto &e : entries) {
    const std::span<const std::byte> key_span{e.key};
    if (e.sequence > max_seq) max_seq = e.sequence;
    if (seq_min == 0 || e.sequence < seq_min) seq_min = e.sequence;
    if (e.sequence > seq_max) seq_max = e.sequence;

    if (e.entry_type == EntryType::Put) {
      const auto existing = key_dir_.get(key_span);
      if (!existing || existing->sequence() < e.sequence) {
        if (existing) {
          const auto dec = entry_size(key_span.size(), existing->value_size());
          const auto ef = existing->file_id();
          file_stats_.update(ef,
                             [dec](FileStats &fs) { fs.live_bytes -= dec; });
        }
        const auto inc = entry_size(key_span.size(), e.value_size);
        file_stats_.update(file_id,
                           [inc](FileStats &fs) { fs.live_bytes += inc; });
        key_dir_.set(key_span,
                     KeyDirEntry::make(e.sequence, e.file_offset, file_id,
                                       e.value_size));
      }
    } else if (e.entry_type == EntryType::Delete) {
      const auto existing = key_dir_.get(key_span);
      if (existing && existing->sequence() < e.sequence) {
        const auto dec = entry_size(key_span.size(), existing->value_size());
        const auto ef = existing->file_id();
        file_stats_.update(ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
        key_dir_.erase(key_span);
      }
    }
  }

  if (max_seq >= next_seq_) {
    next_seq_ = max_seq + 1;
  }

  if (seq_max > 0) {
    file_stats_.update(file_id, [seq_min, seq_max](FileStats &fs) {
      fs.min_sequence = seq_min;
      fs.max_sequence = seq_max;
    });
  }
}

auto TransientEngineState::active_file() -> WritableDataFile & {
  return static_cast<WritableDataFile &>(**files_.get(active_file_id_));
}

auto TransientEngineState::active_file_ptr() const -> std::shared_ptr<DataFile> {
  return *files_.get(active_file_id_);
}

auto TransientEngineState::active_file_id() const noexcept -> std::uint32_t {
  return active_file_id_;
}

auto TransientEngineState::is_rotation_needed(std::uint64_t threshold) const
    -> bool {
  return (*files_.get(active_file_id_))->size() >= threshold;
}

auto TransientEngineState::next_seq() const noexcept -> std::uint64_t {
  return next_seq_;
}

void TransientEngineState::apply_sync(std::uint64_t batch_max_seq) {
  if (batch_max_seq > durable_seq_) {
    durable_seq_ = batch_max_seq;
  }
}

auto TransientEngineState::durable_seq() const noexcept -> std::uint64_t {
  return durable_seq_;
}

auto TransientEngineState::persistent() && -> std::shared_ptr<EngineState> {
  auto s = std::make_shared<EngineState>();
  s->key_dir = std::move(key_dir_).persistent();
  s->files = std::move(files_).persistent();
  s->file_stats = std::move(file_stats_).persistent();
  s->active_file_id = active_file_id_;
  s->next_file_id = next_file_id_;
  s->next_seq = next_seq_;
  s->durable_seq = durable_seq_;
  s->mode = mode_;
  s->degraded = degraded_;
  s->degraded_reason = std::move(degraded_reason_);
  return s;
}

#pragma endregion

#pragma region Construction

// Opens dir, runs recovery, creates initial active data file.
// Throws std::system_error if the directory cannot be prepared.
DB::DB(std::filesystem::path dir, Options opts)
    : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
      use_mmap_{opts.use_mmap},
      size_limits_{std::min(opts.max_key_bytes, kMaxKeySize),
                   std::min(opts.max_value_bytes, kMaxValueSize)},
      state_{std::make_shared<EngineState>()} {
  KeyDirEntry::check_file_offset(opts.max_file_bytes);
  std::filesystem::create_directories(dir_);

  // Acquire exclusive advisory lock on the database directory.
  const auto lock_path = dir_ / ".lock";
  lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ == -1) {
    throw std::system_error{
        errno, std::generic_category(),
        std::format("DB: cannot open lock file '{}'", lock_path.string())};
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == -1) {
    auto err = errno;
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw std::system_error{
        err, std::generic_category(),
        std::format("DB: directory '{}' is locked by another process",
                    dir_.string())};
  }

  try {
    EngineState s;
    const auto recovery_start = std::chrono::steady_clock::now();
    s = recovery_load_parallel(std::move(s), opts.recovery_threads,
                               opts.fail_recovery_on_crc_errors);
    const auto recovery_end = std::chrono::steady_clock::now();
    counters_.recovery_duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            recovery_end - recovery_start)
            .count();
    // Count recovered files and keys.
    std::int64_t file_count = 0;
    for (auto it = s.files.begin(); it != std::default_sentinel; ++it)
      ++file_count;
    counters_.recovery_files = file_count;
    counters_.recovery_keys =
        static_cast<std::int64_t>(s.key_dir.size());
    // Count files opened during recovery.
    counters_.files_opened.store(file_count, std::memory_order_relaxed);
    s.active_file_id = s.next_file_id++;
    const auto stem = make_data_file_stem();
    auto new_active = openDataFileForWrite(
        dir_ / (stem + ".data"), rotation_threshold_, use_mmap_);
    // +1 for the new active file.
    counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
    auto files_t = s.files.transient();
    files_t.set(s.active_file_id, new_active);
    s.files = std::move(files_t).persistent();
    auto fstats_t = s.file_stats.transient();
    fstats_t.set(s.active_file_id, FileStats{});
    s.file_stats = std::move(fstats_t).persistent();
    auto initial = std::make_shared<EngineState>(std::move(s));
    // All recovered entries were previously synced.
    initial->durable_seq =
        initial->next_seq > 0 ? initial->next_seq - 1 : 0;
    initial->mode = opts.initial_mode;
    validate_state_consistency(*initial);
    store_initial_state(std::move(initial));
  } catch (...) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw;
  }
}

#pragma endregion

#pragma region Lifecycle

// Seals the active file, drains background hint tasks, writes hint files for
// all sealed files, then purges stale files.
// At destruction no readers are active.
DB::~DB() {
  auto s = load_state_for_write();
  if (!s->files.empty()) {
    try {
      auto t = s->transient();
      t.active_file().sync();
    } catch (...) {}
  }
  try {
    flush_hints();
  } catch (...) {}
  if (lock_fd_ != -1) {
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
}

#pragma endregion

#pragma region Primary operations

// Writes the value for key into out, reusing its existing capacity to
// amortize allocation across calls. Returns true if the key was found,
// false otherwise.
// Routes the read to the correct data file via KeyDirEntry::file_id.
// Throws std::system_error on I/O failure or std::runtime_error on CRC
// mismatch.
auto DB::get(const ReadOptions &opts, BytesView key,
                   Bytes &out) const -> bool {
  auto s = load_state_for_read(opts);
  const auto kv = s->key_dir.get(key);
  if (!kv) {
    return false;
  }
  if (kv->value_size() == 0) {
    out.clear();
    return true;
  }
  // Per-thread I/O scratch buffer — reused across calls to avoid heap churn.
  // Thread-exit destructor is intentional; suppress the Clang diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local Bytes io_buf;
#pragma clang diagnostic pop
  (*s->files.get(kv->file_id()))
      ->read_value(kv->file_offset(), narrow<std::uint16_t>(key.size()),
                   kv->value_size(), opts.verify_checksums, io_buf, out);
  counters_.disk_reads.fetch_add(1, std::memory_order_relaxed);
  counters_.disk_read_bytes.fetch_add(
      static_cast<std::int64_t>(kv->value_size()), std::memory_order_relaxed);
  return true;
}

// Writes key → value. Overwrites any existing value.
// Rotates the active file if it has reached the threshold.
// opts.sync controls whether fdatasync is called after the write.
// Throws std::system_error on I/O failure or lock contention (try_lock).
void DB::put(const WriteOptions &opts, BytesView key, BytesView value) {
  WritePlan plan{size_limits_};
  plan.put(key, value);
  (void)apply_batch(opts, std::move(plan));
}

auto DB::del(const WriteOptions &opts, BytesView key) -> bool {
  WritePlan plan{size_limits_};
  plan.ensure_present(key);
  plan.del(key);
  return apply_batch(opts, std::move(plan));
}

void DB::del_range(const WriteOptions &opts, BytesView from, BytesView to) {
  check_key_size(from.size(), size_limits_.max_key_bytes);
  check_key_size(to.size(), size_limits_.max_key_bytes);
  if (Key{from} >= Key{to}) return;
  WritePlan plan{size_limits_};
  plan.del_range(from, to);
  (void)apply_batch(opts, std::move(plan));
}

auto DB::contains_key(const ReadOptions& opts, BytesView key) const -> bool {
  auto s = load_state_for_read(opts);
  return s->key_dir.contains(key);
}

#pragma endregion

#pragma region Snapshot and apply_batch

auto DB::snapshot() const -> Snapshot {
  ReadOptions opts{};
  return Snapshot{load_state_for_read(opts), size_limits_};
}

// The single write path. Routes to either write_group_ (default) or
// solo_writer_ depending on plan characteristics. put/del/apply_batch are
// thin wrappers that construct a WritePlan and delegate here.
auto DB::apply_batch(WriteOptions opts,
                        WritePlan plan) -> bool {
  if (auto s = load_state(); !s->is_write_allowed()) {
    if (s->degraded) throw DbDegraded{s->degraded_reason};
    throw DbFollowerMode{"write rejected: engine is in follower mode"};
  }
  if (plan.empty()) return true;

  EngineSlot slot;
  slot.plan = std::move(plan);
  slot.opts = opts;
  slot.sync = opts.sync;

  const bool use_solo = opts.solo
      || slot.plan.write_bytes() > kGroupWriteMaxBytes;

  if (use_solo) {
    solo_writer_.submit(slot);
  } else {
    write_group_.submit(slot);
  }

  return slot.result;
}

#pragma endregion

#pragma region Writer executors

// Prepares and applies one slot against the transient. Pure in-memory: no
// I/O. Entries are appended to all_entries; running_offset is advanced by
// the total byte size of entries produced. Returns false on validation
// failure (slot.result set to false).
auto DB::execute_slot(TransientEngineState &t, EngineSlot &slot,
                      std::vector<DataEntryView> &all_entries,
                      std::uint64_t &running_offset) -> bool {
  if (slot.plan.empty()) {
    slot.result = true;
    return true;
  }

  if (!t.validate_preconditions(slot.plan)) {
    slot.result = false;
    return false;
  }

  auto entries = t.prepare_write(slot.plan);
  if (entries.empty()) {
    slot.result = true;
    return true;
  }

  // Pre-compute offsets from running_offset (tracks the file position
  // across all slots in the group, without actual I/O).
  std::vector<std::uint64_t> offsets(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    offsets[i] = running_offset;
    running_offset += entry_size(entries[i].key.size(),
                                 entries[i].value.size());
  }

  t.apply_writes(slot.plan, offsets);

  all_entries.insert(all_entries.end(),
                     std::make_move_iterator(entries.begin()),
                     std::make_move_iterator(entries.end()));

  slot.result = true;
  return true;
}

// Executor callback shared by solo_writer_ and write_group_. Three phases:
// (1) per-slot validate/prepare/apply in-memory, collecting entries;
// (2) one append_entries call for all collected entries;
// (3) sync/rotate/publish once.
void DB::execute_slots(std::vector<Slot *> &batch) {
  std::lock_guard<std::mutex> wg{*write_mu_};

  auto current = load_state_for_write();
  if (!current->is_write_allowed()) {
    auto ex = current->degraded
        ? std::make_exception_ptr(DbDegraded{current->degraded_reason})
        : std::make_exception_ptr(
              DbFollowerMode{"write rejected: engine is in follower mode"});
    for (auto *s : batch) s->err = ex;
    return;
  }

  counters_.group_writer_batches.fetch_add(1, std::memory_order_relaxed);
  counters_.group_writer_coalesced.fetch_add(
      static_cast<std::int64_t>(batch.size()), std::memory_order_relaxed);

  auto t = current->transient();
  auto &file = t.active_file();
  auto initial_offset = static_cast<std::uint64_t>(file.size());
  auto running_offset = initial_offset;
  std::vector<DataEntryView> all_entries;
  auto any_sync = false;

  // Phase 1: pure in-memory — validate, prepare, pre-compute offsets,
  // apply_writes for each slot sequentially.
  for (auto *s : batch) {
    auto &slot = static_cast<EngineSlot &>(*s);
    slot.sync = slot.opts.sync;
    execute_slot(t, slot, all_entries, running_offset);
    any_sync |= slot.opts.sync;
  }

  if (all_entries.empty()) return;

  // Highest sequence in this batch — used to advance durable_seq after sync.
  const auto batch_max_seq = t.next_seq() - 1;

  // Phase 2: one I/O call for all collected entries.
  std::vector<std::uint64_t> io_offsets(all_entries.size());
  try {
    file.append_entries(all_entries, io_offsets);
  } catch (...) {
    auto ex = std::current_exception();
    try { file.sync(); } catch (...) {}
    auto err_t = current->transient();
    err_t.apply_degrade(std::format(
        "append IO error on '{}': call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(std::move(err_t).persistent());
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }

  counters_.bytes_written.fetch_add(
      static_cast<std::int64_t>(running_offset - initial_offset),
      std::memory_order_relaxed);

  // Phase 3: sync/rotate/publish.
  if (t.is_rotation_needed(rotation_threshold_)) {
    try {
      file.sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
      t.apply_sync(batch_max_seq);
    } catch (...) {
      auto ex = std::current_exception();
      auto err_t = current->transient();
      err_t.apply_degrade(std::format(
          "rotation fdatasync failed on '{}': bytes in page cache but "
          "durability not confirmed. Call resume() to recover.",
          file.path().string()));
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      store_state(std::move(err_t).persistent());
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
    try {
      rotate_active_file(t, current);
      counters_.file_rotations.fetch_add(1, std::memory_order_relaxed);
      counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      auto ex = std::current_exception();
      t.apply_degrade(std::format(
          "post-write rotation failed for '{}': active file is sealed "
          "but new file could not be created. Call resume() to recover.",
          file.path().string()));
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      store_state(current, std::move(t).persistent());
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  if (any_sync) {
    try {
      file.sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
      t.apply_sync(batch_max_seq);
    } catch (...) {
      auto ex = std::current_exception();
      auto err_t = current->transient();
      err_t.apply_degrade(std::format(
          "commit fdatasync failed on '{}': bytes in page cache but "
          "durability not confirmed. Call resume() to recover.",
          file.path().string()));
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      store_state(std::move(err_t).persistent());
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  assert(t.active_file().size() <= rotation_threshold_);
  store_state(current, std::move(t).persistent());
}

#pragma endregion

#pragma region Snapshot read methods

auto Snapshot::contains_key(const ReadOptions& /*opts*/,
                            BytesView key) const -> bool {
  return state_->key_dir.contains(key);
}

// Reads the value for key from the frozen snapshot state into out.
// Thread-local I/O buffer reused across calls to amortize allocation.
auto Snapshot::get(const ReadOptions& opts, BytesView key,
                   Bytes &out) const -> bool {
  const auto kv = state_->key_dir.get(key);
  if (!kv) return false;
  if (kv->value_size() == 0) {
    out.clear();
    return true;
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local Bytes io_buf;
#pragma clang diagnostic pop
  (*state_->files.get(kv->file_id()))
      ->read_value(kv->file_offset(), narrow<std::uint16_t>(key.size()),
                   kv->value_size(), opts.verify_checksums, io_buf, out);
  return true;
}

auto Snapshot::iter_from(const ReadOptions& opts, BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto it = from.empty()
      ? state_->key_dir.value_begin()
      : state_->key_dir.value_lower_bound(from);
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{state_, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto Snapshot::keys_from(const ReadOptions& /*opts*/, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto it =
      from.empty() ? state_->key_dir.begin() : state_->key_dir.lower_bound(from);
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto Snapshot::riter_from(const ReadOptions& opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto it = from.empty()
      ? state_->key_dir.value_rbegin()
      : state_->key_dir.value_rlower_bound(from);
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{state_, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto Snapshot::rkeys_from(const ReadOptions& /*opts*/, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto begin_it = from.empty()
      ? state_->key_dir.rbegin().base()
      : state_->key_dir.upper_bound(from);
  auto end_it = state_->key_dir.begin();
  return {ReverseKeyIterator{KeyIterator{std::move(begin_it)}},
          ReverseKeyIterator{KeyIterator{std::move(end_it)}}};
}

#pragma endregion

#pragma region Range iteration

// Returns an input range of (key, value) pairs with keys >= from.
// Pass an empty span to start from the first key. Each dereference reads
// one value from disk via a single pread (lazy). Results are in ascending
// key order.
// Throws std::system_error on I/O failure.
auto DB::iter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto s = load_state_for_read(opts);
  auto it = from.empty()
      ? s->key_dir.value_begin()
      : s->key_dir.value_lower_bound(from);
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{s, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

// Returns an input range of keys >= from. Walks the in-memory key directory
// only; no disk I/O.
auto DB::keys_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto s = load_state_for_read(opts);
  auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto DB::riter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto s = load_state_for_read(opts);
  auto it = from.empty()
      ? s->key_dir.value_rbegin()
      : s->key_dir.value_rlower_bound(from);
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{s, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto DB::rkeys_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto s = load_state_for_read(opts);
  auto begin_it = from.empty()
      ? s->key_dir.rbegin().base()
      : s->key_dir.upper_bound(from);
  auto end_it = s->key_dir.begin();
  return {ReverseKeyIterator{KeyIterator{std::move(begin_it)}},
          ReverseKeyIterator{KeyIterator{std::move(end_it)}}};
}

#pragma endregion

#pragma region Vacuum

// Thread-safe: vacuum_mu_ serialises concurrent vacuum() calls independently
// from write_mu_, so normal put/del/apply_batch calls are not blocked while
// vacuum scans and rewrites data — only the brief commit step acquires
// write_mu_.
auto DB::vacuum(VacuumOptions opts) -> bool {
  std::lock_guard<std::mutex> vg{*vacuum_mu_};
  if (auto s = load_state(); s->degraded) throw DbDegraded{s->degraded_reason};

  // Drain in-flight background hint writes so that vacuum's
  // flush_hints_for call cannot race on the same .hint.tmp file.
  worker_.drain();

  // Snapshot file_stats and active-file info.
  PersistentU32Map<FileStats> stats_snap;
  std::uint32_t active_id{};
  {
    auto s = load_state_for_write();
    stats_snap = s->file_stats;
    active_id = s->active_file_id;
  }

  // Find the highest-fragmentation sealed file above threshold.
  std::uint32_t target_id{};
  double worst_frag = 0.0;
  for (const auto [fid, fs] : stats_snap) {
    if (fid == active_id) continue;
    if (fs.total_bytes == 0) continue;
    const auto frag = 1.0 - static_cast<double>(fs.live_bytes) /
                                static_cast<double>(fs.total_bytes);
    if (frag > worst_frag && frag > opts.fragmentation_threshold) {
      worst_frag = frag;
      target_id = fid;
    }
  }

  if (target_id == 0 && worst_frag == 0.0) return false;

  const auto target_live = stats_snap.get(target_id)->live_bytes;

  // Fast path: file has no live keys — skip scan entirely.
  if (target_live == 0) {
    vacuum_remove_file(target_id);
    return true;
  }

  // All files with live entries are compacted (sealed→sealed).
  vacuum_compact_file(target_id);
  return true;
}

#pragma endregion

#pragma region File rotation

// Opens a read-only mmap-backed file for the old active, dispatches hint
// generation, and opens a new writable active file.
// Caller must sync the active file before calling if durability is required.
void DB::rotate_active_file(TransientEngineState &t,
                            const std::shared_ptr<const EngineState> &) {
  auto read_only_old = openDataFileForRead(t.active_file().path(), use_mmap_);
  const auto stem = make_data_file_stem();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_rotate_file_creation);
#endif
  auto new_file = openDataFileForWrite(
      dir_ / (stem + ".data"), rotation_threshold_, use_mmap_);
  t.apply_rotate_file(read_only_old, std::move(new_file));
  auto dir = dir_;
  worker_.dispatch([f = std::move(read_only_old), d = std::move(dir)] {
    flush_hints_for(f, d);
  });
}

#pragma endregion

#pragma region Hint internals

// Writes the hint file for a single data file using the temp-then-rename
// protocol. Batch-aware: entries between BulkBegin and BulkEnd are buffered
// and written only when BulkEnd is seen; an incomplete batch (crash
// mid-write) is silently discarded. Idempotent: skips files whose .hint
// already exists.
void DB::flush_hints_for(const std::shared_ptr<DataFile> &file,
                               const std::filesystem::path &dir) {
  const auto stem = file->path().stem().string();
  const auto hint_path = dir / (stem + ".hint");
  const auto tmp_path = dir / (stem + ".hint.tmp");

  if (std::filesystem::exists(hint_path)) {
    return;
  }

  auto hint = HintFile::OpenForWrite(tmp_path);

  for (const auto &[entry, entry_off] : scan_committed(*file)) {
    if (entry.entry_type == EntryType::BulkBegin ||
        entry.entry_type == EntryType::BulkEnd) {
      hint.append(entry.sequence, entry.entry_type, entry_off, {}, 0);
      continue;
    }
    if (entry.entry_type == EntryType::RangeDel) {
      hint.append_range_del(entry.sequence, entry_off, entry.key,
                            entry.value);
    } else {
      hint.append(entry.sequence, entry.entry_type, entry_off, entry.key,
                  narrow<std::uint32_t>(entry.value.size()));
    }
  }

  hint.close();
  std::filesystem::rename(tmp_path, hint_path);
}

// Writes hint files for all sealed data files in the given state.
void DB::flush_hints(const EngineState &s) {
  for (const auto [file_id, file] : s.files) {
    if (file_id == s.active_file_id) {
      continue;
    }
    flush_hints_for(file, dir_);
  }
}

// Drains background hint tasks then writes hint files for all sealed files.
void DB::flush_hints() {
  worker_.drain();
  flush_hints(*load_state_for_write());
}

#pragma endregion

#pragma region Vacuum internals

// Scans source_file and copies live entries into dest_file.
// Live Puts (still current in snap->key_dir for source_file_id),
// all tombstones, and BulkBegin/BulkEnd markers are emitted.
// Incomplete batches at EOF are silently discarded by the iterator.
auto DB::vacuum_scan_and_copy(
    const std::shared_ptr<const EngineState> &snap,
    const DataFile &source_file, WritableDataFile &dest_file,
    std::uint32_t source_file_id) -> VacuumScanResult {
  VacuumScanResult result;

  auto track_seq = [&](std::uint64_t seq) {
    if (result.min_sequence == 0 || seq < result.min_sequence)
      result.min_sequence = seq;
    if (seq > result.max_sequence) result.max_sequence = seq;
  };

  auto emit_entry = [&](const DataEntry &entry, Offset entry_off) {
    switch (entry.entry_type) {
    case EntryType::Put: {
      const auto existing = snap->key_dir.get(entry.key);
      if (existing && existing->file_id() == source_file_id &&
          existing->file_offset() == entry_off &&
          existing->sequence() == entry.sequence) {
        const auto new_off =
            dest_file.append_entry(entry.sequence, EntryType::Put, entry.key,
                             entry.value);
        const auto val_size = narrow<std::uint32_t>(entry.value.size());
        const auto sz = entry_size(entry.key.size(), entry.value.size());
        result.live_bytes += sz;
        result.total_bytes += sz;
        track_seq(entry.sequence);
        result.mappings.push_back({std::vector<std::byte>{entry.key.begin(),
                                                          entry.key.end()},
                                   new_off, entry.sequence, val_size});
      }
      break;
    }
    case EntryType::Delete: {
      std::ignore =
          dest_file.append_entry(entry.sequence, EntryType::Delete, entry.key, {});
      result.total_bytes += entry_size(entry.key.size(), 0);
      track_seq(entry.sequence);
      break;
    }
    case EntryType::RangeDel: {
      std::ignore =
          dest_file.append_entry(entry.sequence, EntryType::RangeDel,
                                 entry.key, entry.value);
      result.total_bytes += entry_size(entry.key.size(), entry.value.size());
      track_seq(entry.sequence);
      break;
    }
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      std::ignore =
          dest_file.append_entry(entry.sequence, entry.entry_type, {}, {});
      track_seq(entry.sequence);
      break;
    }
  };

  for (const auto &[entry, entry_off] : scan_committed(source_file)) {
    emit_entry(entry, entry_off);
  }

  return result;
}

// Remaps key_dir entries from old_file_id to the destination file,
// updates the files map and file_stats, and publishes the new
// EngineState. Caller must hold write_mu_.
// If new_sealed_file is non-null (compact), a fresh file-id is
// allocated and the new file is registered. Otherwise (absorb),
// the active file's stats are incremented.
void DB::vacuum_commit(std::uint32_t old_file_id,
                             const VacuumScanResult &scan,
                             std::shared_ptr<DataFile> new_sealed_file) {
  auto current = load_state_for_write();
  auto t = current->transient();
  t.apply_vacuum(old_file_id, scan, std::move(new_sealed_file));

  store_state(current, std::move(t).persistent());
}

// Unlinks the old data and hint files from the filesystem. Existing readers
// continue via their open fds (POSIX: pread succeeds on unlinked files).
void DB::vacuum_unlink_old_file(
    const std::shared_ptr<const EngineState> &snap, std::uint32_t file_id) {
  auto old_data_file = *snap->files.get(file_id);
  auto old_hint_path =
      dir_ / (old_data_file->path().stem().string() + ".hint");
  std::filesystem::remove(old_data_file->path());
  std::filesystem::remove(old_hint_path);
  counters_.vacuum_files_unlinked.fetch_add(1, std::memory_order_relaxed);
}

// Rewrites a sealed file into a new sealed file containing only live
// entries and tombstones. Called under vacuum_mu_, not write_mu_.
// The new data file is written to .data.tmp, then renamed atomically.
// The old file is deferred for cleanup when no readers reference it.
void DB::vacuum_compact_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  const auto &old_file = **snap->files.get(file_id);

  const auto stem = make_data_file_stem();
  const auto tmp_data_path = dir_ / (stem + ".data.tmp");
  const auto final_data_path = dir_ / (stem + ".data");

  VacuumScanResult scan;
  {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_vacuum_compact_tmp_create);
#endif
    auto tmp_file = openDataFileForWrite(
        tmp_data_path, rotation_threshold_, use_mmap_);
    scan = vacuum_scan_and_copy(snap, old_file, *tmp_file, file_id);
    tmp_file->sync();
  }

#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_vacuum_compact_rename);
#endif
  std::filesystem::rename(tmp_data_path, final_data_path);
  auto new_file = openDataFileForRead(final_data_path, use_mmap_);
  flush_hints_for(new_file, dir_);

  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    vacuum_commit(file_id, scan, new_file);
  }
  // Bytes reclaimed = old total - new live (compacted file is smaller).
  auto old_total = snap->file_stats.get(file_id)->total_bytes;
  counters_.vacuum_bytes_reclaimed.fetch_add(
      static_cast<std::int64_t>(old_total - scan.live_bytes),
      std::memory_order_relaxed);
  counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
  vacuum_unlink_old_file(snap, file_id);
}

// Removes a sealed file that has no live keys. No I/O scan needed — just
// commit the state change and unlink the files. Called under vacuum_mu_.
void DB::vacuum_remove_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  auto old_total = snap->file_stats.get(file_id)->total_bytes;
  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    VacuumScanResult empty{};
    vacuum_commit(file_id, empty, nullptr);
  }
  counters_.vacuum_bytes_reclaimed.fetch_add(
      static_cast<std::int64_t>(old_total), std::memory_order_relaxed);
  vacuum_unlink_old_file(snap, file_id);
}

#pragma endregion

#pragma region State access

auto DB::mode() const noexcept -> Mode {
  return load_state()->mode;
}

auto DB::is_degraded() const noexcept -> bool {
  return load_state()->degraded;
}

auto DB::degraded_reason() const noexcept -> std::string {
  return load_state()->degraded_reason;
}

auto DB::stats() const -> std::map<std::string, std::int64_t> {
  auto s = load_state();
  std::int64_t open_files = 0;
  for (auto it = s->files.begin(); it != std::default_sentinel; ++it)
    ++open_files;
  return {
      {"bytecask.bytes_written",
       counters_.bytes_written.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_batches",
       counters_.group_writer_batches.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_coalesced",
       counters_.group_writer_coalesced.load(std::memory_order_relaxed)},
      {"bytecask.file_rotations",
       counters_.file_rotations.load(std::memory_order_relaxed)},
      {"bytecask.fsyncs",
       counters_.fsyncs.load(std::memory_order_relaxed)},
      {"bytecask.disk_reads",
       counters_.disk_reads.load(std::memory_order_relaxed)},
      {"bytecask.disk_read_bytes",
       counters_.disk_read_bytes.load(std::memory_order_relaxed)},
      {"bytecask.vacuum_bytes_reclaimed",
       counters_.vacuum_bytes_reclaimed.load(std::memory_order_relaxed)},
      {"bytecask.vacuum_files_unlinked",
       counters_.vacuum_files_unlinked.load(std::memory_order_relaxed)},
      {"bytecask.recovery_files", counters_.recovery_files},
      {"bytecask.recovery_keys", counters_.recovery_keys},
      {"bytecask.recovery_duration_us", counters_.recovery_duration_us},
      {"bytecask.files_opened",
       counters_.files_opened.load(std::memory_order_relaxed)},
      {"bytecask.crc_failures",
       counters_.crc_failures.load(std::memory_order_relaxed)},
      {"bytecask.io_errors",
       counters_.io_errors.load(std::memory_order_relaxed)},
      {"bytecask.degraded_transitions",
       counters_.degraded_transitions.load(std::memory_order_relaxed)},
      // Gauges — current state, not monotonic.
      {"bytecask.degraded", s->degraded ? 1 : 0},
      {"bytecask.open_files", open_files},
  };
}

void DB::set_mode(Mode mode) {
  std::lock_guard<std::mutex> wg{*write_mu_};
  auto current = load_state_for_write();
  auto t = current->transient();
  t.apply_set_mode(mode);
  store_state(current, std::move(t).persistent());
}

void DB::deem_as_degraded(std::string reason) {
  auto current = load_state_for_write();
  auto t = current->transient();
  t.apply_degrade(std::move(reason));
  store_state(std::move(t).persistent());
}

void DB::resume() {
  if (!is_degraded()) return;

  auto guard = std::unique_lock<std::mutex>{*write_mu_};
  auto current = load_state_for_write();
  if (!current->degraded) return;  // re-check under lock

  auto t = current->transient();
  const auto old_file_id = t.active_file_id();
  auto &file = t.active_file();

  // Scan the active file to find the last valid committed offset
  // and collect valid committed entries for key_dir replay. Entries written to
  // disk but never published to EngineState (sync-failure paths, degraded
  // transitions between IO and state publication) would otherwise be invisible
  // until cold restart.
  Offset valid_offset = 0;
  std::vector<ResumeEntry> committed;
  try {
    auto iter = CommittedEntryIterator{DataFileIterator{file}};
    while (!(iter == std::default_sentinel)) {
      const auto &[entry, entry_off] = *iter;
      if (entry.entry_type != EntryType::BulkBegin &&
          entry.entry_type != EntryType::BulkEnd) {
        committed.push_back({entry.sequence, entry.entry_type, entry_off,
                             narrow<std::uint32_t>(entry.value.size()),
                             entry.key});
      }
      ++iter;
    }
    valid_offset = iter.committed_offset();
  } catch (...) {
    // Stop at first CRC error — valid_offset is the last known-good position.
  }

  // Remove garbage bytes / orphaned batch markers via truncation.
  if (std::filesystem::file_size(file.path()) != valid_offset) {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_resume_truncate);
#endif
    file.truncate(valid_offset);  // throws std::system_error → stays degraded
  }

  // Sync the truncated file (may throw → stays degraded).
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_resume_sync);
#endif
  file.sync();

  // Open the old active as read-only for hint generation.
  auto read_only_old = openDataFileForRead(file.path(), use_mmap_);

  // Dispatch hint generation — idempotent (flush_hints_for skips files
  // whose .hint already exists).
  worker_.dispatch([f = read_only_old, d = dir_] {
    flush_hints_for(f, d);
  });

  // Create the new active file (may throw → stays degraded).
  const auto stem = make_data_file_stem();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_resume_file_creation);
#endif
  auto new_file = openDataFileForWrite(
      dir_ / (stem + ".data"), rotation_threshold_, use_mmap_);

  // Build and publish new state. Replay scanned entries into key_dir so that
  // entries on disk but not yet in EngineState become visible.
  t.apply_resume(old_file_id, committed, valid_offset);
  t.apply_rotate_file(std::move(read_only_old), std::move(new_file));
  // All entries recovered from disk were previously synced.
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
  t.apply_clear_degraded();
  auto resumed = std::move(t).persistent();
  validate_state_consistency(*resumed);
  store_state(current, std::move(resumed));
}

auto DB::current_sequence(std::chrono::milliseconds timeout) const
    -> std::uint64_t {
  auto baseline = load_state()->durable_seq;
  if (timeout <= std::chrono::milliseconds{0}) return baseline;

  std::unique_lock<std::mutex> lk{durable_mu_};
  durable_cv_.wait_for(lk, timeout, [&] {
    return load_state()->durable_seq > baseline;
  });
  return load_state()->durable_seq;
}

auto DB::create_manifest() -> FileManifest {
  std::shared_ptr<const EngineState> manifest_state;
  std::uint64_t through_seq;
  {
    std::lock_guard<std::mutex> wg{*write_mu_};

    auto current = load_state_for_write();
    if (current->degraded) throw DbDegraded{current->degraded_reason};

    auto t = current->transient();

    // Sync active file to make all entries durable.
    auto &file = t.active_file();
    file.sync();
    const auto max_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
    t.apply_sync(max_seq);

    // Seal active file, dispatch hint generation, open new active.
    try {
      rotate_active_file(t, current);
    } catch (...) {
      t.apply_degrade(
          "create_manifest rotation failed: active file is sealed "
          "but new file could not be created. Call resume() to recover.");
      store_state(current, std::move(t).persistent());
      throw;
    }

    through_seq = max_seq;
    store_state(current, std::move(t).persistent());

    // Capture state under write_mu_ — a concurrent write after
    // store_state but before load_state would produce a snapshot
    // with entries beyond through_sequence, breaking the contract.
    manifest_state = load_state_for_write();
  }

  // Wait for all background hint generation to complete.
  worker_.drain();

  // Build manifest from sealed files.
  std::vector<FileInfo> files;
  for (const auto [file_id, file_ptr] : manifest_state->files) {
    if (file_id == manifest_state->active_file_id) continue;
    const auto data_path = file_ptr->path();
    const auto stem = data_path.stem().string();
    files.push_back({file_id, data_path, dir_ / (stem + ".hint")});
  }

  return FileManifest{Snapshot{manifest_state, size_limits_}, std::move(files), through_seq};
}

// Returns the engine state from a thread-local cache (read path only).
// The hot path is a single relaxed load of state_time_ (plain MOV on x86).
// The snapshot is refreshed only when the last write timestamp exceeds
// staleness_tolerance (session mode: tolerance=0, refreshes on every write).
// Returns a reference to the thread-local snapshot. The snapshot stays
// alive until the same thread calls load_state_for_read again, so callers must
// not stash the reference across a second load_state_for_read call.
auto DB::load_state_for_read(const ReadOptions &opts) const
    -> const std::shared_ptr<const EngineState> & {
  struct TlState {
    std::shared_ptr<const EngineState> snapshot;
    std::int64_t last_write_time{0};
  };
  // Per-thread state cache — thread-exit destructor is intentional.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local TlState tl;
#pragma clang diagnostic pop
  const auto wt = state_time_.load(std::memory_order_relaxed);
  const auto tolerance =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          opts.staleness_tolerance)
          .count();
  if (wt - tl.last_write_time > tolerance) {
    tl.snapshot = load_state();
    tl.last_write_time = wt;
  }
  return tl.snapshot;
}

auto DB::load_state_for_write() const -> std::shared_ptr<EngineState> {
  return load_state();
}

void DB::store_state(const std::shared_ptr<const EngineState> &old_state,
                     std::shared_ptr<EngineState> new_state) {
  // O(1) invariant checks — always on, even in release.
  if (new_state->next_seq < old_state->next_seq) {
    deem_as_degraded(std::format(
        "invariant violation: next_seq regressed from {} to {}",
        old_state->next_seq, new_state->next_seq));
    return;
  }
  if (new_state->active_file_id < old_state->active_file_id) {
    deem_as_degraded(std::format(
        "invariant violation: active_file_id regressed from {} to {}",
        old_state->active_file_id, new_state->active_file_id));
    return;
  }
  if (new_state->next_file_id < old_state->next_file_id) {
    deem_as_degraded(std::format(
        "invariant violation: next_file_id regressed from {} to {}",
        old_state->next_file_id, new_state->next_file_id));
    return;
  }
  if (new_state->durable_seq < old_state->durable_seq) {
    deem_as_degraded(std::format(
        "invariant violation: durable_seq regressed from {} to {}",
        old_state->durable_seq, new_state->durable_seq));
    return;
  }

  const auto durable_advanced =
      new_state->durable_seq > old_state->durable_seq;
  const auto became_degraded =
      new_state->degraded && !old_state->degraded;

#ifndef NDEBUG
  // Debug-only O(n) check: next_seq > max(all key_dir sequences).
  std::uint64_t max_seq = 0;
  for (auto it = new_state->key_dir.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (entry.sequence() > max_seq) max_seq = entry.sequence();
  }
  if (max_seq > 0 && new_state->next_seq <= max_seq) {
    deem_as_degraded(std::format(
        "invariant violation: next_seq {} <= max key_dir sequence {}",
        new_state->next_seq, max_seq));
    return;
  }
#endif

  store_state(std::move(new_state));
  state_time_.store(now_ns(), std::memory_order_release);

  if (became_degraded) {
    counters_.degraded_transitions.fetch_add(1, std::memory_order_relaxed);
  }
  if (durable_advanced) {
    { std::lock_guard<std::mutex> lk{durable_mu_}; }
    durable_cv_.notify_all();
  }
}

void DB::store_initial_state(std::shared_ptr<EngineState> s) {
  store_state(std::move(s));
  state_time_.store(now_ns(), std::memory_order_release);
}

void DB::validate_state_consistency(const EngineState &s) const {
  // 1. Active file exists in files registry.
  if (!s.files.contains(s.active_file_id)) {
    throw std::runtime_error{std::format(
        "state consistency: active_file_id {} not in files registry",
        s.active_file_id)};
  }

  // 4. file_stats covers all files.
  for (const auto [file_id, _] : s.files) {
    if (!s.file_stats.contains(file_id)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} missing from file_stats", file_id)};
    }
  }

  // O(n) key_dir walk: verify dangling file refs, live_bytes, and next_seq.
  // Expensive — only enabled in test builds.
#ifdef BYTECASK_TESTING
  std::map<std::uint32_t, std::uint64_t> computed_live;
  std::uint64_t max_seq = 0;
  for (auto it = s.key_dir.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (!s.files.contains(entry.file_id())) {
      throw std::runtime_error{std::format(
          "state consistency: key references file_id {} not in registry",
          entry.file_id())};
    }
    computed_live[entry.file_id()] +=
        entry_size(key_span.size(), entry.value_size());
    if (entry.sequence() > max_seq) max_seq = entry.sequence();
  }

  if (max_seq > 0 && s.next_seq <= max_seq) {
    throw std::runtime_error{std::format(
        "state consistency: next_seq {} <= max key_dir sequence {}",
        s.next_seq, max_seq)};
  }

  for (const auto [file_id, fs] : s.file_stats) {
    auto it = computed_live.find(file_id);
    auto expected_live = (it != computed_live.end()) ? it->second : 0ULL;
    if (fs.live_bytes != expected_live) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} live_bytes={} but key_dir says {}",
          file_id, fs.live_bytes, expected_live)};
    }
  }
#endif

  // 6. min_sequence / max_sequence coherence.
  for (const auto [file_id, fs] : s.file_stats) {
    if ((fs.min_sequence == 0) != (fs.max_sequence == 0)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} has min_sequence={} max_sequence={} "
          "(one is zero, the other is not)",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
    if (fs.min_sequence > 0 && fs.min_sequence > fs.max_sequence) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} min_sequence {} > max_sequence {}",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
  }
}

#pragma endregion

#pragma region Recovery

// Phase 1 shared by serial and parallel recovery: remove stale .hint.tmp
// files, open all data files, seal them, register in s.files, and
// generate missing hint files. Returns the RecoveredFile list.
auto DB::recovery_prepare_files(EngineState &s)
    -> std::vector<RecoveredFile> {
  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    const auto &p = dir_entry.path();
    if (p.extension() == ".tmp" &&
        (p.stem().extension() == ".hint" || p.stem().extension() == ".data")) {
      std::filesystem::remove(p);
    }
  }

  std::vector<RecoveredFile> files;
  auto files_t = s.files.transient();

  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    const auto &p = dir_entry.path();
    if (p.extension() != ".data") {
      continue;
    }

    const auto file_id = s.next_file_id++;
    auto data_file = openDataFileForRead(p, use_mmap_);
    files_t.set(file_id, data_file);

    const auto hint_path = dir_ / (p.stem().string() + ".hint");
    if (!std::filesystem::exists(hint_path)) {
      flush_hints_for(data_file, dir_);
    }

    files.push_back({file_id, std::move(data_file), hint_path,
                     std::filesystem::file_size(p)});
  }

  s.files = std::move(files_t).persistent();
  return files;
}

// Builds a RecoveryResult from a subset of hint files.
// Each worker calls this independently — no shared mutable state.
// When strict is false, corrupt or unreadable hint files are skipped
// with a warning instead of throwing.
auto DB::recovery_build_from_hints(std::span<RecoveredFile> files, bool strict)
    -> RecoveryResult {
  std::uint64_t max_seq = 0;
  auto t = PersistentRadixTree<KeyDirEntry>{}.transient();
  std::map<Key, std::uint64_t> tombstones;
  std::vector<RangeTombstone> range_tombstones;

  // Use a plain hash map for file_stats accumulation — in-place mutation is
  // O(1) per entry vs. the copy-out/write-back overhead of TransientU32Map::update().
  // Converted to PersistentU32Map once at the end.
  std::unordered_map<std::uint32_t, FileStats> fstats_scratch;
  for (const auto &rf : files) {
    fstats_scratch.emplace(rf.file_id, FileStats{0, rf.total_bytes});
  }

  // live_bytes are NOT tracked per-entry here — Phase 4 in
  // recovery_load_parallel recomputes them in a single pass after the
  // final merge, avoiding redundant O(N) map lookups per worker.
  auto seq_wins = [](const KeyDirEntry &existing, const KeyDirEntry &incoming) {
    return kde_newer(incoming, existing);
  };

  for (auto &[file_id, data_file, hint_path, tb] : files) {
    try {
      auto hint = HintFile::OpenForRead(hint_path);
      auto scanner = hint.make_scanner();
      while (auto he = scanner.next()) {
        // Track per-file sequence bounds for ALL entries, including those
        // suppressed by tombstones. Bounds represent the range of sequences
        // physically present in the file, not just live ones.
        if (he->sequence > max_seq) max_seq = he->sequence;
        auto &file_fs = fstats_scratch[file_id];
        if (file_fs.min_sequence == 0 || he->sequence < file_fs.min_sequence)
          file_fs.min_sequence = he->sequence;
        if (he->sequence > file_fs.max_sequence)
          file_fs.max_sequence = he->sequence;

        if (he->entry_type == EntryType::Put) {
          const auto k = Key{he->key};
          const auto tomb_it = tombstones.find(k);
          if (tomb_it != tombstones.end() && tomb_it->second >= he->sequence) {
            continue;
          }
          // Check range tombstones — O(R) per Put, R expected small.
          bool suppressed = false;
          for (const auto &rt : range_tombstones) {
            if (rt.seq >= he->sequence && k >= rt.start && k < rt.end) {
              suppressed = true;
              break;
            }
          }
          if (suppressed) {
            continue;
          }
          t.upsert(he->key,
                   KeyDirEntry::make(he->sequence, he->file_offset, file_id,
                                     he->value_size),
                   seq_wins);
        } else if (he->entry_type == EntryType::Delete) {
          const auto k = Key{he->key};
          auto &tomb_seq = tombstones[k];
          if (he->sequence > tomb_seq) tomb_seq = he->sequence;
          const auto existing = t.get(he->key);
          if (existing && existing->sequence() < he->sequence) {
            t.erase(he->key);
          }
        } else if (he->entry_type == EntryType::RangeDel) {
          const auto start = Key{he->key};
          const auto end = Key{he->end_key};
          range_tombstones.push_back({start, end, he->sequence});
          // Erase keys in [start, end) with sequence < this tombstone.
          std::vector<Key> to_erase;
          for (auto it = t.lower_bound(he->key);
               it != std::default_sentinel; ++it) {
            auto [key_span, entry] = *it;
            if (Key{key_span} >= end) break;
            if (entry.sequence() < he->sequence) {
              to_erase.emplace_back(key_span);
            }
          }
          for (const auto &ek : to_erase) {
            t.erase(std::span<const std::byte>{ek});
          }
        }
      }
    } catch (const std::exception &e) {
      if (strict) throw;
      std::fprintf(stderr,
                   "bytecask: skipping hint file '%s' due to CRC error: %s\n",
                   hint_path.string().c_str(), e.what());
    }
  }

  auto fstats_t = PersistentU32Map<FileStats>{}.transient();
  for (const auto &[id, fs] : fstats_scratch) fstats_t.set(id, fs);

  return {std::move(t).persistent(), std::move(tombstones),
          std::move(range_tombstones), max_seq,
          std::move(fstats_t).persistent()};
}

// Merges two RecoveryResults. Tree merge uses sequence-based conflict
// resolution, then tombstones from both sides are cross-applied to
// suppress stale PUTs. Tombstone maps and file_stats are unioned.
// live_bytes are NOT recomputed here — deferred to a single pass
// after the final merge to avoid O(N × log₂ W) redundant traversals.
auto DB::recovery_merge_results(RecoveryResult a, RecoveryResult b)
-> RecoveryResult {
  auto merged_stats_t = a.file_stats.transient();
  for (const auto [fid, fs] : b.file_stats) {
    merged_stats_t.set(fid, fs);
  }
  a.file_stats = std::move(merged_stats_t).persistent();

  auto seq_resolver = [](const KeyDirEntry &x, const KeyDirEntry &y) {
    return kde_newer(x, y) ? x : y;
  };

  auto merged =
      PersistentRadixTree<KeyDirEntry>::merge(a.key_dir, b.key_dir, seq_resolver);

  for (const auto &[key, tomb_seq] : b.tombstones) {
    std::span<const std::byte> key_span{key.begin(), key.size()};
    const auto entry = merged.get(key_span);
    if (entry && entry->sequence() < tomb_seq) {
      merged = merged.erase(key_span);
    }
  }

  for (const auto &[key, tomb_seq] : a.tombstones) {
    std::span<const std::byte> key_span{key.begin(), key.size()};
    const auto entry = merged.get(key_span);
    if (entry && entry->sequence() < tomb_seq) {
      merged = merged.erase(key_span);
    }
  }

  auto &merged_tombs = a.tombstones;
  for (auto &[key, seq] : b.tombstones) {
    auto &existing = merged_tombs[key];
    if (seq > existing) existing = seq;
  }

  // Cross-apply range tombstones from both sides.
  auto cross_apply_range_tombs =
      [](PersistentRadixTree<KeyDirEntry> &tree,
         const std::vector<RangeTombstone> &rts) {
        for (const auto &rt : rts) {
          std::vector<Key> to_erase;
          for (auto it = tree.lower_bound(
                   std::span<const std::byte>{rt.start.begin(), rt.start.size()});
               it != std::default_sentinel; ++it) {
            auto [key_span, entry] = *it;
            if (Key{key_span} >= rt.end) break;
            if (entry.sequence() < rt.seq) {
              to_erase.emplace_back(key_span);
            }
          }
          for (const auto &ek : to_erase) {
            tree = tree.erase(std::span<const std::byte>{ek});
          }
        }
      };
  cross_apply_range_tombs(merged, b.range_tombstones);
  cross_apply_range_tombs(merged, a.range_tombstones);

  // Union range tombstone vectors.
  auto &merged_range_tombs = a.range_tombstones;
  merged_range_tombs.insert(merged_range_tombs.end(),
                            std::make_move_iterator(b.range_tombstones.begin()),
                            std::make_move_iterator(b.range_tombstones.end()));

  return {std::move(merged), std::move(merged_tombs),
          std::move(merged_range_tombs),
          std::max(a.max_seq, b.max_seq), std::move(a.file_stats)};
}

// Parallel recovery: file-level partitioning with sequential accumulator merge.
// Round-robin assigns files to W workers, each builds a RecoveryResult,
// then results are merged one-at-a-time into an accumulator as workers finish.
auto DB::recovery_load_parallel(EngineState s, unsigned recovery_threads,
                                bool strict) -> EngineState {
  auto files = recovery_prepare_files(s);

  if (files.empty()) {
    return s;
  }

#ifdef BYTECASK_SINGLE_THREADED
  auto W = 1u;
  (void)recovery_threads;
#else
  auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
  if (W == 0) W = 1;
#endif

  // Phase 1: round-robin file assignment.
  std::vector<std::vector<RecoveredFile>> worker_files(W);
  for (unsigned i = 0; i < files.size(); ++i) {
    worker_files[i % W].push_back(std::move(files[i]));
  }

  // Phase 2: parallel build + Phase 3: sequential accumulator merge.
  // Workers push finished results into a queue; the main thread merges
  // each into an accumulator as it arrives. Each ~N/W-key tree is merged
  // once; disjoint subtrees are shared O(1) by the persistent tree, so
  // total merge work is proportional to overlap, not N × log₂(W).
#ifdef BYTECASK_SINGLE_THREADED
  // Single-threaded: run recovery serially on the calling thread.
  std::vector<RecoveryResult> queue;
  {
    RecoveryResult acc{};
    bool acc_initialized = false;
    for (unsigned i = 0; i < W; ++i) {
      auto result = recovery_build_from_hints(worker_files[i], strict);
      if (!acc_initialized) {
        acc = std::move(result);
        acc_initialized = true;
      } else {
        acc = recovery_merge_results(std::move(acc), std::move(result));
      }
    }
    queue.push_back(std::move(acc));
  }
#else
  std::mutex queue_mu;
  std::condition_variable queue_cv;
  std::vector<RecoveryResult> queue;
  std::vector<std::exception_ptr> worker_errors(W, nullptr);
  unsigned finished_count = 0;

  {
    std::vector<std::jthread> threads;
    threads.reserve(W);
    for (unsigned i = 0; i < W; ++i) {
      threads.emplace_back([&, i] {
        try {
          auto result = recovery_build_from_hints(worker_files[i], strict);
          std::unique_lock<std::mutex> lk{queue_mu};
          queue.push_back(std::move(result));
          ++finished_count;
          queue_cv.notify_one();
        } catch (...) {
          std::unique_lock<std::mutex> lk{queue_mu};
          worker_errors[i] = std::current_exception();
          ++finished_count;  // still advances so main thread doesn't deadlock
          queue_cv.notify_one();
        }
      });
    }

    // Main thread: consume results as they arrive.
    RecoveryResult acc{};
    bool acc_initialized = false;
    unsigned merged_count = 0;

    while (merged_count < W) {
      std::unique_lock<std::mutex> lk{queue_mu};
      queue_cv.wait(lk, [&] { return finished_count > merged_count; });
      std::vector<RecoveryResult> local;
      local.swap(queue);
      merged_count = finished_count;  // advance past all finished, including errored
      lk.unlock();

      for (auto &incoming : local) {
        if (!acc_initialized) {
          acc = std::move(incoming);
          acc_initialized = true;
        } else {
          acc = recovery_merge_results(std::move(acc), std::move(incoming));
        }
      }
    }

    // Store final result for phases 4-5 (threads join at scope exit).
    queue.clear();
    queue.push_back(std::move(acc));
  }

  // Threads are joined. Propagate any worker exceptions now.
  for (const auto &err : worker_errors) {
    if (err) {
      if (strict) std::rethrow_exception(err);
      // lenient: warning already emitted inside recovery_build_from_hints
    }
  }
#endif

  auto &final_result = queue[0];

  // Phase 4: recompute live_bytes once from the fully-merged tree.
  // Accumulate into a hash map (O(1) in-place), then apply to PersistentU32Map
  // in a single pass over the (small) file set — avoids O(N) radix tree
  // mutations for N key_dir entries.
  std::unordered_map<std::uint32_t, std::uint64_t> live_accum;
  for (auto it = final_result.key_dir.begin(); it != std::default_sentinel;
       ++it) {
    const auto &[key_span, kde] = *it;
    live_accum[kde.file_id()] += entry_size(key_span.size(), kde.value_size());
  }
  auto fstats_t = final_result.file_stats.transient();
  for (const auto [fid, _] : final_result.file_stats) {
    const auto acc_it = live_accum.find(fid);
    const auto live = (acc_it != live_accum.end()) ? acc_it->second : 0ULL;
    fstats_t.update(fid, [live](FileStats &fs) { fs.live_bytes = live; });
  }
  final_result.file_stats = std::move(fstats_t).persistent();

  // Phase 5: assembly.
  s.key_dir = std::move(final_result.key_dir);
  s.next_seq = final_result.max_seq + 1;
  s.file_stats = std::move(final_result.file_stats);
  return s;
}

#pragma endregion

#pragma region Change Since


// ---------------------------------------------------------------------------
// ChangeIterator implementation
// ---------------------------------------------------------------------------
// Lazy ChangeIterator: walks files in min_sequence order, scanning one
// batch at a time. Only the current batch's entries are held in memory.
// Files in a single-writer engine have disjoint, monotonically increasing
// sequence ranges, so iterating files by min_sequence and scanning forward
// within each file yields globally ascending sequence order.
class ChangeIterator::Impl {
public:
  Impl(std::shared_ptr<const EngineState> state,
       std::uint64_t from_sequence,
       std::uint64_t durable_sequence)
    : state_(std::move(state)), from_sequence_(from_sequence),
      durable_sequence_(durable_sequence) {

    // Build sorted file list — O(num_files), typically tiny.
    for (const auto [file_id, stats] : state_->file_stats) {
      if (stats.max_sequence > from_sequence && stats.min_sequence <= durable_sequence) {
        file_queue_.emplace_back(stats.min_sequence, file_id);
      }
    }
    std::sort(file_queue_.begin(), file_queue_.end());

    // Position at first valid entry.
    advance_to_next_valid();
  }

  auto has_more() const -> bool { return has_entry_; }

  auto current() const -> const DataEntryView& { return cached_entry_; }

  void advance() {
    advance_to_next_valid();
  }

private:
  auto should_include(std::uint64_t seq) const -> bool {
    return seq > from_sequence_ && seq <= durable_sequence_;
  }

  // Scans forward across entries and files until a valid entry is found
  // or all files are exhausted. The cached BytesView spans point into
  // the iterator's current entry, so we must NOT advance the iterator
  // after caching — that would invalidate the view. Instead, we set
  // needs_advance_ and increment on the next call.
  void advance_to_next_valid() {
    has_entry_ = false;

    // Advance past the previously cached entry (deferred from last call).
    if (needs_advance_ && entry_iter_) {
      ++(*entry_iter_);
      needs_advance_ = false;
    }

    while (true) {
      // Try next entry in the current file.
      if (entry_iter_ && !(*entry_iter_ == std::default_sentinel)) {
        const auto& [entry, entry_off] = **entry_iter_;
        if (should_include(entry.sequence)) {
          cache_entry(entry);
          needs_advance_ = true;
          return;
        }
        ++(*entry_iter_);
        continue;
      }

      // Try next file.
      if (file_idx_ < file_queue_.size()) {
        auto file_id = file_queue_[file_idx_].second;
        ++file_idx_;
        auto file_ptr = state_->files.get(file_id);
        if (file_ptr) {
          entry_iter_.emplace(DataFileIterator{**file_ptr});
        }
        continue;
      }

      // All files exhausted.
      return;
    }
  }

  // Caches the current entry. The BytesView spans point into
  // the iterator's internal buffer which stays alive until advance().
  void cache_entry(const DataEntry& entry) {
    cached_entry_ = {
      .sequence = entry.sequence,
      .entry_type = entry.entry_type,
      .key = BytesView{entry.key},
      .value = BytesView{entry.value}
    };
    has_entry_ = true;
  }

  std::shared_ptr<const EngineState> state_;
  std::uint64_t from_sequence_;
  std::uint64_t durable_sequence_;

  // File traversal — sorted by min_sequence.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> file_queue_;
  std::size_t file_idx_{0};

  // Current file's committed entry scanner.
  std::optional<CommittedEntryIterator> entry_iter_;
  bool needs_advance_{false};

  // Cached current entry — BytesView into entry_iter_'s internal buffer.
  bool has_entry_{false};
  DataEntryView cached_entry_;
};

// ChangeIterator methods
ChangeIterator::ChangeIterator(std::shared_ptr<const EngineState> state,
                               std::uint64_t from_sequence,
                               std::uint64_t durable_sequence)
  : impl_(std::make_unique<Impl>(std::move(state), from_sequence, durable_sequence)) {}

ChangeIterator::~ChangeIterator() = default;

ChangeIterator::ChangeIterator(ChangeIterator&&) noexcept = default;
ChangeIterator& ChangeIterator::operator=(ChangeIterator&&) noexcept = default;

auto ChangeIterator::operator++() -> ChangeIterator& {
  if (impl_) {
    impl_->advance();
  }
  return *this;
}

void ChangeIterator::operator++(int) {
  ++(*this);
}

auto ChangeIterator::operator*() const -> const value_type& {
  return impl_->current();
}

auto ChangeIterator::operator==(std::default_sentinel_t) const noexcept -> bool {
  return !impl_ || !impl_->has_more();
}

// DB::changes_since implementation
auto DB::changes_since(const Snapshot& snap, std::uint64_t from_sequence) const
    -> std::ranges::subrange<ChangeIterator, std::default_sentinel_t> {

  auto state = snap.state();
  auto begin = ChangeIterator{state, from_sequence, state->durable_seq};
  return {std::move(begin), std::default_sentinel};
}

#pragma endregion

#pragma region Ingest (follower replication)

void DB::ingest(std::span<const DataEntryView> entries) {
  if (auto s = load_state(); !s->is_ingestion_allowed()) {
    if (s->degraded) throw DbDegraded{s->degraded_reason};
    throw std::logic_error{"ingest rejected: engine is not in follower mode"};
  }
  if (entries.empty()) return;

  for (const auto &e : entries) {
    check_key_size(e.key.size(), size_limits_.max_key_bytes);
    if (e.entry_type == EntryType::Put) {
      check_value_size(e.value.size(), size_limits_.max_value_bytes);
    }
  }

  std::lock_guard<std::mutex> wg{*write_mu_};

  auto current = load_state_for_write();
  if (!current->is_ingestion_allowed()) {
    if (current->degraded) throw DbDegraded{current->degraded_reason};
    throw std::logic_error{"ingest rejected: engine is not in follower mode"};
  }

  auto t = current->transient();

  // Filter: skip already-ingested entries (idempotency).
  auto remaining = entries;
  while (!remaining.empty() &&
         remaining.front().sequence <= current->durable_seq) {
    remaining = remaining.subspan(1);
  }
  if (remaining.empty()) return;

  // Chunk-and-rotate loop: write entries in chunks, rotating between chunks
  // at safe boundaries (never inside BulkBegin..BulkEnd).
  while (!remaining.empty()) {
    auto &file = t.active_file();

    // Find chunk end: largest prefix that keeps batches intact.
    std::size_t chunk_end = remaining.size();
    bool needs_rotation = false;
    bool in_batch = false;
    auto running_bytes = static_cast<std::uint64_t>(file.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) {
      running_bytes += entry_size(remaining[i].key.size(),
                                  remaining[i].value.size());
      if (remaining[i].entry_type == EntryType::BulkBegin) in_batch = true;
      else if (remaining[i].entry_type == EntryType::BulkEnd) in_batch = false;

      if (!in_batch && running_bytes >= rotation_threshold_ &&
          i + 1 < remaining.size()) {
        chunk_end = i + 1;
        needs_rotation = true;
        break;
      }
    }

    auto chunk = remaining.subspan(0, chunk_end);

    // Phase 1: compute offsets, apply in-memory state.
    auto file_offset = static_cast<std::uint64_t>(file.size());
    std::vector<std::uint64_t> offsets(chunk.size());
    for (std::size_t i = 0; i < chunk.size(); ++i) {
      offsets[i] = file_offset;
      file_offset += entry_size(chunk[i].key.size(), chunk[i].value.size());
    }

    t.apply_ingest(chunk, offsets);
    auto chunk_max_seq = chunk.back().sequence;

    // Phase 2: writev chunk to active file.
    std::vector<std::uint64_t> io_offsets(chunk.size());
    try {
      file.append_entries(chunk, io_offsets);
    } catch (...) {
      auto ex = std::current_exception();
      try { file.sync(); } catch (...) {}
      auto err_t = current->transient();
      err_t.apply_degrade(
          "ingest append IO error: call resume() to recover.");
      store_state(std::move(err_t).persistent());
      std::rethrow_exception(ex);
    }

    // Phase 3: if rotation needed, sync before sealing.
    if (needs_rotation) {
      try {
        file.sync();
        t.apply_sync(chunk_max_seq);
      } catch (...) {
        auto err_t = current->transient();
        err_t.apply_degrade(
            "ingest rotation fdatasync failed: call resume() to recover.");
        store_state(std::move(err_t).persistent());
        throw;
      }
      try {
        rotate_active_file(t, current);
      } catch (...) {
        t.apply_degrade(
            "ingest post-rotation file creation failed: call resume().");
        store_state(current, std::move(t).persistent());
        throw;
      }
    }

    remaining = remaining.subspan(chunk_end);
  }

  // Post-loop rotation: if the last chunk pushed the active file past the
  // threshold, rotate now so the invariant (active file <= threshold) holds.
  if (t.is_rotation_needed(rotation_threshold_)) {
    try {
      t.active_file().sync();
      t.apply_sync(t.next_seq() - 1);
    } catch (...) {
      auto err_t = current->transient();
      err_t.apply_degrade(
          "ingest rotation fdatasync failed: call resume() to recover.");
      store_state(std::move(err_t).persistent());
      throw;
    }
    try {
      rotate_active_file(t, current);
    } catch (...) {
      t.apply_degrade(
          "ingest post-rotation file creation failed: call resume().");
      store_state(current, std::move(t).persistent());
      throw;
    }
  }

  // Final sync: ensure last chunk is durable before publishing.
  try {
    t.active_file().sync();
    t.apply_sync(t.next_seq() - 1);
  } catch (...) {
    auto err_t = current->transient();
    err_t.apply_degrade(
        "ingest fdatasync failed: call resume() to recover.");
    store_state(std::move(err_t).persistent());
    throw;
  }

  assert(t.active_file().size() <= rotation_threshold_);
  store_state(current, std::move(t).persistent());
}

#pragma endregion


} // namespace bytecask
