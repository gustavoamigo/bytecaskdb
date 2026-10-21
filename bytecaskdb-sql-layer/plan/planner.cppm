// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// planner.cppm — a bound DML statement to the statement IR. It decides the
// access path, the bounds, the scan's direction and whether a read covers;
// the code generator (codegen.cppm) prints what it decided. The rules are
// docs/frontend_design.md's, *The planner's choices*, as far as slice 1
// takes them:
//   1. every primary key column compared for equality  -> a point lookup
//   2. a SELECT otherwise takes the primary key or an index whose first part
//      is compared: with an ORDER BY, only one whose order gives it; the
//      longest run of equality-compared parts wins, then a range after the
//      run, then the primary key, then the covering path, then the lower id
//   3. its NULLs placed as PostgreSQL places them, or refused
// A conjunct the chosen key does not express (a residual filter) is refused
// until slice 5 brings residuals. UPDATE and DELETE name one row by the
// whole primary key until slice 4.

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
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

[[noreturn]] void residual(const Table& t, const bound::Compare& c) {
  no_plan("a filter on column \"" + t.column(c.col_id)->name + "\" that the key does not express is not supported yet");
}

// The parameter each primary key column is compared equal to, or nullopt
// if rule 1 does not apply.
auto point_get(const Table& t, const std::vector<bound::Compare>& where) -> std::optional<ir::PointGet> {
  ir::PointGet g;
  std::vector<const bound::Compare*> used;
  for (auto id : t.pk_col_ids) {
    auto it = std::ranges::find_if(where, [&](const auto& c) { return c.col_id == id && c.op == Op::Eq; });
    if (it == where.end()) return std::nullopt;
    g.key_params.push_back(it->param);
    used.push_back(&*it);
  }
  for (const auto& c : where) {
    if (std::ranges::find(used, &c) == used.end()) residual(t, c);
  }
  return g;
}

auto point_or_refuse(const Table& t, const std::vector<bound::Compare>& where, const char* stmt) -> ir::PointGet {
  auto g = point_get(t, where);
  if (!g) no_plan(std::string{stmt} + " must name its row by the full primary key");
  return *g;
}

struct Part {
  std::uint32_t col;
  bool desc;
};

// An access path: the primary key, or an index, with the parts of its key
// in order: an index's, then every part of the primary key, a column the
// index names among them (the catalog's key layout).
struct Path {
  std::uint32_t object;
  bool is_pk;
  std::vector<Part> parts;
};

auto paths(const Table& t) -> std::vector<Path> {
  std::vector<Path> out;
  Path pk{t.id, true, {}};
  for (auto id : t.pk_col_ids) pk.parts.push_back({id, false});
  out.push_back(pk);
  for (const auto& ix : t.indexes) {
    Path p{ix.id, false, {}};
    for (const auto& part : ix.parts) p.parts.push_back({part.col_id, part.desc});
    for (auto id : t.pk_col_ids) p.parts.push_back({id, false});
    out.push_back(std::move(p));
  }
  return out;
}

// What a path makes of the WHERE: the equality run, the range on the part
// after it, and whether its order gives the ORDER BY (and in which
// direction).
struct Choice {
  const Path* path;
  std::vector<const bound::Compare*> eq;  // one per part of the run
  const bound::Compare* lo{nullptr};      // > or >= on the part after the run
  const bound::Compare* hi{nullptr};      // < or <=
  bool serves{true};
  bool reverse{false};
  bool covering{false};
};

