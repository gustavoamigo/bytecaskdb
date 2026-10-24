// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The rewrites and the search on logical plans built by hand: shapes the
// binder does not build from today's SQL, but that the rewrites or another
// front end may hand the search.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.codegen;
import bytecask.sql.parser;
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

// The message a plan is refused with, rewritten first or not; any other
// exception, a std::bad_variant_access among them, fails the test.
auto refusal(const Catalog& cat, logical::NodePtr root, std::size_t params, bool rewrite = true) -> std::string {
  try {
    if (rewrite) {
      (void)generate(cat, std::move(root), params);
    } else {
      (void)search(*root, cat);
    }
  } catch (const ast::FeatureNotSupported& e) {
    return e.what();
  }
  return "accepted";
}

// The plan of a SQL statement against `cat`, its tables costed at `rows`
// each, by name; a table not named is uncounted.
auto plan_sql(const Catalog& cat, std::string_view sql, std::map<std::string, std::uint64_t> rows = {})
    -> GeneratedPlan {
  RowCounts counts = [named = std::move(rows)](const Table& t) -> std::optional<std::uint64_t> {
    auto it = named.find(t.name);
    if (it == named.end()) return std::nullopt;
    return it->second;
  };
  return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts);
}

}  // namespace

TEST_CASE("search: a Filter over a Filter is merged by the rewrites, and only then planned", "[sql][search]") {
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

  // The search plans the merged Filter: the key from one conjunct, the
  // filter from the other. Given both Filters, it refuses rather than guess.
  auto plan = search(*root, f.cat());
  const auto& emit = std::get<physical::Emit>(plan.root->op);
  const auto& residual = std::get<physical::Filter>(emit.input->op);
  CHECK(residual.conjuncts.size() == 1);
  CHECK(std::holds_alternative<physical::PointGet>(residual.input->op));
  CHECK(refusal(f.cat(), two(), 2, false) == "no plan: a Filter over a Filter where the search plans a table");

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
  CHECK(refusal(f.cat(), std::move(over_join), 3) == "no plan: a conjunct that reads no column");

  // On one table, it stays on the table's Filter.
  auto on_table = project(
      filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0)), logical::compare(Op::Eq, p(1), p(2))}),
      {col(t, "id")});
  CHECK(refusal(f.cat(), std::move(on_table), 3) == "no plan: a conjunct on t that does not compare one of its columns");

  // A column compared with another of its own table, outside a join.
  auto two_columns = project(filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0)),
                                             logical::compare(Op::Eq, col(t, "qty"), col(t, "id"))}),
                             {col(t, "id")});
  CHECK(refusal(f.cat(), std::move(two_columns), 1) == "no plan: a conjunct on t compares two columns outside a join");
  // A column of another table outside the join's predicate: it is not on
  // either side of a join, so never usable on t's Filter.
  auto stray = project(filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0)),
                                       logical::compare(Op::Eq, col(t, "qty"), col(u, "k"))}),
                       {col(t, "id")});
  CHECK(refusal(f.cat(), std::move(stray), 1, false) == "no plan: a conjunct on t compares two columns outside a join");
  // A join conjunct whose columns are both of one side.
  auto one_side = project(logical::node(logical::Join{get(t), get(u), {logical::compare(Op::Eq, col(t, "qty"), col(t, "id"))}}),
                          {col(u, "v")});
  CHECK(refusal(f.cat(), std::move(one_side), 0, false) ==
        "no plan: a join conjunct that does not compare a column of each side");
  // A result column of a table the statement does not read.
  auto foreign = project(filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0))}), {col(u, "v")});
  CHECK(refusal(f.cat(), std::move(foreign), 1) == "no plan: a result column that is not a column of its tables");

  // An operator where the search plans another, named with its article.
  auto empty_side = project(logical::node(logical::Join{logical::node(logical::Empty{{&t}}), get(u), {}}), {col(u, "v")});
  CHECK(refusal(f.cat(), std::move(empty_side), 0, false) == "no plan: an Empty where the search plans a table");
  auto agg_source = project(count_star(filter(get(t), {logical::compare(Op::Eq, col(t, "id"), p(0))})), {col(t, "id")});
  CHECK(refusal(f.cat(), std::move(agg_source), 1, false) == "no plan: an Aggregate where the search plans a table");

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
        "no plan: no equality names u's key");

  // u.k = t.qty keys u; t.qty < u.v is a filter with its own operator, the
  // inner column first.
  auto g = generate(f.cat(),
                    join({logical::compare(Op::Eq, col(u, "k"), col(t, "qty")),
                          logical::compare(Op::Lt, col(t, "qty"), col(u, "v"))}),
                    1);
  CHECK(has(g, "bc_key_i64(cx, k0, bc_col_i64(cx, row, COL_1_2));"));
  CHECK(has(g, "!(bc_col_i64(cx, row1, COL_4_1) > bc_col_i64(cx, row, COL_1_2))"));
}

