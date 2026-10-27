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
// (docs/transaction_design.md), whose statements run on its own context.
// Every call runs concurrently with every other but close(): no lock is
// held across engine I/O but the shared one close() waits on (DatabaseLife),
// and a compiled plan is never changed while a run uses it (a recompile
// swaps in a new one). A DDL that finds another in progress fails at once
// with SerializationFailure; a statement's pin waits only for the instant
// a DDL publishes its catalog.
//
// prototype: the shape of #494's embedded API, not its design.

module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <limits>
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
#include <thread>
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

// PostgreSQL's AVG of an exact number, as numeric's text: its numeric_div
// of the sum (the total at the argument's `scale`) by the count, the scale
// select_div_scale chooses (at least 16 significant digits), rounded half
// away from zero. More digits than 64 bits hold, so a result row carries it
// as text (Rows).
[[nodiscard]] auto average_text(std::int64_t sum, std::int64_t count, std::uint8_t scale) -> std::string;

struct Rows {
  std::vector<std::string> columns;
  // Each column's SQL type, as the catalog declares it. A NUMERIC's value,
  // an AVG's, is its text, as bytes: more digits than an int64_t holds.
  std::vector<SqlType> types;
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

// A commit whose outcome is unknown: it failed and left the engine
// degraded, after its batch may have reached the data file. SQLSTATE 08007,
// as PostgreSQL reports a commit it cannot resolve. Never "nothing
// written": a reopen may find the writes.
// It holds nothing allocated and its message is static, so an allocation
// failure can never turn it into another error.
class TransactionResolutionUnknown : public std::exception {
public:
  TransactionResolutionUnknown() = default;
  TransactionResolutionUnknown(const TransactionResolutionUnknown&) = default;
  TransactionResolutionUnknown& operator=(const TransactionResolutionUnknown&) = default;
  ~TransactionResolutionUnknown() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto what() const noexcept -> const char* override;
};

// A statement given to a transaction a statement failed in: SQLSTATE 25P02,
// as PostgreSQL refuses every command until the transaction block ends.
// commit() then rolls back instead, as PostgreSQL's COMMIT does.
class TransactionAborted : public std::runtime_error {
public:
  TransactionAborted() : std::runtime_error{"current transaction is aborted, commands ignored until end of transaction block"} {}
  TransactionAborted(const TransactionAborted&) = default;
  TransactionAborted& operator=(const TransactionAborted&) = default;
  ~TransactionAborted() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// A statement the layer does not support where it was given: SQLSTATE
// 0A000. A DDL statement given to a transaction is one: DDL is its own
// atomic operation. A statement the planner can't plan is another, its
// message starting "no plan:". Thrown before anything runs; in a
// transaction it aborts the transaction, as every error does in PostgreSQL.
using FeatureNotSupported = ast::FeatureNotSupported;

// Parameters a statement does not take: the wrong number, or a value not of
// its parameter's type. Thrown before the plan runs.
class ParamError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
  ParamError(const ParamError&) = default;
  ParamError& operator=(const ParamError&) = default;
  ~ParamError() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// The SQLSTATE PostgreSQL reports for the error, for each exception the API
// throws, and XX000 (internal_error) for anything else. The one mapping:
// the PostgreSQL server reports it, and the proof oracle compares it with
// PostgreSQL's.
[[nodiscard]] auto sqlstate(const std::exception& e) noexcept -> std::string_view;

class Database;

// What a statement does, as PostgreSQL's command tag names it.
enum class Command {
  Select,
  Insert,
  Update,
  Delete,
  CreateTable,
  CreateIndex,
  DropTable,
  DropIndex,
  AlterTable,
};

// A result column: its name and SQL type, as a client is told them before
// the statement runs.
struct ResultColumn {
  std::string name;
  SqlType type;
};

using bound::ParamInfo;

class Statement {
public:
  [[nodiscard]] auto sql() const -> const std::string& { return sql_; }
  [[nodiscard]] auto is_ddl() const -> bool { return ddl_; }
  [[nodiscard]] auto command() const -> Command;
  // Of the plan the statement runs now: a DDL may make it recompile.
  [[nodiscard]] auto param_types() const -> std::vector<PhysType>;
  // Each parameter's SQL type and use, and the result's columns (none for
  // a statement that returns no rows), of the plan the statement runs now:
  // Database::refit first to describe it against the live catalog. Empty
  // for DDL.
  [[nodiscard]] auto params() const -> std::vector<ParamInfo>;
  [[nodiscard]] auto columns() const -> std::vector<ResultColumn>;
  // The plan file, as EXPLAIN would print it; empty for DDL.
  [[nodiscard]] auto plan_source() const -> std::string;

private:
  friend class Database;
  friend class Transaction;
  // One compile of a DML statement, never changed once built but for
  // `checked`. A run holds it by shared_ptr, so a recompile, which swaps in a
  // new one, never changes a plan a run on another thread is using.
  struct Compiled {
    std::unique_ptr<GeneratedPlan> plan;
    std::unique_ptr<TccPlan> code;
    std::vector<std::string> columns;
    std::vector<SqlType> column_types;
    // Each compared parameter's SQL type (bound::Bound::param_sql) and its
    // column's table: a value outside it is BC_C_RANGE on that table,
    // checked before the plan runs, whatever the plan would read.
    std::vector<std::optional<SqlType>> param_sql;
    std::vector<std::uint32_t> param_table;
    std::vector<ParamInfo> params;
    std::vector<std::uint8_t> result_codes;  // the plan's result_types as bc_type codes
    RowLayout result_layout;                 // how its result rows decode
    std::vector<bc_col_ref> col_refs;
    bc_schema_ref ref{};  // points into the members above; catalog_version: the one compiled against
    // Autocommit only: the catalog version the plan was compiled for, or
    // was verified to fit by bc_ctx_check. At that version autocommit makes
    // no check of its own before a run, which checks the parameters against
    // the plan that will run; the runtime verifies the plan on every run
    // regardless. A transaction's statements do not use it.
    mutable std::atomic<std::uint64_t> checked{0};
  };
  [[nodiscard]] auto compiled() const -> std::shared_ptr<const Compiled> {
    std::lock_guard<std::mutex> lock{mu_};
    return compiled_;
  }
  void set_compiled(std::shared_ptr<const Compiled> c) const {
    std::lock_guard<std::mutex> lock{mu_};
    compiled_ = std::move(c);
  }

