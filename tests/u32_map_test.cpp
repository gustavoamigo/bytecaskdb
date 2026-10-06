// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for bytecask.u32_map: both implementations against
// a std::map model, and the direct-addressing table's trimming and snapshots.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <utility>
#include <vector>
import bytecask.u32_map;

// Both implementations provide the interface the engine relies on.
static_assert(bytecask::PersistentU32MapOf<bytecask::PersistentU32Map<int>, int>);
static_assert(bytecask::PersistentU32MapOf<bytecask::PersistentU32Table<int>, int>);
static_assert(bytecask::TransientU32MapOf<bytecask::TransientU32Map<int>, int>);
static_assert(bytecask::TransientU32MapOf<bytecask::TransientU32Table<int>, int>);

namespace {

template <typename P>
auto contents(const P &m) -> std::vector<std::pair<std::uint32_t, int>> {
  std::vector<std::pair<std::uint32_t, int>> out;
  for (auto it = m.begin(); it != std::default_sentinel; ++it) {
    const auto [k, v] = *it;
    out.emplace_back(k, v);
  }
  return out;
}

auto contents(const std::map<std::uint32_t, int> &m)
    -> std::vector<std::pair<std::uint32_t, int>> {
  return {m.begin(), m.end()};
}

} // namespace

TEMPLATE_TEST_CASE("u32 map matches a std::map model", "[u32_map]",
                   bytecask::PersistentU32Map<int>,
                   bytecask::PersistentU32Table<int>) {
  std::mt19937 rng{7};
  std::map<std::uint32_t, int> model;
  TestType map;
  // Keys in a narrow, shifting window, as file ids are: minted upwards,
  // erased from anywhere.
  std::uint32_t lo = 100;
  for (int round = 0; round < 200; ++round) {
    const auto before = map;
    const auto before_model = model;
    auto t = map.transient();
    for (int op = 0; op < 20; ++op) {
      const auto key = lo + static_cast<std::uint32_t>(rng() % 40);
      switch (rng() % 4) {
      case 0:
      case 1:
        t.set(key, static_cast<int>(rng()));
        model[key] = 0; // value filled below from the map
        model[key] = *t.get(key);
        break;
      case 2:
        CHECK(t.erase(key) == (model.erase(key) == 1));
        break;
      default:
        t.update(key, [](int &v) { ++v; });
        if (auto it = model.find(key); it != model.end()) ++it->second;
        break;
      }
      REQUIRE(t.contains(key) == model.contains(key));
    }
    map = std::move(t).persistent();
    REQUIRE(contents(map) == contents(model));
    REQUIRE(map.empty() == model.empty());
    for (const auto &[k, v] : model) REQUIRE(*map.get(k) == v);
    // The version before this round is untouched.
    REQUIRE(contents(before) == contents(before_model));
    if (rng() % 8 == 0) lo += 10;
  }
}

TEST_CASE("u32 table trims to the keys it holds", "[u32_map]") {
  auto t = bytecask::PersistentU32Table<int>{}.transient();
  t.set(10, 1);
  t.set(12, 2);
  t.set(7, 3); // below the base: the table grows downwards
  auto m = std::move(t).persistent();
  CHECK(contents(m) == std::vector<std::pair<std::uint32_t, int>>{
                           {7, 3}, {10, 1}, {12, 2}});
  CHECK(m.get(8) == nullptr);
  CHECK(m.get(13) == nullptr);
  CHECK(m.get(0) == nullptr);

  auto t2 = m.transient();
  CHECK(t2.erase(7));
  CHECK(t2.erase(12));
  CHECK_FALSE(t2.erase(12));
  auto m2 = std::move(t2).persistent();
  CHECK(contents(m2) ==
        std::vector<std::pair<std::uint32_t, int>>{{10, 1}});

  auto t3 = m2.transient();
  CHECK(t3.erase(10));
  auto m3 = std::move(t3).persistent();
  CHECK(m3.empty());
  CHECK(m3.begin() == m3.end());
  // An emptied table starts again wherever the next key lands.
  auto t4 = m3.transient();
  t4.set(500, 5);
  CHECK(contents(std::move(t4).persistent()) ==
        std::vector<std::pair<std::uint32_t, int>>{{500, 5}});
}

TEST_CASE("u32 table: snapshots and iterators outlive later writes",
          "[u32_map]") {
  auto t = bytecask::PersistentU32Table<std::shared_ptr<int>>{}.transient();
  t.set(1, std::make_shared<int>(10));
  t.set(2, std::make_shared<int>(20));
  const auto snap = std::move(t).persistent();
  const auto *one = snap.get(1);
  REQUIRE(one != nullptr);

  auto it = snap.begin();
  {
    auto later = snap.transient();
    CHECK(later.get(1) == one); // a transient reads the shared block
    later.erase(1);
    later.set(3, std::make_shared<int>(30));
    CHECK(later.get(1) == nullptr);
    (void)std::move(later).persistent();
  }
  CHECK(snap.get(1) == one);
  CHECK(**one == 10);
  REQUIRE(it != std::default_sentinel);
  CHECK((*it).first == 1);
  ++it;
  CHECK((*(*it).second) == 20);
  ++it;
  CHECK(it == std::default_sentinel);
}

TEST_CASE("u32 table: untouched transients and iterator equality",
          "[u32_map]") {
  auto t = bytecask::PersistentU32Table<int>{}.transient();
  t.set(4, 40);
  t.set(6, 60);
  const auto m = std::move(t).persistent();

  // A transient that only reads, or updates and erases absent keys, writes
  // nothing and hands back the version it came from.
  auto r = m.transient();
  CHECK(*r.get(4) == 40);
  r.update(5, [](int &v) { ++v; });
  CHECK_FALSE(r.erase(5));
  const auto same = std::move(r).persistent();
  CHECK(same.get(4) == m.get(4));

  auto a = m.begin();
  auto b = m.begin();
  CHECK(a == b);
  ++b;
  CHECK_FALSE(a == b);
  ++a;
  CHECK(a == b);
  ++a;
  ++b;
  CHECK(a == b); // both at the end
  CHECK(a == bytecask::U32TableIterator<int>{});
  CHECK_FALSE(m.begin() == bytecask::U32TableIterator<int>{});
}
