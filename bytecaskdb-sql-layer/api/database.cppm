// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// database.cppm — the embedded SQL API: open a directory, prepare a statement,
// execute it with parameters, read the rows. Every DML statement is compiled
// once to a plan, by TinyCC, and cached by its text; after any DDL a cached
// plan is bound again at its next run (docs/catalog_design.md,
// *Compatibility*). A transaction binds only against the catalog it pinned
// (docs/transaction_design.md, *A transaction binds only against its own
// catalog*), compiling privately what the cache has not bound there.
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
#include <exception>
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
export import bytecask.sql.procedure;
import bytecask.sql.parser;

export namespace bytecask::sql {

using Value = bc_value;  // NULL, BIGINT, DOUBLE PRECISION, BOOLEAN, BYTEA

// The errors the API throws that other modules declare: the catalog's
// refusal of a DDL statement, and the engine's errors for a call after
// close() and for a vacuum on a degraded engine.
using bytecask::sql::DdlRefused;
using bytecask::DbClosed;
using bytecask::DbDegraded;

// Database::vacuum's options, the engine's.
using bytecask::VacuumOptions;

// What a statement reports besides its result, as PostgreSQL's NOTICE: a
// DDL that IF EXISTS or IF NOT EXISTS skipped, with PostgreSQL's SQLSTATE
// and text.
struct Notice {
  std::string sqlstate;
  std::string message;
};

struct Rows {
  std::vector<std::string> columns;
  // Each column's SQL type, as the catalog declares it. A NUMERIC's value,
  // an AVG's, is its text, as bytes: more digits than an int64_t holds.
  std::vector<SqlType> types;
  std::vector<std::vector<Value>> rows;
  // INSERT, UPDATE, DELETE: rows changed. CREATE TABLE AS and SELECT …
  // INTO: rows written, -1 when the query did not run (WITH NO DATA, or IF
  // NOT EXISTS skipped the statement).
  std::int64_t affected{-1};
  std::vector<Notice> notices;
};

// Which of a read statement's rows a run returns: the first `skip` are
// dropped and the next `limit` kept (docs/cursor_design.md, *The replay
// cursor*).
struct RowWindow {
  std::uint64_t skip{0};
  std::uint64_t limit{std::numeric_limits<std::uint64_t>::max()};
};

// A windowed run's rows and where it stopped. `position` is the rows the
// plan ended, dropped ones included; `end` is true when the plan ran out of
// rows before the window was full, and `position` is then the whole result's
// size. A run that kept exactly `limit` rows has not found out whether more
// follow: `end` is false.
struct WindowRows {
  Rows rows;
  std::uint64_t position{0};
  bool end{false};
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
// The HINT PostgreSQL reports with the error, empty for none: an
// ast::SqlError's own, and the one PostgreSQL gives a runtime error.
[[nodiscard]] auto error_hint(const std::exception& e) noexcept -> std::string_view;

class Database;

// What a statement does, as PostgreSQL's command tag names it.
enum class Command {
  Select,
  Insert,
  Update,
  Delete,
  CreateTable,
  CreateTableAs,  // CREATE TABLE … AS
  SelectInto,     // SELECT … INTO: CREATE TABLE AS, written as a SELECT
  CreateIndex,
  DropTable,
  DropIndex,
  AlterTable,
  CreateView,
  DropView,
  CreateProcedure,
  DropProcedure,
  CreateRole,
  DropRole,
  Grant,
  Revoke,
  CreateDomain,
  CreateType,
  CreateSequence,
  DropDomain,
  DropType,
  DropSequence,
};

// The command's name, as PostgreSQL's messages give it: "CREATE TABLE".
[[nodiscard]] auto command_name(Command c) -> std::string_view;

// A result column: its name and SQL type, as a client is told them before
// the statement runs.
struct ResultColumn {
  std::string name;
  SqlType type;
  auto operator==(const ResultColumn&) const -> bool = default;
};

using bound::ParamInfo;

// What a prepared statement fixed when it was described (docs/transaction_design.md,
// *A transaction binds only against its own catalog*): its parameters' types,
// its result columns (none for a statement that returns no rows), and the
// catalog version of the plan that fixed them. A refit gives one; the server
// keeps one per named statement, from Parse.
struct StatementRecord {
  std::vector<ParamInfo> params;
  std::vector<ResultColumn> columns;
  Command command{Command::Select};
  bool ddl{false};  // runs no plan: a DDL, CREATE TABLE AS, SELECT … INTO
  std::uint64_t version{0};
  // Fixed at a transaction's private catalog, after its own DDL: `version`
  // is then the published catalog that transaction was based on, and its
  // columns may be those of a catalog later than it, the one the
  // transaction published at its commit (docs/transaction_design.md,
  // *Savepoints, cursors, portals and prepared statements across DDL*).
  bool private_catalog{false};
  // An UPDATE or DELETE ... WHERE CURRENT OF: the cursor it names, and the
  // table it writes; empty for any other statement.
  std::string current_of;
  std::string current_of_table;
};

// How a run by text treats a statement's result columns and parameter types.
// None: run by text, as execute(sql) runs it: the parameter types are
// inferred and the plan's columns returned, whatever they are. Given: as a
// statement prepared earlier, whose record the caller kept (the server's,
// from Parse): it binds with the record's parameter types, and result
// columns other than the record's are 0A000 "cached plan must not change
// result type", or 40001 in a transaction pinned before the record's
// version. A held Statement is the third form, "own": the Statement is its
// own record.
struct ResultCheck {
  enum class Kind : std::uint8_t { None, Given };
  Kind kind{Kind::None};
  StatementRecord record;
  [[nodiscard]] static auto none() -> ResultCheck { return {}; }
  [[nodiscard]] static auto given(StatementRecord r) -> ResultCheck { return {Kind::Given, std::move(r)}; }
};

class TxnPlan;
class PlanRef;
struct CompiledProcedure;

class Statement {
public:
  [[nodiscard]] auto sql() const -> const std::string& { return sql_; }
  [[nodiscard]] auto is_ddl() const -> bool { return ddl_; }
  [[nodiscard]] auto command() const -> Command;
  // The command a parsed statement is.
  [[nodiscard]] static auto command_of(const ast::Statement& st) -> Command;
  // Of the plan the statement runs now.
  [[nodiscard]] auto param_types() const -> std::vector<PhysType>;
  // Each parameter's SQL type and use, and the result's columns (none for
  // a statement that returns no rows), as the statement fixed them when it
  // was prepared: a rebind keeps both, or is refused. Empty for DDL.
  [[nodiscard]] auto params() const -> std::vector<ParamInfo>;
  [[nodiscard]] auto columns() const -> std::vector<ResultColumn>;
  // The plan file, as EXPLAIN would print it; empty for DDL.
  [[nodiscard]] auto plan_source() const -> std::string;
  // Of the plan the statement runs now: a cursor over it may resume from a
  // checkpoint rather than replay from the first row. False for DDL and for
  // a statement that returns no rows.
  [[nodiscard]] auto is_resumable() const -> bool;
  // The cursor an UPDATE or DELETE ... WHERE CURRENT OF names; "" for any
  // other statement. Run it with Transaction::execute_current_of.
  [[nodiscard]] auto current_of() const -> std::string_view;
  // The table such a statement writes; "" for any other.
  [[nodiscard]] auto current_of_table() const -> std::string_view;

private:
  friend class Database;
  friend class Transaction;
  friend struct CursorState;
  friend class TxnPlan;
  friend class PlanRef;
  friend struct CompiledProcedure;
  // One compile of a DML statement, never changed once built. A run holds
  // it by shared_ptr, so a rebind, which swaps in a new one, never changes a
  // plan a run on another thread is using. Neither copied nor moved: `ref`
  // points into it. A transaction's private plan is one inside a TxnPlan,
  // never a shared_ptr, so set_compiled cannot take it.
  struct Compiled {
    Compiled() = default;
    Compiled(const Compiled&) = delete;
    Compiled& operator=(const Compiled&) = delete;
    Compiled(Compiled&&) = delete;
    Compiled& operator=(Compiled&&) = delete;
    ~Compiled() = default;
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
    // The statement's constants, folded when it was compiled
    // (bound::Bound::fixed): bound after the caller's parameters, as the
    // plan's last ones. A client never sees or sends them.
    std::vector<Value> fixed;
    // Which of `fixed` are the time (bound::Bound::clocks): filled on every
    // run from the statement's time, never from the compile.
    std::vector<bound::ClockSlot> clocks;
    // A cursor's query (Database::prepare_cursor_query): the trailing
    // `key_columns` values of each result row are the primary key of the
    // table it scans, `key_table`, kept by the cursor and shown to no one
    // (`columns` and `column_types` leave them out). 0 for a query that is no
    // simple scan of one table. An UPDATE or DELETE ... WHERE CURRENT OF: the
    // trailing `key_params` parameters are that table's key, filled by the
    // cursor's current row, and `key_table` the table; `params` lists them
    // but Statement::params() does not.
    std::uint32_t key_columns{0};
    std::uint32_t key_table{0};
    std::uint32_t key_params{0};
    // The views the plan was expanded through (bound::Bound::views): it is
    // rebuilt after any DDL, which may have changed what they read.
    std::vector<std::uint32_t> views;
    std::vector<std::uint8_t> result_codes;  // the plan's result_types as bc_type codes
    RowLayout result_layout;                 // how its result rows decode
    std::vector<bc_col_ref> col_refs;
    bc_schema_ref ref{};  // points into the members above; catalog_version: the one compiled against
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
  std::string key_;  // its key in the cache: the text, "\x01" + the text for a cursor's keyed twin
  ast::Statement ast_;
  bool ddl_{false};
  // CREATE TABLE AS and SELECT … INTO: the query, prepared with the
  // statement. Its parameters are the statement's. Null for any other.
  std::unique_ptr<Statement> query_;
  // The data exception (22xxx) folding the query's constants threw at
  // prepare, as 1/0 does: PostgreSQL folds them when it plans the query,
  // after it has looked for the table, so it is thrown when the statement
  // runs, after that check. query_'s plan is then a stand-in, the constant
  // NULL, that gives the statement's parameters and never runs.
  std::exception_ptr query_error_;
  const Database* owner_{nullptr};  // the Database that prepared it, the only one it runs on
  mutable std::mutex mu_;                             // held only to copy or swap compiled_
  // Null for DDL. Swapped only for a plan of the same parameter types and
  // result columns (a rebind at a later catalog), so it is also the
  // statement's record: what it fixed, and the version that fixed it.
  mutable std::shared_ptr<const Compiled> compiled_;
};

// A stored procedure compiled against one catalog: in the database's shared
// map (Database::procedure_plan) or a transaction's private one.
struct CompiledProcedure {
  std::uint32_t id{0};
  std::uint64_t catalog_version{0};
  GeneratedProcedure gen;
  std::vector<std::shared_ptr<Statement::Compiled>> stmts;
  std::unique_ptr<TccPlan> code;
};

// A plan a transaction compiled against its own catalog (docs/transaction_design.md,
// *Mechanism: plans compiled against the transaction's catalog*): it owns
// the plan, the catalog it was built for and the statement's parse, and is
// kept in the transaction's plan map. Its Compiled is a member, never a
// shared_ptr, so no code can put it in the shared cache.
class TxnPlan {
public:
  TxnPlan(const TxnPlan&) = delete;
  TxnPlan& operator=(const TxnPlan&) = delete;
  TxnPlan(TxnPlan&&) = delete;
  TxnPlan& operator=(TxnPlan&&) = delete;
  ~TxnPlan() = default;
  [[nodiscard]] auto catalog() const noexcept -> const std::shared_ptr<const Catalog>& { return catalog_; }
  // What it is charged to TransactionOptions::max_bytes.
  [[nodiscard]] auto estimate() const noexcept -> std::uint64_t { return estimate_; }

private:
  friend class Database;
  friend class Transaction;
  friend class PlanRef;
  TxnPlan(std::shared_ptr<const Catalog> cat, std::uint64_t base, bool is_private)
      : catalog_{std::move(cat)}, version_{base}, private_{is_private} {}
  [[nodiscard]] auto compiled() const -> const Statement::Compiled& { return *compiled_; }
  std::shared_ptr<const Catalog> catalog_;
  // The published catalog it was compiled at: the transaction's pin, also
  // after the transaction's own DDL, when `private_` says its catalog is
  // the transaction's private one.
  std::uint64_t version_{0};
  bool private_{false};
  std::shared_ptr<const ast::Statement> parse_;
  std::optional<Statement::Compiled> compiled_;  // a statement's
  std::optional<CompiledProcedure> procedure_;   // a procedure's
  bool inferred_{true};  // its parameter types are inferred, not a record's
  std::uint64_t estimate_{0};
};

// The plan a run or a cursor holds: the shared one of a Statement, or a
// transaction's private TxnPlan. Either keeps its plan alive, and its
// catalog, for as long as it is held.
class PlanRef {
public:
  PlanRef() = default;
  explicit PlanRef(std::shared_ptr<const Statement::Compiled> c) : shared_{std::move(c)} {}
  explicit PlanRef(std::shared_ptr<const TxnPlan> p) : private_{std::move(p)} {}
  [[nodiscard]] explicit operator bool() const noexcept { return shared_ || private_; }
  // What holding it adds to a cursor's reservation: a private plan's estimate.
  [[nodiscard]] auto private_bytes() const noexcept -> std::uint64_t { return private_ ? private_->estimate() : 0; }
  void reset() noexcept {
    shared_.reset();
    private_.reset();
  }

private:
  friend class Database;
  friend class Transaction;
  friend class Cursor;
  friend struct CursorState;
  [[nodiscard]] auto get() const -> const Statement::Compiled& { return shared_ ? *shared_ : private_->compiled(); }
  std::shared_ptr<const Statement::Compiled> shared_;
  std::shared_ptr<const TxnPlan> private_;
};

// A statement Transaction::prepare described (docs/transaction_design.md,
// *Mechanism: plans compiled against the transaction's catalog*): its text
// and its record, as the server's named statement holds them. It holds no
// plan, so it outlives its transaction: run with check() through any
// transaction's or the Database's execute forms that take a ResultCheck, it
// resolves its text where it runs, keeping its parameter types and result
// columns. A text naming a table its transaction created and did not commit
// is then 42P01.
class TxnStatement {
public:
  [[nodiscard]] auto sql() const -> const std::string& { return sql_; }
  [[nodiscard]] auto record() const -> const StatementRecord& { return record_; }
  [[nodiscard]] auto check() const -> ResultCheck { return ResultCheck::given(record_); }

private:
  friend class Transaction;
  TxnStatement(std::string sql, StatementRecord record) : sql_{std::move(sql)}, record_{std::move(record)} {}
  std::string sql_;
  StatementRecord record_;
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
  void cancel() noexcept { byte().store(1, std::memory_order_relaxed); }
  void reset() noexcept { byte().store(0, std::memory_order_relaxed); }
  [[nodiscard]] auto cancelled() const noexcept -> bool { return byte().load(std::memory_order_relaxed) != 0; }

private:
  friend class Database;
  // Every access from C++ goes through here, so none is a plain read.
  [[nodiscard]] auto byte() const noexcept -> std::atomic_ref<std::uint8_t> { return std::atomic_ref<std::uint8_t>{v_}; }
  // A plain byte, so the C runtime can read it (bc_budget::cancel) with an
  // atomic load. Mutable because std::atomic_ref has no const form before
  // C++26, and cancelled() only loads.
  alignas(std::atomic_ref<std::uint8_t>::required_alignment) mutable std::uint8_t v_{0};
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
class Cursor;

// Whether a cursor outlives the transaction that opened it.
enum class Holdability : std::uint8_t {
  Plain,  // closed with the transaction
  Hold,   // survives its commit (WITH HOLD); dropped by its rollback
};

// Whether a cursor may move backward (DECLARE NO SCROLL says it may not).
enum class Scrolling : std::uint8_t {
  Any,
  ForwardOnly,
};

// A FETCH or MOVE direction, as PostgreSQL's parser gives it: `count` is the
// row count for Forward and Backward (kAll for ALL; negative reverses the
// direction), the target row for Absolute (negative counts from the end) and
// the offset for Relative. FETCH 0 is Forward with count 0.
struct CursorMove {
  enum class Kind : std::uint8_t { Forward, Backward, Absolute, Relative };
  static constexpr std::int64_t kAll = std::numeric_limits<std::int64_t>::max();
  Kind kind{Kind::Forward};
  std::int64_t count{1};
};

// A move the cursor refuses because it was declared NO SCROLL and the move
// goes backward (SQLSTATE 55000).
class CursorForwardOnly : public std::runtime_error {
public:
  CursorForwardOnly() : std::runtime_error("cursor can only scan forward") {}
  ~CursorForwardOnly() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// UPDATE or DELETE ... WHERE CURRENT OF a cursor that names no row to change
// (SQLSTATE 24000): its query is no simple scan of the statement's table, it
// stands before the first row or after the last, or it is a hold cursor whose
// transaction has ended (as PostgreSQL's, which holds its rows, not their
// places in the table).
class CurrentOfRefused : public std::runtime_error {
public:
  enum class Why : std::uint8_t { NotSimple, NotOnRow, Held };
  explicit CurrentOfRefused(Why why)
      : std::runtime_error{why == Why::NotSimple  ? "cursor is not a simply updatable scan of the table"
                           : why == Why::NotOnRow ? "cursor is not positioned on a row"
                                                  : "cursor is held from a previous transaction"},
        why_{why} {}
  ~CurrentOfRefused() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto why() const noexcept -> Why { return why_; }

private:
  Why why_;
};

// What commit() did, as PostgreSQL's COMMIT reports it in its command tag.
enum class CommitOutcome {
  Committed,
  RolledBack,  // the transaction was aborted: nothing was written
};

enum class TxnState : std::uint8_t { Open, Aborted, Ended };

struct TxnHandle;
struct DatabaseLife;

// What a Cursor pins and where it stands (docs/cursor_design.md, *The replay
// cursor*), shared by the Cursor handle and the transaction's list. The
// snapshot and the pinned catalog are the transaction's. Closed when the
// cursor is closed or its transaction ends, aborts or is closed with its
// database.
struct CursorState {
  std::shared_ptr<DatabaseLife> life;
  Database* db{nullptr};  // valid while !life->closed, read under life->mu
  TxnHandle* h{nullptr};  // null once closed, and once a hold cursor's transaction committed
  // A hold cursor (Holdability::Hold) reads its own clone of the snapshot,
  // its catalog and its overlay, not the transaction's, so it outlives the
  // transaction's commit; it is dropped by the transaction's rollback or
  // abort, by close() and by the database's close.
  bc_hold* hold{nullptr};
  std::atomic<bool> live{true};
  // The primary key of the row the cursor stands on, when its query keeps
  // one (Compiled::key_columns); none before the first row, after the last,
  // or for a query that is no simple scan.
  std::optional<std::vector<Value>> key;
  bool aborted{false};  // closed by its transaction's abort
  // Its own fetch failed (docs/cursor_design.md, *Cursors and savepoints*):
  // closed, and kept in its transaction's list so that a rollback to a
  // savepoint taken before it can tell it from a cursor never declared.
  std::atomic<bool> failed{false};
  std::uint64_t seq{0};  // its transaction's count of cursors opened before it
  // The plan the cursor was opened on, shared or its transaction's private
  // one, held so that a rebind cannot change the plan or its order, and a
  // private plan outlives the transaction's plan map.
  PlanRef compiled;
  [[nodiscard]] auto plan() const -> const Statement::Compiled& { return compiled.get(); }
  // The plan's parameters as bound at open: the caller's, the statement's
  // constants and the clocks at the transaction's time.
  std::vector<Value> values;
  bc_overlay* overlay{nullptr};  // the transaction's writes as of open, released at close
  // Where the cursor stands: 0 before the first row, the row's number on a
  // row, `total + 1` after the last. `total` is the result's size once a run
  // has reached the plan's end; position is never past `total + 1`.
  std::uint64_t position{0};
  std::optional<std::uint64_t> total;
  Scrolling scrolling{Scrolling::Any};

