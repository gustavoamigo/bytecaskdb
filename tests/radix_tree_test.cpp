#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
import bytecask.radix_tree;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(std::span<const std::byte> bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    s[i] = static_cast<char>(bytes[i]);
  }
  return s;
}

using Tree = bytecask::PersistentRadixTree<int>;

} // namespace

// ---------------------------------------------------------------------------
// Basic: empty tree
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree empty tree", "[radix_tree]") {
  const Tree t;
  CHECK(t.empty());
  CHECK(t.size() == 0U);
  CHECK_FALSE(t.get(to_bytes("any")).has_value());
  CHECK_FALSE(t.contains(to_bytes("any")));
}

// ---------------------------------------------------------------------------
// Single key set/get round-trip
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree single key set and get", "[radix_tree]") {
  const Tree t;
  auto t2 = t.set(to_bytes("hello"), 42);

  CHECK(t2.size() == 1U);
  CHECK_FALSE(t2.empty());
  REQUIRE(t2.get(to_bytes("hello")).has_value());
  CHECK(*t2.get(to_bytes("hello")) == 42);

  // Original unchanged (immutability).
  CHECK(t.empty());
  CHECK_FALSE(t.get(to_bytes("hello")).has_value());
}

// ---------------------------------------------------------------------------
// Overwrite existing key
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree overwrite existing key", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("key"), 1);
  auto t2 = t.set(to_bytes("key"), 2);

  CHECK(t2.size() == 1U);
  CHECK(*t2.get(to_bytes("key")) == 2);
  // Old version retains original value.
  CHECK(*t.get(to_bytes("key")) == 1);
}

// ---------------------------------------------------------------------------
// Prefix compression: shared prefix
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree prefix compression", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("prefix_A"), 1).set(to_bytes("prefix_B"), 2);

  CHECK(t.size() == 2U);
  CHECK(*t.get(to_bytes("prefix_A")) == 1);
  CHECK(*t.get(to_bytes("prefix_B")) == 2);
  CHECK_FALSE(t.contains(to_bytes("prefix_")));
}

// ---------------------------------------------------------------------------
// Keys that are prefixes of each other
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree key is prefix of another key", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("abc"), 1).set(to_bytes("abcdef"), 2);

  CHECK(t.size() == 2U);
  CHECK(*t.get(to_bytes("abc")) == 1);
  CHECK(*t.get(to_bytes("abcdef")) == 2);
  CHECK_FALSE(t.contains(to_bytes("ab")));
  CHECK_FALSE(t.contains(to_bytes("abcd")));
}

// ---------------------------------------------------------------------------
// Erase existing key
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree erase existing key", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("a"), 1).set(to_bytes("b"), 2);
  auto t2 = t.erase(to_bytes("a"));

  CHECK(t2.size() == 1U);
  CHECK_FALSE(t2.contains(to_bytes("a")));
  CHECK(t2.contains(to_bytes("b")));
  // Original unchanged.
  CHECK(t.size() == 2U);
  CHECK(t.contains(to_bytes("a")));
}

// ---------------------------------------------------------------------------
// Erase absent key — no-op
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree erase absent key", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("a"), 1);
  auto t2 = t.erase(to_bytes("nope"));

  CHECK(t2.size() == 1U);
  CHECK(t2.contains(to_bytes("a")));
}

// ---------------------------------------------------------------------------
// Erase triggers path compression
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree erase path compression", "[radix_tree]") {
  // "abc" and "abd" share prefix "ab" with transition 'c' and 'd'.
  // Erasing "abc" should merge the routing node with "abd".
  auto t = Tree{}.set(to_bytes("abc"), 1).set(to_bytes("abd"), 2);
  auto t2 = t.erase(to_bytes("abc"));

  CHECK(t2.size() == 1U);
  CHECK(*t2.get(to_bytes("abd")) == 2);
  CHECK_FALSE(t2.contains(to_bytes("abc")));
}

// ---------------------------------------------------------------------------
// Erase all keys results in empty tree
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree erase all keys", "[radix_tree]") {
  auto t =
      Tree{}.set(to_bytes("x"), 1).set(to_bytes("y"), 2).set(to_bytes("z"), 3);
  auto t2 = t.erase(to_bytes("x")).erase(to_bytes("y")).erase(to_bytes("z"));

  CHECK(t2.empty());
  CHECK(t2.size() == 0U);
}

