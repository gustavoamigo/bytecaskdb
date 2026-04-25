// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — lock-free adaptive radix tree for the in-memory key index

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
// IntrusivePtr<T>
//
// Lightweight single-pointer smart pointer (8 bytes) replacing
// std::shared_ptr (16 bytes + ~32 byte control block). Requires T to
// provide addref() and release() methods (embedded in Node below).
// No weak_ptr support.
// ---------------------------------------------------------------------------
template <typename T> class IntrusivePtr {
public:
  IntrusivePtr() noexcept = default;
  IntrusivePtr(std::nullptr_t) noexcept {
  } // NOLINT — implicit for pair{nullptr,..}

  // Adopt a raw pointer. Caller must have already set refcount to 1
  // (e.g. via make_intrusive). Does NOT addref — takes ownership.
  static auto adopt(T *p) noexcept -> IntrusivePtr {
    IntrusivePtr ip;
    ip.ptr_ = p;
    return ip;
  }

  ~IntrusivePtr() {
    if (ptr_)
      ptr_->release();
  }

  IntrusivePtr(const IntrusivePtr &o) noexcept : ptr_(o.ptr_) {
    if (ptr_)
      ptr_->addref();
  }

  IntrusivePtr(IntrusivePtr &&o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }

  auto operator=(const IntrusivePtr &o) noexcept -> IntrusivePtr & {
    // Cache RHS pointer before touching *ptr_: if `o` itself lives inside
    // *ptr_ (e.g. a child slot of this node), releasing ptr_ destroys `o`,
    // and a later read of `o.ptr_` would be a use-after-free. Reading the
    // pointer value up front and addref-ing first keeps the target node
    // alive across the release.
    auto *new_ptr = o.ptr_;
    if (new_ptr)
      new_ptr->addref();
    if (ptr_)
      ptr_->release();
    ptr_ = new_ptr;
    return *this;
  }

  auto operator=(IntrusivePtr &&o) noexcept -> IntrusivePtr & {
    if (this == &o)
      return *this;
    // Extract before releasing: handles both the sub-object case (o lives
    // inside *ptr_) and the aliased case (ptr_ == o.ptr_ with two distinct
    // IntrusivePtr objects). Zeroing o.ptr_ first means that if releasing
    // ptr_ transitively destroys `o`'s IntrusivePtr, its destructor sees a
    // null and does not double-decrement the target refcount.
    auto *tmp = o.ptr_;
    o.ptr_ = nullptr;
    if (ptr_)
      ptr_->release();
    ptr_ = tmp;
    return *this;
  }

  auto operator->() const noexcept -> T * { return ptr_; }
  auto operator*() const noexcept -> T & { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
  [[nodiscard]] auto get() const noexcept -> T * { return ptr_; }

  // Relinquish ownership WITHOUT decrementing refcount. The caller
  // must eventually balance the refcount (e.g. via release()). Used by
  // Node::release() for iterative destruction.
  auto detach() noexcept -> T * {
    auto *p = ptr_;
    ptr_ = nullptr;
    return p;
  }

  auto operator==(const IntrusivePtr &o) const noexcept -> bool {
    return ptr_ == o.ptr_;
  }

private:
  T *ptr_{nullptr};
};

template <typename T, typename... Args>
auto make_intrusive(Args &&...args) -> IntrusivePtr<T> {
  // new sets refcount to 1 (default member initializer); adopt() takes
  // ownership without incrementing.
  return IntrusivePtr<T>::adopt(new T(std::forward<Args>(args)...));
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
export template <typename V> class TransientRadixTree;
export template <typename V> class RadixTreeIterator;
export template <typename V> class ReverseRadixTreeIterator;

// Global edit-tag counter for transient sessions.
// Relaxed ordering: only uniqueness is required, not inter-thread visibility
// ordering. Each transient session gets a distinct tag via fetch_add.
namespace detail {
inline std::atomic<std::uint64_t> next_edit_tag{1};
} // namespace detail

// ---------------------------------------------------------------------------
// Node<V> — base type for all radix tree nodes (leaf and internal).
//
// packed_tag_ bit layout:
//   [31: has_value] [30: has_children] [29:0: edit_tag]
//
// 94% of nodes are leaves that never acquire children. By splitting the
// children_ field into a derived InternalNode<V>, leaf nodes are allocated
// at sizeof(Node) = 40 bytes (glibc usable=40) instead of 48 bytes
// (glibc usable=56), saving 16 bytes per leaf.
//
// Safety: children_ does not exist on Node — accessing it through a Node*
// is a compile error. Child accessor methods check has_children_flag()
// before downcasting to InternalNode; a missing flag returns a safe default
// ("no children"), never memory corruption.
// ---------------------------------------------------------------------------
template <typename V> struct InternalNode; // forward declaration

template <typename V> struct Node {
  mutable std::atomic<std::uint32_t> refcount_{1};

  static constexpr std::uint32_t kHasValueBit = 0x8000'0000u;
  static constexpr std::uint32_t kHasChildrenBit = 0x4000'0000u;
  static constexpr std::uint32_t kFlagBits = kHasValueBit | kHasChildrenBit;
  static constexpr std::uint32_t kTagMask = 0x3FFF'FFFFu;

  std::uint32_t packed_tag_{0};
  V value_{};

  using Prefix = CompactPrefix;
  Prefix prefix;

  // -- Child nested types (used by InternalNode, exposed here for callers) --
  struct ChildRef {
    std::byte &transition;
    IntrusivePtr<Node> &ptr;
  };
  struct ConstChildRef {
    const std::byte &transition;
    const IntrusivePtr<Node> &ptr;
  };
  struct ChildStore {
    std::vector<std::byte> transition_bytes;
    std::vector<IntrusivePtr<Node>> ptrs;
    [[nodiscard]] auto size() const noexcept -> std::size_t {
      return transition_bytes.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
      return transition_bytes.empty();
    }
  };

  void addref() const noexcept {
    refcount_.fetch_add(1, std::memory_order_relaxed);
  }
  // Iterative tail-release avoids the O(depth) recursive destructor chain
  // that otherwise occurs via ~IntrusivePtr → release → delete → ~Node →
  // ~ChildStore → ~IntrusivePtr → … .
  //
  // Profiling (perf record, MergeOverlapping/100K) showed this cascade as
  // 29% of total merge time. Converting the last-child release to a loop
  // eliminates recursive call overhead for chains of single-child nodes —
  // the dominant pattern in compressed radix trees.
  void release() const noexcept {
    const Node* cur = this;
    while (cur->refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      auto* mut = const_cast<Node*>(cur);
      bool has_kids = mut->has_children_flag();

      std::unique_ptr<ChildStore> kids;
      if (has_kids) {
        auto* internal = static_cast<InternalNode<V>*>(mut);
        kids = std::move(internal->children_);
        delete internal;
      } else {
        delete mut;
      }

      if (!kids || kids->empty())
        return;

      const Node* tail = nullptr;
      for (std::size_t i = 0; i < kids->size(); ++i) {
        auto* raw = kids->ptrs[i].detach();
        if (!raw) continue;
        if (tail)
          tail->release();
        tail = raw;
      }

      if (!tail)
        return;
      cur = tail;
    }
  }

  [[nodiscard]] auto has_value() const noexcept -> bool {
    return (packed_tag_ & kHasValueBit) != 0;
  }
  [[nodiscard]] auto has_children_flag() const noexcept -> bool {
    return (packed_tag_ & kHasChildrenBit) != 0;
  }
  [[nodiscard]] auto edit_tag() const noexcept -> std::uint32_t {
    return packed_tag_ & kTagMask;
  }
  void set_edit_tag(std::uint32_t tag) noexcept {
    packed_tag_ = (packed_tag_ & kFlagBits) | (tag & kTagMask);
  }
  void set_value(V v) {
    value_ = std::move(v);
    packed_tag_ |= kHasValueBit;
  }
  void clear_value() noexcept { packed_tag_ &= ~kHasValueBit; }

  [[nodiscard]] auto internal_node() noexcept -> InternalNode<V> * {
    if (!has_children_flag())
      return nullptr;
    return static_cast<InternalNode<V> *>(this);
  }
  [[nodiscard]] auto internal_node() const noexcept
      -> const InternalNode<V> * {
    if (!has_children_flag())
      return nullptr;
    return static_cast<const InternalNode<V> *>(this);
  }
  [[nodiscard]] auto child_store_or_null() noexcept -> ChildStore * {
    auto *internal = internal_node();
    if (!internal)
      return nullptr;
    return internal->children_.get();
  }
  [[nodiscard]] auto child_store_or_null() const noexcept
      -> const ChildStore * {
    auto *internal = internal_node();
    if (!internal)
      return nullptr;
    return internal->children_.get();
  }

  // -- Children accessors (delegate to InternalNode via checked cast) -------
  [[nodiscard]] auto child_count() const noexcept -> std::size_t {
    auto *kids = child_store_or_null();
    return kids ? kids->size() : 0;
  }
  [[nodiscard]] auto has_children() const noexcept -> bool {
    auto *kids = child_store_or_null();
    return kids && !kids->empty();
  }
  [[nodiscard]] auto child_at(std::size_t i) const -> ConstChildRef {
    auto *kids = child_store_or_null();
    assert(kids != nullptr);
    assert(i < kids->size());
    return {kids->transition_bytes[i], kids->ptrs[i]};
  }
  [[nodiscard]] auto child_at(std::size_t i) -> ChildRef {
    auto *kids = child_store_or_null();
    assert(kids != nullptr);
    assert(i < kids->size());
    return {kids->transition_bytes[i], kids->ptrs[i]};
  }

  [[nodiscard]] auto find_child(std::byte b) const
      -> std::optional<ConstChildRef> {
    auto *kids = child_store_or_null();
    if (!kids)
      return std::nullopt;
    for (std::size_t i = 0; i < kids->size(); ++i) {
      if (kids->transition_bytes[i] == b)
        return ConstChildRef{kids->transition_bytes[i], kids->ptrs[i]};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto find_child_mut(std::byte b) -> std::optional<ChildRef> {
    auto *kids = child_store_or_null();
    if (!kids)
      return std::nullopt;
    for (std::size_t i = 0; i < kids->size(); ++i) {
      if (kids->transition_bytes[i] == b)
        return ChildRef{kids->transition_bytes[i], kids->ptrs[i]};
    }
    return std::nullopt;
  }

  void insert_child(std::byte b, IntrusivePtr<Node> child) {
    auto *internal = internal_node();
    assert(internal != nullptr);
    if (!internal->children_)
      internal->children_ = std::make_unique<ChildStore>();
    auto &kids = *internal->children_;
    std::size_t pos = 0;
    while (pos < kids.size() && kids.transition_bytes[pos] < b)
      ++pos;
    assert((pos == kids.size() || kids.transition_bytes[pos] != b) &&
           "duplicate transition byte");
    kids.transition_bytes.insert(
      kids.transition_bytes.begin() +
            static_cast<std::ptrdiff_t>(pos),
        b);
    kids.ptrs.insert(
      kids.ptrs.begin() + static_cast<std::ptrdiff_t>(pos),
        std::move(child));
  }

  void remove_child(std::byte b) {
    auto *kids = child_store_or_null();
    if (!kids)
      return;
    for (std::size_t i = 0; i < kids->size(); ++i) {
      if (kids->transition_bytes[i] == b) {
        kids->transition_bytes.erase(
            kids->transition_bytes.begin() +
                static_cast<std::ptrdiff_t>(i));
        kids->ptrs.erase(kids->ptrs.begin() +
                              static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  // Deep clone of this node (not recursive — children are shared).
  [[nodiscard]] auto clone() const -> IntrusivePtr<Node> {
    if (has_children_flag()) {
      auto *self = internal_node();
      assert(self != nullptr);
      auto* n = new InternalNode<V>();
      n->packed_tag_ = packed_tag_ & kFlagBits;
      n->value_ = value_;
      n->prefix = prefix;
      if (self->children_)
        n->children_ = std::make_unique<ChildStore>(*self->children_);
      return IntrusivePtr<Node>::adopt(n);
    }
    auto* n = new Node();
    n->packed_tag_ = packed_tag_ & kHasValueBit;
    n->value_ = value_;
    n->prefix = prefix;
    return IntrusivePtr<Node>::adopt(n);
  }

  // Clone and stamp with edit tag for transient ownership.
  [[nodiscard]] auto clone_for(std::uint32_t tag) const -> IntrusivePtr<Node> {
    auto n = clone();
    n->set_edit_tag(tag);
    return n;
  }

  // Clone, promoting to InternalNode if this is a leaf.
  // Used when the caller will insert children into the clone.
  [[nodiscard]] auto clone_as_internal() const -> IntrusivePtr<Node> {
    auto* n = new InternalNode<V>();
    n->packed_tag_ = (packed_tag_ & kHasValueBit) | kHasChildrenBit;
    n->value_ = value_;
    n->prefix = prefix;
    if (has_children_flag()) {
      auto *src = child_store_or_null();
      if (src)
        n->children_ = std::make_unique<ChildStore>(*src);
    }
    return IntrusivePtr<Node>::adopt(n);
  }
};

// ---------------------------------------------------------------------------
// InternalNode<V> — extends Node with children storage.
//
// Allocated only for nodes that will have children (routing/split nodes).
// sizeof(InternalNode<KeyDirEntry>) == 48, glibc usable=56.
// ---------------------------------------------------------------------------
template <typename V> struct InternalNode : Node<V> {
  std::unique_ptr<typename Node<V>::ChildStore> children_;
};

// Node<uint64_t>: 4+4+8+8 = 24 (no children_ field).
static_assert(sizeof(Node<std::uint64_t>) == 24);
// InternalNode<uint64_t>: 24+8 = 32.
static_assert(sizeof(InternalNode<std::uint64_t>) == 32);

// Factory functions for node allocation.
// make_leaf: allocates Node (40B for KeyDirEntry), no children.
// make_internal: allocates InternalNode (48B for KeyDirEntry), with
// has_children flag set.
template <typename V>
auto make_leaf() -> IntrusivePtr<Node<V>> {
  return IntrusivePtr<Node<V>>::adopt(new Node<V>());
}

template <typename V>
auto make_internal() -> IntrusivePtr<Node<V>> {
  auto* n = new InternalNode<V>();
  n->packed_tag_ |= Node<V>::kHasChildrenBit;
  return IntrusivePtr<Node<V>>::adopt(n);
}

// Promote a leaf node to internal. Returns a new InternalNode with the
// same fields (value, prefix, edit_tag) plus has_children set.
// The original node is not modified — callers replace their pointer.
template <typename V>
auto promote_to_internal(const IntrusivePtr<Node<V>> &node)
    -> IntrusivePtr<Node<V>> {
  assert(!node->has_children_flag());
  auto* n = new InternalNode<V>();
  n->packed_tag_ = (node->packed_tag_ & ~Node<V>::kFlagBits) |
                   (node->packed_tag_ & Node<V>::kHasValueBit) |
                   Node<V>::kHasChildrenBit;
  n->value_ = node->value_;
  n->prefix = node->prefix;
  return IntrusivePtr<Node<V>>::adopt(n);
}

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

// ---------------------------------------------------------------------------
// PersistentRadixTree<V>
// ---------------------------------------------------------------------------
export template <typename V> class PersistentRadixTree {
public:
  PersistentRadixTree() = default;
  PersistentRadixTree(const PersistentRadixTree &) = default;
  auto operator=(const PersistentRadixTree &) -> PersistentRadixTree & = default;

  PersistentRadixTree(PersistentRadixTree &&other) noexcept
      : root_{std::move(other.root_)}, size_{std::exchange(other.size_, 0)} {}
  auto operator=(PersistentRadixTree &&other) noexcept
      -> PersistentRadixTree & {
    root_ = std::move(other.root_);
    size_ = std::exchange(other.size_, 0);
    return *this;
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }

  [[nodiscard]] auto get(std::span<const std::byte> key) const
      -> std::optional<V> {
    if (!root_)
      return std::nullopt;
    return get_impl(root_, key);
  }

  // Returns a pointer to the value stored in the tree node, or nullptr.
  // Valid for the lifetime of this tree instance.
  [[nodiscard]] auto get_ptr(std::span<const std::byte> key) const noexcept
      -> const V * {
    if (!root_)
      return nullptr;
    return get_ptr_impl(root_, key);
  }

  [[nodiscard]] auto contains(std::span<const std::byte> key) const -> bool {
    return get(key).has_value();
  }

  [[nodiscard]] auto set(std::span<const std::byte> key, V val) const
      -> PersistentRadixTree {
    auto [new_root, inserted] = set_impl(root_, key, std::move(val));
    return PersistentRadixTree{std::move(new_root),
                               inserted ? size_ + 1 : size_};
  }

  [[nodiscard]] auto erase(std::span<const std::byte> key) const
      -> PersistentRadixTree {
    if (!root_)
      return *this;
    auto [new_root, removed] = erase_impl(root_, key);
    if (!removed)
      return *this;
    return PersistentRadixTree{std::move(new_root), size_ - 1};
  }

  [[nodiscard]] auto transient() const -> TransientRadixTree<V>;

  // Merge two trees. On key conflicts, resolve(a_val, b_val) picks the winner.
  // Disjoint subtrees are shared in O(1) via IntrusivePtr copy.
  // Size is computed inline as a.size() + b.size() - overlaps (no post-merge walk).
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

private:
  // Returns an iterator-typed end sentinel for upper_bound() and as the
  // starting point for ReverseRadixTreeIterator construction. Not named
  // end() to avoid shadowing the cheaper default_sentinel_t overload used
  // in tight forward-iteration loops.
  [[nodiscard]] auto end_iter() const -> RadixTreeIterator<V>;
  IntrusivePtr<Node<V>> root_;
  std::size_t size_{0};

  PersistentRadixTree(IntrusivePtr<Node<V>> root, std::size_t sz)
      : root_{std::move(root)}, size_{sz} {}

  // -- get --
  // Uses raw pointers during traversal to avoid IntrusivePtr refcount
  // traffic. Safe because the caller's IntrusivePtr to the root keeps the
  // entire node tree alive (parents own IntrusivePtr children).
  static auto get_impl(const IntrusivePtr<Node<V>> &node,
                       std::span<const std::byte> key) -> std::optional<V> {
    auto *p = get_ptr_impl(node, key);
    if (!p)
      return std::nullopt;
    return *p;
  }

  static auto get_ptr_impl(const IntrusivePtr<Node<V>> &node,
                            std::span<const std::byte> key) noexcept
      -> const V * {
    auto remaining = key;
    const Node<V> *cur = node.get();
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
      cur = child->ptr.get();
    }
    return nullptr;
  }

  // -- chain builders --
  // Build a chain of routing nodes ending in a value-bearing leaf.
  // Chunks key left-to-right: each intermediate node gets 7 prefix bytes +
  // 1 transition byte (8 bytes of key material per hop). The final leaf
  // gets the remaining 0–7 bytes as prefix.
  static auto build_leaf_chain(std::span<const std::byte> key, V val,
                               std::uint32_t tag = 0)
      -> IntrusivePtr<Node<V>> {
    // Fast path: key fits in a single node's prefix.
    if (key.size() <= CompactPrefix::kInlineCap) {
      auto leaf = make_leaf<V>();
      if (tag)
        leaf->set_edit_tag(tag);
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
    auto cur = make_leaf<V>();
    if (tag)
      cur->set_edit_tag(tag);
    for (auto b : leaf_prefix)
      cur->prefix.push_back(b);
    cur->set_value(std::move(val));

    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
      auto routing = make_internal<V>();
      if (tag)
        routing->set_edit_tag(tag);
      for (std::size_t i = 0; i < it->prefix_len; ++i)
        routing->prefix.push_back(key[it->prefix_start + i]);
      routing->insert_child(key[it->transition_idx], std::move(cur));
      cur = std::move(routing);
    }
    return cur;
  }

  // Build a chain of routing nodes with an existing terminal node at the end.
  // Overwrites terminal->prefix with the last chunk. Leaves terminal's
  // children and value intact.
  static auto build_routing_chain(std::span<const std::byte> merged,
                                  IntrusivePtr<Node<V>> terminal,
                                  std::uint32_t tag = 0)
      -> IntrusivePtr<Node<V>> {
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
    auto cur = std::move(terminal);
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
      auto routing = make_internal<V>();
      if (tag)
        routing->set_edit_tag(tag);
      for (std::size_t i = 0; i < it->prefix_len; ++i)
        routing->prefix.push_back(merged[it->prefix_start + i]);
      routing->insert_child(merged[it->transition_idx], std::move(cur));
      cur = std::move(routing);
    }
    return cur;
  }

  // -- set (returns new root + whether a new key was inserted) --
  static auto set_impl(const IntrusivePtr<Node<V>> &node,
                       std::span<const std::byte> key, V val)
      -> std::pair<IntrusivePtr<Node<V>>, bool> {
    if (!node) {
      // Create a leaf (chain-split if key > 7 bytes).
      return {build_leaf_chain(key, std::move(val)), true};
    }

    auto new_node = node->clone();
    auto prefix_span = std::span<const std::byte>{new_node->prefix.data(),
                                                  new_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split: divergence within this node's prefix.
      auto split = make_internal<V>();
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(prefix_span[i]);

      // The existing node becomes a child after removing the common prefix +
      // the transition byte.
      auto existing_child = node->clone();
      auto old_transition = prefix_span[cpl];
      typename Node<V>::Prefix old_suffix;
      for (std::size_t i = cpl + 1; i < prefix_span.size(); ++i)
        old_suffix.push_back(prefix_span[i]);
      existing_child->prefix = std::move(old_suffix);
      split->insert_child(old_transition, std::move(existing_child));

      auto remaining = key.subspan(cpl);
      if (remaining.empty()) {
        // The key matches exactly the common prefix — value goes on split
        // node.
        split->set_value(std::move(val));
      } else {
        auto new_transition = remaining[0];
        auto chain = build_leaf_chain(remaining.subspan(1), std::move(val));
        split->insert_child(new_transition, std::move(chain));
      }
      return {std::move(split), true};
    }

    // Full prefix matched.
    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      // Key ends exactly at this node.
      bool was_absent = !new_node->has_value();
      new_node->set_value(std::move(val));
      return {std::move(new_node), was_absent};
    }

    // Recurse into child.
    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing_child = new_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, inserted] =
          set_impl(existing_child->ptr, child_key, std::move(val));
      existing_child->ptr = std::move(new_child);
      return {std::move(new_node), inserted};
    }
    // No child for this transition — create a leaf.
    // Promote to internal if the clone is a leaf (the node previously
    // had no children but now needs one).
    auto chain = build_leaf_chain(child_key, std::move(val));
    if (!new_node->has_children_flag())
      new_node = promote_to_internal(new_node);
    new_node->insert_child(transition, std::move(chain));
    return {std::move(new_node), true};
  }

  // -- erase (returns new root + whether a key was removed) --
  // Also applies path compression: merges a routing node with its single child.
  static auto erase_impl(const IntrusivePtr<Node<V>> &node,
                         std::span<const std::byte> key)
      -> std::pair<IntrusivePtr<Node<V>>, bool> {
    if (!node)
      return {nullptr, false};

    auto prefix_span =
        std::span<const std::byte>{node->prefix.data(), node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Key diverges within prefix — nothing to erase.
      return {node, false};
    }

    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      // Key matches this node.
      if (!node->has_value())
        return {node, false};

      auto new_node = node->clone();
      new_node->clear_value();

      // Path compression.
      if (!new_node->has_children()) {
        // No children and no value — node is dead.
        return {nullptr, true};
      }
      if (new_node->child_count() == 1) {
        // Merge with sole child.
        return {merge_with_child(std::move(new_node)), true};
      }
      return {std::move(new_node), true};
    }

    // Recurse.
    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing = node->find_child(transition);
    if (!existing)
      return {node, false};

    auto [new_child, removed] = erase_impl(existing->ptr, child_key);
    if (!removed)
      return {node, false};

    auto new_node = node->clone();
    if (!new_child) {
      // Child was deleted entirely.
      new_node->remove_child(transition);
      // Path compression: if this node is now a routing node with 1 child and
      // no value, merge.
      if (!new_node->has_value() && new_node->child_count() == 1) {
        return {merge_with_child(std::move(new_node)), true};
      }
      if (!new_node->has_value() && !new_node->has_children()) {
        return {nullptr, true};
      }
      return {std::move(new_node), true};
    }
    auto child_slot = new_node->find_child_mut(transition);
    child_slot->ptr = std::move(new_child);
    // Path compression on the child side: child might now be a routing node
    // with 1 child and no value — but that's the child's responsibility,
    // already handled in the recursive call.
    return {std::move(new_node), true};
  }

  // Merge a routing node (no value) with its single child.
  // New prefix = node.prefix + transition_byte + child.prefix
  // If combined prefix > 7 bytes, builds a chain of routing nodes.
  static auto merge_with_child(IntrusivePtr<Node<V>> node)
      -> IntrusivePtr<Node<V>> {
    assert(!node->has_value() && node->child_count() == 1);
    auto slot0 = node->child_at(0);
    auto transition = slot0.transition;
    auto child = slot0.ptr->clone();

    auto total = node->prefix.size() + 1 + child->prefix.size();
    if (total <= CompactPrefix::kInlineCap) {
      // Fast path: fits in a single prefix.
      typename Node<V>::Prefix merged_prefix;
      for (auto b : node->prefix)
        merged_prefix.push_back(b);
      merged_prefix.push_back(transition);
      for (std::size_t i = 0; i < child->prefix.size(); ++i)
        merged_prefix.push_back(child->prefix[i]);
      child->prefix = std::move(merged_prefix);
      return std::move(child);
    }
    // Overflow: collect merged bytes, build a routing chain.
    std::vector<std::byte> merged;
    merged.reserve(total);
    for (auto b : node->prefix)
      merged.push_back(b);
    merged.push_back(transition);
    for (std::size_t i = 0; i < child->prefix.size(); ++i)
      merged.push_back(child->prefix[i]);
    return build_routing_chain(std::span<const std::byte>{merged},
                               std::move(child));
  }

  // -- merge_impl --
  // Recursively merges two subtrees rooted at `a` and `b`.
  // Disjoint subtrees are shared in O(1) via IntrusivePtr copy (no clone).
  // Returns {merged_root, overlap_count} where overlap_count is the number
  // of keys present in both a and b (i.e. where resolve was called).
  template <typename ResolveFunc>
  static auto merge_impl(const IntrusivePtr<Node<V>> &a,
                         const IntrusivePtr<Node<V>> &b,
                         ResolveFunc &&resolve)
      -> std::pair<IntrusivePtr<Node<V>>, std::size_t> {
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
      auto split = make_internal<V>();
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(pa[i]);

      auto a_trimmed = a->clone();
      typename Node<V>::Prefix a_suffix;
      for (std::size_t i = cpl + 1; i < pa.size(); ++i)
        a_suffix.push_back(pa[i]);
      a_trimmed->prefix = std::move(a_suffix);
      split->insert_child(pa[cpl], std::move(a_trimmed));

      auto b_trimmed = b->clone();
      typename Node<V>::Prefix b_suffix;
      for (std::size_t i = cpl + 1; i < pb.size(); ++i)
        b_suffix.push_back(pb[i]);
      b_trimmed->prefix = std::move(b_suffix);
      split->insert_child(pb[cpl], std::move(b_trimmed));

      return {std::move(split), 0};
    }

    if (cpl < pa.size()) {
      // b's prefix is fully consumed — b's node sits *above* a in the trie.
      // Build result based on b; insert a under b at transition pa[cpl].
      auto new_b = b->clone_as_internal();
      auto a_trimmed = a->clone();
      typename Node<V>::Prefix a_suffix;
      for (std::size_t i = cpl + 1; i < pa.size(); ++i)
        a_suffix.push_back(pa[i]);
      a_trimmed->prefix = std::move(a_suffix);

      auto existing = new_b->find_child_mut(pa[cpl]);
      std::size_t overlaps = 0;
      if (existing) {
        auto [child, child_overlaps] =
            merge_impl(a_trimmed, existing->ptr, resolve);
        existing->ptr = std::move(child);
        overlaps = child_overlaps;
      } else {
        new_b->insert_child(pa[cpl], std::move(a_trimmed));
      }
      return {std::move(new_b), overlaps};
    }

    if (cpl < pb.size()) {
      // a's prefix is fully consumed — a's node sits *above* b in the trie.
      // Build result based on a; insert b under a at transition pb[cpl].
      auto new_a = a->clone_as_internal();
      auto b_trimmed = b->clone();
      typename Node<V>::Prefix b_suffix;
      for (std::size_t i = cpl + 1; i < pb.size(); ++i)
        b_suffix.push_back(pb[i]);
      b_trimmed->prefix = std::move(b_suffix);

      auto existing = new_a->find_child_mut(pb[cpl]);
      std::size_t overlaps = 0;
      if (existing) {
        auto [child, child_overlaps] =
            merge_impl(existing->ptr, b_trimmed, resolve);
        existing->ptr = std::move(child);
        overlaps = child_overlaps;
      } else {
        new_a->insert_child(pb[cpl], std::move(b_trimmed));
      }
      return {std::move(new_a), overlaps};
    }

    // Full prefix match — both nodes share the same compressed key prefix.
    // Clone a as internal — b's children may need to be folded in.
    auto merged = a->clone_as_internal();
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
      for (std::size_t i = 0; i < b->child_count(); ++i) {
        auto b_slot = b->child_at(i);
        auto slot = merged->find_child_mut(b_slot.transition);
        if (slot) {
          auto [child, child_overlaps] =
              merge_impl(slot->ptr, b_slot.ptr, resolve);
          slot->ptr = std::move(child);
          overlaps += child_overlaps;
        } else {
          // Disjoint subtree — share it in O(1), no clone needed.
          merged->insert_child(b_slot.transition, b_slot.ptr);
        }
      }
    }

    return {std::move(merged), overlaps};
  }

  friend class TransientRadixTree<V>;
  friend class RadixTreeIterator<V>;
};

