// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// database.cppm — the embedded SQL API: open a directory, prepare a statement,
// execute it with parameters, read the rows. Every DML statement is compiled
// once to a plan, by TinyCC, and cached by its text; a plan compiled against
// an older catalog is recompiled from that text before it runs.
//
// Autocommit only: each execute is one plan through bc_execute, retried on a
// conflict. One thread at a time.
//
// prototype: the shape of #494's embedded API, not its design. Definitions
// are in database.cpp.

module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

export module bytecask.sql.api;

export import bytecask.sql.runtime;
export import bytecask.sql.codegen;
export import bytecask.sql.tcc_loader;

export namespace bytecask::sql {

using Value = bc_value;  // NULL, BIGINT, DOUBLE PRECISION, BOOLEAN, BYTEA

struct Rows {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  std::int64_t affected{-1};  // INSERT, UPDATE, DELETE: rows changed
};

// A plan that ran and failed: the plan API's error code and message (a
// duplicate key is BC_E_CONSTRAINT, raised by the plan), or BC_CONFLICT when
// every retry conflicted.
class ExecError : public std::runtime_error {
public:
  ExecError(const std::string& what, int code) : std::runtime_error{what}, code_{code} {}
  ExecError(const ExecError&) = default;
  ExecError& operator=(const ExecError&) = default;
  ~ExecError() override;
  [[nodiscard]] auto code() const noexcept -> int { return code_; }

private:
  int code_;
};

class Statement {
public:
  [[nodiscard]] auto sql() const -> const std::string& { return sql_; }
  [[nodiscard]] auto is_ddl() const -> bool { return !plan_; }
  [[nodiscard]] auto param_types() const -> const std::vector<PhysType>&;
  // The plan file, as EXPLAIN would print it; empty for DDL.
  [[nodiscard]] auto plan_source() const -> const std::string&;

private:
  friend class Database;
  std::string sql_;
  ast::Statement ast_;
  std::unique_ptr<GeneratedPlan> plan_;  // null for DDL
  std::unique_ptr<TccPlan> code_;
  std::vector<std::string> columns_;
  std::vector<std::uint8_t> result_codes_;  // the plan's result_types as bc_type codes
  RowLayout result_layout_;                 // how its result rows decode, built once per compile
};

class Database {
public:
  [[nodiscard]] static auto open(const std::filesystem::path& dir) -> std::unique_ptr<Database>;
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Parses, binds and (for DML) plans and compiles; cached by the exact text.
  // Throws ast::SqlError. The reference is valid until close().
  auto prepare(std::string_view sql) -> const Statement&;
  // Throws std::invalid_argument for parameters of the wrong number or type,
  // ast::SqlError if a recompile fails, ExecError if the plan fails.
  auto execute(const Statement& stmt, const std::vector<Value>& params) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}) -> Rows {
    return execute(prepare(sql), params);
  }
  [[nodiscard]] auto cached_plans() const -> std::size_t { return cache_.size(); }

  // Closes the engine; every later call is undefined.
  void close();

private:
  Database() = default;
  void compile(Statement& s);

  std::unique_ptr<DB> db_;
  Catalog cat_;
  bc_db bdb_{};
  bc_catalog bcat_{};
  bc_ctx* cx_{nullptr};
  std::map<std::string, std::unique_ptr<Statement>, std::less<>> cache_;
};

}  // namespace bytecask::sql