// ---------------------------------------------------------------------------
// Iteration: ascending key order
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree iteration ascending order", "[radix_tree]") {
  auto t = Tree{}
               .set(to_bytes("cherry"), 3)
               .set(to_bytes("apple"), 1)
               .set(to_bytes("banana"), 2);

  std::vector<std::string> keys;
  std::vector<int> values;
  for (auto it = t.begin(); it != t.end(); ++it) {
    auto [k, v] = *it;
    keys.push_back(to_string(k));
    values.push_back(v);
  }

  REQUIRE(keys.size() == 3U);
  CHECK(keys[0] == "apple");
  CHECK(keys[1] == "banana");
  CHECK(keys[2] == "cherry");
  CHECK(values[0] == 1);
  CHECK(values[1] == 2);
  CHECK(values[2] == 3);

  // AC 4: Ordered Iteration
  CHECK(std::is_sorted(keys.begin(), keys.end()));
}

// ---------------------------------------------------------------------------
// Iteration: empty tree
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree iteration empty tree", "[radix_tree]") {
  const Tree t;
  CHECK(t.begin() == t.end());
}

// ---------------------------------------------------------------------------
// lower_bound: starts at correct position
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree lower_bound", "[radix_tree]") {
  auto t = Tree{}
               .set(to_bytes("apple"), 1)
               .set(to_bytes("banana"), 2)
               .set(to_bytes("cherry"), 3);

  std::vector<std::string> keys;
  for (auto it = t.lower_bound(to_bytes("banana")); it != t.end(); ++it) {
    auto [k, v] = *it;
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 2U);
  CHECK(keys[0] == "banana");
  CHECK(keys[1] == "cherry");
}

// ---------------------------------------------------------------------------
// lower_bound: target between existing keys
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree lower_bound between keys", "[radix_tree]") {
  auto t = Tree{}
               .set(to_bytes("aaa"), 1)
               .set(to_bytes("ccc"), 2)
               .set(to_bytes("eee"), 3);

  std::vector<std::string> keys;
  for (auto it = t.lower_bound(to_bytes("bbb")); it != t.end(); ++it) {
    auto [k, v] = *it;
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 2U);
  CHECK(keys[0] == "ccc");
  CHECK(keys[1] == "eee");
}

// ---------------------------------------------------------------------------
// lower_bound: target after all keys
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree lower_bound past end", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("aaa"), 1);
  auto it = t.lower_bound(to_bytes("zzz"));
  CHECK(it == t.end());
}

// ---------------------------------------------------------------------------
// Transient: basic set/get/erase
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree transient basic operations", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes("a"), 1);
  auto tr = t.transient();

  tr.set(to_bytes("b"), 2);
  tr.set(to_bytes("c"), 3);
  CHECK(tr.erase(to_bytes("a")));
  CHECK_FALSE(tr.erase(to_bytes("nonexistent")));
  CHECK(tr.contains(to_bytes("b")));
  CHECK_FALSE(tr.contains(to_bytes("a")));

  auto t2 = std::move(tr).persistent();
  CHECK(t2.size() == 2U);
  CHECK_FALSE(t2.contains(to_bytes("a")));
  CHECK(*t2.get(to_bytes("b")) == 2);
  CHECK(*t2.get(to_bytes("c")) == 3);

  // Original unchanged.
  CHECK(t.size() == 1U);
  CHECK(t.contains(to_bytes("a")));
}

// ---------------------------------------------------------------------------
// Transient: bulk insert
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree transient bulk insert", "[radix_tree]") {
  auto tr = Tree{}.transient();
  for (int i = 0; i < 1000; ++i) {
    auto key = std::string("key_") + std::to_string(i);
    tr.set(to_bytes(key), i);
  }
  auto t = std::move(tr).persistent();
  CHECK(t.size() == 1000U);

  for (int i = 0; i < 1000; ++i) {
    auto key = std::string("key_") + std::to_string(i);
    REQUIRE(t.contains(to_bytes(key)));
    CHECK(*t.get(to_bytes(key)) == i);
  }
}

