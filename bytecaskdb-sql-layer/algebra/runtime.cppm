// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// runtime.cppm — the C++ side of the plan API: the types bc_plan.h leaves
// opaque, built by the server (or a test) before it creates a context, and
// the table of plan-callable functions a run-time loader links a plan
// against. The API's definitions are in plan_api.cpp, a plain translation unit
// behind the C header; the symbol table's are here.

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "bytecaskdb-sql-layer/algebra/bc_proc.h"
#include "include/bytecask.hpp"

export module bytecask.sql.runtime;

export import bytecask.sql.catalog;

// One bound parameter: SQL NULL, an integer, a double, a boolean or bytes;
// the catalog's Datum, which defaults are stored as.
export using bc_value = bytecask::sql::Datum;

// bc_plan.h declares these in the global module. A definition in this
// module's purview would attach them to the module, a second declaration of
// the same entity the compiler rejects; a linkage specification keeps the
// definitions in the global module ([basic.link]/10), where plan_api.cpp
// defines bc_ctx and the extern "C" functions. The definitions are not exported: a
// redeclaration of an entity the global module introduced cannot be
// ([module.interface]/6). The using-declarations below export the names, as
// the standard library's module exports its header-declared names.
extern "C++" {

// The memory every statement and transaction of one database holds
// together, against one cap (DatabaseOptions::max_memory_bytes). Each
// statement reserves from `used` in steps, at least what it holds, and
// returns its reservation when it ends; a transaction takes over what its
// statements' write sets reserved and returns it when it ends. Not owned.
struct bc_memory_cap {
  std::atomic<std::uint64_t> used{0};
  std::uint64_t cap;
};

// The open engine a context reads through and commits to, and the memory
// cap its statements and transactions reserve from (null: none). Not owned.
struct bc_db {
  bytecask::DB* db;
  bc_memory_cap* memory{nullptr};
};

// The live catalog. Not owned; must outlive every context built on it. A
// context pins one version of it per execution.
struct bc_catalog {
  bytecask::sql::LiveCatalog* live;
};

struct bc_params {
  std::vector<bc_value> values;
};

// A running procedure (docs/procedures_design.md, *The statement
// boundary*): its frame, whose parameters are its slots, and the caller's
// hooks that begin and end each statement on its transaction. A hook throws
// to fail the procedure; the first exception is kept for the caller, and the
// frame fails so every later call does nothing.
struct bc_proc {
  bc_ctx* frame{nullptr};
  bc_params slots;
  std::vector<bytecask::sql::SqlType> types;  // each slot's declared type
  std::function<bc_ctx*(std::uint32_t)> begin;
  std::function<void(std::uint32_t)> end;
  std::function<bool(std::uint32_t)> next;  // bc_rows_next: false once the rows are done
  std::function<void(std::uint32_t)> done;  // bc_rows_done
  std::exception_ptr error;
  struct Range {
    std::int64_t next{0}, last{0};
    bool reverse{false}, done{true};
  };
  std::vector<Range> ranges;
};

// Begins `cx` as a procedure's frame: its parameters and budget, and no
// snapshot, since it reads no table.
void bc_ctx_begin_frame(bc_ctx* cx, const bc_params* params, bc_budget budget);

// The transaction's private plans (docs/transaction_design.md, *Mechanism:
// plans compiled against the transaction's catalog). The API compiles them
// against the catalog the transaction pinned, which bc_txn_pinned_catalog
// pins first if no statement has: with the snapshot, as a first statement
// would. It throws std::logic_error for an aborted or ended transaction or
// one with a statement open, and what the pin throws.
std::shared_ptr<const bytecask::sql::Catalog> bc_txn_pinned_catalog(bc_txn* txn);
// The catalog the transaction's statements bind against: the pinned one,
// pinned first as bc_txn_pinned_catalog does, plus the transaction's own
// DDL (docs/transaction_design.md, *Mechanism: the transaction's private
// catalog*). Throws as bc_txn_pinned_catalog does.
std::shared_ptr<const bytecask::sql::Catalog> bc_txn_catalog(bc_txn* txn);
// The transaction's catalog as it stands, without pinning: null before its
// first statement and after an abort that dropped its pin.
const bytecask::sql::Catalog* bc_txn_current_catalog(const bc_txn* txn) noexcept;
// The transaction has run DDL: its catalog is not the one it pinned. A
// rollback to a mark taken before its first DDL makes it false again.
bool bc_txn_has_ddl(const bc_txn* txn) noexcept;
// The rows a DDL of the transaction reads: its snapshot with its writes
// over them, one key prefix at a time. Valid until the transaction's next
// statement, DDL or commit. Pins first; throws as bc_txn_pinned_catalog does.
std::unique_ptr<bytecask::sql::RowSource> bc_txn_rows(bc_txn* txn);
// A DDL of the transaction: `next`, the catalog Catalog::next_version made
// from bc_txn_catalog, and its batch `ops`, which must guard and bump the
// catalog version. The writes join the overlay (each op's bytes freed as it
// does), a backfill's row guards join the transaction's guards, the name
// and version guards are dropped (the commit guards the version), and the
// catalog is charged to max_bytes and the memory cap; then `next` is the
// transaction's catalog. False with bc_txn_error set when it fails (BC_E_LIMIT
// past max_bytes, BC_E_MEMORY), and the transaction is aborted, as for a
// failed statement: back to its newest mark, catalog included, if it has one.
// `backfill`: the DDL rewrote or indexed rows.
bool bc_txn_ddl(bc_txn* txn, std::shared_ptr<const bytecask::sql::Catalog> next,
                std::vector<bytecask::sql::Catalog::DdlOp>& ops, bool backfill) noexcept;
// No statement was added and no cursor pinned the overlay: bc_txn_repin would re-pin.
bool bc_txn_first_statement(const bc_txn* txn) noexcept;
// Charges `bytes` of private plans to max_bytes and the memory cap: false,
// with bc_txn_error BC_E_LIMIT or BC_E_MEMORY and nothing charged, when
// either has no room. The transaction is not aborted. The charge is not a
// mark's: a rollback to one keeps it. An abort that drops the pin zeroes it.
bool bc_txn_charge_plans(bc_txn* txn, std::uint64_t bytes) noexcept;
// Returns what bc_txn_charge_plans charged, once the plans are dropped.
void bc_txn_release_plans(bc_txn* txn, std::uint64_t bytes) noexcept;
// bc_txn_overlay_pin, the pin also reserving `extra` bytes against max_bytes
// until released: a cursor's private plan (docs/cursor_design.md).
bc_overlay* bc_txn_overlay_pin_reserving(bc_txn* txn, std::uint64_t extra);
// Whether the last commit's BC_CONFLICT, the transaction's or the context's
// autocommit execution's at SERIALIZABLE, found no key it wrote changed
// since its snapshot: a read/write dependency, reported with its own
// message (docs/transaction_design.md, *Mechanism: the serializable
// commit*, step 5). False after any other outcome.
bool bc_txn_conflict_read_write(const bc_txn* txn) noexcept;
bool bc_ctx_conflict_read_write(const bc_ctx* cx) noexcept;

// A SERIALIZABLE read set's structural transitions (tests/CLAUDE.md, *Fault
// sweeps must reach every structural transition*): a new point, a new range,
// a merge of ranges, a point a range swallowed, and a cursor's reservation
// filled. Counted, process-wide, in a testing build only.
enum class ReadSetTransition : std::uint8_t { NewPoint, NewRange, Merge, Swallow, Fill };
inline constexpr std::size_t kReadSetTransitions = 5;

#ifdef BYTECASK_SQL_TESTING
// Test only: what bc_txn_charge_plans charged and not released, and what
// the transaction reserves of the memory cap for itself.
std::uint64_t bc_txn_plan_bytes_for_testing(const bc_txn* txn) noexcept;
std::uint64_t bc_txn_cap_holds_for_testing(const bc_txn* txn) noexcept;
// Test only (sql_layer_testing): runs `hook` in every bc_ctx_commit of the
// context, after the plan ran and before its batch reaches the engine, so a
// test can make the commit conflict. An empty function removes it.
void bc_ctx_before_commit_for_testing(bc_ctx* cx, std::function<void()> hook);
// Test only: a runtime call that fails the statement with BC_E_DATA or
// BC_E_ARITH (another code is BC_E_HANDLE), `state` in its detail and the
// four lines as given, as a call that builds its error text does: what a
// plan uses to reach bc_error_hint and bc_error_context, which no runtime
// call sets before jsonb.
void bc_ctx_fail_for_testing(bc_ctx* cx, bc_error_code code, bytecask::sql::SqlState state, const char* message,
                             const char* note, const char* hint, const char* context);
// Test only: the bytes the context's statement is charged now.
std::uint64_t bc_ctx_held_for_testing(const bc_ctx* cx);
// Test only: a SERIALIZABLE read set's entries, in key order, what they are
// charged (`bytes`), and what the transaction's read set is charged to its
// bound (`charged`: `bytes` and the open cursors' reservations). Of the
// transaction, or of the context's autocommit execution.
struct ReadSetContents {
  std::vector<bytecask::sql::Bytes> points;
  std::vector<std::pair<bytecask::sql::Bytes, bytecask::sql::Bytes>> ranges;
  std::uint64_t bytes{0};
  std::uint64_t charged{0};
};
ReadSetContents bc_txn_reads_for_testing(const bc_txn* txn);
ReadSetContents bc_ctx_reads_for_testing(const bc_ctx* cx);
// Test only: a fresh read set after each operation of `ops` in order, a
// point (`hi` empty) or a range [lo, hi): the model test's subject.
struct ReadSetOp {
  bytecask::sql::Bytes lo, hi;
};
ReadSetContents read_set_apply_for_testing(std::span<const ReadSetOp> ops);
// Test only: k⁺, the bound a forward cursor that stopped after `k` records.
bytecask::sql::Bytes read_successor_for_testing(bytecask::sql::Bytes k);
// Test only: the transitions above (ReadSetTransition), counted since the
// last call with `reset`.
std::array<std::uint64_t, kReadSetTransitions> read_set_transitions_for_testing(bool reset);
// Test only: the capacity, in elements, of the write set's vectors (effects,
// claims, locks, the tables written).
std::size_t bc_ctx_write_set_slots_for_testing(const bc_ctx* cx);
#endif

}  // extern "C++"

