// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// physical::check refuses each plan the types allow but codegen can't
// print: a compiler bug, raised as std::logic_error, never a plan.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.physical;

using namespace bytecask::sql;
using physical::node;

namespace {

auto plan(physical::NodePtr root, std::vector<std::uint32_t> null_checks = {0, 1}, std::size_t params = 2)
    -> physical::Plan {
  physical::Plan p;
  p.root = std::move(root);
  p.constants = {{"TBL_1", 1, "t"}};
  p.catalog_version = 1;
  p.param_types = std::vector<PhysType>(params, PhysType::I64);
  p.null_checks = std::move(null_checks);
  return p;
}

auto p(std::uint32_t n) -> logical::Expr { return logical::param(n, SqlType{SqlKind::BigInt}); }

const logical::ColumnRef kId{1, 0, PhysType::I64, false};
const logical::ColumnRef kD{1, 1, PhysType::F64, true};

auto get(std::vector<logical::Expr> key, bool for_update = false) -> physical::NodePtr {
  return node(physical::PointGet{1, std::move(key), for_update});
}

// A forward scan of index 3.
auto scan(std::vector<logical::Expr> prefix, std::variant<std::monostate, physical::Range> rest, bool covering = false,
          bool for_update = false) -> physical::Scan {
  return {1, 3, std::move(prefix), std::move(rest), false, covering, for_update};
}

auto emit(physical::NodePtr in, std::vector<logical::ColumnRef> targets = {kId}) -> physical::NodePtr {
  return node(physical::Emit{std::move(in), std::move(targets)});
}

auto value(logical::Expr e) -> physical::KeyBound { return {physical::KeyBound::Kind::Value, std::move(e), false}; }

// The rule check names, or "" if it accepts the plan.
auto refusal(const physical::Plan& plan) -> std::string {
  try {
    physical::check(plan);
  } catch (const std::logic_error& e) {
    return e.what();
  }
  return "";
}

void refused(const physical::Plan& plan, const std::string& rule) {
  auto why = refusal(plan);
  INFO(why);
  CHECK(why.find(rule) != std::string::npos);
}

}  // namespace

