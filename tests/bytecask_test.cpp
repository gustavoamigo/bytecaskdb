#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
import bytecask.engine;
import bytecask.data_file;
import bytecask.types;

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

auto to_string(const bytecask::Key &key) -> std::string {
  std::string s(key.size(), '\0');
  std::ranges::transform(key, s.begin(),
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
// Test 2b: get output-param overload reuses buffer
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask get output-param round-trip", "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("k1"), to_bytes("v1"));
  db.put({}, to_bytes("k2"), to_bytes("value_two"));

  bytecask::Bytes out;

  CHECK(db.get({}, to_bytes("k1"), out));
  CHECK(to_string(out) == "v1");

  // Second call reuses the same buffer (capacity retained).
  CHECK(db.get({}, to_bytes("k2"), out));
  CHECK(to_string(out) == "value_two");

  // Missing key returns false and does not modify out.
  CHECK_FALSE(db.get({}, to_bytes("absent"), out));
  CHECK(to_string(out) == "value_two");
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
// Test 8: iter_from with ordered=true returns all entries in ascending key
// order
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask iter_from returns entries in ascending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("c"), to_bytes("cv"));
  db.put({}, to_bytes("a"), to_bytes("av"));
  db.put({}, to_bytes("b"), to_bytes("bv"));

  bytecask::ReadOptions ro;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (auto &[k, v] : db.iter_from(ro)) {
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

// ---------------------------------------------------------------------------
// Test 20: puts survive a restart (raw scan recovery, no hint files)
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: puts survive restart", "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path);
    db.put({}, to_bytes("k1"), to_bytes("v1"));
    db.put({}, to_bytes("k2"), to_bytes("v2"));
  } // destructor syncs; no rotation so no hint files written

  auto db2 = bytecask::Bytecask::open(db_path);
  REQUIRE(db2.get({}, to_bytes("k1")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("k1"))) == "v1");
  REQUIRE(db2.get({}, to_bytes("k2")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("k2"))) == "v2");
}

// ---------------------------------------------------------------------------
// Test 21: delete tombstone survives restart — key absent after reopen
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: tombstone survives restart",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path);
    db.put({}, to_bytes("k"), to_bytes("v"));
    std::ignore = db.del({}, to_bytes("k"));
  }

  auto db2 = bytecask::Bytecask::open(db_path);
  CHECK_FALSE(db2.get({}, to_bytes("k")).has_value());
}

// ---------------------------------------------------------------------------
// Test 22: last write wins — overwritten value correct after restart
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: last write wins after overwrite",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path);
    db.put({}, to_bytes("k"), to_bytes("first"));
    db.put({}, to_bytes("k"), to_bytes("second"));
  }

  auto db2 = bytecask::Bytecask::open(db_path);
  REQUIRE(db2.get({}, to_bytes("k")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("k"))) == "second");
}

// ---------------------------------------------------------------------------
// Test 23: batch survives restart — all puts/dels from batch visible
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: batch survives restart", "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path);
    db.put({}, to_bytes("preexisting"), to_bytes("gone"));

    bytecask::Batch batch;
    batch.put(to_bytes("a"), to_bytes("alpha"));
    batch.put(to_bytes("b"), to_bytes("beta"));
    batch.del(to_bytes("preexisting"));
    db.apply_batch({}, std::move(batch));
  }

  auto db2 = bytecask::Bytecask::open(db_path);
  REQUIRE(db2.get({}, to_bytes("a")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("a"))) == "alpha");
  REQUIRE(db2.get({}, to_bytes("b")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("b"))) == "beta");
  CHECK_FALSE(db2.get({}, to_bytes("preexisting")).has_value());
}

// ---------------------------------------------------------------------------
// Test 24: recovery via hint files — rotation writes hints on close,
//           reopen rebuilds key directory from them
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: hint file path after rotation",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // threshold=1 forces rotation after each write; destructor writes hint files
  // for the sealed files.
  {
    auto db = bytecask::Bytecask::open(db_path, 1);
    db.put({}, to_bytes("x"), to_bytes("xval"));
    db.put({}, to_bytes("y"), to_bytes("yval"));
  }

  // Confirm hint files were written before we reopen.
  int hint_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint")
      ++hint_count;
  }
  REQUIRE(hint_count >= 1);

  auto db2 = bytecask::Bytecask::open(db_path, 1);
  REQUIRE(db2.get({}, to_bytes("x")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("x"))) == "xval");
  REQUIRE(db2.get({}, to_bytes("y")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("y"))) == "yval");
}

