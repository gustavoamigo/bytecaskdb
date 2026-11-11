// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for the blind-leaf B+ tree
// (docs/blind_leaf_btree_design.md). The tree reads keys through a resolver;
// these tests use an in-memory one that also counts reads.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
import bytecask.btree;
import bytecask.blind_btree;

namespace {

using bytecask::BlindRef;
namespace bd = bytecask::blind_detail;

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

// Keys live in a vector; a record's offset is its index there. Each key is
// interned once, so a key keeps one location across the whole test, as a
// record does until it is rewritten.
struct MemResolver {
  std::vector<std::string> store;
  std::map<std::string, std::uint32_t, std::less<>> index;
  std::size_t reads{0};

  auto ref(const std::string &k) -> BlindRef {
    auto [it, inserted] =
        index.try_emplace(k, static_cast<std::uint32_t>(store.size()));
    if (inserted)
      store.push_back(k);
    return {1, it->second};
  }
  // A new record for k at a new location, as an overwrite writes.
  auto fresh(const std::string &k) -> BlindRef {
    store.push_back(k);
    index[k] = static_cast<std::uint32_t>(store.size() - 1);
    return {1, index[k]};
  }
  auto key_at(BlindRef r) -> std::span<const std::byte> {
    ++reads;
    return to_bytes(store.at(r.offset));
  }
};

// 48 entries per leaf: small leaves split often, so a few hundred keys make
// a tree three or four levels deep.
using SmallTree = bytecask::PersistentBlindBTree<640>;
using Tree = bytecask::PersistentBlindBTree<bytecask::kBlindLeafBytes>;
using SmallLeaf = bd::Leaf<640>;
using SmallTransient = bytecask::TransientBlindBTree<640>;

auto to_string(std::span<const std::byte> bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i)
    s[i] = static_cast<char>(bytes[i]);
  return s;
}

auto keys_of(const SmallTree &t, MemResolver &res) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto it = t.begin(); it != t.end(); ++it) {
    out.push_back(to_string(it.key(res)));
  }
  return out;
}

auto rkeys_of(const SmallTree &t, MemResolver &res) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto it = t.last(); it != t.end(); --it) {
    out.push_back(to_string(it.key(res)));
  }
  return out;
}

// Short keys over {0x00, 0x01, 'a', 0xFF}: prefixes of each other, embedded
// and trailing NULs, and single-bit differences are all common.
auto nasty_key(std::mt19937_64 &rng, std::size_t max_len) -> std::string {
  static constexpr char kAlphabet[] = {'\0', '\x01', 'a', '\xFF'};
  std::uniform_int_distribution<std::size_t> len{0, max_len};
  std::uniform_int_distribution<std::size_t> pick{0, 3};
  std::string k(len(rng), '\0');
  for (auto &c : k)
    c = kAlphabet[pick(rng)];
  return k;
}

// Random bytes: neighbours differ at every bit position of a byte.
auto bit_key(std::mt19937_64 &rng) -> std::string {
  std::uniform_int_distribution<int> byte{0, 255};
  std::uniform_int_distribution<std::size_t> len{0, 4};
  std::string k(len(rng), '\0');
  for (auto &c : k)
    c = static_cast<char>(byte(rng));
  return k;
}

// Blind-tree nodes alive outside the test when it starts — an engine built on
// the blind tree keeps them, e.g. the state a thread's read cache still holds
// after its DB closed — so the counts below are relative to them.
auto foreign_nodes() -> std::int64_t {
  auto &acc = bytecask::btree_detail::btree_accounting<BlindRef>();
  return acc.allocated.load() - acc.freed.load() -
         static_cast<std::int64_t>(SmallTree::parked_nodes().size());
}

// Every node this test allocated is reachable from one of `trees` or parked.
void check_accounting(std::int64_t foreign,
                      std::initializer_list<const SmallTree *> trees) {
  std::set<const void *> seen;
  for (const auto *t : trees)
    t->visit_nodes([&](const void *n) { seen.insert(n); });
  for (const auto *n : SmallTree::parked_nodes())
    seen.insert(n);
  auto &acc = bytecask::btree_detail::btree_accounting<BlindRef>();
  CHECK(acc.allocated.load() - acc.freed.load() - foreign ==
        static_cast<std::int64_t>(seen.size()));
}

} // namespace

TEST_CASE("blind encoding: crit bits agree with byte order", "[blind]") {
  std::mt19937_64 rng{1};
  for (int n = 0; n < 200'000; ++n) {
    const auto a = n % 2 ? nasty_key(rng, 5) : bit_key(rng);
    const auto b = n % 3 ? nasty_key(rng, 5) : bit_key(rng);
    const auto c = bd::crit(to_bytes(a), to_bytes(b));
    if (a == b) {
      REQUIRE(c == bd::kNoCrit);
      continue;
    }
    // Only r = 0..8 encode a bit; 9..15 are gaps in the numbering.
    for (std::uint32_t p = 0; p < c; ++p)
      if ((p & 15u) <= 8)
        REQUIRE(bd::bit(to_bytes(a), p) == bd::bit(to_bytes(b), p));
    REQUIRE(bd::bit(to_bytes(a), c) != bd::bit(to_bytes(b), c));
    // The key with the 1 at the crit bit sorts after the other.
    REQUIRE((bd::bit(to_bytes(b), c) == 1) == (a < b));
  }
  // "ab" < "ab\0": they differ at the continuation bit of byte 2.
  REQUIRE(bd::crit(to_bytes("ab"), to_bytes(std::string_view{"ab\0", 3})) ==
          (2u << bd::kPosShift));
  // 'a' (0x61) vs 'c' (0x63) first differ at bit 7 of the byte: r = 7.
  REQUIRE(bd::crit(to_bytes("xa"), to_bytes("xc")) == ((1u << bd::kPosShift) | 7));
}

