// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — COW maps with uint32_t keys, in two implementations behind
// one interface (PersistentU32MapOf / TransientU32MapOf):
//
// - PersistentU32Map<V> / TransientU32Map<V>: the keyed PersistentBTree.
//   For maps written often and read off the hot path (file_stats).
// - PersistentU32Table<V> / TransientU32Table<V>: a paged direct-addressing
//   table. A lookup is two indexes; a write copies the directory and the
//   pages it touches, and memory follows the pages holding keys. For maps
//   read on every record access and written rarely, with clustered keys
//   (files: ids are minted in sequence and capped at KeyDirEntry::kMaxFileId).
//
// Neither is the key directory's tree: the blind-leaf tree keeps no key bytes
// and reads them back from data files, and a file id is in no data file.

module;
#include <algorithm>
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
// PersistentU32Table<V> / TransientU32Table<V> — paged direct addressing.
//
// Key k lives in page k >> kU32TablePageBits, at slot k % kU32TablePageSize.
// A version is an immutable directory of pages, so a lookup is two indexes.
// Persistent copies share the directory. A transient reads its base directory
// until its first write, which copies the directory (one pointer per page);
// it copies a page the first time it writes to it, and every page it does not
// write stays shared with the version it came from. A transient that only
// reads costs nothing.
//
// A page whose slots are all empty is dropped, and the directory is trimmed
// to its first and last page, so memory follows the pages holding keys, plus
// one directory pointer for each page between them.
// ---------------------------------------------------------------------------
inline constexpr unsigned kU32TablePageBits = 8;
inline constexpr std::size_t kU32TablePageSize = std::size_t{1}
                                                 << kU32TablePageBits;

template <typename V> struct U32TablePage {
  std::size_t used{0}; // slots holding a value; a page at 0 is dropped
  std::array<std::optional<V>, kU32TablePageSize> slots{};
};

template <typename V> struct U32TableDir {
  // pages[i] is page number base + i, null where the page was dropped.
  // Neither end is null: an emptied directory has no pages.
  std::uint32_t base{0};
  std::vector<std::shared_ptr<const U32TablePage<V>>> pages;

  [[nodiscard]] auto page(std::uint32_t p) const noexcept
      -> const U32TablePage<V> * {
    if (p < base) return nullptr;
    const auto i = std::size_t{p - base};
    return i < pages.size() ? pages[i].get() : nullptr;
  }

  [[nodiscard]] auto find(std::uint32_t key) const noexcept -> const V * {
    const auto *pg = page(key >> kU32TablePageBits);
    if (!pg) return nullptr;
    const auto &slot = pg->slots[key & (kU32TablePageSize - 1)];
    return slot ? &*slot : nullptr;
  }
};

export template <typename V> class TransientU32Table;

// Yields pair<uint32_t, const V&> in ascending key order. Holds the directory
// it walks, and so its pages, so it stays valid after the map it came from is
// gone.
export template <typename V> class U32TableIterator {
public:
  using value_type = std::pair<std::uint32_t, const V &>;
  using difference_type = std::ptrdiff_t;

  U32TableIterator() = default;
  explicit U32TableIterator(std::shared_ptr<const U32TableDir<V>> dir)
      : dir_{std::move(dir)} {
    skip_empty();
  }

  auto operator*() const -> std::pair<std::uint32_t, const V &> {
    const auto page_no = dir_->base + static_cast<std::uint32_t>(page_);
    return {(page_no << kU32TablePageBits) | static_cast<std::uint32_t>(slot_),
            *dir_->pages[page_]->slots[slot_]};
  }

  auto operator++() -> U32TableIterator & {
    ++slot_;
    skip_empty();
    return *this;
  }

  auto operator==(const U32TableIterator &other) const noexcept -> bool {
    return at_end() ? other.at_end()
                    : !other.at_end() && dir_ == other.dir_ &&
                          page_ == other.page_ && slot_ == other.slot_;
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return at_end();
  }

private:
  [[nodiscard]] auto at_end() const noexcept -> bool {
    return !dir_ || page_ >= dir_->pages.size();
  }
  void skip_empty() noexcept {
    for (; !at_end(); ++page_, slot_ = 0) {
      const auto *pg = dir_->pages[page_].get();
      if (!pg) continue;
      while (slot_ < kU32TablePageSize && !pg->slots[slot_]) ++slot_;
      if (slot_ < kU32TablePageSize) return;
    }
  }

  std::shared_ptr<const U32TableDir<V>> dir_;
  std::size_t page_{0};
  std::size_t slot_{0};
};

export template <typename V> class PersistentU32Table {
public:
  PersistentU32Table() = default;

  // Valid for the lifetime of this map instance.
  [[nodiscard]] auto get(std::uint32_t key) const noexcept -> const V * {
    return dir_ ? dir_->find(key) : nullptr;
  }

  [[nodiscard]] auto contains(std::uint32_t key) const noexcept -> bool {
    return get(key) != nullptr;
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    return !dir_ || dir_->pages.empty();
  }

  // Pages this version holds: its memory, in units of a page.
  [[nodiscard]] auto pages() const noexcept -> std::size_t {
    if (!dir_) return 0;
    return static_cast<std::size_t>(std::ranges::count_if(
        dir_->pages, [](const auto &pg) { return pg != nullptr; }));
  }

  // O(1): shares this version's directory until the transient's first write.
  [[nodiscard]] auto transient() const -> TransientU32Table<V> {
    return TransientU32Table<V>{dir_};
  }

  [[nodiscard]] auto begin() const -> U32TableIterator<V> {
    return U32TableIterator<V>{dir_};
  }

  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

private:
  friend class TransientU32Table<V>;

  explicit PersistentU32Table(std::shared_ptr<const U32TableDir<V>> dir)
      : dir_{std::move(dir)} {}

  std::shared_ptr<const U32TableDir<V>> dir_;
};

