// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// physical.cppm — the physical plan (docs/planner_design.md, *The physical
// plan and codegen*): a tree of operators the search (search.cppm) builds
// and codegen (codegen.cppm) prints, each with its produce and consume.
// Every id, type and constant name the printer needs is here, so it never
// reads the catalog.
//
// The operators are Empty, the access paths, the joins (an index nested
// loop, a nested loop and a hash join), the choice points, Filter, Sort,
// Limit, Aggregate, GroupAggregate and the sinks: Emit for result rows,
// GroupEmit for a GroupAggregate's, Insert, Update, Delete. check()
// enforces what the types can't express.

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.physical;

export import bytecask.sql.logical;

export namespace bytecask::sql::physical {

using logical::ColumnRef;
using logical::Expr;
using Op = ast::Compare::Op;

// One end of a scan, in key order, on the part after the prefix: open (the
// prefix's first or last entry), a value's key part (a parameter, or $n ± k),
// or the key part that holds NULL; as is, or its successor.
struct KeyBound {
  enum class Kind { Open, Value, NullPart };
  Kind kind{Kind::Open};
  std::optional<Expr> value;  // Value
  bool successor{false};

  friend auto operator==(const KeyBound&, const KeyBound&) -> bool = default;
};

struct Range {
  KeyBound lo, hi;
};

using ScanRest = std::variant<std::monostate, Range>;

// Whether a scan starts at its part's NULLs, ASC's first entries: its low
// end is open (a prefix alone, or no lower bound) or the NULL part as is.
[[nodiscard]] auto starts_at_nulls(const ScanRest& rest) -> bool {
  const auto* r = std::get_if<Range>(&rest);
  return r == nullptr || r->lo.kind == KeyBound::Kind::Open ||
         (r->lo.kind == KeyBound::Kind::NullPart && !r->lo.successor);
}

// `rest` started past its part's NULLs, at the NULL part's successor, where
// it starts at them; its high end kept. What MIN or MAX of a nullable part
// from the keys reads (KeyAggregates).
[[nodiscard]] auto past_nulls(ScanRest rest) -> ScanRest {
  if (!starts_at_nulls(rest)) return rest;
  auto r = std::holds_alternative<Range>(rest) ? std::get<Range>(rest) : Range{};
  r.lo = {KeyBound::Kind::NullPart, std::nullopt, true};
  return r;
}

// No rows, and nothing read: the input a conjunct always false leaves.
struct Empty {};

// One row of no columns, and nothing read: a SELECT without FROM's.
struct OneRow {};

// The whole primary key by equality, each part a parameter: at most one row.
struct PointGet {
  std::uint32_t table{0};
  std::vector<Expr> key;
  bool for_update{false};  // each row returned is guarded unchanged
};

// A scan of the table's primary key or of one of its indexes: every entry
// under an equality prefix of parameters, or a range of entries after it;
// forward, or in reverse. A full scan has no prefix and a range open at
// both ends.
struct Scan {
  std::uint32_t table{0};
  std::uint32_t object{0};                   // the table, or one of its indexes
  std::vector<Expr> prefix;                  // the equality prefix's parameters, in key order
  ScanRest rest;  // nothing more, or a range
  bool reverse{false};
  bool covering{false};    // every column read comes from the entry's key
  bool for_update{false};  // each row returned is guarded unchanged: never a covering scan
};

// An ordered scan whose first ordered part is a nullable ASC part: the key
// order puts its NULLs first, PostgreSQL last for ASC and first for DESC.
// So two ranges: `present`, the part's values (a Range from past the NULL
// part), and the NULL part. Forward: the values, then the NULLs; with
// present.reverse (DESC), the NULLs reversed, then the values reversed.
struct NullSplitScan {
  Scan present;
};

// COUNT answered from the keys: bc_count_range over the scan's range, which
// holds one entry per row counted. An access path that also produces the
// aggregate's row.
struct CountRange {
  Scan scan;
  bool emits{true};  // false under LIMIT 0
};

// Aggregates of one group of every row answered from a range's keys
// alone, `outputs` in the select list's order: COUNT(*) the count of
// `counted`'s range; MIN the first entry of `values`, `arg` being the ASC
// part after its prefix and its range starting past that part's NULLs; MAX
// its last, the first of the same range scanned in reverse. `values` is
// `counted` but for that start, and `arg`, held as an int64_t, is set where
// a MIN or MAX reads it.
struct KeyAggregates {
  Scan counted;
  Scan values;
  std::optional<ColumnRef> arg;
  std::vector<Expr> outputs;
  bool emits{true};  // false under LIMIT 0
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

// An index nested loop: for each outer row, the inner table's row named by
// its whole primary key, each part a parameter or a column of the outer row.
// An outer row whose key column is NULL, or with no inner row, joins nothing.
// Left outer: it joins the inner row where there is one and every conjunct
// of `match` holds of the pair, and else a row of the inner table's NULLs
// (bc_null_row); a NULL key part, parameter or column, matches no row.
struct IndexJoin {
  NodePtr outer;
  std::uint32_t inner{0};
  std::vector<Expr> inner_key;  // in the inner primary key's order
  bool left_outer{false};
  std::vector<Expr> match;  // left outer: the conjuncts that make a pair; empty otherwise
};

// A nested loop whose inner side reads parameters alone: for each outer row,
// every row of the inner scan, under the filters on its own table and the
// join's, which compare its columns with the outer side's. Under an outer
// side of at most one row (a point lookup and lookups by key) the inner
// side is read at most once; under any other, once per outer row.
// Left outer: an outer row the inner side gives no row for, its filters
// the join's ON, is joined once to the inner table's NULLs (bc_null_row);
// where a parameter of `guard` is NULL the inner side gives none, as an
// inner join's plan checks before it reads anything.
struct NestedLoop {
  NodePtr outer;
  NodePtr inner;
  bool left_outer{false};
  std::vector<std::uint32_t> guard;  // left outer: the inner side's compared parameters
};

// One part of a hash join's key: a column of the build side's table and the
// probe side's column it equals, of one physical type, never a DOUBLE.
struct HashKey {
  ColumnRef build;
  ColumnRef probe;
};

// A hash join, a pipeline breaker: every row of `build` is put in a hash
// table (bc_hash_*) under the key of its `keys` columns, then for each row of
// `probe`, every row put under the same key. The key is built with
// bc_sort_key_* from the columns' values, so equal values give equal bytes;
// a row with a NULL key column is neither put nor probed, as `=` never holds
// for NULL. The build side reads one table in one loop, `held`, whose row the
// hash table holds; the probe side is any plan, and its order is the join's.
struct HashJoin {
  NodePtr build;
  NodePtr probe;
  std::vector<HashKey> keys;
  std::uint32_t held{0};
};

// A choice point (docs/planner_design.md, *Choosing at run time*): one
// table's rows by one of two plans, picked when the plan runs by
// bc_count_range over the first one's range, limited to the threshold. A
// count below it takes the first, `below`; one that reaches it, the second.
// Each is a scan under its filters, or such a scan sorted: the choice by the
// memory budget between a sort and a scan in the ORDER BY's order. Or, for
// the choice by the budget between a hash join and a plan without one, the
// hash join, counted by its build side's range, and the other plan of the
// same tables. A threshold of 0 always takes the second, and INT64_MAX the
// first (override_thresholds).
struct Choice {
  Scan counted;  // the range counted: the first plan's, its whole prefix for a NULL split
  std::int64_t threshold{0};
  NodePtr below;
  NodePtr at_or_above;
};

// The rows every conjunct holds for, run as soon as the row is read. NULL
// on either side of a comparison rejects the row. Never on a DOUBLE or REAL
// column.
struct Filter {
  NodePtr input;
  std::vector<Expr> conjuncts;
};

// One part of a sort key: a column of a row in hand, in its direction, its
// NULLs first or last.
struct SortPart {
  ColumnRef col;
  bool desc{false};
  bool nulls_first{false};
};

// A pipeline breaker: every row of its input added to sorters, keyed by
// `keys` (bc_sort_key_*), then streamed in that order. A sorter holds one
// view per row, so there is one per loop of the input whose row is read
// above the Sort, each holding the row of its loop's table, `payloads`,
// outer first, all fed the same key; with none, one holds the keys alone. A
// sorter is stable, and a top-n one keeps the earliest of equal keys, so the
// sorters hold rows of the same input rows in the same order, and are read
// in step. The rows of tables read once before the first loop stay in hand,
// and a table looked up per row after it is looked up again above the Sort.
// Another loop's row is read only below it, its sort key built from it or
// from a covering scan's entry.
// With keep n > 0 each sorter holds only the first n (ORDER BY … LIMIT n); 0
// holds all. A row NULL in a column of `not_null` is not added: a hash
// join's probe key, which matches nothing. With `groups`, its input is a
// GroupAggregate's groups, under its HAVING: the sorter holds each group's
// key, its keys are GROUP BY columns, and `payloads` and `not_null` are
// empty.
struct Sort {
  NodePtr input;
  std::vector<SortPart> keys;
  std::int64_t keep{0};
  std::vector<std::uint32_t> payloads;
  std::vector<ColumnRef> not_null;
  bool groups{false};
};

// The first n rows of Emit's input.
struct Limit {
  NodePtr input;
  std::int64_t n{0};
};

// A result column that is a parameter, the same in every row: a constant
// the binder made one.
struct ParamTarget {
  std::uint32_t param{0};
  PhysType type{PhysType::I64};
};
// A result column that is an expression over the row's columns, parameters
// and constants, evaluated for each row (docs/planner_design.md,
// *Expressions over columns*). Never a bare column or parameter, which have
// targets of their own.
struct ExprTarget {
  Expr expr;
};
// A result column: a column, from the row or from the entry's key when the
// scan covers, a parameter, or an expression.
using EmitTarget = std::variant<ColumnRef, ParamTarget, ExprTarget>;

// Result rows: the targets of each row. No target is a row of no columns, as
// PostgreSQL's `SELECT FROM`.
struct Emit {
  NodePtr input;
  std::vector<EmitTarget> targets;
};

// One aggregate row, of COUNT(*), or COUNT, COUNT(DISTINCT) or SUM of its
// argument's non-NULL values, each held as an int64_t. Any other aggregate
// is a GroupAggregate's.
struct Aggregate {
  NodePtr input;
  Expr agg;
  bool emits{true};  // false under LIMIT 0
};

// A grouped aggregate (docs/planner_design.md, *Aggregates*): the
// input's rows in groups of equal `keys`, NULL equal to NULL, and over each
// group the aggregate calls `aggs`, each once; its rows are the groups. A
// pipeline breaker over bc_agg_*: each row's group found by its key, a sort
// key of its key columns, its values folded into the group's slots, then a
// loop over the groups in the order they first appeared. `streamed`: the
// input's rows come in groups, each group's rows together, so each group is
// kept in locals and given when the next one starts, with no structure.
// With no keys it is one group, given even over no rows, and is streamed. A
// COUNT(DISTINCT) holds each group's distinct values either way; streamed,
// only the group in hand's. `dependent` are columns the keys determine,
// equal in every row of a group: each group gives its first row's, and
// they never key it.
struct GroupAggregate {
  NodePtr input;
  std::vector<ColumnRef> keys;
  std::vector<Expr> aggs;
  bool streamed{false};
  std::vector<ColumnRef> dependent;

