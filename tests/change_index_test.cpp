// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The change index (#443): a sparse per-file index of unit boundaries that
// changes_since builds as it sweeps, so a later slice enters the file holding
// its from_sequence near it instead of parsing the file from its first byte.
// A cache: wrong only by a bug, checked at the fence all the same, and
// rebuilt by the sweep that finds it missing.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fault_injector.h"

import bytecask;
import bytecask.change_index;

#include "proof/invariants.h"

namespace {

using bytecask::ChangeFence;
using bytecask::ChangeIndex;
using bytecask::EntryType;
using bytecask::testing::collect_changes;
using bytecask::testing::OwnedEntries;
using bytecask::testing::to_bytes;

constexpr auto kSpacing = ChangeIndex::kFenceSpacing;

// The test build rotates at 64 KiB; these tests want one file.
auto one_file() -> bytecask::Options {
  return {.max_file_bytes = 8ULL * 1024 * 1024};
}

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    auto tpl = (std::filesystem::temp_directory_path() / "bcdb_cindex_XXXXXX")
                   .string();
    REQUIRE(::mkdtemp(tpl.data()) != nullptr);
    path = tpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir &) = delete;
  auto operator=(const TempDir &) -> TempDir & = delete;
};

// n puts of value_bytes each, unsynced, then one synced flush: the whole
// run is durable and the keys are distinct from any earlier fill.
void fill(bytecask::DB &db, std::size_t n, std::size_t value_bytes,
          std::size_t first_key = 0) {
  const std::string value(value_bytes, 'v');
  for (std::size_t i = 0; i < n; ++i) {
    (void)db.put({.sync = false}, to_bytes(std::format("k{:08}", first_key + i)),
                 to_bytes(value));
  }
  (void)db.apply_batch({.sync = true}, bytecask::WritePlan{});
}

auto pull(bytecask::DB &db, const bytecask::Snapshot &snap, std::uint64_t from,
          std::size_t max_bytes = bytecask::kUnlimitedBytes) -> OwnedEntries {
  return collect_changes(db.changes_since(snap, from, max_bytes));
}

auto seqs(const OwnedEntries &e) -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> out;
  for (const auto &entry : e.entries) out.push_back(entry.sequence);
  return out;
}

// The model of a slice: the whole units of the full history whose first
// sequence is above from.
auto expected_after(const OwnedEntries &history, std::uint64_t from)
    -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> out;
  auto in_batch = false;
  auto included = false;
  for (const auto &e : history.entries) {
    if (!in_batch) included = e.sequence > from;
    if (e.entry_type == EntryType::BulkBegin) in_batch = true;
    if (e.entry_type == EntryType::BulkEnd) in_batch = false;
    if (included) out.push_back(e.sequence);
  }
  return out;
}

auto unit_starts(const OwnedEntries &history) -> std::set<std::uint64_t> {
  std::set<std::uint64_t> out;
  auto in_batch = false;
  for (const auto &e : history.entries) {
    if (!in_batch) out.insert(e.sequence);
    if (e.entry_type == EntryType::BulkBegin) in_batch = true;
    if (e.entry_type == EntryType::BulkEnd) in_batch = false;
  }
  return out;
}

auto stat(bytecask::DB &db, const char *name) -> std::int64_t {
  return db.stats().at(name);
}

// Checks a slice from every `step`th sequence against the model, and
// returns how many slices looked the index up: those from a sequence the
// first file holds, which is every one above 0.
auto check_slices(bytecask::DB &db, const bytecask::Snapshot &snap,
                  const OwnedEntries &history, std::uint64_t step)
    -> std::int64_t {
  std::int64_t lookups = 0;
  const auto durable = db.durable_sequence();
  for (std::uint64_t from = 1; from < durable; from += step) {
    CHECK(seqs(pull(db, snap, from)) == expected_after(history, from));
    ++lookups;
  }
  return lookups;
}

}  // namespace