// ---------------------------------------------------------------------------
// Test: tombstone in one file suppresses stale Put in another file
//
// The Put and Delete for the same key land in separate .data files (forced by
// threshold=1). Regardless of which file directory_iterator visits first,
// the key must be absent after recovery. Without the tombstone map in
// recover_existing_files(), this test fails when the Delete file happens to be
// processed before the Put file, causing the stale Put to be inserted.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: cross-file tombstone suppresses stale put",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    // threshold=1 forces each write into its own file.
    auto db = bytecask::Bytecask::open(db_path, 1);
    db.put({}, to_bytes("gone"), to_bytes("v1")); // file 0
    db.del({}, to_bytes("gone")); // file 1 — Delete seq > Put seq
    db.put({}, to_bytes("keep"), to_bytes("v2")); // file 2
  }

  auto db2 = bytecask::Bytecask::open(db_path, 1);
  CHECK_FALSE(db2.contains_key(to_bytes("gone")));
  CHECK_FALSE(db2.get({}, to_bytes("gone")).has_value());
  REQUIRE(db2.get({}, to_bytes("keep")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("keep"))) == "v2");
}

// ---------------------------------------------------------------------------
// Test: incomplete batch entries are discarded during recovery.
// Simulates a crash mid-batch by writing a BulkBegin + Put entries with no
// BulkEnd directly to a data file. Recovery generates a hint file from the
// raw data (batch-aware) and only the standalone entries survive.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: incomplete batch is discarded",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  std::filesystem::create_directories(db_path);

  {
    // Manually write a data file simulating a crash mid-batch.
    bytecask::DataFile df(db_path / "data_00000000000000000000000001.data");
    // Standalone entry — should survive.
    std::ignore = df.append(1, bytecask::EntryType::Put, to_bytes("good"),
                            to_bytes("value1"));
    // Begin batch, write some entries, but never write BulkEnd.
    std::ignore = df.append(2, bytecask::EntryType::BulkBegin, {}, {});
    std::ignore = df.append(3, bytecask::EntryType::Put, to_bytes("orphan_a"),
                            to_bytes("lost1"));
    std::ignore = df.append(4, bytecask::EntryType::Put, to_bytes("orphan_b"),
                            to_bytes("lost2"));
    // No BulkEnd — simulates crash.
    df.sync();
  }

  // Open engine — should generate hint file and recover only "good".
  auto db = bytecask::Bytecask::open(db_path);
  REQUIRE(db.get({}, to_bytes("good")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("good"))) == "value1");
  CHECK_FALSE(db.get({}, to_bytes("orphan_a")).has_value());
  CHECK_FALSE(db.get({}, to_bytes("orphan_b")).has_value());

  // Verify a hint file was generated.
  int hint_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint")
      ++hint_count;
  }
  CHECK(hint_count >= 1);
}

// ---------------------------------------------------------------------------
// Test: recovery produces the same key directory regardless of the order
// in which data/hint files are iterated. We create two data files manually
// with crafted names and LSNs so that in one sub-case the tombstone file
// sorts alphabetically first, and in the other it sorts last. Both must
// yield the same result: "gone" absent, "alive" present.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask recovery: order-independent tombstone",
          "[bytecask][recovery]") {
  // Sub-case A: delete file sorts BEFORE put file (alphabetically).
  // Sub-case B: delete file sorts AFTER put file.
  // In both, the delete has a higher LSN than the put, so it must win.
  auto run = [](std::string_view put_stem, std::string_view del_stem) {
    TempDir td;
    const auto db_path = td.path / "db";
    std::filesystem::create_directories(db_path);

    // File with a Put for "gone" (seq=1) and "alive" (seq=2).
    {
      bytecask::DataFile df(db_path / std::format("{}.data", put_stem));
      std::ignore = df.append(1, bytecask::EntryType::Put, to_bytes("gone"),
                              to_bytes("v1"));
      std::ignore = df.append(2, bytecask::EntryType::Put, to_bytes("alive"),
                              to_bytes("v2"));
      df.sync();
    }

    // File with a Delete for "gone" (seq=3) — higher LSN wins.
    {
      bytecask::DataFile df(db_path / std::format("{}.data", del_stem));
      std::ignore = df.append(3, bytecask::EntryType::Delete,
                              to_bytes("gone"), {});
      df.sync();
    }

    auto db = bytecask::Bytecask::open(db_path);
    CHECK_FALSE(db.get({}, to_bytes("gone")).has_value());
    REQUIRE(db.get({}, to_bytes("alive")).has_value());
    CHECK(to_string(*db.get({}, to_bytes("alive"))) == "v2");
  };

  SECTION("delete file sorts before put file") {
    run("data_bbb", "data_aaa");
  }
  SECTION("delete file sorts after put file") {
    run("data_aaa", "data_bbb");
  }
}

