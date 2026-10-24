// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The plan API's runtime guarantees, driven by small plans written as
// captureless lambdas: handles are checked, errors are sticky, a cursor's
// views die when it steps, the budget ends a plan, nothing is committed
// unless the plan finishes cleanly, and the sorters, hash tables and
// aggregates order, keep and charge what they are given.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
using bytecask::sql::kCursorEntryBytes;
using bytecask::sql::kAggBytes;
using bytecask::sql::kAggSlotBytes;
using bytecask::sql::kGroupEntryBytes;
using bytecask::sql::kHashBytes;
using bytecask::sql::kHashEntryBytes;
using bytecask::sql::kHashIterEntryBytes;
using bytecask::sql::kKeyEntryBytes;
using bytecask::sql::kRowEntryBytes;
using bytecask::sql::kSortEntryBytes;
using bytecask::sql::kSorterBytes;
using bytecask::sql::kViewEntryBytes;
using bytecask::sql::buffer_bytes;
using bytecask::sql::BytesView;
using bytecask::sql::claim_bytes;
using bytecask::sql::effect_bytes;
using bytecask::sql::kKeptWriteSlots;
using bytecask::sql::kOverlayNodeBytes;
using bytecask::sql::kRangeNodeBytes;
using bytecask::sql::kTableSlotBytes;
using bytecask::sql::kUniqueNodeBytes;
using bytecask::sql::kWrittenRowNodeBytes;
using bytecask::sql::lock_bytes;
using bytecask::sql::PhysType;
using bytecask::sql::plan_entry_bytes;
using bytecask::sql::PlanEntry;
using bytecask::sql::table_bytes;

namespace {

constexpr std::uint32_t kTbl = 1;
constexpr std::uint32_t kIdx = 3;
constexpr std::uint32_t kIdCol = 0, kNameCol = 1, kQtyCol = 2;

// An autocommit statement's charges: an effect with its entry in the
// commit's WritePlan, and t's slot with the commit's guard on its record
// (a transaction's statement is charged the same, and a write's overlay
// node).
auto write_charge(std::size_t a, std::size_t b = 0) -> std::uint64_t {
  return effect_bytes(false, PlanEntry::Write, a, b);
}
auto table_charge() -> std::uint64_t { return table_bytes(bytecask::sql::table_record_key(kTbl).size()); }

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
  // An insert holds the row it built, its builder and its view, and its
  // write set: the row written, its key's claim, its put (t's first write)
  // and its index entry's. Row 2 is row 1's size, and measures it.
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
  const auto& t = *f.cat().table(kTbl);
  Bytes entry;
  std::size_t prefix_len = 0;
  bool has_null = false;
  REQUIRE(bytecask::sql::encode_index_key_from_row(t, t.indexes.at(0), stored, entry, &prefix_len, &has_null));
  // The keys the insert encodes are moved into its effects and charged their
  // capacity; row 2's encode as row 1's do.
  Bytes encoded;
  REQUIRE(bytecask::sql::encode_pk_from_row(t, stored, encoded));
  REQUIRE(encoded.size() == key.size());
  const auto write_set = buffer_bytes(key.size()) + kWrittenRowNodeBytes + claim_bytes(key.size()) +
                         write_charge(encoded.capacity(), stored.size()) + table_charge() +
                         write_charge(entry.capacity());
  const auto insert_holds = kRowEntryBytes + stored.size() + kViewEntryBytes + write_set;
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

// The bytes held right before and right after each write call a probed
// plan makes.
std::vector<std::pair<std::uint64_t, std::uint64_t>> held_around;

// SELECT … FOR UPDATE over every row of t, each guard probed; it then sets
// the qty of the row whose id is param 0, if one is, to 0.
void lock_all(bc_ctx* cx) {
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  int64_t n = 0;
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    auto before = bc_ctx_held_for_testing(cx);
    bc_guard_row(cx, kTbl, row);
    held_around.emplace_back(before, bc_ctx_held_for_testing(cx));
    if (bc_col_i64(cx, row, kIdCol) == bc_param_i64(cx, 0)) {
      bc_row r = bc_row_from(cx, kTbl, row);
      bc_row_set_i64(cx, r, kQtyCol, 0);
      bc_update(cx, kTbl, row, bc_row_end(cx, r));
    }
    n = bc_add(cx, n, 1);
  }
  bc_emit_count(cx, n);
}

// insert_plan_ok, bc_insert probed.
void probed_insert(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_row_set_param(cx, r, kNameCol, 1);
  bc_row_set_param(cx, r, kQtyCol, 2);
  bc_view row = bc_row_end(cx, r);
  auto before = bc_ctx_held_for_testing(cx);
  bc_insert(cx, kTbl, row);
  held_around.emplace_back(before, bc_ctx_held_for_testing(cx));
}

// Deletes the row whose id is param 0, then the ids in [param 1, param 2)
// by range; each call probed.
void probed_deletes(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  auto before = bc_ctx_held_for_testing(cx);
  if (found) bc_delete(cx, kTbl, row);
  held_around.emplace_back(before, bc_ctx_held_for_testing(cx));
  bc_view lo = bc_key_params(cx, kTbl, 1, 1);
  bc_view hi = bc_key_params(cx, kTbl, 2, 1);
  before = bc_ctx_held_for_testing(cx);
  bc_del_range(cx, kTbl, lo, hi);
  held_around.emplace_back(before, bc_ctx_held_for_testing(cx));
}

// Sets the qty of the row whose id is param 0 to param 1, bc_update probed.
void probed_set_qty(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, kTbl, 0, 1), &found);
  if (!found) return;
  bc_row r = bc_row_from(cx, kTbl, row);
  bc_row_set_param(cx, r, kQtyCol, 1);
  bc_view next = bc_row_end(cx, r);
  auto before = bc_ctx_held_for_testing(cx);
  bc_update(cx, kTbl, row, next);
  held_around.emplace_back(before, bc_ctx_held_for_testing(cx));
}

auto pk_of(std::int64_t id) -> Bytes {
  Bytes k;
  bytecask::sql::key_put_prefix(k, bytecask::sql::kNsRow, kTbl);
  bytecask::sql::key_put_i64(k, id, {PhysType::I64});
  return k;
}

auto stored_row(Fixture& f, std::int64_t id) -> Bytes {
  Bytes row;
  REQUIRE(f.db->get({}, pk_of(id), row));
  return row;
}

// The entry `row` has in t's index `i`, and the length of its prefix.
auto entry_of(Fixture& f, std::size_t i, BytesView row) -> std::pair<Bytes, std::size_t> {
  const auto& t = *f.cat().table(kTbl);
  std::pair<Bytes, std::size_t> out;
  bool has_null = false;
  REQUIRE(bytecask::sql::encode_index_key_from_row(t, t.indexes.at(i), row, out.first, &out.second, &has_null));
  return out;
}

