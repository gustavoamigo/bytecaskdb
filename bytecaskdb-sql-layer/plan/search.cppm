// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// search.cppm — the search (docs/planner_design.md, *The search*): a
// rewritten logical plan to the physical plan that computes it, by System R's
// bottom-up dynamic programming over access paths and left-deep join orders,
// each plan costed in the work budget's unit (*The cost model*).
//   1. Access paths, per table: a point lookup on the whole primary key; a
//      scan of the primary key or an index, its bounds an equality prefix of
//      parameters and a range on the next part, forward or reverse, covering
//      or not. No full scan: a table no key reaches has no access path.
//   2. Joins: each set of tables from the sets one smaller, the new table
//      read by its whole primary key from parameters and the other tables'
//      columns (an index nested loop), or, under at most one row, by its
//      own scan from parameters alone, read once, the join's conjuncts
//      filtering its rows (a nested loop); keeping the cheapest plan per
//      set and per interesting order. A plan's order is its first table's
//      scan's, or, under one row, the scan read once; a key lookup per row
//      keeps it.
//   3. The statement on top: the rows emitted, under LIMIT, or aggregated,
//      COUNT and MIN also answered from the keys (bc_count_range, a scan's
//      first entry) where nothing filters the rows.
// The interesting order is the ORDER BY's, NULL placement included: no sort
// exists yet, so a statement whose order no plan gives is refused. A
// conjunct a key does not express is a filter on the row it reads; an
// equality with $n ± k is always one. A conjunct always false leaves an
// Empty input, planned as no rows.

module;
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.search;

export import bytecask.sql.physical;