export using ::bc_db;
export using ::bc_memory_cap;
export using ::bc_catalog;
export using ::bc_params;
export using ::bc_proc;
export using ::bc_proc_fn;
export using ::bc_ctx_begin_frame;
export using ::bc_ctx_affected;
export using ::bc_txn_pinned_catalog;
export using ::bc_txn_first_statement;
export using ::bc_txn_catalog;
export using ::bc_txn_has_ddl;
export using ::bc_txn_current_catalog;
export using ::bc_txn_rows;
export using ::bc_txn_ddl;
export using ::bc_txn_charge_plans;
export using ::bc_txn_release_plans;
export using ::bc_txn_overlay_pin_reserving;
export using ::bc_txn_conflict_read_write;
export using ::ReadSetTransition;
export using ::kReadSetTransitions;
export using ::bc_ctx_conflict_read_write;
#ifdef BYTECASK_SQL_TESTING
export using ::bc_txn_plan_bytes_for_testing;
export using ::bc_txn_cap_holds_for_testing;
export using ::bc_ctx_before_commit_for_testing;
export using ::bc_ctx_fail_for_testing;
export using ::bc_ctx_held_for_testing;
export using ::ReadSetContents;
export using ::ReadSetOp;
export using ::bc_txn_reads_for_testing;
export using ::bc_ctx_reads_for_testing;
export using ::read_set_apply_for_testing;
export using ::read_successor_for_testing;
export using ::read_set_transitions_for_testing;
export using ::bc_ctx_write_set_slots_for_testing;
#endif

