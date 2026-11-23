// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// MIN and MAX of text in an aggregate's slot (#904, stage M1;
// docs/planner_design.md, *MIN and MAX of text*): bytewise order, NULL until
// a value is folded, a held value replaced by a lesser or greater one, a read
// that is a copy, a clear that releases what was held, slot kinds checked;
// and every transition of the held values charged exactly, and ended by a
// budget or an allocation failure with nothing lost from the accounting.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::kAggHeldBytes;
using bytecask::sql::kAllocBytes;
using bytecask::sql::PhysType;

namespace {

// Each call of text_slots that changes what the aggregate holds.
enum class Step : std::uint8_t {
  First,       // a slot's first value: the vector of held values made
  Second,      // another slot's first: the vector grows
  Longer,      // a replacement longer than the value held
  Shorter,     // a replacement shorter than it
  Read,        // bc_agg_bytes, a copy in the arena
  AfterClear,  // the first value after a clear, as a stream's next group: the vector made again
};
constexpr std::size_t kSteps = 6;

// What text_slots saw, for the test to read: the step an error ended, and
// whether the statement's charge was then what it was before the call; the
// change in the charge each step made on a run with no error; the charge
// before the clear, and after it with the group made again.
struct Trace {
  std::optional<Step> ended;
  bool kept_charge{true};
  std::array<std::int64_t, kSteps> delta{};
  std::uint64_t opened{0}, reopened{0};
};
Trace g_trace;  // the tests run one plan at a time

// One group of two slots, MIN in 0 and MAX in 1, over $1 = "m", $2 = 40
// a's and $3 = "A": MIN m, then the longer a's, then the shorter A; MAX m,
// and A, which changes nothing. The two read and emitted; a clear; MAX of
// the a's, read and emitted with MIN NULL.
void text_slots(bc_ctx* cx) {
  auto step = [cx](Step s, const auto& call) {
    if (bc_error(cx) != BC_OK) return;
    const auto before = bc_ctx_held_for_testing(cx);
    call();
    const auto after = bc_ctx_held_for_testing(cx);
    if (bc_error(cx) != BC_OK) {
      g_trace.ended = s;
      // A read's copy is the arena's, charged and held until the step ends,
      // whatever the call then fails on.
      g_trace.kept_charge = s == Step::Read || after == before;
      return;
    }
    g_trace.delta.at(static_cast<std::size_t>(s)) = static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before);
  };
  // The values' views first, so a step's charge is the aggregate's alone.
  const bc_view m = bc_param_bytes(cx, 0);
  const bc_view as = bc_param_bytes(cx, 1);
  const bc_view upper_a = bc_param_bytes(cx, 2);
  bc_agg a = bc_agg_open(cx, 2);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  step(Step::First, [&] { bc_agg_min_bytes(cx, g, 0, m); });
  step(Step::Second, [&] { bc_agg_max_bytes(cx, g, 1, m); });
  step(Step::Longer, [&] { bc_agg_min_bytes(cx, g, 0, as); });
  step(Step::Shorter, [&] { bc_agg_min_bytes(cx, g, 0, upper_a); });
  bc_agg_max_bytes(cx, g, 1, upper_a);
  bc_view lo{};
  step(Step::Read, [&] { lo = bc_agg_bytes(cx, g, 0); });
  bc_emit_begin(cx);
  bc_emit_bytes(cx, lo);
  bc_emit_bytes(cx, bc_agg_bytes(cx, g, 1));
  bc_emit_end(cx);
  g_trace.opened = bc_ctx_held_for_testing(cx);
  bc_agg_clear(cx, a);
  g = bc_agg_group(cx, a, bc_empty(cx));
  g_trace.reopened = bc_ctx_held_for_testing(cx);
  step(Step::AfterClear, [&] { bc_agg_max_bytes(cx, g, 1, as); });
  bc_emit_begin(cx);
  if (bc_agg_null(cx, g, 0)) {
    bc_emit_null(cx);
  } else {
    bc_emit_bytes(cx, bc_agg_bytes(cx, g, 0));
  }
  bc_emit_bytes(cx, bc_agg_bytes(cx, g, 1));
  bc_emit_end(cx);
}

