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
#include <bit>
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

// Shape statistics for one version (debug and benchmark support).
export struct BTreeStats {
  std::size_t nodes{0};
  std::size_t leaves{0};
  std::size_t entries{0};         // keys in leaves
  std::size_t capacity_bytes{0};  // sum of node capacities
  std::size_t used_bytes{0};      // capacity minus free space
  std::size_t dead_bytes{0};      // erased entries not yet compacted
  std::size_t leaf_prefix_bytes{0}; // sum over leaves of prefix_len
  std::size_t leaf_suffix_bytes{0}; // sum over leaf entries of suffix length
  std::size_t height{0};
  std::vector<std::uint32_t> leaf_counts; // entries per leaf, in visit order
  std::vector<std::uint32_t> leaf_prefix_lens;
  std::vector<std::uint32_t> leaf_free_bytes;
};

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

// Test-only: how many splits each rule decided (outlier-last, outlier-first,
// sequential-ascending, sequential-descending, balanced).
export inline std::atomic<std::uint64_t> split_rule_counts[5]{};

constexpr auto align_up(std::size_t n, std::size_t a) noexcept -> std::size_t {
  return (n + a - 1) / a * a;
}

export template <typename T> auto as_ptr(std::byte *p) noexcept -> T * {
  return static_cast<T *>(static_cast<void *>(p));
}
export template <typename T> auto as_ptr(const std::byte *p) noexcept -> const T * {
  return static_cast<const T *>(static_cast<const void *>(p));
}

export inline auto compare_bytes(Bytes a, Bytes b) noexcept -> int {
  const auto n = std::min(a.size(), b.size());
  const int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (c != 0)
    return c;
  if (a.size() == b.size())
    return 0;
  return a.size() < b.size() ? -1 : 1;
}

export inline auto common_prefix_length(Bytes a, Bytes b) noexcept
    -> std::size_t {
  const auto n = std::min(a.size(), b.size());
  std::size_t i = 0;
  // Eight bytes at a time: the first differing byte is the lowest set bit
  // of the xor on a little-endian host, the highest on a big-endian one.
  for (; i + 8 <= n; i += 8) {
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::memcpy(&x, a.data() + i, 8);
    std::memcpy(&y, b.data() + i, 8);
    if (x != y) {
      const auto diff = x ^ y;
      if constexpr (std::endian::native == std::endian::little)
        return i + static_cast<std::size_t>(std::countr_zero(diff)) / 8;
      else
        return i + static_cast<std::size_t>(std::countl_zero(diff)) / 8;
    }
  }
  while (i < n && a[i] == b[i])
    ++i;
  return i;
}

