// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ir.cppm — the statement IR (docs/frontend_design.md, *The statement IR*):
// what the planner decided, in the form the code generator prints. Every
// id, type and constant name the printer needs is here, so it never reads
// the catalog. This is slice 0: the forms the compiler handles today. Later
// slices add the design's other shapes; a type admits only what its printer
// prints, so an UPDATE names its row by a point lookup until one may scan.

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.ir;

export import bytecask.sql.catalog;

export namespace bytecask::sql::ir {

// A column of the plan's table, as the printer names and decodes it.
struct Column {
  std::uint32_t id{0};
  PhysType type{PhysType::I64};
  bool nullable{false};
};

// A value set into a row: a parameter, a column of the row in hand, or a
// column minus a parameter.
struct Expr {
  enum class Kind { Param, Column, Sub };
  Kind kind{Kind::Param};
  std::uint32_t param{0};  // Param, and Sub's right operand
  Column col{};            // Column, and Sub's left operand
};

// The whole primary key by equality: the parameter of each part, in key
// order. A NULL one matches nothing.
struct PointGet {
  std::vector<std::uint32_t> key_params;
};

// One end of a scan, in key order: open, a parameter's key part, or the
// key part that holds NULL; as is, or its successor.
struct KeyBound {
  enum class Kind { Open, Param, NullPart };
  Kind kind{Kind::Open};
  std::uint32_t param{0};
  bool successor{false};
};

struct Range {
  KeyBound lo, hi;
};

// A scan of one index: every entry under an equality prefix, or a range of
// entries.
struct Scan {
  std::uint32_t index{0};
  std::vector<std::uint32_t> eq_params;      // the equality prefix
  std::variant<std::monostate, Range> rest;  // nothing more, or a range
  std::vector<std::uint32_t> null_checks;    // the parameters it compares, in WHERE order
  bool covering{false};                      // every target comes from the entry's key
};

struct Select {
  std::variant<PointGet, Scan> from;
  std::vector<Column> targets;  // from the row, or from the entry's key when the scan covers
};

// The columns given, in table order, each from a parameter; the rest keep
// the default bc_row_begin writes.
struct Insert {
  std::vector<std::pair<Column, std::uint32_t>> values;
};

struct Update {
  PointGet row;
  std::vector<std::pair<Column, Expr>> set;
};

struct Delete {
  PointGet row;
};

using Statement = std::variant<Select, Insert, Update, Delete>;

struct Plan {
  std::uint32_t table{0};                         // the one table the plan reads or writes
  std::vector<Catalog::PlanConstant> constants;   // the table's, in the catalog's order
  std::uint64_t catalog_version{0};
  std::vector<PhysType> param_types;
  Statement stmt;
};

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
  if (s.eq_params.empty() == std::holds_alternative<std::monostate>(s.rest)) {
    broken("a scan needs an equality prefix or a range, and has one of them today");
  }
  // The printer encodes the prefix as one part; slice 1 takes longer ones,
  // with the index's part count to bound them.
  if (s.eq_params.size() > 1) broken("an equality prefix of more than one part");
  check_params(p, s.eq_params);
  check_params(p, s.null_checks);
  for (auto n : s.eq_params) check_null_checked(s, n);
  if (const auto* r = std::get_if<Range>(&s.rest)) {
    for (const auto& b : {r->lo, r->hi}) {
      if (b.kind != KeyBound::Kind::Param) continue;
      check_params(p, {b.param});
      check_null_checked(s, b.param);
    }
  }
}

void check(const Plan& p, const Select& s) {
  if (s.targets.empty()) broken("a select with no target");
  const auto* scan = std::get_if<Scan>(&s.from);
  std::visit([&](const auto& path) { check(p, path); }, s.from);
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
  check(p, s.row);
  if (s.set.empty()) broken("an update that sets no column");
  for (const auto& [col, e] : s.set) {
    if (e.kind != Expr::Kind::Column) check_params(p, {e.param});
    if (e.kind == Expr::Kind::Column && e.col.type != col.type) broken("a column set from a column of another type");
    if (e.kind == Expr::Kind::Sub && (e.col.type != PhysType::I64 || col.type != PhysType::I64)) {
      broken("column - parameter on a type other than an integer");
    }
  }
}

void check(const Plan& p, const Delete& s) { check(p, s.row); }

}  // namespace

void check(const Plan& p) {
  if (p.constants.empty()) broken("a plan with no constants");
  std::visit([&](const auto& s) { check(p, s); }, p.stmt);
}

}  // namespace bytecask::sql::ir