  [[nodiscard]] auto after_end() const noexcept -> bool { return total && position > *total; }

  [[nodiscard]] auto holdable() const noexcept -> bool { return hold != nullptr; }

  // The transaction committed: a hold cursor lets go of the pin it was
  // charged to the transaction by, and keeps the view in its hold. Before
  // the transaction is freed.
  void detach() noexcept {
    if (overlay != nullptr) bc_txn_overlay_release(overlay);
    overlay = nullptr;
    h = nullptr;
  }

  // Releases the overlay pin, before the transaction is freed, and the hold,
  // which releases the engine's snapshot.
  void close() noexcept;
};

// What a query portal of a session reads: the tables and indexes its plan
// names, as PostgreSQL's PortalStart opens them at Bind for a plain SELECT
// (an INSERT, UPDATE or DELETE portal holds none). While the caller holds it,
// a DDL of the transaction on what it reads is 55006, as for an open cursor
// (Transaction::bind_portal).
struct PortalReads {
  std::vector<std::uint32_t> objects;
};

// A savepoint of a transaction: its name, and how many cursors the
// transaction had opened when it was taken. The runtime's mark of the same
// depth holds the rest.
struct Savepoint {
  std::string name;
  std::uint64_t cursors{0};
};

// A transaction's resources, on the heap so that a moved Transaction keeps
// them where the Database's list of open transactions finds them.
struct TxnHandle {
  bc_ctx* cx{nullptr};    // null once ended or aborted: an aborted transaction keeps no snapshot
  bc_txn* txn{nullptr};
  std::atomic<TxnState> state{TxnState::Ended};  // close() ends it from another thread
  std::int64_t started_us{0};  // the transaction's time: what CURRENT_DATE, LOCALTIME and LOCALTIMESTAMP read in it
  std::vector<std::shared_ptr<CursorState>> cursors;  // open; closed before the snapshot is released
  std::uint64_t cursors_opened{0};
  std::vector<Savepoint> savepoints;  // oldest first, one per runtime mark
  // The portals bound in the transaction (Transaction::bind_portal): each
  // counts for 55006 while its holder keeps it.
  std::vector<std::weak_ptr<const PortalReads>> portals;
  // The transaction's private plans (docs/transaction_design.md, *The plan
  // map*), by cache key: the text, "\x01" + the text for a cursor's keyed
  // twin, "\x02" + the name for a procedure. An entry is used only while its
  // catalog is the transaction's. What they are charged (plan_charge) is
  // released when the map is cleared; an abort that drops the pin zeroed it
  // already.
  std::map<std::string, std::shared_ptr<const TxnPlan>, std::less<>> plans;
  std::uint64_t plan_charge{0};
  std::uint64_t max_bytes{0};  // TransactionOptions::max_bytes

  // Drops the private plans compiled for another catalog than `cat`, and
  // returns their charge: a rollback to a mark undid the DDL they followed.
  void drop_plans_not_for(const Catalog* cat) noexcept {
    std::uint64_t dropped = 0;
    for (auto it = plans.begin(); it != plans.end();) {
      if (it->second->catalog().get() == cat) {
        ++it;
        continue;
      }
      dropped += it->second->estimate();
      it = plans.erase(it);
    }
    if (txn != nullptr) bc_txn_release_plans(txn, dropped);
    plan_charge -= std::min(dropped, plan_charge);
  }

  // Drops the private plans and returns their charge to the transaction.
  void clear_plans() noexcept {
    if (txn != nullptr) bc_txn_release_plans(txn, plan_charge);
    plan_charge = 0;
    plans.clear();
  }

  // The transaction committed: its hold cursors outlive it, the others close.
  void commit_cursors() noexcept {
    for (const auto& c : cursors) {
      if (c->holdable()) {
        c->detach();
      } else {
        c->close();
      }
    }
    cursors.clear();
  }

  void release() noexcept {
    for (const auto& c : cursors) c->close();
    cursors.clear();
    savepoints.clear();
    portals.clear();
    plans.clear();  // the transaction's charge goes with it
    plan_charge = 0;
    bc_ctx_free(cx);
    bc_txn_free(txn);  // a transaction not committed writes nothing
    cx = nullptr;
    txn = nullptr;
  }
  // Aborted: what it held is freed now, the snapshot that keeps disk blocks
  // first. Only the state is left, for rollback() or commit() to end. A
  // transaction with savepoints keeps its snapshot, its cursors and its
  // marks instead, for rollback_to(): the context is reset, which the
  // runtime's abort, if the statement got that far, leaves at the newest mark.
  void abort() noexcept {
    if (!savepoints.empty()) {
      bc_ctx_reset(cx);
      state.store(TxnState::Aborted);
      return;
    }
    for (const auto& c : cursors) c->aborted = true;
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
  // Transactions ended, autocommit statements included (Database::transaction_counts).
  std::atomic<std::uint64_t> committed{0};
  std::atomic<std::uint64_t> rolled_back{0};
  bool closed{false};                // under mu
  std::mutex open_mu;                // guards open and holds; taken briefly, never across I/O
  std::set<TxnHandle*> open;
  // The hold cursors still open, so that close() releases their snapshots. A
  // cycle with CursorState::life that close() of either end breaks.
  std::set<std::shared_ptr<CursorState>> holds;
};

inline void CursorState::close() noexcept {
  if (overlay != nullptr) bc_txn_overlay_release(overlay);
  overlay = nullptr;
  const auto was_hold = std::exchange(hold, nullptr);
  if (was_hold != nullptr) bc_txn_hold_free(was_hold);
  h = nullptr;
  compiled.reset();
  live.store(false);
  if (was_hold == nullptr) return;
  // Last: this may be the registry's reference to the state, so the
  // database's life is kept by a copy and nothing of this is read after.
  const auto keep = life;
  std::lock_guard<std::mutex> lock{keep->open_mu};
  std::erase_if(keep->holds, [this](const auto& c) { return c.get() == this; });
}

// A cursor opened by Transaction::open_cursor: fetch() returns the statement's
// rows in order, a page at a time, as a full run would. It is a handle on a
// cursor the transaction owns; dropping it leaves the cursor open until
// close() or the end of the transaction. Move-only; one thread at a time.
// A fetch re-runs the plan to its position (the replay cursor), so it costs
// work proportional to the position.
class Cursor {
public:
  Cursor(Cursor&&) noexcept = default;
  Cursor& operator=(Cursor&&) noexcept = default;
  Cursor(const Cursor&) = delete;
  Cursor& operator=(const Cursor&) = delete;
  ~Cursor() = default;

  // The rows of PostgreSQL's FETCH for `move`, in the order it returns them
  // (BACKWARD in reverse), and the cursor moves as PostgreSQL's does: a fetch
  // past either end returns none and leaves it before the first or after the
  // last row. Every move is position arithmetic and one run from the first
  // row, so it costs the plan's work to the furthest row it reads; FETCH LAST
  // and ABSOLUTE with a negative count first run to the end to learn the
  // size, once. BACKWARD buffers the rows it returns, within the statement
  // budget. Throws CursorForwardOnly for a backward move on a NO SCROLL
  // cursor, std::logic_error on a closed cursor, TransactionAborted once its
  // transaction aborted, DbClosed after the database closed. A run that fails
  // (an ExecError, a budget) aborts the transaction, and so closes its cursors.
  // `opts` bounds each run as execute's do (BC_E_BUDGET, 57014): a fetch so
  // stopped fails like any other run.
  auto fetch(CursorMove move, const ExecutionOptions& opts = {}) -> Rows;
  // The next `k` rows, fewer at the end: fetch of a forward move. None for
  // k 0, without moving or running.
  auto fetch(std::uint64_t k, const ExecutionOptions& opts = {}) -> Rows;
  // The columns and types of the rows, with none; runs nothing, moves nothing.
  [[nodiscard]] auto describe() const -> Rows;
  // 0 before the first row, the row's number on a row, rows + 1 after the last.
  [[nodiscard]] auto position() const noexcept -> std::uint64_t { return s_ ? s_->position : 0; }
  // After the last row: a forward fetch returns none.
  [[nodiscard]] auto at_end() const noexcept -> bool { return s_ && s_->after_end(); }
  [[nodiscard]] auto is_open() const noexcept -> bool { return s_ && s_->live.load(); }
  // Its own fetch failed, and it is closed: in a transaction rolled back to a
  // savepoint since, it cannot be run (PostgreSQL's 55000), until it is
  // closed or the transaction ends. A rollback to a savepoint taken before
  // it was opened forgets it.
  [[nodiscard]] auto is_failed() const noexcept -> bool { return s_ && s_->failed.load(); }
  // Releases what it pins; the transaction goes on. Once closed, does nothing.
  void close() noexcept;

private:
  friend class Transaction;
  explicit Cursor(std::shared_ptr<CursorState> s) : s_{std::move(s)} {}
  std::shared_ptr<CursorState> s_;
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
  // TransactionAborted if it already is.
  //
  // A DDL statement is part of the transaction (docs/transaction_design.md,
  // *Transactional DDL*): later statements see it, and commit() publishes it
  // with the transaction's writes, or nothing; a rollback, an abort, or a
  // rollback_to() a savepoint taken before it undoes it. Other sessions see
  // nothing of it until then, and never wait for it. It is bound against
  // the transaction's catalog. CREATE TABLE AS and SELECT … INTO are refused
  // (0A000). Its errors are the ones it has outside a transaction, and:
  // DdlRefused 25006 on a follower; DdlRefused 55006 for a DROP TABLE,
  // CREATE INDEX or ALTER TABLE of a table an open cursor of the transaction
  // reads, or a DROP INDEX of an index one scans; SerializationFailure
  // (40001) when the live catalog has moved past the one the transaction
  // pinned, unless it is the transaction's first statement, which pins again;
  // ExecError BC_E_LIMIT (54000) when its backfill or what it adds would
  // pass TransactionOptions::max_bytes, or its backfill the engine's 1 GiB
  // batch. A DDL that IF [NOT] EXISTS skips changes nothing, and returns its
  // notice.
  //
  // Once the transaction has run DDL, every statement but rollback() and
  // rollback_to() is SerializationFailure once another DDL has published:
  // the transaction's commit could only conflict. That failure aborts the
  // transaction, as any failed statement does, and rollback_to() a savepoint
  // taken before its first DDL lets it go on.
  //
  // Every statement is bound against the catalog the transaction pinned at
  // its first statement (docs/transaction_design.md, *A transaction binds
  // only against its own catalog*): the shared plan when it was bound at
  // exactly that catalog, else a plan compiled for the transaction alone.
  // The forms that take a Statement are its own record: they bind with its
  // parameter types, and result columns other than its own are 0A000
  // "cached plan must not change result type", or SerializationFailure
  // (40001) when the statement fixed them at a catalog later than the pin.
  // The forms by text bind with inferred parameter types and return the
  // pinned catalog's columns, or follow `check`. While the live catalog is
  // past the pin, any binder error is SerializationFailure: the pinned
  // catalog lacks what the statement needs. A SerializationFailure ends a
  // transaction that has run no DDL.
  auto execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts = {}) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}, const ExecutionOptions& opts = {}) -> Rows;
  auto execute(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
               const ExecutionOptions& opts = {}) -> Rows;
  // What the statement's run would bind, as a record: its parameters, its
  // columns, its command, and the version of the plan the transaction would
  // run. Runs nothing; pins the catalog if no statement has. A DDL text is
  // described, not refused. Throws as execute does, and a failure aborts the
  // transaction.
  auto refit(const Statement& stmt) -> StatementRecord;
  auto refit(std::string_view sql, const ResultCheck& check) -> StatementRecord;
  // refit(sql, check) for a portal the caller binds (the server's Bind), and
  // what the plan it would run reads; null for a DDL text and for an INSERT,
  // UPDATE or DELETE, which PostgreSQL opens only as it runs. While the caller
  // holds it, a DDL of the transaction on a table it reads, or an index it
  // scans, is 55006 "… because it is being used by active queries in this
  // session", as for an open cursor (docs/server_design.md, *Extended
  // query*). Throws as refit does.
  auto bind_portal(std::string_view sql, const ResultCheck& check)
      -> std::pair<StatementRecord, std::shared_ptr<const PortalReads>>;
  // The text and its record, from refit(sql, none): a value to run later
  // through any execute form that takes a ResultCheck, here, in another
  // transaction or in autocommit. Puts nothing in the shared cache. Throws
  // as refit does. A record fixed after the transaction's own DDL is marked
  // so (StatementRecord::private_catalog).
  auto prepare(std::string_view sql) -> TxnStatement;
  // The stored procedure `name` as the transaction's catalog has it; null
  // when it has none (the caller's 42883). SerializationFailure, the
  // transaction ended, when it has none and the live catalog is past the pin.
  auto procedure(std::string_view name) -> std::shared_ptr<const Procedure>;
  // Runs the stored procedure `procedure` in the transaction
  // (docs/procedures_design.md, *Running a procedure*). `args` holds a value
  // for each of its arguments, OUT ones included (ignored, as PostgreSQL's
  // CALL ignores them). Returns one row of its INOUT and OUT arguments'
  // values, named and typed as the arguments, or no row when it has none.
  // Each statement of its body sees the writes of those before it. Throws
  // ast::SqlError 42883 for a procedure that does not exist, ParamError for
  // arguments of the wrong number or type, ExecError BC_C_RANGE for one
  // outside its type, and what its statements throw; the transaction is
  // then aborted, as for a failed statement.
  auto call(std::string_view procedure, const std::vector<Value>& args, const ExecutionOptions& opts = {}) -> Rows;
  // Runs an UPDATE or DELETE ... WHERE CURRENT OF (Statement::current_of) on
  // the row `cursor` stands on: `params` are the statement's own, and the
  // cursor's key follows them. The write goes to the transaction's overlay,
  // which the cursor does not see (it is insensitive). Throws
  // CurrentOfRefused (24000) when the cursor's query is no simple scan of the
  // statement's table, it stands on no row, or it is a hold cursor of an
  // earlier transaction; std::invalid_argument for a statement that is not a
  // WHERE CURRENT OF. The transaction then aborts, as for any failed
  // statement. Otherwise as execute.
  auto execute_current_of(const Statement& stmt, const std::vector<Value>& params, const Cursor& cursor,
                          const ExecutionOptions& opts = {}) -> Rows;
  auto execute_current_of(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                          const Cursor& cursor, const ExecutionOptions& opts = {}) -> Rows;
  // Runs a statement that returns rows over the transaction's snapshot and
  // overlay, keeping the window's rows: the replay runner a cursor is built
  // on. The same statement, parameters and view give the same rows on every
  // run, so a window of the rows is the same slice of the whole result. A
  // statement that returns no rows throws std::invalid_argument, the
  // transaction unchanged. Otherwise as execute.
  auto execute_window(const Statement& stmt, const std::vector<Value>& params, RowWindow window,
                      const ExecutionOptions& opts = {}) -> WindowRows;
  auto execute_window(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params, RowWindow window,
                      const ExecutionOptions& opts = {}) -> WindowRows;
  // Opens a forward-only cursor over a SELECT (docs/cursor_design.md). It
  // pins what the statement reads or is given: the snapshot, the
  // transaction's writes so far, the parameters, the time and the plan. It
  // runs nothing; fetch() does. The cursor sees nothing the transaction
  // writes after this call, and is closed with the transaction, by commit,
  // rollback, an abort or the database's close. It reserves the pinned
  // writes' bound against TransactionOptions::max_bytes until closed: a
  // transaction that cannot is aborted with ExecError BC_E_LIMIT (54000).
  // Throws std::invalid_argument, the transaction unchanged, for a statement
  // that is not a SELECT; otherwise as execute.
  //
  // Holdability::Hold (docs/cursor_design.md, *`WITH HOLD`*) gives the cursor
  // a snapshot handle of its own, so it survives commit() of this
  // transaction and keeps showing the view it was opened on: the snapshot
  // (the engine's Snapshot::clone), the pinned catalog and the transaction's
  // writes so far. It does not materialise the result; each fetch replays
  // from that view. It pins the snapshot, and with it every tree node and
  // data file the snapshot reaches, until close(), the database's close or a
  // rollback or abort of this transaction (which drop it, as PostgreSQL drops
  // a cursor declared in a transaction that rolls back). No timer limits how
  // long that is. The overlay copy is charged to this transaction's bound
  // while it lasts, and to DatabaseOptions::max_memory_bytes from this call
  // to close(), past the commit: this call throws ExecError BC_E_MEMORY
  // (53200) when the cap has no room for it, and the transaction aborts. A
  // hold cursor takes no FOR UPDATE guard: a statement with one is the
  // caller's to refuse.
  //
  // The cursor reads its plan's twin that also reads the scanned table's key
  // (docs/cursor_design.md, *WHERE CURRENT OF*), bound as the statement is;
  // a query the planner cannot plan with the key reads its own plan. A
  // private plan is held by the cursor, and reserved with its pin.
  auto open_cursor(const Statement& stmt, const std::vector<Value>& params, Holdability hold = Holdability::Plain,
                   Scrolling scrolling = Scrolling::Any) -> Cursor;
  auto open_cursor(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                   Holdability hold = Holdability::Plain, Scrolling scrolling = Scrolling::Any) -> Cursor;
  // Writes every statement's writes as one batch: Committed. After a failed
  // statement it writes nothing and returns RolledBack. Throws
  // SerializationFailure on a conflict and ExecError on an engine error,
  // with nothing written; TransactionResolutionUnknown when the commit
  // failed after its batch may have reached the engine. The transaction has
  // ended in every case.
  [[nodiscard]] auto commit() -> CommitOutcome;
  // Ends the transaction; nothing it wrote is kept. Once ended, does nothing.
  void rollback() noexcept;
  // Savepoints (docs/transaction_design.md, *Savepoints*), as PostgreSQL's
  // SAVEPOINT, ROLLBACK TO SAVEPOINT and RELEASE SAVEPOINT. Names are matched
  // exactly (the caller folds case), and a name taken again hides the older
  // one until it is released or rolled back past.
  //
  // savepoint() marks the transaction as it stands. It runs nothing, takes
  // no snapshot and charges nothing to TransactionOptions::max_bytes; the
  // first statement after it that writes keeps a copy of the transaction's
  // writes for it, charged to that bound from then on. As execute, it throws
  // TransactionAborted in an aborted transaction.
  //
  // rollback_to() undoes every statement since the savepoint, which stays:
  // their writes, guards and claims go, the snapshot stays. It takes an
  // aborted transaction back to open, as it does a healthy one: a failed
  // statement after a savepoint aborts the transaction but keeps its
  // snapshot, cursors and savepoints for this call. The cursors opened since
  // the savepoint close; those opened before it stay open where they stand,
  // their fetches not undone, but for one whose own fetch failed, which stays
  // closed (Cursor::is_failed). Savepoints taken since go.
  //
  // release() forgets the savepoint and those taken since, keeping their
  // statements' writes; TransactionAborted in an aborted transaction.
  //
  // A name no savepoint has is ast::SqlError 3B001, and aborts the
  // transaction, as any failed statement does. Each throws DbClosed after the
  // database closed and std::logic_error once the transaction has ended.
  void savepoint(std::string_view name);
  void rollback_to(std::string_view name);
  void release(std::string_view name);
  // Aborts the transaction, as a failed statement would, for an error the
  // caller raised itself: every later call is TransactionAborted until
  // commit(), rollback() or a rollback_to(). Does nothing unless open.
  void fail() noexcept;
  [[nodiscard]] auto is_open() const noexcept -> bool { return h_ && h_->state.load() != TxnState::Ended; }
  [[nodiscard]] auto is_aborted() const noexcept -> bool { return h_ && h_->state.load() == TxnState::Aborted; }
#ifdef BYTECASK_SQL_TESTING
  // Test only: the private plans the transaction holds, and what they are
  // charged to max_bytes as the runtime counts it.
  [[nodiscard]] auto private_plans_for_testing() const -> std::size_t { return h_ ? h_->plans.size() : 0; }
  [[nodiscard]] auto private_plan_keys_for_testing() const -> std::vector<std::string> {
    std::vector<std::string> out;
    if (h_) {
      for (const auto& [key, plan] : h_->plans) out.push_back(key);
    }
    return out;
  }
  [[nodiscard]] auto plan_bytes_for_testing() const -> std::uint64_t {
    return h_ && h_->txn != nullptr ? bc_txn_plan_bytes_for_testing(h_->txn) : 0;
  }
  [[nodiscard]] auto cap_holds_for_testing() const -> std::uint64_t {
    return h_ && h_->txn != nullptr ? bc_txn_cap_holds_for_testing(h_->txn) : 0;
  }
#endif

private:
  friend class Database;
  // What a statement of the transaction is: its text, and its record: a
  // held Statement's own (`held`), a caller's (`given`), or none.
  struct Target {
    std::string_view sql;
    const Statement* held{nullptr};
    const StatementRecord* given{nullptr};
    bool cursor_query{false};  // the keyed twin a cursor reads
    // A text no one has prepared may be prepared in the shared cache at a
    // live pin; Transaction::prepare puts nothing there.
    bool share{true};
  };
  Transaction(Database& db, TransactionOptions opts);
  // Under life_->mu, shared: refuses a call on a closed database or an
  // ended or aborted transaction.
  void check_usable() const;
  // The newest savepoint named `name`; ast::SqlError 3B001, the transaction
  // aborted, if there is none.
  auto find_savepoint(std::string_view name) -> std::size_t;
  // The plan the statement runs (the rule): pins first, re-pins a first
  // statement a DDL raced, and throws 0A000 for DDL, the rule's 40001s and
  // 0A000, and the binder's errors. Null for a DDL text when `describe`.
  auto plan_for(const Target& t, bool describe = false) -> PlanRef;
  auto plan_for(const Target& t, const std::shared_ptr<const ast::Statement>& parse) -> PlanRef;
  // The target's parse, which depends on no catalog: the held statement's,
  // the shared cache's or a private plan's for the text, else parsed now.
  auto parse_of(const Target& t) -> std::shared_ptr<const ast::Statement>;
  // Runs a DDL as a step of the transaction (docs/transaction_design.md,
  // *Mechanism: the transaction's private catalog*).
  auto run_ddl(const ast::Statement& st, const std::vector<Value>& params) -> Rows;
  // 55006 for a DDL on what an open cursor of the transaction reads.
  void check_in_use(const Ddl& ddl, const Catalog& cat) const;
  // 40001 for a transaction that has run DDL once another DDL has published.
  void check_doomed() const;
  auto run_locked(const Target& t, const std::vector<Value>& params, const ExecutionOptions& opts,
                  WindowRows* window = nullptr, RowWindow range = {}, const CursorState* positioned = nullptr) -> Rows;
  // `reads`, when given, is set to what the plan reads, null for a DDL.
  auto refit_locked(const Target& t, std::shared_ptr<const PortalReads>* reads = nullptr) -> StatementRecord;
  auto open_locked(const Target& t, const std::vector<Value>& params, Holdability hold, Scrolling scrolling) -> Cursor;
  auto call_locked(std::string_view procedure, const std::vector<Value>& args, const ExecutionOptions& opts,
                   bool share) -> Rows;
  // Runs `body` as one statement: a SerializationFailure ends the
  // transaction, any other failure aborts it.
  template <typename F>
  auto as_statement(F&& body) -> decltype(body());
  // Under life_->mu, shared: forgets and frees it, and counts it committed or
  // rolled back.
  void end_locked(bool committed = false) noexcept;

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
  // The most memory every statement and transaction in progress may hold
  // together: what each statement's budget counts, and the writes each
  // transaction holds until it ends. A statement that would pass it fails
  // at once with BC_E_MEMORY (SQLSTATE 53200), and in a transaction aborts
  // it; nothing waits. Reserved in steps of 1 MiB per statement, so it can
  // refuse up to 1 MiB per statement in progress early. 0: no cap.
  std::uint64_t max_memory_bytes{0};
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

