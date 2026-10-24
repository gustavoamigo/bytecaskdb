// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// No exception crosses into a plan: every allocation a plan API call makes
// is failed in turn (tests/alloc_faults.h, the engine's counted
// allocation-failure seam), and each failure must come back as the
// context's sticky error, BC_E_MEMORY, never as an exception out of the
// call. A real plan is TinyCC code without unwind tables, where an
// exception terminates the process; these plans are C++, so an escape
// shows here as a throw out of bc_ctx_run. A commit whose allocations
// fail never reports BC_COMMITTED with an error set, and a verification
// that fails part-way leaves the context's verified reference to be rebuilt
// whole by the next run, whose rows are checked.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::PhysType;

namespace {

constexpr std::uint32_t kTbl = 1;
constexpr std::uint32_t kIdx = 3;
constexpr std::uint32_t kIdCol = 0, kNameCol = 1, kQtyCol = 2;

// INSERT INTO t VALUES ($1, $2, $3)
void insert_row(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_row_set_param(cx, r, kNameCol, 1);
  bc_row_set_param(cx, r, kQtyCol, 2);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
}
// SELECT id, name, qty FROM t WHERE id = $1: bc_emit_col.
void emit_row(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  if (!found) return;
  bc_emit_begin(cx);
  bc_emit_col(cx, row, kIdCol);
  bc_emit_col(cx, row, kNameCol);
  bc_emit_col(cx, row, kQtyCol);
  bc_emit_end(cx);
}
// SELECT name FROM t ORDER BY name, from the index key: bc_emit_key_part.
void emit_names(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view key = bc_scan_key(cx, c);
    bc_emit_begin(cx);
    bc_emit_key_part(cx, kIdx, key, 0);
    bc_emit_end(cx);
  }
}
// UPDATE t SET qty = qty + 1 WHERE id = $1, qty BIGINT: decoders, bc_row_from, bc_update.
void bump_qty(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  if (!found) return;
  bc_row r = bc_row_from(cx, kTbl, old);
  bc_row_set_i64(cx, r, kQtyCol, bc_add_bigint(cx, bc_col_i64(cx, old, kQtyCol), 1, kTbl));
  bc_update(cx, kTbl, old, bc_row_end(cx, r));
}
// UPDATE t SET qty = qty + $2 WHERE id = $1: past BIGINT, the range error.
void add_to_qty(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  if (!found) return;
  bc_row r = bc_row_from(cx, kTbl, old);
  bc_row_set_i64(cx, r, kQtyCol, bc_add_bigint(cx, bc_col_i64(cx, old, kQtyCol), bc_param_i64(cx, 1), kTbl));
  bc_update(cx, kTbl, old, bc_row_end(cx, r));
}
// Copies past one arena chunk: the arena grows by another, freed at reset.
void pin_many(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  for (int i = 0; found && i < 3000; ++i) (void)bc_pin(cx, row);
}
// A join of t to itself with COUNT(DISTINCT qty): each outer step builds a
// key, reads a row, runs an inner scan, pins and groups, and the next step
// releases all but the pin and the group; the groups are then iterated, a
// loop of its own. Every step, release and pin is an allocation site.
void join_steps(bc_ctx* cx) {
  bc_agg groups = bc_agg_open(cx, 0);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_key k = bc_key_begin(cx, kTbl);
    bc_key_i64(cx, k, bc_col_i64(cx, bc_scan_row(cx, c), kIdCol));
    bool found = false;
    bc_view inner = bc_get(cx, bc_key_end(cx, k), &found);
    if (!found) continue;
    bc_cursor ic = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
    while (bc_scan_next(cx, ic)) (void)bc_pin(cx, bc_scan_key(cx, ic));
    (void)bc_agg_group(cx, groups, bc_col_key(cx, inner, kQtyCol));
  }
  int64_t n = 0;
  bc_view key = bc_empty(cx);
  while (bc_agg_next(cx, groups, &key, nullptr)) n = bc_add(cx, n, bc_bytes_eq(cx, bc_pin(cx, key), key) ? 1 : 0);
  bc_emit_begin(cx);
  bc_emit_i64(cx, n);
  bc_emit_end(cx);
}
// SELECT id FROM t ORDER BY name, the first $1 (0: all), added in reverse:
// a sorter's copies, its sort, and with $1 a top-n's replacements.
void sort_rows(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, bc_param_i64(cx, 0));
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_REVERSE);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sorter_add(cx, s, bc_col_key(cx, row, kNameCol), row);
  }
  bc_view p = bc_empty(cx);
  while (bc_sorter_next(cx, s, &p)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p, kIdCol);
    bc_emit_end(cx);
  }
}
// SELECT id FROM t ORDER BY qty DESC, name NULLS FIRST, id, the first $1
// (0: all): a sort key of every kind of part built per row, its view and
// its builder released with the step.
void sort_by_sort_key(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, bc_param_i64(cx, 0));
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sort_key k = bc_sort_key_begin(cx);
    bc_sort_key_col(cx, k, row, kQtyCol, BC_SORT_DESC, BC_SORT_NULLS_FIRST);
    bc_sort_key_col(cx, k, row, kNameCol, BC_SORT_ASC, BC_SORT_NULLS_FIRST);
    bc_sort_key_i64(cx, k, bc_col_i64(cx, row, kIdCol), BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sort_key_f64(cx, k, 0.5, BC_SORT_DESC, BC_SORT_NULLS_LAST);
    bc_sort_key_bool(cx, k, true, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sort_key_bytes(cx, k, bc_col_bytes(cx, row, kNameCol), BC_SORT_DESC, BC_SORT_NULLS_LAST);
    bc_sort_key_null(cx, k, BC_SORT_ASC, BC_SORT_NULLS_LAST);
    bc_sorter_add(cx, s, bc_sort_key_end(cx, k), row);
  }
  bc_view p = bc_empty(cx);
  while (bc_sorter_next(cx, s, &p)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p, kIdCol);
    bc_emit_end(cx);
  }
}
// A hash join of t to itself on qty: a hash table's keys and payloads, and
// a probe per outer row.
void hash_join(bc_ctx* cx) {
  bc_hash h = bc_hash_open(cx);
  bc_cursor b = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, b)) {
    bc_view row = bc_scan_row(cx, b);
    bc_hash_put(cx, h, bc_col_key(cx, row, kQtyCol), row);
    bc_hash_put(cx, h, bc_col_key(cx, row, kQtyCol), row);
  }
  int64_t n = 0;
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_hash_iter it = bc_hash_find(cx, h, bc_col_key(cx, bc_scan_row(cx, c), kQtyCol));
    bc_view p = bc_empty(cx);
    while (bc_hash_next(cx, it, &p)) n = bc_add(cx, n, bc_col_i64(cx, p, kQtyCol));
  }
  bc_emit_begin(cx);
  bc_emit_i64(cx, n);
  bc_emit_end(cx);
}
// SELECT COUNT(*), SUM(qty), MAX(qty) FROM t GROUP BY qty > 10: an
// aggregate's groups and slots.
void slot_sums(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 3);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    int64_t q = bc_col_i64(cx, row, kQtyCol);
    bc_group g = bc_agg_group(cx, a, q > 10 ? bc_param_bytes(cx, 0) : bc_empty(cx));
    bc_agg_count(cx, g, 0);
    bc_agg_add_i64(cx, g, 1, q);
    bc_agg_max_i64(cx, g, 2, q);
  }
  bc_group g{};
  while (bc_agg_next(cx, a, nullptr, &g)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, bc_agg_i64(cx, g, 0));
    bc_emit_i64(cx, bc_agg_i64(cx, g, 1));
    bc_emit_i64(cx, bc_agg_i64(cx, g, 2));
    bc_emit_end(cx);
  }
}
// NULL for the NOT NULL id: by parameter, by bc_row_set_null, left unset.
void null_id_param(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
}
void null_id_set(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_null(cx, r, kIdCol);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
}
void id_unset(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
}

}  // namespace

