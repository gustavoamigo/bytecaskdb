// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — C++23 module interface: the public API and every engine
// declaration. Member definitions live in the implementation units
// (bytecaskdb/db_*.cpp, engine_state.cpp).

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
export import :change_iterator;
import bytecask.batch_iterator;
import bytecask.blind_btree;
import bytecask.change_index;
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

#ifdef BYTECASK_TESTING
// The most bytes any of the calling thread's record, fill and append
// scratch keeps between calls (#384).
export [[nodiscard]] inline auto test_thread_scratch_bytes() noexcept
    -> std::size_t {
#ifdef BYTECASK_KEYDIR_BLIND
  const auto reads = key_read_buffer().capacity();
#else
  const auto reads = value_read_buffer().capacity();
#endif
  return std::max({reads, BufferPool::thread_scratch_bytes(),
                   append_scratch_bytes()});
}
#endif

// Sensible defaults — keys live in RAM (the key directory), values go to disk.
export inline constexpr std::uint32_t kDefaultMaxKeyBytes = 4096;
export inline constexpr std::uint32_t kDefaultMaxValueBytes =
    4U * 1024 * 1024; // 4 MiB
export inline constexpr std::uint32_t kDefaultMaxHintBacklog = 4;
inline constexpr std::size_t kDefaultThreadScratchBytes = std::size_t{1} << 20;

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
  // Most bytes a thread's record buffers keep after a call on this DB, and
  // so the largest record a call serves without allocating. A thread shares
  // its buffers between the DBs it touches: after a call on a DB it keeps at
  // most that DB's value (#384). Below 256 KiB, open throws
  // std::invalid_argument.
  std::size_t thread_scratch_bytes{kDefaultThreadScratchBytes};
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
    return {nullptr, &files_, &pending_, true, scratch_bytes_};
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
                           change_markers,
                       std::size_t scratch_bytes);

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
  std::size_t scratch_bytes_;
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

  // Blocks until the published state covers sequence, or the engine
  // degrades or closes.
  void wait_published(std::uint64_t sequence) const;
  // Returns once the outcome of a slot that appended nothing rests only on
  // published writes. Throws DbDegraded when a write it rests on will never
  // be published.
  void settle_unappended(const EngineSlot &slot) const;

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
  // Runs after the scan and before the hint is written: rules on whether the
  // file may end at `end`, the end of its last committed record, and cuts it
  // there. Throws to refuse it, or when the cut fails. It may close the file
  // to cut it: flush_hints_for touches the file only for its name and its
  // scan, both before the call, and not afterwards.
  using TailCut = std::function<void(Offset end)>;
  // Writes hint file via temp-then-rename. Batch-aware; idempotent if .hint
  // exists. Without a cut, an entry that fails its CRC throws. With one, the
  // scan stops there as it does at a zeroed header, and the cut rules on what
  // lies past it; a throw from it leaves no hint behind.
  static void flush_hints_for(const std::shared_ptr<DataFile> &file,
                              const std::filesystem::path &dir,
                              const TailCut &cut = {});
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
  // allows, and wakes every waiter. Allocates nothing to refuse. Publishes
  // by compare-and-swap, so it never replaces a state newer than the one it
  // degraded, and needs no flush role. For a failure after an append that no
  // I/O handler degraded for, and the last step of every degrade.
  void refuse_writes() noexcept;
  // For a failure after bytes may have reached the file. Runs publish,
  // which builds and stores the handler's degraded state, then refuses
  // writes by the flag (refuse_writes) whether or not it could: building the
  // state allocates. Never throws: the handler throws its own error after
  // it, never the allocation's. The flag goes up after the publish, so no
  // reader sees the generic reason before the handler's, and a quiesce()
  // inside publish still lands the flush in flight. publish runs under the
  // caller's flush role, or takes it.
  template <typename Publish>
  void degrade(Publish &&publish) noexcept {
    try {
#ifdef BYTECASK_TESTING
      if (test_before_degrade_publish_) test_before_degrade_publish_();
#endif
      publish();
    } catch (...) {
      // No memory to say why: the flag refuses, and refuse_writes publishes
      // the generic reason if it can.
    }
    refuse_writes();
  }
  // The two shapes of degrade: the published state degraded, or the
  // transient with what it applied. reason is a string literal, or a
  // callable that builds it, so that its allocation is inside degrade too.
  template <typename Reason>
  void degrade_from(const std::shared_ptr<const EngineState> &current,
                    const Reason &reason) noexcept {
    degrade([&] { store_state(current->degraded_copy(make_reason(reason))); });
  }
  template <typename Reason>
  void degrade_transient(TransientEngineState &t,
                         const std::shared_ptr<const EngineState> &current,
                         const Reason &reason) noexcept {
    degrade([&] {
      t.apply_degrade(make_reason(reason));
      store_state(current, std::move(t).persistent());
    });
  }
  template <typename Reason>
  static auto make_reason(const Reason &reason) -> std::string {
    if constexpr (std::is_invocable_v<const Reason &>) {
      return std::string{reason()};
    } else {
      return std::string{reason};
    }
  }
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
  // Syncs and seals ingest's active file and opens the next, between
  // chunks and after the last. A failure degrades from current and throws.
  void ingest_rotate(TransientEngineState &t,
                     const std::shared_ptr<const EngineState> &current);

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
  // refuse_writes' publication, which runs without the flush role: the
  // current state degraded, swapped in only if no publication landed since
  // it was loaded, and otherwise retried on the newer one, so no state is
  // replaced by an older one. A state degraded already keeps its own reason.
  // Throws std::bad_alloc.
  void publish_degraded_if_healthy(std::string_view reason) {
    auto current = std::atomic_load(&state_);
    while (!current->degraded) {
      auto s = current->degraded_copy(std::string{reason});
#ifdef BYTECASK_TESTING
      if (test_before_refusal_swap_) test_before_refusal_swap_();
#endif
      publishing_.fetch_add(1, std::memory_order_acq_rel);
      const bool swapped =
          std::atomic_compare_exchange_strong(&state_, &current, s);
      if (swapped)
        state_gen_.store(next_state_gen(), std::memory_order_release);
      publishing_.fetch_sub(1, std::memory_order_release);
      if (swapped) {
        counters_.degraded_transitions.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // current now holds the state that landed in between.
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
  std::size_t scratch_bytes_;
  mutable Counters counters_;
  // Bookmarks for changes_since to enter a file near from_sequence (#443).
  // Shared with every ChangeIterator, which hands it the bookmarks it lays and
  // can outlive the DB. Cleared by resume(); a file vacuum removes is
  // forgotten.
  std::shared_ptr<ChangeIndex> change_index_{std::make_shared<ChangeIndex>()};
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
  auto& test_change_index() { return *change_index_; }
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
  // Called by degrade before it builds the handler's degraded state. A test
  // throws from it to leave the I/O handler with the flag alone.
  std::function<void()> test_before_degrade_publish_;
  // Called by refuse_writes once it has built its degraded state, before it
  // swaps it in. A test publishes there (test_publish_unchecked) as a flush
  // would, which the swap must not replace.
  std::function<void()> test_before_refusal_swap_;
  void test_publish_unchecked(std::shared_ptr<EngineState> s) {
    store_state(std::move(s));
  }
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
  // A slot that appended nothing: the latest head entry its outcome may
  // rest on — on a conflict, the entry the plan lost to; when its guards
  // held, the head's latest. 0 when nothing was decided. apply_batch
  // returns only once it is published (settle_unappended).
  std::uint64_t rests_on{0};
  // A sync slot that appended nothing: the sequence every earlier write
  // reaches, which commit_wait makes durable before the slot returns. Its
  // result keeps sequence 0 — the write itself wrote nothing.
  std::uint64_t sync_through{0};
};


DbDegraded::~DbDegraded() = default;
DbFollowerMode::~DbFollowerMode() = default;
DbChangeMarkerMismatch::~DbChangeMarkerMismatch() = default;
DbClosed::~DbClosed() = default;

// Why writes are refused while DB::write_fault_ is set.
constexpr const char *kWriteFaultReason =
    "a write reached the data file but could not be published: call "
    "resume() to recover.";

// Ids are given out from the directory's contents at each open and never
// reused within a process, so the ceiling is reached by files created since
// open, and a reopen renumbers.
auto file_ids_exhausted() -> std::runtime_error {
  return std::runtime_error{std::format(
      "file id space exhausted: this process has used all {} file ids. "
      "Reopen the database to renumber its files.",
      KeyDirEntry::kMaxFileId + 1)};
}

// Lexicographic order over raw keys, matching the key directory's order.
// Used both by sorted hint generation and by the ranged recovery merge.
auto recovery_key_cmp(std::span<const std::byte> a,
                      std::span<const std::byte> b) noexcept -> int {
  const auto n = std::min(a.size(), b.size());
  if (n != 0) {
    const auto c = std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

// Generates a unique data file stem; used by rotation and vacuum.
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

} // namespace bytecask