  // prepare, execute, begin, catalog, table_rows and stats throw bytecask::DbClosed
  // once close() was called, as the engine's calls do.

  // Parses; a query, INSERT, UPDATE or DELETE is also bound, planned and
  // compiled, so its binder errors are thrown here. A DDL is bound when it
  // runs, against the catalog it runs in, so prepare throws only its syntax
  // error and execute throws its binder errors. Cached by the exact text.
  // Throws ast::SqlError, or FeatureNotSupported ("no plan: ...") for a
  // statement the planner can't plan. The reference is valid until close(),
  // also once a rebind has replaced the statement in the cache: a DDL that
  // changes the parameter types or result columns its text binds to puts a
  // new Statement there, which a later prepare of the text returns.
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
  //
  // A plan bound at a catalog the live one has since moved past, after any
  // DDL, is bound again at the live catalog (docs/catalog_design.md,
  // *Compatibility*). A held Statement keeps the parameter types and result
  // columns it fixed: bound again with its types, a type that no longer fits
  // is the binder's error, and other result columns are FeatureNotSupported
  // (0A000) "cached plan must not change result type". By text, the
  // statement binds afresh and returns the live catalog's columns; with a
  // given check, as the held form, against the check's record.
  auto execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts = {}) -> Rows;
  auto execute(std::string_view sql, const std::vector<Value>& params = {}, const ExecutionOptions& opts = {}) -> Rows;
  auto execute(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
               const ExecutionOptions& opts = {}) -> Rows;
  // Opens a transaction. Its snapshot is taken at its first statement.
  [[nodiscard]] auto begin(TransactionOptions opts = {}) -> Transaction;
  // Runs a stored procedure in a transaction of its own, then commits it:
  // Transaction::call, then commit(). A conflict reruns the procedure from
  // its arguments, on a fresh snapshot, as many times as an autocommit
  // statement is retried; past that it throws SerializationFailure (40001).
  // Throws what Transaction::call and commit() throw.
  auto call(std::string_view procedure, const std::vector<Value>& args, const ExecutionOptions& opts = {}) -> Rows;
  // Binds the statement's plan again if the live catalog has moved past it,
  // as its next run would. Runs nothing. Throws what that rebind throws,
  // 0A000 for result columns that changed among them; params() and
  // columns() keep what the statement fixed.
  void refit(const Statement& stmt);
  // The record of the text's run in autocommit, under `check`: its
  // parameters, columns, command and the version of its plan. Runs nothing.
  auto refit(std::string_view sql, const ResultCheck& check) -> StatementRecord;
  [[nodiscard]] auto cached_plans() const -> std::size_t {
    std::shared_lock<std::shared_mutex> lock{cache_mu_};
    return cache_.size();
  }
  // How many times a DML statement was compiled, first compiles included.
  [[nodiscard]] auto compilations() const -> std::uint64_t { return compilations_.load(); }
  // The live catalog: the version DDL last published.
  [[nodiscard]] auto catalog() const -> std::shared_ptr<const Catalog>;
  // Reclaims one sealed data file past opts.fragmentation_threshold, if there
  // is one, and returns whether it did. Nothing calls this on its own, so a
  // database whose host never calls it never gets its disk back; the server
  // has a thread for it (docs/session_design.md, *Background vacuum*). Runs
  // concurrently with statements and commits, which wait only for its commit;
  // a second call waits for the first. An open snapshot keeps reading what it
  // reclaims, and keeps those disk blocks until it is released. close() cancels
  // a call in flight, which then returns false, and waits for it to stop: at
  // most its hint write and its commit. So opts.cancel is not read. Throws DbClosed, DbDegraded, or what the failed I/O threw. A
  // failure changes nothing readable, but a failed fdatasync degrades the engine
  // as a commit's does, and a failed unlink of the old file throws after the
  // file was reclaimed, leaving it for the next open.
  [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;
  // Whether a failure has left the engine refusing writes: a failed fdatasync
  // in a vacuum throws the I/O error, not DbDegraded. False after close().
  [[nodiscard]] auto is_degraded() const -> bool;
  // The engine's operational counters and gauges by name, as DB::stats()
  // gives them; the buffer pool's are all zero on a backend with no pool. Live
  // values, not one atomic snapshot. Takes no lock another call waits on.
  [[nodiscard]] auto stats() const -> std::map<std::string, std::int64_t>;
  // How many transactions have ended since open, committed or rolled back,
  // each autocommit statement and DDL counted as one, a failed one as
  // rolled back: what PostgreSQL's pg_stat_database counts as xact_commit
  // and xact_rollback.
  struct TransactionCounts {
    std::uint64_t committed{0};
    std::uint64_t rolled_back{0};
  };
  [[nodiscard]] auto transaction_counts() const -> TransactionCounts {
    return {life_->committed.load(std::memory_order_relaxed), life_->rolled_back.load(std::memory_order_relaxed)};
  }
#ifdef BYTECASK_SQL_TESTING
  // Test only: what the statements, transactions and hold cursors in
  // progress have reserved of max_memory_bytes (0 without a cap).
  [[nodiscard]] auto memory_used_for_testing() const -> std::uint64_t { return memory_.used.load(); }
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
  // transaction's catalog is pinned, before the statement is bound at it.
  // Set it while no transaction runs.
  void after_txn_pin_for_testing(std::function<void()> hook) { after_txn_pin_ = std::move(hook); }
  // Test only: called in vacuum with the options it gives the engine, before
  // it does. Set it while no vacuum runs.
  void before_vacuum_for_testing(std::function<void(const VacuumOptions&)> hook) { before_vacuum_ = std::move(hook); }
  // Test only: the time CURRENT_DATE, LOCALTIME and LOCALTIMESTAMP read, as
  // microseconds since 1970-01-01 00:00:00 UTC; the system clock by default.
  // Set it while no statement runs.
  void clock_for_testing(std::function<std::int64_t()> clock) { clock_ = std::move(clock); }
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
  friend class Cursor;
  using Compiled = Statement::Compiled;
  // A statement of the cache and its plan at the live catalog.
  struct Current {
    const Statement* stmt;
    std::shared_ptr<const Compiled> plan;
  };
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
  auto prepare_locked(std::string_view sql, bool cursor_query = false) -> const Statement&;
  auto execute_locked(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows;
  // A DDL, CREATE TABLE AS or SELECT … INTO statement: run as today, autocommit.
  auto execute_ddl(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows;
  [[nodiscard]] auto cached(std::string_view sql) const -> bool;
  // The cache's statement under `key`, or null.
  [[nodiscard]] auto cached_statement(std::string_view key) const -> const Statement*;
  // A DML statement's plan for the live catalog; null for DDL. Throws what
  // binding, planning and compiling throw.
  [[nodiscard]] auto build(const Statement& s) -> std::shared_ptr<const Compiled>;
  // The statement's plan bound at `cat`, with `fixed` the types of its
  // first parameters (none: inferred); null for DDL.
  [[nodiscard]] auto build_at(const ast::Statement& ast, std::string_view sql, const Catalog& cat,
                              std::span<const SqlType> fixed) -> std::shared_ptr<Compiled>;
  // The same into `c`, a TxnPlan's; refuses DDL.
  void build_into(Compiled& c, const ast::Statement& ast, std::string_view sql, const Catalog& cat,
                  std::span<const SqlType> fixed);
  // The cache's statement `s` at the live catalog (docs/transaction_design.md,
  // *Stages of transactional DDL*, stage 0): its plan when bound there, else
  // bound again with inferred types, swapped into it when its parameter
  // types and result columns are unchanged, else put in a new Statement that
  // replaces it in the cache.
  auto current(const Statement& s) -> Current;
  // A held statement's plan at the live catalog ("own"): bound again with
  // its own parameter types when behind; 0A000 when its columns changed.
  auto held_plan(const Statement& s) -> std::shared_ptr<const Compiled>;
  // The text's plan at the live catalog under a given record: the cache's
  // when its parameter types are the record's, else a one-shot plan at the
  // record's types; 0A000 when the columns differ from the record's.
  auto given_plan(std::string_view sql, const StatementRecord& r, bool cursor_query = false) -> Current;
  // CREATE TABLE AS's query, bound again in place at the live catalog.
  auto query_plan(const Statement& query) -> std::shared_ptr<const Compiled>;
  // Replaces `old` in the cache with a statement of the same text and
  // `plan`; `old` stays valid until close(). The cache's statement after.
  auto replace_cached(const Statement& old, std::shared_ptr<const Compiled> plan) -> const Statement&;
  struct TxnCatalog;
  // The version of a catalog every session sees: the only kind a version
  // names, since two transactions may each hold a private catalog of the
  // same number. Only a TxnCatalog whose catalog is still its pin makes one,
  // and the transaction's version-keyed sites (shared_txn_plan,
  // shared_procedure) take it, so a private catalog cannot reach them.
  class Published {
  public:
    [[nodiscard]] auto version() const -> std::uint64_t { return version_; }

  private:
    friend struct TxnCatalog;
    explicit Published(std::uint64_t version) : version_{version} {}
    std::uint64_t version_;
  };
  // A transaction's catalog (docs/transaction_design.md, *Mechanism: plans
  // compiled against the transaction's catalog*): the published catalog it
  // pinned, and its own, which is that one until its first DDL.
  struct TxnCatalog {
    std::shared_ptr<const Catalog> base;
    std::shared_ptr<const Catalog> cat;
    [[nodiscard]] auto published() const -> std::optional<Published> {
      if (cat != base) return std::nullopt;
      return Published{base->version()};
    }
  };
  // A transaction's plan for its target at its catalog: the shared plan
  // bound at exactly its published catalog, the transaction's private one
  // for its catalog, or a private compile, charged and kept in its map.
  auto txn_plan(TxnHandle& h, const Transaction::Target& t, const TxnCatalog& tc,
                const std::shared_ptr<const ast::Statement>& parse) -> PlanRef;
  // The shared plan for `t` bound at exactly `at`, at the types `fixed`
  // (inferred when none); nullopt when there is none.
  auto shared_txn_plan(const Transaction::Target& t, const std::string& key,
                       const std::optional<std::vector<SqlType>>& fixed, const Published& at) -> std::optional<PlanRef>;
  // A procedure's plan at the transaction's catalog.
  auto txn_procedure(TxnHandle& h, const Procedure& def, const TxnCatalog& tc, bool share)
      -> std::shared_ptr<const CompiledProcedure>;
  // The shared procedure plan compiled at exactly `at`; null when there is none.
  auto shared_procedure(const Procedure& def, bool share, const Published& at)
      -> std::shared_ptr<const CompiledProcedure>;
  // What the search costs plans with: each table's row count, counted on a
  // snapshot taken now.
  [[nodiscard]] auto row_counts() const -> RowCounts;
  // Compiles a plan's text (unless `compile` is false: a procedure's
  // statement, compiled in its procedure's file) and sets what a run needs.
  void link(Compiled& c, bool compile = true);
  // A stored procedure compiled for the live catalog: kept until a DDL
  // moves the catalog on, then built again at the next call.
  [[nodiscard]] auto procedure_plan(const Procedure& def) -> std::shared_ptr<const CompiledProcedure>;
  // `def` compiled against `cat`, into `p`.
  void build_procedure(CompiledProcedure& p, const Procedure& def, const Catalog& cat);
  // Runs it in the transaction `h`. Throws as Transaction::call does.
  auto run_procedure(const CompiledProcedure& p, const Procedure& def, const std::vector<Value>& args, TxnHandle& h,
                     const ExecutionOptions& opts) -> Rows;
  // Runs a DML plan autocommit on `cx`, retried on a conflict. `plan` gives
  // the plan to run, again after a DDL that raced the run (the runtime's
  // check of the plan against the catalog it pinned).
  // `opts` is the call's deadline and cancel flag (budget_for).
  auto run_auto(bc_ctx* cx, const std::function<std::shared_ptr<const Compiled>()>& plan,
                const std::vector<Value>& params, std::int64_t now_us, const ExecutionOptions& opts) -> Rows;
  // Runs `c`, bound at the transaction's pin, as a statement of `txn` on
  // `cx`, its writes added to it. `now_us` is the transaction's time.
  // `window` runs it windowed and records where it stopped.
  auto run_txn(bc_ctx* cx, const Compiled& c, const std::vector<Value>& params, bc_txn* txn, std::int64_t now_us,
               const ExecutionOptions& opts, WindowRows* window = nullptr, RowWindow range = {}) -> Rows;
  // The statement budget with a call's deadline and cancel flag.
  [[nodiscard]] auto budget_for(const ExecutionOptions& opts) const noexcept -> bc_budget;
  // A window of a cursor's rows, run on its pinned plan, parameters and
  // overlay over the transaction's snapshot.
  auto run_pinned(bc_ctx* cx, bc_txn* txn, const CursorState& cursor, RowWindow range, WindowRows& out,
                  const ExecutionOptions& opts) -> Rows;
  // The same on a hold cursor's own view, in a context of the pool.
  auto run_held(const CursorState& cursor, RowWindow range, WindowRows& out, const ExecutionOptions& opts) -> Rows;
  [[nodiscard]] auto clock_now() const -> std::int64_t { return clock_(); }
  // Throws ParamError for parameters the plan does not take, and ExecError
  // BC_C_RANGE for a value outside its parameter's SQL type.
  static void check_params(const Compiled& c, const std::vector<Value>& params);
  // What the plan's parameters are bound to: the caller's, then the
  // statement's constants, with the time's slots filled from `now_us`.
  static auto plan_params(const Compiled& c, const std::vector<Value>& params, std::int64_t now_us) -> std::vector<Value>;
  static auto results(bc_ctx* cx, const Compiled& c, Rows out) -> Rows;
  static auto padded(const std::vector<SqlType>& types, std::vector<Value> values) -> std::vector<Value>;
  // CREATE TABLE AS's query, of a constant whose folding threw `error`:
  // compiled with each such constant NULL (bound::null_failing_constant),
  // for the statement's parameters. Null where no stand-in compiles.
  auto stand_in(const Statement& query, const ast::SqlError& error) -> std::shared_ptr<const Compiled>;
  // Commits a DDL; whether it changed the catalog (false: IF [NOT] EXISTS
  // skipped it). Throws the API's errors for the catalog's failures.
  auto run_ddl(const Ddl& ddl) -> bool;
  // CREATE TABLE AS and SELECT … INTO (docs/frontend_design.md, *The
  // dialect*; docs/catalog_design.md, *CREATE TABLE AS*): the query's rows,
  // autocommit, then the table and its rows as one DDL.
  auto create_table_as(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows;
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
  bc_memory_cap memory_{};  // set by open(); read through bdb_ only when max_memory_bytes is set
  bc_db bdb_{};
  bc_catalog bcat_{};
  std::shared_ptr<DatabaseLife> life_;
  // The cache map: shared to look a statement up, exclusive to add one.
  // Statements are never removed but by close().
  mutable std::shared_mutex cache_mu_;
  std::map<std::string, std::unique_ptr<Statement>, std::less<>> cache_;
  // The statements a rebind replaced in the cache, kept for their holders
  // until close() (#549 bounds them with the cache).
  std::vector<std::unique_ptr<Statement>> retired_;
  std::mutex compile_mu_;  // TinyCC compiles one plan at a time; no I/O under it
  std::mutex procedures_mu_;  // guards procedures_
  std::map<std::string, std::shared_ptr<const CompiledProcedure>, std::less<>> procedures_;
  std::mutex contexts_mu_;  // guards idle_; taken only to take or return one
  std::vector<bc_ctx*> idle_;
  std::atomic<std::uint64_t> compilations_{0};
  // The time of CURRENT_DATE and its kin, microseconds since the Unix epoch: the system clock unless a test sets it.
  std::function<std::int64_t()> clock_{[] {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }};
#ifdef BYTECASK_SQL_TESTING
  std::atomic<bool> conflict_every_commit_{false};
  std::function<void()> after_txn_pin_;
  std::function<void(const VacuumOptions&)> before_vacuum_;
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
CursorForwardOnly::~CursorForwardOnly() = default;
CurrentOfRefused::~CurrentOfRefused() = default;
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
    case BC_E_ARITH:                      // bc_div and bc_mod name their zero divisor; every other failure is a range
      if (std::string_view{e.what()} == "division by zero") return "22012";  // division_by_zero
      if (std::string_view{e.what()} == "negative substring length not allowed") return "22011";  // substring_error
      if (std::string_view{e.what()} == "lower bound must be less than or equal to upper bound") return "22023";
      return "22003";  // numeric_value_out_of_range
    case BC_E_BUDGET: return "57014";     // query_canceled
    case BC_E_MEMORY: return "53200";     // out_of_memory
    case BC_E_CLOSED: return "57P01";     // admin_shutdown
    case BC_E_PATTERN: return "22025";    // invalid_escape_sequence
    case BC_E_CARDINALITY: return "21000";  // cardinality_violation
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
  if (dynamic_cast<const CurrentOfRefused*>(&e) != nullptr) return "24000";
  if (dynamic_cast<const ParamError*>(&e) != nullptr) return "08P01";
  if (const auto* x = dynamic_cast<const DdlRefused*>(&e)) return x->sqlstate();
  if (const auto* x = dynamic_cast<const ProcedureError*>(&e)) return x->sqlstate();
  if (const auto* x = dynamic_cast<const ast::SqlError*>(&e)) return x->sqlstate();
  if (dynamic_cast<const DdlInProgress*>(&e) != nullptr) return "40001";
  if (dynamic_cast<const DbClosed*>(&e) != nullptr) return "57P01";
  if (dynamic_cast<const std::bad_alloc*>(&e) != nullptr) return "53200";
  return "XX000";
}

auto error_hint(const std::exception& e) noexcept -> std::string_view {
  if (const auto* x = dynamic_cast<const ast::SqlError*>(&e)) return x->hint();
  const auto* x = dynamic_cast<const ExecError*>(&e);
  if (x != nullptr && !x->conflict() && x->code() == BC_E_CARDINALITY &&
      std::string_view{x->what()} == "ON CONFLICT DO UPDATE command cannot affect row a second time") {
    return "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.";
  }
  return {};
}

auto Statement::param_types() const -> std::vector<PhysType> {
  if (query_) return query_->param_types();
  auto c = compiled();
  if (!c) return {};
  const auto& all = c->plan->param_types;  // the constants' last: not the caller's to bind
  return {all.begin(), all.end() - std::ssize(c->fixed) - static_cast<std::ptrdiff_t>(c->key_params)};
}

auto Statement::plan_source() const -> std::string {
  auto c = compiled();
  return c ? c->plan->source : std::string{};
}

auto Statement::is_resumable() const -> bool {
  auto c = compiled();
  return c && c->plan->returns_rows && c->plan->resumable;
}

auto Statement::params() const -> std::vector<ParamInfo> {
  if (query_) return query_->params();
  auto c = compiled();
  if (!c) return {};
  auto out = c->params;
  out.resize(out.size() - c->key_params);  // the cursor's key is not the caller's to bind
  return out;
}

auto Statement::current_of_table() const -> std::string_view {
  if (current_of().empty()) return {};
  if (const auto* u = std::get_if<ast::Update>(&ast_)) return u->table;
  return std::get<ast::Delete>(ast_).table;
}

auto Statement::current_of() const -> std::string_view {
  if (const auto* u = std::get_if<ast::Update>(&ast_)) return u->current_of;
  if (const auto* d = std::get_if<ast::Delete>(&ast_)) return d->current_of;
  return {};
}

auto Statement::columns() const -> std::vector<ResultColumn> {
  auto c = compiled();
  std::vector<ResultColumn> out;
  if (!c || !c->plan->returns_rows) return out;
  for (std::size_t i = 0; i < c->columns.size(); ++i) out.push_back({c->columns[i], c->column_types.at(i)});
  return out;
}

auto command_name(Command c) -> std::string_view {
  switch (c) {
    case Command::Select: return "SELECT";
    case Command::Insert: return "INSERT";
    case Command::Update: return "UPDATE";
    case Command::Delete: return "DELETE";
    case Command::CreateTable: return "CREATE TABLE";
    case Command::CreateTableAs: return "CREATE TABLE AS";
    case Command::SelectInto: return "SELECT INTO";
    case Command::CreateIndex: return "CREATE INDEX";
    case Command::DropTable: return "DROP TABLE";
    case Command::DropIndex: return "DROP INDEX";
    case Command::AlterTable: return "ALTER TABLE";
    case Command::CreateView: return "CREATE VIEW";
    case Command::DropView: return "DROP VIEW";
    case Command::CreateProcedure: return "CREATE PROCEDURE";
    case Command::DropProcedure: return "DROP PROCEDURE";
    case Command::CreateRole: return "CREATE ROLE";
    case Command::DropRole: return "DROP ROLE";
    case Command::Grant: return "GRANT";
    case Command::Revoke: return "REVOKE";
    case Command::CreateDomain: return "CREATE DOMAIN";
    case Command::CreateType: return "CREATE TYPE";
    case Command::CreateSequence: return "CREATE SEQUENCE";
    case Command::DropDomain: return "DROP DOMAIN";
    case Command::DropType: return "DROP TYPE";
    case Command::DropSequence: return "DROP SEQUENCE";
  }
  return {};
}

auto Statement::command() const -> Command { return command_of(ast_); }

auto Statement::command_of(const ast::Statement& statement) -> Command {
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
        } else if constexpr (std::is_same_v<T, ast::CreateTableAs>) {
          return st.form == ast::CreateTableAs::Form::SelectInto ? Command::SelectInto : Command::CreateTableAs;
        } else if constexpr (std::is_same_v<T, ast::CreateIndex>) {
          return Command::CreateIndex;
        } else if constexpr (std::is_same_v<T, ast::DropTable>) {
          return Command::DropTable;
        } else if constexpr (std::is_same_v<T, ast::DropIndex>) {
          return Command::DropIndex;
        } else if constexpr (std::is_same_v<T, ast::CreateView>) {
          return Command::CreateView;
        } else if constexpr (std::is_same_v<T, ast::DropView>) {
          return Command::DropView;
        } else if constexpr (std::is_same_v<T, ast::CreateProcedure>) {
          return Command::CreateProcedure;
        } else if constexpr (std::is_same_v<T, ast::DropProcedure>) {
          return Command::DropProcedure;
        } else if constexpr (std::is_same_v<T, ast::CreateRole>) {
          return Command::CreateRole;
        } else if constexpr (std::is_same_v<T, ast::DropRole>) {
          return Command::DropRole;
        } else if constexpr (std::is_same_v<T, ast::Grant>) {
          return st.grant ? Command::Grant : Command::Revoke;
        } else if constexpr (std::is_same_v<T, ast::CreateDomain>) {
          return Command::CreateDomain;
        } else if constexpr (std::is_same_v<T, ast::CreateType>) {
          return Command::CreateType;
        } else if constexpr (std::is_same_v<T, ast::CreateSequence>) {
          return Command::CreateSequence;
        } else if constexpr (std::is_same_v<T, ast::DropTypeOrSequence>) {
          using K = ast::DropTypeOrSequence::Kind;
          return st.kind == K::Domain ? Command::DropDomain : st.kind == K::Type ? Command::DropType : Command::DropSequence;
        } else {
          static_assert(std::is_same_v<T, ast::AddColumn>);
          return Command::AlterTable;
        }
      },
      statement);
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
  d->memory_.cap = opts.max_memory_bytes;
  d->bdb_ = {d->db_.get(), opts.max_memory_bytes == 0 ? nullptr : &d->memory_};
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
  // Released outside open_mu: closing a hold cursor takes it.
  std::set<TxnHandle*> open;
  {
    std::lock_guard<std::mutex> guard{life_->open_mu};
    open.swap(life_->open);
  }
  for (auto* h : open) {
    h->release();
    h->state.store(TxnState::Ended);
  }
  // The hold cursors outlive their transactions, so they are closed too: the
  // snapshots they pin go now, not when a caller drops the handle.
  std::set<std::shared_ptr<CursorState>> holds;
  {
    std::lock_guard<std::mutex> open{life_->open_mu};
    holds.swap(life_->holds);
  }
  for (const auto& c : holds) c->close();
  {
    std::unique_lock<std::shared_mutex> cache{cache_mu_};  // cached_plans() reads it without entering
    cache_.clear();
    retired_.clear();
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

auto Database::is_degraded() const -> bool {
  const std::shared_lock<std::shared_mutex> lock{life_->mu};
  return !life_->closed && db_->is_degraded();
}

auto Database::vacuum(VacuumOptions opts) -> bool {
  const auto alive = enter();
  opts.cancel = &life_->closing;
#ifdef BYTECASK_SQL_TESTING
  if (before_vacuum_) before_vacuum_(opts);
#endif
  return db_->vacuum(opts);
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

auto Database::stats() const -> std::map<std::string, std::int64_t> {
  auto lock = enter();
  return db_->stats();
}

auto Database::row_counts() const -> RowCounts {
  // One snapshot for every table the search counts. Each count is
  // bc_count_range's: at most two record reads, whatever the table holds.
  auto snap = std::make_shared<Snapshot>(db_->snapshot());
  return [snap](const Table& t) -> std::optional<std::uint64_t> {
    auto lo = row_prefix(t.storage_id());
    auto hi = key_successor(lo);
    return snap->count_keys(lo, hi, std::numeric_limits<std::size_t>::max());
  };
}

auto Database::stand_in(const Statement& query, const ast::SqlError& error) -> std::shared_ptr<const Compiled> {
  auto copy = std::make_unique<Statement>();
  copy->sql_ = query.sql_;
  copy->ast_ = ast::clone(std::get<ast::Select>(query.ast_));
  auto pos = error.pos();
  for (int tries = 0; tries < 64; ++tries) {  // a constant at a time: each has its own position
    if (!bound::null_failing_constant(std::get<ast::Select>(copy->ast_), pos)) return nullptr;
    try {
      return build(*copy);
    } catch (const ast::SqlError& e) {
      if (!e.sqlstate().starts_with("22") || e.at_analysis()) return nullptr;
      pos = e.pos();
    }
  }
  return nullptr;
}

namespace {

// Whether the parsed statement runs a plan: DML, as opposed to DDL, CREATE
// TABLE AS and SELECT … INTO.
auto is_dml(const ast::Statement& st) -> bool {
  return std::holds_alternative<ast::Select>(st) || std::holds_alternative<ast::Insert>(st) ||
         std::holds_alternative<ast::Update>(st) || std::holds_alternative<ast::Delete>(st);
}

// A private plan's charge: a constant for what a plan holds besides its
// text, and its text, which its TinyCC code is of the order of.
constexpr std::uint64_t kTxnPlanBytes = 16u << 10;

}  // namespace

auto Database::build(const Statement& s) -> std::shared_ptr<const Compiled> {
  auto cat = live_->current();
  return build_at(s.ast_, s.sql_, *cat, {});
}

auto Database::build_at(const ast::Statement& ast, std::string_view sql, const Catalog& cat, std::span<const SqlType> fixed)
    -> std::shared_ptr<Compiled> {
  auto bound = bound::bind(ast, cat, fixed);
  // DDL has no plan.
  if (std::holds_alternative<Ddl>(bound.stmt)) return nullptr;
  auto c = std::make_shared<Compiled>();
  c->param_sql = std::move(bound.param_sql);
  c->param_table = std::move(bound.param_table);
  c->params = std::move(bound.params);
  c->fixed = bound.fixed;  // copied: the search reads which constants are not NULL
  c->clocks = std::move(bound.clocks);
  c->views = std::move(bound.views);
  c->key_columns = bound.key_columns;
  c->key_table = bound.key_table;
  c->key_params = bound.key_params;
  c->plan = std::make_unique<GeneratedPlan>(generate_plan(std::move(bound), cat, std::string{sql}, row_counts(), budget_.max_bytes));
  link(*c);
  ++compilations_;
  return c;
}

void Database::build_into(Compiled& c, const ast::Statement& ast, std::string_view sql, const Catalog& cat,
                          std::span<const SqlType> fixed) {
  auto bound = bound::bind(ast, cat, fixed);
  if (std::holds_alternative<Ddl>(bound.stmt)) throw std::logic_error("a DDL has no plan");
  c.param_sql = std::move(bound.param_sql);
  c.param_table = std::move(bound.param_table);
  c.params = std::move(bound.params);
  c.fixed = bound.fixed;
  c.clocks = std::move(bound.clocks);
  c.views = std::move(bound.views);
  c.key_columns = bound.key_columns;
  c.key_table = bound.key_table;
  c.key_params = bound.key_params;
  c.plan = std::make_unique<GeneratedPlan>(generate_plan(std::move(bound), cat, std::string{sql}, row_counts(), budget_.max_bytes));
  link(c);
  ++compilations_;
}

void Database::link(Compiled& compiled, bool compile) {
  auto* c = &compiled;
  c->columns = c->plan->result_names;
  c->column_types = c->plan->result_sql;
  c->columns.resize(c->columns.size() - c->key_columns);
  c->column_types.resize(c->column_types.size() - c->key_columns);
  c->result_codes = result_type_codes(c->plan->result_types);
  c->result_layout = make_result_layout(c->plan->result_types);
  if (compile) {
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
}

namespace {

// The caller's parameters of a plan: the cursor key a WHERE CURRENT OF
// takes after them is not the caller's.
template <typename C>
auto caller_params(const C& c) -> std::vector<ParamInfo> {
  auto out = c.params;
  out.resize(out.size() - c.key_params);
  return out;
}

template <typename C>
auto caller_types(const C& c) -> std::vector<SqlType> {
  std::vector<SqlType> out;
  for (const auto& p : caller_params(c)) out.push_back(p.type);
  return out;
}

auto types_of(const std::vector<ParamInfo>& params) -> std::vector<SqlType> {
  std::vector<SqlType> out;
  for (const auto& p : params) out.push_back(p.type);
  return out;
}

// The result columns a client is told: none for a plan that returns no rows.
template <typename C>
auto result_columns_of(const C& c) -> std::vector<ResultColumn> {
  std::vector<ResultColumn> out;
  if (!c.plan->returns_rows) return out;
  for (std::size_t i = 0; i < c.columns.size(); ++i) out.push_back({c.columns[i], c.column_types.at(i)});
  return out;
}

// PostgreSQL's refusal of a prepared statement whose result columns a DDL changed.
[[noreturn]] void throw_result_type_changed() {
  throw FeatureNotSupported(std::string{"cached plan must not change result type"});
}

}  // namespace

auto Database::cached_statement(std::string_view key) const -> const Statement* {
  std::shared_lock<std::shared_mutex> lock{cache_mu_};
  auto it = cache_.find(key);
  return it == cache_.end() ? nullptr : it->second.get();
}

auto Database::replace_cached(const Statement& old, std::shared_ptr<const Compiled> plan) -> const Statement& {
  // Built outside the lock: the parse allocates.
  auto s = std::make_unique<Statement>();
  s->sql_ = old.sql_;
  s->key_ = old.key_;
  s->ast_ = ast::parse(old.sql_);
  if (auto* select = std::get_if<ast::Select>(&s->ast_); select != nullptr && old.key_.starts_with('\x01')) {
    select->row_key = true;
  }
  s->compiled_ = std::move(plan);
  s->owner_ = this;
  std::unique_lock<std::shared_mutex> lock{cache_mu_};
  auto it = cache_.find(old.key_);
  if (it == cache_.end()) return old;  // closing: nothing to replace
  if (it->second.get() != &old) return *it->second;  // another run replaced it first
  retired_.reserve(retired_.size() + 1);
  retired_.push_back(std::move(it->second));
  it->second = std::move(s);
  return *it->second;
}

auto Database::current(const Statement& s) -> Current {
  auto c = s.compiled();
  auto live = live_->current();
  if (c->plan->catalog_version == live->version()) return {&s, c};
  std::shared_ptr<const Compiled> p = build_at(s.ast_, s.sql_, *live, {});
  if (types_of(p->params) == types_of(c->params) && result_columns_of(*p) == result_columns_of(*c)) {
    s.set_compiled(p);
    return {&s, p};
  }
  return {&replace_cached(s, p), p};
}

auto Database::held_plan(const Statement& s) -> std::shared_ptr<const Compiled> {
  auto c = s.compiled();
  auto live = live_->current();
  if (c->plan->catalog_version == live->version()) return c;
  // The cache's own statement binds as a run by text would; its plan is
  // this statement's when nothing it fixed changed.
  if (cached_statement(s.key_) == &s) {
    auto r = current(s);
    if (r.stmt == &s) return r.plan;
    if (types_of(r.plan->params) == types_of(c->params)) throw_result_type_changed();
  }
  // Bound again with the types the statement fixed.
  std::shared_ptr<const Compiled> q = build_at(s.ast_, s.sql_, *live, caller_types(*c));
  if (result_columns_of(*q) != result_columns_of(*c)) throw_result_type_changed();
  s.set_compiled(q);
  return q;
}

auto Database::given_plan(std::string_view sql, const StatementRecord& r, bool cursor_query) -> Current {
  const auto& s = prepare_locked(sql, cursor_query);
  if (s.is_ddl()) return {&s, nullptr};
  auto cur = current(s);
  if (caller_types(*cur.plan) != types_of(r.params)) {
    // Not the cache's: a plan of this run alone, at the record's types.
    auto live = live_->current();
    cur.plan = build_at(cur.stmt->ast_, sql, *live, types_of(r.params));
  }
  if (result_columns_of(*cur.plan) != r.columns) throw_result_type_changed();
  return cur;
}

auto Database::query_plan(const Statement& query) -> std::shared_ptr<const Compiled> {
  auto c = query.compiled();
  if (c->plan->catalog_version == live_->current()->version()) return c;
  auto p = build(query);
  query.set_compiled(p);
  return p;
}

auto Database::prepare(std::string_view sql) -> const Statement& {
  auto lock = enter();
  // A cached statement behind the live catalog binds again, so a prepare of
  // the text returns its columns at the live catalog.
  const auto& s = prepare_locked(sql);
  if (s.is_ddl()) return s;
  return *current(s).stmt;
}

auto Database::cached(std::string_view sql) const -> bool {
  std::shared_lock<std::shared_mutex> lock{cache_mu_};
  return cache_.contains(sql);
}

auto Database::prepare_locked(std::string_view sql, bool cursor_query) -> const Statement& {
  // A cursor's query is another statement from the same text: its plan reads
  // the key too. Its cache key starts with a byte no SQL text does.
  auto key = cursor_query ? "\x01" + std::string{sql} : std::string{sql};
  {
    std::shared_lock<std::shared_mutex> lock{cache_mu_};
    if (auto it = cache_.find(key); it != cache_.end()) return *it->second;
  }
  // Parsed and compiled outside the cache's lock: a statement another
  // thread added meanwhile is kept, and this one dropped.
  auto s = std::make_unique<Statement>();
  s->sql_ = std::string{sql};
  s->key_ = key;
  s->ast_ = ast::parse(sql);
  if (cursor_query) {
    if (auto* select = std::get_if<ast::Select>(&s->ast_)) select->row_key = true;
  }
  if (const auto* ctas = std::get_if<ast::CreateTableAs>(&s->ast_)) {
    // Its query is bound now, as PostgreSQL analyses it before it runs: its
    // errors are the statement's at prepare. The text is the statement's, so
    // a position is into it.
    auto q = std::make_unique<Statement>();
    q->sql_ = s->sql_;
    q->ast_ = ast::clone(ctas->query);
    q->owner_ = this;
    try {
      q->compiled_ = build(*q);
    } catch (const ast::SqlError& e) {
      // A data exception of folding a constant (1/0) is PostgreSQL's when it
      // plans the query, after it has looked for the table: it is thrown
      // when the statement runs. The query is compiled with that constant
      // NULL, for its parameters only: it never runs. One raised as the
      // query is analysed ('abc'::int), or one the stand-in cannot be found
      // for, is thrown now.
      if (!e.sqlstate().starts_with("22") || e.at_analysis()) throw;
      s->query_error_ = std::current_exception();
      q->compiled_ = stand_in(*q, e);
      if (!q->compiled_) throw;
    }
    s->query_ = std::move(q);
  } else if (is_dml(s->ast_)) {
    s->compiled_ = build(*s);
  }
  // A DDL holds its parse and no plan: it is bound when it runs, against the
  // catalog it runs in, so its binder errors are thrown at execute.
  s->ddl_ = s->compiled_ == nullptr;
  s->owner_ = this;
  std::unique_lock<std::shared_mutex> lock{cache_mu_};
  return *cache_.try_emplace(std::move(key), std::move(s)).first->second;
}

auto Database::execute(std::string_view sql, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows {
  return execute(sql, ResultCheck::none(), params, opts);
}

auto Database::execute(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                       const ExecutionOptions& opts) -> Rows {
  auto lock = enter();
  try {
    Rows out;
    // By text: the cache's statement for it, whichever that is when the plan is taken.
    const auto given = check.kind == ResultCheck::Kind::Given;
    const auto& first = given ? *given_plan(sql, check.record).stmt : prepare_locked(sql);
    if (first.is_ddl()) {
      out = execute_ddl(first, params, opts);
    } else {
      if (!first.current_of().empty()) throw ParamError("WHERE CURRENT OF needs a cursor of a transaction");
      auto plan = [&] { return given ? given_plan(sql, check.record).plan : current(prepare_locked(sql)).plan; };
      auto* cx = acquire_context();
      try {
        out = run_auto(cx, plan, params, clock_now(), opts);
        release_context(cx);
      } catch (...) {
        release_context(cx);
        throw;
      }
    }
    life_->committed.fetch_add(1, std::memory_order_relaxed);
    return out;
  } catch (...) {
    life_->rolled_back.fetch_add(1, std::memory_order_relaxed);
    throw;
  }
}

auto Database::execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts) -> Rows {
  auto lock = enter();
  try {
    auto out = execute_locked(stmt, params, opts);
    life_->committed.fetch_add(1, std::memory_order_relaxed);
    return out;
  } catch (...) {
    life_->rolled_back.fetch_add(1, std::memory_order_relaxed);
    throw;
  }
}

namespace {

// A procedure argument's type as PostgreSQL's notices name it.
auto notice_type_name(SqlKind k) -> std::string_view {
  switch (k) {
    case SqlKind::SmallInt: return "pg_catalog.int2";
    case SqlKind::Integer: return "pg_catalog.int4";
    case SqlKind::BigInt: return "pg_catalog.int8";
    case SqlKind::Real: return "pg_catalog.float4";
    case SqlKind::Double: return "pg_catalog.float8";
    case SqlKind::Decimal:
    case SqlKind::Numeric: return "pg_catalog.numeric";
    case SqlKind::Boolean: return "pg_catalog.bool";
    case SqlKind::Text: return "text";
    case SqlKind::Varchar: return "pg_catalog.varchar";
    case SqlKind::Char: return "pg_catalog.bpchar";
    case SqlKind::Bytea: return "bytea";
    case SqlKind::Date: return "date";
    case SqlKind::Time: return "pg_catalog.time";
    case SqlKind::Timestamp: return "pg_catalog.timestamp";
    case SqlKind::TimestampTz: return "timestamptz";
  }
  return "";
}

// The NOTICE PostgreSQL gives for a DDL that IF [NOT] EXISTS skipped.
auto skipped(const Ddl& ddl) -> Notice {
  return std::visit(
      [](const auto& d) -> Notice {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, CreateTableDdl>) {
          return {"42P07", "relation \"" + d.name + "\" already exists, skipping"};
        } else if constexpr (std::is_same_v<T, DropTableDdl>) {
          return {"00000", "table \"" + d.name + "\" does not exist, skipping"};
        } else if constexpr (std::is_same_v<T, DropIndexDdl>) {
          return {"00000", "index \"" + d.name + "\" does not exist, skipping"};
        } else if constexpr (std::is_same_v<T, DropViewDdl>) {
          return {"00000", "view \"" + d.name + "\" does not exist, skipping"};
        } else if constexpr (std::is_same_v<T, DropRoleDdl>) {
          return {"00000", "role \"" + d.names.front() + "\" does not exist, skipping"};
        } else if constexpr (std::is_same_v<T, DropTypeDdl>) {
          return {"00000", "type \"" + d.name + "\" does not exist, skipping"};
        } else if constexpr (std::is_same_v<T, DropSequenceDdl>) {
          return {"00000", "sequence \"" + d.name + "\" does not exist, skipping"};
        } else if constexpr (std::is_same_v<T, CreateSequenceDdl>) {
          return {"42P07", "relation \"" + d.name + "\" already exists, skipping"};
        } else if constexpr (std::is_same_v<T, DropProcedureDdl>) {
          std::string args;
          for (const auto& t : d.arg_types.value_or(std::vector<SqlType>{})) {
            args += (args.empty() ? "" : ",") + std::string{notice_type_name(t.kind)};
          }
          return {"00000", "procedure " + d.name + "(" + args + ") does not exist, skipping"};
        } else {
          throw std::logic_error("a DDL that never skips changed nothing");
        }
      },
      ddl);
}

// A DDL's notices: one per name DROP ROLE IF EXISTS skips, which the
// catalog it was bound against lacks; otherwise, when IF [NOT] EXISTS left
// the catalog unchanged, the one skipped() gives.
auto skip_notices(const Ddl& ddl, const Catalog& before, bool changed) -> std::vector<Notice> {
  std::vector<Notice> out;
  if (const auto* dr = std::get_if<DropRoleDdl>(&ddl)) {
    if (!dr->if_exists) return out;
    for (const auto& name : dr->names) {
      if (before.role(name) == nullptr) out.push_back({"00000", "role \"" + name + "\" does not exist, skipping"});
    }
    return out;
  }
  if (!changed) out.push_back(skipped(ddl));
  return out;
}

}  // namespace

auto Database::execute_locked(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  if (stmt.owner_ != this) throw std::logic_error("the statement was prepared on another database");
  if (!stmt.current_of().empty()) throw ParamError("WHERE CURRENT OF needs a cursor of a transaction");
  if (stmt.is_ddl()) return execute_ddl(stmt, params, opts);
  auto* cx = acquire_context();
  try {
    auto out = run_auto(cx, [&] { return held_plan(stmt); }, params, clock_now(), opts);
    release_context(cx);
    return out;
  } catch (...) {
    release_context(cx);
    throw;
  }
}

auto Database::execute_ddl(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  if (stmt.query_) return create_table_as(stmt, params, opts);
  {
    if (!params.empty()) throw ParamError("DDL takes no parameters");
    auto before = live_->current();
    auto bound = bound::bind(stmt.ast_, *before);
    const auto& ddl = std::get<Ddl>(bound.stmt);
    Rows out;
    out.notices = skip_notices(ddl, *before, run_ddl(ddl));
    out.affected = 0;
    return out;
  }
}

auto Database::run_ddl(const Ddl& ddl) -> bool {
  try {
    return live_->run(ddl).changed;
  } catch (const ConstraintViolation& e) {
    throw ExecError(e.what(), BC_E_CONSTRAINT, e.detail().encode());
  } catch (const DdlInProgress&) {
    throw_serialization_failure(kDdlInProgress);  // a DDL never waits for another
  } catch (const CommitOutcomeUnknown&) {
    throw TransactionResolutionUnknown{};
  }
}

namespace {

// The table CREATE TABLE AS makes of a query's result columns, in
// PostgreSQL's order of refusals: more names than columns (42601), then a
// name twice (42701). A column of no type a table holds yet is refused
// (0A000): a NUMERIC of no declared precision (a NUMERIC column, an AVG, a
// literal 1.5), which a NUMERIC column could hold but CTAS does not map to
// one yet; and a table of no columns, which a CreateTableDdl
// with none would read as a skip. VARCHAR of no length is TEXT, as a column
// declared so is.
auto ctas_columns(const ast::CreateTableAs& ctas, const std::vector<std::string>& names,
                  const std::vector<SqlType>& types) -> std::vector<ColumnSpec> {
  const auto* what = ctas.form == ast::CreateTableAs::Form::SelectInto ? "SELECT … INTO" : "CREATE TABLE AS";
  if (ctas.columns.size() > names.size()) {
    throw ast::SqlError{"too many column names were specified", -1, kSyntaxError};
  }
  std::vector<ColumnSpec> out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    ColumnSpec c;
    c.name = i < ctas.columns.size() ? ctas.columns[i] : names[i];
    c.sql = types.at(i);
    if (std::ranges::contains(out, c.name, &ColumnSpec::name)) {
      throw ast::SqlError{"column \"" + c.name + "\" specified more than once", -1, kDuplicateColumn};
    }
    out.push_back(std::move(c));
  }
  if (out.empty()) throw FeatureNotSupported(std::string{what} + " of no columns is not supported");
  for (auto& c : out) {
    if (c.sql.kind == SqlKind::Varchar && c.sql.length == 0) c.sql = SqlType{SqlKind::Text};
    if (c.sql.kind == SqlKind::Numeric || (c.sql.kind == SqlKind::Decimal && c.sql.precision == 0)) {
      // Only a constant's cast is taken: a NUMERIC column's or an AVG's is not.
      throw FeatureNotSupported(std::string{what} + " of column \"" + c.name +
                                "\", a numeric of no declared precision, is not supported yet" +
                                (c.sql.kind == SqlKind::Decimal ? ": cast it to NUMERIC(p, s)" : ""));
    }
    if (!sql_type_valid(c.sql)) {
      throw FeatureNotSupported(std::string{what} + " of column \"" + c.name + "\" of type " +
                                sql_type_name(c.sql) + " is not supported");
    }
  }
  return out;
}

// A result row as its table stores it: a CHAR(n) without the trailing
// spaces a result pads it with. Each value is checked against its column's
// type, as an INSERT's is.
auto ctas_row(const std::vector<ColumnSpec>& columns, std::vector<Value> values) -> std::vector<Datum> {
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& c = columns.at(i);
    if (!datum_fits(values[i], phys_type(c.sql))) {
      throw ExecError("a value of column " + c.name + " is not of its type", BC_E_CORRUPT);
    }
    if (auto* b = std::get_if<Bytes>(&values[i]); b != nullptr && c.sql.kind == SqlKind::Char) {
      while (!b->empty() && b->back() == std::byte{' '}) b->pop_back();
    }
    if (auto why = out_of_range(c.sql, values[i])) {
      throw ExecError(*why, BC_E_CONSTRAINT, ConstraintDetail{BC_C_RANGE, 0}.encode());
    }
  }
  return values;
}

}  // namespace

auto Database::create_table_as(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  const auto& ctas = std::get<ast::CreateTableAs>(stmt.ast_);
  const auto& query = *stmt.query_;
  Rows out;
  CreateTableDdl ddl;
  ddl.name = ctas.table;
  ddl.if_not_exists = ctas.if_not_exists;
  // As PostgreSQL: a relation of the name is found before the query is
  // planned, and IF NOT EXISTS then skips the statement.
  {
    auto cat = live_->current();
    if (cat->table(ctas.table) != nullptr || cat->view(ctas.table) != nullptr || cat->index(ctas.table).first != nullptr ||
        cat->sequence(ctas.table) != nullptr) {
      if (!ctas.if_not_exists) throw DdlRefused("relation \"" + ctas.table + "\" already exists", kDuplicateTable);
      out.notices.push_back(skipped(Ddl{ddl}));
      return out;
    }
  }
  if (stmt.query_error_) {
    // PostgreSQL does not plan the query of WITH NO DATA, so creates the
    // table: its column of the failing constant has the constant's type,
    // which the stand-in lost.
    if (!ctas.with_data) {
      throw FeatureNotSupported(std::string{"WITH NO DATA of a query whose constant raises an error is not supported"});
    }
    std::rethrow_exception(stmt.query_error_);
  }
  auto* cx = acquire_context();
  try {
    // The table's columns are checked before the query runs, against the
    // plan that will run; then again against the rows' own, which a DDL
    // meanwhile may have recompiled.
    auto c = query_plan(query);
    check_params(*c, params);
    ddl.columns = ctas_columns(ctas, c->columns, c->column_types);
    if (ctas.with_data) {
      auto rows = run_auto(cx, [&] { return query_plan(query); }, params, clock_now(), opts);
      ddl.columns = ctas_columns(ctas, rows.columns, rows.types);
      ddl.rows.reserve(rows.rows.size());
      for (auto& r : rows.rows) ddl.rows.push_back(ctas_row(ddl.columns, std::move(r)));
    }
    release_context(cx);
  } catch (...) {
    release_context(cx);
    throw;
  }
  auto n = std::ssize(ddl.rows);
  bool changed = false;
  try {
    changed = run_ddl(ddl);
  } catch (const std::length_error& e) {
    throw ExecError(e.what(), BC_E_LIMIT);  // the backfill, a row or the ids over the engine's limits: 54000
  }
  if (!changed) {
    out.notices.push_back(skipped(Ddl{std::move(ddl)}));
    return out;
  }
  out.affected = ctas.with_data ? n : -1;
  return out;
}

// A value outside its parameter's SQL type is refused before the plan runs,
// as PostgreSQL refuses a $n it cannot coerce at bind: even one a plan would
// not read (a key whose other part is NULL). The parameters are checked
// against the plan that will run, and so against the catalog it was
// verified against: a DDL since the plan was compiled may have changed what
// they are (an INTEGER key recreated as BIGINT).
void Database::check_params(const Compiled& c, const std::vector<Value>& params) {
  const auto& types = c.plan->param_types;
  if (params.size() != types.size() - c.fixed.size()) {
    throw ParamError("statement takes " + std::to_string(types.size() - c.fixed.size()) + " parameters, got " +
                     std::to_string(params.size()));
  }
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (!datum_fits(params[i], types[i])) throw ParamError("parameter $" + std::to_string(i + 1) + " has the wrong type");
  }
  // An UPDATE's SET values first, then the rest, as PostgreSQL folds a
  // statement's target list before its WHERE: of two out of range, the
  // first so reported.
  for (auto set_first : {true, false}) {
    for (std::size_t i = 0; i < params.size(); ++i) {
      const auto& sql = c.param_sql.at(i);
      if (!sql) continue;  // assigned only: the row builder checks it, in column order
      if (c.params.at(i).assigned_only != set_first) continue;
      if (auto range = out_of_range(*sql, params[i])) {
        throw ExecError(*range + " for parameter $" + std::to_string(i + 1), BC_E_CONSTRAINT,
                        ConstraintDetail{BC_C_RANGE, c.param_table.at(i)}.encode());
      }
    }
  }
}

// The time as a value of each clock slot's type: the day, the time of day, or
// the instant, in UTC, rounded to the slot's fractional digits as PostgreSQL
// rounds a time or timestamp typmod.
static auto clock_value(const bound::ClockSlot& k, std::int64_t now_us) -> Value {
  constexpr std::int64_t kDay = 86'400'000'000;
  auto day = now_us >= 0 ? now_us / kDay : -((-now_us + kDay - 1) / kDay);
  if (k.kind == bound::ClockSlot::Kind::Date) return Value{day};
  auto us = k.kind == bound::ClockSlot::Kind::Time ? now_us - day * kDay : now_us;  // Timestamp and TimestampTz: the instant
  std::int64_t unit = 1;
  for (auto i = k.precision; i < 6; ++i) unit *= 10;
  auto rounded = us - (us >= 0 ? us % unit : (unit + us % unit) % unit);
  if (us - rounded >= (unit + 1) / 2) rounded += unit;
  return Value{rounded};
}

auto Database::plan_params(const Compiled& c, const std::vector<Value>& params, std::int64_t now_us) -> std::vector<Value> {
  auto out = params;
  // A parameter compared with a REAL is typed real, as PostgreSQL types it:
  // rounded once to single precision, as the column's values were, so a
  // filter compares the float of the value given (check_params refused one
  // that overflows).
  for (std::size_t i = 0; i < params.size() && i < c.param_sql.size(); ++i) {
    auto* d = std::get_if<double>(&out[i]);
    if (d != nullptr && c.param_sql[i] && c.param_sql[i]->kind == SqlKind::Real) *d = float_rounding(*d);
  }
  out.insert(out.end(), c.fixed.begin(), c.fixed.end());
  for (const auto& k : c.clocks) out[params.size() + k.slot] = clock_value(k, now_us);
  return out;
}

auto Database::budget_for(const ExecutionOptions& opts) const noexcept -> bc_budget {
  bc_budget b = budget_;
  // Counted from the steady clock's epoch, as the runtime reads it; 0 is none.
  std::int64_t ns = std::chrono::nanoseconds{opts.deadline.time_since_epoch()}.count();
  if (ns != 0) b.deadline_ns = ns > 0 ? static_cast<std::uint64_t>(ns) : 1;
  if (opts.cancel != nullptr) b.cancel = &opts.cancel->v_;
  return b;
}

auto Database::run_auto(bc_ctx* cx, const std::function<std::shared_ptr<const Compiled>()>& plan,
                        const std::vector<Value>& params, std::int64_t now_us, const ExecutionOptions& opts) -> Rows {
  // The context reads the parameters through bp, filled once the plan that
  // runs is known: the caller's, then its constants (plan_params).
  bc_params bp;
  auto budget = budget_for(opts);
  auto c = plan();
  check_params(*c, params);
  bp.values = plan_params(*c, params, now_us);
  auto r = bc_execute(cx, c->code->fn(), &c->ref, &bp, budget, sync_commits_, kMaxRetries);
  // bc_ctx_run checks the plan against the catalog it pinned. A DDL that
  // raced the run since the plan was taken makes a plan it changed
  // incompatible (BC_E_LAYOUT with BC_L_INCOMPATIBLE): the plan is taken
  // again, bound at the new catalog, and run once more.
  if (r == BC_FAILED && bc_error(cx) == BC_E_LAYOUT && bc_error_detail(cx) == BC_L_INCOMPATIBLE) {
    bc_ctx_reset(cx);
    c = plan();
    check_params(*c, params);
    bp.values = plan_params(*c, params, now_us);
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

auto Database::run_txn(bc_ctx* cx, const Compiled& c, const std::vector<Value>& params, bc_txn* txn,
                       std::int64_t now_us, const ExecutionOptions& opts, WindowRows* window, RowWindow range) -> Rows {
  check_params(c, params);
  bc_params bp{plan_params(c, params, now_us)};
  bc_ctx_begin_txn(cx, txn, &bp, budget_for(opts));
  if (window != nullptr) bc_ctx_window(cx, range.skip, range.limit);
  bc_ctx_run(cx, c.code->fn(), &c.ref);
  // The plan was bound at the transaction's pin, which the runtime checks it
  // against: an incompatible plan is a bug, refused as a DDL since would be.
  if (bc_error(cx) == BC_E_LAYOUT && bc_error_detail(cx) == BC_L_INCOMPATIBLE) {
    bc_ctx_reset(cx);
    throw_serialization_failure(kCatalogChanged);
  }
  // The statement's writes join the transaction, or it aborts.
  if (!bc_txn_add(cx)) throw_context_error(cx);
  if (window != nullptr) {
    window->position = bc_ctx_emitted(cx);
    window->end = !bc_ctx_stopped(cx);
  }
  return results(cx, c, Rows{});
}

auto Database::run_pinned(bc_ctx* cx, bc_txn* txn, const CursorState& cursor, RowWindow range, WindowRows& out,
                          const ExecutionOptions& opts) -> Rows {
  const auto& c = cursor.plan();
  bc_params bp;
  bc_ctx_begin_txn(cx, txn, &bp, budget_for(opts));
  bc_ctx_use_overlay(cx, cursor.overlay);
  bc_ctx_window(cx, range.skip, range.limit);
  bp.values = cursor.values;
  // The transaction's pin is the one the plan was verified against at open
  // (a pin blocks a re-pin), and bc_ctx_run verifies it again.
  bc_ctx_run(cx, c.code->fn(), &c.ref);
  if (!bc_txn_add(cx)) throw_context_error(cx);
  out.position = bc_ctx_emitted(cx);
  out.end = !bc_ctx_stopped(cx);
  return results(cx, c, Rows{});
}

auto Database::run_held(const CursorState& cursor, RowWindow range, WindowRows& out, const ExecutionOptions& opts)
    -> Rows {
  const auto& c = cursor.plan();
  auto* cx = acquire_context();
  try {
    bc_params bp;
    bc_ctx_begin_hold(cx, cursor.hold, &bp, budget_for(opts));
    bc_ctx_window(cx, range.skip, range.limit);
    bp.values = cursor.values;
    bc_ctx_run(cx, c.code->fn(), &c.ref);
    if (bc_error(cx) != BC_OK) throw_context_error(cx);
    out.position = bc_ctx_emitted(cx);
    out.end = !bc_ctx_stopped(cx);
    auto rows = results(cx, c, Rows{});
    release_context(cx);
    return rows;
  } catch (...) {
    bc_ctx_reset(cx);
    release_context(cx);
    throw;
  }
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
      // Result rows are decoded after the commit: the context holds the
      // rows of the attempt that committed (each attempt begins with none),
      // copies of the values, a RETURNING's among them.
      auto values = decode_row(c.result_layout, std::as_bytes(std::span{row, len}));
      if (!values) {
        bc_ctx_reset(cx);
        throw ExecError("malformed result row: column " + std::to_string(values.error()), BC_E_CORRUPT);
      }
      out.rows.push_back(padded(out.types, std::move(*values)));
    }
  }
  bc_ctx_reset(cx);
  return out;
}

// A CHAR(n) is stored without its trailing spaces and returned padded with
// spaces to n code points, as PostgreSQL returns it.
auto Database::padded(const std::vector<SqlType>& types, std::vector<Value> values) -> std::vector<Value> {
  for (std::size_t i = 0; i < values.size() && i < types.size(); ++i) {
    auto* b = std::get_if<Bytes>(&values[i]);
    if (types[i].kind != SqlKind::Char || b == nullptr) continue;
    auto have = utf8_code_points(*b).value_or(b->size());
    if (have < types[i].length) b->insert(b->end(), types[i].length - have, std::byte{' '});
  }
  return values;
}

void Database::refit(const Statement& stmt) {
  auto lock = enter();
  if (stmt.owner_ != this) throw std::logic_error("the statement was prepared on another database");
  if (stmt.query_) {  // CREATE TABLE AS: its query's parameters
    if (!stmt.query_error_) (void)query_plan(*stmt.query_);
    return;
  }
  if (stmt.is_ddl()) return;
  (void)held_plan(stmt);
}

namespace {

// A statement's record from the plan it would run.
template <typename C>
auto record_of(const Statement& s, const C& c) -> StatementRecord {
  StatementRecord out;
  out.params = caller_params(c);
  out.columns = result_columns_of(c);
  out.command = s.command();
  out.version = c.plan->catalog_version;
  out.current_of = std::string{s.current_of()};
  out.current_of_table = std::string{s.current_of_table()};
  return out;
}

// A DDL's, CREATE TABLE AS's or SELECT … INTO's record: no plan of its own.
auto ddl_record(const Statement& s) -> StatementRecord {
  StatementRecord out;
  out.params = s.params();
  out.command = s.command();
  out.ddl = true;
  return out;
}

}  // namespace

auto Database::refit(std::string_view sql, const ResultCheck& check) -> StatementRecord {
  auto lock = enter();
  if (check.kind == ResultCheck::Kind::Given) {
    auto cur = given_plan(sql, check.record);
    if (!cur.plan) return ddl_record(*cur.stmt);
    return record_of(*cur.stmt, *cur.plan);
  }
  const auto& s = prepare_locked(sql);
  if (s.is_ddl()) {
    if (s.query_ && !s.query_error_) (void)query_plan(*s.query_);
    return ddl_record(s);
  }
  auto cur = current(s);
  return record_of(*cur.stmt, *cur.plan);
}

auto Database::procedure_plan(const Procedure& def) -> std::shared_ptr<const CompiledProcedure> {
  auto cat = live_->current();
  {
    std::lock_guard<std::mutex> lock{procedures_mu_};
    auto it = procedures_.find(def.name);
    if (it != procedures_.end() && it->second->id == def.id && it->second->catalog_version == cat->version()) {
      return it->second;
    }
  }
  auto p = std::make_shared<CompiledProcedure>();
  build_procedure(*p, def, *cat);
  std::lock_guard<std::mutex> lock{procedures_mu_};
  procedures_.insert_or_assign(def.name, p);
  return p;
}

void Database::build_procedure(CompiledProcedure& p, const Procedure& def, const Catalog& cat) {
  p.id = def.id;
  p.catalog_version = cat.version();
  p.gen = generate_procedure(def, cat, row_counts(), budget_.max_bytes);
  for (auto& st : p.gen.stmts) {
    auto c = std::make_shared<Compiled>();
    c->param_sql = st.param_sql;
    c->param_table = st.param_table;
    c->params = st.params;
    c->fixed = st.fixed;
    c->views = st.views;
    c->plan = std::make_unique<GeneratedPlan>(st.plan);
    link(*c, false);
    p.stmts.push_back(std::move(c));
  }
  {
    std::lock_guard<std::mutex> lock{compile_mu_};
    p.code = TccPlan::compile_procedure(p.gen.source);
  }
  ++compilations_;
}

auto Database::run_procedure(const CompiledProcedure& p, const Procedure& def, const std::vector<Value>& args,
                             TxnHandle& h, const ExecutionOptions& opts) -> Rows {
  const auto& gen = p.gen;
  if (args.size() != def.args.size()) {
    throw ParamError("procedure " + def.name + " takes " + std::to_string(def.args.size()) + " arguments, got " +
                     std::to_string(args.size()));
  }
  bc_proc px;
  px.types = gen.slot_types;
  px.slots.values.resize(gen.var_names.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& a = def.args[i];
    if (a.mode == ProcedureArg::Mode::Out) continue;  // starts NULL, as PostgreSQL's does
    if (!datum_fits(args[i], phys_type(a.type))) throw ParamError("argument " + a.name + " has the wrong type");
    if (auto range = out_of_range(a.type, args[i])) {
      throw ExecError(*range + " for argument " + a.name, BC_E_CONSTRAINT, ConstraintDetail{BC_C_RANGE, 0}.encode());
    }
    px.slots.values[i] = args[i];
  }
  px.slots.values.insert(px.slots.values.end(), gen.constants.begin(), gen.constants.end());
  auto budget = budget_for(opts);
  auto* frame = acquire_context();
  struct Release {
    Database& db;
    bc_ctx* cx;
    ~Release() {
      bc_ctx_reset(cx);
      db.release_context(cx);
    }
  } release{*this, frame};
  px.frame = frame;
  bc_ctx_begin_frame(frame, &px.slots, budget);
  auto* sx = h.cx;
  bc_params bp;
  // bc_stmt_begin: the statement's parameters from the variables, checked
  // as a client's are, then begun on the transaction and verified against
  // its pin. A plan the pin does not fit is a DDL since the transaction began.
  px.begin = [&](std::uint32_t k) -> bc_ctx* {
    const auto& c = *p.stmts.at(k);
    std::vector<Value> params;
    for (auto v : gen.stmts[k].param_vars) params.push_back(px.slots.values.at(v));
    check_params(c, params);
    bp.values = plan_params(c, params, h.started_us);
    bc_ctx_begin_txn(sx, h.txn, &bp, budget);
#ifdef BYTECASK_SQL_TESTING
    if (after_txn_pin_) after_txn_pin_();
#endif
    if (!bc_ctx_check(sx, &c.ref) && bc_error(sx) == BC_E_LAYOUT && bc_error_detail(sx) == BC_L_INCOMPATIBLE) {
      bc_ctx_reset(sx);
      throw_serialization_failure(kCatalogChanged);
    }
    return sx;
  };
  // bc_stmt_end: its effects into the transaction, its first row into the
  // INTO variables, and FOUND.
  // A row's columns into the targets, each checked against its variable's type.
  auto into = [&](const std::vector<std::uint32_t>& targets, const std::vector<Value>* row) {
    for (std::size_t i = 0; i < targets.size(); ++i) {
      auto v = row == nullptr ? Value{} : row->at(i);
      if (auto range = out_of_range(gen.slot_types.at(targets[i]), v)) {
        throw ExecError(*range + " for variable " + gen.var_names.at(targets[i]), BC_E_CONSTRAINT,
                        ConstraintDetail{BC_C_RANGE, 0}.encode());
      }
      px.slots.values.at(targets[i]) = std::move(v);
    }
  };
  // A FOR loop's rows, by statement: what is left of them, and whether there were any.
  struct LoopRows {
    std::vector<std::vector<Value>> rows;
    std::size_t next{0};
  };
  std::map<std::uint32_t, LoopRows> loops;
  px.end = [&](std::uint32_t k) {
    const auto& c = *p.stmts.at(k);
    const auto& st = gen.stmts[k];
    if (!bc_txn_add(sx)) throw_context_error(sx);
    auto rows = results(sx, c, Rows{});
    if (st.loop) {
      loops[k] = LoopRows{std::move(rows.rows), 0};  // FOUND is set when the loop ends
      return;
    }
    auto found = c.plan->returns_rows ? !rows.rows.empty() : rows.affected > 0;
    px.slots.values.at(gen.found) = found;
    if (!st.into) return;
    if (st.strict && rows.rows.empty()) throw ProcedureError("query returned no rows", kNoDataFound);
    if (st.strict && rows.rows.size() > 1) throw ProcedureError("query returned more than one row", kTooManyRows);
    into(st.targets, rows.rows.empty() ? nullptr : &rows.rows[0]);
  };
  px.next = [&](std::uint32_t k) {
    auto& l = loops.at(k);
    if (l.next >= l.rows.size()) return false;
    into(gen.stmts[k].targets, &l.rows[l.next++]);
    return true;
  };
  px.done = [&](std::uint32_t k) {
    auto it = loops.find(k);
    if (it == loops.end()) return;
    px.slots.values.at(gen.found) = !it->second.rows.empty();
    loops.erase(it);
  };
  p.code->proc_fn()(&px);
  if (px.error) std::rethrow_exception(px.error);
  if (bc_error(frame) != BC_OK) throw_context_error(frame);
  Rows out;
  std::vector<Value> row;
  for (std::size_t i = 0; i < def.args.size(); ++i) {
    if (def.args[i].mode == ProcedureArg::Mode::In) continue;
    out.columns.push_back(def.args[i].name);
    out.types.push_back(def.args[i].type);
    row.push_back(px.slots.values[i]);
  }
  if (!out.columns.empty()) out.rows.push_back(padded(out.types, std::move(row)));
  return out;
}

auto Database::call(std::string_view procedure, const std::vector<Value>& args, const ExecutionOptions& opts) -> Rows {
  // Rerun from the arguments on a conflict, as an autocommit statement is
  // retried: kMaxRetries attempts in all.
  for (std::uint32_t attempt = 1;; ++attempt) {
    auto t = begin();
    try {
      Rows out;
      {
        std::shared_lock<std::shared_mutex> lock{life_->mu};
        t.check_usable();
        out = t.call_locked(procedure, args, opts, true);  // compiled into the shared map when the pin is live
      }
      (void)t.commit();
      return out;
    } catch (const SerializationFailure&) {
      if (attempt >= kMaxRetries) throw;
    }
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
  h_->started_us = db.clock_now();
  h_->max_bytes = opts.max_bytes;
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

void Transaction::end_locked(bool committed) noexcept {
  (committed ? life_->committed : life_->rolled_back).fetch_add(1, std::memory_order_relaxed);
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

namespace {
// Under the database's shared lock.
void require_live(const CursorState& s);

// The statement's parameters and the key of the row the cursor stands on.
// (a template: Statement::Compiled is private to its friends).
template <typename Compiled>
auto with_cursor_key(const Compiled& stmt, const CursorState& cursor, const std::vector<Value>& params) -> std::vector<Value> {
  require_live(cursor);
  const auto& c = cursor.plan();
  if (cursor.h == nullptr) throw CurrentOfRefused(CurrentOfRefused::Why::Held);
  if (c.key_columns == 0 || c.key_table != stmt.key_table) throw CurrentOfRefused(CurrentOfRefused::Why::NotSimple);
  if (!cursor.key) throw CurrentOfRefused(CurrentOfRefused::Why::NotOnRow);
  auto out = params;
  out.insert(out.end(), cursor.key->begin(), cursor.key->end());
  return out;
}
}  // namespace

auto Database::shared_txn_plan(const Transaction::Target& t, const std::string& key,
                               const std::optional<std::vector<SqlType>>& fixed, const Published& at)
    -> std::optional<PlanRef> {
  if (t.held != nullptr && !t.cursor_query) {
    auto c = t.held->compiled();
    if (c->plan->catalog_version == at.version()) return PlanRef{c};
    return std::nullopt;
  }
  const auto* s = cached_statement(key);
  // A text no one has prepared, at a pin that is the live catalog: prepared
  // in the shared cache there, as Database::prepare would, since a plan
  // bound at the live catalog is no transaction's own.
  if (s == nullptr && t.share && live_->current()->version() == at.version()) {
    s = &prepare_locked(t.sql, t.cursor_query);
  }
  if (s != nullptr && !s->is_ddl()) {
    auto c = s->compiled();
    if (c->plan->catalog_version == at.version() && (!fixed || caller_types(*c) == *fixed)) return PlanRef{c};
  }
  return std::nullopt;
}

auto Database::txn_plan(TxnHandle& h, const Transaction::Target& t, const TxnCatalog& tc,
                        const std::shared_ptr<const ast::Statement>& parse) -> PlanRef {
  // The shared plan only at the transaction's published catalog: one that
  // has run DDL compiles every statement privately.
  const auto published = tc.published();
  auto key = t.cursor_query ? "\x01" + std::string{t.sql} : std::string{t.sql};
  // The types the run fixed: a held statement's or the caller's record's;
  // none for a run by text, which infers them.
  std::optional<std::vector<SqlType>> fixed;
  if (t.held != nullptr) {
    fixed = caller_types(*t.held->compiled());
  } else if (t.given != nullptr) {
    fixed = types_of(t.given->params);
  }
  if (published) {
    if (auto shared = shared_txn_plan(t, key, fixed, *published)) return std::move(*shared);
  }
  // The transaction's own, compiled for this catalog at these types.
  auto it = h.plans.find(key);
  if (it != h.plans.end()) {
    const auto& e = *it->second;
    if (e.catalog_ == tc.cat && (fixed ? caller_types(e.compiled()) == *fixed : e.inferred_)) return PlanRef{it->second};
  }
  // Compiled privately against the transaction's catalog, never into the shared cache.
  std::shared_ptr<TxnPlan> plan{new TxnPlan{tc.cat, tc.base->version(), !published}};
  plan->parse_ = parse;
  plan->inferred_ = !fixed;
  plan->compiled_.emplace();
  build_into(*plan->compiled_, *parse, t.sql, *tc.cat,
             fixed ? std::span<const SqlType>{*fixed} : std::span<const SqlType>{});
  plan->estimate_ = kTxnPlanBytes + plan->compiled_->plan->source.size();
  if (!bc_txn_charge_plans(h.txn, plan->estimate_)) {
    std::string message;
    try {
      message = bc_txn_error_message(h.txn);
    } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
    }
    throw_error(bc_txn_error(h.txn), bc_txn_error_detail(h.txn), message);
  }
  std::uint64_t replaced = 0;
  try {
    if (it != h.plans.end()) {
      replaced = it->second->estimate_;
      it->second = plan;
    } else {
      h.plans.emplace(std::move(key), plan);
    }
  } catch (...) {
    bc_txn_release_plans(h.txn, plan->estimate_);
    throw;
  }
  h.plan_charge += plan->estimate_ - replaced;
  bc_txn_release_plans(h.txn, replaced);
  return PlanRef{std::shared_ptr<const TxnPlan>{std::move(plan)}};
}

template <typename F>
auto Transaction::as_statement(F&& body) -> decltype(body()) {
  // Whether the transaction has run DDL as the statement starts: its 40001s
  // abort it, recoverably, where a transaction with none ends.
  const auto ddl = h_->txn != nullptr && bc_txn_has_ddl(h_->txn);
  try {
    check_doomed();
    return body();
  } catch (const SerializationFailure&) {
    if (ddl) {
      h_->abort();
    } else {
      end_locked();
    }
    throw;
  } catch (...) {
    // Every other error aborts the transaction, as in PostgreSQL: a
    // statement refused before it ran too.
    h_->abort();
    throw;
  }
}

void Transaction::check_doomed() const {
  // A transaction that has run DDL can only conflict at its commit once
  // another DDL has published: its next statement says so
  // (docs/transaction_design.md, *Catalog snapshots and isolation levels
  // with DDL*).
  if (h_->txn == nullptr || !bc_txn_has_ddl(h_->txn)) return;
  if (db_->live_->current()->version() != bc_txn_catalog_version(h_->txn)) throw_serialization_failure(kCatalogChanged);
}

auto Transaction::parse_of(const Target& t) -> std::shared_ptr<const ast::Statement> {
  const auto key = t.cursor_query ? "\x01" + std::string{t.sql} : std::string{t.sql};
  const Statement* source = t.held != nullptr && !t.cursor_query ? t.held : db_->cached_statement(key);
  // Aliases the statement, which lives until close(), as the transaction may not.
  if (source != nullptr) return std::shared_ptr<const ast::Statement>{std::shared_ptr<const void>{}, &source->ast_};
  if (auto it = h_->plans.find(key); it != h_->plans.end()) return it->second->parse_;
  // A syntax error is the statement's, whatever the catalog.
  auto st = std::make_shared<ast::Statement>(ast::parse(t.sql));
  if (auto* select = std::get_if<ast::Select>(st.get()); select != nullptr && t.cursor_query) select->row_key = true;
  return st;
}

auto Transaction::plan_for(const Target& t, bool describe) -> PlanRef {
  auto parse = parse_of(t);
  if (!is_dml(*parse)) {
    if (describe) return {};
    throw std::logic_error("a DDL has no plan");
  }
  return plan_for(t, parse);
}

auto Transaction::plan_for(const Target& t, const std::shared_ptr<const ast::Statement>& parse) -> PlanRef {
  auto& h = *h_;
  // What the run fixed: its result columns, the version that fixed them,
  // and whether that was a transaction's private catalog.
  std::optional<std::vector<ResultColumn>> columns;
  std::uint64_t fixed_at = 0;
  bool fixed_private = false;
  if (t.held != nullptr) {
    auto c = t.held->compiled();
    columns = result_columns_of(*c);
    fixed_at = c->plan->catalog_version;
  } else if (t.given != nullptr) {
    columns = t.given->columns;
    fixed_at = t.given->version;
    fixed_private = t.given->private_catalog;
  }
  for (auto attempt = 0;; ++attempt) {
    // A first statement that takes the pin itself: one a refit took earlier
    // (a Parse or Bind in a block) is not raced by a DDL since, but older.
    const auto fresh = bc_txn_catalog_version(h.txn) == 0;
    auto cat = bc_txn_catalog(h.txn);
#ifdef BYTECASK_SQL_TESTING
    if (db_->after_txn_pin_) db_->after_txn_pin_();
#endif
    const Database::TxnCatalog tc{bc_txn_pinned_catalog(h.txn), cat};
    const auto base = tc.base->version();
    const auto ddl = bc_txn_has_ddl(h.txn);
    const auto first = fresh && bc_txn_first_statement(h.txn);
    PlanRef plan;
    std::exception_ptr bind_error;
    try {
      plan = db_->txn_plan(h, t, tc, parse);
    } catch (const ast::SqlError&) {
      bind_error = std::current_exception();
    }
    // Exceptions 2 and 3: result columns other than the ones the statement fixed.
    auto changed = !bind_error && columns && result_columns_of(plan.get()) != *columns;
    const auto live = db_->live_->current()->version();
    // The first statement has read nothing: a pin a DDL has since moved past
    // is taken again from the live catalog, as if it had been taken after.
    if (live != base && first && attempt < kTxnPlanAttempts) {
      if (bc_txn_repin(h.txn)) {
        h.clear_plans();
        continue;
      }
    }
    if (bind_error) {
      // Exception 1: the pinned catalog lacks what the statement needs. A
      // transaction that has run DDL reports its own catalog's errors: one
      // whose base is behind was refused before (check_doomed).
      if (live != base && !ddl) throw_serialization_failure(kCatalogChanged);
      std::rethrow_exception(bind_error);
    }
    if (changed) {
      // Exception 2: fixed at a catalog this transaction does not see. A
      // record fixed after another transaction's own DDL at this base may
      // hold the columns that transaction then published, while the live
      // catalog is past the base.
      if (fixed_at > base || (fixed_private && fixed_at == base && !ddl && live != base)) {
        throw_serialization_failure(kCatalogChanged);
      }
      throw_result_type_changed();  // exception 3
    }
    return plan;
  }
}

auto Transaction::run_locked(const Target& t, const std::vector<Value>& params, const ExecutionOptions& opts,
                             WindowRows* window, RowWindow range, const CursorState* positioned) -> Rows {
  return as_statement([&] {
    auto parse = parse_of(t);
    if (!is_dml(*parse)) {
      if (positioned != nullptr || window != nullptr) throw std::invalid_argument("a DDL returns no rows and names no cursor");
      return run_ddl(*parse, params);
    }
    auto plan = plan_for(t, parse);
    const auto& c = plan.get();
    if (positioned != nullptr) {
      return db_->run_txn(h_->cx, c, with_cursor_key(c, *positioned, params), h_->txn, h_->started_us, opts, window,
                          range);
    }
    if (c.key_params != 0) throw ParamError("WHERE CURRENT OF needs a cursor: use execute_current_of");
    return db_->run_txn(h_->cx, c, params, h_->txn, h_->started_us, opts, window, range);
  });
}

void Transaction::check_in_use(const Ddl& ddl, const Catalog& cat) const {
  // PostgreSQL's CheckTableNotInUse: a DDL that changes what an open cursor
  // reads is refused, whatever the cursor has fetched. A cursor's plan names
  // every table and index it reads (its schema ref).
  // A session's bound portals count with the cursors (bind_portal).
  auto read_by_a_cursor = [&](auto&& reads) {
    for (const auto& c : h_->cursors) {
      if (!c->live.load() || !c->compiled) continue;
      const auto& ref = c->plan().ref;
      const std::span<const std::uint32_t> read{ref.objects_read, ref.objects_read == nullptr ? 0 : ref.n_read};
      const std::span<const std::uint32_t> written{ref.tables_written, ref.tables_written == nullptr ? 0 : ref.n_written};
      if (std::ranges::any_of(read, reads) || std::ranges::any_of(written, reads)) return true;
    }
    for (const auto& w : h_->portals) {
      const auto p = w.lock();
      if (p != nullptr && std::ranges::any_of(p->objects, reads)) return true;
    }
    return false;
  };
  auto table_in_use = [&](std::string_view command, const std::string& name) {
    const auto* t = cat.table(name);
    if (t == nullptr) return;
    auto reads = [&](std::uint32_t id) {
      return id == t->id || std::ranges::any_of(t->indexes, [&](const Index& ix) { return ix.id == id; });
    };
    if (read_by_a_cursor(reads)) {
      throw DdlRefused("cannot " + std::string{command} + " \"" + name +
                           "\" because it is being used by active queries in this session",
                       kObjectInUse);
    }
  };
  if (const auto* d = std::get_if<DropTableDdl>(&ddl)) {
    // PostgreSQL checks dependencies before CheckTableNotInUse: a table a
    // view depends on is 2BP01, which next_version raises.
    const auto* t = cat.table(d->name);
    if (t == nullptr || cat.find_view_dependent(t->id) == nullptr) table_in_use("DROP TABLE", d->name);
  } else if (const auto* ci = std::get_if<CreateIndexDdl>(&ddl)) {
    table_in_use("CREATE INDEX", ci->table);
  } else if (const auto* ac = std::get_if<AddColumnDdl>(&ddl)) {
    table_in_use("ALTER TABLE", ac->table);
  } else if (const auto* di = std::get_if<DropIndexDdl>(&ddl)) {
    // Only a cursor that scans the index: one that reads its table some
    // other way does not hold it (PostgreSQL's index_drop).
    const auto [table, ix] = cat.index(di->name);
    if (ix != nullptr && read_by_a_cursor([id = ix->id](std::uint32_t o) { return o == id; })) {
      throw DdlRefused("cannot DROP INDEX \"" + di->name + "\" because it is being used by active queries in this session",
                       kObjectInUse);
    }
  }
}

auto Transaction::run_ddl(const ast::Statement& st, const std::vector<Value>& params) -> Rows {
  auto& h = *h_;
  if (std::holds_alternative<ast::CreateTableAs>(st)) {
    throw FeatureNotSupported("CREATE TABLE AS inside a transaction is not supported: run it outside one");
  }
  if (!params.empty()) throw ParamError("DDL takes no parameters");
  // As PostgreSQL refuses DDL on a standby: a transaction's DDL reaches the
  // engine only at its commit, which a follower would refuse.
  if (db_->db_->mode() == bytecask::Mode::Follower) {
    throw DdlRefused("cannot execute " + std::string{command_name(Statement::command_of(st))} +
                         " in a read-only transaction",
                     kReadOnlySqlTransaction);
  }
  // The DDL builds on the published catalog it pinned, which must still be
  // the live one: its commit would conflict with any DDL published since.
  // A first statement pins again instead, as it would for any statement.
  std::shared_ptr<const Catalog> cat;
  for (auto attempt = 0;; ++attempt) {
    const auto fresh = bc_txn_catalog_version(h.txn) == 0;
    cat = bc_txn_catalog(h.txn);
#ifdef BYTECASK_SQL_TESTING
    if (db_->after_txn_pin_) db_->after_txn_pin_();
#endif
    if (db_->live_->current()->version() == bc_txn_catalog_version(h.txn)) break;
    if (fresh && bc_txn_first_statement(h.txn) && attempt < kTxnPlanAttempts && bc_txn_repin(h.txn)) {
      h.clear_plans();
      continue;
    }
    throw_serialization_failure(kCatalogChanged);
  }
  auto bound = bound::bind(st, *cat);
  const auto& ddl = std::get<Ddl>(bound.stmt);
  check_in_use(ddl, *cat);
  std::vector<Catalog::DdlOp> ops;
  auto backfill = false;
  std::shared_ptr<const Catalog> next;
  {
    // The backfill reads the transaction's view, its own writes included,
    // and is bounded by the transaction's bound as well as the engine's batch.
    const auto rows = bc_txn_rows(h.txn);
    const auto limit = static_cast<std::size_t>(std::min<std::uint64_t>(h.max_bytes, db_->live_->backfill_limit()));
    try {
      next = cat->next_version(ddl, *rows, ops, backfill, limit);
    } catch (const BackfillTooLarge& e) {
      throw ExecError("backfill of table \"" + e.table() + "\" does not fit the transaction's bound of " +
                          std::to_string(limit) + " bytes: run the DDL outside a transaction",
                      BC_E_LIMIT);
    } catch (const ConstraintViolation& e) {
      throw ExecError(e.what(), BC_E_CONSTRAINT, e.detail().encode());
    }
  }
  Rows out;
  out.notices = skip_notices(ddl, *cat, next != nullptr);
  out.affected = 0;
  // A DDL IF [NOT] EXISTS skipped is no DDL of the transaction: nothing changes.
  if (!next) return out;
  if (!bc_txn_ddl(h.txn, std::move(next), ops, backfill)) {
    std::string message;
    try {
      message = bc_txn_error_message(h.txn);
    } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
    }
    throw_error(bc_txn_error(h.txn), bc_txn_error_detail(h.txn), message);
  }
  // The private plans were for the catalog the DDL replaced.
  h.clear_plans();
  return out;
}

auto Transaction::refit_locked(const Target& t, std::shared_ptr<const PortalReads>* reads) -> StatementRecord {
  return as_statement([&] {
    auto plan = plan_for(t, true);
    if (!plan) {
      // A DDL text: described, not bound, and not refused until it runs.
      const auto* s = t.held != nullptr ? t.held : db_->cached_statement(t.sql);
      StatementRecord out;
      out.ddl = true;
      out.command = s != nullptr ? s->command() : Statement::command_of(ast::parse(t.sql));
      return out;
    }
    const auto& c = plan.get();
    // Only a query's portal holds what it reads: PostgreSQL's PortalStart
    // starts the executor at Bind for a plain SELECT alone. An INSERT, UPDATE
    // or DELETE portal, RETURNING or not, opens its tables only while it runs.
    const auto& ref = c.ref;
    if (reads != nullptr && (ref.tables_written == nullptr || ref.n_written == 0)) {
      auto r = std::make_shared<PortalReads>();
      if (ref.objects_read != nullptr) r->objects.assign(ref.objects_read, ref.objects_read + ref.n_read);
      std::erase_if(h_->portals, [](const auto& w) { return w.expired(); });
      h_->portals.push_back(r);
      *reads = std::move(r);
    }
    StatementRecord out;
    out.params = caller_params(c);
    out.columns = result_columns_of(c);
    // A private plan's version is its transaction's published base; one
    // compiled after the transaction's own DDL is marked so.
    out.version = plan.private_ ? plan.private_->version_ : c.plan->catalog_version;
    out.private_catalog = plan.private_ && plan.private_->private_;
    const auto* s = t.held != nullptr ? t.held : db_->cached_statement(t.sql);
    out.command = s != nullptr ? s->command() : Statement::command_of(*plan.private_->parse_);
    if (const auto* u = std::get_if<ast::Update>(s != nullptr ? &s->ast_ : plan.private_->parse_.get())) {
      out.current_of = u->current_of;
      if (!u->current_of.empty()) out.current_of_table = u->table;
    } else if (const auto* d = std::get_if<ast::Delete>(s != nullptr ? &s->ast_ : plan.private_->parse_.get())) {
      out.current_of = d->current_of;
      if (!d->current_of.empty()) out.current_of_table = d->table;
    }
    return out;
  });
}

auto Transaction::prepare(std::string_view sql) -> TxnStatement {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  auto record = refit_locked(Target{.sql = sql, .share = false});
  return TxnStatement{std::string{sql}, std::move(record)};
}

auto Transaction::execute(const Statement& stmt, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  // A misuse of the handle, not a failed statement: the transaction stays as it was.
  if (stmt.owner_ != db_) throw std::logic_error("the statement was prepared on another database");
  return run_locked(Target{stmt.sql_, &stmt}, params, opts);
}

auto Transaction::execute(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                          const ExecutionOptions& opts) -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (h_->state.load() == TxnState::Aborted && !life_->closed && !db_->cached(sql)) {
    // As in PostgreSQL, a syntax error reports that, not 25P02: the raw
    // parse comes before the transaction's state is checked, its analysis after.
    ast::check_syntax(sql);
  }
  check_usable();
  const auto given = check.kind == ResultCheck::Kind::Given;
  return run_locked(Target{sql, nullptr, given ? &check.record : nullptr}, params, opts);
}

auto Transaction::refit(const Statement& stmt) -> StatementRecord {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  if (stmt.owner_ != db_) throw std::logic_error("the statement was prepared on another database");
  return refit_locked(Target{stmt.sql_, &stmt});
}

auto Transaction::refit(std::string_view sql, const ResultCheck& check) -> StatementRecord {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  const auto given = check.kind == ResultCheck::Kind::Given;
  return refit_locked(Target{sql, nullptr, given ? &check.record : nullptr});
}

auto Transaction::bind_portal(std::string_view sql, const ResultCheck& check)
    -> std::pair<StatementRecord, std::shared_ptr<const PortalReads>> {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  const auto given = check.kind == ResultCheck::Kind::Given;
  std::shared_ptr<const PortalReads> reads;
  auto record = refit_locked(Target{sql, nullptr, given ? &check.record : nullptr}, &reads);
  return {std::move(record), std::move(reads)};
}

auto Transaction::execute_current_of(const Statement& stmt, const std::vector<Value>& params, const Cursor& cursor,
                                     const ExecutionOptions& opts) -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  if (stmt.owner_ != db_) throw std::logic_error("the statement was prepared on another database");
  if (stmt.current_of().empty()) throw std::invalid_argument("not an UPDATE or DELETE ... WHERE CURRENT OF");
  if (!cursor.s_) throw std::logic_error("the cursor was moved from");
  // Another transaction's cursor cannot name a row of this one.
  if (cursor.s_->h != nullptr && cursor.s_->h != h_.get()) throw std::logic_error("the cursor belongs to another transaction");
  return run_locked(Target{stmt.sql_, &stmt}, params, opts, nullptr, {}, cursor.s_.get());
}

auto Transaction::execute_current_of(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                                     const Cursor& cursor, const ExecutionOptions& opts) -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  if (!cursor.s_) throw std::logic_error("the cursor was moved from");
  if (cursor.s_->h != nullptr && cursor.s_->h != h_.get()) throw std::logic_error("the cursor belongs to another transaction");
  const auto given = check.kind == ResultCheck::Kind::Given;
  return run_locked(Target{sql, nullptr, given ? &check.record : nullptr}, params, opts, nullptr, {}, cursor.s_.get());
}

auto Transaction::execute_window(const Statement& stmt, const std::vector<Value>& params, RowWindow window,
                                 const ExecutionOptions& opts) -> WindowRows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  if (stmt.owner_ != db_) throw std::logic_error("the statement was prepared on another database");
  auto c = stmt.compiled();
  if (!c || !c->plan->returns_rows) throw std::invalid_argument("execute_window needs a statement that returns rows");
  WindowRows out;
  out.rows = run_locked(Target{stmt.sql_, &stmt}, params, opts, &out, window);
  return out;
}

