// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — integration and model-based correctness tests

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <concepts>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include "mapping_probe.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <array>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <latch>
#include <tuple>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import bytecask;
import bytecask.batch_iterator;
import bytecask.buffer_pool;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_entry;
import bytecask.hint_file;
import bytecask.serialization;
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

// Per-file stats as a sorted multiset. File ids differ between two opens of
// the same directory, so model tests compare the values, not the ids.
using FileStatsTuple =
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
               std::uint64_t, std::uint64_t>;
auto collect_file_stats(bytecask::DB &db) -> std::vector<FileStatsTuple> {
  std::vector<FileStatsTuple> vals;
  for (const auto &[fid, fs] : db.file_stats())
    vals.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                      fs.max_sequence, fs.tombstone_bytes, fs.marker_bytes);
  std::ranges::sort(vals);
  return vals;
}

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
    auto fs = *t.get(bad.active_file_id);
    fs.total_bytes = 1;
    t.set(bad.active_file_id, fs);
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
    auto df = bytecask::WritablePosixDataFile::create(db_path / "data_00000000000000_00000000_V01.data", 0);
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
      auto df = bytecask::WritablePosixDataFile::create(db_path / std::format("{}.data", put_stem), 0);
      std::ignore = df->append_entry(1, bytecask::EntryType::Put, to_bytes("gone"),
                              to_bytes("v1"));
      std::ignore = df->append_entry(2, bytecask::EntryType::Put, to_bytes("alive"),
                              to_bytes("v2"));
      df->sync();
    }

    // File with a Delete for "gone" (seq=3) — higher sequence wins.
    {
      auto df = bytecask::WritablePosixDataFile::create(db_path / std::format("{}.data", del_stem), 0);
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
// Recovering a hint-less data file (#138). Open scans every data file without
// a hint and stops at the first record that does not parse. Only the file
// written last can hold a record a crash tore, because every other file was
// fdatasync'd whole before it was sealed. So past the stop:
// - zeros only: the preallocated tail, trimmed in any file;
// - anything else, in the file with the highest sequences: a torn tail,
//   truncated, as PostgreSQL and RocksDB truncate a log at its first bad
//   record;
// - anything else, in any other file: damage in synced data. Refused in both
//   modes, with the file left byte for byte as it was.
// Each row builds one on-disk shape from a cleanly closed three-file
// database, whose last file is hint-less and exact-size after the close.
// ---------------------------------------------------------------------------
namespace {

constexpr std::uint64_t kEntry = 23;  // 15 header + 2 key + 2 value + 4 CRC

// Data files ordered by the sequence of their first record.
auto data_files_by_sequence(const std::filesystem::path &dir)
    -> std::vector<std::filesystem::path> {
  std::vector<std::pair<std::uint64_t, std::filesystem::path>> files;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() != ".data") continue;
    std::ifstream f{e.path(), std::ios::binary};
    std::array<unsigned char, 8> b{};
    f.read(reinterpret_cast<char *>(b.data()), 8);
    std::uint64_t seq = 0;
    for (int i = 7; i >= 0; --i) seq = (seq << 8) | b[static_cast<std::size_t>(i)];
    files.emplace_back(seq, e.path());
  }
  std::ranges::sort(files);
  std::vector<std::filesystem::path> out;
  for (auto &[seq, p] : files) out.push_back(std::move(p));
  return out;
}

auto read_all(const std::filesystem::path &p) -> std::string {
  std::ifstream f{p, std::ios::binary};
  return {std::istreambuf_iterator<char>{f}, {}};
}

void write_at(const std::filesystem::path &p, std::uint64_t off,
              std::string_view bytes) {
  std::fstream f{p, std::ios::in | std::ios::out | std::ios::binary};
  f.seekp(static_cast<std::streamoff>(off));
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void zero_at(const std::filesystem::path &p, std::uint64_t off,
             std::uint64_t n) {
  write_at(p, off, std::string(n, '\0'));
}

// What a power cut leaves of a file's preallocated tail.
void add_zero_tail(const std::filesystem::path &p) {
  std::filesystem::resize_file(p, std::filesystem::file_size(p) + 4096);
}

void drop_hint(const std::filesystem::path &data) {
  auto hint = data;
  hint.replace_extension(".hint");
  REQUIRE(std::filesystem::remove(hint));
}

// The last entry of p torn: its header reached the disk, the rest did not.
void tear_last_entry(const std::filesystem::path &p) {
  const auto size = std::filesystem::file_size(p);
  add_zero_tail(p);
  zero_at(p, size - kEntry + 15, kEntry - 15);
}

struct TailRow {
  const char *label;
  std::function<void(const std::vector<std::filesystem::path> &)> damage;
  bool opens;
  // Opens: the keys, by write order, that the open must have dropped.
  std::set<int> lost;
  // Refuses: the file, by sequence order, that must come out unchanged.
  std::size_t refused_file;
};

} // namespace

TEST_CASE("DB recovery: a hint-less file's tail is truncated only in the "
          "newest file",
          "[bytecask][recovery][corruption]") {
  // Files by sequence: f0 holds keys 0-4, f1 keys 5-9, f2 (newest) 10-13.
  // Entry i of a file begins at kEntry * i.
  const std::vector<TailRow> rows{
      {"torn last write", [](auto &f) { tear_last_entry(f[2]); }, true, {13},
       0},
      {"file ends mid-entry",
       [](auto &f) {
         std::filesystem::resize_file(f[2],
                                      std::filesystem::file_size(f[2]) - 5);
       },
       true, {13}, 0},
      {"zero tail only", [](auto &f) { add_zero_tail(f[2]); }, true, {}, 0},
      {"hole, then later entries",
       [](auto &f) {
         add_zero_tail(f[2]);
         zero_at(f[2], kEntry, kEntry);
       },
       true, {11, 12, 13}, 0},
      {"first write torn, header landed",
       [](auto &f) {
         std::filesystem::resize_file(f[2], kEntry);
         tear_last_entry(f[2]);
       },
       true, {10, 11, 12, 13}, 0},
      {"first write torn, header lost",
       [](auto &f) {
         std::filesystem::resize_file(f[2], kEntry);
         add_zero_tail(f[2]);
         zero_at(f[2], 0, 15);
       },
       true, {10, 11, 12, 13}, 0},
      // What a power cut leaves of a file that was never synced when its
      // first page is lost and a later one reaches the disk.
      {"first header lost, later entries landed",
       [](auto &f) { zero_at(f[2], 0, 15); }, true, {10, 11, 12, 13}, 0},
      {"sealed zero tail, newest torn",
       [](auto &f) {
         drop_hint(f[0]);
         add_zero_tail(f[0]);
         tear_last_entry(f[2]);
       },
       true, {13}, 0},
      {"hint backlog with zero tails",
       [](auto &f) {
         drop_hint(f[0]);
         drop_hint(f[1]);
         add_zero_tail(f[0]);
         add_zero_tail(f[1]);
       },
       true, {}, 0},
      {"sealed CRC damage",
       [](auto &f) {
         drop_hint(f[0]);
         flip_byte(f[0], kEntry + 15);
       },
       false, {}, 0},
      {"sealed zeroed entry",
       [](auto &f) {
         drop_hint(f[0]);
         zero_at(f[0], kEntry, kEntry);
       },
       false, {}, 0},
      {"sealed oversized value_size",
       [](auto &f) {
         drop_hint(f[0]);
         write_at(f[0], kEntry + 11, "\xff\xff\xff\x7f");
       },
       false, {}, 0},
      // A crash tears one file. Two that need a cut are damage, and neither
      // is cut: once for each file, since the row checks one.
      {"zeroed first header, newest torn too: the older file",
       [](auto &f) {
         drop_hint(f[0]);
         zero_at(f[0], 0, 8);
         tear_last_entry(f[2]);
       },
       false, {}, 0},
      {"zeroed first header, newest torn too: the newest file",
       [](auto &f) {
         drop_hint(f[0]);
         zero_at(f[0], 0, 8);
         tear_last_entry(f[2]);
       },
       false, {}, 2},
      {"two files with a zeroed first header",
       [](auto &f) {
         drop_hint(f[1]);
         zero_at(f[1], 0, 8);
         zero_at(f[2], 0, 8);
       },
       false, {}, 1},
      {"sealed damage, newest torn too",
       [](auto &f) {
         drop_hint(f[1]);
         flip_byte(f[1], kEntry + 15);
         tear_last_entry(f[2]);
       },
       false, {}, 1},
  };

  for (const auto &row : rows) {
    for (const bool strict : {true, false}) {
      DYNAMIC_SECTION(row.label << (strict ? " (strict)" : " (lenient)")) {
        TempDir td;
        const auto dir = td.path / "db";
        const bytecask::Options opts{.max_file_bytes = 100,
                                     .fail_recovery_on_crc_errors = strict};
        {
          auto db = bytecask::DB::open(dir, opts);
          for (int i = 0; i < 14; ++i)
            db.put({.sync = true}, to_bytes(std::format("k{:x}", i)),
                   to_bytes(std::format("v{:x}", i)));
        }
        const auto files = data_files_by_sequence(dir);
        REQUIRE(files.size() == 3);
        REQUIRE(std::filesystem::file_size(files[0]) == 5 * kEntry);
        REQUIRE(std::filesystem::file_size(files[2]) == 4 * kEntry);
        // The damage is a crash's: the file active when it hit has no hint.
        // A clean close writes one.
        drop_hint(files[2]);

        row.damage(files);

        if (row.opens) {
          auto db = bytecask::DB::open(dir, opts);
          for (int i = 0; i < 14; ++i) {
            INFO("key " << i);
            CHECK(db.contains_key({}, to_bytes(std::format("k{:x}", i))) ==
                  !row.lost.contains(i));
          }
          // Everything past the last committed record is gone.
          CHECK(std::filesystem::file_size(files[0]) == 5 * kEntry);
          CHECK(std::filesystem::file_size(files[2]) ==
                (4 - row.lost.size()) * kEntry);
        } else {
          const auto before = read_all(files[row.refused_file]);
          REQUIRE_THROWS_AS(bytecask::DB::open(dir, opts), std::runtime_error);
          CHECK(read_all(files[row.refused_file]) == before);
          auto hint = files[row.refused_file];
          hint.replace_extension(".hint");
          CHECK_FALSE(std::filesystem::exists(hint));
        }
      }
    }
  }
}

// A hint is written to .hint.tmp and renamed into place, so ".hint exists"
// means "the hint is complete". Open writes a hint-less file's hint before it
// trims the file's torn tail, and a hint that later fails its CRC is rebuilt
// by a scan that does no tail handling, since a file with a hint was sealed.
// A hint torn in place by a crash during that open would send the next one
// to that scan over the torn tail, and it would refuse the database.
TEST_CASE("DB recovery: a crash while open writes a hint leaves no hint behind "
          "it, and the next open recovers",
          "[bytecask][recovery][corruption]") {
  TempDir td;
  const auto dir = td.path / "db";
  const bytecask::Options opts{.max_file_bytes = 100};
  {
    auto db = bytecask::DB::open(dir, opts);
    for (int i = 0; i < 14; ++i)
      db.put({.sync = true}, to_bytes(std::format("k{:x}", i)),
             to_bytes(std::format("v{:x}", i)));
  }
  const auto files = data_files_by_sequence(dir);
  REQUIRE(files.size() == 3);
  drop_hint(files[2]);
  tear_last_entry(files[2]);

  {
    // The crash: the hint's frames are written, its trailer is not.
    bytecask::testing::ScopedFaultInjector fi{"io_hint_write"};
    REQUIRE_THROWS(bytecask::DB::open(dir, opts));
  }
  auto hint = files[2];
  hint.replace_extension(".hint");
  CHECK_FALSE(std::filesystem::exists(hint));

  auto db = bytecask::DB::open(dir, opts);
  for (int i = 0; i < 13; ++i) {
    INFO("key " << i);
    CHECK(db.contains_key({}, to_bytes(std::format("k{:x}", i))));
  }
}

// Open cuts a torn tail with truncate_durably, whose fdatasync persists the
// new length. The first io_rewrite_sync is the hint-less file's rewrite, the
// second the cut's; failing the second fails the open only while it is made.
TEST_CASE("DB recovery: open fails when the cut of a torn tail cannot be "
          "synced, and the next open recovers",
          "[bytecask][recovery][corruption]") {
  TempDir td;
  const auto dir = td.path / "db";
  const bytecask::Options opts{.max_file_bytes = 100};
  {
    auto db = bytecask::DB::open(dir, opts);
    for (int i = 0; i < 14; ++i)
      db.put({.sync = true}, to_bytes(std::format("k{:x}", i)),
             to_bytes(std::format("v{:x}", i)));
  }
  const auto files = data_files_by_sequence(dir);
  REQUIRE(files.size() == 3);
  drop_hint(files[2]);
  tear_last_entry(files[2]);

  {
    bytecask::testing::ScopedFaultInjector fi{"io_rewrite_sync"};
    fi.inj.fail_on_nth_match = 2;
    REQUIRE_THROWS_AS(bytecask::DB::open(dir, opts), std::system_error);
  }

  auto db = bytecask::DB::open(dir, opts);
  for (int i = 0; i < 13; ++i) {
    INFO("key " << i);
    CHECK(db.contains_key({}, to_bytes(std::format("k{:x}", i))));
  }
}

TEST_CASE("DB read: a record past the end of its file names the file, the "
          "offset and the header",
          "[bytecask][corruption]") {
  // Only the blind key directory reads whole records at open, to place keys.
  if constexpr (!bytecask::kKeyDirReadsKeys)
    SKIP("the key directory stores its keys and reads no record at open");
  TempDir td;
  const auto dir = td.path / "db";
  const bytecask::Options opts{.max_file_bytes = 100};
  {
    auto db = bytecask::DB::open(dir, opts);
    for (int i = 0; i < 14; ++i)
      db.put({.sync = true}, to_bytes(std::format("k{:x}", i)),
             to_bytes(std::format("v{:x}", i)));
  }
  const auto files = data_files_by_sequence(dir);
  // k1's value_size, in the oldest file, which has a hint and is read, not
  // scanned.
  write_at(files[0], kEntry + 11, "\xff\xff\xff\x7f");
  try {
    auto db = bytecask::DB::open(dir, opts);
    FAIL("open read a record running past the end of its file");
  } catch (const std::runtime_error &e) {
    const std::string what = e.what();
    INFO(what);
    CHECK(what.find("record extends past the end") != std::string::npos);
    CHECK(what.find(files[0].filename().string()) != std::string::npos);
    CHECK(what.find(std::format("offset {}", kEntry)) != std::string::npos);
    CHECK(what.find("value_size 2147483647") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Damage in a sealed file is detected and refused, never trimmed around. The
// scan vacuum copies a sealed file with stops by throwing: if the first entry
// that fails to parse read as the end of the file, vacuum would publish a
// copy without the entries after it and unlink the original, turning one bad
// entry into the permanent loss of every entry behind it.
// ---------------------------------------------------------------------------
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
    // Short keys with prefix overlap to stress shared key prefixes.
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


  // Count data files for the max-parallelism test.
  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }
  REQUIRE(data_file_count > 1);

  // Collect serial file_stats as baseline for parallel comparison.
  // Must use a separate copy since opening mutates the directory.
  std::vector<FileStatsTuple> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_file_stats(db);
  }

  SECTION("serial recovery") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (2 workers)") {
    const auto p = td.path / "p2";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 2});
    verify("parallel/2", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (W = file count)") {
    const auto p = td.path / "pmax";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(
        p, {.max_file_bytes = 1,
            .recovery_threads = static_cast<unsigned>(data_file_count)});
    verify("parallel/max", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

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
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }
}

// ---------------------------------------------------------------------------
// Model-based recovery: keys at the 65,535-byte ceiling, values above 1 MiB.
//
// Keys share all but one byte, at the start, the middle or the end, so the
// key directory tells them apart at every depth. Values above 1 MiB are
// larger than DataFileIterator's read chunk and the test build's
// max_file_bytes, so each large write gets a file of its own. Range deletes
// take large keys as bounds.
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: ceiling-size keys and large values",
          "[bytecask][recovery][parallel][model][limits]") {
  std::mt19937 gen(28801);
  const auto pick = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(gen);
  };

  std::vector<std::string> keys;
  for (int i = 0; i < 12; ++i) {
    std::string k(bytecask::kMaxKeySize, 'k');
    static constexpr std::array<std::size_t, 4> kAt{
        0, bytecask::kMaxKeySize / 2, bytecask::kMaxKeySize - 2,
        bytecask::kMaxKeySize - 1};
    k[kAt[static_cast<std::size_t>(i % 4)]] = static_cast<char>('A' + i);
    keys.push_back(std::move(k));
  }
  for (int i = 0; i < 4; ++i) keys.push_back(std::format("small{}", i));

  const auto rand_key = [&] {
    return keys[static_cast<std::size_t>(pick(0, static_cast<int>(keys.size()) - 1))];
  };
  const auto rand_value = [&] {
    const auto len = pick(0, 9) < 3 ? pick((1 << 20) + 1, 3 << 19)
                                    : pick(0, 64);
    std::string v(static_cast<std::size_t>(len), 'x');
    const auto seed = static_cast<char>(pick('A', 'z'));
    for (std::size_t i = 0; i < v.size(); ++i)
      v[i] = static_cast<char>(seed + static_cast<char>(i % 7));
    return v;
  };

  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;
  {
    auto db = bytecask::DB::open(
        db_path, {.max_key_bytes = bytecask::kMaxKeySize,
                  .max_value_bytes = 2U << 20});
    for (int i = 0; i < 80; ++i) {
      const auto op = pick(0, 19);
      if (op < 10) {
        auto k = rand_key();
        auto v = rand_value();
        db.put({.sync = false}, to_bytes(k), to_bytes(v));
        oracle[k] = std::move(v);
      } else if (op < 14) {
        const auto k = rand_key();
        std::ignore = db.del({.sync = false}, to_bytes(k));
        oracle.erase(k);
      } else if (op < 17) {
        bytecask::WritePlan plan{
            bytecask::SizeLimits{bytecask::kMaxKeySize, 2U << 20}};
        for (int b = pick(2, 3); b > 0; --b) {
          auto k = rand_key();
          if (pick(0, 3) == 0) {
            plan.del(to_bytes(k));
            oracle.erase(k);
          } else {
            auto v = rand_value();
            plan.put(to_bytes(k), to_bytes(v));
            oracle[k] = std::move(v);
          }
        }
        (void)db.apply_batch({.sync = false}, std::move(plan));
      } else {
        auto from = rand_key();
        auto to = rand_key();
        // Distinct bounds: an empty range is refused. Which draws come out
        // equal depends on the standard library's distribution.
        while (to == from) to = rand_key();
        if (to < from) std::swap(from, to);
        // Both bounds come from the same pool, so they can be equal: an
        // empty range is refused before anything is written.
        if (from == to) {
          CHECK_THROWS_AS(
              db.del_range({.sync = false}, to_bytes(from), to_bytes(to)),
              std::invalid_argument);
          continue;
        }
        db.del_range({.sync = false}, to_bytes(from), to_bytes(to));
        std::erase_if(oracle, [&](const auto &kv) {
          return kv.first >= from && kv.first < to;
        });
      }
    }
  }

  const auto open = [&](const std::filesystem::path &p, unsigned threads) {
    return bytecask::DB::open(p, {.recovery_threads = threads,
                                  .max_key_bytes = bytecask::kMaxKeySize,
                                  .max_value_bytes = 2U << 20});
  };
  const auto verify = [&](const std::string &label, bytecask::DB &db) {
    INFO(label);
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({}))
      kv[to_string(entry.key)] = to_string(entry.value);
    CHECK(kv.size() == oracle.size());
    CHECK(kv == oracle);
  };

  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path})
    if (e.path().extension() == ".data") ++data_file_count;
  REQUIRE(data_file_count > 1);

  std::vector<FileStatsTuple> serial_stats_vals;
  {
    const auto p = td.path / "serial_baseline";
    std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
    auto db = open(p, 1);
    verify("serial_baseline", db);
    serial_stats_vals = collect_file_stats(db);
  }

  SECTION("serial recovery") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
    auto db = open(p, 1);
    verify("serial", db);
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (2 workers)") {
    const auto p = td.path / "p2";
    std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
    auto db = open(p, 2);
    verify("parallel/2", db);
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (W = file count)") {
    const auto p = td.path / "pmax";
    std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
    auto db = open(p, static_cast<unsigned>(data_file_count));
    verify("parallel/max", db);
    CHECK(collect_file_stats(db) == serial_stats_vals);
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


  std::vector<FileStatsTuple> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_file_stats(db);
  }

  SECTION("serial") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel (4 workers)") {
    const auto p = td.path / "p4";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 4});
    verify("parallel/4", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

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
    CHECK(collect_file_stats(db) == serial_stats_vals);
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


  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }

  std::vector<FileStatsTuple> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_file_stats(db);
  }

  SECTION("serial") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel (3 workers)") {
    const auto p = td.path / "p3";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 3});
    verify("parallel/3", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel (W = file count)") {
    const auto p = td.path / "pmax";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(
        p, {.max_file_bytes = 1,
            .recovery_threads = static_cast<unsigned>(data_file_count)});
    verify("parallel/max", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

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
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }
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


  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path})
    if (e.path().extension() == ".data") ++data_file_count;
  REQUIRE(data_file_count > 8);

  std::vector<FileStatsTuple> serial_stats_vals;
  {
    const auto p = td.path / "serial_baseline";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_file_stats(db);
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
      CHECK(collect_file_stats(db) == serial_stats_vals);
    }
  }
}

// Rewrites a hint file in the layout written before hints were compressed —
// the entries back to back, then a plain CRC-32C — as a database created by an
// older version holds them.
static void rewrite_hint_uncompressed(const std::filesystem::path &path) {
  std::vector<std::byte> out;
  {
    auto hint = bytecask::HintFile::OpenForRead(path);
    auto scanner = hint.make_scanner();
    while (auto he = scanner.next()) {
      const auto bytes =
          he->entry_type == bytecask::EntryType::RangeDel
              ? bytecask::serialize_range_del_entry(
                    he->sequence, he->file_offset, he->key, he->end_key)
              : bytecask::serialize_entry(he->sequence, he->entry_type,
                                          he->file_offset, he->value_size,
                                          he->key);
      out.insert(out.end(), bytes.begin(), bytes.end());
    }
  }
  bytecask::Crc32 crc{};
  crc.update(out);
  std::array<std::byte, 4> trailer{};
  bytecask::ByteWriter w{trailer};
  w.put(crc.finalize());
  out.insert(out.end(), trailer.begin(), trailer.end());
  std::ofstream f{path, std::ios::binary | std::ios::trunc};
  f.write(reinterpret_cast<const char *>(out.data()), std::ssize(out));
  REQUIRE(f.good());
}

// ---------------------------------------------------------------------------
// Model-based recovery: hint files cut into many frames.
//
// Hint files are zstd frames, and a scanner's entries point into the frame it
// has decoded until its next call. Tiny frames put every scan, seek and merge
// across frame boundaries, and hot keys overwritten many times in one file
// spread one key's duplicates over several frames: the case where a reader
// that kept a key from an earlier frame would read a buffer since reused.
// A directory where some hints still have the uncompressed layout recovers
// the same keys: those hints are refused and rebuilt from their data files.
// ---------------------------------------------------------------------------
TEST_CASE("Recovery model-based: hints split into many frames",
          "[bytecask][recovery][parallel][model]") {
  struct FrameBytes {
    explicit FrameBytes(std::size_t n) {
      bytecask::set_hint_frame_bytes_for_testing(n);
    }
    ~FrameBytes() { bytecask::set_hint_frame_bytes_for_testing(0); }
    FrameBytes(const FrameBytes &) = delete;
    FrameBytes &operator=(const FrameBytes &) = delete;
  } frame_bytes{64};

  std::mt19937 gen(97531);
  auto pick = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(gen);
  };
  // A key's length depends on its number, so keys differ in length and
  // share prefixes; the eight hot keys are rewritten throughout.
  auto key_of = [](int n) {
    return std::format("k{:04d}{}", n, std::string(static_cast<std::size_t>(n % 17), 'x'));
  };
  auto rand_key = [&]() -> std::string {
    if (pick(0, 4) == 0) return std::format("hot:{}", pick(0, 7));
    return key_of(pick(0, 999));
  };
  auto rand_value = [&] {
    return std::string(static_cast<std::size_t>(pick(1, 64)),
                       static_cast<char>(pick('A', 'z')));
  };

  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;
  {
    // 8 KiB files hold a hundred or so entries: dozens of 64-byte frames.
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 8 * 1024});
    const bytecask::WriteOptions wo{.sync = false};
    for (int i = 0; i < 6000; ++i) {
      const auto op = pick(0, 99);
      if (op < 60) {
        auto key = rand_key();
        auto val = rand_value();
        db.put(wo, to_bytes(key), to_bytes(val));
        oracle[key] = val;
      } else if (op < 80) {
        auto key = rand_key();
        std::ignore = db.del(wo, to_bytes(key));
        oracle.erase(key);
      } else if (op < 97) {
        bytecask::WritePlan plan;
        for (int b = pick(2, 6); b > 0; --b) {
          auto key = rand_key();
          if (pick(0, 3) == 0) {
            plan.del(to_bytes(key));
            oracle.erase(key);
          } else {
            auto val = rand_value();
            plan.put(to_bytes(key), to_bytes(val));
            oracle[key] = val;
          }
        }
        (void)db.apply_batch(wo, std::move(plan));
      } else {
        const auto a = pick(0, 990);
        const auto from = std::format("k{:04d}", a);
        const auto to = std::format("k{:04d}", a + pick(1, 30));
        db.del_range(wo, to_bytes(from), to_bytes(to));
        for (auto it = oracle.lower_bound(from); it != oracle.end();)
          it = it->first < to ? oracle.erase(it) : oracle.end();
      }
    }
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
  auto hints_in = [](const std::filesystem::path &dir) {
    std::vector<std::filesystem::path> hints;
    for (const auto &e : std::filesystem::directory_iterator{dir})
      if (e.path().extension() == ".hint") hints.push_back(e.path());
    std::ranges::sort(hints);
    return hints;
  };

  REQUIRE(hints_in(db_path).size() > 8);

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

  for (const unsigned threads : {1U, 2U, 5U, 8U}) {
    DYNAMIC_SECTION("recovery_threads = " << threads) {
      const auto p = td.path / std::format("t{}", threads);
      std::filesystem::copy(db_path, p,
                            std::filesystem::copy_options::recursive);
      auto db = bytecask::DB::open(p, {.recovery_threads = threads});
      verify(std::format("threads/{}", threads), collect(db));
      CHECK(collect_stats(db) == serial_stats_vals);
    }
  }

  for (const unsigned threads : {1U, 4U}) {
    DYNAMIC_SECTION("uncompressed and compressed hints mixed, recovery_threads = "
                    << threads) {
      const auto p = td.path / std::format("mixed{}", threads);
      std::filesystem::copy(db_path, p,
                            std::filesystem::copy_options::recursive);
      const auto hints = hints_in(p);
      std::vector<std::filesystem::path> rewritten;
      for (std::size_t i = 0; i < hints.size(); i += 2) {
        rewrite_hint_uncompressed(hints[i]);
        rewritten.push_back(hints[i]);
      }
      REQUIRE_FALSE(rewritten.empty());
      CHECK_THROWS_AS(bytecask::HintFile::OpenForRead(rewritten.front()),
                      std::runtime_error);
      auto db = bytecask::DB::open(p, {.recovery_threads = threads});
      verify(std::format("mixed/{}", threads), collect(db));
      CHECK(collect_stats(db) == serial_stats_vals);
      // Rebuilt in the current layout.
      for (const auto &h : rewritten)
        CHECK_NOTHROW(bytecask::HintFile::OpenForRead(h));
    }
  }

  // A read that fails on a hint file must not cost keys, wherever it lands.
  // In the pass that opens and verifies a hint, before any of its entries is
  // applied, recovery rebuilds the hint from its data file. In the scan of a
  // hint that verified, some entries are already applied, so the open fails
  // with std::system_error, and the next open recovers. Either way what is
  // recovered is the serial baseline. The fault injector is thread-local, so
  // recovery runs on the thread that opens.
  for (const int nth : {1, 2, 3, 5, 40, 150}) {
    DYNAMIC_SECTION("hint read " << nth << " fails, recovery_threads = 1") {
      const auto p = td.path / std::format("eio{}", nth);
      std::filesystem::copy(db_path, p,
                            std::filesystem::copy_options::recursive);
      bool opened = false;
      {
        bytecask::testing::ScopedFaultInjector fi{"io_hint_read"};
        fi.inj.fail_on_nth_match = nth;
        try {
          auto db = bytecask::DB::open(p, {.recovery_threads = 1});
          opened = true;
          verify(std::format("eio/{}", nth), collect(db));
          CHECK(collect_stats(db) == serial_stats_vals);
        } catch (const std::system_error &) {
        }
        REQUIRE(fi.inj.name_matches >= nth);
      }
      // The first read of the first hint is in its open pass.
      if (nth == 1) CHECK(opened);
      auto db = bytecask::DB::open(p, {.recovery_threads = 1});
      verify(std::format("eio/{}/reopen", nth), collect(db));
      // An open that succeeded added an active file; one that failed did not.
      if (!opened) CHECK(collect_stats(db) == serial_stats_vals);
    }
  }
}

