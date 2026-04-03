module;
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <immer/array.hpp>
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
// Key — lightweight owning byte sequence backed by immer::array<std::byte>.
//
// Copies are O(1) (reference-count bump). Used by KeyIterator and
// EntryIterator to materialise stable keys from RadixTreeIterator's
// transient span, and by the recovery tombstone map (needs operator<=>).
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
  immer::array<std::byte> data_;
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

// Immutable snapshot of the key directory and file registry.
// Readers copy this under a brief mutex. Both fields are O(1) to copy.
struct DirSnapshot {
  PersistentRadixTree<KeyDirEntry> key_dir;
  FileRegistry files;
  std::uint32_t active_file_id{};
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

  EntryIterator(RadixTreeIterator<KeyDirEntry> cur, FileRegistry files)
      : cur_{std::move(cur)}, files_{std::move(files)} {}

  auto operator*() const -> const value_type & {
    if (!has_cached_) {
      auto [key_span, dir_entry] = *cur_;
      cached_.first = Key{key_span};
      files_->at(dir_entry.file_id)
          ->read_entry(dir_entry.file_offset,
                       narrow<std::uint16_t>(key_span.size()),
                       dir_entry.value_size, io_buf_);
      extract_value_into(io_buf_, cached_.second);
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
  RadixTreeIterator<KeyDirEntry> cur_;
  FileRegistry files_;
  mutable value_type cached_;
  mutable Bytes io_buf_;
  mutable bool has_cached_{false};
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

  // When false (default), the write lock is acquired with a blocking wait.
  // When true, a non-blocking try_lock is attempted; if the lock is already
  // held, throws std::system_error with errc::resource_unavailable_try_again.
  bool try_lock{false};
};

// ReadOptions — placeholder for future read-path knobs (e.g. verify_checksums).
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
// Thread safety: write operations (put, del, apply_batch) are serialised by
// write_mu_ (plain mutex). After mutating key_dir_/files_, the writer
// publishes a lightweight snapshot under snapshot_mu_ (~10 ns hold).
// Readers copy the snapshot under snapshot_mu_ and then release; all
// subsequent I/O proceeds without any lock held. Readers never block on
// write_mu_, eliminating the writer-starvation problem of shared_mutex.
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
  [[nodiscard]] auto get(const ReadOptions & /*opts*/, BytesView key,
                         Bytes &out) const -> bool {
    auto [kd, fs] = read_snapshot();
    const auto kv = kd.get(key);
    if (!kv) {
      return false;
    }
    thread_local Bytes io_buf;
    fs->at(kv->file_id)
        ->read_entry(kv->file_offset, narrow<std::uint16_t>(key.size()),
                     kv->value_size, io_buf);
    extract_value_into(io_buf, out);
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
      const auto offset =
          active_file().append(next_lsn_, EntryType::Put, key, value);

      key_dir_ =
          key_dir_.set(key, KeyDirEntry{next_lsn_, active_file_id_, offset,
                                        narrow<std::uint32_t>(value.size())});
      ++next_lsn_;
      if (opts.sync) {
        file_to_sync = files_->at(active_file_id_);
      }
      maybe_rotate();
      publish_snapshot();
    }
    if (file_to_sync) {
      file_to_sync->sync();
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
      if (!key_dir_.contains(key)) {
        return false;
      }
      std::ignore = active_file().append(next_lsn_, EntryType::Delete, key, {});
      ++next_lsn_;

      key_dir_ = key_dir_.erase(key);
      if (opts.sync) {
        file_to_sync = files_->at(active_file_id_);
      }
      maybe_rotate();
      publish_snapshot();
    }
    if (file_to_sync) {
      file_to_sync->sync();
    }
    return true;
  }

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(BytesView key) const -> bool {
    auto [kd, fs] = read_snapshot();
    return kd.contains(key);
  }

