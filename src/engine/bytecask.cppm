module;
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <time.h>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.engine;

import bytecask.crc32;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_file;
import bytecask.persistent_ordered_map;
import bytecask.types;

namespace bytecask {

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

// Owned byte buffer — for return values and batch storage.
export using Bytes = std::vector<std::byte>;

// Owned key — semantically distinct from a generic byte buffer.
// Keys have an upper bound of 65 535 bytes (u16 key_size in the data file).
export using Key = Bytes;

// Non-owning view — used for all input parameters to avoid copies.
export using BytesView = std::span<const std::byte>;

// ---------------------------------------------------------------------------
// KeyDirEntry — one slot in the in-memory key directory.
//
// file_id is a monotonic integer handle, assigned by Bytecask, that indexes
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
// Consumed by Bytecask::apply_batch().
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
  friend class Bytecask;
};

// File registry: a copy-on-write map from file_id to shared DataFile.
// The outer shared_ptr is copied into iterators at construction — O(1),
// independent lifetime from Bytecask. Rotation clones the inner map,
// inserts the new file, then atomically replaces the outer shared_ptr.
export using FileRegistry =
    std::shared_ptr<std::map<std::uint32_t, std::shared_ptr<DataFile>>>;

// ---------------------------------------------------------------------------
// KeyIterator — walks the key directory in ascending key order.
//
// In-memory only: no data file I/O. Satisfies std::input_iterator.
// ---------------------------------------------------------------------------
export class KeyIterator {
public:
  using value_type = Bytes;
  using difference_type = std::ptrdiff_t;

  KeyIterator() = default;

  KeyIterator(PersistentOrderedMap<Key, KeyDirEntry>::const_iterator cur,
              PersistentOrderedMap<Key, KeyDirEntry>::const_iterator end)
      : cur_{cur}, end_{end} {}

  auto operator*() const -> const value_type & { return cur_->key; }

  auto operator++() -> KeyIterator & {
    ++cur_;
    return *this;
  }

  void operator++(int) { ++cur_; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == end_;
  }

private:
  PersistentOrderedMap<Key, KeyDirEntry>::const_iterator cur_{};
  PersistentOrderedMap<Key, KeyDirEntry>::const_iterator end_{};
};

// ---------------------------------------------------------------------------
// EntryIterator — walks key directory in ascending order, reading values
// lazily from disk on each dereference.
//
// Satisfies std::input_iterator. Throws std::system_error on I/O failure.
// files_ is a snapshot of the engine's registry at construction time.
// O(1) structural sharing via immer::map; independent lifetime from Bytecask.
// ---------------------------------------------------------------------------
export class EntryIterator {
public:
  using value_type = std::pair<Bytes, Bytes>;
  using difference_type = std::ptrdiff_t;

  EntryIterator() = default;

  EntryIterator(PersistentOrderedMap<Key, KeyDirEntry>::const_iterator cur,
                PersistentOrderedMap<Key, KeyDirEntry>::const_iterator end,
                FileRegistry files)
      : cur_{cur}, end_{end}, files_{std::move(files)} {}

  // Reads the value from disk on demand (lazy). Caches the result until ++.
  auto operator*() const -> const value_type & {
    if (!cached_) {
      auto entry =
          files_->at(cur_->value.file_id)->read(cur_->value.file_offset);
      cached_.emplace(Bytes{cur_->key}, std::move(entry.value));
    }
    return *cached_;
  }

  auto operator++() -> EntryIterator & {
    ++cur_;
    cached_.reset();
    return *this;
  }

  void operator++(int) {
    ++cur_;
    cached_.reset();
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == end_;
  }

private:
  PersistentOrderedMap<Key, KeyDirEntry>::const_iterator cur_{};
  PersistentOrderedMap<Key, KeyDirEntry>::const_iterator end_{};
  FileRegistry files_;
  mutable std::optional<value_type> cached_;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Generates a data file stem using a microsecond-precision UTC timestamp.
// Format: "data_{YYYYMMDDHHmmssUUUUUU}"  (D11)
auto make_data_file_stem() -> std::string {
  const auto now = std::chrono::system_clock::now();
  const auto us_total = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
  const auto subsec_us = us_total % 1'000'000;

  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  ::gmtime_r(&tt, &tm_buf);

  return std::format("data_{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}{:06d}",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, subsec_us);
}

} // namespace

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
};

// Reserved for future read-path controls (e.g. verify_checksums, snapshots).
export struct ReadOptions {};

