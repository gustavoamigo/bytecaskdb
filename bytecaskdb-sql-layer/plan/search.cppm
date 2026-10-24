// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// search.cppm — the search (docs/planner_design.md, *The search*): a
// rewritten logical plan to the physical plan that computes it. Slice 1 of
// the design: only the access paths and the choices the MVP's planner made,
// with no cost model, so every statement plans as it did. The rules:
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
// No full scan and no sort: a statement that needs one is refused.

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

export module bytecask.sql.search;

export import bytecask.sql.physical;

export namespace bytecask::sql {

// Throws ast::SqlError (position -1) when no rule applies.
[[nodiscard]] auto search(const logical::Node& root, const Catalog& cat) -> physical::Plan;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

using ast::SqlError;
using logical::ColumnRef;
using logical::Expr;
using Op = ast::Compare::Op;
using Conjuncts = std::vector<Expr>;

[[noreturn]] void no_plan(const std::string& why) { throw SqlError{"no plan: " + why, -1}; }

// A WHERE conjunct, as the binder builds it: column <op> operand, the
// operand a parameter, $n ± k, or a column of the other table.
auto col_of(const Expr& c) -> const ColumnRef& { return c.args[0].col; }
auto operand_of(const Expr& c) -> const Expr& { return c.args[1]; }
auto is_compare(const Expr& c) -> bool { return c.kind == Expr::Kind::Compare; }
// The parameter it compares with: the operand's, or $n's in $n ± k.
auto param_of(const Expr& c) -> std::optional<std::uint32_t> {
  if (!is_compare(c)) return std::nullopt;
  const auto& o = operand_of(c);
  if (o.kind == Expr::Kind::Param) return o.param;
  if (o.kind == Expr::Kind::Add || o.kind == Expr::Kind::Sub) return o.args[0].param;
  return std::nullopt;
}
// An equality the key can take: with a parameter, not a computed value.
auto key_eq(const Expr& c) -> bool {
  return is_compare(c) && c.op == Op::Eq && operand_of(c).kind == Expr::Kind::Param;
}
auto on_col(const Expr& c, std::uint32_t col) -> bool { return is_compare(c) && col_of(c).col == col; }

auto column(const Table& t, std::uint32_t id) -> ColumnRef {
  const auto& c = *t.column(id);
  return {t.id, c.id, c.type, c.nullable};
}

auto column_expr(const Table& t, std::uint32_t id) -> Expr { return logical::column(column(t, id), t.column(id)->sql); }

// $n ± k's place in the plan's computed values, added if new.
void add_computed(std::vector<physical::Computed>& comp, const Expr& c, std::uint32_t table) {
  if (!is_compare(c)) return;
  const auto& o = operand_of(c);
  if (o.kind != Expr::Kind::Add && o.kind != Expr::Kind::Sub) return;
  auto v = physical::computed_of(o, table);
  if (std::ranges::find(comp, v) == comp.end()) comp.push_back(v);
}

void add_param(std::vector<std::uint32_t>& to, std::uint32_t p) {
  if (std::ranges::find(to, p) == to.end()) to.push_back(p);
}

// The conjuncts of `where` not in `used`, as filters. A DOUBLE or REAL
// column has none yet: its comparisons need PostgreSQL's NaN and -0.0
// rules, which only its key encoding has.
auto filters(const Table& t, const Conjuncts& where, const std::vector<const Expr*>& used,
             std::vector<physical::Computed>& comp) -> Conjuncts {
  Conjuncts out;
  for (const auto& c : where) {
    if (std::ranges::find(used, &c) != used.end()) continue;
    if (is_compare(c) && col_of(c).type == PhysType::F64) {
      no_plan("a filter on column \"" + t.column(col_of(c).col)->name + "\", a REAL or DOUBLE PRECISION, is not supported yet");
    }
    add_computed(comp, c, t.id);
    out.push_back(c);
  }
  return out;
}

// The whole primary key by equality: the lookup and the conjuncts it uses.
struct Keyed {
  std::vector<Expr> key;
  std::vector<const Expr*> used;
};

// The parameter each primary key column is compared equal to, or nullopt
// if rule 1 does not apply.
auto point_get(const Table& t, const Conjuncts& where) -> std::optional<Keyed> {
  Keyed k;
  for (auto id : t.pk_col_ids) {
    auto it = std::ranges::find_if(where, [&](const auto& c) { return on_col(c, id) && key_eq(c); });
    if (it == where.end()) return std::nullopt;
    k.key.push_back(operand_of(*it));
    k.used.push_back(&*it);
  }
  return k;
}

auto key_params(const std::vector<Expr>& key) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  for (const auto& e : key) out.push_back(e.param);
  return out;
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

struct OrderItem {
  std::uint32_t col;
  bool desc;
};

// What a path makes of the WHERE: the equality run, the range on the part
// after it, and whether its order gives the ORDER BY (and in which
// direction).
struct Choice {
  const Path* path;
  std::vector<const Expr*> eq;  // one per part of the run
  const Expr* lo{nullptr};      // > or >= on the part after the run
  const Expr* hi{nullptr};      // < or <=
  bool serves{true};
  bool reverse{false};
  bool covering{false};
};

auto choose(const Path& p, const Conjuncts& where, const std::vector<OrderItem>& ordered) -> Choice {
  Choice c{&p, {}};
  auto first = [&](std::uint32_t col, auto pred) -> const Expr* {
    auto it = std::ranges::find_if(where, [&](const Expr& w) { return on_col(w, col) && pred(w.op); });
    return it == where.end() ? nullptr : &*it;
  };
  for (const auto& part : p.parts) {
    auto it = std::ranges::find_if(where, [&](const Expr& w) { return on_col(w, part.col) && key_eq(w); });
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
    if (part.col != ordered[i].col || (i > 0 && rev != c.reverse)) {
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

auto names(const Table& t, const std::vector<OrderItem>& items) -> std::string {
  std::string out;
  for (const auto& o : items) out += (out.empty() ? "" : ", ") + t.column(o.col)->name;
  return out;
}

// The rows of a table the WHERE matches, by the path chosen, and the
// conjuncts that path does not express.
struct Rows {
  std::variant<physical::PointGet, physical::Scan, physical::NullSplitScan> path;
  Conjuncts filters;
  std::vector<std::uint32_t> null_checks;  // the parameters the WHERE compares, as the path checks them
};

// The rows of `t` the WHERE matches, in the ORDER BY's order. `used_cols`
// are the columns read from each; with `may_cover`, the scan reads them
// from the entry's key when it holds them all, unless a filter reads the row.
auto rows_of(const Table& t, const Conjuncts& where, const std::vector<OrderItem>& order_by,
             const std::vector<ColumnRef>& used_cols, bool may_cover, std::vector<physical::Computed>& comp) -> Rows {
  auto value_bound = [&](const Expr* c, bool successor) -> physical::KeyBound {
    if (c == nullptr) return {};
    add_computed(comp, *c, t.id);
    return {physical::KeyBound::Kind::Value, operand_of(*c), successor};
  };
  auto null_part = [](bool successor) { return physical::KeyBound{physical::KeyBound::Kind::NullPart, std::nullopt, successor}; };
  // A point lookup returns at most one row: it gives any ORDER BY.
  if (auto k = point_get(t, where)) {
    auto params = key_params(k->key);
    auto f = filters(t, where, k->used, comp);
    return {physical::PointGet{t.id, std::move(k->key), false}, std::move(f), std::move(params)};
  }

  // An ordered column compared for equality is constant in the result.
  std::vector<OrderItem> ordered;
  for (const auto& o : order_by) {
    auto constant = std::ranges::any_of(where, [&](const auto& w) { return on_col(w, o.col) && w.op == Op::Eq; });
    auto repeated = std::ranges::any_of(ordered, [&](const auto& q) { return q.col == o.col; });
    if (!constant && !repeated) ordered.push_back(o);
  }

  auto all = paths(t);
  std::vector<Choice> usable;
  for (const auto& p : all) {
    auto c = choose(p, where, ordered);
    if (c.eq.empty() && c.lo == nullptr && c.hi == nullptr) continue;
    c.covering = may_cover && std::ranges::all_of(used_cols, [&](const ColumnRef& col) { return in_key(t, p, col.col); });
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

  std::vector<const Expr*> used = best.eq;
  for (const auto* b : {best.lo, best.hi}) {
    if (b != nullptr) used.push_back(b);
  }
  auto residual = filters(t, where, used, comp);

  physical::Scan out;
  out.table = t.id;
  out.object = best.path->object;
  for (const auto* e : best.eq) out.prefix.push_back(operand_of(*e));
  std::vector<std::uint32_t> null_checks;
  for (const auto& w : where) {
    if (auto p = param_of(w)) add_param(null_checks, *p);
  }
  out.reverse = best.reverse;
  out.covering = best.covering && residual.empty();
  const auto run = best.eq.size();

  // NULLs in the ordered parts: a part the WHERE compares holds none in the
  // rows it matches. The key order puts an ASC part's NULLs first and a DESC
  // part's last, PostgreSQL the other way round in both directions, so only
  // a nullable ASC part first after the run is served: by two ranges.
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    const auto* col = t.column(ordered[i].col);
    auto compared = std::ranges::any_of(where, [&](const auto& w) { return on_col(w, col->id); });
    if (!col->nullable || compared) continue;
    auto later_nullable = std::ranges::any_of(ordered.begin() + static_cast<std::ptrdiff_t>(i) + 1, ordered.end(),
                                              [&](const auto& o) { return t.column(o.col)->nullable; });
    if (i != 0 || best.path->parts[run].desc || later_nullable) {
      no_plan("ORDER BY " + col->name + " would need its NULLs reordered");
    }
    out.rest = physical::Range{null_part(true), {}};
    return {physical::NullSplitScan{std::move(out)}, std::move(residual), std::move(null_checks)};
  }

  if (best.lo == nullptr && best.hi == nullptr) return {std::move(out), std::move(residual), std::move(null_checks)};
  // A key bound: the compared value's key part, or the part that holds NULL,
  // or open; as is or its successor. A key part is self-delimiting and the
  // rest of the key follows it, so a bound that must pass every entry with
  // the part equal to a value is that value's successor.
  const auto& part = best.path->parts[run];
  auto nullable = t.column(part.col)->nullable;
  const auto* lo = best.lo;
  const auto* hi = best.hi;
  physical::Range r;
  if (!part.desc) {
    // ASC: key order is value order, nulls first. A comparison with NULL is
    // never true, so with no lower bound the scan starts past the null
    // entries, at the successor of the null part.
    r.lo = lo == nullptr && nullable ? null_part(true) : value_bound(lo, lo != nullptr && lo->op == Op::Gt);
    r.hi = value_bound(hi, hi != nullptr && hi->op == Op::Le);
  } else {
    // DESC: key order is descending value order, nulls last. The upper value
    // bound gives the lower key bound and the lower value bound the upper;
    // with no lower value bound the scan stops before the null entries, at
    // the null part.
    r.lo = value_bound(hi, hi != nullptr && hi->op == Op::Lt);
    r.hi = lo == nullptr && nullable ? null_part(false) : value_bound(lo, lo != nullptr && lo->op == Op::Ge);
  }
  out.rest = r;
  return {std::move(out), std::move(residual), std::move(null_checks)};
}

// MIN(arg) from the scan's first entry: arg is the ASC part after its
// prefix, so the first entry past that part's NULLs holds the least value.
// A scan with no range on the part is given one from past its NULLs.
auto min_from_first_entry(const Table& t, physical::Scan& sc, const ColumnRef& arg) -> bool {
  const auto all = paths(t);
  const auto& path = *std::ranges::find_if(all, [&](const Path& p) { return p.object == sc.object; });
  if (sc.prefix.size() >= path.parts.size()) return false;
  const auto& part = path.parts[sc.prefix.size()];
  if (part.col != arg.col || part.desc) return false;
  if (arg.nullable && std::holds_alternative<std::monostate>(sc.rest)) {
    sc.rest = physical::Range{{physical::KeyBound::Kind::NullPart, std::nullopt, true}, {}};
  }
  return true;
}

// The logical plan's parts, as slice 1 plans them: a SELECT is
// Limit? -> Project | Aggregate -> Sort? -> Filter? -> Get, or a Join of two
// Filter? -> Get; a write is over Filter? -> Get.
struct Table1 {
  const logical::Get* get{nullptr};
  Conjuncts where;
};

auto table_of(const logical::Node& n) -> Table1 {
  if (const auto* f = std::get_if<logical::Filter>(&n.op)) {
    auto t = table_of(*f->input);
    t.where = f->conjuncts;
    return t;
  }
  return {&std::get<logical::Get>(n.op), {}};
}

auto is_count(const Expr& a) -> bool { return a.fn == Expr::Fn::Count; }

// A SELECT's result rows over the rows of one table, or one aggregate row.
struct SelectParts {
  std::optional<std::int64_t> limit;
  const logical::Project* project{nullptr};
  const logical::Aggregate* aggregate{nullptr};
  const logical::Sort* sort{nullptr};
  const logical::Node* below{nullptr};  // what Project or Aggregate reads: its output, the columns read
  const logical::Node* source{nullptr};  // Filter? -> Get, or a Join
};

auto select_parts(const logical::Node& root) -> SelectParts {
  SelectParts s;
  const auto* n = &root;
  if (const auto* l = std::get_if<logical::Limit>(&n->op)) {
    s.limit = l->n;
    n = l->input.get();
  }
  if (const auto* p = std::get_if<logical::Project>(&n->op)) {
    s.project = p;
    n = p->input.get();
  } else {
    s.aggregate = &std::get<logical::Aggregate>(n->op);
    n = s.aggregate->input.get();
  }
  s.below = n;
  if (const auto* o = std::get_if<logical::Sort>(&n->op)) {
    s.sort = o;
    n = o->input.get();
  }
  s.source = n;
  return s;
}

class Search {
public:
  explicit Search(physical::Plan& out) : out_{out} {}

  auto statement(const logical::Node& root) -> physical::NodePtr {
    return std::visit(
        [&](const auto& op) -> physical::NodePtr {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, logical::Insert>) {
            constants(*op.table);
            physical::Insert ins{op.table->id, {}};
            for (std::size_t i = 0; i < op.table->columns.size(); ++i) {
              if (op.values[i]) ins.values.emplace_back(column(*op.table, op.table->columns[i].id), *op.values[i]);
            }
            return physical::node(std::move(ins));
          } else if constexpr (std::is_same_v<T, logical::Update>) {
            constants(*op.table);
            return physical::node(physical::Update{write_rows(*op.input), op.table->id, op.set});
          } else if constexpr (std::is_same_v<T, logical::Delete>) {
            constants(*op.table);
            return physical::node(physical::Delete{write_rows(*op.input), op.table->id});
          } else {
            return select(root);
          }
        },
        root.op);
  }

private:
  void constants(const Table& t) {
    auto c = Catalog::constants(t);
    out_.constants.insert(out_.constants.end(), c.begin(), c.end());
  }

  // A leaf's rows, under a Filter of the conjuncts its key does not express.
  static auto filtered(physical::NodePtr leaf, Conjuncts f) -> physical::NodePtr {
    if (f.empty()) return leaf;
    return physical::node(physical::Filter{std::move(leaf), std::move(f)});
  }

  // The rows an UPDATE or DELETE writes: by a point lookup, or a scan, which
  // reads each row whole; and the filters on them.
  auto write_rows(const logical::Node& in) -> physical::NodePtr {
    auto t = table_of(in);
    auto rows = rows_of(*t.get->table, t.where, {}, {}, false, out_.computed);
    out_.null_checks = std::move(rows.null_checks);
    auto leaf = std::visit([](auto&& p) { return physical::node(std::move(p)); }, std::move(rows.path));
    return filtered(std::move(leaf), std::move(rows.filters));  // no ORDER BY, so never split
  }

  // Emit, under LIMIT, or the aggregate's row.
  auto output(const SelectParts& s, physical::NodePtr rows, std::vector<ColumnRef> targets) -> physical::NodePtr {
    if (s.limit) rows = physical::node(physical::Limit{std::move(rows), *s.limit});
    return physical::node(physical::Emit{std::move(rows), std::move(targets)});
  }

  void result(const SelectParts& s) {
    const auto& outs = s.project != nullptr ? s.project->exprs : s.aggregate->aggs;
    for (const auto& o : outs) out_.result.push_back({o.name, o.expr.type, o.expr.phys()});
  }

  auto select(const logical::Node& root) -> physical::NodePtr {
    auto s = select_parts(root);
    result(s);
    if (const auto* j = std::get_if<logical::Join>(&s.source->op)) return join(s, *j);
    auto [get, where] = table_of(*s.source);
    const auto& t = *get->table;
    constants(t);
    std::vector<OrderItem> order_by;
    if (s.sort != nullptr) {
      for (const auto& k : s.sort->keys) order_by.push_back({k.expr.col.col, k.desc});
    }
    const auto* agg = s.aggregate != nullptr ? &s.aggregate->aggs[0].expr : nullptr;
    // FOR UPDATE reads each row, whose key it guards; COUNT(DISTINCT) keys
    // each value from its row.
    auto distinct = agg != nullptr && agg->distinct;
    auto rows = rows_of(t, where, order_by, s.below->output, !get->for_update && !distinct, out_.computed);
    out_.null_checks = std::move(rows.null_checks);
    std::visit(
        [&](auto& p) {
          if constexpr (std::is_same_v<std::decay_t<decltype(p)>, physical::NullSplitScan>) {
            p.present.for_update = get->for_update;
          } else {
            p.for_update = get->for_update;
          }
        },
        rows.path);
    auto emits = !s.limit || *s.limit > 0;
    if (agg == nullptr) {
      std::vector<ColumnRef> targets;
      for (const auto& o : s.project->exprs) targets.push_back(o.expr.col);
      auto leaf = std::visit([](auto&& p) { return physical::node(std::move(p)); }, std::move(rows.path));
      return output(s, filtered(std::move(leaf), std::move(rows.filters)), std::move(targets));
    }
    // The keys answer an aggregate only when no filter drops a row.
    auto* scan = rows.filters.empty() ? std::get_if<physical::Scan>(&rows.path) : nullptr;
    if (scan != nullptr && !agg->distinct && is_count(*agg)) {
      // Every entry of the range is a row to count, unless the argument can
      // be NULL in one: the WHERE compares every column it names, so those
      // hold none.
      const auto* arg = agg->args.empty() ? nullptr : &agg->args[0].col;
      auto may_be_null = arg != nullptr && arg->nullable &&
                         std::ranges::none_of(where, [&](const auto& w) { return on_col(w, arg->col); });
      if (!may_be_null) return physical::node(physical::CountRange{std::move(*scan), emits});
    } else if (scan != nullptr && !agg->distinct && agg->fn == Expr::Fn::Min &&
               min_from_first_entry(t, *scan, agg->args[0].col)) {
      return physical::node(physical::MinFirstEntry{std::move(*scan), agg->args[0].col, emits});
    }
    auto leaf = std::visit([](auto&& p) { return physical::node(std::move(p)); }, std::move(rows.path));
    return physical::node(physical::Aggregate{filtered(std::move(leaf), std::move(rows.filters)), *agg, emits});
  }

  // Whether some key of `t` is reachable from its parameters alone: a path
  // whose first part is compared for equality or bounded.
  static auto reachable(const Table& t, const Conjuncts& where) -> bool {
    const auto all = paths(t);
    return std::ranges::any_of(all, [&](const Path& p) {
      auto c = choose(p, where, {});
      return !c.eq.empty() || c.lo != nullptr || c.hi != nullptr;
    });
  }

  // The inner table's whole primary key, each part from a parameter it is
  // compared equal to or an outer column it is joined on; and what that takes.
  struct InnerKey {
    std::vector<Expr> parts;
    std::vector<const Expr*> used;
    std::vector<const Expr*> on;
  };

  // The column of a join conjunct on `table`'s side, and the other side's.
  static auto side_col(const Expr& on, std::uint32_t table) -> const ColumnRef& {
    return on.args[0].col.table == table ? on.args[0].col : on.args[1].col;
  }
  static auto other_col(const Expr& on, std::uint32_t table) -> const ColumnRef& {
    return on.args[0].col.table == table ? on.args[1].col : on.args[0].col;
  }

  static auto inner_key(const Table* const (&sides)[2], const Conjuncts (&where)[2], const Conjuncts& on,
                        std::uint32_t inner) -> std::optional<InnerKey> {
    const auto outer = 1 - inner;
    InnerKey k;
    for (auto id : sides[inner]->pk_col_ids) {
      auto it = std::ranges::find_if(where[inner], [&](const auto& c) { return on_col(c, id) && key_eq(c); });
      if (it != where[inner].end()) {
        k.parts.push_back(operand_of(*it));
        k.used.push_back(&*it);
        continue;
      }
      auto jt = std::ranges::find_if(on, [&](const Expr& j) { return side_col(j, sides[inner]->id).col == id; });
      if (jt == on.end()) return std::nullopt;
      k.parts.push_back(column_expr(*sides[outer], other_col(*jt, sides[inner]->id).col));
      k.on.push_back(&*jt);
    }
    return k;
  }

  // Two tables (docs/frontend_design.md, *Joins*). With no column of one
  // equal to one of the other, each by its whole primary key, in FROM order.
  // With one, an index nested loop: the outer table reached from its
  // parameters alone, the inner one read by its whole primary key from
  // parameters and the outer row's columns; FROM order first, then the
  // other, also when the first has a filter it cannot serve. Every other
  // conjunct is a filter on the row it names.
  auto join(const SelectParts& s, const logical::Join& j) -> physical::NodePtr {
    if (s.sort != nullptr) no_plan("ORDER BY on a join");
    auto left = table_of(*j.left);
    auto right = table_of(*j.right);
    const Table* const sides[2]{left.get->table, right.get->table};
    const Conjuncts where[2]{left.where, right.where};
    const auto& on = j.predicate;
    constants(*sides[0]);
    constants(*sides[1]);
    std::vector<ColumnRef> targets;
    if (s.project != nullptr) {
      for (const auto& o : s.project->exprs) targets.push_back(o.expr.col);
    }
    const auto* agg = s.aggregate != nullptr ? &s.aggregate->aggs[0].expr : nullptr;
    auto emits = !s.limit || *s.limit > 0;
    // The pairs, as the select list takes them.
    auto finish = [&](physical::NodePtr pairs) {
      if (agg != nullptr) return physical::node(physical::Aggregate{std::move(pairs), *agg, emits});
      return output(s, std::move(pairs), targets);
    };
    if (on.empty()) {
      Keyed keys[2];
      Conjuncts f;
      for (std::size_t i = 0; i < 2; ++i) {
        auto k = point_get(*sides[i], where[i]);
        if (!k) no_plan("a join must name the row of each table by its whole primary key yet");
        auto more = filters(*sides[i], where[i], k->used, out_.computed);
        f.insert(f.end(), more.begin(), more.end());
        keys[i] = std::move(*k);
      }
      out_.null_checks = key_params(keys[0].key);
      for (auto p : key_params(keys[1].key)) add_param(out_.null_checks, p);
      auto outer = physical::node(physical::PointGet{sides[0]->id, std::move(keys[0].key), false});
      auto pairs = physical::node(physical::IndexJoin{std::move(outer), sides[1]->id, std::move(keys[1].key)});
      return finish(filtered(std::move(pairs), std::move(f)));
    }
    // One orientation; a filter it cannot serve refuses it, and the other is
    // tried before the refusal stands.
    auto loop_join = [&](std::uint32_t outer, InnerKey k, std::vector<physical::Computed>& into) -> physical::NodePtr {
      const auto inner = 1 - outer;
      auto rows = rows_of(*sides[outer], where[outer], {}, {}, false, into);
      physical::Scan scan;
      std::vector<std::uint32_t> null_checks;
      if (auto* g = std::get_if<physical::PointGet>(&rows.path)) {
        // One outer row: a scan of its whole key's prefix, so one loop serves.
        scan.table = scan.object = sides[outer]->id;
        scan.prefix = std::move(g->key);
      } else {
        scan = std::move(std::get<physical::Scan>(rows.path));  // no ORDER BY, so never split
        null_checks = std::move(rows.null_checks);
      }
      // A NULL parameter on either side matches no pair.
      for (const auto& w : where) {
        for (const auto& c : w) {
          if (auto p = param_of(c)) add_param(null_checks, *p);
        }
      }
      auto inner_filters = filters(*sides[inner], where[inner], k.used, into);
      for (const auto& c : on) {
        if (std::ranges::find(k.on, &c) != k.on.end()) continue;
        const auto& col = side_col(c, sides[inner]->id);
        if (col.type == PhysType::F64) {
          no_plan("a filter on column \"" + sides[inner]->column(col.col)->name +
                  "\", a REAL or DOUBLE PRECISION, is not supported yet");
        }
        inner_filters.push_back(logical::compare(Op::Eq, column_expr(*sides[inner], col.col),
                                                 column_expr(*sides[outer], other_col(c, sides[inner]->id).col)));
      }
      out_.null_checks = std::move(null_checks);
      auto outer_rows = filtered(physical::node(std::move(scan)), std::move(rows.filters));
      auto pairs = physical::node(physical::IndexJoin{std::move(outer_rows), sides[inner]->id, std::move(k.parts)});
      return filtered(std::move(pairs), std::move(inner_filters));
    };
    std::optional<SqlError> refused;
    for (std::uint32_t outer : {0u, 1u}) {
      if (!reachable(*sides[outer], where[outer])) continue;
      auto k = inner_key(sides, where, on, 1 - outer);
      if (!k) continue;
      auto tried = out_.computed;
      physical::NodePtr pairs;
      try {
        pairs = loop_join(outer, std::move(*k), tried);
      } catch (const SqlError& e) {
        if (!refused) refused = e;
        continue;
      }
      out_.computed = std::move(tried);
      return finish(std::move(pairs));
    }
    if (refused) throw *refused;
    auto outer = reachable(*sides[0], where[0]) ? 0u : reachable(*sides[1], where[1]) ? 1u : 2u;
    if (outer == 2) no_plan("a full scan of " + sides[0]->name);
    const auto* inner = sides[1 - outer];
    std::string cols;
    for (const auto& c : on) cols += (cols.empty() ? "" : ", ") + inner->column(side_col(c, inner->id).col)->name;
    no_plan("no index of " + inner->name + " joins on " + cols);
  }

  physical::Plan& out_;
};

}  // namespace

auto search(const logical::Node& root, const Catalog& cat) -> physical::Plan {
  physical::Plan out;
  out.catalog_version = cat.version();
  out.root = Search{out}.statement(root);
  return out;
}

}  // namespace bytecask::sql
