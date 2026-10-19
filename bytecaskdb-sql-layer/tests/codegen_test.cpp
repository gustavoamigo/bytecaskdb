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

auto generate(Catalog& cat, std::string_view sql) -> GeneratedPlan {
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
    auto plan = generate(f.cat, sql);
    INFO(plan.source);
    CHECK(squeeze(plan.source) == squeeze(read_plan(file)));
  }
}

TEST_CASE("codegen: what the runtime needs besides the text", "[sql][codegen]") {
  sql_test::Fixture f;
  auto sel = generate(f.cat, "SELECT id, qty FROM t WHERE id = $1");
  CHECK(sel.returns_rows);
  CHECK(sel.result_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
  CHECK(sel.tables_read == std::vector<std::uint32_t>{1});
  CHECK(sel.tables_written.empty());
  CHECK(sel.catalog_version == 2);
  auto upd = generate(f.cat, "UPDATE t SET qty = qty - $2 WHERE id = $1");
  CHECK_FALSE(upd.returns_rows);
  CHECK(upd.tables_written == std::vector<std::uint32_t>{1});
  CHECK(upd.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
}

TEST_CASE("codegen: the other bound forms produce plans; no rule, no plan", "[sql][codegen]") {
  sql_test::Fixture f;
  // Exclusive and inclusive bounds become successors of the encoded part.
  auto gt = generate(f.cat, "SELECT qty, name FROM t WHERE name > $1").source;
  CHECK(gt.find("bc_view lo = bc_key_successor(cx, bc_key_end(cx, k0));") != std::string::npos);
  CHECK(gt.find("bc_view hi = bc_key_successor(cx, bc_key_end(cx, k1));") != std::string::npos);
  // An upper bound alone on a nullable column starts past the null entries.
  auto lt = generate(f.cat, "SELECT id FROM t WHERE name < $1").source;
  CHECK(lt.find("bc_key_null(cx, k0);\n  bc_view lo = bc_key_successor(cx, bc_key_end(cx, k0));") != std::string::npos);
  auto eq = generate(f.cat, "SELECT id FROM t WHERE name = $1").source;
  CHECK(eq.find("BC_SCAN_PREFIX") != std::string::npos);
  auto partial = generate(f.cat, "INSERT INTO t (id) VALUES ($1)").source;
  CHECK(partial.find("bc_row_set_null(cx, r0, T_NAME_COL);") != std::string::npos);
  CHECK(partial.find("bc_key_null(cx, k1);") != std::string::npos);
  // A duplicate key is checked on the snapshot and raised, then guarded for the race.
  CHECK(partial.find("if (bc_exists(cx, pk)) {\n    bc_raise(cx, BC_C_UNIQUE, TBL_T);\n    return;\n  }") != std::string::npos);
  CHECK(partial.find("bc_guard_absent(cx, pk);") != std::string::npos);

  CHECK_THROWS_AS(generate(f.cat, "SELECT id FROM t WHERE qty = $1"), ast::SqlError);
  CHECK_THROWS_AS(generate(f.cat, "SELECT id FROM t WHERE id = $1 AND qty = $2"), ast::SqlError);
  CHECK_THROWS_AS(generate(f.cat, "UPDATE t SET name = $1 WHERE id = $2"), ast::SqlError);
  CHECK_THROWS_AS(generate(f.cat, "DELETE FROM t WHERE name = $1"), ast::SqlError);
}
