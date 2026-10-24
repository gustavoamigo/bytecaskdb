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

export namespace bytecask::sql::ast {

// A failure to parse, or a form outside the prototype's scope. Carries the
// SQLSTATE PostgreSQL reports for the same fault; 42601 (syntax_error) is the
// default, for a syntax error and for a form the layer does not support yet.
class SqlError : public std::runtime_error {
public:
  // `sqlstate` is a literal: five characters, kept for the exception's life.
  SqlError(const std::string& what, std::int32_t pos, const char* sqlstate = "42601")
      : std::runtime_error{what}, pos_{pos}, sqlstate_{sqlstate} {}
  SqlError(const SqlError&) = default;
  SqlError& operator=(const SqlError&) = default;
  ~SqlError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto pos() const noexcept -> std::int32_t { return pos_; }
  [[nodiscard]] auto sqlstate() const noexcept -> std::string_view { return sqlstate_; }

private:
  std::int32_t pos_;
  const char* sqlstate_;
};

// A select-list, SET or comparison operand: a column, a parameter, an
// integer literal, a sum or difference of two, or an aggregate: COUNT(*),
// COUNT([DISTINCT] column), SUM(column) or MIN(column).
struct Expr {
  enum class Kind { Column, Param, Const, Add, Sub, Aggregate };
  enum class Fn { Count, Sum, Min };
  Kind kind;
  std::string column;       // Column; Aggregate's argument, "" for COUNT(*)
  std::uint32_t param{0};   // Param: zero-based ($1 is 0)
  std::unique_ptr<Expr> lhs, rhs;  // Add, Sub
  std::int32_t pos{-1};
  std::string table;          // Column, Aggregate: the table it is qualified by, "" for none
  std::int64_t constant{0};   // Const: an integer literal (Add and Sub's right operand only)
  Fn fn{Fn::Count};           // Aggregate
  bool distinct{false};       // Aggregate: COUNT(DISTINCT column)
};

// One conjunct of a WHERE clause: two expressions compared.
struct Compare {
  enum class Op { Eq, Lt, Le, Gt, Ge };
  Op op;
  Expr lhs, rhs;
  std::int32_t pos{-1};
  bool on{false};  // a JOIN's ON conjunct, not the WHERE's
};

// A constant, as written: DEFAULT's operand. Numbers keep their text, so
// the binder folds them exactly to the column's type.
struct Literal {
  enum class Kind { Null, Integer, Number, String, Bool };
  Kind kind;
  std::string text;  // Integer and Number: the digits as written; String: the bytes
  bool b{false};     // Bool
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
};

struct CreateTable {
  std::string table;
  std::vector<ColumnDefAst> columns;
  std::vector<std::string> pk;  // a table-level PRIMARY KEY (…)
  std::int32_t pos{-1};
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
};

struct Select {
  std::vector<Expr> targets;
  std::string table;
  std::string join;  // a second table, "" for none: FROM a, b or a [INNER] JOIN b ON …
  std::vector<Compare> where;
  std::vector<OrderItem> order_by;
  std::optional<std::int64_t> limit;  // nullopt: none, LIMIT ALL or LIMIT NULL
  std::int32_t pos{-1};
  bool for_update{false};  // FOR UPDATE: a guard on each row returned, never a lock
  std::int32_t table_pos{-1}, join_pos{-1};  // where table and join are named
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

}  // namespace bytecask::sql::ast