auto index_count(Fixture& f) -> std::size_t { return f.cat().table(kTbl)->indexes.size(); }

// The capacity of `row`'s primary key as the runtime encodes it: a key it
// encodes is moved into the write set and charged its capacity.
auto pk_capacity(Fixture& f, BytesView row) -> std::size_t {
  Bytes k;
  REQUIRE(bytecask::sql::encode_pk_from_row(*f.cat().table(kTbl), row, k));
  return k.capacity();
}
}  // namespace

TEST_CASE("plan API: each item of an insert's write set is charged to the statement budget as it is added",
          "[sql][algebra][memory]") {
  Fixture f;
  f.ddl(bytecask::sql::CreateIndexDdl{"t_by_qty", "t", {{"qty"}}, true});
  REQUIRE(index_count(f) == 2);
  held_around.clear();
  REQUIRE(run(f, probed_insert, {std::int64_t{1}, bytes("alice"), std::int64_t{5}}) == BC_COMMITTED);
  REQUIRE(held_around.size() == 1);
  const auto [before, after] = held_around[0];
  const auto pk = pk_of(1);
  const auto row = stored_row(f, 1);
  // A key the runtime encoded is moved into its effect and charged its
  // capacity, whatever its size; a copy of a view, its size.
  std::uint64_t puts = 0;
  for (std::size_t i = 0; i < index_count(f); ++i) puts += write_charge(entry_of(f, i, row).first.capacity());
  const auto [unique_entry, prefix_len] = entry_of(f, 1, row);
  const auto prefix = BytesView{unique_entry}.first(prefix_len);
  const auto last_put = write_charge(entry_of(f, index_count(f) - 1, row).first.capacity());
  const auto expected = buffer_bytes(pk.size()) + kWrittenRowNodeBytes  // the row written, to refuse a second write
                        + claim_bytes(pk.size())                         // its key, free in the snapshot
                        + buffer_bytes(prefix.size()) + kUniqueNodeBytes  // the unique prefix, likewise
                        + effect_bytes(false, PlanEntry::RangeGuard, prefix.size(),
                                       bytecask::sql::key_successor(prefix).size())  // its guard
                        + claim_bytes(unique_entry.size())                            // and its claim
                        + write_charge(pk_capacity(f, row), row.size()) + table_charge()  // the row, t's first write
                        + puts;                                           // and its index entries
  CHECK(after - before == expected);

  // Row 2 is row 1's size. A byte short, the last entry's put is refused:
  // the statement fails, nothing is committed, and the refused put is not
  // charged.
  held_around.clear();
  const std::vector<bc_value> row2{std::int64_t{2}, bytes("bobby"), std::int64_t{6}};
  CHECK(run(f, probed_insert, row2, memory_budget(before + expected - 1)) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  CHECK(std::string{bc_error_message(f.cx)}.find("memory budget") != std::string::npos);
  REQUIRE(held_around.size() == 1);
  CHECK(held_around[0].first == before);
  CHECK(held_around[0].second == before + expected - last_put);
  Bytes out;
  CHECK_FALSE(f.db->get({}, pk_of(2), out));
  CHECK(run(f, probed_insert, row2, memory_budget(before + expected)) == BC_COMMITTED);
  CHECK(f.db->get({}, pk_of(2), out));
}

TEST_CASE("plan API: a delete, a range delete and FOR UPDATE's guards are charged to the statement budget",
          "[sql][algebra][memory]") {
  Fixture f;
  for (std::int64_t id = 1; id <= 10; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("n"), id}) == BC_COMMITTED);
  }
  auto entry_put = [&](std::int64_t id) { return write_charge(entry_of(f, 0, stored_row(f, id)).first.capacity()); };
  const auto pk_cap = pk_capacity(f, stored_row(f, 1));
  // A delete: the row written, its tombstone (t's first write) and its
  // entry's. A range delete: the range, each row's entry's tombstone, and
  // the range's own.
  const auto pk1 = pk_of(1), lo = pk_of(5), hi = pk_of(8);
  const auto one =
      buffer_bytes(pk1.size()) + kWrittenRowNodeBytes + write_charge(pk_cap) + table_charge() + entry_put(1);
  const auto range = buffer_bytes(lo.size()) + buffer_bytes(hi.size()) + kRangeNodeBytes + entry_put(5) +
                     entry_put(6) + entry_put(7) + write_charge(lo.size(), hi.size());
  held_around.clear();
  REQUIRE(run(f, probed_deletes, {std::int64_t{1}, std::int64_t{5}, std::int64_t{8}}) == BC_COMMITTED);
  REQUIRE(held_around.size() == 2);
  CHECK(held_around[0].second - held_around[0].first == one);
  CHECK(held_around[1].second - held_around[1].first == range);
  CHECK(bc_ctx_held_for_testing(f.cx) == held_around[1].second);

  // An autocommit SELECT … FOR UPDATE: each guard is its key's.
  held_around.clear();
  REQUIRE(run(f, lock_all, {std::int64_t{0}}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 6);
  REQUIRE(held_around.size() == 6);
  for (const auto& [b, a] : held_around) CHECK(a - b == effect_bytes(false, PlanEntry::KeyGuard, pk_cap, 0));
  // A byte short of the last guard: it is refused and charges nothing.
  const auto last = held_around.back();
  held_around.clear();
  CHECK(run(f, lock_all, {std::int64_t{0}}, memory_budget(last.second - 1)) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LIMIT);
  REQUIRE(held_around.size() == 6);
  CHECK(held_around.back().first == last.first);
  CHECK(held_around.back().second == last.first);
}

