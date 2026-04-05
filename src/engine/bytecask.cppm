module;
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <time.h>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.engine;

import bytecask.concurrency;
import bytecask.util;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_file;
import bytecask.radix_tree;
import bytecask.types;

namespace bytecask {

#pragma region Exported types

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

// Owned byte buffer — for return values and batch storage.
export using Bytes = std::vector<std::byte>;

// Non-owning view — used for all input parameters to avoid copies.
export using BytesView = std::span<const std::byte>;

// ---------------------------------------------------------------------------
// Key — lightweight owning byte sequence backed by std::vector<std::byte>.
//
// Used by KeyIterator and EntryIterator to materialise stable keys from
// RadixTreeIterator's transient span, and by the recovery tombstone map
// (needs operator<=>).
// Keys have an upper bound of 65 535 bytes (u16 key_size in the data file).
// ---------------------------------------------------------------------------
export class Key {
public:
  Key() = default;

  explicit Key(BytesView v) : data_{v.begin(), v.end()} {}

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
// FileStats — per-file live/total byte counters for fragmentation tracking.
// Updated under write_mu_ on every write; rebuilt during recovery.
// ---------------------------------------------------------------------------
export struct FileStats {
  std::uint64_t live_bytes{0};
  std::uint64_t total_bytes{0};
};

// Returns the on-disk size of a data file entry given key and value sizes.
inline constexpr auto entry_size(std::size_t key_size,
                                std::size_t value_size) -> std::uint64_t {
  return kHeaderSize + key_size + value_size + kCrcSize;
}

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

    

#pragma endregion Exported types

#pragma region Internal helpers
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

// Nanoseconds since steady_clock epoch. Called on the write path to
// timestamp state publications; the read path compares against this
// with a single relaxed load (plain MOV on x86).
auto now_ns() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace
#pragma endregion

#pragma region EngineState
// ---------------------------------------------------------------------------
// EngineState — immutable snapshot of all mutable engine state.
//
// Each write produces a new EngineState via a pure transition method
// (apply_put, apply_del, apply_rotation). The old state stays alive as long
// as any reader holds a shared_ptr reference. next_file_id and next_lsn
// are writer-only fields, but including them here makes transitions
// self-contained.
// ---------------------------------------------------------------------------
struct EngineState {
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
  // offset is the byte position returned by DataFile::append().
  [[nodiscard]] auto apply_put(BytesView key, std::uint64_t offset,
                               std::uint32_t value_size) const
      -> EngineState {
    auto s = *this;
    s.key_dir =
        s.key_dir.set(key, KeyDirEntry{s.next_lsn, s.active_file_id, offset,
                                       value_size});
    ++s.next_lsn;
    return s;
  }

  // Pure transition: remove a key.
  [[nodiscard]] auto apply_del(BytesView key) const -> EngineState {
    auto s = *this;
    ++s.next_lsn;
    s.key_dir = s.key_dir.erase(key);
    return s;
  }

  // Pure transition: seal the active file and open a new one.
  [[nodiscard]] auto apply_rotation(
      const std::filesystem::path &dir) const -> EngineState {
    auto s = *this;
    s.active_file_id = s.next_file_id++;
    const auto stem = make_data_file_stem();
    auto next_files =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
            *s.files);
    next_files->emplace(
        s.active_file_id,
        std::make_shared<DataFile>(dir / (stem + ".data")));
    s.files = std::move(next_files);
    return s;
  }
};
#pragma endregion

#pragma region Iterators
// ---------------------------------------------------------------------------
// KeyIterator — walks the key directory in ascending key order.
//
// In-memory only: no data file I/O. Satisfies std::input_iterator.
// Wraps RadixTreeIterator<KeyDirEntry> and materializes Key objects from
// the iterator's key span on each advance.
// ---------------------------------------------------------------------------
export class KeyIterator {
public:
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

