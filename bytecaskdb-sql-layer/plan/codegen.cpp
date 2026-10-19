// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// codegen.cpp — the planner and code generator's definitions; see codegen.cppm.

module;
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"

module bytecask.sql.codegen;

namespace bytecask::sql {

namespace {

using ast::SqlError;
using Op = ast::Compare::Op;

[[noreturn]] void no_plan(const std::string& why) { throw SqlError{"no plan: " + why, -1}; }

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

// Writes the body line by line, and records which catalog constants it used.
class Writer {
public:
  explicit Writer(const Table& t) : t_{t} {}

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
  auto tbl() -> std::string { return use("TBL_" + std::to_string(t_.id)); }
  auto idx(const Index& ix) -> std::string { return use("IDX_" + std::to_string(ix.id)); }
  auto col(std::uint32_t id) -> std::string {
    cols_.insert(id);
    return use("COL_" + std::to_string(t_.id) + "_" + std::to_string(id));
  }
  // The position of a column in an index's key, for the key-part decoders.
  auto part(const Index& ix, std::uint32_t col_id) -> std::string {
    return use("PART_" + std::to_string(ix.id) + "_" + std::to_string(col_id));
  }
  // A table or index the plan reads.
  void reads(std::uint32_t id) { reads_.insert(id); }
  [[nodiscard]] auto objects_read() const -> std::vector<std::uint32_t> { return {reads_.begin(), reads_.end()}; }
  // Every column the plan reads or sets, with its type.
  [[nodiscard]] auto columns_used() const -> std::vector<ColumnDep> {
    std::vector<ColumnDep> out;
    for (auto id : cols_) out.push_back({t_.id, id, t_.column(id)->type});
    return out;
  }
  auto next_key() -> std::string { return "k" + std::to_string(keys_++); }