auto choose(const Path& p, const std::vector<bound::Compare>& where, const std::vector<bound::OrderItem>& ordered)
    -> Choice {
  Choice c{&p, {}};
  auto first = [&](std::uint32_t col, auto pred) -> const bound::Compare* {
    auto it = std::ranges::find_if(where, [&](const bound::Compare& w) { return w.col_id == col && pred(w.op); });
    return it == where.end() ? nullptr : &*it;
  };
  for (const auto& part : p.parts) {
    const auto* e = first(part.col, [](Op op) { return op == Op::Eq; });
    if (e == nullptr) break;
    c.eq.push_back(e);
  }
  auto run = c.eq.size();
  if (run < p.parts.size()) {
    c.lo = first(p.parts[run].col, [](Op op) { return op == Op::Gt || op == Op::Ge; });
    c.hi = first(p.parts[run].col, [](Op op) { return op == Op::Lt || op == Op::Le; });
  }
  // The ordered columns are the parts after the run, in order, each in the
  // part's direction (a forward scan) or each against it (a reverse one).
  if (run + ordered.size() > p.parts.size()) {
    c.serves = false;
    return c;
  }
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    const auto& part = p.parts[run + i];
    auto rev = ordered[i].desc != part.desc;
    if (part.col != ordered[i].col_id || (i > 0 && rev != c.reverse)) {
      c.serves = false;
      return c;
    }
    c.reverse = rev;
  }
  return c;
}

auto in_key(const Table& t, const Path& p, std::uint32_t col) -> bool {
  // Not a DOUBLE: the key encoding writes -0.0 as 0.0 and every NaN as one,
  // so only the row holds its exact value.
  if (t.column(col)->type == PhysType::F64) return false;
  return std::ranges::any_of(p.parts, [&](const Part& q) { return q.col == col; });
}

auto names(const Table& t, const std::vector<bound::OrderItem>& items) -> std::string {
  std::string out;
  for (const auto& o : items) out += (out.empty() ? "" : ", ") + t.column(o.col_id)->name;
  return out;
}

auto param_bound(const bound::Compare* c, bool successor) -> ir::KeyBound {
  if (c == nullptr) return {};
  return {ir::KeyBound::Kind::Param, c->param, successor};
}

auto select_from(const Table& t, const bound::Select& s) -> std::variant<ir::PointGet, ir::Scan, ir::NullSplitScan> {
  // A point lookup returns at most one row: it gives any ORDER BY.
  if (auto g = point_get(t, s.where)) return *g;

  // An ordered column compared for equality is constant in the result.
  std::vector<bound::OrderItem> ordered;
  for (const auto& o : s.order_by) {
    auto constant = std::ranges::any_of(s.where, [&](const auto& w) { return w.col_id == o.col_id && w.op == Op::Eq; });
    auto repeated = std::ranges::any_of(ordered, [&](const auto& q) { return q.col_id == o.col_id; });
    if (!constant && !repeated) ordered.push_back(o);
  }
  std::vector<std::uint32_t> used_cols = s.targets;
  if (s.count && s.count->col_id) used_cols.push_back(*s.count->col_id);

  auto all = paths(t);
  std::vector<Choice> usable;
  for (const auto& p : all) {
    auto c = choose(p, s.where, ordered);
    if (c.eq.empty() && c.lo == nullptr && c.hi == nullptr) continue;
    // FOR UPDATE reads each row, whose key it guards.
    c.covering = !s.for_update && std::ranges::all_of(used_cols, [&](std::uint32_t col) { return in_key(t, p, col); });
    usable.push_back(c);
  }
  if (usable.empty()) no_plan("a full scan of " + t.name);
  std::erase_if(usable, [](const Choice& c) { return !c.serves; });
  if (usable.empty()) no_plan("no index of " + t.name + " gives ORDER BY " + names(t, s.order_by));
  auto rank = [](const Choice& c) {
    return std::tuple{c.eq.size(), c.lo != nullptr || c.hi != nullptr, c.path->is_pk, c.covering};
  };
  const auto& best = *std::ranges::min_element(usable, [&](const Choice& a, const Choice& b) {
    if (rank(a) != rank(b)) return rank(a) > rank(b);
    return a.path->object < b.path->object;
  });

  for (const auto& w : s.where) {
    if (std::ranges::find(best.eq, &w) == best.eq.end() && &w != best.lo && &w != best.hi) residual(t, w);
  }

  ir::Scan out;
  out.object = best.path->object;
  for (const auto* e : best.eq) out.prefix.push_back(e->param);
  for (const auto& w : s.where) {
    if (std::ranges::find(out.null_checks, w.param) == out.null_checks.end()) out.null_checks.push_back(w.param);
  }
  out.reverse = best.reverse;
  out.covering = best.covering;
  const auto run = best.eq.size();

  // NULLs in the ordered parts: a part the WHERE compares holds none in the
  // rows it matches. The key order puts an ASC part's NULLs first and a DESC
  // part's last, PostgreSQL the other way round in both directions, so only
  // a nullable ASC part first after the run is served: by two ranges.
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    const auto* col = t.column(ordered[i].col_id);
    auto compared = std::ranges::any_of(s.where, [&](const auto& w) { return w.col_id == col->id; });
    if (!col->nullable || compared) continue;
    auto later_nullable = std::ranges::any_of(ordered.begin() + static_cast<std::ptrdiff_t>(i) + 1, ordered.end(),
                                              [&](const auto& o) { return t.column(o.col_id)->nullable; });
    if (i != 0 || best.path->parts[run].desc || later_nullable) {
      no_plan("ORDER BY " + col->name + " would need its NULLs reordered");
    }
    out.rest = ir::Range{{ir::KeyBound::Kind::NullPart, 0, true}, {}};
    return ir::NullSplitScan{out};
  }

  if (best.lo == nullptr && best.hi == nullptr) return out;
  // A key bound: the compared value's key part, or the part that holds NULL,
  // or open; as is or its successor. A key part is self-delimiting and the
  // rest of the key follows it, so a bound that must pass every entry with
  // the part equal to a value is that value's successor.
  const auto& part = best.path->parts[run];
  auto nullable = t.column(part.col)->nullable;
  const auto* lo = best.lo;
  const auto* hi = best.hi;
  ir::Range r;
  if (!part.desc) {
    // ASC: key order is value order, nulls first. A comparison with NULL is
    // never true, so with no lower bound the scan starts past the null
    // entries, at the successor of the null part.
    r.lo = lo == nullptr && nullable ? ir::KeyBound{ir::KeyBound::Kind::NullPart, 0, true}
                                     : param_bound(lo, lo != nullptr && lo->op == Op::Gt);
    r.hi = param_bound(hi, hi != nullptr && hi->op == Op::Le);
  } else {
    // DESC: key order is descending value order, nulls last. The upper value
    // bound gives the lower key bound and the lower value bound the upper;
    // with no lower value bound the scan stops before the null entries, at
    // the null part.
    r.lo = param_bound(hi, hi != nullptr && hi->op == Op::Lt);
    r.hi = lo == nullptr && nullable ? ir::KeyBound{ir::KeyBound::Kind::NullPart, 0, false}
                                     : param_bound(lo, lo != nullptr && lo->op == Op::Ge);
  }
  out.rest = r;
  return out;
}

