// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — lock-free adaptive radix tree for the in-memory key index

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module bytecask.radix_tree;

namespace bytecask {
// ---------------------------------------------------------------------------
// CompactPrefix
//
// Fixed 8-byte container for node prefix bytes. Stores up to 7 bytes inline.
// No heap allocation — prefixes longer than 7 bytes are split across a chain
// of routing nodes (each carrying up to 7 prefix bytes + 1 transition byte).
//
// sizeof(CompactPrefix) == 8, alignof(CompactPrefix) == 1.
// ---------------------------------------------------------------------------
class CompactPrefix {
public:
  static constexpr std::size_t kInlineCap = 7;

  CompactPrefix() noexcept = default;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }
  [[nodiscard]] auto data() noexcept -> std::byte * { return data_; }
  [[nodiscard]] auto data() const noexcept -> const std::byte * {
    return data_;
  }
  [[nodiscard]] auto operator[](std::size_t i) noexcept -> std::byte & {
    return data_[i];
  }
  [[nodiscard]] auto operator[](std::size_t i) const noexcept
      -> const std::byte & {
    return data_[i];
  }
  [[nodiscard]] auto begin() noexcept -> std::byte * { return data_; }
  [[nodiscard]] auto end() noexcept -> std::byte * { return data_ + size_; }
  [[nodiscard]] auto begin() const noexcept -> const std::byte * {
    return data_;
  }
  [[nodiscard]] auto end() const noexcept -> const std::byte * {
    return data_ + size_;
  }

  void push_back(std::byte b) {
    assert(size_ < kInlineCap);
    data_[size_++] = b;
  }

  void clear() { size_ = 0; }

private:
  std::uint8_t size_{0};
  std::byte data_[kInlineCap]{};
};
static_assert(sizeof(CompactPrefix) == 8);
static_assert(alignof(CompactPrefix) == 1);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
export template <typename V> class PersistentRadixTree;
export template <typename V> class TransientRadixTree;
export template <typename V> class RadixTreeIterator;
export template <typename V> class ReverseRadixTreeIterator;
export template <typename V> class ValueIterator;
export template <typename V> class ReverseValueIterator;

namespace detail {

// Global edit-tag counter for build sessions (transients and merges).
// Relaxed ordering: only uniqueness is required, not inter-thread visibility
// ordering. Each session gets a distinct tag via fetch_add. Tags are 60 bits
// wide (see Node::packed_tag_) so they never wrap in practice; the tag alone
// decides whether a node may be mutated in place, so it must be unique for
// the lifetime of the process.
inline std::atomic<std::uint64_t> next_edit_tag{1};

// Test-only node accounting (BYTECASK_RADIX_ACCOUNTING). Every node
// allocation and free is counted, per value type so that trees of
// different types cannot disturb each other's totals. `retired` tracks
// nodes the registry still owes a free to. The invariant the tree unit
// tests check is
//   allocated - freed == nodes reachable from live trees, plus parked ones.
export template <typename V> struct RadixAccounting {
  std::atomic<std::int64_t> allocated{0};
  std::atomic<std::int64_t> freed{0};
  std::atomic<std::int64_t> retired{0};
};
export template <typename V>
auto radix_accounting() -> RadixAccounting<V> & {
  static RadixAccounting<V> acc;
  return acc;
}
#ifdef BYTECASK_RADIX_ACCOUNTING
template <typename V> void account_alloc() noexcept {
  radix_accounting<V>().allocated.fetch_add(1, std::memory_order_relaxed);
}
template <typename V> void account_free() noexcept {
  radix_accounting<V>().freed.fetch_add(1, std::memory_order_relaxed);
}
template <typename V> void account_retired(std::int64_t delta) noexcept {
  radix_accounting<V>().retired.fetch_add(delta, std::memory_order_relaxed);
}
#else
template <typename V> void account_alloc() noexcept {}
template <typename V> void account_free() noexcept {}
template <typename V> void account_retired(std::int64_t) noexcept {}
#endif

} // namespace detail


// ---------------------------------------------------------------------------
// Node<V> — base type for all radix tree nodes (leaf and internal).
//
// packed_tag_ bit layout:
//   [63: has_value] [62-60: node_type] [59:0: edit_tag]
//
// Internal nodes are tiered by fanout to avoid dynamic-vector allocation
// overhead at the low fanout where most internal nodes actually live
// (routing/chain-compression nodes have exactly 1 child; splits start at 2),
// and to give a fixed, direct-mapped representation at the high end where a
// byte value maps directly onto a slot:
//
//   - Node4: up to 4 children embedded inline in the node itself — a single
//     allocation, no separate heap-allocated child store.
//   - Node16: up to 16 children, same shape as Node4 (sorted inline array,
//     linear scan) but a bigger inline array — still a single allocation.
//   - Node48: up to 48 children. Same sorted inline array as Node4/Node16
//     (so child_at(i) stays O(1), not a rescan), plus a 256-byte
//     byte->slot index (`child_index_`) that makes find_child(b) O(1)
//     instead of an O(n) scan — the point where a direct lookup starts to
//     matter more than the array's fixed cost. Still a single allocation.
//   - Node256: up to 256 children, direct-mapped by transition byte — no
//     stored key array (the byte value is the index). This is the terminal
//     tier: a transition is a single byte, so 256 slots is the ceiling on
//     fanout for any node, and there is no tier above it.
//
// A Node4 promotes to Node16 on its 5th child; Node16 promotes to Node48 on
// its 17th; Node48 promotes to Node256 on its 49th. Demotion uses
// proportional hysteresis on the two upper boundaries (matching DuckDB's
// ART): Node48 demotes to Node16 once its count drops to <= 12 (75% of
// Node16's capacity, not <= 16), and Node256 demotes to Node48 once its
// count drops to <= 36 (75% of Node48's capacity, not <= 48) — both avoid
// promote/demote thrashing right at the tier boundary. Node16 demotes to
// Node4 at <= 4, symmetrically (see Node::insert_child /
// Node::remove_child). 94% of nodes overall are leaves, which carry no
// children field at all — sizeof(Node) = 40 bytes (glibc usable=40) for
// KeyDirEntry, instead of 48 bytes (glibc usable=56) if every node paid for
// a children field it never used.
//
// Safety: children storage does not exist on Node — accessing a derived
// tier's fields through a Node* is a compile error. Child accessor methods
// check node_type() before downcasting; an unexpected type returns a safe
// default ("no children"), never memory corruption.
// ---------------------------------------------------------------------------
template <typename V> struct Node4;        // forward declaration (4-slot tier)
template <typename V> struct Node16;       // forward declaration (16-slot tier)
template <typename V> struct Node48;       // forward declaration (48-slot tier)
template <typename V> struct Node256;      // forward declaration (256-slot tier)