auto Transaction::execute_window(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                                 RowWindow window, const ExecutionOptions& opts) -> WindowRows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  const auto given = check.kind == ResultCheck::Kind::Given;
  const Target t{sql, nullptr, given ? &check.record : nullptr};
  // The plan that runs says whether the statement returns rows; asking
  // aborts nothing.
  auto plan = as_statement([&] { return plan_for(t, true); });
  if (!plan || !plan.get().plan->returns_rows) {
    throw std::invalid_argument("execute_window needs a statement that returns rows");
  }
  WindowRows out;
  out.rows = run_locked(t, params, opts, &out, window);
  return out;
}

auto Transaction::open_cursor(const Statement& stmt, const std::vector<Value>& params, Holdability hold,
                              Scrolling scrolling) -> Cursor {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  if (stmt.owner_ != db_) throw std::logic_error("the statement was prepared on another database");
  if (stmt.is_ddl() || stmt.command() != Command::Select) throw std::invalid_argument("a cursor needs a SELECT");
  return open_locked(Target{stmt.sql_, &stmt}, params, hold, scrolling);
}

auto Transaction::open_cursor(std::string_view sql, const ResultCheck& check, const std::vector<Value>& params,
                              Holdability hold, Scrolling scrolling) -> Cursor {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  const auto given = check.kind == ResultCheck::Kind::Given;
  const Target t{sql, nullptr, given ? &check.record : nullptr};
  // A misuse, not a failed statement: the transaction stays as it was. Told
  // from the parse, which binds nothing.
  // A text that does not parse fails as a statement does.
  const auto* cached = db_->cached_statement(sql);
  const auto command = as_statement(
      [&] { return cached != nullptr ? cached->command() : Statement::command_of(ast::parse(sql)); });
  if ((cached != nullptr && cached->is_ddl()) || command != Command::Select) {
    throw std::invalid_argument("a cursor needs a SELECT");
  }
  return open_locked(t, params, hold, scrolling);
}