// ---------------------------------------------------------------------------
// Immutability: snapshot before mutation is stable
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree immutability snapshot", "[radix_tree]") {
  auto t1 = Tree{}.set(to_bytes("x"), 10).set(to_bytes("y"), 20);

  // Take snapshot.
  auto snapshot = t1;

  // Mutate.
  auto t2 = t1.set(to_bytes("z"), 30).erase(to_bytes("x"));

  // Snapshot unchanged.
  CHECK(snapshot.size() == 2U);
  CHECK(snapshot.contains(to_bytes("x")));
  CHECK(snapshot.contains(to_bytes("y")));
  CHECK_FALSE(snapshot.contains(to_bytes("z")));

  // New version correct.
  CHECK(t2.size() == 2U);
  CHECK_FALSE(t2.contains(to_bytes("x")));
  CHECK(t2.contains(to_bytes("y")));
  CHECK(t2.contains(to_bytes("z")));
}

// ---------------------------------------------------------------------------
// Model-based property tests (AC 7)
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree model-based property test", "[radix_tree]") {
  std::mt19937 gen(42);
  std::uniform_int_distribution<int> op_dist(0, 1);
  std::uniform_int_distribution<std::size_t> len_dist(1, 8);
  std::uniform_int_distribution<int> char_dist(0, 5);

  std::map<std::string, int> oracle;
  auto tree = Tree{}.transient();
  Tree snap;

  auto rand_string = [&]() {
    std::size_t len = len_dist(gen);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
      s.push_back(static_cast<char>('A' + char_dist(gen)));
    }
    return s;
  };

  for (int round = 1; round <= 10000; ++round) {
    if (round % 200 == 0) {
      snap = std::move(tree).persistent();
      tree = snap.transient();
    }

    std::string k = rand_string();
    if (op_dist(gen) == 0) {
      int v = round;
      oracle[k] = v;
      tree.set(to_bytes(k), v);
    } else {
      oracle.erase(k);
      tree.erase(to_bytes(k));
    }

    auto tree_p = std::move(tree).persistent();

    REQUIRE(tree_p.size() == oracle.size());
    REQUIRE(tree_p.contains(to_bytes(k)) == (oracle.count(k) > 0));
    if (oracle.count(k) > 0) {
      REQUIRE(tree_p.get(to_bytes(k)) == oracle[k]);
    }

    if (round % 50 == 0) {
      auto it = tree_p.begin();
      for (const auto &[ok, ov] : oracle) {
        REQUIRE(it != tree_p.end());
        auto [tk, tv] = *it;
        REQUIRE(to_string(tk) == ok);
        REQUIRE(tv == ov);
        ++it;
      }
      REQUIRE(it == tree_p.end());
    }

    if (round % 100 == 0) {
      std::string probe = rand_string();
      auto o_it = oracle.lower_bound(probe);
      auto t_it = tree_p.lower_bound(to_bytes(probe));
      if (o_it == oracle.end()) {
        REQUIRE(t_it == tree_p.end());
      } else {
        REQUIRE(t_it != tree_p.end());
        auto [tk, tv] = *t_it;
        REQUIRE(to_string(tk) == o_it->first);
      }
    }

    tree = std::move(tree_p).transient();
  }
}

// ---------------------------------------------------------------------------
// Concurrent reader/writer safety (AC 9)
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree concurrent reader/writer safety",
          "[concurrency][radix_tree]") {
  auto pt = Tree{}.set(to_bytes("base1"), 1).set(to_bytes("base2"), 2);

  std::atomic<bool> start{false};
  std::atomic<bool> done{false};

  auto reader_func = [&]() {
    while (!start)
      std::this_thread::yield();
    while (!done) {
      // iterate pt and ensure it only sees base1 and base2 precisely
      int sum = 0;
      std::size_t count = 0;
      for (auto it = pt.begin(); it != pt.end(); ++it) {
        count++;
        auto [k, v] = *it;
        sum += v;
      }
      REQUIRE(count == 2);
      REQUIRE(sum == 3);
    }
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back(reader_func);
  }

  start = true;
  // Writer
  auto tr = pt.transient();
  for (int i = 0; i < 5000; ++i) {
    tr.set(to_bytes("key_" + std::to_string(i)), i);
    if (i % 100 == 0) {
      auto tmp = std::move(tr).persistent();
      tr = tmp.transient();
    }
  }
  done = true;

  for (auto &t : readers) {
    t.join();
  }
}