TEST_CASE("physical: check refuses what codegen can't print", "[sql][physical]") {
  CHECK(refusal(plan(emit(get({p(0)})))).empty());
  CHECK(refusal(plan(emit(get({p(0)}), {}))).empty());
  refused(plan(emit(get({}))), "no key part");
  refused(plan(emit(get({p(2)}))), "past the statement's");
  refused(plan(emit(get({p(1)})), {0}), "does not check for NULL");

  refused(plan(emit(node(scan({}, {})))), "an equality prefix or a range");
  refused(plan(emit(node(scan({p(0)}, {}))), {}), "does not check for NULL");
  CHECK(refusal(plan(emit(node(scan({}, physical::Range{value(p(0)), {}}))), {0})).empty());
  CHECK(refusal(plan(emit(node(scan({p(0)}, physical::Range{value(p(1)), {}}))))).empty());
  refused(plan(emit(node(scan({}, physical::Range{{}, value(p(2))}))), {0}), "past the statement's");
  refused(plan(emit(node(scan({}, physical::Range{value(p(0)), {}}))), {}), "does not check for NULL");
  refused(plan(emit(node(scan({}, physical::Range{value(p(0)), {}}))), {0, 2}), "past the statement's");
  // $n ± k is computed once, before anything is read.
  auto sum = logical::binary(logical::Expr::Kind::Sub, p(0), logical::constant(20, SqlType{SqlKind::Integer}),
                             SqlType{SqlKind::Integer});
  auto by_sum = plan(emit(node(scan({}, physical::Range{value(sum), {}}))), {0});
  refused(by_sum, "does not compute");
  by_sum.computed.push_back({0, -20, 1});
  CHECK(refusal(by_sum).empty());

  CHECK(refusal(plan(emit(node(scan({p(0)}, {}, true))))).empty());
  refused(plan(emit(node(scan({p(0)}, {}, true)), {kD})), "DOUBLE column read from a key");
  // A full scan, of the primary key or of an index.
  CHECK(refusal(plan(emit(node(physical::Scan{1, 1, {}, physical::Range{}, false, false, false})), {})).empty());
  CHECK(refusal(plan(emit(node(scan({}, physical::Range{}))), {})).empty());
  refused(plan(emit(node(scan({p(0)}, {}, true, true)))), "FOR UPDATE scan that covers");
  CHECK(refusal(plan(emit(node(scan({p(0)}, {}, false, true))))).empty());
  // The NULL split's values start past the NULL part.
  refused(plan(emit(node(physical::NullSplitScan{scan({p(0)}, {})}))), "past the NULL part");
  auto past_nulls = physical::Range{{physical::KeyBound::Kind::NullPart, std::nullopt, true}, {}};
  CHECK(refusal(plan(emit(node(physical::NullSplitScan{scan({p(0)}, past_nulls)})))).empty());
  auto reverse = scan({p(0)}, past_nulls);
  reverse.reverse = true;
  // Aggregates from the keys: COUNT(*) of the range, MIN and MAX of the
  // one part after its prefix, from a forward scan past its NULLs.
  auto call = [](logical::Expr::Fn fn, std::optional<logical::ColumnRef> arg) {
    auto e = logical::expr(logical::Expr::Kind::Aggregate, SqlType{SqlKind::BigInt});
    e.fn = fn;
    if (arg) e.args.push_back(logical::column(*arg, SqlType{SqlKind::BigInt}));
    return e;
  };
  const auto count = call(logical::Expr::Fn::Count, std::nullopt);
  const auto min_id = call(logical::Expr::Fn::Min, kId);
  const auto max_id = call(logical::Expr::Fn::Max, kId);
  const auto counted = scan({p(0)}, {});
  const auto forward = scan({p(0)}, past_nulls);
  CHECK(refusal(plan(node(physical::KeyAggregates{counted, forward, kId, {count, max_id, min_id}, true}))).empty());
  CHECK(refusal(plan(node(physical::KeyAggregates{counted, counted, std::nullopt, {count}, true}))).empty());
  refused(plan(node(physical::KeyAggregates{counted, reverse, kId, {min_id}, true})), "reverse scan");
  refused(plan(node(physical::KeyAggregates{counted, forward, std::nullopt, {max_id}, true})), "MIN or MAX of its part");
  refused(plan(node(physical::KeyAggregates{counted, forward, kD, {max_id}, true})), "MIN or MAX of its part");
  auto other_range = scan({p(0), p(1)}, past_nulls);
  refused(plan(node(physical::KeyAggregates{counted, other_range, kId, {count, min_id}, true})), "another range");
  refused(plan(node(physical::Limit{get({p(0)}), 1})), "LIMIT not under");
  CHECK(refusal(plan(emit(node(physical::Limit{get({p(0)}), 1})))).empty());

  refused(plan(node(physical::Insert{1, {}})), "sets no column");
  refused(plan(node(physical::Update{get({p(0)}), 1, {}})), "sets no column");
  auto d_minus = logical::binary(logical::Expr::Kind::Sub, logical::column(kD, SqlType{SqlKind::Double}), p(1),
                                 SqlType{SqlKind::Double});
  refused(plan(node(physical::Update{get({p(0)}), 1, {{kD, d_minus}}})), "other than an integer");
  refused(plan(node(physical::Update{get({p(0)}), 1, {{kId, logical::column(kD, SqlType{SqlKind::Double})}}})),
          "of another type");
  refused(plan(node(physical::Delete{get({}), 1})), "no key part");
  auto no_constants = plan(node(physical::Delete{get({p(0)}), 1}));
  no_constants.constants.clear();
  refused(no_constants, "no constants");
}

