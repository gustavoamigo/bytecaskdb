// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// database.cppm — the embedded SQL API: open a directory, prepare a statement,
// execute it with parameters, read the rows. Every DML statement is compiled
// once to a plan, by TinyCC, and cached by its text; a plan the runtime
// refuses as incompatible with the catalog (docs/catalog_design.md,
// *Compatibility*) is recompiled from that text and run again.
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
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

using Value = bc_value;  // NULL, BIGINT, DOUBLE PRECISION, BOOLEAN, BYTEA

// The errors the API throws that other modules declare: the catalog's
// refusal of a DDL statement, and the engine's error for a call after
// close().
using bytecask::sql::DdlRefused;
using bytecask::DbClosed;

struct Rows {
  std::vector<std::string> columns;
  std::vector<SqlType> types;  // each column's SQL type, as the catalog declares it
  std::vector<std::vector<Value>> rows;
  std::int64_t affected{-1};  // INSERT, UPDATE, DELETE: rows changed
};

// A plan that ran and failed: the plan API's error code, its detail and
// message (a duplicate key is BC_E_CONSTRAINT with BC_C_UNIQUE in the
// detail's high byte, raised by the plan), or BC_CONFLICT when every retry
// conflicted.
class ExecError : public std::runtime_error {
public:
  ExecError(const std::string& what, int code, std::uint32_t detail = 0)
      : std::runtime_error{what}, code_{code}, detail_{detail} {}
  ExecError(const ExecError&) = default;
  ExecError& operator=(const ExecError&) = default;
  ~ExecError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto code() const noexcept -> int { return code_; }
  // bc_error_detail: for BC_E_CONSTRAINT, the bc_constraint in the high byte
  // and the object id in the low 24 bits.
  [[nodiscard]] auto detail() const noexcept -> std::uint32_t { return detail_; }
  // The detail decoded, for BC_E_CONSTRAINT; nullopt for any other code.
  [[nodiscard]] auto constraint() const noexcept -> std::optional<ConstraintDetail> {
    if (code_ != BC_E_CONSTRAINT) return std::nullopt;
    return ConstraintDetail::decode(detail_);
  }

private:
  int code_;
  std::uint32_t detail_;
};

// Parameters a statement does not take: the wrong number, or a value not of
// its parameter's type. Thrown before the plan runs.
class ParamError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
  ParamError(const ParamError&) = default;
  ParamError& operator=(const ParamError&) = default;
  ~ParamError() override;  // out of line: anchors the vtable (-Wweak-vtables)
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
  std::vector<SqlType> column_types_;
  std::vector<std::uint8_t> result_codes_;  // the plan's result_types as bc_type codes
  RowLayout result_layout_;                 // how its result rows decode, built once per compile
  std::vector<bc_col_ref> col_refs_;
  bc_schema_ref ref_{};  // points into plan_ and the vectors above
};

class Database {
public:
  [[nodiscard]] static auto open(const std::filesystem::path& dir) -> std::unique_ptr<Database>;
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // prepare, execute, catalog and table_rows throw bytecask::DbClosed once
  // close() was called, as the engine's calls do.

  // Parses, binds and (for DML) plans and compiles; cached by the exact text.
  // Throws ast::SqlError. The reference is valid until close().
  auto prepare(std::string_view sql) -> const Statement&;
  // Throws ParamError for parameters of the wrong number or type,
  // ast::SqlError if a recompile fails, ExecError if the plan fails, and for
  // DDL, what LiveCatalog::apply throws (DdlRefused for a statement the
  // catalog refuses), but a ConstraintViolation as an ExecError: a unique
  // index over duplicates is BC_E_CONSTRAINT, BC_C_UNIQUE and the table's id.
  auto execute(const Statement& stmt, const std::vector<Value>& params) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}) -> Rows {
    return execute(prepare(sql), params);
  }
  [[nodiscard]] auto cached_plans() const -> std::size_t { return cache_.size(); }
  // How many times a DML statement was compiled, first compiles included.
  [[nodiscard]] auto compilations() const -> std::uint64_t { return compilations_; }
  // The live catalog: the version DDL last published.
  [[nodiscard]] auto catalog() const -> std::shared_ptr<const Catalog> {
    check_open();
    return live_->current();
  }
#ifdef BYTECASK_SQL_TESTING
  // Test only (sql_layer_testing): every row of a table, in primary-key
  // order, read from the engine and decoded through the catalog: every
  // column, in the table's order. For the proof tests, which compare a table
  // with the oracle's; SQL has no full scan. Throws ast::SqlError for a
  // table that does not exist, as a statement naming it would.
  [[nodiscard]] auto table_rows(std::string_view table) const -> Rows;
#endif

  // Closes the engine. A second call returns at once.
  void close();
  [[nodiscard]] auto is_open() const noexcept -> bool { return !closed_; }