// ---------------------------------------------------------------------------
// A hint file a read fails on is rebuilt from its data file (#237).
//
// Recovery used to map hint files, and a page the kernel could not fill
// (EIO, a file truncated underneath) was a SIGBUS that killed the process.
// Hints are read with pread now: a failed read is a std::system_error, and one
// in the pass that opens a hint is treated like a CRC failure — the hint is an
// index, so it is rebuilt from its data file.
// ---------------------------------------------------------------------------
TEST_CASE("DB::open rebuilds a hint file a read fails on",
          "[bytecask][recovery]") {
  TempDir td;
  const auto dir = td.path / "db";
  constexpr int kKeys = 40;
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 256});
    for (int i = 0; i < kKeys; ++i)
      db.put({}, to_bytes(std::format("k{:03d}", i)),
             to_bytes(std::format("v{:03d}", i)));
  }
  std::size_t hint_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".hint") ++hint_count;
  REQUIRE(hint_count > 2);

  auto check_keys = [&](const bytecask::DB &db) {
    for (int i = 0; i < kKeys; ++i) {
      const auto v = get_val(db, to_bytes(std::format("k{:03d}", i)));
      INFO("k" << i);
      REQUIRE(v.has_value());
      CHECK(to_string(*v) == std::format("v{:03d}", i));
    }
  };

  SECTION("an error in the pass that opens a hint rebuilds it") {
    {
      // The first read of the first hint fails. Strict recovery neither skips
      // a file nor gets past one it cannot index, so an open that succeeds,
      // with the reads that followed, rebuilt that hint.
      bytecask::testing::ScopedFaultInjector fi{"io_hint_read"};
      fi.inj.fail_on_nth_match = 1;
      auto db = bytecask::DB::open(dir, {.max_file_bytes = 256,
                                         .recovery_threads = 1});
      REQUIRE(fi.inj.name_matches > 1);
      check_keys(db);
    }
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 256});
    check_keys(db);
  }

  SECTION("an error while a verified hint is scanned fails the open") {
    // One data file, so one hint, read first: its open pass, then its scan.
    const auto one = td.path / "one";
    {
      auto db = bytecask::DB::open(one);
      for (int i = 0; i < kKeys; ++i)
        db.put({}, to_bytes(std::format("k{:03d}", i)),
               to_bytes(std::format("v{:03d}", i)));
    }
    std::vector<std::filesystem::path> hints;
    for (const auto &e : std::filesystem::directory_iterator{one})
      if (e.path().extension() == ".hint") hints.push_back(e.path());
    REQUIRE(hints.size() == 1);
    // The read after those that open the hint is the first of its scan.
    int open_reads = 0;
    {
      bytecask::testing::ScopedFaultInjector fi{"io_hint_read"};
      fi.inj.fail_on_nth_match = std::numeric_limits<int>::max();
      (void)bytecask::HintFile::OpenForRead(hints[0]);
      open_reads = fi.inj.name_matches;
    }
    REQUIRE(open_reads > 0);
    const auto hint_size = std::filesystem::file_size(hints[0]);
    {
      bytecask::testing::ScopedFaultInjector fi{"io_hint_read"};
      fi.inj.fail_on_nth_match = open_reads + 1;
      CHECK_THROWS_AS(bytecask::DB::open(one, {.recovery_threads = 1}),
                      std::system_error);
    }
    // Nothing was rebuilt or skipped, and the next open recovers every key.
    CHECK(std::filesystem::file_size(hints[0]) == hint_size);
    auto db = bytecask::DB::open(one);
    check_keys(db);
  }
}

// ---------------------------------------------------------------------------
// Recovery holds at most one hint file open per thread (#251).
//
// Every data file stays open for the life of the DB, so an open needs one
// descriptor per data file. The hints must not double that: a scanner opens
// its file for the time it takes to read one unit, so however many files a
// merge holds cursors on, its hints cost the thread one descriptor.
// ---------------------------------------------------------------------------
#ifdef __linux__
namespace {
// Descriptors this process holds, not counting the one that lists them.
auto open_fd_count() -> std::size_t {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto &e :
       std::filesystem::directory_iterator{"/proc/self/fd"})
    ++n;
  return n - 1;
}

// Lowers the soft RLIMIT_NOFILE for its scope.
struct ScopedFdLimit {
  rlimit saved{};
  explicit ScopedFdLimit(std::size_t soft) {
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
    auto rl = saved;
    rl.rlim_cur = static_cast<rlim_t>(soft);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &rl) == 0);
  }
  ~ScopedFdLimit() { (void)::setrlimit(RLIMIT_NOFILE, &saved); }
  ScopedFdLimit(const ScopedFdLimit &) = delete;
  ScopedFdLimit &operator=(const ScopedFdLimit &) = delete;
};
} // namespace
#endif

TEST_CASE("DB::open needs one descriptor per data file, not two",
          "[bytecask][recovery]") {
#ifndef __linux__
  SKIP("counts descriptors through /proc/self/fd");
#else
  TempDir td;
  const auto dir = td.path / "db";
  constexpr int kKeys = 800;
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 256});
    for (int i = 0; i < kKeys; ++i)
      db.put({.sync = false}, to_bytes(std::format("k{:03d}", i)),
             to_bytes(std::format("v{:03d}", i)));
    db.close();
  }
  auto check_keys = [&](const bytecask::DB &db) {
    for (int i = 0; i < kKeys; ++i) {
      const auto v = get_val(db, to_bytes(std::format("k{:03d}", i)));
      INFO("k" << i);
      REQUIRE(v.has_value());
      CHECK(to_string(*v) == std::format("v{:03d}", i));
    }
  };

  for (const unsigned threads : {1u, 4u}) {
    DYNAMIC_SECTION("recovery_threads = " << threads) {
      std::size_t data_files = 0;
      for (const auto &e : std::filesystem::directory_iterator{dir})
        if (e.path().extension() == ".data") ++data_files;
      // Room for what the open DB holds — every data file, the active file
      // it adds, the lock — one hint per recovery thread, and the transient
      // descriptors of an open: its directory listing, the hint it writes
      // for the file the last open left active, the directory it syncs. Not
      // for a hint per data file.
      constexpr std::size_t kSlack = 8;
      REQUIRE(data_files > threads + kSlack + 16);
      ScopedFdLimit limit{open_fd_count() + data_files + 2 + threads + kSlack};
      auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
      check_keys(db);
      // Recovery done, no hint file is held open.
      for (const auto &e : std::filesystem::directory_iterator{"/proc/self/fd"}) {
        std::error_code ec;
        const auto target = std::filesystem::read_symlink(e.path(), ec);
        if (!ec) CHECK(target.extension() != ".hint");
      }
    }
  }
#endif
}

// ---------------------------------------------------------------------------
// A hint is rebuilt only on an error that says its bytes are bad (#251).
//
// A CRC or parse failure, or EIO, says the hint is unusable, and the rebuild
// replaces it. EMFILE, ENOMEM or EACCES say nothing about the hint: a rebuild
// on those would delete a good hint and then fail to write it back, and a
// lenient open would then skip the file behind it. Both modes let the error
// out, and the hint stays as it was.
// ---------------------------------------------------------------------------
TEST_CASE("DB::open leaves a hint alone on an error that says nothing about "
          "its bytes",
          "[bytecask][recovery]") {
  TempDir td;
  const auto dir = td.path / "db";
  constexpr int kKeys = 40;
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 256});
    for (int i = 0; i < kKeys; ++i)
      db.put({}, to_bytes(std::format("k{:03d}", i)),
             to_bytes(std::format("v{:03d}", i)));
  }
  auto inode_of = [](const std::filesystem::path &p) {
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_ino;
  };
  const auto hints = list_hint_files(dir);
  REQUIRE(hints.size() > 2);
  std::vector<ino_t> inodes;
  for (const auto &h : hints) inodes.push_back(inode_of(h));

  for (const bool strict : {true, false}) {
    DYNAMIC_SECTION((strict ? "strict" : "lenient")) {
      {
        // The first read of the first hint: in the pass that opens it, where
        // a read that said the hint was damaged would have it rebuilt.
        bytecask::testing::ScopedFaultInjector fi{"io_hint_read"};
        fi.inj.fail_on_nth_match = 1;
        fi.inj.error = std::make_error_code(std::errc::too_many_files_open);
        try {
          auto db = bytecask::DB::open(
              dir, {.recovery_threads = 1,
                    .fail_recovery_on_crc_errors = strict});
          FAIL("open succeeded");
        } catch (const std::system_error &e) {
          CHECK(e.code() == std::errc::too_many_files_open);
        }
      }
      // Every hint is the file it was: none removed, none rewritten. (The
      // open may have added one, for the file the last open left active.)
      for (std::size_t i = 0; i < hints.size(); ++i) {
        INFO(hints[i]);
        CHECK(inode_of(hints[i]) == inodes[i]);
      }
      auto db = bytecask::DB::open(dir);
      for (int i = 0; i < kKeys; ++i) {
        const auto v = get_val(db, to_bytes(std::format("k{:03d}", i)));
        INFO("k" << i);
        REQUIRE(v.has_value());
        CHECK(to_string(*v) == std::format("v{:03d}", i));
      }
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
// Readers traverse the key directory using raw pointers while a writer mutates
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
#ifdef __EMSCRIPTEN__
  if (io_backend == bytecask::IoBackend::Mmap) SKIP("DB::open rejects Mmap on WASM");
#endif
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
TEST_CASE("open removes staged .data and .hint files and nothing else",
          "[recovery]") {
  TempDir td;
  const auto dir = td.path / "db";
  bytecask::DB::open(dir).put({}, to_bytes("k"), to_bytes("v"));
  for (const auto *name : {"x.data.tmp", "x.hint.tmp", "x.tmp", "x.other.tmp"})
    std::ofstream{dir / name} << "staged";

  auto db = bytecask::DB::open(dir);
  CHECK_FALSE(std::filesystem::exists(dir / "x.data.tmp"));
  CHECK_FALSE(std::filesystem::exists(dir / "x.hint.tmp"));
  CHECK(std::filesystem::exists(dir / "x.tmp"));
  CHECK(std::filesystem::exists(dir / "x.other.tmp"));
  CHECK(get_val(db, to_bytes("k")).has_value());
}

// ---------------------------------------------------------------------------
// A hint file that verifies but breaks the sorted layout flush_hints_for
// writes — a head of markers and range tombstones, then Puts and Deletes in
// key order — is refused by the B+ tree recoveries, which bulk-load from it.
// ---------------------------------------------------------------------------
namespace {

struct OwnedHint {
  std::uint64_t seq;
  bytecask::EntryType type;
  std::uint64_t offset;
  std::string key;
  std::uint32_t value_size;
  std::string end_key;
};

auto only_hint(const std::filesystem::path &dir) -> std::filesystem::path {
  std::vector<std::filesystem::path> hints;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".hint") hints.push_back(e.path());
  REQUIRE(hints.size() == 1);
  return hints.front();
}

auto read_hint(const std::filesystem::path &p) -> std::vector<OwnedHint> {
  const auto hf = bytecask::HintFile::OpenForRead(p);
  auto scanner = hf.make_scanner();
  std::vector<OwnedHint> out;
  while (const auto he = scanner.next()) {
    out.push_back({he->sequence, he->entry_type, he->file_offset,
                   to_string(he->key), he->value_size,
                   to_string(he->end_key)});
  }
  return out;
}

void write_hint(const std::filesystem::path &p,
                const std::vector<OwnedHint> &entries) {
  const auto tmp = std::filesystem::path{p.string() + ".crafted"};
  auto hf = bytecask::HintFile::OpenForWrite(tmp);
  for (const auto &e : entries) {
    if (e.type == bytecask::EntryType::RangeDel) {
      hf.append_range_del(e.seq, e.offset, to_bytes(e.key), to_bytes(e.end_key));
    } else {
      hf.append(e.seq, e.type, e.offset, to_bytes(e.key), e.value_size);
    }
  }
  hf.close();
  std::filesystem::rename(tmp, p);
}

}  // namespace

TEST_CASE("recovery checks the layout of a hint file's sorted run",
          "[recovery][hintfile]") {
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    bytecask::WritePlan plan;
    plan.put(to_bytes("a"), to_bytes("1"));
    plan.put(to_bytes("b"), to_bytes("2"));
    (void)db.apply_batch({}, std::move(plan));
    (void)db.del({}, to_bytes("b"));
    db.del_range({}, to_bytes("c"), to_bytes("d"));
  }
  const auto hint = only_hint(dir);
  const auto written = read_hint(hint);
  // The head (both batch markers, then the range tombstone) and the run: a,
  // then b's Delete before its older Put.
  using bytecask::EntryType;
  REQUIRE(written.size() == 6);
  const auto &[begin, end, range, a, b_del, b_put] =
      std::tie(written[0], written[1], written[2], written[3], written[4],
               written[5]);
  REQUIRE(begin.type == EntryType::BulkBegin);
  REQUIRE(end.type == EntryType::BulkEnd);
  REQUIRE(range.type == EntryType::RangeDel);
  REQUIRE(a.key == "a");
  REQUIRE(b_del.type == EntryType::Delete);
  REQUIRE(b_put.type == EntryType::Put);

  SECTION("batch markers inside the run are passed over") {
    write_hint(hint, {range, a, begin, b_del, end, b_put});
    auto db = bytecask::DB::open(dir);
    CHECK(get_val(db, to_bytes("a")).has_value());
    CHECK_FALSE(get_val(db, to_bytes("b")).has_value());
  }
  SECTION("keys descending are refused") {
    write_hint(hint, {begin, end, range, b_del, b_put, a});
    // Each recovery words it its own way.
    CHECK_THROWS_WITH(bytecask::DB::open(dir),
                      Catch::Matchers::ContainsSubstring("not sorted") ||
                          Catch::Matchers::ContainsSubstring("not ascending"));
  }
  SECTION("a range tombstone after the head is refused") {
    write_hint(hint, {begin, end, a, range, b_del, b_put});
    CHECK_THROWS_WITH(
        bytecask::DB::open(dir),
        Catch::Matchers::ContainsSubstring("range tombstone inside"));
  }
}

TEST_CASE("BC_RECOVERY_PHASES=1 times the recovery phases", "[recovery]") {
  // The switch only prints; the open must recover the same either way.
  TempDir td;
  const auto dir = td.path / "db";
  bytecask::DB::open(dir).put({}, to_bytes("k"), to_bytes("v"));
  for (const auto *value : {"1", "0"}) {
    INFO("BC_RECOVERY_PHASES=" << value);
    ::setenv("BC_RECOVERY_PHASES", value, 1);
    auto db = bytecask::DB::open(dir);
    ::unsetenv("BC_RECOVERY_PHASES");
    CHECK(get_val(db, to_bytes("k")).has_value());
  }
}

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

TEST_CASE("vacuum remaps only the keys no write changed during its copy",
          "[vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("a0"));
  db.put({}, to_bytes("b"), to_bytes("b0"));
  db.put({}, to_bytes("c"), to_bytes("c0"));
  db.put({}, to_bytes("c"), to_bytes("c1"));  // the dead bytes to reclaim
  (void)db.create_manifest();                 // seals the file

  // The copy holds a, b and c; by the commit a is rewritten and b deleted,
  // so only c may be remapped into it.
  db.test_before_vacuum_commit_ = [&] {
    db.put({}, to_bytes("a"), to_bytes("a1"));
    (void)db.del({}, to_bytes("b"));
  };
  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));
  db.test_before_vacuum_commit_ = nullptr;

  const auto expected = std::map<std::string, std::string>{{"a", "a1"},
                                                           {"c", "c1"}};
  CHECK(collect_kv(db) == expected);
  std::uint64_t live = 0;
  for (const auto &[fid, fs] : db.file_stats()) live += fs.live_bytes;
  CHECK(live == esize("a", "a1") + esize("c", "c1"));

  db.close();
  auto reopened = bytecask::DB::open(td.path / "db");
  CHECK(collect_kv(reopened) == expected);
}

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

// Two entries under one sequence that differ in value, key or type are two
// writes, not a copy. Recovery refuses them and deletes nothing.
TEST_CASE("recovery refuses two different writes under one sequence",
          "[vacuum][recovery]") {
  using Write = std::function<void(bytecask::DB &)>;
  const auto put = [](std::string k, std::string v) -> Write {
    return [k, v](bytecask::DB &db) { db.put({}, to_bytes(k), to_bytes(v)); };
  };
  const auto del = [](std::string k) -> Write {
    return [k](bytecask::DB &db) { (void)db.del({}, to_bytes(k)); };
  };
  // Sequence 1 is the same write in both; sequence 2 differs.
  const auto [what, second_a, second_b] =
      GENERATE_COPY(table<std::string, Write, Write>({
          {"value", put("key", "aaaa"), put("key", "bbbb")},
          {"key", put("key", "aaaa"), put("kez", "aaaa")},
          {"type", put("key", "aaaa"), del("key")},
      }));
  INFO("the second writes differ in " << what);
  TempDir td;
  const auto a = td.path / "a";
  const auto b = td.path / "b";
  {
    auto db = bytecask::DB::open(a);
    db.put({}, to_bytes("key"), to_bytes("0000"));
    second_a(db);
  }
  {
    auto db = bytecask::DB::open(b);
    db.put({}, to_bytes("key"), to_bytes("0000"));
    second_b(db);
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

// A vacuum that fails after its rename and before its commit removes its
// copy, as one that fails before the rename removes its staging file (#235).
// Left behind, a vacuum retried under a persistent fault placed one copy per
// attempt, and the next open ran a full recovery pass per copy (#304).
TEST_CASE("vacuum that fails between its rename and its commit removes its "
          "copy",
          "[vacuum][recovery]") {
  const auto checkpoint = GENERATE(
      as<std::string>{}, "io_dir_sync_vacuum", "io_vacuum_compact_post_rename",
      "io_hint_write", "io_dir_sync_hint");
  INFO("failing at " << checkpoint);
  TempDir td;
  const auto db_path = td.path / "db";
  const bytecask::Options opts{.max_file_bytes = 4096};
  std::map<std::string, std::string> oracle;
  {
    auto db = bytecask::DB::open(db_path, opts);
    for (int i = 0; i < 200; ++i) {
      auto k = std::format("k{:04d}", i);
      auto v = std::format("v{:04d}", i) + std::string(40, 'x');
      db.put({.sync = false}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
    }
    for (int i = 0; i < 40; i += 2) {
      auto k = std::format("k{:04d}", i);
      auto v = std::format("w{:04d}", i) + std::string(40, 'y');
      db.put({.sync = false}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
    }
  }
  // Reopened, so every sealed file has its hint and no background hint write
  // can meet the fault.
  auto db = bytecask::DB::open(db_path, opts);
  const auto all_files = [&] {
    std::set<std::filesystem::path> out;
    for (const auto &e : std::filesystem::directory_iterator{db_path}) {
      out.insert(e.path().filename());
    }
    return out;
  };
  const auto before = all_files();
  {
    bytecask::testing::ScopedFaultInjector fi{checkpoint};
    for (int attempt = 0; attempt < 3; ++attempt) {
      REQUIRE_THROWS_AS(db.vacuum({.fragmentation_threshold = 0.0}),
                        std::system_error);
    }
  }
  CHECK(all_files() == before);
  CHECK_FALSE(db.is_degraded());
  CHECK(collect_kv(db) == oracle);

  // With the fault gone the vacuum runs, and the reopen finds no copy to
  // remove: every file is still there, beside the new active file.
  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));
  const auto vacuumed = data_files_in(db_path);
  db.close();
  auto reopened = bytecask::DB::open(db_path, opts);
  CHECK(std::ranges::includes(data_files_in(db_path), vacuumed));
  CHECK(collect_kv(reopened) == oracle);
  CHECK(disjoint(sequence_ranges(reopened)));
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

// #166: a sealed file whose only entries are tombstones has live_bytes == 0.
// vacuum() used to drop such a file whole, tombstones included, and the Put
// they shadowed in an older file came back at the next open. The older file
// here also holds a live key, so it stays below the threshold and keeps its
// dead Put on disk.
TEST_CASE("vacuum keeps a tombstone-only file that shadows an older put",
          "[vacuum][tombstone]") {
  TempDir td;
  const auto path = td.path / "db";
  auto shadow = [&](bytecask::DB &db) {
    bytecask::WritePlan plan;  // one file: a (soon dead) and keep (live)
    plan.put(to_bytes("a"), to_bytes("old"));
    plan.put(to_bytes("keep"), to_bytes("x"));
    REQUIRE(db.apply_batch({}, std::move(plan)));
  };
  SECTION("point delete") {
    {
      auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
      shadow(db);
      REQUIRE(db.del({}, to_bytes("a")));
      db.put({}, to_bytes("z"), to_bytes("y"));
      std::ignore = db.vacuum({.fragmentation_threshold = 0.9});
      CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    }
    auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
    CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    CHECK(db.contains_key({}, to_bytes("keep")));
  }
  SECTION("range delete") {
    {
      auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
      shadow(db);
      db.del_range({}, to_bytes("a"), to_bytes("b"));
      db.put({}, to_bytes("z"), to_bytes("y"));
      std::ignore = db.vacuum({.fragmentation_threshold = 0.9});
      CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    }
    auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
    CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    CHECK(db.contains_key({}, to_bytes("keep")));
  }
  SECTION("tombstone beside a dead put") {
    // The dead put gives the file something to reclaim, so it is selected;
    // with no live key it must still be compacted, not dropped.
    {
      auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
      shadow(db);
      {
        bytecask::WritePlan plan;
        plan.del(to_bytes("a"));
        plan.put(to_bytes("b"), to_bytes("soon dead"));
        REQUIRE(db.apply_batch({}, std::move(plan)));
      }
      db.put({}, to_bytes("b"), to_bytes("new"));
      // The batch's markers are kept too, so its dead put is about a third
      // of it; the older file's dead put is about a quarter.
      REQUIRE(db.vacuum({.fragmentation_threshold = 0.3}));
      CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    }
    auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
    CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    CHECK(to_string(*get_val(db, to_bytes("b"))) == "new");
  }
}

// Tombstones are kept by every compaction, so they count as kept bytes when
// vacuum measures fragmentation: a file of nothing but tombstones has nothing
// to reclaim, is never selected, and a vacuum-to-convergence loop ends.
TEST_CASE("vacuum does not select a file holding only tombstones",
          "[vacuum][tombstone][filestats]") {
  TempDir td;
  const auto path = td.path / "db";
  std::map<std::uint32_t, bytecask::FileStats> before;
  {
    auto db = bytecask::DB::open(path, {.max_file_bytes = 1});
    db.put({}, to_bytes("a"), to_bytes("1"));
    REQUIRE(db.del({}, to_bytes("a")));
    db.del_range({}, to_bytes("m"), to_bytes("n"));
    db.put({}, to_bytes("z"), to_bytes("y"));

    std::uint64_t tombstones = 0;
    for (const auto &[fid, fs] : db.file_stats()) tombstones += fs.tombstone_bytes;
    CHECK(tombstones == esize("a", "") + esize("m", "n"));

    int spins = 0;
    while (db.vacuum({.fragmentation_threshold = 0.0})) REQUIRE(++spins < 10);
    // The dead Put's file was the only one with anything to reclaim.
    CHECK(spins == 1);
    CHECK_FALSE(db.contains_key({}, to_bytes("a")));
    before = db.file_stats();
  }
  // Recovery rebuilds tombstone_bytes from the hint files.
  for (const unsigned threads : {1U, 4U}) {
    const auto copy = td.path / std::format("copy{}", threads);
    std::filesystem::copy(path, copy, std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(copy, {.max_file_bytes = 1,
                                        .recovery_threads = threads});
    std::vector<std::uint64_t> want;
    std::vector<std::uint64_t> got;
    // Every open starts a fresh, empty active file; only files with bytes
    // in them are the same files on both sides.
    for (const auto &[fid, fs] : before)
      if (fs.total_bytes > 0) want.push_back(fs.tombstone_bytes);
    for (const auto &[fid, fs] : db.file_stats())
      if (fs.total_bytes > 0) got.push_back(fs.tombstone_bytes);
    std::ranges::sort(want);
    std::ranges::sort(got);
    CHECK(got == want);
    CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  }
}

// ---------------------------------------------------------------------------
// Tombstone elimination (#167)
//
// A tombstone matters only while another file holds an older Put of a key it
// deletes. Recovery records which tombstones are in that position; compaction
// drops the rest, but only in files it compacts for their dead Puts.
// ---------------------------------------------------------------------------
namespace {
// Seals the active file: the next write starts a new one.
void seal(bytecask::DB &db) { (void)db.create_manifest(); }

auto tombstones_dropped(const bytecask::DB &db) -> std::int64_t {
  return db.stats().at("bytecask.vacuum_tombstones_dropped");
}

auto tombstone_bytes_on_disk(const bytecask::DB &db) -> std::uint64_t {
  std::uint64_t total = 0;
  for (const auto &[fid, fs] : db.file_stats()) total += fs.tombstone_bytes;
  return total;
}
} // namespace

// A tombstone written since the open was never examined by recovery, so
// compaction keeps it even when nothing would need it.
TEST_CASE("vacuum keeps a tombstone written since the open",
          "[vacuum][tombstone]") {
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("a"), to_bytes("old"));
    db.put({}, to_bytes("keep"), to_bytes(std::string(200, 'k')));
    seal(db);  // A: a (dead below), keep (live) — lightly fragmented
    db.put({}, to_bytes("x"), to_bytes("1"));
    (void)db.del({}, to_bytes("a"));
    seal(db);  // B: x (dead below), del a — no live bytes
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);

    REQUIRE(db.vacuum({.fragmentation_threshold = 0.3}));  // B, not A
    // Written this session: never examined by recovery, so kept.
    CHECK(tombstones_dropped(db) == 0);
    CHECK(tombstone_bytes_on_disk(db) > 0);
  }
  auto db = bytecask::DB::open(dir);
  CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  CHECK(get_str(db, to_bytes("x")) == "2");
}

// The case a rule based on the key directory alone gets wrong: the tombstone
// and a Put of the same key share a file, and a third, older Put sits in
// another file. Compaction drops the dead Put next to the tombstone; the
// tombstone must stay for the older one.
TEST_CASE("vacuum keeps a tombstone when its own file and an older file "
          "both hold a Put",
          "[vacuum][tombstone]") {
  const auto threads = GENERATE(1U, 3U);
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("a"), to_bytes("old"));
    db.put({}, to_bytes("keep"), to_bytes(std::string(200, 'k')));
    seal(db);  // A
    db.put({}, to_bytes("a"), to_bytes("mid"));
    db.put({}, to_bytes("x"), to_bytes("1"));
    (void)db.del({}, to_bytes("a"));
    seal(db);  // T: a=mid (dead), x=1 (dead below), del a
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);
  }
  {
    auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
    CHECK(db.stats().at("bytecask.tombstones_needed") >= 1);
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.3}));  // T, not A
    CHECK(tombstones_dropped(db) == 0);
    CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  }
  auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
  CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  CHECK(get_str(db, to_bytes("x")) == "2");
}

