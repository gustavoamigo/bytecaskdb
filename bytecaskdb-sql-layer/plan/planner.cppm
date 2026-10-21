// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// planner.cppm — a bound DML statement to the statement IR. It decides the
// access path, the bounds and whether a read covers; the code generator
// (codegen.cppm) prints what it decided. The rules are slice 0's, today's:
//   1. every primary key column compared for equality  -> a point lookup
//   2. a range or equality on the first column of a secondary index -> a
//      scan of the index, covering when its key holds every result column
//      (SELECT only)
//   3. anything else                                    -> refused
// docs/frontend_design.md, *The planner's choices*, is where later slices
// take them.

module;
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.planner;

export import bytecask.sql.binder;
export import bytecask.sql.ir;

export namespace bytecask::sql {

// Throws ast::SqlError (position -1) when no rule applies.
[[nodiscard]] auto plan_statement(const bound::Bound& b, const Catalog& cat) -> ir::Plan;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

using ast::SqlError;
using Op = ast::Compare::Op;

[[noreturn]] void no_plan(const std::string& why) { throw SqlError{"no plan: " + why, -1}; }

auto column(const Table& t, std::uint32_t id) -> ir::Column {
  const auto& c = *t.column(id);
  return {c.id, c.type, c.nullable};
}

// The parameter each primary key column is compared equal to, or nullopt
// if rule 1 does not apply.
auto point_get(const Table& t, const std::vector<bound::Compare>& where) -> std::optional<ir::PointGet> {
  ir::PointGet g;
  for (auto id : t.pk_col_ids) {
    auto it = std::ranges::find_if(where, [&](const auto& c) { return c.col_id == id && c.op == Op::Eq; });
    if (it == where.end()) return std::nullopt;
    g.key_params.push_back(it->param);
  }
  if (where.size() != t.pk_col_ids.size()) no_plan("predicates beyond the primary key are not supported");
  return g;
}

auto param_bound(const std::optional<bound::Compare>& c, bool successor) -> ir::KeyBound {
  if (!c) return {};
  return {ir::KeyBound::Kind::Param, c->param, successor};
}

// Rule 2: every conjunct ranges over the first column of one index.
auto scan(const Table& t, const bound::Select& s) -> ir::Scan {
  if (s.where.empty()) no_plan("a full scan is not supported");
  auto col_id = s.where[0].col_id;
  auto ix_it = std::ranges::find_if(t.indexes, [&](const Index& ix) { return ix.parts[0].col_id == col_id; });
  if (ix_it == t.indexes.end()) no_plan("no index leads with column \"" + t.column(col_id)->name + "\"");
  const auto& ix = *ix_it;
  std::optional<bound::Compare> lo, hi, eq;
  for (const auto& c : s.where) {
    if (c.col_id != col_id) no_plan("predicates on more than one column are not supported");
    auto& slot = c.op == Op::Eq ? eq : (c.op == Op::Gt || c.op == Op::Ge) ? lo : hi;
    if (slot) no_plan("two bounds on the same side are not supported");
    slot = c;
  }
  if (eq && (lo || hi)) no_plan("an equality and a range on one column are not supported");

  ir::Scan out;
  out.index = ix.id;
  for (const auto& c : s.where) {
    if (std::ranges::find(out.null_checks, c.param) == out.null_checks.end()) out.null_checks.push_back(c.param);
  }
  // Covering when the index key holds every column the result needs: its
  // parts, then the primary key's. Not a DOUBLE: the key encoding writes
  // -0.0 as 0.0 and every NaN as one, so only the row holds its exact value.
  auto in_key = [&](std::uint32_t id) {
    if (t.column(id)->type == PhysType::F64) return false;
    return std::ranges::any_of(ix.parts, [&](const IndexPart& p) { return p.col_id == id; }) ||
           std::ranges::find(t.pk_col_ids, id) != t.pk_col_ids.end();
  };
  out.covering = std::ranges::all_of(s.targets, in_key);
  if (eq) {
    out.eq_params = {eq->param};
    return out;
  }
  // A key bound: the compared value's key part, or the part that holds NULL,
  // or neither; as is or its successor. A key part is self-delimiting and the
  // primary key follows it, so a bound that must pass every entry with the
  // part equal to a value is that value's successor.
  auto nullable = t.column(col_id)->nullable;
  ir::Range r;
  if (!ix.parts[0].desc) {
    // ASC: key order is value order, nulls first. A comparison with NULL is
    // never true, so with no lower bound the scan starts past the null
    // entries, at the successor of the null part.
    r.lo = !lo && nullable ? ir::KeyBound{ir::KeyBound::Kind::NullPart, 0, true} : param_bound(lo, lo && lo->op == Op::Gt);
    r.hi = param_bound(hi, hi && hi->op == Op::Le);
  } else {
    // DESC: key order is descending value order, nulls last. The upper value
    // bound gives the lower key bound and the lower value bound the upper;
    // with no lower value bound the scan stops before the null entries, at
    // the null part.
    r.lo = param_bound(hi, hi && hi->op == Op::Lt);
    r.hi = !lo && nullable ? ir::KeyBound{ir::KeyBound::Kind::NullPart, 0, false} : param_bound(lo, lo && lo->op == Op::Ge);
  }
  out.rest = r;
  return out;
}

auto point_or_refuse(const Table& t, const std::vector<bound::Compare>& where, const char* stmt) -> ir::PointGet {
  auto g = point_get(t, where);
  if (!g) no_plan(std::string{stmt} + " must name its row by the full primary key");
  return *g;
}

}  // namespace