  // What a group gives of its rows' columns: its keys, then its dependent
  // columns.
  [[nodiscard]] auto columns() const -> std::vector<ColumnRef> {
    auto out = keys;
    out.insert(out.end(), dependent.begin(), dependent.end());
    return out;
  }
};

// Where a GroupAggregate keeps what it gives for a group, by bc_agg_* slots
// (group_layout, of its columns()): slot 0 counts its rows; a BIGINT-like
// column (an I64), a key or dependent, takes a slot holding its value,
// folded once; any other is read from a row of its table, the group's
// first, held in a hash table under the group's key, one per such table
// (`held`); each aggregate takes
// a slot, AVG two (its sum, then its count), COUNT(*) slot 0. A
// COUNT(DISTINCT) also keeps a structure of its own, one per argument.
struct GroupLayout {
  std::uint32_t slots{1};
  std::vector<std::optional<std::uint32_t>> key_slot;  // per column: its slot, or nullopt for one read from a held row
  std::vector<std::uint32_t> agg_slot;                 // per aggregate
  std::vector<std::uint32_t> held;                     // the tables whose first row each group holds
  std::vector<ColumnRef> distinct;                     // each COUNT(DISTINCT)'s argument, once
};

[[nodiscard]] auto group_layout(const std::vector<ColumnRef>& columns, const std::vector<Expr>& aggs) -> GroupLayout {
  GroupLayout l;
  for (const auto& k : columns) {
    if (k.type == PhysType::I64) {
      l.key_slot.emplace_back(l.slots++);
      continue;
    }
    l.key_slot.emplace_back(std::nullopt);
    if (std::ranges::find(l.held, k.table) == l.held.end()) l.held.push_back(k.table);
  }
  for (const auto& a : aggs) {
    if (a.fn == Expr::Fn::Count && a.args.empty()) {
      l.agg_slot.push_back(0);
      continue;
    }
    l.agg_slot.push_back(l.slots);
    l.slots += a.fn == Expr::Fn::Avg ? 2 : 1;
    if (a.distinct && std::ranges::find(l.distinct, a.args[0].col) == l.distinct.end()) l.distinct.push_back(a.args[0].col);
  }
  return l;
}

// Result rows of a GroupAggregate's groups: each output a key column of the
// GroupAggregate below, or one of its aggregates. Its input is the
// GroupAggregate under a Filter (HAVING), a Sort of its groups and a
// Limit, or a choice point between two such plans.
struct GroupEmit {
  NodePtr input;
  std::vector<Expr> outputs;
};

// The columns given, in table order, each from a parameter; the rest keep
// the default bc_row_begin writes.
struct Insert {
  std::uint32_t table{0};
  std::vector<std::pair<ColumnRef, Expr>> values;
};

// UPDATE and DELETE write each row their input gives, read from the
// snapshot the statement began on.
struct Update {
  NodePtr input;
  std::uint32_t table{0};
  std::vector<std::pair<ColumnRef, Expr>> set;
};

struct Delete {
  NodePtr input;
  std::uint32_t table{0};
};

struct Node {
  std::variant<Empty, OneRow, PointGet, Scan, NullSplitScan, CountRange, KeyAggregates, IndexJoin, NestedLoop, HashJoin, Choice,
               Filter, Sort, Limit, Emit, Aggregate, GroupAggregate, GroupEmit, Insert, Update, Delete>
      op;
};

template <typename T>
[[nodiscard]] auto node(T op) -> NodePtr {
  return std::make_unique<Node>(Node{std::move(op)});
}

// The GroupAggregate whose groups a node gives, under its HAVING and a sort
// of its groups, or null.
[[nodiscard]] inline auto group_below(const Node& n) -> const GroupAggregate* {
  if (const auto* g = std::get_if<GroupAggregate>(&n.op)) return g;
  if (const auto* f = std::get_if<Filter>(&n.op)) return group_below(*f->input);
  if (const auto* o = std::get_if<Sort>(&n.op); o != nullptr && o->groups) return group_below(*o->input);
  return nullptr;
}

// $param + offset as an INTEGER, computed once before anything is read:
// BC_C_RANGE on `table` when it leaves INTEGER, as PostgreSQL's operator
// raises 22003.
struct Computed {
  std::uint32_t param{0};
  std::int64_t offset{0};
  std::uint32_t table{0};

