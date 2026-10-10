// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Change markers (#397). A promotion appends a ChangeMarker entry that names
// the history from there on; changes_since hands out whole-unit slices under a
// header naming the source's marker; ingest refuses a slice that starts past
// the follower (a gap) or whose history diverged at a promotion (a fork). The
// Python reference (bytecaskdb-python/reference/bytecask_ref.py) and its tests
// are the model these follow.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fault_injector.h"

import bytecask;
import bytecask.data_entry;

#include "proof/invariants.h"

namespace {

using bytecask::ChangeMarker;
using bytecask::EntryType;
using bytecask::Mode;
using bytecask::kOriginMarker;
using bytecask::testing::collect_changes;
using bytecask::testing::OwnedEntries;
using bytecask::testing::to_bytes;
using bytecask::testing::to_string;

constexpr auto kMarkerBytes =
    bytecask::change_marker_size(EntryType::ChangeMarker);

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    auto tpl = (std::filesystem::temp_directory_path() / "bcdb_marker_XXXXXX")
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

auto follower_opts(std::uint64_t max_file_bytes = 64ULL * 1024 * 1024)
    -> bytecask::Options {
  return {.max_file_bytes = max_file_bytes, .initial_mode = Mode::Follower};
}

// One changes_since slice of src, owned.
auto slice(bytecask::DB &src, std::uint64_t from,
           std::size_t max_bytes = bytecask::kUnlimitedBytes) -> OwnedEntries {
  auto snap = src.snapshot();
  return collect_changes(src.changes_since(snap, from, max_bytes));
}

// One changes_since -> ingest, from dst's position unless given.
auto replicate(bytecask::DB &src, bytecask::DB &dst,
               std::optional<std::uint64_t> from = std::nullopt,
               std::size_t max_bytes = bytecask::kUnlimitedBytes)
    -> OwnedEntries {
  auto s = slice(src, from.value_or(dst.durable_sequence()), max_bytes);
  dst.ingest(s.header, s.views());
  return s;
}

void catch_up(bytecask::DB &src, bytecask::DB &dst) {
  while (dst.durable_sequence() < src.durable_sequence())
    (void)replicate(src, dst);
}

// A new follower from src's manifest: the sealed files copied into dir.
auto bootstrap(bytecask::DB &src, const std::filesystem::path &dir)
    -> bytecask::DB {
  const auto m = src.create_manifest();
  std::filesystem::create_directories(dir);
  for (const auto &fi : m.files) {
    std::filesystem::copy_file(fi.data_path, dir / fi.data_path.filename());
    if (std::filesystem::exists(fi.hint_path))
      std::filesystem::copy_file(fi.hint_path, dir / fi.hint_path.filename());
  }
  return bytecask::DB::open(dir, follower_opts());
}

auto history(bytecask::DB &db) -> OwnedEntries { return slice(db, 0); }

auto same_history(const OwnedEntries &a, const OwnedEntries &b) -> bool {
  if (a.entries.size() != b.entries.size()) return false;
  for (std::size_t i = 0; i < a.entries.size(); ++i) {
    const auto &x = a.entries[i];
    const auto &y = b.entries[i];
    if (std::tie(x.sequence, x.entry_type, x.key, x.value) !=
        std::tie(y.sequence, y.entry_type, y.key, y.value))
      return false;
  }
  return true;
}

auto markers(bytecask::DB &db) -> std::vector<ChangeMarker> {
  std::vector<ChangeMarker> out;
  for (const auto &e : history(db).entries)
    if (e.entry_type == EntryType::ChangeMarker)
      out.push_back({e.sequence, bytecask::decode_marker_id(e.value)});
  return out;
}

auto marker_at(bytecask::DB &db, std::uint64_t sequence) -> ChangeMarker {
  auto snap = db.snapshot();
  return db.changes_since(snap, sequence).header.marker;
}

auto sequences(const OwnedEntries &s) -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> out;
  for (const auto &e : s.entries) out.push_back(e.sequence);
  return out;
}

auto kv(bytecask::DB &db) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  for (auto &[key, value] : db.iter_from({}))
    out[to_string(key)] = to_string(value);
  return out;
}

auto data_file_bytes(const std::filesystem::path &dir) -> std::uintmax_t {
  std::uintmax_t total = 0;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".data") total += e.file_size();
  return total;
}

auto change_marker_bytes(bytecask::DB &db) -> std::uint64_t {
  std::uint64_t total = 0;
  for (const auto &[fid, fs] : db.file_stats()) total += fs.change_marker_bytes;
  return total;
}

}  // namespace

