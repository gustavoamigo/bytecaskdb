// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The plan API's runtime guarantees, driven by small plans written as
// captureless lambdas: handles are checked, errors are sticky, a cursor's
// views die when it steps, the budget ends a plan, and nothing is committed
// unless the plan finishes cleanly.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
using bytecask::sql::kCursorEntryBytes;
using bytecask::sql::kGroupEntryBytes;
using bytecask::sql::kKeyEntryBytes;
using bytecask::sql::kRowEntryBytes;
using bytecask::sql::kViewEntryBytes;
using bytecask::sql::PhysType;

namespace {

constexpr std::uint32_t kTbl = 1;
constexpr std::uint32_t kIdx = 3;
constexpr std::uint32_t kIdCol = 0, kNameCol = 1, kQtyCol = 2;

// Inserts (id, name, qty) from params 0..2, then sets qty from param 99,
// which is unbound.
void insert_plan(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_row_set_param(cx, r, kNameCol, 1);
  bc_row_set_param(cx, r, kQtyCol, 2);
  bc_view row = bc_row_end(cx, r);
  bc_insert(cx, kTbl, row);
  bc_row r2 = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r2, kQtyCol, 99);
  bc_emit_count(cx, 1);
}

void insert_plan_ok(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_row_set_param(cx, r, kNameCol, 1);
  bc_row_set_param(cx, r, kQtyCol, 2);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}