template <typename V> struct Node {
  enum class NodeType : std::uint32_t {
    Leaf = 0,
    Node4 = 1,
    Node256 = 2,
    Node16 = 3,
    Node48 = 4,
  };

  // packed_tag_ bit layout: [63: has_value] [62-60: node_type] [59:0: edit_tag]
  //
  // No reference count: nodes are owned by the version history, not by the
  // pointers to them. A node is freed either by the session that created it
  // (when that session discards it before publishing) or by the epoch
  // registry once every version that could reach it is gone — see
  // EpochRegistry. Child slots are therefore plain pointers, and cloning a
  // node is a memcpy of its child array.
  static constexpr std::uint64_t kHasValueBit = 1ull << 63;
  static constexpr unsigned kNodeTypeShift = 60u;
  static constexpr std::uint64_t kNodeTypeMask = 0x7ull << kNodeTypeShift;
  // Set while this node sits on some build session's retired list, so a
  // second session that supersedes the same shared node does not retire it
  // twice. Outside kTagMask, so a tier promotion that carries the edit tag
  // across does not carry this with it.
  static constexpr std::uint64_t kRetiredBit = 1ull << 59;
  static constexpr std::uint64_t kFlagBits =
      kHasValueBit | kNodeTypeMask | kRetiredBit;
  static constexpr std::uint64_t kTagMask = kRetiredBit - 1;

  // Atomic only so that marking a node retired is not a data race against
  // lock-free readers walking the version it still belongs to: the retired
  // bit shares this word with the node type and has-value flag. Every
  // access is relaxed — the word carries no ordering, just its own bits —
  // which on every target this builds for is a plain load or store, and it
  // keeps the node exactly as large as a plain uint64_t would.
  std::atomic<std::uint64_t> packed_tag_{0};
  V value_{};

  using Prefix = CompactPrefix;
  Prefix prefix;

  // -- Child nested types (returned by child_at/find_child) --
  //
  // `transition` is a value, not a reference: Node256 has no stored key
  // array (the transition byte *is* the index into children_), so there is
  // no lvalue byte to bind a reference to for that tier. `ptr` stays a
  // real reference — callers do assign through `.ptr` (e.g. splicing in a
  // merged child) — and every tier has an actual child slot to bind it to.
  struct ChildRef {
    std::byte transition;
    Node *&ptr;
  };
  struct ConstChildRef {
    std::byte transition;
    Node *const &ptr;
  };

  // The whole tag word. Every read and write of packed_tag_ goes through
  // these two.
  [[nodiscard]] auto tag_word() const noexcept -> std::uint64_t {
    return packed_tag_.load(std::memory_order_relaxed);
  }
  void set_tag_word(std::uint64_t w) noexcept {
    packed_tag_.store(w, std::memory_order_relaxed);
  }

  [[nodiscard]] auto has_value() const noexcept -> bool {
    return (tag_word() & kHasValueBit) != 0;
  }
  [[nodiscard]] auto node_type() const noexcept -> NodeType {
    return static_cast<NodeType>((tag_word() & kNodeTypeMask) >>
                                  kNodeTypeShift);
  }
  void set_node_type(NodeType t) noexcept {
    set_tag_word((tag_word() & ~kNodeTypeMask) |
                 (static_cast<std::uint64_t>(t) << kNodeTypeShift));
  }
  [[nodiscard]] auto edit_tag() const noexcept -> std::uint64_t {
    return tag_word() & kTagMask;
  }
  void set_edit_tag(std::uint64_t tag) noexcept {
    set_tag_word((tag_word() & kFlagBits) | (tag & kTagMask));
  }
  [[nodiscard]] auto is_retired() const noexcept -> bool {
    return (tag_word() & kRetiredBit) != 0;
  }
  // The one write to a node that outlives the session that created it, and
  // the reason this word is atomic. Only ever done by the thread that
  // retires the node, and only once (a node already marked is left alone),
  // so a plain relaxed store of the new value is enough.
  void mark_retired() noexcept { set_tag_word(tag_word() | kRetiredBit); }
  void clear_retired() noexcept { set_tag_word(tag_word() & ~kRetiredBit); }
  void set_value(V v) {
    value_ = std::move(v);
    set_tag_word(tag_word() | kHasValueBit);
  }
  void clear_value() noexcept { set_tag_word(tag_word() & ~kHasValueBit); }

  [[nodiscard]] auto as_node4() noexcept -> Node4<V> * {
    if (node_type() != NodeType::Node4)
      return nullptr;
    return static_cast<Node4<V> *>(this);
  }
  [[nodiscard]] auto as_node4() const noexcept -> const Node4<V> * {
    if (node_type() != NodeType::Node4)
      return nullptr;
    return static_cast<const Node4<V> *>(this);
  }
  [[nodiscard]] auto as_node16() noexcept -> Node16<V> * {
    if (node_type() != NodeType::Node16)
      return nullptr;
    return static_cast<Node16<V> *>(this);
  }
  [[nodiscard]] auto as_node16() const noexcept -> const Node16<V> * {
    if (node_type() != NodeType::Node16)
      return nullptr;
    return static_cast<const Node16<V> *>(this);
  }
  [[nodiscard]] auto as_node48() noexcept -> Node48<V> * {
    if (node_type() != NodeType::Node48)
      return nullptr;
    return static_cast<Node48<V> *>(this);
  }
  [[nodiscard]] auto as_node48() const noexcept -> const Node48<V> * {
    if (node_type() != NodeType::Node48)
      return nullptr;
    return static_cast<const Node48<V> *>(this);
  }
  [[nodiscard]] auto as_node256() noexcept -> Node256<V> * {
    if (node_type() != NodeType::Node256)
      return nullptr;
    return static_cast<Node256<V> *>(this);
  }
  [[nodiscard]] auto as_node256() const noexcept -> const Node256<V> * {
    if (node_type() != NodeType::Node256)
      return nullptr;
    return static_cast<const Node256<V> *>(this);
  }

  // -- Children accessors (dispatch on node_type()) --------------------------
  [[nodiscard]] auto child_count() const noexcept -> std::size_t {
    switch (node_type()) {
    case NodeType::Leaf:
      return 0;
    case NodeType::Node4:
      return static_cast<const Node4<V> *>(this)->count_;
    case NodeType::Node16:
      return static_cast<const Node16<V> *>(this)->count_;
    case NodeType::Node48:
      return static_cast<const Node48<V> *>(this)->count_;
    case NodeType::Node256:
      return static_cast<const Node256<V> *>(this)->count_;
    }
    // NodeType is exhaustively covered above; this is reachable only if
    // packed_tag_'s node-type bits are somehow corrupted to a stray value
    // outside the 5 declared enumerators. A safe default ("no children")
    // is a wrong-but-safe answer, not silent corruption — see the
    // Runtime safety guidance in .github/copilot-instructions.md.
    return 0;
  }
  [[nodiscard]] auto has_children() const noexcept -> bool {
    return child_count() != 0;
  }
  // Both child_at overloads and find_child/find_child_mut read node_type()
  // once and switch on it, rather than chaining as_node4()/as_node16()/
  // as_node48()/as_node256() — each of those independently re-derives
  // node_type() from packed_tag_, so chaining them means the common case
  // (a low tier) still pays for every check up to and including its own.
  // This redundant-dispatch pattern was flagged as a possible contributor
  // to Node256's original regression; folding it into one switch removed
  // a measurable few percent off RadixTree/Iterate here, though the
  // dominant cause of that regression was the inline_kids issue (see
  // Node::release() and §7.9).
  [[nodiscard]] auto child_at(std::size_t i) const -> ConstChildRef {
    switch (node_type()) {
    case NodeType::Leaf:
      assert(false && "child_at on a leaf");
      __builtin_unreachable();
    case NodeType::Node4: {
      auto *n4 = static_cast<const Node4<V> *>(this);
      assert(i < n4->count_);
      return {n4->keys_[i], n4->children_[i]};
    }
    case NodeType::Node16: {
      auto *n16 = static_cast<const Node16<V> *>(this);
      assert(i < n16->count_);
      return {n16->keys_[i], n16->children_[i]};
    }
    case NodeType::Node48: {
      auto *n48 = static_cast<const Node48<V> *>(this);
      assert(i < n48->count_);
      return {n48->keys_[i], n48->children_[i]};
    }
    case NodeType::Node256: {
      auto *n256 = static_cast<const Node256<V> *>(this);
      assert(i < n256->count_);
      for (std::size_t b = 0, seen = 0; b < 256; ++b) {
        if (!n256->children_[b])
          continue;
        if (seen == i)
          return {static_cast<std::byte>(b), n256->children_[b]};
        ++seen;
      }
      assert(false && "child_at(i) with i >= count_ on a Node256");
      __builtin_unreachable();
    }
    }
    __builtin_unreachable();
  }
  [[nodiscard]] auto child_at(std::size_t i) -> ChildRef {
    switch (node_type()) {
    case NodeType::Leaf:
      assert(false && "child_at on a leaf");
      __builtin_unreachable();
    case NodeType::Node4: {
      auto *n4 = static_cast<Node4<V> *>(this);
      assert(i < n4->count_);
      return {n4->keys_[i], n4->children_[i]};
    }
    case NodeType::Node16: {
      auto *n16 = static_cast<Node16<V> *>(this);
      assert(i < n16->count_);
      return {n16->keys_[i], n16->children_[i]};
    }
    case NodeType::Node48: {
      auto *n48 = static_cast<Node48<V> *>(this);
      assert(i < n48->count_);
      return {n48->keys_[i], n48->children_[i]};
    }
    case NodeType::Node256: {
      auto *n256 = static_cast<Node256<V> *>(this);
      assert(i < n256->count_);
      for (std::size_t b = 0, seen = 0; b < 256; ++b) {
        if (!n256->children_[b])
          continue;
        if (seen == i)
          return {static_cast<std::byte>(b), n256->children_[b]};
        ++seen;
      }
      assert(false && "child_at(i) with i >= count_ on a Node256");
      __builtin_unreachable();
    }
    }
    __builtin_unreachable();
  }

  [[nodiscard]] auto find_child(std::byte b) const
      -> std::optional<ConstChildRef> {
    switch (node_type()) {
    case NodeType::Leaf:
      return std::nullopt;
    case NodeType::Node4: {
      auto *n4 = static_cast<const Node4<V> *>(this);
      for (std::size_t i = 0; i < n4->count_; ++i)
        if (n4->keys_[i] == b)
          return ConstChildRef{n4->keys_[i], n4->children_[i]};
      return std::nullopt;
    }
    case NodeType::Node16: {
      auto *n16 = static_cast<const Node16<V> *>(this);
      for (std::size_t i = 0; i < n16->count_; ++i)
        if (n16->keys_[i] == b)
          return ConstChildRef{n16->keys_[i], n16->children_[i]};
      return std::nullopt;
    }
    case NodeType::Node48: {
      auto *n48 = static_cast<const Node48<V> *>(this);
      auto pos = n48->child_index_[std::to_integer<std::uint8_t>(b)];
      if (pos == Node48<V>::kEmptyMarker)
        return std::nullopt;
      return ConstChildRef{n48->keys_[pos], n48->children_[pos]};
    }
    case NodeType::Node256: {
      auto *n256 = static_cast<const Node256<V> *>(this);
      auto idx = std::to_integer<std::uint8_t>(b);
      if (!n256->children_[idx])
        return std::nullopt;
      return ConstChildRef{b, n256->children_[idx]};
    }
    }
    // See child_count()'s trailing comment — safe default, not UB.
    return std::nullopt;
  }

  [[nodiscard]] auto find_child_mut(std::byte b) -> std::optional<ChildRef> {
    switch (node_type()) {
    case NodeType::Leaf:
      return std::nullopt;
    case NodeType::Node4: {
      auto *n4 = static_cast<Node4<V> *>(this);
      for (std::size_t i = 0; i < n4->count_; ++i)
        if (n4->keys_[i] == b)
          return ChildRef{n4->keys_[i], n4->children_[i]};
      return std::nullopt;
    }
    case NodeType::Node16: {
      auto *n16 = static_cast<Node16<V> *>(this);
      for (std::size_t i = 0; i < n16->count_; ++i)
        if (n16->keys_[i] == b)
          return ChildRef{n16->keys_[i], n16->children_[i]};
      return std::nullopt;
    }
    case NodeType::Node48: {
      auto *n48 = static_cast<Node48<V> *>(this);
      auto pos = n48->child_index_[std::to_integer<std::uint8_t>(b)];
      if (pos == Node48<V>::kEmptyMarker)
        return std::nullopt;
      return ChildRef{n48->keys_[pos], n48->children_[pos]};
    }
    case NodeType::Node256: {
      auto *n256 = static_cast<Node256<V> *>(this);
      auto idx = std::to_integer<std::uint8_t>(b);
      if (!n256->children_[idx])
        return std::nullopt;
      return ChildRef{b, n256->children_[idx]};
    }
    }
    // See child_count()'s trailing comment — safe default, not UB.
    return std::nullopt;
  }

  // Cursor for an ascending walk over one node's children. Each tier reads
  // whichever field is O(1) for its own layout — the packed tiers index
  // keys_/children_ by `ordinal`, Node256 scans its direct-mapped slots
  // from `probe` — and only next_child() ever writes either, so the two
  // cannot drift out of step.
  struct ChildCursor {
    std::size_t ordinal{0};
    unsigned probe{0};
  };

  // Next child in ascending transition-byte order, advancing the cursor past
  // it; nullopt once the children are exhausted. On return, cursor.ordinal
  // is the ordinal *after* the child yielded, so the ordinal of that child
  // is the value read from the cursor before the call.
  //
  // Prefer this over child_at(i) for any in-order traversal of a node's
  // children. A full walk here costs O(count) on the packed tiers and
  // O(256) on Node256; the same walk driven by child_at(i) is quadratic in
  // fanout on Node256, which has no packed key array and so has to rescan
  // its slots from zero on every call to turn an ordinal into a byte. That
  // cost is real and was measured, not theoretical: moving these walks off
  // child_at(i) made a single lower_bound() on a full-byte-range key shape
  // ~9x faster (9.8 us -> 1.0 us at 100k keys) and MergeOverlapping on that
  // shape ~18% faster, while leaving the packed tiers slightly faster too
  // (see §7.12). child_at(i) remains for random ordinal access — the
  // iterators' frames index by ordinal in both directions.
  [[nodiscard]] auto next_child(ChildCursor &cursor) const
      -> std::optional<ConstChildRef> {
    switch (node_type()) {
    case NodeType::Leaf:
      return std::nullopt;
    case NodeType::Node4: {
      auto *n4 = static_cast<const Node4<V> *>(this);
      if (cursor.ordinal >= n4->count_)
        return std::nullopt;
      auto i = cursor.ordinal++;
      return ConstChildRef{n4->keys_[i], n4->children_[i]};
    }
    case NodeType::Node16: {
      auto *n16 = static_cast<const Node16<V> *>(this);
      if (cursor.ordinal >= n16->count_)
        return std::nullopt;
      auto i = cursor.ordinal++;
      return ConstChildRef{n16->keys_[i], n16->children_[i]};
    }
    case NodeType::Node48: {
      auto *n48 = static_cast<const Node48<V> *>(this);
      if (cursor.ordinal >= n48->count_)
        return std::nullopt;
      auto i = cursor.ordinal++;
      return ConstChildRef{n48->keys_[i], n48->children_[i]};
    }
    case NodeType::Node256: {
      auto *n256 = static_cast<const Node256<V> *>(this);
      while (cursor.probe < 256) {
        auto slot = cursor.probe++;
        if (!n256->children_[slot])
          continue;
        ++cursor.ordinal;
        return ConstChildRef{static_cast<std::byte>(slot),
                             n256->children_[slot]};
      }
      return std::nullopt;
    }
    }
    // See child_count()'s trailing comment — safe default, not UB.
    return std::nullopt;
  }

  // -- Allocation and destruction ------------------------------------------
  //
  // Every node allocation goes through alloc<T>() and every free through
  // destroy(), so the test-only accounting sees them all. Nodes have no
  // virtual destructor: destroy() dispatches on node_type() to delete the
  // tier the node was allocated as.
  template <typename T> [[nodiscard]] static auto alloc() -> T * {
    detail::account_alloc<V>();
    return new T();
  }

  static void destroy(Node *n) noexcept {
    switch (n->node_type()) {
    case NodeType::Leaf:
      delete n;
      break;
    case NodeType::Node4:
      delete static_cast<Node4<V> *>(n);
      break;
    case NodeType::Node16:
      delete static_cast<Node16<V> *>(n);
      break;
    case NodeType::Node48:
      delete static_cast<Node48<V> *>(n);
      break;
    case NodeType::Node256:
      delete static_cast<Node256<V> *>(n);
      break;
    }
    detail::account_free<V>();
  }

  // Allocates a Node4 carrying src's value/prefix (not its children — src
  // is assumed to have none, and not its edit_tag — matches clone()'s
  // tag-free contract). Shared by clone_as_internal() and by
  // insert_child/remove_child's tier transitions, which OR the tag back in
  // themselves since (unlike clone()) they must preserve it.
  [[nodiscard]] static auto make_node4_like(const Node &src) -> Node4<V> * {
    auto *n = alloc<Node4<V>>();
    n->set_tag_word(n->tag_word() | (src.tag_word() & kHasValueBit));
    n->value_ = src.value_;
    n->prefix = src.prefix;
    return n;
  }
  // Allocates an (empty) Node16 node carrying src's value/prefix (tag-free
  // — see make_node4_like). Shared by insert_child's Node4 -> Node16
  // promotion and remove_child's Node48 -> Node16 demotion.
  [[nodiscard]] static auto make_node16_like(const Node &src) -> Node16<V> * {
    auto *n = alloc<Node16<V>>();
    n->set_tag_word(n->tag_word() | (src.tag_word() & kHasValueBit));
    n->value_ = src.value_;
    n->prefix = src.prefix;
    return n;
  }
  // Allocates an (empty) Node48 node carrying src's value/prefix (tag-free —
  // see make_node4_like). Shared by insert_child's Node16 -> Node48
  // promotion and remove_child's Node256 -> Node48 demotion. child_index_
  // is default-constructed to all-kEmptyMarker by Node48's constructor.
  [[nodiscard]] static auto make_node48_like(const Node &src) -> Node48<V> * {
    auto *n = alloc<Node48<V>>();
    n->set_tag_word(n->tag_word() | (src.tag_word() & kHasValueBit));
    n->value_ = src.value_;
    n->prefix = src.prefix;
    return n;
  }
  // Allocates an (empty) Node256 node carrying src's value/prefix (tag-free
  // — see make_node4_like). Shared by insert_child's Node48 -> Node256
  // promotion. children_ is default-constructed to all-null.
  [[nodiscard]] static auto make_node256_like(const Node &src)
      -> Node256<V> * {
    auto *n = alloc<Node256<V>>();
    n->set_tag_word(n->tag_word() | (src.tag_word() & kHasValueBit));
    n->value_ = src.value_;
    n->prefix = src.prefix;
    return n;
  }

  // Insert a child under transition byte b. Handles every promotion: a Leaf
  // promotes to Node4 for its first child; a Node4 at capacity promotes to
  // Node16 on its 5th; a Node16 at capacity promotes to Node48 on its
  // 17th; a Node48 at capacity promotes to Node256 on its 49th. Node256 is
  // the terminal tier — 256 slots is the ceiling on fanout for a single
  // byte transition, so it never promotes further.
  //
  // `node` must be owned by `session` (it is mutated in place). When a
  // promotion replaces it, the narrower node is handed to
  // session.discard(), which frees it at once. Returns the resulting node,
  // which may be a different allocation.
  template <typename Session>
  [[nodiscard]] static auto insert_child(Session &session, Node *node,
                                         std::byte b, Node *child) -> Node * {
    if (auto *n4 = node->as_node4()) {
      if (n4->count_ < Node4<V>::kCapacity) {
        std::size_t pos = 0;
        while (pos < n4->count_ && n4->keys_[pos] < b)
          ++pos;
        assert((pos == n4->count_ || n4->keys_[pos] != b) &&
               "duplicate transition byte");
        for (std::size_t i = n4->count_; i > pos; --i) {
          n4->keys_[i] = n4->keys_[i - 1];
          n4->children_[i] = n4->children_[i - 1];
        }
        n4->keys_[pos] = b;
        n4->children_[pos] = child;
        ++n4->count_;
        return node;
      }
      // Node4 at capacity — promote to Node16. Unlike clone(), a tier
      // transition must preserve the source node's edit_tag: `node` was
      // already claimed by the current session (via ensure_mutable) before
      // insert_child was called.
      auto *n16 = make_node16_like(*node);
      n16->set_tag_word(n16->tag_word() | (node->tag_word() & kTagMask));
      n16->count_ = n4->count_;
      for (std::size_t i = 0; i < n4->count_; ++i) {
        n16->keys_[i] = n4->keys_[i];
        n16->children_[i] = n4->children_[i];
      }
      session.discard(node);
      return insert_child(session, n16, b, child);
    }

    if (auto *n16 = node->as_node16()) {
      if (n16->count_ < Node16<V>::kCapacity) {
        std::size_t pos = 0;
        while (pos < n16->count_ && n16->keys_[pos] < b)
          ++pos;
        assert((pos == n16->count_ || n16->keys_[pos] != b) &&
               "duplicate transition byte");
        for (std::size_t i = n16->count_; i > pos; --i) {
          n16->keys_[i] = n16->keys_[i - 1];
          n16->children_[i] = n16->children_[i - 1];
        }
        n16->keys_[pos] = b;
        n16->children_[pos] = child;
        ++n16->count_;
        return node;
      }
      // Node16 at capacity — promote to Node48. Preserve edit_tag (see the
      // Node4 -> Node16 branch above). child_index_ starts all-empty (set
      // by Node48's constructor) and is filled in below alongside keys_.
      auto *n48 = make_node48_like(*node);
      n48->set_tag_word(n48->tag_word() | (node->tag_word() & kTagMask));
      n48->count_ = n16->count_;
      for (std::size_t i = 0; i < n16->count_; ++i) {
        n48->keys_[i] = n16->keys_[i];
        n48->children_[i] = n16->children_[i];
        n48->child_index_[std::to_integer<std::uint8_t>(n16->keys_[i])] =
            static_cast<std::uint8_t>(i);
      }
      session.discard(node);
      return insert_child(session, n48, b, child);
    }

    if (auto *n48 = node->as_node48()) {
      if (n48->count_ < Node48<V>::kCapacity) {
        std::size_t pos = 0;
        while (pos < n48->count_ && n48->keys_[pos] < b)
          ++pos;
        assert((pos == n48->count_ || n48->keys_[pos] != b) &&
               "duplicate transition byte");
        for (std::size_t i = n48->count_; i > pos; --i) {
          n48->keys_[i] = n48->keys_[i - 1];
          n48->children_[i] = n48->children_[i - 1];
          n48->child_index_[std::to_integer<std::uint8_t>(n48->keys_[i])] =
              static_cast<std::uint8_t>(i);
        }
        n48->keys_[pos] = b;
        n48->children_[pos] = child;
        n48->child_index_[std::to_integer<std::uint8_t>(b)] =
            static_cast<std::uint8_t>(pos);
        ++n48->count_;
        return node;
      }
      // Node48 at capacity — promote to Node256. Preserve edit_tag (see the
      // Node4 -> Node16 branch above). children_ starts all-null (set by
      // Node256's constructor) and is filled in below by transition byte.
      auto *n256 = make_node256_like(*node);
      n256->set_tag_word(n256->tag_word() | (node->tag_word() & kTagMask));
      n256->count_ = n48->count_;
      for (std::size_t i = 0; i < n48->count_; ++i) {
        n256->children_[std::to_integer<std::uint8_t>(n48->keys_[i])] =
            n48->children_[i];
      }
      session.discard(node);
      return insert_child(session, n256, b, child);
    }

    if (node->node_type() == NodeType::Leaf) {
      // First child — promote leaf to Node4, preserving edit_tag (see the
      // Node4 -> Node16 branch above for why).
      auto *n4 = make_node4_like(*node);
      n4->set_tag_word(n4->tag_word() | (node->tag_word() & kTagMask));
      session.discard(node);
      return insert_child(session, n4, b, child);
    }

    // Node256 — the terminal tier. Direct-mapped by transition byte, so
    // insertion is O(1) with no shifting and no further promotion.
    auto *n256 = node->as_node256();
    assert(n256 != nullptr);
    assert(!n256->children_[std::to_integer<std::uint8_t>(b)] &&
           "duplicate transition byte");
    n256->children_[std::to_integer<std::uint8_t>(b)] = child;
    ++n256->count_;
    return node;
  }

  // Remove the child under transition byte b. If this is Node256 and
  // removing drops its count to <= Node256<V>::kShrinkThreshold (75% of
  // Node48's capacity, matching DuckDB's ART — hysteresis to avoid
  // thrashing right at the 48/49 promotion boundary), demotes to Node48;
  // if this is Node48 and its count drops to <= Node48<V>::kShrinkThreshold
  // (75% of Node16's capacity, same rationale, for the 16/17 boundary),
  // demotes to Node16; if this is Node16 and its count drops to <=
  // Node4<V>::kCapacity, demotes to Node4 (symmetric — no hysteresis needed
  // at this boundary, see §7.7). `node` must be owned by `session`; a
  // demoted-from node is handed to session.discard(). The removed child is
  // the caller's to discard. Returns the resulting node.
  template <typename Session>
  [[nodiscard]] static auto remove_child(Session &session, Node *node,
                                         std::byte b) -> Node * {
    if (auto *n4 = node->as_node4()) {
      for (std::size_t i = 0; i < n4->count_; ++i) {
        if (n4->keys_[i] == b) {
          for (std::size_t j = i; j + 1 < n4->count_; ++j) {
            n4->keys_[j] = n4->keys_[j + 1];
            n4->children_[j] = n4->children_[j + 1];
          }
          --n4->count_;
          n4->children_[n4->count_] = nullptr;
          return node;
        }
      }
      return node;
    }

    if (auto *n16 = node->as_node16()) {
      for (std::size_t i = 0; i < n16->count_; ++i) {
        if (n16->keys_[i] == b) {
          for (std::size_t j = i; j + 1 < n16->count_; ++j) {
            n16->keys_[j] = n16->keys_[j + 1];
            n16->children_[j] = n16->children_[j + 1];
          }
          --n16->count_;
          n16->children_[n16->count_] = nullptr;
          break;
        }
      }
      if (n16->count_ > Node4<V>::kCapacity)
        return node;
      // Demote back to Node4 — Node16's per-slot cost only pays off above
      // this fanout. Preserve edit_tag (see insert_child's Node4 -> Node16
      // branch for why).
      auto *n4 = make_node4_like(*node);
      n4->set_tag_word(n4->tag_word() | (node->tag_word() & kTagMask));
      n4->count_ = static_cast<std::uint8_t>(n16->count_);
      for (std::size_t i = 0; i < n16->count_; ++i) {
        n4->keys_[i] = n16->keys_[i];
        n4->children_[i] = n16->children_[i];
      }
      session.discard(node);
      return n4;
    }

    if (auto *n48 = node->as_node48()) {
      auto pos = n48->child_index_[std::to_integer<std::uint8_t>(b)];
      if (pos != Node48<V>::kEmptyMarker) {
        for (std::size_t j = pos; j + 1 < n48->count_; ++j) {
          n48->keys_[j] = n48->keys_[j + 1];
          n48->children_[j] = n48->children_[j + 1];
          n48->child_index_[std::to_integer<std::uint8_t>(n48->keys_[j])] =
              static_cast<std::uint8_t>(j);
        }
        --n48->count_;
        n48->children_[n48->count_] = nullptr;
        n48->child_index_[std::to_integer<std::uint8_t>(b)] =
            Node48<V>::kEmptyMarker;
      }
      if (n48->count_ > Node48<V>::kShrinkThreshold)
        return node;
      // Demote back to Node16 — see the comment above remove_child for the
      // hysteresis rationale.
      auto *n16 = make_node16_like(*node);
      n16->set_tag_word(n16->tag_word() | (node->tag_word() & kTagMask));
      n16->count_ = static_cast<std::uint8_t>(n48->count_);
      for (std::size_t i = 0; i < n48->count_; ++i) {
        n16->keys_[i] = n48->keys_[i];
        n16->children_[i] = n48->children_[i];
      }
      session.discard(node);
      return n16;
    }

    // Node256 — direct-mapped, so removal is just clearing the slot; no
    // shifting, unlike the packed lower tiers.
    auto *n256 = node->as_node256();
    if (!n256)
      return node;
    auto idx = std::to_integer<std::uint8_t>(b);
    if (n256->children_[idx]) {
      n256->children_[idx] = nullptr;
      --n256->count_;
    }
    // 75% of Node48's capacity — DuckDB's proportional hysteresis, mirrors
    // Node48's own demotion threshold (see the comment above remove_child).
    if (n256->count_ > Node256<V>::kShrinkThreshold)
      return node;

    // Demote back to Node48.
    auto *n48 = make_node48_like(*node);
    n48->set_tag_word(n48->tag_word() | (node->tag_word() & kTagMask));
    std::uint8_t pos = 0;
    for (std::size_t i = 0; i < 256; ++i) {
      if (!n256->children_[i])
        continue;
      n48->keys_[pos] = static_cast<std::byte>(i);
      n48->children_[pos] = n256->children_[i];
      n48->child_index_[i] = pos;
      ++pos;
    }
    n48->count_ = pos;
    session.discard(node);
    return n48;
  }

  // Shallow clone of this node: same tier, value, prefix and child
  // pointers (children are shared, not copied — a plain array copy, no
  // per-child work). The clone carries no edit tag; the caller stamps it.
  [[nodiscard]] auto clone() const -> Node * {
    if (auto *self = as_node4()) {
      auto *n = alloc<Node4<V>>();
      n->set_tag_word(n->tag_word() | (tag_word() & kHasValueBit));
      n->value_ = value_;
      n->prefix = prefix;
      n->count_ = self->count_;
      n->keys_ = self->keys_;
      n->children_ = self->children_;
      return n;
    }
    if (auto *self = as_node16()) {
      auto *n = alloc<Node16<V>>();
      n->set_tag_word(n->tag_word() | (tag_word() & kHasValueBit));
      n->value_ = value_;
      n->prefix = prefix;
      n->count_ = self->count_;
      n->keys_ = self->keys_;
      n->children_ = self->children_;
      return n;
    }
    if (auto *self = as_node48()) {
      auto *n = alloc<Node48<V>>();
      n->set_tag_word(n->tag_word() | (tag_word() & kHasValueBit));
      n->value_ = value_;
      n->prefix = prefix;
      n->count_ = self->count_;
      n->keys_ = self->keys_;
      n->children_ = self->children_;
      n->child_index_ = self->child_index_;
      return n;
    }
    if (auto *self = as_node256()) {
      auto *n = alloc<Node256<V>>();
      n->set_tag_word(n->tag_word() | (tag_word() & kHasValueBit));
      n->value_ = value_;
      n->prefix = prefix;
      n->count_ = self->count_;
      n->children_ = self->children_;
      return n;
    }
    auto *n = alloc<Node>();
    n->set_tag_word(n->tag_word() | (tag_word() & kHasValueBit));
    n->value_ = value_;
    n->prefix = prefix;
    return n;
  }
};

