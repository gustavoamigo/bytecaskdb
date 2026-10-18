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
// prototype: the shape of #494's embedded API, not its design.

module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <span>
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
import bytecask.sql.parser;

export namespace bytecask::sql {

using Value = bc_value;  // NULL, BIGINT, double (unused), BYTEA

struct Rows {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  std::int64_t affected{-1};  // INSERT, UPDATE, DELETE: rows changed
};

// A plan that ran and failed: the plan API's error code and message, or
// BC_CONFLICT when every retry conflicted (for INSERT, usually a duplicate
// primary key: the API cannot tell the two apart).
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
  std::vector<std::uint32_t> tables_read_, tables_written_;
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

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kArenaBytes = 4u << 20;
constexpr std::uint32_t kMaxRetries = 3;

auto decode_row(const RowLayout& layout, BytesView row) -> std::vector<Value> {
  std::vector<Value> out;
  for (const auto& c : layout.columns) {
    if (row_is_null(row, c)) {
      out.emplace_back(std::monostate{});
    } else if (c.type == PhysType::I64) {
      out.emplace_back(row_i64(row, c.pos).value_or(0));
    } else {
      auto v = row_bytes(row, c.pos).value_or(BytesView{});
      out.emplace_back(Bytes(v.begin(), v.end()));
    }
  }
  return out;
}

auto matches(const Value& v, PhysType t) -> bool {
  if (std::holds_alternative<std::monostate>(v)) return true;
  return t == PhysType::I64 ? std::holds_alternative<std::int64_t>(v) : std::holds_alternative<Bytes>(v);
}

}  // namespace

ExecError::~ExecError() = default;

auto Statement::param_types() const -> const std::vector<PhysType>& {
  static const std::vector<PhysType> kNone;
  return plan_ ? plan_->param_types : kNone;
}

auto Statement::plan_source() const -> const std::string& {
  static const std::string kNone;
  return plan_ ? plan_->source : kNone;
}

auto Database::open(const std::filesystem::path& dir) -> std::unique_ptr<Database> {
  std::unique_ptr<Database> d{new Database};
  d->db_.reset(new DB(DB::open(dir)));
  d->cat_ = Catalog::load(*d->db_);
  d->bdb_ = {d->db_.get()};
  d->bcat_ = {&d->cat_};
  d->cx_ = bc_ctx_new(&d->bdb_, &d->bcat_, kArenaBytes);
  if (d->cx_ == nullptr) throw std::bad_alloc();
  return d;
}

Database::~Database() {
  try {
    close();
  } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor cannot report; call close() to see errors
  }
}

void Database::close() {
  cache_.clear();
  bc_ctx_free(cx_);
  cx_ = nullptr;
  if (db_) db_->close();
  db_.reset();
}

void Database::compile(Statement& s) {
  auto bound = bound::bind(s.ast_, cat_);
  if (std::holds_alternative<bound::CreateTable>(bound.stmt) || std::holds_alternative<bound::CreateIndex>(bound.stmt)) {
    s.plan_.reset();
    s.code_.reset();
    return;
  }
  if (const auto* sel = std::get_if<bound::Select>(&bound.stmt)) {
    s.columns_.clear();
    for (auto id : sel->targets) s.columns_.push_back(sel->table->column(id)->name);
  }
  s.plan_ = std::make_unique<GeneratedPlan>(generate_plan(bound, cat_, s.sql_));
  s.code_ = TccPlan::compile(s.plan_->source);
}

auto Database::prepare(std::string_view sql) -> const Statement& {
  if (auto it = cache_.find(sql); it != cache_.end()) return *it->second;
  auto s = std::make_unique<Statement>();
  s->sql_ = std::string{sql};
  s->ast_ = ast::parse(sql);
  compile(*s);
  return *cache_.emplace(s->sql_, std::move(s)).first->second;
}

auto Database::execute(const Statement& cstmt, const std::vector<Value>& params) -> Rows {
  // The cache owns every Statement; the const handle is the caller's view of it.
  auto& stmt = *cache_.at(cstmt.sql_);
  Rows out;
  if (stmt.is_ddl()) {
    if (!params.empty()) throw std::invalid_argument("DDL takes no parameters");
    auto bound = bound::bind(stmt.ast_, cat_);
    if (const auto* ct = std::get_if<bound::CreateTable>(&bound.stmt)) {
      (void)cat_.create_table(*db_, ct->table, ct->columns, ct->pk);
    } else {
      const auto& ci = std::get<bound::CreateIndex>(bound.stmt);
      (void)cat_.create_index(*db_, ci.index, ci.table, ci.columns);
    }
    out.affected = 0;
    return out;
  }
  // A plan compiled against an older catalog is recompiled from its text.
  if (stmt.plan_->catalog_version != cat_.version()) compile(stmt);

  const auto& types = stmt.plan_->param_types;
  if (params.size() != types.size()) {
    throw std::invalid_argument("statement takes " + std::to_string(types.size()) + " parameters, got " +
                                std::to_string(params.size()));
  }
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (!matches(params[i], types[i])) throw std::invalid_argument("parameter $" + std::to_string(i + 1) + " has the wrong type");
  }
  bc_params bp{params};
  const auto& p = *stmt.plan_;
  bc_schema_ref ref{p.catalog_version, p.tables_read.data(), p.tables_written.data(),
                    static_cast<std::uint32_t>(p.tables_read.size()),
                    static_cast<std::uint32_t>(p.tables_written.size())};
  auto r = bc_execute(cx_, stmt.code_->fn(), &ref, &bp, {}, true, kMaxRetries);
  if (r == BC_CONFLICT) throw ExecError("conflict: a guard failed on every attempt (for INSERT, a duplicate key)", BC_CONFLICT);
  if (r == BC_FAILED) throw ExecError(bc_error_message(cx_), bc_error(cx_));

  out.affected = bc_ctx_affected(cx_);
  if (p.returns_rows) {
    out.columns = stmt.columns_;
    const auto* layout = cat_.row_layout(p.result_layout);
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx_, &row, &len)) out.rows.push_back(decode_row(*layout, std::as_bytes(std::span{row, len})));
  }
  bc_ctx_reset(cx_);
  return out;
}

}  // namespace bytecask::sql