TEST_CASE("physical: a nested loop reads its inner scan once, under one outer row", "[sql][physical]") {
  const logical::ColumnRef inner_col{2, 1, PhysType::I64, true};
  const logical::ColumnRef other{3, 0, PhysType::I64, false};
  auto col = [](const logical::ColumnRef& c) { return logical::column(c, SqlType{SqlKind::BigInt}); };
  auto inner = [] { return node(physical::Scan{2, 4, {p(1)}, {}, false, false, false}); };
  auto loop = [](physical::NodePtr outer, physical::NodePtr in) {
    return emit(node(physical::NestedLoop{std::move(outer), std::move(in)}));
  };
  // The inner scan under a join conjunct: its column, then one of `c`'s.
  auto joined = [&](const logical::ColumnRef& c) {
    return node(physical::Filter{inner(), {logical::compare(ast::Compare::Op::Eq, col(inner_col), col(c))}});
  };
  CHECK(refusal(plan(loop(get({p(0)}), inner()))).empty());
  // Under an outer loop, read once per outer row: never with a count of its own.
  CHECK(refusal(plan(loop(node(scan({p(0)}, {})), inner()))).empty());
  auto counted = [&] {
    return node(physical::Choice{physical::Scan{2, 4, {p(1)}, {}, false, false, false}, 10, inner(), inner()});
  };
  CHECK(refusal(plan(loop(get({p(0)}), counted()))).empty());
  refused(plan(loop(node(scan({p(0)}, {})), counted())), "a choice point read once per outer row");
  refused(plan(loop(node(physical::Insert{1, {{kId, p(0)}}}), inner())), "neither one row nor a loop");
  refused(plan(loop(get({p(0)}), get({p(1)}))), "inner side is not a scan");
  // A join conjunct reads a column of the outer side: of its point lookup,
  // or of a table it looks up by key.
  CHECK(refusal(plan(loop(get({p(0)}), joined(kId)))).empty());
  refused(plan(loop(get({p(0)}), joined(other))), "reads a column outside its outer side");
  CHECK(refusal(plan(loop(node(physical::IndexJoin{get({p(0)}), 3, {p(0)}}), joined(other)))).empty());
  // Its first column is the inner table's, read from the inner scan's row:
  // never another table's, and never over a scan that covers.
  auto outer_first = node(physical::Filter{inner(), {logical::compare(ast::Compare::Op::Eq, col(kId), col(inner_col))}});
  refused(plan(loop(get({p(0)}), std::move(outer_first))), "compares a column outside its inner side");
  auto covered = node(physical::Filter{node(physical::Scan{2, 4, {p(1)}, {}, false, true, false}),
                                       {logical::compare(ast::Compare::Op::Eq, col(inner_col), col(kId))}});
  refused(plan(loop(get({p(0)}), std::move(covered))), "reads the row of a covering scan");
}

TEST_CASE("physical: a choice point counts its first plan's range, and each plan is a scan of its table",
          "[sql][physical][choice]") {
  // Of index 3 by p0, or of index 5 by p1, each a scan of table 1.
  auto by = [](std::uint32_t object, std::uint32_t param) {
    return physical::Scan{1, object, {p(param)}, {}, false, false, false};
  };
  auto choice = [&](physical::Scan counted, physical::NodePtr below, physical::NodePtr at_or_above, std::int64_t t) {
    return emit(node(physical::Choice{std::move(counted), t, std::move(below), std::move(at_or_above)}));
  };
  CHECK(refusal(plan(choice(by(3, 0), node(by(3, 0)), node(by(5, 1)), 100))).empty());
  // Its thresholds overridden, as alternatives agree runs each plan.
  CHECK(refusal(plan(choice(by(3, 0), node(by(3, 0)), node(by(5, 1)), 0))).empty());
  refused(plan(choice(by(3, 0), node(by(3, 0)), node(by(5, 1)), -1)), "negative threshold");
  refused(plan(choice(by(5, 1), node(by(3, 0)), node(by(5, 1)), 100)), "counts another range");
  refused(plan(choice(by(3, 0), node(by(3, 0)), get({p(0)}), 100)), "plan is not a scan");
  auto other_table = by(5, 1);
  other_table.table = 2;
  refused(plan(choice(by(3, 0), node(by(3, 0)), node(other_table), 100)), "read another table");
  refused(plan(choice(by(3, 0), node(by(3, 0)), node(by(5, 2)), 100)), "past the statement's");
  // A DOUBLE target from a key, on either plan.
  auto covering = by(5, 1);
  covering.covering = true;
  refused(plan(emit(node(physical::Choice{by(3, 0), 100, node(by(3, 0)), node(covering)}), {kD})),
          "DOUBLE column read from a key");

  // The count is of the first plan's whole range: its prefix's parameters,
  // its bounds and its direction.
  auto ranged = [&](logical::Expr lo, bool successor, bool reverse) {
    auto s = physical::Scan{1, 3, {p(0)}, physical::Range{{physical::KeyBound::Kind::Value, std::move(lo), successor}, {}},
                            reverse, false, false};
    return s;
  };
  CHECK(refusal(plan(choice(ranged(p(1), false, false), node(ranged(p(1), false, false)), node(by(5, 1)), 9))).empty());
  refused(plan(choice(ranged(p(0), false, false), node(ranged(p(1), false, false)), node(by(5, 1)), 9)),
          "counts another range");
  refused(plan(choice(ranged(p(1), true, false), node(ranged(p(1), false, false)), node(by(5, 1)), 9)),
          "counts another range");
  refused(plan(choice(ranged(p(1), false, true), node(ranged(p(1), false, false)), node(by(5, 1)), 9)),
          "counts another range");
  refused(plan(choice(by(3, 1), node(by(3, 0)), node(by(5, 1)), 9)), "counts another range");
  refused(plan(choice(ranged(p(1), false, false), node(by(3, 0)), node(by(5, 1)), 9)), "counts another range");
  // A NULL split's count is of its whole prefix, which holds both its ranges.
  auto past_nulls = physical::Range{{physical::KeyBound::Kind::NullPart, std::nullopt, true}, {}};
  auto split = [&] { return node(physical::NullSplitScan{physical::Scan{1, 3, {p(0)}, past_nulls, false, false, false}}); };
  CHECK(refusal(plan(choice(by(3, 0), split(), node(by(5, 1)), 9))).empty());
  refused(plan(choice(physical::Scan{1, 3, {p(0)}, past_nulls, false, false, false}, split(), node(by(5, 1)), 9)),
          "counts another range");

  physical::Plan two = plan(choice(by(3, 0), node(by(3, 0)), node(by(5, 1)), 100));
  CHECK(physical::choices(two) == 1);
  physical::override_thresholds(two, {0});
  CHECK(std::get<physical::Choice>(std::get<physical::Emit>(two.root->op).input->op).threshold == 0);
  CHECK_THROWS_AS(physical::override_thresholds(two, {0, 0}), std::logic_error);
}