template <typename V> struct Node4 : Node<V> {
  static constexpr std::uint8_t kCapacity = 4;

  Node4() { Node<V>::set_node_type(Node<V>::NodeType::Node4); }
  std::uint8_t count_{0};
  std::array<std::byte, kCapacity> keys_{};
  std::array<Node<V> *, kCapacity> children_{};
};

// ---------------------------------------------------------------------------
// Node16<V> — fixed 16-slot internal node, same shape as Node4 (sorted
// inline array, linear scan) but a bigger array. Still a single
// allocation. Promotes to Node48 on its 17th child; Node48 demotes back to
// Node16 when its count drops to <= kShrinkThreshold (see
// Node::insert_child / Node::remove_child).
// ---------------------------------------------------------------------------
template <typename V> struct Node16 : Node<V> {
  static constexpr std::uint8_t kCapacity = 16;

  Node16() { Node<V>::set_node_type(Node<V>::NodeType::Node16); }
  std::uint8_t count_{0};
  std::array<std::byte, kCapacity> keys_{};
  std::array<Node<V> *, kCapacity> children_{};
};

// ---------------------------------------------------------------------------
// Node48<V> — fixed 48-slot internal node. Keeps the same sorted inline
// array as Node4/Node16 (keys_/children_) so child_at(i) stays an O(1)
// ordinal lookup — no rescan — plus a 256-byte child_index_ that maps a
// transition byte directly to its slot, making find_child(b) O(1) instead
// of Node16's O(count) linear scan. kEmptyMarker (== kCapacity, an
// otherwise-unused slot value) marks a byte with no child, mirroring
// DuckDB's ART Node48::EMPTY_MARKER. Promotes to Node256 on its 49th
// child; demotes back to Node16 when its count drops to <=
// kShrinkThreshold (see Node::insert_child / Node::remove_child).
// ---------------------------------------------------------------------------
template <typename V> struct Node48 : Node<V> {
  static constexpr std::uint8_t kCapacity = 48;
  static constexpr std::uint8_t kEmptyMarker = kCapacity;
  // 75% of Node16::kCapacity — DuckDB's proportional hysteresis, avoids
  // promote/demote thrashing right at the 16/17 boundary.
  static constexpr std::uint8_t kShrinkThreshold = 12;

  Node48() {
    Node<V>::set_node_type(Node<V>::NodeType::Node48);
    child_index_.fill(kEmptyMarker);
  }
  std::uint8_t count_{0};
  std::array<std::uint8_t, 256> child_index_{};
  std::array<std::byte, kCapacity> keys_{};
  std::array<Node<V> *, kCapacity> children_{};
};

// ---------------------------------------------------------------------------
// Node256<V> — fixed 256-slot internal node, direct-mapped by transition
// byte: children_[b] is the child for byte b, no stored key array (the
// byte value *is* the index), so find_child(b) and insertion/removal are
// O(1). child_at(i) — the ordinal accessor iteration uses — has no packed
// array to index into and scans children_ for the i-th occupied slot; this
// is the terminal tier (256 slots is the ceiling on fanout for a single
// byte transition), so there is no tier above it to promote to.
// count_ is uint16_t, not uint8_t: a full node holds 256 children, which
// does not fit in 8 bits. Demotes to Node48 when its count drops to <=
// kShrinkThreshold (see Node::insert_child / Node::remove_child).
// ---------------------------------------------------------------------------
template <typename V> struct Node256 : Node<V> {
  static constexpr std::uint16_t kCapacity = 256;
  // 75% of Node48::kCapacity — DuckDB's proportional hysteresis, avoids
  // promote/demote thrashing right at the 48/49 boundary.
  static constexpr std::uint16_t kShrinkThreshold = 36;

  Node256() { Node<V>::set_node_type(Node<V>::NodeType::Node256); }
  std::uint16_t count_{0};
  std::array<Node<V> *, kCapacity> children_{};
};

// Node<uint64_t>: 8+8+8 = 24 (no children field; no pointers, so this
// holds regardless of the target's pointer size).
static_assert(sizeof(Node<std::uint64_t>) == 24);

// The four tiers below embed Node* child arrays, so their sizes
// scale with the target's pointer width: 8 bytes/child on 64-bit hosts
// (native, MariaDB plugin, Python bindings), 4 bytes/child on wasm32 (the
// Node.js WASM backend, built with -DBYTECASK_SINGLE_THREADED). Both are
// asserted explicitly rather than skipping the check on non-64-bit targets.
//
// Node4<uint64_t>: 24 + 1(count) + 4(keys, padded) + 4*children = 64 (8B
// children) or 48 (4B children).
static_assert(sizeof(Node4<std::uint64_t>) == (sizeof(void *) == 8 ? 64 : 48));
// Node16<uint64_t>: 24 + 1(count) + 16(keys) + pad + 16*children = 176 (8B
// children) or 112 (4B children).
static_assert(sizeof(Node16<std::uint64_t>) ==
              (sizeof(void *) == 8 ? 176 : 112));
// Node48<uint64_t>: 24 + 1(count) + 256(child_index) + 48(keys) + pad +
// 48*children = 720 (8B children) or 528 (4B children).
static_assert(sizeof(Node48<std::uint64_t>) ==
              (sizeof(void *) == 8 ? 720 : 528));
// Node256<uint64_t>: 24 + 2(count) + pad + 256*children = 2080 (8B children)
// or 1056 (4B children).
static_assert(sizeof(Node256<std::uint64_t>) ==
              (sizeof(void *) == 8 ? 2080 : 1056));


