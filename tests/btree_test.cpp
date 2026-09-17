// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for the persistent B+ tree

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
import bytecask.btree;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(std::span<const std::byte> bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i)
    s[i] = static_cast<char>(bytes[i]);
  return s;
}

using Tree = bytecask::PersistentBTree<int>;

// Every allocated node is either reachable from a live version or parked in
// the chain waiting for one to die; nothing else may be allocated, and
// nothing reachable may have been freed.
auto live_nodes(std::initializer_list<const Tree *> trees)
    -> std::set<const void *> {
  std::set<const void *> seen;
  for (const auto *t : trees)
    t->visit_nodes([&](const void *n) { seen.insert(n); });
  for (const auto *n : Tree::parked_nodes())
    seen.insert(n);
  return seen;
}

void check_accounting(std::initializer_list<const Tree *> trees) {
  auto &acc = bytecask::btree_detail::btree_accounting<int>();
  const auto outstanding = acc.allocated.load() - acc.freed.load();
  CHECK(outstanding == static_cast<std::int64_t>(live_nodes(trees).size()));
  CHECK(acc.retired.load() ==
        static_cast<std::int64_t>(Tree::parked_nodes().size()));
}

auto keys_of(const Tree &t) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto it = t.begin(); it != t.end(); ++it) {
    auto [k, v] = *it;
    out.push_back(to_string(k));
  }
  return out;
}

auto build(const std::vector<std::string> &keys) -> Tree {
  auto tr = Tree{}.transient();
  for (std::size_t i = 0; i < keys.size(); ++i)
    tr.set(to_bytes(keys[i]), static_cast<int>(i));
  return std::move(tr).persistent();
}

} // namespace

TEST_CASE("BTree empty tree", "[btree]") {
  const Tree t;
  CHECK(t.empty());
  CHECK(t.size() == 0U);
  CHECK_FALSE(t.get(to_bytes("any")).has_value());
  CHECK_FALSE(t.contains(to_bytes("any")));
  CHECK(t.begin() == t.end());
  CHECK(t.rbegin() == t.rend());
  CHECK(t.validate() == 0U);
  check_accounting({&t});
}

TEST_CASE("BTree single key overwrite and persistence", "[btree]") {
  const Tree t;
  auto t2 = t.set(to_bytes("hello"), 42);
  CHECK(t2.size() == 1U);
  REQUIRE(t2.get(to_bytes("hello")).has_value());
  CHECK(*t2.get(to_bytes("hello")) == 42);
  CHECK(t.empty());

  auto t3 = t2.set(to_bytes("hello"), 7);
  CHECK(*t3.get(to_bytes("hello")) == 7);
  CHECK(*t2.get(to_bytes("hello")) == 42);
  CHECK(t3.size() == 1U);
  CHECK(t3.validate() == 1U);
  check_accounting({&t, &t2, &t3});
}

