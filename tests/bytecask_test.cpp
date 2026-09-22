// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — integration and model-based correctness tests

#include <algorithm>
#include <atomic>
#include <concepts>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include "mapping_probe.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <array>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

import bytecask;
import bytecask.batch_iterator;
import bytecask.buffer_pool;
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

auto to_string(std::span<const std::byte> span) -> std::string {
  std::string s(span.size(), '\0');
  std::ranges::transform(span, s.begin(),
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

// get_val as a string, naming the absent case so a failed CHECK says which
// of the two it was.
auto get_str(const bytecask::DB &db, bytecask::BytesView key) -> std::string {
  const auto v = get_val(db, key);
  return v ? to_string(*v) : std::string{"<missing>"};
}

// Creates a unique temp directory for each test, cleaned up on scope exit.
struct TempDir {
  std::filesystem::path path;

  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "bc_test_{}_{}",
                 std::chrono::system_clock::now().time_since_epoch().count(),
                 next_id())} {
    std::filesystem::create_directories(path);
  }

  ~TempDir() { std::filesystem::remove_all(path); }

 private:
  static auto next_id() -> unsigned {
    static unsigned counter = 0;
    return counter++;
  }
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

void flip_byte(const std::filesystem::path &p, std::uint64_t off) {
  std::fstream f{p, std::ios::in | std::ios::out | std::ios::binary};
  f.seekg(static_cast<std::streamoff>(off));
  char c{};
  f.get(c);
  f.seekp(static_cast<std::streamoff>(off));
  f.put(static_cast<char>(c ^ static_cast<char>(0xFF)));
}

// Corrupts the key of the first entry in a data file. The 15-byte header is
// left intact so the entry still frames correctly and fails on its CRC —
// which is what a rescan of the file has to hit for the rescan to fail.
void corrupt_first_entry_key(const std::filesystem::path &data) {
  flip_byte(data, 15);
}

// Damages a hint file AND the data file behind it: the one state in which
// recovery genuinely cannot index a file, because rebuilding the hint from
// the data file is not available either.
void corrupt_hint_beyond_rebuild(const std::filesystem::path &hint) {
  corrupt_file_middle(hint);
  corrupt_first_entry_key(hint.parent_path() /
                          (hint.stem().string() + ".data"));
}

// Returns .hint files in dir sorted by name (ascending creation-time order).
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
  const bool removed = db.del({}, to_bytes("key1")).has_value();

  CHECK(removed);
  CHECK_FALSE(get_val(db, to_bytes("key1")).has_value());
}

// ---------------------------------------------------------------------------
// Test 6: contains_key tracks puts and dels
// ---------------------------------------------------------------------------
TEST_CASE("DB contains_key tracks mutations", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  CHECK_FALSE(db.contains_key({}, to_bytes("k")));
  db.put({}, to_bytes("k"), to_bytes("v"));
  CHECK(db.contains_key({}, to_bytes("k")));
  CHECK(db.del({}, to_bytes("k")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k")));
}

// ---------------------------------------------------------------------------
// Test 7: apply_batch — mixed puts and del, all visible atomically
// ---------------------------------------------------------------------------
TEST_CASE("DB apply_batch mixed operations", "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Pre-insert a key that the plan will remove.
  db.put({}, to_bytes("del"), to_bytes("gone"));

  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("alpha"));
  plan.put(to_bytes("b"), to_bytes("beta"));
  plan.del(to_bytes("del"));
  (void)db.apply_batch({}, std::move(plan));

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
  for (auto &entry : db.iter_from(ro)) {
    keys.push_back(to_string(entry.key));
    values.push_back(to_string(entry.value));
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
// Entry iterators are move-only: operator* caches spans into the iterator's
// own io_buf_, so a copy would carry spans addressing the source's storage.
// Deleting the copy makes that unrepresentable instead of merely documented.
// See "View and span lifetimes" in CONTRACT.md.
// ---------------------------------------------------------------------------
TEST_CASE("entry iterators are move-only and still model input_iterator",
          "[bytecask]") {
  static_assert(!std::copyable<bytecask::EntryIterator>);
  static_assert(!std::copyable<bytecask::ReverseEntryIterator>);
  static_assert(std::movable<bytecask::EntryIterator>);
  static_assert(std::movable<bytecask::ReverseEntryIterator>);
  static_assert(std::input_iterator<bytecask::EntryIterator>);
  static_assert(std::input_iterator<bytecask::ReverseEntryIterator>);

  // Key iterators materialize an owning Key, so a copy owns its own bytes
  // and stays copyable — ReverseIterator<KeyIterator> needs that.
  static_assert(std::copyable<bytecask::KeyIterator>);
  static_assert(std::copyable<bytecask::ReverseKeyIterator>);

  // A moved-from iterator hands its buffer to the destination, so spans
  // taken before the move keep addressing live memory.
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("av"));

  bytecask::ReadOptions ro;
  auto range = db.iter_from(ro);
  auto it = range.begin();
  const auto value = (*it).value;
  auto moved = std::move(it);
  CHECK(to_string(value) == "av");
  CHECK(to_string((*moved).value) == "av");
  CHECK(value.data() == (*moved).value.data());

  // Move assignment carries the buffer too, so the span stays addressed at
  // the same storage rather than at the moved-from iterator's.
  bytecask::EntryIterator sink;
  sink = std::move(moved);
  CHECK(to_string((*sink).value) == "av");
  CHECK(value.data() == (*sink).value.data());

  auto rrange = db.riter_from(ro);
  auto rit = rrange.begin();
  const auto rvalue = (*rit).value;
  bytecask::ReverseEntryIterator rsink;
  rsink = std::move(rit);
  CHECK(to_string((*rsink).value) == "av");
  CHECK(rvalue.data() == (*rsink).value.data());
}

// ---------------------------------------------------------------------------
// Invariant P: every published entry lies inside the committed extent of the
// file it names. This is what lets resume() shorten the active file under
// lock-free readers — see "View and span lifetimes" in CONTRACT.md. The check
// only fires on a violation the engine cannot currently produce, so the seam
// is exercised directly on a hand-built state.
// ---------------------------------------------------------------------------
TEST_CASE("state consistency rejects an entry outside its file's extent",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k0"), to_bytes("v0"));

  auto good = *db.engine_state();
  REQUIRE_NOTHROW(db.test_validate_state_consistency(good));

  // Shrink the active file's committed extent below the entry that points
  // into it — the shape a truncation past a published offset would leave.
  auto bad = good;
  {
    auto t = bad.file_stats.transient();
    t.update(bad.active_file_id,
             [](bytecask::FileStats &fs) { fs.total_bytes = 1; });
    bad.file_stats = std::move(t).persistent();
  }
  bool threw = false;
  try {
    db.test_validate_state_consistency(bad);
  } catch (const std::runtime_error &e) {
    threw = true;
    CHECK(std::string{e.what()}.find("committed extent") != std::string::npos);
  }
  CHECK(threw);
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
  for (auto &entry : db.iter_from({}, to_bytes("banana"))) {
    keys.push_back(to_string(entry.key));
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
// Data file stems carry a 64-bit salt
//
// The timestamp only separates files by the second, so within one second the
// salt separates them alone. A 32-bit salt repeated often enough to be hit in
// CI (~5e-4 per 2,000-file test run); 64 bits puts that at ~1e-13. Recovery
// keys on the V01 suffix and never parses the stem's interior, so widening it
// leaves older files readable — "DB recovery: incomplete batch is discarded"
// below still hand-writes an 8-hex-salt name and recovers from it.
// ---------------------------------------------------------------------------
TEST_CASE("DB data file stems carry a 64-bit salt", "[bytecask][rotation]") {
  TempDir td;
  const auto db_path = td.path / "db";
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
  for (int i = 0; i < 8; ++i) {
    db.put({}, to_bytes("k" + std::to_string(i)), to_bytes("v"));
  }

  // data_<14 digits>_<16 lowercase hex>_V01
  auto well_formed = [](std::string_view stem) {
    if (!stem.starts_with("data_") || !stem.ends_with("_V01")) return false;
    if (stem.size() != 5 + 14 + 1 + 16 + 4) return false;
    for (std::size_t i = 5; i < 19; ++i) {
      if (stem[i] < '0' || stem[i] > '9') return false;
    }
    if (stem[19] != '_') return false;
    for (std::size_t i = 20; i < 36; ++i) {
      const auto c = stem[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
  };

  std::set<std::string> salts;
  int checked = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() != ".data") continue;
    const auto stem = e.path().stem().string();
    INFO("stem=\"" << stem << "\"");
    CHECK(well_formed(stem));
    salts.insert(stem.substr(20, 16));
    ++checked;
  }
  REQUIRE(checked > 1);
  // Every file drew its own salt — the old generator was seeded once per
  // thread, which is the property that had to change.
  CHECK(salts.size() == static_cast<std::size_t>(checked));
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
  for (auto &entry : db.iter_from({})) {
    keys.push_back(to_string(entry.key));
    values.push_back(to_string(entry.value));
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
  const bool removed = db.del(no_sync, to_bytes("k")).has_value();

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
  bytecask::WritePlan plan;
  plan.put(to_bytes("x"), to_bytes("xv"));
  plan.put(to_bytes("y"), to_bytes("yv"));
  (void)db.apply_batch(no_sync, std::move(plan));

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
// Test 23: plan survives restart — all puts/dels from plan visible
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: batch survives restart", "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);
    db.put({}, to_bytes("preexisting"), to_bytes("gone"));

    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("alpha"));
    plan.put(to_bytes("b"), to_bytes("beta"));
    plan.del(to_bytes("preexisting"));
    (void)db.apply_batch({}, std::move(plan));
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
  CHECK_FALSE(db2.contains_key({}, to_bytes("gone")));
  CHECK_FALSE(get_val(db2, to_bytes("gone")).has_value());
  REQUIRE(get_val(db2, to_bytes("keep")).has_value());
  CHECK(to_string(*get_val(db2, to_bytes("keep"))) == "v2");
}

// ---------------------------------------------------------------------------
// Test: incomplete batch entries are discarded during recovery.
// Simulates a crash mid-batch by writing a BulkBegin + Put entries with no
// BulkEnd directly to a data file. Recovery generates a hint file from the
// raw data and only the standalone entries survive (incomplete batch discarded).
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: incomplete batch is discarded",
          "[bytecask][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  std::filesystem::create_directories(db_path);

  {
    // Manually write a data file simulating a crash mid-batch.
    auto df = bytecask::openDataFileForWrite(
        db_path / "data_00000000000000_00000000_V01.data", 0, bytecask::IoBackend::Pread);
    // Standalone entry — should survive.
    std::ignore = df->append_entry(1, bytecask::EntryType::Put, to_bytes("good"),
                            to_bytes("value1"));
    // Begin batch, write some entries, but never write BulkEnd.
    std::ignore = df->append_entry(2, bytecask::EntryType::BulkBegin, {}, {});
    std::ignore = df->append_entry(3, bytecask::EntryType::Put, to_bytes("orphan_a"),
                            to_bytes("lost1"));
    std::ignore = df->append_entry(4, bytecask::EntryType::Put, to_bytes("orphan_b"),
                            to_bytes("lost2"));
    // No BulkEnd — simulates crash.
    df->sync();
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
// with crafted names and sequences so that in one sub-case the tombstone file
// sorts alphabetically first, and in the other it sorts last. Both must
// yield the same result: "gone" absent, "alive" present.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: order-independent tombstone",
          "[bytecask][recovery]") {
  // Sub-case A: delete file sorts BEFORE put file (alphabetically).
  // Sub-case B: delete file sorts AFTER put file.
  // In both, the delete has a higher sequence than the put, so it must win.
  auto run = [](std::string_view put_stem, std::string_view del_stem) {
    TempDir td;
    const auto db_path = td.path / "db";
    std::filesystem::create_directories(db_path);

    // File with a Put for "gone" (seq=1) and "alive" (seq=2).
    {
      auto df = bytecask::openDataFileForWrite(
          db_path / std::format("{}.data", put_stem), 0, bytecask::IoBackend::Pread);
      std::ignore = df->append_entry(1, bytecask::EntryType::Put, to_bytes("gone"),
                              to_bytes("v1"));
      std::ignore = df->append_entry(2, bytecask::EntryType::Put, to_bytes("alive"),
                              to_bytes("v2"));
      df->sync();
    }

    // File with a Delete for "gone" (seq=3) — higher sequence wins.
    {
      auto df = bytecask::openDataFileForWrite(
          db_path / std::format("{}.data", del_stem), 0, bytecask::IoBackend::Pread);
      std::ignore = df->append_entry(3, bytecask::EntryType::Delete,
                              to_bytes("gone"), {});
      df->sync();
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
    for (auto &entry : serial.iter_from({})) {
      serial_kv[to_string(entry.key)] = to_string(entry.value);
    }
  }

  // Recover in parallel.
  auto parallel = bytecask::DB::open(db_path, {.max_file_bytes = 1, .recovery_threads = 4});

  // Collect parallel results.
  std::map<std::string, std::string> parallel_kv;
  for (auto &entry : parallel.iter_from({})) {
    parallel_kv[to_string(entry.key)] = to_string(entry.value);
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
// A hint file is a derived index, so a corrupt one alone is repaired rather
// than fatal. Damage the data file behind it too and there is nothing left to
// rebuild from: strict recovery must throw std::runtime_error from DB::open.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: strict mode throws when a hint cannot be rebuilt",
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

  // Corrupt the second hint file (middle of three by timestamp sort) and the
  // data file it indexes, so the rebuild has no intact source to scan.
  const auto hints = list_hint_files(db_path);
  REQUIRE(hints.size() >= 2);
  corrupt_hint_beyond_rebuild(hints[1]);

  // Default options → fail_recovery_on_crc_errors=true → must throw.
  REQUIRE_THROWS_AS(bytecask::DB::open(db_path), std::runtime_error);
}

// ---------------------------------------------------------------------------
// BC-157: fail_recovery_on_crc_errors — lenient mode
//
// Same corruption; lenient recovery must open successfully.
// Keys from the unindexable file are absent; keys from clean files present.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: lenient mode opens with partial recovery when a hint "
          "cannot be rebuilt",
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
  // Filenames contain random salts so name-sort order is non-deterministic.
  // Corrupt the middle pair by index — we don't know which key it holds.
  corrupt_hint_beyond_rebuild(hints[1]);

  // Lenient recovery must succeed even with a corrupt hint file.
  bytecask::DB db =
      bytecask::DB::open(db_path, {.max_file_bytes = 1,
                                   .fail_recovery_on_crc_errors = false});
  CHECK_FALSE(db.is_degraded());
  // Exactly one key should be missing (the one from the corrupt hint file).
  int found = 0;
  if (get_val(db, to_bytes("key_first")).has_value()) ++found;
  if (get_val(db, to_bytes("key_second")).has_value()) ++found;
  if (get_val(db, to_bytes("key_third")).has_value()) ++found;
  CHECK(found == 2);
}

// ---------------------------------------------------------------------------
// BC-157: parallel recovery terminate bug fix
//
// Before the fix, an exception escaping a jthread lambda called std::terminate.
// This test verifies that a corrupt hint file with recovery_threads>1 and
// fail_recovery_on_crc_errors=true raises std::runtime_error — not std::terminate.
// ---------------------------------------------------------------------------
TEST_CASE("DB parallel recovery: an unrebuildable hint throws instead of "
          "terminating",
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
  corrupt_hint_beyond_rebuild(hints[1]);

  // recovery_threads>1 + strict mode → must throw, not terminate.
  REQUIRE_THROWS_AS(
      bytecask::DB::open(db_path,
                         {.max_file_bytes = 1,
                          .recovery_threads = 4,
                          .fail_recovery_on_crc_errors = true}),
      std::runtime_error);
}

// ---------------------------------------------------------------------------
// A hint file is a derived index, not the records it points at. A CRC failure
// in one says the index is damaged, not the data file behind it, so recovery
// rebuilds it from that data file — the path a missing hint already takes —
// and loses nothing. Before this, a corrupt hint was skipped: its keys went
// missing from an intact file, and the next vacuum, seeing a file with no
// live bytes, unlinked it and made the loss permanent.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: a corrupt hint is rebuilt from its data file",
          "[bytecask][recovery][hint_rebuild]") {
  const auto strict = GENERATE(true, false);
  const auto threads = GENERATE(1u, 4u);
  CAPTURE(strict, threads);

  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("key_first"), to_bytes("v1"));
    db.put({}, to_bytes("key_second"), to_bytes("v2"));
    db.put({}, to_bytes("key_third"), to_bytes("v3"));
  }

  // Only the index is damaged; every entry in the data file still holds.
  const auto hints = list_hint_files(db_path);
  REQUIRE(hints.size() >= 2);
  corrupt_file_middle(hints[1]);

  {
    auto db = bytecask::DB::open(db_path,
                                 {.max_file_bytes = 1,
                                  .recovery_threads = threads,
                                  .fail_recovery_on_crc_errors = strict});

    // Nothing is lost, in either recovery mode: a damaged index costs the
    // time to rebuild it, not keys.
    CHECK(get_str(db, to_bytes("key_first")) == "v1");
    CHECK(get_str(db, to_bytes("key_second")) == "v2");
    CHECK(get_str(db, to_bytes("key_third")) == "v3");
  }

  // The repair is persisted, not redone per open: the hint is back on disk,
  // and a second open — strict, so an unreadable hint that could not be
  // rebuilt would throw — still finds everything.
  CHECK(std::filesystem::exists(hints[1]));
  CHECK(std::filesystem::file_size(hints[1]) > 0);

  auto reopened = bytecask::DB::open(db_path, {.max_file_bytes = 1});
  CHECK(get_str(reopened, to_bytes("key_first")) == "v1");
  CHECK(get_str(reopened, to_bytes("key_second")) == "v2");
  CHECK(get_str(reopened, to_bytes("key_third")) == "v3");
}

// ---------------------------------------------------------------------------
// The loss in the case above only became permanent at the next vacuum, which
// saw a file with no live bytes and unlinked it. With the file indexed again
// its bytes are live, so vacuum has to leave it alone.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: vacuum keeps a data file whose hint was rebuilt",
          "[bytecask][recovery][hint_rebuild][vacuum]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("key_first"), to_bytes("v1"));
    db.put({}, to_bytes("key_second"), to_bytes("v2"));
    db.put({}, to_bytes("key_third"), to_bytes("v3"));
  }

  const auto hints = list_hint_files(db_path);
  REQUIRE(hints.size() >= 2);
  const auto victim_data =
      db_path / (hints[1].stem().string() + ".data");
  corrupt_file_middle(hints[1]);

  auto db = bytecask::DB::open(db_path, {.fail_recovery_on_crc_errors = false});
  for (int i = 0; i < 8 && db.vacuum(); ++i) {
  }

  CHECK(std::filesystem::exists(victim_data));
  CHECK(get_str(db, to_bytes("key_first")) == "v1");
  CHECK(get_str(db, to_bytes("key_second")) == "v2");
  CHECK(get_str(db, to_bytes("key_third")) == "v3");
}

// ---------------------------------------------------------------------------
// Damage in a data file is detected and refused, never trimmed around. The
// scan that indexes a hint-less file at open and the one vacuum copies a
// sealed file with both stop by throwing: the first entry that fails to parse
// must not read as the end of the file. If it did, open would truncate the
// data file to that point and vacuum would publish a copy without the entries
// after it and unlink the original — each turning one bad entry into the
// permanent loss of every entry behind it.
// ---------------------------------------------------------------------------
TEST_CASE("DB recovery: a damaged hint-less data file refuses to open and is "
          "not truncated",
          "[bytecask][recovery][corruption]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path);
    for (int i = 0; i < 6; ++i)
      db.put({.sync = true}, to_bytes(std::format("k{}", i)),
             to_bytes(std::format("v{}", i)));
  }
  // A clean close leaves the file that was active at shutdown without a
  // hint, so the next open has to scan it.
  REQUIRE(list_hint_files(db_path).empty());
  std::filesystem::path data;
  for (const auto &e : std::filesystem::directory_iterator{db_path})
    if (e.path().extension() == ".data") data = e.path();
  const auto size_before = std::filesystem::file_size(data);

  // k2's key: 2-byte keys and values make every entry 23 bytes, and the
  // key starts after the 15-byte header.
  flip_byte(data, 2 * 23 + 15);

  REQUIRE_THROWS_AS(bytecask::DB::open(db_path), std::runtime_error);
  CHECK(std::filesystem::file_size(data) == size_before);
  CHECK(list_hint_files(db_path).empty());
}

TEST_CASE("DB vacuum: a damaged sealed file is not compacted away",
          "[bytecask][vacuum][corruption]") {
  TempDir td;
  const auto db_path = td.path / "db";
  const bytecask::Options opts{.max_file_bytes = 190};
  std::filesystem::path victim;
  {
    auto db = bytecask::DB::open(db_path, opts);
    for (int i = 0; i < 8; ++i) {
      db.put({.sync = true}, to_bytes(std::format("a{}", i)),
             to_bytes(std::format("v{}", i)));
      // Only one data file exists until the first rotation.
      if (i == 0)
        for (const auto &e : std::filesystem::directory_iterator{db_path})
          if (e.path().extension() == ".data") victim = e.path();
    }
    // Overwrite half of them so the first file qualifies for compaction.
    for (int i = 0; i < 4; ++i)
      db.put({.sync = true}, to_bytes(std::format("a{}", i)), to_bytes("xx"));
  }
  // The sealed file's hint is written by a background worker after rotation,
  // which would otherwise race the damage below: it scans the same file and
  // could meet the flipped byte first. A close writes it synchronously, so
  // from here on the file is indexed and nothing else reads it.
  REQUIRE(std::filesystem::exists(
      db_path / (victim.stem().string() + ".hint")));
  {
    auto db = bytecask::DB::open(db_path, opts);
    const auto size_before = std::filesystem::file_size(victim);
    flip_byte(victim, 5 * 23 + 15);  // a5's key, a live entry

    CHECK_THROWS_AS(db.vacuum({.fragmentation_threshold = 0.1}),
                    std::runtime_error);
    CHECK(std::filesystem::exists(victim));
    CHECK(std::filesystem::file_size(victim) == size_before);
  }

  // The entries behind the damaged one are still indexed and still read.
  auto db = bytecask::DB::open(db_path, opts);
  CHECK(get_str(db, to_bytes("a4")) == "v4");
  CHECK(get_str(db, to_bytes("a6")) == "v6");
  CHECK(get_str(db, to_bytes("a7")) == "v7");
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
        bytecask::WritePlan plan;
        for (int b = 0; b < batch_size; ++b) {
          if (std::uniform_int_distribution<int>(0, 3)(gen) == 0) {
            auto key = rand_key();
            plan.del(to_bytes(key));
            oracle.erase(key);
          } else {
            auto key = rand_key();
            auto val = rand_value();
            plan.put(to_bytes(key), to_bytes(val));
            oracle[key] = val;
          }
        }
        (void)db.apply_batch({}, std::move(plan));
      }
    }
  }
  // DB is closed — all files sealed, hints flushed via background worker.

  // Helper: collect all (key, value) from a DB into a map.
  auto collect = [](bytecask::DB &db) {
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({})) {
      kv[to_string(entry.key)] = to_string(entry.value);
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
    std::vector<std::tuple<std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes,
                        fs.min_sequence, fs.max_sequence);
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
  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> serial_stats_vals;
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

#ifndef __EMSCRIPTEN__
  // The pool serves every value read below, so this proves recovery and
  // pool-backed reads agree with the oracle together — not just that the
  // key directory was rebuilt.
  SECTION("parallel recovery through the buffer pool (2 workers)") {
    const auto p = td.path / "pool";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 2,
                                     .io_backend = bytecask::IoBackend::BufferPool,
                                     .buffer_pool = {.capacity_bytes = 1 << 20}});
    verify("parallel/2/pool", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }
#endif
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
        bytecask::WritePlan plan;
        for (int b = 0; b < batch_size; ++b) {
          if (std::uniform_int_distribution<int>(0, 4)(gen) == 0) {
            auto key = rand_key();
            plan.del(to_bytes(key));
            oracle.erase(key);
          } else {
            auto key = rand_key();
            auto val = rand_value();
            plan.put(to_bytes(key), to_bytes(val));
            oracle[key] = val;
          }
        }
        (void)db.apply_batch({}, std::move(plan));
      }
    }
  }

  auto collect = [](bytecask::DB &db) {
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({})) {
      kv[to_string(entry.key)] = to_string(entry.value);
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
    std::vector<std::tuple<std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes,
                        fs.min_sequence, fs.max_sequence);
    }
    std::ranges::sort(vals);
    return vals;
  };

  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> serial_stats_vals;
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

