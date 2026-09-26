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
#include <cstring>
#include <cstdlib>
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
import bytecask.blind_btree;
import bytecask.btree;
import bytecask.concurrency;
export import bytecask.buffer_pool;
export import bytecask.counters;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_entry;
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
  // Minimum fragmentation ratio a sealed file must exceed to be eligible for
  // vacuum: 1 − (live + tombstone bytes) / total bytes, the share compaction
  // can reclaim. Range [0.0, 1.0].
  double fragmentation_threshold{0.5};
};


// Default active-file size threshold: 64 MiB. Active files are zero-filled
// in chunks up to this size (see WritableFileOps::ensure_zeroed), so the
// test build uses 64 KiB: the suite opens hundreds of databases, and a
// multi-MiB fill for each is disk writes for no extra coverage.
#ifdef BYTECASK_TESTING
export inline constexpr std::uint64_t kDefaultRotationThreshold =
    64ULL * 1024;
#else
export inline constexpr std::uint64_t kDefaultRotationThreshold =
    64ULL * 1024 * 1024;
#endif

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

// Outcome of a committed write. sequence is the highest sequence assigned to
// the write (for a multi-op batch this is the BulkEnd marker's sequence). A
// reader — local or follower — whose durable_sequence() >= sequence is
// guaranteed to see every entry of this write.
export struct CommitResult {
  // Highest sequence assigned to this write. 0 means nothing was written
  // (empty plan, guard-only plan, or empty-range del_range) — there is
  // nothing to wait for.
  std::uint64_t sequence{0};

  // True if fdatasync confirmed durability of this write before return.
  // Always true for sync=true writes. May be true for sync=false writes
  // that were coalesced into a group containing a sync writer, or that
  // triggered a rotation sync.
  bool durable{false};
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
  // Selects how data files are read. Pread (default) issues pread(2) per read
  // and avoids virtual address space pressure under memory contention; Mmap
  // memory-maps sealed files for zero-copy reads; BufferPool serves sealed
  // files from a bounded, engine-owned cache. See IoBackend.
  IoBackend io_backend{IoBackend::Pread};
  // Only read when io_backend == IoBackend::BufferPool.
  BufferPoolOptions buffer_pool{};
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

  explicit KeyIterator(KeyDirIter cur)
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

  KeyDirIter cur_;
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

  // Move-only: operator* caches spans into io_buf_, and a copy would carry
  // those spans while deep-copying the buffer they address — the copy would
  // point into the source's storage. Moving is safe (the buffer moves with
  // the spans). Same reasoning as ChangeIterator.
  EntryIterator(const EntryIterator &) = delete;
  auto operator=(const EntryIterator &) -> EntryIterator & = delete;
  EntryIterator(EntryIterator &&) noexcept = default;
  auto operator=(EntryIterator &&) noexcept -> EntryIterator & = default;

  EntryIterator(std::shared_ptr<const EngineState> state,
                KeyDirValueIter cur,
                bool verify_checksums = true)
      : state_{std::move(state)}, cur_{std::move(cur)},
        verify_checksums_{verify_checksums} {}

