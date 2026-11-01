// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Unit tests of the DeleteLog and StagedRemovals (bytecaskdb/delete_log.cppm;
// docs/bytecask_design.md, "Checking a plan by its read sequence"). A delete
// names a key: a del by the key, a del_range by its bounds, whatever it
// covered. A query asks for a delete above the read sequence and below the
// next sequence of the state checked. The cases drive each condition of the
// file's decisions both ways, for the MC/DC gate (scripts/run_coverage.sh).

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bytecask;

namespace {

auto b(std::string_view s) -> std::span<const std::byte> {
  return std::as_bytes(std::span{s.data(), s.size()});
}

// No budget limit, on 32-bit targets (the wasm build) too.
constexpr std::size_t kBig = std::numeric_limits<std::size_t>::max();

// Records `staged` as a group that ends at `last` and starts right after
// the last sequence recorded: the sequences between were writes that
// deleted nothing.
void record_through(bytecask::DeleteLog &log, bytecask::StagedRemovals &staged,
                    std::uint64_t last) {
  log.record(staged, log.recorded_through() + 1, last);
}

void del(bytecask::DeleteLog &log, std::string_view key, std::uint64_t seq) {
  bytecask::StagedRemovals staged;
  staged.add_key(b(key), seq);
  record_through(log, staged, seq);
}

void del_range(bytecask::DeleteLog &log, std::string_view from,
               std::string_view to, std::uint64_t seq) {
  bytecask::StagedRemovals staged;
  staged.add_range(b(from), b(to), seq);
  record_through(log, staged, seq);
}

// Writes that delete nothing, up to the state the queries check (below 100).
void fill(bytecask::DeleteLog &log, std::uint64_t last = 99) {
  bytecask::StagedRemovals none;
  record_through(log, none, last);
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
  fill(log);
  CHECK(log.removed(b("k"), 10, 100) == true);
  CHECK(log.removed(b("k"), 20, 100) == false);  // at or below the read
  CHECK(log.removed(b("j"), 10, 100) == false);  // another key
  CHECK(log.removed(b("k"), 10, 20) == false);   // a later state's delete
}

TEST_CASE("delete log: a key deleted again past the state checked still answers from its earlier delete",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  del(log, "other", 30);
  del(log, "k", 40);
  // The latest delete of k is past `below`; the earlier one is in the window.
  CHECK(log.removed(b("k"), 10, 35) == true);
  CHECK(log.removed(b("k"), 25, 35) == false);
  CHECK(log.removed_in(b("j"), b("l"), 10, 35) == true);
  CHECK(log.removed_in(b("j"), b("l"), 25, 35) == false);
}

TEST_CASE("delete log: a del_range names every key it covers",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del_range(log, "c", "f", 20);
  fill(log);
  CHECK(log.removed(b("c"), 10, 100) == true);
  CHECK(log.removed(b("e9"), 10, 100) == true);
  CHECK(log.removed(b("b"), 10, 100) == false);  // before from
  CHECK(log.removed(b("f"), 10, 100) == false);  // to is exclusive
  CHECK(log.removed(b("d"), 20, 100) == false);  // at or below the read
  CHECK(log.removed(b("d"), 10, 20) == false);   // a later state's delete
}

TEST_CASE("delete log: a range query finds the deletes that overlap it",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  del_range(log, "c", "f", 30);
  fill(log);
  CHECK(log.removed_in(b("j"), b("l"), 10, 100) == true);   // the del
  CHECK(log.removed_in(b("l"), b("z"), 10, 100) == false);
  CHECK(log.removed_in(b("j"), b("l"), 20, 100) == false);
  CHECK(log.removed_in(b("a"), b("d"), 25, 100) == true);   // the range, from below
  CHECK(log.removed_in(b("e"), b("g"), 25, 100) == true);   // the range, from above
  CHECK(log.removed_in(b("a"), b("c"), 25, 100) == false);  // ends at its from
  CHECK(log.removed_in(b("f"), b("g"), 25, 100) == false);  // starts at its to
  CHECK(log.removed_in(b("a"), b("z"), 30, 100) == false);  // all at or below
  CHECK(log.removed_in(b("a"), b("z"), 10, 20) == false);   // all later
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
    fill(log);
    CHECK(log.horizon() >= 10);
    CHECK_FALSE(log.removed(b("a"), 9, 100).has_value());
    CHECK_FALSE(log.removed_in(b("a"), b("z"), 9, 100).has_value());
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
    fill(log);
    CHECK(log.horizon() >= 20);
    CHECK(log.removed(b("a"), log.horizon(), 100).has_value());
    CHECK(log.removed(b("a"), log.horizon(), 100) == false);
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
  fill(log);
  CHECK_FALSE(log.removed(b("k"), 29, 100).has_value());
  CHECK(log.removed(b("k"), 30, 100) == false);
  CHECK_FALSE(log.removed_in(b("a"), b("z"), 29, 100).has_value());
  CHECK(log.removed_in(b("a"), b("z"), 30, 100) == false);
}

// docs/bytecask_design.md, "Checking a plan by its read sequence": the log
// answers only for sequences it was handed.
TEST_CASE("delete log: a group that does not follow the last recorded one forgets everything older",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  del_range(log, "a", "b", 30);
  // Sequences 31 to 39 came from elsewhere (an ingest, a marker).
  bytecask::StagedRemovals none;
  log.record(none, 40, 45);
  CHECK(log.bytes() == 0);
  CHECK(log.horizon() == 39);
  CHECK(log.recorded_through() == 45);
  CHECK_FALSE(log.removed(b("k"), 10, 46).has_value());  // forgotten
  CHECK(log.removed(b("k"), 39, 46) == false);
  // A group that reuses sequences (after resume() cut a tail) is one too.
  del(log, "k", 50);
  log.record(none, 48, 60);
  CHECK(log.horizon() == 47);
  CHECK(log.removed(b("k"), 47, 61) == false);
}

TEST_CASE("delete log: a state past the last recorded sequence is a gap",
          "[delete_log]") {
  bytecask::DeleteLog log{kBig};
  del(log, "k", 20);
  CHECK(log.removed(b("k"), 10, 21) == true);
  // The state checked holds sequences 21 to 24, which no group recorded.
  CHECK_FALSE(log.removed(b("k"), 10, 25).has_value());
  CHECK_FALSE(log.removed_in(b("a"), b("z"), 10, 25).has_value());
  CHECK(log.removed(b("k"), 24, 25) == false);  // nothing above the read
  // A group that deleted nothing still closes the gap.
  fill(log, 24);
  CHECK(log.removed(b("k"), 10, 25) == true);
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
  record_through(log, staged, 99);
  CHECK(staged.empty());
  CHECK_FALSE(staged.removes(b("k")));
  CHECK(log.removed(b("k"), 10, 100) == true);
  CHECK(log.removed(b("d"), 10, 100) == true);
}

TEST_CASE("staged removals: a key deleted twice in a group is logged by its later delete",
          "[delete_log]") {
  bytecask::StagedRemovals staged;
  staged.add_key(b("k"), 20);
  staged.add_key(b("j"), 21);
  staged.add_key(b("k"), 22);
  bytecask::DeleteLog log{kBig};
  record_through(log, staged, 99);
  CHECK(log.removed(b("k"), 21, 100) == true);
  CHECK(log.removed(b("j"), 21, 100) == false);
}

TEST_CASE("staged removals: a group is logged in sequence order, not key order",
          "[delete_log]") {
  // Two dels fill the budget exactly; a third drops the oldest, which is
  // z's although a sorts first.
  bytecask::StagedRemovals staged;
  staged.add_key(b("z"), 20);
  staged.add_key(b("a"), 21);
  bytecask::DeleteLog log{log_bytes_for_two()};
  record_through(log, staged, 21);
  del(log, "m", 30);
  fill(log);
  CHECK(log.horizon() == 20);
  CHECK(log.removed(b("a"), 20, 100) == true);
}

TEST_CASE("range segments: a stamp covers its keys and replaces older stamps where it lies",
          "[delete_log]") {
  bytecask::RangeSegments segs;
  CHECK(segs.latest_at(b("k")) == 0);
  segs.stamp(b("a"), b("m"), 10);
  segs.stamp(b("f"), b("z"), 20);
  CHECK(segs.latest_at(b("a")) == 10);
  CHECK(segs.latest_at(b("e")) == 10);
  CHECK(segs.latest_at(b("f")) == 20);
  CHECK(segs.latest_at(b("y")) == 20);
  CHECK(segs.latest_at(b("z")) == 0);  // to is exclusive
  CHECK(segs.latest_at(b("0")) == 0);  // before every segment
  segs.stamp(b("c"), b("d"), 30);      // inside [a, f): splits it in three
  CHECK(segs.latest_at(b("b")) == 10);
  CHECK(segs.latest_at(b("c")) == 30);
  CHECK(segs.latest_at(b("d")) == 10);

  // Oldest first: removing 10 leaves what 30 and 20 cover.
  segs.unstamp(b("a"), b("m"), 10);
  CHECK(segs.latest_at(b("b")) == 0);
  CHECK(segs.latest_at(b("c")) == 30);
  CHECK(segs.latest_at(b("d")) == 0);
  CHECK(segs.latest_at(b("g")) == 20);
  segs.unstamp(b("f"), b("z"), 20);
  segs.unstamp(b("c"), b("d"), 30);
  CHECK(segs.bytes() == 0);
}

TEST_CASE("range segments: a range query sees exactly the segments it overlaps",
          "[delete_log]") {
  bytecask::RangeSegments segs;
  segs.stamp(b("c"), b("f"), 10);
  segs.stamp(b("m"), b("p"), 20);
  const auto stamps = [&](std::string_view from, std::string_view to) {
    std::vector<std::uint64_t> out;
    (void)segs.any_overlapping(b(from), b(to), [&](std::uint64_t s) {
      out.push_back(s);
      return false;
    });
    return out;
  };
  CHECK(stamps("a", "c").empty());   // ends at its from
  CHECK(stamps("f", "m").empty());   // between the two
  CHECK(stamps("p", "z").empty());   // starts at the last one's to
  CHECK(stamps("d", "e") == std::vector<std::uint64_t>{10});  // inside one
  CHECK(stamps("a", "d") == std::vector<std::uint64_t>{10});
  CHECK(stamps("e", "n") == std::vector<std::uint64_t>{10, 20});
  CHECK(segs.any_overlapping(b("e"), b("n"),
                             [](std::uint64_t s) { return s == 20; }));
}

TEST_CASE("delete log: answers like a list of every delete, whatever the budget",
          "[delete_log][model]") {
  // The oracle keeps every delete and scans them all; the log may drop the
  // oldest, so it is asked only where it covers the read sequence. A state
  // checked holds whole groups, so `below` is a group boundary: the log
  // keeps a key's last delete in each group.
  struct Delete {
    std::string from;
    std::string to;  // empty: a del of `from`
    std::uint64_t seq;
  };
  const std::array<std::string_view, 8> keys{"a", "b", "b0", "c", "d", "e", "f", "g"};
  const auto names = [](const Delete &d, const std::string &key) {
    return d.to.empty() ? d.from == key : d.from <= key && key < d.to;
  };
  const auto overlaps = [](const Delete &d, const std::string &from,
                           const std::string &to) {
    return d.to.empty() ? from <= d.from && d.from < to
                        : d.from < to && from < d.to;
  };
  const auto budget = GENERATE(std::size_t{300}, std::size_t{2000}, kBig);
  std::mt19937_64 rng{budget};
  const auto pick = [&] {
    return std::string{keys[std::uniform_int_distribution<std::size_t>{
        0, keys.size() - 1}(rng)]};
  };
  bytecask::DeleteLog log{budget};
  std::vector<Delete> all;
  std::vector<std::uint64_t> boundaries;  // each group's next sequence
  std::uint64_t seq = 0;
  std::uint64_t last_gap = 0;  // the newest sequence no group recorded
  for (int group = 0; group < 400; ++group) {
    if (rng() % 10 == 0) {
      // Sequences assigned elsewhere (an ingest, a marker): the log never
      // sees them, and cannot answer across them.
      seq += std::uniform_int_distribution<std::uint64_t>{1, 3}(rng);
      last_gap = seq;
      boundaries.push_back(seq + 1);
    }
    const auto first = seq + 1;
    bytecask::StagedRemovals staged;
    for (int i = std::uniform_int_distribution<int>{1, 3}(rng); i > 0; --i) {
      ++seq;
      auto k = pick();
      if (rng() % 3 == 0) {
        auto j = pick();
        if (j == k) j += "~";
        const auto [from, to] = std::minmax(k, j);
        staged.add_range(b(from), b(to), seq);
        all.push_back({from, to, seq});
      } else {
        staged.add_key(b(k), seq);
        all.push_back({k, {}, seq});
      }
    }
    log.record(staged, first, seq);
    boundaries.push_back(seq + 1);
    for (int q = 0; q < 8; ++q) {
      const auto read_seq = std::uniform_int_distribution<std::uint64_t>{0, seq}(rng);
      // Below a gap the log must not answer, whatever it still holds.
      if (read_seq < last_gap) CHECK(log.horizon() >= last_gap);
      if (read_seq < log.horizon()) {
        // Below the horizon a delete in the window may have been dropped:
        // no answer, rather than a wrong one.
        CHECK_FALSE(log.removed(b(pick()), read_seq, seq + 1).has_value());
        CHECK_FALSE(log.removed_in(b("a"), b("~"), read_seq, seq + 1).has_value());
        continue;
      }
      const auto later = std::ranges::upper_bound(boundaries, read_seq);
      const auto below = *std::next(
          later, std::uniform_int_distribution<std::ptrdiff_t>{
                     0, std::distance(later, boundaries.end()) - 1}(rng));
      const auto in_window = [&](const Delete &d) {
        return d.seq > read_seq && d.seq < below;
      };
      const auto key = pick();
      INFO(std::format("group {} key {} read {} below {}", group, key, read_seq, below));
      CHECK(log.removed(b(key), read_seq, below) ==
            std::ranges::any_of(all, [&](const Delete &d) {
              return in_window(d) && names(d, key);
            }));
      auto from = pick();
      auto to = pick();
      if (to == from) to += "~";
      if (to < from) std::swap(from, to);
      INFO(std::format("range [{}, {})", from, to));
      CHECK(log.removed_in(b(from), b(to), read_seq, below) ==
            std::ranges::any_of(all, [&](const Delete &d) {
              return in_window(d) && overlaps(d, from, to);
            }));
    }
    CHECK(log.bytes() <= budget);
  }
}
