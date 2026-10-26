// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The code generator reproduces the hand-written plans in plans/, which
// tests/plans_test.cpp runs, modulo whitespace.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bytecask.sql.codegen;
import bytecask.sql.parser;
import bytecask.sql.tests.fixture;

using namespace bytecask::sql;

namespace {

auto read_plan(const std::string& name) -> std::string {
  std::ifstream in{std::string{SQL_LAYER_PLANS_DIR} + "/" + name + ".c"};
  REQUIRE(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

auto squeeze(std::string_view s) -> std::string {
  std::string out;
  for (auto ch : s) {
    if (std::isspace(static_cast<unsigned char>(ch)) == 0) out.push_back(ch);
  }
  return out;
}

auto generate(const Catalog& cat, std::string_view sql) -> GeneratedPlan {
  return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql);
}

}  // namespace

TEST_CASE("codegen: generated plans equal the hand-written ones", "[sql][codegen]") {
  sql_test::Fixture f;
  const std::vector<std::pair<std::string, std::string>> cases{
      {"select_by_id", "SELECT id, qty FROM t WHERE id = $1"},
      {"select_name_range", "SELECT id FROM t WHERE name >= $1 AND name < $2"},
      {"insert", "INSERT INTO t VALUES ($1, $2, $3)"},
      {"update_qty", "UPDATE t SET qty = qty - $2 WHERE id = $1"},
      {"delete_by_id", "DELETE FROM t WHERE id = $1"},
  };
  for (const auto& [file, sql] : cases) {
    INFO(file);
    auto plan = generate(f.cat(), sql);
    INFO(plan.source);
    CHECK(squeeze(plan.source) == squeeze(read_plan(file)));
  }
}

TEST_CASE("codegen: what the runtime needs besides the text", "[sql][codegen]") {
  sql_test::Fixture f;
  auto sel = generate(f.cat(), "SELECT id, qty FROM t WHERE id = $1");
  CHECK(sel.returns_rows);
  CHECK(sel.result_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
  CHECK(sel.objects_read == std::vector<std::uint32_t>{1});
  CHECK(sel.tables_written.empty());
  CHECK(sel.catalog_version == 2);
  REQUIRE(sel.columns.size() == 2);  // id and qty, by table and id: no position
  CHECK(sel.columns[0].table == 1);
  CHECK(sel.columns[0].col == 0);
  CHECK(sel.columns[1].col == 2);
  CHECK(sel.columns[1].type == PhysType::I64);
  // A covering range reads the index alone.
  auto range = generate(f.cat(), "SELECT id FROM t WHERE name >= $1");
  CHECK(range.objects_read == std::vector<std::uint32_t>{3});
  CHECK(range.columns.empty());
  CHECK(range.source.find("bc_scan_row") == std::string::npos);
  // A range that needs a column the index does not hold fetches the row.
  auto fetch = generate(f.cat(), "SELECT id, qty FROM t WHERE name >= $1");
  CHECK(fetch.objects_read == std::vector<std::uint32_t>{1, 3});
  CHECK(fetch.source.find("bc_view row = bc_scan_row(cx, c);") != std::string::npos);
  auto upd = generate(f.cat(), "UPDATE t SET qty = qty - $2 WHERE id = $1");
  CHECK_FALSE(upd.returns_rows);
  CHECK(upd.tables_written == std::vector<std::uint32_t>{1});  // its indexes are the runtime's
  CHECK(upd.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
}

TEST_CASE("codegen: the other bound forms produce plans; no rule, no plan", "[sql][codegen]") {
  sql_test::Fixture f;
  // Exclusive and inclusive bounds become successors of the encoded part.
  auto gt = generate(f.cat(), "SELECT qty, name FROM t WHERE name > $1").source;
  CHECK(gt.find("bc_view lo = bc_key_successor(cx, bc_key_params(cx, IDX_3, 0, 1));") != std::string::npos);
  CHECK(gt.find("bc_view hi = bc_empty(cx);") != std::string::npos);
  auto le = generate(f.cat(), "SELECT id FROM t WHERE name <= $1").source;
  CHECK(le.find("bc_view hi = bc_key_successor(cx, bc_key_params(cx, IDX_3, 0, 1));") != std::string::npos);
  // An upper bound alone on a nullable column starts past the null entries.
  auto lt = generate(f.cat(), "SELECT id FROM t WHERE name < $1").source;
  CHECK(lt.find("bc_key_null(cx, k0);\n  bc_view lo = bc_key_successor(cx, bc_key_end(cx, k0));") != std::string::npos);
  auto eq = generate(f.cat(), "SELECT id FROM t WHERE name = $1").source;
  CHECK(eq.find("BC_SCAN_PREFIX") != std::string::npos);
  // A column not given keeps the default bc_row_begin wrote; the plan names
  // no index and no key: the runtime derives them.
  auto partial = generate(f.cat(), "INSERT INTO t (id) VALUES ($1)").source;
  CHECK(partial.find("COL_1_1") == std::string::npos);
  CHECK(partial.find("bc_row_set_param(cx, r, COL_1_0, 0);\n  bc_insert(cx, TBL_1, bc_row_end(cx, r));") !=
        std::string::npos);
  CHECK(partial.find("IDX_") == std::string::npos);
  // A SET from a parameter copies it, NULL included.
  auto set = generate(f.cat(), "UPDATE t SET name = $2 WHERE id = $1").source;
  CHECK(set.find("bc_row_set_param(cx, r, COL_1_1, 1);\n  bc_update(cx, TBL_1, old, bc_row_end(cx, r));") !=
        std::string::npos);

  // Nothing bounds the table: a full scan of its primary key, the conjunct a
  // filter on each row.
  auto full = generate(f.cat(), "SELECT id FROM t WHERE qty = $1").source;
  CHECK(full.find("bc_view lo = bc_empty(cx);\n  bc_view hi = bc_empty(cx);\n  bc_cursor c = bc_scan_open(cx, TBL_1, lo, "
                  "hi, BC_SCAN_FORWARD);") != std::string::npos);
  CHECK(full.find("bc_col_null(cx, row, COL_1_2) || bc_param_null(cx, 0)") != std::string::npos);
  // A conjunct the key does not express filters each row: NULL on either side
  // rejects it.
  auto point = generate(f.cat(), "SELECT id FROM t WHERE id = $1 AND qty = $2").source;
  CHECK(point.find("if (bc_col_null(cx, row, COL_1_2) || bc_param_null(cx, 1) || !(bc_col_i64(cx, row, COL_1_2) == "
                   "bc_param_i64(cx, 1))) {\n    return;") != std::string::npos);
  auto range = generate(f.cat(), "SELECT id FROM t WHERE name > $1 AND name > $2").source;
  CHECK(range.find("|| !(bc_bytes_cmp(cx, bc_col_bytes(cx, row, COL_1_1), bc_param_bytes(cx, 1)) > 0)) continue;") !=
        std::string::npos);
  auto del = generate(f.cat(), "DELETE FROM t WHERE qty = $1").source;
  CHECK(del.find("bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);") != std::string::npos);
  CHECK(del.find("bc_delete(cx, TBL_1, old);") != std::string::npos);
}

TEST_CASE("codegen: UPDATE and DELETE write each row their key reaches, and count them", "[sql][codegen]") {
  sql_test::Fixture f;
  // By the index: each row read whole, written, counted; a NULL key writes none.
  auto up = generate(f.cat(), "UPDATE t SET qty = $2 WHERE name = $1");
  CHECK(up.objects_read == std::vector<std::uint32_t>{1, 3});
  CHECK(up.tables_written == std::vector<std::uint32_t>{1});
  CHECK(up.source.find("bc_emit_count(cx, 0);\n    return;") != std::string::npos);
  CHECK(up.source.find("while (bc_scan_next(cx, c)) {\n    bc_view old = bc_scan_row(cx, c);") != std::string::npos);
  CHECK(up.source.find("bc_update(cx, TBL_1, old, bc_row_end(cx, r));\n    ++n;\n  }\n  bc_emit_count(cx, n);") !=
        std::string::npos);
  auto del = generate(f.cat(), "DELETE FROM t WHERE id >= $1 AND id < $2");
  CHECK(del.source.find("bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD)") != std::string::npos);
  CHECK(del.source.find("bc_delete(cx, TBL_1, old);\n    ++n;") != std::string::npos);
  // One row by the whole primary key, as before.
  CHECK(generate(f.cat(), "DELETE FROM t WHERE id = $1").source.find("bc_get(") != std::string::npos);
  // Nothing bounds it: a full scan, each row filtered, then written.
  auto full = generate(f.cat(), "UPDATE t SET qty = $1 WHERE qty = $2").source;
  CHECK(full.find("bc_cursor c = bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);\n  while (bc_scan_next(cx, c)) {\n    "
                  "bc_view old = bc_scan_row(cx, c);\n    if (bc_col_null(cx, old, COL_1_2)") != std::string::npos);
  auto filtered = generate(f.cat(), "DELETE FROM t WHERE name = $1 AND qty = $2").source;
  CHECK(filtered.find("bc_view old = bc_scan_row(cx, c);\n    if (bc_col_null(cx, old, COL_1_2)") != std::string::npos);
}

TEST_CASE("codegen: MIN from the first entry past the NULLs, or a loop; SUM by a loop", "[sql][codegen]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"m",
                       {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("g", PhysType::I64, false),
                        sql_test::Fixture::col("x", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"m_gx", "m", {{"g"}, {"x"}}});
  // x is the ASC part after the prefix: the scan starts past its NULLs, and
  // its first entry is the least value, read into a local.
  auto first = generate(f.cat(), "SELECT min(x) FROM m WHERE g = $1").source;
  CHECK(first.find("bc_key_null(cx, k0);\n    bc_view lo = bc_key_successor(cx, bc_key_end(cx, k0));") !=
        std::string::npos);
  CHECK(first.find("if (bc_scan_next(cx, c)) {") != std::string::npos);
  CHECK(first.find("kv0 = bc_key_part_i64(cx, IDX_6, bc_scan_key(cx, c), PART_6_2);") != std::string::npos);
  CHECK(first.find("while") == std::string::npos);
  // k is not after the prefix: every entry is read, as one group's MIN.
  auto loop = generate(f.cat(), "SELECT min(k) FROM m WHERE g = $1").source;
  CHECK(loop.find("if (!xs0_0 || v0_0 < x0_0) x0_0 = v0_0;") != std::string::npos);
  auto sum = generate(f.cat(), "SELECT sum(x) FROM m WHERE g = $1");
  CHECK(sum.result_names == std::vector<std::string>{"sum"});
  // A BIGINT's SUM is numeric with no declared precision: the total may take
  // 19 digits.
  REQUIRE(sum.result_sql.size() == 1);
  CHECK(sum.result_sql[0].kind == SqlKind::Decimal);
  CHECK(sum.result_sql[0].precision == 0);
  CHECK(sql_type_name(sum.result_sql[0]) == "numeric");
  CHECK(sum.source.find("sum = bc_add(cx, sum, bc_key_part_i64(cx, IDX_6, key, PART_6_2));") != std::string::npos);
  CHECK(generate(f.cat(), "SELECT min(x) FROM m WHERE g = $1").result_names == std::vector<std::string>{"min"});
}

TEST_CASE("codegen: a constant is emitted from its parameter; a SELECT without FROM reads nothing", "[sql][codegen]") {
  sql_test::Fixture f;
  // The constant is $2, after the statement's own $1, read in every row.
  auto beside = generate(f.cat(), "SELECT id, 7 FROM t WHERE id = $1").source;
  CHECK(beside.find("bc_emit_col(cx, row, COL_1_0);") != std::string::npos);
  CHECK(beside.find("if (bc_param_null(cx, 1)) {") != std::string::npos);
  CHECK(beside.find("bc_emit_i64(cx, bc_param_i64(cx, 1));") != std::string::npos);
  // No table: one row, emitted once; no constants block, no read.
  auto bare = generate(f.cat(), "SELECT 'a', 2.5");
  CHECK(bare.result_names == std::vector<std::string>{"?column?", "?column?"});
  CHECK(bare.source.find("bc_emit_bytes(cx, bc_param_bytes(cx, 0));") != std::string::npos);
  CHECK(bare.source.find("bc_emit_i64(cx, bc_param_i64(cx, 1));") != std::string::npos);
  CHECK(bare.source.find("enum") == std::string::npos);
  CHECK(bare.source.find("bc_scan") == std::string::npos);
  CHECK(bare.source.find("while") == std::string::npos);
  CHECK(bare.objects_read.empty());
}

TEST_CASE("codegen: a predicate filter is true or false in SQL's three-valued logic, each node printed once",
          "[sql][codegen]") {
  sql_test::Fixture f;
  auto ne = generate(f.cat(), "SELECT id FROM t WHERE qty <> $1").source;
  INFO(ne);
  // NOT (qty = $1) is true where the comparison is false: neither NULL,
  // and not equal.
  CHECK(ne.find("if (!(!(bc_col_null(cx, row, COL_1_2) || bc_param_null(cx, 0)) && !(bc_col_i64(cx, row, COL_1_2) == "
                "bc_param_i64(cx, 0)))) continue;") != std::string::npos);
  auto either = generate(f.cat(), "SELECT id FROM t WHERE qty IS NULL OR name = $1").source;
  INFO(either);
  CHECK(either.find("(bc_col_null(cx, row, COL_1_2)) || (!(bc_col_null(cx, row, COL_1_1) || bc_param_null(cx, 0)) && "
                    "(bc_bytes_cmp(cx, bc_col_bytes(cx, row, COL_1_1), bc_param_bytes(cx, 0)) == 0))") != std::string::npos);
  // A BETWEEN of the key at the top is its range; a predicate never is.
  auto between = generate(f.cat(), "SELECT id FROM t WHERE id BETWEEN $1 AND $2").source;
  CHECK(between.find("bc_scan_open(cx, TBL_1") != std::string::npos);
  CHECK(between.find("continue;") == std::string::npos);
  // IN is an OR: no key takes it, so the rows are scanned and filtered.
  auto in = generate(f.cat(), "SELECT id FROM t WHERE id IN ($1, $2)").source;
  CHECK(in.find("bc_get(") == std::string::npos);
  CHECK(in.find("bc_param_i64(cx, 0))) || (") != std::string::npos);
  CHECK(in.find("continue;") != std::string::npos);
}

TEST_CASE("codegen: over no rows, only a COUNT call is 0; anything else is NULL", "[sql][codegen][group]") {
  // Expr::fn defaults to Count: a column output is NULL whatever its fn.
  // Hand-built, since check refuses a column of one group of every row.
  const logical::ColumnRef v{1, 2, PhysType::I64, true};
  auto count = logical::expr(logical::Expr::Kind::Aggregate, SqlType{SqlKind::BigInt});
  auto column = logical::column(v, SqlType{SqlKind::BigInt});
  REQUIRE(column.fn == logical::Expr::Fn::Count);
  physical::Scan rows{1, 3, {}, physical::Range{}, false, false, false};
  auto g = physical::node(physical::GroupAggregate{physical::node(std::move(rows)), {}, {count}, true, {}});
  auto root = physical::node(physical::GroupEmit{std::move(g), {column, count}});
  CHECK(no_rows_lines(*root) ==
        std::vector<std::string>{"bc_emit_begin(cx);", "bc_emit_null(cx);", "bc_emit_i64(cx, 0);", "bc_emit_end(cx);"});
}

TEST_CASE("codegen: a DOUBLE result column is read from the row, never the index key", "[sql][codegen]") {
  // The key encoding writes -0.0 as 0.0 and every NaN as one NaN: a covering
  // read would return a value the row does not hold.
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"dd", {sql_test::Fixture::col("id", PhysType::I64, false), sql_test::Fixture::col("d", PhysType::F64, true)},
                       {"id"}});
  f.ddl(CreateIndexDdl{"dd_d", "dd", {{"d"}}});
  auto by_d = generate(f.cat(), "SELECT d FROM dd WHERE d >= $1");
  CHECK(by_d.source.find("bc_view row = bc_scan_row(cx, c);") != std::string::npos);
  CHECK(by_d.source.find("bc_emit_key_part") == std::string::npos);
  auto ids = generate(f.cat(), "SELECT id FROM dd WHERE d >= $1");  // the id is exact in the key
  CHECK(ids.source.find("bc_emit_key_part") != std::string::npos);
}

TEST_CASE("codegen: a composite key from parameters out of order takes the builder", "[sql][codegen]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"p", {sql_test::Fixture::col("a", PhysType::I64, false), sql_test::Fixture::col("b", PhysType::I64, false),
                             sql_test::Fixture::col("v", PhysType::I64, true)},
                       {"a", "b"}});
  auto in_order = generate(f.cat(), "SELECT v FROM p WHERE a = $1 AND b = $2").source;
  CHECK(in_order.find("bc_get(cx, bc_key_params(cx, TBL_4, 0, 2), &found);") != std::string::npos);
  auto swapped = generate(f.cat(), "SELECT v FROM p WHERE b = $1 AND a = $2").source;
  CHECK(swapped.find("bc_key k0 = bc_key_begin(cx, TBL_4);\n  bc_key_param(cx, k0, 1);\n  bc_key_param(cx, k0, 0);") !=
        std::string::npos);
  CHECK(swapped.find("bc_get(cx, bc_key_end(cx, k0), &found);") != std::string::npos);
}

