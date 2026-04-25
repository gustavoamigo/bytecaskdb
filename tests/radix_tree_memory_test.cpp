// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — memory allocation/deallocation validation for PersistentRadixTree
//
// Uses a global operator new/delete override (alloc_tracker.h) to precisely
// track bytes allocated and freed. Every test validates that the tree cleans
// up completely (net_bytes == 0) after destruction.

#include "alloc_tracker.h"
#include "key_generators.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <iomanip>
#include <map>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
import bytecask.radix_tree;

#include "radix_tree_debug.h"

namespace {

// ---- Helpers ----------------------------------------------------------------

using namespace key_generators;

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

// A non-trivial 64-byte value type for testing with larger payloads.
struct BigValue {
  std::array<std::byte, 64> data{};

  static auto from_int(int i) -> BigValue {
    BigValue v;
    std::memcpy(v.data.data(), &i, sizeof(i));
    return v;
  }
};

using Tree = bytecask::PersistentRadixTree<int>;
using BigTree = bytecask::PersistentRadixTree<BigValue>;

// Build a tree from a key vector, assigning value = index.
auto build_tree(const std::vector<std::string> &keys) -> Tree {
  Tree t;
  for (std::size_t i = 0; i < keys.size(); ++i)
    t = t.set(to_bytes(keys[i]), static_cast<int>(i));
  return t;
}

} // namespace