TEST_CASE("blind leaf: search finds every key and every insertion point",
          "[blind]") {
  // Brute force over single leaves: every query's position must match
  // std::lower_bound on the sorted key set, for present and absent keys.
  std::mt19937_64 rng{2};
  std::uniform_int_distribution<std::size_t> set_size{1, SmallLeaf::kCap};
  for (int round = 0; round < 23'000; ++round) {
    MemResolver res;
    std::set<std::string> keys;
    const auto target = set_size(rng);
    const bool bits = round % 2 == 0;
    while (keys.size() < target && keys.size() < 200)
      keys.insert(bits ? bit_key(rng) : nasty_key(rng, 6));
    bytecask::BlindBulkLoader<640> loader;
    for (const auto &k : keys)
      loader.append(to_bytes(k), res.ref(k));
    const auto t = std::move(loader).finish();
    REQUIRE(t.stats().leaves == 1);
    for (int q = 0; q < 12; ++q) {
      const auto query = q < 4 ? *std::next(keys.begin(),
                                            static_cast<std::ptrdiff_t>(
                                                rng() % keys.size()))
                               : (bits ? bit_key(rng) : nasty_key(rng, 7));
      const auto present = keys.contains(query);
      res.reads = 0;
      const auto got = t.get(to_bytes(query), res);
      REQUIRE(got.has_value() == present);
      if (present)
        REQUIRE(res.store.at(got->offset) == query);
      REQUIRE(res.reads <= 1);
      auto it = t.lower_bound(to_bytes(query), res);
      const auto want = keys.lower_bound(query);
      if (want == keys.end()) {
        REQUIRE(it == std::default_sentinel);
      } else {
        REQUIRE(it != std::default_sentinel);
        REQUIRE(res.store.at((*it).offset) == *want);
      }
    }
  }
}

// Distinct keys with the same 24-bit fingerprint, found by brute force. A
// lookup scans fingerprints, so these are the keys it must tell apart by
// reading.
auto colliding_keys(std::mt19937_64 &rng, std::size_t pairs)
    -> std::vector<std::pair<std::string, std::string>> {
  std::map<std::uint32_t, std::string> seen;
  std::vector<std::pair<std::string, std::string>> out;
  std::uniform_int_distribution<int> byte{'a', 'z'};
  while (out.size() < pairs) {
    std::string k(12, '\0');
    for (auto &c : k)
      c = static_cast<char>(byte(rng));
    const auto fp = bd::fingerprint(to_bytes(k));
    auto [it, inserted] = seen.try_emplace(fp, k);
    if (!inserted && it->second != k)
      out.emplace_back(it->second, k);
  }
  return out;
}

TEST_CASE("blind leaf: lookups tell colliding fingerprints apart",
          "[blind]") {
  std::mt19937_64 rng{7};
  const auto pairs = colliding_keys(rng, 3);
  for (const auto &[a, b] : pairs) {
    REQUIRE(a != b);
    REQUIRE(bd::fingerprint(to_bytes(a)) == bd::fingerprint(to_bytes(b)));
  }
  MemResolver res;
  std::set<std::string> keys;
  for (const auto &[a, b] : pairs) {
    keys.insert(a);
    keys.insert(b);
  }
  for (int i = 0; i < 20; ++i)
    keys.insert("filler" + std::to_string(i));
  bytecask::BlindBulkLoader<640> loader;
  for (const auto &k : keys)
    loader.append(to_bytes(k), res.ref(k));
  const auto t = std::move(loader).finish();
  REQUIRE(t.stats().leaves == 1);

  // Every key resolves to its own record: the entries with the query's
  // fingerprint are read in index order until one is the query.
  for (const auto &k : keys) {
    res.reads = 0;
    const auto got = t.get(to_bytes(k), res);
    REQUIRE(got.has_value());
    REQUIRE(res.store.at(got->offset) == k);
    REQUIRE(res.reads <= 2);
  }
  // An absent key whose fingerprint two entries share reads both and finds
  // neither.
  std::string absent;
  const auto want = bd::fingerprint(to_bytes(pairs[0].first));
  std::uniform_int_distribution<int> byte{'a', 'z'};
  do {
    absent.assign(12, '\0');
    for (auto &c : absent)
      c = static_cast<char>(byte(rng));
  } while (bd::fingerprint(to_bytes(absent)) != want || keys.contains(absent));
  res.reads = 0;
  REQUIRE_FALSE(t.get(to_bytes(absent), res).has_value());
  REQUIRE(res.reads == 2);

  // Erasing one of a pair leaves the other findable, and the erased one not.
  auto tr = t.transient();
  REQUIRE(tr.erase(to_bytes(pairs[0].first), res).has_value());
  REQUIRE_FALSE(tr.get(to_bytes(pairs[0].first), res).has_value());
  const auto other = tr.get(to_bytes(pairs[0].second), res);
  REQUIRE(other.has_value());
  REQUIRE(res.store.at(other->offset) == pairs[0].second);
}

TEST_CASE("blind tree: random operations match std::map", "[blind]") {
  std::mt19937_64 rng{3};
  const auto foreign = foreign_nodes();
  for (int round = 0; round < 40; ++round) {
    MemResolver res;
    std::map<std::string, BlindRef> model;
    SmallTree t;
    std::vector<std::pair<SmallTree, std::map<std::string, BlindRef>>> snaps;
    const std::size_t max_len = round % 4 == 0 ? 3 : 8;
    for (int batch = 0; batch < 30; ++batch) {
      auto tr = t.transient();
      for (int op = 0; op < 40; ++op) {
        auto k = nasty_key(rng, max_len);
        const auto dice = rng() % 10;
        if (dice < 6) {
          // Insert or overwrite; an overwrite moves the key to a new record.
          const auto ref = model.contains(k) ? res.fresh(k) : res.ref(k);
          const auto displaced = tr.upsert(
              to_bytes(k), ref, res,
              [](const BlindRef &, const BlindRef &) { return true; });
          REQUIRE(displaced.has_value() == model.contains(k));
          if (displaced)
            REQUIRE(*displaced == model[k]);
          model[k] = ref;
        } else if (dice < 9) {
          const auto erased = tr.erase(to_bytes(k), res);
          REQUIRE(erased.has_value() == model.contains(k));
          if (erased)
            REQUIRE(*erased == model[k]);
          model.erase(k);
        } else {
          const auto got = tr.get(to_bytes(k), res);
          REQUIRE(got.has_value() == model.contains(k));
          if (got)
            REQUIRE(*got == model[k]);
        }
        REQUIRE(tr.size() == model.size());
      }
      t = std::move(tr).persistent();
      t.validate(res);
      if (batch % 7 == 0)
        snaps.emplace_back(t, model);
    }
    // Every snapshot still reads as it did when it was taken.
    for (auto &[snap, m] : snaps) {
      snap.validate(res);
      std::vector<std::string> want;
      for (const auto &[k, v] : m)
        want.push_back(k);
      REQUIRE(keys_of(snap, res) == want);
      std::reverse(want.begin(), want.end());
      REQUIRE(rkeys_of(snap, res) == want);
      for (const auto &[k, v] : m)
        REQUIRE(snap.get(to_bytes(k), res) == v);
    }
    check_accounting(foreign, {&t});
    for (const auto &[snap, m] : snaps)
      check_accounting(foreign, {&t, &snap});
  }
}

