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
// loop, and a nested loop), the choice points, Filter, Sort, Limit,
// Aggregate and the sinks: Emit for result rows, Insert, Update, Delete. No
// hash join yet. check() enforces what the types can't express.

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
};

struct Range {
  KeyBound lo, hi;
};

// No rows, and nothing read: the input a conjunct always false leaves.
struct Empty {};

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
  std::variant<std::monostate, Range> rest;  // nothing more, or a range
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

// MIN answered from the keys: the scan's first entry, `arg` being the ASC
// part after its prefix and its range starting past that part's NULLs.
struct MinFirstEntry {
  Scan scan;
  ColumnRef arg;
  bool emits{true};
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

// An index nested loop: for each outer row, the inner table's row named by
// its whole primary key, each part a parameter or a column of the outer row.
// An outer row whose key column is NULL, or with no inner row, joins nothing.
struct IndexJoin {
  NodePtr outer;
  std::uint32_t inner{0};
  std::vector<Expr> inner_key;  // in the inner primary key's order
};

// A nested loop whose inner side reads parameters alone: for each outer row,
// every row of the inner scan, under the filters on its own table and the
// join's, which compare its columns with the outer side's. Under an outer
// side of at most one row (a point lookup and lookups by key) the inner
// side is read at most once; under any other, once per outer row.
struct NestedLoop {
  NodePtr outer;
  NodePtr inner;
};

// A choice point (docs/planner_design.md, *Choosing at run time*): one
// table's rows by one of two plans, picked when the plan runs by
// bc_count_range over the first one's range, limited to the threshold. A
// count below it takes the first, `below`; one that reaches it, the second.
// Each is a scan under its filters, or such a scan sorted: the choice by the
// memory budget between a sort and a scan in the ORDER BY's order. A threshold of 0 always takes the
// second, and INT64_MAX the first (override_thresholds).
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

// A pipeline breaker: every row of its input added to a sorter, keyed by
// `keys` (bc_sort_key_*), then streamed in that order. A sorter holds one
// view per row, so the input reads one table in a loop, `payload`, whose
// row is the one held; the rows of tables read once before that loop stay
// in hand, and a table looked up per row after it is looked up again above
// the Sort. With keep n > 0 the sorter holds only the first n (ORDER BY …
// LIMIT n); 0 holds all.
struct Sort {
  NodePtr input;
  std::vector<SortPart> keys;
  std::int64_t keep{0};
  std::uint32_t payload{0};
};

// The first n rows of Emit's input.
struct Limit {
  NodePtr input;
  std::int64_t n{0};
};

// Result rows: the targets of each row. No target is a row of no columns, as
// PostgreSQL's `SELECT FROM`.
struct Emit {
  NodePtr input;
  std::vector<ColumnRef> targets;  // from the row, or from the entry's key when the scan covers
};

// One aggregate row, of COUNT(*), or COUNT, COUNT(DISTINCT), SUM or MIN of
// its argument's non-NULL values, each held as an int64_t.
struct Aggregate {
  NodePtr input;
  Expr agg;
  bool emits{true};  // false under LIMIT 0
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
  std::variant<Empty, PointGet, Scan, NullSplitScan, CountRange, MinFirstEntry, IndexJoin, NestedLoop, Choice, Filter,
               Sort, Limit, Emit, Aggregate, Insert, Update, Delete>
      op;
};

template <typename T>
[[nodiscard]] auto node(T op) -> NodePtr {
  return std::make_unique<Node>(Node{std::move(op)});
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
  PhysType type{PhysType::I64};
};

struct Plan {
  NodePtr root;
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

// Whether two expressions are the same: kind, operands and values.
auto same(const Expr& a, const Expr& b) -> bool {
  if (a.kind != b.kind || a.param != b.param || a.constant != b.constant || !(a.col == b.col) || a.op != b.op ||
      a.args.size() != b.args.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.args.size(); ++i) {
    if (!same(a.args[i], b.args[i])) return false;
  }
  return true;
}

auto same(const KeyBound& a, const KeyBound& b) -> bool {
  return a.kind == b.kind && a.successor == b.successor && a.value.has_value() == b.value.has_value() &&
         (!a.value || same(*a.value, *b.value));
}

class Checker {
public:
  explicit Checker(const Plan& p) : p_{p} {}