#ifndef __EMSCRIPTEN__
  // The pool serves every value read below, so this proves recovery and
  // pool-backed reads agree with the oracle together — not just that the
  // key directory was rebuilt.
  SECTION("parallel through the buffer pool (4 workers)") {
    const auto p = td.path / "pool";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 4,
                                     .io_backend = bytecask::IoBackend::BufferPool,
                                     .buffer_pool = {.capacity_bytes = 1 << 20}});
    verify("parallel/4/pool", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }
#endif
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
    for (auto &entry : db.iter_from({})) {
      kv[to_string(entry.key)] = to_string(entry.value);
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
    std::vector<std::tuple<std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes,
                        fs.min_sequence, fs.max_sequence);
    }
    std::ranges::sort(vals);
    return vals;
  };

  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }

  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> serial_stats_vals;
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

#ifndef __EMSCRIPTEN__
  // The pool serves every value read below, so this proves recovery and
  // pool-backed reads agree with the oracle together — not just that the
  // key directory was rebuilt.
  SECTION("parallel through the buffer pool (3 workers)") {
    const auto p = td.path / "pool";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 3,
                                     .io_backend = bytecask::IoBackend::BufferPool,
                                     .buffer_pool = {.capacity_bytes = 1 << 20}});
    verify("parallel/3/pool", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }
#endif
}

// ---------------------------------------------------------------------------
// Model-based recovery: many keys per worker, with range deletes.
//
// The other model tests rotate on every write, so each worker ends up with a
// handful of keys. This one gives every worker a tree several levels deep
// spanning the whole key space, which is what a real recovery looks like and
// what a range-partitioned merge needs in order to have more than one range:
// the workers' key ranges overlap almost completely, tombstones and range
// tombstones cross worker boundaries, and the winning entry for a key is as
// likely to come from one worker as another.
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: wide workers with range deletes",
          "[bytecask][recovery][parallel][model]") {
  std::mt19937 gen(24680);

  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;

  // Keys interleave five prefixes so that no file holds a contiguous slice.
  static constexpr std::array prefixes = {"alpha:", "bravo:", "delta:",
                                          "gamma:", "omega:"};
  auto key_at = [&](int i) {
    return std::format("{}{:05d}",
                       prefixes[static_cast<std::size_t>(i) % prefixes.size()],
                       i);
  };

  {
    // 16 KiB files: a dozen files, each holding enough keys that a worker's
    // tree is several leaves deep and so has separators to offer.
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 16 * 1024});

    // The setup is not what is under test and the close below seals and
    // flushes everything, so it does not pay for an fdatasync per write.
    const bytecask::WriteOptions wo{.sync = false};

    for (int i = 0; i < 4000; ++i) {
      auto key = key_at(i);
      auto val = std::format("v{:06d}", i);
      db.put(wo, to_bytes(key), to_bytes(val));
      oracle[key] = val;
    }
    // Overwrite a third of them, so the winner for a key sits in a later file
    // than the one the round-robin gave the same worker.
    for (int i = 0; i < 4000; i += 3) {
      auto key = key_at(i);
      auto val = std::format("w{:06d}", i);
      db.put(wo, to_bytes(key), to_bytes(val));
      oracle[key] = val;
    }
    // Point deletes scattered across the space.
    for (int i = 0; i < 4000; i += 7) {
      auto key = key_at(i);
      std::ignore = db.del(wo, to_bytes(key));
      oracle.erase(key);
    }
    // Range deletes: two whole prefixes' worth of sub-ranges, so the
    // tombstones span keys that several workers hold.
    for (const auto *bounds : {"bravo:00600", "gamma:02000"}) {
      const std::string from{bounds};
      const auto to = from.substr(0, 6) + "02600";
      db.del_range(wo, to_bytes(from), to_bytes(to));
      for (auto it = oracle.lower_bound(from); it != oracle.end();)
        it = it->first < to ? oracle.erase(it) : oracle.end();
    }
    // Writes after the range deletes must survive them.
    for (int i = 0; i < 100; ++i) {
      auto key = std::format("bravo:0{:04d}", 600 + i * 5);
      auto val = std::format("late{:04d}", i);
      db.put(wo, to_bytes(key), to_bytes(val));
      oracle[key] = val;
    }
    std::ignore = gen;
  }

  auto collect = [](bytecask::DB &db) {
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({}))
      kv[to_string(entry.key)] = to_string(entry.value);
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
    std::vector<std::tuple<std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats())
      vals.emplace_back(fs.live_bytes, fs.total_bytes,
                        fs.min_sequence, fs.max_sequence);
    std::ranges::sort(vals);
    return vals;
  };

  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path})
    if (e.path().extension() == ".data") ++data_file_count;
  REQUIRE(data_file_count > 8);

  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> serial_stats_vals;
  {
    const auto p = td.path / "serial_baseline";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_stats(db);
  }

  // Every thread count changes both the partitioning and the number of
  // ranges the merge runs over; all must land on the same directory.
  for (const unsigned threads : {1U, 2U, 3U, 5U, 8U}) {
    DYNAMIC_SECTION("recovery_threads = " << threads) {
      const auto p = td.path / std::format("t{}", threads);
      std::filesystem::copy(db_path, p,
                            std::filesystem::copy_options::recursive);
      auto db = bytecask::DB::open(p, {.recovery_threads = threads});
      verify(std::format("threads/{}", threads), collect(db));
      CHECK(collect_stats(db) == serial_stats_vals);
    }
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

TEST_CASE("DB group commit apply_batch conflict returns false",
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

  CHECK_FALSE(db.apply_batch({}, std::move(plan)));

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

  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("va"));
  plan.put(to_bytes("b"), to_bytes("vb"));
  (void)db.apply_batch({}, std::move(plan));

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

  bytecask::WritePlan plan;
  plan.del(to_bytes("k"));
  (void)db.apply_batch({}, std::move(plan));

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
// The active file is preallocated and zero-filled to max_file_bytes so that
// commits never wait on extent conversion. Once a file is sealed — by
// rotation, clean close, or recovery after a crash — its physical size must
// be its logical size again: file_size is what seeds total_bytes at recovery.
// ---------------------------------------------------------------------------
namespace {

// (physical size, total_bytes) for every file in the state except the
// active one, plus the active file's physical and logical sizes.
struct SizeReport {
  std::vector<std::pair<std::uintmax_t, std::uint64_t>> sealed;
  std::uintmax_t active_physical{0};
  std::uint64_t active_logical{0};
};

auto size_report(const bytecask::DB &db) -> SizeReport {
  SizeReport r;
  const auto s = db.engine_state();
  const auto stats = db.file_stats();
  for (const auto [id, file] : s->files) {
    const auto physical = std::filesystem::file_size(file->path());
    if (id == s->active_file_id) {
      r.active_physical = physical;
      r.active_logical = file->size();
    } else {
      r.sealed.emplace_back(physical, stats.at(id).total_bytes);
    }
  }
  return r;
}

}  // namespace

TEST_CASE("Preallocated tail: sealed files shrink to their logical size",
          "[bytecask][filestats]") {
  const auto io_backend =
      GENERATE(bytecask::IoBackend::Pread, bytecask::IoBackend::Mmap,
               bytecask::IoBackend::BufferPool);
  CAPTURE(static_cast<int>(io_backend));
  constexpr std::uint64_t kCapacity = 4096;
  bytecask::Options opts{.max_file_bytes = kCapacity,
                         .io_backend = io_backend};
  if (io_backend == bytecask::IoBackend::BufferPool) {
    opts.buffer_pool.capacity_bytes = 1024 * 1024;
  }

  TempDir td;
  const auto db_path = td.path / "db";
  const std::string big(1500, 'v');

  std::uint64_t pre_total = 0;
  {
    auto db = bytecask::DB::open(db_path, opts);
    // Three 1.5 KiB values cross the 4 KiB threshold: at least one rotation.
    db.put({}, to_bytes("a"), to_bytes(big));
    db.put({}, to_bytes("b"), to_bytes(big));
    db.put({}, to_bytes("c"), to_bytes(big));

    const auto r = size_report(db);
    REQUIRE(!r.sealed.empty());
    for (const auto &[physical, total] : r.sealed) CHECK(physical == total);
    // The active file still carries its preallocated tail.
    CHECK(r.active_physical == kCapacity);
    CHECK(r.active_logical < kCapacity);
    for (const auto &[id, fs] : db.file_stats()) pre_total += fs.total_bytes;

    // Crash: snapshot the data files while the engine is live. Only .data
    // files — hints are derived and regenerated at open, and the background
    // hint writer may be renaming a .hint.tmp under a recursive copy. The
    // copied active file has its 4 KiB tail; recovery must drop it.
    const auto crash_path = td.path / "crash";
    std::filesystem::create_directories(crash_path);
    for (const auto &e : std::filesystem::directory_iterator{db_path}) {
      if (e.path().extension() == ".data")
        std::filesystem::copy_file(e.path(), crash_path / e.path().filename());
    }
    {
      auto crashed = bytecask::DB::open(crash_path, opts);
      const auto cr = size_report(crashed);
      for (const auto &[physical, total] : cr.sealed) CHECK(physical == total);
      std::uint64_t post_total = 0;
      for (const auto &[id, fs] : crashed.file_stats()) post_total += fs.total_bytes;
      CHECK(post_total == pre_total);
      bytecask::Bytes out;
      CHECK(crashed.get({}, to_bytes("c"), out));
    }
  }

  // Clean close drops the active file's tail: on disk, every file is now
  // exactly its logical size, so the physical bytes add up to total_bytes.
  std::uint64_t on_disk = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      on_disk += std::filesystem::file_size(e.path());
  }
  CHECK(on_disk == pre_total);

  auto db2 = bytecask::DB::open(db_path, opts);
  const auto r2 = size_report(db2);
  for (const auto &[physical, total] : r2.sealed) CHECK(physical == total);
  std::uint64_t post_total = 0;
  for (const auto &[id, fs] : db2.file_stats()) post_total += fs.total_bytes;
  CHECK(post_total == pre_total);
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
// A kill inside vacuum's publish window — after the compacted file is renamed
// into place and before the source is unlinked — leaves two files holding the
// same entries under the same sequences. Recovery undoes the vacuum: it
// deletes the compacted copy, keeps the source, and opens with every key.
// See docs/vacuum_crash_recovery_design.md.
// ---------------------------------------------------------------------------
namespace {

auto data_files_in(const std::filesystem::path &dir)
    -> std::set<std::filesystem::path> {
  std::set<std::filesystem::path> out;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() == ".data") out.insert(e.path().filename());
  }
  return out;
}

auto collect_kv(bytecask::DB &db) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  for (auto &[key, value] : db.iter_from({})) {
    out[to_string(key)] = to_string(value);
  }
  return out;
}

auto sequence_ranges(bytecask::DB &db)
    -> std::vector<std::pair<std::uint64_t, std::uint64_t>> {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
  for (const auto &[fid, fs] : db.file_stats()) {
    if (fs.min_sequence > 0) out.emplace_back(fs.min_sequence, fs.max_sequence);
  }
  std::ranges::sort(out);
  return out;
}

auto disjoint(const std::vector<std::pair<std::uint64_t, std::uint64_t>> &r)
    -> bool {
  for (std::size_t i = 1; i < r.size(); ++i) {
    if (r[i - 1].second >= r[i].first) return false;
  }
  return true;
}

} // namespace

TEST_CASE("recovery undoes a vacuum killed before the source was unlinked",
          "[vacuum][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;
  std::set<std::filesystem::path> before_vacuum;
  std::filesystem::path copy;
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 4096});
    for (int i = 0; i < 200; ++i) {
      auto k = std::format("k{:04d}", i);
      auto v = std::format("v{:04d}", i) + std::string(40, 'x');
      db.put({.sync = false}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
    }
    // Garbage in the first file, which keeps live keys too: vacuum compacts
    // it rather than removing it.
    for (int i = 0; i < 40; i += 2) {
      auto k = std::format("k{:04d}", i);
      auto v = std::format("w{:04d}", i) + std::string(40, 'y');
      db.put({.sync = false}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
    }
    (void)db.del({.sync = false}, to_bytes("k0007"));
    oracle.erase("k0007");

    before_vacuum = data_files_in(db_path);
    {
      bytecask::testing::ScopedFaultInjector fi{"io_vacuum_compact_unlink"};
      REQUIRE_THROWS_AS(db.vacuum({.fragmentation_threshold = 0.0}),
                        std::system_error);
    }
    std::vector<std::filesystem::path> added;
    std::ranges::set_difference(data_files_in(db_path), before_vacuum,
                                std::back_inserter(added));
    REQUIRE(added.size() == 1);
    copy = added.front();
  }
  // The window's on-disk state: the compacted copy and every file that
  // existed before vacuum, its source among them.
  const auto on_disk = data_files_in(db_path);
  REQUIRE(on_disk.contains(copy));
  REQUIRE(std::ranges::includes(on_disk, before_vacuum));

  auto check_undone = [&](bytecask::DB &db, const std::filesystem::path &p) {
    CHECK(collect_kv(db) == oracle);
    const auto after = data_files_in(p);
    CHECK_FALSE(after.contains(copy));
    CHECK(std::ranges::includes(after, before_vacuum));
    CHECK(disjoint(sequence_ranges(db)));
  };

  std::vector<std::pair<std::uint64_t, std::uint64_t>> serial_ranges;
  {
    const auto p = td.path / "serial";
    std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 4096, .recovery_threads = 1});
    check_undone(db, p);
    serial_ranges = sequence_ranges(db);

    // Each sequence once: the copy's duplicates are gone from changes_since
    // too (#129).
    auto snap = db.snapshot();
    std::vector<std::uint64_t> seqs;
    for (const auto &entry : db.changes_since(snap, 0)) {
      seqs.push_back(entry.sequence);
    }
    auto sorted = seqs;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());

    // The vacuum that was undone runs again.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));
    CHECK(collect_kv(db) == oracle);
  }
  // Files go to workers round-robin, so worker counts vary in whether the two
  // files meet inside one worker's build or in the merge across workers.
  const auto n_files = static_cast<unsigned>(on_disk.size());
  for (unsigned w = 2; w <= n_files; ++w) {
    DYNAMIC_SECTION("parallel recovery agrees with serial, " << w << " workers") {
      const auto p = td.path / std::format("parallel{}", w);
      std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
      auto db = bytecask::DB::open(p, {.max_file_bytes = 4096, .recovery_threads = w});
      check_undone(db, p);
      CHECK(sequence_ranges(db) == serial_ranges);
    }
  }
}

// Deleting the smaller file is safe on the one thing recovery checks — that
// its entries are all in the larger one. A copy of a prefix of a file passes
// that check; deleting the larger file would lose every write after the copy.
TEST_CASE("recovery keeps the full file over a copy of its prefix",
          "[vacuum][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  const auto prefix_name =
      std::filesystem::path{"data_20000101000000_0000000000000000_V01.data"};
  std::map<std::string, std::string> oracle;
  {
    auto db = bytecask::DB::open(db_path);
    for (int i = 0; i < 20; ++i) {
      auto k = std::format("k{:02d}", i);
      auto v = std::format("v{:02d}", i);
      db.put({}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
      if (i == 9) {
        // Copy the active file's first ten entries: its logical size, not
        // its preallocated one.
        const auto s = db.engine_state();
        const auto &active = **s->files.get(s->active_file_id);
        const auto len = s->file_stats.get(s->active_file_id)->total_bytes;
        std::ifstream in{active.path(), std::ios::binary};
        std::string bytes(len, '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(len));
        std::ofstream out{td.path / prefix_name, std::ios::binary};
        out.write(bytes.data(), static_cast<std::streamsize>(len));
      }
    }
  }
  std::filesystem::copy_file(td.path / prefix_name, db_path / prefix_name);
  const auto before = data_files_in(db_path);

  auto db = bytecask::DB::open(db_path);
  CHECK(collect_kv(db) == oracle);
  auto kept = before;
  kept.erase(prefix_name);
  const auto after = data_files_in(db_path);
  CHECK_FALSE(after.contains(prefix_name));
  CHECK(std::ranges::includes(after, kept));
  CHECK(disjoint(sequence_ranges(db)));
}

// The same key under the same sequence with a different value of the same
// size is two writes, not a copy. Recovery refuses it and deletes nothing.
TEST_CASE("recovery refuses two different writes under one sequence",
          "[vacuum][recovery]") {
  TempDir td;
  const auto a = td.path / "a";
  const auto b = td.path / "b";
  {
    auto db = bytecask::DB::open(a);
    db.put({}, to_bytes("key"), to_bytes("aaaa"));
  }
  {
    auto db = bytecask::DB::open(b);
    db.put({}, to_bytes("key"), to_bytes("bbbb"));
  }
  for (const auto &e : std::filesystem::directory_iterator{b}) {
    const auto ext = e.path().extension();
    if (ext == ".data" || ext == ".hint") {
      std::filesystem::copy_file(e.path(), a / e.path().filename());
    }
  }
  const auto before = data_files_in(a);
  REQUIRE(before.size() >= 2);

  for (unsigned w : {1u, 2u}) {
    INFO("recovery_threads = " << w);
    try {
      auto db = bytecask::DB::open(a, {.recovery_threads = w});
      FAIL("open must refuse the directory");
    } catch (const std::runtime_error &e) {
      CHECK(std::string_view{e.what()}.find(
                "neither is a compacted copy") != std::string_view::npos);
    }
    CHECK(data_files_in(a) == before);
  }
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
    CHECK_FALSE(db.contains_key({}, to_bytes("gone")));
  }

  // Reopen to verify tombstone survives recovery.
  auto db2 = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
  CHECK_FALSE(db2.contains_key({}, to_bytes("gone")));
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

    // Sealed file with "a"="1" has 100% dead entries → vacuum_remove_file.
    // Any sealed file with live entries → vacuum_compact_file (sealed→sealed).
    // vacuum() drains pending hint writes internally before selecting target.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

    CHECK(to_string(*get_val(db, to_bytes("a"))) == "2");
    // db destroyed here — background worker drains, hints written
  }

  // Verify recovery after vacuum.
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
  {
    int spins = 0;
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
      REQUIRE(++spins < 200);  // vacuum must converge, not spin
    }
  }

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
  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("new_a"));
  plan.put(to_bytes("b"), to_bytes("new_b"));
  (void)db.apply_batch({}, std::move(plan));

  // Vacuum — the file with the batch should be compacted.
  {
    int spins = 0;
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
      REQUIRE(++spins < 200);  // vacuum must converge, not spin
    }
  }

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

  bytecask::WritePlan plan;
  plan.put(to_bytes("keep"), to_bytes("updated"));
  plan.del(to_bytes("gone"));
  (void)db.apply_batch({}, std::move(plan));

  // Vacuum until stable. Convergence is the assertion: the compacted file
  // keeps a tombstone, which can never be live_bytes, so at threshold 0 it
  // stays eligible forever unless vacuum declines a file it cannot shrink.
  int spins = 0;
  while (db.vacuum({.fragmentation_threshold = 0.0})) {
    REQUIRE(++spins < 200);
  }

  auto vk = get_val(db, to_bytes("keep"));
  REQUIRE(vk.has_value());
  CHECK(to_string(*vk) == "updated");
  CHECK_FALSE(db.contains_key({}, to_bytes("gone")));
}

// ---------------------------------------------------------------------------
// A compacted file's total_bytes must equal its size on disk, because that is
// what recovery seeds total_bytes from. Batch markers are preserved by
// compaction, so their bytes have to be counted like every other entry's —
// otherwise file_stats() reports one number before a restart and another
// after, and published offsets fall outside the extent the engine believes
// the file has.
// ---------------------------------------------------------------------------
TEST_CASE("vacuum keeps total_bytes equal to the compacted file's size",
          "[vacuum][batch]") {
  TempDir td;
  auto db_path = td.path / "db";

  auto stats_shape = [](const bytecask::DB &db) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    for (const auto &[fid, fs] : db.file_stats()) {
      if (fs.total_bytes > 0) out.emplace_back(fs.live_bytes, fs.total_bytes);
    }
    std::ranges::sort(out);
    return out;
  };

  std::vector<std::pair<std::uint64_t, std::uint64_t>> before;
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("old_a"));
    db.put({}, to_bytes("b"), to_bytes("old_b"));

    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("new_a"));
    plan.put(to_bytes("b"), to_bytes("new_b"));
    (void)db.apply_batch({}, std::move(plan));

    int spins = 0;
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
      REQUIRE(++spins < 10);
    }
    before = stats_shape(db);
  }

  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
  // Recovery computes total_bytes from the file's actual size. If vacuum
  // undercounted, these disagree by kHeaderSize + kCrcSize per marker.
  CHECK(stats_shape(db) == before);

  // And the file must not be re-compacted: there is nothing in it to reclaim.
  CHECK_FALSE(db.vacuum({.fragmentation_threshold = 0.0}));
}

