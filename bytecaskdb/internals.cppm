// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — internal engine types shared across implementation units

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

export module bytecask:internals;

import bytecask.data_entry;
import bytecask.data_file;
import bytecask.radix_tree;
import bytecask.types;
import bytecask.u32_map;
import bytecask.util;

namespace bytecask {

// Forward-declared in the primary interface; defined here so the DB
// class definition (in bytecask.cppm) can use them as member types.

// ---------------------------------------------------------------------------
// FileStats — per-file live/total byte counters for fragmentation tracking.
// Updated under write_mu_ on every write; rebuilt during recovery.
// Exported only in BYTECASK_TESTING builds so the public API stays minimal.
// ---------------------------------------------------------------------------
#ifdef BYTECASK_TESTING
export struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
};
#else
struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
};
#endif

// File registry: a COW map from file_id to shared DataFile.
// PersistentU32Map provides O(1) snapshot sharing; rotation and vacuum
// fork a transient, mutate it, and freeze — no O(N) map clone.

// ---------------------------------------------------------------------------
// KeyDirEntry — one slot in the in-memory key directory.
//
// Bit-packed into two 64-bit words to reduce per-node radix tree overhead.
// Access is through accessor methods so the internal layout can be changed
// without touching call sites.
//
// Layout (variant 3b — split file_id):
//   word0 [63:16] sequence     (48 bits, max 281 T)
//         [15: 0] file_id_low  (16 bits)
//   word1 [63:32] file_offset  (32 bits, max 4 GiB)
//         [31: 4] value_size   (28 bits, max 256 MiB)
//         [ 3: 0] file_id_high ( 4 bits)
//
// file_id = (file_id_high << 16) | file_id_low — effective 20 bits (max 1 048 575).
//
// file_id is a monotonic integer handle, assigned by DB, that indexes
// into the engine's file registry. file_offset is the byte offset where
// the full DataEntry begins.
// ---------------------------------------------------------------------------
export struct KeyDirEntry {
  std::uint64_t word0_{};
  std::uint64_t word1_{};

  // Field limits — checked at construction time.
  static constexpr std::uint64_t kMaxSequence  = (std::uint64_t{1} << 48) - 1;
  static constexpr std::uint32_t kMaxFileId    = (std::uint32_t{1} << 20) - 1;
  static constexpr std::uint64_t kMaxFileOffset = (std::uint64_t{1} << 32) - 1;
  static constexpr std::uint32_t kMaxValueSize = (std::uint32_t{1} << 28) - 1;