TEST_CASE("physical: a DOUBLE column is never read from a key, wherever codegen reads it", "[sql][physical]") {
  const logical::ColumnRef other{2, 0, PhysType::I64, false};
  auto col = [](const logical::ColumnRef& c, SqlKind k = SqlKind::BigInt) { return logical::column(c, SqlType{k}); };
  auto covering = [] { return node(scan({p(0)}, {}, true)); };
  // The inner key of an index join, built from the outer row.
  CHECK(refusal(plan(emit(node(physical::IndexJoin{node(scan({p(0)}, {})), 2, {col(kD, SqlKind::Double)}}), {other})))
            .empty());
  refused(plan(emit(node(physical::IndexJoin{covering(), 2, {col(kD, SqlKind::Double)}}), {other})),
          "DOUBLE column read from a key");
  // A filter's operand, a column of another table.
  auto by_double = logical::compare(ast::Compare::Op::Eq, col(other), col(kD, SqlKind::Double));
  refused(plan(emit(node(physical::Filter{node(physical::IndexJoin{covering(), 2, {p(1)}}), {by_double}}), {other})),
          "DOUBLE column read from a key");
  // An aggregate's argument.
  auto min_d = logical::expr(logical::Expr::Kind::Aggregate, SqlType{SqlKind::Double});
  min_d.fn = logical::Expr::Fn::Min;
  min_d.args.push_back(col(kD, SqlKind::Double));
  CHECK(refusal(plan(node(physical::Aggregate{node(scan({p(0)}, {})), min_d, true}))).empty());
  refused(plan(node(physical::Aggregate{covering(), min_d, true})), "DOUBLE column read from a key");
}

