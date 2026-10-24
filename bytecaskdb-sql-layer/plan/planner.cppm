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
// UPDATE and DELETE take their rows by the same rules, without an order. A
// conjunct the chosen key does not express is a filter on each row it
// reads; an equality with $n ± k is always one, never a key part. A join of
// two tables reads them in FROM order by two point lookups, or scans one and
// reads the other by its whole primary key from the scanned row's columns.

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
  return {c.id, c.type, c.nullable, t.id};
}

// An equality the key can take: with a parameter, not a computed value.
auto key_eq(const bound::Compare& c) -> bool { return c.op == Op::Eq && !c.offset; }

// $param + offset's index in the plan's computed values, added if new.
auto computed(std::vector<ir::Computed>& comp, const bound::Compare& c, std::uint32_t table) -> std::uint32_t {
  auto it = std::ranges::find_if(
      comp, [&](const ir::Computed& v) { return v.param == c.param && v.offset == *c.offset && v.table == table; });
  if (it != comp.end()) return static_cast<std::uint32_t>(it - comp.begin());
  comp.push_back({c.param, *c.offset, table});
  return static_cast<std::uint32_t>(comp.size() - 1);
}

// The conjuncts of `where` not in `used`, as filters. A DOUBLE or REAL
// column has none yet: its comparisons need PostgreSQL's NaN and -0.0
// rules, which only its key encoding has.
auto filters(const Table& t, const std::vector<bound::Compare>& where, const std::vector<const bound::Compare*>& used,
             std::vector<ir::Computed>& comp) -> std::vector<ir::Filter> {
  std::vector<ir::Filter> out;
  for (const auto& c : where) {
    if (std::ranges::find(used, &c) != used.end()) continue;
    auto col = column(t, c.col_id);
    if (col.type == PhysType::F64) {
      no_plan("a filter on column \"" + t.column(c.col_id)->name + "\", a REAL or DOUBLE PRECISION, is not supported yet");
    }
    ir::Operand rhs{c.param, std::nullopt, std::nullopt};
    if (c.offset) rhs.computed = computed(comp, c, t.id);
    out.push_back({col, c.op, rhs});
  }
  return out;
}

// The whole primary key by equality: the lookup and the conjuncts it uses.
struct Keyed {
  ir::PointGet get;
  std::vector<const bound::Compare*> used;
};