TEST_CASE("promotion writes a change marker, and only a promotion does",
          "[replication][change_marker]") {
  TempDir td;
  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("a"), to_bytes("1"));
  leader.set_mode(Mode::Leader);  // leader to leader: nothing written
  CHECK(leader.durable_sequence() == 1);
  CHECK(markers(leader).empty());
  CHECK(marker_at(leader, 1) == kOriginMarker);

  auto follower = bytecask::DB::open(td.path / "follower", follower_opts());
  (void)replicate(leader, follower);
  leader.set_mode(Mode::Follower);
  follower.set_mode(Mode::Leader);
  CHECK(follower.mode() == Mode::Leader);

  const auto ms = markers(follower);
  REQUIRE(ms.size() == 1);
  CHECK(ms[0].since_sequence == 2);
  CHECK(ms[0].id != 0);
  CHECK(follower.durable_sequence() == 2);  // synced before the mode changed
  // The first write takes the sequence after the marker's.
  CHECK(follower.put({}, to_bytes("b"), to_bytes("2")).sequence == 3);
  // The marker names the history from its sequence on; before it, the origin.
  CHECK(marker_at(follower, 1) == kOriginMarker);
  CHECK(marker_at(follower, 2) == ms[0]);
  CHECK(marker_at(follower, 99) == ms[0]);
  // On disk: an entry of its own, keyless, with the id as its value.
  const auto h = history(follower);
  REQUIRE(h.entries.size() == 3);
  CHECK(h.entries[1].entry_type == EntryType::ChangeMarker);
  CHECK(h.entries[1].key.empty());
  CHECK(h.entries[1].value.size() == bytecask::kChangeMarkerIdBytes);
  CHECK(change_marker_bytes(follower) == kMarkerBytes);

  // The old leader follows the new one and receives the marker as an entry.
  catch_up(follower, leader);
  CHECK(markers(leader) == ms);
  CHECK(marker_at(leader, 99) == ms[0]);
  CHECK(kv(leader) == kv(follower));
  CHECK(change_marker_bytes(leader) == kMarkerBytes);

  // Reopening in Leader mode is not a promotion.
  follower.close();
  auto reopened = bytecask::DB::open(td.path / "follower");
  CHECK(markers(reopened) == ms);
  CHECK(reopened.durable_sequence() == 3);
  CHECK(reopened.put({}, to_bytes("c"), to_bytes("3")).sequence == 4);
  CHECK(markers(reopened) == ms);
  CHECK(change_marker_bytes(reopened) == kMarkerBytes);
}

TEST_CASE("ingest refuses a slice that starts past the follower, before "
          "anything is written",
          "[replication][change_marker]") {
  TempDir td;
  auto leader = bytecask::DB::open(td.path / "leader");
  for (char c = 'a'; c <= 'f'; ++c)
    leader.put({}, to_bytes(std::string(1, c)), to_bytes("v"));
  auto follower = bytecask::DB::open(td.path / "follower", follower_opts());
  (void)replicate(leader, follower, std::nullopt, 1);  // one unit per slice
  (void)replicate(leader, follower, std::nullopt, 1);
  REQUIRE(follower.durable_sequence() == 2);

  auto s = slice(leader, 3);  // 3 is missing in between
  REQUIRE(s.entries.front().sequence == 4);
  const auto before = data_file_bytes(td.path / "follower");
  CHECK_THROWS_WITH(follower.ingest(s.header, s.views()),
                    Catch::Matchers::ContainsSubstring("gap"));
  CHECK(data_file_bytes(td.path / "follower") == before);
  CHECK(follower.durable_sequence() == 2);
  CHECK_FALSE(follower.is_degraded());

  (void)replicate(leader, follower);  // from 2: accepted
  CHECK(same_history(history(follower), history(leader)));
}

