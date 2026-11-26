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
// Writes are bc_insert (bc_insert_on_conflict for ON CONFLICT DO NOTHING,
// bc_insert_or_existing for DO UPDATE), bc_update and bc_delete: the
// runtime maintains the indexes
// (docs/plan_api_design.md, *Decisions, not mechanics*).

module;
#include <algorithm>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
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
export import bytecask.sql.search;
import bytecask.sql.rewrite;
import bytecask.sql.text;

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
  // it: a column's own, or "count", a bigint; one of result_types each.
  std::vector<std::string> result_names;
  std::vector<SqlType> result_sql;
  std::vector<std::uint32_t> result_pad;  // physical::ResultColumn::pad, each
  // What its bc_schema_ref lists (docs/catalog_design.md,
  // *Compatibility*): the objects and columns it depends on.
  std::uint64_t catalog_version{0};
  std::vector<std::uint32_t> objects_read, tables_written;
  std::vector<ColumnDep> columns;
  double cost{0};  // what the search predicted the plan charges the work budget
  // A cursor over the plan can resume from a checkpoint of its outermost
  // scan instead of replaying from the first row (docs/cursor_design.md,
  // *The replay cursor*). A whitelist: false until a shape is proven.
  bool resumable{false};
  // The plan as a part of a procedure's file (docs/procedures_design.md):
  // its functions, the plan's named as asked and its subqueries' after it,
  // and the constants they use, each with its value.
  std::string functions;
  std::vector<std::pair<std::string, std::uint32_t>> constants;
};

// Plans a bound DML statement, its logical plan rewritten, then searched,
// its tables costed at `counts` and its sorts weighed against the
// statement's memory budget `memory_bytes` (0: none), and prints the
// cheapest plan. Throws FeatureNotSupported ("no plan: ...") when the search
// finds none, and std::logic_error if the physical plan fails
// physical::check, a compiler bug.
// `fn` names the plan's function: plan() for a plan of its own, s<i> for a
// procedure's statement, whose subqueries are then <fn>_sub<k>.
[[nodiscard]] auto generate_plan(bound::Bound b, const Catalog& cat, std::string_view sql, const RowCounts& counts = {},
                                 std::uint64_t memory_bytes = 0, std::string_view fn = "plan") -> GeneratedPlan;

// Every plan the search costs for the statement, cheapest first, each
// printed as generate_plan prints the one it picks (*Tests*, alternatives
// agree); a plan with choice points once per combination of their
// branches, its thresholds overridden so each takes its first plan
// (INT64_MAX) or its second (0), every first first. Throws what
// generate_plan throws.
[[nodiscard]] auto generate_alternatives(bound::Bound b, const Catalog& cat, std::string_view sql,
                                         const RowCounts& counts = {}, std::uint64_t memory_bytes = 0)
    -> std::vector<GeneratedPlan>;

// A procedure's body, bound (docs/procedures_design.md): its expressions are
// over its variables, each a parameter of the procedure's frame, $n the
// variable or constant in slot n.
namespace proc {
struct Stmt;
using Body = std::vector<Stmt>;
// var := value, of the variable's own physical type. An exact number of
// another scale is brought to the variable's: `rescale` digits up (times a
// power of ten) or, when negative, down, rounded half away from zero, as
// PostgreSQL's assignment cast to NUMERIC(p, s) rounds.
struct Assign {
  std::uint32_t var;
  logical::Expr value;
  int rescale{0};
};
struct If {
  std::vector<std::pair<logical::Expr, Body>> branches;  // each a BOOLEAN condition
  Body otherwise;
};
// FOR var IN lower..upper: `range` numbers the loop's state in the frame.
struct ForRange {
  std::uint32_t var;
  std::uint32_t range;
  logical::Expr lower, upper;  // BIGINT, evaluated once, on entry
  bool reverse{false};
  Body body;
};
// WHILE cond LOOP, or LOOP without one.
struct While {
  std::optional<logical::Expr> cond;
  Body body;
};
// EXIT (`exit`) or CONTINUE of the innermost loop, when `cond` holds.
struct Exit {
  bool exit{true};
  std::optional<logical::Expr> cond;
};
// The procedure's statement `stmt`: s<stmt>, between bc_stmt_begin and bc_stmt_end.
struct Exec {
  std::uint32_t stmt;
};
struct Return {};
// FOR targets IN query: statement `stmt` once, then the body per row of it.
struct ForQuery {
  std::uint32_t stmt;
  Body body;
};
struct Stmt {
  std::variant<Assign, If, ForRange, While, Exit, Exec, Return, ForQuery> v;
};
}  // namespace proc

// proc()'s body: control flow, and expressions printed as plans print them,
// reading the frame's slots as parameters. Every loop ends once the
// procedure has failed: each pass is bc_loop or bc_range_next.
[[nodiscard]] auto print_procedure_body(const proc::Body& body) -> std::string;

// The lines a plan rooted at `root` prints for when no row can match: a
// write's count of 0, an aggregate's row over no rows, or none. A test seam:
// it reads `root` unchecked, so a test can hand it what check refuses.
[[nodiscard]] auto no_rows_lines(const physical::Node& root) -> std::vector<std::string>;

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
    case PhysType::Num: return "num";
    case PhysType::Jsonb: return "jsonb";
  }
  return "i64";
}

// The suffix of a key's, a sort key's or a set's call for a value of type t:
// a NUMERIC and a jsonb are carried and emitted, never keyed, compared or
// deduplicated.
auto key_suffix(PhysType t) -> std::string {
  if (!keyable(t)) throw std::logic_error{"physical plan: a NUMERIC or a jsonb in a key, a sort key or a set"};
  return suffix(t);
}

// The suffix of a call that only carries a value of type t, never orders or
// compares it (a column read, a parameter or a row's set): a NUMERIC rides
// as its text, bytes; a jsonb as a bc_jsonb.
auto carry_suffix(PhysType t) -> std::string {
  switch (t) {
    case PhysType::Num: return "bytes";
    case PhysType::I64:
    case PhysType::F64:
    case PhysType::Bool:
    case PhysType::Bytes:
    case PhysType::Jsonb: return suffix(t);
  }
  return suffix(t);
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
  explicit Writer(const physical::Plan& p, std::string sub_prefix = "sub") : p_{p}, sub_prefix_{std::move(sub_prefix)} {}

  // The name of the plan's subquery function `i`.
  [[nodiscard]] auto sub(std::size_t i) const -> std::string { return sub_prefix_ + std::to_string(i); }

  void line(const std::string& s) { body_ += std::string(2 * depth_, ' ') + s + "\n"; }
  void open(const std::string& s) {
    line(s.empty() ? "{" : s + " {");
    ++depth_;
  }
  void close(const std::string& tail = "") {
    --depth_;
    line("}" + tail);
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
    cols_.emplace(storage(c.table), c.col, c.type);
    return use("COL_" + std::to_string(c.table) + "_" + std::to_string(c.col));
  }
  // The position of a column in an index's key, for the key-part decoders.
  auto part(std::uint32_t index, const ColumnRef& c) -> std::string {
    return use("PART_" + std::to_string(index) + "_" + std::to_string(c.col));
  }
  // A table or index the plan reads.
  void reads(std::uint32_t id) { reads_.insert(storage(id)); }
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

  // The constants the plan uses, with their values, in the catalog's order.
  [[nodiscard]] auto constant_values() const -> std::vector<std::pair<std::string, std::uint32_t>> {
    std::vector<std::pair<std::string, std::uint32_t>> out;
    for (const auto& c : p_.constants) {
      if (used_.contains(c.name)) out.emplace_back(c.name, c.value);
    }
    return out;
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

  // The lines another writer printed, indented to this one's depth; what that
  // writer used joins this one's.
  void splice(const Writer& inner) {
    absorb(inner);
    std::string_view text = inner.body_;
    while (!text.empty()) {
      auto end = text.find('\n');
      body_ += std::string(2 * (depth_ - 1), ' ');
      body_ += text.substr(0, end + 1);
      text.remove_prefix(end + 1);
    }
  }

  // What a subquery's writer used, which the statement's plan declares.
  void absorb(const Writer& w) {
    used_.insert(w.used_.begin(), w.used_.end());
    cols_.insert(w.cols_.begin(), w.cols_.end());
    reads_.insert(w.reads_.begin(), w.reads_.end());
  }

private:
  // The table whose rows a table of the plan reads: its own, or, for an
  // instance of it (Table::source), the table it is an instance of. Any other
  // object is itself.
  auto storage(std::uint32_t id) const -> std::uint32_t {
    auto it = std::ranges::find(p_.constants, "TBL_" + std::to_string(id), &Catalog::PlanConstant::name);
    return it == p_.constants.end() ? id : it->value;
  }

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
  std::string sub_prefix_;
  std::string body_;
  std::size_t depth_{1};
  int keys_{0};
  std::set<std::string> used_;
  std::set<std::tuple<std::uint32_t, std::uint32_t, PhysType>> cols_;
  std::set<std::uint32_t> reads_;
};

// A value of type t as C: its zero, and the type of a local that holds it.
auto zero(PhysType t) -> std::string {
  switch (t) {
    case PhysType::I64: return "INT64_C(0)";
    case PhysType::F64: return "0.0";
    case PhysType::Bool: return "false";
    case PhysType::Bytes:
    case PhysType::Num: return "bc_empty(cx)";
    case PhysType::Jsonb: throw std::logic_error{"physical plan: a jsonb has no zero expression: declared()"};
  }
  return "INT64_C(0)";
}


auto c_type(PhysType t) -> std::string {
  switch (t) {
    case PhysType::I64: return "int64_t";
    case PhysType::F64: return "double";
    case PhysType::Bool: return "bool";
    case PhysType::Bytes:
    case PhysType::Num: return "bc_view";
    case PhysType::Jsonb: return "bc_jsonb";
  }
  return "int64_t";
}

// A local of type t named `name`, declared at its zero: a jsonb's handle
// field by field, since a struct initializer compiles to a memset call that a
// plan cannot link.
auto declared(PhysType t, const std::string& name) -> std::string {
  if (t == PhysType::Jsonb) return "bc_jsonb " + name + "; " + name + ".i = 0; " + name + ".g = 0;";
  return c_type(t) + " " + name + " = " + zero(t) + ";";
}

auto param(PhysType t, std::uint32_t n) -> std::string {
  return "bc_param_" + carry_suffix(t) + "(cx, " + std::to_string(n) + ")";
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

// The parts of a key whose every part is a parameter.
auto param_parts(const std::vector<Expr>& params) -> std::vector<KeySrc> {
  std::vector<KeySrc> parts;
  for (const auto& e : params) parts.push_back({e.param, {}, PhysType::I64});
  return parts;
}

// A key of `object` from its leading parts, as an expression. One
// bc_key_params when they are consecutive parameters, a builder otherwise.
// In a loop, `miss_if_long` skips to the next iteration when a byte string
// read from a row makes the key too long for any row to have; `fits`, when
// given, is set to that test instead, for a caller where such a key is a
// miss but the row goes on.
auto key(Writer& w, std::uint32_t object, const std::vector<KeySrc>& parts, bool miss_if_long = false,
         std::string* fits = nullptr) -> std::string {
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
      w.line("bc_key_" + key_suffix(p.type) + "(cx, " + k + ", " + p.value + ");");
    }
  }
  // Only a string read from a row can be past every key: the search keys
  // a join only by a column whose part holds every value of the other
  // (keyable, types_design.md, *Key encoding*), and a parameter or $n ± k
  // is checked against the column's type before the plan runs.
  auto long_value =
      std::ranges::any_of(parts, [](const KeySrc& p) { return !p.value.empty() && p.type == PhysType::Bytes; });
  if (miss_if_long && long_value) w.line("if (!bc_key_fits(cx, " + k + ")) continue;");
  if (fits != nullptr && long_value) *fits = "bc_key_fits(cx, " + k + ")";
  return "bc_key_end(cx, " + k + ")";
}

// The C name of a computed value (physical::Computed), printed before
// anything else.
auto computed(std::size_t i) -> std::string { return "v" + std::to_string(i); }

// Each computed value, once, before anything is read: as PostgreSQL folds
// $n ± k when it binds, one that leaves INTEGER fails the statement whatever
// the other parameters are; a NULL $n gives NULL, never an error. $n is
// checked as an INTEGER before the plan runs (the proof test
// test_a_computed_bound_past_integer_is_22003_before_the_plan_runs), so the
// addition itself cannot overflow int64.
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

