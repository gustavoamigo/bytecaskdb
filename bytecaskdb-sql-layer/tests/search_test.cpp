// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The rewrites and the search on logical plans built by hand: shapes the
// binder does not build from today's SQL, but that the rewrites or another
// front end may hand the search.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.codegen;
import bytecask.sql.rewrite;
import bytecask.sql.search;
import bytecask.sql.tests.fixture;

using namespace bytecask::sql;
using logical::Expr;
using Op = ast::Compare::Op;

namespace {

auto get(const Table& t) -> logical::NodePtr { return logical::node(logical::Get{&t, false}); }

auto col(const Table& t, std::string_view name) -> Expr { return logical::column_of(t.id, *t.column(name)); }

auto p(std::uint32_t n) -> Expr { return logical::param(n, SqlType{SqlKind::BigInt}); }

auto filter(logical::NodePtr in, std::vector<Expr> conjuncts) -> logical::NodePtr {
  return logical::node(logical::Filter{std::move(in), std::move(conjuncts)});
}

auto project(logical::NodePtr in, std::vector<Expr> cols) -> logical::NodePtr {
  std::vector<logical::Output> outs;
  for (auto& c : cols) outs.push_back({std::move(c), "c"});
  return logical::node(logical::Project{std::move(in), std::move(outs)});
}

auto count_star(logical::NodePtr in) -> logical::NodePtr {
  auto agg = logical::expr(Expr::Kind::Aggregate, SqlType{SqlKind::BigInt});
  agg.fn = Expr::Fn::Count;
  std::vector<logical::Output> outs;
  outs.push_back({std::move(agg), "count"});
  return logical::node(logical::Aggregate{std::move(in), std::move(outs)});
}

auto decimal(std::int64_t v, std::uint8_t scale) -> Expr {
  return logical::constant(v, SqlType{SqlKind::Decimal, 0, 18, scale});
}

// The plan of a logical plan of `params` BIGINT parameters, as generate_plan
// prints it: rewritten, searched, checked.
auto generate(const Catalog& cat, logical::NodePtr root, std::size_t params) -> GeneratedPlan {
  bound::Bound b;
  b.stmt = std::move(root);
  b.param_types = std::vector<PhysType>(params, PhysType::I64);
  return generate_plan(std::move(b), cat, "hand-built");
}

auto has(const GeneratedPlan& g, std::string_view text) -> bool { return g.source.find(text) != std::string::npos; }

// The message a plan is refused with; any other exception, a
// std::bad_variant_access among them, fails the test.
auto refusal(const Catalog& cat, logical::NodePtr root, std::size_t params) -> std::string {
  try {
    (void)generate(cat, std::move(root), params);
  } catch (const ast::FeatureNotSupported& e) {
    return e.what();
  }
  return "accepted";
}

}  // namespace

TEST_CASE("search: a Filter over a Filter keeps both conjunct lists, merged by the rewrites", "[sql][search]") {
  sql_test::Fixture f;
  const auto& t = *f.cat().table("t");
  auto two = [&] {
    auto inner = filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0))});
    return project(filter(std::move(inner), {logical::compare(Op::Eq, col(t, "qty"), p(1))}), {col(t, "id")});
  };

  // The rewrites leave one Filter, the lower one's conjuncts first.
  auto root = two();
  logical::rewrite(*root);
  const auto& merged = std::get<logical::Filter>(std::get<logical::Project>(root->op).input->op);
  REQUIRE(merged.conjuncts.size() == 2);
  CHECK(merged.conjuncts[0].args[0].col == col(t, "id").col);
  CHECK(merged.conjuncts[1].args[0].col == col(t, "qty").col);
  CHECK(std::holds_alternative<logical::Get>(merged.input->op));

  // The search, given both Filters, reads both: the key from the lower one,
  // the filter from the upper.
  auto plan = search(*two(), f.cat());
  const auto& emit = std::get<physical::Emit>(plan.root->op);
  const auto& residual = std::get<physical::Filter>(emit.input->op);
  CHECK(residual.conjuncts.size() == 1);
  CHECK(std::holds_alternative<physical::PointGet>(residual.input->op));

  auto g = generate(f.cat(), two(), 2);
  CHECK(has(g, "bc_get(cx, bc_key_params(cx, TBL_1, 0, 1), &found)"));
  CHECK(has(g, "!(bc_col_i64(cx, row, COL_1_2) == bc_param_i64(cx, 1))"));
}

TEST_CASE("search: a shape it does not plan is refused with no plan, never a variant exception", "[sql][search]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  const auto& t = *f.cat().table("t");
  const auto& u = *f.cat().table("u");

  // A conjunct that reads no column stays above the Join pushdown moved the
  // others from.
  auto pair = logical::node(logical::Join{get(t), get(u), {}});
  auto over_join = project(filter(std::move(pair), {logical::compare(Op::Eq, col(t, "id"), p(0)),
                                                   logical::compare(Op::Eq, col(t, "qty"), col(u, "k")),
                                                   logical::compare(Op::Eq, p(1), p(2))}),
                           {col(u, "v")});
  CHECK(refusal(f.cat(), std::move(over_join), 3) == "no plan: a Join where the search plans a table");

  // On one table, it stays on the table's Filter.
  auto on_table = project(
      filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0)), logical::compare(Op::Eq, p(1), p(2))}),
      {col(t, "id")});
  CHECK(refusal(f.cat(), std::move(on_table), 3) == "no plan: a conjunct on t the search does not plan yet");

  // No Project or Aggregate at the top of the rows.
  auto bare = filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0))});
  CHECK(refusal(f.cat(), std::move(bare), 1) == "no plan: a Filter where the search plans a Project or an Aggregate");
}