// Nothing older holds the key, so the tombstone goes with the dead Puts and
// the compacted file is empty: it is removed instead of published.
TEST_CASE("vacuum drops a tombstone no older file needs",
          "[vacuum][tombstone]") {
  const auto threads = GENERATE(1U, 3U);
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("b"), to_bytes(std::string(200, 'b')));
    seal(db);  // A: holds no a
    db.put({}, to_bytes("a"), to_bytes("mid"));
    db.put({}, to_bytes("x"), to_bytes("1"));
    (void)db.del({}, to_bytes("a"));
    seal(db);  // T: a=mid (dead), x=1 (dead below), del a
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);
  }
  {
    auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
    CHECK(db.stats().at("bytecask.tombstones_needed") == 0);
    const auto files_before = db.file_stats().size();
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.3}));
    CHECK(tombstones_dropped(db) == 1);
    CHECK(db.file_stats().size() == files_before - 1);
    CHECK(tombstone_bytes_on_disk(db) == 0);
  }
  auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
  CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  CHECK(get_str(db, to_bytes("x")) == "2");
  CHECK(get_str(db, to_bytes("b")) == std::string(200, 'b'));
}

// Once the older Put is compacted away, the next open finds the tombstone
// no longer needed and a later compaction of its file drops it.
TEST_CASE("vacuum drops a tombstone after the Put it hid is gone",
          "[vacuum][tombstone]") {
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("a"), to_bytes("old"));
    seal(db);  // A: only a
    db.put({}, to_bytes("x"), to_bytes("1"));
    db.put({}, to_bytes("y"), to_bytes("1"));
    (void)db.del({}, to_bytes("a"));
    db.put({}, to_bytes("z"), to_bytes(std::string(100, 'z')));
    seal(db);  // T: x, y (dead below), del a, z (live)
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);
  }
  {
    auto db = bytecask::DB::open(dir);
    CHECK(db.stats().at("bytecask.tombstones_needed") == 1);
    // A is all dead and holds no tombstone: removed without a scan. T is
    // below the threshold with only x dead.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.5}));
    CHECK_FALSE(db.vacuum({.fragmentation_threshold = 0.5}));
    db.put({}, to_bytes("y"), to_bytes("2"));  // now T has more dead bytes
    seal(db);
  }
  {
    auto db = bytecask::DB::open(dir);
    CHECK(db.stats().at("bytecask.tombstones_needed") == 0);
    // T is the only file with dead bytes.
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.1}));
    CHECK(tombstones_dropped(db) == 1);
  }
  auto db = bytecask::DB::open(dir);
  CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  CHECK(get_str(db, to_bytes("x")) == "2");
  CHECK(get_str(db, to_bytes("y")) == "2");
}

TEST_CASE("vacuum keeps a range tombstone over a Put in an older file",
          "[vacuum][tombstone][del_range]") {
  const auto threads = GENERATE(1U, 3U);
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("r5"), to_bytes("old"));
    db.put({}, to_bytes("keep"), to_bytes(std::string(200, 'k')));
    seal(db);  // A
    db.put({}, to_bytes("x"), to_bytes(std::string(50, 'x')));
    db.del_range({}, to_bytes("r0"), to_bytes("r9"));
    seal(db);  // T
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);
  }
  {
    auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.3}));  // T, not A
    CHECK(tombstones_dropped(db) == 0);
  }
  auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
  CHECK_FALSE(db.contains_key({}, to_bytes("r5")));
}

TEST_CASE("vacuum drops a range tombstone no older file needs",
          "[vacuum][tombstone][del_range]") {
  const auto threads = GENERATE(1U, 3U);
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("keep"), to_bytes(std::string(200, 'k')));
    seal(db);  // A: nothing in [r0, r9)
    db.put({}, to_bytes("r5"), to_bytes("mid"));
    db.put({}, to_bytes("x"), to_bytes(std::string(50, 'x')));
    db.del_range({}, to_bytes("r0"), to_bytes("r9"));
    seal(db);  // T: its own r5 is the only key in range
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);
  }
  {
    auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
    REQUIRE(db.vacuum({.fragmentation_threshold = 0.3}));
    CHECK(tombstones_dropped(db) == 1);
  }
  auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
  CHECK_FALSE(db.contains_key({}, to_bytes("r5")));
  CHECK(get_str(db, to_bytes("x")) == "2");
}

// A lenient open that skips an unreadable file never saw its Puts, so it
// cannot show any tombstone unneeded: that open drops none.
TEST_CASE("vacuum drops no tombstone after a lenient open skipped a file",
          "[vacuum][tombstone][corruption]") {
  TempDir td;
  const auto dir = td.path / "db";
  std::filesystem::path victim;
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("a"), to_bytes("old"));
    for (const auto &e : std::filesystem::directory_iterator{dir})
      if (e.path().extension() == ".data") victim = e.path();
    seal(db);  // A: the only Put of a
    db.put({}, to_bytes("x"), to_bytes("1"));
    (void)db.del({}, to_bytes("a"));
    seal(db);  // T: x (dead below), del a
    db.put({}, to_bytes("x"), to_bytes("2"));
    seal(db);
  }
  corrupt_hint_beyond_rebuild(dir / (victim.stem().string() + ".hint"));

  auto db = bytecask::DB::open(dir, {.fail_recovery_on_crc_errors = false});
  // The skipped file measures all dead and goes first; then T.
  while (db.vacuum({.fragmentation_threshold = 0.3})) {
  }
  CHECK(tombstones_dropped(db) == 0);
  CHECK(tombstone_bytes_on_disk(db) > 0);
}

// Batch markers are kept by every compaction, so they are not reclaimable.
// Counted as dead, they made a batch file of live Puts look more fragmented
// than a file with real dead bytes; vacuum picked it, could not shrink it,
// and returned false with the other file never reclaimed (#169).
TEST_CASE("vacuum is not stalled by a file whose only dead bytes are markers",
          "[vacuum][batch]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  bytecask::WritePlan plan;
  plan.put(to_bytes("a"), to_bytes("1"));
  plan.put(to_bytes("b"), to_bytes("1"));
  (void)db.apply_batch({}, std::move(plan));
  seal(db);  // A: a batch of two live Puts, about half markers
  db.put({}, to_bytes("c"), to_bytes(std::string(200, 'c')));
  db.put({}, to_bytes("d"), to_bytes("1"));
  seal(db);  // B: c (live), d (dead below)
  db.put({}, to_bytes("d"), to_bytes("2"));
  seal(db);

  for (const auto &[fid, fs] : db.file_stats())
    if (fs.marker_bytes > 0) CHECK(fs.reclaimable_bytes() == 0);
  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));  // B
  CHECK_FALSE(db.vacuum({.fragmentation_threshold = 0.0}));
  CHECK(get_str(db, to_bytes("a")) == "1");
  CHECK(get_str(db, to_bytes("d")) == "2");
}

// Random writes with vacuum between them, over many opens: after every
// vacuum and every reopen the database must equal the model. Dropping a
// tombstone that still hid a Put would bring a key back at the next open.
// Vacuum runs one file at a time at thresholds that leave lightly fragmented
// files alone, so older Puts outlive the compaction of their tombstones.
TEST_CASE("Vacuum model-based: dropping tombstones never resurrects a key",
          "[vacuum][tombstone][model]") {
  std::mt19937 gen(16716);
  auto pick = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(gen);
  };
  // A wide key space, so a deleted key usually stays deleted until the next
  // open — where a wrongly dropped tombstone would show.
  constexpr int kKeys = 400;
  auto key_at = [](int i) { return std::format("k{:03d}", i); };
  auto rand_value = [&] {
    return std::string(static_cast<std::size_t>(pick(1, 300)), 'v') +
           std::to_string(pick(0, 999));
  };

  TempDir td;
  const auto dir = td.path / "db";
  std::map<std::string, std::string> oracle;

  auto verify = [&](bytecask::DB &db, const std::string &label) {
    INFO(label);
    std::map<std::string, std::string> kv;
    for (auto &entry : db.iter_from({}))
      kv[to_string(entry.key)] = to_string(entry.value);
    CHECK(kv == oracle);
  };

  std::int64_t dropped = 0;
  for (int session = 0; session < 24; ++session) {
    const auto threads = static_cast<unsigned>(1 + session % 5);
    auto db = bytecask::DB::open(
        dir, {.max_file_bytes = 2048, .recovery_threads = threads});
    verify(db, std::format("open {}", session));
    for (int op = 0; op < 150; ++op) {
      const auto r = pick(0, 99);
      if (r < 45 || oracle.empty()) {
        const auto k = key_at(pick(0, kKeys - 1));
        const auto v = rand_value();
        db.put({.sync = false}, to_bytes(k), to_bytes(v));
        oracle[k] = v;
      } else if (r < 75) {
        auto it = oracle.begin();
        std::advance(it, pick(0, static_cast<int>(oracle.size()) - 1));
        const auto k = it->first;
        std::ignore = db.del({.sync = false}, to_bytes(k));
        oracle.erase(k);
      } else if (r < 80) {
        const auto lo = pick(0, kKeys - 2);
        const auto from = key_at(lo);
        const auto to = key_at(lo + pick(1, 4));
        db.del_range({.sync = false}, to_bytes(from), to_bytes(to));
        for (auto it = oracle.lower_bound(from);
             it != oracle.end() && it->first < to;)
          it = oracle.erase(it);
      } else {
        bytecask::WritePlan plan;
        std::map<std::string, std::optional<std::string>> staged;
        for (int b = pick(2, 5); b > 0; --b) {
          const auto k = key_at(pick(0, kKeys - 1));
          if (pick(0, 2) == 0) {
            plan.del(to_bytes(k));
            staged[k] = std::nullopt;
          } else {
            const auto v = rand_value();
            plan.put(to_bytes(k), to_bytes(v));
            staged[k] = v;
          }
        }
        REQUIRE(db.apply_batch({.sync = false}, std::move(plan)));
        for (const auto &[k, v] : staged) {
          if (v) oracle[k] = *v;
          else oracle.erase(k);
        }
      }
      if (op % 10 == 9) {
        constexpr std::array kThresholds{0.3, 0.5, 0.7};
        const auto threshold =
            kThresholds[static_cast<std::size_t>(pick(0, 2))];
        // Half the passes keep history above a random point, as a
        // replication service would: dead entries above it stay on disk.
        const auto retain =
            pick(0, 1) == 0
                ? std::numeric_limits<std::uint64_t>::max()
                : db.durable_sequence() * static_cast<std::uint64_t>(pick(0, 100)) / 100;
        std::ignore = db.vacuum(
            {.fragmentation_threshold = threshold, .retain_after = retain});
        verify(db, std::format("session {} op {}", session, op));
      }
    }
    dropped += tombstones_dropped(db);
  }
  for (const unsigned threads : {1U, 4U}) {
    auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
    verify(db, std::format("final, {} threads", threads));
  }
  // The test proves nothing unless tombstones were actually dropped.
  CHECK(dropped > 0);
}

// The replication contract of retain_after: a follower that resumes
// changes_since from its own durable_sequence() converges with the leader,
// as long as every vacuum kept what lies above that point — across leader
// restarts, with dead Puts, tombstones, range deletes and batches in the
// history.
TEST_CASE("Vacuum model-based: a follower resuming at retain_after converges",
          "[vacuum][replication][model]") {
  std::mt19937 gen(19316);
  auto pick = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(gen);
  };
  constexpr int kKeys = 120;
  auto key_at = [](int i) { return std::format("k{:03d}", i); };
  auto rand_value = [&] {
    return std::string(static_cast<std::size_t>(pick(1, 200)), 'v') +
           std::to_string(pick(0, 999));
  };

  TempDir td;
  auto follower = bytecask::DB::open(
      td.path / "follower", {.max_file_bytes = 2048,
                             .initial_mode = bytecask::Mode::Follower});
  auto catch_up = [&](bytecask::DB &leader) {
    auto snap = leader.snapshot();
    std::vector<bytecask::DataEntry> owned;
    for (const auto &e : leader.changes_since(snap, follower.durable_sequence()))
      owned.push_back({e.sequence, e.entry_type,
                       {e.key.begin(), e.key.end()},
                       {e.value.begin(), e.value.end()}});
    std::vector<bytecask::DataEntryView> views;
    for (const auto &e : owned)
      views.push_back({e.sequence, e.entry_type, e.key, e.value});
    if (!views.empty()) follower.ingest(views);
    CHECK(follower.durable_sequence() == leader.durable_sequence());
    CHECK(collect_kv(follower) == collect_kv(leader));
  };

  std::int64_t reclaimed = 0;
  for (int session = 0; session < 10; ++session) {
    auto leader = bytecask::DB::open(td.path / "leader",
                                     {.max_file_bytes = 2048});
    for (int op = 0; op < 120; ++op) {
      const auto r = pick(0, 99);
      if (r < 50) {
        leader.put({}, to_bytes(key_at(pick(0, kKeys - 1))),
                   to_bytes(rand_value()));
      } else if (r < 75) {
        std::ignore = leader.del({}, to_bytes(key_at(pick(0, kKeys - 1))));
      } else if (r < 80) {
        const auto lo = pick(0, kKeys - 2);
        leader.del_range({}, to_bytes(key_at(lo)),
                         to_bytes(key_at(lo + pick(1, 4))));
      } else {
        bytecask::WritePlan plan;
        for (int b = pick(2, 5); b > 0; --b) {
          const auto k = key_at(pick(0, kKeys - 1));
          if (pick(0, 2) == 0) plan.del(to_bytes(k));
          else plan.put(to_bytes(k), to_bytes(rand_value()));
        }
        REQUIRE(leader.apply_batch({}, std::move(plan)));
      }
      // The follower lags: it catches up only now and then, while the
      // leader vacuums everything its position allows.
      if (op % 7 == 6) {
        while (leader.vacuum({.fragmentation_threshold = 0.0,
                              .retain_after = follower.durable_sequence()})) {
        }
      }
      if (op % 30 == 29) catch_up(leader);
    }
    reclaimed += leader.stats().at("bytecask.vacuum_bytes_reclaimed");
  }
  {
    auto leader = bytecask::DB::open(td.path / "leader",
                                     {.max_file_bytes = 2048});
    catch_up(leader);
  }
  // The test proves nothing unless vacuum dropped history below the
  // follower while it lagged.
  CHECK(reclaimed > 0);
}

// Recovery rebuilds tombstone_bytes and marker_bytes from the hint files to
// the values the write path kept.
TEST_CASE("FileStats: tombstone and marker bytes survive reopen",
          "[filestats][tombstone]") {
  const auto threads = GENERATE(1U, 3U);
  TempDir td;
  const auto dir = td.path / "db";
  std::vector<FileStatsTuple> before;
  {
    auto db = bytecask::DB::open(dir);
    for (int round = 0; round < 4; ++round) {
      bytecask::WritePlan plan;
      plan.put(to_bytes(std::format("k{}", round)), to_bytes("v"));
      plan.put(to_bytes(std::format("j{}", round)), to_bytes("v"));
      plan.del(to_bytes(std::format("k{}", round - 1)));
      (void)db.apply_batch({}, std::move(plan));
      (void)db.del({}, to_bytes(std::format("j{}", round)));
      db.del_range({}, to_bytes("m0"), to_bytes(std::format("m{}", round + 1)));
      seal(db);
    }
    before = collect_file_stats(db);
  }
  auto db = bytecask::DB::open(dir, {.recovery_threads = threads});
  auto after = collect_file_stats(db);
  // Each open starts an empty active file.
  const auto empty = [](const FileStatsTuple &t) { return std::get<1>(t) == 0; };
  std::erase_if(before, empty);
  std::erase_if(after, empty);
  CHECK(after == before);
  for (const auto &t : after) {
    CHECK(std::get<4>(t) > 0);  // every file holds tombstones
    CHECK(std::get<5>(t) > 0);  // and a batch's markers
  }
}

// ---------------------------------------------------------------------------
// VacuumOptions::retain_after: history a follower still needs is kept.
// ---------------------------------------------------------------------------

namespace {

auto stream_sequences(const bytecask::DB &db, std::uint64_t from)
    -> std::vector<std::uint64_t> {
  auto snap = db.snapshot();
  std::vector<std::uint64_t> seqs;
  for (const auto &e : db.changes_since(snap, from)) seqs.push_back(e.sequence);
  return seqs;
}

// Every sequence in (from, durable_sequence()] is in the stream: each write
// below is a single sync put, so the leader assigned them without gaps.
void check_complete_from(const bytecask::DB &db, std::uint64_t from) {
  const auto seqs = stream_sequences(db, from);
  REQUIRE(seqs.size() == db.durable_sequence() - from);
  for (std::size_t i = 0; i < seqs.size(); ++i) CHECK(seqs[i] == from + 1 + i);
}

} // namespace

TEST_CASE("vacuum keeps dead entries above retain_after", "[vacuum][retain]") {
  TempDir td;
  const auto dir = td.path / "db";
  std::uint64_t retain = 0;
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 128});
    for (int i = 0; i < 10; ++i) {
      const auto seq =
          db.put({}, to_bytes(std::format("k{}", i)), to_bytes("old")).sequence;
      if (i == 4) retain = seq;
    }
    for (int i = 0; i < 10; ++i)
      db.put({}, to_bytes(std::format("k{}", i)), to_bytes("new"));

    while (db.vacuum({.fragmentation_threshold = 0.0, .retain_after = retain})) {
    }
    CHECK(db.stats().at("bytecask.vacuum_bytes_reclaimed") > 0);
    // Dropped at or below retain_after, kept above it.
    CHECK(stream_sequences(db, 0).front() > 1);
    check_complete_from(db, retain);
    CHECK(collect_kv(db).at("k7") == "new");
  }
  // The kept dead Puts are history, not live data, after recovery too.
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 128});
  check_complete_from(db, retain);
  for (int i = 0; i < 10; ++i)
    CHECK(collect_kv(db).at(std::format("k{}", i)) == "new");
  // Once the follower moves on, the rest can go.
  while (db.vacuum({.fragmentation_threshold = 0.0})) {
  }
  CHECK(collect_kv(db).at("k9") == "new");
}

TEST_CASE("vacuum keeps tombstones above retain_after",
          "[vacuum][retain][tombstone]") {
  TempDir td;
  const auto dir = td.path / "db";
  std::uint64_t retain = 0;
  {
    auto db = bytecask::DB::open(dir);
    retain = db.put({}, to_bytes("a"), to_bytes("1")).sequence;
    seal(db);
    for (int i = 0; i < 6; ++i) {
      db.put({}, to_bytes(std::format("t{}", i)), to_bytes("v"));
      (void)db.del({}, to_bytes(std::format("t{}", i)));
    }
    seal(db);
  }
  // Recovery decides at open which tombstones may go: these hide only Puts
  // in their own file, so all six may.
  auto db = bytecask::DB::open(dir);
  while (db.vacuum({.fragmentation_threshold = 0.0, .retain_after = retain})) {
  }
  CHECK(db.stats().at("bytecask.vacuum_tombstones_dropped") == 0);
  check_complete_from(db, retain);
  // Without the restriction they go.
  while (db.vacuum({.fragmentation_threshold = 0.0})) {
  }
  CHECK(db.stats().at("bytecask.vacuum_tombstones_dropped") == 6);
}

TEST_CASE("vacuum is not stalled by a file whose dead entries are all above "
          "retain_after",
          "[vacuum][retain]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  // File 1: six keys, one of them later overwritten — a little to reclaim,
  // all of it at or below the retention point.
  for (int i = 0; i < 6; ++i)
    db.put({}, to_bytes(std::format("a{}", i)), to_bytes("xxxxxxxx"));
  seal(db);
  // File 2: two keys that stay live, then four that die, above the point.
  db.put({}, to_bytes("l0"), to_bytes("xxxxxxxx"));
  const auto retain =
      db.put({}, to_bytes("l1"), to_bytes("xxxxxxxx")).sequence;
  for (int i = 0; i < 4; ++i)
    db.put({}, to_bytes(std::format("d{}", i)), to_bytes("xxxxxxxx"));
  seal(db);
  db.put({}, to_bytes("a0"), to_bytes("yyyyyyyy"));
  for (int i = 0; i < 4; ++i)
    db.put({}, to_bytes(std::format("d{}", i)), to_bytes("yyyyyyyy"));

  // File 2 is the more fragmented but has nothing vacuum may drop; file 1
  // is compacted instead.
  CHECK(db.vacuum({.fragmentation_threshold = 0.0, .retain_after = retain}));
  CHECK(stream_sequences(db, 0).front() == 2);  // a0's first write went
  check_complete_from(db, retain);
  CHECK_FALSE(db.vacuum({.fragmentation_threshold = 0.0, .retain_after = retain}));
}

TEST_CASE("vacuum with retain_after 0 drops nothing", "[vacuum][retain]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 128});
  for (int round = 0; round < 3; ++round)
    for (int i = 0; i < 5; ++i)
      db.put({}, to_bytes(std::format("k{}", i)), to_bytes("v"));
  CHECK_FALSE(db.vacuum({.fragmentation_threshold = 0.0, .retain_after = 0}));
  check_complete_from(db, 0);
  CHECK(bytecask::VacuumOptions{}.retain_after == bytecask::kNoRetention);
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

// A sync=true plan that writes nothing makes every earlier write durable:
// how a caller writing with sync=false bounds what an OS crash can lose.
TEST_CASE("apply_batch: an empty sync plan makes earlier unsynced writes "
          "durable with one fdatasync", "[apply_batch][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto w1 = db.put({.sync = false}, to_bytes("k1"), to_bytes("v1"));
  const auto w2 = db.put({.sync = false}, to_bytes("k2"), to_bytes("v2"));
  REQUIRE_FALSE(w2.durable);
  REQUIRE(db.durable_sequence() < w2.sequence);
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  const auto r = db.apply_batch({.sync = true}, bytecask::WritePlan{});

  REQUIRE(r.has_value());
  CHECK(r->durable);
  CHECK(r->sequence == 0);  // it wrote nothing itself
  CHECK(db.durable_sequence() >= w2.sequence);
  CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 1);
  CHECK(w1.sequence < w2.sequence);
  const auto s = db.engine_state();
  CHECK(s->durable_seq >= s->sync_requested_seq);

  SECTION("a second one finds nothing to sync") {
    const auto again = db.apply_batch({.sync = true}, bytecask::WritePlan{});
    REQUIRE(again.has_value());
    CHECK(again->durable);
    CHECK(again->sequence == 0);
    CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 1);
  }
}

