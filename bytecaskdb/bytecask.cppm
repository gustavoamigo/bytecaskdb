// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — C++23 module interface: public API surface for the storage engine

module;
#include <atomic>
#include <chrono>
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
#include <utility>
#include <variant>
#include <vector>

export module bytecask;

export import :internals;
import bytecask.concurrency;
import bytecask.data_file;
import bytecask.radix_tree;
import bytecask.types;
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
  // When true (default): any CRC error during recovery causes DB::open to
  // throw std::runtime_error. When false: corrupt entries and hint files are
  // skipped; the DB opens with whatever was successfully recovered, and a
  // warning is printed to stderr for each skipped item.
  bool fail_recovery_on_crc_errors{true};
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
      (*state_->files.get(dir_entry.file_id))
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
      -> std::vector<AppendEntry>;

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
  // next_seq past the highest sequence in the entries. Cannot fail.
  void apply_resume(std::uint32_t file_id,
                    const std::vector<ResumeEntry> &entries);

  // Pure queries the coordinator needs for IO decisions.
  [[nodiscard]] auto active_file() -> DataFile &;
  [[nodiscard]] auto active_file_id() const noexcept -> std::uint32_t;
  [[nodiscard]] auto is_rotation_needed(std::uint64_t threshold) const -> bool;

  // Returns the current next_seq value — used to capture the post-write sequence
  // before consuming the transient on sync failure (F/G).
  [[nodiscard]] auto next_seq() const noexcept -> std::uint64_t;

  // Advances next_seq_ to new_seq without applying any key-dir or file-stats
  // changes. Used on F/G sync failure to prevent sequence reuse for bytes already
  // in the page cache, while keeping key changes unpublished.
  void advance_next_seq(std::uint64_t new_seq) noexcept;

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
                       std::uint64_t next_seq);

  TransientRadixTree<KeyDirEntry> key_dir_;
  TransientU32Map<std::shared_ptr<DataFile>> files_;
  TransientU32Map<FileStats> file_stats_;
  std::uint32_t active_file_id_;
  std::uint32_t next_file_id_;
  std::uint64_t next_seq_;
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

// Default group-write byte-size threshold: plans above this size are routed
// to the solo writer (a large batch would monopolize the group).
export inline constexpr std::uint64_t kGroupWriteMaxBytes = 256ULL * 1024;

// Forward declaration — defined after WritePlan.
export struct EngineSlot;

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
  [[nodiscard]] auto contains_key(BytesView key) const -> bool;

  // Returns true if the engine has entered a degraded state. A degraded DB
  // refuses all writes but reads remain available. Call resume() to attempt
  // in-process recovery.
  [[nodiscard]] auto is_degraded() const noexcept -> bool {
    return degraded_.load(std::memory_order_acquire);
  }

  // Returns the reason the engine entered degraded state. Safe to call after
  // is_degraded() returns true — the acquire load synchronises access.
  [[nodiscard]] auto degraded_reason() const noexcept -> const std::string & {
    return degraded_reason_;
  }

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

private:
  explicit DB(std::filesystem::path dir, Options opts);

  // File rotation
  // Seals active file, dispatches hint write to background, opens new active file.
  void rotate_active_file(TransientEngineState &t,
                          const std::shared_ptr<const EngineState> &current);

  // Publishes a sequence-only state (key_dir unchanged) advanced to target_seq.
  // Called on any IO failure where bytes may have reached disk.
  void publish_seq_advance(const std::shared_ptr<const EngineState> &current,
                            std::uint64_t target_seq);

  // Degrade — sets the engine to write-blocked state with a reason.
  void deem_as_degraded(std::string reason);

  // Hint file management
  // Writes hint file via temp-then-rename. Batch-aware; idempotent if .hint exists.
  // strict=true (default): throws on data-file CRC error. strict=false: stops
  // at the first corrupt entry and writes a partial hint for the valid prefix.
  static void flush_hints_for(const std::shared_ptr<DataFile> &file,
                               const std::filesystem::path &dir,
                               bool strict = true);
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
  void vacuum_absorb_file(std::uint32_t file_id);

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
                    std::vector<AppendEntry> &all_entries,
                    std::uint64_t &running_offset) -> bool;
  // Executor callback shared by solo_writer_ and write_group_. Three phases:
  // (1) per-slot validate/prepare/apply in-memory, (2) one append_entries,
  // (3) sync/rotate/publish.
  void execute_slots(std::vector<Slot *> &batch);

  // Recovery
  // Phase 1: opens all data files, seals them, generates missing hint files.
  auto recovery_prepare_files(EngineState &s, bool strict)
      -> std::vector<RecoveredFile>;
  // Builds a RecoveryResult from a set of hint files; no shared mutable state.
  static auto recovery_build_from_hints(std::span<RecoveredFile> files,
                                        bool strict) -> RecoveryResult;
  // Merges two RecoveryResults with sequence-based conflict resolution.
  static auto recovery_merge_results(RecoveryResult a, RecoveryResult b)
      -> RecoveryResult;
  // Reconstructs key_dir from hint files using a single thread.
  auto recovery_load_serial(EngineState s, bool strict) -> EngineState;
  // Reconstructs key_dir using file-level fan-in parallelism.
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
  // Set by deem_as_degraded() on write-path failure. Blocks all writes;
  // reads remain available. Cleared by resume() on successful recovery.
  std::atomic<bool> degraded_{false};
  // Written before degraded_.store(release); read after
  // degraded_.load(acquire) — the atomic bool synchronises access.
  std::string degraded_reason_;
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

  // Returns a range of (key, value) pairs in descending key order.
  // When from is non-empty, starts at the last key <= from.
  // When from is empty, starts at the last key in the DB.
  [[nodiscard]] auto riter_from(BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator>;

  // Returns a range of keys in descending order. Pure in-memory — no disk I/O.
  [[nodiscard]] auto rkeys_from(BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

private:
  explicit Snapshot(std::shared_ptr<const EngineState> state)
      : state_{std::move(state)} {}
  std::shared_ptr<const EngineState> state_;
  friend class DB;
  friend class TransientEngineState;
  friend class WritePlan;
#ifdef BYTECASK_TESTING
public:
  static auto from_state(std::shared_ptr<const EngineState> s) -> Snapshot {
    return Snapshot{std::move(s)};
  }
#endif
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
  explicit WritePlan(Snapshot snap) : snap_{std::move(snap)} {}
  WritePlan(const WritePlan &) = delete;
  WritePlan &operator=(const WritePlan &) = delete;
  WritePlan(WritePlan &&) noexcept = default;
  WritePlan &operator=(WritePlan &&) noexcept = default;

  // --- Writes (unconditional) ---

  void put(BytesView key, BytesView value) {
    writes_.emplace_back(
        PointPut{Bytes{key.begin(), key.end()},
                 Bytes{value.begin(), value.end()}});
  }

  void del(BytesView key) {
    writes_.emplace_back(PointDel{Bytes{key.begin(), key.end()}});
  }

  // --- Range writes ---

  void del_range(BytesView from, BytesView to) {
    writes_.emplace_back(
        RangeDel{Bytes{from.begin(), from.end()},
                 Bytes{to.begin(), to.end()}});
  }

  // --- Point guards ---

  void ensure_present(BytesView key) {
    set_precondition(key, Precondition::MustExist);
  }

  void ensure_absent(BytesView key) {
    set_precondition(key, Precondition::MustBeAbsent);
  }

  // Requires a snapshot — throws std::logic_error if constructed without one.
  void ensure_unchanged(BytesView key) {
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
