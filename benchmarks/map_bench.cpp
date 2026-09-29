// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — benchmarks for the adaptive radix tree and ordered map structures

#include "../tests/alloc_tracker.h"
#include "../tests/key_generators.h"
#include <algorithm>
#include <benchmark/benchmark.h>
#include <bit>
#include <functional>
#include <optional>
#include <random>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
import bytecask.btree;
import bytecask.blind_btree;
import bytecask.radix_tree;
import bytecask;

namespace {

using key_generators::generate_uniform_keys;
using key_generators::generate_prefixed_keys;
using key_generators::generate_binary_keys;

auto to_bytes(const std::string &s) -> std::span<const std::byte> {
  return std::as_bytes(std::span{s.data(), s.size()});
}

// ===========================================================================
// Container adapters — normalize each container's API for generic benchmarks.
// ===========================================================================

struct RTreeAdapter {
  using key_type = std::string;
  using map_type = bytecask::PersistentRadixTree<bytecask::KeyDirEntry>;
  using transient_type = bytecask::TransientRadixTree<bytecask::KeyDirEntry>;

  static auto make_keys(const std::vector<std::string> &strs)
      -> std::vector<key_type> {
    return strs;
  }

  static auto build(const std::vector<key_type> &keys) -> map_type {
    auto t = map_type{};
    for (std::size_t i = 0; i < keys.size(); ++i)
      t = t.set(to_bytes(keys[i]),
                bytecask::KeyDirEntry::make(i, 0, 0, 0));
    return t;
  }

  static auto transient_build(const std::vector<key_type> &keys) -> map_type {
    auto tr = map_type{}.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]),
             bytecask::KeyDirEntry::make(i, 0, 0, 0));
    return std::move(tr).persistent();
  }

  static auto get(const map_type &m, const key_type &k) {
    return m.get(to_bytes(k));
  }

  static auto lower_bound(const map_type &m, const key_type &k) {
    return m.lower_bound(to_bytes(k));
  }

  static auto iterate_sum(const map_type &m) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (auto it = m.begin(); it != m.end(); ++it) {
      auto [k, v] = *it;
      sum += v.sequence();
    }
    return sum;
  }

  static auto iterate_reverse_sum(const map_type &m) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (auto it = m.rbegin(); it != m.rend(); ++it) {
      auto [k, v] = *it;
      sum += v.sequence();
    }
    return sum;
  }

  static auto upper_bound(const map_type &m, const key_type &k) {
    return m.upper_bound(to_bytes(k));
  }

  static auto build_transient(const std::vector<key_type> &keys)
      -> transient_type {
    auto tr = map_type{}.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]),
             bytecask::KeyDirEntry::make(i, 0, 0, 0));
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
      tr.set(to_bytes(keys[i]),
             bytecask::KeyDirEntry::make(i + 1, 0, 0, 0));
    return std::move(tr).persistent();
  }

  static void transient_set(transient_type &tr, const key_type &k,
                            std::size_t i) {
    tr.set(to_bytes(k), bytecask::KeyDirEntry::make(i, 0, 0, 0));
  }

  template <typename Resolve>
  static auto merge(map_type a, map_type b, Resolve &&resolve) -> map_type {
    return map_type::merge(std::move(a), std::move(b),
                           std::forward<Resolve>(resolve));
  }
};

// Same surface over the persistent B+ tree (docs/persistent_btree_design.md).
struct BTreeAdapter {
  using key_type = std::string;
  using map_type = bytecask::PersistentBTree<bytecask::KeyDirEntry>;
  using transient_type = bytecask::TransientBTree<bytecask::KeyDirEntry>;

  static auto make_keys(const std::vector<std::string> &strs)
      -> std::vector<key_type> {
    return strs;
  }

  static auto build(const std::vector<key_type> &keys) -> map_type {
    auto t = map_type{};
    for (std::size_t i = 0; i < keys.size(); ++i)
      t = t.set(to_bytes(keys[i]), bytecask::KeyDirEntry::make(i, 0, 0, 0));
    return t;
  }