TEST_CASE("apply_batch: an empty plan without sync is still a no-op",
          "[apply_batch][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto w = db.put({.sync = false}, to_bytes("k"), to_bytes("v"));
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  const auto r = db.apply_batch({.sync = false}, bytecask::WritePlan{});

  REQUIRE(r.has_value());
  CHECK(r->sequence == 0);
  CHECK(r->durable);
  CHECK(db.durable_sequence() < w.sequence);
  CHECK(db.stats().at("bytecask.fsyncs") == fsyncs_before);
}

TEST_CASE("apply_batch: an empty sync plan on a fresh DB does not sync",
          "[apply_batch][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  const auto r = db.apply_batch({.sync = true}, bytecask::WritePlan{});

  REQUIRE(r.has_value());
  CHECK(r->sequence == 0);
  CHECK(r->durable);
  CHECK(db.stats().at("bytecask.fsyncs") == fsyncs_before);
}

TEST_CASE("apply_batch: a guard-only sync plan makes earlier unsynced writes "
          "durable", "[apply_batch][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto w = db.put({.sync = false}, to_bytes("k"), to_bytes("v"));

  bytecask::WritePlan plan;
  plan.ensure_present(to_bytes("k"));
  const auto r = db.apply_batch({.sync = true}, std::move(plan));

  REQUIRE(r.has_value());
  CHECK(r->durable);
  CHECK(db.durable_sequence() >= w.sequence);
}

TEST_CASE("apply_batch: an empty sync plan whose fdatasync fails degrades "
          "the engine", "[apply_batch][sync][degraded]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto w = db.put({.sync = false}, to_bytes("k"), to_bytes("v"));
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.apply_batch({.sync = true}, bytecask::WritePlan{}),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());
  CHECK(db.durable_sequence() < w.sequence);
  REQUIRE_THROWS_AS(db.apply_batch({.sync = true}, bytecask::WritePlan{}),
                    bytecask::DbDegraded);
  // The unsynced write was published before the failure and stays readable.
  CHECK(db.contains_key({}, to_bytes("k")));
}

// ---------------------------------------------------------------------------
// Snapshot::count_keys
// ---------------------------------------------------------------------------

namespace {

constexpr auto kNoLimit = std::numeric_limits<std::size_t>::max();

// Keys of a sorted reference set in [from, to), no further than `limit`.
// The keys are ASCII, so std::string order is byte order.
auto reference_count(const std::set<std::string> &keys, const std::string &from,
                     const std::string &to, std::size_t limit) -> std::size_t {
  if (!(from < to))
    return 0;
  const auto n = std::distance(keys.lower_bound(from), keys.lower_bound(to));
  return std::min(static_cast<std::size_t>(n), limit);
}

// Writes every key in one batch. A debug build walks the whole key directory
// on each publish to check its invariants, so a put per key makes loading
// quadratic: 40,000 puts took most of an hour under a sanitizer.
void put_all(bytecask::DB &db, const std::ranges::input_range auto &keys,
             std::string_view value) {
  bytecask::WritePlan plan;
  for (const auto &k : keys)
    plan.put(to_bytes(k), to_bytes(value));
  REQUIRE(db.apply_batch({.sync = false}, std::move(plan)));
}

// Structured keys (shared prefixes, as index encodings have) and random ones
// of varying length, so leaves split on every kind of crit bit.
auto count_test_keys(std::size_t n) -> std::set<std::string> {
  std::mt19937_64 rng{42};
  std::set<std::string> keys;
  while (keys.size() < n) {
    if (rng() % 2 == 0) {
      keys.insert(std::format("t{:02d}:{:07d}", rng() % 7, rng() % 1000000));
    } else {
      std::string k = "r";
      const auto len = 1 + rng() % 24;
      for (std::size_t i = 0; i < len; ++i)
        k.push_back(static_cast<char>('0' + rng() % 75));
      keys.insert(std::move(k));
    }
  }
  return keys;
}

// A bound near the key set: an existing key, one just past it, a random
// string between keys, or one of the two ends.
auto count_test_bound(const std::vector<std::string> &sorted,
                      std::mt19937_64 &rng) -> std::string {
  const auto &k = sorted[rng() % sorted.size()];
  switch (rng() % 5) {
  case 0: return k;
  case 1: return k + '\x01';
  case 2: return k.substr(0, 1 + rng() % k.size());
  case 3: return "";
  default: return "~~~~";
  }
}

}  // namespace

TEST_CASE("count_keys matches the keys in the range, at every limit",
          "[count_keys]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto keys = count_test_keys(40'000);
  put_all(db, keys, "v");
  const std::vector<std::string> sorted(keys.begin(), keys.end());
  const auto snap = db.snapshot();

  REQUIRE(snap.count_keys(to_bytes(""), to_bytes("~~~~"), kNoLimit) ==
          keys.size());

  std::mt19937_64 rng{7};
  for (int i = 0; i < 3'000; ++i) {
    const auto from = count_test_bound(sorted, rng);
    const auto to = count_test_bound(sorted, rng);
    // An empty or swapped range is refused, at every limit.
    if (!(from < to)) {
      CHECK_THROWS_AS(snap.count_keys(to_bytes(from), to_bytes(to), 0),
                      std::invalid_argument);
      CHECK_THROWS_AS(snap.count_keys(to_bytes(from), to_bytes(to), kNoLimit),
                      std::invalid_argument);
      continue;
    }
    for (const std::size_t limit :
         {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{100},
          std::size_t{1024}, kNoLimit}) {
      const auto got = snap.count_keys(to_bytes(from), to_bytes(to), limit);
      const auto want = reference_count(keys, from, to, limit);
      if (got != want)
        FAIL(std::format("[{}, {}) limit {}: got {}, want {}", from, to,
                         limit, got, want));
    }
  }
}

TEST_CASE("count_keys on a snapshot ignores later writes", "[count_keys]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  auto keys = count_test_keys(5'000);
  put_all(db, keys, "v");
  const auto before = keys;
  const auto old_snap = db.snapshot();

  // New keys, deletions, and a range deletion across several leaves.
  for (int i = 0; i < 500; ++i) {
    const auto k = std::format("t03:{:07d}x", i * 997);
    db.put({.sync = false}, to_bytes(k), to_bytes("v"));
    keys.insert(k);
  }
  int erased = 0;
  for (auto it = keys.begin(); it != keys.end() && erased < 300; ++erased) {
    REQUIRE(db.del({.sync = false}, to_bytes(*it)).has_value());
    it = keys.erase(it);
    std::advance(it, std::min<std::ptrdiff_t>(3, std::distance(it, keys.end())));
  }
  db.del_range({.sync = false}, to_bytes("t05:"), to_bytes("t06:"));
  keys.erase(keys.lower_bound("t05:"), keys.lower_bound("t06:"));

  const auto new_snap = db.snapshot();
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"", "~~~~"}, {"t03:", "t04:"}, {"t05:", "t06:"}, {"r", "s"},
           {"t00:0500000", "t02:"}}) {
    CHECK(old_snap.count_keys(to_bytes(from), to_bytes(to), kNoLimit) ==
          reference_count(before, from, to, kNoLimit));
    CHECK(new_snap.count_keys(to_bytes(from), to_bytes(to), kNoLimit) ==
          reference_count(keys, from, to, kNoLimit));
  }
}

TEST_CASE("count_keys reads no key per counted entry", "[count_keys]") {
  // Buffer-pool hits count the key directory's record reads. One key read
  // can take more than one hit (header, then the rest), so the budget is
  // measured, not assumed: a contains_key of a present key is one key read.
  TempDir td;
  auto db = bytecask::DB::open(
      td.path / "db", {.io_backend = bytecask::IoBackend::BufferPool,
                       .buffer_pool = {.capacity_bytes = 256 * 1024 * 1024}});
  put_all(db,
          std::views::iota(0, 20'000) | std::views::transform([](int i) {
            return std::format("k{:06d}", i);
          }),
          "value");
  const auto snap = db.snapshot();
  const auto hits = [&] { return db.stats().at("bytecask.pool_hits"); };

  auto h0 = hits();
  REQUIRE(snap.contains_key({}, to_bytes("k010000")));
  const auto one_key_read = hits() - h0;

  h0 = hits();
  REQUIRE(snap.count_keys(to_bytes("k000100"), to_bytes("k019000"), kNoLimit) ==
          18'900);
  const auto count_cost = hits() - h0;
  if constexpr (bytecask::kKeyDirReadsKeys)
    CHECK(count_cost <= 2 * one_key_read);
  else
    CHECK(count_cost == 0);

  // Small ranges cost no more than the keys_from walk they replace: step
  // from `from` until a key at or past `to`.
  const auto walk = [&](const std::string &from, const std::string &to) {
    std::size_t n = 0;
    for (const auto &k : snap.keys_from({}, to_bytes(from))) {
      if (!std::ranges::lexicographical_compare(k, to_bytes(to)))
        break;
      ++n;
    }
    return n;
  };
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"k005000x", "k005001"},    // empty
           {"k005000", "k005001"},     // one key
           {"k005000", "k005002"}}) {  // two keys
    h0 = hits();
    const auto walked = walk(from, to);
    const auto walk_cost = hits() - h0;
    h0 = hits();
    CHECK(snap.count_keys(to_bytes(from), to_bytes(to), kNoLimit) == walked);
    CHECK(hits() - h0 <= walk_cost);
  }
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

  db.test_write_group().on_batch_start_ = [&] {
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

  db.test_write_group().on_batch_start_ = nullptr;

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

  db.test_write_group().on_batch_start_ = [&] {
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

  db.test_write_group().on_batch_start_ = nullptr;

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

// ---------------------------------------------------------------------------
// Directory sync (#199). fdatasync makes a file's bytes durable, not the
// directory entry that names it, so every create or rename that something
// later depends on is followed by a sync of its directory. Each site passes
// its own fault injection checkpoint to sync_directory, so failing a site's
// sync shows the site still calls it, and that nothing it guards goes ahead.
// ---------------------------------------------------------------------------

TEST_CASE("directory sync: a failed sync at rotation degrades before the new "
          "file takes a write",
          "[dir_sync][degraded][resume]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    // As in "resume() recovers from degraded state": k2 crosses the 30-byte
    // threshold, and the rotation after its commit creates the next file.
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 30});
    db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
    const auto files_before = data_files_in(db_path);
    {
      bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_rotate"};
      REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                        std::system_error);
    }
    // The file was created, its name was not confirmed durable, and the
    // engine refuses writes instead of acknowledging one into it.
    CHECK(data_files_in(db_path).size() == files_before.size() + 1);
    REQUIRE(db.is_degraded());
    CHECK_THROWS_AS(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")),
                    bytecask::DbDegraded);

    REQUIRE_NOTHROW(db.resume());
    REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
    CHECK(collect_kv(db) == std::map<std::string, std::string>{
                                {"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}});
  }
  // The file left behind by the failed rotation is recovered like the active
  // file of a crash.
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 30});
  CHECK(collect_kv(db) == std::map<std::string, std::string>{
                              {"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}});
}

TEST_CASE("directory sync: a failed sync in resume() stays degraded",
          "[dir_sync][degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 30});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  {
    bytecask::testing::ScopedFaultInjector fi{"io_rotate_file_creation"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("k2"), to_bytes("v2")),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());
  {
    bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_resume"};
    REQUIRE_THROWS_AS(db.resume(), std::system_error);
  }
  CHECK(db.is_degraded());
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  REQUIRE_NOTHROW(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")));
  CHECK(collect_kv(db) == std::map<std::string, std::string>{
                              {"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}});
}

TEST_CASE("directory sync: open fails when the active file's entry cannot be "
          "synced",
          "[dir_sync][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path);
    db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  }
  {
    bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_open"};
    REQUIRE_THROWS_AS(bytecask::DB::open(db_path), std::system_error);
  }
  auto db = bytecask::DB::open(db_path);
  CHECK(collect_kv(db) ==
        std::map<std::string, std::string>{{"k1", "v1"}});
}

TEST_CASE("directory sync: open syncs each directory it creates",
          "[dir_sync]") {
  TempDir td;
  const auto db_path = td.path / "a" / "b" / "db";
  {
    bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_create_dir"};
    REQUIRE_THROWS_AS(bytecask::DB::open(db_path), std::system_error);
  }
  std::filesystem::remove_all(td.path / "a");
  {
    // Three directories created, three parents synced before anything else
    // open does: the third checkpoint is still one of them.
    bytecask::testing::ScopedFaultInjector fi{2};
    REQUIRE_THROWS_AS(bytecask::DB::open(db_path), std::system_error);
    CHECK(fi.inj.call_count == 3);
    CHECK(fi.inj.last_checkpoint == "io_dir_sync_create_dir");
  }
  std::filesystem::remove_all(td.path / "a");
  { auto db = bytecask::DB::open(db_path); }
  // An existing directory creates nothing, so there is nothing to sync.
  bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_create_dir"};
  REQUIRE_NOTHROW(bytecask::DB::open(db_path));
}

TEST_CASE("directory sync: a hint rebuilt at open is synced",
          "[dir_sync][recovery]") {
  TempDir td;
  const auto db_path = td.path / "db";
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 30});
    for (int i = 0; i < 6; ++i) {
      db.put({.sync = true}, to_bytes(std::format("k{}", i)), to_bytes("v"));
    }
  }
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".hint") std::filesystem::remove(e.path());
  }
  {
    bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_hint"};
    REQUIRE_THROWS_AS(bytecask::DB::open(db_path, {.max_file_bytes = 30}),
                      std::system_error);
  }
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 30});
  CHECK(collect_kv(db).size() == 6);
}

TEST_CASE("directory sync: a failed sync in vacuum keeps the source",
          "[dir_sync][vacuum]") {
  TempDir td;
  const auto db_path = td.path / "db";
  std::map<std::string, std::string> oracle;
  std::set<std::filesystem::path> before_vacuum;
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 4096});
    for (int i = 0; i < 200; ++i) {
      auto k = std::format("k{:04d}", i);
      auto v = std::format("v{:04d}", i) + std::string(40, 'x');
      db.put({.sync = false}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
    }
    // Garbage in the first file, which keeps live keys too: vacuum compacts
    // it, renaming a compacted copy into place.
    for (int i = 0; i < 40; i += 2) {
      auto k = std::format("k{:04d}", i);
      auto v = std::format("w{:04d}", i) + std::string(40, 'y');
      db.put({.sync = false}, to_bytes(k), to_bytes(v));
      oracle[k] = v;
    }
    before_vacuum = data_files_in(db_path);
    {
      bytecask::testing::ScopedFaultInjector fi{"io_dir_sync_vacuum"};
      REQUIRE_THROWS_AS(db.vacuum({.fragmentation_threshold = 0.0}),
                        std::system_error);
    }
    // The rename happened and the commit did not. Every source is still on
    // disk and published, and vacuum removed its copy (#304).
    CHECK(data_files_in(db_path) == before_vacuum);
    CHECK(collect_kv(db) == oracle);
  }
  auto db = bytecask::DB::open(db_path, {.max_file_bytes = 4096});
  CHECK(collect_kv(db) == oracle);
  CHECK(std::ranges::includes(data_files_in(db_path), before_vacuum));
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

// The truncate's new length is metadata only an fdatasync persists. The
// test fails the first data file sync resume() makes after its rewrite —
// the one after the truncate — so it fails only while that sync is made.
TEST_CASE("resume() stays degraded when the sync after its truncate fails",
          "[degraded][resume]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  db.put({.sync = true}, to_bytes("k2"), to_bytes("v2"));
  const auto offsets = degrade_with_unsynced_batch(db, dir);
  REQUIRE(offsets.size() == 6);  // k1, k2, BulkBegin, b1, b2, BulkEnd
  // A damaged entry in the unpublished batch: resume() cuts the file there.
  flip_byte(active_data_file(dir), offsets[3] + 15);

  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS(db.resume(), std::system_error);
  }
  CHECK(db.is_degraded());

  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  CHECK(get_str(db, to_bytes("k1")) == "v1");
  CHECK(get_str(db, to_bytes("k2")) == "v2");
  CHECK_FALSE(db.contains_key({}, to_bytes("b1")));
}

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

// fsyncgate (#231). Under the page cache model a failed fdatasync leaves the
// pages it covered clean and off the device, as Linux does, and
// restore_device() puts back what the device holds: a power cut, or the
// kernel evicting those pages.
namespace {

constexpr int kUnsyncedPuts = 40;

auto fsyncgate_key(int i) -> std::string { return std::format("k{}", i); }
// Long enough that the unsynced puts span several pages.
auto fsyncgate_value(int i) -> std::string { return std::format("{:0>200}", i); }

// k0 synced, then puts no sync covers.
void put_unsynced(bytecask::DB &db) {
  db.put({.sync = true}, to_bytes(fsyncgate_key(0)), to_bytes(fsyncgate_value(0)));
  for (int i = 1; i <= kUnsyncedPuts; ++i)
    db.put({.sync = false}, to_bytes(fsyncgate_key(i)),
           to_bytes(fsyncgate_value(i)));
}

void check_all_present(const bytecask::DB &db) {
  for (int i = 0; i <= kUnsyncedPuts; ++i) {
    INFO("key " << i);
    CHECK(get_str(db, to_bytes(fsyncgate_key(i))) == fsyncgate_value(i));
  }
}

auto has_hint(const std::filesystem::path &data) -> bool {
  auto hint = data;
  hint.replace_extension(".hint");
  return std::filesystem::exists(hint);
}

constexpr std::array kFsyncgateBackends{bytecask::IoBackend::Pread,
#ifndef __EMSCRIPTEN__
                                        bytecask::IoBackend::Mmap,  // refused on WASM
#endif
                                        bytecask::IoBackend::BufferPool};

auto fsyncgate_opts(bytecask::IoBackend backend) -> bytecask::Options {
  return {.max_file_bytes = 1'000'000,
          .io_backend = backend,
          .buffer_pool = {.capacity_bytes = 16 << 20}};
}

}  // namespace

TEST_CASE("resume() makes durable what it publishes after a failed fdatasync",
          "[degraded][resume][fsyncgate]") {
  for (const auto backend : kFsyncgateBackends) {
    DYNAMIC_SECTION("io_backend " << static_cast<int>(backend)) {
      bytecask::testing::ScopedPageCacheModel cache;
      TempDir td;
      const auto dir = td.path / "db";
      const auto opts = fsyncgate_opts(backend);
      {
        auto db = bytecask::DB::open(dir, opts);
        put_unsynced(db);
        {
          bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
          REQUIRE_THROWS_AS(
              db.put({.sync = true}, to_bytes("kf"), to_bytes("vf")),
              std::system_error);
        }
        REQUIRE(db.is_degraded());
        REQUIRE_NOTHROW(db.resume());
        check_all_present(db);
        CHECK(get_str(db, to_bytes("kf")) == "vf");
        // A durable write in a later file: a lost page below it is a hole
        // under the durable watermark.
        db.put({.sync = true}, to_bytes("later"), to_bytes("x"));
      }
      // The clean close synced everything else; the failed sync's pages
      // are what a rewrite has to have written.
      CHECK(cache.model.undurable_pages() == 0);
      cache.model.restore_device(dir);  // power cut

      auto db = bytecask::DB::open(dir, opts);
      check_all_present(db);
      CHECK(get_str(db, to_bytes("kf")) == "vf");
      CHECK(get_str(db, to_bytes("later")) == "x");
    }
  }
}

TEST_CASE("resume() refuses when a failed fdatasync's pages were evicted, and "
          "a reopen recovers what the device holds",
          "[degraded][resume][fsyncgate]") {
  for (const auto backend : kFsyncgateBackends) {
    DYNAMIC_SECTION("io_backend " << static_cast<int>(backend)) {
      bytecask::testing::ScopedPageCacheModel cache;
      TempDir td;
      const auto dir = td.path / "db";
      const auto opts = fsyncgate_opts(backend);
      {
        auto db = bytecask::DB::open(dir, opts);
        put_unsynced(db);
        {
          bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
          REQUIRE_THROWS_AS(
              db.put({.sync = true}, to_bytes("kf"), to_bytes("vf")),
              std::system_error);
        }
        REQUIRE(db.is_degraded());
        // Evicted: reads return what the device holds. The published
        // sync=false puts are gone, and the key directory holds no older
        // state to resume into.
        cache.model.restore_device(dir);
        REQUIRE_THROWS_AS(db.resume(), std::runtime_error);
        CHECK(db.is_degraded());
      }
      // A degraded close leaves the active file hint-less, so the reopen
      // rewrites it, scans it and truncates at the lost bytes.
      REQUIRE_FALSE(has_hint(active_data_file(dir)));
      auto db = bytecask::DB::open(dir, opts);
      CHECK(get_str(db, to_bytes(fsyncgate_key(0))) == fsyncgate_value(0));
      for (int i = 1; i <= kUnsyncedPuts; ++i) {
        INFO("key " << i);
        CHECK_FALSE(db.contains_key({}, to_bytes(fsyncgate_key(i))));
      }
      CHECK_FALSE(db.contains_key({}, to_bytes("kf")));
    }
  }
}

TEST_CASE("open makes a hint-less file durable before it indexes it: close "
          "after a failed fdatasync",
          "[recovery][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  {
    // Declared first, so it outlives db and fails the close's fdatasync.
    std::unique_ptr<bytecask::testing::ScopedFaultInjector> fail_close;
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
    put_unsynced(db);
    fail_close = std::make_unique<bytecask::testing::ScopedFaultInjector>(
        "io_data_file_sync");
  }
  // The close could not make the file durable, so it wrote no hint for it.
  REQUIRE_FALSE(has_hint(active_data_file(dir)));
  {
    auto db = bytecask::DB::open(dir);
    check_all_present(db);
    db.put({.sync = true}, to_bytes("later"), to_bytes("x"));
  }
  CHECK(cache.model.undurable_pages() == 0);
  cache.model.restore_device(dir);  // power cut

  auto db = bytecask::DB::open(dir);
  check_all_present(db);
  CHECK(get_str(db, to_bytes("later")) == "x");
}

// DB::close() (#257): the shutdown ~DB() does, with its outcome reported.
TEST_CASE("close() throws when its fdatasync fails, closes all the same, and "
          "a reopen recovers what was durable",
          "[close][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
  put_unsynced(db);
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS(db.close(), std::system_error);
  }
  CHECK_THROWS_AS(db.put({}, to_bytes("after"), to_bytes("x")),
                  bytecask::DbClosed);
  // A retry cannot make the lost pages durable, and does not pretend to.
  CHECK_NOTHROW(db.close());
  REQUIRE_FALSE(has_hint(active_data_file(dir)));

  cache.model.restore_device(dir);  // power cut
  // The lock is released: the handle is still alive, and the open succeeds.
  auto reopened = bytecask::DB::open(dir);
  CHECK(get_str(reopened, to_bytes(fsyncgate_key(0))) == fsyncgate_value(0));
  for (int i = 1; i <= kUnsyncedPuts; ++i) {
    INFO("key " << i);
    CHECK_FALSE(reopened.contains_key({}, to_bytes(fsyncgate_key(i))));
  }
}

TEST_CASE("close() makes unsynced writes durable and writes the active "
          "file's hint",
          "[close][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
  put_unsynced(db);
  const auto last = db.durable_sequence();
  db.close();
  CHECK(has_hint(active_data_file(dir)));
  CHECK(cache.model.undurable_pages() == 0);
  cache.model.restore_device(dir);  // power cut

  auto reopened = bytecask::DB::open(dir);
  check_all_present(reopened);
  CHECK(reopened.durable_sequence() > last);
}

// The commit promise itself: a sync write that returned is on the device,
// with nothing after it — no close, no later write — to sync it.
TEST_CASE("a sync write survives a power cut as soon as it returns",
          "[fsyncgate][durable_seq]") {
  for (const auto backend : kFsyncgateBackends) {
    for (const bool solo : {false, true}) {
      DYNAMIC_SECTION("io_backend " << static_cast<int>(backend)
                                    << (solo ? ", solo" : ", group commit")) {
        bytecask::testing::ScopedPageCacheModel cache;
        TempDir td;
        const auto dir = td.path / "db";
        const auto after_cut = td.path / "after_cut";
        const auto opts = fsyncgate_opts(backend);
        {
          auto db = bytecask::DB::open(dir, opts);
          const auto r = db.put({.sync = true, .solo = solo},
                                to_bytes(fsyncgate_key(0)),
                                to_bytes(fsyncgate_value(0)));
          CHECK(r.durable);
          CHECK(db.durable_sequence() >= r.sequence);
          cache.model.copy_device(dir, after_cut);  // power cut
        }
        auto db = bytecask::DB::open(after_cut, opts);
        CHECK(get_str(db, to_bytes(fsyncgate_key(0))) == fsyncgate_value(0));
      }
    }
  }
}

// #281: create_manifest's sync is the active file's like any other. Were the
// engine left healthy after it failed, the next sync write's fdatasync would
// find the failed pages clean, return 0, and move durable_sequence past
// writes the device does not hold.
TEST_CASE("create_manifest whose fdatasync fails degrades, and a power cut "
          "keeps everything below durable_sequence",
          "[manifest][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  const auto after_cut = td.path / "after_cut";
  std::uint64_t durable = 0;
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
    put_unsynced(db);
    const auto synced = db.durable_sequence();
    {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      CHECK_THROWS_AS(db.create_manifest(), std::system_error);
    }
    CHECK(db.is_degraded());
    CHECK(db.durable_sequence() == synced);
    // Healthy, this sync would succeed without writing the failed pages.
    CHECK_THROWS_AS(db.put({.sync = true}, to_bytes("later"), to_bytes("x")),
                    bytecask::DbDegraded);
    durable = db.durable_sequence();
    cache.model.copy_device(dir, after_cut);  // power cut
  }
  // k{i} was written at sequence i + 1.
  auto db = bytecask::DB::open(after_cut);
  for (int i = 0; i <= kUnsyncedPuts; ++i) {
    if (static_cast<std::uint64_t>(i) + 1 > durable) break;
    INFO("key " << i << ", durable_sequence " << durable);
    CHECK(get_str(db, to_bytes(fsyncgate_key(i))) == fsyncgate_value(i));
  }
}

