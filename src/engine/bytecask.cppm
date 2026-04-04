module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
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
#include <time.h>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.engine;

import bytecask.crc32;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_file;
import bytecask.radix_tree;
import bytecask.types;

namespace bytecask {

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

// ---------------------------------------------------------------------------
// SyncGroup — group commit primitive for fdatasync.
//
// Multiple writers that appended to the same file share a single fdatasync.
// The first writer to arrive after the previous sync becomes the leader and
// calls fdatasync; all others wait on the epoch advance. This amortises the
// ~2 ms fdatasync cost across N concurrent writers.
// ---------------------------------------------------------------------------
class SyncGroup {
public:
  void sync(DataFile &file) {
    std::unique_lock lk{mu_};
    const auto my_epoch = epoch_;
    if (syncing_) {
      if (syncing_file_ == &file) {
        // Same file — the in-flight fdatasync covers our append.
        cv_.wait(lk, [&] { return epoch_ > my_epoch; });
        return;
      }
      // Different file (rotation happened) — wait for current sync to finish,
      // then become leader for our own file.
      cv_.wait(lk, [&] { return !syncing_; });
    }
    syncing_ = true;
    syncing_file_ = &file;
    lk.unlock();

    file.sync();

    lk.lock();
    ++epoch_;
    syncing_ = false;
    syncing_file_ = nullptr;
    lk.unlock();
    cv_.notify_all();
  }

private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::uint64_t epoch_{0};
  bool syncing_{false};
  DataFile *syncing_file_{nullptr};
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
// Thread safety: write operations (put, del, apply_batch) are serialised by
// write_mu_ (plain mutex). After producing a new EngineState via pure
// transition methods, the writer publishes it via state_.store().
// Readers call state_.load() without acquiring write_mu_.
// State is published via std::atomic<std::shared_ptr<EngineState>>.
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
  Bytecask(Bytecask &&) = delete;
  Bytecask &operator=(Bytecask &&) = delete;