// ---------------------------------------------------------------------------
// vacuum unlinks stale files immediately; snapshot reads still work via open fd
//
// After vacuum compacts a file, its .data file is unlinked from the
// directory. A snapshot that references the old file can still read via
// the open fd (POSIX: pread succeeds on unlinked files).
// ---------------------------------------------------------------------------
TEST_CASE("vacuum unlinks stale file immediately, snapshot reads via open fd",
          "[vacuum]") {
  TempDir td;
  auto db_path = td.path / "db";
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

  db.put({}, to_bytes("k"), to_bytes("v1"));
  db.put({}, to_bytes("k"), to_bytes("v2")); // kills v1 → file qualifies

  // Count .data files before vacuum.
  auto count_data_files = [&]() {
    int n = 0;
    for (const auto &e : std::filesystem::directory_iterator{db_path}) {
      if (e.path().extension() == ".data") ++n;
    }
    return n;
  };
  const int files_before = count_data_files();

  {
    // Take a snapshot that references the old file.
    auto snap = db.snapshot();

    // Vacuum compacts the dead file and unlinks it immediately.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));

    // The old .data file is gone from the directory.
    CHECK(count_data_files() < files_before);

    // The key is still readable from the live DB.
    auto v = get_val(db, to_bytes("k"));
    REQUIRE(v.has_value());
    CHECK(to_string(*v) == "v2");

    // The snapshot can still read via its open fd (POSIX guarantee).
    bytecask::Bytes snap_out;
    CHECK(snap.get({}, to_bytes("k"), snap_out));
  }

  // DB still consistent after snapshot is dropped.
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
  REQUIRE(snap.get({}, to_bytes("k"), out));
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

  CHECK(snap.contains_key({}, to_bytes("k")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k")));
}

// Snapshot::get returns false for a key absent at snapshot time.
TEST_CASE("Snapshot get returns false for absent key", "[snapshot]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("v"));

  bytecask::Bytes out;
  CHECK_FALSE(snap.get({}, to_bytes("k"), out));
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
  for (const auto &entry : snap.iter_from({})) {
    keys.push_back(to_string(entry.key));
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
  for (const auto &k : snap.keys_from({})) {
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
  for (auto &entry : db.riter_from(ro)) {
    keys.push_back(to_string(entry.key));
    values.push_back(to_string(entry.value));
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
  for (auto &entry : db.riter_from({}, to_bytes("banana"))) {
    keys.push_back(to_string(entry.key));
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
  for (auto &entry : db.riter_from({}, to_bytes("d"))) {
    keys.push_back(to_string(entry.key));
  }

  REQUIRE(keys.size() == 2);
  CHECK(keys[0] == "c");
  CHECK(keys[1] == "a");
}

TEST_CASE("DB riter_from with a key that is a strict prefix of an existing "
          "key starts at the predecessor",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // "ab" is a strict prefix of "abc" and has no entry of its own; the
  // reverse scan from "ab" must begin at "aa", never at "abc" (> "ab").
  db.put({}, to_bytes("aa"), to_bytes("1"));
  db.put({}, to_bytes("abc"), to_bytes("2"));
  db.put({}, to_bytes("abd"), to_bytes("3"));

  SECTION("entry iterator") {
    std::vector<std::string> keys;
    for (auto &entry : db.riter_from({}, to_bytes("ab"))) {
      keys.push_back(to_string(entry.key));
    }
    REQUIRE(keys == std::vector<std::string>{"aa"});
  }

  SECTION("key iterator agrees") {
    std::vector<std::string> keys;
    for (auto &k : db.rkeys_from({}, to_bytes("ab"))) {
      keys.push_back(to_string(k));
    }
    REQUIRE(keys == std::vector<std::string>{"aa"});
  }

  SECTION("bound that diverges below a node prefix") {
    // "ab\x00" < "abc": lower bound is "abc", predecessor is "aa".
    std::vector<std::string> keys;
    for (auto &entry : db.riter_from({}, to_bytes(std::string_view{"ab\0", 3}))) {
      keys.push_back(to_string(entry.key));
    }
    REQUIRE(keys == std::vector<std::string>{"aa"});
  }

  SECTION("exact key present still starts inclusively") {
    db.put({}, to_bytes("ab"), to_bytes("4"));
    std::vector<std::string> keys;
    for (auto &entry : db.riter_from({}, to_bytes("ab"))) {
      keys.push_back(to_string(entry.key));
    }
    REQUIRE(keys == std::vector<std::string>{"ab", "aa"});
  }
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
  for (auto &entry : db.riter_from({})) {
    keys.push_back(to_string(entry.key));
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
  for (const auto &entry : snap.riter_from({})) {
    keys.push_back(to_string(entry.key));
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
  for (const auto &k : snap.rkeys_from({})) {
    keys.push_back(to_string(k));
  }
  CHECK(keys == std::vector<std::string>{"b", "a"});
}

// ---------------------------------------------------------------------------
// apply_batch tests
// ---------------------------------------------------------------------------

// No conflict: plan applies when no concurrent write touched the keys.
TEST_CASE("apply_batch succeeds with no conflict", "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));

  const auto result = get_val(db, to_bytes("k"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "v1");
}

// W-W conflict: key modified after snapshot — returns false.
TEST_CASE("apply_batch returns false on modified key",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("interleaved"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// Conflict: key appeared after snapshot — returns false.
TEST_CASE("apply_batch returns false when key appeared",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot(); // "k" absent at snapshot time
  db.put({}, to_bytes("k"), to_bytes("appeared"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// Conflict: key deleted after snapshot — returns false.
TEST_CASE("apply_batch returns false when key deleted",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  (void)db.del({}, to_bytes("k"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// No conflict on disjoint keys: concurrent write touches "a", plan writes "b".
TEST_CASE("apply_batch no conflict on disjoint keys", "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("v0"));
  db.put({}, to_bytes("b"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("a"), to_bytes("concurrent"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("b"), to_bytes("v1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));

  const auto result = get_val(db, to_bytes("b"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "v1");
}

// Empty plan is a no-op and returns true.
TEST_CASE("apply_batch empty plan is a no-op", "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  REQUIRE(db.apply_batch({}, std::move(plan)));

  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v0");
}

// ---------------------------------------------------------------------------
// WritePlan guard tests
// ---------------------------------------------------------------------------

// ensure_present succeeds when key exists.
TEST_CASE("apply_batch ensure_present passes when key exists",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v1");
}

// ensure_present fails when key is absent.
TEST_CASE("apply_batch ensure_present fails when key absent",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// ensure_absent succeeds when key does not exist.
TEST_CASE("apply_batch ensure_absent passes when key absent",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v1");
}

// ensure_absent fails when key exists.
TEST_CASE("apply_batch ensure_absent fails when key exists",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// ensure_unchanged succeeds when no concurrent writes.
TEST_CASE("apply_batch ensure_unchanged passes without modification",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "v1");
}

// ensure_unchanged fails when key modified since snapshot.
TEST_CASE("apply_batch ensure_unchanged fails on modification",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("concurrent"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// ensure_unchanged on absent key — passes when still absent.
TEST_CASE("apply_batch ensure_unchanged passes for absent key staying absent",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("nonexistent"));
  REQUIRE(db.apply_batch({}, std::move(plan)));
}

// ensure_unchanged on absent key — fails when key appeared.
TEST_CASE("apply_batch ensure_unchanged fails when absent key appeared",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("appeared"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// ensure_range_unchanged succeeds when no keys in range were modified.
TEST_CASE("apply_batch ensure_range_unchanged passes when range clean",
          "[apply_batch]") {
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
  REQUIRE(db.apply_batch({}, std::move(plan)));
}

// ensure_range_unchanged fails when a key in range was modified.
TEST_CASE("apply_batch ensure_range_unchanged fails on in-range modification",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("b"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("b"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// ensure_range_unchanged fails when a key in range was inserted.
TEST_CASE("apply_batch ensure_range_unchanged fails on in-range insertion",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto snap = db.snapshot();
  db.put({}, to_bytes("b"), to_bytes("inserted"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// ensure_range_unchanged fails when a key in range was deleted.
TEST_CASE("apply_batch ensure_range_unchanged fails on in-range deletion",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("b"), to_bytes("v0"));

  auto snap = db.snapshot();
  (void)db.del({}, to_bytes("b"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  REQUIRE_FALSE(db.apply_batch({}, std::move(plan)));
}

// Guards-only plan with no writes — validates consistency without disk I/O.
TEST_CASE("apply_batch guards-only plan with no writes", "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  REQUIRE(db.apply_batch({}, std::move(plan)));
}

// Contradictory guards throw std::logic_error at build time.
TEST_CASE("WritePlan contradictory guards throw logic_error",
          "[apply_batch]") {
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
// Single-op optimization: a 1-write apply_batch with snapshot writes no
// BulkBegin/BulkEnd markers, so total_bytes matches an equivalent plain put().
TEST_CASE("apply_batch single-op with snapshot writes no markers",
          "[apply_batch]") {
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
    (void)db.apply_batch({}, std::move(plan));
  });

  CHECK(batch_if_bytes == put_bytes);
}

// Single-op optimization: a 1-entry apply_batch writes no markers.
TEST_CASE("apply_batch single-op writes no markers", "[apply_batch]") {
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
    bytecask::WritePlan b;
    b.put(to_bytes("k"), to_bytes("value"));
    (void)db.apply_batch({}, std::move(b));
  });

  CHECK(batch_bytes == put_bytes);
}

TEST_CASE("apply_batch duplicate key in same plan does not conflict",
          "[apply_batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("v0"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("a"), to_bytes("t0"));
  plan.put(to_bytes("a"), to_bytes("t1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));

  auto result = get_val(db, to_bytes("a"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "t1");
}

TEST_CASE("apply_batch group commit: second slot conflicts on existing key",
          "[apply_batch][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = false}, to_bytes("a"), to_bytes("v0"));

  // Two snapshots at the same point — identical state.
  auto snapA = db.snapshot();
  auto snapB = db.snapshot();

  bytecask::WritePlan planA{std::move(snapA)};
  planA.put(to_bytes("a"), to_bytes("fromA"));

  bytecask::WritePlan planB{std::move(snapB)};
  planB.put(to_bytes("a"), to_bytes("fromB"));

  // Sync point: force both slots into the same group commit batch.
  std::mutex mu;
  std::condition_variable cv;
  bool leader_ready = false;

  db.test_write_group().on_leader_start_ = [&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      leader_ready = true;
      cv.notify_all();
    }
    // Spin until thread B's slot is also in the queue (2 total).
    db.test_write_group().wait_for_queue_size(2);
  };

  std::optional<bytecask::CommitResult> resultA;
  std::optional<bytecask::CommitResult> resultB;

  // Thread A becomes leader, blocks in hook until thread B enqueues.
  std::thread tA([&] {
    resultA = db.apply_batch({.sync = false}, std::move(planA));
  });

  // Thread B waits for leader, then enqueues via apply_batch.
  std::thread tB([&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      cv.wait(lk, [&] { return leader_ready; });
    }
    resultB = db.apply_batch({.sync = false}, std::move(planB));
  });

  tA.join();
  tB.join();

  db.test_write_group().on_leader_start_ = nullptr;

  CHECK(resultA.has_value());
  CHECK_FALSE(resultB.has_value());

  auto val = get_val(db, to_bytes("a"));
  REQUIRE(val.has_value());
  CHECK(to_string(*val) == "fromA");
}

TEST_CASE("apply_batch group commit: second slot conflicts on new key",
          "[apply_batch][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Key "a" does not exist. Both snapshots see absence.
  auto snapA = db.snapshot();
  auto snapB = db.snapshot();

  bytecask::WritePlan planA{std::move(snapA)};
  planA.put(to_bytes("a"), to_bytes("fromA"));

  bytecask::WritePlan planB{std::move(snapB)};
  planB.put(to_bytes("a"), to_bytes("fromB"));

  std::mutex mu;
  std::condition_variable cv;
  bool leader_ready = false;

  db.test_write_group().on_leader_start_ = [&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      leader_ready = true;
      cv.notify_all();
    }
    db.test_write_group().wait_for_queue_size(2);
  };

  std::optional<bytecask::CommitResult> resultA;
  std::optional<bytecask::CommitResult> resultB;

  std::thread tA([&] {
    resultA = db.apply_batch({.sync = false}, std::move(planA));
  });

  std::thread tB([&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      cv.wait(lk, [&] { return leader_ready; });
    }
    resultB = db.apply_batch({.sync = false}, std::move(planB));
  });

  tA.join();
  tB.join();

  db.test_write_group().on_leader_start_ = nullptr;

  CHECK(resultA.has_value());
  CHECK_FALSE(resultB.has_value());

  auto val = get_val(db, to_bytes("a"));
  REQUIRE(val.has_value());
  CHECK(to_string(*val) == "fromA");
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
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("v1"));
    plan.put(to_bytes("b"), to_bytes("v2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
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
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("v1"));
    plan.put(to_bytes("b"), to_bytes("v2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // get — returns pre-degrade data.
  const auto val = get_val(db, to_bytes("x"));
  REQUIRE(val.has_value());
  CHECK(to_string(*val) == "val_x");

  // contains_key — pure in-memory.
  CHECK(db.contains_key({}, to_bytes("x")));
  CHECK_FALSE(db.contains_key({}, to_bytes("a")));

  // snapshot — frozen read-only view.
  auto snap = db.snapshot();
  bytecask::Bytes snap_out;
  CHECK(snap.get({}, to_bytes("x"), snap_out));

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

#ifndef __EMSCRIPTEN__
// resume() truncates the active file while reads stay lock-free. With
// IoBackend::Mmap it must do so without disturbing the mapping: an EntryIterator
// hands out spans into it that stay valid until the next operator++, i.e.
// across the body of the caller's range-for — and resume() can land there.
TEST_CASE("mmap: resume() keeps reader spans valid",
          "[degraded][resume][mmap]") {
  TempDir td;
  constexpr std::uint64_t kCapacity = 1024 * 1024;
  auto db = bytecask::DB::open(
      td.path / "db",
      {.max_file_bytes = kCapacity, .io_backend = bytecask::IoBackend::Mmap});
  db.put({.sync = false}, to_bytes("k"), to_bytes("mapped_value"));

  // Degrade via an orphaned BulkBegin: the batch's bytes reach the file but
  // are never published, so resume() has to truncate them away.
  {
    bytecask::testing::ScopedFaultInjector fi{2};
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("v1"));
    plan.put(to_bytes("b"), to_bytes("v2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // Reads remain available while degraded. Take an entry and hold it.
  auto range = db.iter_from({}, to_bytes("k"));
  auto it = range.begin();
  REQUIRE(it != std::default_sentinel);
  const auto &entry = *it;
  const auto *addr = entry.value.data();
  REQUIRE(to_string(entry.value) == "mapped_value");

  // A second iterator resolving to the same address proves the value is
  // served from the mapping rather than copied into each iterator's buffer —
  // without it the checks below would hold vacuously.
  auto probe = db.iter_from({}, to_bytes("k"));
  auto probe_it = probe.begin();
  REQUIRE((*probe_it).value.data() == addr);

  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // The span survives resume(): same address, still mapped, same bytes. The
  // bytes are what discriminate here — resume() opens the new active file at
  // the same capacity, so a mapping that took over the address would satisfy
  // both the mapped and address checks while serving zeros from that empty
  // file.
  CHECK(bytecask::testing::is_mapped(addr, kCapacity / 2));
  CHECK(entry.value.data() == addr);
  CHECK(to_string(entry.key) == "k");
  CHECK(to_string(entry.value) == "mapped_value");
}
#endif

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
    REQUIRE_THROWS_AS(db.apply_batch({.sync = true}, std::move(plan)),
                      std::system_error);
  }

  // Key must not be visible — write was not confirmed durable.
  CHECK_FALSE(db.contains_key({}, to_bytes("new_key")));
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
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
                      std::system_error);
  }

  // Key must not be visible — write was not confirmed durable.
  CHECK_FALSE(db.contains_key({}, to_bytes("new_key")));
  // Engine must be degraded — bytes in page cache, key_dir diverges.
  CHECK(db.is_degraded());
  // Pre-existing key must still be visible.
  CHECK(db.contains_key({}, to_bytes("seed")));
  // resume() clears the degraded state.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  // Pre-existing key still visible after resume.
  CHECK(db.contains_key({}, to_bytes("seed")));
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
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));

  // Subsequent writes succeed after resume().
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key({}, to_bytes("k3")));
}

TEST_CASE("resume() replays unpublished entries from active file",
          "[degraded][resume]") {
  TempDir td;

  // Use a large max_file_bytes to keep everything on one active file.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1'000'000});

  // k1 committed normally — baseline.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  CHECK(db.contains_key({}, to_bytes("k1")));

  // k2 written to page cache but key changes NOT published (class F sync
  // failure). fdatasync fails → engine degrades; k2 bytes in page cache.
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                      std::system_error);
  }
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));
  // F degrades the engine immediately.
  REQUIRE(db.is_degraded());

  // resume() scans the active file. The k2 bytes are in the page cache
  // and pread sees them. resume() replays k2 into key_dir, then syncs
  // (confirming durability), seals the file, and opens a fresh active file.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // k1 was always committed and must still be visible.
  CHECK(db.contains_key({}, to_bytes("k1")));
  // k2 was replayed by resume() from the page-cache bytes.
  CHECK(db.contains_key({}, to_bytes("k2")));

  // Verify actual values.
  auto v = get_val(db, to_bytes("k2"));
  REQUIRE(v.has_value());
  CHECK(to_string(*v) == "v2");

  // Subsequent writes work.
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key({}, to_bytes("k3")));
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
    bytecask::WritePlan plan;
    plan.put(to_bytes("k3"), to_bytes("v3"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
                      bytecask::DbDegraded);
  }

  // apply_batch must throw DbDegraded.
  {
    bytecask::WritePlan plan;
    plan.put(to_bytes("k4"), to_bytes("v4"));
    REQUIRE_THROWS_AS(
        (void)db.apply_batch({.sync = false}, std::move(plan)),
        bytecask::DbDegraded);
  }

  // After resume, writes succeed again.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  REQUIRE_NOTHROW(db.put({.sync = false}, to_bytes("k5"), to_bytes("v5")));
  CHECK(db.contains_key({}, to_bytes("k5")));
}

namespace {

// Walks the raw entry headers of a data file and returns each entry's offset.
// Mirrors the on-disk format documented in data_entry.cppm: a 15-byte header
// (u64 sequence, u8 entry_type, u16 key_size, u32 value_size, all little
// endian) followed by key, value and a 4-byte CRC. Stops at the zero-filled
// tail, which a zero sequence identifies.
auto raw_entry_offsets(const std::filesystem::path &data)
    -> std::vector<std::uint64_t> {
  std::ifstream f{data, std::ios::binary};
  const auto size = std::filesystem::file_size(data);
  std::vector<std::uint64_t> offsets;
  for (std::uint64_t off = 0; off + 19 <= size;) {
    std::array<unsigned char, 15> hdr{};
    f.seekg(static_cast<std::streamoff>(off));
    f.read(reinterpret_cast<char *>(hdr.data()), std::ssize(hdr));
    const auto le = [&hdr](std::size_t pos, std::size_t len) -> std::uint64_t {
      std::uint64_t v = 0;
      for (std::size_t i = len; i-- > 0;) v = (v << 8) | hdr[pos + i];
      return v;
    };
    if (le(0, 8) == 0) break;  // zero-filled tail
    offsets.push_back(off);
    off += 15 + le(9, 2) + le(11, 4) + 4;
  }
  return offsets;
}

auto active_data_file(const std::filesystem::path &dir) -> std::filesystem::path {
  std::filesystem::path newest;
  for (const auto &e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == ".data" &&
        (newest.empty() || e.path().filename() > newest.filename())) {
      newest = e.path();
    }
  }
  return newest;
}

// Peak resident set size in bytes, as the kernel records it. Monotonic over
// the process lifetime, so it captures a transient spike that has already
// been freed by the time the caller looks.
auto peak_rss_bytes() -> std::uint64_t {
  std::ifstream f{"/proc/self/status"};
  for (std::string line; std::getline(f, line);) {
    if (line.starts_with("VmHWM:")) {
      return std::stoull(line.substr(6)) * 1024;
    }
  }
  return 0;
}

// Degrades db by failing the fdatasync of a batch whose bytes reached the
// file, and returns the offsets of the active file's entries.
auto degrade_with_unsynced_batch(bytecask::DB &db,
                                 const std::filesystem::path &dir)
    -> std::vector<std::uint64_t> {
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    bytecask::WritePlan plan;
    plan.put(to_bytes("b1"), to_bytes("bv1"));
    plan.put(to_bytes("b2"), to_bytes("bv2"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = true}, std::move(plan)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());
  return raw_entry_offsets(active_data_file(dir));
}

}  // namespace

TEST_CASE("resume() discards pending batch on CRC error in active file",
          "[degraded][resume]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});

  // Committed entries — baseline.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));

  const auto offsets = degrade_with_unsynced_batch(db, dir);
  REQUIRE(offsets.size() == 6);  // k1, k2, BulkBegin, b1, b2, BulkEnd

  // Corrupt an entry inside the unsynced batch, so resume()'s scan throws
  // part-way through the file. The flip has to land on an entry: an active
  // file is zero-filled ahead of the write cursor, so its last bytes are
  // tail, which the scan stops before and never reads.
  flip_byte(active_data_file(dir), offsets[3] + 15);

  // resume() truncates the batch away and recovers.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // The entries committed before the corruption must still be readable.
  // Present in the key directory is not enough — truncating to the wrong
  // offset leaves them there while their bytes are gone.
  bytecask::Bytes out;
  CHECK(db.get({}, to_bytes("k1"), out));
  CHECK(to_string(out) == "v1");
  CHECK(db.get({}, to_bytes("k2"), out));
  CHECK(to_string(out) == "v2");

  // Writes succeed after resume.
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key({}, to_bytes("k3")));
}

// An I/O error while resume() scans the active file says nothing about the
// bytes, so it must not be read as the end of the file: truncating there
// would cut acknowledged data over a fault the next attempt may not see.
// resume() rethrows it unchanged, truncates nothing, and a retry once the
// fault clears trims only the failed write's tail.
TEST_CASE("resume() rethrows an I/O error from its scan and truncates nothing",
          "[degraded][resume]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});

  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));

  const auto offsets = degrade_with_unsynced_batch(db, dir);
  REQUIRE(offsets.size() == 6);  // k1, k2, BulkBegin, b1, b2, BulkEnd
  const auto size_before = std::filesystem::file_size(active_data_file(dir));

  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_scan"};
    REQUIRE_THROWS_AS(db.resume(), std::system_error);
  }
  CHECK(db.is_degraded());
  CHECK(std::filesystem::file_size(active_data_file(dir)) == size_before);

  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  CHECK(get_str(db, to_bytes("k1")) == "v1");
  CHECK(get_str(db, to_bytes("k2")) == "v2");
}

TEST_CASE("resume() does not trust an entry size that runs past the file",
          "[degraded][resume]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});

  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));

  const auto offsets = degrade_with_unsynced_batch(db, dir);
  REQUIRE(offsets.size() == 6);

  // Corrupt the value_size field (header offset 11, u32 LE) of the first
  // entry in the unsynced batch so it claims a ~4 GiB value. The CRC that
  // would reject the entry covers the value, so it cannot be checked until
  // that many bytes have been read.
  {
    std::fstream f{active_data_file(dir),
                   std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(static_cast<std::streamoff>(offsets[3] + 11));
    const std::array<char, 4> huge{'\x00', '\x00', '\x00', '\x7F'};
    f.write(huge.data(), std::ssize(huge));
  }

  const auto peak_before = peak_rss_bytes();
  REQUIRE_NOTHROW(db.resume());
  const auto peak_after = peak_rss_bytes();

  CHECK_FALSE(db.is_degraded());
  // A declared size is not a reason to allocate: the entry ends past
  // everything ever written to the file, so it is corrupt by inspection.
  CHECK(peak_after - peak_before < 256uz * 1024 * 1024);

  bytecask::Bytes out;
  CHECK(db.get({}, to_bytes("k1"), out));
  CHECK(to_string(out) == "v1");
  CHECK(db.get({}, to_bytes("k2"), out));
  CHECK(to_string(out) == "v2");
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
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("va"));
    plan.put(to_bytes("b"), to_bytes("vb"));
    REQUIRE_THROWS_AS(db.apply_batch({.sync = false}, std::move(plan)),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // Take snapshot while degraded — pins data files.
  auto snap = db.snapshot();
  bytecask::Bytes out;
  CHECK(snap.get({}, to_bytes("k1"), out));
  CHECK(snap.get({}, to_bytes("k2"), out));
  CHECK_FALSE(snap.contains_key({}, to_bytes("a")));

  // resume() with snapshot still alive.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  // Snapshot still readable — pinned files not deleted.
  CHECK(snap.get({}, to_bytes("k1"), out));
  CHECK(snap.get({}, to_bytes("k2"), out));

  // Post-resume writes succeed.
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(db.contains_key({}, to_bytes("k3")));

  // DB reads still correct.
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));
}

// ---------------------------------------------------------------------------
// validate_preconditions unit tests
// ---------------------------------------------------------------------------
// These test TransientEngineState::validate_preconditions in isolation —
// no DB, no disk I/O. We construct EngineState directly, call .transient(),
// and verify each guard path.

namespace {

auto kde(std::uint64_t seq, std::uint64_t off, std::uint32_t fid,
         std::uint32_t vsz) -> bytecask::KeyDirEntry {
  return bytecask::KeyDirEntry::make(seq, off, fid, vsz);
}

// Builds a minimal EngineState with the given key→KeyDirEntry pairs.
auto make_state(
    std::initializer_list<std::pair<std::string, bytecask::KeyDirEntry>> entries)
    -> std::shared_ptr<bytecask::EngineState> {
  auto s = std::make_shared<bytecask::EngineState>();
  s->active_file_id = 1;
  s->next_file_id = 2;
  s->next_seq = 100;
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
  auto state = make_state({{"k", kde(10, 0, 1, 5)}});
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
  auto state = make_state({{"k", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.ensure_absent(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: None precondition always passes",
          "[validate_preconditions]") {
  auto state = make_state({{"k", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan;
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

// --- MustBeUnchanged (requires snapshot) ---

TEST_CASE("validate_preconditions: MustBeUnchanged passes when key unchanged",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
  auto state = make_state({{"k", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeUnchanged fails when key modified",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
  auto state = make_state({{"k", kde(20, 100, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: MustBeUnchanged fails when key deleted",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
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
  auto state = make_state({{"k", kde(15, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

// --- Range guards ---

TEST_CASE("validate_preconditions: range guard passes when range unchanged",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", kde(5, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}, {"d", kde(15, 0, 1, 4)}});
  auto state = make_state({{"a", kde(5, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}, {"d", kde(15, 0, 1, 4)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard fails when key modified in range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"b", kde(10, 0, 1, 5)}});
  auto state = make_state({{"b", kde(20, 100, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard fails when key inserted in range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({{"c", kde(20, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard fails when key deleted in range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"c", kde(10, 0, 1, 5)}});
  auto state = make_state({});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: range guard ignores keys outside range",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", kde(5, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}});
  auto state = make_state({{"a", kde(50, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("b"), to_bytes("d"));
  plan.put(to_bytes("x"), to_bytes("new"));
  CHECK(t.validate_preconditions(plan));
}

// --- Implicit W-W conflict detection (snapshot present) ---

TEST_CASE("validate_preconditions: W-W passes when write key unchanged",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
  auto state = make_state({{"k", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W fails when write key modified",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
  auto state = make_state({{"k", kde(20, 100, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W fails when write key appeared",
          "[validate_preconditions]") {
  auto snap = make_snapshot({});
  auto state = make_state({{"k", kde(15, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: W-W fails when write key deleted",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
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
  auto snap = make_snapshot({{"g", kde(10, 0, 1, 5)}, {"k", kde(10, 0, 1, 5)}});
  auto state = make_state({{"g", kde(20, 100, 1, 3)}, {"k", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("g"));
  plan.put(to_bytes("k"), to_bytes("v1"));
  CHECK(t.validate_preconditions(plan));
}

// --- Combined scenarios ---

TEST_CASE("validate_preconditions: multiple guards all pass",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", kde(5, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}});
  auto state = make_state({{"a", kde(5, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("a"));
  plan.ensure_unchanged(to_bytes("b"));
  plan.put(to_bytes("a"), to_bytes("new_a"));
  CHECK(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: one failing guard rejects plan",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"a", kde(5, 0, 1, 3)}, {"b", kde(10, 0, 1, 5)}});
  auto state = make_state({{"a", kde(5, 0, 1, 3)}, {"b", kde(20, 100, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_present(to_bytes("a"));
  plan.ensure_unchanged(to_bytes("b"));
  plan.put(to_bytes("a"), to_bytes("new_a"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: del with snapshot triggers W-W check",
          "[validate_preconditions]") {
  auto snap = make_snapshot({{"k", kde(10, 0, 1, 5)}});
  auto state = make_state({{"k", kde(20, 100, 1, 5)}});
  auto t = state->transient();
  bytecask::WritePlan plan{std::move(snap)};
  plan.del(to_bytes("k"));
  CHECK_FALSE(t.validate_preconditions(plan));
}

TEST_CASE("validate_preconditions: no snapshot skips W-W checks",
          "[validate_preconditions]") {
  auto state = make_state({{"k", kde(20, 0, 1, 5)}});
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
// resume entries, and verify key_dir, file_stats, and next_seq.

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
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence() == 50);
  CHECK(kv->file_id() == 1);
  CHECK(kv->file_offset() == 0);
  CHECK(kv->value_size() == 4);
}

TEST_CASE("apply_resume: put with higher sequence overwrites existing",
          "[apply_resume]") {
  // file_id=2 holds existing key at seq=10
  auto state = make_state_with_stats(
      {{"k1", kde(10, 100, 2, 5)}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(50, bytecask::EntryType::Put, "k1", 200, 8)};
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence() == 50);
  CHECK(kv->file_id() == 1);
  CHECK(kv->file_offset() == 200);
  CHECK(kv->value_size() == 8);
  // Old file's live_bytes decreased.
  CHECK(s->file_stats.get(2)->live_bytes == 0);
  // New file's live_bytes increased.
  CHECK(s->file_stats.get(1)->live_bytes == bytecask::entry_size(2, 8));
}

TEST_CASE("apply_resume: put with lower sequence is ignored",
          "[apply_resume]") {
  auto state = make_state_with_stats(
      {{"k1", kde(50, 100, 2, 5)}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(10, bytecask::EntryType::Put, "k1", 200, 8)};
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence() == 50);
  CHECK(kv->file_id() == 2);
  // file_stats unchanged.
  CHECK(s->file_stats.get(2)->live_bytes == bytecask::entry_size(2, 5));
  CHECK(s->file_stats.get(1)->live_bytes == 0);
}

TEST_CASE("apply_resume: delete removes key when sequence is higher",
          "[apply_resume]") {
  auto state = make_state_with_stats(
      {{"k1", kde(10, 100, 2, 5)}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(50, bytecask::EntryType::Delete, "k1")};
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  CHECK_FALSE(s->key_dir.get(to_bytes("k1")).has_value());
  CHECK(s->file_stats.get(2)->live_bytes == 0);
}

TEST_CASE("apply_resume: delete is ignored when sequence is lower",
          "[apply_resume]") {
  auto state = make_state_with_stats(
      {{"k1", kde(50, 100, 2, 5)}},
      {{1, {0, 0}}, {2, {bytecask::entry_size(2, 5), 0}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(10, bytecask::EntryType::Delete, "k1")};
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  auto kv = s->key_dir.get(to_bytes("k1"));
  REQUIRE(kv.has_value());
  CHECK(kv->sequence() == 50);
}

TEST_CASE("apply_resume: advances next_seq past highest seen sequence",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  state->next_seq = 10;
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(25, bytecask::EntryType::Put, "a", 0, 3),
      make_resume_entry(30, bytecask::EntryType::Put, "b", 100, 4)};
  t.apply_resume(1, entries, 0);

  CHECK(t.next_seq() == 31);
}

TEST_CASE("apply_resume: does not regress next_seq when entries have lower sequence",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  state->next_seq = 100;
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(5, bytecask::EntryType::Put, "a", 0, 3)};
  t.apply_resume(1, entries, 0);

  CHECK(t.next_seq() == 100);
}

TEST_CASE("apply_resume: empty entries is a no-op",
          "[apply_resume]") {
  auto state = make_state_with_stats({{"k1", kde(10, 0, 1, 5)}}, {{1, {42, 100}}});
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries;
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  CHECK(s->key_dir.get(to_bytes("k1")).has_value());
  CHECK(s->file_stats.get(1)->live_bytes == 42);
  CHECK(s->next_seq == 100);
}

TEST_CASE("apply_resume: multiple entries replayed in order",
          "[apply_resume]") {
  auto state = make_state_with_stats({}, {{1, {0, 0}}});
  state->next_seq = 1;
  auto t = state->transient();

  std::vector<bytecask::ResumeEntry> entries{
      make_resume_entry(10, bytecask::EntryType::Put, "k1", 0, 5),
      make_resume_entry(11, bytecask::EntryType::Put, "k2", 100, 8),
      make_resume_entry(12, bytecask::EntryType::Delete, "k1")};
  t.apply_resume(1, entries, 0);

  auto s = std::move(t).persistent();
  CHECK_FALSE(s->key_dir.get(to_bytes("k1")).has_value());
  auto kv2 = s->key_dir.get(to_bytes("k2"));
  REQUIRE(kv2.has_value());
  CHECK(kv2->sequence() == 11);
  CHECK(s->next_seq == 13);
}

#endif

// ---------------------------------------------------------------------------
// del_range tests
// ---------------------------------------------------------------------------

TEST_CASE("del_range deletes keys in range and leaves others",
          "[bytecask][del_range]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));
  db.put({}, to_bytes("d"), to_bytes("4"));
  db.put({}, to_bytes("e"), to_bytes("5"));

  // Delete [b, d) — should remove b and c, leave a, d, e.
  db.del_range({}, to_bytes("b"), to_bytes("d"));

  bytecask::Bytes out;
  CHECK(db.get({}, to_bytes("a"), out));
  CHECK_FALSE(db.get({}, to_bytes("b"), out));
  CHECK_FALSE(db.get({}, to_bytes("c"), out));
  CHECK(db.get({}, to_bytes("d"), out));
  CHECK(db.get({}, to_bytes("e"), out));
}

TEST_CASE("del_range is a no-op when from >= to",
          "[bytecask][del_range]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  bytecask::Bytes out;

  // from == to: no-op
  db.del_range({}, to_bytes("a"), to_bytes("a"));
  CHECK(db.get({}, to_bytes("a"), out));

  // from > to: no-op
  db.del_range({}, to_bytes("z"), to_bytes("a"));
  CHECK(db.get({}, to_bytes("a"), out));
  CHECK(db.get({}, to_bytes("b"), out));
}

TEST_CASE("del_range with no matching keys still writes entry",
          "[bytecask][del_range]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("z"), to_bytes("2"));

  // Range [m, n) covers no keys — entry written but no keys erased.
  db.del_range({}, to_bytes("m"), to_bytes("n"));

  bytecask::Bytes out;
  CHECK(db.get({}, to_bytes("a"), out));
  CHECK(db.get({}, to_bytes("z"), out));
}

TEST_CASE("del_range in batch combined with puts and deletes",
          "[bytecask][del_range]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));
  db.put({}, to_bytes("d"), to_bytes("4"));

  bytecask::WritePlan plan;
  plan.put(to_bytes("e"), to_bytes("5"));
  plan.del_range(to_bytes("b"), to_bytes("d"));
  plan.del(to_bytes("a"));
  (void)db.apply_batch({}, std::move(plan));

  bytecask::Bytes out;
  CHECK_FALSE(db.get({}, to_bytes("a"), out));
  CHECK_FALSE(db.get({}, to_bytes("b"), out));
  CHECK_FALSE(db.get({}, to_bytes("c"), out));
  CHECK(db.get({}, to_bytes("d"), out));
  CHECK(db.get({}, to_bytes("e"), out));
}

TEST_CASE("del_range in WritePlan with guards",
          "[bytecask][del_range]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));

  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("c"));
  plan.put(to_bytes("d"), to_bytes("4"));

  CHECK(db.apply_batch({}, std::move(plan)));

  bytecask::Bytes out;
  CHECK_FALSE(db.get({}, to_bytes("a"), out));
  CHECK_FALSE(db.get({}, to_bytes("b"), out));
  CHECK(db.get({}, to_bytes("c"), out));
  CHECK(db.get({}, to_bytes("d"), out));
}

TEST_CASE("del_range followed by put on same key — put wins",
          "[bytecask][del_range]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("old"));
  db.del_range({}, to_bytes("a"), to_bytes("b"));
  db.put({}, to_bytes("a"), to_bytes("new"));

  bytecask::Bytes out;
  REQUIRE(db.get({}, to_bytes("a"), out));
  CHECK(to_string(out) == "new");
}

TEST_CASE("del_range survives recovery",
          "[bytecask][del_range][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("1"));
    db.put({}, to_bytes("b"), to_bytes("2"));
    db.put({}, to_bytes("c"), to_bytes("3"));
    db.put({}, to_bytes("d"), to_bytes("4"));
    db.del_range({}, to_bytes("b"), to_bytes("d"));
    // Put after range delete — must survive.
    db.put({}, to_bytes("b"), to_bytes("new_b"));
  }

  auto collect = [](bytecask::DB &db) {
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({})) {
      kv[to_string(entry.key)] = to_string(entry.value);
    }
    return kv;
  };

  // Serial recovery.
  {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1,
                                      .recovery_threads = 1});
    auto kv = collect(db);
    CHECK(kv.count("a") == 1);
    CHECK(kv.count("b") == 1);
    CHECK(kv["b"] == "new_b");
    CHECK(kv.count("c") == 0);
    CHECK(kv.count("d") == 1);
  }

  // Parallel recovery.
  {
    const auto p = td.path / "p2";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1,
                                      .recovery_threads = 4});
    auto kv = collect(db);
    CHECK(kv.count("a") == 1);
    CHECK(kv.count("b") == 1);
    CHECK(kv["b"] == "new_b");
    CHECK(kv.count("c") == 0);
    CHECK(kv.count("d") == 1);
  }
}

TEST_CASE("del_range survives vacuum",
          "[bytecask][del_range][vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});

  // Create some keys.
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));

  // Range delete to create tombstone.
  db.del_range({}, to_bytes("a"), to_bytes("c"));

  // Vacuum to compact files.
  while (db.vacuum()) {}

  bytecask::Bytes out;
  CHECK_FALSE(db.get({}, to_bytes("a"), out));
  CHECK_FALSE(db.get({}, to_bytes("b"), out));
  CHECK(db.get({}, to_bytes("c"), out));
}

TEST_CASE("ensure_unchanged detects concurrent del_range",
          "[bytecask][del_range][guards]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));

  auto snap = db.snapshot();

  // Concurrent range delete.
  db.del_range({}, to_bytes("a"), to_bytes("c"));

  // Plan with ensure_unchanged on a key that was range-deleted.
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("b"));
  plan.put(to_bytes("x"), to_bytes("new"));

  CHECK_FALSE(db.apply_batch({}, std::move(plan)));
}

TEST_CASE("ensure_range_unchanged detects concurrent del_range",
          "[bytecask][del_range][guards]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto snap = db.snapshot();

  // Concurrent range delete.
  db.del_range({}, to_bytes("a"), to_bytes("b"));

  // Plan guarding the range that was modified.
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_range_unchanged(to_bytes("a"), to_bytes("c"));
  plan.put(to_bytes("x"), to_bytes("new"));

  CHECK_FALSE(db.apply_batch({}, std::move(plan)));
}

// ---------------------------------------------------------------------------
// del_range implicit W-W conflict detection (snapshot-based WritePlan)
// ---------------------------------------------------------------------------

// del_range in a WritePlan with snapshot must detect keys modified in range.
TEST_CASE("del_range in WritePlan conflicts on modified key in range",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("b"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("d"));
  CHECK_FALSE(db.apply_batch({}, std::move(plan)));
}

// del_range in a WritePlan with snapshot must detect keys inserted in range.
TEST_CASE("del_range in WritePlan conflicts on inserted key in range",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("b"), to_bytes("new"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("d"));
  CHECK_FALSE(db.apply_batch({}, std::move(plan)));
}

// del_range in a WritePlan with snapshot must detect keys deleted in range.
TEST_CASE("del_range in WritePlan conflicts on deleted key in range",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));

  auto snap = db.snapshot();
  (void)db.del({}, to_bytes("b"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("d"));
  CHECK_FALSE(db.apply_batch({}, std::move(plan)));
}

// del_range in a WritePlan with snapshot succeeds when no keys changed in range.
TEST_CASE("del_range in WritePlan succeeds when range is clean",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("d"), to_bytes("4"));

  auto snap = db.snapshot();
  // Modify a key outside the range.
  db.put({}, to_bytes("d"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("c"));
  CHECK(db.apply_batch({}, std::move(plan)));

  bytecask::Bytes out;
  CHECK_FALSE(db.get({}, to_bytes("a"), out));
  CHECK_FALSE(db.get({}, to_bytes("b"), out));
}

// del_range in a WritePlan with snapshot succeeds on an empty range.
TEST_CASE("del_range in WritePlan succeeds on empty range",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("z"), to_bytes("26"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("a"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  // Range [m, n) has no keys — should succeed despite other modifications.
  plan.del_range(to_bytes("m"), to_bytes("n"));
  CHECK(db.apply_batch({}, std::move(plan)));
}

// del_range conflict detection is per-range: only the affected range triggers conflict.
TEST_CASE("del_range in WritePlan — conflict only within range boundary",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("c"), to_bytes("3"));

  auto snap = db.snapshot();
  // Modify "c" which is at the exclusive upper bound — outside [a, c).
  db.put({}, to_bytes("c"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("c"));
  // "c" is at the exclusive boundary, so no conflict for the range itself.
  // But "c" is not in the write set either, so no implicit W-W check on it.
  CHECK(db.apply_batch({}, std::move(plan)));
}

// Multiple del_range operations in one plan — first clean, second conflicts.
TEST_CASE("del_range in WritePlan — multiple ranges, one conflicts",
          "[bytecask][del_range][conflict]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  db.put({}, to_bytes("x"), to_bytes("24"));
  db.put({}, to_bytes("y"), to_bytes("25"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("y"), to_bytes("modified"));

  bytecask::WritePlan plan{std::move(snap)};
  plan.del_range(to_bytes("a"), to_bytes("c")); // clean range
  plan.del_range(to_bytes("x"), to_bytes("z")); // conflicting range
  CHECK_FALSE(db.apply_batch({}, std::move(plan)));
}

// ---------------------------------------------------------------------------
// Causality: operation order within a batch/plan must be preserved
// ---------------------------------------------------------------------------

TEST_CASE("WritePlan: put then del_range — put is killed",
          "[bytecask][del_range][causality]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  bytecask::WritePlan plan;
  plan.put(to_bytes("key"), to_bytes("val"));
  plan.del_range(to_bytes("a"), to_bytes("z"));
  (void)db.apply_batch({}, std::move(plan));

  CHECK_FALSE(db.contains_key({}, to_bytes("key")));
}

TEST_CASE("WritePlan: del_range then put — put survives",
          "[bytecask][del_range][causality]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Pre-populate so del_range has something to delete.
  db.put({}, to_bytes("key"), to_bytes("old"));

  bytecask::WritePlan plan;
  plan.del_range(to_bytes("a"), to_bytes("z"));
  plan.put(to_bytes("key"), to_bytes("new"));
  (void)db.apply_batch({}, std::move(plan));

  bytecask::Bytes out;
  REQUIRE(db.get({}, to_bytes("key"), out));
  CHECK(to_string(out) == "new");
}

TEST_CASE("WritePlan: interleaved puts and del_range — correct causality",
          "[bytecask][del_range][causality]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("1"));
  plan.del_range(to_bytes("a"), to_bytes("z"));
  plan.put(to_bytes("b"), to_bytes("2"));
  (void)db.apply_batch({}, std::move(plan));

  CHECK_FALSE(db.contains_key({}, to_bytes("a"))); // killed by del_range
  bytecask::Bytes out;
  REQUIRE(db.get({}, to_bytes("b"), out));      // survives — after del_range
  CHECK(to_string(out) == "2");
}

TEST_CASE("Causality survives recovery",
          "[bytecask][del_range][causality][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);

    // Plan 1: del_range then put — put should survive.
    bytecask::WritePlan b1;
    b1.del_range(to_bytes("a"), to_bytes("z"));
    b1.put(to_bytes("key:surv"), to_bytes("alive"));
    (void)db.apply_batch({}, std::move(b1));

    // Plan 2: put then del_range — put should be killed.
    bytecask::WritePlan b2;
    b2.put(to_bytes("key:dead"), to_bytes("doomed"));
    b2.del_range(to_bytes("key:d"), to_bytes("key:e"));
    (void)db.apply_batch({}, std::move(b2));
  }

  SECTION("serial recovery") {
    auto db = bytecask::DB::open(db_path, {.recovery_threads = 1});
    bytecask::Bytes out;
    REQUIRE(db.get({}, to_bytes("key:surv"), out));
    CHECK(to_string(out) == "alive");
    CHECK_FALSE(db.contains_key({}, to_bytes("key:dead")));
  }

  SECTION("parallel recovery") {
    auto db = bytecask::DB::open(db_path, {.recovery_threads = 4});
    bytecask::Bytes out;
    REQUIRE(db.get({}, to_bytes("key:surv"), out));
    CHECK(to_string(out) == "alive");
    CHECK_FALSE(db.contains_key({}, to_bytes("key:dead")));
  }
}

// ---------------------------------------------------------------------------
// Model-based recovery with range deletes
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: workload with range deletes",
          "[bytecask][recovery][parallel][model]") {
  std::mt19937 gen(77777);

  auto rand_key = [&]() -> std::string {
    static constexpr std::string_view alphabet = "mnopqr";
    const auto len = std::uniform_int_distribution<int>(1, 5)(gen);
    std::string k;
    for (int i = 0; i < len; ++i) {
      k += alphabet[static_cast<std::size_t>(std::uniform_int_distribution<int>(
          0, static_cast<int>(alphabet.size()) - 1)(gen))];
    }
    return k;
  };

  auto rand_value = [&]() -> std::string {
    const auto len = std::uniform_int_distribution<int>(1, 32)(gen);
    std::string v(static_cast<std::size_t>(len), 'R');
    for (auto &c : v) {
      c = static_cast<char>(
          std::uniform_int_distribution<int>('A', 'z')(gen));
    }
    return v;
  };

  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;

  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

    constexpr int kOps = 2000;
    for (int i = 0; i < kOps; ++i) {
      const auto op = std::uniform_int_distribution<int>(0, 9)(gen);

      if (op < 4) {
        // 40% put
        auto key = rand_key();
        auto val = rand_value();
        db.put({}, to_bytes(key), to_bytes(val));
        oracle[key] = val;
      } else if (op < 6) {
        // 20% delete
        auto key = rand_key();
        std::ignore = db.del({}, to_bytes(key));
        oracle.erase(key);
      } else if (op < 8) {
        // 20% batch with causality-sensitive ordering
        auto from = rand_key();
        auto to_key = rand_key();
        if (from > to_key) std::swap(from, to_key);
        if (from == to_key) to_key += "~";

        auto put_key = rand_key();
        auto put_val = rand_value();

        // Alternate: half put-then-del_range, half del_range-then-put.
        const bool put_first = (i % 2 == 0);

        bytecask::WritePlan plan;
        if (put_first) {
          plan.put(to_bytes(put_key), to_bytes(put_val));
          plan.del_range(to_bytes(from), to_bytes(to_key));
        } else {
          plan.del_range(to_bytes(from), to_bytes(to_key));
          plan.put(to_bytes(put_key), to_bytes(put_val));
        }
        (void)db.apply_batch({}, std::move(plan));

        // Mirror to oracle in the same order.
        if (put_first) {
          oracle[put_key] = put_val;
          auto it = oracle.lower_bound(from);
          while (it != oracle.end() && it->first < to_key) {
            it = oracle.erase(it);
          }
        } else {
          auto it = oracle.lower_bound(from);
          while (it != oracle.end() && it->first < to_key) {
            it = oracle.erase(it);
          }
          oracle[put_key] = put_val;
        }
      } else {
        // 20% standalone range delete
        auto from = rand_key();
        auto to_key = rand_key();
        if (from > to_key) std::swap(from, to_key);
        if (from == to_key) to_key += "~";
        db.del_range({}, to_bytes(from), to_bytes(to_key));
        auto it = oracle.lower_bound(from);
        while (it != oracle.end() && it->first < to_key) {
          it = oracle.erase(it);
        }
      }
    }
  }

  auto collect = [](bytecask::DB &db) {
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({})) {
      kv[to_string(entry.key)] = to_string(entry.value);
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
    std::vector<std::tuple<std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats()) {
      vals.emplace_back(fs.live_bytes, fs.total_bytes,
                        fs.min_sequence, fs.max_sequence);
    }
    std::ranges::sort(vals);
    return vals;
  };

  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }
  REQUIRE(data_file_count > 1);

  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> serial_stats_vals;
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

#ifndef __EMSCRIPTEN__
  // The pool serves every value read below, so this proves recovery and
  // pool-backed reads agree with the oracle together — not just that the
  // key directory was rebuilt.
  SECTION("parallel recovery through the buffer pool (2 workers)") {
    const auto p = td.path / "pool";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 2,
                                     .io_backend = bytecask::IoBackend::BufferPool,
                                     .buffer_pool = {.capacity_bytes = 1 << 20}});
    verify("parallel/2/pool", collect(db));
    CHECK(collect_stats(db) == serial_stats_vals);
  }
#endif
}

// ---------------------------------------------------------------------------
// durable_sequence: durable_sequence reflects sync writes
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence reflects sync writes", "[durable_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Fresh DB with no writes: durable_seq == 0 (next_seq starts at 1,
  // no keys recovered, so durable_seq = next_seq - 1 = 0).
  CHECK(db.durable_sequence() == 0);

  // NoSync write — must NOT advance durable_seq.
  db.put({.sync = false}, to_bytes("k1"), to_bytes("v1"));
  CHECK(db.durable_sequence() == 0);

  // Sync write — must advance durable_seq to cover both keys
  // (group commit: the sync also confirms the earlier nosync entry).
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  auto seq_after_sync = db.durable_sequence();
  CHECK(seq_after_sync >= 2);
}

// ---------------------------------------------------------------------------
// durable_sequence: nosync-only writes never advance durable_seq
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence stays at zero for nosync-only writes",
          "[durable_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  for (int i = 0; i < 10; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{}", i)),
           to_bytes(std::format("v{}", i)));
  }
  CHECK(db.durable_sequence() == 0);
}

// ---------------------------------------------------------------------------
// durable_sequence: target already reached / min_sequence=0 return immediately
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence returns immediately for reached targets",
          "[durable_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  auto baseline = db.durable_sequence();
  REQUIRE(baseline >= 1);

  auto start = std::chrono::steady_clock::now();
  // min_sequence = 0: always trivially reached.
  CHECK(db.durable_sequence(0, std::chrono::milliseconds{5000}) == baseline);
  // Target already reached: returns without blocking on the long timeout.
  CHECK(db.durable_sequence(baseline, std::chrono::milliseconds{5000}) == baseline);
  auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed < std::chrono::milliseconds{2000});
}

// ---------------------------------------------------------------------------
// durable_sequence: long-poll wakes when a sync write reaches the target
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence long-poll wakes on sync write", "[durable_seq][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  std::atomic<std::uint64_t> polled_seq{0};
  std::thread poller{[&] {
    polled_seq.store(
        db.durable_sequence(1, std::chrono::milliseconds{5000}),
        std::memory_order_release);
  }};

  // Give poller time to block on the condvar.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  // Sync write wakes the poller by reaching target sequence 1.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));

  poller.join();
  CHECK(polled_seq.load(std::memory_order_acquire) >= 1);
}

// ---------------------------------------------------------------------------
// durable_sequence: long-poll times out when the target is never reached
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence long-poll times out on idle DB", "[durable_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto start = std::chrono::steady_clock::now();
  // Target 1 is never reached — nothing is written.
  auto seq = db.durable_sequence(1, std::chrono::milliseconds{50});
  auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(seq < 1);
  CHECK(elapsed >= std::chrono::milliseconds{40});
}

// ---------------------------------------------------------------------------
// durable_sequence: correct after recovery
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence correct after recovery", "[durable_seq]") {
  TempDir td;
  auto db_path = td.path / "db";

  {
    auto db = bytecask::DB::open(db_path);
    db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
    db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  }

  // Reopen — all recovered entries were previously synced.
  auto db = bytecask::DB::open(db_path);
  auto seq = db.durable_sequence();
  // durable_seq should equal next_seq - 1 after recovery.
  // We wrote 2 sync entries, so durable_seq >= 2.
  CHECK(seq >= 2);

  // Verify values survived.
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));
}

// ---------------------------------------------------------------------------
// durable_sequence: correct after resume
// ---------------------------------------------------------------------------
TEST_CASE("durable_sequence correct after resume", "[durable_seq][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1'000'000});

  // Committed baseline.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  auto seq_before = db.durable_sequence();
  CHECK(seq_before >= 1);

  // Degrade via sync failure.
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());

  // Resume — should recover and set durable_seq correctly.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());

  auto seq_after = db.durable_sequence();
  // After resume, durable_seq should be >= what it was before the failure.
  CHECK(seq_after >= seq_before);
}

// ===========================================================================
// CommitResult — sequence and durability reporting
// ===========================================================================

// ---------------------------------------------------------------------------
// CommitResult: sequence is monotonic across put/del/del_range/apply_batch
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult sequence is monotonic across write operations",
          "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto r1 = db.put({}, to_bytes("a"), to_bytes("v1"));
  REQUIRE(r1.sequence > 0);

  auto r2 = db.put({}, to_bytes("b"), to_bytes("v2"));
  CHECK(r2.sequence > r1.sequence);

  auto r3 = db.del_range({}, to_bytes("z0"), to_bytes("z9"));
  CHECK(r3.sequence > r2.sequence);

  auto r4 = db.del({}, to_bytes("a"));
  REQUIRE(r4.has_value());
  CHECK(r4->sequence > r3.sequence);

  bytecask::WritePlan plan;
  plan.put(to_bytes("c"), to_bytes("v3"));
  auto r5 = db.apply_batch({}, std::move(plan));
  REQUIRE(r5.has_value());
  CHECK(r5->sequence > r4->sequence);
}

// ---------------------------------------------------------------------------
// CommitResult: multi-op batch result equals the BulkEnd marker's sequence
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult sequence equals BulkEnd sequence for multi-op batch",
          "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto from_seq = db.durable_sequence();

  bytecask::WritePlan plan;
  plan.put(to_bytes("k1"), to_bytes("v1"));
  plan.put(to_bytes("k2"), to_bytes("v2"));
  plan.del_range(to_bytes("z0"), to_bytes("z1"));
  auto result = db.apply_batch({}, std::move(plan));
  REQUIRE(result.has_value());

  auto snap = db.snapshot();
  auto changes = db.changes_since(snap, from_seq);
  bool found_bulk_end = false;
  for (const auto &entry : changes) {
    if (entry.entry_type == bytecask::EntryType::BulkEnd) {
      found_bulk_end = true;
      CHECK(entry.sequence == result->sequence);
    }
  }
  CHECK(found_bulk_end);
}

// ---------------------------------------------------------------------------
// CommitResult: del of an absent key returns nullopt (nothing written)
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult del of absent key returns nullopt", "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto before = db.durable_sequence();
  auto result = db.del({}, to_bytes("nonexistent"));
  CHECK_FALSE(result.has_value());
  CHECK(db.durable_sequence() == before);
}

// ---------------------------------------------------------------------------
// CommitResult: apply_batch conflict returns nullopt, consumes no sequence
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult apply_batch conflict returns nullopt",
          "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));

  auto snap = db.snapshot();
  db.put({}, to_bytes("k"), to_bytes("v1"));  // Modifies "k" after snap.

  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("v2"));

  auto before = db.durable_sequence();
  auto result = db.apply_batch({}, std::move(plan));
  CHECK_FALSE(result.has_value());
  CHECK(db.durable_sequence() == before);
}

// ---------------------------------------------------------------------------
// CommitResult: empty plan commits as a no-op — {sequence = 0, durable = true}
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult empty plan returns durable zero sequence",
          "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  bytecask::WritePlan plan;
  auto result = db.apply_batch({}, std::move(plan));
  REQUIRE(result.has_value());
  CHECK(result->sequence == 0);
  CHECK(result->durable);
}

// ---------------------------------------------------------------------------
// CommitResult: a guard-only plan that passes returns {0, true}
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult guard-only plan returns durable zero sequence",
          "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("g"), to_bytes("v"));

  bytecask::WritePlan plan;
  plan.ensure_present(to_bytes("g"));
  auto result = db.apply_batch({}, std::move(plan));
  REQUIRE(result.has_value());
  CHECK(result->sequence == 0);
  CHECK(result->durable);
}

// ---------------------------------------------------------------------------
// CommitResult: del_range with an empty range writes nothing
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult del_range empty range returns durable zero sequence",
          "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto result = db.del_range({}, to_bytes("z"), to_bytes("a"));  // from >= to
  CHECK(result.sequence == 0);
  CHECK(result.durable);
}

// ---------------------------------------------------------------------------
// CommitResult: sync=true write is always durable
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult sync write is durable", "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto result = db.put({.sync = true}, to_bytes("k"), to_bytes("v"));
  CHECK(result.durable);
}

// ---------------------------------------------------------------------------
// CommitResult: solo nosync write is not durable
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult solo nosync write is not durable", "[commit_result]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto result = db.put({.sync = false, .solo = true}, to_bytes("k"), to_bytes("v"));
  CHECK_FALSE(result.durable);
}

// ---------------------------------------------------------------------------
// CommitResult: a nosync writer coalesced with a sync writer in the same
// group-commit batch reports durable = true (deterministic via the
// test_write_group() seam).
// ---------------------------------------------------------------------------
TEST_CASE("CommitResult nosync writer coalesced with sync writer is durable",
          "[commit_result][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  std::mutex mu;
  std::condition_variable cv;
  bool leader_ready = false;

  db.test_write_group().on_leader_start_ = [&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      leader_ready = true;
      cv.notify_all();
    }
    db.test_write_group().wait_for_queue_size(2);
  };

  std::optional<bytecask::CommitResult> nosync_result;
  std::optional<bytecask::CommitResult> sync_result;

  // Thread A becomes leader with a nosync write, blocks until B enqueues.
  std::thread tA([&] {
    nosync_result = db.apply_batch({.sync = false}, [] {
      bytecask::WritePlan p;
      p.put(to_bytes("a"), to_bytes("va"));
      return p;
    }());
  });

  // Thread B enqueues a sync write into the same batch.
  std::thread tB([&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      cv.wait(lk, [&] { return leader_ready; });
    }
    sync_result = db.apply_batch({.sync = true}, [] {
      bytecask::WritePlan p;
      p.put(to_bytes("b"), to_bytes("vb"));
      return p;
    }());
  });

  tA.join();
  tB.join();

  db.test_write_group().on_leader_start_ = nullptr;

  REQUIRE(nosync_result.has_value());
  REQUIRE(sync_result.has_value());
  CHECK(sync_result->durable);
  CHECK(nosync_result->durable);
}

// ---------------------------------------------------------------------------
// vacuum preserves BulkBegin/BulkEnd markers for live batches
// ---------------------------------------------------------------------------
TEST_CASE("vacuum preserves BulkBegin/BulkEnd markers", "[vacuum][batch]") {
  TempDir td;
  auto db_path = td.path / "db";
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

  // Write individual keys first to create dead entries.
  db.put({}, to_bytes("a"), to_bytes("old_a"));
  db.put({}, to_bytes("b"), to_bytes("old_b"));

  // Batch overwrites — these land in one sealed file with BulkBegin/BulkEnd.
  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("new_a"));
  plan.put(to_bytes("b"), to_bytes("new_b"));
  (void)db.apply_batch({}, std::move(plan));

  // Vacuum — the batch file has live entries, so markers should be preserved.
  for (int i = 0; i < 10 && db.vacuum({.fragmentation_threshold = 0.0}); ++i) {}

  // Verify data is correct.
  auto va = get_val(db, to_bytes("a"));
  auto vb = get_val(db, to_bytes("b"));
  REQUIRE(va.has_value());
  REQUIRE(vb.has_value());
  CHECK(to_string(*va) == "new_a");
  CHECK(to_string(*vb) == "new_b");

  // Scan vacuumed files for BulkBegin/BulkEnd markers.
  bool found_begin = false;
  bool found_end = false;
  for (const auto &entry : std::filesystem::directory_iterator{db_path}) {
    if (entry.path().extension() != ".data") continue;
    auto df_ptr = bytecask::openDataFileForRead(entry.path()); auto &df = *df_ptr;
    bytecask::Offset off = 0;
    while (auto sr = df.scan(off)) {
      const auto &[de, next] = *sr;
      if (de.entry_type == bytecask::EntryType::BulkBegin) found_begin = true;
      if (de.entry_type == bytecask::EntryType::BulkEnd) found_end = true;
      off = next;
    }
  }
  CHECK(found_begin);
  CHECK(found_end);
}

// ---------------------------------------------------------------------------
// vacuum drops batch when all entries are stale
// ---------------------------------------------------------------------------
TEST_CASE("vacuum drops batch when all entries are stale",
          "[vacuum][batch]") {
  TempDir td;
  auto db_path = td.path / "db";
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 1});

  // Write a batch.
  {
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("batch_a"));
    plan.put(to_bytes("b"), to_bytes("batch_b"));
    (void)db.apply_batch({}, std::move(plan));
  }

  // Overwrite all batch keys individually — makes the batch stale.
  db.put({}, to_bytes("a"), to_bytes("solo_a"));
  db.put({}, to_bytes("b"), to_bytes("solo_b"));

  // Vacuum — the stale batch should be dropped entirely (no markers).
  for (int i = 0; i < 10 && db.vacuum({.fragmentation_threshold = 0.0}); ++i) {}

  // Verify latest values.
  auto va = get_val(db, to_bytes("a"));
  auto vb = get_val(db, to_bytes("b"));
  REQUIRE(va.has_value());
  REQUIRE(vb.has_value());
  CHECK(to_string(*va) == "solo_a");
  CHECK(to_string(*vb) == "solo_b");

  // Scan all data files — no BulkBegin/BulkEnd markers should remain.
  bool found_marker = false;
  for (const auto &entry : std::filesystem::directory_iterator{db_path}) {
    if (entry.path().extension() != ".data") continue;
    auto df_ptr = bytecask::openDataFileForRead(entry.path()); auto &df = *df_ptr;
    bytecask::Offset off = 0;
    while (auto sr = df.scan(off)) {
      const auto &[de, next] = *sr;
      if (de.entry_type == bytecask::EntryType::BulkBegin ||
          de.entry_type == bytecask::EntryType::BulkEnd) {
        found_marker = true;
      }
      off = next;
    }
  }
  CHECK_FALSE(found_marker);
}

// ---------------------------------------------------------------------------
// FileStats min_sequence / max_sequence
// ---------------------------------------------------------------------------

TEST_CASE("single write sets min_sequence and max_sequence",
          "[file_stats_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &[fid, fs] = *stats.begin();
  CHECK(fs.min_sequence == 1);
  CHECK(fs.max_sequence == 1);
}

TEST_CASE("multiple writes update sequence range", "[file_stats_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &[fid, fs] = *stats.begin();
  CHECK(fs.min_sequence == 1);
  CHECK(fs.max_sequence == 3);
}

TEST_CASE("batch write covers marker sequences", "[file_stats_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  // 3-op batch: BulkBegin(1), Put(2), Put(3), Put(4), BulkEnd(5)
  bytecask::WritePlan plan;
  plan.put(to_bytes("k1"), to_bytes("v1"));
  plan.put(to_bytes("k2"), to_bytes("v2"));
  plan.put(to_bytes("k3"), to_bytes("v3"));
  (void)db.apply_batch({.sync = true}, std::move(plan));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &[fid, fs] = *stats.begin();
  CHECK(fs.min_sequence == 1);
  CHECK(fs.max_sequence == 5);
}

TEST_CASE("rotation preserves sealed file bounds and resets new",
          "[file_stats_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_file_bytes = 1});
  // First write goes to file 1; rotation triggered.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  // Second write goes to a new active file.
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));

  auto stats = db.file_stats();
  REQUIRE(stats.size() >= 2);
  // Find the sealed file (min_sequence == 1) and the new active.
  bool found_sealed = false;
  bool found_new = false;
  for (const auto &[fid, fs] : stats) {
    if (fs.min_sequence == 1 && fs.max_sequence == 1) {
      found_sealed = true;
    }
    if (fs.min_sequence == 2 && fs.max_sequence == 2) {
      found_new = true;
    }
  }
  CHECK(found_sealed);
  CHECK(found_new);
}

TEST_CASE("vacuum compact tracks sequences", "[file_stats_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_file_bytes = 1});
  // Write 3 keys, each triggers rotation.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));

  // Overwrite k1 to create fragmentation in the first file.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1b"));

  // Vacuum the fragmented file.
  (void)db.vacuum({.fragmentation_threshold = 0.0});

  // Verify all files have coherent sequence bounds.
  for (const auto &[fid, fs] : db.file_stats()) {
    if (fs.total_bytes > 0) {
      CHECK(fs.min_sequence > 0);
      CHECK(fs.max_sequence >= fs.min_sequence);
    }
  }
}

TEST_CASE("recovery reconstructs min_max sequences", "[file_stats_seq]") {
  TempDir td;
  std::map<std::uint32_t, bytecask::FileStats> pre_close_stats;
  {
    auto db = bytecask::DB::open(td.path, {.max_file_bytes = 1});
    db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
    db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
    db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));
    pre_close_stats = db.file_stats();
  }

  // Reopen — recovery rebuilds stats from hint files.
  auto db = bytecask::DB::open(td.path);
  auto post_open_stats = db.file_stats();

  // Verify each sealed file from pre-close appears in post-open with
  // matching sequence bounds. Skip empty active files (min==0, max==0).
  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> pre_vals;
  for (const auto &[fid, fs] : pre_close_stats) {
    if (fs.min_sequence == 0 && fs.max_sequence == 0) continue;
    pre_vals.emplace_back(fs.live_bytes, fs.total_bytes,
                          fs.min_sequence, fs.max_sequence);
  }
  std::ranges::sort(pre_vals);

  std::vector<std::tuple<std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t>> post_vals;
  for (const auto &[fid, fs] : post_open_stats) {
    if (fs.min_sequence == 0 && fs.max_sequence == 0) continue;
    post_vals.emplace_back(fs.live_bytes, fs.total_bytes,
                           fs.min_sequence, fs.max_sequence);
  }
  std::ranges::sort(post_vals);

  CHECK(pre_vals == post_vals);
}

TEST_CASE("nosync writes track sequences", "[file_stats_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  db.put({.sync = false}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = false}, to_bytes("k2"), to_bytes("v2"));

  auto stats = db.file_stats();
  REQUIRE(stats.size() == 1);
  const auto &[fid, fs] = *stats.begin();
  CHECK(fs.min_sequence == 1);
  CHECK(fs.max_sequence == 2);
}

// ---------------------------------------------------------------------------
// create_manifest tests
// ---------------------------------------------------------------------------

TEST_CASE("basic manifest contains sealed files with hints", "[manifest]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));

  auto manifest = db.create_manifest();

  CHECK(manifest.through_sequence > 0);
  CHECK_FALSE(manifest.files.empty());

  // Every sealed file in the manifest should exist on disk with a .hint companion.
  for (const auto &fi : manifest.files) {
    CHECK(std::filesystem::exists(fi.data_path));
    CHECK(std::filesystem::exists(fi.hint_path));
  }

  // Snapshot should be readable.
  bytecask::Bytes out;
  CHECK(manifest.snap.get({}, to_bytes("k1"), out));
  CHECK(manifest.snap.get({}, to_bytes("k2"), out));
  CHECK(manifest.snap.get({}, to_bytes("k3"), out));
}

TEST_CASE("empty db manifest", "[manifest]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  auto manifest = db.create_manifest();

  // No writes — through_sequence is 0.
  CHECK(manifest.through_sequence == 0);

  // Snapshot has no keys.
  bytecask::Bytes out;
  CHECK_FALSE(manifest.snap.get({}, to_bytes("anything"), out));
}

TEST_CASE("writes continue after manifest", "[manifest]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));

  auto manifest = db.create_manifest();
  const auto manifest_seq = manifest.through_sequence;
  CHECK(manifest_seq > 0);

  // Write more after manifest — not visible in the snapshot.
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));

  bytecask::Bytes out;
  CHECK_FALSE(manifest.snap.get({}, to_bytes("k2"), out));
  CHECK_FALSE(manifest.snap.get({}, to_bytes("k3"), out));

  // durable_sequence advances beyond through_sequence.
  CHECK(db.durable_sequence() > manifest_seq);
}

TEST_CASE("through_sequence includes nosync entries", "[manifest]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Write without sync — entries are in the active file but not yet durable.
  db.put({.sync = false}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = false}, to_bytes("k2"), to_bytes("v2"));

  // create_manifest syncs before rotation, so these become durable.
  auto manifest = db.create_manifest();
  CHECK(manifest.through_sequence >= 2);

  // Both keys visible in snapshot.
  bytecask::Bytes out;
  CHECK(manifest.snap.get({}, to_bytes("k1"), out));
  CHECK(manifest.snap.get({}, to_bytes("k2"), out));
}

TEST_CASE("bootstrap simulation", "[manifest]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));

  auto manifest = db.create_manifest();

  // Copy manifest files to a new directory (simulating file transfer).
  TempDir td2;
  const auto dest = td2.path / "replica";
  std::filesystem::create_directories(dest);
  for (const auto &fi : manifest.files) {
    std::filesystem::copy(fi.data_path, dest / fi.data_path.filename());
    std::filesystem::copy(fi.hint_path, dest / fi.hint_path.filename());
  }

  // Open replica from copied files.
  auto replica = bytecask::DB::open(dest);

  bytecask::Bytes out;
  CHECK(replica.get({}, to_bytes("k1"), out));
  CHECK(replica.get({}, to_bytes("k2"), out));
  CHECK(replica.get({}, to_bytes("k3"), out));
}

TEST_CASE("manifest after vacuum", "[manifest]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));

  // Overwrite to create fragmentation, then vacuum.
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1_new"));
  (void)db.vacuum({.fragmentation_threshold = 0.0});

  auto manifest = db.create_manifest();

  CHECK(manifest.through_sequence > 0);
  CHECK_FALSE(manifest.files.empty());

  // All files exist on disk.
  for (const auto &fi : manifest.files) {
    CHECK(std::filesystem::exists(fi.data_path));
    CHECK(std::filesystem::exists(fi.hint_path));
  }

  // All keys present in snapshot with correct values.
  bytecask::Bytes out;
  CHECK(manifest.snap.get({}, to_bytes("k1"), out));
  CHECK(to_string(out) == "v1_new");
  CHECK(manifest.snap.get({}, to_bytes("k2"), out));
  CHECK(to_string(out) == "v2");
  CHECK(manifest.snap.get({}, to_bytes("k3"), out));
  CHECK(to_string(out) == "v3");
}

// ===========================================================================
// changes_since iterator tests
// ===========================================================================

TEST_CASE("changes_since iterator yields entries in sequence order", "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Write some entries
  db.put({}, to_bytes("key1"), to_bytes("value1"));
  db.put({}, to_bytes("key2"), to_bytes("value2"));
  (void)db.del({}, to_bytes("key1"));  // tombstone
  db.put({}, to_bytes("key3"), to_bytes("value3"));

  // Get current sequence - this is the boundary
  auto from_seq = db.durable_sequence();

  // Write more entries after the baseline (these should be yielded)
  db.put({}, to_bytes("key4"), to_bytes("value4"));
  db.put({}, to_bytes("key5"), to_bytes("value5"));

  // Take snapshot after writing the new entries
  auto snap = db.snapshot();

  // changes_since should yield entries after from_seq in sequence order
  auto changes = db.changes_since(snap, from_seq);

  std::vector<std::string> collected_keys;
  std::vector<std::string> collected_values;
  std::vector<std::uint64_t> collected_sequences;

  for (const auto& entry : changes) {
    collected_sequences.push_back(entry.sequence);
    collected_keys.emplace_back(reinterpret_cast<const char*>(entry.key.data()), entry.key.size());
    collected_values.emplace_back(reinterpret_cast<const char*>(entry.value.data()), entry.value.size());
  }

  // Debug what we actually got
  INFO("from_seq: " << from_seq);
  INFO("snap durable_seq: " << db.durable_sequence());
  INFO("collected " << collected_sequences.size() << " entries");
  for (size_t i = 0; i < collected_sequences.size(); ++i) {
    INFO("Entry " << i << " seq=" << collected_sequences[i] << " key='" << collected_keys[i] << "' value='" << collected_values[i] << "'");
  }

  // Should have 2 entries (sequences 5 and 6) in sequence order
  REQUIRE(collected_sequences.size() == 2);
  REQUIRE(collected_sequences[0] < collected_sequences[1]);
  REQUIRE(collected_sequences[0] == 5);
  REQUIRE(collected_sequences[1] == 6);

  // Check that we got the expected keys and values
  REQUIRE(collected_keys[0] == "key4");
  REQUIRE(collected_values[0] == "value4");
  REQUIRE(collected_keys[1] == "key5");
  REQUIRE(collected_values[1] == "value5");
}

TEST_CASE("changes_since empty iterator when no new entries", "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  // Write some entries
  db.put({}, to_bytes("key1"), to_bytes("value1"));
  db.put({}, to_bytes("key2"), to_bytes("value2"));

  auto from_seq = db.durable_sequence();
  auto snap = db.snapshot();

  // changes_since should be empty (no entries after from_seq)
  auto changes = db.changes_since(snap, from_seq);

  std::vector<bytecask::DataEntryView> entries;
  for (const auto& entry : changes) {
    entries.push_back({
      .sequence = entry.sequence,
      .entry_type = entry.entry_type,
      .key = bytecask::Bytes{entry.key.begin(), entry.key.end()},
      .value = bytecask::Bytes{entry.value.begin(), entry.value.end()}
    });
  }

  REQUIRE(entries.empty());
}

// ---------------------------------------------------------------------------
// Ingest + Mode::Follower tests
// ---------------------------------------------------------------------------

TEST_CASE("mode enforcement: put/del/apply_batch throw in follower mode",
          "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  CHECK(db.mode() == bytecask::Mode::Leader);

  db.set_mode(bytecask::Mode::Follower);
  CHECK(db.mode() == bytecask::Mode::Follower);

  CHECK_THROWS_AS(db.put({}, to_bytes("k"), to_bytes("v")),
                  bytecask::DbFollowerMode);
  CHECK_THROWS_AS((void)db.del({}, to_bytes("k")),
                  bytecask::DbFollowerMode);
  CHECK_THROWS_AS(db.del_range({}, to_bytes("a"), to_bytes("z")),
                  bytecask::DbFollowerMode);
  bytecask::WritePlan plan;
  plan.put(to_bytes("k"), to_bytes("v"));
  CHECK_THROWS_AS((void)db.apply_batch({}, std::move(plan)),
                  bytecask::DbFollowerMode);

  // Reads still work.
  bytecask::Bytes out;
  CHECK_FALSE(db.get({}, to_bytes("k"), out));
  CHECK_FALSE(db.contains_key({}, to_bytes("k")));
}

TEST_CASE("ingest throws in leader mode", "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  std::vector<bytecask::DataEntryView> entries;
  CHECK_THROWS_AS(db.ingest(entries), std::logic_error);
}

TEST_CASE("set_mode transitions: leader -> follower -> leader", "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  CHECK(db.mode() == bytecask::Mode::Leader);
  db.set_mode(bytecask::Mode::Follower);
  CHECK(db.mode() == bytecask::Mode::Follower);
  db.set_mode(bytecask::Mode::Leader);
  CHECK(db.mode() == bytecask::Mode::Leader);

  // After returning to leader, writes should work again.
  db.put({}, to_bytes("k"), to_bytes("v"));
  bytecask::Bytes out;
  REQUIRE(db.get({}, to_bytes("k"), out));
}

TEST_CASE("basic ingest: entries from changes_since are ingested correctly",
          "[replication]") {
  TempDir td;

  // Leader writes.
  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("user:1"), to_bytes("alice"));
  leader.put({}, to_bytes("user:2"), to_bytes("bob"));
  leader.put({}, to_bytes("user:3"), to_bytes("carol"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  // Collect entries (must own data since iterator views are transient).
  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }

  // Build DataEntryView span from owned data.
  std::vector<bytecask::DataEntryView> views;
  views.reserve(owned.size());
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  // Follower ingests.
  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);

  // Verify all keys match.
  bytecask::Bytes out;
  REQUIRE(follower.get({}, to_bytes("user:1"), out));
  CHECK(to_string(out) == "alice");
  REQUIRE(follower.get({}, to_bytes("user:2"), out));
  CHECK(to_string(out) == "bob");
  REQUIRE(follower.get({}, to_bytes("user:3"), out));
  CHECK(to_string(out) == "carol");
}

TEST_CASE("ingest idempotency: re-ingesting is a no-op", "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("k1"), to_bytes("v1"));
  leader.put({}, to_bytes("k2"), to_bytes("v2"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);
  auto seq_after_first = follower.durable_sequence();

  // Re-ingest same entries — should be a no-op.
  follower.ingest(views);
  CHECK(follower.durable_sequence() == seq_after_first);
}

TEST_CASE("ingest with batches: BulkBegin/BulkEnd preserved", "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  bytecask::WritePlan plan;
  plan.put(to_bytes("batch:1"), to_bytes("val1"));
  plan.put(to_bytes("batch:2"), to_bytes("val2"));
  plan.put(to_bytes("batch:3"), to_bytes("val3"));
  (void)leader.apply_batch({}, std::move(plan));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  // Verify BulkBegin and BulkEnd are present.
  REQUIRE(owned.front().entry_type == bytecask::EntryType::BulkBegin);
  REQUIRE(owned.back().entry_type == bytecask::EntryType::BulkEnd);

  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);

  bytecask::Bytes out;
  REQUIRE(follower.get({}, to_bytes("batch:1"), out));
  CHECK(to_string(out) == "val1");
  REQUIRE(follower.get({}, to_bytes("batch:2"), out));
  CHECK(to_string(out) == "val2");
  REQUIRE(follower.get({}, to_bytes("batch:3"), out));
  CHECK(to_string(out) == "val3");
}

TEST_CASE("ingest with range delete", "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("a"), to_bytes("1"));
  leader.put({}, to_bytes("b"), to_bytes("2"));
  leader.put({}, to_bytes("c"), to_bytes("3"));
  leader.put({}, to_bytes("d"), to_bytes("4"));
  leader.del_range({}, to_bytes("b"), to_bytes("d"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);

  bytecask::Bytes out;
  CHECK(follower.get({}, to_bytes("a"), out));
  CHECK_FALSE(follower.get({}, to_bytes("b"), out));
  CHECK_FALSE(follower.get({}, to_bytes("c"), out));
  CHECK(follower.get({}, to_bytes("d"), out));
}

TEST_CASE("ingest sequence continuity: durable_sequence matches max ingested",
          "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("k1"), to_bytes("v1"));
  leader.put({}, to_bytes("k2"), to_bytes("v2"));
  auto leader_seq = leader.durable_sequence();

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);

  CHECK(follower.durable_sequence() == leader_seq);
}

TEST_CASE("ingest recovery equivalence: survives close and reopen",
          "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("rk1"), to_bytes("rv1"));
  leader.put({}, to_bytes("rk2"), to_bytes("rv2"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  {
    auto follower = bytecask::DB::open(td.path / "follower",
                                       {.initial_mode = bytecask::Mode::Follower});
    follower.ingest(views);
  }

  // Reopen and verify state survived recovery.
  auto follower = bytecask::DB::open(td.path / "follower");
  bytecask::Bytes out;
  REQUIRE(follower.get({}, to_bytes("rk1"), out));
  CHECK(to_string(out) == "rv1");
  REQUIRE(follower.get({}, to_bytes("rk2"), out));
  CHECK(to_string(out) == "rv2");
}

TEST_CASE("promotion continuity: first put after ingest gets next sequence",
          "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("k1"), to_bytes("v1"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);
  auto seq_after_ingest = follower.durable_sequence();

  // Promote to leader and write.
  follower.set_mode(bytecask::Mode::Leader);
  follower.put({}, to_bytes("k2"), to_bytes("v2"));

  CHECK(follower.durable_sequence() == seq_after_ingest + 1);
}

TEST_CASE("ingest triggers file rotation", "[replication]") {
  TempDir td;

  // Leader with small rotation threshold to generate multiple files.
  auto leader = bytecask::DB::open(td.path / "leader", {.max_file_bytes = 256});
  for (int i = 0; i < 50; ++i) {
    auto key = std::format("key:{:04d}", i);
    auto val = std::format("value:{:04d}", i);
    leader.put({}, to_bytes(key), to_bytes(val));
  }

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  // Follower with same small threshold — ingest should trigger rotation.
  {
    auto follower = bytecask::DB::open(td.path / "follower",
                                       {.max_file_bytes = 256,
                                        .initial_mode = bytecask::Mode::Follower});
    follower.ingest(views);

    // Verify all data is present.
    bytecask::Bytes out;
    for (int i = 0; i < 50; ++i) {
      auto key = std::format("key:{:04d}", i);
      auto val = std::format("value:{:04d}", i);
      REQUIRE(follower.get({}, to_bytes(key), out));
      CHECK(to_string(out) == val);
    }
  }

  // Verify data survives recovery (rotation created valid sealed files).
  {
    bytecask::Bytes out;
    auto reopened = bytecask::DB::open(td.path / "follower");
    for (int i = 0; i < 50; ++i) {
      auto key = std::format("key:{:04d}", i);
      auto val = std::format("value:{:04d}", i);
      REQUIRE(reopened.get({}, to_bytes(key), out));
      CHECK(to_string(out) == val);
    }
  }
}

TEST_CASE("ingest post-loop rotation: last entry tips file past threshold",
          "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");
  for (char c = 'a'; c <= 'e'; ++c)
    leader.put({}, to_bytes(std::string(1, c)), to_bytes("x"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  // 5 entries × 21 bytes = 105 bytes total. With max_file_bytes=100,
  // the last entry crosses the threshold but mid-loop guard (i+1 < size)
  // is false, so the post-loop rotation block fires instead.
  bytecask::Bytes out;
  {
    auto follower = bytecask::DB::open(
        td.path / "follower",
        {.max_file_bytes = 100, .initial_mode = bytecask::Mode::Follower});
    follower.ingest(views);

    for (char c = 'a'; c <= 'e'; ++c) {
      REQUIRE(follower.get({}, to_bytes(std::string(1, c)), out));
      CHECK(to_string(out) == "x");
    }
  }

  {
    auto reopened = bytecask::DB::open(td.path / "follower");
    for (char c = 'a'; c <= 'e'; ++c) {
      REQUIRE(reopened.get({}, to_bytes(std::string(1, c)), out));
      CHECK(to_string(out) == "x");
    }
  }
}

TEST_CASE("batch-safe rotation: batch is not split across files",
          "[replication]") {
  TempDir td;

  // Leader writes a batch that should be near the rotation threshold.
  auto leader = bytecask::DB::open(td.path / "leader", {.max_file_bytes = 256});
  // First, fill up close to the threshold with individual puts.
  for (int i = 0; i < 3; ++i) {
    auto key = std::format("pre:{}", i);
    leader.put({}, to_bytes(key), to_bytes("padding"));
  }
  // Then a batch that must stay together.
  bytecask::WritePlan plan;
  plan.put(to_bytes("batch:a"), to_bytes("A"));
  plan.put(to_bytes("batch:b"), to_bytes("B"));
  plan.put(to_bytes("batch:c"), to_bytes("C"));
  (void)leader.apply_batch({}, std::move(plan));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  // Follower with small threshold — rotation should not split the batch.
  {
    auto follower = bytecask::DB::open(td.path / "follower",
                                       {.max_file_bytes = 256,
                                        .initial_mode = bytecask::Mode::Follower});
    follower.ingest(views);

    bytecask::Bytes out;
    REQUIRE(follower.get({}, to_bytes("batch:a"), out));
    CHECK(to_string(out) == "A");
    REQUIRE(follower.get({}, to_bytes("batch:b"), out));
    CHECK(to_string(out) == "B");
    REQUIRE(follower.get({}, to_bytes("batch:c"), out));
    CHECK(to_string(out) == "C");
  }

  // Verify recovery works — batch was not split.
  {
    bytecask::Bytes out;
    auto reopened = bytecask::DB::open(td.path / "follower");
    REQUIRE(reopened.get({}, to_bytes("batch:a"), out));
    CHECK(to_string(out) == "A");
    REQUIRE(reopened.get({}, to_bytes("batch:b"), out));
    CHECK(to_string(out) == "B");
    REQUIRE(reopened.get({}, to_bytes("batch:c"), out));
    CHECK(to_string(out) == "C");
  }
}

TEST_CASE("leader-to-follower replication round-trip", "[replication]") {
  TempDir td;

  auto leader = bytecask::DB::open(td.path / "leader");

  // Mixed workload: puts, deletes, batch, range delete.
  leader.put({}, to_bytes("a"), to_bytes("1"));
  leader.put({}, to_bytes("b"), to_bytes("2"));
  leader.put({}, to_bytes("c"), to_bytes("3"));
  (void)leader.del({}, to_bytes("b"));

  bytecask::WritePlan plan;
  plan.put(to_bytes("d"), to_bytes("4"));
  plan.put(to_bytes("e"), to_bytes("5"));
  (void)leader.apply_batch({}, std::move(plan));

  leader.put({}, to_bytes("f"), to_bytes("6"));
  leader.put({}, to_bytes("g"), to_bytes("7"));
  leader.del_range({}, to_bytes("f"), to_bytes("g"));

  auto snap = leader.snapshot();
  auto changes = leader.changes_since(snap, 0);

  struct OwnedEntry {
    std::uint64_t sequence;
    bytecask::EntryType entry_type;
    bytecask::Bytes key;
    bytecask::Bytes value;
  };
  std::vector<OwnedEntry> owned;
  for (const auto &e : changes) {
    owned.push_back({e.sequence, e.entry_type,
                     bytecask::Bytes{e.key.begin(), e.key.end()},
                     bytecask::Bytes{e.value.begin(), e.value.end()}});
  }
  std::vector<bytecask::DataEntryView> views;
  for (const auto &o : owned) {
    views.push_back({o.sequence, o.entry_type, o.key, o.value});
  }

  auto follower = bytecask::DB::open(td.path / "follower",
                                     {.initial_mode = bytecask::Mode::Follower});
  follower.ingest(views);

  // Verify key-value equivalence with leader.
  bytecask::Bytes leader_out, follower_out;
  for (const auto &key_str : {"a", "b", "c", "d", "e", "f", "g"}) {
    auto key = to_bytes(key_str);
    auto leader_found = leader.get({}, key, leader_out);
    auto follower_found = follower.get({}, key, follower_out);
    CHECK(leader_found == follower_found);
    if (leader_found && follower_found) {
      CHECK(leader_out == follower_out);
    }
  }

  CHECK(follower.durable_sequence() == leader.durable_sequence());
}

// ---------------------------------------------------------------------------
// DataFileIterator tests
// ---------------------------------------------------------------------------

TEST_CASE("DataFileIterator over empty file yields nothing", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "empty.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;

  std::vector<bytecask::DataEntry> entries;
  for (const auto& [entry, off] : bytecask::scan_entries(file)) {
    entries.push_back(entry);
  }
  REQUIRE(entries.empty());
}

TEST_CASE("DataFileIterator yields all entries in order", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  (void)file.append_entry(1, bytecask::EntryType::Put,
                          to_bytes("k1"), to_bytes("v1"));
  (void)file.append_entry(2, bytecask::EntryType::Put,
                          to_bytes("k2"), to_bytes("v2"));
  (void)file.append_entry(3, bytecask::EntryType::Delete,
                          to_bytes("k1"), {});

  std::vector<std::pair<std::uint64_t, bytecask::EntryType>> results;
  for (const auto& [entry, off] : bytecask::scan_entries(file)) {
    results.emplace_back(entry.sequence, entry.entry_type);
  }

  REQUIRE(results.size() == 3);
  CHECK(results[0] == std::pair{std::uint64_t{1}, bytecask::EntryType::Put});
  CHECK(results[1] == std::pair{std::uint64_t{2}, bytecask::EntryType::Put});
  CHECK(results[2] == std::pair{std::uint64_t{3}, bytecask::EntryType::Delete});
}

TEST_CASE("DataFileIterator reports correct offsets", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  auto off1 = file.append_entry(1, bytecask::EntryType::Put,
                                to_bytes("a"), to_bytes("1"));
  auto off2 = file.append_entry(2, bytecask::EntryType::Put,
                                to_bytes("b"), to_bytes("2"));

  std::vector<bytecask::Offset> offsets;
  for (const auto& [entry, off] : bytecask::scan_entries(file)) {
    offsets.push_back(off);
  }

  REQUIRE(offsets.size() == 2);
  CHECK(offsets[0] == off1);
  CHECK(offsets[1] == off2);
}

// ---------------------------------------------------------------------------
// CommittedEntryIterator tests
// ---------------------------------------------------------------------------

TEST_CASE("scan_committed standalone entries yield individual entries",
          "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  (void)file.append_entry(1, bytecask::EntryType::Put,
                          to_bytes("k1"), to_bytes("v1"));
  (void)file.append_entry(2, bytecask::EntryType::Delete,
                          to_bytes("k2"), {});

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }

  REQUIRE(entries.size() == 2);
  CHECK(entries[0].first.sequence == 1);
  CHECK(entries[0].first.entry_type == bytecask::EntryType::Put);
  CHECK(entries[1].first.sequence == 2);
  CHECK(entries[1].first.entry_type == bytecask::EntryType::Delete);
}

TEST_CASE("scan_committed yields BulkBegin/BulkEnd as regular entries",
          "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  (void)file.append_entry(10, bytecask::EntryType::BulkBegin, {}, {});
  (void)file.append_entry(11, bytecask::EntryType::Put,
                          to_bytes("k1"), to_bytes("v1"));
  (void)file.append_entry(12, bytecask::EntryType::Put,
                          to_bytes("k2"), to_bytes("v2"));
  (void)file.append_entry(13, bytecask::EntryType::BulkEnd, {}, {});

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }

  REQUIRE(entries.size() == 4);
  CHECK(entries[0].first.entry_type == bytecask::EntryType::BulkBegin);
  CHECK(entries[0].first.sequence == 10);
  CHECK(entries[1].first.entry_type == bytecask::EntryType::Put);
  CHECK(entries[1].first.sequence == 11);
  CHECK(entries[2].first.entry_type == bytecask::EntryType::Put);
  CHECK(entries[2].first.sequence == 12);
  CHECK(entries[3].first.entry_type == bytecask::EntryType::BulkEnd);
  CHECK(entries[3].first.sequence == 13);
}

TEST_CASE("scan_committed discards incomplete batch at EOF", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  // Standalone entry first, then an incomplete batch.
  (void)file.append_entry(1, bytecask::EntryType::Put,
                          to_bytes("k1"), to_bytes("v1"));
  (void)file.append_entry(10, bytecask::EntryType::BulkBegin, {}, {});
  (void)file.append_entry(11, bytecask::EntryType::Put,
                          to_bytes("k2"), to_bytes("v2"));
  // No BulkEnd — batch is incomplete.

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }

  // Only the standalone entry should be yielded.
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].first.sequence == 1);
}

TEST_CASE("scan_committed interleaved standalone and batch entries",
          "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  (void)file.append_entry(1, bytecask::EntryType::Put,
                          to_bytes("standalone1"), to_bytes("v1"));
  (void)file.append_entry(10, bytecask::EntryType::BulkBegin, {}, {});
  (void)file.append_entry(11, bytecask::EntryType::Put,
                          to_bytes("batch1"), to_bytes("v2"));
  (void)file.append_entry(12, bytecask::EntryType::BulkEnd, {}, {});
  (void)file.append_entry(20, bytecask::EntryType::Delete,
                          to_bytes("standalone2"), {});

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }

  REQUIRE(entries.size() == 5);
  CHECK(entries[0].first.entry_type == bytecask::EntryType::Put);
  CHECK(entries[1].first.entry_type == bytecask::EntryType::BulkBegin);
  CHECK(entries[2].first.entry_type == bytecask::EntryType::Put);
  CHECK(entries[3].first.entry_type == bytecask::EntryType::BulkEnd);
  CHECK(entries[4].first.entry_type == bytecask::EntryType::Delete);
}

TEST_CASE("scan_committed committed_offset tracks last committed position",
          "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  (void)file.append_entry(1, bytecask::EntryType::Put,
                          to_bytes("k"), to_bytes("v"));
  (void)file.append_entry(10, bytecask::EntryType::BulkBegin, {}, {});
  (void)file.append_entry(11, bytecask::EntryType::Put,
                          to_bytes("bk"), to_bytes("bv"));
  (void)file.append_entry(12, bytecask::EntryType::BulkEnd, {}, {});
  auto file_size = file.size();

  auto iter = bytecask::CommittedEntryIterator{bytecask::DataFileIterator{file}};
  while (!(iter == std::default_sentinel)) {
    ++iter;
  }
  // After exhaustion, committed_offset should equal file size.
  CHECK(iter.committed_offset() == file_size);
}

TEST_CASE("scan_committed over empty file yields nothing", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "empty.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }
  REQUIRE(entries.empty());
}

TEST_CASE("scan_committed handles RangeDel inside batch", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::openDataFileForWrite(td.path / "test.data", 0, bytecask::IoBackend::Pread); auto &file = *file_ptr;
  (void)file.append_entry(10, bytecask::EntryType::BulkBegin, {}, {});
  (void)file.append_entry(11, bytecask::EntryType::Put,
                          to_bytes("k1"), to_bytes("v1"));
  (void)file.append_entry(12, bytecask::EntryType::RangeDel,
                          to_bytes("a"), to_bytes("z"));
  (void)file.append_entry(13, bytecask::EntryType::BulkEnd, {}, {});

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }

  REQUIRE(entries.size() == 4);
  CHECK(entries[0].first.entry_type == bytecask::EntryType::BulkBegin);
  CHECK(entries[1].first.entry_type == bytecask::EntryType::Put);
  CHECK(entries[2].first.entry_type == bytecask::EntryType::RangeDel);
  CHECK(entries[3].first.entry_type == bytecask::EntryType::BulkEnd);
}

// ---------------------------------------------------------------------------
// Size limits
// ---------------------------------------------------------------------------

TEST_CASE("Size limits: put rejects oversized key", "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_key_bytes = 8});
  CHECK_NOTHROW(db.put({.sync = false}, to_bytes("12345678"), to_bytes("v")));
  CHECK_THROWS_AS(
      db.put({.sync = false}, to_bytes("123456789"), to_bytes("v")),
      std::invalid_argument);
}

TEST_CASE("Size limits: put rejects oversized value", "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_value_bytes = 4});
  CHECK_NOTHROW(db.put({.sync = false}, to_bytes("k"), to_bytes("1234")));
  CHECK_THROWS_AS(
      db.put({.sync = false}, to_bytes("k"), to_bytes("12345")),
      std::invalid_argument);
}

TEST_CASE("Size limits: del rejects oversized key", "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_key_bytes = 4});
  CHECK_THROWS_AS(db.del({.sync = false}, to_bytes("12345")),
                  std::invalid_argument);
}

TEST_CASE("Size limits: del_range rejects oversized boundary",
          "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_key_bytes = 4});
  CHECK_THROWS_AS(
      db.del_range({.sync = false}, to_bytes("12345"), to_bytes("z")),
      std::invalid_argument);
  CHECK_THROWS_AS(
      db.del_range({.sync = false}, to_bytes("a"), to_bytes("12345")),
      std::invalid_argument);
}

TEST_CASE("Size limits: WritePlan validates from snapshot limits",
          "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_key_bytes = 8});
  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  CHECK_NOTHROW(plan.put(to_bytes("12345678"), to_bytes("v")));
  CHECK_THROWS_AS(plan.put(to_bytes("123456789"), to_bytes("v")),
                  std::invalid_argument);
}

TEST_CASE("Size limits: WritePlan default uses default limits",
          "[bytecask][limits]") {
  // Default WritePlan (no DB) uses kDefaultMaxKeyBytes = 4096.
  bytecask::WritePlan plan;
  std::string key_at_limit(4096, 'k');
  std::string key_over_limit(4097, 'k');
  CHECK_NOTHROW(plan.put(to_bytes(key_at_limit), to_bytes("v")));
  CHECK_THROWS_AS(plan.put(to_bytes(key_over_limit), to_bytes("v")),
                  std::invalid_argument);
}

TEST_CASE("Size limits: guard methods validate key size",
          "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_key_bytes = 4});
  auto snap = db.snapshot();
  bytecask::WritePlan plan{std::move(snap)};
  CHECK_THROWS_AS(plan.ensure_present(to_bytes("12345")),
                  std::invalid_argument);
  CHECK_THROWS_AS(plan.ensure_absent(to_bytes("12345")),
                  std::invalid_argument);
  CHECK_THROWS_AS(plan.ensure_unchanged(to_bytes("12345")),
                  std::invalid_argument);
  CHECK_THROWS_AS(
      plan.ensure_range_unchanged(to_bytes("12345"), to_bytes("z")),
      std::invalid_argument);
}

TEST_CASE("Size limits: hard ceiling clamps user value",
          "[bytecask][limits]") {
  TempDir td;
  // User passes a value larger than the wire format ceiling.
  // DB::open clamps it to kMaxKeySize.
  auto db = bytecask::DB::open(td.path, {.max_key_bytes = 100000});
  // A 65535-byte key should be accepted (at wire format ceiling).
  std::string key_at_hard_limit(65535, 'k');
  CHECK_NOTHROW(
      db.put({.sync = false}, to_bytes(key_at_hard_limit), to_bytes("v")));
}

// ---------------------------------------------------------------------------
// stats() — operational counters
// ---------------------------------------------------------------------------

TEST_CASE("stats: fresh DB has zero counters and one open file",
          "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  auto s = db.stats();
  CHECK(s.at("bytecask.bytes_written") == 0);
  CHECK(s.at("bytecask.group_writer_batches") == 0);
  CHECK(s.at("bytecask.group_writer_coalesced") == 0);
  CHECK(s.at("bytecask.fsyncs") == 0);
  CHECK(s.at("bytecask.disk_reads") == 0);
  CHECK(s.at("bytecask.disk_read_bytes") == 0);
  CHECK(s.at("bytecask.crc_failures") == 0);
  CHECK(s.at("bytecask.io_errors") == 0);
  CHECK(s.at("bytecask.degraded_transitions") == 0);
  CHECK(s.at("bytecask.degraded") == 0);
  // Fresh DB: recovery found no files, but we opened one active file.
  CHECK(s.at("bytecask.recovery_files") == 0);
  CHECK(s.at("bytecask.recovery_keys") == 0);
  CHECK(s.at("bytecask.files_opened") == 1);
  CHECK(s.at("bytecask.open_files") == 1);
}

TEST_CASE("stats: write counters increment on put",
          "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  auto s = db.stats();
  CHECK(s.at("bytecask.bytes_written") > 0);
  CHECK(s.at("bytecask.group_writer_batches") >= 2);
  CHECK(s.at("bytecask.group_writer_coalesced") >= 2);
  CHECK(s.at("bytecask.fsyncs") >= 2);
}

TEST_CASE("stats: disk_reads and disk_read_bytes increment on get",
          "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  db.put({.sync = false}, to_bytes("k1"), to_bytes("hello"));
  db.put({.sync = false}, to_bytes("k2"), to_bytes("world"));

  bytecask::Bytes out;
  (void)db.get({}, to_bytes("k1"), out);
  (void)db.get({}, to_bytes("k2"), out);

  auto s = db.stats();
  CHECK(s.at("bytecask.disk_reads") == 2);
  CHECK(s.at("bytecask.disk_read_bytes") == 10);  // "hello" + "world"
}

TEST_CASE("stats: recovery counters after reopen", "[bytecask][stats]") {
  TempDir td;
  {
    auto db = bytecask::DB::open(td.path);
    for (int i = 0; i < 100; ++i) {
      auto key = std::format("k{:04d}", i);
      db.put({.sync = false}, to_bytes(key), to_bytes("v"));
    }
  }
  auto db = bytecask::DB::open(td.path);
  auto s = db.stats();
  CHECK(s.at("bytecask.recovery_files") >= 1);
  CHECK(s.at("bytecask.recovery_keys") == 100);
  CHECK(s.at("bytecask.recovery_duration_us") > 0);
}

TEST_CASE("stats: vacuum counters", "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_file_bytes = 256});
  // Write enough keys to rotate at least one file, then overwrite them all.
  for (int i = 0; i < 50; ++i) {
    auto key = std::format("k{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes("original"));
  }
  // Overwrite all keys so the old files are fully dead.
  for (int i = 0; i < 50; ++i) {
    auto key = std::format("k{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes("updated"));
  }
  // Run vacuum until nothing qualifies.
  {
    int spins = 0;
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
      REQUIRE(++spins < 200);  // vacuum must converge, not spin
    }
  }
  auto s = db.stats();
  CHECK(s.at("bytecask.vacuum_files_unlinked") > 0);
  CHECK(s.at("bytecask.vacuum_bytes_reclaimed") > 0);
}

TEST_CASE("stats: file_rotations increments", "[bytecask][stats]") {
  TempDir td;
  // Very small rotation threshold to force rotations.
  auto db = bytecask::DB::open(td.path, {.max_file_bytes = 64});
  for (int i = 0; i < 20; ++i) {
    auto key = std::format("k{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes("value"));
  }
  auto s = db.stats();
  CHECK(s.at("bytecask.file_rotations") > 0);
  CHECK(s.at("bytecask.files_opened") > 1);
}

TEST_CASE("stats: all expected keys are present in dump",
          "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  auto s = db.stats();
  std::vector<std::string> expected = {
      "bytecask.keydir_keys",
      "bytecask.keydir_versions_live",
      "bytecask.keydir_nodes_parked",
      "bytecask.bytes_written",
      "bytecask.group_writer_batches",
      "bytecask.group_writer_coalesced",
      "bytecask.file_rotations",
      "bytecask.fsyncs",
      "bytecask.commit_wait_blocked",
      "bytecask.disk_reads",
      "bytecask.disk_read_bytes",
      "bytecask.pool_hits",
      "bytecask.pool_misses",
      "bytecask.pool_fills",
      "bytecask.pool_evictions",
      "bytecask.pool_frames_total",
      "bytecask.pool_frames_resident",
      "bytecask.pool_direct_io_fallbacks",
      "bytecask.vacuum_bytes_reclaimed",
      "bytecask.vacuum_files_unlinked",
      "bytecask.recovery_files",
      "bytecask.recovery_keys",
      "bytecask.recovery_duration_us",
      "bytecask.files_opened",
      "bytecask.crc_failures",
      "bytecask.io_errors",
      "bytecask.degraded_transitions",
      "bytecask.degraded",
      "bytecask.open_files",
  };
  for (const auto &name : expected) {
    CHECK(s.contains(name));
  }
  CHECK(s.size() == expected.size());
}

// ---------------------------------------------------------------------------
// ReadOptions: verify_checksums=false exercises read_entry_unverified paths
// ---------------------------------------------------------------------------

TEST_CASE("iter_from and riter_from with verify_checksums=false",
          "[bytecask]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path, {.max_file_bytes = 64});

  for (int i = 0; i < 20; ++i) {
    auto key = std::format("key:{:04d}", i);
    auto val = std::format("val:{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes(val));
  }

  bytecask::ReadOptions ropts{.verify_checksums = false};

  std::vector<std::string> forward_keys;
  for (const auto &[key, value] : db.iter_from(ropts)) {
    forward_keys.push_back(to_string(key));
  }
  CHECK(forward_keys.size() == 20);
  CHECK(std::is_sorted(forward_keys.begin(), forward_keys.end()));

  std::vector<std::string> reverse_keys;
  for (const auto &[key, value] : db.riter_from(ropts)) {
    reverse_keys.push_back(to_string(key));
  }
  CHECK(reverse_keys.size() == 20);
  CHECK(std::is_sorted(reverse_keys.begin(), reverse_keys.end(),
                        std::greater<>{}));
}

// ---------------------------------------------------------------------------
// Options: io_backend
// ---------------------------------------------------------------------------

TEST_CASE("io_backend=Pread: put/get with file rotation",
          "[bytecask][no_mmap]") {
  TempDir td;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 64, .io_backend = bytecask::IoBackend::Pread});
  constexpr int kCount = 50;
  for (int i = 0; i < kCount; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes(val));
  }
  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    REQUIRE(db.get({}, to_bytes(key), out));
    CHECK(to_string(out) == val);
  }
}

TEST_CASE("io_backend=Mmap: put/get round-trip",
          "[bytecask][mmap]") {
  TempDir td;
#ifdef __EMSCRIPTEN__
  // WASM/Emscripten builds never support mmap: mmap emulation would
  // double-buffer the data file into the WASM heap rather than avoiding a
  // copy, so DB::open rejects the option instead of silently ignoring it.
  CHECK_THROWS_AS(bytecask::DB::open(td.path, {.io_backend = bytecask::IoBackend::Mmap}),
                  std::invalid_argument);
#else
  auto db = bytecask::DB::open(td.path, {.io_backend = bytecask::IoBackend::Mmap});
  constexpr int kCount = 20;
  for (int i = 0; i < kCount; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes(val));
  }
  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    REQUIRE(db.get({}, to_bytes(key), out));
    CHECK(to_string(out) == val);
  }
#endif
}

TEST_CASE("io_backend=BufferPool: a pool smaller than 2x max_file_bytes is rejected",
          "[bytecask][buffer_pool]") {
  TempDir td;
  // Below this the active file alone would consume half the pool, leaving a
  // cache that silently does nothing. Rejecting beats degrading.
  CHECK_THROWS_AS(
      bytecask::DB::open(
          td.path,
          {.max_file_bytes = 8 * 1024 * 1024,
           .io_backend = bytecask::IoBackend::BufferPool,
           .buffer_pool = {.capacity_bytes = 4 * 1024 * 1024}}),
      std::invalid_argument);
}

TEST_CASE("io_backend=BufferPool: put/get round-trip across rotation",
          "[bytecask][buffer_pool]") {
  TempDir td;
#ifdef __EMSCRIPTEN__
  CHECK_THROWS_AS(
      bytecask::DB::open(td.path,
                         {.io_backend = bytecask::IoBackend::BufferPool,
                          .buffer_pool = {.capacity_bytes = 4 * 1024 * 1024}}),
      std::invalid_argument);
#else
  // Small files so the reads under test land on sealed, pool-backed files
  // rather than on the active file.
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 64 * 1024,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 1024 * 1024}});
  constexpr int kCount = 2000;
  const auto value_for = [](int i) {
    return std::format("v{:05d}", i) + std::string(200, 'x');
  };
  for (int i = 0; i < kCount; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(value_for(i)));
  }
  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
  // Re-read the same keys: now served from frames the first pass admitted.
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
  // Rotation must actually have happened, or the reads never left the
  // active file and this proves nothing about the pool.
  CHECK(db.stats().at("bytecask.file_rotations") > 0);
  const auto stats = db.stats();
  CHECK(stats.at("bytecask.pool_frames_total") > 0);
  CHECK(stats.at("bytecask.pool_hits") > 0);
#endif
}

TEST_CASE("io_backend=BufferPool: values survive recovery",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  TempDir td;
  constexpr int kCount = 500;
  {
    auto db = bytecask::DB::open(
        td.path, {.max_file_bytes = 32 * 1024,
                  .io_backend = bytecask::IoBackend::BufferPool,
                  .buffer_pool = {.capacity_bytes = 512 * 1024}});
    for (int i = 0; i < kCount; ++i) {
      db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
             to_bytes(std::format("v{:05d}", i) + std::string(200, 'x')));
    }
  }
  // Reopening rebuilds from hint files and re-registers every sealed file with
  // a fresh pool; cache ids are per-pool, so nothing carries over.
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 32 * 1024,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 512 * 1024}});
  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) ==
          std::format("v{:05d}", i) + std::string(200, 'x'));
  }
