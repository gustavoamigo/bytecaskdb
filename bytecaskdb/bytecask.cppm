// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — C++23 module: public API surface and engine implementation

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
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
// VacuumOptions::retain_after value meaning no restriction: -1 as an
// unsigned sequence, above every sequence the engine assigns.
export inline constexpr std::uint64_t kNoRetention =
    std::numeric_limits<std::uint64_t>::max();

export struct VacuumOptions {
  // Minimum fragmentation ratio a sealed file must exceed to be eligible for
  // vacuum: 1 − (live + tombstone + marker bytes) / total bytes, the share
  // of dead Puts compaction is sure to reclaim. Range [0.0, 1.0].
  double fragmentation_threshold{0.5};
  // Entries with a sequence above this are kept even when dead: a dead Put
  // or a tombstone vacuum could otherwise drop is copied into the compacted
  // file, and a file holding one is never removed whole. changes_since from
  // any sequence >= retain_after then still yields the complete history.
  // Vacuum has to be coordinated with replication: the replication service
  // passes the lowest durable_sequence() among the followers it counts on,
  // on every node, since a follower can become leader. kNoRetention (-1),
  // the default, means no restriction.
  std::uint64_t retain_after{kNoRetention};
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
// key_size is u16 on disk; value_size is 28 bits in a packed KeyDirEntry.
export inline constexpr std::uint32_t kMaxKeySize = 65535;
export inline constexpr std::uint32_t kMaxValueSize = KeyDirEntry::kMaxValueSize;

// Most bytes one write (a plan, or an atomic batch in an ingest slice) may
// append, markers included. A write never spans two files, and a file is only
// rotated once it has crossed max_file_bytes, so an entry starts below
// kMaxFileBytes + kMaxBatchBytes. The two are chosen to keep that below the
// 32-bit offset a KeyDirEntry holds.
export inline constexpr std::uint64_t kMaxBatchBytes = std::uint64_t{1} << 30;
export inline constexpr std::uint64_t kMaxFileBytes = std::uint64_t{3} << 30;
static_assert(kMaxFileBytes + kMaxBatchBytes - 1 <= KeyDirEntry::kMaxFileOffset);

#ifdef BYTECASK_TESTING
// The limit apply_batch and ingest enforce in test builds. A test lowers it,
// never raises it, to reach the boundary without a gigabyte plan, and
// restores it. Not synchronised: set it while no write is running.
export inline std::uint64_t test_max_batch_bytes = kMaxBatchBytes;
#endif

[[nodiscard]] inline auto max_batch_bytes() noexcept -> std::uint64_t {
#ifdef BYTECASK_TESTING
  return test_max_batch_bytes;
#else
  return kMaxBatchBytes;
#endif
}

// Size a commit group may take the active file to: one past the largest
// packed offset, so every entry the group writes starts inside it.
#ifdef BYTECASK_TESTING
// Lowered by a test, never raised, so a group splits without writing 4 GiB.
// Not synchronised: set it while no write is running.
export inline std::uint64_t test_max_group_file_bytes =
    std::uint64_t{KeyDirEntry::kMaxFileOffset} + 1;
#endif

[[nodiscard]] inline auto max_group_file_bytes() noexcept -> std::uint64_t {
#ifdef BYTECASK_TESTING
  return test_max_group_file_bytes;
#else
  return std::uint64_t{KeyDirEntry::kMaxFileOffset} + 1;
#endif
}

// Sensible defaults — keys live in RAM (the key directory), values go to disk.
export inline constexpr std::uint32_t kDefaultMaxKeyBytes = 4096;
export inline constexpr std::uint32_t kDefaultMaxValueBytes =
    4U * 1024 * 1024; // 4 MiB
export inline constexpr std::uint32_t kDefaultMaxHintBacklog = 4;

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

// A range [from, to) must hold at least one possible key. An empty or
// inverted one is refused rather than treated as nothing to do: from >= to is
// almost always swapped bounds, and a write that silently does nothing hides
// the bug.
inline void check_range(BytesView from, BytesView to) {
  if (!std::ranges::lexicographical_compare(from, to)) {
    throw std::invalid_argument{
        "range [from, to) is empty: from must sort before to"};
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

// Options for read operations. A read sees every write that returned before
// it began, and every write another read has already seen, on any thread.
export struct ReadOptions {
  // When true (default), all data read from underlying storage is verified
  // against its CRC32 checksum. Set to false for higher read throughput at
  // the cost of silent corruption detection.
  bool verify_checksums{true};
};

// Options passed to DB::open().
export struct Options {
  // Active-file rotation threshold in bytes (default 64 MiB). When the active
  // file reaches this size it is sealed and a new one is opened, so 0 seals
  // a file after every write. Hard ceiling: kMaxFileBytes (3 GiB); open
  // rejects more.
  std::uint64_t max_file_bytes{kDefaultRotationThreshold};
  // Number of threads used to rebuild the key directory at open time.
  // 1 selects the serial path; >1 uses file-level fan-in parallelism. 0 is
  // refused at open with std::invalid_argument.
#ifdef BYTECASK_SINGLE_THREADED
  unsigned recovery_threads{1};
#else
  unsigned recovery_threads{4};
#endif
  // A hint file that fails its CRC, or that a read fails on, is rebuilt from
  // its data file in both modes. This governs a file that still cannot be
  // indexed — its data file cannot be rescanned. When true (default): DB::open
  // throws. When false: the file is skipped; the DB opens with whatever was
  // recovered from the rest, and a warning is printed to stderr for each
  // skipped file. Neither applies to the tail of a data file without a hint,
  // which recovery_check_tail truncates or refuses the same way in both
  // modes.
  bool fail_recovery_on_crc_errors{true};
  // Initial engine mode. Leader (default) allows normal writes; Follower
  // blocks put/del/apply_batch and allows ingest().
  Mode initial_mode{Mode::Leader};
  // Maximum key size in bytes. Keys exceeding this limit are rejected with
  // std::invalid_argument. Hard ceiling: 65,535 (u16 wire format); open
  // rejects more.
  std::uint32_t max_key_bytes{kDefaultMaxKeyBytes};
  // Maximum value size in bytes. Values exceeding this limit are rejected with
  // std::invalid_argument. Hard ceiling: 268,435,455 (2^28 - 1, 28-bit packed
  // KeyDirEntry); open rejects more.
  std::uint32_t max_value_bytes{kDefaultMaxValueBytes};
  // Selects how data files are read. Pread (default) issues pread(2) per read
  // and avoids virtual address space pressure under memory contention; Mmap
  // memory-maps sealed files for zero-copy reads; BufferPool serves sealed
  // files from a bounded, engine-owned cache. See IoBackend.
  IoBackend io_backend{IoBackend::Pread};
  // Only read when io_backend == IoBackend::BufferPool.
  BufferPoolOptions buffer_pool{};
  // Maximum number of sealed files waiting for their hint file. A rotation
  // that would exceed it waits for the background worker, stalling writes
  // (reads are unaffected). This bounds both close, which writes the backlog
  // out, and the next open after a crash, which rebuilds whatever is missing.
  // 0 turns the bound off: writes never wait and close/open are unbounded.
  std::uint32_t max_hint_backlog{kDefaultMaxHintBacklog};
};

// ---------------------------------------------------------------------------
// KeyIterator — walks the key directory in ascending key order.
//
// Reads no values. A key directory that holds no key bytes (the default)
// reads each key back from its record as the iterator advances, through the
// file registry the iterator pins. Satisfies std::input_iterator.
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
                          std::uint64_t durable_sequence,
                          std::size_t max_bytes);

  auto operator++() -> ChangeIterator &;
  void operator++(int);
  auto operator*() const -> const value_type &;
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  // Implementation details hidden from public interface
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// One changes_since result, which is one ingest call: where the slice starts
// and which history it is from, and the entries, read lazily.
export struct ChangeBatch {
  ChangeHeader header;
  std::ranges::subrange<ChangeIterator, std::default_sentinel_t> entries;
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

  // Remaps the scan's live entries from old_file_id to new_sealed_file, which
  // must carry an id reserved by reserve_file_id(). A null new_sealed_file
  // removes old_file_id outright: its scan must hold no live entries, and
  // dest_file_id is ignored.
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

  // State transition: a ChangeMarker appended to the active file by a
  // promotion. Counts it in the file's stats, advances next_seq and adds it
  // to the marker list. Cannot fail.
  void apply_change_marker(std::uint64_t sequence, std::uint64_t id);

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
  // How many more ids reserve_file_id can give.
  [[nodiscard]] auto file_ids_left() const noexcept -> std::uint32_t;
#ifdef BYTECASK_TESTING
  // Starts the id counter near its ceiling, so a test reaches it without
  // creating a million files.
  void test_set_next_file_id(std::uint32_t id) { next_file_id_ = id; }
#endif

  // Returns the current next_seq value — used to capture the post-write sequence
  // before consuming the transient on sync failure (F/G).
  [[nodiscard]] auto next_seq() const noexcept -> std::uint64_t;

  [[nodiscard]] auto durable_seq() const noexcept -> std::uint64_t;

  // Commit: consume the transient and produce a new immutable EngineState.
  [[nodiscard]] auto persistent() && -> std::shared_ptr<EngineState>;

private:
  // Returns true if [from, to) differs between snap and the head: a key
  // changed, inserted or deleted since snap. Sets lost_to as
  // validate_preconditions documents. Shared by range guards and the
  // implicit W-W check of a planned del_range.
  [[nodiscard]] auto range_changed(const EngineState &snap,
                                   std::span<const std::byte> from,
                                   std::span<const std::byte> to,
                                   std::uint64_t &lost_to) const -> bool;
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
                       TransientU32Table<std::shared_ptr<DataFile>> files,
                       FileStatsTransient file_stats,
                       std::uint32_t active_file_id,
                       std::uint32_t next_file_id,
                       std::uint64_t next_seq,
                       std::uint64_t durable_seq,
                       std::uint64_t sync_requested_seq,
                       Mode mode,
                       bool degraded,
                       std::string degraded_reason,
                       std::shared_ptr<const std::vector<ChangeMarker>>
                           change_markers);

  // Adds m to the marker list, which is immutable: copied and replaced.
  void append_marker(ChangeMarker m);

  KeyDirTransient key_dir_;
  TransientU32Table<std::shared_ptr<DataFile>> files_;
  // Records this transient placed in the active file before they are
  // written, for a key directory that reads keys back (kKeyDirReadsKeys).
  PendingRecords pending_;
  FileStatsTransient file_stats_;
  std::uint32_t active_file_id_;
  std::uint32_t next_file_id_;
  std::uint64_t next_seq_;
  std::uint64_t durable_seq_;
  std::uint64_t sync_requested_seq_;
  Mode mode_;
  bool degraded_;
  std::string degraded_reason_;
  std::shared_ptr<const std::vector<ChangeMarker>> change_markers_;
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

// ---------------------------------------------------------------------------
// DbChangeMarkerMismatch — thrown by ingest for a slice of a history that is
// not this node's: the two diverged at a promotion one of them did not
// follow. Nothing was written. The remedy is a re-bootstrap from the
// source's manifest; the engine never repairs or truncates (#397).
// ---------------------------------------------------------------------------
export class DbChangeMarkerMismatch : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
  DbChangeMarkerMismatch(const DbChangeMarkerMismatch &) = default;
  auto operator=(const DbChangeMarkerMismatch &)
      -> DbChangeMarkerMismatch & = default;
  ~DbChangeMarkerMismatch() override;
};

// ---------------------------------------------------------------------------
// DbClosed — thrown by every DB operation after close(). Using a closed
// handle is a caller bug, not an I/O condition, hence std::logic_error.
// ---------------------------------------------------------------------------
export class DbClosed : public std::logic_error {
public:
  DbClosed() : std::logic_error("DB is closed") {}
  DbClosed(const DbClosed &) = default;
  auto operator=(const DbClosed &) -> DbClosed & = default;
  ~DbClosed() override;
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
//
// Closing a DB takes the same way every entry that belongs to it, in every
// slot, and waits out the slots it finds claimed: a claimed slot may be a
// read of the closing DB that puts its entry back. Nothing the DB opened
// outlives it through a cache — with the buffer pool, that is every frame.
// ---------------------------------------------------------------------------
export class DB;

inline constexpr auto kNoStateGen = std::numeric_limits<std::uint64_t>::max();

// State generations come from one process-wide counter. A read cache knows
// its DB only by address, and a new DB can reuse a destroyed one's; a
// per-DB counter would restart at 0 and could hand the new DB a generation
// the cache still holds for the old one, which would then serve the old
// DB's state.
inline auto next_state_gen() -> std::uint64_t {
  static std::atomic<std::uint64_t> gen{0};
  return gen.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct ReadCacheEntry {
  // Compared, never dereferenced. Written by its reader, read by a close
  // taking the DB's entries, hence atomic.
  std::atomic<const DB *> owner{nullptr};
  std::shared_ptr<const EngineState> state;
  // state_gen_ when state was loaded with no publication in progress;
  // kNoStateGen otherwise, which never matches.
  std::uint64_t gen{kNoStateGen};
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
  // been used for `idle_epochs` epochs.
  void scrape(std::uint64_t idle_epochs) {
    const auto now = epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
    (void)take([&](const ReadCacheEntry &e) {
      return now - e.used_epoch.load(std::memory_order_relaxed) > idle_epochs;
    });
  }

  // Takes every entry of `db` from every slot. A claimed slot is retried
  // until it is released, since the read holding it may be one of `db`
  // that puts an entry back; claims last one read, so this ends.
  void release(const DB *db) {
    while (take([db](const ReadCacheEntry &e) {
      return e.owner.load(std::memory_order_relaxed) == db;
    })) {
      std::this_thread::yield();
    }
  }

private:
  ReadCacheRegistry() = default;

  // Takes the entry `select` picks from every slot and frees it with mu_
  // released: freeing a state runs the destructors of what it held (files,
  // the buffer pool), which must not run under the lock every exiting
  // thread takes. `select` reads only the entry's atomics; the claim holder owns
  // the rest. A slot read as kInUse is skipped — its reader is in the
  // middle of a read — and a claim that lands between the load and the
  // swap makes the swap fail, so an entry is only ever freed once no reader
  // can be holding it. Returns whether a slot was skipped as kInUse.
  template <typename Select>
  [[nodiscard]] auto take(const Select &select) -> bool {
    std::vector<ReadCacheEntry *> taken;
    auto claimed = false;
    {
      std::lock_guard<std::mutex> lk{mu_};
      for (auto *slot : slots_) {
        auto *e = slot->ptr_.load(std::memory_order_acquire);
        if (e == ReadCacheSlot::in_use()) claimed = true;
        if (!ReadCacheSlot::is_entry(e) || !select(*e)) continue;
        if (slot->ptr_.compare_exchange_strong(e, ReadCacheSlot::obsolete(),
                                               std::memory_order_acq_rel)) {
          taken.push_back(e);
        } else {
          claimed = true;
        }
      }
    }
    for (auto *e : taken) delete e;
    return claimed;
  }

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

  // Calls close() if it was not called, and swallows its errors.
  ~DB();

  // Makes every write durable, trims the active file, writes the hint files
  // and releases the directory lock. Waits for in-flight writes and a running
  // vacuum to finish first. Returns normally only if every write the DB
  // acknowledged is durable and the shutdown completed; otherwise throws —
  // std::system_error for a failed fdatasync, trim or hint write, DbDegraded
  // for a degraded engine holding acknowledged writes that are not durable.
  // The DB is closed either way, and a retry cannot help: a failed
  // fdatasync leaves its pages clean (#231). Afterwards every operation
  // throws DbClosed, except mode(), is_degraded() and degraded_reason(),
  // and a second close() returns at once. Snapshots and iterators taken
  // before stay readable.
  void close();

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
  // optional fdatasync. Cannot conflict. Throws std::invalid_argument, before
  // anything is written, if from >= to.
  auto del_range(const WriteOptions &opts, BytesView from,
                BytesView to) -> CommitResult;

  // Returns true if key exists. A key directory that holds no key bytes
  // reads the key's record to confirm it.
  [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                  BytesView key) const -> bool;

  // Returns the current engine mode. Lock-free (reads published state).
  [[nodiscard]] auto mode() const noexcept -> Mode;

  // Switches the engine between Leader and Follower mode.
  // Acquires write_mu_ to ensure no in-flight write straddles the boundary.
  // Leader -> Follower first fdatasyncs, so durable_sequence() covers every
  // write acknowledged before the switch, sync or not. A failed fdatasync
  // degrades the engine and throws; the mode is unchanged.
  // Follower -> Leader is a promotion: it appends a ChangeMarker at the next
  // sequence and fdatasyncs it before the mode changes, so the first write
  // after it takes the sequence after the marker's. A failed append or sync
  // degrades, throws and leaves the mode; so does a promotion on a degraded
  // engine, with DbDegraded. Leader -> Leader writes nothing; opening in
  // Leader mode is not a promotion (#397).
  void set_mode(Mode mode);

  // Returns true if the engine has entered a degraded state. A degraded DB
  // refuses all writes but reads remain available. Call resume() to attempt
  // in-process recovery.
  [[nodiscard]] auto is_degraded() const noexcept -> bool;

  // Returns the reason the engine entered degraded state, or an empty string.
  // A copy, not a reference: the published state holding the reason can be
  // replaced, and freed, by another thread at any time. Copying may throw
  // std::bad_alloc, so this is not noexcept (#390).
  [[nodiscard]] auto degraded_reason() const -> std::string;

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
    for (const auto [id, fs] : s->file_stats.all()) result.emplace(id, fs);
    return result;
  }

  // Returns the current engine state for invariant checking.
  // Only available in test builds (BYTECASK_TESTING).
  [[nodiscard]] auto engine_state() const -> std::shared_ptr<const EngineState> {
    return load_state();
  }

  // Starts the file id counter at id, so a test reaches its ceiling without
  // creating a million files. Ids only move forward: id must not be below
  // the current counter.
  void test_set_next_file_id(std::uint32_t id) {
    WriteBarrier barrier{*this};
    auto current = load_state_for_write();
    auto t = current->transient();
    t.test_set_next_file_id(id);
    store_state(current, std::move(t).persistent());
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

  // Returns an input range of keys >= from. Reads no values; a key directory
  // that holds no key bytes reads each key's record as the range advances.
  [[nodiscard]] auto keys_from(const ReadOptions &opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

  // Returns a range of (key, value) pairs in descending key order.
  // When from is non-empty, starts at the last key <= from.
  // When from is empty, starts at the last key in the DB.
  [[nodiscard]] auto riter_from(const ReadOptions &opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;

  // Returns a range of keys in descending order. Reads keys as keys_from does.
  [[nodiscard]] auto rkeys_from(const ReadOptions &opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

  // Selects the highest-fragmentation sealed file above the threshold and
  // compacts its live entries into a new sealed file, or removes the file
  // when nothing in it is live. Returns true if a file was reclaimed, false
  // if no file qualified or none could be made smaller.
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

  // One slice of this node's history for ingest: a header (this node's
  // marker at from_sequence, and from_sequence) and the whole units above
  // from_sequence that were durable at snap, in ascending sequence order.
  // A unit is a standalone entry, a ChangeMarker or a BulkBegin..BulkEnd
  // batch; a batch straddling from_sequence is left out, since a node whose
  // position is a unit boundary below it holds it already. The slice ends
  // at the snapshot's durable sequence, or after the unit that takes it
  // past max_bytes, so it never ends inside a batch. Entries visible in the
  // snapshot but not yet fdatasync'd are excluded. The entries are read
  // lazily, one file at a time.
  [[nodiscard]] auto changes_since(const Snapshot& snap,
                                   std::uint64_t from_sequence,
                                   std::size_t max_bytes = kUnlimitedBytes)
      const -> ChangeBatch;

  // Applies one slice of a source's history, as changes_since produced it.
  // Follower mode only (std::logic_error otherwise). Checks, before anything
  // is written, with P = durable_sequence() and X = min(P, L), L the slice's
  // last sequence or header.from_sequence if it is empty:
  //   - header.from_sequence > P: a gap, std::invalid_argument;
  //   - the source's marker at X (the last ChangeMarker in the slice at or
  //     below X, or header.marker) differs from this node's marker at X: a
  //     fork, DbChangeMarkerMismatch;
  //   - a slice not strictly increasing above from_sequence, a ChangeMarker
  //     inside a batch or malformed, or a slice ending inside a batch:
  //     std::invalid_argument (#188).
  // Entries at or below P are skipped (idempotency); the rest is appended,
  // synced and published in one step, a batch never split across files.
  void ingest(const ChangeHeader &header,
              std::span<const DataEntryView> entries);

  // Returns all operational counters and gauges as a flat map.
  // Copies atomic counters (relaxed load) and reads current gauges from
  // EngineState. Designed for pull-based scraping (Prometheus, logging).
  [[nodiscard]] auto stats() const -> std::map<std::string, std::int64_t>;

private:
  explicit DB(std::filesystem::path dir, Options opts);

  // Blocks until the published state covers sequence — a fresh snapshot
  // can see the write a plan lost to — or the engine degrades or closes.
  // See apply_batch.
  void wait_published(std::uint64_t sequence) const;

  // Drains background hint tasks then writes all sealed hint files.
  void flush_hints();

  // File rotation, in two steps. prepare_rotation does the I/O: seals the
  // active file and creates the next, leaving t as it was but for the id it
  // reserves; its failure is the one the callers degrade for, by publishing
  // t. finish_rotation installs both files in t and hands the sealed one to
  // the hint worker. It does no I/O, and a failure in it leaves t half
  // changed, so it must never be published: the callers refuse writes.
  struct PreparedRotation {
    std::shared_ptr<DataFile> sealed;
    std::shared_ptr<WritableDataFile> next;
    std::uint32_t next_id{0};
  };
  [[nodiscard]] auto prepare_rotation(TransientEngineState &t)
      -> PreparedRotation;
  void finish_rotation(TransientEngineState &t, PreparedRotation r);
  // Creates the active file for file_id under a fresh stem and syncs dir_,
  // so the file's name is durable before any write into it is acknowledged.
  // checkpoint names the caller's fault injection point.
  [[nodiscard]] auto create_active_file(std::uint32_t file_id,
                                        const char *checkpoint)
      -> std::shared_ptr<WritableDataFile>;

  // Degrade — sets the engine to write-blocked state with a reason.
  // Used by store_state invariant checks; error catch blocks use
  // apply_degrade() on the transient directly.
  void deem_as_degraded(std::string reason);

  // Hint file management
  // Blocks until the hint backlog is below max_hint_backlog_ (0: returns at
  // once). Called by every path that seals a file, before it seals, so the
  // backlog never exceeds the limit.
  void wait_for_hint_backlog();
  // Queues hint generation for a sealed file on the background worker.
  void dispatch_hint(std::shared_ptr<DataFile> file);
  // Decides, before a hint is renamed into place, whether the file may end
  // at `end`, the end of its last committed record. Throws to refuse it.
  using TailCheck = std::function<void(Offset end)>;
  // Writes hint file via temp-then-rename. Batch-aware; idempotent if .hint
  // exists. Returns the offset past the last committed entry, or nullopt
  // when the hint already existed and nothing was scanned. Without a check,
  // an entry that fails its CRC throws. With one, the scan stops there as it
  // does at a zeroed header, and the check rules on what lies past it; a
  // refusal leaves no hint behind.
  static auto flush_hints_for(const std::shared_ptr<DataFile> &file,
                              const std::filesystem::path &dir,
                              const TailCheck &check = {})
      -> std::optional<Offset>;
  // recovery_prepare_files' check on a hint-less file, which is where a
  // crash can leave a torn tail. See docs/bytecask_design.md, *Recovering a
  // Hint-less File*.
  static void recovery_check_tail(
      const DataFile &file, Offset end,
      const std::vector<std::filesystem::path> &data_paths);
  // Writes hint files for all sealed files in s.
  void flush_hints(const EngineState &s);
  // Opens a hint file, rebuilding it from its data file if its bytes are bad.
  // HintFile::OpenForRead reads and verifies every byte before returning, so
  // this is where a damaged hint — one that fails its CRC, or one a read
  // fails on — is found, before any of its entries is applied. A hint is a
  // derived index, not the records it points at: neither failure says the
  // data file behind it is damaged. An error that says nothing about the
  // hint's bytes (EMFILE, ENOMEM) is thrown as it is: a rebuild would remove
  // a good hint and then fail to write it back. Throws when the rebuild
  // cannot produce a readable hint.
  static auto open_hint_or_rebuild(const std::shared_ptr<DataFile> &data_file,
                                   const std::filesystem::path &hint_path)
      -> HintFile;
  // open_hint_or_rebuild for recovery: applies fail_recovery_on_crc_errors
  // (strict) to a file that cannot be indexed. Lenient recovery skips such a
  // file — nullopt, after a warning on stderr — but only a file whose bytes
  // are the problem: an error of the process or the environment is thrown in
  // both modes, since a skip would cost the file's keys, and vacuum then the
  // file.
  static auto open_hint_or_skip(const std::shared_ptr<DataFile> &data_file,
                                const std::filesystem::path &hint_path,
                                bool strict) -> std::optional<HintFile>;

  

  // Vacuum helpers
  // Batch-aware scan: copies live Puts, markers and the tombstones `needed`
  // does not allow dropping from source_file into dest_file.
  static auto vacuum_scan_and_copy(
      const std::shared_ptr<const EngineState> &snap,
      const DataFile &source_file, WritableDataFile &dest_file,
      std::uint32_t source_file_id, const NeededTombstones &needed,
      std::uint64_t retain_after) -> VacuumScanResult;
  // Remaps key_dir entries, updates file registry, publishes new state.
  // First makes every write through decided_at durable: the state vacuum
  // judged the file's records dead by. Caller must hold write_mu_.
  void vacuum_commit(std::uint32_t old_file_id, const VacuumScanResult &scan,
                     std::shared_ptr<DataFile> new_sealed_file,
                     std::uint32_t dest_file_id, std::uint64_t decided_at);
  // fdatasyncs the active file and advances t's durable_seq to its last
  // sequence. On failure degrades, publishes the degraded state over
  // current, and rethrows. Caller holds the write barrier.
  void sync_active_file(TransientEngineState &t,
                        const std::shared_ptr<const EngineState> &current,
                        std::string_view caller);
  // Unlinks the old data and hint files. Open fds survive (POSIX).
  void vacuum_unlink_old_file(const std::shared_ptr<const EngineState> &snap,
                              std::uint32_t file_id);
  // Rewrites a sealed file into a new sealed file containing only live entries.
  [[nodiscard]] auto vacuum_compact_file(std::uint32_t file_id,
                                         std::uint64_t retain_after) -> bool;
  // Removes a sealed file with no live key and nothing compaction would keep:
  // the state change and the unlink, no scan.
  void vacuum_remove_file(std::uint32_t file_id);

  // State access helpers — raw state_ access is confined here.
  // Per-thread read cache behind load_state_for_read (see ReadCacheSlot).
  // One function-local thread_local slot shared by every DB the thread
  // touches, so its entry records its owner.
  [[nodiscard]] static auto read_cache() -> ReadCacheSlot &;
  // Claims this thread's slot and returns the guard; the entry behind it
  // holds this DB's state, refreshed if a publication is in progress or has
  // completed since the entry was filled.
  [[nodiscard]] auto load_state_for_read() const
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
  // wake every waiter. Releases the role however flush_pending ends: one
  // that throws refuses writes and records the error for the waiters.
  void flush_once();
  // execute_group's steps after the append; see the definition.
  void commit_appended(TransientEngineState &t,
                       std::shared_ptr<const EngineState> published,
                       std::span<Slot *> batch, bool any_sync,
                       std::uint64_t batch_max_seq,
                       std::uint64_t appended_bytes);
  // flush_pending threw, with its exception in flight: records it for the
  // waiters and refuses writes. The caller still holds the flush role.
  void flush_threw() noexcept;
  // Why a write is refused, or nullptr if it may go ahead.
  [[nodiscard]] auto write_refusal(const EngineState &s) const
      -> std::exception_ptr;
  // Whether the engine refuses writes as degraded: the published state says
  // so, or write_fault_ is set.
  [[nodiscard]] auto refused(const EngineState &s) const noexcept -> bool;
  // Refuses writes from now on, then publishes the degraded state if memory
  // allows, and wakes every waiter. Allocates nothing to refuse. For a
  // failure after an append that no I/O handler degraded for.
  void refuse_writes() noexcept;
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
  // Prepares and applies one slot against the transient. Appends nothing:
  // its only I/O is the record reads a key directory that holds no key bytes
  // needs to place keys and check guards. Appends prepared entries to
  // all_entries; running_offset is
  // advanced by the total byte size produced. Returns false on validation
  // failure (sets slot.result) or when the slot cannot be written (sets
  // slot.err): its sequences would pass the packed limit, or it would cross
  // the rotation threshold and no file id is left (can_rotate false).
  auto execute_slot(TransientEngineState &t, EngineSlot &slot,
                    std::vector<DataEntryView> &all_entries,
                    std::uint64_t &running_offset, bool can_rotate) -> bool;
  // Executor callback shared by solo_writer_ and write_group_. Runs the
  // batch as consecutive groups, each through execute_group.
  void execute_slots(std::vector<Slot *> &batch);
  // Runs a prefix of batch as one group, in three phases: (1) per-slot
  // validate/prepare/apply in-memory, (2) one append_entries, (3)
  // sync/rotate/publish. The prefix ends before a slot that would take the
  // active file past 2^32 bytes, the packed offset limit. Returns the prefix
  // length. Caller holds write_mu_.
  auto execute_group(std::span<Slot *> batch) -> std::size_t;

  // The prefix of an ingest slice written to the active file before the
  // next rotation: it ends after the first entry, outside a batch, that
  // takes the file (now file_size bytes) to the threshold. needs_rotation
  // is false when the prefix is the whole slice; end_size is the file size
  // after it.
  struct IngestChunk {
    std::size_t end;
    bool needs_rotation;
    std::uint64_t end_size;
  };
  [[nodiscard]] auto ingest_chunk(std::span<const DataEntryView> entries,
                                  std::uint64_t file_size) const
      -> IngestChunk;

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
#ifdef BYTECASK_KEYDIR_BLIND
  // Reconstructs a blind key directory straight from the sorted hint files:
  // fences in each file, range splitters from the fences, a k-way merge per
  // range into blind leaves. Builds no intermediate tree.
  auto recovery_load_streams(EngineState s, std::vector<RecoveredFile> files,
                             unsigned recovery_threads, bool strict)
      -> EngineState;
#endif
  // Reconstructs key_dir from hint files by range merge. Coupled to the B+
  // tree: it needs the tree to sample its own separators and to bulk-build
  // and concatenate range-disjoint slices.
  auto recovery_load_ranged(EngineState s, std::vector<RecoveredFile> files,
                            unsigned recovery_threads, bool strict)
      -> EngineState;
  // Builds a RecoveryResult by merging sorted hint runs into a bulk loader.
  // Requires the sorted hint files flush_hints_for writes.
  static auto recovery_build_sorted(std::span<RecoveredFile> files,
                                    bool strict) -> RecoveryResult;

  // Portable atomic load/store for shared_ptr. The C++20 specialization
  // std::atomic<std::shared_ptr<T>> is not yet available in all libc++
  // versions (e.g. Homebrew LLVM). The C++11 free functions work everywhere
  // but are deprecated in libstdc++ — suppress the warning here once.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  auto load_state() const -> std::shared_ptr<EngineState> {
    return std::atomic_load(&state_);
  }
  // Every publication passes through here — the checked store_state and
  // the degrade paths that publish a degraded copy directly — so this is
  // where a transition into degraded is counted, once.
  //
  // It is also where readers learn that the state moved. publishing_ is
  // raised before the store and state_gen_ bumped after it, so anyone who
  // has seen the new state (a reader that loaded it, a writer whose commit
  // it covers) happens-after the raise: a read that starts after them finds
  // publishing_ raised or state_gen_ moved, and reloads instead of serving
  // its cached state (see load_state_for_read).
  void store_state(std::shared_ptr<EngineState> s) {
    const bool degraded = s->degraded;
    publishing_.fetch_add(1, std::memory_order_acq_rel);
    const auto old = std::atomic_exchange(&state_, std::move(s));
#ifdef BYTECASK_TESTING
    if (test_between_publish_stores_) test_between_publish_stores_();
#endif
    state_gen_.store(next_state_gen(), std::memory_order_release);
    publishing_.fetch_sub(1, std::memory_order_release);
    // state_ is never null: it starts as an empty state (see open).
    if (degraded && !old->degraded) {
      counters_.degraded_transitions.fetch_add(1, std::memory_order_relaxed);
    }
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
  int lock_fd_{-1};  // flock() on dir_/.lock; released by DB::close()
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  std::uint32_t max_hint_backlog_{kDefaultMaxHintBacklog};  // 0 = unbounded
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
  // Readers serve their cached state only while publishing_ is 0 and
  // state_gen_ is the value they cached it under. Both are maintained by the raw store_state; generations are
  // unique across DB instances (next_state_gen).
  std::atomic<std::uint32_t> publishing_{0};
  std::atomic<std::uint64_t> state_gen_{0};
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
  // How long a flush that owes an fdatasync waits for the commits the last
  // one released, before it captures the head. See CommitDelay.
  CommitDelay commit_delay_;
  // Names this DB to per-thread state that must not match another DB: unlike
  // its address, never reused in the process. Starts at 1; 0 is no DB.
  const std::uint64_t instance_id_{next_instance_id()};
  static auto next_instance_id() noexcept -> std::uint64_t {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }
  // The error that failed a flush — a failed fdatasync, or anything that
  // threw before it published — rethrown by commit_wait to every writer
  // whose entries were appended since the last successful flush. Guarded by
  // durable_mu_. Cleared by resume().
  std::exception_ptr flush_error_;
  // Set when a write reached the data file and could not be published, by a
  // failure no I/O handler degrades for: an allocation, or anything else
  // unexpected, after the append. The file then holds bytes the published
  // state does not cover, so writes are refused until resume() reconciles
  // them, as for a degraded state. A flag rather than a degraded state
  // because publishing one allocates, and the failure is often that
  // nothing could be. refuse_writes() publishes the degraded state too when
  // it can; the flag is what holds. Cleared by resume().
  std::atomic<bool> write_fault_{false};
  // Serialises writers (put, del, apply_batch). Readers never acquire this.
  std::unique_ptr<std::mutex> write_mu_{std::make_unique<std::mutex>()};
  // Serialises vacuum() calls. Separate from write_mu_ so vacuum I/O does
  // not block normal writes.
  std::unique_ptr<std::mutex> vacuum_mu_{std::make_unique<std::mutex>()};
  // Tombstones compaction must keep. Set by recovery during open, read-only
  // afterwards.
  NeededTombstones needed_tombstones_;
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
  auto& test_commit_delay() { return commit_delay_; }
  // Called by flush_pending on the flushing thread just before the
  // fdatasync. Lets a test hold a flush in flight while other writers
  // append behind it.
  std::function<void()> test_before_flush_sync_;
  // Called by store_state on the publishing thread between the state store
  // and the state_gen_ bump, while publishing_ is raised. Lets a test hold a
  // publication in that gap.
  std::function<void()> test_between_publish_stores_;
  // Called by apply_batch on the writer thread after stage 1, just before
  // commit_wait. Lets a test hold a writer whose entries are already in the
  // head while another thread flushes and publishes them.
  std::function<void()> test_before_commit_wait_;
  // Called on the background worker at the start of every hint task queued
  // by dispatch_hint. Lets a test hold the worker to build a backlog.
  std::function<void()> test_before_hint_;
  // Called by vacuum once its copy is in place, before it takes the write
  // barrier to commit. Lets a test write to the keys the copy holds.
  std::function<void()> test_before_vacuum_commit_;
  // Called under durable_mu_ each time a thread blocked in wait_published or
  // durable_sequence checks its condition, the first time before it blocks.
  // Lets a test act once a waiter is parked. Must not take durable_mu_.
  std::function<void()> test_in_sequence_wait_;
  // Called by execute_group once a batch is appended and by flush_pending
  // before it publishes. A test throws from them to fail the step that
  // follows an append, as a failed allocation there would.
  std::function<void()> test_after_append_;
  std::function<void()> test_before_publish_;
  // Called by refuse_writes before it publishes the degraded state. A test
  // throws from it to leave the flag as the only refusal.
  std::function<void()> test_before_refusal_publish_;
  // Called by finish_rotation before it installs the new files in the
  // transient. A test throws from it to fail the rotation's in-memory step.
  std::function<void()> test_in_finish_rotation_;
  // Leaves plans unresolved, so validation and the key directory update go
  // by key, as before record locations were used as version tokens. The
  // differential test runs one workload both ways.
  bool test_resolve_by_key_{false};
  // Called on the state open or resume() is about to validate, so a test can
  // break it and drive the cold-path consistency checks through their real
  // callers. Static: open has no DB to hang a hook on yet. A plain pointer,
  // so the hook has no constructor or destructor to run at program exit.
  static inline void (*test_before_validate_)(EngineState &) = nullptr;
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

  // Returns an input range of keys >= from. Under the blind-leaf key
  // directory each step reads its key's record (the leaf holds no key
  // bytes); the keyed directories read nothing.
  [[nodiscard]] auto keys_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;

  // Returns a range of (key, value) pairs in descending key order.
  // When from is non-empty, starts at the last key <= from.
  // When from is empty, starts at the last key in the DB.
  [[nodiscard]] auto riter_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;

  // Returns a range of keys in descending order. Reads as keys_from does.
  [[nodiscard]] auto rkeys_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

  // Live keys in [from, to), counted no further than `limit`: returns
  // min(count, limit). Throws std::invalid_argument if from >= to. Reads no key per counted entry: at
  // most two record reads under the blind-leaf key directory, to place each
  // end, and none under the keyed ones.
  [[nodiscard]] auto count_keys(BytesView from, BytesView to,
                                std::size_t limit) const -> std::size_t;

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

  // Deletes [from, to). from >= to throws std::invalid_argument.
  void del_range(BytesView from, BytesView to) {
    check_key_size(from.size(), limits_.max_key_bytes);
    check_key_size(to.size(), limits_.max_key_bytes);
    check_range(from, to);
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
  // from is inclusive, to is exclusive; from >= to throws
  // std::invalid_argument.
  // Requires a snapshot — throws std::logic_error if constructed without one.
  void ensure_range_unchanged(BytesView from, BytesView to) {
    check_key_size(from.size(), limits_.max_key_bytes);
    check_key_size(to.size(), limits_.max_key_bytes);
    check_range(from, to);
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

  // Looks up each point write's key in the snapshot, on the caller's
  // thread, before the plan joins a batch: under the write lock the check
  // and the update then find the key by the location found here, without
  // reading its record (docs/unsynced_commit_design.md, D3).
  void resolve_snapshot_entries();
  // The snapshot's entry for write i: resolved earlier, or looked up now.
  [[nodiscard]] auto snapshot_entry(std::size_t i, BytesView key) const
      -> std::optional<KeyDirEntry>;
  // The resolved snapshot entry for write i, if resolution ran and the key
  // was present: the location an update or erase can go by.
  [[nodiscard]] auto resolved_entry(std::size_t i) const
      -> const std::optional<KeyDirEntry> & {
    static const std::optional<KeyDirEntry> kNone;
    return i < snap_entries_.size() ? snap_entries_[i] : kNone;
  }

  std::optional<Snapshot> snap_;
  SizeLimits limits_;
  std::vector<WriteOp> writes_;
  std::map<Bytes, KeyGuard> guards_;
  std::vector<RangeGuard> range_guards_;
  // Parallel to writes_ once resolve_snapshot_entries ran; empty otherwise.
  std::vector<std::optional<KeyDirEntry>> snap_entries_;
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
  // A sync slot that appended nothing: the sequence every earlier write
  // reaches, which commit_wait makes durable before the slot returns. Its
  // result keeps sequence 0 — the write itself wrote nothing.
  std::uint64_t sync_through{0};
};


DbDegraded::~DbDegraded() = default;
DbFollowerMode::~DbFollowerMode() = default;
DbChangeMarkerMismatch::~DbChangeMarkerMismatch() = default;
DbClosed::~DbClosed() = default;

#pragma region Internal helpers

namespace {

// Ids are given out from the directory's contents at each open and never
// reused within a process, so the ceiling is reached by files created since
// open, and a reopen renumbers.
auto file_ids_exhausted() -> std::runtime_error {
  return std::runtime_error{std::format(
      "file id space exhausted: this process has used all {} file ids. "
      "Reopen the database to renumber its files.",
      KeyDirEntry::kMaxFileId + 1)};
}

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
  // TLS: process — an entropy source; no DB data.
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

// A ChangeMarker's id: 64 random bits, from the entropy source, like the
// file stem's salt. Two promotions anywhere must not share one.
auto random_marker_id() -> std::uint64_t {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // TLS: process — an entropy source; no DB data.
  static thread_local std::random_device rd;
#pragma clang diagnostic pop
  return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

// The marker list recovery publishes: sorted by sequence, null when empty.
auto sorted_markers(std::vector<ChangeMarker> markers)
    -> std::shared_ptr<const std::vector<ChangeMarker>> {
  if (markers.empty()) return nullptr;
  std::ranges::sort(markers, {}, &ChangeMarker::since_sequence);
  return std::make_shared<const std::vector<ChangeMarker>>(std::move(markers));
}

} // namespace

#pragma endregion

#pragma region TransientEngineState

TransientEngineState::TransientEngineState(
    KeyDirTransient key_dir,
    TransientU32Table<std::shared_ptr<DataFile>> files,
    FileStatsTransient file_stats,
    std::uint32_t active_file_id, std::uint32_t next_file_id,
    std::uint64_t next_seq, std::uint64_t durable_seq,
    std::uint64_t sync_requested_seq,
    Mode mode, bool degraded, std::string degraded_reason,
    std::shared_ptr<const std::vector<ChangeMarker>> change_markers)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_file_id_{next_file_id}, next_seq_{next_seq},
      durable_seq_{durable_seq}, sync_requested_seq_{sync_requested_seq},
      mode_{mode}, degraded_{degraded},
      degraded_reason_{std::move(degraded_reason)},
      change_markers_{std::move(change_markers)} {}

auto EngineState::transient() const -> TransientEngineState {
  return TransientEngineState{
      key_dir.transient(), files.transient(), file_stats.transient(),
      active_file_id, next_file_id, next_seq, durable_seq,
      sync_requested_seq, mode, degraded, degraded_reason, change_markers};
}

void TransientEngineState::append_marker(ChangeMarker m) {
  auto list = change_markers_ ? *change_markers_ : std::vector<ChangeMarker>{};
  list.push_back(m);
  change_markers_ =
      std::make_shared<const std::vector<ChangeMarker>>(std::move(list));
}

void TransientEngineState::apply_change_marker(std::uint64_t sequence,
                                               std::uint64_t id) {
  const auto sz = change_marker_size(EntryType::ChangeMarker);
  file_stats_.patch(active_file_id_,
                    {.total_added = sz, .change_marker_added = sz,
                     .min_sequence = sequence, .max_sequence = sequence});
  if (sequence >= next_seq_) next_seq_ = sequence + 1;
  append_marker({sequence, id});
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
  for (const auto &rg : plan.range_guards_)
    if (range_changed(*snap_state, rg.from, rg.to, lost_to)) return false;

  // 3. Implicit W-W check on all write keys (only when snapshot present).
  if (snap_state) {
    // The key changed since the snapshot unless the head maps it to the
    // snapshot's record, or it is absent from both. The head is asked by
    // location first, which reads nothing; only a key that moved is read,
    // to tell a changed key from one vacuum relocated.
    const auto point_conflict = [&](std::size_t i, const Bytes &key) {
      const std::span<const std::byte> key_span{key};
      const auto snap_entry = plan.snapshot_entry(i, key_span);
      if (snap_entry && kd_holds(key_dir_, key_span, *snap_entry)) return false;
      const auto cur_entry = kd_get(key_dir_, key_span, kd_ctx());
      const bool appeared = !snap_entry && cur_entry;
      const bool deleted = snap_entry && !cur_entry;
      const bool modified = snap_entry && cur_entry &&
                            cur_entry->sequence() != snap_entry->sequence();
      if (!(appeared || deleted || modified)) return false;
      lost_to = cur_entry ? cur_entry->sequence() : head_latest;
      return true;
    };
    for (std::size_t i = 0; i < plan.writes_.size(); ++i) {
      bool has_conflict = false;
      std::visit(
          [&](const auto &op) -> void {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, WritePlan::PointPut> ||
                          std::is_same_v<T, WritePlan::PointDel>) {
              has_conflict = point_conflict(i, op.key);
            } else if constexpr (std::is_same_v<T, WritePlan::RangeDel>) {
              has_conflict = range_changed(*snap_state, op.from, op.to, lost_to);
            }
          },
          plan.writes_[i]);
      if (has_conflict) return false;
    }
  }

  return true;
}

auto TransientEngineState::range_changed(const EngineState &snap,
                                         std::span<const std::byte> from,
                                         std::span<const std::byte> to,
                                         std::uint64_t &lost_to) const -> bool {
  // Keys changed or inserted since the snapshot: present in the head with a
  // sequence the snapshot does not hold for them. A key vacuum relocated
  // keeps its sequence, so it is not a change.
  for (auto it = kd_lower_bound(key_dir_, from, kd_ctx());
       it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (Key{key_span} >= Key{to}) break;
    const auto snap_entry = kd_get(snap.key_dir, key_span, snap.kd_ctx());
    const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
    if (entry.sequence() != snap_seq) {
      lost_to = entry.sequence();
      return true;
    }
  }
  // Keys deleted since the snapshot, by del or by a range tombstone: present
  // in the snapshot, absent from the head. They leave no entry to carry a
  // sequence, so a retry has to see the head's latest.
  for (auto it = kd_lower_bound(snap.key_dir, from, snap.kd_ctx());
       it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (Key{key_span} >= Key{to}) break;
    if (!kd_get(key_dir_, key_span, kd_ctx())) {
      lost_to = next_seq_ - 1;
      return true;
    }
  }
  return false;
}

void WritePlan::resolve_snapshot_entries() {
  if (!snap_) return;
  const auto &s = *snap_->state_;
  snap_entries_.clear();
  snap_entries_.reserve(writes_.size());
  for (const auto &w : writes_) {
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, PointPut> ||
                        std::is_same_v<T, PointDel>) {
            snap_entries_.push_back(kd_get(s.key_dir, op.key, s.kd_ctx()));
          } else {
            snap_entries_.emplace_back();
          }
        },
        w);
  }
}

auto WritePlan::snapshot_entry(std::size_t i, BytesView key) const
    -> std::optional<KeyDirEntry> {
  if (i < snap_entries_.size()) return snap_entries_[i];
  const auto &s = *snap_->state_;
  return kd_get(s.key_dir, key, s.kd_ctx());
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
  constexpr auto kMarker = marker_size(EntryType::BulkBegin);

  // Account for BulkBegin marker.
  if (multi) {
    file_stats_.patch(active_file_id_,
                      {.total_added = kMarker, .marker_added = kMarker});
    ++next_seq_;
    ++io_idx;
  }

  for (std::size_t w_idx = 0; w_idx < plan.writes_.size(); ++w_idx) {
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
            const std::span<const std::byte> key_span{op.key};
            const auto val_size = narrow<std::uint32_t>(op.value.size());
            note_pending(offsets[io_idx], next_seq_, key_span, val_size);
            const auto entry = KeyDirEntry::make(next_seq_, offsets[io_idx],
                                                 active_file_id_, val_size);
            // An update of a key still at the snapshot's record goes by that
            // location and reads nothing; anything else is placed by key.
            const auto &at = plan.resolved_entry(w_idx);
            const bool by_location =
                at && kd_put_at(key_dir_, key_span, *at, entry);
            const auto existing =
                by_location ? std::optional{kd_hit(*at)}
                            : kd_put(key_dir_, key_span, entry, kd_ctx());
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              file_stats_.patch(existing->file_id(), {.live_removed = dec});
            }
            const auto sz = entry_size(key_span.size(), val_size);
            file_stats_.patch(active_file_id_, {.live_added = sz, .total_added = sz});
            ++next_seq_;
            ++io_idx;
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            const std::span<const std::byte> key_span{op.key};
            const auto &at = plan.resolved_entry(w_idx);
            const bool by_location = at && kd_erase_at(key_dir_, key_span, *at);
            const auto existing =
                by_location ? std::optional{kd_hit(*at)}
                            : kd_erase(key_dir_, key_span, kd_ctx());
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              file_stats_.patch(existing->file_id(), {.live_removed = dec});
            }
            const auto del_sz = entry_size(key_span.size(), 0);
            file_stats_.patch(active_file_id_, {.total_added = del_sz, .tombstone_added = del_sz});
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
              file_stats_.patch(entry.file_id(), {.live_removed = dec});
              to_erase.emplace_back(key_span);
            }
            for (const auto &k : to_erase) {
              (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
            }

            const auto rd_sz = entry_size(op.from.size(), op.to.size());
            file_stats_.patch(active_file_id_, {.total_added = rd_sz, .tombstone_added = rd_sz});
            ++next_seq_;
            ++io_idx;
          }
        },
        plan.writes_[w_idx]);
  }