// ---------------------------------------------------------------------------
// Helper: compute the common prefix length between a node's prefix and a key
// slice.
// ---------------------------------------------------------------------------
inline auto common_prefix_length(std::span<const std::byte> a,
                                 std::span<const std::byte> b) -> std::size_t {
  auto len = std::min(a.size(), b.size());
  std::size_t i = 0;
  while (i < len && a[i] == b[i])
    ++i;
  return i;
}

// Walks the subtree under `root`, descending through every node for which
// `is_garbage(node)` holds and freeing those of them that are not on some
// build session's retired list (whoever retired a node frees it). The
// predicate must be monotone along a path — once it is false for a node it
// is false for everything below it — which holds for both predicates used
// here, because a node's children are never newer than the node itself: a
// session links new children only into nodes it owns. Iterative — no
// recursion on tree depth.
template <typename V, typename Pred>
void free_subtree_if(Node<V> *root, Pred is_garbage) {
  if (!root || !is_garbage(root))
    return;
  std::vector<Node<V> *> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    auto *n = stack.back();
    stack.pop_back();
    typename Node<V>::ChildCursor cursor;
    while (auto child = n->next_child(cursor)) {
      if (is_garbage(child->ptr))
        stack.push_back(child->ptr);
    }
    if (!n->is_retired())
      Node<V>::destroy(n);
  }
}

// ---------------------------------------------------------------------------
// VersionRegistry<V> — owns node lifetime for every PersistentRadixTree<V>.
//
// A persistent tree is a *version*. Each version is identified by the edit
// tag of the session that built it, and is registered here while any handle
// to it lives. The session retires the base nodes it makes unreachable —
// the ones it clones or unlinks — and hands that list over when it
// publishes. Nothing carries a reference count; freeing is a batch of plain
// deletes off the write path.
//
// When can a retired node be freed? Tags increase with every session, and a
// session starts only after its base was published, so a node created by
// session S is reachable only from the version S published and from
// versions derived from it — all of which have tags >= S. A node X retired
// by session R is, by definition, not reachable from R's version nor from
// anything derived from it. So X is reachable from a live version V exactly
// when tag(V) lies in [tag(X), tag(R)).
//
// Each retired node is therefore parked on the live version that currently
// blocks it — the smallest live tag at or above its own — and freed the
// moment no live tag falls in its window. A version's death re-examines
// only the nodes parked on it, so the cost is paid once per node per
// blocker, never as a scan. A long-lived snapshot holds exactly the nodes
// it can still reach, not everything retired since it was taken.
//
// Forks. The window argument assumes a version's tag orders it against
// everything that can reach its nodes, which holds while versions form a
// chain. Two versions derived from the same base — a fork — break it: each
// can reach what the other superseded while carrying a larger tag. Parking
// is therefore suspended while any base has more than one version derived
// from it, and the held nodes are placed as soon as that resolves. The
// engine forks only between a failed flush and the resume() that clears it.
//
// A version whose last handle goes while nothing was derived from it frees
// what only it could reach: walking down from its root, a node whose tag is
// above every version it was derived from was created after all of them,
// so nothing else can reach it. A node's children are never newer than the
// node, so the walk stops where the test fails, and parked nodes are
// stepped over because their blocker frees them.
//
// That floor is what the derivation graph is for. A dead version is spliced
// out and its successor inherits its bases, which lowers the successor's
// floor onto the nodes it now holds alone — but only while at most one
// version was derived from it. A dead version with two or more successors
// stays as a tombstone: they share its nodes with each other, and it is
// what stops either one's walk from freeing what the other still reads.
// When their number falls back to one, it is spliced out like any other and
// the survivor takes over its nodes. So the last version of a lineage has
// no bases left and frees the whole structure.
//
// Reclamation runs on the thread that drops the last handle. All registry
// state is under one mutex, and the freeing of retired nodes happens after
// it is released; the walk above is the exception and holds it, because
// once a version leaves the graph another thread may decide that what it
// reached is free while the walk is still stepping through those nodes.
//
// There are only ever a handful of versions, so they live in flat vectors
// searched linearly rather than in node-based containers: records sorted by
// tag, the derivation graph as a list of base-to-successor pairs, and the
// retired nodes kept in the vector the session already built. Publishing a
// version then allocates nothing once those buffers have settled, which
// matters because the engine publishes three of them per batch. The buffers
// are handed back when the last version of this value type goes.
// ---------------------------------------------------------------------------
template <typename V> class VersionRegistry {
public:
  static auto instance() -> VersionRegistry & {
    // Immortal: a tree can outlive static destruction, so the registry is
    // constructed once in static storage and never destroyed. Static
    // storage rather than the heap, so a leak checker sees nothing left
    // behind at exit.
    alignas(VersionRegistry) static std::byte storage[sizeof(VersionRegistry)];
    static auto *registry = new (storage) VersionRegistry();
    return *registry;
  }

  // Registers a new version built by session `tag` from `bases` (tag 0 = the
  // empty tree, which owns nothing) and returns its id. On success `retired`
  // is taken over and left empty; on a throw it is untouched and still
  // belongs to the session. Bases must be live — the builder holds a handle
  // to each.
  [[nodiscard]] auto publish(std::uint64_t tag,
                             std::span<const std::uint64_t> bases,
                             std::vector<Node<V> *> &retired)
      -> std::uint64_t {
    std::vector<Node<V> *> to_free;
    {
      std::lock_guard<std::mutex> lk{mu_};
      records_.insert(seat_for(tag), Record{.tag = tag, .live = 1});
      for (auto b : bases) {
        if (b != 0)
          add_edge(b, tag);
      }
      if (!retired.empty())
        pending_.push_back(Parcel{std::move(retired), tag});
      retired.clear();
      drain_pending(to_free);
    }
    destroy_all(to_free);
    return tag;
  }

  void pin(std::uint64_t id) {
    std::lock_guard<std::mutex> lk{mu_};
    ++find(id)->live;
  }

  // Drops one handle of `id`. `root` is walked only when this was the last
  // handle and no version was derived from it — see the class comment for
  // the tag test that decides what such a walk frees.
  void unpin(std::uint64_t id, Node<V> *root) {
    std::vector<Node<V> *> to_free;
    {
      std::lock_guard<std::mutex> lk{mu_};
      auto *rec = find(id);
      if (--rec->live > 0)
        return;
      bool walk_root = true;
      std::uint64_t floor = 0;
      // Ids are session tags, so the largest base id is the newest state
      // this version still shares with something else.
      for (const auto &e : edges_) {
        if (e.base == id)
          walk_root = false;
        else if (e.successor == id)
          floor = std::max(floor, e.base);
      }
      // Under the lock: once this version leaves the graph another thread
      // may decide that what it reached is free, and this walk is still
      // stepping through those nodes to reach the ones below them. Walks
      // are rare — only a version nothing was derived from has one — so
      // holding the lock across it costs nothing in the steady state.
      if (walk_root)
        free_subtree_if<V>(root, [floor](Node<V> *n) {
          return n->edit_tag() > floor;
        });
      // Nodes parked on this version may now be free, or may have to move
      // to an older version that still reaches them.
      take_parcels(*rec);
      collapse(id, to_free);
      drain_pending(to_free);
      release_buffers_if_idle();
    }
    destroy_all(to_free);
  }

  // Test-only: every retired node still waiting for a live version to
  // release it.
  [[nodiscard]] auto parked_nodes() -> std::vector<const void *> {
    std::lock_guard<std::mutex> lk{mu_};
    std::vector<const void *> out;
    auto add = [&out](const Parcel &parcel) {
      out.insert(out.end(), parcel.nodes.begin(), parcel.nodes.end());
    };
    for (const auto &rec : records_) {
      add(rec.parked);
      for (const auto &parcel : rec.more)
        add(parcel);
    }
    for (const auto &parcel : forked_)
      add(parcel);
    for (const auto &parcel : pending_)
      add(parcel);
    return out;
  }

private:
  // base -> successor. Also flat: the same handful of versions, and reading
  // it is a scan of a few contiguous pairs.
  struct Edge {
    std::uint64_t base{0};
    std::uint64_t successor{0};
  };

  // Nodes retired by one session. `retired_by` closes their reachability
  // window: a live version with a tag at or above it was derived from that
  // session and cannot reach them.
  struct Parcel {
    std::vector<Node<V> *> nodes;
    std::uint64_t retired_by{0};
  };

  // One version. Live versions and the tombstones described in the class
  // comment, kept in one vector sorted by tag: there are a handful of them,
  // so a flat vector searches faster than a node-based container and, once
  // its capacity has settled, publishing a version allocates nothing.
  struct Record {
    std::uint64_t tag{0};
    std::uint32_t live{0};
    // Nodes parked on this version, because it is the live version that
    // still reaches them. One parcel inline covers the usual case — one
    // published version's worth of superseded nodes — so parking allocates
    // nothing; further ones spill into `more`.
    Parcel parked;
    std::vector<Parcel> more;
  };

  std::mutex mu_;
  std::uint32_t forks_{0}; // versions with more than one successor
  std::vector<Record> records_;
  std::vector<Edge> edges_;
  std::vector<Parcel> forked_;  // held while a fork is unresolved
  // Scratch reused under mu_, so the steady state allocates nothing: a
  // published version's bookkeeping is pushes into vectors that already
  // have the capacity.
  std::vector<Parcel> pending_;
  std::vector<std::uint64_t> work_;
  std::vector<std::uint64_t> bases_;
  std::vector<std::uint64_t> successors_;

  // Where `tag` belongs in the sorted record vector.
  [[nodiscard]] auto seat_for(std::uint64_t tag) -> typename std::vector<Record>::iterator {
    return std::lower_bound(records_.begin(), records_.end(), tag,
                            [](const Record &r, std::uint64_t t) {
                              return r.tag < t;
                            });
  }
  [[nodiscard]] auto find(std::uint64_t tag) -> Record * {
    auto it = seat_for(tag);
    assert(it != records_.end() && it->tag == tag);
    return &*it;
  }

  [[nodiscard]] auto successor_count(std::uint64_t tag) const -> std::size_t {
    std::size_t n = 0;
    for (const auto &e : edges_)
      if (e.base == tag)
        ++n;
    return n;
  }

  void add_edge(std::uint64_t base, std::uint64_t successor) {
    if (successor_count(base) == 1)
      ++forks_;
    edges_.push_back(Edge{base, successor});
  }

  void remove_edge(std::uint64_t base, std::uint64_t successor) {
    auto it = std::find_if(edges_.begin(), edges_.end(),
                           [&](const Edge &e) {
                             return e.base == base && e.successor == successor;
                           });
    if (it == edges_.end())
      return;
    edges_.erase(it);
    if (successor_count(base) == 1 && --forks_ == 0) {
      pending_.insert(pending_.end(), std::make_move_iterator(forked_.begin()),
                      std::make_move_iterator(forked_.end()));
      forked_.clear();
    }
  }

  // Under mu_. Removes every dead record that no longer holds two or more
  // successors apart, starting at `id`. See the class comment for why a
  // dead version with two successors has to stay.
  void collapse(std::uint64_t id, std::vector<Node<V> *> & /*out*/) {
    work_.clear();
    work_.push_back(id);
    while (!work_.empty()) {
      const auto cur = work_.back();
      work_.pop_back();
      auto it = seat_for(cur);
      if (it == records_.end() || it->tag != cur || it->live > 0)
        continue;
      bases_.clear();
      successors_.clear();
      for (const auto &e : edges_) {
        if (e.successor == cur)
          bases_.push_back(e.base);
        else if (e.base == cur)
          successors_.push_back(e.successor);
      }
      if (successors_.size() > 1)
        continue; // tombstone: it keeps its successors' nodes apart
      take_parcels(*it);
      records_.erase(it);
      for (auto b : bases_) {
        // Connect before disconnecting: dropping to one successor first
        // would briefly read as "no forks left" and release parked nodes
        // that the successor taking this one's place still blocks.
        for (auto s : successors_)
          add_edge(b, s);
        remove_edge(b, cur);
      }
      for (auto s : successors_)
        remove_edge(cur, s);
      work_.insert(work_.end(), bases_.begin(), bases_.end());
    }
  }

  // Under mu_: moves everything parked on `rec` onto the pending queue, to
  // be placed again once the graph has settled.
  void take_parcels(Record &rec) {
    if (!rec.parked.nodes.empty()) {
      pending_.push_back(std::move(rec.parked));
      rec.parked.nodes.clear();
    }
    if (!rec.more.empty()) {
      pending_.insert(pending_.end(),
                      std::make_move_iterator(rec.more.begin()),
                      std::make_move_iterator(rec.more.end()));
      rec.more.clear();
    }
  }

  // Under mu_: places every pending parcel. Runs after the record vector
  // has stopped moving, so park() can hold a reference into it.
  void drain_pending(std::vector<Node<V> *> &out) {
    while (!pending_.empty()) {
      auto parcel = std::move(pending_.back());
      pending_.pop_back();
      park(std::move(parcel), out);
    }
  }

  // The live version that still reaches a node retired by `retired_by`: the
  // smallest live tag at or above the node's own, when that is below
  // `retired_by`. Returns 0 when nothing reaches it.
  [[nodiscard]] auto blocker_for(std::uint64_t node_tag,
                                 std::uint64_t retired_by) const
      -> std::uint64_t {
    auto it = std::lower_bound(records_.begin(), records_.end(), node_tag,
                               [](const Record &r, std::uint64_t t) {
                                 return r.tag < t;
                               });
    for (; it != records_.end() && it->tag < retired_by; ++it)
      if (it->live > 0)
        return it->tag;
    return 0;
  }

  // Under mu_: sends each node of `parcel` to the live version that still
  // reaches it, or to `out` when none does. The nodes of one parcel almost
  // always share a blocker — they came off one path in one version — so
  // that case moves the whole list and allocates nothing.
  void park(Parcel parcel, std::vector<Node<V> *> &out) {
    if (forks_ > 0) {
      forked_.push_back(std::move(parcel));
      return;
    }
    const auto first = blocker_for(parcel.nodes.front()->edit_tag(),
                                   parcel.retired_by);
    bool uniform = true;
    for (auto *n : parcel.nodes) {
      if (blocker_for(n->edit_tag(), parcel.retired_by) != first) {
        uniform = false;
        break;
      }
    }
    if (uniform) {
      if (first == 0)
        out.insert(out.end(), parcel.nodes.begin(), parcel.nodes.end());
      else
        hold(first, std::move(parcel));
      return;
    }
    // Mixed: split by blocker. Rare, and the split lists are short.
    std::vector<Parcel> groups;
    for (auto *n : parcel.nodes) {
      const auto blocker = blocker_for(n->edit_tag(), parcel.retired_by);
      if (blocker == 0) {
        out.push_back(n);
        continue;
      }
      auto group = std::find_if(groups.begin(), groups.end(),
                                [blocker](const Parcel &g) {
                                  return g.retired_by == blocker;
                                });
      if (group == groups.end())
        groups.push_back(Parcel{{n}, blocker});
      else
        group->nodes.push_back(n);
    }
    for (auto &group : groups) {
      const auto blocker = group.retired_by;
      hold(blocker, Parcel{std::move(group.nodes), parcel.retired_by});
    }
  }

  // Under mu_: hands a parcel to the version that still reaches it.
  void hold(std::uint64_t blocker, Parcel parcel) {
    auto *rec = find(blocker);
    if (rec->parked.nodes.empty())
      rec->parked = std::move(parcel);
    else
      rec->more.push_back(std::move(parcel));
  }

  // Under mu_: when the last version of every tree of this value type is
  // gone, hand the registry's own buffers back too. They are reused and
  // never shrink otherwise, which is what makes publishing allocation-free
  // in the steady state — but an idle process should not be holding them,
  // and the tree is then provably responsible for no memory at all, which
  // the memory tests check.
  void release_buffers_if_idle() {
    if (!records_.empty() || !edges_.empty() || !forked_.empty() ||
        !pending_.empty())
      return;
    records_.shrink_to_fit();
    edges_.shrink_to_fit();
    forked_.shrink_to_fit();
    pending_.shrink_to_fit();
    work_.shrink_to_fit();
    bases_.shrink_to_fit();
    successors_.shrink_to_fit();
  }

  static void destroy_all(const std::vector<Node<V> *> &nodes) noexcept {
    if (nodes.empty())
      return;
    detail::account_retired<V>(-static_cast<std::int64_t>(nodes.size()));
    for (auto *n : nodes)
      Node<V>::destroy(n);
  }
};