TEST_CASE("plan API: a reset keeps the write set's small slot buffers and frees large ones",
          "[sql][algebra][memory]") {
  Fixture f;
  for (std::int64_t id = 1; id <= 100; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("n"), id}) == BC_COMMITTED);
  }
  // A one-row insert's slots stay after a reset, cleared and uncharged, and
  // the next one reuses them.
  REQUIRE(run(f, insert_plan_ok, {std::int64_t{101}, bytes("n"), std::int64_t{1}}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  const auto kept = bc_ctx_write_set_slots_for_testing(f.cx);
  CHECK(kept > 0);
  CHECK(kept <= 4 * kKeptWriteSlots);
  CHECK(bc_ctx_held_for_testing(f.cx) == 0);
  REQUIRE(run(f, insert_plan_ok, {std::int64_t{102}, bytes("n"), std::int64_t{1}}) == BC_COMMITTED);
  bc_ctx_reset(f.cx);
  CHECK(bc_ctx_write_set_slots_for_testing(f.cx) == kept);

  // An update of every row: its effects, locks and tables, committed, then
  // reset. The commit empties the effects but leaves their slots, past what
  // a reset keeps.
  REQUIRE(run(f, bump_all, {}) == BC_COMMITTED);
  CHECK(bc_ctx_write_set_slots_for_testing(f.cx) >= 100);
  bc_ctx_reset(f.cx);
  CHECK(bc_ctx_write_set_slots_for_testing(f.cx) <= 4 * kKeptWriteSlots);
  CHECK(bc_ctx_held_for_testing(f.cx) == 0);

  // In a transaction: bc_txn_add moves the items out, and the reset frees
  // the large slot buffers they leave.
  auto* txn = bc_txn_new(&f.bdb, &f.bcat, 64u << 20);
  REQUIRE(txn != nullptr);
  auto ps = params({});
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  bc_ctx_begin_txn(f.cx, txn, &ps, {});
  bc_ctx_run(f.cx, bump_all, &ref);
  REQUIRE(bc_txn_add(f.cx));
  CHECK(bc_ctx_write_set_slots_for_testing(f.cx) >= 200);  // effects, and the locks only a transaction keeps
  bc_ctx_reset(f.cx);
  CHECK(bc_ctx_write_set_slots_for_testing(f.cx) <= 4 * kKeptWriteSlots);
  CHECK(bc_txn_commit(txn, false) == BC_COMMITTED);
  bc_txn_free(txn);
  CHECK(qty_of(f, 100) == 102);
}

namespace {
// Runs probed_set_qty for each (id, qty) as a statement of one transaction
// under max_bytes, adding each and committing after the last: the error of
// the first statement that failed, or BC_OK; the bytes the context held
// before and after each bc_txn_add; and the commit's result.
struct TxnSteps {
  bc_error_code error{BC_OK};
  std::string message;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> held_at_add;
  int committed{BC_FAILED};
  bc_error_code commit_error{BC_OK};
};
auto txn_set_qtys(Fixture& f, std::uint64_t max_bytes, std::vector<std::pair<std::int64_t, std::int64_t>> sets)
    -> TxnSteps {
  bc_ctx_reset(f.cx);
  auto* txn = bc_txn_new(&f.bdb, &f.bcat, max_bytes);
  REQUIRE(txn != nullptr);
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), {}, f.cat().version()};
  auto ref = sr.get();
  TxnSteps out;
  for (const auto& [id, qty] : sets) {
    auto ps = params({id, qty});
    bc_ctx_begin_txn(f.cx, txn, &ps, {});
    bc_ctx_run(f.cx, probed_set_qty, &ref);
    auto before = bc_ctx_held_for_testing(f.cx);
    if (bc_error(f.cx) != BC_OK) {
      out.error = bc_error(f.cx);
      out.message = bc_error_message(f.cx);
    }
    auto added = bc_txn_add(f.cx);
    out.held_at_add.emplace_back(before, bc_ctx_held_for_testing(f.cx));
    bc_ctx_reset(f.cx);
    if (!added) break;
  }
  if (out.error == BC_OK) {
    out.committed = bc_txn_commit(txn, false);
    out.commit_error = bc_txn_error(txn);
  }
  bc_txn_free(txn);
  return out;
}
}  // namespace

TEST_CASE("plan API: bc_txn_add moves the write set from the statement budget to the transaction's bound",
          "[sql][algebra][memory][txn]") {
  Fixture f;
  for (std::int64_t id = 1; id <= 3; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);
  }
  // An update of one row's qty in a transaction: the row written (statement
  // scratch, which stays), and the write set: its lock, its put (t's first
  // write) with its overlay node and its entry in the commit's WritePlan,
  // and t's slot with the commit's guard on its record. No index entry
  // moves. The rows' keys are alike, and qty 1 to 7 encode alike.
  const auto pk = pk_of(1);
  const auto row = stored_row(f, 1);
  const auto write_set = lock_bytes(pk.size()) +
                         effect_bytes(true, PlanEntry::Write, pk_capacity(f, row), row.size()) + table_charge();
  // Once added, the transaction holds all of it but the effect's slot: the
  // overlay node with its plan entry, the lock, and t.
  const auto txn_holds = write_set - bytecask::sql::kEffectSlotBytes;
  held_around.clear();
  auto at = txn_set_qtys(f, write_set, {{1, 7}});
  CHECK(at.error == BC_OK);
  REQUIRE(held_around.size() == 1);
  CHECK(held_around[0].second - held_around[0].first == write_set + buffer_bytes(pk.size()) + kWrittenRowNodeBytes);
  // bc_txn_add releases the write set from the statement, and takes it in
  // at a bound of exactly its size. The commit's copy was counted with it:
  // the commit needs nothing more, and commits.
  REQUIRE(at.held_at_add.size() == 1);
  CHECK(at.held_at_add[0].first - at.held_at_add[0].second == write_set);
  CHECK(at.committed == BC_COMMITTED);
  CHECK(at.commit_error == BC_OK);
  CHECK(qty_of(f, 1) == 7);
  // A byte below, the update fails at the call, and nothing commits.
  auto below = txn_set_qtys(f, write_set - 1, {{1, 1}});
  CHECK(below.error == BC_E_LIMIT);
  CHECK(below.message.find("transaction's writes would pass its bound of " + std::to_string(write_set - 1)) !=
        std::string::npos);
  CHECK(qty_of(f, 1) == 7);

  // A second statement's write set, the same as the first's, is held to
  // what the transaction holds, counted by the same sizes. Filled to its
  // bound, the transaction commits; a write past it fails at its statement,
  // never at COMMIT.
  const auto two = txn_holds + write_set;
  auto both = txn_set_qtys(f, two, {{1, 2}, {2, 3}});
  CHECK(both.error == BC_OK);
  REQUIRE(both.held_at_add.size() == 2);
  CHECK(both.held_at_add[1].first - both.held_at_add[1].second == write_set);
  CHECK(both.committed == BC_COMMITTED);
  CHECK(both.commit_error == BC_OK);
  CHECK(qty_of(f, 1) == 2);
  CHECK(qty_of(f, 2) == 3);
  auto short_of = txn_set_qtys(f, two - 1, {{1, 4}, {2, 5}});
  CHECK(short_of.error == BC_E_LIMIT);
  CHECK(short_of.held_at_add.size() == 2);  // the second statement, refused
  auto third = txn_set_qtys(f, two, {{1, 4}, {2, 5}, {3, 6}});
  CHECK(third.error == BC_E_LIMIT);
  CHECK(third.held_at_add.size() == 3);
  CHECK(third.message.find("transaction's writes would pass its bound of " + std::to_string(two)) !=
        std::string::npos);
  CHECK(qty_of(f, 1) == 2);
  CHECK(qty_of(f, 2) == 3);
  CHECK(qty_of(f, 3) == 3);
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
  // At it, bc_txn_add takes the statement in, and the commit, whose copy
  // the statement was charged, commits. A byte below it, the statement fails
  // at the write that passes it, before bc_txn_add, which then refuses it.
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