  // Account for BulkEnd marker.
  if (multi) {
    file_stats_.patch(active_file_id_,
                      {.total_added = kMarker, .marker_added = kMarker});
    ++next_seq_;
    ++io_idx;
  }

  file_stats_.patch(active_file_id_, {.min_sequence = batch_start_seq, .max_sequence = next_seq_ - 1});
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
    case EntryType::BulkEnd: {
      const auto m = marker_size(e.entry_type);
      file_stats_.patch(active_file_id_, {.total_added = m, .marker_added = m});
      break;
    }

    case EntryType::ChangeMarker: {
      // The source's promotion, stored as this node's: from here on the two
      // histories carry the same identity (#397).
      const auto m = change_marker_size(e.entry_type);
      file_stats_.patch(active_file_id_,
                        {.total_added = m, .change_marker_added = m});
      append_marker({e.sequence, decode_marker_id(e.value)});
      break;
    }

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
        file_stats_.patch(existing->file_id(), {.live_removed = dec});
      }
      const auto sz = entry_size(e.key.size(), val_size);
      file_stats_.patch(active_file_id_, {.live_added = sz, .total_added = sz});
      break;
    }

    case EntryType::Delete: {
      const auto existing = kd_erase(key_dir_, e.key, kd_ctx());
      if (existing) {
        const auto dec =
            entry_size(e.key.size(), existing->value_size());
        file_stats_.patch(existing->file_id(), {.live_removed = dec});
      }
      const auto del_sz = entry_size(e.key.size(), 0);
      file_stats_.patch(active_file_id_, {.total_added = del_sz, .tombstone_added = del_sz});
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
        file_stats_.patch(entry.file_id(), {.live_removed = dec});
        to_erase.emplace_back(key_span);
      }
      for (const auto &k : to_erase) {
        (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
      }
      const auto rd_sz = entry_size(e.key.size(), e.value.size());
      file_stats_.patch(active_file_id_, {.total_added = rd_sz, .tombstone_added = rd_sz});
      break;
    }
    }
  }

  // Advance next_seq past the highest ingested sequence.
  if (max_seq >= next_seq_) next_seq_ = max_seq + 1;

  file_stats_.patch(active_file_id_, {.min_sequence = batch_start_seq, .max_sequence = max_seq});
}