TEST_CASE("a slice from inside a file enters it at a fence and matches a "
          "sweep from the first byte",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 3000, 200);  // ~700 KiB: a fence every 4 KiB is ~170 fences
  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);  // from 0: no lookup, lays the fences
  REQUIRE(history.entries.size() == 3000);

  auto &idx = db.test_change_index();
  const auto ids = idx.file_ids();
  REQUIRE(ids.size() == 1);
  const auto fences = idx.fences(ids[0]);
  REQUIRE(fences.size() > 100);
  CHECK(fences.front().offset >= kSpacing);
  const auto starts = unit_starts(history);
  for (std::size_t i = 0; i < fences.size(); ++i) {
    CHECK(starts.contains(fences[i].sequence));
    if (i > 0) {
      CHECK(fences[i].offset >= fences[i - 1].offset + kSpacing);
      CHECK(fences[i].sequence > fences[i - 1].sequence);
    }
  }
  CHECK(stat(db, "bytecask.change_index_files") == 1);
  CHECK(stat(db, "bytecask.change_index_bytes") ==
        std::ssize(fences) * static_cast<std::int64_t>(sizeof(ChangeFence)));

  const auto hits0 = stat(db, "bytecask.change_index_hits");
  const auto misses0 = stat(db, "bytecask.change_index_misses");
  const auto lookups = check_slices(db, snap, history, 97);
  const auto hits = stat(db, "bytecask.change_index_hits") - hits0;
  const auto misses = stat(db, "bytecask.change_index_misses") - misses0;
  CHECK(hits + misses == lookups);
  // Only a from below the first fence's sequence has no fence to enter at.
  CHECK(misses == 1);
  CHECK(hits == lookups - 1);
}

TEST_CASE("fences sit on unit boundaries; a slice from inside a batch leaves "
          "the batch out",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  const std::string value(600, 'v');
  for (std::size_t i = 0; i < 200; ++i) {
    if (i % 3 != 2) {  // two batches, a single, so fences meet both
      bytecask::WritePlan plan;  // ~7 KiB on disk: longer than a spacing
      for (std::size_t j = 0; j < 12; ++j)
        plan.put(to_bytes(std::format("b{:04}-{:02}", i, j)), to_bytes(value));
      REQUIRE(db.apply_batch({.sync = false}, std::move(plan)));
    } else {
      (void)db.put({.sync = false}, to_bytes(std::format("s{:04}", i)),
                   to_bytes(value));
    }
  }
  (void)db.apply_batch({.sync = true}, bytecask::WritePlan{});
  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);

  auto &idx = db.test_change_index();
  const auto ids = idx.file_ids();
  REQUIRE(ids.size() == 1);
  const auto fences = idx.fences(ids[0]);
  REQUIRE(fences.size() > 50);
  const auto starts = unit_starts(history);
  std::size_t on_batches = 0;
  for (const auto &f : fences) {
    CHECK(starts.contains(f.sequence));
    const auto it = std::ranges::find(history.entries, f.sequence,
                                      &OwnedEntries::Entry::sequence);
    REQUIRE(it != history.entries.end());
    if (it->entry_type == EntryType::BulkBegin) ++on_batches;
  }
  CHECK(on_batches > 0);  // a batch longer than a spacing takes the fence
  (void)check_slices(db, snap, history, 7);
}

TEST_CASE("the active file's table grows with the pulls",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 1000, 200);
  const auto durable1 = db.durable_sequence();
  {
    const auto snap = db.snapshot();
    (void)pull(db, snap, 0);
  }
  auto &idx = db.test_change_index();
  const auto id = idx.file_ids().at(0);
  const auto n1 = idx.fences(id).size();
  REQUIRE(n1 > 10);

  fill(db, 1000, 200, 1000);
  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);
  const auto hits0 = stat(db, "bytecask.change_index_hits");
  CHECK(seqs(pull(db, snap, durable1)) == expected_after(history, durable1));
  CHECK(stat(db, "bytecask.change_index_hits") == hits0 + 1);
  const auto fences = idx.fences(id);
  CHECK(fences.size() > n1 + 10);
  CHECK(fences.back().sequence > durable1);
  CHECK(idx.file_ids().size() == 1);
}

