#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>
import bytecask.engine;

namespace {

auto to_bytes(std::string_view sv) -> bytecask::BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(const bytecask::Bytes &bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  std::ranges::transform(bytes, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

// Creates a unique temp directory for each test, cleaned up on scope exit.
struct TempDir {
  std::filesystem::path path;

  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "bc_test_{}",
                 std::chrono::system_clock::now().time_since_epoch().count())} {
    std::filesystem::create_directories(path);
  }

  ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

// ---------------------------------------------------------------------------
// Test 1: open() creates the directory and does not throw
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask open creates directory", "[bytecask]") {
  TempDir td;
  const auto db_path = td.path / "db";
  REQUIRE_NOTHROW(bytecask::Bytecask::open(db_path));
  CHECK(std::filesystem::is_directory(db_path));
}

// ---------------------------------------------------------------------------
// Test 2: put + get round-trip
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask put and get round-trip", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("key1"), to_bytes("value1"));

  const auto result = db.get({}, to_bytes("key1"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "value1");
}

// ---------------------------------------------------------------------------
// Test 3: put overwrites an existing key
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask put overwrites existing key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("key1"), to_bytes("first"));
  db.put({}, to_bytes("key1"), to_bytes("second"));

  const auto result = db.get({}, to_bytes("key1"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "second");
}

// ---------------------------------------------------------------------------
// Test 4: del returns false for a key that does not exist
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask del returns false for absent key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  CHECK_FALSE(db.del({}, to_bytes("missing")));
}

// ---------------------------------------------------------------------------
// Test 5: del returns true; subsequent get returns nullopt
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask del existing key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("key1"), to_bytes("value1"));
  const bool removed = db.del({}, to_bytes("key1"));

  CHECK(removed);
  CHECK_FALSE(db.get({}, to_bytes("key1")).has_value());
}

// ---------------------------------------------------------------------------
// Test 6: contains_key tracks puts and dels
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask contains_key tracks mutations", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  CHECK_FALSE(db.contains_key(to_bytes("k")));
  db.put({}, to_bytes("k"), to_bytes("v"));
  CHECK(db.contains_key(to_bytes("k")));
  CHECK(db.del({}, to_bytes("k")));
  CHECK_FALSE(db.contains_key(to_bytes("k")));
}

// ---------------------------------------------------------------------------
// Test 7: apply_batch — mixed puts and del, all visible atomically
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask apply_batch mixed operations", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  // Pre-insert a key that the batch will remove.
  db.put({}, to_bytes("del"), to_bytes("gone"));

  bytecask::Batch batch;
  batch.put(to_bytes("a"), to_bytes("alpha"));
  batch.put(to_bytes("b"), to_bytes("beta"));
  batch.del(to_bytes("del"));
  db.apply_batch({}, std::move(batch));

  REQUIRE(db.get({}, to_bytes("a")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("a"))) == "alpha");
  REQUIRE(db.get({}, to_bytes("b")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("b"))) == "beta");
  CHECK_FALSE(db.get({}, to_bytes("del")).has_value());
}

// ---------------------------------------------------------------------------
// Test 8: iter_from({}) returns all entries in ascending key order
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask iter_from returns entries in ascending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("c"), to_bytes("cv"));
  db.put({}, to_bytes("a"), to_bytes("av"));
  db.put({}, to_bytes("b"), to_bytes("bv"));

  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (auto &[k, v] : db.iter_from({})) {
    keys.push_back(to_string(k));
    values.push_back(to_string(v));
  }

  REQUIRE(keys.size() == 3);
  CHECK(keys[0] == "a");
  CHECK(keys[1] == "b");
  CHECK(keys[2] == "c");
  CHECK(values[0] == "av");
  CHECK(values[1] == "bv");
  CHECK(values[2] == "cv");
}

// ---------------------------------------------------------------------------
// Test 9: iter_from(mid_key) starts at that key, earlier keys are absent
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask iter_from starts from given key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("apple"), to_bytes("1"));
  db.put({}, to_bytes("banana"), to_bytes("2"));
  db.put({}, to_bytes("cherry"), to_bytes("3"));

  std::vector<std::string> keys;
  for (auto &[k, v] : db.iter_from({}, to_bytes("banana"))) {
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 2);
  CHECK(keys[0] == "banana");
  CHECK(keys[1] == "cherry");
}

// ---------------------------------------------------------------------------
// Test 10: keys_from({}) returns all keys ascending — no data file I/O
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask keys_from returns all keys in ascending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("z"), to_bytes("zv"));
  db.put({}, to_bytes("m"), to_bytes("mv"));
  db.put({}, to_bytes("a"), to_bytes("av"));

  std::vector<std::string> keys;
  for (auto &k : db.keys_from({})) {
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 3);
  CHECK(keys[0] == "a");
  CHECK(keys[1] == "m");
  CHECK(keys[2] == "z");
}

// ---------------------------------------------------------------------------
// Test 11: rotation creates a second .data file on disk
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask rotation creates new data file", "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";
  // A threshold of 1 means any write will trigger rotation.
  auto db = bytecask::Bytecask::open(db_path, 1);

  db.put({}, to_bytes("key"), to_bytes("value"));

  // Count .data files: should be 2 (the sealed one + the new active one).
  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data") {
      ++data_file_count;
    }
  }
  CHECK(data_file_count == 2);
}