namespace {

// "a" and a 24-bit counter, big-endian: a stream in key order.
auto counter_key(std::uint32_t v) -> std::string {
  return {'a', static_cast<char>((v >> 16) & 0xFF),
          static_cast<char>((v >> 8) & 0xFF), static_cast<char>(v & 0xFF)};
}

// Key shapes the split rules tell apart: random keys, a stream that ascends,
// one that descends, and an ascending stream with other keys landing among it.
auto shaped_key(std::mt19937_64 &rng, int shape, std::uint32_t &up,
                std::uint32_t &down) -> std::string {
  switch (shape) {
  case 0:
    return nasty_key(rng, 8);
  case 1:
    return counter_key(up++);
  case 2:
    return counter_key(down--);
  default:
    return rng() % 4 == 0 ? bit_key(rng) : counter_key(up++);
  }
}

// lower_bound agrees with the model's, and two iterators compare equal
// exactly when the model's iterators for the same seeks do.
void check_seeks(const SmallTree &t, const std::map<std::string, BlindRef> &model,
                 MemResolver &res, std::mt19937_64 &rng) {
  using It = decltype(t.begin());
  std::vector<std::pair<It, std::map<std::string, BlindRef>::const_iterator>>
      seen;
  seen.emplace_back(t.begin(), model.begin());
  seen.emplace_back(t.end_iter(), model.end());
  for (int i = 0; i < 12; ++i) {
    std::string target;
    if (!model.empty() && rng() % 2 == 0) {
      auto o = model.begin();
      std::advance(o, static_cast<std::ptrdiff_t>(rng() % model.size()));
      target = o->first;
    } else {
      auto up = static_cast<std::uint32_t>(rng() % 0x1000000);
      auto down = up;
      target = shaped_key(rng, static_cast<int>(rng() % 4), up, down);
    }
    auto it = t.lower_bound(to_bytes(target), res);
    auto o = model.lower_bound(target);
    REQUIRE((it == std::default_sentinel) == (o == model.end()));
    if (o != model.end()) {
      REQUIRE(to_string(it.key(res)) == o->first);
      REQUIRE(*it == o->second);
    }
    seen.emplace_back(std::move(it), o);
  }
  for (const auto &[a, oa] : seen)
    for (const auto &[b, ob] : seen)
      REQUIRE((a == b) == (oa == ob));
}

} // namespace

TEST_CASE("blind tree model: key streams and seeks match std::map",
          "[blind][model]") {
  std::mt19937_64 rng{11};
  const auto foreign = foreign_nodes();
  MemResolver res;
  std::map<std::string, BlindRef> model;
  SmallTree t;
  check_seeks(t, model, res, rng);
  std::uint32_t up = 0;
  std::uint32_t down = 0xFFFFFF;
  for (int batch = 0; batch < 80; ++batch) {
    const auto shape = batch % 4;
    auto tr = t.transient();
    for (int op = 0; op < 80; ++op) {
      const auto k = shaped_key(rng, shape, up, down);
      if (rng() % 8 == 0) {
        const auto erased = tr.erase(to_bytes(k), res);
        REQUIRE(erased.has_value() == model.contains(k));
        model.erase(k);
      } else {
        const auto ref = model.contains(k) ? res.fresh(k) : res.ref(k);
        tr.set(to_bytes(k), ref, res);
        model[k] = ref;
      }
    }
    // Halfway, drain the tree: seeks on a tree emptied by erase.
    if (batch == 39) {
      for (const auto &[k, v] : model)
        REQUIRE(tr.erase(to_bytes(k), res).has_value());
      model.clear();
    }
    t = std::move(tr).persistent();
    REQUIRE(t.size() == model.size());
    t.validate(res);
    check_seeks(t, model, res, rng);
  }
  check_accounting(foreign, {&t});
}

// validate() is the oracle every model test above leans on for structure, so
// it must reject a broken tree: each single-point corruption of a valid tree
// makes it throw, and undoing it makes it pass again.
TEST_CASE("blind tree model: validate rejects every single-point corruption",
          "[blind][model]") {
  using Node = bytecask::btree_detail::Node<BlindRef>;
  std::mt19937_64 rng{13};
  for (int round = 0; round < 6; ++round) {
    MemResolver res;
    auto tr = SmallTree{}.transient();
    std::uint32_t up = 0;
    std::uint32_t down = 0xFFFFFF;
    for (int i = 0; i < 3000; ++i) {
      const auto k = shaped_key(rng, (i / 200) % 4, up, down);
      tr.set(to_bytes(k), res.ref(k), res);
    }
    const auto t = std::move(tr).persistent();
    std::vector<Node *> leaves;
    std::vector<Node *> inners;
    // An inner node may hold its first child alone — a split can push its
    // last separator up — and has no separator to corrupt.
    t.visit_nodes([&](const void *p) {
      auto *n = static_cast<Node *>(const_cast<void *>(p));
      if (n->is_leaf)
        leaves.push_back(n);
      else if (n->count > 0)
        inners.push_back(n);
    });
    REQUIRE(!inners.empty());
    auto pick = [&](const std::vector<Node *> &v) {
      return v[static_cast<std::size_t>(rng() % v.size())];
    };
    auto rejected = [&](auto corrupt, auto restore) {
      corrupt();
      CHECK_THROWS_AS(t.validate(res), std::logic_error);
      restore();
      t.validate(res);
    };

    for (int trial = 0; trial < 8; ++trial) {
      // A leaf count of zero, or past the leaf's capacity.
      auto *leaf = pick(leaves);
      const auto leaf_count = leaf->count;
      rejected([&] { leaf->count = 0; }, [&] { leaf->count = leaf_count; });
      rejected(
          [&] {
            leaf->count = static_cast<std::uint32_t>(SmallTree::kLeafEntries) + 1;
          },
          [&] { leaf->count = leaf_count; });

      // Two separators out of order.
      auto *inner = pick(inners);
      if (inner->count >= 2) {
        const auto j = static_cast<std::uint32_t>(rng() % (inner->count - 1));
        rejected([&] { std::swap(inner->slots()[j], inner->slots()[j + 1]); },
                 [&] { std::swap(inner->slots()[j], inner->slots()[j + 1]); });
      }

      // A child whose keys fall below its lower bound (its left sibling in
      // its place) or at or above its upper bound (its right sibling).
      const auto c = static_cast<std::uint32_t>(rng() % inner->count);
      auto *left = inner->child(c);
      auto *right = inner->child(c + 1);
      rejected([&] { inner->set_child(c + 1, left); },
               [&] { inner->set_child(c + 1, right); });
      rejected([&] { inner->set_child(c, right); },
               [&] { inner->set_child(c, left); });
    }
  }
}