TEST_CASE("a slice that stops early indexes what it crossed",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 3000, 200);
  const auto snap = db.snapshot();
  const auto durable = db.durable_sequence();
  constexpr std::size_t kBudget = 64 * 1024;
  const auto first = pull(db, snap, 0, kBudget);
  REQUIRE(first.entries.size() < 1000);

  auto &idx = db.test_change_index();
  const auto id = idx.file_ids().at(0);
  const auto laid = idx.fences(id);
  CHECK(laid.size() >= kBudget / kSpacing - 1);
  CHECK(laid.size() <= kBudget / kSpacing + 1);
  CHECK(laid.back().sequence <= first.entries.back().sequence);

  // A later slice enters at the last fence and sweeps on from there.
  const auto history = pull(db, snap, 0);
  const auto hits0 = stat(db, "bytecask.change_index_hits");
  CHECK(seqs(pull(db, snap, durable - 10)) ==
        expected_after(history, durable - 10));
  CHECK(stat(db, "bytecask.change_index_hits") == hits0 + 1);
  CHECK(idx.fences(id).size() > laid.size() * 5);
}

TEST_CASE("fences are handed over during the sweep, a bounded number at a time",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 8000, 200);  // ~1.8 MiB: more than 256 fences
  const auto snap = db.snapshot();
  auto &idx = db.test_change_index();
  auto batch = db.changes_since(snap, 0);
  auto it = std::move(batch.entries).begin();
  for (std::size_t i = 0; i < 6000 && !(it == std::default_sentinel); ++i) ++it;
  REQUIRE(!(it == std::default_sentinel));
  // The sweep is still in the file: the first chunk is already in the index.
  const auto ids = idx.file_ids();
  REQUIRE(ids.size() == 1);
  const auto mid = idx.fences(ids[0]).size();
  CHECK(mid >= 256);
  CHECK(mid % 256 == 0);
  while (!(it == std::default_sentinel)) ++it;
  const auto at_end = idx.fences(ids[0]).size();
  CHECK(at_end > mid);  // the tail went with the sweep's end
  CHECK(at_end > 400);
  const auto history = pull(db, snap, 0);
  (void)check_slices(db, snap, history, 997);
}

TEST_CASE("a sweep whose table was evicted does not start a tail-only one",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 8000, 200);  // ~1.8 MiB: several hand-overs
  const auto snap = db.snapshot();
  auto &idx = db.test_change_index();
  {
    auto batch = db.changes_since(snap, 0);
    auto it = std::move(batch.entries).begin();
    for (std::size_t i = 0; i < 6000; ++i) ++it;  // past the first hand-over
    REQUIRE(idx.files() == 1);
    const auto id = idx.file_ids().at(0);
    idx.forget(id);  // what eviction by other pulls would do
    while (!(it == std::default_sentinel)) ++it;
  }
  // The rest of that sweep went to no table: a table starts at the file's
  // first fence or not at all, so a sweep from the first byte can rebuild it.
  CHECK(idx.files() == 0);
  const auto history = pull(db, snap, 0);
  const auto fences = idx.fences(idx.file_ids().at(0));
  REQUIRE(fences.size() > 400);
  CHECK(fences.front().offset < 2 * kSpacing);
  (void)check_slices(db, snap, history, 997);
}

TEST_CASE("nothing above the durable sequence gets a fence",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 1000, 200);
  const auto durable = db.durable_sequence();
  const std::string value(200, 'u');
  for (std::size_t i = 0; i < 500; ++i)
    (void)db.put({.sync = false}, to_bytes(std::format("u{:04}", i)),
                 to_bytes(value));
  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);
  REQUIRE(history.entries.back().sequence == durable);

  auto &idx = db.test_change_index();
  const auto fences = idx.fences(idx.file_ids().at(0));
  REQUIRE(!fences.empty());
  for (const auto &f : fences) CHECK(f.sequence <= durable);
}

