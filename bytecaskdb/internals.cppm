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
// file_id is a monotonic integer handle, assigned by DB, that indexes
// into the engine's file registry. Using a compact integer avoids pointer-
// stability hazards (pointers into a growing container would be invalidated)
// and keeps each entry to a fixed small size.
// file_offset is the byte offset where the full DataEntry begins.
// ---------------------------------------------------------------------------
export struct KeyDirEntry {
  std::uint64_t sequence{};
  std::uint64_t file_offset{};
  std::uint32_t file_id{};
  std::uint32_t value_size{};
};
static_assert(sizeof(KeyDirEntry) == 24);

// Canonical key-ownership comparator. Returns true if `a` is strictly newer
// than `b`. Sequence numbers are unique per logical write, so equal sequences
// must point to the same physical record. If they don't, the database is
// corrupt and recovery is aborted.
export inline auto kde_newer(const KeyDirEntry &a, const KeyDirEntry &b) -> bool {
  if (a.sequence != b.sequence) return a.sequence > b.sequence;
  // Same sequence — must be the same physical record.
  if (a.file_id != b.file_id || a.file_offset != b.file_offset) {
    throw std::runtime_error(
        "bytecask: corrupt database — two entries share the same sequence "
        "number but differ in physical location");
  }
  return false; // identical record
}

// Canonical key-ownership comparator. Returns true if `a` is strictly newer
// than `b`. Sequence numbers are unique per logical write, so equal sequences
// must point to the same physical record. If they don't, the database is
// corrupt and recovery is aborted.
export inline auto kde_newer(const KeyDirEntry &a, const KeyDirEntry &b) -> bool {
  if (a.sequence != b.sequence) return a.sequence > b.sequence;
  // Same sequence — must be the same physical record.
  if (a.file_id != b.file_id || a.file_offset != b.file_offset) {
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