  void operator++(int) {
    ++cur_;
    cache_key();
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
// EntryIterator — walks the key directory in ascending key order, reading
// values lazily from disk on each dereference.
//
// Satisfies std::input_iterator. Throws std::system_error on I/O failure.
// Wraps RadixTreeIterator<KeyDirEntry>; materializes Key + value on demand
// via a single pread per entry (read_entry). files_ is a snapshot of the
// engine's registry at construction time — independent lifetime from
// Bytecask via shared_ptr.
// ---------------------------------------------------------------------------
export class EntryIterator {
public:
  using value_type = std::pair<Key, Bytes>;
  using difference_type = std::ptrdiff_t;

  EntryIterator() = default;

  EntryIterator(std::shared_ptr<const EngineState> state,
                RadixTreeIterator<KeyDirEntry> cur)
      : state_{std::move(state)}, cur_{std::move(cur)} {}

  auto operator*() const -> const value_type & {
    if (!has_cached_) {
      auto [key_span, dir_entry] = *cur_;
      cached_.first = Key{key_span};
      state_->files->at(dir_entry.file_id)
          ->read_value(dir_entry.file_offset,
                       narrow<std::uint16_t>(key_span.size()),
                       dir_entry.value_size, io_buf_, cached_.second);
      has_cached_ = true;
    }
    return cached_;
  }

  auto operator++() -> EntryIterator & {
    ++cur_;
    has_cached_ = false;
    return *this;
  }

  void operator++(int) {
    ++cur_;
    has_cached_ = false;
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return cur_ == std::default_sentinel;
  }

private:
  std::shared_ptr<const EngineState> state_;
  RadixTreeIterator<KeyDirEntry> cur_;
  mutable value_type cached_;
  mutable Bytes io_buf_;
  mutable bool has_cached_{false};
};
#pragma endregion

#pragma region Options
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

  // When false (default), the write lock is acquired with a blocking wait.
  // When true, a non-blocking try_lock is attempted; if the lock is already
  // held, throws std::system_error with errc::resource_unavailable_try_again.
  bool try_lock{false};
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
};

// Options passed to Bytecask::open().
export struct Options {
  // Active-file rotation threshold in bytes (default 64 MiB). When the active
  // file reaches this size it is sealed and a new one is opened.
  std::uint64_t max_file_bytes{kDefaultRotationThreshold};
  // Number of threads used to rebuild the key directory at open time.
  // 1 selects the serial path; >1 uses file-level fan-in parallelism.
  unsigned recovery_threads{4};
};

// ---------------------------------------------------------------------------
// Bytecask — SWMR key-value store
//
// Intent: Public engine API. open() always creates a fresh active data file.
// Rotation: when the active file exceeds max_file_bytes, it is sealed and a
// new active file is created. Sealed files remain open for reads.
// Hint files are written deferred at engine close to keep write-path latency
// flat and bounded (predictable latency over peak throughput).
//
// Thread safety: write operations (put, del, apply_batch) are serialised by
// write_mu_ (plain mutex). After producing a new EngineState via pure
// transition methods, the writer publishes it via state_.store().
// Readers call state_.load() without acquiring write_mu_.
// State is published via std::atomic<std::shared_ptr<EngineState>>.
// ---------------------------------------------------------------------------
#pragma endregion

export class Bytecask {
public:
  #pragma region Lifecycle
  // Opens or creates a database rooted at dir.
  // Always creates a new active data file.
  // Throws std::system_error if the directory cannot be prepared.
  [[nodiscard]] static auto
  open(std::filesystem::path dir, Options opts = {}) -> Bytecask {
    return Bytecask{std::move(dir), std::move(opts)};
  }

  Bytecask(const Bytecask &) = delete;
  Bytecask &operator=(const Bytecask &) = delete;
  Bytecask(Bytecask &&) = delete;
  Bytecask &operator=(Bytecask &&) = delete;

