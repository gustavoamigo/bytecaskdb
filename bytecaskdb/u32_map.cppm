// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — COW maps with uint32_t keys, in two implementations behind
// one interface (PersistentU32MapOf / TransientU32MapOf):
//
// - PersistentU32Map<V> / TransientU32Map<V>: the keyed PersistentBTree.
//   For maps written often and read off the hot path (file_stats).
// - PersistentU32Table<V> / TransientU32Table<V>: a direct-addressing table,
//   one slot per key between the lowest and highest key held. A lookup is an
//   index; a write copies the table. For maps read on every record access and
//   written rarely, with dense keys (files: ids are minted in sequence and
//   capped at KeyDirEntry::kMaxFileId).
//
// Neither is the key directory's tree: the blind-leaf tree keeps no key bytes
// and reads them back from data files, and a file id is in no data file.

module;
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

export module bytecask.u32_map;

import bytecask.btree;

namespace bytecask {

// Encode a uint32_t as 4 big-endian bytes.
// Byte order is numeric order, so the tree iterates in ascending key order.
auto encode_key(std::uint32_t k) noexcept -> std::array<std::byte, 4> {
  return {
      static_cast<std::byte>((k >> 24) & 0xFF),
      static_cast<std::byte>((k >> 16) & 0xFF),
      static_cast<std::byte>((k >> 8) & 0xFF),
      static_cast<std::byte>(k & 0xFF),
  };
}

auto decode_key(std::span<const std::byte> b) noexcept -> std::uint32_t {
  return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(b[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(b[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(b[2])) << 8) |
          static_cast<std::uint32_t>(std::to_integer<unsigned char>(b[3]));
}

export template <typename V> class TransientU32Map;

// ---------------------------------------------------------------------------
// U32MapIterator<V> — decodes uint32_t key from BTreeIterator's byte span.
// operator* returns pair<uint32_t, const V&>; the V& references the backing
// tree node directly, valid while the originating U32Map is alive.
// ---------------------------------------------------------------------------
export template <typename V> class U32MapIterator {
public:
  using value_type = std::pair<std::uint32_t, const V &>;
  using difference_type = std::ptrdiff_t;

  U32MapIterator() = default;
  explicit U32MapIterator(BTreeIterator<V> inner)
      : inner_{std::move(inner)} {}

  auto operator*() const -> std::pair<std::uint32_t, const V &> {
    const auto &[key_bytes, val] = *inner_;
    return {decode_key(key_bytes), val};
  }

  auto operator++() -> U32MapIterator & {
    ++inner_;
    return *this;
  }

  auto operator==(const U32MapIterator &other) const noexcept -> bool {
    return inner_ == other.inner_;
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return inner_ == std::default_sentinel;
  }

private:
  BTreeIterator<V> inner_;
};

// ---------------------------------------------------------------------------
// PersistentU32Map<V> — immutable COW map with uint32_t keys.
// O(1) snapshot (copy) via structural sharing.
// ---------------------------------------------------------------------------
export template <typename V> class PersistentU32Map {
public:
  PersistentU32Map() = default;

  // Returns a pointer to the stored value, or nullptr if absent.
  // Valid for the lifetime of this map instance; do not retain across
  // structural mutations on any transient derived from this snapshot.
  [[nodiscard]] auto get(std::uint32_t key) const noexcept -> const V * {
    const auto encoded = encode_key(key);
    return tree_.get_ptr(std::span<const std::byte>{encoded});
  }

  [[nodiscard]] auto contains(std::uint32_t key) const noexcept -> bool {
    return get(key) != nullptr;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return tree_.empty(); }

  // O(1) COW fork — shares structure with this snapshot.
  [[nodiscard]] auto transient() const -> TransientU32Map<V> {
    return TransientU32Map<V>{tree_.transient()};
  }

  [[nodiscard]] auto begin() const -> U32MapIterator<V> {
    return U32MapIterator<V>{tree_.begin()};
  }

  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

private:
  friend class TransientU32Map<V>;

  explicit PersistentU32Map(PersistentBTree<V> tree)
      : tree_{std::move(tree)} {}

  PersistentBTree<V> tree_;
};

// ---------------------------------------------------------------------------
// TransientU32Map<V> — mutable working copy.
// Produced by PersistentU32Map::transient(); frozen by persistent() &&.
// ---------------------------------------------------------------------------
export template <typename V> class TransientU32Map {
public:
  TransientU32Map(const TransientU32Map &) = delete;
  auto operator=(const TransientU32Map &) -> TransientU32Map & = delete;
  TransientU32Map(TransientU32Map &&) noexcept = default;
  auto operator=(TransientU32Map &&) noexcept -> TransientU32Map & = default;

  // Returns a pointer to the stored value, or nullptr if absent.
  // Safe for immediate use; do not retain across set()/erase() calls.
  [[nodiscard]] auto get(std::uint32_t key) const noexcept -> const V * {
    const auto encoded = encode_key(key);
    return tree_.get_ptr(std::span<const std::byte>{encoded});
  }

  [[nodiscard]] auto contains(std::uint32_t key) const noexcept -> bool {
    return get(key) != nullptr;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return tree_.empty(); }

  void set(std::uint32_t key, V value) {
    const auto encoded = encode_key(key);
    tree_.set(std::span<const std::byte>{encoded}, std::move(value));
  }

  auto erase(std::uint32_t key) -> bool {
    const auto encoded = encode_key(key);
    return tree_.erase(std::span<const std::byte>{encoded});
  }

  // Read-modify-write: calls func(V&) on the existing value. No-op if absent.
  template <typename Func> void update(std::uint32_t key, Func &&func) {
    const auto encoded = encode_key(key);
    std::span<const std::byte> key_span{encoded};
    auto current = tree_.get(key_span);
    if (!current) return;
    std::forward<Func>(func)(*current);
    tree_.set(key_span, std::move(*current));
  }

  // Freeze and consume; produces an immutable snapshot.
  [[nodiscard]] auto persistent() && -> PersistentU32Map<V> {
    return PersistentU32Map<V>{std::move(tree_).persistent()};
  }

private:
  friend class PersistentU32Map<V>;

  explicit TransientU32Map(TransientBTree<V> tree)
      : tree_{std::move(tree)} {}

  TransientBTree<V> tree_;
};

// ---------------------------------------------------------------------------
// PersistentU32Table<V> / TransientU32Table<V> — direct addressing.
//
// A version is an immutable block: slots[k - base] holds key k. Persistent
// copies share the block; a transient reads its base block until its first
// write, which copies it, so a transient that only reads costs nothing.
// Erasing the lowest or highest key trims the block, so it spans the keys
// held, not every key ever set.
// ---------------------------------------------------------------------------
template <typename V> struct U32TableBlock {
  std::uint32_t base{0};
  std::vector<std::optional<V>> slots;

  [[nodiscard]] auto find(std::uint32_t key) const noexcept -> const V * {
    if (key < base) return nullptr;
    const auto i = std::size_t{key - base};
    if (i >= slots.size() || !slots[i]) return nullptr;
    return &*slots[i];
  }
};

export template <typename V> class TransientU32Table;

// Yields pair<uint32_t, const V&> in ascending key order. Holds the block it
// walks, so it stays valid after the map it came from is gone.
export template <typename V> class U32TableIterator {
public:
  using value_type = std::pair<std::uint32_t, const V &>;
  using difference_type = std::ptrdiff_t;

  U32TableIterator() = default;
  explicit U32TableIterator(std::shared_ptr<const U32TableBlock<V>> block)
      : block_{std::move(block)} {
    skip_empty();
  }

  auto operator*() const -> std::pair<std::uint32_t, const V &> {
    return {block_->base + static_cast<std::uint32_t>(i_), *block_->slots[i_]};
  }

  auto operator++() -> U32TableIterator & {
    ++i_;
    skip_empty();
    return *this;
  }

  auto operator==(const U32TableIterator &other) const noexcept -> bool {
    return at_end() ? other.at_end()
                    : !other.at_end() && block_ == other.block_ &&
                          i_ == other.i_;
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return at_end();
  }

private:
  [[nodiscard]] auto at_end() const noexcept -> bool {
    return !block_ || i_ >= block_->slots.size();
  }
  void skip_empty() noexcept {
    while (!at_end() && !block_->slots[i_]) ++i_;
  }

  std::shared_ptr<const U32TableBlock<V>> block_;
  std::size_t i_{0};
};

export template <typename V> class PersistentU32Table {
public:
  PersistentU32Table() = default;

  // Valid for the lifetime of this map instance.
  [[nodiscard]] auto get(std::uint32_t key) const noexcept -> const V * {
    return block_ ? block_->find(key) : nullptr;
  }

  [[nodiscard]] auto contains(std::uint32_t key) const noexcept -> bool {
    return get(key) != nullptr;
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    return !block_ || block_->slots.empty();
  }

  // O(1): shares this version's block until the transient's first write.
  [[nodiscard]] auto transient() const -> TransientU32Table<V> {
    return TransientU32Table<V>{block_};
  }

  [[nodiscard]] auto begin() const -> U32TableIterator<V> {
    return U32TableIterator<V>{block_};
  }

  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

private:
  friend class TransientU32Table<V>;

  explicit PersistentU32Table(std::shared_ptr<const U32TableBlock<V>> block)
      : block_{std::move(block)} {}

  std::shared_ptr<const U32TableBlock<V>> block_;
};

export template <typename V> class TransientU32Table {
public:
  TransientU32Table(const TransientU32Table &) = delete;
  auto operator=(const TransientU32Table &) -> TransientU32Table & = delete;
  TransientU32Table(TransientU32Table &&) noexcept = default;
  auto operator=(TransientU32Table &&) noexcept -> TransientU32Table & = default;

  // Safe for immediate use; do not retain across set()/erase() calls.
  [[nodiscard]] auto get(std::uint32_t key) const noexcept -> const V * {
    const auto *b = view();
    return b ? b->find(key) : nullptr;
  }

  [[nodiscard]] auto contains(std::uint32_t key) const noexcept -> bool {
    return get(key) != nullptr;
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    const auto *b = view();
    return !b || b->slots.empty();
  }

  void set(std::uint32_t key, V value) {
    auto &b = writable();
    if (b.slots.empty()) {
      b.base = key;
    } else if (key < b.base) {
      b.slots.insert(b.slots.begin(), std::size_t{b.base - key},
                     std::nullopt);
      b.base = key;
    }
    const auto i = std::size_t{key - b.base};
    if (i >= b.slots.size()) b.slots.resize(i + 1);
    b.slots[i] = std::move(value);
  }

  auto erase(std::uint32_t key) -> bool {
    if (!contains(key)) return false;
    auto &b = writable();
    b.slots[std::size_t{key - b.base}].reset();
    while (!b.slots.empty() && !b.slots.back()) b.slots.pop_back();
    std::size_t lead = 0;
    while (lead < b.slots.size() && !b.slots[lead]) ++lead;
    b.slots.erase(b.slots.begin(),
                  b.slots.begin() + static_cast<std::ptrdiff_t>(lead));
    b.base = b.slots.empty() ? 0 : b.base + static_cast<std::uint32_t>(lead);
    return true;
  }

  // Read-modify-write: calls func(V&) on the existing value. No-op if absent.
  template <typename Func> void update(std::uint32_t key, Func &&func) {
    if (!contains(key)) return;
    auto &b = writable();
    std::forward<Func>(func)(*b.slots[std::size_t{key - b.base}]);
  }

  // Freeze and consume; produces an immutable snapshot.
  [[nodiscard]] auto persistent() && -> PersistentU32Table<V> {
    if (own_) return PersistentU32Table<V>{std::move(own_)};
    return PersistentU32Table<V>{std::move(base_)};
  }

private:
  friend class PersistentU32Table<V>;

  explicit TransientU32Table(std::shared_ptr<const U32TableBlock<V>> base)
      : base_{std::move(base)} {}

  [[nodiscard]] auto view() const noexcept -> const U32TableBlock<V> * {
    return own_ ? own_.get() : base_.get();
  }

  // The block this transient writes to: a copy of the base, made once.
  auto writable() -> U32TableBlock<V> & {
    if (!own_)
      own_ = base_ ? std::make_unique<U32TableBlock<V>>(*base_)
                   : std::make_unique<U32TableBlock<V>>();
    return *own_;
  }

  std::shared_ptr<const U32TableBlock<V>> base_;
  std::unique_ptr<U32TableBlock<V>> own_;
};

// ---------------------------------------------------------------------------
// The interface both implementations provide; the engine relies on no more.
// ---------------------------------------------------------------------------
export template <typename M, typename V>
concept PersistentU32MapOf = requires(const M m, std::uint32_t k) {
  { m.get(k) } noexcept -> std::same_as<const V *>;
  { m.contains(k) } noexcept -> std::same_as<bool>;
  { m.empty() } noexcept -> std::same_as<bool>;
  m.transient();
  { *m.begin() } -> std::same_as<std::pair<std::uint32_t, const V &>>;
  { m.begin() == m.end() } -> std::same_as<bool>;
};

export template <typename T, typename V>
concept TransientU32MapOf =
    std::movable<T> &&
    requires(T t, const T ct, std::uint32_t k, V v) {
      { ct.get(k) } noexcept -> std::same_as<const V *>;
      { ct.contains(k) } noexcept -> std::same_as<bool>;
      { ct.empty() } noexcept -> std::same_as<bool>;
      t.set(k, std::move(v));
      { t.erase(k) } -> std::same_as<bool>;
      t.update(k, [](V &) {});
      std::move(t).persistent();
    };

static_assert(PersistentU32MapOf<PersistentU32Map<int>, int>);
static_assert(PersistentU32MapOf<PersistentU32Table<int>, int>);
static_assert(TransientU32MapOf<TransientU32Map<int>, int>);
static_assert(TransientU32MapOf<TransientU32Table<int>, int>);

} // namespace bytecask
