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

// Table 1 with columns 0 and 1, and index 3 over column 0.
auto plan(ir::Statement s, std::size_t params = 2) -> ir::Plan {
  std::vector<Catalog::PlanConstant> constants{
      {"TBL_1", 1, "t"}, {"IDX_3", 3, "t_i"}, {"COL_1_0", 0, "t.id"}, {"COL_1_1", 1, "t.d"}, {"PART_3_0", 0, "t_i.id"}};
  return ir::Plan{1, std::move(constants), 1, std::vector<PhysType>(params, PhysType::I64), std::move(s)};
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
  refused(plan(ir::Select{ir::PointGet{{0}}, {ir::Column{7, PhysType::I64, false}}}), "does not have: COL_1_7");

  ir::Scan neither{3, {}, {}, {}, false};
  refused(plan(ir::Select{neither, {kId}}), "an equality prefix or a range");
  ir::Scan both{3, {0}, ir::Range{}, {0}, false};
  refused(plan(ir::Select{both, {kId}}), "an equality prefix or a range");
  ir::Scan two_parts{3, {0, 1}, {}, {0, 1}, false};
  refused(plan(ir::Select{two_parts, {kId}}), "more than one part");
  ir::Scan eq_unchecked{3, {0}, {}, {}, false};
  refused(plan(ir::Select{eq_unchecked, {kId}}), "does not check for NULL");

  const ir::KeyBound p0{ir::KeyBound::Kind::Param, 0, false};
  const ir::KeyBound p2{ir::KeyBound::Kind::Param, 2, false};
  ir::Scan range{3, {}, ir::Range{p0, {}}, {0}, false};
  CHECK_NOTHROW(ir::check(plan(ir::Select{range, {kId}})));
  ir::Scan range_past{3, {}, ir::Range{{}, p2}, {}, false};
  refused(plan(ir::Select{range_past, {kId}}), "past the statement's");
  ir::Scan range_unchecked{3, {}, ir::Range{p0, {}}, {}, false};
  refused(plan(ir::Select{range_unchecked, {kId}}), "does not check for NULL");
  ir::Scan null_check_past{3, {}, ir::Range{p0, {}}, {0, 2}, false};
  refused(plan(ir::Select{null_check_past, {kId}}), "past the statement's");
  const ir::KeyBound null_part{ir::KeyBound::Kind::NullPart, 0, true};
  ir::Scan no_param{3, {}, ir::Range{null_part, {}}, {}, false};
  refused(plan(ir::Select{no_param, {kId}}), "compares no parameter");
  ir::Scan other_index{4, {0}, {}, {0}, false};
  refused(plan(ir::Select{other_index, {kId}}), "does not have: IDX_4");

  ir::Scan covering{3, {0}, {}, {0}, true};
  CHECK_NOTHROW(ir::check(plan(ir::Select{covering, {kId}})));
  refused(plan(ir::Select{covering, {kD}}), "DOUBLE target");
  // Column 1 is not in index 3's key: no part to read it from.
  refused(plan(ir::Select{covering, {ir::Column{1, PhysType::I64, false}}}), "does not have: PART_3_1");

  refused(plan(ir::Insert{}), "sets no column");
  refused(plan(ir::Update{ir::PointGet{{0}}, {}}), "sets no column");
  refused(plan(ir::Update{ir::PointGet{{0}}, {{kD, ir::Expr{ir::Expr::Kind::Sub, 1, kD}}}}), "other than an integer");
  refused(plan(ir::Update{ir::PointGet{{0}}, {{kId, ir::Expr{ir::Expr::Kind::Column, 0, kD}}}}), "of another type");
  refused(plan(ir::Delete{ir::PointGet{{}}}), "no key part");
  refused(ir::Plan{1, {}, 1, {}, ir::Delete{ir::PointGet{{0}}}}, "no constants");
}
