// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — persistent B+ tree with byte-string keys.
//
// One node shape: a slotted page with a shared key prefix, a sorted slot
// array (key head in the high half, entry offset in the low half) and an
// entry heap growing down from the end. A leaf entry is V + key suffix; an
// inner entry is Node* + separator suffix. Versions share structure by path
// copying; a transient edits the nodes it created in place; node lifetime
// belongs to VersionChain. See docs/persistent_btree_design.md.

module;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bytecask.btree;
import bytecask.version_chain;

namespace bytecask {

export inline constexpr std::size_t kBTreeLeafBytes = 4096;
export inline constexpr std::size_t kBTreeInnerBytes = 4096;
// Entry lengths are 16-bit; the data file's key_size field has the same
// ceiling, so no legal key or separator exceeds it.
export inline constexpr std::size_t kBTreeMaxKeyBytes = 65535;

export template <typename V> class PersistentBTree;
export template <typename V> class TransientBTree;
export template <typename V, bool WithKey> class BasicBTreeIterator;
export template <typename V, bool WithKey> class BasicReverseBTreeIterator;

namespace btree_detail {

using Bytes = std::span<const std::byte>;

// Test-only node accounting: allocated − freed == reachable + parked.
export template <typename V> struct BTreeAccounting {
  std::atomic<std::int64_t> allocated{0};
  std::atomic<std::int64_t> freed{0};
  std::atomic<std::int64_t> retired{0};
};
export template <typename V> auto btree_accounting() -> BTreeAccounting<V> & {
  static BTreeAccounting<V> acc;
  return acc;
}
#ifdef BYTECASK_TESTING
template <typename V> void account_alloc() noexcept {
  btree_accounting<V>().allocated.fetch_add(1, std::memory_order_relaxed);
}
template <typename V> void account_free() noexcept {
  btree_accounting<V>().freed.fetch_add(1, std::memory_order_relaxed);
}
template <typename V> void account_retired(std::int64_t delta) noexcept {
  btree_accounting<V>().retired.fetch_add(delta, std::memory_order_relaxed);
}
#else
template <typename V> void account_alloc() noexcept {}
template <typename V> void account_free() noexcept {}
template <typename V> void account_retired(std::int64_t) noexcept {}
#endif

constexpr auto align_up(std::size_t n, std::size_t a) noexcept -> std::size_t {
  return (n + a - 1) / a * a;
}

template <typename T> auto as_ptr(std::byte *p) noexcept -> T * {
  return static_cast<T *>(static_cast<void *>(p));
}
template <typename T> auto as_ptr(const std::byte *p) noexcept -> const T * {
  return static_cast<const T *>(static_cast<const void *>(p));
}

inline auto compare_bytes(Bytes a, Bytes b) noexcept -> int {
  const auto n = std::min(a.size(), b.size());
  const int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (c != 0)
    return c;
  if (a.size() == b.size())
    return 0;
  return a.size() < b.size() ? -1 : 1;
}

inline auto common_prefix_length(Bytes a, Bytes b) noexcept -> std::size_t {
  const auto n = std::min(a.size(), b.size());
  std::size_t i = 0;
  while (i < n && a[i] == b[i])
    ++i;
  return i;
}

// First four suffix bytes as a big-endian integer, zero-padded. For two
// suffixes, head(a) < head(b) implies a < b and head(a) > head(b) implies
// a > b; equal heads decide nothing.
inline auto head_of(Bytes suffix) noexcept -> std::uint32_t {
  std::uint32_t h = 0;
  const auto n = std::min<std::size_t>(4, suffix.size());
  for (std::size_t i = 0; i < n; ++i)
    h |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(suffix[i]))
         << (24 - 8 * i);
  return h;
}

// A full key as two pieces — a node prefix and an entry suffix, or a flat
// key and nothing — so keys are compared and copied without concatenation.
struct KeyParts {
  Bytes a;
  Bytes b;

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return a.size() + b.size();
  }
  [[nodiscard]] auto at(std::size_t i) const noexcept -> std::byte {
    return i < a.size() ? a[i] : b[i - a.size()];
  }
  // Copies bytes [from, size()) to dst. memmove: dst may overlap a source.
  void copy_tail(std::size_t from, std::byte *dst) const noexcept {
    if (from < a.size()) {
      std::memmove(dst, a.data() + from, a.size() - from);
      dst += a.size() - from;
      from = 0;
    } else {
      from -= a.size();
    }
    if (from < b.size())
      std::memmove(dst, b.data() + from, b.size() - from);
  }
};

inline auto common_prefix_length(const KeyParts &x,
                                 const KeyParts &y) noexcept -> std::size_t {
  const auto n = std::min(x.size(), y.size());
  std::size_t i = 0;
  while (i < n && x.at(i) == y.at(i))
    ++i;
  return i;
}