// The parameter each primary key column is compared equal to, or nullopt
// if rule 1 does not apply.
auto point_get(const Table& t, const std::vector<bound::Compare>& where) -> std::optional<Keyed> {
  Keyed k;
  for (auto id : t.pk_col_ids) {
    auto it = std::ranges::find_if(where, [&](const auto& c) { return c.col_id == id && key_eq(c); });
    if (it == where.end()) return std::nullopt;
    k.get.key_params.push_back(it->param);
    k.used.push_back(&*it);
  }
  return k;
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
    auto it = std::ranges::find_if(where, [&](const bound::Compare& w) { return w.col_id == part.col && key_eq(w); });
    if (it == where.end()) break;
    c.eq.push_back(&*it);
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

// The rows of a table the WHERE matches, by the path chosen, and the
// conjuncts that path does not express.
struct Rows {
  std::variant<ir::PointGet, ir::Scan, ir::NullSplitScan> path;
  std::vector<ir::Filter> filters;
};

// The rows of `t` the WHERE matches, in the ORDER BY's order. `used_cols`
// are the columns read from each; with `may_cover`, the scan reads them
// from the entry's key when it holds them all, unless a filter reads the row.
auto rows_of(const Table& t, const std::vector<bound::Compare>& where, const std::vector<bound::OrderItem>& order_by,
             const std::vector<std::uint32_t>& used_cols, bool may_cover, std::vector<ir::Computed>& comp) -> Rows {
  auto param_bound = [&](const bound::Compare* c, bool successor) -> ir::KeyBound {
    if (c == nullptr) return {};
    if (c->offset) return {ir::KeyBound::Kind::Computed, computed(comp, *c, t.id), successor};
    return {ir::KeyBound::Kind::Param, c->param, successor};
  };
  // A point lookup returns at most one row: it gives any ORDER BY.
  if (auto k = point_get(t, where)) return {k->get, filters(t, where, k->used, comp)};

  // An ordered column compared for equality is constant in the result.
  std::vector<bound::OrderItem> ordered;
  for (const auto& o : order_by) {
    auto constant = std::ranges::any_of(where, [&](const auto& w) { return w.col_id == o.col_id && w.op == Op::Eq; });
    auto repeated = std::ranges::any_of(ordered, [&](const auto& q) { return q.col_id == o.col_id; });
    if (!constant && !repeated) ordered.push_back(o);
  }

  auto all = paths(t);
  std::vector<Choice> usable;
  for (const auto& p : all) {
    auto c = choose(p, where, ordered);
    if (c.eq.empty() && c.lo == nullptr && c.hi == nullptr) continue;
    c.covering = may_cover && std::ranges::all_of(used_cols, [&](std::uint32_t col) { return in_key(t, p, col); });
    usable.push_back(c);
  }
  if (usable.empty()) no_plan("a full scan of " + t.name);
  std::erase_if(usable, [](const Choice& c) { return !c.serves; });
  if (usable.empty()) no_plan("no index of " + t.name + " gives ORDER BY " + names(t, order_by));
  auto rank = [](const Choice& c) {
    return std::tuple{c.eq.size(), c.lo != nullptr || c.hi != nullptr, c.path->is_pk, c.covering};
  };
  const auto& best = *std::ranges::min_element(usable, [&](const Choice& a, const Choice& b) {
    if (rank(a) != rank(b)) return rank(a) > rank(b);
    return a.path->object < b.path->object;
  });

  std::vector<const bound::Compare*> used = best.eq;
  for (const auto* b : {best.lo, best.hi}) {
    if (b != nullptr) used.push_back(b);
  }
  auto residual = filters(t, where, used, comp);

  ir::Scan out;
  out.object = best.path->object;
  for (const auto* e : best.eq) out.prefix.push_back(e->param);
  for (const auto& w : where) {
    if (std::ranges::find(out.null_checks, w.param) == out.null_checks.end()) out.null_checks.push_back(w.param);
  }
  out.reverse = best.reverse;
  out.covering = best.covering && residual.empty();
  const auto run = best.eq.size();

  // NULLs in the ordered parts: a part the WHERE compares holds none in the
  // rows it matches. The key order puts an ASC part's NULLs first and a DESC
  // part's last, PostgreSQL the other way round in both directions, so only
  // a nullable ASC part first after the run is served: by two ranges.
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    const auto* col = t.column(ordered[i].col_id);
    auto compared = std::ranges::any_of(where, [&](const auto& w) { return w.col_id == col->id; });
    if (!col->nullable || compared) continue;
    auto later_nullable = std::ranges::any_of(ordered.begin() + static_cast<std::ptrdiff_t>(i) + 1, ordered.end(),
                                              [&](const auto& o) { return t.column(o.col_id)->nullable; });
    if (i != 0 || best.path->parts[run].desc || later_nullable) {
      no_plan("ORDER BY " + col->name + " would need its NULLs reordered");
    }
    out.rest = ir::Range{{ir::KeyBound::Kind::NullPart, 0, true}, {}};
    return {ir::NullSplitScan{out}, residual};
  }

  if (best.lo == nullptr && best.hi == nullptr) return {out, residual};
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
  return {out, residual};
}

// MIN(arg) from the scan's first entry: arg is the ASC part after its
// prefix, so the first entry past that part's NULLs holds the least value.
// A scan with no range on the part is given one from past its NULLs.
auto min_from_first_entry(const Table& t, ir::Scan& sc, const ir::Column& arg) -> bool {
  const auto all = paths(t);
  const auto& path = *std::ranges::find_if(all, [&](const Path& p) { return p.object == sc.object; });
  if (sc.prefix.size() >= path.parts.size()) return false;
  const auto& part = path.parts[sc.prefix.size()];
  if (part.col != arg.id || part.desc) return false;
  if (arg.nullable && std::holds_alternative<std::monostate>(sc.rest)) {
    sc.rest = ir::Range{{ir::KeyBound::Kind::NullPart, 0, true}, {}};
  }
  return true;
}

auto aggregate(const bound::Aggregate& a, const Table& t) -> ir::Aggregate {
  using Fn = ast::Expr::Fn;
  ir::Aggregate agg;
  switch (a.fn) {
    case Fn::Count: agg.fn = ir::Aggregate::Fn::Count; break;
    case Fn::Sum: agg.fn = ir::Aggregate::Fn::Sum; break;
    case Fn::Min: agg.fn = ir::Aggregate::Fn::Min; break;
  }
  if (a.col_id) agg.arg = column(t, *a.col_id);
  agg.distinct = a.distinct;
  return agg;
}