  // The constants block, in the catalog's order.
  auto constants() const -> std::string {
    std::string out = "enum { ";
    auto first = true;
    for (const auto& c : Catalog::constants(t_)) {
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
    for (const auto& c : Catalog::constants(t_)) {
      if (used_.contains(c.name)) out += (out.empty() ? "" : ", ") + c.name + " " + c.object;
    }
    return out;
  }

  [[nodiscard]] auto body() const -> const std::string& { return body_; }

private:
  auto use(std::string name) -> std::string {
    used_.insert(name);
    return name;
  }

  const Table& t_;
  std::string body_;
  std::size_t depth_{1};
  int keys_{0};
  std::set<std::string> used_;
  std::set<std::uint32_t> cols_, reads_;
};

auto param(PhysType t, std::uint32_t n) -> std::string {
  return "bc_param_" + suffix(t) + "(cx, " + std::to_string(n) + ")";
}

auto param_null(std::uint32_t n) -> std::string { return "bc_param_null(cx, " + std::to_string(n) + ")"; }

auto read_col(Writer& w, const Column& c, const std::string& row) -> std::string {
  return "bc_col_" + suffix(c.type) + "(cx, " + row + ", " + w.col(c.id) + ")";
}

auto col_null(Writer& w, const Column& c, const std::string& row) -> std::string {
  return "bc_col_null(cx, " + row + ", " + w.col(c.id) + ")";
}

// The parameter each primary key column is compared equal to, or nullopt
// if rule 1 does not apply.
auto pk_params(const Table& t, const std::vector<bound::Compare>& where) -> std::optional<std::vector<std::uint32_t>> {
  std::vector<std::uint32_t> out;
  for (auto id : t.pk_col_ids) {
    auto it = std::ranges::find_if(where, [&](const auto& c) { return c.col_id == id && c.op == Op::Eq; });
    if (it == where.end()) return std::nullopt;
    out.push_back(it->param);
  }
  if (where.size() != t.pk_col_ids.size()) no_plan("predicates beyond the primary key are not supported");
  return out;
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

void found_or_return(Writer& w, bool count) {
  w.open("if (!found)");
  if (count) w.line("bc_emit_count(cx, 0);");
  w.line("return;");
  w.close();
}

// Reads the row the primary-key parameters name into `name`, returning
// when a parameter is NULL or there is no such row.
void fetch_row(Writer& w, const std::vector<std::uint32_t>& pk_ps, const std::string& name, bool count) {
  return_if_null(w, pk_ps, count);
  w.line("bool found = false;");
  w.line("bc_view " + name + " = bc_get(cx, " + pk_key(w, pk_ps) + ", &found);");
  found_or_return(w, count);
}

void emit_columns(Writer& w, const std::vector<std::uint32_t>& targets, const std::string& row) {
  w.line("bc_emit_begin(cx);");
  for (auto id : targets) w.line("bc_emit_col(cx, " + row + ", " + w.col(id) + ");");
  w.line("bc_emit_end(cx);");
}

auto insert(Writer& w, const bound::Insert& s) -> void {
  const auto& t = *s.table;
  // bc_row_begin sets every column to its default; a column not given keeps
  // it. The runtime checks the keys and writes the index entries.
  w.line("bc_row r = bc_row_begin(cx, " + w.tbl() + ");");
  for (std::size_t i = 0; i < t.columns.size(); ++i) {
    if (!s.values[i]) continue;
    w.line("bc_row_set_param(cx, r, " + w.col(t.columns[i].id) + ", " + std::to_string(s.values[i]->param) + ");");
  }
  w.line("bc_insert(cx, " + w.tbl() + ", bc_row_end(cx, r));");
  w.line("bc_emit_count(cx, 1);");
}

auto select_by_pk(Writer& w, const bound::Select& s, const std::vector<std::uint32_t>& pk_ps) -> void {
  w.reads(s.table->id);
  fetch_row(w, pk_ps, "row", false);
  emit_columns(w, s.targets, "row");
}

// Rule 2: every conjunct ranges over the first column of one index.
auto select_by_index(Writer& w, const bound::Select& s) -> void {
  const auto& t = *s.table;
  if (s.where.empty()) no_plan("a full scan is not supported");
  auto col_id = s.where[0].col_id;
  auto ix_it = std::ranges::find_if(t.indexes, [&](const Index& ix) { return ix.parts[0].col_id == col_id; });
  if (ix_it == t.indexes.end()) no_plan("no index leads with column \"" + t.column(col_id)->name + "\"");
  const auto& ix = *ix_it;
  std::optional<bound::Compare> lo, hi, eq;
  for (const auto& c : s.where) {
    if (c.col_id != col_id) no_plan("predicates on more than one column are not supported");
    auto& slot = c.op == Op::Eq ? eq : (c.op == Op::Gt || c.op == Op::Ge) ? lo : hi;
    if (slot) no_plan("two bounds on the same side are not supported");
    slot = c;
  }
  if (eq && (lo || hi)) no_plan("an equality and a range on one column are not supported");
  // Covering when the index key holds every column the result needs: its
  // parts, then the primary key's. Not a DOUBLE: the key encoding writes
  // -0.0 as 0.0 and every NaN as one, so only the row holds its exact value.
  auto in_key = [&](std::uint32_t id) {
    if (t.column(id)->type == PhysType::F64) return false;
    return std::ranges::any_of(ix.parts, [&](const IndexPart& p) { return p.col_id == id; }) ||
           std::ranges::find(t.pk_col_ids, id) != t.pk_col_ids.end();
  };
  auto covering = std::ranges::all_of(s.targets, in_key);
  w.reads(ix.id);
  if (!covering) w.reads(t.id);

  std::vector<std::uint32_t> params;
  for (const auto& c : s.where) {
    if (std::ranges::find(params, c.param) == params.end()) params.push_back(c.param);
  }
  return_if_null(w, params, false);

  auto nullable = t.column(col_id)->nullable;
  auto desc = ix.parts[0].desc;
  // A key bound: the index prefix, then the compared value or the null part
  // (or neither: the prefix alone), as is or its successor. A key part is
  // self-delimiting and the primary key follows it, so a bound that must
  // pass every entry with the part equal to a value is that value's
  // successor. The prefix alone is the index's start, its successor the
  // end: both are the empty bound.
  auto key_from = [&](const std::optional<bound::Compare>& c, const std::string& name, bool successor,
                      bool null_part = false) {
    std::string key;
    if (null_part) {
      auto k = w.next_key();
      w.line("bc_key " + k + " = bc_key_begin(cx, " + w.idx(ix) + ");");
      w.line("bc_key_null(cx, " + k + ");");
      key = "bc_key_end(cx, " + k + ")";
    } else if (c) {
      key = "bc_key_params(cx, " + w.idx(ix) + ", " + std::to_string(c->param) + ", 1)";
    } else {
      w.line("bc_view " + name + " = bc_empty(cx);");
      return;
    }
    w.line("bc_view " + name + " = " + (successor ? "bc_key_successor(cx, " + key + ")" : key) + ";");
  };
  if (eq) {
    key_from(eq, "lo", false);
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(ix) + ", lo, lo, BC_SCAN_PREFIX);");
  } else if (!desc) {
    // ASC: key order is value order, nulls first. A comparison with NULL is
    // never true, so with no lower bound the scan starts past the null
    // entries, at the successor of the null part.
    auto skip_nulls = !lo && nullable;
    key_from(lo, "lo", skip_nulls || (lo && lo->op == Op::Gt), skip_nulls);
    key_from(hi, "hi", hi && hi->op == Op::Le);
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(ix) + ", lo, hi, BC_SCAN_FORWARD);");
  } else {
    // DESC: key order is descending value order, nulls last. The upper value
    // bound gives the lower key bound and the lower value bound the upper;
    // with no lower value bound the scan stops before the null entries, at
    // the null part.
    key_from(hi, "lo", hi && hi->op == Op::Lt);
    if (lo) {
      key_from(lo, "hi", lo->op == Op::Ge);
    } else {
      key_from(std::nullopt, "hi", false, nullable);
    }
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(ix) + ", lo, hi, BC_SCAN_FORWARD);");
  }
  w.open("while (bc_scan_next(cx, c))");
  if (covering) {
    // Every result column from the entry's key: no row read.
    w.line("bc_view key = bc_scan_key(cx, c);");
    w.line("bc_emit_begin(cx);");
    for (auto id : s.targets) w.line("bc_emit_key_part(cx, " + w.idx(ix) + ", key, " + w.part(ix, id) + ");");
    w.line("bc_emit_end(cx);");
  } else {
    w.line("bc_view row = bc_scan_row(cx, c);");
    emit_columns(w, s.targets, "row");
  }
  w.close();
}

auto update(Writer& w, const bound::Update& s) -> void {
  const auto& t = *s.table;
  auto pk_ps = pk_params(t, s.where);
  if (!pk_ps) no_plan("UPDATE must name its row by the full primary key");
  w.reads(t.id);
  fetch_row(w, *pk_ps, "old", true);
  w.line("bc_row r = bc_row_from(cx, " + w.tbl() + ", old);");
  for (const auto& [id, e] : s.set) {
    const auto& c = *t.column(id);
    if (e.kind == bound::Expr::Kind::Param) {
      w.line("bc_row_set_param(cx, r, " + w.col(id) + ", " + std::to_string(e.param) + ");");
      continue;
    }
    auto set = "bc_row_set_" + suffix(c.type) + "(cx, r, " + w.col(id) + ", ";
    std::string value, null_if;
    const auto& src = *t.column(e.col_id);
    auto null_src = src.nullable ? col_null(w, src, "old") : "";
    if (e.kind == bound::Expr::Kind::Column) {
      value = read_col(w, src, "old");
      null_if = null_src;
    } else {
      value = "bc_sub(cx, " + read_col(w, src, "old") + ", " + param(PhysType::I64, e.param) + ")";
      null_if = (null_src.empty() ? "" : null_src + " || ") + param_null(e.param);
    }
    if (null_if.empty()) {
      w.line(set + value + ");");
      continue;
    }
    w.open("if (" + null_if + ")");
    w.line("bc_row_set_null(cx, r, " + w.col(id) + ");");
    w.otherwise();
    w.line(set + value + ");");
    w.close();
  }
  // The runtime moves the index entries whose keys changed.
  w.line("bc_update(cx, " + w.tbl() + ", old, bc_row_end(cx, r));");
  w.line("bc_emit_count(cx, 1);");
}

auto del(Writer& w, const bound::Delete& s) -> void {
  const auto& t = *s.table;
  auto pk_ps = pk_params(t, s.where);
  if (!pk_ps) no_plan("DELETE must name its row by the full primary key");
  w.reads(t.id);
  fetch_row(w, *pk_ps, "old", true);
  w.line("bc_delete(cx, " + w.tbl() + ", old);");
  w.line("bc_emit_count(cx, 1);");
}

}  // namespace