  std::string sql_;
  ast::Statement ast_;
  bool ddl_{false};
  const Database* owner_{nullptr};  // the Database that prepared it, the only one it runs on
  mutable std::mutex mu_;                             // held only to copy or swap compiled_
  mutable std::shared_ptr<const Compiled> compiled_;  // null for DDL
};

struct TransactionOptions {
  // The most a transaction may hold: its writes' keys and values, the
  // structures they are kept in, its guards, claims and locks. Each
  // statement's write set moves here from its statement budget when the
  // statement ends. A statement that would take it past is BC_E_LIMIT
  // (SQLSTATE 54000), and aborts the transaction.
  std::uint64_t max_bytes{64u << 20};
};

// A flag another thread sets to cancel the statement running with it
// (ExecutionOptions::cancel). Not copyable: a statement reads it by address
// while it runs.
class CancelFlag {
public:
  CancelFlag() = default;
  CancelFlag(const CancelFlag&) = delete;
  CancelFlag& operator=(const CancelFlag&) = delete;
  void cancel() noexcept { __atomic_store_n(&v_, std::uint8_t{1}, __ATOMIC_RELAXED); }
  void reset() noexcept { __atomic_store_n(&v_, std::uint8_t{0}, __ATOMIC_RELAXED); }
  [[nodiscard]] auto cancelled() const noexcept -> bool { return __atomic_load_n(&v_, __ATOMIC_RELAXED) != 0; }

private:
  friend class Database;
  // A plain byte, so the C runtime can read it (bc_budget::cancel), always
  // through atomic operations.
  std::uint8_t v_{0};
};

// What may end one statement early, per call. Either fails it with ExecError
// BC_E_BUDGET (SQLSTATE 57014, query_canceled), checked where the runtime
// charges its work: every 64 record reads or cursor steps. In a transaction,
// the statement so failed aborts the transaction, as any failed statement.
struct ExecutionOptions {
  std::chrono::steady_clock::time_point deadline{};  // the clock's epoch: none
  const CancelFlag* cancel{nullptr};
};

class Database;

// What commit() did, as PostgreSQL's COMMIT reports it in its command tag.
enum class CommitOutcome {
  Committed,
  RolledBack,  // the transaction was aborted: nothing was written
};

enum class TxnState : std::uint8_t { Open, Aborted, Ended };

// A transaction's resources, on the heap so that a moved Transaction keeps
// them where the Database's list of open transactions finds them.
struct TxnHandle {
  bc_ctx* cx{nullptr};    // null once ended or aborted: an aborted transaction keeps no snapshot
  bc_txn* txn{nullptr};
  std::atomic<TxnState> state{TxnState::Ended};  // close() ends it from another thread

  void release() noexcept {
    bc_ctx_free(cx);
    bc_txn_free(txn);  // a transaction not committed writes nothing
    cx = nullptr;
    txn = nullptr;
  }
  // Aborted: what it held is freed now, the snapshot that holds vacuum back
  // first. Only the state is left, for rollback() or commit() to end.
  void abort() noexcept {
    release();
    state.store(TxnState::Aborted);
  }
};

// What a Database shares with its transactions, and what outlives it while
// a Transaction does: whether it is closed, and the transactions open.
struct DatabaseLife {
  // Shared by every call for as long as it runs, engine I/O included:
  // statements, commits and autocommit execute alike. Exclusive only for
  // close(), which so waits for the calls in flight and no more; no other
  // call ever waits on it.
  std::shared_mutex mu;
  std::atomic<bool> closing{false};  // set before close() waits: calls arriving meanwhile refuse at once
  bool closed{false};                // under mu
  std::mutex open_mu;                // guards open; taken briefly, never across I/O
  std::set<TxnHandle*> open;
};

// A transaction from Database::begin(): its statements read the snapshot
// taken at its first statement plus their earlier statements' writes, never
// their own, and commit() writes them all as one batch, or nothing. A
// statement that fails aborts it: every later execute() throws
// TransactionAborted (25P02), and commit() rolls it back, as PostgreSQL's
// COMMIT does in an aborted transaction. After commit() or rollback() it has
// ended, and every call but rollback() throws std::logic_error; a
// moved-from Transaction has ended. Destroying an open one rolls it back.
// Move-only; one thread at a time. It may outlive its Database: close()
// ends it, and after that it touches nothing of the Database.
class Transaction {
public:
  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  ~Transaction();

  // Runs one statement in the transaction. Throws what Database::execute
  // throws for a statement that fails, and the transaction is then aborted;
  // TransactionAborted if it already is. A DDL statement is refused. A
  // statement whose plan does not fit the catalog the transaction pinned
  // throws SerializationFailure, and the transaction has ended.
  auto execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts = {}) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}, const ExecutionOptions& opts = {}) -> Rows;
  // Writes every statement's writes as one batch: Committed. After a failed
  // statement it writes nothing and returns RolledBack. Throws
  // SerializationFailure on a conflict and ExecError on an engine error,
  // with nothing written; TransactionResolutionUnknown when the commit
  // failed after its batch may have reached the engine. The transaction has
  // ended in every case.
  [[nodiscard]] auto commit() -> CommitOutcome;
  // Ends the transaction; nothing it wrote is kept. Once ended, does nothing.
  void rollback() noexcept;
  [[nodiscard]] auto is_open() const noexcept -> bool { return h_ && h_->state.load() != TxnState::Ended; }
  [[nodiscard]] auto is_aborted() const noexcept -> bool { return h_ && h_->state.load() == TxnState::Aborted; }

private:
  friend class Database;
  Transaction(Database& db, TransactionOptions opts);
  // Under life_->mu, shared: refuses a call on a closed database or an
  // ended or aborted transaction.
  void check_usable() const;
  auto run_locked(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows;
  void end_locked() noexcept;  // under life_->mu, shared: forgets and frees it

  std::shared_ptr<DatabaseLife> life_;
  Database* db_{nullptr};         // valid while !life_->closed, read under life_->mu
  std::unique_ptr<TxnHandle> h_;  // null once moved from
};

// When a committed write reaches the disk, as the MariaDB plugin's
// bytecaskdb_sync. A commit is written to the data file before it returns in
// every mode, so a process crash loses nothing; the modes differ in what an
// OS crash or power loss loses. DDL is synced in every mode.
enum class SyncMode {
  AtEveryCommit,   // before the commit returns; concurrent commits share one fdatasync
  AtInterval,      // by a background thread every sync_interval_ms: at most one interval is lost
  AtFileRotation,  // when the active data file fills, and at close: up to one data file is lost
};

struct DatabaseOptions {
  SyncMode sync{SyncMode::AtEveryCommit};
  std::uint32_t sync_interval_ms{1000};  // 1 to 60,000, as the plugin's; open refuses another
  // How sealed data files are read, as the plugin's bytecaskdb_io_backend,
  // bytecaskdb_buffer_pool_size and bytecaskdb_buffer_pool_direct_io. The
  // pool's capacity is its whole footprint and is read only for BufferPool;
  // open refuses one under twice the engine's file size, with the engine's
  // std::invalid_argument.
  IoBackend io_backend{IoBackend::Pread};
  BufferPoolOptions buffer_pool{};
  // The most memory one statement may hold: the keys and rows it builds or
  // reads, its pinned values, its groups, its result rows and its write set
  // (what it keeps until commit or, in a transaction, until the statement
  // ends). Past it the statement fails with BC_E_LIMIT (SQLSTATE
  // 54000), never a spill; open refuses 0.
  std::uint64_t statement_memory_bytes{64u << 20};
};

// Every call may be made from any thread, concurrently with any other but
// close(). None waits on another's engine I/O: a commit, a statement or a
// prepare never waits for another transaction's commit or sync, and a DDL
// that finds another DDL in progress throws SerializationFailure rather
// than wait. The one wait left: a statement's pin waits for the instant
// between a DDL's batch reaching the engine and its catalog taking effect.
class Database {
public:
  // Throws std::invalid_argument for a sync that is not a SyncMode, a
  // sync_interval_ms outside 1 to 60,000, or a statement_memory_bytes of 0.
  [[nodiscard]] static auto open(const std::filesystem::path& dir, DatabaseOptions opts = {})
      -> std::unique_ptr<Database>;
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // prepare, execute, begin, catalog and table_rows throw bytecask::DbClosed
  // once close() was called, as the engine's calls do.

