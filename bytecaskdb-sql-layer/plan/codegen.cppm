// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// codegen.cppm — the code generator: prints the physical plan the search
// built (search.cppm, physical.cppm) as a plan file in the subset of
// docs/plan_api_design.md, plus what the runtime needs to run it (parameter
// types, result layout, schema reference). It is produce/consume
// (docs/planner_design.md, *The physical plan and codegen*): an operator's
// produce asks its input to emit rows, passing what runs on each row, and
// everything between two pipeline breakers becomes one nested loop over API
// cursors. It makes no choice of its own and reads the plan alone, never the
// catalog.
//
// Writes are bc_insert, bc_update and bc_delete: the runtime maintains the
// indexes (docs/plan_api_design.md, *Decisions, not mechanics*).

module;
#include <algorithm>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <limits>
#include <optional>
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
import bytecask.sql.rewrite;
import bytecask.sql.search;

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

// Plans a bound DML statement, its logical plan rewritten, then searched,
// and prints the plan. Throws ast::SqlError (position -1) when no rule
// applies, and std::logic_error if the physical plan fails physical::check,
// a compiler bug.
[[nodiscard]] auto generate_plan(bound::Bound b, const Catalog& cat, std::string_view sql) -> GeneratedPlan;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

using logical::ColumnRef;
using logical::Expr;
using Op = ast::Compare::Op;

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
  explicit Writer(const physical::Plan& p) : p_{p} {}

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
  auto tbl(std::uint32_t id) -> std::string { return use("TBL_" + std::to_string(id)); }
  auto idx(std::uint32_t id) -> std::string { return use("IDX_" + std::to_string(id)); }
  // A scan's or a key's object: a table (its primary key) or an index.
  auto obj(std::uint32_t id) -> std::string {
    auto table = std::ranges::any_of(p_.constants, [&](const auto& c) { return c.name == "TBL_" + std::to_string(id); });
    return table ? tbl(id) : idx(id);
  }
  auto col(const ColumnRef& c) -> std::string {
    cols_.emplace(c.table, c.col, c.type);
    return use("COL_" + std::to_string(c.table) + "_" + std::to_string(c.col));
  }
  // The position of a column in an index's key, for the key-part decoders.
  auto part(std::uint32_t index, const ColumnRef& c) -> std::string {
    return use("PART_" + std::to_string(index) + "_" + std::to_string(c.col));
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
  [[nodiscard]] auto p() const -> const physical::Plan& { return p_; }

private:
  // A name the plan's constants lack would fail at TinyCC: a compiler bug,
  // refused here instead.
  auto use(std::string name) -> std::string {
    if (std::ranges::none_of(p_.constants, [&](const auto& c) { return c.name == name; })) {
      throw std::logic_error{"physical plan: a constant the tables do not have: " + name};
    }
    used_.insert(name);
    return name;
  }

  const physical::Plan& p_;
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

auto successor(const std::string& key) -> std::string { return "bc_key_successor(cx, " + key + ")"; }

// One part of a key being built: a parameter; or a value the plan holds,
// an expression of `type`; or, with neither, NULL.
struct KeySrc {
  std::optional<std::uint32_t> param;
  std::string value;
  PhysType type{PhysType::I64};
};

// A key of `object` from its leading parts, as an expression. One
// bc_key_params when they are consecutive parameters, a builder otherwise.
// In a loop, `miss_if_long` skips to the next iteration when a byte string
// read from a row makes the key too long for any row to have.
auto key(Writer& w, std::uint32_t object, const std::vector<KeySrc>& parts, bool miss_if_long = false)
    -> std::string {
  auto consecutive = !parts.empty() && parts[0].param.has_value();
  for (std::size_t i = 1; consecutive && i < parts.size(); ++i) {
    consecutive = parts[i].param.has_value() && *parts[i].param == *parts[0].param + i;
  }
  if (consecutive) {
    return "bc_key_params(cx, " + w.obj(object) + ", " + std::to_string(*parts[0].param) + ", " +
           std::to_string(parts.size()) + ")";
  }
  auto k = w.next_key();
  w.line("bc_key " + k + " = bc_key_begin(cx, " + w.obj(object) + ");");
  for (const auto& p : parts) {
    if (p.param) {
      w.line("bc_key_param(cx, " + k + ", " + std::to_string(*p.param) + ");");
    } else if (p.value.empty()) {
      w.line("bc_key_null(cx, " + k + ");");
    } else {
      w.line("bc_key_" + suffix(p.type) + "(cx, " + k + ", " + p.value + ");");
    }
  }
  auto long_value =
      std::ranges::any_of(parts, [](const KeySrc& p) { return !p.value.empty() && p.type == PhysType::Bytes; });
  if (miss_if_long && long_value) w.line("if (!bc_key_fits(cx, " + k + ")) continue;");
  return "bc_key_end(cx, " + k + ")";
}

// The C name of a computed value (physical::Computed), printed before
// anything else.
auto computed(std::size_t i) -> std::string { return "v" + std::to_string(i); }

// Each computed value, once, before anything is read: as PostgreSQL folds
// $n ± k when it binds, one that leaves INTEGER fails the statement whatever
// the other parameters are; a NULL $n gives NULL, never an error.
void print_computed(Writer& w) {
  const auto& comp = w.p().computed;
  for (std::size_t i = 0; i < comp.size(); ++i) {
    const auto& v = comp[i];
    auto name = computed(i);
    auto sum = std::string{v.offset < 0 ? "bc_sub" : "bc_add"} + "(cx, " + param(PhysType::I64, v.param) + ", " +
               constant(v.offset < 0 ? -v.offset : v.offset) + ")";
    w.line("int64_t " + name + " = 0;");
    w.line("if (!" + param_null(v.param) + ") " + name + " = bc_check_int(cx, " + sum + ", BC_INTEGER, " + w.tbl(v.table) +
           ");");
  }
}

auto op_text(Op op) -> std::string {
  switch (op) {
    case Op::Eq: return "==";
    case Op::Lt: return "<";
    case Op::Le: return "<=";
    case Op::Gt: return ">";
    case Op::Ge: return ">=";
  }
  return "==";
}

// `if (any parameter is NULL) { <then> return; }`: a comparison with NULL is
// never true, so the statement matches no row; `then` reports that.
void return_if_null(Writer& w, const std::vector<std::uint32_t>& params, const std::vector<std::string>& then) {
  std::string cond;
  for (auto p : params) cond += (cond.empty() ? "" : " || ") + param_null(p);
  w.open("if (" + cond + ")");
  for (const auto& line : then) w.line(line);
  w.line("return;");
  w.close();
}

auto count_row(const std::string& n) -> std::vector<std::string> {
  return {"bc_emit_begin(cx);", "bc_emit_i64(cx, " + n + ");", "bc_emit_end(cx);"};
}

// The aggregate's row over no rows: COUNT 0, SUM and MIN NULL.
auto empty_row(const Expr& a) -> std::vector<std::string> {
  if (a.fn == Expr::Fn::Count) return count_row("0");
  return {"bc_emit_begin(cx);", "bc_emit_null(cx);", "bc_emit_end(cx);"};
}

// What a COUNT(DISTINCT) groups its values in: ample for any key set a
// statement of the MVP reads; past it the statement fails (BC_E_MEMORY).
constexpr std::uint32_t kGroupBytes = 16u << 20;

// Prints a physical plan, operator by operator. produce(n, k) prints n's
// code, and k at the point where each row n emits is in hand: inside the
// loop over a cursor, or once for a single row. `k` is the parent's consume.
class Gen {
public:
  using Consume = std::function<void()>;

  explicit Gen(Writer& w) : w_{w} {}

  void plan(const physical::Node& root) {
    print_computed(w_);
    no_rows_ = no_rows(root);
    if (!w_.p().null_checks.empty()) return_if_null(w_, w_.p().null_checks, no_rows_);
    produce(root, [] {});
  }

private:
  // A row in hand: its variable, or, for a covering scan, the entry its
  // cursor is on.
  struct Binding {
    std::uint32_t table;
    std::string row;
    bool covering{false};
    std::uint32_t object{0};
    bool for_update{false};
  };

  // What the root reports when no row can match: nothing for result rows,
  // a count of 0 for a write, the empty row for an aggregate.
  static auto no_rows(const physical::Node& root) -> std::vector<std::string> {
    return std::visit(
        [](const auto& op) -> std::vector<std::string> {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Update> || std::is_same_v<T, physical::Delete>) {
            return {"bc_emit_count(cx, 0);"};
          } else if constexpr (std::is_same_v<T, physical::Aggregate>) {
            return op.emits ? empty_row(op.agg) : std::vector<std::string>{};
          } else if constexpr (std::is_same_v<T, physical::CountRange>) {
            return op.emits ? count_row("0") : std::vector<std::string>{};
          } else if constexpr (std::is_same_v<T, physical::MinFirstEntry>) {
            return op.emits ? std::vector<std::string>{"bc_emit_begin(cx);", "bc_emit_null(cx);", "bc_emit_end(cx);"}
                            : std::vector<std::string>{};
          } else {
            return {};
          }
        },
        root.op);
  }

  // Whether n's rows come from a loop, or are at most one, read once.
  static auto loops(const physical::Node& n) -> bool {
    return std::visit(
        [](const auto& op) -> bool {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Scan> || std::is_same_v<T, physical::NullSplitScan>) {
            return true;
          } else if constexpr (std::is_same_v<T, physical::IndexJoin>) {
            return loops(*op.outer);
          } else if constexpr (requires { op.input; }) {
            return loops(*op.input);
          } else {
            return false;
          }
        },
        n.op);
  }

  auto binding(std::uint32_t table) const -> const Binding& {
    return *std::ranges::find_if(rows_, [&](const Binding& b) { return b.table == table; });
  }
  auto row_of(const ColumnRef& c) const -> std::string { return binding(c.table).row; }

  auto read_col(const ColumnRef& c) -> std::string {
    return "bc_col_" + suffix(c.type) + "(cx, " + row_of(c) + ", " + w_.col(c) + ")";
  }
  auto col_null(const ColumnRef& c) -> std::string { return "bc_col_null(cx, " + row_of(c) + ", " + w_.col(c) + ")"; }

  // The index of $n ± k, compared with a column of `table`, in the values
  // computed before anything is read.
  auto computed_name(const Expr& e, std::uint32_t table) const -> std::string {
    const auto& comp = w_.p().computed;
    auto it = std::ranges::find(comp, physical::computed_of(e, table));
    return computed(static_cast<std::size_t>(it - comp.begin()));
  }

  // A row with no match for the rest of the pipeline: in a loop, the next
  // one; for a single row, the plan ends, with what the root gives for no
  // rows.
  void miss(const std::string& cond) {
    if (loop_) {
      w_.line("if (" + cond + ") continue;");
      return;
    }
    w_.open("if (" + cond + ")");
    for (const auto& line : no_rows_) w_.line(line);
    w_.line("return;");
    w_.close();
  }

  // Whether the row fails the conjunct, as a C expression: NULL on either
  // side, or the comparison false.
  auto fails(const Expr& c) -> std::string {
    if (c.kind == Expr::Kind::Const) return c.constant != 0 ? "0" : "1";  // folded: never, or always
    const auto& lhs = c.args[0].col;
    const auto& rhs = c.args[1];
    auto out = col_null(lhs);
    std::string value;
    switch (rhs.kind) {
      case Expr::Kind::Column:
        out += " || " + col_null(rhs.col);
        value = read_col(rhs.col);
        break;
      case Expr::Kind::Param:
        out += " || " + param_null(rhs.param);
        value = param(lhs.type, rhs.param);
        break;
      case Expr::Kind::Add:
      case Expr::Kind::Sub:
        out += " || " + param_null(rhs.args[0].param);
        value = computed_name(rhs, lhs.table);
        break;
      case Expr::Kind::Const:
      case Expr::Kind::Compare:
      case Expr::Kind::Aggregate: throw std::logic_error{"physical plan: a filter operand codegen does not print"};
    }
    auto col = read_col(lhs);
    // Text and bytes compare as bytes: COLLATE "C".
    auto cmp = lhs.type == PhysType::Bytes ? "bc_bytes_cmp(cx, " + col + ", " + value + ") " + op_text(c.op) + " 0"
                                           : col + " " + op_text(c.op) + " " + value;
    return out + " || !(" + cmp + ")";
  }

  void produce(const physical::Node& n, const Consume& k) {
    std::visit([&](const auto& op) { produce(op, k); }, n.op);
  }

  // The single row a point lookup names. In a one-row aggregate a missing
  // row is a condition, `hit_`, not an end.
  void produce(const physical::PointGet& g, const Consume& k) {
    w_.reads(g.table);
    std::vector<KeySrc> parts;
    for (const auto& e : g.key) parts.push_back({e.param, {}, PhysType::I64});
    w_.line("bool found = false;");
    w_.line("bc_view " + row_name_ + " = bc_get(cx, " + key(w_, g.table, parts) + ", &found);");
    if (hit_) {
      *hit_ = "found";
    } else {
      miss("!found");
    }
    rows_.push_back({g.table, row_name_, false, 0, g.for_update});
    k();
  }

  // One end of a scan, in key order, as an expression.
  auto bound(const physical::Scan& sc, const physical::KeyBound& b, bool is_lo) -> std::string {
    auto parts = prefix(sc);
    switch (b.kind) {
      case physical::KeyBound::Kind::Open:
        // The prefix's first entry, or past its last; with no prefix, the
        // object's.
        if (parts.empty()) return "bc_empty(cx)";
        return is_lo ? key(w_, sc.object, parts) : successor(key(w_, sc.object, parts));
      case physical::KeyBound::Kind::Value:
        if (b.value->kind == Expr::Kind::Param) {
          parts.push_back({b.value->param, {}, PhysType::I64});
        } else {
          parts.push_back({std::nullopt, computed_name(*b.value, sc.table), PhysType::I64});
        }
        break;
      case physical::KeyBound::Kind::NullPart: parts.push_back({}); break;
    }
    auto k = key(w_, sc.object, parts);
    return b.successor ? successor(k) : k;
  }

  static auto prefix(const physical::Scan& sc) -> std::vector<KeySrc> {
    std::vector<KeySrc> parts;
    for (const auto& e : sc.prefix) parts.push_back({e.param, {}, PhysType::I64});
    return parts;
  }

  // `bc_view lo` and `bc_view hi` of a scan's range.
  void print_range(const physical::Scan& sc, const physical::Range& r) {
    w_.line("bc_view lo = " + bound(sc, r.lo, true) + ";");
    w_.line("bc_view hi = " + bound(sc, r.hi, false) + ";");
  }

  // `bc_cursor c` over every entry under the key `lo` holds.
  void open_prefix(const physical::Scan& sc) {
    w_.line("bc_cursor c = bc_scan_open(cx, " + w_.obj(sc.object) + ", lo, lo, BC_SCAN_PREFIX" +
            (sc.reverse ? " | BC_SCAN_REVERSE" : "") + ");");
  }

  // `bc_view lo`, `bc_view hi` and `bc_cursor c` of the scan.
  void open_scan(const physical::Scan& sc) {
    w_.reads(sc.object);
    if (const auto* r = std::get_if<physical::Range>(&sc.rest)) {
      print_range(sc, *r);
      w_.line("bc_cursor c = bc_scan_open(cx, " + w_.obj(sc.object) + ", lo, hi, " +
              (sc.reverse ? "BC_SCAN_REVERSE" : "BC_SCAN_FORWARD") + ");");
      return;
    }
    w_.line("bc_view lo = " + key(w_, sc.object, prefix(sc)) + ";");
    open_prefix(sc);
  }

  // The NULL part of a NullSplitScan: every entry under the prefix and NULL.
  void open_null_part(const physical::Scan& sc) {
    auto parts = prefix(sc);
    parts.push_back({});
    w_.line("bc_view lo = " + key(w_, sc.object, parts) + ";");
    open_prefix(sc);
  }

  // The loop over the open cursor `c`: each entry's row read unless the scan
  // covers, then k. A LIMIT's counter stops it before the next step.
  void scan_loop(const physical::Scan& sc, const Consume& k) {
    w_.open("while (" + guard_ + "bc_scan_next(cx, c))");
    if (!sc.covering) {
      w_.reads(sc.table);
      w_.line("bc_view " + row_name_ + " = bc_scan_row(cx, c);");
    }
    rows_.push_back({sc.table, row_name_, sc.covering, sc.object, sc.for_update});
    auto outer = loop_;
    loop_ = true;
    k();
    loop_ = outer;
    rows_.pop_back();
    w_.close();
  }

  void produce(const physical::Scan& sc, const Consume& k) {
    open_scan(sc);
    scan_loop(sc, k);
  }

  // The present values and the NULL part, each in its own block, in
  // PostgreSQL's order; one LIMIT counter across both.
  void produce(const physical::NullSplitScan& split, const Consume& k) {
    const auto& sc = split.present;
    for (auto nulls : sc.reverse ? std::vector<bool>{true, false} : std::vector<bool>{false, true}) {
      w_.open("");
      if (nulls) {
        w_.reads(sc.object);
        open_null_part(sc);
      } else {
        open_scan(sc);
      }
      scan_loop(sc, k);
      w_.close();
    }
  }

  // Every entry of the range is one row to count.
  void produce(const physical::CountRange& cr, const Consume&) {
    const auto& sc = cr.scan;
    w_.reads(sc.object);
    if (const auto* r = std::get_if<physical::Range>(&sc.rest)) {
      print_range(sc, *r);
    } else {
      w_.line("bc_view lo = " + key(w_, sc.object, prefix(sc)) + ";");
      w_.line("bc_view hi = bc_key_successor(cx, lo);");
    }
    w_.line("int64_t n = bc_count_range(cx, " + w_.obj(sc.object) + ", lo, hi, INT64_MAX);");
    if (cr.emits) emit_count("n");
  }

  // MIN from the scan's first entry, past the part's NULLs; none is NULL.
  void produce(const physical::MinFirstEntry& m, const Consume&) {
    if (!m.emits) return;
    const auto& sc = m.scan;
    open_scan(sc);
    w_.open("if (bc_scan_next(cx, c))");
    w_.line("bc_emit_begin(cx);");
    if (sc.covering) {
      w_.line("bc_emit_key_part(cx, " + w_.obj(sc.object) + ", bc_scan_key(cx, c), " + w_.part(sc.object, m.arg) + ");");
    } else {
      w_.reads(sc.table);
      w_.line("bc_emit_col(cx, bc_scan_row(cx, c), " + w_.col(m.arg) + ");");
    }
    w_.line("bc_emit_end(cx);");
    w_.otherwise();
    for (const auto& line : no_rows_) w_.line(line);
    w_.close();
  }

  // For each outer row, the inner row its key names. Of an outer column, a
  // NULL names no row, nor does a byte string too long for any key.
  void produce(const physical::IndexJoin& j, const Consume& k) {
    produce(*j.outer, [&] {
      std::vector<KeySrc> parts;
      for (const auto& e : j.inner_key) {
        if (e.kind == Expr::Kind::Param) {
          parts.push_back({e.param, {}, PhysType::I64});
          continue;
        }
        if (e.col.nullable) miss(col_null(e.col));
        parts.push_back({std::nullopt, read_col(e.col), e.col.type});
      }
      w_.reads(j.inner);
      w_.line("bool found1 = false;");
      auto inner_key = key(w_, j.inner, parts, true);
      if (hit_) {
        // No pair without the outer row: the inner one is not read.
        w_.line("bc_view row1 = bc_empty(cx);");
        w_.line("if (found) row1 = bc_get(cx, " + inner_key + ", &found1);");
        *hit_ += " && found1";
      } else {
        w_.line("bc_view row1 = bc_get(cx, " + inner_key + ", &found1);");
        miss("!found1");
      }
      rows_.push_back({j.inner, "row1"});
      k();
      rows_.pop_back();
    });
  }

  void produce(const physical::Filter& f, const Consume& k) {
    produce(*f.input, [&] {
      for (const auto& c : f.conjuncts) {
        if (hit_) {
          *hit_ += " && !(" + fails(c) + ")";
        } else {
          miss(fails(c));
        }
      }
      k();
    });
  }

  // Over a loop, a counter the loop's condition checks; over a single row,
  // the row or, for LIMIT 0, nothing.
  void produce(const physical::Limit& l, const Consume& k) {
    if (!loops(*l.input)) {
      produce(*l.input, l.n > 0 ? k : Consume{[] {}});
      return;
    }
    w_.line("int64_t n = 0;");
    guard_ = "n < " + std::to_string(l.n) + " && ";
    produce(*l.input, [&] {
      k();
      w_.line("++n;");
    });
  }

  // FOR UPDATE: the row's key is guarded unchanged, so the commit conflicts
  // if a concurrent commit changed the row (docs/transaction_design.md,
  // *Isolation*).
  void produce(const physical::Emit& e, const Consume&) {
    produce(*e.input, [&] {
      if (rows_.size() == 1 && rows_[0].covering) {
        // Every result column from the entry's key: no row read.
        const auto& b = rows_[0];
        if (!e.targets.empty()) w_.line("bc_view key = bc_scan_key(cx, c);");
        w_.line("bc_emit_begin(cx);");
        for (const auto& t : e.targets) {
          w_.line("bc_emit_key_part(cx, " + w_.obj(b.object) + ", key, " + w_.part(b.object, t) + ");");
        }
        w_.line("bc_emit_end(cx);");
        return;
      }
      for (const auto& b : rows_) {
        if (b.for_update) w_.line("bc_guard_row(cx, " + w_.tbl(b.table) + ", " + b.row + ");");
      }
      w_.line("bc_emit_begin(cx);");
      for (const auto& t : e.targets) w_.line("bc_emit_col(cx, " + row_of(t) + ", " + w_.col(t) + ");");
      w_.line("bc_emit_end(cx);");
    });
  }

  void emit_count(const std::string& n) {
    for (const auto& line : count_row(n)) w_.line(line);
  }

  // The SUM or MIN row: the value, or NULL when no row gave one.
  void emit_value_or_null(const std::string& value) {
    w_.line("bc_emit_begin(cx);");
    w_.open("if (seen)");
    w_.line("bc_emit_i64(cx, " + value + ");");
    w_.otherwise();
    w_.line("bc_emit_null(cx);");
    w_.close();
    w_.line("bc_emit_end(cx);");
  }

  // The aggregate's argument in the row in hand: its NULL test and its
  // value, from the entry's key when the scan covers.
  auto arg_of_row(const ColumnRef& arg) -> std::pair<std::string, std::string> {
    const auto& b = binding(arg.table);
    if (b.covering) {
      w_.line("bc_view key = bc_scan_key(cx, c);");
      auto at = w_.obj(b.object) + ", key, " + w_.part(b.object, arg) + ")";
      return {"bc_key_part_null(cx, " + at, "bc_key_part_i64(cx, " + at};
    }
    return {col_null(arg), read_col(arg)};
  }

  // The accumulators an aggregate's loop keeps: n for COUNT, sum or m and
  // whether a value was seen for SUM and MIN, groups for COUNT(DISTINCT).
  void open_aggregate(const Expr& a) {
    if (a.distinct) {
      w_.line("bc_agg groups = bc_agg_open(cx, " + std::to_string(kGroupBytes) + "u, 0);");
      return;
    }
    switch (a.fn) {
      case Expr::Fn::Count: w_.line("int64_t n = 0;"); break;
      case Expr::Fn::Sum: w_.line("int64_t sum = 0;"); break;
      case Expr::Fn::Min: w_.line("int64_t m = 0;"); break;
    }
    if (a.fn != Expr::Fn::Count) w_.line("bool seen = false;");
  }

  // The aggregate's row, after its loop: COUNT(DISTINCT) counts its groups.
  void emit_aggregate(const Expr& a) {
    if (a.distinct) {
      w_.line("int64_t n = 0;");
      w_.line("while (bc_agg_next(cx, groups, 0, 0)) ++n;");
      emit_count("n");
      return;
    }
    switch (a.fn) {
      case Expr::Fn::Count: emit_count("n"); break;
      case Expr::Fn::Sum: emit_value_or_null("sum"); break;
      case Expr::Fn::Min: emit_value_or_null("m"); break;
    }
  }

  // The row in hand folded into the aggregate the loop keeps. NULLs are
  // skipped. COUNT(DISTINCT) groups the value's key, from its row.
  void fold(const Expr& a) {
    if (a.args.empty()) {
      w_.line("++n;");  // COUNT(*)
      return;
    }
    const auto& arg = a.args[0].col;
    auto [is_null, value] = arg_of_row(arg);
    if (a.distinct) {
      auto group = "bc_agg_group(cx, groups, bc_col_key(cx, " + row_of(arg) + ", " + w_.col(arg) + "));";
      w_.line(arg.nullable ? "if (!" + is_null + ") " + group : group);
      return;
    }
    switch (a.fn) {
      case Expr::Fn::Count: w_.line("if (!" + is_null + ") ++n;"); return;
      case Expr::Fn::Sum:
        if (arg.nullable) w_.open("if (!" + is_null + ")");
        w_.line("sum = bc_add(cx, sum, " + value + ");");
        w_.line("seen = true;");
        if (arg.nullable) w_.close();
        return;
      case Expr::Fn::Min:
        if (arg.nullable) w_.open("if (!" + is_null + ")");
        w_.line("int64_t v = " + value + ";");
        w_.line("if (!seen || v < m) m = v;");
        w_.line("seen = true;");
        if (arg.nullable) w_.close();
        return;
    }
  }

  // An aggregate over at most one row, read: its value when `hit` holds,
  // NULL as NULL (COUNT: 1, or 0 for a NULL argument); over none, COUNT 0 and
  // SUM and MIN NULL.
  void emit_one_row(const Expr& a, const std::string& hit) {
    if (a.fn != Expr::Fn::Count) {
      const auto& arg = a.args[0].col;
      w_.line("bc_emit_begin(cx);");
      w_.open("if (" + hit + ")");
      w_.line("bc_emit_col(cx, " + row_of(arg) + ", " + w_.col(arg) + ");");
      w_.otherwise();
      w_.line("bc_emit_null(cx);");
      w_.close();
      w_.line("bc_emit_end(cx);");
      return;
    }
    w_.line("int64_t n = 0;");
    w_.line("if (" + hit + (a.args.empty() ? "" : " && !" + col_null(a.args[0].col)) + ") n = 1;");
    emit_count("n");
  }

  // A pipeline breaker: over a loop, accumulators the loop folds each row
  // into, then the row; over at most one row, no accumulator.
  void produce(const physical::Aggregate& a, const Consume&) {
    const auto* g = std::get_if<physical::PointGet>(&a.input->op);
    if (g != nullptr && a.agg.fn == Expr::Fn::Count && a.agg.args.empty()) {
      // COUNT(*) of one row: whether it exists.
      w_.reads(g->table);
      std::vector<KeySrc> parts;
      for (const auto& e : g->key) parts.push_back({e.param, {}, PhysType::I64});
      auto k = key(w_, g->table, parts);
      w_.line("int64_t n = 0;");
      w_.line("if (bc_exists(cx, " + k + ")) n = 1;");
      if (a.emits) emit_count("n");
      return;
    }
    if (!a.emits) return;
    if (!loops(*a.input)) {
      std::string hit;
      hit_ = &hit;
      produce(*a.input, [&] {
        hit_ = nullptr;
        emit_one_row(a.agg, hit);
      });
      return;
    }
    open_aggregate(a.agg);
    produce(*a.input, [&] { fold(a.agg); });
    emit_aggregate(a.agg);
  }

  void produce(const physical::Insert& s, const Consume&) {
    // bc_row_begin sets every column to its default; a column not given keeps
    // it. The runtime checks the keys and writes the index entries.
    w_.line("bc_row r = bc_row_begin(cx, " + w_.tbl(s.table) + ");");
    for (const auto& [col, e] : s.values) {
      w_.line("bc_row_set_param(cx, r, " + w_.col(col) + ", " + std::to_string(e.param) + ");");
    }
    w_.line("bc_insert(cx, " + w_.tbl(s.table) + ", bc_row_end(cx, r));");
    w_.line("bc_emit_count(cx, 1);");
  }

  // Each row of an UPDATE or DELETE as `old`, `write` printing what is done
  // with it; then the count of rows written. A scan reads the snapshot the
  // statement began on: its own writes never feed back into it.
  void write_rows(std::uint32_t table, const physical::Node& input, const Consume& write) {
    row_name_ = "old";
    w_.reads(table);
    if (!loops(input)) {
      produce(input, [&] {
        write();
        w_.line("bc_emit_count(cx, 1);");
      });
      return;
    }
    w_.line("int64_t n = 0;");
    produce(input, [&] {
      write();
      w_.line("++n;");
    });
    w_.line("bc_emit_count(cx, n);");
  }

  void produce(const physical::Update& s, const Consume&) {
    write_rows(s.table, *s.input, [&] {
      w_.line("bc_row r = bc_row_from(cx, " + w_.tbl(s.table) + ", old);");
      for (const auto& [col, e] : s.set) set(col, e);
      // The runtime moves the index entries whose keys changed.
      w_.line("bc_update(cx, " + w_.tbl(s.table) + ", old, bc_row_end(cx, r));");
    });
  }

  void produce(const physical::Delete& s, const Consume&) {
    write_rows(s.table, *s.input, [&] { w_.line("bc_delete(cx, " + w_.tbl(s.table) + ", old);"); });
  }

  // SET col = e, from the row as read: a parameter as given, NULL included;
  // a column, or column ± a parameter or a constant, NULL if an operand is.
  void set(const ColumnRef& col, const Expr& e) {
    if (e.kind == Expr::Kind::Param) {
      w_.line("bc_row_set_param(cx, r, " + w_.col(col) + ", " + std::to_string(e.param) + ");");
      return;
    }
    auto store = "bc_row_set_" + suffix(col.type) + "(cx, r, " + w_.col(col) + ", ";
    const auto& src = e.kind == Expr::Kind::Column ? e.col : e.args[0].col;
    std::string value, null_if;
    auto null_src = src.nullable ? col_null(src) : "";
    if (e.kind == Expr::Kind::Column) {
      value = read_col(src);
      null_if = null_src;
    } else {
      const auto& rhs = e.args[1];
      auto fn = e.kind == Expr::Kind::Add ? "bc_add" : "bc_sub";
      auto by_const = rhs.kind == Expr::Kind::Const;
      value = std::string{fn} + "(cx, " + read_col(src) + ", " + (by_const ? constant(rhs.constant) : param(PhysType::I64, rhs.param)) + ")";
      null_if = by_const ? null_src : (null_src.empty() ? "" : null_src + " || ") + param_null(rhs.param);
    }
    if (null_if.empty()) {
      w_.line(store + value + ");");
      return;
    }
    w_.open("if (" + null_if + ")");
    w_.line("bc_row_set_null(cx, r, " + w_.col(col) + ");");
    w_.otherwise();
    w_.line(store + value + ");");
    w_.close();
  }

  Writer& w_;
  std::vector<std::string> no_rows_;
  std::string row_name_{"row"};  // the row a leaf reads into: "old" for a write
  std::vector<Binding> rows_;    // the rows in hand, outer first
  bool loop_{false};             // inside a loop: a miss is `continue`
  std::string guard_;            // a LIMIT's condition on the scan loop
  std::string* hit_{nullptr};    // a one-row aggregate: a miss joins this condition instead
};

}  // namespace