// ---------------------------------------------------------------------------
// TransientRadixTree<V>
// ---------------------------------------------------------------------------
export template <typename V> class TransientRadixTree {
public:
  TransientRadixTree(const TransientRadixTree &) = delete;
  auto operator=(const TransientRadixTree &) -> TransientRadixTree & = delete;
  TransientRadixTree(TransientRadixTree &&other) noexcept
      : root_{std::move(other.root_)},
        size_{std::exchange(other.size_, 0)},
        tag_{std::exchange(other.tag_, 0)} {}
  auto operator=(TransientRadixTree &&other) noexcept
      -> TransientRadixTree & {
    root_ = std::move(other.root_);
    size_ = std::exchange(other.size_, 0);
    tag_ = std::exchange(other.tag_, 0);
    return *this;
  }

  [[nodiscard]] auto get(std::span<const std::byte> key) const
      -> std::optional<V> {
    ensure_active();
    if (!root_)
      return std::nullopt;
    return PersistentRadixTree<V>::get_impl(root_, key);
  }

  [[nodiscard]] auto get_ptr(std::span<const std::byte> key) const
      -> const V * {
    ensure_active();
    if (!root_)
      return nullptr;
    return PersistentRadixTree<V>::get_ptr_impl(root_, key);
  }

  [[nodiscard]] auto contains(std::span<const std::byte> key) const -> bool {
    return get(key).has_value();
  }

  void set(std::span<const std::byte> key, V val) {
    ensure_active();
    auto [new_root, inserted] = set_transient(root_, key, std::move(val), tag_);
    root_ = std::move(new_root);
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
    auto [new_root, displaced, inserted] = upsert_transient(
        root_, key, std::move(val), tag_,
        std::forward<Pred>(should_replace));
    root_ = std::move(new_root);
    if (inserted)
      ++size_;
    return displaced;
  }

  auto erase(std::span<const std::byte> key) -> bool {
    ensure_active();
    if (!root_)
      return false;
    auto [new_root, removed] = erase_transient(root_, key, tag_);
    root_ = std::move(new_root);
    if (removed)
      --size_;
    return removed;
  }

  [[nodiscard]] auto persistent() && -> PersistentRadixTree<V> {
    ensure_active();
    auto live_size = std::exchange(size_, 0);
    tag_ = 0; // Retire the tag — nodes become immutable.
    return PersistentRadixTree<V>{std::move(root_), live_size};
  }

  // Iteration support for range scans on the transient tree.
  // Shares the same internal structure as PersistentRadixTree.
  [[nodiscard]] auto lower_bound(std::span<const std::byte> key) const
      -> RadixTreeIterator<V> {
    ensure_active();
    return RadixTreeIterator<V>{root_, key};
  }

private:
  IntrusivePtr<Node<V>> root_;
  std::size_t size_{0};
  std::uint32_t tag_{0};

  TransientRadixTree(IntrusivePtr<Node<V>> root, std::size_t sz,
                     std::uint32_t tag)
      : root_{std::move(root)}, size_{sz}, tag_{tag} {}

  void ensure_active() const {
    if (tag_ == 0) [[unlikely]] {
      throw std::logic_error{"TransientRadixTree already consumed"};
    }
  }

  // Ensure a node is owned by this transient session.
  // Requires both matching edit tag AND unique ownership (refcount == 1)
  // to allow in-place mutation. The refcount check defends against tag
  // wraparound after 2^31 transient sessions: even if an old node
  // happens to carry the same 31-bit tag, it will be cloned if shared.
  using Ops = PersistentRadixTree<V>;

  static auto ensure_mutable(const IntrusivePtr<Node<V>> &node,
                             std::uint32_t tag) -> IntrusivePtr<Node<V>> {
    if (node && node->edit_tag() == tag &&
        node->refcount_.load(std::memory_order_acquire) == 1)
      return node;
    if (!node) {
      auto n = make_leaf<V>();
      n->set_edit_tag(tag);
      return n;
    }
    return node->clone_for(tag);
  }

  // Transient set — mutates owned nodes in-place, copies shared ones.
  static auto set_transient(const IntrusivePtr<Node<V>> &node,
                            std::span<const std::byte> key, V val,
                            std::uint32_t tag)
      -> std::pair<IntrusivePtr<Node<V>>, bool> {
    if (!node) {
      return {Ops::build_leaf_chain(key, std::move(val), tag), true};
    }

    auto mutable_node = ensure_mutable(node, tag);
    auto prefix_span = std::span<const std::byte>{mutable_node->prefix.data(),
                                                  mutable_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split.
      auto split = make_internal<V>();
      split->set_edit_tag(tag);
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(prefix_span[i]);

      auto old_transition = prefix_span[cpl];
      typename Node<V>::Prefix old_suffix;
      for (std::size_t i = cpl + 1; i < prefix_span.size(); ++i)
        old_suffix.push_back(prefix_span[i]);
      mutable_node->prefix = std::move(old_suffix);
      split->insert_child(old_transition, std::move(mutable_node));

      auto remaining = key.subspan(cpl);
      if (remaining.empty()) {
        split->set_value(std::move(val));
      } else {
        auto new_transition = remaining[0];
        auto chain = Ops::build_leaf_chain(remaining.subspan(1), std::move(val), tag);
        split->insert_child(new_transition, std::move(chain));
      }
      return {std::move(split), true};
    }

    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      bool was_absent = !mutable_node->has_value();
      mutable_node->set_value(std::move(val));
      return {std::move(mutable_node), was_absent};
    }

    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing_child = mutable_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, inserted] =
          set_transient(existing_child->ptr, child_key, std::move(val), tag);
      existing_child->ptr = std::move(new_child);
      return {std::move(mutable_node), inserted};
    }
    auto chain = Ops::build_leaf_chain(child_key, std::move(val), tag);
    if (!mutable_node->has_children_flag())
      mutable_node = promote_to_internal(mutable_node);
    mutable_node->insert_child(transition, std::move(chain));
    return {std::move(mutable_node), true};
  }

  // Single-traversal upsert — like set_transient, but conditionally replaces
  // an existing value. Returns {new_root, displaced_value, was_newly_inserted}.
  // When the key already exists, calls should_replace(existing, incoming);
  // if true, swaps in the new value and returns the old one as displaced.
  template <typename Pred>
  static auto upsert_transient(const IntrusivePtr<Node<V>> &node,
                               std::span<const std::byte> key, V val,
                               std::uint32_t tag, Pred &&should_replace)
      -> std::tuple<IntrusivePtr<Node<V>>, std::optional<V>, bool> {
    if (!node) {
      return {Ops::build_leaf_chain(key, std::move(val), tag), std::nullopt, true};
    }

    auto mutable_node = ensure_mutable(node, tag);
    auto prefix_span = std::span<const std::byte>{mutable_node->prefix.data(),
                                                  mutable_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split — key diverges from prefix, so this is always a new insert.
      auto split = make_internal<V>();
      split->set_edit_tag(tag);
      for (std::size_t i = 0; i < cpl; ++i)
        split->prefix.push_back(prefix_span[i]);

      auto old_transition = prefix_span[cpl];
      typename Node<V>::Prefix old_suffix;
      for (std::size_t i = cpl + 1; i < prefix_span.size(); ++i)
        old_suffix.push_back(prefix_span[i]);
      mutable_node->prefix = std::move(old_suffix);
      split->insert_child(old_transition, std::move(mutable_node));

      auto remaining = key.subspan(cpl);
      if (remaining.empty()) {
        split->set_value(std::move(val));
      } else {
        auto new_transition = remaining[0];
        auto chain = Ops::build_leaf_chain(remaining.subspan(1), std::move(val), tag);
        split->insert_child(new_transition, std::move(chain));
      }
      return {std::move(split), std::nullopt, true};
    }

    auto remaining = key.subspan(cpl);
    if (remaining.empty()) {
      if (mutable_node->has_value()) {
        if (should_replace(mutable_node->value_, val)) {
          auto old = std::move(mutable_node->value_);
          mutable_node->set_value(std::move(val));
          return {std::move(mutable_node), std::move(old), false};
        }
        return {std::move(mutable_node), std::nullopt, false};
      }
      mutable_node->set_value(std::move(val));
      return {std::move(mutable_node), std::nullopt, true};
    }

    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing_child = mutable_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, displaced, inserted] = upsert_transient(
          existing_child->ptr, child_key, std::move(val), tag,
          std::forward<Pred>(should_replace));
      existing_child->ptr = std::move(new_child);
      return {std::move(mutable_node), std::move(displaced), inserted};
    }
    auto chain = Ops::build_leaf_chain(child_key, std::move(val), tag);
    if (!mutable_node->has_children_flag())
      mutable_node = promote_to_internal(mutable_node);
    mutable_node->insert_child(transition, std::move(chain));
    return {std::move(mutable_node), std::nullopt, true};
  }

  // Transient erase with path compression.
  static auto erase_transient(const IntrusivePtr<Node<V>> &node,
                              std::span<const std::byte> key, std::uint32_t tag)
      -> std::pair<IntrusivePtr<Node<V>>, bool> {
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

      auto mutable_node = ensure_mutable(node, tag);
      mutable_node->clear_value();

      if (!mutable_node->has_children())
        return {nullptr, true};
      if (mutable_node->child_count() == 1) {
        return {merge_with_child_transient(std::move(mutable_node), tag), true};
      }
      return {std::move(mutable_node), true};
    }

    auto transition = remaining[0];
    auto child_key = remaining.subspan(1);
    auto existing = node->find_child(transition);
    if (!existing)
      return {node, false};

    auto [new_child, removed] =
        erase_transient(existing->ptr, child_key, tag);
    if (!removed)
      return {node, false};

    auto mutable_node = ensure_mutable(node, tag);
    if (!new_child) {
      mutable_node->remove_child(transition);
      if (!mutable_node->has_value() && mutable_node->child_count() == 1) {
        return {merge_with_child_transient(std::move(mutable_node), tag), true};
      }
      if (!mutable_node->has_value() && !mutable_node->has_children()) {
        return {nullptr, true};
      }
      return {std::move(mutable_node), true};
    }
    auto child_slot = mutable_node->find_child_mut(transition);
    child_slot->ptr = std::move(new_child);
    return {std::move(mutable_node), true};
  }

  static auto merge_with_child_transient(IntrusivePtr<Node<V>> node,
                                         std::uint32_t tag)
      -> IntrusivePtr<Node<V>> {
    assert(!node->has_value() && node->child_count() == 1);
    auto slot = node->child_at(0);
    auto transition = slot.transition;
    auto child = ensure_mutable(slot.ptr, tag);

    auto total = node->prefix.size() + 1 + child->prefix.size();
    if (total <= CompactPrefix::kInlineCap) {
      typename Node<V>::Prefix merged_prefix;
      for (std::size_t i = 0; i < node->prefix.size(); ++i)
        merged_prefix.push_back(node->prefix[i]);
      merged_prefix.push_back(transition);
      for (std::size_t i = 0; i < child->prefix.size(); ++i)
        merged_prefix.push_back(child->prefix[i]);
      child->prefix = std::move(merged_prefix);
      return std::move(child);
    }    std::vector<std::byte> merged;
    merged.reserve(total);
    for (std::size_t i = 0; i < node->prefix.size(); ++i)
      merged.push_back(node->prefix[i]);
    merged.push_back(transition);
    for (std::size_t i = 0; i < child->prefix.size(); ++i)
      merged.push_back(child->prefix[i]);
    return Ops::build_routing_chain(std::span<const std::byte>{merged},
                               std::move(child), tag);
  }

  friend class PersistentRadixTree<V>;
};

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
  auto [new_root, overlaps] =
      merge_impl(a.root_, b.root_, std::forward<ResolveFunc>(resolve));
  auto sz = a.size_ + b.size_ - overlaps;
  return PersistentRadixTree{std::move(new_root), sz};
}