  static auto transient_build(const std::vector<key_type> &keys) -> map_type {
    auto tr = map_type{}.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]), bytecask::KeyDirEntry::make(i, 0, 0, 0));
    return std::move(tr).persistent();
  }

  static auto get(const map_type &m, const key_type &k) {
    return m.get(to_bytes(k));
  }

  static auto lower_bound(const map_type &m, const key_type &k) {
    return m.lower_bound(to_bytes(k));
  }

  static auto iterate_sum(const map_type &m) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (auto it = m.begin(); it != m.end(); ++it) {
      auto [k, v] = *it;
      sum += v.sequence();
    }
    return sum;
  }

  static auto iterate_reverse_sum(const map_type &m) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (auto it = m.rbegin(); it != m.rend(); ++it) {
      auto [k, v] = *it;
      sum += v.sequence();
    }
    return sum;
  }

  static auto upper_bound(const map_type &m, const key_type &k) {
    return m.upper_bound(to_bytes(k));
  }

  static auto build_transient(const std::vector<key_type> &keys)
      -> transient_type {
    auto tr = map_type{}.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]), bytecask::KeyDirEntry::make(i, 0, 0, 0));
    return tr;
  }

  static auto transient_get(const transient_type &tr, const key_type &k) {
    return tr.get(to_bytes(k));
  }

  static auto transient_update(const map_type &base,
                               const std::vector<key_type> &keys) -> map_type {
    auto tr = base.transient();
    for (std::size_t i = 0; i < keys.size(); ++i)
      tr.set(to_bytes(keys[i]),
             bytecask::KeyDirEntry::make(i + 1, 0, 0, 0));
    return std::move(tr).persistent();
  }

  static void transient_set(transient_type &tr, const key_type &k,
                            std::size_t i) {
    tr.set(to_bytes(k), bytecask::KeyDirEntry::make(i, 0, 0, 0));
  }

  template <typename Resolve>
  static auto merge(map_type a, map_type b, Resolve &&resolve) -> map_type {
    return map_type::merge(std::move(a), std::move(b),
                           std::forward<Resolve>(resolve));
  }
};

// The blind-leaf tree (docs/blind_leaf_btree_design.md) with a resolver that
// does no I/O: keys are interned in a vector before the timed region and a
// record's offset is its index there. What is timed is the tree plus one
// in-memory key read per operation that needs a key — the G2 gate.
struct BenchResolver {
  std::vector<std::string> store;
  std::unordered_map<std::string, std::uint32_t> index;

  auto intern(const std::string &k) -> bytecask::BlindRef {
    auto [it, inserted] =
        index.try_emplace(k, static_cast<std::uint32_t>(store.size()));
    if (inserted)
      store.push_back(k);
    return {0, it->second};
  }
  auto key_at(bytecask::BlindRef r) -> std::span<const std::byte> {
    return to_bytes(store[r.offset]);
  }
};

auto bench_resolver() -> BenchResolver & {
  static auto *r = new BenchResolver; // never destroyed: outlives every tree
  return *r;
}

template <std::size_t LeafBytes> struct BlindAdapter {
  struct key_type {
    std::string s;
    bytecask::BlindRef ref;
  };
  using map_type = bytecask::PersistentBlindBTree<LeafBytes>;
  using transient_type = bytecask::TransientBlindBTree<LeafBytes>;

  static auto make_keys(const std::vector<std::string> &strs)
      -> std::vector<key_type> {
    std::vector<key_type> out;
    out.reserve(strs.size());
    for (const auto &s : strs)
      out.push_back({s, bench_resolver().intern(s)});
    return out;
  }

  static auto build(const std::vector<key_type> &keys) -> map_type {
    auto t = map_type{};
    for (const auto &k : keys)
      t = t.set(to_bytes(k.s), k.ref, bench_resolver());
    return t;
  }

  static auto transient_build(const std::vector<key_type> &keys) -> map_type {
    auto tr = map_type{}.transient();
    for (const auto &k : keys)
      tr.set(to_bytes(k.s), k.ref, bench_resolver());
    return std::move(tr).persistent();
  }

  static auto get(const map_type &m, const key_type &k) {
    return m.get(to_bytes(k.s), bench_resolver());
  }

  static auto lower_bound(const map_type &m, const key_type &k) {
    return m.lower_bound(to_bytes(k.s), bench_resolver());
  }

  // Yields keys, as the other adapters' iterators do: one read per key.
  static auto iterate_sum(const map_type &m) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (auto it = m.begin(); it != m.end(); ++it)
      sum += (*it).offset + it.key(bench_resolver()).size();
    return sum;
  }

  static auto iterate_reverse_sum(const map_type &m) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (auto it = m.last(); it != m.end(); --it)
      sum += (*it).offset + it.key(bench_resolver()).size();
    return sum;
  }

  static auto build_transient(const std::vector<key_type> &keys)
      -> transient_type {
    auto tr = map_type{}.transient();
    for (const auto &k : keys)
      tr.set(to_bytes(k.s), k.ref, bench_resolver());
    return tr;
  }

  static auto transient_get(const transient_type &tr, const key_type &k) {
    return tr.get(to_bytes(k.s), bench_resolver());
  }

  static auto transient_update(const map_type &base,
                               const std::vector<key_type> &keys) -> map_type {
    auto tr = base.transient();
    for (const auto &k : keys)
      tr.set(to_bytes(k.s), k.ref, bench_resolver());
    return std::move(tr).persistent();
  }

  static void transient_set(transient_type &tr, const key_type &k,
                            std::size_t) {
    tr.set(to_bytes(k.s), k.ref, bench_resolver());
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

  static auto iterate_reverse_sum(const map_type &m) -> int {
    int sum = 0;
    for (auto it = m.rbegin(); it != m.rend(); ++it)
      sum += it->second;
    return sum;
  }

  static auto upper_bound(const map_type &m, const key_type &k) {
    return m.upper_bound(k);
  }
};

