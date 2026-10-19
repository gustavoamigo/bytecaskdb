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
#include <memory>
#include <string>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::PhysType;

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
  bytecask::sql::key_put_i64(key, 1, {bytecask::sql::PhysType::I64});
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
  bc_view all = bc_key_end(cx, bc_key_begin(cx, kTbl));
  bc_cursor c = bc_scan_open(cx, kTbl, all, all, BC_SCAN_PREFIX);
  bool first = true;
  bc_view kept = all;
  while (bc_scan_next(cx, c)) {
    if (!first) {
      bc_emit_count(cx, bc_col_i64(cx, kept, 20));  // the previous entry's row: stale
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

void unimplemented_call(bc_ctx* cx) { bc_sorter_open(cx, 1024); }
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

TEST_CASE("plan API: the ephemeral structures are not implemented yet and set BC_E_HANDLE", "[sql][algebra]") {
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

TEST_CASE("plan API: bc_ctx_run checks what a plan depends on, not a catalog version", "[sql][algebra][catalog]") {
  Fixture f;
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  auto run_with = [&](const SchemaRef& sr) {
    auto ref = sr.get();
    return bc_execute(f.cx, insert_plan_ok, &ref, &ps, {}, false, 0);
  };
  const auto& t = *f.cat().table("t");
  auto col = [&](std::uint32_t id) {
    const auto* slot = t.layout.find(id);
    return bc_col_ref{kLay, id, slot->pos, static_cast<std::uint8_t>(slot->type)};
  };
  SchemaRef ok{{kTbl}, {kTbl}, {kIdx}, {col(kIdCol), col(kNameCol), col(kQtyCol)}, {}, 0};

  // The version in the ref is for messages: a stale one runs.
  CHECK(run_with(ok) == BC_COMMITTED);
  // A write plan that does not maintain an index of its table does not run.
  auto missing_index = ok;
  missing_index.indexes.clear();
  CHECK(run_with(missing_index) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(std::string{bc_error_message(f.cx)}.find("does not maintain index t_by_name") != std::string::npos);
  // A column at another position or of another type does not either.
  auto moved = ok;
  moved.cols[2].pos += 8;
  CHECK(run_with(moved) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  auto retyped = ok;
  retyped.cols[2].type = BC_T_F64;
  CHECK(run_with(retyped) == BC_FAILED);
  // Nor one that reads an object that is gone.
  auto gone = ok;
  gone.read.push_back(4242);
  CHECK(run_with(gone) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);

  // ADD COLUMN keeps the plan; the new column takes its default in rows the
  // plan writes, without the plan naming it.
  auto v = f.cat().version();
  f.ddl(bytecask::sql::AddColumnDdl{"t", {"tag", {bytecask::sql::SqlKind::BigInt}, true, std::int64_t{7}}});
  CHECK(f.cat().version() == v + 1);
  ps = params({std::int64_t{2}, bytes("b"), std::int64_t{6}});
  CHECK(run_with(ok) == BC_COMMITTED);
  Bytes key, row;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, 2, {PhysType::I64});
  REQUIRE(f.db->get({}, key, row));
  const auto& t2 = *f.cat().table("t");
  CHECK(bytecask::sql::row_i64(row, t2.layout.find(t2.column("tag")->id)->pos) == 7);

  // DROP INDEX: the plan maintains an index that is gone.
  f.ddl(bytecask::sql::DropIndexDdl{"t_by_name"});
  ps = params({std::int64_t{3}, bytes("c"), std::int64_t{7}});
  CHECK(run_with(ok) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  auto no_index = ok;
  no_index.indexes.clear();
  CHECK(run_with(no_index) == BC_COMMITTED);
}

TEST_CASE("plan API: a context holds a catalog version only while it executes", "[sql][algebra][catalog]") {
  Fixture f;
  std::weak_ptr<const bytecask::sql::Catalog> seen = f.live->current();
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  bc_ctx_begin(f.cx, &ps, {});
  CHECK(seen.use_count() == 2);  // the live catalog and the context
  bc_ctx_run(f.cx, insert_plan_ok, nullptr);
  CHECK(bc_ctx_commit(f.cx, false) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  CHECK(seen.use_count() == 1);
  f.ddl(bytecask::sql::AddColumnDdl{"t", {"x", {bytecask::sql::SqlKind::BigInt}, true, {}}});
  CHECK(seen.expired());  // superseded, and no idle context keeps it
}

TEST_CASE("plan API: a write committed across a DDL on its table is a conflict", "[sql][algebra][catalog]") {
  Fixture f;
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  // Begin and run on catalog v2, then a DDL on t commits before the plan does.
  bc_ctx_begin(f.cx, &ps, {});
  bc_ctx_run(f.cx, insert_plan_ok, nullptr);
  REQUIRE(bc_error(f.cx) == BC_OK);
  f.ddl(bytecask::sql::CreateIndexDdl{"t_by_qty", "t", {{"qty"}}});
  CHECK(bc_ctx_commit(f.cx, false) == BC_CONFLICT);
  bc_ctx_reset(f.cx);
  Bytes key, out;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, 1, {PhysType::I64});
  CHECK_FALSE(f.db->get({}, key, out));

  // A DDL on another table does not conflict.
  f.ddl(bytecask::sql::CreateTableDdl{"other", {Fixture::col("k", PhysType::I64, false)}, {"k"}});
  bc_ctx_begin(f.cx, &ps, {});
  bc_ctx_run(f.cx, insert_plan_ok, nullptr);
  f.ddl(bytecask::sql::AddColumnDdl{"other", {"x", {bytecask::sql::SqlKind::Text}, true, {}}});
  CHECK(bc_ctx_commit(f.cx, false) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  CHECK(f.db->get({}, key, out));
}