#endif
}

TEST_CASE("io_backend=BufferPool: vacuum does not pollute the pool",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // A vacuum pass sweeps whole files. If scan() admitted those frames it would
  // evict the working set every pass, so scan reads straight from the fd.
  TempDir td;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 32 * 1024,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 1024 * 1024}});
  constexpr int kCount = 600;
  const auto value_for = [](int i) {
    return std::format("v{:05d}", i) + std::string(200, 'x');
  };
  for (int i = 0; i < kCount; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(value_for(i)));
  }
  for (int i = 0; i < kCount; i += 2) {
    (void)db.del({.sync = false}, to_bytes(std::format("k{:05d}", i)));
  }
  // Populate the pool with the reads we care about keeping.
  bytecask::Bytes out;
  for (int i = 1; i < kCount; i += 2) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
  }
  // With active-file residency the writer put every frame there and these
  // reads all hit, so "fills" is not the precondition — residency is.
  const auto before = db.stats();
  REQUIRE(before.at("bytecask.pool_frames_resident") > 0);
  const auto fills_before = before.at("bytecask.pool_fills");

  bool vacuumed = false;
  // Threshold 0 so a pass definitely runs — a no-op vacuum would prove nothing.
  while (db.vacuum({.fragmentation_threshold = 0.0})) {
    vacuumed = true;
  }
  REQUIRE(vacuumed);

  // The sweep must not have admitted a single frame — not through a read
  // fill, and not through the compaction writer, which is created without
  // a pool on purpose.
  const auto after = db.stats();
  CHECK(after.at("bytecask.pool_fills") == fills_before);

  // ... and the data is still correct afterwards.
  for (int i = 1; i < kCount; i += 2) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