TEST_CASE("rewrite: an input folded to Empty empties the Filter above it; Filters over a Join merge", "[sql][search]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false)}, {"k"}});
  const auto& t = *f.cat().table("t");
  const auto& u = *f.cat().table("u");
  auto never = logical::compare(Op::Eq, decimal(1, 0), decimal(2, 0));
  auto root = project(filter(filter(get(t), {std::move(never)}), {logical::compare(Op::Eq, col(t, "id"), p(0))}),
                      {col(t, "id")});
  logical::rewrite(*root);
  CHECK(std::holds_alternative<logical::Empty>(std::get<logical::Project>(root->op).input->op));

  // Two Filters over a Join, of conjuncts that read no column, become one.
  auto pair = logical::node(logical::Join{get(t), get(u), {logical::compare(Op::Eq, col(t, "qty"), col(u, "k"))}});
  auto over = project(filter(filter(std::move(pair), {logical::compare(Op::Eq, p(0), p(1))}),
                             {logical::compare(Op::Eq, p(1), p(2))}),
                      {col(u, "k")});
  logical::rewrite(*over);
  const auto& top = std::get<logical::Filter>(std::get<logical::Project>(over->op).input->op);
  CHECK(top.conjuncts.size() == 2);
  CHECK(std::holds_alternative<logical::Join>(top.input->op));
}

