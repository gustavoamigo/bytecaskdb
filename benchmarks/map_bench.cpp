#include <atomic>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <map>
#include <new>
#include <span>
#include <sstream>
#include <string>
#include <vector>
import bytecask.radix_tree;

// ---------------------------------------------------------------------------
// Global allocation tracker — counts bytes allocated via operator new.
// Thread-safe (atomic), but only tracks allocations that go through the
// replaceable global operator new, not container-internal allocators.
//
// Each allocation prepends a size_t header so that both sized and unsized
// operator delete can accurately subtract freed bytes from the running total.
// ---------------------------------------------------------------------------
namespace alloc_tracker {
std::atomic<std::size_t> g_allocated{0};
std::atomic<std::size_t> g_freed{0};

constexpr std::size_t kHeaderSize = alignof(std::max_align_t);

void reset() noexcept {
  g_allocated.store(0, std::memory_order_relaxed);
  g_freed.store(0, std::memory_order_relaxed);
}

auto net_bytes() noexcept -> std::size_t {
  return g_allocated.load(std::memory_order_relaxed) -
         g_freed.load(std::memory_order_relaxed);
}
} // namespace alloc_tracker

// Replaceable global operator new/delete.
void *operator new(std::size_t size) {
  alloc_tracker::g_allocated.fetch_add(size, std::memory_order_relaxed);
  void *raw = std::malloc(size + alloc_tracker::kHeaderSize);
  if (!raw)
    throw std::bad_alloc();
  // Store requested size in the header, return the user pointer after it.
  std::memcpy(raw, &size, sizeof(size));
  return static_cast<std::byte *>(raw) + alloc_tracker::kHeaderSize;
}

void operator delete(void *p) noexcept {
  if (!p)
    return;
  auto *raw = static_cast<std::byte *>(p) - alloc_tracker::kHeaderSize;
  std::size_t size = 0;
  std::memcpy(&size, raw, sizeof(size));
  alloc_tracker::g_freed.fetch_add(size, std::memory_order_relaxed);
  std::free(raw);
}

void operator delete(void *p, std::size_t /*size*/) noexcept {
  // Delegate to unsized delete which reads the header.
  ::operator delete(p);
}

namespace {

auto generate_keys(std::size_t n) -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    keys.push_back("key_" + std::to_string(i));
  return keys;
}

// Generate realistic prefix-heavy keys that mimic a database key directory.
// Produces keys like:
//   user::018f6e2c-0001-7000-8000-00000000002a
//   order::018f6e2c-0001-7000-8000-00000000002a
// The UUIDv7-like suffix shares a time prefix (first 8 hex digits), so the
// radix tree can compress:  "user::018f6e2c-" once for all user keys, etc.
auto generate_prefixed_keys(std::size_t n) -> std::vector<std::string> {
  static constexpr std::array prefixes = {
      "user::", "order::", "session::", "invoice::", "product::"};
  std::vector<std::string> keys;
  keys.reserve(n);
  auto per_prefix = n / prefixes.size();

  for (auto *pfx : prefixes) {
    for (std::size_t i = 0; i < per_prefix; ++i) {
      // UUIDv7-like: shared time prefix + sequential counter in lower bits.
      std::ostringstream oss;
      oss << pfx << "018f6e2c-" << std::hex << std::setfill('0') << std::setw(4)
          << (i >> 16) << "-7000-8000-" << std::setw(12) << (i & 0xFFFFFFFF);
      keys.push_back(oss.str());
    }
  }
  return keys;
}

auto to_bytes(const std::string &s) -> std::span<const std::byte> {
  return std::as_bytes(std::span{s.data(), s.size()});
}

// ===========================================================================
// Container adapters — normalize each container's API for generic benchmarks.
// ===========================================================================

struct RTreeAdapter {
  using key_type = std::string;
  using map_type = bytecask::PersistentRadixTree<int>;
  using transient_type = bytecask::TransientRadixTree<int>;

  static auto make_keys(const std::vector<std::string> &strs)
      -> std::vector<key_type> {
    return strs;
  }

  static auto build(const std::vector<key_type> &keys) -> map_type {
    auto t = map_type{};
    for (std::size_t i = 0; i < keys.size(); ++i)
      t = t.set(to_bytes(keys[i]), static_cast<int>(i));
    return t;
  }