export template <typename V> class TransientU32Table {
public:
  TransientU32Table(const TransientU32Table &) = delete;
  auto operator=(const TransientU32Table &) -> TransientU32Table & = delete;
  TransientU32Table(TransientU32Table &&) noexcept = default;
  auto operator=(TransientU32Table &&) noexcept -> TransientU32Table & = default;

  // Safe for immediate use; do not retain across set()/erase() calls.
  [[nodiscard]] auto get(std::uint32_t key) const noexcept -> const V * {
    const auto *d = view();
    return d ? d->find(key) : nullptr;
  }

  [[nodiscard]] auto contains(std::uint32_t key) const noexcept -> bool {
    return get(key) != nullptr;
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    const auto *d = view();
    return !d || d->pages.empty();
  }

  void set(std::uint32_t key, V value) {
    auto &pg = writable_page(key >> kU32TablePageBits);
    auto &slot = pg.slots[key & (kU32TablePageSize - 1)];
    if (!slot) ++pg.used;
    slot = std::move(value);
  }

  auto erase(std::uint32_t key) -> bool {
    if (!contains(key)) return false;
    const auto p = key >> kU32TablePageBits;
    auto &pg = writable_page(p);
    pg.slots[key & (kU32TablePageSize - 1)].reset();
    if (--pg.used == 0) drop_page(p);
    return true;
  }

  // Read-modify-write: calls func(V&) on the existing value. No-op if absent.
  template <typename Func> void update(std::uint32_t key, Func &&func) {
    if (!contains(key)) return;
    auto &pg = writable_page(key >> kU32TablePageBits);
    std::forward<Func>(func)(*pg.slots[key & (kU32TablePageSize - 1)]);
  }

  // Freeze and consume; produces an immutable snapshot.
  [[nodiscard]] auto persistent() && -> PersistentU32Table<V> {
    owned_.clear();
    if (own_) return PersistentU32Table<V>{std::move(own_)};
    return PersistentU32Table<V>{std::move(base_)};
  }

private:
  friend class PersistentU32Table<V>;

  explicit TransientU32Table(std::shared_ptr<const U32TableDir<V>> base)
      : base_{std::move(base)} {}

  [[nodiscard]] auto view() const noexcept -> const U32TableDir<V> * {
    return own_ ? own_.get() : base_.get();
  }

  // The page this transient writes to: a copy of the shared page, or a new
  // one, made once and listed in owned_. Pages not in owned_ are shared with
  // other versions and never written.
  auto writable_page(std::uint32_t p) -> U32TablePage<V> & {
    for (const auto &[no, pg] : owned_)
      if (no == p) return *pg;
    const auto *d = view();
    const auto *shared = d ? d->page(p) : nullptr;
    auto pg = shared ? std::make_shared<U32TablePage<V>>(*shared)
                     : std::make_shared<U32TablePage<V>>();
    owned_.reserve(owned_.size() + 1);
    if (!own_)
      own_ = base_ ? std::make_unique<U32TableDir<V>>(*base_)
                   : std::make_unique<U32TableDir<V>>();
    auto &dir = *own_;
    if (dir.pages.empty()) {
      dir.base = p;
    } else if (p < dir.base) {
      dir.pages.insert(dir.pages.begin(), std::size_t{dir.base - p}, nullptr);
      dir.base = p;
    }
    const auto i = std::size_t{p - dir.base};
    if (i >= dir.pages.size()) dir.pages.resize(i + 1);
    dir.pages[i] = pg;
    owned_.emplace_back(p, pg);
    return *pg;
  }

  // Drops an emptied page this transient owns and trims the directory.
  void drop_page(std::uint32_t p) {
    std::erase_if(owned_, [p](const auto &e) { return e.first == p; });
    auto &dir = *own_;
    dir.pages[std::size_t{p - dir.base}].reset();
    while (!dir.pages.empty() && !dir.pages.back()) dir.pages.pop_back();
    std::size_t lead = 0;
    while (lead < dir.pages.size() && !dir.pages[lead]) ++lead;
    dir.pages.erase(dir.pages.begin(),
                    dir.pages.begin() + static_cast<std::ptrdiff_t>(lead));
    dir.base = dir.pages.empty() ? 0 : dir.base + static_cast<std::uint32_t>(lead);
  }

  std::shared_ptr<const U32TableDir<V>> base_;
  std::unique_ptr<U32TableDir<V>> own_;
  std::vector<std::pair<std::uint32_t, std::shared_ptr<U32TablePage<V>>>>
      owned_;
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

// A callable for update() in the concept below. A named type, not a lambda:
// Homebrew clang 23 crashed compiling this module interface while the
// concept held a lambda (#343).
template <typename V> struct U32MapNoOpUpdate {
  void operator()(V &) const noexcept {}
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
      t.update(k, U32MapNoOpUpdate<V>{});
      std::move(t).persistent();
    };

} // namespace bytecask