  void plan() {
    if (p_.constants.empty()) broken("a plan with no constants");
    if (!p_.root) broken("a plan with no operator");
    for (auto n : p_.null_checks) param(n);
    covering_tables(*p_.root, covering_);
    node(*p_.root, nullptr);
  }

private:
  void param(std::uint32_t n) const {
    if (n >= p_.param_types.size()) broken("a parameter past the statement's");
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

  // The scan an operator's rows come from, or null.
  static auto scan_below(const Node& n) -> const Scan* {
    if (const auto* s = std::get_if<Scan>(&n.op)) return s;
    if (const auto* s = std::get_if<NullSplitScan>(&n.op)) return &s->present;
    if (const auto* f = std::get_if<Filter>(&n.op)) return scan_below(*f->input);
    if (const auto* l = std::get_if<Limit>(&n.op)) return scan_below(*l->input);
    if (const auto* o = std::get_if<Sort>(&n.op)) return scan_below(*o->input);
    return nullptr;
  }

  static auto split_below(const Node& n) -> bool {
    if (std::holds_alternative<NullSplitScan>(n.op)) return true;
    if (const auto* f = std::get_if<Filter>(&n.op)) return split_below(*f->input);
    if (const auto* o = std::get_if<Sort>(&n.op)) return split_below(*o->input);
    return false;
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
  }
  static auto tables_of(const Node& n) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> out;
    tables_of(n, out);
    return out;
  }

  // The loops over a cursor or a sorter that a node's rows come from, one
  // inside another; and the table the innermost one reads, if any.
  static auto loops(const Node& n) -> std::pair<std::size_t, std::optional<std::uint32_t>> {
    if (const auto* s = scan_below(n); s != nullptr && !std::holds_alternative<Sort>(n.op)) return {1, s->table};
    if (const auto* o = std::get_if<Sort>(&n.op)) return {1, o->payload};
    if (const auto* f = std::get_if<Filter>(&n.op)) return loops(*f->input);
    if (const auto* l = std::get_if<Limit>(&n.op)) return loops(*l->input);
    if (const auto* c = std::get_if<Choice>(&n.op)) return loops(*c->below);
    if (const auto* j = std::get_if<IndexJoin>(&n.op)) return loops(*j->outer);
    if (const auto* j = std::get_if<NestedLoop>(&n.op)) {
      auto [outer, table] = loops(*j->outer);
      auto [inner, inner_table] = loops(*j->inner);
      return {outer + inner, inner_table ? inner_table : table};
    }
    return {0, std::nullopt};
  }

  // The tables whose rows come from a covering scan's key, not a row.
  static void covering_tables(const Node& n, std::vector<std::uint32_t>& out) {
    if (const auto* s = scan_below(n); s != nullptr && s->covering) out.push_back(s->table);
    std::visit(
        [&](const auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, MinFirstEntry>) {
            if (op.scan.covering) out.push_back(op.scan.table);
          } else if constexpr (std::is_same_v<T, IndexJoin>) {
            covering_tables(*op.outer, out);
          } else if constexpr (std::is_same_v<T, NestedLoop>) {
            covering_tables(*op.outer, out);
            covering_tables(*op.inner, out);
          } else if constexpr (std::is_same_v<T, Choice>) {
            covering_tables(*op.below, out);
            covering_tables(*op.at_or_above, out);
          } else if constexpr (requires { op.input; }) {
            covering_tables(*op.input, out);
          }
        },
        n.op);
  }