private:
  Database() = default;
  void check_open() const {
    if (closed_) throw DbClosed();
  }
  void compile(Statement& s);

  std::unique_ptr<DB> db_;
  std::unique_ptr<LiveCatalog> live_;
  bc_db bdb_{};
  bc_catalog bcat_{};
  bc_ctx* cx_{nullptr};
  std::map<std::string, std::unique_ptr<Statement>, std::less<>> cache_;
  std::uint64_t compilations_{0};
  bool closed_{false};
};

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kArenaBytes = 4u << 20;
constexpr std::uint32_t kMaxRetries = 3;

}  // namespace

ExecError::~ExecError() = default;
ParamError::~ParamError() = default;

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
  if (closed_) return;
  closed_ = true;
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
  std::vector<SqlType> column_types;
  if (const auto* sel = std::get_if<bound::Select>(&bound.stmt)) {
    for (auto id : sel->targets) {
      columns.push_back(sel->table->column(id)->name);
      column_types.push_back(sel->table->column(id)->sql);
    }
  }
  auto plan = std::make_unique<GeneratedPlan>(generate_plan(bound, *cat, s.sql_));
  auto codes = result_type_codes(plan->result_types);
  auto layout = make_result_layout(plan->result_types);
  auto code = TccPlan::compile(plan->source);
  std::vector<bc_col_ref> col_refs;
  for (const auto& c : plan->columns) col_refs.push_back({c.table, c.col, static_cast<std::uint8_t>(c.type)});
  auto n = [](const auto& v) { return narrow<std::uint32_t>(v.size()); };
  const auto& p = *plan;
  bc_schema_ref ref{p.catalog_version,
                    p.objects_read.data(),
                    n(p.objects_read),
                    p.tables_written.data(),
                    n(p.tables_written),
                    col_refs.data(),
                    n(col_refs),
                    codes.data(),
                    n(codes)};
  // Moves keep every buffer ref points into.
  if (std::holds_alternative<bound::Select>(bound.stmt)) {
    s.columns_ = std::move(columns);
    s.column_types_ = std::move(column_types);
  }
  s.plan_ = std::move(plan);
  s.code_ = std::move(code);
  s.result_codes_ = std::move(codes);
  s.result_layout_ = std::move(layout);
  s.col_refs_ = std::move(col_refs);
  s.ref_ = ref;
  ++compilations_;
}

auto Database::prepare(std::string_view sql) -> const Statement& {
  check_open();
  if (auto it = cache_.find(sql); it != cache_.end()) return *it->second;
  auto s = std::make_unique<Statement>();
  s->sql_ = std::string{sql};
  s->ast_ = ast::parse(sql);
  compile(*s);
  return *cache_.emplace(s->sql_, std::move(s)).first->second;
}

auto Database::execute(const Statement& cstmt, const std::vector<Value>& params) -> Rows {
  check_open();
  // The cache owns every Statement; the const handle is the caller's view of it.
  auto& stmt = *cache_.at(cstmt.sql_);
  Rows out;
  if (stmt.is_ddl()) {
    if (!params.empty()) throw ParamError("DDL takes no parameters");
    auto bound = bound::bind(stmt.ast_, *live_->current());
    try {
      (void)live_->apply(std::get<Ddl>(bound.stmt));
    } catch (const ConstraintViolation& e) {
      throw ExecError(e.what(), BC_E_CONSTRAINT, e.detail().encode());
    }
    out.affected = 0;
    return out;
  }
  // Throws ParamError for parameters the statement's current plan
  // does not take: checked again after a recompile, whose plan may differ.
  auto mismatch = [&]() -> std::string {
    const auto& types = stmt.plan_->param_types;
    if (params.size() != types.size()) {
      return "statement takes " + std::to_string(types.size()) + " parameters, got " + std::to_string(params.size());
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (!datum_fits(params[i], types[i])) return "parameter $" + std::to_string(i + 1) + " has the wrong type";
    }
    return {};
  };
  auto check_params = [](const std::string& why) {
    if (!why.empty()) throw ParamError(why);
  };
  bc_params bp{params};
  // Parameters that do not fit the cached plan may fit the plan a DDL has
  // since made of the text. The runtime says whether the cached plan still
  // fits the catalog (bc_ctx_check, the verification bc_ctx_run makes); it
  // is recompiled only if not, and the parameters are refused against the
  // plan that will run. The runtime alone decides compatibility.
  if (auto why = mismatch(); !why.empty()) {
    // A plan compiled against the current version is the plan of the text:
    // a recompile would take the same parameters.
    if (stmt.ref_.catalog_version == live_->current()->version()) throw ParamError(why);
    bc_ctx_begin(cx_, &bp, {});
    auto fits = bc_ctx_check(cx_, &stmt.ref_);
    auto stale = !fits && bc_error(cx_) == BC_E_LAYOUT && bc_error_detail(cx_) == BC_L_INCOMPATIBLE;
    if (!fits && !stale) {
      ExecError e{bc_error_message(cx_), bc_error(cx_), bc_error_detail(cx_)};
      bc_ctx_reset(cx_);
      throw e;
    }
    bc_ctx_reset(cx_);
    if (stale) compile(stmt);
    check_params(stale ? mismatch() : why);
  }
  auto r = bc_execute(cx_, stmt.code_->fn(), &stmt.ref_, &bp, {}, true, kMaxRetries);
  // bc_ctx_run checks the plan against the catalog it pinned. A plan a DDL
  // made incompatible (BC_E_LAYOUT with BC_L_INCOMPATIBLE) is recompiled
  // from its text and run once more; one a DDL left compatible keeps
  // running, unchecked here.
  if (r == BC_FAILED && bc_error(cx_) == BC_E_LAYOUT && bc_error_detail(cx_) == BC_L_INCOMPATIBLE) {
    bc_ctx_reset(cx_);
    compile(stmt);
    check_params(mismatch());
    r = bc_execute(cx_, stmt.code_->fn(), &stmt.ref_, &bp, {}, true, kMaxRetries);
  }
  const auto& p = *stmt.plan_;
  if (r == BC_CONFLICT) {
    bc_ctx_reset(cx_);
    throw ExecError("conflict: a guard failed on every attempt", BC_CONFLICT);
  }
  if (r == BC_FAILED) {
    ExecError e{bc_error_message(cx_), bc_error(cx_), bc_error_detail(cx_)};
    bc_ctx_reset(cx_);  // the context keeps no snapshot or catalog version while idle
    throw e;
  }

  out.affected = bc_ctx_affected(cx_);
  if (p.returns_rows) {
    out.columns = stmt.columns_;
    out.types = stmt.column_types_;
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx_, &row, &len)) {
      out.rows.push_back(decode_row(stmt.result_layout_, std::as_bytes(std::span{row, len})));
    }
  }
  bc_ctx_reset(cx_);
  return out;
}