export namespace bytecask::sql {

// A procedure's failure that is no statement's: a NULL FOR bound (22004),
// STRICT's no row (P0002) or second row (P0003).
class ProcedureError : public std::runtime_error {
public:
  ProcedureError(const std::string& what, SqlState state) : std::runtime_error{what}, state_{state} {}
  ProcedureError(const ProcedureError&) = default;
  ProcedureError& operator=(const ProcedureError&) = default;
  ~ProcedureError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto sqlstate() const noexcept -> std::string_view { return state_.view(); }

private:
  SqlState state_;
};

// What the plan API charges a statement's memory budget for an entry of its
// own, beside the bytes the entry holds (docs/plan_api_design.md, *Memory*):
// each handle a plan is given (a view, a pin, a key or row builder, a
// cursor, a hash table probe), each sorter, hash table and aggregate the
// plan opens, each aggregate group's node and slot, and each sorter and
// hash table entry's node. Fixed numbers, so a statement
// is charged the same on every run and every build; plan_api.cpp checks
// that each is no less than what it stands for.
inline constexpr std::uint64_t kViewEntryBytes = 40;
inline constexpr std::uint64_t kKeyEntryBytes = 64;
inline constexpr std::uint64_t kRowEntryBytes = 152;
inline constexpr std::uint64_t kCursorEntryBytes = 408;
inline constexpr std::uint64_t kGroupEntryBytes = 136;
inline constexpr std::uint64_t kAggSlotBytes = 48;
inline constexpr std::uint64_t kSortEntryBytes = 128;
inline constexpr std::uint64_t kHashEntryBytes = 240;
inline constexpr std::uint64_t kHashIterEntryBytes = 32;
inline constexpr std::uint64_t kSorterBytes = 192;
inline constexpr std::uint64_t kHashBytes = 192;
inline constexpr std::uint64_t kAggBytes = 320;

// What the write set is charged for what it holds (docs/plan_api_design.md,
// *The write set is statement memory*): a conservative bound, never less
// than the memory used, and the one size both of its bounds count, the
// statement budget as a call adds an item and a transaction's max_bytes as
// bc_txn_add takes it in. Each heap block costs its capacity and
// kAllocBytes, the allocator's header and rounding; each item a slot in its
// vector, which grows to at most twice its size; and each row, unique prefix
// or range the statement wrote, to refuse a second write, the node it takes
// in its set. Each effect and table also costs its entry in the commit's
// WritePlan, which copies its buffers, in autocommit and in a transaction
// alike, so no commit needs memory its statements were not charged; and a
// write in a transaction the node it takes in the overlay.
inline constexpr std::uint64_t kAllocBytes = 32;
inline constexpr std::uint64_t kEffectSlotBytes = 112;
inline constexpr std::uint64_t kClaimSlotBytes = 112;
inline constexpr std::uint64_t kLockSlotBytes = 64;
inline constexpr std::uint64_t kTableSlotBytes = 8;  // a table's, the first time it is written
// The most slots each of the write set's vectors keeps across a reset,
// cleared and uncharged: bounded scratch, so small statements on a pooled
// context do not regrow them. A vector grown past it is freed.
inline constexpr std::size_t kKeptWriteSlots = 32;
inline constexpr std::uint64_t kOverlayNodeBytes = 120;
inline constexpr std::uint64_t kWrittenRowNodeBytes = 96;
inline constexpr std::uint64_t kUniqueNodeBytes = 96;
inline constexpr std::uint64_t kRangeNodeBytes = 112;
// A WritePlan's: a put, delete or range delete's slot in its writes; a point
// guard's node in its map of guards; a range guard's slot in its list.
inline constexpr std::uint64_t kPlanWriteSlotBytes = 112;
inline constexpr std::uint64_t kPlanKeyGuardNodeBytes = 96;
inline constexpr std::uint64_t kPlanRangeGuardSlotBytes = 96;

[[nodiscard]] constexpr auto buffer_bytes(std::size_t capacity) -> std::uint64_t {
  return capacity == 0 ? 0 : capacity + kAllocBytes;
}
// The one size of a buffer the write set holds: its capacity, whatever its
// size. A copy made from a view has its size as its capacity.
[[nodiscard]] constexpr auto held_bytes(const Bytes& b) -> std::uint64_t { return buffer_bytes(b.capacity()); }

// A WritePlan entry's slot or node: the engine copies its key, and its value
// or range end, into buffers of their size.
enum class PlanEntry : std::uint8_t { Write, KeyGuard, RangeGuard };
[[nodiscard]] constexpr auto plan_entry_bytes(PlanEntry e, std::size_t a, std::size_t b) -> std::uint64_t {
  std::uint64_t slot = 0;
  switch (e) {
    case PlanEntry::Write: slot = kPlanWriteSlotBytes; break;
    case PlanEntry::KeyGuard: slot = kPlanKeyGuardNodeBytes; break;
    case PlanEntry::RangeGuard: slot = kPlanRangeGuardSlotBytes; break;
  }
  return buffer_bytes(a) + buffer_bytes(b) + slot;
}
// An effect with buffers of these capacities, whose plan entry is `e`, and
// in a transaction a write's overlay node.
[[nodiscard]] constexpr auto effect_bytes(bool in_txn, PlanEntry e, std::size_t a, std::size_t b) -> std::uint64_t {
  auto own = buffer_bytes(a) + buffer_bytes(b) + kEffectSlotBytes + plan_entry_bytes(e, a, b);
  return own + (in_txn && e == PlanEntry::Write ? kOverlayNodeBytes : 0);
}
// A table the writes are in: its slot and the commit's guard on its record,
// a key of `record_key` bytes.
[[nodiscard]] constexpr auto table_bytes(std::size_t record_key) -> std::uint64_t {
  return kTableSlotBytes + plan_entry_bytes(PlanEntry::KeyGuard, record_key, 0);
}
// A SERIALIZABLE read set's entries (docs/transaction_design.md, *Read-set
// memory and the 54000 bound*): a point's node in its set and a range's in
// its map, each with its buffers and its guard's entry in the commit's
// WritePlan. A point is charged the larger of its two guard forms: it goes
// as a one-key range when a hand-written guard sits on its key.
inline constexpr std::uint64_t kReadPointNodeBytes = 96;
inline constexpr std::uint64_t kReadRangeNodeBytes = 112;
[[nodiscard]] constexpr auto read_point_bytes(std::size_t key) -> std::uint64_t {
  const auto as_point = plan_entry_bytes(PlanEntry::KeyGuard, key, 0);
  const auto as_range = plan_entry_bytes(PlanEntry::RangeGuard, key, key + 1);
  return buffer_bytes(key) + kReadPointNodeBytes + (as_point > as_range ? as_point : as_range);
}
[[nodiscard]] constexpr auto read_range_bytes(std::size_t lo, std::size_t hi) -> std::uint64_t {
  return buffer_bytes(lo) + buffer_bytes(hi) + kReadRangeNodeBytes + plan_entry_bytes(PlanEntry::RangeGuard, lo, hi);
}
[[nodiscard]] constexpr auto claim_bytes(std::size_t key) -> std::uint64_t {
  return buffer_bytes(key) + kClaimSlotBytes;
}
[[nodiscard]] constexpr auto lock_bytes(std::size_t key) -> std::uint64_t { return buffer_bytes(key) + kLockSlotBytes; }

// Every column of a row, by its layout: NULL, or the typed value; the id of
// the first malformed column (row_column), never a plausible value. The one
// decoder of whole rows: result rows for the embedded API and the tests,
// stored rows for Database::table_rows.
[[nodiscard]] auto decode_row(const RowLayout& layout, BytesView row)
    -> std::expected<std::vector<bc_value>, std::uint32_t>;


// A plan's result_types as the bc_type codes a bc_schema_ref carries.
[[nodiscard]] auto result_type_codes(std::span<const PhysType> types) -> std::vector<std::uint8_t>;

}  // namespace bytecask::sql

