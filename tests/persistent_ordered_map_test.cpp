#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>
import bytecask.persistent_ordered_map;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

using Map = bytecask::PersistentOrderedMap<std::string, int>;

auto keys_of(const Map &m) -> std::vector<std::string> {
  std::vector<std::string> ks;
  for (const auto &e : m)
    ks.push_back(e.key);
  return ks;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: empty map
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap empty map", "[persistent_ordered_map]") {
  const Map m;
  CHECK(m.empty());
  CHECK(m.size() == 0U);
  CHECK(m.begin() == m.end());
  CHECK_FALSE(m.contains("x"));
  CHECK(m.get("x") == std::nullopt);
  CHECK(m.lower_bound("x") == m.end());
}

// ---------------------------------------------------------------------------
// Test 2: single insert and lookup
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap single set and get",
          "[persistent_ordered_map]") {
  const auto m1 = Map{}.set("b", 2);
  REQUIRE(m1.size() == 1U);
  REQUIRE(m1.get("b") == 2);
  CHECK(m1.contains("b"));
  CHECK_FALSE(m1.contains("a"));
  CHECK(m1.get("a") == std::nullopt);
}

// ---------------------------------------------------------------------------
// Test 3: insertion maintains sorted order regardless of insert sequence
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap sorted iteration after out-of-order inserts",
          "[persistent_ordered_map]") {
  const auto m = Map{}.set("dog", 3).set("ant", 1).set("cat", 2).set("emu", 4);

  const std::vector<std::string> expected{"ant", "cat", "dog", "emu"};
  CHECK(keys_of(m) == expected);
}

// ---------------------------------------------------------------------------
// Test 4: set overwrites an existing key
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap set overwrites existing key",
          "[persistent_ordered_map]") {
  const auto m1 = Map{}.set("k", 10);
  const auto m2 = m1.set("k", 99);

  CHECK(m1.get("k") == 10); // original unchanged
  CHECK(m2.get("k") == 99);
  CHECK(m2.size() == 1U); // no duplicate entry
}

// ---------------------------------------------------------------------------
// Test 5: persistence — set does not mutate the original
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap set preserves original version",
          "[persistent_ordered_map]") {
  const auto base = Map{}.set("a", 1).set("b", 2);
  const auto extended = base.set("c", 3);

  CHECK(base.size() == 2U);
  CHECK(base.get("c") == std::nullopt);
  CHECK(extended.size() == 3U);
  CHECK(extended.get("c") == 3);
}

// ---------------------------------------------------------------------------
// Test 6: erase removes an existing key
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap erase removes existing key",
          "[persistent_ordered_map]") {
  const auto m1 = Map{}.set("x", 7).set("y", 8).set("z", 9);
  const auto m2 = m1.erase("y");

  CHECK(m2.size() == 2U);
  CHECK(m2.get("y") == std::nullopt);
  CHECK(m2.get("x") == 7);
  CHECK(m2.get("z") == 9);
  CHECK(keys_of(m2) == (std::vector<std::string>{"x", "z"}));
}

// ---------------------------------------------------------------------------
// Test 7: erase is a no-op for an absent key
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap erase absent key is a no-op",
          "[persistent_ordered_map]") {
  const auto m1 = Map{}.set("p", 1);
  const auto m2 = m1.erase("q"); // "q" not present

  CHECK(m2.size() == 1U);
  CHECK(m2.get("p") == 1);
}

// ---------------------------------------------------------------------------
// Test 8: persistence — erase does not mutate the original
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap erase preserves original version",
          "[persistent_ordered_map]") {
  const auto full = Map{}.set("a", 1).set("b", 2).set("c", 3);
  const auto reduced = full.erase("b");

  CHECK(full.size() == 3U);
  CHECK(full.get("b") == 2); // original still has "b"
  CHECK(reduced.size() == 2U);
  CHECK(reduced.get("b") == std::nullopt);
}