TEST_CASE("promoting the less advanced follower forks the one ahead",
          "[replication][change_marker]") {
  // The design's unplanned failover: L dies at 20; N is at 10, F at 20.
  TempDir td;
  auto L = bytecask::DB::open(td.path / "L");
  for (int i = 0; i < 10; ++i)
    L.put({}, to_bytes(std::format("k{:02}", i)), to_bytes("L"));
  auto N = bootstrap(L, td.path / "N");
  auto G = bootstrap(L, td.path / "G");
  for (int i = 10; i < 20; ++i)
    L.put({}, to_bytes(std::format("k{:02}", i)), to_bytes("L"));
  auto F = bootstrap(L, td.path / "F");
  REQUIRE(N.durable_sequence() == 10);
  REQUIRE(G.durable_sequence() == 10);
  REQUIRE(F.durable_sequence() == 20);
  L.close();  // L dies

  N.set_mode(Mode::Leader);  // the wrong choice: F is ahead of N
  N.put({}, to_bytes("n"), to_bytes("1"));
  const auto ms = markers(N);
  REQUIRE(ms.size() == 1);
  CHECK(ms[0].since_sequence == 11);
  CHECK(marker_at(N, 20) == ms[0]);
  CHECK(marker_at(F, 20) == kOriginMarker);

  // F holds L's 11..20, which N never had: refused, nothing written.
  const auto before = history(F);
  const auto bytes_before = data_file_bytes(td.path / "F");
  CHECK_THROWS_AS((void)replicate(N, F), bytecask::DbChangeMarkerMismatch);
  // Re-delivered from an earlier point, the marker is in the slice: refused.
  CHECK_THROWS_AS((void)replicate(N, F, 5), bytecask::DbChangeMarkerMismatch);
  CHECK(same_history(history(F), before));
  CHECK(data_file_bytes(td.path / "F") == bytes_before);
  CHECK(F.durable_sequence() == 20);
  CHECK_FALSE(F.is_degraded());

  // G, at N's position, catches up and receives the marker.
  catch_up(N, G);
  CHECK(markers(G) == ms);
  CHECK(same_history(history(G), history(N)));

  // L comes back with writes nobody replicated: refused the same way.
  auto L2 = bytecask::DB::open(td.path / "L", follower_opts());
  REQUIRE(L2.durable_sequence() == 20);
  CHECK_THROWS_AS((void)replicate(N, L2), bytecask::DbChangeMarkerMismatch);

  // Re-bootstrapped from N, everything converges.
  F.close();
  L2.close();
  auto F2 = bootstrap(N, td.path / "F2");
  auto L3 = bootstrap(N, td.path / "L3");
  N.put({}, to_bytes("n"), to_bytes("2"));
  for (auto *f : {&F2, &L3, &G}) {
    catch_up(N, *f);
    CHECK(same_history(history(*f), history(N)));
    CHECK(kv(*f) == kv(N));
  }
}

TEST_CASE("re-delivery of the same history is accepted, with a promotion in "
          "the range",
          "[replication][change_marker]") {
  TempDir td;
  auto leader = bytecask::DB::open(td.path / "leader");
  for (char c = 'a'; c <= 'f'; ++c)
    leader.put({}, to_bytes(std::string(1, c)), to_bytes("1"));
  auto follower = bytecask::DB::open(td.path / "follower", follower_opts());
  catch_up(leader, follower);
  CHECK_FALSE(replicate(leader, follower, 3).entries.empty());  // all duplicates
  CHECK(follower.durable_sequence() == 6);
  CHECK(history(follower).entries.size() == 6);
  leader.put({}, to_bytes("g"), to_bytes("1"));
  leader.put({}, to_bytes("h"), to_bytes("1"));
  (void)replicate(leader, follower, 3);  // 4..8: duplicates skipped, 7, 8 applied
  CHECK(same_history(history(follower), history(leader)));

  leader.set_mode(Mode::Follower);
  follower.set_mode(Mode::Leader);  // marker at 9
  follower.put({}, to_bytes("i"), to_bytes("1"));  // 10
  catch_up(follower, leader);
  CHECK(markers(leader) == markers(follower));
  (void)replicate(follower, leader, 4);  // 5..10 again, marker included
  (void)replicate(follower, leader, 9);  // from the marker itself
  CHECK(same_history(history(leader), history(follower)));
}