  // Sync the active file before destruction.
  // hint files for sealed files are handled by the BackgroundWorker, which
  // destructs first (declared last) and drains all pending tasks before any
  // other member is destroyed.
  ~Bytecask() {
    auto s = state_.load();
    if (s->files && !s->files->empty()) {
      try {
        s->files->at(s->active_file_id)->sync();
      } catch (...) {
        // Best-effort: swallow errors in destructors.
      }
    }
    // At destruction no readers are active — purge all stale files.
    for (auto &sf : stale_files_) {
      try {
        auto path = sf.data_file->path();
        sf.data_file.reset();
        std::filesystem::remove(path);
        std::filesystem::remove(sf.hint_path);
      } catch (...) {}
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
  [[nodiscard]] auto get(const ReadOptions &opts, BytesView key,
                         Bytes &out) const -> bool {
    auto s = load_state(opts);
    const auto kv = s->key_dir.get(key);
    if (!kv) {
      return false;
    }
    thread_local Bytes io_buf;
    s->files->at(kv->file_id)
        ->read_value(kv->file_offset, narrow<std::uint16_t>(key.size()),
                     kv->value_size, io_buf, out);
    return true;
  }

  // Writes key → value. Overwrites any existing value.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  void put(const WriteOptions &opts, BytesView key, BytesView value) {
    std::shared_ptr<DataFile> file_to_sync;
    {
      auto guard = acquire_write_lock(opts);
      auto s = *state_.load();

      const auto existing = s.key_dir.get(key);
      if (existing) stats_retire_entry(key, *existing);
      stats_publish_put(s.active_file_id, key, value);

      const auto offset =
          s.active_file().append(s.next_lsn, EntryType::Put, key, value);

      s = s.apply_put(key, offset, narrow<std::uint32_t>(value.size()));
      if (opts.sync) {
        file_to_sync = s.files->at(s.active_file_id);
      }
      s = rotate_if_needed(std::move(s));
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
    if (file_to_sync) {
      sync_group_.sync([&]{ file_to_sync->sync(); });
    }
  }

  // Writes a tombstone for key.
  // Returns true if the key existed and was removed, false if it was absent.
  // Rotates the active file if it has reached the threshold.
  // opts.sync controls whether fdatasync is called after the write.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  [[nodiscard]] auto del(const WriteOptions &opts, BytesView key) -> bool {
    std::shared_ptr<DataFile> file_to_sync;
    {
      auto guard = acquire_write_lock(opts);
      auto s = *state_.load();
      const auto existing = s.key_dir.get(key);
      if (!existing) {
        return false;
      }

      stats_retire_entry(key, *existing);
      stats_publish_tombstone(s.active_file_id, key);

      std::ignore =
          s.active_file().append(s.next_lsn, EntryType::Delete, key, {});

      s = s.apply_del(key);
      if (opts.sync) {
        file_to_sync = s.files->at(s.active_file_id);
      }
      s = rotate_if_needed(std::move(s));
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
    if (file_to_sync) {
      sync_group_.sync([&]{ file_to_sync->sync(); });
    }
    return true;
  }

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(BytesView key) const -> bool {
    auto s = state_.load();
    return s->key_dir.contains(key);
  }

  // Returns a snapshot of per-file stats (under write_mu_).
  // Useful for monitoring fragmentation and testing stats consistency.
  [[nodiscard]] auto file_stats() const
      -> std::map<std::uint32_t, FileStats> {
    std::lock_guard lk{*write_mu_};
    return file_stats_;
  }


  // Atomically applies all operations in batch, wrapped in BulkBegin/BulkEnd.
  // batch is consumed (move-only). No-op if batch.empty().
  // opts.sync controls whether a single fdatasync is issued at the end.
  // Rotates the active file after the sync if the threshold is reached.
  // Throws std::system_error on I/O failure or lock contention (try_lock).
  void apply_batch(const WriteOptions &opts, Batch batch) {
    if (batch.empty()) {
      return;
    }
    std::shared_ptr<DataFile> file_to_sync;
    {
      auto guard = acquire_write_lock(opts);
      auto s = *state_.load();

      stats_publish_bulk_marker(s.active_file_id);
      std::ignore =
          s.active_file().append(s.next_lsn++, EntryType::BulkBegin, {}, {});

      auto t = s.key_dir.transient();
      for (auto &op : batch.operations_) {
        std::visit(
            [&](auto &o) {
              using T = std::decay_t<decltype(o)>;
              if constexpr (std::is_same_v<T, BatchInsert>) {
                const std::span<const std::byte> key_span{o.key};
                const auto existing = t.get(key_span);
                if (existing) stats_retire_entry(key_span, *existing);
                stats_publish_put(s.active_file_id, key_span,
                           std::span<const std::byte>{o.value});

                const auto offset =
                    s.active_file().append(s.next_lsn, EntryType::Put,
                                         key_span,
                                         std::span<const std::byte>{o.value});
                t.set(key_span,
                      KeyDirEntry{s.next_lsn, s.active_file_id, offset,
                                  narrow<std::uint32_t>(o.value.size())});
                ++s.next_lsn;
              } else if constexpr (std::is_same_v<T, BatchRemove>) {
                const std::span<const std::byte> key_span{o.key};
                const auto existing = t.get(key_span);
                if (existing) stats_retire_entry(key_span, *existing);
                stats_publish_tombstone(s.active_file_id, key_span);

                std::ignore =
                    s.active_file().append(s.next_lsn, EntryType::Delete,
                                         key_span, {});
                ++s.next_lsn;
                t.erase(key_span);
              }
            },
            op);
      }

      stats_publish_bulk_marker(s.active_file_id);
      std::ignore =
          s.active_file().append(s.next_lsn++, EntryType::BulkEnd, {}, {});

      s.key_dir = std::move(t).persistent();
      if (opts.sync) {
        file_to_sync = s.files->at(s.active_file_id);
      }
      s = rotate_if_needed(std::move(s));
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
    if (file_to_sync) {
      sync_group_.sync([&]{ file_to_sync->sync(); });
    }
  }

  #pragma endregion

  #pragma region Range iteration
  // ── Range iteration ────────────────────────────────────────────────────

  // Returns an input range of (key, value) pairs with keys >= from.
  // Pass an empty span to start from the first key. Each dereference reads
  // one value from disk via a single pread (lazy). Results are in ascending
  // key order.
  // Throws std::system_error on I/O failure.
  [[nodiscard]] auto iter_from(const ReadOptions & /*opts*/,
                               BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
    auto s = state_.load();
    auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
    return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
        EntryIterator{s, std::move(it)}, std::default_sentinel};
  }

  // Returns an input range of keys >= from. Walks the in-memory key directory
  // only; no disk I/O.
  [[nodiscard]] auto keys_from(const ReadOptions & /*opts*/,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
    auto s = state_.load();
    auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
    return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
        KeyIterator{std::move(it)}, std::default_sentinel};
  }
  #pragma endregion

  #pragma region Vacuum

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
  [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool {
    std::lock_guard vg{*vacuum_mu_};

    // Drain in-flight background hint writes so that vacuum's
    // flush_hints_for call cannot race on the same .hint.tmp file
    // if make_data_file_stem() returns a colliding timestamp.
    worker_.drain();

    // Purge stale files no longer referenced by any reader.
    vacuum_purge_stale_files();

    // Snapshot file_stats and active-file info under write_mu_.
    std::map<std::uint32_t, FileStats> stats_snap;
    std::uint32_t active_id{};
    std::uint64_t active_size{};
    {
      std::lock_guard wg{*write_mu_};
      stats_snap = file_stats_;
      auto s = state_.load();
      active_id = s->active_file_id;
      active_size = s->active_file().size();
    }

    // Find the highest-fragmentation sealed file above threshold.
    std::uint32_t target_id{};
    double worst_frag = 0.0;
    for (const auto &[fid, fs] : stats_snap) {
      if (fid == active_id) continue;
      if (fs.total_bytes == 0) continue;
      const auto frag =
          1.0 - static_cast<double>(fs.live_bytes) /
                    static_cast<double>(fs.total_bytes);
      if (frag > worst_frag && frag > opts.fragmentation_threshold) {
        worst_frag = frag;
        target_id = fid;
      }
    }

    if (target_id == 0 && worst_frag == 0.0) return false;

    // Absorb only if the file is small (below absorb_threshold) and its live
    // data fits in the active file without triggering rotation. Files above
    // absorb_threshold are always compacted into a new sealed file.
    const auto target_live = stats_snap.at(target_id).live_bytes;
    if (target_live <= opts.absorb_threshold &&
        target_live + active_size <= rotation_threshold_) {
      vacuum_absorb_file(target_id);
    } else {
      vacuum_compact_file(target_id);
    }
    return true;
  }
  #pragma endregion

private:
  #pragma region Hint internals
  // Writes the hint file for a single data file using the temp-then-rename
  // protocol. Batch-aware: entries between BulkBegin and BulkEnd are buffered
  // and written only when BulkEnd is seen; an incomplete batch (crash
  // mid-write) is silently discarded. Idempotent: skips files whose .hint
  // already exists.
  static void flush_hints_for(const std::shared_ptr<DataFile> &file,
                              const std::filesystem::path &dir) {
    const auto stem = file->path().stem().string();
    const auto hint_path = dir / (stem + ".hint");
    const auto tmp_path = dir / (stem + ".hint.tmp");

    if (std::filesystem::exists(hint_path)) {
      return;
    }

    struct PendingHint {
      std::uint64_t seq;
      EntryType type;
      std::uint64_t file_off;
      std::uint32_t val_size;
      std::vector<std::byte> key;
    };

    auto hint = HintFile::OpenForWrite(tmp_path);
    bool in_batch = false;
    std::vector<PendingHint> pending;
    Offset off = 0;

    while (auto result = file->scan(off)) {
      const auto entry_off = off;
      const auto &[entry, next] = *result;
      switch (entry.entry_type) {
      case EntryType::BulkBegin:
        in_batch = true;
        pending.clear();
        break;
      case EntryType::BulkEnd:
        for (auto &pe : pending) {
          hint.append(pe.seq, pe.type, pe.file_off, pe.key, pe.val_size);
        }
        pending.clear();
        in_batch = false;
        break;
      case EntryType::Put:
      case EntryType::Delete:
        if (in_batch) {
          pending.push_back({entry.sequence, entry.entry_type, entry_off,
                             narrow<std::uint32_t>(entry.value.size()),
                             entry.key});
        } else {
          hint.append(entry.sequence, entry.entry_type, entry_off, entry.key,
                      narrow<std::uint32_t>(entry.value.size()));
        }
        break;
      }
      off = next;
    }

    if (in_batch) {
      std::cerr << "bytecask: discarding incomplete batch in "
                << file->path() << " while generating hint file\n";
    }

    hint.sync();
    std::filesystem::rename(tmp_path, hint_path);
  }

  // Writes hint files for all sealed data files in the given state.
  void flush_hints(const EngineState &s) {
    for (auto &[file_id, file] : *s.files) {
      if (file_id == s.active_file_id) {
        continue;
      }
      flush_hints_for(file, dir_);
    }
  }

  // Drains background hint tasks then writes hint files for all sealed files.
  void flush_hints() {
    worker_.drain();
    flush_hints(*state_.load());
  }

  #pragma endregion

  #pragma region Vacuum internals

  // Data file removed from the registry by vacuum but potentially still
  // referenced by in-flight readers. Purged when use_count drops to 1.
  //  Protected by vacuum_mu_.
  struct StaleFile {
    std::shared_ptr<DataFile> data_file;
    std::filesystem::path hint_path;
  };

  // Mapping produced during the I/O phase for each live Put that was copied
  // to the new/active file. The commit phase uses these to remap key_dir.
  struct VacuumMapping {
    Bytes key;
    std::uint64_t new_offset;
    std::uint64_t sequence;
    std::uint32_t value_size;
  };

  struct VacuumScanResult {
    std::vector<VacuumMapping> mappings;
    std::uint64_t live_bytes{0};
    std::uint64_t total_bytes{0};
  };

  // Batch-aware scan of source_file: copies live Puts (still current in
  // snap->key_dir for source_file_id) and all tombstones into dest_file.
  // Entries inside BulkBegin..BulkEnd are buffered and emitted only on
  // BulkEnd; incomplete batches at EOF are silently discarded.
  static auto vacuum_scan_and_copy(
      const std::shared_ptr<const EngineState> &snap,
      const DataFile &source_file, DataFile &dest_file,
      std::uint32_t source_file_id) -> VacuumScanResult {
    VacuumScanResult result;

    struct PendingEntry {
      DataEntry entry;
      Offset original_offset;
    };
    bool in_batch = false;
    std::vector<PendingEntry> pending;

    auto emit_entry = [&](const DataEntry &entry, Offset entry_off) {
      switch (entry.entry_type) {
      case EntryType::Put: {
        const auto existing = snap->key_dir.get(entry.key);
        if (existing && existing->file_id == source_file_id &&
            existing->file_offset == entry_off &&
            existing->sequence == entry.sequence) {
          const auto new_off = dest_file.append(
              entry.sequence, EntryType::Put, entry.key, entry.value);
          const auto val_size =
              narrow<std::uint32_t>(entry.value.size());
          const auto sz =
              entry_size(entry.key.size(), entry.value.size());
          result.live_bytes += sz;
          result.total_bytes += sz;
          result.mappings.push_back(
              {Bytes{entry.key.begin(), entry.key.end()}, new_off,
               entry.sequence, val_size});
        }
        break;
      }
      case EntryType::Delete: {
        std::ignore = dest_file.append(
            entry.sequence, EntryType::Delete, entry.key, {});
        result.total_bytes += entry_size(entry.key.size(), 0);
        break;
      }
      default:
        break;
      }
    };

    Offset off = 0;
    while (auto scan_result = source_file.scan(off)) {
      const auto entry_off = off;
      const auto &[entry, next] = *scan_result;

      switch (entry.entry_type) {
      case EntryType::BulkBegin:
        in_batch = true;
        pending.clear();
        break;
      case EntryType::BulkEnd:
        for (auto &pe : pending) {
          emit_entry(pe.entry, pe.original_offset);
        }
        pending.clear();
        in_batch = false;
        break;
      case EntryType::Put:
      case EntryType::Delete:
        if (in_batch) {
          pending.push_back({entry, entry_off});
        } else {
          emit_entry(entry, entry_off);
        }
        break;
      }

      off = next;
    }
    // Incomplete batch at EOF → silently discard pending entries.

    return result;
  }

  // Purge stale files whose DataFile is only held by stale_files_ (no
  // in-flight readers). Called at the start of vacuum() under vacuum_mu_.
  void vacuum_purge_stale_files() {
    std::erase_if(stale_files_, [](StaleFile &sf) {
      if (sf.data_file.use_count() == 1) {
        auto path = sf.data_file->path();
        sf.data_file.reset();
        std::filesystem::remove(path);
        std::filesystem::remove(sf.hint_path);
        return true;
      }
      return false;
    });
  }

  // Remaps key_dir entries from old_file_id to the destination file,
  // updates the files map and file_stats_, and publishes the new
  // EngineState. Caller must hold write_mu_.
  // If new_sealed_file is non-null (compact), a fresh file-id is
  // allocated and the new file is registered. Otherwise (absorb),
  // the active file's stats are incremented.
  void vacuum_commit(std::uint32_t old_file_id,
                     const VacuumScanResult &scan,
                     std::shared_ptr<DataFile> new_sealed_file) {
    auto s = *state_.load();

    const auto dest_file_id = new_sealed_file
        ? s.next_file_id++
        : s.active_file_id;

    auto t = s.key_dir.transient();
    auto actual_live_bytes = scan.live_bytes;
    for (const auto &m : scan.mappings) {
      const std::span<const std::byte> key_span{m.key};
      const auto cur = t.get(key_span);
      if (cur && cur->sequence == m.sequence) {
        t.set(key_span,
              KeyDirEntry{m.sequence, dest_file_id, m.new_offset,
                          m.value_size});
      } else {
        actual_live_bytes -= entry_size(m.key.size(), m.value_size);
      }
    }
    s.key_dir = std::move(t).persistent();

    auto next_files =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
            *s.files);
    next_files->erase(old_file_id);
    if (new_sealed_file) {
      next_files->emplace(dest_file_id, std::move(new_sealed_file));
    }
    s.files = std::move(next_files);

    file_stats_.erase(old_file_id);
    if (dest_file_id != s.active_file_id) {
      file_stats_[dest_file_id] = FileStats{actual_live_bytes,
                                             scan.total_bytes};
    } else {
      auto &active_stats = file_stats_[dest_file_id];
      active_stats.live_bytes += actual_live_bytes;
      active_stats.total_bytes += scan.total_bytes;
    }

    state_.store(std::make_shared<EngineState>(std::move(s)));
    state_time_.store(now_ns(), std::memory_order_release);
  }

  // Stashes the old data file and its hint path for deferred removal
  // once no in-flight readers reference it.
  void vacuum_defer_old_file(
      const std::shared_ptr<const EngineState> &snap,
      std::uint32_t file_id) {
    auto old_data_file = snap->files->at(file_id);
    auto old_hint_path =
        dir_ / (old_data_file->path().stem().string() + ".hint");
    stale_files_.push_back({std::move(old_data_file),
                            std::move(old_hint_path)});
  }

  // Rewrites a sealed file into a new sealed file containing only live
  // entries and tombstones. Called under vacuum_mu_, not write_mu_.
  //
  // The new data file is written to .data.tmp, then renamed atomically.
  // The old file is deferred for cleanup when no readers reference it.
  void vacuum_compact_file(std::uint32_t file_id) {
    auto snap = state_.load();
    const auto &old_file = *snap->files->at(file_id);

    const auto stem = make_data_file_stem();
    const auto tmp_data_path = dir_ / (stem + ".data.tmp");
    const auto final_data_path = dir_ / (stem + ".data");

    VacuumScanResult scan;
    {
      DataFile tmp_file(tmp_data_path);
      scan = vacuum_scan_and_copy(snap, old_file, tmp_file, file_id);
      tmp_file.sync();
    }

    std::filesystem::rename(tmp_data_path, final_data_path);
    auto new_file = std::make_shared<DataFile>(final_data_path);
    new_file->seal();
    flush_hints_for(new_file, dir_);

    {
      std::lock_guard wg{*write_mu_};
      vacuum_commit(file_id, scan, new_file);
    }
    vacuum_defer_old_file(snap, file_id);
  }

  // Appends live entries from a sealed file to the active file, then
  // removes the sealed file. Called under vacuum_mu_.
  // The entire I/O + commit phase runs under write_mu_ because
  // scan_and_copy appends to the shared active DataFile, which is
  // NOT thread-safe (requires external synchronization).
  // The old file is deferred for cleanup when no readers reference it.
  void vacuum_absorb_file(std::uint32_t file_id) {
    auto snap = state_.load();
    const auto &old_file = *snap->files->at(file_id);

    {
      std::lock_guard wg{*write_mu_};
      auto &active = snap->active_file();
      auto scan = vacuum_scan_and_copy(snap, old_file, active, file_id);
      active.sync();
      vacuum_commit(file_id, scan, nullptr);
    }
    vacuum_defer_old_file(snap, file_id);
  }

  #pragma endregion

  #pragma region FileStats helpers
  // ── FileStats helpers (called under write_mu_) ─────────────────────────

  // Marks an existing entry as dead in its file's stats.
  void stats_retire_entry(BytesView key,
                          const KeyDirEntry &old) {
    file_stats_[old.file_id].live_bytes -=
        entry_size(key.size(), old.value_size);
  }

  // Records a new Put entry: live + total on the active file.
  void stats_publish_put(std::uint32_t active_file_id,
                  BytesView key, BytesView value) {
    const auto sz = entry_size(key.size(), value.size());
    auto &st = file_stats_[active_file_id];
    st.live_bytes += sz;
    st.total_bytes += sz;
  }

  // Records a tombstone (Delete): total only on the active file.
  void stats_publish_tombstone(std::uint32_t active_file_id, BytesView key) {
    file_stats_[active_file_id].total_bytes += entry_size(key.size(), 0);
  }

  // Records a bulk marker (BulkBegin / BulkEnd): total only.
  void stats_publish_bulk_marker(std::uint32_t active_file_id) {
    file_stats_[active_file_id].total_bytes += kHeaderSize + kCrcSize;
  }

  #pragma endregion

  #pragma region Construction

  explicit Bytecask(std::filesystem::path dir, Options opts)
      : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
        state_{std::make_shared<EngineState>()} {
    std::filesystem::create_directories(dir_);
    EngineState s;
    s.files =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>();
    if (opts.recovery_threads <= 1) {
      s = recovery_load_serial(std::move(s));
    } else {
      s = recovery_load_parallel(std::move(s), opts.recovery_threads);
    }
    s.active_file_id = s.next_file_id++;
    const auto stem = make_data_file_stem();
    s.files->emplace(s.active_file_id,
                     std::make_shared<DataFile>(dir_ / (stem + ".data")));
    // Initialize stats for the new active file.
    file_stats_[s.active_file_id] = FileStats{};
    state_.store(std::make_shared<EngineState>(std::move(s)));
    state_time_.store(now_ns(), std::memory_order_release);
  }

  #pragma endregion

  #pragma region State access

  // Returns the engine state from a thread-local cache.
  // The hot path is a single relaxed load of state_time_ (plain MOV on x86).
  // The snapshot is refreshed only when the last write timestamp exceeds
  // staleness_tolerance (session mode: tolerance=0, refreshes on every write).
  // Returns a reference to the thread-local snapshot. The snapshot stays
  // alive until the same thread calls load_state again, so callers must
  // not stash the reference across a second load_state call.
  [[nodiscard]] auto load_state(const ReadOptions &opts) const
      -> const std::shared_ptr<const EngineState> & {
    struct TlState {
      std::shared_ptr<const EngineState> snapshot;
      std::int64_t last_write_time{0};
    };
    thread_local TlState tl;
    const auto wt = state_time_.load(std::memory_order_relaxed);
    const auto tolerance =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            opts.staleness_tolerance)
            .count();
    if (wt - tl.last_write_time > tolerance) {
      tl.snapshot = state_.load();
      tl.last_write_time = wt;
    }
    return tl.snapshot;
  }