  friend auto operator==(const Computed&, const Computed&) -> bool = default;
};

// A result column as PostgreSQL names and types it.
struct ResultColumn {
  std::string name;
  SqlType sql;
};

// The plan's columns a result column of type `t` is: a NUMERIC, an AVG, two
// I64, its sum and its count, which the API divides as PostgreSQL's numeric
// division does; any other one column of its physical type.
[[nodiscard]] auto plan_columns(SqlType t) -> std::vector<PhysType> {
  if (t.kind == SqlKind::Numeric) return {PhysType::I64, PhysType::I64};
  return {phys_type(t)};
}

struct Plan;

// A set operation's plan (docs/planner_design.md, *Set operations*): a tree
// whose leaves are SELECTs searched alone, each a plan whose root is an Emit.
// `types` are the columns of the rows the node gives, which a leaf's rows are
// converted to.
struct SetPlan {
  logical::SetOp::Kind kind{logical::SetOp::Kind::Union};
  bool all{false};
  std::shared_ptr<const SetPlan> left, right;  // both, or neither for a leaf
  std::shared_ptr<const Plan> select;          // a leaf's
  std::vector<SqlType> types;
};

// An uncorrelated subquery's plan, run before the statement's own: its rows
// folded as `kind` says into the parameters from `param` on, and for a Set
// into the set InSet tests by its index here (logical::Subquery).
struct Sub {
  std::shared_ptr<const Plan> plan;
  logical::Subquery::Kind kind{logical::Subquery::Kind::Value};
  SqlType type{};
  std::uint32_t param{0};
};

struct Plan {
  NodePtr root;  // null for a set operation
  // A set operation's rows, its arms' plans streamed into it, under the
  // statement's LIMIT. The arms hold no subqueries of their own: this plan's
  // `subs` are the statement's.
  std::shared_ptr<const SetPlan> set;
  std::optional<std::int64_t> set_limit;
  // ORDER BY of the combined rows, by the columns of the root's `types`.
  std::vector<logical::SetOp::Order> set_order;
  std::vector<Sub> subs;  // run in order, before anything else
  std::vector<Catalog::PlanConstant> constants;  // its tables', in the catalog's order
  std::uint64_t catalog_version{0};
  std::vector<PhysType> param_types;
  // The parameters a NULL of which matches no row: the plan ends before it
  // reads anything, with what the sink gives for no rows.
  std::vector<std::uint32_t> null_checks;
  std::vector<Computed> computed;    // what the statement's $n ± k evaluate to
  std::vector<ResultColumn> result;  // empty for a write
  // What the cost model predicts the plan charges the work budget: record
  // reads plus cursor steps (docs/planner_design.md, *The cost model*).
  double cost{0};
};

// $n ± k, as the Computed it evaluates to, compared with a column of `table`.
[[nodiscard]] auto computed_of(const Expr& e, std::uint32_t table) -> Computed {
  auto k = e.args[1].constant;
  return {e.args[0].param, e.kind == Expr::Kind::Add ? k : -k, table};
}

// Checks what the types can't express. Throws std::logic_error naming the
// broken rule: a compiler bug, never a refusal of the statement.
void check(const Plan& p);

// The number of choice points in the plan.
[[nodiscard]] auto choices(const Plan& p) -> std::size_t;

// Sets each choice point's threshold, outer first, to the one at its place
// in `t`: the test seam that runs any combination of branches (*Tests*,
// alternatives agree). 0 takes a choice point's second plan, INT64_MAX its
// first. `t` holds one per choice point.
void override_thresholds(Plan& p, const std::vector<std::int64_t>& t);

}  // namespace bytecask::sql::physical

namespace bytecask::sql::physical {

namespace {

[[noreturn]] void broken(const std::string& rule) { throw std::logic_error{"physical plan: " + rule}; }

// A SELECT without FROM's plan, Emit over OneRow under its LIMIT: the one
// plan that names no table, and so has no constants.
auto of_no_table(const Node& root) -> bool {
  const auto* e = std::get_if<Emit>(&root.op);
  if (e == nullptr) return false;
  const auto* n = e->input.get();
  if (const auto* l = std::get_if<Limit>(&n->op)) n = l->input.get();
  return std::holds_alternative<OneRow>(n->op);
}

class Checker {
public:
  explicit Checker(const Plan& p) : p_{p} {}

  void plan() {
    if (p_.set) {
      if (p_.root || !p_.null_checks.empty() || !p_.computed.empty()) broken("a set operation with a pipeline of its own");
      if (p_.set_limit && *p_.set_limit < 0) broken("a negative LIMIT");
      for (const auto& o : p_.set_order) {
        if (o.column >= p_.set->types.size()) broken("a set operation ordered by a column it does not have");
      }
      subqueries();
      arms(*p_.set);
      return;
    }
    if (p_.constants.empty() && !of_no_table(*p_.root)) broken("a plan with no constants");
    if (!p_.root) broken("a plan with no operator");
    for (auto n : p_.null_checks) param(n);
    subqueries();
    // Each combination of the choice points' branches is checked as the plan
    // it runs: what one branch's covering scan reads from a key says nothing
    // of the other's.
    std::vector<const Choice*> all;
    each_choice(*p_.root, all);
    for (std::uint64_t pick = 0; pick < (std::uint64_t{1} << all.size()); ++pick) {
      second_.clear();
      for (std::size_t i = 0; i < all.size(); ++i) {
        if ((pick >> i & 1) != 0) second_.push_back(all[i]);
      }
      covering_.clear();
      covering_tables(*p_.root);
      node(*p_.root, nullptr);
    }
  }

private:
  void param(std::uint32_t n) const {
    if (n >= p_.param_types.size()) broken("a parameter past the statement's");
  }