// ---------------------------------------------------------------------------
// Node<V> — the header of every node; the rest of the allocation follows it.
//
//   [header][prefix bytes][slots: count × u64 ->]  ...free...  [<- entries]
//
//   slot   = head << 32 | off        off: distance from the node end to the
//                                    entry start (a multiple of kAlign)
//   entry  = [payload][u16 len][suffix bytes]   payload: V (leaf) or Node*
//
// Nodes are immutable once their version is published; only the session
// whose tag they carry writes to them.
// ---------------------------------------------------------------------------
template <typename V> struct Node {
  std::uint64_t tag{0};        // creating session: ownership and reclamation
  Node *first_child{nullptr};  // inner only: child left of every separator
  std::uint32_t capacity{0};   // bytes allocated
  std::uint32_t heap_floor{0}; // distance from the node end to the lowest entry
  std::uint32_t dead_bytes{0}; // erased entries still occupying the heap
  std::uint32_t count{0};      // entries
  std::uint16_t prefix_len{0}; // bytes every key in the node shares
  std::uint8_t is_leaf{1};

  static constexpr std::size_t kAlign =
      std::max({alignof(V), alignof(void *), std::size_t{8}});
  static constexpr std::size_t kLenBytes = 2;
  static constexpr std::size_t kSlotBytes = 8;

  [[nodiscard]] static auto header_bytes() noexcept -> std::size_t {
    return align_up(sizeof(Node), 8);
  }
  [[nodiscard]] static auto slots_offset_for(std::size_t prefix) noexcept
      -> std::size_t {
    return align_up(header_bytes() + prefix, kSlotBytes);
  }
  [[nodiscard]] static auto payload_size(bool leaf) noexcept -> std::size_t {
    return leaf ? sizeof(V) : sizeof(Node *);
  }
  [[nodiscard]] static auto entry_size(bool leaf, std::size_t len) noexcept
      -> std::size_t {
    return align_up(payload_size(leaf) + kLenBytes + len, kAlign);
  }
  [[nodiscard]] static auto node_bytes(bool leaf) noexcept -> std::size_t {
    return leaf ? kBTreeLeafBytes : kBTreeInnerBytes;
  }

  // Allocates a node of at least `min_capacity` bytes with `prefix` stored
  // and no entries. Storage comes from the global operator new so the
  // memory tests see it.
  [[nodiscard]] static auto allocate(std::size_t min_capacity, bool leaf,
                                     std::uint64_t session_tag, Bytes pre)
      -> Node * {
    const auto needed = slots_offset_for(pre.size());
    const auto capacity = align_up(std::max(min_capacity, needed), 16);
    auto *mem = static_cast<std::byte *>(::operator new(capacity));
    auto *n = new (mem) Node{};
    n->tag = session_tag;
    n->capacity = static_cast<std::uint32_t>(capacity);
    n->prefix_len = static_cast<std::uint16_t>(pre.size());
    n->is_leaf = leaf ? 1 : 0;
    if (!pre.empty())
      std::memcpy(mem + header_bytes(), pre.data(), pre.size());
    account_alloc<V>();
    return n;
  }

  // Frees one node and, for a leaf, the values it holds. Children are not
  // touched: their lifetime is the chain's business.
  static void destroy(Node *n) noexcept {
    if constexpr (!std::is_trivially_destructible_v<V>) {
      if (n->is_leaf) {
        for (std::uint32_t i = 0; i < n->count; ++i)
          std::destroy_at(n->template payload<V>(i));
      }
    }
    n->~Node();
    ::operator delete(static_cast<void *>(n));
    account_free<V>();
  }

  [[nodiscard]] auto bytes() noexcept -> std::byte * {
    return static_cast<std::byte *>(static_cast<void *>(this));
  }
  [[nodiscard]] auto bytes() const noexcept -> const std::byte * {
    return static_cast<const std::byte *>(static_cast<const void *>(this));
  }
  [[nodiscard]] auto prefix() const noexcept -> Bytes {
    return {bytes() + header_bytes(), prefix_len};
  }
  [[nodiscard]] auto slots() noexcept -> std::uint64_t * {
    return as_ptr<std::uint64_t>(bytes() + slots_offset_for(prefix_len));
  }
  [[nodiscard]] auto slots() const noexcept -> const std::uint64_t * {
    return as_ptr<std::uint64_t>(bytes() + slots_offset_for(prefix_len));
  }
  [[nodiscard]] static auto slot_head(std::uint64_t slot) noexcept
      -> std::uint32_t {
    return static_cast<std::uint32_t>(slot >> 32);
  }
  [[nodiscard]] static auto slot_off(std::uint64_t slot) noexcept
      -> std::uint32_t {
    return static_cast<std::uint32_t>(slot & 0xFFFF'FFFFu);
  }
  [[nodiscard]] auto entry(std::uint32_t i) noexcept -> std::byte * {
    return bytes() + capacity - slot_off(slots()[i]);
  }
  [[nodiscard]] auto entry(std::uint32_t i) const noexcept
      -> const std::byte * {
    return bytes() + capacity - slot_off(slots()[i]);
  }
  [[nodiscard]] auto entry_len(const std::byte *e) const noexcept
      -> std::size_t {
    std::uint16_t len = 0;
    std::memcpy(&len, e + payload_size(is_leaf), kLenBytes);
    return len;
  }
  [[nodiscard]] auto suffix(std::uint32_t i) const noexcept -> Bytes {
    const auto *e = entry(i);
    return {e + payload_size(is_leaf) + kLenBytes, entry_len(e)};
  }
  [[nodiscard]] auto key(std::uint32_t i) const noexcept -> KeyParts {
    return {prefix(), suffix(i)};
  }
  template <typename P>
  [[nodiscard]] auto payload(std::uint32_t i) noexcept -> P * {
    return std::launder(as_ptr<P>(entry(i)));
  }
  template <typename P>
  [[nodiscard]] auto payload(std::uint32_t i) const noexcept -> const P * {
    return std::launder(as_ptr<P>(entry(i)));
  }
  [[nodiscard]] auto payload_raw(std::uint32_t i) const noexcept
      -> const std::byte * {
    return entry(i);
  }

  // Children of an inner node: first_child, then one per entry.
  [[nodiscard]] auto child(std::uint32_t i) const noexcept -> Node * {
    return i == 0 ? first_child : *payload<Node *>(i - 1);
  }
  void set_child(std::uint32_t i, Node *c) noexcept {
    if (i == 0)
      first_child = c;
    else
      *payload<Node *>(i - 1) = c;
  }

  [[nodiscard]] auto free_bytes() const noexcept -> std::size_t {
    return capacity - heap_floor - slots_offset_for(prefix_len) -
           count * kSlotBytes;
  }
  // Bytes a compacted copy with `prefix` would need, before any new entry.
  [[nodiscard]] auto packed_bytes(std::size_t prefix) const noexcept
      -> std::size_t {
    return slots_offset_for(prefix) + count * kSlotBytes + heap_floor -
           dead_bytes;
  }

  struct Pos {
    std::uint32_t idx;
    bool exact;
  };

  // Position of the first entry whose full key is >= `key`, and whether it
  // is equal. A key outside the node prefix sorts before or after every
  // entry, so idx is 0 or count.
  [[nodiscard]] auto search(Bytes key) const noexcept -> Pos {
    const auto pre = prefix();
    const auto cpl = common_prefix_length(pre, key);
    if (cpl < pre.size()) {
      if (cpl == key.size() || key[cpl] < pre[cpl])
        return {0, false};
      return {count, false};
    }
    const auto suf = key.subspan(pre.size());
    const auto head = head_of(suf);
    const auto target = std::uint64_t{head} << 32;
    const auto *s = slots();
    std::uint32_t pos = 0;
    for (std::uint32_t i = 0; i < count; ++i)
      pos += s[i] < target ? 1u : 0u;
    for (; pos < count && slot_head(s[pos]) == head; ++pos) {
      const auto c = compare_bytes(suffix(pos), suf);
      if (c >= 0)
        return {pos, c == 0};
    }
    return {pos, false};
  }
  [[nodiscard]] auto child_index(Bytes key) const noexcept -> std::uint32_t {
    const auto p = search(key);
    return p.exact ? p.idx + 1 : p.idx;
  }

  // Writes an entry into the heap and its slot at `pos`. The caller checked
  // that entry_size + kSlotBytes fits in free_bytes().
  template <typename P>
  void insert_entry(std::uint32_t pos, Bytes suf, const P &payload_src) {
    const auto size = entry_size(is_leaf, suf.size());
    const auto off = heap_floor + size;
    heap_floor = static_cast<std::uint32_t>(off);
    auto *e = bytes() + capacity - off;
    std::construct_at(as_ptr<P>(e), payload_src);
    const auto len = static_cast<std::uint16_t>(suf.size());
    std::memcpy(e + payload_size(is_leaf), &len, kLenBytes);
    if (!suf.empty())
      std::memcpy(e + payload_size(is_leaf) + kLenBytes, suf.data(),
                  suf.size());
    auto *s = slots();
    std::memmove(s + pos + 1, s + pos, (count - pos) * kSlotBytes);
    s[pos] = (std::uint64_t{head_of(suf)} << 32) | off;
    ++count;
  }

  void remove_entry(std::uint32_t pos) noexcept {
    auto *e = entry(pos);
    if constexpr (!std::is_trivially_destructible_v<V>) {
      if (is_leaf)
        std::destroy_at(std::launder(as_ptr<V>(e)));
    }
    dead_bytes +=
        static_cast<std::uint32_t>(entry_size(is_leaf, entry_len(e)));
    auto *s = slots();
    std::memmove(s + pos, s + pos + 1, (count - pos - 1) * kSlotBytes);
    --count;
  }

  template <typename F> void for_each_child(F &&f) const {
    if (is_leaf)
      return;
    f(first_child);
    for (std::uint32_t i = 0; i < count; ++i)
      f(*payload<Node *>(i));
  }
};

template <typename V> struct ChainTraits {
  using Node = btree_detail::Node<V>;
  static auto tag(const Node *n) noexcept -> std::uint64_t { return n->tag; }
  template <typename F> static void for_each_child(Node *n, F &&f) {
    n->for_each_child(std::forward<F>(f));
  }
  static void destroy(Node *n) noexcept { Node::destroy(n); }
  static void account_retired(std::int64_t delta) noexcept {
    btree_detail::account_retired<V>(delta);
  }
};

// Lookup shared by the persistent and transient trees. Raw pointers: the
// caller's handle or session keeps the nodes alive.
template <typename V>
auto find_ptr(const Node<V> *cur, Bytes key) noexcept -> const V * {
  while (cur) {
    if (cur->is_leaf) {
      const auto p = cur->search(key);
      return p.exact ? cur->template payload<V>(p.idx) : nullptr;
    }
    cur = cur->child(cur->child_index(key));
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// BuildSession<V> — one build of a new version from a base.
//
// Owns the tag that marks the nodes it creates and the list of base nodes it
// retires. A node the session owns (tag == tag_) is edited in place and
// freed at once when replaced; a foreign node is copied and the original
// retired. The three places a node becomes garbage — superseded by a copy,
// replaced by a rebuild or split, or never published — all go through
// discard(). Used by one thread at a time.
// ---------------------------------------------------------------------------
template <typename V> class BuildSession {
public:
  using N = Node<V>;

  struct Result {
    N *node{nullptr};  // the subtree root, or null if it emptied
    N *right{nullptr}; // second node if the subtree split; separator in sep_
    bool changed{false};
    bool inserted{false};
  };

  BuildSession() : tag_{new_version_tag()} {}
  BuildSession(const BuildSession &) = delete;
  auto operator=(const BuildSession &) -> BuildSession & = delete;
  BuildSession(BuildSession &&other) noexcept
      : tag_{std::exchange(other.tag_, 0)}, retired_{std::move(other.retired_)},
        sep_{std::move(other.sep_)}, displaced_{std::move(other.displaced_)} {
    other.retired_.clear();
  }
  auto operator=(BuildSession &&other) noexcept -> BuildSession & {
    if (this != &other) {
      forget_retired();
      tag_ = std::exchange(other.tag_, 0);
      retired_ = std::move(other.retired_);
      other.retired_.clear();
      sep_ = std::move(other.sep_);
      displaced_ = std::move(other.displaced_);
    }
    return *this;
  }
  ~BuildSession() { forget_retired(); }

  [[nodiscard]] auto tag() const noexcept -> std::uint64_t { return tag_; }
  [[nodiscard]] auto owns(const N *n) const noexcept -> bool {
    return n->tag == tag_;
  }
  [[nodiscard]] auto retired_list() noexcept -> std::vector<N *> & {
    return retired_;
  }
  [[nodiscard]] auto separator() const noexcept -> Bytes { return sep_; }
  [[nodiscard]] auto take_displaced() noexcept -> std::optional<V> {
    return std::exchange(displaced_, std::nullopt);
  }

  // Ends the session after publishing: the nodes it created belong to the
  // version now, and the retired list to the chain.
  void finish() noexcept {
    tag_ = 0;
    retired_.clear();
  }

  // For a session dropped without publishing: frees every node it created
  // that is reachable from `root` and forgets the retired list. The base is
  // intact, so nothing retired is garbage.
  void discard_all(N *root) noexcept {
    const auto tag = tag_;
    if (tag != 0)
      free_subtree_if<ChainTraits<V>>(root,
                                      [tag](N *n) { return n->tag == tag; });
    forget_retired();
    tag_ = 0;
  }

  // The one place a node becomes garbage.
  void discard(N *n) {
    if (owns(n)) {
      N::destroy(n);
      return;
    }
    retired_.push_back(n);
    account_retired<V>(1);
  }

  // Insert or replace. `should_replace(existing, incoming)` is asked on an
  // existing key; a refused replacement changes nothing. The displaced
  // value, if any, is kept for take_displaced().
  template <typename Pred>
  auto upsert(N *root, Bytes key, const V &val, Pred &&should_replace)
      -> Result {
    displaced_.reset();
    if (!root) {
      auto *leaf = pack(
          true, 1,
          [&](std::uint32_t) {
            return Item{KeyParts{key, {}}, payload_bytes_of(val)};
          },
          0);
      return {leaf, nullptr, true, true};
    }
    return upsert_rec(root, key, val, should_replace, true);
  }

  auto erase(N *root, Bytes key) -> Result {
    if (!root)
      return {nullptr, nullptr, false, false};
    return erase_rec(root, key);
  }

  // A fresh root over two halves of a split root.
  [[nodiscard]] auto make_root(N *left, N *right) -> N * {
    auto *root = pack(
        false, 1,
        [&](std::uint32_t) {
          return Item{KeyParts{sep_, {}}, payload_bytes_of(right)};
        },
        0);
    root->first_child = left;
    return root;
  }

private:
  std::uint64_t tag_;
  std::vector<N *> retired_;
  std::vector<std::byte> sep_; // separator handed up by the last split
  std::optional<V> displaced_;

  void forget_retired() noexcept {
    if (!retired_.empty()) {
      account_retired<V>(-static_cast<std::int64_t>(retired_.size()));
      retired_.clear();
    }
  }

  // One logical entry for pack(): its full key and where its payload is.
  struct Item {
    KeyParts key;
    const std::byte *payload;
  };
  template <typename P>
  static auto payload_bytes_of(const P &p) noexcept -> const std::byte * {
    return static_cast<const std::byte *>(static_cast<const void *>(&p));
  }

  // Builds a node from `n_items` logical entries, in order, with `prefix`
  // bytes of every key stored once. Capacity is the node size for the kind,
  // or more when the entries need it.
  template <typename ItemFn>
  [[nodiscard]] auto pack(bool leaf, std::uint32_t n_items, ItemFn &&item,
                          std::size_t prefix) -> N * {
    std::size_t heap = 0;
    for (std::uint32_t i = 0; i < n_items; ++i)
      heap += N::entry_size(leaf, item(i).key.size() - prefix);
    const auto top = N::slots_offset_for(prefix) + n_items * N::kSlotBytes;
    std::vector<std::byte> prefix_buf;
    Bytes prefix_bytes;
    if (prefix > 0) {
      const auto k = item(0).key;
      if (prefix <= k.a.size()) {
        prefix_bytes = k.a.first(prefix);
      } else {
        prefix_buf.resize(prefix);
        for (std::size_t i = 0; i < prefix; ++i)
          prefix_buf[i] = k.at(i);
        prefix_bytes = prefix_buf;
      }
    }
    auto *n = N::allocate(std::max(N::node_bytes(leaf), top + heap), leaf,
                          tag_, prefix_bytes);
    auto *s = n->slots();
    for (std::uint32_t i = 0; i < n_items; ++i) {
      const auto it = item(i);
      const auto len = it.key.size() - prefix;
      const auto size = N::entry_size(leaf, len);
      const auto off = n->heap_floor + size;
      n->heap_floor = static_cast<std::uint32_t>(off);
      auto *e = n->bytes() + n->capacity - off;
      if (leaf)
        std::construct_at(as_ptr<V>(e), *std::launder(as_ptr<V>(it.payload)));
      else
        std::memcpy(e, it.payload, sizeof(N *));
      const auto len16 = static_cast<std::uint16_t>(len);
      std::memcpy(e + N::payload_size(leaf), &len16, N::kLenBytes);
      auto *suf = e + N::payload_size(leaf) + N::kLenBytes;
      it.key.copy_tail(prefix, suf);
      s[i] = (std::uint64_t{head_of({suf, len})} << 32) | off;
    }
    n->count = n_items;
    return n;
  }

  // A compacted copy of `node` with `prefix` bytes shared; the original is
  // discarded. The one function behind cloning a foreign node, compacting
  // an owned one and changing a node's prefix.
  [[nodiscard]] auto rebuild(N *node, std::size_t prefix) -> N * {
    auto *fresh = pack(
        node->is_leaf != 0, node->count,
        [node](std::uint32_t i) {
          return Item{node->key(i), node->payload_raw(i)};
        },
        prefix);
    fresh->first_child = node->first_child;
    discard(node);
    return fresh;
  }

  [[nodiscard]] auto own(N *node) -> N * {
    return owns(node) ? node : rebuild(node, node->prefix_len);
  }

  // The prefix a node built from items [from, to) should store: what its
  // first and last keys share, or, for a single entry, what it shares with
  // its neighbour — a guess at what the next keys will share too.
  template <typename ItemFn>
  [[nodiscard]] static auto prefix_for(ItemFn &&item, std::uint32_t from,
                                       std::uint32_t to, std::uint32_t total)
      -> std::size_t {
    if (to - from >= 2)
      return common_prefix_length(item(from).key, item(to - 1).key);
    if (to - from == 0)
      return 0;
    if (from > 0)
      return common_prefix_length(item(from - 1).key, item(from).key);
    if (to < total)
      return common_prefix_length(item(from).key, item(to).key);
    return 0;
  }

  // Inserts (key, payload) at entry position `pos` of `node`, which may be
  // foreign. Shrinks the prefix, compacts, or splits as needed. `rightmost`
  // is true when `node` is on the rightmost path of the tree: an insert past
  // the last entry then splits off only the new entry, so ascending inserts
  // fill nodes instead of leaving them half empty.
  template <typename P>
  auto place(N *node, std::uint32_t pos, Bytes key, const P &payload,
             bool rightmost) -> Result {
    const auto cpl = common_prefix_length(node->prefix(), key);
    if (cpl < node->prefix_len)
      node = rebuild(node, cpl);
    const auto suf = key.subspan(node->prefix_len);
    const bool leaf = node->is_leaf != 0;
    const auto need = N::entry_size(leaf, suf.size()) + N::kSlotBytes;
    if (node->free_bytes() >= need) {
      node = own(node);
      node->insert_entry(pos, suf, payload);
      return {node, nullptr, true, true};
    }
    if (node->packed_bytes(node->prefix_len) + need <= node->capacity) {
      node = rebuild(node, node->prefix_len);
      node->insert_entry(pos, suf, payload);
      return {node, nullptr, true, true};
    }
    return split(node, pos, key, payload, rightmost);
  }

  // Splits `node` plus the new entry into two nodes. A leaf keeps every
  // entry and hands up a truncated copy of the right half's first key; an
  // inner node hands up one separator whole, and its child becomes the
  // right half's first child. `key` may alias sep_: it is consumed before
  // sep_ is rewritten.
  template <typename P>
  auto split(N *node, std::uint32_t pos, Bytes key, const P &payload,
             bool rightmost) -> Result {
    const bool leaf = node->is_leaf != 0;
    const auto total = static_cast<std::uint32_t>(node->count + 1);
    const auto *payload_bytes = payload_bytes_of(payload);
    auto item = [&](std::uint32_t i) -> Item {
      if (i < pos)
        return {node->key(i), node->payload_raw(i)};
      if (i == pos)
        return {KeyParts{key, {}}, payload_bytes};
      return {node->key(i - 1), node->payload_raw(i - 1)};
    };
    // Split index: for a leaf, the first entry of the right half; for an
    // inner node, the separator pushed up. Balanced by bytes unless this is
    // an append on the rightmost path.
    std::uint32_t m = 0;
    if (rightmost && pos == node->count) {
      m = total - 1;
    } else {
      std::size_t sum = 0;
      for (std::uint32_t i = 0; i < total; ++i)
        sum += N::entry_size(leaf, item(i).key.size()) + N::kSlotBytes;
      std::size_t acc = 0;
      for (m = 0; m < total; ++m) {
        acc += N::entry_size(leaf, item(m).key.size()) + N::kSlotBytes;
        if (acc >= sum / 2)
          break;
      }
      m = std::clamp<std::uint32_t>(m, 1, total - 1);
    }
    N *left = nullptr;
    N *right = nullptr;
    if (leaf) {
      left = pack(true, m, item, prefix_for(item, 0, m, total));
      auto right_item = [&](std::uint32_t i) { return item(i + m); };
      right = pack(true, total - m, right_item,
                   prefix_for(item, m, total, total));
      // Shortest separator above every left key and at most the right's
      // first key: the right's first key cut after the first byte that
      // differs from the left's last.
      const auto cut = common_prefix_length(item(m - 1).key, item(m).key) + 1;
      const auto first_right = item(m).key;
      sep_.resize(first_right.size());
      first_right.copy_tail(0, sep_.data());
      sep_.resize(std::min(cut, first_right.size()));
    } else {
      left = pack(false, m, item, prefix_for(item, 0, m, total));
      left->first_child = node->first_child;
      auto right_item = [&](std::uint32_t i) { return item(i + m + 1); };
      const auto n_right = total - m - 1;
      right = pack(false, n_right, right_item,
                   prefix_for(item, m + 1, total, total));
      N *mid_child = nullptr;
      std::memcpy(&mid_child, item(m).payload, sizeof(N *));
      right->first_child = mid_child;
      const auto mid = item(m).key;
      sep_.resize(mid.size());
      mid.copy_tail(0, sep_.data());
    }
    discard(node);
    return {left, right, true, true};
  }

  template <typename Pred>
  auto upsert_rec(N *node, Bytes key, const V &val, Pred &should_replace,
                  bool rightmost) -> Result {
    if (node->is_leaf) {
      const auto p = node->search(key);
      if (p.exact) {
        auto *existing = node->template payload<V>(p.idx);
        if (!should_replace(*existing, val))
          return {node, nullptr, false, false};
        auto *n = own(node);
        auto *slot = n->template payload<V>(p.idx);
        displaced_ = std::move(*slot);
        std::destroy_at(slot);
        std::construct_at(slot, val);
        return {n, nullptr, true, false};
      }
      return place(node, p.idx, key, val, rightmost);
    }
    const auto idx = node->child_index(key);
    auto *child = node->child(idx);
    const auto r = upsert_rec(child, key, val, should_replace,
                              rightmost && idx == node->count);
    if (!r.changed)
      return {node, nullptr, false, false};
    auto *n = own(node);
    n->set_child(idx, r.node);
    if (!r.right)
      return {n, nullptr, true, r.inserted};
    // The child split: its separator sits in sep_ and its right half goes
    // in as entry idx, whose payload is the child to the right of it.
    auto placed = place(n, idx, sep_, r.right, rightmost);
    placed.inserted = r.inserted;
    return placed;
  }

  auto erase_rec(N *node, Bytes key) -> Result {
    if (node->is_leaf) {
      const auto p = node->search(key);
      if (!p.exact)
        return {node, nullptr, false, false};
      auto *n = own(node);
      n->remove_entry(p.idx);
      if (n->count == 0) {
        discard(n);
        return {nullptr, nullptr, true, false};
      }
      return {n, nullptr, true, false};
    }
    const auto idx = node->child_index(key);
    const auto r = erase_rec(node->child(idx), key);
    if (!r.changed)
      return {node, nullptr, false, false};
    auto *n = own(node);
    if (r.node) {
      n->set_child(idx, r.node);
      return {n, nullptr, true, false};
    }
    // The child emptied: drop it and the separator next to it.
    if (idx == 0) {
      if (n->count == 0) {
        discard(n);
        return {nullptr, nullptr, true, false};
      }
      n->first_child = n->child(1);
      n->remove_entry(0);
    } else {
      n->remove_entry(idx - 1);
    }
    return {n, nullptr, true, false};
  }
};

} // namespace btree_detail

// ---------------------------------------------------------------------------
// BasicBTreeIterator<V, WithKey> — an in-order cursor over one version.
//
// Holds a handle (one pin) and a stack of (node, index) frames from the
// root to the current leaf entry. With WithKey the current key is
// materialised in a buffer on every step; operator* returns a span into it,
// valid until the next step. Without it, operator* is the value only.
// ---------------------------------------------------------------------------
export template <typename V, bool WithKey> class BasicBTreeIterator {
  using N = btree_detail::Node<V>;
  using Bytes = btree_detail::Bytes;

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using iterator_concept = std::bidirectional_iterator_tag;
  using value_type =
      std::conditional_t<WithKey, std::pair<Bytes, V>, V>;
  using difference_type = std::ptrdiff_t;

  BasicBTreeIterator() = default;

  using reference =
      std::conditional_t<WithKey, std::pair<Bytes, const V &>, const V &>;

  auto operator*() const -> reference {
    const auto &f = stack_.back();
    const V &v = *f.node->template payload<V>(f.idx);
    if constexpr (WithKey)
      return reference{Bytes{key_}, v};
    else
      return v;
  }

  auto operator++() -> BasicBTreeIterator & {
    advance();
    return *this;
  }
  auto operator++(int) -> BasicBTreeIterator {
    auto tmp = *this;
    ++*this;
    return tmp;
  }
  auto operator--() -> BasicBTreeIterator & {
    retreat();
    return *this;
  }
  auto operator--(int) -> BasicBTreeIterator {
    auto tmp = *this;
    --*this;
    return tmp;
  }

  auto operator==(const BasicBTreeIterator &o) const noexcept -> bool {
    if (stack_.empty() || o.stack_.empty())
      return stack_.empty() && o.stack_.empty();
    return stack_.back().node == o.stack_.back().node &&
           stack_.back().idx == o.stack_.back().idx;
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return stack_.empty();
  }

private:
  struct Frame {
    const N *node;
    std::uint32_t idx;
  };

  PersistentBTree<V> tree_; // pin; empty for a transient's iterator
  const N *root_{nullptr};
  std::vector<Frame> stack_;
  std::vector<std::byte> key_;

  enum class Seek { First, Last, End };

  BasicBTreeIterator(PersistentBTree<V> tree, const N *root, Seek where)
      : tree_{std::move(tree)}, root_{root} {
    stack_.reserve(8);
    if (!root_ || where == Seek::End)
      return;
    if (where == Seek::First)
      descend_leftmost(root_);
    else
      descend_rightmost(root_);
    load_key();
  }

  // Positions at the first entry >= target (lower bound).
  BasicBTreeIterator(PersistentBTree<V> tree, const N *root, Bytes target)
      : tree_{std::move(tree)}, root_{root} {
    stack_.reserve(8);
    const N *cur = root_;
    while (cur && !cur->is_leaf) {
      const auto idx = cur->child_index(target);
      stack_.push_back({cur, idx});
      cur = cur->child(idx);
    }
    if (!cur || cur->count == 0)
      return;
    const auto p = cur->search(target);
    stack_.push_back({cur, p.idx});
    if (p.idx == cur->count) {
      // Past this leaf: step back to its last entry and advance from there.
      stack_.back().idx = cur->count - 1;
      advance();
    } else {
      load_key();
    }
  }

  void descend_leftmost(const N *n) {
    while (!n->is_leaf) {
      stack_.push_back({n, 0});
      n = n->child(0);
    }
    stack_.push_back({n, 0});
  }
  void descend_rightmost(const N *n) {
    while (!n->is_leaf) {
      stack_.push_back({n, n->count});
      n = n->child(n->count);
    }
    stack_.push_back({n, static_cast<std::uint32_t>(n->count - 1)});
  }

  void load_key() {
    if constexpr (WithKey) {
      const auto &f = stack_.back();
      const auto pre = f.node->prefix();
      const auto suf = f.node->suffix(f.idx);
      key_.resize(pre.size() + suf.size());
      if (!pre.empty())
        std::memcpy(key_.data(), pre.data(), pre.size());
      if (!suf.empty())
        std::memcpy(key_.data() + pre.size(), suf.data(), suf.size());
    }
  }

  void advance() {
    if (stack_.empty())
      return;
    if (++stack_.back().idx < stack_.back().node->count) {
      load_key();
      return;
    }
    stack_.pop_back();
    while (!stack_.empty()) {
      auto &f = stack_.back();
      if (f.idx < f.node->count) {
        ++f.idx;
        descend_leftmost(f.node->child(f.idx));
        load_key();
        return;
      }
      stack_.pop_back();
    }
  }

  // --end() is the last entry; --begin() is end.
  void retreat() {
    if (stack_.empty()) {
      if (root_) {
        descend_rightmost(root_);
        load_key();
      }
      return;
    }
    if (stack_.back().idx > 0) {
      --stack_.back().idx;
      load_key();
      return;
    }
    stack_.pop_back();
    while (!stack_.empty()) {
      auto &f = stack_.back();
      if (f.idx > 0) {
        --f.idx;
        descend_rightmost(f.node->child(f.idx));
        load_key();
        return;
      }
      stack_.pop_back();
    }
  }

  friend class PersistentBTree<V>;
  friend class TransientBTree<V>;
  friend class BasicReverseBTreeIterator<V, WithKey>;
};

// ---------------------------------------------------------------------------
// BasicReverseBTreeIterator<V, WithKey> — holds the forward cursor alive and
// pre-decrements, so operator* is never a span into a temporary. ++rend()
// is a no-op and rend().base() == begin().
// ---------------------------------------------------------------------------
export template <typename V, bool WithKey> class BasicReverseBTreeIterator {
public:
  using Forward = BasicBTreeIterator<V, WithKey>;
  using iterator_category = std::forward_iterator_tag;
  using value_type = typename Forward::value_type;
  using difference_type = std::ptrdiff_t;

  BasicReverseBTreeIterator() = default;

  explicit BasicReverseBTreeIterator(Forward past_pos) : cur_{std::move(past_pos)} {
    --cur_;
    if (cur_ == std::default_sentinel)
      past_rend_ = true;
  }

  auto operator*() const -> typename Forward::reference { return *cur_; }

  auto operator++() -> BasicReverseBTreeIterator & {
    if (past_rend_)
      return *this;
    --cur_;
    if (cur_ == std::default_sentinel)
      past_rend_ = true;
    return *this;
  }
  auto operator++(int) -> BasicReverseBTreeIterator {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  auto operator==(const BasicReverseBTreeIterator &o) const noexcept -> bool {
    if (past_rend_ != o.past_rend_)
      return false;
    return past_rend_ || cur_ == o.cur_;
  }
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return past_rend_;
  }

  [[nodiscard]] auto base() const -> Forward {
    if (past_rend_)
      return Forward{cur_.tree_, cur_.root_, Forward::Seek::First};
    auto fwd = cur_;
    ++fwd;
    return fwd;
  }

private:
  Forward cur_;
  bool past_rend_{false};
};

export template <typename V> using BTreeIterator = BasicBTreeIterator<V, true>;
export template <typename V>
using ReverseBTreeIterator = BasicReverseBTreeIterator<V, true>;
export template <typename V>
using BTreeValueIterator = BasicBTreeIterator<V, false>;
export template <typename V>
using ReverseBTreeValueIterator = BasicReverseBTreeIterator<V, false>;

// ---------------------------------------------------------------------------
// PersistentBTree<V> — a handle to one immutable version.
//
// Copies are O(1): a root pointer, a size and a pin on the version. Node
// lifetime belongs to VersionChain; this class only pins and unpins. set()
// and erase() are one-operation transients.
// ---------------------------------------------------------------------------
export template <typename V> class PersistentBTree {
  using N = btree_detail::Node<V>;
  using Bytes = btree_detail::Bytes;
  using Chain = VersionChain<btree_detail::ChainTraits<V>>;

public:
  PersistentBTree() = default;
  PersistentBTree(const PersistentBTree &other)
      : root_{other.root_}, size_{other.size_}, version_{other.version_} {
    if (version_)
      chain().pin(version_);
  }
  auto operator=(const PersistentBTree &other) -> PersistentBTree & {
    if (this == &other)
      return *this;
    if (other.version_)
      chain().pin(other.version_);
    release();
    root_ = other.root_;
    size_ = other.size_;
    version_ = other.version_;
    return *this;
  }
  PersistentBTree(PersistentBTree &&other) noexcept
      : root_{std::exchange(other.root_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        version_{std::exchange(other.version_, 0)} {}
  auto operator=(PersistentBTree &&other) noexcept -> PersistentBTree & {
    if (this == &other)
      return *this;
    release();
    root_ = std::exchange(other.root_, nullptr);
    size_ = std::exchange(other.size_, 0);
    version_ = std::exchange(other.version_, 0);
    return *this;
  }
  ~PersistentBTree() { release(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }

  [[nodiscard]] auto get(Bytes key) const -> std::optional<V> {
    const auto *p = btree_detail::find_ptr<V>(root_, key);
    if (!p)
      return std::nullopt;
    return *p;
  }
  // Valid for the lifetime of this handle.
  [[nodiscard]] auto get_ptr(Bytes key) const noexcept -> const V * {
    return btree_detail::find_ptr<V>(root_, key);
  }
  [[nodiscard]] auto contains(Bytes key) const noexcept -> bool {
    return get_ptr(key) != nullptr;
  }

  [[nodiscard]] auto set(Bytes key, V val) const -> PersistentBTree;
  [[nodiscard]] auto erase(Bytes key) const -> PersistentBTree;
  [[nodiscard]] auto transient() const -> TransientBTree<V>;

  // A new tree with every key of `a` and `b`; on a key in both,
  // resolve(a_val, b_val) picks the value. The result shares no node with
  // its inputs and starts a lineage of its own; the inputs are untouched.
  template <typename ResolveFunc>
  [[nodiscard]] static auto merge(const PersistentBTree &a,
                                  const PersistentBTree &b,
                                  ResolveFunc &&resolve) -> PersistentBTree;

  [[nodiscard]] auto begin() const -> BTreeIterator<V> {
    return {*this, root_, BTreeIterator<V>::Seek::First};
  }
  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }
  [[nodiscard]] auto end_iter() const -> BTreeIterator<V> {
    return {*this, root_, BTreeIterator<V>::Seek::End};
  }
  [[nodiscard]] auto rbegin() const -> ReverseBTreeIterator<V> {
    return ReverseBTreeIterator<V>{end_iter()};
  }
  [[nodiscard]] auto rend() const noexcept -> std::default_sentinel_t {
    return {};
  }
  [[nodiscard]] auto lower_bound(Bytes key) const -> BTreeIterator<V> {
    return {*this, root_, key};
  }
  [[nodiscard]] auto upper_bound(Bytes key) const -> BTreeIterator<V> {
    auto it = lower_bound(key);
    if (it != std::default_sentinel) {
      auto [k, v] = *it;
      if (btree_detail::compare_bytes(k, key) == 0)
        ++it;
    }
    return it;
  }

  [[nodiscard]] auto value_begin() const -> BTreeValueIterator<V> {
    return {*this, root_, BTreeValueIterator<V>::Seek::First};
  }
  [[nodiscard]] auto value_lower_bound(Bytes key) const
      -> BTreeValueIterator<V> {
    return {*this, root_, key};
  }
  [[nodiscard]] auto value_rbegin() const -> ReverseBTreeValueIterator<V> {
    return ReverseBTreeValueIterator<V>{
        BTreeValueIterator<V>{*this, root_, BTreeValueIterator<V>::Seek::End}};
  }
  // Reverse from the last key <= `upper`.
  [[nodiscard]] auto value_rlower_bound(Bytes upper) const
      -> ReverseBTreeValueIterator<V> {
    auto fwd = BTreeValueIterator<V>{*this, root_, upper};
    if (fwd != std::default_sentinel) {
      // fwd is at the first key >= upper; include it only if equal.
      auto probe = BTreeIterator<V>{*this, root_, upper};
      auto [k, v] = *probe;
      if (btree_detail::compare_bytes(k, upper) == 0)
        ++fwd;
    }
    return ReverseBTreeValueIterator<V>{std::move(fwd)};
  }

  // -- Test and debug support --------------------------------------------

  // Every retired node still waiting on a live version.
  [[nodiscard]] static auto parked_nodes() -> std::vector<const void *> {
    return chain().parked_nodes();
  }
  // Calls f(const void*) for every node reachable from this version.
  template <typename F> void visit_nodes(F &&f) const {
    std::vector<const N *> stack;
    if (root_)
      stack.push_back(root_);
    while (!stack.empty()) {
      const auto *n = stack.back();
      stack.pop_back();
      f(static_cast<const void *>(n));
      n->for_each_child([&](N *c) { stack.push_back(c); });
    }
  }
  // Checks every structural invariant; throws std::logic_error on the first
  // violation. Returns the tree height (0 for an empty tree).
  auto validate() const -> std::size_t {
    if (!root_)
      return 0;
    std::size_t leaf_depth = 0;
    std::size_t keys = 0;
    std::vector<std::byte> lo;
    std::vector<std::byte> hi;
    validate_node(root_, 1, false, lo, false, hi, leaf_depth, keys);
    if (keys != size_)
      throw std::logic_error{"btree: size does not match key count"};
    return leaf_depth;
  }

private:
  N *root_{nullptr};
  std::size_t size_{0};
  std::uint64_t version_{0};

  PersistentBTree(N *root, std::size_t size, std::uint64_t version) noexcept
      : root_{root}, size_{size}, version_{version} {}

  static auto chain() -> Chain & { return Chain::instance(); }

  void release() noexcept {
    if (version_)
      chain().unpin(version_, root_);
    root_ = nullptr;
    size_ = 0;
    version_ = 0;
  }

  static void validate_node(const N *n, std::size_t depth, bool has_lo,
                            const std::vector<std::byte> &lo, bool has_hi,
                            const std::vector<std::byte> &hi,
                            std::size_t &leaf_depth, std::size_t &keys) {
    auto fail = [] [[noreturn]] (const char *what) {
      throw std::logic_error{std::string{"btree: "} + what};
    };
    if (n->count == 0 && (n->is_leaf || n->first_child == nullptr))
      fail("empty node");
    std::vector<std::byte> prev;
    for (std::uint32_t i = 0; i < n->count; ++i) {
      const auto k = n->key(i);
      std::vector<std::byte> full(k.size());
      k.copy_tail(0, full.data());
      if (n->suffix(i).size() > 0 &&
          N::slot_head(n->slots()[i]) != btree_detail::head_of(n->suffix(i)))
        fail("head does not match suffix");
      if (i > 0 && btree_detail::compare_bytes(full, prev) <= 0)
        fail("entries out of order");
      if (has_lo && btree_detail::compare_bytes(full, lo) < 0)
        fail("key below lower bound");
      if (has_hi && btree_detail::compare_bytes(full, hi) >= 0)
        fail("key at or above upper bound");
      prev = std::move(full);
    }
    if (n->free_bytes() > n->capacity)
      fail("free bytes overflow");
    if (n->is_leaf) {
      if (leaf_depth == 0)
        leaf_depth = depth;
      else if (leaf_depth != depth)
        fail("leaves at different depths");
      keys += n->count;
      return;
    }
    for (std::uint32_t c = 0; c <= n->count; ++c) {
      std::vector<std::byte> clo = lo;
      std::vector<std::byte> chi = hi;
      bool c_has_lo = has_lo;
      bool c_has_hi = has_hi;
      if (c > 0) {
        const auto k = n->key(c - 1);
        clo.resize(k.size());
        k.copy_tail(0, clo.data());
        c_has_lo = true;
      }
      if (c < n->count) {
        const auto k = n->key(c);
        chi.resize(k.size());
        k.copy_tail(0, chi.data());
        c_has_hi = true;
      }
      validate_node(n->child(c), depth + 1, c_has_lo, clo, c_has_hi, chi,
                    leaf_depth, keys);
    }
  }

  friend class TransientBTree<V>;
  friend class BasicBTreeIterator<V, true>;
  friend class BasicBTreeIterator<V, false>;
};

// ---------------------------------------------------------------------------
// TransientBTree<V> — a mutable working copy of one version.
//
// Produced by PersistentBTree::transient(); frozen by persistent() &&. Nodes
// the session created are edited in place; everything else is copied on
// first touch. A transient nothing was written to publishes nothing and
// hands back its base. Single-use: any call after persistent() throws.
// ---------------------------------------------------------------------------
export template <typename V> class TransientBTree {
  using N = btree_detail::Node<V>;
  using Bytes = btree_detail::Bytes;

public:
  TransientBTree(const TransientBTree &) = delete;
  auto operator=(const TransientBTree &) -> TransientBTree & = delete;
  TransientBTree(TransientBTree &&other) noexcept
      : base_{std::move(other.base_)}, session_{std::move(other.session_)},
        root_{std::exchange(other.root_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        changed_{std::exchange(other.changed_, false)} {}
  auto operator=(TransientBTree &&other) noexcept -> TransientBTree & {
    if (this != &other) {
      discard();
      base_ = std::move(other.base_);
      session_ = std::move(other.session_);
      root_ = std::exchange(other.root_, nullptr);
      size_ = std::exchange(other.size_, 0);
      changed_ = std::exchange(other.changed_, false);
    }
    return *this;
  }
  ~TransientBTree() { discard(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }

  [[nodiscard]] auto get(Bytes key) const -> std::optional<V> {
    ensure_active();
    const auto *p = btree_detail::find_ptr<V>(root_, key);
    if (!p)
      return std::nullopt;
    return *p;
  }
  // Valid until the next mutation.
  [[nodiscard]] auto get_ptr(Bytes key) const -> const V * {
    ensure_active();
    return btree_detail::find_ptr<V>(root_, key);
  }
  [[nodiscard]] auto contains(Bytes key) const -> bool {
    return get_ptr(key) != nullptr;
  }

  void set(Bytes key, V val) {
    (void)upsert(key, std::move(val),
                 [](const V &, const V &) { return true; });
  }

  // Single descent: inserts if absent, replaces if
  // should_replace(existing, incoming). Returns the displaced value.
  template <typename Pred>
  auto upsert(Bytes key, V val, Pred &&should_replace) -> std::optional<V> {
    ensure_active();
    if (key.size() > kBTreeMaxKeyBytes)
      throw std::length_error{"TransientBTree: key exceeds 65535 bytes"};
    const auto r = session_.upsert(root_, key, val, should_replace);
    if (!r.changed)
      return std::nullopt;
    changed_ = true;
    root_ = r.right ? session_.make_root(r.node, r.right) : r.node;
    if (r.inserted)
      ++size_;
    return session_.take_displaced();
  }

  auto erase(Bytes key) -> bool {
    ensure_active();
    const auto r = session_.erase(root_, key);
    if (!r.changed)
      return false;
    changed_ = true;
    root_ = r.node;
    // Collapse a root left with a single child.
    while (root_ && !root_->is_leaf && root_->count == 0) {
      auto *old = root_;
      root_ = old->first_child;
      session_.discard(old);
    }
    --size_;
    return true;
  }

  // Publishes the built tree as a new version. Throws std::logic_error if
  // the base already has a successor (the chain contract); the transient
  // is then still active and its destructor discards what it built.
  [[nodiscard]] auto persistent() && -> PersistentBTree<V> {
    ensure_active();
    if (!changed_) {
      assert(root_ == base_.root_ && session_.retired_list().empty());
      session_.finish();
      root_ = nullptr;
      size_ = 0;
      return std::move(base_);
    }
    PersistentBTree<V>::chain().publish(session_.tag(), base_.version_,
                                        session_.retired_list());
    const auto version = session_.tag();
    session_.finish();
    auto live_size = std::exchange(size_, 0);
    auto *root = std::exchange(root_, nullptr);
    base_ = PersistentBTree<V>{};
    return PersistentBTree<V>{root, live_size, version};
  }

  // Range scans on the transient. The iterator holds raw node pointers: do
  // not mutate the transient while it is alive.
  [[nodiscard]] auto lower_bound(Bytes key) const -> BTreeIterator<V> {
    ensure_active();
    return BTreeIterator<V>{PersistentBTree<V>{}, root_, key};
  }

private:
  PersistentBTree<V> base_;
  btree_detail::BuildSession<V> session_;
  N *root_{nullptr};
  std::size_t size_{0};
  bool changed_{false};

  explicit TransientBTree(const PersistentBTree<V> &base)
      : base_{base}, root_{base.root_}, size_{base.size_} {}

  void ensure_active() const {
    if (session_.tag() == 0) [[unlikely]]
      throw std::logic_error{"TransientBTree already consumed"};
  }

  void discard() noexcept {
    session_.discard_all(root_);
    root_ = nullptr;
    size_ = 0;
  }

  friend class PersistentBTree<V>;
};

template <typename V>
auto PersistentBTree<V>::transient() const -> TransientBTree<V> {
  return TransientBTree<V>{*this};
}

template <typename V>
auto PersistentBTree<V>::set(Bytes key, V val) const -> PersistentBTree {
  auto t = transient();
  t.set(key, std::move(val));
  return std::move(t).persistent();
}

template <typename V>
auto PersistentBTree<V>::erase(Bytes key) const -> PersistentBTree {
  if (!root_)
    return *this;
  auto t = transient();
  if (!t.erase(key))
    return *this;
  return std::move(t).persistent();
}

template <typename V>
template <typename ResolveFunc>
auto PersistentBTree<V>::merge(const PersistentBTree &a,
                               const PersistentBTree &b,
                               ResolveFunc &&resolve) -> PersistentBTree {
  auto t = PersistentBTree{}.transient();
  auto ia = a.begin();
  auto ib = b.begin();
  while (ia != std::default_sentinel || ib != std::default_sentinel) {
    if (ib == std::default_sentinel) {
      auto [k, v] = *ia;
      t.set(k, v);
      ++ia;
      continue;
    }
    if (ia == std::default_sentinel) {
      auto [k, v] = *ib;
      t.set(k, v);
      ++ib;
      continue;
    }
    auto [ka, va] = *ia;
    auto [kb, vb] = *ib;
    const auto c = btree_detail::compare_bytes(ka, kb);
    if (c < 0) {
      t.set(ka, va);
      ++ia;
    } else if (c > 0) {
      t.set(kb, vb);
      ++ib;
    } else {
      t.set(ka, resolve(va, vb));
      ++ia;
      ++ib;
    }
  }
  return std::move(t).persistent();
}

} // namespace bytecask