// ===========================================================================
// Generic benchmark templates
// ===========================================================================

template <typename A> void BM_Build(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state)
    benchmark::DoNotOptimize(A::build(keys));
}

template <typename A> void BM_TransientBuild(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state)
    benchmark::DoNotOptimize(A::transient_build(keys));
}

// Transient build with prefix-heavy keys — matches the ByteCask recovery path
// where all keys share a common type prefix (e.g. "user::", "order::").
template <typename A> void BM_TransientBuildPrefixed(benchmark::State &state) {
  auto keys = A::make_keys(
      generate_prefixed_keys(static_cast<std::size_t>(state.range(0))));
  for (auto _ : state)
    benchmark::DoNotOptimize(A::transient_build(keys));
}

// Persistent -> transient -> update all keys -> persistent.
// The persistent tree is pre-built outside the loop; only the transient
// mutation round-trip is measured.
template <typename A> void BM_TransientUpdate(benchmark::State &state) {
  auto keys = A::make_keys(
      generate_prefixed_keys(static_cast<std::size_t>(state.range(0))));
  auto base = A::transient_build(keys);
  for (auto _ : state)
    benchmark::DoNotOptimize(A::transient_update(base, keys));
}

// The engine's write path: a transient on a large existing tree, a lookup
// then a set for each key of a small batch of new keys, publish. Measures
// the tree's share of a group commit, not a build from scratch.
template <typename A> void BM_TransientInsertBatch(benchmark::State &state) {
  constexpr std::size_t kBatch = 100;
  auto n = static_cast<std::size_t>(state.range(0));
  auto keys = A::make_keys(generate_uniform_keys(n));
  auto base = A::transient_build(keys);
  std::size_t next = n;
  std::vector<std::string> names(kBatch);
  for (auto _ : state) {
    state.PauseTiming();
    for (auto &k : names)
      k = "key_" + std::to_string(next++);
    const auto batch = A::make_keys(names);
    state.ResumeTiming();
    auto tr = base.transient();
    for (std::size_t i = 0; i < kBatch; ++i) {
      benchmark::DoNotOptimize(A::transient_get(tr, batch[i]));
      A::transient_set(tr, batch[i], i);
    }
    benchmark::DoNotOptimize(std::move(tr).persistent());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

template <typename A> void BM_Get(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::get(m, keys[idx % keys.size()]));
    ++idx;
  }
}

// Lookups of keys that are not in the tree, each landing between two that
// are. For the blind tree this is the lookup that reads nothing: the
// candidate's fingerprint rejects it.
template <typename A> void BM_GetAbsent(benchmark::State &state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  auto present = generate_uniform_keys(n);
  std::vector<std::string> absent_strs;
  absent_strs.reserve(n);
  for (const auto &k : present)
    absent_strs.push_back(k + "x");
  auto keys = A::make_keys(present);
  auto absent = A::make_keys(absent_strs);
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::get(m, absent[idx % absent.size()]));
    ++idx;
  }
}

template <typename A> void BM_TransientGet(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  auto tr = A::build_transient(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::transient_get(tr, keys[idx % keys.size()]));
    ++idx;
  }
}

template <typename A> void BM_Iterate(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  for (auto _ : state)
    benchmark::DoNotOptimize(A::iterate_sum(m));
}

template <typename A> void BM_LowerBound(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::lower_bound(m, keys[idx % keys.size()]));
    ++idx;
  }
}

template <typename A> void BM_ReverseIterate(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  for (auto _ : state)
    benchmark::DoNotOptimize(A::iterate_reverse_sum(m));
}

template <typename A> void BM_UpperBound(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::upper_bound(m, keys[idx % keys.size()]));
    ++idx;
  }
}

// ---------------------------------------------------------------------------
// Binary-key variants of the ordered-access benchmarks.
//
// The uniform/prefixed shapes above are generated from a sequential numeric
// index, so their branching is digit-driven and bounded (<= 10-way): they
// never build a node with wide fanout. generate_binary_keys varies byte 0
// over the full 0x00-0xFF range, so the root becomes a full 256-child node
// past 256 keys. That is the only shape here that exercises the widest
// tier's ordinal-to-byte mapping, which is what descent (lower_bound) and
// ordered traversal (iterate) depend on.
// ---------------------------------------------------------------------------
template <typename A> void BM_LowerBoundBinary(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_binary_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  std::size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(A::lower_bound(m, keys[idx % keys.size()]));
    ++idx;
  }
}

template <typename A> void BM_IterateBinary(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_binary_keys(static_cast<std::size_t>(state.range(0))));
  auto m = A::build(keys);
  for (auto _ : state)
    benchmark::DoNotOptimize(A::iterate_sum(m));
}