// ------------------------------------------------------ ephemeral structures

namespace {

// A run whose result rows have `types`.
auto run_rows(Fixture& f, bc_plan_fn plan, std::vector<bc_value> p, std::vector<PhysType> types, bc_budget budget = {})
    -> int {
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes(types), f.cat().version()};
  auto ref = sr.get();
  return bc_execute(f.cx, plan, &ref, &ps, budget, false, 0);
}

auto ids(Fixture& f) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  for (const auto& r : f.results(std::vector{PhysType::I64})) out.push_back(std::get<std::int64_t>(r[0]));
  return out;
}

// Rows (id, "n", qty) for qty in `qtys`, ids from 1; nullopt is NULL.
void insert_qtys(Fixture& f, std::vector<std::optional<std::int64_t>> qtys) {
  std::int64_t id = 0;
  for (auto q : qtys) {
    REQUIRE(run(f, insert_plan_ok, {++id, bytes("n"), q ? bc_value{*q} : bc_value{}}) == BC_COMMITTED);
  }
}

// SELECT id FROM t ORDER BY qty, NULLs first as the key codec sorts them,
// keeping the first $1 (0: all); the scan forward, or reverse with $2. The
// count is the sorter's.
void ids_by_qty(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, bc_param_i64(cx, 0));
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx),
                             bc_param_bool(cx, 1) ? BC_SCAN_REVERSE : BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sorter_add(cx, s, bc_col_key(cx, row, kQtyCol), row);
  }
  bc_view p = bc_empty(cx);
  while (bc_sorter_next(cx, s, &p)) {
    bc_emit_begin(cx);
    bc_emit_col(cx, p, kIdCol);  // the payload is still a row of t
    bc_emit_end(cx);
  }
  bc_emit_count(cx, bc_sorter_count(cx, s));
}

// The same sort read by position: the id of the $2-th row, then the count.
void nth_by_qty(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, bc_param_i64(cx, 0));
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sorter_add(cx, s, bc_col_key(cx, row, kQtyCol), row);
  }
  bc_view p = bc_sorter_nth(cx, s, bc_param_i64(cx, 1));
  bc_emit_begin(cx);
  bc_emit_col(cx, p, kIdCol);
  bc_emit_end(cx);
  bc_emit_count(cx, bc_sorter_count(cx, s));
}