  // Acquires the write mutex. Blocking or try-lock based on opts.try_lock.
  auto acquire_write_lock(const WriteOptions &opts)
      -> std::unique_lock<std::mutex> {
    if (opts.try_lock) {
      std::unique_lock<std::mutex> lk{*write_mu_, std::try_to_lock};
      if (!lk.owns_lock()) {
        throw std::system_error{
            std::make_error_code(std::errc::resource_unavailable_try_again),
            "bytecask: write lock unavailable"};
      }
      return lk;
    }
    return std::unique_lock<std::mutex>{*write_mu_};
  }

  #pragma endregion

  #pragma region Recovery
  // Intermediate result produced by each recovery worker and consumed
  // by the fan-in merge.
  struct RecoveryResult {
    PersistentRadixTree<KeyDirEntry> key_dir;
    std::map<Key, std::uint64_t> tombstones;
    std::uint64_t max_lsn{0};
    std::map<std::uint32_t, FileStats> file_stats;
  };

  struct RecoveredFile {
    std::uint32_t file_id;
    std::shared_ptr<DataFile> data_file;
    std::filesystem::path hint_path;
    std::uint64_t total_bytes{0};
  };

  // Phase 1 shared by serial and parallel recovery: remove stale .hint.tmp
  // files, open all data files, seal them, register in s.files, and
  // generate missing hint files. Returns the RecoveredFile list.
  auto recovery_prepare_files(EngineState &s) -> std::vector<RecoveredFile> {
    // Remove stale .hint.tmp and .data.tmp files from a prior crash.
    for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
      const auto &p = dir_entry.path();
      if (p.extension() == ".tmp" &&
          (p.stem().extension() == ".hint" ||
           p.stem().extension() == ".data")) {
        std::filesystem::remove(p);
      }
    }

