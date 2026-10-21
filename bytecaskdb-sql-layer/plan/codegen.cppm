// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// codegen.cppm — the code generator: prints the statement IR the planner
// built (planner.cppm, ir.cppm) as a plan file in the subset of
// docs/plan_api_design.md, plus what the runtime needs to run it (parameter
// types, result layout, schema reference). Each IR shape has one pattern;
// the printer reads the IR alone, never the catalog.
//
// Writes are bc_insert, bc_update and bc_delete: the runtime maintains the
// indexes (docs/plan_api_design.md, *Decisions, not mechanics*).

module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

export module bytecask.sql.codegen;

export import bytecask.sql.binder;
import bytecask.sql.planner;

export namespace bytecask::sql {

// A column a plan reads or sets, as compiled: what bc_col_ref carries.
struct ColumnDep {
  std::uint32_t table, col;
  PhysType type;
};

struct GeneratedPlan {
  std::string source;  // the plan file: #include "bc_plan.h", constants, plan()
  std::vector<PhysType> param_types;
  bool returns_rows{false};
  std::vector<PhysType> result_types;  // the plan's result layout, for its bc_schema_ref
  // Each result column's name and SQL type, as PostgreSQL names and types
  // it: a column's own, or "count", a bigint.
  std::vector<std::string> result_names;
  std::vector<SqlType> result_sql;
  // What its bc_schema_ref lists (docs/catalog_design.md,
  // *Compatibility*): the objects and columns it depends on.
  std::uint64_t catalog_version{0};
  std::vector<std::uint32_t> objects_read, tables_written;
  std::vector<ColumnDep> columns;
};

// Plans a DML statement and prints the plan. Throws ast::SqlError (position
// -1) when no rule applies, and std::logic_error if the planner's IR fails
// ir::check, a compiler bug.
[[nodiscard]] auto generate_plan(const bound::Bound& b, const Catalog& cat, std::string_view sql) -> GeneratedPlan;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

auto suffix(PhysType t) -> std::string {
  switch (t) {
    case PhysType::I64: return "i64";
    case PhysType::F64: return "f64";
    case PhysType::Bool: return "bool";
    case PhysType::Bytes: return "bytes";
  }
  return "i64";
}

// One line of the plan's header, as a // comment. SQL and names are the
// user's, and none of their bytes may reach the C lexer as anything but
// comment text: a newline would end the comment, a backslash before one (or
// "??/", its trigraph) would splice the next line into it. So every byte
// outside letters, digits, space and printable punctuation other than "\"
// and "?" is written as \xNN; a line can then neither end early nor splice.
auto comment_line(std::string_view s) -> std::string {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string out = "// ";
  for (auto ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c >= 0x20 && c < 0x7F && c != '\\' && c != '?') {
      out.push_back(ch);
      continue;
    }
    out += "\\x";
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  return out + "\n";
}

// Writes the body line by line, and records which constants it used and
// which objects and columns it read or set.
class Writer {
public:
  explicit Writer(const ir::Plan& p) : p_{p} {}

  void line(const std::string& s) { body_ += std::string(2 * depth_, ' ') + s + "\n"; }
  void open(const std::string& s) {
    line(s.empty() ? "{" : s + " {");
    ++depth_;
  }
  void close() {
    --depth_;
    line("}");
  }
  void otherwise() {
    --depth_;
    line("} else {");
    ++depth_;
  }