TEST_CASE("a slice cut before the follower's marker is accepted; a forked "
          "source is refused once the comparison reaches the divergence",
          "[replication][change_marker]") {
  TempDir td;
  auto A = bytecask::DB::open(td.path / "A");
  for (char c = 'a'; c <= 'e'; ++c)
    A.put({}, to_bytes(std::string(1, c)), to_bytes("1"));
  auto G = bootstrap(A, td.path / "G");
  auto B = bootstrap(A, td.path / "B");
  A.set_mode(Mode::Follower);
  G.set_mode(Mode::Leader);  // marker at 6
  G.put({}, to_bytes("g"), to_bytes("1"));  // 7
  G.put({}, to_bytes("h"), to_bytes("1"));  // 8
  while (B.durable_sequence() < G.durable_sequence())
    (void)replicate(G, B, std::nullopt, 1);  // unit by unit: the marker alone
  const auto ms = markers(B);
  REQUIRE(ms.size() == 1);
  CHECK(ms[0].since_sequence == 6);
  REQUIRE(B.durable_sequence() == 8);

  // The comparison point stops at the slice's last entry: a re-delivered
  // slice from 2 cut at 3 says nothing about 6, and passes.
  auto s = slice(G, 2, 1);
  CHECK(sequences(s) == std::vector<std::uint64_t>{3});
  CHECK(s.header.marker == kOriginMarker);
  CHECK(s.header.from_sequence == 2);
  B.ingest(s.header, s.views());
  s = slice(G, 2);  // uncut: the marker is in the slice and matches
  CHECK(std::ranges::any_of(s.entries, [](const auto &e) {
    return e.entry_type == EntryType::ChangeMarker;
  }));
  B.ingest(s.header, s.views());
  CHECK(B.durable_sequence() == 8);
  CHECK(same_history(history(B), history(G)));

  // A at 5 is promoted too (split brain): its own marker at 6. Cut before
  // the divergence a slice passes; at or past it, refused.
  A.set_mode(Mode::Leader);
  A.put({}, to_bytes("split"), to_bytes("1"));  // 7
  s = slice(A, 2, 1);
  CHECK(sequences(s) == std::vector<std::uint64_t>{3});
  B.ingest(s.header, s.views());
  CHECK_THROWS_AS((void)replicate(A, B, 2), bytecask::DbChangeMarkerMismatch);
  CHECK_THROWS_AS((void)replicate(A, B), bytecask::DbChangeMarkerMismatch);
  CHECK(B.durable_sequence() == 8);
  CHECK(same_history(history(B), history(G)));
}

TEST_CASE("an empty slice compares the histories at from_sequence",
          "[replication][change_marker]") {
  TempDir td;
  auto A = bytecask::DB::open(td.path / "A");
  for (char c = 'a'; c <= 'e'; ++c)
    A.put({}, to_bytes(std::string(1, c)), to_bytes("1"));
  auto B = bootstrap(A, td.path / "B");
  auto Z = bootstrap(A, td.path / "Z");
  A.set_mode(Mode::Follower);
  B.set_mode(Mode::Leader);  // marker {6, b}
  B.put({}, to_bytes("b"), to_bytes("2"));  // 7
  Z.set_mode(Mode::Leader);  // split brain: marker {6, z}
  Z.put({}, to_bytes("z"), to_bytes("2"));  // 7
  Z.set_mode(Mode::Follower);
  REQUIRE(Z.durable_sequence() == B.durable_sequence());

  // Z is not behind B, so the slice is empty, and it is still refused.
  auto s = slice(B, Z.durable_sequence());
  CHECK(s.entries.empty());
  CHECK(s.header.marker == markers(B).front());
  CHECK_THROWS_AS(Z.ingest(s.header, s.views()),
                  bytecask::DbChangeMarkerMismatch);

  // Not behind on the same history: an empty slice is accepted.
  auto C = bootstrap(B, td.path / "C");
  REQUIRE(C.durable_sequence() == B.durable_sequence());
  (void)replicate(B, C);
  CHECK(C.durable_sequence() == B.durable_sequence());
  CHECK(markers(C) == markers(B));
}

TEST_CASE("ingest refuses a malformed slice with nothing written",
          "[replication][change_marker]") {
  TempDir td;
  auto leader = bytecask::DB::open(td.path / "leader");
  auto follower = bytecask::DB::open(td.path / "follower", follower_opts());
  const bytecask::ChangeHeader header{};
  const auto a = to_bytes("a");
  const auto v = to_bytes("1");
  const auto id = bytecask::encode_marker_id(1);
  const auto short_id = to_bytes("short");
  using V = bytecask::DataEntryView;
  const std::vector<std::pair<std::string, std::vector<V>>> cases{
      {"not increasing", {{2, EntryType::Put, a, v}, {1, EntryType::Put, a, v}}},
      {"repeated", {{1, EntryType::Put, a, v}, {1, EntryType::Put, a, v}}},
      {"at from", {{0, EntryType::Put, a, v}}},
      {"ends inside a batch", {{1, EntryType::BulkBegin, {}, {}}, {2, EntryType::Put, a, v}}},
      {"marker inside a batch",
       {{1, EntryType::BulkBegin, {}, {}}, {2, EntryType::ChangeMarker, {}, id},
        {3, EntryType::BulkEnd, {}, {}}}},
      {"marker with a key", {{1, EntryType::ChangeMarker, a, id}}},
      {"marker with a short id", {{1, EntryType::ChangeMarker, {}, short_id}}},
  };
  const auto before = data_file_bytes(td.path / "follower");
  for (const auto &[name, entries] : cases) {
    INFO(name);
    CHECK_THROWS_AS(follower.ingest(header, entries), std::invalid_argument);
    CHECK(follower.durable_sequence() == 0);
  }
  CHECK(data_file_bytes(td.path / "follower") == before);
  CHECK_THROWS_AS(leader.ingest(header, {}), std::logic_error);

  // A well-formed slice with a standalone marker goes in.
  const std::vector<V> good{{1, EntryType::BulkBegin, {}, {}},
                            {2, EntryType::Put, a, v},
                            {3, EntryType::BulkEnd, {}, {}},
                            {4, EntryType::ChangeMarker, {}, id}};
  follower.ingest(header, good);
  CHECK(follower.durable_sequence() == 4);
  CHECK(markers(follower) == std::vector<ChangeMarker>{{4, 1}});
  CHECK(marker_at(follower, 3) == kOriginMarker);
  CHECK(marker_at(follower, 4) == ChangeMarker{4, 1});
  CHECK(change_marker_bytes(follower) == kMarkerBytes);
}