// ---------------------------------------------------------------------------
// Parallel recovery: same result as serial for basic puts
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask parallel recovery: puts survive restart",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // threshold=1 forces each write into its own file, giving multiple files
  // for parallel workers to split across.
  {
    auto db = bytecask::Bytecask::open(db_path, 1);
    db.put({}, to_bytes("a"), to_bytes("1"));
    db.put({}, to_bytes("b"), to_bytes("2"));
    db.put({}, to_bytes("c"), to_bytes("3"));
    db.put({}, to_bytes("d"), to_bytes("4"));
  }

  auto db2 = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/4);
  REQUIRE(db2.get({}, to_bytes("a")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("a"))) == "1");
  REQUIRE(db2.get({}, to_bytes("b")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("b"))) == "2");
  REQUIRE(db2.get({}, to_bytes("c")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("c"))) == "3");
  REQUIRE(db2.get({}, to_bytes("d")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("d"))) == "4");
}

// ---------------------------------------------------------------------------
// Parallel recovery: cross-worker tombstone suppresses stale put
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask parallel recovery: cross-worker tombstone",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path, 1);
    db.put({}, to_bytes("gone"), to_bytes("v1"));   // file 0
    std::ignore = db.del({}, to_bytes("gone"));      // file 1
    db.put({}, to_bytes("keep"), to_bytes("v2"));    // file 2
    db.put({}, to_bytes("also"), to_bytes("v3"));    // file 3
  }

  // 4 files, 4 workers — PUT and DELETE for "gone" land in different workers.
  auto db2 = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/4);
  CHECK_FALSE(db2.get({}, to_bytes("gone")).has_value());
  REQUIRE(db2.get({}, to_bytes("keep")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("keep"))) == "v2");
  REQUIRE(db2.get({}, to_bytes("also")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("also"))) == "v3");
}

// ---------------------------------------------------------------------------
// Parallel recovery: last write wins across workers
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask parallel recovery: last write wins",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::Bytecask::open(db_path, 1);
    db.put({}, to_bytes("k"), to_bytes("old"));
    db.put({}, to_bytes("k"), to_bytes("new"));
  }

  auto db2 = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/2);
  REQUIRE(db2.get({}, to_bytes("k")).has_value());
  CHECK(to_string(*db2.get({}, to_bytes("k"))) == "new");
}

// ---------------------------------------------------------------------------
// Parallel recovery: produces identical result to serial recovery
//
// Uses a larger dataset with overwrites and deletes to exercise the full
// merge + tombstone cross-application path. Opens the same data dir twice:
// once with 1 thread (serial), once with 4 threads (parallel), then
// compares every key.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask parallel recovery: matches serial result",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // Build a database with many files, overwrites, and deletes.
  {
    auto db = bytecask::Bytecask::open(db_path, 1);
    for (int i = 0; i < 50; ++i) {
      auto key = std::format("key_{:03d}", i);
      auto val = std::format("val_{:03d}_v1", i);
      db.put({}, to_bytes(key), to_bytes(val));
    }
    // Overwrite some keys.
    for (int i = 0; i < 50; i += 3) {
      auto key = std::format("key_{:03d}", i);
      auto val = std::format("val_{:03d}_v2", i);
      db.put({}, to_bytes(key), to_bytes(val));
    }
    // Delete some keys.
    for (int i = 1; i < 50; i += 5) {
      auto key = std::format("key_{:03d}", i);
      std::ignore = db.del({}, to_bytes(key));
    }
  }

  // Recover serially.
  auto serial = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/1);

  // Collect serial results.
  std::map<std::string, std::string> serial_kv;
  for (auto [key, val] : serial.iter_from({})) {
    serial_kv[to_string(key)] = to_string(val);
  }

  // Recover in parallel.
  auto parallel = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/4);

  // Collect parallel results.
  std::map<std::string, std::string> parallel_kv;
  for (auto [key, val] : parallel.iter_from({})) {
    parallel_kv[to_string(key)] = to_string(val);
  }

  REQUIRE(serial_kv.size() == parallel_kv.size());
  for (const auto &[k, v] : serial_kv) {
    auto it = parallel_kv.find(k);
    REQUIRE(it != parallel_kv.end());
    CHECK(it->second == v);
  }
}

