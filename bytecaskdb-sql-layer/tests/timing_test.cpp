// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Where prepare's time goes, per statement: parse, bind, generate, TinyCC.
// Hidden ([.]): it measures, it does not check. Run with
// `sql_layer_tests "[timing]"`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

import bytecask.sql.codegen;
import bytecask.sql.parser;
import bytecask.sql.tcc_loader;
import bytecask.sql.tests.fixture;

using namespace bytecask::sql;

namespace {

template <typename F>
auto median_us(F&& f) -> double {
  std::vector<double> us;
  for (int i = 0; i < 25; ++i) {
    auto start = std::chrono::steady_clock::now();
    f();
    us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
  }
  std::ranges::sort(us);
  return us[us.size() / 2];
}

}  // namespace

TEST_CASE("timing: prepare, stage by stage", "[.][timing]") {
  sql_test::Fixture f;
  const char* stmts[] = {
      "INSERT INTO t VALUES ($1, $2, $3)",
      "SELECT id, qty FROM t WHERE id = $1",
      "SELECT id FROM t WHERE name >= $1 AND name < $2",
      "UPDATE t SET qty = qty - $2 WHERE id = $1",
      "DELETE FROM t WHERE id = $1",
  };
  std::printf("%-50s %9s %9s %9s %9s\n", "statement", "parse", "bind", "generate", "tcc");
  for (const auto* sql : stmts) {
    auto parse = median_us([&] { (void)ast::parse(sql); });
    auto tree = ast::parse(sql);
    auto bind = median_us([&] { (void)bound::bind(tree, f.cat()); });
    auto gen = median_us([&] { (void)generate_plan(bound::bind(tree, f.cat()), f.cat(), sql); }) - bind;
    auto src = generate_plan(bound::bind(tree, f.cat()), f.cat(), sql).source;
    auto tcc = median_us([&] { (void)TccPlan::compile(src); });
    std::printf("%-50s %7.1fus %7.1fus %7.1fus %7.1fus\n", sql, parse, bind, gen, tcc);
  }
  // TinyCC's floor: a plan that only includes the header.
  auto empty = median_us([] { (void)TccPlan::compile("#include \"bc_plan.h\"\nvoid plan(bc_ctx *cx) {}\n"); });
  std::printf("%-50s %39.1fus\n", "empty plan (bc_plan.h only)", empty);
}
