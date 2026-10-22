// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Golden plans (docs/frontend_design.md, *Tests*): each tests/golden/<name>.sql
// is compiled against the fixture's schema plus tests/golden/schema.sql, each
// tests/golden/tprocc/<name>.sql against the fixture's plus TPROC-C's, and
// its plan text must equal tests/golden/<name>.c byte for byte and compile
// with TinyCC. A change to the code generator regenerates them with
// SQL_LAYER_REGEN_GOLDEN=1, and the PR shows the diff.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import bytecask.sql.codegen;
import bytecask.sql.parser;
import bytecask.sql.tcc_loader;
import bytecask.sql.tests.fixture;
import bytecask.sql.tests.tprocc;

using namespace bytecask::sql;

namespace {

auto read_file(const std::filesystem::path& p) -> std::string {
  std::ifstream in{p, std::ios::binary};
  REQUIRE(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// One statement per file, without its trailing newline.
auto statement(const std::filesystem::path& p) -> std::string {
  auto s = read_file(p);
  while (!s.empty() && (s.back() == '\n' || s.back() == ';')) s.pop_back();
  return s;
}

auto bind_sql(const Catalog& cat, std::string_view sql) -> bound::Bound { return bound::bind(ast::parse(sql), cat); }

// Every <name>.sql in `dir` against the fixture plus `schema`, one DDL
// statement each.
void check_dir(const std::filesystem::path& dir, const std::vector<std::string>& schema) {
  sql_test::Fixture f;
  for (const auto& ddl : schema) f.ddl(std::get<Ddl>(bind_sql(f.cat(), ddl).stmt));

  std::vector<std::filesystem::path> cases;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == ".sql" && e.path().stem() != "schema") cases.push_back(e.path());
    // A golden plan with no statement checks nothing: delete it.
    if (e.path().extension() == ".c") {
      INFO(e.path().filename().string());
      CHECK(std::filesystem::exists(std::filesystem::path{e.path()}.replace_extension(".sql")));
    }
  }
  std::ranges::sort(cases);
  REQUIRE_FALSE(cases.empty());

  const auto* regen = std::getenv("SQL_LAYER_REGEN_GOLDEN");
  for (const auto& sql_file : cases) {
    INFO(sql_file.stem().string());
    auto sql = statement(sql_file);
    auto source = generate_plan(bind_sql(f.cat(), sql), f.cat(), sql).source;
    CHECK_NOTHROW(TccPlan::compile(source));
    auto golden = std::filesystem::path{sql_file}.replace_extension(".c");
    if (regen != nullptr && std::string_view{regen} == "1") {
      std::ofstream{golden, std::ios::binary} << source;
      WARN("regenerated, not compared: " << golden.filename().string());
      continue;
    }
    REQUIRE(std::filesystem::exists(golden));
    CHECK(source == read_file(golden));
  }
}

}  // namespace

TEST_CASE("golden: every plan the compiler generates equals its golden file", "[sql][codegen][golden]") {
  const std::filesystem::path dir{SQL_LAYER_GOLDEN_DIR};
  std::vector<std::string> schema;
  std::istringstream text{read_file(dir / "schema.sql")};
  for (std::string line; std::getline(text, line);) {
    while (!line.empty() && line.back() == ';') line.pop_back();
    if (!line.empty()) schema.push_back(line);
  }
  check_dir(dir, schema);
  // TPROC-C's statements, against its schema (tprocc_schema.cppm).
  check_dir(dir / "tprocc", {sql_test::kTproccSchema.begin(), sql_test::kTproccSchema.end()});
}
