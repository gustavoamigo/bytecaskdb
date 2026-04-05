module;
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <queue>
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
// SyncGroup — amortises fdatasync across concurrent writers.
//
// fdatasync is expensive (~2 ms). When N writers finish their writev at
// roughly the same time, one fdatasync can cover all of them. SyncGroup
// batches those writers so only one actually issues the syscall fdatasync 
// while the rest wait and piggyback on its result.
//
// Precondition: callers must have completed their writev before entering
// sync(). fdatasync only flushes data already in the page cache.
//
// Invariant: sync() does not return to a caller until an fdatasync that
// started *after* that caller's writev has completed successfully.
// ---------------------------------------------------------------------------
class SyncGroup {
public:
  void sync(DataFile &file) {
    std::unique_lock lk{mu_};

    // Phase 1: take a ticket — our writev is done, data is in page cache.
    const auto my_ticket = next_ticket_++;

    // Phase 2: wait until covered by a completed sync, or become leader.
    cv_.wait(lk, [&] {
      return current_synced_ticket_ >= my_ticket || !syncing_;
    });

    // A sync that started after our writev already covered us.
    if (current_synced_ticket_ >= my_ticket) return;

    // Phase 3: we are the leader — snapshot the watermark, sync, notify.
    syncing_ = true;
    const auto batch_end = next_ticket_ - 1;
    lk.unlock();

    try {
      file.sync();
    } catch (...) {
      // If sync fails, reset the syncing flag and wake up waiters so they can 
      // retry (or handle their own failure). Do not advance the synced ticket.
      lk.lock();
      syncing_ = false;
      lk.unlock();
      cv_.notify_all();
      throw; // Rethrow the exception to the caller
    }

    lk.lock();
    current_synced_ticket_ = batch_end;
    syncing_ = false;
    lk.unlock();
    cv_.notify_all();
  }

private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::uint64_t next_ticket_{1};            // next ticket to hand out
  std::uint64_t current_synced_ticket_{0};  // highest ticket on disk
  bool syncing_{false};                     // is an fdatasync in flight?
};

// ---------------------------------------------------------------------------
// BackgroundWorker — single persistent background thread for deferred work.
//
// Tasks are enqueued via dispatch() and executed in FIFO order. dispatch() is
// non-blocking; the caller returns immediately after enqueuing. Exceptions
// thrown by tasks are caught, logged to stderr, and swallowed — hint file
// writes are correctness-safe to drop (recovery falls back to raw data scan).
//
// Lifecycle: the thread starts at construction and joins at destruction.
// drain() blocks until the queue is empty and the last task has finished.
//
// Declare BackgroundWorker as the LAST member of any owning class so that
// it destructs first, ensuring the background thread joins before any other
// member (e.g. dir_, state_) is destroyed.
// ---------------------------------------------------------------------------
class BackgroundWorker {
public:
  BackgroundWorker() : thread_{[this] { run(); }} {}

  ~BackgroundWorker() {
    {
      std::unique_lock lk{mu_};
      stop_ = true;
    }
    cv_task_.notify_one();
    thread_.join();
  }

  BackgroundWorker(const BackgroundWorker &) = delete;
  BackgroundWorker &operator=(const BackgroundWorker &) = delete;

  // Enqueue a task. Non-blocking; returns immediately.
  void dispatch(std::function<void()> task) {
    {
      std::unique_lock lk{mu_};
      queue_.push(std::move(task));
    }
    cv_task_.notify_one();
  }

  // Block until the queue is empty and the running task (if any) has finished.
  void drain() {
    std::unique_lock lk{mu_};
    cv_idle_.wait(lk, [this] { return queue_.empty() && active_ == 0; });
  }

private:
  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lk{mu_};
        cv_task_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty())
          return;
        task = std::move(queue_.front());
        queue_.pop();
        ++active_;
      }
      try {
        task();
      } catch (const std::exception &e) {
        std::cerr << "bytecask: background worker exception: " << e.what()
                  << "\n";
      } catch (...) {
        std::cerr << "bytecask: background worker: unknown exception\n";
      }
      {
        std::unique_lock lk{mu_};
        --active_;
      }
      cv_idle_.notify_all();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_task_;
  std::condition_variable cv_idle_;
  std::queue<std::function<void()>> queue_;
  std::size_t active_{0};
  bool stop_{false};
  std::thread thread_;
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
  // Drains all background hint tasks first, then writes any remaining files
  // synchronously. Draining before the synchronous loop avoids racing with
  // in-flight background tasks on the same .hint.tmp file.
  void flush_hints() {
    worker_.drain();
    flush_hints(*state_.load());
  }

private:
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

  // Reconstructs the key directory from hint files.
  // Pre-generates missing hint files from raw data scans (batch-aware),
  // then recovers exclusively from hints — single code path.
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

    // Phase 1: open all data files and generate missing hint files.
    struct RecoveredFile {
      std::uint32_t file_id;
      std::shared_ptr<DataFile> data_file;
      std::filesystem::path hint_path;
    };
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

      files.push_back({file_id, std::move(data_file), hint_path});
    }

    // Phase 2: recover from hint files only.
    std::uint64_t max_lsn = 0;
    auto transient_key_dir = s.key_dir.transient();
    std::map<Key, std::uint64_t> tombstones;

    for (auto &[file_id, data_file, hint_path] : files) {
      const auto apply_put = [&](std::uint64_t seq, std::uint64_t file_off,
                                 std::uint32_t val_size,
                                 std::span<const std::byte> key) {
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
                                 std::span<const std::byte> key) {
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

      auto hint = HintFile::OpenForRead(hint_path);
      Offset off = 0;
      while (auto r = hint.scan_view(off)) {
        const auto &[he, next] = *r;
        if (he.entry_type == EntryType::Put) {
          apply_put(he.sequence, he.file_offset, he.value_size, he.key);
        } else if (he.entry_type == EntryType::Delete) {
          apply_del(he.sequence, he.key);
        }
        off = next;
      }
    }

    s.key_dir = std::move(transient_key_dir).persistent();
    s.next_lsn = max_lsn + 1;
    return s;
  }

  // Seals the active file, opens a new one, and dispatches hint file writing
  // for the now-sealed file to the background worker. Returns a new EngineState.
  // fdatasync on the sealed file remains synchronous for durability correctness.
  [[nodiscard]] auto rotate_active_file(EngineState s) const -> EngineState {
    s.active_file().sync();
    s.active_file().seal();
    // Capture the sealed file before apply_rotation changes active_file_id.
    auto sealed = s.files->at(s.active_file_id);
    auto dir = dir_;
    worker_.dispatch([f = std::move(sealed), d = std::move(dir)] {
      flush_hints_for(f, d);
    });
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
  // Background worker for deferred hint file writes (BC-026).
  // Declared last so it destructs first, joining the background thread before
  // any other member (dir_, state_, files) is destroyed.
  mutable BackgroundWorker worker_;
};

} // namespace bytecask
