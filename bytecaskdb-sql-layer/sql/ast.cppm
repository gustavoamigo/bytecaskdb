// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ast.cppm — the small AST the parser hands the binder: the prototype's DML
// forms and the stage-1 DDL of docs/catalog_design.md. Names are as
// written; positions are byte offsets into the statement text, -1 where
// libpg_query gives none.
//
// prototype: replaced by the designed front end (#492).

module;
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module bytecask.sql.ast;

export import bytecask.sql.sqlstate;

export namespace bytecask::sql::ast {

// A failure to parse, or a form outside the prototype's scope. Carries the
// SQLSTATE PostgreSQL reports for the same fault; 42601 (syntax_error) is the
// default, for a syntax error and for a form the layer does not support yet.
class SqlError : public std::runtime_error {
public:
  SqlError(const std::string& what, std::int32_t pos, SqlState sqlstate = kSyntaxError)
      : std::runtime_error{what}, pos_{pos}, sqlstate_{sqlstate} {}
  SqlError(const SqlError&) = default;
  SqlError& operator=(const SqlError&) = default;
  ~SqlError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto pos() const noexcept -> std::int32_t { return pos_; }
  [[nodiscard]] auto sqlstate() const noexcept -> std::string_view { return sqlstate_.view(); }

private:
  std::int32_t pos_;
  SqlState sqlstate_;
};

// A statement the layer does not support where it was given, the API's
// FeatureNotSupported (api/database.cppm): SQLSTATE 0A000. Here, below the
// planner, so the planner can refuse with it.
class FeatureNotSupported : public std::logic_error {
public:
  using std::logic_error::logic_error;
  FeatureNotSupported(const FeatureNotSupported&) = default;
  FeatureNotSupported& operator=(const FeatureNotSupported&) = default;
  ~FeatureNotSupported() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// A constant, as written: a literal, or DEFAULT's operand. Numbers keep
// their text, so the binder folds them exactly to the type they meet.
struct Literal {
  enum class Kind { Null, Integer, Number, String, Bool };
  Kind kind{Kind::Null};
  std::string text;  // Integer and Number: the digits as written; String: the bytes
  bool b{false};     // Bool
  std::int32_t pos{-1};
};

struct Select;

// A comparison as a value (SELECT 3 < 5): its operator, <> among them. A
// WHERE's comparisons are Compare's, below.
enum class ValueCompare { Eq, Ne, Lt, Le, Gt, Ge };

// A select-list, SET or comparison operand: a column, a parameter, a
// literal (an integer within BIGINT after + or - is a Const), a sum,
// difference, product or quotient of two, unary + or - of one, a comparison of two, a cast of
// one (DATE '…' and '…'::date alike), an aggregate: COUNT(*),
// COUNT([DISTINCT] column), SUM, MIN, MAX or AVG of a column; or a scalar
// subquery, (SELECT …), the value of its one row; CASE (searched, or simple
// on `lhs`), COALESCE and NULLIF; a string function or ||; or, in a select list only,
// * or `table`.*, which the binder expands to columns.
struct Expr {
  enum class Kind { Column, Param, Const, Literal, Add, Sub, Mul, Div, Star, Plus, Neg, Compare, Cast, Aggregate, Subquery, Case, Coalesce, NullIf, Func };
  enum class Fn { Count, Sum, Min, Max, Avg };
  Kind kind;
  std::string column;       // Column; Aggregate's argument, "" for COUNT(*)
  std::uint32_t param{0};   // Param: zero-based ($1 is 0)
  std::unique_ptr<Expr> lhs, rhs;  // Add, Sub, Mul, Div, Compare; Plus, Neg and Cast: rhs
  std::int32_t pos{-1};
  std::string table;          // Column, Aggregate, Star: the table it is qualified by, "" for none
  std::int64_t constant{0};   // Const: an integer literal (Add and Sub's right operand only)
  Fn fn{Fn::Count};           // Aggregate
  bool distinct{false};       // Aggregate: COUNT(DISTINCT column)
  Literal literal{};          // Literal
  ValueCompare cmp{ValueCompare::Eq};  // Compare
  std::string cast_type;               // Cast: libpg_query's last type name, as a column's
  std::vector<std::int32_t> cast_typmods;
  std::shared_ptr<const Select> subquery;  // Subquery
  std::string alias;  // a select-list item's AS name, "" for none
  // Case: WHEN, THEN, WHEN, THEN … in order, `lhs` the simple form's operand
  // and `rhs` its ELSE, each none where absent. Coalesce: its arguments.
  // NullIf: `lhs` and `rhs`.
  std::vector<Expr> args;
  // Func: a string function or ||, `func` its name as PostgreSQL resolves it
  // (btrim for TRIM(BOTH …), position(string, substring) for POSITION(sub IN
  // string)), its operands in `args`.
  std::string func;
};

// One conjunct of a WHERE clause: two expressions compared; or, as a
// predicate, AND, OR or NOT of conjuncts (`args`), `lhs` IS NULL, `lhs`
// LIKE the pattern `rhs`, with its ESCAPE as written, EXISTS (`subquery`),
// or `lhs` compared by `quantified` with ANY or ALL (`all`) of the
// subquery's rows: IN is = ANY, NOT IN its NOT. The
// parser writes <>, BETWEEN and IN with them: NOT (a = b); a >= lo AND a <=
// hi; a = x OR a = y.
struct Compare {
  enum class Op { Eq, Lt, Le, Gt, Ge };
  enum class Kind { Compare, And, Or, Not, IsNull, Like, Exists, Quantified };
  Op op;
  Expr lhs, rhs;
  std::int32_t pos{-1};
  bool on{false};  // a JOIN's ON conjunct, not the WHERE's
  Kind kind{Kind::Compare};
  std::vector<Compare> args;  // And, Or: two or more; Not: one
  std::optional<std::string> escape;  // Like: ESCAPE's string; none for the default, a backslash
  std::shared_ptr<const Select> subquery;  // Exists, Quantified
  ValueCompare quantified{ValueCompare::Eq};
  bool all{false};
};

// A copy of e, which owns its operands: what BETWEEN and IN repeat.
[[nodiscard]] auto clone(const Expr& e) -> Expr {
  Expr out{e.kind, e.column, e.param, nullptr, nullptr, e.pos};
  if (e.lhs) out.lhs = std::make_unique<Expr>(clone(*e.lhs));
  if (e.rhs) out.rhs = std::make_unique<Expr>(clone(*e.rhs));
  out.table = e.table;
  out.constant = e.constant;
  out.fn = e.fn;
  out.distinct = e.distinct;
  out.literal = e.literal;
  out.cmp = e.cmp;
  out.cast_type = e.cast_type;
  out.cast_typmods = e.cast_typmods;
  out.subquery = e.subquery;  // never changed once parsed: shared
  for (const auto& a : e.args) out.args.push_back(clone(a));
  out.func = e.func;
  return out;
}

// A FOREIGN KEY constraint of CREATE TABLE, on a column (REFERENCES) or
// on the table: its name, "" for none, its columns, and the table and
// columns it references, none for that table's primary key. Checked
// against the referenced table's keys, then not enforced.
struct ForeignKeyAst {
  std::string name;
  std::vector<std::string> columns;
  std::string table;
  std::vector<std::string> ref_columns;
  std::int32_t pos{-1};
};

struct ColumnDefAst {
  std::string name;
  std::string type;                    // libpg_query's last type name: int8, varchar, numeric, …
  std::vector<std::int32_t> typmods;   // VARCHAR(n): {n}; NUMERIC(p, s): {p, s} or {p}
  bool not_null{false};
  bool primary_key{false};
  std::optional<Literal> default_value;
  std::int32_t pos{-1};
  // Each UNIQUE constraint on the column: its name, "" for none.
  std::vector<std::string> unique;
  std::vector<ForeignKeyAst> references;  // each REFERENCES on the column
};

// A UNIQUE constraint of CREATE TABLE, on a column or on the table, in the
// order written: its name ("" for one the catalog chooses) and columns.
struct UniqueAst {
  std::string name;
  std::vector<std::string> columns;
  std::int32_t pos{-1};
};

struct CreateTable {
  std::string table;
  std::vector<ColumnDefAst> columns;
  std::vector<std::string> pk;  // a table-level PRIMARY KEY (…)
  std::int32_t pos{-1};
  std::vector<UniqueAst> unique;  // every UNIQUE constraint, columns' and the table's, in order
  std::vector<ForeignKeyAst> foreign_keys;  // every FOREIGN KEY, columns' and the table's, in order
};

struct IndexColumnAst {
  std::string name;
  bool desc{false};
};

struct CreateIndex {
  std::string index;
  std::string table;
  std::vector<IndexColumnAst> columns;
  bool unique{false};
  std::int32_t pos{-1};
};

struct DropTable {
  std::string table;
  bool if_exists{false};
  std::int32_t pos{-1};
};

struct DropIndex {
  std::string index;
  bool if_exists{false};
  std::int32_t pos{-1};
};

// ALTER TABLE … ADD COLUMN, the one ALTER TABLE form in scope.
struct AddColumn {
  std::string table;
  ColumnDefAst column;
  std::int32_t pos{-1};
};

struct Insert {
  std::string table;
  std::vector<std::string> columns;  // empty: every column, in order
  std::vector<Expr> values;
  std::int32_t pos{-1};
};

struct OrderItem {
  std::string column;
  bool desc{false};
  std::int32_t pos{-1};
  std::string table;  // the table it is qualified by, "" for none
  // NULLS FIRST (true) or NULLS LAST (false); nullopt for PostgreSQL's
  // default, last under ASC and first under DESC.
  std::optional<bool> nulls_first;
};

struct Select {
  std::vector<Expr> targets;
  std::string table;  // "" for none: a SELECT without FROM
  std::string join;  // a second table, "" for none: FROM a, b or a [INNER] JOIN b ON …
  std::vector<Compare> where;
  std::vector<OrderItem> order_by;
  std::optional<std::int64_t> limit;  // nullopt: none, LIMIT ALL or LIMIT NULL
  std::int32_t pos{-1};
  bool for_update{false};  // FOR UPDATE: a guard on each row returned, never a lock
  std::int32_t table_pos{-1}, join_pos{-1};  // where table and join are named
  std::vector<Expr> group_by;  // GROUP BY's columns, as written
  std::vector<Compare> having;  // HAVING's conjuncts
  bool distinct{false};         // SELECT DISTINCT, not DISTINCT ON
  // An inner join's USING (…), or NATURAL: the columns both tables have,
  // compared by =, each named alone as one column. using_alias names them
  // too: USING (…) AS alias.
  std::vector<std::string> using_columns;
  bool natural{false};
  std::string using_alias;
  // The join's kind: LEFT keeps every row of `table`, RIGHT every row of
  // `join`, each joined to NULLs where no row of the other matches.
  enum class JoinKind { Inner, Left, Right };
  JoinKind join_kind{JoinKind::Inner};
};

struct Assignment {
  std::string column;
  Expr value;
  std::int32_t pos{-1};
};

struct Update {
  std::string table;
  std::vector<Assignment> set;
  std::vector<Compare> where;
  std::int32_t pos{-1};
};

struct Delete {
  std::string table;
  std::vector<Compare> where;
  std::int32_t pos{-1};
};

using Statement =
    std::variant<CreateTable, CreateIndex, DropTable, DropIndex, AddColumn, Insert, Select, Update, Delete>;

}  // namespace bytecask::sql::ast

namespace bytecask::sql::ast {

SqlError::~SqlError() = default;
FeatureNotSupported::~FeatureNotSupported() = default;

}  // namespace bytecask::sql::ast
