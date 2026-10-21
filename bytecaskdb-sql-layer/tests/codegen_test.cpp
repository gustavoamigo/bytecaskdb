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

  CHECK_THROWS_AS(generate(f.cat(), "SELECT id FROM t WHERE qty = $1"), ast::SqlError);
  CHECK_THROWS_AS(generate(f.cat(), "SELECT id FROM t WHERE id = $1 AND qty = $2"), ast::SqlError);
  CHECK_THROWS_AS(generate(f.cat(), "DELETE FROM t WHERE name = $1"), ast::SqlError);
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

TEST_CASE("codegen: what the planner refuses, with its message", "[sql][codegen]") {
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
    } catch (const ast::SqlError& e) {
      return e.what();
    }
    return "accepted";
  };
  CHECK(refusal("SELECT id FROM t WHERE qty = $1") == "no plan: a full scan of t");
  CHECK(refusal("SELECT id FROM t WHERE name = $1 ORDER BY qty") == "no plan: no index of t gives ORDER BY qty");
  CHECK(refusal("SELECT k FROM o WHERE a = $1 ORDER BY b DESC") == "no plan: ORDER BY b would need its NULLs reordered");
  // c is nullable, and its NULLs would fall within each value of b.
  CHECK(refusal("SELECT k FROM o WHERE a = $1 AND b = $2 ORDER BY c, k") == "accepted");
  CHECK(refusal("SELECT k FROM o WHERE a = $1 ORDER BY b DESC, c") ==
        "no plan: ORDER BY b would need its NULLs reordered");
  CHECK(refusal("SELECT id FROM t WHERE id = $1 AND qty = $2") ==
        "no plan: a filter on column \"qty\" that the key does not express is not supported yet");
  CHECK(refusal("SELECT id FROM t WHERE name > $1 AND name > $2") ==
        "no plan: a filter on column \"name\" that the key does not express is not supported yet");
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
  auto refusal = [&](std::string_view sql) -> std::string {
    try {
      (void)generate(f.cat(), sql);
    } catch (const ast::SqlError& e) {
      return e.what();
    }
    return "accepted";
  };
  CHECK(refusal("SELECT v FROM u, t WHERE k = $1 AND id = $2 ORDER BY v") == "no plan: ORDER BY on a join");
  CHECK(refusal("SELECT v FROM u, t WHERE k = $1 AND name = $2") ==
        "no plan: a join must name the row of each table by its whole primary key yet");
}