TEST_CASE("changes_since hands out whole units and cuts at unit boundaries",
          "[replication][change_marker]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  db.put({}, to_bytes("a"), to_bytes("1"));  // 1
  bytecask::WritePlan plan;
  plan.put(to_bytes("b"), to_bytes("1"));
  plan.put(to_bytes("c"), to_bytes("1"));
  REQUIRE(db.apply_batch({}, std::move(plan)));  // 2 BulkBegin, 3, 4, 5 BulkEnd
  db.put({}, to_bytes("d"), to_bytes("1"));  // 6
  db.put({}, to_bytes("e"), to_bytes("1"));  // 7
  using S = std::vector<std::uint64_t>;
  CHECK(sequences(slice(db, 0)) == S{1, 2, 3, 4, 5, 6, 7});
  CHECK(sequences(slice(db, 1)) == S{2, 3, 4, 5, 6, 7});
  // A batch straddling from is left out whole.
  CHECK(sequences(slice(db, 3)) == S{6, 7});
  CHECK(sequences(slice(db, 4)) == S{6, 7});
  // max_bytes cuts after the unit that passes it, never inside a batch.
  CHECK(sequences(slice(db, 1, 1)) == S{2, 3, 4, 5});
  CHECK(sequences(slice(db, 0, 1)) == S{1});
  CHECK(sequences(slice(db, 5, 1)) == S{6});
  CHECK(sequences(slice(db, 0, 1000)) == S{1, 2, 3, 4, 5, 6, 7});
  CHECK(sequences(slice(db, 7)).empty());
  // Nothing a sync=false write left unsynced.
  (void)db.put({.sync = false}, to_bytes("f"), to_bytes("1"));  // 8
  CHECK(sequences(slice(db, 6)) == S{7});

  // A follower fed unit by unit holds the same history.
  auto follower = bytecask::DB::open(td.path / "follower", follower_opts());
  while (follower.durable_sequence() < db.durable_sequence())
    (void)replicate(db, follower, std::nullopt, 1);
  CHECK(same_history(history(follower), history(db)));
}

TEST_CASE("a follower whose writes stopped cannot be promoted",
          "[replication][change_marker][degraded]") {
  TempDir td;
  auto leader = bytecask::DB::open(td.path / "leader");
  leader.put({}, to_bytes("a"), to_bytes("1"));
  auto follower = bytecask::DB::open(td.path / "follower", follower_opts());
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS((void)replicate(leader, follower), std::system_error);
  }
  REQUIRE(follower.is_degraded());
  CHECK_THROWS_AS(follower.set_mode(Mode::Leader), bytecask::DbDegraded);
  CHECK(follower.mode() == Mode::Follower);
  CHECK(markers(follower).empty());
  REQUIRE_NOTHROW(follower.resume());
  follower.set_mode(Mode::Leader);
  CHECK(follower.mode() == Mode::Leader);
  CHECK(markers(follower).size() == 1);
}