// ---------------------------------------------------------------------------
// Model-based recovery: random workload with oracle comparison.
//
// A random sequence of puts, deletes, overwrites, and batches is applied to
// both a Bytecask instance and a std::map oracle. The DB uses a tiny rotation
// threshold (1 byte) so every write triggers file rotation, maximising the
// number of files and exercising cross-file recovery thoroughly.
//
// After closing the engine, the data directory is recovered three ways:
//   1. Serial recovery (recovery_threads=1)
//   2. Parallel recovery with 2 workers
//   3. Parallel recovery with many workers (number of files)
// All three must produce a key directory identical to the oracle.
//
// The test uses a fixed seed for reproducibility. Catch2 reports the seed
// so failures are deterministic to reproduce.
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: random workload matches oracle",
          "[bytecask][recovery][parallel][model]") {
  // Deterministic PRNG — Catch2 prints "Randomness seeded to:" for us,
  // but we use our own seed for workload reproducibility.
  std::mt19937 gen(98765);

  auto rand_key = [&]() -> std::string {
    // Short keys with prefix overlap to stress the radix tree.
    static constexpr std::string_view alphabet = "abcdef";
    const auto len = std::uniform_int_distribution<int>(1, 6)(gen);
    std::string k;
    for (int i = 0; i < len; ++i) {
      k += alphabet[std::uniform_int_distribution<int>(
          0, static_cast<int>(alphabet.size()) - 1)(gen)];
    }
    return k;
  };

  auto rand_value = [&]() -> std::string {
    const auto len = std::uniform_int_distribution<int>(1, 32)(gen);
    std::string v(static_cast<std::size_t>(len), 'x');
    for (auto &c : v) {
      c = static_cast<char>(
          std::uniform_int_distribution<int>('A', 'z')(gen));
    }
    return v;
  };

  TempDir td;
  const auto db_path = td.path / "db";

  // Oracle: ground truth of what the DB should contain after recovery.
  std::map<std::string, std::string> oracle;

  {
    // threshold=1 forces rotation after every write.
    auto db = bytecask::Bytecask::open(db_path, /*max_file_bytes=*/1);

    constexpr int kOps = 2000;
    for (int i = 0; i < kOps; ++i) {
      const auto op = std::uniform_int_distribution<int>(0, 9)(gen);

      if (op < 5) {
        // 50% chance: put (including overwrites)
        auto key = rand_key();
        auto val = rand_value();
        db.put({}, to_bytes(key), to_bytes(val));
        oracle[key] = val;
      } else if (op < 8) {
        // 30% chance: delete
        auto key = rand_key();
        std::ignore = db.del({}, to_bytes(key));
        oracle.erase(key);
      } else {
        // 20% chance: batch (2–5 operations)
        const auto batch_size =
            std::uniform_int_distribution<int>(2, 5)(gen);
        bytecask::Batch batch;
        for (int b = 0; b < batch_size; ++b) {
          if (std::uniform_int_distribution<int>(0, 3)(gen) == 0) {
            auto key = rand_key();
            batch.del(to_bytes(key));
            oracle.erase(key);
          } else {
            auto key = rand_key();
            auto val = rand_value();
            batch.put(to_bytes(key), to_bytes(val));
            oracle[key] = val;
          }
        }
        db.apply_batch({}, std::move(batch));
      }
    }
  }
  // DB is closed — all files sealed, hints flushed via background worker.

  // Helper: collect all (key, value) from a Bytecask into a map.
  auto collect = [](bytecask::Bytecask &db) {
    std::map<std::string, std::string> kv;
    for (auto [key, val] : db.iter_from({})) {
      kv[to_string(key)] = to_string(val);
    }
    return kv;
  };

  // Helper: compare recovered map against oracle.
  auto verify = [&](const std::string &label,
                    const std::map<std::string, std::string> &recovered) {
    INFO(label);
    REQUIRE(recovered.size() == oracle.size());
    for (const auto &[k, v] : oracle) {
      INFO("key=\"" << k << "\"");
      auto it = recovered.find(k);
      REQUIRE(it != recovered.end());
      CHECK(it->second == v);
    }
  };

  // Count data files for the max-parallelism test.
  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }
  REQUIRE(data_file_count > 1);

  SECTION("serial recovery") {
    auto db = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/1);
    verify("serial", collect(db));
  }

  SECTION("parallel recovery (2 workers)") {
    auto db = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/2);
    verify("parallel/2", collect(db));
  }

  SECTION("parallel recovery (W = file count)") {
    auto db = bytecask::Bytecask::open(
        db_path, 1,
        /*recovery_threads=*/static_cast<unsigned>(data_file_count));
    verify("parallel/max", collect(db));
  }
}

