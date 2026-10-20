// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// database.cppm — the embedded SQL API: open a directory, prepare a statement,
// execute it with parameters, read the rows. Every DML statement is compiled
// once to a plan, by TinyCC, and cached by its text; a plan the runtime
// refuses as incompatible with the catalog (docs/catalog_design.md,
// *Compatibility*) is recompiled from that text and run again.
//
// Database::execute is autocommit: each statement is one plan through
// bc_execute, retried on a conflict. Database::begin opens a transaction
// (docs/transaction_design.md), whose statements run on its own context, so
// transactions on different threads run concurrently; everything else on a
// Database is one thread at a time.
//
// prototype: the shape of #494's embedded API, not its design.

module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <shared_mutex>
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
// detail's high byte, raised by the plan); or a conflict on every retry.
// BC_CONFLICT and BC_E_IO are both 1, so conflict() tells them apart, not
// code().
class ExecError : public std::runtime_error {
  struct Conflict {};

public:
  // A failure: a bc_error_code, so a bc_commit_result such as BC_CONFLICT
  // does not compile here. A conflict is made only by conflicted().
  ExecError(const std::string& what, bc_error_code code, std::uint32_t detail = 0)
      : std::runtime_error{what}, code_{code}, detail_{detail} {}
  // Every attempt conflicted; code() is BC_CONFLICT.
  [[nodiscard]] static auto conflicted(const std::string& what) -> ExecError { return {what, Conflict{}}; }
  ExecError(const ExecError&) = default;
  ExecError& operator=(const ExecError&) = default;
  ~ExecError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  // A bc_error_code; BC_CONFLICT when conflict().
  [[nodiscard]] auto code() const noexcept -> int { return code_; }
  [[nodiscard]] auto conflict() const noexcept -> bool { return conflict_; }
  // bc_error_detail: for BC_E_CONSTRAINT, the bc_constraint in the high byte
  // and the object id in the low 24 bits.
  [[nodiscard]] auto detail() const noexcept -> std::uint32_t { return detail_; }
  // The detail decoded, for BC_E_CONSTRAINT; nullopt for any other code.
  [[nodiscard]] auto constraint() const noexcept -> std::optional<ConstraintDetail> {
    if (code_ != BC_E_CONSTRAINT) return std::nullopt;
    return ConstraintDetail::decode(detail_);
  }

private:
  ExecError(const std::string& what, Conflict)
      : std::runtime_error{what}, code_{BC_CONFLICT}, detail_{0}, conflict_{true} {}

  int code_;
  std::uint32_t detail_;
  bool conflict_{false};
};

// A transaction's commit conflicted with a concurrent commit: SQLSTATE 40001.
// Nothing was written, and the transaction has ended; rerun it.
class SerializationFailure : public ExecError {
public:
  explicit SerializationFailure(const std::string& what) : ExecError{conflicted(what)} {}
  SerializationFailure(const SerializationFailure&) = default;
  SerializationFailure& operator=(const SerializationFailure&) = default;
  ~SerializationFailure() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// A call on a transaction a statement failed in, other than rollback():
// SQLSTATE 25P02, as PostgreSQL refuses every command until the
// transaction block ends.
class TransactionAborted : public std::runtime_error {
public:
  TransactionAborted() : std::runtime_error{"current transaction is aborted, commands ignored until end of transaction block"} {}
  TransactionAborted(const TransactionAborted&) = default;
  TransactionAborted& operator=(const TransactionAborted&) = default;
  ~TransactionAborted() override;  // out of line: anchors the vtable (-Wweak-vtables)
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
  // Each compared parameter's SQL type (bound::Bound::param_sql) and the
  // statement's table: a value outside it is BC_C_RANGE on the table,
  // checked before the plan runs, whatever the plan would read.
  std::vector<std::optional<SqlType>> param_sql_;
  std::uint32_t table_id_{0};
  std::vector<std::uint8_t> result_codes_;  // the plan's result_types as bc_type codes
  RowLayout result_layout_;                 // how its result rows decode, built once per compile
  std::vector<bc_col_ref> col_refs_;
  bc_schema_ref ref_{};  // points into plan_ and the vectors above
};

struct TransactionOptions {
  // The most a transaction's writes may hold, keys and values plus a fixed
  // cost per write. A statement that would pass it is BC_E_LIMIT (SQLSTATE
  // 54000), and aborts the transaction.
  std::uint64_t max_bytes{64u << 20};
};

class Database;

// A transaction from Database::begin(): its statements read the snapshot
// taken at its first statement plus their earlier statements' writes, never
// their own, and commit() writes them all as one batch, or nothing. A
// statement that fails aborts it: every later execute() and commit() throws
// TransactionAborted (25P02) until rollback(). After commit() or rollback()
// it has ended, and every call but rollback() throws std::logic_error.
// Destroying an open one rolls it back. Move-only; one thread at a time, and
// ended before the Database closes it.
class Transaction {
public:
  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  ~Transaction();

