// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ir.cppm — the statement IR (docs/frontend_design.md, *The statement IR*):
// what the planner decided, in the form the code generator prints. Every
// id, type and constant name the printer needs is here, so it never reads
// the catalog. Slices 0 to 5: point lookups, scans of a key prefix or a
// range in either direction, the two-range scan of a nullable ordered part,
// filters on what the key does not express, LIMIT, COUNT, COUNT(DISTINCT),
// SUM and MIN, the join of two point lookups and the join of a scan to a
// point lookup by its row's columns, and UPDATE and DELETE of the rows a
// point lookup or a scan reaches. A type admits only what its printer
// prints.

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.ir;

export import bytecask.sql.ast;
export import bytecask.sql.catalog;

export namespace bytecask::sql::ir {

using Op = ast::Compare::Op;

// A column of the plan's table, as the printer names and decodes it.
struct Column {
  std::uint32_t id{0};
  PhysType type{PhysType::I64};
  bool nullable{false};
  std::uint32_t table{0};  // the plan's table, or a join's other one
};

// A value set into a row: a parameter, a column of the row in hand, or a
// column plus or minus a parameter or a constant.
struct Expr {
  enum class Kind { Param, Column, Add, Sub };
  Kind kind{Kind::Param};
  std::uint32_t param{0};  // Param, and Add's and Sub's right operand
  Column col{};            // Column, and Add's and Sub's left operand
  std::optional<std::int64_t> constant;  // Add's and Sub's right operand instead of the parameter
  // Add and Sub: the column read is a SQL BIGINT, the one integer type whose
  // sum can leave int64. PostgreSQL types the sum by it, not by the target.
  bool bigint{false};
};

// The whole primary key by equality: the parameter of each part, in key
// order. A NULL one matches nothing.
struct PointGet {
  std::vector<std::uint32_t> key_params;
};

// $param + offset as an INTEGER, computed once before anything is read:
// BC_C_RANGE on `table` when it leaves INTEGER, as PostgreSQL's operator
// raises 22003. Named by its index in Plan::computed.
struct Computed {
  std::uint32_t param{0};
  std::int64_t offset{0};
  std::uint32_t table{0};
};

// One end of a scan, in key order, on the part after the prefix: open (the
// prefix's first or last entry), a parameter's key part, a computed value's,
// or the key part that holds NULL; as is, or its successor.
struct KeyBound {
  enum class Kind { Open, Param, NullPart, Computed };
  Kind kind{Kind::Open};
  std::uint32_t param{0};  // Param: the parameter; Computed: its index
  bool successor{false};
};

// What a filter compares its column with: a parameter, a computed value, or
// a column of the join's other row.
struct Operand {
  std::uint32_t param{0};
  std::optional<std::uint32_t> computed;
  std::optional<Column> column;
};

// A conjunct the key does not express, run on each row as soon as the row is
// read, before it is counted, emitted or written. NULL on either side
// rejects the row. Never on a DOUBLE or REAL column.
struct Filter {
  Column col;
  Op op{Op::Eq};
  Operand rhs;
};

struct Range {
  KeyBound lo, hi;
};

// A scan of the table's primary key or of one of its indexes: every entry
// under an equality prefix, or a range of entries after it; forward, or in
// reverse.
struct Scan {
  std::uint32_t object{0};                   // the table, or one of its indexes
  std::vector<std::uint32_t> prefix;         // the equality prefix's parameters, in key order
  std::variant<std::monostate, Range> rest;  // nothing more, or a range
  bool reverse{false};
  std::vector<std::uint32_t> null_checks;    // the parameters it compares, in WHERE order
  bool covering{false};                      // every column read comes from the entry's key
};

// An ordered scan whose first ordered part is a nullable ASC part: the key
// order puts its NULLs first, PostgreSQL last for ASC and first for DESC.
// So two ranges: `present`, the part's values (a Range from past the NULL
// part), and the NULL part. Forward: the values, then the NULLs; with
// present.reverse (DESC), the NULLs reversed, then the values reversed.
struct NullSplitScan {
  Scan present;
};

// Two tables, each named by its whole primary key: the row pair, if both
// rows exist. `tables[0]` is the first table in FROM, the plan's table.
struct PointJoin {
  std::uint32_t tables[2]{};
  PointGet rows[2];
};

// One part of the inner key of a LoopJoin: a parameter, or a column of the
// outer row.
struct KeyPart {
  std::uint32_t param{0};
  std::optional<Column> column;
};

// The outer table scanned, the inner one read by its whole primary key, its
// parts from parameters and the outer row's columns: an outer row whose
// column is NULL, or with no inner row, joins nothing. `tables` are the outer
// table and the inner one, whichever FROM names first.
struct LoopJoin {
  std::uint32_t tables[2]{};
  Scan outer;
  std::vector<KeyPart> inner_key;  // in the inner primary key's order
};

// COUNT(*), or COUNT, COUNT(DISTINCT), SUM or MIN of arg over its non-NULL
// values, each held as an int64_t. Otherwise a loop over the rows; from_keys
// on a Scan:
// - COUNT: bc_count_range over its range, which counts its entries
//   (COUNT(*), or an arg no matching row holds NULL in);
// - MIN: its first entry, arg being the ASC part after its prefix, and its
//   range starting past that part's NULLs.
struct Aggregate {
  enum class Fn { Count, Sum, Min };
  Fn fn{Fn::Count};
  std::optional<Column> arg;
  bool from_keys{false};
  bool distinct{false};  // COUNT(DISTINCT arg): its keys grouped, then counted
};

// The result: the targets of each row, or one aggregate row. No target is a
// row of no columns, as PostgreSQL's `SELECT FROM`.
struct Select {
  std::variant<PointGet, Scan, NullSplitScan, PointJoin, LoopJoin> from;
  std::vector<Column> targets;         // from the row, or from the entry's key when the scan covers
  std::optional<Aggregate> aggregate;  // instead of targets
  std::optional<std::int64_t> limit;
  bool for_update{false};       // each row returned is guarded unchanged: never a covering scan
  std::vector<Filter> filters;  // in WHERE order; a scan with one never covers
};

// The columns given, in table order, each from a parameter; the rest keep
// the default bc_row_begin writes.
struct Insert {
  std::vector<std::pair<Column, std::uint32_t>> values;
};

// UPDATE and DELETE write each row the point lookup or the scan reaches,
// read from the snapshot the statement began on.
struct Update {
  std::variant<PointGet, Scan> rows;
  std::vector<std::pair<Column, Expr>> set;
  std::vector<Filter> filters;
};

struct Delete {
  std::variant<PointGet, Scan> rows;
  std::vector<Filter> filters;
};

using Statement = std::variant<Select, Insert, Update, Delete>;

struct Plan {
  std::uint32_t table{0};                         // the table the plan reads or writes; a join's first
  std::vector<Catalog::PlanConstant> constants;   // its tables', in the catalog's order
  std::uint64_t catalog_version{0};
  std::vector<PhysType> param_types;
  Statement stmt;
  std::vector<Computed> computed;  // what the statement's $n ± k evaluate to
};

// The scan a SELECT reads by, a NullSplitScan's present values for it; null
// for a point lookup.
auto scan_of(const Select& s) -> const Scan* {
  if (const auto* split = std::get_if<NullSplitScan>(&s.from)) return &split->present;
  return std::get_if<Scan>(&s.from);
}

// Checks what the types can't express. Throws std::logic_error naming the
// broken rule: a compiler bug, never a refusal of the statement.
void check(const Plan& p);

}  // namespace bytecask::sql::ir