  static auto transient_build(const std::vector<key_type> &keys) -> map_type {
    auto tr = map_type{}.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]), static_cast<int>(i));
    return std::move(tr).persistent();
  }

  static auto get(const map_type &m, const key_type &k) {
    return m.get(to_bytes(k));
  }

  static auto lower_bound(const map_type &m, const key_type &k) {
    return m.lower_bound(to_bytes(k));
  }

  static auto iterate_sum(const map_type &m) -> int {
    int sum = 0;
    for (auto it = m.begin(); it != m.end(); ++it) {
      auto [k, v] = *it;
      sum += v;
    }
    return sum;
  }

  static auto build_transient(const std::vector<key_type> &keys)
      -> transient_type {
    auto tr = map_type{}.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]), static_cast<int>(i));
    return tr;
  }

  static auto transient_get(const transient_type &tr, const key_type &k) {
    return tr.get(to_bytes(k));
  }

  // Snapshot an existing persistent tree, update all keys, return a new
  // persistent tree. Exercises the persistent -> transient -> mutate path
  // and the refcount==1 fast path in ensure_mutable.
  static auto transient_update(const map_type &base,
                               const std::vector<key_type> &keys) -> map_type {
    auto tr = base.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]), static_cast<int>(i) + 1);
    return std::move(tr).persistent();
  }
};

struct StdMapAdapter {
  using key_type = std::string;
  using map_type = std::map<std::string, int>;

  static auto make_keys(const std::vector<std::string> &strs)
      -> std::vector<key_type> {
    return strs;
  }

  static auto build(const std::vector<key_type> &keys) -> map_type {
    map_type m;
    for (std::size_t i = 0; i < keys.size(); ++i)
      m[keys[i]] = static_cast<int>(i);
    return m;
  }

  static auto get(const map_type &m, const key_type &k) { return m.find(k); }

  static auto lower_bound(const map_type &m, const key_type &k) {
    return m.lower_bound(k);
  }

  static auto iterate_sum(const map_type &m) -> int {
    int sum = 0;
    for (auto &[k, v] : m)
      sum += v;
    return sum;
  }
};

// ===========================================================================
// Generic benchmark templates
// ===========================================================================

template <typename A> void BM_Build(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state)
    benchmark::DoNotOptimize(A::build(keys));
}

template <typename A> void BM_TransientBuild(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state)
    benchmark::DoNotOptimize(A::transient_build(keys));
}

// Transient build with prefix-heavy keys — matches the ByteCask recovery path
// where all keys share a common type prefix (e.g. "user::", "order::").
void BM_TransientBuildPrefixed(benchmark::State &state) {
  auto keys = RTreeAdapter::make_keys(
      generate_prefixed_keys(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state)
    benchmark::DoNotOptimize(RTreeAdapter::transient_build(keys));
}

// Persistent -> transient -> update all keys -> persistent.
// The persistent tree is pre-built outside the loop; only the transient
// mutation round-trip is measured.
void BM_TransientUpdate(benchmark::State &state) {
  auto keys = RTreeAdapter::make_keys(
      generate_prefixed_keys(static_cast<std::size_t>(state.range(0))));
  auto base = RTreeAdapter::transient_build(keys);
  for (auto _ : state)
    benchmark::DoNotOptimize(RTreeAdapter::transient_update(base, keys));
}

template <typename A> void BM_Get(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::get(m, keys[idx % keys.size()]));
    ++idx;
  }
}

template <typename A> void BM_TransientGet(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  auto tr = A::build_transient(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::transient_get(tr, keys[idx % keys.size()]));
    ++idx;
  }
}

template <typename A> void BM_Iterate(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  for (auto _ : state)
    benchmark::DoNotOptimize(A::iterate_sum(m));
}

template <typename A> void BM_LowerBound(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::lower_bound(m, keys[idx % keys.size()]));
    ++idx;
  }
}

// ---------------------------------------------------------------------------
// Memory footprint: measures net heap bytes after building a container of N
// keys.  Reports bytes/key via a custom counter.
// ---------------------------------------------------------------------------
template <typename A> void BM_MemoryFootprint(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_keys(static_cast<std::size_t>(state.range(0))));
  std::size_t net = 0;
  for (auto _ : state) {
    state.PauseTiming();
    alloc_tracker::reset();
    state.ResumeTiming();

    auto m = A::build(keys);
    benchmark::DoNotOptimize(&m);

    state.PauseTiming();
    net = alloc_tracker::net_bytes();
    state.ResumeTiming();
  }
  state.counters["bytes_total"] = benchmark::Counter(static_cast<double>(net));
  state.counters["bytes_per_key"] = benchmark::Counter(
      static_cast<double>(net) / static_cast<double>(state.range(0)));
}