  // Runs one statement in the transaction. Throws what Database::execute
  // throws for a statement that fails, and the transaction is then aborted;
  // TransactionAborted if it already is. A DDL statement is refused.
  auto execute(const Statement& stmt, const std::vector<Value>& params) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}) -> Rows;
  // Writes every statement's writes as one batch. Throws
  // SerializationFailure on a conflict, ExecError on an engine error, and
  // TransactionAborted after a failed statement; in every case but the last
  // the transaction has ended with nothing written.
  void commit();
  // Ends the transaction; nothing it wrote is kept. Once ended, does nothing.
  void rollback() noexcept;
  [[nodiscard]] auto is_open() const noexcept -> bool { return state_ != State::Ended; }
  [[nodiscard]] auto is_aborted() const noexcept -> bool { return state_ == State::Aborted; }

private:
  friend class Database;
  enum class State : std::uint8_t { Open, Aborted, Ended };
  Transaction(Database& db, TransactionOptions opts);
  void end() noexcept;  // frees the context and the transaction
  void check_open() const;

  Database* db_{nullptr};
  bc_ctx* cx_{nullptr};
  bc_txn* txn_{nullptr};
  State state_{State::Ended};
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
  // Opens a transaction. Its snapshot is taken at its first statement.
  [[nodiscard]] auto begin(TransactionOptions opts = {}) -> Transaction;
  [[nodiscard]] auto cached_plans() const -> std::size_t { return cache_.size(); }
  // How many times a DML statement was compiled, first compiles included.
  [[nodiscard]] auto compilations() const -> std::uint64_t { return compilations_; }
  // The live catalog: the version DDL last published.
  [[nodiscard]] auto catalog() const -> std::shared_ptr<const Catalog> {
    check_open();
    return live_->current();
  }
#ifdef BYTECASK_SQL_TESTING
  // Test only: how many times a cached plan was checked (bc_ctx_check)
  // against a catalog version it was not compiled or checked against.
  [[nodiscard]] auto plan_checks_for_testing() const -> std::uint64_t { return plan_checks_; }
  // Test only (sql_layer_testing): every row of a table, in primary-key
  // order, read from the engine and decoded through the catalog: every
  // column, in the table's order. For the proof tests, which compare a table
  // with the oracle's; SQL has no full scan. Throws ast::SqlError for a
  // table that does not exist, as a statement naming it would.
  [[nodiscard]] auto table_rows(std::string_view table) const -> Rows;
  // Test only: while on, every commit of a plan that writes a table
  // conflicts. Before each commit reaches the engine, every table's catalog
  // record is written back unchanged, which such a plan guards, so execute
  // exhausts its retries. A plan that only reads commits nothing to conflict.
  void conflict_on_every_commit_for_testing(bool on);
  // Test only: the engine under the layer, for a test that stores what the
  // layer would refuse, to check that a read refuses it too.
  [[nodiscard]] auto engine_for_testing() -> DB& {
    check_open();
    return *db_;
  }
#endif

  // Closes the engine. A second call returns at once.
  void close();
  [[nodiscard]] auto is_open() const noexcept -> bool { return !closed_; }

private:
  friend class Transaction;
  Database() = default;
  void check_open() const {
    if (closed_) throw DbClosed();
  }
  void compile(Statement& s);
  // Runs a DML statement on `cx`: autocommit, retried on a conflict, when
  // `txn` is null; else as a statement of `txn`, its writes added to it.
  // Brings a DML statement's plan up to the live catalog. Under the unique lock.
  void refresh(Statement& stmt, const std::vector<Value>& params);
  [[nodiscard]] auto checked_for_live(const Statement& stmt) const -> bool {
    return stmt.ref_.catalog_version == live_->current()->version();
  }
  auto run(bc_ctx* cx, Statement& stmt, const std::vector<Value>& params, bc_txn* txn) -> Rows;
  static auto results(bc_ctx* cx, const Statement& stmt, Rows out) -> Rows;

  std::unique_ptr<DB> db_;
  std::unique_ptr<LiveCatalog> live_;
  bc_db bdb_{};
  bc_catalog bcat_{};
  bc_ctx* cx_{nullptr};
  // Exclusive for whatever changes the statement cache or a statement (prepare,
  // autocommit execute, which may recompile, close); shared for a
  // transaction's statements, which only read a prepared statement.
  mutable std::shared_mutex mu_;
  std::set<Transaction*> open_;  // the transactions close() ends; under mu_
  std::map<std::string, std::unique_ptr<Statement>, std::less<>> cache_;
  std::uint64_t compilations_{0};
