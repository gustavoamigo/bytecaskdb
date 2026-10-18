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

// Writes the body line by line, and records which catalog constants it used.
class Writer {
public:
  explicit Writer(const Catalog& cat, const Table& t) : cat_{cat}, t_{t} {}

  [[nodiscard]] auto catalog() const -> const Catalog& { return cat_; }

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

  // Constant names, recorded as used.
  auto tbl() -> std::string { return use("TBL_" + up(t_.name)); }
  auto lay() -> std::string { return use("LAY_" + up(t_.name)); }
  auto idx(const Index& ix) -> std::string { return use("IDX_" + up(ix.name)); }
  auto col(std::uint32_t id) -> std::string { return use(up(t_.name) + "_" + up(t_.column(id)->name) + "_COL"); }
  auto pos(std::uint32_t id) -> std::string {
    const auto* c = t_.column(id);
    return use(up(t_.name) + "_" + up(c->name) + (is_fixed(c->type) ? "_OFF" : "_SLOT"));
  }
  auto next_key() -> std::string { return "k" + std::to_string(keys_++); }

  // The constants block, in the catalog's order.
  auto constants() const -> std::string {
    std::string out = "enum { ";
    auto first = true;
    for (const auto& [name, value] : cat_.constants(t_)) {
      if (!used_.contains(name)) continue;
      out += (first ? "" : ", ") + name + " = " + std::to_string(value);
      first = false;
    }
    return out + " };";
  }

  [[nodiscard]] auto body() const -> const std::string& { return body_; }

private:
  static auto up(std::string s) -> std::string {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
  }
  auto use(std::string name) -> std::string {
    used_.insert(name);
    return name;
  }

  const Catalog& cat_;
  const Table& t_;
  std::string body_;
  std::size_t depth_{1};
  int keys_{0};
  std::set<std::string> used_;
};

auto param(PhysType t, std::uint32_t n) -> std::string {
  return "bc_param_" + suffix(t) + "(cx, " + std::to_string(n) + ")";
}

auto param_null(std::uint32_t n) -> std::string { return "bc_param_null(cx, " + std::to_string(n) + ")"; }