// A sorter added to after it was read.
void add_after_read(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_sorter_add(cx, s, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  (void)bc_sorter_nth(cx, s, 0);
  bc_sorter_add(cx, s, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
}

// A sorter's payload read after its loop moved on.
void stale_sorted(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_sorter_add(cx, s, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  bc_sorter_add(cx, s, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  bc_view first = bc_empty(cx);
  bc_view p = bc_empty(cx);
  bool started = false;
  while (bc_sorter_next(cx, s, &p)) {
    if (started) bc_emit_count(cx, bc_bytes_eq(cx, first, p) ? 1 : 0);
    first = p;
    started = true;
  }
}
}  // namespace

TEST_CASE("plan API: a sorter returns its payloads in sort key order, equal keys in the order added",
          "[sql][algebra][structures]") {
  Fixture f;
  insert_qtys(f, {3, 1, std::nullopt, 1, 2, 3});
  CHECK(run_rows(f, ids_by_qty, {std::int64_t{0}, false}, {PhysType::I64}) == BC_COMMITTED);
  CHECK(ids(f) == std::vector<std::int64_t>{3, 2, 4, 5, 1, 6});
  CHECK(bc_ctx_affected(f.cx) == 6);
  // Added in reverse, equal keys come back in reverse: the order added, not
  // the payloads'.
  CHECK(run_rows(f, ids_by_qty, {std::int64_t{0}, true}, {PhysType::I64}) == BC_COMMITTED);
  CHECK(ids(f) == std::vector<std::int64_t>{3, 4, 2, 5, 6, 1});

  for (std::int64_t n = 0; n < 6; ++n) {
    INFO("nth " << n);
    REQUIRE(run_rows(f, nth_by_qty, {std::int64_t{0}, n}, {PhysType::I64}) == BC_COMMITTED);
    CHECK(ids(f) == std::vector<std::int64_t>{std::vector<std::int64_t>{3, 2, 4, 5, 1, 6}[static_cast<std::size_t>(n)]});
  }
  CHECK(run_rows(f, nth_by_qty, {std::int64_t{0}, std::int64_t{6}}, {PhysType::I64}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  CHECK(run_rows(f, nth_by_qty, {std::int64_t{0}, std::int64_t{-1}}, {PhysType::I64}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);

  CHECK(run(f, add_after_read, {bytes("k")}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
  // A payload is its step's: the next step makes it stale.
  CHECK(run(f, stale_sorted, {bytes("k")}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
std::vector<std::uint64_t> held_at_nth;  // NOLINT: the plans are captureless
std::vector<std::int64_t> ids_at_nth;    // NOLINT: likewise

// Every row of t into a sorter, then each entry by bc_sorter_nth outside
// every loop, as a plan's top-level for over nth reads them: the id of each,
// and what the statement held after each call. Nothing is emitted, since a
// result row is charged too.
void each_nth(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sorter_add(cx, s, bc_col_key(cx, row, kQtyCol), row);
  }
  for (int64_t i = 0; i < bc_sorter_count(cx, s); ++i) {
    bc_view p = bc_sorter_nth(cx, s, i);
    held_at_nth.push_back(bc_ctx_held_for_testing(cx));
    ids_at_nth.push_back(bc_col_i64(cx, p, kIdCol));
  }
}

// An nth view read after the next bc_sorter_nth on its sorter; with $1, on
// another sorter, which leaves it live.
void nth_after_next_nth(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_sorter t = bc_sorter_open(cx, 0);
  bc_sorter_add(cx, s, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  bc_sorter_add(cx, t, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  bc_view first = bc_sorter_nth(cx, s, 0);
  (void)bc_sorter_nth(cx, bc_param_bool(cx, 1) ? t : s, 0);
  bc_emit_count(cx, bc_bytes_eq(cx, first, bc_param_bytes(cx, 0)) ? 1 : 0);
}
}  // namespace

TEST_CASE("plan API: a sorter's nth view lives until the next bc_sorter_nth on it, which takes its slot",
          "[sql][algebra][structures][memory]") {
  Fixture f;
  insert_qtys(f, {3, 1, std::nullopt, 1, 2, 3});
  held_at_nth.clear();
  ids_at_nth.clear();
  CHECK(run(f, each_nth, {}) == BC_COMMITTED);
  CHECK(ids_at_nth == std::vector<std::int64_t>{3, 2, 4, 5, 1, 6});
  // One view's charge, whatever the number of calls.
  REQUIRE(held_at_nth.size() == 6);
  CHECK(std::ranges::all_of(held_at_nth, [&](auto h) { return h == held_at_nth.front(); }));

  CHECK(run(f, nth_after_next_nth, {bytes("k"), true}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == 1);
  CHECK(run(f, nth_after_next_nth, {bytes("k"), false}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

TEST_CASE("plan API: a top-n sorter keeps the first n in sort order, the earliest of equal keys",
          "[sql][algebra][structures]") {
  Fixture f;
  insert_qtys(f, {3, 1, std::nullopt, 1, 2, 3});
  const std::vector<std::int64_t> all{3, 2, 4, 5, 1, 6};
  for (std::int64_t keep = 1; keep <= 8; ++keep) {
    INFO("keep " << keep);
    auto held = std::min<std::int64_t>(keep, 6);
    const std::vector<std::int64_t> first(all.begin(), all.begin() + held);
    REQUIRE(run_rows(f, ids_by_qty, {keep, false}, {PhysType::I64}) == BC_COMMITTED);
    CHECK(ids(f) == first);
    CHECK(bc_ctx_affected(f.cx) == held);  // the count is what it holds
    REQUIRE(run_rows(f, ids_by_qty, {keep, true}, {PhysType::I64}) == BC_COMMITTED);
    std::vector<std::int64_t> rev{3, 4, 2, 5, 6, 1};
    CHECK(ids(f) == std::vector<std::int64_t>(rev.begin(), rev.begin() + held));
    REQUIRE(run_rows(f, nth_by_qty, {keep, held - 1}, {PhysType::I64}) == BC_COMMITTED);
    CHECK(ids(f) == std::vector<std::int64_t>{first.back()});
  }
  CHECK(run_rows(f, ids_by_qty, {std::int64_t{-1}, false}, {PhysType::I64}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
// A hash join of t to itself on qty, the build side keyed by qty's key with
// the row as payload: per outer row, its id and each matching inner id, in
// the order the build side put them. Then a probe of a key nobody put.
void self_hash_join(bc_ctx* cx) {
  bc_hash h = bc_hash_open(cx);
  bc_cursor b = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, b)) {
    bc_view row = bc_scan_row(cx, b);
    bc_hash_put(cx, h, bc_col_key(cx, row, kQtyCol), row);
  }
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view outer = bc_scan_row(cx, c);
    bc_hash_iter it = bc_hash_find(cx, h, bc_col_key(cx, outer, kQtyCol));
    bc_view inner = bc_empty(cx);
    while (bc_hash_next(cx, it, &inner)) {
      bc_emit_begin(cx);
      bc_emit_col(cx, outer, kIdCol);
      bc_emit_col(cx, inner, kIdCol);  // the payload is still a row of t
      bc_emit_end(cx);
    }
  }
  int64_t none = 0;
  bc_hash_iter miss = bc_hash_find(cx, h, bc_param_bytes(cx, 0));
  while (bc_hash_next(cx, miss, nullptr)) none = bc_add(cx, none, 1);
  bc_emit_count(cx, none);
}

// A probe's payload read after its loop moved on.
void stale_probe(bc_ctx* cx) {
  bc_hash h = bc_hash_open(cx);
  bc_hash_put(cx, h, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  bc_hash_put(cx, h, bc_param_bytes(cx, 0), bc_param_bytes(cx, 0));
  bc_hash_iter it = bc_hash_find(cx, h, bc_param_bytes(cx, 0));
  bc_view first = bc_empty(cx);
  bc_view p = bc_empty(cx);
  bool started = false;
  while (bc_hash_next(cx, it, &p)) {
    if (started) bc_emit_count(cx, bc_bytes_eq(cx, first, p) ? 1 : 0);
    first = p;
    started = true;
  }
}
}  // namespace

TEST_CASE("plan API: a hash table returns every payload of a key, in the order put", "[sql][algebra][structures]") {
  Fixture f;
  insert_qtys(f, {3, 1, std::nullopt, 1, 2, 3});
  CHECK(run_rows(f, self_hash_join, {bytes("absent")}, {PhysType::I64, PhysType::I64}) == BC_COMMITTED);
  using R = std::vector<bc_value>;
  auto r = [](std::int64_t a, std::int64_t b) { return R{a, b}; };
  CHECK(f.results(std::vector{PhysType::I64, PhysType::I64}) ==
        std::vector<R>{r(1, 1), r(1, 6), r(2, 2), r(2, 4), r(3, 3), r(4, 2), r(4, 4), r(5, 5), r(6, 1), r(6, 6)});
  CHECK(bc_ctx_affected(f.cx) == 0);  // a key never put finds nothing
  CHECK(run(f, stale_probe, {bytes("k")}) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_HANDLE);
}

namespace {
// SELECT COUNT(*), COUNT(qty), SUM(qty), MIN(qty), MAX(qty), AVG(qty) FROM t
// GROUP BY name, as a plan computes PostgreSQL's NULLs: a slot of a group
// no value reached is NULL, which bc_agg_null tells. With $1 not
// NULL, ungrouped over the rows named $1: its one group is made before the
// scan, so it exists over no rows.
void aggregates(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 6);
  bool grouped = bc_param_null(cx, 0);
  bc_group all = grouped ? bc_group{} : bc_agg_group(cx, a, bc_empty(cx));
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    if (!grouped && !bc_bytes_eq(cx, bc_col_bytes(cx, row, kNameCol), bc_param_bytes(cx, 0))) continue;
    bc_group g = grouped ? bc_agg_group(cx, a, bc_col_key(cx, row, kNameCol)) : all;
    bc_agg_count(cx, g, 0);
    if (bc_col_null(cx, row, kQtyCol)) continue;
    int64_t q = bc_col_i64(cx, row, kQtyCol);
    bc_agg_count(cx, g, 1);
    bc_agg_add_i64(cx, g, 2, q);
    bc_agg_min_i64(cx, g, 3, q);
    bc_agg_max_i64(cx, g, 4, q);
    bc_agg_add_f64(cx, g, 5, bc_i64_to_f64(q));
  }
  bc_group g = all;
  while (bc_agg_next(cx, a, nullptr, &g)) {
    bc_emit_begin(cx);
    bc_emit_i64(cx, bc_agg_i64(cx, g, 0));
    bc_emit_i64(cx, bc_agg_i64(cx, g, 1));
    // No value folded into the sum is none into any: one slot tells.
    if (bc_agg_null(cx, g, 2)) {
      bc_emit_null(cx);
      bc_emit_null(cx);
      bc_emit_null(cx);
      bc_emit_null(cx);
    } else {
      bc_emit_i64(cx, bc_agg_i64(cx, g, 2));
      bc_emit_i64(cx, bc_agg_i64(cx, g, 3));
      bc_emit_i64(cx, bc_agg_i64(cx, g, 4));
      bc_emit_f64(cx, bc_agg_f64(cx, g, 5) / bc_i64_to_f64(bc_agg_i64(cx, g, 1)));
    }
    bc_emit_end(cx);
  }
}

// bc_agg_null of a group's six slots, before and after a fold into the
// first five (an integer sum, a double sum, a minimum, a maximum, a count):
// the sixth is never folded.
const std::vector<PhysType> kNulls(6, PhysType::Bool);
void null_slots(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 6);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  auto emit = [](bc_ctx* c, bc_group h) {
    bc_emit_begin(c);
    for (uint32_t slot = 0; slot < 6; ++slot) bc_emit_bool(c, bc_agg_null(c, h, slot));
    bc_emit_end(c);
  };
  emit(cx, g);
  bc_agg_add_i64(cx, g, 0, 0);
  bc_agg_add_f64(cx, g, 1, 0);
  bc_agg_min_i64(cx, g, 2, 0);
  bc_agg_max_i64(cx, g, 3, 0);
  bc_agg_count(cx, g, 4);
  emit(cx, g);
}

// $1 + $2 in an integer slot, $3 + $4 in a double one.
void sum_two(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 2);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_add_i64(cx, g, 0, bc_param_i64(cx, 0));
  bc_agg_add_i64(cx, g, 0, bc_param_i64(cx, 1));
  bc_agg_add_f64(cx, g, 1, bc_param_f64(cx, 2));
  bc_agg_add_f64(cx, g, 1, bc_param_f64(cx, 3));
  bc_emit_begin(cx);
  bc_emit_i64(cx, bc_agg_i64(cx, g, 0));
  bc_emit_f64(cx, bc_agg_f64(cx, g, 1));
  bc_emit_end(cx);
}

// Wrong plans: a slot folded two ways, read as the other type, past the
// aggregate's slots, and a group of another aggregate.
void slot_two_ways(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_add_i64(cx, g, 0, 1);
  bc_agg_min_i64(cx, g, 0, 1);
}
void slot_read_as_double(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_count(cx, g, 0);
  bc_emit_count(cx, bc_f64_to_i64(cx, bc_agg_f64(cx, g, 0)));
}
void slot_past_end(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  bc_agg_count(cx, g, 1);
}
void null_past_end(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  (void)bc_agg_null(cx, g, 1);
}
void foreign_group(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 1);
  bc_agg b = bc_agg_open(cx, 1);
  bc_group g = bc_agg_group(cx, a, bc_empty(cx));
  (void)bc_agg_group(cx, b, bc_empty(cx));
  bc_group other = {1, g.g};  // a's second group, which a does not have
  bc_agg_count(cx, other, 0);
}
}  // namespace

TEST_CASE("plan API: an aggregate's slots fold SUM, MIN, MAX and COUNT, and an integer sum that overflows is BC_E_ARITH",
          "[sql][algebra][structures]") {
  Fixture f;
  auto row = [&](std::int64_t id, const char* name, std::optional<std::int64_t> qty) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes(name), qty ? bc_value{*qty} : bc_value{}}) == BC_COMMITTED);
  };
  row(1, "a", 5);
  row(2, "b", std::nullopt);
  row(3, "a", 7);
  row(4, "b", std::nullopt);
  row(5, "c", -2);
  const std::vector<PhysType> types{PhysType::I64, PhysType::I64, PhysType::I64, PhysType::I64, PhysType::I64,
                                    PhysType::F64};
  using R = std::vector<bc_value>;
  CHECK(run_rows(f, aggregates, {bc_value{}}, types) == BC_COMMITTED);
  CHECK(f.results(types) == std::vector<R>{R{std::int64_t{2}, std::int64_t{2}, std::int64_t{12}, std::int64_t{5},
                                             std::int64_t{7}, 6.0},
                                           R{std::int64_t{2}, std::int64_t{0}, bc_value{}, bc_value{}, bc_value{},
                                             bc_value{}},
                                           R{std::int64_t{1}, std::int64_t{1}, std::int64_t{-2}, std::int64_t{-2},
                                             std::int64_t{-2}, -2.0}});
  // Ungrouped over no rows: one row, the counts 0 and the rest NULL.
  CHECK(run_rows(f, aggregates, {bytes("zzz")}, types) == BC_COMMITTED);
  CHECK(f.results(types) ==
        std::vector<R>{R{std::int64_t{0}, std::int64_t{0}, bc_value{}, bc_value{}, bc_value{}, bc_value{}}});
  CHECK(run_rows(f, aggregates, {bytes("a")}, types) == BC_COMMITTED);
  CHECK(f.results(types) == std::vector<R>{R{std::int64_t{2}, std::int64_t{2}, std::int64_t{12}, std::int64_t{5},
                                             std::int64_t{7}, 6.0}});

  const std::vector<PhysType> two{PhysType::I64, PhysType::F64};
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  CHECK(run_rows(f, sum_two, {kMax, std::int64_t{-1}, 1.5, 2.0}, two) == BC_COMMITTED);
  CHECK(f.results(two) == std::vector<R>{R{kMax - 1, 3.5}});
  CHECK(run_rows(f, sum_two, {kMax, std::int64_t{1}, 1.5, 2.0}, two) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_ARITH);
  CHECK(std::string{bc_error_message(f.cx)} == "bigint out of range");
  // A double sum that overflows is an error, as PostgreSQL's; an infinite
  // input carries through.
  CHECK(run_rows(f, sum_two, {std::int64_t{0}, std::int64_t{0}, 1e308, 1e308}, two) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_ARITH);
  CHECK(run_rows(f, sum_two, {std::int64_t{0}, std::int64_t{0}, HUGE_VAL, 1.0}, two) == BC_COMMITTED);
  CHECK(f.results(two) == std::vector<R>{R{std::int64_t{0}, HUGE_VAL}});

  // bc_agg_null of each kind of slot: true until a value is folded in, of
  // either type, and true for a slot never folded.
  CHECK(run_rows(f, null_slots, {}, kNulls) == BC_COMMITTED);
  CHECK(f.results(kNulls) == std::vector<R>{R{true, true, true, true, true, true},
                                            R{false, false, false, false, false, true}});

  for (auto plan : {slot_two_ways, slot_read_as_double, slot_past_end, null_past_end, foreign_group}) {
    CHECK(run(f, plan, {}) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_HANDLE);
  }
}

namespace {
// What one add, put or group charged, and what each next one did.
std::vector<std::uint64_t> charges;  // NOLINT: the plans are captureless

void charge_of(bc_ctx* cx, std::uint64_t before) { charges.push_back(bc_ctx_held_for_testing(cx) - before); }

// A sorter keeping $1: three adds of a 1-byte key and a 2-byte payload, the
// third key sorting first, then one whose key ties the first.
void sorter_charges(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, bc_param_i64(cx, 0));
  bc_view k1 = bc_param_bytes(cx, 1), k0 = bc_param_bytes(cx, 2), p = bc_param_bytes(cx, 3),
          q = bc_param_bytes(cx, 4);
  auto before = bc_ctx_held_for_testing(cx);
  bc_sorter_add(cx, s, k1, p);
  charge_of(cx, before);
  before = bc_ctx_held_for_testing(cx);
  bc_sorter_add(cx, s, k1, p);
  charge_of(cx, before);
  before = bc_ctx_held_for_testing(cx);
  bc_sorter_add(cx, s, k0, q);
  charge_of(cx, before);
}

// Two puts under one 1-byte key, then a probe.
void hash_charges(bc_ctx* cx) {
  bc_hash h = bc_hash_open(cx);
  bc_view k = bc_param_bytes(cx, 0), p = bc_param_bytes(cx, 1);
  auto before = bc_ctx_held_for_testing(cx);
  bc_hash_put(cx, h, k, p);
  charge_of(cx, before);
  before = bc_ctx_held_for_testing(cx);
  bc_hash_put(cx, h, k, p);
  charge_of(cx, before);
  before = bc_ctx_held_for_testing(cx);
  (void)bc_hash_find(cx, h, k);
  charge_of(cx, before);
}

// A sorter, a hash table and an aggregate of 3 slots opened.
void open_charges(bc_ctx* cx) {
  auto before = bc_ctx_held_for_testing(cx);
  (void)bc_sorter_open(cx, 0);
  charge_of(cx, before);
  before = bc_ctx_held_for_testing(cx);
  (void)bc_hash_open(cx);
  charge_of(cx, before);
  before = bc_ctx_held_for_testing(cx);
  (void)bc_agg_open(cx, 3);
  charge_of(cx, before);
}

// Opens a sorter, a hash table or an aggregate of 3 slots ($1: 0, 1, 2),
// and emits a count.
void open_one(bc_ctx* cx) {
  int64_t which = bc_param_i64(cx, 0);
  if (which == 0) (void)bc_sorter_open(cx, 0);
  if (which == 1) (void)bc_hash_open(cx);
  if (which == 2) (void)bc_agg_open(cx, 3);
  bc_emit_count(cx, 1);
}

// A group of an aggregate of 3 slots, keyed by $1.
void slot_charges(bc_ctx* cx) {
  bc_agg a = bc_agg_open(cx, 3);
  bc_view k = bc_param_bytes(cx, 0);
  auto before = bc_ctx_held_for_testing(cx);
  (void)bc_agg_group(cx, a, k);
  charge_of(cx, before);
}

// INSERT INTO t VALUES ($1, 'x', 0), then every row of t into a sorter, a
// hash table or an aggregate's groups ($2: 0, 1, 2), each row its own key.
void insert_then_hold(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_row_set_bytes(cx, r, kNameCol, bc_param_bytes(cx, 2));
  bc_row_set_i64(cx, r, kQtyCol, 0);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
  int64_t which = bc_param_i64(cx, 1);
  bc_sorter s = bc_sorter_open(cx, 0);
  bc_hash h = bc_hash_open(cx);
  bc_agg a = bc_agg_open(cx, 2);
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_view key = bc_scan_key(cx, c);
    if (which == 0) bc_sorter_add(cx, s, key, row);
    if (which == 1) bc_hash_put(cx, h, key, row);
    if (which == 2) bc_agg_count(cx, bc_agg_group(cx, a, key), 0);
  }
  bc_emit_count(cx, 1);
}
}  // namespace

TEST_CASE("plan API: a sorter, hash table or aggregate entry is charged its bytes and a fixed amount",
          "[sql][algebra][structures][memory]") {
  Fixture f;
  // Keep all: each add its key, payload and kSortEntryBytes.
  charges.clear();
  CHECK(run(f, sorter_charges, {std::int64_t{0}, bytes("b"), bytes("a"), bytes("pp"), bytes("qqqq")}) ==
        BC_COMMITTED);
  CHECK(charges == std::vector<std::uint64_t>{3 + kSortEntryBytes, 3 + kSortEntryBytes, 5 + kSortEntryBytes});
  // Keep 2: the third add replaces the last of the two, charged the
  // difference in bytes; an add that ties the last is refused, charged nothing.
  charges.clear();
  CHECK(run(f, sorter_charges, {std::int64_t{2}, bytes("b"), bytes("a"), bytes("pp"), bytes("qqqq")}) ==
        BC_COMMITTED);
  CHECK(charges == std::vector<std::uint64_t>{3 + kSortEntryBytes, 3 + kSortEntryBytes, 2});
  charges.clear();
  CHECK(run(f, sorter_charges, {std::int64_t{1}, bytes("b"), bytes("a"), bytes("pp"), bytes("qqqq")}) ==
        BC_COMMITTED);
  CHECK(charges == std::vector<std::uint64_t>{3 + kSortEntryBytes, 0, 2});
  // A hash table charges a key once, each payload and kHashEntryBytes per
  // put, and a probe its handle.
  charges.clear();
  CHECK(run(f, hash_charges, {bytes("k"), bytes("pp")}) == BC_COMMITTED);
  CHECK(charges == std::vector<std::uint64_t>{1 + 2 + kHashEntryBytes, 2 + kHashEntryBytes, kHashIterEntryBytes});
  // A group, its key, kGroupEntryBytes and its slots.
  charges.clear();
  CHECK(run(f, slot_charges, {bytes("kk")}) == BC_COMMITTED);
  CHECK(charges == std::vector<std::uint64_t>{2 + kGroupEntryBytes + 3 * kAggSlotBytes});
  // Opening one: a fixed amount, and an aggregate a byte per slot.
  charges.clear();
  CHECK(run(f, open_charges, {}) == BC_COMMITTED);
  CHECK(charges == std::vector<std::uint64_t>{kSorterBytes, kHashBytes, kAggBytes + 3});
}

TEST_CASE("plan API: opening a sorter, hash table or aggregate past the memory budget is BC_E_LIMIT",
          "[sql][algebra][structures][memory]") {
  Fixture f;
  const std::uint64_t opens[] = {kSorterBytes, kHashBytes, kAggBytes + 3};
  for (std::int64_t which = 0; which <= 2; ++which) {
    INFO("structure " << which);
    auto bytes_held = opens[static_cast<std::size_t>(which)];
    // A budget of what the open needs, less one byte, then exactly that.
    CHECK(run(f, open_one, {which}, memory_budget(bytes_held - 1)) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_LIMIT);  // SQLSTATE 54000
    CHECK(std::string{bc_error_message(f.cx)}.find("memory budget") != std::string::npos);
    CHECK(run(f, open_one, {which}, memory_budget(bytes_held)) == BC_COMMITTED);
  }
}

TEST_CASE("plan API: a sorter, hash table or aggregate past the memory budget is BC_E_LIMIT, and leaves nothing behind",
          "[sql][algebra][structures][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 200;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes(std::string(100, 'n')), id}) == BC_COMMITTED);
  }
  auto key = [&](std::int64_t id) {
    Bytes k;
    bytecask::sql::key_put_prefix(k, bytecask::sql::kNsRow, kTbl);
    bytecask::sql::key_put_i64(k, id, f.cat().key_layout(kTbl)->parts[0]);
    return k;
  };
  for (std::int64_t which = 0; which <= 2; ++which) {
    INFO("structure " << which);
    // 200 rows of over 100 bytes pass 16 KiB in any of them.
    CHECK(run(f, insert_then_hold, {std::int64_t{1000}, which, bytes("x")}, memory_budget(16u << 10)) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_LIMIT);
    CHECK(std::string{bc_error_message(f.cx)}.find("memory budget") != std::string::npos);
    Bytes out;
    CHECK_FALSE(f.db->get({}, key(1000), out));  // the insert before it is not committed
    bc_ctx_reset(f.cx);
    CHECK(bc_ctx_held_for_testing(f.cx) == 0);  // and the reset frees what it held
  }
  // Under a budget they fit, the same statement commits.
  CHECK(run(f, insert_then_hold, {std::int64_t{1000}, std::int64_t{0}, bytes("x")}, memory_budget(4u << 20)) ==
        BC_COMMITTED);
  Bytes out;
  CHECK(f.db->get({}, key(1000), out));
}

namespace {
// Every row of t through a sorter keeping $1, then streamed: each step of the
// stream builds a key, reads the payload's group key and pins nothing.
void stream_sorted(bc_ctx* cx) {
  bc_sorter s = bc_sorter_open(cx, bc_param_i64(cx, 0));
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx),
                             bc_param_bool(cx, 1) ? BC_SCAN_REVERSE : BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    bc_view row = bc_scan_row(cx, c);
    bc_sorter_add(cx, s, bc_col_key(cx, row, kQtyCol), row);
    held_at_step.push_back(bc_ctx_held_for_testing(cx));
  }
  int64_t n = 0;
  bc_view p = bc_empty(cx);
  while (bc_sorter_next(cx, s, &p)) {
    held_at_step.push_back(bc_ctx_held_for_testing(cx));
    bc_key k = bc_key_begin(cx, kTbl);
    bc_key_i64(cx, k, bc_col_i64(cx, p, kIdCol));
    if (bc_bytes_eq(cx, bc_key_end(cx, k), bc_row_key(cx, kTbl, p)) && !bc_col_null(cx, p, kQtyCol)) {
      n = bc_add(cx, n, 1);
    }
  }
  bc_emit_count(cx, n);
}

// Every row of t into a hash table keyed by id, then probed by every row:
// each outer step makes a key, a probe and a payload view.
void stream_probes(bc_ctx* cx) {
  bc_hash h = bc_hash_open(cx);
  bc_cursor b = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, b)) bc_hash_put(cx, h, bc_scan_key(cx, b), bc_scan_row(cx, b));
  int64_t n = 0;
  bc_cursor c = bc_scan_open(cx, kTbl, bc_empty(cx), bc_empty(cx), BC_SCAN_FORWARD);
  while (bc_scan_next(cx, c)) {
    held_at_step.push_back(bc_ctx_held_for_testing(cx));
    bc_hash_iter it = bc_hash_find(cx, h, bc_row_key(cx, kTbl, bc_scan_row(cx, c)));
    bc_view p = bc_empty(cx);
    while (bc_hash_next(cx, it, &p)) {
      if (bc_bytes_eq(cx, bc_col_key(cx, p, kIdCol), bc_col_key(cx, bc_scan_row(cx, c), kIdCol))) n = bc_add(cx, n, 1);
    }
  }
  bc_emit_count(cx, n);
}
}  // namespace