TEST_CASE("physical: a sort holds the row of each of its input's loops read above it, keyed by columns in hand",
          "[sql][physical]") {
  auto by_id = std::vector<physical::SortPart>{{kId, true, true}};
  auto sort = [&](physical::NodePtr in, std::vector<physical::SortPart> keys, std::int64_t keep,
                  std::vector<std::uint32_t> payloads, std::vector<logical::ColumnRef> not_null = {},
                  std::vector<logical::ColumnRef> targets = {kId}) {
    return emit(node(physical::Sort{std::move(in), std::move(keys), keep, std::move(payloads), std::move(not_null)}),
                std::move(targets));
  };
  CHECK(refusal(plan(sort(node(scan({p(0)}, {})), by_id, 0, {1}))).empty());
  CHECK(refusal(plan(sort(node(scan({p(0)}, {})), by_id, 5, {1}))).empty());
  refused(plan(sort(node(scan({p(0)}, {})), {}, 0, {1})), "a sort with no key");
  refused(plan(sort(node(scan({p(0)}, {})), by_id, -1, {1})), "negative number of rows");
  refused(plan(sort(get({p(0)}), by_id, 0, {1})), "sorters hold other rows than its loops'");
  refused(plan(sort(node(scan({p(0)}, {})), by_id, 0, {2}, {}, {})), "sorters hold other rows than its loops'");
  refused(plan(sort(node(scan({p(0)}, {}, true)), by_id, 0, {1})), "a sort over a covering scan");
  // A choice by the budget: the sort in its first plan, a covering scan in
  // the order in its second. Each plan is checked as it runs, so the second's
  // key is no reason to refuse the first's sort; a sort over a covering scan
  // in its own branch still is.
  auto by_budget = [&](bool sort_covers) {
    auto sorted = node(physical::Sort{node(scan({p(0)}, {}, sort_covers)), by_id, 0, {1}, {}});
    auto in_order = node(physical::Scan{1, 5, {p(1)}, {}, false, true, false});
    return emit(node(physical::Choice{scan({p(0)}, {}, sort_covers), 9, std::move(sorted), std::move(in_order)}));
  };
  CHECK(refusal(plan(by_budget(false))).empty());
  refused(plan(by_budget(true)), "a sort over a covering scan");
  refused(plan(sort(node(scan({p(0)}, {})), {{{2, 0, PhysType::I64, false}, false, false}}, 0, {1})),
          "a sort key outside its input's tables");
  // Two loops, one inside the other: a sorter per loop read above the sort,
  // outer first.
  auto two_loops = [](bool inner_covers = false) {
    return node(physical::NestedLoop{node(scan({p(0)}, {})),
                                     node(physical::Scan{2, 4, {p(1)}, {}, false, inner_covers, false})});
  };
  const logical::ColumnRef u_v{2, 1, PhysType::I64, true};
  const logical::ColumnRef u_d{2, 2, PhysType::F64, true};
  CHECK(refusal(plan(sort(two_loops(), by_id, 0, {1, 2}))).empty());
  CHECK(refusal(plan(sort(two_loops(), by_id, 0, {1}))).empty());
  CHECK(refusal(plan(sort(two_loops(), by_id, 0, {2}, {}, {u_v}))).empty());
  refused(plan(sort(two_loops(), by_id, 0, {2})), "a column read above a sort from a loop no sorter holds");
  refused(plan(sort(two_loops(), by_id, 0, {2, 1})), "sorters hold other rows than its loops'");
  // With none read above it, one sorter holds the keys alone.
  CHECK(refusal(plan(sort(two_loops(), by_id, 0, {}, {}, {}))).empty());
  refused(plan(sort(two_loops(), by_id, 0, {})), "a column read above a sort from a loop no sorter holds");
  // A loop no sorter holds may cover: its sort key is built from the entry,
  // never a DOUBLE.
  CHECK(refusal(plan(sort(two_loops(true), {{u_v, false, false}}, 0, {1}))).empty());
  refused(plan(sort(two_loops(true), {{u_d, false, false}}, 0, {1})), "DOUBLE column read from a key");
  // A lookup by key repeated above the sort reads a held row.
  auto again = [&](std::vector<std::uint32_t> payloads) {
    auto sorted = node(physical::Sort{two_loops(), by_id, 0, std::move(payloads), {}});
    return emit(node(physical::IndexJoin{std::move(sorted), 3, {logical::column(u_v, SqlType{SqlKind::BigInt})}}), {});
  };
  CHECK(refusal(plan(again({2}))).empty());
  refused(plan(again({1})), "a column read above a sort from a loop no sorter holds");
  refused(plan(sort(node(physical::NestedLoop{node(scan({p(0)}, {})),
                                              node(physical::Scan{2, 4, {p(1)}, {}, false, true, false})}),
                    by_id, 0, {1, 2})),
          "a sort over a covering scan");
  // Under one row read once, the inner scan's loop.
  auto once = node(physical::NestedLoop{get({p(0)}), node(physical::Scan{2, 4, {p(1)}, {}, false, false, false})});
  CHECK(refusal(plan(sort(std::move(once), by_id, 0, {2}))).empty());
  // A NULL test before a row is added reads a column of its input's tables.
  CHECK(refusal(plan(sort(node(scan({p(0)}, {})), by_id, 0, {1}, {kId}))).empty());
  refused(plan(sort(node(scan({p(0)}, {})), by_id, 0, {1}, {{2, 0, PhysType::I64, true}})),
          "a sort's NULL test outside its input's tables");
}

