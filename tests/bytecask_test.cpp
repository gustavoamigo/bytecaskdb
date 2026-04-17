// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — integration and model-based correctness tests

#include <algorithm>
#include <atomic>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
import bytecask;
import bytecask.data_entry;
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

// Convenience wrapper: reads key into a temporary buffer and returns it as
// optional. Used by tests that don't need to reuse the output buffer.
auto get_val(const bytecask::DB &db, bytecask::BytesView key)
    -> std::optional<bytecask::Bytes> {
  bytecask::Bytes out;
  if (!db.get({}, key, out)) return std::nullopt;
  return out;
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

// Flips a byte in the middle of path to invalidate its CRC checksum.
void corrupt_file_middle(const std::filesystem::path &path) {
  const auto size = std::filesystem::file_size(path);
  if (size == 0) return;
  const auto pos = size / 2;
  std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
  f.seekg(static_cast<std::streamoff>(pos));
  char c{};
  f.get(c);
  f.seekp(static_cast<std::streamoff>(pos));
  c ^= static_cast<char>(0xFF);
  f.put(c);
}

// Returns .hint files in dir sorted by name (ascending timestamp order).
auto list_hint_files(const std::filesystem::path &dir)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() == ".hint") {
      paths.push_back(e.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: open() creates the directory and does not throw
// ---------------------------------------------------------------------------
TEST_CASE("DB open creates directory", "[bytecask]") {
  TempDir td;
  const auto db_path = td.path / "db";
  REQUIRE_NOTHROW(bytecask::DB::open(db_path));
  CHECK(std::filesystem::is_directory(db_path));
}

// ---------------------------------------------------------------------------
// Test 2: put + get round-trip
// ---------------------------------------------------------------------------
TEST_CASE("DB put and get round-trip", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("key1"), to_bytes("value1"));

  const auto result = get_val(db, to_bytes("key1"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "value1");
}

// ---------------------------------------------------------------------------
// Test 2b: get output-param overload reuses buffer
// ---------------------------------------------------------------------------
TEST_CASE("DB get output-param round-trip", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
TEST_CASE("DB put overwrites existing key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("key1"), to_bytes("first"));
  db.put({}, to_bytes("key1"), to_bytes("second"));

  const auto result = get_val(db, to_bytes("key1"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "second");
}

// ---------------------------------------------------------------------------
// Test 4: del returns false for a key that does not exist
// ---------------------------------------------------------------------------
TEST_CASE("DB del returns false for absent key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  CHECK_FALSE(db.del({}, to_bytes("missing")));
}

// ---------------------------------------------------------------------------
// Test 5: del returns true; subsequent get returns nullopt
// ---------------------------------------------------------------------------
TEST_CASE("DB del existing key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("key1"), to_bytes("value1"));
  const bool removed = db.del({}, to_bytes("key1"));

  CHECK(removed);
  CHECK_FALSE(get_val(db, to_bytes("key1")).has_value());
}

// ---------------------------------------------------------------------------
// Test 6: contains_key tracks puts and dels
// ---------------------------------------------------------------------------
TEST_CASE("DB contains_key tracks mutations", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  CHECK_FALSE(db.contains_key(to_bytes("k")));
  db.put({}, to_bytes("k"), to_bytes("v"));
  CHECK(db.contains_key(to_bytes("k")));
  CHECK(db.del({}, to_bytes("k")));
  CHECK_FALSE(db.contains_key(to_bytes("k")));
}

// ---------------------------------------------------------------------------
// Test 7: apply_batch — mixed puts and del, all visible atomically
// ---------------------------------------------------------------------------
TEST_CASE("DB apply_batch mixed operations", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Pre-insert a key that the batch will remove.
  db.put({}, to_bytes("del"), to_bytes("gone"));

  bytecask::Batch batch;
  batch.put(to_bytes("a"), to_bytes("alpha"));
  batch.put(to_bytes("b"), to_bytes("beta"));
  batch.del(to_bytes("del"));
  db.apply_batch({}, std::move(batch));

  REQUIRE(get_val(db, to_bytes("a")).has_value());
  CHECK(to_string(*get_val(db, to_bytes("a"))) == "alpha");
  REQUIRE(get_val(db, to_bytes("b")).has_value());
  CHECK(to_string(*get_val(db, to_bytes("b"))) == "beta");
  CHECK_FALSE(get_val(db, to_bytes("del")).has_value());
}

// ---------------------------------------------------------------------------
// Test 8: iter_from with ordered=true returns all entries in ascending key
// order
// ---------------------------------------------------------------------------
TEST_CASE("DB iter_from returns entries in ascending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
TEST_CASE("DB iter_from starts from given key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
TEST_CASE("DB keys_from returns all keys in ascending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
TEST_CASE("DB rotation creates new data file", "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";
  // A threshold of 1 means any write will trigger rotation.
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

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
TEST_CASE("DB get resolves value from rotated file",
          "[bytecask][rotation]") {
  TempDir td;
  // Threshold of 1 triggers rotation after each write.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  db.put({}, to_bytes("key_a"), to_bytes("alpha"));
  // After put, active file is now rotated. key_a lives in the sealed file.
  db.put({}, to_bytes("key_b"), to_bytes("beta"));

  const auto a = get_val(db, to_bytes("key_a"));
  REQUIRE(a.has_value());
  CHECK(to_string(*a) == "alpha");

  const auto b = get_val(db, to_bytes("key_b"));
  REQUIRE(b.has_value());
  CHECK(to_string(*b) == "beta");
}

// ---------------------------------------------------------------------------
// Test 13: iter_from spans entries across multiple data files
// ---------------------------------------------------------------------------
TEST_CASE("DB iter_from spans multiple rotated files",
          "[bytecask][rotation]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

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
// Test 14: close (destructor) writes hint files for sealed data files
// ---------------------------------------------------------------------------
TEST_CASE("DB close writes hint file for sealed file",
          "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("k"), to_bytes("v"));
    // db destroyed here — background worker drains, hints written
  }

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
// ---------------------------------------------------------------------------
// Test 15: ~DB calls flush_hints — hint file exists after scope exit
// ---------------------------------------------------------------------------
TEST_CASE("DB destructor flushes hint files", "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
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
TEST_CASE("DB WriteOptions sync=false data still readable",
          "[bytecask][write_options]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  const bytecask::WriteOptions no_sync{.sync = false};
  db.put(no_sync, to_bytes("k1"), to_bytes("v1"));
  db.put(no_sync, to_bytes("k2"), to_bytes("v2"));

  const auto r1 = get_val(db, to_bytes("k1"));
  REQUIRE(r1.has_value());
  CHECK(to_string(*r1) == "v1");

  const auto r2 = get_val(db, to_bytes("k2"));
  REQUIRE(r2.has_value());
  CHECK(to_string(*r2) == "v2");
}

// ---------------------------------------------------------------------------
// Test 18: WriteOptions{.sync=false} on del — key is removed, no fdatasync.
// ---------------------------------------------------------------------------
TEST_CASE("DB WriteOptions sync=false del still removes key",
          "[bytecask][write_options]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("v"));

  const bytecask::WriteOptions no_sync{.sync = false};
  const bool removed = db.del(no_sync, to_bytes("k"));

  CHECK(removed);
  CHECK_FALSE(get_val(db, to_bytes("k")).has_value());
}

// ---------------------------------------------------------------------------
// Test 19: WriteOptions{.sync=false} on apply_batch — results visible.
// ---------------------------------------------------------------------------
TEST_CASE("DB WriteOptions sync=false apply_batch results visible",
          "[bytecask][write_options]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  const bytecask::WriteOptions no_sync{.sync = false};
  bytecask::Batch batch;
  batch.put(to_bytes("x"), to_bytes("xv"));
  batch.put(to_bytes("y"), to_bytes("yv"));
  db.apply_batch(no_sync, std::move(batch));

  REQUIRE(get_val(db, to_bytes("x")).has_value());
  CHECK(to_string(*get_val(db, to_bytes("x"))) == "xv");
  REQUIRE(get_val(db, to_bytes("y")).has_value());
  CHECK(to_string(*get_val(db, to_bytes("y"))) == "yv");
}

// ---------------------------------------------------------------------------
// Test 20: puts survive a restart (raw scan recovery, no hint files)
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: puts survive restart", "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);
    db.put({}, to_bytes("k1"), to_bytes("v1"));
    db.put({}, to_bytes("k2"), to_bytes("v2"));
  } // destructor syncs; no rotation so no hint files written

  auto db2 = bytecask::DB::open(db_path);
  REQUIRE(get_val(db2, to_bytes("k1")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("k1"))) == "v1");
  REQUIRE(get_val(db2, to_bytes("k2")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("k2"))) == "v2");
}

// ---------------------------------------------------------------------------
// Test 21: delete tombstone survives restart — key absent after reopen
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: tombstone survives restart",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);
    db.put({}, to_bytes("k"), to_bytes("v"));
    std::ignore = db.del({}, to_bytes("k"));
  }

  auto db2 = bytecask::DB::open(db_path);
  CHECK_FALSE(get_val(db2, to_bytes("k")).has_value());
}

// ---------------------------------------------------------------------------
// Test 22: last write wins — overwritten value correct after restart
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: last write wins after overwrite",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);
    db.put({}, to_bytes("k"), to_bytes("first"));
    db.put({}, to_bytes("k"), to_bytes("second"));
  }

  auto db2 = bytecask::DB::open(db_path);
  REQUIRE(get_val(db2, to_bytes("k")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("k"))) == "second");
}

// ---------------------------------------------------------------------------
// Test 23: batch survives restart — all puts/dels from batch visible
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: batch survives restart", "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);
    db.put({}, to_bytes("preexisting"), to_bytes("gone"));

    bytecask::Batch batch;
    batch.put(to_bytes("a"), to_bytes("alpha"));
    batch.put(to_bytes("b"), to_bytes("beta"));
    batch.del(to_bytes("preexisting"));
    db.apply_batch({}, std::move(batch));
  }

  auto db2 = bytecask::DB::open(db_path);
  REQUIRE(get_val(db2, to_bytes("a")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("a"))) == "alpha");
  REQUIRE(get_val(db2, to_bytes("b")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("b"))) == "beta");
  CHECK_FALSE(get_val(db2, to_bytes("preexisting")).has_value());
}