// `a op b` as C for values of SQL types `at` and `bt`, of one physical type
// or an integer with a float: text and bytes compare as bytes (COLLATE "C"),
// a float in PostgreSQL's float8 order (-0.0 equal to 0.0, NaN equal to NaN
// and above every other value), an integer beside one cast to double as
// PostgreSQL casts it to float8, the rest as C compares them.
auto compare_text(SqlType at, SqlType bt, Op op, const std::string& a, const std::string& b) -> std::string {
  auto x = phys_type(at);
  auto y = phys_type(bt);
  if (x == PhysType::F64 || y == PhysType::F64) {
    auto as_double = [](PhysType t, const std::string& v) { return t == PhysType::F64 ? v : "(double)(" + v + ")"; };
    return "bc_cmp_f64(" + as_double(x, a) + ", " + as_double(y, b) + ") " + op_text(op) + " 0";
  }
  switch (x) {
    case PhysType::Bytes: return "bc_bytes_cmp(cx, " + a + ", " + b + ") " + op_text(op) + " 0";
    case PhysType::Num: throw std::logic_error{"physical plan: a comparison of a NUMERIC"};
    case PhysType::Jsonb: throw std::logic_error{"physical plan: a comparison of a jsonb"};
    case PhysType::F64:
    case PhysType::I64:
    case PhysType::Bool: break;
  }
  return a + " " + op_text(op) + " " + b;
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

// The row of aggregates over no rows: each COUNT 0, anything else NULL.
auto empty_row(const std::vector<Expr>& outputs) -> std::vector<std::string> {
  std::vector<std::string> out{"bc_emit_begin(cx);"};
  for (const auto& o : outputs) {
    if (o.kind == Expr::Kind::Aggregate && o.fn == Expr::Fn::Count) {
      out.emplace_back("bc_emit_i64(cx, 0);");
      continue;
    }
    out.emplace_back("bc_emit_null(cx);");
  }
  out.emplace_back("bc_emit_end(cx);");
  return out;
}

// Prints a physical plan, operator by operator. produce(n, k) prints n's
// code, and k at the point where each row n emits is in hand: inside the
// loop over a cursor, or once for a single row. `k` is the parent's consume.
class Gen {
public:
  using Consume = std::function<void()>;

  explicit Gen(Writer& w) : w_{w} {}

  // Each uncorrelated subquery first, once, on the statement's snapshot: it
  // sets the parameters the rest reads (bc_sub_kind). A correlated one runs
  // at its conjunct (run_correlated).
  void subqueries() {
    const auto& subs = w_.p().subs;
    for (std::size_t i = 0; i < subs.size(); ++i) {
      if (!subs[i].args.empty()) continue;
      run(i);
    }
  }

  void run(std::size_t i) {
    const auto& s = w_.p().subs[i];
    w_.line("bc_sub_begin(cx, " + std::to_string(i) + ", " + sub_kind(s.kind) + ", " + type_code(s) + ", " +
            std::to_string(s.param) + ");");
    w_.line(w_.sub(i) + "(cx);");
    w_.line("bc_sub_end(cx);");
  }

  // The correlated subqueries conjunct c tests: those whose parameters it
  // reads, or whose set its InSet does.
  auto correlated_in(const Expr& c) const -> std::vector<std::size_t> {
    const auto& subs = w_.p().subs;
    std::vector<std::size_t> out;
    std::function<void(const Expr&)> walk = [&](const Expr& e) {
      for (std::size_t i = 0; i < subs.size(); ++i) {
        if (subs[i].args.empty() || std::ranges::contains(out, i)) continue;
        auto set = e.kind == Expr::Kind::InSet && std::cmp_equal(e.constant, i);
        auto first = subs[i].param;
        auto read = e.kind == Expr::Kind::Param && e.param >= first && e.param < first + logical::Subquery::params(subs[i].kind);
        if (set || read) out.push_back(i);
      }
      for (const auto& a : e.args) walk(a);
    };
    walk(c);
    std::ranges::sort(out);
    return out;
  }

  // Runs correlated subquery i for the row in hand: each argument set from
  // the row's column, then the run, which sets its parameters and set.
  void run_correlated(std::size_t i) {
    for (const auto& a : w_.p().subs[i].args) {
      auto p = std::to_string(a.param);
      auto set = "bc_sub_arg_" + suffix(a.col.type) + "(cx, " + p + ", " + read_col(a.col) + ");";
      if (!a.col.nullable) {
        w_.line(set);
        continue;
      }
      w_.open("if (" + col_null(a.col) + ")");
      w_.line("bc_sub_arg_null(cx, " + p + ");");
      w_.otherwise();
      w_.line(set);
      w_.close();
    }
    run(i);
  }

  void plan(const physical::Node& root) {
    subqueries();
    print_computed(w_);
    // LIMIT 0 returns nothing, so nothing is read: a write of its rows
    // reports it wrote none.
    no_rows_ = no_rows(root);
    if (limit_zero(root)) {
      for (const auto& line : no_rows_) w_.line(line);
      return;
    }
    if (!w_.p().null_checks.empty()) return_if_null(w_, w_.p().null_checks, no_rows_);
    produce(root, [] {});
  }

  // A value in hand, as C: whether it is NULL, and its value (garbage where it is).
  struct Val {
    std::string null, value;
    PhysType type{PhysType::I64};
  };
  using Row = std::vector<Val>;
  // What a set operation does with each row an arm gives, printed where the
  // row is in hand.
  using Sink = std::function<void(Writer&, const Row&)>;

  // An arm of a set operation, a SELECT's plan whose result rows go to
  // `sink` instead. `limit` is a loop condition that stops the arm once the
  // statement's LIMIT is reached, or empty. The arm's misses skip the rest
  // of it instead of ending the plan: where a row is missing, the code after
  // is in an `if`, which the arm closes.
  void arm(const physical::Plan& arm_plan, const std::string& limit, const Sink& sink) {
    sink_ = &sink;
    base_limit_ = limit;
    limit_ = limit;
    const auto& root = *arm_plan.root;
    print_computed(w_);
    if (limit_zero(root)) return;
    if (!arm_plan.null_checks.empty()) {
      std::string cond;
      for (auto n : arm_plan.null_checks) cond += (cond.empty() ? "" : " || ") + param_null(n);
      w_.open("if (!(" + cond + "))");
      ++open_;
    }
    produce(root, [] {});
    for (; open_ > 0; --open_) w_.close();
  }

  static auto sub_kind(logical::Subquery::Kind k) -> std::string {
    switch (k) {
      case logical::Subquery::Kind::Value: return "BC_SUB_VALUE";
      case logical::Subquery::Kind::Exists: return "BC_SUB_EXISTS";
      case logical::Subquery::Kind::Bounds: return "BC_SUB_BOUNDS";
      case logical::Subquery::Kind::Set: return "BC_SUB_SET";
    }
    return "BC_SUB_VALUE";
  }

  // The bc_type of a subquery's column; EXISTS keeps none, so any.
  static auto type_code(const physical::Sub& s) -> std::string {
    if (s.kind == logical::Subquery::Kind::Exists) return "BC_T_BOOL";
    switch (phys_type(s.type)) {
      case PhysType::I64: return "BC_T_I64";
      case PhysType::F64: return "BC_T_F64";
      case PhysType::Bool: return "BC_T_BOOL";
      case PhysType::Bytes: return "BC_T_BYTES";
      case PhysType::Num: return "BC_T_NUM";
      case PhysType::Jsonb: return "BC_T_JSONB";
    }
    return "BC_T_I64";
  }

  // What the root reports when no row can match: nothing for result rows,
  // a count of 0 for a write, the empty row for an aggregate.
  static auto no_rows(const physical::Node& root) -> std::vector<std::string> {
    return std::visit(
        [](const auto& op) -> std::vector<std::string> {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Update> || std::is_same_v<T, physical::Delete> ||
                        std::is_same_v<T, physical::InsertFrom>) {
            return {"bc_emit_count(cx, 0);"};
          } else if constexpr (std::is_same_v<T, physical::Aggregate>) {
            return op.emits ? empty_row({op.agg}) : std::vector<std::string>{};
          } else if constexpr (std::is_same_v<T, physical::CountRange>) {
            return op.emits ? count_row("0") : std::vector<std::string>{};
          } else if constexpr (std::is_same_v<T, physical::KeyAggregates>) {
            return op.emits ? empty_row(op.outputs) : std::vector<std::string>{};
          } else if constexpr (std::is_same_v<T, physical::GroupEmit>) {
            return empty_group(op);
          } else {
            return {};
          }
        },
        root.op);
  }

  // An expression over parameters alone, as a procedure's body evaluates it.
  auto value_of(const Expr& e) -> Val { return eval(e); }

private:
  // A row in hand: its variable, or, for a covering scan, the entry its
  // cursor is on.
  struct Binding {
    std::uint32_t table;
    std::string row;
    bool covering{false};
    std::uint32_t object{0};
    bool for_update{false};
    std::string cursor{"c"};  // a covering scan's
    bool looped{false};       // bound by a loop: a scan's, or a hash join's over the rows it holds
    bool derived{false};      // a derived relation's group: its columns are the locals <row>_v<col>, NULL while <row>_n<col>
  };

  // Whether the root is under LIMIT 0: rows or groups emitted under it, or
  // an aggregate's row that is not emitted.
  static auto limit_zero(const physical::Node& root) -> bool {
    return std::visit(
        [](const auto& op) -> bool {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Emit> || std::is_same_v<T, physical::GroupEmit>) {
            const auto* l = std::get_if<physical::Limit>(&op.input->op);
            return l != nullptr && l->n <= l->offset;
          } else if constexpr (std::is_same_v<T, physical::Aggregate> || std::is_same_v<T, physical::CountRange> ||
                               std::is_same_v<T, physical::KeyAggregates>) {
            return !op.emits;
          } else if constexpr (std::is_same_v<T, physical::InsertFrom>) {
            return limit_zero(*op.input);
          } else {
            return false;
          }
        },
        root.op);
  }

  // Whether n's rows come from a loop, or are at most one, read once.
  static auto loops(const physical::Node& n) -> bool {
    return std::visit(
        [](const auto& op) -> bool {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Scan> || std::is_same_v<T, physical::NullSplitScan> ||
                        std::is_same_v<T, physical::Sort>) {
            return true;
          } else if constexpr (std::is_same_v<T, physical::GroupAggregate>) {
            // A structure's groups are a loop; streamed, the input's.
            return !op.streamed || loops(*op.input);
          } else if constexpr (std::is_same_v<T, physical::IndexJoin>) {
            return loops(*op.outer);
          } else if constexpr (std::is_same_v<T, physical::NestedLoop> || std::is_same_v<T, physical::HashJoin> ||
                               std::is_same_v<T, physical::Choice> || std::is_same_v<T, physical::Derived>) {
            return true;
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

  // A column of the row in hand, or of the entry a covering scan's cursor is
  // on: never a DOUBLE (physical::check).
  auto read_col(const ColumnRef& c) -> std::string {
    const auto& b = binding(c.table);
    if (b.derived) return b.row + "_v" + std::to_string(c.col);
    if (b.covering) return "bc_key_part_" + key_suffix(c.type) + "(cx, " + key_part(b, c);
    return "bc_col_" + carry_suffix(c.type) + "(cx, " + row_of(c) + ", " + w_.col(c) + ")";
  }
  auto col_null(const ColumnRef& c) -> std::string {
    const auto& b = binding(c.table);
    if (b.derived) return b.row + "_n" + std::to_string(c.col);
    if (b.covering) return "bc_key_part_null(cx, " + key_part(b, c);
    return "bc_col_null(cx, " + row_of(c) + ", " + w_.col(c) + ")";
  }
  // "<object>, bc_scan_key(cx, c), <part>)": the arguments that name c's part
  // of the entry the covering scan is on.
  auto key_part(const Binding& b, const ColumnRef& c) -> std::string {
    const auto key = &b == keyed_ ? std::string{"key"} : "bc_scan_key(cx, " + b.cursor + ")";
    return w_.obj(b.object) + ", " + key + ", " + w_.part(b.object, c) + ")";
  }

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
    if (sink_ != nullptr) {
      w_.open("if (!(" + cond + "))");
      ++open_;
      return;
    }
    w_.open("if (" + cond + ")");
    for (const auto& line : no_rows_) w_.line(line);
    w_.line("return;");
    w_.close();
  }

  // Whether the row fails the conjunct, as a C expression: NULL on either
  // side, or the comparison false; a predicate, unless it is true.
  auto fails(const Expr& c) -> std::string {
    if (c.kind == Expr::Kind::Const) return c.constant != 0 ? "0" : "1";  // folded: never, or always
    if (logical::is_predicate(c)) return "!(" + truth(c, true) + ")";
    auto [null, cmp] = comparison(c);
    return null + " || !(" + cmp + ")";
  }

  // Whether predicate e is true (`want`) or false, as C: SQL's three-valued
  // logic as two conditions, unknown being neither. NOT swaps them; AND is
  // true when all are and false when one is, OR the mirror. Each node is
  // printed once, in one of the two, and short-circuits.
  auto truth(const Expr& e, bool want) -> std::string {
    switch (e.kind) {
      case Expr::Kind::Not: return truth(e.args[0], !want);
      case Expr::Kind::And:
      case Expr::Kind::Or: {
        auto all = (e.kind == Expr::Kind::And) == want;
        std::string out;
        for (const auto& a : e.args) out += (out.empty() ? "(" : all ? " && (" : " || (") + truth(a, want) + ")";
        return out;
      }
      case Expr::Kind::IsNull: return (want ? "" : "!") + col_null(e.args[0].col);  // never unknown
      case Expr::Kind::Like: {
        // ESCAPE $n is checked, and its code point taken, at each row: a
        // parameter's value is known only when the plan runs.
        const auto& col = e.args[0].col;
        auto p = e.args[1].param;
        auto null = col_null(col) + " || " + param_null(p);
        auto escape = std::to_string(e.constant);
        if (e.args.size() == 3) {
          null += " || " + param_null(e.args[2].param);
          escape = "bc_like_escape(cx, " + param(PhysType::Bytes, e.args[2].param) + ")";
        }
        auto match = std::string{e.fold ? "bc_ilike" : "bc_like"} + "(cx, " + read_col(col) + ", " + param(PhysType::Bytes, p) + ", " +
                     escape + ")";
        return "!(" + null + ") && " + (want ? "" : "!") + match;
      }
      case Expr::Kind::Const: return (e.constant != 0) == want ? "1" : "0";
      case Expr::Kind::Compare: {
        auto [null, cmp] = comparison(e);
        return "!(" + null + ") && " + (want ? "(" : "!(") + cmp + ")";
      }
      case Expr::Kind::Truth: {  // a subquery's BOOLEAN parameter
        auto p = e.args[0].param;
        return "!" + param_null(p) + " && " + (want ? "" : "!") + param(PhysType::Bool, p);
      }
      case Expr::Kind::InSet: {  // the column's value among a subquery's
        const auto& col = e.args[0].col;
        auto has = "bc_sub_has_" + key_suffix(col.type) + "(cx, " + std::to_string(e.constant) + ", " + read_col(col) + ")";
        return "!(" + col_null(col) + ") && " + (want ? "" : "!") + has;
      }
      case Expr::Kind::Column:
      case Expr::Kind::Param:
      case Expr::Kind::Add:
      case Expr::Kind::Sub:
      case Expr::Kind::Mul:
      case Expr::Kind::Div:
      case Expr::Kind::Case:
      case Expr::Kind::Coalesce:
      case Expr::Kind::Concat:
      case Expr::Kind::Substr:
      case Expr::Kind::TextFn:
      case Expr::Kind::Extract:
      case Expr::Kind::Cast:
      case Expr::Kind::Jsonb:
      case Expr::Kind::GroupKey:
      case Expr::Kind::Mod:
      case Expr::Kind::Neg:
      case Expr::Kind::Abs:
      case Expr::Kind::Random:
      case Expr::Kind::Aggregate: break;
    }
    throw std::logic_error{"physical plan: a predicate codegen does not print"};
  }

  // A comparison's NULL test (either side NULL) and the comparison, as C.
  auto comparison(const Expr& c) -> std::pair<std::string, std::string> {
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
      case Expr::Kind::Mul:
      case Expr::Kind::Div:
      case Expr::Kind::Compare:
      case Expr::Kind::Aggregate:
      case Expr::Kind::And:
      case Expr::Kind::Or:
      case Expr::Kind::Not:
      case Expr::Kind::IsNull:
      case Expr::Kind::Like:
      case Expr::Kind::Truth:
      case Expr::Kind::InSet:
      case Expr::Kind::Case:
      case Expr::Kind::Coalesce:
      case Expr::Kind::Concat:
      case Expr::Kind::Substr:
      case Expr::Kind::TextFn:
      case Expr::Kind::Extract:
      case Expr::Kind::Cast:
      case Expr::Kind::Jsonb:
      case Expr::Kind::GroupKey:
      case Expr::Kind::Mod:
      case Expr::Kind::Neg:
      case Expr::Kind::Abs:
      case Expr::Kind::Random: throw std::logic_error{"physical plan: a filter operand codegen does not print"};
    }
    auto col = read_col(lhs);
    return {out, compare_text(c.args[0].type, rhs.type, c.op, col, value)};
  }

  void produce(const physical::Node& n, const Consume& k) {
    std::visit([&](const auto& op) { produce(op, k); }, n.op);
  }

  // No rows: nothing is read, and the root reports none.
  void produce(const physical::Empty&, const Consume&) {
    for (const auto& line : no_rows_) w_.line(line);
  }

  // One row of no columns: the consumer, once.
  void produce(const physical::OneRow&, const Consume& k) { k(); }

  // The single row a point lookup names. In a one-row aggregate a missing
  // row is a condition, `hit_`, not an end.
  void produce(const physical::PointGet& g, const Consume& k) {
    w_.reads(g.table);
    w_.line("bool found = false;");
    w_.line("bc_view " + row_name_ + " = bc_get(cx, " + key(w_, g.table, param_parts(g.key)) + ", &found);");
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

  static auto prefix(const physical::Scan& sc) -> std::vector<KeySrc> { return param_parts(sc.prefix); }

  // `bc_view <lo>` and `bc_view <hi>` of a scan's range.
  void print_range(const physical::Scan& sc, const physical::Range& r, const std::string& lo = "lo",
                   const std::string& hi = "hi") {
    w_.line("bc_view " + lo + " = " + bound(sc, r.lo, true) + ";");
    w_.line("bc_view " + hi + " = " + bound(sc, r.hi, false) + ";");
  }

  // `bc_cursor c` over every entry under the key `lo` holds.
  void open_prefix(const physical::Scan& sc, const std::string& lo = "lo") {
    w_.line("bc_cursor " + cursor_ + " = bc_scan_open(cx, " + w_.obj(sc.object) + ", " + lo + ", " + lo +
            ", BC_SCAN_PREFIX" + (sc.reverse ? " | BC_SCAN_REVERSE" : "") + ");");
  }

  // A choice point's counted range, as the names of its bounds.
  using Counted = std::pair<std::string, std::string>;

  // `bc_view lo`, `bc_view hi` and `bc_cursor c` of the scan; with `counted`,
  // a choice point's first plan, `c` alone, on the range its count built.
  void open_scan(const physical::Scan& sc, const Counted* counted = nullptr) {
    w_.reads(sc.object);
    auto lo = counted != nullptr ? counted->first : std::string{"lo"};
    auto hi = counted != nullptr ? counted->second : std::string{"hi"};
    if (const auto* r = std::get_if<physical::Range>(&sc.rest)) {
      if (!counted) print_range(sc, *r);
      w_.line("bc_cursor " + cursor_ + " = bc_scan_open(cx, " + w_.obj(sc.object) + ", " + lo + ", " + hi + ", " +
              (sc.reverse ? "BC_SCAN_REVERSE" : "BC_SCAN_FORWARD") + ");");
      return;
    }
    if (!counted) w_.line("bc_view lo = " + key(w_, sc.object, prefix(sc)) + ";");
    open_prefix(sc, lo);
  }

  // The NULL part of a NullSplitScan: every entry under the prefix and NULL.
  void open_null_part(const physical::Scan& sc) {
    auto parts = prefix(sc);
    parts.push_back({});
    w_.line("bc_view lo = " + key(w_, sc.object, parts) + ";");
    open_prefix(sc);
  }

  // The loop over the open cursor `c`: each entry's row read unless the scan
  // covers, then k. A LIMIT's counter stops it before the next step, so the
  // loop can end on an entry: the cursor is closed right after it, which
  // ends that step (docs/plan_api_design.md, *A loop's step owns what it
  // allocates*).
  void scan_loop(const physical::Scan& sc, const Consume& k) {
    w_.open("while (" + guard() + "bc_scan_next(cx, " + cursor_ + "))");
    if (!sc.covering) {
      w_.reads(sc.table);
      w_.line("bc_view " + row_name_ + " = bc_scan_row(cx, " + cursor_ + ");");
    }
    rows_.push_back({sc.table, row_name_, sc.covering, sc.object, sc.for_update, cursor_, true});
    auto outer = loop_;
    loop_ = true;
    k();
    loop_ = outer;
    rows_.pop_back();
    w_.close();
    if (!limit_.empty()) w_.line("bc_scan_close(cx, " + cursor_ + ");");
  }

  void produce(const physical::Scan& sc, const Consume& k) {
    auto it = counted_.find(&sc);
    open_scan(sc, it == counted_.end() ? nullptr : &it->second);
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

  // `bc_view <lo>` and `bc_view <hi>` of the scan's whole range, for a count.
  void print_count_range(const physical::Scan& sc, const std::string& lo = "lo", const std::string& hi = "hi") {
    w_.reads(sc.object);
    if (const auto* r = std::get_if<physical::Range>(&sc.rest)) {
      print_range(sc, *r, lo, hi);
    } else {
      w_.line("bc_view " + lo + " = " + key(w_, sc.object, prefix(sc)) + ";");
      w_.line("bc_view " + hi + " = bc_key_successor(cx, " + lo + ");");
    }
  }

  // Every entry of the range is one row to count.
  void produce(const physical::CountRange& cr, const Consume&) {
    const auto& sc = cr.scan;
    print_count_range(sc);
    w_.line("int64_t n = bc_count_range(cx, " + w_.obj(sc.object) + ", lo, hi, INT64_MAX);");
    if (cr.emits) emit_count("n");
  }

  // The scan of the table a choice point counts that a plan's rows come from,
  // under its filters and sort, or a hash join's build side, or a join whose
  // outer side a hash join is; null for a NULL split.
  static auto counted_scan(const physical::Node& n, const physical::Scan& counted) -> const physical::Scan* {
    if (const auto* sc = std::get_if<physical::Scan>(&n.op)) return sc->table == counted.table ? sc : nullptr;
    if (const auto* f = std::get_if<physical::Filter>(&n.op)) return counted_scan(*f->input, counted);
    if (const auto* o = std::get_if<physical::Sort>(&n.op)) return counted_scan(*o->input, counted);
    if (const auto* g = std::get_if<physical::GroupAggregate>(&n.op)) return counted_scan(*g->input, counted);
    if (const auto* j = std::get_if<physical::IndexJoin>(&n.op)) return counted_scan(*j->outer, counted);
    if (const auto* j = std::get_if<physical::NestedLoop>(&n.op)) return counted_scan(*j->outer, counted);
    if (const auto* h = std::get_if<physical::HashJoin>(&n.op)) return counted_scan(*h->build, counted);
    return nullptr;
  }

  // The count of the first plan's range, limited to the threshold; then
  // each plan, under the condition that picks it, with the consume printed
  // once in each. The first plan's scan opens on the range the count built.
  void produce(const physical::Choice& ch, const Consume& k) {
    const auto i = std::to_string(choices_++);
    const auto n = "count" + i;
    const auto t = ch.threshold == std::numeric_limits<std::int64_t>::max() ? std::string{"INT64_MAX"}
                                                                             : std::to_string(ch.threshold);
    print_count_range(ch.counted, "lo" + i, "hi" + i);
    w_.line("int64_t " + n + " = bc_count_range(cx, " + w_.obj(ch.counted.object) + ", lo" + i + ", hi" + i + ", " + t +
            ");");
    w_.open("if (" + n + " < " + t + ")");
    // A NULL split's ranges are its own, not the count of its prefix.
    if (const auto* first = counted_scan(*ch.below, ch.counted)) counted_.emplace(first, Counted{"lo" + i, "hi" + i});
    produce(*ch.below, k);
    w_.otherwise();
    produce(*ch.at_or_above, k);
    w_.close();
  }

  // Aggregates from the keys: COUNT(*) counts the range, and each MIN or
  // MAX, once, reads the first entry of its range past the part's NULLs,
  // forward or in reverse, into kv<i>, NULL while kn<i>: one step, never the
  // scan's end, in a block of its own, the cursor closed after it. Then the
  // row is emitted.
  void produce(const physical::KeyAggregates& m, const Consume&) {
    if (!m.emits) return;
    if (std::ranges::any_of(m.outputs, [](const Expr& o) { return o.fn == Expr::Fn::Count; })) {
      print_count_range(m.counted);
      w_.line("int64_t n = bc_count_range(cx, " + w_.obj(m.counted.object) + ", lo, hi, INT64_MAX);");
    }
    // Each MIN or MAX once: its index in `read`.
    std::vector<Expr::Fn> read;
    for (const auto& o : m.outputs) {
      if (o.fn == Expr::Fn::Count || std::ranges::find(read, o.fn) != read.end()) continue;
      const auto i = std::to_string(read.size());
      read.push_back(o.fn);
      w_.line("int64_t kv" + i + " = 0;");
      w_.line("bool kn" + i + " = true;");
      w_.open("");
      auto sc = m.values;
      sc.reverse = o.fn == Expr::Fn::Max;
      open_scan(sc);
      w_.open("if (bc_scan_next(cx, " + cursor_ + "))");
      if (sc.covering) {
        w_.line("kv" + i + " = bc_key_part_i64(cx, " + w_.obj(sc.object) + ", bc_scan_key(cx, " + cursor_ + "), " +
                w_.part(sc.object, *m.arg) + ");");
      } else {
        w_.reads(sc.table);
        w_.line("kv" + i + " = bc_col_i64(cx, bc_scan_row(cx, " + cursor_ + "), " + w_.col(*m.arg) + ");");
      }
      w_.line("kn" + i + " = false;");
      w_.close();
      w_.line("bc_scan_close(cx, " + cursor_ + ");");
      w_.close();
    }
    w_.line("bc_emit_begin(cx);");
    for (const auto& o : m.outputs) {
      if (o.fn == Expr::Fn::Count) {
        w_.line("bc_emit_i64(cx, n);");
        continue;
      }
      const auto i = std::to_string(std::ranges::find(read, o.fn) - read.begin());
      w_.open("if (kn" + i + ")");
      w_.line("bc_emit_null(cx);");
      w_.otherwise();
      w_.line("bc_emit_i64(cx, kv" + i + ");");
      w_.close();
    }
    w_.line("bc_emit_end(cx);");
  }

  // For each outer row, the inner row its key names. Of an outer column, a
  // NULL names no row, nor does a byte string too long for any key. The
  // inner row of the d-th join from the first table is row<d>, found<d>.
  // A left outer join's row of the inner table's NULLs, nr<i>, made once
  // before its outer side's loop.
  auto null_row(std::uint32_t table) -> std::string {
    auto n = "nr" + std::to_string(nulls_++);
    w_.reads(table);
    w_.line("bc_view " + n + " = bc_null_row(cx, " + w_.tbl(table) + ");");
    return n;
  }

  // The table a nested loop's inner side reads.
  static auto inner_table(const physical::Node& n) -> std::uint32_t {
    return std::visit(
        [](const auto& op) -> std::uint32_t {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Scan> || std::is_same_v<T, physical::PointGet>) {
            return op.table;
          } else if constexpr (std::is_same_v<T, physical::NullSplitScan>) {
            return op.present.table;
          } else if constexpr (std::is_same_v<T, physical::Choice>) {
            return inner_table(*op.below);
          } else if constexpr (requires { op.input; }) {
            return inner_table(*op.input);
          } else {
            throw std::logic_error{"physical plan: a nested loop's inner side reads no one table"};
          }
        },
        n.op);
  }

  // The tables a left outer nested loop's inner side NULL-extends, outer
  // first: its one table, or each of a group's, inner nested loops of each
  // other.
  static auto inner_tables(const physical::Node& n) -> std::vector<std::uint32_t> {
    const auto* j = std::get_if<physical::NestedLoop>(&n.op);
    if (j == nullptr) return {inner_table(n)};
    auto out = inner_tables(*j->outer);
    out.push_back(inner_table(*j->inner));
    return out;
  }

  // Left outer: the inner row where its key has no NULL part, the table
  // holds it and every match conjunct holds of the pair; else the null row.
  // The outer row is never dropped.
  void outer_index_join(const physical::IndexJoin& j, const Consume& k) {
    const auto nr = null_row(j.inner);
    produce(*j.outer, [&] {
      const auto d = std::to_string(rows_.size());
      const auto row = "row" + d;
      const auto found = "found" + d;
      std::vector<KeySrc> parts;
      std::string nulls;
      for (const auto& e : j.inner_key) {
        if (e.kind == Expr::Kind::Param) {
          parts.push_back({e.param, {}, PhysType::I64});
          nulls += (nulls.empty() ? "" : " || ") + param_null(e.param);
          continue;
        }
        if (e.col.nullable) nulls += (nulls.empty() ? "" : " || ") + col_null(e.col);
        parts.push_back({std::nullopt, read_col(e.col), e.col.type});
      }
      w_.reads(j.inner);
      w_.line("bool " + found + " = false;");
      w_.line("bc_view " + row + " = bc_empty(cx);");
      // A NULL part, or a value too long for any key, names no row: the key
      // is built only without one, and the outer row goes on either way.
      if (!nulls.empty()) w_.open("if (!(" + nulls + "))");
      std::string fits;
      auto inner_key = key(w_, j.inner, parts, false, &fits);
      w_.line((fits.empty() ? "" : "if (" + fits + ") ") + row + " = bc_get(cx, " + inner_key + ", &" + found + ");");
      if (!nulls.empty()) w_.close();
      rows_.push_back({j.inner, row});
      auto matched = found;
      if (std::ranges::any_of(j.match, [](const Expr& c) { return physical::evaluated(c); })) {
        // A block per conjunct: no conjunct's code runs for a row the key
        // did not find or an earlier conjunct rejected.
        matched = "jm" + d;
        w_.line("bool " + matched + " = " + found + ";");
        for (const auto& c : j.match) match_block(matched, c);
      } else {
        for (const auto& c : j.match) matched += " && !(" + fails(c) + ")";
      }
      w_.line("if (!(" + matched + ")) " + row + " = " + nr + ";");
      k();
      rows_.pop_back();
    });
  }

  // `flag &&= conjunct c holds`, c's code run only while `flag` is set.
  void match_block(const std::string& flag, const Expr& c) {
    if (!physical::evaluated(c)) {
      w_.line("if (" + flag + ") " + flag + " = !(" + fails(c) + ");");
      return;
    }
    w_.open("if (" + flag + ")");
    auto v = eval(c);
    w_.line(flag + " = !(" + rejects(v) + ");");
    w_.close();
  }

  void produce(const physical::IndexJoin& j, const Consume& k) {
    if (j.left_outer) {
      outer_index_join(j, k);
      return;
    }
    produce(*j.outer, [&] {
      const auto d = std::to_string(rows_.size());
      const auto row = "row" + d;
      const auto found = "found" + d;
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
      w_.line("bool " + found + " = false;");
      auto inner_key = key(w_, j.inner, parts, true);
      if (hit_) {
        // No row without the rows before it: the inner one is not read.
        std::string before = "found";
        for (std::size_t i = 1; i < rows_.size(); ++i) before += " && found" + std::to_string(i);
        w_.line("bc_view " + row + " = bc_empty(cx);");
        w_.line("if (" + before + ") " + row + " = bc_get(cx, " + inner_key + ", &" + found + ");");
        *hit_ += " && " + found;
      } else {
        w_.line("bc_view " + row + " = bc_get(cx, " + inner_key + ", &" + found + ");");
        miss("!" + found);
      }
      rows_.push_back({j.inner, row});
      k();
      rows_.pop_back();
    });
  }

  // The outer side's row, then, if it has one, the inner scan's loop: its
  // row is row<d>, as an index join's. Inside an outer loop, its cursor is
  // c<d>, so the outer one's stays in reach.
  // Left outer: m<d> says whether the inner side gave a row for the outer
  // one, its filters the join's ON, set by a group's innermost loop; where it
  // gave none, or its guard's parameters hold a NULL, the outer row once
  // more, joined to the null row of each table of the inner side.
  void produce(const physical::NestedLoop& j, const Consume& k) {
    const auto outer_loops = loops(*j.outer);
    std::vector<std::pair<std::uint32_t, std::string>> nulls;
    if (j.left_outer) {
      for (auto t : inner_tables(*j.inner)) nulls.emplace_back(t, null_row(t));
    }
    produce(*j.outer, [&] {
      const auto d = std::to_string(rows_.size());
      auto outer_row = std::exchange(row_name_, "row" + d);
      auto outer_cursor = outer_loops ? std::exchange(cursor_, "c" + d) : cursor_;
      if (!j.left_outer) {
        produce(*j.inner, k);
        row_name_ = outer_row;
        cursor_ = outer_cursor;
        return;
      }
      const auto m = "m" + d;
      w_.line("bool " + m + " = false;");
      std::string guard;
      for (auto p : j.guard) guard += (guard.empty() ? "" : " || ") + param_null(p);
      if (!guard.empty()) w_.open("if (!(" + guard + "))");
      produce(*j.inner, [&] {
        w_.line(m + " = true;");
        k();
      });
      if (!guard.empty()) w_.close();
      row_name_ = outer_row;
      cursor_ = outer_cursor;
      w_.open("if (!" + m + ")");
      for (const auto& [t, nr] : nulls) rows_.push_back({t, nr});
      k();
      rows_.resize(rows_.size() - nulls.size());
      w_.close();
    });
  }

  // A hash join's key for the row in hand, `hk<i>`, from each column's value:
  // after a NULL in any of them, the next row, since `=` never holds for NULL;
  // a column a sort below tested already is not tested again.
  // A value is a key part of its physical type, so equal values give equal
  // bytes on both sides, read from a row or from a covering scan's entry.
  auto tested(const ColumnRef& c) const -> bool { return std::ranges::find(not_null_, c) != not_null_.end(); }
  auto hash_key(const std::string& i, const std::vector<ColumnRef>& cols) -> std::string {
    std::vector<ColumnRef> checked;
    for (const auto& c : cols) {
      if (!c.nullable || tested(c) || std::ranges::find(checked, c) != checked.end()) continue;
      miss(col_null(c));
      checked.push_back(c);
    }
    const auto k = "hk" + i;
    w_.line("bc_sort_key " + k + " = bc_sort_key_begin(cx);");
    for (const auto& c : cols) {
      w_.line("bc_sort_key_" + key_suffix(c.type) + "(cx, " + k + ", " + read_col(c) + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
    }
    return "bc_sort_key_end(cx, " + k + ")";
  }

  // A pipeline breaker: the build side's loop, in a block of its own, puts
  // each row under its key in hash table h<i>; then, for each row of the probe
  // side, a loop over the rows put under its key, the held one bound as b<i>.
  // A LIMIT's counter is the probe side's and that loop's, never the build's.
  void produce(const physical::HashJoin& h, const Consume& k) {
    const auto i = std::to_string(hashes_++);
    const auto table = "h" + i;
    const auto held = "b" + i;
    std::vector<ColumnRef> build_cols, probe_cols;
    for (const auto& key : h.keys) {
      build_cols.push_back(key.build);
      probe_cols.push_back(key.probe);
    }
    // The build side is named in the plan text.
    w_.line("bc_hash " + table + " = bc_hash_open(cx);  // built on " + w_.tbl(h.held) + "'s rows");
    auto limit = std::exchange(limit_, "");
    w_.open("");
    produce(*h.build, [&] {
      auto key = hash_key(i, build_cols);
      w_.line("bc_hash_put(cx, " + table + ", " + key + ", " + binding(h.held).row + ");");
    });
    w_.close();
    limit_ = limit;
    produce(*h.probe, [&] {
      auto key = hash_key(i, probe_cols);
      const auto probe = "q" + i;
      w_.line("bc_hash_iter " + probe + " = bc_hash_find(cx, " + table + ", " + key + ");");
      w_.line("bc_view " + held + " = bc_empty(cx);");
      w_.open("while (" + guard() + "bc_hash_next(cx, " + probe + ", &" + held + "))");
      rows_.push_back({h.held, held, false, 0, false, "c", true});
      auto outer = std::exchange(loop_, true);
      k();
      loop_ = outer;
      rows_.pop_back();
      w_.close();
    });
  }

  // A pipeline breaker: each row of its input added to sorter s<i>, keyed by
  // bc_sort_key_* from the rows in hand, then a loop over the sorter in sort
  // order, which binds the row it held as p<i>. A sort of several loops has
  // a sorter per loop whose row is read above it, `payloads`, each given the
  // same key and its loop's row, numbered on from i: the loop steps the
  // first by bc_sorter_next and reads each other's entry at the same place,
  // r<j>, by bc_sorter_nth. With none, one sorter holds the keys alone. A
  // row NULL in a `not_null` column is not added. The rows read once before
  // the input's first loop stay in hand; a LIMIT's counter is the sorter
  // loop's.
  void produce(const physical::Sort& s, const Consume& k) {
    if (s.groups) {
      sort_groups(s, k);
      return;
    }
    std::vector<std::string> ids;
    for ([[maybe_unused]] auto t : s.payloads) ids.push_back(std::to_string(sorts_++));
    if (ids.empty()) ids.push_back(std::to_string(sorts_++));
    for (const auto& i : ids) {
      w_.line("bc_sorter s" + i + " = bc_sorter_open(cx, " + std::to_string(s.keep) + ");");
    }
    auto limit = std::exchange(limit_, "");
    std::vector<Binding> in_hand;
    produce(*s.input, [&] {
      std::vector<ColumnRef> checked;
      for (const auto& c : s.not_null) {
        if (!c.nullable || tested(c) || std::ranges::find(checked, c) != checked.end()) continue;
        miss(col_null(c));
        checked.push_back(c);
      }
      w_.line("bc_sort_key sk = bc_sort_key_begin(cx);");
      for (const auto& part : s.keys) sort_part(part);
      auto key = std::string{"bc_sort_key_end(cx, sk)"};
      if (ids.size() > 1) {
        w_.line("bc_view skv = " + key + ";");
        key = "skv";
      }
      for (std::size_t j = 0; j < ids.size(); ++j) {
        const auto payload = s.payloads.empty() ? std::string{"bc_empty(cx)"} : binding(s.payloads[j]).row;
        w_.line("bc_sorter_add(cx, s" + ids[j] + ", " + key + ", " + payload + ");");
      }
      // The rows in hand before the first loop, and each loop's whose row a
      // sorter holds.
      in_hand.clear();
      auto looped = false;
      for (const auto& b : rows_) {
        looped = looped || b.looped;
        if (!looped || std::ranges::find(s.payloads, b.table) != s.payloads.end()) in_hand.push_back(b);
      }
    });
    limit_ = limit;
    for (auto& b : in_hand) {
      auto at = std::ranges::find(s.payloads, b.table);
      if (at != s.payloads.end()) b.row = "p" + ids[static_cast<std::size_t>(at - s.payloads.begin())];
    }
    w_.line("bc_view p" + ids[0] + " = bc_empty(cx);");
    for (std::size_t j = 1; j < ids.size(); ++j) w_.line("int64_t r" + ids[j] + " = 0;");
    w_.open("while (" + guard() + "bc_sorter_next(cx, s" + ids[0] + ", &p" + ids[0] + "))");
    for (std::size_t j = 1; j < ids.size(); ++j) {
      w_.line("bc_view p" + ids[j] + " = bc_sorter_nth(cx, s" + ids[j] + ", r" + ids[j] + ");");
      w_.line("++r" + ids[j] + ";");
    }
    auto outer_rows = std::exchange(rows_, in_hand);
    auto outer = std::exchange(loop_, true);
    const auto tested_before = not_null_.size();
    not_null_.insert(not_null_.end(), s.not_null.begin(), s.not_null.end());
    k();
    not_null_.resize(tested_before);
    loop_ = outer;
    rows_ = outer_rows;
    w_.close();
  }

  // One part of sort key `sk`: a row's column, NULL included, or a covering
  // scan's entry's, never a DOUBLE (physical::check); or an expression,
  // evaluated over the rows in hand into bc_sort_key_<type>, an error it
  // raises (22003, 22012) the statement's.
  void sort_part(const physical::SortPart& part) {
    const auto placed = std::string{part.desc ? "BC_SORT_DESC" : "BC_SORT_ASC"} + ", " +
                        (part.nulls_first ? "BC_SORT_NULLS_FIRST" : "BC_SORT_NULLS_LAST") + ");";
    std::string is_null;
    std::string value;
    if (const auto* x = std::get_if<physical::ExprTarget>(&part.key)) {
      auto v = eval(x->expr);
      is_null = v.null;
      value = "bc_sort_key_" + key_suffix(v.type) + "(cx, sk, " + v.value + ", " + placed;
    } else {
      const auto& c = std::get<ColumnRef>(part.key);
      if (!binding(c.table).covering) {
        w_.line("bc_sort_key_col(cx, sk, " + row_of(c) + ", " + w_.col(c) + ", " + placed);
        return;
      }
      is_null = c.nullable ? col_null(c) : "";
      value = "bc_sort_key_" + key_suffix(c.type) + "(cx, sk, " + read_col(c) + ", " + placed;
    }
    if (is_null.empty()) {
      w_.line(value);
      return;
    }
    w_.open("if (" + is_null + ")");
    w_.line("bc_sort_key_null(cx, sk, " + placed);
    w_.otherwise();
    w_.line(value);
    w_.close();
  }

  void produce(const physical::Filter& f, const Consume& k) {
    if (physical::group_below(*f.input)) {
      // A HAVING: the group's consumer runs only for a group that passes,
      // in a block, since a streamed group is given inside its input's loop.
      produce(*f.input, [&] {
        std::string cond;
        for (const auto& c : f.conjuncts) cond += (cond.empty() ? "" : " || ") + group_fails(c);
        w_.open("if (!(" + cond + "))");
        k();
        w_.close();
      });
      return;
    }
    // In written order, as the search returns them: a conjunct that tests
    // a correlated subquery runs it for the row in hand just before its
    // test, so a row an earlier conjunct rejects runs none. Over a one-row
    // aggregate's row (`hit_`) a run is for a row that is there and has
    // passed the conjuncts before.
    produce(*f.input, [&] {
      auto test = [&](const Expr& c) {
        if (!physical::evaluated(c)) {
          if (hit_) {
            *hit_ += " && !(" + fails(c) + ")";
          } else {
            miss(fails(c));
          }
          return;
        }
        // An evaluated conjunct prints statements: after the tests before
        // it, so its code never runs for a row they reject or that is not
        // there (`hit_`: a block of its own over a one-row aggregate's row).
        if (hit_) {
          auto flag = "h" + std::to_string(exprs_++);
          w_.line("bool " + flag + " = " + (hit_->empty() ? std::string{"1"} : *hit_) + ";");
          match_block(flag, c);
          *hit_ = flag;
          return;
        }
        miss(rejects(eval(c)));
      };
      for (const auto& c : f.conjuncts) {
        auto runs = correlated_in(c);
        if (!runs.empty()) {
          if (hit_) w_.open("if (" + (hit_->empty() ? std::string{"1"} : *hit_) + ")");
          for (auto i : runs) run_correlated(i);
          if (hit_) w_.close();
        }
        test(c);
      }
      k();
    });
  }

  // Over a loop, a counter the loop's condition checks, the rows before the
  // offset counted and not passed on; over a single row, the row, which an
  // offset drops. LIMIT 0, or an OFFSET past the LIMIT, prints nothing
  // (limit_zero).
  void produce(const physical::Limit& l, const Consume& k) {
    if (!loops(*l.input)) {
      if (l.offset > 0) return;
      produce(*l.input, k);
      return;
    }
    w_.line("int64_t n = 0;");
    limit_ = (base_limit_.empty() ? "" : base_limit_ + " && ") + "n < " + std::to_string(l.n);
    produce(*l.input, [&] {
      if (l.offset > 0) {
        w_.open("if (n >= " + std::to_string(l.offset) + ")");
        k();
        w_.close();
      } else {
        k();
      }
      w_.line("++n;");
    });
  }

  // FOR UPDATE: the row's key is guarded unchanged, so the commit conflicts
  // if a concurrent commit changed the row (docs/transaction_design.md,
  // *Isolation*).
  void produce(const physical::Emit& e, const Consume&) {
    produce(*e.input, [&] {
      // Each expression first, in the row in hand: an error it raises is
      // sticky, so the row is never half written.
      std::vector<Val> computed(e.targets.size());
      for (std::size_t i = 0; i < e.targets.size(); ++i) {
        if (const auto* x = std::get_if<physical::ExprTarget>(&e.targets[i])) computed[i] = eval(x->expr);
      }
      if (sink_ != nullptr) {
        Row row;
        for (std::size_t i = 0; i < e.targets.size(); ++i) {
          if (const auto* p = std::get_if<physical::ParamTarget>(&e.targets[i])) {
            row.push_back({param_null(p->param), param(p->type, p->param), p->type});
          } else if (std::holds_alternative<physical::ExprTarget>(e.targets[i])) {
            row.push_back({computed[i].null.empty() ? "0" : computed[i].null, computed[i].value, computed[i].type});
          } else {
            const auto& c = std::get<ColumnRef>(e.targets[i]);
            row.push_back({col_null(c), read_col(c), c.type});
          }
        }
        (*sink_)(w_, row);
        return;
      }
      if (rows_.size() == 1 && rows_[0].covering) {
        // Every result column from the entry's key: no row read.
        const auto& b = rows_[0];
        if (std::ranges::any_of(e.targets, [](const auto& t) { return std::holds_alternative<ColumnRef>(t); })) {
          w_.line("bc_view key = bc_scan_key(cx, " + b.cursor + ");");
        }
        w_.line("bc_emit_begin(cx);");
        for (std::size_t i = 0; i < e.targets.size(); ++i) {
          if (const auto* p = std::get_if<physical::ParamTarget>(&e.targets[i])) {
            emit_param(*p);
          } else if (std::holds_alternative<physical::ExprTarget>(e.targets[i])) {
            emit_val(computed[i]);
          } else {
            w_.line("bc_emit_key_part(cx, " + w_.obj(b.object) + ", key, " + w_.part(b.object, std::get<ColumnRef>(e.targets[i])) + ");");
          }
        }
        w_.line("bc_emit_end(cx);");
        return;
      }
      for (const auto& b : rows_) {
        if (b.for_update) w_.line("bc_guard_row(cx, " + w_.tbl(b.table) + ", " + b.row + ");");
      }
      w_.line("bc_emit_begin(cx);");
      for (std::size_t i = 0; i < e.targets.size(); ++i) {
        if (const auto* p = std::get_if<physical::ParamTarget>(&e.targets[i])) {
          emit_param(*p);
          continue;
        }
        if (std::holds_alternative<physical::ExprTarget>(e.targets[i])) {
          emit_val(computed[i]);
          continue;
        }
        const auto& t = std::get<ColumnRef>(e.targets[i]);
        if (const auto& b = binding(t.table); b.covering) {
          w_.line("bc_emit_key_part(cx, " + key_part(b, t) + ";");
        } else if (b.derived) {
          emit_val({t.nullable ? col_null(t) : "", read_col(t), t.type});
        } else {
          w_.line("bc_emit_col(cx, " + row_of(t) + ", " + w_.col(t) + ");");
        }
      }
      w_.line("bc_emit_end(cx);");
    });
  }

  // ---------------------------------------------------- expressions over columns

  // The value of e for the rows in hand, in C: NULL (empty: never) and the
  // value (garbage where it is NULL). Printed as it goes: an operator's
  // result is a local, so each operand is evaluated once however deep the
  // tree; AND and OR evaluate an operand only while the result is open, as
  // PostgreSQL does left to right, so a guard such as `b <> 0 AND a / b > 1`
  // raises no error. Integer and DECIMAL arithmetic is the plan API's
  // checked helpers: an overflow is 22003, a zero divisor 22012. A DECIMAL is
  // an int64_t at the scale of its type, operands brought to a common scale
  // by a power of ten the binder has kept within 18 digits.
  auto eval(const Expr& e) -> Val {
    switch (e.kind) {
      case Expr::Kind::Column: return {e.col.nullable ? col_null(e.col) : "", read_col(e.col), e.col.type};
      case Expr::Kind::Param: return {param_null(e.param), param(e.phys(), e.param), e.phys()};
      case Expr::Kind::Const: return {"", e.phys() == PhysType::Bool ? (e.constant != 0 ? "1" : "0") : constant(e.constant), e.phys()};
      case Expr::Kind::Add:
      case Expr::Kind::Sub:
      case Expr::Kind::Mul:
      case Expr::Kind::Div: return eval_arithmetic(e);
      case Expr::Kind::Mod: return eval_mod(e);
      case Expr::Kind::Neg:
      case Expr::Kind::Abs: return eval_sign(e);
      case Expr::Kind::Compare: return eval_compare(e);
      case Expr::Kind::IsNull: {
        auto v = eval(e.args[0]);
        return {"", v.null.empty() ? "0" : "(" + v.null + ")", PhysType::Bool};
      }
      case Expr::Kind::Not: {
        auto v = eval(e.args[0]);
        return {v.null, "!(" + v.value + ")", PhysType::Bool};
      }
      case Expr::Kind::And:
      case Expr::Kind::Or: return eval_connective(e);
      case Expr::Kind::Case: return eval_case(e);
      case Expr::Kind::Coalesce: return eval_coalesce(e);
      case Expr::Kind::Concat: return eval_concat(e);
      case Expr::Kind::Substr: return eval_substr(e);
      case Expr::Kind::TextFn: return eval_text(e);
      case Expr::Kind::Extract: return eval_extract(e);
      case Expr::Kind::Cast: return eval_cast(e);
      case Expr::Kind::Jsonb: return eval_jsonb(e);
      case Expr::Kind::Random: {
        auto lo = eval(e.args[0]);
        auto hi = eval(e.args[1]);
        return guarded("int64_t", "0", either(lo.null, hi.null), "bc_random(cx, " + lo.value + ", " + hi.value + ")",
                       PhysType::I64);
      }
      case Expr::Kind::Like: {
        // As truth prints a column's: ESCAPE $n checked, and its code point
        // taken, at each row.
        auto x = eval(e.args[0]);
        auto p = eval(e.args[1]);
        auto null = either(x.null, p.null);
        auto escape = std::to_string(e.constant);
        if (e.args.size() == 3) {
          auto c = eval(e.args[2]);
          null = either(null, c.null);
          escape = "bc_like_escape(cx, " + c.value + ")";
        }
        return guarded("bool", "0", null,
                       std::string{e.fold ? "bc_ilike" : "bc_like"} + "(cx, " + x.value + ", " + p.value + ", " + escape + ")",
                       PhysType::Bool);
      }
      case Expr::Kind::Truth: {  // a subquery's BOOLEAN parameter
        auto p = e.args[0].param;
        return {param_null(p), param(PhysType::Bool, p), PhysType::Bool};
      }
      case Expr::Kind::InSet: {  // the column's value among a subquery's
        const auto& col = e.args[0].col;
        auto x = eval(e.args[0]);
        return guarded("bool", "0", x.null,
                       "bc_sub_has_" + key_suffix(col.type) + "(cx, " + std::to_string(e.constant) + ", " + x.value + ")",
                       PhysType::Bool);
      }
      case Expr::Kind::Aggregate:
      case Expr::Kind::GroupKey: break;  // the group in hand's (group_value)
    }
    throw std::logic_error{"physical plan: an expression over columns codegen does not print"};
  }

  // Whether a BOOLEAN value rejects the row, as a conjunct: NULL or false.
  [[nodiscard]] static auto rejects(const Val& v) -> std::string {
    return v.null.empty() ? "!(" + v.value + ")" : v.null + " || !(" + v.value + ")";
  }

  // The negation of a NULL test, as C: parenthesized where it is a
  // disjunction (either).
  [[nodiscard]] static auto is_not(const std::string& null) -> std::string {
    return null.find("||") == std::string::npos ? "!" + null : "!(" + null + ")";
  }

  [[nodiscard]] static auto either(const std::string& a, const std::string& b) -> std::string {
    return a.empty() ? b : b.empty() ? a : a + " || " + b;
  }

  // `v` of type `from` at `scale`, a finer one: times a power of ten.
  static auto at_scale(const Val& v, SqlType from, int scale) -> std::string {
    auto up = scale - decimal_scale(from);
    if (up == 0) return v.value;
    std::int64_t factor = 1;
    for (auto i = 0; i < up; ++i) factor *= 10;
    return "bc_mul(cx, " + v.value + ", " + constant(factor) + ")";
  }

  auto eval_arithmetic(const Expr& e) -> Val {
    auto a = eval(e.args[0]);
    auto b = eval(e.args[1]);
    auto additive = e.kind == Expr::Kind::Add || e.kind == Expr::Kind::Sub;
    // A sum is taken at the result's scale; a product or quotient of the
    // operands' own.
    auto scale = decimal_scale(e.type);
    auto x = additive ? at_scale(a, e.args[0].type, scale) : a.value;
    auto y = additive ? at_scale(b, e.args[1].type, scale) : b.value;
    // A BIGINT sum fails with that type's range error; a SMALLINT or INTEGER
    // result is checked against its own range, as PostgreSQL's operators of
    // those types are (the operands are held in 64 bits, so these cannot
    // overflow first). The object a range error names is range_object_.
    std::string call;
    switch (e.kind) {
      case Expr::Kind::Add:
      case Expr::Kind::Sub: {
        auto fn = std::string{e.kind == Expr::Kind::Add ? "bc_add" : "bc_sub"};
        call = e.type.kind == SqlKind::BigInt ? fn + "_bigint(cx, " + x + ", " + y + ", " + range_object_ + ")" : fn + "(cx, " + x + ", " + y + ")";
        break;
      }
      case Expr::Kind::Mul: call = "bc_mul(cx, " + x + ", " + y + ")"; break;
      case Expr::Kind::Div: call = "bc_div(cx, " + x + ", " + y + ")"; break;
      case Expr::Kind::Column:
      case Expr::Kind::Param:
      case Expr::Kind::Const:
      case Expr::Kind::Compare:
      case Expr::Kind::Aggregate:
      case Expr::Kind::And:
      case Expr::Kind::Or:
      case Expr::Kind::Not:
      case Expr::Kind::IsNull:
      case Expr::Kind::Like:
      case Expr::Kind::Truth:
      case Expr::Kind::InSet:
      case Expr::Kind::Case:
      case Expr::Kind::Coalesce:
      case Expr::Kind::Concat:
      case Expr::Kind::Substr:
      case Expr::Kind::TextFn:
      case Expr::Kind::Extract:
      case Expr::Kind::Cast:
      case Expr::Kind::Jsonb:
      case Expr::Kind::GroupKey:
      case Expr::Kind::Random:
      case Expr::Kind::Mod:
      case Expr::Kind::Neg:
      case Expr::Kind::Abs: throw std::logic_error{"physical plan: not an arithmetic expression"};
    }
    if (e.type.kind == SqlKind::SmallInt || e.type.kind == SqlKind::Integer) {
      call = "bc_check_int(cx, " + call + ", " + (e.type.kind == SqlKind::SmallInt ? "BC_SMALLINT" : "BC_INTEGER") + ", " + range_object_ + ")";
    }
    return guarded("int64_t", "0", either(a.null, b.null), call, PhysType::I64);
  }

  // A jsonb accessor (docs/jsonb_design.md, stage 3): a call that writes
  // its result through a pointer and returns false for SQL NULL, after its
  // operands' own NULL tests; jsonb_typeof and jsonb_array_length, strict.
  // A path is a folded one's parameter, a parameter's text read by
  // bc_jsonb_path_in, or ARRAY[…]'s elements added in turn.
  auto eval_jsonb(const Expr& e) -> Val {
    using J = Expr::Jsonb;
    switch (e.jsonb) {
      case J::Typeof:
      case J::ArrayLength: {
        auto x = eval(e.args[0]);
        if (e.jsonb == J::Typeof) return guarded("bc_view", "bc_empty(cx)", x.null, "bc_jsonb_typeof(cx, " + x.value + ")", PhysType::Bytes);
        return guarded("int64_t", "0", x.null, "bc_jsonb_array_length(cx, " + x.value + ")", PhysType::I64);
      }
      case J::PathIn: {
        auto x = eval(e.args[0]);
        return guarded("bc_view", "bc_empty(cx)", x.null, "bc_jsonb_path_in(cx, " + x.value + ")", PhysType::Bytes);
      }
      case J::PathArray: {
        auto id = std::to_string(exprs_++);
        w_.line("bc_view v" + id + " = bc_empty(cx);");
        for (const auto& a : e.args) {
          auto x = eval(a);
          // A NULL element's value is not read: a NULL parameter has none.
          auto null = x.null.empty() ? std::string{"0"} : "(" + x.null + ")";
          auto value = x.null.empty() ? x.value : null + " ? bc_empty(cx) : " + x.value;
          w_.line("v" + id + " = bc_jsonb_path_add(cx, v" + id + ", " + value + ", " + null + ");");
        }
        return {"", "v" + id, PhysType::Bytes};
      }
      case J::Field:
      case J::FieldText:
      case J::Element:
      case J::ElementText:
      case J::Path:
      case J::PathText: break;
    }
    auto x = eval(e.args[0]);
    auto k = eval(e.args[1]);
    auto text = e.jsonb == J::FieldText || e.jsonb == J::ElementText || e.jsonb == J::PathText;
    auto fn = std::string{e.jsonb == J::Field || e.jsonb == J::FieldText       ? "bc_jsonb_field"
                          : e.jsonb == J::Element || e.jsonb == J::ElementText ? "bc_jsonb_element"
                                                                               : "bc_jsonb_path"} +
              (text ? "_text" : "");
    auto id = std::to_string(exprs_++);
    auto type = text ? PhysType::Bytes : PhysType::Jsonb;
    w_.line(text ? "bc_view v" + id + " = bc_empty(cx);" : declared(type, "v" + id));
    auto null = either(x.null, k.null);
    auto call = "!" + fn + "(cx, " + x.value + ", " + k.value + ", &v" + id + ")";
    if (null.empty()) {
      w_.line("bool n" + id + " = " + call + ";");
    } else {
      w_.line("bool n" + id + " = " + null + ";");
      w_.line("if (!n" + id + ") n" + id + " = " + call + ";");
    }
    return {"n" + id, "v" + id, type};
  }

  // EXTRACT(field FROM x): the plan API's bc_extract, its field the
  // binder's DateField code and its source x's type.
  auto eval_extract(const Expr& e) -> Val {
    auto x = eval(e.args[0]);
    auto source = std::string{e.args[0].type.kind == SqlKind::Date        ? "BC_SOURCE_DATE"
                              : e.args[0].type.kind == SqlKind::Time      ? "BC_SOURCE_TIME"
                              : e.args[0].type.kind == SqlKind::Timestamp ? "BC_SOURCE_TIMESTAMP"
                                                                          : "BC_SOURCE_TIMESTAMPTZ"};
    auto call = "bc_extract(cx, " + x.value + ", " + std::to_string(e.constant) + ", " + source + ")";
    return guarded("int64_t", "0", x.null, call, PhysType::I64);
  }

  // a % b at the result's scale: the plan API's bc_mod (a zero divisor is
  // 22012, INT64_MIN % -1 is 0). The remainder is below the divisor, so no
  // range check.
  auto eval_mod(const Expr& e) -> Val {
    auto a = eval(e.args[0]);
    auto b = eval(e.args[1]);
    auto scale = decimal_scale(e.type);
    auto call = "bc_mod(cx, " + at_scale(a, e.args[0].type, scale) + ", " + at_scale(b, e.args[1].type, scale) + ")";
    return guarded("int64_t", "0", either(a.null, b.null), call, PhysType::I64);
  }

  // -x and abs(x), of x's type. An integer's is 0 - x, its range checked as
  // its type's operator does ("bigint out of range" past 64 bits); a
  // DECIMAL's cannot leave 18 digits. A float's is IEEE: -x, and abs as
  // 0.0 - x for x <= 0, so abs(-0.0) is 0 as PostgreSQL's fabs gives it.
  auto eval_sign(const Expr& e) -> Val {
    auto x = eval(e.args[0]);
    auto abs = e.kind == Expr::Kind::Abs;
    if (x.type == PhysType::F64) {
      auto value = abs ? "(" + x.value + " <= 0 ? 0.0 - " + x.value + " : " + x.value + ")" : "(-" + x.value + ")";
      return guarded("double", "0", x.null, value, PhysType::F64);
    }
    std::string neg;
    switch (e.type.kind) {
      case SqlKind::BigInt: neg = "bc_sub_bigint(cx, 0, " + x.value + ", " + range_object_ + ")"; break;
      case SqlKind::SmallInt:
      case SqlKind::Integer:
        neg = "bc_check_int(cx, bc_sub(cx, 0, " + x.value + "), " +
              (e.type.kind == SqlKind::SmallInt ? "BC_SMALLINT" : "BC_INTEGER") + ", " + range_object_ + ")";
        break;
      case SqlKind::Decimal: neg = "bc_sub(cx, 0, " + x.value + ")"; break;
      case SqlKind::Real:
      case SqlKind::Double:
      case SqlKind::Numeric:
      case SqlKind::Jsonb:
      case SqlKind::Boolean:
      case SqlKind::Text:
      case SqlKind::Varchar:
      case SqlKind::Char:
      case SqlKind::Name:
      case SqlKind::Bytea:
      case SqlKind::Date:
      case SqlKind::Time:
      case SqlKind::TimeTz:
      case SqlKind::Timestamp:
      case SqlKind::TimestampTz: throw std::logic_error{"physical plan: - or abs of other than an integer or a DECIMAL"};
    }
    // The operand is a local or a column read: evaluated once, and the
    // negation only where it is taken.
    auto value = abs ? "(" + x.value + " < 0 ? " + neg + " : " + x.value + ")" : neg;
    return guarded("int64_t", "0", x.null, value, PhysType::I64);
  }

  // A SQL type as the plan API's bc_sql_type names it.
  static auto sql_type_code(SqlKind k) -> std::string {
    switch (k) {
      case SqlKind::SmallInt: return "BC_TYPE_SMALLINT";
      case SqlKind::Integer: return "BC_TYPE_INTEGER";
      case SqlKind::BigInt: return "BC_TYPE_BIGINT";
      case SqlKind::Real: return "BC_TYPE_REAL";
      case SqlKind::Double: return "BC_TYPE_DOUBLE";
      case SqlKind::Boolean: return "BC_TYPE_BOOLEAN";
      case SqlKind::Bytea: return "BC_TYPE_BYTEA";
      case SqlKind::Text: return "BC_TYPE_TEXT";
      case SqlKind::Varchar: return "BC_TYPE_VARCHAR";
      case SqlKind::Decimal: return "BC_TYPE_DECIMAL";
      case SqlKind::Timestamp: return "BC_TYPE_TIMESTAMP";
      case SqlKind::Date: return "BC_TYPE_DATE";
      case SqlKind::Time: return "BC_TYPE_TIME";
      case SqlKind::Numeric: return "BC_TYPE_NUMERIC";
      case SqlKind::TimestampTz: return "BC_TYPE_TIMESTAMPTZ";
      case SqlKind::Char: return "BC_TYPE_CHAR";
      case SqlKind::Name: return "BC_TYPE_NAME";
      case SqlKind::TimeTz: return "BC_TYPE_TIMETZ";
      case SqlKind::Jsonb: return "BC_TYPE_JSONB";
    }
    throw std::logic_error{"physical plan: an unknown SQL type"};
  }

  static auto int_type_code(SqlKind k) -> std::string {
    return k == SqlKind::SmallInt ? "BC_SMALLINT" : k == SqlKind::Integer ? "BC_INTEGER" : "BC_BIGINT";
  }

  // x::T computed per row (docs/planner_design.md, *Casts in the search, the
  // check and codegen*), its operand's NULL never reaching a call: pure C
  // where nothing can fail, else the plan API's cast helpers over the
  // functions the binder folds a constant's cast with. A range error names
  // no object (0).
  auto eval_cast(const Expr& e) -> Val {
    using K = SqlKind;
    auto v = eval(e.args[0]);
    const auto from = e.args[0].type;
    const auto to = e.type;
    auto is_int = [](K k) { return k == K::SmallInt || k == K::Integer || k == K::BigInt; };
    auto is_float = [](K k) { return k == K::Real || k == K::Double; };
    auto is_text = [](K k) { return k == K::Text || k == K::Varchar || k == K::Name || k == K::Char; };
    auto is_time = [](K k) { return k == K::Date || k == K::Time || k == K::TimeTz || k == K::Timestamp || k == K::TimestampTz; };
    auto call = [&](const std::string& ctype, const std::string& fallback, const std::string& value) {
      return guarded(ctype, fallback, v.null, value, e.phys());
    };
    auto view = [&](const std::string& value) { return call("bc_view", "bc_empty(cx)", value); };
    auto i64 = [&](const std::string& value) { return call("int64_t", "0", value); };
    auto f64 = [&](const std::string& value) { return call("double", "0.0", value); };
    auto n = [](auto x) { return std::to_string(x); };
    // A relabel over a cast: the cast's value, of another type of the same bytes.
    if (cast_kind(from, to) == CastKind::Relabel) return v;
    // jsonb's casts out, and text to jsonb (stage 3's calls).
    if (to.kind == K::Jsonb) return guarded("bc_jsonb", "", v.null, "bc_jsonb_in(cx, " + v.value + ")", PhysType::Jsonb);
    if (from.kind == K::Jsonb) {
      if (is_text(to.kind)) {
        auto text = "bc_jsonb_text(cx, " + v.value + ")";
        return view(to.kind == K::Name ? "bc_text_in_bytes(cx, " + text + ", BC_TYPE_NAME)" : text);
      }
      if (to.kind == K::Boolean) return call("bool", "0", "bc_jsonb_to_bool(cx, " + v.value + ")");
      if (is_int(to.kind)) return i64("bc_jsonb_to_i64(cx, " + v.value + ", " + int_type_code(to.kind) + ", " + range_object_ + ")");
      if (is_float(to.kind)) return f64("bc_jsonb_to_f64(cx, " + v.value + ", " + (to.kind == K::Real ? "1" : "0") + ")");
      if (to.kind == K::Numeric) return view("bc_jsonb_to_num(cx, " + v.value + ")");
      throw std::logic_error{"physical plan: a jsonb cast codegen does not print"};
    }
    // To a string type: a CHAR(n)'s value without its trailing spaces, a
    // NAME's cut; any other type's output function.
    if (is_text(to.kind)) {
      std::string text;
      if (is_text(from.kind) || from.kind == K::Numeric) {
        text = from.kind == K::Char ? "bc_trim(cx, " + v.value + ", bc_empty(cx), BC_TRIM_TRAILING | BC_TRIM_SPACES)" : v.value;
      } else if (from.kind == K::Bytea) {
        text = "bc_bytea_text(cx, " + v.value + ")";
      } else if (from.kind == K::Boolean) {
        // booltext to text; boolout, the I/O cast, to a name.
        text = "bc_value_text(cx, " + v.value + " ? 1 : 0, BC_TYPE_BOOLEAN, " + (to.kind == K::Name ? "1" : "0") + ")";
      } else {
        text = "bc_value_text(cx, " + v.value + ", " + sql_type_code(from.kind) + ", " + n(decimal_scale(from)) + ")";
      }
      if (to.kind == K::Name && from.kind != K::Name) text = "bc_text_in_bytes(cx, " + text + ", BC_TYPE_NAME)";
      return view(text);
    }
    // From a string type: the target's input function.
    if (is_text(from.kind)) {
      if (is_int(to.kind) || is_time(to.kind) || to.kind == K::Decimal) {
        return i64("bc_text_in_i64(cx, " + v.value + ", " + sql_type_code(to.kind) + ", " + n(to.precision) + ", " +
                   n(to.scale) + ", " + range_object_ + ")");
      }
      if (is_float(to.kind)) return f64("bc_text_in_f64(cx, " + v.value + ", " + sql_type_code(to.kind) + ")");
      if (to.kind == K::Boolean) return call("bool", "0", "bc_text_in_bool(cx, " + v.value + ")");
      if (to.kind == K::Bytea) return view("bc_text_in_bytes(cx, " + v.value + ", BC_TYPE_BYTEA)");
      throw std::logic_error{"physical plan: a cast from text codegen does not print"};
    }
    if (is_time(from.kind)) {
      if (from.kind == to.kind || ((from.kind == K::Timestamp || from.kind == K::TimestampTz) &&
                                   (to.kind == K::Timestamp || to.kind == K::TimestampTz))) {
        return i64(v.value);  // unchanged in the layer's UTC
      }
      return i64("bc_temporal_cast(cx, " + v.value + ", " + sql_type_code(from.kind) + ", " + sql_type_code(to.kind) + ")");
    }
    if (from.kind == K::Boolean) return i64(v.value + " ? 1 : 0");
    if (to.kind == K::Boolean) return call("bool", "0", v.value + " != 0");
    if (is_float(from.kind)) {
      if (is_int(to.kind)) return i64("bc_f64_to_int(cx, " + v.value + ", " + int_type_code(to.kind) + ", " + range_object_ + ")");
      if (to.kind == K::Decimal) {
        return i64("bc_f64_to_decimal(cx, " + v.value + ", " + (from.kind == K::Real ? "1" : "0") + ", " + n(to.precision) +
                   ", " + n(to.scale) + ", " + range_object_ + ")");
      }
      return f64(to.kind == K::Real ? "bc_f64_to_f32(cx, " + v.value + ")" : v.value);
    }
    // An integer or a DECIMAL.
    if (is_float(to.kind)) {
      if (from.kind == K::Decimal) {
        return f64("bc_decimal_to_f64(cx, " + v.value + ", " + n(from.scale) + ", " + (to.kind == K::Real ? "1" : "0") + ")");
      }
      return f64(to.kind == K::Real ? "bc_i64_to_f32(" + v.value + ")" : "bc_i64_to_f64(" + v.value + ")");
    }
    if (from.kind != K::Decimal && is_int(to.kind)) {
      // Widening is the value; narrowing is range-checked, never wrapped.
      auto narrows = to.kind == K::SmallInt || (to.kind == K::Integer && from.kind == K::BigInt);
      return i64(narrows ? "bc_check_int(cx, " + v.value + ", " + int_type_code(to.kind) + ", " + range_object_ + ")" : v.value);
    }
    return i64("bc_decimal_rescale(cx, " + v.value + ", " + n(decimal_scale(from)) + ", " + sql_type_code(to.kind) + ", " +
               n(to.precision) + ", " + n(to.scale) + ", " + range_object_ + ")");
  }

  // a || b: text with text, an integer or a DECIMAL through its text. NULL if
  // either is.
  auto eval_concat(const Expr& e) -> Val {
    auto text = [&](const Expr& arg) {
      auto v = eval(arg);
      if (v.type == PhysType::Bytes) return v;
      v.value = "bc_value_text(cx, " + v.value + ", " + sql_type_code(arg.type.kind) + ", " +
                std::to_string(decimal_scale(arg.type)) + ")";
      return v;
    };
    auto a = text(e.args[0]);
    auto b = text(e.args[1]);
    return guarded("bc_view", "bc_empty(cx)", either(a.null, b.null), "bc_concat(cx, " + a.value + ", " + b.value + ")",
                   PhysType::Bytes);
  }

  // substring(s FROM a [FOR n]): NULL if any operand is.
  auto eval_substr(const Expr& e) -> Val {
    auto s = eval(e.args[0]);
    auto from = eval(e.args[1]);
    std::optional<Val> count;
    if (e.args.size() == 3) count = eval(e.args[2]);
    auto null = either(s.null, from.null);
    if (count) null = either(null, count->null);
    auto n = count ? count->value : constant(std::numeric_limits<std::int64_t>::max());
    return guarded("bc_view", "bc_empty(cx)", null, "bc_substr(cx, " + s.value + ", " + from.value + ", " + n + ")",
                   PhysType::Bytes);
  }

  // A string function of text: NULL if any operand is.
  auto eval_text(const Expr& e) -> Val {
    using T = logical::Expr::Text;
    std::vector<Val> args;
    std::string null;
    for (const auto& a : e.args) {
      args.push_back(eval(a));
      null = either(null, args.back().null);
    }
    const auto& s = args[0].value;
    auto text = [&](const std::string& call) { return guarded("bc_view", "bc_empty(cx)", null, call, PhysType::Bytes); };
    auto number = [&](const std::string& call) { return guarded("int64_t", "0", null, call, PhysType::I64); };
    auto trim = [&](const char* how) {
      auto set = args.size() == 2 ? args[1].value : std::string{"bc_empty(cx)"};
      auto flags = std::string{how} + (args.size() == 2 ? "" : " | BC_TRIM_SPACES");
      return text("bc_trim(cx, " + s + ", " + set + ", " + flags + ")");
    };
    switch (e.text) {
      case T::Upper: return text("bc_text_case(cx, " + s + ", true)");
      case T::Lower: return text("bc_text_case(cx, " + s + ", false)");
      case T::CharLength: return number("bc_char_length(cx, " + s + ")");
      case T::OctetLength: return number("bc_octet_length(cx, " + s + ")");
      case T::Btrim: return trim("BC_TRIM_LEADING | BC_TRIM_TRAILING");
      case T::Ltrim: return trim("BC_TRIM_LEADING");
      case T::Rtrim: return trim("BC_TRIM_TRAILING");
      case T::Position: return number("bc_position(cx, " + s + ", " + args[1].value + ")");
    }
    throw std::logic_error{"physical plan: a string function codegen does not print"};
  }

  // A local `type v = fallback`, set to `value` unless `null`; the Val of
  // the local and the NULL test, evaluated once.
  auto guarded(const std::string& ctype, const std::string& fallback, const std::string& null, const std::string& value,
               PhysType type) -> Val {
    auto id = std::to_string(exprs_++);
    w_.line(type == PhysType::Jsonb ? declared(type, "v" + id) : ctype + " v" + id + " = " + fallback + ";");
    if (null.empty()) {
      w_.line("v" + id + " = " + value + ";");
      return {"", "v" + id, type};
    }
    w_.line("bool n" + id + " = " + null + ";");
    w_.open("if (!n" + id + ")");
    w_.line("v" + id + " = " + value + ";");
    w_.close();
    return {"n" + id, "v" + id, type};
  }

  // A comparison as a value: NULL if an operand is. Numbers meet at the
  // finer scale, text and bytes compare as bytes (COLLATE "C").
  auto eval_compare(const Expr& e) -> Val {
    auto a = eval(e.args[0]);
    auto b = eval(e.args[1]);
    const auto& lt = e.args[0].type;
    const auto& rt = e.args[1].type;
    auto scale = std::max(decimal_scale(lt), decimal_scale(rt));
    auto x = at_scale(a, lt, scale);
    auto y = at_scale(b, rt, scale);
    return guarded("bool", "0", either(a.null, b.null), compare_text(lt, rt, e.op, x, y), PhysType::Bool);
  }

  // AND and OR in SQL's three-valued logic, an operand evaluated only while
  // no earlier one has settled the result (false for AND, true for OR).
  auto eval_connective(const Expr& e) -> Val {
    auto id = std::to_string(exprs_++);
    auto is_and = e.kind == Expr::Kind::And;
    w_.line("bool d" + id + " = 0;");
    w_.line("bool n" + id + " = 0;");
    for (const auto& arg : e.args) {
      w_.open("if (!d" + id + ")");
      auto v = eval(arg);
      auto settles = is_and ? "!(" + v.value + ")" : v.value;
      if (v.null.empty()) {
        w_.line("if (" + settles + ") d" + id + " = 1;");
      } else {
        w_.line("if (" + v.null + ") n" + id + " = 1; else if (" + settles + ") d" + id + " = 1;");
      }
      w_.close();
    }
    w_.line("n" + id + " = n" + id + " && !d" + id + ";");
    return {"n" + id, is_and ? "!d" + id : "d" + id, PhysType::Bool};
  }

  // The result of a CASE or COALESCE: a local, NULL until a branch gives a
  // value (so a CASE no WHEN takes and has no ELSE is NULL).
  auto open_result(const Expr& e) -> std::string {
    auto id = std::to_string(exprs_++);
    switch (e.phys()) {
      case PhysType::I64: w_.line("int64_t v" + id + " = 0;"); break;
      case PhysType::F64: w_.line("double v" + id + " = 0;"); break;
      case PhysType::Bool: w_.line("bool v" + id + " = 0;"); break;
      case PhysType::Bytes:
      case PhysType::Num: w_.line("bc_view v" + id + "; v" + id + ".i = 0; v" + id + ".g = 0;"); break;
      case PhysType::Jsonb: w_.line("bc_jsonb v" + id + "; v" + id + ".i = 0; v" + id + ".g = 0;"); break;
    }
    w_.line("bool n" + id + " = 1;");
    return id;
  }

  // A branch's value as the result: at the result's scale if both are
  // numerics, and NULL as NULL. Printed only inside the branch's own block,
  // so an error the branch raises is raised only when it is taken.
  void take(const std::string& id, const Expr& result, const Expr& branch) {
    auto v = eval(branch);
    auto value = result.type.kind == SqlKind::Decimal ? at_scale(v, branch.type, decimal_scale(result.type)) : v.value;
    if (v.null.empty()) {
      w_.line("n" + id + " = 0;");
      w_.line("v" + id + " = " + value + ";");
      return;
    }
    w_.line("n" + id + " = " + v.null + ";");
    w_.open("if (!n" + id + ")");
    w_.line("v" + id + " = " + value + ";");
    w_.close();
  }

  // CASE: each WHEN is evaluated only while no earlier one was true, its
  // THEN only when its WHEN is. A NULL condition is not true.
  auto eval_case(const Expr& e) -> Val {
    auto id = open_result(e);
    w_.line("bool d" + id + " = 0;");
    for (std::size_t i = 0; i + 1 < e.args.size(); i += 2) {
      w_.open("if (!d" + id + ")");
      auto c = eval(e.args[i]);
      w_.open("if (" + (c.null.empty() ? c.value : "!(" + c.null + ") && " + c.value) + ")");
      w_.line("d" + id + " = 1;");
      take(id, e, e.args[i + 1]);
      w_.close();
      w_.close();
    }
    if (e.args.size() % 2 == 1) {
      w_.open("if (!d" + id + ")");
      take(id, e, e.args.back());
      w_.close();
    }
    return {"n" + id, "v" + id, e.phys()};
  }

  // COALESCE: each argument is evaluated only while the ones before it are
  // NULL.
  auto eval_coalesce(const Expr& e) -> Val {
    auto id = open_result(e);
    for (const auto& arg : e.args) {
      w_.open("if (n" + id + ")");
      take(id, e, arg);
      w_.close();
    }
    return {"n" + id, "v" + id, e.phys()};
  }

  // A computed value as a result column, NULL as NULL.
  void emit_val(const Val& v) {
    auto emit = "bc_emit_" + suffix(v.type) + "(cx, " + v.value + ");";
    if (v.null.empty()) {
      w_.line(emit);
      return;
    }
    w_.open("if (" + v.null + ")");
    w_.line("bc_emit_null(cx);");
    w_.otherwise();
    w_.line(emit);
    w_.close();
  }

  // A parameter as a result column, NULL as NULL.
  void emit_param(const physical::ParamTarget& p) {
    w_.open("if (" + param_null(p.param) + ")");
    w_.line("bc_emit_null(cx);");
    w_.otherwise();
    w_.line("bc_emit_" + suffix(p.type) + "(cx, " + param(p.type, p.param) + ");");
    w_.close();
  }

  void emit_count(const std::string& n) {
    for (const auto& line : count_row(n)) w_.line(line);
  }

  // The SUM row: the value, or NULL when no row gave one.
  void emit_value_or_null(const std::string& value) {
    w_.line("bc_emit_begin(cx);");
    w_.open("if (seen)");
    w_.line("bc_emit_i64(cx, " + value + ");");
    w_.otherwise();
    w_.line("bc_emit_null(cx);");
    w_.close();
    w_.line("bc_emit_end(cx);");
  }

  // An aggregate's argument for the row in hand, evaluated once, as eval
  // prints it where the call is: whether it is NULL ("" if never) and its
  // value. An error it raises (22003, 22012) is the statement's. Of the one
  // aggregate row's loop, a column is read as arg_of_row reads it.
  auto agg_arg(const Expr& a, bool row_loop = false) -> Val {
    const auto& x = a.args[0];
    if (!row_loop || x.kind != Expr::Kind::Column) return eval(x);
    auto [is_null, value] = arg_of_row(x.col);
    return {x.col.nullable ? is_null : "", value, x.col.type};
  }

  // A column argument in the row in hand: its NULL test and its value, from
  // the entry's key when the scan covers.
  auto arg_of_row(const ColumnRef& arg) -> std::pair<std::string, std::string> {
    const auto& b = binding(arg.table);
    if (b.covering) {
      w_.line("bc_view key = bc_scan_key(cx, " + b.cursor + ");");
      auto at = w_.obj(b.object) + ", key, " + w_.part(b.object, arg) + ")";
      return {"bc_key_part_null(cx, " + at, "bc_key_part_i64(cx, " + at};
    }
    return {col_null(arg), read_col(arg)};
  }

  // The accumulators an aggregate's loop keeps: n for COUNT, sum and whether
  // a value was seen for SUM, groups for COUNT(DISTINCT).
  void open_aggregate(const Expr& a) {
    if (a.distinct) {
      w_.line("bc_agg groups = bc_agg_open(cx, 0);");
      return;
    }
    switch (a.fn) {
      case Expr::Fn::Count: w_.line("int64_t n = 0;"); break;
      case Expr::Fn::Sum: w_.line("int64_t sum = 0;"); break;
      case Expr::Fn::Min:
      case Expr::Fn::Max:
      case Expr::Fn::Avg: break;  // a GroupAggregate's
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
      case Expr::Fn::Min:
      case Expr::Fn::Max:
      case Expr::Fn::Avg: break;  // a GroupAggregate's
    }
  }

  // The row in hand folded into the aggregate the loop keeps. NULLs are
  // skipped. COUNT(DISTINCT) groups the value's key: a column's from its
  // row, an expression's from its value, evaluated before the key builder
  // opens (a DOUBLE's -0.0 and NaN keyed as PostgreSQL's equality has them).
  void fold(const Expr& a) {
    if (a.args.empty()) {
      w_.line("++n;");  // COUNT(*)
      return;
    }
    if (a.distinct && a.args[0].kind == Expr::Kind::Column) {
      const auto& arg = a.args[0].col;
      auto group = "bc_agg_group(cx, groups, bc_col_key(cx, " + row_of(arg) + ", " + w_.col(arg) + "));";
      w_.line(arg.nullable ? "if (!" + arg_of_row(arg).first + ") " + group : group);
      return;
    }
    if (a.distinct) {
      auto v = eval(a.args[0]);
      if (!v.null.empty()) w_.open("if (" + is_not(v.null) + ")");
      w_.line("bc_sort_key dvk = bc_sort_key_begin(cx);");
      w_.line("bc_sort_key_" + key_suffix(v.type) + "(cx, dvk, " + v.value + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
      w_.line("bc_agg_group(cx, groups, bc_sort_key_end(cx, dvk));");
      if (!v.null.empty()) w_.close();
      return;
    }
    auto v = agg_arg(a, true);
    switch (a.fn) {
      case Expr::Fn::Count: w_.line(v.null.empty() ? "++n;" : "if (" + is_not(v.null) + ") ++n;"); return;
      case Expr::Fn::Sum:
        if (!v.null.empty()) w_.open("if (" + is_not(v.null) + ")");
        w_.line("sum = bc_add(cx, sum, " + v.value + ");");
        w_.line("seen = true;");
        if (!v.null.empty()) w_.close();
        return;
      case Expr::Fn::Min:
      case Expr::Fn::Max:
      case Expr::Fn::Avg: return;  // a GroupAggregate's
    }
  }

  // An aggregate over at most one row, read: its value when `hit` holds,
  // NULL as NULL (COUNT: 1, or 0 for a NULL argument); over none, COUNT 0 and
  // SUM NULL.
  void emit_one_row(const Expr& a, const std::string& hit) {
    if (a.args.size() == 1 && a.args[0].kind != Expr::Kind::Column) {
      // An expression: evaluated where the row is in hand.
      if (a.fn == Expr::Fn::Count) w_.line("int64_t n = 0;");
      if (a.fn != Expr::Fn::Count) w_.line("bc_emit_begin(cx);");
      w_.open("if (" + hit + ")");
      auto v = eval(a.args[0]);
      if (a.fn == Expr::Fn::Count) {
        w_.line(v.null.empty() ? "n = 1;" : "if (" + is_not(v.null) + ") n = 1;");
      } else {
        emit_val(v);
      }
      if (a.fn != Expr::Fn::Count) {
        w_.otherwise();
        w_.line("bc_emit_null(cx);");
      }
      w_.close();
      if (a.fn == Expr::Fn::Count) {
        emit_count("n");
      } else {
        w_.line("bc_emit_end(cx);");
      }
      return;
    }
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
      auto k = key(w_, g->table, param_parts(g->key));
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
      hit_ = nullptr;  // an Empty input calls no consume
      return;
    }
    open_aggregate(a.agg);
    produce(*a.input, [&] { fold(a.agg); });
    emit_aggregate(a.agg);
  }

  // ------------------------------------------------------------- GROUP BY

  // The group in hand (docs/planner_design.md, *Aggregates*): in a
  // structure, its handle g<i> and the row each held table's hash table holds
  // for it, fr<i>_<j>; streamed, the locals kv<i>_<j> (a key's value) and
  // kn<i>_<j> (its NULL), x<i>_<j> (an aggregate's value), xs<i>_<j> (whether
  // a SUM, MIN or MAX saw one) and xc<i>_<j> (an AVG's count).
  struct Group {
    const physical::GroupAggregate* op{nullptr};
    std::vector<ColumnRef> columns;  // its keys, then its dependent columns
    physical::GroupLayout layout;
    std::string i;
    std::string handle;  // g<i>, in a structure
  };

  // A column's index in the group's columns, which number its slots and
  // locals.
  static auto key_index(const Group& g, const ColumnRef& c) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::find(g.columns, c) - g.columns.begin());
  }
  auto agg_index(const Group& g, const Expr& a) const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::find(g.op->aggs, a) - g.op->aggs.begin());
  }
  static auto local(const std::string& kind, const Group& g, std::size_t j) -> std::string {
    return kind + g.i + "_" + std::to_string(j);
  }

  // A column read from the row its group holds: a structure's key or
  // dependent column that is not an I64.
  auto held_row(const Group& g, const ColumnRef& c) const -> std::optional<std::string> {
    if (g.op->streamed || g.layout.key_slot[key_index(g, c)]) return std::nullopt;
    auto j = static_cast<std::size_t>(std::ranges::find(g.layout.held, c.table) - g.layout.held.begin());
    return local("fr", g, j);
  }

  // A key or an aggregate of the group in hand, not an AVG: whether it is
  // NULL, as C ("" if never), and its value.
  auto group_value(const Expr& e) -> std::pair<std::string, std::string> {
    const auto& g = *group_;
    if (e.kind == Expr::Kind::GroupKey) {
      // An expression key, in its slot from the group's first row; never
      // streamed (physical::check).
      auto slot = std::to_string(g.layout.expr_slot.at(static_cast<std::size_t>(e.constant)));
      auto at = "(cx, " + g.handle + ", " + slot + ")";
      auto value = std::string{e.phys() == PhysType::F64 ? "bc_agg_f64" : e.phys() == PhysType::Bytes ? "bc_agg_bytes" : "bc_agg_i64"} + at;
      if (e.phys() == PhysType::Bool) value = "(" + value + " != 0)";
      return {"bc_agg_null" + at, value};
    }
    if (e.kind == Expr::Kind::Column) {
      const auto& c = e.col;
      auto j = key_index(g, c);
      if (auto row = held_row(g, c)) {
        return {"bc_col_null(cx, " + *row + ", " + w_.col(c) + ")",
                "bc_col_" + carry_suffix(c.type) + "(cx, " + *row + ", " + w_.col(c) + ")"};
      }
      if (g.op->streamed) return {c.nullable ? local("kn", g, j) : "", local("kv", g, j)};
      auto slot = std::to_string(*g.layout.key_slot[j]);
      auto value = "bc_agg_i64(cx, " + g.handle + ", " + slot + ")";
      // A derived relation's BOOLEAN, held as 0 or 1 (group_layout).
      if (c.type == PhysType::Bool) value = "(" + value + " != 0)";
      return {c.nullable ? "bc_agg_null(cx, " + g.handle + ", " + slot + ")" : "", value};
    }
    auto j = agg_index(g, e);
    auto count = e.fn == Expr::Fn::Count;
    if (g.op->streamed && folds_bytes(e)) {
      // In the stream's holder (stream).
      auto at = "(cx, " + local("hg", g, 0) + ", " + std::to_string(holder_slot(g, j)) + ")";
      return {"bc_agg_null" + at, "bc_agg_bytes" + at};
    }
    if (g.op->streamed) return {count ? "" : "!" + local("xs", g, j), local("x", g, j)};
    auto slot = std::to_string(g.layout.agg_slot[j]);
    auto read = folds_f64(e) ? "bc_agg_f64" : folds_bytes(e) ? "bc_agg_bytes" : "bc_agg_i64";
    return {count ? "" : "bc_agg_null(cx, " + g.handle + ", " + slot + ")",
            std::string{read} + "(cx, " + g.handle + ", " + slot + ")"};
  }

  // Whether the group fails a HAVING conjunct, as C: NULL on either side, or
  // the comparison false.
  auto group_fails(const Expr& c) -> std::string {
    const auto& lhs = c.args[0];
    const auto& rhs = c.args[1];
    auto [is_null, value] = group_value(lhs);
    // The parameter is read as the key's or the aggregate's type.
    auto out = (is_null.empty() ? "" : is_null + " || ") + param_null(rhs.param);
    return out + " || !(" + compare_text(lhs.type, lhs.type, c.op, value, param(lhs.phys(), rhs.param)) + ")";
  }

  // An AVG of the group in hand: whether it is NULL, its sum and its count,
  // as C.
  struct Average {
    std::string is_null, sum, count;
  };
  auto average(const Expr& e) const -> Average {
    const auto& g = *group_;
    auto j = agg_index(g, e);
    if (g.op->streamed) return {"!" + local("xs", g, j), local("x", g, j), local("xc", g, j)};
    auto slot = g.layout.agg_slot[j];
    return {"bc_agg_null(cx, " + g.handle + ", " + std::to_string(slot) + ")",
            std::string{folds_f64(e) ? "bc_agg_f64" : "bc_agg_i64"} + "(cx, " + g.handle + ", " + std::to_string(slot) + ")",
            "bc_agg_i64(cx, " + g.handle + ", " + std::to_string(slot + 1) + ")"};
  }

  // PostgreSQL's float8 average: the sum over the count.
  static auto float_average(const std::string& sum, const std::string& count) -> std::string {
    return sum + " / bc_i64_to_f64(" + count + ")";
  }

  // PostgreSQL's numeric average of an integer or a DECIMAL, its sum at the
  // argument's scale: numeric text.
  static auto num_average(const Expr& e, const std::string& sum, const std::string& count) -> std::string {
    return "bc_num_quotient(cx, " + sum + ", " + count + ", " + std::to_string(e.type.scale) + ")";
  }

  // One output of the group in hand: a key or an aggregate, NULL as NULL. An
  // AVG is its sum over its count: a DOUBLE's float8 quotient, any other's
  // numeric text (bc_num_quotient).
  void emit_group(const Expr& e) {
    const auto& g = *group_;
    if (e.kind == Expr::Kind::Column) {
      if (auto row = held_row(g, e.col)) {
        w_.line("bc_emit_col(cx, " + *row + ", " + w_.col(e.col) + ");");
        return;
      }
    }
    if (e.kind == Expr::Kind::Aggregate && e.fn == Expr::Fn::Avg) {
      auto [is_null, sum, count] = average(e);
      w_.open("if (" + is_null + ")");
      w_.line("bc_emit_null(cx);");
      w_.otherwise();
      if (folds_f64(e)) {
        w_.line("bc_emit_f64(cx, " + float_average(sum, count) + ");");
      } else {
        w_.line("bc_emit_num(cx, " + num_average(e, sum, count) + ");");
      }
      w_.close();
      return;
    }
    auto [is_null, value] = group_value(e);
    auto type = e.kind == Expr::Kind::Column     ? e.col.type
                : e.kind == Expr::Kind::GroupKey ? e.phys()
                : folds_f64(e)                ? PhysType::F64
                : folds_bytes(e)              ? PhysType::Bytes
                                              : PhysType::I64;
    auto emit = "bc_emit_" + suffix(type) + "(cx, " + value + ");";
    if (is_null.empty()) {
      w_.line(emit);
      return;
    }
    w_.open("if (" + is_null + ")");
    w_.line("bc_emit_null(cx);");
    w_.otherwise();
    w_.line(emit);
    w_.close();
  }

  // The part of sort key `k` a column of the row in hand gives, ascending,
  // NULL first: a group key's, or a distinct value's. From a covering scan's
  // entry, its key part, an I64; of a derived relation, its local.
  void sort_key_part(const std::string& k, const ColumnRef& c) {
    const auto& b = binding(c.table);
    if (!b.covering && !b.derived) {
      w_.line("bc_sort_key_col(cx, " + k + ", " + b.row + ", " + w_.col(c) + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
      return;
    }
    auto value = "bc_sort_key_" + key_suffix(c.type) + "(cx, " + k + ", " + read_col(c) + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);";
    if (!c.nullable) {
      w_.line(value);
      return;
    }
    w_.open("if (" + col_null(c) + ")");
    w_.line("bc_sort_key_null(cx, " + k + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
    w_.otherwise();
    w_.line(value);
    w_.close();
  }

  // The part of sort key `sk` a key of the group in hand gives, in its
  // ORDER BY direction and NULL placement.
  void group_sort_part(const physical::SortPart& part) {
    auto dir = std::string{part.desc ? "BC_SORT_DESC" : "BC_SORT_ASC"} + ", " +
               (part.nulls_first ? "BC_SORT_NULLS_FIRST" : "BC_SORT_NULLS_LAST");
    // A key column, or a GroupKey or an aggregate (physical::check).
    const auto* col = std::get_if<ColumnRef>(&part.key);
    if (col != nullptr) {
      if (auto row = held_row(*group_, *col)) {
        w_.line("bc_sort_key_col(cx, sk, " + *row + ", " + w_.col(*col) + ", " + dir + ");");
        return;
      }
    }
    const auto e = col != nullptr ? logical::column(*col, {}) : std::get<physical::ExprTarget>(part.key).expr;
    auto [is_null, value] = group_value(e);
    const auto type = col != nullptr                     ? col->type
                      : e.kind == Expr::Kind::GroupKey ? e.phys()
                      : folds_f64(e)                   ? PhysType::F64
                      : folds_bytes(e)                 ? PhysType::Bytes
                                                       : PhysType::I64;
    auto put = "bc_sort_key_" + key_suffix(type) + "(cx, sk, " + value + ", " + dir + ");";
    if (is_null.empty()) {
      w_.line(put);
      return;
    }
    w_.open("if (" + is_null + ")");
    w_.line("bc_sort_key_null(cx, sk, " + dir + ");");
    w_.otherwise();
    w_.line(put);
    w_.close();
  }

  // The group whose key is `key`, found in its structure, and the rows its
  // held tables hold for it, each in a loop of its own that runs once; then
  // k with it in hand.
  void in_group(Group g, const std::string& key, const Consume& k) {
    g.handle = "g" + g.i;
    w_.line("bc_group " + g.handle + " = bc_agg_group(cx, a" + g.i + ", " + key + ");");
    for (std::size_t j = 0; j < g.layout.held.size(); ++j) {
      w_.line("bc_hash_iter " + local("fq", g, j) + " = bc_hash_find(cx, " + local("f", g, j) + ", " + key + ");");
      w_.line("bc_view " + local("fr", g, j) + " = bc_empty(cx);");
      w_.open("while (bc_hash_next(cx, " + local("fq", g, j) + ", &" + local("fr", g, j) + "))");
    }
    auto outer = std::exchange(group_, &g);
    auto was_loop = std::exchange(loop_, true);
    k();
    loop_ = was_loop;
    group_ = outer;
    for (std::size_t j = 0; j < g.layout.held.size(); ++j) w_.close();
  }

  // The row in hand folded into the group the structure holds for its key,
  // gg<i>: on the group's first row, the held rows and the values of its
  // keys and dependent columns.
  void fold_into_structure(const Group& g) {
    const auto& op = *g.op;
    const auto gb = "gb" + g.i;
    const auto gr = "gr" + g.i;
    const auto gg = "gg" + g.i;
    // The expression keys first: eval prints statements, which must not run
    // inside an open key builder. Each is a part after the columns',
    // ascending, NULLs first; a DOUBLE's -0.0 and NaN key as PostgreSQL's
    // equality groups them (bc_sort_key_f64).
    std::vector<Val> exprs;
    for (const auto& x : op.exprs) exprs.push_back(eval(x));
    w_.line("bc_sort_key " + gb + " = bc_sort_key_begin(cx);");
    for (const auto& k : op.keys) sort_key_part(gb, k);
    for (const auto& v : exprs) {
      auto put = "bc_sort_key_" + key_suffix(v.type) + "(cx, " + gb + ", " + v.value + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);";
      if (v.null.empty()) {
        w_.line(put);
        continue;
      }
      w_.open("if (" + v.null + ")");
      w_.line("bc_sort_key_null(cx, " + gb + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
      w_.otherwise();
      w_.line(put);
      w_.close();
    }
    w_.line("bc_view " + gr + " = bc_sort_key_end(cx, " + gb + ");");
    w_.line("bc_group " + gg + " = bc_agg_group(cx, a" + g.i + ", " + gr + ");");
    auto keyed = !exprs.empty() || std::ranges::any_of(g.layout.key_slot, [](const auto& s) { return s.has_value(); });
    if (!g.layout.held.empty() || keyed) {
      w_.open("if (bc_agg_null(cx, " + gg + ", 0))");
      for (std::size_t j = 0; j < g.layout.held.size(); ++j) {
        w_.line("bc_hash_put(cx, " + local("f", g, j) + ", " + gr + ", " + binding(g.layout.held[j]).row + ");");
      }
      for (std::size_t j = 0; j < g.columns.size(); ++j) {
        const auto& slot = g.layout.key_slot[j];
        if (!slot) continue;
        const auto& c = g.columns[j];
        auto put = "bc_agg_min_i64(cx, " + gg + ", " + std::to_string(*slot) + ", " + read_col(c) + ");";
        w_.line(c.nullable ? "if (!" + col_null(c) + ") " + put : put);
      }
      // An expression key's value, set once: on an empty slot a minimum
      // stores it exactly, -0.0 and NaN included. A NULL leaves it unset.
      for (std::size_t j = 0; j < exprs.size(); ++j) {
        const auto& v = exprs[j];
        auto fold = v.type == PhysType::F64 ? "bc_agg_min_f64" : v.type == PhysType::Bytes ? "bc_agg_min_bytes" : "bc_agg_min_i64";
        auto put = std::string{fold} + "(cx, " + gg + ", " + std::to_string(g.layout.expr_slot[j]) + ", " + v.value + ");";
        w_.line(v.null.empty() ? put : "if (" + is_not(v.null) + ") " + put);
      }
      w_.close();
    }
    w_.line("bc_agg_count(cx, " + gg + ", 0);");
    for (std::size_t j = 0; j < op.aggs.size(); ++j) {
      const auto& a = op.aggs[j];
      if (a.args.empty() || a.distinct) continue;  // COUNT(*): slot 0
      auto v = agg_arg(a);
      if (!v.null.empty()) w_.open("if (" + is_not(v.null) + ")");
      for (const auto& line : structure_fold(g, j, gg, v.value)) w_.line(line);
      if (!v.null.empty()) w_.close();
    }
    fold_distinct(g, gr, [&](std::size_t j, const std::string& v) { return structure_fold(g, j, gg, v); });
  }

  // Whether the aggregate folds doubles: a SUM, AVG, MIN or MAX of a DOUBLE.
  static auto folds_f64(const Expr& a) -> bool {
    return a.fn != Expr::Fn::Count && !a.args.empty() && a.args[0].phys() == PhysType::F64;
  }

  // Whether the aggregate folds bytes: a MIN or MAX of text, held beside
  // its slot (bc_agg_min_bytes), in a stream in its holder (stream).
  static auto folds_bytes(const Expr& a) -> bool {
    return (a.fn == Expr::Fn::Min || a.fn == Expr::Fn::Max) && !a.args.empty() && a.args[0].phys() == PhysType::Bytes;
  }

  // A streamed group's holder's slot of aggregate j, which folds bytes: its
  // place among the group's aggregates that do.
  static auto holder_slot(const Group& g, std::size_t j) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(g.op->aggs.begin(), g.op->aggs.begin() + static_cast<std::ptrdiff_t>(j),
                                                          [](const Expr& a) { return folds_bytes(a); }));
  }

  // The statements folding the row in hand's argument of aggregate j, its
  // value `v`, into its slots of group `gg`.
  auto structure_fold(const Group& g, std::size_t j, const std::string& gg, const std::string& v) const
      -> std::vector<std::string> {
    const auto& a = g.op->aggs[j];
    const auto slot = std::to_string(g.layout.agg_slot[j]);
    const auto at = "(cx, " + gg + ", " + slot + ", " + v + ");";
    const auto f64 = folds_f64(a);
    switch (a.fn) {
      case Expr::Fn::Count: return {"bc_agg_count(cx, " + gg + ", " + slot + ");"};
      case Expr::Fn::Sum: return {std::string{f64 ? "bc_agg_add_f64" : "bc_agg_add_i64"} + at};
      case Expr::Fn::Min: return {std::string{f64 ? "bc_agg_min_f64" : folds_bytes(a) ? "bc_agg_min_bytes" : "bc_agg_min_i64"} + at};
      case Expr::Fn::Max: return {std::string{f64 ? "bc_agg_max_f64" : folds_bytes(a) ? "bc_agg_max_bytes" : "bc_agg_max_i64"} + at};
      case Expr::Fn::Avg:
        return {std::string{f64 ? "bc_agg_add_f64" : "bc_agg_add_i64"} + at,
                "bc_agg_count(cx, " + gg + ", " + std::to_string(g.layout.agg_slot[j] + 1) + ");"};
    }
    std::unreachable();
  }

  // The row in hand's DISTINCT values, each argument of the group's layout
  // once: evaluated once (an expression before any key builder opens, since
  // eval prints statements), a NULL skipped, else keyed by distinct_value
  // and, when new to the group, folded by `fold_of(j, value)` into each
  // DISTINCT aggregate j over that argument.
  void fold_distinct(const Group& g, const std::string& group_key,
                     const std::function<std::vector<std::string>(std::size_t, const std::string&)>& fold_of) {
    const auto& op = *g.op;
    for (std::size_t m = 0; m < g.layout.distinct.size(); ++m) {
      const auto& arg = g.layout.distinct[m];
      auto v = eval(arg);
      std::vector<std::string> folds;
      for (std::size_t j = 0; j < op.aggs.size(); ++j) {
        if (!op.aggs[j].distinct || op.aggs[j].args[0] != arg) continue;
        for (auto& line : fold_of(j, v.value)) folds.push_back(std::move(line));
      }
      if (!v.null.empty()) w_.open("if (" + is_not(v.null) + ")");
      distinct_value(g, m, v, group_key, folds);
      if (!v.null.empty()) w_.close();
    }
  }

  // The aggregates of DISTINCT values over argument m in the group in hand:
  // its value `v`, keyed in the argument's structure d<i>_<m>, a column by
  // its row's part, an expression by its value (bc_sort_key_f64 keys a
  // DOUBLE's -0.0 and NaN as PostgreSQL's equality has them); in a
  // structure after the group's key `group_key`, streamed alone, since the
  // stream clears d<i>_<m> as each group ends. `folds` run when the value is
  // new to the group, all the aggregates over the argument at once. The
  // structure grows by a key per distinct value per group and is charged to
  // the statement's memory budget like a group: past it the statement fails
  // with 54000.
  void distinct_value(const Group& g, std::size_t m, const Val& v, const std::string& group_key,
                      const std::vector<std::string>& folds) {
    const auto& arg = g.layout.distinct[m];
    const auto dk = local("dk", g, m);
    const auto dg = local("dg", g, m);
    w_.line("bc_sort_key " + dk + " = bc_sort_key_begin(cx);");
    if (!group_key.empty()) w_.line("bc_sort_key_bytes(cx, " + dk + ", " + group_key + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
    if (arg.kind == Expr::Kind::Column) {
      sort_key_part(dk, arg.col);
    } else {
      w_.line("bc_sort_key_" + key_suffix(v.type) + "(cx, " + dk + ", " + v.value + ", BC_SORT_ASC, BC_SORT_NULLS_FIRST);");
    }
    w_.line("bc_group " + dg + " = bc_agg_group(cx, " + local("d", g, m) + ", bc_sort_key_end(cx, " + dk + "));");
    w_.open("if (bc_agg_null(cx, " + dg + ", 0))");
    w_.line("bc_agg_count(cx, " + dg + ", 0);");
    for (const auto& f : folds) w_.line(f);
    w_.close();
  }

  // The structures a GroupAggregate opens: its distinct values', and, in a
  // structure, the groups' and the held rows' hash tables.
  void open_structures(const Group& g) {
    if (!g.op->streamed) {
      w_.line("bc_agg a" + g.i + " = bc_agg_open(cx, " + std::to_string(g.layout.slots) + ");");
      for (std::size_t j = 0; j < g.layout.held.size(); ++j) {
        w_.line("bc_hash " + local("f", g, j) + " = bc_hash_open(cx);  // each group's first row of " +
                w_.tbl(g.layout.held[j]));
      }
    }
    for (std::size_t m = 0; m < g.layout.distinct.size(); ++m) {
      w_.line("bc_agg " + local("d", g, m) + " = bc_agg_open(cx, 1);  // DISTINCT values seen per group");
    }
  }

  // A pipeline breaker (docs/planner_design.md, *The physical plan and
  // codegen*): every input row folded into its group in structure a<i>,
  // the input's loop in a block of its own; then a loop over the groups in
  // the order they first appeared, each found by its key, gk<i>. A LIMIT's
  // counter is that loop's, never the input's. Streamed, the groups are
  // given one at a time (stream).
  void produce(const physical::GroupAggregate& op, const Consume& k) {
    const auto columns = op.columns();
    Group g{&op, columns, physical::group_layout(columns, op.aggs, op.exprs), std::to_string(groups_++), {}};
    open_structures(g);
    if (op.streamed) {
      stream(g, k);
      return;
    }
    auto limit = std::exchange(limit_, "");
    if (!std::holds_alternative<physical::Empty>(op.input->op)) {
      w_.open("");
      produce(*op.input, [&] { fold_into_structure(g); });
      w_.close();
    }
    limit_ = limit;
    const auto gk = "gk" + g.i;
    w_.line("bc_view " + gk + " = bc_empty(cx);");
    w_.open("while (" + guard() + "bc_agg_next(cx, a" + g.i + ", &" + gk + ", 0))");
    in_group(g, gk, k);
    w_.close();
  }

  // The groups streamed: the input's rows come with each group's together,
  // so the group is kept in locals, given (k) inside the input's loop when a
  // row of another group starts the next, and after it for the last. Each
  // dependent column is read from every row and kept as a key is: every row
  // of a group holds the same value. An aggregate's distinct values are cleared as
  // each group ends: the stream holds only the group in hand's. With no key,
  // one group, given after the input, over no rows too. A LIMIT's counter is
  // the input loop's, and the last group's.
  void stream(const Group& g, const Consume& k) {
    const auto& op = *g.op;
    const auto st = "st" + g.i;
    if (!op.keys.empty()) w_.line("bool " + st + " = false;");
    for (std::size_t j = 0; j < g.columns.size(); ++j) {
      const auto& c = g.columns[j];
      w_.line(c.type == PhysType::Bool ? "bool " + local("kv", g, j) + " = false;" : "int64_t " + local("kv", g, j) + " = 0;");
      if (c.nullable) w_.line("bool " + local("kn", g, j) + " = false;");
    }
    // MIN and MAX of text are held in holder h<i>, one group under the
    // empty key, hg<i>, cleared as each group ends: a local could not keep
    // their bytes past the row that gave them.
    const auto held = static_cast<std::size_t>(std::ranges::count_if(op.aggs, [](const Expr& a) { return folds_bytes(a); }));
    const auto h = local("h", g, 0);
    const auto hg = local("hg", g, 0);
    if (held > 0) {
      w_.line("bc_agg " + h + " = bc_agg_open(cx, " + std::to_string(held) + ");  // MIN and MAX of text of the group in hand");
      w_.line("bc_group " + hg + " = bc_agg_group(cx, " + h + ", bc_empty(cx));");
    }
    auto reset = [&](bool declare) {
      for (std::size_t j = 0; j < op.aggs.size(); ++j) {
        const auto& a = op.aggs[j];
        if (folds_bytes(a)) continue;
        w_.line((declare ? (folds_f64(a) ? "double " : "int64_t ") : "") + local("x", g, j) + " = 0;");
        if (a.fn != Expr::Fn::Count) w_.line((declare ? "bool " : "") + local("xs", g, j) + " = false;");
        if (a.fn == Expr::Fn::Avg) w_.line((declare ? "int64_t " : "") + local("xc", g, j) + " = 0;");
      }
    };
    reset(true);
    Group in = g;
    auto give = [&] {
      auto outer = std::exchange(group_, &in);
      k();
      group_ = outer;
    };
    if (!std::holds_alternative<physical::Empty>(op.input->op)) {
      produce(*op.input, [&] {
        if (!op.keys.empty()) {
          // The row's keys and dependent columns, then whether its keys
          // start another group.
          std::string changed;
          for (std::size_t j = 0; j < g.columns.size(); ++j) {
            const auto& c = g.columns[j];
            const auto rk = local("rk", g, j);
            const auto rn = local("rn", g, j);
            const auto kv = local("kv", g, j);
            const auto kn = local("kn", g, j);
            auto type = c.type == PhysType::Bool ? std::string{"bool "} : std::string{"int64_t "};
            if (c.nullable) {
              w_.line("bool " + rn + " = " + col_null(c) + ";");
              w_.line(type + rk + " = " + (c.type == PhysType::Bool ? "false" : "0") + ";");
              w_.line("if (!" + rn + ") " + rk + " = " + read_col(c) + ";");
            } else {
              w_.line(type + rk + " = " + read_col(c) + ";");
            }
            if (j >= op.keys.size()) continue;
            auto differs = c.type == PhysType::Bool ? "(" + rk + " && !" + kv + ") || (" + kv + " && !" + rk + ")"
                                                    : rk + " != " + kv;
            changed += (changed.empty() ? "" : " || ") + differs;
            if (c.nullable) changed += " || (" + rn + " && !" + kn + ") || (" + kn + " && !" + rn + ")";
          }
          w_.open("if (" + st + " && (" + changed + "))");
          give();
          reset(false);
          // The group's distinct values go with it.
          for (std::size_t m = 0; m < g.layout.distinct.size(); ++m) w_.line("bc_agg_clear(cx, " + local("d", g, m) + ");");
          if (held > 0) {
            w_.line("bc_agg_clear(cx, " + h + ");");
            w_.line(hg + " = bc_agg_group(cx, " + h + ", bc_empty(cx));");
          }
          w_.close();
          for (std::size_t j = 0; j < g.columns.size(); ++j) {
            w_.line(local("kv", g, j) + " = " + local("rk", g, j) + ";");
            if (g.columns[j].nullable) w_.line(local("kn", g, j) + " = " + local("rn", g, j) + ";");
          }
          w_.line(st + " = true;");
        }
        fold_into_locals(g);
      });
    }
    // The last group, or the one group of no key.
    std::string cond = op.keys.empty() ? limit_ : (limit_.empty() ? st : limit_ + " && " + st);
    if (cond.empty()) {
      give();
      return;
    }
    w_.open("if (" + cond + ")");
    give();
    w_.close();
  }

  // The statements folding the row in hand's argument of aggregate j, its
  // value `v`, into the streamed group's locals: x<j> the count, sum,
  // minimum or maximum, xs<j> whether a value was folded, xc<j> an AVG's
  // count.
  static auto local_fold(const Group& g, std::size_t j, const std::string& v) -> std::vector<std::string> {
    const auto& a = g.op->aggs[j];
    if (folds_bytes(a)) {
      return {std::string{a.fn == Expr::Fn::Min ? "bc_agg_min_bytes" : "bc_agg_max_bytes"} + "(cx, " + local("hg", g, 0) + ", " +
              std::to_string(holder_slot(g, j)) + ", " + v + ");"};
    }
    const auto x = local("x", g, j);
    const auto xs = local("xs", g, j);
    const auto f64 = folds_f64(a);
    auto sum = x + " = " + (f64 ? "bc_add_f64" : "bc_add") + "(cx, " + x + ", " + v + ");";
    switch (a.fn) {
      case Expr::Fn::Count: return {"++" + x + ";"};
      case Expr::Fn::Sum: return {sum, xs + " = true;"};
      case Expr::Fn::Min:
      case Expr::Fn::Max: {
        const auto lv = local("v", g, j);
        if (f64) {
          auto pick = std::string{a.fn == Expr::Fn::Min ? "bc_min_f64" : "bc_max_f64"};
          return {"double " + lv + " = " + v + ";", x + " = " + xs + " ? " + pick + "(" + x + ", " + lv + ") : " + lv + ";",
                  xs + " = true;"};
        }
        return {"int64_t " + lv + " = " + v + ";",
                "if (!" + xs + " || " + lv + (a.fn == Expr::Fn::Min ? " < " : " > ") + x + ") " + x + " = " + lv + ";",
                xs + " = true;"};
      }
      case Expr::Fn::Avg: return {sum, xs + " = true;", "++" + local("xc", g, j) + ";"};
    }
    std::unreachable();
  }

  // The row in hand folded into the streamed group's locals. NULLs are
  // skipped.
  void fold_into_locals(const Group& g) {
    const auto& op = *g.op;
    // A covering scan's entry is bound once for the row, as arg_of_row binds
    // it, however many reads of its arguments follow.
    auto covered = std::ranges::find_if(rows_, [&](const Binding& b) {
      return b.covering &&
             std::ranges::any_of(op.aggs, [&](const Expr& a) {
               std::vector<ColumnRef> cols;
               if (!a.args.empty()) logical::columns_of(a.args[0], cols);
               return std::ranges::any_of(cols, [&](const ColumnRef& c) { return c.table == b.table; });
             });
    });
    if (covered != rows_.end()) {
      w_.line("bc_view key = bc_scan_key(cx, " + covered->cursor + ");");
      keyed_ = &*covered;
    }
    for (std::size_t j = 0; j < op.aggs.size(); ++j) {
      const auto& a = op.aggs[j];
      if (a.args.empty()) {
        w_.line("++" + local("x", g, j) + ";");  // COUNT(*)
        continue;
      }
      if (a.distinct) continue;
      auto v = agg_arg(a);
      if (!v.null.empty()) w_.open("if (" + is_not(v.null) + ")");
      for (const auto& line : local_fold(g, j, v.value)) w_.line(line);
      if (!v.null.empty()) w_.close();
    }
    fold_distinct(g, "", [&](std::size_t j, const std::string& v) { return local_fold(g, j, v); });
    keyed_ = nullptr;
  }

  // A sort of a structure's groups: each group that passes its HAVING added
  // to sorter s<i> by its keys, holding the group's key; then a loop over
  // the sorter, each group found again by the key held, p<i>. A LIMIT's
  // counter is the sorter loop's.
  void sort_groups(const physical::Sort& s, const Consume& k) {
    const auto i = std::to_string(sorts_++);
    const auto sorter = "s" + i;
    const auto held = "p" + i;
    w_.line("bc_sorter " + sorter + " = bc_sorter_open(cx, " + std::to_string(s.keep) + ");");
    auto limit = std::exchange(limit_, "");
    std::optional<Group> sorted;
    produce(*s.input, [&] {
      w_.line("bc_sort_key sk = bc_sort_key_begin(cx);");
      for (const auto& part : s.keys) group_sort_part(part);
      w_.line("bc_sorter_add(cx, " + sorter + ", bc_sort_key_end(cx, sk), gk" + group_->i + ");");
      sorted = *group_;
    });
    limit_ = limit;
    w_.line("bc_view " + held + " = bc_empty(cx);");
    w_.open("while (" + guard() + "bc_sorter_next(cx, " + sorter + ", &" + held + "))");
    in_group(*sorted, held, k);
    w_.close();
  }

  // The rows of the groups: each output, from the group in hand.
  void produce(const physical::GroupEmit& e, const Consume&) {
    produce(*e.input, [&] {
      w_.line("bc_emit_begin(cx);");
      for (const auto& o : e.outputs) emit_group(o);
      w_.line("bc_emit_end(cx);");
    });
  }

  // A derived relation's rows (docs/planner_design.md, *Derived
  // relations*): its query's plan printed in place, a block of its own, and
  // for each group it gives, where a GroupEmit would emit it, each output
  // bound to the locals d<i>_v<j> and d<i>_n<j> (its NULL, its value the
  // type's zero then); then k, the plan above, on them, inside `do { … }
  // while (0)` so a miss (`continue`) leaves it for the next group, wherever
  // the query gives the group: in its loop over groups, in its input's loop,
  // or after it. The statement's LIMIT counts the rows or groups above,
  // never the query's: limit_ is reset to the arm's own around it, and the
  // plan above runs for a group only while the LIMIT is not met. A join of
  // the relation (its rows the plan's outermost loop) counts its pairs so,
  // a lookup by key per group included, which no loop of its own stops.
  void produce(const physical::Derived& d, const Consume& k) {
    const auto& e = std::get<physical::GroupEmit>(d.input->op);
    const auto name = "d" + std::to_string(derived_++);
    auto limit = std::exchange(limit_, base_limit_);
    w_.open("");
    produce(*e.input, [&] {
      for (std::size_t j = 0; j < e.outputs.size(); ++j) {
        const auto& o = e.outputs[j];
        const auto n = name + "_n" + std::to_string(j);
        const auto v = name + "_v" + std::to_string(j);
        const auto type = d.columns[j].type;
        std::string is_null, value;
        if (o.kind == Expr::Kind::Aggregate && o.fn == Expr::Fn::Avg) {
          auto [avg_null, sum, count] = average(o);  // of a DOUBLE: physical::check
          is_null = avg_null;
          value = float_average(sum, count);
        } else {
          std::tie(is_null, value) = group_value(o);
        }
        w_.line("bool " + n + " = " + (is_null.empty() ? "false" : is_null) + ";");
        w_.line(declared(type, v));
        w_.line("if (!" + n + ") " + v + " = " + value + ";");
      }
      rows_.push_back({d.table, name, false, 0, false, "c", true, true});
      auto was_loop = std::exchange(loop_, true);
      auto outer = std::exchange(limit_, limit);
      // The statement's LIMIT, met, leaves the groups after it unread by
      // the plan above, a lookup by key as much as a loop.
      if (!limit.empty()) w_.open("if (" + limit + ")");
      w_.open("do");
      k();
      w_.close(" while (0);");
      if (!limit.empty()) w_.close();
      limit_ = outer;
      loop_ = was_loop;
      rows_.pop_back();
    });
    w_.close();
    limit_ = limit;
  }

  // What a GroupEmit gives for no rows: no group with a key; with none, the
  // one group's row over no rows (COUNT 0, every other aggregate NULL)
  // where its HAVING holds of it and its LIMIT is not 0.
  static auto empty_group(const physical::GroupEmit& e) -> std::vector<std::string> {
    const auto* n = e.input.get();
    if (const auto* l = std::get_if<physical::Limit>(&n->op)) {
      if (l->n == 0 || l->offset > 0) return {};  // the one row is not emitted, or is skipped
      n = l->input.get();
    }
    const auto* g = physical::group_below(*n);
    if (g == nullptr || !g->keyless()) return {};
    std::string cond;
    if (const auto* f = std::get_if<physical::Filter>(&n->op)) {
      for (const auto& c : f->conjuncts) {
        // Only a COUNT is not NULL over no rows.
        if (c.args[0].fn != Expr::Fn::Count) return {};
        const auto p = c.args[1].param;
        cond += (cond.empty() ? "" : " && ") + std::string{"!"} + param_null(p) + " && INT64_C(0) " + op_text(c.op) + " " +
                param(PhysType::I64, p);
      }
    }
    auto row = empty_row(e.outputs);
    if (cond.empty()) return row;
    std::vector<std::string> out{"if (" + cond + ") {"};
    for (const auto& r : row) out.push_back("  " + r);
    out.emplace_back("}");
    return out;
  }

  // Ends the row `r` and gives what the write takes as its row. With
  // CHECK constraints the row is first tested: each is evaluated over the
  // row as written, and one that is false (not NULL) raises 23514 before
  // anything is written, so a refused row leaves no index entry.
  // With `keep` the row stays in hand as `nr` after it is written, for RETURNING.
  auto checked_row(std::uint32_t table, const std::vector<Expr>& checks, bool keep) -> std::string {
    if (checks.empty() && !keep) return "bc_row_end(cx, r)";
    w_.line("bc_view nr = bc_row_end(cx, r);");
    auto outer = std::exchange(rows_, std::vector<Binding>{{table, "nr"}});
    for (const auto& c : checks) {
      auto v = eval(c);
      auto unknown = v.null.empty() ? std::string{} : "!(" + v.null + ") && ";
      w_.line("if (" + unknown + "!(" + v.value + ")) bc_raise(cx, BC_C_CHECK, " + std::to_string(table) + ");");
    }
    rows_ = std::move(outer);
    return "nr";
  }

  // Writes the row `r` with `call` + the row + ");", checked as checked_row says.
  void write_checked(std::uint32_t table, const std::vector<Expr>& checks, const std::string& call, bool keep = false) {
    w_.line(call + checked_row(table, checks, keep) + ");");
  }

  // Inserts the row `r` and gives its RETURNING row; with ON CONFLICT DO
  // NOTHING's arbiter, only when the row is written, counting it in
  // `count`. NOT NULL (bc_row_end) and CHECK are tested first, as
  // PostgreSQL tests them before the arbiter; their error is sticky, and
  // bc_insert_on_conflict then writes nothing. With DO UPDATE's `update`,
  // the row that holds the key is read (bc_insert_or_existing) and updated
  // in its place as an UPDATE updates it, its CHECK tested again, and its
  // RETURNING row is the row as updated; each row proposed is counted.
  //
  // A check option's conjuncts (`view_checks`, never with an arbiter) are
  // evaluated over the row once bc_insert has written it, so its unique
  // keys are checked first, as PostgreSQL checks them, and before its
  // RETURNING row is emitted: one false or NULL raises BC_C_CHECK_OPTION
  // with its view's id and the row.
  void insert_row(std::uint32_t table, const std::vector<Expr>& checks, const std::vector<logical::ViewCheck>& view_checks,
                  const std::vector<Expr>& returning, std::optional<std::uint32_t> arbiter, const std::string& count,
                  const std::vector<std::pair<ColumnRef, Expr>>* update = nullptr) {
    if (arbiter && !view_checks.empty()) throw std::logic_error{"physical plan: a check option on an insert with an arbiter"};
    if (!arbiter) {
      write_checked(table, checks, "bc_insert(cx, " + w_.tbl(table) + ", ", !returning.empty() || !view_checks.empty());
      if (!view_checks.empty()) {
        auto outer = std::exchange(rows_, std::vector<Binding>{{table, "nr"}});
        for (const auto& c : view_checks) {
          auto v = eval(c.where);
          auto fails = v.null.empty() ? "!(" + v.value + ")" : "(" + v.null + ") || !(" + v.value + ")";
          w_.line("if (" + fails + ") bc_raise_row(cx, BC_C_CHECK_OPTION, " + std::to_string(c.view) + ", " + w_.tbl(table) +
                  ", nr);");
        }
        rows_ = std::move(outer);
      }
      returned(table, "nr", returning);
      if (!count.empty()) w_.line("++" + count + ";");
      return;
    }
    auto row = checked_row(table, checks, !returning.empty());
    auto by = *arbiter == logical::kAnyArbiter ? std::string{"BC_ARBITER_ANY"} : w_.obj(*arbiter);
    if (update == nullptr) {
      w_.open("if (bc_insert_on_conflict(cx, " + w_.tbl(table) + ", " + row + ", " + by + "))");
      returned(table, "nr", returning);
      w_.line("++" + count + ";");
      w_.close();
      return;
    }
    w_.reads(table);
    w_.line("bc_view old = bc_empty(cx);");
    w_.open("if (bc_insert_or_existing(cx, " + w_.tbl(table) + ", " + row + ", " + by + ", &old))");
    returned(table, "nr", returning);
    w_.otherwise();
    // An inner block: its `r` and `nr` are the updated row's.
    w_.line("bc_row r = bc_row_from(cx, " + w_.tbl(table) + ", old);");
    auto outer = std::exchange(rows_, std::vector<Binding>{{table, "old"}});
    for (const auto& [col, e] : *update) set(col, e);
    rows_ = std::move(outer);
    write_checked(table, checks, "bc_update(cx, " + w_.tbl(table) + ", old, ", !returning.empty());
    returned(table, "nr", returning);
    w_.close();
    w_.line("++" + count + ";");
  }

  void produce(const physical::Insert& s, const Consume&) {
    // Each row in a block of its own, in order. bc_row_begin sets every
    // column to its default; a column not given keeps it. The runtime checks
    // the keys and writes the index entries; an error it raises is sticky,
    // so the rows after one that fails are not written.
    // With ON CONFLICT DO NOTHING the count is the rows written.
    // With DO UPDATE it is the rows inserted or updated.
    const std::string count = s.arbiter ? "inserted" : "";
    if (s.arbiter) w_.line("int64_t " + count + " = 0;");
    const auto blocks = s.rows.size() > 1;
    for (std::size_t i = 0; i < s.rows.size(); ++i) {
      if (blocks) w_.open("");
      w_.line("bc_row r = bc_row_begin(cx, " + w_.tbl(s.table) + ");");
      for (const auto& [col, e] : s.rows[i]) {
        w_.line("bc_row_set_param(cx, r, " + w_.col(col) + ", " + std::to_string(e.param) + ");");
      }
      insert_row(s.table, s.checks, s.view_checks, s.returning, s.arbiter, count, s.update.empty() ? nullptr : &s.update[i]);
      if (blocks) w_.close();
    }
    w_.line("bc_emit_count(cx, " + (s.arbiter ? count : std::to_string(s.rows.size())) + ");");
  }

  // INSERT … SELECT: the query's Emit, whose rows go to a sink that writes
  // each as a row of the table, in a block of its own, as an INSERT of it
  // alone would be; then the count of rows written. Where a row is missing
  // the rest is in an `if` (miss), closed after the query, as an arm's.
  void produce(const physical::InsertFrom& s, const Consume&) {
    const std::string count = "inserted";
    w_.line("int64_t " + count + " = 0;");
    const Sink sink = [&](Writer& w, const Row& row) {
      w.open("");
      w.line("bc_row r = bc_row_begin(cx, " + w.tbl(s.table) + ");");
      for (std::size_t i = 0; i < s.columns.size(); ++i) store(w, s.columns[i].column, row[i]);
      for (const auto& [col, e] : s.fixed) {
        w.line("bc_row_set_param(cx, r, " + w.col(col) + ", " + std::to_string(e.param) + ");");
      }
      insert_row(s.table, s.checks, s.view_checks, s.returning, s.arbiter, count);
      w.close();
    };
    sink_ = &sink;
    // The count below is what no rows report: an Empty source prints nothing.
    auto none = std::exchange(no_rows_, {});
    produce(*s.input, [] {});
    no_rows_ = std::move(none);
    for (; open_ > 0; --open_) w_.close();
    sink_ = nullptr;
    w_.line("bc_emit_count(cx, " + count + ");");
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
      write_checked(s.table, s.checks, "bc_update(cx, " + w_.tbl(s.table) + ", old, ", !s.returning.empty());
      returned(s.table, "nr", s.returning);
    });
  }

  void produce(const physical::Delete& s, const Consume&) {
    write_rows(s.table, *s.input, [&] {
      w_.line("bc_delete(cx, " + w_.tbl(s.table) + ", old);");
      returned(s.table, "old", s.returning);
    });
  }

  // RETURNING: a result row of `list` over `row`, the row of `table` just
  // written (or, for a DELETE, as it was). Each value is evaluated before
  // the row begins, as an Emit's are.
  void returned(std::uint32_t table, const std::string& row, const std::vector<Expr>& list) {
    if (list.empty()) return;
    auto outer = std::exchange(rows_, std::vector<Binding>{{table, row}});
    std::vector<Val> values;
    for (const auto& e : list) values.push_back(eval(e));
    w_.line("bc_emit_begin(cx);");
    for (const auto& v : values) emit_val(v);
    w_.line("bc_emit_end(cx);");
    rows_ = std::move(outer);
  }

  // SET col = e, from the row as read (`old`): a parameter as given, NULL
  // included; any other value evaluated and stored (store). A range error
  // its arithmetic or casts raise names the updated table, as the row
  // builder's does.
  void set(const ColumnRef& col, const Expr& e) {
    if (e.kind == Expr::Kind::Param) {
      w_.line("bc_row_set_param(cx, r, " + w_.col(col) + ", " + std::to_string(e.param) + ");");
      return;
    }
    auto rows = std::exchange(rows_, std::vector<Binding>{{col.table, "old"}});
    auto object = std::exchange(range_object_, w_.tbl(col.table));
    auto v = eval(e);
    range_object_ = std::move(object);
    rows_ = std::move(rows);
    store(w_, col, v);
  }

  // A value into column `col` of the row `r` being built: NULL as NULL, else
  // by its physical type, which is the column's (physical::assigns); the row
  // builder copies it and checks its range and length.
  static void store(Writer& w, const ColumnRef& col, const Val& v) {
    if (v.type != col.type) throw std::logic_error{"physical plan: a value stored into a column held as another type"};
    auto line = "bc_row_set_" + carry_suffix(col.type) + "(cx, r, " + w.col(col) + ", " + v.value + ");";
    if (v.null.empty()) {
      w.line(line);
      return;
    }
    w.open("if (" + v.null + ")");
    w.line("bc_row_set_null(cx, r, " + w.col(col) + ");");
    w.otherwise();
    w.line(line);
    w.close();
  }

  // The loop condition's prefix a LIMIT makes: limit_ and " && ", or empty.
  [[nodiscard]] auto guard() const -> std::string { return limit_.empty() ? std::string{} : limit_ + " && "; }

  Writer& w_;
  std::vector<std::string> no_rows_;
  std::string row_name_{"row"};  // the row a leaf reads into: "old" for a write
  std::vector<Binding> rows_;    // the rows in hand, outer first
  const Binding* keyed_{nullptr};  // the covering scan whose entry the row has bound as `key`, or null
  bool loop_{false};             // inside a loop: a miss is `continue`
  std::string limit_;            // a LIMIT's condition, n < its count, or empty
  const Sink* sink_{nullptr};    // an arm of a set operation: where its result rows go
  std::string range_object_{"0"};  // the object a range error eval prints names: SET's table, else none
  std::string base_limit_;       // an arm's: the statement's LIMIT, which every loop of the arm also stops at
  int open_{0};                  // an arm's: the `if` blocks its misses opened
  int exprs_{0};                 // the locals the expressions over columns printed: v<i>, n<i>, d<i>
  std::string* hit_{nullptr};    // a one-row aggregate: a miss joins this condition instead
  int choices_{0};               // the choice points printed: each counts into count<i>, over lo<i> and hi<i>
  int sorts_{0};                 // the sorts printed: each into sorter s<i>, its row p<i>
  int hashes_{0};                // the hash joins printed: each into table h<i>, its row b<i>
  int nulls_{0};                 // the outer joins printed: each's null row nr<i>
  int groups_{0};                // the grouped aggregates printed: each into a<i>, its group g<i>
  int derived_{0};               // the derived relations printed: each group's outputs in d<i>_v<j>, d<i>_n<j>
  const Group* group_{nullptr};  // the group in hand, for a grouped aggregate's consumer
  std::string cursor_{"c"};      // the cursor a scan opens
  // The columns a sort below the rows in hand tested for NULL before it added
  // their row: they hold none.
  std::vector<ColumnRef> not_null_;
  // Each choice point's first plan's scan, and the counted range, lo<i> and
  // hi<i>, it opens on.
  std::map<const physical::Scan*, Counted> counted_;
};

