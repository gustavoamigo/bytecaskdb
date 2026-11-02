// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The rewrites and the search on logical plans built by hand: shapes the
// binder does not build from today's SQL, but that the rewrites or another
// front end may hand the search.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
  return logical::node(logical::Aggregate{std::move(in), std::move(outs), {}, false, {}});
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
  CHECK(refusal(f.cat(), std::move(foreign), 1) ==
        "no plan: a result column that is not an expression of columns of its tables");

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

  // u.k < t.qty names no row of u by its key: under t's one row, u is read
  // once by a full scan, the conjunct filtering its rows.
  auto lt = generate(f.cat(), join({logical::compare(Op::Lt, col(u, "k"), col(t, "qty"))}), 1);
  CHECK(has(lt, "bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);"));
  CHECK(has(lt, "!(bc_col_i64(cx, row1, COL_4_0) < bc_col_i64(cx, row, COL_1_2))"));

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

  // a small: scan a's 100 entries for x, covering, since a_x's key holds k,
  // which the result and the join read; and look up b by its key for each
  // of the 100: 100 + 100.
  auto small_a = plan_sql(cat, sql, {{"a", 1000}, {"b", 1'000'000}});
  CHECK(scans(small_a, a_x));
  CHECK_FALSE(scans(small_a, b_y));
  CHECK_FALSE(has(small_a, "bc_scan_row"));
  CHECK(has(small_a, "bc_key_part_i64(cx, IDX_" + a_x + ", bc_scan_key(cx, c), PART_" + a_x));
  CHECK(small_a.cost == Catch::Approx(200));
  // b small: the other order.
  auto small_b = plan_sql(cat, sql, {{"a", 1'000'000}, {"b", 1000}});
  CHECK(scans(small_b, b_y));
  CHECK_FALSE(scans(small_b, a_x));
  CHECK(small_b.cost == Catch::Approx(200));
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
  // u by its point lookup, 1; then t's 100 entries under name, no row
  // fetched, since nothing reads t's columns: 1 + 100. Scanning t first and
  // looking u up for each of its 100 rows would be 200 + 100.
  auto once = plan_sql(cat, sql, {{"t", 1000}, {"u", 1000}});
  CHECK(once.cost == Catch::Approx(101));
  CHECK_FALSE(has(once, "bc_scan_row"));
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
  REQUIRE(all.size() >= 2);
  CHECK(all[0].source == once.source);
  // t's scan covers: nothing reads its rows. 100 + 100.
  auto loop = std::ranges::find_if(all, [](const GeneratedPlan& g) { return g.cost == Catch::Approx(200); });
  REQUIRE(loop != all.end());
  CHECK(has(*loop, "bc_view row1 = bc_get(cx, bc_key_params(cx, TBL_"));
  CHECK_FALSE(has(*loop, "bc_scan_row"));
}

TEST_CASE("search: a table read once is filtered by the join's conjuncts, which its cost and rows count",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true),
                        Fixture::col("dv", PhysType::F64, true)},
                       {"k"}});
  f.ddl(CreateTableDdl{"w",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("d", PhysType::F64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"w_a", "w", {{"a"}}});
  const auto& cat = f.cat();
  // u by its point lookup, then t's 100 entries under name, each row fetched
  // for the join's conjunct: 1 + 200, and 1 x 100 x 1/10 rows. Under LIMIT 5
  // that is 201 x 5 / 10. Scanning t first and looking u up for each of its
  // rows: (200 + 100) x 5 / 10.
  const std::string sql = "SELECT qty FROM u, t WHERE k = $1 AND name = $2 AND t.qty = u.v";
  auto once = plan_sql(cat, sql, {{"t", 1000}, {"u", 1000}});
  CHECK(once.cost == Catch::Approx(201));
  CHECK(plan_sql(cat, sql + " LIMIT 5", {{"t", 1000}, {"u", 1000}}).cost == Catch::Approx(100.5));
  // t's rows compared with u's row, inside t's loop.
  auto scan = once.source.find("bc_view row1 = bc_scan_row(cx, c);");
  auto joined = once.source.find("bc_col_i64(cx, row1, COL_");
  REQUIRE(scan != std::string::npos);
  REQUIRE(joined != std::string::npos);
  CHECK(scan < joined);
  CHECK(once.source.find("bc_col_i64(cx, row, COL_", joined) != std::string::npos);
  // A join conjunct on DOUBLE columns is a filter neither order takes: w's
  // key is named by nothing, and the conjunct filters w read once, or u
  // looked up per row.
  try {
    (void)plan_sql(cat, "SELECT w.k FROM u, w WHERE u.k = $1 AND w.a = $2 AND w.d = u.dv");
    FAIL("planned");
  } catch (const ast::FeatureNotSupported& e) {
    CHECK(std::string{e.what()} ==
          "no plan: a filter on column \"d\", a REAL or DOUBLE PRECISION, is not supported yet");
  }
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
  // Nothing names b's key from a, nor a's from b: a hash join, or each
  // order reading its inner table once per outer row.
  CHECK(refused("SELECT a.k FROM a, b WHERE a.x = $1 AND b.y = $2 AND a.z = b.y ORDER BY a.z") == "accepted");
  // Keyed by a.z, b joins a, sorted by a.k.
  CHECK(refused("SELECT a.k FROM a, b WHERE b.y = $1 AND b.k = a.z AND a.x = $2 ORDER BY a.k") == "accepted");
  // A filter on a DOUBLE in every plan of both tables, past a plan of one
  // table that the other's fails.
  f.ddl(CreateTableDdl{"d", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::F64, true)}, {"k"}});
  CHECK(refused("SELECT a.k FROM a, d WHERE a.x = $1 AND d.k = a.z AND d.v < $2") ==
        "no plan: a filter on column \"v\", a REAL or DOUBLE PRECISION, is not supported yet");
  // e's index takes the DOUBLE bound, and its full scan leaves it a filter:
  // the plans by e_v are planned, past the full scan's DOUBLE filter.
  f.ddl(CreateTableDdl{"e",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::F64, true),
                        Fixture::col("y", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"e_v", "e", {{"v"}}});
  CHECK(refused("SELECT a.k FROM a, e WHERE a.x = $1 AND e.v < $2 AND e.y = a.z") == "accepted");
  // A join of two scans sorted by both tables' columns: a sorter per table.
  CHECK(refused("SELECT a.k FROM a, b WHERE a.x = $1 AND b.y = $2 AND a.z = b.y ORDER BY b.k, a.k") == "accepted");
}

TEST_CASE("search: a join of two loops is sorted by a sorter per loop, read in step",
          "[sql][search][cost][hash]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"p",
                       {Fixture::col("a", PhysType::I64, false), Fixture::col("v", PhysType::I64, true),
                        Fixture::col("w", PhysType::I64, false)},
                       {"a"}});
  f.ddl(CreateIndexDdl{"p_w", "p", {{"w"}}});
  const auto& cat = f.cat();
  // No equality between the sides, and neither named by its whole key: a
  // nested loop of two scans, t's under the name and p's under w. Sorted by
  // one table's column or by both, each row of the pair goes to its own
  // sorter with the same key; the loop steps the first and reads the second
  // at the same place.
  for (const auto* order : {"p.v", "p.v, t.id DESC", "t.qty"}) {
    const auto sql = std::string{"SELECT t.id, p.a FROM t, p WHERE t.name = $1 AND p.w = $2 ORDER BY "} + order;
    INFO(sql);
    auto g = plan_sql(cat, sql, {{"t", 100}, {"p", 100}});
    CHECK(has(g, "bc_sorter s0 = bc_sorter_open(cx, 0);"));
    CHECK(has(g, "bc_sorter s1 = bc_sorter_open(cx, 0);"));
    CHECK(has(g, "bc_view skv = bc_sort_key_end(cx, sk);"));
    CHECK(has(g, "bc_sorter_add(cx, s0, skv, row);"));
    CHECK(has(g, "bc_sorter_add(cx, s1, skv, row1);"));
    CHECK(has(g, "while (bc_sorter_next(cx, s0, &p0)) {"));
    CHECK(has(g, "bc_view p1 = bc_sorter_nth(cx, s1, r1);"));
    CHECK(has(g, "bc_emit_col(cx, p0, COL_1_0);"));
    CHECK(has(g, "bc_emit_col(cx, p1, COL_4_0);"));
    // t's 10 rows under the name, each fetched, times p's 10 under w, each
    // fetched; the 100 pairs held, at a step and the sort's weight in the
    // first sorter and the weight in the second.
    CHECK(g.cost == Catch::Approx(2 * 10 + 10 * 2 * 10 + 100 * (1 + kSortWeight) + 100 * kSortWeight));
  }
  // Under LIMIT n both sorters keep n: each keeps the earliest of equal keys,
  // so the same pairs.
  auto top = plan_sql(cat, "SELECT t.id, p.a FROM t, p WHERE t.name = $1 AND p.w = $2 ORDER BY p.v LIMIT 4",
                      {{"t", 100}, {"p", 100}});
  CHECK(has(top, "bc_sorter s0 = bc_sorter_open(cx, 4);"));
  CHECK(has(top, "bc_sorter s1 = bc_sorter_open(cx, 4);"));
  CHECK(has(top, "while (n < 4 && bc_sorter_next(cx, s0, &p0)) {"));
  // A hash join sorted by both tables' columns: the sort above the join, a
  // sorter for the probe side's row and one for the row held.
  auto hashed = plan_sql(cat, "SELECT t.id, p.a FROM t, p WHERE t.name = $1 AND p.v = t.qty ORDER BY p.a, t.id",
                         {{"t", 100}, {"p", 1000}});
  CHECK(has(hashed, "// built on TBL_1's rows"));
  CHECK(hashed.source.find("bc_hash_find") < hashed.source.find("bc_sorter_add(cx, s0, skv, row);"));
  CHECK(has(hashed, "bc_sorter_add(cx, s1, skv, b0);"));
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