TEST_CASE("a compacted file is swept from its first byte and indexed again",
          "[replication][change_index][vacuum]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 32 * 1024});
  fill(db, 600, 100);  // ~128 bytes an entry: ~256 a file
  const std::string value(100, 'w');
  for (std::size_t i = 0; i < 150; ++i)  // most of the first file is now dead
    (void)db.put({.sync = false}, to_bytes(std::format("k{:08}", i)),
                 to_bytes(value));
  (void)db.apply_batch({.sync = true}, bytecask::WritePlan{});

  auto &idx = db.test_change_index();
  std::vector<std::uint32_t> before;
  {
    const auto snap = db.snapshot();
    const auto history = pull(db, snap, 0);
    before = idx.file_ids();
    REQUIRE(before.size() > 1);
    // The first file's survivors start above the dead run.
    const auto from = history.entries.at(170).sequence;
    const auto hits0 = stat(db, "bytecask.change_index_hits");
    CHECK(seqs(pull(db, snap, from)) == expected_after(history, from));
    CHECK(stat(db, "bytecask.change_index_hits") == hits0 + 1);
  }

  REQUIRE(db.vacuum());
  // The compacted file is gone from the index; its copy is a new id that
  // no sweep has crossed yet.
  const auto after = idx.file_ids();
  CHECK(!std::ranges::contains(after, before.front()));
  CHECK(after.size() == before.size() - 1);

  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);
  const auto copy = idx.file_ids().back();
  CHECK(copy > before.back());
  // A sequence the copy holds, past its first fence.
  const auto from = history.entries.at(50).sequence;
  REQUIRE(from < idx.fences(copy).back().sequence);
  const auto hits0 = stat(db, "bytecask.change_index_hits");
  const auto misses0 = stat(db, "bytecask.change_index_misses");
  idx.forget(copy);  // as before the sweep that laid them
  CHECK(seqs(pull(db, snap, from)) == expected_after(history, from));
  CHECK(stat(db, "bytecask.change_index_hits") == hits0);
  CHECK(stat(db, "bytecask.change_index_misses") == misses0 + 1);
  CHECK(seqs(pull(db, snap, from)) == expected_after(history, from));
  CHECK(stat(db, "bytecask.change_index_hits") == hits0 + 1);
}

TEST_CASE("a fence that does not hold is dropped and the file swept from its "
          "first byte",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 3000, 200);
  const auto snap = db.snapshot();
  const auto durable = db.durable_sequence();
  const auto history = pull(db, snap, 0);
  auto &idx = db.test_change_index();
  const auto id = idx.file_ids().at(0);
  const auto fences = idx.fences(id);
  REQUIRE(fences.size() > 3);
  const auto from = durable - 1;
  auto bad = fences;

  SECTION("past the end of the file") {
    bad.push_back({from, std::uint64_t{1} << 40});
  }
  SECTION("a header carrying another sequence") {
    bad.back().sequence += 1;
    REQUIRE(bad.back().sequence <= from);
  }
  idx.forget(id);
  REQUIRE(idx.extend(id, 0, bad));
  REQUIRE(idx.seek(id, from) == bad.back());

  const auto hits0 = stat(db, "bytecask.change_index_hits");
  const auto misses0 = stat(db, "bytecask.change_index_misses");
  CHECK(seqs(pull(db, snap, from)) == expected_after(history, from));
  CHECK(stat(db, "bytecask.change_index_hits") == hits0);
  CHECK(stat(db, "bytecask.change_index_misses") == misses0 + 1);
  CHECK(idx.fences(id) == fences);  // the sweep from the first byte relaid them
}