// ---------------------------------------------------------------------------
// Test 24: recovery via hint files — rotation writes hints on close,
//           reopen rebuilds key directory from them
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: hint file path after rotation",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // threshold=1 forces rotation after each write; destructor writes hint files
  // for the sealed files.
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
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

  auto db2 = bytecask::DB::open(db_path, {.max_file_bytes = 1});
  REQUIRE(get_val(db2, to_bytes("x")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("x"))) == "xval");
  REQUIRE(get_val(db2, to_bytes("y")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("y"))) == "yval");
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
TEST_CASE("DB recovery: cross-file tombstone suppresses stale put",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    // threshold=1 forces each write into its own file.
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("gone"), to_bytes("v1")); // file 0
    (void)db.del({}, to_bytes("gone")); // file 1 — Delete seq > Put seq
    db.put({}, to_bytes("keep"), to_bytes("v2")); // file 2
  }

  auto db2 = bytecask::DB::open(db_path, {.max_file_bytes = 1});
  CHECK_FALSE(db2.contains_key(to_bytes("gone")));
  CHECK_FALSE(get_val(db2, to_bytes("gone")).has_value());
  REQUIRE(get_val(db2, to_bytes("keep")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("keep"))) == "v2");
}

// ---------------------------------------------------------------------------
// Test: incomplete batch entries are discarded during recovery.
// Simulates a crash mid-batch by writing a BulkBegin + Put entries with no
// BulkEnd directly to a data file. Recovery generates a hint file from the
// raw data (batch-aware) and only the standalone entries survive.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: incomplete batch is discarded",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  std::filesystem::create_directories(db_path);

  {
    // Manually write a data file simulating a crash mid-batch.
    bytecask::DataFile df(db_path / "data_00000000000000000000000001.data");
    // Standalone entry — should survive.
    std::ignore = df.append_entry(1, bytecask::EntryType::Put, to_bytes("good"),
                            to_bytes("value1"));
    // Begin batch, write some entries, but never write BulkEnd.
    std::ignore = df.append_entry(2, bytecask::EntryType::BulkBegin, {}, {});
    std::ignore = df.append_entry(3, bytecask::EntryType::Put, to_bytes("orphan_a"),
                            to_bytes("lost1"));
    std::ignore = df.append_entry(4, bytecask::EntryType::Put, to_bytes("orphan_b"),
                            to_bytes("lost2"));
    // No BulkEnd — simulates crash.
    df.sync();
  }

  // Open engine — should generate hint file and recover only "good".
  auto db = bytecask::DB::open(db_path);
  REQUIRE(get_val(db, to_bytes("good")).has_value());
  CHECK(to_string(*get_val(db, to_bytes("good"))) == "value1");
  CHECK_FALSE(get_val(db, to_bytes("orphan_a")).has_value());
  CHECK_FALSE(get_val(db, to_bytes("orphan_b")).has_value());

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
TEST_CASE("DB recovery: order-independent tombstone",
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
      std::ignore = df.append_entry(1, bytecask::EntryType::Put, to_bytes("gone"),
                              to_bytes("v1"));
      std::ignore = df.append_entry(2, bytecask::EntryType::Put, to_bytes("alive"),
                              to_bytes("v2"));
      df.sync();
    }

    // File with a Delete for "gone" (seq=3) — higher LSN wins.
    {
      bytecask::DataFile df(db_path / std::format("{}.data", del_stem));
      std::ignore = df.append_entry(3, bytecask::EntryType::Delete,
                              to_bytes("gone"), {});
      df.sync();
    }

    auto db = bytecask::DB::open(db_path);
    CHECK_FALSE(get_val(db, to_bytes("gone")).has_value());
    REQUIRE(get_val(db, to_bytes("alive")).has_value());
    CHECK(to_string(*get_val(db, to_bytes("alive"))) == "v2");
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
TEST_CASE("DB parallel recovery: puts survive restart",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // threshold=1 forces each write into its own file, giving multiple files
  // for parallel workers to split across.
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("1"));
    db.put({}, to_bytes("b"), to_bytes("2"));
    db.put({}, to_bytes("c"), to_bytes("3"));
    db.put({}, to_bytes("d"), to_bytes("4"));
  }

  auto db2 = bytecask::DB::open(db_path, {.max_file_bytes = 1, .recovery_threads = 4});
  REQUIRE(get_val(db2, to_bytes("a")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("a"))) == "1");
  REQUIRE(get_val(db2, to_bytes("b")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("b"))) == "2");
  REQUIRE(get_val(db2, to_bytes("c")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("c"))) == "3");
  REQUIRE(get_val(db2, to_bytes("d")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("d"))) == "4");
}

// ---------------------------------------------------------------------------
// Parallel recovery: cross-worker tombstone suppresses stale put
// ---------------------------------------------------------------------------
TEST_CASE("DB parallel recovery: cross-worker tombstone",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("gone"), to_bytes("v1"));   // file 0
    std::ignore = db.del({}, to_bytes("gone"));      // file 1
    db.put({}, to_bytes("keep"), to_bytes("v2"));    // file 2
    db.put({}, to_bytes("also"), to_bytes("v3"));    // file 3
  }

  // 4 files, 4 workers — PUT and DELETE for "gone" land in different workers.
  auto db2 = bytecask::DB::open(db_path, {.max_file_bytes = 1, .recovery_threads = 4});
  CHECK_FALSE(get_val(db2, to_bytes("gone")).has_value());
  REQUIRE(get_val(db2, to_bytes("keep")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("keep"))) == "v2");
  REQUIRE(get_val(db2, to_bytes("also")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("also"))) == "v3");
}

// ---------------------------------------------------------------------------
// Parallel recovery: last write wins across workers
// ---------------------------------------------------------------------------
TEST_CASE("DB parallel recovery: last write wins",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("k"), to_bytes("old"));
    db.put({}, to_bytes("k"), to_bytes("new"));
  }

  auto db2 = bytecask::DB::open(db_path, {.max_file_bytes = 1, .recovery_threads = 2});
  REQUIRE(get_val(db2, to_bytes("k")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("k"))) == "new");
}

// ---------------------------------------------------------------------------
// Parallel recovery: produces identical result to serial recovery
//
// Uses a larger dataset with overwrites and deletes to exercise the full
// merge + tombstone cross-application path. Opens the same data dir twice:
// once with 1 thread (serial), once with 4 threads (parallel), then
// compares every key.
// ---------------------------------------------------------------------------
TEST_CASE("DB parallel recovery: matches serial result",
          "[bytecask][recovery][parallel]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // Build a database with many files, overwrites, and deletes.
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
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
  std::map<std::string, std::string> serial_kv;
  {
    auto serial = bytecask::DB::open(db_path, {.max_file_bytes = 1, .recovery_threads = 1});
    for (auto [key, val] : serial.iter_from({})) {
      serial_kv[to_string(key)] = to_string(val);
    }
  }

  // Recover in parallel.
  auto parallel = bytecask::DB::open(db_path, {.max_file_bytes = 1, .recovery_threads = 4});

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
// BC-157: fail_recovery_on_crc_errors — strict mode (default)
//
// Corrupt one hint file; strict recovery (fail_recovery_on_crc_errors=true)
// must throw std::runtime_error from DB::open.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: strict mode throws on corrupt hint file",
          "[bytecask][recovery][recovery_strict]") {
  TempDir td;
  const auto db_path = td.path / "db";

  // Three writes with max_file_bytes=1 → three sealed data files, three hint files.
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("key_first"),  to_bytes("v1"));
    db.put({}, to_bytes("key_second"), to_bytes("v2"));
    db.put({}, to_bytes("key_third"),  to_bytes("v3"));
  }

  // Corrupt the second hint file (middle of three by timestamp sort).
  const auto hints = list_hint_files(db_path);
  REQUIRE(hints.size() >= 2);
  corrupt_file_middle(hints[1]);

  // Default options → fail_recovery_on_crc_errors=true → must throw.
  REQUIRE_THROWS_AS(bytecask::DB::open(db_path), std::runtime_error);
}

// ---------------------------------------------------------------------------
// BC-157: fail_recovery_on_crc_errors — lenient mode
//
// Same corruption; lenient recovery must open successfully.
// Keys from the corrupt file are absent; keys from clean files are present.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: lenient mode opens with partial recovery on corrupt hint",
          "[bytecask][recovery][recovery_lenient]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("key_first"),  to_bytes("v1"));
    db.put({}, to_bytes("key_second"), to_bytes("v2"));
    db.put({}, to_bytes("key_third"),  to_bytes("v3"));
  }

  const auto hints = list_hint_files(db_path);
  REQUIRE(hints.size() >= 2);
  corrupt_file_middle(hints[1]);

  // Lenient recovery must succeed even with a corrupt hint file.
  bytecask::DB db =
      bytecask::DB::open(db_path, {.max_file_bytes = 1,
                                   .fail_recovery_on_crc_errors = false});
  CHECK_FALSE(db.is_degraded());
  CHECK(get_val(db, to_bytes("key_first")).has_value());
  CHECK(get_val(db, to_bytes("key_third")).has_value());
  // Key from the corrupt file must be absent.
  CHECK_FALSE(get_val(db, to_bytes("key_second")).has_value());
}

// ---------------------------------------------------------------------------
// BC-157: parallel recovery terminate bug fix
//
// Before the fix, an exception escaping a jthread lambda called std::terminate.
// This test verifies that a corrupt hint file with recovery_threads>1 and
// fail_recovery_on_crc_errors=true raises std::runtime_error — not std::terminate.
// ---------------------------------------------------------------------------
TEST_CASE("DB parallel recovery: corrupt hint throws instead of terminating",
          "[bytecask][recovery][recovery_parallel_terminate_fix]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("k1"), to_bytes("v1"));
    db.put({}, to_bytes("k2"), to_bytes("v2"));
    db.put({}, to_bytes("k3"), to_bytes("v3"));
    db.put({}, to_bytes("k4"), to_bytes("v4"));
  }

  const auto hints = list_hint_files(db_path);
  REQUIRE(hints.size() >= 2);
  corrupt_file_middle(hints[1]);

  // recovery_threads>1 + strict mode → must throw, not terminate.
  REQUIRE_THROWS_AS(
      bytecask::DB::open(db_path,
                         {.max_file_bytes = 1,
                          .recovery_threads = 4,
                          .fail_recovery_on_crc_errors = true}),
      std::runtime_error);
}