#endif
}

TEST_CASE("io_backend=BufferPool: verify_checksums=false read paths",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // Exercises read_value's unverified branch and read_entry_unverified, which
  // the CRC-verifying default never reaches.
  TempDir td;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 32 * 1024,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 512 * 1024}});
  constexpr int kCount = 400;
  const auto value_for = [](int i) {
    return std::format("v{:05d}", i) + std::string(200, 'x');
  };
  for (int i = 0; i < kCount; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(value_for(i)));
  }
  const bytecask::ReadOptions no_crc{.verify_checksums = false};

  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(db.get(no_crc, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
  // Forward and reverse iteration both take read_entry_unverified here.
  int seen = 0;
  for (auto &[key, value] : db.iter_from(no_crc)) {
    CHECK(to_string(value).size() == value_for(0).size());
    ++seen;
  }
  CHECK(seen == kCount);
  int rseen = 0;
  for (auto &[key, value] : db.riter_from(no_crc, to_bytes("k~"))) {
    (void)key;
    (void)value;
    ++rseen;
  }
  CHECK(rseen == kCount);
#endif
}

namespace {
// Whether the test directory's filesystem serves uncached reads. Where it does
// not, the pool falls back per file and the direct-path assertions below do
// not apply — the fallback itself is what is under test there. Asks the same
// function the engine does, so the two can never disagree about a mount.
auto temp_dir_supports_direct_io() -> bool {
  static constexpr std::size_t kProbeBytes = 8192;
  const auto path = std::filesystem::temp_directory_path() / "bc_odirect_probe";
  {
    std::ofstream f{path, std::ios::binary};
    f << std::string(kProbeBytes, 'p');
  }
  const auto fd = bytecask::open_uncached(path, kProbeBytes);
  const bool ok = fd != -1;
  if (ok) ::close(fd);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return ok;
}
}  // namespace

TEST_CASE("io_backend=BufferPool: direct I/O fills serve identical bytes",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // Same workload through O_DIRECT fills and through buffered fills; both
  // must agree with each other and with what was written. The O_DIRECT path
  // reads block multiples past EOF and copies out of an aligned scratch, so
  // the file tail is the part most worth checking.
  const bool direct_supported = temp_dir_supports_direct_io();
  auto run = [](bool direct_io) {
    TempDir td;
    auto db = bytecask::DB::open(
        td.path, {.max_file_bytes = 32 * 1024,
                  .io_backend = bytecask::IoBackend::BufferPool,
                  .buffer_pool = {.capacity_bytes = 512 * 1024,
                                  .direct_io = direct_io}});
    constexpr int kCount = 700;
    // Odd sizes so entries straddle frame boundaries and files end unaligned.
    const auto value_for = [](int i) {
      return std::format("v{:05d}", i) + std::string(37 + (i % 211), 'x');
    };
    for (int i = 0; i < kCount; ++i) {
      db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
             to_bytes(value_for(i)));
    }
    std::vector<std::string> seen;
    bytecask::Bytes out;
    for (int i = 0; i < kCount; ++i) {
      REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
      CHECK(to_string(out) == value_for(i));
      seen.push_back(to_string(out));
    }
    const auto st = db.stats();
    REQUIRE(st.at("bytecask.file_rotations") > 0);
    return std::make_pair(seen, st.at("bytecask.pool_direct_io_fallbacks"));
  };
  const auto [direct_seen, direct_fallbacks] = run(true);
  const auto [buffered_seen, buffered_fallbacks] = run(false);
  CHECK(direct_seen == buffered_seen);
  // direct_io=false never attempts O_DIRECT, so it can never fall back.
  CHECK(buffered_fallbacks == 0);
  if (direct_supported) {
    // On a filesystem that serves O_DIRECT every sealed file must have taken
    // it — otherwise the pool was measured with the page cache underneath.
    CHECK(direct_fallbacks == 0);
  } else {
    WARN("temp dir refuses O_DIRECT: fallback path exercised, direct path not");
  }
#endif
}