  // Parses, binds and (for DML) plans and compiles; cached by the exact text.
  // Throws ast::SqlError, or FeatureNotSupported ("no plan: ...") for a
  // statement the planner can't plan. The reference is valid until close().
  auto prepare(std::string_view sql) -> const Statement&;
  // Throws ParamError for parameters of the wrong number or type,
  // ast::SqlError or FeatureNotSupported if a recompile fails, as prepare
  // does, ExecError if the plan fails (BC_E_BUDGET, 57014, once `opts`'s
  // deadline passes or its flag is set; DDL runs to the end regardless),
  // TransactionResolutionUnknown if its commit's outcome is unknown,
  // std::logic_error for a statement another Database prepared, and for
  // DDL, what LiveCatalog::apply throws (DdlRefused for a statement the
  // catalog refuses), but a ConstraintViolation as an ExecError (a unique
  // index over duplicates is BC_E_CONSTRAINT, BC_C_UNIQUE and the table's
  // id) and another DDL in progress as SerializationFailure.
  auto execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts = {}) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}, const ExecutionOptions& opts = {}) -> Rows;
  // Opens a transaction. Its snapshot is taken at its first statement.
  [[nodiscard]] auto begin(TransactionOptions opts = {}) -> Transaction;
  // Recompiles the statement's plan if the live catalog no longer fits it,
  // as its next run would, so params() and columns() describe what that run
  // takes and returns. Runs nothing. Throws what a recompile throws.
  void refit(const Statement& stmt);
  [[nodiscard]] auto cached_plans() const -> std::size_t {
    std::shared_lock<std::shared_mutex> lock{cache_mu_};
    return cache_.size();
  }
  // How many times a DML statement was compiled, first compiles included.
  [[nodiscard]] auto compilations() const -> std::uint64_t { return compilations_.load(); }
  // The live catalog: the version DDL last published.
  [[nodiscard]] auto catalog() const -> std::shared_ptr<const Catalog>;
#ifdef BYTECASK_SQL_TESTING
  // Test only: how many times a cached plan was checked (bc_ctx_check)
  // against a catalog version it was not compiled or checked against.
  [[nodiscard]] auto plan_checks_for_testing() const -> std::uint64_t { return plan_checks_.load(); }
  // Test only (sql_layer_testing): every row of a table, in primary-key
  // order, read from the engine and decoded through the catalog: every
  // column, in the table's order. For the proof tests, which compare a table
  // with the oracle's; SQL has no full scan. Throws ast::SqlError for a
  // table that does not exist, as a statement naming it would.
  [[nodiscard]] auto table_rows(std::string_view table) const -> Rows;
  // Test only: runs every plan the search costs for a SELECT, autocommit,
  // each on its own snapshot, and returns each one's rows, the cheapest
  // plan's first (docs/planner_design.md, *Tests*, alternatives agree).
  // Throws what prepare and execute throw.
  [[nodiscard]] auto alternatives_for_testing(std::string_view sql, const std::vector<Value>& params) -> std::vector<Rows>;
  // Test only: while on, every autocommit plan that writes a table
  // conflicts. Before each commit reaches the engine, every table's catalog
  // record is written back unchanged, which such a plan guards, so execute
  // exhausts its retries. A plan that only reads commits nothing to conflict.
  void conflict_on_every_commit_for_testing(bool on);
  // Test only: called in each transaction statement right after the
  // transaction's catalog is pinned, before the plan is checked against it.
  // Set it while no transaction runs.
  void after_txn_pin_for_testing(std::function<void()> hook) { after_txn_pin_ = std::move(hook); }
  // Test only: the live catalog, for a test that holds a DDL mid-commit.
  [[nodiscard]] auto live_catalog_for_testing() -> LiveCatalog& {
    if (life_->closing.load()) throw DbClosed();
    return *live_;
  }
  // Test only: the engine under the layer, for a test that stores what the
  // layer would refuse, to check that a read refuses it too.
  [[nodiscard]] auto engine_for_testing() -> DB& {
    if (life_->closing.load()) throw DbClosed();
    return *db_;
  }
#endif

  // Ends every open transaction, then closes the engine, once the calls in
  // flight have returned. A second call returns at once.
  void close();
  [[nodiscard]] auto is_open() const noexcept -> bool { return !life_->closing.load(); }

private:
  friend class Transaction;
  using Compiled = Statement::Compiled;
  Database() = default;
  // The AtInterval flusher: an empty synced write returns once every earlier
  // write is durable, and costs no fdatasync when none is waiting.
  void sync_loop(std::chrono::milliseconds interval);
  // Stops and joins the flusher, if one runs. A concurrent call waits for the
  // join, so no caller goes on to close the engine under a running flusher.
  void stop_flusher();
  // Takes life_->mu shared for a call, or throws DbClosed.
  [[nodiscard]] auto enter() const -> std::shared_lock<std::shared_mutex>;
  // The same calls, under the lock enter() took.
  auto prepare_locked(std::string_view sql) -> const Statement&;
  [[nodiscard]] auto cached(std::string_view sql) const -> bool;
  // A DML statement's plan for the live catalog; null for DDL. Throws what
  // binding, planning and compiling throw.
  [[nodiscard]] auto build(const Statement& s) -> std::shared_ptr<const Compiled>;
  // What the search costs plans with: each table's row count, counted on a
  // snapshot taken now.
  [[nodiscard]] auto row_counts() const -> RowCounts;
  // Compiles a plan's text and sets what a run needs from it.
  void link(Compiled& c);
  auto recompile(const Statement& stmt) -> std::shared_ptr<const Compiled>;
  // The statement's plan, checked against the live catalog and recompiled
  // if it no longer fits.
  auto fitting_plan(bc_ctx* cx, const Statement& stmt, const std::vector<Value>& params)
      -> std::shared_ptr<const Compiled>;
  // Runs a DML statement on `cx`: autocommit, retried on a conflict, when
  // `txn` is null; else as a statement of `txn`, its writes added to it.
  auto run(bc_ctx* cx, const Statement& stmt, const std::vector<Value>& params, bc_txn* txn,
           const ExecutionOptions& opts) -> Rows;
  // The statement budget with a call's deadline and cancel flag.
  [[nodiscard]] auto budget_for(const ExecutionOptions& opts) const noexcept -> bc_budget;
  // Throws ParamError for parameters the plan does not take, and ExecError
  // BC_C_RANGE for a value outside its parameter's SQL type.
  static void check_params(const Compiled& c, const std::vector<Value>& params);
  static auto results(bc_ctx* cx, const Compiled& c, Rows out) -> Rows;
  static auto quotients(const GeneratedPlan& p, std::vector<Value> values) -> std::vector<Value>;
  // An autocommit context: one per call in flight, kept for the next.
  [[nodiscard]] auto acquire_context() -> bc_ctx*;
  void release_context(bc_ctx* cx) noexcept;

  bool sync_commits_{true};  // set by open(); whether a commit waits for fdatasync
  bc_budget budget_{};       // set by open(); every statement's
  std::mutex flush_mu_;
  std::condition_variable flush_cv_;
  bool flush_stop_{false};  // under flush_mu_
  std::mutex flusher_stop_mu_;  // held across the join by stop_flusher()
  std::thread flusher_;         // AtInterval only; joined before the engine closes
  std::unique_ptr<DB> db_;
  std::unique_ptr<LiveCatalog> live_;
  bc_db bdb_{};
  bc_catalog bcat_{};
  std::shared_ptr<DatabaseLife> life_;
  // The cache map: shared to look a statement up, exclusive to add one.
  // Statements are never removed but by close().
  mutable std::shared_mutex cache_mu_;
  std::map<std::string, std::unique_ptr<Statement>, std::less<>> cache_;
  std::mutex compile_mu_;  // TinyCC compiles one plan at a time; no I/O under it
  std::mutex contexts_mu_;  // guards idle_; taken only to take or return one
  std::vector<bc_ctx*> idle_;
  std::atomic<std::uint64_t> compilations_{0};