TEST_CASE("search: a plan in the ORDER BY's order is kept, and compared with a cheaper one sorted",
          "[sql][search][cost]") {
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
  // ORDER BY c: o_ac gives it, b a filter on each of its 1,000 rows, each
  // fetched: 2,000. o_ab's 100 rows, fetched for the sorter and sorted, cost
  // less: 200 + 100 x (1 + 0.5), and 2 for the count of the choice point
  // between the two under the sort.
  const std::string sql = "SELECT k FROM o WHERE a = $1 AND b = $2 ORDER BY c";
  auto sorted = plan_sql(cat, sql, {{"o", 10'000}});
  CHECK(has(sorted, "bc_sorter s0 = bc_sorter_open(cx, 0);"));
  CHECK(has(sorted, "bc_sort_key_col(cx, sk, row, COL_4_3, BC_SORT_ASC, BC_SORT_NULLS_LAST);"));
  CHECK(sorted.cost == Catch::Approx(200 + 150 + 2));
  // The plan in the order is kept, beside it.
  auto counts = [](const Table&) -> std::optional<std::uint64_t> { return 10'000; };
  auto all = generate_alternatives(bound::bind(ast::parse(sql), cat), cat, sql, counts);
  auto in_order = std::ranges::find_if(all, [](const GeneratedPlan& g) { return g.cost == Catch::Approx(2000); });
  REQUIRE(in_order != all.end());
  CHECK(has(*in_order, "bc_scan_open(cx, " + ac + ", lo, lo, BC_SCAN_PREFIX)"));
  CHECK_FALSE(has(*in_order, "bc_sorter"));
  // A LIMIT stops the scan in the order early: 1 of the 100 rows the filter
  // keeps, 20, where the sort still reads all 100.
  auto limited = plan_sql(cat, sql + " LIMIT 1", {{"o", 10'000}});
  CHECK(has(limited, ac));
  CHECK_FALSE(has(limited, "bc_sorter"));
  CHECK(limited.cost == Catch::Approx(20));
  // Under LIMIT 50 the scan in the order costs 1,000, the sort, keeping 50,
  // 200 + 50 x 1.5. No choice point: past o_ac's 1,000 entries, which hold
  // o_ab's, the two sorted plans cost the same.
  auto top = plan_sql(cat, sql + " LIMIT 50", {{"o", 10'000}});
  CHECK(has(top, "bc_sorter s0 = bc_sorter_open(cx, 50);"));
  CHECK(top.cost == Catch::Approx(200 + 75));
  // LIMIT 0 returns no row, in any order: no sort.
  CHECK_FALSE(has(plan_sql(cat, sql + " LIMIT 0", {{"o", 10'000}}), "bc_sorter"));
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
  // o_a and o_ab reach a; and COUNT from the keys or by a loop over each,
  // and by a loop over a full scan, a filtering its rows.
  auto plans = all("SELECT count(*) FROM o WHERE a = $1");
  REQUIRE(plans.size() == 5);
  for (std::size_t i = 1; i < plans.size(); ++i) CHECK(plans[i - 1].cost <= plans[i].cost);
  CHECK(plans[0].source == plan_sql(cat, "SELECT count(*) FROM o WHERE a = $1").source);
  // k by the primary key, by o_a and by o_ab; a point lookup keeps its one plan.
  CHECK(all("SELECT a FROM o WHERE k = $1").size() == 1);
  // Each of the three carries a choice point, and is printed once per
  // branch.
  auto ranges = all("SELECT b FROM o WHERE a = $1 AND k >= $2");
  REQUIRE(ranges.size() == 6);
  for (std::size_t i = 0; i < ranges.size(); i += 2) {
    CHECK(has(ranges[i], "bc_count_range(cx, "));
    CHECK(has(ranges[i], ", lo0, hi0, INT64_MAX);"));
    CHECK(has(ranges[i + 1], ", lo0, hi0, 0);"));
    CHECK(ranges[i].cost == Catch::Approx(ranges[i + 1].cost));
  }
}

namespace {

// The threshold of the plan's first choice point, or -1 without one.
auto threshold(const GeneratedPlan& g) -> std::int64_t {
  constexpr std::string_view kIf = "if (count0 < ";
  auto at = g.source.find(kIf);
  if (at == std::string::npos) return -1;
  return std::stoll(g.source.substr(at + kIf.size()));
}

}  // namespace

TEST_CASE("search: a choice point where two ranges of a table cross, its threshold where their costs do",
          "[sql][search][cost][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"c",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("b", PhysType::I64, true), Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"c_a", "c", {{"a"}}});
  f.ddl(CreateIndexDdl{"c_b", "c", {{"b"}}});
  const auto& cat = f.cat();
  const auto c_a = "IDX_" + std::to_string(cat.table("c")->indexes[0].id);
  const auto c_b = "IDX_" + std::to_string(cat.table("c")->indexes[1].id);
  const std::string sql = "SELECT v FROM c WHERE a = $1 AND b = $2";

  // 10,000 rows: each index reaches 1,000 entries and fetches each row,
  // 2,000. c_a first, by its id: past 1,000 of its entries it costs more
  // than c_b's estimate.
  auto g = plan_sql(cat, sql, {{"c", 10'000}});
  CHECK(threshold(g) == 1001);
  CHECK(has(g, "int64_t count0 = bc_count_range(cx, " + c_a + ", lo0, hi0, 1001);"));
  // The first plan's scan opens on the range the count built.
  CHECK(has(g, "bc_cursor c = bc_scan_open(cx, " + c_a + ", lo0, lo0, BC_SCAN_PREFIX);"));
  // Each branch scans its index, and filters on the other column.
  auto below = g.source.find("if (count0 < 1001) {");
  auto other = g.source.find("} else {", below);
  REQUIRE(other != std::string::npos);
  CHECK(g.source.find("bc_scan_open(cx, " + c_a, below) < other);
  CHECK(g.source.find("bc_scan_open(cx, " + c_b, other) != std::string::npos);
  // The count's two reads are charged.
  CHECK(g.cost == Catch::Approx(2000 + 2));
  // The threshold follows the counts: 1,000 rows, 100 entries; 10 rows,
  // 1, which fetching costs 2, and a count of 2 passes. A table of one row:
  // no count passes 1.
  CHECK(threshold(plan_sql(cat, sql, {{"c", 1000}})) == 101);
  CHECK(threshold(plan_sql(cat, sql, {{"c", 10}})) == 2);
  CHECK(threshold(plan_sql(cat, sql, {{"c", 1}})) == -1);
}

TEST_CASE("search: no choice point where the costs can't cross within the count's range", "[sql][search][cost][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"c",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("b", PhysType::I64, true), Fixture::col("v", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"c_a", "c", {{"a"}}});
  f.ddl(CreateIndexDdl{"c_ab", "c", {{"a"}, {"b"}}});
  f.ddl(CreateIndexDdl{"c_v", "c", {{"v"}}, true});
  const auto& cat = f.cat();
  const std::map<std::string, std::uint64_t> rows{{"c", 10'000}};
  // c_ab's range lies inside c_a's: however many entries it holds, c_a's
  // holds at least as many.
  auto inside = plan_sql(cat, "SELECT v FROM c WHERE a = $1 AND b = $2", rows);
  CHECK(has(inside, "bc_key_params(cx, IDX_" + std::to_string(cat.table("c")->indexes[1].id) + ", 0, 2)"));
  CHECK(threshold(inside) == -1);
  // Under LIMIT 1, the first entry ends either scan: neither cost grows
  // with the count.
  CHECK(threshold(plan_sql(cat, "SELECT k FROM c WHERE a = $1 AND v >= $2 LIMIT 1", rows)) == -1);
  // A unique index names at most one entry; a point lookup one row.
  CHECK(threshold(plan_sql(cat, "SELECT k FROM c WHERE a = $1 AND v = $2", rows)) == -1);
  CHECK(threshold(plan_sql(cat, "SELECT v FROM c WHERE a = $1 AND k = $2", rows)) == -1);
  // Without a LIMIT, the same two ranges cross.
  CHECK(threshold(plan_sql(cat, "SELECT k FROM c WHERE a = $1 AND v >= $2", rows)) > 1);
}

TEST_CASE("search: a choice point at a loop a sorter holds reads rows in both plans, though an index covers",
          "[sql][search][cost][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"p",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true),
                        Fixture::col("w", PhysType::I64, false)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"p_w", "p", {{"w"}}});
  const auto& cat = f.cat();
  const auto p_w = "IDX_" + std::to_string(cat.table("p")->indexes[0].id);
  // No equality between the sides: t by its key, then p by p_w's range or
  // its primary key's, a choice point between them. p_w holds w and k, all
  // p's columns the statement reads, so it could cover. Read above the sort,
  // p's row is held by a sorter: each plan of the choice point reads it.
  const std::string from = " FROM t, p WHERE t.id = $1 AND p.w = $2 AND p.k > $3 ORDER BY t.qty, p.k";
  const std::map<std::string, std::uint64_t> rows{{"t", 100}, {"p", 100}};
  auto held = plan_sql(cat, "SELECT t.id, p.w" + from, rows);
  CHECK(threshold(held) == 11);
  const auto other = held.source.find("} else {");
  REQUIRE(other != std::string::npos);
  CHECK(held.source.find("bc_view row1 = bc_scan_row(cx, c);") < other);
  CHECK(held.source.find("bc_view row1 = bc_scan_row(cx, c);", other) != std::string::npos);
  CHECK_FALSE(has(held, "bc_key_part_"));
  CHECK(has(held, "bc_emit_col(cx, p0, COL_"));
  // Every plan costed, with every combination of its choice points'
  // branches, passes check: no sorter holds a covering scan's entry.
  for (const auto* sql : {"SELECT t.id, p.w", "SELECT t.id"}) {
    INFO(sql);
    const auto text = sql + from;
    CHECK_NOTHROW(generate_alternatives(bound::bind(ast::parse(text), cat), cat, text,
                                        [&](const Table& t) { return std::optional{rows.at(t.name)}; }));
  }
  // Not read above the sort, p's row is held by none: p_w covers, and the
  // sort key's part is read from its entry.
  auto free = plan_sql(cat, "SELECT t.id" + from, rows);
  CHECK(has(free, "bc_sort_key_i64(cx, sk, bc_key_part_i64(cx, " + p_w + ", bc_scan_key(cx, c), PART_"));
  CHECK(has(free, "bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), bc_empty(cx));"));
  CHECK(free.cost < held.cost);
}

TEST_CASE("search: a sort holds only the rows read above it; another loop's access may cover",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"p",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("v", PhysType::I64, true), Fixture::col("w", PhysType::I64, false)},
                       {"k"}});
  // p_wva holds every column of p the statement reads but v, and does not
  // give ORDER BY p.a under w = $2.
  f.ddl(CreateIndexDdl{"p_wva", "p", {{"w"}, {"v"}, {"a"}}});
  const auto& cat = f.cat();
  const auto p_wva = "IDX_" + std::to_string(cat.table("p")->indexes[0].id);
  const std::map<std::string, std::uint64_t> rows{{"t", 100}, {"p", 100}};
  // The shape of golden/sort_cross_join: a nested loop of two scans, sorted
  // by p's column, only t's read above the sort. One sorter, of t's rows; p
  // covers, and the key's part, nullable, is read from its entry.
  auto g = plan_sql(cat, "SELECT t.id FROM t, p WHERE t.name = $1 AND p.w = $2 ORDER BY p.a", rows);
  CHECK(has(g, "bc_sorter s0 = bc_sorter_open(cx, 0);"));
  CHECK_FALSE(has(g, "bc_sorter s1"));
  CHECK_FALSE(has(g, "bc_sorter_nth"));
  CHECK_FALSE(has(g, "bc_scan_row(cx, c1)"));
  CHECK(has(g, "if (bc_key_part_null(cx, " + p_wva + ", bc_scan_key(cx, c1), PART_"));
  CHECK(has(g, "bc_sort_key_null(cx, sk, BC_SORT_ASC, BC_SORT_NULLS_LAST);"));
  CHECK(has(g, "bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);"));
  // t's 10 rows under the name, each fetched; p's 10 under w, covered, per
  // row of t; the 100 pairs held in one sorter.
  CHECK(g.cost == Catch::Approx(2 * 10 + 10 * 10 + 100 * (1 + kSortWeight)));
  // p's v read above the sort: a sorter of p's rows beside t's, and p reads
  // them.
  auto both = plan_sql(cat, "SELECT t.id, p.v FROM t, p WHERE t.name = $1 AND p.w = $2 ORDER BY p.a", rows);
  CHECK(has(both, "bc_sorter_add(cx, s1, skv, row1);"));
  CHECK(has(both, "bc_view row1 = bc_scan_row(cx, c1);"));
  CHECK(both.cost == Catch::Approx(2 * 10 + 10 * 2 * 10 + 100 * (1 + kSortWeight) + 100 * kSortWeight));
}

TEST_CASE("search: a hash join that sorts its probe side reads only that side's rows for it",
          "[sql][search][cost][hash]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true),
                        Fixture::col("x", PhysType::I64, true)},
                       {"k"}});
  // u_xv covers u's k, v and x, and does not give ORDER BY u.k under x = $1.
  f.ddl(CreateIndexDdl{"u_xv", "u", {{"x"}, {"v"}}});
  f.ddl(CreateTableDdl{"s",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("b", PhysType::I64, true)},
                       {"k"}});
  const auto& cat = f.cat();
  const auto u_xv = "IDX_" + std::to_string(cat.table("u")->indexes[0].id);
  const std::map<std::string, std::uint64_t> rows{{"t", 100}, {"u", 10'000}};
  // ORDER BY u.k is all the probe side's: u is sorted before it probes,
  // decided before any loop is made to read rows. Its sorter holds u's row,
  // though nothing above reads one, so u_xv's range fetches each; t, held,
  // reads its rows for the hash table. The plan is costed as it runs: t's
  // 100 rows held, u's 1,000 entries fetched and sorted, 10,000 pairs.
  auto g = plan_sql(cat, "SELECT t.id FROM t, u WHERE u.x = $1 AND u.v = t.qty ORDER BY u.k", rows);
  CHECK(has(g, "// built on TBL_1's rows"));
  CHECK(has(g, "bc_cursor c = bc_scan_open(cx, " + u_xv + ", lo, lo, BC_SCAN_PREFIX);"));
  CHECK(g.source.find("bc_view row = bc_scan_row(cx, c);", g.source.find(u_xv + ", lo, lo")) <
        g.source.find("bc_sorter_add"));
  CHECK(has(g, "bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), row);"));
  CHECK(g.cost == Catch::Approx(100 * (2 + kHashWeight) + 1000 * (2 + 1 + kSortWeight) + 100 * 1000 / 10));
  // Two keys on one probe column: its NULL test, before the sort, once.
  auto two = plan_sql(cat, "SELECT s.k FROM s, u WHERE u.x = $1 AND u.v = s.a AND u.v = s.b ORDER BY u.k",
                      {{"s", 100}, {"u", 10'000}});
  CHECK(has(two, "bc_hash_open"));
  const auto u_v = "bc_col_null(cx, row, COL_" + std::to_string(cat.table("u")->id) + "_1)";
  const auto first = two.source.find(u_v);
  REQUIRE(first != std::string::npos);
  CHECK(first < two.source.find("bc_sorter_add"));
  CHECK(two.source.find(u_v, first + 1) == std::string::npos);
}

TEST_CASE("search: a table read once carries its choice point under the outer row", "[sql][search][cost][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"c",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("b", PhysType::I64, true), Fixture::col("x", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"c_a", "c", {{"a"}}});
  f.ddl(CreateIndexDdl{"c_b", "c", {{"b"}}});
  const auto& cat = f.cat();
  // t by its key, then c read once by c_a or c_b, each row fetched for the
  // join's conjunct: 1 + 2 x 1,000, and the count.
  auto g = plan_sql(cat, "SELECT c.k FROM t, c WHERE t.id = $1 AND c.a = $2 AND c.b = $3 AND c.x = t.qty",
                    {{"t", 10'000}, {"c", 10'000}});
  CHECK(threshold(g) == 1001);
  CHECK(g.cost == Catch::Approx(1 + 2000 + 2));
  auto get = g.source.find("bc_get(cx, bc_key_params(cx, TBL_");
  auto count = g.source.find("bc_count_range(");
  REQUIRE(get != std::string::npos);
  CHECK(get < count);
  // Both branches compare c's rows with t's.
  auto joined = g.source.find("bc_col_i64(cx, row, COL_");
  CHECK(g.source.find("bc_col_i64(cx, row, COL_", joined + 1) != std::string::npos);
}

TEST_CASE("search: a full scan is the plan when nothing bounds the table, or when it costs least",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  // Every row read, each filtered: the table's rows.
  auto full = plan_sql(cat, "SELECT id FROM t WHERE qty = $1", {{"t", 1000}});
  CHECK(has(full, "bc_scan_open(cx, TBL_1, lo, hi, BC_SCAN_FORWARD);"));
  CHECK(full.cost == Catch::Approx(1000));
  // name > $1 reaches a third of the index's entries, each row fetched for
  // qty: 2 x 333 against 1,000. Two bounds, a ninth: 222.
  CHECK(has(plan_sql(cat, "SELECT qty FROM t WHERE name > $1", {{"t", 1000}}), "IDX_3"));
  // No choice point at a full scan, whose count no parameter changes, nor
  // a full scan as a choice point's second plan.
  CHECK(threshold(full) == -1);
  CHECK(threshold(plan_sql(cat, "SELECT qty FROM t WHERE name = $1", {{"t", 1000}})) == -1);
}

TEST_CASE("search: a table no key reaches is hash joined, or read once per outer row, by their costs",
          "[sql][search][cost][hash]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT u.k FROM t, u WHERE t.name = $1 AND u.v = t.qty";
  // 10 rows of t, 1 under the name, fetched for qty; 100 of u. Reading u once
  // per row of t, 2 + 100, costs less than a hash join: the same reads, plus
  // the row it holds and the pairs it probes.
  auto loop = plan_sql(cat, sql, {{"t", 10}, {"u", 100}});
  CHECK(has(loop, "bc_cursor c1 = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);"));
  CHECK(has(loop, "while (bc_scan_next(cx, c1)) {"));
  CHECK_FALSE(has(loop, "bc_hash"));
  CHECK(loop.cost == Catch::Approx(2 + 100));
  // 40 rows of t, 4 under the name: u once per row costs 8 + 4 x 100; the
  // hash join reads each side once, holds t's 4 rows and steps through 4 x
  // 100 / 10 pairs.
  auto hashed = plan_sql(cat, sql, {{"t", 40}, {"u", 100}});
  CHECK(has(hashed, "bc_hash h0 = bc_hash_open(cx);  // built on TBL_1's rows"));
  CHECK(hashed.cost == Catch::Approx(8 + 4 * (1 + kHashWeight) + 100 + 40));
  // The build side's loop puts each row, in a block of its own; then the probe
  // side's loop finds, and steps through the rows held under its key. A NULL
  // key column is neither put nor probed.
  const auto put = hashed.source.find("bc_hash_put(cx, h0, bc_sort_key_end(cx, hk0), row);");
  const auto find = hashed.source.find("bc_hash_iter q0 = bc_hash_find(cx, h0, bc_sort_key_end(cx, hk0));");
  REQUIRE(put != std::string::npos);
  REQUIRE(find != std::string::npos);
  CHECK(put < find);
  CHECK(hashed.source.find("bc_cursor c = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);", put) < find);
  CHECK(has(hashed, "if (bc_col_null(cx, row, COL_1_2)) continue;"));
  CHECK(has(hashed, "bc_sort_key_i64(cx, hk0, bc_col_i64(cx, row, COL_1_2), BC_SORT_ASC, BC_SORT_NULLS_FIRST);"));
  CHECK(has(hashed, "if (bc_col_null(cx, row, COL_4_1)) continue;"));
  CHECK(has(hashed, "while (bc_hash_next(cx, q0, &b0)) {"));
  CHECK(has(hashed, "bc_emit_col(cx, row, COL_4_0);"));
}

TEST_CASE("search: a hash join builds on the side the counts make smaller", "[sql][search][cost][hash]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT t.id, u.k FROM t, u WHERE u.v = t.qty";
  // Both tables read in full: the hash table holds the smaller one's rows,
  // and the larger one probes it. Each costs both scans, the rows held and
  // 1,000 x 10 / 10 pairs.
  auto on_u = plan_sql(cat, sql, {{"t", 1000}, {"u", 10}});
  CHECK(has(on_u, "// built on TBL_4's rows"));
  CHECK(has(on_u, "bc_emit_col(cx, b0, COL_4_0);"));
  CHECK(on_u.cost == Catch::Approx(1000 + 10 + 10 * (1 + kHashWeight) + 1000));
  auto on_t = plan_sql(cat, sql, {{"t", 10}, {"u", 1000}});
  CHECK(has(on_t, "// built on TBL_1's rows"));
  CHECK(has(on_t, "bc_emit_col(cx, b0, COL_1_0);"));
  CHECK(on_t.cost == Catch::Approx(on_u.cost));
  // Under a LIMIT the build still holds every row; only the probes stop.
  auto limited = plan_sql(cat, sql + " LIMIT 10", {{"t", 1000}, {"u", 10}});
  CHECK(has(limited, "// built on TBL_4's rows"));
  CHECK(has(limited, "while (n < 10 && bc_hash_next(cx, q0, &b0)) {"));
  CHECK(limited.cost == Catch::Approx(10 + 10 * (1 + kHashWeight) + (1000 + 1000) * 10 / 1000.0));
  // A sorter holds the probe side's rows, sorted before it probes, every row
  // kept: so the side ORDER BY names is probed, and the hash table holds the
  // other's, whichever is smaller.
  auto sorted = plan_sql(cat, sql + " ORDER BY u.v", {{"t", 1000}, {"u", 10}});
  CHECK(has(sorted, "// built on TBL_1's rows"));
  CHECK(has(sorted, "bc_sorter s0 = bc_sorter_open(cx, 0);"));
  CHECK(sorted.source.find("bc_sorter_open") < sorted.source.find("bc_hash_find"));
  CHECK(sorted.cost == Catch::Approx(1000 * (2 + kHashWeight) + 10 * (2 + kSortWeight) + 1000));
  // Under LIMIT 3, the larger side probing and its pairs sorted, three kept
  // in a sorter of each table's rows, cost less: 2,023 and 3 x 2.
  auto top = plan_sql(cat, sql + " ORDER BY u.v LIMIT 3", {{"t", 1000}, {"u", 10}});
  CHECK(has(top, "// built on TBL_4's rows"));
  CHECK(has(top, "bc_sorter s0 = bc_sorter_open(cx, 3);"));
  CHECK(has(top, "bc_sorter s1 = bc_sorter_open(cx, 3);"));
  CHECK(top.source.find("bc_hash_find") < top.source.find("bc_sorter_add"));
  CHECK(has(top, "while (n < 3 && bc_sorter_next(cx, s0, &p0)) {"));
  CHECK(top.cost == Catch::Approx(10 * (2 + kHashWeight) + 1000 + 1000 + 3 * (1 + kSortWeight) + 3 * kSortWeight));
  CHECK(has(plan_sql(cat, sql + " ORDER BY t.id DESC", {{"t", 1000}, {"u", 10}}), "// built on TBL_4's rows"));
}

TEST_CASE("search: under LIMIT 0 a plan reads nothing", "[sql][search][hash]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  const auto& cat = f.cat();
  // A hash join's build, a sort, a lookup, a count: none runs, and the
  // statement returns no row.
  for (const auto* sql : {
           "SELECT t.id, u.k FROM t, u WHERE u.v = t.qty LIMIT 0",
           "SELECT t.id FROM t, u WHERE t.name = $1 AND u.v = t.qty ORDER BY u.k LIMIT 0",
           "SELECT id FROM t WHERE name = $1 ORDER BY qty LIMIT 0",
           "SELECT id FROM t WHERE id = $1 LIMIT 0",
           "SELECT count(*) FROM t WHERE id = $1 LIMIT 0",
           "SELECT count(*) FROM t WHERE name = $1 LIMIT 0",
           "SELECT min(qty) FROM t WHERE name = $1 LIMIT 0",
       }) {
    INFO(sql);
    auto g = plan_sql(cat, sql, {{"t", 1000}, {"u", 10}});
    CHECK(has(g, "void plan(bc_ctx *cx) {\n}\n"));
    CHECK(g.objects_read.empty());
  }
}

TEST_CASE("search: a hash build that may pass the memory budget is a choice point, or left out",
          "[sql][search][cost][hash][memory][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT u.k FROM t, u WHERE t.name = $1 AND u.v = t.qty";
  RowCounts counts = [](const Table& t) -> std::optional<std::uint64_t> { return t.name == "t" ? 40 : 100; };
  auto plan = [&](std::uint64_t budget) {
    return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts, budget);
  };
  // A hash table of n rows holds at least 192 + n x (240 + 36 + 9) bytes: a
  // row of t holds at least its 12-byte header, an 8-byte bitmap and two
  // fixed columns, and its key a marker and a BIGINT. Past 10,000 from 35
  // rows. The rows t's range holds when the plan runs decide: below 35 the
  // hash join, from it u read once per row of t, which holds nothing.
  auto choice = plan(10'000);
  CHECK(has(choice, "int64_t count0 = bc_count_range(cx, IDX_3, lo0, hi0, 35);"));
  CHECK(has(choice, "bc_cursor c = bc_scan_open(cx, IDX_3, lo0, lo0, BC_SCAN_PREFIX);"));
  const auto other = choice.source.find("} else {");
  REQUIRE(other != std::string::npos);
  CHECK(choice.source.find("bc_hash_open") < other);
  CHECK(choice.source.find("bc_cursor c1 = bc_scan_open(cx, TBL_4, lo, hi, BC_SCAN_FORWARD);", other) != std::string::npos);
  CHECK(choice.cost == Catch::Approx(8 + 4 * (1 + kHashWeight) + 100 + 40 + 2));
  CHECK(threshold(choice) == 35);
  // Under 192 bytes no hash table fits: the plan avoids it.
  auto avoided = plan(100);
  CHECK_FALSE(has(avoided, "bc_hash"));
  CHECK(has(avoided, "while (bc_scan_next(cx, c1)) {"));
  // With no budget, the hash join alone.
  auto unbounded = plan(0);
  CHECK(has(unbounded, "bc_hash_open"));
  CHECK_FALSE(has(unbounded, "bc_count_range"));
}

TEST_CASE("search: the planner uses the memory budget", "[sql][search][cost][memory]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"o",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, false),
                        Fixture::col("c", PhysType::I64, false)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"o_c", "o", {{"c"}}});
  f.ddl(CreateIndexDdl{"o_a", "o", {{"a"}}});
  const auto& cat = f.cat();
  auto plan = [&](std::string_view sql, std::uint64_t rows, std::uint64_t budget) {
    RowCounts counts = [rows](const Table& t) -> std::optional<std::uint64_t> {
      return t.name == "o" ? std::optional{rows} : std::nullopt;
    };
    return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts, budget);
  };
  // ORDER BY c of the whole table: a full scan of o_c, in order, each row
  // fetched, 2,000; or a full scan sorted, 1,000 + 1,000 x 1.5, whose 1,000
  // rows hold at least 192 + 1,000 x 128 bytes: past a budget of 100,000,
  // that plan is left out, as it is never chosen.
  const std::string all = "SELECT k, a FROM o ORDER BY c";
  auto plans = [&](std::uint64_t budget) {
    RowCounts counts = [](const Table& t) -> std::optional<std::uint64_t> {
      return t.name == "o" ? std::optional<std::uint64_t>{1000} : std::nullopt;
    };
    return generate_alternatives(bound::bind(ast::parse(all), cat), cat, all, counts, budget);
  };
  auto sorts = [](const GeneratedPlan& g) { return has(g, "bc_sorter_open"); };
  CHECK(std::ranges::any_of(plans(0), sorts));
  CHECK(std::ranges::any_of(plans(200'000), sorts));
  // The count read at compile time ages as the table grows: the full scan
  // sorted is a choice point by the whole table's count, read when the plan
  // runs. Past (100,000 - 192) / 128 + 1 = 780 rows, o_c's scan in order
  // instead, whatever the table held at compile time.
  for (auto budget : {100'000, 200'000}) {
    for (const auto& g : plans(static_cast<std::uint64_t>(budget))) {
      if (sorts(g)) CHECK(has(g, "int64_t count0 = bc_count_range(cx, TBL_"));
    }
  }
  CHECK(threshold(plan(all, 10, 100'000)) == -1);  // the index costs less: no sort to choose by
  CHECK(plan(all, 1000, 100'000).cost == Catch::Approx(2000));
  // A range whose count depends on $n: a choice point by the budget. Past
  // (100,000 - 192) / 128 + 1 = 780 entries, o_c's scan in order instead.
  auto by_a = plan("SELECT k FROM o WHERE a = $1 ORDER BY c", 100'000, 100'000);
  CHECK(threshold(by_a) == 780);
  auto below = by_a.source.find("if (count0 < 780) {");
  auto other = by_a.source.find("} else {", below);
  REQUIRE(other != std::string::npos);
  CHECK(by_a.source.find("bc_sorter_open", below) < other);
  CHECK(by_a.source.find("bc_sorter", other) == std::string::npos);
  // With no budget, no such choice.
  CHECK_FALSE(has(plan("SELECT k FROM o WHERE a = $1 ORDER BY c", 100'000, 0), "count0"));
  // Nor under a LIMIT the budget holds.
  CHECK(threshold(plan("SELECT k FROM o WHERE a = $1 ORDER BY c LIMIT 10", 100'000, 100'000)) == -1);
}

TEST_CASE("search: a sort of a whole table, compiled while it was small, is chosen by its count as it runs",
          "[sql][search][cost][memory][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"w",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("x", PhysType::I64, false),
                        Fixture::col("y", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"w_x", "w", {{"x"}}});
  const auto& cat = f.cat();
  const auto table = "TBL_" + std::to_string(cat.table("w")->id);
  const auto index = "IDX_" + std::to_string(cat.table("w")->indexes[0].id);
  auto plan = [&](std::string_view sql, std::uint64_t rows, std::uint64_t budget) {
    RowCounts counts = [rows](const Table& t) -> std::optional<std::uint64_t> {
      return t.name == "w" ? std::optional{rows} : std::nullopt;
    };
    return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts, budget);
  };
  // 10 rows when compiled: the full scan, filtered, sorted, 10 + 3.3 x 1.5,
  // costs less than w_x's scan in order, 2 x 10. Its rows can't be fewer
  // than none nor more than the table's, so the table's count picks: from
  // 780, the sorter would pass the budget, and w_x's scan runs instead.
  auto g = plan("SELECT k FROM w WHERE y >= $1 ORDER BY x", 10, 100'000);
  CHECK(threshold(g) == 780);
  CHECK(has(g, "bc_view lo0 = bc_empty(cx);\n  bc_view hi0 = bc_empty(cx);\n  int64_t count0 = bc_count_range(cx, " +
                   table + ", lo0, hi0, 780);"));
  auto below = g.source.find("if (count0 < 780) {");
  auto other = g.source.find("} else {", below);
  REQUIRE(other != std::string::npos);
  CHECK(g.source.find("bc_sorter_open", below) < other);
  CHECK(g.source.find("bc_scan_open(cx, " + table + ", lo0, hi0, BC_SCAN_FORWARD)", below) < other);
  CHECK(g.source.find("bc_scan_open(cx, " + index, other) != std::string::npos);
  CHECK(g.source.find("bc_sorter", other) == std::string::npos);
  CHECK(g.cost == Catch::Approx(10 + 10.0 / 3 * 1.5 + 2));
  INFO(g.source);
  // The counted range is passed to the scan it belongs to, under the sort:
  // that scan, and no other, opens on lo0 and hi0.
  auto on_counted = [](const std::string& src) {
    std::size_t n = 0;
    constexpr std::string_view kOpen = ", lo0, hi0, BC_SCAN_";
    for (auto at = src.find(kOpen); at != std::string::npos; at = src.find(kOpen, at + 1)) ++n;
    return n;
  };
  CHECK(on_counted(g.source) == 1);
  CHECK(has(g, "bc_view lo = bc_empty(cx);\n    bc_view hi = bc_empty(cx);\n    bc_cursor c = bc_scan_open(cx, " +
                   index));
}

TEST_CASE("search: a choice point between a sort and a covering scan in the order passes check",
          "[sql][search][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"v",
                       {Fixture::col("id", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("b", PhysType::I64, true)},
                       {"id"}});
  f.ddl(CreateIndexDdl{"ix_a", "v", {{"a"}}});
  f.ddl(CreateIndexDdl{"ix_ab", "v", {{"a"}, {"b"}}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT a, b FROM v WHERE a = $1 ORDER BY b";
  RowCounts counts = [](const Table& t) -> std::optional<std::uint64_t> {
    return t.name == "v" ? std::optional<std::uint64_t>{1000} : std::nullopt;
  };
  // ix_a's range sorted, below the count past the budget; from it ix_ab's,
  // in the order and covering. Each alternative is checked (generate_*
  // runs physical::check): the covering scan is in the other branch.
  auto plans = generate_alternatives(bound::bind(ast::parse(sql), cat), cat, sql, counts, 100'000);
  const auto ix_ab = "IDX_" + std::to_string(cat.table("v")->indexes[1].id);
  auto both = std::ranges::find_if(plans, [&](const GeneratedPlan& g) {
    auto other = g.source.find("} else {");
    return has(g, "if (count0 < ") && other != std::string::npos &&
           g.source.find("bc_sorter_open") < other && g.source.find("bc_scan_key(cx, c)", other) != std::string::npos &&
           g.source.find("bc_scan_open(cx, " + ix_ab, other) != std::string::npos;
  });
  CHECK(both != plans.end());
  // The default budget's threshold, past 524K rows.
  auto big = [](const Table& t) -> std::optional<std::uint64_t> {
    return t.name == "v" ? std::optional<std::uint64_t>{600'000} : std::nullopt;
  };
  CHECK_NOTHROW(generate_alternatives(bound::bind(ast::parse(sql), cat), cat, sql, big, 64u << 20));
}

TEST_CASE("search: an unordered plan is kept at the cost of the plan it becomes, sorted and reading rows",
          "[sql][search][cost]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  // w_ab covers w's a, b and k.
  f.ddl(CreateTableDdl{"w",
                       {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                        Fixture::col("b", PhysType::I64, true)},
                       {"k"}});
  f.ddl(CreateIndexDdl{"w_ab", "w", {{"a"}, {"b"}}});
  const auto& cat = f.cat();
  const auto table = "TBL_" + std::to_string(cat.table("w")->id);
  // 2 rows of w, then t by its key. w_ab's range, a = $1, reaches its first
  // entry, 1 covering; the full scan reaches 2, a filtered. Neither gives
  // ORDER BY k, b, so the plan of w is kept, for the join, at the cost it has
  // sorted: a sorter holds a row, so w_ab's fetches each, 2, and the full
  // scan, first of equals, is kept. Sorted, it holds 0.2 rows against 1:
  // 2 + 0.2 lookups + 0.2 x (1.5 + a lookup again).
  auto g = plan_sql(cat, "SELECT w.k FROM w, t WHERE w.a = $1 AND t.id = w.k ORDER BY w.k, w.b", {{"w", 2}, {"t", 10}});
  INFO(g.source);
  CHECK(has(g, "bc_sorter_open"));
  CHECK(has(g, "bc_scan_open(cx, " + table + ", lo, hi, BC_SCAN_FORWARD)"));
  CHECK(g.cost == Catch::Approx(2 + 0.2 + 0.2 * 2.5));
}

TEST_CASE("search: crossing() takes no threshold where two costs cross more than once", "[sql][search][choice]") {
  auto first = [](double x) { return x; };
  // Once: the first costs more from 6.
  CHECK(crossing(first, [](double) { return 5.0; }, 1000) == 6);
  // Three times: past 5, under 2x from 10, past 50 from 51, as a sort's cost
  // and a clamp's can. No count picks either: no choice point.
  auto twice = [](double x) { return x < 10 ? 5.0 : x < 100 ? 2 * x : 50.0; };
  CHECK(crossing(first, twice, 1000) == std::nullopt);
  // Where it ends below the first, or starts above it: none either.
  CHECK(crossing(first, [](double) { return 0.5; }, 1000) == std::nullopt);
  CHECK(crossing(first, [](double x) { return 2 * x; }, 1000) == std::nullopt);
}

TEST_CASE("search: GROUP BY streams groups in an index's order, or holds them in a structure, by their costs",
          "[sql][search][cost][group]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"g", {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"g_a", "g", {{"a"}}});
  const auto& cat = f.cat();
  // 1,000 rows. g_a's full scan gives a's groups, covering: 1,000 entries,
  // each group given as the next starts, in locals. The structure over the
  // same scan costs 1,000, each row held at 1 + 0.3, and a step per group,
  // 100 of them at 1/10 per key.
  auto streamed = plan_sql(cat, "SELECT a, count(*) FROM g GROUP BY a", {{"g", 1000}});
  CHECK(has(streamed, "bool st0 = false;"));
  CHECK(has(streamed, "bc_key_part_i64"));
  CHECK_FALSE(has(streamed, "bc_agg_open"));
  CHECK(streamed.cost == Catch::Approx(1000));
  // k < $1 keeps a third of the rows, by the primary key; g_a's order would
  // fetch every row for the filter, 2 x 1,000. The structure costs less.
  auto held = plan_sql(cat, "SELECT a, count(*) FROM g WHERE k < $1 GROUP BY a", {{"g", 1000}});
  CHECK(has(held, "bc_agg a0 = bc_agg_open(cx, 2);"));
  CHECK(has(held, "while (bc_agg_next(cx, a0, &gk0, 0)) {"));
  CHECK_FALSE(has(held, "bool st0"));
  CHECK(held.cost == Catch::Approx(1000.0 / 3 + 1000.0 / 3 * (1 + kHashWeight) + 1000.0 / 30));
  // ORDER BY a: the stream's order gives it; ORDER BY a DESC too, read in
  // reverse, its NULLs first. ORDER BY count(*) is not in the dialect.
  CHECK_FALSE(has(plan_sql(cat, "SELECT a, count(*) FROM g GROUP BY a ORDER BY a NULLS FIRST", {{"g", 1000}}), "bc_sorter"));
  auto desc = plan_sql(cat, "SELECT a, count(*) FROM g GROUP BY a ORDER BY a DESC", {{"g", 1000}});
  CHECK(has(desc, "BC_SCAN_REVERSE"));
  CHECK_FALSE(has(desc, "bc_sorter"));
  // The structure's groups come in the order of their first rows, so a sort
  // of them where the plan's order is not the ORDER BY's, and a LIMIT that
  // counts the sorted groups. Under LIMIT 2 the stream in g_a's reverse
  // order stops after 2 of its 100 groups, and costs less; under LIMIT 50,
  // 1,000, more than the structure and its sort.
  CHECK(has(plan_sql(cat, "SELECT a, count(*) FROM g WHERE k < $1 GROUP BY a ORDER BY a DESC LIMIT 2", {{"g", 1000}}),
            "bool st0 = false;"));
  auto sorted = plan_sql(cat, "SELECT a, count(*) FROM g WHERE k < $1 GROUP BY a ORDER BY a DESC LIMIT 50", {{"g", 1000}});
  CHECK(has(sorted, "bc_sorter s0 = bc_sorter_open(cx, 50);"));
  CHECK(has(sorted, "bc_sorter_add(cx, s0, bc_sort_key_end(cx, sk), gk0);"));
  CHECK(has(sorted, "while (n < 50 && bc_sorter_next(cx, s0, &p0)) {"));
  CHECK(sorted.cost == Catch::Approx(800 + 1000.0 / 30 * (1 + kSortWeight)));
  CHECK(has(sorted, "bc_group g0 = bc_agg_group(cx, a0, p0);"));
  // A key a WHERE equality makes constant leaves the rest to group by: a
  // prefix of the primary key streams by the part after it.
  auto prefix = plan_sql(cat, "SELECT k, a, count(*) FROM g WHERE a = $1 GROUP BY k, a", {{"g", 1000}});
  CHECK(has(prefix, "bool st0 = false;"));
  // With no GROUP BY, one group, given over no rows too.
  auto one = plan_sql(cat, "SELECT count(*), max(v) FROM g WHERE k < $1", {{"g", 1000}});
  CHECK_FALSE(has(one, "bc_agg_open"));
  CHECK(has(one, "if (!xs0_1) {"));
}

TEST_CASE("search: a structure of groups that may pass the memory budget is a choice point, or left out",
          "[sql][search][cost][group][memory][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"g", {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"g_a", "g", {{"a"}}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT a, count(*) FROM g WHERE k < $1 GROUP BY a";
  RowCounts counts = [](const Table&) -> std::optional<std::uint64_t> { return 1000; };
  auto plan = [&](std::uint64_t budget) {
    return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts, budget);
  };
  // An aggregate of n groups of 2 slots holds at least 320 + 2 + n x (136 +
  // 2 x 48) bytes: past 10,000 from 42 groups. No more groups than the rows
  // k's range holds, counted when the plan runs: below 42 the structure,
  // from it the stream in g_a's order, which holds nothing.
  auto choice = plan(10'000);
  CHECK(threshold(choice) == 42);
  CHECK(has(choice, ", lo0, hi0, 42);"));
  const auto other = choice.source.find("} else {");
  REQUIRE(other != std::string::npos);
  CHECK(choice.source.find("bc_agg_open") < other);
  CHECK(choice.source.find("bool st1 = false;", other) != std::string::npos);
  CHECK(choice.cost == Catch::Approx(1000.0 / 3 + 1000.0 / 3 * (1 + kHashWeight) + 1000.0 / 30 + 2));
  // Under the empty aggregate's bytes nothing fits: the stream.
  auto avoided = plan(300);
  CHECK_FALSE(has(avoided, "bc_agg_open"));
  CHECK(has(avoided, "bool st0 = false;"));
  // With no budget, the structure alone.
  auto unbounded = plan(0);
  CHECK(has(unbounded, "bc_agg_open"));
  CHECK_FALSE(has(unbounded, "bc_count_range"));
  // Where no plan streams, the structure runs, and past the budget fails
  // with 54000: grouped by v, which no index orders.
  const std::string by_v = "SELECT v, count(*) FROM g WHERE k < $1 GROUP BY v";
  auto alone = generate_plan(bound::bind(ast::parse(by_v), cat), cat, by_v, counts, 10'000);
  CHECK(has(alone, "bc_agg_open"));
  CHECK_FALSE(has(alone, "bc_count_range"));
}

TEST_CASE("search: a grouped query over a hash join takes the build's choice by the budget, as the join alone does",
          "[sql][search][cost][group][hash][memory][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"t0", {Fixture::col("k", PhysType::I64, false), Fixture::col("c1", PhysType::I64, true),
                              Fixture::col("c2", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateTableDdl{"t1", {Fixture::col("k", PhysType::I64, false), Fixture::col("c1", PhysType::I64, true)}, {"k"}});
  const auto& cat = f.cat();
  RowCounts counts = [](const Table& t) -> std::optional<std::uint64_t> { return t.name == "t0" ? 40 : 100; };
  auto plan = [&](const std::string& sql, std::uint64_t budget) {
    return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts, budget);
  };
  const std::string joined = "SELECT t0.c2 FROM t0 JOIN t1 ON t0.c1 = t1.c1";
  const std::string grouped = "SELECT t0.c2, count(*) FROM t0 JOIN t1 ON t0.c1 = t1.c1 GROUP BY t0.c2";
  // Built on t0's whole table, counted when the plan runs: below the count
  // the hash join, from it t1 read once per row of t0. Grouped, the same
  // choice, inside the structure's input.
  const auto alone = threshold(plan(joined, 10'000));
  REQUIRE(alone > 0);
  auto g = plan(grouped, 10'000);
  CHECK(threshold(g) == alone);
  const auto below = g.source.find("if (count0 < ");
  const auto other = g.source.find("} else {", below);
  REQUIRE(other != std::string::npos);
  CHECK(g.source.find("bc_agg_open") < below);
  CHECK(g.source.find("bc_hash_open", below) < other);
  CHECK(g.source.find("bc_hash_open", other) == std::string::npos);
  // Under 192 bytes no hash table fits: left out, grouped or not.
  CHECK_FALSE(has(plan(joined, 100), "bc_hash"));
  CHECK_FALSE(has(plan(grouped, 100), "bc_hash"));
  // With no budget, the hash join, and no count.
  auto unbounded = plan(grouped, 0);
  CHECK(has(unbounded, "bc_hash_open"));
  CHECK_FALSE(has(unbounded, "bc_count_range"));
}

TEST_CASE("search: a choice point counts a NULL split of no prefix as its whole index, a hash join's build among them",
          "[sql][search][hash][memory][choice]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"t0", {Fixture::col("k", PhysType::I64, false), Fixture::col("c1", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateTableDdl{"t1", {Fixture::col("k", PhysType::I64, false), Fixture::col("c1", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"t1_c1", "t1", {{"c1"}}});
  const auto& cat = f.cat();
  RowCounts counts = [](const Table& t) -> std::optional<std::uint64_t> { return t.name == "t0" ? 40 : 100; };
  // t1_c1 in ORDER BY c1's order, NULLs last: its present values, then its
  // NULLs. A hash join built on it counts the whole index, every plan
  // passing check.
  const std::string sql = "SELECT t1.k FROM t0, t1 WHERE t1.c1 = t0.c1 ORDER BY t1.c1";
  std::vector<GeneratedPlan> all;
  REQUIRE_NOTHROW(all = generate_alternatives(bound::bind(ast::parse(sql), cat), cat, sql, counts, 10'000));
  auto counted = std::ranges::any_of(all, [](const GeneratedPlan& g) {
    return has(g, "bc_view lo0 = bc_empty(cx);\n  bc_view hi0 = bc_empty(cx);\n  int64_t count0 = bc_count_range(cx, IDX_") &&
           has(g, "built on TBL_");
  });
  CHECK(counted);
}

TEST_CASE("search: a column the GROUP BY's primary key determines is read from the group, never a key",
          "[sql][search][group]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"g", {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"g_a", "g", {{"a"}}});
  const auto& cat = f.cat();
  // Grouped by k, the primary key: v is the group's, and the primary key's
  // order streams the groups, which a GROUP BY of k and v would not.
  auto by_k = plan_sql(cat, "SELECT k, v, count(*) FROM g GROUP BY k", {{"g", 1000}});
  CHECK(has(by_k, "bool st0 = false;"));
  CHECK_FALSE(has(by_k, "bc_agg_open"));
  CHECK(by_k.cost == Catch::Approx(1000));
  // In a structure, an integer is a slot set on the group's first row,
  // and the group's key is k's alone.
  auto held = plan_sql(cat, "SELECT k, v, count(*) FROM g WHERE a < $1 GROUP BY k ORDER BY v", {{"g", 1000}});
  CHECK(has(held, "bc_agg a0 = bc_agg_open(cx, 3);"));
  CHECK(has(held, "bc_agg_min_i64(cx, gg0, 2, bc_col_i64(cx, row, COL_"));
  std::size_t parts = 0;
  for (auto at = held.source.find("(cx, gb0, "); at != std::string::npos; at = held.source.find("(cx, gb0, ", at + 1)) ++parts;
  CHECK(parts == 1);
}

TEST_CASE("search: a stream reads a BOOLEAN key from a covering entry; a structure reads it from a row it holds",
          "[sql][search][group]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"b", {Fixture::col("k", PhysType::I64, false), Fixture::col("f", PhysType::Bool, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"b_f", "b", {{"f"}}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT f, count(*) FROM b GROUP BY f";
  RowCounts counts = [](const Table&) -> std::optional<std::uint64_t> { return 1000; };
  auto streamed = plan_sql(cat, sql, {{"b", 1000}});
  CHECK(has(streamed, "bc_key_part_bool"));
  CHECK_FALSE(has(streamed, "bc_scan_row"));
  CHECK(streamed.cost == Catch::Approx(1000));
  // Every plan passes check: the structure over the same scan reads rows.
  for (const auto& g : generate_alternatives(bound::bind(ast::parse(sql), cat), cat, sql, counts, 0)) {
    if (has(g, "bc_agg_open")) CHECK(has(g, "bc_scan_row"));
  }
}

TEST_CASE("search: with no GROUP BY, no path is built for the groups; the groups count only non-constant keys",
          "[sql][search][cost][group]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"g", {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"g_a", "g", {{"a"}}});
  const auto& cat = f.cat();
  RowCounts counts = [](const Table&) -> std::optional<std::uint64_t> { return 1000; };
  auto all = [&](const std::string& sql) {
    return generate_alternatives(bound::bind(ast::parse(sql), cat), cat, sql, counts, 0);
  };
  // One group of every row: g_a's full scan, which only an ORDER BY or a
  // GROUP BY builds, is not a path.
  for (const auto& g : all("SELECT count(*), max(a) FROM g")) CHECK_FALSE(has(g, "IDX_"));
  // a = $1 makes a constant: one group. Its structure over g_a's range,
  // covering, 100 entries: 100 + 100 x 1.3 + 1, not + 10.
  auto least = std::numeric_limits<double>::max();
  for (const auto& g : all("SELECT a, count(*) FROM g WHERE a = $1 GROUP BY a")) {
    if (has(g, "bc_agg_open")) least = std::min(least, g.cost);
  }
  CHECK(least == Catch::Approx(100 + 100 * (1 + kHashWeight) + 1));
}

TEST_CASE("search: a structure's threshold counts its keys', held rows' and distinct values' bytes",
          "[sql][search][group][memory]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"g", {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"g_a", "g", {{"a"}}});
  const auto& cat = f.cat();
  RowCounts counts = [](const Table&) -> std::optional<std::uint64_t> { return 1000; };
  auto plan = [&](const std::string& sql) {
    return generate_plan(bound::bind(ast::parse(sql), cat), cat, sql, counts, 10'000);
  };
  // 3 slots, one COUNT(DISTINCT): 320 + 3 + 321 bytes, and per row a group,
  // its key a's NULL marker at least, 1 + 136 + 3 x 48, and a value new to
  // it, its key the group's (1 + 1) and v's (9), 136 + 48: past 10,000 from
  // (10,000 - 644) / 476 + 1 = 20 rows. The fixed amounts alone would give 34.
  CHECK(threshold(plan("SELECT a, count(DISTINCT v) FROM g WHERE k < $1 GROUP BY a")) == 20);
}

TEST_CASE("search: COUNT(*), MIN and MAX of one group are answered from the range's keys alone",
          "[sql][search][group]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"g", {Fixture::col("k", PhysType::I64, false), Fixture::col("a", PhysType::I64, true),
                             Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateIndexDdl{"g_a", "g", {{"a"}}});
  const auto& cat = f.cat();
  auto plan = [&](const std::string& sql) { return plan_sql(cat, sql, {{"g", 1000}}); };
  // MAX the last entry, read as a reverse scan's first; with COUNT(*) and
  // MIN, a count and two steps, never a loop over the range.
  for (const auto* sql : {"SELECT max(a) FROM g WHERE a > $1", "SELECT count(*), min(a), max(a) FROM g WHERE a > $1",
                          "SELECT max(k), min(k), count(*) FROM g WHERE k >= $1"}) {
    INFO(sql);
    auto g = plan(sql);
    CHECK(has(g, "BC_SCAN_REVERSE"));
    CHECK_FALSE(has(g, "while ("));
  }
  CHECK(has(plan("SELECT count(*), min(a), max(a) FROM g WHERE a > $1"), "bc_count_range"));
  // A lone MIN and a lone MAX take one route: each read into a local from
  // its first entry, MIN forward and MAX in reverse, and over every row
  // otherwise, as a group of every row.
  for (const auto* sql : {"SELECT min(a) FROM g WHERE a > $1", "SELECT max(a) FROM g WHERE a > $1"}) {
    INFO(sql);
    auto g = plan(sql);
    CHECK(has(g, "kv0 = bc_key_part_i64("));
    CHECK(has(g, "BC_SCAN_REVERSE") == (std::string_view{sql}.find("max") != std::string_view::npos));
    CHECK_FALSE(has(g, "while ("));
  }
  for (const auto* sql : {"SELECT min(v) FROM g WHERE a > $1", "SELECT max(v) FROM g WHERE a > $1"}) {
    INFO(sql);
    auto g = plan(sql);
    CHECK(has(g, "x0_0"));
    CHECK(has(g, "while ("));
  }
  // A filter the range does not take, an aggregate other than these, a
  // second column, a GROUP BY or a HAVING: every row is read.
  for (const auto* sql : {"SELECT max(a) FROM g WHERE a > $1 AND v = $2", "SELECT max(a), count(v) FROM g WHERE a > $1",
                          "SELECT max(a), sum(a) FROM g WHERE a > $1", "SELECT max(a), min(k) FROM g WHERE a > $1",
                          "SELECT a, max(a) FROM g WHERE a > $1 GROUP BY a",
                          "SELECT max(a) FROM g WHERE a > $1 HAVING count(*) > $2"}) {
    INFO(sql);
    CHECK(has(plan(sql), "while ("));
  }
}

namespace {

// How many times `needle` is in `text`.
auto occurrences(const std::string& text, std::string_view needle) -> std::size_t {
  std::size_t n = 0;
  for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

}  // namespace

TEST_CASE("search: a join of three tables is planned in any order the costs choose, each table by its own access",
          "[sql][search][cost][join]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateTableDdl{"w", {Fixture::col("j", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"j"}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT t.id, u.k, w.j FROM t, u, w WHERE t.name = $1 AND u.k = t.qty AND w.j = u.v";
  // t by its index on name, then u and w each by the whole key from the row before it: three loops' worth of
  // lookups, one loop.
  auto keyed = plan_sql(cat, sql, {{"t", 100}, {"u", 100}, {"w", 100}});
  CHECK(keyed.source.find("bc_hash") == std::string::npos);
  CHECK(occurrences(keyed.source, "bc_scan_open(") == 1);
  CHECK(has(keyed, "TBL_4"));
  CHECK(has(keyed, "TBL_6"));
  // Every plan the search costed reads the three tables, and none holds two hash tables.
  const std::string by_columns = "SELECT t.id, u.k, w.j FROM t, u, w WHERE t.qty = u.v AND u.v = w.v";
  auto b = bound::bind(ast::parse(by_columns), cat);
  auto all = generate_alternatives(std::move(b), cat, by_columns, [](const Table&) -> std::optional<std::uint64_t> { return 1000; });
  REQUIRE(all.size() > 3);
  for (const auto& g : all) {
    INFO(g.source);
    CHECK(occurrences(g.source, "bc_hash_open(") <= 1);
    CHECK(has(g, "TBL_1"));
    CHECK(has(g, "TBL_4"));
    CHECK(has(g, "TBL_6"));
  }
  // One of them hashes a table and reads the third once per row of the pairs.
  CHECK(std::ranges::any_of(all, [](const GeneratedPlan& g) { return occurrences(g.source, "bc_hash_open(") == 1; }));
}

TEST_CASE("search: a chain of left joins is planned in FROM order, each right table under the rows before it",
          "[sql][search][join][outer]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  f.ddl(CreateTableDdl{"w", {Fixture::col("j", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"j"}});
  const auto& cat = f.cat();
  const std::string sql = "SELECT t.id, u.k, w.j FROM t LEFT JOIN u ON u.k = t.qty LEFT JOIN w ON w.j = u.v";
  // Every plan the search costed has the same order, and a row of NULLs for each right table.
  auto b = bound::bind(ast::parse(sql), cat);
  auto all = generate_alternatives(std::move(b), cat, sql, [](const Table&) -> std::optional<std::uint64_t> { return 1000; });
  REQUIRE_FALSE(all.empty());
  for (const auto& g : all) {
    INFO(g.source);
    CHECK(occurrences(g.source, "bc_null_row(") == 2);
    CHECK(g.source.find("bc_hash") == std::string::npos);  // a left join's right side is never hashed
    CHECK(g.source.find("COL_1_0") < g.source.find("COL_4_0"));
    CHECK(g.source.find("COL_4_0") < g.source.find("COL_6_0"));
  }
  // u is a primary key looked up from t, and w from u: a key from a row the first join may have NULL-extended
  // is a miss, tested before it is read.
  const auto& plan = all[0].source;
  CHECK(has(all[0], "bc_col_null(cx, row1, COL_4_1)"));
  // An inner join after an outer one is not outer: no row of NULLs for it.
  const std::string inner = "SELECT t.id, u.k, w.j FROM t LEFT JOIN u ON u.k = t.qty JOIN w ON w.j = u.v";
  auto mixed = plan_sql(cat, inner, {{"t", 1000}, {"u", 1000}, {"w", 1000}});
  CHECK(occurrences(mixed.source, "bc_null_row(") == 1);
  (void)plan;
}

TEST_CASE("search: a predicate over the columns of two tables filters the pairs where the last of them is read",
          "[sql][search][join]") {
  sql_test::Fixture f;
  using sql_test::Fixture;
  f.ddl(CreateTableDdl{"u", {Fixture::col("k", PhysType::I64, false), Fixture::col("v", PhysType::I64, true)}, {"k"}});
  const auto& cat = f.cat();
  // NOT (a = b) over t and u: not a key of either, so u is read once per row of t, the pair tested.
  auto g = plan_sql(cat, "SELECT t.id, u.k FROM t JOIN u ON NOT (t.qty = u.v) WHERE t.name = $1");
  CHECK(has(g, "bc_col_i64(cx, row, COL_1_2)"));
  CHECK(has(g, "bc_col_i64(cx, row1, COL_4_1)"));
  CHECK(has(g, "bc_emit_col(cx, row1, COL_4_0);"));
  // In an outer join's ON, the pair is a match: no pair, a row of NULLs.
  auto outer = plan_sql(cat, "SELECT t.id, u.k FROM t LEFT JOIN u ON u.k = t.qty AND (t.id < u.v OR u.v IS NULL)");
  CHECK(has(outer, "bc_null_row("));
  // A predicate over a DOUBLE column is refused, as any filter on one is.
  f.ddl(CreateTableDdl{"d", {Fixture::col("k", PhysType::I64, false), Fixture::col("x", PhysType::F64, true)}, {"k"}});
  f.ddl(CreateTableDdl{"e", {Fixture::col("k", PhysType::I64, false), Fixture::col("y", PhysType::F64, true)}, {"k"}});
  auto refused = [&](std::string_view sql) {
    try {
      (void)plan_sql(f.cat(), sql);
    } catch (const ast::FeatureNotSupported& e) {
      return std::string{e.what()};
    }
    return std::string{"accepted"};
  };
  CHECK(refused("SELECT d.k FROM d JOIN e ON NOT (d.x = e.y)").starts_with("no plan: a filter on column"));
}

TEST_CASE("search: a table read twice has an instance for each use, and the plan names the table's ids once",
          "[sql][search][join]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  auto g = plan_sql(cat, "SELECT x.id, y.id FROM t x JOIN t y ON x.qty < y.id WHERE x.name = $1");
  INFO(g.source);
  // x is read by the table's index on name, y by a scan of the table's rows; the constants of each are named by
  // its own id and stand for the one table's.
  CHECK(has(g, "COL_1073741824_0 = 0"));
  CHECK(has(g, "COL_1073741824_2 = 2"));
  CHECK(has(g, "TBL_1073741825 = 1"));
  CHECK(has(g, "COL_1073741825_0 = 0"));
  // The index is the table's, whichever use reads it.
  CHECK(occurrences(g.source, "IDX_3 = 3") == 1);
  // The header says whose rows they are: the instance is named as the query names it.
  CHECK(has(g, "TBL_1073741825 y"));
  CHECK(has(g, "COL_1073741824_0 x.id"));
}