export namespace bytecask::sql {

// A table's row count, which the search costs plans with: bc_count_range over
// its primary key, read on the database's snapshot when the plan is compiled.
// nullopt, or no function, for a table nothing counted.
using RowCounts = std::function<std::optional<std::uint64_t>(const Table&)>;

// The cost model's constants (docs/planner_design.md, *The cost model*). A
// table nothing counted, as when a golden plan is compiled with no database,
// is costed at kUncountedRows: large enough that a key read beats a loop, as
// it does on any table where the choice matters. The selectivities are
// System R's, for bounds that depend on $n; checked on the harness
// (*Decisions*, 9).
inline constexpr double kUncountedRows = 1'000'000;
inline constexpr double kEqSelectivity = 1.0 / 10;
inline constexpr double kRangeSelectivity = 1.0 / 3;

// The cheapest plan for a rewritten logical plan, its cost in Plan::cost.
// Throws FeatureNotSupported, its message starting "no plan:", when no plan
// computes it or the logical plan has a shape the search does not plan.
[[nodiscard]] auto search(const logical::Node& root, const Catalog& cat, const RowCounts& counts = {})
    -> physical::Plan;

// Every plan the search costs for the statement, none pruned, cheapest first
// (*Tests*, alternatives agree). Refuses what search refuses.
[[nodiscard]] auto search_all(const logical::Node& root, const Catalog& cat, const RowCounts& counts = {})
    -> std::vector<physical::Plan>;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

using ast::FeatureNotSupported;
using logical::ColumnRef;
using logical::Expr;
using Op = ast::Compare::Op;
using Conjuncts = std::vector<Expr>;

[[noreturn]] void no_plan(const std::string& why) { throw FeatureNotSupported{"no plan: " + why}; }

// An operator's name, for a refusal of a shape the search does not plan.
auto op_name(const logical::Node& n) -> std::string {
  constexpr const char* kNames[] = {"Get",  "Empty", "Filter", "Project", "Join",  "Aggregate",
                                    "Sort", "Limit", "Insert", "Update",  "Delete"};
  static_assert(std::size(kNames) == std::variant_size_v<decltype(n.op)>);
  return kNames[n.op.index()];
}

// "a Filter", "an Empty".
auto with_article(const std::string& name) -> std::string {
  return (name.find_first_of("AEIOU") == 0 ? "an " : "a ") + name;
}

// A usable conjunct (use_of): column <op> operand, the operand a parameter,
// $n ± k, or a column of another table.
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

// The operator of c with its operands swapped: a < b is b > a.
auto mirrored(Op op) -> Op {
  switch (op) {
    case Op::Eq: return Op::Eq;
    case Op::Lt: return Op::Gt;
    case Op::Le: return Op::Ge;
    case Op::Gt: return Op::Lt;
    case Op::Ge: return Op::Le;
  }
  return op;
}

// The fraction of rows a conjunct keeps, as the cost model assumes it.
auto selectivity(const Expr& c) -> double { return c.op == Op::Eq ? kEqSelectivity : kRangeSelectivity; }
auto selectivity(const Conjuncts& cs) -> double {
  auto s = 1.0;
  for (const auto& c : cs) s *= selectivity(c);
  return s;
}

// An operand's $n ± k, compared with a column of `table`: its place in the
// plan's computed values, added if new.
void add_computed(std::vector<physical::Computed>& comp, const Expr& o, std::uint32_t table) {
  if (o.kind != Expr::Kind::Add && o.kind != Expr::Kind::Sub) return;
  auto v = physical::computed_of(o, table);
  if (std::ranges::find(comp, v) == comp.end()) comp.push_back(v);
}

void add_param(std::vector<std::uint32_t>& to, std::uint32_t p) {
  if (std::ranges::find(to, p) == to.end()) to.push_back(p);
}

// ---------------------------------------------------------------- conjuncts

using Tables = std::vector<std::uint32_t>;

auto reads(const Tables& ts, const Expr& e) -> bool {
  return e.kind == Expr::Kind::Column && std::ranges::find(ts, e.col.table) != ts.end();
}

enum class Use { Ok, NoColumn, Operand, Columns };

// The one test of a conjunct the search can use, on a table's Filter and in
// a Join's predicate alike: a column of `here` compared with a parameter, with
// $n ± k, or with a column of `other`. For a Filter `other` is empty; for a
// Join's predicate `here` is the side the conjunct's first column is on and
// `other` the other side, so a column operand is accepted only from the
// other side of a join, only in its predicate.
auto use_of(const Expr& c, const Tables& here, const Tables& other) -> Use {
  if (!is_compare(c) || !reads(here, c.args[0])) return Use::NoColumn;
  const auto& o = operand_of(c);
  switch (o.kind) {
    case Expr::Kind::Param: return Use::Ok;
    case Expr::Kind::Add:
    case Expr::Kind::Sub:
      return o.args[0].kind == Expr::Kind::Param && o.args[1].kind == Expr::Kind::Const ? Use::Ok : Use::Operand;
    case Expr::Kind::Column: return reads(other, o) ? Use::Ok : Use::Columns;
    case Expr::Kind::Const:
    case Expr::Kind::Compare:
    case Expr::Kind::Aggregate: return Use::Operand;
  }
  return Use::Operand;
}

// ------------------------------------------------------------- access paths

// The whole primary key by equality: the lookup and the conjuncts it uses.
struct Keyed {
  std::vector<Expr> key;
  std::vector<const Expr*> used;
};

// The first conjunct of `where` that compares `col` equal to a parameter, or
// null.
auto key_eq_on(const Conjuncts& where, std::uint32_t col) -> const Expr* {
  auto it = std::ranges::find_if(where, [&](const Expr& c) { return on_col(c, col) && key_eq(c); });
  return it == where.end() ? nullptr : &*it;
}

// The parameter each primary key column is compared equal to, or nullopt.
auto point_get(const Table& t, const Conjuncts& where) -> std::optional<Keyed> {
  Keyed k;
  for (auto id : t.pk_col_ids) {
    const auto* c = key_eq_on(where, id);
    if (c == nullptr) return std::nullopt;
    k.key.push_back(operand_of(*c));
    k.used.push_back(c);
  }
  return k;
}

// Every parameter `where` compares, added to `to`: the ones a NULL of which
// matches no row.
void add_params(std::vector<std::uint32_t>& to, const Conjuncts& where) {
  for (const auto& w : where) {
    if (auto p = param_of(w)) add_param(to, *p);
  }
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
// index names among them (the catalog's key layout). Its first `unique`
// parts name at most one row.
struct Path {
  std::uint32_t object;
  bool is_pk;
  std::vector<Part> parts;
  std::size_t unique;
};

auto paths(const Table& t) -> std::vector<Path> {
  std::vector<Path> out;
  Path pk{t.id, true, {}, t.pk_col_ids.size()};
  for (auto id : t.pk_col_ids) pk.parts.push_back({id, false});
  out.push_back(pk);
  for (const auto& ix : t.indexes) {
    Path p{ix.id, false, {}, 0};
    for (const auto& part : ix.parts) p.parts.push_back({part.col_id, part.desc});
    for (auto id : t.pk_col_ids) p.parts.push_back({id, false});
    p.unique = ix.unique ? ix.parts.size() : p.parts.size();
    out.push_back(std::move(p));
  }
  return out;
}

struct OrderItem {
  ColumnRef col;
  bool desc;
};

// What a path makes of the WHERE: the equality run and the range on the part
// after it, each the first such conjunct in WHERE order.
struct Bounds {
  std::vector<const Expr*> eq;  // one per part of the run
  const Expr* lo{nullptr};      // > or >= on the part after the run
  const Expr* hi{nullptr};      // < or <=
};

auto bounds(const Path& p, const Conjuncts& where) -> Bounds {
  Bounds b;
  auto first = [&](std::uint32_t col, auto pred) -> const Expr* {
    auto it = std::ranges::find_if(where, [&](const Expr& w) { return on_col(w, col) && pred(w.op); });
    return it == where.end() ? nullptr : &*it;
  };
  for (const auto& part : p.parts) {
    const auto* c = key_eq_on(where, part.col);
    if (c == nullptr) break;
    b.eq.push_back(c);
  }
  auto run = b.eq.size();
  if (run < p.parts.size()) {
    b.lo = first(p.parts[run].col, [](Op op) { return op == Op::Gt || op == Op::Ge; });
    b.hi = first(p.parts[run].col, [](Op op) { return op == Op::Lt || op == Op::Le; });
  }
  return b;
}

// Whether the path, its first `run` parts compared for equality, gives the
// ordered columns of `table`: the parts after the run, in order, each in the
// part's direction (a forward scan) or each against it (a reverse one).
// nullopt if not; else whether in reverse.
auto direction(const Path& p, std::size_t run, std::uint32_t table, const std::vector<OrderItem>& ordered)
    -> std::optional<bool> {
  if (run + ordered.size() > p.parts.size()) return std::nullopt;
  auto reverse = false;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    const auto& part = p.parts[run + i];
    auto rev = ordered[i].desc != part.desc;
    if (ordered[i].col.table != table || part.col != ordered[i].col.col || (i > 0 && rev != reverse)) {
      return std::nullopt;
    }
    reverse = rev;
  }
  return reverse;
}

auto in_key(const Table& t, const Path& p, std::uint32_t col) -> bool {
  // Not a DOUBLE: the key encoding writes -0.0 as 0.0 and every NaN as one,
  // so only the row holds its exact value.
  if (t.column(col)->type == PhysType::F64) return false;
  return std::ranges::any_of(p.parts, [&](const Part& q) { return q.col == col; });
}

// Where a plan stopped. A plan reaches its tables (Reach), gives the ORDER
// BY (Order), places its NULLs (Nulls), and takes every filter (Filter).
enum class Stage { Reach, Order, Nulls, Filter };

// Why one plan failed: the stage it stopped at.
struct Refusal {
  Stage stage{Stage::Reach};
  std::string why;
};

// The failure of a plan made of two parts: the one at the earlier stage,
// `a` of equals.
auto earlier(const std::optional<Refusal>& a, const std::optional<Refusal>& b) -> std::optional<Refusal> {
  if (!a) return b;
  if (!b) return a;
  return b->stage < a->stage ? b : a;
}

auto double_filter(const std::string& column) -> Refusal {
  return {Stage::Filter, "a filter on column \"" + column + "\", a REAL or DOUBLE PRECISION, is not supported yet"};
}

// The conjuncts of `where` not in `used`, as filters; refused when one is on
// a DOUBLE or REAL column: its comparisons need PostgreSQL's NaN and -0.0
// rules, which only its key encoding has.
auto residual(const Table& t, const Conjuncts& where, const std::vector<const Expr*>& used)
    -> std::expected<Conjuncts, Refusal> {
  Conjuncts out;
  for (const auto& c : where) {
    if (std::ranges::find(used, &c) != used.end()) continue;
    if (col_of(c).type == PhysType::F64) return std::unexpected(double_filter(t.column(col_of(c).col)->name));
    out.push_back(c);
  }
  return out;
}

// One table's rows by one access path, costed.
struct Access {
  std::variant<physical::PointGet, physical::Scan, physical::NullSplitScan> path;
  Conjuncts filters;                       // the conjuncts the path does not express
  std::vector<std::uint32_t> null_checks;  // the parameters the WHERE compares, as the path checks them
  double cost{0};
  double rows{0};
};

// An access path tried: costed, or the refusal that stopped it.
struct Tried {
  Access access;  // its path, even when it failed
  std::optional<Refusal> failed;