void TransientEngineState::apply_rotate_file(
    std::shared_ptr<DataFile> sealed_old,
    std::shared_ptr<DataFile> new_file, std::uint32_t new_file_id) {
  files_.set(active_file_id_, std::move(sealed_old));
  active_file_id_ = new_file_id;
  files_.set(active_file_id_, std::move(new_file));
  file_stats_.set(active_file_id_, FileStats{});
}

auto TransientEngineState::file_ids_left() const noexcept -> std::uint32_t {
  return next_file_id_ > KeyDirEntry::kMaxFileId
             ? 0
             : KeyDirEntry::kMaxFileId - next_file_id_ + 1;
}

auto TransientEngineState::reserve_file_id() -> std::uint32_t {
  if (file_ids_left() == 0) throw file_ids_exhausted();
  return next_file_id_++;
}

void TransientEngineState::apply_vacuum(
    std::uint32_t old_file_id, const VacuumScanResult &scan,
    std::shared_ptr<DataFile> new_sealed_file, std::uint32_t dest_file_id) {
  if (!new_sealed_file) {
    // Removal: nothing in the file is live, so there is nothing to remap.
    if (!scan.mappings.empty())
      throw std::logic_error{"apply_vacuum: live entries without a destination"};
    files_.erase(old_file_id);
    file_stats_.erase(old_file_id);
    return;
  }

  // The destination is registered before the remap: a key directory that
  // reads keys back may resolve an already remapped record while placing the
  // next one.
  files_.set(dest_file_id, std::move(new_sealed_file));

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
  file_stats_.set(dest_file_id,
                  FileStats{actual_live_bytes, scan.total_bytes,
                            scan.min_sequence, scan.max_sequence,
                            scan.tombstone_bytes, scan.marker_bytes,
                            scan.change_marker_bytes});
}

