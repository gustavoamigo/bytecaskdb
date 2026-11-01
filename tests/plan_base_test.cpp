// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Model-based test of the two ways a plan names what its reads came from:
// a Snapshot, or the snapshot's sequence alone (CONTRACT.md, "Isolation is
// what the plan guards"). Point keys: ensure_unchanged guards and the
// implicit W-W check on written keys; tests/range_conflict_test.cpp does
// ranges.
//
// Two databases take the same history. Each round takes a snapshot of both,
// makes random intervening writes to both, then applies one plan to each:
// on the snapshot to db_snap, on its sequence to db_seq. A std::map with a
// version per write is the oracle:
//   snapshot  conflict exactly when a key the plan guards or writes differs
//             between the snapshot and the head (absent in both is equal).
//   sequence  the same, or when an intervening delete named such a key: a
//             del that removed it, or a del_range over it, present or not.
// So a snapshot conflict is always a sequence conflict, never the reverse.
// When only db_snap commits, its writes are replayed on db_seq unguarded so
// the two histories stay the same.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bytecask;

namespace {

struct TempDir {
  std::filesystem::path path;
  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "bc_plan_base_{}_{}",
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

auto to_bytes(std::string_view sv) -> bytecask::BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

constexpr int kKeys = 8;
auto key_of(int i) -> std::string { return std::format("k{}", i); }

// key -> version; every write gets a fresh version, as every record gets a
// fresh sequence in the engine.
using Model = std::map<std::string, std::uint64_t>;

auto at(const Model &m, const std::string &k) -> std::optional<std::uint64_t> {
  const auto it = m.find(k);
  return it == m.end() ? std::nullopt : std::optional{it->second};
}

struct Write {
  bool put;
  std::string key;
};

// What intervening deletes named: keys a del removed (DB::del of an absent
// key writes nothing), and every del_range's bounds.
struct Named {
  std::set<std::string> keys;
  std::vector<std::pair<std::string, std::string>> ranges;
  [[nodiscard]] auto names(const std::string &k) const -> bool {
    return keys.contains(k) ||
           std::ranges::any_of(ranges, [&](const auto &r) {
             return k >= r.first && k < r.second;
           });
  }
};

// Applies an intervening write to the model and both databases, adding what
// a delete named to `named`.
void intervene(bytecask::DB &a, bytecask::DB &b, Model &m,
               std::uint64_t &version, std::mt19937_64 &rng, Named &named) {
  const auto kind = std::uniform_int_distribution<int>{0, 9}(rng);
  const auto k = key_of(std::uniform_int_distribution<int>{0, kKeys - 1}(rng));
  if (kind < 5) {
    const auto v = std::format("v{}", ++version);
    a.put({.sync = false}, to_bytes(k), to_bytes(v));
    b.put({.sync = false}, to_bytes(k), to_bytes(v));
    m[k] = version;
  } else if (kind < 8) {
    (void)a.del({.sync = false}, to_bytes(k));
    (void)b.del({.sync = false}, to_bytes(k));
    if (m.erase(k) != 0) named.keys.insert(k);
  } else {
    const auto j = key_of(std::uniform_int_distribution<int>{0, kKeys}(rng));
    const auto from = std::min(k, j);
    const auto to = std::max(k, j) + "~";
    a.del_range({.sync = false}, to_bytes(from), to_bytes(to));
    b.del_range({.sync = false}, to_bytes(from), to_bytes(to));
    m.erase(m.lower_bound(from), m.lower_bound(to));
    named.ranges.emplace_back(from, to);
  }
}

auto contents(const bytecask::DB &db) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  for (auto &[k, v] : db.iter_from({})) {
    std::string ks(k.size(), '\0');
    std::string vs(v.size(), '\0');
    std::ranges::transform(k, ks.begin(), [](std::byte c) { return static_cast<char>(c); });
    std::ranges::transform(v, vs.begin(), [](std::byte c) { return static_cast<char>(c); });
    out.emplace(std::move(ks), std::move(vs));
  }
  return out;
}

auto contents(const Model &m) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  for (const auto &[k, v] : m) out.emplace(k, std::format("v{}", v));
  return out;
}

} // namespace