  // A point lookup gives at most one row.
  [[nodiscard]] auto one_row() const -> bool { return std::holds_alternative<physical::PointGet>(access.path); }
};

// A table of the statement and the conjuncts on it.
struct Rel {
  const logical::Get* get{nullptr};
  Conjuncts where;

  [[nodiscard]] auto table() const -> const Table& { return *get->table; }
};

// The inner side of an index nested loop: a table read by its whole primary
// key, each part a parameter or a column of a table read before it.
struct Probe {
  std::size_t rel{0};
  std::vector<Expr> key;  // in the primary key's order
  Conjuncts filters;      // its conjuncts the key does not take, then the join's, its column first
  bool by_column{false};  // a key part is an outer column
};

// The inner side of a nested loop read once: a table by its own access path,
// from parameters alone, under an outer side of at most one row; its filters
// include the join's conjuncts.
struct Nested {
  std::size_t rel{0};
  Access access;
};

using Step = std::variant<Probe, Nested>;

// A left-deep plan for a set of tables: the first by an access path, then
// each other one by a step. A plan the search could not complete is kept,
// failed, only for the refusal.
struct Candidate {
  std::size_t first{0};
  Access access;
  std::vector<Step> steps;
  std::uint32_t set{0};
  double cost{0};
  double rows{0};
  std::optional<Refusal> failed;  // the stage it stopped at, the earliest of its parts'

  // A point lookup, then lookups by key: at most one row.
  [[nodiscard]] auto one_row() const -> bool {
    return std::holds_alternative<physical::PointGet>(access.path) &&
           std::ranges::all_of(steps, [](const Step& s) { return std::holds_alternative<Probe>(s); });
  }
};

// A SELECT's result rows over its source, or one aggregate row.
struct SelectParts {
  std::optional<std::int64_t> limit;
  const logical::Project* project{nullptr};
  const logical::Aggregate* aggregate{nullptr};
  const logical::Sort* sort{nullptr};
  const logical::Node* below{nullptr};   // what Project or Aggregate reads: its output, the columns read
  const logical::Node* source{nullptr};  // Filter? -> Get, a Join of them, or Empty
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
  } else if (const auto* a = std::get_if<logical::Aggregate>(&n->op)) {
    s.aggregate = a;
    n = a->input.get();
  } else {
    no_plan(with_article(op_name(*n)) + " where the search plans a Project or an Aggregate");
  }
  s.below = n;
  if (const auto* o = std::get_if<logical::Sort>(&n->op)) {
    s.sort = o;
    n = o->input.get();
  }
  s.source = n;
  return s;
}

// A plan finished but for what every plan of the statement shares.
struct Finished {
  physical::NodePtr root;
  std::vector<std::uint32_t> null_checks;
  std::vector<physical::Computed> computed;
  double cost{0};
};

class Search {
public:
  Search(const Catalog& cat, const RowCounts& counts, bool all) : cat_{cat}, counts_{counts}, all_{all} {}