  void subqueries() const {
    for (const auto& s : p_.subs) {
      if (!s.plan || !s.plan->subs.empty()) broken("a subquery with no plan, or one of its own");
      using K = logical::Subquery::Kind;
      auto n = s.kind == K::Bounds ? 4u : s.kind == K::Set ? 2u : 1u;
      if (std::uint64_t{s.param} + n > p_.param_types.size()) broken("a subquery's parameters past the statement's");
      check(*s.plan);
    }
  }

  // Each leaf is a SELECT whose rows are the node's columns, as many as its
  // Emit has; the operation's two sides give as many as it does.
  void arms(const SetPlan& n) const {
    if (n.types.empty()) broken("a set operation of no columns");
    if (!n.select) {
      if (!n.left || !n.right || n.left->types.size() != n.types.size() || n.right->types.size() != n.types.size()) {
        broken("a set operation whose sides differ in their columns");
      }
      arms(*n.left);
      arms(*n.right);
      return;
    }
    if (n.left || n.right) broken("a set operation leaf with sides");
    const auto& leaf = *n.select;
    if (!leaf.root || !std::holds_alternative<Emit>(leaf.root->op) || !leaf.subs.empty() || leaf.set) {
      broken("a set operation arm that is not a SELECT of rows with no subquery of its own");
    }
    if (std::get<Emit>(leaf.root->op).targets.size() != n.types.size()) broken("a set operation arm of other columns");
    check(leaf);
  }

  void params(const Expr& e) const {
    if (e.kind == Expr::Kind::Param) param(e.param);
    for (const auto& a : e.args) params(a);
  }

  // A NULL parameter matches nothing: the plan must end before it encodes
  // one into a key.
  void null_checked(const Expr& e) const {
    if (e.kind != Expr::Kind::Param) return;
    param(e.param);
    if (std::ranges::contains(guarded_, e.param)) return;  // an outer join's inner side guards it
    if (std::ranges::find(p_.null_checks, e.param) == p_.null_checks.end()) {
      broken("a compared parameter the plan does not check for NULL");
    }
  }

  // $n ± k is computed before anything is read.
  void operand(const Expr& e, std::uint32_t table) const {
    params(e);
    if (e.kind != Expr::Kind::Add && e.kind != Expr::Kind::Sub) return;
    if (std::ranges::find(p_.computed, computed_of(e, table)) == p_.computed.end()) broken("a $n ± k the plan does not compute");
  }

  // A column codegen reads, from the row or, for a table a covering scan
  // reads, from the entry's key: never a DOUBLE from a key, whose encoding
  // writes -0.0 as 0.0 and every NaN as one NaN.
  void read(const ColumnRef& c) const {
    if (c.type == PhysType::F64 && std::ranges::find(covering_, c.table) != covering_.end()) {
      broken("a DOUBLE column read from a key");
    }
  }
  void reads(const Expr& e) const {
    if (e.kind == Expr::Kind::Column) read(e.col);
    for (const auto& a : e.args) reads(a);
  }

  // A result column's expression: what codegen's eval prints, each operator
  // with its operands.
  void value(const Expr& e) const {
    auto operands = [&](std::size_t at_least, std::size_t at_most) {
      if (e.args.size() < at_least || e.args.size() > at_most) broken("an operator of the wrong number of operands");
    };
    switch (e.kind) {
      case Expr::Kind::Column:
      case Expr::Kind::Param:
      case Expr::Kind::Const: operands(0, 0); break;
      case Expr::Kind::Add:
      case Expr::Kind::Sub:
      case Expr::Kind::Mul:
      case Expr::Kind::Div:
      case Expr::Kind::Compare: operands(2, 2); break;
      case Expr::Kind::IsNull:
      case Expr::Kind::Not: operands(1, 1); break;
      case Expr::Kind::And:
      case Expr::Kind::Or: operands(2, e.args.size()); break;
      case Expr::Kind::Aggregate:
      case Expr::Kind::Like:
      case Expr::Kind::Truth:
      case Expr::Kind::InSet: broken("a result column of an expression codegen does not print");
    }
    for (const auto& a : e.args) value(a);
  }

  // A predicate filter: AND, OR and NOT of comparisons of a column with a
  // parameter or a constant, none of a DOUBLE, and IS NULL of a column.
  void predicate(const Expr& e) const {
    switch (e.kind) {
      case Expr::Kind::And:
      case Expr::Kind::Or:
        if (e.args.size() < 2) broken("an AND or OR of fewer than two");
        for (const auto& a : e.args) predicate(a);
        return;
      case Expr::Kind::Not:
        if (e.args.size() != 1) broken("a NOT of other than one");
        predicate(e.args[0]);
        return;
      case Expr::Kind::IsNull:
        if (e.args.size() != 1 || e.args[0].kind != Expr::Kind::Column) broken("IS NULL of other than a column");
        reads(e);
        return;
      case Expr::Kind::Like:
        if (e.args.size() != 2 || e.args[0].kind != Expr::Kind::Column || e.args[0].col.type != PhysType::Bytes ||
            e.args[1].kind != Expr::Kind::Param) {
          broken("LIKE of other than a text column and a parameter");
        }
        params(e.args[1]);
        reads(e);
        return;
      case Expr::Kind::Compare:
        if (e.args[0].kind != Expr::Kind::Column || e.args[0].col.type == PhysType::F64 ||
            (e.args[1].kind != Expr::Kind::Param && e.args[1].kind != Expr::Kind::Const)) {
          broken("a predicate's comparison other than of a column, not a DOUBLE, with a parameter");
        }
        params(e.args[1]);
        reads(e);
        return;
      case Expr::Kind::Const: return;  // a comparison folded
      case Expr::Kind::Truth:
        if (e.args.size() != 1 || e.args[0].kind != Expr::Kind::Param || e.args[0].phys() != PhysType::Bool) {
          broken("a truth of other than a BOOLEAN parameter");
        }
        params(e);
        return;
      case Expr::Kind::InSet: {
        // Of a column of its subquery's set's type.
        auto i = static_cast<std::size_t>(e.constant);
        if (e.args.size() != 1 || e.args[0].kind != Expr::Kind::Column || e.constant < 0 || i >= p_.subs.size() ||
            p_.subs[i].kind != logical::Subquery::Kind::Set || phys_type(p_.subs[i].type) != e.args[0].col.type) {
          broken("IN of a subquery of other than a column of its set's type");
        }
        reads(e);
        return;
      }
      case Expr::Kind::Column:
      case Expr::Kind::Param:
      case Expr::Kind::Add:
      case Expr::Kind::Sub:
      case Expr::Kind::Mul:
      case Expr::Kind::Div:
      case Expr::Kind::Aggregate: break;
    }
    broken("a predicate of an expression codegen does not print");
  }