constexpr std::string_view kLong = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 40
constexpr std::array kTwo{PhysType::Bytes, PhysType::Bytes};

auto run_slots(Fixture& f, bc_budget budget = {}) -> int {
  g_trace = {};
  auto ps = params({bytes("m"), bytes(std::string{kLong}), bytes("A")});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(kTwo), f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, text_slots, &ref, &ps, budget, false, 0);
}

// Bytewise, as bc_bytes_cmp and sort keys order: é (0xC3 0xA9) after z,
// a after B; the empty string before all.
void order(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 2);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  for (uint32_t i = 0; i < 5; ++i) {
    bc_agg_min_bytes(cx, g, 0, bc_param_bytes(cx, i));
    bc_agg_max_bytes(cx, g, 1, bc_param_bytes(cx, i));
  }
  bc_emit_begin(cx);
  bc_emit_bytes(cx, bc_agg_bytes(cx, g, 0));
  bc_emit_bytes(cx, bc_agg_bytes(cx, g, 1));
  bc_emit_end(cx);
}

// A copy outlives the value it was read from: MIN "b" read, then "a" folded.
void read_then_replace(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_min_bytes(cx, g, 0, bc_param_bytes(cx, 0));
  bc_view before = bc_agg_bytes(cx, g, 0);
  bc_agg_min_bytes(cx, g, 0, bc_param_bytes(cx, 1));
  bc_emit_begin(cx);
  bc_emit_bytes(cx, before);
  bc_emit_bytes(cx, bc_agg_bytes(cx, g, 0));
  bc_emit_end(cx);
}

// Wrong plans: a bytes slot read as an integer, an integer slot folded as
// bytes, MIN then MAX of bytes in one slot, a group dropped by a clear.
void bytes_read_as_integer(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_min_bytes(cx, g, 0, bc_param_bytes(cx, 0));
  bc_emit_count(cx, bc_agg_i64(cx, g, 0));
}
void integer_read_as_bytes(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_min_i64(cx, g, 0, 1);
  (void)bc_agg_bytes(cx, g, 0);
}
void integer_then_bytes(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_max_i64(cx, g, 0, 1);
  bc_agg_max_bytes(cx, g, 0, bc_param_bytes(cx, 0));
}
void min_then_max(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_min_bytes(cx, g, 0, bc_param_bytes(cx, 0));
  bc_agg_max_bytes(cx, g, 0, bc_param_bytes(cx, 0));
}
void cleared_group(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_min_bytes(cx, g, 0, bc_param_bytes(cx, 0));
  bc_agg_clear(cx, a);
  (void)bc_agg_bytes(cx, g, 0);
}

auto run_plan(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, std::span<const PhysType> types) -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(types), f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, plan, &ref, &ps, {}, false, 0);
}

}  // namespace