auto generate_plan(const bound::Bound& b, const Catalog& cat, std::string_view sql) -> GeneratedPlan {
  GeneratedPlan out;
  out.param_types = b.param_types;
  out.catalog_version = cat.version();
  auto build = [&](const Table& t, auto&& body) {
    Writer w{t};
    body(w);
    out.source = comment_line(sql) + comment_line("isolation: snapshot   schema: catalog v" + std::to_string(cat.version())) +
                 comment_line("objects: " + w.legend()) + "#include \"bc_plan.h\"\n" + w.constants() +
                 "\n\nvoid plan(bc_ctx *cx) {\n" + w.body() + "}\n";
    out.objects_read = w.objects_read();
    out.columns = w.columns_used();
  };
  // The runtime maintains the table's indexes: a write plan names none.
  auto writes = [&](const Table& t) { out.tables_written = {t.id}; };
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, bound::Insert>) {
          writes(*s.table);
          build(*s.table, [&](Writer& w) { insert(w, s); });
        } else if constexpr (std::is_same_v<T, bound::Select>) {
          out.returns_rows = true;
          for (auto id : s.targets) out.result_types.push_back(s.table->column(id)->type);
          auto pk_ps = pk_params(*s.table, s.where);
          build(*s.table, [&](Writer& w) {
            if (pk_ps) {
              select_by_pk(w, s, *pk_ps);
            } else {
              select_by_index(w, s);
            }
          });
        } else if constexpr (std::is_same_v<T, bound::Update>) {
          writes(*s.table);
          build(*s.table, [&](Writer& w) { update(w, s); });
        } else if constexpr (std::is_same_v<T, bound::Delete>) {
          writes(*s.table);
          build(*s.table, [&](Writer& w) { del(w, s); });
        } else {
          throw SqlError{"DDL has no plan", -1};
        }
      },
      b.stmt);
  return out;
}

}  // namespace bytecask::sql