  void scan(const Scan& s) const {
    if (s.prefix.empty() && std::holds_alternative<std::monostate>(s.rest)) {
      broken("a scan needs an equality prefix or a range");
    }
    for (const auto& e : s.prefix) {
      if (e.kind != Expr::Kind::Param) broken("a scan prefix part that is not a parameter");
      null_checked(e);
    }
    // FOR UPDATE guards each row by its key, read from the row.
    if (s.covering && s.for_update) broken("a FOR UPDATE scan that covers");
    if (const auto* r = std::get_if<Range>(&s.rest)) {
      for (const auto* b : {&r->lo, &r->hi}) {
        if ((b->kind == KeyBound::Kind::Value) != b->value.has_value()) broken("a key bound's value");
        if (!b->value) continue;
        operand(*b->value, s.table);
        null_checked(*b->value);
      }
    }
  }

  // The scan an operator's rows come from, or null; a GroupAggregate's,
  // its input's.
  static auto scan_below(const Node& n) -> const Scan* {
    if (const auto* s = std::get_if<Scan>(&n.op)) return s;
    if (const auto* s = std::get_if<NullSplitScan>(&n.op)) return &s->present;
    if (const auto* f = std::get_if<Filter>(&n.op)) return scan_below(*f->input);
    if (const auto* l = std::get_if<Limit>(&n.op)) return scan_below(*l->input);
    if (const auto* o = std::get_if<Sort>(&n.op)) return scan_below(*o->input);
    if (const auto* g = std::get_if<GroupAggregate>(&n.op)) return scan_below(*g->input);
    return nullptr;
  }

  static auto split_below(const Node& n) -> bool {
    if (std::holds_alternative<NullSplitScan>(n.op)) return true;
    if (const auto* f = std::get_if<Filter>(&n.op)) return split_below(*f->input);
    if (const auto* o = std::get_if<Sort>(&n.op)) return split_below(*o->input);
    return false;
  }

  // The hash join a node's rows come from, under the filters on its pairs
  // and a sort of them, or null.
  static auto hash_below(const Node& n) -> const HashJoin* {
    if (const auto* h = std::get_if<HashJoin>(&n.op)) return h;
    if (const auto* f = std::get_if<Filter>(&n.op)) return hash_below(*f->input);
    if (const auto* o = std::get_if<Sort>(&n.op)) return hash_below(*o->input);
    return nullptr;
  }

  // The plans a node's rows may come from: a choice point's two, or itself.
  static auto branches(const Node& n) -> std::vector<const Node*> {
    if (const auto* c = std::get_if<Choice>(&n.op)) return {c->below.get(), c->at_or_above.get()};
    return {&n};
  }

  // The tables of a side of at most one row, read once: a point lookup, then
  // lookups by key. nullopt for any other side.
  static auto one_row_tables(const Node& n) -> std::optional<std::vector<std::uint32_t>> {
    if (const auto* g = std::get_if<PointGet>(&n.op)) return std::vector<std::uint32_t>{g->table};
    if (const auto* j = std::get_if<IndexJoin>(&n.op)) {
      auto out = one_row_tables(*j->outer);
      if (out) out->push_back(j->inner);
      return out;
    }
    if (const auto* f = std::get_if<Filter>(&n.op)) return one_row_tables(*f->input);
    return std::nullopt;
  }

  // Each table whose rows a node gives.
  static void tables_of(const Node& n, std::vector<std::uint32_t>& out) {
    if (const auto* g = std::get_if<PointGet>(&n.op)) out.push_back(g->table);
    if (const auto* s = std::get_if<Scan>(&n.op)) out.push_back(s->table);
    if (const auto* s = std::get_if<NullSplitScan>(&n.op)) out.push_back(s->present.table);
    if (const auto* f = std::get_if<Filter>(&n.op)) tables_of(*f->input, out);
    if (const auto* l = std::get_if<Limit>(&n.op)) tables_of(*l->input, out);
    if (const auto* o = std::get_if<Sort>(&n.op)) tables_of(*o->input, out);
    if (const auto* c = std::get_if<Choice>(&n.op)) tables_of(*c->below, out);
    if (const auto* j = std::get_if<IndexJoin>(&n.op)) {
      tables_of(*j->outer, out);
      out.push_back(j->inner);
    }
    if (const auto* j = std::get_if<NestedLoop>(&n.op)) {
      tables_of(*j->outer, out);
      tables_of(*j->inner, out);
    }
    if (const auto* h = std::get_if<HashJoin>(&n.op)) {
      tables_of(*h->probe, out);
      tables_of(*h->build, out);
    }
    if (const auto* g = std::get_if<GroupAggregate>(&n.op)) tables_of(*g->input, out);
  }
  static auto tables_of(const Node& n) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> out;
    tables_of(n, out);
    return out;
  }

  // The tables whose rows the loops over a cursor or a sorter that a node's
  // rows come from bind, one loop inside another, outer first; a sort's loop
  // binds each of its sorters' rows.
  static auto loops(const Node& n) -> std::vector<std::uint32_t> {
    if (const auto* s = scan_below(n); s != nullptr && !std::holds_alternative<Sort>(n.op)) return {s->table};
    if (const auto* o = std::get_if<Sort>(&n.op)) return o->payloads;
    if (const auto* f = std::get_if<Filter>(&n.op)) return loops(*f->input);
    if (const auto* l = std::get_if<Limit>(&n.op)) return loops(*l->input);
    if (const auto* c = std::get_if<Choice>(&n.op)) return loops(*c->below);
    if (const auto* j = std::get_if<IndexJoin>(&n.op)) return loops(*j->outer);
    if (const auto* j = std::get_if<NestedLoop>(&n.op)) {
      auto out = loops(*j->outer);
      auto inner = loops(*j->inner);
      out.insert(out.end(), inner.begin(), inner.end());
      return out;
    }
    // The probe side's loops, and inside them the loop over a probe's rows.
    if (const auto* h = std::get_if<HashJoin>(&n.op)) {
      auto out = loops(*h->probe);
      out.push_back(h->held);
      return out;
    }
    return {};
  }

  // Every choice point under n, outer first.
  static void each_choice(const Node& n, std::vector<const Choice*>& out) {
    std::visit(
        [&](const auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, Choice>) {
            out.push_back(&op);
            each_choice(*op.below, out);
            each_choice(*op.at_or_above, out);
          } else if constexpr (std::is_same_v<T, IndexJoin>) {
            each_choice(*op.outer, out);
          } else if constexpr (std::is_same_v<T, NestedLoop>) {
            each_choice(*op.outer, out);
            each_choice(*op.inner, out);
          } else if constexpr (std::is_same_v<T, HashJoin>) {
            each_choice(*op.build, out);
            each_choice(*op.probe, out);
          } else if constexpr (requires { op.input; }) {
            each_choice(*op.input, out);
          }
        },
        n.op);
  }

  // The branch of a choice point the combination being checked takes.
  auto taken(const Choice& c) const -> const Node& {
    return std::ranges::find(second_, &c) == second_.end() ? *c.below : *c.at_or_above;
  }

  // The tables whose rows come from a covering scan's key, not a row, in the
  // branches taken.
  void covering_tables(const Node& n) {
    if (const auto* s = scan_below(n); s != nullptr && s->covering) covering_.push_back(s->table);
    std::visit(
        [&](const auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, KeyAggregates>) {
            if (op.arg && op.values.covering) covering_.push_back(op.values.table);
          } else if constexpr (std::is_same_v<T, IndexJoin>) {
            covering_tables(*op.outer);
          } else if constexpr (std::is_same_v<T, NestedLoop>) {
            covering_tables(*op.outer);
            covering_tables(*op.inner);
          } else if constexpr (std::is_same_v<T, HashJoin>) {
            covering_tables(*op.build);
            covering_tables(*op.probe);
          } else if constexpr (std::is_same_v<T, Choice>) {
            covering_tables(taken(op));
          } else if constexpr (requires { op.input; }) {
            covering_tables(*op.input);
          }
        },
        n.op);
  }

