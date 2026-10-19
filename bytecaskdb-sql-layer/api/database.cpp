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
  d->live_ = std::make_unique<LiveCatalog>(*d->db_);
  d->bdb_ = {d->db_.get()};
  d->bcat_ = {d->live_.get()};
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
  live_.reset();
  if (db_) db_->close();
  db_.reset();
}

void Database::compile(Statement& s) {
  auto cat = live_->current();
  auto bound = bound::bind(s.ast_, *cat);
  if (std::holds_alternative<Ddl>(bound.stmt)) {
    s.ref_ = {};
    s.plan_.reset();
    s.code_.reset();
    return;
  }
  // Everything is built first and the statement changed only once nothing
  // can throw: ref_ points into plan_ and the vectors, and a statement that
  // fails to recompile keeps its old, consistent plan.
  std::vector<std::string> columns;
  if (const auto* sel = std::get_if<bound::Select>(&bound.stmt)) {
    for (auto id : sel->targets) columns.push_back(sel->table->column(id)->name);
  }
  auto plan = std::make_unique<GeneratedPlan>(generate_plan(bound, *cat, s.sql_));
  auto codes = result_type_codes(plan->result_types);
  auto layout = make_result_layout(plan->result_types);
  auto code = TccPlan::compile(plan->source);
  std::vector<bc_col_ref> col_refs;
  for (const auto& c : plan->columns) col_refs.push_back({c.table, c.col, static_cast<std::uint8_t>(c.type)});
  auto n = [](const auto& v) { return narrow<std::uint32_t>(v.size()); };
  const auto& p = *plan;
  bc_schema_ref ref{p.id,
                    p.catalog_version,
                    p.objects_read.data(),
                    n(p.objects_read),
                    p.tables_written.data(),
                    n(p.tables_written),
                    col_refs.data(),
                    n(col_refs),
                    codes.data(),
                    n(codes)};
  // Moves keep every buffer ref points into.
  if (std::holds_alternative<bound::Select>(bound.stmt)) s.columns_ = std::move(columns);
  s.plan_ = std::move(plan);
  s.code_ = std::move(code);
  s.result_codes_ = std::move(codes);
  s.result_layout_ = std::move(layout);
  s.col_refs_ = std::move(col_refs);
  s.ref_ = ref;
  ++compilations_;
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
    auto bound = bound::bind(stmt.ast_, *live_->current());
    (void)live_->apply(std::get<Ddl>(bound.stmt));
    out.affected = 0;
    return out;
  }
  // Throws std::invalid_argument for parameters the statement's current plan
  // does not take: checked again after a recompile, whose plan may differ.
  auto check_params = [&] {
    const auto& types = stmt.plan_->param_types;
    if (params.size() != types.size()) {
      throw std::invalid_argument("statement takes " + std::to_string(types.size()) + " parameters, got " +
                                  std::to_string(params.size()));
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (!matches(params[i], types[i])) {
        throw std::invalid_argument("parameter $" + std::to_string(i + 1) + " has the wrong type");
      }
    }
  };
  check_params();
  bc_params bp{params};
  auto r = bc_execute(cx_, stmt.code_->fn(), &stmt.ref_, &bp, {}, true, kMaxRetries);
  // bc_ctx_run checks the plan against the catalog it pinned. A plan a DDL
  // made incompatible (BC_E_LAYOUT with BC_L_INCOMPATIBLE) is recompiled
  // from its text and run once more; one a DDL left compatible keeps
  // running, unchecked here.
  if (r == BC_FAILED && bc_error(cx_) == BC_E_LAYOUT && bc_error_detail(cx_) == BC_L_INCOMPATIBLE) {
    bc_ctx_reset(cx_);
    compile(stmt);
    check_params();
    r = bc_execute(cx_, stmt.code_->fn(), &stmt.ref_, &bp, {}, true, kMaxRetries);
  }
  const auto& p = *stmt.plan_;
  if (r == BC_CONFLICT) {
    bc_ctx_reset(cx_);
    throw ExecError("conflict: a guard failed on every attempt", BC_CONFLICT);
  }
  if (r == BC_FAILED) {
    ExecError e{bc_error_message(cx_), bc_error(cx_)};
    bc_ctx_reset(cx_);  // the context keeps no snapshot or catalog version while idle
    throw e;
  }

  out.affected = bc_ctx_affected(cx_);
  if (p.returns_rows) {
    out.columns = stmt.columns_;
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx_, &row, &len)) {
      out.rows.push_back(decode_row(stmt.result_layout_, std::as_bytes(std::span{row, len})));
    }
  }
  bc_ctx_reset(cx_);
  return out;
}

}  // namespace bytecask::sql