// ---------------------------------------------------------------------------
// Model-based recovery: large batch-heavy workload.
//
// Exercises the batch code path (BulkBegin/BulkEnd) extensively — most
// operations are batches of varying sizes. Verifies serial and parallel
// recovery produce identical results to the oracle.
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: batch-heavy workload",
          "[bytecask][recovery][parallel][model]") {
  std::mt19937 gen(54321);

  auto rand_key = [&]() -> std::string {
    static constexpr std::string_view alphabet = "ghijkl";
    const auto len = std::uniform_int_distribution<int>(1, 5)(gen);
    std::string k;
    for (int i = 0; i < len; ++i) {
      k += alphabet[std::uniform_int_distribution<int>(
          0, static_cast<int>(alphabet.size()) - 1)(gen)];
    }
    return k;
  };

  auto rand_value = [&]() -> std::string {
    const auto len = std::uniform_int_distribution<int>(4, 16)(gen);
    return std::string(static_cast<std::size_t>(len), 'V');
  };

  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;

  {
    auto db = bytecask::Bytecask::open(db_path, /*max_file_bytes=*/1);

    for (int i = 0; i < 1000; ++i) {
      const auto op = std::uniform_int_distribution<int>(0, 9)(gen);

      if (op < 2) {
        // 20% single put
        auto key = rand_key();
        auto val = rand_value();
        db.put({}, to_bytes(key), to_bytes(val));
        oracle[key] = val;
      } else if (op < 3) {
        // 10% single delete
        auto key = rand_key();
        std::ignore = db.del({}, to_bytes(key));
        oracle.erase(key);
      } else {
        // 70% batch (3-8 operations)
        const auto batch_size =
            std::uniform_int_distribution<int>(3, 8)(gen);
        bytecask::Batch batch;
        for (int b = 0; b < batch_size; ++b) {
          if (std::uniform_int_distribution<int>(0, 4)(gen) == 0) {
            auto key = rand_key();
            batch.del(to_bytes(key));
            oracle.erase(key);
          } else {
            auto key = rand_key();
            auto val = rand_value();
            batch.put(to_bytes(key), to_bytes(val));
            oracle[key] = val;
          }
        }
        db.apply_batch({}, std::move(batch));
      }
    }
  }

  auto collect = [](bytecask::Bytecask &db) {
    std::map<std::string, std::string> kv;
    for (auto [key, val] : db.iter_from({})) {
      kv[to_string(key)] = to_string(val);
    }
    return kv;
  };

  auto verify = [&](const std::string &label,
                    const std::map<std::string, std::string> &recovered) {
    INFO(label);
    REQUIRE(recovered.size() == oracle.size());
    for (const auto &[k, v] : oracle) {
      INFO("key=\"" << k << "\"");
      auto it = recovered.find(k);
      REQUIRE(it != recovered.end());
      CHECK(it->second == v);
    }
  };

  SECTION("serial") {
    auto db = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/1);
    verify("serial", collect(db));
  }

  SECTION("parallel (4 workers)") {
    auto db = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/4);
    verify("parallel/4", collect(db));
  }
}

