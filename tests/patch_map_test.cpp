// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for bytecask.patch_map: every version, new and
// old, against a std::map model, across chain depths.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>
import bytecask;
import bytecask.patch_map;

namespace {

// A value with an additive field and a first-write-wins field, the two kinds
// file_stats has (byte counters, min_sequence).
struct Val {
  std::int64_t sum{0};
  std::int64_t first{0}; // 0 = unset
  auto operator==(const Val &) const -> bool = default;
};

struct ValDelta {
  std::int64_t add{0};
  std::int64_t first{0};
};

struct ValPolicy {
  using Delta = ValDelta;
  static void apply(Val &v, const Delta &d) {
    v.sum += d.add;
    if (v.first == 0) v.first = d.first;
  }
  static void combine(Delta &older, const Delta &d) {
    older.add += d.add;
    if (older.first == 0) older.first = d.first;
  }
};

using Model = std::map<std::uint32_t, Val>;

// The contract PatchPolicy states and the compiler cannot check: for random
// values and delta sequences, combining the deltas and applying the result
// equals applying them one by one, however the sequence is split, so that
// combine is associative as well. Every PatchPolicy gets a case below.
template <typename P, typename V, typename GenValue, typename GenDelta,
          typename Eq>
void check_patch_policy(GenValue gen_value, GenDelta gen_delta, Eq eq) {
  using D = typename P::Delta;
  std::mt19937_64 rng{3};
  for (int trial = 0; trial < 2000; ++trial) {
    const V v = gen_value(rng);
    std::vector<D> ds(1 + rng() % 6);
    for (auto &d : ds) d = gen_delta(rng);

    auto one_by_one = v;
    for (const auto &d : ds) P::apply(one_by_one, d);

    // Folded left to right: what a transient keeps for a key.
    auto left = ds[0];
    for (std::size_t i = 1; i < ds.size(); ++i) P::combine(left, ds[i]);
    auto via_left = v;
    P::apply(via_left, left);
    INFO("trial " << trial << ", " << ds.size() << " deltas, left fold");
    CHECK(eq(via_left, one_by_one));

    // Split at a random point, each side folded, then the two combined:
    // what merging runs that each hold a folded patch produces.
    const auto cut = 1 + rng() % ds.size();
    auto head = ds[0];
    for (std::size_t i = 1; i < cut; ++i) P::combine(head, ds[i]);
    if (cut < ds.size()) {
      auto tail = ds[cut];
      for (std::size_t i = cut + 1; i < ds.size(); ++i) P::combine(tail, ds[i]);
      P::combine(head, tail);
    }
    auto via_split = v;
    P::apply(via_split, head);
    INFO("split at " << cut);
    CHECK(eq(via_split, one_by_one));
  }
}

template <typename M>
void check(const M &map, const Model &model, std::uint32_t lo,
           std::uint32_t hi) {
  for (auto k = lo; k < hi; ++k) {
    const auto it = model.find(k);
    const auto got = map.get(k);
    if (it == model.end()) {
      CHECK_FALSE(got.has_value());
    } else {
      REQUIRE(got.has_value());
      CHECK(*got == it->second);
    }
  }
}

template <typename P>
void check_all(const P &map, const Model &model) {
  const std::vector<std::pair<std::uint32_t, Val>> want{model.begin(),
                                                        model.end()};
  CHECK(map.all() == want);
}

template <std::size_t Depth> struct Config {
  static constexpr std::size_t kDepth = Depth;
  using Map = bytecask::PersistentPatchMap<Val, ValPolicy, Depth>;
};

} // namespace

TEST_CASE("patch policy: the test policy keeps the combine contract",
          "[patch_map]") {
  check_patch_policy<ValPolicy, Val>(
      [](auto &rng) {
        return Val{static_cast<std::int64_t>(rng() % 100),
                   static_cast<std::int64_t>(rng() % 3)};
      },
      [](auto &rng) {
        return ValDelta{static_cast<std::int64_t>(rng() % 21) - 10,
                        static_cast<std::int64_t>(rng() % 4)};
      },
      [](const Val &a, const Val &b) { return a == b; });
}