TEST_CASE("a promotion whose fdatasync fails degrades and keeps the mode; "
          "resume finds its marker once",
          "[replication][change_marker][degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", follower_opts());
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
    CHECK_THROWS_AS(db.set_mode(Mode::Leader), std::system_error);
  }
  CHECK(db.is_degraded());
  CHECK(db.mode() == Mode::Follower);
  REQUIRE_NOTHROW(db.resume());
  // The marker reached the file; resume() replays it, once.
  const auto found = markers(db);
  REQUIRE(found.size() == 1);
  CHECK(found[0].since_sequence == 1);
  CHECK(change_marker_bytes(db) == kMarkerBytes);
  db.set_mode(Mode::Leader);  // a promotion of its own: a second marker
  CHECK(markers(db).size() == 2);
  CHECK(markers(db)[0] == found[0]);
  CHECK(db.put({}, to_bytes("a"), to_bytes("1")).sequence == 3);
  db.close();
  auto reopened = bytecask::DB::open(td.path / "db");
  CHECK(markers(reopened).size() == 2);
  CHECK(markers(reopened)[0] == found[0]);
  CHECK(change_marker_bytes(reopened) == 2 * kMarkerBytes);
}

TEST_CASE("a promotion whose append fails degrades and keeps the mode",
          "[replication][change_marker][degraded][resume]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db", follower_opts());
  {
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_append"};
    CHECK_THROWS_AS(db.set_mode(Mode::Leader), std::system_error);
  }
  CHECK(db.is_degraded());
  CHECK(db.mode() == Mode::Follower);
  REQUIRE_NOTHROW(db.resume());
  CHECK(markers(db).empty());
  db.set_mode(Mode::Leader);
  CHECK(markers(db).size() == 1);
  CHECK(db.put({}, to_bytes("a"), to_bytes("1")).sequence ==
        markers(db)[0].since_sequence + 1);
}

TEST_CASE("vacuum keeps a change marker: compaction leaves it in a file of "
          "its own, and never removes that file",
          "[replication][change_marker][vacuum]") {
  TempDir td;
  // k=v is 21 bytes, a marker 27: the marker tips the first file past 40 and
  // rotates it, so file 1 holds a put and the marker.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 40});
  db.put({}, to_bytes("k"), to_bytes("v"));  // 1
  db.set_mode(Mode::Follower);
  db.set_mode(Mode::Leader);  // 2: the marker, and a rotation
  REQUIRE(db.file_stats().size() == 2);
  db.put({}, to_bytes("k"), to_bytes("w"));  // 3: k=v is dead
  const auto ms = markers(db);
  REQUIRE(ms.size() == 1);

  REQUIRE(db.vacuum({.fragmentation_threshold = 0.0}));
  // The dead put went; the marker stayed, in a file of its own.
  bool marker_only_file = false;
  for (const auto &[fid, fs] : db.file_stats()) {
    if (fs.change_marker_bytes == kMarkerBytes) {
      marker_only_file = true;
      CHECK(fs.total_bytes == kMarkerBytes);
      CHECK(fs.live_bytes == 0);
      CHECK(fs.reclaimable_bytes() == 0);
    }
  }
  CHECK(marker_only_file);
  CHECK(markers(db) == ms);
  CHECK(marker_at(db, 3) == ms[0]);
  // Nothing left to reclaim in it: a second vacuum declines.
  CHECK_FALSE(db.vacuum({.fragmentation_threshold = 0.0}));
  CHECK(change_marker_bytes(db) == kMarkerBytes);
  db.close();
  auto reopened = bytecask::DB::open(td.path / "db", {.max_file_bytes = 40});
  CHECK(markers(reopened) == ms);
  CHECK(change_marker_bytes(reopened) == kMarkerBytes);
  bytecask::Bytes out;
  REQUIRE(reopened.get({}, to_bytes("k"), out));
  CHECK(to_string(out) == "w");
}

TEST_CASE("an unknown entry type on disk is damage: cut from the newest "
          "file's tail, refused anywhere else",
          "[replication][change_marker][recovery]") {
  TempDir td;
  const auto dir = td.path / "db";
  std::filesystem::path sealed;
  {
    auto db = bytecask::DB::open(dir);
    db.put({}, to_bytes("a"), to_bytes("1"));
    const auto m = db.create_manifest();  // seals the first file
    REQUIRE(m.files.size() == 1);
    sealed = m.files[0].data_path;
    db.put({}, to_bytes("b"), to_bytes("2"));
  }
  std::filesystem::path newest;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() == ".hint") std::filesystem::remove(e.path());
    else if (e.path().extension() == ".data" && e.path() != sealed)
      newest = e.path();
  }
  REQUIRE_FALSE(newest.empty());
  const auto bad = bytecask::serialize_entry(
      99, static_cast<EntryType>(9), to_bytes("x"), to_bytes("y"));
  auto append = [&](const std::filesystem::path &p) {
    std::ofstream f{p, std::ios::binary | std::ios::app};
    f.write(reinterpret_cast<const char *>(bad.data()),
            static_cast<std::streamsize>(bad.size()));
  };

  const auto newest_size = std::filesystem::file_size(newest);
  append(newest);
  {
    auto db = bytecask::DB::open(dir);  // the tail is cut
    CHECK(kv(db) == std::map<std::string, std::string>{{"a", "1"}, {"b", "2"}});
  }
  CHECK(std::filesystem::file_size(newest) == newest_size);

  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".hint") std::filesystem::remove(e.path());
  append(sealed);
  CHECK_THROWS_AS(bytecask::DB::open(dir), std::runtime_error);
}