TEST_CASE("physical: a hash join holds the row of its build side's one loop, keyed by columns of one type",
          "[sql][physical][hash]") {
  // t (table 1) built from its scan of index 3, probed by u's (table 2)
  // scan of index 4.
  const logical::ColumnRef t_v{1, 2, PhysType::I64, true};
  const logical::ColumnRef u_v{2, 1, PhysType::I64, true};
  const logical::ColumnRef u_b{2, 2, PhysType::Bytes, true};
  const logical::ColumnRef u_d{2, 3, PhysType::F64, true};
  auto u_scan = [](bool covering = false) {
    return node(physical::Scan{2, 4, {p(1)}, {}, false, covering, false});
  };
  auto hash = [&](physical::NodePtr build, physical::NodePtr probe, std::vector<physical::HashKey> keys,
                  std::uint32_t held = 1) {
    return emit(node(physical::HashJoin{std::move(build), std::move(probe), std::move(keys), held}), {kId, u_v});
  };
  CHECK(refusal(plan(hash(node(scan({p(0)}, {})), u_scan(), {{t_v, u_v}}))).empty());
  // The probe side's key may come from a covering scan's entry; the build
  // side's row is the one held.
  CHECK(refusal(plan(hash(node(scan({p(0)}, {})), u_scan(true), {{t_v, u_v}}))).empty());
  refused(plan(hash(node(scan({p(0)}, {}, true)), u_scan(), {{t_v, u_v}})), "holds a covering scan's entry");
  refused(plan(hash(node(scan({p(0)}, {})), u_scan(), {})), "with no key");
  refused(plan(hash(node(scan({p(0)}, {})), u_scan(), {{t_v, u_v}}, 2)), "not one loop over the table it holds");
  refused(plan(hash(node(scan({p(0)}, {})), u_scan(), {{u_v, t_v}})), "outside the table held");
  refused(plan(hash(node(scan({p(0)}, {})), u_scan(), {{t_v, t_v}})), "outside the probe side");
  refused(plan(hash(node(scan({p(0)}, {})), u_scan(), {{t_v, u_b}})), "of two types");
  refused(plan(hash(node(scan({p(0)}, {})), u_scan(), {{kD, u_d}})), "on a DOUBLE column");
  // One table read once per row of the other, as a build side: two loops.
  auto twice = node(physical::NestedLoop{node(scan({p(0)}, {})), u_scan()});
  refused(plan(hash(std::move(twice), u_scan(), {{t_v, u_v}})), "not one loop");
  // A choice by the budget counts the build side's range; its other plan
  // reads the same tables.
  auto choice = [&](physical::Scan counted, physical::NodePtr other) {
    auto first = node(physical::HashJoin{node(scan({p(0)}, {})), u_scan(), {{t_v, u_v}}, 1});
    return emit(node(physical::Choice{std::move(counted), 41, std::move(first), std::move(other)}), {kId, u_v});
  };
  auto loop = [&] { return node(physical::NestedLoop{node(scan({p(0)}, {})), u_scan()}); };
  CHECK(refusal(plan(choice(scan({p(0)}, {}), loop()))).empty());
  refused(plan(choice(physical::Scan{2, 4, {p(1)}, {}, false, false, false}, loop())),
          "counts another range than its build side's");
  refused(plan(choice(scan({p(0)}, {}), node(scan({p(0)}, {})))), "whose other plan reads other tables");
}