auto Transaction::open_locked(const Target& t, const std::vector<Value>& params, Holdability hold, Scrolling scrolling)
    -> Cursor {
  return as_statement([&] {
    // In the transaction's list before it pins anything: whatever fails
    // after, its abort closes the cursor before the transaction is freed.
    auto state = std::make_shared<CursorState>();
    state->life = life_;
    state->db = db_;
    state->h = h_.get();
    state->scrolling = scrolling;
    state->seq = h_->cursors_opened;
    h_->cursors.push_back(state);
    ++h_->cursors_opened;
    // The cursor reads the query's twin that also reads the table's key,
    // when it is a simple scan of one table (docs/cursor_design.md, *WHERE
    // CURRENT OF*). A query the planner cannot plan with the key keeps its
    // own plan, and then no WHERE CURRENT OF can name the cursor. Any other
    // failure is the open's.
    auto keyed = t;
    keyed.cursor_query = true;
    PlanRef plan;
    try {
      plan = plan_for(keyed);
    } catch (const FeatureNotSupported&) {
      plan = plan_for(t);
    }
    const auto& c = plan.get();
    Database::check_params(c, params);
    state->values = Database::plan_params(c, params, h_->started_us);
    state->compiled = plan;
    auto fail_from_txn = [&] {
      std::string message;
      try {
        message = bc_txn_error_message(h_->txn);
      } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
      }
      throw_error(bc_txn_error(h_->txn), bc_txn_error_detail(h_->txn), message);
    };
    // A private plan is reserved with the pin, so it leaves pinned_bytes,
    // and a hold's share, together with the overlay's copy.
    state->overlay = bc_txn_overlay_pin_reserving(h_->txn, plan.private_bytes());
    if (state->overlay == nullptr) fail_from_txn();
    if (hold == Holdability::Hold) {
      state->hold = bc_txn_hold(h_->txn, state->overlay);
      if (state->hold == nullptr) fail_from_txn();
      // Registered last, so that a failure above leaves nothing in the registry.
      std::lock_guard<std::mutex> lock{life_->open_mu};
      life_->holds.insert(state);
    }
    return Cursor{std::move(state)};
  });
}