TEST_CASE("Recovery model-based: change markers survive serial and parallel "
          "recovery, resume and vacuum",
          "[model][change_marker]") {
  TempDir td;
  const auto db_path = td.path / "db";
  std::mt19937_64 rng{397};
  auto pick = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>{lo, hi}(rng);
  };
  std::map<std::string, std::string> oracle;
  std::size_t promotions = 0;
  {
    auto db = bytecask::DB::open(db_path, {.max_file_bytes = 2048});
    for (int step = 0; step < 1500; ++step) {
      const bytecask::WriteOptions wo{.sync = pick(0, 9) == 0};
      const auto op = pick(0, 99);
      const auto key = std::format("k{:03}", pick(0, 199));
      if (op < 68) {
        const auto val = std::format("v{}", step);
        db.put(wo, to_bytes(key), to_bytes(val));
        oracle[key] = val;
      } else if (op < 85) {
        (void)db.del(wo, to_bytes(key));
        oracle.erase(key);
      } else if (op < 93) {
        bytecask::WritePlan plan;
        for (int b = pick(2, 5); b > 0; --b) {
          const auto k = std::format("k{:03}", pick(0, 199));
          const auto val = std::format("b{}", step);
          plan.put(to_bytes(k), to_bytes(val));
          oracle[k] = val;
        }
        (void)db.apply_batch(wo, std::move(plan));
      } else if (op < 96) {
        const auto a = pick(0, 180);
        const auto from = std::format("k{:03}", a);
        const auto to = std::format("k{:03}", a + pick(1, 10));
        db.del_range(wo, to_bytes(from), to_bytes(to));
        for (auto it = oracle.lower_bound(from); it != oracle.end();)
          it = it->first < to ? oracle.erase(it) : oracle.end();
      } else {
        // A promotion: step down, step up. The marker lands wherever the
        // writes have taken the active file.
        db.set_mode(Mode::Follower);
        db.set_mode(Mode::Leader);
        ++promotions;
      }
    }
    REQUIRE(promotions >= 20);
    CHECK(markers(db).size() == promotions);
  }

  using Stats = std::vector<std::tuple<std::uint64_t, std::uint64_t,
                                       std::uint64_t, std::uint64_t,
                                       std::uint64_t, std::uint64_t,
                                       std::uint64_t>>;
  auto collect_stats = [](bytecask::DB &db) {
    Stats vals;
    for (const auto &[fid, fs] : db.file_stats())
      vals.emplace_back(fs.live_bytes, fs.total_bytes, fs.min_sequence,
                        fs.max_sequence, fs.tombstone_bytes, fs.marker_bytes,
                        fs.change_marker_bytes);
    std::ranges::sort(vals);
    return vals;
  };
  auto copy_to = [&](const std::filesystem::path &p) {
    std::filesystem::copy(db_path, p, std::filesystem::copy_options::recursive);
  };

  std::vector<ChangeMarker> serial_markers;
  Stats serial_stats;
  {
    const auto p = td.path / "serial";
    copy_to(p);
    auto db = bytecask::DB::open(p, {.recovery_threads = 1});
    REQUIRE(kv(db) == oracle);
    serial_markers = markers(db);
    REQUIRE(serial_markers.size() == promotions);
    serial_stats = collect_stats(db);
    CHECK(change_marker_bytes(db) == promotions * kMarkerBytes);
    CHECK(marker_at(db, db.durable_sequence()) == serial_markers.back());
  }

  for (const unsigned threads : {2U, 5U, 8U}) {
    DYNAMIC_SECTION("recovery_threads = " << threads) {
      const auto p = td.path / std::format("t{}", threads);
      copy_to(p);
      auto db = bytecask::DB::open(p, {.recovery_threads = threads});
      CHECK(kv(db) == oracle);
      CHECK(markers(db) == serial_markers);
      CHECK(collect_stats(db) == serial_stats);
    }
  }

  SECTION("vacuum to convergence keeps every marker") {
    const auto p = td.path / "vacuum";
    copy_to(p);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 2048});
    while (db.vacuum({.fragmentation_threshold = 0.0})) {
    }
    CHECK(kv(db) == oracle);
    CHECK(markers(db) == serial_markers);
    CHECK(change_marker_bytes(db) == promotions * kMarkerBytes);
    db.close();
    auto reopened = bytecask::DB::open(p, {.max_file_bytes = 2048});
    CHECK(kv(reopened) == oracle);
    CHECK(markers(reopened) == serial_markers);
    CHECK(collect_stats(reopened) == collect_stats(reopened));
  }

  SECTION("resume replays the active file's markers, without duplicates") {
    const auto p = td.path / "resume";
    copy_to(p);
    auto db = bytecask::DB::open(p, {.max_file_bytes = 1 << 20});
    db.set_mode(Mode::Follower);
    db.set_mode(Mode::Leader);  // a published marker in the active file
    auto expected = serial_markers;
    expected.push_back(markers(db).back());
    {
      bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
      CHECK_THROWS_AS(db.put({}, to_bytes("k000"), to_bytes("x")),
                      std::system_error);
    }
    REQUIRE(db.is_degraded());
    REQUIRE_NOTHROW(db.resume());
    CHECK(markers(db) == expected);
    CHECK(change_marker_bytes(db) == expected.size() * kMarkerBytes);
    db.close();
    auto reopened = bytecask::DB::open(p);
    CHECK(markers(reopened) == expected);
  }
}