// ---------------------------------------------------------------------------
// Bytecask — SWMR key-value store
//
// Intent: Public engine API. open() always creates a fresh active data file.
// Rotation: when the active file exceeds max_file_bytes, it is sealed and a
// new active file is created. Sealed files remain open for reads.
// Hint files are written deferred, at engine close or via flush_hints(), to
// keep write-path latency flat and bounded (predictable latency over peak
// throughput).
//
// Thread safety: NOT thread-safe. Follow the SWMR contract at the call site.
// ---------------------------------------------------------------------------
export class Bytecask {
public:
  // Opens or creates a database rooted at dir.
  // Always creates a new active data file (BC-019 adds recovery).
  // max_file_bytes controls the rotation threshold (default 64 MiB).
  // Throws std::system_error if the directory cannot be prepared.
  [[nodiscard]] static auto
  open(std::filesystem::path dir,
       std::uint64_t max_file_bytes = kDefaultRotationThreshold) -> Bytecask {
    return Bytecask{std::move(dir), max_file_bytes};
  }

  Bytecask(const Bytecask &) = delete;
  Bytecask &operator=(const Bytecask &) = delete;
  Bytecask(Bytecask &&) noexcept = default;
  Bytecask &operator=(Bytecask &&) noexcept = default;

  // Flush hints and sync the active file before destruction.
  ~Bytecask() {
    if (files_ && !files_->empty()) {
      try {
        files_->at(active_file_id_)->sync();
        flush_hints();
      } catch (...) {
        // Best-effort: swallow errors in destructors.
      }
    }
  }

  // ── Primary operations ─────────────────────────────────────────────────

  // Returns the value for key, or std::nullopt if the key does not exist.
  // Routes the read to the correct data file via KeyDirEntry::file_id.
  // Throws std::system_error on I/O failure or std::runtime_error on CRC
  // mismatch.
  [[nodiscard]] auto get(const ReadOptions & /*opts*/, BytesView key) const
      -> std::optional<Bytes> {
    const auto kv = key_dir_.get(to_key(key));
    if (!kv) {
      return std::nullopt;
    }
    const auto entry = files_->at(kv->file_id)->read(kv->file_offset);
    return entry.value;
  }

  // Writes key → value. Overwrites any existing value.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure.
  void put(const WriteOptions &opts, BytesView key, BytesView value) {
    auto k = to_key(key);
    const auto offset =
        active_file().append(next_lsn_, EntryType::Put, key, value);

    key_dir_ = key_dir_.set(std::move(k),
                            KeyDirEntry{next_lsn_, active_file_id_, offset,
                                        narrow<std::uint32_t>(value.size())});
    ++next_lsn_;
    if (opts.sync) {
      active_file().sync();
    }
    maybe_rotate();
  }

  // Writes a tombstone for key.
  // Returns true if the key existed and was removed, false if it was absent.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure.
  [[nodiscard]] auto del(const WriteOptions &opts, BytesView key) -> bool {
    const auto k = to_key(key);

    if (!key_dir_.contains(k)) {
      return false;
    }
    std::ignore = active_file().append(next_lsn_, EntryType::Delete, key, {});
    ++next_lsn_;

    key_dir_ = key_dir_.erase(k);
    if (opts.sync) {
      active_file().sync();
    }
    maybe_rotate();
    return true;
  }

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(BytesView key) const -> bool {
    return key_dir_.contains(to_key(key));
  }

  // ── Batch ──────────────────────────────────────────────────────────────

  // Atomically applies all operations in batch, wrapped in BulkBegin/BulkEnd.
  // batch is consumed (move-only). No-op if batch.empty().
  // opts.sync controls whether a single fdatasync is issued at the end.
  // Rotates the active file after the sync if the threshold is reached.
  // Throws std::system_error on I/O failure.
  void apply_batch(const WriteOptions &opts, Batch batch) {
    if (batch.empty()) {
      return;
    }

    std::ignore =
        active_file().append(next_lsn_++, EntryType::BulkBegin, {}, {});

    auto t = key_dir_.transient();
    for (auto &op : batch.operations_) {
      std::visit(
          [&](auto &o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, BatchInsert>) {
              const auto offset = active_file().append(
                  next_lsn_, EntryType::Put, std::span<const std::byte>{o.key},
                  std::span<const std::byte>{o.value});
              t.set(Key{o.key},
                    KeyDirEntry{next_lsn_, active_file_id_, offset,
                                narrow<std::uint32_t>(o.value.size())});
              ++next_lsn_;
            } else if constexpr (std::is_same_v<T, BatchRemove>) {
              std::ignore =
                  active_file().append(next_lsn_, EntryType::Delete,
                                       std::span<const std::byte>{o.key}, {});
              ++next_lsn_;
              t.erase(Key{o.key});
            }
          },
          op);
    }