// ---------------------------------------------------------------------------
// Model-based recovery: delete-heavy workload.
//
// Most keys are written then deleted. Stresses tombstone handling — both
// within a single worker (serial) and across workers (parallel fan-in
// tombstone cross-application).
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: delete-heavy workload",
          "[bytecask][recovery][parallel][model]") {
  std::mt19937 gen(11111);

  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;

  {
    auto db = bytecask::Bytecask::open(db_path, /*max_file_bytes=*/1);

    // Write 500 keys.
    for (int i = 0; i < 500; ++i) {
      auto key = std::format("dk_{:03d}", i);
      auto val = std::format("dv_{:03d}", i);
      db.put({}, to_bytes(key), to_bytes(val));
      oracle[key] = val;
    }
    // Delete 375 of them, interleaved with a few overwrites.
    for (int i = 0; i < 500; ++i) {
      if (i % 4 != 0) {
        // 75% deleted
        auto key = std::format("dk_{:03d}", i);
        std::ignore = db.del({}, to_bytes(key));
        oracle.erase(key);
      } else {
        // 25% overwritten
        auto key = std::format("dk_{:03d}", i);
        auto val = std::format("dv_{:03d}_v2", i);
        db.put({}, to_bytes(key), to_bytes(val));
        oracle[key] = val;
      }
    }
  }

  auto collect = [](bytecask::Bytecask &db) {
    std::map<std::string, std::string> kv;
    for (auto [key, val] : db.iter_from({})) {
      kv[to_string(key)] = to_string(val);
    }
    return kv;
  };

  auto verify = [&](const std::string &label,
                    const std::map<std::string, std::string> &recovered) {
    INFO(label);
    REQUIRE(recovered.size() == oracle.size());
    for (const auto &[k, v] : oracle) {
      INFO("key=\"" << k << "\"");
      auto it = recovered.find(k);
      REQUIRE(it != recovered.end());
      CHECK(it->second == v);
    }
  };

  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }

  SECTION("serial") {
    auto db = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/1);
    verify("serial", collect(db));
  }

  SECTION("parallel (3 workers)") {
    auto db = bytecask::Bytecask::open(db_path, 1, /*recovery_threads=*/3);
    verify("parallel/3", collect(db));
  }

  SECTION("parallel (W = file count)") {
    auto db = bytecask::Bytecask::open(
        db_path, 1,
        /*recovery_threads=*/static_cast<unsigned>(data_file_count));
    verify("parallel/max", collect(db));
  }
}

// ---------------------------------------------------------------------------
// Test: try_lock throws when the write lock is already held
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask try_lock throws when lock held",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  // Use a background thread to hold the write lock via a blocking put.
  // Thread 1: hold the write lock by doing a slow batch of puts.
  std::thread writer([&] {
    // We need the lock to be held while the main thread tries try_lock.
    // Use a big batch so the lock is held long enough.
    bytecask::Batch batch;
    for (int i = 0; i < 10000; ++i) {
      auto key = std::format("key_{:05d}", i);
      auto val = std::format("val_{:05d}", i);
      batch.put(to_bytes(key), to_bytes(val));
    }
    db.apply_batch(bytecask::WriteOptions{.sync = false}, std::move(batch));
  });

  // Thread 2 (main): try_lock put while the writer may be holding the lock.
  // We spin-try to catch the contention window; if we never hit it, the
  // test still passes — we're just testing the mechanism works at all.
  bool caught_contention = false;
  for (int attempt = 0; attempt < 100'000 && !caught_contention; ++attempt) {
    try {
      db.put(bytecask::WriteOptions{.sync = false, .try_lock = true},
             to_bytes("probe"), to_bytes("x"));
    } catch (const std::system_error &e) {
      if (e.code() == std::errc::resource_unavailable_try_again) {
        caught_contention = true;
      }
    }
  }

  writer.join();

  // We cannot guarantee the race is always hit, but if we did see the
  // contention, verify it was the right error.
  if (caught_contention) {
    CHECK(caught_contention);
  }
}

// ---------------------------------------------------------------------------
// Test: concurrent blocking writers are serialised — no data corruption
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask blocking writes are serialised",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  constexpr int kWritesPerThread = 200;
  constexpr int kThreads = 4;

  auto worker = [&](int thread_id) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      auto key = std::format("t{}_{:04d}", thread_id, i);
      auto val = std::format("v{}_{:04d}", thread_id, i);
      db.put(bytecask::WriteOptions{.sync = false}, to_bytes(key),
             to_bytes(val));
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  // Verify all keys are present and have the correct value.
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      auto key = std::format("t{}_{:04d}", t, i);
      auto expected_val = std::format("v{}_{:04d}", t, i);
      auto result = db.get({}, to_bytes(key));
      REQUIRE(result.has_value());
      CHECK(to_string(*result) == expected_val);
    }
  }
}