// ---------------------------------------------------------------------------
// Model-based recovery: random workload with oracle comparison.
//
// A random sequence of puts, deletes, overwrites, and batches is applied to
// both a DB instance and a std::map oracle. The DB uses a tiny rotation
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
      k += alphabet[static_cast<std::size_t>(std::uniform_int_distribution<int>(
          0, static_cast<int>(alphabet.size()) - 1)(gen))];
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
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

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

  // Helper: collect all (key, value) from a DB into a map.
  auto collect = [](bytecask::DB &db) {
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

  // Helper: collect per-file stats as a sorted vector for comparison.
  // File IDs may differ, but the multiset of (live_bytes, total_bytes)
  // must match between serial and parallel.
  auto collect_stats = [](bytecask::DB &db) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes);
    }
    std::ranges::sort(vals);
    return vals;
  };

  // Count data files for the max-parallelism test.
  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }
  REQUIRE(data_file_count > 1);

  // Collect serial file_stats as baseline for parallel comparison.
  // Must use a separate copy since opening mutates the directory.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_stats(db);
  }

  SECTION("serial recovery") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (2 workers)") {
    const auto p = td.path / "p2";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 2});
    verify("parallel/2", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (W = file count)") {
    const auto p = td.path / "pmax";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(
        p, {.max_file_bytes = 1,
            .recovery_threads = static_cast<unsigned>(data_file_count)});
    verify("parallel/max", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
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
      k += alphabet[static_cast<std::size_t>(std::uniform_int_distribution<int>(
          0, static_cast<int>(alphabet.size()) - 1)(gen))];
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
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

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

  auto collect = [](bytecask::DB &db) {
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

  auto collect_stats = [](bytecask::DB &db) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes);
    }
    std::ranges::sort(vals);
    return vals;
  };

  std::vector<std::pair<std::uint64_t, std::uint64_t>> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_stats(db);
  }

  SECTION("serial") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }

  SECTION("parallel (4 workers)") {
    const auto p = td.path / "p4";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 4});
    verify("parallel/4", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
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
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

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

  auto collect = [](bytecask::DB &db) {
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

  auto collect_stats = [](bytecask::DB &db) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes);
    }
    std::ranges::sort(vals);
    return vals;
  };

  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }

  std::vector<std::pair<std::uint64_t, std::uint64_t>> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_stats(db);
  }

  SECTION("serial") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }

  SECTION("parallel (3 workers)") {
    const auto p = td.path / "p3";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 3});
    verify("parallel/3", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }

  SECTION("parallel (W = file count)") {
    const auto p = td.path / "pmax";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(
        p, {.max_file_bytes = 1,
            .recovery_threads = static_cast<unsigned>(data_file_count)});
    verify("parallel/max", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }
}

// ---------------------------------------------------------------------------
// Test: concurrent blocking writers are serialised — no data corruption
// ---------------------------------------------------------------------------
TEST_CASE("DB blocking writes are serialised",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
      auto result = get_val(db, to_bytes(key));
      REQUIRE(result.has_value());
      CHECK(to_string(*result) == expected_val);
    }
  }
}

// ---------------------------------------------------------------------------
// Test: reads proceed concurrently with a writer (true SWMR)
// ---------------------------------------------------------------------------
TEST_CASE("DB reads proceed during writes", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
          auto result = get_val(db, to_bytes(key));
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
TEST_CASE("DB concurrent mixed get/put/del", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
        auto result = get_val(db, to_bytes(key));
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
    auto result = get_val(db, to_bytes(key));
    if (result.has_value()) {
      CHECK(!result->empty());
    }
  }
}

// ---------------------------------------------------------------------------
// Test: group commit — concurrent sync writers produce correct results
// ---------------------------------------------------------------------------
TEST_CASE("DB group commit correctness", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
      auto result = get_val(db, to_bytes(key));
      REQUIRE(result.has_value());
      CHECK(to_string(*result) == expected_val);
    }
  }
}

TEST_CASE("DB group commit concurrent put+del", "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  constexpr int kKeys = 100;
  for (int i = 0; i < kKeys; ++i) {
    db.put(bytecask::WriteOptions{.sync = false},
           to_bytes(std::format("gd_{:04d}", i)),
           to_bytes(std::format("val_{:04d}", i)));
  }

  constexpr int kThreads = 4;
  constexpr int kOps = 50;

  auto worker = [&](int tid) {
    for (int i = 0; i < kOps; ++i) {
      auto key = std::format("gd_{:04d}", (tid * 25 + i) % kKeys);
      if (i % 2 == 0) {
        db.put(bytecask::WriteOptions{.sync = true}, to_bytes(key),
               to_bytes(std::format("new_t{}_{}", tid, i)));
      } else {
        std::ignore = db.del(bytecask::WriteOptions{.sync = true},
                             to_bytes(key));
      }
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
  for (auto &t : threads) t.join();

  for (int i = 0; i < kKeys; ++i) {
    auto key = std::format("gd_{:04d}", i);
    auto result = get_val(db, to_bytes(key));
    if (result.has_value()) {
      CHECK(!result->empty());
    }
  }
}

TEST_CASE("DB group commit solo fallback for large batches",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  std::string big_value(300 * 1024, 'X');
  db.put(bytecask::WriteOptions{.sync = true}, to_bytes("big_key"),
         to_bytes(big_value));

  auto result = get_val(db, to_bytes("big_key"));
  REQUIRE(result.has_value());
  CHECK(result->size() == big_value.size());
}

TEST_CASE("DB group commit with opts.solo bypasses group",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  constexpr int kThreads = 4;
  constexpr int kOps = 50;

  auto worker = [&](int tid) {
    for (int i = 0; i < kOps; ++i) {
      auto key = std::format("solo_t{}_{:04d}", tid, i);
      auto val = std::format("sv_t{}_{:04d}", tid, i);
      db.put(bytecask::WriteOptions{.sync = true, .solo = true},
             to_bytes(key), to_bytes(val));
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
  for (auto &t : threads) t.join();

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOps; ++i) {
      auto key = std::format("solo_t{}_{:04d}", t, i);
      auto expected = std::format("sv_t{}_{:04d}", t, i);
      auto result = get_val(db, to_bytes(key));
      REQUIRE(result.has_value());
      CHECK(to_string(*result) == expected);
    }
  }
}

TEST_CASE("DB group commit apply_batch_if conflict returns false",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put(bytecask::WriteOptions{.sync = false}, to_bytes("ck"),
         to_bytes("v1"));

  auto snap = db.snapshot();

  db.put(bytecask::WriteOptions{.sync = false}, to_bytes("ck"),
         to_bytes("v2"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("ck"));
  plan.put(to_bytes("ck"), to_bytes("v3"));

  CHECK_FALSE(db.apply_batch_if({}, std::move(plan)));

  auto result = get_val(db, to_bytes("ck"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "v2");
}

TEST_CASE("DB group commit recovery preserves all keys",
          "[bytecask][concurrency]") {
  TempDir td;
  auto dir = td.path / "db";

  constexpr int kThreads = 4;
  constexpr int kOps = 50;

  {
    auto db = bytecask::DB::open(dir);
    auto worker = [&](int tid) {
      for (int i = 0; i < kOps; ++i) {
        auto key = std::format("rc_t{}_{:04d}", tid, i);
        auto val = std::format("rv_t{}_{:04d}", tid, i);
        db.put(bytecask::WriteOptions{.sync = true}, to_bytes(key),
               to_bytes(val));
      }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto &t : threads) t.join();
  }

  auto db = bytecask::DB::open(dir);
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOps; ++i) {
      auto key = std::format("rc_t{}_{:04d}", t, i);
      auto expected = std::format("rv_t{}_{:04d}", t, i);
      auto result = get_val(db, to_bytes(key));
      REQUIRE(result.has_value());
      CHECK(to_string(*result) == expected);
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
TEST_CASE("DB concurrent reads during writes",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
        auto result = get_val(db, to_bytes(key));
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
    auto result = get_val(db, to_bytes(key));
    REQUIRE(result.has_value());
  }
}

// ---------------------------------------------------------------------------
// Test: snapshot isolation — load_state ref stays valid during get
// ---------------------------------------------------------------------------
// A reader holds a reference from load_state while a writer publishes a new
// EngineState. The reader must still see a consistent (old or new) snapshot
// and never observe a dangling reference.
TEST_CASE("DB snapshot isolation under concurrent writes",
          "[bytecask][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

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
        auto result = get_val(db, to_bytes("snap_key"));
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

// ===========================================================================
// FileStats tracking tests
// ===========================================================================

// Helper: compute on-disk entry size from key and value string views.
static auto esize(std::string_view key, std::string_view value)
    -> std::uint64_t {
  return bytecask::kHeaderSize + key.size() + value.size() +
         bytecask::kCrcSize;
}

// Tombstone (Delete) entry has value_size = 0.
static auto tombstone_size(std::string_view key) -> std::uint64_t {
  return bytecask::kHeaderSize + key.size() + bytecask::kCrcSize;
}

// ---------------------------------------------------------------------------
// put accounts for live_bytes and total_bytes on the active file
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: put tracks live_bytes and total_bytes",
          "[bytecask][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("k1"), to_bytes("v1"));
  db.put({}, to_bytes("k2"), to_bytes("val2"));

  auto stats = db.file_stats();
  // Only one file (active).
  REQUIRE(stats.size() == 1);
  const auto &[fid, fs] = *stats.begin();

  const auto expected = esize("k1", "v1") + esize("k2", "val2");
  CHECK(fs.live_bytes == expected);
  CHECK(fs.total_bytes == expected);
}

// ---------------------------------------------------------------------------
// overwrite decrements old file's live_bytes, increments new entry
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: overwrite decrements old live_bytes",
          "[bytecask][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("old_value"));
  db.put({}, to_bytes("k"), to_bytes("new_value"));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &fs = stats.begin()->second;

  // Only the new entry is live; both entries exist on disk.
  CHECK(fs.live_bytes == esize("k", "new_value"));
  CHECK(fs.total_bytes == esize("k", "old_value") + esize("k", "new_value"));
}

// ---------------------------------------------------------------------------
// del decrements live_bytes and adds tombstone to total_bytes only
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: del decrements live_bytes, tombstone in total_bytes",
          "[bytecask][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("value"));
  (void)db.del({}, to_bytes("k"));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &fs = stats.begin()->second;

  CHECK(fs.live_bytes == 0);
  CHECK(fs.total_bytes == esize("k", "value") + tombstone_size("k"));
}

// ---------------------------------------------------------------------------
// apply_batch tracks stats including BulkBegin/BulkEnd markers
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: apply_batch tracks stats with bulk markers",
          "[bytecask][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  bytecask::Batch batch;
  batch.put(to_bytes("a"), to_bytes("va"));
  batch.put(to_bytes("b"), to_bytes("vb"));
  db.apply_batch({}, std::move(batch));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &fs = stats.begin()->second;

  const auto bulk_marker_size = bytecask::kHeaderSize + bytecask::kCrcSize;
  const auto expected_total = bulk_marker_size * 2 + // BulkBegin + BulkEnd
                              esize("a", "va") + esize("b", "vb");
  CHECK(fs.live_bytes == esize("a", "va") + esize("b", "vb"));
  CHECK(fs.total_bytes == expected_total);
}

// ---------------------------------------------------------------------------
// batch with delete inside decrements old entry's live_bytes
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: batch del decrements live_bytes",
          "[bytecask][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("val"));

  bytecask::Batch batch;
  batch.del(to_bytes("k"));
  db.apply_batch({}, std::move(batch));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &fs = stats.begin()->second;

  CHECK(fs.live_bytes == 0);
}