  // The inner side of a nested loop, one of its plans: a scan under filters
  // that each compare a column of the scan's table, read from its row, with
  // a parameter or a column of the outer side.
  static void inner_side(const Node& inner, const std::vector<std::uint32_t>& outer) {
    const auto* sc = scan_below(inner);
    if (sc == nullptr || std::holds_alternative<Sort>(inner.op)) broken("a nested loop whose inner side is not a scan");
    for (const auto* in = &inner; const auto* f = std::get_if<Filter>(&in->op); in = f->input.get()) {
      for (const auto& c : f->conjuncts) {
        if (c.kind != Expr::Kind::Compare) continue;
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
  // the whole prefix, which holds both of its ranges.
  static void counts_first(const Choice& c, const Scan& first, bool split) {
    auto same_range = [&] {
      if (split) return std::holds_alternative<std::monostate>(c.counted.rest);
      const auto* a = std::get_if<Range>(&c.counted.rest);
      const auto* b = std::get_if<Range>(&first.rest);
      if (a == nullptr || b == nullptr) return a == b;
      return same(a->lo, b->lo) && same(a->hi, b->hi);
    };
    auto same_prefix = c.counted.prefix.size() == first.prefix.size() &&
                       std::ranges::equal(c.counted.prefix, first.prefix, [](const Expr& a, const Expr& b) { return same(a, b); });
    if (c.counted.object != first.object || !same_prefix || c.counted.reverse != first.reverse || !same_range()) {
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
          } else if constexpr (std::is_same_v<T, MinFirstEntry>) {
            scan(op.scan);
            if (op.scan.reverse) broken("MIN from the first entry of a reverse scan");
            if (op.scan.covering) read(op.arg);
          } else if constexpr (std::is_same_v<T, IndexJoin>) {
            if (op.inner_key.empty()) broken("a join whose inner key has no part");
            for (const auto& e : op.inner_key) {
              null_checked(e);
              reads(e);
            }
            node(*op.outer, &n);
          } else if constexpr (std::is_same_v<T, NestedLoop>) {
            auto outer = one_row_tables(*op.outer);
            if (!outer) {
              // Read once per outer row: no count of its own per row.
              if (loops(*op.outer).first == 0) broken("a nested loop whose outer side is neither one row nor a loop");
              if (std::holds_alternative<Choice>(op.inner->op)) broken("a choice point read once per outer row");
              outer = tables_of(*op.outer);
            }
            for (const auto* b : branches(*op.inner)) inner_side(*b, *outer);
            node(*op.outer, &n);
            node(*op.inner, &n);
          } else if constexpr (std::is_same_v<T, Choice>) {
            scan(op.counted);
            if (op.threshold < 0) broken("a choice point with a negative threshold");
            if (!op.below || !op.at_or_above) broken("a choice point without two plans");
            const auto* first = scan_below(*op.below);
            const auto* second = scan_below(*op.at_or_above);
            if (first == nullptr || second == nullptr) {
              broken("a choice point whose plan is not a scan");
            }
            if (first->table != op.counted.table || second->table != op.counted.table) {
              broken("a choice point whose plans read another table than the one counted");
            }
            counts_first(op, *first, split_below(*op.below));
            node(*op.below, &n);
            node(*op.at_or_above, &n);
          } else if constexpr (std::is_same_v<T, Filter>) {
            for (const auto& c : op.conjuncts) {
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
            auto [depth, table] = loops(*op.input);
            if (depth != 1) broken("a sort whose input is not one loop");
            if (table != op.payload) broken("a sort that holds another row than its loop's");
            // The sorter holds the row; a covering scan's entry has none.
            if (std::ranges::find(covering_, op.payload) != covering_.end()) broken("a sort over a covering scan");
            auto in = tables_of(*op.input);
            for (const auto& k : op.keys) {
              if (std::ranges::find(in, k.col.table) == in.end()) broken("a sort key outside its input's tables");
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Limit>) {
            if (parent == nullptr || !std::holds_alternative<Emit>(parent->op)) broken("a LIMIT not under the rows emitted");
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Emit>) {
            for (const auto& t : op.targets) read(t);
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Aggregate>) {
            if (op.agg.kind != Expr::Kind::Aggregate) broken("an aggregate that is not an aggregate call");
            if ((op.agg.fn != Expr::Fn::Count || op.agg.distinct) && op.agg.args.empty()) {
              broken("SUM, MIN or COUNT(DISTINCT) with no argument");
            }
            reads(op.agg);
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
  std::vector<std::uint32_t> covering_;  // every table a covering scan reads, in any plan of a choice point
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
        } else if constexpr (requires { op.input; }) {
          each_choice(*op.input, f);
        }
      },
      n.op);
}

}  // namespace

auto choices(const Plan& p) -> std::size_t {
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