TEST_CASE("BTree ordered iteration matches std::map", "[btree]") {
  std::vector<std::string> keys;
  for (int i = 0; i < 20000; ++i)
    keys.push_back("key_" + std::to_string((i * 7919) % 20000));
  auto t = build(keys);
  CHECK(t.size() == keys.size());
  CHECK(t.validate() >= 2U);

  std::map<std::string, int> oracle;
  for (std::size_t i = 0; i < keys.size(); ++i)
    oracle[keys[i]] = static_cast<int>(i);

  std::size_t n = 0;
  auto oit = oracle.begin();
  for (auto it = t.begin(); it != t.end(); ++it, ++oit, ++n) {
    auto [k, v] = *it;
    REQUIRE(oit != oracle.end());
    CHECK(to_string(k) == oit->first);
    CHECK(v == oit->second);
  }
  CHECK(n == oracle.size());

  // Reverse.
  auto roit = oracle.rbegin();
  n = 0;
  for (auto it = t.rbegin(); it != t.rend(); ++it, ++roit, ++n) {
    auto [k, v] = *it;
    CHECK(to_string(k) == roit->first);
  }
  CHECK(n == oracle.size());

  // Bidirectional: walk back from end.
  auto it = t.end_iter();
  --it;
  CHECK(to_string((*it).first) == oracle.rbegin()->first);
  ++it;
  CHECK(it == t.end());

  // lower_bound / upper_bound at present and absent keys.
  for (const auto probe : {"key_0", "key_1", "key_10000", "key_19999",
                           "key_", "key_2a", "kez", "a", "z", ""}) {
    auto lb = t.lower_bound(to_bytes(probe));
    auto olb = oracle.lower_bound(probe);
    if (olb == oracle.end()) {
      CHECK(lb == t.end());
    } else {
      REQUIRE(lb != t.end());
      CHECK(to_string((*lb).first) == olb->first);
    }
    auto ub = t.upper_bound(to_bytes(probe));
    auto oub = oracle.upper_bound(probe);
    if (oub == oracle.end()) {
      CHECK(ub == t.end());
    } else {
      REQUIRE(ub != t.end());
      CHECK(to_string((*ub).first) == oub->first);
    }
  }

  // Value iterators.
  n = 0;
  oit = oracle.begin();
  for (auto vit = t.value_begin(); vit != std::default_sentinel;
       ++vit, ++oit, ++n)
    CHECK(*vit == oit->second);
  CHECK(n == oracle.size());
  n = 0;
  roit = oracle.rbegin();
  for (auto vit = t.value_rbegin(); vit != std::default_sentinel;
       ++vit, ++roit, ++n)
    CHECK(*vit == roit->second);
  CHECK(n == oracle.size());
  {
    auto vit = t.value_rlower_bound(to_bytes("key_5"));
    REQUIRE(vit != std::default_sentinel);
    CHECK(*vit == oracle.find("key_5")->second);
    auto vit2 = t.value_rlower_bound(to_bytes("key_5!"));
    REQUIRE(vit2 != std::default_sentinel);
    CHECK(*vit2 == oracle.find("key_5")->second);
  }
  check_accounting({&t});
}

TEST_CASE("BTree erase down to empty", "[btree]") {
  std::vector<std::string> keys;
  for (int i = 0; i < 5000; ++i)
    keys.push_back("k" + std::to_string(i));
  auto t = build(keys);
  auto tr = t.transient();
  std::mt19937 rng{42};
  std::shuffle(keys.begin(), keys.end(), rng);
  std::size_t left = keys.size();
  for (const auto &k : keys) {
    CHECK(tr.erase(to_bytes(k)));
    CHECK_FALSE(tr.erase(to_bytes(k)));
    --left;
    CHECK(tr.size() == left);
    CHECK_FALSE(tr.contains(to_bytes(k)));
  }
  auto t2 = std::move(tr).persistent();
  CHECK(t2.empty());
  CHECK(t2.begin() == t2.end());
  CHECK(t2.validate() == 0U);
  CHECK(t.size() == 5000U); // the old version is intact
  CHECK(t.validate() >= 2U);
  check_accounting({&t, &t2});
}

TEST_CASE("BTree model: random operations against std::map", "[btree]") {
  std::mt19937_64 rng{7};
  std::map<std::string, int> oracle;
  Tree t;
  std::vector<std::pair<Tree, std::map<std::string, int>>> snapshots;

  for (int round = 0; round < 40; ++round) {
    auto tr = t.transient();
    for (int op = 0; op < 500; ++op) {
      const auto id = static_cast<int>(rng() % 3000);
      std::string key = "k" + std::to_string(id);
      if (rng() % 4 == 0)
        key += std::string(static_cast<std::size_t>(rng() % 60), 'x');
      const auto action = rng() % 3;
      if (action < 2) {
        const auto val = static_cast<int>(rng() % 100000);
        tr.set(to_bytes(key), val);
        oracle[key] = val;
      } else {
        const bool removed = tr.erase(to_bytes(key));
        CHECK(removed == (oracle.erase(key) > 0));
      }
    }
    t = std::move(tr).persistent();
    REQUIRE(t.size() == oracle.size());
    (void)t.validate();
    if (round % 5 == 0)
      snapshots.emplace_back(t, oracle);
  }

  auto check = [](const Tree &tree, const std::map<std::string, int> &m) {
    REQUIRE(tree.size() == m.size());
    auto oit = m.begin();
    for (auto it = tree.begin(); it != tree.end(); ++it, ++oit) {
      auto [k, v] = *it;
      REQUIRE(oit != m.end());
      CHECK(to_string(k) == oit->first);
      CHECK(v == oit->second);
    }
    CHECK(oit == m.end());
    for (const auto &[k, v] : m)
      CHECK(tree.get(to_bytes(k)) == v);
  };
  check(t, oracle);
  for (const auto &[snap, m] : snapshots)
    check(snap, m);

  // Drop the snapshots one at a time, in a shuffled order, and verify what
  // remains — this is what exercises parking and retraction.
  std::shuffle(snapshots.begin(), snapshots.end(), rng);
  while (!snapshots.empty()) {
    snapshots.pop_back();
    check(t, oracle);
    for (const auto &[snap, m] : snapshots)
      check(snap, m);
  }
  check_accounting({&t});
}