namespace {

// What a move reads: rows first .. first + count - 1 (count 0: none, nothing
// runs), returned last row first when `reverse`, and the position it leaves
// the cursor on, before clamping to the result's size.
struct FetchPlan {
  std::uint64_t first{1};
  std::uint64_t count{0};
  bool reverse{false};
  std::uint64_t target{0};
};

constexpr auto kNoLimit = std::numeric_limits<std::uint64_t>::max();

// A count's size: the direction is the caller's.
constexpr auto magnitude(std::int64_t c) -> std::uint64_t {
  if (c == CursorMove::kAll) return kNoLimit;
  return c < 0 ? 0 - static_cast<std::uint64_t>(c) : static_cast<std::uint64_t>(c);
}

constexpr auto saturating_add(std::uint64_t a, std::uint64_t b) -> std::uint64_t {
  return a > kNoLimit - b ? kNoLimit : a + b;
}

// PostgreSQL turns a negative FORWARD count into BACKWARD and the reverse.
struct Step {
  bool backward{false};
  std::uint64_t n{0};
};

constexpr auto step_of(CursorMove m) -> Step {
  auto toward_end = m.kind == CursorMove::Kind::Forward ? m.count >= 0 : m.count < 0;
  return {!toward_end, magnitude(m.count)};
}

// Whether a NO SCROLL cursor refuses the move, as PostgreSQL does: any move
// that goes back, or that would re-read a row at or before the position
// (FETCH 0 and RELATIVE 0 among them, with a row to re-read).
auto refused_when_forward_only(CursorMove m, const CursorState& s) -> bool {
  const auto has_row = s.position >= 1 && !s.after_end();
  switch (m.kind) {
    case CursorMove::Kind::Forward:
    case CursorMove::Kind::Backward: {
      auto st = step_of(m);
      return st.backward || (st.n == 0 && has_row);
    }
    case CursorMove::Kind::Absolute: {
      if (m.count < 0) return true;
      if (m.count == 0) return s.position >= 1;
      auto at = s.total ? std::min(s.position, *s.total) : s.position;
      return static_cast<std::uint64_t>(m.count) <= at;
    }
    case CursorMove::Kind::Relative: return m.count < 0 || (m.count == 0 && has_row);
  }
  return false;
}

// Absolute with a negative count counts from the end: it needs the size.
constexpr auto needs_total(CursorMove m) -> bool { return m.kind == CursorMove::Kind::Absolute && m.count < 0; }

// The window of rows a move reads from position `p` (0 before the first row,
// total + 1 after the last), the arithmetic PostgreSQL's FETCH defines.
// `total` is the result's size when known; it only trims windows that lie
// past it, which a run would find empty anyway.
auto plan_fetch(CursorMove m, std::uint64_t p, std::optional<std::uint64_t> total) -> FetchPlan {
  FetchPlan f;
  auto one_row = [&](std::uint64_t row) {
    f.first = row;
    f.count = 1;
    f.target = row;
  };
  switch (m.kind) {
    case CursorMove::Kind::Forward:
    case CursorMove::Kind::Backward: {
      auto st = step_of(m);
      if (st.n == 0) {  // the current row again
        one_row(p);
        if (p == 0) f.count = 0;
      } else if (!st.backward) {
        f.first = p + 1;
        f.count = st.n;
        f.target = saturating_add(p, st.n);
      } else {
        f.count = p == 0 ? 0 : std::min(st.n, p - 1);
        f.first = p - f.count;
        f.reverse = true;
        f.target = p > st.n ? p - st.n : 0;
      }
      break;
    }
    case CursorMove::Kind::Absolute:
      if (m.count > 0) {
        one_row(static_cast<std::uint64_t>(m.count));
      } else if (m.count < 0) {
        auto from_end = magnitude(m.count);
        if (from_end <= total.value_or(0)) {
          one_row(*total - from_end + 1);
        } else {
          f.target = 0;
        }
      }
      break;
    case CursorMove::Kind::Relative:
      if (m.count > 0) {
        one_row(saturating_add(p, static_cast<std::uint64_t>(m.count)));
      } else if (m.count < 0) {
        auto back = magnitude(m.count);
        if (back < p) one_row(p - back);
      } else {
        one_row(p);
        if (p == 0) f.count = 0;
      }
      break;
  }
  if (f.count > 0 && total) {
    if (f.first > *total) {
      f.count = 0;
    } else {
      f.count = std::min(f.count, *total - f.first + 1);
    }
  }
  return f;
}

}  // namespace