// ---------------------------------------------------------------------------
// Empty key
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree empty key", "[radix_tree]") {
  auto t = Tree{}.set(to_bytes(""), 99);
  CHECK(t.size() == 1U);
  REQUIRE(t.get(to_bytes("")).has_value());
  CHECK(*t.get(to_bytes("")) == 99);

  auto t2 = t.set(to_bytes("a"), 1);
  CHECK(t2.size() == 2U);
  CHECK(*t2.get(to_bytes("")) == 99);
  CHECK(*t2.get(to_bytes("a")) == 1);
}

// ---------------------------------------------------------------------------
// Many keys: iteration order matches std::map
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree iteration matches std::map order", "[radix_tree]") {
  std::vector<std::string> test_keys = {
      "user:001", "user:002",     "user:010",      "admin:root",
      "admin:su", "config:debug", "config:release"};

  auto t = Tree{};
  std::map<std::string, int> oracle;
  for (int i = 0; auto &k : test_keys) {
    t = t.set(to_bytes(k), i);
    oracle[k] = i;
    ++i;
  }

  std::vector<std::string> tree_keys;
  std::vector<int> tree_vals;
  for (auto it = t.begin(); it != t.end(); ++it) {
    auto [k, v] = *it;
    tree_keys.push_back(to_string(k));
    tree_vals.push_back(v);
  }

  std::vector<std::string> oracle_keys;
  std::vector<int> oracle_vals;
  for (auto &[k, v] : oracle) {
    oracle_keys.push_back(k);
    oracle_vals.push_back(v);
  }

  CHECK(tree_keys == oracle_keys);
  CHECK(tree_vals == oracle_vals);
}

// ---------------------------------------------------------------------------
// Model-based: random operations vs std::map oracle
//
// On every round: size, get, contains for the operated key.
// Periodically: full iteration order, lower_bound, immutability snapshots.
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree model-based random operations", "[radix_tree]") {
  // Simple PRNG for reproducibility.
  std::uint32_t seed = 12345;
  auto next_rand = [&]() -> std::uint32_t {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };

  auto rand_key = [&]() -> std::string {
    // Keys of length 1–8 from a small alphabet for good prefix overlap.
    auto len = (next_rand() % 8) + 1;
    std::string k;
    for (std::uint32_t i = 0; i < len; ++i) {
      k += static_cast<char>('a' + (next_rand() % 6));
    }
    return k;
  };

  // Collect tree iteration into a vector of (key, value) for comparison.
  auto collect = [](const Tree &tree) {
    std::vector<std::pair<std::string, int>> out;
    for (auto it = tree.begin(); it != tree.end(); ++it) {
      auto [k, v] = *it;
      out.emplace_back(to_string(k), v);
    }
    return out;
  };

  auto collect_oracle = [](const std::map<std::string, int> &m) {
    std::vector<std::pair<std::string, int>> out;
    for (auto &[k, v] : m) {
      out.emplace_back(k, v);
    }
    return out;
  };

  Tree t;
  std::map<std::string, int> oracle;

  for (int round = 0; round < 10000; ++round) {
    auto op = next_rand() % 3;
    auto key = rand_key();
    auto val = static_cast<int>(next_rand() % 1000);

    // --- Periodic immutability snapshot (before mutation) ---
    Tree snapshot;
    std::vector<std::pair<std::string, int>> snapshot_entries;
    bool check_snapshot = (round % 200 == 0) && !oracle.empty();
    if (check_snapshot) {
      snapshot = t;
      snapshot_entries = collect(snapshot);
    }

    // --- Apply operation ---
    if (op < 2) {
      t = t.set(to_bytes(key), val);
      oracle[key] = val;
    } else {
      t = t.erase(to_bytes(key));
      oracle.erase(key);
    }

    // --- Per-round invariants ---
    INFO("round " << round << " key=\"" << key << "\"");

    // size
    REQUIRE(t.size() == oracle.size());

    // get — operated key
    auto tree_val = t.get(to_bytes(key));
    auto oracle_it = oracle.find(key);
    if (oracle_it != oracle.end()) {
      REQUIRE(tree_val.has_value());
      CHECK(*tree_val == oracle_it->second);
    } else {
      CHECK_FALSE(tree_val.has_value());
    }

    // contains — operated key
    CHECK(t.contains(to_bytes(key)) == (oracle.count(key) > 0));

    // --- Full iteration order (every 50 rounds) ---
    if (round % 50 == 0) {
      CHECK(collect(t) == collect_oracle(oracle));
    }

    // --- lower_bound (every 100 rounds) ---
    if (round % 100 == 0 && !oracle.empty()) {
      auto probe = rand_key();
      auto tree_lb = t.lower_bound(to_bytes(probe));
      auto oracle_lb = oracle.lower_bound(probe);

      if (oracle_lb == oracle.end()) {
        CHECK(tree_lb == t.end());
      } else {
        REQUIRE(tree_lb != t.end());
        auto [tk, tv] = *tree_lb;
        CHECK(to_string(tk) == oracle_lb->first);
        CHECK(tv == oracle_lb->second);
      }
    }

    // --- Immutability snapshot (every 200 rounds) ---
    if (check_snapshot) {
      CHECK(collect(snapshot) == snapshot_entries);
    }
  }

  // Final full consistency: every oracle key present with correct value.
  for (auto &[k, v] : oracle) {
    REQUIRE(t.contains(to_bytes(k)));
    CHECK(*t.get(to_bytes(k)) == v);
  }
  CHECK(collect(t) == collect_oracle(oracle));
}

