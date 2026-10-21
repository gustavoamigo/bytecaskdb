// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The generated plans, compiled in process by TinyCC, give the same results
// as the clang-linked hand-written ones; prints each plan's compile latency.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

import bytecask.sql.codegen;
import bytecask.sql.parser;
import bytecask.sql.runtime;
import bytecask.sql.tcc_loader;
import bytecask.sql.tests.dml_scenario;

using namespace bytecask::sql;

namespace {

auto compile(const Catalog& cat, std::string_view sql) -> std::unique_ptr<TccPlan> {
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
    for (int i = 0; i < 9; ++i) us.push_back(compile(f.cat(), sql)->compile_us());
    std::ranges::sort(us);
    std::printf("tcc compile %-18s median %7.1f us  min %7.1f us  max %7.1f us\n", name, us[4], us.front(), us.back());
    plans.push_back(compile(f.cat(), sql));
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

TEST_CASE("tcc: a unique insert raced by another commit conflicts, and its retry reports the duplicate",
          "[sql][tcc][catalog]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"c", {sql_test::Fixture::col("id", PhysType::I64, false),
                             sql_test::Fixture::col("email", PhysType::Bytes, true)}, {"id"}});
  f.ddl(CreateIndexDdl{"c_email", "c", {{"email"}}, true});
  constexpr std::string_view kSql = "INSERT INTO c VALUES ($1, $2)";
  auto gen = generate_plan(bound::bind(ast::parse(kSql), f.cat()), f.cat(), kSql);
  auto code = TccPlan::compile(gen.source);
  sql_test::SchemaRef sr{gen.objects_read, gen.tables_written, {}, {}, gen.catalog_version};
  for (const auto& c : gen.columns) sr.cols.push_back({c.table, c.col, static_cast<std::uint8_t>(c.type)});
  auto ref = sr.get();

  // A runs on a snapshot without "a"; B inserts "a" under another key and
  // commits first; A's range guard on the prefix refuses its commit.
  auto pa = sql_test::params({std::int64_t{1}, sql_test::bytes("a")});
  auto pb = sql_test::params({std::int64_t{2}, sql_test::bytes("a")});
  auto* other = bc_ctx_new(&f.bdb, &f.bcat, 1u << 20);
  REQUIRE(other != nullptr);
  bc_ctx_begin(f.cx, &pa, {});
  bc_ctx_run(f.cx, code->fn(), &ref);
  REQUIRE(bc_error(f.cx) == BC_OK);
  CHECK(bc_execute(other, code->fn(), &ref, &pb, {}, false, 0) == BC_COMMITTED);
  CHECK(bc_ctx_commit(f.cx, false) == BC_CONFLICT);
  bc_ctx_reset(f.cx);
  // On a fresh snapshot the check sees the duplicate and raises: final, not retried.
  CHECK(bc_execute(f.cx, code->fn(), &ref, &pa, {}, false, 3) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
  CHECK(bc_error_detail(f.cx) == ((std::uint32_t{BC_C_UNIQUE} << 24) | f.cat().index("c_email").second->id));
  bc_ctx_free(other);
}

TEST_CASE("tcc: an insert whose last retry conflicts on a key a racing commit took reports the duplicate",
          "[sql][tcc][catalog]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"c", {sql_test::Fixture::col("id", PhysType::I64, false),
                             sql_test::Fixture::col("email", PhysType::Bytes, true)}, {"id"}});
  f.ddl(CreateIndexDdl{"c_email", "c", {{"email"}}, true});
  constexpr std::string_view kSql = "INSERT INTO c VALUES ($1, $2)";
  auto gen = generate_plan(bound::bind(ast::parse(kSql), f.cat()), f.cat(), kSql);
  auto code = TccPlan::compile(gen.source);
  sql_test::SchemaRef sr{gen.objects_read, gen.tables_written, {}, {}, gen.catalog_version};
  for (const auto& c : gen.columns) sr.cols.push_back({c.table, c.col, static_cast<std::uint8_t>(c.type)});
  auto ref = sr.get();
  // No retry left: B commits "a" under another key between A's run and its
  // commit, so A's one attempt conflicts, and the conflict is told as the
  // duplicate its retry would have found.
  auto pa = sql_test::params({std::int64_t{1}, sql_test::bytes("a")});
  auto pb = sql_test::params({std::int64_t{2}, sql_test::bytes("a")});
  auto* other = bc_ctx_new(&f.bdb, &f.bcat, 1u << 20);
  REQUIRE(other != nullptr);
  bc_ctx_before_commit_for_testing(f.cx, [&] { CHECK(bc_execute(other, code->fn(), &ref, &pb, {}, false, 0) == BC_COMMITTED); });
  CHECK(bc_execute(f.cx, code->fn(), &ref, &pa, {}, false, 0) == BC_FAILED);
  CHECK(bc_error(f.cx) == BC_E_CONSTRAINT);
  CHECK(bc_error_detail(f.cx) == ((std::uint32_t{BC_C_UNIQUE} << 24) | f.cat().index("c_email").second->id));
  bc_ctx_before_commit_for_testing(f.cx, {});
  bc_ctx_free(other);
}