auto generate_plan(bound::Bound b, const Catalog& cat, std::string_view sql) -> GeneratedPlan {
  auto* stmt = std::get_if<logical::NodePtr>(&b.stmt);
  if (stmt == nullptr) throw ast::SqlError{"DDL has no plan", -1};
  logical::rewrite(**stmt);
  auto p = search(**stmt, cat);
  p.param_types = b.param_types;
  physical::check(p);
  GeneratedPlan out;
  out.param_types = p.param_types;
  out.catalog_version = p.catalog_version;
  Writer w{p};
  Gen{w}.plan(*p.root);
  std::visit(
      [&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, physical::Insert> || std::is_same_v<T, physical::Update> ||
                      std::is_same_v<T, physical::Delete>) {
          // The runtime maintains the table's indexes: a write plan names none.
          out.tables_written = {op.table};
        } else {
          out.returns_rows = true;
        }
      },
      p.root->op);
  for (const auto& r : p.result) {
    out.result_types.push_back(r.type);
    out.result_names.push_back(r.name);
    out.result_sql.push_back(r.sql);
  }
  out.source = comment_line(sql) +
               comment_line("isolation: snapshot   schema: catalog v" + std::to_string(p.catalog_version)) +
               comment_line("objects: " + w.legend()) + "#include \"bc_plan.h\"\n" + w.constants() +
               "\n\nvoid plan(bc_ctx *cx) {\n" + w.body() + "}\n";
  out.objects_read = w.objects_read();
  out.columns = w.columns_used();
  return out;
}

}  // namespace bytecask::sql
