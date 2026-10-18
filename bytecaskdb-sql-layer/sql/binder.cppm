// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// binder.cppm — resolves an AST against the catalog: table and column names to
// ids, each parameter's type from where it is used, column types and DEFAULT
// constants folded to the catalog's.
// What it returns names no column by name; the planner reads offsets and
// slots from the catalog through the column ids.
//
// prototype: the seven forms only; replaced by the designed binder (#492).
// Definitions are in binder.cpp.

module;
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.binder;

export import bytecask.sql.ast;
export import bytecask.sql.catalog;

export namespace bytecask::sql::bound {

// A column, a parameter, or column - parameter (the only arithmetic in scope).
struct Expr {
  enum class Kind { Column, Param, Sub };
  Kind kind;
  std::uint32_t col_id{0};  // Column, and the left of Sub
  std::uint32_t param{0};   // Param, and the right of Sub
  PhysType type;
};

// column <op> parameter, normalised so the column is on the left.
struct Compare {
  ast::Compare::Op op;
  std::uint32_t col_id;
  std::uint32_t param;
};

struct Insert {
  const Table* table;
  // One per table column, in column order; nullopt: the column is not given
  // and takes its default, which bc_row_begin writes.
  std::vector<std::optional<Expr>> values;
};

struct Select {
  const Table* table;
  std::vector<std::uint32_t> targets;  // column ids, in select-list order
  std::vector<Compare> where;
};

struct Update {
  const Table* table;
  std::vector<std::pair<std::uint32_t, Expr>> set;  // column id, new value
  std::vector<Compare> where;
};

struct Delete {
  const Table* table;
  std::vector<Compare> where;
};

// DDL binds to the catalog's statement values: names checked, types and
// defaults folded.
using Statement = std::variant<Ddl, Insert, Select, Update, Delete>;

struct Bound {
  Statement stmt;
  std::vector<PhysType> param_types;  // $1 is [0]
};

// Throws ast::SqlError with a message and the position of the offending node.
[[nodiscard]] auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound;

}  // namespace bytecask::sql::bound