TEST_CASE("stats: keydir gauges track the live key count",
          "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  CHECK(db.stats().at("bytecask.keydir_keys") == 0);
  for (int i = 0; i < 100; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:03d}", i)), to_bytes("v"));
  }
  auto st = db.stats();
  CHECK(st.at("bytecask.keydir_keys") == 100);
  for (int i = 0; i < 40; ++i) {
    (void)db.del({.sync = false}, to_bytes(std::format("k{:03d}", i)));
  }
  st = db.stats();
  CHECK(st.at("bytecask.keydir_keys") == 60);
}

TEST_CASE("io_backend=BufferPool: the active file is resident on write",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // Read-your-own-writes must never miss: every byte of the active file went
  // into the pool as it was appended. Small values so one frame is extended
  // in place many times; a second batch large enough to straddle frames.
  TempDir td;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 8 * 1024 * 1024,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 32 * 1024 * 1024}});
  const auto value_for = [](int i) {
    return std::format("v{:05d}", i) + std::string(i < 400 ? 20 : 900, 'x');
  };
  constexpr int kCount = 600;
  for (int i = 0; i < kCount; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(value_for(i)));
  }
  auto st = db.stats();
  REQUIRE(st.at("bytecask.file_rotations") == 0);  // all in the active file
  CHECK(st.at("bytecask.pool_frames_resident") > 0);

  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
  st = db.stats();
  CHECK(st.at("bytecask.pool_misses") == 0);
  CHECK(st.at("bytecask.pool_hits") == kCount);
  // Nothing was ever filled by a read: the writer put it all there.
  CHECK(st.at("bytecask.pool_fills") == 0);
  // Batches take the other append path; same guarantee.
  bytecask::WritePlan plan;
  for (int i = 0; i < 50; ++i) {
    plan.put(to_bytes(std::format("b{:05d}", i)), to_bytes(value_for(i)));
  }
  REQUIRE(db.apply_batch({.sync = false}, std::move(plan)).has_value());
  for (int i = 0; i < 50; ++i) {
    REQUIRE(db.get({}, to_bytes(std::format("b{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
  CHECK(db.stats().at("bytecask.pool_misses") == 0);
#endif
}

TEST_CASE("io_backend=BufferPool: rotation releases the previous active file",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // The pool is barely larger than the 2 x max_file_bytes floor, so it can
  // hold the active file and about one sealed one. Writing through several
  // rotations must evict sealed frames — never pinned ones — and every read
  // must still be right.
  TempDir td;
  constexpr std::uint64_t kFile = 64 * 1024;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = kFile,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 2 * kFile + 32 * 1024}});
  const auto value_for = [](int i) {
    return std::format("v{:05d}", i) + std::string(300, 'x');
  };
  constexpr int kCount = 1500;  // ~470 KiB: many rotations
  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(value_for(i)));
  }
  const auto st = db.stats();
  REQUIRE(st.at("bytecask.file_rotations") >= 4);
  CHECK(st.at("bytecask.pool_evictions") > 0);
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
    CHECK(to_string(out) == value_for(i));
  }