// First four suffix bytes as a big-endian integer, zero-padded. For two
// suffixes, head(a) < head(b) implies a < b and head(a) > head(b) implies
// a > b; equal heads decide nothing.
inline auto head_of(Bytes suffix) noexcept -> std::uint32_t {
  std::uint32_t h = 0;
  if (suffix.size() >= 4) {
    std::memcpy(&h, suffix.data(), 4);
  } else {
    for (std::size_t i = 0; i < suffix.size(); ++i)
      h |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(suffix[i]))
           << (24 - 8 * i);
    return h;
  }
  if constexpr (std::endian::native == std::endian::little)
    return std::byteswap(h);
  else
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
//   [header 40][hints 64][prefix bytes][slots: count × u64 ->] ...free... [<- entries]
//
//   slot   = head << 32 | off        off: distance from the node end to the
//                                    entry start (a multiple of kAlign)
//   entry  = [payload][u16 len][suffix bytes]   payload: V (leaf) or Node*
//
// Nodes are immutable once their version is published; only the session
// whose tag they carry writes to them.
// ---------------------------------------------------------------------------
export template <typename V> struct Node {
  std::uint64_t tag{0};        // creating session: ownership and reclamation
  Node *first_child{nullptr};  // inner only: child left of every separator
  std::uint32_t capacity{0};   // bytes allocated
  std::uint32_t heap_floor{0}; // distance from the node end to the lowest entry
  std::uint32_t dead_bytes{0}; // erased entries still occupying the heap
  std::uint32_t count{0};      // entries
  std::uint16_t prefix_len{0}; // bytes every key in the node shares
  static constexpr std::uint16_t kNoLastPos = 0xFFFF;
  static constexpr std::uint32_t kHints = 16;
  static constexpr std::uint32_t kHintMinCount = 2 * (kHints + 1);

  std::uint16_t last_pos{kNoLastPos}; // where the last in-place insert went
  std::uint8_t is_leaf{1};
  // The header ends here: 40 bytes, shared with every node shape built on
  // it (blind_btree.cppm's leaves). What follows is the slotted page's.
  //
  // hints: heads sampled every count/(kHints+1) slots, stored right after the
  // header. A search scans the hints (64 bytes) to find the run of slots that
  // can hold the key, then scans that run: a few slots instead of the whole
  // array.
  static constexpr std::size_t kHintBytes = kHints * sizeof(std::uint32_t);

  static constexpr std::size_t kAlign =
      std::max({alignof(V), alignof(void *), std::size_t{8}});
  static constexpr std::size_t kLenBytes = 2;
  static constexpr std::size_t kSlotBytes = 8;

  [[nodiscard]] static constexpr auto header_bytes() noexcept -> std::size_t {
    static_assert(align_up(sizeof(Node), 8) == 40,
                  "the node header is shared with blind leaves; keep it small");
    return align_up(sizeof(Node), 8);
  }
  [[nodiscard]] static constexpr auto prefix_offset() noexcept -> std::size_t {
    return header_bytes() + kHintBytes;
  }
  [[nodiscard]] static auto slots_offset_for(std::size_t prefix) noexcept
      -> std::size_t {
    return align_up(prefix_offset() + prefix, kSlotBytes);
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
    std::memset(mem + header_bytes(), 0, kHintBytes);
    if (!pre.empty())
      std::memcpy(mem + prefix_offset(), pre.data(), pre.size());
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
    return {bytes() + prefix_offset(), prefix_len};
  }
  [[nodiscard]] auto hints() noexcept -> std::uint32_t * {
    return as_ptr<std::uint32_t>(bytes() + header_bytes());
  }
  [[nodiscard]] auto hints() const noexcept -> const std::uint32_t * {
    return as_ptr<std::uint32_t>(bytes() + header_bytes());
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
    std::uint32_t lo = 0;
    std::uint32_t hi = count;
    if (count >= kHintMinCount) {
      // Hints below the head are runs of slots entirely below the key; the
      // first hint at or above it bounds the run that can hold it.
      std::uint32_t k = 0;
      const auto *h = hints();
      for (std::uint32_t i = 0; i < kHints; ++i)
        k += h[i] < head ? 1u : 0u;
      const auto dist = count / (kHints + 1);
      lo = k == 0 ? 0 : k * dist + 1;
      hi = k == kHints ? count : (k + 1) * dist + 1;
    }
    // Branch-free count over the run; the compiler vectorises it.
    std::uint32_t pos = lo;
    for (std::uint32_t i = lo; i < hi; ++i)
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
    last_pos = pos < kNoLastPos ? static_cast<std::uint16_t>(pos) : kNoLastPos;
    update_hints();
  }

  // Rebuilds the hints from the slot array. O(kHints); called after every
  // change to the slot array.
  void update_hints() noexcept {
    if (count < kHintMinCount)
      return;
    const auto dist = count / (kHints + 1);
    const auto *s = slots();
    auto *h = hints();
    for (std::uint32_t i = 0; i < kHints; ++i)
      h[i] = slot_head(s[(i + 1) * dist]);
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
    update_hints();
  }

  template <typename F> void for_each_child(F &&f) const {
    if (is_leaf)
      return;
    f(first_child);
    for (std::uint32_t i = 0; i < count; ++i)
      f(*payload<Node *>(i));
  }
};

export template <typename V> class BulkLoader;
export template <typename V> class LeafRun;