TEST_CASE("create_manifest after resume() from its failed fdatasync makes "
          "every write durable",
          "[manifest][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  const auto after_cut = td.path / "after_cut";
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
    put_unsynced(db);
    {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      CHECK_THROWS_AS(db.create_manifest(), std::system_error);
    }
    REQUIRE_NOTHROW(db.resume());
    const auto manifest = db.create_manifest();
    CHECK(manifest.through_sequence == static_cast<std::uint64_t>(kUnsyncedPuts + 1));
    cache.model.copy_device(dir, after_cut);  // power cut
  }
  auto db = bytecask::DB::open(after_cut);
  check_all_present(db);
}

// #245: vacuum judges a record dead by the key directory, which holds
// sync=false writes no fdatasync has covered yet. Dropping the record is
// durable once vacuum commits, so it must not go before what superseded it.
// The power is cut with the DB still open — its close would sync — so the
// directory is copied as the device holds it (copy_device) before ~DB runs.
TEST_CASE("vacuum does not drop a durable record that only an unsynced write "
          "supersedes",
          "[vacuum][fsyncgate]") {
  // Compaction keeps k0; with both keys overwritten, the file goes whole.
  for (const bool whole_file : {false, true}) {
    DYNAMIC_SECTION((whole_file ? "file removed" : "file compacted")) {
      bytecask::testing::ScopedPageCacheModel cache;
      TempDir td;
      const auto dir = td.path / "db";
      const auto after_cut = td.path / "after_cut";
      // Two 25-byte entries fill the first file and seal it. The 23-byte
      // overwrites stay below the limit, so no rotation syncs them.
      const bytecask::Options opts{.max_file_bytes = 50};
      {
        auto db = bytecask::DB::open(dir, opts);
        db.put({.sync = true}, to_bytes("k0"), to_bytes("v_k0"));
        db.put({.sync = true}, to_bytes("k1"), to_bytes("v_k1"));
        if (whole_file)
          db.put({.sync = false}, to_bytes("k0"), to_bytes("n0"));
        const auto last = db.put({.sync = false}, to_bytes("k1"), to_bytes("n1"));
        REQUIRE(db.durable_sequence() < last.sequence);
        REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));
        cache.model.copy_device(dir, after_cut);  // power cut
      }
      auto db = bytecask::DB::open(after_cut, opts);
      const auto k0 = get_str(db, to_bytes("k0"));
      const auto k1 = get_str(db, to_bytes("k1"));
      CHECK((k0 == "v_k0" || (whole_file && k0 == "n0")));
      CHECK((k1 == "v_k1" || k1 == "n1"));
    }
  }
}

TEST_CASE("vacuum whose fdatasync fails degrades, keeps the source file, and "
          "the durable values survive a power cut",
          "[vacuum][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  const auto after_cut = td.path / "after_cut";
  const bytecask::Options opts{.max_file_bytes = 50};  // as above
  {
    auto db = bytecask::DB::open(dir, opts);
    db.put({.sync = true}, to_bytes("k0"), to_bytes("v_k0"));
    db.put({.sync = true}, to_bytes("k1"), to_bytes("v_k1"));
    db.put({.sync = false}, to_bytes("k0"), to_bytes("n0"));
    const auto last = db.put({.sync = false}, to_bytes("k1"), to_bytes("n1"));
    REQUIRE(db.durable_sequence() < last.sequence);
    auto data_files = [&] {
      std::set<std::filesystem::path> out;
      for (const auto &e : std::filesystem::directory_iterator{dir})
        if (e.path().extension() == ".data") out.insert(e.path());
      return out;
    };
    const auto files_before = data_files();
    {
      // The whole-file path writes no staging copy: the first data file
      // sync is the one that makes the overwrites durable.
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      REQUIRE_THROWS_AS(db.vacuum({.fragmentation_threshold = 0.0}),
                        std::system_error);
    }
    CHECK(db.is_degraded());
    CHECK(data_files() == files_before);
    cache.model.copy_device(dir, after_cut);  // power cut
  }
  auto db = bytecask::DB::open(after_cut, opts);
  CHECK(get_str(db, to_bytes("k0")) == "v_k0");
  CHECK(get_str(db, to_bytes("k1")) == "v_k1");
}

TEST_CASE("after close() every operation throws DbClosed; snapshots taken "
          "before stay readable",
          "[close]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir);
  db.put({}, to_bytes("k"), to_bytes("v"));
  auto snap = db.snapshot();
  auto it = db.iter_from({}, to_bytes("k")).begin();
  db.close();
  db.close();  // idempotent

  bytecask::Bytes out;
  CHECK_THROWS_AS((void)db.get({}, to_bytes("k"), out), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.contains_key({}, to_bytes("k")), bytecask::DbClosed);
  CHECK_THROWS_AS(db.put({}, to_bytes("k"), to_bytes("v")), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.del({}, to_bytes("k")), bytecask::DbClosed);
  CHECK_THROWS_AS(db.del_range({}, to_bytes("a"), to_bytes("z")),
                  bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.apply_batch({}, bytecask::WritePlan{}),
                  bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.snapshot(), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.iter_from({}), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.keys_from({}), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.riter_from({}), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.rkeys_from({}), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.vacuum(), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.durable_sequence(), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.create_manifest(), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.changes_since(snap, 0), bytecask::DbClosed);
  CHECK_THROWS_AS(db.set_mode(bytecask::Mode::Follower), bytecask::DbClosed);
  CHECK_THROWS_AS(db.resume(), bytecask::DbClosed);
  CHECK_THROWS_AS((void)db.stats(), bytecask::DbClosed);
  CHECK_THROWS_AS(db.ingest({}), bytecask::DbClosed);
  CHECK(db.mode() == bytecask::Mode::Leader);
  CHECK_FALSE(db.is_degraded());

  CHECK(snap.get({}, to_bytes("k"), out));
  CHECK(to_string(out) == "v");
  REQUIRE(it != std::default_sentinel);
  CHECK(to_string((*it).value) == "v");

  auto reopened = bytecask::DB::open(dir);
  CHECK(get_str(reopened, to_bytes("k")) == "v");
}

TEST_CASE("close() on a degraded engine reports acknowledged writes that are "
          "not durable",
          "[close][degraded][fsyncgate]") {
  bytecask::testing::ScopedPageCacheModel cache;
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
  put_unsynced(db);
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    REQUIRE_THROWS_AS(db.put({.sync = true}, to_bytes("kf"), to_bytes("vf")),
                      std::system_error);
  }
  REQUIRE(db.is_degraded());
  CHECK_THROWS_AS(db.close(), bytecask::DbDegraded);
  CHECK(db.is_degraded());
  CHECK_THROWS_AS((void)db.stats(), bytecask::DbClosed);
  CHECK_NOTHROW(db.close());
}

TEST_CASE("close() on a degraded engine whose acknowledged writes are all "
          "durable returns normally",
          "[close][degraded]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  // The failed batch was never acknowledged, so nothing acknowledged is lost.
  (void)degrade_with_unsynced_batch(db, dir);
  CHECK_NOTHROW(db.close());

  auto reopened = bytecask::DB::open(dir);
  // The failed batch may be there or not: it was never acknowledged.
  CHECK(get_str(reopened, to_bytes("k1")) == "v1");
}

TEST_CASE("writers and readers racing close() either complete or throw "
          "DbClosed, and every acknowledged write survives",
          "[close][concurrency]") {
  TempDir td;
  const auto dir = td.path / "db";
  constexpr int kWriters = 4;
  constexpr int kReaders = 2;
  std::array<std::vector<std::string>, kWriters> acked;
  std::atomic<int> started{0};
  std::atomic<bool> unexpected{false};
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 64 * 1024});
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
      threads.emplace_back([&, w] {
        try {
          for (int i = 0;; ++i) {
            const auto key = std::format("w{}:{}", w, i);
            (void)db.put({.sync = (i % 3 == 0)}, to_bytes(key),
                         to_bytes(key));
            acked[static_cast<std::size_t>(w)].push_back(key);
            if (i == 20) started.fetch_add(1);
          }
        } catch (const bytecask::DbClosed &) {
        } catch (...) {
          unexpected = true;
        }
      });
    }
    for (int r = 0; r < kReaders; ++r) {
      threads.emplace_back([&] {
        try {
          bytecask::Bytes out;
          for (;;) (void)db.get({}, to_bytes("w0:0"), out);
        } catch (const bytecask::DbClosed &) {
        } catch (...) {
          unexpected = true;
        }
      });
    }
    while (started.load() < kWriters) std::this_thread::yield();
    db.close();
    for (auto &t : threads) t.join();
  }
  CHECK_FALSE(unexpected);
  auto db = bytecask::DB::open(dir);
  for (const auto &keys : acked) {
    CHECK(keys.size() > 20);
    for (const auto &key : keys) {
      INFO(key);
      CHECK(get_str(db, to_bytes(key)) == key);
    }
  }
}

#ifndef __EMSCRIPTEN__
TEST_CASE("open makes a hint-less file durable before it indexes it: a "
          "process killed before its sync",
          "[recovery][fsyncgate]") {
  TempDir td;
  const auto dir = td.path / "db";
  // The child appends with sync=false and dies without closing; the page
  // cache keeps what it wrote.
  const auto pid = ::fork();
  REQUIRE(pid != -1);
  if (pid == 0) {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
    for (int i = 0; i <= kUnsyncedPuts; ++i)
      db.put({.sync = false}, to_bytes(fsyncgate_key(i)),
             to_bytes(fsyncgate_value(i)));
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  bytecask::testing::ScopedPageCacheModel cache;
  const auto killed = active_data_file(dir);
  REQUIRE_FALSE(has_hint(killed));
  cache.model.mark_unsynced(killed);
  {
    auto db = bytecask::DB::open(dir);
    check_all_present(db);
    db.put({.sync = true}, to_bytes("later"), to_bytes("x"));
  }
  CHECK(cache.model.undurable_pages() == 0);
  cache.model.restore_device(dir);  // power cut

  auto db = bytecask::DB::open(dir);
  check_all_present(db);
  CHECK(get_str(db, to_bytes("later")) == "x");
}
#endif

namespace {

auto read_file(const std::filesystem::path &p) -> std::vector<char> {
  std::ifstream f{p, std::ios::binary};
  return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("resume() stays degraded when its rewrite's fdatasync fails, and "
          "leaves the file as it was",
          "[degraded][resume][fsyncgate]") {
  TempDir td;
  const auto dir = td.path / "db";
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
  put_unsynced(db);
  (void)degrade_with_unsynced_batch(db, dir);
  const auto active = active_data_file(dir);
  const auto before = read_file(active);
  {
    bytecask::testing::ScopedFaultInjector fi{"io_rewrite_sync"};
    REQUIRE_THROWS_AS(db.resume(), std::system_error);
  }
  CHECK(db.is_degraded());
  CHECK(read_file(active) == before);

  REQUIRE_NOTHROW(db.resume());
  check_all_present(db);
  CHECK(get_str(db, to_bytes("b1")) == "bv1");
}

TEST_CASE("open fails when the rewrite of a hint-less file cannot be synced, "
          "and writes no hint for it",
          "[recovery][fsyncgate]") {
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
    put_unsynced(db);
  }
  // A crash's shape: the active file has no hint.
  const auto active = active_data_file(dir);
  auto hint = active;
  hint.replace_extension(".hint");
  REQUIRE(std::filesystem::remove(hint));
  {
    bytecask::testing::ScopedFaultInjector fi{"io_rewrite_sync"};
    REQUIRE_THROWS_AS(bytecask::DB::open(dir), std::system_error);
  }
  CHECK_FALSE(has_hint(active));

  auto db = bytecask::DB::open(dir);
  check_all_present(db);
}

TEST_CASE("a clean close writes the active file's hint",
          "[recovery][fsyncgate]") {
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1'000'000});
    put_unsynced(db);
  }
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() != ".data") continue;
    INFO(e.path().string());
    CHECK(has_hint(e.path()));
  }
  auto db = bytecask::DB::open(dir);
  check_all_present(db);
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

// The validate_preconditions and apply_resume unit tests build an
// EngineState by hand, with key directory entries that point at no data file.
// A blind key directory keeps no keys: it reads them back from the data
// files, so these states cannot be built for it. Both code paths run in the
// blind build through the DB-level conflict and resume tests.
#ifndef BYTECASK_KEYDIR_BLIND
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

#endif // !BYTECASK_KEYDIR_BLIND
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

TEST_CASE("Ranges: from >= to is refused, before anything is written",
          "[bytecask][del_range][limits][edges]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));
  db.put({}, to_bytes("b"), to_bytes("2"));
  const auto bytes = [&] { return db.stats().at("bytecask.bytes_written"); };
  const auto before = bytes();
  const auto seq_before = db.durable_sequence();

  // Equal bounds hold no key; swapped ones are a caller's bug. Both throw,
  // the empty key included, and the range check comes before anything else.
  for (const auto &[from, to] : {std::pair{"a", "a"}, std::pair{"z", "a"},
                                 std::pair{"", ""}, std::pair{"a", ""}}) {
    INFO("[" << from << ", " << to << ")");
    CHECK_THROWS_AS(db.del_range({}, to_bytes(from), to_bytes(to)),
                    std::invalid_argument);
    bytecask::WritePlan plan;
    CHECK_THROWS_AS(plan.del_range(to_bytes(from), to_bytes(to)),
                    std::invalid_argument);
    bytecask::WritePlan guarded{db.snapshot()};
    CHECK_THROWS_AS(
        guarded.ensure_range_unchanged(to_bytes(from), to_bytes(to)),
        std::invalid_argument);
    CHECK_THROWS_AS(db.snapshot().count_keys(to_bytes(from), to_bytes(to), 10),
                    std::invalid_argument);
    CHECK_THROWS_AS(db.snapshot().count_keys(to_bytes(from), to_bytes(to), 0),
                    std::invalid_argument);
  }

  // A refused range leaves its plan as it was: the rest still commits.
  bytecask::WritePlan plan;
  plan.put(to_bytes("c"), to_bytes("3"));
  CHECK_THROWS_AS(plan.del_range(to_bytes("z"), to_bytes("a")),
                  std::invalid_argument);
  const auto r = db.apply_batch({}, std::move(plan));
  REQUIRE(r.has_value());
  CHECK(bytes() == before + static_cast<std::int64_t>(
                                bytecask::entry_size(1, 1)));
  CHECK(r->sequence == seq_before + 1);
  CHECK(collect_kv(db) == std::map<std::string, std::string>{
                              {"a", "1"}, {"b", "2"}, {"c", "3"}});

  // The smallest valid range: [k, k + "\0") holds exactly k.
  const std::string k0{"a\0", 2};
  CHECK(db.snapshot().count_keys(to_bytes("a"), to_bytes(k0), 10) == 1);
  db.del_range({}, to_bytes("a"), to_bytes(k0));
  CHECK_FALSE(db.contains_key({}, to_bytes("a")));
  CHECK(db.contains_key({}, to_bytes("b")));
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


  int data_file_count = 0;
  for (const auto &e : std::filesystem::directory_iterator{db_path}) {
    if (e.path().extension() == ".data")
      ++data_file_count;
  }
  REQUIRE(data_file_count > 1);

  std::vector<FileStatsTuple> serial_stats_vals;
  {
    const auto serial_path = td.path / "serial_baseline";
    std::filesystem::copy(db_path, serial_path,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(serial_path, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial_baseline", collect(db));
    serial_stats_vals = collect_file_stats(db);
  }

  SECTION("serial recovery") {
    const auto p = td.path / "s1";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 1});
    verify("serial", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (2 workers)") {
    const auto p = td.path / "p2";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1, .recovery_threads = 2});
    verify("parallel/2", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

  SECTION("parallel recovery (W = file count)") {
    const auto p = td.path / "pmax";
    std::filesystem::copy(db_path, p,
                          std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(
        p, {.max_file_bytes = 1,
            .recovery_threads = static_cast<unsigned>(data_file_count)});
    verify("parallel/max", collect(db));
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }

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
    CHECK(collect_file_stats(db) == serial_stats_vals);
  }
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

TEST_CASE("durable_sequence with no timeout returns an unreached target at once",
          "[durable_seq]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto start = std::chrono::steady_clock::now();
  CHECK(db.durable_sequence(1, std::chrono::milliseconds{0}) == 0);
  CHECK(std::chrono::steady_clock::now() - start <
        std::chrono::milliseconds{2000});
}

TEST_CASE("durable_sequence wakes a waiter when the DB closes",
          "[durable_seq][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  std::latch parked{1};
  std::atomic<bool> signalled{false};
  db.test_in_sequence_wait_ = [&] {
    if (!signalled.exchange(true)) parked.count_down();
  };
  std::exception_ptr err;
  std::thread waiter{[&] {
    try {
      (void)db.durable_sequence(1, std::chrono::seconds{30});
    } catch (...) {
      err = std::current_exception();
    }
  }};
  parked.wait();  // the waiter has checked its condition under the mutex
  db.close();
  waiter.join();
  REQUIRE(err);
  CHECK_THROWS_AS(std::rethrow_exception(err), bytecask::DbClosed);
}

TEST_CASE("a conflict waiting on a write whose flush fails returns when the "
          "engine degrades",
          "[pipeline][degraded][concurrency]") {
  // B's plan loses to A's write, which is in the head but not published; B
  // waits for A's publication (wait_published). A's fdatasync then fails,
  // so A's write is never published and only the degrade can release B.
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("v0"));
  auto snap = db.snapshot();

  std::latch in_flush{1};
  std::latch release{1};
  db.test_before_flush_sync_ = [&] {
    in_flush.count_down();
    release.wait();
  };
  std::exception_ptr ea;
  std::thread a{[&] {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    try {
      db.put({.sync = true}, to_bytes("k"), to_bytes("v1"));
    } catch (...) {
      ea = std::current_exception();
    }
  }};
  in_flush.wait();
  std::atomic<bool> released{false};
  db.test_in_sequence_wait_ = [&] {
    if (!released.exchange(true)) release.count_down();
  };

  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("k"), to_bytes("v2"));
  const auto r = db.apply_batch({.sync = false}, std::move(plan));
  a.join();
  db.test_before_flush_sync_ = nullptr;
  db.test_in_sequence_wait_ = nullptr;

  CHECK_FALSE(r.has_value());
  CHECK(released.load());  // B did wait
  REQUIRE(ea);
  CHECK(db.is_degraded());
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

  db.test_write_group().on_batch_start_ = [&] {
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

  db.test_write_group().on_batch_start_ = nullptr;

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
  plan.put(to_bytes("c"), to_bytes("new_c"));
  (void)db.apply_batch({}, std::move(plan));
  // A later write makes one of the batch's entries dead, so the batch's file
  // is compacted: a file holding only live entries never is, and would keep
  // its markers whatever compaction does with them.
  db.put({}, to_bytes("c"), to_bytes("newer_c"));

  // Vacuum — the batch file keeps live entries, so its markers must be kept.
  for (int i = 0; i < 10 && db.vacuum({.fragmentation_threshold = 0.0}); ++i) {}

  // Verify data is correct.
  auto va = get_val(db, to_bytes("a"));
  auto vb = get_val(db, to_bytes("b"));
  auto vc = get_val(db, to_bytes("c"));
  REQUIRE(va.has_value());
  REQUIRE(vb.has_value());
  REQUIRE(vc.has_value());
  CHECK(to_string(*va) == "new_a");
  CHECK(to_string(*vb) == "new_b");
  CHECK(to_string(*vc) == "newer_c");
  CHECK(db.stats().at("bytecask.vacuum_bytes_reclaimed") > 0);

  // Scan vacuumed files for BulkBegin/BulkEnd markers.
  bool found_begin = false;
  bool found_end = false;
  for (const auto &entry : std::filesystem::directory_iterator{db_path}) {
    if (entry.path().extension() != ".data") continue;
    auto df_ptr = bytecask::openDataFileForRead(entry.path()); auto &df = *df_ptr;
    for (const auto &[de, off] :
         std::ranges::subrange{bytecask::DataFileIterator{df}, std::default_sentinel}) {
      if (de.entry_type == bytecask::EntryType::BulkBegin) found_begin = true;
      if (de.entry_type == bytecask::EntryType::BulkEnd) found_end = true;
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
    for (const auto &[de, off] :
         std::ranges::subrange{bytecask::DataFileIterator{df}, std::default_sentinel}) {
      if (de.entry_type == bytecask::EntryType::BulkBegin ||
          de.entry_type == bytecask::EntryType::BulkEnd) {
        found_marker = true;
      }
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

// A batch's end marker carries the highest sequence of the batch. A restart
// rebuilds next_seq from the hint files, so they must keep the markers, or
// the next write reuses the marker's sequence.
TEST_CASE("a write after a restart does not reuse a batch marker's sequence",
          "[file_stats_seq][recovery]") {
  TempDir td;
  std::uint64_t batch_end = 0;
  {
    auto db = bytecask::DB::open(td.path);
    bytecask::WritePlan plan;
    plan.put(to_bytes("k1"), to_bytes("v1"));
    plan.put(to_bytes("k2"), to_bytes("v2"));
    const auto r = db.apply_batch({.sync = true}, std::move(plan));
    REQUIRE(r.has_value());
    batch_end = r->sequence;  // the BulkEnd's
  }
  auto db = bytecask::DB::open(td.path);
  CHECK(db.durable_sequence() == batch_end);
  CHECK(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")).sequence >
        batch_end);
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
  std::vector<FileStatsTuple> pre_vals;
  for (const auto &[fid, fs] : pre_close_stats) {
    if (fs.min_sequence == 0 && fs.max_sequence == 0) continue;
    pre_vals.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                          fs.max_sequence, fs.tombstone_bytes, fs.marker_bytes);
  }
  std::ranges::sort(pre_vals);

  std::vector<FileStatsTuple> post_vals;
  for (const auto &[fid, fs] : post_open_stats) {
    if (fs.min_sequence == 0 && fs.max_sequence == 0) continue;
    post_vals.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                           fs.max_sequence, fs.tombstone_bytes, fs.marker_bytes);
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

TEST_CASE("iterators advance by post-increment as by pre-increment",
          "[iterator]") {
  // The forms the standard iterator concepts require; nothing in the engine
  // calls them.
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  for (const auto *k : {"a", "b", "c"}) db.put({}, to_bytes(k), to_bytes(k));

  auto keys = db.keys_from({});
  auto k = keys.begin();
  const auto was = k++;
  CHECK(to_string(*was) == "a");
  CHECK(to_string(*k) == "b");
  const auto then = k--;
  CHECK(to_string(*then) == "b");
  CHECK(to_string(*k) == "a");

  auto entries = db.iter_from({});
  auto e = entries.begin();
  e++;
  CHECK(to_string((*e).key) == "b");

  auto reversed = db.riter_from({});
  auto r = reversed.begin();
  r++;
  CHECK(to_string((*r).key) == "b");

  auto snap = db.snapshot();
  auto changes = db.changes_since(snap, 0);
  auto c = changes.begin();
  c++;
  CHECK(to_string((*c).key) == "b");
  bytecask::ChangeIterator moved;
  moved = std::move(c);
  CHECK(to_string((*moved).key) == "b");
}

TEST_CASE("changes_since stops at the snapshot's durable sequence",
          "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto synced = db.put({.sync = true}, to_bytes("a"), to_bytes("1"));
  const auto unsynced = db.put({.sync = false}, to_bytes("b"), to_bytes("2"));
  REQUIRE_FALSE(unsynced.durable);
  auto snap = db.snapshot();
  std::vector<std::uint64_t> seqs;
  for (const auto &e : db.changes_since(snap, 0)) seqs.push_back(e.sequence);
  CHECK(seqs == std::vector<std::uint64_t>{synced.sequence});
}

TEST_CASE("a default ChangeIterator is at its end", "[replication]") {
  const bytecask::ChangeIterator it;
  CHECK(it == std::default_sentinel);
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

TEST_CASE("set_mode(Follower) makes unsynced acknowledged writes durable",
          "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");

  db.put({.sync = true}, to_bytes("a"), to_bytes("1"));
  const auto synced = db.durable_sequence();
  const auto r = db.put({.sync = false}, to_bytes("b"), to_bytes("2"));
  REQUIRE_FALSE(r.durable);
  REQUIRE(db.durable_sequence() < r.sequence);

  db.set_mode(bytecask::Mode::Follower);
  CHECK(db.durable_sequence() >= r.sequence);
  CHECK(db.durable_sequence() > synced);

  // What stepping down made durable is what a new leader can be sent.
  auto snap = db.snapshot();
  auto shipped = false;
  for (const auto &e : db.changes_since(snap, synced)) {
    if (e.sequence == r.sequence) shipped = true;
  }
  CHECK(shipped);
}

TEST_CASE("set_mode(Follower): a failed fdatasync degrades and keeps the mode",
          "[replication]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto r = db.put({.sync = false}, to_bytes("b"), to_bytes("2"));
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS(db.set_mode(bytecask::Mode::Follower), std::system_error);
  }
  CHECK(db.is_degraded());
  CHECK(db.mode() == bytecask::Mode::Leader);
  CHECK(db.durable_sequence() < r.sequence);
  REQUIRE_NOTHROW(db.resume());
  db.set_mode(bytecask::Mode::Follower);
  CHECK(db.mode() == bytecask::Mode::Follower);
}

TEST_CASE("set_mode(Follower) on a degraded leader steps down without a sync",
          "[replication]") {
  // A sync after a failed one proves nothing (#231), so the unsynced write
  // stays above durable_sequence instead of being reported durable.
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const auto r = db.put({.sync = false}, to_bytes("b"), to_bytes("2"));
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS(db.set_mode(bytecask::Mode::Follower), std::system_error);
  }
  REQUIRE(db.is_degraded());
  const auto durable = db.durable_sequence();
  REQUIRE(durable < r.sequence);

  db.set_mode(bytecask::Mode::Follower);
  CHECK(db.mode() == bytecask::Mode::Follower);
  CHECK(db.is_degraded());
  CHECK(db.durable_sequence() == durable);
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "empty.data", 0); auto &file = *file_ptr;

  std::vector<bytecask::DataEntry> entries;
  for (const auto& [entry, off] :
       std::ranges::subrange{bytecask::DataFileIterator{file}, std::default_sentinel}) {
    entries.push_back(entry);
  }
  REQUIRE(entries.empty());
}

TEST_CASE("DataFileIterator yields all entries in order", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
  (void)file.append_entry(1, bytecask::EntryType::Put,
                          to_bytes("k1"), to_bytes("v1"));
  (void)file.append_entry(2, bytecask::EntryType::Put,
                          to_bytes("k2"), to_bytes("v2"));
  (void)file.append_entry(3, bytecask::EntryType::Delete,
                          to_bytes("k1"), {});

  std::vector<std::pair<std::uint64_t, bytecask::EntryType>> results;
  for (const auto& [entry, off] :
       std::ranges::subrange{bytecask::DataFileIterator{file}, std::default_sentinel}) {
    results.emplace_back(entry.sequence, entry.entry_type);
  }

  REQUIRE(results.size() == 3);
  CHECK(results[0] == std::pair{std::uint64_t{1}, bytecask::EntryType::Put});
  CHECK(results[1] == std::pair{std::uint64_t{2}, bytecask::EntryType::Put});
  CHECK(results[2] == std::pair{std::uint64_t{3}, bytecask::EntryType::Delete});
}

TEST_CASE("DataFileIterator reports correct offsets", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
  auto off1 = file.append_entry(1, bytecask::EntryType::Put,
                                to_bytes("a"), to_bytes("1"));
  auto off2 = file.append_entry(2, bytecask::EntryType::Put,
                                to_bytes("b"), to_bytes("2"));

  std::vector<bytecask::Offset> offsets;
  for (const auto& [entry, off] :
       std::ranges::subrange{bytecask::DataFileIterator{file}, std::default_sentinel}) {
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
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
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "empty.data", 0); auto &file = *file_ptr;

  std::vector<std::pair<bytecask::DataEntry, bytecask::Offset>> entries;
  for (const auto& e : bytecask::scan_committed(file)) {
    entries.push_back(e);
  }
  REQUIRE(entries.empty());
}

TEST_CASE("scan_committed handles RangeDel inside batch", "[iterator]") {
  TempDir td;
  auto file_ptr = bytecask::WritablePosixDataFile::create(td.path / "test.data", 0); auto &file = *file_ptr;
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

TEST_CASE("Limits: open rejects options above the hard ceilings",
          "[bytecask][limits]") {
  TempDir td;
  // At each ceiling: accepted.
  CHECK_NOTHROW(bytecask::DB::open(
      td.path / "at", {.max_file_bytes = bytecask::kMaxFileBytes,
                       .max_key_bytes = bytecask::kMaxKeySize,
                       .max_value_bytes = bytecask::kMaxValueSize}));
  // One past: refused, not lowered to the ceiling.
  CHECK_THROWS_AS(
      bytecask::DB::open(td.path / "k",
                         {.max_key_bytes = bytecask::kMaxKeySize + 1}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      bytecask::DB::open(td.path / "v",
                         {.max_value_bytes = bytecask::kMaxValueSize + 1}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      bytecask::DB::open(td.path / "f",
                         {.max_file_bytes = bytecask::kMaxFileBytes + 1}),
      std::invalid_argument);
  // A refusal takes no lock: the directory opens with valid options.
  CHECK_NOTHROW(bytecask::DB::open(td.path / "k"));
}

// ---------------------------------------------------------------------------
// Zero and empty boundaries (#371), and options at their edges (#372).
// ---------------------------------------------------------------------------

namespace {
auto count_data_files(const std::filesystem::path &dir) -> int {
  int n = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".data") ++n;
  return n;
}
}  // namespace

TEST_CASE("Edges: an empty key and an empty value behave like any other",
          "[bytecask][limits][edges]") {
  TempDir td;
  const auto dir = td.path / "db";
  const auto empty = to_bytes("");
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, empty, to_bytes("ev"));
    db.put({}, to_bytes("a"), empty);
    db.put({}, to_bytes("b"), to_bytes("bv"));

    CHECK(get_str(db, empty) == "ev");
    CHECK(db.contains_key({}, empty));
    bytecask::Bytes out{std::byte{1}};
    CHECK(db.get({}, to_bytes("a"), out));
    CHECK(out.empty());

    // An empty `from` means the start (forward) or the end (reverse), and
    // the empty key, the smallest of all, is still yielded.
    std::vector<std::string> fwd;
    for (const auto &k : db.keys_from({})) fwd.push_back(to_string(k));
    CHECK(fwd == std::vector<std::string>{"", "a", "b"});
    CHECK(to_string((*db.iter_from({}).begin()).key).empty());
    std::vector<std::string> rev;
    for (const auto &k : db.rkeys_from({})) rev.push_back(to_string(k));
    CHECK(rev == std::vector<std::string>{"b", "a", ""});

    // Guards on the empty key.
    {
      bytecask::WritePlan p{db.snapshot()};
      p.ensure_present(empty);
      p.put(to_bytes("g1"), to_bytes("x"));
      CHECK(db.apply_batch({}, std::move(p)).has_value());
    }
    {
      bytecask::WritePlan p{db.snapshot()};
      p.ensure_absent(empty);
      p.put(to_bytes("g2"), to_bytes("x"));
      CHECK_FALSE(db.apply_batch({}, std::move(p)).has_value());
    }
    {
      bytecask::WritePlan p{db.snapshot()};
      p.ensure_unchanged(empty);
      db.put({}, empty, to_bytes("ev2"));
      p.put(to_bytes("g3"), to_bytes("x"));
      CHECK_FALSE(db.apply_batch({}, std::move(p)).has_value());
    }

    // del and a range starting at the empty key remove it.
    CHECK(db.del({}, empty).has_value());
    CHECK_FALSE(db.contains_key({}, empty));
    db.put({}, empty, to_bytes("ev3"));
    db.del_range({}, empty, to_bytes("a"));
    CHECK_FALSE(db.contains_key({}, empty));
    CHECK(db.contains_key({}, to_bytes("a")));
    db.put({}, empty, to_bytes("ev4"));
  }

  const std::map<std::string, std::string> expected{
      {"", "ev4"}, {"a", ""}, {"b", "bv"}, {"g1", "x"}};
  for (const unsigned threads : {1U, 4U}) {
    INFO("recovery_threads=" << threads);
    const auto copy = td.path / std::format("r{}", threads);
    std::filesystem::copy(dir, copy, std::filesystem::copy_options::recursive);
    auto db = bytecask::DB::open(copy, {.recovery_threads = threads});
    CHECK(collect_kv(db) == expected);
  }

  // Through changes_since and ingest to a follower.
  auto leader = bytecask::DB::open(dir);
  auto follower = bytecask::DB::open(
      td.path / "f", {.initial_mode = bytecask::Mode::Follower});
  auto snap = leader.snapshot();
  std::vector<bytecask::DataEntry> owned;
  for (const auto &e : leader.changes_since(snap, 0))
    owned.push_back({e.sequence, e.entry_type, {e.key.begin(), e.key.end()},
                     {e.value.begin(), e.value.end()}});
  std::vector<bytecask::DataEntryView> views;
  for (const auto &e : owned)
    views.push_back({e.sequence, e.entry_type, e.key, e.value});
  follower.ingest(views);
  CHECK(collect_kv(follower) == expected);
}

TEST_CASE("Edges: count_keys with limit 0, and a range holding the empty key",
          "[bytecask][limits][edges]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  for (const auto *k : {"", "a", "b"}) db.put({}, to_bytes(k), to_bytes("v"));
  const auto snap = db.snapshot();
  CHECK(snap.count_keys(to_bytes(""), to_bytes("z"), 0) == 0);
  // The empty key counts like any other.
  CHECK(snap.count_keys(to_bytes(""), to_bytes("a"), 10) == 1);
  CHECK(snap.count_keys(to_bytes(""), to_bytes("z"), 10) == 3);
}

TEST_CASE("Options: recovery_threads = 0 is refused at open",
          "[bytecask][limits][edges]") {
  TempDir td;
  {
    auto db = bytecask::DB::open(td.path / "db");
    db.put({}, to_bytes("k"), to_bytes("v"));
  }
  CHECK_THROWS_AS(bytecask::DB::open(td.path / "db", {.recovery_threads = 0}),
                  std::invalid_argument);
  auto db = bytecask::DB::open(td.path / "db", {.recovery_threads = 1});
  CHECK(get_str(db, to_bytes("k")) == "v");
}

TEST_CASE("Options: a buffer pool of exactly 2 x max_file_bytes is accepted, "
          "one byte less is not", "[bytecask][limits][edges]") {
  TempDir td;
  constexpr std::uint64_t kFile = 1U << 16;
  const auto open = [&](const char *name, std::size_t capacity) {
    return bytecask::DB::open(
        td.path / name, {.max_file_bytes = kFile,
                         .io_backend = bytecask::IoBackend::BufferPool,
                         .buffer_pool = {.capacity_bytes = capacity}});
  };
  CHECK_NOTHROW(open("at", 2 * kFile));
  CHECK_THROWS_WITH(open("under", 2 * kFile - 1),
                    Catch::Matchers::ContainsSubstring(
                        std::format("2 x max_file_bytes = {}", 2 * kFile)));
}

TEST_CASE("Options: a value larger than max_file_bytes gets a file of its own",
          "[bytecask][limits][edges]") {
  TempDir td;
  // Larger than the test build's default max_file_bytes (64 KiB), and in the
  // pool case larger than the whole pool.
  const std::string big(300 * 1024, 'x');
  for (const auto backend :
       {bytecask::IoBackend::Pread, bytecask::IoBackend::BufferPool}) {
    const bytecask::Options opts{
        .max_value_bytes = 1U << 20, .io_backend = backend,
        .buffer_pool = {.capacity_bytes = 2 * bytecask::kDefaultRotationThreshold}};
    const auto dir =
        td.path / (backend == bytecask::IoBackend::Pread ? "pread" : "pool");
    {
      auto db = bytecask::DB::open(dir, opts);
      db.put({}, to_bytes("k1"), to_bytes(big));
      db.put({}, to_bytes("s"), to_bytes("small"));
      db.put({}, to_bytes("k2"), to_bytes(big));
      CHECK(get_str(db, to_bytes("k1")) == big);
    }
    // Each large write filled a file past the threshold and sealed it.
    CHECK(count_data_files(dir) >= 3);
    auto db = bytecask::DB::open(dir, opts);
    CHECK(get_str(db, to_bytes("k1")) == big);
    CHECK(get_str(db, to_bytes("k2")) == big);
    CHECK(get_str(db, to_bytes("s")) == "small");
  }
}

TEST_CASE("Options: max_hint_backlog = 0 under a value-heavy load",
          "[bytecask][limits][edges]") {
  TempDir td;
  const auto dir = td.path / "db";
  const auto value = [](int i) {
    return std::string(20 * 1024, static_cast<char>('a' + i % 26));
  };
  // Every second write rotates, and with the bound off writes never wait
  // for the hint writer.
  {
    auto db = bytecask::DB::open(
        dir, {.max_file_bytes = 32 * 1024, .max_hint_backlog = 0});
    for (int i = 0; i < 200; ++i)
      db.put({.sync = false}, to_bytes(std::format("k{:03}", i)),
             to_bytes(value(i)));
    CHECK(db.stats().at("bytecask.hint_backpressure_stalls") == 0);
  }
  auto db = bytecask::DB::open(dir);
  for (int i = 0; i < 200; ++i)
    CHECK(get_str(db, to_bytes(std::format("k{:03}", i))) == value(i));
}

TEST_CASE("Options: max_file_bytes = 0 seals a file after every write",
          "[bytecask][limits][edges]") {
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 0});
    for (int i = 0; i < 5; ++i)
      db.put({}, to_bytes(std::format("k{}", i)), to_bytes("v"));
    bytecask::WritePlan plan;
    plan.put(to_bytes("x"), to_bytes("1"));
    plan.put(to_bytes("y"), to_bytes("2"));
    REQUIRE(db.apply_batch({}, std::move(plan)).has_value());
  }
  // Six writes, each in a file of its own, and the active file.
  CHECK(count_data_files(dir) == 7);
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 0});
  CHECK(collect_kv(db) == std::map<std::string, std::string>{
                              {"k0", "v"}, {"k1", "v"}, {"k2", "v"},
                              {"k3", "v"}, {"k4", "v"}, {"x", "1"}, {"y", "2"}});
}