#ifdef BYTECASK_SQL_TESTING
  std::atomic<std::uint64_t> plan_checks_{0};
  std::atomic<bool> conflict_every_commit_{false};
  std::function<void()> after_txn_pin_;
#endif
};

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

constexpr std::uint32_t kMaxRetries = 3;

}  // namespace

ExecError::~ExecError() = default;
SerializationFailure::~SerializationFailure() = default;
TransactionAborted::~TransactionAborted() = default;
TransactionResolutionUnknown::~TransactionResolutionUnknown() = default;
auto TransactionResolutionUnknown::what() const noexcept -> const char* {
  return "the transaction's outcome is unknown: its commit failed after its batch may have reached the data file "
         "(SQLSTATE 08007)";
}
ParamError::~ParamError() = default;

namespace {

// Throws make(what), or make("") if the message cannot be allocated: the
// outcome an exception reports never turns into std::bad_alloc. (An empty
// message allocates nothing, and the copy thrown shares the message.)
template <class Make>
[[noreturn]] void throw_keeping_outcome(std::string_view what, Make make) {
  using E = decltype(make(std::string{}));
  std::optional<E> e;
  try {
    e.emplace(make(std::string{what}));
  } catch (const std::bad_alloc&) {
    e.emplace(make(std::string{}));
  }
  throw E{*e};
}

[[noreturn]] void throw_serialization_failure(std::string_view what) {
  throw_keeping_outcome(what, [](const std::string& m) { return SerializationFailure{m}; });
}

// A failure the runtime reported, as the API's exception for it.
[[noreturn]] void throw_error(bc_error_code code, std::uint32_t detail, std::string_view message) {
  if (code == BC_E_UNKNOWN) throw TransactionResolutionUnknown{};
  throw_keeping_outcome(message, [&](const std::string& m) { return ExecError{m, code, detail}; });
}

// The context's error, thrown once the context is reset.
[[noreturn]] void throw_context_error(bc_ctx* cx) {
  auto code = bc_error(cx);
  auto detail = bc_error_detail(cx);
  std::string message;
  if (code != BC_E_UNKNOWN) {  // its exception's message is static
    try {
      message = bc_error_message(cx);
    } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
    }
  }
  bc_ctx_reset(cx);  // the context keeps no snapshot or catalog version while idle
  throw_error(code, detail, message);
}

constexpr std::string_view kConflict = "could not serialize access due to concurrent update";
constexpr std::string_view kCatalogChanged =
    "could not serialize access: the catalog changed since the transaction's first statement";
constexpr std::string_view kDdlInProgress = "could not serialize access: another DDL is in progress";
// A transaction's statement re-pins or recompiles at most this many times
// before it gives up with 40001: each is for a DDL racing it.
constexpr int kTxnPlanAttempts = 3;

}  // namespace

auto average_text(std::int64_t sum, std::int64_t count, std::uint8_t scale) -> std::string {
  // The magnitude, INT64_MIN's too.
  const auto num = sum < 0 ? 0 - static_cast<std::uint64_t>(sum) : static_cast<std::uint64_t>(sum);
  const auto den = static_cast<std::uint64_t>(count);
  // select_div_scale's weights: of each operand's first non-zero base-10000
  // digit, and that digit. A zero has weight 0 and digit 0.
  auto weight_of = [](std::uint64_t v, std::uint8_t s) -> std::pair<int, std::uint64_t> {
    if (v == 0) return {0, 0};
    auto digits = std::to_string(v);
    if (digits.size() > s) {
      // An integer part: its base-10000 digits, aligned at the point.
      auto whole = digits.substr(0, digits.size() - s);
      auto w = static_cast<int>((whole.size() - 1) / 4);
      return {w, std::stoull(whole.substr(0, whole.size() - static_cast<std::size_t>(w) * 4))};
    }
    // Only a fraction: its base-10000 digits after the point.
    auto frac = std::string(s - digits.size(), '0') + digits;
    frac.append((4 - frac.size() % 4) % 4, '0');
    for (std::size_t i = 0; i < frac.size(); i += 4) {
      auto d = std::stoull(frac.substr(i, 4));
      if (d != 0) return {-static_cast<int>(i / 4) - 1, d};
    }
    return {0, 0};
  };
  auto [w1, d1] = weight_of(num, scale);
  auto [w2, d2] = weight_of(den, 0);
  auto qweight = w1 - w2;
  if (d1 <= d2) --qweight;
  auto rscale = std::clamp(std::max(16 - qweight * 4, static_cast<int>(scale)), 0, 1000);
  // num / den's digits, the quotient's and then its fraction's, one past
  // rscale once moved `scale` places: the last one rounds.
  auto q = num / den;
  unsigned __int128 r = num % den;
  auto digits = std::to_string(q);
  for (auto i = 0; i < rscale - static_cast<int>(scale) + 1; ++i) {
    r *= 10;
    digits.push_back(static_cast<char>('0' + static_cast<int>(r / den)));
    r %= den;
  }
  auto up = digits.back() >= '5';
  digits.pop_back();
  for (auto i = digits.size(); up && i-- > 0;) {
    up = digits[i] == '9';
    digits[i] = up ? '0' : static_cast<char>(digits[i] + 1);
  }
  if (up) digits.insert(digits.begin(), '1');
  auto frac_digits = static_cast<std::size_t>(rscale);
  if (digits.size() <= frac_digits) digits.insert(0, frac_digits + 1 - digits.size(), '0');
  auto whole = digits.substr(0, digits.size() - frac_digits);
  whole.erase(0, std::min(whole.find_first_not_of('0'), whole.size() - 1));
  auto text = whole + (frac_digits > 0 ? "." + digits.substr(digits.size() - frac_digits) : "");
  auto zero = std::ranges::all_of(digits, [](char c) { return c == '0'; });
  return (sum < 0 && !zero ? "-" : "") + text;
}

namespace {

auto constraint_sqlstate(const ExecError& e) noexcept -> std::string_view {
  auto c = ConstraintDetail::decode(e.detail());
  switch (c.constraint) {
    case BC_C_UNIQUE: return "23505";
    case BC_C_NOT_NULL: return "23502";
    case BC_C_CHECK: return "23514";
    case BC_C_FOREIGN_KEY: return "23503";
    case BC_C_RANGE: {
      auto state = range_sqlstate(e.what());
      return state.empty() ? "22000" : state;  // data_exception: a range message out_of_range did not write
    }
    default: return "23000";  // integrity_constraint_violation
  }
}

auto exec_sqlstate(const ExecError& e) noexcept -> std::string_view {
  if (e.conflict()) return "40001";
  switch (e.code()) {
    case BC_E_CONSTRAINT: return constraint_sqlstate(e);
    case BC_E_LIMIT: return "54000";      // program_limit_exceeded
    case BC_E_ARITH: return "22003";      // numeric_value_out_of_range: the subset has no division
    case BC_E_BUDGET: return "57014";     // query_canceled
    case BC_E_MEMORY: return "53200";     // out_of_memory
    case BC_E_CLOSED: return "57P01";     // admin_shutdown
    case BC_E_IO:
    case BC_E_CORRUPT:
    case BC_E_DEGRADED: return "58030";   // io_error
    default: return "XX000";
  }
}

}  // namespace