  // Flush hints and sync the active file before destruction.
  ~Bytecask() {
    auto s = state_.load();
    if (s->files && !s->files->empty()) {
      try {
        s->files->at(s->active_file_id)->sync();
        flush_hints(*s);
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
  [[nodiscard]] auto get(const ReadOptions &opts, BytesView key) const
      -> std::optional<Bytes> {
    Bytes out;
    if (!get(opts, key, out)) {
      return std::nullopt;
    }
    return out;
  }

  // Output-parameter variant: writes the value into out, reusing its existing
  // capacity to amortize allocation across calls. Returns true if the key was
  // found, false otherwise.
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
      const auto offset =
          s.active_file().append(s.next_lsn, EntryType::Put, key, value);

      s = s.apply_put(key, offset, narrow<std::uint32_t>(value.size()));
      if (opts.sync) {
        file_to_sync = s.files->at(s.active_file_id);
      }
      s = maybe_rotate(std::move(s));
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
    if (file_to_sync) {
      sync_group_.sync(*file_to_sync);
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
      if (!s.key_dir.contains(key)) {
        return false;
      }
      std::ignore =
          s.active_file().append(s.next_lsn, EntryType::Delete, key, {});

      s = s.apply_del(key);
      if (opts.sync) {
        file_to_sync = s.files->at(s.active_file_id);
      }
      s = maybe_rotate(std::move(s));
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
    if (file_to_sync) {
      sync_group_.sync(*file_to_sync);
    }
    return true;
  }

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(BytesView key) const -> bool {
    auto s = state_.load();
    return s->key_dir.contains(key);
  }

  // ── Batch ──────────────────────────────────────────────────────────────

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

      std::ignore =
          s.active_file().append(s.next_lsn++, EntryType::BulkBegin, {}, {});

      auto t = s.key_dir.transient();
      for (auto &op : batch.operations_) {
        std::visit(
            [&](auto &o) {
              using T = std::decay_t<decltype(o)>;
              if constexpr (std::is_same_v<T, BatchInsert>) {
                const auto offset =
                    s.active_file().append(s.next_lsn, EntryType::Put,
                                         std::span<const std::byte>{o.key},
                                         std::span<const std::byte>{o.value});
                t.set(std::span<const std::byte>{o.key},
                      KeyDirEntry{s.next_lsn, s.active_file_id, offset,
                                  narrow<std::uint32_t>(o.value.size())});
                ++s.next_lsn;
              } else if constexpr (std::is_same_v<T, BatchRemove>) {
                std::ignore =
                    s.active_file().append(s.next_lsn, EntryType::Delete,
                                         std::span<const std::byte>{o.key}, {});
                ++s.next_lsn;
                t.erase(std::span<const std::byte>{o.key});
              }
            },
            op);
      }

      std::ignore =
          s.active_file().append(s.next_lsn++, EntryType::BulkEnd, {}, {});

      s.key_dir = std::move(t).persistent();
      if (opts.sync) {
        file_to_sync = s.files->at(s.active_file_id);
      }
      s = maybe_rotate(std::move(s));
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
    if (file_to_sync) {
      sync_group_.sync(*file_to_sync);
    }
  }

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

  // ── Hint file management ───────────────────────────────────────────────

  // Writes a hint file for every sealed data file that does not yet have one.
  // Uses a temp-then-rename protocol for atomicity: writes to ".hint.tmp",
  // fdatasyncs, then renames to ".hint". Idempotent: files with an existing
  // ".hint" are skipped. Called automatically at engine close (~Bytecask).
  //
  // Hint files are never written on the write path (predictable latency).
  void flush_hints() {
    flush_hints(*state_.load());
  }

private:
  // Writes hint files for all sealed data files in the given state.
  void flush_hints(const EngineState &s) {
    for (auto &file : *s.files) {
      if (file.first == s.active_file_id) {
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

  explicit Bytecask(std::filesystem::path dir, std::uint64_t max_file_bytes)
      : dir_{std::move(dir)}, rotation_threshold_{max_file_bytes},
        state_{std::make_shared<EngineState>()} {
    std::filesystem::create_directories(dir_);
    EngineState s;
    s.files =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>();
    s = recover_existing_files(std::move(s));
    s.active_file_id = s.next_file_id++;
    const auto stem = make_data_file_stem();
    s.files->emplace(s.active_file_id,
                     std::make_shared<DataFile>(dir_ / (stem + ".data")));
    state_.store(std::make_shared<EngineState>(std::move(s)));
    state_time_.store(now_ns(), std::memory_order_release);
  }

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

  // Reconstructs the key directory from existing .data and .hint files.
  // Returns a new EngineState with key_dir populated and next_lsn set to
  // max_seen + 1. next_file_id is advanced for each recovered file.
  auto recover_existing_files(EngineState s) -> EngineState {
    // Remove stale .hint.tmp files from a prior crash.
    for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
      const auto &p = dir_entry.path();
      if (p.extension() == ".tmp" && p.stem().extension() == ".hint") {
        std::filesystem::remove(p);
      }
    }

    std::uint64_t max_lsn = 0;
    auto transient_key_dir = s.key_dir.transient();
    std::map<Key, std::uint64_t> tombstones;

    for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
      const auto &p = dir_entry.path();
      if (p.extension() != ".data") {
        continue;
      }

      const auto file_id = s.next_file_id++;
      auto data_file = std::make_shared<DataFile>(p);
      data_file->seal();
      s.files->emplace(file_id, data_file);

      // LSN-based freshness: apply an entry only if it is strictly newer
      // than what the key directory already holds for that key.
      //
      // apply_put also consults the tombstone map: if a Delete with a higher
      // sequence was already processed (from another file, in any order), the
      // Put must be suppressed even though the KeyDir has no entry for the key
      // at this moment.
      const auto apply_put = [&](std::uint64_t seq, std::uint64_t file_off,
                                 std::uint32_t val_size,
                                 const std::vector<std::byte> &key) {
        const auto k = Key{key};
        const auto tomb_it = tombstones.find(k);
        if (tomb_it != tombstones.end() && tomb_it->second >= seq) {
          if (seq > max_lsn)
            max_lsn = seq;
          return;
        }
        const auto existing =
            transient_key_dir.get(std::span<const std::byte>{key});
        if (!existing || existing->sequence < seq) {
          transient_key_dir.set(std::span<const std::byte>{key},
                                KeyDirEntry{seq, file_id, file_off, val_size});
        }
        if (seq > max_lsn)
          max_lsn = seq;
      };

      const auto apply_del = [&](std::uint64_t seq,
                                 const std::vector<std::byte> &key) {
        const auto k = Key{key};
        auto &tomb_seq = tombstones[k];
        if (seq > tomb_seq)
          tomb_seq = seq;
        const auto existing =
            transient_key_dir.get(std::span<const std::byte>{key});
        if (existing && existing->sequence < seq) {
          transient_key_dir.erase(std::span<const std::byte>{key});
        }
        if (seq > max_lsn)
          max_lsn = seq;
      };

      const auto hint_path = dir_ / (p.stem().string() + ".hint");

      if (std::filesystem::exists(hint_path)) {
        auto hint = HintFile::OpenForRead(hint_path);
        Offset off = 0;
        while (auto r = hint.scan(off)) {
          const auto &[he, next] = *r;
          if (he.entry_type == EntryType::Put) {
            apply_put(he.sequence, he.file_offset, he.value_size, he.key);
          } else if (he.entry_type == EntryType::Delete) {
            apply_del(he.sequence, he.key);
          }
          off = next;
        }
      } else {
        // Raw scan with a BulkBegin/BulkEnd batch state machine.
        // Entries between BulkBegin and BulkEnd are buffered and applied
        // only when BulkEnd is seen. An incomplete batch — BulkBegin with
        // no matching BulkEnd — is discarded; it indicates a crash mid-write.
        struct PendingEntry {
          std::uint64_t seq;
          EntryType type;
          std::uint64_t file_off;
          std::uint32_t val_size;
          std::vector<std::byte> key;
        };

        bool in_batch = false;
        std::vector<PendingEntry> pending;
        Offset off = 0;

        while (auto result = data_file->scan(off)) {
          const auto entry_off = off;
          const auto &[de, next] = *result;
          switch (de.entry_type) {
          case EntryType::BulkBegin:
            in_batch = true;
            pending.clear();
            if (de.sequence > max_lsn)
              max_lsn = de.sequence;
            break;
          case EntryType::BulkEnd:
            for (auto &pe : pending) {
              if (pe.type == EntryType::Put) {
                apply_put(pe.seq, pe.file_off, pe.val_size, pe.key);
              } else {
                apply_del(pe.seq, pe.key);
              }
            }
            pending.clear();
            in_batch = false;
            if (de.sequence > max_lsn)
              max_lsn = de.sequence;
            break;
          case EntryType::Put:
          case EntryType::Delete:
            if (in_batch) {
              pending.push_back({de.sequence, de.entry_type, entry_off,
                                 narrow<std::uint32_t>(de.value.size()),
                                 de.key});
            } else if (de.entry_type == EntryType::Put) {
              apply_put(de.sequence, entry_off,
                        narrow<std::uint32_t>(de.value.size()), de.key);
            } else {
              apply_del(de.sequence, de.key);
            }
            break;
          }
          off = next;
        }

        if (in_batch) {
          std::cerr << "bytecask: discarding incomplete batch in " << p
                    << " — BulkBegin with no matching BulkEnd (crash "
                       "mid-write)\n";
        }
      }
    }

    s.key_dir = std::move(transient_key_dir).persistent();
    s.next_lsn = max_lsn + 1;
    return s;
  }

  // Seals the active file and opens a new one. Returns a new EngineState.
  [[nodiscard]] auto rotate_active_file(EngineState s) const -> EngineState {
    s.active_file().sync();
    s.active_file().seal();
    return s.apply_rotation(dir_);
  }

  [[nodiscard]] auto maybe_rotate(EngineState s) const -> EngineState {
    if (s.active_file().size() >= rotation_threshold_) {
      return rotate_active_file(std::move(s));
    }
    return s;
  }

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
};

} // namespace bytecask