TEST_CASE("search: costs choose the join order, from the tables' counts", "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"a", {Fixture::col("k", PhysType::I64, false), Fixture::col("x", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"a_x", "a", {{"x"}}});
  f.ddl(CreateTableDdl{"b", {Fixture::col("k", PhysType::I64, false), Fixture::col("y", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"b_y", "b", {{"y"}}});
  const auto& cat = f.cat();
  const auto a_x = std::to_string(cat.table("a")->indexes[0].id);
  const auto b_y = std::to_string(cat.table("b")->indexes[0].id);
  // Either table is reached by its index, and either keys the other by k.
  const std::string sql = "SELECT a.k FROM a, b WHERE x = $1 AND y = $2 AND a.k = b.k";
  auto scans = [](const GeneratedPlan& g, const std::string& index) {
    return has(g, "bc_scan_open(cx, IDX_" + index + ", lo, lo, BC_SCAN_PREFIX)");
  };

  // a small: scan a's 100 entries for x, fetch each row, and look up b by
  // its key for each of the 100: 2 * 100 + 100.
  auto small_a = plan_sql(cat, sql, {{"a", 1000}, {"b", 1'000'000}});
  CHECK(scans(small_a, a_x));
  CHECK_FALSE(scans(small_a, b_y));
  CHECK(small_a.cost == Catch::Approx(300));
  // b small: the other order.
  auto small_b = plan_sql(cat, sql, {{"a", 1'000'000}, {"b", 1000}});
  CHECK(scans(small_b, b_y));
  CHECK_FALSE(scans(small_b, a_x));
  CHECK(small_b.cost == Catch::Approx(300));
  // Equal: FROM order.
  CHECK(scans(plan_sql(cat, sql, {{"a", 1000}, {"b", 1000}}), a_x));
}

TEST_CASE("search: a table keyed by parameters alone is read once, before the other table's scan",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  f.ddl(CreateTableDdl{"u", {sql_test::Fixture::col("k", PhysType::I64, false), sql_test::Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT v FROM u, t WHERE k = $1 AND name = $2";
  // u by its point lookup, 1; then t's 100 entries under name, each row
  // fetched: 1 + 200. Scanning t first and looking u up for each of its 100
  // rows would be 200 + 100.
  auto once = plan_sql(cat, sql, {{"t", 1000}, {"u", 1000}});
  CHECK(once.cost == Catch::Approx(201));
  auto get = once.source.find("bc_view row = bc_get(cx, bc_key_params(cx, TBL_");
  auto missing = once.source.find("if (!found) {\n    return;\n  }");
  auto scan = once.source.find("bc_scan_open(cx, IDX_");
  REQUIRE(get != std::string::npos);
  REQUIRE(missing != std::string::npos);
  REQUIRE(scan != std::string::npos);
  // No row of u: the plan ends before t is scanned.
  CHECK(get < missing);
  CHECK(missing < scan);
  CHECK_FALSE(has(once, "found1"));
  // Both orders are costed; the other is the loop with a lookup per row.
  auto b = bound::bind(ast::parse(sql), cat);
  auto all = generate_alternatives(std::move(b), cat, sql, [](const Table& t) -> std::optional<std::uint64_t> {
    return t.name == "t" || t.name == "u" ? std::optional<std::uint64_t>{1000} : std::nullopt;
  });
  REQUIRE(all.size() == 2);
  CHECK(all[0].source == once.source);
  CHECK(has(all[1], "bc_view row1 = bc_get(cx, bc_key_params(cx, TBL_"));
  CHECK(all[1].cost == Catch::Approx(300));
}

TEST_CASE("search: a refusal names the failure of a plan of every table, not of one table's access",
          "[sql][search]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"a",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("x", PhysType::I64, true),
                        Fixture::col("z", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"a_xz", "a", {{"x"}, {"z"}}});
  f.ddl(CreateTableDdl{"b", {Fixture::col("k", PhysType::I64, false), Fixture::col("y", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"b_y", "b", {{"y"}}});
  auto refused = [&](std::string_view sql) -> std::string {
    try {
      (void)plan_sql(f.cat(), sql);
    } catch (const ast::FeatureNotSupported& e) {
      return e.what();
    }
    return "accepted";
  };
  // b's scan by y is not in a.z's order, but no plan reaches both tables:
  // nothing names b's key from a, nor a's from b.
  CHECK(refused("SELECT a.k FROM a, b WHERE a.x = $1 AND b.y = $2 AND a.z = b.y ORDER BY a.z") ==
        "no plan: no equality names b's key");
  // Keyed by a.z, b joins a, and the plan of both fails on its order alone.
  CHECK(refused("SELECT a.k FROM a, b WHERE b.y = $1 AND b.k = a.z AND a.x = $2 ORDER BY a.k") ==
        "no plan: no index of a gives ORDER BY k");
}

TEST_CASE("search: COUNT from the keys or by a loop, by the count of the range", "[sql][search][cost]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  const std::string sql = "SELECT count(*) FROM t WHERE name = $1";
  // 10 rows: about one under a name, one cursor step against
  // bc_count_range's two record reads.
  auto few = plan_sql(cat, sql, {{"t", 10}});
  CHECK(has(few, "while (bc_scan_next(cx, c))"));
  CHECK_FALSE(has(few, "bc_count_range"));
  CHECK(few.cost == Catch::Approx(1));
  // A thousand: a hundred steps against two reads.
  auto many = plan_sql(cat, sql, {{"t", 1000}});
  CHECK(has(many, "bc_count_range"));
  CHECK(many.cost == Catch::Approx(2));
  // Uncounted (no database): costed as kUncountedRows, so from the keys.
  CHECK(has(plan_sql(cat, sql), "bc_count_range"));
}

TEST_CASE("search: an ORDER BY keeps the plan in its order, even where another costs less", "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"o",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, false),
                        Fixture::col("b", PhysType::I64, false), Fixture::col("c", PhysType::I64, false)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"o_ab", "o", {{"a"}, {"b"}}});
  f.ddl(CreateIndexDdl{"o_ac", "o", {{"a"}, {"c"}}});
  const auto& cat = f.cat();
  const auto& o = *cat.table("o");
  const auto ab = "IDX_" + std::to_string(o.indexes[0].id);
  const auto ac = "IDX_" + std::to_string(o.indexes[1].id);
  // Without an order, the longer prefix: o_ab, two parts compared.
  auto any = plan_sql(cat, "SELECT k FROM o WHERE a = $1 AND b = $2", {{"o", 10'000}});
  CHECK(has(any, "bc_key_params(cx, " + ab + ", 0, 2)"));
  CHECK(any.cost == Catch::Approx(100));  // 10,000 / 10 / 10 entries, covering
  // ORDER BY c: only o_ac gives it, b a filter on each of its rows.
  auto ordered = plan_sql(cat, "SELECT k FROM o WHERE a = $1 AND b = $2 ORDER BY c", {{"o", 10'000}});
  CHECK(has(ordered, "bc_scan_open(cx, " + ac + ", lo, lo, BC_SCAN_PREFIX)"));
  CHECK(ordered.cost == Catch::Approx(2000));  // 1,000 entries, each row fetched for its filter
  // A LIMIT stops the scan early: 1 of the 100 rows the filter keeps.
  auto limited = plan_sql(cat, "SELECT k FROM o WHERE a = $1 AND b = $2 ORDER BY c LIMIT 1", {{"o", 10'000}});
  CHECK(has(limited, ac));
  CHECK(limited.cost == Catch::Approx(20));
}

TEST_CASE("search: a point lookup beats a unique index's one entry; a covering scan beats a fetching one",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"w", {Fixture::col("k", PhysType::I64, false), Fixture::col("u", PhysType::I64, false),
                             Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"w_u", "w", {{"u"}}, true});
  const auto& cat = f.cat();
  auto both = plan_sql(cat, "SELECT v FROM w WHERE k = $1 AND u = $2", {{"w", 1000}});
  CHECK(has(both, "bc_get(cx, bc_key_params(cx, TBL_"));
  CHECK(both.cost == Catch::Approx(1));
  // The unique index names at most one entry, and fetches its row.
  auto by_u = plan_sql(cat, "SELECT v FROM w WHERE u = $1", {{"w", 1000}});
  CHECK(has(by_u, "bc_scan_row"));
  CHECK(by_u.cost == Catch::Approx(2));
  // k is in every index key: no row fetched.
  auto covered = plan_sql(cat, "SELECT k FROM w WHERE u = $1", {{"w", 1000}});
  CHECK_FALSE(has(covered, "bc_scan_row"));
  CHECK(covered.cost == Catch::Approx(1));
}

TEST_CASE("search: search_all returns every plan it costed, cheapest first, the one search picks among them",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"o",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, false),
                        Fixture::col("b", PhysType::I64, false)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"o_a", "o", {{"a"}}});
  f.ddl(CreateIndexDdl{"o_ab", "o", {{"a"}, {"b"}}});
  const auto& cat = f.cat();
  auto all = [&](std::string_view sql) {
    auto b = bound::bind(ast::parse(sql), cat);
    return generate_alternatives(std::move(b), cat, sql);
  };
  // o_a and o_ab reach a; and COUNT from the keys or by a loop over each.
  auto plans = all("SELECT count(*) FROM o WHERE a = $1");
  REQUIRE(plans.size() == 4);
  for (std::size_t i = 1; i < plans.size(); ++i) CHECK(plans[i - 1].cost <= plans[i].cost);
  CHECK(plans[0].source == plan_sql(cat, "SELECT count(*) FROM o WHERE a = $1").source);
  // k by the primary key, by o_a and by o_ab; a point lookup keeps its one plan.
  CHECK(all("SELECT a FROM o WHERE k = $1").size() == 1);
  CHECK(all("SELECT b FROM o WHERE a = $1 AND k >= $2").size() == 3);
}
