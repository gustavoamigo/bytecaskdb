// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// rewrite.cppm — the fixed rewrites (docs/planner_design.md, *Rewrites*):
// each turns a logical plan into an equivalent one, the same plan every
// time, and none needs a cost. Applied in order, each to a fixpoint:
//   1. constant folding: an expression of constants becomes a constant; a
//      conjunct always true is dropped, and one always false makes its input
//      Empty, as does an empty side of a Join;
//   2. predicate pushdown: a conjunct moves to the lowest operator whose
//      columns it reads, a Filter on one Get or a Join's predicate; two
//      Filters in a row become one, so a Get has at most one;
//   3. projection pruning: each operator's `output` is the columns
//      something above it reads.

module;
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.rewrite;

export import bytecask.sql.logical;

export namespace bytecask::sql::logical {

void rewrite(Node& root);

}  // namespace bytecask::sql::logical

namespace bytecask::sql::logical {

namespace {

using Kind = Expr::Kind;

auto is_const(const Expr& e) -> bool { return e.kind == Kind::Const; }

auto is_bool(const Expr& e, bool v) -> bool {
  return is_const(e) && e.type.kind == SqlKind::Boolean && e.constant == (v ? 1 : 0);
}

// a ± b, or nullopt where it leaves int64_t: left for the plan to raise, as
// PostgreSQL's operator would when it runs.
auto add(std::int64_t a, std::int64_t b, bool sub) -> std::optional<std::int64_t> {
  std::int64_t out = 0;
  if (sub ? __builtin_sub_overflow(a, b, &out) : __builtin_add_overflow(a, b, &out)) return std::nullopt;
  return out;
}

auto holds(Op op, std::int64_t a, std::int64_t b) -> bool {
  switch (op) {
    case Op::Eq: return a == b;
    case Op::Lt: return a < b;
    case Op::Le: return a <= b;
    case Op::Gt: return a > b;
    case Op::Ge: return a >= b;
  }
  return false;
}

// A SMALLINT, INTEGER, BIGINT or DECIMAL constant's scale: the power of ten
// its int64_t is scaled by. nullopt for any other type.
auto exact_scale(const SqlType& t) -> std::optional<std::uint8_t> {
  switch (t.kind) {
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt: return std::uint8_t{0};
    case SqlKind::Decimal: return t.scale;
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::Numeric: return std::nullopt;
  }
  return std::nullopt;
}

// v scaled up by 10^by, or nullopt where it leaves int64_t.
auto rescale(std::int64_t v, std::uint8_t by) -> std::optional<std::int64_t> {
  for (std::uint8_t i = 0; i < by; ++i) {
    if (__builtin_mul_overflow(v, std::int64_t{10}, &v)) return std::nullopt;
  }
  return v;
}

// The two constants' values in one representation, so comparing the int64_t
// values compares what they stand for: exact numbers at the larger scale; a
// BOOLEAN, DATE, TIMESTAMP or TIME only with one of its own kind. nullopt
// when they have none, left for the plan.
auto comparable(const Expr& a, const Expr& b) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
  auto sa = exact_scale(a.type);
  auto sb = exact_scale(b.type);
  if (sa && sb) {
    auto scale = std::max(*sa, *sb);
    auto va = rescale(a.constant, static_cast<std::uint8_t>(scale - *sa));
    auto vb = rescale(b.constant, static_cast<std::uint8_t>(scale - *sb));
    if (!va || !vb) return std::nullopt;
    return std::pair{*va, *vb};
  }
  auto ordinal = [](SqlKind k) {
    return k == SqlKind::Boolean || k == SqlKind::Date || k == SqlKind::Timestamp || k == SqlKind::TimestampTz || k == SqlKind::Time;
  };
  if (a.type.kind == b.type.kind && ordinal(a.type.kind)) return std::pair{a.constant, b.constant};
  return std::nullopt;
}

// Folds e bottom up. True if it changed.
auto fold(Expr& e) -> bool {
  auto changed = false;
  for (auto& a : e.args) changed = fold(a) || changed;
  if (e.args.size() != 2 || !is_const(e.args[0]) || !is_const(e.args[1])) return changed;
  switch (e.kind) {
    case Kind::Add:
    case Kind::Sub: {
      // Only of operands in the result's representation, and of the
      // expression's own type, so a SMALLINT or INTEGER result outside it is
      // the plan's to refuse.
      auto scale = exact_scale(e.type);
      auto same = [&](const Expr& a) { return scale && exact_scale(a.type) == scale; };
      if (!same(e.args[0]) || !same(e.args[1])) return changed;
      auto v = add(e.args[0].constant, e.args[1].constant, e.kind == Kind::Sub);
      if (!v || out_of_range(e.type, *v)) return changed;
      e = constant(*v, e.type);
      return true;
    }
    case Kind::Compare: {
      auto v = comparable(e.args[0], e.args[1]);
      if (!v) return changed;
      e = constant(holds(e.op, v->first, v->second) ? 1 : 0, SqlType{SqlKind::Boolean});
      return true;
    }
    case Kind::Column:
    case Kind::Param:
    case Kind::Const:
    case Kind::Mul:
    case Kind::Div:
    case Kind::Aggregate:
    case Kind::And:
    case Kind::Or:
    case Kind::Not:
    case Kind::IsNull:
    case Kind::Like:
    case Kind::Truth:
    case Kind::InSet:
    case Kind::Case:
    case Kind::Coalesce: return changed;
  }
  return changed;
}

// Folds each conjunct and drops those always true. True in `never` when one
// is always false.
auto fold_conjuncts(std::vector<Expr>& conjuncts, bool& never) -> bool {
  auto changed = false;
  for (auto& c : conjuncts) changed = fold(c) || changed;
  never = std::ranges::any_of(conjuncts, [](const Expr& c) { return is_bool(c, false); });
  auto dropped = std::erase_if(conjuncts, [](const Expr& c) { return is_bool(c, true); });
  return changed || dropped > 0;
}

// The tables a subtree reads.
void tables_of(const Node& n, std::vector<const Table*>& out);

auto empty_of(const Node& n) -> Node {
  Empty e;
  tables_of(n, e.tables);
  return Node{std::move(e), {}};
}

auto fold_constants(Node& n) -> bool {
  auto never = false;
  auto changed = std::visit(
      [&](auto& op) {
        using T = std::decay_t<decltype(op)>;
        auto c = false;
        if constexpr (std::is_same_v<T, Filter>) {
          // The input first, so an input it folds to Empty empties this
          // Filter in the same pass.
          c = fold_constants(*op.input);
          c = fold_conjuncts(op.conjuncts, never) || c;
          never = never || std::holds_alternative<Empty>(op.input->op);
        } else if constexpr (std::is_same_v<T, Join>) {
          // An inner join with an empty side, or a predicate never true, is
          // empty. A left outer join keeps its left side's rows whatever its
          // right side and its ON hold.
          auto on_never = false;
          c = fold_conjuncts(op.predicate, on_never);
          never = never || (!op.left_outer && on_never);
          c = fold_constants(*op.left) || c;
          c = fold_constants(*op.right) || c;
          never = never || std::holds_alternative<Empty>(op.left->op) ||
                  (!op.left_outer && std::holds_alternative<Empty>(op.right->op));
        } else if constexpr (std::is_same_v<T, Project> || std::is_same_v<T, Aggregate>) {
          auto& outs = [&]() -> std::vector<Output>& {
            if constexpr (std::is_same_v<T, Project>) {
              return op.exprs;
            } else {
              return op.aggs;
            }
          }();
          for (auto& o : outs) c = fold(o.expr) || c;
        } else if constexpr (std::is_same_v<T, Update>) {
          for (auto& [col, e] : op.set) c = fold(e) || c;
        } else if constexpr (std::is_same_v<T, Insert>) {
          for (auto& v : op.values) {
            if (v) c = fold(*v) || c;
          }
        }
        return c;
      },
      n.op);
  // A conjunct always false: no rows, and nothing to read.
  if (never) {
    n = empty_of(n);
    return true;
  }
  // A Filter left with no conjunct is its input.
  if (auto* f = std::get_if<Filter>(&n.op); f != nullptr && f->conjuncts.empty()) {
    auto input = std::move(f->input);
    n = std::move(*input);
    return true;
  }
  if (std::holds_alternative<Filter>(n.op)) return changed;  // its input is folded
  if (auto* in = input_of(n); in != nullptr && *in) changed = fold_constants(**in) || changed;
  return changed;
}

void tables_of(const Node& n, std::vector<const Table*>& out) {
  if (const auto* g = std::get_if<Get>(&n.op)) {
    out.push_back(g->table);
    return;
  }
  if (const auto* e = std::get_if<Empty>(&n.op)) {
    out.insert(out.end(), e->tables.begin(), e->tables.end());
    return;
  }
  if (const auto* j = std::get_if<Join>(&n.op)) {
    tables_of(*j->left, out);
    tables_of(*j->right, out);
    return;
  }
  if (const auto* in = input_of(n); in != nullptr && *in) tables_of(**in, out);
}

// Whether e is a predicate that reads no column: a subquery's EXISTS or
// its flags, the same for every row.
auto reads_none(const Expr& e) -> bool {
  std::vector<ColumnRef> cols;
  columns_of(e, cols);
  return cols.empty() && logical::is_predicate(e);
}

auto reads_only(const Expr& e, const std::vector<const Table*>& tables) -> bool {
  std::vector<ColumnRef> cols;
  columns_of(e, cols);
  return !cols.empty() && std::ranges::all_of(cols, [&](const ColumnRef& c) {
    return std::ranges::find(tables, c.table, &Table::id) != tables.end();
  });
}

// Appends conjuncts to a Filter directly above `side`, made if there is none.
void push_onto(NodePtr& side, std::vector<Expr> conjuncts) {
  if (conjuncts.empty()) return;
  if (auto* f = std::get_if<Filter>(&side->op)) {
    for (auto& c : conjuncts) f->conjuncts.push_back(std::move(c));
    return;
  }
  side = node(Filter{std::move(side), std::move(conjuncts)});
}

auto push_down(Node& n) -> bool {
  auto changed = false;
  // Two Filters in a row are one, the lower one's conjuncts first: a Get has
  // at most one.
  while (auto* f = std::get_if<Filter>(&n.op)) {
    auto* below = std::get_if<Filter>(&f->input->op);
    if (below == nullptr) break;
    for (auto& c : f->conjuncts) below->conjuncts.push_back(std::move(c));
    auto input = std::move(f->input);
    n = std::move(*input);
    changed = true;
  }
  if (auto* f = std::get_if<Filter>(&n.op); f != nullptr && std::holds_alternative<Join>(f->input->op)) {
    auto& j = std::get<Join>(f->input->op);
    std::vector<const Table*> left, right;
    tables_of(*j.left, left);
    tables_of(*j.right, right);
    auto both = left;
    both.insert(both.end(), right.begin(), right.end());
    std::vector<Expr> kept, to_left, to_right;
    for (auto& c : f->conjuncts) {
      // One that reads no column holds of every row or of none: it filters
      // the left side, whose rows an outer join keeps.
      if (reads_only(c, left) || reads_none(c)) {
        to_left.push_back(std::move(c));
      } else if (j.left_outer) {
        // Above a left outer join, a conjunct reading its right side filters
        // the rows the join gives, its NULLs among them: it stays.
        kept.push_back(std::move(c));
      } else if (reads_only(c, right)) {
        to_right.push_back(std::move(c));
      } else if (reads_only(c, both)) {
        j.predicate.push_back(std::move(c));
      } else {
        kept.push_back(std::move(c));
      }
    }
    changed = changed || kept.size() != f->conjuncts.size();
    push_onto(j.left, std::move(to_left));
    push_onto(j.right, std::move(to_right));
    f->conjuncts = std::move(kept);
    if (f->conjuncts.empty()) {
      auto input = std::move(f->input);
      n = std::move(*input);
    }
  }
  if (auto* j = std::get_if<Join>(&n.op)) {
    // A left outer join's ON conjunct reading its right side alone filters
    // that side; one reading its left side decides which pairs there are,
    // never which left rows: it stays.
    if (j->left_outer) {
      std::vector<const Table*> right;
      tables_of(*j->right, right);
      std::vector<Expr> stays, to_right;
      // One of no column decides every pair alike: a filter of the right side.
      for (auto& c : j->predicate) (reads_only(c, right) || reads_none(c) ? to_right : stays).push_back(std::move(c));
      changed = changed || !to_right.empty();
      j->predicate = std::move(stays);
      push_onto(j->right, std::move(to_right));
    }
    changed = push_down(*j->left) || changed;
    return push_down(*j->right) || changed;
  }
  if (auto* in = input_of(n); in != nullptr && *in) changed = push_down(**in) || changed;
  return changed;
}

void add_columns(std::vector<ColumnRef>& to, const std::vector<ColumnRef>& from) {
  for (const auto& c : from) {
    if (std::ranges::find(to, c) == to.end()) to.push_back(c);
  }
}

auto columns(const std::vector<Expr>& es) -> std::vector<ColumnRef> {
  std::vector<ColumnRef> out;
  for (const auto& e : es) columns_of(e, out);
  return out;
}

auto all_columns(const Table& t) -> std::vector<ColumnRef> {
  std::vector<ColumnRef> out;
  for (const auto& c : t.columns) out.push_back(column_of(t.id, c).col);
  return out;
}

// n's output is `needed`; its input's, what n reads plus what it passes up.
auto prune(Node& n, std::vector<ColumnRef> needed) -> bool {
  std::vector<ColumnRef> unique;
  add_columns(unique, needed);
  auto changed = unique != n.output;
  n.output = unique;
  auto below = [&](std::vector<ColumnRef> more, bool pass) {
    auto out = pass ? unique : std::vector<ColumnRef>{};
    add_columns(out, more);
    return out;
  };
  std::visit(
      [&](auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, Filter>) {
          changed = prune(*op.input, below(columns(op.conjuncts), true)) || changed;
        } else if constexpr (std::is_same_v<T, Project>) {
          std::vector<ColumnRef> cols;
          for (const auto& o : op.exprs) columns_of(o.expr, cols);
          changed = prune(*op.input, below(cols, false)) || changed;
        } else if constexpr (std::is_same_v<T, Aggregate>) {
          std::vector<ColumnRef> cols;
          for (const auto& o : op.aggs) columns_of(o.expr, cols);
          for (const auto& g : op.group_by) columns_of(g, cols);
          for (const auto& d : op.dependent) columns_of(d, cols);
          // A HAVING above reads aggregates' arguments the select list may not.
          changed = prune(*op.input, below(cols, true)) || changed;
        } else if constexpr (std::is_same_v<T, Sort>) {
          std::vector<ColumnRef> cols;
          for (const auto& k : op.keys) columns_of(k.expr, cols);
          changed = prune(*op.input, below(cols, true)) || changed;
        } else if constexpr (std::is_same_v<T, Limit>) {
          changed = prune(*op.input, unique) || changed;
        } else if constexpr (std::is_same_v<T, Join>) {
          auto all = below(columns(op.predicate), true);
          for (auto* side : {&op.left, &op.right}) {
            std::vector<const Table*> tables;
            tables_of(**side, tables);
            std::vector<ColumnRef> mine;
            for (const auto& c : all) {
              if (std::ranges::find(tables, c.table, &Table::id) != tables.end()) mine.push_back(c);
            }
            changed = prune(**side, mine) || changed;
          }
        } else if constexpr (std::is_same_v<T, Update> || std::is_same_v<T, Delete>) {
          // A write reads each row whole: bc_row_from copies it.
          changed = prune(*op.input, all_columns(*op.table)) || changed;
        }
      },
      n.op);
  return changed;
}

}  // namespace

void rewrite(Node& root) {
  // A set operation's arms are queries of their own, rewritten alone.
  auto* top = &root;
  if (auto* l = std::get_if<Limit>(&top->op)) top = l->input.get();
  if (auto* set = std::get_if<SetOp>(&top->op)) {
    rewrite(*set->left);
    rewrite(*set->right);
    return;
  }
  while (fold_constants(root)) {
  }
  while (push_down(root)) {
  }
  while (prune(root, {})) {
  }
}

}  // namespace bytecask::sql::logical