// Prints a set operation (docs/planner_design.md, *Set operations*). Each
// arm is printed as its own SELECT, in a block of the plan's one function,
// and the rows it gives go to a sink that does the operation's part: they
// are not collected, so the plan holds only what telling duplicates apart
// needs, a bc_agg of the rows' keys (a sort key of every column, NULL equal
// to NULL) with a count or two per key. UNION ALL holds nothing. UNION
// streams both arms through a set of the keys seen. INTERSECT and EXCEPT
// stream the arm whose rows can be returned, after the other's keys are in
// the aggregate with how many rows had each; a row is returned while the
// count of the rows it matched is not used up (ALL), or once (DISTINCT).
class SetGen {
public:
  SetGen(Writer& w, const physical::Plan& p) : w_{w}, p_{p} {}

  void plan() {
    Gen{w_}.subqueries();
    const auto& limit = p_.set_limit;
    if (limit && *limit == 0) return;  // LIMIT 0 returns nothing, so nothing is read
    if (!p_.set_order.empty()) {
      ordered(limit.value_or(0));
      return;
    }
    if (limit) {
      stop_ = "setcount < " + std::to_string(*limit);
      w_.line("int64_t setcount = 0;");
    }
    stream(*p_.set, [&](Writer& w, const Row& row) {
      if (limit) w.open("if (" + stop_ + ")");
      emit(w, row, p_.result.size());
      if (limit) {
        w.line("setcount = bc_add(cx, setcount, INT64_C(1));");
        w.close();
      }
    }, true);
  }

private:
  using Row = Gen::Row;
  using Sink = Gen::Sink;
  using Val = Gen::Val;