// ---------------------------------------------------------------------------
// Test 12: get() resolves value from a rotated (sealed) file
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask get resolves value from rotated file",
          "[bytecask][rotation]") {
  TempDir td;
  // Threshold of 1 triggers rotation after each write.
  auto db = bytecask::Bytecask::open(td.path / "db", 1);

  db.put({}, to_bytes("key_a"), to_bytes("alpha"));
  // After put, active file is now rotated. key_a lives in the sealed file.
  db.put({}, to_bytes("key_b"), to_bytes("beta"));

  const auto a = db.get({}, to_bytes("key_a"));
  REQUIRE(a.has_value());
  CHECK(to_string(*a) == "alpha");

  const auto b = db.get({}, to_bytes("key_b"));
  REQUIRE(b.has_value());
  CHECK(to_string(*b) == "beta");
}

// ---------------------------------------------------------------------------
// Test 13: iter_from spans entries across multiple data files
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask iter_from spans multiple rotated files",
          "[bytecask][rotation]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db", 1);

  db.put({}, to_bytes("a"), to_bytes("av"));
  db.put({}, to_bytes("b"), to_bytes("bv"));
  db.put({}, to_bytes("c"), to_bytes("cv"));

  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (auto &[k, v] : db.iter_from({})) {
    keys.push_back(to_string(k));
    values.push_back(to_string(v));
  }

  REQUIRE(keys.size() == 3);
  CHECK(keys[0] == "a");
  CHECK(keys[1] == "b");
  CHECK(keys[2] == "c");
  CHECK(values[0] == "av");
  CHECK(values[1] == "bv");
  CHECK(values[2] == "cv");
}

// ---------------------------------------------------------------------------
// Test 14: flush_hints writes a .hint file for each sealed data file
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask flush_hints writes hint file for sealed file",
          "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";
  auto db = bytecask::Bytecask::open(db_path, 1);

  db.put({}, to_bytes("k"), to_bytes("v"));
  // At this point one file is sealed; a new active file exists too.

  db.flush_hints();

  int hint_count = 0;
  int tmp_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint") {
      ++hint_count;
    }
    if (e.path().string().ends_with(".hint.tmp")) {
      ++tmp_count;
    }
  }
  CHECK(hint_count >= 1);
  CHECK(tmp_count == 0);
}

// ---------------------------------------------------------------------------
// Test 15: flush_hints is idempotent — calling it twice does not error
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask flush_hints is idempotent", "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";
  auto db = bytecask::Bytecask::open(db_path, 1);

  db.put({}, to_bytes("k"), to_bytes("v"));
  db.flush_hints();

  // Collect hint file count after first call.
  int count_first = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint")
      ++count_first;
  }

  REQUIRE_NOTHROW(db.flush_hints());

  int count_second = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint")
      ++count_second;
  }

  CHECK(count_first == count_second);
}

// ---------------------------------------------------------------------------
// Test 16: ~Bytecask calls flush_hints — hint file exists after scope exit
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask destructor flushes hint files", "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path, 1);
    db.put({}, to_bytes("k"), to_bytes("v"));
    // db destroyed here — destructor should call flush_hints()
  }

  int hint_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint")
      ++hint_count;
  }
  CHECK(hint_count >= 1);
}

// ---------------------------------------------------------------------------
// Test 17: WriteOptions{.sync=false} — data is written but fdatasync skipped;
//           values are still readable within the same engine instance.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask WriteOptions sync=false data still readable",
          "[bytecask][write_options]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  const bytecask::WriteOptions no_sync{.sync = false};
  db.put(no_sync, to_bytes("k1"), to_bytes("v1"));
  db.put(no_sync, to_bytes("k2"), to_bytes("v2"));

  const auto r1 = db.get({}, to_bytes("k1"));
  REQUIRE(r1.has_value());
  CHECK(to_string(*r1) == "v1");

  const auto r2 = db.get({}, to_bytes("k2"));
  REQUIRE(r2.has_value());
  CHECK(to_string(*r2) == "v2");
}

// ---------------------------------------------------------------------------
// Test 18: WriteOptions{.sync=false} on del — key is removed, no fdatasync.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask WriteOptions sync=false del still removes key",
          "[bytecask][write_options]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("v"));

  const bytecask::WriteOptions no_sync{.sync = false};
  const bool removed = db.del(no_sync, to_bytes("k"));

  CHECK(removed);
  CHECK_FALSE(db.get({}, to_bytes("k")).has_value());
}

// ---------------------------------------------------------------------------
// Test 19: WriteOptions{.sync=false} on apply_batch — results visible.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask WriteOptions sync=false apply_batch results visible",
          "[bytecask][write_options]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  const bytecask::WriteOptions no_sync{.sync = false};
  bytecask::Batch batch;
  batch.put(to_bytes("x"), to_bytes("xv"));
  batch.put(to_bytes("y"), to_bytes("yv"));
  db.apply_batch(no_sync, std::move(batch));

  REQUIRE(db.get({}, to_bytes("x")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("x"))) == "xv");
  REQUIRE(db.get({}, to_bytes("y")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("y"))) == "yv");
}