// ---------------------------------------------------------------------------
// BuildSession<V> — one build of a new version from a base.
//
// Owns the edit tag that marks the nodes it creates and the list of base
// nodes it retires. Every structural algorithm lives here so the four
// places a node becomes garbage all go through discard():
//   1. superseded — mutable_copy() clones a node it does not own;
//   2. replaced — insert_child / remove_child / merge_with_child swap a
//      node for a different tier or collapse a routing node;
//   3. dropped — erase unlinks a node that has no value and no children;
//   4. never published — the session is destroyed without publishing:
//      the nodes it created are freed by walking the root (see
//      TransientRadixTree), and its retired list is forgotten.
// A node the session owns (edit tag == tag_) is freed at once — nothing
// outside the session can reach it. A foreign node is retired: it stays
// allocated until every version that can reach it is gone.
//
// Used by one thread at a time. Iterators over a session's root hold raw
// node pointers: do not mutate while one is alive.
// ---------------------------------------------------------------------------
template <typename V> class BuildSession {
public:
  using N = Node<V>;

  BuildSession() : tag_{new_tag()} {}
  BuildSession(const BuildSession &) = delete;
  auto operator=(const BuildSession &) -> BuildSession & = delete;
  BuildSession(BuildSession &&other) noexcept
      : tag_{std::exchange(other.tag_, 0)},
        retired_{std::move(other.retired_)} {
    other.retired_.clear();
  }
  auto operator=(BuildSession &&other) noexcept -> BuildSession & {
    if (this != &other) {
      forget_retired();
      tag_ = std::exchange(other.tag_, 0);
      retired_ = std::move(other.retired_);
      other.retired_.clear();
    }
    return *this;
  }
  ~BuildSession() { forget_retired(); }

  [[nodiscard]] auto tag() const noexcept -> std::uint64_t { return tag_; }
  [[nodiscard]] auto owns(const N *n) const noexcept -> bool {
    return n->edit_tag() == tag_;
  }

  // The retired list, for EpochRegistry::publish to take over on success.
  [[nodiscard]] auto retired_list() noexcept -> std::vector<N *> & {
    return retired_;
  }

  // Ends the session after publishing: nothing it created is owned by it
  // any more, and the retired list now belongs to the registry.
  void finish() noexcept {
    tag_ = 0;
    retired_.clear();
  }

  // Frees every node this session created that is still reachable from
  // `root`, and forgets the retired list. For a session that is dropped
  // without publishing: the base version is intact, so nothing it retired
  // is garbage, and nothing it created is reachable from anywhere else.
  void discard_all(N *root) noexcept {
    const auto tag = tag_;
    if (tag != 0)
      free_subtree_if<V>(root, [tag](N *n) { return n->edit_tag() == tag; });
    forget_retired();
    tag_ = 0;
  }

  void discard(N *n) {
    if (owns(n)) {
      // Created by this session and never published: nothing outside can
      // reach it.
      N::destroy(n);
      return;
    }
    if (n->is_retired())
      return; // another session already owns its disposal
    n->mark_retired();
    retired_.push_back(n);
    detail::account_retired<V>(1);
  }

  // The node itself if this session owns it, else a clone stamped with the
  // session's tag; the original is retired. Null yields a fresh leaf.
  [[nodiscard]] auto mutable_copy(N *node) -> N * {
    if (!node)
      return make_leaf();
    if (owns(node))
      return node;
    auto *n = node->clone();
    n->set_edit_tag(tag_);
    discard(node);
    return n;
  }

  [[nodiscard]] auto make_leaf() -> N * {
    auto *n = N::template alloc<N>();
    n->set_edit_tag(tag_);
    return n;
  }
  [[nodiscard]] auto make_internal() -> N * {
    auto *n = N::template alloc<Node4<V>>();
    n->set_edit_tag(tag_);
    return n;
  }

  // -- chain builders --
  // Build a chain of routing nodes ending in a value-bearing leaf.
  // Chunks key left-to-right: each intermediate node gets 7 prefix bytes +
  // 1 transition byte (8 bytes of key material per hop). The final leaf
  // gets the remaining 0–7 bytes as prefix.
  auto build_leaf_chain(std::span<const std::byte> key, V val) -> N * {
    // Fast path: key fits in a single node's prefix.
    if (key.size() <= CompactPrefix::kInlineCap) {
      auto *leaf = make_leaf();
      for (auto b : key)
        leaf->prefix.push_back(b);
      leaf->set_value(std::move(val));
      return leaf;
    }

    // Partition key into chunks of 7 prefix + 1 transition byte.
    // Collect chunk boundaries first, then build bottom-up.
    struct Chunk {
      std::size_t prefix_start;
      std::size_t prefix_len;
      std::size_t transition_idx; // index of transition byte (unused for last)
    };
    std::vector<Chunk> chunks;
    std::size_t pos = 0;
    while (key.size() - pos > CompactPrefix::kInlineCap) {
      chunks.push_back({pos, CompactPrefix::kInlineCap, pos + CompactPrefix::kInlineCap});
      pos += CompactPrefix::kInlineCap + 1; // 7 prefix + 1 transition
    }
    // Last chunk: remaining 0–7 bytes become the leaf's prefix.
    auto leaf_prefix = key.subspan(pos);

    // Build bottom-up: leaf first, then wrap in routing nodes.
    auto *cur = make_leaf();
    for (auto b : leaf_prefix)
      cur->prefix.push_back(b);
    cur->set_value(std::move(val));

    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
      auto *routing = make_internal();
      for (std::size_t i = 0; i < it->prefix_len; ++i)
        routing->prefix.push_back(key[it->prefix_start + i]);
      cur = N::insert_child(*this, routing, key[it->transition_idx], cur);
    }
    return cur;
  }

  // Build a chain of routing nodes with an existing terminal node at the end.
  // Overwrites terminal->prefix with the last chunk. Leaves terminal's
  // children and value intact. `terminal` must be owned by this session.
  auto build_routing_chain(std::span<const std::byte> merged, N *terminal)
      -> N * {
    assert(owns(terminal));
    // Fast path: fits in a single prefix.
    if (merged.size() <= CompactPrefix::kInlineCap) {
      terminal->prefix.clear();
      for (auto b : merged)
        terminal->prefix.push_back(b);
      return terminal;
    }

    // Partition into chunks.
    struct Chunk {
      std::size_t prefix_start;
      std::size_t prefix_len;
      std::size_t transition_idx;
    };
    std::vector<Chunk> chunks;
    std::size_t pos = 0;
    while (merged.size() - pos > CompactPrefix::kInlineCap) {
      chunks.push_back({pos, CompactPrefix::kInlineCap, pos + CompactPrefix::kInlineCap});
      pos += CompactPrefix::kInlineCap + 1;
    }

    // Set terminal's prefix to the last chunk.
    terminal->prefix.clear();
    for (auto b : merged.subspan(pos))
      terminal->prefix.push_back(b);

    // Build bottom-up: terminal is the innermost node.
    auto *cur = terminal;
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
      auto *routing = make_internal();
      for (std::size_t i = 0; i < it->prefix_len; ++i)
        routing->prefix.push_back(merged[it->prefix_start + i]);
      cur = N::insert_child(*this, routing, merged[it->transition_idx], cur);
    }
    return cur;
  }

  // -- set: mutates owned nodes in place, copies foreign ones --
  // Returns {new subtree root, whether a new key was inserted}.
  auto set(N *node, std::span<const std::byte> key, V val)
      -> std::pair<N *, bool> {
    if (!node)
      return {build_leaf_chain(key, std::move(val)), true};

    auto *mutable_node = mutable_copy(node);
    auto prefix_span = std::span<const std::byte>{mutable_node->prefix.data(),
                                                  mutable_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split: divergence within this node's prefix.
      auto *split = make_internal();
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(prefix_span[i]);

      auto old_transition = prefix_span[cpl];
      typename N::Prefix old_suffix;
      for (std::size_t i = cpl + 1; i < prefix_span.size(); ++i)
        old_suffix.push_back(prefix_span[i]);
      mutable_node->prefix = std::move(old_suffix);
      split = N::insert_child(*this, split, old_transition, mutable_node);

      auto remaining = key.subspan(cpl);
      if (remaining.empty()) {
        split->set_value(std::move(val));
      } else {
        auto new_transition = remaining[0];
        auto *chain = build_leaf_chain(remaining.subspan(1), std::move(val));
        split = N::insert_child(*this, split, new_transition, chain);
      }
      return {split, true};
    }

    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      bool was_absent = !mutable_node->has_value();
      mutable_node->set_value(std::move(val));
      return {mutable_node, was_absent};
    }

    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing_child = mutable_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, inserted] =
          set(existing_child->ptr, child_key, std::move(val));
      existing_child->ptr = new_child;
      return {mutable_node, inserted};
    }
    // insert_child promotes mutable_node (leaf -> Node4 -> Node16 ->
    // Node48 -> Node256) as needed.
    auto *chain = build_leaf_chain(child_key, std::move(val));
    mutable_node = N::insert_child(*this, mutable_node, transition, chain);
    return {mutable_node, true};
  }

  // Single-traversal upsert — like set, but conditionally replaces an
  // existing value. Returns {new subtree root, displaced value, inserted}.
  // When the key already exists, calls should_replace(existing, incoming);
  // if true, swaps in the new value and returns the old one as displaced.
  template <typename Pred>
  auto upsert(N *node, std::span<const std::byte> key, V val,
              Pred &&should_replace)
      -> std::tuple<N *, std::optional<V>, bool> {
    if (!node)
      return {build_leaf_chain(key, std::move(val)), std::nullopt, true};

    auto *mutable_node = mutable_copy(node);
    auto prefix_span = std::span<const std::byte>{mutable_node->prefix.data(),
                                                  mutable_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split — key diverges from prefix, so this is always a new insert.
      auto *split = make_internal();
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(prefix_span[i]);

      auto old_transition = prefix_span[cpl];
      typename N::Prefix old_suffix;
      for (std::size_t i = cpl + 1; i < prefix_span.size(); ++i)
        old_suffix.push_back(prefix_span[i]);
      mutable_node->prefix = std::move(old_suffix);
      split = N::insert_child(*this, split, old_transition, mutable_node);

      auto remaining = key.subspan(cpl);
      if (remaining.empty()) {
        split->set_value(std::move(val));
      } else {
        auto new_transition = remaining[0];
        auto *chain = build_leaf_chain(remaining.subspan(1), std::move(val));
        split = N::insert_child(*this, split, new_transition, chain);
      }
      return {split, std::nullopt, true};
    }

    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      if (mutable_node->has_value()) {
        if (should_replace(mutable_node->value_, val)) {
          auto old = std::move(mutable_node->value_);
          mutable_node->set_value(std::move(val));
          return {mutable_node, std::move(old), false};
        }
        return {mutable_node, std::nullopt, false};
      }
      mutable_node->set_value(std::move(val));
      return {mutable_node, std::nullopt, true};
    }

    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing_child = mutable_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, displaced, inserted] =
          upsert(existing_child->ptr, child_key, std::move(val),
                 std::forward<Pred>(should_replace));
      existing_child->ptr = new_child;
      return {mutable_node, std::move(displaced), inserted};
    }
    auto *chain = build_leaf_chain(child_key, std::move(val));
    mutable_node = N::insert_child(*this, mutable_node, transition, chain);
    return {mutable_node, std::nullopt, true};
  }

  // -- erase with path compression --
  // Returns {new subtree root (null if the subtree is gone), removed}.
  // Only nodes on a path that actually changes are copied: a node that
  // disappears entirely (a leaf, or a routing node whose only child is
  // going away) is discarded without a clone.
  auto erase(N *node, std::span<const std::byte> key)
      -> std::pair<N *, bool> {
    if (!node)
      return {nullptr, false};

    auto prefix_span =
        std::span<const std::byte>{node->prefix.data(), node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size())
      return {node, false};

    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      if (!node->has_value())
        return {node, false};
      if (!node->has_children()) {
        discard(node);
        return {nullptr, true};
      }
      auto *mutable_node = mutable_copy(node);
      mutable_node->clear_value();
      if (mutable_node->child_count() == 1)
        return {merge_with_child(mutable_node), true};
      return {mutable_node, true};
    }

    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing = node->find_child(transition);
    if (!existing)
      return {node, false};

    auto [new_child, removed] = erase(existing->ptr, child_key);
    if (!removed)
      return {node, false};

    if (!new_child) {
      if (!node->has_value() && node->child_count() == 1) {
        // A routing node whose only child is gone goes with it.
        discard(node);
        return {nullptr, true};
      }
      auto *mutable_node = mutable_copy(node);
      mutable_node = N::remove_child(*this, mutable_node, transition);
      if (!mutable_node->has_value() && mutable_node->child_count() == 1)
        return {merge_with_child(mutable_node), true};
      return {mutable_node, true};
    }
    auto *mutable_node = mutable_copy(node);
    mutable_node->find_child_mut(transition)->ptr = new_child;
    return {mutable_node, true};
  }

  // Merge a routing node (owned, no value, one child) into that child.
  // New prefix = node.prefix + transition_byte + child.prefix; if the
  // combined prefix exceeds 7 bytes, builds a chain of routing nodes.
  auto merge_with_child(N *node) -> N * {
    assert(owns(node) && !node->has_value() && node->child_count() == 1);
    auto slot = node->child_at(0);
    auto transition = slot.transition;
    auto *child = mutable_copy(slot.ptr);

    auto total = node->prefix.size() + 1 + child->prefix.size();
    if (total <= CompactPrefix::kInlineCap) {
      typename N::Prefix merged_prefix;
      for (std::size_t i = 0; i < node->prefix.size(); ++i)
        merged_prefix.push_back(node->prefix[i]);
      merged_prefix.push_back(transition);
      for (std::size_t i = 0; i < child->prefix.size(); ++i)
        merged_prefix.push_back(child->prefix[i]);
      child->prefix = std::move(merged_prefix);
      discard(node);
      return child;
    }
    std::vector<std::byte> merged;
    merged.reserve(total);
    for (std::size_t i = 0; i < node->prefix.size(); ++i)
      merged.push_back(node->prefix[i]);
    merged.push_back(transition);
    for (std::size_t i = 0; i < child->prefix.size(); ++i)
      merged.push_back(child->prefix[i]);
    discard(node);
    return build_routing_chain(std::span<const std::byte>{merged}, child);
  }

  // -- merge of two subtrees --
  // Recursively merges the subtrees rooted at `a` and `b`. Disjoint
  // subtrees are shared in O(1) (no clone). Every node of either input that
  // the result does not reuse is retired. Returns {merged_root,
  // overlap_count} where overlap_count is the number of keys present in
  // both a and b (i.e. where resolve was called).
  template <typename ResolveFunc>
  auto merge(N *a, N *b, ResolveFunc &&resolve)
      -> std::pair<N *, std::size_t> {
    if (!a)
      return {b, 0};
    if (!b)
      return {a, 0};

    // Align the two nodes on their common prefix.
    auto pa = std::span<const std::byte>{a->prefix.data(), a->prefix.size()};
    auto pb = std::span<const std::byte>{b->prefix.data(), b->prefix.size()};
    auto cpl = common_prefix_length(pa, pb);

    if (cpl < pa.size() && cpl < pb.size()) {
      // The two prefixes diverge — build a split node with the common prefix,
      // then place trimmed a and trimmed b as its two children.
      //
      // Every byte of pa and pb this branch needs is copied out first:
      // mutable_copy may hand back the node itself (when this session owns
      // it), and rewriting its prefix below would otherwise change the
      // bytes pa/pb point at.
      const auto a_transition = pa[cpl];
      const auto b_transition = pb[cpl];
      typename N::Prefix a_suffix;
      for (std::size_t i = cpl + 1; i < pa.size(); ++i)
        a_suffix.push_back(pa[i]);
      typename N::Prefix b_suffix;
      for (std::size_t i = cpl + 1; i < pb.size(); ++i)
        b_suffix.push_back(pb[i]);

      auto *split = make_internal();
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(pa[i]);

      auto *a_trimmed = mutable_copy(a);
      a_trimmed->prefix = std::move(a_suffix);
      split = N::insert_child(*this, split, a_transition, a_trimmed);

      auto *b_trimmed = mutable_copy(b);
      b_trimmed->prefix = std::move(b_suffix);
      split = N::insert_child(*this, split, b_transition, b_trimmed);

      return {split, 0};
    }

    if (cpl < pa.size()) {
      // b's prefix is fully consumed — b's node sits *above* a in the trie.
      // Build result based on b; insert a under b at transition pa[cpl].
      // Copied out before any prefix is rewritten in place — see the
      // diverging branch above.
      const auto a_transition = pa[cpl];
      typename N::Prefix a_suffix;
      for (std::size_t i = cpl + 1; i < pa.size(); ++i)
        a_suffix.push_back(pa[i]);

      auto *new_b = mutable_as_internal(b);
      auto *a_trimmed = mutable_copy(a);
      a_trimmed->prefix = std::move(a_suffix);

      auto existing = new_b->find_child_mut(a_transition);
      std::size_t overlaps = 0;
      if (existing) {
        auto [child, child_overlaps] =
            merge(a_trimmed, existing->ptr, resolve);
        existing->ptr = child;
        overlaps = child_overlaps;
      } else {
        new_b = N::insert_child(*this, new_b, a_transition, a_trimmed);
      }
      return {new_b, overlaps};
    }

    if (cpl < pb.size()) {
      // a's prefix is fully consumed — a's node sits *above* b in the trie.
      // Build result based on a; insert b under a at transition pb[cpl].
      // Copied out before any prefix is rewritten in place — see the
      // diverging branch above.
      const auto b_transition = pb[cpl];
      typename N::Prefix b_suffix;
      for (std::size_t i = cpl + 1; i < pb.size(); ++i)
        b_suffix.push_back(pb[i]);

      auto *new_a = mutable_as_internal(a);
      auto *b_trimmed = mutable_copy(b);
      b_trimmed->prefix = std::move(b_suffix);

      auto existing = new_a->find_child_mut(b_transition);
      std::size_t overlaps = 0;
      if (existing) {
        auto [child, child_overlaps] =
            merge(existing->ptr, b_trimmed, resolve);
        existing->ptr = child;
        overlaps = child_overlaps;
      } else {
        new_a = N::insert_child(*this, new_a, b_transition, b_trimmed);
      }
      return {new_a, overlaps};
    }

    // Full prefix match — both nodes share the same compressed key prefix.
    // Fold b's value and children into a; b's node itself is not reused.
    auto *merged = mutable_as_internal(a);
    std::size_t overlaps = 0;

    if (b->has_value()) {
      if (merged->has_value()) {
        merged->set_value(resolve(merged->value_, b->value_));
        ++overlaps;
      } else {
        merged->set_value(b->value_);
      }
    }

    if (b->has_children()) {
      // Cursor walk rather than child_at(i) per ordinal: b's children are
      // visited in full here, and on a Node256 the ordinal accessor rescans
      // the direct-mapped slots on every call, making the walk quadratic in
      // fanout (see Node::next_child).
      typename N::ChildCursor cursor;
      while (auto b_slot = b->next_child(cursor)) {
        auto slot = merged->find_child_mut(b_slot->transition);
        if (slot) {
          auto [child, child_overlaps] =
              merge(slot->ptr, b_slot->ptr, resolve);
          slot->ptr = child;
          overlaps += child_overlaps;
        } else {
          // Disjoint subtree — share it in O(1), no clone needed.
          merged = N::insert_child(*this, merged, b_slot->transition,
                                   b_slot->ptr);
        }
      }
    }
    discard(b);

    return {merged, overlaps};
  }