#ifdef BYTECASK_SQL_TESTING
  std::uint64_t plan_checks_{0};
#endif
  bool closed_{false};
};

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kArenaBytes = 4u << 20;
constexpr std::uint32_t kMaxRetries = 3;

}  // namespace

ExecError::~ExecError() = default;
SerializationFailure::~SerializationFailure() = default;
TransactionAborted::~TransactionAborted() = default;
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
  std::unique_lock<std::shared_mutex> lock{mu_};
  if (closed_) return;
  closed_ = true;
  // A transaction still open holds the engine's snapshot: ended first, with
  // nothing written, and every later call on it throws.
  for (auto* t : open_) t->end();
  open_.clear();
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
  // Every statement but DDL reads or writes one table; DDL has no plan.
  const auto* table = std::visit(
      [](const auto& st) -> const Table* {
        if constexpr (std::is_same_v<std::decay_t<decltype(st)>, Ddl>) {
          return nullptr;
        } else {
          return st.table;
        }
      },
      bound.stmt);
  if (table == nullptr) {
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
  s.param_sql_ = std::move(bound.param_sql);
  s.table_id_ = table->id;
  s.plan_ = std::move(plan);
  s.code_ = std::move(code);
  s.result_codes_ = std::move(codes);
  s.result_layout_ = std::move(layout);
  s.col_refs_ = std::move(col_refs);
  s.ref_ = ref;
  ++compilations_;
}

auto Database::prepare(std::string_view sql) -> const Statement& {
  std::unique_lock<std::shared_mutex> lock{mu_};
  check_open();
  if (auto it = cache_.find(sql); it != cache_.end()) return *it->second;
  auto s = std::make_unique<Statement>();
  s->sql_ = std::string{sql};
  s->ast_ = ast::parse(sql);
  compile(*s);
  return *cache_.emplace(s->sql_, std::move(s)).first->second;
}

auto Database::execute(const Statement& cstmt, const std::vector<Value>& params) -> Rows {
  std::unique_lock<std::shared_mutex> lock{mu_};
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
  refresh(stmt, params);
  return run(cx_, stmt, params, nullptr);
}

void Database::refresh(Statement& stmt, const std::vector<Value>& params) {
  // The runtime says whether the cached plan still fits the catalog
  // (bc_ctx_check, the verification bc_ctx_run makes); it is recompiled only
  // if not. The runtime alone decides compatibility. A plan that fits is
  // marked with the version it was checked against, so it is checked once
  // per catalog version, not on every run after a DDL. The runtime pins that
  // version or a later one: a later one is checked again next time.
  if (checked_for_live(stmt)) return;
  auto version = live_->current()->version();
#ifdef BYTECASK_SQL_TESTING
  ++plan_checks_;
#endif
  bc_params bp{params};
  bc_ctx_begin(cx_, &bp, {});
  auto fits = bc_ctx_check(cx_, &stmt.ref_);
  auto stale = !fits && bc_error(cx_) == BC_E_LAYOUT && bc_error_detail(cx_) == BC_L_INCOMPATIBLE;
  if (!fits && !stale) {
    ExecError e{bc_error_message(cx_), bc_error(cx_), bc_error_detail(cx_)};
    bc_ctx_reset(cx_);
    throw e;
  }
  bc_ctx_reset(cx_);
  if (stale) {
    compile(stmt);
  } else {
    stmt.ref_.catalog_version = version;
  }
}