TEST_CASE("at most kMaxFiles files are indexed, the least recently pulled out",
          "[replication][change_index]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 16 * 1024});
  fill(db, 3000, 100);  // ~24 files of 16 KiB, 3 fences each
  constexpr auto kMaxFiles = static_cast<std::int64_t>(ChangeIndex::kMaxFiles);
  REQUIRE(stat(db, "bytecask.open_files") > kMaxFiles);
  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);
  auto &idx = db.test_change_index();
  CHECK(idx.files() == ChangeIndex::kMaxFiles);
  (void)check_slices(db, snap, history, 131);
  CHECK(idx.files() == ChangeIndex::kMaxFiles);
  CHECK(stat(db, "bytecask.change_index_files") == kMaxFiles);
}

TEST_CASE("resume clears the index", "[replication][change_index][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", one_file());
  fill(db, 1000, 200);
  {
    const auto snap = db.snapshot();
    (void)pull(db, snap, 0);
  }
  auto &idx = db.test_change_index();
  REQUIRE(idx.files() == 1);
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS((void)db.put({}, to_bytes("x"), to_bytes("y")),
                    std::system_error);
  }
  REQUIRE(db.is_degraded());
  REQUIRE_NOTHROW(db.resume());
  CHECK(idx.files() == 0);
  const auto snap = db.snapshot();
  const auto history = pull(db, snap, 0);
  (void)check_slices(db, snap, history, 211);
}

// ---------------------------------------------------------------------------
// The index on its own.
// ---------------------------------------------------------------------------

TEST_CASE("ChangeIndex: seek returns the last fence at or below from",
          "[change_index]") {
  ChangeIndex idx;
  const std::vector<ChangeFence> f{{10, 4096}, {20, 8192}, {30, 12288}};
  const auto plan = idx.extend(1, 0, f);
  REQUIRE(plan);
  CHECK(plan->generation != 0);
  CHECK(plan->next_offset == 12288 + kSpacing);
  CHECK(!idx.seek(2, 10));  // no table
  CHECK(!idx.seek(1, 9));   // none that low
  CHECK(idx.seek(1, 10) == f[0]);
  CHECK(idx.seek(1, 25) == f[1]);
  CHECK(idx.seek(1, 1000) == f[2]);
  CHECK(idx.plan(1).next_offset == 12288 + kSpacing);
  CHECK(idx.plan(1).spacing == kSpacing);
  CHECK(idx.plan(2).next_offset == kSpacing);
  CHECK(idx.plan(2).spacing == kSpacing);
  CHECK(idx.plan(2).generation == 0);
  CHECK(idx.file_ids() == std::vector<std::uint32_t>{1});
  idx.forget(1);
  CHECK(!idx.seek(1, 1000));
  CHECK(idx.files() == 0);
}

TEST_CASE("ChangeIndex: fences go only to the table their plan came from",
          "[change_index]") {
  ChangeIndex idx;
  const auto first = idx.extend(1, 0, std::vector<ChangeFence>{{10, 4096}});
  REQUIRE(first);
  // The table is gone: its sweep's fences start no new table.
  idx.forget(1);
  CHECK(!idx.extend(1, first->generation, std::vector<ChangeFence>{{20, 8192}}));
  CHECK(idx.files() == 0);
  // Another sweep from the first byte made a new table: the old plan's
  // fences are refused, the new table's own are taken.
  const auto second = idx.extend(1, 0, std::vector<ChangeFence>{{11, 4100}});
  REQUIRE(second);
  CHECK(second->generation != first->generation);
  CHECK(!idx.extend(1, first->generation, std::vector<ChangeFence>{{20, 8192}}));
  CHECK(idx.extend(1, second->generation, std::vector<ChangeFence>{{21, 8200}}));
  CHECK(idx.fences(1) == std::vector<ChangeFence>{{11, 4100}, {21, 8200}});
  // A plan for no table does not extend an existing one either.
  CHECK(!idx.extend(1, 0, std::vector<ChangeFence>{{30, 12300}}));
  CHECK(idx.fences(1).size() == 2);
}