  // The sort a node's rows come from, through the lookups by key repeated
  // above it, in the branches taken; or null.
  auto sort_below(const Node& n) const -> const Sort* {
    if (const auto* o = std::get_if<Sort>(&n.op)) return o;
    if (const auto* f = std::get_if<Filter>(&n.op)) return sort_below(*f->input);
    if (const auto* l = std::get_if<Limit>(&n.op)) return sort_below(*l->input);
    if (const auto* j = std::get_if<IndexJoin>(&n.op)) return sort_below(*j->outer);
    if (const auto* c = std::get_if<Choice>(&n.op)) return sort_below(taken(*c));
    return nullptr;
  }

  // A column read above the sort `n`'s rows come from is of a row in hand:
  // never of a loop no sorter holds, whose row the sorter loop has not.
  void held_above(const Node& n, const ColumnRef& c) const {
    const auto* o = sort_below(n);
    if (o == nullptr || std::ranges::find(o->payloads, c.table) != o->payloads.end()) return;
    auto in = loops(*o->input);
    if (std::ranges::find(in, c.table) != in.end()) broken("a column read above a sort from a loop no sorter holds");
  }

  // The inner side of a nested loop, one of its plans: a scan under filters
  // that each compare a column of the scan's table, read from its row, with
  // a parameter or a column of the outer side.
  // `left_outer`: the join's ON may also compare the outer side's columns
  // alone, which decides the pairs.
  static void inner_side(const Node& inner, const std::vector<std::uint32_t>& outer, bool left_outer) {
    const auto* sc = scan_below(inner);
    if (sc == nullptr || std::holds_alternative<Sort>(inner.op)) broken("a nested loop whose inner side is not a scan");
    auto of_outer = [&](const Expr& e) { return e.kind == Expr::Kind::Column && std::ranges::contains(outer, e.col.table); };
    for (const auto* in = &inner; const auto* f = std::get_if<Filter>(&in->op); in = f->input.get()) {
      for (const auto& c : f->conjuncts) {
        if (c.kind != Expr::Kind::Compare) continue;
        if (left_outer && of_outer(c.args[0]) && (c.args[1].kind != Expr::Kind::Column || of_outer(c.args[1]))) continue;
        if (c.args[0].kind != Expr::Kind::Column || c.args[0].col.table != sc->table) {
          broken("a nested loop whose inner filter compares a column outside its inner side");
        }
        if (sc->covering) broken("a nested loop whose inner filter reads the row of a covering scan");
        if (c.args[1].kind == Expr::Kind::Column && std::ranges::find(outer, c.args[1].col.table) == outer.end()) {
          broken("a nested loop whose inner filter reads a column outside its outer side");
        }
      }
    }
  }

  // A choice point counts exactly its first plan's range: its object, its
  // prefix's parameters, its bounds and its direction; for a NULL split,
  // the whole prefix, which holds both of its ranges, or with no prefix the
  // whole object.
  static void counts_first(const Choice& c, const Scan& first, bool split) {
    auto same_range = [&] {
      // A NULL split's whole prefix; of none, the whole object.
      if (split && !first.prefix.empty()) return std::holds_alternative<std::monostate>(c.counted.rest);
      if (split) {
        const auto* r = std::get_if<Range>(&c.counted.rest);
        return r != nullptr && r->lo == KeyBound{} && r->hi == KeyBound{};
      }
      const auto* a = std::get_if<Range>(&c.counted.rest);
      const auto* b = std::get_if<Range>(&first.rest);
      if (a == nullptr || b == nullptr) return a == b;
      return a->lo == b->lo && a->hi == b->hi;
    };
    if (c.counted.object != first.object || c.counted.prefix != first.prefix || c.counted.reverse != first.reverse ||
        !same_range()) {
      broken("a choice point that counts another range than its first plan's");
    }
  }

