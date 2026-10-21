// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ir::check refuses each IR the types allow but the printer can't print:
// a compiler bug, raised as std::logic_error, never a plan.

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.ir;

using namespace bytecask::sql;

namespace {

auto plan(ir::Statement s, std::size_t params = 2) -> ir::Plan {
  return ir::Plan{1, {{"TBL_1", 1, "t"}}, 1, std::vector<PhysType>(params, PhysType::I64), std::move(s)};
}

// A forward scan of index 3.
auto scan(std::vector<std::uint32_t> prefix, std::variant<std::monostate, ir::Range> rest,
          std::vector<std::uint32_t> null_checks, bool covering = false) -> ir::Scan {
  return {3, std::move(prefix), rest, false, std::move(null_checks), covering};
}

const ir::Column kId{0, PhysType::I64, false};
const ir::Column kD{1, PhysType::F64, true};

// The rule ir::check names, or "" if it accepts the plan.
auto refusal(const ir::Plan& p) -> std::string {
  try {
    ir::check(p);
  } catch (const std::logic_error& e) {
    return e.what();
  }
  return "";
}

void refused(const ir::Plan& p, const std::string& rule) {
  auto why = refusal(p);
  INFO(why);
  CHECK(why.find(rule) != std::string::npos);
}

}  // namespace

TEST_CASE("ir: check refuses what the printer can't print", "[sql][ir]") {
  CHECK_NOTHROW(ir::check(plan(ir::Select{ir::PointGet{{0}}, {kId}})));

  refused(plan(ir::Select{ir::PointGet{{}}, {kId}}), "no key part");
  refused(plan(ir::Select{ir::PointGet{{2}}, {kId}}), "past the statement's");
  CHECK_NOTHROW(ir::check(plan(ir::Select{ir::PointGet{{0}}, {}})));

  refused(plan(ir::Select{scan({}, {}, {}), {kId}}), "an equality prefix or a range");
  refused(plan(ir::Select{scan({0}, {}, {}), {kId}}), "does not check for NULL");

  const ir::KeyBound p0{ir::KeyBound::Kind::Param, 0, false};
  const ir::KeyBound p1{ir::KeyBound::Kind::Param, 1, false};
  const ir::KeyBound p2{ir::KeyBound::Kind::Param, 2, false};
  CHECK_NOTHROW(ir::check(plan(ir::Select{scan({}, ir::Range{p0, {}}, {0}), {kId}})));
  CHECK_NOTHROW(ir::check(plan(ir::Select{scan({0}, ir::Range{p1, {}}, {0, 1}), {kId}})));
  refused(plan(ir::Select{scan({}, ir::Range{{}, p2}, {}), {kId}}), "past the statement's");
  refused(plan(ir::Select{scan({}, ir::Range{p0, {}}, {}), {kId}}), "does not check for NULL");
  refused(plan(ir::Select{scan({}, ir::Range{p0, {}}, {0, 2}), {kId}}), "past the statement's");

  auto covering = scan({0}, {}, {0}, true);
  CHECK_NOTHROW(ir::check(plan(ir::Select{covering, {kId}})));
  refused(plan(ir::Select{covering, {kD}}), "DOUBLE target");

  refused(plan(ir::Insert{}), "sets no column");
  refused(plan(ir::Update{ir::PointGet{{0}}, {}}), "sets no column");
  refused(plan(ir::Update{ir::PointGet{{0}}, {{kD, ir::Expr{ir::Expr::Kind::Sub, 1, kD}}}}), "other than an integer");
  refused(plan(ir::Update{ir::PointGet{{0}}, {{kId, ir::Expr{ir::Expr::Kind::Column, 0, kD}}}}), "of another type");
  refused(plan(ir::Delete{ir::PointGet{{}}}), "no key part");
  refused(ir::Plan{1, {}, 1, {}, ir::Delete{ir::PointGet{{0}}}}, "no constants");
}