TEST_CASE("resume replays a marker an ingest appended but never published",
          "[replication][change_marker][resume]") {
  // Two rounds: into a node holding no marker, then into one holding one.
  TempDir td;
  auto leader = bytecask::DB::open(td.path / "leader");
  auto follower =
      bytecask::DB::open(td.path / "follower", follower_opts(256));
  const std::string big(300, 'x');
  std::vector<ChangeMarker> expected;
  for (int round = 0; round < 2; ++round) {
    leader.set_mode(Mode::Follower);
    leader.set_mode(Mode::Leader);  // a marker, then writes that rotate
    expected.push_back(markers(leader).back());
    for (int i = 0; i < 3; ++i)
      leader.put({}, to_bytes(std::format("r{}k{}", round, i)), to_bytes(big));
    // The first chunk, marker included, is appended; the rotation after it
    // fails in memory, so nothing is published and writes are refused.
    follower.test_in_finish_rotation_ = [] { throw std::bad_alloc{}; };
    CHECK_THROWS_AS((void)replicate(leader, follower), std::bad_alloc);
    follower.test_in_finish_rotation_ = {};
    CHECK(markers(follower).size() == static_cast<std::size_t>(round));
    REQUIRE_NOTHROW(follower.resume());
    CHECK(markers(follower) == expected);
    catch_up(leader, follower);
    CHECK(markers(follower) == expected);
    CHECK(same_history(history(follower), history(leader)));
  }
}

TEST_CASE("a promotion that fills the active file rotates it, unless no file "
          "id is left",
          "[replication][change_marker][limits]") {
  // A 75-byte put leaves the 100-byte file 25 short; the 27-byte marker
  // takes it past the threshold.
  TempDir td;
  const std::string value(55, 'v');
  const std::vector<bytecask::DataEntryView> slice{
      {1, EntryType::Put, to_bytes("k"), to_bytes(value)}};
  SECTION("with an id, the file is rotated") {
    auto db = bytecask::DB::open(td.path / "db", follower_opts(100));
    db.ingest({}, slice);
    REQUIRE(db.file_stats().size() == 1);
    db.set_mode(Mode::Leader);
    CHECK(db.mode() == Mode::Leader);
    CHECK(db.file_stats().size() == 2);
    CHECK(markers(db).size() == 1);
    CHECK(db.put({}, to_bytes("k2"), to_bytes("v")).sequence == 3);
  }
  SECTION("without one, the promotion still goes through and the file "
          "stays active") {
    auto db = bytecask::DB::open(td.path / "db", follower_opts(100));
    db.ingest({}, slice);
    db.test_set_next_file_id(bytecask::KeyDirEntry::kMaxFileId + 1);
    db.set_mode(Mode::Leader);
    CHECK(db.mode() == Mode::Leader);
    CHECK(db.file_stats().size() == 1);
    CHECK(markers(db).size() == 1);
    CHECK(db.durable_sequence() == 2);
    CHECK_FALSE(db.is_degraded());
  }
}
