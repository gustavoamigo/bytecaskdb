// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// logical.cppm — the logical plan (docs/planner_design.md, *The logical
// plan*): a tree of relational operators over typed expressions, saying what
// to compute and not how. The binder builds it, the rewrites (rewrite.cppm)
// turn it into an equivalent one, and the search (search.cppm) picks the
// physical plan that computes it.
//
// The expressions are those the dialect has today: columns, parameters,
// integer constants, + and -, comparisons, a conjunction of them, and
// aggregate calls. A predicate is its conjuncts, in the order the statement
// wrote them.

module;
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.logical;

export import bytecask.sql.ast;
export import bytecask.sql.catalog;

export namespace bytecask::sql::logical {

using Op = ast::Compare::Op;

// A column of one of the statement's tables. A self-join is refused, so the
// table names the operand. Its physical type and nullability are the
// catalog's, so codegen never reads the catalog.
struct ColumnRef {
  std::uint32_t table{0};
  std::uint32_t col{0};
  PhysType type{PhysType::I64};
  bool nullable{false};

  friend auto operator==(const ColumnRef& a, const ColumnRef& b) -> bool {
    return a.table == b.table && a.col == b.col;
  }
};

// A typed expression. Its SQL type is the binder's, by types_design.md's
// rules: a comparison's is BOOLEAN, an aggregate's PostgreSQL's result type.
struct Expr {
  enum class Kind { Column, Param, Const, Add, Sub, Compare, Aggregate };
  enum class Fn { Count, Sum, Min };
  Kind kind{Kind::Param};
  SqlType type{};
  ColumnRef col{};              // Column
  std::uint32_t param{0};       // Param: zero-based ($1 is 0)
  std::int64_t constant{0};     // Const: in its type's representation, a DECIMAL's scaled; a BOOLEAN's 0 or 1
  Op op{Op::Eq};                // Compare
  Fn fn{Fn::Count};             // Aggregate
  bool distinct{false};         // Aggregate: COUNT(DISTINCT arg)
  std::vector<Expr> args;       // Add, Sub, Compare: two; Aggregate: none (COUNT(*)) or its argument

  [[nodiscard]] auto phys() const -> PhysType { return phys_type(type); }

  friend auto operator==(const Expr&, const Expr&) -> bool = default;
};

// An expression of `kind` and `type`, its operands to be set.
[[nodiscard]] auto expr(Expr::Kind kind, SqlType type) -> Expr {
  Expr e;
  e.kind = kind;
  e.type = type;
  return e;
}

[[nodiscard]] auto column(ColumnRef c, SqlType type) -> Expr {
  auto e = expr(Expr::Kind::Column, type);
  e.col = c;
  return e;
}

// Column `c` of table `table`, as the catalog declares it.
[[nodiscard]] auto column_of(std::uint32_t table, const Column& c) -> Expr {
  return column({table, c.id, c.type, c.nullable}, c.sql);
}

[[nodiscard]] auto param(std::uint32_t n, SqlType type) -> Expr {
  auto e = expr(Expr::Kind::Param, type);
  e.param = n;
  return e;
}

[[nodiscard]] auto constant(std::int64_t v, SqlType type) -> Expr {
  auto e = expr(Expr::Kind::Const, type);
  e.constant = v;
  return e;
}

[[nodiscard]] auto binary(Expr::Kind kind, Expr lhs, Expr rhs, SqlType type) -> Expr {
  auto e = expr(kind, type);
  e.args.push_back(std::move(lhs));
  e.args.push_back(std::move(rhs));
  return e;
}

[[nodiscard]] auto compare(Op op, Expr lhs, Expr rhs) -> Expr {
  auto e = binary(Expr::Kind::Compare, std::move(lhs), std::move(rhs), SqlType{SqlKind::Boolean});
  e.op = op;
  return e;
}

// Every column the expression reads, in the order it reads them.
void columns_of(const Expr& e, std::vector<ColumnRef>& out) {
  if (e.kind == Expr::Kind::Column) {
    out.push_back(e.col);
    return;
  }
  for (const auto& a : e.args) columns_of(a, out);
}

struct Node;
using NodePtr = std::unique_ptr<Node>;

// Every row of a table. With FOR UPDATE, each row the statement returns is
// guarded unchanged (transaction_design.md, *Isolation*).
struct Get {
  const Table* table{nullptr};
  bool for_update{false};
};

// No rows: what constant folding leaves of an input a conjunct that is
// always false filters. It names the tables it replaced.
struct Empty {
  std::vector<const Table*> tables;
};

// The rows for which every conjunct is true.
struct Filter {
  NodePtr input;
  std::vector<Expr> conjuncts;
};

// An output column: an expression and the name PostgreSQL gives it.
struct Output {
  Expr expr;
  std::string name;
};

// One output row per input row.
struct Project {
  NodePtr input;
  std::vector<Output> exprs;
};

// The inner join: the pairs for which every conjunct of the predicate is
// true.
struct Join {
  NodePtr left, right;
  std::vector<Expr> predicate;
};

// No GROUP BY: one row over all the input's rows, of aggregate calls.
struct Aggregate {
  NodePtr input;
  std::vector<Output> aggs;
};

// A key, its direction and where its NULLs go: as NULLS FIRST or NULLS
// LAST says, or where PostgreSQL's default puts them for the direction,
// last for ASC and first for DESC.
struct SortKey {
  Expr expr;
  bool desc{false};
  bool nulls_first{false};
};

struct Sort {
  NodePtr input;
  std::vector<SortKey> keys;
};

// The first n rows.
struct Limit {
  NodePtr input;
  std::int64_t n{0};
};

// One row of VALUES: per table column, in column order, its value, or
// nullopt for the default bc_row_begin writes.
struct Insert {
  const Table* table{nullptr};
  std::vector<std::optional<Expr>> values;
};

// Each row the input gives, written: its set columns from expressions of
// the row as read.
struct Update {
  NodePtr input;
  const Table* table{nullptr};
  std::vector<std::pair<ColumnRef, Expr>> set;
};

struct Delete {
  NodePtr input;
  const Table* table{nullptr};
};

struct Node {
  std::variant<Get, Empty, Filter, Project, Join, Aggregate, Sort, Limit, Insert, Update, Delete> op;
  // The columns something above reads from this operator's rows: set by
  // projection pruning, empty before it.
  std::vector<ColumnRef> output;
};

template <typename T>
[[nodiscard]] auto node(T op) -> NodePtr {
  return std::make_unique<Node>(Node{std::move(op), {}});
}

// The operator below n, or null for a leaf (Get, Empty, Insert); a Join has
// two.
template <typename N>
  requires std::is_same_v<std::remove_const_t<N>, Node>
[[nodiscard]] auto input_of(N& n) {
  using Ptr = std::conditional_t<std::is_const_v<N>, const NodePtr*, NodePtr*>;
  return std::visit(
      [](auto& op) -> Ptr {
        if constexpr (requires { op.input; }) {
          return &op.input;
        } else {
          return nullptr;
        }
      },
      n.op);
}

}  // namespace bytecask::sql::logical