// ---------------------------------------------------------------------------
// Test 9a: lower_bound — exact match
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap lower_bound exact match",
          "[persistent_ordered_map]") {
  const auto m = Map{}.set("b", 2).set("d", 4).set("f", 6);
  auto it = m.lower_bound("d");
  REQUIRE(it != m.end());
  CHECK(it->key == "d");
}

// ---------------------------------------------------------------------------
// Test 9b: lower_bound — key between entries points to successor
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap lower_bound between entries",
          "[persistent_ordered_map]") {
  const auto m = Map{}.set("b", 2).set("d", 4).set("f", 6);
  auto it = m.lower_bound("c");
  REQUIRE(it != m.end());
  CHECK(it->key == "d");
}

// ---------------------------------------------------------------------------
// Test 9c: lower_bound — key before all entries returns begin
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap lower_bound before all returns begin",
          "[persistent_ordered_map]") {
  const auto m = Map{}.set("b", 2).set("d", 4).set("f", 6);
  CHECK(m.lower_bound("a") == m.begin());
}

// ---------------------------------------------------------------------------
// Test 9d: lower_bound — key past all entries returns end
// ---------------------------------------------------------------------------
TEST_CASE("PersistentOrderedMap lower_bound past all returns end",
          "[persistent_ordered_map]") {
  const auto m = Map{}.set("b", 2).set("d", 4).set("f", 6);
  CHECK(m.lower_bound("z") == m.end());
}

// ---------------------------------------------------------------------------
// Test 10: transient bulk inserts produce correct sorted map
// ---------------------------------------------------------------------------
TEST_CASE("OrderedMapTransient bulk set produces sorted persistent map",
          "[persistent_ordered_map]") {
  auto t = Map{}.transient();
  t.set("charlie", 3);
  t.set("alpha", 1);
  t.set("bravo", 2);
  t.set("delta", 4);

  const auto result = std::move(t).persistent();

  const std::vector<std::string> expected{"alpha", "bravo", "charlie", "delta"};
  CHECK(keys_of(result) == expected);
  CHECK(result.get("alpha") == 1);
  CHECK(result.get("delta") == 4);
}

// ---------------------------------------------------------------------------
// Test 11: transient set overwrites, erase removes
// ---------------------------------------------------------------------------
TEST_CASE("OrderedMapTransient set overwrites and erase removes",
          "[persistent_ordered_map]") {
  const auto base = Map{}.set("a", 1).set("b", 2).set("c", 3);
  auto t = base.transient();

  t.set("b", 99); // overwrite
  t.erase("a");   // remove
  t.set("d", 4);  // new key

  const auto result = std::move(t).persistent();

  CHECK(result.size() == 3U);
  CHECK(result.get("a") == std::nullopt);
  CHECK(result.get("b") == 99);
  CHECK(result.get("c") == 3);
  CHECK(result.get("d") == 4);
  CHECK(keys_of(result) == (std::vector<std::string>{"b", "c", "d"}));
}

// ---------------------------------------------------------------------------
// Test 12: transient erase absent key is a no-op
// ---------------------------------------------------------------------------
TEST_CASE("OrderedMapTransient erase absent key is a no-op",
          "[persistent_ordered_map]") {
  auto t = Map{}.set("x", 1).transient();
  t.erase("y"); // absent — should not throw or corrupt

  const auto result = std::move(t).persistent();
  CHECK(result.size() == 1U);
  CHECK(result.get("x") == 1);
}

// ---------------------------------------------------------------------------
// Test 13: transient does not mutate the source map
// ---------------------------------------------------------------------------
TEST_CASE("OrderedMapTransient does not mutate source map",
          "[persistent_ordered_map]") {
  const auto original = Map{}.set("a", 1).set("b", 2);
  auto t = original.transient();
  t.set("c", 3);
  t.erase("a");
  [[maybe_unused]] auto updated = std::move(t).persistent();

  // original must be completely unchanged
  CHECK(original.size() == 2U);
  CHECK(original.get("a") == 1);
  CHECK(original.get("b") == 2);
  CHECK(original.get("c") == std::nullopt);
}