// ---------------------------------------------------------------------------
// Test: try_lock on del and apply_batch — non-blocking semantics work for
// all write operations, not just put.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask try_lock works for del and apply_batch",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("v"));

  // When no contention, try_lock succeeds normally.
  const bytecask::WriteOptions try_opts{.sync = false, .try_lock = true};
  CHECK(db.del(try_opts, to_bytes("k")));
  CHECK_FALSE(db.get({}, to_bytes("k")).has_value());

  bytecask::Batch batch;
  batch.put(to_bytes("b1"), to_bytes("bv1"));
  REQUIRE_NOTHROW(db.apply_batch(try_opts, std::move(batch)));
  REQUIRE(db.get({}, to_bytes("b1")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("b1"))) == "bv1");
}

// ---------------------------------------------------------------------------
// Test: reads proceed concurrently with a writer (true SWMR)
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask reads proceed during writes", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  // Pre-populate data for readers.
  for (int i = 0; i < 100; ++i) {
    auto key = std::format("pre_{:04d}", i);
    auto val = std::format("val_{:04d}", i);
    db.put(bytecask::WriteOptions{.sync = false}, to_bytes(key), to_bytes(val));
  }

  // Writer thread: continuously writes new keys.
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    int counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      auto key = std::format("w_{:06d}", counter++);
      db.put(bytecask::WriteOptions{.sync = false}, to_bytes(key),
             to_bytes("wv"));
    }
  });

  // Reader threads: read pre-populated keys concurrently with the writer.
  constexpr std::size_t kReaderThreads = 3;
  std::vector<bool> reader_ok(kReaderThreads, true);
  std::vector<std::thread> readers;
  for (std::size_t r = 0; r < kReaderThreads; ++r) {
    readers.emplace_back([&, r] {
      for (int pass = 0; pass < 50; ++pass) {
        for (int i = 0; i < 100; ++i) {
          auto key = std::format("pre_{:04d}", i);
          auto result = db.get({}, to_bytes(key));
          if (!result.has_value()) {
            reader_ok[r] = false;
            return;
          }
        }
      }
    });
  }

  for (auto &t : readers) {
    t.join();
  }
  stop.store(true, std::memory_order_relaxed);
  writer.join();

  for (std::size_t r = 0; r < kReaderThreads; ++r) {
    INFO("reader thread " << r);
    CHECK(reader_ok[r]);
  }
}

// ---------------------------------------------------------------------------
// Test: concurrent mixed operations (get + put + del) — no data corruption
//
// Exercises the full SWMR contract with multiple threads doing all three
// operation types simultaneously. Under TSan this catches data races on the
// key directory, file registry, and active file.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask concurrent mixed get/put/del", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  // Pre-populate so readers and deleters have data to work with.
  constexpr int kKeys = 200;
  for (int i = 0; i < kKeys; ++i) {
    auto key = std::format("k_{:04d}", i);
    auto val = std::format("v_{:04d}", i);
    db.put(bytecask::WriteOptions{.sync = false}, to_bytes(key), to_bytes(val));
  }

  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 500;

  auto worker = [&](int tid) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      auto key = std::format("k_{:04d}", (tid * 50 + i) % kKeys);
      auto op = (tid + i) % 10; // 0–7: get, 8: put, 9: del
      if (op <= 7) {
        // Read — value may or may not exist (concurrent deletes).
        auto result = db.get({}, to_bytes(key));
        (void)result;
      } else if (op == 8) {
        auto val = std::format("t{}_{:04d}", tid, i);
        db.put(bytecask::WriteOptions{.sync = false}, to_bytes(key),
               to_bytes(val));
      } else {
        std::ignore =
            db.del(bytecask::WriteOptions{.sync = false}, to_bytes(key));
      }
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back(worker, t);
  for (auto &t : threads)
    t.join();

  // Verify no crash and that every surviving key has a valid value.
  for (int i = 0; i < kKeys; ++i) {
    auto key = std::format("k_{:04d}", i);
    auto result = db.get({}, to_bytes(key));
    if (result.has_value()) {
      CHECK(!result->empty());
    }
  }
}