// ---------------------------------------------------------------------------
// rotation: overwrite across files decrements old file's live_bytes
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: cross-file overwrite decrements old file",
          "[bytecask][filestats]") {
  TempDir td;
  // Threshold of 1 triggers rotation after each write.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  db.put({}, to_bytes("k"), to_bytes("old_value"));
  // After this put, file rotated. Now write to new active file.
  db.put({}, to_bytes("k"), to_bytes("new_value"));

  auto stats = db.file_stats();
  // Should have 3 files: sealed file 0, sealed file 1, active file 2.
  REQUIRE(stats.size() >= 2);

  // Sum live_bytes across all files — should equal the one live entry.
  std::uint64_t total_live = 0;
  for (const auto &[fid, fs] : stats) {
    total_live += fs.live_bytes;
  }
  CHECK(total_live == esize("k", "new_value"));
}

// ---------------------------------------------------------------------------
// fragmentation: file with 50% dead entries
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: fragmentation computation", "[bytecask][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Write two entries of equal size, then overwrite the first.
  db.put({}, to_bytes("k1"), to_bytes("value"));
  db.put({}, to_bytes("k2"), to_bytes("value"));
  db.put({}, to_bytes("k1"), to_bytes("value")); // overwrites k1

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &fs = stats.begin()->second;

  // 3 entries on disk, 2 live.
  const auto one_entry = esize("k1", "value");
  CHECK(fs.live_bytes == 2 * one_entry);
  CHECK(fs.total_bytes == 3 * one_entry);

  // fragmentation = 1 - live/total = 1/3 ≈ 0.333
  auto frag = 1.0 - static_cast<double>(fs.live_bytes) /
                         static_cast<double>(fs.total_bytes);
  CHECK(frag > 0.33);
  CHECK(frag < 0.34);
}

// ---------------------------------------------------------------------------
// Recovery reconstructs correct file_stats
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: recovery reconstructs stats", "[bytecask][filestats]") {
  TempDir td;
  const auto db_path = td.path / "db";

  std::map<std::uint32_t, bytecask::FileStats> pre_stats;
  {
    // Threshold of 1 triggers rotation after every write.
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("v1"));
    db.put({}, to_bytes("b"), to_bytes("v2"));
    db.put({}, to_bytes("a"), to_bytes("v3")); // overwrite across files
    pre_stats = db.file_stats();
  }

  // Reopen — recovery from hint files.
  auto db2 = bytecask::DB::open(db_path, {.max_file_bytes = 1});
  auto post_stats = db2.file_stats();

  // Verify live_bytes and total_bytes per sealed file match.
  // The active file from pre_stats won't match exactly (it was sealed on close
  // and a new empty active file was created on reopen), so compare sealed files.
  // We check that the sum of live_bytes matches.
  std::uint64_t pre_live = 0, post_live = 0;
  for (const auto &[fid, fs] : pre_stats) {
    pre_live += fs.live_bytes;
  }
  for (const auto &[fid, fs] : post_stats) {
    post_live += fs.live_bytes;
  }
  CHECK(pre_live == post_live);

  // Check total_bytes of non-empty files match (sealed files preserved).
  std::uint64_t pre_total = 0, post_total = 0;
  for (const auto &[fid, fs] : pre_stats) {
    if (fs.total_bytes > 0) pre_total += fs.total_bytes;
  }
  for (const auto &[fid, fs] : post_stats) {
    if (fs.total_bytes > 0) post_total += fs.total_bytes;
  }
  CHECK(pre_total == post_total);
}

// ---------------------------------------------------------------------------
// Parallel recovery stats match serial recovery stats
// ---------------------------------------------------------------------------
TEST_CASE("FileStats: parallel recovery matches serial",
          "[bytecask][filestats]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    for (int i = 0; i < 50; ++i) {
      auto k = std::format("key{:04d}", i);
      auto v = std::format("val{:04d}", i);
      db.put({}, to_bytes(k), to_bytes(v));
    }
    // Overwrite some keys across files.
    for (int i = 0; i < 25; ++i) {
      auto k = std::format("key{:04d}", i);
      auto v = std::format("new{:04d}", i);
      db.put({}, to_bytes(k), to_bytes(v));
    }
    // Delete some.
    for (int i = 40; i < 50; ++i) {
      auto k = std::format("key{:04d}", i);
      (void)db.del({}, to_bytes(k));
    }
  }

  // Copy the db directory to two separate locations for isolated recovery.
  const auto serial_path = td.path / "serial";
  const auto parallel_path = td.path / "parallel";
  std::filesystem::copy(db_path, serial_path,
                        std::filesystem::copy_options::recursive);
  std::filesystem::copy(db_path, parallel_path,
                        std::filesystem::copy_options::recursive);

  // Serial recovery.
  auto db_serial = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
  auto serial_stats = db_serial.file_stats();

  // Parallel recovery (4 threads).
  auto db_parallel = bytecask::DB::open(parallel_path, {.max_file_bytes = 1, .recovery_threads = 4});
  auto parallel_stats = db_parallel.file_stats();

  // Sum of live_bytes must match.
  std::uint64_t serial_live = 0, parallel_live = 0;
  for (const auto &[fid, fs] : serial_stats) {
    serial_live += fs.live_bytes;
  }
  for (const auto &[fid, fs] : parallel_stats) {
    parallel_live += fs.live_bytes;
  }
  CHECK(serial_live == parallel_live);

  // Sum of total_bytes must match.
  std::uint64_t serial_total = 0, parallel_total = 0;
  for (const auto &[fid, fs] : serial_stats) {
    serial_total += fs.total_bytes;
  }
  for (const auto &[fid, fs] : parallel_stats) {
    parallel_total += fs.total_bytes;
  }
  CHECK(serial_total == parallel_total);

  // Sorted live_bytes and total_bytes vectors must match (file IDs may differ
  // between serial and parallel because directory iteration order is
  // non-deterministic, but the multisets of per-file values must be equal).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> serial_vals, parallel_vals;
  for (const auto &[fid, fs] : serial_stats) {
    serial_vals.emplace_back(fs.live_bytes, fs.total_bytes);
  }
  for (const auto &[fid, fs] : parallel_stats) {
    parallel_vals.emplace_back(fs.live_bytes, fs.total_bytes);
  }
  std::ranges::sort(serial_vals);
  std::ranges::sort(parallel_vals);
  CHECK(serial_vals == parallel_vals);
}

// ===========================================================================
// Vacuum tests
// ===========================================================================

// ---------------------------------------------------------------------------
// vacuum_compact_file: basic compaction
// ---------------------------------------------------------------------------
TEST_CASE("vacuum compact removes dead entries", "[vacuum]") {
  TempDir td;
  // Threshold=1 forces rotation after each write → every put lands in its
  // own sealed file (the active file is always a fresh, empty one).
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  // Write k1, then overwrite it → original entry is dead.
  db.put({}, to_bytes("k1"), to_bytes("old"));
  db.put({}, to_bytes("k1"), to_bytes("new"));

  // At this point we have 3 files: file with old k1, file with new k1,
  // and an empty active file. Total dead bytes > 0.
  const auto stats_before = db.file_stats();

  // Vacuum with threshold=0 so all fragmented files qualify.
  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

  // Verify k1 is still readable and has the latest value.
  const auto val = get_val(db, to_bytes("k1"));
  REQUIRE(val.has_value());
  CHECK(to_string(*val) == "new");

  // After vacuum, at least one file should have been removed or replaced.
  const auto stats_after = db.file_stats();

  // The old file (with only dead k1) should be gone.
  // Stats should still be consistent: total live_bytes across all files
  // equals the size of the one live entry.
  std::uint64_t total_live = 0;
  for (const auto &[fid, fs] : stats_after) {
    total_live += fs.live_bytes;
  }
  CHECK(total_live == esize("k1", "new"));
}

// ---------------------------------------------------------------------------
// vacuum_compact_file: tombstones are preserved
// ---------------------------------------------------------------------------
TEST_CASE("vacuum compact preserves tombstones", "[vacuum]") {
  TempDir td;
  {
    auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

    db.put({}, to_bytes("gone"), to_bytes("value"));
    std::ignore = db.del({}, to_bytes("gone"));
    // Now we have: file with Put("gone"), file with Delete("gone"), empty active.

    // Vacuum the file containing the Put (it's 100% dead).
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

    // The key should still be absent.
    CHECK_FALSE(db.contains_key(to_bytes("gone")));
  }

  // Reopen to verify tombstone survives recovery.
  auto db2 = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
  CHECK_FALSE(db2.contains_key(to_bytes("gone")));
}

// ---------------------------------------------------------------------------
// vacuum_compact_file: all entries dead (except tombstones)
// ---------------------------------------------------------------------------
TEST_CASE("vacuum compact on fully dead file", "[vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  db.put({}, to_bytes("x"), to_bytes("v1"));
  // Overwrite so the first file's entry is dead.
  db.put({}, to_bytes("x"), to_bytes("v2"));

  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

  const auto val = get_val(db, to_bytes("x"));
  REQUIRE(val.has_value());
  CHECK(to_string(*val) == "v2");
}

// ---------------------------------------------------------------------------
// vacuum: no files qualify → no-op
// ---------------------------------------------------------------------------
TEST_CASE("vacuum no-op when nothing exceeds threshold", "[vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("k"), to_bytes("v"));

  const auto stats_before = db.file_stats();
  // Default threshold is 0.5 — active file has 0% fragmentation.
  CHECK_FALSE(db.vacuum());
  const auto stats_after = db.file_stats();

  // Same number of files, same content.
  CHECK(stats_before.size() == stats_after.size());
  for (const auto &[fid, fs] : stats_before) {
    auto it = stats_after.find(fid);
    REQUIRE(it != stats_after.end());
    CHECK(it->second.live_bytes == fs.live_bytes);
    CHECK(it->second.total_bytes == fs.total_bytes);
  }
}