auto sqlstate(const std::exception& e) noexcept -> std::string_view {
  // Most derived first: SerializationFailure is an ExecError.
  if (dynamic_cast<const SerializationFailure*>(&e) != nullptr) return "40001";
  if (const auto* x = dynamic_cast<const ExecError*>(&e)) return exec_sqlstate(*x);
  if (dynamic_cast<const TransactionResolutionUnknown*>(&e) != nullptr) return "08007";
  if (dynamic_cast<const TransactionAborted*>(&e) != nullptr) return "25P02";
  if (dynamic_cast<const FeatureNotSupported*>(&e) != nullptr) return "0A000";
  if (dynamic_cast<const ParamError*>(&e) != nullptr) return "08P01";
  if (const auto* x = dynamic_cast<const DdlRefused*>(&e)) return x->sqlstate();
  if (const auto* x = dynamic_cast<const ast::SqlError*>(&e)) return x->sqlstate();
  if (dynamic_cast<const DdlInProgress*>(&e) != nullptr) return "40001";
  if (dynamic_cast<const DbClosed*>(&e) != nullptr) return "57P01";
  if (dynamic_cast<const std::bad_alloc*>(&e) != nullptr) return "53200";
  return "XX000";
}

auto Statement::param_types() const -> std::vector<PhysType> {
  auto c = compiled();
  return c ? c->plan->param_types : std::vector<PhysType>{};
}

auto Statement::plan_source() const -> std::string {
  auto c = compiled();
  return c ? c->plan->source : std::string{};
}

auto Statement::params() const -> std::vector<ParamInfo> {
  auto c = compiled();
  return c ? c->params : std::vector<ParamInfo>{};
}

auto Statement::columns() const -> std::vector<ResultColumn> {
  auto c = compiled();
  std::vector<ResultColumn> out;
  if (!c || !c->plan->returns_rows) return out;
  for (std::size_t i = 0; i < c->columns.size(); ++i) out.push_back({c->columns[i], c->column_types.at(i)});
  return out;
}

auto Statement::command() const -> Command {
  return std::visit(
      [](const auto& st) {
        using T = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<T, ast::Select>) {
          return Command::Select;
        } else if constexpr (std::is_same_v<T, ast::Insert>) {
          return Command::Insert;
        } else if constexpr (std::is_same_v<T, ast::Update>) {
          return Command::Update;
        } else if constexpr (std::is_same_v<T, ast::Delete>) {
          return Command::Delete;
        } else if constexpr (std::is_same_v<T, ast::CreateTable>) {
          return Command::CreateTable;
        } else if constexpr (std::is_same_v<T, ast::CreateIndex>) {
          return Command::CreateIndex;
        } else if constexpr (std::is_same_v<T, ast::DropTable>) {
          return Command::DropTable;
        } else if constexpr (std::is_same_v<T, ast::DropIndex>) {
          return Command::DropIndex;
        } else {
          static_assert(std::is_same_v<T, ast::AddColumn>);
          return Command::AlterTable;
        }
      },
      ast_);
}

auto Database::open(const std::filesystem::path& dir, DatabaseOptions opts) -> std::unique_ptr<Database> {
  if (opts.sync != SyncMode::AtEveryCommit && opts.sync != SyncMode::AtInterval &&
      opts.sync != SyncMode::AtFileRotation) {
    throw std::invalid_argument("sync is not a SyncMode");  // a cast integer: refused, not a silent mode
  }
  if (opts.sync_interval_ms < 1 || opts.sync_interval_ms > 60'000) {
    throw std::invalid_argument("sync_interval_ms is 1 to 60000");
  }
  if (opts.statement_memory_bytes == 0) throw std::invalid_argument("statement_memory_bytes is at least 1");
  std::unique_ptr<Database> d{new Database};
  d->sync_commits_ = opts.sync == SyncMode::AtEveryCommit;
  d->budget_ = {.max_reads = 0, .max_bytes = opts.statement_memory_bytes, .deadline_ns = 0, .cancel = nullptr};
  d->life_ = std::make_shared<DatabaseLife>();
  bytecask::Options engine;
  engine.io_backend = opts.io_backend;
  engine.buffer_pool = opts.buffer_pool;
  d->db_.reset(new DB(DB::open(dir, engine)));
  d->live_ = std::make_unique<LiveCatalog>(*d->db_);
  d->bdb_ = {d->db_.get()};
  d->bcat_ = {d->live_.get()};
  d->release_context(d->acquire_context());  // the first, made now: open fails, not the first call
  if (d->idle_.empty()) throw std::bad_alloc();
  // Last, so nothing that can still fail leaves a thread running.
  if (opts.sync == SyncMode::AtInterval) {
    d->flusher_ = std::thread{[db = d.get(), ms = std::chrono::milliseconds{opts.sync_interval_ms}] { db->sync_loop(ms); }};
  }
  return d;
}

void Database::sync_loop(std::chrono::milliseconds interval) {
  std::unique_lock<std::mutex> lock{flush_mu_};
  auto failing = false;  // a failure is logged once, not once per interval
  while (!flush_cv_.wait_for(lock, interval, [this] { return flush_stop_; })) {
    lock.unlock();
    try {
      (void)db_->apply_batch(WriteOptions{.sync = true}, WritePlan{});
      failing = false;
    } catch (const std::exception& e) {
      if (!failing) std::fprintf(stderr, "bytecask sql: background sync failed: %s\n", e.what());
      failing = true;
      // A failed fdatasync degrades the engine: every later write fails, and
      // close() throws, naming the commits that are not durable. No sync can
      // succeed after it, so the flusher stops. Anything else is retried.
      if (db_->is_degraded()) {
        std::fprintf(stderr, "bytecask sql: the engine is degraded; background syncs stop\n");
        return;
      }
    }
    lock.lock();
  }
}

Database::~Database() {
  try {
    close();
  } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor cannot report; call close() to see errors
  }
}

auto Database::enter() const -> std::shared_lock<std::shared_mutex> {
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (life_->closed || life_->closing.load()) throw DbClosed();
  return lock;
}

void Database::close() {
  if (!life_) return;  // open() failed before it was made
  life_->closing.store(true);
  // Before the exclusive lock: a last sync in flight holds up no other call.
  // The flusher takes no lock a call holds.
  stop_flusher();
  std::unique_lock<std::shared_mutex> lock{life_->mu};
  if (life_->closed) return;
  life_->closed = true;
  // A transaction still open holds the engine's snapshot: ended first, with
  // nothing written, and every later call on it throws. No call is in
  // flight: each holds the lock shared.
  {
    std::lock_guard<std::mutex> open{life_->open_mu};
    for (auto* h : life_->open) {
      h->release();
      h->state.store(TxnState::Ended);
    }
    life_->open.clear();
  }
  {
    std::unique_lock<std::shared_mutex> cache{cache_mu_};  // cached_plans() reads it without entering
    cache_.clear();
  }
  for (auto* cx : idle_) bc_ctx_free(cx);
  idle_.clear();
  live_.reset();
  if (db_) db_->close();
  db_.reset();
}

