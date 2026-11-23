// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// logical.cppm — the logical plan (docs/planner_design.md, *The logical
// plan*): a tree of relational operators over typed expressions, saying what
// to compute and not how. The binder builds it, the rewrites (rewrite.cppm)
// turn it into an equivalent one, and the search (search.cppm) picks the
// physical plan that computes it.
//
// The expressions are those the dialect has today: columns, parameters
// (a literal is one, folded by the binder), integer constants, + - * and /,
// comparisons, a conjunction of them, and aggregate calls. A predicate is its conjuncts, in the order the statement
// wrote them. A select list's expression over columns (Project's outputs) is
// a tree of these, each node typed: the one value expression the search
// plans and codegen evaluates (docs/planner_design.md, *Expressions over
// columns*).

module;
#include <cstdint>
#include <limits>
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

// A column of one of the statement's tables, named by the table's id. A table
// read twice is two tables, an instance (Table::source) for each use, so the
// id names the use. Its physical type and nullability are the catalog's, but
// for a column of a table an outer join may NULL-extend, which the binder
// marks nullable, so codegen never reads the catalog.
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
// Truth and InSet are an uncorrelated subquery's (Subquery, below): a
// BOOLEAN parameter it set, true, false or NULL as it holds; and whether a
// column's value is among a BC_SUB_SET subquery's values, `constant` its
// index in the statement's subqueries (NULL for a NULL column).
// And, Or, Not, IsNull and Like are values over operands of any value
// expression, in a filter as in a select list: a BOOLEAN, NULL where SQL's
// logic says unknown (a WHERE keeps a row only where it is true). A WHERE
// or ON conjunct is any BOOLEAN expression; one that is not a column
// compared with what a key takes (physical::column_compare) is a filter,
// never a key's bound. Case and Coalesce are values of a select list only:
// every branch is of the expression's type, or a numeric of a finer scale
// below it, which the evaluator brings up. Concat, Substr, TextFn and Extract, built
// at run time, are values of a select list (and of a procedure's
// expressions) only.
struct Expr {
  enum class Kind { Column, Param, Const, Add, Sub, Mul, Div, Compare, Aggregate, And, Or, Not, IsNull, Like, Truth, InSet, Case, Coalesce,
                    Concat, Substr, Random, Mod, Neg, Abs, TextFn, Extract, JsonbCast, Jsonb };
  enum class Fn { Count, Sum, Min, Max, Avg };
  // TextFn's function: PostgreSQL's of the same name, over UTF-8 text.
  enum class Text { Upper, Lower, CharLength, OctetLength, Btrim, Ltrim, Rtrim, Position };
  // Jsonb's function (docs/jsonb_design.md, stage 3): -> and ->> by key
  // (Field) and by index (Element), #> and #>> (Path), jsonb_typeof,
  // jsonb_array_length; PathIn and PathArray make the path #> takes, from a
  // text[]'s text or from ARRAY[…]'s text elements, and are never a value.
  enum class Jsonb { Field, FieldText, Element, ElementText, Path, PathText, Typeof, ArrayLength, PathIn, PathArray };
  Kind kind{Kind::Param};
  SqlType type{};
  ColumnRef col{};              // Column
  std::uint32_t param{0};       // Param: zero-based ($1 is 0)
  std::int64_t constant{0};     // Const: in its type's representation, a DECIMAL's scaled; a BOOLEAN's 0 or 1;
                                // Like: the escape's code point, -1 for none; InSet: its subquery;
                                // Extract: the field, a DateField (bc_plan.h's bc_date_field)
  Op op{Op::Eq};                // Compare
  Fn fn{Fn::Count};             // Aggregate
  Text text{Text::Upper};       // TextFn
  Jsonb jsonb{Jsonb::Field};    // Jsonb
  bool distinct{false};         // Aggregate: COUNT(DISTINCT arg)
  bool fold{false};             // Like: ILIKE, ASCII letters matched in either case
  std::vector<Expr> args;       // Add, Sub, Mul, Div, Compare: two; Aggregate: none (COUNT(*)) or its argument;
                                // And, Or: two or more; Not: one; IsNull: its operand; Like: the text and the
                                // pattern, any text values, and an ESCAPE parameter, which `constant` then is not; Truth: its parameter; InSet: its column;
                                // Case: WHEN, THEN, WHEN, THEN … and an ELSE last if there is one (an odd count);
                                // Coalesce: two or more; Concat: two, text or an integer or DECIMAL (its
                                // text); Substr: the text, the start, and the count if given (INTEGERs);
                                // Random: its two bounds, both included; Mod: two, of the result's type and scale;
                                // Neg, Abs: one, of the result's type; TextFn: the text, then the trim's
                                // characters if given, or POSITION's needle; Extract: one, a DATE, TIME,
                                // TIMESTAMP or TIMESTAMPTZ; JsonbCast: one, a jsonb to its text (the
                                // expression's type TEXT), a string read as jsonb (type jsonb), or a jsonb
                                // to BOOLEAN, an integer, REAL, DOUBLE PRECISION or NUMERIC;
                                // Jsonb: the jsonb, then the key (text), the index (an integer) or the path
                                // (a PathIn of text or a PathArray of text elements, any NULL); Typeof and
                                // ArrayLength: the jsonb alone

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

// Whether e is a predicate: And, Or, Not, IsNull, Like, Truth or InSet.
[[nodiscard]] auto is_predicate(const Expr& e) -> bool {
  return e.kind == Expr::Kind::And || e.kind == Expr::Kind::Or || e.kind == Expr::Kind::Not ||
         e.kind == Expr::Kind::IsNull || e.kind == Expr::Kind::Like || e.kind == Expr::Kind::Truth ||
         e.kind == Expr::Kind::InSet;
}

// A predicate of two or more conjuncts or disjuncts, or one alone.
[[nodiscard]] auto predicate_of(Expr::Kind kind, std::vector<Expr> args) -> Expr {
  auto e = expr(kind, SqlType{SqlKind::Boolean});
  e.args = std::move(args);
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

// The rows of a grouped view's query, read as a relation of its own
// (docs/planner_design.md, *Derived relations*): `plan` is that query's
// logical plan, an Aggregate under any HAVING Filter, and `table` the
// binder's derived table (Table::derived), whose i-th column is the query's
// i-th output. A leaf of the plan around it: nothing above reads into
// `plan`, and the rewrites rewrite it alone.
struct Derived {
  NodePtr plan;
  const Table* table{nullptr};
};

// No rows: what constant folding leaves of an input a conjunct that is
// always false filters. It names the tables it replaced.
struct Empty {
  std::vector<const Table*> tables;
};

// One row of no columns: what a SELECT without FROM reads.
struct OneRow {};

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
// true. Left outer: those, and each row of `left` none pairs with, once,
// with `right`'s columns NULL; the predicate is its ON, which decides the
// pairs, never which rows of `left` there are. A join of several tables is
// left-deep: `left` is the join of the tables before, `right` one table.
struct Join {
  NodePtr left, right;
  std::vector<Expr> predicate;
  bool left_outer{false};
};

// One row per group of the input's rows whose `group_by` columns are equal,
// NULL equal to NULL, of `aggs`: aggregate calls and grouped columns, in the
// select list's order. Not `grouped` (no GROUP BY): one row over all the
// input's rows, none of `aggs` a column. A HAVING is a Filter above it.
// `dependent` holds the columns selected, ordered by or compared in HAVING
// that are not grouped but whose table's whole primary key is: equal in
// every row of a group (PostgreSQL's functional dependency), so read from
// any one, never grouped by.
struct Aggregate {
  NodePtr input;
  std::vector<Output> aggs;
  std::vector<Expr> group_by;  // columns, each once, as the GROUP BY writes them
  bool grouped{false};
  std::vector<Expr> dependent;  // columns, each once, none of group_by
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

// Every row: a Limit of an OFFSET without a LIMIT.
inline constexpr std::int64_t kAllRows = std::numeric_limits<std::int64_t>::max();

// The first n rows, less the first `offset` of them (LIMIT l OFFSET o is n =
// o + l, offset o): n bounds what the rows below must give, and only what
// emits them drops the offset's.
struct Limit {
  NodePtr input;
  std::int64_t n{0};
  std::int64_t offset{0};
};

// One row of VALUES: per table column, in column order, its value, or
// nullopt for the default bc_row_begin writes.
//
// `checks` are the table's CHECK constraints over the row as written, each
// a BOOLEAN over the table's columns and constants: the row is refused
// (23514) where one is false, and accepted where it is true or NULL.
//
// `arbiter` is ON CONFLICT's: the table's id (its primary key), a unique
// index's, or kAnyArbiter. With DO NOTHING a row whose key under it is
// taken is skipped (bc_insert_on_conflict). With DO UPDATE, `update` holds
// a SET list per row of `rows`, as Update's `set`, over the row that holds
// the key, which is updated in the proposed row's place
// (bc_insert_or_existing); EXCLUDED.col is that row's value for col.
inline constexpr std::uint32_t kAnyArbiter = 0xFFFFFFFF;  // BC_ARBITER_ANY

// A WHERE conjunct of a view WITH CHECK OPTION the row is written through,
// a BOOLEAN over the table's columns and constants: the row is refused
// (44000, BC_C_CHECK_OPTION with the view's id) where it is false or NULL,
// once it is written and before its RETURNING row. Innermost view first.
struct ViewCheck {
  std::uint32_t view{0};
  Expr where;
};

struct Insert {
  const Table* table{nullptr};
  std::vector<std::vector<std::optional<Expr>>> rows;  // VALUES' rows, written in order
  std::vector<Expr> checks;
  std::vector<Output> returning;  // RETURNING's: a row of these per row written, over the row as written
  std::optional<std::uint32_t> arbiter;
  std::vector<std::vector<std::pair<ColumnRef, Expr>>> update;  // empty but for DO UPDATE
  std::vector<ViewCheck> view_checks;
};

// INSERT … SELECT: each row the input gives, a SELECT's rows (a Project
// under any Sort and Limit), written as one row of the table: its i-th
// column into columns[i], each of a type the binder checked it assigns to;
// `fixed` sets a column left out to a parameter (a DEFAULT of the clock),
// and the rest keep the default bc_row_begin writes. `checks`,
// `returning` and `arbiter` are as Insert's.
struct InsertFrom {
  NodePtr input;
  const Table* table{nullptr};
  std::vector<ColumnRef> columns;
  std::vector<std::pair<ColumnRef, Expr>> fixed;
  std::vector<Expr> checks;
  std::vector<Output> returning;
  std::optional<std::uint32_t> arbiter;
  std::vector<ViewCheck> view_checks;
};

// Each row the input gives, written: its set columns from expressions of
// the row as read.
struct Update {
  NodePtr input;
  const Table* table{nullptr};
  std::vector<std::pair<ColumnRef, Expr>> set;
  std::vector<Expr> checks;  // as Insert's, over the row as it will be written
  std::vector<Output> returning;  // as Insert's, over the row as updated
};

struct Delete {
  NodePtr input;
  const Table* table{nullptr};
  std::vector<Output> returning;  // as Insert's, over the row as it was
};

// UNION, INTERSECT or EXCEPT of two queries' rows, each a Project (under a
// Sort or Limit of its own) or another SetOp: the rows of both, or those in
// both, or those in `left` and not in `right`, each row once unless `all`
// keeps them (docs/planner_design.md, *Set operations*). Two rows are alike
// where each column is equal, NULL equal to NULL. `outputs` are its columns:
// the names of the left arm's, the types the arms' resolve to, each expr a
// Param of that type that stands for the column and is read by no one. Only
// the root may sit under a Limit. The root alone has an `order`: ORDER BY of
// the combined rows, by output columns only (a Sort has columns of a table).
struct SetOp {
  using Kind = ast::SetOperation::Kind;
  struct Order {
    std::size_t column;  // index into `outputs`
    bool desc;
    bool nulls_first;
  };
  NodePtr left, right;
  Kind kind{Kind::Union};
  bool all{false};
  std::vector<Output> outputs;
  // Columns after `outputs` the arms give and the statement does not
  // return: a FULL JOIN's ORDER BY keys it does not select (the binder).
  std::vector<Output> hidden;
  std::vector<Order> order;
};

struct Node {
  std::variant<Get, Empty, OneRow, Filter, Project, Join, Aggregate, Sort, Limit, SetOp, Insert, InsertFrom, Update, Delete, Derived>
      op;
  // The columns something above reads from this operator's rows: set by
  // projection pruning, empty before it.
  std::vector<ColumnRef> output;
};

template <typename T>
[[nodiscard]] auto node(T op) -> NodePtr {
  return std::make_unique<Node>(Node{std::move(op), {}});
}

// A subquery: a SELECT whose rows of one column are folded as `kind` says
// into the hidden parameters from `param` on (bc_sub_kind in bc_plan.h).
// Value: its one row's value, NULL for none, a second row 21000. Exists:
// a BOOLEAN. Bounds: its largest and smallest non-NULL values, then two
// BOOLEANs, NULL if a row was NULL else FALSE, and whether it gave a row.
// Set: its non-NULL values, which InSet tests, then those two BOOLEANs.
// Uncorrelated (no `args`), it runs once, before the statement's own plan,
// on its snapshot. Correlated, it reads columns of the outer query's one
// table, each a hidden parameter of its own (an Arg): it runs again for
// each outer row that reaches the conjunct testing it, those parameters
// set from that row first (docs/planner_design.md, *Subqueries*).
struct Subquery {
  enum class Kind { Value, Exists, Bounds, Set };
  struct Arg {
    ColumnRef col;  // the outer row's column
    std::uint32_t param{0};
  };
  NodePtr plan;
  Kind kind{Kind::Value};
  SqlType type{};  // its column's; Exists: none it reads
  std::uint32_t param{0};
  std::vector<Arg> args;
  [[nodiscard]] auto correlated() const -> bool { return !args.empty(); }
  // How many hidden parameters it sets from `param` on.
  [[nodiscard]] static auto params(Kind k) -> std::uint32_t { return k == Kind::Bounds ? 4u : k == Kind::Set ? 2u : 1u; }
};

// The operator below n, or null for a leaf (Get, Empty, OneRow, Insert,
// Derived); a Join has two.
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
