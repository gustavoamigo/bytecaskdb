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
  auto tbl() -> std::string { return use("TBL_" + std::to_string(p_.table)); }
  auto idx(std::uint32_t id) -> std::string { return use("IDX_" + std::to_string(id)); }
  // A scan's object: the table (its primary key) or an index.
  auto obj(std::uint32_t id) -> std::string { return id == p_.table ? tbl() : idx(id); }
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
// never true, so the statement matches no row; `then` reports that.
void return_if_null(Writer& w, const std::vector<std::uint32_t>& params, const std::vector<std::string>& then = {}) {
  std::string cond;
  for (auto p : params) cond += (cond.empty() ? "" : " || ") + param_null(p);
  w.open("if (" + cond + ")");
  for (const auto& line : then) w.line(line);
  w.line("return;");
  w.close();
}

const std::vector<std::string> kNoRowWritten{"bc_emit_count(cx, 0);"};

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
  return_if_null(w, g.key_params, count ? kNoRowWritten : std::vector<std::string>{});
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

// Whether the statement emits its rows: LIMIT 0 emits none.
auto emits(const ir::Select& s) -> bool { return !s.limit || *s.limit > 0; }

// A COUNT over no rows, when a parameter the WHERE compares is NULL.
void return_count_if_null(Writer& w, const ir::Select& s, const std::vector<std::uint32_t>& params) {
  return_if_null(w, params, emits(s) ? count_row("0") : std::vector<std::string>{});
}

void print_point_select(Writer& w, const ir::Select& s, const ir::PointGet& g) {
  w.reads(w.p().table);
  if (s.count) {
    return_count_if_null(w, s, g.key_params);
    auto k = key(w, w.p().table, g.key_params);
    w.line("int64_t n = 0;");
    if (!s.count->arg) {
      w.line("if (bc_exists(cx, " + k + ")) n = 1;");
    } else {
      w.line("bool found = false;");
      w.line("bc_view row = bc_get(cx, " + k + ", &found);");
      w.line("if (found && !" + col_null(w, *s.count->arg, "row") + ") n = 1;");
    }
    if (emits(s)) emit_count(w, "n");
    return;
  }
  fetch_row(w, g, "row", false);
  if (emits(s)) emit_columns(w, s.targets, "row");
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

// The loop over the open cursor `c`: each entry's row emitted, or counted.
void scan_loop(Writer& w, const ir::Select& s, const ir::Scan& sc) {
  auto limited = s.limit.has_value() && !s.count;
  w.open(std::string{"while ("} + (limited ? "n < " + std::to_string(*s.limit) + " && " : "") + "bc_scan_next(cx, c))");
  if (s.count) {
    const auto& arg = *s.count->arg;  // a COUNT the keys can't answer has an argument that may be NULL
    if (sc.covering) {
      w.line("bc_view key = bc_scan_key(cx, c);");
      w.line("if (!bc_key_part_null(cx, " + w.obj(sc.object) + ", key, " + w.part(sc.object, arg) + ")) ++n;");
    } else {
      w.reads(w.p().table);
      w.line("bc_view row = bc_scan_row(cx, c);");
      w.line("if (!" + col_null(w, arg, "row") + ") ++n;");
    }
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
    emit_columns(w, s.targets, "row");
  }
  if (limited) w.line("++n;");
  w.close();
}

void print_scan_select(Writer& w, const ir::Select& s, const ir::Scan& sc) {
  if (!s.count) {
    return_if_null(w, sc.null_checks);
    if (s.limit) w.line("int64_t n = 0;");
    open_scan(w, sc);
    scan_loop(w, s, sc);
    return;
  }
  return_count_if_null(w, s, sc.null_checks);
  if (s.count->from_keys) {
    // Every entry of the range is one row to count.
    w.reads(sc.object);
    if (const auto* r = std::get_if<ir::Range>(&sc.rest)) {
      print_range(w, sc, *r);
    } else {
      w.line("bc_view lo = " + key(w, sc.object, sc.prefix) + ";");
      w.line("bc_view hi = bc_key_successor(cx, lo);");
    }
    w.line("int64_t n = bc_count_range(cx, " + w.obj(sc.object) + ", lo, hi, INT64_MAX);");
  } else {
    w.line("int64_t n = 0;");
    open_scan(w, sc);
    scan_loop(w, s, sc);
  }
  if (emits(s)) emit_count(w, "n");
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

void print(Writer& w, const ir::Select& s) {
  std::visit(
      [&](const auto& from) {
        using T = std::decay_t<decltype(from)>;
        if constexpr (std::is_same_v<T, ir::PointGet>) {
          print_point_select(w, s, from);
        } else if constexpr (std::is_same_v<T, ir::Scan>) {
          print_scan_select(w, s, from);
        } else {
          print_split_select(w, s, from);
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
      auto fn = e.kind == ir::Expr::Kind::Add ? "bc_add" : "bc_sub";
      value = std::string{fn} + "(cx, " + read_col(w, e.col, "old") + ", " + param(PhysType::I64, e.param) + ")";
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
          if (s.count) out.result_types.push_back(PhysType::I64);
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
    if (sel->count) {
      out.result_names.emplace_back("count");
      out.result_sql.push_back(SqlType{SqlKind::BigInt});
    }
  }
  out.objects_read = w.objects_read();
  out.columns = w.columns_used();
  return out;
}

}  // namespace bytecask::sql