TEST_CASE("rewrite: a conjunct always false makes the input empty, planned as no rows and no reads",
          "[sql][search]") {
  sql_test::Fixture f;
  const auto& t = *f.cat().table("t");
  auto never = [] { return logical::compare(Op::Eq, decimal(1, 0), decimal(2, 0)); };

  auto root = project(filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0)), never()}), {col(t, "id")});
  logical::rewrite(*root);
  const auto* empty = std::get_if<logical::Empty>(&std::get<logical::Project>(root->op).input->op);
  REQUIRE(empty != nullptr);
  CHECK(empty->tables == std::vector<const Table*>{&t});

  auto rows = generate(f.cat(), project(filter(get(t), {never()}), {col(t, "id")}), 0);
  CHECK(rows.objects_read.empty());
  CHECK(!has(rows, "bc_get"));
  CHECK(!has(rows, "bc_scan"));
  CHECK(!has(rows, "bc_emit"));

  auto count = generate(f.cat(), count_star(filter(get(t), {never()})), 0);
  CHECK(count.objects_read.empty());
  CHECK(has(count, "bc_emit_begin(cx);\n  bc_emit_i64(cx, 0);\n  bc_emit_end(cx);"));

  auto del = generate(f.cat(), logical::node(logical::Delete{filter(get(t), {never()}), &t}), 0);
  CHECK(del.tables_written == std::vector<std::uint32_t>{t.id});
  CHECK(has(del, "bc_emit_count(cx, 0);"));
  CHECK(!has(del, "bc_get"));
  CHECK(!has(del, "bc_delete"));

  // A join with an empty side is empty.
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false)}, {"k"}});
  const auto& t2 = *f.cat().table("t");
  const auto& u = *f.cat().table("u");
  auto pair = logical::node(logical::Join{filter(get(t2), {never()}), get(u), {}});
  auto joined = generate(f.cat(), project(std::move(pair), {col(u, "k")}), 0);
  CHECK(joined.objects_read.empty());
}

TEST_CASE("rewrite: folding compares constants by their SQL type", "[sql][search]") {
  sql_test::Fixture f;
  const auto& t = *f.cat().table("t");
  // The conjuncts left on t's Filter after the rewrites, beside id = $1.
  auto folded = [&](Expr c) -> logical::NodePtr {
    auto root = project(filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0)), std::move(c)}), {col(t, "id")});
    logical::rewrite(*root);
    return std::move(std::get<logical::Project>(root->op).input);
  };
  auto kept = [](const logical::NodePtr& n) -> std::size_t {
    const auto* fl = std::get_if<logical::Filter>(&n->op);
    return fl == nullptr ? 0 : fl->conjuncts.size();
  };

  // 1.50 = 1.5: true, so dropped. As raw int64s, 150 and 15, it would be false.
  auto same = folded(logical::compare(Op::Eq, decimal(150, 2), decimal(15, 1)));
  CHECK(kept(same) == 1);
  // 15 < 1.50 (an INTEGER and a DECIMAL(18, 2)): false, so no rows.
  auto less = folded(logical::compare(Op::Lt, logical::constant(15, SqlType{SqlKind::Integer}), decimal(150, 2)));
  CHECK(std::holds_alternative<logical::Empty>(less->op));
  // A BOOLEAN and a BIGINT have no common representation: not folded.
  auto mixed = folded(logical::compare(Op::Eq, logical::constant(1, SqlType{SqlKind::Boolean}),
                                       logical::constant(1, SqlType{SqlKind::BigInt})));
  REQUIRE(kept(mixed) == 2);
  CHECK(std::get<logical::Filter>(mixed->op).conjuncts[1].kind == Expr::Kind::Compare);
  // Two BOOLEANs compare as BOOLEANs: false < true.
  auto booleans = folded(logical::compare(Op::Lt, logical::constant(0, SqlType{SqlKind::Boolean}),
                                          logical::constant(1, SqlType{SqlKind::Boolean})));
  CHECK(kept(booleans) == 1);
  // A sum of DECIMALs of two scales is left for the plan, not added raw.
  auto sum = logical::binary(Expr::Kind::Add, decimal(1, 1), decimal(1, 2), SqlType{SqlKind::Decimal, 0, 18, 2});
  auto sums = folded(logical::compare(Op::Eq, std::move(sum), decimal(11, 2)));
  CHECK(kept(sums) == 2);
}

TEST_CASE("search: only an equality conjunct of a join keys the inner side", "[sql][search]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  const auto& t = *f.cat().table("t");
  const auto& u = *f.cat().table("u");
  auto join = [&](std::vector<Expr> on) {
    auto outer = filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0))});
    return project(logical::node(logical::Join{std::move(outer), get(u), std::move(on)}), {col(u, "v")});
  };

  // u.k < t.qty names no row of u by its key.
  CHECK(refusal(f.cat(), join({logical::compare(Op::Lt, col(u, "k"), col(t, "qty"))}), 1) ==
        "no plan: no index of u joins on k");

  // u.k = t.qty keys u; t.qty < u.v is a filter with its own operator, the
  // inner column first.
  auto g = generate(f.cat(),
                    join({logical::compare(Op::Eq, col(u, "k"), col(t, "qty")),
                          logical::compare(Op::Lt, col(t, "qty"), col(u, "v"))}),
                    1);
  CHECK(has(g, "bc_key_i64(cx, k0, bc_col_i64(cx, row, COL_1_2));"));
  CHECK(has(g, "!(bc_col_i64(cx, row1, COL_4_1) > bc_col_i64(cx, row, COL_1_2))"));
}
