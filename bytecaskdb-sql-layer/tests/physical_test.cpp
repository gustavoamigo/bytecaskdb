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
  refused(plan(emit(node(scan({p(0)}, {}, true)), {kD})), "DOUBLE target");
  refused(plan(emit(node(scan({p(0)}, {}, true, true)))), "FOR UPDATE scan that covers");
  CHECK(refusal(plan(emit(node(scan({p(0)}, {}, false, true))))).empty());
  // The NULL split's values start past the NULL part.
  refused(plan(emit(node(physical::NullSplitScan{scan({p(0)}, {})}))), "past the NULL part");
  auto past_nulls = physical::Range{{physical::KeyBound::Kind::NullPart, std::nullopt, true}, {}};
  CHECK(refusal(plan(emit(node(physical::NullSplitScan{scan({p(0)}, past_nulls)})))).empty());
  auto reverse = scan({p(0)}, past_nulls);
  reverse.reverse = true;
  refused(plan(node(physical::MinFirstEntry{reverse, kId, true})), "reverse scan");
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
  refused(plan(loop(node(scan({p(0)}, {})), inner())), "outer side is not one row");
  refused(plan(loop(get({p(0)}), get({p(1)}))), "inner side is not a scan");
  // A join conjunct reads a column of the outer side: of its point lookup,
  // or of a table it looks up by key.
  CHECK(refusal(plan(loop(get({p(0)}), joined(kId)))).empty());
  refused(plan(loop(get({p(0)}), joined(other))), "reads a column outside its outer side");
  CHECK(refusal(plan(loop(node(physical::IndexJoin{get({p(0)}), 3, {p(0)}}), joined(other)))).empty());
}