  // Constant names (Catalog::constants: from ids, never from SQL names),
  // recorded as used; a column's constants record the column for the plan's
  // bc_schema_ref.
  auto tbl() -> std::string { return tbl(p_.table); }
  auto tbl(std::uint32_t id) -> std::string { return use("TBL_" + std::to_string(id)); }
  auto idx(std::uint32_t id) -> std::string { return use("IDX_" + std::to_string(id)); }
  // A scan's or a key's object: a table (its primary key) or an index.
  auto obj(std::uint32_t id) -> std::string {
    auto table = std::ranges::any_of(p_.constants, [&](const auto& c) { return c.name == "TBL_" + std::to_string(id); });
    return table ? tbl(id) : idx(id);
  }
  auto col(const ir::Column& c) -> std::string {
    cols_.emplace(c.table, c.id, c.type);
    return use("COL_" + std::to_string(c.table) + "_" + std::to_string(c.id));
  }
  // The position of a column in an index's key, for the key-part decoders.
  auto part(std::uint32_t index, const ir::Column& c) -> std::string {
    return use("PART_" + std::to_string(index) + "_" + std::to_string(c.id));
  }
  // A table or index the plan reads.
  void reads(std::uint32_t id) { reads_.insert(id); }
  [[nodiscard]] auto objects_read() const -> std::vector<std::uint32_t> { return {reads_.begin(), reads_.end()}; }
  // Every column the plan reads or sets, with its type.
  [[nodiscard]] auto columns_used() const -> std::vector<ColumnDep> {
    std::vector<ColumnDep> out;
    for (const auto& [table, id, type] : cols_) out.push_back({table, id, type});
    return out;
  }
  auto next_key() -> std::string { return "k" + std::to_string(keys_++); }

  // The constants block, in the catalog's order; none when the plan uses
  // none (C has no empty enum).
  auto constants() const -> std::string {
    if (used_.empty()) return "";
    std::string out = "enum { ";
    auto first = true;
    for (const auto& c : p_.constants) {
      if (!used_.contains(c.name)) continue;
      out += (first ? "" : ", ") + c.name + " = " + std::to_string(c.value);
      first = false;
    }
    return out + " };";
  }

  // What each constant the plan uses stands for, for the header comment:
  // "TBL_1 t, IDX_3 t_by_name, COL_1_1 t.name".
  auto legend() const -> std::string {
    std::string out;
    for (const auto& c : p_.constants) {
      if (used_.contains(c.name)) out += (out.empty() ? "" : ", ") + c.name + " " + c.object;
    }
    return out;
  }

  [[nodiscard]] auto body() const -> const std::string& { return body_; }
  [[nodiscard]] auto p() const -> const ir::Plan& { return p_; }

private:
  // A name the plan's constants lack would fail at TinyCC: a compiler bug,
  // refused here instead.
  auto use(std::string name) -> std::string {
    if (std::ranges::none_of(p_.constants, [&](const auto& c) { return c.name == name; })) {
      throw std::logic_error{"statement IR: a constant the table does not have: " + name};
    }
    used_.insert(name);
    return name;
  }