    std::vector<RecoveredFile> files;

    for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
      const auto &p = dir_entry.path();
      if (p.extension() != ".data") {
        continue;
      }

      const auto file_id = s.next_file_id++;
      auto data_file = std::make_shared<DataFile>(p);
      data_file->seal();
      s.files->emplace(file_id, data_file);

      const auto hint_path = dir_ / (p.stem().string() + ".hint");
      if (!std::filesystem::exists(hint_path)) {
        flush_hints_for(data_file, dir_);
      }

      files.push_back({file_id, std::move(data_file), hint_path,
                       std::filesystem::file_size(p)});
    }

    return files;
  }

  // Builds a RecoveryResult from a subset of hint files.
  // Each worker calls this independently — no shared mutable state.
  static auto recovery_build_from_hints(std::span<RecoveredFile> files)
      -> RecoveryResult {
    std::uint64_t max_lsn = 0;
    auto t = PersistentRadixTree<KeyDirEntry>{}.transient();
    std::map<Key, std::uint64_t> tombstones;
    std::map<std::uint32_t, FileStats> fstats;

    // Seed total_bytes for each file from the filesystem.
    for (const auto &rf : files) {
      fstats[rf.file_id].total_bytes = rf.total_bytes;
    }

    for (auto &[file_id, data_file, hint_path, tb] : files) {
      auto hint = HintFile::OpenForRead(hint_path);
      Offset off = 0;
      while (auto r = hint.scan(off)) {
        const auto &[he, next] = *r;
        if (he.entry_type == EntryType::Put) {
          const auto k = Key{he.key};
          const auto tomb_it = tombstones.find(k);
          if (tomb_it != tombstones.end() && tomb_it->second >= he.sequence) {
            if (he.sequence > max_lsn) max_lsn = he.sequence;
            off = next;
            continue;
          }
          const auto existing = t.get(he.key);
          if (!existing || existing->sequence < he.sequence) {
            // Displace loser if overwriting.
            if (existing) {
              fstats[existing->file_id].live_bytes -=
                  entry_size(he.key.size(), existing->value_size);
            }
            fstats[file_id].live_bytes +=
                entry_size(he.key.size(), he.value_size);
            t.set(he.key,
                  KeyDirEntry{he.sequence, file_id, he.file_offset,
                              he.value_size});
          }
        } else if (he.entry_type == EntryType::Delete) {
          const auto k = Key{he.key};
          auto &tomb_seq = tombstones[k];
          if (he.sequence > tomb_seq) tomb_seq = he.sequence;
          const auto existing = t.get(he.key);
          if (existing && existing->sequence < he.sequence) {
            fstats[existing->file_id].live_bytes -=
                entry_size(he.key.size(), existing->value_size);
            t.erase(he.key);
          }
        }
        if (he.sequence > max_lsn) max_lsn = he.sequence;
        off = next;
      }
    }

    return {std::move(t).persistent(), std::move(tombstones), max_lsn,
            std::move(fstats)};
  }

