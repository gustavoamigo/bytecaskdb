// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// rewrite.cppm — the fixed rewrites (docs/planner_design.md, *Rewrites*):
// each turns a logical plan into an equivalent one, the same plan every
// time, and none needs a cost. Applied in order, each to a fixpoint:
//   1. constant folding: an expression of constants becomes a constant; a
//      conjunct always true is dropped, one always false is all its Filter
//      keeps;
//   2. predicate pushdown: a conjunct moves to the lowest operator whose
//      columns it reads, a Filter on one Get or a Join's predicate;
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

// Folds e bottom up. True if it changed.
auto fold(Expr& e) -> bool {
  auto changed = false;
  for (auto& a : e.args) changed = fold(a) || changed;
  if (e.args.size() != 2 || !is_const(e.args[0]) || !is_const(e.args[1])) return changed;
  const auto lhs = e.args[0].constant;
  const auto rhs = e.args[1].constant;
  switch (e.kind) {
    case Kind::Add:
    case Kind::Sub: {
      // Of the expression's own type, so a SMALLINT or INTEGER result
      // outside it is the plan's to refuse.
      auto v = add(lhs, rhs, e.kind == Kind::Sub);
      if (!v || out_of_range(e.type, *v)) return changed;
      e = constant(*v, e.type);
      return true;
    }
    case Kind::Compare: e = constant(holds(e.op, lhs, rhs) ? 1 : 0, SqlType{SqlKind::Boolean}); return true;
    case Kind::Column:
    case Kind::Param:
    case Kind::Const:
    case Kind::Aggregate: return changed;
  }
  return changed;
}

auto fold_conjuncts(std::vector<Expr>& conjuncts) -> bool {
  auto changed = false;
  for (auto& c : conjuncts) changed = fold(c) || changed;
  if (auto it = std::ranges::find_if(conjuncts, [](const Expr& c) { return is_bool(c, false); }); it != conjuncts.end()) {
    if (conjuncts.size() > 1) {
      auto never = std::move(*it);
      conjuncts.clear();
      conjuncts.push_back(std::move(never));
      changed = true;
    }
    return changed;
  }
  auto dropped = std::erase_if(conjuncts, [](const Expr& c) { return is_bool(c, true); });
  return changed || dropped > 0;
}

auto fold_constants(Node& n) -> bool {
  auto changed = std::visit(
      [&](auto& op) {
        using T = std::decay_t<decltype(op)>;
        auto c = false;
        if constexpr (std::is_same_v<T, Filter>) {
          c = fold_conjuncts(op.conjuncts);
        } else if constexpr (std::is_same_v<T, Join>) {
          c = fold_conjuncts(op.predicate);
          c = fold_constants(*op.left) || c;
          c = fold_constants(*op.right) || c;
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
  // A Filter left with no conjunct is its input.
  if (auto* f = std::get_if<Filter>(&n.op); f != nullptr && f->conjuncts.empty()) {
    auto input = std::move(f->input);
    n = std::move(*input);
    return true;
  }
  if (auto* in = input_of(n); in != nullptr && *in) changed = fold_constants(**in) || changed;
  return changed;
}

// The tables a subtree reads.
void tables_of(const Node& n, std::vector<std::uint32_t>& out) {
  if (const auto* g = std::get_if<Get>(&n.op)) {
    out.push_back(g->table->id);
    return;
  }
  if (const auto* j = std::get_if<Join>(&n.op)) {
    tables_of(*j->left, out);
    tables_of(*j->right, out);
    return;
  }
  if (const auto* in = input_of(n); in != nullptr && *in) tables_of(**in, out);
}

auto reads_only(const Expr& e, const std::vector<std::uint32_t>& tables) -> bool {
  std::vector<ColumnRef> cols;
  columns_of(e, cols);
  return !cols.empty() && std::ranges::all_of(cols, [&](const ColumnRef& c) { return std::ranges::find(tables, c.table) != tables.end(); });
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
  if (auto* f = std::get_if<Filter>(&n.op); f != nullptr && std::holds_alternative<Join>(f->input->op)) {
    auto& j = std::get<Join>(f->input->op);
    std::vector<std::uint32_t> left, right;
    tables_of(*j.left, left);
    tables_of(*j.right, right);
    auto both = left;
    both.insert(both.end(), right.begin(), right.end());
    std::vector<Expr> kept, to_left, to_right;
    for (auto& c : f->conjuncts) {
      if (reads_only(c, left)) {
        to_left.push_back(std::move(c));
      } else if (reads_only(c, right)) {
        to_right.push_back(std::move(c));
      } else if (reads_only(c, both)) {
        j.predicate.push_back(std::move(c));
      } else {
        kept.push_back(std::move(c));  // reads no column
      }
    }
    changed = kept.size() != f->conjuncts.size();
    push_onto(j.left, std::move(to_left));
    push_onto(j.right, std::move(to_right));
    f->conjuncts = std::move(kept);
    if (f->conjuncts.empty()) {
      auto input = std::move(f->input);
      n = std::move(*input);
    }
  }
  if (auto* j = std::get_if<Join>(&n.op)) {
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
  for (const auto& c : t.columns) out.push_back({t.id, c.id, c.type, c.nullable});
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
          changed = prune(*op.input, below(cols, false)) || changed;
        } else if constexpr (std::is_same_v<T, Sort>) {
          std::vector<ColumnRef> cols;
          for (const auto& k : op.keys) columns_of(k.expr, cols);
          changed = prune(*op.input, below(cols, true)) || changed;
        } else if constexpr (std::is_same_v<T, Limit>) {
          changed = prune(*op.input, unique) || changed;
        } else if constexpr (std::is_same_v<T, Join>) {
          auto all = below(columns(op.predicate), true);
          for (auto* side : {&op.left, &op.right}) {
            std::vector<std::uint32_t> tables;
            tables_of(**side, tables);
            std::vector<ColumnRef> mine;
            for (const auto& c : all) {
              if (std::ranges::find(tables, c.table) != tables.end()) mine.push_back(c);
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
  while (fold_constants(root)) {
  }
  while (push_down(root)) {
  }
  while (prune(root, {})) {
  }
}

}  // namespace bytecask::sql::logical