TEST_CASE("Limits: a KeyDirEntry packs each field at its ceiling and refuses "
          "one past", "[bytecask][limits]") {
  using E = bytecask::KeyDirEntry;
  const auto e = E::make(E::kMaxSequence, E::kMaxFileOffset, E::kMaxFileId,
                         E::kMaxValueSize);
  CHECK(e.sequence() == E::kMaxSequence);
  CHECK(e.file_offset() == E::kMaxFileOffset);
  CHECK(e.file_id() == E::kMaxFileId);
  CHECK(e.value_size() == E::kMaxValueSize);

  CHECK_THROWS_AS(E::make(E::kMaxSequence + 1, 0, 0, 0), std::runtime_error);
  CHECK_THROWS_AS(E::make(1, E::kMaxFileOffset + 1, 0, 0), std::runtime_error);
  CHECK_THROWS_AS(E::make(1, 0, E::kMaxFileId + 1, 0), std::runtime_error);
  CHECK_THROWS_AS(E::make(1, 0, 0, E::kMaxValueSize + 1), std::runtime_error);

  // The engine's limits keep every entry inside the packed fields.
  STATIC_CHECK(bytecask::kMaxValueSize == E::kMaxValueSize);
  STATIC_CHECK(bytecask::kMaxFileBytes + bytecask::kMaxBatchBytes - 1 <=
               E::kMaxFileOffset);
}

namespace {
// Lowers the per-write byte limit for one test and restores it.
struct ScopedBatchLimit {
  explicit ScopedBatchLimit(std::uint64_t bytes) {
    bytecask::test_max_batch_bytes = bytes;
  }
  ~ScopedBatchLimit() {
    bytecask::test_max_batch_bytes = bytecask::kMaxBatchBytes;
  }
  ScopedBatchLimit(const ScopedBatchLimit &) = delete;
  auto operator=(const ScopedBatchLimit &) -> ScopedBatchLimit & = delete;
};

struct ScopedWritevLimits {
  explicit ScopedWritevLimits(bytecask::WritevLimits limits)
      : saved_{bytecask::test_writev_limits} {
    bytecask::test_writev_limits = limits;
  }
  ~ScopedWritevLimits() { bytecask::test_writev_limits = saved_; }
  ScopedWritevLimits(const ScopedWritevLimits &) = delete;
  auto operator=(const ScopedWritevLimits &) -> ScopedWritevLimits & = delete;

private:
  bytecask::WritevLimits saved_;
};

auto data_file_bytes(const std::filesystem::path &dir) -> std::uintmax_t {
  std::uintmax_t total = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".data") total += e.file_size();
  return total;
}
}  // namespace

TEST_CASE("Limits: a plan larger than the per-write byte limit is refused "
          "before any I/O", "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  const std::string value(100, 'v');

  // Two puts and their batch markers.
  const auto two_puts = 2 * bytecask::entry_size(2, value.size()) +
                        2 * (bytecask::kHeaderSize + bytecask::kCrcSize);
  const auto make_plan = [&] {
    bytecask::WritePlan plan;
    plan.put(to_bytes("k1"), to_bytes(value));
    plan.put(to_bytes("k2"), to_bytes(value));
    return plan;
  };

  {
    const ScopedBatchLimit limit{two_puts};
    CHECK(db.apply_batch({}, make_plan()).has_value());
  }
  (void)db.del({}, to_bytes("k1"));
  (void)db.del({}, to_bytes("k2"));
  const auto seq_before = db.durable_sequence();
  const auto bytes_before = db.stats().at("bytecask.bytes_written");
  {
    const ScopedBatchLimit limit{two_puts - 1};
    CHECK_THROWS_AS(db.apply_batch({}, make_plan()), std::invalid_argument);
  }
  CHECK(db.durable_sequence() == seq_before);
  CHECK(db.stats().at("bytecask.bytes_written") == bytes_before);
  CHECK_FALSE(db.is_degraded());
  CHECK_FALSE(db.contains_key({}, to_bytes("k1")));
}

TEST_CASE("Limits: ingest refuses an atomic batch past the per-write byte "
          "limit", "[bytecask][limits]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db",
                               {.initial_mode = bytecask::Mode::Follower});
  // One 4 MiB buffer behind every entry: the slice is over 1 GiB on the wire
  // without holding it in memory.
  const std::vector<std::byte> value(4U * 1024 * 1024);
  const auto per_put = bytecask::entry_size(1, value.size());
  const auto puts = bytecask::kMaxBatchBytes / per_put + 1;
  std::vector<bytecask::DataEntryView> slice;
  std::uint64_t seq = 1;
  slice.push_back({seq++, bytecask::EntryType::BulkBegin, {}, {}});
  const auto key = to_bytes("k");
  for (std::uint64_t i = 0; i < puts; ++i)
    slice.push_back({seq++, bytecask::EntryType::Put, key, value});
  slice.push_back({seq++, bytecask::EntryType::BulkEnd, {}, {}});

  const auto bytes_before = data_file_bytes(td.path / "db");
  CHECK_THROWS_AS(db.ingest(slice), std::invalid_argument);
  CHECK(data_file_bytes(td.path / "db") == bytes_before);
  CHECK(db.durable_sequence() == 0);
  CHECK_FALSE(db.is_degraded());
}

TEST_CASE("Limits: ingest takes the last packable sequence and refuses the "
          "next", "[bytecask][limits]") {
  using E = bytecask::KeyDirEntry;
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db =
        bytecask::DB::open(dir, {.initial_mode = bytecask::Mode::Follower});
    const auto k = to_bytes("k");
    const auto v = to_bytes("v");

    // Every entry type is checked, not only the ones the key directory
    // packs: a tombstone or marker past the limit would reach the file.
    for (const auto type : {bytecask::EntryType::Put,
                            bytecask::EntryType::Delete,
                            bytecask::EntryType::BulkBegin}) {
      const std::array past{bytecask::DataEntryView{
          E::kMaxSequence + 1, type, k,
          type == bytecask::EntryType::Put ? v : bytecask::BytesView{}}};
      CHECK_THROWS_AS(db.ingest(past), std::invalid_argument);
    }
    CHECK(db.durable_sequence() == 0);

    const std::array at{bytecask::DataEntryView{
        E::kMaxSequence, bytecask::EntryType::Put, k, v}};
    db.ingest(at);
    CHECK(db.durable_sequence() == E::kMaxSequence);

    // The sequence space is used up: a leader write needs the next one and
    // is refused before it appends, leaving the engine healthy.
    db.set_mode(bytecask::Mode::Leader);
    const auto bytes_before = db.stats().at("bytecask.bytes_written");
    CHECK_THROWS_AS(db.put({}, to_bytes("k2"), v), std::runtime_error);
    CHECK(db.stats().at("bytecask.bytes_written") == bytes_before);
    CHECK_FALSE(db.is_degraded());
    CHECK(get_str(db, k) == "v");
  }
  auto db = bytecask::DB::open(dir);
  CHECK(get_str(db, to_bytes("k")) == "v");
  CHECK(db.durable_sequence() == E::kMaxSequence);
}

TEST_CASE("Limits: the last file id is used, then a write that needs another "
          "is refused until a reopen", "[bytecask][limits]") {
  using E = bytecask::KeyDirEntry;
  TempDir td;
  const auto dir = td.path / "db";
  const bytecask::Options opts{.max_file_bytes = 256};
  const std::string small(10, 's');
  const std::string big(300, 'b');
  {
    auto db = bytecask::DB::open(dir, opts);
    db.put({}, to_bytes("seed"), to_bytes(small));
    db.test_set_next_file_id(E::kMaxFileId);

    // Crosses the threshold: the rotation takes the last id.
    db.put({}, to_bytes("last"), to_bytes(big));
    CHECK(db.engine_state()->active_file_id == E::kMaxFileId);

    // Below the threshold no new file is needed.
    db.put({}, to_bytes("fits"), to_bytes(small));

    // Would reach it: refused before any I/O, the engine still writable.
    const auto bytes_before = db.stats().at("bytecask.bytes_written");
    CHECK_THROWS_WITH(db.put({}, to_bytes("over"), to_bytes(big)),
                      Catch::Matchers::ContainsSubstring("exhausted"));
    CHECK(db.stats().at("bytecask.bytes_written") == bytes_before);
    CHECK_FALSE(db.is_degraded());
    CHECK_FALSE(db.contains_key({}, to_bytes("over")));
    db.put({}, to_bytes("fits2"), to_bytes(small));

    // vacuum needs an id for its copy and refuses before making one.
    db.put({}, to_bytes("seed"), to_bytes(small));
    CHECK_THROWS_AS(db.vacuum({.fragmentation_threshold = 0.0}),
                    std::runtime_error);
    for (const auto &e : std::filesystem::directory_iterator{dir})
      CHECK(e.path().extension() != ".tmp");
    CHECK_FALSE(db.is_degraded());
  }
  // Open numbers the files from the directory, so the space is back.
  auto db = bytecask::DB::open(dir, opts);
  db.put({}, to_bytes("over"), to_bytes(big));
  for (const auto *k : {"seed", "fits", "fits2"})
    CHECK(get_str(db, to_bytes(k)) == small);
  CHECK(get_str(db, to_bytes("last")) == big);
  CHECK(get_str(db, to_bytes("over")) == big);
}

TEST_CASE("Limits: ingest refuses a slice that needs a file id it does not "
          "have, before any I/O", "[bytecask][limits]") {
  using E = bytecask::KeyDirEntry;
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db",
                               {.max_file_bytes = 256,
                                .initial_mode = bytecask::Mode::Follower});
  db.test_set_next_file_id(E::kMaxFileId + 1);
  const std::string big(300, 'b');
  const auto k = to_bytes("k");
  const std::array slice{
      bytecask::DataEntryView{1, bytecask::EntryType::Put, k, to_bytes("v")},
      bytecask::DataEntryView{2, bytecask::EntryType::Put, k, to_bytes(big)}};

  const auto bytes_before = data_file_bytes(td.path / "db");
  CHECK_THROWS_WITH(db.ingest(slice),
                    Catch::Matchers::ContainsSubstring("exhausted"));
  CHECK(data_file_bytes(td.path / "db") == bytes_before);
  CHECK(db.durable_sequence() == 0);
  CHECK_FALSE(db.is_degraded());
  // A slice that stays below the threshold still goes in.
  db.ingest(std::span{slice}.first(1));
  CHECK(db.durable_sequence() == 1);
}

TEST_CASE("Limits: resume without a file id leaves the engine degraded and "
          "untouched", "[bytecask][limits]") {
  using E = bytecask::KeyDirEntry;
  TempDir td;
  const auto dir = td.path / "db";
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("a"), to_bytes("1"));
    {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      CHECK_THROWS_AS(db.put({}, to_bytes("b"), to_bytes("2")),
                      std::system_error);
    }
    REQUIRE(db.is_degraded());
    db.test_set_next_file_id(E::kMaxFileId + 1);
    const auto bytes_before = data_file_bytes(dir);
    CHECK_THROWS_WITH(db.resume(),
                      Catch::Matchers::ContainsSubstring("exhausted"));
    CHECK(db.is_degraded());
    CHECK(data_file_bytes(dir) == bytes_before);
    CHECK(get_str(db, to_bytes("a")) == "1");
  }
  auto db = bytecask::DB::open(dir);
  CHECK(get_str(db, to_bytes("a")) == "1");
  db.put({}, to_bytes("c"), to_bytes("3"));
}

TEST_CASE("Limits: durable_sequence waits with a timeout the clock cannot "
          "represent", "[bytecask][limits][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  for (const auto timeout : {std::chrono::milliseconds::max(),
                             std::chrono::milliseconds::max() -
                                 std::chrono::milliseconds{1}}) {
    const auto target = db.durable_sequence() + 1;
    std::thread writer{[&] {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      db.put({.sync = true}, to_bytes("k"), to_bytes("v"));
    }};
    // A deadline that wrapped into the past would return at once, below
    // the target.
    CHECK(db.durable_sequence(target, timeout) >= target);
    writer.join();
  }
}

TEST_CASE("Limits: a commit group ends before a slot that would carry the "
          "file past the group limit", "[bytecask][limits][concurrency]") {
  TempDir td;
  const auto dir = td.path / "db";
  const std::string value(179, 'v');
  REQUIRE(bytecask::entry_size(2, value.size()) == 200);
  // Each slot alone fills a file past max_file_bytes, and the group limit,
  // lowered from 2^32 to keep the test small, admits one slot per group.
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 150});
  struct RestoreLimit {
    ~RestoreLimit() {
      bytecask::test_max_group_file_bytes =
          std::uint64_t{bytecask::KeyDirEntry::kMaxFileOffset} + 1;
    }
  } restore;
  bytecask::test_max_group_file_bytes = 150;

  // Both writers' slots reach the executor as one batch: the leader waits in
  // on_batch_start_ until the second is queued.
  std::mutex mu;
  std::condition_variable cv;
  bool leader_ready = false;
  db.test_write_group().on_batch_start_ = [&] {
    {
      std::lock_guard<std::mutex> lk{mu};
      leader_ready = true;
    }
    cv.notify_all();
    db.test_write_group().wait_for_queue_size(2);
  };
  std::thread leader{[&] { db.put({}, to_bytes("g0"), to_bytes(value)); }};
  std::thread follower{[&] {
    std::unique_lock<std::mutex> lk{mu};
    cv.wait(lk, [&] { return leader_ready; });
    lk.unlock();
    db.put({}, to_bytes("g1"), to_bytes(value));
  }};
  leader.join();
  follower.join();
  db.test_write_group().on_batch_start_ = nullptr;

  CHECK(get_str(db, to_bytes("g0")) == value);
  CHECK(get_str(db, to_bytes("g1")) == value);
  // Two groups, each rotating the file it filled: two sealed files and the
  // active one. One group would have put both slots in one file.
  int data_files = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".data") ++data_files;
  CHECK(data_files == 3);
}