private:
  std::uint64_t tag_{0};
  std::vector<N *> retired_;

  static auto new_tag() -> std::uint64_t {
    // Relaxed ordering: only uniqueness is required. Tags are truncated to
    // the 60-bit tag field; 0 is reserved for "no session".
    auto raw = detail::next_edit_tag.fetch_add(1, std::memory_order_relaxed);
    auto tag = raw & N::kTagMask;
    if (tag == 0) [[unlikely]]
      tag = detail::next_edit_tag.fetch_add(1, std::memory_order_relaxed) &
            N::kTagMask;
    return tag;
  }

  // Like mutable_copy, but a leaf becomes a Node4 so children can be added.
  [[nodiscard]] auto mutable_as_internal(N *node) -> N * {
    if (node->node_type() != N::NodeType::Leaf)
      return mutable_copy(node);
    auto *n = N::make_node4_like(*node);
    n->set_edit_tag(tag_);
    discard(node);
    return n;
  }

  // Gives the retired nodes back: they are still reachable from the base,
  // which outlives an unpublished session. Only for a session that never
  // published — a published one hands the list to the registry.
  void forget_retired() noexcept {
    if (retired_.empty())
      return;
    detail::account_retired<V>(-static_cast<std::int64_t>(retired_.size()));
    for (auto *n : retired_)
      n->clear_retired();
    retired_.clear();
  }
};