TEST_CASE("patch policy: FileStatsPolicy keeps the combine contract",
          "[patch_map]") {
  using Policy = decltype(bytecask::EngineState::file_stats)::Policy;
  using Delta = Policy::Delta;
  // Small ranges, zero included, so first-batch-wins on min_sequence and
  // max on max_sequence meet each other's edge cases; the counters are
  // unsigned and wrap, which the contract must survive too.
  const auto small = [](auto &rng) { return std::uint64_t{rng() % 5}; };
  check_patch_policy<Policy, bytecask::FileStats>(
      [&](auto &rng) {
        return bytecask::FileStats{small(rng), small(rng), small(rng),
                                   small(rng), small(rng), small(rng)};
      },
      [&](auto &rng) {
        return Delta{.live_added = small(rng),
                     .live_removed = small(rng),
                     .total_added = small(rng),
                     .tombstone_added = small(rng),
                     .marker_added = small(rng),
                     .min_sequence = small(rng),
                     .max_sequence = small(rng)};
      },
      [](const bytecask::FileStats &a, const bytecask::FileStats &b) {
        return a.live_bytes == b.live_bytes &&
               a.total_bytes == b.total_bytes &&
               a.min_sequence == b.min_sequence &&
               a.max_sequence == b.max_sequence &&
               a.tombstone_bytes == b.tombstone_bytes &&
               a.marker_bytes == b.marker_bytes;
      });
}

TEMPLATE_TEST_CASE("patch map matches a std::map model, old versions too",
                   "[patch_map]",
                   Config<1>, Config<2>, Config<5>, Config<8>,
                   Config<32>) {
  using Map = typename TestType::Map;
  std::mt19937 rng{11};
  Model model;
  Map map;
  // Retained versions, as snapshots retain states, with what each held.
  std::vector<std::pair<Map, Model>> kept;
  // Keys in a narrow, rising window, as file ids are.
  std::uint32_t lo = 50;
  for (int round = 0; round < 400; ++round) {
    auto t = map.transient();
    const auto ops = static_cast<int>(rng() % 12); // some rounds write nothing
    for (int op = 0; op < ops; ++op) {
      const auto key = lo + static_cast<std::uint32_t>(rng() % 24);
      switch (rng() % 6) {
      case 0: {
        const Val v{static_cast<std::int64_t>(rng() % 100),
                    static_cast<std::int64_t>(rng() % 3)};
        t.set(key, v);
        model[key] = v;
        break;
      }
      case 1:
        t.erase(key);
        model.erase(key);
        break;
      default: {
        const ValDelta d{static_cast<std::int64_t>(rng() % 21) - 10,
                         static_cast<std::int64_t>(rng() % 4)};
        t.patch(key, d);
        if (auto it = model.find(key); it != model.end())
          ValPolicy::apply(it->second, d);
        break;
      }
      }
      // The transient sees its own writes.
      const auto got = t.get(key);
      const auto it = model.find(key);
      REQUIRE(got.has_value() == (it != model.end()));
      if (got) CHECK(*got == it->second);
    }
    map = std::move(t).persistent();
    REQUIRE(map.depth() <= TestType::kDepth);
    check(map, model, lo - 4, lo + 28);
    check_all(map, model);

    if (round % 7 == 0) kept.emplace_back(map, model);
    if (round % 50 == 49) lo += 8;
  }
  // Every retained version still reads as it did, through later writes and
  // squashes of the versions derived from it.
  for (const auto &[old, old_model] : kept) {
    check_all(old, old_model);
  }
}

TEST_CASE("patch map squashes when the chain would pass its depth",
          "[patch_map]") {
  using Map = bytecask::PersistentPatchMap<Val, ValPolicy, 3>;
  Map map;
  for (std::size_t i = 1; i <= 3; ++i) {
    auto t = map.transient();
    t.set(static_cast<std::uint32_t>(i), Val{1, 0});
    map = std::move(t).persistent();
    CHECK(map.depth() == i);
  }
  // A freeze with nothing written pushes no run.
  map = map.transient().persistent();
  CHECK(map.depth() == 3);

  auto t = map.transient();
  t.patch(1, ValDelta{5, 9});
  map = std::move(t).persistent();
  CHECK(map.depth() == 0);
  CHECK(map.get(1) == Val{6, 9});
  CHECK(map.get(2) == Val{1, 0});
}

TEST_CASE("patch map: a delta to an absent key is a no-op", "[patch_map]") {
  using Map = bytecask::PersistentPatchMap<Val, ValPolicy, 2>;
  Map map;
  auto t = map.transient();
  t.patch(7, ValDelta{3, 1});
  CHECK_FALSE(t.get(7).has_value());
  t.set(7, Val{10, 0});
  t.patch(7, ValDelta{3, 1});
  t.erase(7);
  t.patch(7, ValDelta{3, 1});
  map = std::move(t).persistent();
  CHECK_FALSE(map.get(7).has_value());
  CHECK(map.all().empty());
}