#endif
}

TEST_CASE("io_backend=BufferPool: iteration and vacuum agree with pread",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // The pool must be byte-identical to pread through every read path, so the
  // same workload under both back-ends has to produce the same key/value set.
  constexpr int kCount = 800;
  auto run = [](bytecask::IoBackend backend) {
    TempDir td;
    bytecask::Options opts{.max_file_bytes = 32 * 1024, .io_backend = backend};
    if (backend == bytecask::IoBackend::BufferPool) {
      opts.buffer_pool.capacity_bytes = 256 * 1024;  // forces eviction
    }
    auto db = bytecask::DB::open(td.path, opts);
    for (int i = 0; i < kCount; ++i) {
      db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
             to_bytes(std::format("v{:05d}", i) + std::string(200, 'x')));
    }
    for (int i = 0; i < kCount; i += 3) {
      (void)db.del({.sync = false}, to_bytes(std::format("k{:05d}", i)));
    }
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
    }
    std::vector<std::string> seen;
    for (auto &[key, value] : db.iter_from({})) {
      seen.push_back(to_string(key) + "=" + to_string(value));
    }
    return seen;
  };
  CHECK(run(bytecask::IoBackend::BufferPool) ==
        run(bytecask::IoBackend::Pread));
#endif
}

TEST_CASE("io_backend=BufferPool: lent entry spans hold while readers evict",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // An iterator over the pool is handed spans into frames, not copies. The
  // frame under a span must stay put for as long as the iterator is on that
  // entry, while other readers miss and evict around it: the iterator is
  // deliberately slow — it re-reads its spans after other readers have
  // churned the pool — and every byte it sees must still be the entry's.
  constexpr int kCount = 2000;
  TempDir td;
  bytecask::Options opts{.max_file_bytes = 32 * 1024,
                         .io_backend = bytecask::IoBackend::BufferPool};
  opts.buffer_pool.capacity_bytes = 128 * 1024;  // a few frames: evicts hard
  auto db = bytecask::DB::open(td.path, opts);
  for (int i = 0; i < kCount; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(std::format("v{:05d}", i) + std::string(300, 'x')));
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> churn;
  for (int t = 0; t < 4; ++t) {
    churn.emplace_back([&db, &stop, t] {
      std::mt19937 rng{static_cast<unsigned>(1000 + t)};
      bytecask::Bytes out;
      while (!stop.load(std::memory_order_relaxed)) {
        const auto i = static_cast<int>(rng() % kCount);
        REQUIRE(db.get({}, to_bytes(std::format("k{:05d}", i)), out));
      }
    });
  }

  const auto stats_before = db.stats();
  int seen = 0;
  for (auto &[key, value] : db.iter_from({.verify_checksums = false})) {
    const auto first = to_string(key) + "=" + to_string(value);
    // Give the churn time to evict everything it can around this frame.
    bytecask::Bytes out;
    for (int j = 0; j < 20; ++j) {
      (void)db.get({}, to_bytes(std::format("k{:05d}", (seen * 7 + j) % kCount)), out);
    }
    const auto again = to_string(key) + "=" + to_string(value);
    REQUIRE(again == first);
    REQUIRE(first == std::format("k{:05d}=v{:05d}", seen, seen) + std::string(300, 'x'));
    ++seen;
  }
  stop.store(true);
  for (auto &th : churn) th.join();
  CHECK(seen == kCount);
  // The point of the sizing: eviction really ran underneath the iterator.
  CHECK(db.stats().at("bytecask.pool_evictions") >
        stats_before.at("bytecask.pool_evictions"));
#endif
}

TEST_CASE("io_backend=BufferPool: an iterator outlives the DB with its spans",
          "[bytecask][buffer_pool]") {
#ifndef __EMSCRIPTEN__
  // CONTRACT.md: views already taken stay valid and readable after ~DB. For
  // the pool that means the frames a lent span points into, the pool that
  // owns them and the counters a hit bumps must all outlive the DB, held
  // through the iterator's data file. Both a span taken before destruction
  // and entries read after it have to be right.
  TempDir td;
  bytecask::Options opts{.max_file_bytes = 32 * 1024,
                         .io_backend = bytecask::IoBackend::BufferPool};
  opts.buffer_pool.capacity_bytes = 256 * 1024;
  bytecask::EntryIterator it;
  bytecask::BytesView first_value;
  {
    auto db = bytecask::DB::open(td.path, opts);
    for (int i = 0; i < 300; ++i) {
      db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
             to_bytes(std::format("v{:05d}", i) + std::string(200, 'x')));
    }
    auto range = db.iter_from({.verify_checksums = false});
    it = range.begin();
    first_value = (*it).value;  // a span into a frame, lease held
    REQUIRE(to_string(first_value) == "v00000" + std::string(200, 'x'));
  }  // ~DB

  CHECK(to_string(first_value) == "v00000" + std::string(200, 'x'));
  int seen = 1;
  for (++it; it != std::default_sentinel; ++it, ++seen) {
    REQUIRE(to_string((*it).key) == std::format("k{:05d}", seen));
    REQUIRE(to_string((*it).value) ==
            std::format("v{:05d}", seen) + std::string(200, 'x'));
  }
  CHECK(seen == 300);
#endif
}

// A thread's cached read snapshot pins the key directory version it last
// saw. Under writes that version comes to hold a whole retained copy of the
// tree — a MariaDB thread parked in the server's thread cache did exactly
// that, at ~1.1 GB — so the engine takes the cache away from a thread that
// has gone idle, without that thread's help: the scrape that runs on
// publish obsoletes any slot unused for ~1 s. The idle thread here does
// nothing after its one read; the two stats gauges show its version go.
TEST_CASE("an idle thread's cached version is reclaimed without its help",
          "[bytecask][reclamation]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  for (int i = 0; i < 2000; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(std::format("v{:05d}", i)));
  }

  // A second thread reads once, then idles with its snapshot cached.
  std::mutex mu;
  std::condition_variable cv;
  int step = 0;
  std::thread idle{[&] {
    bytecask::Bytes out;
    REQUIRE(db.get({}, to_bytes("k00001"), out));
    { std::lock_guard<std::mutex> lk{mu}; step = 1; }
    cv.notify_all();
    { std::unique_lock<std::mutex> lk{mu}; cv.wait(lk, [&] { return step == 2; }); }
  }};
  { std::unique_lock<std::mutex> lk{mu}; cv.wait(lk, [&] { return step == 1; }); }

  // Overwrite every key: each write retires nodes the idle thread's
  // version still reaches, and nothing can free them while it is held.
  for (int i = 0; i < 2000; ++i) {
    db.put({.sync = false}, to_bytes(std::format("k{:05d}", i)),
           to_bytes(std::format("w{:05d}", i)));
  }
  // Whether the idle thread's version is still held here depends on how
  // long the overwrites took against the idle grace (under a sanitizer they
  // take longer than it), so it is reported, not asserted.
  auto st = db.stats();
  INFO("after overwrite: versions_live="
       << st.at("bytecask.keydir_versions_live")
       << " nodes_parked=" << st.at("bytecask.keydir_nodes_parked"));

  // Keep publishing past the idle grace. The idle thread is not involved;
  // the writer's scrapes take its cache and the version it pinned goes.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  bool reclaimed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    db.put({.sync = false}, to_bytes("tick"), to_bytes("t"));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    st = db.stats();
    if (st.at("bytecask.keydir_versions_live") == 1 &&
        st.at("bytecask.keydir_nodes_parked") == 0) {
      reclaimed = true;
      break;
    }
  }
  CHECK(reclaimed);

  { std::lock_guard<std::mutex> lk{mu}; step = 2; }
  cv.notify_all();
  idle.join();
}