auto read_col(Writer& w, const Column& c, const std::string& row) -> std::string {
  return "bc_col_" + suffix(c.type) + "(cx, " + row + ", " + w.pos(c.id) + ")";
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

// Builds the primary key from parameters; returns the builder's name.
auto pk_key(Writer& w, const Table& t, const std::vector<std::uint32_t>& params) -> std::string {
  auto k = w.next_key();
  w.line("bc_key " + k + " = bc_key_begin(cx, " + w.tbl() + ");");
  for (std::size_t i = 0; i < t.pk_col_ids.size(); ++i) {
    auto type = t.column(t.pk_col_ids[i])->type;
    w.line("bc_key_" + suffix(type) + "(cx, " + k + ", " + param(type, params[i]) + ");");
  }
  return k;
}

void found_or_return(Writer& w, bool count) {
  w.open("if (!found)");
  if (count) w.line("bc_emit_count(cx, 0);");
  w.line("return;");
  w.close();
}

void emit_columns(Writer& w, const Table& t, const std::vector<std::uint32_t>& targets, const std::string& row) {
  w.line("bc_emit_begin(cx);");
  for (auto id : targets) {
    const auto& c = *t.column(id);
    if (!c.nullable) {
      w.line("bc_emit_" + suffix(c.type) + "(cx, " + read_col(w, c, row) + ");");
      continue;
    }
    w.open("if (" + col_null(w, c, row) + ")");
    w.line("bc_emit_null(cx);");
    w.otherwise();
    w.line("bc_emit_" + suffix(c.type) + "(cx, " + read_col(w, c, row) + ");");
    w.close();
  }
  w.line("bc_emit_end(cx);");
}

auto insert(Writer& w, const bound::Insert& s) -> void {
  const auto& t = *s.table;
  auto value_param = [&](std::uint32_t col_id) -> std::optional<std::uint32_t> {
    auto at = static_cast<std::size_t>(t.column(col_id) - t.columns.data());
    if (!s.values[at]) return std::nullopt;
    return s.values[at]->param;
  };
  std::vector<std::uint32_t> pk_ps;
  for (auto id : t.pk_col_ids) pk_ps.push_back(*value_param(id));
  auto k0 = pk_key(w, t, pk_ps);
  w.line("bc_view pk = bc_key_end(cx, " + k0 + ");");
  // A duplicate is a constraint error, not a conflict: checked on the
  // snapshot and raised; the guard below covers the race.
  w.open("if (bc_exists(cx, pk))");
  w.line("bc_raise(cx, BC_C_UNIQUE, " + w.tbl() + ");");
  w.line("return;");
  w.close();
  w.line("bc_row r0 = bc_row_begin(cx, " + w.lay() + ");");
  for (const auto& c : t.columns) {
    auto p = value_param(c.id);
    auto set = "bc_row_set_" + suffix(c.type) + "(cx, r0, " + w.col(c.id) + ", ";
    if (!p) {
      w.line("bc_row_set_null(cx, r0, " + w.col(c.id) + ");");
    } else if (!c.nullable) {
      w.line(set + param(c.type, *p) + ");");
    } else {
      w.open("if (" + param_null(*p) + ")");
      w.line("bc_row_set_null(cx, r0, " + w.col(c.id) + ");");
      w.otherwise();
      w.line(set + param(c.type, *p) + ");");
      w.close();
    }
  }
  w.line("bc_view row = bc_row_end(cx, r0);");
  w.line("bc_guard_absent(cx, pk);");
  w.line("bc_put(cx, pk, row);");
  for (const auto& ix : t.indexes) {
    auto k = w.next_key();
    w.line("bc_key " + k + " = bc_key_begin(cx, " + w.idx(ix) + ");");
    for (auto id : ix.col_ids) {
      const auto& c = *t.column(id);
      auto p = value_param(id);
      auto part = "bc_key_" + suffix(c.type) + "(cx, " + k + ", ";
      if (!p) {
        w.line("bc_key_null(cx, " + k + ");");
      } else if (!c.nullable) {
        w.line(part + param(c.type, *p) + ");");
      } else {
        w.open("if (" + param_null(*p) + ")");
        w.line("bc_key_null(cx, " + k + ");");
        w.otherwise();
        w.line(part + param(c.type, *p) + ");");
        w.close();
      }
    }
    for (std::size_t i = 0; i < t.pk_col_ids.size(); ++i) {
      auto type = t.column(t.pk_col_ids[i])->type;
      w.line("bc_key_" + suffix(type) + "(cx, " + k + ", " + param(type, pk_ps[i]) + ");");
    }
    w.line("bc_put(cx, bc_key_end(cx, " + k + "), bc_empty(cx));");
  }
  w.line("bc_emit_count(cx, 1);");
}

auto select_by_pk(Writer& w, const bound::Select& s, const std::vector<std::uint32_t>& pk_ps) -> void {
  const auto& t = *s.table;
  return_if_null(w, pk_ps, false);
  auto k0 = pk_key(w, t, pk_ps);
  w.line("bool found = false;");
  w.line("bc_view row = bc_get(cx, bc_key_end(cx, " + k0 + "), &found);");
  found_or_return(w, false);
  emit_columns(w, t, s.targets, "row");
}

// Rule 2: every conjunct ranges over the first column of one index.
auto select_by_index(Writer& w, const bound::Select& s) -> void {
  const auto& t = *s.table;
  if (s.where.empty()) no_plan("a full scan is not supported");
  auto col_id = s.where[0].col_id;
  auto ix_it = std::ranges::find_if(t.indexes, [&](const Index& ix) { return ix.col_ids[0] == col_id; });
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

  std::vector<std::uint32_t> params;
  for (const auto& c : s.where) {
    if (std::ranges::find(params, c.param) == params.end()) params.push_back(c.param);
  }
  return_if_null(w, params, false);

  auto type = t.column(col_id)->type;
  auto nullable = t.column(col_id)->nullable;
  // The bounds below assume ASC: nulls first, successor for > and <=. A DESC
  // part inverts both, which this generator does not handle yet (#489 adds DESC).
  if (const auto* kl = w.catalog().key_layout(ix.id); kl == nullptr || kl->parts.empty() || kl->parts[0].desc) {
    no_plan("a range on a DESC index part is not supported");
  }
  // null_part: the bound is the null part itself. A comparison with NULL is
  // never true, so a scan with no lower bound starts past the null entries,
  // which sort first: at the successor of the null part.
  auto key_from = [&](const std::optional<bound::Compare>& c, const std::string& name, bool successor,
                      bool null_part = false) {
    auto k = w.next_key();
    w.line("bc_key " + k + " = bc_key_begin(cx, " + w.idx(ix) + ");");
    if (c) w.line("bc_key_" + suffix(type) + "(cx, " + k + ", " + param(type, c->param) + ");");
    if (null_part) w.line("bc_key_null(cx, " + k + ");");
    // A key part is self-delimiting and the primary key follows it, so an
    // exclusive bound on the part is the successor of its encoded prefix.
    auto end = "bc_key_end(cx, " + k + ")";
    w.line("bc_view " + name + " = " + (successor ? "bc_key_successor(cx, " + end + ")" : end) + ";");
  };
  if (eq) {
    key_from(eq, "lo", false);
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(ix) + ", lo, lo, BC_SCAN_PREFIX);");
  } else {
    auto skip_nulls = !lo && nullable;
    key_from(lo, "lo", skip_nulls || (lo && lo->op == Op::Gt), skip_nulls);
    key_from(hi, "hi", !hi || hi->op == Op::Le);
    w.line("bc_cursor c = bc_scan_open(cx, " + w.idx(ix) + ", lo, hi, BC_SCAN_FORWARD);");
  }
  w.open("while (bc_scan_next(cx, c))");
  w.line("bool found = false;");
  w.line("bc_view row = bc_lookup(cx, " + w.tbl() + ", bc_scan_pk(cx, c), &found);");
  w.open("if (found)");
  emit_columns(w, t, s.targets, "row");
  w.close();
  w.close();
}

