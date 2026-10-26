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
//      or not; a full scan of the primary key when nothing bounds it.
//   2. Joins: each set of tables from the sets one smaller, the new table
//      read by its whole primary key from parameters and the other tables'
//      columns (an index nested loop), or by its own scan from parameters
//      alone: the join's conjuncts filtering its rows (a nested loop), read
//      once under at most one row, else once per outer row; or, where no key
//      reaches it and the join has an equality between the sides, a hash
//      join, built on either side; keeping the cheapest plan per set and per
//      interesting order. A plan's order is that of the one loop its rows
//      come from, a hash join's probe side's; a key lookup per row keeps it.
//   3. The statement on top: the rows emitted, under LIMIT, or aggregated,
//      COUNT and MIN also answered from the keys (bc_count_range, a scan's
//      first entry) where nothing filters the rows. A plan not in the ORDER
//      BY's order sorts its rows (bc_sorter_*), a sorter per loop, and is
//      compared with the plans in that order.
//   4. Choice points (*Choosing at run time*): where another range of a
//      table a plan scans would cost less past some count of the plan's own
//      range, the plan carries both, picked by bc_count_range when it runs;
//      where a sort of one table's range would pass the memory budget past
//      some count, the plan in the ORDER BY's order beside it; and where a
//      hash join's build would, the cheapest plan without one.
// The interesting order is the ORDER BY's, NULL placement included. A
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
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.search;

export import bytecask.sql.physical;
import bytecask.sql.runtime;

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
// bc_count_range's charge: at most two record reads, whatever the range holds.
inline constexpr double kCountCost = 2;
// What a sort costs per row it holds, beyond the one bc_sorter_next charges:
// the CPU and memory of adding and sorting it, in record reads. Measured on
// the harness (*Decisions*, 9; docs/planner_design.md, *The cost model*).
inline constexpr double kSortWeight = 0.5;
// What a hash join's build costs per row it holds, beyond 1: the CPU and
// memory of building its key and putting it. Measured on the harness
// (*Decisions*, 9; docs/planner_design.md, *The cost model*).
inline constexpr double kHashWeight = 0.3;
// The most choice points one plan carries (*Decisions*, 11); past them the
// search takes its compile-time choice.
inline constexpr std::size_t kMaxChoices = 4;

// The cheapest plan for a rewritten logical plan, its cost in Plan::cost.
// `memory_bytes` is the statement's memory budget, bc_budget::max_bytes (0:
// none): a sort that may pass it, where another plan avoids the sort, is
// the first plan of a choice point by the count it holds, the other its
// second. Throws FeatureNotSupported, its message starting "no plan:", when no
// plan computes it or the logical plan has a shape the search does not plan.
[[nodiscard]] auto search(const logical::Node& root, const Catalog& cat, const RowCounts& counts = {},
                          std::uint64_t memory_bytes = 0) -> physical::Plan;

// Every plan the search costs for the statement, none pruned, cheapest first
// (*Tests*, alternatives agree). Refuses what search refuses.
[[nodiscard]] auto search_all(const logical::Node& root, const Catalog& cat, const RowCounts& counts = {},
                              std::uint64_t memory_bytes = 0) -> std::vector<physical::Plan>;

// The threshold of a choice point between two plans whose costs, at a count
// x of the first plan's range, are first(x) and second(x): the least x in
// [1, max] at which the first costs more. nullopt where they don't cross
// within the range, so the count could not pick either: where they only meet,
// cross past `max` or at 1, or cross more than once at counts spread over the
// range (1, 2, 3, 4, 6, 9, … and max), as a sort's cost and entries_beside's
// clamps can. There the search keeps its compile-time choice.
[[nodiscard]] auto crossing(const std::function<double(double)>& first, const std::function<double(double)>& second,
                            double max) -> std::optional<std::int64_t>;

}  // namespace bytecask::sql

namespace bytecask::sql {

auto crossing(const std::function<double(double)>& first, const std::function<double(double)>& second, double max)
    -> std::optional<std::int64_t> {
  auto reached = [&](std::int64_t x) { return first(static_cast<double>(x)) > second(static_cast<double>(x)); };
  // A count past INT64_MAX is no count a table holds.
  auto hi = static_cast<std::int64_t>(std::min(std::floor(max), 0x1p62));
  std::int64_t lo = 1;
  if (hi <= lo || reached(lo) || !reached(hi)) return std::nullopt;
  // The binary search below assumes the first costs more from one count on.
  auto changes = 0;
  auto last = false;
  for (std::int64_t x = 2;; x = std::max(x + 1, x + x / 2)) {
    x = std::min(x, hi);
    auto now = reached(x);
    changes += now != last ? 1 : 0;
    last = now;
    if (x == hi) break;
  }
  if (changes > 1) return std::nullopt;
  while (hi - lo > 1) {
    auto mid = lo + (hi - lo) / 2;
    (reached(mid) ? hi : lo) = mid;
  }
  return hi;
}

namespace {

using ast::FeatureNotSupported;
using logical::ColumnRef;
using logical::Expr;
using Op = ast::Compare::Op;
using Conjuncts = std::vector<Expr>;

[[noreturn]] void no_plan(const std::string& why) { throw FeatureNotSupported{"no plan: " + why}; }

// An operator's name, for a refusal of a shape the search does not plan.
auto op_name(const logical::Node& n) -> std::string {
  constexpr const char* kNames[] = {"Get",  "Empty", "OneRow", "Filter", "Project", "Join",  "Aggregate",
                                    "Sort", "Limit", "Insert", "Update", "Delete"};
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

// The fraction of rows a conjunct keeps, as the cost model assumes it: a
// predicate's as a range's.
auto selectivity(const Expr& c) -> double {
  return is_compare(c) && c.op == Op::Eq ? kEqSelectivity : kRangeSelectivity;
}
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
    case Expr::Kind::Aggregate:
    case Expr::Kind::And:
    case Expr::Kind::Or:
    case Expr::Kind::Not:
    case Expr::Kind::IsNull:
    case Expr::Kind::Like: return Use::Operand;
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
  bool nulls_first;
};

// A statement's GROUP BY columns the WHERE does not make constant, which an
// access path gives in groups when they are its next parts; nullopt with no
// GROUP BY.
using Grouping = std::optional<std::vector<ColumnRef>>;

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

// Where a plan stopped: at reaching its tables (Reach), or at a filter it
// can't run (Filter).
enum class Stage { Reach, Filter };

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

// The DOUBLE or REAL column a predicate compares, or null: IS NULL of one
// needs no comparison.
auto float_compared(const Expr& p) -> const ColumnRef* {
  if (p.kind == Expr::Kind::Compare) return col_of(p).type == PhysType::F64 ? &col_of(p) : nullptr;
  if (p.kind == Expr::Kind::IsNull || p.kind == Expr::Kind::Like) return nullptr;  // Like: of text
  for (const auto& a : p.args) {
    if (const auto* c = float_compared(a)) return c;
  }
  return nullptr;
}

// One table's rows by one access path. Its cost and rows follow from the
// entries its range reaches (access_cost, access_rows).
struct Access {
  std::variant<physical::PointGet, physical::Scan, physical::NullSplitScan> path;
  Conjuncts filters;                       // the conjuncts the path does not express
  std::vector<std::uint32_t> null_checks;  // the parameters the WHERE compares, as the path checks them
  double entries{1};                       // as the cost model estimates them; 1 for a point lookup
  double max_entries{1};                   // the most it can reach: 1 when its bounds name at most one row
  std::vector<std::size_t> keyed;          // the conjuncts of its table's WHERE its key takes, by position
  bool ordered{true};                      // its rows come in the ORDER BY's order, NULLs placed as it asks
  bool grouped{false};                     // its rows come in groups of the GROUP BY: a statement's with one
};

// The scan an access path reads, or null for a point lookup; a NULL split's
// present values, whose prefix is the split's.
auto scan_of(const Access& a) -> const physical::Scan* {
  if (const auto* s = std::get_if<physical::Scan>(&a.path)) return s;
  if (const auto* s = std::get_if<physical::NullSplitScan>(&a.path)) return &s->present;
  return nullptr;
}
auto scan_of(Access& a) -> physical::Scan* {
  if (auto* s = std::get_if<physical::Scan>(&a.path)) return s;
  if (auto* s = std::get_if<physical::NullSplitScan>(&a.path)) return &s->present;
  return nullptr;
}

// One read per entry, and one more per row an index scan fetches.
auto reads_per_entry(const Access& a) -> double {
  const auto* s = scan_of(a);
  return s != nullptr && s->object != s->table && !s->covering ? 2 : 1;
}
auto access_cost(const Access& a, double entries) -> double { return entries * reads_per_entry(a); }

// A full scan: nothing bounds it, so no parameter changes its count.
auto full_scan(const Access& a) -> bool {
  const auto* s = scan_of(a);
  return s != nullptr && s->prefix.empty() && a.keyed.empty();
}

// The access with its rows read: a sorter holds a row, not a covering
// scan's entry.
auto uncovered(Access a) -> Access {
  if (auto* s = scan_of(a)) s->covering = false;
  return a;
}
auto access_rows(const Access& a, double entries) -> double { return entries * selectivity(a.filters); }

// The least count of entries past the memory budget of a structure that
// holds at least `fixed` bytes and `per_entry` per entry; 0 for a budget
// under `fixed`, which no count fits.
auto least_past(std::uint64_t budget, std::uint64_t fixed, std::uint64_t per_entry) -> std::uint64_t {
  if (budget < fixed) return 0;
  return std::min<std::uint64_t>((budget - fixed) / per_entry + 1, std::uint64_t{1} << 62);
}

// The fewest bytes a sort key part of `c` holds (plan_api_design.md, *Sort
// keys*): its marker alone where it may be NULL, else the marker and the
// least value, 8 bytes for an integer or a DOUBLE, 1 for a BOOLEAN, 2 for an
// empty byte string.
auto least_part_bytes(const ColumnRef& c, bool may_be_null) -> std::uint64_t {
  if (may_be_null) return 1;
  return 1 + (c.type == PhysType::Bool ? 1 : c.type == PhysType::Bytes ? 2 : 8);
}

// The fewest bytes a stored row of `t` holds: its header, bitmap and fixed
// columns, in the smallest layout its rows may have.
auto least_row_bytes(const Table& t) -> std::uint64_t {
  std::uint64_t out = t.layout.fixed_end;
  for (const auto& [id, l] : t.retired_layouts) out = std::min<std::uint64_t>(out, l.fixed_end);
  return out;
}

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
  Conjuncts where;  // its comparisons, which its access paths may take
  Conjuncts preds;  // its predicates (logical::is_predicate): filters only