TEST_CASE("plan API: MIN and MAX of bytes fold bytewise, NULL until a value, a read a copy",
          "[sql][algebra][structures]") {
  Fixture f;
  using R = std::vector<bc_value>;
  REQUIRE(run_plan(f, order, {bytes("z"), bytes("é"), bytes("B"), bytes("a"), bytes("")}, kTwo) == BC_COMMITTED);
  CHECK(f.results(kTwo) == std::vector<R>{R{bytes(""), bytes("é")}});
  // A prefix orders first.
  REQUIRE(run_plan(f, order, {bytes("ab"), bytes("abc"), bytes("a"), bytes("abd"), bytes("b")}, kTwo) == BC_COMMITTED);
  CHECK(f.results(kTwo) == std::vector<R>{R{bytes("a"), bytes("b")}});
  REQUIRE(run_plan(f, read_then_replace, {bytes("b"), bytes("a")}, kTwo) == BC_COMMITTED);
  CHECK(f.results(kTwo) == std::vector<R>{R{bytes("b"), bytes("a")}});
  REQUIRE(run_slots(f) == BC_COMMITTED);
  CHECK(f.results(kTwo) == std::vector<R>{R{bytes("A"), bytes("m")}, R{bc_value{}, bytes(std::string{kLong})}});

  for (auto plan : {bytes_read_as_integer, integer_read_as_bytes, integer_then_bytes, min_then_max, cleared_group}) {
    CHECK(run_plan(f, plan, {bytes("x")}, std::array{PhysType::Bytes}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_HANDLE);
  }
}

TEST_CASE("plan API: a held MIN or MAX of bytes is charged its bytes, released by a clear, and past the budget fails "
          "keeping the value held and charged",
          "[sql][algebra][memory]") {
  Fixture f;
  REQUIRE(run_slots(f) == BC_COMMITTED);
  const auto made = static_cast<std::int64_t>(kAllocBytes + sizeof(std::vector<Bytes>));
  const auto held = static_cast<std::int64_t>(kAggHeldBytes);
  const auto n = static_cast<std::int64_t>(kLong.size());
  auto at = [](Step s) { return g_trace.delta.at(static_cast<std::size_t>(s)); };
  CHECK(at(Step::First) == made + held + 1);
  CHECK(at(Step::Second) == held + 1);
  CHECK(at(Step::Longer) == n - 1);
  CHECK(at(Step::Shorter) == 1 - n);
  CHECK(at(Step::AfterClear) == made + held + n);
  // The clear released the two held values and the vector; the group made
  // again is charged what the dropped one was.
  CHECK(g_trace.opened - g_trace.reopened == static_cast<std::uint64_t>(made + 2 * (held + 1)));
  bc_ctx_reset(f.cx);
  CHECK(bc_ctx_held_for_testing(f.cx) == 0);

  // Every budget below the one that suffices fails with BC_E_LIMIT, the
  // call it ends leaving the charge as it was: the old value held.
  std::set<Step> ended;
  for (std::uint64_t budget = 1;; ++budget) {
    REQUIRE(budget < 100000);
    INFO("budget " << budget);
    auto r = run_slots(f, {.max_reads = 0, .max_bytes = budget, .deadline_ns = 0, .cancel = nullptr});
    if (r == BC_COMMITTED) break;
    CHECK(bc_error(f.cx) == BC_E_LIMIT);
    if (g_trace.ended) {
      ended.insert(*g_trace.ended);
      CHECK(g_trace.kept_charge);
    }
    bc_ctx_reset(f.cx);
    CHECK(bc_ctx_held_for_testing(f.cx) == 0);
  }
  // The budget's edge passes through each transition that raises the
  // statement's peak. A shorter replacement's peak, the held value and the
  // new one, is below the longer one's before it: the allocation sweep
  // below reaches it.
  CHECK(ended == std::set<Step>{Step::First, Step::Second, Step::Longer, Step::Read});
}

TEST_CASE("plan API: an allocation failure in each transition of a held MIN or MAX of bytes is BC_E_MEMORY, with "
          "the charge as it was",
          "[sql][algebra][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Fixture f;
  // The sweep must reach every transition: each must be the call an
  // injected failure ended (tests/CLAUDE.md, *Fault sweeps must reach every
  // structural transition*).
  std::set<Step> ended;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 100000);
    INFO("allocation " << nth);
    g_trace = {};
    auto ps = params({bytes("m"), bytes(std::string{kLong}), bytes("A")});
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(kTwo), f.cat().version()};
    auto ref = sr.get();
    // A context of its own, so each run makes the same allocations: a
    // reset context keeps its vectors' capacity.
    auto* cx = bc_ctx_new(&f.bdb, &f.bcat);
    bc_ctx_begin(cx, &ps, {});
    auto threw = false;
    bytecask::testing::AllocFaultReport report;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      try {
        bc_ctx_run(cx, text_slots, &ref);
      } catch (...) {
        threw = true;
      }
      report = faults.report();
    }
    CHECK_FALSE(threw);
    const auto error = bc_error(cx);
    if (!report.fired) {
      CHECK(error == BC_OK);
      bc_ctx_free(cx);
      break;
    }
    CHECK((error == BC_E_MEMORY || error == BC_OK));
    if (g_trace.ended) {
      ended.insert(*g_trace.ended);
      CHECK(g_trace.kept_charge);
    }
    bc_ctx_reset(cx);
    CHECK(bc_ctx_held_for_testing(cx) == 0);
    bc_ctx_free(cx);
  }
  CHECK(ended == std::set<Step>{Step::First, Step::Second, Step::Longer, Step::Shorter, Step::Read, Step::AfterClear});
#endif
}