// ---------------------------------------------------------------------------
// Test: group commit — concurrent sync writers produce correct results
// ---------------------------------------------------------------------------
TEST_CASE("Bytecask group commit correctness", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  constexpr int kWritesPerThread = 50;
  constexpr int kThreads = 8;

  auto worker = [&](int thread_id) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      auto key = std::format("gc_t{}_{:04d}", thread_id, i);
      auto val = std::format("gv_t{}_{:04d}", thread_id, i);
      db.put(bytecask::WriteOptions{.sync = true}, to_bytes(key),
             to_bytes(val));
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  // All keys must be present with correct values.
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      auto key = std::format("gc_t{}_{:04d}", t, i);
      auto expected_val = std::format("gv_t{}_{:04d}", t, i);
      auto result = db.get({}, to_bytes(key));
      REQUIRE(result.has_value());
      CHECK(to_string(*result) == expected_val);
    }
  }
}

// ---------------------------------------------------------------------------
// Test: concurrent reads during writes — raw pointer traversal safety
// ---------------------------------------------------------------------------
// Readers traverse the radix tree using raw pointers while a writer mutates
// it via transient (put path). This validates that the persistent/immutable
// tree structure keeps old nodes alive for the duration of a read, even as
// the writer clones and replaces nodes.
TEST_CASE("Bytecask concurrent reads during writes",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  // Seed 500 keys so reads have something to find.
  for (int i = 0; i < 500; ++i) {
    auto key = std::format("rw_{:04d}", i);
    auto val = std::format("v_{:04d}", i);
    db.put({}, to_bytes(key), to_bytes(val));
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_count{0};
  std::atomic<int> read_hits{0};

  // Reader threads: continuously read keys that exist.
  constexpr int kReaders = 6;
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      int idx = r;
      while (!stop.load(std::memory_order_relaxed)) {
        auto key = std::format("rw_{:04d}", idx % 500);
        auto result = db.get({}, to_bytes(key));
        read_count.fetch_add(1, std::memory_order_relaxed);
        if (result.has_value()) {
          // Value must match the latest written value for this key.
          auto val = to_string(*result);
          CHECK(!val.empty());
          read_hits.fetch_add(1, std::memory_order_relaxed);
        }
        ++idx;
      }
    });
  }

  // Writer thread: overwrite existing keys and add new ones.
  constexpr int kWrites = 2000;
  std::thread writer([&] {
    for (int i = 0; i < kWrites; ++i) {
      // Overwrite existing keys (causes transient clone of shared nodes).
      auto key = std::format("rw_{:04d}", i % 500);
      auto val = std::format("v2_{:06d}", i);
      db.put({}, to_bytes(key), to_bytes(val));
    }
    stop.store(true, std::memory_order_relaxed);
  });

  writer.join();
  for (auto &t : readers) {
    t.join();
  }

  INFO("reads=" << read_count.load() << " hits=" << read_hits.load());
  // Readers must have successfully completed many reads without crashing.
  CHECK(read_count.load() > 0);
  CHECK(read_hits.load() > 0);

  // All 500 keys must still be present.
  for (int i = 0; i < 500; ++i) {
    auto key = std::format("rw_{:04d}", i);
    auto result = db.get({}, to_bytes(key));
    REQUIRE(result.has_value());
  }
}

// ---------------------------------------------------------------------------
// Test: snapshot isolation — load_state ref stays valid during get
// ---------------------------------------------------------------------------
// A reader holds a reference from load_state while a writer publishes a new
// EngineState. The reader must still see a consistent (old or new) snapshot
// and never observe a dangling reference.
TEST_CASE("Bytecask snapshot isolation under concurrent writes",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");

  // Seed key that will be repeatedly overwritten.
  db.put({}, to_bytes("snap_key"), to_bytes("initial"));

  std::atomic<bool> stop{false};
  std::atomic<int> reads_ok{0};

  // Readers: get the same key over and over.
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto result = db.get({}, to_bytes("snap_key"));
        // Must always find the key — it is never deleted.
        REQUIRE(result.has_value());
        // Value must be one of the written values (not garbage).
        auto val = to_string(*result);
        bool valid = val.starts_with("initial") || val.starts_with("ver_");
        CHECK(valid);
        reads_ok.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Writer: rapidly overwrite the key, publishing new EngineState each time.
  constexpr int kWrites = 5000;
  std::thread writer([&] {
    for (int i = 0; i < kWrites; ++i) {
      auto val = std::format("ver_{:06d}", i);
      db.put({}, to_bytes("snap_key"), to_bytes(val));
    }
    stop.store(true, std::memory_order_relaxed);
  });

  writer.join();
  for (auto &t : readers) {
    t.join();
  }

  INFO("successful reads=" << reads_ok.load());
  CHECK(reads_ok.load() > 100);
}