TEST_CASE("physical: check refuses a grouped aggregate codegen can't print", "[sql][physical][group]") {
  const logical::ColumnRef name{1, 2, PhysType::Bytes, true};
  auto count = logical::expr(logical::Expr::Kind::Aggregate, SqlType{SqlKind::BigInt});
  auto rows = [] { return node(scan({p(0)}, std::monostate{})); };
  auto grouped = [&](std::vector<logical::ColumnRef> keys, bool streamed, std::vector<logical::Expr> outputs) {
    auto g = node(physical::GroupAggregate{rows(), std::move(keys), {count}, streamed, {}});
    return plan(node(physical::GroupEmit{std::move(g), std::move(outputs)}), {0});
  };
  auto key = logical::column(kId, SqlType{SqlKind::BigInt});
  CHECK(refusal(grouped({kId}, true, {key, count})).empty());
  CHECK(refusal(grouped({kId}, false, {key, count})).empty());
  CHECK(refusal(grouped({}, true, {count})).empty());
  refused(grouped({}, false, {count}), "a structure of groups with no key");
  refused(grouped({name}, true, {count}), "a streamed GROUP BY key held in no local");
  refused(grouped({kId}, true, {logical::column(name, SqlType{SqlKind::Bytea})}), "no key or aggregate of its groups");
  // A key not an I64 is read from the row its group holds: never a covering
  // scan's entry.
  auto covered = node(physical::GroupAggregate{node(scan({p(0)}, std::monostate{}, true)), {name}, {count}, false, {}});
  refused(plan(node(physical::GroupEmit{std::move(covered), {count}}), {0}), "holds a covering scan's entry");
  // A HAVING compares a key or an aggregate of the groups with a parameter.
  auto having = [&](logical::Expr lhs) {
    auto g = node(physical::GroupAggregate{rows(), {kId}, {count}, false, {}});
    auto f = node(physical::Filter{std::move(g), {logical::compare(ast::Compare::Op::Gt, std::move(lhs), p(1))}});
    return plan(node(physical::GroupEmit{std::move(f), {count}}), {0});
  };
  CHECK(refusal(having(count)).empty());
  refused(having(logical::column(name, SqlType{SqlKind::Bytea})), "compares no key or aggregate");
  // A sort holds a structure's groups, by its keys.
  auto sorted = [&](bool streamed, logical::ColumnRef by) {
    auto g = node(physical::GroupAggregate{rows(), {kId}, {count}, streamed, {}});
    auto s = node(physical::Sort{std::move(g), {{by, false, false}}, 0, {}, {}, true});
    return plan(node(physical::GroupEmit{std::move(s), {count}}), {0});
  };
  CHECK(refusal(sorted(false, kId)).empty());
  refused(sorted(true, kId), "a sort of groups not over a structure of them");
  refused(sorted(false, name), "a sort of groups by a column not of them");
  // A stream holds no row: its BOOLEAN key read from a covering scan's
  // entry, as an I64's is. A structure reads it from the row it holds.
  const logical::ColumnRef flag{1, 3, PhysType::Bool, true};
  auto over_entry = [&](bool streamed) {
    auto g = node(physical::GroupAggregate{node(scan({p(0)}, std::monostate{}, true)), {flag}, {count}, streamed, {}});
    return plan(node(physical::GroupEmit{std::move(g), {count}}), {0});
  };
  CHECK(refusal(over_entry(true)).empty());
  refused(over_entry(false), "holds a covering scan's entry");
  // A dependent column is given, sorted by and compared like a key, and is
  // none: a group of every row has none.
  auto dependent = [&](std::vector<logical::ColumnRef> keys, std::vector<logical::ColumnRef> dep, bool streamed) {
    auto g = node(physical::GroupAggregate{rows(), std::move(keys), {count}, streamed, std::move(dep)});
    auto s = streamed ? std::move(g) : node(physical::Sort{std::move(g), {{name, false, false}}, 0, {}, {}, true});
    return plan(node(physical::GroupEmit{std::move(s), {logical::column(name, SqlType{SqlKind::Bytea}), count}}), {0});
  };
  CHECK(refusal(dependent({kId}, {name}, false)).empty());
  refused(dependent({kId}, {name}, true), "a streamed GROUP BY key held in no local");
  refused(dependent({}, {name}, true), "a dependent column of one group of every row");
  refused(dependent({kId, name}, {name}, false), "a dependent column that is a key");
}