  const ir::Plan& p_;
  std::string body_;
  std::size_t depth_{1};
  int keys_{0};
  std::set<std::string> used_;
  std::set<std::tuple<std::uint32_t, std::uint32_t, PhysType>> cols_;
  std::set<std::uint32_t> reads_;
};

auto param(PhysType t, std::uint32_t n) -> std::string {
  return "bc_param_" + suffix(t) + "(cx, " + std::to_string(n) + ")";
}

// An int64_t constant as C text: INT64_MIN has no literal.
auto constant(std::int64_t v) -> std::string {
  if (v == std::numeric_limits<std::int64_t>::min()) return "INT64_MIN";
  return "INT64_C(" + std::to_string(v) + ")";
}

auto param_null(std::uint32_t n) -> std::string { return "bc_param_null(cx, " + std::to_string(n) + ")"; }

auto read_col(Writer& w, const ir::Column& c, const std::string& row) -> std::string {
  return "bc_col_" + suffix(c.type) + "(cx, " + row + ", " + w.col(c) + ")";
}

auto col_null(Writer& w, const ir::Column& c, const std::string& row) -> std::string {
  return "bc_col_null(cx, " + row + ", " + w.col(c) + ")";
}

// `if (any parameter is NULL) { <then> return; }`: a comparison with NULL is
// never true, so the statement matches no row; `then` reports that.
void return_if_null(Writer& w, const std::vector<std::uint32_t>& params, const std::vector<std::string>& then = {}) {
  std::string cond;
  for (auto p : params) cond += (cond.empty() ? "" : " || ") + param_null(p);
  w.open("if (" + cond + ")");
  for (const auto& line : then) w.line(line);
  w.line("return;");
  w.close();
}


// A key of `object` from its leading parts, as an expression: each a
// parameter or NULL (nullopt). One bc_key_params when they are consecutive
// parameters, a builder otherwise.
auto key(Writer& w, std::uint32_t object, const std::vector<std::optional<std::uint32_t>>& parts) -> std::string {
  auto consecutive = !parts.empty() && parts[0].has_value();
  for (std::size_t i = 1; consecutive && i < parts.size(); ++i) {
    consecutive = parts[i].has_value() && *parts[i] == *parts[0] + i;
  }
  if (consecutive) {
    return "bc_key_params(cx, " + w.obj(object) + ", " + std::to_string(*parts[0]) + ", " +
           std::to_string(parts.size()) + ")";
  }
  auto k = w.next_key();
  w.line("bc_key " + k + " = bc_key_begin(cx, " + w.obj(object) + ");");
  for (const auto& p : parts) {
    w.line(p ? "bc_key_param(cx, " + k + ", " + std::to_string(*p) + ");" : "bc_key_null(cx, " + k + ");");
  }
  return "bc_key_end(cx, " + k + ")";
}

auto key(Writer& w, std::uint32_t object, const std::vector<std::uint32_t>& params) -> std::string {
  return key(w, object, std::vector<std::optional<std::uint32_t>>(params.begin(), params.end()));
}

auto successor(const std::string& key) -> std::string { return "bc_key_successor(cx, " + key + ")"; }

// Reads the row a point lookup names into `name`, returning when a
// parameter is NULL or there is no such row.
void fetch_row(Writer& w, const ir::PointGet& g, const std::string& name, bool count) {
  return_if_null(w, g.key_params, count ? std::vector<std::string>{"bc_emit_count(cx, 0);"} : std::vector<std::string>{});
  w.line("bool found = false;");
  w.line("bc_view " + name + " = bc_get(cx, " + key(w, w.p().table, g.key_params) + ", &found);");
  w.open("if (!found)");
  if (count) w.line("bc_emit_count(cx, 0);");
  w.line("return;");
  w.close();
}

void emit_columns(Writer& w, const std::vector<ir::Column>& targets, const std::string& row) {
  w.line("bc_emit_begin(cx);");
  for (const auto& t : targets) w.line("bc_emit_col(cx, " + row + ", " + w.col(t) + ");");
  w.line("bc_emit_end(cx);");
}

auto count_row(const std::string& n) -> std::vector<std::string> {
  return {"bc_emit_begin(cx);", "bc_emit_i64(cx, " + n + ");", "bc_emit_end(cx);"};
}

void emit_count(Writer& w, const std::string& n) {
  for (const auto& line : count_row(n)) w.line(line);
}

// The aggregate's row over no rows: COUNT 0, SUM and MIN NULL.
auto empty_row(const ir::Aggregate& a) -> std::vector<std::string> {
  if (a.fn == ir::Aggregate::Fn::Count) return count_row("0");
  return {"bc_emit_begin(cx);", "bc_emit_null(cx);", "bc_emit_end(cx);"};
}

// The SUM or MIN row: the value, or NULL when no row gave one.
void emit_value_or_null(Writer& w, const std::string& value) {
  w.line("bc_emit_begin(cx);");
  w.open("if (seen)");
  w.line("bc_emit_i64(cx, " + value + ");");
  w.otherwise();
  w.line("bc_emit_null(cx);");
  w.close();
  w.line("bc_emit_end(cx);");
}

void print(Writer& w, const ir::Insert& s) {
  // bc_row_begin sets every column to its default; a column not given keeps
  // it. The runtime checks the keys and writes the index entries.
  w.line("bc_row r = bc_row_begin(cx, " + w.tbl() + ");");
  for (const auto& [col, p] : s.values) {
    w.line("bc_row_set_param(cx, r, " + w.col(col) + ", " + std::to_string(p) + ");");
  }
  w.line("bc_insert(cx, " + w.tbl() + ", bc_row_end(cx, r));");
  w.line("bc_emit_count(cx, 1);");
}

// FOR UPDATE: the row's key is guarded unchanged, so the commit conflicts
// if a concurrent commit changed the row (docs/transaction_design.md,
// *Isolation*).
void guard_row(Writer& w, const ir::Select& s, const std::string& row) {
  if (s.for_update) w.line("bc_guard_row(cx, " + w.tbl() + ", " + row + ");");
}

// Whether the statement emits its rows: LIMIT 0 emits none.
auto emits(const ir::Select& s) -> bool { return !s.limit || *s.limit > 0; }

// An aggregate over no rows, when a parameter the WHERE compares is NULL.
void return_empty_if_null(Writer& w, const ir::Select& s, const std::vector<std::uint32_t>& params) {
  return_if_null(w, params, emits(s) ? empty_row(*s.aggregate) : std::vector<std::string>{});
}

void print_point_select(Writer& w, const ir::Select& s, const ir::PointGet& g) {
  w.reads(w.p().table);
  if (s.aggregate) {
    const auto& a = *s.aggregate;
    return_empty_if_null(w, s, g.key_params);
    auto k = key(w, w.p().table, g.key_params);
    if (a.fn != ir::Aggregate::Fn::Count) {
      // SUM or MIN of one row: its value, NULL as NULL; no row is NULL.
      if (!emits(s)) return;
      w.line("bool found = false;");
      w.line("bc_view row = bc_get(cx, " + k + ", &found);");
      w.line("bc_emit_begin(cx);");
      w.open("if (found)");
      w.line("bc_emit_col(cx, row, " + w.col(*a.arg) + ");");
      w.otherwise();
      w.line("bc_emit_null(cx);");
      w.close();
      w.line("bc_emit_end(cx);");
      return;
    }
    w.line("int64_t n = 0;");
    if (!a.arg) {
      w.line("if (bc_exists(cx, " + k + ")) n = 1;");
    } else {
      w.line("bool found = false;");
      w.line("bc_view row = bc_get(cx, " + k + ", &found);");
      w.line("if (found && !" + col_null(w, *a.arg, "row") + ") n = 1;");
    }
    if (emits(s)) emit_count(w, "n");
    return;
  }
  fetch_row(w, g, "row", false);
  if (emits(s)) {
    guard_row(w, s, "row");
    emit_columns(w, s.targets, "row");
  }
}

// One end of a scan, in key order, as an expression.
auto bound(Writer& w, const ir::Scan& sc, const ir::KeyBound& b, bool is_lo) -> std::string {
  std::vector<std::optional<std::uint32_t>> parts(sc.prefix.begin(), sc.prefix.end());
  std::string k;
  switch (b.kind) {
    case ir::KeyBound::Kind::Open:
      // The prefix's first entry, or past its last; with no prefix, the
      // object's.
      if (parts.empty()) return "bc_empty(cx)";
      return is_lo ? key(w, sc.object, parts) : successor(key(w, sc.object, parts));
    case ir::KeyBound::Kind::Param: parts.emplace_back(b.param); break;
    case ir::KeyBound::Kind::NullPart: parts.emplace_back(std::nullopt); break;
  }
  k = key(w, sc.object, parts);
  return b.successor ? successor(k) : k;
}

// `bc_view lo` and `bc_view hi` of a scan's range.
void print_range(Writer& w, const ir::Scan& sc, const ir::Range& r) {
  w.line("bc_view lo = " + bound(w, sc, r.lo, true) + ";");
  w.line("bc_view hi = " + bound(w, sc, r.hi, false) + ";");
}

// `bc_cursor c` over every entry under the key `lo` holds.
void open_prefix(Writer& w, const ir::Scan& sc) {
  w.line("bc_cursor c = bc_scan_open(cx, " + w.obj(sc.object) + ", lo, lo, BC_SCAN_PREFIX" +
         (sc.reverse ? " | BC_SCAN_REVERSE" : "") + ");");
}

// `bc_view lo`, `bc_view hi` and `bc_cursor c` of the scan.
void open_scan(Writer& w, const ir::Scan& sc) {
  w.reads(sc.object);
  if (const auto* r = std::get_if<ir::Range>(&sc.rest)) {
    print_range(w, sc, *r);
    w.line("bc_cursor c = bc_scan_open(cx, " + w.obj(sc.object) + ", lo, hi, " +
           (sc.reverse ? "BC_SCAN_REVERSE" : "BC_SCAN_FORWARD") + ");");
    return;
  }
  w.line("bc_view lo = " + key(w, sc.object, sc.prefix) + ";");
  open_prefix(w, sc);
}

// The NULL part of a NullSplitScan: every entry under the prefix and NULL.
void open_null_part(Writer& w, const ir::Scan& sc) {
  std::vector<std::optional<std::uint32_t>> parts(sc.prefix.begin(), sc.prefix.end());
  parts.emplace_back(std::nullopt);
  w.line("bc_view lo = " + key(w, sc.object, parts) + ";");
  open_prefix(w, sc);
}

// The aggregate's argument in the entry the cursor `c` is on: its NULL test
// and its value, from the entry's key when the scan covers, else its row.
auto arg_of_entry(Writer& w, const ir::Scan& sc, const ir::Column& arg) -> std::pair<std::string, std::string> {
  if (sc.covering) {
    w.line("bc_view key = bc_scan_key(cx, c);");
    auto at = w.obj(sc.object) + ", key, " + w.part(sc.object, arg) + ")";
    return {"bc_key_part_null(cx, " + at, "bc_key_part_i64(cx, " + at};
  }
  w.reads(w.p().table);
  w.line("bc_view row = bc_scan_row(cx, c);");
  return {col_null(w, arg, "row"), read_col(w, arg, "row")};
}

// One entry folded into the aggregate the loop keeps: n for COUNT, sum or m
// and whether a value was seen for SUM and MIN. NULLs are skipped.
void fold_entry(Writer& w, const ir::Aggregate& a, const ir::Scan& sc) {
  const auto& arg = *a.arg;  // an aggregate the keys can't answer has an argument
  auto [is_null, value] = arg_of_entry(w, sc, arg);
  switch (a.fn) {
    case ir::Aggregate::Fn::Count: w.line("if (!" + is_null + ") ++n;"); return;
    case ir::Aggregate::Fn::Sum:
      if (arg.nullable) w.open("if (!" + is_null + ")");
      w.line("sum = bc_add(cx, sum, " + value + ");");
      w.line("seen = true;");
      if (arg.nullable) w.close();
      return;
    case ir::Aggregate::Fn::Min:
      if (arg.nullable) w.open("if (!" + is_null + ")");
      w.line("int64_t v = " + value + ";");
      w.line("if (!seen || v < m) m = v;");
      w.line("seen = true;");
      if (arg.nullable) w.close();
      return;
  }
}

// The loop over the open cursor `c`: each entry's row emitted, or folded
// into the aggregate.
void scan_loop(Writer& w, const ir::Select& s, const ir::Scan& sc) {
  auto limited = s.limit.has_value() && !s.aggregate;
  w.open(std::string{"while ("} + (limited ? "n < " + std::to_string(*s.limit) + " && " : "") + "bc_scan_next(cx, c))");
  if (s.aggregate) {
    fold_entry(w, *s.aggregate, sc);
  } else if (sc.covering) {
    // Every result column from the entry's key: no row read.
    if (!s.targets.empty()) w.line("bc_view key = bc_scan_key(cx, c);");
    w.line("bc_emit_begin(cx);");
    for (const auto& t : s.targets) {
      w.line("bc_emit_key_part(cx, " + w.obj(sc.object) + ", key, " + w.part(sc.object, t) + ");");
    }
    w.line("bc_emit_end(cx);");
  } else {
    w.reads(w.p().table);
    w.line("bc_view row = bc_scan_row(cx, c);");
    guard_row(w, s, "row");
    emit_columns(w, s.targets, "row");
  }
  if (limited) w.line("++n;");
  w.close();
}

// MIN from the scan's first entry, past the part's NULLs; none is NULL.
void print_min_first_entry(Writer& w, const ir::Select& s, const ir::Scan& sc) {
  const auto& arg = *s.aggregate->arg;
  open_scan(w, sc);
  w.open("if (bc_scan_next(cx, c))");
  w.line("bc_emit_begin(cx);");
  if (sc.covering) {
    w.line("bc_emit_key_part(cx, " + w.obj(sc.object) + ", bc_scan_key(cx, c), " + w.part(sc.object, arg) + ");");
  } else {
    w.reads(w.p().table);
    w.line("bc_emit_col(cx, bc_scan_row(cx, c), " + w.col(arg) + ");");
  }
  w.line("bc_emit_end(cx);");
  w.otherwise();
  for (const auto& line : empty_row(*s.aggregate)) w.line(line);
  w.close();
}

void print_scan_select(Writer& w, const ir::Select& s, const ir::Scan& sc) {
  if (!s.aggregate) {
    return_if_null(w, sc.null_checks);
    if (s.limit) w.line("int64_t n = 0;");
    open_scan(w, sc);
    scan_loop(w, s, sc);
    return;
  }
  const auto& a = *s.aggregate;
  return_empty_if_null(w, s, sc.null_checks);
  if (a.fn == ir::Aggregate::Fn::Count && a.from_keys) {
    // Every entry of the range is one row to count.
    w.reads(sc.object);
    if (const auto* r = std::get_if<ir::Range>(&sc.rest)) {
      print_range(w, sc, *r);
    } else {
      w.line("bc_view lo = " + key(w, sc.object, sc.prefix) + ";");
      w.line("bc_view hi = bc_key_successor(cx, lo);");
    }
    w.line("int64_t n = bc_count_range(cx, " + w.obj(sc.object) + ", lo, hi, INT64_MAX);");
    if (emits(s)) emit_count(w, "n");
    return;
  }
  if (!emits(s)) return;
  if (a.from_keys) {
    print_min_first_entry(w, s, sc);
    return;
  }
  switch (a.fn) {
    case ir::Aggregate::Fn::Count: w.line("int64_t n = 0;"); break;
    case ir::Aggregate::Fn::Sum: w.line("int64_t sum = 0;"); break;
    case ir::Aggregate::Fn::Min: w.line("int64_t m = 0;"); break;
  }
  if (a.fn != ir::Aggregate::Fn::Count) w.line("bool seen = false;");
  open_scan(w, sc);
  scan_loop(w, s, sc);
  switch (a.fn) {
    case ir::Aggregate::Fn::Count: emit_count(w, "n"); break;
    case ir::Aggregate::Fn::Sum: emit_value_or_null(w, "sum"); break;
    case ir::Aggregate::Fn::Min: emit_value_or_null(w, "m"); break;
  }
}

// The present values and the NULL part, each in its own block, in
// PostgreSQL's order; one LIMIT counter across both.
void print_split_select(Writer& w, const ir::Select& s, const ir::NullSplitScan& split) {
  const auto& sc = split.present;
  return_if_null(w, sc.null_checks);
  if (s.limit) w.line("int64_t n = 0;");
  for (auto nulls : sc.reverse ? std::vector<bool>{true, false} : std::vector<bool>{false, true}) {
    w.open("");
    if (nulls) {
      w.reads(sc.object);
      open_null_part(w, sc);
    } else {
      open_scan(w, sc);
    }
    scan_loop(w, s, sc);
    w.close();
  }
}

// The two rows, then each target from its own.
void print_join_select(Writer& w, const ir::Select& s, const ir::PointJoin& j) {
  const std::string rows[2]{"row", "row1"};
  auto params = j.rows[0].key_params;
  for (auto p : j.rows[1].key_params) {
    if (std::ranges::find(params, p) == params.end()) params.push_back(p);
  }
  return_if_null(w, params);
  for (std::size_t i = 0; i < 2; ++i) {
    w.reads(j.tables[i]);
    auto found = std::string{i == 0 ? "found" : "found1"};
    w.line("bool " + found + " = false;");
    w.line("bc_view " + rows[i] + " = bc_get(cx, " + key(w, j.tables[i], j.rows[i].key_params) + ", &" + found + ");");
    w.open("if (!" + found + ")");
    w.line("return;");
    w.close();
  }
  if (!emits(s)) return;
  w.line("bc_emit_begin(cx);");
  for (const auto& t : s.targets) {
    w.line("bc_emit_col(cx, " + rows[t.table == j.tables[0] ? 0 : 1] + ", " + w.col(t) + ");");
  }
  w.line("bc_emit_end(cx);");
}

void print(Writer& w, const ir::Select& s) {
  std::visit(
      [&](const auto& from) {
        using T = std::decay_t<decltype(from)>;
        if constexpr (std::is_same_v<T, ir::PointGet>) {
          print_point_select(w, s, from);
        } else if constexpr (std::is_same_v<T, ir::Scan>) {
          print_scan_select(w, s, from);
        } else if constexpr (std::is_same_v<T, ir::NullSplitScan>) {
          print_split_select(w, s, from);
        } else {
          print_join_select(w, s, from);
        }
      },
      s.from);
}

// Each row of an UPDATE or DELETE as `old`, `write` printing what is done
// with it; then the count of rows written.
void for_each_row(Writer& w, const std::variant<ir::PointGet, ir::Scan>& rows, const auto& write) {
  w.reads(w.p().table);
  if (const auto* g = std::get_if<ir::PointGet>(&rows)) {
    fetch_row(w, *g, "old", true);
    write();
    w.line("bc_emit_count(cx, 1);");
    return;
  }
  // Each row the scan reaches, of the snapshot the statement began on: its
  // own writes never feed back into it.
  const auto& sc = std::get<ir::Scan>(rows);
  return_if_null(w, sc.null_checks, {"bc_emit_count(cx, 0);"});
  w.line("int64_t n = 0;");
  open_scan(w, sc);
  w.open("while (bc_scan_next(cx, c))");
  w.line("bc_view old = bc_scan_row(cx, c);");
  write();
  w.line("++n;");
  w.close();
  w.line("bc_emit_count(cx, n);");
}

void print_set(Writer& w, const ir::Update& s) {
  w.line("bc_row r = bc_row_from(cx, " + w.tbl() + ", old);");
  for (const auto& [col, e] : s.set) {
    if (e.kind == ir::Expr::Kind::Param) {
      w.line("bc_row_set_param(cx, r, " + w.col(col) + ", " + std::to_string(e.param) + ");");
      continue;
    }
    auto set = "bc_row_set_" + suffix(col.type) + "(cx, r, " + w.col(col) + ", ";
    std::string value, null_if;
    auto null_src = e.col.nullable ? col_null(w, e.col, "old") : "";
    if (e.kind == ir::Expr::Kind::Column) {
      value = read_col(w, e.col, "old");
      null_if = null_src;
    } else {
      auto fn = e.kind == ir::Expr::Kind::Add ? "bc_add" : "bc_sub";
      value = std::string{fn} + "(cx, " + read_col(w, e.col, "old") + ", " +
              (e.constant ? constant(*e.constant) : param(PhysType::I64, e.param)) + ")";
      if (!e.constant) null_if = (null_src.empty() ? "" : null_src + " || ") + param_null(e.param);
      else null_if = null_src;
    }
    if (null_if.empty()) {
      w.line(set + value + ");");
      continue;
    }
    w.open("if (" + null_if + ")");
    w.line("bc_row_set_null(cx, r, " + w.col(col) + ");");
    w.otherwise();
    w.line(set + value + ");");
    w.close();
  }
  // The runtime moves the index entries whose keys changed.
  w.line("bc_update(cx, " + w.tbl() + ", old, bc_row_end(cx, r));");
}

void print(Writer& w, const ir::Update& s) {
  for_each_row(w, s.rows, [&] { print_set(w, s); });
}

void print(Writer& w, const ir::Delete& s) {
  for_each_row(w, s.rows, [&] { w.line("bc_delete(cx, " + w.tbl() + ", old);"); });
}

// An aggregate's result column as PostgreSQL names and types it: COUNT a
// bigint; SUM of a SMALLINT or INTEGER a bigint, of a BIGINT or DECIMAL a
// numeric at its scale; MIN of the column's type.
auto aggregate_result(const bound::Aggregate& a, const Table& t) -> std::pair<std::string, SqlType> {
  using Fn = ast::Expr::Fn;
  switch (a.fn) {
    case Fn::Count: return {"count", SqlType{SqlKind::BigInt}};
    case Fn::Sum: {
      const auto& sql = t.column(*a.col_id)->sql;
      if (sql.kind == SqlKind::SmallInt || sql.kind == SqlKind::Integer) return {"sum", SqlType{SqlKind::BigInt}};
      return {"sum", SqlType{SqlKind::Decimal, 0, static_cast<std::uint8_t>(kMaxDecimalPrecision), sql.scale}};
    }
    case Fn::Min: return {"min", t.column(*a.col_id)->sql};
  }
  return {"", {}};
}

}  // namespace