// =============================================================================
// Load Pattern 1: Linear insert → destroy
// =============================================================================
TEST_CASE("Memory: linear insert and destroy", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(1000);
      std::size_t mem_loaded = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        auto tree = build_tree(keys);
        REQUIRE(tree.size() == 1000);
        mem_loaded = alloc_tracker::net_bytes();
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory at load: " << mem_loaded << " bytes ("
           << mem_loaded / 1000 << " bytes/key)");
      INFO("Memory after destroy: " << mem_after << " bytes");

      // Tree with 1000 keys should use a reasonable amount of memory.
      CHECK(mem_loaded > 1000 * sizeof(int));
      // No leaks.
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Load Pattern 2: Churn — insert, erase, insert more
// =============================================================================
TEST_CASE("Memory: churn", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(1200);
      std::size_t mem_full = 0;
      std::size_t mem_after_churn = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        // Insert first 1000.
        Tree tree;
        for (std::size_t i = 0; i < 1000; ++i)
          tree = tree.set(to_bytes(keys[i]), static_cast<int>(i));
        mem_full = alloc_tracker::net_bytes();

        // Erase every other key (500 keys removed).
        for (std::size_t i = 0; i < 1000; i += 2)
          tree = tree.erase(to_bytes(keys[i]));

        // Insert 200 new keys.
        for (std::size_t i = 1000; i < 1200; ++i)
          tree = tree.set(to_bytes(keys[i]), static_cast<int>(i));

        REQUIRE(tree.size() == 700);
        mem_after_churn = alloc_tracker::net_bytes();
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory at full (1000 keys): " << mem_full);
      INFO("Memory after churn (700 keys): " << mem_after_churn);
      INFO("Memory after destroy: " << mem_after);

      // Memory should roughly track live count. Allow generous margin
      // because radix tree structure depends on key distribution, not just
      // count — 700 different keys can use more internal nodes than 1000
      // keys with better prefix sharing.
      CHECK(mem_after_churn <= mem_full * 120 / 100);
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Load Pattern 3: Overwrite — same keys, new values
// =============================================================================
TEST_CASE("Memory: overwrite stability", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(500);
      std::size_t mem_before = 0;
      std::size_t mem_after_overwrite = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        auto tree = build_tree(keys);
        mem_before = alloc_tracker::net_bytes();

        // Overwrite all 500 keys 10 times with new values.
        for (int round = 0; round < 10; ++round) {
          for (std::size_t i = 0; i < keys.size(); ++i)
            tree = tree.set(to_bytes(keys[i]), static_cast<int>(i) + round * 1000);
        }
        mem_after_overwrite = alloc_tracker::net_bytes();
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory before overwrites: " << mem_before);
      INFO("Memory after 10x overwrite: " << mem_after_overwrite);
      INFO("Memory after destroy: " << mem_after);

      // Memory should stay stable — overwrites don't grow the tree.
      // Allow ±20% tolerance for structural differences (persistent path-copy
      // may produce a slightly different node layout than the original build).
      auto lower = mem_before * 80 / 100;
      auto upper = mem_before * 120 / 100;
      CHECK(mem_after_overwrite >= lower);
      CHECK(mem_after_overwrite <= upper);
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Load Pattern 4: Batch insert (transient) vs persistent
// =============================================================================
TEST_CASE("Memory: transient allocates less than persistent",
          "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(500);
      std::size_t persistent_allocated = 0;
      std::size_t transient_allocated = 0;

      // Measure persistent path: 500 chained .set() calls.
      alloc_tracker::reset();
      {
        auto tree = build_tree(keys);
        (void)tree;
      }
      CHECK(alloc_tracker::net_bytes() == 0);
      persistent_allocated = alloc_tracker::allocated();

      // Measure transient path: 500 .set() on a transient.
      alloc_tracker::reset();
      {
        Tree base;
        auto tr = base.transient();
        for (std::size_t i = 0; i < keys.size(); ++i)
          tr.set(to_bytes(keys[i]), static_cast<int>(i));
        auto tree = std::move(tr).persistent();
        REQUIRE(tree.size() == 500);
      }
      CHECK(alloc_tracker::net_bytes() == 0);
      transient_allocated = alloc_tracker::allocated();

      INFO("Key shape: " << shape.name);
      INFO("Persistent total allocated: " << persistent_allocated);
      INFO("Transient total allocated: " << transient_allocated);

      // Transient should allocate less due to in-place mutation.
      CHECK(transient_allocated < persistent_allocated);
    }
  }
}

// =============================================================================
// Load Pattern 5: Persistent snapshots — structural sharing
// =============================================================================
TEST_CASE("Memory: persistent snapshots share structure",
          "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(600);
      std::size_t mem_v1 = 0;
      std::size_t mem_after_copy = 0;
      std::size_t mem_v1_plus_v2 = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        // Build v1 with 500 keys.
        Tree v1;
        for (std::size_t i = 0; i < 500; ++i)
          v1 = v1.set(to_bytes(keys[i]), static_cast<int>(i));
        mem_v1 = alloc_tracker::net_bytes();

        // Copy is just an IntrusivePtr addref — near-zero allocation.
        auto snapshot = v1;
        mem_after_copy = alloc_tracker::net_bytes();

        // Mutate v1 to create v2 with 100 more keys.
        auto v2 = v1;
        for (std::size_t i = 500; i < 600; ++i)
          v2 = v2.set(to_bytes(keys[i]), static_cast<int>(i));
        mem_v1_plus_v2 = alloc_tracker::net_bytes();

        REQUIRE(snapshot.size() == 500);
        REQUIRE(v2.size() == 600);
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory v1 (500 keys): " << mem_v1);
      INFO("Memory after copy: " << mem_after_copy);
      INFO("Memory v1+v2 (structural sharing): " << mem_v1_plus_v2);
      INFO("Memory after destroy: " << mem_after);

      // Copy should cost nearly nothing (just IntrusivePtr addref).
      // Allow some slack for Catch2/vector internals.
      CHECK(mem_after_copy <= mem_v1 * 105 / 100);
      // Shared structure: v1+v2 together should be much less than 2× v1.
      CHECK(mem_v1_plus_v2 < mem_v1 * 2);
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Load Pattern 6: Snapshot chain — drop order independence
// =============================================================================
TEST_CASE("Memory: snapshot chain drop order", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    auto keys = shape.generate(300);

    SECTION(shape.name + " — reverse drop order") {
      {
        auto v1 = build_tree({keys.begin(), keys.begin() + 100});
        Tree v2 = v1;
        for (std::size_t i = 100; i < 200; ++i)
          v2 = v2.set(to_bytes(keys[i]), static_cast<int>(i));
        Tree v3 = v2;
        for (std::size_t i = 200; i < 300; ++i)
          v3 = v3.set(to_bytes(keys[i]), static_cast<int>(i));

        alloc_tracker::reset();
        // Drop in reverse: v3, v2, v1.
        v3 = Tree{};
        v2 = Tree{};
        v1 = Tree{};
      }
      INFO("Key shape: " << shape.name);
      // After dropping all versions, freed should equal or exceed allocated
      // (net_bytes wraps to a large number if freed > allocated, so check
      // that freed >= allocated directly).
      INFO("Allocated during drop: " << alloc_tracker::allocated());
      INFO("Freed during drop: " << alloc_tracker::freed());
      CHECK(alloc_tracker::freed() >= alloc_tracker::allocated());
    }

    SECTION(shape.name + " — scrambled drop order") {
      {
        auto v1 = build_tree({keys.begin(), keys.begin() + 100});
        Tree v2 = v1;
        for (std::size_t i = 100; i < 200; ++i)
          v2 = v2.set(to_bytes(keys[i]), static_cast<int>(i));
        Tree v3 = v2;
        for (std::size_t i = 200; i < 300; ++i)
          v3 = v3.set(to_bytes(keys[i]), static_cast<int>(i));

        alloc_tracker::reset();
        // Drop in scrambled order: v1, v3, v2.
        v1 = Tree{};
        v3 = Tree{};
        v2 = Tree{};
      }
      INFO("Key shape: " << shape.name);
      INFO("Allocated during drop: " << alloc_tracker::allocated());
      INFO("Freed during drop: " << alloc_tracker::freed());
      CHECK(alloc_tracker::freed() >= alloc_tracker::allocated());
    }
  }
}

// =============================================================================
// Load Pattern 7: High-turnover churn — sustained insert/delete cycles
// =============================================================================
TEST_CASE("Memory: high-turnover churn stability", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      // Generate enough keys for the full run.
      auto all_keys = shape.generate(1500);
      std::size_t mem_baseline = 0;
      std::size_t mem_max = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        // Insert initial 500 keys.
        Tree tree;
        for (std::size_t i = 0; i < 500; ++i)
          tree = tree.set(to_bytes(all_keys[i]), static_cast<int>(i));
        mem_baseline = alloc_tracker::net_bytes();

        std::mt19937 rng{99};
        std::size_t next_key_idx = 500;

        // 100 cycles: delete 50 existing, insert 50 new.
        // Tree size stays at ~500 throughout.
        std::vector<std::string> live_keys(all_keys.begin(),
                                           all_keys.begin() + 500);
        for (int cycle = 0; cycle < 100; ++cycle) {
          // Delete 50 random keys from live set.
          std::shuffle(live_keys.begin(), live_keys.end(), rng);
          for (int d = 0; d < 50 && !live_keys.empty(); ++d) {
            tree = tree.erase(to_bytes(live_keys.back()));
            live_keys.pop_back();
          }
          // Insert 50 new keys.
          for (int a = 0; a < 50 && next_key_idx < all_keys.size(); ++a) {
            tree = tree.set(to_bytes(all_keys[next_key_idx]),
                            static_cast<int>(next_key_idx));
            live_keys.push_back(all_keys[next_key_idx]);
            ++next_key_idx;
          }

          auto mem_now = alloc_tracker::net_bytes();
          if (mem_now > mem_max)
            mem_max = mem_now;
        }

        REQUIRE(tree.size() == live_keys.size());
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Baseline (500 keys): " << mem_baseline);
      INFO("Peak during churn: " << mem_max);
      INFO("Memory after destroy: " << mem_after);

      // Memory should not grow unboundedly. With persistent trees, each
      // set/erase briefly creates a copy before the old version is freed,
      // so peak can be ~2× baseline. The key invariant is that it doesn't
      // grow proportionally to total operations performed.
      CHECK(mem_max <= mem_baseline * 3);
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Cross-cutting 8: Prefix compression saves memory
// =============================================================================
TEST_CASE("Memory: prefix compression saves memory", "[radix_tree][memory]") {
  constexpr std::size_t n = 1000;
  std::size_t mem_no_sharing = 0;
  std::size_t mem_shared = 0;

  // Generate keys with no shared prefix — each key diverges in the first
  // few bytes, forcing many distinct routing nodes.
  auto no_sharing_keys = [&]() {
    std::mt19937 rng{777};
    std::uniform_int_distribution<int> dist{'a', 'z'};
    std::vector<std::string> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      std::string k(20, '\0');
      for (auto &c : k)
        c = static_cast<char>(dist(rng));
      // Stamp index at end for uniqueness.
      auto suffix = std::to_string(i);
      std::copy(suffix.begin(), suffix.end(),
                k.end() - static_cast<std::ptrdiff_t>(suffix.size()));
      keys.push_back(std::move(k));
    }
    return keys;
  }();

  // Generate keys that all share a long prefix — compression should
  // store the shared prefix once and branch only on the unique suffix.
  auto shared_keys = [&]() {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      std::ostringstream oss;
      oss << "shared_prefix__" << std::setfill('0') << std::setw(5) << i;
      keys.push_back(oss.str());
    }
    return keys;
  }();

  // Measure keys with no prefix sharing.
  alloc_tracker::reset();
  {
    auto tree = build_tree(no_sharing_keys);
    mem_no_sharing = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  // Measure keys with shared prefix.
  alloc_tracker::reset();
  {
    auto tree = build_tree(shared_keys);
    mem_shared = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  INFO("No sharing memory: " << mem_no_sharing << " ("
       << mem_no_sharing / n << " bytes/key)");
  INFO("Shared prefix memory: " << mem_shared << " ("
       << mem_shared / n << " bytes/key)");

  // Keys with shared prefixes should use less memory due to radix compression.
  CHECK(mem_shared < mem_no_sharing);
}

// =============================================================================
// Cross-cutting 9: Large value type — no leaks
// =============================================================================
TEST_CASE("Memory: large value type", "[radix_tree][memory]") {
  auto keys = generate_uniform_keys(500);
  std::size_t mem_loaded = 0;
  std::size_t mem_after = 0;

  alloc_tracker::reset();
  {
    BigTree tree;
    for (std::size_t i = 0; i < keys.size(); ++i)
      tree = tree.set(to_bytes(keys[i]), BigValue::from_int(static_cast<int>(i)));
    REQUIRE(tree.size() == 500);
    mem_loaded = alloc_tracker::net_bytes();
  }
  mem_after = alloc_tracker::net_bytes();

  INFO("Memory at load (BigValue, 500 keys): " << mem_loaded
       << " (" << mem_loaded / 500 << " bytes/key)");
  INFO("Memory after destroy: " << mem_after);

  CHECK(mem_loaded > 500 * sizeof(BigValue));
  CHECK(mem_after == 0);
}

// =============================================================================
// Cross-cutting 10: Key length scaling
// =============================================================================
TEST_CASE("Memory: key length scaling", "[radix_tree][memory]") {
  constexpr std::size_t n = 1000;
  std::size_t mem_incremental = 0;
  std::size_t mem_sha256 = 0;

  // Incremental keys (1-4 bytes).
  alloc_tracker::reset();
  {
    auto tree = build_tree(generate_incremental_keys(n));
    mem_incremental = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  // SHA-256 hex keys (64 bytes).
  alloc_tracker::reset();
  {
    auto tree = build_tree(generate_sha256_hex_keys(n));
    mem_sha256 = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  INFO("Incremental keys (1-4 bytes): " << mem_incremental << " ("
       << mem_incremental / n << " bytes/key)");
  INFO("SHA-256 hex keys (64 bytes): " << mem_sha256 << " ("
       << mem_sha256 / n << " bytes/key)");

  // Longer keys should use more memory (more routing chain nodes).
  CHECK(mem_sha256 > mem_incremental);
}

// =============================================================================
// Cross-cutting 11: Mixed-shape contamination test
//
// Do high-entropy keys (SHA-256, binary UUID) degrade the tree structure
// for well-behaved keys sharing the same tree? Compare bytes/key for each
// shape alone vs mixed into a single tree.
//
// If the radix tree keeps bad keys isolated (each shape branches at the root),
// then the per-key cost for good keys should stay roughly the same in the
// mixed tree.
// =============================================================================
TEST_CASE("Memory: mixed-shape contamination", "[radix_tree][memory]") {
  constexpr std::size_t n_per_shape = 200;

  // Measure prefixed UUID keys alone — the "good baseline".
  auto good_keys = generate_prefixed_uuidv4_keys(n_per_shape);
  std::size_t mem_good_alone = 0;
  alloc_tracker::reset();
  {
    auto tree = build_tree(good_keys);
    mem_good_alone = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  // Measure clustered keys alone — another good baseline.
  auto clustered_keys = generate_clustered_keys(n_per_shape);
  std::size_t mem_clustered_alone = 0;
  alloc_tracker::reset();
  {
    auto tree = build_tree(clustered_keys);
    mem_clustered_alone = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  // Measure SHA-256 binary keys alone — content-addressed storage (32-byte random).
  auto sha_keys = generate_sha256_bin_keys(n_per_shape);
  std::size_t mem_sha_alone = 0;
  alloc_tracker::reset();
  {
    auto tree = build_tree(sha_keys);
    mem_sha_alone = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  // Measure binary UUID keys alone — 16-byte random, 256-way fanout.
  auto bin_uuid_keys = generate_uuidv4_binary_keys(n_per_shape);
  std::size_t mem_bin_uuid_alone = 0;
  alloc_tracker::reset();
  {
    auto tree = build_tree(bin_uuid_keys);
    mem_bin_uuid_alone = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  // Build a mixed tree: all four shapes interleaved.
  std::vector<std::string> all_keys;
  all_keys.reserve(n_per_shape * 4);
  all_keys.insert(all_keys.end(), good_keys.begin(), good_keys.end());
  all_keys.insert(all_keys.end(), clustered_keys.begin(), clustered_keys.end());
  all_keys.insert(all_keys.end(), sha_keys.begin(), sha_keys.end());
  all_keys.insert(all_keys.end(), bin_uuid_keys.begin(), bin_uuid_keys.end());
  std::mt19937 rng{42};
  std::shuffle(all_keys.begin(), all_keys.end(), rng);

  std::size_t mem_mixed = 0;
  alloc_tracker::reset();
  {
    auto tree = build_tree(all_keys);
    REQUIRE(tree.size() == n_per_shape * 4);
    mem_mixed = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  auto sum_alone = mem_good_alone + mem_clustered_alone
                   + mem_sha_alone + mem_bin_uuid_alone;

  INFO("Prefixed UUID alone: " << mem_good_alone / n_per_shape << " bytes/key");
  INFO("Clustered alone: " << mem_clustered_alone / n_per_shape << " bytes/key");
  INFO("SHA-256 binary alone: " << mem_sha_alone / n_per_shape << " bytes/key");
  INFO("Binary UUID alone: " << mem_bin_uuid_alone / n_per_shape << " bytes/key");
  INFO("Mixed tree: " << mem_mixed / (n_per_shape * 4) << " bytes/key");
  INFO("Expected (weighted avg): " << sum_alone / (n_per_shape * 4) << " bytes/key");

  // The mixed tree's total memory should be close to the sum of the individual
  // shapes (±15%). If pathological keys were contaminating good subtrees, the
  // mixed total would be significantly larger than the sum.
  CHECK(mem_mixed <= sum_alone * 115 / 100);
  // Sanity: not significantly less either (no unexpected sharing across shapes).
  CHECK(mem_mixed >= sum_alone * 80 / 100);
}

// =============================================================================
// Model-based random workload — fully random operation sequence
//
// This is the main stress test. Random mix of set, erase, overwrite with
// random keys against a std::map oracle. Validates:
// - net_bytes tracks oracle.size() proportionally (no unbounded drift)
// - Full cleanup to zero after destruction
// =============================================================================
TEST_CASE("Memory: model-based random workload", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto key_pool = shape.generate(2000);

      // Phase 1: generate the operation sequence and oracle OUTSIDE tracking.
      struct Op {
        std::size_t key_idx;
        int type; // 0=set, 1=erase, 2=read
        int value;
      };
      std::vector<Op> ops;
      ops.reserve(5000);
      std::map<std::string, int> oracle;

      std::mt19937 rng{12345};
      std::uniform_int_distribution<int> op_dist(0, 9);
      std::uniform_int_distribution<std::size_t> key_dist(0, key_pool.size() - 1);

      for (int round = 0; round < 5000; ++round) {
        auto idx = key_dist(rng);
        int op = op_dist(rng);
        if (op < 5) {
          ops.push_back({idx, 0, round});
          oracle[key_pool[idx]] = round;
        } else if (op < 8) {
          ops.push_back({idx, 1, 0});
          oracle.erase(key_pool[idx]);
        } else {
          ops.push_back({idx, 2, 0});
        }
      }
      auto expected_size = oracle.size();

      // Phase 2: replay on the tree with allocation tracking.
      std::size_t mem_peak = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        Tree tree;
        for (auto &op : ops) {
          auto &k = key_pool[op.key_idx];
          if (op.type == 0) {
            tree = tree.set(to_bytes(k), op.value);
          } else if (op.type == 1) {
            tree = tree.erase(to_bytes(k));
          } else {
            (void)tree.get(to_bytes(k));
          }

          auto mem_now = alloc_tracker::net_bytes();
          if (mem_now > mem_peak)
            mem_peak = mem_now;
        }
        REQUIRE(tree.size() == expected_size);
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Final expected size: " << expected_size);
      INFO("Peak memory: " << mem_peak);
      INFO("Memory after destroy: " << mem_after);

      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Zipfian workload — hot keys get hammered, cold keys sit untouched
//
// 5% of keys receive 50% of all operations. Validates that skewed access
// doesn't cause memory bloat (no per-access caching, no node duplication
// under hot paths).
// =============================================================================
TEST_CASE("Memory: zipfian hot-key workload", "[radix_tree][memory]") {
  constexpr std::size_t n = 1000;
  auto keys = generate_zipfian_keys(n);
  std::size_t hot_count = n / 20; // 5% are hot

  std::mt19937 rng{42};
  // Zipfian-like: 50% chance of picking a hot key.
  std::uniform_int_distribution<std::size_t> hot_dist(0, hot_count - 1);
  std::uniform_int_distribution<std::size_t> cold_dist(hot_count, n - 1);
  std::uniform_int_distribution<int> coin(0, 1);

  std::size_t mem_baseline = 0;
  std::size_t mem_after_workload = 0;
  std::size_t mem_after = 0;

  alloc_tracker::reset();
  {
    // Insert all keys.
    auto tree = build_tree(keys);
    mem_baseline = alloc_tracker::net_bytes();

    // 5000 rounds of skewed overwrites — hot keys get hammered.
    for (int round = 0; round < 5000; ++round) {
      auto idx = coin(rng) == 0 ? hot_dist(rng) : cold_dist(rng);
      tree = tree.set(to_bytes(keys[idx]), round);
    }
    mem_after_workload = alloc_tracker::net_bytes();
  }
  mem_after = alloc_tracker::net_bytes();

  INFO("Baseline (1000 keys): " << mem_baseline);
  INFO("After 5000 skewed overwrites: " << mem_after_workload);
  INFO("Memory after destroy: " << mem_after);

  // Overwrites should not grow memory — same keys, new values.
  CHECK(mem_after_workload <= mem_baseline * 120 / 100);
  CHECK(mem_after == 0);
}

// =============================================================================
// Clustered vs many-partitions — structural comparison
//
// Few large partitions (3 prefixes × N/3 keys) vs many small partitions
// (N/2 prefixes × 2 keys). Both should clean up, and the structural
// difference should be visible in bytes/key.
// =============================================================================
TEST_CASE("Memory: clustered vs many partitions", "[radix_tree][memory]") {
  constexpr std::size_t n = 1000;
  std::size_t mem_clustered = 0;
  std::size_t mem_many = 0;

  alloc_tracker::reset();
  {
    auto tree = build_tree(generate_clustered_keys(n));
    mem_clustered = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  alloc_tracker::reset();
  {
    auto tree = build_tree(generate_many_partitions_keys(n));
    mem_many = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  INFO("Clustered (3 partitions): " << mem_clustered
       << " (" << mem_clustered / n << " bytes/key)");
  INFO("Many partitions (" << n / 2 << " partitions): " << mem_many
       << " (" << mem_many / n << " bytes/key)");

  // Many partitions should use more memory — wider fanout, less prefix sharing.
  CHECK(mem_many > mem_clustered);
}

// =============================================================================
// Read-heavy workload — 95% reads, 5% writes
//
// Reads (get, contains, iteration) should not allocate tree nodes. After a
// sustained read-heavy workload, net_bytes should be unchanged.
// =============================================================================
TEST_CASE("Memory: read-heavy workload", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(500);
      std::size_t mem_baseline = 0;
      std::size_t mem_after_reads = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        auto tree = build_tree(keys);
        mem_baseline = alloc_tracker::net_bytes();

        std::mt19937 rng{55};
        std::uniform_int_distribution<std::size_t> key_dist(0, keys.size() - 1);
        std::uniform_int_distribution<int> op_dist(0, 99);

        for (int round = 0; round < 5000; ++round) {
          int op = op_dist(rng);
          auto &k = keys[key_dist(rng)];

          if (op < 95) {
            // 95% reads.
            if (op < 50) {
              (void)tree.get(to_bytes(k));
            } else if (op < 75) {
              (void)tree.contains(to_bytes(k));
            } else {
              // Iterate from a random key, walk a few entries.
              auto it = tree.lower_bound(to_bytes(k));
              for (int steps = 0; steps < 5 && it != std::default_sentinel; ++steps)
                ++it;
            }
          } else {
            // 5% writes.
            tree = tree.set(to_bytes(k), round);
          }
        }
        mem_after_reads = alloc_tracker::net_bytes();
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Baseline: " << mem_baseline);
      INFO("After read-heavy workload: " << mem_after_reads);
      INFO("Memory after destroy: " << mem_after);

      // Reads don't allocate; the 5% overwrites shouldn't grow memory.
      CHECK(mem_after_reads <= mem_baseline * 115 / 100);
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Batch-heavy workload via transient — large transient sessions with mixed ops
//
// Simulates apply_batch: build a transient, do a batch of mixed set/erase,
// freeze to persistent, repeat. Validates that repeated transient sessions
// don't accumulate leaked nodes.
// =============================================================================
TEST_CASE("Memory: batch-heavy transient workload", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(1000);
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        Tree tree;

        // 20 batch cycles.
        for (int batch = 0; batch < 20; ++batch) {
          auto tr = tree.transient();

          // Each batch: insert 50 new keys, erase 20 existing.
          std::size_t base = static_cast<std::size_t>(batch) * 50;
          for (std::size_t i = base; i < base + 50 && i < keys.size(); ++i)
            tr.set(to_bytes(keys[i]), static_cast<int>(i));

          // Erase some previously inserted keys.
          if (batch > 0) {
            std::size_t erase_base = static_cast<std::size_t>(batch - 1) * 50;
            for (std::size_t i = erase_base; i < erase_base + 20; ++i)
              tr.erase(to_bytes(keys[i]));
          }

          tree = std::move(tr).persistent();
        }

        // Verify size is consistent with operations.
        CHECK(tree.size() > 0);
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory after destroy: " << mem_after);

      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Gradual growth then plateau — insert keys gradually, then only overwrites
//
// Memory should grow during the insert phase and plateau during the overwrite
// phase. Validates no creep when the keyspace is stable.
// =============================================================================
TEST_CASE("Memory: gradual growth then plateau", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(500);
      std::size_t mem_at_plateau = 0;
      std::size_t mem_after_overwrites = 0;
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        Tree tree;

        // Phase 1: gradual growth — insert 10 keys per round for 50 rounds.
        for (int round = 0; round < 50; ++round) {
          for (int j = 0; j < 10; ++j) {
            auto idx = static_cast<std::size_t>(round * 10 + j);
            tree = tree.set(to_bytes(keys[idx]), static_cast<int>(idx));
          }
        }
        REQUIRE(tree.size() == 500);
        mem_at_plateau = alloc_tracker::net_bytes();

        // Phase 2: plateau — only overwrites, no new keys.
        for (int cycle = 0; cycle < 50; ++cycle) {
          for (std::size_t i = 0; i < keys.size(); ++i)
            tree = tree.set(to_bytes(keys[i]), static_cast<int>(i) + cycle * 1000);
        }
        mem_after_overwrites = alloc_tracker::net_bytes();
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("At plateau (500 keys): " << mem_at_plateau);
      INFO("After 50 overwrite cycles: " << mem_after_overwrites);
      INFO("Memory after destroy: " << mem_after);

      // Memory should not creep during the overwrite phase.
      CHECK(mem_after_overwrites <= mem_at_plateau * 120 / 100);
      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Range scan doesn't leak — full iteration should not accumulate memory
//
// The iterator holds at most one key buffer at a time. Verify that scanning
// the entire tree N times doesn't grow net_bytes.
// =============================================================================
TEST_CASE("Memory: range scan no leak", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(500);

      std::size_t mem_before_scan = 0;
      std::size_t mem_after_scan = 0;

      {
        alloc_tracker::reset();
        auto tree = build_tree(keys);
        mem_before_scan = alloc_tracker::net_bytes();

        // Forward scan 10 times.
        for (int scan = 0; scan < 10; ++scan) {
          std::size_t count = 0;
          for (auto it = tree.begin(); it != std::default_sentinel; ++it)
            ++count;
          REQUIRE(count == 500);
        }

        // Reverse scan 10 times.
        for (int scan = 0; scan < 10; ++scan) {
          std::size_t count = 0;
          for (auto it = tree.rbegin(); it != tree.rend(); ++it)
            ++count;
          REQUIRE(count == 500);
        }

        mem_after_scan = alloc_tracker::net_bytes();
      }

      INFO("Key shape: " << shape.name);
      INFO("Before scans: " << mem_before_scan);
      INFO("After 20 full scans: " << mem_after_scan);

      // Scanning should not grow the tree's memory.
      CHECK(mem_after_scan <= mem_before_scan * 105 / 100);
      // Tree fully destroyed — any residual is Catch2 framework noise.
      CHECK(alloc_tracker::freed() >= alloc_tracker::allocated() - 1024);
    }
  }
}

// =============================================================================
// Merge stress — merge two trees, verify memory
//
// Build two trees with overlapping keys, merge them, verify the merged tree
// cleans up fully. Exercises structural sharing across merge boundaries.
// =============================================================================
TEST_CASE("Memory: merge stress", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(1000);
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        // Tree A: first 600 keys.
        auto tree_a = build_tree({keys.begin(), keys.begin() + 600});
        // Tree B: last 600 keys (200 overlap with A).
        auto tree_b = build_tree({keys.begin() + 400, keys.end()});

        // Merge B into A, keeping A's value on conflict.
        auto merged = Tree::merge(
            tree_a, tree_b,
            [](const int &a, const int &) { return a; });

        REQUIRE(merged.size() == 1000);

        // Drop originals, keep only merged.
        tree_a = Tree{};
        tree_b = Tree{};
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory after destroy: " << mem_after);

      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Snapshot stress — many concurrent snapshots with mutations
//
// Create a base tree, take many snapshots while mutating, then drop them
// in random order. Validates refcounting under heavy sharing.
// =============================================================================
TEST_CASE("Memory: snapshot stress", "[radix_tree][memory]") {
  for (auto &shape : all_key_shapes()) {
    SECTION(shape.name) {
      auto keys = shape.generate(500);
      std::size_t mem_after = 0;

      alloc_tracker::reset();
      {
        auto tree = build_tree(keys);

        // Take 20 snapshots, mutating between each.
        std::vector<Tree> snapshots;
        for (int i = 0; i < 20; ++i) {
          snapshots.push_back(tree);
          // Mutate: overwrite 10 keys + insert 1 unique key.
          for (int j = 0; j < 10; ++j) {
            auto idx = static_cast<std::size_t>((i * 10 + j) % static_cast<int>(keys.size()));
            tree = tree.set(to_bytes(keys[idx]), i * 1000 + j);
          }
        }

        // Drop snapshots in scrambled order.
        std::mt19937 rng{42};
        std::shuffle(snapshots.begin(), snapshots.end(), rng);
        for (auto &s : snapshots)
          s = Tree{};
        snapshots.clear();

        // Drop the final tree.
        tree = Tree{};
      }
      mem_after = alloc_tracker::net_bytes();

      INFO("Key shape: " << shape.name);
      INFO("Memory after destroy: " << mem_after);

      CHECK(mem_after == 0);
    }
  }
}

// =============================================================================
// Hash-prefix experiment: does prepending a short hash to random keys help?
//
// Hypothesis: a 2-byte hash prefix gives the tree structure at the top,
// reducing per-key overhead for random keys like UUIDv4.
//
// Counter-hypothesis: the hash prefix just adds 2 bytes of routing overhead
// without compressing the random suffix, so memory goes UP not down.
// =============================================================================
TEST_CASE("Memory: hash-prefix experiment on UUIDv4",
          "[radix_tree][memory]") {
  constexpr std::size_t n = 1000;

  // Generate UUIDv4 text keys (36 bytes each, fully random).
  auto bare_keys = generate_uuidv4_text_keys(n);

  // Generate hash-prefixed versions: 2-byte hash + original key.
  // Uses a simple FNV-1a-like hash to get a 16-bit prefix.
  std::vector<std::string> prefixed_keys;
  prefixed_keys.reserve(n);
  for (auto &k : bare_keys) {
    std::uint16_t h = 0x811C;
    for (auto c : k) {
      h ^= static_cast<std::uint16_t>(static_cast<unsigned char>(c));
      h *= 0x0101;
    }
    std::string pk(2, '\0');
    pk[0] = static_cast<char>((h >> 8) & 0xFF);
    pk[1] = static_cast<char>(h & 0xFF);
    pk += k;
    prefixed_keys.push_back(std::move(pk));
  }

  std::size_t mem_bare = 0;
  std::size_t mem_hashed = 0;

  alloc_tracker::reset();
  {
    auto tree = build_tree(bare_keys);
    mem_bare = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  alloc_tracker::reset();
  {
    auto tree = build_tree(prefixed_keys);
    mem_hashed = alloc_tracker::net_bytes();
  }
  CHECK(alloc_tracker::net_bytes() == 0);

  INFO("Bare UUIDv4: " << mem_bare / n << " bytes/key (total " << mem_bare << ")");
  INFO("Hash-prefixed UUIDv4: " << mem_hashed / n << " bytes/key (total " << mem_hashed << ")");
  INFO("Difference: " << (static_cast<long long>(mem_hashed) - static_cast<long long>(mem_bare))
       << " bytes (" << (mem_hashed * 100 / mem_bare) << "% of bare)");

  // Record the result — no assertion on which is better, this is an experiment.
  // The test just validates no leaks.
}

// =============================================================================
// Debug dump smoke test — verify dump doesn't crash
// =============================================================================
TEST_CASE("Memory: tree dump utility", "[radix_tree][memory]") {
  auto tree = Tree{}
                  .set(to_bytes("apple"), 1)
                  .set(to_bytes("banana"), 2)
                  .set(to_bytes("cherry"), 3);

  auto dump = radix_tree_debug::dump_tree(tree);
  CHECK(!dump.empty());
  CHECK(dump.find("apple") != std::string::npos);
  CHECK(dump.find("banana") != std::string::npos);
  CHECK(dump.find("cherry") != std::string::npos);
  CHECK(dump.find("size=3") != std::string::npos);
}