auto select(const Table& t, const bound::Select& s) -> ir::Select {
  ir::Select sel;
  sel.from = select_from(t, s);
  sel.limit = s.limit;
  sel.for_update = s.for_update;
  const auto* scan = ir::scan_of(sel);
  if (!s.count) {
    for (auto id : s.targets) sel.targets.push_back(column(t, id));
    return sel;
  }
  ir::Count count;
  if (s.count->col_id) count.arg = column(t, *s.count->col_id);
  // Every entry of the range is a row to count, unless the argument can be
  // NULL in one: the WHERE compares every column it names, so those hold
  // none.
  auto may_be_null = count.arg && count.arg->nullable &&
                     std::ranges::none_of(s.where, [&](const auto& w) { return w.col_id == count.arg->id; });
  count.from_keys = scan != nullptr && !may_be_null;
  sel.count = count;
  return sel;
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
          table(*s.table);
          return select(*s.table, s);
        } else if constexpr (std::is_same_v<T, bound::Update>) {
          const auto& t = *s.table;
          table(t);
          ir::Update up{point_or_refuse(t, s.where, "UPDATE"), {}};
          for (const auto& [id, e] : s.set) {
            ir::Expr v;
            switch (e.kind) {
              case bound::Expr::Kind::Param: v = {ir::Expr::Kind::Param, e.param, {}}; break;
              case bound::Expr::Kind::Column: v = {ir::Expr::Kind::Column, 0, column(t, e.col_id)}; break;
              case bound::Expr::Kind::Add: v = {ir::Expr::Kind::Add, e.param, column(t, e.col_id)}; break;
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