// ---------------------------------------------------------------------------
// Memory footprint: measures net heap bytes after building a container of N
// keys.  Reports bytes/key via a custom counter.
// ---------------------------------------------------------------------------
template <typename A> void BM_MemoryFootprint(benchmark::State &state) {
  auto keys =
      A::make_keys(generate_uniform_keys(static_cast<std::size_t>(state.range(0))));
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
// pointer (best case — no conflict resolution). merge consumes its inputs,
// so the merge-only benchmarks rebuild them each iteration, and free the
// previous result, with the timer paused; what is timed is the merge and
// the freeing of the input nodes it does not reuse.
template <typename A> void BM_MergeDisjoint(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_uniform_keys(n);
  std::vector<std::string> ka(all.begin(), all.begin() + std::ssize(all) / 2);
  std::vector<std::string> kb(all.begin() + std::ssize(all) / 2, all.end());
  auto resolve = [](const bytecask::KeyDirEntry &,
                    const bytecask::KeyDirEntry &b) noexcept { return b; };
  typename A::map_type merged;
  for (auto _ : state) {
    state.PauseTiming();
    merged = {}; // free the previous result off the clock
    auto ta = A::transient_build(ka);
    auto tb = A::transient_build(kb);
    state.ResumeTiming();
    merged = A::merge(std::move(ta), std::move(tb), resolve);
    benchmark::DoNotOptimize(merged);
  }
}

// Merge-only: two N/2-key trees with ~50% key overlap (worst realistic case).
// Half the keys exist in both trees and require conflict resolution.
template <typename A> void BM_MergeOverlapping(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_uniform_keys(n);
  auto quarter = std::ssize(all) / 4;
  std::vector<std::string> ka(all.begin(), all.begin() + quarter * 3);
  std::vector<std::string> kb(all.begin() + quarter, all.end());
  auto resolve = [](const bytecask::KeyDirEntry &,
                    const bytecask::KeyDirEntry &b) noexcept { return b; };
  typename A::map_type merged;
  for (auto _ : state) {
    state.PauseTiming();
    merged = {}; // free the previous result off the clock
    auto ta = A::transient_build(ka);
    auto tb = A::transient_build(kb);
    state.ResumeTiming();
    merged = A::merge(std::move(ta), std::move(tb), resolve);
    benchmark::DoNotOptimize(merged);
  }
}

// Merge-only on binary keys — same ~50% overlap as BM_MergeOverlapping, but
// on the one shape that produces a full 256-child node (see the binary-key
// note above). merge_impl walks the right-hand node's children in order, so
// this is the merge-path counterpart to BM_LowerBoundBinary.
template <typename A> void BM_MergeOverlappingBinary(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_binary_keys(n);
  auto quarter = std::ssize(all) / 4;
  std::vector<std::string> ka(all.begin(), all.begin() + quarter * 3);
  std::vector<std::string> kb(all.begin() + quarter, all.end());
  auto resolve = [](const bytecask::KeyDirEntry &,
                    const bytecask::KeyDirEntry &b) noexcept { return b; };
  typename A::map_type merged;
  for (auto _ : state) {
    state.PauseTiming();
    merged = {}; // free the previous result off the clock
    auto ta = A::transient_build(ka);
    auto tb = A::transient_build(kb);
    state.ResumeTiming();
    merged = A::merge(std::move(ta), std::move(tb), resolve);
    benchmark::DoNotOptimize(merged);
  }
}

// Full parallel-recovery simulation (measured sequentially):
//   build(N/2) + build(N/2) + merge
// Compare against TransientSet(N) to decide if split+merge is worthwhile.
// In true parallel execution, build times overlap → real time ≈ build(N/2) +
// merge.
template <typename A> void BM_SplitBuildMerge(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_uniform_keys(n);
  std::vector<std::string> ka(all.begin(), all.begin() + std::ssize(all) / 2);
  std::vector<std::string> kb(all.begin() + std::ssize(all) / 2, all.end());
  auto resolve = [](const bytecask::KeyDirEntry &,
                    const bytecask::KeyDirEntry &b) noexcept { return b; };
  for (auto _ : state) {
    auto ta = A::transient_build(ka);
    auto tb = A::transient_build(kb);
    benchmark::DoNotOptimize(A::merge(std::move(ta), std::move(tb), resolve));
  }
}

// Split-build-merge with ~20% key overlap — simulates later rounds in the
// fan-in merge tree where partial overlap is expected (e.g. hot keys updated
// across multiple data files).
template <typename A> void BM_SplitBuildMergeOverlapping(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_uniform_keys(n);
  // 10% overlap on each side → 20% of keys shared between the two halves.
  auto overlap = std::ssize(all) / 10;
  auto mid = std::ssize(all) / 2;
  std::vector<std::string> ka(all.begin(), all.begin() + mid + overlap);
  std::vector<std::string> kb(all.begin() + mid - overlap, all.end());
  auto resolve = [](const bytecask::KeyDirEntry &,
                    const bytecask::KeyDirEntry &b) noexcept { return b; };
  for (auto _ : state) {
    auto ta = A::transient_build(ka);
    auto tb = A::transient_build(kb);
    benchmark::DoNotOptimize(A::merge(std::move(ta), std::move(tb), resolve));
  }
}

// Same as above but with prefix-heavy keys — realistic recovery workload.
template <typename A> void BM_SplitBuildMergePrefixed(benchmark::State &state) {
  auto n = static_cast<std::size_t>(state.range(0));
  auto all = generate_prefixed_keys(n);
  std::vector<std::string> ka(all.begin(), all.begin() + std::ssize(all) / 2);
  std::vector<std::string> kb(all.begin() + std::ssize(all) / 2, all.end());
  auto resolve = [](const bytecask::KeyDirEntry &,
                    const bytecask::KeyDirEntry &b) noexcept { return b; };
  for (auto _ : state) {
    auto ta = A::transient_build(ka);
    auto tb = A::transient_build(kb);
    benchmark::DoNotOptimize(A::merge(std::move(ta), std::move(tb), resolve));
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
BENCHMARK(BM_TransientBuildPrefixed<RTreeAdapter>)->Name("RadixTree/TransientSetPrefixed") SIZES;
BENCHMARK(BM_TransientUpdate<RTreeAdapter>)->Name("RadixTree/TransientUpdate")      SIZES;
BENCHMARK(BM_TransientInsertBatch<RTreeAdapter>)->Name("RadixTree/TransientInsertBatch") SIZES;
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

// upper_bound
BENCHMARK(BM_UpperBound<RTreeAdapter>)    ->Name("RadixTree/UpperBound")     SIZES;
BENCHMARK(BM_UpperBound<StdMapAdapter>)   ->Name("StdMap/UpperBound")        SIZES;

// Binary keys — the only shape that builds a full 256-child node
BENCHMARK(BM_LowerBoundBinary<RTreeAdapter>) ->Name("RadixTree/LowerBoundBinary") SIZES;
BENCHMARK(BM_LowerBoundBinary<StdMapAdapter>)->Name("StdMap/LowerBoundBinary")    SIZES;
BENCHMARK(BM_IterateBinary<RTreeAdapter>)    ->Name("RadixTree/IterateBinary")    ITER_SIZES;
BENCHMARK(BM_IterateBinary<StdMapAdapter>)   ->Name("StdMap/IterateBinary")       ITER_SIZES;

// Reverse iteration
BENCHMARK(BM_ReverseIterate<RTreeAdapter>)->Name("RadixTree/ReverseIterate") ITER_SIZES;
BENCHMARK(BM_ReverseIterate<StdMapAdapter>)->Name("StdMap/ReverseIterate")   ITER_SIZES;

// Memory footprint with prefix-heavy keys (user::uuid, order::uuid, …)
BENCHMARK(BM_PrefixedMemory<RTreeAdapter>)   ->Name("RadixTree/PrefixedMemory")  SIZES;
BENCHMARK(BM_PrefixedMemory<StdMapAdapter>)  ->Name("StdMap/PrefixedMemory")     SIZES;

// Merge
BENCHMARK(BM_MergeDisjoint<RTreeAdapter>)                  ->Name("RadixTree/MergeDisjoint")           SIZES;
BENCHMARK(BM_MergeOverlapping<RTreeAdapter>)               ->Name("RadixTree/MergeOverlapping")        SIZES;
BENCHMARK(BM_MergeOverlappingBinary<RTreeAdapter>)         ->Name("RadixTree/MergeOverlappingBinary")  SIZES;
BENCHMARK(BM_SplitBuildMerge<RTreeAdapter>)                ->Name("RadixTree/SplitBuildMerge")              SIZES;
BENCHMARK(BM_SplitBuildMergeOverlapping<RTreeAdapter>)     ->Name("RadixTree/SplitBuildMergeOverlapping")   SIZES;
BENCHMARK(BM_SplitBuildMergePrefixed<RTreeAdapter>)        ->Name("RadixTree/SplitBuildMergePrefixed")      SIZES;

// B+ tree, same rows
BENCHMARK(BM_Build<BTreeAdapter>)         ->Name("BTree/PersistentSet")        SIZES;
BENCHMARK(BM_TransientBuild<BTreeAdapter>)->Name("BTree/TransientSet")         SIZES;
BENCHMARK(BM_TransientBuildPrefixed<BTreeAdapter>)->Name("BTree/TransientSetPrefixed") SIZES;
BENCHMARK(BM_TransientUpdate<BTreeAdapter>)->Name("BTree/TransientUpdate")      SIZES;
BENCHMARK(BM_TransientInsertBatch<BTreeAdapter>)->Name("BTree/TransientInsertBatch") SIZES;
BENCHMARK(BM_MemoryFootprint<BTreeAdapter>)->Name("BTree/Memory")  SIZES;
BENCHMARK(BM_Get<BTreeAdapter>)           ->Name("BTree/Get")            SIZES;
BENCHMARK(BM_GetAbsent<BTreeAdapter>)     ->Name("BTree/GetAbsent")      SIZES;
BENCHMARK(BM_TransientGet<BTreeAdapter>)  ->Name("BTree/TransientGet")   SIZES;
BENCHMARK(BM_Iterate<BTreeAdapter>)       ->Name("BTree/Iterate")        ITER_SIZES;
BENCHMARK(BM_LowerBound<BTreeAdapter>)    ->Name("BTree/LowerBound")     SIZES;
BENCHMARK(BM_UpperBound<BTreeAdapter>)    ->Name("BTree/UpperBound")     SIZES;
BENCHMARK(BM_LowerBoundBinary<BTreeAdapter>) ->Name("BTree/LowerBoundBinary") SIZES;
BENCHMARK(BM_IterateBinary<BTreeAdapter>)    ->Name("BTree/IterateBinary")    ITER_SIZES;
BENCHMARK(BM_ReverseIterate<BTreeAdapter>)->Name("BTree/ReverseIterate") ITER_SIZES;
BENCHMARK(BM_PrefixedMemory<BTreeAdapter>)   ->Name("BTree/PrefixedMemory")  SIZES;
BENCHMARK(BM_MergeDisjoint<BTreeAdapter>)                  ->Name("BTree/MergeDisjoint")           SIZES;
BENCHMARK(BM_MergeOverlapping<BTreeAdapter>)               ->Name("BTree/MergeOverlapping")        SIZES;
BENCHMARK(BM_MergeOverlappingBinary<BTreeAdapter>)         ->Name("BTree/MergeOverlappingBinary")  SIZES;
BENCHMARK(BM_SplitBuildMerge<BTreeAdapter>)                ->Name("BTree/SplitBuildMerge")              SIZES;
BENCHMARK(BM_SplitBuildMergeOverlapping<BTreeAdapter>)     ->Name("BTree/SplitBuildMergeOverlapping")   SIZES;
BENCHMARK(BM_SplitBuildMergePrefixed<BTreeAdapter>)        ->Name("BTree/SplitBuildMergePrefixed")      SIZES;

// Blind-leaf tree. The number is the leaf allocation in bytes (a jemalloc
// size class): 640 = 48 entries, 1024 = 80 (the engine's), 1280 = 101,
// 2560 = 208.
#define BLIND_ROWS(BYTES)                                                                                        \
  BENCHMARK(BM_Build<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/PersistentSet") SIZES;                         \
  BENCHMARK(BM_TransientBuild<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/TransientSet") SIZES;                 \
  BENCHMARK(BM_TransientBuildPrefixed<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/TransientSetPrefixed") SIZES; \
  BENCHMARK(BM_TransientUpdate<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/TransientUpdate") SIZES;             \
  BENCHMARK(BM_TransientInsertBatch<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/TransientInsertBatch") SIZES;   \
  BENCHMARK(BM_MemoryFootprint<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/Memory") SIZES;                      \
  BENCHMARK(BM_Get<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/Get") SIZES;                                     \
  BENCHMARK(BM_GetAbsent<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/GetAbsent") SIZES;                         \
  BENCHMARK(BM_TransientGet<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/TransientGet") SIZES;                   \
  BENCHMARK(BM_Iterate<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/Iterate") ITER_SIZES;                        \
  BENCHMARK(BM_LowerBound<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/LowerBound") SIZES;                       \
  BENCHMARK(BM_LowerBoundBinary<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/LowerBoundBinary") SIZES;           \
  BENCHMARK(BM_IterateBinary<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/IterateBinary") ITER_SIZES;            \
  BENCHMARK(BM_ReverseIterate<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/ReverseIterate") ITER_SIZES;          \
  BENCHMARK(BM_PrefixedMemory<BlindAdapter<BYTES>>)->Name("Blind" #BYTES "/PrefixedMemory") SIZES;
BLIND_ROWS(640)
BLIND_ROWS(1024)
BLIND_ROWS(1280)
BLIND_ROWS(2560)
#undef BLIND_ROWS

#undef SIZES
#undef ITER_SIZES
// clang-format on


// ===========================================================================
// PROTOTYPE — a buffered blind tree, to price an idea before designing it
// (docs/commit_path_scaling_design.md): the serial section appends each change
// to a delta table instead of updating the tree, and a merger folds the table
// into the tree later, in sorted batches. A wrapper: the blind tree is used
// as is. Three costs decide it, each benchmarked below against the tree
// alone, on a 1M-key tree:
//   BufWrite  the writer's cost per change (what leaves the serial section);
//   BufMerge  the merger's cost per change, sorted batches vs arrival order;
//   BufGet / BufRange50  what readers pay for consulting the table.
// Single-threaded: it prices operations; the concurrent protocol (published
// prefixes, per-key chains for older versions) is not built.
// ===========================================================================

using BlindA = BlindAdapter<1024>;
using BlindTree = bytecask::PersistentBlindBTree<1024>;
constexpr std::size_t kBufBase = 1'000'000;

auto key_hash(std::span<const std::byte> k) -> std::uint64_t {
  return std::hash<std::string_view>{}(
      std::string_view{reinterpret_cast<const char *>(k.data()), k.size()});
}

// Append-only log of (key hash, record) with an open-addressing index on the
// hash; the index keeps each key's newest entry. A hash match is confirmed by
// reading the record's key, as the blind tree confirms a fingerprint.
struct DeltaTable {
  struct Entry {
    std::uint64_t h;
    bytecask::BlindRef ref;
  };
  std::vector<Entry> log;
  std::vector<std::uint32_t> slots;  // log index + 1; 0 is empty
  std::uint64_t mask;

  explicit DeltaTable(std::size_t capacity)
      : slots(std::bit_ceil(capacity * 2)), mask(slots.size() - 1) {
    log.reserve(capacity);
  }
  void clear() {
    log.clear();
    std::ranges::fill(slots, 0u);
  }
  auto slot_of(std::span<const std::byte> key, std::uint64_t h) const -> std::size_t {
    for (auto i = h & mask;; i = (i + 1) & mask) {
      const auto s = slots[i];
      if (s == 0) return i;
      const auto &e = log[s - 1];
      if (e.h == h && std::ranges::equal(bench_resolver().key_at(e.ref), key)) return i;
    }
  }
  auto find(std::span<const std::byte> key) const -> std::optional<bytecask::BlindRef> {
    const auto s = slots[slot_of(key, key_hash(key))];
    if (s == 0) return std::nullopt;
    return log[s - 1].ref;
  }
  void put(std::span<const std::byte> key, bytecask::BlindRef ref) {
    const auto h = key_hash(key);
    const auto i = slot_of(key, h);
    log.push_back({h, ref});
    slots[i] = static_cast<std::uint32_t>(log.size());
  }
};

// The shared base: a 1M-key blind tree, built once.
struct BufBase {
  std::vector<BlindA::key_type> keys;
  BlindTree tree;
};
auto buf_base() -> const BufBase & {
  static auto *b = [] {
    auto *p = new BufBase;  // never destroyed
    p->keys = BlindA::make_keys(generate_uniform_keys(kBufBase));
    p->tree = BlindA::transient_build(p->keys);
    return p;
  }();
  return *b;
}
auto fresh_keys(std::size_t n, std::size_t &next) -> std::vector<BlindA::key_type> {
  std::vector<std::string> names(n);
  for (auto &k : names) k = "fresh_" + std::to_string(next++);
  return BlindA::make_keys(names);
}

// Writer: one batch of 100 inserts of new keys, as a group commit applies
// them. mode 0: today — a transient on the tree, get then set per key,
// publish. mode 1: the table, with the tree lookup today's check needs
// (absent from the table, then from the tree). mode 2: the table alone —
// the check answered by the table, if it holds every change since the
// snapshot.
void BM_BufWrite(benchmark::State &state) {
  const auto mode = state.range(0);
  const auto &b = buf_base();
  constexpr std::size_t kBatch = 100;
  constexpr std::size_t kTableCap = 16384;
  DeltaTable table{kTableCap};
  std::size_t next = 0;
  for (auto _ : state) {
    state.PauseTiming();
    const auto batch = fresh_keys(kBatch, next);
    if (table.log.size() + kBatch > kTableCap) table.clear();
    state.ResumeTiming();
    if (mode == 0) {
      auto tr = b.tree.transient();
      for (const auto &k : batch) {
        benchmark::DoNotOptimize(BlindA::transient_get(tr, k));
        tr.set(to_bytes(k.s), k.ref, bench_resolver());
      }
      benchmark::DoNotOptimize(std::move(tr).persistent());
    } else {
      for (const auto &k : batch) {
        const auto key = to_bytes(k.s);
        benchmark::DoNotOptimize(table.find(key));
        if (mode == 1) benchmark::DoNotOptimize(b.tree.get(key, bench_resolver()));
        table.put(key, k.ref);
      }
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

// Merger: fold D changes into the tree in one transient. range(1) = 0 applies
// them in arrival order, 1 sorts them by key first (the sort is timed: the
// merger has to do it). range(2) = 0 inserts new keys, 1 updates existing
// ones. D = 400 is today's batch: ~20 commits of ~20 writes.
void BM_BufMerge(benchmark::State &state) {
  const auto d = static_cast<std::size_t>(state.range(0));
  const bool sorted = state.range(1) != 0;
  const bool updates = state.range(2) != 0;
  const auto &b = buf_base();
  std::size_t next = 0;
  std::mt19937_64 rng{42};
  for (auto _ : state) {
    state.PauseTiming();
    std::vector<BlindA::key_type> changes;
    if (updates) {
      changes.reserve(d);
      for (std::size_t i = 0; i < d; ++i)
        changes.push_back(b.keys[rng() % b.keys.size()]);
    } else {
      changes = fresh_keys(d, next);
      std::ranges::shuffle(changes, rng);
    }
    state.ResumeTiming();
    if (sorted)
      std::ranges::sort(changes, {}, [](const BlindA::key_type &k) -> const std::string & { return k.s; });
    auto tr = b.tree.transient();
    for (const auto &k : changes) tr.set(to_bytes(k.s), k.ref, bench_resolver());
    benchmark::DoNotOptimize(std::move(tr).persistent());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(d));
}

// A table of D changes: half updates of existing keys, half new keys.
auto filled_table(std::size_t d) -> DeltaTable {
  const auto &b = buf_base();
  DeltaTable t{std::max<std::size_t>(d, 16)};
  std::size_t next = 1'000'000'000;
  std::mt19937_64 rng{7};
  const auto fresh = fresh_keys(d / 2, next);
  for (std::size_t i = 0; i < d; ++i) {
    const auto &k = i % 2 == 0 ? b.keys[rng() % b.keys.size()] : fresh[i / 2];
    t.put(to_bytes(k.s), k.ref);
  }
  return t;
}

// Point reads of existing keys with a table of D changes in front of the
// tree: a probe, then the tree when the table misses (most keys). D = 0 is
// the tree alone, with no probe.
void BM_BufGet(benchmark::State &state) {
  const auto d = static_cast<std::size_t>(state.range(0));
  const auto &b = buf_base();
  auto table = filled_table(d);
  std::mt19937_64 rng{3};
  for (auto _ : state) {
    const auto &k = b.keys[rng() % b.keys.size()];
    const auto key = to_bytes(k.s);
    if (d > 0) {
      if (auto hit = table.find(key)) {
        benchmark::DoNotOptimize(hit);
        continue;
      }
    }
    benchmark::DoNotOptimize(b.tree.get(key, bench_resolver()));
  }
}

// A 50-key range scan with a table of D changes merged in. range(1) = 0 finds
// the table's entries in range by scanning it (the table as built: unsorted);
// 1 by binary search in a sorted copy made outside the timed region (what a
// sorted table would cost readers — keeping it sorted is not priced here).
void BM_BufRange50(benchmark::State &state) {
  const auto d = static_cast<std::size_t>(state.range(0));
  const bool sorted_table = state.range(1) != 0;
  const auto &b = buf_base();
  auto table = filled_table(d);
  std::vector<std::string> sorted_keys;
  for (const auto &e : table.log) {
    const auto k = bench_resolver().key_at(e.ref);
    sorted_keys.emplace_back(reinterpret_cast<const char *>(k.data()), k.size());
  }
  std::ranges::sort(sorted_keys);
  std::mt19937_64 rng{5};
  std::vector<std::string_view> in_range;
  for (auto _ : state) {
    const auto &start = b.keys[rng() % b.keys.size()];
    auto it = b.tree.lower_bound(to_bytes(start.s), bench_resolver());
    std::uint64_t sum = 0;
    std::string last;
    for (int i = 0; i < 50 && it != std::default_sentinel; ++i, ++it) {
      const auto k = it.key(bench_resolver());
      sum += (*it).offset + k.size();
      if (i == 49) last.assign(reinterpret_cast<const char *>(k.data()), k.size());
    }
    if (d > 0) {
      const std::string_view lo{start.s};
      in_range.clear();
      if (sorted_table) {
        auto first = std::ranges::lower_bound(sorted_keys, lo);
        for (; first != sorted_keys.end() && *first <= last; ++first) in_range.push_back(*first);
      } else {
        for (const auto &e : table.log) {
          const auto kb = bench_resolver().key_at(e.ref);
          const std::string_view k{reinterpret_cast<const char *>(kb.data()), kb.size()};
          if (k >= lo && k <= last) in_range.push_back(k);
        }
        std::ranges::sort(in_range);
      }
      sum += in_range.size();
    }
    benchmark::DoNotOptimize(sum);
  }
}

BENCHMARK(BM_BufWrite)->Name("BufBlind/Write")->ArgName("mode")->Arg(0)->Arg(1)->Arg(2)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_BufMerge)->Name("BufBlind/Merge")->ArgNames({"d", "sorted", "updates"})
    ->Args({400, 0, 0})->Args({400, 1, 0})->Args({10000, 0, 0})->Args({10000, 1, 0})->Args({100000, 1, 0})
    ->Args({400, 0, 1})->Args({400, 1, 1})->Args({10000, 0, 1})->Args({10000, 1, 1})->Args({100000, 1, 1})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_BufGet)->Name("BufBlind/Get")->ArgName("d")->Arg(0)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK(BM_BufRange50)->Name("BufBlind/Range50")->ArgNames({"d", "sorted"})
    ->Args({0, 0})->Args({1000, 0})->Args({10000, 0})->Args({1000, 1})->Args({10000, 1})->Args({100000, 1});

} // namespace


BENCHMARK_MAIN();