// ---------------------------------------------------------------------------
// Concurrency: readers iterate a persistent snapshot while a writer mutates
// a transient copy. The persistent tree's immutability guarantees that
// readers never observe partial writes — no locks needed.
//
// Under TSan (`xmake f --sanitizer=thread`) this catches data races on
// shared_ptr ref counts and node fields.
// ---------------------------------------------------------------------------
TEST_CASE("RadixTree concurrent readers with writer",
          "[radix_tree][concurrency]") {
  // Build a baseline tree with 500 entries.
  auto tr = Tree{}.transient();
  for (int i = 0; i < 500; ++i) {
    auto key = std::string("key_") + std::to_string(i);
    tr.set(to_bytes(key), i);
  }
  auto snapshot = std::move(tr).persistent();

  // Capture expected state of the snapshot before any concurrent mutation.
  std::vector<std::pair<std::string, int>> expected;
  for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
    auto [k, v] = *it;
    expected.emplace_back(to_string(k), v);
  }

  constexpr int kReaderThreads = 4;
  constexpr int kIterationsPerReader = 20;
  std::vector<bool> reader_ok(kReaderThreads, true);

  // Launch readers that iterate the snapshot concurrently.
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaderThreads; ++r) {
    readers.emplace_back([&, r]() {
      for (int iter = 0; iter < kIterationsPerReader; ++iter) {
        std::vector<std::pair<std::string, int>> observed;
        for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
          auto [k, v] = *it;
          observed.emplace_back(to_string(k), v);
        }
        if (observed != expected) {
          reader_ok[static_cast<std::size_t>(r)] = false;
          return;
        }
      }
    });
  }

  // Writer: mutate a transient derived from the same snapshot.
  // This exercises shared_ptr ref-count bumps on nodes shared with readers.
  std::thread writer([&]() {
    auto w = snapshot.transient();
    for (int i = 500; i < 1000; ++i) {
      auto key = std::string("key_") + std::to_string(i);
      w.set(to_bytes(key), i);
    }
    // Erase some keys that readers are also accessing.
    for (int i = 0; i < 250; ++i) {
      auto key = std::string("key_") + std::to_string(i);
      w.erase(to_bytes(key));
    }
    auto result = std::move(w).persistent();
    CHECK(result.size() == 750U);
  });

  writer.join();
  for (auto &t : readers)
    t.join();

  // Verify all readers saw a consistent snapshot.
  for (int r = 0; r < kReaderThreads; ++r) {
    INFO("reader thread " << r);
    CHECK(reader_ok[static_cast<std::size_t>(r)]);
  }

  // Original snapshot unchanged after all concurrent activity.
  CHECK(snapshot.size() == 500U);
  std::vector<std::pair<std::string, int>> post;
  for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
    auto [k, v] = *it;
    post.emplace_back(to_string(k), v);
  }
  CHECK(post == expected);
}