  // ORDER BY of the combined rows. Every row the arms give is added to one
  // sorter, as it is in hand: keyed by the ORDER BY columns, each with its
  // direction and NULL placement, its payload a sort key of all the columns,
  // each ascending, which the loop after the arms reads back part by part
  // (bc_sort_key_part_*). The payload only carries, so a NUMERIC rides in it
  // as bytes; only an ORDER BY key part throws for one. A LIMIT is the sorter's `keep`: it holds the first n
  // in order and no more, and every arm is read whole. Without one the sorter
  // holds every row, charged to the statement's budget.
  void ordered(std::int64_t keep) {
    const auto sorter = fresh("s");
    w_.line("bc_sorter " + sorter + " = bc_sorter_open(cx, " + std::to_string(keep) + ");");
    stream(*p_.set, [this, sorter](Writer& w, const Row& row) { add_to_sorter(w, sorter, row); }, false);
    const auto held = fresh("p");
    w_.line("bc_view " + held + " = bc_empty(cx);");
    w_.open("while (bc_sorter_next(cx, " + sorter + ", &" + held + "))");
    Row row;
    int part = 0;
    for (const auto& t : p_.set->types) {
      const auto type = phys_type(t);
      const auto n = fresh("n");
      const auto x = fresh("x");
      const auto at = std::to_string(part++);
      w_.line("bool " + n + " = bc_sort_key_part_null(cx, " + held + ", " + at + ");");
      w_.line(declared(type, x));
      // A jsonb rides as its text and is read back by jsonb_in: the same value.
      if (type == PhysType::Jsonb) {
        w_.line("if (!" + n + ") " + x + " = bc_jsonb_in(cx, bc_sort_key_part_bytes(cx, " + held + ", " + at + "));");
      } else {
        w_.line("if (!" + n + ") " + x + " = bc_sort_key_part_" + carry_suffix(type) + "(cx, " + held + ", " + at + ");");
      }
      if (type == PhysType::F64) {
        const auto neg = std::to_string(part++);
        w_.line("if (bc_sort_key_part_bool(cx, " + held + ", " + neg + ")) " + x + " *= -1.0;");
      }
      row.push_back({n, x, type});
    }
    emit(w_, row, p_.result.size());
    w_.close();
  }