  // The statement's plans, cheapest first: one, or every one costed.
  auto plans(const logical::Node& root) -> std::vector<physical::Plan> {
    auto finished = statement(root);
    std::ranges::stable_sort(finished, {}, &Finished::cost);
    if (!all_) finished.resize(1);
    std::vector<physical::Plan> out;
    for (auto& f : finished) {
      physical::Plan p;
      p.root = std::move(f.root);
      p.constants = constants_;
      p.catalog_version = cat_.version();
      p.null_checks = std::move(f.null_checks);
      p.computed = std::move(f.computed);
      p.result = result_;
      p.cost = f.cost;
      out.push_back(std::move(p));
    }
    return out;
  }

private:
  auto statement(const logical::Node& root) -> std::vector<Finished> {
    return std::visit(
        [&](const auto& op) -> std::vector<Finished> {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, logical::Insert>) {
            constants(*op.table);
            physical::Insert ins{op.table->id, {}};
            for (std::size_t i = 0; i < op.table->columns.size(); ++i) {
              const auto& c = op.table->columns[i];
              if (op.values[i]) ins.values.emplace_back(logical::column_of(op.table->id, c).col, *op.values[i]);
            }
            std::vector<Finished> out;
            out.push_back({physical::node(std::move(ins)), {}, {}, 1});
            return out;
          } else if constexpr (std::is_same_v<T, logical::Update>) {
            constants(*op.table);
            return write_rows(*op.input, [&](physical::NodePtr rows) {
              return physical::node(physical::Update{std::move(rows), op.table->id, op.set});
            });
          } else if constexpr (std::is_same_v<T, logical::Delete>) {
            constants(*op.table);
            return write_rows(*op.input, [&](physical::NodePtr rows) {
              return physical::node(physical::Delete{std::move(rows), op.table->id});
            });
          } else {
            return select(root);
          }
        },
        root.op);
  }

  void constants(const Table& t) {
    auto c = Catalog::constants(t);
    constants_.insert(constants_.end(), c.begin(), c.end());
  }

  auto table_rows(const Table& t) -> double {
    if (auto it = rows_.find(t.id); it != rows_.end()) return it->second;
    std::optional<std::uint64_t> n;
    if (counts_) n = counts_(t);
    auto rows = n ? static_cast<double>(*n) : kUncountedRows;
    rows_.emplace(t.id, rows);
    return rows;
  }

  auto name_of(const ColumnRef& c) const -> std::string { return cat_.table(c.table)->column(c.col)->name; }

  // ------------------------------------------------------------ the source

  // The tables the statement reads, in FROM order, each with its conjuncts,
  // and the join's conjuncts, each comparing columns of two of them: every
  // conjunct validated by use_of.
  struct Query {
    std::vector<Rel> rels;
    Conjuncts on;

    [[nodiscard]] auto index_of(std::uint32_t table) const -> std::size_t {
      for (std::size_t i = 0; i < rels.size(); ++i) {
        if (rels[i].table().id == table) return i;
      }
      return rels.size();
    }
  };

  static auto tables_of(const Query& q, std::size_t from) -> Tables {
    Tables out;
    for (auto i = from; i < q.rels.size(); ++i) out.push_back(q.rels[i].table().id);
    return out;
  }

  void flatten(const logical::Node& n, Query& q) {
    if (const auto* f = std::get_if<logical::Filter>(&n.op)) {
      const auto* g = std::get_if<logical::Get>(&f->input->op);
      if (g == nullptr) {
        // What pushdown leaves above a Join reads no column.
        for (const auto& c : f->conjuncts) {
          std::vector<ColumnRef> cols;
          logical::columns_of(c, cols);
          if (cols.empty()) no_plan("a conjunct that reads no column");
        }
        no_plan("a Filter over " + with_article(op_name(*f->input)) + " where the search plans a table");
      }
      const auto& t = *g->table;
      for (const auto& c : f->conjuncts) {
        switch (use_of(c, {t.id}, {})) {
          case Use::Ok: break;
          case Use::NoColumn: no_plan("a conjunct on " + t.name + " that does not compare one of its columns");
          case Use::Columns: no_plan("a conjunct on " + t.name + " compares two columns outside a join");
          case Use::Operand:
            no_plan("a conjunct on " + t.name + " compares " + name_of(col_of(c)) +
                    " with an expression the search does not plan yet");
        }
      }
      add_rel(q, {g, f->conjuncts});
      return;
    }
    if (const auto* g = std::get_if<logical::Get>(&n.op)) {
      add_rel(q, {g, {}});
      return;
    }
    const auto* j = std::get_if<logical::Join>(&n.op);
    if (j == nullptr) no_plan(with_article(op_name(n)) + " where the search plans a table");
    auto split = q.rels.size();
    flatten(*j->left, q);
    auto right_from = q.rels.size();
    flatten(*j->right, q);
    Tables left;
    for (auto i = split; i < right_from; ++i) left.push_back(q.rels[i].table().id);
    auto right = tables_of(q, right_from);
    for (const auto& c : j->predicate) {
      auto on_right = is_compare(c) && reads(right, c.args[0]);
      const auto& here = on_right ? right : left;
      const auto& other = on_right ? left : right;
      switch (use_of(c, here, other)) {
        case Use::Ok: break;
        case Use::NoColumn:
        case Use::Columns: no_plan("a join conjunct that does not compare a column of each side");
        case Use::Operand:
          no_plan("a join conjunct compares " + name_of(col_of(c)) + " with an expression the search does not plan yet");
      }
      // One of its own table's: on that table, as pushdown would put it.
      if (operand_of(c).kind == Expr::Kind::Column) {
        q.on.push_back(c);
      } else {
        q.rels[q.index_of(col_of(c).table)].where.push_back(c);
      }
    }
  }

  static void add_rel(Query& q, Rel r) {
    if (q.index_of(r.table().id) != q.rels.size()) no_plan("a table joined with itself");
    // Sets of tables are bit masks.
    if (q.rels.size() == 32) no_plan("a join of more than 32 tables");
    q.rels.push_back(std::move(r));
  }

  // ---------------------------------------------------------- access paths

  // Every access path of a table, costed. `ordered` is the ORDER BY without
  // the columns its WHERE makes constant; `used` the columns read from each
  // row, which a scan reads from the entry's key when `may_cover` and it
  // holds them all.
  auto accesses(const Rel& rel, const std::vector<OrderItem>& order_by, const std::vector<OrderItem>& ordered,
                const std::vector<ColumnRef>& used, bool may_cover) -> std::vector<Tried> {
    const auto& t = rel.table();
    const auto& where = rel.where;
    const auto n = table_rows(t);
    auto for_update = rel.get->for_update;
    std::vector<Tried> out;
    auto refused = [&](Refusal r) {
      Access a;
      a.path = physical::Scan{};
      out.push_back({std::move(a), std::move(r)});
    };
    // A point lookup returns at most one row: it gives any ORDER BY.
    if (auto k = point_get(t, where)) {
      auto f = residual(t, where, k->used);
      Access a;
      a.null_checks = key_params(k->key);
      a.path = physical::PointGet{t.id, std::move(k->key), for_update};
      if (!f) {
        out.push_back({std::move(a), std::move(f.error())});
      } else {
        a.cost = 1;
        a.rows = selectivity(*f);
        a.filters = std::move(*f);
        out.push_back({std::move(a), std::nullopt});
      }
    }
    std::vector<std::uint32_t> null_checks;
    add_params(null_checks, where);
    auto value_bound = [&](const Expr* c, bool successor) -> physical::KeyBound {
      if (c == nullptr) return {};
      return {physical::KeyBound::Kind::Value, operand_of(*c), successor};
    };
    auto null_part = [](bool successor) { return physical::KeyBound{physical::KeyBound::Kind::NullPart, std::nullopt, successor}; };
    for (const auto& p : paths(t)) {
      auto b = bounds(p, where);
      if (b.eq.empty() && b.lo == nullptr && b.hi == nullptr) continue;
      const auto run = b.eq.size();
      if (p.is_pk && run == p.parts.size()) continue;  // the point lookup
      // No sort yet: a path not in the ORDER BY's order is no plan.
      auto reverse = ordered.empty() ? std::optional<bool>{false} : direction(p, run, t.id, ordered);
      if (!reverse) {
        refused({Stage::Order, "no index of " + t.name + " gives ORDER BY " + names(order_by)});
        continue;
      }
      // NULLs in the ordered parts: a part the WHERE compares holds none in
      // the rows it matches. The key order puts an ASC part's NULLs first and
      // a DESC part's last, PostgreSQL the other way round in both
      // directions, so only a nullable ASC part first after the run is
      // served: by two ranges.
      auto split = false;
      for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto* col = t.column(ordered[i].col.col);
        auto compared = std::ranges::any_of(where, [&](const auto& w) { return on_col(w, col->id); });
        if (!col->nullable || compared) continue;
        auto later_nullable = std::ranges::any_of(ordered.begin() + static_cast<std::ptrdiff_t>(i) + 1, ordered.end(),
                                                  [&](const auto& o) { return t.column(o.col.col)->nullable; });
        if (i != 0 || p.parts[run].desc || later_nullable) {
          refused({Stage::Nulls, "ORDER BY " + col->name + " would need its NULLs reordered"});
          reverse.reset();
        } else {
          split = true;
        }
        break;
      }
      if (!reverse) continue;
      std::vector<const Expr*> keyed = b.eq;
      for (const auto* e : {b.lo, b.hi}) {
        if (e != nullptr) keyed.push_back(e);
      }
      auto f = residual(t, where, keyed);
      if (!f) {
        refused(std::move(f.error()));
        continue;
      }

      physical::Scan sc;
      sc.table = t.id;
      sc.object = p.object;
      for (const auto* e : b.eq) sc.prefix.push_back(operand_of(*e));
      sc.reverse = *reverse;
      sc.covering = may_cover && f->empty() &&
                    std::ranges::all_of(used, [&](const ColumnRef& col) { return in_key(t, p, col.col); });
      sc.for_update = for_update;

      Access a;
      a.null_checks = null_checks;
      auto entries = n * std::pow(kEqSelectivity, static_cast<double>(run)) *
                     std::pow(kRangeSelectivity, static_cast<double>(keyed.size() - run));
      if (run >= p.unique) entries = std::min(entries, 1.0);
      entries = std::max(entries, 1.0);  // a scan reads at least its first entry
      a.cost = entries * (!p.is_pk && !sc.covering ? 2 : 1);
      a.rows = entries * selectivity(*f);
      a.filters = std::move(*f);
      if (split) {
        sc.rest = physical::Range{null_part(true), {}};
        a.path = physical::NullSplitScan{std::move(sc)};
        out.push_back({std::move(a), std::nullopt});
        continue;
      }
      if (b.lo != nullptr || b.hi != nullptr) {
        // A key bound: the compared value's key part, or the part that holds
        // NULL, or open; as is or its successor. A key part is
        // self-delimiting and the rest of the key follows it, so a bound
        // that must pass every entry with the part equal to a value is that
        // value's successor.
        const auto& part = p.parts[run];
        auto nullable = t.column(part.col)->nullable;
        const auto* lo = b.lo;
        const auto* hi = b.hi;
        physical::Range r;
        if (!part.desc) {
          // ASC: key order is value order, nulls first. A comparison with
          // NULL is never true, so with no lower bound the scan starts past
          // the null entries, at the successor of the null part.
          r.lo = lo == nullptr && nullable ? null_part(true) : value_bound(lo, lo != nullptr && lo->op == Op::Gt);
          r.hi = value_bound(hi, hi != nullptr && hi->op == Op::Le);
        } else {
          // DESC: key order is descending value order, nulls last. The upper
          // value bound gives the lower key bound and the lower value bound
          // the upper; with no lower value bound the scan stops before the
          // null entries, at the null part.
          r.lo = value_bound(hi, hi != nullptr && hi->op == Op::Lt);
          r.hi = lo == nullptr && nullable ? null_part(false) : value_bound(lo, lo != nullptr && lo->op == Op::Ge);
        }
        sc.rest = r;
      }
      a.path = std::move(sc);
      out.push_back({std::move(a), std::nullopt});
    }
    return out;
  }

  auto names(const std::vector<OrderItem>& items) const -> std::string {
    std::string out;
    for (const auto& o : items) out += (out.empty() ? "" : ", ") + name_of(o.col);
    return out;
  }

  // ----------------------------------------------------------------- joins

  // The join's conjuncts between `q.rels[inner]` and the tables of `outer`,
  // the inner column first: each that use_of takes with the inner table on
  // its own side and `outer` on the other.
  static auto joins(const Query& q, std::size_t inner, std::uint32_t outer) -> Conjuncts {
    const Tables here{q.rels[inner].table().id};
    Tables there;
    for (std::size_t i = 0; i < q.rels.size(); ++i) {
      if ((outer & (1u << i)) != 0) there.push_back(q.rels[i].table().id);
    }
    Conjuncts out;
    for (const auto& c : q.on) {
      if (use_of(c, here, there) == Use::Ok) {
        out.push_back(c);
        continue;
      }
      auto m = logical::compare(mirrored(c.op), c.args[1], c.args[0]);
      if (use_of(m, here, there) == Use::Ok) out.push_back(std::move(m));
    }
    return out;
  }

  // `filters`, then the join conjuncts `on`, each a filter with its own
  // operator; refused when one is on a DOUBLE or REAL column, as any filter
  // is.
  auto with_joins(Conjuncts filters, const Conjuncts& on) const -> std::expected<Conjuncts, Refusal> {
    for (const auto& c : on) {
      for (const auto& col : {c.args[0].col, c.args[1].col}) {
        if (col.type == PhysType::F64) return std::unexpected(double_filter(name_of(col)));
      }
      filters.push_back(c);
    }
    return filters;
  }

  // `q.rels[inner]` read by its whole primary key from parameters and the
  // columns of `outer`'s tables, or the refusal: at Reach when no equality
  // names a key column, at Filter for a filter on a DOUBLE.
  auto probe(const Query& q, std::size_t inner, std::uint32_t outer) const -> std::expected<Probe, Refusal> {
    const auto& rel = q.rels[inner];
    const auto& t = rel.table();
    auto on = joins(q, inner, outer);
    Probe pr;
    pr.rel = inner;
    std::vector<const Expr*> used;
    std::vector<std::size_t> keyed_on;
    std::vector<std::string> missing;
    for (auto id : t.pk_col_ids) {
      if (const auto* c = key_eq_on(rel.where, id)) {
        pr.key.push_back(operand_of(*c));
        used.push_back(c);
        continue;
      }
      // Only an equality keys the inner side.
      auto jt = std::ranges::find_if(on, [&](const Expr& j) { return j.op == Op::Eq && col_of(j).col == id; });
      if (jt == on.end()) {
        missing.push_back(t.column(id)->name);
        continue;
      }
      const auto& o = operand_of(*jt).col;
      pr.key.push_back(logical::column_of(o.table, *cat_.table(o.table)->column(o.col)));
      pr.by_column = true;
      keyed_on.push_back(static_cast<std::size_t>(jt - on.begin()));
    }
    if (!missing.empty()) return std::unexpected(Refusal{Stage::Reach, missing_key(t, missing)});
    auto f = residual(t, rel.where, used);
    if (!f) return std::unexpected(std::move(f.error()));
    // Every join conjunct the key does not take is a filter.
    Conjuncts rest;
    for (std::size_t i = 0; i < on.size(); ++i) {
      if (std::ranges::find(keyed_on, i) == keyed_on.end()) rest.push_back(on[i]);
    }
    auto filters = with_joins(std::move(*f), rest);
    if (!filters) return std::unexpected(std::move(filters.error()));
    pr.filters = std::move(*filters);
    return pr;
  }

  static auto missing_key(const Table& t, const std::vector<std::string>& missing) -> std::string {
    if (missing.size() == t.pk_col_ids.size()) return "no equality names " + t.name + "'s key";
    std::string cols;
    for (const auto& m : missing) cols += (cols.empty() ? "" : ", ") + m;
    return "no equality names " + cols + " of " + t.name + "'s key";
  }

  // Keeps a candidate for its set. For the whole statement's set, every one,
  // in the order they come: complete ones are compared once finished, and
  // the refusal is the first failure of the furthest stage. For a smaller
  // set, a complete one: every one when the search returns them all; else
  // the cheapest, the first of equals, and the cheapest of at most one row
  // apart, which a table read once can extend. A failed one: the furthest
  // per set and kind, the first of equals. Every candidate is in the ORDER
  // BY's order, the one interesting order: with no sort, a plan that is not
  // is never completed.
  void keep(std::map<std::uint32_t, std::vector<Candidate>>& best, Candidate c, std::uint32_t full) const {
    auto& v = best[c.set];
    if (c.set == full || (!c.failed && all_)) {
      v.push_back(std::move(c));
      return;
    }
    auto it = std::ranges::find_if(v, [&](const Candidate& o) {
      return o.one_row() == c.one_row() && o.failed.has_value() == c.failed.has_value();
    });
    if (it == v.end()) {
      v.push_back(std::move(c));
    } else if (c.failed ? c.failed->stage > it->failed->stage : c.cost < it->cost) {
      *it = std::move(c);
    }
  }

  // Every plan of the whole statement's set of tables, by dynamic
  // programming over sets of tables, each set from the sets one smaller;
  // refused if none. A plan that fails is carried on as far as it reaches,
  // so the refusal names the failure of the furthest stage a plan of every
  // table reached.
  auto candidates(const Query& q, const std::vector<OrderItem>& order_by, const std::vector<ColumnRef>& used,
                  bool may_cover) -> std::vector<Candidate> {
    const auto n = q.rels.size();
    const auto full = n == 32 ? ~0u : (1u << n) - 1;
    // The ORDER BY, without the columns the WHERE makes constant.
    std::vector<OrderItem> ordered;
    for (const auto& o : order_by) {
      const auto& where = q.rels[q.index_of(o.col.table)].where;
      auto constant = std::ranges::any_of(where, [&](const auto& w) { return on_col(w, o.col.col) && w.op == Op::Eq; });
      auto repeated = std::ranges::any_of(ordered, [&](const auto& p) { return p.col == o.col; });
      if (!constant && !repeated) ordered.push_back(o);
    }
    std::map<std::uint32_t, std::vector<Candidate>> best;
    // Each table's access paths, and the ones it is read once by, under one
    // row: a scan of those covers when nothing above reads the table's
    // columns, its join conjuncts included.
    std::vector<std::vector<Tried>> tried(n);
    std::vector<std::vector<Tried>> once(n);
    for (std::size_t i = 0; i < n; ++i) {
      tried[i] = accesses(q.rels[i], order_by, ordered, n == 1 ? used : std::vector<ColumnRef>{}, may_cover && n == 1);
      const auto id = q.rels[i].table().id;
      auto unread = std::ranges::none_of(used, [&](const ColumnRef& c) { return c.table == id; }) &&
                    std::ranges::none_of(q.on, [&](const Expr& c) {
                      return c.args[0].col.table == id || c.args[1].col.table == id;
                    });
      once[i] = n > 1 && may_cover && unread ? accesses(q.rels[i], order_by, ordered, {}, true) : tried[i];
      for (const auto& a : tried[i]) {
        Candidate c;
        c.first = i;
        c.set = 1u << i;
        c.access = a.access;
        c.failed = a.failed;
        if (!c.failed) {
          c.cost = a.access.cost;
          c.rows = a.access.rows;
        }
        keep(best, std::move(c), full);
      }
    }
    // Why a set could not be extended by a table: no equality names its key.
    std::map<std::pair<std::uint32_t, std::size_t>, std::string> unkeyed;
    for (std::size_t size = 2; size <= n; ++size) {
      std::vector<std::pair<std::uint32_t, std::size_t>> extend;
      for (const auto& [set, cands] : best) {
        if (static_cast<std::size_t>(std::popcount(set)) != size - 1) continue;
        for (std::size_t t = 0; t < n; ++t) {
          if ((set & (1u << t)) == 0) extend.emplace_back(set, t);
        }
      }
      for (const auto& [set, t] : extend) {
        auto pr = probe(q, t, set);
        auto on = joins(q, t, set);
        if (!pr && pr.error().stage == Stage::Reach) unkeyed.emplace(std::pair{set, t}, pr.error().why);
        auto outers = best.at(set);  // copied: keep may add to the map
        for (const auto& o : outers) {
          if (pr || pr.error().stage != Stage::Reach) {
            Candidate c = o;
            c.set = set | (1u << t);
            c.failed = earlier(o.failed, pr ? std::nullopt : std::optional{pr.error()});
            if (!c.failed) {
              c.steps.emplace_back(*pr);
              c.cost = o.cost + o.rows;  // one key lookup per outer row
              c.rows = o.rows * selectivity(pr->filters);
            }
            keep(best, std::move(c), full);
          }
          // At most one outer row: t read once, by its own scan, the join's
          // conjuncts filtering its rows. Its point lookup is the probe by
          // parameters above.
          if (!o.one_row()) continue;
          for (const auto& a : once[t]) {
            if (a.one_row()) continue;
            Candidate c = o;
            c.set = set | (1u << t);
            Nested ne{t, a.access};
            auto why = a.failed;
            if (!why) {
              auto f = with_joins(std::move(ne.access.filters), on);
              if (f) {
                ne.access.filters = std::move(*f);
              } else {
                why = std::move(f.error());
              }
            }
            c.failed = earlier(o.failed, why);
            c.steps.emplace_back(std::move(ne));
            if (!c.failed) {
              c.cost = o.cost + a.access.cost;
              c.rows = o.rows * a.access.rows * selectivity(on);
            }
            keep(best, std::move(c), full);
          }
        }
      }
    }
    std::vector<Candidate> out;
    if (auto it = best.find(full); it != best.end()) {
      for (auto& c : it->second) {
        if (!c.failed) out.push_back(std::move(c));
      }
    }
    if (!out.empty()) return out;
    refuse(q, best, unkeyed, full);
  }

  // No plan for the statement: the first failure of the furthest stage a
  // plan of every table reached; where none reached them all, the first
  // table the largest set planned lacks, by the key no equality names; where
  // no table was reached, a full scan of the first.
  [[noreturn]] static void refuse(const Query& q, const std::map<std::uint32_t, std::vector<Candidate>>& best,
                                  const std::map<std::pair<std::uint32_t, std::size_t>, std::string>& unkeyed,
                                  std::uint32_t full) {
    if (auto it = best.find(full); it != best.end()) {
      const Refusal* furthest = nullptr;
      for (const auto& c : it->second) {
        if (furthest == nullptr || c.failed->stage > furthest->stage) furthest = &*c.failed;
      }
      if (furthest != nullptr) no_plan(furthest->why);
    }
    // The largest set planned, the first of them; the first table it lacks.
    std::optional<std::uint32_t> largest;
    for (const auto& [set, cands] : best) {
      if (!largest || std::popcount(set) > std::popcount(*largest)) largest = set;
    }
    if (largest) {
      for (std::size_t i = 0; i < q.rels.size(); ++i) {
        if ((*largest & (1u << i)) != 0) continue;
        if (auto it = unkeyed.find({*largest, i}); it != unkeyed.end()) no_plan(it->second);
      }
    }
    no_plan("a full scan of " + q.rels[0].table().name);
  }

  // --------------------------------------------------------- finished plans

  // A leaf's rows, under a Filter of the conjuncts its key does not express.
  static auto filtered(physical::NodePtr leaf, Conjuncts f) -> physical::NodePtr {
    if (f.empty()) return leaf;
    return physical::node(physical::Filter{std::move(leaf), std::move(f)});
  }

  // A candidate's rows, the parameters it checks for NULL and the $n ± k it
  // computes.
  struct Built {
    physical::NodePtr rows;
    std::vector<std::uint32_t> null_checks;
    std::vector<physical::Computed> computed;
  };

  // The $n ± k an access path compares with a column of `table`: in its
  // filters and its scan's range.
  static void access_computed(std::vector<physical::Computed>& comp, const Access& a, std::uint32_t table) {
    for (const auto& f : a.filters) add_computed(comp, operand_of(f), table);
    const physical::Scan* scan = std::get_if<physical::Scan>(&a.path);
    if (const auto* split = std::get_if<physical::NullSplitScan>(&a.path)) scan = &split->present;
    if (scan == nullptr) return;
    if (const auto* r = std::get_if<physical::Range>(&scan->rest)) {
      for (const auto* b : {&r->lo, &r->hi}) {
        if (b->value) add_computed(comp, *b->value, table);
      }
    }
  }

  // An access path's rows, under its filters.
  static auto leaf(const Access& a) -> physical::NodePtr {
    return filtered(std::visit([](const auto& p) { return physical::node(p); }, a.path), a.filters);
  }

  static auto build(const Query& q, const Candidate& c) -> Built {
    Built out;
    access_computed(out.computed, c.access, q.rels[c.first].table().id);
    auto by_column = false;
    auto nested = false;
    for (const auto& step : c.steps) {
      if (const auto* pr = std::get_if<Probe>(&step)) {
        for (const auto& f : pr->filters) add_computed(out.computed, operand_of(f), q.rels[pr->rel].table().id);
        by_column = by_column || pr->by_column;
      } else {
        const auto& ne = std::get<Nested>(step);
        access_computed(out.computed, ne.access, q.rels[ne.rel].table().id);
        nested = true;
      }
    }
    const auto* g = std::get_if<physical::PointGet>(&c.access.path);
    if (!by_column) {
      out.null_checks = c.access.null_checks;
      for (const auto& step : c.steps) {
        if (const auto* pr = std::get_if<Probe>(&step)) {
          for (auto p : key_params(pr->key)) add_param(out.null_checks, p);
        } else {
          for (auto p : std::get<Nested>(step).access.null_checks) add_param(out.null_checks, p);
        }
      }
      out.rows = leaf(c.access);
    } else if (g != nullptr && !nested) {
      // One outer row: a scan of its whole key's prefix, so one loop serves
      // and each inner key is built only from a row read.
      physical::Scan sc;
      sc.table = sc.object = g->table;
      sc.prefix = g->key;
      sc.for_update = g->for_update;
      out.rows = filtered(physical::node(std::move(sc)), c.access.filters);
    } else {
      out.null_checks = c.access.null_checks;
      out.rows = leaf(c.access);
    }
    // A NULL parameter on any table matches no row.
    if (by_column) {
      for (const auto& rel : q.rels) add_params(out.null_checks, rel.where);
    }
    for (const auto& step : c.steps) {
      if (const auto* pr = std::get_if<Probe>(&step)) {
        auto pairs = physical::node(physical::IndexJoin{std::move(out.rows), q.rels[pr->rel].table().id, pr->key});
        out.rows = filtered(std::move(pairs), pr->filters);
      } else {
        out.rows = physical::node(physical::NestedLoop{std::move(out.rows), leaf(std::get<Nested>(step).access)});
      }
    }
    return out;
  }

  // The rows an UPDATE or DELETE writes: each row whole, by a point lookup or
  // a scan, and the filters on them. None from an Empty input.
  auto write_rows(const logical::Node& in, const std::function<physical::NodePtr(physical::NodePtr)>& wrap)
      -> std::vector<Finished> {
    std::vector<Finished> out;
    if (std::holds_alternative<logical::Empty>(in.op)) {
      out.push_back({wrap(physical::node(physical::Empty{})), {}, {}, 0});
      return out;
    }
    Query q;
    flatten(in, q);
    if (q.rels.size() != 1) no_plan("a Join where the search plans a table");
    for (const auto& c : candidates(q, {}, {}, false)) {
      auto b = build(q, c);
      out.push_back({wrap(std::move(b.rows)), std::move(b.null_checks), std::move(b.computed), c.cost});
    }
    return out;
  }

  void result(const SelectParts& s) {
    const auto& outs = s.project != nullptr ? s.project->exprs : s.aggregate->aggs;
    for (const auto& o : outs) result_.push_back({o.name, o.expr.type, o.expr.phys()});
  }

  // Each column the SELECT names is a column of one of its tables.
  static void check_columns(const SelectParts& s, const Tables& tables) {
    auto column = [&](const Expr& e) { return reads(tables, e); };
    if (s.project != nullptr) {
      for (const auto& o : s.project->exprs) {
        if (!column(o.expr)) no_plan("a result column that is not a column of its tables");
      }
    } else {
      if (s.aggregate->aggs.size() != 1) no_plan("an Aggregate of other than one aggregate call");
      const auto& a = s.aggregate->aggs[0].expr;
      if (a.kind != Expr::Kind::Aggregate || (!a.args.empty() && !column(a.args[0]))) {
        no_plan("an aggregate call that does not take a column of its tables");
      }
    }
    if (s.sort != nullptr) {
      for (const auto& k : s.sort->keys) {
        if (!column(k.expr)) no_plan("an ORDER BY key that is not a column of its tables");
      }
    }
  }

  // A SELECT over no rows: nothing is read, and the sink gives what it gives
  // for none.
  auto nothing(const SelectParts& s, const logical::Empty& e) -> std::vector<Finished> {
    for (const auto* t : e.tables) constants(*t);
    auto leaf = physical::node(physical::Empty{});
    std::vector<Finished> out;
    if (s.aggregate != nullptr) {
      auto emits = !s.limit || *s.limit > 0;
      out.push_back({physical::node(physical::Aggregate{std::move(leaf), s.aggregate->aggs[0].expr, emits}), {}, {}, 0});
      return out;
    }
    std::vector<ColumnRef> targets;
    for (const auto& o : s.project->exprs) targets.push_back(o.expr.col);
    out.push_back({emit(s, std::move(leaf), std::move(targets)), {}, {}, 0});
    return out;
  }

  // Emit, under LIMIT.
  static auto emit(const SelectParts& s, physical::NodePtr rows, std::vector<ColumnRef> targets) -> physical::NodePtr {
    if (s.limit) rows = physical::node(physical::Limit{std::move(rows), *s.limit});
    return physical::node(physical::Emit{std::move(rows), std::move(targets)});
  }

  auto select(const logical::Node& root) -> std::vector<Finished> {
    auto s = select_parts(root);
    result(s);
    if (const auto* e = std::get_if<logical::Empty>(&s.source->op)) return nothing(s, *e);
    Query q;
    flatten(*s.source, q);
    for (const auto& rel : q.rels) constants(rel.table());
    Tables tables = tables_of(q, 0);
    check_columns(s, tables);
    std::vector<OrderItem> order_by;
    if (s.sort != nullptr) {
      for (const auto& k : s.sort->keys) order_by.push_back({k.expr.col, k.desc});
    }
    const auto* agg = s.aggregate != nullptr ? &s.aggregate->aggs[0].expr : nullptr;
    auto for_update = std::ranges::any_of(q.rels, [](const Rel& r) { return r.get->for_update; });
    // FOR UPDATE reads each row, whose key it guards; COUNT(DISTINCT) keys
    // each value from its row.
    auto distinct = agg != nullptr && agg->distinct;
    auto cands = candidates(q, order_by, s.below->output, !for_update && !distinct);
    auto emits = !s.limit || *s.limit > 0;
    std::vector<Finished> out;
    for (const auto& c : cands) {
      if (agg == nullptr) {
        std::vector<ColumnRef> targets;
        for (const auto& o : s.project->exprs) targets.push_back(o.expr.col);
        auto b = build(q, c);
        // A LIMIT stops the loop once it has its rows.
        auto cost = c.cost;
        if (s.limit && c.rows > static_cast<double>(*s.limit)) cost *= static_cast<double>(*s.limit) / c.rows;
        out.push_back({emit(s, std::move(b.rows), std::move(targets)), std::move(b.null_checks), std::move(b.computed), cost});
        continue;
      }
      // The keys answer an aggregate when nothing filters a row.
      const auto* scan = c.steps.empty() && c.access.filters.empty() ? std::get_if<physical::Scan>(&c.access.path) : nullptr;
      if (scan != nullptr && !agg->distinct && agg->fn == Expr::Fn::Count) {
        // Every entry of the range is a row to count, unless the argument can
        // be NULL in one: the WHERE compares every column it names, so those
        // hold none.
        const auto& where = q.rels[0].where;
        const auto* arg = agg->args.empty() ? nullptr : &agg->args[0].col;
        auto may_be_null = arg != nullptr && arg->nullable &&
                           std::ranges::none_of(where, [&](const auto& w) { return on_col(w, arg->col); });
        if (!may_be_null) {
          auto b = build(q, c);
          out.push_back({physical::node(physical::CountRange{*scan, emits}), std::move(b.null_checks), std::move(b.computed), 2});
        }
      } else if (scan != nullptr && !agg->distinct && agg->fn == Expr::Fn::Min && !scan->reverse) {
        auto first = *scan;
        if (min_from_first_entry(q.rels[0].table(), first, agg->args[0].col)) {
          auto b = build(q, c);
          auto cost = first.covering || first.object == first.table ? 1.0 : 2.0;
          out.push_back({physical::node(physical::MinFirstEntry{std::move(first), agg->args[0].col, emits}),
                         std::move(b.null_checks), std::move(b.computed), cost});
        }
      }
      auto b = build(q, c);
      // COUNT(DISTINCT) steps through its groups once more.
      auto cost = c.cost + (agg->distinct ? c.rows : 0);
      out.push_back({physical::node(physical::Aggregate{std::move(b.rows), *agg, emits}), std::move(b.null_checks),
                     std::move(b.computed), cost});
    }
    return out;
  }

  // MIN(arg) from the scan's first entry: arg is the ASC part after its
  // prefix, so the first entry past that part's NULLs holds the least value.
  // A scan with no range on the part is given one from past its NULLs.
  static auto min_from_first_entry(const Table& t, physical::Scan& sc, const ColumnRef& arg) -> bool {
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

  const Catalog& cat_;
  const RowCounts& counts_;
  bool all_;
  std::map<std::uint32_t, double> rows_;
  std::vector<Catalog::PlanConstant> constants_;
  std::vector<physical::ResultColumn> result_;
};

}  // namespace

auto search(const logical::Node& root, const Catalog& cat, const RowCounts& counts) -> physical::Plan {
  return std::move(Search{cat, counts, false}.plans(root).front());
}

auto search_all(const logical::Node& root, const Catalog& cat, const RowCounts& counts) -> std::vector<physical::Plan> {
  return Search{cat, counts, true}.plans(root);
}

}  // namespace bytecask::sql