auto select(const Table& t, const bound::Select& s, std::vector<ir::Computed>& comp) -> ir::Select {
  std::vector<std::uint32_t> used_cols = s.targets;
  if (s.aggregate && s.aggregate->col_id) used_cols.push_back(*s.aggregate->col_id);
  ir::Select sel;
  // FOR UPDATE reads each row, whose key it guards; COUNT(DISTINCT) keys
  // each value from its row.
  auto distinct = s.aggregate && s.aggregate->distinct;
  auto rows = rows_of(t, s.where, s.order_by, used_cols, !s.for_update && !distinct, comp);
  std::visit([&](auto&& from) { sel.from = std::move(from); }, std::move(rows.path));
  sel.filters = std::move(rows.filters);
  sel.limit = s.limit;
  sel.for_update = s.for_update;
  // The keys answer an aggregate only when no filter drops a row.
  auto* scan = sel.filters.empty() ? std::get_if<ir::Scan>(&sel.from) : nullptr;
  if (!s.aggregate) {
    for (auto id : s.targets) sel.targets.push_back(column(t, id));
    return sel;
  }
  auto agg = aggregate(*s.aggregate, t);
  if (agg.distinct) {
    // Its values grouped and counted, in a loop.
  } else if (agg.fn == ir::Aggregate::Fn::Count) {
    // Every entry of the range is a row to count, unless the argument can be
    // NULL in one: the WHERE compares every column it names, so those hold
    // none.
    auto may_be_null = agg.arg && agg.arg->nullable &&
                       std::ranges::none_of(s.where, [&](const auto& w) { return w.col_id == agg.arg->id; });
    agg.from_keys = scan != nullptr && !may_be_null;
  } else if (agg.fn == ir::Aggregate::Fn::Min && scan != nullptr) {
    agg.from_keys = min_from_first_entry(t, *scan, *agg.arg);
  }
  sel.aggregate = agg;
  return sel;
}

// The rows an UPDATE or DELETE writes: by a point lookup, or a scan, which
// reads each row whole; and the filters on them.
auto write_rows(const Table& t, const std::vector<bound::Compare>& where, std::vector<ir::Computed>& comp)
    -> std::pair<std::variant<ir::PointGet, ir::Scan>, std::vector<ir::Filter>> {
  auto rows = rows_of(t, where, {}, {}, false, comp);
  if (auto* g = std::get_if<ir::PointGet>(&rows.path)) return {*g, std::move(rows.filters)};
  return {std::get<ir::Scan>(rows.path), std::move(rows.filters)};  // no ORDER BY, so never split
}

// Whether some key of `t` is reachable from its parameters alone: a path
// whose first part is compared for equality or bounded.
auto reachable(const Table& t, const std::vector<bound::Compare>& where) -> bool {
  const auto all = paths(t);
  return std::ranges::any_of(all, [&](const Path& p) {
    auto c = choose(p, where, {});
    return !c.eq.empty() || c.lo != nullptr || c.hi != nullptr;
  });
}

// The inner table's whole primary key, each part from a parameter it is
// compared equal to or an outer column it is joined on; and what that takes.
struct InnerKey {
  std::vector<ir::KeyPart> parts;
  std::vector<const bound::Compare*> used;
  std::vector<const bound::JoinOn*> on;
};

auto inner_key(const Table* const (&sides)[2], const bound::Join& s, std::uint32_t inner) -> std::optional<InnerKey> {
  const auto outer = 1 - inner;
  InnerKey k;
  for (auto id : sides[inner]->pk_col_ids) {
    const auto& where = s.where[inner];
    auto it = std::ranges::find_if(where, [&](const auto& c) { return c.col_id == id && key_eq(c); });
    if (it != where.end()) {
      k.parts.push_back({it->param, std::nullopt});
      k.used.push_back(&*it);
      continue;
    }
    auto jt = std::ranges::find_if(s.on, [&](const bound::JoinOn& j) { return j.col[inner] == id; });
    if (jt == s.on.end()) return std::nullopt;
    k.parts.push_back({0, column(*sides[outer], jt->col[outer])});
    k.on.push_back(&*jt);
  }
  return k;
}

