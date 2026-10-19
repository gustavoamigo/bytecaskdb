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
#include <variant>
#include <vector>

export module bytecask.sql.ast;

export namespace bytecask::sql::ast {

// A failure to parse, or a form outside the prototype's scope.
class SqlError : public std::runtime_error {
public:
  SqlError(const std::string& what, std::int32_t pos) : std::runtime_error{what}, pos_{pos} {}
  SqlError(const SqlError&) = default;
  SqlError& operator=(const SqlError&) = default;
  ~SqlError() override;  // out of line: anchors the vtable
  [[nodiscard]] auto pos() const noexcept -> std::int32_t { return pos_; }

private:
  std::int32_t pos_;
};

struct Expr {
  enum class Kind { Column, Param, Sub };
  Kind kind;
  std::string column;       // Column
  std::uint32_t param{0};   // Param: zero-based ($1 is 0)
  std::unique_ptr<Expr> lhs, rhs;  // Sub
  std::int32_t pos{-1};
};

// One conjunct of a WHERE clause: a column compared with an expression.
struct Compare {
  enum class Op { Eq, Lt, Le, Gt, Ge };
  Op op;
  Expr lhs, rhs;
  std::int32_t pos{-1};
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

struct Select {
  std::vector<Expr> targets;
  std::string table;
  std::vector<Compare> where;
  std::int32_t pos{-1};
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
