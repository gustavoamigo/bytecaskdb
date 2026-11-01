// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Unit tests of the DeleteLog and StagedRemovals (bytecaskdb/delete_log.cppm;
// docs/bytecask_design.md, "Checking a plan by its read sequence"). A delete
// names a key: a del by the key, a del_range by its bounds, whatever it
// covered. A query asks for a delete above the read sequence and below the
// next sequence of the state checked. The cases drive each condition of the
// file's decisions both ways, for the MC/DC gate (scripts/run_coverage.sh).

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import bytecask;

namespace {

auto b(std::string_view s) -> std::span<const std::byte> {
  return std::as_bytes(std::span{s.data(), s.size()});
}

constexpr std::size_t kBig = std::size_t{1} << 40;

void del(bytecask::DeleteLog &log, std::string_view key, std::uint64_t seq) {
  bytecask::StagedRemovals staged;
  staged.add_key(b(key), seq);
  log.record(staged);
}

void del_range(bytecask::DeleteLog &log, std::string_view from,
               std::string_view to, std::uint64_t seq) {
  bytecask::StagedRemovals staged;
  staged.add_range(b(from), b(to), seq);
  log.record(staged);
}

// The budget two one-byte dels fill exactly.
auto log_bytes_for_two() -> std::size_t {
  bytecask::DeleteLog probe{kBig};
  del(probe, "x", 1);
  del(probe, "y", 2);
  return probe.bytes();
}

} // namespace

TEST_CASE("delete log: a del names its key inside the window only",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  CHECK(log.removed(b("k"), 10, 100));
  CHECK_FALSE(log.removed(b("k"), 20, 100));  // at or below the read
  CHECK_FALSE(log.removed(b("j"), 10, 100));  // another key
  CHECK_FALSE(log.removed(b("k"), 10, 20));   // a later state's delete
}

TEST_CASE("delete log: a key deleted again past the state checked still answers from its earlier delete",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  del(log, "other", 30);
  del(log, "k", 40);
  // The latest delete of k is past `below`; the earlier one is in the window.
  CHECK(log.removed(b("k"), 10, 35));
  CHECK_FALSE(log.removed(b("k"), 25, 35));
  CHECK(log.removed_in(b("j"), b("l"), 10, 35));
  CHECK_FALSE(log.removed_in(b("j"), b("l"), 25, 35));
}

TEST_CASE("delete log: a del_range names every key it covers",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del_range(log, "c", "f", 20);
  CHECK(log.removed(b("c"), 10, 100));
  CHECK(log.removed(b("e9"), 10, 100));
  CHECK_FALSE(log.removed(b("b"), 10, 100));  // before from
  CHECK_FALSE(log.removed(b("f"), 10, 100));  // to is exclusive
  CHECK_FALSE(log.removed(b("d"), 20, 100));  // at or below the read
  CHECK_FALSE(log.removed(b("d"), 10, 20));   // a later state's delete
}

TEST_CASE("delete log: a range query finds the deletes that overlap it",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  del_range(log, "c", "f", 30);
  CHECK(log.removed_in(b("j"), b("l"), 10, 100));   // the del
  CHECK_FALSE(log.removed_in(b("l"), b("z"), 10, 100));
  CHECK_FALSE(log.removed_in(b("j"), b("l"), 20, 100));
  CHECK(log.removed_in(b("a"), b("d"), 25, 100));   // the range, from below
  CHECK(log.removed_in(b("e"), b("g"), 25, 100));   // the range, from above
  CHECK_FALSE(log.removed_in(b("a"), b("c"), 25, 100));  // ends at its from
  CHECK_FALSE(log.removed_in(b("f"), b("g"), 25, 100));  // starts at its to
  CHECK_FALSE(log.removed_in(b("a"), b("z"), 30, 100));  // all at or below
  CHECK_FALSE(log.removed_in(b("a"), b("z"), 10, 20));   // all later
}