  void add_to_sorter(Writer& w, const std::string& sorter, const Row& row) {
    if (row.size() != p_.set->types.size()) {
      throw std::logic_error{"physical plan: a set operation row of other columns than its types"};
    }
    auto part = [&](const std::string& key, const Val& v, const std::string& placed, const std::string& fn) {
      w.open("if (" + v.null + ")");
      w.line("bc_sort_key_null(cx, " + key + ", " + placed + ");");
      w.otherwise();
      w.line("bc_sort_key_" + fn + "(cx, " + key + ", " + v.value + ", " + placed + ");");
      w.close();
    };
    const auto k = fresh("k");
    w.line("bc_sort_key " + k + " = bc_sort_key_begin(cx);");
    for (const auto& o : p_.set_order) {
      part(k, row[o.column],
           std::string{o.desc ? "BC_SORT_DESC" : "BC_SORT_ASC"} + ", " +
               (o.nulls_first ? "BC_SORT_NULLS_FIRST" : "BC_SORT_NULLS_LAST"),
           key_suffix(row[o.column].type));
    }
    const auto key = fresh("v");
    w.line("bc_view " + key + " = bc_sort_key_end(cx, " + k + ");");
    const auto d = fresh("k");
    w.line("bc_sort_key " + d + " = bc_sort_key_begin(cx);");
    for (const auto& v : row) {
      if (v.type == PhysType::Jsonb) {
        part(d, Val{v.null, "bc_jsonb_text(cx, " + v.value + ")", PhysType::Bytes}, "BC_SORT_ASC, BC_SORT_NULLS_LAST", "bytes");
      } else {
        part(d, v, "BC_SORT_ASC, BC_SORT_NULLS_LAST", carry_suffix(v.type));
      }
      // The key holds -0.0 as 0.0; the sign of a zero rides in a part of its own.
      if (v.type == PhysType::F64) {
        w.line("bc_sort_key_bool(cx, " + d + ", !" + v.null + " && " + v.value + " == 0.0 && 1.0 / " + v.value +
               " < 0.0, BC_SORT_ASC, BC_SORT_NULLS_LAST);");
      }
    }
    w.line("bc_sorter_add(cx, " + sorter + ", " + key + ", bc_sort_key_end(cx, " + d + "));");
  }