TEST_CASE("Limits: batches at the production writev chunk boundary round-trip",
          "[bytecask][limits]") {
  TempDir td;
  const auto dir = td.path / "db";
  const auto value = [](std::size_t n, std::size_t i) {
    return std::format("v{}-{}", n, i);
  };
  // Puts per plan; each plan adds a begin and an end marker. 254 puts fill
  // one call exactly, 255 and 256 spill the end marker or one put into a
  // second, and 600 needs three.
  const std::array<std::size_t, 5> plans{
      bytecask::kMaxEntriesPerWritev - 2, bytecask::kMaxEntriesPerWritev - 1,
      bytecask::kMaxEntriesPerWritev, bytecask::kMaxEntriesPerWritev + 1, 600};
  {
    const ScopedWritevLimits limits{{bytecask::kMaxEntriesPerWritev,
                                     bytecask::kMaxBytesPerWritev}};
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 1U << 20});
    for (const auto n : plans) {
      bytecask::WritePlan plan;
      for (std::size_t i = 0; i < n; ++i)
        plan.put(to_bytes(std::format("p{}-{:04}", n, i)),
                 to_bytes(value(n, i)));
      REQUIRE(db.apply_batch({}, std::move(plan)).has_value());
    }
  }
  auto db = bytecask::DB::open(dir, {.max_file_bytes = 1U << 20});
  for (const auto n : plans)
    for (std::size_t i = 0; i < n; ++i)
      CHECK(get_str(db, to_bytes(std::format("p{}-{:04}", n, i))) ==
            value(n, i));
}

TEST_CASE("Limits: a batch split by the writev byte limit round-trips",
          "[bytecask][limits]") {
  TempDir td;
  const auto dir = td.path / "db";
  const std::string value(50, 'v');
  {
    // Every entry above 64 bytes goes out in a call of its own; markers pair.
    const ScopedWritevLimits limits{{bytecask::kMaxEntriesPerWritev, 64}};
    auto db = bytecask::DB::open(dir);
    bytecask::WritePlan plan;
    for (int i = 0; i < 20; ++i)
      plan.put(to_bytes(std::format("k{:02}", i)), to_bytes(value));
    plan.del(to_bytes("k03"));
    REQUIRE(db.apply_batch({}, std::move(plan)).has_value());
    CHECK(get_str(db, to_bytes("k19")) == value);
  }
  auto db = bytecask::DB::open(dir);
  for (int i = 0; i < 20; ++i) {
    const auto k = std::format("k{:02}", i);
    CHECK(get_str(db, to_bytes(k)) == (i == 3 ? "<missing>" : value));
  }
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
  CHECK(s.at("bytecask.group_writer_busy_us") == 0);
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

TEST_CASE("stats: group_writer_busy_us counts the serial section, within wall time",
          "[bytecask][stats]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path);
  const auto before = db.stats().at("bytecask.group_writer_busy_us");
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) {
    const auto k = std::format("k{}", i);
    db.put({.sync = false}, to_bytes(k), to_bytes("value"));
  }
  const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  const auto busy = db.stats().at("bytecask.group_writer_busy_us") - before;
  CHECK(busy > 0);
  CHECK(busy <= wall_us);
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
      "bytecask.keydir_pool_bytes",
      "bytecask.bytes_written",
      "bytecask.group_writer_batches",
      "bytecask.group_writer_coalesced",
      "bytecask.group_writer_busy_us",
      "bytecask.write_check_fallbacks",
      "bytecask.file_rotations",
      "bytecask.fsyncs",
      "bytecask.commit_wait_blocked",
      "bytecask.flush_settles",
      "bytecask.commit_delay_waits",
      "bytecask.commit_delay_us",
      "bytecask.commit_delay_fsync_us",
      "bytecask.commit_delay_round_trip_us",
      "bytecask.disk_reads",
      "bytecask.disk_read_bytes",
      "bytecask.pool_hits",
      "bytecask.pool_misses",
      "bytecask.pool_fills",
      "bytecask.pool_evictions",
      "bytecask.pool_frames_total",
      "bytecask.pool_frames_resident",
      "bytecask.pool_frames_released",
      "bytecask.pool_direct_io_fallbacks",
      "bytecask.vacuum_bytes_reclaimed",
      "bytecask.vacuum_files_unlinked",
      "bytecask.vacuum_tombstones_dropped",
      "bytecask.tombstones_needed",
      "bytecask.recovery_files",
      "bytecask.recovery_keys",
      "bytecask.recovery_duration_us",
      "bytecask.files_opened",
      "bytecask.crc_failures",
      "bytecask.io_errors",
      "bytecask.degraded_transitions",
      "bytecask.hint_backpressure_stalls",
      "bytecask.hint_backpressure_stall_us",
      "bytecask.degraded",
      "bytecask.hint_backlog",
      "bytecask.open_files",
  };
  for (const auto &name : expected) {
    CHECK(s.contains(name));
  }
  CHECK(s.size() == expected.size());
}

// ---------------------------------------------------------------------------
// Hint backlog backpressure (#146)
//
// Every rotation queues a hint task. With max_hint_backlog set, a rotation
// that finds that many tasks pending waits for the worker before it seals,
// so the backlog — what close must write and what an open after a crash must
// rebuild — never exceeds the limit. The worker is held with
// test_before_hint_ to build a backlog on demand.
// ---------------------------------------------------------------------------
namespace {

// Holds every hint task at its start until open() is called.
struct HintGate {
  std::mutex mu;
  std::condition_variable cv;
  bool is_open{false};

  void wait() {
    std::unique_lock<std::mutex> lk{mu};
    cv.wait(lk, [this] { return is_open; });
  }
  void open() {
    {
      std::lock_guard<std::mutex> lk{mu};
      is_open = true;
    }
    cv.notify_all();
  }
};

// Opens the gate on scope exit. Declared after the DB, so it runs before
// ~DB drains the worker — a closed gate there would never let it return.
struct OpenGateOnExit {
  HintGate &gate;
  ~OpenGateOnExit() { gate.open(); }
};

// Polls stats() until pred holds or two seconds pass.
template <typename Pred>
auto eventually(const bytecask::DB &db, Pred pred) -> bool {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred(db.stats())) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return pred(db.stats());
}

auto hintless_data_files(const std::filesystem::path &dir) -> int {
  int n = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() != ".data") continue;
    auto hint = e.path();
    hint.replace_extension(".hint");
    if (!std::filesystem::exists(hint)) ++n;
  }
  return n;
}

} // namespace

// [concurrency]: the single-threaded WASM build runs hint tasks inline, so a
// held worker would hold the writer too.
TEST_CASE("hint backlog: a rotation past max_hint_backlog waits for the worker",
          "[hint_backlog][concurrency]") {
  constexpr std::uint32_t kLimit = 2;
  constexpr int kPuts = 12;
  TempDir td;
  HintGate gate;
  // max_file_bytes = 1: every put seals its file and queues a hint.
  auto db = bytecask::DB::open(
      td.path / "db", {.max_file_bytes = 1, .max_hint_backlog = kLimit});
  OpenGateOnExit release{gate};
  db.test_before_hint_ = [&gate] { gate.wait(); };

  std::atomic<int> done{0};
  std::thread writer{[&] {
    for (int i = 0; i < kPuts; ++i) {
      db.put({}, to_bytes(std::format("k{:02d}", i)), to_bytes("v"));
      done.fetch_add(1);
    }
  }};

  // The writer fills the backlog, then stalls before sealing the next file.
  REQUIRE(eventually(db, [&](const auto &st) {
    return st.at("bytecask.hint_backlog") == kLimit;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  const auto stalled_at = done.load();
  CHECK(stalled_at < kPuts);
  CHECK(db.stats().at("bytecask.hint_backlog") == kLimit);
  CHECK(db.stats().at("bytecask.hint_backpressure_stalls") == 0);

  SECTION("an open after a crash here rebuilds at most limit + 1 hints") {
    // Copy the directory as a kill would leave it: the backlog and the
    // active file are the only data files without a hint.
    const auto crash = td.path / "crash";
    std::filesystem::copy(td.path / "db", crash);
    CHECK(hintless_data_files(crash) <= static_cast<int>(kLimit) + 1);
    // Every acknowledged put survives the crash.
    auto db2 = bytecask::DB::open(crash);
    for (int i = 0; i < stalled_at; ++i)
      CHECK(get_val(db2, to_bytes(std::format("k{:02d}", i))).has_value());
  }

  gate.open();
  writer.join();
  CHECK(done.load() == kPuts);
  const auto st = db.stats();
  CHECK(st.at("bytecask.hint_backlog") <= kLimit);
  CHECK(st.at("bytecask.hint_backpressure_stalls") >= 1);
  CHECK(st.at("bytecask.hint_backpressure_stall_us") > 0);
}

TEST_CASE("hint backlog: max_hint_backlog = 0 never waits",
          "[hint_backlog][concurrency]") {
  constexpr int kPuts = 12;
  TempDir td;
  HintGate gate;
  auto db = bytecask::DB::open(td.path,
                               {.max_file_bytes = 1, .max_hint_backlog = 0});
  OpenGateOnExit release{gate};
  db.test_before_hint_ = [&gate] { gate.wait(); };

  // With the worker held, every put still returns.
  for (int i = 0; i < kPuts; ++i)
    db.put({}, to_bytes(std::format("k{:02d}", i)), to_bytes("v"));
  const auto st = db.stats();
  CHECK(st.at("bytecask.hint_backlog") >= kPuts - 1);
  CHECK(st.at("bytecask.hint_backpressure_stalls") == 0);

  gate.open();
  CHECK(eventually(db, [](const auto &s) {
    return s.at("bytecask.hint_backlog") == 0;
  }));
}

// ---------------------------------------------------------------------------
// ReadOptions: verify_checksums=false exercises the unverified read paths
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
}

// Found by the chaos soak (#92). A point read of the active file fetches
// past the record it wants — a first read sized by a key budget or to the
// page end — so it reads frame bytes a concurrent append is writing. The
// file's logical end was stored and loaded relaxed, which ordered nothing,
// so ThreadSanitizer reported the frame copy racing the read. Under TSan
// this fails without the release/acquire on WritableFileOps::offset_;
// elsewhere it is a smoke test of reads against a moving active file.
TEST_CASE("io_backend=BufferPool: reads of the active file race no append",
          "[bytecask][buffer_pool][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(
      td.path, {.max_file_bytes = 8 * 1024 * 1024,
                .io_backend = bytecask::IoBackend::BufferPool,
                .buffer_pool = {.capacity_bytes = 32 * 1024 * 1024}});
  constexpr int kKeys = 64;
  for (int i = 0; i < kKeys; ++i)
    db.put({.sync = false}, to_bytes(std::format("k{:03d}", i)),
           to_bytes(std::format("v{}", i)));

  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (int n = 0; n < 4000; ++n)
      db.put({.sync = false}, to_bytes(std::format("k{:03d}", n % kKeys)),
             to_bytes(std::format("v{}", n)));
    stop.store(true);
  });
  bytecask::Bytes out;
  int reads = 0;
  while (!stop.load()) {
    // The most recently written keys: their records sit just below the end
    // the writer is appending at.
    CHECK(db.get({.verify_checksums = (reads % 2) == 0},
                 to_bytes(std::format("k{:03d}", reads % kKeys)), out));
    ++reads;
  }
  writer.join();
  CHECK(reads > 0);
}

TEST_CASE("io_backend=BufferPool: values survive recovery",
          "[bytecask][buffer_pool]") {
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
}

TEST_CASE("io_backend=BufferPool: vacuum does not pollute the pool",
          "[bytecask][buffer_pool]") {
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
}

TEST_CASE("io_backend=BufferPool: vacuum releases the frames of the files it "
          "deletes",
          "[bytecask][buffer_pool]") {
  // A deleted file's frames can never be read again. Left in the pool they
  // would each hold a frame until the SIEVE hand came round; vacuum gives
  // them back as it deletes the file.
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
  const auto before = db.stats();
  REQUIRE(before.at("bytecask.pool_frames_resident") > 0);
  REQUIRE(before.at("bytecask.pool_frames_released") == 0);

  bool vacuumed = false;
  while (db.vacuum({.fragmentation_threshold = 0.0})) {
    vacuumed = true;
  }
  REQUIRE(vacuumed);

  const auto after = db.stats();
  REQUIRE(after.at("bytecask.vacuum_files_unlinked") > 0);
  const auto released = after.at("bytecask.pool_frames_released");
  CHECK(released > 0);
  // Vacuum admits nothing, so residency falls by exactly what it released.
  REQUIRE(after.at("bytecask.pool_fills") == before.at("bytecask.pool_fills"));
  CHECK(after.at("bytecask.pool_frames_resident") ==
        before.at("bytecask.pool_frames_resident") - released);

  bytecask::Bytes out;
  for (int i = 0; i < kCount; ++i) {
    const auto key = std::format("k{:05d}", i);
    if (i % 2 == 0) {
      CHECK_FALSE(db.get({}, to_bytes(key), out));
    } else {
      REQUIRE(db.get({}, to_bytes(key), out));
      CHECK(to_string(out) == value_for(i));
    }
  }
}

TEST_CASE("io_backend=BufferPool: verify_checksums=false read paths",
          "[bytecask][buffer_pool]") {
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
  // One hit per get. A key directory that reads keys back also reads through
  // the pool on every put (the candidate's key) and reads a record's header
  // before the rest, so it counts more; it must still never miss.
  if constexpr (bytecask::kKeyDirReadsKeys)
    CHECK(st.at("bytecask.pool_hits") >= kCount);
  else
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
}

TEST_CASE("io_backend=BufferPool: rotation releases the previous active file",
          "[bytecask][buffer_pool]") {
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
}

TEST_CASE("io_backend=BufferPool: iteration and vacuum agree with pread",
          "[bytecask][buffer_pool]") {
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
}

TEST_CASE("io_backend=BufferPool: lent entry spans hold while readers evict",
          "[bytecask][buffer_pool][concurrency]") {
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
}

TEST_CASE("io_backend=BufferPool: an iterator outlives the DB with its spans",
          "[bytecask][buffer_pool]") {
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
}

// A thread's cached read snapshot pins the key directory version it last
// saw. Under writes that version comes to hold a whole retained copy of the
// tree — a MariaDB thread parked in the server's thread cache did exactly
// that, at ~1.1 GB — so the engine takes the cache away from a thread that
// has gone idle, without that thread's help: the scrape that runs on
// publish obsoletes any slot unused for ~1 s. The idle thread here does
// nothing after its one read; the two stats gauges show its version go.
TEST_CASE("an idle thread's cached version is reclaimed without its help",
          "[bytecask][reclamation][concurrency]") {
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

// A read cache knows its DB only by address and is reclaimed by idleness, so
// a closed DB's last state used to stay in every slot that read it — on the
// closing thread and on any other — until that thread read another DB or a
// scrape found it idle. With the buffer pool that state holds the pool and
// all its frames. Closing a DB drops its entries from every thread's slot.
TEST_CASE("closing a DB releases the state every thread's read cache holds",
          "[bytecask][reclamation][concurrency][buffer_pool]") {
  TempDir td;
  std::weak_ptr<const bytecask::EngineState> state;
  std::weak_ptr<bytecask::DataFile> active;

  // A second thread reads once, then stays alive past the close.
  std::mutex mu;
  std::condition_variable cv;
  int step = 0;
  std::thread reader;
  const auto advance = [&](int to) {
    { std::lock_guard<std::mutex> lk{mu}; step = to; }
    cv.notify_all();
  };
  const auto await = [&](int at) {
    std::unique_lock<std::mutex> lk{mu};
    cv.wait(lk, [&] { return step >= at; });
  };
  {
    auto db = bytecask::DB::open(
        td.path / "db",
        {.io_backend = bytecask::IoBackend::BufferPool,
         .buffer_pool = {.capacity_bytes = 256ULL << 20}});
    db.put({}, to_bytes("k"), to_bytes("v"));
    bytecask::Bytes out;
    REQUIRE(db.get({}, to_bytes("k"), out));
    reader = std::thread{[&] {
      bytecask::Bytes v;
      CHECK(db.get({}, to_bytes("k"), v));
      advance(1);
      await(2);
    }};
    await(1);
    {
      const auto s = db.engine_state();
      state = s;
      active = *s->files.get(s->active_file_id);
    }

    SECTION("by close()") {
      db.close();
      CHECK(state.expired());
      CHECK(active.expired());
    }
    SECTION("by the destructor") {}
  }
  CHECK(state.expired());
  CHECK(active.expired());
  advance(2);
  reader.join();
}

// Close takes entries from slots other threads are claiming and releasing
// as it runs; readers here read until they see DbClosed. Nothing of the DB
// may be left cached once it is gone.
TEST_CASE("closing a DB under concurrent reads leaves nothing cached",
          "[bytecask][reclamation][concurrency][buffer_pool]") {
  TempDir td;
  std::weak_ptr<const bytecask::EngineState> state;
  std::vector<std::thread> readers;
  std::atomic<int> started{0};
  {
    auto db = bytecask::DB::open(
        td.path / "db",
        {.io_backend = bytecask::IoBackend::BufferPool,
         .buffer_pool = {.capacity_bytes = 256ULL << 20}});
    db.put({}, to_bytes("k"), to_bytes("v"));
    state = db.engine_state();
    for (int i = 0; i < 4; ++i) {
      readers.emplace_back([&] {
        bytecask::Bytes v;
        started.fetch_add(1);
        try {
          while (true) (void)db.get({}, to_bytes("k"), v);
        } catch (const bytecask::DbClosed &) {}
      });
    }
    while (started.load() < 4) std::this_thread::yield();
    db.close();
    for (auto &t : readers) t.join();
  }
  CHECK(state.expired());
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

// The periodic sync of a caller that writes with sync=false: while a
// synced commit's fdatasync is in flight, a sync-only write waits behind it
// like any synced writer, and the next flush covers it and the unsynced
// writes before it with one more fdatasync.
TEST_CASE("pipeline: a sync-only write joins group commit behind an "
          "in-flight flush", "[pipeline][concurrency][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  FlushGate gate;
  db.test_before_flush_sync_ = gate.hook();

  std::optional<bytecask::CommitResult> ra;
  std::optional<bytecask::CommitResult> rs;
  std::thread ta([&] { ra = db.put({.sync = true}, to_bytes("k1"), to_bytes("v1")); });
  gate.wait_in_flush();

  // Appended under the open flush, unsynced: published by the next flush.
  bytecask::CommitResult unsynced;
  std::thread tu([&] {
    unsynced = db.put({.sync = false}, to_bytes("k2"), to_bytes("v2"));
  });
  wait_until([&] { return blocked_count(db) >= 1; });
  std::thread ts([&] { rs = db.apply_batch({.sync = true}, bytecask::WritePlan{}); });
  wait_until([&] { return blocked_count(db) >= 2; });

  gate.open();
  ta.join();
  tu.join();
  ts.join();
  db.test_before_flush_sync_ = nullptr;

  REQUIRE(ra.has_value());
  REQUIRE(rs.has_value());
  CHECK(rs->durable);
  CHECK(db.durable_sequence() >= unsynced.sequence);
  // k1's fdatasync, then one covering k2 and the sync-only write.
  CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 2);
}

namespace {

// Five unsynced puts; with max_file_bytes = 4096 the next 8 KiB value
// crosses the rotation threshold.
void put_unsynced_tail(bytecask::DB &db) {
  for (int i = 0; i < 5; ++i) {
    const auto k = std::format("u{}", i);
    (void)db.put({.sync = false}, to_bytes(k), to_bytes("v"));
  }
}

constexpr std::size_t kRotatingValueBytes = 8192;

}  // namespace

// A sync-only write batched with a write that crosses the rotation
// threshold: the rotation barrier syncs the sealed file, which covers the
// sync-only write's target, so it adds no fdatasync of its own.
TEST_CASE("pipeline: a sync-only write in the same batch as a rotation is "
          "covered by the rotation's fdatasync", "[pipeline][concurrency][sync]") {
  TempDir td;

  // What the rotating write costs on its own.
  std::int64_t rotation_fsyncs = 0;
  {
    auto alone = bytecask::DB::open(td.path / "alone", {.max_file_bytes = 4096});
    put_unsynced_tail(alone);
    const auto before = alone.stats().at("bytecask.fsyncs");
    (void)alone.put({.sync = true}, to_bytes("big"), to_bytes(std::string(kRotatingValueBytes, 'x')));
    REQUIRE(alone.stats().at("bytecask.file_rotations") == 1);
    rotation_fsyncs = alone.stats().at("bytecask.fsyncs") - before;
  }

  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 4096});
  put_unsynced_tail(db);
  REQUIRE(db.durable_sequence() < db.engine_state()->next_seq - 1);
  const auto stats_before = db.stats();

  std::mutex mu;
  std::condition_variable cv;
  bool leader_ready = false;
  db.test_write_group().on_batch_start_ = [&] {
    {
      std::lock_guard<std::mutex> lk{mu};
      leader_ready = true;
    }
    cv.notify_all();
    db.test_write_group().wait_for_queue_size(2);
  };

  std::optional<bytecask::CommitResult> rbig;
  std::optional<bytecask::CommitResult> rsync;
  std::thread tbig([&] {
    rbig = db.put({.sync = true}, to_bytes("big"), to_bytes(std::string(kRotatingValueBytes, 'x')));
  });
  std::thread tsync([&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      cv.wait(lk, [&] { return leader_ready; });
    }
    rsync = db.apply_batch({.sync = true}, bytecask::WritePlan{});
  });
  tbig.join();
  tsync.join();
  db.test_write_group().on_batch_start_ = nullptr;

  const auto stats_after = db.stats();
  REQUIRE(rbig.has_value());
  REQUIRE(rsync.has_value());
  CHECK(rbig->durable);
  CHECK(rsync->durable);
  CHECK(rsync->sequence == 0);
  CHECK(stats_after.at("bytecask.file_rotations") -
            stats_before.at("bytecask.file_rotations") == 1);
  CHECK(stats_after.at("bytecask.fsyncs") - stats_before.at("bytecask.fsyncs") ==
        rotation_fsyncs);
  CHECK(db.durable_sequence() >= rbig->sequence);
  const auto st = db.engine_state();
  CHECK(st->durable_seq >= st->sync_requested_seq);
}

// Several sync-only writers — say a periodic flush and a caller's own —
// arriving behind an in-flight flush coalesce into the next one.
TEST_CASE("pipeline: concurrent sync-only writes share one fdatasync",
          "[pipeline][concurrency][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  const auto fsyncs_before = db.stats().at("bytecask.fsyncs");

  FlushGate gate;
  db.test_before_flush_sync_ = gate.hook();

  std::thread ta([&] { (void)db.put({.sync = true}, to_bytes("k1"), to_bytes("v1")); });
  gate.wait_in_flush();

  bytecask::CommitResult unsynced;
  std::thread tu([&] {
    unsynced = db.put({.sync = false}, to_bytes("k2"), to_bytes("v2"));
  });
  wait_until([&] { return blocked_count(db) >= 1; });

  constexpr int kSyncOnly = 3;
  std::vector<std::optional<bytecask::CommitResult>> rs(kSyncOnly);
  std::vector<std::thread> ts;
  for (int i = 0; i < kSyncOnly; ++i) {
    ts.emplace_back([&, i] {
      rs[static_cast<std::size_t>(i)] =
          db.apply_batch({.sync = true}, bytecask::WritePlan{});
    });
  }
  wait_until([&] { return blocked_count(db) >= 1 + kSyncOnly; });

  gate.open();
  ta.join();
  tu.join();
  for (auto &t : ts) t.join();
  db.test_before_flush_sync_ = nullptr;

  for (const auto &r : rs) {
    REQUIRE(r.has_value());
    CHECK(r->durable);
  }
  CHECK(db.durable_sequence() >= unsynced.sequence);
  // k1's fdatasync, then one for k2 and all three sync-only writes.
  CHECK(db.stats().at("bytecask.fsyncs") - fsyncs_before == 2);
}