TEST_CASE("BTree prefix and head edge cases", "[btree]") {
  std::vector<std::string> keys = {
      "",       "a",      "ab",     "abc",    "abcd",    "abcde",
      "abcdef", "abd",    "b",      std::string("a\0b", 3),
      std::string("a\0", 2), std::string("\0", 1), std::string("\xff\xff", 2),
      "user::018f6e2c-1111-7000-8000-aaaaaaaaaaaa",
      "user::018f6e2c-1111-7000-8000-aaaaaaaaaaab",
      "user::018f6e2c-1111-7000-8000-aaaaaaaaaaac",
      "user::018f6e2c-2222-7000-8000-aaaaaaaaaaaa",
      "order::018f6e2c-1111-7000-8000-aaaaaaaaaaaa",
  };
  auto t = build(keys);
  (void)t.validate();
  std::vector<std::string> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  CHECK(keys_of(t) == sorted);
  for (std::size_t i = 0; i < keys.size(); ++i)
    CHECK(t.get(to_bytes(keys[i])) == static_cast<int>(i));
  CHECK_FALSE(t.contains(to_bytes("abcdefg")));
  CHECK_FALSE(t.contains(to_bytes("user::")));
  CHECK_FALSE(t.contains(to_bytes("user::018f6e2c-1111-7000-8000-aaaaaaaaaaa")));

  // Many keys equal in their first four suffix bytes: the tie loop.
  auto tr = t.transient();
  for (int i = 0; i < 300; ++i)
    tr.set(to_bytes("user::018f6e2c-1111-7000-8000-" + std::to_string(i)), i);
  auto t2 = std::move(tr).persistent();
  (void)t2.validate();
  for (int i = 0; i < 300; ++i)
    CHECK(t2.get(to_bytes("user::018f6e2c-1111-7000-8000-" +
                          std::to_string(i))) == i);
  CHECK(t2.size() == keys.size() + 300);
  check_accounting({&t, &t2});
}

TEST_CASE("BTree giant keys", "[btree]") {
  const std::string giant(65534, 'g');
  const std::string big(5000, 'b');
  auto tr = Tree{}.transient();
  tr.set(to_bytes(giant), 1);
  tr.set(to_bytes(big), 2);
  for (int i = 0; i < 2000; ++i)
    tr.set(to_bytes("small" + std::to_string(i)), i);
  for (int i = 0; i < 20; ++i)
    tr.set(to_bytes(std::string(static_cast<std::size_t>(3000 + i * 100), 'c') + std::to_string(i)), i);
  tr.set(to_bytes(giant + "!"), 3); // 65535 bytes: the ceiling, and a
                                     // separator as long as the key
  CHECK_THROWS_AS(tr.set(to_bytes(giant + "!!"), 4), std::length_error);
  auto t = std::move(tr).persistent();
  (void)t.validate();
  CHECK(t.get(to_bytes(giant)) == 1);
  CHECK(t.get(to_bytes(big)) == 2);
  CHECK(t.get(to_bytes(giant + "!")) == 3);
  CHECK(t.size() == 2023U);
  auto t2 = t.erase(to_bytes(giant));
  (void)t2.validate();
  CHECK_FALSE(t2.contains(to_bytes(giant)));
  CHECK(t2.get(to_bytes(giant + "!")) == 3);
  check_accounting({&t, &t2});
}

TEST_CASE("BTree ascending inserts fill nodes", "[btree]") {
  std::vector<std::string> keys;
  for (int i = 0; i < 100000; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08d", i);
    keys.emplace_back(buf);
  }
  auto t = build(keys);
  (void)t.validate();
  std::size_t nodes = 0;
  t.visit_nodes([&](const void *) { ++nodes; });
  // 8-byte keys, 16 B of overhead each after the shared prefix: a full leaf
  // holds well over 150; half-full leaves would need twice the nodes.
  CHECK(nodes < 100000 / 150);
  CHECK(keys_of(t) == keys);
  check_accounting({&t});
}

