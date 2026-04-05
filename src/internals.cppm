module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <vector>

export module bytecask:internals;

import bytecask.data_entry;
import bytecask.data_file;
import bytecask.radix_tree;
import bytecask.types;
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
};
#else
struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
};
#endif

// File registry: a copy-on-write map from file_id to shared DataFile.
// The outer shared_ptr is copied into iterators at construction — O(1),
// independent lifetime from DB. Rotation clones the inner map,
// inserts the new file, then atomically replaces the outer shared_ptr.
export using FileRegistry =
    std::shared_ptr<std::map<std::uint32_t, std::shared_ptr<DataFile>>>;

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
  std::uint32_t file_id{};
  std::uint64_t file_offset{};
  std::uint32_t value_size{};
};

// Returns the on-disk size of a data file entry given key and value sizes.
export inline constexpr auto entry_size(std::size_t key_size,
                                        std::size_t value_size)
    -> std::uint64_t {
  return kHeaderSize + key_size + value_size + kCrcSize;
}

// ---------------------------------------------------------------------------
// EngineState — immutable snapshot of all mutable engine state.
//
// Each write produces a new EngineState via a pure transition method.
// The old state stays alive as long as any reader holds a shared_ptr.
// ---------------------------------------------------------------------------
export struct EngineState {
  PersistentRadixTree<KeyDirEntry> key_dir;
  FileRegistry files;
  std::uint32_t active_file_id{};
  std::uint32_t next_file_id{};
  std::uint64_t next_lsn{1};

  [[nodiscard]] auto active_file() -> DataFile & {
    return *files->at(active_file_id);
  }

  [[nodiscard]] auto active_file() const -> const DataFile & {
    return *files->at(active_file_id);
  }

  // Pure transition: insert or overwrite a key after the I/O has been done.
  [[nodiscard]] auto apply_put(std::span<const std::byte> key,
                               std::uint64_t offset,
                               std::uint32_t value_size) const -> EngineState {
    auto s = *this;
    s.key_dir = s.key_dir.set(
        key, KeyDirEntry{s.next_lsn, s.active_file_id, offset, value_size});
    ++s.next_lsn;
    return s;
  }

  // Pure transition: remove a key.
  [[nodiscard]] auto apply_del(std::span<const std::byte> key) const
      -> EngineState {
    auto s = *this;
    ++s.next_lsn;
    s.key_dir = s.key_dir.erase(key);
    return s;
  }

  // Pure transition: seal the active file and open a new one.
  [[nodiscard]] auto apply_rotation(const std::filesystem::path &dir) const
      -> EngineState;  // defined in bytecask.cpp (needs make_data_file_stem)
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

export struct VacuumScanResult {
  std::vector<VacuumMapping> mappings;
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
};

// ---------------------------------------------------------------------------
// StaleFile — data file removed from the registry by vacuum but potentially
// still referenced by in-flight readers. Purged when use_count drops to 1.
// Protected by vacuum_mu_.
// ---------------------------------------------------------------------------
export struct StaleFile {
  std::shared_ptr<DataFile> data_file;
  std::filesystem::path hint_path;
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

export struct RecoveryResult {
  PersistentRadixTree<KeyDirEntry> key_dir;
  std::map<Key, std::uint64_t> tombstones;
  std::uint64_t max_lsn{0};
  std::map<std::uint32_t, FileStats> file_stats;
};

} // namespace bytecask