TEST_CASE("delete log: past its budget the oldest delete goes first, point or range",
          "[delete_log]") {
  // Small enough that a third delete pushes the first out.
  const std::size_t one = 100;
  SECTION("an older del before a newer del_range") {
    bytecask::DeleteLog log{2 * one};
    del(log, "a", 10);
    del_range(log, "b", "c", 20);
    del(log, "d", 30);
    CHECK(log.horizon() >= 10);
    CHECK_FALSE(log.covers(9));
    CHECK(log.bytes() <= 2 * one);
  }
  SECTION("an older del_range before a newer del") {
    bytecask::DeleteLog log{2 * one};
    del_range(log, "b", "c", 10);
    del(log, "a", 20);
    del_range(log, "d", "e", 30);
    CHECK(log.horizon() >= 10);
    CHECK(log.bytes() <= 2 * one);
  }
  SECTION("dels only") {
    bytecask::DeleteLog log{one};
    del(log, "a", 10);
    del(log, "a", 20);
    del(log, "b", 30);
    CHECK(log.horizon() >= 20);
    CHECK(log.covers(log.horizon()));
    CHECK_FALSE(log.removed(b("a"), log.horizon(), 100));
  }
  SECTION("del_ranges only") {
    bytecask::DeleteLog log{one};
    del_range(log, "a", "b", 10);
    del_range(log, "c", "d", 20);
    CHECK(log.horizon() >= 10);
    CHECK(log.bytes() <= one);
  }
}

TEST_CASE("delete log: with no budget nothing is kept and every delete raises the horizon",
          "[delete_log]") {
  bytecask::DeleteLog log{0};
  del(log, "k", 20);
  CHECK(log.bytes() == 0);
  CHECK(log.horizon() == 20);
  del_range(log, "a", "z", 30);
  CHECK(log.bytes() == 0);
  CHECK(log.horizon() == 30);
  CHECK_FALSE(log.covers(29));
  CHECK(log.covers(30));
}

TEST_CASE("delete log: reset forgets every delete and raises the horizon",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  del_range(log, "a", "b", 30);
  log.reset(40);
  CHECK(log.bytes() == 0);
  CHECK(log.horizon() == 40);
  CHECK_FALSE(log.removed(b("k"), 10, 100));
  log.reset(5);  // never lowers it
  CHECK(log.horizon() == 40);
}

TEST_CASE("staged removals: a group's later slots see its earlier deletes",
          "[delete_log]") {
  bytecask::StagedRemovals staged;
  CHECK(staged.empty());
  CHECK_FALSE(staged.removes(b("k")));
  CHECK_FALSE(staged.removes_any_in(b("a"), b("z")));
  staged.add_key(b("k"), 20);
  staged.add_range(b("c"), b("f"), 21);
  CHECK_FALSE(staged.empty());
  CHECK(staged.removes(b("k")));
  CHECK(staged.removes(b("d")));
  CHECK_FALSE(staged.removes(b("f")));
  CHECK(staged.removes_any_in(b("j"), b("l")));   // the del
  CHECK_FALSE(staged.removes_any_in(b("g"), b("j")));  // between the two
  CHECK_FALSE(staged.removes_any_in(b("l"), b("z")));  // past every del
  CHECK(staged.removes_any_in(b("a"), b("d")));   // the range
  CHECK_FALSE(staged.removes_any_in(b("f"), b("g")));

  // Recording takes them out.
  bytecask::DeleteLog log{kBig};
  log.record(staged);
  CHECK(staged.empty());
  CHECK_FALSE(staged.removes(b("k")));
  CHECK(log.removed(b("k"), 10, 100));
  CHECK(log.removed(b("d"), 10, 100));
}

TEST_CASE("staged removals: a key deleted twice in a group is logged by its later delete",
          "[delete_log]") {
  bytecask::StagedRemovals staged;
  staged.add_key(b("k"), 20);
  staged.add_key(b("j"), 21);
  staged.add_key(b("k"), 22);
  bytecask::DeleteLog log{kBig};
  log.record(staged);
  CHECK(log.removed(b("k"), 21, 100));
  CHECK_FALSE(log.removed(b("j"), 21, 100));
}

TEST_CASE("staged removals: a group is logged in sequence order, not key order",
          "[delete_log]") {
  // Two dels fill the budget exactly; a third drops the oldest, which is
  // z's although a sorts first.
  bytecask::StagedRemovals staged;
  staged.add_key(b("z"), 20);
  staged.add_key(b("a"), 21);
  bytecask::DeleteLog log{log_bytes_for_two()};
  log.record(staged);
  del(log, "m", 30);
  CHECK(log.horizon() == 20);
  CHECK(log.removed(b("a"), 20, 100));
}