TEST_CASE("ChangeIndex: a fence not a spacing past the last is dropped",
          "[change_index]") {
  ChangeIndex idx;
  CHECK(!idx.extend(1, 0, std::vector<ChangeFence>{{5, 100}}));  // below the first spacing
  CHECK(idx.files() == 0);
  const auto plan = idx.extend(1, 0, std::vector<ChangeFence>{{10, 4096}});
  REQUIRE(plan);
  const auto gen = plan->generation;
  CHECK(idx.extend(1, gen, std::vector<ChangeFence>{{12, 6000}, {20, 8192}}));
  CHECK(idx.extend(1, gen, std::vector<ChangeFence>{{20, 8192}}));  // a concurrent sweep's
  CHECK(idx.fences(1) == std::vector<ChangeFence>{{10, 4096}, {20, 8192}});
  CHECK(idx.bytes() == 2 * sizeof(ChangeFence));
  idx.clear();
  CHECK(idx.files() == 0);
  CHECK(idx.bytes() == 0);
}

TEST_CASE("ChangeIndex: the least recently used table goes past max_files",
          "[change_index]") {
  ChangeIndex idx{2};
  const auto one = idx.extend(1, 0, std::vector<ChangeFence>{{10, 4096}});
  REQUIRE(one);
  REQUIRE(idx.extend(2, 0, std::vector<ChangeFence>{{20, 4096}}));
  (void)idx.seek(1, 10);  // 1 is now the more recent
  REQUIRE(idx.extend(3, 0, std::vector<ChangeFence>{{30, 4096}}));
  CHECK(idx.file_ids() == std::vector<std::uint32_t>{1, 3});
  REQUIRE(idx.extend(1, one->generation, std::vector<ChangeFence>{{11, 8192}}));  // 1 again
  REQUIRE(idx.extend(4, 0, std::vector<ChangeFence>{{40, 4096}}));
  CHECK(idx.file_ids() == std::vector<std::uint32_t>{1, 4});
}

TEST_CASE("ChangeIndex: a full table keeps every other fence and doubles its "
          "spacing",
          "[change_index]") {
  ChangeIndex idx{ChangeIndex::kMaxFiles, 4};
  const auto plan = idx.extend(1, 0, std::vector<ChangeFence>{
                                         {1, 4096}, {2, 8192}, {3, 12288}, {4, 16384}});
  REQUIRE(plan);
  const auto gen = plan->generation;
  CHECK(idx.fences(1) == std::vector<ChangeFence>{{1, 4096}, {3, 12288}});
  CHECK(idx.spacing(1) == 8192);
  CHECK(plan->next_offset == 12288 + 8192);
  CHECK(plan->spacing == 8192);
  CHECK(idx.extend(1, gen, std::vector<ChangeFence>{{5, 16384}}));  // a spacing ago: dropped, table kept
  CHECK(idx.fences(1).size() == 2);
  CHECK(idx.extend(1, gen, std::vector<ChangeFence>{{6, 20480}, {7, 28672}}));
  CHECK(idx.fences(1) == std::vector<ChangeFence>{{1, 4096}, {6, 20480}});
  CHECK(idx.spacing(1) == 16384);
  CHECK(idx.seek(1, 6) == ChangeFence{6, 20480});
}

// The reproduction on #443: one-unit slices pulled from points spread across
// a 64 MiB file. Hidden; run with "[change_index_bench]".
TEST_CASE("changes_since one-unit pulls across a 64 MiB file",
          "[.][change_index_bench]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db",
                               {.max_file_bytes = 256ULL * 1024 * 1024});
  constexpr std::size_t kPuts = 64 * 1024;
  fill(db, kPuts, 1024);
  const auto snap = db.snapshot();
  const auto durable = db.durable_sequence();
  constexpr std::size_t kPulls = 200;
  std::size_t entries = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kPulls; ++i) {
    const auto from = durable * i / kPulls;
    for (const auto &e : db.changes_since(snap, from, 1).entries) {
      (void)e;
      ++entries;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  WARN(std::format("{} pulls of one unit: {} ms total, {} us per pull",
                   kPulls, us / 1000, us / static_cast<long>(kPulls)));
  CHECK(entries == kPulls);
}