export struct ApiSymbol {
  const char* name;
  const void* address;
};

// Every plan-callable function of bc_plan.h, by name, for loaders that link a
// plan at run time (TinyCC's tcc_add_symbol). Runtime calls (bc_ctx_*,
// bc_txn_*, bc_execute, bc_error*) are left out: a plan never calls them.
// tests/plan_api_test.cpp checks the table against the header.
export [[nodiscard]] auto plan_api_symbols() -> std::span<const ApiSymbol>;

namespace {

// A function's address as the void* a C loader takes. Converting a function
// pointer to void* is conditionally supported; POSIX requires it (dlsym).
template <typename F>
auto fn_addr(F* f) -> const void* {
  return reinterpret_cast<const void*>(f);
}

}  // namespace

namespace bytecask::sql {

ProcedureError::~ProcedureError() = default;


auto decode_row(const RowLayout& layout, BytesView row) -> std::expected<std::vector<bc_value>, std::uint32_t> {
  std::vector<bc_value> out;
  out.reserve(layout.columns.size());
  for (const auto& c : layout.columns) {
    auto v = row_column(row, c);
    switch (v.state) {
      case ColumnState::Malformed: return std::unexpected(c.col_id);
      case ColumnState::Absent: out.emplace_back(std::monostate{}); continue;
      case ColumnState::Value: break;
    }
    switch (c.type) {
      case PhysType::I64: out.emplace_back(v.i64()); break;
      case PhysType::F64: out.emplace_back(v.f64()); break;
      case PhysType::Bool: out.emplace_back(v.boolean()); break;
      case PhysType::Bytes:
      case PhysType::Num:
      case PhysType::Jsonb: out.emplace_back(Bytes(v.bytes.begin(), v.bytes.end())); break;  // a result's jsonb is its text
    }
  }
  return out;
}

auto result_type_codes(std::span<const PhysType> types) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  out.reserve(types.size());
  for (auto t : types) out.push_back(static_cast<std::uint8_t>(t));
  return out;
}

}  // namespace bytecask::sql