void Database::stop_flusher() {
  std::lock_guard<std::mutex> once{flusher_stop_mu_};
  {
    std::lock_guard<std::mutex> stop{flush_mu_};
    flush_stop_ = true;
  }
  flush_cv_.notify_one();
  if (flusher_.joinable()) flusher_.join();
}

auto Database::acquire_context() -> bc_ctx* {
  bc_ctx* cx = nullptr;
  {
    std::lock_guard<std::mutex> lock{contexts_mu_};
    if (!idle_.empty()) {
      cx = idle_.back();
      idle_.pop_back();
    }
  }
  if (cx == nullptr) cx = bc_ctx_new(&bdb_, &bcat_);
  if (cx == nullptr) throw std::bad_alloc();
#ifdef BYTECASK_SQL_TESTING
  if (!conflict_every_commit_.load()) {
    bc_ctx_before_commit_for_testing(cx, {});
  } else {
    bc_ctx_before_commit_for_testing(cx, [this] {
      auto cat = live_->current();  // held: the loop reads its tables
      for (const auto& [id, t] : cat->tables()) {
        auto key = table_record_key(id);
        Bytes record;
        if (db_->get({}, key, record)) (void)db_->put({}, key, record);
      }
    });
  }
#endif
  return cx;
}

void Database::release_context(bc_ctx* cx) noexcept {
  std::lock_guard<std::mutex> lock{contexts_mu_};
  try {
    idle_.push_back(cx);
  } catch (const std::bad_alloc&) {
    bc_ctx_free(cx);  // not kept: the next call makes one
  }
}

auto Database::catalog() const -> std::shared_ptr<const Catalog> {
  auto lock = enter();
  return live_->current();
}

auto Database::row_counts() const -> RowCounts {
  // One snapshot for every table the search counts. Each count is
  // bc_count_range's: at most two record reads, whatever the table holds.
  auto snap = std::make_shared<Snapshot>(db_->snapshot());
  return [snap](const Table& t) -> std::optional<std::uint64_t> {
    auto lo = row_prefix(t.id);
    auto hi = key_successor(lo);
    return snap->count_keys(lo, hi, std::numeric_limits<std::size_t>::max());
  };
}

auto Database::build(const Statement& s) -> std::shared_ptr<const Compiled> {
  auto cat = live_->current();
  auto bound = bound::bind(s.ast_, *cat);
  // DDL has no plan.
  if (std::holds_alternative<Ddl>(bound.stmt)) return nullptr;
  auto c = std::make_shared<Compiled>();
  c->param_sql = std::move(bound.param_sql);
  c->param_table = std::move(bound.param_table);
  c->params = std::move(bound.params);
  c->plan = std::make_unique<GeneratedPlan>(generate_plan(std::move(bound), *cat, s.sql_, row_counts(), budget_.max_bytes));
  link(*c);
  ++compilations_;
  return c;
}

void Database::link(Compiled& compiled) {
  auto* c = &compiled;
  c->columns = c->plan->result_names;
  c->column_types = c->plan->result_sql;
  c->result_codes = result_type_codes(c->plan->result_types);
  c->result_layout = make_result_layout(c->plan->result_types);
  {
    std::lock_guard<std::mutex> lock{compile_mu_};
    c->code = TccPlan::compile(c->plan->source);
  }
  for (const auto& col : c->plan->columns) {
    c->col_refs.push_back({col.table, col.col, static_cast<std::uint8_t>(col.type)});
  }
  auto n = [](const auto& v) { return narrow<std::uint32_t>(v.size()); };
  const auto& p = *c->plan;
  c->ref = {p.catalog_version,
            p.objects_read.data(),
            n(p.objects_read),
            p.tables_written.data(),
            n(p.tables_written),
            c->col_refs.data(),
            n(c->col_refs),
            c->result_codes.data(),
            n(c->result_codes)};
  c->checked.store(p.catalog_version);
}

auto Database::recompile(const Statement& stmt) -> std::shared_ptr<const Compiled> {
  auto c = build(stmt);
  stmt.set_compiled(c);
  return c;
}

auto Database::prepare(std::string_view sql) -> const Statement& {
  auto lock = enter();
  return prepare_locked(sql);
}

auto Database::cached(std::string_view sql) const -> bool {
  std::shared_lock<std::shared_mutex> lock{cache_mu_};
  return cache_.contains(sql);
}

auto Database::prepare_locked(std::string_view sql) -> const Statement& {
  {
    std::shared_lock<std::shared_mutex> lock{cache_mu_};
    if (auto it = cache_.find(sql); it != cache_.end()) return *it->second;
  }
  // Parsed and compiled outside the cache's lock: a statement another
  // thread added meanwhile is kept, and this one dropped.
  auto s = std::make_unique<Statement>();
  s->sql_ = std::string{sql};
  s->ast_ = ast::parse(sql);
  s->compiled_ = build(*s);
  s->ddl_ = s->compiled_ == nullptr;
  s->owner_ = this;
  std::unique_lock<std::shared_mutex> lock{cache_mu_};
  return *cache_.try_emplace(s->sql_, std::move(s)).first->second;
}

auto Database::execute(std::string_view sql, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows {
  return execute(prepare(sql), params, opts);
}

auto Database::execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows {
  auto lock = enter();
  if (stmt.owner_ != this) throw std::logic_error("the statement was prepared on another database");
  if (stmt.is_ddl()) {
    if (!params.empty()) throw ParamError("DDL takes no parameters");
    auto bound = bound::bind(stmt.ast_, *live_->current());
    try {
      (void)live_->apply(std::get<Ddl>(bound.stmt));
    } catch (const ConstraintViolation& e) {
      throw ExecError(e.what(), BC_E_CONSTRAINT, e.detail().encode());
    } catch (const DdlInProgress&) {
      throw_serialization_failure(kDdlInProgress);  // a DDL never waits for another
    } catch (const CommitOutcomeUnknown&) {
      throw TransactionResolutionUnknown{};
    }
    Rows out;
    out.affected = 0;
    return out;
  }
  auto* cx = acquire_context();
  try {
    auto out = run(cx, stmt, params, nullptr, opts);
    release_context(cx);
    return out;
  } catch (...) {
    release_context(cx);
    throw;
  }
}

auto Database::fitting_plan(bc_ctx* cx, const Statement& stmt, const std::vector<Value>& params)
    -> std::shared_ptr<const Compiled> {
  // The runtime says whether the cached plan still fits the catalog
  // (bc_ctx_check, the verification bc_ctx_run makes); it is recompiled only
  // if not. The runtime alone decides compatibility. A plan that fits is
  // marked with the version it was checked against, so it is checked once
  // per catalog version, not on every run after a DDL. The runtime pins that
  // version or a later one: a later one is checked again next time.
  auto c = stmt.compiled();
  auto version = live_->current()->version();
  if (c->checked.load() == version) return c;
#ifdef BYTECASK_SQL_TESTING
  ++plan_checks_;
#endif
  bc_params bp{params};
  bc_ctx_begin(cx, &bp, budget_);
  auto fits = bc_ctx_check(cx, &c->ref);
  auto stale = !fits && bc_error(cx) == BC_E_LAYOUT && bc_error_detail(cx) == BC_L_INCOMPATIBLE;
  if (!fits && !stale) throw_context_error(cx);
  bc_ctx_reset(cx);
  if (stale) return recompile(stmt);
  // Marked with the version it was verified against: the context pinned it
  // between the two reads, so only when no DDL moved the version meanwhile.
  if (live_->current()->version() == version) c->checked.store(version);
  return c;
}

// A value outside its parameter's SQL type is refused before the plan runs,
// as PostgreSQL refuses a $n it cannot coerce at bind: even one a plan would
// not read (a key whose other part is NULL). The parameters are checked
// against the plan that will run, and so against the catalog it was
// verified against: a DDL since the plan was compiled may have changed what
// they are (an INTEGER key recreated as BIGINT).
void Database::check_params(const Compiled& c, const std::vector<Value>& params) {
  const auto& types = c.plan->param_types;
  if (params.size() != types.size()) {
    throw ParamError("statement takes " + std::to_string(types.size()) + " parameters, got " +
                     std::to_string(params.size()));
  }
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (!datum_fits(params[i], types[i])) throw ParamError("parameter $" + std::to_string(i + 1) + " has the wrong type");
  }
  for (std::size_t i = 0; i < params.size(); ++i) {
    const auto& sql = c.param_sql.at(i);
    if (!sql) continue;  // assigned only: the row builder checks it, in column order
    if (auto range = out_of_range(*sql, params[i])) {
      throw ExecError(*range + " for parameter $" + std::to_string(i + 1), BC_E_CONSTRAINT,
                      ConstraintDetail{BC_C_RANGE, c.param_table.at(i)}.encode());
    }
  }
}

