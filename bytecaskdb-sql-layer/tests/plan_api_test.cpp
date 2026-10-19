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
#include "bytecaskdb-sql-layer/algebra/bc_plan_testing.h"
#include "include/bytecask.hpp"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::Bytes;
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
  CHECK(bytecask::sql::row_i64(row, t2.layout.find(t2.column("tag")->id)->pos) == 7);

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

TEST_CASE("plan API: a context caches a verified reference by id and catalog version", "[sql][algebra][catalog]") {
  Fixture f;
  auto none = params({});
  SchemaRef reads_index{{kIdx}, {}, {}, {}, 0};
  auto ref = reads_index.get();
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_COMMITTED);
  // Whether a run was served from the cache: it commits and verifies nothing.
  auto served_from_cache = [&](const bc_schema_ref& r) {
    auto before = bc_ctx_verifications(f.cx);
    auto got = bc_execute(f.cx, scan_index, &r, &none, {}, false, 0);
    return got == BC_COMMITTED && bc_ctx_verifications(f.cx) == before;
  };
  CHECK(served_from_cache(ref));

  // The same reference, its object list changed in place to name an object
  // that does not exist: same id, same arrays at the same addresses, other
  // contents. Verified on its own and refused, never served the entry.
  reads_index.read[0] = 4242;
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  reads_index.read[0] = kIdx;
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_COMMITTED);
  CHECK(served_from_cache(ref));

  // A different reference that reuses the id is verified on its own, not
  // served the other's entry: its arrays differ.
  SchemaRef lying{{4242}, {}, {}, {}, 0};
  auto liar = lying.get();
  liar.id = ref.id;
  CHECK(bc_execute(f.cx, scan_index, &liar, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);  // a recompile can cure it
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_COMMITTED);  // verified again
  // An id of 0 is never cached: checked on every run.
  auto uncached = ref;
  uncached.id = 0;
  CHECK(bc_execute(f.cx, scan_index, &uncached, &none, {}, false, 0) == BC_COMMITTED);
  CHECK_FALSE(served_from_cache(uncached));

  // An id the runtime never issued is no id: verified on every run.
  auto forged = ref;
  forged.id = std::numeric_limits<std::uint64_t>::max();
  CHECK(bc_execute(f.cx, scan_index, &forged, &none, {}, false, 0) == BC_COMMITTED);
  CHECK_FALSE(served_from_cache(forged));

  // A flood of 2,000 other references, twice the cache, with the first one
  // run every 20: CLOCK keeps it.
  for (int i = 0; i < 2000; ++i) {
    auto r = reads_index.get();
    REQUIRE(bc_execute(f.cx, scan_index, &r, &none, {}, false, 0) == BC_COMMITTED);
    if (i % 20 == 0) REQUIRE(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_COMMITTED);
  }
  CHECK(served_from_cache(ref));

  // A cache full of dead references, as 1,024 recompiles leave it, then two
  // live ones in turn: each keeps its entry, so neither verifies again.
  for (int i = 0; i < 1024; ++i) {
    auto r = reads_index.get();
    REQUIRE(bc_execute(f.cx, scan_index, &r, &none, {}, false, 0) == BC_COMMITTED);
  }
  SchemaRef other_index{{kIdx}, {}, {}, {}, 0};
  auto a = reads_index.get();
  auto b = other_index.get();
  for (int i = 0; i < 100; ++i) {
    REQUIRE(bc_execute(f.cx, scan_index, &a, &none, {}, false, 0) == BC_COMMITTED);
    REQUIRE(bc_execute(f.cx, scan_index, &b, &none, {}, false, 0) == BC_COMMITTED);
  }
  CHECK(served_from_cache(a));

  // A DDL between two executions moves the version, so the cached entry no
  // longer matches and the reference is checked again: a stale plan is
  // refused, even with an id this context verified.
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_COMMITTED);
  f.ddl(bytecask::sql::DropIndexDdl{"t_by_name"});
  CHECK(bc_execute(f.cx, scan_index, &ref, &none, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_LAYOUT);
  CHECK(bc_error_detail(f.cx) == BC_L_INCOMPATIBLE);
  CHECK(std::string{bc_error_message(f.cx)}.find("no longer exists") != std::string::npos);
}

namespace {
void insert_one(bc_ctx* cx) {
  bc_row r = bc_row_begin(cx, kTbl);
  bc_row_set_param(cx, r, kIdCol, 0);
  bc_insert(cx, kTbl, bc_row_end(cx, r));
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