  auto fresh(const std::string& what) -> std::string { return "set" + what + std::to_string(ids_++); }

  // The row's first `returned` columns: the rest are the hidden ORDER BY
  // keys of a FULL JOIN (logical::SetOp::hidden).
  static void emit(Writer& w, const Row& row, std::size_t returned) {
    w.line("bc_emit_begin(cx);");
    for (std::size_t i = 0; i < returned; ++i) {
      const auto& v = row.at(i);
      w.open("if (" + v.null + ")");
      w.line("bc_emit_null(cx);");
      w.otherwise();
      w.line("bc_emit_" + suffix(v.type) + "(cx, " + v.value + ");");
      w.close();
    }
    w.line("bc_emit_end(cx);");
  }

  // The arm's values in locals, a NULL one's value the type's zero: what
  // the rest reads more than once, and where it is not NULL.
  Sink held(Sink next) {
    return [this, into = std::move(next)](Writer& w, const Row& row) {
      Row out;
      for (const auto& v : row) {
        auto n = fresh("n");
        auto x = fresh("x");
        w.line("bool " + n + " = " + v.null + ";");
        w.line(declared(v.type, x));
        w.line("if (!" + n + ") " + x + " = " + v.value + ";");
        out.push_back({n, x, v.type});
      }
      into(w, out);
    };
  }