TEST_CASE("plans on a snapshot and on its sequence match their models for point keys",
          "[model][apply_batch][read_sequence][snapshot]") {
  TempDir td;
  auto db_snap = bytecask::DB::open(td.path / "snap");
  auto db_seq = bytecask::DB::open(td.path / "seq");
  std::mt19937_64 rng{7};
  Model model;
  std::uint64_t version = 0;
  int snap_conflicts = 0;
  int seq_conflicts = 0;
  int commits = 0;
  int removal_only = 0;

  for (int round = 0; round < 3000; ++round) {
    INFO(std::format("round {}", round));
    Named before;
    for (int i = std::uniform_int_distribution<int>{0, 2}(rng); i > 0; --i)
      intervene(db_snap, db_seq, model, version, rng, before);

    auto snap = db_snap.snapshot();
    const auto read_seq = db_seq.snapshot().sequence();
    REQUIRE(read_seq == snap.sequence());  // same history, same sequences
    const auto snap_model = model;

    // The plan: 1-3 writes and 0-2 unchanged guards over a small alphabet.
    std::vector<Write> writes;
    std::set<std::string> guarded;
    for (int i = std::uniform_int_distribution<int>{1, 3}(rng); i > 0; --i)
      writes.push_back({std::uniform_int_distribution<int>{0, 2}(rng) != 0,
                        key_of(std::uniform_int_distribution<int>{0, kKeys - 1}(rng))});
    for (int i = std::uniform_int_distribution<int>{0, 2}(rng); i > 0; --i)
      guarded.insert(key_of(std::uniform_int_distribution<int>{0, kKeys - 1}(rng)));
    // Reserved now, so no intervening write reuses the plan's version.
    const auto plan_version = ++version;
    const auto build = [&](bytecask::WritePlan plan, bool guards) {
      if (guards)
        for (const auto &g : guarded) plan.ensure_unchanged(to_bytes(g));
      for (const auto &w : writes) {
        if (w.put)
          plan.put(to_bytes(w.key), to_bytes(std::format("v{}", plan_version)));
        else
          plan.del(to_bytes(w.key));
      }
      return plan;
    };
    auto on_snap = build(bytecask::WritePlan{std::move(snap)}, true);
    auto on_seq = build(bytecask::WritePlan{read_seq}, true);

    Named since;
    for (int i = std::uniform_int_distribution<int>{0, 3}(rng); i > 0; --i)
      intervene(db_snap, db_seq, model, version, rng, since);

    std::set<std::string> touched = guarded;
    for (const auto &w : writes) touched.insert(w.key);
    bool states_differ = false;
    bool any_removed = false;
    for (const auto &k : touched) {
      states_differ |= at(snap_model, k) != at(model, k);
      any_removed |= since.names(k);
    }
    const bool expect_snap_conflict = states_differ;
    const bool expect_seq_conflict = states_differ || any_removed;
    if (any_removed && !states_differ) ++removal_only;

    const auto rs = db_snap.apply_batch({.sync = false}, std::move(on_snap));
    const auto rq = db_seq.apply_batch({.sync = false}, std::move(on_seq));
    REQUIRE(rs.has_value() == !expect_snap_conflict);
    REQUIRE(rq.has_value() == !expect_seq_conflict);
    if (!rs) REQUIRE(rs.error() == bytecask::PlanRefusal::Conflict);
    if (!rq) REQUIRE(rq.error() == bytecask::PlanRefusal::Conflict);
    // A snapshot conflict is always a sequence conflict.
    if (!rs) REQUIRE_FALSE(rq.has_value());
    snap_conflicts += rs ? 0 : 1;
    seq_conflicts += rq ? 0 : 1;

    if (rs) {
      ++commits;
      for (const auto &w : writes) {
        if (w.put)
          model[w.key] = plan_version;
        else
          model.erase(w.key);
      }
      // Keep the histories the same: replay what db_snap committed.
      if (!rq) REQUIRE(db_seq.apply_batch({.sync = false}, build(bytecask::WritePlan{}, false)).has_value());
    }
    REQUIRE(contents(db_snap) == contents(model));
    REQUIRE(contents(db_seq) == contents(model));
  }
  INFO(std::format("{} commits, {} snapshot conflicts, {} sequence conflicts, "
                   "{} rounds only a removal told apart",
                   commits, snap_conflicts, seq_conflicts, removal_only));
  CHECK(commits > 0);
  CHECK(snap_conflicts > 0);
  CHECK(seq_conflicts > snap_conflicts);
  CHECK(removal_only > 0);
}