namespace {
// Removes the key a cursor's query appended to each row (Compiled::key_columns,
// the trailing values) and returns the last row's: the row the cursor stands on.
auto take_keys(Rows& rows, std::size_t key_columns) -> std::optional<std::vector<Value>> {
  if (key_columns == 0 || rows.rows.empty()) return std::nullopt;
  std::vector<Value> last;
  for (auto& row : rows.rows) {
    auto tail = row.end() - static_cast<std::ptrdiff_t>(key_columns);
    last.assign(std::make_move_iterator(tail), std::make_move_iterator(row.end()));
    row.erase(tail, row.end());
  }
  return last;
}
}  // namespace

auto Cursor::fetch(std::uint64_t k, const ExecutionOptions& opts) -> Rows {
  if (k == 0) return describe();
  return fetch(CursorMove{CursorMove::Kind::Forward, static_cast<std::int64_t>(std::min<std::uint64_t>(k, CursorMove::kAll))},
               opts);
}

namespace {
// Under the database's shared lock.
void require_live(const CursorState& s) {
  if (s.life->closed || s.life->closing.load()) throw DbClosed();
  if (!s.live.load()) {
    if (s.aborted) throw TransactionAborted();
    if (s.failed.load()) throw std::logic_error("the cursor's fetch failed: it cannot be run");
    throw std::logic_error("the cursor is closed");
  }
}
}  // namespace