TEST_CASE("plan API: an allocation failure in any call is BC_E_MEMORY, never an exception into the plan",
          "[sql][algebra][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("operator new is the sanitizer's in this build");
  auto seed = [](Fixture& f, std::int64_t id, std::int64_t qty) {
    auto ps = params({id, bytes("n" + std::to_string(id)), qty});
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
    auto ref = sr.get();
    REQUIRE(bc_execute(f.cx, insert_row, &ref, &ps, {}, false, 0) == BC_COMMITTED);
  };
  Fixture f;
  for (std::int64_t i = 1; i <= 3; ++i) seed(f, i, i * 10);
  // Its own fixture, so the sort and join cases keep their three rows: one
  // row at the BIGINT maximum, so qty + 1, the constant form, overflows.
  Fixture at_max;
  seed(at_max, 4, std::numeric_limits<std::int64_t>::max());
  struct Case {
    const char* name;
    bc_plan_fn plan;
    std::vector<bc_value> params;
    std::vector<PhysType> result;
    int clean;  // the error of a run no allocation failure reached
    std::vector<std::vector<bc_value>> rows{};  // a clean run's result rows, when checked
  };
  const std::vector<Case> cases{
      {"insert", insert_row, {std::int64_t{9}, bytes("nine"), std::int64_t{90}}, {}, BC_OK},
      {"bc_emit_col", emit_row, {std::int64_t{2}}, {PhysType::I64, PhysType::Bytes, PhysType::I64}, BC_OK},
      {"bc_emit_col, rows checked",
       emit_row,
       {std::int64_t{2}},
       {PhysType::I64, PhysType::Bytes, PhysType::I64},
       BC_OK,
       {{std::int64_t{2}, bytes("n2"), std::int64_t{20}}}},
      {"bc_emit_key_part", emit_names, {}, {PhysType::Bytes}, BC_OK},
      {"update", bump_qty, {std::int64_t{3}}, {}, BC_OK},
      {"bc_pin, past one arena chunk", pin_many, {std::int64_t{1}}, {}, BC_OK},
      {"loop steps released, a join", join_steps, {}, {PhysType::I64}, BC_OK, {{std::int64_t{3}}}},
      {"a sorter",
       sort_rows,
       {std::int64_t{0}},
       {PhysType::I64},
       BC_OK,
       {{std::int64_t{1}}, {std::int64_t{2}}, {std::int64_t{3}}}},
      {"a top-n sorter", sort_rows, {std::int64_t{2}}, {PhysType::I64}, BC_OK, {{std::int64_t{1}}, {std::int64_t{2}}}},
      {"a sorter keyed by bc_sort_key_*",
       sort_by_sort_key,
       {std::int64_t{0}},
       {PhysType::I64},
       BC_OK,
       {{std::int64_t{3}}, {std::int64_t{2}}, {std::int64_t{1}}}},
      {"a top-n sorter keyed by bc_sort_key_*",
       sort_by_sort_key,
       {std::int64_t{1}},
       {PhysType::I64},
       BC_OK,
       {{std::int64_t{3}}}},
      {"a hash join", hash_join, {}, {PhysType::I64}, BC_OK, {{std::int64_t{120}}}},
      {"an aggregate's slots",
       slot_sums,
       {bytes("big")},
       {PhysType::I64, PhysType::I64, PhysType::I64},
       BC_OK,
       {{std::int64_t{1}, std::int64_t{10}, std::int64_t{10}}, {std::int64_t{2}, std::int64_t{50}, std::int64_t{30}}}},
      {"update, qty + $2 leaves BIGINT",
       add_to_qty,
       {std::int64_t{3}, std::numeric_limits<std::int64_t>::max()},
       {},
       BC_E_CONSTRAINT},
      {"bc_row_set_param NULL", null_id_param, {bc_value{}}, {}, BC_E_CONSTRAINT},
      {"bc_row_set_null", null_id_set, {}, {}, BC_E_CONSTRAINT},
      {"bc_row_end, a column unset", id_unset, {}, {}, BC_E_CONSTRAINT},
  };
  const std::vector<Case> at_max_cases{
      {"update, qty + 1 leaves BIGINT", bump_qty, {std::int64_t{4}}, {}, BC_E_CONSTRAINT},
  };
  auto fail_each_allocation = [](Fixture& f, const Case& c) {
    INFO(c.name);
    auto failed = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 100000);
      // Built outside the count. The run verifies it, so verification's
      // allocations are failed too.
      auto ps = params(c.params);
      SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(c.result),
                   f.cat().version()};
      auto ref = sr.get();
      bc_ctx_begin(f.cx, &ps, {});
      auto threw = false;
      bytecask::testing::AllocFaultReport report;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        try {
          bc_ctx_run(f.cx, c.plan, &ref);
        } catch (...) {
          threw = true;
        }
        report = faults.report();
      }
      INFO("allocation " << nth);
      CHECK_FALSE(threw);
      auto error = static_cast<int>(bc_error(f.cx));
      if (!report.fired) {
        CHECK(error == c.clean);
        if (!c.rows.empty()) CHECK(f.results(c.result) == c.rows);
        bc_ctx_reset(f.cx);
        break;
      }
      bc_ctx_reset(f.cx);  // nothing is committed: the engine is never asked to write
      CHECK((error == BC_E_MEMORY || error == c.clean));
      // A failed statement's charges go at reset, whatever the throw left
      // charged, and so does the write set's storage past what a reset keeps.
      CHECK(bc_ctx_held_for_testing(f.cx) == 0);
      CHECK(bc_ctx_write_set_slots_for_testing(f.cx) <= 4 * bytecask::sql::kKeptWriteSlots);
      ++failed;
    }
    CHECK(failed > 0);  // the plan allocates, and each allocation was failed once
  };
  for (const auto& c : cases) fail_each_allocation(f, c);
  for (const auto& c : at_max_cases) fail_each_allocation(at_max, c);
}