  // Merges two RecoveryResults. Tree merge uses LSN-based conflict
  // resolution, then tombstones from both sides are cross-applied to
  // suppress stale PUTs. Tombstone maps and file_stats are unioned.
  // live_bytes are recomputed from the merged tree after all conflict
  // resolution (the merge resolver doesn't have access to key sizes).
  static auto recovery_merge_results(RecoveryResult a, RecoveryResult b)
      -> RecoveryResult {
    // Union file_stats (file IDs are disjoint across workers).
    // Preserve total_bytes from both sides; live_bytes will be recomputed.
    auto &merged_stats = a.file_stats;
    for (auto &[fid, fs] : b.file_stats) {
      merged_stats[fid] = fs;
    }

    auto lsn_resolver = [](const KeyDirEntry &x, const KeyDirEntry &y) {
      return (x.sequence >= y.sequence) ? x : y;
    };

    auto merged = PersistentRadixTree<KeyDirEntry>::merge(
        a.key_dir, b.key_dir, lsn_resolver);

    // Cross-apply B's tombstones to suppress stale PUTs from A.
    for (const auto &[key, tomb_seq] : b.tombstones) {
      std::span<const std::byte> key_span{key.begin(), key.size()};
      const auto entry = merged.get(key_span);
      if (entry && entry->sequence < tomb_seq) {
        merged = merged.erase(key_span);
      }
    }

    // Cross-apply A's tombstones to suppress stale PUTs from B.
    for (const auto &[key, tomb_seq] : a.tombstones) {
      std::span<const std::byte> key_span{key.begin(), key.size()};
      const auto entry = merged.get(key_span);
      if (entry && entry->sequence < tomb_seq) {
        merged = merged.erase(key_span);
      }
    }

    // Recompute live_bytes from the merged tree. The merge resolver and
    // tombstone cross-application may have displaced entries whose key sizes
    // are not available inside the resolver callback.
    for (auto &[fid, fs] : merged_stats) {
      fs.live_bytes = 0;
    }
    for (auto it = merged.begin(); it != std::default_sentinel; ++it) {
      const auto &[key_span, kde] = *it;
      merged_stats[kde.file_id].live_bytes +=
          entry_size(key_span.size(), kde.value_size);
    }

    // Union tombstone maps (max seq per key) for subsequent rounds.
    auto &merged_tombs = a.tombstones;
    for (auto &[key, seq] : b.tombstones) {
      auto &existing = merged_tombs[key];
      if (seq > existing) existing = seq;
    }

    return {std::move(merged), std::move(merged_tombs),
            std::max(a.max_lsn, b.max_lsn), std::move(merged_stats)};
  }