auto Database::budget_for(const ExecutionOptions& opts) const noexcept -> bc_budget {
  bc_budget b = budget_;
  // Counted from the steady clock's epoch, as the runtime reads it; 0 is none.
  std::int64_t ns = std::chrono::nanoseconds{opts.deadline.time_since_epoch()}.count();
  if (ns != 0) b.deadline_ns = ns > 0 ? static_cast<std::uint64_t>(ns) : 1;
  if (opts.cancel != nullptr) b.cancel = &opts.cancel->v_;
  return b;
}

auto Database::run(bc_ctx* cx, const Statement& stmt, const std::vector<Value>& params, bc_txn* txn,
                   const ExecutionOptions& opts) -> Rows {
  bc_params bp{params};
  auto budget = budget_for(opts);
  if (txn != nullptr) {
    // The plan must fit the catalog the transaction pinned at its first
    // statement. One that does not is recompiled when the pin is the live
    // catalog (the cached plan is merely behind it); is pinned again when
    // this is the first statement, which has read nothing (PostgreSQL takes
    // the snapshot as the first statement starts); and is otherwise a DDL
    // since the transaction began: 40001, as a commit after it would be.
    for (auto attempt = 0;; ++attempt) {
      bc_ctx_begin_txn(cx, txn, &bp, budget);
      auto pinned = bc_txn_catalog_version(txn);
#ifdef BYTECASK_SQL_TESTING
      if (after_txn_pin_) after_txn_pin_();
#endif
      auto c = stmt.compiled();  // taken after the pin: the plan of a DDL that raced it, if one did
      // A plan that does not fit the pin has run nothing: bc_ctx_run
      // verifies it before the plan's body. Recompiled, pinned again, or
      // 40001.
      auto stale = [&] {
        bc_ctx_reset(cx);
        if (attempt < kTxnPlanAttempts) {
          if (pinned == live_->current()->version()) {
            (void)recompile(stmt);
            return;
          }
          if (bc_txn_repin(txn)) return;
        }
        throw_serialization_failure(kCatalogChanged);
      };
      auto incompatible = [&] { return bc_error(cx) == BC_E_LAYOUT && bc_error_detail(cx) == BC_L_INCOMPATIBLE; };
      try {
        check_params(*c, params);
      } catch (...) {
        // Parameters a stale plan refuses may fit the plan it recompiles
        // to: only then is it checked before running.
        if (!bc_ctx_check(cx, &c->ref) && incompatible()) {
          stale();
          continue;
        }
        bc_ctx_reset(cx);
        throw;
      }
      bc_ctx_run(cx, c->code->fn(), &c->ref);
      if (incompatible()) {
        stale();
        continue;
      }
      // The statement's writes join the transaction, or it aborts.
      if (!bc_txn_add(cx)) throw_context_error(cx);
      return results(cx, *c, Rows{});
    }
  }
  auto c = fitting_plan(cx, stmt, params);
  check_params(*c, params);
  auto r = bc_execute(cx, c->code->fn(), &c->ref, &bp, budget, sync_commits_, kMaxRetries);
  // bc_ctx_run checks the plan against the catalog it pinned. A plan a DDL
  // made incompatible (BC_E_LAYOUT with BC_L_INCOMPATIBLE) is recompiled
  // from its text and run once more; one a DDL left compatible keeps
  // running, unchecked here.
  if (r == BC_FAILED && bc_error(cx) == BC_E_LAYOUT && bc_error_detail(cx) == BC_L_INCOMPATIBLE) {
    bc_ctx_reset(cx);
    c = recompile(stmt);
    check_params(*c, params);
    r = bc_execute(cx, c->code->fn(), &c->ref, &bp, budget, sync_commits_, kMaxRetries);
  }
  if (r == BC_CONFLICT) {
    bc_ctx_reset(cx);
    throw_keeping_outcome("conflict: a guard failed on every attempt",
                          [](const std::string& m) { return ExecError::conflicted(m); });
  }
  if (r == BC_FAILED) throw_context_error(cx);
  return results(cx, *c, Rows{});
}

// The rows and count the statement the context ran left, decoded; the
// context is reset either way.
auto Database::results(bc_ctx* cx, const Compiled& c, Rows out) -> Rows {
  const auto& p = *c.plan;
  out.affected = bc_ctx_affected(cx);
  if (p.returns_rows) {
    out.columns = c.columns;
    out.types = c.column_types;
    const std::uint8_t* row = nullptr;
    std::uint32_t len = 0;
    while (bc_ctx_next_result(cx, &row, &len)) {
      // Result rows are decoded after the commit. That is safe while the
      // compiled write plans emit no rows (only bc_emit_count): a RETURNING
      // will have to decode its rows before it commits.
      auto values = decode_row(c.result_layout, std::as_bytes(std::span{row, len}));
      if (!values) {
        bc_ctx_reset(cx);
        throw ExecError("malformed result row: column " + std::to_string(values.error()), BC_E_CORRUPT);
      }
      out.rows.push_back(quotients(p, std::move(*values)));
    }
  }
  bc_ctx_reset(cx);
  return out;
}