// ---------------------------------------------------------------------------
// vacuum_absorb_file: basic absorption into active file
// ---------------------------------------------------------------------------
TEST_CASE("vacuum absorb moves entries to active file", "[vacuum]") {
  TempDir td;
  // Use a large threshold so absorb path is chosen (live data fits in active).
  {
    auto db = bytecask::DB::open(td.path / "db");
    db.put({}, to_bytes("k1"), to_bytes("v1"));
    db.put({}, to_bytes("k2"), to_bytes("v2"));
    // Overwrite k1 to create fragmentation.
    db.put({}, to_bytes("k1"), to_bytes("v1_new"));
    // db destroyed here — background worker drains, hints written
  }
  {
    auto db2 = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
    // Writes to force rotation of existing files.
    db2.put({}, to_bytes("k3"), to_bytes("v3"));
    // Now we have: sealed files from original writes + sealed file with k3 + active.
    // Overwrite k2 so one of the old sealed files has dead entries.
    db2.put({}, to_bytes("k2"), to_bytes("v2_new"));
    // db2 destroyed here — background worker drains, hints written
  }

  // Reopen with large threshold so absorb path is used.
  auto db3 = bytecask::DB::open(td.path / "db");
  const auto stats_before = db3.file_stats().size();

  (void)db3.vacuum({.fragmentation_threshold = 0.0});

  // One sealed file should have been absorbed → fewer files in registry.
  const auto stats_after = db3.file_stats().size();
  CHECK(stats_after < stats_before);

  // All keys still readable.
  CHECK(to_string(*get_val(db3, to_bytes("k1"))) == "v1_new");
  CHECK(to_string(*get_val(db3, to_bytes("k2"))) == "v2_new");
  CHECK(to_string(*get_val(db3, to_bytes("k3"))) == "v3");
}

// ---------------------------------------------------------------------------
// vacuum_absorb_file: LSN preservation across reopen
// ---------------------------------------------------------------------------
TEST_CASE("vacuum absorb preserves LSNs across recovery", "[vacuum]") {
  TempDir td;
  {
    auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("alpha"));
    db.put({}, to_bytes("b"), to_bytes("beta"));
    // Overwrite a to create fragmentation.
    db.put({}, to_bytes("a"), to_bytes("alpha2"));
    // db destroyed here — background worker drains, hints written
  }

  // Reopen with large threshold so absorb is used.
  {
    auto db2 = bytecask::DB::open(td.path / "db");
    (void)db2.vacuum({.fragmentation_threshold = 0.0});
    // db2 destroyed here — background worker drains, hints written
  }

  // Reopen again — recovery should see absorbed entries correctly.
  auto db3 = bytecask::DB::open(td.path / "db");
  CHECK(to_string(*get_val(db3, to_bytes("a"))) == "alpha2");
  CHECK(to_string(*get_val(db3, to_bytes("b"))) == "beta");
}

// ---------------------------------------------------------------------------
// vacuum: compact path chosen for large file
// ---------------------------------------------------------------------------
TEST_CASE("vacuum chooses compact for large file", "[vacuum]") {
  TempDir td;
  // Threshold = 1 forces many small sealed files, each rotated immediately.
  {
    auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("1"));
    db.put({}, to_bytes("a"), to_bytes("2")); // kills first entry.

    // Sealed file with "a"="1" has 100% dead entries.
    // Active file is tiny (threshold=1 → always rotates).
    // With threshold=1, absorb path's precondition (live + active <= threshold)
    // will fail for any file with live data, so compact is always chosen.
    // vacuum() drains pending hint writes internally before selecting target.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

    CHECK(to_string(*get_val(db, to_bytes("a"))) == "2");
    // db destroyed here — background worker drains, hints written
  }

  // Verify recovery after compact.
  auto db2 = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
  CHECK(to_string(*get_val(db2, to_bytes("a"))) == "2");
}

// ---------------------------------------------------------------------------
// vacuum: loop until nothing qualifies
// ---------------------------------------------------------------------------
TEST_CASE("vacuum loop reclaims all fragmentation", "[vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  // Create multiple fragmented files.
  db.put({}, to_bytes("x"), to_bytes("1"));
  db.put({}, to_bytes("y"), to_bytes("2"));
  db.put({}, to_bytes("x"), to_bytes("3")); // kills x=1
  db.put({}, to_bytes("y"), to_bytes("4")); // kills y=2

  // Run vacuum until nothing qualifies.
  while (db.vacuum({.fragmentation_threshold = 0.0})) {}

  CHECK(to_string(*get_val(db, to_bytes("x"))) == "3");
  CHECK(to_string(*get_val(db, to_bytes("y"))) == "4");

  // All live entries should still be accounted for.
  std::uint64_t total_live = 0;
  for (const auto &[fid, fs] : db.file_stats()) {
    total_live += fs.live_bytes;
  }
  CHECK(total_live == esize("x", "3") + esize("y", "4"));
}

// ---------------------------------------------------------------------------
// vacuum: stats consistency after compact
// ---------------------------------------------------------------------------
TEST_CASE("vacuum compact stats consistency", "[vacuum][filestats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  db.put({}, to_bytes("k1"), to_bytes("aaa"));
  db.put({}, to_bytes("k2"), to_bytes("bbb"));
  db.put({}, to_bytes("k1"), to_bytes("ccc")); // kills k1=aaa

  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

  const auto stats = db.file_stats();

  // Sum of live_bytes across all files should equal the live entries.
  std::uint64_t total_live = 0;
  std::uint64_t total_total = 0;
  for (const auto &[fid, fs] : stats) {
    total_live += fs.live_bytes;
    total_total += fs.total_bytes;
    // live_bytes <= total_bytes for every file.
    CHECK(fs.live_bytes <= fs.total_bytes);
  }

  CHECK(total_live == esize("k1", "ccc") + esize("k2", "bbb"));
  // total_bytes >= total_live (there may be tombstones or overhead).
  CHECK(total_total >= total_live);
}

// ---------------------------------------------------------------------------
// vacuum_compact_file: batch entries are compacted correctly
//
// apply_batch writes BulkBegin + entries + BulkEnd markers. Vacuum must
// buffer entries inside BulkBegin..BulkEnd and emit them only when BulkEnd
// is seen, preserving atomicity semantics in the compacted file.
// ---------------------------------------------------------------------------
TEST_CASE("vacuum compact handles batch entries", "[vacuum]") {
  TempDir td;
  // max_file_bytes=1 forces rotation after each write, but apply_batch
  // writes the entire batch (including markers) to the active file before
  // rotation is checked. So the batch lands in one sealed file.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  // Write single keys first to create dead entries that make the batch file
  // qualify for vacuum.
  db.put({}, to_bytes("a"), to_bytes("old_a"));
  db.put({}, to_bytes("b"), to_bytes("old_b"));

  // Now write a batch that overwrites both keys.
  bytecask::Batch batch;
  batch.put(to_bytes("a"), to_bytes("new_a"));
  batch.put(to_bytes("b"), to_bytes("new_b"));
  db.apply_batch({}, std::move(batch));

  // Vacuum — the file with the batch should be compacted.
  while (db.vacuum({.fragmentation_threshold = 0.0})) {}

  // Both keys should have the batch values.
  auto va = get_val(db, to_bytes("a"));
  auto vb = get_val(db, to_bytes("b"));
  REQUIRE(va.has_value());
  REQUIRE(vb.has_value());
  CHECK(to_string(*va) == "new_a");
  CHECK(to_string(*vb) == "new_b");

  // Stats: only live entries remain.
  std::uint64_t total_live = 0;
  for (const auto &[fid, fs] : db.file_stats()) {
    total_live += fs.live_bytes;
  }
  CHECK(total_live == esize("a", "new_a") + esize("b", "new_b"));
}

// ---------------------------------------------------------------------------
// vacuum_compact_file: batch with deletes
//
// A batch that puts one key and deletes another. After vacuum, the put
// should survive and the delete target should be absent.
// ---------------------------------------------------------------------------
TEST_CASE("vacuum compact handles batch with mixed put/del", "[vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  db.put({}, to_bytes("keep"), to_bytes("old"));
  db.put({}, to_bytes("gone"), to_bytes("val"));

  bytecask::Batch batch;
  batch.put(to_bytes("keep"), to_bytes("updated"));
  batch.del(to_bytes("gone"));
  db.apply_batch({}, std::move(batch));

  // Vacuum until stable (limit iterations to avoid infinite loop).
  for (int i = 0; i < 10 && db.vacuum({.fragmentation_threshold = 0.0}); ++i) {}

  auto vk = get_val(db, to_bytes("keep"));
  REQUIRE(vk.has_value());
  CHECK(to_string(*vk) == "updated");
  CHECK_FALSE(db.contains_key(to_bytes("gone")));
}

// ---------------------------------------------------------------------------
// vacuum: stale file deferred when snapshot holds a reference
//
// When a snapshot is alive that references a vacuumed file, the physical
// file deletion is deferred until the snapshot is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("vacuum defers stale file purge while snapshot is alive",
          "[vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  db.put({}, to_bytes("k"), to_bytes("v1"));
  db.put({}, to_bytes("k"), to_bytes("v2")); // kills v1 → file qualifies

  {
    // Take a snapshot that pins the old file.
    auto snap = db.snapshot();

    // Vacuum compacts the dead file, but the snapshot holds a reference.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

    // The key is still readable from the live DB.
    auto v = get_val(db, to_bytes("k"));
    REQUIRE(v.has_value());
    CHECK(to_string(*v) == "v2");

    // The snapshot can still read the old value (frozen view).
    bytecask::Bytes snap_out;
    CHECK(snap.get(to_bytes("k"), snap_out));
  }
  // Snapshot dropped — stale file can now be purged.

  // Another vacuum pass should purge the stale file.
  (void)db.vacuum({.fragmentation_threshold = 0.0});

  // DB still consistent.
  auto v = get_val(db, to_bytes("k"));
  REQUIRE(v.has_value());
  CHECK(to_string(*v) == "v2");
}

// ---------------------------------------------------------------------------
// Snapshot tests
// ---------------------------------------------------------------------------

// Snapshot::get returns the value frozen at snapshot time, not later writes.
TEST_CASE("Snapshot get is frozen at snapshot time", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v1"));

  auto snap = db.snapshot();

  db.put({}, to_bytes("k"), to_bytes("v2"));

  bytecask::Bytes out;
  REQUIRE(snap.get(to_bytes("k"), out));
  CHECK(to_string(out) == "v1");

  REQUIRE(db.get({}, to_bytes("k"), out));
  CHECK(to_string(out) == "v2");
}

// Snapshot::contains_key reflects state at snapshot time, not after a del.
TEST_CASE("Snapshot contains_key is frozen at snapshot time", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v"));

  auto snap = db.snapshot();

  (void)db.del({}, to_bytes("k"));

  CHECK(snap.contains_key(to_bytes("k")));
  CHECK_FALSE(db.contains_key(to_bytes("k")));
}

// Snapshot::get returns false for a key absent at snapshot time.
TEST_CASE("Snapshot get returns false for absent key", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("v"));

  bytecask::Bytes out;
  CHECK_FALSE(snap.get(to_bytes("k"), out));
}

// Snapshot::iter_from yields entries frozen at snapshot time.
TEST_CASE("Snapshot iter_from is frozen at snapshot time", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto snap = db.snapshot();

  // Add a key after the snapshot — must not appear in snap iteration.
  db.put({}, to_bytes("c"), to_bytes("3"));

  std::vector<std::string> keys;
  for (const auto &[k, v] : snap.iter_from()) {
    keys.push_back(to_string(k));
  }
  CHECK(keys == std::vector<std::string>{"a", "b"});
}