  // Reconstructs the key directory from hint files (serial path).
  // Pre-generates missing hint files from raw data scans (batch-aware),
  // then recovers exclusively from hints — single code path.
  // Returns a new EngineState with key_dir populated and next_lsn set to
  // max_seen + 1. next_file_id is advanced for each recovered file.
  auto recovery_load_serial(EngineState s) -> EngineState {
    auto files = recovery_prepare_files(s);

    // Seed total_bytes for each file from the filesystem.
    for (const auto &rf : files) {
      file_stats_[rf.file_id].total_bytes = rf.total_bytes;
    }

    // Recover from hint files only.
    std::uint64_t max_lsn = 0;
    auto transient_key_dir = s.key_dir.transient();
    std::map<Key, std::uint64_t> tombstones;

    for (auto &[file_id, data_file, hint_path, tb] : files) {
      auto hint = HintFile::OpenForRead(hint_path);
      Offset off = 0;
      while (auto r = hint.scan(off)) {
        const auto &[he, next] = *r;
        if (he.entry_type == EntryType::Put) {
          const auto k = Key{he.key};
          const auto tomb_it = tombstones.find(k);
          if (tomb_it != tombstones.end() && tomb_it->second >= he.sequence) {
            if (he.sequence > max_lsn) max_lsn = he.sequence;
            off = next;
            continue;
          }
          const auto existing = transient_key_dir.get(he.key);
          if (!existing || existing->sequence < he.sequence) {
            if (existing) {
              file_stats_[existing->file_id].live_bytes -=
                  entry_size(he.key.size(), existing->value_size);
            }
            file_stats_[file_id].live_bytes +=
                entry_size(he.key.size(), he.value_size);
            transient_key_dir.set(
                he.key,
                KeyDirEntry{he.sequence, file_id, he.file_offset,
                            he.value_size});
          }
        } else if (he.entry_type == EntryType::Delete) {
          const auto k = Key{he.key};
          auto &tomb_seq = tombstones[k];
          if (he.sequence > tomb_seq) tomb_seq = he.sequence;
          const auto existing = transient_key_dir.get(he.key);
          if (existing && existing->sequence < he.sequence) {
            file_stats_[existing->file_id].live_bytes -=
                entry_size(he.key.size(), existing->value_size);
            transient_key_dir.erase(he.key);
          }
        }
        if (he.sequence > max_lsn) max_lsn = he.sequence;
        off = next;
      }
    }

    s.key_dir = std::move(transient_key_dir).persistent();
    s.next_lsn = max_lsn + 1;
    return s;
  }

