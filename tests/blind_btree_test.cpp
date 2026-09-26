// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for the blind-leaf B+ tree
// (docs/blind_leaf_btree_design.md). The tree reads keys through a resolver;
// these tests use an in-memory one that also counts reads.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
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
