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
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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
    line(s + " {");
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
  auto tbl() -> std::string { return use("TBL_" + std::to_string(p_.table)); }
  auto idx(std::uint32_t id) -> std::string { return use("IDX_" + std::to_string(id)); }
  auto col(const ir::Column& c) -> std::string {
    cols_.emplace(c.id, c.type);
    return use("COL_" + std::to_string(p_.table) + "_" + std::to_string(c.id));
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
    for (const auto& [id, type] : cols_) out.push_back({p_.table, id, type});
    return out;
  }
  auto next_key() -> std::string { return "k" + std::to_string(keys_++); }

  // The constants block, in the catalog's order.
  auto constants() const -> std::string {
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
  std::set<std::pair<std::uint32_t, PhysType>> cols_;
  std::set<std::uint32_t> reads_;
};

auto param(PhysType t, std::uint32_t n) -> std::string {
  return "bc_param_" + suffix(t) + "(cx, " + std::to_string(n) + ")";
}

auto param_null(std::uint32_t n) -> std::string { return "bc_param_null(cx, " + std::to_string(n) + ")"; }

auto read_col(Writer& w, const ir::Column& c, const std::string& row) -> std::string {
  return "bc_col_" + suffix(c.type) + "(cx, " + row + ", " + w.col(c) + ")";
}

auto col_null(Writer& w, const ir::Column& c, const std::string& row) -> std::string {
  return "bc_col_null(cx, " + row + ", " + w.col(c) + ")";
}

// `if (any parameter is NULL) { <then> return; }`: a comparison with NULL is
// never true.
void return_if_null(Writer& w, const std::vector<std::uint32_t>& params, bool count) {
  std::string cond;
  for (auto p : params) cond += (cond.empty() ? "" : " || ") + param_null(p);
  w.open("if (" + cond + ")");
  if (count) w.line("bc_emit_count(cx, 0);");
  w.line("return;");
  w.close();
}

// The primary key from parameters, as an expression: one bc_key_params when
// they are consecutive, a builder otherwise.
auto pk_key(Writer& w, const std::vector<std::uint32_t>& params) -> std::string {
  auto consecutive = true;
  for (std::size_t i = 1; i < params.size(); ++i) consecutive = consecutive && params[i] == params[0] + i;
  if (consecutive) {
    return "bc_key_params(cx, " + w.tbl() + ", " + std::to_string(params[0]) + ", " + std::to_string(params.size()) + ")";
  }
  auto k = w.next_key();
  w.line("bc_key " + k + " = bc_key_begin(cx, " + w.tbl() + ");");
  for (auto p : params) w.line("bc_key_param(cx, " + k + ", " + std::to_string(p) + ");");
  return "bc_key_end(cx, " + k + ")";
}

// Reads the row a point lookup names into `name`, returning when a
// parameter is NULL or there is no such row.
void fetch_row(Writer& w, const ir::PointGet& g, const std::string& name, bool count) {
  return_if_null(w, g.key_params, count);
  w.line("bool found = false;");
  w.line("bc_view " + name + " = bc_get(cx, " + pk_key(w, g.key_params) + ", &found);");
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

void print_point_select(Writer& w, const ir::Select& s, const ir::PointGet& g) {
  w.reads(w.p().table);
  fetch_row(w, g, "row", false);
  emit_columns(w, s.targets, "row");
}

// One end of a scan as `bc_view <name> = ...;`.
void key_bound(Writer& w, std::uint32_t index, const ir::KeyBound& b, const std::string& name) {
  std::string key;
  switch (b.kind) {
    case ir::KeyBound::Kind::Open:
      w.line("bc_view " + name + " = bc_empty(cx);");
      return;
    case ir::KeyBound::Kind::Param:
      key = "bc_key_params(cx, " + w.idx(index) + ", " + std::to_string(b.param) + ", 1)";
      break;
    case ir::KeyBound::Kind::NullPart: {
      auto k = w.next_key();
      w.line("bc_key " + k + " = bc_key_begin(cx, " + w.idx(index) + ");");
      w.line("bc_key_null(cx, " + k + ");");
      key = "bc_key_end(cx, " + k + ")";
      break;
    }
  }
  w.line("bc_view " + name + " = " + (b.successor ? "bc_key_successor(cx, " + key + ")" : key) + ";");
}

void print_scan_select(Writer& w, const ir::Select& s, const ir::Scan& sc) {
  w.reads(sc.index);
  if (!sc.covering) w.reads(w.p().table);
  return_if_null(w, sc.null_checks, false);
  if (const auto* r = std::get_if<ir::Range>(&sc.rest)) {
    key_bound(w, sc.index, r->lo, "lo");
    key_bound(w, sc.index, r->hi, "hi");
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(sc.index) + ", lo, hi, BC_SCAN_FORWARD);");
  } else {
    w.line("bc_view lo = bc_key_params(cx, " + w.idx(sc.index) + ", " + std::to_string(sc.eq_params[0]) + ", " +
           std::to_string(sc.eq_params.size()) + ");");
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(sc.index) + ", lo, lo, BC_SCAN_PREFIX);");
  }
  w.open("while (bc_scan_next(cx, c))");
  if (sc.covering) {
    // Every result column from the entry's key: no row read.
    if (!s.targets.empty()) w.line("bc_view key = bc_scan_key(cx, c);");
    w.line("bc_emit_begin(cx);");
    for (const auto& t : s.targets) {
      w.line("bc_emit_key_part(cx, " + w.idx(sc.index) + ", key, " + w.part(sc.index, t) + ");");
    }
    w.line("bc_emit_end(cx);");
  } else {
    w.line("bc_view row = bc_scan_row(cx, c);");
    emit_columns(w, s.targets, "row");
  }
  w.close();
}

void print(Writer& w, const ir::Select& s) {
  std::visit(
      [&](const auto& from) {
        if constexpr (std::is_same_v<std::decay_t<decltype(from)>, ir::PointGet>) {
          print_point_select(w, s, from);
        } else {
          print_scan_select(w, s, from);
        }
      },
      s.from);
}

void print(Writer& w, const ir::Update& s) {
  w.reads(w.p().table);
  fetch_row(w, s.row, "old", true);
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
      value = "bc_sub(cx, " + read_col(w, e.col, "old") + ", " + param(PhysType::I64, e.param) + ")";
      null_if = (null_src.empty() ? "" : null_src + " || ") + param_null(e.param);
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
  w.line("bc_emit_count(cx, 1);");
}

void print(Writer& w, const ir::Delete& s) {
  w.reads(w.p().table);
  fetch_row(w, s.row, "old", true);
  w.line("bc_delete(cx, " + w.tbl() + ", old);");
  w.line("bc_emit_count(cx, 1);");
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
  out.objects_read = w.objects_read();
  out.columns = w.columns_used();
  return out;
}

}  // namespace bytecask::sql