auto plan_api_symbols() -> std::span<const ApiSymbol> {
  // Function-local: initialised on first use, not by a global constructor.
  static const ApiSymbol kSymbols[] = {
      {"bc_proc_ctx", fn_addr(bc_proc_ctx)},
      {"bc_stmt_begin", fn_addr(bc_stmt_begin)},
      {"bc_stmt_end", fn_addr(bc_stmt_end)},
      {"bc_var_set_i64", fn_addr(bc_var_set_i64)},
      {"bc_var_set_f64", fn_addr(bc_var_set_f64)},
      {"bc_var_set_bool", fn_addr(bc_var_set_bool)},
      {"bc_var_set_bytes", fn_addr(bc_var_set_bytes)},
      {"bc_var_set_null", fn_addr(bc_var_set_null)},
      {"bc_loop", fn_addr(bc_loop)},
      {"bc_rows_next", fn_addr(bc_rows_next)},
      {"bc_rows_done", fn_addr(bc_rows_done)},
      {"bc_range_open", fn_addr(bc_range_open)},
      {"bc_range_next", fn_addr(bc_range_next)},
      {"bc_param_i64", fn_addr(bc_param_i64)},
      {"bc_param_f64", fn_addr(bc_param_f64)},
      {"bc_param_bool", fn_addr(bc_param_bool)},
      {"bc_param_bytes", fn_addr(bc_param_bytes)},
      {"bc_param_jsonb", fn_addr(bc_param_jsonb)},
      {"bc_param_null", fn_addr(bc_param_null)},
      {"bc_emit_begin", fn_addr(bc_emit_begin)},
      {"bc_emit_i64", fn_addr(bc_emit_i64)},
      {"bc_emit_f64", fn_addr(bc_emit_f64)},
      {"bc_emit_bool", fn_addr(bc_emit_bool)},
      {"bc_emit_bytes", fn_addr(bc_emit_bytes)},
      {"bc_emit_num", fn_addr(bc_emit_num)},
      {"bc_emit_jsonb", fn_addr(bc_emit_jsonb)},
      {"bc_emit_null", fn_addr(bc_emit_null)},
      {"bc_emit_col", fn_addr(bc_emit_col)},
      {"bc_emit_key_part", fn_addr(bc_emit_key_part)},
      {"bc_emit_end", fn_addr(bc_emit_end)},
      {"bc_emit_count", fn_addr(bc_emit_count)},
      {"bc_add", fn_addr(bc_add)},
      {"bc_sub", fn_addr(bc_sub)},
      {"bc_add_bigint", fn_addr(bc_add_bigint)},
      {"bc_sub_bigint", fn_addr(bc_sub_bigint)},
      {"bc_mul", fn_addr(bc_mul)},
      {"bc_div", fn_addr(bc_div)},
      {"bc_mod", fn_addr(bc_mod)},
      {"bc_neg", fn_addr(bc_neg)},
      {"bc_f64_to_i64", fn_addr(bc_f64_to_i64)},
      {"bc_check_int", fn_addr(bc_check_int)},
      {"bc_i64_to_f64", fn_addr(bc_i64_to_f64)},
      {"bc_add_f64", fn_addr(bc_add_f64)},
      {"bc_min_f64", fn_addr(bc_min_f64)},
      {"bc_max_f64", fn_addr(bc_max_f64)},
      {"bc_cmp_f64", fn_addr(bc_cmp_f64)},
      {"bc_bytes_eq", fn_addr(bc_bytes_eq)},
      {"bc_bytes_cmp", fn_addr(bc_bytes_cmp)},
      {"bc_bytes_prefix", fn_addr(bc_bytes_prefix)},
      {"bc_concat", fn_addr(bc_concat)},
      {"bc_jsonb_in", fn_addr(bc_jsonb_in)},
      {"bc_jsonb_text", fn_addr(bc_jsonb_text)},
      {"bc_number_text", fn_addr(bc_number_text)},
      {"bc_num_quotient", fn_addr(bc_num_quotient)},
      {"bc_substr", fn_addr(bc_substr)},
      {"bc_text_case", fn_addr(bc_text_case)},
      {"bc_trim", fn_addr(bc_trim)},
      {"bc_char_length", fn_addr(bc_char_length)},
      {"bc_octet_length", fn_addr(bc_octet_length)},
      {"bc_position", fn_addr(bc_position)},
      {"bc_random", fn_addr(bc_random)},
      {"bc_extract", fn_addr(bc_extract)},
      {"bc_like", fn_addr(bc_like)},
      {"bc_ilike", fn_addr(bc_ilike)},
      {"bc_like_escape", fn_addr(bc_like_escape)},
      {"bc_sub_begin", fn_addr(bc_sub_begin)},
      {"bc_sub_end", fn_addr(bc_sub_end)},
      {"bc_sub_arg_null", fn_addr(bc_sub_arg_null)},
      {"bc_sub_arg_i64", fn_addr(bc_sub_arg_i64)},
      {"bc_sub_arg_f64", fn_addr(bc_sub_arg_f64)},
      {"bc_sub_arg_bool", fn_addr(bc_sub_arg_bool)},
      {"bc_sub_arg_bytes", fn_addr(bc_sub_arg_bytes)},
      {"bc_sub_has_i64", fn_addr(bc_sub_has_i64)},
      {"bc_sub_has_f64", fn_addr(bc_sub_has_f64)},
      {"bc_sub_has_bool", fn_addr(bc_sub_has_bool)},
      {"bc_sub_has_bytes", fn_addr(bc_sub_has_bytes)},
      {"bc_null_row", fn_addr(bc_null_row)},
      {"bc_key_begin", fn_addr(bc_key_begin)},
      {"bc_key_i64", fn_addr(bc_key_i64)},
      {"bc_key_f64", fn_addr(bc_key_f64)},
      {"bc_key_bool", fn_addr(bc_key_bool)},
      {"bc_key_bytes", fn_addr(bc_key_bytes)},
      {"bc_key_null", fn_addr(bc_key_null)},
      {"bc_key_param", fn_addr(bc_key_param)},
      {"bc_key_end", fn_addr(bc_key_end)},
      {"bc_key_fits", fn_addr(bc_key_fits)},
      {"bc_key_params", fn_addr(bc_key_params)},
      {"bc_key_successor", fn_addr(bc_key_successor)},
      {"bc_sort_key_begin", fn_addr(bc_sort_key_begin)},
      {"bc_sort_key_i64", fn_addr(bc_sort_key_i64)},
      {"bc_sort_key_f64", fn_addr(bc_sort_key_f64)},
      {"bc_sort_key_bool", fn_addr(bc_sort_key_bool)},
      {"bc_sort_key_bytes", fn_addr(bc_sort_key_bytes)},
      {"bc_sort_key_null", fn_addr(bc_sort_key_null)},
      {"bc_sort_key_col", fn_addr(bc_sort_key_col)},
      {"bc_sort_key_end", fn_addr(bc_sort_key_end)},
      {"bc_get", fn_addr(bc_get)},
      {"bc_exists", fn_addr(bc_exists)},
      {"bc_scan_open", fn_addr(bc_scan_open)},
      {"bc_scan_next", fn_addr(bc_scan_next)},
      {"bc_scan_key", fn_addr(bc_scan_key)},
      {"bc_scan_row", fn_addr(bc_scan_row)},
      {"bc_scan_close", fn_addr(bc_scan_close)},
      {"bc_empty", fn_addr(bc_empty)},
      {"bc_key_part_i64", fn_addr(bc_key_part_i64)},
      {"bc_key_part_f64", fn_addr(bc_key_part_f64)},
      {"bc_key_part_bool", fn_addr(bc_key_part_bool)},
      {"bc_key_part_bytes", fn_addr(bc_key_part_bytes)},
      {"bc_key_part_null", fn_addr(bc_key_part_null)},
      {"bc_sort_key_part_i64", fn_addr(bc_sort_key_part_i64)},
      {"bc_sort_key_part_f64", fn_addr(bc_sort_key_part_f64)},
      {"bc_sort_key_part_bool", fn_addr(bc_sort_key_part_bool)},
      {"bc_sort_key_part_bytes", fn_addr(bc_sort_key_part_bytes)},
      {"bc_sort_key_part_null", fn_addr(bc_sort_key_part_null)},
      {"bc_count_range", fn_addr(bc_count_range)},
      {"bc_col_null", fn_addr(bc_col_null)},
      {"bc_col_i64", fn_addr(bc_col_i64)},
      {"bc_col_f64", fn_addr(bc_col_f64)},
      {"bc_col_bool", fn_addr(bc_col_bool)},
      {"bc_col_bytes", fn_addr(bc_col_bytes)},
      {"bc_col_jsonb", fn_addr(bc_col_jsonb)},
      {"bc_col_key", fn_addr(bc_col_key)},
      {"bc_row_key", fn_addr(bc_row_key)},
      {"bc_row_begin", fn_addr(bc_row_begin)},
      {"bc_row_from", fn_addr(bc_row_from)},
      {"bc_row_set_i64", fn_addr(bc_row_set_i64)},
      {"bc_row_set_f64", fn_addr(bc_row_set_f64)},
      {"bc_row_set_bool", fn_addr(bc_row_set_bool)},
      {"bc_row_set_bytes", fn_addr(bc_row_set_bytes)},
      {"bc_row_set_jsonb", fn_addr(bc_row_set_jsonb)},
      {"bc_row_set_null", fn_addr(bc_row_set_null)},
      {"bc_row_set_param", fn_addr(bc_row_set_param)},
      {"bc_row_end", fn_addr(bc_row_end)},
      {"bc_pin", fn_addr(bc_pin)},
      {"bc_sorter_open", fn_addr(bc_sorter_open)},
      {"bc_sorter_add", fn_addr(bc_sorter_add)},
      {"bc_sorter_count", fn_addr(bc_sorter_count)},
      {"bc_sorter_nth", fn_addr(bc_sorter_nth)},
      {"bc_sorter_next", fn_addr(bc_sorter_next)},
      {"bc_hash_open", fn_addr(bc_hash_open)},
      {"bc_hash_put", fn_addr(bc_hash_put)},
      {"bc_hash_find", fn_addr(bc_hash_find)},
      {"bc_hash_next", fn_addr(bc_hash_next)},
      {"bc_agg_open", fn_addr(bc_agg_open)},
      {"bc_agg_group", fn_addr(bc_agg_group)},
      {"bc_agg_add_i64", fn_addr(bc_agg_add_i64)},
      {"bc_agg_add_f64", fn_addr(bc_agg_add_f64)},
      {"bc_agg_min_i64", fn_addr(bc_agg_min_i64)},
      {"bc_agg_max_i64", fn_addr(bc_agg_max_i64)},
      {"bc_agg_min_f64", fn_addr(bc_agg_min_f64)},
      {"bc_agg_max_f64", fn_addr(bc_agg_max_f64)},
      {"bc_agg_count", fn_addr(bc_agg_count)},
      {"bc_agg_next", fn_addr(bc_agg_next)},
      {"bc_agg_clear", fn_addr(bc_agg_clear)},
      {"bc_agg_i64", fn_addr(bc_agg_i64)},
      {"bc_agg_f64", fn_addr(bc_agg_f64)},
      {"bc_agg_null", fn_addr(bc_agg_null)},
      {"bc_insert", fn_addr(bc_insert)},
      {"bc_insert_on_conflict", fn_addr(bc_insert_on_conflict)},
      {"bc_insert_or_existing", fn_addr(bc_insert_or_existing)},
      {"bc_update", fn_addr(bc_update)},
      {"bc_delete", fn_addr(bc_delete)},
      {"bc_del_range", fn_addr(bc_del_range)},
      {"bc_guard_unchanged", fn_addr(bc_guard_unchanged)},
      {"bc_guard_row", fn_addr(bc_guard_row)},
      {"bc_guard_range", fn_addr(bc_guard_range)},
      {"bc_guard_present", fn_addr(bc_guard_present)},
      {"bc_guard_absent", fn_addr(bc_guard_absent)},
      {"bc_raise", fn_addr(bc_raise)},
      {"bc_raise_row", fn_addr(bc_raise_row)},
  };
  return kSymbols;
}