#ifdef BYTECASK_SQL_TESTING
auto Database::table_rows(std::string_view table) const -> Rows {
  check_open();
  auto [cat, snap] = live_->pin();
  const auto* t = cat->table(table);
  if (t == nullptr) throw ast::SqlError("relation \"" + std::string{table} + "\" does not exist", -1);
  Rows out;
  // The table's layout with its slots in column order, the order the rows
  // are read in. A column a row's bitmap has no bit for reads as NULL.
  auto layout = t->layout;
  layout.columns.clear();
  for (const auto& c : t->columns) {
    const auto* slot = t->layout.find(c.id);
    if (slot == nullptr) throw std::runtime_error("column " + c.name + " is not in the layout of table " + t->name);
    layout.columns.push_back(*slot);
    out.columns.push_back(c.name);
    out.types.push_back(c.sql);
  }
  // Damage is BC_E_CORRUPT, never a plausible value: this is the oracle's
  // reader, so unlike decode_row (result rows the runtime built) every
  // present field must decode.
  auto corrupt = [&](const std::string& what) { return ExecError(what + " in table " + t->name, BC_E_CORRUPT); };
  for_each_row(snap, t->id, [&](BytesView, BytesView row) {
    if (!row_valid(row)) throw corrupt("malformed row");
    // The slots are the current layout's: a row of another generation would
    // decode from the wrong positions, so it is refused, as the plan
    // runtime refuses it.
    if (auto gen = row_generation(row); gen != t->layout.generation) {
      throw corrupt("row of layout generation " + std::to_string(gen) + ", whose layout is generation " +
                    std::to_string(t->layout.generation));
    }
    std::vector<Value> values;
    for (const auto& c : layout.columns) {
      if (row_is_null(row, c)) {
        values.emplace_back(std::monostate{});
        continue;
      }
      // Present, so the row must hold it: outside its regions it would read as 0 or empty.
      if (!row_holds(row, c)) throw corrupt("a present column " + std::to_string(c.col_id) + " outside its row");
      auto take = [&](auto v) {
        if (!v) throw corrupt("column " + std::to_string(c.col_id) + " does not decode");
        values.emplace_back(Value{*v});
      };
      switch (c.type) {
        case PhysType::I64: take(row_i64(row, c.pos)); break;
        case PhysType::F64: take(row_f64(row, c.pos)); break;
        case PhysType::Bool: take(row_bool(row, c.pos)); break;
        case PhysType::Bytes: {
          auto b = row_bytes(row, c.pos);
          if (!b) throw corrupt("column " + std::to_string(c.col_id) + " does not decode");
          values.emplace_back(Bytes{b->begin(), b->end()});
          break;
        }
      }
    }
    out.rows.push_back(std::move(values));
  });
  return out;
}
#endif

}  // namespace bytecask::sql