TEST_CASE("io_backend=Pread: full pread mode",
          "[bytecask][pread_mode]") {
  TempDir td;
  auto db = bytecask::DB::open(
      td.path,
      {.max_file_bytes = 64, .io_backend = bytecask::IoBackend::Pread});
  constexpr int kCount = 50;
  for (int i = 0; i < kCount; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes(val));
  }
  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    REQUIRE(db.get({}, to_bytes(key), out));
    CHECK(to_string(out) == val);
  }
}

TEST_CASE("io_backend=Pread: recovery loads sealed files via pread",
          "[bytecask][no_mmap][recovery]") {
  TempDir td;
  auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(
        db_path, {.max_file_bytes = 64, .io_backend = bytecask::IoBackend::Pread});
    for (int i = 0; i < 50; ++i) {
      auto key = std::format("k{:04d}", i);
      auto val = std::format("v{:04d}", i);
      db.put({.sync = false}, to_bytes(key), to_bytes(val));
    }
  }
  auto db = bytecask::DB::open(db_path, {.io_backend = bytecask::IoBackend::Pread});
  bytecask::Bytes out;
  for (int i = 0; i < 50; ++i) {
    auto key = std::format("k{:04d}", i);
    auto val = std::format("v{:04d}", i);
    REQUIRE(db.get({}, to_bytes(key), out));
    CHECK(to_string(out) == val);
  }
}

TEST_CASE("io_backend=Pread: vacuum reclaims space",
          "[bytecask][no_mmap][vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 64, .io_backend = bytecask::IoBackend::Pread});
  for (int i = 0; i < 30; ++i) {
    auto key = std::format("k{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes("initial"));
  }
  for (int i = 0; i < 30; ++i) {
    auto key = std::format("k{:04d}", i);
    db.put({.sync = false}, to_bytes(key), to_bytes("updated"));
  }
  {
    int spins = 0;
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
      REQUIRE(++spins < 200);  // vacuum must converge, not spin
    }
  }
  bytecask::Bytes out;
  for (int i = 0; i < 30; ++i) {
    auto key = std::format("k{:04d}", i);
    REQUIRE(db.get({}, to_bytes(key), out));
    CHECK(to_string(out) == "updated");
  }
}

// BC-243: DB::load_state_for_read cached the current generation in a
// function-local thread_local keyed by nothing, so a thread reading from
// two different DB instances could see one DB's generation while querying
// the other. Interleave reads on two DBs from the same thread and confirm
// each DB reports only its own data.
TEST_CASE("BC-243: thread-local read cache does not leak across DB instances",
          "[bytecask][tl-cache]") {
  TempDir td_a;
  TempDir td_b;
  auto a = bytecask::DB::open(td_a.path);
  auto b = bytecask::DB::open(td_b.path);

  a.put({.sync = false}, to_bytes("shared"), to_bytes("from_a"));
  // Ensure b's publish timestamp is strictly newer than a's, which is the
  // condition that made the stale-owner check in load_state_for_read pass
  // incorrectly.
  std::this_thread::sleep_for(std::chrono::milliseconds{2});
  b.put({.sync = false}, to_bytes("shared"), to_bytes("from_b"));
  b.put({.sync = false}, to_bytes("only_in_b"), to_bytes("x"));

  bytecask::Bytes out;

  // Read b first so its generation is cached on this thread ...
  REQUIRE(b.get({}, to_bytes("shared"), out));
  CHECK(to_string(out) == "from_b");

  // ... then read a: must see a's own data, not b's cached generation.
  REQUIRE(a.get({}, to_bytes("shared"), out));
  CHECK(to_string(out) == "from_a");
  CHECK_FALSE(a.contains_key({}, to_bytes("only_in_b")));

  // Flip back to b to confirm the cache correctly re-targets both ways.
  REQUIRE(b.get({}, to_bytes("shared"), out));
  CHECK(to_string(out) == "from_b");
  CHECK(b.contains_key({}, to_bytes("only_in_b")));
}

// ===========================================================================
// Commit pipeline (docs/commit_pipeline_design.md): stage 1 of the next
// batch overlaps the in-flight fdatasync; publication waits for the flush.
// ===========================================================================

namespace {

// Holds the first flush that reaches the fdatasync open until released, so
// other writers can append behind it. Installed via
// DB::test_before_flush_sync_; runs on the flushing thread.
struct FlushGate {
  std::mutex mu;
  std::condition_variable cv;
  bool in_flush{false};
  bool release{false};
  int calls{0};
  // Runs on the flushing thread after the gate opens, before the fdatasync.
  std::function<void()> after_release;

  auto hook() {
    return [this] {
      std::unique_lock<std::mutex> lk{mu};
      if (++calls != 1) return;
      in_flush = true;
      cv.notify_all();
      cv.wait(lk, [&] { return release; });
      if (after_release) after_release();
    };
  }
  void wait_in_flush() {
    std::unique_lock<std::mutex> lk{mu};
    REQUIRE(cv.wait_for(lk, std::chrono::seconds{10}, [&] { return in_flush; }));
  }
  void open() {
    {
      std::lock_guard<std::mutex> lk{mu};
      release = true;
    }
    cv.notify_all();
  }
};

template <typename Pred>
void wait_until(Pred pred) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!pred()) {
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::microseconds{200});
  }
}

auto blocked_count(const bytecask::DB &db) -> std::int64_t {
  return db.stats().at("bytecask.commit_wait_blocked");
}

} // namespace

TEST_CASE("pipeline: sync write is invisible until its fdatasync returns; a "
          "writer appended behind it lands in the next flush",
          "[pipeline][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  FlushGate gate;
  db.test_before_flush_sync_ = gate.hook();

  std::optional<bytecask::CommitResult> ra;
  std::optional<bytecask::CommitResult> rb;
  std::thread ta([&] { ra = db.put({.sync = true}, to_bytes("k1"), to_bytes("v1")); });
  gate.wait_in_flush();

  // k1 is appended and in the page cache, its fdatasync has not returned:
  // not visible, not in a snapshot, not durable.
  CHECK_FALSE(db.contains_key({}, to_bytes("k1")));
  CHECK_FALSE(db.snapshot().contains_key({}, to_bytes("k1")));

  // A second writer runs stage 1 underneath the open flush and then waits.
  std::thread tb([&] { rb = db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")); });
  wait_until([&] { return blocked_count(db) >= 1; });
  CHECK_FALSE(db.contains_key({}, to_bytes("k1")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));

  gate.open();
  ta.join();
  tb.join();
  db.test_before_flush_sync_ = nullptr;

  REQUIRE(ra.has_value());
  REQUIRE(rb.has_value());
  CHECK(ra->durable);
  CHECK(rb->durable);
  CHECK(rb->sequence > ra->sequence);
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));
  CHECK(db.durable_sequence() >= rb->sequence);
  // One fdatasync per flush: k1's, then k2's — never one per write more.
  CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 2);
  const auto s = db.engine_state();
  CHECK(s->durable_seq >= s->sync_requested_seq);
}

TEST_CASE("pipeline: fdatasync failure fails every writer appended since the "
          "last flush and resume() recovers them",
          "[pipeline][f_visibility][degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1'000'000});
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));

  // Armed on the flushing thread, inside the gate, so only that thread's
  // fdatasync fails (the injector is thread-local).
  bytecask::testing::FaultInjector inj;
  inj.fail_at_name = "io_data_file_sync";
  FlushGate gate;
  gate.after_release = [&] { bytecask::testing::active_injector = &inj; };
  db.test_before_flush_sync_ = gate.hook();

  std::exception_ptr ea;
  std::exception_ptr eb;
  std::thread ta([&] {
    try {
      (void)db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
    } catch (...) {
      ea = std::current_exception();
    }
    bytecask::testing::active_injector = nullptr;
  });
  gate.wait_in_flush();
  std::thread tb([&] {
    try {
      (void)db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
    } catch (...) {
      eb = std::current_exception();
    }
  });
  wait_until([&] { return blocked_count(db) >= 1; });

  gate.open();
  ta.join();
  tb.join();
  db.test_before_flush_sync_ = nullptr;

  // Both writers built on unpublished state; both receive the I/O error.
  REQUIRE(ea);
  REQUIRE(eb);
  CHECK_THROWS_AS(std::rethrow_exception(ea), std::system_error);
  CHECK_THROWS_AS(std::rethrow_exception(eb), std::system_error);
  CHECK(db.is_degraded());
  CHECK_FALSE(db.contains_key({}, to_bytes("k1")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));
  CHECK(db.contains_key({}, to_bytes("seed")));
  // Nothing accepted while degraded.
  CHECK_THROWS_AS(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")),
                  bytecask::DbDegraded);

  // resume() scans the active file: both entries are in the page cache.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));
  auto r = db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));
  CHECK(r.durable);
  CHECK(db.contains_key({}, to_bytes("k3")));
}

TEST_CASE("pipeline: nosync write behind an in-flight flush becomes visible "
          "after it, without an fdatasync of its own",
          "[pipeline][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  FlushGate gate;
  db.test_before_flush_sync_ = gate.hook();

  std::optional<bytecask::CommitResult> ra;
  std::optional<bytecask::CommitResult> rb;
  std::thread ta([&] { ra = db.put({.sync = true}, to_bytes("k1"), to_bytes("v1")); });
  gate.wait_in_flush();
  std::thread tb([&] { rb = db.put({.sync = false}, to_bytes("k2"), to_bytes("v2")); });
  wait_until([&] { return blocked_count(db) >= 1; });
  // The nosync write cannot be published ahead of the sync write it was
  // built on.
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));

  gate.open();
  ta.join();
  tb.join();
  db.test_before_flush_sync_ = nullptr;

  REQUIRE(ra.has_value());
  REQUIRE(rb.has_value());
  CHECK(ra->durable);
  CHECK_FALSE(rb->durable);
  CHECK(db.contains_key({}, to_bytes("k2")));
  CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 1);
  CHECK(db.durable_sequence() == ra->sequence);
}

TEST_CASE("pipeline: snapshot conflict is detected against a write that is "
          "appended but not yet published",
          "[pipeline][apply_batch][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("a"), to_bytes("v0"));
  auto snap = db.snapshot();

  FlushGate gate;
  db.test_before_flush_sync_ = gate.hook();
  std::optional<bytecask::CommitResult> ra;
  std::thread ta([&] { ra = db.put({.sync = true}, to_bytes("a"), to_bytes("fromA")); });
  gate.wait_in_flush();

  // Reads still see v0 ...
  auto v = get_val(db, to_bytes("a"));
  REQUIRE(v.has_value());
  CHECK(to_string(*v) == "v0");
  // ... and a plan from the older snapshot conflicts with the pending
  // write. The conflict is validated against the head at once — the plan
  // is never appended — but it is reported only once A's write is
  // published: until then no snapshot can see what the plan lost to, and a
  // retry would lose the same way for the length of the flush.
  std::atomic<bool> b_returned{false};
  std::optional<bytecask::CommitResult> rb;
  std::thread tb([&] {
    bytecask::WritePlan plan{std::move(snap)};
    plan.put(to_bytes("a"), to_bytes("fromB"));
    rb = db.apply_batch({.sync = true}, std::move(plan));
    b_returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  CHECK_FALSE(b_returned.load());
  v = get_val(db, to_bytes("a"));
  CHECK(to_string(*v) == "v0");  // B's wait published nothing

  gate.open();
  ta.join();
  tb.join();
  db.test_before_flush_sync_ = nullptr;
  REQUIRE(ra.has_value());
  CHECK_FALSE(rb.has_value());
  v = get_val(db, to_bytes("a"));
  REQUIRE(v.has_value());
  CHECK(to_string(*v) == "fromA");
}

TEST_CASE("pipeline: rotation waits for the in-flight flush and then runs as "
          "a barrier",
          "[pipeline][rotation][concurrency]") {
  TempDir td;
  const std::string big(8192, 'x');
  {
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 4096});
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  const auto rotations_before = db.stats().at("bytecask.file_rotations");

  FlushGate gate;
  db.test_before_flush_sync_ = gate.hook();
  std::optional<bytecask::CommitResult> ra;
  std::optional<bytecask::CommitResult> rb;
  std::thread ta([&] { ra = db.put({.sync = true}, to_bytes("k1"), to_bytes("v1")); });
  gate.wait_in_flush();

  // Crosses the threshold: its stage 1 must quiesce behind the open flush.
  std::thread tb([&] { rb = db.put({.sync = true}, to_bytes("k2"), to_bytes(big)); });
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  CHECK_FALSE(db.contains_key({}, to_bytes("k1")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));
  CHECK(db.stats().at("bytecask.file_rotations") == rotations_before);

  gate.open();
  ta.join();
  tb.join();
  db.test_before_flush_sync_ = nullptr;

  REQUIRE(ra.has_value());
  REQUIRE(rb.has_value());
  CHECK(ra->durable);
  CHECK(rb->durable);
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));
  CHECK(db.stats().at("bytecask.file_rotations") == rotations_before + 1);
  CHECK(db.durable_sequence() >= rb->sequence);
  }

  // Everything survives recovery.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 4096});
  CHECK(db.contains_key({}, to_bytes("seed")));
  CHECK(db.contains_key({}, to_bytes("k1")));
  auto v = get_val(db, to_bytes("k2"));
  REQUIRE(v.has_value());
  CHECK(to_string(*v) == big);
}

TEST_CASE("pipeline: a lone writer flushes on its own thread and never blocks",
          "[pipeline]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");
  for (int i = 0; i < 50; ++i) {
    auto r = db.put({.sync = true}, to_bytes(std::format("k{}", i)),
                    to_bytes("v"));
    CHECK(r.durable);
    CHECK(db.durable_sequence() == r.sequence);
  }
  CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 50);
  CHECK(blocked_count(db) == 0);
}

TEST_CASE("pipeline: published state never owes an fdatasync",
          "[pipeline][invariants]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  std::mt19937 rng{7};
  for (int i = 0; i < 200; ++i) {
    const bool sync = (rng() % 3) == 0;
    auto r = db.put({.sync = sync}, to_bytes(std::format("k{}", rng() % 50)),
                    to_bytes("v"));
    const auto s = db.engine_state();
    CHECK(s->durable_seq >= s->sync_requested_seq);
    CHECK(s->next_seq > r.sequence);
    if (sync) {
      CHECK(s->sync_requested_seq == r.sequence);
      CHECK(r.durable);
    }
  }
  // The consistency check rejects a state that owes a flush.
  auto bad = *db.engine_state();
  bad.sync_requested_seq = bad.durable_seq + 1;
  CHECK_THROWS_AS(db.test_validate_state_consistency(bad), std::runtime_error);
}

TEST_CASE("pipeline: many concurrent sync writers, every commit durable and "
          "visible, recovery agrees",
          "[pipeline][concurrency]") {
  TempDir td;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 60;
  std::vector<bytecask::CommitResult> results(
      static_cast<std::size_t>(kThreads * kPerThread));
  {
    auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 64 * 1024});
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i) {
          const auto key = std::format("t{}-{}", t, i);
          auto r = db.put({.sync = (i % 4) != 3}, to_bytes(key), to_bytes(key));
          results[static_cast<std::size_t>(t * kPerThread + i)] = r;
          // Read-your-own-writes on return, whatever the sync option.
          auto v = get_val(db, to_bytes(key));
          REQUIRE(v.has_value());
          CHECK(to_string(*v) == key);
          if ((i % 4) != 3) {
            CHECK(r.durable);
            CHECK(db.durable_sequence() >= r.sequence);
          }
        }
      });
    }
    for (auto &th : threads) th.join();
    const auto s = db.engine_state();
    CHECK(s->durable_seq >= s->sync_requested_seq);
    CHECK(db.stats().at("bytecask.fsyncs") < kThreads * kPerThread);
  }
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 64 * 1024});
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      const auto key = std::format("t{}-{}", t, i);
      auto v = get_val(db, to_bytes(key));
      REQUIRE(v.has_value());
      CHECK(to_string(*v) == key);
    }
  }
}

TEST_CASE("pipeline: a writer whose write another thread published sees it "
          "on its next read, even before state_time_ is stored",
          "[pipeline][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));

  // T runs stage 1 and parks before commit_wait, so its entry is in the
  // head. F appends behind it, wins the flush role and publishes a state
  // covering both, then parks between the state store and the state_time_
  // store. T is released into that gap: commit_wait finds its sequence
  // covered and returns while state_time_ still carries the timestamp T's
  // read cache was warmed with.
  FlushGate before_wait;
  FlushGate between_stores;
  db.test_before_commit_wait_ = before_wait.hook();
  db.test_between_publish_stores_ = between_stores.hook();

  std::optional<bytecask::CommitResult> rt;
  bool t_saw_own_write = false;
  std::thread tt([&] {
    // Warm this thread's read cache with the pre-put state.
    (void)db.contains_key({}, to_bytes("seed"));
    rt = db.put({.sync = true}, to_bytes("t"), to_bytes("vt"));
    t_saw_own_write = get_val(db, to_bytes("t")).has_value();
  });
  before_wait.wait_in_flush();

  std::optional<bytecask::CommitResult> rf;
  std::thread tf([&] { rf = db.put({.sync = true}, to_bytes("f"), to_bytes("vf")); });
  between_stores.wait_in_flush();

  before_wait.open();
  tt.join();
  between_stores.open();
  tf.join();
  db.test_before_commit_wait_ = nullptr;
  db.test_between_publish_stores_ = nullptr;

  REQUIRE(rt.has_value());
  REQUIRE(rf.has_value());
  CHECK(rt->durable);
  CHECK(rf->sequence > rt->sequence);
  CHECK(t_saw_own_write);
  CHECK(db.contains_key({}, to_bytes("t")));
}

TEST_CASE("pipeline: a plan that loses to a write not yet published reports "
          "the conflict once a retry can see that write",
          "[pipeline][concurrency]") {
  // Plans are validated against the head, which holds writes still in
  // their fdatasync; a snapshot sees only the published state. A plan that
  // collides with such a write cannot succeed on any retry until the write
  // is published, so the conflict must not come back before then — a
  // retry loop otherwise spins for the length of the flush (the CAS
  // benchmark's average attempts went from 1 to 7-350 with the pipeline).
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("k"), to_bytes("0"));

  // A: stage 1 done, entry for k in the head, parked before its flush.
  FlushGate before_wait;
  db.test_before_commit_wait_ = before_wait.hook();
  std::thread ta([&] { db.put({.sync = true}, to_bytes("k"), to_bytes("1")); });
  before_wait.wait_in_flush();

  // B: snapshot (sees "0"), guarded write on k — loses to A's in-flight
  // write, and must block until A publishes.
  std::atomic<bool> b_returned{false};
  std::optional<bytecask::CommitResult> rb;
  std::thread tb([&] {
    auto snap = db.snapshot();
    bytecask::Bytes out;
    REQUIRE(snap.get({}, to_bytes("k"), out));
    REQUIRE(to_string(out) == "0");
    bytecask::WritePlan plan{std::move(snap)};
    plan.ensure_unchanged(to_bytes("k"));
    plan.put(to_bytes("k"), to_bytes("b"));
    rb = db.apply_batch({.sync = true}, std::move(plan));
    b_returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  CHECK_FALSE(b_returned.load());

  before_wait.open();
  ta.join();
  tb.join();
  db.test_before_commit_wait_ = nullptr;
  CHECK_FALSE(rb.has_value());

  // The retry sees A's write and succeeds first time.
  auto snap = db.snapshot();
  bytecask::Bytes out;
  REQUIRE(snap.get({}, to_bytes("k"), out));
  CHECK(to_string(out) == "1");
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("2"));
  CHECK(db.apply_batch({.sync = true}, std::move(plan)).has_value());
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "2");
}

TEST_CASE("pipeline: a plan whose snapshot is already behind the published "
          "state reports its conflict at once",
          "[pipeline][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("k"), to_bytes("0"));
  auto snap = db.snapshot();
  db.put({.sync = true}, to_bytes("k"), to_bytes("1"));  // published
  bytecask::WritePlan plan{std::move(snap)};
  plan.ensure_unchanged(to_bytes("k"));
  plan.put(to_bytes("k"), to_bytes("b"));
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(db.apply_batch({.sync = true}, std::move(plan)).has_value());
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds{100});
}

TEST_CASE("pipeline: a batch admitted before a flush failure is rejected as "
          "degraded, not appended",
          "[pipeline][degraded][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 1'000'000});
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));

  // A passes apply_batch's admission check and becomes the group leader,
  // then parks in the leader hook before its executor runs.
  std::mutex mu;
  std::condition_variable cv;
  bool leader_parked = false;
  bool go = false;
  db.test_write_group().on_leader_start_ = [&] {
    std::unique_lock<std::mutex> lk{mu};
    leader_parked = true;
    cv.notify_all();
    cv.wait(lk, [&] { return go; });
  };
  std::exception_ptr ea;
  std::thread ta([&] {
    try {
      (void)db.put({.sync = true}, to_bytes("ka"), to_bytes("va"));
    } catch (...) {
      ea = std::current_exception();
    }
  });
  {
    std::unique_lock<std::mutex> lk{mu};
    REQUIRE(cv.wait_for(lk, std::chrono::seconds{10}, [&] { return leader_parked; }));
  }

  // B bypasses the group (solo), appends, and its flush fails: degraded.
  std::exception_ptr eb;
  std::thread tb([&] {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    try {
      (void)db.put({.sync = true, .solo = true}, to_bytes("kb"), to_bytes("vb"));
    } catch (...) {
      eb = std::current_exception();
    }
  });
  tb.join();
  REQUIRE(eb);
  CHECK_THROWS_AS(std::rethrow_exception(eb), std::system_error);
  REQUIRE(db.is_degraded());

  // Release A: its executor must reject on the published state.
  {
    std::lock_guard<std::mutex> lk{mu};
    go = true;
  }
  cv.notify_all();
  ta.join();
  db.test_write_group().on_leader_start_ = nullptr;
  REQUIRE(ea);
  CHECK_THROWS_AS(std::rethrow_exception(ea), bytecask::DbDegraded);

  // kb reached the page cache and is replayed; ka was never appended.
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  CHECK(db.contains_key({}, to_bytes("kb")));
  CHECK_FALSE(db.contains_key({}, to_bytes("ka")));
  CHECK(db.put({.sync = true}, to_bytes("kc"), to_bytes("vc")).durable);
}

TEST_CASE("pipeline: publishing a state that owes an fdatasync degrades the "
          "engine",
          "[pipeline][invariants][degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));

  auto bad = std::make_shared<bytecask::EngineState>(*db.engine_state());
  bad->sync_requested_seq = bad->durable_seq + 1;
  db.test_publish(bad);

  CHECK(db.is_degraded());
  CHECK(db.degraded_reason().find("sync_requested_seq") != std::string::npos);
  CHECK_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                  bytecask::DbDegraded);
  // The rejected state was never published: k1 is still readable, and
  // resume() brings the engine back with it.
  CHECK(db.contains_key({}, to_bytes("k1")));
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")).durable);
  const auto s = db.engine_state();
  CHECK(s->durable_seq >= s->sync_requested_seq);
}