// Snapshot::keys_from yields keys frozen at snapshot time.
TEST_CASE("Snapshot keys_from is frozen at snapshot time", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto snap = db.snapshot();

  db.put({}, to_bytes("c"), to_bytes("3"));
  (void)db.del({}, to_bytes("a"));

  std::vector<std::string> keys;
  for (const auto &k : snap.keys_from()) {
    keys.push_back(to_string(k));
  }
  CHECK(keys == std::vector<std::string>{"a", "b"});
}

// ---------------------------------------------------------------------------
// Reverse iteration tests
// ---------------------------------------------------------------------------

TEST_CASE("DB riter_from returns entries in descending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("av"));
  db.put({}, to_bytes("b"), to_bytes("bv"));
  db.put({}, to_bytes("c"), to_bytes("cv"));

  bytecask::ReadOptions ro;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (auto &[k, v] : db.riter_from(ro)) {
    keys.push_back(to_string(k));
    values.push_back(to_string(v));
  }

  REQUIRE(keys.size() == 3);
  CHECK(keys[0] == "c");
  CHECK(keys[1] == "b");
  CHECK(keys[2] == "a");
  CHECK(values[0] == "cv");
  CHECK(values[1] == "bv");
  CHECK(values[2] == "av");
}

TEST_CASE("DB riter_from starts at given key", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("apple"), to_bytes("1"));
  db.put({}, to_bytes("banana"), to_bytes("2"));
  db.put({}, to_bytes("cherry"), to_bytes("3"));

  std::vector<std::string> keys;
  for (auto &[k, v] : db.riter_from({}, to_bytes("banana"))) {
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 2);
  CHECK(keys[0] == "banana");
  CHECK(keys[1] == "apple");
}

TEST_CASE("DB riter_from with nonexistent key starts at predecessor",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("c"), to_bytes("2"));
  db.put({}, to_bytes("e"), to_bytes("3"));

  std::vector<std::string> keys;
  // "d" doesn't exist; upper_bound("d") points to "e", so reverse starts at "c"
  for (auto &[k, v] : db.riter_from({}, to_bytes("d"))) {
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 2);
  CHECK(keys[0] == "c");
  CHECK(keys[1] == "a");
}

TEST_CASE("DB rkeys_from returns all keys in descending order",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("z"), to_bytes("zv"));
  db.put({}, to_bytes("m"), to_bytes("mv"));
  db.put({}, to_bytes("a"), to_bytes("av"));

  std::vector<std::string> keys;
  for (auto &k : db.rkeys_from({})) {
    keys.push_back(to_string(k));
  }

  REQUIRE(keys.size() == 3);
  CHECK(keys[0] == "z");
  CHECK(keys[1] == "m");
  CHECK(keys[2] == "a");
}

TEST_CASE("DB riter_from on empty DB yields nothing", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  std::vector<std::string> keys;
  for (auto &[k, v] : db.riter_from({})) {
    keys.push_back(to_string(k));
  }
  CHECK(keys.empty());
}

TEST_CASE("Snapshot riter_from is frozen at snapshot time", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto snap = db.snapshot();

  db.put({}, to_bytes("c"), to_bytes("3"));

  std::vector<std::string> keys;
  for (const auto &[k, v] : snap.riter_from()) {
    keys.push_back(to_string(k));
  }
  CHECK(keys == std::vector<std::string>{"b", "a"});
}

TEST_CASE("Snapshot rkeys_from is frozen at snapshot time", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto snap = db.snapshot();

  db.put({}, to_bytes("c"), to_bytes("3"));
  (void)db.del({}, to_bytes("a"));

  std::vector<std::string> keys;
  for (const auto &k : snap.rkeys_from()) {
    keys.push_back(to_string(k));
  }
  CHECK(keys == std::vector<std::string>{"b", "a"});
}

// ---------------------------------------------------------------------------
// apply_batch_if tests
// ---------------------------------------------------------------------------

