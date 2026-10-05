// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Model-based test of range conflict detection (#287): ensure_range_unchanged
// and the implicit W-W check of a planned del_range, against a std::map.
//
// Each round takes a snapshot, makes random intervening writes, then applies a
// plan that guards or range-deletes a random [from, to). The oracle: the plan
// conflicts exactly when the model, restricted to [from, to), differs between
// the snapshot and the head — a key changed, inserted or deleted. Both missed
// conflicts and false ones fail. The key alphabet puts keys on range bounds
// and between prefix neighbours ("b" < "b\0" < "b0" < "bb").
//
// The intervening writes land four ways:
//   published   plain writes, published before the plan is validated
//   same group  one batch in the same group commit as the plan, applied to
//               the pipeline head but not yet published
//   vacuum      published, then vacuum relocates keys: no change by itself
//   resume      the last write's fdatasync fails, resume() replays it

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "fault_injector.h"

import bytecask;

namespace {

struct TempDir {
  std::filesystem::path path;

  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "bc_range_test_{}_{}",
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

// Sorted bytewise. Range bounds are drawn from the same set plus "" and "f",
// so a bound often equals a key.
constexpr std::array<std::string_view, 8> kKeys{
    "a", "b", std::string_view{"b\0", 2}, "b0", "bb", "c", "d", "e"};

// key -> version. Every write gets a fresh version, so equal versions mean
// the same record, as equal sequences do in the engine.
using Model = std::map<std::string, std::uint64_t>;

auto slice(const Model &m, const std::string &from, const std::string &to)
    -> Model {
  return {m.lower_bound(from), m.lower_bound(to)};
}

enum class OpKind : std::uint8_t { Put, Del, DelRange };

struct Op {
  OpKind kind;
  std::string a; // key, or range from
  std::string b; // range to
};

void apply(Model &m, const Op &op, std::uint64_t &version) {
  switch (op.kind) {
  case OpKind::Put:
    m[op.a] = ++version;
    break;
  case OpKind::Del:
    m.erase(op.a);
    break;
  case OpKind::DelRange:
    m.erase(m.lower_bound(op.a), m.lower_bound(op.b));
    break;
  }
}

auto random_range(std::mt19937_64 &rng) -> std::pair<std::string, std::string> {
  std::vector<std::string> bounds{""};
  for (const auto k : kKeys) bounds.emplace_back(k);
  bounds.emplace_back("f");
  std::uniform_int_distribution<std::size_t> pick{0, bounds.size() - 1};
  for (;;) {
    auto from = pick(rng);
    auto to = pick(rng);
    if (from < to) return {bounds[from], bounds[to]};
  }
}

auto random_op(std::mt19937_64 &rng) -> Op {
  std::uniform_int_distribution<int> kind{0, 9};
  std::uniform_int_distribution<std::size_t> key{0, kKeys.size() - 1};
  const auto k = kind(rng);
  if (k < 6) return {OpKind::Put, std::string{kKeys[key(rng)]}, {}};
  if (k < 8) return {OpKind::Del, std::string{kKeys[key(rng)]}, {}};
  auto [from, to] = random_range(rng);
  return {OpKind::DelRange, from, to};
}

// One write per op; the value names the version so a mismatch is readable.
void write(bytecask::DB &db, const Op &op, std::uint64_t version, bool sync) {
  const auto v = std::format("v{}", version);
  switch (op.kind) {
  case OpKind::Put:
    db.put({.sync = sync}, to_bytes(op.a), to_bytes(v));
    break;
  case OpKind::Del:
    (void)db.del({.sync = sync}, to_bytes(op.a));
    break;
  case OpKind::DelRange:
    db.del_range({.sync = sync}, to_bytes(op.a), to_bytes(op.b));
    break;
  }
}

void add_to_plan(bytecask::WritePlan &plan, const Op &op, std::uint64_t version) {
  const auto v = std::format("v{}", version);
  switch (op.kind) {
  case OpKind::Put:
    plan.put(to_bytes(op.a), to_bytes(v));
    break;
  case OpKind::Del:
    plan.del(to_bytes(op.a));
    break;
  case OpKind::DelRange:
    plan.del_range(to_bytes(op.a), to_bytes(op.b));
    break;
  }
}

auto to_string(std::span<const std::byte> span) -> std::string {
  std::string s(span.size(), '\0');
  std::ranges::transform(span, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

auto db_contents(const bytecask::DB &db) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  for (auto &[k, v] : db.iter_from({})) out.emplace(to_string(k), to_string(v));
  return out;
}

auto model_contents(const Model &m) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  for (const auto &[k, ver] : m) out.emplace(k, std::format("v{}", ver));
  return out;
}

enum class Placement : std::uint8_t { Published, SameGroup, Vacuum, Resume };

auto placement_name(Placement p) -> std::string_view {
  switch (p) {
  case Placement::Published:
    return "published";
  case Placement::SameGroup:
    return "same group";
  case Placement::Vacuum:
    return "vacuum";
  case Placement::Resume:
    return "resume";
  }
  return "?";
}

// Plan shapes: a guard alone, a del_range alone, or both over the same range.
enum class PlanShape : std::uint8_t { Guard, DelRange, Both };

// Submits `first` and then `second` into one group commit: the batch that
// starts with `first` waits in the batch hook until the second slot is
// queued, so `second` is validated against a head holding `first`'s writes
// before either is published.
auto same_group(bytecask::DB &db, bytecask::WritePlan first,
                bytecask::WritePlan second)
    -> std::pair<std::optional<bytecask::CommitResult>,
                 std::optional<bytecask::CommitResult>> {
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
  std::optional<bytecask::CommitResult> ra;
  std::optional<bytecask::CommitResult> rb;
  std::thread ta{[&] { ra = db.apply_batch({.sync = false}, std::move(first)); }};
  std::thread tb{[&] {
    {
      std::unique_lock<std::mutex> lk{mu};
      cv.wait(lk, [&] { return leader_ready; });
    }
    rb = db.apply_batch({.sync = false}, std::move(second));
  }};
  ta.join();
  tb.join();
  db.test_write_group().on_batch_start_ = nullptr;
  return {ra, rb};
}

struct Counts {
  int conflicts{0};
  int commits{0};
  int vacuumed{0};
};

void run_rounds(Placement placement, std::uint64_t seed, int rounds,
                Counts &counts) {
  TempDir td;
  // Small files so vacuum has sealed files to relocate keys out of.
  auto db = bytecask::DB::open(td.path / "db", {.max_file_bytes = 512});
  std::mt19937_64 rng{seed};
  Model model;
  std::uint64_t version = 0;

  for (int round = 0; round < rounds; ++round) {
    INFO(std::format("placement {} seed {} round {}", placement_name(placement),
                     seed, round));

    // Background history the snapshot sees.
    for (int i = std::uniform_int_distribution<int>{0, 3}(rng); i > 0; --i) {
      const auto op = random_op(rng);
      apply(model, op, version);
      write(db, op, version, false);
    }

    auto snap = db.snapshot();
    const auto snap_model = model;

    const auto [from, to] = random_range(rng);
    const auto shape = static_cast<PlanShape>(
        std::uniform_int_distribution<int>{0, 2}(rng));
    bytecask::WritePlan plan{std::move(snap)};
    if (shape != PlanShape::DelRange)
      plan.ensure_range_unchanged(to_bytes(from), to_bytes(to));
    if (shape != PlanShape::Guard)
      plan.del_range(to_bytes(from), to_bytes(to));

    // Intervening writes. The same-group placement needs at least one, or
    // its batch writes nothing and never reaches the leader hook.
    const int min_ops = placement == Placement::Published ? 0 : 1;
    const auto n = std::uniform_int_distribution<int>{min_ops, 3}(rng);
    std::vector<std::pair<Op, std::uint64_t>> ops;
    for (int i = 0; i < n; ++i) {
      auto op = random_op(rng);
      // A del of an absent key writes nothing, so there is no fdatasync to
      // fail: the resume placement's last write is never a del.
      if (placement == Placement::Resume && i == n - 1 && op.kind == OpKind::Del)
        op.kind = OpKind::Put;
      apply(model, op, version);
      ops.emplace_back(op, version);
    }

    std::optional<bytecask::CommitResult> result;
    switch (placement) {
    case Placement::Published:
      for (const auto &[op, ver] : ops) write(db, op, ver, false);
      result = db.apply_batch({.sync = false}, std::move(plan));
      break;
    case Placement::SameGroup: {
      bytecask::WritePlan batch;
      for (const auto &[op, ver] : ops) add_to_plan(batch, op, ver);
      auto [ra, rb] = same_group(db, std::move(batch), std::move(plan));
      REQUIRE(ra.has_value());
      result = rb;
      break;
    }
    case Placement::Vacuum:
      for (const auto &[op, ver] : ops) write(db, op, ver, false);
      for (int i = 0; i < 4 && db.vacuum({.fragmentation_threshold = 0.0}); ++i)
        ++counts.vacuumed;
      result = db.apply_batch({.sync = false}, std::move(plan));
      break;
    case Placement::Resume: {
      for (std::size_t i = 0; i + 1 < ops.size(); ++i)
        write(db, ops[i].first, ops[i].second, false);
      {
        // The last write is appended in full; only its fdatasync fails.
        // resume() rewrites and syncs the file and replays it.
        bytecask::testing::ScopedFaultInjector fi{"io_data_file_sync"};
        REQUIRE_THROWS_AS(write(db, ops.back().first, ops.back().second, true),
                          std::system_error);
      }
      REQUIRE(db.is_degraded());
      db.resume();
      REQUIRE_FALSE(db.is_degraded());
      result = db.apply_batch({.sync = false}, std::move(plan));
      break;
    }
    }

    const bool expect_conflict =
        slice(snap_model, from, to) != slice(model, from, to);
    INFO(std::format("plan shape {} over [{}, {}), {} intervening writes",
                     static_cast<int>(shape), from, to, n));
    REQUIRE(result.has_value() == !expect_conflict);

    if (result) {
      ++counts.commits;
      if (shape != PlanShape::Guard)
        apply(model, {OpKind::DelRange, from, to}, version);
    } else {
      ++counts.conflicts;
    }
    REQUIRE(db_contents(db) == model_contents(model));
  }
  db.close();
}

} // namespace

TEST_CASE("range conflicts match a model of the key range",
          "[model][range_conflict][del_range][guards]") {
  const auto placement = GENERATE(Placement::Published, Placement::SameGroup,
                                  Placement::Vacuum, Placement::Resume);
  Counts counts;
  const int rounds = placement == Placement::Published ? 400 : 150;
  for (std::uint64_t seed = 1; seed <= 4; ++seed)
    run_rounds(placement, seed, rounds, counts);

  // A run where the oracle never said "conflict", or never said "commit",
  // proved nothing in that direction.
  INFO(std::format("{}: {} conflicts, {} commits, {} vacuums",
                   placement_name(placement), counts.conflicts, counts.commits,
                   counts.vacuumed));
  CHECK(counts.conflicts > 0);
  CHECK(counts.commits > 0);
  if (placement == Placement::Vacuum) CHECK(counts.vacuumed > 0);
}