TEST_CASE("plan API: a structure's loops hold the same bytes at every step, and a top-n sorter holds n entries",
          "[sql][algebra][structures][memory]") {
  Fixture f;
  constexpr std::int64_t kRows = 2000;
  for (std::int64_t id = 1; id <= kRows; ++id) {
    REQUIRE(run(f, insert_plan_ok, {id, bytes("name"), id}) == BC_COMMITTED);  // rows of one size
  }
  // The stream: each step's key, views and copies went at the next step.
  held_at_step.clear();
  CHECK(run(f, stream_sorted, {std::int64_t{0}, false}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
  REQUIRE(held_at_step.size() == 2 * kRows);
  CHECK(std::all_of(held_at_step.begin() + kRows, held_at_step.end(),
                    [&](auto h) { return h == held_at_step[kRows]; }));
  // A top-n sorter holds n entries whatever it is given: in order, every
  // later row is refused; in reverse, every later row replaces one.
  for (bool reverse : {false, true}) {
    INFO("reverse " << reverse);
    held_at_step.clear();
    CHECK(run(f, stream_sorted, {std::int64_t{5}, reverse}) == BC_COMMITTED);
    CHECK(bc_ctx_affected(f.cx) == 5);
    REQUIRE(held_at_step.size() == kRows + 5);
    CHECK(std::all_of(held_at_step.begin() + 4, held_at_step.begin() + kRows,
                      [&](auto h) { return h == held_at_step[4]; }));
    // So it runs where keeping every row cannot.
    const auto budget = held_at_step[4] + 4096;
    CHECK(run(f, stream_sorted, {std::int64_t{5}, reverse}, memory_budget(budget)) == BC_COMMITTED);
    CHECK(run(f, stream_sorted, {std::int64_t{0}, reverse}, memory_budget(budget)) == BC_FAILED);
    CHECK(bc_error(f.cx) == BC_E_LIMIT);
  }
  // The probes: each outer step's probe, keys and payload views went with it.
  held_at_step.clear();
  CHECK(run(f, stream_probes, {}) == BC_COMMITTED);
  CHECK(bc_ctx_affected(f.cx) == kRows);
  REQUIRE(held_at_step.size() == kRows);
  CHECK(std::ranges::all_of(held_at_step, [&](auto h) { return h == held_at_step.front(); }));
}