TEST_CASE("codegen: what the planner refuses, with its message, and what it sorts", "[sql][codegen]") {
  sql_test::Fixture f;
  // o: an index whose second part is a nullable DESC part.
  f.ddl(CreateTableDdl{"o",
                       {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("a", PhysType::I64, false),
                        sql_test::Fixture::col("b", PhysType::I64, true), sql_test::Fixture::col("c", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"o_ab", "o", {{"a"}, {"b", true}, {"c"}}});
  auto refusal = [&](std::string_view sql) -> std::string {
    try {
      (void)generate(f.cat(), sql);
    } catch (const ast::FeatureNotSupported& e) {
      return e.what();
    }
    return "accepted";
  };
  auto sorts = [&](std::string_view sql) { return generate(f.cat(), sql).source.find("bc_sorter_open") != std::string::npos; };
  // No index gives the order: a sort.
  CHECK(sorts("SELECT id FROM t WHERE name = $1 ORDER BY qty"));
  // b's NULLs: o_ab's key puts a DESC part's last, PostgreSQL's DESC first;
  // NULLS LAST is the key's order.
  CHECK(sorts("SELECT k FROM o WHERE a = $1 ORDER BY b DESC"));
  CHECK_FALSE(sorts("SELECT k FROM o WHERE a = $1 ORDER BY b DESC NULLS LAST"));
  // b compared: c is the nullable part, ASC in the key, NULLS FIRST in it;
  // PostgreSQL's default ASC puts them last, which two ranges give.
  CHECK_FALSE(sorts("SELECT k FROM o WHERE a = $1 AND b = $2 ORDER BY c, k"));
  CHECK_FALSE(sorts("SELECT k FROM o WHERE a = $1 AND b = $2 ORDER BY c NULLS FIRST, k"));
  // c's NULLs fall within each value of b: the key's order only where the
  // ORDER BY asks for it.
  CHECK(sorts("SELECT k FROM o WHERE a = $1 ORDER BY b DESC, c"));
  CHECK_FALSE(sorts("SELECT k FROM o WHERE a = $1 ORDER BY b DESC NULLS LAST, c NULLS FIRST"));
  // A DOUBLE or REAL column takes no filter yet.
  f.ddl(CreateTableDdl{"fd", {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("d", PhysType::F64, true)},
                       {"k"}});
  CHECK(refusal("SELECT k FROM fd WHERE k = $1 AND d < $2") ==
        "no plan: a filter on column \"d\", a REAL or DOUBLE PRECISION, is not supported yet");
  // $n + k bounds a range on a key, but compared by = it is a filter: it names no key.
  f.ddl(CreateTableDdl{"ik", {{"k", {SqlKind::Integer}, false, {}}}, {"k"}});
  CHECK(refusal("SELECT k FROM ik WHERE k >= $1 + 1") == "accepted");
  CHECK(generate(f.cat(), "SELECT k FROM ik WHERE k = $1 + 1").source.find("bc_empty(cx);\n  bc_cursor c = bc_scan_open(cx, TBL_") !=
        std::string::npos);
}

TEST_CASE("codegen: FOR UPDATE guards the key of every row it returns, read from the row", "[sql][codegen][txn]") {
  sql_test::Fixture f;
  auto by_pk = generate(f.cat(), "SELECT id, qty FROM t WHERE id = $1 FOR UPDATE");
  CHECK(by_pk.source.find("bc_guard_row(cx, TBL_1, row);") != std::string::npos);
  CHECK(generate(f.cat(), "SELECT id, qty FROM t WHERE id = $1").source.find("bc_guard") == std::string::npos);
  // A range the index would cover reads each row instead: its key is guarded.
  auto range = generate(f.cat(), "SELECT id FROM t WHERE name >= $1 FOR UPDATE");
  CHECK(range.objects_read == std::vector<std::uint32_t>{1, 3});
  CHECK(range.source.find("bc_view row = bc_scan_row(cx, c);") != std::string::npos);
  CHECK(range.source.find("bc_guard_row(cx, TBL_1, row);") != std::string::npos);
  CHECK(range.tables_written.empty());
}

TEST_CASE("codegen: a join is of two point lookups, in FROM order", "[sql][codegen]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  auto j = generate(f.cat(), "SELECT v, qty FROM u JOIN t ON id = $2 WHERE k = $1");
  CHECK(j.objects_read == std::vector<std::uint32_t>{1, 4});
  CHECK(j.source.find("bc_view row = bc_get(cx, bc_key_params(cx, TBL_4, 0, 1), &found);") != std::string::npos);
  CHECK(j.source.find("bc_view row1 = bc_get(cx, bc_key_params(cx, TBL_1, 1, 1), &found1);") != std::string::npos);
  // An aggregate over the pair reads the second row only if the first is there.
  auto agg = generate(f.cat(), "SELECT count(*) FROM u JOIN t ON id = $2 WHERE k = $1");
  CHECK(agg.source.find("if (found) row1 = bc_get(cx, bc_key_params(cx, TBL_1, 1, 1), &found1);") != std::string::npos);
  auto refusal = [&](std::string_view sql) -> std::string {
    try {
      (void)generate(f.cat(), sql);
    } catch (const ast::FeatureNotSupported& e) {
      return e.what();
    }
    return "accepted";
  };
  // Two point lookups give at most one row: any ORDER BY.
  CHECK(refusal("SELECT v FROM u, t WHERE k = $1 AND id = $2 ORDER BY v") == "accepted");
  // u by its key from a parameter, read once; then t by its index, scanned
  // only if u's row is there (golden/cross.sql).
  auto cross = generate(f.cat(), "SELECT v FROM u, t WHERE k = $1 AND name = $2");
  auto lookup = cross.source.find("bc_view row = bc_get(cx, bc_key_params(cx, TBL_4, 0, 1), &found);");
  auto scan = cross.source.find("bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);");
  REQUIRE(lookup != std::string::npos);
  REQUIRE(scan != std::string::npos);
  CHECK(lookup < scan);
  // Nothing reads t's columns: its scan covers, and reads no row.
  CHECK(cross.source.find("bc_scan_row") == std::string::npos);
  auto read = generate(f.cat(), "SELECT v, qty FROM u, t WHERE k = $1 AND name = $2");
  CHECK(read.source.find("bc_view row1 = bc_scan_row(cx, c);") != std::string::npos);
  // A join on a column: the outer table reached from its parameters, the
  // inner one read by its whole primary key from the outer row.
  auto loop = generate(f.cat(), "SELECT v FROM t, u WHERE name >= $1 AND k = qty");
  CHECK(loop.objects_read == std::vector<std::uint32_t>{1, 3, 4});
  CHECK(loop.source.find("bc_key_i64(cx, k0, bc_col_i64(cx, row, COL_1_2));") != std::string::npos);
  CHECK(loop.source.find("if (!found1) continue;") != std::string::npos);
  // FROM order first, then the other: u is the one its parameters reach.
  CHECK(refusal("SELECT v FROM t, u WHERE k = $1 AND id = v") == "accepted");
  // t by a full scan, u looked up per row; t by its key, u scanned once.
  CHECK(refusal("SELECT v FROM t, u WHERE qty = $1 AND k = qty") == "accepted");
  auto once = generate(f.cat(), "SELECT v FROM t, u WHERE id = $1 AND v = qty").source;
  CHECK(once.find("bc_view row1 = bc_scan_row(cx, c);") != std::string::npos);
  // An ORDER BY on a join is the first table's scan's order: a lookup per
  // row keeps it. A column of the other table is in no scan's order.
  auto ordered = generate(f.cat(), "SELECT v FROM t, u WHERE name >= $1 AND k = qty ORDER BY name DESC");
  CHECK(ordered.source.find("bc_scan_open(cx, IDX_3, lo, hi, BC_SCAN_REVERSE);") != std::string::npos);
  // A column of the other table is in no scan's order: the pairs are
  // sorted, the sorter holding t's row, and u looked up again from it.
  auto by_v = generate(f.cat(), "SELECT v FROM t, u WHERE name >= $1 AND k = qty ORDER BY v").source;
  auto add = by_v.find("bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);");
  auto next = by_v.find("while (bc_sorter_next(cx, s0, &p0)) {");
  auto again = by_v.find("bc_key_i64(cx, k1, bc_col_i64(cx, p0, COL_1_2));");
  REQUIRE(add != std::string::npos);
  REQUIRE(next != std::string::npos);
  REQUIRE(again != std::string::npos);
  CHECK(add < next);
  CHECK(next < again);
  // name compared for equality: the index's next part, id, gives the order.
  auto by_id = generate(f.cat(), "SELECT v FROM t, u WHERE name = $1 AND k = qty ORDER BY id");
  CHECK(by_id.source.find("bc_scan_open(cx, IDX_3, lo, lo, BC_SCAN_PREFIX);") != std::string::npos);
  // FROM order would leave x.f = y.f a filter on a DOUBLE PRECISION; the
  // other order keys x by it.
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"x", {Fixture::col("f", PhysType::F64, false), Fixture::col("c", PhysType::I64, true)}, {"f"}});
  f.ddl(CreateIndexDdl{"x_by_c", "x", {{"c"}}});
  f.ddl(CreateTableDdl{"y", {Fixture::col("yk", PhysType::I64, false), Fixture::col("yf", PhysType::F64, true)}, {"yk"}});
  auto other = generate(f.cat(), "SELECT c FROM x, y WHERE c = $1 AND yk = $2 AND f = yf");
  CHECK(other.source.find("bc_scan_open(cx, TBL_9, lo, lo, BC_SCAN_PREFIX);") != std::string::npos);
  CHECK(other.source.find("bc_key_f64(cx, k0, bc_col_f64(cx, row, COL_9_1));") != std::string::npos);
  // Both orders refused: the first one's refusal stands.
  CHECK(refusal("SELECT c FROM x, y WHERE c = $1 AND yk = $2 AND f = yf AND yf = $3") ==
        "no plan: a filter on column \"yf\", a REAL or DOUBLE PRECISION, is not supported yet");
  // A long outer value makes an inner key no row has: that outer row joins nothing.
  f.ddl(CreateTableDdl{"s", {Fixture::col("sk", PhysType::Bytes, false)}, {"sk"}});
  auto by_bytes = generate(f.cat(), "SELECT id FROM t, s WHERE name >= $1 AND sk = name");
  auto fits = by_bytes.source.find("if (!bc_key_fits(cx, k0)) continue;");
  auto get = by_bytes.source.find("bc_view row1 = bc_get(cx, bc_key_end(cx, k0), &found1);");
  REQUIRE(fits != std::string::npos);
  REQUIRE(get != std::string::npos);
  CHECK(fits < get);
  CHECK(loop.source.find("bc_key_fits") == std::string::npos);  // an integer key always fits
}