auto plan_statement(const bound::Bound& b, const Catalog& cat) -> ir::Plan {
  ir::Plan out;
  out.catalog_version = cat.version();
  out.param_types = b.param_types;
  auto table = [&](const Table& t) {
    out.table = t.id;
    out.constants = Catalog::constants(t);
  };
  out.stmt = std::visit(
      [&](const auto& s) -> ir::Statement {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, bound::Insert>) {
          const auto& t = *s.table;
          table(t);
          ir::Insert ins;
          for (std::size_t i = 0; i < t.columns.size(); ++i) {
            if (s.values[i]) ins.values.emplace_back(column(t, t.columns[i].id), s.values[i]->param);
          }
          return ins;
        } else if constexpr (std::is_same_v<T, bound::Select>) {
          const auto& t = *s.table;
          table(t);
          ir::Select sel;
          if (auto g = point_get(t, s.where)) {
            sel.from = *g;
          } else {
            sel.from = scan(t, s);
          }
          for (auto id : s.targets) sel.targets.push_back(column(t, id));
          return sel;
        } else if constexpr (std::is_same_v<T, bound::Update>) {
          const auto& t = *s.table;
          table(t);
          ir::Update up{point_or_refuse(t, s.where, "UPDATE"), {}};
          for (const auto& [id, e] : s.set) {
            ir::Expr v;
            switch (e.kind) {
              case bound::Expr::Kind::Param: v = {ir::Expr::Kind::Param, e.param, {}}; break;
              case bound::Expr::Kind::Column: v = {ir::Expr::Kind::Column, 0, column(t, e.col_id)}; break;
              case bound::Expr::Kind::Sub: v = {ir::Expr::Kind::Sub, e.param, column(t, e.col_id)}; break;
            }
            up.set.emplace_back(column(t, id), v);
          }
          return up;
        } else if constexpr (std::is_same_v<T, bound::Delete>) {
          const auto& t = *s.table;
          table(t);
          return ir::Delete{point_or_refuse(t, s.where, "DELETE")};
        } else {
          throw SqlError{"DDL has no plan", -1};
        }
      },
      b.stmt);
  return out;
}

}  // namespace bytecask::sql
