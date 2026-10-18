// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The plan API's runtime guarantees, driven by small plans written as
// captureless lambdas: handles are checked, errors are sticky, a cursor's
// views die when it steps, the budget ends a plan, and nothing is committed
// unless the plan finishes cleanly.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;

namespace {

constexpr std::uint32_t kTbl = 1;
constexpr std::uint32_t kLay = 2;
constexpr std::uint32_t kIdx = 3;
constexpr std::uint32_t kIdCol = 0, kNameCol = 1, kQtyCol = 2;

// Inserts (id, name, qty) from params 0..2, maintaining the index.
void insert_plan(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, kb);
  bc_row rb = bc_row_begin(cx, kLay);
  bc_row_set_i64(cx, rb, kIdCol, bc_param_i64(cx, 0));
  bc_row_set_bytes(cx, rb, kNameCol, bc_param_bytes(cx, 1));
  bc_row_set_i64(cx, rb, kQtyCol, bc_param_i64(cx, 2));
  bc_view row = bc_row_end(cx, rb);
  bc_guard_absent(cx, pk);
  bc_put(cx, pk, row);
  bc_key ib = bc_key_begin(cx, kIdx);
  bc_key_bytes(cx, ib, bc_param_bytes(cx, 1));
  bc_key_i64(cx, ib, bc_param_i64(cx, 0));
  bc_put(cx, bc_key_end(cx, ib), bc_param_bytes(cx, 99));  // index entries carry no value: param 99 is unbound
  bc_emit_count(cx, 1);
}

void insert_plan_ok(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kTbl);
  bc_key_i64(cx, kb, bc_param_i64(cx, 0));
  bc_view pk = bc_key_end(cx, kb);
  bc_row rb = bc_row_begin(cx, kLay);
  bc_row_set_i64(cx, rb, kIdCol, bc_param_i64(cx, 0));
  bc_row_set_bytes(cx, rb, kNameCol, bc_param_bytes(cx, 1));
  bc_row_set_i64(cx, rb, kQtyCol, bc_param_i64(cx, 2));
  bc_guard_absent(cx, pk);
  bc_put(cx, pk, bc_row_end(cx, rb));
  bc_emit_count(cx, 1);
}

auto run(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, bc_budget budget = {}) -> int {
  auto ps = params(std::move(p));
  return bc_execute(f.cx, plan, nullptr, &ps, budget, false, 0);
}

}  // namespace

TEST_CASE("plan API: an error is sticky and nothing is committed", "[sql][algebra]") {
  Fixture f;
  CHECK(run(f, insert_plan, {std::int64_t{1}, bytes("a"), std::int64_t{5}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("parameter") != std::string::npos);
  CHECK(bc_ctx_affected(f.cx) == -1);  // bc_emit_count after the error was dropped

  Bytes out;
  Bytes key;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, 1, false);
  CHECK_FALSE(f.db->get({}, key, out));  // the row put before the error was not committed

  CHECK(run(f, insert_plan_ok, {std::int64_t{1}, bytes("a"), std::int64_t{5}}) == BC_COMMITTED);
  CHECK(bc_error(f.cx) == BC_OK);
  CHECK(bc_ctx_affected(f.cx) == 1);
  CHECK(f.db->get({}, key, out));

  // The same insert again fails its guard: a conflict, retried, then reported.
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  CHECK(bc_execute(f.cx, insert_plan_ok, nullptr, &ps, {}, false, 2) == BC_CONFLICT);
}

namespace {
bc_view g_stale;  // a view smuggled out of one execution into the next
void leak_view(bc_ctx* cx) { g_stale = bc_param_bytes(cx, 0); }
void read_stale(bc_ctx* cx) {
  bc_key kb = bc_key_begin(cx, kIdx);
  bc_key_bytes(cx, kb, g_stale);
}
}  // namespace

TEST_CASE("plan API: a handle from an earlier execution is stale", "[sql][algebra]") {
  Fixture f;
  CHECK(run(f, leak_view, {bytes("x")}) == BC_COMMITTED);
  CHECK(run(f, read_stale, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
void keep_view_across_step(bc_ctx* cx) {
  // A plan cannot make an empty view, so "the whole table" is its prefix.
  bc_view all = bc_key_end(cx, bc_key_begin(cx, kTbl));
  bc_cursor c = bc_scan_open(cx, kTbl, all, all, BC_SCAN_PREFIX);
  bool first = true;
  bc_view kept = all;
  while (bc_scan_next(cx, c)) {
    if (!first) {
      bc_emit_count(cx, bc_col_i64(cx, kept, 16));  // the previous entry's row: stale
    }
    kept = bc_scan_value(cx, c);
    first = false;
  }
}

void count_rows(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    n = bc_sub(cx, n, -1);
  }
  bc_emit_count(cx, n);
}

void unimplemented_call(bc_ctx* cx) { bc_pin(cx, bc_param_bytes(cx, 0)); }
}  // namespace

TEST_CASE("plan API: a cursor's views die when it steps", "[sql][algebra]") {
  Fixture f;
  for (std::int64_t i = 1; i <= 3; ++i) {
    REQUIRE(run(f, insert_plan_ok, {i, bytes("n"), i}) == BC_COMMITTED);
  }
  CHECK(run(f, keep_view_across_step, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("cursor moved") != std::string::npos);
}

TEST_CASE("plan API: the budget ends a plan, and scans stay inside their table", "[sql][algebra]") {
  Fixture f;
  for (std::int64_t i = 1; i <= 5; ++i) {
    REQUIRE(run(f, insert_plan_ok, {i, bytes("n"), i}) == BC_COMMITTED);
  }
  // Catalog records sort before the table and index entries after it: a full
  // scan of the table sees exactly its five rows.
  CHECK(run(f, count_rows, {Bytes{}}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 5);

  CHECK(run(f, count_rows, {Bytes{}}, {.max_reads = 3, .deadline_ns = 0}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_BUDGET);
  CHECK(bc_ctx_affected(f.cx) == -1);
}

TEST_CASE("plan API: calls outside the prototype set BC_E_HANDLE", "[sql][algebra]") {
  Fixture f;
  CHECK(run(f, unimplemented_call, {bytes("x")}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
void overflow(bc_ctx* cx) { bc_emit_count(cx, bc_sub(cx, bc_param_i64(cx, 0), 1)); }
}  // namespace

TEST_CASE("plan API: bc_sub reports overflow instead of wrapping", "[sql][algebra]") {
  Fixture f;
  CHECK(run(f, overflow, {std::numeric_limits<std::int64_t>::min()}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_ARITH);
}

TEST_CASE("plan API: a plan compiled against another catalog version does not run", "[sql][algebra]") {
  Fixture f;
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  std::uint32_t tables[] = {kTbl};
  bc_schema_ref stale{f.cat.version() - 1, nullptr, tables, 0, 1};
  CHECK(bc_execute(f.cx, insert_plan_ok, &stale, &ps, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  bc_schema_ref live{f.cat.version(), nullptr, tables, 0, 1};
  CHECK(bc_execute(f.cx, insert_plan_ok, &live, &ps, {}, false, 0) == BC_COMMITTED);
}
