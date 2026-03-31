module;
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#include <iterator>
#include <optional>
#include <utility>

export module bytecask.persistent_ordered_map;

namespace bytecask {

// Forward declaration — needed so PersistentOrderedMap can name it in
// return types and friend declarations before the full definition.
export template <typename K, typename V>
  requires std::totally_ordered<K> && std::copyable<K> && std::copyable<V>
class OrderedMapTransient;

// ---------------------------------------------------------------------------
// PersistentOrderedMap<K, V>
//
// Immutable, sorted associative container backed by immer::flex_vector<Entry>
// (Radix Balanced Tree).  Every mutating operation returns a *new* version;
// the original is unchanged and shares structure with the result.
//
// Iteration order: ascending by K using operator<.
// Complexity: get/contains/lower_bound — O(log n); set/erase — O(log n).
// ---------------------------------------------------------------------------
export template <typename K, typename V>
  requires std::totally_ordered<K> && std::copyable<K> && std::copyable<V>
class PersistentOrderedMap {
public:
  struct Entry {
    K key;
    V value;
  };

  using flex_type = immer::flex_vector<Entry>;
  using const_iterator = typename flex_type::const_iterator;

  PersistentOrderedMap() = default;

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return data_.size();
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return data_.empty(); }

  // Returns the value for key, or std::nullopt if absent.
  [[nodiscard]] auto get(const K &key) const -> std::optional<V> {
    auto it = lower_bound(key);
    if (it != data_.end() && it->key == key)
      return it->value;
    return std::nullopt;
  }

  // Returns true if key is present.
  [[nodiscard]] auto contains(const K &key) const -> bool {
    auto it = lower_bound(key);
    return it != data_.end() && it->key == key;
  }

  // Returns an iterator to the first entry whose key is >= k.
  // Returns end() if no such entry exists.
  [[nodiscard]] auto lower_bound(const K &k) const -> const_iterator {
    return std::lower_bound(
        data_.begin(), data_.end(), k,
        [](const Entry &e, const K &key) { return e.key < key; });
  }

  [[nodiscard]] auto begin() const -> const_iterator { return data_.begin(); }
  [[nodiscard]] auto end() const -> const_iterator { return data_.end(); }

  // Returns a new version with key mapped to value (insert or overwrite).
  [[nodiscard]] auto set(K key, V value) const -> PersistentOrderedMap {
    auto it = lower_bound(key);
    // lower_bound returns an iterator in [begin, end]; distance is
    // non-negative.
    auto idx = static_cast<std::size_t>(std::distance(data_.begin(), it));
    if (it != data_.end() && it->key == key) {
      return PersistentOrderedMap(
          data_.set(idx, {std::move(key), std::move(value)}));
    }
    return PersistentOrderedMap(
        data_.insert(idx, {std::move(key), std::move(value)}));
  }

  // Returns a new version without key.  Returns an unchanged version if
  // key is absent (no-op).
  [[nodiscard]] auto erase(const K &key) const -> PersistentOrderedMap {
    auto it = lower_bound(key);
    if (it == data_.end() || it->key != key)
      return *this;
    auto idx = static_cast<std::size_t>(std::distance(data_.begin(), it));
    return PersistentOrderedMap(data_.erase(idx));
  }

  // Returns a short-lived mutable builder that starts from this version.
  // Call persistent() on the result to obtain the final immutable map.
  // O(1) — structural sharing, no copy of the data.
  [[nodiscard]] auto transient() const -> OrderedMapTransient<K, V>;

private:
  flex_type data_;

  explicit PersistentOrderedMap(flex_type data) : data_(std::move(data)) {}

  friend class OrderedMapTransient<K, V>;
};

// ---------------------------------------------------------------------------
// OrderedMapTransient<K, V>
//
// Short-lived mutable builder over a PersistentOrderedMap.  Obtain via
// PersistentOrderedMap::transient(); finalise via persistent().
//
// Backed by immer::flex_vector_transient, which mutates RBT nodes in place
// until frozen.  This makes batch-loading millions of pre-sorted keys
// dramatically faster than chaining persistent set() calls.
//
// Complexity per operation:
//   set — overwrite existing:  O(1) amortized (node mutated in place)
//   set — append at tail:      O(1) amortized (push_back, no split)
//   set — insert in middle:    O(log n) (split → push_back → rejoin)
//   erase:                     O(log n) (freeze → erase → refreeze)
//   persistent():              O(1) (freeze transient into immutable map)
// ---------------------------------------------------------------------------
export template <typename K, typename V>
  requires std::totally_ordered<K> && std::copyable<K> && std::copyable<V>
class OrderedMapTransient {
public:
  using Entry = typename PersistentOrderedMap<K, V>::Entry;

  // Inserts key → value, or overwrites the existing value.
  void set(K key, V value) {
    auto it =
        std::lower_bound(data_.begin(), data_.end(), key,
                         [](const Entry &e, const K &k) { return e.key < k; });
    auto idx = static_cast<std::size_t>(std::distance(data_.begin(), it));

    if (it != data_.end() && it->key == key) {
      // Overwrite: mutates the existing node in place. O(1) amortized.
      data_.set(idx, {std::move(key), std::move(value)});
    } else if (idx == data_.size()) {
      // Append at tail: no split needed. O(1) amortized.
      // This is the hot path for bulk-loading pre-sorted keys.
      data_.push_back({std::move(key), std::move(value)});
    } else {
      // Insert in the middle: freeze, split around idx, push new entry,
      // then rejoin the tail. O(log n).
      auto frozen = std::move(data_).persistent();
      auto tail = frozen.drop(idx).transient();
      data_ = frozen.take(idx)
                  .push_back({std::move(key), std::move(value)})
                  .transient();
      data_.append(tail);
    }
  }

  // Removes key.  No-op if absent.  O(log n).
  void erase(const K &key) {
    auto it =
        std::lower_bound(data_.begin(), data_.end(), key,
                         [](const Entry &e, const K &k) { return e.key < k; });
    if (it == data_.end() || it->key != key)
      return;
    auto idx = static_cast<std::size_t>(std::distance(data_.begin(), it));
    // Freeze, remove the element at idx, refreeze. O(log n).
    data_ = std::move(data_).persistent().erase(idx).transient();
  }

  // Consumes this transient and freezes it into an immutable map. O(1).
  [[nodiscard]] auto persistent() && -> PersistentOrderedMap<K, V> {
    return PersistentOrderedMap<K, V>(std::move(data_).persistent());
  }

private:
  using transient_type =
      typename PersistentOrderedMap<K, V>::flex_type::transient_type;
  transient_type data_;

  explicit OrderedMapTransient(transient_type data) : data_(std::move(data)) {}

  friend class PersistentOrderedMap<K, V>;
};

// Out-of-line definition: requires OrderedMapTransient to be complete.
template <typename K, typename V>
  requires std::totally_ordered<K> && std::copyable<K> && std::copyable<V>
auto PersistentOrderedMap<K, V>::transient() const
    -> OrderedMapTransient<K, V> {
  // data_.transient() copies the RBT root with O(1) structural sharing.
  return OrderedMapTransient<K, V>(data_.transient());
}

} // namespace bytecask