TEST_CASE("BTree chain contract and retraction", "[btree]") {
  std::vector<std::string> keys;
  for (int i = 0; i < 3000; ++i)
    keys.push_back("key_" + std::to_string(i));
  auto t1 = build(keys);

  SECTION("a second derivation from a version with a successor throws") {
    auto t2 = t1.set(to_bytes("key_7"), 7000);
    CHECK_THROWS_AS((void)t1.set(to_bytes("key_8"), 8000), std::logic_error);
    CHECK(*t2.get(to_bytes("key_7")) == 7000);
    CHECK(*t1.get(to_bytes("key_7")) == 7);
    check_accounting({&t1, &t2});
  }

  SECTION("a dropped successor is retracted and the base can derive again") {
    {
      auto t2 = t1.set(to_bytes("key_7"), 7000);
      CHECK(*t2.get(to_bytes("key_7")) == 7000);
    }
    auto t3 = t1.set(to_bytes("key_8"), 8000);
    CHECK(*t3.get(to_bytes("key_7")) == 7);
    CHECK(*t3.get(to_bytes("key_8")) == 8000);
    CHECK(t3.size() == 3000U);
    check_accounting({&t1, &t3});
  }

  SECTION("a dead segment of two behind a live base") {
    {
      auto t2 = t1.set(to_bytes("key_1"), 1);
      auto t3 = t2.set(to_bytes("key_2"), 2);
      auto t4 = t3.erase(to_bytes("key_3"));
      CHECK(t4.size() == 2999U);
    }
    auto t5 = t1.set(to_bytes("new"), 1);
    CHECK(t5.size() == 3001U);
    for (const auto &k : keys)
      CHECK(t5.contains(to_bytes(k)));
    (void)t5.validate();
    check_accounting({&t1, &t5});
  }

  SECTION("a long-lived snapshot holds only what it reaches") {
    auto &acc = bytecask::btree_detail::btree_accounting<int>();
    std::size_t base_nodes = 0;
    t1.visit_nodes([&](const void *) { ++base_nodes; });
    auto head = t1;
    for (int i = 0; i < 2000; ++i)
      head = head.set(to_bytes("key_" + std::to_string(i % 3000)), -i);
    const auto outstanding = acc.allocated.load() - acc.freed.load();
    std::size_t head_nodes = 0;
    head.visit_nodes([&](const void *) { ++head_nodes; });
    // Everything alive is reachable from t1 or head (the two live versions),
    // not the 2000 superseded paths in between.
    CHECK(outstanding <= static_cast<std::int64_t>(base_nodes + head_nodes));
    check_accounting({&t1, &head});
  }

  SECTION("transient dropped without publishing frees what it built") {
    {
      auto tr = t1.transient();
      for (int i = 0; i < 500; ++i)
        tr.set(to_bytes("tmp" + std::to_string(i)), i);
      CHECK(tr.size() == 3500U);
    }
    CHECK(t1.size() == 3000U);
    (void)t1.validate();
    check_accounting({&t1});
  }

  SECTION("an unchanged transient hands back its base") {
    auto tr = t1.transient();
    CHECK_FALSE(tr.erase(to_bytes("absent")));
    auto t2 = std::move(tr).persistent();
    CHECK(t2.size() == 3000U);
    auto t3 = t1.set(to_bytes("x"), 1); // t1 still has no successor
    CHECK(t3.size() == 3001U);
    check_accounting({&t1, &t2, &t3});
  }
}

TEST_CASE("BTree merge", "[btree]") {
  std::vector<std::string> ka;
  std::vector<std::string> kb;
  for (int i = 0; i < 3000; ++i) {
    if (i % 3 != 0)
      ka.push_back("key_" + std::to_string(i));
    if (i % 2 == 0)
      kb.push_back("key_" + std::to_string(i));
  }
  auto a = build(ka);
  auto b = build(kb);
  auto merged = Tree::merge(a, b, [](int x, int y) { return x + y; });
  (void)merged.validate();
  std::map<std::string, int> oracle;
  for (std::size_t i = 0; i < ka.size(); ++i)
    oracle[ka[i]] += static_cast<int>(i);
  for (std::size_t i = 0; i < kb.size(); ++i)
    oracle[kb[i]] += static_cast<int>(i);
  REQUIRE(merged.size() == oracle.size());
  for (const auto &[k, v] : oracle)
    CHECK(merged.get(to_bytes(k)) == v);
  CHECK(a.size() == ka.size());
  CHECK(b.size() == kb.size());
  check_accounting({&a, &b, &merged});
}