  [[nodiscard]] auto table() const -> const Table& { return *get->table; }
};

// The conjuncts of rel's WHERE not in `used`, then its predicates, as
// filters; refused when one compares a DOUBLE or REAL column: its
// comparisons need PostgreSQL's NaN and -0.0 rules, which only its key
// encoding has.
auto residual(const Rel& rel, const std::vector<const Expr*>& used) -> std::expected<Conjuncts, Refusal> {
  const auto& t = rel.table();
  Conjuncts out;
  for (const auto& c : rel.where) {
    if (std::ranges::find(used, &c) != used.end()) continue;
    if (col_of(c).type == PhysType::F64) return std::unexpected(double_filter(t.column(col_of(c).col)->name));
    out.push_back(c);
  }
  for (const auto& p : rel.preds) {
    if (const auto* c = float_compared(p)) return std::unexpected(double_filter(t.column(c->col)->name));
    out.push_back(p);
  }
  return out;
}

// The inner side of an index nested loop: a table read by its whole primary
// key, each part a parameter or a column of a table read before it.
struct Probe {
  std::size_t rel{0};
  std::vector<Expr> key;  // in the primary key's order
  Conjuncts filters;      // its conjuncts the key does not take, then the join's, its column first
  bool by_column{false};  // a key part is an outer column
};

// The inner side of a nested loop: a table by its own access path, from
// parameters alone, read once under an outer side of at most one row and
// once per outer row under any other (`rescan`); its filters end with the
// join's conjuncts, `on`.
struct Nested {
  std::size_t rel{0};
  Access access;
  Conjuncts on;
  bool rescan{false};
};

// The inner side of a hash join: a table by its own access path, from
// parameters alone, read once; the join's equalities key the hash table, and
// its other conjuncts filter the pairs. With `build_inner` the hash table
// holds the inner table's rows, probed by the outer plan's; otherwise it
// holds the outer table's, the plan's first and only one before it, probed
// by the inner one's.
struct Hashed {
  std::size_t rel{0};
  Access access;
  std::vector<physical::HashKey> keys;  // each the held table's column, then the probe side's
  Conjuncts filters;
  bool build_inner{true};
};

using Step = std::variant<Probe, Nested, Hashed>;

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
  bool probe_sorted{false};       // a hash join's probe side is sorted before it probes

  // A point lookup, then lookups by key: at most one row.
  [[nodiscard]] auto one_row() const -> bool {
    return std::holds_alternative<physical::PointGet>(access.path) &&
           std::ranges::all_of(steps, [](const Step& s) { return std::holds_alternative<Probe>(s); });
  }

  // The tables of the loops its rows come from, one inside another, outer
  // first, by position in FROM order: the first table's scan, each table read
  // by its own scan, and a hash join's probe side's loops, then the loop over
  // the rows it holds.
  [[nodiscard]] auto loop_rels() const -> std::vector<std::size_t> {
    std::vector<std::size_t> out;
    if (!std::holds_alternative<physical::PointGet>(access.path)) out.push_back(first);
    for (const auto& s : steps) {
      if (const auto* ne = std::get_if<Nested>(&s)) {
        out.push_back(ne->rel);
      } else if (const auto* h = std::get_if<Hashed>(&s)) {
        // Built on the first table: probed by this one's scan, then its rows.
        if (h->build_inner) {
          out.push_back(h->rel);
        } else {
          out = {h->rel, first};
        }
      }
    }
    return out;
  }

  // The first step after its first loop: the lookups by key from it on read
  // a row per row of the loops.
  [[nodiscard]] auto after_first_loop() const -> std::size_t {
    if (!std::holds_alternative<physical::PointGet>(access.path)) return 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (!std::holds_alternative<Probe>(steps[i])) return i;
    }
    return steps.size();
  }

  // Its hash join, or null.
  [[nodiscard]] auto hash() const -> const Hashed* {
    for (const auto& s : steps) {
      if (const auto* h = std::get_if<Hashed>(&s)) return h;
    }
    return nullptr;
  }

  // Where its one loop is, the one its order is: 0 the first table's
  // access, i + 1 step i's; a hash join built on the first table, its probe
  // side's; nullopt for at most one row.
  [[nodiscard]] auto driving() const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (const auto* h = std::get_if<Hashed>(&steps[i]); h != nullptr && !h->build_inner) return i + 1;
    }
    if (!std::holds_alternative<physical::PointGet>(access.path)) return 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (std::holds_alternative<Nested>(steps[i])) return i + 1;
    }
    return std::nullopt;
  }

  // The access path of the first table (0), or of step at - 1's table, read
  // by its own scan.
  [[nodiscard]] auto access_at(std::size_t at) const -> const Access& {
    if (at == 0) return access;
    if (const auto* h = std::get_if<Hashed>(&steps[at - 1])) return h->access;
    return std::get<Nested>(steps[at - 1]).access;
  }
  [[nodiscard]] auto access_at(std::size_t at) -> Access& {
    if (at == 0) return access;
    if (auto* h = std::get_if<Hashed>(&steps[at - 1])) return h->access;
    return std::get<Nested>(steps[at - 1]).access;
  }

  // Its rows come in the ORDER BY's order: its loop's order, which a lookup
  // by key per row, or a loop inside it, keeps; any order for one row.
  [[nodiscard]] auto ordered() const -> bool {
    auto d = driving();
    return !d || access_at(*d).ordered;
  }

  // Its rows come in groups of the GROUP BY, each group's together: the
  // first table's scan gives them, and each table after it is looked up by
  // key; or it has at most one row.
  [[nodiscard]] auto streams() const -> bool {
    if (one_row()) return true;
    return driving() == 0 && access.grouped &&
           std::ranges::all_of(steps, [](const Step& s) { return std::holds_alternative<Probe>(s); });
  }
};