// Reads the row of a primary-key UPDATE or DELETE into `old`.
auto fetch_old(Writer& w, const Table& t, const std::vector<std::uint32_t>& pk_ps) -> void {
  return_if_null(w, pk_ps, true);
  auto k0 = pk_key(w, t, pk_ps);
  w.line("bc_view pk = bc_key_end(cx, " + k0 + ");");
  w.line("bool found = false;");
  w.line("bc_view old = bc_get(cx, pk, &found);");
  found_or_return(w, true);
}

auto update(Writer& w, const bound::Update& s) -> void {
  const auto& t = *s.table;
  auto pk_ps = pk_params(t, s.where);
  if (!pk_ps) no_plan("UPDATE must name its row by the full primary key");
  for (const auto& [id, e] : s.set) {
    for (const auto& ix : t.indexes) {
      if (std::ranges::find(ix.col_ids, id) != ix.col_ids.end()) no_plan("updating an indexed column is not supported");
    }
  }
  fetch_old(w, t, *pk_ps);
  w.line("bc_row r0 = bc_row_from(cx, " + w.lay() + ", old);");
  for (const auto& [id, e] : s.set) {
    const auto& c = *t.column(id);
    auto set = "bc_row_set_" + suffix(c.type) + "(cx, r0, " + w.col(id) + ", ";
    std::string value, null_if;
    switch (e.kind) {
      case bound::Expr::Kind::Param:
        value = param(c.type, e.param);
        null_if = param_null(e.param);
        break;
      case bound::Expr::Kind::Column:
        value = read_col(w, *t.column(e.col_id), "old");
        null_if = t.column(e.col_id)->nullable ? col_null(w, *t.column(e.col_id), "old") : "";
        break;
      case bound::Expr::Kind::Sub: {
        const auto& src = *t.column(e.col_id);
        auto null_src = src.nullable ? col_null(w, src, "old") : "";
        value = "bc_sub(cx, " + read_col(w, src, "old") + ", " + param(PhysType::I64, e.param) + ")";
        null_if = (null_src.empty() ? "" : null_src + " || ") + param_null(e.param);
        break;
      }
    }
    if (null_if.empty()) {
      w.line(set + value + ");");
      continue;
    }
    w.open("if (" + null_if + ")");
    w.line("bc_row_set_null(cx, r0, " + w.col(id) + ");");
    w.otherwise();
    w.line(set + value + ");");
    w.close();
  }
  w.line("bc_put(cx, pk, bc_row_end(cx, r0));");
  w.line("bc_emit_count(cx, 1);");
}