auto Cursor::describe() const -> Rows {
  if (!s_) throw std::logic_error("the cursor was moved from");
  std::shared_lock<std::shared_mutex> lock{s_->life->mu};
  require_live(*s_);
  Rows none;
  none.columns = s_->plan().columns;
  none.types = s_->plan().column_types;
  return none;
}

auto Cursor::fetch(CursorMove move, const ExecutionOptions& opts) -> Rows {
  if (!s_) throw std::logic_error("the cursor was moved from");
  std::shared_lock<std::shared_mutex> lock{s_->life->mu};
  // An aborted transaction with savepoints keeps its cursors open, for a
  // rollback to one: until then they fetch nothing.
  if (s_->h != nullptr && s_->h->state.load() == TxnState::Aborted && !s_->life->closed) throw TransactionAborted();
  require_live(*s_);
  auto& st = *s_;
  if (st.scrolling == Scrolling::ForwardOnly && refused_when_forward_only(move, st)) throw CursorForwardOnly();
  auto* h = st.h;
  const auto was_plain = !st.holdable();
  // A hold cursor reads its own view, in a context of the pool: nothing of the
  // transaction is read, so it works before and after the transaction ends.
  auto run = [&](RowWindow w) {
    WindowRows out;
    out.rows = st.holdable() ? st.db->run_held(st, w, out, opts) : st.db->run_pinned(h->cx, h->txn, st, w, out, opts);
    return out;
  };
  std::optional<std::vector<Value>> new_key;
  try {
    // A run to the end that keeps one row learns the size, once.
    if (needs_total(move) && !st.total) st.total = run(RowWindow{kNoLimit, 1}).position;
    auto plan = plan_fetch(move, st.position, st.total);
    Rows rows;
    if (plan.count > 0) {
      auto out = run(RowWindow{plan.first - 1, plan.count});
      if (out.end) st.total = out.position;
      rows = std::move(out.rows);
      if (plan.reverse) std::ranges::reverse(rows.rows);
      new_key = take_keys(rows, st.plan().key_columns);
    } else {
      rows.columns = st.plan().columns;
      rows.types = st.plan().column_types;
    }
    auto before = std::exchange(st.position, st.total ? std::min(plan.target, *st.total + 1) : plan.target);
    // The cursor stands on the last row it returned, when it stands on a row.
    if (st.position == 0 || st.after_end()) {
      st.key.reset();
    } else if (new_key) {
      st.key = std::move(new_key);
    } else if (st.position != before) {
      st.key.reset();
    }
    return rows;
  } catch (...) {
    // A failed fetch closes this cursor, which stays in its transaction's
    // list, failed, for rollback_to.
    if (h != nullptr) st.failed.store(true);
    st.close();
    // A hold cursor's closes no other; any other's aborts the transaction
    // (which closes the others too, unless it has savepoints).
    if (was_plain && h != nullptr) h->abort();
    throw;
  }
}

void Cursor::close() noexcept {
  if (!s_) return;
  std::shared_lock<std::shared_mutex> lock{s_->life->mu};
  if (!s_->live.load()) {  // closed already, with its transaction or the database, or by its failed fetch
    s_->failed.store(false);
    return;
  }
  if (s_->h != nullptr) std::erase(s_->h->cursors, s_);
  s_->close();
}

auto Database::shared_procedure(const Procedure& def, bool share, const Published& at)
    -> std::shared_ptr<const CompiledProcedure> {
  {
    std::lock_guard<std::mutex> lock{procedures_mu_};
    auto it = procedures_.find(def.name);
    if (it != procedures_.end() && it->second->id == def.id && it->second->catalog_version == at.version()) {
      return it->second;
    }
  }
  if (share && live_->current()->version() == at.version()) {
    auto p = procedure_plan(def);
    if (p->catalog_version == at.version() && p->id == def.id) return p;
  }
  return nullptr;
}

auto Database::txn_procedure(TxnHandle& h, const Procedure& def, const TxnCatalog& tc, bool share)
    -> std::shared_ptr<const CompiledProcedure> {
  // The shared plan, only when compiled at exactly the pin, by a transaction
  // that has run no DDL. A procedure called in autocommit (Database::call)
  // compiles it there when the pin is live.
  if (const auto published = tc.published()) {
    if (auto p = shared_procedure(def, share, *published)) return p;
  }
  auto key = "\x02" + def.name;
  auto it = h.plans.find(key);
  if (it != h.plans.end() && it->second->catalog_ == tc.cat && it->second->procedure_->id == def.id) {
    // Aliases the TxnPlan, which keeps the procedure alive.
    return {it->second, &*it->second->procedure_};
  }
  std::shared_ptr<TxnPlan> plan{new TxnPlan{tc.cat, tc.base->version(), !tc.published()}};
  plan->procedure_.emplace();
  build_procedure(*plan->procedure_, def, *tc.cat);
  plan->estimate_ = kTxnPlanBytes + plan->procedure_->gen.source.size();
  if (!bc_txn_charge_plans(h.txn, plan->estimate_)) {
    std::string message;
    try {
      message = bc_txn_error_message(h.txn);
    } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
    }
    throw_error(bc_txn_error(h.txn), bc_txn_error_detail(h.txn), message);
  }
  std::uint64_t replaced = 0;
  try {
    if (it != h.plans.end()) {
      replaced = it->second->estimate_;
      it->second = plan;
    } else {
      h.plans.emplace(std::move(key), plan);
    }
  } catch (...) {
    bc_txn_release_plans(h.txn, plan->estimate_);
    throw;
  }
  h.plan_charge += plan->estimate_ - replaced;
  bc_txn_release_plans(h.txn, replaced);
  const auto* proc = &*plan->procedure_;
  return {std::shared_ptr<const TxnPlan>{std::move(plan)}, proc};
}

auto Transaction::procedure(std::string_view name) -> std::shared_ptr<const Procedure> {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  return as_statement([&]() -> std::shared_ptr<const Procedure> {
    for (auto attempt = 0;; ++attempt) {
      const auto fresh = bc_txn_catalog_version(h_->txn) == 0;
      auto cat = bc_txn_catalog(h_->txn);
      const auto* def = cat->procedure(name);
      if (def != nullptr) return {cat, def};
      // A binder error at a stale pin (exception 1); a first statement
      // re-pins. A transaction that has run DDL reports its own catalog's.
      if (bc_txn_has_ddl(h_->txn) || db_->live_->current()->version() == bc_txn_catalog_version(h_->txn)) {
        return nullptr;
      }
      if (fresh && bc_txn_first_statement(h_->txn) && attempt < kTxnPlanAttempts && bc_txn_repin(h_->txn)) {
        h_->clear_plans();
        continue;
      }
      throw_serialization_failure(kCatalogChanged);
    }
  });
}

auto Transaction::call(std::string_view procedure, const std::vector<Value>& args, const ExecutionOptions& opts)
    -> Rows {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  return call_locked(procedure, args, opts, false);
}

auto Transaction::call_locked(std::string_view procedure, const std::vector<Value>& args, const ExecutionOptions& opts,
                              bool share) -> Rows {
  return as_statement([&] {
    for (auto attempt = 0;; ++attempt) {
      // The procedure as the transaction's catalog has it, compiled there.
      // The hook runs as each statement of the body begins (run_procedure).
      const auto fresh = bc_txn_catalog_version(h_->txn) == 0;
      auto cat = bc_txn_catalog(h_->txn);
      const Database::TxnCatalog tc{bc_txn_pinned_catalog(h_->txn), cat};
      const auto base = tc.base->version();
      const auto first = fresh && bc_txn_first_statement(h_->txn);
      const auto* def = cat->procedure(procedure);
      std::shared_ptr<const CompiledProcedure> plan;
      std::exception_ptr bind_error;
      if (def == nullptr) {
        bind_error = std::make_exception_ptr(
            ast::SqlError{"procedure " + std::string{procedure} + " does not exist", -1, kUndefinedFunction});
      } else {
        try {
          plan = db_->txn_procedure(*h_, *def, tc, share);
        } catch (const ast::SqlError&) {
          bind_error = std::current_exception();
        }
      }
      const auto live = db_->live_->current()->version();
      if (live != base && first && attempt < kTxnPlanAttempts && bc_txn_repin(h_->txn)) {
        h_->clear_plans();
        continue;
      }
      if (bind_error) {
        if (live != base && !bc_txn_has_ddl(h_->txn)) throw_serialization_failure(kCatalogChanged);
        std::rethrow_exception(bind_error);
      }
      return db_->run_procedure(*plan, *def, args, *h_, opts);
    }
  });
}

auto Transaction::execute(std::string_view sql, const std::vector<Value>& params, const ExecutionOptions& opts)
    -> Rows {
  return execute(sql, ResultCheck::none(), params, opts);
}

auto Transaction::find_savepoint(std::string_view name) -> std::size_t {
  const auto& sp = h_->savepoints;
  for (auto i = sp.size(); i > 0; --i) {
    if (sp[i - 1].name == name) return i - 1;
  }
  if (h_->state.load() == TxnState::Open) h_->abort();
  throw ast::SqlError{"savepoint \"" + std::string{name} + "\" does not exist", -1, kInvalidSavepointSpecification};
}

void Transaction::savepoint(std::string_view name) {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  try {
    check_doomed();
    // Room first, so that nothing after the runtime's mark can fail.
    Savepoint sp{std::string{name}, h_->cursors_opened};
    h_->savepoints.reserve(h_->savepoints.size() + 1);
    if (bc_txn_savepoint(h_->txn) == 0) {
      std::string message;
      try {
        message = bc_txn_error_message(h_->txn);
      } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch): the message is dropped, not the outcome
      }
      throw_error(bc_txn_error(h_->txn), bc_txn_error_detail(h_->txn), message);
    }
    h_->savepoints.push_back(std::move(sp));
  } catch (...) {
    h_->abort();  // as a failed statement: SAVEPOINT that fails aborts in PostgreSQL too
    throw;
  }
}

void Transaction::rollback_to(std::string_view name) {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (life_->closed || life_->closing.load()) throw DbClosed();
  if (h_->state.load() == TxnState::Ended) throw std::logic_error("the transaction has ended");
  // Not check_usable: an aborted transaction is what this is for.
  auto k = find_savepoint(name);
  bc_ctx_reset(h_->cx);  // no statement is open between calls; a reset one aborted the runtime's already
  // Never refused: the mark exists (one per savepoint) and no statement is open.
  if (!bc_txn_rollback_to(h_->txn, static_cast<std::uint32_t>(k + 1))) {
    h_->abort();
    throw std::logic_error("the runtime refused a rollback to a live savepoint");
  }
  // The DDL since the mark is undone with its catalog: plans compiled for
  // that catalog are never used again (TxnHandle::plans), and go.
  h_->drop_plans_not_for(bc_txn_current_catalog(h_->txn));
  // The cursors opened since the savepoint close, a failed one with them:
  // forgotten, as if never opened. Those opened before stay as they are.
  const auto since = h_->savepoints[k].cursors;
  std::erase_if(h_->cursors, [since](const auto& c) {
    if (c->seq < since) return false;
    c->failed.store(false);
    c->close();
    return true;
  });
  h_->savepoints.erase(h_->savepoints.begin() + static_cast<std::ptrdiff_t>(k + 1), h_->savepoints.end());
  h_->state.store(TxnState::Open);
}

void Transaction::release(std::string_view name) {
  if (!h_) throw std::logic_error("the transaction has ended");
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  check_usable();
  try {
    check_doomed();
  } catch (...) {
    h_->abort();
    throw;
  }
  auto k = find_savepoint(name);
  if (!bc_txn_release(h_->txn, static_cast<std::uint32_t>(k + 1))) {
    h_->abort();
    throw std::logic_error("the runtime refused to release a live savepoint");
  }
  h_->savepoints.erase(h_->savepoints.begin() + static_cast<std::ptrdiff_t>(k), h_->savepoints.end());
}

void Transaction::fail() noexcept {
  if (!h_) return;
  std::shared_lock<std::shared_mutex> lock{life_->mu};
  if (h_->state.load() == TxnState::Open) h_->abort();
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
  if (r == BC_COMMITTED) h_->commit_cursors();  // the hold cursors leave before the transaction is freed
  end_locked(r == BC_COMMITTED);
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
  auto param_info = bound.params;
  auto fixed = bound.fixed;
  auto clocks = bound.clocks;
  std::vector<Rows> out;
  for (auto& plan : generate_alternatives(std::move(bound), *cat, sql, row_counts(), budget_.max_bytes)) {
    if (!plan.returns_rows) throw std::logic_error("alternatives of a statement that writes");
    Compiled c;
    c.param_sql = param_sql;
    c.param_table = param_table;
    c.params = param_info;
    c.fixed = fixed;
    c.clocks = clocks;
    c.plan = std::make_unique<GeneratedPlan>(std::move(plan));
    check_params(c, params);
    link(c);
    auto* cx = acquire_context();
    try {
      bc_params bp{plan_params(c, params, clock_now())};
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
  // are read in, without the hidden row id: the columns a statement sees. A
  // column a row's bitmap has no bit for reads as NULL.
  auto layout = t->layout;
  layout.columns.clear();
  for (const auto& c : t->columns) {
    if (c.hidden) continue;
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