    std::ignore = active_file().append(next_lsn_++, EntryType::BulkEnd, {}, {});

    key_dir_ = std::move(t).persistent();
    if (opts.sync) {
      active_file().sync();
    }
    maybe_rotate();
  }

  // ── Range iteration ────────────────────────────────────────────────────

  // Returns an input range of (key, value) pairs with keys >= from.
  // Pass an empty span to start from the first key. Each increment reads one
  // value from disk (lazy), routing through the file registry.
  // Throws std::system_error on I/O failure.
  [[nodiscard]] auto iter_from(const ReadOptions & /*opts*/,
                               BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
    const auto k = to_key(from);
    const auto it = from.empty() ? key_dir_.begin() : key_dir_.lower_bound(k);
    return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
        EntryIterator{it, key_dir_.end(), files_}, std::default_sentinel};
  }

  // Returns an input range of keys >= from. Walks the in-memory key directory
  // only; no disk I/O.
  [[nodiscard]] auto keys_from(const ReadOptions & /*opts*/,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
    const auto k = to_key(from);
    const auto it = from.empty() ? key_dir_.begin() : key_dir_.lower_bound(k);
    return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
        KeyIterator{it, key_dir_.end()}, std::default_sentinel};
  }

  // ── Hint file management ───────────────────────────────────────────────

  // Writes a hint file for every sealed data file that does not yet have one.
  // Uses a temp-then-rename protocol for atomicity: writes to ".hint.tmp",
  // fdatasyncs, then renames to ".hint". Idempotent: files with an existing
  // ".hint" are skipped. Called automatically at engine close (~Bytecask).
  //
  // Hint files are never written on the write path (predictable latency).
  void flush_hints() {
    for (auto &file : *files_) {
      if (file.first == active_file_id_) {
        continue;
      }
      const auto stem = file.second->path().stem().string();
      const auto hint_path = dir_ / (stem + ".hint");
      const auto tmp_path = dir_ / (stem + ".hint.tmp");

      if (std::filesystem::exists(hint_path)) {
        continue;
      }

      auto hint = HintFile::OpenForWrite(tmp_path);
      Offset off = 0;
      while (auto result = file.second->scan(off)) {
        const auto &[entry, next] = *result;
        if (entry.entry_type == EntryType::Put ||
            entry.entry_type == EntryType::Delete) {
          hint.append(entry.sequence, entry.entry_type, off, entry.key,
                      narrow<std::uint32_t>(entry.value.size()));
        }
        off = next;
      }
      hint.sync();
      std::filesystem::rename(tmp_path, hint_path);
    }
  }

private:
  explicit Bytecask(std::filesystem::path dir, std::uint64_t max_file_bytes)
      : dir_{std::move(dir)}, rotation_threshold_{max_file_bytes} {
    std::filesystem::create_directories(dir_);
    active_file_id_ = next_file_id_++;
    const auto stem = make_data_file_stem();
    files_ =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>();
    files_->emplace(active_file_id_,
                    std::make_shared<DataFile>(dir_ / (stem + ".data")));
    next_lsn_ = 1;
  }

  // Converts a BytesView (or empty span) into a Key.
  static auto to_key(BytesView v) -> Key { return Key{v.begin(), v.end()}; }

  // Returns a reference to the current active DataFile.
  auto active_file() -> DataFile & { return *files_->at(active_file_id_); }
  auto active_file() const -> const DataFile & {
    return *files_->at(active_file_id_);
  }

  // Seals the active file and opens a new one if the size threshold is met.
  void rotate_active_file() {
    active_file().seal();
    active_file_id_ = next_file_id_++;
    const auto stem = make_data_file_stem();
    auto next =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
            *files_);
    next->emplace(active_file_id_,
                  std::make_shared<DataFile>(dir_ / (stem + ".data")));
    files_ = std::move(next);
  }

  void maybe_rotate() {
    if (active_file().size() >= rotation_threshold_) {
      rotate_active_file();
    }
  }

  std::filesystem::path dir_;
  PersistentOrderedMap<Key, KeyDirEntry> key_dir_;
  // Copy-on-write registry: rotation clones the inner map and replaces
  // files_. In-flight iterators hold a copy of the outer shared_ptr and see
  // the old snapshot without any locking. The fd is never closed while any
  // snapshot retains the inner shared_ptr<DataFile>.
  FileRegistry files_;
  std::uint32_t active_file_id_{0};
  std::uint32_t next_file_id_{0};
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  std::uint64_t next_lsn_{1};
};

} // namespace bytecask