// Lookup shared by the persistent and transient trees. Uses raw pointers:
// the caller's handle (or session) keeps the whole subtree alive.
template <typename V>
auto radix_get_ptr(const Node<V> *cur, std::span<const std::byte> key) noexcept
    -> const V * {
  auto remaining = key;
  while (cur) {
    auto prefix_span =
        std::span<const std::byte>{cur->prefix.data(), cur->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, remaining);
    if (cpl < prefix_span.size())
      return nullptr;
    remaining = remaining.subspan(cpl);
    if (remaining.empty()) {
      if (cur->has_value())
        return &cur->value_;
      return nullptr;
    }
    auto transition = remaining[0];
    remaining = remaining.subspan(1);
    auto child = cur->find_child(transition);
    if (!child)
      return nullptr;
    cur = child->ptr;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// PersistentRadixTree<V>
//
// A handle to one immutable version. Copies are O(1): a root pointer, a
// size and a pin on the version. Node lifetime belongs to VersionRegistry;
// this class only pins and unpins. set() and erase() are one-operation
// transients, so every mutation goes through the same path.
// ---------------------------------------------------------------------------
export template <typename V> class PersistentRadixTree {
public:
  PersistentRadixTree() = default;
  PersistentRadixTree(const PersistentRadixTree &other)
      : root_{other.root_}, size_{other.size_}, version_{other.version_} {
    if (version_)
      registry().pin(version_);
  }
  auto operator=(const PersistentRadixTree &other) -> PersistentRadixTree & {
    if (this == &other)
      return *this;
    // Pin the new epoch before releasing the old one: if both are the same
    // version this keeps it alive throughout; if not, order does not matter.
    if (other.version_)
      registry().pin(other.version_);
    release();
    root_ = other.root_;
    size_ = other.size_;
    version_ = other.version_;
    return *this;
  }
  PersistentRadixTree(PersistentRadixTree &&other) noexcept
      : root_{std::exchange(other.root_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        version_{std::exchange(other.version_, 0)} {}
  auto operator=(PersistentRadixTree &&other) noexcept
      -> PersistentRadixTree & {
    if (this == &other)
      return *this;
    release();
    root_ = std::exchange(other.root_, nullptr);
    size_ = std::exchange(other.size_, 0);
    version_ = std::exchange(other.version_, 0);
    return *this;
  }
  ~PersistentRadixTree() { release(); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }

  [[nodiscard]] auto get(std::span<const std::byte> key) const
      -> std::optional<V> {
    auto *p = radix_get_ptr<V>(root_, key);
    if (!p)
      return std::nullopt;
    return *p;
  }

  // Returns a pointer to the value stored in the tree node, or nullptr.
  // Valid for the lifetime of this tree instance.
  [[nodiscard]] auto get_ptr(std::span<const std::byte> key) const noexcept
      -> const V * {
    return radix_get_ptr<V>(root_, key);
  }

  [[nodiscard]] auto contains(std::span<const std::byte> key) const -> bool {
    return get(key).has_value();
  }

  [[nodiscard]] auto set(std::span<const std::byte> key, V val) const
      -> PersistentRadixTree;

  [[nodiscard]] auto erase(std::span<const std::byte> key) const
      -> PersistentRadixTree;

  [[nodiscard]] auto transient() const -> TransientRadixTree<V>;

  // Merge two trees into a new version derived from both. On key conflicts,
  // resolve(a_val, b_val) picks the winner. Disjoint subtrees are shared in
  // O(1); nodes of either input the result does not reuse are retired and
  // freed once the inputs are gone. Size is computed inline as a.size() +
  // b.size() - overlaps (no post-merge walk).
  template <typename ResolveFunc>
  [[nodiscard]] static auto merge(const PersistentRadixTree &a,
                                  const PersistentRadixTree &b,
                                  ResolveFunc &&resolve)
      -> PersistentRadixTree;

  // Iteration
  [[nodiscard]] auto begin() const -> RadixTreeIterator<V>;
  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

  [[nodiscard]] auto rbegin() const -> ReverseRadixTreeIterator<V>;
  [[nodiscard]] auto rend() const noexcept -> std::default_sentinel_t {
    return {};
  }

  [[nodiscard]] auto lower_bound(std::span<const std::byte> key) const
      -> RadixTreeIterator<V>;
  [[nodiscard]] auto upper_bound(std::span<const std::byte> key) const
      -> RadixTreeIterator<V>;

  // Value-only iteration (no key construction — faster tree walk).
  [[nodiscard]] auto value_begin() const -> ValueIterator<V>;
  [[nodiscard]] auto value_lower_bound(std::span<const std::byte> key) const
      -> ValueIterator<V>;
  [[nodiscard]] auto value_rbegin() const -> ReverseValueIterator<V>;
  [[nodiscard]] auto value_rlower_bound(std::span<const std::byte> key) const
      -> ReverseValueIterator<V>;

  // Test-only: every node still held back by a live version, as opaque
  // identities. With for_each_node this gives the whole set of allocated
  // nodes, which the accounting counters must agree with.
  [[nodiscard]] static auto parked_nodes() -> std::vector<const void *> {
    return registry().parked_nodes();
  }

  // Test-only: visits every node reachable from this version once. Used
  // with detail::radix_accounting() to check the node accounting
  // invariant. O(nodes).
  template <typename Visit> void for_each_node(Visit visit) const {
    if (!root_)
      return;
    std::vector<const Node<V> *> stack{root_};
    while (!stack.empty()) {
      auto *n = stack.back();
      stack.pop_back();
      visit(n);
      typename Node<V>::ChildCursor cursor;
      while (auto child = n->next_child(cursor))
        stack.push_back(child->ptr);
    }
  }
private:
  // Returns an iterator-typed end sentinel for upper_bound() and as the
  // starting point for ReverseRadixTreeIterator construction. Not named
  // end() to avoid shadowing the cheaper default_sentinel_t overload used
  // in tight forward-iteration loops.
  [[nodiscard]] auto end_iter() const -> RadixTreeIterator<V>;

  Node<V> *root_{nullptr};
  std::size_t size_{0};
  std::uint64_t version_{0}; // 0: the empty tree, pins nothing

  static auto registry() -> VersionRegistry<V> & {
    return VersionRegistry<V>::instance();
  }

  // Adopts a freshly published version (the registry already counts this
  // handle).
  PersistentRadixTree(Node<V> *root, std::size_t sz, std::uint64_t version)
      : root_{root}, size_{sz}, version_{version} {}

  void release() noexcept {
    if (version_) {
      registry().unpin(version_, root_);
      version_ = 0;
    }
    root_ = nullptr;
    size_ = 0;
  }

  friend class TransientRadixTree<V>;
  friend class RadixTreeIterator<V>;
  friend class ValueIterator<V>;
  friend class ReverseValueIterator<V>;
};

// ---------------------------------------------------------------------------
// TransientRadixTree<V>
//
// Builder for the next version of a base tree. Holds a handle to the base
// (so the base cannot die under it) and a BuildSession. persistent()
// publishes the result as a new version; a transient destroyed without
// persistent() frees what it built and leaves the base untouched.
// ---------------------------------------------------------------------------
export template <typename V> class TransientRadixTree {
public:
  TransientRadixTree(const TransientRadixTree &) = delete;
  auto operator=(const TransientRadixTree &) -> TransientRadixTree & = delete;
  TransientRadixTree(TransientRadixTree &&other) noexcept
      : base_{std::move(other.base_)},
        session_{std::move(other.session_)},
        root_{std::exchange(other.root_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        changed_{std::exchange(other.changed_, false)} {}
  auto operator=(TransientRadixTree &&other) noexcept
      -> TransientRadixTree & {
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
  ~TransientRadixTree() { discard(); }

  [[nodiscard]] auto get(std::span<const std::byte> key) const
      -> std::optional<V> {
    ensure_active();
    auto *p = radix_get_ptr<V>(root_, key);
    if (!p)
      return std::nullopt;
    return *p;
  }

  [[nodiscard]] auto get_ptr(std::span<const std::byte> key) const
      -> const V * {
    ensure_active();
    return radix_get_ptr<V>(root_, key);
  }

  [[nodiscard]] auto contains(std::span<const std::byte> key) const -> bool {
    return get(key).has_value();
  }

  void set(std::span<const std::byte> key, V val) {
    ensure_active();
    auto [new_root, inserted] = session_.set(root_, key, std::move(val));
    root_ = new_root;
    changed_ = true;
    if (inserted)
      ++size_;
  }

  // Single-traversal insert-or-conditional-replace.
  // If key absent: inserts val, returns nullopt.
  // If key present: calls should_replace(existing, val).
  //   If true: replaces with val, returns the displaced old value.
  //   If false: no-op, returns nullopt.
  template <typename Pred>
  auto upsert(std::span<const std::byte> key, V val, Pred &&should_replace)
      -> std::optional<V> {
    ensure_active();
    auto [new_root, displaced, inserted] = session_.upsert(
        root_, key, std::move(val), std::forward<Pred>(should_replace));
    root_ = new_root;
    // Unconditional: upsert copies the path before it asks the predicate,
    // so even a no-op upsert has built nodes that must be published.
    changed_ = true;
    if (inserted)
      ++size_;
    return displaced;
  }

  auto erase(std::span<const std::byte> key) -> bool {
    ensure_active();
    auto [new_root, removed] = session_.erase(root_, key);
    root_ = new_root;
    // An erase that removes nothing returns the subtree it was given, at
    // every level, without copying anything — so the tree is untouched.
    changed_ = changed_ || removed;
    if (removed)
      --size_;
    return removed;
  }

  // Publishes the built tree as a new version. Throws std::logic_error if
  // the base already has a live successor; the transient is then still
  // active and its destructor discards what it built.
  [[nodiscard]] auto persistent() && -> PersistentRadixTree<V> {
    ensure_active();
    if (!changed_) {
      // Nothing was touched, so this is the base version, not a new one.
      // Handing back the base's own handle keeps the registry out of it
      // entirely — the engine opens a transient per batch on maps that
      // most batches do not change. Every operation that builds or
      // supersedes a node sets changed_, so there is nothing to publish
      // and nothing to free.
      assert(root_ == base_.root_ && session_.retired_list().empty());
      session_.finish();
      root_ = nullptr;
      size_ = 0;
      return std::move(base_);
    }
    auto &retired = session_.retired_list();
    const std::uint64_t bases[] = {base_.version_};
    auto version = PersistentRadixTree<V>::registry().publish(
        session_.tag(), bases, retired);
    session_.finish();
    auto live_size = std::exchange(size_, 0);
    auto *root = std::exchange(root_, nullptr);
    base_ = PersistentRadixTree<V>{};
    return PersistentRadixTree<V>{root, live_size, version};
  }

  // Iteration support for range scans on the transient tree. The iterator
  // holds raw node pointers: do not mutate the transient while it is alive.
  [[nodiscard]] auto lower_bound(std::span<const std::byte> key) const
      -> RadixTreeIterator<V> {
    ensure_active();
    return RadixTreeIterator<V>{PersistentRadixTree<V>{}, root_, key};
  }

private:
  PersistentRadixTree<V> base_;
  BuildSession<V> session_;
  Node<V> *root_{nullptr};
  std::size_t size_{0};
  bool changed_{false}; // whether anything was actually written

  explicit TransientRadixTree(const PersistentRadixTree<V> &base)
      : base_{base}, root_{base.root_}, size_{base.size_} {}

  void ensure_active() const {
    if (session_.tag() == 0) [[unlikely]] {
      throw std::logic_error{"TransientRadixTree already consumed"};
    }
  }

  void discard() noexcept {
    session_.discard_all(root_);
    root_ = nullptr;
    size_ = 0;
  }

  friend class PersistentRadixTree<V>;
};

// Out-of-line: PersistentRadixTree::set() / erase() — one-operation
// transients.
template <typename V>
auto PersistentRadixTree<V>::set(std::span<const std::byte> key, V val) const
    -> PersistentRadixTree {
  auto t = transient();
  t.set(key, std::move(val));
  return std::move(t).persistent();
}

template <typename V>
auto PersistentRadixTree<V>::erase(std::span<const std::byte> key) const
    -> PersistentRadixTree {
  if (!root_)
    return *this;
  auto t = transient();
  if (!t.erase(key))
    return *this; // nothing touched: t discards an empty session
  return std::move(t).persistent();
}

// Out-of-line: PersistentRadixTree::merge()
template <typename V>
template <typename ResolveFunc>
auto PersistentRadixTree<V>::merge(const PersistentRadixTree &a,
                                   const PersistentRadixTree &b,
                                   ResolveFunc &&resolve)
    -> PersistentRadixTree {
  if (!a.root_)
    return b;
  if (!b.root_)
    return a;
  BuildSession<V> session;
  auto [new_root, overlaps] =
      session.merge(a.root_, b.root_, std::forward<ResolveFunc>(resolve));
  auto sz = a.size_ + b.size_ - overlaps;
  const std::uint64_t bases[] = {a.version_, b.version_};
  std::uint64_t version = 0;
  try {
    version = registry().publish(session.tag(), bases, session.retired_list());
  } catch (...) {
    session.discard_all(new_root);
    throw;
  }
  session.finish();
  return PersistentRadixTree{new_root, sz, version};
}

// Out-of-line: PersistentRadixTree::transient()
template <typename V>
auto PersistentRadixTree<V>::transient() const -> TransientRadixTree<V> {
  return TransientRadixTree<V>{*this};
}

// ---------------------------------------------------------------------------
// RadixTreeIterator<V>
//
// DFS iterator that materializes keys by concatenating prefixes + transition
// bytes along the path. Satisfies std::input_iterator.
// ---------------------------------------------------------------------------
export template <typename V> class RadixTreeIterator {
public:
  using iterator_category = std::bidirectional_iterator_tag;
  using iterator_concept = std::bidirectional_iterator_tag;
  using value_type = std::pair<std::span<const std::byte>, V>;
  using difference_type = std::ptrdiff_t;

  RadixTreeIterator() = default;

  auto operator*() const -> std::pair<std::span<const std::byte>, const V &> {
    return {std::span<const std::byte>{current_key_},
            stack_.back().node->value_};
  }

  auto operator++() -> RadixTreeIterator & {
    advance();
    return *this;
  }

  auto operator++(int) -> RadixTreeIterator {
    auto tmp = *this;
    advance();
    return tmp;
  }

  auto operator--() -> RadixTreeIterator & {
    retreat();
    return *this;
  }

  auto operator--(int) -> RadixTreeIterator {
    auto tmp = *this;
    retreat();
    return tmp;
  }

  auto operator==(const RadixTreeIterator &other) const noexcept -> bool {
    if (stack_.empty() && other.stack_.empty())
      return true;
    if (stack_.empty() != other.stack_.empty())
      return false;
    return stack_.back().node == other.stack_.back().node &&
           stack_.back().child_idx == other.stack_.back().child_idx &&
           current_key_ == other.current_key_;
  }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return stack_.empty();
  }

private:
  struct Frame {
    Node<V> *node;
    std::size_t child_idx; // Next child to visit.
    std::size_t key_len;   // Length of current_key_ when this frame was pushed.
  };

  // pin_ keeps the version alive; frames hold raw pointers into it. An
  // iterator over a transient has an empty pin_ and must not outlive the
  // transient or span a mutation of it.
  PersistentRadixTree<V> pin_;
  Node<V> *root_{nullptr};
  std::vector<Frame> stack_;
  std::vector<std::byte> current_key_;

  // Construct an iterator starting at begin (visit the whole tree).
  explicit RadixTreeIterator(PersistentRadixTree<V> pin)
      : pin_{std::move(pin)}, root_{pin_.root_} {
    if (!root_)
      return;
    push_node(root_, 0);
    if (!stack_.empty() && !stack_.back().node->has_value()) {
      advance();
    }
  }

  // Construct a lower_bound iterator over `root` (pin_.root_ for a
  // persistent tree, the working root for a transient).
  RadixTreeIterator(PersistentRadixTree<V> pin, Node<V> *root,
                    std::span<const std::byte> target)
      : pin_{std::move(pin)}, root_{root} {
    stack_.reserve(16);
    current_key_.reserve(128);
    if (!root_)
      return;
    auto at_target = seek(root_, target);
    if (stack_.empty())
      return;
    if (at_target && stack_.back().node->has_value())
      return;
    advance();
  }

  // Construct an end iterator (empty stack, root stored for --end()).
  RadixTreeIterator(PersistentRadixTree<V> pin, std::default_sentinel_t)
      : pin_{std::move(pin)}, root_{pin_.root_} {}

  void push_node(Node<V> *node,
                 std::size_t key_len_before) {
    // Append this node's prefix to the key.
    for (std::size_t i = 0; i < node->prefix.size(); ++i) {
      current_key_.push_back(node->prefix[i]);
    }
    stack_.push_back({node, 0, key_len_before});
  }

  void advance() {
    // Find next value-bearing node via DFS.
    while (!stack_.empty()) {
      auto &frame = stack_.back();
      if (frame.child_idx < frame.node->child_count()) {
        auto slot = frame.node->child_at(frame.child_idx);
        ++frame.child_idx;
        auto key_before = current_key_.size();
        current_key_.push_back(slot.transition);
        push_node(slot.ptr, key_before);
        if (stack_.back().node->has_value())
          return;
        // Continue DFS.
      } else {
        // Pop this frame and restore key.
        current_key_.resize(frame.key_len);
        stack_.pop_back();
      }
    }
  }

  // From the current stack top, follow the rightmost child at each level
  // until reaching a node with no children. Sets child_idx = child_count()
  // on each intermediate frame (all children "visited" for backtracking).
  void descend_rightmost() {
    while (stack_.back().node->has_children()) {
      auto &frame = stack_.back();
      auto last = frame.node->child_count() - 1;
      frame.child_idx = frame.node->child_count();
      auto slot = frame.node->child_at(last);
      auto key_before = current_key_.size();
      current_key_.push_back(slot.transition);
      push_node(slot.ptr, key_before);
    }
  }

  // Move to the previous value-bearing node (reverse DFS preorder).
  void retreat() {
    if (stack_.empty()) {
      // --end(): descend to the rightmost (largest) key in the tree.
      if (!root_)
        return;
      push_node(root_, 0);
      stack_.back().child_idx = stack_.back().node->child_count();
      descend_rightmost();
      // Leaf nodes always have values (path compression invariant).
      return;
    }

    // The current position is at a value-bearing node. We need to find the
    // previous one in DFS preorder. In preorder: parent is visited before
    // children. So the previous node is either:
    //   (a) the rightmost leaf of the previous sibling's subtree, or
    //   (b) the parent itself (if it has a value and we are its first child).

    // Pop current node.
    current_key_.resize(stack_.back().key_len);
    stack_.pop_back();

    while (!stack_.empty()) {
      auto &frame = stack_.back();
      // frame.child_idx is the index of the *next* child to visit forward.
      // The child we just came from was child_idx - 1. The previous sibling
      // is child_idx - 2.
      if (frame.child_idx >= 2) {
        // There is a previous sibling. Undo the transition byte of the child
        // we popped (current_key_ already trimmed to frame's key_len + prefix).
        --frame.child_idx;
        auto prev_idx = frame.child_idx - 1;
        auto slot = frame.node->child_at(prev_idx);
        auto key_before = current_key_.size();
        current_key_.push_back(slot.transition);
        push_node(slot.ptr, key_before);
        stack_.back().child_idx = stack_.back().node->child_count();
        descend_rightmost();
        return;
      }
      // child_idx <= 1: no previous sibling. Check if the parent node
      // itself has a value.
      frame.child_idx = 0;
      if (frame.node->has_value())
        return;
      // Parent is a routing node — continue upward.
      current_key_.resize(frame.key_len);
      stack_.pop_back();
    }
    // Retreated past begin() — iterator becomes end (empty stack).
  }

  // Navigate the trie to find the first position whose key >= target.
  // Returns true if the stack top is a node whose key >= target (caller
  // should check the value). Returns false if the stack is set up for
  // advance() to find the next valid position.
  //
  // IMPORTANT: each iteration appends the full node prefix to current_key_
  // before checking divergence. On the "subtree < target" path the append is
  // undone via current_key_.resize(klb). Any refactoring must preserve this
  // append-then-undo discipline, or the key buffer will be corrupted.
  auto seek(Node<V> *root,
            std::span<const std::byte> target) -> bool {
    auto remaining = target;
    auto cur = root;
    std::size_t klb = 0;

    while (cur) {
      auto prefix_span =
          std::span<const std::byte>{cur->prefix.data(), cur->prefix.size()};
      auto cpl = common_prefix_length(prefix_span, remaining);

      for (std::size_t i = 0; i < cur->prefix.size(); ++i)
        current_key_.push_back(cur->prefix[i]);

      if (cpl < prefix_span.size() && cpl < remaining.size()) {
        if (prefix_span[cpl] > remaining[cpl]) {
          // Subtree > target. Position here.
          stack_.push_back({cur, 0, klb});
          return true;
        }
        // Subtree < target. Undo prefix append and backtrack.
        current_key_.resize(klb);
        return false;
      }

      if (cpl < prefix_span.size()) {
        // Target exhausted within prefix — node key > target.
        stack_.push_back({cur, 0, klb});
        return true;
      }

      // Full prefix matched.
      remaining = remaining.subspan(cpl);

      if (remaining.empty()) {
        // Target ends at this node's key.
        stack_.push_back({cur, 0, klb});
        return true;
      }

      // More target to consume. Find child with matching transition byte.
      auto target_byte = remaining[0];
      auto child_remaining = remaining.subspan(1);

      // Walk children in order with a cursor rather than calling child_at(i)
      // per candidate: on a Node256 child_at(i) costs O(i), which would make
      // this descent quadratic in the node's fanout (see Node::next_child).
      typename Node<V>::ChildCursor cursor;
      for (;;) {
        auto ordinal = cursor.ordinal;
        auto slot = cur->next_child(cursor);

        if (!slot || slot->transition > target_byte) {
          // Either every child sorts below the target (walk exhausted) or
          // the first one at/after it is strictly greater, so all remaining
          // children are > target. Either way `ordinal` is where advance()
          // must resume: one past the last child for the exhausted case,
          // the greater child itself otherwise.
          stack_.push_back({cur, ordinal, klb});
          return false;
        }
        if (slot->transition < target_byte)
          continue;

        // Exact match — push parent frame positioned past this child, then
        // descend into it.
        stack_.push_back({cur, ordinal + 1, klb});
        klb = current_key_.size();
        current_key_.push_back(target_byte);
        cur = slot->ptr;
        remaining = child_remaining;
        break;
      }
    }
    return false;
  }

  friend class PersistentRadixTree<V>;
  friend class TransientRadixTree<V>;
  friend class ReverseRadixTreeIterator<V>;
};


// Out-of-line: PersistentRadixTree::begin()
template <typename V>
auto PersistentRadixTree<V>::begin() const -> RadixTreeIterator<V> {
  return RadixTreeIterator<V>{*this};
}

// Out-of-line: PersistentRadixTree::end_iter()
template <typename V>
auto PersistentRadixTree<V>::end_iter() const -> RadixTreeIterator<V> {
  return RadixTreeIterator<V>{*this, std::default_sentinel};
}

// ---------------------------------------------------------------------------
// ReverseRadixTreeIterator<V>
//
// Safe reverse iterator that pre-decrements from past-the-end and holds the
// underlying RadixTreeIterator alive. operator* returns a span into the live
// iterator's key buffer — no temporary, no dangling reference.
// ---------------------------------------------------------------------------
export template <typename V> class ReverseRadixTreeIterator {
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = std::pair<std::span<const std::byte>, V>;
  using difference_type = std::ptrdiff_t;

  ReverseRadixTreeIterator() = default;

  explicit ReverseRadixTreeIterator(RadixTreeIterator<V> past_pos)
      : cur_{std::move(past_pos)} {
    --cur_; // position at the last element (or stay at end if tree is empty)
    // Empty tree: the forward iterator has no stack to retreat into, so we
    // are simultaneously rbegin and rend.
    if (cur_ == std::default_sentinel)
      past_rend_ = true;
  }

  auto operator*() const
      -> std::pair<std::span<const std::byte>, const V &> {
    return *cur_; // span is into cur_'s live key buffer — no dangling
  }

  auto operator++() -> ReverseRadixTreeIterator & {
    // ++rend() is undefined in the standard; we define it as a no-op to
    // avoid the alternative where retreat() on an empty stack re-descends
    // the tree and silently wraps back to the last element.
    if (past_rend_)
      return *this;
    --cur_;
    if (cur_ == std::default_sentinel)
      past_rend_ = true;
    return *this;
  }

  auto operator++(int) -> ReverseRadixTreeIterator {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  auto operator==(const ReverseRadixTreeIterator &other) const noexcept
      -> bool {
    if (past_rend_ != other.past_rend_)
      return false;
    if (past_rend_)
      return true;
    return cur_ == other.cur_;
  }

  // Compares against the end sentinel. True when the iterator has advanced
  // past the first element (explicitly tracked to distinguish from rbegin()
  // on an empty tree, which also has an empty stack).
  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return past_rend_;
  }

  // Standard reverse_iterator semantics: *rit == *(base() - 1), and
  // rend().base() == begin(). Useful for converting a reverse starting
  // point back to a forward iterator (e.g. passing to ReverseKeyIterator
  // in DB::rkeys_from).
  [[nodiscard]] auto base() const -> RadixTreeIterator<V> {
    if (past_rend_) {
      // rend().base() must equal begin(). Reconstruct from the underlying
      // iterator's retained root pointer.
      return RadixTreeIterator<V>{cur_.pin_};
    }
    auto fwd = cur_;
    ++fwd;
    return fwd;
  }

private:
  RadixTreeIterator<V> cur_;
  bool past_rend_{false};
};

// Out-of-line: PersistentRadixTree::rbegin()
template <typename V>
auto PersistentRadixTree<V>::rbegin() const -> ReverseRadixTreeIterator<V> {
  return ReverseRadixTreeIterator<V>{end_iter()};
}

// Out-of-line: PersistentRadixTree::lower_bound()
template <typename V>
auto PersistentRadixTree<V>::lower_bound(std::span<const std::byte> key) const
    -> RadixTreeIterator<V> {
  return RadixTreeIterator<V>{*this, root_, key};
}

// Out-of-line: PersistentRadixTree::upper_bound()
template <typename V>
auto PersistentRadixTree<V>::upper_bound(std::span<const std::byte> key) const
    -> RadixTreeIterator<V> {
  auto it = lower_bound(key);
  if (it != std::default_sentinel) {
    auto [k, v] = *it;
    if (k.size() == key.size() &&
        std::equal(k.begin(), k.end(), key.begin()))
      ++it;
  }
  return it;
}

// ---------------------------------------------------------------------------
// ValueIterator<V>
//
// Forward-only tree walk that yields const V& without constructing keys.
// Identical DFS logic to RadixTreeIterator but with no current_key_ buffer,
// no key_len in Frame, and no push_back/resize per node.
// ---------------------------------------------------------------------------
export template <typename V> class ValueIterator {
public:
  using value_type = V;
  using difference_type = std::ptrdiff_t;

  ValueIterator() = default;

  auto operator*() const -> const V & {
    return stack_.back().node->value_;
  }

  auto operator++() -> ValueIterator & {
    advance();
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return stack_.empty();
  }

private:
  struct Frame {
    Node<V> *node;
    std::size_t child_idx;
  };

  PersistentRadixTree<V> pin_; // keeps the version alive (see RadixTreeIterator)
  std::vector<Frame> stack_;

  explicit ValueIterator(PersistentRadixTree<V> pin) : pin_{std::move(pin)} {
    if (!pin_.root_)
      return;
    push_node(pin_.root_);
    if (!stack_.empty() && !stack_.back().node->has_value())
      advance();
  }

  ValueIterator(PersistentRadixTree<V> pin, std::span<const std::byte> target)
      : pin_{std::move(pin)} {
    stack_.reserve(16);
    if (!pin_.root_)
      return;
    auto pos = seek(pin_.root_, target);
    if (stack_.empty())
      return;
    if (pos.at_node && stack_.back().node->has_value())
      return;
    advance();
  }

  void push_node(Node<V> *node) {
    stack_.push_back({node, 0});
  }

  void advance() {
    while (!stack_.empty()) {
      auto &frame = stack_.back();
      if (frame.child_idx < frame.node->child_count()) {
        auto slot = frame.node->child_at(frame.child_idx);
        ++frame.child_idx;
        push_node(slot.ptr);
        if (stack_.back().node->has_value())
          return;
      } else {
        stack_.pop_back();
      }
    }
  }

  void descend_rightmost() {
    while (stack_.back().node->has_children()) {
      auto &frame = stack_.back();
      auto last = frame.node->child_count() - 1;
      frame.child_idx = frame.node->child_count();
      auto slot = frame.node->child_at(last);
      push_node(slot.ptr);
    }
  }

  void retreat() {
    if (stack_.empty())
      return;

    stack_.pop_back();

    while (!stack_.empty()) {
      auto &frame = stack_.back();
      if (frame.child_idx >= 2) {
        --frame.child_idx;
        auto prev_idx = frame.child_idx - 1;
        auto slot = frame.node->child_at(prev_idx);
        push_node(slot.ptr);
        stack_.back().child_idx = stack_.back().node->child_count();
        descend_rightmost();
        return;
      }
      frame.child_idx = 0;
      if (frame.node->has_value())
        return;
      stack_.pop_back();
    }
  }

  // Where seek() left the cursor. `at_node`: the top frame's node is the
  // first node in key order whose key is >= target (child_idx 0, so the
  // node itself is visited before its children). `exact`: that node's key
  // equals target byte for byte. `exact` implies `at_node`; `at_node`
  // without `exact` means the node's key is strictly greater than target
  // (target ended inside the node's prefix, or diverged below it).
  struct SeekPos {
    bool at_node{false};
    bool exact{false};
  };

  auto seek(Node<V> *root,
            std::span<const std::byte> target) -> SeekPos {
    auto remaining = target;
    auto cur = root;

    while (cur) {
      auto prefix_span =
          std::span<const std::byte>{cur->prefix.data(), cur->prefix.size()};
      auto cpl = common_prefix_length(prefix_span, remaining);

      if (cpl < prefix_span.size() && cpl < remaining.size()) {
        if (prefix_span[cpl] > remaining[cpl]) {
          stack_.push_back({cur, 0});
          return {.at_node = true, .exact = false};
        }
        return {};
      }

      if (cpl < prefix_span.size()) {
        stack_.push_back({cur, 0});
        return {.at_node = true, .exact = false};
      }

      remaining = remaining.subspan(cpl);

      if (remaining.empty()) {
        stack_.push_back({cur, 0});
        return {.at_node = true, .exact = true};
      }

      auto target_byte = remaining[0];
      auto child_remaining = remaining.subspan(1);

      // Cursor walk — same reasoning as RadixTreeIterator::seek.
      typename Node<V>::ChildCursor cursor;
      for (;;) {
        auto ordinal = cursor.ordinal;
        auto slot = cur->next_child(cursor);

        if (!slot || slot->transition > target_byte) {
          stack_.push_back({cur, ordinal});
          return {};
        }
        if (slot->transition < target_byte)
          continue;

        stack_.push_back({cur, ordinal + 1});
        cur = slot->ptr;
        remaining = child_remaining;
        break;
      }
    }
    return {};
  }

  friend class PersistentRadixTree<V>;
  friend class ReverseValueIterator<V>;
};

// ---------------------------------------------------------------------------
// ReverseValueIterator<V>
//
// Reverse tree walk yielding const V& without key construction.
// Wraps ValueIterator and uses its retreat()/descend_rightmost() methods.
// ---------------------------------------------------------------------------
export template <typename V> class ReverseValueIterator {
public:
  using value_type = V;
  using difference_type = std::ptrdiff_t;

  ReverseValueIterator() = default;

  explicit ReverseValueIterator(PersistentRadixTree<V> pin)
      : pin_{std::move(pin)} {
    auto *root = pin_.root_;
    if (!root) {
      past_rend_ = true;
      return;
    }
    // Descend to the rightmost (largest) value node.
    cur_.push_node(root);
    cur_.stack_.back().child_idx = cur_.stack_.back().node->child_count();
    cur_.descend_rightmost();
    if (cur_.stack_.empty())
      past_rend_ = true;
  }

  ReverseValueIterator(PersistentRadixTree<V> pin,
                       std::span<const std::byte> upper)
      : pin_{std::move(pin)} {
    auto *root = pin_.root_;
    if (!root) {
      past_rend_ = true;
      return;
    }
    // Position a forward cursor at lower_bound(upper): the first value node
    // with key >= upper. If that key is exactly upper, start there
    // (inclusive). Otherwise the last key <= upper is one retreat before
    // the lower bound. Only a byte-exact match may start inclusively — a
    // node that seek() lands on because upper ended inside its prefix has
    // a key strictly greater than upper and must not be yielded.
    ValueIterator<V> fwd;
    fwd.stack_.reserve(16);
    auto pos = fwd.seek(root, upper);
    const bool on_value = !fwd.stack_.empty() && pos.at_node &&
                          fwd.stack_.back().node->has_value();
    if (on_value && pos.exact) {
      cur_ = std::move(fwd);
    } else {
      if (!fwd.stack_.empty() && !on_value) {
        fwd.advance();
      }
      if (fwd.stack_.empty()) {
        // Every key is < upper: start at the rightmost value node.
        cur_.push_node(root);
        cur_.stack_.back().child_idx = cur_.stack_.back().node->child_count();
        cur_.descend_rightmost();
      } else {
        cur_ = std::move(fwd);
        cur_.retreat();
      }
    }
    if (cur_.stack_.empty())
      past_rend_ = true;
  }

  auto operator*() const -> const V & {
    return cur_.stack_.back().node->value_;
  }

  auto operator++() -> ReverseValueIterator & {
    if (past_rend_)
      return *this;
    cur_.retreat();
    if (cur_.stack_.empty())
      past_rend_ = true;
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return past_rend_;
  }

  auto operator==(const ReverseValueIterator &other) const noexcept -> bool {
    if (past_rend_ != other.past_rend_)
      return false;
    if (past_rend_)
      return true;
    return cur_.stack_.size() == other.cur_.stack_.size();
  }

private:
  PersistentRadixTree<V> pin_; // keeps the version alive
  ValueIterator<V> cur_;
  bool past_rend_{false};

  friend class PersistentRadixTree<V>;
};

// Out-of-line: PersistentRadixTree::value_begin()
template <typename V>
auto PersistentRadixTree<V>::value_begin() const -> ValueIterator<V> {
  return ValueIterator<V>{*this};
}

// Out-of-line: PersistentRadixTree::value_lower_bound()
template <typename V>
auto PersistentRadixTree<V>::value_lower_bound(
    std::span<const std::byte> key) const -> ValueIterator<V> {
  return ValueIterator<V>{*this, key};
}

// Out-of-line: PersistentRadixTree::value_rbegin()
template <typename V>
auto PersistentRadixTree<V>::value_rbegin() const -> ReverseValueIterator<V> {
  return ReverseValueIterator<V>{*this};
}

// Out-of-line: PersistentRadixTree::value_rlower_bound()
template <typename V>
auto PersistentRadixTree<V>::value_rlower_bound(
    std::span<const std::byte> key) const -> ReverseValueIterator<V> {
  return ReverseValueIterator<V>{*this, key};
}

} // namespace bytecask