// holds / replace_at / erase_at go by a record of the key that a caller
// looked up earlier. They read nothing, and act only when the key's entry
// still points at that very record: its current one, never one the key had
// before.
TEST_CASE("blind tree: operations by location read nothing and act only on "
          "the record named", "[blind]") {
  std::mt19937_64 rng{11};
  const auto foreign = foreign_nodes();
  MemResolver res;
  std::map<std::string, BlindRef> model;
  std::map<std::string, BlindRef> previous;  // a key's record before its last write
  SmallTree t;
  const auto always = [](const BlindRef &, const BlindRef &) { return true; };
  std::size_t acted = 0;
  std::size_t declined = 0;
  for (int batch = 0; batch < 40; ++batch) {
    auto tr = t.transient();
    for (int op = 0; op < 60; ++op) {
      const auto k = nasty_key(rng, 6);
      const auto dice = rng() % 10;
      if (dice < 4) {
        const auto ref = model.contains(k) ? res.fresh(k) : res.ref(k);
        if (model.contains(k))
          previous[k] = model[k];
        (void)tr.upsert(to_bytes(k), ref, res, always);
        model[k] = ref;
        continue;
      }
      // The record to go by: the key's current one or its previous one.
      std::optional<BlindRef> at;
      if (rng() % 2 == 0 && model.contains(k)) {
        at = model[k];
      } else if (previous.contains(k)) {
        at = previous[k];
      }
      if (!at)
        continue;
      const bool current = model.contains(k) && model[k] == *at;
      res.reads = 0;
      REQUIRE(tr.holds(to_bytes(k), *at) == current);
      if (dice < 7) {
        const auto to = res.fresh(k);
        REQUIRE(tr.replace_at(to_bytes(k), *at, to) == current);
        if (current) {
          previous[k] = model[k];
          model[k] = to;
        }
      } else {
        REQUIRE(tr.erase_at(to_bytes(k), *at) == current);
        if (current) {
          previous[k] = model[k];
          model.erase(k);
        }
      }
      REQUIRE(res.reads == 0);
      REQUIRE(tr.size() == model.size());
      ++(current ? acted : declined);
    }
    t = std::move(tr).persistent();
    t.validate(res);
    for (const auto &[k, v] : model) {
      REQUIRE(t.get(to_bytes(k), res) == v);
      REQUIRE(t.holds(to_bytes(k), v));
    }
    std::vector<std::string> want;
    for (const auto &[k, v] : model)
      want.push_back(k);
    REQUIRE(keys_of(t, res) == want);
  }
  CHECK(acted > 100);
  CHECK(declined > 100);
  check_accounting(foreign, {&t});
}

// Keys whose fingerprints collide share the scan that finds candidates; the
// record location is what tells their entries apart.
TEST_CASE("blind tree: operations by location pick the key's entry among "
          "colliding fingerprints", "[blind]") {
  std::mt19937_64 rng{7};
  const auto pairs = colliding_keys(rng, 3);
  MemResolver res;
  SmallTree t;
  {
    auto tr = t.transient();
    for (const auto &[a, b] : pairs) {
      tr.set(to_bytes(a), res.ref(a), res);
      tr.set(to_bytes(b), res.ref(b), res);
    }
    t = std::move(tr).persistent();
  }
  for (const auto &[a, b] : pairs) {
    const auto ra = res.ref(a);
    const auto rb = res.ref(b);
    REQUIRE(t.holds(to_bytes(a), ra));
    REQUIRE(t.holds(to_bytes(b), rb));
    auto tr = t.transient();
    // a moves to a new record: its old one names nothing any more, and b is
    // untouched.
    const auto ra2 = res.fresh(a);
    CHECK(tr.replace_at(to_bytes(a), ra, ra2));
    CHECK_FALSE(tr.holds(to_bytes(a), ra));
    CHECK_FALSE(tr.replace_at(to_bytes(a), ra, res.fresh(a)));
    CHECK_FALSE(tr.erase_at(to_bytes(a), ra));
    CHECK(tr.get(to_bytes(a), res) == ra2);
    CHECK(tr.get(to_bytes(b), res) == rb);
    // Erasing b by its record leaves a.
    CHECK(tr.erase_at(to_bytes(b), rb));
    CHECK(tr.get(to_bytes(a), res) == ra2);
    CHECK_FALSE(tr.get(to_bytes(b), res).has_value());
    auto u = std::move(tr).persistent();
    u.validate(res);
  }
}