export template <typename V> struct ChainTraits {
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
export template <typename V> class BuildSession {
public:
  using N = Node<V>;
  friend class BulkLoader<V>;

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
      free_node_subtree_if<ChainTraits<V>>(root,
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
    auto leaf_step = [&](N *leaf) {
      return upsert_leaf(leaf, key, val, should_replace);
    };
    return descend_upsert(root, key, leaf_step);
  }

  auto erase(N *root, Bytes key) -> Result {
    if (!root)
      return {nullptr, nullptr, false, false};
    auto leaf_step = [&](N *leaf) { return erase_leaf(leaf, key); };
    return descend_erase(root, key, leaf_step);
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

  // Everything below is shared with trees whose leaves have another layout
  // (blind_btree.cppm): they route through the same inner nodes and supply
  // their own leaf step to the descent.
protected:
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
    n->update_hints();
    return n;
  }

  // A compacted copy of `node` with `prefix` bytes shared; the original is
  // discarded. The one function behind cloning a foreign node, compacting
  // an owned one and changing a node's prefix.
  [[nodiscard]] auto rebuild(N *node, std::size_t prefix) -> N * {
    if constexpr (std::is_trivially_copyable_v<V>) {
      if (prefix == node->prefix_len && node->dead_bytes == 0) {
        // Same layout: the slot array and the heap are position-independent
        // within a node of the same capacity, so two memcpys clone it.
        auto *fresh = N::allocate(node->capacity, node->is_leaf != 0, tag_,
                                  node->prefix());
        const auto slots_off = N::slots_offset_for(node->prefix_len);
        std::memcpy(fresh->bytes() + slots_off, node->bytes() + slots_off,
                    node->count * N::kSlotBytes);
        std::memcpy(fresh->bytes() + node->capacity - node->heap_floor,
                    node->bytes() + node->capacity - node->heap_floor,
                    node->heap_floor);
        fresh->count = node->count;
        fresh->heap_floor = node->heap_floor;
        fresh->first_child = node->first_child;
        fresh->last_pos = node->last_pos;
        std::memcpy(fresh->hints(), node->hints(), N::kHintBytes);
        discard(node);
        return fresh;
      }
    }
    auto *fresh = pack(
        node->is_leaf != 0, node->count,
        [node](std::uint32_t i) {
          return Item{node->key(i), node->payload_raw(i)};
        },
        prefix);
    fresh->first_child = node->first_child;
    // A rebuild keeps the entries and their order, so the sequential-insert
    // evidence stays valid; losing it here made every prefix shrink on a
    // filling node end in a balanced split.
    fresh->last_pos = node->last_pos;
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
  // foreign. Shrinks the prefix, compacts, or splits as needed.
  template <typename P>
  auto place(N *node, std::uint32_t pos, Bytes key, const P &payload)
      -> Result {
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
    if (node->count == 0) {
      // Nothing to split. An empty node whose one new entry does not fit the
      // standard capacity is the "a node is larger only when a single key
      // needs it" case, and such a node holds exactly that entry. pack()
      // sizes a node to its contents, so build it directly; falling through
      // to split() would ask for a split index in [1, 0].
      const auto *payload_bytes = payload_bytes_of(payload);
      auto *fresh = pack(
          leaf, 1,
          [&](std::uint32_t) { return Item{KeyParts{key, {}}, payload_bytes}; },
          0);
      fresh->first_child = node->first_child;
      discard(node);
      return {fresh, nullptr, true, true};
    }
    return split(node, pos, key, payload);
  }

  // Splits `node` plus the new entry into two nodes. A leaf keeps every
  // entry and hands up a truncated copy of the right half's first key; an
  // inner node hands up one separator whole, and its child becomes the
  // right half's first child. `key` may alias sep_: it is consumed before
  // sep_ is rewritten.
  template <typename P>
  auto split(N *node, std::uint32_t pos, Bytes key, const P &payload)
      -> Result {
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
    // inner node, the separator pushed up. In order of preference:
    //   1. an entry at either end that alone shortens the node prefix by
    //      8 bytes or more is split off on its own — a boundary between two
    //      key families otherwise keeps the whole node at that short prefix;
    //   2. inserts arriving in order split at the insert point, so a
    //      sequential stream fills nodes instead of leaving them half empty
    //      (the previous in-place insert into this node tells);
    //   3. otherwise balanced by bytes.
    std::uint32_t m = 0;
    std::size_t rule = 4;
    const auto p_all = common_prefix_length(item(0).key, item(total - 1).key);
    if (total >= 3 &&
        common_prefix_length(item(0).key, item(total - 2).key) >= p_all + 8) {
      m = total - 1;
      rule = 0;
    } else if (total >= 3 &&
               common_prefix_length(item(1).key, item(total - 1).key) >=
                   p_all + 8) {
      m = 1;
      rule = 1;
    } else if (node->last_pos != N::kNoLastPos && pos == node->last_pos + 1u) {
      m = pos; // ascending stream: the new entry starts the right node
      rule = 2;
      // Entries after the insert point that share far less with the new
      // key than its predecessor does are another key family: give them a
      // node of their own instead of carrying them along with the stream,
      // which would leave a short node behind at every split.
      if (pos >= 1 && pos + 1 < total) {
        const auto with_prev =
            common_prefix_length(item(pos - 1).key, item(pos).key);
        const auto with_next =
            common_prefix_length(item(pos).key, item(pos + 1).key);
        if (with_next + 8 <= with_prev)
          m = pos + 1;
      }
    } else if (pos == 0 && node->last_pos == 0) {
      m = 1; // descending stream: the new entry ends the left node
      rule = 3;
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
    }
    m = std::clamp<std::uint32_t>(m, 1, total - 1);
#ifdef BYTECASK_TESTING
    split_rule_counts[rule].fetch_add(1, std::memory_order_relaxed);
#else
    (void)rule;
#endif
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

  // Path copy from `node` down to the leaf that holds `key`, where
  // `leaf_step(leaf)` makes the change. A leaf that split hands its right
  // half and its separator (in sep_) up; this places them in the parent.
  template <typename LeafStep>
  auto descend_upsert(N *node, Bytes key, LeafStep &leaf_step) -> Result {
    if (node->is_leaf)
      return leaf_step(node);
    const auto idx = node->child_index(key);
    auto *child = node->child(idx);
    const auto r = descend_upsert(child, key, leaf_step);
    if (!r.changed)
      return {node, nullptr, false, false};
    auto *n = own(node);
    n->set_child(idx, r.node);
    if (!r.right)
      return {n, nullptr, true, r.inserted};
    // The child split: its separator sits in sep_ and its right half goes
    // in as entry idx, whose payload is the child to the right of it.
    auto placed = place(n, idx, sep_, r.right);
    placed.inserted = r.inserted;
    return placed;
  }

  // As descend_upsert, for a removal: a leaf step that empties its leaf
  // discards it and returns a null node, and the parent drops the child.
  template <typename LeafStep>
  auto descend_erase(N *node, Bytes key, LeafStep &leaf_step) -> Result {
    if (node->is_leaf)
      return leaf_step(node);
    const auto idx = node->child_index(key);
    const auto r = descend_erase(node->child(idx), key, leaf_step);
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

private:
  template <typename Pred>
  auto upsert_leaf(N *node, Bytes key, const V &val, Pred &should_replace)
      -> Result {
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
    return place(node, p.idx, key, val);
  }

  auto erase_leaf(N *node, Bytes key) -> Result {
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
};


// ---------------------------------------------------------------------------
// LeafRun<V> — the sealed leaves of one BulkLoader, in ascending key order.
//
// A loader over one slice of the key space hands its leaves over instead of
// finishing a tree, so several loaders that ran in parallel over disjoint,
// ordered slices can be stitched into one tree by BulkLoader::concat. Owns
// the leaves until concat takes them: a run destroyed on an error path frees
// them, so an abandoned parallel build leaks nothing.
// ---------------------------------------------------------------------------
export template <typename V> class LeafRun {
public:
  LeafRun() = default;
  LeafRun(const LeafRun &) = delete;
  auto operator=(const LeafRun &) -> LeafRun & = delete;
  LeafRun(LeafRun &&other) noexcept
      : leaves_{std::move(other.leaves_)}, seps_{std::move(other.seps_)},
        first_key_{std::move(other.first_key_)},
        last_key_{std::move(other.last_key_)},
        size_{std::exchange(other.size_, 0)},
        tag_{std::exchange(other.tag_, 0)} {
    other.leaves_.clear();
  }
  auto operator=(LeafRun &&other) noexcept -> LeafRun & {
    if (this != &other) {
      free_leaves();
      leaves_ = std::move(other.leaves_);
      other.leaves_.clear();
      seps_ = std::move(other.seps_);
      first_key_ = std::move(other.first_key_);
      last_key_ = std::move(other.last_key_);
      size_ = std::exchange(other.size_, 0);
      tag_ = std::exchange(other.tag_, 0);
    }
    return *this;
  }
  ~LeafRun() { free_leaves(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return leaves_.empty(); }

private:
  friend class BulkLoader<V>;

  void free_leaves() noexcept {
    for (auto *leaf : leaves_)
      Node<V>::destroy(leaf);
    leaves_.clear();
  }

  std::vector<Node<V> *> leaves_;
  // seps_[i] separates leaves_[i] from leaves_[i + 1].
  std::vector<std::vector<std::byte>> seps_;
  std::vector<std::byte> first_key_;
  std::vector<std::byte> last_key_;
  std::size_t size_{0};
  std::uint64_t tag_{0};
};

// ---------------------------------------------------------------------------
// BulkLoader<V> — builds a tree from entries appended in ascending key order.
//
// Leaves are filled to capacity and sealed, then the inner levels are built
// bottom-up from the sealed children, so there is no descent, no split and no
// path copy: each key is written exactly once. This is what a merge or a
// recovery rebuild should use; `set()` in a loop pays a full descent per key
// and splits a leaf every fanout inserts.
//
// The result starts its own lineage in the version chain. Keys must arrive
// strictly ascending; the caller owns that (an ordered merge does).
// ---------------------------------------------------------------------------
export template <typename V> class BulkLoader {
public:
  using N = Node<V>;

  BulkLoader() = default;
  BulkLoader(const BulkLoader &) = delete;
  auto operator=(const BulkLoader &) -> BulkLoader & = delete;

  void append(Bytes key, const V &value) {
    if (key.size() > kBTreeMaxKeyBytes)
      throw std::length_error{"BulkLoader: key exceeds 65535 bytes"};
    // Sealing the current leaf first keeps the size estimate exact.
    if (!leaf_lens_.empty() && !fits(key)) {
      seal_leaf();
    }
    if (size_ == 0)
      first_key_.assign(key.begin(), key.end());
    admit(key);
    leaf_vals_.push_back(value);
    ++size_;
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }

  [[nodiscard]] auto finish() && -> PersistentBTree<V> {
    seal_leaf();
    const auto tag = session_.tag();
    return std::move(*this).assemble(tag);
  }

  // Seals the leaves without building the tree above them, so this loader's
  // slice can be concatenated with the slices other threads built.
  [[nodiscard]] auto seal() && -> LeafRun<V> {
    seal_leaf();
    LeafRun<V> run;
    if (!levels_.empty()) {
      run.leaves_ = std::move(levels_[0].children);
      run.seps_ = std::move(levels_[0].seps);
      levels_.clear();
    }
    run.first_key_ = std::move(first_key_);
    run.last_key_ = std::move(prev_last_);
    run.size_ = size_;
    run.tag_ = session_.tag();
    session_.finish();
    return run;
  }

  // Stitches runs built over disjoint, ascending slices into one tree. Only
  // the levels above the leaves are built here — O(entries / fanout) — so the
  // serial tail of a parallel build stays proportional to the leaf count.
  // The caller owns the ordering: run r's keys must all sort below run r + 1's.
  [[nodiscard]] static auto concat(std::vector<LeafRun<V>> runs)
      -> PersistentBTree<V> {
    BulkLoader<V> out;
    const auto publish_tag = concat_into(out, runs);
    return std::move(out).assemble(publish_tag);
  }

  // The level-building and publishing half below is shared with loaders
  // whose leaves have another layout (blind_btree.cppm).
protected:
  // Adds the runs' leaves to `out`'s leaf level in order, with the
  // separators between runs, and returns the tag to publish under: at or
  // above every node's.
  static auto concat_into(BulkLoader &out, std::vector<LeafRun<V>> &runs)
      -> std::uint64_t {
    auto publish_tag = out.session_.tag();
    std::vector<std::byte> prev_last;
    for (auto &run : runs) {
      if (run.leaves_.empty())
        continue;
      publish_tag = std::max(publish_tag, run.tag_);
      for (std::size_t i = 0; i < run.leaves_.size(); ++i) {
        auto sep = i > 0 ? std::move(run.seps_[i - 1])
                         : out.first_leaf()
                               ? std::vector<std::byte>{}
                               : separator(Bytes{prev_last},
                                           Bytes{run.first_key_});
        out.add_child(0, run.leaves_[i], sep);
      }
      run.leaves_.clear();  // ownership moved to `out`
      prev_last = std::move(run.last_key_);
      out.size_ += run.size_;
    }
    return publish_tag;
  }

  // One built level: the children produced for it, and the separator that
  // sits between each child and the one before it (so seps[i] separates
  // children[i] from children[i + 1], and there are children.size() - 1).
  struct Level {
    std::vector<N *> children;
    std::vector<std::vector<std::byte>> seps;
  };

  BuildSession<V> session_;
  // The leaf being filled: key bytes packed into one arena, plus the values.
  std::vector<std::byte> leaf_arena_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> leaf_spans_;
  std::vector<std::uint32_t> leaf_lens_;
  std::vector<V> leaf_vals_;
  std::size_t leaf_prefix_{0};
  std::size_t leaf_heap_{0};
  std::vector<std::byte> first_key_;  // first key appended to this loader
  std::vector<std::byte> prev_last_;  // last key of the previous leaf
  std::vector<std::byte> pending_sep_; // separator before the leaf being filled
  std::vector<Level> levels_;
  std::size_t size_{0};

  [[nodiscard]] auto key_at(std::size_t i) const noexcept -> Bytes {
    return {leaf_arena_.data() + leaf_spans_[i].first, leaf_spans_[i].second};
  }

  // True while no leaf has been added yet — the first one needs no separator.
  [[nodiscard]] auto first_leaf() const noexcept -> bool {
    return levels_.empty() || levels_[0].children.empty();
  }

  // Builds the levels above the sealed leaves and publishes the version.
  // `publish_tag` must be at or above the tag of every node in the tree: the
  // chain frees a dead version by tag interval, and a node tagged above the
  // published version would be taken for a later version's garbage.
  [[nodiscard]] auto assemble(std::uint64_t publish_tag) && -> PersistentBTree<V> {
    auto *root = std::move(*this).assemble_root(publish_tag);
    if (!root)
      return {};
    return PersistentBTree<V>{root, size_, publish_tag};
  }

  // The root of the published version, or null for an empty loader.
  [[nodiscard]] auto assemble_root(std::uint64_t publish_tag) && -> N * {
    if (first_leaf())
      return nullptr;
    std::size_t lvl = 0;
    while (levels_[lvl].children.size() > 1) {
      build_parent(lvl);
      ++lvl;
    }
    auto *root = levels_[lvl].children.front();
    VersionChain<ChainTraits<V>>::instance().publish(publish_tag, 0,
                                                     session_.retired_list());
    session_.finish();
    levels_.clear();
    return root;
  }

  // Would `key` still fit in the leaf being filled? A shorter shared prefix
  // grows every entry, so the heap is recomputed when the prefix moves.
  [[nodiscard]] auto fits(Bytes key) -> bool {
    const auto p = std::min<std::size_t>(
        leaf_prefix_, common_prefix_length(key_at(0), key));
    auto heap = leaf_heap_;
    if (p != leaf_prefix_) {
      heap = 0;
      for (auto len : leaf_lens_)
        heap += N::entry_size(true, len - p);
    }
    heap += N::entry_size(true, key.size() - p);
    const auto total = N::slots_offset_for(p) +
                       (leaf_lens_.size() + 1) * N::kSlotBytes + heap;
    if (total > N::node_bytes(true))
      return false;
    leaf_prefix_ = p;
    leaf_heap_ = heap - N::entry_size(true, key.size() - p);
    return true;
  }

  void admit(Bytes key) {
    if (leaf_lens_.empty()) {
      leaf_prefix_ = key.size();
      if (!prev_last_.empty() || !levels_.empty())
        pending_sep_ = separator(Bytes{prev_last_}, key);
    }
    const auto off = static_cast<std::uint32_t>(leaf_arena_.size());
    leaf_arena_.insert(leaf_arena_.end(), key.begin(), key.end());
    leaf_spans_.emplace_back(off, static_cast<std::uint32_t>(key.size()));
    leaf_lens_.push_back(static_cast<std::uint32_t>(key.size()));
    leaf_heap_ += N::entry_size(true, key.size() - leaf_prefix_);
  }

  void seal_leaf() {
    if (leaf_lens_.empty())
      return;
    auto item = [this](std::uint32_t i) {
      return typename BuildSession<V>::Item{
          KeyParts{key_at(i), {}},
          BuildSession<V>::payload_bytes_of(leaf_vals_[i])};
    };
    auto *leaf = session_.pack(
        true, static_cast<std::uint32_t>(leaf_lens_.size()), item, leaf_prefix_);
    const auto last = key_at(leaf_lens_.size() - 1);
    std::vector<std::byte> last_copy{last.begin(), last.end()};
    add_child(0, leaf, pending_sep_);
    prev_last_ = std::move(last_copy);
    leaf_arena_.clear();
    leaf_spans_.clear();
    leaf_lens_.clear();
    leaf_vals_.clear();
    leaf_prefix_ = 0;
    leaf_heap_ = 0;
    pending_sep_.clear();
  }

  void add_child(std::size_t lvl, N *child, const std::vector<std::byte> &sep) {
    if (levels_.size() <= lvl)
      levels_.emplace_back();
    auto &L = levels_[lvl];
    if (!L.children.empty())
      L.seps.push_back(sep);
    L.children.push_back(child);
  }

  // The shortest key that sorts above `prev` and at or below `next`.
  [[nodiscard]] static auto separator(Bytes prev, Bytes next)
      -> std::vector<std::byte> {
    const auto cut = std::min(common_prefix_length(prev, next) + 1, next.size());
    return {next.begin(), next.begin() + static_cast<std::ptrdiff_t>(cut)};
  }

  // Packs level `lvl`'s children into inner nodes one level up. Each inner
  // node takes a child as first_child, then (separator, child) entries until
  // full; the separator that would have started the next node is pushed up.
  void build_parent(std::size_t lvl) {
    auto src = std::move(levels_[lvl]);
    if (levels_.size() <= lvl + 1)
      levels_.emplace_back();
    std::size_t i = 0;
    while (i < src.children.size()) {
      const auto up_sep = i == 0 ? std::vector<std::byte>{} : src.seps[i - 1];
      auto *first = src.children[i];
      std::vector<Bytes> keys;
      std::vector<N *> kids;
      std::size_t prefix = 0;
      std::size_t heap = 0;
      std::size_t j = i + 1;
      for (; j < src.children.size(); ++j) {
        const Bytes sep{src.seps[j - 1]};
        const auto p = keys.empty()
                           ? sep.size()
                           : std::min(prefix, common_prefix_length(keys[0], sep));
        auto h = heap;
        if (p != prefix) {
          h = 0;
          for (auto k : keys)
            h += N::entry_size(false, k.size() - p);
        }
        h += N::entry_size(false, sep.size() - p);
        const auto total =
            N::slots_offset_for(p) + (keys.size() + 1) * N::kSlotBytes + h;
        if (!keys.empty() && total > N::node_bytes(false))
          break;
        prefix = p;
        heap = h;
        keys.push_back(sep);
        kids.push_back(src.children[j]);
      }
      auto item = [&](std::uint32_t k) {
        return typename BuildSession<V>::Item{
            KeyParts{keys[k], {}}, BuildSession<V>::payload_bytes_of(kids[k])};
      };
      auto *inner = session_.pack(
          false, static_cast<std::uint32_t>(keys.size()), item, prefix);
      inner->first_child = first;
      add_child(lvl + 1, inner, up_sep);
      i = j;
    }
    levels_[lvl] = Level{};
  }

  friend class PersistentBTree<V>;
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
  // its inputs and starts a lineage of its own. Consuming, matching the
  // radix tree's contract: the inputs are taken by value and released here,
  // so one engine call site serves either key directory.
  template <typename ResolveFunc>
  [[nodiscard]] static auto merge(PersistentBTree a, PersistentBTree b,
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

  // Up to `n` keys that cut the tree into roughly equal parts, in ascending
  // order. Taken from the inner separators, which sit one per leaf boundary:
  // the sample is uniform in leaves without reading a single leaf, and the
  // leaves of a bulk-loaded tree hold within a few percent of each other, so
  // it is uniform in keys too. A tree small enough to be one leaf has no
  // separators, so its own keys are sampled instead — the same cut points,
  // one level down. The result is separators, not necessarily keys of the
  // tree: use them as range bounds, not as lookups.
  [[nodiscard]] auto sample_separators(std::size_t n) const
      -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> out;
    if (!root_ || n == 0)
      return out;
    if (root_->is_leaf)
      return sample_leaf_keys(root_, n);
    std::size_t total = 0;
    count_separators(root_, total);
    if (total == 0)
      return out;
    // total separators sit between total + 1 leaves. Cutting those leaves
    // into `groups` even runs puts the j-th cut at separator index
    // j * (total + 1) / groups - 1.
    const auto groups = std::min(n + 1, total + 1);
    std::vector<std::size_t> targets;
    targets.reserve(groups - 1);
    for (std::size_t j = 1; j < groups; ++j)
      targets.push_back(j * (total + 1) / groups - 1);
    out.reserve(targets.size());
    std::size_t seen = 0;
    std::size_t next = 0;
    emit_separators(root_, targets, seen, next, out);
    return out;
  }

  // -- Test and debug support --------------------------------------------

  // Every retired node still waiting on a live version.
  [[nodiscard]] static auto parked_nodes() -> std::vector<const void *> {
    return chain().parked_nodes();
  }
  // Live versions and parked retired nodes, for stats().
  [[nodiscard]] static auto reclamation_gauges() {
    return chain().gauges();
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
  [[nodiscard]] auto stats() const -> BTreeStats {
    BTreeStats st;
    std::vector<std::pair<const N *, std::size_t>> stack;
    if (root_)
      stack.emplace_back(root_, 1);
    while (!stack.empty()) {
      const auto [n, depth] = stack.back();
      stack.pop_back();
      ++st.nodes;
      st.capacity_bytes += n->capacity;
      st.used_bytes += n->capacity - n->free_bytes();
      st.dead_bytes += n->dead_bytes;
      st.height = std::max(st.height, depth);
      if (n->is_leaf) {
        ++st.leaves;
        st.entries += n->count;
        st.leaf_counts.push_back(n->count);
        st.leaf_prefix_lens.push_back(n->prefix_len);
        st.leaf_free_bytes.push_back(static_cast<std::uint32_t>(n->free_bytes()));
        st.leaf_prefix_bytes += n->prefix_len;
        for (std::uint32_t i = 0; i < n->count; ++i)
          st.leaf_suffix_bytes += n->suffix(i).size();
        continue;
      }
      n->for_each_child([&](N *c) { stack.emplace_back(c, depth + 1); });
    }
    return st;
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

  friend class btree_detail::BulkLoader<V>;

  static auto chain() -> Chain & { return Chain::instance(); }

  // A single-leaf tree: its own keys are the only cut points available. The
  // first key is skipped, so every cut leaves something below it.
  [[nodiscard]] static auto sample_leaf_keys(const N *leaf, std::size_t n)
      -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> out;
    if (leaf->count < 2)
      return out;
    const std::size_t cuts = leaf->count - 1;
    const auto groups = std::min(n + 1, cuts + 1);
    out.reserve(groups - 1);
    for (std::size_t j = 1; j < groups; ++j) {
      const auto i = static_cast<std::uint32_t>(j * (cuts + 1) / groups);
      const auto pre = leaf->prefix();
      const auto suf = leaf->suffix(i);
      std::vector<std::byte> key;
      key.reserve(pre.size() + suf.size());
      key.insert(key.end(), pre.begin(), pre.end());
      key.insert(key.end(), suf.begin(), suf.end());
      out.push_back(std::move(key));
    }
    return out;
  }

  static void count_separators(const N *n, std::size_t &total) noexcept {
    if (n->is_leaf)
      return;
    total += n->count;
    n->for_each_child([&](N *c) { count_separators(c, total); });
  }

  // In-order walk of the inner nodes: the separators of one node interleave
  // with its subtrees, so this yields them in ascending key order.
  static void emit_separators(const N *n,
                              const std::vector<std::size_t> &targets,
                              std::size_t &seen, std::size_t &next,
                              std::vector<std::vector<std::byte>> &out) {
    if (n->is_leaf || next == targets.size())
      return;
    for (std::uint32_t i = 0; i <= n->count; ++i) {
      emit_separators(n->child(i), targets, seen, next, out);
      if (i == n->count)
        break;
      if (next < targets.size() && seen == targets[next]) {
        ++next;
        const auto pre = n->prefix();
        const auto suf = n->suffix(i);
        std::vector<std::byte> key;
        key.reserve(pre.size() + suf.size());
        key.insert(key.end(), pre.begin(), pre.end());
        key.insert(key.end(), suf.begin(), suf.end());
        out.push_back(std::move(key));
      }
      ++seen;
      if (next == targets.size())
        return;
    }
  }

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
auto PersistentBTree<V>::merge(PersistentBTree a, PersistentBTree b,
                               ResolveFunc &&resolve) -> PersistentBTree {
  // Ordered merge into a bulk loader: each key is written once, with no
  // descent and no split. Rebuilding with set() in a loop costs a full
  // descent per key and was the whole cost of recovery's fan-in.
  btree_detail::BulkLoader<V> out;
  auto ia = a.begin();
  auto ib = b.begin();
  while (ia != std::default_sentinel && ib != std::default_sentinel) {
    auto [ka, va] = *ia;
    auto [kb, vb] = *ib;
    const auto c = btree_detail::compare_bytes(ka, kb);
    if (c < 0) {
      out.append(ka, va);
      ++ia;
    } else if (c > 0) {
      out.append(kb, vb);
      ++ib;
    } else {
      out.append(ka, resolve(va, vb));
      ++ia;
      ++ib;
    }
  }
  for (; ia != std::default_sentinel; ++ia) {
    auto [k, v] = *ia;
    out.append(k, v);
  }
  for (; ib != std::default_sentinel; ++ib) {
    auto [k, v] = *ib;
    out.append(k, v);
  }
  auto merged = std::move(out).finish();
  // The iterators pin the inputs, so they are released only now.
  ia = {};
  ib = {};
  a = {};
  b = {};
  return merged;
}

} // namespace bytecask