void TransientEngineState::apply_resume(
    std::uint32_t file_id, const std::vector<ResumeEntry> &entries,
    std::uint64_t valid_offset) {
  std::uint64_t max_seq = 0;
  std::uint64_t seq_min = 0;
  std::uint64_t seq_max = 0;
  std::uint64_t tomb = 0;
  std::uint64_t mark = 0;
  std::uint64_t cm = 0;
  for (const auto &e : entries) {
    const std::span<const std::byte> key_span{e.key};
    if (e.sequence > max_seq) max_seq = e.sequence;
    if (seq_min == 0) seq_min = e.sequence;  // entries are in file order
    if (e.sequence > seq_max) seq_max = e.sequence;
    // entries holds every committed entry in the file, so the file's
    // tombstones are rebuilt from scratch here, like its bounds.
    tomb += tombstone_size(e.entry_type, e.key.size(), e.range_end.size());
    mark += marker_size(e.entry_type);
    cm += change_marker_size(e.entry_type);

    switch (e.entry_type) {
    case EntryType::Put: {
      const auto existing = kd_get(key_dir_, key_span, kd_ctx());
      if (!existing || existing->sequence() < e.sequence) {
        if (existing) {
          const auto dec = entry_size(key_span.size(), existing->value_size());
          file_stats_.patch(existing->file_id(), {.live_removed = dec});
        }
        const auto inc = entry_size(key_span.size(), e.value_size);
        file_stats_.patch(file_id, {.live_added = inc});
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
        file_stats_.patch(existing->file_id(), {.live_removed = dec});
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
        file_stats_.patch(entry.file_id(), {.live_removed = dec});
        to_erase.emplace_back(k);
      }
      for (const auto &k : to_erase)
        (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
      break;
    }
    case EntryType::ChangeMarker:
      // A promotion whose marker reached the file but was never published —
      // set_mode(Leader) failed after its append — is in the list from here.
      // One already published is in it already, so only a marker above the
      // list's last is added.
      // The list is never empty when it exists.
      if (const auto *ms = change_markers_.get();
          !ms || ms->back().since_sequence < e.sequence) {
        append_marker({e.sequence, e.marker_id});
      }
      break;
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

  // apply_resume owns the truncated file's stats, so callers need no
  // mutable file_stats access. Its live bytes moved entry by entry above;
  // its extent, bounds and tombstones are rebuilt from the entries.
  if (auto fs = file_stats_.get(file_id)) {
    fs->total_bytes = valid_offset;
    fs->min_sequence = seq_min;
    fs->max_sequence = seq_max;
    fs->tombstone_bytes = tomb;
    fs->marker_bytes = mark;
    fs->change_marker_bytes = cm;
    file_stats_.set(file_id, *fs);
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
  s->change_markers = std::move(change_markers_);
  return s;
}

#pragma endregion

#pragma region Construction

// Opens dir, runs recovery, creates initial active data file.
// Throws std::system_error if the directory cannot be prepared.
DB::DB(std::filesystem::path dir, Options opts)
    : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
      max_hint_backlog_{opts.max_hint_backlog},
      io_backend_{opts.io_backend},
      size_limits_{opts.max_key_bytes, opts.max_value_bytes},
      state_{std::make_shared<EngineState>()} {
  // A setting above a hard ceiling is refused, not lowered: the engine never
  // runs with a limit other than the one asked for.
  if (opts.max_key_bytes > kMaxKeySize) {
    throw std::invalid_argument{std::format(
        "max_key_bytes = {} exceeds the hard ceiling of {}",
        opts.max_key_bytes, kMaxKeySize)};
  }
  if (opts.max_value_bytes > kMaxValueSize) {
    throw std::invalid_argument{std::format(
        "max_value_bytes = {} exceeds the hard ceiling of {}",
        opts.max_value_bytes, kMaxValueSize)};
  }
  if (opts.max_file_bytes > kMaxFileBytes) {
    throw std::invalid_argument{std::format(
        "max_file_bytes = {} exceeds the hard ceiling of {}",
        opts.max_file_bytes, kMaxFileBytes)};
  }
  if (opts.recovery_threads == 0) {
    throw std::invalid_argument{
        "recovery_threads = 0: recovery needs at least one thread"};
  }
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
          opts.buffer_pool.capacity_bytes, 2 * opts.max_file_bytes)};
    }
    pool_ = std::make_shared<BufferPool>(opts.buffer_pool);
  }
  // The directories open creates, each named by an entry in its parent that
  // has to be durable before the database's files can be.
  std::vector<std::filesystem::path> created;
  for (auto p = std::filesystem::absolute(dir_); !std::filesystem::exists(p);
       p = p.parent_path()) {
    created.push_back(p);
  }
  std::filesystem::create_directories(dir_);
  for (const auto &p : created)
    sync_directory(p.parent_path(), "io_dir_sync_create_dir");

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
    auto new_active = create_active_file(s.active_file_id, "io_dir_sync_open");
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
    // Every recovered entry is durable: sealed files were synced whole
    // before they were sealed, and recovery_prepare_files rewrote and synced
    // every hint-less one before reading it.
    initial->durable_seq =
        initial->next_seq > 0 ? initial->next_seq - 1 : 0;
    initial->mode = opts.initial_mode;
#ifdef BYTECASK_TESTING
    if (test_before_validate_) test_before_validate_(*initial);
#endif
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

DB::~DB() {
  try {
    close();
  } catch (...) {}
  // close() took this DB's read-cache entries, but a read on another thread
  // racing an explicit close() is not ordered after it and can cache one
  // again. Every call on this DB has returned by now, so this pass is the
  // last word.
  try {
    ReadCacheRegistry::instance().release(this);
  } catch (...) {}
}

// Takes vacuum_mu_ before write_mu_, the order vacuum() takes them in. The
// barrier publishes every write already appended, and admits no new one
// before the closed state, so the state it reads holds every write the DB
// will ever acknowledge.
void DB::close() {
  std::lock_guard<std::mutex> vg{*vacuum_mu_};
  std::exception_ptr error;
  {
    WriteBarrier barrier{*this};
    auto current = load_state_for_write();
    if (current->closed) return;
    auto t = current->transient();
    const auto last_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
    if (refused(*current)) {
      // No sync here can be trusted: one that failed earlier left pages
      // clean that a later one does not write (#231). What was durable when
      // the engine degraded is what is durable. The trim is best effort; a
      // degraded active file may already be sealed.
      if (t.durable_seq() < last_seq) {
        error = std::make_exception_ptr(DbDegraded{std::format(
            "close: writes after sequence {} were acknowledged but are not "
            "durable; the engine was degraded: {}",
            t.durable_seq(), current->degraded_reason)});
      }
      try {
        t.active_file().shrink_to_fit();
      } catch (...) {}
    } else {
      try {
        t.active_file().sync();
        counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
        t.apply_sync(last_seq);
        t.active_file().shrink_to_fit();
        // With every byte of the active file durable, its hint spares the
        // next open the rewrite of a hint-less file. An empty file costs the
        // next open nothing.
        if (t.active_file().size() > 0)
          flush_hints_for(t.active_file_ptr(), dir_);
      } catch (...) {
        counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
        error = std::current_exception();
      }
    }
    // Sealed files were synced whole before they were sealed, so their hints
    // are written whether or not the engine is degraded.
    try {
      flush_hints();
    } catch (...) {
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      if (!error) error = std::current_exception();
    }
    store_state(current, std::move(t).persistent()->closed_copy());
  }
  // The closed state holds no files; the states threads cached do.
  ReadCacheRegistry::instance().release(this);
  if (lock_fd_ != -1) {
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
  if (error) std::rethrow_exception(error);
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
  const auto s = load_state_for_read();
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
  // TLS: scratch — this call's record; keeps the largest value's size (#384).
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
  WritePlan plan{size_limits_};
  plan.del_range(from, to);
  return *apply_batch(opts, std::move(plan));
}

auto DB::contains_key(const ReadOptions& opts, BytesView key) const -> bool {
  const auto s = load_state_for_read();
  return kd_contains(s->key_dir, key, s->kd_ctx(opts.verify_checksums));
}

#pragma endregion

#pragma region Snapshot and apply_batch

auto DB::snapshot() const -> Snapshot {
  return Snapshot{load_state_for_read().state(), size_limits_};
}

// Why writes are refused while DB::write_fault_ is set.
constexpr const char *kWriteFaultReason =
    "a write reached the data file but could not be published: call "
    "resume() to recover.";

// Why a state refuses writes: is_write_allowed() is false.
static auto write_rejection(const EngineState &s) -> std::exception_ptr {
  if (s.closed) return std::make_exception_ptr(DbClosed{});
  if (s.degraded) return std::make_exception_ptr(DbDegraded{s.degraded_reason});
  return std::make_exception_ptr(
      DbFollowerMode{"write rejected: engine is in follower mode"});
}

auto DB::write_refusal(const EngineState &s) const -> std::exception_ptr {
  if (!s.is_write_allowed()) return write_rejection(s);
  if (write_fault_.load(std::memory_order_acquire))
    return std::make_exception_ptr(DbDegraded{kWriteFaultReason});
  return nullptr;
}

// The calling thread's last synced commit: the DB it went to and when it
// returned. The commit delay's round trip is measured from it, per DB, so a
// thread that writes to two databases does not mix their timings.
struct LastSyncedReturn {
  std::uint64_t db{0};  // DB::instance_id_
  std::chrono::steady_clock::time_point at{};
};
// TLS: per-DB — names its DB by instance_id_, not by address, so a DB opened
// where a destroyed one lived does not take its timing. Holds no resources.
// Tested by "commit delay: a DB at a destroyed DB's address does not inherit
// its round trip".
static thread_local LastSyncedReturn last_synced_return{};

// The single write path. Routes to either write_group_ (default) or
// solo_writer_ depending on plan characteristics. put/del/apply_batch are
// thin wrappers that construct a WritePlan and delegate here.
auto DB::apply_batch(WriteOptions opts,
                     WritePlan plan) -> std::optional<CommitResult> {
  if (auto refusal = write_refusal(*load_state())) {
    std::rethrow_exception(refusal);
  }
  if (plan.write_bytes() > max_batch_bytes()) {
    throw std::invalid_argument{std::format(
        "write plan of {} bytes exceeds the limit of {} bytes per write",
        plan.write_bytes(), max_batch_bytes())};
  }
  // With sync, even an empty plan goes through the pipeline: it returns once
  // every earlier write is durable (see execute_slots).
  if (plan.empty() && !opts.sync) {
    return CommitResult{.sequence = 0, .durable = true};
  }

  if (opts.sync && last_synced_return.db == instance_id_)
    commit_delay_.on_round_trip(std::chrono::steady_clock::now() -
                                last_synced_return.at);

  EngineSlot slot;
  slot.plan = std::move(plan);
  slot.opts = opts;
  slot.sync = opts.sync;
#ifdef BYTECASK_TESTING
  if (!test_resolve_by_key_)
#endif
    slot.plan.resolve_snapshot_entries();

  const bool use_solo = opts.solo
      || slot.plan.write_bytes() > kGroupWriteMaxBytes;

  if (use_solo) {
    solo_writer_.submit(slot);
  } else {
    write_group_.submit(slot);
  }
  if (opts.sync) commit_delay_.on_arrival();

  // Stage 1 is done: the slot's entries are in the prepared head and the
  // page cache. Stage 2 — fdatasync (if sync) and publication — happens
  // here, on this thread when the flush role is free.
#ifdef BYTECASK_TESTING
  if (test_before_commit_wait_) test_before_commit_wait_();
#endif
  if (slot.result && (slot.result->sequence != 0 || slot.sync_through != 0)) {
    commit_wait(slot);
  }
  if (opts.sync)
    last_synced_return = {instance_id_, std::chrono::steady_clock::now()};

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

// Prepares and applies one slot against the transient. Appends nothing: its
// only I/O is the record reads a key directory that holds no key bytes needs
// to place keys and check guards. Entries are appended to all_entries;
// running_offset is advanced by
// the total byte size of entries produced. Returns false on validation
// failure (slot.result set to nullopt). durable is left false on success;
// execute_slots fills it in once the batch's durability is known.
auto DB::execute_slot(TransientEngineState &t, EngineSlot &slot,
                      std::vector<DataEntryView> &all_entries,
                      std::uint64_t &running_offset, bool can_rotate) -> bool {
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

  // Both refusals come before apply_writes touches the transient, so the
  // slot fails alone and the rest of the group commits.
  if (entries.back().sequence > KeyDirEntry::kMaxSequence) {
    slot.err = std::make_exception_ptr(std::runtime_error{std::format(
        "sequence space exhausted: the write needs sequences up to {}, "
        "above the limit of {}",
        entries.back().sequence, KeyDirEntry::kMaxSequence)});
    return false;
  }
  if (!can_rotate &&
      running_offset + slot.plan.write_bytes() >= rotation_threshold_) {
    slot.err = std::make_exception_ptr(file_ids_exhausted());
    return false;
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
  // Declared after the lock, so it measures only the time the lock is held.
  struct BusyTimer {
    std::atomic<std::int64_t> &total;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    ~BusyTimer() {
      total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count(),
                      std::memory_order_relaxed);
    }
  } busy{counters_.group_writer_busy_ns};

  counters_.group_writer_batches.fetch_add(1, std::memory_order_relaxed);
  counters_.group_writer_coalesced.fetch_add(
      static_cast<std::int64_t>(batch.size()), std::memory_order_relaxed);

  // Every group starts below max_file_bytes (a group that reaches it
  // rotates), and its first slot holds at most kMaxBatchBytes, so that slot
  // ends inside the 32-bit packed offset. A later slot that would end past
  // it starts the next group, after the rotation. At the default
  // max_file_bytes a group would need gigabytes of writes to split.
  std::span<Slot *> rest{batch};
  while (!rest.empty()) rest = rest.subspan(execute_group(rest));
}

auto DB::execute_group(std::span<Slot *> batch) -> std::size_t {
  // Admission is decided on the published state, not the head: a flush
  // fails without write_mu_ and head_ is only reset by the next barrier,
  // so the head can be non-degraded while the engine is. (Mode is the same
  // in both — set_mode is a barrier.) A batch that loaded head_ just before
  // such a failure still ends in commit_wait with the flush error, its
  // bytes handled by resume() like the failed flush's.
  auto published = load_state();
  if (const auto ex = write_refusal(*published)) {
    for (auto *s : batch) s->err = ex;
    return batch.size();
  }
  auto current = load_head();

  auto t = current->transient();
  auto &file = t.active_file();
  auto initial_offset = static_cast<std::uint64_t>(file.size());
  auto running_offset = initial_offset;
  std::vector<DataEntryView> all_entries;
  auto any_sync = false;
  // Without an id for the next file the active file cannot be rotated, so a
  // write that would take it to the threshold is refused before any I/O.
  const auto can_rotate = t.file_ids_left() > 0;

  // Phase 1: pure in-memory — validate, prepare, pre-compute offsets,
  // apply_writes for each slot sequentially, up to the slot that reaches
  // the rotation threshold.
  // The group ends before a slot that would take the file past the 32-bit
  // packed offset; the first slot always fits (see execute_slots).
  std::size_t taken = 0;
  for (; taken < batch.size(); ++taken) {
    auto &slot = static_cast<EngineSlot &>(*batch[taken]);
    if (taken > 0 &&
        running_offset + slot.plan.write_bytes() > max_group_file_bytes()) {
      break;
    }
    slot.sync = slot.opts.sync;
    execute_slot(t, slot, all_entries, running_offset, can_rotate);
    any_sync |= slot.opts.sync;
  }
  batch = batch.first(taken);

  // A sync slot that appended nothing (an empty, guard-only or no-op plan)
  // still makes the promise every sync=true write makes: on return, every
  // write before it is durable. It waits for the head's last sequence, which
  // is how a caller that writes with sync=false bounds what a crash can lose.
  const auto head_last_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
  auto sync_only_pending = false;
  for (auto *s : batch) {
    auto &slot = static_cast<EngineSlot &>(*s);
    if (slot.opts.sync && slot.result && slot.result->sequence == 0 &&
        head_last_seq > published->durable_seq) {
      slot.sync_through = head_last_seq;
      sync_only_pending = true;
    }
  }

  if (all_entries.empty()) {
    // No entry was appended by any slot: every result is {sequence = 0}.
    // durable is true once nothing earlier is unsynced — at once, or after
    // the flush this head now asks for (commit_wait).
    for (auto *s : batch) {
      auto &slot = static_cast<EngineSlot &>(*s);
      if (slot.result) slot.result->durable = slot.sync_through == 0;
    }
    if (sync_only_pending) {
      t.note_sync_requested(head_last_seq);
      store_head(std::move(t).persistent());
    }
    return batch.size();
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
    return batch.size();
  }

  try {
#ifdef BYTECASK_TESTING
    if (test_after_append_) test_after_append_();
#endif
    commit_appended(t, std::move(published), batch, any_sync, batch_max_seq,
                    running_offset - initial_offset);
  } catch (...) {
    // The batch is in the file and nothing published it: what the engine
    // holds no longer matches the file, so writes after it would build on
    // a wrong state. Refused until resume(), which replays the batch.
    const auto ex = std::current_exception();
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    refuse_writes();
  }
  return batch.size();
}

// Everything after a group's entries reach the file: publishing the head,
// or the rotation barrier and the publication after it. Throws only for a
// failure no handler here degrades for, which execute_group turns into a
// refusal: the entries are in the file either way.
void DB::commit_appended(TransientEngineState &t,
                         std::shared_ptr<const EngineState> published,
                         std::span<Slot *> batch, bool any_sync,
                         std::uint64_t batch_max_seq,
                         std::uint64_t appended_bytes) {
  auto &file = t.active_file();
  counters_.bytes_written.fetch_add(static_cast<std::int64_t>(appended_bytes),
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
  // The flush quiesce() waited on — this thread's or another writer's — may
  // have failed. t is built on a head the engine has already given up on,
  // so publishing it would clear the degrade without resume() and leave
  // flush_error_ set under a healthy-looking state. The batch fails with
  // that flush's error, like every other write appended behind it; resume()
  // recovers its bytes with theirs.
  if (refused(*published)) {
    std::exception_ptr ex;
    {
      std::lock_guard<std::mutex> lk{durable_mu_};
      ex = flush_error_;
    }
    if (!ex) ex = std::make_exception_ptr(DbDegraded{degraded_reason()});
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }
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
  PreparedRotation rotation;
  try {
    rotation = prepare_rotation(t);
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
  finish_rotation(t, std::move(rotation));
  counters_.file_rotations.fetch_add(1, std::memory_order_relaxed);
  counters_.files_opened.fetch_add(1, std::memory_order_relaxed);

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
  if (refused(*published)) return;
  const bool need_sync = head->sync_requested_seq > published->durable_seq;
  // Nothing appended and no sync asked for. A sync-only write appends
  // nothing but asks for one: the entries it covers may all be published
  // already, by flushes that did not sync them.
  if (head->next_seq <= published->next_seq && !need_sync) return;

  if (need_sync) {
#ifdef BYTECASK_TESTING
    if (test_before_flush_sync_) test_before_flush_sync_();
#endif
    commit_delay_.on_sync_start();
    const auto sync_start = std::chrono::steady_clock::now();
    try {
      static_cast<WritableDataFile &>(head->active_file()).sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      flush_failed(std::current_exception(), published,
                   head->active_file().path());
      return;
    }
    commit_delay_.on_sync_end(std::chrono::steady_clock::now() - sync_start);
  }

#ifdef BYTECASK_TESTING
  if (test_before_publish_) test_before_publish_();
#endif
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
  //
  // Only a flush that will fdatasync settles: one the head already owes, or
  // one a synced writer still in stage 1 will need. An unsynced flush has
  // no fdatasync to share, and settling would only delay its publication —
  // under continuous unsynced writers, by the whole bound on every flush
  // (docs/write_path_investigation.md: +19% oltp_write_only without it).
  {
    const auto owes_sync = [&] {
      return write_group_.sync_busy() ||
             load_head()->sync_requested_seq > load_state()->durable_seq;
    };
    if (owes_sync()) commit_delay_.wait();
    const auto deadline =
        std::chrono::steady_clock::now() + kFlushSettleMax;
    bool settled = false;
    while (write_group_.busy() && owes_sync()
           && std::chrono::steady_clock::now() < deadline) {
      settled = true;
      std::this_thread::yield();
    }
    if (settled) counters_.flush_settles.fetch_add(1, std::memory_order_relaxed);
  }
  try {
    flush_pending();
  } catch (...) {
    flush_threw();
  }
  finish_flush();
}

void DB::flush_threw() noexcept {
  // The head's entries are in the file and the flush that should have
  // published them did not: writes are refused, and every writer waiting
  // on this flush gets its error, as after a failed fdatasync.
  {
    std::lock_guard<std::mutex> lk{durable_mu_};
    flush_error_ = std::current_exception();
  }
  refuse_writes();
}

auto DB::refused(const EngineState &s) const noexcept -> bool {
  return s.degraded || write_fault_.load(std::memory_order_acquire);
}

void DB::refuse_writes() noexcept {
  write_fault_.store(true, std::memory_order_release);
  try {
#ifdef BYTECASK_TESTING
    if (test_before_refusal_publish_) test_before_refusal_publish_();
#endif
    // A state that is degraded already keeps its own reason.
    if (const auto current = load_state(); !current->degraded) {
      auto s = current->degraded_copy(kWriteFaultReason);
      std::lock_guard<std::mutex> lk{durable_mu_};
      store_state(std::move(s));
    }
  } catch (...) {
    // No memory to publish it: write_fault_ alone refuses until resume().
  }
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

auto DB::quiesce() -> FlushRole {
  for (;;) {
    if (!flush_in_flight_.exchange(true, std::memory_order_acq_rel)) {
      try {
        flush_pending();
      } catch (...) {
        flush_threw();
        finish_flush();
        throw;
      }
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
  const auto target =
      result.sequence != 0 ? result.sequence : slot.sync_through;
  const bool want_durable = slot.opts.sync;
  for (;;) {
    auto published = load_state();
    const bool covered = want_durable
        ? published->durable_seq >= target
        : published->next_seq > target;
    if (covered) {
      result.durable = published->durable_seq >= target;
      // Another thread may still be inside the publication that covers this
      // write. Reads that start after this return reload all the same: this
      // thread saw the state, so it happens-after the publisher raised
      // publishing_ (see the raw store_state).
      return;
    }
    {
      std::lock_guard<std::mutex> lk{durable_mu_};
      if (flush_error_) std::rethrow_exception(flush_error_);
    }
    if (refused(*published)) throw DbDegraded{degraded_reason()};

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
  // TLS: scratch — this call's record; keeps the largest value's size (#384).
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

auto Snapshot::count_keys(BytesView from, BytesView to,
                          std::size_t limit) const -> std::size_t {
  check_range(from, to);
  if (limit == 0) return 0;
  return kd_count(state_->key_dir, from, to, limit, state_->kd_ctx());
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
  auto s = load_state_for_read();
  auto it = kd_value_lower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{s.state(), std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

// Returns an input range of keys >= from. Reads no values; a key directory
// that holds no key bytes reads each key's record as the range advances.
auto DB::keys_from(const ReadOptions & /*opts*/, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto s = load_state_for_read();
  auto it = from.empty() ? kd_begin(s->key_dir, s->kd_ctx())
                         : kd_lower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto DB::riter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto s = load_state_for_read();
  auto it = kd_value_rlower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{s.state(), std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto DB::rkeys_from(const ReadOptions & /*opts*/, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto s = load_state_for_read();
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
  if (auto s = load_state(); s->closed) {
    throw DbClosed{};
  } else if (refused(*s)) {
    throw DbDegraded{degraded_reason()};
  }
  // Publishes scrape idle read caches; with no writes there are none, and
  // what the last writes retired stays pinned by whichever thread went idle
  // last. vacuum and stats are the entry points that keep running without
  // writes, so they scrape too.
  scrape_read_caches();

  // Drain in-flight background hint writes so that vacuum's
  // flush_hints_for call cannot race on the same .hint.tmp file.
  worker_.drain();

  // Snapshot file_stats and active-file info.
  FileStatsMap stats_snap;
  std::uint32_t active_id{};
  {
    auto s = load_state_for_write();
    stats_snap = s->file_stats;
    active_id = s->active_file_id;
  }

  // Sealed files above the threshold, most fragmented first. Tombstones and
  // batch markers count as kept, not as fragmentation: a file is compacted
  // for its dead Puts, so a file holding nothing else has nothing to
  // reclaim. Nor has a file whose every entry is above retain_after.
  std::vector<std::pair<double, std::uint32_t>> candidates;
  for (const auto [fid, fs] : stats_snap.all()) {
    if (fid == active_id) continue;
    if (fs.total_bytes == 0) continue;
    if (fs.min_sequence > opts.retain_after) continue;
    const auto frag = static_cast<double>(fs.reclaimable_bytes()) /
                      static_cast<double>(fs.total_bytes);
    if (frag > 0.0 && frag > opts.fragmentation_threshold)
      candidates.emplace_back(frag, fid);
  }
  std::ranges::sort(candidates, std::greater<>{});

  for (const auto &[frag, fid] : candidates) {
    const auto target = *stats_snap.get(fid);
    // Fast path: nothing in the file needs keeping — skip the scan and drop
    // it. A tombstone may need keeping even with no live key left: dropping
    // it could let recovery resurrect a Put it shadows in an older file, and
    // only compaction decides which tombstones can go. Nor may a file go
    // whole while it holds an entry above retain_after, or a change marker,
    // which is kept for ever.
    if (target.live_bytes == 0 && target.tombstone_bytes == 0 &&
        target.change_marker_bytes == 0 &&
        target.max_sequence <= opts.retain_after) {
      vacuum_remove_file(fid);
      return true;
    }
    // Compaction returns false when it cannot make the file smaller: every
    // dead entry in it may be above retain_after. The next candidate may
    // still have something to reclaim.
    if (vacuum_compact_file(fid, opts.retain_after)) return true;
  }
  return false;
}

#pragma endregion

#pragma region File rotation

// Drops the old active file's preallocated tail, opens it read-only,
// dispatches hint generation, and opens a new writable active file.
// Caller must sync the active file before calling if durability is required.
auto DB::prepare_rotation(TransientEngineState &t) -> PreparedRotation {
  wait_for_hint_backlog();
  t.active_file().shrink_to_fit();
  auto read_only_old = openDataFileForRead(t.active_file().path(), io_backend_, pool_,
                                         t.active_file_id());
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_rotate_file_creation);
#endif
  const auto new_file_id = t.reserve_file_id();
  auto new_file = create_active_file(new_file_id, "io_dir_sync_rotate");
  return {std::move(read_only_old), std::move(new_file), new_file_id};
}

void DB::finish_rotation(TransientEngineState &t, PreparedRotation r) {
#ifdef BYTECASK_TESTING
  if (test_in_finish_rotation_) test_in_finish_rotation_();
#endif
  t.apply_rotate_file(r.sealed, std::move(r.next), r.next_id);
  // The sealed file's frames become evictable and the new file's pinned.
  if (pool_) pool_->set_active_file(r.next_id);
  dispatch_hint(std::move(r.sealed));
}

auto DB::create_active_file(std::uint32_t file_id, const char *checkpoint)
    -> std::shared_ptr<WritableDataFile> {
  auto file = createDataFileForWrite(dir_, make_data_file_stem(), ".data",
                                     rotation_threshold_, io_backend_, pool_,
                                     file_id);
  // On failure the created file is left behind empty, which is the state a
  // crash right after the create leaves too; the next open recovers it.
  sync_directory(dir_, checkpoint);
  return file;
}

// The wait runs on the writer's thread with the write path held, so every
// writer stalls until the worker catches up; readers never touch the worker.
// It cannot deadlock: a hint task takes no lock a writer holds —
// flush_hints_for is static and reads the file outside the buffer pool.
void DB::wait_for_hint_backlog() {
  if (max_hint_backlog_ == 0 || worker_.pending() < max_hint_backlog_) return;
  const auto start = std::chrono::steady_clock::now();
  worker_.wait_pending_below(max_hint_backlog_);
  const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);
  counters_.hint_backpressure_stalls.fetch_add(1, std::memory_order_relaxed);
  counters_.hint_backpressure_stall_us.fetch_add(waited.count(),
                                                 std::memory_order_relaxed);
}

void DB::dispatch_hint(std::shared_ptr<DataFile> file) {
#ifdef BYTECASK_TESTING
  worker_.dispatch([f = std::move(file), d = dir_, hook = test_before_hint_] {
    if (hook) hook();
    flush_hints_for(f, d);
  });
#else
  worker_.dispatch(
      [f = std::move(file), d = dir_] { flush_hints_for(f, d); });
#endif
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

// True when the error says a file's bytes are bad or cannot be delivered — a
// CRC or parse failure, EIO, a short read — which a rebuild answers for a
// hint and a skip for a data file. False for an error of the process or the
// environment (EMFILE, ENOMEM, EACCES), which says nothing about the file:
// acting on the file would turn a transient failure into a lost index, or a
// lost file.
static auto is_file_damage(const std::exception &e) noexcept -> bool {
  if (const auto *se = dynamic_cast<const std::system_error *>(&e))
    return se->code() == std::errc::io_error;
  return dynamic_cast<const std::runtime_error *>(&e) != nullptr;
}

auto DB::open_hint_or_rebuild(const std::shared_ptr<DataFile> &data_file,
                              const std::filesystem::path &hint_path)
    -> HintFile {
  try {
    return HintFile::OpenForRead(hint_path);
  } catch (const std::exception &e) {
    if (!is_file_damage(e)) throw;
    // flush_hints_for leaves an existing hint alone, so the damaged one has
    // to go first. Nothing is lost by removing it: it is unreadable either
    // way, and a rebuild that does not finish here leaves the file hint-less,
    // which the next open regenerates through the same scan.
    std::filesystem::remove(hint_path);
    (void)flush_hints_for(data_file, hint_path.parent_path());
    auto hint = HintFile::OpenForRead(hint_path);
    // The tail-drop recovery_prepare_files does for a hint-less file is not
    // repeated here: a file that had a hint at all was sealed, and sealing
    // already gave its preallocated tail back.
    std::fprintf(stderr,
                 "bytecask: rebuilt hint file '%s' from its data file: %s\n",
                 hint_path.string().c_str(), e.what());
    return hint;
  }
}

auto DB::open_hint_or_skip(const std::shared_ptr<DataFile> &data_file,
                           const std::filesystem::path &hint_path, bool strict)
    -> std::optional<HintFile> {
  try {
    return open_hint_or_rebuild(data_file, hint_path);
  } catch (const std::exception &e) {
    if (strict || !is_file_damage(e)) throw;
    std::fprintf(stderr,
                 "bytecask: skipping data file for hint '%s' — could not "
                 "read it or rebuild it from the data file: %s\n",
                 hint_path.string().c_str(), e.what());
    return std::nullopt;
  }
}

auto DB::flush_hints_for(const std::shared_ptr<DataFile> &file,
                         const std::filesystem::path &dir,
                         const TailCheck &check) -> std::optional<Offset> {
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

  auto committed =
      scan_committed(*file, 0, check ? OnDamage::Stop : OnDamage::Throw);
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
    if (entry.entry_type == EntryType::ChangeMarker) {
      // Recovery rebuilds the marker list from hints alone, so the id goes
      // with the entry. Keyless, like the batch markers, ahead of the run.
      hint.append_change_marker(entry.sequence, entry_off,
                                decode_marker_id(entry.value));
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

  if (check) {
    try {
      check(it.committed_offset());
    } catch (...) {
      std::error_code ec;
      std::filesystem::remove(tmp_path, ec);
      throw;
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
  // Only restart time depends on this: a lost rename leaves the file
  // hint-less, and the next open rebuilds the hint from it.
  sync_directory(dir, "io_dir_sync_hint");
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
// Live Puts (still current in snap->key_dir for source_file_id), the
// tombstones recovery found still needed or never examined, and
// BulkBegin/BulkEnd markers are emitted.
// Incomplete batches at EOF are silently discarded by the iterator.
auto DB::vacuum_scan_and_copy(
    const std::shared_ptr<const EngineState> &snap,
    const DataFile &source_file, WritableDataFile &dest_file,
    std::uint32_t source_file_id, const NeededTombstones &needed,
    std::uint64_t retain_after) -> VacuumScanResult {
  VacuumScanResult result;

  // The source is scanned in file order, where sequences ascend.
  auto track_seq = [&](std::uint64_t seq) {
    if (result.min_sequence == 0) result.min_sequence = seq;
    if (seq > result.max_sequence) result.max_sequence = seq;
  };

  auto emit_entry = [&](const DataEntry &entry, Offset entry_off) {
    switch (entry.entry_type) {
    case EntryType::Put: {
      const auto existing = kd_get(snap->key_dir, entry.key, snap->kd_ctx());
      // A location names one record, so an entry still at this one is this
      // record, sequence and all.
      if (existing && existing->file_id() == source_file_id &&
          existing->file_offset() == entry_off) {
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
      } else if (entry.sequence > retain_after) {
        // Dead, but a follower may still need it: kept as history. It is
        // counted in total_bytes and not in live_bytes, so it stays
        // reclaimable for a later vacuum that may drop it.
        std::ignore = dest_file.append_entry(entry.sequence, EntryType::Put,
                                             entry.key, entry.value);
        result.total_bytes += entry_size(entry.key.size(), entry.value.size());
        track_seq(entry.sequence);
      }
      break;
    }
    case EntryType::Delete:
    case EntryType::RangeDel: {
      // A tombstone no older Put in another file needs goes with its file.
      if (entry.sequence <= retain_after && needed.droppable(entry.sequence)) {
        ++result.tombstones_dropped;
        break;
      }
      std::ignore = dest_file.append_entry(entry.sequence, entry.entry_type,
                                           entry.key, entry.value);
      const auto sz = entry_size(entry.key.size(), entry.value.size());
      result.total_bytes += sz;
      result.tombstone_bytes += sz;
      track_seq(entry.sequence);
      break;
    }
    case EntryType::ChangeMarker: {
      // Kept for ever: the identity of the history from here on (#397).
      std::ignore = dest_file.append_entry(entry.sequence, entry.entry_type,
                                           {}, entry.value);
      const auto sz = entry_size(0, entry.value.size());
      result.total_bytes += sz;
      result.change_marker_bytes += sz;
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
      result.marker_bytes += kHeaderSize + kCrcSize;
      track_seq(entry.sequence);
      break;
    }
  };

  for (const auto &[entry, entry_off] : scan_committed(source_file)) {
    emit_entry(entry, entry_off);
  }

  return result;
}

// Remaps key_dir entries from old_file_id to new_sealed_file, updates the
// files map and file_stats, and publishes the new EngineState. A null
// new_sealed_file removes old_file_id (see apply_vacuum). Caller must hold
// write_mu_.
void DB::vacuum_commit(std::uint32_t old_file_id,
                             const VacuumScanResult &scan,
                             std::shared_ptr<DataFile> new_sealed_file,
                             std::uint32_t dest_file_id,
                             std::uint64_t decided_at) {
  auto current = load_state_for_write();
  auto t = current->transient();
  // A record left out as superseded is gone for good once the source is
  // unlinked, but what superseded it may be a sync=false write a crash can
  // still lose, taking the key's last durable value with it (#245). So
  // everything the scan saw becomes durable before the source can go.
  if (t.durable_seq() < decided_at) {
    // A sync after a failed one proves nothing (#231).
    if (refused(*current)) throw DbDegraded{degraded_reason()};
    sync_active_file(t, current, "vacuum");
  }
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
  // Its frames would otherwise hold a frame each until the SIEVE hand came
  // round, which in a pool that rarely misses is a long time.
  if (pool_) {
    pool_->release_file(file_id, old_data_file->size());
  }
  counters_.vacuum_files_unlinked.fetch_add(1, std::memory_order_relaxed);
}

// Rewrites a sealed file into a new sealed file containing only live
// entries, markers and the tombstones still needed. Called under vacuum_mu_,
// not write_mu_.
// The new data file is written to .data.tmp, then renamed atomically.
// The old file is deferred for cleanup when no readers reference it.
auto DB::vacuum_compact_file(std::uint32_t file_id, std::uint64_t retain_after)
    -> bool {
  auto snap = load_state_for_write();
  // Checked again where the id is reserved; here so a vacuum that cannot
  // finish does not copy the file first.
  if (snap->next_file_id > KeyDirEntry::kMaxFileId) throw file_ids_exhausted();
  const auto &old_file = **snap->files.get(file_id);

  const auto stem = make_data_file_stem();
  const auto tmp_data_path = dir_ / (stem + ".data.tmp");
  const auto final_data_path = dir_ / (stem + ".data");

  // Until the commit publishes it, vacuum's copy is removed on every exit —
  // the early returns below and any failure: scan, copy, sync, shrink, the
  // rename, and after the rename the directory sync, the open, the hint and
  // the commit itself. A vacuum retried under a persistent fault (ENOSPC,
  // EMFILE) would otherwise leave a full copy behind per attempt, and the next
  // open runs a recovery pass per copy (#235, #304). Before the commit the
  // published state still holds the source, so nothing reads the copy. A
  // removal that fails under the same fault, or that a power cut loses, is
  // left to recovery, which deletes a .data.tmp and a placed copy beside its
  // source at open; it must not replace the exception in flight.
  struct CopyCleanup {
    std::vector<std::filesystem::path> paths;
    bool armed{true};
    ~CopyCleanup() {
      if (!armed) return;
      std::error_code ec;
      for (const auto &p : paths) std::filesystem::remove(p, ec);
    }
  } cleanup{{tmp_data_path}};

  VacuumScanResult scan;
  {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_vacuum_compact_tmp_create);
#endif
    auto tmp_file = createDataFileForWrite(
        dir_, stem, ".data.tmp", rotation_threshold_,
        stagingBackend(io_backend_));
    scan = vacuum_scan_and_copy(snap, old_file, *tmp_file, file_id,
                                needed_tombstones_, retain_after);
    tmp_file->sync();
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_vacuum_compact_shrink);
#endif
    tmp_file->shrink_to_fit();
  }

  // Nothing to reclaim: every byte in this file is live data, a needed
  // tombstone or a batch marker, and compaction must preserve all three.
  // Publishing an identical file would churn I/O for nothing.
  const auto old_total = snap->file_stats.get(file_id)->total_bytes;
  if (scan.total_bytes >= old_total) return false;
  // Nothing left at all: every Put was dead and every tombstone could go.
  // Remove the file rather than publish an empty one.
  if (scan.total_bytes == 0) {
    vacuum_remove_file(file_id);
    counters_.vacuum_tombstones_dropped.fetch_add(
        static_cast<std::int64_t>(scan.tombstones_dropped),
        std::memory_order_relaxed);
    return true;
  }

#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_vacuum_compact_rename);
#endif
  // Exclusive placement, not std::filesystem::rename: vacuum stages its copy
  // holding only vacuum_mu_, so a concurrent rotation can mint this stem
  // between here and the staging create. renameDataFileExclusive refuses the
  // target instead of replacing it.
  renameDataFileExclusive(tmp_data_path, final_data_path);
  // Placed: from here the copy is the final file and the hint flush_hints_for
  // writes for it, staged or renamed, hint first as recovery removes them.
  // Only once the rename succeeded: until then the final name may be a file
  // rotation minted. Frames the hint scan admits under the buffer pool are
  // left to eviction if the copy goes: their file id was reserved for it and
  // is never reused.
  cleanup.paths = {dir_ / (stem + ".hint.tmp"), dir_ / (stem + ".hint"),
                   final_data_path};
  // The source is unlinked once this commits. Were that unlink durable and
  // the rename not, the next open would find only .data.tmp, delete it as
  // staging, and lose every live entry the source held.
  sync_directory(dir_, "io_dir_sync_vacuum");
#ifdef BYTECASK_TESTING
  // *Orphaned .data files* in docs/correctness_validation.md: the rename
  // completed and the commit did not. The compacted file is on disk under its
  // final name while the old one is still the published state's; cleanup
  // removes it.
  FAULT_INJECTION(io_vacuum_compact_post_rename);
  if (test_before_vacuum_commit_) test_before_vacuum_commit_();
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
    vacuum_commit(file_id, scan, new_file, dest_file_id,
                  snap->next_seq > 0 ? snap->next_seq - 1 : 0);
  }
  // Published: the copy is the file now. A failure from here on, the source's
  // unlink, leaves the source beside it, which open resolves.
  cleanup.armed = false;
  // Bytes reclaimed = the shrinkage, not old_total - live_bytes: the compacted
  // file also carries the tombstones and markers that had to be preserved.
  counters_.vacuum_bytes_reclaimed.fetch_add(
      static_cast<std::int64_t>(old_total - scan.total_bytes),
      std::memory_order_relaxed);
  counters_.vacuum_tombstones_dropped.fetch_add(
      static_cast<std::int64_t>(scan.tombstones_dropped),
      std::memory_order_relaxed);
  counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
  vacuum_unlink_old_file(snap, file_id);
  return true;
}

// Removes a sealed file that has no live keys and nothing compaction would
// keep: no tombstone (vacuum() checks) or none still needed (the compaction
// that found its output empty). No I/O scan needed — just commit the state
// change and unlink the files. Called under vacuum_mu_.
void DB::vacuum_remove_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  auto old_total = snap->file_stats.get(file_id)->total_bytes;
  {
    WriteBarrier barrier{*this};
    VacuumScanResult empty{};
    // No new sealed file, so no id is consumed.
    vacuum_commit(file_id, empty, nullptr, 0,
                  snap->next_seq > 0 ? snap->next_seq - 1 : 0);
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
  return refused(*load_state());
}

auto DB::degraded_reason() const -> std::string {
  const auto s = load_state();
  if (s->degraded) return s->degraded_reason;
  if (write_fault_.load(std::memory_order_acquire)) return kWriteFaultReason;
  return {};
}

auto DB::stats() const -> std::map<std::string, std::int64_t> {
  scrape_read_caches();  // see vacuum()
  auto s = load_state();
  if (s->closed) throw DbClosed{};
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
      // Gauge, process-wide: freed key directory nodes held for reuse on the
      // node pool's shared list (thread caches not counted).
      {"bytecask.keydir_pool_bytes", btree_detail::node_pool_bytes()},
      {"bytecask.bytes_written",
       counters_.bytes_written.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_batches",
       counters_.group_writer_batches.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_coalesced",
       counters_.group_writer_coalesced.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_busy_us",
       counters_.group_writer_busy_ns.load(std::memory_order_relaxed) / 1000},
      {"bytecask.file_rotations",
       counters_.file_rotations.load(std::memory_order_relaxed)},
      {"bytecask.fsyncs",
       counters_.fsyncs.load(std::memory_order_relaxed)},
      {"bytecask.commit_wait_blocked",
       counters_.commit_wait_blocked.load(std::memory_order_relaxed)},
      {"bytecask.flush_settles",
       counters_.flush_settles.load(std::memory_order_relaxed)},
      {"bytecask.commit_delay_waits", commit_delay_.waits()},
      {"bytecask.commit_delay_us",
       std::chrono::duration_cast<std::chrono::microseconds>(commit_delay_.waited()).count()},
      {"bytecask.commit_delay_fsync_us",
       std::chrono::duration_cast<std::chrono::microseconds>(commit_delay_.fsync_estimate()).count()},
      {"bytecask.commit_delay_round_trip_us",
       std::chrono::duration_cast<std::chrono::microseconds>(commit_delay_.round_trip_estimate()).count()},
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
      {"bytecask.pool_frames_released",
       pool ? pool->frames_released.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_direct_io_fallbacks",
       pool ? pool->direct_io_fallbacks.load(std::memory_order_relaxed) : 0},
      {"bytecask.vacuum_bytes_reclaimed",
       counters_.vacuum_bytes_reclaimed.load(std::memory_order_relaxed)},
      {"bytecask.vacuum_files_unlinked",
       counters_.vacuum_files_unlinked.load(std::memory_order_relaxed)},
      {"bytecask.vacuum_tombstones_dropped",
       counters_.vacuum_tombstones_dropped.load(std::memory_order_relaxed)},
      {"bytecask.tombstones_needed",
       std::ssize(needed_tombstones_.sequences)},
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
      {"bytecask.hint_backpressure_stalls",
       counters_.hint_backpressure_stalls.load(std::memory_order_relaxed)},
      {"bytecask.hint_backpressure_stall_us",
       counters_.hint_backpressure_stall_us.load(std::memory_order_relaxed)},
      // Gauges — current state, not monotonic.
      {"bytecask.degraded", refused(*s) ? 1 : 0},
      // Sealed files whose hint is queued or being written. What close must
      // still write, and what an open after a crash would rebuild.
      {"bytecask.hint_backlog", narrow<std::int64_t>(worker_.pending())},
      {"bytecask.open_files", open_files},
  };
}

void DB::set_mode(Mode mode) {
  WriteBarrier barrier{*this};
  auto current = load_state_for_write();
  if (current->closed) throw DbClosed{};
  if (mode == current->mode) return;
  auto t = current->transient();
  // A leader stepping down makes every write it acknowledged durable, and
  // so shippable: changes_since stops at durable_sequence, and a sync=false
  // write left above it would never reach the next leader, which then
  // reuses its sequence. Only a leader can hold such a write: ingest syncs
  // before it publishes.
  const auto last_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
  if (mode == Mode::Follower) {
    if (!refused(*current) && t.durable_seq() < last_seq) {
      sync_active_file(t, current, "set_mode(Follower)");
    }
  } else {
    // A promotion. The marker names the history from here on, so it has to
    // be on disk before any write of the new leader is, and the mode must
    // not change without it: a degraded engine cannot write it and stays a
    // follower (#397).
    if (refused(*current)) throw DbDegraded{degraded_reason()};
    const auto seq = t.next_seq();
    // The marker is a write: past the last packable sequence it is refused
    // as a write is, and the engine stays a healthy follower.
    if (seq > KeyDirEntry::kMaxSequence) {
      throw std::runtime_error{std::format(
          "sequence space exhausted: a promotion needs sequence {}, past the "
          "limit of {}",
          seq, KeyDirEntry::kMaxSequence)};
    }
    const auto id = random_marker_id();
    const auto id_bytes = encode_marker_id(id);
    auto &file = t.active_file();
    try {
      std::ignore = file.append_entry(seq, EntryType::ChangeMarker, {},
                                      id_bytes);
    } catch (...) {
      auto ex = std::current_exception();
      try { file.sync(); } catch (...) {}
      store_state(current->degraded_copy(
          "set_mode(Leader) marker append IO error: call resume() to "
          "recover."));
      std::rethrow_exception(ex);
    }
    // From here the marker is in the file. A failure below that no handler
    // degrades for — an allocation, typically — would leave a healthy
    // follower whose next promotion appends a second marker under the same
    // sequence, so writes are refused until resume() reconciles the file, as
    // after any append (#364).
    try {
      t.apply_change_marker(seq, id);
      counters_.bytes_written.fetch_add(
          static_cast<std::int64_t>(
              change_marker_size(EntryType::ChangeMarker)),
          std::memory_order_relaxed);
      // Publishes t degraded, marker included, on failure: the entry is in
      // the file, and resume() finds it there either way.
      sync_active_file(t, current, "set_mode(Leader)");
      if (t.is_rotation_needed(rotation_threshold_) &&
          t.file_ids_left() > 0) {
        PreparedRotation rotation;
        try {
          rotation = prepare_rotation(t);
        } catch (...) {
          t.apply_degrade(
              "set_mode(Leader) rotation file creation failed: call "
              "resume().");
          store_state(current, std::move(t).persistent());
          throw;
        }
        finish_rotation(t, std::move(rotation));
      }
      t.apply_set_mode(mode);
      store_state(current, std::move(t).persistent());
    } catch (...) {
      refuse_writes();
      throw;
    }
    return;
  }
  t.apply_set_mode(mode);
  store_state(current, std::move(t).persistent());
}

void DB::sync_active_file(TransientEngineState &t,
                          const std::shared_ptr<const EngineState> &current,
                          std::string_view caller) {
  try {
    t.active_file().sync();
    counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
  } catch (...) {
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    t.apply_degrade(std::format(
        "{} fdatasync failed on '{}': writes acknowledged without sync are "
        "not confirmed durable. Call resume() to recover.",
        caller, t.active_file().path().string()));
    store_state(current, std::move(t).persistent());
    throw;
  }
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
}

void DB::deem_as_degraded(std::string reason) {
  store_state(load_state()->degraded_copy(std::move(reason)));
  // A waiter on a publication that will now never come re-checks and sees
  // the degrade. Not under durable_mu_ here — no caller holds it.
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

void DB::resume() {
  if (auto s = load_state(); s->closed) {
    throw DbClosed{};
  } else if (!refused(*s)) {
    return;
  }

  WriteBarrier barrier{*this};
  auto current = load_state_for_write();
  if (current->closed) throw DbClosed{};
  if (!refused(*current)) return;  // re-check under lock

  // The failed flush left one or two heads derived from the published
  // state alive in head_. The key directory derives a version only from
  // the end of its chain, so drop them now — ~FlushRole would only do it at
  // the end of the barrier — and the resumed state is derived from the
  // published one with those heads already reclaimed.
  store_head(current);

  // resume() seals the active file like a rotation does.
  wait_for_hint_backlog();

  auto t = current->transient();
  // resume() seals the active file and needs an id for the next one; without
  // it nothing is touched and the engine stays degraded until a reopen.
  if (t.file_ids_left() == 0) throw file_ids_exhausted();
  const auto old_file_id = t.active_file_id();
  auto &file = t.active_file();

  // The degrade may be a failed fdatasync, which on Linux leaves the pages
  // it covered clean without writing them (#231): every byte appended since
  // the last successful sync, sync=false writes included, reads back from
  // the cache and is not on the device, and file.sync() below would find
  // nothing to write. Rewriting the file and syncing it makes what reads
  // return durable before the scan reads it (throws → stays degraded). Up to
  // the logical end, where the scan stops: past it lie only the zero-filled
  // preallocation and the bytes of an append that failed.
  rewrite_durably(file.path(), file.size());

  // Scan the active file to find the last valid committed offset
  // and collect valid committed entries for key_dir replay. Entries written to
  // disk but never published to EngineState (sync-failure paths, degraded
  // transitions between IO and state publication) would otherwise be invisible
  // until cold restart. The scan goes through read_raw, a pread of the page
  // cache on every back-end — never the buffer pool's frames, which hold what
  // was appended rather than what the rewrite found and made durable.
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
                           std::move(range_end),
                           entry.entry_type == EntryType::ChangeMarker
                               ? decode_marker_id(entry.value)
                               : 0});
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
  // scan that stops short of it has found bytes readers were served that the
  // file no longer holds: damage, or sync=false writes a failed fdatasync
  // left undurable and the kernel evicted before the rewrite read them. The
  // key directory holds no older version to fall back to for the keys they
  // overwrote, so there is no consistent state to resume into, and resume()
  // refuses — before truncating, so the file is left exactly as it was found
  // and the engine stays degraded. A reopen recovers the state the device
  // holds. This is detection, not repair: resume() makes no promise about
  // what a damaged file still holds, only that it will not truncate
  // acknowledged bytes or report success over them.
  if (valid_offset < published_extent) {
    throw std::runtime_error{std::format(
        "resume: active file '{}' ends at offset {}, inside data already "
        "published (up to {}): damaged, or unsynced writes lost after a "
        "failed fdatasync; refusing to truncate it. Reopen the database to "
        "recover what the file holds",
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
  dispatch_hint(read_only_old);

  // Create the new active file (may throw → stays degraded).
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_resume_file_creation);
#endif
  const auto new_file_id = t.reserve_file_id();
  auto new_file = create_active_file(new_file_id, "io_dir_sync_resume");

  // Build and publish new state. Replay scanned entries into key_dir so that
  // entries on disk but not yet in EngineState become visible.
  t.apply_resume(old_file_id, committed, valid_offset);
  t.apply_rotate_file(std::move(read_only_old), std::move(new_file),
                      new_file_id);
  if (pool_) pool_->set_active_file(new_file_id);
  // Every entry replayed was read after rewrite_durably synced it.
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
  t.apply_clear_degraded();
  auto resumed = std::move(t).persistent();
#ifdef BYTECASK_TESTING
  if (test_before_validate_) test_before_validate_(*resumed);
#endif
  validate_state_consistency(*resumed);
  store_state(current, std::move(resumed));
  // Writers appended since the failed flush have all been told; new ones
  // start clean.
  std::lock_guard<std::mutex> lk{durable_mu_};
  flush_error_ = nullptr;
  write_fault_.store(false, std::memory_order_release);
}

void DB::wait_published(std::uint64_t sequence) const {
  std::unique_lock<std::mutex> lk{durable_mu_};
  durable_cv_.wait(lk, [&] {
#ifdef BYTECASK_TESTING
    if (test_in_sequence_wait_) test_in_sequence_wait_();
#endif
    const auto s = load_state();
    // mcdc-exempt(C3): close() takes the write barrier, which waits for the
    // flush this waiter's sequence is in, so by the time a state is closed
    // that flush has either published the sequence (C1) or degraded (C2).
    // The test stays so a waiter can never outlive the engine.
    return s->next_seq > sequence || refused(*s) || s->closed;
  });
}

auto DB::durable_sequence(std::uint64_t min_sequence,
                         std::chrono::milliseconds timeout) const
    -> std::uint64_t {
  const auto s = load_state();
  if (s->closed) throw DbClosed{};
  if (min_sequence == 0 || s->durable_seq >= min_sequence
      || timeout <= std::chrono::milliseconds{0}) {
    return s->durable_seq;
  }

  const auto reached = [&] {
#ifdef BYTECASK_TESTING
    if (test_in_sequence_wait_) test_in_sequence_wait_();
#endif
    const auto cur = load_state();
    return cur->durable_seq >= min_sequence || cur->closed;
  };
  std::unique_lock<std::mutex> lk{durable_mu_};
  // wait_for adds the timeout to now() in nanoseconds, which overflows for a
  // timeout such as milliseconds::max(). One past what the clock can
  // represent waits with no deadline.
  const auto now = std::chrono::steady_clock::now();
  const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::time_point::max() - now);
  if (timeout >= room) {
    durable_cv_.wait(lk, reached);
  } else {
    durable_cv_.wait_until(lk, now + timeout, reached);
  }
  const auto last = load_state();
  if (last->closed) throw DbClosed{};
  return last->durable_seq;
}

auto DB::create_manifest() -> FileManifest {
  std::shared_ptr<const EngineState> manifest_state;
  std::uint64_t through_seq;
  {
    WriteBarrier barrier{*this};

    auto current = load_state_for_write();
    if (current->closed) throw DbClosed{};
    if (refused(*current)) throw DbDegraded{degraded_reason()};

    auto t = current->transient();

    // Make every entry durable before sealing. A failure degrades: the pages
    // it left clean are written by no later fdatasync (#231, #281).
    sync_active_file(t, current, "create_manifest");
    const auto max_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;

    // Seal active file, dispatch hint generation, open new active.
    PreparedRotation rotation;
    try {
      rotation = prepare_rotation(t);
    } catch (...) {
      t.apply_degrade(
          "create_manifest rotation failed: active file is sealed "
          "but new file could not be created. Call resume() to recover.");
      store_state(current, std::move(t).persistent());
      throw;
    }
    try {
      finish_rotation(t, std::move(rotation));
    } catch (...) {
      // The active file is sealed on disk and t half rotated: refused, so
      // nothing is appended to the sealed file before resume().
      refuse_writes();
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
// The hot path is two plain loads (publishing_, state_gen_; MOVs on x86)
// plus an owner-pointer compare. The entry is refreshed whenever a
// publication is in progress or has completed since it was filled, and
// whenever it holds a different DB instance's state. Returns a
// reference to the thread-local snapshot. The snapshot stays alive until
// the same thread calls load_state_for_read again, so callers must not
// stash the reference across a second load_state_for_read call.
auto DB::read_cache() -> ReadCacheSlot & {
  // Per-thread slot — thread-exit destructor is intentional: it is what
  // leaves the registry.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // TLS: per-DB — entries name their DB, and close() and ~DB take them from
  // every thread's slot, so no later DB at the same address finds one. Tested
  // by "closing a DB releases the state every thread's read cache holds" and
  // "BC-243: thread-local read cache does not leak across DB instances".
  thread_local ReadCacheSlot tl;
#pragma clang diagnostic pop
  return tl;
}

auto DB::load_state_for_read() const
    -> ReadStateGuard {
  auto &slot = read_cache();
  auto *e = slot.claim();
  // owner identifies which DB instance the entry belongs to: without it a
  // thread that reads from two DBs could see one DB's generation while
  // querying the other. An entry of another DB is dropped, not reused —
  // this DB must never take ownership of another DB's state.
  if (e == nullptr || e->owner.load(std::memory_order_relaxed) != this) {
    if (e == nullptr) {
      e = new ReadCacheEntry();
    } else {
      e->state.reset();
    }
    e->owner.store(this, std::memory_order_relaxed);
    e->gen = kNoStateGen;
  }
  // A read must see every state published before it began, and every state
  // another thread has already seen: a marker stored after the state could
  // not tell a reader that a publication it has not seen finish is already
  // visible to others. publishing_ can (see the raw store_state).
  const auto busy = publishing_.load(std::memory_order_acquire);
  const auto gen = state_gen_.load(std::memory_order_acquire);
  if (busy != 0 || gen != e->gen) {
    e->state = load_state();
    e->gen = busy != 0 ? kNoStateGen : gen;
  }
  e->used_epoch.store(ReadCacheRegistry::instance().epoch(),
                      std::memory_order_relaxed);
  if (e->state->closed) {
    slot.release(e);
    throw DbClosed{};
  }
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
  // close() publishes once; nothing publishes after it.
  const auto became_closed = new_state->closed;

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
      const auto fs = new_state->file_stats.get(loc.file_id());
      if (!fs) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {}, which has no file_stats",
            loc.file_id()));
        return;
      }
      if (loc.file_offset() >= fs->total_bytes) {
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
    // (invariant P — see "Why a lent view survives file events" in
    // docs/bytecask_design.md).
    std::uint64_t max_seq = 0;
    for (auto it = kd_begin(new_state->key_dir, new_state->kd_ctx(/*verify=*/false));
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (entry.sequence() > max_seq) max_seq = entry.sequence();
      const auto entry_end = entry.file_offset() +
                             entry_size(key_span.size(), entry.value_size());
      const auto fs = new_state->file_stats.get(entry.file_id());
      if (!fs) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {}, which has no file_stats",
            entry.file_id()));
        return;
      }
      if (entry_end > fs->total_bytes) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {} ends at {} but the file's "
            "committed extent is {}",
            entry.file_id(), entry_end, fs->total_bytes));
        return;
      }
    }
    // next_seq starts at 1, so an empty key directory passes.
    if (new_state->next_seq <= max_seq) {
      deem_as_degraded(std::format(
          "invariant violation: next_seq {} <= max key_dir sequence {}",
          new_state->next_seq, max_seq));
      return;
    }
  }
#endif

  store_state(std::move(new_state));

  // The version this publish superseded is freed once nothing holds it;
  // an idle thread's read cache is the holder nothing else can reach.
  scrape_read_caches();

  // Wakes the sequence waiters: durable_sequence on a durable advance, and
  // wait_published on any publication, which is why a nosync publish that
  // advances no durable sequence notifies too. (A failed flush publishes
  // through the raw store under durable_mu_ and finish_flush notifies for
  // it; deem_as_degraded notifies for itself.)
  if (durable_advanced || published_advanced || became_degraded ||
      became_closed) {
    { std::lock_guard<std::mutex> lk{durable_mu_}; }
    durable_cv_.notify_all();
  }
}

void DB::store_initial_state(std::shared_ptr<EngineState> s) {
  store_head(s);
  store_state(std::move(s));
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

  // 5. The marker list is in sequence order and below next_seq.
  if (const auto *ms = s.change_markers.get()) {
    std::uint64_t prev = 0;
    for (const auto &m : *ms) {
      if (m.since_sequence <= prev) {
        throw std::runtime_error{std::format(
            "state consistency: change marker at {} out of order after {}",
            m.since_sequence, prev)};
      }
      if (m.since_sequence >= s.next_seq) {
        throw std::runtime_error{std::format(
            "state consistency: change marker at {} at or above next_seq {}",
            m.since_sequence, s.next_seq)};
      }
      prev = m.since_sequence;
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
    // leave a reader's span addressing a page beyond EOF. See "Why a lent
    // view survives file events" in docs/bytecask_design.md.
    // Check 4 put every registered file in file_stats.
    const auto size = entry_size(key_span.size(), entry.value_size());
    const auto entry_end = entry.file_offset() + size;
    if (entry_end > s.file_stats.get(entry.file_id())->total_bytes) {
      throw std::runtime_error{std::format(
          "state consistency: key in file_id {} ends at {} but the file's "
          "committed extent is {}",
          entry.file_id(), entry_end,
          s.file_stats.get(entry.file_id())->total_bytes)};
    }
    computed_live[entry.file_id()] += size;
    if (entry.sequence() > max_seq) max_seq = entry.sequence();
  }

  if (max_seq > 0 && s.next_seq <= max_seq) {
    throw std::runtime_error{std::format(
        "state consistency: next_seq {} <= max key_dir sequence {}",
        s.next_seq, max_seq)};
  }

  for (const auto [file_id, fs] : s.file_stats.all()) {
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
  for (const auto [file_id, fs] : s.file_stats.all()) {
    if ((fs.min_sequence == 0) != (fs.max_sequence == 0)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} has min_sequence={} max_sequence={} "
          "(one is zero, the other is not)",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
    // Both are zero or neither is (above), so this needs no zero test.
    if (fs.min_sequence > fs.max_sequence) {
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
  for (const auto [fid, fs] : s.file_stats.all()) {
    if (fs.min_sequence > 0) {
      ranges.push_back({fs.min_sequence, fs.max_sequence, fid});
    }
  }
  // Sorted by min, a range overlaps an earlier one only if it overlaps the
  // one just before it: until an overlap is found the ranges are disjoint,
  // so the previous range is also the one reaching furthest.
  std::ranges::sort(ranges, {}, &Range::min);
  const Range *prev = nullptr;
  for (const auto &r : ranges) {
    if (prev != nullptr && r.min <= prev->max) {
      return std::pair{prev->file_id, r.file_id};
    }
    prev = &r;
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
    // A rerun after undoing an interrupted vacuum decides afresh; an empty
    // directory leaves it empty, which keeps every tombstone.
    needed_tombstones_ = {};
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
#else
        s = recovery_load_ranged(std::move(s), std::move(files),
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
  // Each data file takes an id, and the new active file one more.
  if (data_paths.size() > KeyDirEntry::kMaxFileId) {
    throw std::runtime_error{std::format(
        "DB::open: '{}' holds {} data files; at most {} can be opened",
        dir_.string(), data_paths.size(), KeyDirEntry::kMaxFileId)};
  }

  std::vector<RecoveredFile> files;
  auto files_t = s.files.transient();

  for (const auto &p : data_paths) {
    const auto file_id = s.next_file_id++;
    const auto hint_path = dir_ / (p.stem().string() + ".hint");
    const auto hintless = !std::filesystem::exists(hint_path);
    // A file without a hint was the active file when the last process
    // stopped, or a sealed file whose hint was not written yet. The active
    // file's last bytes may be in the page cache and not on the device: the
    // process was killed before its sync, or a sync failed and left its
    // pages clean without writing them (#231). A hint built from them would
    // outlive them at a power loss, and point into zeros. So they are made
    // durable before anything reads them — every hint-less file, since none
    // of them says which one was active.
    // No writer says where its entries end, so the whole file.
    if (hintless) rewrite_durably(p, std::filesystem::file_size(p));
    auto data_file =
        openDataFileForRead(p, io_backend_, pool_, file_id);

    if (hintless) {
      // Whatever lies past the file's last committed record goes, once
      // recovery_check_tail has ruled that it may: the preallocated tail a
      // crash leaves, or a torn write. That makes its physical size its
      // logical size, as for every other sealed file — file_size below is
      // what seeds total_bytes for vacuum.
      const auto end = flush_hints_for(data_file, dir_, [&](Offset e) {
        recovery_check_tail(*data_file, e, data_paths);
      });
      // The file had no hint, so flush_hints_for wrote one and returns the
      // end of what it indexed.
      if (end.value() < std::filesystem::file_size(p)) {
        data_file.reset();
        truncate_durably(p, *end);
        // Same file_id as the open above, deliberately. Under the buffer
        // pool that open's hint scan may have admitted frames under this id,
        // but the truncate only drops a tail: every byte below *end is
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


namespace {

// The sequence in the file's first header, 0 when there is none. Read
// without its CRC: in the file a crash tore, the first record may be the
// torn one.
auto first_sequence(const DataFile &file) -> std::uint64_t {
  std::array<std::byte, kHeaderSize> hdr{};
  if (file.read_raw(0, hdr) < kHeaderSize) return 0;
  return read_header(hdr).sequence;
}

// Whether every byte of file from end on is zero.
auto tail_is_zero(const DataFile &file, Offset end) -> bool {
  if (end >= file.size()) return true;
  std::vector<std::byte> buf(static_cast<std::size_t>(
      std::min<Offset>(Offset{1} << 20, file.size() - end)));
  for (auto off = end; off < file.size();) {
    const auto n = file.read_raw(off, buf);
    if (n == 0) break;
    if (std::ranges::any_of(std::span{buf}.first(n),
                            [](std::byte b) { return b != std::byte{0}; }))
      return false;
    off += n;
  }
  return true;
}

} // namespace

// Only the file written last can hold a record a crash tore: every other
// file was fdatasync'd whole before it was sealed. So past a hint-less file's
// last committed record there may be:
// - nothing but zeros: the preallocated tail of the active file. Sealing
//   truncates it and syncs the truncate, so a sealed file carries one only
//   if an earlier build left it. It goes either way.
// - anything else, in the file written last: a torn write, which was never
//   acknowledged durable. It goes, as PostgreSQL and RocksDB truncate their
//   log at the first bad record. Damage there is indistinguishable and goes
//   with it.
// - anything else, in any other file: damage in data that was synced.
//   Refused, in every mode, and the file is left as it was.
// "Written last" is read off sequences, which no two files share: no other
// file may start at a higher one. A file whose first header is zero has no
// sequence to compare. It may be the newest, its first page lost at a power
// cut and a later one kept, so it goes whole.
// Either way it is the one file a crash tore, so no other file may need such
// a cut. The files open handled before this one have their hints by now;
// the rest are scanned here, before anything is cut.
void DB::recovery_check_tail(
    const DataFile &file, Offset end,
    const std::vector<std::filesystem::path> &data_paths) {
  if (tail_is_zero(file, end)) return;
  const auto seq = first_sequence(file);
  for (const auto &other : data_paths) {
    if (other == file.path()) continue;
    const auto other_file = openDataFileForRead(other);
    if (seq != 0 && first_sequence(*other_file) > seq) {
      throw std::runtime_error{std::format(
          "bytecask: corrupt database — data file '{}' does not parse past "
          "offset {}, and '{}' holds later sequences, so it is not the file a "
          "crash could have torn; refusing to truncate it",
          file.path().string(), end, other.string())};
    }
    auto hint = other;
    hint.replace_extension(".hint");
    if (std::filesystem::exists(hint)) continue;
    auto committed = scan_committed(*other_file, 0, OnDamage::Stop);
    auto it = committed.begin();
    while (it != std::default_sentinel) ++it;
    if (!tail_is_zero(*other_file, it.committed_offset())) {
      throw std::runtime_error{std::format(
          "bytecask: corrupt database — data files '{}' and '{}' both hold "
          "data past their last committed record, and a crash can tear only "
          "the file being written; refusing to truncate either",
          file.path().string(), other.string())};
    }
  }
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

// Collects the tombstones recovery marked as needed into the sorted set
// vacuum consults. `horizon` is the highest sequence recovery saw: nothing
// above it was examined. After a lenient open that skipped a file, the
// horizon is 0 and every tombstone is kept: the skipped file may hold the
// Put a tombstone hides, and may be readable at the next open.
static auto recovery_needed_tombstones(
    std::vector<std::uint64_t> point, const std::vector<RangeTombstone> &ranges,
    std::uint64_t horizon, bool skipped_files) -> NeededTombstones {
  if (skipped_files) return {};
  for (const auto &rt : ranges)
    if (rt.needed) point.push_back(rt.seq);
  std::ranges::sort(point);
  const auto dup = std::ranges::unique(point);
  point.erase(dup.begin(), dup.end());
  return {horizon, std::move(point)};
}

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


// Marks the tombstones that one key's hint entries show are still needed.
// `matches` holds one entry per file: that file's newest entry for the key.
// A tombstone is needed when an older Put from another file sits on the key.
// An older Put a file hides under its own newer entry needs nothing more:
// that newer entry is a Put that outranks it, or a Delete in the same file
// that compaction drops together with it.
template <typename EntryOf, typename FileOf>
static void recovery_mark_needed(std::span<const std::byte> key,
                                 std::span<const std::size_t> matches,
                                 EntryOf entry_of, FileOf file_of,
                                 std::vector<RangeTombstone> &ranges,
                                 std::vector<std::uint64_t> &needed) {
  const auto older_put_elsewhere = [&](std::uint64_t seq, std::uint32_t file) {
    for (const auto j : matches) {
      const auto &p = entry_of(j);
      if (p.entry_type == EntryType::Put && p.sequence < seq &&
          file_of(j) != file)
        return true;
    }
    return false;
  };
  for (const auto i : matches) {
    const auto &d = entry_of(i);
    if (d.entry_type == EntryType::Delete &&
        older_put_elsewhere(d.sequence, file_of(i)))
      needed.push_back(d.sequence);
  }
  for (auto &rt : ranges) {
    if (rt.needed) continue;
    if (recovery_key_cmp(key, recovery_span_of(rt.start)) < 0) continue;
    if (recovery_key_cmp(key, recovery_span_of(rt.end)) >= 0) continue;
    if (older_put_elsewhere(rt.seq, rt.file_id)) rt.needed = true;
  }
}

// Builds a RecoveryResult by merging sorted hint runs straight into a bulk
// loader, instead of inserting key by key into a transient.
//
// Inserting key by key costs a descent, a slot shift and a split every
// fanout inserts, per key; per-phase timing put it at 90 of the B+ tree's
// 128 ms at 1M keys and 4 threads, which was the whole of its deficit against
// the radix tree it replaced. A bulk loader writes each key exactly once with
// no descent and no split, but it needs its keys in ascending order — which
// is what a sorted hint file gives.
//
// Requires the sorted hint files flush_hints_for writes. A sorted hint file is
// (batch markers and range tombstones, in scan order) followed by one run of
// Put and Delete entries sorted by key. The merge checks the order it is
// given and throws rather than handing a bulk loader keys it cannot take.
auto DB::recovery_build_sorted(std::span<RecoveredFile> files, bool strict)
    -> RecoveryResult {
  std::uint64_t max_seq = 0;
  std::map<Key, PointTombstone> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::vector<std::uint64_t> needed;
  bool skipped = false;
  std::vector<ChangeMarker> markers;
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
    fs.marker_bytes += marker_size(he.entry_type);
    fs.change_marker_bytes += change_marker_size(he.entry_type);
  };

  // One cursor per hint file, parked on its next Put or Delete. A scanner's
  // entries last only until its next call, so the cursor owns the entries it
  // keeps.
  struct Cursor {
    HintFile::Scanner scanner;
    std::uint32_t file_id;
    HintRecord cur;       // best entry for the key it sits on
    HintRecord lookahead; // first entry of the following key
    bool has_cur{false};
    bool has_lookahead{false};
  };
  std::vector<Cursor> cursors;
  cursors.reserve(files.size());

  // Phase A: the head of each file — markers and range tombstones — is read
  // up front, so every range tombstone this worker owns is known before any
  // Put is admitted.
  for (auto &[file_id, data_file, hint_path, tb] : files) {
    // Only a file that cannot be opened is skipped. An error once its entries
    // are being read fails the open: some of them are already noted.
    const auto hint = open_hint_or_skip(data_file, hint_path, strict);
    if (!hint) {
      skipped = true;
      continue;
    }
    Cursor c{hint->make_scanner(), file_id, {}, {}, false, false};
    while (auto he = c.scanner.next()) {
      note(file_id, *he);
      if (he->entry_type == EntryType::RangeDel) {
        range_tombstones.push_back(
            {Key{he->key}, Key{he->end_key}, he->sequence, file_id});
        continue;
      }
      if (he->entry_type == EntryType::BulkBegin ||
          he->entry_type == EntryType::BulkEnd) {
        continue;
      }
      if (he->entry_type == EntryType::ChangeMarker) {
        markers.push_back({he->sequence, he->marker_id});
        continue;
      }
      c.lookahead.assign(*he);
      c.has_lookahead = true;
      break;
    }
    cursors.push_back(std::move(c));
  }

  // Phase B: merge the runs. For each key the highest sequence across every
  // file decides; a Delete that wins drops the key, and every Delete is
  // recorded so other workers' entries for that key are suppressed too.
  KeyDirBulkLoader out;
  std::vector<std::size_t> matches;
  matches.reserve(cursors.size());

  // The next Put or Delete in this file, counting every entry it steps over
  // towards the file's sequence bounds.
  auto next_data = [&](Cursor &c) -> std::optional<HintEntry> {
    while (auto he = c.scanner.next()) {
      note(c.file_id, *he);
      switch (he->entry_type) {
      case EntryType::BulkBegin:
      case EntryType::BulkEnd:
      case EntryType::ChangeMarker:
        continue;  // keyless: nothing to park a cursor on
      case EntryType::RangeDel:
        // Range tombstones only ever sit in the head of a sorted file.
        throw std::runtime_error{
            "bytecask: range tombstone inside a sorted hint run"};
      case EntryType::Put:
      case EntryType::Delete:
        return *he;
      }
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
  // ~180 files made recovery 3x slower than the heap; the heap makes
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
      if (cursors[i].cur.sequence > slot.seq)
        slot = {cursors[i].cur.sequence, cursors[i].file_id};
    }
    recovery_mark_needed(
        key, matches,
        [&](std::size_t i) -> const HintRecord & { return cursors[i].cur; },
        [&](std::size_t i) { return cursors[i].file_id; }, range_tombstones,
        needed);

    if (winner.entry_type == EntryType::Put) {
      auto suppressed = false;
      for (const auto &rt : range_tombstones) {
        if (winner.sequence >= rt.seq) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt.start)) < 0) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt.end)) >= 0) continue;
        suppressed = true;
        break;
      }
      // Keys leave the heap ascending: each cursor's are (load checks it)
      // and equal keys are merged above. The bulk loader checks it again.
      if (!suppressed) {
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

  auto fstats_t = PersistentU32Table<FileStats>{}.transient();
  for (const auto &[id, fs] : fstats_scratch) fstats_t.set(id, fs);

  return {std::move(out).finish(), std::move(tombstones),
          std::move(range_tombstones), max_seq,
          std::move(fstats_t).persistent(), std::move(needed), skipped,
          std::move(markers)};
}

// ---------------------------------------------------------------------------
// Range-partitioned recovery — the B+ tree path.
//
// Folding W per-worker trees together pairwise would rewrite every surviving
// key once per level of the fan-in: log2(W) passes over the key set, the
// last of them serial. That fold is what stopped recovery scaling past a few
// threads when the radix tree used it.
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
  // Both are at least 1: open rejects recovery_threads = 0, and an empty
  // file list returned above.
  const auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
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
  // In both modes: the lenient skip happens inside the worker
  // (open_hint_or_skip), so what escapes it is an error no mode answers.
  for (const auto &err : worker_errors) {
    if (err) std::rethrow_exception(err);
  }
  bool skipped_files = false;
  plog.mark("build_sorted");

  // Phase 3: union the parts' metadata, and pool their separators into R
  // splitters. Both are O(W × files) or O(W × R) — nothing here touches a key.
  std::map<Key, PointTombstone> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::vector<std::uint64_t> needed;
  std::unordered_map<std::uint32_t, FileStats> fstats;
  std::vector<ChangeMarker> change_markers;
  std::uint64_t max_seq = 0;
  for (auto &part : parts) {
    max_seq = std::max(max_seq, part.max_seq);
    change_markers.insert(change_markers.end(), part.change_markers.begin(),
                          part.change_markers.end());
    for (const auto &[key, tomb] : part.tombstones) {
      auto &existing = tombstones[key];
      if (tomb.seq > existing.seq) existing = tomb;
    }
    needed.insert(needed.end(), part.needed_tombstones.begin(),
                  part.needed_tombstones.end());
    skipped_files = skipped_files || part.skipped_files;
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
    std::vector<std::uint64_t> needed;
    std::vector<std::size_t> needed_ranges; // indexes into range_tombstones
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
      const auto on_key =
          tomb != tombstones.end() &&
          recovery_key_cmp(recovery_span_of(tomb->first), key) == 0;

      // Each part offers its own newest Put for the key; the tombstones
      // come from every part. One that is newer than a Put from another
      // file is needed. Within a part, recovery_build_sorted already marked
      // what it saw.
      for (const auto i : matches) {
        const auto &e = cursors[i].entry;
        // recovery_build_sorted reduces each file to its newest entry for a
        // key, so a file offers a Put or a tombstone for it, never both: a
        // Put older than the tombstone is from another file.
        if (on_key && e.sequence() < tomb->second.seq)
          outs[r].needed.push_back(tomb->second.seq);
        for (const auto *rt : rts) {
          if (e.sequence() >= rt->seq || e.file_id() == rt->file_id) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->start)) < 0) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->end)) >= 0) continue;
          outs[r].needed_ranges.push_back(
              static_cast<std::size_t>(rt - range_tombstones.data()));
        }
      }

      auto drop = on_key && winner.sequence() < tomb->second.seq;
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

  auto fstats_t = PersistentU32Table<FileStats>{}.transient();
  for (auto &[fid, fs] : fstats) {
    const auto it = live_accum.find(fid);
    fs.live_bytes = it != live_accum.end() ? it->second : 0ULL;
    fstats_t.set(fid, fs);
  }

  for (auto &o : outs) {
    needed.insert(needed.end(), o.needed.begin(), o.needed.end());
    for (const auto idx : o.needed_ranges) range_tombstones[idx].needed = true;
  }
  needed_tombstones_ = recovery_needed_tombstones(
      std::move(needed), range_tombstones, max_seq, skipped_files);

  s.key_dir = key_dir_from_recovered(std::move(key_dir));
  s.next_seq = max_seq + 1;
  s.file_stats = FileStatsMap{std::move(fstats_t).persistent()};
  s.change_markers = sorted_markers(std::move(change_markers));
  return s;
}

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
  // Both are at least 1: open rejects recovery_threads = 0, and an empty
  // file list returned above.
  const auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
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
    std::vector<ChangeMarker> markers;
  };
  std::vector<FileRun> runs(files.size());

  // Phase 1: fences, range tombstones and sequence bounds, per file.
  auto scan_file = [&](std::size_t f) {
    auto &run = runs[f];
    const auto &rf = files[f];
    run.stats = FileStats{0, rf.total_bytes};
    run.hint = open_hint_or_skip(rf.data_file, rf.hint_path, strict);
    if (!run.hint) return;
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
      run.stats.marker_bytes += marker_size(he->entry_type);
      run.stats.change_marker_bytes += change_marker_size(he->entry_type);
      switch (he->entry_type) {
      case EntryType::BulkBegin:
      case EntryType::BulkEnd:
        continue;
      case EntryType::ChangeMarker:
        run.markers.push_back({he->sequence, he->marker_id});
        continue;
      case EntryType::RangeDel:
        // Range tombstones only ever sit in the head of a sorted file.
        if (in_data)
          throw std::runtime_error{
              "bytecask: range tombstone inside a sorted hint run"};
        run.range_tombstones.push_back(
            {Key{he->key}, Key{he->end_key}, he->sequence, rf.file_id});
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
  std::vector<ChangeMarker> change_markers;
  for (auto &run : runs) {
    max_seq = std::max(max_seq, run.max_seq);
    change_markers.insert(change_markers.end(), run.markers.begin(),
                          run.markers.end());
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
    std::vector<std::uint64_t> needed;
    // This range's own copy, so marking needs no lock; OR-ed after the merge.
    std::vector<RangeTombstone> ranges;
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
    auto &mark_ranges = outs[r].ranges;
    for (const auto *rt : rts) mark_ranges.push_back(*rt);

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

      // Every file is merged here, so this is the whole picture for the key.
      recovery_mark_needed(
          key, matches,
          [&](std::size_t i) -> const HintRecord & { return cursors[i].cur; },
          [&](std::size_t i) { return cursors[i].file_id; }, mark_ranges,
          outs[r].needed);

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
  std::vector<std::uint64_t> needed;
  for (auto &o : outs) {
    for (const auto &[fid, bytes] : o.live) live_accum[fid] += bytes;
    leaf_runs.push_back(std::move(o.run));
    needed.insert(needed.end(), o.needed.begin(), o.needed.end());
    for (const auto &rt : o.ranges)
      if (rt.needed) needed.push_back(rt.seq);
  }
  const auto skipped_files =
      std::ranges::any_of(runs, [](const FileRun &run) { return !run.hint; });
  needed_tombstones_ =
      recovery_needed_tombstones(std::move(needed), {}, max_seq, skipped_files);
  auto fstats_t = PersistentU32Table<FileStats>{}.transient();
  for (std::size_t f = 0; f < files.size(); ++f) {
    auto fs = runs[f].stats;
    const auto it = live_accum.find(files[f].file_id);
    fs.live_bytes = it != live_accum.end() ? it->second : 0ULL;
    fstats_t.set(files[f].file_id, fs);
  }
  s.key_dir = BlindBulkLoader<kBlindLeafBytes>::concat(std::move(leaf_runs));
  plog.mark("concat");
  s.next_seq = max_seq + 1;
  s.file_stats = FileStatsMap{std::move(fstats_t).persistent()};
  s.change_markers = sorted_markers(std::move(change_markers));
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
       std::uint64_t durable_sequence,
       std::size_t max_bytes)
    : state_(std::move(state)), from_sequence_(from_sequence),
      durable_sequence_(durable_sequence), max_bytes_(max_bytes) {

    // Build sorted file list — O(num_files), typically tiny.
    for (const auto [file_id, stats] : state_->file_stats.all()) {
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
  // Scans forward across entries and files until the next entry of the
  // slice is found, the slice is cut, or all files are exhausted. The cached
  // BytesView spans point into the iterator's current entry, so we must NOT
  // advance the iterator after caching — that would invalidate the view.
  // Instead, we set needs_advance_ and increment on the next call.
  //
  // The slice is whole units. A unit starts at any entry outside a batch; it
  // is in the slice when its first sequence is above from_sequence_, so a
  // batch that straddles from_sequence_ is left out whole. Durability is
  // per unit too: the durable sequence always sits on a unit boundary, so
  // the first entry above it ends the slice, and a torn batch at the end of
  // the active file, which lies above it, is never reached.
  void advance_to_next_valid() {
    has_entry_ = false;

    // Advance past the previously cached entry (deferred from last call).
    // Set only once an entry was read from entry_iter_.
    if (needs_advance_) {
      ++(*entry_iter_);
      needs_advance_ = false;
    }
    if (done_) return;

    while (true) {
      // Try next entry in the current file.
      if (entry_iter_ && !(*entry_iter_ == std::default_sentinel)) {
        const auto& [entry, entry_off] = **entry_iter_;
        if (!in_batch_) included_ = entry.sequence > from_sequence_;
        if (entry.entry_type == EntryType::BulkBegin) in_batch_ = true;
        else if (entry.entry_type == EntryType::BulkEnd) in_batch_ = false;
        if (!included_) {
          ++(*entry_iter_);
          continue;
        }
        if (entry.sequence > durable_sequence_) {
          done_ = true;
          return;
        }
        cache_entry(entry);
        needs_advance_ = true;
        sent_ += entry_size(entry.key.size(), entry.value.size());
        // Cut after the unit that passes max_bytes: never inside a batch.
        if (sent_ >= max_bytes_ && !in_batch_) done_ = true;
        return;
      }

      // Try next file. A batch never spans two files.
      if (file_idx_ < file_queue_.size()) {
        auto file_id = file_queue_[file_idx_].second;
        ++file_idx_;
        in_batch_ = false;
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
  std::size_t max_bytes_;
  std::size_t sent_{0};     // bytes of the entries yielded so far
  bool in_batch_{false};    // inside a BulkBegin..BulkEnd of the current file
  bool included_{false};    // whether the current unit is in the slice
  bool done_{false};        // the slice ended: cut, or past durable

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
                               std::uint64_t durable_sequence,
                               std::size_t max_bytes)
  : impl_(std::make_unique<Impl>(std::move(state), from_sequence,
                                 durable_sequence, max_bytes)) {}

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
auto DB::changes_since(const Snapshot& snap, std::uint64_t from_sequence,
                       std::size_t max_bytes) const -> ChangeBatch {
  if (load_state()->closed) throw DbClosed{};
  auto state = snap.state();
  ChangeHeader header{state->marker_at(from_sequence), from_sequence};
  auto begin =
      ChangeIterator{state, from_sequence, state->durable_seq, max_bytes};
  return {header, {std::move(begin), std::default_sentinel}};
}

#pragma endregion

#pragma region Ingest (follower replication)

auto DB::ingest_chunk(std::span<const DataEntryView> entries,
                      std::uint64_t file_size) const -> IngestChunk {
  auto in_batch = false;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    file_size += entry_size(entries[i].key.size(), entries[i].value.size());
    if (entries[i].entry_type == EntryType::BulkBegin) in_batch = true;
    else if (entries[i].entry_type == EntryType::BulkEnd) in_batch = false;

    if (!in_batch && file_size >= rotation_threshold_ &&
        i + 1 < entries.size()) {
      return {.end = i + 1, .needs_rotation = true, .end_size = file_size};
    }
  }
  return {.end = entries.size(), .needs_rotation = false,
          .end_size = file_size};
}

void DB::ingest(const ChangeHeader &header,
                std::span<const DataEntryView> entries) {
  if (auto s = load_state(); !s->is_ingestion_allowed()) {
    if (s->closed) throw DbClosed{};
    if (s->degraded) throw DbDegraded{s->degraded_reason};
    throw std::logic_error{"ingest rejected: engine is not in follower mode"};
  }
  if (write_fault_.load(std::memory_order_acquire))
    throw DbDegraded{kWriteFaultReason};

  // An atomic batch is written to one file like a plan, so it is held to the
  // same byte limit; with it every entry's offset fits the packed field.
  std::uint64_t batch_bytes = 0;
  auto in_batch = false;
  auto previous = header.from_sequence;
  for (const auto &e : entries) {
    if (e.sequence <= previous) {
      throw std::invalid_argument{std::format(
          "ingest: sequence {} after {}: a slice is strictly increasing "
          "above its header's from_sequence",
          e.sequence, previous)};
    }
    previous = e.sequence;
    // A type byte outside the enum would be appended as given and read back
    // as damage, cutting the follower's newest file at the next open.
    if (!is_known_entry_type(e.entry_type)) {
      throw std::invalid_argument{std::format(
          "ingest: unknown entry type {}",
          static_cast<unsigned>(e.entry_type))};
    }
    if (e.entry_type == EntryType::ChangeMarker) {
      // A marker is a unit of its own, and its value is its id.
      if (in_batch || !e.key.empty() ||
          e.value.size() != kChangeMarkerIdBytes) {
        throw std::invalid_argument{
            "ingest: a change marker inside an atomic batch, or malformed"};
      }
    }
    check_key_size(e.key.size(), size_limits_.max_key_bytes);
    if (e.entry_type == EntryType::Put) {
      check_value_size(e.value.size(), size_limits_.max_value_bytes);
    }
    if (e.sequence > KeyDirEntry::kMaxSequence) {
      throw std::invalid_argument{std::format(
          "ingest: sequence {} exceeds the limit of {}", e.sequence,
          KeyDirEntry::kMaxSequence)};
    }
    if (e.entry_type == EntryType::BulkBegin) {
      in_batch = true;
      batch_bytes = 0;
    }
    if (in_batch) {
      batch_bytes += entry_size(e.key.size(), e.value.size());
      if (batch_bytes > max_batch_bytes()) {
        throw std::invalid_argument{std::format(
            "ingest: atomic batch exceeds the limit of {} bytes",
            max_batch_bytes())};
      }
    }
    if (e.entry_type == EntryType::BulkEnd) in_batch = false;
  }
  // A slice published as given would expose half a batch, and a rotation
  // after it would seal the BulkBegin into a file with no BulkEnd: recovery
  // then drops entries this follower acknowledged and synced (#188).
  if (in_batch) {
    throw std::invalid_argument{
        "ingest: the slice ends inside an atomic batch; cut slices after a "
        "BulkEnd or a standalone entry"};
  }

  WriteBarrier barrier{*this};

  auto current = load_state_for_write();
  if (!current->is_ingestion_allowed()) {
    if (current->closed) throw DbClosed{};
    if (refused(*current)) throw DbDegraded{degraded_reason()};
    throw std::logic_error{"ingest rejected: engine is not in follower mode"};
  }

  // The slice must continue this node's own history (#397). With P the
  // position and X the lower of P and the slice's last sequence: a slice
  // that starts past P has a hole before it, and the source's marker at X
  // must be this node's, or the two histories diverged at a promotion. X
  // stops at the slice's last sequence because the slice says nothing past
  // it, and at P because the slice is skipped as duplicates up to there.
  // An empty slice compares at from_sequence, which is how a node ahead of
  // its source on another history is found out.
  const auto position = current->durable_seq;
  if (header.from_sequence > position) {
    throw std::invalid_argument{std::format(
        "ingest: the slice starts at {}, past this node's durable sequence "
        "{}: a gap",
        header.from_sequence, position)};
  }
  const auto last =
      entries.empty() ? header.from_sequence : entries.back().sequence;
  const auto at = std::min(position, last);
  auto source = header.marker;
  for (const auto &e : entries) {
    if (e.sequence > at) break;
    if (e.entry_type == EntryType::ChangeMarker)
      source = {e.sequence, decode_marker_id(e.value)};
  }
  if (const auto mine = current->marker_at(at); source != mine) {
    throw DbChangeMarkerMismatch{std::format(
        "ingest: at sequence {} the source's history is marker {{{}, {:#x}}} "
        "and this node's is {{{}, {:#x}}}: they diverged at a promotion; "
        "re-bootstrap this node",
        at, source.since_sequence, source.id, mine.since_sequence, mine.id)};
  }

  auto t = current->transient();

  // Filter: skip already-ingested entries (idempotency).
  auto remaining = entries;
  while (!remaining.empty() &&
         remaining.front().sequence <= current->durable_seq) {
    remaining = remaining.subspan(1);
  }
  if (remaining.empty()) return;

  // Every file the slice will need is counted before anything is written:
  // a rotation that finds no id would fail after its file was sealed.
  {
    std::uint32_t rotations = 0;
    auto size = static_cast<std::uint64_t>(t.active_file().size());
    for (auto rest = remaining; !rest.empty();) {
      const auto chunk = ingest_chunk(rest, size);
      if (chunk.needs_rotation) {
        ++rotations;
        size = 0;
      } else {
        size = chunk.end_size;
      }
      rest = rest.subspan(chunk.end);
    }
    if (size >= rotation_threshold_) ++rotations;
    if (rotations > t.file_ids_left()) throw file_ids_exhausted();
  }

  // Chunk-and-rotate loop: write entries in chunks, rotating between chunks
  // at safe boundaries (never inside BulkBegin..BulkEnd).
  // Once a chunk is in the file, a throw that no handler below degraded
  // for — an allocation, typically — leaves bytes nothing published, and
  // writes are refused until resume() reconciles them.
  auto appended = false;
  try {
    while (!remaining.empty()) {
      auto &file = t.active_file();

      const auto next =
          ingest_chunk(remaining, static_cast<std::uint64_t>(file.size()));
      const auto chunk_end = next.end;
      const auto needs_rotation = next.needs_rotation;

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
      appended = true;

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
        PreparedRotation rotation;
        try {
          rotation = prepare_rotation(t);
        } catch (...) {
          t.apply_degrade(
              "ingest post-rotation file creation failed: call resume().");
          store_state(current, std::move(t).persistent());
          throw;
        }
        finish_rotation(t, std::move(rotation));
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
      PreparedRotation rotation;
      try {
        rotation = prepare_rotation(t);
      } catch (...) {
        t.apply_degrade(
            "ingest post-rotation file creation failed: call resume().");
        store_state(current, std::move(t).persistent());
        throw;
      }
      finish_rotation(t, std::move(rotation));
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
  } catch (...) {
    if (appended) refuse_writes();
    throw;
  }
}

#pragma endregion


} // namespace bytecask
