// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// database.cpp — the embedded API's definitions; see database.cppm.

module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

module bytecask.sql.api;

import bytecask.sql.parser;

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kArenaBytes = 4u << 20;
constexpr std::uint32_t kMaxRetries = 3;

auto decode_row(const RowLayout& layout, BytesView row) -> std::vector<Value> {
  std::vector<Value> out;
  for (const auto& c : layout.columns) {
    if (row_is_null(row, c)) {
      out.emplace_back(std::monostate{});
      continue;
    }
    switch (c.type) {
      case PhysType::I64: out.emplace_back(row_i64(row, c.pos).value_or(0)); break;
      case PhysType::F64: out.emplace_back(row_f64(row, c.pos).value_or(0.0)); break;
      case PhysType::Bool: out.emplace_back(row_bool(row, c.pos).value_or(false)); break;
      case PhysType::Bytes: {
        auto v = row_bytes(row, c.pos).value_or(BytesView{});
        out.emplace_back(Bytes(v.begin(), v.end()));
        break;
      }
    }
  }
  return out;
}

auto matches(const Value& v, PhysType t) -> bool {
  if (std::holds_alternative<std::monostate>(v)) return true;
  switch (t) {
    case PhysType::I64: return std::holds_alternative<std::int64_t>(v);
    case PhysType::F64: return std::holds_alternative<double>(v);
    case PhysType::Bool: return std::holds_alternative<bool>(v);
    case PhysType::Bytes: return std::holds_alternative<Bytes>(v);
  }
  return false;
}

auto constraint_message(std::uint32_t detail) -> std::string {
  auto object = std::to_string(detail & 0x00FF'FFFFu);
  switch (detail >> 24) {
    case BC_C_UNIQUE: return "duplicate key value violates unique constraint of object " + object;
    case BC_C_NOT_NULL: return "null value violates not-null constraint of column " + object;
    case BC_C_RANGE: return "value out of range for column " + object;
    default: return "constraint violated on object " + object;
  }
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
  s.result_codes_.clear();
  for (auto t : s.plan_->result_types) s.result_codes_.push_back(static_cast<std::uint8_t>(t));
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
                    static_cast<std::uint32_t>(p.tables_written.size()), stmt.result_codes_.data(),
                    static_cast<std::uint32_t>(stmt.result_codes_.size())};
  auto r = bc_execute(cx_, stmt.code_->fn(), &ref, &bp, {}, true, kMaxRetries);
  if (r == BC_CONFLICT) throw ExecError("conflict: a guard failed on every attempt", BC_CONFLICT);
  if (r == BC_FAILED && bc_error(cx_) == BC_E_CONSTRAINT) {
    throw ExecError(constraint_message(bc_error_detail(cx_)), BC_E_CONSTRAINT);
  }
  if (r == BC_FAILED) throw ExecError(bc_error_message(cx_), bc_error(cx_));

  out.affected = bc_ctx_affected(cx_);
  if (p.returns_rows) {
    out.columns = stmt.columns_;
    auto layout = make_result_layout(p.result_types);
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx_, &row, &len)) out.rows.push_back(decode_row(layout, std::as_bytes(std::span{row, len})));
  }
  bc_ctx_reset(cx_);
  return out;
}

}  // namespace bytecask::sql