// A reference declaring every column, so the plans may name any.
auto run(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, bc_budget budget = {}) -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, plan, &ref, &ps, budget, false, 0);
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

  // The same insert again: the runtime's check on the snapshot raises the
  // duplicate, which is final.
  CHECK(run(f, insert_plan_ok, {std::int64_t{1}, bytes("a"), std::int64_t{5}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
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
      bc_emit_count(cx, bc_col_i64(cx, kept, kIdCol));  // the previous entry's row: stale
    }
    kept = bc_scan_row(cx, c);
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

void unimplemented_call(bc_ctx* cx) { bc_sorter_open(cx); }
}  // namespace

TEST_CASE("plan API: a cursor's views die when it steps", "[sql][algebra]") {
  Fixture f;
  for (std::int64_t i = 1; i <= 3; ++i) {
    REQUIRE(run(f, insert_plan_ok, {i, bytes("n"), i}) == BC_COMMITTED);
  }
  CHECK(run(f, keep_view_across_step, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("loop moved on") != std::string::npos);
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

  CHECK(run(f, count_rows, {Bytes{}}, {.max_reads = 3, .max_bytes = 0, .deadline_ns = 0}) == BC_FAILED);
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

namespace {
// Every id in t_by_name order, from the index key alone.
void scan_index(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) n = bc_add(cx, n, bc_key_part_i64(cx, kIdx, bc_scan_key(cx, c), 1));
  bc_emit_count(cx, n);
}
}  // namespace

TEST_CASE("plan API: bc_ctx_run checks what a plan depends on, not a catalog version", "[sql][algebra][catalog]") {
  Fixture f;
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  auto run_with = [&](const SchemaRef& sr) {
    auto ref = sr.get();
    return bc_execute(f.cx, insert_plan_ok, &ref, &ps, {}, false, 0);
  };
  auto col = [](std::uint32_t id, std::uint8_t type) { return bc_col_ref{kTbl, id, type}; };
  SchemaRef ok{{kTbl}, {kTbl}, {col(kIdCol, BC_T_I64), col(kNameCol, BC_T_BYTES), col(kQtyCol, BC_T_I64)}, {}, 0};

  // The version in the ref is for messages: a stale one runs.
  CHECK(run_with(ok) == BC_COMMITTED);
  // A column of another type, or one the table does not have, does not run.
  auto retyped = ok;
  retyped.cols[2].type = BC_T_F64;
  CHECK(run_with(retyped) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  auto unknown = ok;
  unknown.cols.push_back(col(9, BC_T_I64));
  CHECK(run_with(unknown) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  // Nor one that reads an object that is gone, or writes a table that is.
  auto gone = ok;
  gone.read.push_back(4242);
  CHECK(run_with(gone) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  auto gone_table = ok;
  gone_table.written.push_back(4242);
  CHECK(run_with(gone_table) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  // A column the plan names but its reference does not declare is a plan bug
  // at the call: the reference is complete, so the check covers every column.
  auto undeclared = ok;
  undeclared.cols.pop_back();
  ps = params({std::int64_t{2}, bytes("b"), std::int64_t{6}});
  CHECK(run_with(undeclared) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("not declared") != std::string::npos);

  // ADD COLUMN keeps the plan; the new column takes its default in rows the
  // plan writes, without the plan naming it.
  auto v = f.cat().version();
  f.ddl(bytecask::sql::AddColumnDdl{"t", {"tag", {bytecask::sql::SqlKind::BigInt}, true, std::int64_t{7}}});
  CHECK(f.cat().version() == v + 1);
  CHECK(run_with(ok) == BC_COMMITTED);
  Bytes key, row;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, 2, {PhysType::I64});
  REQUIRE(f.db->get({}, key, row));
  const auto& t2 = *f.cat().table("t");
  CHECK(bytecask::sql::row_column(row, *t2.layout.find(t2.column("tag")->id)).i64() == 7);

  // CREATE INDEX keeps the write plan, which maintains the new index.
  f.ddl(bytecask::sql::CreateIndexDdl{"t_by_qty", "t", {{"qty"}}});
  const auto by_qty = f.cat().index("t_by_qty").second->id;
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, by_qty) == 2);  // the backfill
  ps = params({std::int64_t{3}, bytes("c"), std::int64_t{7}});
  CHECK(run_with(ok) == BC_COMMITTED);
  CHECK(count_entries(*f.db, bytecask::sql::kNsIndex, by_qty) == 3);

  // DROP INDEX keeps the write plan too, and refuses a plan that scans it.
  SchemaRef reads_index{{kIdx}, {}, {}, {}, 0};
  auto scan_ref = reads_index.get();
  auto none = params({});
  CHECK(bc_execute(f.cx, scan_index, &scan_ref, &none, {}, false, 0) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1 + 2 + 3);
  f.ddl(bytecask::sql::DropIndexDdl{"t_by_name"});
  ps = params({std::int64_t{4}, bytes("d"), std::int64_t{8}});
  CHECK(run_with(ok) == BC_COMMITTED);
  CHECK(bc_execute(f.cx, scan_index, &scan_ref, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
}

TEST_CASE("plan API: bc_ctx_check verifies a reference as bc_ctx_run does, and runs nothing", "[sql][algebra][catalog]") {
  Fixture f;
  auto none = params({});
  SchemaRef reads_index{{kIdx}, {}, {}, {}, 0};
  auto ref = reads_index.get();
  // Without bc_ctx_begin there is no pinned version to check against.
  CHECK_FALSE(bc_ctx_check(f.cx, &ref));
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  bc_ctx_begin(f.cx, &none, {});
  CHECK(bc_ctx_check(f.cx, &ref));
  CHECK(bc_ctx_affected(f.cx) == -1);  // no plan ran
  bc_ctx_reset(f.cx);
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_COMMITTED);
  // A plan a DDL made incompatible: false, with the detail that asks for a
  // recompile.
  f.ddl(bytecask::sql::DropIndexDdl{"t_by_name"});
  bc_ctx_begin(f.cx, &none, {});
  CHECK_FALSE(bc_ctx_check(f.cx, &ref));
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  bc_ctx_reset(f.cx);
}

namespace {
void insert_one(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
}
// Every row of t through t_by_name: an index cursor handing out table rows.
void rows_through_index(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kIdx, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    (void)bc_scan_row(cx, c);
    n = bc_add(cx, n, 1);
  }
  bc_emit_count(cx, n);
}
}  // namespace

TEST_CASE("plan API: a plan names only the objects its reference declares", "[sql][algebra][catalog]") {
  // Every table and index a plan names must be in its reference, writes in
  // tables_written, as every column must: then a DDL on any of them is a
  // refusal the embedded API recompiles on, never a stray error.
  Fixture f;
  auto none = params({});
  auto one = params({std::int64_t{1}});
  auto id_col = bc_col_ref{kTbl, kIdCol, BC_T_I64};
  SchemaRef no_index{{kTbl}, {}, {}, {}, 0};  // scans t_by_name without naming it
  auto r1 = no_index.get();
  CHECK(bc_execute(f.cx, scan_index, &r1, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("not declared") != std::string::npos);
  SchemaRef read_only{{kTbl}, {}, {id_col}, {}, 0};  // writes t, declared only as read
  auto r2 = read_only.get();
  CHECK(bc_execute(f.cx, insert_one, &r2, &one, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("writes") != std::string::npos);
  SchemaRef writes{{}, {kTbl}, {id_col}, {}, 0};
  auto r3 = writes.get();
  CHECK(bc_execute(f.cx, insert_one, &r3, &one, {}, false, 0) == BC_COMMITTED);
  // bc_scan_row on an index cursor hands out the table's rows: a reference
  // that declares only the index cannot read them.
  SchemaRef index_only{{kIdx}, {}, {}, {}, 0};
  auto r4 = index_only.get();
  CHECK(bc_execute(f.cx, rows_through_index, &r4, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  SchemaRef index_and_table{{kIdx, kTbl}, {}, {}, {}, 0};
  auto r5 = index_and_table.get();
  CHECK(bc_execute(f.cx, rows_through_index, &r5, &none, {}, false, 0) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1);
  // An index where a table is required is malformed, not stale: no
  // recompile would cure it.
  SchemaRef writes_index{{}, {kIdx}, {}, {}, 0};
  auto r7 = writes_index.get();
  CHECK(bc_execute(f.cx, scan_index, &r7, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_PLAN);
  SchemaRef index_col{{kIdx}, {}, {bc_col_ref{kIdx, kIdCol, BC_T_I64}}, {}, 0};
  auto r8 = index_col.get();
  CHECK(bc_execute(f.cx, scan_index, &r8, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_PLAN);
  // A column of a table the reference does not name is a malformed reference.
  SchemaRef stray_col{{kIdx}, {}, {id_col}, {}, 0};
  auto r6 = stray_col.get();
  CHECK(bc_execute(f.cx, scan_index, &r6, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_PLAN);
  // The table dropped and created again under its name: a new id, so the
  // reference that declares the old one is incompatible, a recompilable
  // refusal.
  f.ddl(bytecask::sql::DropTableDdl{"t"});
  f.ddl(bytecask::sql::CreateTableDdl{"t", {Fixture::col("id", PhysType::I64, false)}, {"id"}});
  CHECK(bc_execute(f.cx, insert_one, &r3, &one, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
}

TEST_CASE("plan API: a context holds a catalog version only while it executes", "[sql][algebra][catalog]") {
  Fixture f;
  std::weak_ptr<const bytecask::sql::Catalog> seen = f.live->current();
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, 0};
  auto ref = sr.get();
  bc_ctx_begin(f.cx, &ps, {});
  CHECK(seen.use_count() == 2);  // the live catalog and the context
  bc_ctx_run(f.cx, insert_plan_ok, &ref);
  CHECK(bc_ctx_commit(f.cx, false) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  CHECK(seen.use_count() == 1);
  f.ddl(bytecask::sql::AddColumnDdl{"t", {"x", {bytecask::sql::SqlKind::BigInt}, true, {}}});
  CHECK(seen.expired());  // superseded, and no idle context keeps it
}

TEST_CASE("plan API: a write committed across a DDL on its table is a conflict", "[sql][algebra][catalog]") {
  Fixture f;
  auto ps = params({std::int64_t{1}, bytes("a"), std::int64_t{5}});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, 0};
  auto ref = sr.get();
  // Begin and run on catalog v2, then a DDL on t commits before the plan
  // does: the plan wrote no entry for the new index, so it must not commit.
  bc_ctx_begin(f.cx, &ps, {});
  bc_ctx_run(f.cx, insert_plan_ok, &ref);
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
  bc_ctx_run(f.cx, insert_plan_ok, &ref);
  f.ddl(bytecask::sql::AddColumnDdl{"other", {"x", {bytecask::sql::SqlKind::Text}, true, {}}});
  CHECK(bc_ctx_commit(f.cx, false) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  CHECK(f.db->get({}, key, out));
}

namespace {
// COUNT(DISTINCT qty) over t: each non-NULL qty's key grouped, then the
// groups counted.
void count_distinct_qty(bc_ctx* cx) {
  bc_view all = bc_key_end(cx, bc_key_begin(cx, kTbl));
  bc_cursor c = bc_scan_open(cx, kTbl, all, all, BC_SCAN_PREFIX);
  bc_agg a = bc_agg_open(cx, 0);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (bc_col_null(cx, row, kQtyCol)) continue;
    (void)bc_agg_group(cx, a, bc_col_key(cx, row, kQtyCol));
  }
  int64_t n = 0;
  bc_view key;
  bc_group g;
  while (bc_agg_next(cx, a, &key, &g)) ++n;
  bc_emit_count(cx, n);
}

// Table w (id, b), with no index: inserts (id, b) from params 0 and 1.
constexpr std::uint32_t kWide = 4;
void insert_wide(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kWide);
  bc_row_set_param(cx, r, 0, 0);
  bc_row_set_param(cx, r, 1, 1);
  bc_insert(cx, kWide, bc_row_end(cx, r));
  bc_emit_count(cx, 1);
}

// COUNT(DISTINCT b) over w.
void count_distinct_wide(bc_ctx* cx) {
  bc_view all = bc_key_end(cx, bc_key_begin(cx, kWide));
  bc_cursor c = bc_scan_open(cx, kWide, all, all, BC_SCAN_PREFIX);
  bc_agg a = bc_agg_open(cx, 0);
  while (bc_scan_next(cx, c)) (void)bc_agg_group(cx, a, bc_col_key(cx, bc_scan_row(cx, c), 1));
  int64_t n = 0;
  while (bc_agg_next(cx, a, nullptr, nullptr)) ++n;
  bc_emit_count(cx, n);
}

// A group key read after its cursor moved on.
void stale_col_key(bc_ctx* cx) {
  bc_view all = bc_key_end(cx, bc_key_begin(cx, kTbl));
  bc_cursor c = bc_scan_open(cx, kTbl, all, all, BC_SCAN_PREFIX);
  (void)bc_scan_next(cx, c);
  bc_view k = bc_col_key(cx, bc_scan_row(cx, c), kQtyCol);
  (void)bc_scan_next(cx, c);
  bc_agg a = bc_agg_open(cx, 0);
  (void)bc_agg_group(cx, a, k);
  bc_emit_count(cx, 1);
}

// bc_check_int of param 0 as an INTEGER, on t.
void check_integer(bc_ctx* cx) { bc_emit_count(cx, bc_check_int(cx, bc_param_i64(cx, 0), BC_INTEGER, kTbl)); }
}  // namespace

TEST_CASE("plan API: an aggregate's groups, keyed by a column, and the range check of a computed integer",
          "[sql][algebra]") {
  Fixture f;
  for (std::int64_t id = 1; id <= 6; ++id) {
    bc_value qty = id == 6 ? bc_value{} : bc_value{std::int64_t{id % 3}};  // 1, 2, 0, 1, 2, NULL
    REQUIRE(run(f, insert_plan_ok, {id, bytes("n"), qty}) == BC_COMMITTED);
  }
  // Equal values give equal keys: three groups, the NULL skipped.
  CHECK(run(f, count_distinct_qty, {}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 3);
  // A memory budget the groups outgrow is BC_E_LIMIT, never a wrong count:
  // every budget below the one that suffices fails.
  for (std::uint64_t budget = 1;; ++budget) {
    REQUIRE(budget < 4096);
    INFO("budget " << budget);
    if (run(f, count_distinct_qty, {}, {.max_reads = 0, .max_bytes = budget, .deadline_ns = 0}) == BC_COMMITTED) {
      CHECK(bc_ctx_affected(f.cx) == 3);
      break;
    }
    CHECK(bc_error(f.cx) == BC_E_LIMIT);
  }
  // A scan's group key lives as long as its entry, outside the arena.
  CHECK(run(f, stale_col_key, {}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // A group key is not an engine key: a value past the key ceiling groups.
  f.ddl(bytecask::sql::CreateTableDdl{"w", {Fixture::col("id", PhysType::I64, false), Fixture::col("b", PhysType::Bytes, true)}, {"id"}});
  const std::string wide(70000, 'x');
  for (std::int64_t id = 1; id <= 3; ++id) REQUIRE(run(f, insert_wide, {id, bytes(wide)}) == BC_COMMITTED);
  CHECK(run(f, count_distinct_wide, {}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1);

  CHECK(run(f, check_integer, {std::int64_t{2147483647}}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 2147483647);
  CHECK(run(f, check_integer, {std::int64_t{-2147483649}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
  CHECK(bc_error_detail(f.cx) == ((std::uint32_t{BC_C_RANGE} << 24) | kTbl));
  CHECK(std::string{bc_error_message(f.cx)}.starts_with("integer out of range"));
}

namespace {
// SELECT id, name, qty FROM t WHERE id = $1.
void point_select(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  if (!found) return;
  bc_emit_begin(cx);
  bc_emit_col(cx, row, kIdCol);
  bc_emit_col(cx, row, kNameCol);
  bc_emit_col(cx, row, kQtyCol);
  bc_emit_end(cx);
}

// Every row of t, counted: a scan holds one entry at a time.
void scan_all(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) n = bc_add(cx, n, bc_col_null(cx, bc_scan_row(cx, c), kQtyCol) ? 0 : 1);
  bc_emit_count(cx, n);
}

auto memory_budget(std::uint64_t bytes) -> bc_budget { return {.max_reads = 0, .max_bytes = bytes, .deadline_ns = 0}; }
}  // namespace

TEST_CASE("plan API: a statement is charged the bytes it holds, exactly, and past its budget fails with BC_E_LIMIT",
          "[sql][algebra][memory]") {
  Fixture f;
  // An insert holds the row it built, its builder and its view: its write
  // set is not charged. Row 2 is row 1's size, and measures it.
  auto row_key = [&](std::int64_t id) {
    Bytes k;
    bytecask::sql::key_put_prefix(k, bytecask::sql::kNsRow, kTbl);
    bytecask::sql::key_put_i64(k, id, f.cat().key_layout(kTbl)->parts[0]);
    return k;
  };
  REQUIRE(run(f, insert_plan_ok, {std::int64_t{2}, bytes("bobby"), std::int64_t{6}}) == BC_COMMITTED);
  Bytes stored;
  REQUIRE(f.db->get({}, row_key(2), stored));
  const auto key = row_key(1);
  std::vector<bc_value> row1{std::int64_t{1}, bytes("alice"), std::int64_t{5}};
  const auto insert_holds = kRowEntryBytes + stored.size() + kViewEntryBytes;
  CHECK(run(f, insert_plan_ok, row1, memory_budget(insert_holds - 1)) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(std::string{bc_error_message(f.cx)}.find("memory budget") != std::string::npos);
  Bytes out;
  CHECK_FALSE(f.db->get({}, key, out));  // nothing committed
  CHECK(run(f, insert_plan_ok, row1, memory_budget(insert_holds)) == BC_COMMITTED);
  CHECK(f.db->get({}, key, out));

  // A point read holds the key it built, the row it read, a view of each,
  // and the result row with its 4-byte length.
  const std::vector<PhysType> types{PhysType::I64, PhysType::Bytes, PhysType::I64};
  auto select = [&](bc_budget budget) {
    auto ps = params({std::int64_t{1}});
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(types), f.cat().version()};
    auto ref = sr.get();
    return bc_execute(f.cx, point_select, &ref, &ps, budget, false, 0);
  };
  REQUIRE(select({}) == BC_COMMITTED);
  const std::uint8_t* row = nullptr;
  std::uint32_t len = 0;
  REQUIRE(bc_ctx_next_result(f.cx, &row, &len));
  const auto exact = key.size() + stored.size() + 2 * kViewEntryBytes + 4 + len;
  CHECK(select(memory_budget(exact)) == BC_COMMITTED);
  CHECK(f.results(types) == std::vector<std::vector<bc_value>>{row1});
  CHECK(select(memory_budget(exact - 1)) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  // The budget is the statement's: the next one starts from nothing.
  CHECK(select(memory_budget(exact)) == BC_COMMITTED);
}

TEST_CASE("plan API: a scan holds one entry at a time against the memory budget", "[sql][algebra][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 50;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name-" + std::to_string(id)), id}) == BC_COMMITTED);
  }
  // 50 rows and their views hold over 3,000 bytes; a scan keeps only its
  // cursor, its bounds, the entry it is on and that entry's view.
  CHECK(run(f, scan_all, {}, memory_budget(kCursorEntryBytes + 256)) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
}

namespace {
// What the statement held at each step of the plans below.
std::vector<std::uint64_t> held_at_step;  // NOLINT: the plans are captureless

// Every row of t, and per row: its key built and read again, a row built
// from it, that row's key rebuilt and a column compared with param 0. What a
// long scan holds, step by step: views, builders and copies in each step.
void scan_holding(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    held_at_step.push_back(bc_ctx_held_for_testing(cx));
    bc_view row = bc_scan_row(cx, c);
    bc_key k = bc_key_begin(cx, kTbl);
    bc_key_i64(cx, k, bc_col_i64(cx, row, kIdCol));
    bool found = false;
    bc_view again = bc_get(cx, bc_key_end(cx, k), &found);
    bc_view built = bc_row_end(cx, bc_row_from(cx, kTbl, again));
    if (found && bc_bytes_eq(cx, bc_row_key(cx, kTbl, built), bc_scan_key(cx, c)) &&
        bc_bytes_eq(cx, bc_col_bytes(cx, built, kNameCol), bc_param_bytes(cx, 0))) {
      n = bc_add(cx, n, 1);
    }
  }
  bc_emit_count(cx, n);
}

// A join of t to itself, the inner row read by the primary key the outer
// row gives, with a COUNT(DISTINCT) of the inner qty: the shape of #551.
// The inner key, its builder, the inner row and the group key are the
// outer step's.
void self_join_distinct(bc_ctx* cx) {
  bc_agg groups = bc_agg_open(cx, 0);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    held_at_step.push_back(bc_ctx_held_for_testing(cx));
    bc_view row = bc_scan_row(cx, c);
    bool found = false;
    bc_key k = bc_key_begin(cx, kTbl);
    bc_key_i64(cx, k, bc_col_i64(cx, row, kIdCol));
    bc_view inner = bc_get(cx, bc_key_end(cx, k), &found);
    if (!found || bc_col_null(cx, inner, kQtyCol)) continue;
    // An inner scan too, opened and run to its end within the outer step.
    bc_view lo = bc_key_params(cx, kTbl, 0, 0);
    bc_cursor ic = bc_scan_open(cx, kTbl, lo, lo, BC_SCAN_PREFIX);
    if (bc_scan_next(cx, ic)) (void)bc_pin(cx, bc_scan_key(cx, ic));
    bc_scan_close(cx, ic);
    bc_agg_group(cx, groups, bc_col_key(cx, inner, kQtyCol));
  }
  int64_t n = 0;
  while (bc_agg_next(cx, groups, 0, 0)) n = bc_add(cx, n, 1);
  bc_emit_count(cx, n);
}

// The size of one int group key: the null marker and eight bytes.
constexpr std::uint64_t kIntGroupKey = 9;
std::uint64_t group_charge = 0;  // NOLINT: the plan is captureless
// One group made, then found again: what each charged.
void group_twice(bc_ctx* cx) {
  bc_agg groups = bc_agg_open(cx, 0);
  bc_view key = bc_key_params(cx, kTbl, 0, 1);
  bool found = false;
  bc_view row = bc_get(cx, key, &found);
  bc_view group_key = bc_col_key(cx, row, kQtyCol);
  auto before = bc_ctx_held_for_testing(cx);
  (void)bc_agg_group(cx, groups, group_key);
  group_charge = bc_ctx_held_for_testing(cx) - before;
  (void)bc_agg_group(cx, groups, group_key);
  bc_emit_count(cx, static_cast<int64_t>(bc_ctx_held_for_testing(cx) - before));
}
}  // namespace

TEST_CASE("plan API: a long scan holds the same bytes at every step", "[sql][algebra][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 2000;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);  // rows of one size
  }
  held_at_step.clear();
  CHECK(run(f, scan_holding, {bytes("name")}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
  REQUIRE(held_at_step.size() == kRows);
  // Each step's views, builders and copies went at the next step.
  CHECK(std::ranges::all_of(held_at_step, [&](auto h) { return h == held_at_step.front(); }));
  // So the scan runs in what one step holds, whatever the number of rows:
  // each step makes over 300 bytes of handles and copies, 600 KB in all.
  CHECK(run(f, scan_holding, {bytes("name")}, memory_budget(held_at_step.front() + 2048)) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
}

TEST_CASE("plan API: a join's inner key, row and group key go when its outer row moves on (#551)",
          "[sql][algebra][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 2000;
  const std::string name(300, 'n');  // inner rows of 300 bytes and more
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes(name), id % 7}) == BC_COMMITTED);
  }
  held_at_step.clear();
  CHECK(run(f, self_join_distinct, {}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 7);
  REQUIRE(held_at_step.size() == kRows);
  // Once each of the seven groups is made, the outer step holds the same:
  // only the pins grow, one key per outer row.
  const auto first = held_at_step[7];
  for (std::size_t i = 8; i < held_at_step.size(); ++i) {
    INFO("step " << i);
    REQUIRE(held_at_step[i] - held_at_step[i - 1] == held_at_step[8] - held_at_step[7]);
  }
  const auto per_pin = held_at_step[8] - held_at_step[7];
  CHECK(per_pin < 64);  // a pin's key and its view: not an inner row
  // 2,000 inner rows of over 300 bytes pass 600 KB; the join runs in a
  // budget of a few: what one outer step holds and the pins.
  const auto budget = first + kRows * per_pin + 4096;
  CHECK(budget < 128u << 10);
  CHECK(run(f, self_join_distinct, {}, memory_budget(budget)) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 7);
}

TEST_CASE("plan API: an aggregate group is charged its key and the aggregate's own node", "[sql][algebra][memory]") {
  Fixture f;
  REQUIRE(run(f, insert_plan_ok, {std::int64_t{1}, bytes("n"), std::int64_t{5}}) == BC_COMMITTED);
  CHECK(run(f, group_twice, {std::int64_t{1}}) == BC_COMMITTED);
  CHECK(group_charge == kIntGroupKey + kGroupEntryBytes);
  // Finding the group again charges nothing.
  CHECK(bc_ctx_affected(f.cx) == static_cast<std::int64_t>(kIntGroupKey + kGroupEntryBytes));
}

namespace {
// Every row of t, its qty plus one: a write set that grows with the table.
void bump_all(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_row r = bc_row_from(cx, kTbl, row);
    bc_row_set_i64(cx, r, kQtyCol, bc_add(cx, bc_col_i64(cx, row, kQtyCol), 1));
    bc_update(cx, kTbl, row, bc_row_end(cx, r));
    n = bc_add(cx, n, 1);
  }
  bc_emit_count(cx, n);
}

auto qty_of(Fixture& f, std::int64_t id) -> std::int64_t {
  Bytes k, row;
  bytecask::sql::key_put_prefix(k, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(k, id, {PhysType::I64});
  REQUIRE(f.db->get({}, k, row));
  auto cols = bytecask::sql::decode_row(f.cat().table(kTbl)->layout, row);
  REQUIRE(cols);
  return std::get<std::int64_t>((*cols)[kQtyCol]);
}
}  // namespace

namespace {
// Every row of t, its qty set to param 0: the rows it puts are the same size
// whatever the qty was.
void set_all(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_row r = bc_row_from(cx, kTbl, row);
    bc_row_set_param(cx, r, kQtyCol, 0);
    bc_update(cx, kTbl, row, bc_row_end(cx, r));
    n = bc_add(cx, n, 1);
  }
  bc_emit_count(cx, n);
}

// SELECT … FOR UPDATE over every row of t; it then sets the qty of the row
// whose id is param 0, if one is, to 0.
void lock_all(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_guard_row(cx, kTbl, row);
    if (bc_col_i64(cx, row, kIdCol) == bc_param_i64(cx, 0)) {
      bc_row r = bc_row_from(cx, kTbl, row);
      bc_row_set_i64(cx, r, kQtyCol, 0);
      bc_update(cx, kTbl, row, bc_row_end(cx, r));
    }
    n = bc_add(cx, n, 1);
  }
  bc_emit_count(cx, n);
}

// The bytes the engine appends for a plan that puts each of t's rows as it
// is now (docs/file_format.md): an entry of a 15-byte header, the key, the
// value and a 4-byte CRC per row, and two empty markers around them.
auto rows_on_disk(Fixture& f) -> std::uint64_t {
  constexpr std::uint64_t kEntry = 15 + 4;
  Bytes prefix;
  bytecask::sql::key_put_prefix(prefix, bytecask::sql::kNsRow, kTbl);
  std::uint64_t bytes = 2 * kEntry;
  for (const auto& [key, value] : f.db->iter_from({}, prefix)) {
    if (!std::ranges::equal(std::span{key}.first(std::min(key.size(), prefix.size())), prefix)) break;
    bytes += kEntry + key.size() + value.size();
  }
  return bytes;
}
}  // namespace

TEST_CASE("plan API: an autocommit write set is held to the engine's batch limit by its bytes on disk",
          "[sql][algebra][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 200;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);
  }
  // The write set is not the statement budget's: 200 updates hold far more
  // than 16 KiB, and each row built goes with its step.
  CHECK(run(f, set_all, {std::int64_t{7}}, memory_budget(16u << 10)) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
  // What the next UPDATE appends: its rows are these rows' size.
  const auto on_disk = rows_on_disk(f);
  // A byte short of it: the update that passes the limit fails the
  // statement, whatever its budget, and nothing is committed.
  bc_ctx_write_bound_for_testing(f.cx, on_disk - 1);
  CHECK(run(f, set_all, {std::int64_t{8}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(std::string{bc_error_message(f.cx)}.find("batch limit of " + std::to_string(on_disk - 1) + " bytes") !=
        std::string::npos);
  CHECK(bc_ctx_affected(f.cx) == -1);
  CHECK(qty_of(f, 1) == 7);
  CHECK(qty_of(f, kRows) == 7);
  // At it, the same update commits: the engine takes the plan.
  bc_ctx_write_bound_for_testing(f.cx, on_disk);
  CHECK(run(f, set_all, {std::int64_t{8}}) == BC_COMMITTED);
  CHECK(qty_of(f, 1) == 8);
  CHECK(qty_of(f, kRows) == 8);
}

TEST_CASE("plan API: an autocommit SELECT FOR UPDATE's guards are not held to the batch limit",
          "[sql][algebra][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 200;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);
  }
  // A guard writes nothing: a guard-only execution commits nothing, and 200
  // guards are not refused under a limit of one byte.
  bc_ctx_write_bound_for_testing(f.cx, 1);
  CHECK(run(f, lock_all, {std::int64_t{0}}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
  // With one write among them, the limit counts that write's entry alone:
  // one write, no markers.
  Bytes key, row;
  bytecask::sql::key_put_prefix(key, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(key, 1, {PhysType::I64});
  REQUIRE(f.db->get({}, key, row));
  const auto one_entry = 15 + key.size() + row.size() + 4;  // qty 1 and 0 encode alike
  bc_ctx_write_bound_for_testing(f.cx, one_entry - 1);
  CHECK(run(f, lock_all, {std::int64_t{1}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(qty_of(f, 1) == 1);
  bc_ctx_write_bound_for_testing(f.cx, one_entry);
  CHECK(run(f, lock_all, {std::int64_t{1}}) == BC_COMMITTED);
  CHECK(qty_of(f, 1) == 0);
}

TEST_CASE("plan API: a transaction statement's write set past the room left in its bound fails at the call",
          "[sql][algebra][memory][txn]") {
  Fixture f;
  constexpr std::int64_t kRows = 200;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);
  }
  bc_ctx_reset(f.cx);
  auto* txn = bc_txn_new(&f.bdb, &f.bcat, 16u << 10);
  REQUIRE(txn != nullptr);
  auto ps = params({});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  bc_ctx_begin_txn(f.cx, txn, &ps, {});
  bc_ctx_run(f.cx, bump_all, &ref);
  // The plan stopped at the update that passed it, before bc_txn_add.
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(std::string{bc_error_message(f.cx)}.find("transaction's writes would pass its bound of 16384") !=
        std::string::npos);
  CHECK_FALSE(bc_txn_add(f.cx));
  bc_ctx_reset(f.cx);
  CHECK(bc_txn_commit(txn, false) == BC_FAILED);  // aborted: nothing to commit
  bc_txn_free(txn);
  CHECK(qty_of(f, 1) == 1);
  CHECK(qty_of(f, kRows) == kRows);
}

namespace {
// set_all as a transaction's one statement under max_bytes: the statement's
// error, whether bc_txn_add took it in, and, with `commit`, the commit's
// result (else the transaction is rolled back).
struct TxnRun {
  bc_error_code run;
  std::string message;
  bool added;
  int committed;
};
auto txn_set_all(Fixture& f, std::uint64_t max_bytes, std::int64_t qty, bool commit, bc_budget budget = {})
    -> TxnRun {
  bc_ctx_reset(f.cx);
  auto* txn = bc_txn_new(&f.bdb, &f.bcat, max_bytes);
  REQUIRE(txn != nullptr);
  auto ps = params({qty});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  bc_ctx_begin_txn(f.cx, txn, &ps, budget);
  bc_ctx_run(f.cx, set_all, &ref);
  TxnRun out{bc_error(f.cx), bc_error_message(f.cx), false, BC_FAILED};
  out.added = bc_txn_add(f.cx);
  bc_ctx_reset(f.cx);
  if (commit && out.added) out.committed = bc_txn_commit(txn, false);
  bc_txn_free(txn);
  return out;
}
}  // namespace

TEST_CASE("plan API: a transaction statement's writes and bc_txn_add agree on the bound at its boundary",
          "[sql][algebra][memory][txn]") {
  Fixture f;
  constexpr std::int64_t kRows = 200;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);
  }
  // The least bound the statement runs under, found by bisection.
  std::uint64_t lo = 1, hi = 64u << 20;
  REQUIRE(txn_set_all(f, hi, 7, false).run == BC_OK);
  while (lo < hi) {
    auto mid = lo + (hi - lo) / 2;
    if (txn_set_all(f, mid, 7, false).run == BC_OK) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  // At it, bc_txn_add takes the statement in and the commit lands; a byte
  // below it, the statement fails at the write that passes it, before
  // bc_txn_add, which then refuses it.
  auto at = txn_set_all(f, lo, 7, true);
  CHECK(at.run == BC_OK);
  CHECK(at.added);
  CHECK(at.committed == BC_COMMITTED);
  CHECK(qty_of(f, kRows) == 7);
  auto below = txn_set_all(f, lo - 1, 8, true);
  CHECK(below.run == BC_E_LIMIT);
  CHECK(below.message.find("transaction's writes would pass its bound of " + std::to_string(lo - 1)) !=
        std::string::npos);
  CHECK_FALSE(below.added);
  CHECK(qty_of(f, kRows) == 7);
  // The statement's scratch (the rows it wrote, its vectors' slots) is
  // statement memory: under a bound with room, a budget it passes refuses it.
  auto budgeted = txn_set_all(f, 1u << 30, 8, false, memory_budget(16u << 10));
  CHECK(budgeted.run == BC_E_LIMIT);
  CHECK(budgeted.message.find("memory budget") != std::string::npos);
  CHECK_FALSE(budgeted.added);
}

namespace {
// A loop left after one step, its cursor closed right after it, as codegen
// emits for MIN and LIMIT; a view made after it, outside every loop, read
// after another loop has run. With `close` false the cursor is left on its
// entry, and the view is that step's.
template <bool close>
void early_exit(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t first = 0;
  if (bc_scan_next(cx, c)) first = bc_col_i64(cx, bc_scan_row(cx, c), kIdCol);
  if (close) bc_scan_close(cx, c);
  bc_view after = bc_key_params(cx, kTbl, 0, 1);
  bc_cursor d = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, d)) {
  }
  if (!close) bc_scan_close(cx, c);
  bool found = false;
  (void)bc_get(cx, after, &found);
  bc_emit_count(cx, found ? first : -1);
}
}  // namespace

TEST_CASE("plan API: a view made after a loop left early and closed stays valid", "[sql][algebra][memory]") {
  Fixture f;
  for (std::int64_t id = 1; id <= 5; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);
  }
  CHECK(run(f, early_exit<true>, {std::int64_t{3}}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1);
  // Left on its entry, the cursor's step is still open: the view made after
  // the loop is that step's, and goes when the cursor closes.
  CHECK(run(f, early_exit<false>, {std::int64_t{3}}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
// For the first outer row, an inner scan that reads the outer row's group
// key at every step: what the statement held at each inner step.
void outer_key_in_inner_loop(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  if (bc_scan_next(cx, c)) {
    bc_view outer = bc_scan_row(cx, c);
    bc_cursor ic = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
    while (bc_scan_next(cx, ic)) {
      held_at_step.push_back(bc_ctx_held_for_testing(cx));
      if (bc_bytes_eq(cx, bc_col_key(cx, outer, kQtyCol), bc_col_key(cx, bc_scan_row(cx, ic), kQtyCol))) {
        n = bc_add(cx, n, 1);
      }
    }
  }
  bc_scan_close(cx, c);
  bc_emit_count(cx, n);
}
}  // namespace

TEST_CASE("plan API: an outer row's key read in an inner loop is the inner step's", "[sql][algebra][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 500;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id % 2}) == BC_COMMITTED);
  }
  held_at_step.clear();
  CHECK(run(f, outer_key_in_inner_loop, {}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows / 2);
  REQUIRE(held_at_step.size() == kRows);
  // Each inner step's keys, the outer row's included, went at the next step.
  CHECK(std::ranges::all_of(held_at_step, [&](auto h) { return h == held_at_step.front(); }));
}