  // The arm's rows as the operation's columns: an integer type meeting a
  // floating one is converted to it.
  Sink converted(const std::vector<SqlType>& from_types, const std::vector<SqlType>& to_types, Sink next) {
    auto same = true;
    for (std::size_t i = 0; i < to_types.size(); ++i) same = same && phys_type(from_types[i]) == phys_type(to_types[i]);
    if (same) return next;
    return [this, from = from_types, to = to_types, into = std::move(next)](Writer& w, const Row& row) {
      Row out = row;
      for (std::size_t i = 0; i < to.size(); ++i) {
        if (phys_type(from[i]) == phys_type(to[i])) continue;
        if (phys_type(from[i]) != PhysType::I64 || phys_type(to[i]) != PhysType::F64) {
          throw std::logic_error{"physical plan: a set operation column converted other than from integer to double"};
        }
        auto x = fresh("x");
        w.line("double " + x + " = bc_i64_to_f64(" + row[i].value + ");");
        out[i] = {row[i].null, x, PhysType::F64};
      }
      into(w, out);
    };
  }

  // The group of the aggregate the row's key names.
  auto group(Writer& w, const std::string& agg, const Row& row) -> std::string {
    auto k = fresh("k");
    w.line("bc_sort_key " + k + " = bc_sort_key_begin(cx);");
    for (const auto& v : row) {
      w.open("if (" + v.null + ")");
      w.line("bc_sort_key_null(cx, " + k + ", BC_SORT_ASC, BC_SORT_NULLS_LAST);");
      w.otherwise();
      w.line("bc_sort_key_" + key_suffix(v.type) + "(cx, " + k + ", " + v.value + ", BC_SORT_ASC, BC_SORT_NULLS_LAST);");
      w.close();
    }
    auto g = fresh("g");
    w.line("bc_group " + g + " = bc_agg_group(cx, " + agg + ", bc_sort_key_end(cx, " + k + "));");
    return g;
  }