// Unsynced writers each calling a sync-only write after their own writes:
// on return, everything that thread wrote is durable, whichever flush —
// its own, another thread's, or a rotation — made it so.
TEST_CASE("pipeline: sync-only writes under concurrent unsynced writers",
          "[pipeline][concurrency][sync]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 4096});
  constexpr int kThreads = 8;
  constexpr int kRounds = 50;
  std::atomic<int> violations{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (int r = 0; r < kRounds; ++r) {
        bytecask::CommitResult mine;
        for (int i = 0; i < 4; ++i) {
          const auto k = std::format("t{}-r{}-{}", t, r, i);
          mine = db.put({.sync = false}, to_bytes(k), to_bytes("value"));
        }
        const auto res = db.apply_batch({.sync = true}, bytecask::WritePlan{});
        if (!res || !res->durable || db.durable_sequence() < mine.sequence) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &th : ts) th.join();

  CHECK(violations.load() == 0);
  CHECK(db.stats().at("bytecask.file_rotations") > 0);  // rotations were exercised
  const auto st = db.engine_state();
  CHECK(st->durable_seq >= st->sync_requested_seq);
  CHECK(st->durable_seq == st->next_seq - 1);
}

namespace {
struct SettleCase {
  bool w1_sync;  // the writer whose flush is observed
  bool w2_sync;  // a writer still in stage 1 during that flush
  bool settles;
};
}  // namespace

// A flush waits for writers still in stage 1 only when it will fdatasync:
// then they share it. An unsynced flush has nothing to share, so it
// publishes at once instead of waiting out kFlushSettleMax.
TEST_CASE("pipeline: a flush settles for writers in stage 1 only when it "
          "owes an fdatasync", "[pipeline][concurrency]") {
  const auto c = GENERATE(SettleCase{false, false, false},  // nothing owed
                          SettleCase{false, true, true},    // W2 will owe
                          SettleCase{true, false, true});   // W1 owes
  CAPTURE(c.w1_sync, c.w2_sync);
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  const auto settles_before = db.stats().at("bytecask.flush_settles");

  // W1 appends and parks before its flush; W2 is elected leader and parks
  // in stage 1. W1 is then released to flush with W2 still in the group.
  FlushGate before_wait;
  FlushGate leading;
  db.test_before_commit_wait_ = before_wait.hook();
  std::optional<bytecask::CommitResult> r1;
  std::optional<bytecask::CommitResult> r2;
  std::thread t1([&] { r1 = db.put({.sync = c.w1_sync}, to_bytes("k1"), to_bytes("v1")); });
  before_wait.wait_in_flush();
  db.test_write_group().on_batch_start_ = leading.hook();
  std::thread t2([&] { r2 = db.put({.sync = c.w2_sync}, to_bytes("k2"), to_bytes("v2")); });
  leading.wait_in_flush();

  before_wait.open();
  t1.join();  // returns while W2 is still held in stage 1
  CHECK(db.stats().at("bytecask.flush_settles") - settles_before ==
        (c.settles ? 1 : 0));
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));

  leading.open();
  t2.join();
  db.test_before_commit_wait_ = nullptr;
  db.test_write_group().on_batch_start_ = nullptr;
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  CHECK(db.contains_key({}, to_bytes("k2")));
  CHECK(r1->durable == c.w1_sync);
}

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
          "[pipeline][f_visibility][degraded][resume][concurrency]") {
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
  // A failed flush publishes its degraded state directly, not through the
  // checked store; it is still one transition.
  CHECK(db.stats().at("bytecask.degraded_transitions") == 1);
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

// Found by the chaos soak (#92). A rotating batch quiesces behind another
// writer's flush; when that flush failed, the rotation used to publish its
// own state — built on the head the failed flush left behind — clearing the
// degrade without resume() while flush_error_ stayed set. Every later sync
// writer then got the stale I/O error back, and a conflicting apply_batch
// waited forever for entries nothing would publish.
TEST_CASE("pipeline: rotation behind a failed flush stays degraded",
          "[pipeline][rotation][degraded][resume][concurrency]") {
  TempDir td;
  const std::string big(8192, 'x');
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 4096});
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));

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
  // Crosses the threshold: quiesces behind A's flush, which will fail.
  std::thread tb([&] {
    try {
      (void)db.put({.sync = true}, to_bytes("k2"), to_bytes(big));
    } catch (...) {
      eb = std::current_exception();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  gate.open();
  ta.join();
  tb.join();
  db.test_before_flush_sync_ = nullptr;

  REQUIRE(ea);
  REQUIRE(eb);
  CHECK_THROWS_AS(std::rethrow_exception(ea), std::system_error);
  CHECK_THROWS_AS(std::rethrow_exception(eb), std::system_error);
  CHECK(db.is_degraded());
  CHECK(db.stats().at("bytecask.degraded_transitions") == 1);
  CHECK_FALSE(db.contains_key({}, to_bytes("k1")));
  CHECK_FALSE(db.contains_key({}, to_bytes("k2")));
  CHECK_THROWS_AS(db.put({.sync = true}, to_bytes("k3"), to_bytes("v3")),
                  bytecask::DbDegraded);

  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  CHECK(db.contains_key({}, to_bytes("k1")));
  CHECK(db.contains_key({}, to_bytes("k2")));
  auto r = db.put({.sync = true}, to_bytes("k3"), to_bytes("v3"));
  CHECK(r.durable);
  CHECK(db.contains_key({}, to_bytes("k3")));
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

// A plan with a snapshot is checked and applied by the record location the
// snapshot gave each written key (D3), where it used to look the key up in
// both states. The two must agree on every outcome: one seeded workload runs
// on two databases, one resolving by location and one by key, and every
// commit, every conflict and the final contents and file stats must match.
// Vacuum relocates records under the snapshots, so a key whose location
// changed but whose version did not takes the fallback read.
TEST_CASE("location tokens: plans checked and applied by location commit and "
          "conflict exactly as by key", "[model][location]") {
  const auto seed = GENERATE(1u, 2u, 3u, 4u, 5u);
  CAPTURE(seed);
  TempDir td;
  const bytecask::Options opts{.max_file_bytes = 8 * 1024};
  auto a = bytecask::DB::open(td.path / "a", opts);
  auto b = bytecask::DB::open(td.path / "b", opts);
  b.test_resolve_by_key_ = true;

  std::mt19937_64 rng{seed};
  const auto key = [&] { return std::format("k{:02d}", rng() % 12); };
  const auto value = [&] { return std::string(rng() % 200, static_cast<char>('a' + rng() % 26)); };
  std::vector<std::pair<bytecask::Snapshot, bytecask::Snapshot>> snaps;
  int commits = 0;
  int conflicts = 0;
  // Snapshots are taken more often than plans use them, and age in a queue
  // of 16, so a plan's snapshot is often several writes old.
  for (int step = 0; step < 4000; ++step) {
    const auto dice = rng() % 100;
    if (dice < 45) {
      snaps.emplace_back(a.snapshot(), b.snapshot());
      if (snaps.size() > 16) snaps.erase(snaps.begin());
    } else if (dice < 47) {
      const auto ta = rng() % 2 == 0 ? 0.0 : 0.5;
      REQUIRE(a.vacuum({.fragmentation_threshold = ta}) ==
              b.vacuum({.fragmentation_threshold = ta}));
    } else if (dice < 60) {
      const auto k = key();
      if (rng() % 3 == 0) {
        const auto ra = a.del({.sync = false}, to_bytes(k));
        const auto rb = b.del({.sync = false}, to_bytes(k));
        REQUIRE(ra.has_value() == rb.has_value());
      } else {
        const auto v = value();
        REQUIRE(a.put({.sync = false}, to_bytes(k), to_bytes(v)).sequence ==
                b.put({.sync = false}, to_bytes(k), to_bytes(v)).sequence);
      }
    } else {
      std::optional<bytecask::WritePlan> pa;
      std::optional<bytecask::WritePlan> pb;
      if (!snaps.empty() && rng() % 5 != 0) {
        const auto i = static_cast<std::ptrdiff_t>(rng() % snaps.size());
        auto pair = std::move(snaps[static_cast<std::size_t>(i)]);
        snaps.erase(snaps.begin() + i);
        pa.emplace(std::move(pair.first));
        pb.emplace(std::move(pair.second));
      } else {
        pa.emplace();
        pb.emplace();
      }
      const auto ops = 1 + rng() % 4;
      std::string last;
      for (std::size_t o = 0; o < ops; ++o) {
        // Now and then the same key twice in one plan.
        const auto k = !last.empty() && rng() % 4 == 0 ? last : key();
        last = k;
        const auto kind = rng() % 10;
        if (kind < 6) {
          const auto v = value();
          pa->put(to_bytes(k), to_bytes(v));
          pb->put(to_bytes(k), to_bytes(v));
        } else if (kind < 9) {
          pa->del(to_bytes(k));
          pb->del(to_bytes(k));
        } else if (pa->has_snapshot()) {
          pa->ensure_unchanged(to_bytes(k));
          pb->ensure_unchanged(to_bytes(k));
        }
      }
      const auto ra = a.apply_batch({.sync = false}, std::move(*pa));
      const auto rb = b.apply_batch({.sync = false}, std::move(*pb));
      REQUIRE(ra.has_value() == rb.has_value());
      if (ra) {
        REQUIRE(ra->sequence == rb->sequence);
        ++commits;
      } else {
        ++conflicts;
      }
    }
  }
  snaps.clear();
  CHECK(commits > 300);
  CHECK(conflicts > 300);
  REQUIRE(collect_kv(a) == collect_kv(b));
  const auto stats = [](bytecask::DB &db) {
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                           std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats())
      vals.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                        fs.max_sequence);
    std::ranges::sort(vals);
    return vals;
  };
  REQUIRE(stats(a) == stats(b));
}

// ---------------------------------------------------------------------------
// Checked puts: a plan of snapshot puts is checked by the descents that apply
// it (TransientEngineState::apply_puts_checked). A put they cannot confirm
// undoes the plan, which is then checked and applied in two passes.
// ---------------------------------------------------------------------------

namespace {
auto fallbacks(bytecask::DB &db) -> std::int64_t {
  return db.stats().at("bytecask.write_check_fallbacks");
}
}  // namespace

TEST_CASE("checked puts: a plan of snapshot updates and inserts commits "
          "without a fallback", "[bytecask][checked_puts]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("u1"), to_bytes("old1"));
  db.put({}, to_bytes("u2"), to_bytes("old2"));
  const auto before = fallbacks(db);
  bytecask::WritePlan plan{db.snapshot()};
  plan.put(to_bytes("u1"), to_bytes("new1"));   // updates
  plan.put(to_bytes("u2"), to_bytes("new2"));
  plan.put(to_bytes("i1"), to_bytes("ins1"));   // inserts
  plan.put(to_bytes("i2"), to_bytes("ins2"));
  REQUIRE(db.apply_batch({}, std::move(plan)).has_value());
  CHECK(fallbacks(db) == before);
  CHECK(to_string(*get_val(db, to_bytes("u1"))) == "new1");
  CHECK(to_string(*get_val(db, to_bytes("u2"))) == "new2");
  CHECK(to_string(*get_val(db, to_bytes("i1"))) == "ins1");
  CHECK(to_string(*get_val(db, to_bytes("i2"))) == "ins2");
}

TEST_CASE("checked puts: an update that lost its race undoes the whole plan",
          "[bytecask][checked_puts]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("a0"));
  db.put({}, to_bytes("z"), to_bytes("z0"));
  auto snap = db.snapshot();
  const auto won = db.put({}, to_bytes("z"), to_bytes("z1"));  // after the snapshot
  const auto before = fallbacks(db);
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("a"), to_bytes("a1"));   // applied, then undone
  plan.put(to_bytes("n"), to_bytes("n1"));   // inserted, then undone
  plan.put(to_bytes("z"), to_bytes("z2"));   // the conflict
  CHECK_FALSE(db.apply_batch({}, std::move(plan)).has_value());
  CHECK(fallbacks(db) == before + 1);
  CHECK(to_string(*get_val(db, to_bytes("a"))) == "a0");
  CHECK_FALSE(get_val(db, to_bytes("n")).has_value());
  CHECK(to_string(*get_val(db, to_bytes("z"))) == "z1");
  // Nothing of the plan was written: the next write takes the next sequence.
  CHECK(db.put({}, to_bytes("q"), to_bytes("q")).sequence == won.sequence + 1);
}

TEST_CASE("checked puts: an insert whose key appeared since the snapshot "
          "conflicts", "[bytecask][checked_puts]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("a0"));
  auto snap = db.snapshot();
  db.put({}, to_bytes("n"), to_bytes("theirs"));  // appears after the snapshot
  bytecask::WritePlan plan{std::move(snap)};
  plan.put(to_bytes("a"), to_bytes("a1"));
  plan.put(to_bytes("n"), to_bytes("mine"));
  CHECK_FALSE(db.apply_batch({}, std::move(plan)).has_value());
  CHECK(to_string(*get_val(db, to_bytes("a"))) == "a0");
  CHECK(to_string(*get_val(db, to_bytes("n"))) == "theirs");
}

TEST_CASE("checked puts: the same key twice in a plan falls back and commits",
          "[bytecask][checked_puts]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("k"), to_bytes("k0"));
  const auto before = fallbacks(db);
  bytecask::WritePlan plan{db.snapshot()};
  plan.put(to_bytes("k"), to_bytes("first"));
  plan.put(to_bytes("new"), to_bytes("n1"));
  plan.put(to_bytes("new"), to_bytes("n2"));
  plan.put(to_bytes("k"), to_bytes("second"));
  REQUIRE(db.apply_batch({}, std::move(plan)).has_value());
  CHECK(fallbacks(db) == before + 1);
  CHECK(to_string(*get_val(db, to_bytes("k"))) == "second");
  CHECK(to_string(*get_val(db, to_bytes("new"))) == "n2");
}

// The fused check must agree with the two-pass check on every outcome. One
// seeded workload of put-only snapshot plans — updates, inserts of new keys,
// the same key twice, plain writes landing between snapshot and commit, and
// vacuum moving records under the snapshots — runs on two databases, one
// checking puts in their descents and one in a pass of their own. Every
// commit and conflict, the final contents and the file stats must match,
// and both paths must actually have run.
TEST_CASE("checked puts: plans checked by their descents commit and conflict "
          "exactly as when checked in a pass of their own",
          "[model][checked_puts]") {
  const auto seed = GENERATE(1u, 2u, 3u, 4u, 5u);
  CAPTURE(seed);
  TempDir td;
  const bytecask::Options opts{.max_file_bytes = 8 * 1024};
  const auto stats = [](bytecask::DB &db) {
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                           std::uint64_t, std::uint64_t>> vals;
    for (const auto &[fid, fs] : db.file_stats())
      vals.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                        fs.max_sequence, fs.marker_bytes);
    std::ranges::sort(vals);
    return vals;
  };
  decltype(collect_kv(std::declval<bytecask::DB &>())) kv_before;
  decltype(stats(std::declval<bytecask::DB &>())) stats_before;
  {
  auto a = bytecask::DB::open(td.path / "a", opts);
  auto b = bytecask::DB::open(td.path / "b", opts);
  b.test_two_pass_ = true;

  std::mt19937_64 rng{seed};
  int next_new = 0;
  const auto key = [&] {
    // Mostly a small hot set, so plans collide; now and then a fresh key.
    return rng() % 5 == 0 ? std::format("n{:05d}", next_new++)
                          : std::format("k{:02d}", rng() % 16);
  };
  const auto value = [&] {
    return std::string(1 + rng() % 150, static_cast<char>('a' + rng() % 26));
  };
  std::vector<std::pair<bytecask::Snapshot, bytecask::Snapshot>> snaps;
  int commits = 0;
  int conflicts = 0;
  for (int step = 0; step < 5000; ++step) {
    const auto dice = rng() % 100;
    if (dice < 40) {
      snaps.emplace_back(a.snapshot(), b.snapshot());
      if (snaps.size() > 12) snaps.erase(snaps.begin());
    } else if (dice < 42) {
      const auto ta = rng() % 2 == 0 ? 0.0 : 0.5;
      REQUIRE(a.vacuum({.fragmentation_threshold = ta}) ==
              b.vacuum({.fragmentation_threshold = ta}));
    } else if (dice < 55) {
      const auto k = key();
      const auto v = value();
      REQUIRE(a.put({.sync = false}, to_bytes(k), to_bytes(v)).sequence ==
              b.put({.sync = false}, to_bytes(k), to_bytes(v)).sequence);
    } else if (!snaps.empty()) {
      const auto i = static_cast<std::ptrdiff_t>(rng() % snaps.size());
      auto pair = std::move(snaps[static_cast<std::size_t>(i)]);
      snaps.erase(snaps.begin() + i);
      bytecask::WritePlan pa{std::move(pair.first)};
      bytecask::WritePlan pb{std::move(pair.second)};
      const auto ops = 1 + rng() % 6;
      std::string last;
      for (std::size_t o = 0; o < ops; ++o) {
        const auto k = !last.empty() && rng() % 6 == 0 ? last : key();
        last = k;
        const auto v = value();
        pa.put(to_bytes(k), to_bytes(v));
        pb.put(to_bytes(k), to_bytes(v));
      }
      const auto ra = a.apply_batch({.sync = false}, std::move(pa));
      const auto rb = b.apply_batch({.sync = false}, std::move(pb));
      REQUIRE(ra.has_value() == rb.has_value());
      if (ra) {
        REQUIRE(ra->sequence == rb->sequence);
        ++commits;
      } else {
        ++conflicts;
      }
    }
  }
  snaps.clear();
  CHECK(commits > 300);
  CHECK(conflicts > 300);
  CHECK(fallbacks(a) > 0);                 // the undo path ran
  CHECK(fallbacks(a) < commits + conflicts);  // and so did the fused one
  CHECK(fallbacks(b) == 0);
  REQUIRE(collect_kv(a) == collect_kv(b));
  REQUIRE(stats(a) == stats(b));
  kv_before = collect_kv(a);
  stats_before = stats(a);
  }
  // Recovery rebuilds the file stats from the data files: they must be the
  // ones the fused path kept in memory — apart from the empty active file
  // the reopen creates — and the ones recovery rebuilds for the two-pass
  // database.
  auto reopened_a = bytecask::DB::open(td.path / "a", opts);
  auto reopened_b = bytecask::DB::open(td.path / "b", opts);
  CHECK(collect_kv(reopened_a) == kv_before);
  CHECK(collect_kv(reopened_b) == kv_before);
  CHECK(stats(reopened_a) == stats(reopened_b));
  auto recovered = stats(reopened_a);
  std::erase_if(recovered, [](const auto &v) { return std::get<1>(v) == 0; });
  CHECK(recovered == stats_before);
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

// Found by the Elle isolation check (docs/isolation_checking_design.md) as
// G-single-item-realtime: a transaction that began after another returned
// read state from before that write. The gap is the one above, seen from a
// third thread: a write is "visible to subsequent reads" (CONTRACT.md), and
// a read that starts after put() returned is subsequent, whichever thread
// makes it.
TEST_CASE("pipeline: a write another thread published is visible to every "
          "thread once put returns, even before state_time_ is stored",
          "[pipeline][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));

  FlushGate before_wait;
  FlushGate between_stores;
  db.test_before_commit_wait_ = before_wait.hook();
  db.test_between_publish_stores_ = between_stores.hook();

  // R warms its read cache with the pre-put state, then reads once T's put
  // has returned.
  std::mutex mu;
  std::condition_variable cv;
  bool t_returned = false;
  bool r_warm = false;
  bool r_get_saw = false;
  bool r_snapshot_saw = false;
  std::thread tr([&] {
    (void)db.contains_key({}, to_bytes("seed"));
    std::unique_lock<std::mutex> lk{mu};
    r_warm = true;
    cv.notify_all();
    cv.wait(lk, [&] { return t_returned; });
    r_get_saw = get_val(db, to_bytes("t")).has_value();
    r_snapshot_saw = db.snapshot().contains_key({}, to_bytes("t"));
  });
  {
    std::unique_lock<std::mutex> lk{mu};
    cv.wait(lk, [&] { return r_warm; });
  }

  std::optional<bytecask::CommitResult> rt;
  std::thread tt([&] { rt = db.put({.sync = true}, to_bytes("t"), to_bytes("vt")); });
  before_wait.wait_in_flush();

  std::optional<bytecask::CommitResult> rf;
  std::thread tf([&] { rf = db.put({.sync = true}, to_bytes("f"), to_bytes("vf")); });
  between_stores.wait_in_flush();

  // T returns while F is parked between the state store and the
  // state_time_ store; only then does R read.
  before_wait.open();
  tt.join();
  {
    std::lock_guard<std::mutex> lk{mu};
    t_returned = true;
  }
  cv.notify_all();
  tr.join();
  between_stores.open();
  tf.join();
  db.test_before_commit_wait_ = nullptr;
  db.test_between_publish_stores_ = nullptr;

  REQUIRE(rt.has_value());
  REQUIRE(rf.has_value());
  CHECK(r_get_saw);
  CHECK(r_snapshot_saw);
}

// Found by the replication check's Elle run as G-single-item-realtime on
// the leader: reader A saw a write still in flight, and reader B, which
// began after A finished, did not. No writer has returned here, so nothing
// on the write side can close the gap; the publication itself must tell B's
// cache that the state moved before any thread can see the new state.
TEST_CASE("pipeline: a write one reader has seen is visible to every later "
          "reader, while its publication is still in progress",
          "[pipeline][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));

  FlushGate before_wait;
  FlushGate between_stores;
  db.test_before_commit_wait_ = before_wait.hook();
  db.test_between_publish_stores_ = between_stores.hook();

  // B warms its read cache with the pre-put state, then waits for A.
  std::mutex mu;
  std::condition_variable cv;
  bool a_done = false;
  bool b_warm = false;
  bool b_saw = false;
  std::thread tb([&] {
    (void)db.contains_key({}, to_bytes("seed"));
    std::unique_lock<std::mutex> lk{mu};
    b_warm = true;
    cv.notify_all();
    cv.wait(lk, [&] { return a_done; });
    b_saw = get_val(db, to_bytes("t")).has_value();
  });
  {
    std::unique_lock<std::mutex> lk{mu};
    cv.wait(lk, [&] { return b_warm; });
  }

  // T's entry is in the head, T parked before commit_wait. F publishes a
  // state covering both writes and parks inside that publication.
  std::thread tt([&] { (void)db.put({.sync = true}, to_bytes("t"), to_bytes("vt")); });
  before_wait.wait_in_flush();
  std::thread tf([&] { (void)db.put({.sync = true}, to_bytes("f"), to_bytes("vf")); });
  between_stores.wait_in_flush();

  // A, a thread with no cached state, reads the new state.
  bool a_saw = false;
  std::thread ta([&] { a_saw = get_val(db, to_bytes("t")).has_value(); });
  ta.join();
  {
    std::lock_guard<std::mutex> lk{mu};
    a_done = true;
  }
  cv.notify_all();
  tb.join();

  between_stores.open();
  tf.join();
  before_wait.open();
  tt.join();
  db.test_before_commit_wait_ = nullptr;
  db.test_between_publish_stores_ = nullptr;

  REQUIRE(a_saw);
  CHECK(b_saw);
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
  db.test_write_group().on_batch_start_ = [&] {
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
  db.test_write_group().on_batch_start_ = nullptr;
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

#ifndef NDEBUG
TEST_CASE("pipeline: publishing a key outside its file's stats degrades the "
          "engine",
          "[pipeline][invariants][degraded]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("k1"), to_bytes("v1"));
  auto bad = std::make_shared<bytecask::EngineState>(*db.engine_state());
  auto stats = bad->file_stats.transient();
  std::string reason;
  SECTION("a file with no stats") {
    stats.erase(bad->active_file_id);
    reason = "has no file_stats";
  }
  SECTION("a key past the file's committed extent") {
    auto fs = *stats.get(bad->active_file_id);
    fs.total_bytes = 0;
    stats.set(bad->active_file_id, fs);
    reason = "committed extent";
  }
  bad->file_stats = std::move(stats).persistent();
  db.test_publish(bad);
  CHECK(db.is_degraded());
  CHECK(db.degraded_reason().find(reason) != std::string::npos);
}
#endif

// ---------------------------------------------------------------------------
// Commit delay: who waits before an fdatasync (CommitDelay)
// ---------------------------------------------------------------------------

using namespace std::chrono_literals;

TEST_CASE("commit delay: a lone synced writer never waits",
          "[pipeline][commit_delay]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  for (int i = 0; i < 200; ++i)
    db.put({.sync = true}, to_bytes(std::format("k{}", i)), to_bytes("v"));
  const auto s = db.stats();
  CHECK(s.at("bytecask.commit_delay_waits") == 0);
  CHECK(s.at("bytecask.fsyncs") >= 200);
}

TEST_CASE("commit delay: unsynced writers never wait",
          "[pipeline][commit_delay][concurrency]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t)
    writers.emplace_back([&, t] {
      for (int i = 0; i < 500; ++i)
        db.put({.sync = false}, to_bytes(std::format("t{}-{}", t, i)), to_bytes("v"));
    });
  for (auto &w : writers) w.join();
  CHECK(db.stats().at("bytecask.commit_delay_waits") == 0);
}

TEST_CASE("commit delay: a synced commit waits once the last flush covered two",
          "[pipeline][commit_delay]") {
#ifdef BYTECASK_SINGLE_THREADED
  SKIP("one thread: no other writer can arrive, so a flush never waits");
#endif
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = true}, to_bytes("seed"), to_bytes("s"));
  // As if the last fdatasync covered 3 commits; nobody comes back, so the
  // next synced flush waits out min(F, 2R) = 20 ms.
  auto &d = db.test_commit_delay();
  for (int i = 0; i < 3; ++i) d.on_arrival();
  d.on_sync_start();
  d.on_sync_end(1s);
  d.on_round_trip(10ms);
  const auto before = db.stats().at("bytecask.commit_delay_waits");
  db.put({.sync = true}, to_bytes("k"), to_bytes("v"));
  CHECK(db.stats().at("bytecask.commit_delay_waits") == before + 1);
  CHECK(db.contains_key({}, to_bytes("k")));
}

// The round trip is per thread and per DB. A DB opened in the storage of a
// destroyed one has its address, and once named its DB by address, a
// thread's first synced commit there was timed from its last commit on the
// dead DB.
TEST_CASE("commit delay: a DB at a destroyed DB's address does not inherit "
          "its round trip",
          "[pipeline][commit_delay][tls]") {
  TempDir td;
  alignas(bytecask::DB) std::array<std::byte, sizeof(bytecask::DB)> storage{};
  auto *a = ::new (storage.data()) bytecask::DB{bytecask::DB::open(td.path / "a")};
  a->put({.sync = true}, to_bytes("k"), to_bytes("v"));
  std::destroy_at(a);

  auto *b = ::new (storage.data()) bytecask::DB{bytecask::DB::open(td.path / "b")};
  REQUIRE(static_cast<void *>(b) == static_cast<void *>(a));
  // Any round trip shorter than the fsync estimate is taken as a sample.
  b->test_commit_delay().on_sync_end(1h);
  b->put({.sync = true}, to_bytes("k"), to_bytes("v"));
  CHECK(b->test_commit_delay().round_trip_estimate() ==
        std::chrono::steady_clock::duration::zero());
  // The next commit on b is this thread's second there: it is timed.
  b->put({.sync = true}, to_bytes("k2"), to_bytes("v"));
  CHECK(b->test_commit_delay().round_trip_estimate() >
        std::chrono::steady_clock::duration::zero());
  std::destroy_at(b);
}

// Barriers flush to make state durable, not to batch: they take the flush
// role through quiesce(), which never waits, however the policy is primed.
TEST_CASE("commit delay: barriers never wait",
          "[pipeline][commit_delay]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({.sync = false}, to_bytes("k"), to_bytes("v"));  // a flush is owed
  auto &d = db.test_commit_delay();
  for (int i = 0; i < 3; ++i) d.on_arrival();
  d.on_sync_start();
  d.on_sync_end(30s);
  d.on_round_trip(15s);  // a wait would last up to 30 s
  const auto t0 = std::chrono::steady_clock::now();
  {
    const auto manifest = db.create_manifest();  // rotation: two fdatasyncs
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);
  }
  CHECK(db.stats().at("bytecask.commit_delay_waits") == 0);
  db.close();
  CHECK(std::chrono::steady_clock::now() - t0 < 10s);
}