auto del(Writer& w, const bound::Delete& s) -> void {
  const auto& t = *s.table;
  auto pk_ps = pk_params(t, s.where);
  if (!pk_ps) no_plan("DELETE must name its row by the full primary key");
  fetch_old(w, t, *pk_ps);
  w.line("bc_del(cx, pk);");
  for (const auto& ix : t.indexes) {
    auto k = w.next_key();
    w.line("bc_key " + k + " = bc_key_begin(cx, " + w.idx(ix) + ");");
    for (auto id : ix.col_ids) {
      const auto& c = *t.column(id);
      auto part = "bc_key_" + suffix(c.type) + "(cx, " + k + ", " + read_col(w, c, "old") + ");";
      if (!c.nullable) {
        w.line(part);
        continue;
      }
      w.open("if (" + col_null(w, c, "old") + ")");
      w.line("bc_key_null(cx, " + k + ");");
      w.otherwise();
      w.line(part);
      w.close();
    }
    for (std::size_t i = 0; i < t.pk_col_ids.size(); ++i) {
      auto type = t.column(t.pk_col_ids[i])->type;
      w.line("bc_key_" + suffix(type) + "(cx, " + k + ", " + param(type, (*pk_ps)[i]) + ");");
    }
    w.line("bc_del(cx, bc_key_end(cx, " + k + "));");
  }
  w.line("bc_emit_count(cx, 1);");
}

}  // namespace

auto generate_plan(const bound::Bound& b, const Catalog& cat, std::string_view sql) -> GeneratedPlan {
  GeneratedPlan out;
  out.param_types = b.param_types;
  out.catalog_version = cat.version();
  auto build = [&](const Table& t, auto&& body) {
    Writer w{cat, t};
    body(w);
    out.source = "/* " + std::string{sql} + "\n   isolation: snapshot   schema: catalog v" +
                 std::to_string(cat.version()) + " */\n#include \"bc_plan.h\"\n" + w.constants() +
                 "\n\nvoid plan(bc_ctx *cx) {\n" + w.body() + "}\n";
  };
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, bound::Insert>) {
          out.tables_written = {s.table->id};
          build(*s.table, [&](Writer& w) { insert(w, s); });
        } else if constexpr (std::is_same_v<T, bound::Select>) {
          out.tables_read = {s.table->id};
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
          out.tables_read = out.tables_written = {s.table->id};
          build(*s.table, [&](Writer& w) { update(w, s); });
        } else if constexpr (std::is_same_v<T, bound::Delete>) {
          out.tables_read = out.tables_written = {s.table->id};
          build(*s.table, [&](Writer& w) { del(w, s); });
        } else {
          throw SqlError{"DDL has no plan", -1};
        }
      },
      b.stmt);
  return out;
}

}  // namespace bytecask::sql
