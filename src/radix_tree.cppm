module;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bytecask.radix_tree;

namespace bytecask {

// ---------------------------------------------------------------------------
// SmallVector<T, N>
//
// Stores up to N elements inline (no heap allocation), spilling to an
// std::vector<T> beyond that. Provides a minimal subset of the vector
// interface sufficient for the radix tree's node layout.
// ---------------------------------------------------------------------------
export template <typename T, std::size_t N> class SmallVector {
public:
  SmallVector() : size_{0} {
    // Intentionally leave inline_storage_ uninitialized — no elements
    // constructed yet. The union's heap_ member is not active.
  }

  ~SmallVector() { destroy_all(); }

  SmallVector(const SmallVector &other) : size_{other.size_} {
    if (other.on_heap()) {
      new (&heap_) std::vector<T>(other.heap_);
    } else {
      for (std::size_t i = 0; i < size_; ++i) {
        std::construct_at(inline_ptr() + i, other.inline_ptr()[i]);
      }
    }
  }

  SmallVector(SmallVector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : size_{other.size_} {
    if (other.on_heap()) {
      new (&heap_) std::vector<T>(std::move(other.heap_));
      other.heap_.~vector();
      other.size_ = 0;
    } else {
      for (std::size_t i = 0; i < size_; ++i) {
        std::construct_at(inline_ptr() + i, std::move(other.inline_ptr()[i]));
      }
      other.destroy_inline();
      other.size_ = 0;
    }
  }

  auto operator=(const SmallVector &other) -> SmallVector & {
    if (this != &other) {
      auto tmp{other};          // copy first — if this throws, *this is untouched
      *this = std::move(tmp);   // move-assign is noexcept for our element types
    }
    return *this;
  }

  auto operator=(SmallVector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>) -> SmallVector & {
    if (this == &other)
      return *this;
    destroy_all();
    size_ = other.size_;
    if (other.on_heap()) {
      new (&heap_) std::vector<T>(std::move(other.heap_));
      other.heap_.~vector();
      other.size_ = 0;
    } else {
      for (std::size_t i = 0; i < size_; ++i) {
        std::construct_at(inline_ptr() + i, std::move(other.inline_ptr()[i]));
      }
      other.destroy_inline();
      other.size_ = 0;
    }
    return *this;
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return on_heap() ? heap_.size() : size_;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

  [[nodiscard]] auto data() noexcept -> T * {
    return on_heap() ? heap_.data() : inline_ptr();
  }

  [[nodiscard]] auto data() const noexcept -> const T * {
    return on_heap() ? heap_.data() : inline_ptr();
  }

  [[nodiscard]] auto operator[](std::size_t i) noexcept -> T & {
    return data()[i];
  }

  [[nodiscard]] auto operator[](std::size_t i) const noexcept -> const T & {
    return data()[i];
  }

  [[nodiscard]] auto begin() noexcept -> T * { return data(); }
  [[nodiscard]] auto end() noexcept -> T * { return data() + size(); }
  [[nodiscard]] auto begin() const noexcept -> const T * { return data(); }
  [[nodiscard]] auto end() const noexcept -> const T * {
    return data() + size();
  }

  void push_back(const T &val) {
    if (on_heap()) {
      heap_.push_back(val);
    } else if (size_ < N) {
      std::construct_at(inline_ptr() + size_, val);
      ++size_;
    } else {
      spill_to_heap();
      heap_.push_back(val);
    }
  }

  void push_back(T &&val) {
    if (on_heap()) {
      heap_.push_back(std::move(val));
    } else if (size_ < N) {
      std::construct_at(inline_ptr() + size_, std::move(val));
      ++size_;
    } else {
      spill_to_heap();
      heap_.push_back(std::move(val));
    }
  }

  // Insert at position. Returns pointer to inserted element.
  auto insert(T *pos, T val) -> T * {
    auto idx = static_cast<std::size_t>(pos - data());
    if (on_heap()) {
      auto it = heap_.insert(heap_.begin() + static_cast<std::ptrdiff_t>(idx),
                             std::move(val));
      return &*it;
    }
    if (size_ < N) {
      if (idx < size_) {
        // Shift elements [idx, size_) right by one.
        std::construct_at(inline_ptr() + size_,
                          std::move(inline_ptr()[size_ - 1]));
        for (auto i = size_ - 1; i > idx; --i) {
          inline_ptr()[i] = std::move(inline_ptr()[i - 1]);
        }
        inline_ptr()[idx] = std::move(val);
      } else {
        // Inserting at the end — no shift needed.
        std::construct_at(inline_ptr() + idx, std::move(val));
      }
      ++size_;
      return inline_ptr() + idx;
    }
    spill_to_heap();
    auto it = heap_.insert(heap_.begin() + static_cast<std::ptrdiff_t>(idx),
                           std::move(val));
    return &*it;
  }

  void erase(T *pos) {
    auto idx = static_cast<std::size_t>(pos - data());
    if (on_heap()) {
      heap_.erase(heap_.begin() + static_cast<std::ptrdiff_t>(idx));
      return;
    }
    for (auto i = idx; i + 1 < size_; ++i) {
      inline_ptr()[i] = std::move(inline_ptr()[i + 1]);
    }
    std::destroy_at(inline_ptr() + size_ - 1);
    --size_;
  }

  void clear() {
    destroy_all();
    // After destroy_all() the union has no active member. Setting size_ = 0
    // switches back to inline mode; subsequent push_back will construct_at
    // into inline_storage_. This is valid under C++20 implicit-lifetime rules
    // (std::byte is an implicit-lifetime type and the union provides storage).
    size_ = 0;
  }

private:
  static constexpr std::size_t kSpillSentinel = N + 1;

  [[nodiscard]] auto on_heap() const noexcept -> bool {
    return size_ == kSpillSentinel;
  }

  auto inline_ptr() noexcept -> T * {
    return std::launder(reinterpret_cast<T *>(&inline_storage_));
  }

  auto inline_ptr() const noexcept -> const T * {
    return std::launder(reinterpret_cast<const T *>(&inline_storage_));
  }

  void destroy_inline() {
    if (!on_heap()) {
      for (std::size_t i = 0; i < size_; ++i) {
        std::destroy_at(inline_ptr() + i);
      }
    }
  }

  void destroy_all() {
    if (on_heap()) {
      heap_.~vector();
    } else {
      destroy_inline();
    }
  }

  void spill_to_heap() {
    std::vector<T> tmp;
    tmp.reserve(N + 1);
    for (std::size_t i = 0; i < size_; ++i) {
      tmp.push_back(std::move(inline_ptr()[i]));
    }
    destroy_inline();
    new (&heap_) std::vector<T>(std::move(tmp));
    size_ = kSpillSentinel;
  }

  std::size_t size_{0};
  union {
    alignas(T) std::byte inline_storage_[sizeof(T) * N];
    std::vector<T> heap_;
  };
};

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
    // Addref before release: if o is a sub-object of *ptr_, releasing
    // ptr_ first would destroy o (use-after-free).
    if (o.ptr_)
      o.ptr_->addref();
    if (ptr_)
      ptr_->release();
    ptr_ = o.ptr_;
    return *this;
  }

  auto operator=(IntrusivePtr &&o) noexcept -> IntrusivePtr & {
    if (ptr_ != o.ptr_) {
      // Detach o before releasing ptr_ to avoid use-after-free when
      // o is a sub-object of *ptr_.
      auto *tmp = o.ptr_;
      o.ptr_ = nullptr;
      if (ptr_)
        ptr_->release();
      ptr_ = tmp;
    }
    return *this;
  }

  auto operator->() const noexcept -> T * { return ptr_; }
  auto operator*() const noexcept -> T & { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
  [[nodiscard]] auto get() const noexcept -> T * { return ptr_; }

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

// Global edit-tag counter for transient sessions.
// Relaxed ordering: only uniqueness is required, not inter-thread visibility
// ordering. Each transient session gets a distinct tag via fetch_add.
namespace detail {
inline std::atomic<std::uint64_t> next_edit_tag{1};
} // namespace detail

// ---------------------------------------------------------------------------
// Node<V>
// ---------------------------------------------------------------------------
template <typename V> struct Node {
  // Intrusive reference count. mutable so addref/release work through const
  // paths (same semantics as shared_ptr's control block).
  // Starts at 1: make_intrusive + IntrusivePtr::adopt() take ownership
  // without incrementing.
  mutable std::atomic<std::uint32_t> refcount_{1};

  // High bit of packed_tag_ = 1 means this node holds a value.
  // Low 31 bits = transient edit tag (0 = immutable).
  std::uint32_t packed_tag_{0};
  V value_{};

  using Prefix = SmallVector<std::byte, 24>;
  Prefix prefix;

  // Children: null for leaf nodes, heap-allocated for internal nodes.
  // 94% of nodes are leaves — they pay only 8 B (null pointer) instead
  // of 32 B for an empty SmallVector.
  using ChildSlot = std::pair<std::byte, IntrusivePtr<Node>>;
  using ChildVec = std::vector<ChildSlot>;
  std::unique_ptr<ChildVec> children_;

  void addref() const noexcept {
    refcount_.fetch_add(1, std::memory_order_relaxed);
  }
  void release() const noexcept {
    // acq_rel: ensures all writes to the node are visible before the
    // deleting thread runs the destructor.
    if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }

  [[nodiscard]] auto has_value() const noexcept -> bool {
    return (packed_tag_ >> 31) != 0;
  }
  [[nodiscard]] auto edit_tag() const noexcept -> std::uint32_t {
    return packed_tag_ & 0x7FFF'FFFFu;
  }
  void set_edit_tag(std::uint32_t tag) noexcept {
    packed_tag_ = (packed_tag_ & 0x8000'0000u) | (tag & 0x7FFF'FFFFu);
  }
  void set_value(V v) {
    value_ = std::move(v);
    packed_tag_ |= 0x8000'0000u;
  }
  void clear_value() noexcept { packed_tag_ &= 0x7FFF'FFFFu; }

  // -- Children accessors ---------------------------------------------------
  [[nodiscard]] auto child_count() const noexcept -> std::size_t {
    return children_ ? children_->size() : 0;
  }
  [[nodiscard]] auto has_children() const noexcept -> bool {
    return children_ && !children_->empty();
  }
  [[nodiscard]] auto child_at(std::size_t i) const -> const ChildSlot & {
    return (*children_)[i];
  }
  [[nodiscard]] auto child_at(std::size_t i) -> ChildSlot & {
    return (*children_)[i];
  }

  // Find child by transition byte. Children are sorted by transition byte.
  // Linear scan is used because prefix compression keeps child counts small
  // (typically 1–4), where it outperforms binary search.
  [[nodiscard]] auto find_child(std::byte b) const -> const ChildSlot * {
    if (!children_)
      return nullptr;
    for (auto &c : *children_) {
      if (c.first == b)
        return &c;
    }
    return nullptr;
  }

  [[nodiscard]] auto find_child_mut(std::byte b) -> ChildSlot * {
    if (!children_)
      return nullptr;
    for (auto &c : *children_) {
      if (c.first == b)
        return &c;
    }
    return nullptr;
  }

  // Insert child in sorted order by transition byte.
  void insert_child(std::byte b, IntrusivePtr<Node> child) {
    if (!children_)
      children_ = std::make_unique<ChildVec>();
    auto it = children_->begin();
    while (it != children_->end() && it->first < b)
      ++it;
    assert((it == children_->end() || it->first != b) &&
           "duplicate transition byte");
    children_->insert(it, {b, std::move(child)});
  }

  void remove_child(std::byte b) {
    if (!children_)
      return;
    for (auto it = children_->begin(); it != children_->end(); ++it) {
      if (it->first == b) {
        children_->erase(it);
        return;
      }
    }
  }

  // Deep clone of this node (not recursive — children are shared).
  [[nodiscard]] auto clone() const -> IntrusivePtr<Node> {
    auto n = make_intrusive<Node>();
    // Clear edit_tag in the clone; preserve has_value bit.
    n->packed_tag_ = packed_tag_ & 0x8000'0000u;
    n->value_ = value_;
    n->prefix = prefix;
    if (children_)
      n->children_ = std::make_unique<ChildVec>(*children_);
    return n;
  }

  // Clone and stamp with edit tag for transient ownership.
  [[nodiscard]] auto clone_for(std::uint32_t tag) const -> IntrusivePtr<Node> {
    auto n = clone();
    n->set_edit_tag(tag);
    return n;
  }
};

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
  // Size of the result is computed in O(N) after the merge.
  template <typename ResolveFunc>
  [[nodiscard]] static auto merge(const PersistentRadixTree &a,
                                  const PersistentRadixTree &b,
                                  ResolveFunc &&resolve)
      -> PersistentRadixTree;

  // Iteration
  [[nodiscard]] auto begin() const -> RadixTreeIterator<V>;
  [[nodiscard]] auto end() const -> std::default_sentinel_t {
    return std::default_sentinel;
  }

  [[nodiscard]] auto lower_bound(std::span<const std::byte> key) const
      -> RadixTreeIterator<V>;

private:
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
    auto remaining = key;
    const Node<V> *cur = node.get();
    while (cur) {
      auto prefix_span =
          std::span<const std::byte>{cur->prefix.data(), cur->prefix.size()};
      auto cpl = common_prefix_length(prefix_span, remaining);
      if (cpl < prefix_span.size()) {
        // Key diverges within this node's prefix — not found.
        return std::nullopt;
      }
      remaining = remaining.subspan(cpl);
      if (remaining.empty()) {
        if (cur->has_value())
          return cur->value_;
        return std::nullopt;
      }
      auto transition = remaining[0];
      remaining = remaining.subspan(1);
      auto *child = cur->find_child(transition);
      if (!child)
        return std::nullopt;
      cur = child->second.get();
    }
    return std::nullopt;
  }

  // -- set (returns new root + whether a new key was inserted) --
  static auto set_impl(const IntrusivePtr<Node<V>> &node,
                       std::span<const std::byte> key, V val)
      -> std::pair<IntrusivePtr<Node<V>>, bool> {
    if (!node) {
      // Create a leaf.
      auto leaf = make_intrusive<Node<V>>();
      leaf->prefix = typename Node<V>::Prefix{};
      for (auto b : key)
        leaf->prefix.push_back(b);
      leaf->set_value(std::move(val));
      return {std::move(leaf), true};
    }

    auto new_node = node->clone();
    auto prefix_span = std::span<const std::byte>{new_node->prefix.data(),
                                                  new_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split: divergence within this node's prefix.
      auto split = make_intrusive<Node<V>>();
      // Split node gets the common prefix.
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
        auto new_leaf = make_intrusive<Node<V>>();
        for (auto b : remaining.subspan(1))
          new_leaf->prefix.push_back(b);
        new_leaf->set_value(std::move(val));
        split->insert_child(new_transition, std::move(new_leaf));
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
    auto *existing_child = new_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, inserted] =
          set_impl(existing_child->second, child_key, std::move(val));
      existing_child->second = std::move(new_child);
      return {std::move(new_node), inserted};
    }
    // No child for this transition — create a leaf.
    auto leaf = make_intrusive<Node<V>>();
    for (auto b : child_key)
      leaf->prefix.push_back(b);
    leaf->set_value(std::move(val));
    new_node->insert_child(transition, std::move(leaf));
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
    auto *existing = node->find_child(transition);
    if (!existing)
      return {node, false};

    auto [new_child, removed] = erase_impl(existing->second, child_key);
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
    auto *child_slot = new_node->find_child_mut(transition);
    child_slot->second = std::move(new_child);
    // Path compression on the child side: child might now be a routing node
    // with 1 child and no value — but that's the child's responsibility,
    // already handled in the recursive call.
    return {std::move(new_node), true};
  }

  // Merge a routing node (no value) with its single child.
  // New prefix = node.prefix + transition_byte + child.prefix
  static auto merge_with_child(IntrusivePtr<Node<V>> node)
      -> IntrusivePtr<Node<V>> {
    assert(!node->has_value() && node->child_count() == 1);
    auto transition = node->child_at(0).first;
    auto child = node->child_at(0).second->clone();

    typename Node<V>::Prefix merged_prefix;
    for (auto b : node->prefix)
      merged_prefix.push_back(b);
    merged_prefix.push_back(transition);
    for (std::size_t i = 0; i < child->prefix.size(); ++i)
      merged_prefix.push_back(child->prefix[i]);
    child->prefix = std::move(merged_prefix);
    return child;
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
      auto split = make_intrusive<Node<V>>();
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
      auto new_b = b->clone();
      auto a_trimmed = a->clone();
      typename Node<V>::Prefix a_suffix;
      for (std::size_t i = cpl + 1; i < pa.size(); ++i)
        a_suffix.push_back(pa[i]);
      a_trimmed->prefix = std::move(a_suffix);

      auto *existing = new_b->find_child_mut(pa[cpl]);
      std::size_t overlaps = 0;
      if (existing) {
        auto [child, child_overlaps] =
            merge_impl(a_trimmed, existing->second, resolve);
        existing->second = std::move(child);
        overlaps = child_overlaps;
      } else {
        new_b->insert_child(pa[cpl], std::move(a_trimmed));
      }
      return {std::move(new_b), overlaps};
    }

    if (cpl < pb.size()) {
      // a's prefix is fully consumed — a's node sits *above* b in the trie.
      // Build result based on a; insert b under a at transition pb[cpl].
      auto new_a = a->clone();
      auto b_trimmed = b->clone();
      typename Node<V>::Prefix b_suffix;
      for (std::size_t i = cpl + 1; i < pb.size(); ++i)
        b_suffix.push_back(pb[i]);
      b_trimmed->prefix = std::move(b_suffix);

      auto *existing = new_a->find_child_mut(pb[cpl]);
      std::size_t overlaps = 0;
      if (existing) {
        auto [child, child_overlaps] =
            merge_impl(existing->second, b_trimmed, resolve);
        existing->second = std::move(child);
        overlaps = child_overlaps;
      } else {
        new_a->insert_child(pb[cpl], std::move(b_trimmed));
      }
      return {std::move(new_a), overlaps};
    }

    // Full prefix match — both nodes share the same compressed key prefix.
    // Clone a and fold b's value + children into it.
    auto merged = a->clone();
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
        auto [tb, child_b] = b->child_at(i);
        auto *slot = merged->find_child_mut(tb);
        if (slot) {
          auto [child, child_overlaps] =
              merge_impl(slot->second, child_b, resolve);
          slot->second = std::move(child);
          overlaps += child_overlaps;
        } else {
          // Disjoint subtree — share it in O(1), no clone needed.
          merged->insert_child(tb, child_b);
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
    if (!root_)
      return std::nullopt;
    return PersistentRadixTree<V>::get_impl(root_, key);
  }

  [[nodiscard]] auto contains(std::span<const std::byte> key) const -> bool {
    return get(key).has_value();
  }

  void set(std::span<const std::byte> key, V val) {
    assert(tag_ != 0 && "transient already consumed");
    auto [new_root, inserted] = set_transient(root_, key, std::move(val), tag_);
    root_ = std::move(new_root);
    if (inserted)
      ++size_;
  }

  auto erase(std::span<const std::byte> key) -> bool {
    assert(tag_ != 0 && "transient already consumed");
    if (!root_)
      return false;
    auto [new_root, removed] = erase_transient(root_, key, tag_);
    root_ = std::move(new_root);
    if (removed)
      --size_;
    return removed;
  }

  [[nodiscard]] auto persistent() && -> PersistentRadixTree<V> {
    tag_ = 0; // Retire the tag — nodes become immutable.
    return PersistentRadixTree<V>{std::move(root_), size_};
  }

private:
  IntrusivePtr<Node<V>> root_;
  std::size_t size_{0};
  std::uint32_t tag_{0};

  TransientRadixTree(IntrusivePtr<Node<V>> root, std::size_t sz,
                     std::uint32_t tag)
      : root_{std::move(root)}, size_{sz}, tag_{tag} {}

  // Ensure a node is owned by this transient session.
  // Requires both matching edit tag AND unique ownership (refcount == 1)
  // to allow in-place mutation. The refcount check defends against tag
  // wraparound after 2^31 transient sessions: even if an old node
  // happens to carry the same 31-bit tag, it will be cloned if shared.
  static auto ensure_mutable(const IntrusivePtr<Node<V>> &node,
                             std::uint32_t tag) -> IntrusivePtr<Node<V>> {
    if (node && node->edit_tag() == tag &&
        node->refcount_.load(std::memory_order_acquire) == 1)
      return node;
    if (!node) {
      auto n = make_intrusive<Node<V>>();
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
      auto leaf = make_intrusive<Node<V>>();
      leaf->set_edit_tag(tag);
      for (auto b : key)
        leaf->prefix.push_back(b);
      leaf->set_value(std::move(val));
      return {std::move(leaf), true};
    }

    auto mutable_node = ensure_mutable(node, tag);
    auto prefix_span = std::span<const std::byte>{mutable_node->prefix.data(),
                                                  mutable_node->prefix.size()};
    auto cpl = common_prefix_length(prefix_span, key);

    if (cpl < prefix_span.size()) {
      // Split.
      auto split = make_intrusive<Node<V>>();
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
        auto new_leaf = make_intrusive<Node<V>>();
        new_leaf->set_edit_tag(tag);
        for (auto b : remaining.subspan(1))
          new_leaf->prefix.push_back(b);
        new_leaf->set_value(std::move(val));
        split->insert_child(new_transition, std::move(new_leaf));
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
    auto *existing_child = mutable_node->find_child_mut(transition);
    if (existing_child) {
      auto [new_child, inserted] =
          set_transient(existing_child->second, child_key, std::move(val), tag);
      existing_child->second = std::move(new_child);
      return {std::move(mutable_node), inserted};
    }
    auto leaf = make_intrusive<Node<V>>();
    leaf->set_edit_tag(tag);
    for (auto b : child_key)
      leaf->prefix.push_back(b);
    leaf->set_value(std::move(val));
    mutable_node->insert_child(transition, std::move(leaf));
    return {std::move(mutable_node), true};
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
    auto *existing = node->find_child(transition);
    if (!existing)
      return {node, false};

    auto [new_child, removed] =
        erase_transient(existing->second, child_key, tag);
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
    auto *child_slot = mutable_node->find_child_mut(transition);
    child_slot->second = std::move(new_child);
    return {std::move(mutable_node), true};
  }

  static auto merge_with_child_transient(IntrusivePtr<Node<V>> node,
                                         std::uint32_t tag)
      -> IntrusivePtr<Node<V>> {
    assert(!node->has_value() && node->child_count() == 1);
    auto transition = node->child_at(0).first;
    auto child = ensure_mutable(node->child_at(0).second, tag);

    typename Node<V>::Prefix merged_prefix;
    for (std::size_t i = 0; i < node->prefix.size(); ++i)
      merged_prefix.push_back(node->prefix[i]);
    merged_prefix.push_back(transition);
    for (std::size_t i = 0; i < child->prefix.size(); ++i)
      merged_prefix.push_back(child->prefix[i]);
    child->prefix = std::move(merged_prefix);
    return child;
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
  // Truncate to 31 bits — tag 0 is reserved for "immutable" sentinel.
  auto raw = detail::next_edit_tag.fetch_add(1, std::memory_order_relaxed);
  auto tag = static_cast<std::uint32_t>(raw & 0x7FFF'FFFFu);
  if (tag == 0) [[unlikely]]
    tag = static_cast<std::uint32_t>(
        detail::next_edit_tag.fetch_add(1, std::memory_order_relaxed) &
        0x7FFF'FFFFu);
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
  using iterator_category = std::input_iterator_tag;
  using iterator_concept = std::input_iterator_tag;
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

  std::vector<Frame> stack_;
  std::vector<std::byte> current_key_;

  // Construct an iterator starting at begin (visit the whole tree).
  explicit RadixTreeIterator(IntrusivePtr<Node<V>> root) {
    if (!root)
      return;
    push_node(root, 0);
    if (!stack_.empty() && !stack_.back().node->has_value()) {
      advance();
    }
  }

  // Construct a lower_bound iterator.
  RadixTreeIterator(IntrusivePtr<Node<V>> root,
                    std::span<const std::byte> target) {
    stack_.reserve(16);
    current_key_.reserve(128);
    if (!root)
      return;
    auto at_target = seek(root, target);
    if (stack_.empty())
      return;
    if (at_target && stack_.back().node->has_value())
      return;
    advance();
  }

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
        auto &[transition, child] = frame.node->child_at(frame.child_idx);
        ++frame.child_idx;
        auto key_before = current_key_.size();
        current_key_.push_back(transition);
        push_node(child, key_before);
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
        auto cb = cur->child_at(i).first;
        if (cb < target_byte)
          continue;

        if (cb == target_byte) {
          // Exact match — push parent frame, descend into child.
          stack_.push_back({cur, i + 1, klb});
          klb = current_key_.size();
          current_key_.push_back(cb);
          cur = cur->child_at(i).second;
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
};

// Out-of-line: PersistentRadixTree::begin()
template <typename V>
auto PersistentRadixTree<V>::begin() const -> RadixTreeIterator<V> {
  return RadixTreeIterator<V>{root_};
}

// Out-of-line: PersistentRadixTree::lower_bound()
template <typename V>
auto PersistentRadixTree<V>::lower_bound(std::span<const std::byte> key) const
    -> RadixTreeIterator<V> {
  return RadixTreeIterator<V>{root_, key};
}

} // namespace bytecask