// Two tables (docs/frontend_design.md, *Joins*). With no column of one equal
// to one of the other, each by its whole primary key, in FROM order. With
// one, an index nested loop: the outer table reached from its parameters
// alone, the inner one read by its whole primary key from parameters and the
// outer row's columns; FROM order first, then the other, also when the
// first has a filter it cannot serve. Every other conjunct is a filter on the
// row it names.
auto join(const bound::Join& s, std::vector<ir::Computed>& comp) -> ir::Select {
  if (s.ordered) no_plan("ORDER BY on a join");
  const Table* const sides[2]{s.table, s.second};
  ir::Select sel;
  sel.limit = s.limit;
  for (const auto& c : s.targets) sel.targets.push_back(column(*sides[c.side], c.col_id));
  if (s.aggregate) sel.aggregate = aggregate(*s.aggregate, *sides[s.aggregate->side]);
  if (s.on.empty()) {
    ir::PointJoin j{{s.table->id, s.second->id}, {}};
    for (std::size_t i = 0; i < 2; ++i) {
      auto k = point_get(*sides[i], s.where[i]);
      if (!k) no_plan("a join must name the row of each table by its whole primary key yet");
      j.rows[i] = k->get;
      auto f = filters(*sides[i], s.where[i], k->used, comp);
      sel.filters.insert(sel.filters.end(), f.begin(), f.end());
    }
    sel.from = j;
    return sel;
  }
  // One orientation; a filter it cannot serve refuses it, and the other is
  // tried before the refusal stands.
  auto loop_join = [&](std::uint32_t outer, InnerKey k, std::vector<ir::Computed>& into) {
    const auto inner = 1 - outer;
    auto rows = rows_of(*sides[outer], s.where[outer], {}, {}, false, into);
    ir::Scan scan;
    if (const auto* g = std::get_if<ir::PointGet>(&rows.path)) {
      // One outer row: a scan of its whole key's prefix, so one loop serves.
      scan.object = sides[outer]->id;
      scan.prefix = g->key_params;
    } else {
      scan = std::get<ir::Scan>(rows.path);  // no ORDER BY, so never split
    }
    // A NULL parameter on either side matches no pair.
    for (const auto& where : s.where) {
      for (const auto& c : where) {
        if (std::ranges::find(scan.null_checks, c.param) == scan.null_checks.end()) scan.null_checks.push_back(c.param);
      }
    }
    sel.filters = std::move(rows.filters);
    auto f = filters(*sides[inner], s.where[inner], k.used, into);
    sel.filters.insert(sel.filters.end(), f.begin(), f.end());
    for (const auto& j : s.on) {
      if (std::ranges::find(k.on, &j) != k.on.end()) continue;
      auto col = column(*sides[inner], j.col[inner]);
      if (col.type == PhysType::F64) {
        no_plan("a filter on column \"" + sides[inner]->column(j.col[inner])->name +
                "\", a REAL or DOUBLE PRECISION, is not supported yet");
      }
      sel.filters.push_back({col, Op::Eq, {0, std::nullopt, column(*sides[outer], j.col[outer])}});
    }
    sel.from = ir::LoopJoin{{sides[outer]->id, sides[inner]->id}, std::move(scan), std::move(k.parts)};
  };
  std::optional<SqlError> refused;
  for (std::uint32_t outer : {0u, 1u}) {
    if (!reachable(*sides[outer], s.where[outer])) continue;
    auto k = inner_key(sides, s, 1 - outer);
    if (!k) continue;
    auto tried = comp;
    try {
      loop_join(outer, std::move(*k), tried);
    } catch (const SqlError& e) {
      if (!refused) refused = e;
      sel.filters.clear();
      continue;
    }
    comp = std::move(tried);
    return sel;
  }
  if (refused) throw *refused;
  auto outer = reachable(*sides[0], s.where[0]) ? 0u : reachable(*sides[1], s.where[1]) ? 1u : 2u;
  if (outer == 2) no_plan("a full scan of " + s.table->name);
  const auto* inner = sides[1 - outer];
  std::string cols;
  for (const auto& j : s.on) cols += (cols.empty() ? "" : ", ") + inner->column(j.col[1 - outer])->name;
  no_plan("no index of " + inner->name + " joins on " + cols);
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
          return select(*s.table, s, out.computed);
        } else if constexpr (std::is_same_v<T, bound::Join>) {
          table(*s.table);
          auto second = Catalog::constants(*s.second);
          out.constants.insert(out.constants.end(), second.begin(), second.end());
          return join(s, out.computed);
        } else if constexpr (std::is_same_v<T, bound::Update>) {
          const auto& t = *s.table;
          table(t);
          auto [rows, f] = write_rows(t, s.where, out.computed);
          ir::Update up{rows, {}, std::move(f)};
          for (const auto& [id, e] : s.set) {
            ir::Expr v;
            switch (e.kind) {
              case bound::Expr::Kind::Param: v = {ir::Expr::Kind::Param, e.param, {}}; break;
              case bound::Expr::Kind::Column: v = {ir::Expr::Kind::Column, 0, column(t, e.col_id)}; break;
              // Only a BIGINT read can overflow int64: a DECIMAL of scale 0 has
              // BIGINT's physical type, but its operand is held to 18 digits.
              case bound::Expr::Kind::Add:
              case bound::Expr::Kind::Sub:
                v = {e.kind == bound::Expr::Kind::Add ? ir::Expr::Kind::Add : ir::Expr::Kind::Sub, e.param,
                     column(t, e.col_id), e.constant, t.column(e.col_id)->sql.kind == SqlKind::BigInt};
                break;
            }
            up.set.emplace_back(column(t, id), v);
          }
          return up;
        } else if constexpr (std::is_same_v<T, bound::Delete>) {
          const auto& t = *s.table;
          table(t);
          auto [rows, f] = write_rows(t, s.where, out.computed);
          return ir::Delete{rows, std::move(f)};
        } else {
          throw SqlError{"DDL has no plan", -1};
        }
      },
      b.stmt);
  return out;
}

}  // namespace bytecask::sql