namespace bytecask::sql::ir {

namespace {

[[noreturn]] void broken(const std::string& rule) { throw std::logic_error{"statement IR: " + rule}; }

void check_params(const Plan& p, const std::vector<std::uint32_t>& params) {
  for (auto n : params) {
    if (n >= p.param_types.size()) broken("a parameter past the statement's");
  }
}

// A NULL parameter matches nothing: the scan must return before it encodes
// one into a key.
void check_null_checked(const Scan& s, std::uint32_t param) {
  if (std::ranges::find(s.null_checks, param) == s.null_checks.end()) {
    broken("a compared parameter the scan does not check for NULL");
  }
}

void check(const Plan& p, const PointGet& g) {
  if (g.key_params.empty()) broken("a point lookup with no key part");
  check_params(p, g.key_params);
}

void check(const Plan& p, const Scan& s) {
  if (s.prefix.empty() && std::holds_alternative<std::monostate>(s.rest)) {
    broken("a scan needs an equality prefix or a range");
  }
  check_params(p, s.prefix);
  check_params(p, s.null_checks);
  for (auto n : s.prefix) check_null_checked(s, n);
  if (const auto* r = std::get_if<Range>(&s.rest)) {
    for (const auto& b : {r->lo, r->hi}) {
      if (b.kind == KeyBound::Kind::Computed && b.param >= p.computed.size()) broken("a computed bound past the plan's");
      if (b.kind != KeyBound::Kind::Param) continue;
      check_params(p, {b.param});
      check_null_checked(s, b.param);
    }
  }
}

void check(const Plan& p, const NullSplitScan& s) { check(p, s.present); }

void check(const Plan& p, const PointJoin& j) {
  for (const auto& g : j.rows) check(p, g);
}

void check(const Plan& p, const LoopJoin& j) { check(p, j.outer); }

void check(const Plan& p, const Select& s) {
  const auto* scan = scan_of(s);
  std::visit([&](const auto& path) { check(p, path); }, s.from);
  // FOR UPDATE guards each row by its key, read from the row.
  if (scan != nullptr && scan->covering && s.for_update) broken("a FOR UPDATE scan that covers");
  for (const auto& t : s.targets) {
    // The key encoding writes -0.0 as 0.0 and every NaN as one NaN.
    if (scan != nullptr && scan->covering && t.type == PhysType::F64) broken("a DOUBLE target read from a key");
  }
}

void check(const Plan& p, const Insert& s) {
  if (s.values.empty()) broken("an insert that sets no column");
  for (const auto& [col, param] : s.values) check_params(p, {param});
}

void check(const Plan& p, const Update& s) {
  std::visit([&](const auto& path) { check(p, path); }, s.rows);
  if (s.set.empty()) broken("an update that sets no column");
  for (const auto& [col, e] : s.set) {
    if (e.kind != Expr::Kind::Column && !e.constant) check_params(p, {e.param});
    if (e.kind == Expr::Kind::Column && e.col.type != col.type) broken("a column set from a column of another type");
    if ((e.kind == Expr::Kind::Add || e.kind == Expr::Kind::Sub) &&
        (e.col.type != PhysType::I64 || col.type != PhysType::I64)) {
      broken("column + or - parameter on a type other than an integer");
    }
  }
}

void check(const Plan& p, const Delete& s) {
  std::visit([&](const auto& path) { check(p, path); }, s.rows);
}

}  // namespace

void check(const Plan& p) {
  if (p.constants.empty()) broken("a plan with no constants");
  std::visit([&](const auto& s) { check(p, s); }, p.stmt);
}

}  // namespace bytecask::sql::ir
