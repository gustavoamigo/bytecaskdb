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
// UPDATE t SET qty = qty + 1 WHERE id = $1: decoders, bc_row_from, bc_update.
void bump_qty(bc_ctx* cx) {
  bool found = false;
  bc_view old = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  if (!found) return;
  bc_row r = bc_row_from(cx, kTbl, old);
  bc_row_set_i64(cx, r, kQtyCol, bc_add(cx, bc_col_i64(cx, old, kQtyCol), 1));
  bc_update(cx, kTbl, old, bc_row_end(cx, r));
}
// Copies past one arena chunk: the arena grows by another, freed at reset.
void pin_many(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  for (int i = 0; found && i < 3000; ++i) (void)bc_pin(cx, row);
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
  Fixture f;
  for (std::int64_t i = 1; i <= 3; ++i) {
    auto ps = params({i, bytes("n" + std::to_string(i)), i * 10});
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
    auto ref = sr.get();
    REQUIRE(bc_execute(f.cx, insert_row, &ref, &ps, {}, false, 0) == BC_COMMITTED);
  }
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
      {"bc_row_set_param NULL", null_id_param, {bc_value{}}, {}, BC_E_CONSTRAINT},
      {"bc_row_set_null", null_id_set, {}, {}, BC_E_CONSTRAINT},
      {"bc_row_end, a column unset", id_unset, {}, {}, BC_E_CONSTRAINT},
  };
  for (const auto& c : cases) {
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
      ++failed;
    }
    CHECK(failed > 0);  // the plan allocates, and each allocation was failed once
  }
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