auto Database::run(bc_ctx* cx, Statement& stmt, const std::vector<Value>& params, bc_txn* txn) -> Rows {
  Rows out;
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
  // A value outside its parameter's SQL type is refused before the plan
  // runs, as PostgreSQL refuses a $n it cannot coerce at bind: even one a
  // plan would not read (a key whose other part is NULL).
  auto check_params = [&] {
    if (auto why = mismatch(); !why.empty()) throw ParamError(why);
    for (std::size_t i = 0; i < params.size(); ++i) {
      const auto& sql = stmt.param_sql_.at(i);
      if (!sql) continue;  // assigned only: the row builder checks it, in column order
      if (auto range = out_of_range(*sql, params[i])) {
        throw ExecError(*range + " for parameter $" + std::to_string(i + 1), BC_E_CONSTRAINT,
                        ConstraintDetail{BC_C_RANGE, stmt.table_id_}.encode());
      }
    }
  };
  bc_params bp{params};
  // The parameters are checked against the plan that will run, and so
  // against the catalog it was verified against (refresh): a DDL since the
  // plan was compiled may have changed what they are (an INTEGER key
  // recreated as BIGINT). In a transaction the plan runs against the catalog
  // version its first statement pinned, and bc_ctx_run verifies it there: a
  // plan that does not fit fails the statement, and the transaction.
  check_params();
  if (txn != nullptr) {
    // The statement's writes join the transaction, or it aborts.
    bc_ctx_begin_txn(cx, txn, &bp, {});
    bc_ctx_run(cx, stmt.code_->fn(), &stmt.ref_);
    if (!bc_txn_add(cx)) {
      ExecError e{bc_error_message(cx), bc_error(cx), bc_error_detail(cx)};
      bc_ctx_reset(cx);
      throw e;
    }
    return results(cx, stmt, std::move(out));
  }
  auto r = bc_execute(cx, stmt.code_->fn(), &stmt.ref_, &bp, {}, true, kMaxRetries);
  // bc_ctx_run checks the plan against the catalog it pinned. A plan a DDL
  // made incompatible (BC_E_LAYOUT with BC_L_INCOMPATIBLE) is recompiled
  // from its text and run once more; one a DDL left compatible keeps
  // running, unchecked here.
  if (r == BC_FAILED && bc_error(cx) == BC_E_LAYOUT && bc_error_detail(cx) == BC_L_INCOMPATIBLE) {
    bc_ctx_reset(cx);
    compile(stmt);
    check_params();
    r = bc_execute(cx, stmt.code_->fn(), &stmt.ref_, &bp, {}, true, kMaxRetries);
  }
  if (r == BC_CONFLICT) {
    bc_ctx_reset(cx);
    throw ExecError::conflicted("conflict: a guard failed on every attempt");
  }
  if (r == BC_FAILED) {
    ExecError e{bc_error_message(cx), bc_error(cx), bc_error_detail(cx)};
    bc_ctx_reset(cx);  // the context keeps no snapshot or catalog version while idle
    throw e;
  }

  return results(cx, stmt, std::move(out));
}

// The rows and count the statement the context ran left, decoded; the
// context is reset either way.
auto Database::results(bc_ctx* cx, const Statement& stmt, Rows out) -> Rows {
  const auto& p = *stmt.plan_;
  out.affected = bc_ctx_affected(cx);
  if (p.returns_rows) {
    out.columns = stmt.columns_;
    out.types = stmt.column_types_;
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx, &row, &len)) {
      // Result rows are decoded after the commit. That is safe while the
      // compiled write plans emit no rows (only bc_emit_count): a RETURNING
      // will have to decode its rows before it commits.
      auto values = decode_row(stmt.result_layout_, std::as_bytes(std::span{row, len}));
      if (!values) {
        bc_ctx_reset(cx);
        throw ExecError("malformed result row: column " + std::to_string(values.error()), BC_E_CORRUPT);
      }
      out.rows.push_back(std::move(*values));
    }
  }
  bc_ctx_reset(cx);
  return out;
}

auto Database::begin(TransactionOptions opts) -> Transaction {
  std::unique_lock<std::shared_mutex> lock{mu_};
  check_open();
  return Transaction{*this, opts};
}

Transaction::Transaction(Database& db, TransactionOptions opts) : db_{&db} {
  cx_ = bc_ctx_new(&db.bdb_, &db.bcat_, kArenaBytes);
  txn_ = cx_ == nullptr ? nullptr : bc_txn_new(&db.bdb_, &db.bcat_, opts.max_bytes);
  if (txn_ == nullptr) {
    bc_ctx_free(cx_);
    throw std::bad_alloc();
  }
  try {
    db.open_.insert(this);  // under the lock begin() holds
  } catch (...) {
    end();
    throw;
  }
  state_ = State::Open;
}

Transaction::Transaction(Transaction&& other) noexcept { *this = std::move(other); }

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this == &other) return *this;
  rollback();
  if (other.state_ != State::Ended) {
    std::unique_lock<std::shared_mutex> lock{other.db_->mu_};
    // The node is reused, not reallocated: nothing here can throw.
    auto node = other.db_->open_.extract(&other);
    node.value() = this;
    other.db_->open_.insert(std::move(node));
  }
  db_ = std::exchange(other.db_, nullptr);
  cx_ = std::exchange(other.cx_, nullptr);
  txn_ = std::exchange(other.txn_, nullptr);
  state_ = std::exchange(other.state_, State::Ended);
  return *this;
}

Transaction::~Transaction() { rollback(); }