TEST_CASE("BTree upsert with predicate", "[btree]") {
  auto t = build({"a", "b", "c"});
  auto tr = t.transient();
  auto d1 = tr.upsert(to_bytes("b"), 100, [](int old, int) { return old < 5; });
  CHECK(d1 == 1);
  auto d2 = tr.upsert(to_bytes("b"), 200, [](int old, int) { return old < 5; });
  CHECK_FALSE(d2.has_value());
  auto d3 = tr.upsert(to_bytes("z"), 26, [](int, int) { return false; });
  CHECK_FALSE(d3.has_value());
  CHECK(tr.size() == 4U);
  auto t2 = std::move(tr).persistent();
  CHECK(*t2.get(to_bytes("b")) == 100);
  CHECK(*t2.get(to_bytes("z")) == 26);
  CHECK(*t.get(to_bytes("b")) == 1);
  check_accounting({&t, &t2});
}

TEST_CASE("BTree non-trivial value type", "[btree]") {
  using PTree = bytecask::PersistentBTree<std::shared_ptr<int>>;
  auto shared = std::make_shared<int>(5);
  {
    auto tr = PTree{}.transient();
    for (int i = 0; i < 2000; ++i)
      tr.set(to_bytes("k" + std::to_string(i)), shared);
    auto t = std::move(tr).persistent();
    CHECK(shared.use_count() == 2001);
    auto t2 = t.set(to_bytes("k5"), std::make_shared<int>(6));
    CHECK(**t2.get(to_bytes("k5")) == 6);
    CHECK(**t.get(to_bytes("k5")) == 5);
    auto t3 = t2.erase(to_bytes("k6"));
    CHECK_FALSE(t3.contains(to_bytes("k6")));
    (void)t3.validate();
  }
  CHECK(shared.use_count() == 1);
}

TEST_CASE("BTree transient iteration and reverse base", "[btree]") {
  auto t = build({"a", "b", "c", "d"});
  auto tr = t.transient();
  tr.set(to_bytes("bb"), 9);
  std::vector<std::string> seen;
  for (auto it = tr.lower_bound(to_bytes("b")); it != std::default_sentinel;
       ++it)
    seen.push_back(to_string((*it).first));
  CHECK(seen == std::vector<std::string>{"b", "bb", "c", "d"});
  auto t2 = std::move(tr).persistent();

  auto r = t2.rbegin();
  CHECK(to_string((*r).first) == "d");
  auto fwd = r.base();
  CHECK(fwd == t2.end());
  ++r;
  ++r;
  ++r;
  ++r;
  CHECK(to_string((*r).first) == "a");
  ++r;
  CHECK(r == t2.rend());
  ++r; // no-op past rend
  CHECK(r == t2.rend());
  CHECK(r.base() == t2.begin());
  check_accounting({&t, &t2});
}

TEST_CASE("BTree sequential stream below other keys fills leaves", "[btree]") {
  // An ascending stream inserted under a larger key family: the leaf that
  // receives the stream also holds the first key of the next family, so a
  // balanced split would leave every abandoned half small. Sequential-insert
  // detection and outlier isolation keep the fill near a full node.
  auto tr = Tree{}.transient();
  tr.set(to_bytes("zzzz"), 0);
  for (int i = 0; i < 20000; ++i) {
    char tmp[64];
    std::snprintf(tmp, sizeof tmp, "order::018f6e2c-0000-7000-8000-%012x", i);
    tr.set(to_bytes(tmp), i);
  }
  auto t = std::move(tr).persistent();
  const auto st = t.stats();
  CHECK(st.entries == 20001U);
  const auto fill = static_cast<double>(st.used_bytes) /
                    static_cast<double>(st.capacity_bytes);
  CHECK(fill > 0.6);
  CHECK(st.leaf_prefix_bytes / st.leaves >= 38U);
  check_accounting({&t});
}