auto generate_plan(const bound::Bound& b, const Catalog& cat, std::string_view sql) -> GeneratedPlan {
  auto p = plan_statement(b, cat);
  ir::check(p);
  GeneratedPlan out;
  out.param_types = p.param_types;
  out.catalog_version = p.catalog_version;
  Writer w{p};
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Select>) {
          out.returns_rows = true;
          for (const auto& t : s.targets) out.result_types.push_back(t.type);
          if (s.aggregate) out.result_types.push_back(PhysType::I64);
        } else {
          // The runtime maintains the table's indexes: a write plan names none.
          out.tables_written = {p.table};
        }
        print(w, s);
      },
      p.stmt);
  out.source = comment_line(sql) +
               comment_line("isolation: snapshot   schema: catalog v" + std::to_string(p.catalog_version)) +
               comment_line("objects: " + w.legend()) + "#include \"bc_plan.h\"\n" + w.constants() +
               "\n\nvoid plan(bc_ctx *cx) {\n" + w.body() + "}\n";
  if (const auto* sel = std::get_if<bound::Select>(&b.stmt)) {
    for (auto id : sel->targets) {
      out.result_names.push_back(sel->table->column(id)->name);
      out.result_sql.push_back(sel->table->column(id)->sql);
    }
    if (const auto& a = sel->aggregate) {
      auto [name, type] = aggregate_result(*a, *sel->table);
      out.result_names.push_back(name);
      out.result_sql.push_back(type);
    }
  }
  if (const auto* j = std::get_if<bound::Join>(&b.stmt)) {
    for (const auto& t : j->targets) {
      const auto* c = (t.side == 0 ? j->table : j->second)->column(t.col_id);
      out.result_names.push_back(c->name);
      out.result_sql.push_back(c->sql);
    }
  }
  out.objects_read = w.objects_read();
  out.columns = w.columns_used();
  return out;
}

}  // namespace bytecask::sql
