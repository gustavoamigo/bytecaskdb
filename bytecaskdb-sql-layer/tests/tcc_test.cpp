// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The generated plans, compiled in process by TinyCC, give the same results
// as the clang-linked hand-written ones; prints each plan's compile latency.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.codegen;
import bytecask.sql.parser;
import bytecask.sql.tcc_loader;
import bytecask.sql.tests.dml_scenario;

using namespace bytecask::sql;

namespace {

auto compile(Catalog& cat, std::string_view sql) -> std::unique_ptr<TccPlan> {
  auto plan = generate_plan(bound::bind(ast::parse(sql), cat), cat, sql);
  return TccPlan::compile(plan.source);
}

}  // namespace

TEST_CASE("tcc: generated plans compile in process and match the clang-linked results", "[sql][tcc]") {
  sql_test::Fixture f;
  const std::pair<const char*, const char*> stmts[] = {
      {"insert", "INSERT INTO t VALUES ($1, $2, $3)"},
      {"select_by_id", "SELECT id, qty FROM t WHERE id = $1"},
      {"select_name_range", "SELECT id FROM t WHERE name >= $1 AND name < $2"},
      {"update_qty", "UPDATE t SET qty = qty - $2 WHERE id = $1"},
      {"delete_by_id", "DELETE FROM t WHERE id = $1"},
  };
  std::vector<std::unique_ptr<TccPlan>> plans;
  for (const auto& [name, sql] : stmts) {
    // The median of several compiles: the first pays for page faults.
    std::vector<double> us;
    for (int i = 0; i < 9; ++i) us.push_back(compile(f.cat, sql)->compile_us());
    std::ranges::sort(us);
    std::printf("tcc compile %-18s median %7.1f us  min %7.1f us  max %7.1f us\n", name, us[4], us.front(), us.back());
    plans.push_back(compile(f.cat, sql));
  }
  sql_test::run_dml_scenario(f, {plans[0]->fn(), plans[1]->fn(), plans[2]->fn(), plans[3]->fn(), plans[4]->fn()});
}

TEST_CASE("tcc: a plan reaching outside the plan API does not link", "[sql][tcc]") {
  // No libc: the plan sees only the functions in plan_api_symbols().
  CHECK_THROWS_AS(TccPlan::compile("#include \"bc_plan.h\"\nint puts(const char *);\n"
                                   "void plan(bc_ctx *cx) { puts(\"hi\"); }\n"),
                  std::runtime_error);
  CHECK_THROWS_AS(TccPlan::compile("void plan(bc_ctx *cx) { syntax error }"), std::runtime_error);
}