TEST_CASE("blind tree: structured keys build a deep tree", "[blind]") {
  MemResolver res;
  std::vector<std::string> keys;
  for (int i = 0; i < 20'000; ++i)
    keys.push_back("user::" + std::to_string(i * 7919 % 20'000) + "::x");
  SmallTree t;
  {
    auto tr = t.transient();
    for (const auto &k : keys)
      tr.set(to_bytes(k), res.ref(k), res);
    t = std::move(tr).persistent();
  }
  REQUIRE(t.size() == keys.size());
  REQUIRE(t.validate(res) >= 3);
  std::sort(keys.begin(), keys.end());
  REQUIRE(keys_of(t, res) == keys);
  // Erase every other key, then check the rest are found and iterate in order.
  auto tr = t.transient();
  std::vector<std::string> kept;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i % 2 == 0)
      REQUIRE(tr.erase(to_bytes(keys[i]), res).has_value());
    else
      kept.push_back(keys[i]);
  }
  const auto t2 = std::move(tr).persistent();
  t2.validate(res);
  REQUIRE(keys_of(t2, res) == kept);
  REQUIRE(keys_of(t, res) == keys); // the base is untouched
}

TEST_CASE("blind tree: bulk load matches inserting", "[blind]") {
  MemResolver res;
  std::set<std::string> keys;
  std::mt19937_64 rng{4};
  while (keys.size() < 5'000)
    keys.insert(nasty_key(rng, 10));
  bytecask::BlindBulkLoader<640> loader;
  for (const auto &k : keys)
    loader.append(to_bytes(k), res.ref(k));
  const auto t = std::move(loader).finish();
  t.validate(res);
  REQUIRE(keys_of(t, res) == std::vector<std::string>(keys.begin(), keys.end()));
  // Writes on a bulk-loaded tree keep the invariants.
  auto tr = t.transient();
  for (int i = 0; i < 2'000; ++i) {
    const auto k = nasty_key(rng, 10);
    if (i % 3 == 0)
      (void)tr.erase(to_bytes(k), res);
    else
      tr.set(to_bytes(k), res.ref(k), res);
  }
  std::move(tr).persistent().validate(res);

  bytecask::BlindBulkLoader<640> bad;
  bad.append(to_bytes("b"), res.ref("b"));
  REQUIRE_THROWS_AS(bad.append(to_bytes("a"), res.ref("a")),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(bad.append(to_bytes("b"), res.ref("b")),
                    std::invalid_argument);
}

TEST_CASE("blind bulk load: an abandoned loader frees what it sealed",
          "[blind]") {
  MemResolver res;
  const auto foreign = foreign_nodes();
  std::vector<std::string> keys;
  for (int i = 0; i < 2'000; ++i)
    keys.push_back("k" + std::to_string(100'000 + i));
  {
    // Out of order after many leaves have been sealed, as a mis-sorted hint
    // stream would be during recovery.
    bytecask::BlindBulkLoader<640> loader;
    for (const auto &k : keys)
      loader.append(to_bytes(k), res.ref(k));
    REQUIRE_THROWS_AS(loader.append(to_bytes("a"), res.ref("a")),
                      std::invalid_argument);
  }
  check_accounting(foreign, {});
  {
    bytecask::BlindBulkLoader<640> loader{0.6, 1.0};
    for (const auto &k : keys)
      loader.append(to_bytes(k), res.ref(k));
  }
  check_accounting(foreign, {});
}

// A blind leaf split reads the right half's first key from the data file
// for its separator, and that read can fail (EIO). The failed write must
// leave every node it allocated freed (#652): the design's *prepare, commit,
// then release* order puts every call that can throw before the allocations.
TEST_CASE("blind tree: a read that fails during a write leaks no node",
          "[blind]") {
  struct FailingResolver {
    MemResolver &mem;
    std::size_t fail_at;
    std::size_t reads{0};
    auto key_at(BlindRef r) -> std::span<const std::byte> {
      if (reads++ == fail_at)
        throw std::runtime_error{"injected read error"};
      return mem.key_at(r);
    }
  };
  auto &splits = bytecask::btree_detail::transition_counts[static_cast<
      std::size_t>(bytecask::btree_detail::Transition::kLeafSplit)];
  std::mt19937_64 rng{652};
  std::uniform_int_distribution<int> byte{'a', 'z'};
  auto random_key = [&] {
    std::string k(8, '\0');
    for (auto &c : k)
      c = static_cast<char>(byte(rng));
    return k;
  };
  MemResolver res;
  const auto foreign = foreign_nodes();
  SmallTree t;
  std::size_t failed_splits = 0;
  for (int i = 0; i < 600; ++i) {
    const auto k = random_key();
    const auto ref = res.ref(k);
    // Fail each read of the write in turn, until one runs to the end.
    for (std::size_t fail_at = 0;; ++fail_at) {
      FailingResolver failing{res, fail_at};
      auto tr = t.transient();
      const auto splits_before = splits.load();
      try {
        tr.set(to_bytes(k), ref, failing);
      } catch (const std::runtime_error &) {
        if (splits.load() != splits_before)
          ++failed_splits;
        continue;
      }
      t = std::move(tr).persistent();
      break;
    }
    check_accounting(foreign, {&t});
  }
  // The sweep reached the separator read: some failures ended a split.
  REQUIRE(failed_splits > 0);
  t.validate(res);
  check_accounting(foreign, {&t});
}

TEST_CASE("blind tree: a refused replacement changes nothing", "[blind]") {
  MemResolver res;
  SmallTree t;
  auto tr = t.transient();
  const auto first = res.ref("k");
  tr.set(to_bytes("k"), first, res);
  const auto refused = tr.upsert(
      to_bytes("k"), res.fresh("k"), res,
      [](const BlindRef &, const BlindRef &) { return false; });
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(tr.size() == 1);
  REQUIRE(tr.get(to_bytes("k"), res) == first);
  t = std::move(tr).persistent();
  t.validate(res);
  REQUIRE(t.get(to_bytes("k"), res) == first);
}

TEST_CASE("blind tree: stepping back from the end of an empty tree",
          "[blind]") {
  MemResolver res;
  SmallTree empty;
  auto it = empty.end_iter();
  --it;
  REQUIRE(it == std::default_sentinel);
  // Emptied by erasing every key, rather than never filled.
  auto tr = empty.transient();
  for (int i = 0; i < 500; ++i)
    tr.set(to_bytes("k" + std::to_string(1'000 + i)),
           res.ref("k" + std::to_string(1'000 + i)), res);
  for (int i = 0; i < 500; ++i)
    REQUIRE(tr.erase(to_bytes("k" + std::to_string(1'000 + i)), res));
  const auto emptied = std::move(tr).persistent();
  emptied.validate(res);
  auto it2 = emptied.end_iter();
  --it2;
  REQUIRE(it2 == std::default_sentinel);
  REQUIRE(emptied.begin() == std::default_sentinel);
}

TEST_CASE("blind tree: default-size leaves split on varied keys", "[blind]") {
  // The engine's leaf size, with keys of mixed length and shared prefixes,
  // inserted at random: splits at every crit-bit depth.
  std::mt19937_64 rng{11};
  MemResolver res;
  std::map<std::string, BlindRef> model;
  Tree t;
  for (int batch = 0; batch < 20; ++batch) {
    auto tr = t.transient();
    for (int op = 0; op < 1'000; ++op) {
      auto k = nasty_key(rng, 24);
      if (rng() % 5 == 0) {
        REQUIRE(tr.erase(to_bytes(k), res).has_value() == model.contains(k));
        model.erase(k);
      } else {
        const auto ref = model.contains(k) ? res.fresh(k) : res.ref(k);
        tr.set(to_bytes(k), ref, res);
        model[k] = ref;
      }
    }
    t = std::move(tr).persistent();
    t.validate(res);
  }
  REQUIRE(t.size() == model.size());
  REQUIRE(t.stats().height >= 2);
  for (const auto &[k, v] : model)
    REQUIRE(t.get(to_bytes(k), res) == v);
}

TEST_CASE("blind tree: reads per operation", "[blind]") {
  MemResolver res;
  std::vector<std::string> keys;
  for (int i = 0; i < 10'000; ++i)
    keys.push_back("k" + std::to_string(i * 104'729 % 1'000'003));
  Tree t;
  std::size_t splits_bound = 0;
  {
    auto tr = t.transient();
    res.reads = 0;
    for (const auto &k : keys)
      tr.set(to_bytes(k), res.ref(k), res);
    t = std::move(tr).persistent();
    // One read per insert (none for the first), plus at most one per split.
    splits_bound = t.stats().leaves;
    CHECK(res.reads >= keys.size() - 1);
    CHECK(res.reads <= keys.size() - 1 + splits_bound);
  }
  res.reads = 0;
  for (const auto &k : keys)
    REQUIRE(t.get(to_bytes(k), res).has_value());
  CHECK(res.reads == keys.size()); // a present key: one read

  res.reads = 0;
  for (int i = 0; i < 10'000; ++i)
    REQUIRE_FALSE(t.get(to_bytes("absent" + std::to_string(i)), res));
  CHECK(res.reads <= 1); // an absent key: none, bar a fingerprint collision

  auto tr = t.transient();
  res.reads = 0;
  for (std::size_t i = 0; i < 100; ++i)
    tr.set(to_bytes(keys[i]), res.ref(keys[i]), res); // overwrite
  CHECK(res.reads == 100);
  res.reads = 0;
  for (std::size_t i = 0; i < 100; ++i)
    REQUIRE(tr.erase(to_bytes(keys[i]), res).has_value());
  CHECK(res.reads == 100);
  res.reads = 0;
  for (std::size_t i = 0; i < 100; ++i)
    REQUIRE_FALSE(tr.erase(to_bytes(keys[i]), res).has_value());
  CHECK(res.reads <= 1);
}

TEST_CASE("blind tree: 65535-byte keys", "[blind]") {
  MemResolver res;
  std::vector<std::string> keys;
  const std::string base(65'534, 'x');
  for (int i = 0; i < 100; ++i) {
    auto k = base;
    k.push_back(static_cast<char>(i));
    keys.push_back(k);                 // 65,535 bytes, differing in the last
    keys.push_back(base.substr(0, static_cast<std::size_t>(i) * 600)); // prefixes
  }
  SmallTree t;
  auto tr = t.transient();
  for (const auto &k : keys)
    tr.set(to_bytes(k), res.ref(k), res);
  t = std::move(tr).persistent();
  t.validate(res);
  std::set<std::string> want(keys.begin(), keys.end());
  REQUIRE(keys_of(t, res) == std::vector<std::string>(want.begin(), want.end()));
  for (const auto &k : want)
    REQUIRE(t.get(to_bytes(k), res).has_value());
  const std::string too_long(65'536, 'y');
  auto tr2 = t.transient();
  REQUIRE_THROWS_AS(tr2.set(to_bytes(too_long), res.ref(too_long), res),
                    std::length_error);
}

TEST_CASE("blind tree: sealed slices concatenate into one tree", "[blind]") {
  MemResolver res;
  std::set<std::string> key_set;
  std::mt19937_64 rng{5};
  while (key_set.size() < 3'000)
    key_set.insert(nasty_key(rng, 9));
  const std::vector<std::string> keys(key_set.begin(), key_set.end());
  // Three slices, one of them empty, as ranges without keys produce.
  const std::size_t cuts[] = {0, 1'000, 1'000, 2'500, keys.size()};
  std::vector<bytecask::btree_detail::LeafRun<BlindRef>> runs;
  for (std::size_t r = 0; r + 1 < std::size(cuts); ++r) {
    bytecask::BlindBulkLoader<640> loader;
    for (auto i = cuts[r]; i < cuts[r + 1]; ++i)
      loader.append(to_bytes(keys[i]), res.ref(keys[i]));
    runs.push_back(std::move(loader).seal());
  }
  const auto t = bytecask::BlindBulkLoader<640>::concat(std::move(runs));
  REQUIRE(t.size() == keys.size());
  t.validate(res);
  REQUIRE(keys_of(t, res) == keys);
  for (const auto &k : keys)
    REQUIRE(t.get(to_bytes(k), res).has_value());
}

TEST_CASE("blind tree: bulk load spreads leaf fill over a range", "[blind]") {
  MemResolver res;
  std::set<std::string> key_set;
  std::mt19937_64 rng{6};
  while (key_set.size() < 4'000)
    key_set.insert(nasty_key(rng, 9));
  bytecask::BlindBulkLoader<640> loader{0.5, 1.0};
  for (const auto &k : key_set)
    loader.append(to_bytes(k), res.ref(k));
  const auto t = std::move(loader).finish();
  t.validate(res);
  const auto cap = SmallTree::kLeafEntries;
  const auto st = t.stats();
  // Every leaf but the last one loaded (wherever stats() lists it) is
  // between half full and full.
  std::set<std::uint32_t> sizes;
  std::size_t short_leaves = 0;
  for (const auto c : st.leaf_counts) {
    CHECK(c <= cap);
    if (c < cap / 2)
      ++short_leaves;
    sizes.insert(c);
  }
  CHECK(short_leaves <= 1);
  CHECK(sizes.size() > 5); // spread, not one size
}

// ---------------------------------------------------------------------------
// Paths prepared in one version, replayed on a later one (PreparedPath,
// TransientBlindBTree::route; docs/blind_leaf_btree_design.md, *Paths
// prepared before the write lock*). A path write must decide exactly what
// the keyed write decides; only how it reaches the leaf differs.
// ---------------------------------------------------------------------------

namespace {
struct PathOp {
  enum Kind { kUpsert, kReplaceAt, kEraseAt } kind;
  std::string key;
  BlindRef at{};  // kReplaceAt, kEraseAt: the record the key had at P
  bytecask::PreparedPath path;
};
}  // namespace

// Plans of updates and erases by location, and of inserts placed at P, are
// prepared in a version P. Other writes then change the tree (inserts that
// split leaves, erases that empty them, overwrites), and the plans are
// applied, each with its own PathFollow, interleaved with more keyed writes.
// Every outcome and the final tree must match std::map, the tree must stay
// valid, and paths must both replay and miss.
TEST_CASE("blind tree model: writes down paths prepared in an earlier "
          "version decide what writes by key decide", "[blind][model]") {
  const auto seed = GENERATE(1u, 2u, 3u, 4u);
  CAPTURE(seed);
  std::mt19937_64 rng{seed};
  const auto foreign = foreign_nodes();
  MemResolver res;
  std::map<std::string, BlindRef> model;
  SmallTree t;
  const auto always = [](const BlindRef &, const BlindRef &) { return true; };
  std::uint32_t up = 0;
  std::uint32_t down = 0xFFFFFF;
  const auto key = [&] {
    return shaped_key(rng, static_cast<int>(rng() % 4), up, down);
  };
  const auto keyed_write = [&](SmallTransient &tr) {
    const auto k = !model.empty() && rng() % 3 == 0
                       ? std::next(model.begin(), static_cast<std::ptrdiff_t>(
                                                      rng() % model.size()))
                             ->first
                       : key();
    if (rng() % 3 == 0) {
      const auto erased = tr.erase(to_bytes(k), res);
      REQUIRE(erased.has_value() == model.contains(k));
      model.erase(k);
    } else {
      const auto ref = model.contains(k) ? res.fresh(k) : res.ref(k);
      (void)tr.upsert(to_bytes(k), ref, res, always);
      model[k] = ref;
    }
  };
  {
    auto tr = t.transient();
    for (int i = 0; i < 400; ++i) keyed_write(tr);
    t = std::move(tr).persistent();
  }
  bytecask::PathStats stats;
  for (int batch = 0; batch < 60; ++batch) {
    // P: the version the plans prepare their paths in, pinned until they
    // are applied.
    const auto p = t;
    std::vector<std::vector<PathOp>> plans(1 + rng() % 6);
    for (auto &plan : plans) {
      const auto n = 1 + rng() % 8;
      for (std::size_t i = 0; i < n; ++i) {
        // Mostly keys near each other, so a plan's writes share leaves.
        const auto k = !model.empty() && rng() % 2 == 0
                           ? std::next(model.begin(),
                                       static_cast<std::ptrdiff_t>(
                                           rng() % model.size()))
                                 ->first
                           : key();
        PathOp op{PathOp::kUpsert, k, {}, {}};
        if (model.contains(k) && rng() % 3 != 0) {
          op.kind = rng() % 4 == 0 ? PathOp::kEraseAt : PathOp::kReplaceAt;
          op.at = model[k];
        }
        p.prepare_path(to_bytes(k), res, op.kind == PathOp::kUpsert, op.path);
        plan.push_back(std::move(op));
      }
    }
    // Other commits between P and the plans' section.
    if (rng() % 4 != 0) {
      auto tr = t.transient();
      const auto n = rng() % 60;
      for (std::size_t i = 0; i < n; ++i) keyed_write(tr);
      t = std::move(tr).persistent();
    }
    auto tr = t.transient();
    for (auto &plan : plans) {
      bytecask::PathFollow follow;
      for (auto &op : plan) {
        const auto &k = op.key;
        const bool current = model.contains(k) && model[k] == op.at;
        switch (op.kind) {
        case PathOp::kUpsert: {
          const auto ref = model.contains(k) ? res.fresh(k) : res.ref(k);
          const auto displaced =
              tr.upsert(to_bytes(k), ref, res, always, op.path, follow);
          REQUIRE(displaced.has_value() == model.contains(k));
          if (displaced)
            REQUIRE(*displaced == model[k]);
          model[k] = ref;
          break;
        }
        case PathOp::kReplaceAt: {
          const auto to = res.fresh(k);
          REQUIRE(tr.replace_at(to_bytes(k), op.at, to, op.path, follow) ==
                  current);
          if (current)
            model[k] = to;
          break;
        }
        case PathOp::kEraseAt:
          REQUIRE(tr.erase_at(to_bytes(k), op.at, op.path, follow) == current);
          if (current)
            model.erase(k);
          break;
        }
        REQUIRE(tr.size() == model.size());
      }
      // Another slot of the group, applied by key.
      if (rng() % 3 == 0) keyed_write(tr);
    }
    stats.replayed += tr.path_stats().replayed;
    stats.missed += tr.path_stats().missed;
    t = std::move(tr).persistent();
    t.validate(res);
    for (const auto &[k, v] : model)
      REQUIRE(t.get(to_bytes(k), res) == v);
    std::vector<std::string> want;
    for (const auto &[k, v] : model)
      want.push_back(k);
    REQUIRE(keys_of(t, res) == want);
  }
  CAPTURE(stats.replayed, stats.missed);
  CHECK(stats.replayed > 150);
  CHECK(stats.missed > 150);
  check_accounting(foreign, {&t});
}

// The three routes, one at a time. A path reaches its own leaf while that
// leaf is unchanged; after the plan's own write it reaches the copy that
// write made, but only until any other write runs; after a split it
// reaches neither and the write goes by key.
TEST_CASE("blind tree: a path is replayed to its leaf, or to the plan's copy "
          "of it until another write, and missed after a split", "[blind]") {
  MemResolver res;
  SmallTree t;
  const auto always = [](const BlindRef &, const BlindRef &) { return true; };
  {
    // Even keys, shuffled: leaves split in halves and keep room.
    std::vector<std::uint32_t> even;
    for (std::uint32_t i = 0; i < 200; i += 2) even.push_back(i);
    std::ranges::shuffle(even, std::mt19937_64{5});
    auto tr = t.transient();
    for (const auto i : even)
      tr.set(to_bytes(counter_key(i)), res.ref(counter_key(i)), res);
    t = std::move(tr).persistent();
  }
  const auto prepare = [&](const SmallTree &v, std::uint32_t i) {
    bytecask::PreparedPath p;
    v.prepare_path(to_bytes(counter_key(i)), res, true, p);
    REQUIRE(p.leaf != 0);
    REQUIRE(p.pos.has_value());
    REQUIRE_FALSE(p.pos->exact);  // odd keys are absent
    return p;
  };
  const auto insert = [&](SmallTransient &tr, std::uint32_t i,
                          const bytecask::PreparedPath &p,
                          bytecask::PathFollow &f) {
    const auto before = tr.path_stats();
    res.reads = 0;
    REQUIRE_FALSE(tr.upsert(to_bytes(counter_key(i)), res.ref(counter_key(i)),
                            res, always, p, f)
                      .has_value());
    return std::pair{tr.path_stats().replayed - before.replayed,
                     res.reads};
  };
  const auto p = t;
  // Three keys in one leaf, and one far away.
  const auto p41 = prepare(p, 41);
  const auto p43 = prepare(p, 43);
  const auto p45 = prepare(p, 45);
  const auto p151 = prepare(p, 151);
  REQUIRE(p41.leaf == p43.leaf);
  REQUIRE(p43.leaf == p45.leaf);
  REQUIRE(p151.leaf != p41.leaf);

  auto tr = t.transient();
  bytecask::PathFollow f;
  // Unchanged leaf: replayed, placed where P said, no read.
  CHECK(insert(tr, 41, p41, f) == std::pair{std::uint64_t{1}, std::size_t{0}});
  // The plan's own copy of that leaf, just written: followed, placed by a read.
  CHECK(insert(tr, 43, p43, f).first == 1);
  // Any other write in between ends the follow: by key.
  bytecask::PathFollow other;
  CHECK(insert(tr, 151, p151, other).first == 1);
  CHECK(insert(tr, 45, p45, f).first == 0);
  t = std::move(tr).persistent();
  t.validate(res);

  // A key present at P, overwritten in its unchanged leaf: replaced where the
  // path found it, with no read, displacing the record it found.
  {
    const auto p42_version = t;
    bytecask::PreparedPath p42;
    p42_version.prepare_path(to_bytes(counter_key(42)), res, true, p42);
    REQUIRE(p42.pos.has_value());
    REQUIRE(p42.pos->exact);
    auto ow = t.transient();
    bytecask::PathFollow fo;
    res.reads = 0;
    const auto displaced =
        ow.upsert(to_bytes(counter_key(42)), res.fresh(counter_key(42)), res,
                  always, p42, fo);
    CHECK(res.reads == 0);
    REQUIRE(displaced.has_value());
    CHECK(*displaced == p42.found);
    CHECK(p42.displaced_found(*displaced));
    t = std::move(ow).persistent();
    t.validate(res);
  }

  // A split replaces the leaf: a path prepared before it misses.
  const auto before_split = t;
  const auto p47 = prepare(before_split, 47);
  {
    auto split = t.transient();
    // Keys between 40 and 41: more than a leaf holds, in that leaf.
    for (std::uint32_t i = 0; i < 120; ++i) {
      const auto k = counter_key(40) + std::string(1, static_cast<char>(i));
      split.set(to_bytes(k), res.ref(k), res);
    }
    t = std::move(split).persistent();
  }
  auto tr2 = t.transient();
  bytecask::PathFollow f2;
  CHECK(insert(tr2, 47, p47, f2).first == 0);
  t = std::move(tr2).persistent();
  t.validate(res);
  CHECK(t.get(to_bytes(counter_key(47)), res).has_value());
}

// A path's leaf may be freed before the path is replayed: a later commit
// copies it, and the last version holding it is released. Its memory then
// goes to another node, here the copy of the same leaf, at the same address
// with different contents. The path must not take that node for its leaf:
// the tag tells them apart, and the write goes by key.
TEST_CASE("blind tree: a path whose leaf was freed and its memory reused is "
          "not replayed", "[blind]") {
  MemResolver res;
  SmallTree t;
  {
    std::vector<std::uint32_t> even;
    for (std::uint32_t i = 0; i < 200; i += 2) even.push_back(i);
    std::ranges::shuffle(even, std::mt19937_64{7});
    auto tr = t.transient();
    for (const auto i : even)
      tr.set(to_bytes(counter_key(i)), res.ref(counter_key(i)), res);
    t = std::move(tr).persistent();
  }
  bytecask::PreparedPath path;
  t.prepare_path(to_bytes(counter_key(41)), res, true, path);
  REQUIRE(path.pos.has_value());
  REQUIRE_FALSE(path.pos->exact);
  // Each commit rewrites key 40 in place, copying its leaf (41's), and
  // releases the version before it, freeing the copy that version held.
  // The pool hands freed node memory out again; stop once the leaf's
  // current copy sits where the prepared leaf was.
  bool reused = false;
  for (int i = 0; i < 5000 && !reused; ++i) {
    auto tr = t.transient();
    const auto k = counter_key(40);
    const auto from = res.ref(k);
    const auto to = res.fresh(k);
    REQUIRE(tr.replace_at(to_bytes(k), from, to));
    t = std::move(tr).persistent();
    bytecask::PreparedPath now;
    t.prepare_path(to_bytes(counter_key(41)), res, true, now);
    REQUIRE(now.leaf_tag != path.leaf_tag);
    reused = now.leaf == path.leaf;
  }
  // Whether freed memory comes back at that address depends on the
  // allocator's order, which differs between builds (the pool's batches,
  // ASan's quarantine, coverage instrumentation). The release build reaches
  // it; a build that never does has nothing to check.
  if (!reused)
    SKIP("node memory was not reused at the prepared leaf's address");
  auto tr = t.transient();
  bytecask::PathFollow f;
  REQUIRE_FALSE(tr.upsert(to_bytes(counter_key(41)), res.ref(counter_key(41)),
                          res, [](const BlindRef &, const BlindRef &) {
                            return true;
                          },
                          path, f)
                    .has_value());
  CHECK(tr.path_stats().replayed == 0);
  CHECK(tr.path_stats().missed == 1);
  t = std::move(tr).persistent();
  t.validate(res);
  CHECK(t.get(to_bytes(counter_key(41)), res).has_value());
}