// ---------------------------------------------------------------------------
// Memory footprint with prefix-heavy keys (user::uuid, order::uuid, …).
// Shows radix tree prefix compression benefit vs flat key storage.
// ---------------------------------------------------------------------------
template <typename A> void BM_PrefixedMemory(benchmark::State &state) {
  auto keys = A::make_keys(
      generate_prefixed_keys(static_cast<std::size_t>(state.range(0))));
  std::size_t net = 0;
  for (auto _ : state) {
    state.PauseTiming();
    alloc_tracker::reset();
    state.ResumeTiming();

    auto m = A::build(keys);
    benchmark::DoNotOptimize(&m);

    state.PauseTiming();
    net = alloc_tracker::net_bytes();
    state.ResumeTiming();
  }
  state.counters["bytes_total"] = benchmark::Counter(static_cast<double>(net));
  state.counters["bytes_per_key"] = benchmark::Counter(
      static_cast<double>(net) / static_cast<double>(state.range(0)));
}

// ===========================================================================
// Merge benchmarks — disjoint vs overlapping, and split-build-merge vs linear
// ===========================================================================

// Merge-only: two disjoint N/2-key trees (zero overlap).
// Measures the cost of structural merge when all subtrees are adopted by
// pointer (best case — no conflict resolution).
void BM_MergeDisjoint(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_keys(n);
  std::vector<std::string> ka(all.begin(), all.begin() + std::ssize(all) / 2);
  std::vector<std::string> kb(all.begin() + std::ssize(all) / 2, all.end());
  auto ta = RTreeAdapter::transient_build(ka);
  auto tb = RTreeAdapter::transient_build(kb);
  auto resolve = [](const int &, const int &b) { return b; };
  for (auto _ : state)
    benchmark::DoNotOptimize(
        bytecask::PersistentRadixTree<int>::merge(ta, tb, resolve));
}

// Merge-only: two N/2-key trees with ~50% key overlap (worst realistic case).
// Half the keys exist in both trees and require conflict resolution.
void BM_MergeOverlapping(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_keys(n);
  auto quarter = std::ssize(all) / 4;
  std::vector<std::string> ka(all.begin(), all.begin() + quarter * 3);
  std::vector<std::string> kb(all.begin() + quarter, all.end());
  auto ta = RTreeAdapter::transient_build(ka);
  auto tb = RTreeAdapter::transient_build(kb);
  auto resolve = [](const int &, const int &b) { return b; };
  for (auto _ : state)
    benchmark::DoNotOptimize(
        bytecask::PersistentRadixTree<int>::merge(ta, tb, resolve));
}

// Full parallel-recovery simulation (measured sequentially):
//   build(N/2) + build(N/2) + merge
// Compare against TransientSet(N) to decide if split+merge is worthwhile.
// In true parallel execution, build times overlap → real time ≈ build(N/2) +
// merge.
void BM_SplitBuildMerge(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_keys(n);
  std::vector<std::string> ka(all.begin(), all.begin() + std::ssize(all) / 2);
  std::vector<std::string> kb(all.begin() + std::ssize(all) / 2, all.end());
  auto resolve = [](const int &, const int &b) { return b; };
  for (auto _ : state) {
    auto ta = RTreeAdapter::transient_build(ka);
    auto tb = RTreeAdapter::transient_build(kb);
    benchmark::DoNotOptimize(
        bytecask::PersistentRadixTree<int>::merge(ta, tb, resolve));
  }
}

// Split-build-merge with ~20% key overlap — simulates later rounds in the
// fan-in merge tree where partial overlap is expected (e.g. hot keys updated
// across multiple data files).
void BM_SplitBuildMergeOverlapping(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_keys(n);
  // 10% overlap on each side → 20% of keys shared between the two halves.
  auto overlap = std::ssize(all) / 10;
  auto mid = std::ssize(all) / 2;
  std::vector<std::string> ka(all.begin(), all.begin() + mid + overlap);
  std::vector<std::string> kb(all.begin() + mid - overlap, all.end());
  auto resolve = [](const int &, const int &b) { return b; };
  for (auto _ : state) {
    auto ta = RTreeAdapter::transient_build(ka);
    auto tb = RTreeAdapter::transient_build(kb);
    benchmark::DoNotOptimize(
        bytecask::PersistentRadixTree<int>::merge(ta, tb, resolve));
  }
}