// The candidate with the rows of the tables `rels` read by its own access
// paths, for sorters to hold.
void read_rows(Candidate& c, const std::vector<std::size_t>& rels) {
  auto held = [&](std::size_t r) { return std::ranges::find(rels, r) != rels.end(); };
  if (held(c.first)) c.access = uncovered(c.access);
  for (auto& s : c.steps) {
    if (auto* ne = std::get_if<Nested>(&s); ne != nullptr && held(ne->rel)) ne->access = uncovered(ne->access);
    if (auto* h = std::get_if<Hashed>(&s); h != nullptr && held(h->rel)) h->access = uncovered(h->access);
  }
}

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
  Search(const Catalog& cat, const RowCounts& counts, std::uint64_t memory, bool all)
      : cat_{cat}, counts_{counts}, memory_{memory}, all_{all} {}

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
    // A left outer join of rels[0] and rels[1]: rels[0] first, by any path,
    // rels[1] probed or scanned under it, never hashed; the join's ON among
    // rels[1]'s conjuncts and `on`; `post` the WHERE's conjuncts on rels[1]'s
    // columns, which filter the rows the join gives, NULLs included.
    bool left_outer{false};
    Conjuncts post;
    std::vector<ColumnRef> above;    // what a SELECT reads above a sort: its result's columns
    std::vector<ColumnRef> sort_by;  // the ORDER BY's columns a sort keys on

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
      if (const auto* j = std::get_if<logical::Join>(&f->input->op); j != nullptr && j->left_outer) {
        for (const auto& c : f->conjuncts) {
          // Of the join's rows: on a DOUBLE or REAL column, as any filter's.
          const auto* fc = is_compare(c) ? (col_of(c).type == PhysType::F64 ? &col_of(c) : nullptr) : float_compared(c);
          if (fc != nullptr) no_plan(double_filter(cat_.table(fc->table)->column(fc->col)->name).why);
          q.post.push_back(c);
        }
        flatten(*f->input, q);
        return;
      }
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
      Rel rel{g, {}, {}};
      for (const auto& c : f->conjuncts) {
        if (logical::is_predicate(c)) {
          rel.preds.push_back(c);
          continue;
        }
        rel.where.push_back(c);
        switch (use_of(c, {t.id}, {})) {
          case Use::Ok: break;
          case Use::NoColumn: no_plan("a conjunct on " + t.name + " that does not compare one of its columns");
          case Use::Columns: no_plan("a conjunct on " + t.name + " compares two columns outside a join");
          case Use::Operand:
            no_plan("a conjunct on " + t.name + " compares " + name_of(col_of(c)) +
                    " with an expression the search does not plan yet");
        }
      }
      add_rel(q, std::move(rel));
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
    if (j->left_outer) {
      if (q.rels.size() != 2) no_plan("a left outer join of other than two tables");
      q.left_outer = true;
    }
    for (const auto& c : j->predicate) {
      // An outer join's ON conjunct on its left side alone decides its pairs:
      // a filter of its right side's rows, which reads the left row too.
      if (j->left_outer) {
        std::vector<ColumnRef> cols;
        logical::columns_of(c, cols);
        if (!cols.empty() && std::ranges::all_of(cols, [&](const ColumnRef& r) { return reads(left, logical::column(r, {})); })) {
          q.rels[right_from].preds.push_back(c);
          continue;
        }
      }
      if (logical::is_predicate(c)) {
        // A filter of the one table whose columns it reads.
        std::vector<ColumnRef> cols;
        logical::columns_of(c, cols);
        if (cols.empty() || std::ranges::any_of(cols, [&](const ColumnRef& r) { return r.table != cols[0].table; })) {
          no_plan("a predicate (AND, OR, NOT, IS NULL) of other than one table's columns");
        }
        q.rels[q.index_of(cols[0].table)].preds.push_back(c);
        continue;
      }
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

  // How a path, its first `run` parts compared for equality, gives the
  // ORDER BY `ordered` (without the columns the WHERE makes constant), NULL
  // placement included: in its direction or reversed, and by one range or
  // two; nullopt when it does not.
  struct Order {
    bool reverse{false};
    bool split{false};
  };
  static auto order_of(const Table& t, const Path& p, std::size_t run, const Conjuncts& where,
                       const std::vector<OrderItem>& ordered) -> std::optional<Order> {
    if (ordered.empty()) return Order{};
    auto reverse = direction(p, run, t.id, ordered);
    if (!reverse) return std::nullopt;
    auto split = false;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      // A part the WHERE compares holds no NULLs in the rows it matches.
      const auto* col = t.column(ordered[i].col.col);
      auto compared = std::ranges::any_of(where, [&](const auto& w) { return on_col(w, col->id); });
      if (!col->nullable || compared) continue;
      // The key order puts an ASC part's NULLs first and a DESC part's last,
      // and a reverse scan the other way round.
      const auto& part = p.parts[run + i];
      if ((part.desc == *reverse) == ordered[i].nulls_first) continue;
      // Two ranges, the values and the NULL part, place them the other way:
      // only for the first ordered part, an ASC one.
      if (i != 0 || part.desc) return std::nullopt;
      split = true;
    }
    return Order{*reverse, split};
  }

  // Whether a path, its first `run` parts compared for equality, gives the
  // rows of `grouping` (the GROUP BY's columns the WHERE does not make
  // constant) in groups: every one is t's, and they are its next parts, in
  // any order. Each direction, and either NULL split, keeps a group's rows
  // together.
  static auto in_groups(const Table& t, const Path& p, std::size_t run, const std::vector<ColumnRef>& grouping) -> bool {
    if (std::ranges::any_of(grouping, [&](const ColumnRef& c) { return c.table != t.id; })) return false;
    if (run + grouping.size() > p.parts.size()) return false;
    for (std::size_t i = 0; i < grouping.size(); ++i) {
      auto col = p.parts[run + i].col;
      if (std::ranges::none_of(grouping, [&](const ColumnRef& c) { return c.col == col; })) return false;
    }
    return true;
  }

  // Every access path of a table: the point lookup, a scan of each path
  // whose first part the WHERE compares, a full scan of the primary key
  // when nothing bounds it, and of an index whose order gives the ORDER BY. `ordered` is the ORDER BY without the columns
  // its WHERE makes constant; a path that does not give it reads in key
  // order, for a sort. `read` the columns read from each of its rows, which
  // a scan with no filter reads from the entry's key when `may_cover` and
  // the key holds them all: it covers.
  // `grouping`, a statement with GROUP BY's: its columns the WHERE does not
  // make constant, which a path gives in groups when they are its parts after
  // its equality prefix, in any order and direction; a full scan of an index
  // that does is built too.
  auto accesses(const Rel& rel, const std::vector<OrderItem>& ordered, const std::vector<ColumnRef>& read,
                bool may_cover, const Grouping& grouping) -> std::vector<Tried> {
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
    auto positions = [&](const std::vector<const Expr*>& used) {
      std::vector<std::size_t> at;
      for (const auto* e : used) at.push_back(static_cast<std::size_t>(e - where.data()));
      return at;
    };
    // A point lookup returns at most one row: it gives any ORDER BY.
    if (auto k = point_get(t, where)) {
      auto f = residual(rel, k->used);
      Access a;
      a.null_checks = key_params(k->key);
      a.keyed = positions(k->used);
      a.grouped = grouping.has_value();  // one row is one group
      a.path = physical::PointGet{t.id, std::move(k->key), for_update};
      if (!f) {
        out.push_back({std::move(a), std::move(f.error())});
      } else {
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
      const auto run = b.eq.size();
      auto bounded = run > 0 || b.lo != nullptr || b.hi != nullptr;
      if (p.is_pk && run == p.parts.size()) continue;  // the point lookup
      auto order = order_of(t, p, run, where, ordered);
      auto groups = grouping && in_groups(t, p, run, *grouping);
      // A full scan of an index only for the ORDER BY it gives, or the
      // groups.
      if (!bounded && !p.is_pk && (ordered.empty() || !order) && !groups) continue;
      std::vector<const Expr*> keyed = b.eq;
      for (const auto* e : {b.lo, b.hi}) {
        if (e != nullptr) keyed.push_back(e);
      }
      auto f = residual(rel, keyed);
      if (!f) {
        refused(std::move(f.error()));
        continue;
      }

      physical::Scan sc;
      sc.table = t.id;
      sc.object = p.object;
      for (const auto* e : b.eq) sc.prefix.push_back(operand_of(*e));
      sc.reverse = order && order->reverse;
      sc.covering = may_cover && f->empty() &&
                    std::ranges::all_of(read, [&](const ColumnRef& col) { return in_key(t, p, col.col); });
      sc.for_update = for_update;

      Access a;
      a.null_checks = null_checks;
      a.keyed = positions(keyed);
      a.max_entries = run >= p.unique ? 1 : n;
      a.entries = n * std::pow(kEqSelectivity, static_cast<double>(run)) *
                  std::pow(kRangeSelectivity, static_cast<double>(keyed.size() - run));
      a.entries = std::max(std::min(a.entries, a.max_entries), 1.0);  // a scan reads at least its first entry
      a.filters = std::move(*f);
      a.ordered = order.has_value();
      a.grouped = groups;
      if (order && order->split) {
        sc.rest = physical::Range{null_part(true), {}};
        a.path = physical::NullSplitScan{std::move(sc)};
        out.push_back({std::move(a), std::nullopt});
        continue;
      }
      if (!bounded) {
        // Every entry.
        sc.rest = physical::Range{};
      } else if (b.lo != nullptr || b.hi != nullptr) {
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

  // Why join conjuncts can't be filters, each with its own operator: the
  // first on a DOUBLE or REAL column, refused as any filter is.
  auto double_in(const Conjuncts& on) const -> std::optional<Refusal> {
    for (const auto& c : on) {
      for (const auto& col : {c.args[0].col, c.args[1].col}) {
        if (col.type == PhysType::F64) return double_filter(name_of(col));
      }
    }
    return std::nullopt;
  }

  // `q.rels[inner]` read by its whole primary key from parameters and the
  // columns of the tables already read, `on` its join conjuncts with them
  // (joins), or the refusal: at Reach when no equality names a key column, at
  // Filter for a filter on a DOUBLE.
  auto probe(const Query& q, std::size_t inner, const Conjuncts& on) const -> std::expected<Probe, Refusal> {
    const auto& rel = q.rels[inner];
    const auto& t = rel.table();
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
    auto f = residual(rel, used);
    if (!f) return std::unexpected(std::move(f.error()));
    // Every join conjunct the key does not take is a filter.
    pr.filters = std::move(*f);
    for (std::size_t i = 0; i < on.size(); ++i) {
      if (std::ranges::find(keyed_on, i) != keyed_on.end()) continue;
      if (auto d = double_in({on[i]})) return std::unexpected(std::move(*d));
      pr.filters.push_back(on[i]);
    }
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
  // the cheapest per kind, the first of equals: of at most one row apart,
  // which a table read once can extend, per interesting order, the ORDER
  // BY's or none, since a plan in that order needs no sort, and per whether
  // its rows come in the GROUP BY's groups, which stream. A failed
  // one: the furthest per set and kind, the first of equals.
  void keep(std::map<std::uint32_t, std::vector<Candidate>>& best, Candidate c, std::uint32_t full) const {
    auto& v = best[c.set];
    if (c.set == full || (!c.failed && all_)) {
      v.push_back(std::move(c));
      return;
    }
    auto it = std::ranges::find_if(v, [&](const Candidate& o) {
      return o.one_row() == c.one_row() && o.ordered() == c.ordered() && o.streams() == c.streams() &&
             o.failed.has_value() == c.failed.has_value();
    });
    if (it == v.end()) {
      v.push_back(std::move(c));
    } else if (c.failed ? c.failed->stage > it->failed->stage : c.cost < it->cost) {
      *it = std::move(c);
    }
  }

  // The columns read from each row of `q.rels[i]`: those of `used` on its
  // table, and its join conjuncts' columns, which a key built from it or a
  // filter on the pair reads.
  static auto read_set(const Query& q, std::size_t i, const std::vector<ColumnRef>& used) -> std::vector<ColumnRef> {
    const auto id = q.rels[i].table().id;
    std::vector<ColumnRef> out;
    for (const auto& c : used) {
      if (c.table == id) out.push_back(c);
    }
    for (const auto& c : q.on) {
      for (const auto& col : {c.args[0].col, c.args[1].col}) {
        if (col.table == id) out.push_back(col);
      }
    }
    // An outer join's conjuncts that read both sides: its ON's on the left
    // side alone, among the right side's, and the WHERE's above it.
    std::vector<ColumnRef> cols;
    for (const auto& r : q.rels) {
      for (const auto& c : r.preds) logical::columns_of(c, cols);
    }
    for (const auto& c : q.post) logical::columns_of(c, cols);
    for (const auto& col : cols) {
      if (col.table == id) out.push_back(col);
    }
    return out;
  }

  // An access path read once, under one row: the join's conjuncts `on`
  // filter its rows, so a scan reads each row and does not cover.
  static auto read_once(Access a, const Conjuncts& on) -> Access {
    if (on.empty()) return a;
    if (auto* sc = scan_of(a)) sc->covering = false;
    a.filters.insert(a.filters.end(), on.begin(), on.end());
    return a;
  }

  // The cost and rows of a candidate, each part's from its entries: the
  // first table's access, then for each step one key lookup per outer row,
  // or the access read once, or once per outer row, or a hash join; and the
  // part of the cost a LIMIT does not cut, what a hash join does before its
  // first pair. `at`, if given, replaces one access's entries (0 the first
  // table's, i + 1 step i's).
  struct Entries {
    std::size_t at{0};
    double entries{0};
  };
  struct Measured {
    double cost{0};
    double rows{0};
    double fixed{0};
  };
  static auto measure(const Candidate& c, std::optional<Entries> at = std::nullopt) -> Measured {
    auto entries = [&](std::size_t i, const Access& a) { return at && at->at == i ? at->entries : a.entries; };
    Measured m;
    m.cost = access_cost(c.access, entries(0, c.access));
    m.rows = access_rows(c.access, entries(0, c.access));
    for (std::size_t i = 0; i < c.steps.size(); ++i) {
      if (const auto* pr = std::get_if<Probe>(&c.steps[i])) {
        m.cost += m.rows;
        m.rows *= selectivity(pr->filters);
      } else if (const auto* ne = std::get_if<Nested>(&c.steps[i])) {
        auto x = entries(i + 1, ne->access);
        m.cost += (ne->rescan ? std::max(m.rows, 1.0) : 1.0) * access_cost(ne->access, x);
        m.rows *= access_rows(ne->access, x);
      } else {
        const auto& h = std::get<Hashed>(c.steps[i]);
        auto x = entries(i + 1, h.access);
        // Each side read once. The build side's rows are held, each at the
        // hash weight; a sorted probe side's, at the sort's, before it probes.
        Measured inner{access_cost(h.access, x), access_rows(h.access, x), 0};
        auto& built = h.build_inner ? inner : m;
        auto& probed = h.build_inner ? m : inner;
        built.cost += built.rows * (1 + kHashWeight);
        if (c.probe_sorted) probed.cost += probed.rows * (1 + kSortWeight);
        // Each pair is a step of bc_hash_next.
        auto pairs = m.rows * inner.rows * std::pow(kEqSelectivity, static_cast<double>(h.keys.size()));
        m.fixed = built.cost + (c.probe_sorted ? probed.cost : 0);
        m.cost = built.cost + probed.cost + pairs;
        m.rows = pairs * selectivity(h.filters);
      }
    }
    return m;
  }

  static void costed(Candidate& c) {
    if (c.failed) return;
    auto m = measure(c);
    c.cost = m.cost;
    c.rows = m.rows;
  }

  // A hash join of the outer plan `o` and `q.rels[t]`, by the access `a`, on
  // the join's equalities in `on`, built on t's rows or, when the plan is one
  // table's access alone, on its rows; nullopt where `on` has no equality or
  // the outer plan is more than one access. The built side's rows are read,
  // for the hash table to hold. A DOUBLE in `on`, `on_double`, refuses it as
  // it refuses a nested loop.
  static auto hash_join(const Candidate& o, std::size_t t, const Tried& a, const Conjuncts& on,
                        const std::optional<Refusal>& on_double, bool build_inner) -> std::optional<Candidate> {
    if (!build_inner && !o.steps.empty()) return std::nullopt;
    Hashed h;
    h.rel = t;
    h.build_inner = build_inner;
    h.access = build_inner ? uncovered(a.access) : a.access;
    for (const auto& j : on) {
      if (j.op == Op::Eq) {
        h.keys.push_back(build_inner ? physical::HashKey{col_of(j), operand_of(j).col}
                                     : physical::HashKey{operand_of(j).col, col_of(j)});
      } else {
        h.filters.push_back(j);
      }
    }
    if (h.keys.empty()) return std::nullopt;
    Candidate c = o;
    c.set |= 1u << t;
    if (!build_inner) c.access = uncovered(c.access);
    c.failed = earlier(o.failed, a.failed ? a.failed : on_double);
    c.steps.emplace_back(std::move(h));
    return c;
  }

  // The plans of the whole statement's set of tables, and each table's access
  // paths, which the choice points take their second plans from.
  struct Searched {
    std::vector<Candidate> plans;
    std::vector<std::vector<Tried>> tried;  // per table, in FROM order
  };

  // Every plan of the whole statement's set of tables, by dynamic
  // programming over sets of tables, each set from the sets one smaller;
  // refused if none. A plan that fails is carried on as far as it reaches,
  // so the refusal names the failure of the furthest stage a plan of every
  // table reached. `ordered` is the ORDER BY without the columns the WHERE
  // makes constant. With `sorts`, a plan not in its order sorts its rows, the
  // loops its sorters hold reading them (read_for_sort): each candidate is
  // kept and pruned at the cost of the plan it becomes.
  auto candidates(const Query& q, const std::vector<OrderItem>& ordered, const std::vector<ColumnRef>& used,
                  bool may_cover, bool sorts, const Grouping& grouping = std::nullopt) -> Searched {
    auto cost = [&](Candidate& c) {
      if (sorts && !c.ordered()) read_for_sort(q, c);
      costed(c);
    };
    auto cost_of = [&](Candidate c) {
      cost(c);
      return c;
    };
    const auto n = q.rels.size();
    const auto full = n == 32 ? ~0u : (1u << n) - 1;
    std::map<std::uint32_t, std::vector<Candidate>> best;
    Searched out;
    out.tried.assign(n, {});
    for (std::size_t i = 0; i < n; ++i) {
      // An outer join's right side reads rows: where it matches nothing its
      // row is bc_null_row's, never an entry's key.
      out.tried[i] = accesses(q.rels[i], ordered, read_set(q, i, used), may_cover && !(q.left_outer && i == 1), grouping);
      for (const auto& a : out.tried[i]) {
        Candidate c;
        c.first = i;
        c.set = 1u << i;
        c.access = a.access;
        c.failed = a.failed;
        cost(c);
        keep(best, std::move(c), full);
      }
    }
    for (std::size_t size = 2; size <= n; ++size) {
      std::vector<std::pair<std::uint32_t, std::size_t>> extend;
      for (const auto& [set, cands] : best) {
        if (static_cast<std::size_t>(std::popcount(set)) != size - 1) continue;
        for (std::size_t t = 0; t < n; ++t) {
          if ((set & (1u << t)) == 0) extend.emplace_back(set, t);
        }
      }
      for (const auto& [set, t] : extend) {
        if (q.left_outer && t == 0) continue;  // its left side first
        auto on = joins(q, t, set);
        auto pr = probe(q, t, on);
        // Read by its own scan, t's rows are filtered by every join conjunct.
        auto on_double = double_in(on);
        auto outers = best.at(set);  // copied: keep may add to the map
        for (const auto& o : outers) {
          if (pr || pr.error().stage != Stage::Reach) {
            Candidate c = o;
            c.set = set | (1u << t);
            c.failed = earlier(o.failed, pr ? std::nullopt : std::optional{pr.error()});
            if (!c.failed) c.steps.emplace_back(*pr);
            cost(c);
            keep(best, std::move(c), full);
          }
          // t by its own scan, the join's conjuncts filtering its rows: read
          // once under at most one outer row, else once per outer row. Its
          // point lookup is the probe by parameters above.
          for (const auto& a : out.tried[t]) {
            if (a.one_row()) continue;
            Candidate c = o;
            c.set = set | (1u << t);
            auto rescan = !o.one_row();
            c.failed = earlier(o.failed, a.failed ? a.failed : on_double);
            c.steps.emplace_back(Nested{t, read_once(a.access, on), on, rescan});
            cost(c);
            keep(best, std::move(c), full);
            // Where no key reaches t, a hash join on the join's equalities,
            // built on t or on the outer table. Under at most one outer row,
            // reading t once costs less.
            if (q.left_outer || pr || pr.error().stage != Stage::Reach || o.one_row()) continue;
            for (auto build_inner : {true, false}) {
              if (auto h = hash_join(o, t, a, on, on_double, build_inner)) keep(best, cost_of(std::move(*h)), full);
            }
          }
        }
      }
    }
    // Every table has a full scan, so some plan, complete or failed, reaches
    // them all.
    for (auto& c : best.at(full)) {
      if (!c.failed) out.plans.push_back(std::move(c));
    }
    if (out.plans.empty()) refuse(best.at(full));
    return out;
  }

  // No plan for the statement: the first failure of the furthest stage a
  // plan of every table reached.
  [[noreturn]] static void refuse(const std::vector<Candidate>& failed) {
    const Refusal* furthest = nullptr;
    for (const auto& c : failed) {
      if (furthest == nullptr || c.failed->stage > furthest->stage) furthest = &*c.failed;
    }
    no_plan(furthest->why);
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
    for (const auto& f : a.filters) {
      if (is_compare(f)) add_computed(comp, operand_of(f), table);
    }
    const auto* scan = scan_of(a);
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

  // The ORDER BY a plan sorts its rows by, when it does not give it: each
  // part's column, direction and NULL placement, and the rows it keeps,
  // LIMIT's n, or 0 for all.
  struct SortSpec {
    std::vector<physical::SortPart> keys;
    std::int64_t keep{0};
  };

  // `rows` sorted, a sorter per table of `payloads` holding its row; a row
  // NULL in a column of `not_null` is not added.
  static auto sorted(physical::NodePtr rows, const SortSpec& s, std::vector<std::uint32_t> payloads,
                     std::vector<ColumnRef> not_null = {}) -> physical::NodePtr {
    return physical::node(physical::Sort{std::move(rows), s.keys, s.keep, std::move(payloads), std::move(not_null)});
  }

  // A choice point at one table of a plan (docs/planner_design.md, *Choosing
  // at run time*): the access at `at` (0 the first table's, i + 1 step i's)
  // is the first plan, `other` the second, taken once the count of the
  // first one's range reaches `threshold`. With `sorts_first`, the first
  // plan sorts its rows in its branch and the second gives the ORDER BY: the
  // sort's choice by the memory budget.
  struct ChoiceAt {
    std::size_t at{0};
    Access other;
    std::int64_t threshold{0};
    bool sorts_first{false};
  };

  // A choice point counting the whole range of `counted`, a NULL split's
  // prefix, which holds both of its ranges, or with no prefix the whole
  // object (physical::check's counts_first): `below` under the threshold,
  // `at_or_above` from it. Every choice point is made here.
  static auto choice(const Access& counted, std::int64_t threshold, physical::NodePtr below,
                     physical::NodePtr at_or_above) -> physical::NodePtr {
    physical::Choice ch;
    ch.counted = *scan_of(counted);
    if (std::holds_alternative<physical::NullSplitScan>(counted.path)) {
      ch.counted.rest = ch.counted.prefix.empty() ? physical::ScanRest{physical::Range{}} : physical::ScanRest{};
    }
    ch.threshold = threshold;
    ch.below = std::move(below);
    ch.at_or_above = std::move(at_or_above);
    return physical::node(std::move(ch));
  }

  // The access at `at`, or, at a choice point, the choice between it and
  // the other.
  static auto leaf_at(const Access& a, std::size_t at, const std::vector<ChoiceAt>& choices, const SortSpec* sort)
      -> physical::NodePtr {
    auto it = std::ranges::find(choices, at, &ChoiceAt::at);
    if (it == choices.end()) return leaf(a);
    auto below = it->sorts_first ? sorted(leaf(a), *sort, {scan_of(a)->table}) : leaf(a);
    return choice(a, it->threshold, std::move(below), leaf(it->other));
  }

  // The table a step reads by its own access path.
  static auto rel_of(const Step& s) -> std::size_t {
    if (const auto* h = std::get_if<Hashed>(&s)) return h->rel;
    return std::get<Nested>(s).rel;
  }

  // The candidate's rows, with its choice points. With `sort`, they are
  // sorted: a sorter per loop read above the sort holds that loop's row
  // (held_rels), each fed the same key,
  // and each table looked up by key after the first loop is looked up again
  // from them, above the sort; its filters held already. A hash join's probe
  // side is sorted instead, where it probes in the order (probe_sorted),
  // before it probes, every row kept but those its key's NULLs drop: the
  // join may drop any.
  static auto build(const Query& q, const Candidate& c, const std::vector<ChoiceAt>& choices, const SortSpec* sort)
      -> Built {
    Built out;
    access_computed(out.computed, c.access, q.rels[c.first].table().id);
    auto by_column = false;
    auto nested = false;
    for (std::size_t i = 0; i < c.steps.size(); ++i) {
      if (const auto* pr = std::get_if<Probe>(&c.steps[i])) {
        for (const auto& f : pr->filters) {
          if (is_compare(f)) add_computed(out.computed, operand_of(f), q.rels[pr->rel].table().id);
        }
        by_column = by_column || pr->by_column;
      } else {
        access_computed(out.computed, c.access_at(i + 1), q.rels[rel_of(c.steps[i])].table().id);
        nested = true;
      }
    }
    const auto probe_sort = sort != nullptr && c.probe_sorted ? std::optional{SortSpec{sort->keys, 0}} : std::nullopt;
    auto leaf_of = [&](const Access& a, std::size_t at, std::size_t rel) {
      auto rows = leaf_at(a, at, choices, sort);
      if (!probe_sort || c.driving() != at) return rows;
      // A NULL in the probe key matches nothing: dropped before the sort.
      const auto table = q.rels[rel].table().id;
      std::vector<ColumnRef> keys;
      for (const auto& k : c.hash()->keys) {
        if (k.probe.table == table) keys.push_back(k.probe);
      }
      return sorted(std::move(rows), *probe_sort, {table}, std::move(keys));
    };
    const auto* g = std::get_if<physical::PointGet>(&c.access.path);
    if (!by_column) {
      out.null_checks = c.access.null_checks;
      for (std::size_t i = 0; i < c.steps.size(); ++i) {
        if (const auto* pr = std::get_if<Probe>(&c.steps[i])) {
          for (auto p : key_params(pr->key)) add_param(out.null_checks, p);
        } else {
          for (auto p : c.access_at(i + 1).null_checks) add_param(out.null_checks, p);
        }
      }
      out.rows = leaf_of(c.access, 0, c.first);
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
      out.rows = leaf_of(c.access, 0, c.first);
    }
    // A NULL parameter on any table matches no row.
    if (by_column) {
      for (const auto& rel : q.rels) add_params(out.null_checks, rel.where);
    }
    // A left outer join keeps its left rows whatever its right side's
    // parameters hold: a NULL among them is the inner side's to match
    // nothing, by the join's guard, never the plan's to end on.
    std::vector<std::uint32_t> guard;
    if (q.left_outer) {
      add_params(guard, q.rels[1].where);
      std::erase_if(out.null_checks, [&](std::uint32_t p) { return std::ranges::contains(guard, p); });
      add_params(out.null_checks, q.rels[0].where);
    }
    // The second plan of each choice point checks and computes what it reads.
    for (const auto& ch : choices) {
      for (auto p : ch.other.null_checks) add_param(q.left_outer && ch.at > 0 ? guard : out.null_checks, p);
      const auto rel = ch.at == 0 ? c.first : rel_of(c.steps[ch.at - 1]);
      access_computed(out.computed, ch.other, q.rels[rel].table().id);
    }
    for (std::size_t i = 0; i < c.steps.size(); ++i) {
      if (const auto* pr = std::get_if<Probe>(&c.steps[i])) {
        if (q.left_outer) {
          out.rows = physical::node(
              physical::IndexJoin{std::move(out.rows), q.rels[pr->rel].table().id, pr->key, true, pr->filters});
          continue;
        }
        auto pairs = physical::node(physical::IndexJoin{std::move(out.rows), q.rels[pr->rel].table().id, pr->key});
        out.rows = filtered(std::move(pairs), pr->filters);
      } else if (const auto* h = std::get_if<Hashed>(&c.steps[i])) {
        auto inner = leaf_of(h->access, i + 1, h->rel);
        physical::HashJoin hj;
        hj.keys = h->keys;
        if (h->build_inner) {
          hj.build = std::move(inner);
          hj.probe = std::move(out.rows);
          hj.held = q.rels[h->rel].table().id;
        } else {
          hj.build = std::move(out.rows);
          hj.probe = std::move(inner);
          hj.held = q.rels[c.first].table().id;
        }
        out.rows = filtered(physical::node(std::move(hj)), h->filters);
      } else {
        out.rows = physical::node(physical::NestedLoop{
            std::move(out.rows), leaf_at(std::get<Nested>(c.steps[i]).access, i + 1, choices, sort), q.left_outer,
            q.left_outer ? guard : std::vector<std::uint32_t>{}});
      }
    }
    // An outer join's WHERE on its right side: of the rows it gives, before
    // a sort, so a LIMIT counts the rows it keeps.
    if (!q.post.empty()) {
      for (const auto& f : q.post) {
        if (is_compare(f)) add_computed(out.computed, operand_of(f), col_of(f).table);
      }
      out.rows = filtered(std::move(out.rows), q.post);
    }
    // A choice by the budget sorts in its first plan's branch.
    auto in_choice = std::ranges::any_of(choices, &ChoiceAt::sorts_first);
    if (sort != nullptr && !in_choice && !c.probe_sorted) {
      std::vector<std::uint32_t> payloads;
      for (auto r : held_rels(q, c)) payloads.push_back(q.rels[r].table().id);
      out.rows = sorted(std::move(out.rows), *sort, std::move(payloads));
      for (auto i = c.after_first_loop(); i < c.steps.size(); ++i) {
        if (const auto* pr = std::get_if<Probe>(&c.steps[i])) {
          out.rows = physical::node(physical::IndexJoin{std::move(out.rows), q.rels[pr->rel].table().id, pr->key,
                                                        q.left_outer, q.left_outer ? pr->filters : Conjuncts{}});
        }
      }
    }
    return out;
  }

  // ---------------------------------------------------------- choice points

  // What a finished plan does with its rows, which its cost follows from:
  // emits them under LIMIT, counts their distinct values, sorts them, or
  // none of these. A sort reads every row, holds LIMIT's n of them or all,
  // in `copies` more sorters than one, and makes `again` lookups by key per
  // row held, above it.
  // A grouped aggregate's part (*Aggregates*): its groups held in a
  // structure (`hash`) or streamed, the rows it groups by `keys` columns, its
  // HAVING keeping a share of them, and a sort of them.
  struct GroupCost {
    bool on{false};
    bool hash{false};
    std::size_t keys{0};
    double kept{1};
    bool sorted{false};
  };
  struct Sink {
    std::optional<std::int64_t> limit;
    bool distinct{false};
    bool sorted{false};
    std::size_t again{0};
    std::size_t copies{0};
    GroupCost group{};
  };

  // The groups of `rows` rows by `keys` columns, as the cost model estimates
  // them: one per row × 1/10 per column, as many equalities would keep; at
  // least one, and one with no column.
  static auto groups_of(double rows, std::size_t keys) -> double {
    if (keys == 0) return 1;
    return std::max(rows * std::pow(kEqSelectivity, static_cast<double>(keys)), 1.0);
  }

  static auto finished_cost(const Sink& s, const Measured& m) -> double {
    auto [cost, rows, fixed] = m;
    if (s.group.on) {
      // A COUNT(DISTINCT) finds each row's value in its structure.
      if (s.distinct) cost += rows;
      // A structure of groups costs as a hash build: each row held at the
      // hash weight, then a step of bc_agg_next per group, all after the
      // last row is read.
      auto groups = groups_of(rows, s.group.keys);
      if (s.group.hash) {
        cost += rows * (1 + kHashWeight);
        fixed = cost;
        cost += groups;
      }
      auto out = groups * s.group.kept;
      if (s.group.sorted) return cost + out * (1 + kSortWeight);
      if (s.limit && out > static_cast<double>(*s.limit)) cost = fixed + (cost - fixed) * static_cast<double>(*s.limit) / out;
      return cost;
    }
    // COUNT(DISTINCT) steps through its groups once more.
    if (s.distinct) cost += rows;
    // A sort holds its rows, each a step of bc_sorter_next and its weight.
    // Each further sorter holds a copy at the weight, read by bc_sorter_nth,
    // which steps no cursor.
    if (s.sorted) {
      auto held = s.limit ? std::min(rows, static_cast<double>(*s.limit)) : rows;
      return cost + held * (1 + kSortWeight + static_cast<double>(s.again)) +
             held * kSortWeight * static_cast<double>(s.copies);
    }
    // A LIMIT stops the loop once it has its rows, past what a hash join
    // does before its first.
    if (s.limit && rows > static_cast<double>(*s.limit)) cost = fixed + (cost - fixed) * static_cast<double>(*s.limit) / rows;
    return cost;
  }

  // The entries `b`'s range reaches when `a`'s reaches x: at least x when
  // b's key takes only conjuncts a's takes, so its range holds a's rows; at
  // most x when a's takes only conjuncts b's takes; else its estimate.
  static auto entries_beside(const Access& a, const Access& b, double x) -> double {
    auto within = [](const std::vector<std::size_t>& p, const std::vector<std::size_t>& q) {
      return std::ranges::all_of(p, [&](std::size_t i) { return std::ranges::find(q, i) != q.end(); });
    };
    auto e = b.entries;
    if (within(b.keyed, a.keyed)) e = std::max(e, x);
    if (within(a.keyed, b.keyed)) e = std::min(e, x);
    return std::min(e, b.max_entries);
  }

  // The sort's choice by the memory budget, for a table alone, where another
  // plan gives the ORDER BY: below the least count of the sorted range past
  // the budget, the sort; from it, the cheapest plan that gives the order
  // instead. The rows held are at most the range's count, a full scan's the
  // table's, whatever the filters keep, and that count is read when the plan
  // runs, so it holds as the table grows. A sorter of n entries holds at
  // least kSorterBytes and n × kSortEntryBytes. None under a LIMIT the budget
  // holds.
  auto by_budget(const Candidate& c, const std::vector<Tried>& tried, const SortSpec& sort, const Sink& sink) const
      -> std::optional<ChoiceAt> {
    if (memory_ == 0 || !c.steps.empty()) return std::nullopt;
    auto past = least_past(memory_, kSorterBytes, kSortEntryBytes);
    if (sort.keep > 0 && static_cast<std::uint64_t>(sort.keep) < past) return std::nullopt;
    const auto* sa = scan_of(c.access);
    std::optional<ChoiceAt> best;
    auto best_cost = 0.0;
    for (const auto& t : tried) {
      if (t.failed || t.one_row() || !t.access.ordered) continue;
      const auto* sb = scan_of(t.access);
      if (sb->object == sa->object && t.access.keyed == c.access.keyed) continue;  // the same range
      auto other = c;
      other.access = t.access;
      auto cost = finished_cost(Sink{sink.limit, false, false, 0}, measure(other));
      if (!best || cost < best_cost) {
        best = ChoiceAt{0, t.access, static_cast<std::int64_t>(past), true};
        best_cost = cost;
      }
    }
    return best;
  }

  // What a choice point's second plan must keep of the first's: the ORDER
  // BY's order, which a plan gives rather than sorts (`order`); the groups a
  // streamed GROUP BY reads (`groups`); and rows, not a covering scan's
  // entries, of the tables `rows` names, by position in FROM order, whose
  // rows a structure of groups holds.
  struct Needs {
    bool order{false};
    bool groups{false};
    std::vector<std::size_t> rows;
  };

  // The plan's choice points, at most kMaxChoices: at each table it reads by
  // a scan, outer first, the second plan is the cheapest other range of the
  // table whose cost crosses the first one's within the count of its range.
  // Never at a point lookup, nor at a scan whose bounds name at most one
  // row, nor at a full scan, whose count no parameter changes, nor at a scan
  // read once per outer row; and the second plan is never a full scan. In a
  // plan that gives the ORDER BY by its order (`needs.order`), the loop's
  // second plan gives it too, and in one that streams groups
  // (`needs.groups`), the loop's second plan gives them in groups. A sorted
  // plan of one table takes the choice by the budget (by_budget) there
  // instead, at a full scan too, its second plan any that gives the order.
  // In a sorted plan, the second plan of a loop a sorter holds reads rows,
  // as its first does, and so does one of a table in `needs.rows`. `tried`
  // is each table's access paths.
  auto choose(const Query& q, const Candidate& c, const Sink& sink, const std::vector<std::vector<Tried>>& tried,
              const Needs& needs, const SortSpec* sort) const -> std::vector<ChoiceAt> {
    std::vector<ChoiceAt> out;
    const auto loop = c.driving();
    auto held = sort != nullptr ? held_rels(q, c) : std::vector<std::size_t>{};
    held.insert(held.end(), needs.rows.begin(), needs.rows.end());
    for (std::size_t at = 0; at <= c.steps.size() && out.size() < kMaxChoices; ++at) {
      const auto* ne = at == 0 ? nullptr : std::get_if<Nested>(&c.steps[at - 1]);
      if (at > 0 && (ne == nullptr || ne->rescan)) continue;  // a lookup by key, or a count per outer row
      const auto& a = c.access_at(at);
      const auto rel = at == 0 ? c.first : ne->rel;
      if (scan_of(a) == nullptr || a.max_entries <= 1) continue;
      if (sort != nullptr && at == loop) {
        if (auto b = by_budget(c, tried[rel], *sort, sink)) {
          out.push_back(std::move(*b));
          continue;
        }
      }
      if (full_scan(a)) continue;
      auto first = [&](double x) { return finished_cost(sink, measure(c, Entries{at, x})); };
      std::optional<ChoiceAt> best;
      auto best_cost = 0.0;
      for (const auto& t : tried[rel]) {
        if (t.failed || t.one_row() || full_scan(t.access)) continue;
        if (needs.order && at == loop && !t.access.ordered) continue;
        if (needs.groups && at == loop && !t.access.grouped) continue;
        const auto* sb = scan_of(t.access);
        if (sb->object == scan_of(a)->object && t.access.keyed == a.keyed) continue;  // the same range
        auto b = ne == nullptr ? t.access : read_once(t.access, ne->on);
        if (std::ranges::find(held, rel) != held.end()) b = uncovered(std::move(b));
        auto other = c;
        other.access_at(at) = b;
        auto second = [&](double x) { return finished_cost(sink, measure(other, Entries{at, entries_beside(a, b, x)})); };
        auto th = crossing(first, second, a.max_entries);
        if (!th) continue;
        auto cost = finished_cost(sink, measure(other));
        if (!best || cost < best_cost) {
          best = ChoiceAt{at, std::move(b), *th, false};
          best_cost = cost;
        }
      }
      if (best) out.push_back(std::move(*best));
    }
    return out;
  }

  // A candidate made ready for `order` (the statement's ORDER BY, or null):
  // where it does not give it, it is sorted (read_for_sort), by the sink, a
  // sorter per loop it holds, or, for a hash join, on its probe side. Whether
  // it sorts.
  static auto prepare(const Query& q, Candidate& c, Sink& sink, const SortSpec* order) -> bool {
    if (order == nullptr || c.ordered()) return false;
    read_for_sort(q, c);
    if (!c.probe_sorted) {
      sink.sorted = true;
      sink.copies = std::max<std::size_t>(held_rels(q, c).size(), 1) - 1;
      sink.again = static_cast<std::size_t>(std::ranges::count_if(
          c.steps.begin() + static_cast<std::ptrdiff_t>(c.after_first_loop()), c.steps.end(),
          [](const Step& st) { return std::holds_alternative<Probe>(st); }));
    }
    costed(c);
    return true;
  }

  // The tables, by position in FROM order, of the loops whose rows a sort
  // above the candidate holds, outer first: those read above it, by the
  // result or by a lookup by key repeated there. Every other loop's row is
  // read only below the sort, for its key, filters and joins, and is not
  // held; with none held, one sorter holds the keys alone.
  static auto held_rels(const Query& q, const Candidate& c) -> std::vector<std::size_t> {
    auto read = q.above;
    for (auto i = c.after_first_loop(); i < c.steps.size(); ++i) {
      if (const auto* pr = std::get_if<Probe>(&c.steps[i])) {
        for (const auto& e : pr->key) logical::columns_of(e, read);
      }
    }
    std::vector<std::size_t> out;
    for (auto r : c.loop_rels()) {
      const auto id = q.rels[r].table().id;
      if (std::ranges::any_of(read, [&](const ColumnRef& col) { return col.table == id; })) out.push_back(r);
    }
    return out;
  }

  // A candidate not in the ORDER BY's order made ready for its sort, decided
  // before any access is uncovered: a hash join whose ORDER BY is all of its
  // probe side's loop's table sorts its probe side (probe_sorted), whose loop
  // alone reads rows; any other plan is sorted above, each loop a sorter
  // holds (held_rels) reading rows.
  static void read_for_sort(const Query& q, Candidate& c) {
    const auto d = *c.driving();
    const auto rel = d == 0 ? c.first : rel_of(c.steps[d - 1]);
    const auto table = q.rels[rel].table().id;
    c.probe_sorted = c.hash() != nullptr &&
                     std::ranges::all_of(q.sort_by, [&](const ColumnRef& col) { return col.table == table; });
    read_rows(c, c.probe_sorted ? std::vector<std::size_t>{rel} : held_rels(q, c));
  }

  // A count of rows past which a hash table of `h`'s build may not fit the
  // memory budget: one of n rows holds kHashBytes, and per row
  // kHashEntryBytes and its row, no smaller than the fewest bytes a row can
  // hold (least_row_bytes), and per distinct key its bytes, here a marker
  // and the value of each key part, an empty one's for a byte string. This
  // counts a key's bytes per row, as if no two rows shared a key, so it is
  // conservative: it may switch to the other plan at fewer rows than the
  // least that would pass, never at more. A build under it fits whenever
  // its rows and keys are no larger than those bounds.
  auto hash_past(const Query& q, const Candidate& c, const Hashed& h) const -> std::uint64_t {
    const auto& held = q.rels[h.build_inner ? h.rel : c.first].table();
    std::uint64_t key = 0;
    for (const auto& k : h.keys) key += least_part_bytes(k.build, false);  // a NULL key is never put
    return least_past(memory_, kHashBytes, kHashEntryBytes + least_row_bytes(held) + key);
  }

  // The cheapest finished plan of the statement without a hash join, the
  // ORDER BY it sorts by, or null, and the query it was made ready under,
  // whose sort_by and above it is built by: what a hash join's choice by the
  // budget takes as its other plan. nullopt with no budget, or none.
  struct Avoiding {
    Candidate plan;
    const SortSpec* sort{nullptr};
    Query query;
  };
  auto without_hash(const Query& q, const Searched& searched, const Sink& sink, const SortSpec* order) const
      -> std::optional<Avoiding> {
    if (memory_ == 0) return std::nullopt;
    std::optional<Avoiding> out;
    auto out_cost = 0.0;
    for (auto o : searched.plans) {
      if (o.hash() != nullptr) continue;
      auto s = sink;
      prepare(q, o, s, order);
      auto oc = finished_cost(s, measure(o));
      if (!out || oc < out_cost) {
        out = Avoiding{std::move(o), s.sorted ? order : nullptr, q};
        out_cost = oc;
      }
    }
    return out;
  }

  // A candidate finished, for `order`, the statement's ORDER BY or null: sorted
  // where it does not give it (prepare); its choice points chosen, its rows
  // built and wrapped by the sink, its cost the sink's plus each count's. A
  // hash join takes the choice by the budget instead (finish_hash), its other
  // plan `avoiding`; nullopt when it is left out.
  template <typename Wrap>
  auto finish(const Query& q, Candidate c, Sink sink, const Searched& searched, const SortSpec* order,
              const std::optional<Avoiding>& avoiding, const Wrap& wrap) const -> std::optional<Finished> {
    auto sorts = prepare(q, c, sink, order);
    return finish_prepared(q, std::move(c), sink, searched, sorts ? order : nullptr, Needs{order != nullptr && !sorts, false, {}},
                           avoiding, wrap);
  }

  // A candidate made ready (prepare, or a GROUP BY's own), finished: sorted
  // by `sort` above it, or not; a hash join by finish_hash, any other plan
  // its choice points by costs chosen (choose), keeping what `needs` says.
  // Its rows built and wrapped, its cost the sink's plus each count's.
  template <typename Wrap>
  auto finish_prepared(const Query& q, Candidate c, const Sink& sink, const Searched& searched, const SortSpec* sort,
                       Needs needs, const std::optional<Avoiding>& avoiding, const Wrap& wrap) const
      -> std::optional<Finished> {
    if (c.hash() != nullptr) return finish_hash(q, std::move(c), sink, sort, avoiding, wrap);
    auto choices = choose(q, c, sink, searched.tried, needs, sort);
    auto b = build(q, c, choices, sort);
    auto cost = finished_cost(sink, measure(c)) + kCountCost * static_cast<double>(choices.size());
    return Finished{wrap(std::move(b.rows)), std::move(b.null_checks), std::move(b.computed), cost};
  }

  // The parameters `from`'s rows test for NULL and the values it computes,
  // added to `into`'s: a choice point's two plans run under one check.
  static void merge_params(Built& into, const Built& from) {
    for (auto p : from.null_checks) add_param(into.null_checks, p);
    for (const auto& v : from.computed) {
      if (std::ranges::find(into.computed, v) == into.computed.end()) into.computed.push_back(v);
    }
  }

  // A hash join finished (*Memory*, the planner uses the budget): its build
  // side's rows are held, so where they may pass the memory budget and
  // another plan of the statement avoids the hash table, `avoiding`, that
  // plan is the second plan of a choice point, counting the build side's
  // range, a full scan's the whole table, limited to a count past
  // which it may not fit (hash_past). A build known not to fit, of a range of
  // at most one row, is left out where another plan avoids it, and one known
  // to fit takes no choice point. Where none avoids it, the hash join runs,
  // and past the budget fails with 54000. No choice point by costs is at
  // either side.
  template <typename Wrap>
  auto finish_hash(const Query& q, Candidate c, const Sink& sink, const SortSpec* sort,
                   const std::optional<Avoiding>& avoiding, const Wrap& wrap) const -> std::optional<Finished> {
    const auto& h = *c.hash();
    const auto& built = h.build_inner ? h.access : c.access;
    auto b = build(q, c, {}, sort);
    auto cost = finished_cost(sink, measure(c));
    const auto past = hash_past(q, c, h);
    const auto known = past == 0 || built.max_entries <= 1;
    if (!avoiding || (known && built.max_entries < static_cast<double>(past))) {
      return Finished{wrap(std::move(b.rows)), std::move(b.null_checks), std::move(b.computed), cost};
    }
    if (known) return std::nullopt;
    auto ob = build(avoiding->query, avoiding->plan, {}, avoiding->sort);
    merge_params(b, ob);
    auto rows = choice(built, static_cast<std::int64_t>(past), std::move(b.rows), std::move(ob.rows));
    return Finished{wrap(std::move(rows)), std::move(b.null_checks), std::move(b.computed), cost + kCountCost};
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
    auto searched = candidates(q, {}, {}, false, false);
    for (const auto& c : searched.plans) {
      if (auto f = finish(q, c, Sink{}, searched, nullptr, std::nullopt, wrap)) out.push_back(std::move(*f));
    }
    return out;
  }

  void result(const SelectParts& s) {
    const auto& outs = s.project != nullptr ? s.project->exprs : s.aggregate->aggs;
    for (const auto& o : outs) result_.push_back({o.name, o.expr.type});
  }

  // Each column the SELECT names is a column of one of its tables.
  static void check_columns(const SelectParts& s, const Tables& tables) {
    auto column = [&](const Expr& e) { return reads(tables, e); };
    if (s.project != nullptr) {
      for (const auto& o : s.project->exprs) {
        if (!column(o.expr) && o.expr.kind != Expr::Kind::Param) {
          no_plan("a result column that is not a column of its tables or a constant");
        }
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
    out.push_back({emit(s, std::move(leaf), emit_targets(s)), {}, {}, 0});
    return out;
  }

  // A SELECT without FROM: its one row of constants, nothing read.
  static auto one_row(const SelectParts& s) -> std::vector<Finished> {
    if (s.project == nullptr) no_plan("an Aggregate of no table");
    std::vector<Finished> out;
    out.push_back({emit(s, physical::node(physical::OneRow{}), emit_targets(s)), {}, {}, 0});
    return out;
  }

  // The SELECT's result columns: its columns, and its constants, which the
  // binder made parameters.
  static auto emit_targets(const SelectParts& s) -> std::vector<physical::EmitTarget> {
    std::vector<physical::EmitTarget> out;
    for (const auto& o : s.project->exprs) {
      if (o.expr.kind == Expr::Kind::Param) {
        out.emplace_back(physical::ParamTarget{o.expr.param, o.expr.phys()});
      } else {
        out.emplace_back(o.expr.col);
      }
    }
    return out;
  }

  // Emit, under LIMIT.
  static auto emit(const SelectParts& s, physical::NodePtr rows, std::vector<physical::EmitTarget> targets)
      -> physical::NodePtr {
    if (s.limit) rows = physical::node(physical::Limit{std::move(rows), *s.limit});
    return physical::node(physical::Emit{std::move(rows), std::move(targets)});
  }

  // The ORDER BY, without the columns the WHERE makes constant and those
  // already ordered by.
  static auto ordered_items(const Query& q, const std::vector<OrderItem>& order_by) -> std::vector<OrderItem> {
    std::vector<OrderItem> out;
    for (const auto& o : order_by) {
      const auto& where = q.rels[q.index_of(o.col.table)].where;
      auto constant = std::ranges::any_of(where, [&](const auto& w) { return on_col(w, o.col.col) && w.op == Op::Eq; });
      auto repeated = std::ranges::any_of(out, [&](const auto& p) { return p.col == o.col; });
      if (!constant && !repeated) out.push_back(o);
    }
    return out;
  }

  auto select(const logical::Node& root) -> std::vector<Finished> {
    if (grouped(root)) return group_select(root);
    auto s = select_parts(root);
    result(s);
    if (const auto* e = std::get_if<logical::Empty>(&s.source->op)) return nothing(s, *e);
    if (std::holds_alternative<logical::OneRow>(s.source->op)) return one_row(s);
    Query q;
    flatten(*s.source, q);
    for (const auto& rel : q.rels) constants(rel.table());
    Tables tables = tables_of(q, 0);
    check_columns(s, tables);
    // LIMIT 0 returns no row, in any order.
    std::vector<OrderItem> order_by;
    if (s.sort != nullptr && (!s.limit || *s.limit > 0)) {
      for (const auto& k : s.sort->keys) order_by.push_back({k.expr.col, k.desc, k.nulls_first});
    }
    const auto ordered = ordered_items(q, order_by);
    SortSpec sort{{}, s.limit.value_or(0)};
    for (const auto& o : ordered) sort.keys.push_back({o.col, o.desc, o.nulls_first});
    const auto* agg = s.aggregate != nullptr ? &s.aggregate->aggs[0].expr : nullptr;
    auto for_update = std::ranges::any_of(q.rels, [](const Rel& r) { return r.get->for_update; });
    // FOR UPDATE reads each row, whose key it guards; COUNT(DISTINCT) keys
    // each value from its row.
    auto distinct = agg != nullptr && agg->distinct;
    auto sorting = agg == nullptr && !ordered.empty();
    // A loop no sorter holds builds the sort key from its entry, which a
    // covering scan's must hold.
    auto used = s.below->output;
    if (sorting) {
      for (const auto& o : ordered) {
        q.sort_by.push_back(o.col);
        if (std::ranges::find(used, o.col) == used.end()) used.push_back(o.col);
      }
      for (const auto& o : s.project->exprs) {
        if (o.expr.kind == Expr::Kind::Column) q.above.push_back(o.expr.col);
      }
    }
    auto searched = candidates(q, ordered, used, !for_update && !distinct, sorting);
    auto emits = !s.limit || *s.limit > 0;
    const auto sink = agg == nullptr ? Sink{s.limit} : Sink{std::nullopt, distinct};
    const auto* order = sorting ? &sort : nullptr;
    const auto hashes = std::ranges::any_of(searched.plans, [](const Candidate& c) { return c.hash() != nullptr; });
    const auto avoiding = hashes ? without_hash(q, searched, sink, order) : std::nullopt;
    std::vector<Finished> out;
    for (const auto& c : searched.plans) {
      if (agg == nullptr) {
        // A plan not in the ORDER BY's order sorts its rows.
        auto f = finish(q, c, sink, searched, order, avoiding,
                        [&](physical::NodePtr rows) { return emit(s, std::move(rows), emit_targets(s)); });
        if (f) out.push_back(std::move(*f));
        continue;
      }
      // The keys answer COUNT when nothing filters a row.
      const auto* scan = bare_scan(c);
      if (scan != nullptr && !agg->distinct && agg->fn == Expr::Fn::Count) {
        // Every entry of the range is a row to count, unless the argument can
        // be NULL in one: the WHERE compares every column it names, so those
        // hold none.
        const auto& where = q.rels[0].where;
        const auto* arg = agg->args.empty() ? nullptr : &agg->args[0].col;
        auto may_be_null = arg != nullptr && arg->nullable &&
                           std::ranges::none_of(where, [&](const auto& w) { return on_col(w, arg->col); });
        if (!may_be_null) {
          auto b = build(q, c, {}, nullptr);
          out.push_back({physical::node(physical::CountRange{*scan, emits}), std::move(b.null_checks), std::move(b.computed),
                         kCountCost});
        }
      }
      auto f = finish(q, c, sink, searched, nullptr, avoiding, [&](physical::NodePtr rows) {
        return physical::node(physical::Aggregate{std::move(rows), *agg, emits});
      });
      if (f) out.push_back(std::move(*f));
    }
    return out;
  }

  // ------------------------------------------------------------- GROUP BY

  // The parts of an aggregated SELECT the grouped aggregate plans: Limit? ->
  // Sort? -> Filter? (its HAVING) -> Aggregate -> its source.
  struct GroupParts {
    std::optional<std::int64_t> limit;
    const logical::Sort* sort{nullptr};
    const logical::Filter* having{nullptr};
    const logical::Aggregate* aggregate{nullptr};
  };

  // Whether a SELECT is aggregated other than as slice 1's one aggregate row
  // (a COUNT or a SUM, no GROUP BY, no HAVING): grouped, or of other
  // aggregates, or several. A lone MIN takes the grouped route, as a lone
  // MAX does, and from_keys answers either there.
  static auto grouped(const logical::Node& root) -> bool {
    const auto* n = &root;
    if (const auto* l = std::get_if<logical::Limit>(&n->op)) n = l->input.get();
    // A Sort or a Filter at the top over an Aggregate: a SELECT that is not
    // aggregated sorts below its Project.
    if (const auto* o = std::get_if<logical::Sort>(&n->op)) n = o->input.get();
    if (const auto* f = std::get_if<logical::Filter>(&n->op)) {
      return std::holds_alternative<logical::Aggregate>(f->input->op);
    }
    const auto* a = std::get_if<logical::Aggregate>(&n->op);
    if (a == nullptr) return false;
    if (a->grouped || !a->group_by.empty() || a->aggs.size() != 1) return true;
    const auto& e = a->aggs[0].expr;
    return e.kind != Expr::Kind::Aggregate ||
           (e.fn != Expr::Fn::Count && e.fn != Expr::Fn::Sum);
  }

  static auto group_parts(const logical::Node& root) -> GroupParts {
    GroupParts g;
    const auto* n = &root;
    if (const auto* l = std::get_if<logical::Limit>(&n->op)) {
      g.limit = l->n;
      n = l->input.get();
    }
    if (const auto* o = std::get_if<logical::Sort>(&n->op)) {
      g.sort = o;
      n = o->input.get();
    }
    if (const auto* f = std::get_if<logical::Filter>(&n->op)) {
      g.having = f;
      n = f->input.get();
    }
    g.aggregate = std::get_if<logical::Aggregate>(&n->op);
    if (g.aggregate == nullptr) no_plan(with_article(op_name(*n)) + " where the search plans an Aggregate");
    return g;
  }

  // The least count of its input's rows past which a structure of groups
  // may not fit the memory budget (*Memory*); 0 for a budget under an empty
  // one's. It holds kAggBytes and a byte per slot, kHashBytes per table
  // whose row a group holds, kAggBytes and a slot per COUNT(DISTINCT), and
  // kSorterBytes where it sorts its groups; and per row, as if each started
  // a group of its own and gave each COUNT(DISTINCT) a new value:
  // - the group: its key, kGroupEntryBytes and kAggSlotBytes per slot;
  // - per held table: the row, kHashEntryBytes and the key, new to it;
  // - per COUNT(DISTINCT): the group's key and the value's part, as one key,
  //   kGroupEntryBytes and one slot;
  // - in a sort: its sort key, the group's key as payload, kSortEntryBytes.
  // Each key part is at least least_part_bytes, each row least_row_bytes.
  // Counting each row a group and a new value is conservative: where rows
  // share groups or values, it switches at fewer rows than the least that
  // would pass, never at more. Under it a structure fits whenever its rows
  // and key parts are no larger than those bounds.
  auto group_past(const Query& q, const physical::GroupLayout& l, const std::vector<ColumnRef>& keys,
                  const SortSpec* sort) const -> std::uint64_t {
    std::uint64_t key = 0;
    for (const auto& k : keys) key += least_part_bytes(k, k.nullable);
    auto fixed = kAggBytes + l.slots + l.held.size() * kHashBytes + l.distinct.size() * (kAggBytes + 1) +
                 (sort != nullptr ? kSorterBytes : 0);
    auto per = key + kGroupEntryBytes + l.slots * kAggSlotBytes;
    for (auto t : l.held) per += least_row_bytes(q.rels[q.index_of(t)].table()) + kHashEntryBytes + key;
    // The group's key is one byte-string part of the value's key: at least
    // its marker and its bytes.
    for (const auto& d : l.distinct) per += 1 + key + least_part_bytes(d, false) + kGroupEntryBytes + kAggSlotBytes;
    if (sort != nullptr) {
      per += kSortEntryBytes + key;
      for (const auto& part : sort->keys) per += least_part_bytes(part.col, part.col.nullable);
    }
    return least_past(memory_, fixed, per);
  }

  // An aggregated SELECT (docs/planner_design.md, *Aggregates*). For
  // each plan of its rows, the groups in a structure (bc_agg_*), and, where
  // the plan's rows come in groups, streamed with none; with no GROUP BY one
  // group, streamed. Its HAVING filters the groups, an ORDER BY its plan's
  // order does not give sorts them, and its LIMIT counts them. Each plan is
  // finished as an ungrouped one is (finish_prepared): a hash join takes the
  // choice by the budget at its build, any other plan its choice points by
  // costs. A structure whose groups may pass the memory budget, where a
  // streamed plan avoids it, is instead the first plan of a choice point by
  // the count of its input's range, the cheapest streamed plan the second.
  auto group_select(const logical::Node& root) -> std::vector<Finished> {
    const auto g = group_parts(root);
    const auto& agg = *g.aggregate;
    for (const auto& o : agg.aggs) {
      result_.push_back({o.name, o.expr.type});
    }
    auto columns_of_exprs = [](const std::vector<Expr>& in, const char* what) {
      std::vector<ColumnRef> out;
      for (const auto& e : in) {
        if (e.kind != Expr::Kind::Column) no_plan(what);
        out.push_back(e.col);
      }
      return out;
    };
    const auto keys = columns_of_exprs(agg.group_by, "a GROUP BY key that is not a column");
    const auto dependent = columns_of_exprs(agg.dependent, "a dependent column that is not a column");
    // What a group gives of its rows: its keys, then the columns they
    // determine.
    auto columns = keys;
    columns.insert(columns.end(), dependent.begin(), dependent.end());
    auto of_group = [&](const ColumnRef& c) { return std::ranges::find(columns, c) != columns.end(); };
    std::vector<Expr> aggs;
    auto add_agg = [&](const Expr& e) {
      if (std::ranges::find(aggs, e) == aggs.end()) aggs.push_back(e);
    };
    std::vector<Expr> outputs;
    for (const auto& o : agg.aggs) {
      if (o.expr.kind == Expr::Kind::Aggregate) {
        add_agg(o.expr);
      } else if (o.expr.kind != Expr::Kind::Column || !of_group(o.expr.col)) {
        no_plan("a result column that is not grouped");
      }
      outputs.push_back(o.expr);
    }
    Conjuncts having = g.having != nullptr ? g.having->conjuncts : Conjuncts{};
    for (const auto& c : having) {
      if (!is_compare(c) || operand_of(c).kind != Expr::Kind::Param) no_plan("a HAVING conjunct that does not compare with a parameter");
      const auto& lhs = c.args[0];
      if (lhs.kind == Expr::Kind::Aggregate) {
        add_agg(lhs);
      } else if (lhs.kind != Expr::Kind::Column || !of_group(lhs.col)) {
        no_plan("a HAVING conjunct that compares no grouped column or aggregate");
      }
    }
    for (const auto& a : aggs) {
      if ((a.fn != Expr::Fn::Count || a.distinct) && (a.args.empty() || a.args[0].kind != Expr::Kind::Column)) {
        no_plan("an aggregate call that does not take a column of its tables");
      }
    }
    std::vector<OrderItem> order_by;
    if (g.sort != nullptr && (!g.limit || *g.limit > 0)) {
      for (const auto& k : g.sort->keys) {
        if (k.expr.kind != Expr::Kind::Column || !of_group(k.expr.col)) no_plan("an ORDER BY key that is not grouped");
        order_by.push_back({k.expr.col, k.desc, k.nulls_first});
      }
    }
    // The rows of the groups: GroupEmit over Limit? over the branch, or a
    // choice point between two.
    auto branch = [&](physical::NodePtr rows, bool streamed, const SortSpec* sort) {
      auto n = physical::node(physical::GroupAggregate{std::move(rows), keys, aggs, streamed, dependent});
      if (!having.empty()) n = physical::node(physical::Filter{std::move(n), having});
      if (sort != nullptr) n = physical::node(physical::Sort{std::move(n), sort->keys, sort->keep, {}, {}, true});
      return n;
    };
    auto rooted = [&](physical::NodePtr rows) {
      if (g.limit) rows = physical::node(physical::Limit{std::move(rows), *g.limit});
      return physical::node(physical::GroupEmit{std::move(rows), outputs});
    };
    std::vector<Finished> out;
    const auto* source = agg.input.get();
    if (const auto* e = std::get_if<logical::Empty>(&source->op)) {
      for (const auto* t : e->tables) constants(*t);
      out.push_back({rooted(branch(physical::node(physical::Empty{}), keys.empty(), nullptr)), {}, {}, 0});
      return out;
    }
    Query q;
    flatten(*source, q);
    for (const auto& rel : q.rels) constants(rel.table());
    const Tables tables = tables_of(q, 0);
    auto of_tables = [&](const ColumnRef& c) { return std::ranges::find(tables, c.table) != tables.end(); };
    if (!std::ranges::all_of(columns, of_tables)) no_plan("a GROUP BY key that is not a column of its tables");
    for (const auto& a : aggs) {
      if (!a.args.empty() && !of_tables(a.args[0].col)) no_plan("an aggregate call that does not take a column of its tables");
    }
    // A HAVING on a DOUBLE key: its comparisons need PostgreSQL's NaN and
    // -0.0 rules, as a filter's do.
    for (const auto& c : having) {
      if (c.args[0].kind == Expr::Kind::Column && c.args[0].col.type == PhysType::F64) {
        no_plan(double_filter(name_of(c.args[0].col)).why);
      }
    }
    const auto ordered = ordered_items(q, order_by);
    SortSpec sort{{}, g.limit.value_or(0)};
    for (const auto& o : ordered) sort.keys.push_back({o.col, o.desc, o.nulls_first});
    // The keys the WHERE does not make constant, which a stream's rows come
    // grouped by, and which number the groups. With no GROUP BY, no
    // grouping: one group of every row, which any plan gives.
    std::vector<ColumnRef> grouping;
    for (const auto& k : keys) {
      const auto& where = q.rels[q.index_of(k.table)].where;
      if (std::ranges::none_of(where, [&](const auto& w) { return on_col(w, k.col) && w.op == Op::Eq; })) grouping.push_back(k);
    }
    auto distinct = std::ranges::any_of(aggs, [](const Expr& a) { return a.distinct; });
    // A stream keeps its columns in locals, integers and booleans, read from
    // a row or a covering scan's entry. A COUNT(DISTINCT) keys each value
    // from its row; a structure's columns other than integers are read from
    // a row its group holds, so its plan reads those tables' rows.
    auto in_locals = std::ranges::all_of(columns, [](const ColumnRef& k) {
      return k.type == PhysType::I64 || k.type == PhysType::Bool;
    });
    auto may_cover = !distinct && in_locals;
    auto searched =
        candidates(q, ordered, agg.input->output, may_cover, false, keys.empty() ? Grouping{} : Grouping{grouping});
    const auto layout = physical::group_layout(columns, aggs);
    std::vector<std::size_t> held;
    for (auto t : layout.held) held.push_back(q.index_of(t));
    auto kept = selectivity(having);
    auto sink_of = [&](bool hash, bool sorted) {
      return Sink{g.limit, distinct, false, 0, 0, GroupCost{true, hash, grouping.size(), kept, sorted}};
    };
    // One group of COUNT(*), MIN and MAX alone, nothing filtering a row
    // after its range: answered from the keys.
    if (keys.empty() && having.empty()) {
      for (const auto& c : searched.plans) {
        if (auto f = from_keys(q, c, outputs, !g.limit || *g.limit > 0)) out.push_back(std::move(*f));
      }
    }
    const auto hashes = std::ranges::any_of(searched.plans, [](const Candidate& c) { return c.hash() != nullptr; });
    // A hash join streams only one group, of no key.
    const auto avoiding = hashes && keys.empty() ? without_hash(q, searched, sink_of(false, false), nullptr) : std::nullopt;
    // Streamed: every plan whose rows come in groups, in the ORDER BY's order
    // where it has one, since nothing holds its groups to sort them. The
    // cheapest one that is not a hash join is a structure's other plan, built
    // there with no choice point, so compared by that plan's cost.
    const Candidate* stream = nullptr;
    auto stream_cost = 0.0;
    for (const auto& c : searched.plans) {
      if (!(keys.empty() || (in_locals && c.streams())) || (!ordered.empty() && !c.ordered())) continue;
      auto sink = sink_of(false, false);
      auto f = finish_prepared(q, c, sink, searched, nullptr, Needs{!ordered.empty(), !keys.empty(), {}}, avoiding,
                               [&](physical::NodePtr rows) { return rooted(branch(std::move(rows), true, nullptr)); });
      if (!f) continue;
      if (c.hash() == nullptr) {
        const auto built = finished_cost(sink, measure(c));
        if (stream == nullptr || built < stream_cost) {
          stream = &c;
          stream_cost = built;
        }
      }
      out.push_back(std::move(*f));
    }
    if (keys.empty()) return out;
    // In a structure: every plan, sorted where it does not give the ORDER BY,
    // since groups come in the order their first rows do, each table whose
    // row it holds read by rows; a hash join's other plan, by the budget, of
    // those plans too.
    std::optional<Searched> read;
    if (!held.empty()) {
      read = searched;
      for (auto& c : read->plans) {
        read_rows(c, held);
        costed(c);
      }
    }
    const auto& structured = read ? *read : searched;
    // A hash join's other plan is sorted above the groups where the hash
    // join is; where the hash join gives the ORDER BY by its order, its
    // other plan sorts its rows by it, every row kept, since a structure's
    // groups come in the order of their first rows. Its sorters hold the
    // rows of every table whose columns the groups read.
    const SortSpec rows_sort{sort.keys, 0};
    std::optional<Avoiding> avoiding_unordered;
    std::optional<Avoiding> avoiding_ordered;
    if (hashes) {
      avoiding_unordered = without_hash(q, structured, sink_of(true, false), nullptr);
      if (!ordered.empty()) {
        auto sorted = q;
        sorted.sort_by.clear();
        for (const auto& o : ordered) sorted.sort_by.push_back(o.col);
        sorted.above = columns;
        for (const auto& a : aggs) {
          if (!a.args.empty()) sorted.above.push_back(a.args[0].col);
        }
        avoiding_ordered = without_hash(sorted, structured, sink_of(true, false), &rows_sort);
      }
    }
    for (const auto& c : structured.plans) {
      auto sorts = !ordered.empty() && !c.ordered();
      const auto& avoiding_structure = !ordered.empty() && !sorts ? avoiding_ordered : avoiding_unordered;
      auto sink = sink_of(true, sorts);
      const auto* in = sorts ? &sort : nullptr;
      const auto past = group_past(q, layout, keys, in);
      const auto known = past == 0 || c.access.max_entries <= 1;
      // The choice by the budget counts one table's range, which holds every
      // row the structure groups.
      auto countable = c.steps.empty() && scan_of(c.access) != nullptr && memory_ != 0;
      if (stream == nullptr || !countable || (known && c.access.max_entries < static_cast<double>(past))) {
        auto f = finish_prepared(q, c, sink, structured, nullptr, Needs{!ordered.empty() && !sorts, false, held},
                                 avoiding_structure,
                                 [&](physical::NodePtr rows) { return rooted(branch(std::move(rows), false, in)); });
        if (f) out.push_back(std::move(*f));
        continue;
      }
      if (known) continue;  // past the budget, where the stream avoids it
      auto cost = finished_cost(sink, measure(c));
      auto b = build(q, c, {}, nullptr);
      auto sb = build(q, *stream, {}, nullptr);
      merge_params(b, sb);
      auto rows = choice(c.access, static_cast<std::int64_t>(past), branch(std::move(b.rows), false, in),
                         branch(std::move(sb.rows), true, nullptr));
      out.push_back({rooted(std::move(rows)), std::move(b.null_checks), std::move(b.computed), cost + kCountCost});
    }
    return out;
  }

  // Aggregates of one group of every row answered from the keys of `c`'s
  // range (KeyAggregates), where each is COUNT(*), or MIN or MAX of one
  // column, the ASC part after the scan's prefix, and nothing filters a
  // row: COUNT(*) its count, MIN its first entry past the part's NULLs, MAX
  // its last, each held as an int64_t. nullopt where they are not. Its cost:
  // a count's, and a read of an entry, and of its row where the scan does not
  // cover, per MIN or MAX.
  auto from_keys(const Query& q, const Candidate& c, const std::vector<Expr>& outputs, bool emits) const
      -> std::optional<Finished> {
    const auto* scan = bare_scan(c);
    if (scan == nullptr || scan->reverse || outputs.empty()) return std::nullopt;
    std::optional<ColumnRef> arg;
    auto cost = 0.0;
    for (const auto& o : outputs) {
      if (o.kind != Expr::Kind::Aggregate || o.distinct) return std::nullopt;
      if (o.fn == Expr::Fn::Count && o.args.empty()) {
        cost = kCountCost;
        continue;
      }
      if ((o.fn != Expr::Fn::Min && o.fn != Expr::Fn::Max) || o.args[0].kind != Expr::Kind::Column) return std::nullopt;
      if ((arg && *arg != o.args[0].col) || o.args[0].col.type != PhysType::I64) return std::nullopt;
      arg = o.args[0].col;
    }
    auto values = *scan;
    if (arg) {
      if (!min_from_first_entry(q.rels[0].table(), values, *arg)) return std::nullopt;
      auto has = [&](Expr::Fn fn) { return std::ranges::any_of(outputs, [&](const Expr& o) { return o.fn == fn; }); };
      auto reads = (has(Expr::Fn::Min) ? 1.0 : 0.0) + (has(Expr::Fn::Max) ? 1.0 : 0.0);
      cost += reads * (values.covering || values.object == values.table ? 1.0 : 2.0);
    }
    auto b = build(q, c, {}, nullptr);
    return Finished{physical::node(physical::KeyAggregates{*scan, std::move(values), arg, outputs, emits}),
                    std::move(b.null_checks), std::move(b.computed), cost};
  }

  // MIN(arg) from the scan's first entry: arg is the ASC part after its
  // prefix, so the first entry past that part's NULLs holds the least value,
  // and the last the greatest. Where the scan starts at the NULLs
  // (physical::starts_at_nulls) and arg is nullable, it starts past them
  // instead.
  static auto min_from_first_entry(const Table& t, physical::Scan& sc, const ColumnRef& arg) -> bool {
    const auto all = paths(t);
    const auto& path = *std::ranges::find_if(all, [&](const Path& p) { return p.object == sc.object; });
    if (sc.prefix.size() >= path.parts.size()) return false;
    const auto& part = path.parts[sc.prefix.size()];
    if (part.col != arg.col || part.desc) return false;
    if (arg.nullable) sc.rest = physical::past_nulls(std::move(sc.rest));
    return true;
  }

  // The scan a candidate reads its one table by, where nothing filters a row
  // after its range: what the keys answer an aggregate from. Null otherwise.
  static auto bare_scan(const Candidate& c) -> const physical::Scan* {
    return c.steps.empty() && c.access.filters.empty() ? std::get_if<physical::Scan>(&c.access.path) : nullptr;
  }

  const Catalog& cat_;
  const RowCounts& counts_;
  std::uint64_t memory_;  // the statement's memory budget, 0 for none
  bool all_;
  std::map<std::uint32_t, double> rows_;
  std::vector<Catalog::PlanConstant> constants_;
  std::vector<physical::ResultColumn> result_;
};

}  // namespace

auto search(const logical::Node& root, const Catalog& cat, const RowCounts& counts, std::uint64_t memory_bytes)
    -> physical::Plan {
  return std::move(Search{cat, counts, memory_bytes, false}.plans(root).front());
}

auto search_all(const logical::Node& root, const Catalog& cat, const RowCounts& counts, std::uint64_t memory_bytes)
    -> std::vector<physical::Plan> {
  return Search{cat, counts, memory_bytes, true}.plans(root);
}

}  // namespace bytecask::sql