  void node(const Node& n, const Node* parent) const {
    std::visit(
        [&](const auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, Empty>) {
            // Reads nothing.
          } else if constexpr (std::is_same_v<T, PointGet>) {
            if (op.key.empty()) broken("a point lookup with no key part");
            for (const auto& e : op.key) null_checked(e);
          } else if constexpr (std::is_same_v<T, Scan>) {
            scan(op);
          } else if constexpr (std::is_same_v<T, NullSplitScan>) {
            scan(op.present);
            const auto* r = std::get_if<Range>(&op.present.rest);
            if (r == nullptr || r->lo.kind != KeyBound::Kind::NullPart || !r->lo.successor) {
              broken("a NULL split whose values do not start past the NULL part");
            }
          } else if constexpr (std::is_same_v<T, CountRange>) {
            scan(op.scan);
          } else if constexpr (std::is_same_v<T, KeyAggregates>) {
            // Each output COUNT(*), or MIN or MAX of the one part `arg`.
            auto counts = false;
            for (const auto& o : op.outputs) {
              if (o.kind != Expr::Kind::Aggregate || o.distinct) broken("an aggregate from the keys that is not a plain call");
              if (o.fn == Expr::Fn::Count && o.args.empty()) {
                counts = true;
              } else if ((o.fn != Expr::Fn::Min && o.fn != Expr::Fn::Max) || o.args.size() != 1 || !op.arg ||
                         o.args[0].kind != Expr::Kind::Column || o.args[0].col != *op.arg) {
                broken("an aggregate from the keys other than COUNT(*), or MIN or MAX of its part");
              }
            }
            if (op.outputs.empty()) broken("aggregates from the keys with no output");
            if (counts) scan(op.counted);
            if (op.arg) {
              scan(op.values);
              // Each is read into an int64_t.
              if (op.arg->type != PhysType::I64) broken("MIN or MAX from the keys of a column not held as I64");
              if (op.values.reverse) broken("MIN or MAX from the first entry of a reverse scan");
              if (op.arg->nullable && starts_at_nulls(op.values.rest)) {
                broken("MIN or MAX from a range that starts at its part's NULLs");
              }
              if (op.values.object != op.counted.object || op.values.prefix != op.counted.prefix) {
                broken("MIN or MAX from another range than the one counted");
              }
              if (op.values.covering) read(*op.arg);
            }
          } else if constexpr (std::is_same_v<T, IndexJoin>) {
            if (op.inner_key.empty()) broken("a join whose inner key has no part");
            if (!op.left_outer && !op.match.empty()) broken("an inner index join with match conjuncts");
            for (const auto& e : op.inner_key) {
              if (op.left_outer) {
                params(e);  // a NULL part matches nothing: codegen does not probe
              } else {
                null_checked(e);
              }
              reads(e);
              if (e.kind == Expr::Kind::Column) held_above(*op.outer, e.col);
            }
            // Left outer: its ON, the pair's, as a filter's conjuncts are.
            for (const auto& c : op.match) {
              if (logical::is_predicate(c)) {
                predicate(c);
                continue;
              }
              if (c.kind != Expr::Kind::Compare) continue;
              if (c.args[0].kind == Expr::Kind::Column && c.args[0].col.type == PhysType::F64) {
                broken("a match conjunct on a DOUBLE column");
              }
              operand(c.args[1], c.args[0].col.table);
              reads(c);
            }
            node(*op.outer, &n);
          } else if constexpr (std::is_same_v<T, NestedLoop>) {
            auto outer = one_row_tables(*op.outer);
            if (!outer) {
              // Read once per outer row: no count of its own per row.
              if (loops(*op.outer).empty()) broken("a nested loop whose outer side is neither one row nor a loop");
              if (std::holds_alternative<Choice>(op.inner->op)) broken("a choice point read once per outer row");
              outer = tables_of(*op.outer);
            }
            for (const auto* b : branches(*op.inner)) inner_side(*b, *outer, op.left_outer);
            // Left outer: its row of NULLs is a row, so the inner side reads rows.
            for (const auto* b : branches(*op.inner)) {
              if (const auto* sc = scan_below(*b); op.left_outer && sc != nullptr && sc->covering) {
                broken("a left outer nested loop whose inner scan covers");
              }
            }
            if (!op.left_outer && !op.guard.empty()) broken("an inner nested loop with a guard");
            node(*op.outer, &n);
            // Left outer: the inner side's parameters are its guard's.
            auto before = guarded_.size();
            guarded_.insert(guarded_.end(), op.guard.begin(), op.guard.end());
            for (auto g : op.guard) param(g);
            node(*op.inner, &n);
            guarded_.resize(before);
          } else if constexpr (std::is_same_v<T, HashJoin>) {
            if (op.keys.empty()) broken("a hash join with no key");
            // The hash table holds the row of the build side's one loop.
            if (loops(*op.build) != std::vector<std::uint32_t>{op.held}) {
              broken("a hash join whose build side is not one loop over the table it holds");
            }
            if (std::ranges::find(covering_, op.held) != covering_.end()) broken("a hash join that holds a covering scan's entry");
            auto probed = tables_of(*op.probe);
            if (std::ranges::find(probed, op.held) != probed.end()) broken("a hash join that holds a table its probe side reads");
            for (const auto& k : op.keys) {
              if (k.build.table != op.held) broken("a hash key outside the table held");
              if (std::ranges::find(probed, k.probe.table) == probed.end()) broken("a hash key outside the probe side");
              if (k.build.type != k.probe.type) broken("a hash key of two types");
              if (k.build.type == PhysType::F64) broken("a hash key on a DOUBLE column");
              read(k.probe);
            }
            node(*op.build, &n);
            node(*op.probe, &n);
          } else if constexpr (std::is_same_v<T, Choice>) {
            scan(op.counted);
            if (op.threshold < 0) broken("a choice point with a negative threshold");
            if (!op.below || !op.at_or_above) broken("a choice point without two plans");
            // The choice by the budget at a hash join counts its build side's
            // range; its other plan reads the same tables.
            if (const auto* h = hash_below(*op.below)) {
              const auto* built = scan_below(*h->build);
              if (built == nullptr || built->table != op.counted.table) {
                broken("a choice point at a hash join that counts another range than its build side's");
              }
              counts_first(op, *built, split_below(*h->build));
              // A table looked up again above a sort is read twice.
              auto distinct = [](std::vector<std::uint32_t> v) {
                std::ranges::sort(v);
                v.erase(std::ranges::unique(v).begin(), v.end());
                return v;
              };
              if (distinct(tables_of(*op.below)) != distinct(tables_of(*op.at_or_above))) {
                broken("a choice point at a hash join whose other plan reads other tables");
              }
              node(taken(op), &n);
              return;
            }
            // The choice by the budget at a grouped aggregate counts its
            // input's range; its other plan, streamed, reads the same tables.
            if (const auto* g = group_below(*op.below)) {
              const auto* other = group_below(*op.at_or_above);
              const auto* counted = scan_below(*g->input);
              if (g->streamed || other == nullptr || !other->streamed) {
                broken("a choice point at a grouped aggregate whose plans are not a structure and a stream");
              }
              if (counted == nullptr || counted->table != op.counted.table) {
                broken("a choice point at a grouped aggregate that counts another range than its input's");
              }
              counts_first(op, *counted, split_below(*g->input));
              auto a = tables_of(*op.below);
              auto b = tables_of(*op.at_or_above);
              std::ranges::sort(a);
              std::ranges::sort(b);
              if (a != b || g->keys != other->keys || g->dependent != other->dependent || g->aggs != other->aggs) {
                broken("a choice point at a grouped aggregate whose other plan groups other rows");
              }
              node(taken(op), parent);
              return;
            }
            const auto* first = scan_below(*op.below);
            const auto* second = scan_below(*op.at_or_above);
            if (first == nullptr || second == nullptr) {
              broken("a choice point whose plan is not a scan");
            }
            if (first->table != op.counted.table || second->table != op.counted.table) {
              broken("a choice point whose plans read another table than the one counted");
            }
            counts_first(op, *first, split_below(*op.below));
            node(taken(op), &n);
          } else if constexpr (std::is_same_v<T, Filter>) {
            // A HAVING compares a key or an aggregate of the groups below.
            if (const auto* g = group_below(*op.input)) {
              const auto columns = g->columns();
              for (const auto& c : op.conjuncts) {
                const auto& lhs = c.args[0];
                auto known = lhs.kind == Expr::Kind::Column ? std::ranges::find(columns, lhs.col) != columns.end()
                                                            : std::ranges::find(g->aggs, lhs) != g->aggs.end();
                if (!known || c.args[1].kind != Expr::Kind::Param) broken("a HAVING that compares no key or aggregate of its groups");
                if (lhs.kind == Expr::Kind::Column && lhs.col.type == PhysType::F64) broken("a filter on a DOUBLE column");
                params(c);
              }
              node(*op.input, &n);
              return;
            }
            for (const auto& c : op.conjuncts) {
              if (logical::is_predicate(c)) {
                predicate(c);
                continue;
              }
              if (c.kind != Expr::Kind::Compare) continue;
              if (c.args[0].kind == Expr::Kind::Column && c.args[0].col.type == PhysType::F64) {
                broken("a filter on a DOUBLE column");
              }
              operand(c.args[1], c.args[0].col.table);
              reads(c);
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Sort>) {
            if (op.keys.empty()) broken("a sort with no key");
            if (op.keep < 0) broken("a sort that keeps a negative number of rows");
            if (op.groups) {
              // The sorter holds each group's key: the groups of a structure.
              const auto* g = group_below(*op.input);
              if (g == nullptr || g->streamed) broken("a sort of groups not over a structure of them");
              const auto columns = g->columns();
              for (const auto& k : op.keys) {
                if (std::ranges::find(columns, k.col) == columns.end()) broken("a sort of groups by a column not of them");
              }
              node(*op.input, &n);
              return;
            }
            // A sorter per loop it holds, outer first, each holding its
            // loop's row; a covering scan's entry has none.
            auto held = op.payloads.begin();
            for (auto t : loops(*op.input)) {
              if (held != op.payloads.end() && *held == t) ++held;
            }
            if (held != op.payloads.end()) broken("a sort whose sorters hold other rows than its loops'");
            for (auto t : op.payloads) {
              if (std::ranges::find(covering_, t) != covering_.end()) broken("a sort over a covering scan");
            }
            auto in = tables_of(*op.input);
            for (const auto& k : op.keys) {
              if (std::ranges::find(in, k.col.table) == in.end()) broken("a sort key outside its input's tables");
              read(k.col);
            }
            // A row's NULL is tested before it is added, from a row in hand.
            for (const auto& c : op.not_null) {
              if (std::ranges::find(in, c.table) == in.end()) broken("a sort's NULL test outside its input's tables");
              read(c);
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Limit>) {
            if (parent == nullptr || (!std::holds_alternative<Emit>(parent->op) && !std::holds_alternative<GroupEmit>(parent->op))) {
              broken("a LIMIT not under the rows emitted");
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Emit>) {
            for (const auto& t : op.targets) {
              if (const auto* c = std::get_if<ColumnRef>(&t)) {
                read(*c);
                held_above(*op.input, *c);
              } else if (const auto* x = std::get_if<ExprTarget>(&t)) {
                value(x->expr);
                params(x->expr);
                std::vector<ColumnRef> cols;
                logical::columns_of(x->expr, cols);
                for (const auto& c : cols) {
                  read(c);
                  held_above(*op.input, c);
                }
              }
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Aggregate>) {
            if (op.agg.kind != Expr::Kind::Aggregate) broken("an aggregate that is not an aggregate call");
            if (op.agg.fn != Expr::Fn::Count && op.agg.fn != Expr::Fn::Sum) {
              broken("an aggregate row of other than COUNT or SUM");
            }
            if ((op.agg.fn != Expr::Fn::Count || op.agg.distinct) && op.agg.args.empty()) {
              broken("SUM or COUNT(DISTINCT) with no argument");
            }
            reads(op.agg);
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, GroupAggregate>) {
            auto in = tables_of(*op.input);
            auto of_input = [&](const ColumnRef& c) { return std::ranges::find(in, c.table) != in.end(); };
            if (!op.streamed && op.keys.empty()) broken("a structure of groups with no key");
            if (op.keys.empty() && !op.dependent.empty()) broken("a dependent column of one group of every row");
            const auto columns = op.columns();
            for (const auto& k : columns) {
              if (!of_input(k)) broken("a GROUP BY key outside its input's tables");
              if (op.streamed && k.type != PhysType::I64 && k.type != PhysType::Bool) broken("a streamed GROUP BY key held in no local");
              read(k);
              held_above(*op.input, k);
            }
            for (const auto& d : op.dependent) {
              if (std::ranges::find(op.keys, d) != op.keys.end()) broken("a dependent column that is a key");
            }
            // A structure holds a row per group for each column it reads
            // from one; a stream holds no row.
            if (!op.streamed) {
              for (auto t : group_layout(columns, op.aggs).held) {
                if (std::ranges::find(covering_, t) != covering_.end()) broken("a structure of groups that holds a covering scan's entry");
              }
            }
            for (const auto& a : op.aggs) {
              if (a.kind != Expr::Kind::Aggregate) broken("an aggregate that is not an aggregate call");
              if ((a.fn != Expr::Fn::Count || a.distinct) && a.args.empty()) broken("an aggregate call with no argument");
              if (!a.args.empty() && !of_input(a.args[0].col)) broken("an aggregate's argument outside its input's tables");
              if (!a.args.empty()) held_above(*op.input, a.args[0].col);
              reads(a);
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, GroupEmit>) {
            // Each output a key or an aggregate of the groups of every plan
            // the rows may come from.
            std::vector<const Node*> below{op.input.get()};
            if (const auto* l = std::get_if<Limit>(&op.input->op)) below = {l->input.get()};
            if (const auto* c = std::get_if<Choice>(&below[0]->op)) below = {c->below.get(), c->at_or_above.get()};
            for (const auto* b : below) {
              const auto* g = group_below(*b);
              if (g == nullptr) broken("rows of groups over no grouped aggregate");
              const auto columns = g->columns();
              for (const auto& o : op.outputs) {
                auto known = o.kind == Expr::Kind::Column ? std::ranges::find(columns, o.col) != columns.end()
                                                          : std::ranges::find(g->aggs, o) != g->aggs.end();
                if (!known) broken("a grouped output that is no key or aggregate of its groups");
              }
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Insert>) {
            if (op.values.empty()) broken("an insert that sets no column");
            for (const auto& [col, e] : op.values) params(e);
          } else if constexpr (std::is_same_v<T, Update>) {
            if (op.set.empty()) broken("an update that sets no column");
            for (const auto& [col, e] : op.set) {
              params(e);
              if (e.kind == Expr::Kind::Column && e.col.type != col.type) broken("a column set from a column of another type");
              if ((e.kind == Expr::Kind::Add || e.kind == Expr::Kind::Sub) &&
                  (e.args[0].kind != Expr::Kind::Column || e.args[0].col.type != PhysType::I64 || col.type != PhysType::I64)) {
                broken("column + or - parameter on a type other than an integer");
              }
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Delete>) {
            node(*op.input, &n);
          }
        },
        n.op);
  }

  const Plan& p_;
  std::vector<const Choice*> second_;    // the choice points whose second plan is taken
  std::vector<std::uint32_t> covering_;  // every table a covering scan reads, in the branches taken
  // The parameters an outer nested loop's guard checks, while its inner
  // side is checked.
  mutable std::vector<std::uint32_t> guarded_;
};

}  // namespace

void check(const Plan& p) { Checker{p}.plan(); }

namespace {

// Each choice point under n, outer first.
template <typename F>
void each_choice(Node& n, const F& f) {
  std::visit(
      [&](auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, Choice>) {
          f(op);
          each_choice(*op.below, f);
          each_choice(*op.at_or_above, f);
        } else if constexpr (std::is_same_v<T, IndexJoin>) {
          each_choice(*op.outer, f);
        } else if constexpr (std::is_same_v<T, NestedLoop>) {
          each_choice(*op.outer, f);
          each_choice(*op.inner, f);
        } else if constexpr (std::is_same_v<T, HashJoin>) {
          each_choice(*op.build, f);
          each_choice(*op.probe, f);
        } else if constexpr (requires { op.input; }) {
          each_choice(*op.input, f);
        }
      },
      n.op);
}

}  // namespace

auto choices(const Plan& p) -> std::size_t {
  if (!p.root) return 0;
  std::size_t n = 0;
  each_choice(*p.root, [&](const Choice&) { ++n; });
  return n;
}

void override_thresholds(Plan& p, const std::vector<std::int64_t>& t) {
  if (t.size() != choices(p)) throw std::logic_error{"physical plan: a threshold per choice point"};
  std::size_t i = 0;
  each_choice(*p.root, [&](Choice& c) { c.threshold = t[i++]; });
}

}  // namespace bytecask::sql::physical
