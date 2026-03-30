module;
#include <time.h>

export module bytecask.engine;

import bytecask.crc32;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.persistent_ordered_map;
import std;

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
// file_id identifies the data file that holds this entry (timestamp stem).
// file_offset is the byte offset where the full DataEntry begins.
// ---------------------------------------------------------------------------
export struct KeyDirEntry {
  std::uint64_t sequence{};
  std::string file_id;
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
// ---------------------------------------------------------------------------
export class EntryIterator {
public:
  using value_type = std::pair<Bytes, Bytes>;
  using difference_type = std::ptrdiff_t;

  EntryIterator() = default;

  EntryIterator(PersistentOrderedMap<Key, KeyDirEntry>::const_iterator cur,
                PersistentOrderedMap<Key, KeyDirEntry>::const_iterator end,
                const DataFile *file)
      : cur_{cur}, end_{end}, file_{file} {}

  // Reads the value from disk on demand (lazy). Caches the result until ++.
  auto operator*() const -> const value_type & {
    if (!cached_) {
      auto entry = file_->read(cur_->value.file_offset);
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
  const DataFile *file_{nullptr};
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

// ---------------------------------------------------------------------------
// Bytecask — SWMR key-value store
//
// Intent: Public engine API. open() always creates a fresh active data file;
// BC-019 will add recovery from existing files.
//
// Thread safety: NOT thread-safe. Follow the SWMR contract at the call site.
// ---------------------------------------------------------------------------
export class Bytecask {
public:
  // Opens or creates a database rooted at dir.
  // Always creates a new active data file (recovery is BC-019).
  // Throws std::system_error if the directory cannot be prepared.
  [[nodiscard]] static auto open(std::filesystem::path dir) -> Bytecask {
    return Bytecask{std::move(dir)};
  }

  Bytecask(const Bytecask &) = delete;
  Bytecask &operator=(const Bytecask &) = delete;
  Bytecask(Bytecask &&) noexcept = default;
  Bytecask &operator=(Bytecask &&) noexcept = default;

  // Flush any buffered writes before destruction.
  ~Bytecask() {
    if (active_file_.has_value()) {
      active_file_->sync();
    }
  }

  // ── Primary operations ─────────────────────────────────────────────────

  // Returns the value for key, or std::nullopt if the key does not exist.
  // Throws std::system_error on I/O failure or std::runtime_error on CRC
  // mismatch.
  [[nodiscard]] auto get(BytesView key) const -> std::optional<Bytes> {
    const auto kv = key_dir_.get(to_key(key));
    if (!kv) {
      return std::nullopt;
    }
    const auto entry = active_file_->read(kv->file_offset);
    return entry.value;
  }

  // Writes key → value. Overwrites any existing value.
  // Throws std::system_error on I/O failure.
  void put(BytesView key, BytesView value) {
    auto k = to_key(key);
    const auto offset =
        active_file_->append(next_lsn_, EntryType::Put, key, value);

    // As part of the SWMR, the key dir pointer is replaced here to ensure that
    // readers only read written data
    key_dir_ = key_dir_.set(std::move(k),
                            KeyDirEntry{next_lsn_, active_file_id_, offset,
                                        narrow<std::uint32_t>(value.size())});
    ++next_lsn_;
    active_file_->sync();
  }

  // Writes a tombstone for key.
  // Returns true if the key existed and was removed, false if it was absent.
  // Throws std::system_error on I/O failure.
  [[nodiscard]] auto del(BytesView key) -> bool {
    const auto k = to_key(key);
    if (!key_dir_.contains(k)) {
      return false;
    }
    std::ignore = active_file_->append(next_lsn_, EntryType::Delete, key, {});
    ++next_lsn_;

    // As part of the SWMR, the key dir pointer is replaced here to ensure that
    // readers only read written data
    key_dir_ = key_dir_.erase(k);
    active_file_->sync(); // should we sync before or after key_dir_ assignment?
    return true;
  }

  // Returns true if key exists in the index (no disk I/O).
  [[nodiscard]] auto contains_key(BytesView key) const -> bool {
    return key_dir_.contains(to_key(key));
  }

  // ── Batch ──────────────────────────────────────────────────────────────

  // Atomically applies all operations in batch, wrapped in BulkBegin/BulkEnd.
  // batch is consumed (move-only). No-op if batch.empty().
  // Single fdatasync at the end; no per-operation sync.
  // Throws std::system_error on I/O failure.
  void apply_batch(Batch batch) {
    if (batch.empty()) {
      return;
    }

    std::ignore =
        active_file_->append(next_lsn_++, EntryType::BulkBegin, {}, {});

    auto t = key_dir_.transient();
    for (auto &op : batch.operations_) {
      std::visit(
          [&](auto &o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, BatchInsert>) {
              const auto offset = active_file_->append(
                  next_lsn_, EntryType::Put, std::span<const std::byte>{o.key},
                  std::span<const std::byte>{o.value});
              t.set(Key{o.key},
                    KeyDirEntry{next_lsn_, active_file_id_, offset,
                                narrow<std::uint32_t>(o.value.size())});
              ++next_lsn_;
            } else if constexpr (std::is_same_v<T, BatchRemove>) {
              std::ignore =
                  active_file_->append(next_lsn_, EntryType::Delete,
                                       std::span<const std::byte>{o.key}, {});
              ++next_lsn_;
              t.erase(Key{o.key});
            }
          },
          op);
    }

    std::ignore = active_file_->append(next_lsn_++, EntryType::BulkEnd, {}, {});

    // As part of the SWMR, the key dir pointer is replaced here to ensure that
    // readers only read written data
    key_dir_ = std::move(t).persistent();
    active_file_->sync(); // should we sync before or after key_dir_ assignment?
  }

  // ── Range iteration ────────────────────────────────────────────────────

  // Returns an input range of (key, value) pairs with keys >= from.
  // Pass an empty span to start from the first key. Each increment reads one
  // value from disk (lazy). Throws std::system_error on I/O failure.
  [[nodiscard]] auto iter_from(BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
    const auto k = to_key(from);
    const auto it = from.empty() ? key_dir_.begin() : key_dir_.lower_bound(k);
    return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
        EntryIterator{it, key_dir_.end(), &*active_file_},
        std::default_sentinel};
  }

  // Returns an input range of keys >= from. Walks the in-memory key directory
  // only; no disk I/O.
  [[nodiscard]] auto keys_from(BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
    const auto k = to_key(from);
    const auto it = from.empty() ? key_dir_.begin() : key_dir_.lower_bound(k);
    return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
        KeyIterator{it, key_dir_.end()}, std::default_sentinel};
  }

private:
  explicit Bytecask(std::filesystem::path dir) : dir_{std::move(dir)} {
    std::filesystem::create_directories(dir_);
    active_file_id_ = make_data_file_stem();
    active_file_.emplace(dir_ / (active_file_id_ + ".data"));
    next_lsn_ = 1;
  }

  // Converts a BytesView (or empty span) into a Key.
  static auto to_key(BytesView v) -> Key { return Key{v.begin(), v.end()}; }

  std::filesystem::path dir_;
  PersistentOrderedMap<Key, KeyDirEntry> key_dir_;
  std::optional<DataFile> active_file_; // optional so destructor can guard
  std::string active_file_id_;
  std::uint64_t next_lsn_{1};
};

} // namespace bytecask