TEST_CASE("codegen: a subquery is a function of its own, run first into its parameters", "[sql][codegen]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  auto has = [](const GeneratedPlan& p, std::string_view s) { return p.source.find(s) != std::string::npos; };
  // A scalar subquery keys t's lookup: its value is a parameter, set first.
  auto scalar = generate(f.cat(), "SELECT qty FROM t WHERE id = (SELECT MAX(k) FROM u)");
  CHECK(has(scalar, "static void sub0(bc_ctx *cx) {"));
  CHECK(has(scalar, "bc_sub_begin(cx, 0, BC_SUB_VALUE, BC_T_I64, 0);\n  sub0(cx);\n  bc_sub_end(cx);"));
  CHECK(has(scalar, "bc_key_params(cx, TBL_1, 0, 1)"));
  CHECK(scalar.objects_read == std::vector<std::uint32_t>{1, 4});  // the subquery's table among them
  CHECK(scalar.source.find("sub0(cx);") < scalar.source.find("bc_param_null(cx, 0)"));  // its NULL ends the plan after
  // IN tests the subquery's set, NULL where its flags say.
  auto in = generate(f.cat(), "SELECT id FROM t WHERE qty IN (SELECT v FROM u)");
  CHECK(has(in, "bc_sub_begin(cx, 0, BC_SUB_SET, BC_T_I64, 0);"));
  CHECK(has(in, "bc_sub_has_i64(cx, 0, "));
  // EXISTS reads at most one row, whatever its columns.
  auto exists = generate(f.cat(), "SELECT id FROM t WHERE EXISTS (SELECT k, v FROM u)");
  CHECK(has(exists, "bc_sub_begin(cx, 0, BC_SUB_EXISTS, BC_T_BOOL, 0);"));
  CHECK(has(exists, "bc_param_bool(cx, 0)"));
  // ALL and ANY of other than = read its bounds.
  auto all = generate(f.cat(), "SELECT id FROM t WHERE qty < ALL (SELECT v FROM u)");
  CHECK(has(all, "bc_sub_begin(cx, 0, BC_SUB_BOUNDS, BC_T_I64, 0);"));
}