// Same as above but with prefix-heavy keys — realistic recovery workload.
void BM_SplitBuildMergePrefixed(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_prefixed_keys(n);
  std::vector<std::string> ka(all.begin(), all.begin() + std::ssize(all) / 2);
  std::vector<std::string> kb(all.begin() + std::ssize(all) / 2, all.end());
  auto resolve = [](const int &, const int &b) { return b; };
  for (auto _ : state) {
    auto ta = RTreeAdapter::transient_build(ka);
    auto tb = RTreeAdapter::transient_build(kb);
    benchmark::DoNotOptimize(
        bytecask::PersistentRadixTree<int>::merge(ta, tb, resolve));
  }
}

// ===========================================================================
// Registration
// ===========================================================================

constexpr int kSmall = 1000;
constexpr int kMedium = 10000;
constexpr int kLarge = 100000;

// clang-format off
#define SIZES ->Arg(kSmall)->Arg(kMedium)->Arg(kLarge)
#define ITER_SIZES ->Arg(kSmall)->Arg(kMedium)

// Bulk insert
BENCHMARK(BM_Build<RTreeAdapter>)         ->Name("RadixTree/PersistentSet")        SIZES;
BENCHMARK(BM_TransientBuild<RTreeAdapter>)->Name("RadixTree/TransientSet")         SIZES;
BENCHMARK(BM_TransientBuildPrefixed)      ->Name("RadixTree/TransientSetPrefixed") SIZES;
BENCHMARK(BM_TransientUpdate)             ->Name("RadixTree/TransientUpdate")      SIZES;
BENCHMARK(BM_Build<StdMapAdapter>)        ->Name("StdMap/Set")                     SIZES;

// Memory footprint
BENCHMARK(BM_MemoryFootprint<RTreeAdapter>)->Name("RadixTree/Memory")  SIZES;
BENCHMARK(BM_MemoryFootprint<StdMapAdapter>)->Name("StdMap/Memory")    SIZES;

// Point lookups
BENCHMARK(BM_Get<RTreeAdapter>)           ->Name("RadixTree/Get")            SIZES;
BENCHMARK(BM_TransientGet<RTreeAdapter>)  ->Name("RadixTree/TransientGet")   SIZES;
BENCHMARK(BM_Get<StdMapAdapter>)          ->Name("StdMap/Get")               SIZES;

// Full iteration
BENCHMARK(BM_Iterate<RTreeAdapter>)       ->Name("RadixTree/Iterate")        ITER_SIZES;
BENCHMARK(BM_Iterate<StdMapAdapter>)      ->Name("StdMap/Iterate")           ITER_SIZES;

// lower_bound
BENCHMARK(BM_LowerBound<RTreeAdapter>)    ->Name("RadixTree/LowerBound")     SIZES;
BENCHMARK(BM_LowerBound<StdMapAdapter>)   ->Name("StdMap/LowerBound")        SIZES;

// Memory footprint with prefix-heavy keys (user::uuid, order::uuid, …)
BENCHMARK(BM_PrefixedMemory<RTreeAdapter>)   ->Name("RadixTree/PrefixedMemory")  SIZES;
BENCHMARK(BM_PrefixedMemory<StdMapAdapter>)  ->Name("StdMap/PrefixedMemory")     SIZES;

// Merge
BENCHMARK(BM_MergeDisjoint)                  ->Name("RadixTree/MergeDisjoint")           SIZES;
BENCHMARK(BM_MergeOverlapping)               ->Name("RadixTree/MergeOverlapping")        SIZES;
BENCHMARK(BM_SplitBuildMerge)                ->Name("RadixTree/SplitBuildMerge")              SIZES;
BENCHMARK(BM_SplitBuildMergeOverlapping)     ->Name("RadixTree/SplitBuildMergeOverlapping")   SIZES;
BENCHMARK(BM_SplitBuildMergePrefixed)        ->Name("RadixTree/SplitBuildMergePrefixed")      SIZES;

#undef SIZES
#undef ITER_SIZES
// clang-format on

} // namespace

BENCHMARK_MAIN();