void Transaction::end() noexcept {
  bc_txn_free(txn_);  // a transaction not committed writes nothing
  bc_ctx_free(cx_);
  txn_ = nullptr;
  cx_ = nullptr;
  state_ = State::Ended;
}

void Transaction::rollback() noexcept {
  if (state_ == State::Ended) return;
  std::unique_lock<std::shared_mutex> lock{db_->mu_};
  if (state_ == State::Ended) return;  // the database closed it meanwhile
  db_->open_.erase(this);
  end();
}

void Transaction::check_open() const {
  if (state_ == State::Ended) throw std::logic_error("the transaction has ended");
  if (state_ == State::Aborted) throw TransactionAborted();
}

auto Transaction::execute(const Statement& cstmt, const std::vector<Value>& params) -> Rows {
  std::shared_lock<std::shared_mutex> lock{db_->mu_};
  if (db_->closed_) throw DbClosed();
  check_open();
  try {
    auto& stmt = *db_->cache_.at(cstmt.sql());
    // DDL is its own atomic operation, never part of a transaction.
    if (stmt.is_ddl()) throw std::logic_error("DDL inside a transaction is not supported");
    if (!db_->checked_for_live(stmt)) {
      // A recompile changes the statement other transactions may be running:
      // under the unique lock. A DDL between it and the run fails the
      // statement in bc_ctx_run.
      lock.unlock();
      {
        std::unique_lock<std::shared_mutex> unique{db_->mu_};
        if (db_->closed_) throw DbClosed();
        db_->refresh(stmt, params);
      }
      lock.lock();
      if (db_->closed_) throw DbClosed();
      if (state_ == State::Ended) throw std::logic_error("the transaction has ended");
    }
    return db_->run(cx_, stmt, params, txn_);
  } catch (...) {
    // Every error aborts the transaction, as in PostgreSQL: a statement
    // refused before it ran too.
    state_ = State::Aborted;
    throw;
  }
}

auto Transaction::execute(std::string_view sql, const std::vector<Value>& params) -> Rows {
  {
    std::shared_lock<std::shared_mutex> lock{db_->mu_};
    if (db_->closed_) throw DbClosed();
    if (state_ == State::Aborted && !db_->cache_.contains(sql)) {
      // As in PostgreSQL, a statement that does not parse reports that, not
      // 25P02: the text is parsed before the transaction's state is checked.
      (void)ast::parse(sql);
    }
    check_open();
  }
  const Statement* stmt = nullptr;
  try {
    stmt = &db_->prepare(sql);
  } catch (const DbClosed&) {
    throw;
  } catch (...) {
    state_ = State::Aborted;  // a statement that does not parse or bind aborts it too
    throw;
  }
  return execute(*stmt, params);
}

void Transaction::commit() {
  {
    std::shared_lock<std::shared_mutex> lock{db_->mu_};
    if (db_->closed_) throw DbClosed();
    check_open();
  }
  // Shared: commits on different transactions run together, and share the
  // engine's group commit. The commit ends the transaction, whatever the
  // outcome.
  auto r = BC_FAILED;
  std::optional<ExecError> failed;
  {
    std::shared_lock<std::shared_mutex> lock{db_->mu_};
    if (db_->closed_) throw DbClosed();
    r = bc_txn_commit(cx_, txn_, true);
    if (r == BC_FAILED) failed.emplace(bc_error_message(cx_), bc_error(cx_), bc_error_detail(cx_));
  }
  rollback();  // frees what is left; nothing more is written
  if (r == BC_CONFLICT) throw SerializationFailure("could not serialize access due to concurrent update");
  if (failed) throw *failed;
}

#ifdef BYTECASK_SQL_TESTING
void Database::conflict_on_every_commit_for_testing(bool on) {
  check_open();
  if (!on) {
    bc_ctx_before_commit_for_testing(cx_, {});
    return;
  }
  bc_ctx_before_commit_for_testing(cx_, [this] {
    auto cat = live_->current();  // held: the loop reads its tables
    for (const auto& [id, t] : cat->tables()) {
      auto key = table_record_key(id);
      Bytes record;
      if (db_->get({}, key, record)) (void)db_->put({}, key, record);
    }
  });
}

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
  // reader. decode_row reads each column through row_column, as the
  // runtime's readers do, so a present column the row does not hold is
  // refused here exactly where a plan refuses it.
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
    auto values = decode_row(layout, row);
    if (!values) throw corrupt("column " + std::to_string(values.error()) + " does not decode");
    out.rows.push_back(std::move(*values));
  });
  return out;
}
#endif

}  // namespace bytecask::sql