TEST_CASE("plan API: a commit whose allocations fail is never BC_COMMITTED with an error set",
          "[sql][algebra][alloc]") {
  if (!bytecask::testing::kAllocFaultsAvailable) SKIP("operator new is the sanitizer's in this build");
  Fixture f;
  auto failed = 0;
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 100000);
    auto ps = params({std::int64_t{1000} + nth, bytes("x"), std::int64_t{1}});
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
    auto ref = sr.get();
    bc_ctx_begin(f.cx, &ps, {});
    bc_ctx_run(f.cx, insert_row, &ref);
    REQUIRE(bc_error(f.cx) == BC_OK);
    auto threw = false;
    auto r = BC_FAILED;
    bytecask::testing::AllocFaultReport report;
    {
      // The engine leaks a key directory node it cannot place for want of
      // memory rather than free one still reachable (#390).
      [[maybe_unused]] const bytecask::testing::ExpectedLeaks leaks;
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      try {
        r = bc_ctx_commit(f.cx, false);
      } catch (...) {
        threw = true;
      }
      report = faults.report();
    }
    INFO("allocation " << nth);
    CHECK_FALSE(threw);
    auto error = bc_error(f.cx);
    // The one contract: success is BC_OK and BC_COMMITTED together.
    CHECK((r == BC_COMMITTED) == (error == BC_OK));
    bc_ctx_reset(f.cx);
    // A write that reached the data file before an allocation failed leaves
    // the engine refusing writes until resume().
    if (f.db->is_degraded()) f.db->resume();
    if (!report.fired) {
      CHECK(r == BC_COMMITTED);
      break;
    }
    ++failed;
  }
  CHECK(failed > 0);
}
