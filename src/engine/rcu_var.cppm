module;
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

export module bytecask.rcu_var;

namespace bytecask {

// ---------------------------------------------------------------------------
// RcuVar<T> — single-writer / multiple-reader publish/subscribe variable.
//
// Implements the Read-Copy-Update (RCU) pattern: the writer produces new
// immutable values via store(), and any number of readers obtain a
// reference-counted snapshot via load().
//
// store() is NOT thread-safe — callers must serialise writers externally
// (e.g. via a mutex). load() is lock-free and scales linearly with thread
// count.
//
// Memory ordering: store() does a release store on value_ followed by a
// release fetch_add on gen_. load()'s acquire load of gen_ synchronizes-with
// that fetch_add, establishing happens-before for the subsequent relaxed
// load of value_. See bytecask_design.md for the full proof.
//
// instance_id_ prevents TLS cache aliasing when the allocator reuses an
// address for a new RcuVar that coincidentally reaches the same gen_ value.
// ---------------------------------------------------------------------------
export template <typename T>
class RcuVar {
public:
  explicit RcuVar(std::shared_ptr<T> initial)
      : instance_id_{next_instance_id_.fetch_add(1, std::memory_order_relaxed)} {
    value_.store(std::move(initial), std::memory_order_release);
  }

  RcuVar(const RcuVar &) = delete;
  RcuVar &operator=(const RcuVar &) = delete;
  RcuVar(RcuVar &&) = delete;
  RcuVar &operator=(RcuVar &&) = delete;

  // Publish a new value. Not thread-safe — external serialisation required.
  void store(std::shared_ptr<T> value) {
    value_.store(std::move(value), std::memory_order_release);
    gen_.fetch_add(1, std::memory_order_release);
  }

  // Load the current value. Lock-free, scales with thread count.
  // Fast path (gen match): one acquire load + shared_ptr copy from TLS.
  // Slow path (first call or after a store): one relaxed atomic load of
  // value_, safe via the acquire on gen_.
  [[nodiscard]] auto load() const -> std::shared_ptr<T> {
    struct TlCache {
      std::uint64_t instance_id{std::numeric_limits<std::uint64_t>::max()};
      std::uint64_t gen{0};
      std::shared_ptr<T> snap;
    };
    thread_local TlCache cache;
    const auto current_gen = gen_.load(std::memory_order_acquire);
    if (cache.instance_id == instance_id_ && cache.gen == current_gen) {
      return cache.snap;
    }
    cache.instance_id = instance_id_;
    cache.snap = value_.load(std::memory_order_relaxed);
    cache.gen = current_gen;
    return cache.snap;
  }

private:
  static inline std::atomic<std::uint64_t> next_instance_id_{0};

  const std::uint64_t instance_id_;
  mutable std::atomic<std::shared_ptr<T>> value_;
  mutable std::atomic<std::uint64_t> gen_{1};
};

} // namespace bytecask