  // Limit-checking helpers — usable both from make() and from call sites
  // that validate individual fields before construction (e.g. file rotation).
  static void check_sequence(std::uint64_t v) {
    if (v > kMaxSequence)
      throw std::runtime_error("sequence " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxSequence));
  }
  static void check_file_id(std::uint32_t v) {
    if (v > kMaxFileId)
      throw std::runtime_error("file_id " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxFileId));
  }
  static void check_file_offset(std::uint64_t v) {
    if (v > kMaxFileOffset)
      throw std::runtime_error("file_offset " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxFileOffset));
  }
  static void check_value_size(std::uint32_t v) {
    if (v > kMaxValueSize)
      throw std::runtime_error("value_size " + std::to_string(v) +
                               " exceeds packed limit " + std::to_string(kMaxValueSize));
  }

  static auto make(std::uint64_t sequence, std::uint64_t file_offset,
                   std::uint32_t file_id, std::uint32_t value_size)
      -> KeyDirEntry {
    check_sequence(sequence);
    check_file_id(file_id);
    check_file_offset(file_offset);
    check_value_size(value_size);
    KeyDirEntry e;
    e.word0_ = (sequence << 16) | (file_id & 0xFFFFu);
    e.word1_ = (file_offset << 32) |
               (static_cast<std::uint64_t>(value_size) << 4) |
               (file_id >> 16);
    return e;
  }

  [[nodiscard]] auto sequence()    const -> std::uint64_t { return word0_ >> 16; }
  [[nodiscard]] auto file_id()     const -> std::uint32_t {
    auto low  = static_cast<std::uint32_t>(word0_ & 0xFFFFu);
    auto high = static_cast<std::uint32_t>(word1_ & 0xFu);
    return (high << 16) | low;
  }
  [[nodiscard]] auto file_offset() const -> std::uint64_t { return word1_ >> 32; }
  [[nodiscard]] auto value_size()  const -> std::uint32_t { return static_cast<std::uint32_t>((word1_ >> 4) & 0xFFF'FFFFu); }
};
static_assert(sizeof(KeyDirEntry) == 16);

// Canonical key-ownership comparator. Returns true if `a` is strictly newer
// than `b`. Sequence numbers are unique per logical write, so equal sequences
// must point to the same physical record. If they don't, the database is
// corrupt and recovery is aborted.
export inline auto kde_newer(const KeyDirEntry &a, const KeyDirEntry &b) -> bool {
  if (a.sequence() != b.sequence()) return a.sequence() > b.sequence();
  // Same sequence — must be the same physical record.
  if (a.file_id() != b.file_id() || a.file_offset() != b.file_offset()) {
    throw std::runtime_error(
        "bytecask: corrupt database — two entries share the same sequence "
        "number but differ in physical location");
  }
  return false; // identical record
}

// Returns the on-disk size of a data file entry given key and value sizes.
export inline constexpr auto entry_size(std::size_t key_size,
                                        std::size_t value_size)
    -> std::uint64_t {
  return kHeaderSize + key_size + value_size + kCrcSize;
}

// Forward declaration — defined in bytecask.cppm (primary interface).
export class TransientEngineState;

// ---------------------------------------------------------------------------
// EngineState — immutable snapshot of all mutable engine state.
//
// Each write produces a new EngineState via a pure transition method.
// The old state stays alive as long as any reader holds a shared_ptr.
// ---------------------------------------------------------------------------
export struct EngineState {
  PersistentRadixTree<KeyDirEntry> key_dir;
  PersistentU32Map<std::shared_ptr<DataFile>> files;
  PersistentU32Map<FileStats> file_stats;
  std::uint32_t active_file_id{};
  std::uint32_t next_file_id{};
  std::uint64_t next_seq{1};
  std::uint64_t durable_seq{0};
  Mode mode{Mode::Leader};
  bool degraded{false};
  std::string degraded_reason;

  [[nodiscard]] auto is_write_allowed() const noexcept -> bool {
    return mode == Mode::Leader && !degraded;
  }

  [[nodiscard]] auto is_ingestion_allowed() const noexcept -> bool {
    return mode == Mode::Follower && !degraded;
  }

  [[nodiscard]] auto active_file() -> DataFile & {
    return **files.get(active_file_id);
  }

  [[nodiscard]] auto active_file() const -> const DataFile & {
    return **files.get(active_file_id);
  }

  // Creates a mutable working copy for the write path.
  // Defined in bytecask.cpp (needs TransientEngineState's full definition).
  [[nodiscard]] auto transient() const -> TransientEngineState;
};

// ---------------------------------------------------------------------------
// VacuumMapping — per-live-entry mapping produced during vacuum I/O phase.
// The commit phase uses these to remap key_dir entries.
// ---------------------------------------------------------------------------
export struct VacuumMapping {
  std::vector<std::byte> key;
  std::uint64_t new_offset;
  std::uint64_t sequence;
  std::uint32_t value_size;
};

// ---------------------------------------------------------------------------
// ResumeEntry — one valid committed entry collected during resume()'s scan.
// Passed to TransientEngineState::apply_resume for key_dir replay.
// ---------------------------------------------------------------------------
export struct ResumeEntry {
  std::uint64_t sequence;
  EntryType entry_type;
  std::uint64_t file_offset;
  std::uint32_t value_size;
  std::vector<std::byte> key;
};

export struct VacuumScanResult {
  std::vector<VacuumMapping> mappings;
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
  std::uint64_t min_sequence{0};
  std::uint64_t max_sequence{0};
};

// RecoveredFile and RecoveryResult are private to bytecask.cpp.

// ---------------------------------------------------------------------------
// Key — owning byte sequence for iterator value_type and recovery tombstone map.
// Needs operator<=> for use as map key in recovery.
// ---------------------------------------------------------------------------
export class Key {
public:
  Key() = default;
  explicit Key(std::span<const std::byte> v) : data_{v.begin(), v.end()} {}

  [[nodiscard]] auto begin() const { return data_.begin(); }
  [[nodiscard]] auto end() const { return data_.end(); }
  [[nodiscard]] auto size() const noexcept { return data_.size(); }

  auto operator<=>(const Key &other) const -> std::strong_ordering {
    return std::lexicographical_compare_three_way(
        data_.begin(), data_.end(), other.data_.begin(), other.data_.end(),
        [](std::byte a, std::byte b) -> std::strong_ordering {
          return std::to_integer<unsigned char>(a) <=>
                 std::to_integer<unsigned char>(b);
        });
  }

  auto operator==(const Key &other) const -> bool {
    return data_ == other.data_;
  }

private:
  std::vector<std::byte> data_;
};

export struct RecoveredFile {
  std::uint32_t file_id;
  std::shared_ptr<DataFile> data_file;
  std::filesystem::path hint_path;
  std::uint64_t total_bytes{0};
};

export struct RangeTombstone {
  Key start;
  Key end; // exclusive — [start, end)
  std::uint64_t seq;
};

export struct RecoveryResult {
  PersistentRadixTree<KeyDirEntry> key_dir;
  std::map<Key, std::uint64_t> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::uint64_t max_seq{0};
  PersistentU32Map<FileStats> file_stats;
};

} // namespace bytecask