  // Flushes all pending writes to physical storage (fdatasync on the active
  // file). Useful for callers that issue multiple sync=false writes and then
  // want a single fdatasync to cover them all.
  //
  // Correctness: if a rotation occurred during a batch of writes, the
  // rotated file was fdatasynced inside rotate_active_file() before being
  // sealed, so all pre-rotation writes are already durable. This call covers
  // any writes that landed in the new active file after rotation.
  void sync() {
    std::shared_ptr<DataFile> f;
    {
      std::lock_guard lk{*snapshot_mu_};
      f = snapshot_.files->at(snapshot_.active_file_id);
    }
    f->sync();
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

      std::ignore =
          active_file().append(next_lsn_++, EntryType::BulkBegin, {}, {});

      auto t = key_dir_.transient();
      for (auto &op : batch.operations_) {
        std::visit(
            [&](auto &o) {
              using T = std::decay_t<decltype(o)>;
              if constexpr (std::is_same_v<T, BatchInsert>) {
                const auto offset =
                    active_file().append(next_lsn_, EntryType::Put,
                                         std::span<const std::byte>{o.key},
                                         std::span<const std::byte>{o.value});
                t.set(std::span<const std::byte>{o.key},
                      KeyDirEntry{next_lsn_, active_file_id_, offset,
                                  narrow<std::uint32_t>(o.value.size())});
                ++next_lsn_;
              } else if constexpr (std::is_same_v<T, BatchRemove>) {
                std::ignore =
                    active_file().append(next_lsn_, EntryType::Delete,
                                         std::span<const std::byte>{o.key}, {});
                ++next_lsn_;
                t.erase(std::span<const std::byte>{o.key});
              }
            },
            op);
      }

      std::ignore =
          active_file().append(next_lsn_++, EntryType::BulkEnd, {}, {});

      key_dir_ = std::move(t).persistent();
      if (opts.sync) {
        file_to_sync = files_->at(active_file_id_);
      }
      maybe_rotate();
      publish_snapshot();
    }
    if (file_to_sync) {
      file_to_sync->sync();
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
    auto [kd, fs] = read_snapshot();
    auto it = from.empty() ? kd.begin() : kd.lower_bound(from);
    return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
        EntryIterator{std::move(it), std::move(fs)}, std::default_sentinel};
  }

  // Returns an input range of keys >= from. Walks the in-memory key directory
  // only; no disk I/O.
  [[nodiscard]] auto keys_from(const ReadOptions & /*opts*/,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
    auto [kd, fs] = read_snapshot();
    auto it = from.empty() ? kd.begin() : kd.lower_bound(from);
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
    files_ =
        std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>();
    const auto max_lsn = recover_existing_files();
    next_lsn_ = max_lsn + 1;
    active_file_id_ = next_file_id_++;
    const auto stem = make_data_file_stem();
    files_->emplace(active_file_id_,
                    std::make_shared<DataFile>(dir_ / (stem + ".data")));
    snapshot_ = {key_dir_, files_, active_file_id_};
  }

  // Snapshots key_dir_ and files_ under a brief mutex lock.
  // Hold time is ~10 ns (two O(1) copies). Readers never block on write_mu_.
  auto read_snapshot() const
      -> std::pair<PersistentRadixTree<KeyDirEntry>, FileRegistry> {
    std::lock_guard lk{*snapshot_mu_};
    return {snapshot_.key_dir, snapshot_.files};
  }

  // Copies key_dir_, files_, and active_file_id_ into the published snapshot.
  // Called by writers after mutating key_dir_/files_, under write_mu_.
  void publish_snapshot() {
    std::lock_guard lk{*snapshot_mu_};
    snapshot_ = {key_dir_, files_, active_file_id_};
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

  // Returns a reference to the current active DataFile.
  auto active_file() -> DataFile & { return *files_->at(active_file_id_); }
  auto active_file() const -> const DataFile & {
    return *files_->at(active_file_id_);
  }

  // Reconstructs the key directory from existing .data and .hint files.
  // Removes stale .hint.tmp files left by a prior crash, then scans each
  // .data file — via its .hint companion when present, raw otherwise.
  // LSN is the sole freshness authority; file processing order is
  // intentionally unspecified and never relied upon for correctness.
  // No hint files are generated here; they are produced on clean shutdown
  // by flush_hints(). Returns the highest sequence number seen, or 0 if
  // the directory contains no .data files.
  auto recover_existing_files() -> std::uint64_t {
    // Remove stale .hint.tmp files from a prior crash.
    for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
      const auto &p = dir_entry.path();
      if (p.extension() == ".tmp" && p.stem().extension() == ".hint") {
        std::filesystem::remove(p);
      }
    }

    std::uint64_t max_lsn = 0;
    // Single transient for the entire recovery pass: one persistent snapshot
    // at the end instead of one per entry.
    auto transient_key_dir = key_dir_.transient();
    // Highest Delete sequence seen per key across all files, regardless of
    // processing order. Prevents a stale Put (lower seq) processed after a
    // Delete (higher seq, different file) from being incorrectly inserted.
    std::map<Key, std::uint64_t> tombstones;

    for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
      const auto &p = dir_entry.path();
      if (p.extension() != ".data") {
        continue;
      }

      const auto file_id = next_file_id_++;
      auto data_file = std::make_shared<DataFile>(p);
      data_file->seal();
      files_->emplace(file_id, data_file);

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

    key_dir_ = std::move(transient_key_dir).persistent();
    return max_lsn;
  }

  // Seals the active file and opens a new one if the size threshold is met.
  // fdatasync is called before sealing: any writes that reached this file
  // (including prior sync=false writes) are durable by the time the file
  // becomes immutable.
  void rotate_active_file() {
    active_file().sync();
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
  PersistentRadixTree<KeyDirEntry> key_dir_;
  // Copy-on-write registry: rotation clones the inner map and replaces
  // files_. In-flight iterators hold a copy of the outer shared_ptr and see
  // the old snapshot without any locking. The fd is never closed while any
  // snapshot retains the inner shared_ptr<DataFile>.
  FileRegistry files_;
  // Published snapshot: readers copy under snapshot_mu_, writers publish
  // after mutations. Both PersistentRadixTree and FileRegistry are O(1) copies.
  DirSnapshot snapshot_;
  // Protects snapshot_ reads/writes. Held ~10 ns; never during I/O.
  mutable std::unique_ptr<std::mutex> snapshot_mu_{
      std::make_unique<std::mutex>()};
  // Serialises writers (put, del, apply_batch). Readers never acquire this.
  std::unique_ptr<std::mutex> write_mu_{std::make_unique<std::mutex>()};
  std::uint32_t active_file_id_{0};
  std::uint32_t next_file_id_{0};
  std::uint64_t rotation_threshold_{kDefaultRotationThreshold};
  std::uint64_t next_lsn_{1};
};

} // namespace bytecask