  auto operator*() const -> const EntryView & {
    if (!has_cached_) {
      const auto &dir_entry = *cur_;
      auto &file = *(*state_->files.get(dir_entry.file_id()));
      raw_cached_ = file.lend_record(dir_entry.file_offset(),
                                     value_size_hint(dir_entry),
                                     verify_checksums_, io_buf_, lease_);
      cached_ = EntryView{.key = raw_cached_.key, .value = raw_cached_.value};
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> EntryIterator & {
    ++cur_;
    has_cached_ = false;
    lease_.reset();  // the spans are dead now; let the frame go
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  std::shared_ptr<const EngineState> state_;
  KeyDirValueIter cur_;
  bool verify_checksums_{true};
  mutable DataEntryView raw_cached_;
  mutable EntryView cached_;
  mutable Bytes io_buf_;
  // Backs the spans when the file lends them out of a pool frame; empty
  // when they point into io_buf_. Declared after io_buf_ so it is released
  // first: nothing may still reference the frame when the pin drops.
  mutable FrameLease lease_;
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

  // Move-only for the same reason as EntryIterator — see there.
  ReverseEntryIterator(const ReverseEntryIterator &) = delete;
  auto operator=(const ReverseEntryIterator &)
      -> ReverseEntryIterator & = delete;
  ReverseEntryIterator(ReverseEntryIterator &&) noexcept = default;
  auto operator=(ReverseEntryIterator &&) noexcept
      -> ReverseEntryIterator & = default;

  ReverseEntryIterator(std::shared_ptr<const EngineState> state,
                       KeyDirReverseValueIter cur,
                       bool verify_checksums = true)
      : state_{std::move(state)}, cur_{std::move(cur)},
        verify_checksums_{verify_checksums} {}

  auto operator*() const -> const EntryView & {
    if (!has_cached_) {
      const auto &dir_entry = *cur_;
      auto &file = *(*state_->files.get(dir_entry.file_id()));
      raw_cached_ = file.lend_record(dir_entry.file_offset(),
                                     value_size_hint(dir_entry),
                                     verify_checksums_, io_buf_, lease_);
      cached_ = EntryView{.key = raw_cached_.key, .value = raw_cached_.value};
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> ReverseEntryIterator & {
    ++cur_;
    has_cached_ = false;
    lease_.reset();  // the spans are dead now; let the frame go
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  std::shared_ptr<const EngineState> state_;
  KeyDirReverseValueIter cur_;
  bool verify_checksums_{true};
  mutable DataEntryView raw_cached_;
  mutable EntryView cached_;
  mutable Bytes io_buf_;
  // Backs the spans when the file lends them out of a pool frame; empty
  // when they point into io_buf_. Declared after io_buf_ so it is released
  // first: nothing may still reference the frame when the pin drops.
  mutable FrameLease lease_;
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
// the key directory tree: mutations are batched on a
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
      -> bool {
    std::uint64_t ignored = 0;
    return validate_preconditions(plan, ignored);
  }
  // On failure, lost_to is the highest sequence among the entries the plan
  // lost to — what a retry has to be able to see — or, for a key the plan
  // found deleted or absent, the head's latest sequence.
  [[nodiscard]] auto validate_preconditions(const WritePlan &plan,
                                            std::uint64_t &lost_to) const
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
  // new_file_id must come from reserve_file_id(): the new active file is
  // created before this runs, and under the buffer pool it carries its id
  // from construction so the writer can key its frames.
  void apply_rotate_file(std::shared_ptr<DataFile> sealed_old,
                         std::shared_ptr<DataFile> new_file,
                         std::uint32_t new_file_id);

  // State transition: remap keys after vacuum scan+copy.
  // Cannot fail.
  // Mints the next file id without registering a file under it. Vacuum needs
  // the id before it opens the compacted file, because the buffer pool keys
  // frames by file_id and the file must carry its id from construction.
  // Reserving under the write barrier is what keeps it disjoint from the id
  // rotation mints on the same counter. A reserved id that is never used
  // simply leaves a gap, which is harmless: ids are monotonic within a
  // process and are reassigned from scratch at recovery.
  [[nodiscard]] auto reserve_file_id() -> std::uint32_t;

  // dest_file_id must have been reserved by reserve_file_id(); it is ignored
  // when new_sealed_file is null, since the live entries then move into the
  // active file.
  void apply_vacuum(std::uint32_t old_file_id, const VacuumScanResult &scan,
                    std::shared_ptr<DataFile> new_sealed_file,
                    std::uint32_t dest_file_id);

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

  // State transition: record that a sync=true slot wrote up to seq. The
  // state cannot be published until durable_seq >= sync_requested_seq.
  // Monotonic. Cannot fail.
  void note_sync_requested(std::uint64_t seq);

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
  // Where key_dir_ reads the keys it does not store: this transient's files
  // and the records it has not written yet.
  [[nodiscard]] auto kd_ctx() const -> KeyDirCtx {
    return {nullptr, &files_, &pending_, true};
  }
  // Records a put this transient placed at `offset` of the active file,
  // before the batch is written. A no-op for key directories that store
  // their keys.
  void note_pending(std::uint64_t offset, std::uint64_t sequence,
                    std::span<const std::byte> key, std::uint32_t value_size) {
    if constexpr (kKeyDirReadsKeys)
      pending_.insert_or_assign(
          pending_slot(active_file_id_, offset),
          PendingRecord{sequence, value_size, {key.begin(), key.end()}});
  }

  friend class DB;
  friend struct EngineState;
  TransientEngineState(KeyDirTransient key_dir,
                       TransientU32Map<std::shared_ptr<DataFile>> files,
                       TransientU32Map<FileStats> file_stats,
                       std::uint32_t active_file_id,
                       std::uint32_t next_file_id,
                       std::uint64_t next_seq,
                       std::uint64_t durable_seq,
                       std::uint64_t sync_requested_seq,
                       Mode mode,
                       bool degraded,
                       std::string degraded_reason);

  KeyDirTransient key_dir_;
  TransientU32Map<std::shared_ptr<DataFile>> files_;
  // Records this transient placed in the active file before they are
  // written, for a key directory that reads keys back (kKeyDirReadsKeys).
  PendingRecords pending_;
  TransientU32Map<FileStats> file_stats_;
  std::uint32_t active_file_id_;
  std::uint32_t next_file_id_;
  std::uint64_t next_seq_;
  std::uint64_t durable_seq_;
  std::uint64_t sync_requested_seq_;
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
// Per-thread read cache.
//
// A read caches the engine state it saw in a slot owned by its thread, so a
// get costs no reference-count traffic on the control block every thread
// shares. The slot is not private to its thread, though. A thread that reads
// once and then idles would otherwise keep the key directory version it saw
// alive for as long as it idles, and under a write load a version held long
// enough comes to hold a whole retained copy of the tree — measured at
// ~1.1 GB for 20 M keys on a MariaDB thread parked in the server's thread
// cache. So every slot is registered, and the writer, on publishing, takes
// the cached state away from any slot that has not been used for a while.
//
// The protocol is a claim on the slot (RocksDB's per-thread SuperVersion
// cache works the same way). A reader exchanges the slot's pointer for
// kInUse, uses the entry, and stores the pointer back. The scrape leaves a
// slot that reads kInUse alone and swaps any other idle one to kObsolete,
// then frees the entry it took. A reader that finds kObsolete, or an empty
// slot, acquires the current state afresh. Nothing on the reader's path
// contends: the exchange is on its own line, and "recently used" is a scrape
// epoch the reader copies from a counter the scrape bumps ~10 times a
// second, not a clock.
// ---------------------------------------------------------------------------
export class DB;

struct ReadCacheEntry {
  const DB *owner{nullptr};  // compared, never dereferenced
  std::shared_ptr<const EngineState> state;
  std::int64_t last_write_time{0};
  // The scrape epoch this entry was last used in. Written by its reader,
  // read by the scrape, hence atomic; nothing else in the entry is touched
  // by anyone but the holder of the claim.
  std::atomic<std::uint64_t> used_epoch{0};
};

class ReadCacheRegistry;

class ReadCacheSlot {
public:
  ReadCacheSlot();
  ~ReadCacheSlot();
  ReadCacheSlot(const ReadCacheSlot &) = delete;
  auto operator=(const ReadCacheSlot &) -> ReadCacheSlot & = delete;

  // Sentinels: a slot is either empty, claimed, obsoleted, or holds an entry.
  [[nodiscard]] static auto in_use() noexcept -> ReadCacheEntry *;
  [[nodiscard]] static auto obsolete() noexcept -> ReadCacheEntry *;
  [[nodiscard]] static auto is_entry(const ReadCacheEntry *e) noexcept -> bool {
    return e != nullptr && e != in_use() && e != obsolete();
  }

  // Takes the claim. Returns the entry this thread cached, or nullptr when
  // there is none or the scrape took it; either way the slot reads kInUse
  // until release().
  [[nodiscard]] auto claim() noexcept -> ReadCacheEntry * {
    auto *e = ptr_.exchange(in_use(), std::memory_order_acquire);
    return is_entry(e) ? e : nullptr;
  }
  void release(ReadCacheEntry *e) noexcept {
    ptr_.store(e, std::memory_order_release);
  }

private:
  friend class ReadCacheRegistry;
  std::atomic<ReadCacheEntry *> ptr_{nullptr};
};

class ReadCacheRegistry {
public:
  static auto instance() -> ReadCacheRegistry & {
    // Immortal, like VersionChain: a thread can exit after static
    // destruction and its slot has to find the registry then.
    alignas(ReadCacheRegistry) static std::byte storage[sizeof(ReadCacheRegistry)];
    static auto *r = new (storage) ReadCacheRegistry();
    return *r;
  }

  void add(ReadCacheSlot *slot) {
    std::lock_guard<std::mutex> lk{mu_};
    slots_.push_back(slot);
  }
  void remove(ReadCacheSlot *slot) {
    std::lock_guard<std::mutex> lk{mu_};
    std::erase(slots_, slot);
  }

  [[nodiscard]] auto epoch() const noexcept -> std::uint64_t {
    return epoch_.load(std::memory_order_relaxed);
  }

  // Advances the epoch and takes the entry from every slot that has not
  // been used for `idle_epochs` epochs. A slot read as kInUse is skipped —
  // its reader is in the middle of a read — and a claim that lands between
  // the load and the swap makes the swap fail, so an entry is only ever
  // freed once no reader can be holding it.
  void scrape(std::uint64_t idle_epochs) {
    std::vector<ReadCacheEntry *> taken;
    {
      std::lock_guard<std::mutex> lk{mu_};
      const auto now = epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
      for (auto *slot : slots_) {
        auto *e = slot->ptr_.load(std::memory_order_acquire);
        if (!ReadCacheSlot::is_entry(e)) continue;
        if (now - e->used_epoch.load(std::memory_order_relaxed) <= idle_epochs)
          continue;
        if (slot->ptr_.compare_exchange_strong(e, ReadCacheSlot::obsolete(),
                                               std::memory_order_acq_rel)) {
          taken.push_back(e);
        }
      }
    }
    for (auto *e : taken) delete e;
  }

private:
  ReadCacheRegistry() = default;
  std::mutex mu_;
  std::vector<ReadCacheSlot *> slots_;
  std::atomic<std::uint64_t> epoch_{1};
};

inline ReadCacheSlot::ReadCacheSlot() { ReadCacheRegistry::instance().add(this); }
inline ReadCacheSlot::~ReadCacheSlot() {
  // Claim first, so a scrape in flight leaves the slot alone, then leave the
  // registry, then free what the thread was holding.
  auto *e = ptr_.exchange(in_use(), std::memory_order_acq_rel);
  ReadCacheRegistry::instance().remove(this);
  if (is_entry(e)) delete e;
}
inline auto ReadCacheSlot::in_use() noexcept -> ReadCacheEntry * {
  alignas(ReadCacheEntry) static std::byte storage[sizeof(ReadCacheEntry)];
  static auto *e = new (storage) ReadCacheEntry();
  return e;
}
inline auto ReadCacheSlot::obsolete() noexcept -> ReadCacheEntry * {
  alignas(ReadCacheEntry) static std::byte storage[sizeof(ReadCacheEntry)];
  static auto *e = new (storage) ReadCacheEntry();
  return e;
}

// What load_state_for_read hands out: the claim on this thread's slot for
// the duration of one read. The state reference it exposes is valid while
// the guard lives; the destructor gives the slot back.
class ReadStateGuard {
public:
  ReadStateGuard(ReadCacheSlot &slot, ReadCacheEntry *entry) noexcept
      : slot_{&slot}, entry_{entry} {}
  ~ReadStateGuard() { slot_->release(entry_); }
  ReadStateGuard(const ReadStateGuard &) = delete;
  auto operator=(const ReadStateGuard &) -> ReadStateGuard & = delete;
  ReadStateGuard(ReadStateGuard &&) = delete;
  auto operator=(ReadStateGuard &&) -> ReadStateGuard & = delete;

  [[nodiscard]] auto state() const noexcept
      -> const std::shared_ptr<const EngineState> & {
    return entry_->state;
  }
  [[nodiscard]] auto operator->() const noexcept -> const EngineState * {
    return entry_->state.get();
  }

private:
  ReadCacheSlot *slot_;
  ReadCacheEntry *entry_;
};

// ---------------------------------------------------------------------------
// DB — the public interface to a ByteCaskDB database.
//
// Thread safety: write operations (put, del, apply_batch) are serialised by
// write_mu_ for their in-memory phase (stage 1), which produces the prepared
// head. The fdatasync and the publication of a state (stage 2) run under the
// flush role, outside write_mu_, so the next batch's stage 1 overlaps the
// current fdatasync. Readers call state_.load() without acquiring either.
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

  // Writes key → value. Overwrites any existing value. Cannot conflict.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  auto put(const WriteOptions &opts, BytesView key,
           BytesView value) -> CommitResult;

  // Writes a tombstone for key.
  // Returns nullopt if the key was absent — nothing was written, no
  // sequence assigned. Rotates the active file if it has reached the
  // threshold. opts.sync controls whether fdatasync is called after the
  // write. Throws std::system_error on I/O failure or lock contention
  // (try_lock).
  [[nodiscard]] auto del(const WriteOptions &opts,
                        BytesView key) -> std::optional<CommitResult>;

  // Deletes all keys in [from, to). One append to the data file, one
  // optional fdatasync. Cannot conflict. Returns {sequence = 0, durable =
  // true} without writing if from >= to.
  auto del_range(const WriteOptions &opts, BytesView from,
                BytesView to) -> CommitResult;

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
  // modified since snap. Returns nullopt on conflict (a guard failed or
  // the implicit W-W check detected a conflict) — nothing was written.
  // A guardless, snapshot-less plan always commits. An empty plan commits
  // as a no-op, returning {sequence = 0, durable = true}.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  [[nodiscard]] auto apply_batch(WriteOptions opts,
                                 WritePlan plan) -> std::optional<CommitResult>;

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

  // Blocks until the durable sequence (highest sequence confirmed by
  // fdatasync) is >= min_sequence or timeout expires; returns the durable
  // sequence at return. min_sequence = 0, an already-reached target, or a
  // nonpositive timeout returns immediately. Identical semantics in Leader
  // and Follower mode (on a follower it reflects the last synced ingest).
  // Blocks until the published state covers sequence — a fresh snapshot
  // can see the write a plan lost to — or the engine degrades. See
  // apply_batch.
  void wait_published(std::uint64_t sequence) const;

  [[nodiscard]] auto durable_sequence(
      std::uint64_t min_sequence = 0,
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
  // Writes hint file via temp-then-rename. Batch-aware; idempotent if .hint
  // exists. Returns the offset past the last committed entry, or nullopt
  // when the hint already existed and nothing was scanned.
  static auto flush_hints_for(const std::shared_ptr<DataFile> &file,
                              const std::filesystem::path &dir)
      -> std::optional<Offset>;
  // Writes hint files for all sealed files in s.
  void flush_hints(const EngineState &s);
  // Both HintFile openers verify the file-level CRC before any parsing, so
  // either one is the point a damaged hint is detected.
  using HintOpener = auto (*)(std::filesystem::path) -> HintFile;
  // Opens a hint file, rebuilding it from its data file if it will not open.
  // A hint is a derived index, not the records it points at: a CRC failure in
  // one says the index is damaged, not that the data file behind it is.
  // Throws when the rebuild cannot produce a readable hint, leaving the
  // caller to apply fail_recovery_on_crc_errors to a file it cannot index.
  static auto open_hint_or_rebuild(const std::shared_ptr<DataFile> &data_file,
                                   const std::filesystem::path &hint_path,
                                   HintOpener open) -> HintFile;

  

  // Vacuum helpers
  // Batch-aware scan: copies live Puts and tombstones from source_file into dest_file.
  static auto vacuum_scan_and_copy(
      const std::shared_ptr<const EngineState> &snap,
      const DataFile &source_file, WritableDataFile &dest_file,
      std::uint32_t source_file_id) -> VacuumScanResult;
  // Remaps key_dir entries, updates file registry, publishes new state. Caller must hold write_mu_.
  void vacuum_commit(std::uint32_t old_file_id, const VacuumScanResult &scan,
                     std::shared_ptr<DataFile> new_sealed_file,
                     std::uint32_t dest_file_id);
  // Unlinks the old data and hint files. Open fds survive (POSIX).
  void vacuum_unlink_old_file(const std::shared_ptr<const EngineState> &snap,
                              std::uint32_t file_id);
  // Rewrites a sealed file into a new sealed file containing only live entries.
  [[nodiscard]] auto vacuum_compact_file(std::uint32_t file_id) -> bool;
  // Appends live entries from a sealed file into the active file, then removes the sealed file.
  void vacuum_remove_file(std::uint32_t file_id);

  // State access helpers — raw state_ / state_time_ access is confined here.
  // Per-thread read cache behind load_state_for_read (see ReadCacheSlot).
  // One function-local thread_local slot shared by every DB the thread
  // touches, so its entry records its owner. commit_wait seeds it with the
  // state that covered the thread's own write — see there for why the
  // timestamp alone is not enough.
  [[nodiscard]] static auto read_cache() -> ReadCacheSlot &;
  // Claims this thread's slot and returns the guard; the entry behind it
  // holds this DB's state, refreshed when the staleness rule says so.
  [[nodiscard]] auto load_state_for_read(const ReadOptions &opts) const
      -> ReadStateGuard;
  // Publishes reclaim: bumps the scrape epoch and frees the entries of
  // slots idle for kReadCacheIdleEpochs epochs. Rate-limited to one in
  // kReadCacheScrapePeriod; called from store_state.
  void scrape_read_caches() const;
  static constexpr auto kReadCacheScrapePeriod = std::chrono::milliseconds{100};
  static constexpr std::uint64_t kReadCacheIdleEpochs = 10;  // ~1 s idle
  mutable std::atomic<std::int64_t> last_scrape_ns_{0};
  // Barrier write path: the published state, which after quiesce() covers
  // the prepared head. Caller must hold write_mu_ and the flush role. Only
  // execute_slots builds on the head itself (load_head).
  [[nodiscard]] auto load_state_for_write() const
      -> std::shared_ptr<EngineState>;
  // Publish new state + bump timestamp. Caller must hold the flush role.
  // Runs O(1) invariant checks comparing old vs new; degrades on violation.
  void store_state(const std::shared_ptr<const EngineState> &old_state,
                   std::shared_ptr<EngineState> new_state);

  // Commit pipeline (stage 2). See docs/commit_pipeline_design.md.
  //
  // RAII holder of the flush role for barrier operations. Its destructor
  // resets head_ to the published state — after a barrier everything the
  // operation built is published, so the two heads must agree — then
  // releases the role and wakes waiters. Destroy it while write_mu_ is
  // still held: declare it after the lock guard in the same scope.
  class FlushRole {
  public:
    explicit FlushRole(DB &db) : db_{&db} {}
    // Neither copied nor moved: quiesce() returns a prvalue and every
    // holder initialises from it directly, so the role has one owner.
    FlushRole(const FlushRole &) = delete;
    FlushRole(FlushRole &&) = delete;
    auto operator=(const FlushRole &) -> FlushRole & = delete;
    auto operator=(FlushRole &&) -> FlushRole & = delete;
    ~FlushRole() {
      db_->store_head(db_->load_state());
      db_->finish_flush();
    }

  private:
    DB *db_;
  };
  // Takes the flush role, flushing and publishing the prepared head on this
  // thread if it is not already covered, then returns holding the role.
  // Caller must hold write_mu_, so head_ cannot move underneath. On return
  // the published state covers head_, or the engine is degraded.
  [[nodiscard]] auto quiesce() -> FlushRole;
  // write_mu_ plus quiesce() as one object, for barrier operations: locks,
  // quiesces, and on destruction resets head_, releases the role, then
  // unlocks — in that order, by member order, so the ordering rule cannot
  // be broken at a call site.
  class WriteBarrier {
  public:
    explicit WriteBarrier(DB &db)
        : lk_{*db.write_mu_}, role_{db.quiesce()} {}
    WriteBarrier(const WriteBarrier &) = delete;
    auto operator=(const WriteBarrier &) -> WriteBarrier & = delete;

  private:
    std::unique_lock<std::mutex> lk_;
    FlushRole role_;
  };
  // Blocks until the published state covers slot's sequence — durable for
  // sync=true, visible for sync=false — flushing on this thread whenever
  // the flush role is free. Sets slot.result->durable. Rethrows the flush
  // error that degraded the engine if this slot's entries are among the
  // ones it lost.
  void commit_wait(EngineSlot &slot);
  // Publishes the prepared head, after an fdatasync if it owes one. Caller
  // holds the flush role. Never takes write_mu_: stage 1 of the next batch
  // runs underneath the fdatasync. A failed fdatasync degrades the engine
  // and records the error for every writer appended since the last flush.
  void flush_pending();
  // commit_wait's flush: settle, flush_pending, then release the role and
  // wake every waiter.
  void flush_once();
  // Releases the flush role and wakes commit_wait / quiesce / durable_sequence
  // waiters. The empty durable_mu_ critical section orders the release
  // before the notify for waiters that checked the predicate and are about
  // to block.
  void finish_flush();
  // Degrade path of flush_pending: publishes a degraded copy of the
  // published state and records ex for the waiters.
  void flush_failed(std::exception_ptr ex,
                    const std::shared_ptr<const EngineState> &published,
                    const std::filesystem::path &active_path);
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
  // Runs recovery, undoing an interrupted vacuum it finds on the way.
  auto recovery_open(const Options &opts) -> EngineState;
  // Deletes the compacted file of an interrupted vacuum, or throws if the two
  // files are not a compacted file and its source.
  static void recovery_undo_interrupted_vacuum(const std::filesystem::path &a,
                                               const std::filesystem::path &b);
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
  auto recovery_load_parallel(EngineState s, std::vector<RecoveredFile> files,
                              unsigned recovery_threads, bool strict)
      -> EngineState;
#ifdef BYTECASK_KEYDIR_BLIND
  // Reconstructs a blind key directory straight from the sorted hint files:
  // fences in each file, range splitters from the fences, a k-way merge per
  // range into blind leaves. Builds no intermediate tree.
  auto recovery_load_streams(EngineState s, std::vector<RecoveredFile> files,
                             unsigned recovery_threads, bool strict)
      -> EngineState;
#endif
#ifdef BYTECASK_USE_BTREE
  // Reconstructs key_dir from hint files by range merge. Coupled to the B+
  // tree: it needs the tree to sample its own separators and to bulk-build
  // and concatenate range-disjoint slices. See the definition for why the
  // radix tree wants a different strategy.
  auto recovery_load_ranged(EngineState s, std::vector<RecoveredFile> files,
                            unsigned recovery_threads, bool strict)
      -> EngineState;
  // Builds a RecoveryResult by merging sorted hint runs into a bulk loader.
  // Requires the sorted hint files flush_hints_for writes.
  static auto recovery_build_sorted(std::span<RecoveredFile> files,
                                    bool strict) -> RecoveryResult;
#endif

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
  auto load_head() const -> std::shared_ptr<EngineState> {
    return std::atomic_load(&head_);
  }
  void store_head(std::shared_ptr<EngineState> s) {
    std::atomic_store(&head_, std::move(s));
  }
#pragma clang diagnostic pop

  // Member variables
  std::filesystem::path dir_;
  int lock_fd_{-1};  // flock() on dir_/.lock; released by close() in ~DB()
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  IoBackend io_backend_{IoBackend::Pread};
  // Declared before state_ so it outlives every DataFile holding a pointer to
  // it: members are destroyed in reverse declaration order.
  // Shared with every pool-backed data file: a file lends spans into the
  // pool's frames and can outlive the DB in an iterator's hands.
  std::shared_ptr<BufferPool> pool_;
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
  // Prepared head: the latest state produced by stage 1 of a write batch.
  // It may contain sync=true entries whose fdatasync has not returned, so
  // readers never see it. state_ is always an ancestor of head_ in the
  // persistent chain; they agree whenever no flush is in flight and no
  // batch is pending. Written only under write_mu_: advanced by
  // execute_slots, reset to state_ by ~FlushRole. Read by flush_pending
  // without it. Its durable_seq is not meaningful — the head chain never
  // learns about flushes — and flush_pending assigns it at publish.
  std::shared_ptr<EngineState> head_;
  // The flush role: exactly one thread runs flush_pending at a time. Taken
  // with exchange(true) in commit_wait and quiesce; released by
  // finish_flush. No thread holds it across two flushes of its own accord —
  // a writer flushes at most the head as it stands when it wins the role.
  std::atomic<bool> flush_in_flight_{false};
  // Longest the flush role waits for in-progress stage-1 work before it
  // captures the head. See flush_pending.
  static constexpr auto kFlushSettleMax = std::chrono::microseconds{200};
  // The fdatasync error that degraded the engine, rethrown by commit_wait
  // to every writer whose entries were appended since the last successful
  // flush. Guarded by durable_mu_. Cleared by resume().
  std::exception_ptr flush_error_;
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
  // Called by flush_pending on the flushing thread just before the
  // fdatasync. Lets a test hold a flush in flight while other writers
  // append behind it.
  std::function<void()> test_before_flush_sync_;
  // Called by store_state on the publishing thread between the state store
  // and the state_time_ store. Lets a test hold a publication in that gap.
  std::function<void()> test_between_publish_stores_;
  // Called by apply_batch on the writer thread after stage 1, just before
  // commit_wait. Lets a test hold a writer whose entries are already in the
  // head while another thread flushes and publishes them.
  std::function<void()> test_before_commit_wait_;
  // Publishes s through the checked store_state under a write barrier, so
  // tests can drive the runtime invariant checks with a crafted state.
  void test_publish(std::shared_ptr<EngineState> s) {
    WriteBarrier barrier{*this};
    store_state(load_state(), std::move(s));
  }
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
  std::optional<CommitResult> result;
  // On a conflict: the sequence of the entry the plan lost to, and the
  // plan's snapshot's next_seq. A snapshot that already covers every
  // published write lost to something still in flight — see apply_batch,
  // which then reports the conflict only once that write is published.
  std::uint64_t conflict_lost_to{0};
  std::uint64_t conflict_snap_next{0};
};


DbDegraded::~DbDegraded() = default;
DbFollowerMode::~DbFollowerMode() = default;

#pragma region Internal helpers

namespace {

// Generates a unique data file stem.
// Format: "data_{YYYYMMDDHHmmss}_{RRRRRRRRRRRRRRRR}_V01"
//   - Timestamp: UTC second precision, human-readable creation time (debug hint
//     only — does not reflect content age after compaction).
//   - R...: 8-byte random hex salt. The timestamp only separates files by the
//     second, so the salt alone separates every file minted inside one — with
//     a small max_file_bytes that is thousands of files, and the collision rate
//     grows with the square of that count. 64 bits keeps it negligible
//     (~1e-13/second at 2,000 files) where 32 bits did not (~5e-4).
//   - V01: file format version. Nothing parses the stem's interior; recovery
//     keys on this suffix, so salt width can change without breaking old files.
//
// Drawn from random_device per call rather than from a seeded PRNG: seeding
// mt19937 with one 32-bit draw would give a process only 2^32 possible salt
// sequences however wide each output was, which is the property that has to
// improve. Two reads per rotation, and rotation is per max_file_bytes.
auto make_data_file_stem() -> std::string {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  static thread_local std::random_device rd;
#pragma clang diagnostic pop
  const auto salt = (static_cast<std::uint64_t>(rd()) << 32) | rd();

  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  ::gmtime_r(&tt, &tm_buf);

  return std::format("data_{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}_{:016x}_V01",
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
    KeyDirTransient key_dir,
    TransientU32Map<std::shared_ptr<DataFile>> files,
    TransientU32Map<FileStats> file_stats,
    std::uint32_t active_file_id, std::uint32_t next_file_id,
    std::uint64_t next_seq, std::uint64_t durable_seq,
    std::uint64_t sync_requested_seq,
    Mode mode, bool degraded, std::string degraded_reason)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_file_id_{next_file_id}, next_seq_{next_seq},
      durable_seq_{durable_seq}, sync_requested_seq_{sync_requested_seq},
      mode_{mode}, degraded_{degraded},
      degraded_reason_{std::move(degraded_reason)} {}

auto EngineState::transient() const -> TransientEngineState {
  return TransientEngineState{
      key_dir.transient(), files.transient(), file_stats.transient(),
      active_file_id, next_file_id, next_seq, durable_seq,
      sync_requested_seq, mode, degraded, degraded_reason};
}

auto TransientEngineState::validate_preconditions(
    const WritePlan &plan, std::uint64_t &lost_to) const -> bool {
  const auto *snap_state =
      plan.snap_ ? plan.snap_->state_.get() : nullptr;
  // The head's latest sequence: what a retry must see when the entry the
  // plan lost to has no sequence of its own (deleted, or absent).
  const auto head_latest = next_seq_ - 1;
  const auto lost = [&](const std::optional<KeyDirEntry> &cur) {
    lost_to = cur ? cur->sequence() : head_latest;
    return false;
  };

  // 1. Point guards.
  for (const auto &[key, guard] : plan.guards_) {
    const std::span<const std::byte> key_span{key};
    const auto cur_entry = kd_get(key_dir_, key_span, kd_ctx());

    switch (guard.precondition) {
    case WritePlan::Precondition::MustExist:
      if (!cur_entry) return lost(std::nullopt);
      break;
    case WritePlan::Precondition::MustBeAbsent:
      if (cur_entry) return lost(cur_entry);
      break;
    case WritePlan::Precondition::MustBeUnchanged: {
      // ensure_unchanged already enforced snap_ is present at build time.
      const auto snap_entry = kd_get(snap_state->key_dir, key_span, snap_state->kd_ctx());
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
      const std::uint64_t cur_seq = cur_entry ? cur_entry->sequence() : 0;
      if (cur_seq != snap_seq) return lost(cur_entry);
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
    for (auto it = kd_lower_bound(key_dir_, from_span, kd_ctx());
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (Key{key_span} >= Key{to_span}) break;
      const auto snap_entry = kd_get(snap_state->key_dir, key_span, snap_state->kd_ctx());
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
      if (entry.sequence() != snap_seq) return lost(entry);
    }

    // Check snapshot for keys deleted since snapshot.
    for (auto it = kd_lower_bound(snap_state->key_dir, from_span,
                                         snap_state->kd_ctx());
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (Key{key_span} >= Key{to_span}) break;
      if (!kd_get(key_dir_, key_span, kd_ctx())) return lost(std::nullopt);
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
              const auto snap_entry = kd_get(snap_state->key_dir, key_span, snap_state->kd_ctx());
              const auto cur_entry = kd_get(key_dir_, key_span, kd_ctx());
              const bool appeared = !snap_entry && cur_entry;
              const bool deleted = snap_entry && !cur_entry;
              const bool modified =
                  snap_entry && cur_entry &&
                  cur_entry->sequence() != snap_entry->sequence();
              if (appeared || deleted || modified) {
                has_conflict = true;
                lost_to = cur_entry ? cur_entry->sequence() : head_latest;
              }
            } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
              const std::span<const std::byte> key_span{op.key};
              const auto snap_entry = kd_get(snap_state->key_dir, key_span, snap_state->kd_ctx());
              const auto cur_entry = kd_get(key_dir_, key_span, kd_ctx());
              const bool appeared = !snap_entry && cur_entry;
              const bool deleted = snap_entry && !cur_entry;
              const bool modified =
                  snap_entry && cur_entry &&
                  cur_entry->sequence() != snap_entry->sequence();
              if (appeared || deleted || modified) {
                has_conflict = true;
                lost_to = cur_entry ? cur_entry->sequence() : head_latest;
              }
            } else if constexpr (std::is_same_v<T, WritePlan::RangeDel>) {
              // Range conflict check: verify no keys in [from, to) changed since snapshot
              const std::span<const std::byte> from_span{op.from};
              const std::span<const std::byte> to_span{op.to};

              // Check current state for keys modified since snapshot.
              for (auto it = kd_lower_bound(key_dir_, from_span, kd_ctx());
                   it != std::default_sentinel && !has_conflict; ++it) {
                auto [key_span, entry] = *it;
                if (Key{key_span} >= Key{to_span}) break;
                const auto snap_entry = kd_get(snap_state->key_dir, key_span, snap_state->kd_ctx());
                const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
                if (entry.sequence() != snap_seq) {
                  has_conflict = true;
                  lost_to = entry.sequence();
                }
              }

              // Check snapshot for keys deleted since snapshot.
              for (auto it = kd_lower_bound(snap_state->key_dir, from_span,
                                         snap_state->kd_ctx());
                   it != std::default_sentinel && !has_conflict; ++it) {
                auto [key_span, entry] = *it;
                if (Key{key_span} >= Key{to_span}) break;
                if (!kd_get(key_dir_, key_span, kd_ctx())) {
                  has_conflict = true;
                  lost_to = head_latest;
                }
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
            const auto val_size = narrow<std::uint32_t>(op.value.size());
            note_pending(offsets[io_idx], next_seq_, key_span, val_size);
            const auto existing = kd_put(
                key_dir_, key_span,
                KeyDirEntry::make(next_seq_, offsets[io_idx], active_file_id_,
                                  val_size),
                kd_ctx());
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              const auto ef = existing->file_id();
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
            }
            const auto sz = entry_size(key_span.size(), val_size);
            file_stats_.update(active_file_id_, [sz](FileStats &fs) {
              fs.live_bytes += sz;
              fs.total_bytes += sz;
            });
            ++next_seq_;
            ++io_idx;
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            const std::span<const std::byte> key_span{op.key};
            const auto existing = kd_erase(key_dir_, key_span, kd_ctx());
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
              fs.tombstone_bytes += del_sz;
            });
            ++next_seq_;
            ++io_idx;
          } else {
            // RangeDel: iterate key_dir in [from, to), decrement live_bytes
            // on each affected file, erase from key_dir, then account for
            // the entry itself (a tombstone: total and tombstone bytes, never
            // live).
            const std::span<const std::byte> from_span{op.from};
            const std::span<const std::byte> to_span{op.to};

            // Collect keys to erase — cannot erase during iteration.
            std::vector<Key> to_erase;
            for (auto it = kd_lower_bound(key_dir_, from_span, kd_ctx());
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
              (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
            }

            const auto rd_sz = entry_size(op.from.size(), op.to.size());
            file_stats_.update(active_file_id_, [rd_sz](FileStats &fs) {
              fs.total_bytes += rd_sz;
              fs.tombstone_bytes += rd_sz;
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
      const auto val_size = narrow<std::uint32_t>(e.value.size());
      note_pending(offset, e.sequence, e.key, val_size);
      const auto existing = kd_put(
          key_dir_, e.key,
          KeyDirEntry::make(e.sequence, offset, active_file_id_, val_size),
          kd_ctx());
      if (existing) {
        const auto dec =
            entry_size(e.key.size(), existing->value_size());
        const auto ef = existing->file_id();
        file_stats_.update(
            ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
      }
      const auto sz = entry_size(e.key.size(), val_size);
      file_stats_.update(active_file_id_, [sz](FileStats &fs) {
        fs.live_bytes += sz;
        fs.total_bytes += sz;
      });
      break;
    }

    case EntryType::Delete: {
      const auto existing = kd_erase(key_dir_, e.key, kd_ctx());
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
        fs.tombstone_bytes += del_sz;
      });
      break;
    }

    case EntryType::RangeDel: {
      // key = from, value = to
      std::vector<Key> to_erase;
      for (auto it = kd_lower_bound(key_dir_, e.key, kd_ctx());
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
        (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
      }
      const auto rd_sz = entry_size(e.key.size(), e.value.size());
      file_stats_.update(active_file_id_, [rd_sz](FileStats &fs) {
        fs.total_bytes += rd_sz;
        fs.tombstone_bytes += rd_sz;
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
    std::shared_ptr<DataFile> new_file, std::uint32_t new_file_id) {
  files_.set(active_file_id_, std::move(sealed_old));
  active_file_id_ = new_file_id;
  files_.set(active_file_id_, std::move(new_file));
  file_stats_.set(active_file_id_, FileStats{});
}

auto TransientEngineState::reserve_file_id() -> std::uint32_t {
  KeyDirEntry::check_file_id(next_file_id_);
  return next_file_id_++;
}

void TransientEngineState::apply_vacuum(
    std::uint32_t old_file_id, const VacuumScanResult &scan,
    std::shared_ptr<DataFile> new_sealed_file, std::uint32_t reserved_file_id) {
  const auto dest_file_id =
      new_sealed_file ? reserved_file_id : active_file_id_;

  // The destination is registered before the remap: a key directory that
  // reads keys back may resolve an already remapped record while placing the
  // next one.
  if (new_sealed_file) {
    files_.set(dest_file_id, std::move(new_sealed_file));
  }

  auto actual_live_bytes = scan.live_bytes;
  for (const auto &m : scan.mappings) {
    const std::span<const std::byte> key_span{m.key};
    const auto cur = kd_get(key_dir_, key_span, kd_ctx());
    if (cur && cur->sequence() == m.sequence) {
      (void)kd_put(key_dir_, key_span,
                   KeyDirEntry::make(m.sequence, m.new_offset, dest_file_id,
                                     m.value_size),
                   kd_ctx());
    } else {
      actual_live_bytes -= entry_size(m.key.size(), m.value_size);
    }
  }

  files_.erase(old_file_id);

  file_stats_.erase(old_file_id);
  if (dest_file_id != active_file_id_) {
    file_stats_.set(dest_file_id,
                    FileStats{actual_live_bytes, scan.total_bytes,
                              scan.min_sequence, scan.max_sequence,
                              scan.tombstone_bytes});
  } else {
    file_stats_.update(dest_file_id, [actual_live_bytes,
                                      total = scan.total_bytes,
                                      tomb = scan.tombstone_bytes,
                                      smin = scan.min_sequence,
                                      smax = scan.max_sequence](FileStats &fs) {
      fs.live_bytes += actual_live_bytes;
      fs.total_bytes += total;
      fs.tombstone_bytes += tomb;
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
    fs.tombstone_bytes = 0;
  });

  std::uint64_t max_seq = 0;
  std::uint64_t seq_min = 0;
  std::uint64_t seq_max = 0;
  std::uint64_t tomb = 0;
  for (const auto &e : entries) {
    const std::span<const std::byte> key_span{e.key};
    if (e.sequence > max_seq) max_seq = e.sequence;
    if (seq_min == 0 || e.sequence < seq_min) seq_min = e.sequence;
    if (e.sequence > seq_max) seq_max = e.sequence;
    // entries holds every committed entry in the file, so the file's
    // tombstones are rebuilt from scratch here, like its bounds.
    tomb += tombstone_size(e.entry_type, e.key.size(), e.range_end.size());

    switch (e.entry_type) {
    case EntryType::Put: {
      const auto existing = kd_get(key_dir_, key_span, kd_ctx());
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
        (void)kd_put(key_dir_, key_span,
                     KeyDirEntry::make(e.sequence, e.file_offset, file_id,
                                       e.value_size),
                     kd_ctx());
      }
      break;
    }
    case EntryType::Delete: {
      const auto existing = kd_get(key_dir_, key_span, kd_ctx());
      if (existing && existing->sequence() < e.sequence) {
        const auto dec = entry_size(key_span.size(), existing->value_size());
        const auto ef = existing->file_id();
        file_stats_.update(ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
        (void)kd_erase(key_dir_, key_span, kd_ctx());
      }
      break;
    }
    case EntryType::RangeDel: {
      // Same suppression rule recovery applies when it replays a range
      // tombstone from a hint file: erase every key in [from, to) carrying a
      // lower sequence. Skipping it here would leave the resumed key
      // directory holding keys a fresh open would not — the one divergence
      // resume() exists to prevent.
      const Key end{std::span<const std::byte>{e.range_end}};
      std::vector<Key> to_erase;
      for (auto it = kd_lower_bound(key_dir_, key_span, kd_ctx());
           it != std::default_sentinel; ++it) {
        auto [k, entry] = *it;
        if (Key{k} >= end) break;
        if (entry.sequence() >= e.sequence) continue;
        const auto dec = entry_size(k.size(), entry.value_size());
        const auto ef = entry.file_id();
        file_stats_.update(ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
        to_erase.emplace_back(k);
      }
      for (const auto &k : to_erase)
        (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
      break;
    }
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      // A marker carries no key and no value, so it moves no key directory
      // entry and no live byte. Its sequence is counted above with every
      // other entry's, which is the whole reason the scan collects it: the
      // file's bounds must describe the sequences the file actually holds.
      // An orphaned pair never reaches here — it lies above valid_offset and
      // is truncated rather than replayed.
      break;
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
  file_stats_.update(file_id,
                     [tomb](FileStats &fs) { fs.tombstone_bytes = tomb; });
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

void TransientEngineState::note_sync_requested(std::uint64_t seq) {
  sync_requested_seq_ = std::max(sync_requested_seq_, seq);
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
  s->sync_requested_seq = sync_requested_seq_;
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
      io_backend_{opts.io_backend},
      size_limits_{std::min(opts.max_key_bytes, kMaxKeySize),
                   std::min(opts.max_value_bytes, kMaxValueSize)},
      state_{std::make_shared<EngineState>()} {
#ifdef __EMSCRIPTEN__
  if (opts.io_backend == IoBackend::Mmap) {
    throw std::invalid_argument{
        "IoBackend::Mmap is not supported on WASM/Emscripten builds: "
        "mmap emulation would double-buffer the data file into the WASM "
        "heap instead of avoiding a copy, so this build always uses the "
        "pread-based data file regardless of this option"};
  }
#endif
  if (opts.io_backend == IoBackend::BufferPool) {
    // The active file is pinned in the pool by a later phase; until then the
    // floor only has to leave room for a working set beyond one file. Rejecting
    // rather than degrading keeps the configured bound meaningful.
    if (opts.buffer_pool.capacity_bytes < 2 * opts.max_file_bytes) {
      throw std::invalid_argument{std::format(
          "IoBackend::BufferPool: buffer_pool.capacity_bytes = {} must be at "
          "least 2 x max_file_bytes = {}. Raise the pool, or lower "
          "max_file_bytes — the two are coupled.",
          opts.buffer_pool.capacity_bytes, opts.max_file_bytes)};
    }
    pool_ = std::make_shared<BufferPool>(opts.buffer_pool);
  }
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
    const auto recovery_start = std::chrono::steady_clock::now();
    auto s = recovery_open(opts);
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
    auto new_active = createDataFileForWrite(
        dir_, stem, ".data", rotation_threshold_, io_backend_, pool_,
        s.active_file_id);
    if (pool_) pool_->set_active_file(s.active_file_id);
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
  auto s = load_state();
  if (!s->files.empty()) {
    try {
      auto t = s->transient();
      t.active_file().sync();
      t.active_file().shrink_to_fit();
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
  // The guard, not a copy of the state: copying it is a read-modify-write
  // on a control block every reader shares — the line that capped
  // concurrent gets before the read did.
  const auto s = load_state_for_read(opts);
#ifdef BYTECASK_KEYDIR_BLIND
  // The read that confirms the key is the read of the value.
  if (!kd_read_value(s->key_dir, key, s->kd_ctx(opts.verify_checksums), out))
    return false;
  counters_.disk_reads.add(1);
  counters_.disk_read_bytes.add(std::ssize(out));
  return true;
#else
  const auto kv = kd_get(s->key_dir, key, s->kd_ctx());
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
  counters_.disk_reads.add(1);
  counters_.disk_read_bytes.add(static_cast<std::int64_t>(kv->value_size()));
  return true;
#endif
}

// Writes key → value. Overwrites any existing value. Cannot conflict.
// Rotates the active file if it has reached the threshold.
// opts.sync controls whether fdatasync is called after the write.
// Throws std::system_error on I/O failure or lock contention (try_lock).
auto DB::put(const WriteOptions &opts, BytesView key,
            BytesView value) -> CommitResult {
  WritePlan plan{size_limits_};
  plan.put(key, value);
  return *apply_batch(opts, std::move(plan));
}

auto DB::del(const WriteOptions &opts,
            BytesView key) -> std::optional<CommitResult> {
  WritePlan plan{size_limits_};
  plan.ensure_present(key);
  plan.del(key);
  return apply_batch(opts, std::move(plan));
}

auto DB::del_range(const WriteOptions &opts, BytesView from,
                  BytesView to) -> CommitResult {
  check_key_size(from.size(), size_limits_.max_key_bytes);
  check_key_size(to.size(), size_limits_.max_key_bytes);
  if (Key{from} >= Key{to}) return CommitResult{.sequence = 0, .durable = true};
  WritePlan plan{size_limits_};
  plan.del_range(from, to);
  return *apply_batch(opts, std::move(plan));
}

auto DB::contains_key(const ReadOptions& opts, BytesView key) const -> bool {
  const auto s = load_state_for_read(opts);
  return kd_contains(s->key_dir, key, s->kd_ctx(opts.verify_checksums));
}

#pragma endregion

#pragma region Snapshot and apply_batch

auto DB::snapshot() const -> Snapshot {
  ReadOptions opts{};
  return Snapshot{load_state_for_read(opts).state(), size_limits_};
}

// The single write path. Routes to either write_group_ (default) or
// solo_writer_ depending on plan characteristics. put/del/apply_batch are
// thin wrappers that construct a WritePlan and delegate here.
auto DB::apply_batch(WriteOptions opts,
                     WritePlan plan) -> std::optional<CommitResult> {
  if (auto s = load_state(); !s->is_write_allowed()) {
    if (s->degraded) throw DbDegraded{s->degraded_reason};
    throw DbFollowerMode{"write rejected: engine is in follower mode"};
  }
  if (plan.empty()) return CommitResult{.sequence = 0, .durable = true};

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

  // Stage 1 is done: the slot's entries are in the prepared head and the
  // page cache. Stage 2 — fdatasync (if sync) and publication — happens
  // here, on this thread when the flush role is free.
#ifdef BYTECASK_TESTING
  if (test_before_commit_wait_) test_before_commit_wait_();
#endif
  if (slot.result && slot.result->sequence != 0) commit_wait(slot);

  // A conflict against a write the head holds but no snapshot can see yet.
  // Plans are validated against the head, which is right — two in-flight
  // writes to one key must not both pass — but a snapshot sees only the
  // published state, so until that write is published every retry from a
  // fresh snapshot finds the same state and loses again: a caller's retry
  // loop spun through the whole fdatasync of a competing write, 50 to 300
  // attempts per collision, and the CAS benchmark's average attempts went
  // from 1.0 to 7-350 when the commit was pipelined. So a conflict is
  // reported once the head it lost to is published — the moment a retry
  // can make progress — which is when it was reported before the pipeline.
  // A plan whose snapshot is already behind the published state returns at
  // once: its retry has something new to see. The wait is for the write the
  // plan lost to, not for the whole head: under many writers the head
  // always holds entries appended after the flush in progress began, and
  // waiting for those would cost a second flush for nothing.
  if (!slot.result && slot.conflict_snap_next != 0 &&
      slot.conflict_snap_next >= load_state()->next_seq) {
    wait_published(slot.conflict_lost_to);
  }

  return slot.result;
}

#pragma endregion

#pragma region Writer executors

// Prepares and applies one slot against the transient. Pure in-memory: no
// I/O. Entries are appended to all_entries; running_offset is advanced by
// the total byte size of entries produced. Returns false on validation
// failure (slot.result set to nullopt). durable is left false on success;
// execute_slots fills it in once the batch's durability is known.
auto DB::execute_slot(TransientEngineState &t, EngineSlot &slot,
                      std::vector<DataEntryView> &all_entries,
                      std::uint64_t &running_offset) -> bool {
  if (slot.plan.empty()) {
    slot.result = CommitResult{};
    return true;
  }

  if (!t.validate_preconditions(slot.plan, slot.conflict_lost_to)) {
    slot.result = std::nullopt;
    slot.conflict_snap_next =
        slot.plan.snap_ ? slot.plan.snap_->state_->next_seq : 0;
    return false;
  }

  auto entries = t.prepare_write(slot.plan);
  if (entries.empty()) {
    slot.result = CommitResult{};
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

  const auto committed_sequence = entries.back().sequence;

  all_entries.insert(all_entries.end(),
                     std::make_move_iterator(entries.begin()),
                     std::make_move_iterator(entries.end()));

  slot.result = CommitResult{.sequence = committed_sequence};
  return true;
}

// Executor callback shared by solo_writer_ and write_group_ — stage 1 of
// the commit pipeline: (1) per-slot validate/prepare/apply in-memory,
// collecting entries; (2) one append_entries call for all collected
// entries; then the resulting state becomes the prepared head. No fdatasync
// and no publication here — commit_wait does both, so this batch's phase 1
// and 2 overlap the previous batch's fdatasync. The one exception is a
// batch that crosses the rotation threshold: it quiesces the pipeline and
// runs sync / rotate / publish inline, once per max_file_bytes.
void DB::execute_slots(std::vector<Slot *> &batch) {
  std::lock_guard<std::mutex> wg{*write_mu_};

  // Admission is decided on the published state, not the head: a flush
  // fails without write_mu_ and head_ is only reset by the next barrier,
  // so the head can be non-degraded while the engine is. (Mode is the same
  // in both — set_mode is a barrier.) A batch that loaded head_ just before
  // such a failure still ends in commit_wait with the flush error, its
  // bytes handled by resume() like the failed flush's.
  auto published = load_state();
  if (!published->is_write_allowed()) {
    auto ex = published->degraded
        ? std::make_exception_ptr(DbDegraded{published->degraded_reason})
        : std::make_exception_ptr(
              DbFollowerMode{"write rejected: engine is in follower mode"});
    for (auto *s : batch) s->err = ex;
    return;
  }
  auto current = load_head();

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

  if (all_entries.empty()) {
    // No entry was appended by any slot (empty/guard-only plans only) —
    // nothing to flush or publish. Every committed slot's {sequence = 0}
    // result is trivially durable.
    for (auto *s : batch) {
      auto &slot = static_cast<EngineSlot &>(*s);
      if (slot.result) slot.result->durable = true;
    }
    return;
  }

  // Highest sequence in this batch — the fdatasync that covers it advances
  // durable_seq to it.
  const auto batch_max_seq = t.next_seq() - 1;

  // Phase 2: one I/O call for all collected entries.
  std::vector<std::uint64_t> io_offsets(all_entries.size());
  try {
    file.append_entries(all_entries, io_offsets);
  } catch (...) {
    auto ex = std::current_exception();
    try { file.sync(); } catch (...) {}
    // Publishing needs the flush role: an in-flight flush must land (or
    // fail) first, so its publication cannot overwrite the degraded state.
    auto role = quiesce();
    auto err_s = load_state()->degraded_copy(std::format(
        "append IO error on '{}': call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(std::move(err_s));
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }

  counters_.bytes_written.fetch_add(
      static_cast<std::int64_t>(running_offset - initial_offset),
      std::memory_order_relaxed);
  if (any_sync) t.note_sync_requested(batch_max_seq);

  if (!t.is_rotation_needed(rotation_threshold_)) {
    assert(t.active_file().size() <= rotation_threshold_);
    store_head(std::move(t).persistent());
    // durable is filled in by commit_wait once the flush covering this
    // batch has landed.
    return;
  }

  // Rotation barrier: everything before this batch is flushed and
  // published, and this thread holds the flush role until `role` dies.
  // Two fdatasyncs, once per max_file_bytes: quiesce() flushes the previous
  // head, then this batch is synced before the file is sealed. apply_sync
  // gives t the durable_seq the head chain does not carry; degraded
  // transitions build on the published state.
  auto role = quiesce();
  published = load_state();  // quiesce may have published the previous head
  try {
    file.sync();
    counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
    t.apply_sync(batch_max_seq);
  } catch (...) {
    auto ex = std::current_exception();
    auto err_s = published->degraded_copy(std::format(
        "rotation fdatasync failed on '{}': bytes in page cache but "
        "durability not confirmed. Call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(std::move(err_s));
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }
  try {
    rotate_active_file(t, published);
    counters_.file_rotations.fetch_add(1, std::memory_order_relaxed);
    counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
  } catch (...) {
    auto ex = std::current_exception();
    t.apply_degrade(std::format(
        "post-write rotation failed for '{}': active file is sealed "
        "but new file could not be created. Call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(published, std::move(t).persistent());
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }

  if (any_sync) {
    try {
      file.sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
      t.apply_sync(batch_max_seq);
    } catch (...) {
      auto ex = std::current_exception();
      auto err_s = published->degraded_copy(std::format(
          "commit fdatasync failed on '{}': bytes in page cache but "
          "durability not confirmed. Call resume() to recover.",
          file.path().string()));
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      store_state(std::move(err_s));
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  assert(t.active_file().size() <= rotation_threshold_);
  const auto final_durable_seq = t.durable_seq();
  store_state(published, std::move(t).persistent());
  for (auto *s : batch) {
    auto &slot = static_cast<EngineSlot &>(*s);
    if (slot.result) slot.result->durable = final_durable_seq >= slot.result->sequence;
  }
}

#pragma endregion

#pragma region Commit pipeline stage 2

void DB::flush_pending() {
  auto head = load_head();
  auto published = load_state();
  if (published->degraded) return;
  if (head->next_seq <= published->next_seq) return;  // nothing pending

  const bool need_sync = head->sync_requested_seq > published->durable_seq;
  if (need_sync) {
#ifdef BYTECASK_TESTING
    if (test_before_flush_sync_) test_before_flush_sync_();
#endif
    try {
      static_cast<WritableDataFile &>(head->active_file()).sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      flush_failed(std::current_exception(), published,
                   head->active_file().path());
      return;
    }
  }

  // Publish the head as it stood before the fdatasync. Entries appended
  // since are not claimed: the next flush covers them. O(1): persistent
  // roots are shared, only the integers differ. durable_seq is assigned
  // here; the head's own value is whatever state it was derived from.
  auto pub = std::make_shared<EngineState>(*head);
  pub->durable_seq = need_sync ? head->next_seq - 1 : published->durable_seq;
  store_state(published, std::move(pub));
}

void DB::flush_failed(std::exception_ptr ex,
                      const std::shared_ptr<const EngineState> &published,
                      const std::filesystem::path &active_path) {
  auto err_s = published->degraded_copy(std::format(
      "commit fdatasync failed on '{}': bytes in page cache but "
      "durability not confirmed. Call resume() to recover.",
      active_path.string()));
  counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
  // Published and recorded under one durable_mu_ hold: commit_wait checks
  // flush_error_ under the same mutex, so no waiter can see the degraded
  // state without the error and report DbDegraded where the contract
  // promises the I/O error. (Raw store: the checked store_state takes
  // durable_mu_ itself.)
  std::lock_guard<std::mutex> lk{durable_mu_};
  store_state(std::move(err_s));
  flush_error_ = std::move(ex);
}

void DB::finish_flush() {
  flush_in_flight_.store(false, std::memory_order_release);
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

void DB::flush_once() {
  // Let slots already inside the write group finish stage 1 so this flush
  // covers them. Writers released by the previous flush re-enter within a
  // few microseconds but need a stage-1 batch (validate, apply, pwritev)
  // before they are in the head; a flush that starts the instant the role
  // is won leaves them for the flush after. Measured with 8 zero-think-time
  // writers: 4.15 → 7.78 commits per fdatasync, +64% throughput, same
  // fdatasync rate. A throughput heuristic only, which is why it lives
  // here and not in flush_pending: quiesce() callers hold write_mu_, so a
  // running leader would be blocked on it and busy() could never clear.
  // Bounded so a stalled leader cannot hold the disk; the bound only binds
  // under continuous arrivals, where it is ~8% of a flush.
  {
    const auto deadline =
        std::chrono::steady_clock::now() + kFlushSettleMax;
    while (write_group_.busy()
           && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
  }
  flush_pending();
  finish_flush();
}

auto DB::quiesce() -> FlushRole {
  for (;;) {
    if (!flush_in_flight_.exchange(true, std::memory_order_acq_rel)) {
      flush_pending();
      return FlushRole{*this};
    }
    std::unique_lock<std::mutex> lk{durable_mu_};
    durable_cv_.wait(lk, [&] {
      return !flush_in_flight_.load(std::memory_order_acquire);
    });
  }
}

void DB::commit_wait(EngineSlot &slot) {
  auto &result = *slot.result;
  const auto target = result.sequence;
  const bool want_durable = slot.opts.sync;
  for (;;) {
    auto published = load_state();
    const bool covered = want_durable
        ? published->durable_seq >= target
        : published->next_seq > target;
    if (covered) {
      result.durable = published->durable_seq >= target;
      // Another thread may have published this state and not yet stored
      // state_time_: a read on this thread would compare timestamps, find
      // nothing new and serve its cached pre-write snapshot. Seed the
      // cache with the covering state instead. last_write_time is left as
      // is: once the timestamp lands the next read refreshes as usual.
      {
        auto &slot = read_cache();
        auto *e = slot.claim();
        if (e == nullptr) {
          e = new ReadCacheEntry();
        }
        if (e->owner != this) {
          e->owner = this;
          e->last_write_time = 0;
        }
        e->state = std::move(published);
        e->used_epoch.store(ReadCacheRegistry::instance().epoch(),
                            std::memory_order_relaxed);
        slot.release(e);
      }
      return;
    }
    {
      std::lock_guard<std::mutex> lk{durable_mu_};
      if (flush_error_) std::rethrow_exception(flush_error_);
    }
    if (published->degraded) throw DbDegraded{published->degraded_reason};

    if (!flush_in_flight_.exchange(true, std::memory_order_acq_rel)) {
      flush_once();
      continue;
    }
    counters_.commit_wait_blocked.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk{durable_mu_};
    // Woken by finish_flush (role released, error recorded) and by
    // store_state when durable_seq advances. The coverage term is for a
    // waiter that loaded `published` before a flush landed and reached the
    // wait after: it must not sleep until the flush after that one.
    durable_cv_.wait(lk, [&] {
      if (!flush_in_flight_.load(std::memory_order_acquire)) return true;
      if (flush_error_ != nullptr) return true;
      auto now = load_state();
      return want_durable ? now->durable_seq >= target
                          : now->next_seq > target;
    });
  }
}

#pragma endregion

#pragma region Snapshot read methods

auto Snapshot::contains_key(const ReadOptions& opts,
                            BytesView key) const -> bool {
  return kd_contains(state_->key_dir, key,
                     state_->kd_ctx(opts.verify_checksums));
}

// Reads the value for key from the frozen snapshot state into out.
// Thread-local I/O buffer reused across calls to amortize allocation.
auto Snapshot::get(const ReadOptions& opts, BytesView key,
                   Bytes &out) const -> bool {
#ifdef BYTECASK_KEYDIR_BLIND
  return kd_read_value(state_->key_dir, key,
                       state_->kd_ctx(opts.verify_checksums), out);
#else
  const auto kv = kd_get(state_->key_dir, key, state_->kd_ctx());
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
#endif
}

auto Snapshot::iter_from(const ReadOptions& opts, BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto it = kd_value_lower_bound(state_->key_dir, from, state_->kd_ctx());
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{state_, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto Snapshot::keys_from(const ReadOptions& /*opts*/, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto it = from.empty() ? kd_begin(state_->key_dir, state_->kd_ctx())
                         : kd_lower_bound(state_->key_dir, from,
                                          state_->kd_ctx());
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto Snapshot::riter_from(const ReadOptions& opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto it = kd_value_rlower_bound(state_->key_dir, from, state_->kd_ctx());
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{state_, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto Snapshot::rkeys_from(const ReadOptions& /*opts*/, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto begin_it = from.empty()
      ? kd_end(state_->key_dir, state_->kd_ctx())
      : kd_upper_bound(state_->key_dir, from, state_->kd_ctx());
  auto end_it = kd_begin(state_->key_dir, state_->kd_ctx());
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
  auto it = kd_value_lower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{s.state(), std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

// Returns an input range of keys >= from. Walks the in-memory key directory
// only; no disk I/O.
auto DB::keys_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto s = load_state_for_read(opts);
  auto it = from.empty() ? kd_begin(s->key_dir, s->kd_ctx())
                         : kd_lower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto DB::riter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto s = load_state_for_read(opts);
  auto it = kd_value_rlower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{s.state(), std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto DB::rkeys_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto s = load_state_for_read(opts);
  auto begin_it = from.empty()
      ? kd_end(s->key_dir, s->kd_ctx())
      : kd_upper_bound(s->key_dir, from, s->kd_ctx());
  auto end_it = kd_begin(s->key_dir, s->kd_ctx());
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
  // Publishes scrape idle read caches; with no writes there are none, and
  // what the last writes retired stays pinned by whichever thread went idle
  // last. vacuum and stats are the entry points that keep running without
  // writes, so they scrape too.
  scrape_read_caches();

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

  // Find the highest-fragmentation sealed file above threshold. Tombstones
  // count as kept, not as fragmentation: compaction copies every one of them,
  // so a file holding nothing but tombstones has nothing to reclaim.
  std::uint32_t target_id{};
  double worst_frag = 0.0;
  for (const auto [fid, fs] : stats_snap) {
    if (fid == active_id) continue;
    if (fs.total_bytes == 0) continue;
    const auto kept = fs.live_bytes + fs.tombstone_bytes;
    const auto frag = 1.0 - static_cast<double>(kept) /
                                static_cast<double>(fs.total_bytes);
    if (frag > worst_frag && frag > opts.fragmentation_threshold) {
      worst_frag = frag;
      target_id = fid;
    }
  }

  if (target_id == 0 && worst_frag == 0.0) return false;

  const auto &target = *stats_snap.get(target_id);

  // Fast path: nothing in the file needs keeping — skip the scan and drop it.
  // A tombstone does need keeping even with no live key left: dropping it
  // would let recovery resurrect a Put it shadows in an older file.
  if (target.live_bytes == 0 && target.tombstone_bytes == 0) {
    vacuum_remove_file(target_id);
    return true;
  }

  // All files with live entries are compacted (sealed→sealed). Returns false
  // when the scan finds nothing to reclaim.
  return vacuum_compact_file(target_id);
}

#pragma endregion

#pragma region File rotation

// Drops the old active file's preallocated tail, opens it read-only,
// dispatches hint generation, and opens a new writable active file.
// Caller must sync the active file before calling if durability is required.
void DB::rotate_active_file(TransientEngineState &t,
                            const std::shared_ptr<const EngineState> &) {
  t.active_file().shrink_to_fit();
  auto read_only_old = openDataFileForRead(t.active_file().path(), io_backend_, pool_,
                                         t.active_file_id());
  const auto stem = make_data_file_stem();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_rotate_file_creation);
#endif
  const auto new_file_id = t.reserve_file_id();
  auto new_file = createDataFileForWrite(
      dir_, stem, ".data", rotation_threshold_, io_backend_, pool_,
      new_file_id);
  t.apply_rotate_file(read_only_old, std::move(new_file), new_file_id);
  // The sealed file's frames become evictable and the new file's pinned.
  if (pool_) pool_->set_active_file(new_file_id);
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
// Lexicographic order over raw keys, matching the key directory's order.
// Used both by sorted hint generation and by the ranged recovery merge.
static auto recovery_key_cmp(std::span<const std::byte> a,
                             std::span<const std::byte> b) noexcept -> int {
  const auto n = std::min(a.size(), b.size());
  if (n != 0) {
    const auto c = std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

auto DB::open_hint_or_rebuild(const std::shared_ptr<DataFile> &data_file,
                              const std::filesystem::path &hint_path,
                              HintOpener open) -> HintFile {
  try {
    return open(hint_path);
  } catch (const std::exception &e) {
    // flush_hints_for leaves an existing hint alone, so the damaged one has
    // to go first. Nothing is lost by removing it: it is unreadable either
    // way, and a rebuild that does not finish here leaves the file hint-less,
    // which the next open regenerates through the same scan.
    std::filesystem::remove(hint_path);
    (void)flush_hints_for(data_file, hint_path.parent_path());
    auto hint = open(hint_path);
    // The tail-drop recovery_prepare_files does for a hint-less file is not
    // repeated here: a file that had a hint at all was sealed, and sealing
    // already gave its preallocated tail back.
    std::fprintf(stderr,
                 "bytecask: rebuilt hint file '%s' from its data file: %s\n",
                 hint_path.string().c_str(), e.what());
    return hint;
  }
}

auto DB::flush_hints_for(const std::shared_ptr<DataFile> &file,
                              const std::filesystem::path &dir)
    -> std::optional<Offset> {
  const auto stem = file->path().stem().string();
  const auto hint_path = dir / (stem + ".hint");
  const auto tmp_path = dir / (stem + ".hint.tmp");

  if (std::filesystem::exists(hint_path)) {
    return std::nullopt;
  }

  auto hint = HintFile::OpenForWrite(tmp_path);

  // Put and Delete entries go out sorted by key, restoring what BC-088
  // introduced and 45d0e90 traded away for O(1) working memory. Sorting costs
  // one buffer per data file — bounded by max_file_bytes, not by the database
  // — and lets recovery bulk-load a B+ tree instead of inserting key by key.
  // Every reader resolves entries by sequence rather than by position, so the
  // order is free to serve the key directory being built.
  //
  // They are staged here first. Keys live in one arena: the scanner's key
  // span dies on the next advance, and a vector per entry would cost an
  // allocation per key.
  struct Staged {
    std::uint64_t seq;
    std::uint64_t file_off;
    std::uint32_t val_size;
    std::uint32_t key_off;
    std::uint32_t key_len;
    EntryType type;
  };
  std::vector<Staged> staged;
  std::vector<std::byte> key_arena;

  auto committed = scan_committed(*file);
  auto it = committed.begin();
  for (; it != std::default_sentinel; ++it) {
    const auto &[entry, entry_off] = *it;
    if (entry.entry_type == EntryType::BulkBegin ||
        entry.entry_type == EntryType::BulkEnd) {
      // Structural markers carry no key; recovery ignores them. They stay in
      // scan order ahead of the sorted run.
      hint.append(entry.sequence, entry.entry_type, entry_off, {}, 0);
      continue;
    }
    if (entry.entry_type == EntryType::RangeDel) {
      // A range tombstone's key is a range bound, not a key of the file, so
      // it must not join the sorted run — deduplicating by key would let it
      // collide with a real key. Recovery's tombstone handling is order
      // independent, so writing these first is safe.
      hint.append_range_del(entry.sequence, entry_off, entry.key,
                            entry.value);
    } else {
      staged.push_back({entry.sequence, entry_off,
                        narrow<std::uint32_t>(entry.value.size()),
                        narrow<std::uint32_t>(key_arena.size()),
                        narrow<std::uint32_t>(entry.key.size()),
                        entry.entry_type});
      key_arena.insert(key_arena.end(), entry.key.begin(), entry.key.end());
    }
  }

  auto key_of = [&](const Staged &e) {
    return std::span<const std::byte>{key_arena.data() + e.key_off, e.key_len};
  };
  // Key ascending, and within a key sequence descending, so the first entry
  // for each key is the authoritative one.
  std::ranges::sort(staged, [&](const Staged &a, const Staged &b) {
    const auto c = recovery_key_cmp(key_of(a), key_of(b));
    return c < 0 || (c == 0 && a.seq > b.seq);
  });
  // BC-088 also deduplicated, keeping the highest-sequence entry per key.
  // That is not done here: file_stats.min_sequence and max_sequence are
  // rebuilt at recovery from the entries the hint file still holds, and
  // ChangeIterator selects data files by that range. Dropping an entry can
  // raise min_sequence above a sequence the data file really contains, and
  // replication would then skip the file. Recovering it needs the bounds in
  // the hint header, which is a format change.
  for (const auto &e : staged)
    hint.append(e.seq, e.type, e.file_off, key_of(e), e.val_size);

  hint.close();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_hint_rename);
#endif
  std::filesystem::rename(tmp_path, hint_path);
  return it.committed_offset();
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
  flush_hints(*load_state());
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
      const auto existing = kd_get(snap->key_dir, entry.key, snap->kd_ctx());
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
      const auto sz = entry_size(entry.key.size(), 0);
      result.total_bytes += sz;
      result.tombstone_bytes += sz;
      track_seq(entry.sequence);
      break;
    }
    case EntryType::RangeDel: {
      std::ignore =
          dest_file.append_entry(entry.sequence, EntryType::RangeDel,
                                 entry.key, entry.value);
      const auto sz = entry_size(entry.key.size(), entry.value.size());
      result.total_bytes += sz;
      result.tombstone_bytes += sz;
      track_seq(entry.sequence);
      break;
    }
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      std::ignore =
          dest_file.append_entry(entry.sequence, entry.entry_type, {}, {});
      // A marker occupies a header and a CRC in the compacted file, exactly
      // as it did in the file the write path produced. Counting it keeps
      // total_bytes equal to the file's size on disk — which is what
      // recovery seeds it from — and keeps every published offset inside it.
      result.total_bytes += kHeaderSize + kCrcSize;
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
                             std::shared_ptr<DataFile> new_sealed_file,
                             std::uint32_t dest_file_id) {
  auto current = load_state_for_write();
  auto t = current->transient();
  t.apply_vacuum(old_file_id, scan, std::move(new_sealed_file), dest_file_id);

  store_state(current, std::move(t).persistent());
}

// Unlinks the old data and hint files from the filesystem. Existing readers
// continue via their open fds (POSIX: pread succeeds on unlinked files).
void DB::vacuum_unlink_old_file(
    const std::shared_ptr<const EngineState> &snap, std::uint32_t file_id) {
#ifdef BYTECASK_TESTING
  // The state is committed and the compacted file is on disk; failing here
  // is the kill inside vacuum's publish window, with the source left behind.
  FAULT_INJECTION(io_vacuum_compact_unlink);
#endif
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
auto DB::vacuum_compact_file(std::uint32_t file_id) -> bool {
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
    auto tmp_file = createDataFileForWrite(
        dir_, stem, ".data.tmp", rotation_threshold_,
        stagingBackend(io_backend_));
    scan = vacuum_scan_and_copy(snap, old_file, *tmp_file, file_id);
    tmp_file->sync();
    tmp_file->shrink_to_fit();
  }

  // Nothing to reclaim: every byte in this file is live data, a tombstone or
  // a batch marker, and compaction must preserve all three. Publishing an
  // identical file would churn I/O, and at fragmentation_threshold 0 the file
  // would qualify again on the next call and never converge — fragmentation
  // counts live and tombstone bytes as kept, but batch markers as
  // reclaimable, so a file whose only dead bytes are markers still qualifies.
  const auto old_total = snap->file_stats.get(file_id)->total_bytes;
  if (scan.total_bytes >= old_total) {
    std::error_code ec;
    std::filesystem::remove(tmp_data_path, ec);
    return false;
  }

#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_vacuum_compact_rename);
#endif
  // Exclusive placement, not std::filesystem::rename: vacuum stages its copy
  // holding only vacuum_mu_, so a concurrent rotation can mint this stem
  // between here and the staging create. renameDataFileExclusive refuses the
  // target instead of replacing it.
  renameDataFileExclusive(tmp_data_path, final_data_path);
#ifdef BYTECASK_TESTING
  // Class G in docs/correctness_validation.md: the rename completed and the
  // process did not get to confirm it. The compacted file is on disk under its
  // final name while the old one is still the published state's.
  FAULT_INJECTION(io_vacuum_compact_post_rename);
#endif

  // Reserve the destination id before opening the file, so the file carries
  // its engine file_id from construction — the buffer pool keys frames by it.
  // Reading next_file_id_ outside the barrier would race rotation, which
  // mints from the same counter, so the reservation is published first and
  // the commit below consumes it.
  std::uint32_t dest_file_id = 0;
  {
    WriteBarrier barrier{*this};
    auto current = load_state_for_write();
    auto t = current->transient();
    dest_file_id = t.reserve_file_id();
    store_state(current, std::move(t).persistent());
  }

  auto new_file = openDataFileForRead(final_data_path, io_backend_, pool_,
                                      dest_file_id);
  flush_hints_for(new_file, dir_);

  {
    WriteBarrier barrier{*this};
    vacuum_commit(file_id, scan, new_file, dest_file_id);
  }
  // Bytes reclaimed = the shrinkage, not old_total - live_bytes: the compacted
  // file also carries the tombstones and markers that had to be preserved.
  counters_.vacuum_bytes_reclaimed.fetch_add(
      static_cast<std::int64_t>(old_total - scan.total_bytes),
      std::memory_order_relaxed);
  counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
  vacuum_unlink_old_file(snap, file_id);
  return true;
}

// Removes a sealed file that has no live keys. No I/O scan needed — just
// commit the state change and unlink the files. Called under vacuum_mu_.
void DB::vacuum_remove_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  auto old_total = snap->file_stats.get(file_id)->total_bytes;
  {
    WriteBarrier barrier{*this};
    VacuumScanResult empty{};
    // No new sealed file, so no id is consumed.
    vacuum_commit(file_id, empty, nullptr, 0);
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
  scrape_read_caches();  // see vacuum()
  auto s = load_state();
  const auto *pool = pool_ ? &pool_->counters() : nullptr;
  const auto reclaim = KeyDirTree::reclamation_gauges();
  std::int64_t open_files = 0;
  for (auto it = s->files.begin(); it != std::default_sentinel; ++it)
    ++open_files;
  return {
      // What a pool has to be sized against: the key directory is resident
      // and not a cache. Multiply by the bytes/key your key shape measures
      // (scripts/run_memory_profile.py; about 50 for typical keys).
      {"bytecask.keydir_keys", narrow<std::int64_t>(s->key_dir.size())},
      // Gauges: versions of the key directory alive right now (1 = only the
      // published state) and retired nodes those older versions still pin.
      {"bytecask.keydir_versions_live", narrow<std::int64_t>(reclaim.versions)},
      {"bytecask.keydir_nodes_parked",
       narrow<std::int64_t>(reclaim.parked_nodes)},
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
      {"bytecask.commit_wait_blocked",
       counters_.commit_wait_blocked.load(std::memory_order_relaxed)},
      {"bytecask.disk_reads",
       counters_.disk_reads.load()},
      {"bytecask.disk_read_bytes",
       counters_.disk_read_bytes.load()},
      {"bytecask.pool_hits", pool ? pool->hits.load() : 0},
      {"bytecask.pool_misses", pool ? pool->misses.load() : 0},
      {"bytecask.pool_fills",
       pool ? pool->fills.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_evictions",
       pool ? pool->evictions.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_frames_total", pool ? pool->frames_total : 0},
      {"bytecask.pool_frames_resident",
       pool ? pool->frames_resident.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_direct_io_fallbacks",
       pool ? pool->direct_io_fallbacks.load(std::memory_order_relaxed) : 0},
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
  WriteBarrier barrier{*this};
  auto current = load_state_for_write();
  auto t = current->transient();
  t.apply_set_mode(mode);
  store_state(current, std::move(t).persistent());
}

void DB::deem_as_degraded(std::string reason) {
  store_state(load_state()->degraded_copy(std::move(reason)));
  // A waiter on a publication that will now never come re-checks and sees
  // the degrade. Not under durable_mu_ here — no caller holds it.
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

void DB::resume() {
  if (!is_degraded()) return;

  WriteBarrier barrier{*this};
  auto current = load_state_for_write();
  if (!current->degraded) return;  // re-check under lock

  // The failed flush left one or two heads derived from the published
  // state alive in head_. The key directory derives a version only from
  // the end of its chain, so drop them now — ~FlushRole would only do it at
  // the end of the barrier — and the resumed state is derived from the
  // published one with those heads already reclaimed.
  store_head(current);

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
      // Batch markers are collected like everything else. They have no key
      // directory effect — apply_resume says so once, in its switch — but
      // they do consume sequences, and the file's min/max bounds have to
      // count them or resume reports a min_sequence above a sequence the
      // file really contains. Hint files carry markers for the same reason,
      // so dropping them here made resume and a cold open disagree.
      // A range tombstone carries its exclusive upper bound in the value,
      // and apply_resume needs both bounds to suppress the range.
      auto range_end = entry.entry_type == EntryType::RangeDel
                           ? std::vector<std::byte>{entry.value.begin(),
                                                    entry.value.end()}
                           : std::vector<std::byte>{};
      committed.push_back({entry.sequence, entry.entry_type, entry_off,
                           narrow<std::uint32_t>(entry.value.size()),
                           {entry.key.begin(), entry.key.end()},
                           std::move(range_end)});
      // Every entry yielded lies below the iterator's committed offset, so
      // recording it here keeps valid_offset in step with `committed` even
      // when the next entry is corrupt and ++iter throws.
      valid_offset = iter.committed_offset();
      ++iter;
    }
  } catch (const std::system_error &) {
    // An I/O error says nothing about the bytes — the next attempt may read
    // them fine. Truncating on it would destroy data over a transient fault.
    throw;  // stays degraded
  } catch (const std::runtime_error &) {
    // The scan stopped at an entry that does not parse. valid_offset is the
    // end of the last committed entry or batch before it; whether that is
    // a torn tail to trim or damage to refuse is decided below.
  }
  const auto active_stats = current->file_stats.get(old_file_id);
  const auto published_extent = active_stats ? active_stats->total_bytes : 0;
  // resume() trims what a failed write left behind: bytes appended but never
  // published. Everything below the published extent was acknowledged, and a
  // scan that stops short of it has found damage in data readers have
  // already been served, not a torn tail. There is no consistent state to
  // resume into from there, so resume() refuses — before truncating, so the
  // file is left exactly as it was found and the engine stays degraded.
  // This is detection, not repair: resume() makes no promise about what a
  // damaged file still holds, only that it will not truncate acknowledged
  // bytes or report success over them.
  if (valid_offset < published_extent) {
    throw std::runtime_error{std::format(
        "resume: active file '{}' is damaged at offset {}, inside data "
        "already published (up to {}); refusing to truncate it",
        file.path().string(), valid_offset, published_extent)};
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
  auto read_only_old = openDataFileForRead(file.path(), io_backend_, pool_,
                                         old_file_id);

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
  const auto new_file_id = t.reserve_file_id();
  auto new_file = createDataFileForWrite(
      dir_, stem, ".data", rotation_threshold_, io_backend_, pool_,
      new_file_id);

  // Build and publish new state. Replay scanned entries into key_dir so that
  // entries on disk but not yet in EngineState become visible.
  t.apply_resume(old_file_id, committed, valid_offset);
  t.apply_rotate_file(std::move(read_only_old), std::move(new_file),
                      new_file_id);
  if (pool_) pool_->set_active_file(new_file_id);
  // All entries recovered from disk were previously synced.
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
  t.apply_clear_degraded();
  auto resumed = std::move(t).persistent();
  validate_state_consistency(*resumed);
  store_state(current, std::move(resumed));
  // Writers appended since the failed flush have all been told; new ones
  // start clean.
  std::lock_guard<std::mutex> lk{durable_mu_};
  flush_error_ = nullptr;
}

void DB::wait_published(std::uint64_t sequence) const {
  std::unique_lock<std::mutex> lk{durable_mu_};
  durable_cv_.wait(lk, [&] {
    const auto s = load_state();
    return s->next_seq > sequence || s->degraded;
  });
}

auto DB::durable_sequence(std::uint64_t min_sequence,
                         std::chrono::milliseconds timeout) const
    -> std::uint64_t {
  auto baseline = load_state()->durable_seq;
  if (min_sequence == 0 || baseline >= min_sequence
      || timeout <= std::chrono::milliseconds{0}) {
    return baseline;
  }

  std::unique_lock<std::mutex> lk{durable_mu_};
  durable_cv_.wait_for(lk, timeout, [&] {
    return load_state()->durable_seq >= min_sequence;
  });
  return load_state()->durable_seq;
}

auto DB::create_manifest() -> FileManifest {
  std::shared_ptr<const EngineState> manifest_state;
  std::uint64_t through_seq;
  {
    WriteBarrier barrier{*this};

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

    // Capture the published state under write_mu_ and the flush role —
    // nothing can be appended or published in between, so the snapshot
    // has no entries beyond through_sequence.
    manifest_state = load_state();
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
// The hot path is a single relaxed load of state_time_ (plain MOV on x86)
// plus an owner-pointer compare. The snapshot is refreshed only when the
// last write timestamp exceeds staleness_tolerance (session mode:
// tolerance=0, refreshes on every write), or when this thread's cache
// currently holds a different DB instance's generation. Returns a
// reference to the thread-local snapshot. The snapshot stays alive until
// the same thread calls load_state_for_read again, so callers must not
// stash the reference across a second load_state_for_read call.
auto DB::read_cache() -> ReadCacheSlot & {
  // Per-thread slot — thread-exit destructor is intentional: it is what
  // leaves the registry.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local ReadCacheSlot tl;
#pragma clang diagnostic pop
  return tl;
}

auto DB::load_state_for_read(const ReadOptions &opts) const
    -> ReadStateGuard {
  auto &slot = read_cache();
  auto *e = slot.claim();
  // owner identifies which DB instance the entry belongs to: without it a
  // thread that reads from two DBs could see one DB's generation while
  // querying the other. An entry of another DB is dropped, not reused —
  // this DB must never take ownership of another DB's state.
  if (e == nullptr || e->owner != this) {
    if (e == nullptr) {
      e = new ReadCacheEntry();
    } else {
      e->state.reset();
    }
    e->owner = this;
    e->last_write_time = 0;
  }
  const auto wt = state_time_.load(std::memory_order_relaxed);
  const auto tolerance =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          opts.staleness_tolerance)
          .count();
  if (wt - e->last_write_time > tolerance) {
    e->state = load_state();
    e->last_write_time = wt;
  }
  e->used_epoch.store(ReadCacheRegistry::instance().epoch(),
                      std::memory_order_relaxed);
  return ReadStateGuard{slot, e};
}

void DB::scrape_read_caches() const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto period =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          kReadCacheScrapePeriod)
          .count();
  if (now - last_scrape_ns_.load(std::memory_order_relaxed) < period) return;
  last_scrape_ns_.store(now, std::memory_order_relaxed);
  ReadCacheRegistry::instance().scrape(kReadCacheIdleEpochs);
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
  // Durability before visibility: a published state never contains a
  // sync=true write that fdatasync has not confirmed.
  if (new_state->sync_requested_seq > new_state->durable_seq) {
    deem_as_degraded(std::format(
        "invariant violation: publishing sync_requested_seq {} above "
        "durable_seq {}",
        new_state->sync_requested_seq, new_state->durable_seq));
    return;
  }

  const auto durable_advanced =
      new_state->durable_seq > old_state->durable_seq;
  const auto published_advanced =
      new_state->next_seq > old_state->next_seq;
  const auto became_degraded =
      new_state->degraded && !old_state->degraded;

#ifndef NDEBUG
  if constexpr (kKeyDirReadsKeys) {
    // A blind key directory holds locations only: sequences and sizes are
    // in the records, and reading every one on every publish would make
    // each debug commit O(n) in disk reads (and fill the buffer pool from
    // files no reader asked for). What the locations alone show is checked:
    // every entry starts inside its file's committed extent (invariant P).
    for (auto it = kd_value_lower_bound(new_state->key_dir, {},
                                        new_state->kd_ctx(/*verify=*/false));
         it != std::default_sentinel; ++it) {
      const auto loc = *it;
      if (const auto *fs = new_state->file_stats.get(loc.file_id());
          fs != nullptr && loc.file_offset() >= fs->total_bytes) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {} starts at {} but the "
            "file's committed extent is {}",
            loc.file_id(), loc.file_offset(), fs->total_bytes));
        return;
      }
    }
  } else {
    // Debug-only O(n) checks, sharing one walk: next_seq > max(all key_dir
    // sequences), and every entry inside its file's committed extent
    // (invariant P — see "View and span lifetimes" in CONTRACT.md).
    std::uint64_t max_seq = 0;
    for (auto it = kd_begin(new_state->key_dir, new_state->kd_ctx(/*verify=*/false));
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (entry.sequence() > max_seq) max_seq = entry.sequence();
      const auto entry_end = entry.file_offset() +
                             entry_size(key_span.size(), entry.value_size());
      if (const auto *fs = new_state->file_stats.get(entry.file_id());
          fs != nullptr && entry_end > fs->total_bytes) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {} ends at {} but the file's "
            "committed extent is {}",
            entry.file_id(), entry_end, fs->total_bytes));
        return;
      }
    }
    if (max_seq > 0 && new_state->next_seq <= max_seq) {
      deem_as_degraded(std::format(
          "invariant violation: next_seq {} <= max key_dir sequence {}",
          new_state->next_seq, max_seq));
      return;
    }
  }
#endif

  store_state(std::move(new_state));
#ifdef BYTECASK_TESTING
  if (test_between_publish_stores_) test_between_publish_stores_();
#endif
  state_time_.store(now_ns(), std::memory_order_release);

  // The version this publish superseded is freed once nothing holds it;
  // an idle thread's read cache is the holder nothing else can reach.
  scrape_read_caches();

  if (became_degraded) {
    counters_.degraded_transitions.fetch_add(1, std::memory_order_relaxed);
  }
  // Wakes the sequence waiters: durable_sequence on a durable advance, and
  // wait_published on any publication, which is why a nosync publish that
  // advances no durable sequence notifies too. (A failed flush publishes
  // through the raw store under durable_mu_ and finish_flush notifies for
  // it; deem_as_degraded notifies for itself.)
  if (durable_advanced || published_advanced || became_degraded) {
    { std::lock_guard<std::mutex> lk{durable_mu_}; }
    durable_cv_.notify_all();
  }
}

void DB::store_initial_state(std::shared_ptr<EngineState> s) {
  store_head(s);
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

  // 2. Nothing published owes an fdatasync.
  if (s.sync_requested_seq > s.durable_seq) {
    throw std::runtime_error{std::format(
        "state consistency: sync_requested_seq {} > durable_seq {}",
        s.sync_requested_seq, s.durable_seq)};
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
  for (auto it = kd_begin(s.key_dir, s.kd_ctx(/*verify=*/false));
       it != std::default_sentinel;
       ++it) {
    auto [key_span, entry] = *it;
    if (!s.files.contains(entry.file_id())) {
      throw std::runtime_error{std::format(
          "state consistency: key references file_id {} not in registry",
          entry.file_id())};
    }
    // Offset containment: a published entry must lie inside the committed
    // extent of the file it names. This is what lets resume() shorten the
    // active file under lock-free readers — with use_mmap the mapping stays
    // put and only mmap_end_ moves, so an entry above the new extent would
    // leave a reader's span addressing a page beyond EOF. See "View and span
    // lifetimes" in CONTRACT.md.
    const auto size = entry_size(key_span.size(), entry.value_size());
    const auto entry_end = entry.file_offset() + size;
    if (const auto *fs = s.file_stats.get(entry.file_id());
        fs != nullptr && entry_end > fs->total_bytes) {
      throw std::runtime_error{std::format(
          "state consistency: key in file_id {} ends at {} but the file's "
          "committed extent is {}",
          entry.file_id(), entry_end, fs->total_bytes)};
    }
    computed_live[entry.file_id()] += size;
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

namespace {

// True if every committed entry of `copy` appears in `source`, in order, with
// the same sequence, type, key and value. Reads both files end to end; only
// called for two files whose sequences overlap, which a healthy directory
// never has.
auto is_compaction_of(const std::filesystem::path &copy,
                      const std::filesystem::path &source) -> bool {
  const auto copy_file = openDataFileForRead(copy);
  const auto source_file = openDataFileForRead(source);
  auto src = scan_committed(*source_file);
  auto it = src.begin();
  for (const auto &[entry, entry_off] : scan_committed(*copy_file)) {
    for (;; ++it) {
      if (it == std::default_sentinel) return false;
      const auto &cand = (*it).first;
      if (cand.sequence > entry.sequence) return false;
      if (cand.sequence == entry.sequence &&
          cand.entry_type == entry.entry_type && cand.key == entry.key &&
          cand.value == entry.value) {
        ++it;
        break;
      }
    }
  }
  return true;
}

// The first two files whose sequence ranges overlap, by id. Every file is
// sequence-disjoint from every other (D18) except for what an interrupted
// vacuum leaves, so this is nullopt on every healthy open.
auto find_sequence_overlap(const EngineState &s)
    -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
  struct Range {
    std::uint64_t min, max;
    std::uint32_t file_id;
  };
  std::vector<Range> ranges;
  for (const auto [fid, fs] : s.file_stats) {
    if (fs.min_sequence > 0) {
      ranges.push_back({fs.min_sequence, fs.max_sequence, fid});
    }
  }
  std::ranges::sort(ranges, {}, &Range::min);
  const Range *widest = nullptr;
  for (const auto &r : ranges) {
    if (widest != nullptr && r.min <= widest->max) {
      return std::pair{widest->file_id, r.file_id};
    }
    if (widest == nullptr || r.max > widest->max) widest = &r;
  }
  return std::nullopt;
}

} // namespace

// Recovery, with one repair: the compacted file of an interrupted vacuum is
// deleted and recovery runs again. Vacuum renames its compacted file into
// place, commits, and only then unlinks the source, so a kill in between
// leaves both on disk under the same sequences. Recovery stops on that — by
// SequenceOverlap as soon as two files claim one key under one sequence, or
// by the file ranges check once it is done, for a pair that shares no key —
// and recovery_undo_interrupted_vacuum decides what the pair is. Each pass
// removes one file or throws, so this ends. A healthy open runs one pass and
// pays only for the ranges check, which is over files, not keys. See
// docs/vacuum_crash_recovery_design.md.
auto DB::recovery_open(const Options &opts) -> EngineState {
  for (;;) {
    std::filesystem::path a;
    std::filesystem::path b;
    {
      EngineState s;
      auto files = recovery_prepare_files(s);
      // Maps the file ids in a SequenceOverlap back to paths once s is gone.
      const auto registered = s.files;
      const auto path_of = [&](std::uint32_t id) {
        return (*registered.get(id))->path();
      };
      try {
#if defined(BYTECASK_KEYDIR_BLIND)
        s = recovery_load_streams(std::move(s), std::move(files),
                                  opts.recovery_threads,
                                  opts.fail_recovery_on_crc_errors);
#elif defined(BYTECASK_USE_BTREE)
        s = recovery_load_ranged(std::move(s), std::move(files),
                                 opts.recovery_threads,
                                 opts.fail_recovery_on_crc_errors);
#else
        s = recovery_load_parallel(std::move(s), std::move(files),
                                   opts.recovery_threads,
                                   opts.fail_recovery_on_crc_errors);
#endif
        const auto overlap = find_sequence_overlap(s);
        if (!overlap) return s;
        a = path_of(overlap->first);
        b = path_of(overlap->second);
      } catch (const SequenceOverlap &e) {
        a = path_of(e.file_a);
        b = path_of(e.file_b);
      }
    }
    recovery_undo_interrupted_vacuum(a, b);
    // The next pass numbers files afresh, so an id can now name a different
    // file. The pool keys frames by id; a fresh one holds none of the old.
    if (pool_) pool_ = std::make_shared<BufferPool>(opts.buffer_pool);
  }
}

// Two files hold entries under the same sequences. The one shape the engine
// produces is vacuum's: a compacted file C and its source S, both on disk
// because the kill came between the rename and the unlink. Vacuum is
// committed on disk only once S is gone, so C is unfinished work and is
// deleted — as recovery already deletes a leftover .data.tmp. That is safe on
// exactly what is checked here, that every entry of C is also in S: nothing
// is lost. Deleting S instead would also assume S's other entries are dead,
// which nothing here proves. Anything that is not such a pair is corruption.
void DB::recovery_undo_interrupted_vacuum(const std::filesystem::path &a,
                                          const std::filesystem::path &b) {
  // C holds a subset of S, so it is never the larger file. Two files of one
  // size that pass are identical, and either may go.
  const auto a_smaller =
      std::filesystem::file_size(a) <= std::filesystem::file_size(b);
  const auto &copy = a_smaller ? a : b;
  const auto &source = a_smaller ? b : a;
  if (!is_compaction_of(copy, source)) {
    throw std::runtime_error{std::format(
        "bytecask: corrupt database — data files '{}' and '{}' hold entries "
        "under the same sequence numbers, and neither is a compacted copy of "
        "the other",
        a.string(), b.string())};
  }
  std::fprintf(stderr,
               "bytecask: removing '%s', the compacted copy of '%s' left by "
               "an interrupted vacuum\n",
               copy.string().c_str(), source.string().c_str());
  auto hint = copy;
  hint.replace_extension(".hint");
  std::filesystem::remove(hint);
  std::filesystem::remove(copy);
}

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

  // Name order, so file ids are a function of the directory's contents and
  // not of the order the filesystem lists it in.
  std::vector<std::filesystem::path> data_paths;
  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    if (dir_entry.path().extension() == ".data") {
      data_paths.push_back(dir_entry.path());
    }
  }
  std::ranges::sort(data_paths);

  std::vector<RecoveredFile> files;
  auto files_t = s.files.transient();

  for (const auto &p : data_paths) {
    const auto file_id = s.next_file_id++;
    auto data_file =
        openDataFileForRead(p, io_backend_, pool_, file_id);

    const auto hint_path = dir_ / (p.stem().string() + ".hint");
    if (!std::filesystem::exists(hint_path)) {
      // A file without a hint was the active file at the last shutdown. A
      // clean close already dropped its preallocated tail; after a crash it
      // still carries it. Drop it now so its physical size is its logical
      // size, as for every other sealed file — file_size below is what
      // seeds total_bytes for vacuum.
      const auto end = flush_hints_for(data_file, dir_);
      if (end && *end < std::filesystem::file_size(p)) {
        data_file.reset();
        std::filesystem::resize_file(p, *end);
        // Same file_id as the open above, deliberately. Under the buffer
        // pool that open's hint scan may have admitted frames under this id,
        // but resize_file only drops a tail: every byte below *end is
        // unchanged, and no reader addresses anything above it. Frames past
        // the new end are orphans CLOCK reclaims.
        data_file = openDataFileForRead(p, io_backend_, pool_, file_id);
      }
    }
    files_t.set(file_id, data_file);

    files.push_back({file_id, std::move(data_file), hint_path,
                     std::filesystem::file_size(p)});
  }

  s.files = std::move(files_t).persistent();
  return files;
}


// BC_RECOVERY_PHASES=1 prints how long each recovery phase took, so the two
// key directories can be compared phase by phase rather than in total.
namespace {
struct RecoveryPhaseLog {
  bool on{};
  std::chrono::steady_clock::time_point last{};
  explicit RecoveryPhaseLog() {
    const char *e = std::getenv("BC_RECOVERY_PHASES");
    on = e && *e == '1';
    last = std::chrono::steady_clock::now();
  }
  void mark(const char *name) {
    if (!on) return;
    const auto now = std::chrono::steady_clock::now();
    std::fprintf(stderr, "  phase %-22s %7.1f ms\n", name,
                 std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
  }
};
} // namespace

// Builds a RecoveryResult from a subset of hint files.
// Each worker calls this independently — no shared mutable state.
// When strict is false, corrupt or unreadable hint files are skipped
// with a warning instead of throwing.
auto DB::recovery_build_from_hints(std::span<RecoveredFile> files, bool strict)
    -> RecoveryResult {
  std::uint64_t max_seq = 0;
  auto t = RecoveryKeyDirTree{}.transient();
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
      auto hint =
          open_hint_or_rebuild(data_file, hint_path, &HintFile::OpenForRead);
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
        file_fs.tombstone_bytes +=
            tombstone_size(he->entry_type, he->key.size(), he->end_key.size());

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
    } catch (const SequenceOverlap &) {
      // Not a damaged file to skip: recovery_open resolves or rejects it.
      throw;
    } catch (const std::exception &e) {
      if (strict) throw;
      std::fprintf(stderr,
                   "bytecask: skipping data file for hint '%s' — could not "
                   "read it or rebuild it from the data file: %s\n",
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

  // merge consumes both inputs; a and b are ours, moved in by the caller.
  auto merged = RecoveryKeyDirTree::merge(std::move(a.key_dir), std::move(b.key_dir),
                                  seq_resolver);

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
      [](RecoveryKeyDirTree &tree,
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
auto DB::recovery_load_parallel(EngineState s,
                                std::vector<RecoveredFile> files,
                                unsigned recovery_threads, bool strict)
    -> EngineState {
  RecoveryPhaseLog plog;

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

  // Threads are joined. Propagate any worker exceptions now. A
  // SequenceOverlap propagates in both modes: it is not a file to skip.
  for (const auto &err : worker_errors) {
    if (!err) continue;
    try {
      std::rethrow_exception(err);
    } catch (const SequenceOverlap &) {
      throw;
    } catch (...) {
      if (strict) throw;
      // lenient: warning already emitted inside recovery_build_from_hints
    }
  }
#endif

  auto &final_result = queue[0];
  plog.mark("build + fan-in merge");

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
  plog.mark("live_bytes pass");

  // Phase 5: assembly.
  s.key_dir = key_dir_from_recovered(std::move(final_result.key_dir));
  s.next_seq = final_result.max_seq + 1;
  s.file_stats = std::move(final_result.file_stats);
  return s;
}

#ifdef BYTECASK_USE_BTREE
static auto recovery_span_of(const Key &k) noexcept
    -> std::span<const std::byte> {
  return {k.begin(), k.size()};
}

// Runs body(0..n) on n threads. An exception leaving a std::thread is
// std::terminate, so each thread catches its own and the first is rethrown
// here once every thread is joined: a failure in any phase — a range
// merge's SequenceOverlap among them — is an exception from DB::open.
template <typename Body>
static void recovery_parallel_for(unsigned n, Body &body) {
#ifdef BYTECASK_SINGLE_THREADED
  for (unsigned i = 0; i < n; ++i) body(i);
#else
  if (n <= 1) {
    if (n == 1) body(0);
    return;
  }
  std::vector<std::exception_ptr> errors(n);
  {
    std::vector<std::jthread> threads;
    threads.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
      threads.emplace_back([&body, &errors, i] {
        try {
          body(i);
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    }
  }
  for (const auto &err : errors) {
    if (err) std::rethrow_exception(err);
  }
#endif
}


// Builds a RecoveryResult by merging sorted hint runs straight into a bulk
// loader, instead of inserting key by key into a transient.
//
// recovery_build_from_hints costs a descent, a slot shift and a split every
// fanout inserts, per key; per-phase timing put it at 90 of the B+ tree's
// 128 ms at 1M keys and 4 threads, which is the whole of its deficit against
// the radix tree. A bulk loader writes each key exactly once with no descent
// and no split, but it needs its keys in ascending order — which is what a
// sorted hint file gives.
//
// Requires the sorted hint files flush_hints_for writes. A sorted hint file is
// (batch markers and range tombstones, in scan order) followed by one run of
// Put and Delete entries sorted by key. The merge checks the order it is
// given and throws rather than handing a bulk loader keys it cannot take.
auto DB::recovery_build_sorted(std::span<RecoveredFile> files, bool strict)
    -> RecoveryResult {
  std::uint64_t max_seq = 0;
  std::map<Key, std::uint64_t> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::unordered_map<std::uint32_t, FileStats> fstats_scratch;
  for (const auto &rf : files)
    fstats_scratch.emplace(rf.file_id, FileStats{0, rf.total_bytes});

  auto note = [&](std::uint32_t file_id, const HintEntry &he) {
    const auto seq = he.sequence;
    if (seq > max_seq) max_seq = seq;
    auto &fs = fstats_scratch[file_id];
    if (fs.min_sequence == 0 || seq < fs.min_sequence) fs.min_sequence = seq;
    if (seq > fs.max_sequence) fs.max_sequence = seq;
    fs.tombstone_bytes +=
        tombstone_size(he.entry_type, he.key.size(), he.end_key.size());
  };

  // One cursor per hint file, parked on its next Put or Delete. The scanner
  // reads the hint file's own buffer or mapping, so the files must outlive
  // the merge. A scanner's entries last only until its next call, so the
  // cursor owns the entries it keeps.
  struct Cursor {
    HintFile::Scanner scanner;
    std::uint32_t file_id;
    HintRecord cur;       // best entry for the key it sits on
    HintRecord lookahead; // first entry of the following key
    bool has_cur{false};
    bool has_lookahead{false};
  };
  std::vector<HintFile> open_hints;
  std::vector<Cursor> cursors;
  open_hints.reserve(files.size());
  cursors.reserve(files.size());

  // Phase A: the head of each file — markers and range tombstones — is read
  // up front, so every range tombstone this worker owns is known before any
  // Put is admitted.
  for (auto &[file_id, data_file, hint_path, tb] : files) {
    try {
      open_hints.push_back(
          open_hint_or_rebuild(data_file, hint_path, &HintFile::OpenForMerge));
      Cursor c{open_hints.back().make_scanner(), file_id, {}, {}, false, false};
      while (auto he = c.scanner.next()) {
        note(file_id, *he);
        if (he->entry_type == EntryType::RangeDel) {
          range_tombstones.push_back(
              {Key{he->key}, Key{he->end_key}, he->sequence});
          continue;
        }
        if (he->entry_type == EntryType::BulkBegin ||
            he->entry_type == EntryType::BulkEnd) {
          continue;
        }
        c.lookahead.assign(*he);
        c.has_lookahead = true;
        break;
      }
      cursors.push_back(std::move(c));
    } catch (const std::exception &e) {
      if (strict) throw;
      std::fprintf(stderr,
                   "bytecask: skipping data file for hint '%s' — could not "
                   "read it or rebuild it from the data file: %s\n",
                   hint_path.string().c_str(), e.what());
    }
  }

  // Phase B: merge the runs. For each key the highest sequence across every
  // file decides; a Delete that wins drops the key, and every Delete is
  // recorded so other workers' entries for that key are suppressed too.
  KeyDirBulkLoader out;
  std::vector<std::size_t> matches;
  matches.reserve(cursors.size());
  std::vector<std::byte> prev_key;
  bool have_prev = false;

  // The next Put or Delete in this file, counting every entry it steps over
  // towards the file's sequence bounds.
  auto next_data = [&](Cursor &c) -> std::optional<HintEntry> {
    while (auto he = c.scanner.next()) {
      note(c.file_id, *he);
      if (he->entry_type == EntryType::BulkBegin ||
          he->entry_type == EntryType::BulkEnd)
        continue;
      if (he->entry_type == EntryType::RangeDel) {
        // Range tombstones only ever sit in the head of a sorted file.
        throw std::runtime_error{
            "bytecask: range tombstone inside a sorted hint run"};
      }
      return *he;
    }
    return std::nullopt;
  };

  // Parks the cursor on the highest-sequence entry of its next distinct key.
  // Hints are no longer deduplicated, so one key can repeat within a run;
  // collapsing here is what keeps the merged stream strictly ascending.
  auto load = [&](Cursor &c) {
    if (c.has_lookahead) {
      std::swap(c.cur, c.lookahead);
      c.has_lookahead = false;
    } else if (auto he = next_data(c)) {
      c.cur.assign(*he);
    } else {
      c.has_cur = false;
      return;
    }
    c.has_cur = true;
    while (auto he = next_data(c)) {
      const auto ord = recovery_key_cmp(he->key, c.cur.key);
      if (ord < 0) {
        throw std::runtime_error{
            "bytecask: hint file is not sorted — recovery_build_sorted needs "
            "the sorted hint files flush_hints_for writes"};
      }
      if (ord == 0) {
        if (he->sequence > c.cur.sequence) c.cur.assign(*he);
        continue;
      }
      c.lookahead.assign(*he);
      c.has_lookahead = true;
      break;
    }
  };
  for (auto &c : cursors) load(c);

  // A heap, not a scan over the cursors: there is one cursor per hint file
  // this worker owns, and at one recovery thread that is every file in the
  // database. A linear scan costs O(files) per key, which at 10M keys and
  // ~180 files made recovery 3x slower than the radix tree; the heap makes
  // it O(log files).
  const auto ahead = [&](std::size_t a, std::size_t b) {
    return recovery_key_cmp(cursors[a].cur.key, cursors[b].cur.key) > 0;
  };
  std::vector<std::size_t> heap;
  heap.reserve(cursors.size());
  for (std::size_t i = 0; i < cursors.size(); ++i)
    if (cursors[i].has_cur) heap.push_back(i);
  std::ranges::make_heap(heap, ahead);

  while (!heap.empty()) {
    std::ranges::pop_heap(heap, ahead);
    const auto best = heap.back();
    heap.pop_back();

    // `key` is the cursor's own copy: valid until that cursor is loaded,
    // which happens only once this key is done.
    const auto key = std::span<const std::byte>{cursors[best].cur.key};
    auto winner_at = best;
    matches.clear();
    matches.push_back(best);
    // Every other cursor sitting on the same key is adjacent at the top.
    while (!heap.empty() &&
           recovery_key_cmp(cursors[heap.front()].cur.key, key) == 0) {
      std::ranges::pop_heap(heap, ahead);
      const auto i = heap.back();
      heap.pop_back();
      matches.push_back(i);
      if (cursors[i].cur.sequence > cursors[winner_at].cur.sequence)
        winner_at = i;
    }
    const auto &winner = cursors[winner_at].cur;
    const auto winner_file = cursors[winner_at].file_id;
    // A Delete that wins its file is recorded even when another file's Put
    // outranks it here, so workers that never saw this key still learn of it.
    // A Delete that loses within its own file cannot matter: the entry that
    // beat it is newer and lives in the same file.
    for (const auto i : matches) {
      if (cursors[i].cur.entry_type != EntryType::Delete) continue;
      auto &slot = tombstones[Key{key}];
      if (cursors[i].cur.sequence > slot) slot = cursors[i].cur.sequence;
    }

    if (winner.entry_type == EntryType::Put) {
      auto suppressed = false;
      for (const auto &rt : range_tombstones) {
        if (winner.sequence >= rt.seq) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt.start)) < 0) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt.end)) >= 0) continue;
        suppressed = true;
        break;
      }
      if (!suppressed) {
        if (have_prev && recovery_key_cmp(key, prev_key) <= 0) {
          throw std::runtime_error{
              "bytecask: merged hint keys are not ascending"};
        }
        prev_key.assign(key.begin(), key.end());
        have_prev = true;
        out.append(key, KeyDirEntry::make(winner.sequence, winner.file_offset,
                                          winner_file, winner.value_size));
      }
    }

    for (const auto i : matches) {
      load(cursors[i]);
      if (cursors[i].has_cur) {
        heap.push_back(i);
        std::ranges::push_heap(heap, ahead);
      }
    }
  }

  auto fstats_t = PersistentU32Map<FileStats>{}.transient();
  for (const auto &[id, fs] : fstats_scratch) fstats_t.set(id, fs);

  return {std::move(out).finish(), std::move(tombstones),
          std::move(range_tombstones), max_seq,
          std::move(fstats_t).persistent()};
}

// ---------------------------------------------------------------------------
// Range-partitioned recovery — the B+ tree path.
//
// recovery_load_parallel folds the W per-worker trees together pairwise, so
// every surviving key is rewritten once per level of the fan-in: log2(W)
// passes over the key set, the last of them serial. That fold is what stops
// recovery scaling past a few threads.
//
// Here the fold is replaced by a partition. The workers' trees are cut at the
// same splitters, each range is merged by one thread straight into a bulk
// loader, and the results — disjoint and already in order — are concatenated.
// Every key is written exactly once, in parallel, and the serial tail
// rebuilds only the levels above the leaves.
//
// The splitters are free: a B+ tree's inner separators are already a uniform
// one-per-leaf sample of its keys, so each worker offers a few hundred of
// them and the pooled sample is cut into R even parts. Only the *output* has
// to be disjoint — each worker still reads whatever keys its own files held —
// so nothing is shuffled and the phase costs two barriers.
//
// This is not a strategy the radix tree would want. A trie is canonical: two
// radix trees agree on the shape of any subtree whose key set they agree on,
// so their merge adopts whole subtrees by pointer and does work proportional
// to the overlap rather than to N. Rebuilding the key set into range slices
// would throw that away, which is why the radix path keeps the pairwise fold.
// ---------------------------------------------------------------------------
auto DB::recovery_load_ranged(EngineState s, std::vector<RecoveredFile> files,
                              unsigned recovery_threads, bool strict)
    -> EngineState {
  RecoveryPhaseLog plog;
  if (files.empty()) {
    return s;
  }

#ifdef BYTECASK_SINGLE_THREADED
  const auto W = 1u;
  (void)recovery_threads;
#else
  auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
  if (W == 0) W = 1;
#endif

  const auto parallel_for = [](unsigned n, auto &&body) {
    recovery_parallel_for(n, body);
  };

  // Phase 1: round-robin file assignment.
  std::vector<std::vector<RecoveredFile>> worker_files(W);
  for (unsigned i = 0; i < files.size(); ++i) {
    worker_files[i % W].push_back(std::move(files[i]));
  }

  // Phase 2: one tree per worker, in parallel.
  std::vector<RecoveryResult> parts(W);
  std::vector<std::exception_ptr> worker_errors(W, nullptr);
  // Hint files are sorted, so a worker merges its files' runs straight into
  // a bulk loader rather than inserting key by key.
  parallel_for(W, [&](unsigned i) {
    try {
      parts[i] = recovery_build_sorted(worker_files[i], strict);
    } catch (...) {
      worker_errors[i] = std::current_exception();
    }
  });
  for (const auto &err : worker_errors) {
    if (err) {
      if (strict) std::rethrow_exception(err);
      // lenient: warning already emitted inside recovery_build_sorted
    }
  }
  plog.mark("build_sorted");

  // Phase 3: union the parts' metadata, and pool their separators into R
  // splitters. Both are O(W × files) or O(W × R) — nothing here touches a key.
  std::map<Key, std::uint64_t> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::unordered_map<std::uint32_t, FileStats> fstats;
  std::uint64_t max_seq = 0;
  for (auto &part : parts) {
    max_seq = std::max(max_seq, part.max_seq);
    for (const auto &[key, seq] : part.tombstones) {
      auto &existing = tombstones[key];
      if (seq > existing) existing = seq;
    }
    range_tombstones.insert(
        range_tombstones.end(),
        std::make_move_iterator(part.range_tombstones.begin()),
        std::make_move_iterator(part.range_tombstones.end()));
    part.range_tombstones.clear();
    // Files are assigned round-robin, so no two parts report the same one.
    for (const auto [fid, fs] : part.file_stats) fstats.emplace(fid, fs);
  }

  const auto R = W;
  std::vector<std::vector<std::byte>> splitters;
  if (R > 1) {
    // Oversampling smooths over the leaves a worker happens to leave short.
    constexpr std::size_t kOversample = 8;
    std::vector<std::vector<std::byte>> pool;
    for (const auto &part : parts) {
      auto seps = part.key_dir.sample_separators(R * kOversample);
      pool.insert(pool.end(), std::make_move_iterator(seps.begin()),
                  std::make_move_iterator(seps.end()));
    }
    std::sort(pool.begin(), pool.end());
    splitters.reserve(R - 1);
    for (unsigned r = 1; r < R && !pool.empty(); ++r) {
      const auto idx = static_cast<std::size_t>(r) * pool.size() / R;
      if (idx < pool.size()) splitters.push_back(pool[idx]);
    }
    // Repeated splitters would only produce empty ranges.
    splitters.erase(std::unique(splitters.begin(), splitters.end()),
                    splitters.end());
  }
  const auto ranges = static_cast<unsigned>(splitters.size()) + 1;
  plog.mark("union + splitters");

  // Phase 4: one thread per range. Each merges that slice of every part,
  // resolves by sequence, applies the pooled tombstones, and bulk-loads the
  // survivors. live_bytes is accumulated here rather than in a pass of its
  // own — the keys are already in hand.
  struct RangeOut {
    KeyDirLeafRun run;
    std::unordered_map<std::uint32_t, std::uint64_t> live;
  };
  std::vector<RangeOut> outs(ranges);

  parallel_for(ranges, [&](unsigned r) {
    const auto lo = r == 0 ? std::span<const std::byte>{}
                           : std::span<const std::byte>{splitters[r - 1]};
    const auto *hi = r + 1 < ranges ? &splitters[r] : nullptr;
    const auto hi_span =
        hi ? std::span<const std::byte>{*hi} : std::span<const std::byte>{};

    // Only the range tombstones that reach into this slice can matter.
    std::vector<const RangeTombstone *> rts;
    for (const auto &rt : range_tombstones) {
      const auto start = recovery_span_of(rt.start);
      const auto end = recovery_span_of(rt.end);
      const auto starts_below_hi =
          hi == nullptr || recovery_key_cmp(start, hi_span) < 0;
      const auto ends_above_lo =
          lo.empty() || recovery_key_cmp(end, lo) > 0;
      if (starts_below_hi && ends_above_lo) rts.push_back(&rt);
    }

    // Point tombstones are sorted the way this merge emits keys, so one
    // cursor walks them in lockstep instead of a lookup per key.
    auto tomb = lo.empty() ? tombstones.begin()
                           : tombstones.lower_bound(Key{lo});

    // One cursor per part, each holding the key it currently sits on. The
    // key spans into the iterator's own buffer and stays valid until that
    // iterator advances, so a cursor is only refreshed right after its own
    // increment.
    struct Cursor {
      RecoveryKeyDirIter it;
      std::span<const std::byte> key;
      KeyDirEntry entry{};
      bool live{false};
    };
    auto refresh = [&](Cursor &c) {
      c.live = false;
      if (c.it == std::default_sentinel) return;
      const auto [k, e] = *c.it;
      if (hi != nullptr && recovery_key_cmp(k, hi_span) >= 0) return;
      c.key = k;
      c.entry = e;
      c.live = true;
    };
    std::vector<Cursor> cursors;
    cursors.reserve(parts.size());
    for (const auto &part : parts) {
      cursors.push_back(Cursor{part.key_dir.lower_bound(lo), {}, {}, false});
      refresh(cursors.back());
    }

    KeyDirBulkLoader out;
    auto &live = outs[r].live;
    std::vector<std::size_t> matches;
    matches.reserve(cursors.size());

    // A linear scan picks the smallest key: W comparisons per emitted key,
    // where W is bounded by Options::recovery_threads. A loser tree would
    // make it log2(W), which is only worth the code at thread counts far
    // above a core count.
    while (true) {
      auto best = cursors.size();
      for (std::size_t i = 0; i < cursors.size(); ++i) {
        if (!cursors[i].live) continue;
        if (best == cursors.size() ||
            recovery_key_cmp(cursors[i].key, cursors[best].key) < 0)
          best = i;
      }
      if (best == cursors.size()) break;

      const auto key = cursors[best].key;
      auto winner = cursors[best].entry;
      matches.clear();
      matches.push_back(best);
      for (std::size_t i = 0; i < cursors.size(); ++i) {
        if (i == best || !cursors[i].live) continue;
        if (recovery_key_cmp(cursors[i].key, key) != 0) continue;
        matches.push_back(i);
        if (kde_newer(cursors[i].entry, winner)) winner = cursors[i].entry;
      }

      while (tomb != tombstones.end() &&
             recovery_key_cmp(recovery_span_of(tomb->first), key) < 0)
        ++tomb;
      auto drop = tomb != tombstones.end() &&
                  recovery_key_cmp(recovery_span_of(tomb->first), key) == 0 &&
                  winner.sequence() < tomb->second;
      if (!drop) {
        for (const auto *rt : rts) {
          if (winner.sequence() >= rt->seq) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->start)) < 0) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->end)) >= 0) continue;
          drop = true;
          break;
        }
      }
      if (!drop) {
        out.append(key, winner);
        live[winner.file_id()] += entry_size(key.size(), winner.value_size());
      }

      // `key` points into cursors[best]'s buffer — nothing advances until here.
      for (const auto i : matches) {
        ++cursors[i].it;
        refresh(cursors[i]);
      }
    }

    outs[r].run = std::move(out).seal();
  });

  plog.mark("range merge");

  // Phase 5: concatenate the range-disjoint slices and fold in live_bytes.
  std::vector<KeyDirLeafRun> runs;
  runs.reserve(outs.size());
  std::unordered_map<std::uint32_t, std::uint64_t> live_accum;
  for (auto &o : outs) {
    for (const auto &[fid, bytes] : o.live) live_accum[fid] += bytes;
    runs.push_back(std::move(o.run));
  }
  auto key_dir = KeyDirBulkLoader::concat(std::move(runs));
  plog.mark("concat");

  auto fstats_t = PersistentU32Map<FileStats>{}.transient();
  for (auto &[fid, fs] : fstats) {
    const auto it = live_accum.find(fid);
    fs.live_bytes = it != live_accum.end() ? it->second : 0ULL;
    fstats_t.set(fid, fs);
  }

  s.key_dir = key_dir_from_recovered(std::move(key_dir));
  s.next_seq = max_seq + 1;
  s.file_stats = std::move(fstats_t).persistent();
  return s;
}
#endif  // BYTECASK_USE_BTREE

#ifdef BYTECASK_KEYDIR_BLIND
// ---------------------------------------------------------------------------
// Hint-stream recovery — the blind key directory's path.
//
// Two blind trees cannot be merged without reading their keys back from the
// data files, so this path never builds one per worker. It merges the hint
// files themselves, which are sorted and carry every key:
//
//   1. Per file, in parallel: open it (rebuilding a damaged hint), collect its
//      range tombstones and sequence bounds, and record a fence — the key and
//      position of an entry — at the start of every frame and every 4 KiB
//      inside one. A fence sits only on the first entry of a key, so seeking
//      to one never skips an older duplicate of it.
//   2. Pool the fence keys and cut them into one range per thread.
//   3. Per range, in parallel: seek every file to its last fence below the
//      range and k-way merge its slice. The newest entry of a key wins
//      (kde_newer, so two files under one sequence still throw
//      SequenceOverlap); a winning Delete, or a range tombstone newer than
//      the winner, drops the key. Survivors go straight into a blind loader.
//   4. Concatenate the ranges.
//
// Every file is merged in every range, so a Delete in one file and a Put in
// another meet in the same merge: no tombstone map crosses threads.
// ---------------------------------------------------------------------------
auto DB::recovery_load_streams(EngineState s, std::vector<RecoveredFile> files,
                               unsigned recovery_threads, bool strict)
    -> EngineState {
  RecoveryPhaseLog plog;
  if (files.empty()) {
    return s;
  }
#ifdef BYTECASK_SINGLE_THREADED
  const auto W = 1u;
  (void)recovery_threads;
#else
  auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
  if (W == 0) W = 1;
#endif
  constexpr std::size_t kFenceStep = 4096;

  using Position = HintFile::Scanner::Position;
  struct Fence {
    std::vector<std::byte> key; // a copy: the scanner's frame is reused
    Position at;
  };
  struct FileRun {
    std::optional<HintFile> hint; // empty: skipped (lenient open)
    Position data_start;          // where the first Put or Delete starts
    std::vector<Fence> fences;
    std::vector<RangeTombstone> range_tombstones;
    FileStats stats;
    std::uint64_t max_seq{0};
  };
  std::vector<FileRun> runs(files.size());

  // Phase 1: fences, range tombstones and sequence bounds, per file.
  auto scan_file = [&](std::size_t f) {
    auto &run = runs[f];
    const auto &rf = files[f];
    run.stats = FileStats{0, rf.total_bytes};
    try {
      run.hint.emplace(open_hint_or_rebuild(rf.data_file, rf.hint_path,
                                            &HintFile::OpenForMerge));
    } catch (const std::exception &e) {
      if (strict) throw;
      std::fprintf(stderr,
                   "bytecask: skipping data file for hint '%s' — could not "
                   "read it or rebuild it from the data file: %s\n",
                   rf.hint_path.string().c_str(), e.what());
      return;
    }
    auto scanner = run.hint->make_scanner();
    bool in_data = false;
    Position next_fence{};
    std::vector<std::byte> prev_key;
    bool have_prev = false;
    for (;;) {
      const auto at = scanner.position();
      const auto he = scanner.next();
      if (!he) break;
      if (run.stats.min_sequence == 0 || he->sequence < run.stats.min_sequence)
        run.stats.min_sequence = he->sequence;
      run.stats.max_sequence = std::max(run.stats.max_sequence, he->sequence);
      run.stats.tombstone_bytes +=
          tombstone_size(he->entry_type, he->key.size(), he->end_key.size());
      run.max_seq = std::max(run.max_seq, he->sequence);
      switch (he->entry_type) {
      case EntryType::BulkBegin:
      case EntryType::BulkEnd:
        continue;
      case EntryType::RangeDel:
        // Range tombstones only ever sit in the head of a sorted file.
        if (in_data)
          throw std::runtime_error{
              "bytecask: range tombstone inside a sorted hint run"};
        run.range_tombstones.push_back(
            {Key{he->key}, Key{he->end_key}, he->sequence});
        continue;
      case EntryType::Put:
      case EntryType::Delete:
        break;
      }
      if (!in_data) {
        in_data = true;
        run.data_start = at;
      }
      const bool new_key =
          !have_prev || recovery_key_cmp(he->key, prev_key) != 0;
      if (!new_key) continue;
      // A new frame always passes: its position sorts after any fence
      // target inside the previous one.
      if (at >= next_fence) {
        run.fences.push_back({{he->key.begin(), he->key.end()}, at});
        next_fence = {at.frame, (at.offset / kFenceStep + 1) * kFenceStep};
      }
      prev_key.assign(he->key.begin(), he->key.end());
      have_prev = true;
    }
    if (!in_data) run.data_start = scanner.position();
  };
  auto scan_worker = [&](unsigned w) {
    for (std::size_t f = w; f < files.size(); f += W) scan_file(f);
  };
  recovery_parallel_for(W, scan_worker);
  plog.mark("scan + fences");

  std::uint64_t max_seq = 0;
  std::vector<RangeTombstone> range_tombstones;
  for (auto &run : runs) {
    max_seq = std::max(max_seq, run.max_seq);
    range_tombstones.insert(
        range_tombstones.end(),
        std::make_move_iterator(run.range_tombstones.begin()),
        std::make_move_iterator(run.range_tombstones.end()));
  }

  // Phase 2: splitters from the pooled fences, which sample every file's keys
  // every 4 KiB of hint — even by bytes, close to even by keys.
  std::vector<std::vector<std::byte>> splitters;
  if (W > 1) {
    std::vector<std::span<const std::byte>> pool;
    for (const auto &run : runs)
      for (const auto &fence : run.fences) pool.push_back(fence.key);
    std::ranges::sort(pool, [](auto a, auto b) {
      return recovery_key_cmp(a, b) < 0;
    });
    for (unsigned r = 1; r < W && !pool.empty(); ++r) {
      const auto k = pool[static_cast<std::size_t>(r) * pool.size() / W];
      if (splitters.empty() ||
          recovery_key_cmp(k, std::span<const std::byte>{splitters.back()}) > 0)
        splitters.emplace_back(k.begin(), k.end());
    }
  }
  const auto ranges = static_cast<unsigned>(splitters.size()) + 1;
  plog.mark("splitters");

  // Phase 3: one k-way merge per range, into blind leaves.
  struct RangeOut {
    btree_detail::LeafRun<BlindRef> run;
    std::unordered_map<std::uint32_t, std::uint64_t> live;
  };
  std::vector<RangeOut> outs(ranges);
  auto merge_range = [&](unsigned r) {
    const auto lo = r == 0 ? std::span<const std::byte>{}
                           : std::span<const std::byte>{splitters[r - 1]};
    const bool bounded = r + 1 < ranges;
    const auto hi = bounded ? std::span<const std::byte>{splitters[r]}
                            : std::span<const std::byte>{};
    auto below_hi = [&](std::span<const std::byte> k) {
      return !bounded || recovery_key_cmp(k, hi) < 0;
    };

    std::vector<const RangeTombstone *> rts;
    for (const auto &rt : range_tombstones) {
      const auto start = recovery_span_of(rt.start);
      const auto end = recovery_span_of(rt.end);
      if (below_hi(start) && (lo.empty() || recovery_key_cmp(end, lo) > 0))
        rts.push_back(&rt);
    }

    // A scanner's entries last only until its next call, so the cursor owns
    // the entries it keeps.
    struct Cursor {
      HintFile::Scanner scanner;
      std::uint32_t file_id;
      HintRecord cur;       // newest entry of the key it sits on
      HintRecord lookahead; // first entry of the following key
      bool has_cur{false};
      bool has_lookahead{false};
    };
    std::vector<Cursor> cursors;
    cursors.reserve(files.size());
    auto next_data = [](Cursor &c) -> std::optional<HintEntry> {
      while (auto he = c.scanner.next()) {
        if (he->entry_type == EntryType::Put ||
            he->entry_type == EntryType::Delete)
          return *he;
        if (he->entry_type == EntryType::RangeDel)
          throw std::runtime_error{
              "bytecask: range tombstone inside a sorted hint run"};
      }
      return std::nullopt;
    };
    // Parks the cursor on the newest entry of its next distinct key inside
    // the range, or empties it.
    auto load = [&](Cursor &c) {
      c.has_cur = false;
      if (c.has_lookahead) {
        std::swap(c.cur, c.lookahead);
        c.has_lookahead = false;
      } else if (auto he = next_data(c)) {
        c.cur.assign(*he);
      } else {
        return;
      }
      if (!below_hi(c.cur.key)) return;
      c.has_cur = true;
      while (auto he = next_data(c)) {
        const auto ord = recovery_key_cmp(he->key, c.cur.key);
        if (ord < 0)
          throw std::runtime_error{
              "bytecask: hint file is not sorted — recovery needs the sorted "
              "hint files flush_hints_for writes"};
        if (ord == 0) {
          if (he->sequence > c.cur.sequence) c.cur.assign(*he);
          continue;
        }
        c.lookahead.assign(*he);
        c.has_lookahead = true;
        break;
      }
    };

    for (std::size_t f = 0; f < files.size(); ++f) {
      const auto &run = runs[f];
      if (!run.hint) continue;
      // The last fence below lo: every entry at or above lo lies after it.
      auto start = run.data_start;
      if (!lo.empty()) {
        const auto it = std::ranges::lower_bound(
            run.fences, lo,
            [](std::span<const std::byte> a, std::span<const std::byte> b) {
              return recovery_key_cmp(a, b) < 0;
            },
            &Fence::key);
        if (it != run.fences.begin()) start = std::prev(it)->at;
      }
      Cursor c{run.hint->make_scanner(), files[f].file_id, {}, {}, false, false};
      c.scanner.seek(start);
      // Step over the keys below lo.
      if (!lo.empty()) {
        while (auto he = next_data(c)) {
          if (recovery_key_cmp(he->key, lo) >= 0) {
            c.lookahead.assign(*he);
            c.has_lookahead = true;
            break;
          }
        }
      }
      load(c);
      if (c.has_cur) cursors.push_back(std::move(c));
    }

    const auto ahead = [&](std::size_t a, std::size_t b) {
      return recovery_key_cmp(cursors[a].cur.key, cursors[b].cur.key) > 0;
    };
    std::vector<std::size_t> heap(cursors.size());
    for (std::size_t i = 0; i < cursors.size(); ++i) heap[i] = i;
    std::ranges::make_heap(heap, ahead);

    BlindBulkLoader<kBlindLeafBytes> out{kBlindRecoveryFillMin,
                                         kBlindRecoveryFillMax};
    auto &live = outs[r].live;
    std::vector<std::size_t> matches;
    auto entry_of = [](const Cursor &c) {
      return KeyDirEntry::make(c.cur.sequence, c.cur.file_offset, c.file_id,
                               c.cur.value_size);
    };
    while (!heap.empty()) {
      std::ranges::pop_heap(heap, ahead);
      const auto first = heap.back();
      heap.pop_back();
      // The cursor's own copy: valid until that cursor is loaded, which
      // happens only once this key is done.
      const auto key = std::span<const std::byte>{cursors[first].cur.key};
      auto winner = first;
      auto winner_entry = entry_of(cursors[first]);
      matches.assign(1, first);
      while (!heap.empty() &&
             recovery_key_cmp(cursors[heap.front()].cur.key, key) == 0) {
        std::ranges::pop_heap(heap, ahead);
        const auto i = heap.back();
        heap.pop_back();
        matches.push_back(i);
        const auto e = entry_of(cursors[i]);
        if (kde_newer(e, winner_entry)) {
          winner = i;
          winner_entry = e;
        }
      }

      auto keep = cursors[winner].cur.entry_type == EntryType::Put;
      for (const auto *rt : rts) {
        if (!keep) break;
        if (winner_entry.sequence() >= rt->seq) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt->start)) < 0) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt->end)) >= 0) continue;
        keep = false;
      }
      if (keep) {
        out.append(key, BlindRef{winner_entry.file_id(),
                                 static_cast<std::uint32_t>(
                                     winner_entry.file_offset())});
        live[winner_entry.file_id()] +=
            entry_size(key.size(), winner_entry.value_size());
      }

      for (const auto i : matches) {
        load(cursors[i]);
        if (cursors[i].has_cur) {
          heap.push_back(i);
          std::ranges::push_heap(heap, ahead);
        }
      }
    }
    outs[r].run = std::move(out).seal();
  };
  recovery_parallel_for(ranges, merge_range);
  plog.mark("range merge");

  // Phase 4: concatenate the ranges and fold in live bytes.
  std::vector<btree_detail::LeafRun<BlindRef>> leaf_runs;
  leaf_runs.reserve(outs.size());
  std::unordered_map<std::uint32_t, std::uint64_t> live_accum;
  for (auto &o : outs) {
    for (const auto &[fid, bytes] : o.live) live_accum[fid] += bytes;
    leaf_runs.push_back(std::move(o.run));
  }
  auto fstats_t = PersistentU32Map<FileStats>{}.transient();
  for (std::size_t f = 0; f < files.size(); ++f) {
    auto fs = runs[f].stats;
    const auto it = live_accum.find(files[f].file_id);
    fs.live_bytes = it != live_accum.end() ? it->second : 0ULL;
    fstats_t.set(files[f].file_id, fs);
  }
  s.key_dir = BlindBulkLoader<kBlindLeafBytes>::concat(std::move(leaf_runs));
  plog.mark("concat");
  s.next_seq = max_seq + 1;
  s.file_stats = std::move(fstats_t).persistent();
  return s;
}
#endif  // BYTECASK_KEYDIR_BLIND

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

  WriteBarrier barrier{*this};

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
      auto err_s = current->degraded_copy(
          "ingest append IO error: call resume() to recover.");
      store_state(std::move(err_s));
      std::rethrow_exception(ex);
    }

    // Phase 3: if rotation needed, sync before sealing.
    if (needs_rotation) {
      try {
        file.sync();
        t.apply_sync(chunk_max_seq);
      } catch (...) {
        auto err_s = current->degraded_copy(
            "ingest rotation fdatasync failed: call resume() to recover.");
        store_state(std::move(err_s));
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
      auto err_s = current->degraded_copy(
          "ingest rotation fdatasync failed: call resume() to recover.");
      store_state(std::move(err_s));
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
    auto err_s = current->degraded_copy(
        "ingest fdatasync failed: call resume() to recover.");
    store_state(std::move(err_s));
    throw;
  }

  assert(t.active_file().size() <= rotation_threshold_);
  store_state(current, std::move(t).persistent());
}

#pragma endregion


} // namespace bytecask