  static auto add_one(const std::string& g, int slot) -> std::string {
    return "bc_agg_add_i64(cx, " + g + ", " + std::to_string(slot) + ", INT64_C(1));";
  }

  static auto slot_i64(const std::string& g, int slot) -> std::string {
    return "bc_agg_i64(cx, " + g + ", " + std::to_string(slot) + ")";
  }

  // Slot 0 counts the rows of the side held, slot 1 the rows returned.
  Sink hold_rows(const std::string& agg) {
    return [this, agg](Writer& w, const Row& row) { w.line(add_one(group(w, agg, row), 0)); };
  }

  // The rows of `side` are the node's, streamed after the held side's.
  void stream(const physical::SetPlan& n, const Sink& sink, bool direct) {
    if (n.select) {
      leaf(n, sink, direct);
      return;
    }
    auto side = [&](const physical::SetPlan& s, const Sink& into, bool d) {
      stream(s, converted(s.types, n.types, into), d);
    };
    using Kind = logical::SetOp::Kind;
    if (n.kind == Kind::Union && n.all) {
      side(*n.left, sink, direct);
      side(*n.right, sink, direct);
      return;
    }
    auto agg = fresh("u");
    w_.line("bc_agg " + agg + " = bc_agg_open(cx, 2);");
    if (n.kind == Kind::Union) {
      Sink once = [this, agg, sink](Writer& w, const Row& row) {
        auto g = group(w, agg, row);
        w.open("if (bc_agg_null(cx, " + g + ", 0))");
        w.line(add_one(g, 0));
        sink(w, row);
        w.close();
      };
      side(*n.left, once, direct);
      side(*n.right, once, direct);
      return;
    }
    // EXCEPT returns the left arm's rows the right's did not use up;
    // INTERSECT the right arm's that the left's did not.
    auto except = n.kind == Kind::Except;
    Sink match = [this, agg, sink, except, all = n.all](Writer& w, const Row& row) {
      auto g = group(w, agg, row);
      if (all) {
        w.open("if (" + slot_i64(g, 1) + " < " + slot_i64(g, 0) + ")");
        w.line(add_one(g, 1));
        if (!except) sink(w, row);
        if (except) {
          w.otherwise();
          sink(w, row);
        }
        w.close();
        return;
      }
      // DISTINCT: once, if a row of the held side had the key (INTERSECT) or
      // none did (EXCEPT).
      w.open("if (" + std::string{except ? "" : "!"} + "bc_agg_null(cx, " + g + ", 0))");
      w.open("if (bc_agg_null(cx, " + g + ", 1))");
      w.line(add_one(g, 1));
      sink(w, row);
      w.close();
      w.close();
    };
    const auto& held_side = except ? *n.right : *n.left;
    const auto& probed = except ? *n.left : *n.right;
    side(held_side, hold_rows(agg), false);
    side(probed, match, direct);
  }

  // The arm as a block of its own, its rows held in locals and converted by
  // the node above before the sink sees them.
  void leaf(const physical::SetPlan& n, const Sink& sink, bool direct) {
    Writer lw{*n.select};
    Gen g{lw};
    g.arm(*n.select, direct && !stop_.empty() ? stop_ : std::string{}, held(sink));
    w_.open("");
    w_.splice(lw);
    w_.close();
  }

  Writer& w_;
  const physical::Plan& p_;
  std::string stop_;  // the statement's LIMIT as a loop condition, or empty
  int ids_{0};
};

// A plan's code: a SELECT's pipeline, or a set operation's arms.
void print_body(Writer& w, const physical::Plan& p) {
  if (p.set) {
    SetGen{w, p}.plan();
  } else {
    Gen{w}.plan(*p.root);
  }
}

// The inner side of a nested loop: a scan or point lookup under filters, a
// pure function of the outer row.
auto resumable_inner(const physical::Node& n) -> bool {
  if (const auto* f = std::get_if<physical::Filter>(&n.op)) return resumable_inner(*f->input);
  return std::holds_alternative<physical::Scan>(n.op) || std::holds_alternative<physical::PointGet>(n.op);
}

// Emit over filters, joins and at most one Limit, the outermost loop a Scan;
// the joins' inner sides are scans or lookups. No Sort, hash join, aggregate,
// set operation, choice point or NullSplitScan.
auto resumable_plan(const physical::Plan& p) -> bool {
  if (p.set || !p.root) return false;
  const auto* emit = std::get_if<physical::Emit>(&p.root->op);
  if (emit == nullptr) return false;
  const auto* n = emit->input.get();
  if (const auto* l = std::get_if<physical::Limit>(&n->op)) {
    if (l->offset > 0) return false;  // a resumed run would skip the offset's rows again
    n = l->input.get();
  }
  for (;;) {
    if (const auto* f = std::get_if<physical::Filter>(&n->op)) {
      n = f->input.get();
    } else if (const auto* j = std::get_if<physical::IndexJoin>(&n->op)) {
      n = j->outer.get();
    } else if (const auto* nl = std::get_if<physical::NestedLoop>(&n->op)) {
      if (!resumable_inner(*nl->inner)) return false;
      n = nl->outer.get();
    } else {
      return std::holds_alternative<physical::Scan>(n->op);
    }
  }
}

// Prints a searched plan, checked first.
auto print_plan(physical::Plan& p, const std::vector<PhysType>& param_types, std::string_view sql,
                std::string_view fn = "plan") -> GeneratedPlan {
  p.param_types = param_types;
  physical::check(p);
  GeneratedPlan out;
  out.param_types = p.param_types;
  out.catalog_version = p.catalog_version;
  auto own = fn == "plan";
  Writer w{p, own ? std::string{"sub"} : std::string{fn} + "_sub"};
  // Each subquery as a function of its own: a miss in it returns from it.
  std::string subs;
  for (std::size_t i = 0; i < p.subs.size(); ++i) {
    Writer sw{*p.subs[i].plan};
    print_body(sw, *p.subs[i].plan);
    w.absorb(sw);
    subs += "\nstatic void " + w.sub(i) + "(bc_ctx *cx) {\n" + sw.body() + "}\n";
  }
  print_body(w, p);
  if (p.set) {
    out.returns_rows = true;
  } else {
    std::visit(
        [&](const auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, physical::Insert> || std::is_same_v<T, physical::InsertFrom> ||
                        std::is_same_v<T, physical::Update> || std::is_same_v<T, physical::Delete>) {
            // The runtime maintains the table's indexes: a write plan names none.
            out.tables_written = {op.table};
            out.returns_rows = !op.returning.empty();
          } else {
            out.returns_rows = true;
          }
        },
        p.root->op);
  }
  for (const auto& r : p.result) {
    out.result_types.push_back(phys_type(r.sql));
    out.result_names.push_back(r.name);
    out.result_sql.push_back(r.sql);
    out.result_pad.push_back(r.pad);
  }
  out.functions = subs + "\n" + comment_line(sql) + std::string{own ? "" : "static "} + "void " + std::string{fn} +
                  "(bc_ctx *cx) {\n" + w.body() + "}\n";
  out.constants = w.constant_values();
  if (own) {
    out.source = comment_line(sql) +
                 comment_line("schema: catalog v" + std::to_string(p.catalog_version)) +
                 comment_line("objects: " + w.legend()) + "#include \"bc_plan.h\"\n" + w.constants() + "\n" + subs +
                 "\nvoid plan(bc_ctx *cx) {\n" + w.body() + "}\n";
  }
  out.objects_read = w.objects_read();
  out.columns = w.columns_used();
  out.cost = p.cost;
  out.resumable = resumable_plan(p);
  return out;
}

auto rewritten(bound::Bound& b) -> logical::Node& {
  auto* stmt = std::get_if<logical::NodePtr>(&b.stmt);
  if (stmt == nullptr) throw ast::SqlError{"DDL has no plan", -1};
  logical::rewrite(**stmt);
  return **stmt;
}

// A statement or subquery that is a set operation, with the LIMIT over it.
struct SetRoot {
  const logical::Node* node;  // the set operation, under the LIMIT
  const logical::SetOp* op;
  std::optional<std::int64_t> limit;
};

auto set_root(const logical::Node& n) -> std::optional<SetRoot> {
  const auto* top = &n;
  std::optional<std::int64_t> limit;
  if (const auto* l = std::get_if<logical::Limit>(&n.op)) {
    limit = l->n;
    top = l->input.get();
  }
  if (const auto* s = std::get_if<logical::SetOp>(&top->op)) return SetRoot{top, s, limit};
  return std::nullopt;
}

// The plans of a set operation's arms, each searched alone, as a tree. Their
// constants and costs are added to `into`.
auto set_tree(const logical::Node& n, const Catalog& cat, const RowCounts& counts, std::uint64_t memory_bytes,
              const std::vector<PhysType>& param_types, const std::vector<std::uint32_t>& never_null,
              physical::Plan& into) -> std::shared_ptr<const physical::SetPlan> {
  auto out = std::make_shared<physical::SetPlan>();
  if (const auto* s = std::get_if<logical::SetOp>(&n.op)) {
    out->kind = s->kind;
    out->all = s->all;
    out->left = set_tree(*s->left, cat, counts, memory_bytes, param_types, never_null, into);
    out->right = set_tree(*s->right, cat, counts, memory_bytes, param_types, never_null, into);
    for (const auto& o : s->outputs) out->types.push_back(o.expr.type);
    for (const auto& o : s->hidden) out->types.push_back(o.expr.type);
    return out;
  }
  auto arm = search(n, cat, counts, memory_bytes, never_null);
  arm.param_types = param_types;
  for (const auto& c : arm.constants) {
    if (std::ranges::none_of(into.constants, [&](const auto& d) { return d.name == c.name; })) into.constants.push_back(c);
  }
  into.cost += arm.cost;
  for (const auto& r : arm.result) out->types.push_back(r.sql);
  out->select = std::make_shared<const physical::Plan>(std::move(arm));
  return out;
}

// The cheapest plan of a rewritten statement or subquery: a SELECT's, or a
// set operation's, whose arms are each searched alone.
auto plan_for(const logical::Node& n, const Catalog& cat, const RowCounts& counts, std::uint64_t memory_bytes,
              const std::vector<PhysType>& param_types, const std::vector<std::uint32_t>& never_null) -> physical::Plan {
  const auto set = set_root(n);
  if (!set) return search(n, cat, counts, memory_bytes, never_null);
  physical::Plan p;
  p.catalog_version = cat.version();
  p.set = set_tree(*set->node, cat, counts, memory_bytes, param_types, never_null, p);
  p.set_limit = set->limit;
  p.set_order = set->op->order;
  for (const auto& o : set->op->outputs) p.result.push_back(physical::result_column(o));
  return p;
}

// The statement's parameters whose value is never NULL: its constants
// (bound::Bound::fixed, after its own parameters) that are not NULL. A
// subquery's hidden parameter is NULL there until the subquery runs, and a
// clock's is its reading, never NULL.
auto never_null(const bound::Bound& b) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  const auto first = b.param_types.size() - b.fixed.size();
  for (std::size_t i = 0; i < b.fixed.size(); ++i) {
    if (!std::holds_alternative<std::monostate>(b.fixed[i])) out.push_back(narrow<std::uint32_t>(first + i));
  }
  return out;
}

// Each subquery's cheapest plan, its parameters the statement's, given to
// `p`; the constants of its tables join the statement's, so the one source
// declares them all.
void attach_subqueries(physical::Plan& p, bound::Bound& b, const Catalog& cat, const RowCounts& counts,
                       std::uint64_t memory_bytes) {
  for (auto& s : b.subqueries) {
    logical::rewrite(*s.plan);
    auto sp = plan_for(*s.plan, cat, counts, memory_bytes, b.param_types, never_null(b));
    sp.param_types = b.param_types;
    for (const auto& c : sp.constants) {
      if (std::ranges::none_of(p.constants, [&](const auto& d) { return d.name == c.name; })) p.constants.push_back(c);
    }
    p.subs.push_back({std::make_shared<const physical::Plan>(std::move(sp)), s.kind, s.type, s.param, s.args});
  }
}

}  // namespace

auto no_rows_lines(const physical::Node& root) -> std::vector<std::string> { return Gen::no_rows(root); }

namespace {

class ProcPrinter {
public:
  ProcPrinter() : w_{empty_}, gen_{w_} {}

  auto print(const proc::Body& body) -> std::string {
    w_.line("bc_ctx *cx = bc_proc_ctx(px);  /* the frame: $n is slot n */");
    stmts(body);
    return w_.body();
  }

private:
  // A condition's C test: true only when the value is TRUE, as PL/pgSQL takes NULL as false.
  auto truth(const logical::Expr& e) -> std::string {
    auto v = gen_.value_of(e);
    return v.null.empty() ? "(" + v.value + ")" : "(!(" + v.null + ") && (" + v.value + "))";
  }

  void set(std::uint32_t var, const logical::Expr& e, int rescale = 0) {
    auto v = gen_.value_of(e);
    if (rescale != 0) {
      std::int64_t unit = 1;
      for (auto k = 0; k < (rescale < 0 ? -rescale : rescale); ++k) unit *= 10;
      auto u = constant(unit);
      if (rescale > 0) {
        v.value = "bc_mul(cx, " + v.value + ", " + u + ")";
      } else {
        // Half the unit, away from zero, then truncated by the division.
        auto half = constant(unit / 2);
        v.value = "bc_div(cx, bc_add_bigint(cx, " + v.value + ", (" + v.value + ") < 0 ? -" + half + " : " + half + ", 0), " +
                  u + ")";
      }
    }
    auto slot = std::to_string(var);
    auto call = [&]() -> std::string {
      switch (v.type) {
        case PhysType::I64: return "bc_var_set_i64(px, " + slot + ", " + v.value + ");";
        case PhysType::F64: return "bc_var_set_f64(px, " + slot + ", " + v.value + ");";
        case PhysType::Bool: return "bc_var_set_bool(px, " + slot + ", " + v.value + ");";
        case PhysType::Bytes: return "bc_var_set_bytes(px, " + slot + ", " + v.value + ");";
        case PhysType::Num: throw std::logic_error{"procedure: a variable set to a NUMERIC"};
        case PhysType::Jsonb: throw std::logic_error{"procedure: a variable set to a jsonb"};
      }
      return {};
    }();
    if (v.null.empty()) {
      w_.line(call);
      return;
    }
    w_.open("if (" + v.null + ")");
    w_.line("bc_var_set_null(px, " + slot + ");");
    w_.otherwise();
    w_.line(call);
    w_.close();
  }

  void stmts(const proc::Body& body) {
    for (const auto& s : body) std::visit([&](const auto& x) { stmt(x); }, s.v);
  }

  void stmt(const proc::Assign& a) {
    w_.open("");
    set(a.var, a.value, a.rescale);
    w_.close();
  }
  void stmt(const proc::If& i) { branch(i, 0); }
  void branch(const proc::If& i, std::size_t k) {
    if (k == i.branches.size()) {
      stmts(i.otherwise);
      return;
    }
    w_.open("");
    w_.open("if " + truth(i.branches[k].first));
    stmts(i.branches[k].second);
    if (k + 1 < i.branches.size() || !i.otherwise.empty()) {
      w_.otherwise();
      branch(i, k + 1);
    }
    w_.close();
    w_.close();
  }
  void stmt(const proc::ForRange& f) {
    auto r = std::to_string(f.range);
    w_.open("");
    auto lo = gen_.value_of(f.lower);
    auto hi = gen_.value_of(f.upper);
    // A NULL bound's value is not read: a NULL parameter has none.
    auto bound = [](const Gen::Val& v) {
      return v.null.empty() ? v.value + ", 0" : "(" + v.null + ") ? 0 : " + v.value + ", " + v.null;
    };
    w_.line("bc_range_open(px, " + r + ", " + bound(lo) + ", " + bound(hi) + ", " + (f.reverse ? "1" : "0") + ");");
    ++loops_;
    w_.open("while (bc_range_next(px, " + r + ", " + std::to_string(f.var) + "))");
    stmts(f.body);
    w_.close();
    --loops_;
    w_.close();
  }
  void stmt(const proc::While& l) {
    ++loops_;
    w_.open("while (bc_loop(px))");
    if (l.cond) {
      w_.open("");
      w_.line("if (!" + truth(*l.cond) + ") break;");
      w_.close();
    }
    stmts(l.body);
    w_.close();
    --loops_;
  }
  void stmt(const proc::Exit& e) {
    if (loops_ == 0) throw ast::SqlError{std::string{e.exit ? "EXIT" : "CONTINUE"} + " cannot be used outside a loop", -1};
    auto jump = std::string{e.exit ? "break;" : "continue;"};
    if (!e.cond) {
      w_.line(jump);
      return;
    }
    w_.open("");
    w_.line("if " + truth(*e.cond) + " " + jump);
    w_.close();
  }
  void stmt(const proc::Exec& x) {
    auto k = std::to_string(x.stmt);
    w_.line("s" + k + "(bc_stmt_begin(px, " + k + "));");
    w_.line("bc_stmt_end(px, " + k + ");");
  }
  void stmt(const proc::Return&) { w_.line("return;"); }
  void stmt(const proc::ForQuery& f) {
    auto k = std::to_string(f.stmt);
    w_.line("s" + k + "(bc_stmt_begin(px, " + k + "));");
    w_.line("bc_stmt_end(px, " + k + ");");
    ++loops_;
    w_.open("while (bc_rows_next(px, " + k + "))");
    stmts(f.body);
    w_.close();
    --loops_;
    w_.line("bc_rows_done(px, " + k + ");");
  }

  physical::Plan empty_;
  Writer w_;
  Gen gen_;
  int loops_{0};
};

}  // namespace

auto print_procedure_body(const proc::Body& body) -> std::string { return ProcPrinter{}.print(body); }

auto generate_plan(bound::Bound b, const Catalog& cat, std::string_view sql, const RowCounts& counts,
                   std::uint64_t memory_bytes, std::string_view fn) -> GeneratedPlan {
  auto p = plan_for(rewritten(b), cat, counts, memory_bytes, b.param_types, never_null(b));
  attach_subqueries(p, b, cat, counts, memory_bytes);
  return print_plan(p, b.param_types, sql, fn);
}

auto generate_alternatives(bound::Bound b, const Catalog& cat, std::string_view sql, const RowCounts& counts,
                           std::uint64_t memory_bytes) -> std::vector<GeneratedPlan> {
  // A set operation's arms are each searched alone, and only the cheapest of each is kept.
  if (const auto* stmt = std::get_if<logical::NodePtr>(&b.stmt); stmt != nullptr && set_root(**stmt)) {
    std::vector<GeneratedPlan> only;
    only.push_back(generate_plan(std::move(b), cat, sql, counts, memory_bytes));
    return only;
  }
  std::vector<GeneratedPlan> out;
  physical::Plan with_subs;  // the subqueries' plans, the same for every alternative
  attach_subqueries(with_subs, b, cat, counts, memory_bytes);
  for (auto& p : search_all(rewritten(b), cat, counts, memory_bytes, never_null(b))) {
    p.subs = with_subs.subs;
    for (const auto& c : with_subs.constants) {
      if (std::ranges::none_of(p.constants, [&](const auto& d) { return d.name == c.name; })) p.constants.push_back(c);
    }
    const auto n = physical::choices(p);
    // Bit i of a combination takes choice point i's second plan.
    for (std::uint32_t combination = 0; combination < (1u << n); ++combination) {
      if (n > 0) {
        std::vector<std::int64_t> t;
        for (std::size_t i = 0; i < n; ++i) {
          t.push_back((combination >> i & 1u) != 0 ? 0 : std::numeric_limits<std::int64_t>::max());
        }
        physical::override_thresholds(p, t);
      }
      out.push_back(print_plan(p, b.param_types, sql));
    }
  }
  return out;
}

}  // namespace bytecask::sql