// Out-of-line: PersistentRadixTree::transient()
template <typename V>
auto PersistentRadixTree<V>::transient() const -> TransientRadixTree<V> {
  // Relaxed ordering: only uniqueness is required, not inter-thread visibility
  // ordering. Each transient session gets a distinct tag via fetch_add.
  // Truncate to 30 bits — tag 0 is reserved for "immutable" sentinel.
  // Bit 31 = has_value, bit 30 = has_children, bits 29:0 = edit_tag.
  auto raw = detail::next_edit_tag.fetch_add(1, std::memory_order_relaxed);
  auto tag = static_cast<std::uint32_t>(raw & Node<V>::kTagMask);
  if (tag == 0) [[unlikely]]
    tag = static_cast<std::uint32_t>(
        detail::next_edit_tag.fetch_add(1, std::memory_order_relaxed) &
        Node<V>::kTagMask);
  return TransientRadixTree<V>{root_, size_, tag};
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
    IntrusivePtr<Node<V>> node;
    std::size_t child_idx; // Next child to visit.
    std::size_t key_len;   // Length of current_key_ when this frame was pushed.
  };

  IntrusivePtr<Node<V>> root_;
  std::vector<Frame> stack_;
  std::vector<std::byte> current_key_;

  // Construct an iterator starting at begin (visit the whole tree).
  explicit RadixTreeIterator(IntrusivePtr<Node<V>> root)
      : root_{std::move(root)} {
    if (!root_)
      return;
    push_node(root_, 0);
    if (!stack_.empty() && !stack_.back().node->has_value()) {
      advance();
    }
  }

  // Construct a lower_bound iterator.
  RadixTreeIterator(IntrusivePtr<Node<V>> root,
                    std::span<const std::byte> target)
      : root_{std::move(root)} {
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
  RadixTreeIterator(IntrusivePtr<Node<V>> root, std::default_sentinel_t)
      : root_{std::move(root)} {}

  void push_node(const IntrusivePtr<Node<V>> &node,
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
  auto seek(const IntrusivePtr<Node<V>> &root,
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

      bool descended = false;
      for (std::size_t i = 0; i < cur->child_count(); ++i) {
        auto cb = cur->child_at(i).transition;
        if (cb < target_byte)
          continue;

        if (cb == target_byte) {
          // Exact match — push parent frame, descend into child.
          stack_.push_back({cur, i + 1, klb});
          klb = current_key_.size();
          current_key_.push_back(cb);
          cur = cur->child_at(i).ptr;
          remaining = child_remaining;
          descended = true;
          break;
        }

        // cb > target_byte — children from here onward are all > target.
        // Push frame so advance() picks up children[i].
        stack_.push_back({cur, i, klb});
        return false;
      }

      if (!descended) {
        // All children < target_byte. Push exhausted frame for backtracking.
        stack_.push_back({cur, cur->child_count(), klb});
        return false;
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
  return RadixTreeIterator<V>{root_};
}

// Out-of-line: PersistentRadixTree::end_iter()
template <typename V>
auto PersistentRadixTree<V>::end_iter() const -> RadixTreeIterator<V> {
  return RadixTreeIterator<V>{root_, std::default_sentinel};
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
      return RadixTreeIterator<V>{cur_.root_};
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
  return RadixTreeIterator<V>{root_, key};
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

} // namespace bytecask