  // Parallel recovery (v1): file-level partitioning with fan-in merge.
  // Round-robin assigns files to W workers, each builds a RecoveryResult,
  // then pairwise merges reduce to a single result in ⌈log₂(W)⌉ rounds.
  auto recovery_load_parallel(EngineState s, unsigned recovery_threads)
      -> EngineState {
    auto files = recovery_prepare_files(s);

    if (files.empty()) {
      return s;
    }

    auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
    if (W == 0) W = 1;

    // Round-robin file assignment to W workers.
    std::vector<std::vector<RecoveredFile>> worker_files(W);
    for (unsigned i = 0; i < files.size(); ++i) {
      worker_files[i % W].push_back(std::move(files[i]));
    }

    // Phase 2: parallel build.
    std::vector<RecoveryResult> results(W);
    {
      std::vector<std::jthread> threads;
      threads.reserve(W);
      for (unsigned i = 0; i < W; ++i) {
        threads.emplace_back([&results, &worker_files, i] {
          results[i] = recovery_build_from_hints(worker_files[i]);
        });
      }
    }

    // Phase 3: parallel fan-in merge.
    while (results.size() > 1) {
      const auto count = results.size();
      const auto pairs = count / 2;
      std::vector<RecoveryResult> next(pairs);

      {
        std::vector<std::jthread> threads;
        threads.reserve(pairs);
        for (std::size_t i = 0; i < pairs; ++i) {
          threads.emplace_back([&results, &next, i] {
            next[i] = recovery_merge_results(std::move(results[i * 2]),
                                    std::move(results[i * 2 + 1]));
          });
        }
      }

      // Odd element carries forward.
      if (count % 2 != 0) {
        next.push_back(std::move(results.back()));
      }

      results = std::move(next);
    }

    // Phase 4: assembly.
    s.key_dir = std::move(results[0].key_dir);
    s.next_lsn = results[0].max_lsn + 1;
    file_stats_ = std::move(results[0].file_stats);
    return s;
  }
  #pragma endregion

  #pragma region File rotation
  // Seals the active file, opens a new one, and dispatches hint file writing
  // for the now-sealed file to the background worker. Returns a new EngineState.
  // fdatasync on the sealed file remains synchronous for durability correctness.
  [[nodiscard]] auto rotate_active_file(EngineState s) -> EngineState {
    s.active_file().sync();
    s.active_file().seal();
    // Capture the sealed file before apply_rotation changes active_file_id.
    auto sealed = s.files->at(s.active_file_id);
    auto dir = dir_;
    worker_.dispatch([f = std::move(sealed), d = std::move(dir)] {
      flush_hints_for(f, d);
    });
    s = s.apply_rotation(dir_);
    // Initialize stats for the new active file.
    file_stats_[s.active_file_id] = FileStats{};
    return s;
  }

  [[nodiscard]] auto rotate_if_needed(EngineState s) -> EngineState {
    if (s.active_file().size() >= rotation_threshold_) {
      return rotate_active_file(std::move(s));
    }
    return s;
  }
  #pragma endregion

  #pragma region Member variables
  std::filesystem::path dir_;
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  // All mutable state — SWMR. Writers publish via state_.store()
  // under write_mu_; readers call state_.load() (never acquiring write_mu_).
  std::atomic<std::shared_ptr<EngineState>> state_;
  // Written (release) by every state_.store() with steady_clock::now().
  // Stale readers compare this against a thread-local timestamp with a
  // single relaxed load (plain MOV on x86) to decide whether to refresh.
  std::atomic<std::int64_t> state_time_{0};
  // Serialises writers (put, del, apply_batch). Readers never acquire this.
  std::unique_ptr<std::mutex> write_mu_{std::make_unique<std::mutex>()};
  // Group commit: batches concurrent fdatasync calls so one sync covers
  // multiple writers. See design doc "Group commit" section.
  SyncGroup sync_group_;
  // Serialises vacuum() calls. Separate from write_mu_ so vacuum I/O does
  // not block normal writes. Only the commit phase briefly acquires write_mu_.
  std::unique_ptr<std::mutex> vacuum_mu_{std::make_unique<std::mutex>()};
  // See StaleFile in #pragma region Vacuum internals.
  // Protected by vacuum_mu_.
  std::vector<StaleFile> stale_files_;
  // Per-file live/total byte counters for vacuum fragmentation tracking.
  // Updated under write_mu_ on every write; rebuilt during recovery.
  std::map<std::uint32_t, FileStats> file_stats_;
  // Background worker for deferred hint file writes (BC-026).
  // Declared last so it destructs first, joining the background thread before
  // any other member (dir_, state_, files) is destroyed.
  mutable BackgroundWorker worker_;
  #pragma endregion
};
} // namespace bytecask