// No conflict: plan applies when no concurrent write touched the keys.
TEST_CASE("apply_batch_if succeeds with no conflict", "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));

  const auto result = get_val(db, to_bytes("k"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "v1");
}

// W-W conflict: key modified after snapshot — returns false.
TEST_CASE("apply_batch_if returns false on modified key",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("interleaved"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// Conflict: key appeared after snapshot — returns false.
TEST_CASE("apply_batch_if returns false when key appeared",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot(); // "k" absent at snapshot time
  db.put({}, to_bytes("k"), to_bytes("appeared"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// Conflict: key deleted after snapshot — returns false.
TEST_CASE("apply_batch_if returns false when key deleted",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  (void)db.del({}, to_bytes("k"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// No conflict on disjoint keys: concurrent write touches "a", plan writes "b".
TEST_CASE("apply_batch_if no conflict on disjoint keys", "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("v0"));
  db.put({}, to_bytes("b"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("a"), to_bytes("concurrent"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("b"), to_bytes("v1"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));

  const auto result = get_val(db, to_bytes("b"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "v1");
}

// Empty plan is a no-op and returns true.
TEST_CASE("apply_batch_if empty plan is a no-op", "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  REQUIRE(db.apply_batch_if({}, std::move(plan)));

  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v0");
}

// ---------------------------------------------------------------------------
// WritePlan guard tests
// ---------------------------------------------------------------------------

// ensure_present succeeds when key exists.
TEST_CASE("apply_batch_if ensure_present passes when key exists",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v1");
}

// ensure_present fails when key is absent.
TEST_CASE("apply_batch_if ensure_present fails when key absent",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_absent succeeds when key does not exist.
TEST_CASE("apply_batch_if ensure_absent passes when key absent",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v1");
}

// ensure_absent fails when key exists.
TEST_CASE("apply_batch_if ensure_absent fails when key exists",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_unchanged succeeds when no concurrent writes.
TEST_CASE("apply_batch_if ensure_unchanged passes without modification",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v1");
}

// ensure_unchanged fails when key modified since snapshot.
TEST_CASE("apply_batch_if ensure_unchanged fails on modification",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("concurrent"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_unchanged on absent key — passes when still absent.
TEST_CASE("apply_batch_if ensure_unchanged passes for absent key staying absent",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("nonexistent"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_unchanged on absent key — fails when key appeared.
TEST_CASE("apply_batch_if ensure_unchanged fails when absent key appeared",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("appeared"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_range_unchanged succeeds when no keys in range were modified.
TEST_CASE("apply_batch_if ensure_range_unchanged passes when range clean",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("v0"));
  db.put({}, to_bytes("c"), to_bytes("v0"));

  auto snap = db.snapshot();
  // Modify a key outside the guarded range.
  db.put({}, to_bytes("a"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_range_unchanged fails when a key in range was modified.
TEST_CASE("apply_batch_if ensure_range_unchanged fails on in-range modification",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("b"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("b"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_range_unchanged fails when a key in range was inserted.
TEST_CASE("apply_batch_if ensure_range_unchanged fails on in-range insertion",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  db.put({}, to_bytes("b"), to_bytes("inserted"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// ensure_range_unchanged fails when a key in range was deleted.
TEST_CASE("apply_batch_if ensure_range_unchanged fails on in-range deletion",
          "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("b"), to_bytes("v0"));

  auto snap = db.snapshot();
  (void)db.del({}, to_bytes("b"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  REQUIRE_FALSE(db.apply_batch_if({}, std::move(plan)));
}

// Guards-only plan with no writes — validates consistency without disk I/O.
TEST_CASE("apply_batch_if guards-only plan with no writes", "[apply_batch_if]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  REQUIRE(db.apply_batch_if({}, std::move(plan)));
}

// Contradictory guards throw std::logic_error at build time.
TEST_CASE("WritePlan contradictory guards throw logic_error",
          "[apply_batch_if]") {
  bytecask::WritePlan plan;
  plan.ensure_present(to_bytes("k"));
  REQUIRE_THROWS_AS(plan.ensure_absent(to_bytes("k")), std::logic_error);
}

TEST_CASE("DB rejects concurrent open on same directory", "[bytecask][lock]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  CHECK_THROWS_AS(bytecask::DB::open(td.path / "db"), std::system_error);
}

TEST_CASE("DB directory unlocked after close", "[bytecask][lock]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path);
    db.put({}, to_bytes("k"), to_bytes("v"));
  }
  auto db2 = bytecask::DB::open(db_path);
  const auto result = get_val(db2, to_bytes("k"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "v");
}

TEST_CASE("DB lock file persists after close", "[bytecask][lock]") {
  TempDir td;
  const auto db_path = td.path / "db";
  { auto db = bytecask::DB::open(db_path); }
  CHECK(std::filesystem::exists(db_path / ".lock"));
}

TEST_CASE("DB lock error includes directory path", "[bytecask][lock]") {
  TempDir td;
  const auto db_path = td.path / "db";
  auto db = bytecask::DB::open(db_path);
  try {
    auto db2 = bytecask::DB::open(db_path);
    FAIL("expected std::system_error");
  } catch (const std::system_error &e) {
    CHECK(std::string_view{e.what()}.find(db_path.string())
          != std::string_view::npos);
  }
}

#ifdef BYTECASK_TESTING
// Single-op optimization: a 1-write apply_batch_if writes no BulkBegin/BulkEnd
// markers, so total_bytes matches an equivalent plain put().
TEST_CASE("apply_batch_if single-op writes no markers", "[apply_batch_if]") {
  auto measure_total = [](auto &&fn) -> std::uint64_t {
    TempDir td;
    auto db = bytecask::DB::open(td.path / "db");
    fn(db);
    std::uint64_t total = 0;
    for (const auto &[fid, fs] : db.file_stats()) total += fs.total_bytes;
    return total;
  };

  const auto put_bytes = measure_total([](auto &db) {
    db.put({}, to_bytes("k"), to_bytes("value"));
  });

  const auto batch_if_bytes = measure_total([](auto &db) {
    auto snap = db.snapshot();
    bytecask::WritePlan plan{std::move(snap)};
    plan.put(to_bytes("k"), to_bytes("value"));
    (void)db.apply_batch_if({}, std::move(plan));
  });

  CHECK(batch_if_bytes == put_bytes);
}

// Single-op optimization: a 1-entry apply_batch writes no markers.
TEST_CASE("apply_batch single-op writes no markers", "[apply_batch_if]") {
  auto measure_total = [](auto &&fn) -> std::uint64_t {
    TempDir td;
    auto db = bytecask::DB::open(td.path / "db");
    fn(db);
    std::uint64_t total = 0;
    for (const auto &[fid, fs] : db.file_stats()) total += fs.total_bytes;
    return total;
  };

  const auto put_bytes = measure_total([](auto &db) {
    db.put({}, to_bytes("k"), to_bytes("value"));
  });

  const auto batch_bytes = measure_total([](auto &db) {
    bytecask::Batch b;
    b.put(to_bytes("k"), to_bytes("value"));
    db.apply_batch({}, std::move(b));
  });

  CHECK(batch_bytes == put_bytes);
}

// ---------------------------------------------------------------------------
// Fault injection tests — directly exercise the BC-131 and BC-133 paths.
// ---------------------------------------------------------------------------

// BC-131: If append() throws mid-batch after BulkBegin has been written, the
// engine force-rotates to a fresh active file. Recovery then sees an orphaned
// BulkBegin with no matching BulkEnd and discards the partial batch entries.
TEST_CASE("mid-batch append failure rotates file and discards partial batch",
          "[fault_inject]") {
  TempDir td;

  // A 2-op batch produces: BulkBegin(0), Put-a(1), Put-b(2), BulkEnd(3).
  // Fail on append call index 2 (Put-b): BulkBegin and Put-a are orphaned in
  // the active file with no matching BulkEnd.
  //
  // The count-based injector fires from checkpoint 3 onward, which also
  // fails the isolation rotation (io_rotate_file_creation) — poisoning
  // this DB instance. That's expected: we're testing that recovery
  // handles the orphaned BulkBegin correctly regardless.
  {
    auto db = bytecask::DB::open(td.path / "db");
    // Write a key before the failure so recovery has something to find.
    db.put({.sync = false}, to_bytes("c"), to_bytes("v3"));

    bytecask::testing::ScopedFaultInjector fi{3};
    bytecask::Batch batch;
    batch.put(to_bytes("a"), to_bytes("v1"));
    batch.put(to_bytes("b"), to_bytes("v2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(batch)),
                      std::system_error);
  } // db closes, fi resets

  // Reopen: recovery must discard the orphaned batch and retain only "c".
  {
    auto db2 = bytecask::DB::open(td.path / "db");
    CHECK_FALSE(get_val(db2, to_bytes("a")).has_value());
    CHECK_FALSE(get_val(db2, to_bytes("b")).has_value());
    const auto vc = get_val(db2, to_bytes("c"));
    REQUIRE(vc.has_value());
    CHECK(to_string(*vc) == "v3");
  }
}

// ---------------------------------------------------------------------------
// Degraded DB tests — mechanism smoke tests not covered by [prove] matrix.
// ---------------------------------------------------------------------------

TEST_CASE("reads work on a degraded DB", "[degraded]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Pre-populate before degrading.
  db.put({.sync = false}, to_bytes("x"), to_bytes("val_x"));

  // Degrade the DB via an orphaned BulkBegin batch.
  {
    bytecask::testing::ScopedFaultInjector fi{2};
    bytecask::Batch batch;
    batch.put(to_bytes("a"), to_bytes("v1"));
    batch.put(to_bytes("b"), to_bytes("v2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(batch)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // get — returns pre-degrade data.
  const auto val = get_val(db, to_bytes("x"));
  REQUIRE(val.has_value());
  CHECK(to_string(*val) == "val_x");

  // contains_key — pure in-memory.
  CHECK(db.contains_key(to_bytes("x")));
  CHECK_FALSE(db.contains_key(to_bytes("a")));

  // snapshot — frozen read-only view.
  auto snap = db.snapshot();
  bytecask::Bytes snap_out;
  CHECK(snap.get(to_bytes("x"), snap_out));

  // iter_from — lazy value fetch.
  auto range = db.iter_from({}, to_bytes("x"));
  CHECK(range.begin() != std::default_sentinel);

  // keys_from — pure in-memory walk.
  auto keys = db.keys_from({}, to_bytes("x"));
  CHECK(keys.begin() != std::default_sentinel);

  // resume() clears the degraded state.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
}

// ---------------------------------------------------------------------------
// F/G visibility tests — BC-155: key changes not published on sync failure.
// ---------------------------------------------------------------------------

TEST_CASE("class F: key not visible after commit sync failure", "[f_visibility]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  bytecask::WritePlan plan;
  plan.put(to_bytes("new_key"), to_bytes("new_val"));

  {
    // io_data_file_sync fires on the commit fdatasync (class F).
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.apply_batch_if({.sync = true}, std::move(plan)),
                      std::system_error);
  }

  // Key must not be visible — write was not confirmed durable.
  CHECK_FALSE(db.contains_key(to_bytes("new_key")));
  // Engine must be degraded — bytes in page cache, key_dir diverges.
  CHECK(db.is_degraded());
  // resume() clears the degraded state.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
}

TEST_CASE("class G: key not visible after rotation sync failure", "[g_visibility]") {
  TempDir td;
  // max_file_bytes=1 forces rotation after the first write.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  // Seed one key to ensure there is something to rotate past.
  db.put({.sync = false}, to_bytes("seed"), to_bytes("v"));

  bytecask::WritePlan plan;
  plan.put(to_bytes("new_key"), to_bytes("new_val"));

  {
    // sync=false means no commit sync, so the only io_data_file_sync
    // checkpoint that fires is the pre-rotation sync (class G).
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.apply_batch_if({.sync = false}, std::move(plan)),
                      std::system_error);
  }

  // Key must not be visible — write was not confirmed durable.
  CHECK_FALSE(db.contains_key(to_bytes("new_key")));
  // Engine must be degraded — bytes in page cache, key_dir diverges.
  CHECK(db.is_degraded());
  // Pre-existing key must still be visible.
  CHECK(db.contains_key(to_bytes("seed")));
  // resume() clears the degraded state.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  // Pre-existing key still visible after resume.
  CHECK(db.contains_key(to_bytes("seed")));
}

TEST_CASE("resume() recovers from degraded state", "[degraded][resume]") {
  TempDir td;

  // max_file_bytes=30 triggers rotation once the file exceeds ~30 bytes.
  // Each entry is ~23 bytes (15 hdr + 4 crc + 2 key + 2 value).
  // k1 fits (23 bytes); k2 pushes the file to ~46 bytes, triggering rotation.
  bytecask::DB db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 30});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));

  {
    // Fault fires during rotation after k2 is committed (class T4).
    bytecask::testing::ScopedFaultInjector fi{"io_rotate_file_creation"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // resume() clears the degraded state.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // Both k1 and k2 were committed before/during the fault — must still be visible.
  CHECK(db.contains_key(to_bytes("k1")));
  CHECK(db.contains_key(to_bytes("k2")));

  // Subsequent writes succeed after resume().
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key(to_bytes("k3")));
}

TEST_CASE("resume() replays unpublished entries from active file",
          "[degraded][resume]") {
  TempDir td;

  // Use a large max_file_bytes to keep everything on one active file.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1'000'000});

  // k1 committed normally — baseline.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  CHECK(db.contains_key(to_bytes("k1")));

  // k2 written to page cache but key changes NOT published (class F sync
  // failure). fdatasync fails → engine degrades; k2 bytes in page cache.
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                      std::system_error);
  }
  CHECK_FALSE(db.contains_key(to_bytes("k2")));
  // F degrades the engine immediately.
  REQUIRE(db.is_degraded());

  // resume() scans the active file. The k2 bytes are in the page cache
  // and pread sees them. resume() replays k2 into key_dir, then syncs
  // (confirming durability), seals the file, and opens a fresh active file.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // k1 was always committed and must still be visible.
  CHECK(db.contains_key(to_bytes("k1")));
  // k2 was replayed by resume() from the page-cache bytes.
  CHECK(db.contains_key(to_bytes("k2")));

  // Verify actual values.
  auto v = get_val(db, to_bytes("k2"));
  REQUIRE(v.has_value());
  CHECK(to_string(*v) == "v2");

  // Subsequent writes work.
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key(to_bytes("k3")));
}

TEST_CASE("writes throw DbDegraded on a degraded engine", "[degraded]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({.sync = false}, to_bytes("k1"), to_bytes("v1"));

  // Degrade via sync failure (class F).
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("a"), to_bytes("v")),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // put must throw DbDegraded.
  REQUIRE_THROWS_AS(db.put({.sync = false}, to_bytes("k2"), to_bytes("v2")),
                    bytecask::DbDegraded);

  // del must throw DbDegraded.
  REQUIRE_THROWS_AS((void)db.del({.sync = false}, to_bytes("k1")),
                    bytecask::DbDegraded);

  // apply_batch must throw DbDegraded.
  {
    bytecask::Batch batch;
    batch.put(to_bytes("k3"), to_bytes("v3"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(batch)),
                      bytecask::DbDegraded);
  }

  // apply_batch_if must throw DbDegraded.
  {
    bytecask::WritePlan plan;
    plan.put(to_bytes("k4"), to_bytes("v4"));
    REQUIRE_THROWS_AS(
        (void)db.apply_batch_if({.sync = false}, std::move(plan)),
        bytecask::DbDegraded);
  }

  // After resume, writes succeed again.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  REQUIRE_NOTHROW(db.put({.sync = false}, to_bytes("k5"), to_bytes("v5")));
  CHECK(db.contains_key(to_bytes("k5")));
}

TEST_CASE("resume() discards pending batch on CRC error in active file",
          "[degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1'000'000});

  // Committed entries — baseline.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));

  // Write a batch that lands on disk (append succeeds) but sync fails,
  // degrading the engine. The batch bytes are in the page cache.
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    bytecask::Batch batch;
    batch.put(to_bytes("b1"), to_bytes("bv1"));
    batch.put(to_bytes("b2"), to_bytes("bv2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = true}, std::move(batch)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // Corrupt the active data file — flip a byte in the batch region so
  // resume's scan hits a CRC error mid-batch.
  for (auto &e : std::filesystem::directory_iterator(td.path / "db")) {
    if (e.path().extension() == ".data") {
      auto sz = std::filesystem::file_size(e.path());
      if (sz > 10) {
        std::fstream f{e.path(), std::ios::in | std::ios::out | std::ios::binary};
        // Corrupt near the end — where the batch entries live.
        f.seekp(static_cast<std::streamoff>(sz - 5));
        char c{};
        f.get(c);
        f.seekp(static_cast<std::streamoff>(sz - 5));
        c ^= static_cast<char>(0xFF);
        f.put(c);
      }
    }
  }

  // resume() should recover — the CRC error causes pending.clear() and
  // the incomplete batch is discarded via truncation.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // Committed entries before the batch must survive.
  CHECK(db.contains_key(to_bytes("k1")));
  CHECK(db.contains_key(to_bytes("k2")));

  // Writes succeed after resume.
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key(to_bytes("k3")));
}

TEST_CASE("resume() with live snapshot on degraded DB",
          "[degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Pre-populate.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));

  // Degrade via orphaned BulkBegin (class C).
  {
    bytecask::testing::ScopedFaultInjector fi{2};
    bytecask::Batch batch;
    batch.put(to_bytes("a"), to_bytes("va"));
    batch.put(to_bytes("b"), to_bytes("vb"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(batch)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // Take snapshot while degraded — pins data files.
  auto snap = db.snapshot();
  bytecask::Bytes out;
  CHECK(snap.get(to_bytes("k1"), out));
  CHECK(snap.get(to_bytes("k2"), out));
  CHECK_FALSE(snap.contains_key(to_bytes("a")));

  // resume() with snapshot still alive.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // Snapshot still readable — pinned files not deleted.
  CHECK(snap.get(to_bytes("k1"), out));
  CHECK(snap.get(to_bytes("k2"), out));

  // Post-resume writes succeed.
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key(to_bytes("k3")));

  // DB reads still correct.
  CHECK(db.contains_key(to_bytes("k1")));
  CHECK(db.contains_key(to_bytes("k2")));
}

// ---------------------------------------------------------------------------
// validate_preconditions unit tests
// ---------------------------------------------------------------------------
// These test TransientEngineState::validate_preconditions in isolation —
// no DB, no disk I/O. We construct EngineState directly, call .transient(),
// and verify each guard path.

namespace {

// Builds a minimal EngineState with the given key→KeyDirEntry pairs.
auto make_state(
    std::initializer_list<std::pair<std::string, bytecask::KeyDirEntry>> entries)
    -> std::shared_ptr<bytecask::EngineState> {
  auto s = std::make_shared<bytecask::EngineState>();
  s->active_file_id = 1;
  s->next_file_id = 2;
  s->next_lsn = 100;
  for (const auto &[k, v] : entries) {
    s->key_dir = s->key_dir.set(to_bytes(k), v);
  }
  return s;
}

auto make_snapshot(
    std::initializer_list<std::pair<std::string, bytecask::KeyDirEntry>> entries)
    -> bytecask::Snapshot {
  return bytecask::Snapshot::from_state(make_state(entries));
}

} // namespace

// --- Point guards without snapshot ---

TEST_CASE("validate_preconditions: MustExist passes when key present",
          "[validate_preconditions]") {
  auto state = make_state({{"k", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.ensure_present(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustExist fails when key absent",
          "[validate_preconditions]") {
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.ensure_present(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeAbsent passes when key absent",
          "[validate_preconditions]") {
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeAbsent fails when key present",
          "[validate_preconditions]") {
  auto state = make_state({{"k", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: None precondition always passes",
          "[validate_preconditions]") {
  auto state = make_state({{"k", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

// --- MustBeUnchanged (requires snapshot) ---

TEST_CASE("validate_preconditions: MustBeUnchanged passes when key unchanged",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({{"k", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeUnchanged fails when key modified",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({{"k", {20, 1, 100, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeUnchanged fails when key deleted",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeUnchanged passes when key absent in both",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeUnchanged fails when key appeared",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({{"k", {15, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

// --- Range guards ---

TEST_CASE("validate_preconditions: range guard passes when range unchanged",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", {5, 1, 0, 3}}, {"b", {10, 1, 0, 5}}, {"d", {15, 1, 0, 4}}});
  auto state = make_state({{"a", {5, 1, 0, 3}}, {"b", {10, 1, 0, 5}}, {"d", {15, 1, 0, 4}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard fails when key modified in range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"b", {10, 1, 0, 5}}});
  auto state = make_state({{"b", {20, 1, 100, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard fails when key inserted in range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({{"c", {20, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard fails when key deleted in range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"c", {10, 1, 0, 5}}});
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard ignores keys outside range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", {5, 1, 0, 3}}, {"b", {10, 1, 0, 5}}});
  auto state = make_state({{"a", {50, 1, 0, 3}}, {"b", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK(t.validate_preconditions(plan));
}

// --- Implicit W-W conflict detection (snapshot present) ---

TEST_CASE("validate_preconditions: W-W passes when write key unchanged",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({{"k", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W fails when write key modified",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({{"k", {20, 1, 100, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W fails when write key appeared",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({{"k", {15, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W fails when write key deleted",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W passes for new key absent in both",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("new_key"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W skips guard-only keys",
          "[validate_preconditions]") {
  // Key "g" is guard-only (ensure_present), "k" is the write.
  // "g" was modified concurrently but W-W only checks write keys.
  auto snap = make_snapshot({{"g", {10, 1, 0, 5}}, {"k", {10, 1, 0, 5}}});
  auto state = make_state({{"g", {20, 1, 100, 3}}, {"k", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("g"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

// --- Combined scenarios ---

TEST_CASE("validate_preconditions: multiple guards all pass",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", {5, 1, 0, 3}}, {"b", {10, 1, 0, 5}}});
  auto state = make_state({{"a", {5, 1, 0, 3}}, {"b", {10, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("a"));
  plan.ensure_unchanged(to_bytes("b"));
  plan.put(to_bytes("a"), to_bytes("new_a"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: one failing guard rejects plan",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", {5, 1, 0, 3}}, {"b", {10, 1, 0, 5}}});
  auto state = make_state({{"a", {5, 1, 0, 3}}, {"b", {20, 1, 100, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("a"));
  plan.ensure_unchanged(to_bytes("b"));
  plan.put(to_bytes("a"), to_bytes("new_a"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: del with snapshot triggers W-W check",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", {10, 1, 0, 5}}});
  auto state = make_state({{"k", {20, 1, 100, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.del(to_bytes("k"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: no snapshot skips W-W checks",
          "[validate_preconditions]") {
  auto state = make_state({{"k", {20, 1, 0, 5}}});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

// ---------------------------------------------------------------------------
// apply_resume unit tests
// ---------------------------------------------------------------------------
// These test TransientEngineState::apply_resume in isolation — no DB, no
// disk I/O. We construct EngineState directly, call .transient(), apply
// resume entries, and verify key_dir, file_stats, and next_lsn.

namespace {

auto make_resume_entry(std::uint64_t seq, bytecask::EntryType type,
                       std::string key, std::uint64_t file_off = 0,
                       std::uint32_t val_size = 0) -> bytecask::ResumeEntry {
  return {seq, type, file_off, val_size,
          {to_bytes(key).begin(), to_bytes(key).end()}};
}

auto make_state_with_stats(
    std::initializer_list<std::pair<std::string, bytecask::KeyDirEntry>> entries,
    std::map<std::uint32_t, bytecask::FileStats> stats = {})
    -> std::shared_ptr<bytecask::EngineState> {
  auto s = make_state(entries);
  auto fstats_t = s->file_stats.transient();
  for (const auto &[id, fs] : stats) fstats_t.set(id, fs);
  s->file_stats = std::move(fstats_t).persistent();
  return s;
}

} // namespace

TEST_CASE("apply_resume: put inserts new key into empty key_dir",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(50, bytecask::EntryType::Put, "k1", 0, 4)};
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence == 50);
  CHECK(kv->file_id == 1);
  CHECK(kv->file_offset == 0);
  CHECK(kv->value_size == 4);
}

TEST_CASE("apply_resume: put with higher LSN overwrites existing",
          "[apply_resume]") {
  // file_id=2 holds existing key at seq=10
  auto state = make_state_with_stats(
      {{"k1", {10, 2, 100, 5}}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(50, bytecask::EntryType::Put, "k1", 200, 8)};
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence == 50);
  CHECK(kv->file_id == 1);
  CHECK(kv->file_offset == 200);
  CHECK(kv->value_size == 8);
  // Old file's live_bytes decreased.
  CHECK(s->file_stats.get(2)->live_bytes == 0);
  // New file's live_bytes increased.
  CHECK(s->file_stats.get(1)->live_bytes == bytecask::entry_size(2, 8));
}

TEST_CASE("apply_resume: put with lower LSN is ignored",
          "[apply_resume]") {
  auto state = make_state_with_stats(
      {{"k1", {50, 2, 100, 5}}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(10, bytecask::EntryType::Put, "k1", 200, 8)};
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence == 50);
  CHECK(kv->file_id == 2);
  // file_stats unchanged.
  CHECK(s->file_stats.get(2)->live_bytes == bytecask::entry_size(2, 5));
  CHECK(s->file_stats.get(1)->live_bytes == 0);
}

TEST_CASE("apply_resume: delete removes key when LSN is higher",
          "[apply_resume]") {
  auto state = make_state_with_stats(
      {{"k1", {10, 2, 100, 5}}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(50, bytecask::EntryType::Delete, "k1")};
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  CHECK_FALSE(s->key_dir.get(to_bytes("k1")).has_value());
  CHECK(s->file_stats.get(2)->live_bytes == 0);
}

TEST_CASE("apply_resume: delete is ignored when LSN is lower",
          "[apply_resume]") {
  auto state = make_state_with_stats(
      {{"k1", {50, 2, 100, 5}}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(10, bytecask::EntryType::Delete, "k1")};
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence == 50);
}

TEST_CASE("apply_resume: advances next_lsn past highest seen LSN",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  state->next_lsn = 10;
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(25, bytecask::EntryType::Put, "a", 0, 3),
      make_resume_entry(30, bytecask::EntryType::Put, "b", 100, 4)};
  t.apply_resume(1, entries);

  CHECK(t.next_lsn() == 31);
}

TEST_CASE("apply_resume: does not regress next_lsn when entries have lower LSN",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  state->next_lsn = 100;
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(5, bytecask::EntryType::Put, "a", 0, 3)};
  t.apply_resume(1, entries);

  CHECK(t.next_lsn() == 100);
}

TEST_CASE("apply_resume: empty entries is a no-op",
          "[apply_resume]") {
  auto state = make_state_with_stats({{"k1", {10, 1, 0, 5}}}, {{1, {42, 100}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries;
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  CHECK(s->key_dir.get(to_bytes("k1")).has_value());
  CHECK(s->file_stats.get(1)->live_bytes == 42);
  CHECK(s->next_lsn == 100);
}

TEST_CASE("apply_resume: multiple entries replayed in order",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  state->next_lsn = 1;
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(10, bytecask::EntryType::Put, "k1", 0, 5),
      make_resume_entry(11, bytecask::EntryType::Put, "k2", 100, 8),
      make_resume_entry(12, bytecask::EntryType::Delete, "k1")};
  t.apply_resume(1, entries);

  auto s = std::move(t).persistent();
  CHECK_FALSE(s->key_dir.get(to_bytes("k1")).has_value());
  auto kv2 = s->key_dir.get(to_bytes("k2"));
  REQUIRE(kv2.has_value());
  CHECK(kv2->sequence == 11);
  CHECK(s->next_lsn == 13);
}

#endif