// A result row's NUMERICs, each its plan's columns (physical::plan_columns),
// its sum and count, as one: NULL, or its text.
auto Database::quotients(const GeneratedPlan& p, std::vector<Value> values) -> std::vector<Value> {
  auto numeric = [](const SqlType& t) { return t.kind == SqlKind::Numeric; };
  if (std::ranges::none_of(p.result_sql, numeric)) return values;
  std::vector<Value> out;
  std::size_t at = 0;
  for (const auto& t : p.result_sql) {
    const auto first = std::exchange(at, at + physical::plan_columns(t).size());
    if (!numeric(t)) {
      out.push_back(std::move(values[first]));
      continue;
    }
    const auto* sum = std::get_if<std::int64_t>(&values[first]);
    const auto* count = std::get_if<std::int64_t>(&values[first + 1]);
    if (sum == nullptr || count == nullptr || *count <= 0) {
      out.emplace_back(std::monostate{});
      continue;
    }
    auto text = average_text(*sum, *count, t.scale);
    auto b = std::as_bytes(std::span{text.data(), text.size()});
    out.emplace_back(Bytes(b.begin(), b.end()));
  }
  return out;
}

void Database::refit(const Statement& stmt) {
  auto lock = enter();
  if (stmt.owner_ != this) throw std::logic_error("the statement was prepared on another database");
  if (stmt.is_ddl()) return;
  auto* cx = acquire_context();
  try {
    (void)fitting_plan(cx, stmt, {});
    release_context(cx);
  } catch (...) {
    release_context(cx);
    throw;
  }
}

auto Database::begin(TransactionOptions opts) -> Transaction {
  auto lock = enter();
  return Transaction{*this, opts};
}

Transaction::Transaction(Database& db, TransactionOptions opts)
    : life_{db.life_}, db_{&db}, h_{std::make_unique<TxnHandle>()} {
  h_->cx = bc_ctx_new(&db.bdb_, &db.bcat_);
  h_->txn = h_->cx == nullptr ? nullptr : bc_txn_new(&db.bdb_, &db.bcat_, opts.max_bytes);
  if (h_->txn == nullptr) {
    h_->release();
    throw std::bad_alloc();
  }
  try {
    std::lock_guard<std::mutex> lock{life_->open_mu};
    life_->open.insert(h_.get());
  } catch (...) {
    h_->release();
    throw;
  }
  h_->state.store(TxnState::Open);
}

Transaction::Transaction(Transaction&& other) noexcept = default;

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this == &other) return *this;
  rollback();
  life_ = std::move(other.life_);
  db_ = std::exchange(other.db_, nullptr);
  h_ = std::move(other.h_);
  return *this;
}

Transaction::~Transaction() { rollback(); }

void Transaction::end_locked() noexcept {
  {
    std::lock_guard<std::mutex> lock{life_->open_mu};
    life_->open.erase(h_.get());
  }
  h_->release();
  h_->state.store(TxnState::Ended);
}

void Transaction::rollback() noexcept {
  if (!h_) return;
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (h_->state.load() == TxnState::Ended) return;  // ended already, or by close()
  end_locked();
}

void Transaction::check_usable() const {
  if (life_->closed || life_->closing.load()) throw DbClosed();
  auto state = h_->state.load();
  if (state == TxnState::Ended) throw std::logic_error("the transaction has ended");
  if (state == TxnState::Aborted) throw TransactionAborted();
}

auto Transaction::run_locked(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  try {
    // DDL is its own atomic operation, never part of a transaction.
    if (stmt.is_ddl()) throw FeatureNotSupported("DDL inside a transaction is not supported: run it outside one");
    return db_->run(h_->cx, stmt, params, h_->txn, opts);
  } catch (const SerializationFailure&) {
    end_locked();
    throw;
  } catch (...) {
    // Every other error aborts the transaction, as in PostgreSQL: a
    // statement refused before it ran too.
    h_->abort();
    throw;
  }
}

auto Transaction::execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  // A misuse of the handle, not a failed statement: the transaction stays as it was.
  if (stmt.owner_ != db_) throw std::logic_error("the statement was prepared on another database");
  return run_locked(stmt, params, opts);
}

auto Transaction::execute(std::string_view sql, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (h_->state.load() == TxnState::Aborted && !life_->closed && !db_->cached(sql)) {
    // As in PostgreSQL, a statement that does not parse reports that, not
    // 25P02: the text is parsed before the transaction's state is checked.
    (void)ast::parse(sql);
  }
  check_usable();
  const Statement* stmt = nullptr;
  try {
    stmt = &db_->prepare_locked(sql);
  } catch (...) {
    h_->abort();  // a statement that does not parse or bind aborts it too
    throw;
  }
  return run_locked(*stmt, params, opts);
}

auto Transaction::commit() -> CommitOutcome {
  if (!h_) throw std::logic_error("the transaction has ended");
  // Shared, as every call: commits on different transactions run together
  // and share the engine's group commit. The commit ends the transaction,
  // whatever the outcome, and the outcome is fixed before any message is
  // built.
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (life_->closed || life_->closing.load()) throw DbClosed();
  auto state = h_->state.load();
  if (state == TxnState::Ended) throw std::logic_error("the transaction has ended");
  if (state == TxnState::Aborted) {
    end_locked();
    return CommitOutcome::RolledBack;
  }
  auto r = bc_txn_commit(h_->txn, db_->sync_commits_);
  auto code = bc_txn_error(h_->txn);
  auto detail = bc_txn_error_detail(h_->txn);
  std::string message;
  if (r == BC_FAILED && code != BC_E_UNKNOWN) {  // its exception's message is static
    try {
      message = bc_txn_error_message(h_->txn);
    } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
    }
  }
  end_locked();
  lock.unlock();
  if (r == BC_CONFLICT) throw_serialization_failure(kConflict);
  if (r == BC_FAILED) throw_error(code, detail, message);
  return CommitOutcome::Committed;
}

#ifdef BYTECASK_SQL_TESTING
void Database::conflict_on_every_commit_for_testing(bool on) {
  auto lock = enter();
  conflict_every_commit_.store(on);  // each autocommit context takes it up when it is next taken
}

auto Database::alternatives_for_testing(std::string_view sql, const std::vector<Value>& params) -> std::vector<Rows> {
  auto lock = enter();
  auto cat = live_->current();
  auto bound = bound::bind(ast::parse(sql), *cat);
  if (std::holds_alternative<Ddl>(bound.stmt)) throw std::logic_error("DDL has no plan");
  auto param_sql = bound.param_sql;
  auto param_table = bound.param_table;
  std::vector<Rows> out;
  for (auto& plan : generate_alternatives(std::move(bound), *cat, sql, row_counts(), budget_.max_bytes)) {
    if (!plan.returns_rows) throw std::logic_error("alternatives of a statement that writes");
    Compiled c;
    c.param_sql = param_sql;
    c.param_table = param_table;
    c.plan = std::make_unique<GeneratedPlan>(std::move(plan));
    check_params(c, params);
    link(c);
    auto* cx = acquire_context();
    try {
      bc_params bp{params};
      auto r = bc_execute(cx, c.code->fn(), &c.ref, &bp, budget_, sync_commits_, kMaxRetries);
      if (r == BC_CONFLICT) throw ExecError::conflicted("conflict: a guard failed on every attempt");
      if (r == BC_FAILED) throw_context_error(cx);
      out.push_back(results(cx, c, Rows{}));
      release_context(cx);
    } catch (...) {
      bc_ctx_reset(cx);
      release_context(cx);
      throw;
    }
  }
  return out;
}

auto Database::table_rows(std::string_view table) const -> Rows {
  auto lock = enter();
  auto [cat, snap] = live_->pin();
  const auto* t = cat->table(table);
  if (t == nullptr) throw ast::SqlError("relation \"" + std::string{table} + "\" does not exist", -1, kUndefinedTable);
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
