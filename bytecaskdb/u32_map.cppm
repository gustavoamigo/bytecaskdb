// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — PersistentU32Map<V> / TransientU32Map<V>
// COW map with uint32_t keys backed by PersistentRadixTree.

module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

export module bytecask.u32_map;

import bytecask.radix_tree;

namespace bytecask {

// Encode a uint32_t as 4 big-endian bytes.
// Preserves numeric ordering through the radix tree's byte-by-byte traversal.
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
// U32MapIterator<V> — decodes uint32_t key from RadixTreeIterator's byte span.
// operator* returns pair<uint32_t, const V&>; the V& references the backing
// tree node directly, valid while the originating U32Map is alive.
// ---------------------------------------------------------------------------
export template <typename V> class U32MapIterator {
public:
  using value_type = std::pair<std::uint32_t, const V &>;
  using difference_type = std::ptrdiff_t;

  U32MapIterator() = default;
  explicit U32MapIterator(RadixTreeIterator<V> inner)
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
  RadixTreeIterator<V> inner_;
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

  explicit PersistentU32Map(PersistentRadixTree<V> tree)
      : tree_{std::move(tree)} {}

  PersistentRadixTree<V> tree_;
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

  explicit TransientU32Map(TransientRadixTree<V> tree)
      : tree_{std::move(tree)} {}

  TransientRadixTree<V> tree_;
};

} // namespace bytecask
