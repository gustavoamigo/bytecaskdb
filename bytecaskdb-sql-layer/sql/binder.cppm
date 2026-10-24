// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// binder.cppm — resolves an AST against the catalog: table and column names to
// ids, each parameter's type from where it is used, column types and DEFAULT
// constants folded to the catalog's.
// What it returns names no column by name; the planner reads offsets and
// slots from the catalog through the column ids.
//
// prototype: the seven forms only; replaced by the designed binder (#492).

module;
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.binder;

export import bytecask.sql.ast;
export import bytecask.sql.catalog;

export namespace bytecask::sql::bound {

// A column, a parameter, or column ± parameter or integer literal (the
// only arithmetic in scope: on integers and DECIMAL, of the column's type).
struct Expr {
  enum class Kind { Column, Param, Add, Sub };
  Kind kind;
  std::uint32_t col_id{0};  // Column, and the left of Add and Sub
  std::uint32_t param{0};   // Param, and the right of Add and Sub
  PhysType type;
  // Add and Sub: a literal right operand instead of the parameter, in the
  // column's representation (a DECIMAL's, scaled by 10^scale).
  std::optional<std::int64_t> constant;
};

// column <op> parameter, normalised so the column is on the left; or
// column <op> parameter + offset, a computed operand: an INTEGER, as
// PostgreSQL types $n ± an integer literal, compared with an INTEGER column.
struct Compare {
  ast::Compare::Op op;
  std::uint32_t col_id;
  std::uint32_t param;
  std::optional<std::int64_t> offset;  // the literal added, negated for -
};

struct Insert {
  const Table* table;
  // One per table column, in column order; nullopt: the column is not given
  // and takes its default, which bc_row_begin writes.
  std::vector<std::optional<Expr>> values;
};

// COUNT(*), COUNT(column), SUM(column) or MIN(column), over the rows the
// WHERE matches. SUM takes an integer or DECIMAL column, MIN those and a
// DATE, TIMESTAMP or TIME column: values the plan holds as int64_t.
struct Aggregate {
  ast::Expr::Fn fn{ast::Expr::Fn::Count};
  std::optional<std::uint32_t> col_id;  // none for COUNT(*)
  bool distinct{false};                 // COUNT(DISTINCT column)
  std::uint32_t side{0};                // a join's: the table col_id is of
};

struct OrderItem {
  std::uint32_t col_id;
  bool desc{false};
};

struct Select {
  const Table* table;
  std::vector<std::uint32_t> targets;  // column ids, in select-list order; empty with an aggregate
  std::optional<Aggregate> aggregate;  // the one aggregate, alone in the select list
  std::vector<Compare> where;
  std::vector<OrderItem> order_by;
  std::optional<std::int64_t> limit;   // nullopt: no limit
  bool for_update{false};
};

struct Update {
  const Table* table;
  std::vector<std::pair<std::uint32_t, Expr>> set;  // column id, new value
  std::vector<Compare> where;
};

struct Delete {
  const Table* table;
  std::vector<Compare> where;
};

// A column of a join: of its first table in FROM (side 0) or its second.
struct JoinColumn {
  std::uint32_t side;
  std::uint32_t col_id;
};

// A column of the first table equal to one of the second: the join's
// condition. Both of one SQL type.
struct JoinOn {
  std::uint32_t col[2];  // by side
};

// A SELECT of two tables. Each WHERE conjunct compares a column of one of
// them with a parameter, or a column of each by equality.
struct Join {
  const Table* table;   // the first table in FROM
  const Table* second;
  std::vector<JoinColumn> targets;
  std::vector<Compare> where[2];  // each side's conjuncts, in WHERE order
  bool ordered{false};            // has an ORDER BY, which no join plan gives yet
  std::optional<std::int64_t> limit;
  std::vector<JoinOn> on;
  std::optional<Aggregate> aggregate;  // the one aggregate, alone in the select list
};

// DDL binds to the catalog's statement values: names checked, types and
// defaults folded.
using Statement = std::variant<Ddl, Insert, Select, Join, Update, Delete>;

// A parameter as a client sees it: the declared type of the column of its
// first use, and whether every use assigns it (INSERT's VALUES, UPDATE's SET
// col = $n), so a value finer than a DECIMAL's scale may be rounded as
// PostgreSQL rounds on assignment. A compared or added parameter is not.
struct ParamInfo {
  SqlType type;
  bool assigned_only{true};
};

struct Bound {
  Statement stmt;
  std::vector<PhysType> param_types;  // $1 is [0]
  // For each parameter compared with a column (WHERE) or added to one (SET
  // col = col ± $n), its SQL type without the modifier (compared_type's
  // range; a DECIMAL operand's, 18 digits at its scale): what its value is
  // checked against before the plan runs, so a key, bound or operand
  // outside it is refused whatever the plan would read. One an UPDATE
  // assigns (SET col = $n) is checked against the column's whole type.
  // nullopt for one only an INSERT assigns, which the row builder checks in
  // column order, as PostgreSQL's assignment does.
  std::vector<std::optional<SqlType>> param_sql;
  // For each parameter param_sql checks, the table of the column it is
  // checked as: the object a BC_C_RANGE refusal names.
  std::vector<std::uint32_t> param_table;
  std::vector<ParamInfo> params;  // $1 is [0]
};

// Throws ast::SqlError with a message and the position of the offending node.
[[nodiscard]] auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound;

}  // namespace bytecask::sql::bound

namespace bytecask::sql::bound {

namespace {

using ast::SqlError;

auto type_name(PhysType t) -> std::string {
  switch (t) {
    case PhysType::I64: return "bigint";
    case PhysType::Bytes: return "bytea";
    case PhysType::F64: return "double precision";
    case PhysType::Bool: return "boolean";
  }
  return "?";
}

// SQL type names as libpg_query canonicalises them, onto the stage-1 types of
// docs/types_design.md.
auto sql_type(const ast::ColumnDefAst& c) -> SqlType {
  auto no_mods = [&](SqlKind k) {
    if (!c.typmods.empty()) throw SqlError{"type " + c.type + " takes no modifier", c.pos};
    return SqlType{k};
  };
  if (c.type == "int2") return no_mods(SqlKind::SmallInt);
  if (c.type == "int4") return no_mods(SqlKind::Integer);
  if (c.type == "int8") return no_mods(SqlKind::BigInt);
  if (c.type == "float4") return no_mods(SqlKind::Real);
  if (c.type == "float8") return no_mods(SqlKind::Double);
  if (c.type == "bool") return no_mods(SqlKind::Boolean);
  if (c.type == "bytea") return no_mods(SqlKind::Bytea);
  if (c.type == "text") return no_mods(SqlKind::Text);
  if (c.type == "timestamp") return no_mods(SqlKind::Timestamp);
  if (c.type == "date") return no_mods(SqlKind::Date);
  if (c.type == "time") return no_mods(SqlKind::Time);
  // The bounds are the catalog's (sql_type_valid); a modifier out of a
  // field's range becomes an invalid field value for it to refuse.
  auto field = [](std::int32_t v) -> std::uint8_t { return std::in_range<std::uint8_t>(v) ? static_cast<std::uint8_t>(v) : 0; };
  if (c.type == "varchar") {
    if (c.typmods.empty()) return SqlType{SqlKind::Text};  // VARCHAR without a length is TEXT
    SqlType t{SqlKind::Varchar, std::in_range<std::uint32_t>(c.typmods[0]) ? static_cast<std::uint32_t>(c.typmods[0]) : 0};
    if (c.typmods.size() != 1 || !sql_type_valid(t)) throw SqlError{"VARCHAR takes one positive length", c.pos};
    return t;
  }
  if (c.type == "numeric") {
    auto limits = "precision 1 to " + std::to_string(kMaxDecimalPrecision) + " (it is stored in 64 bits), scale 0 to precision";
    if (c.typmods.empty() || c.typmods.size() > 2) throw SqlError{"NUMERIC needs a precision: " + limits, c.pos};
    auto p = c.typmods[0];
    auto sc = c.typmods.size() == 2 ? c.typmods[1] : 0;
    SqlType t{SqlKind::Decimal, 0, field(p), field(sc)};
    if (sc < 0 || !sql_type_valid(t)) {
      throw SqlError{"NUMERIC(" + std::to_string(p) + ", " + std::to_string(sc) + ") is not supported: " + limits, c.pos};
    }
    return t;
  }
  if (c.type == "bpchar") throw SqlError{"CHAR(n) is not supported; use VARCHAR(n)", c.pos};
  throw SqlError{"type " + c.type + " is not supported", c.pos};
}

// Why a literal is not a DECIMAL value: not of the form [-]digits[.digits],
// or more digits than any DECIMAL holds, so it cannot be scaled in an i64.
enum class DecimalRefusal { NotDecimal, TooWide };

// The literal [-]digits[.digits] scaled by 10^scale, rounded half away from
// zero. The column's precision is checked by out_of_range, not here.
auto scaled_decimal(std::string_view text, std::uint8_t scale) -> std::expected<std::int64_t, DecimalRefusal> {
  auto neg = !text.empty() && text[0] == '-';
  if (neg) text.remove_prefix(1);
  auto dot = text.find('.');
  auto whole = text.substr(0, dot);
  auto frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  auto digits = [](std::string_view d) { return std::ranges::all_of(d, [](char ch) { return ch >= '0' && ch <= '9'; }); };
  if ((whole.empty() && frac.empty()) || !digits(whole) || !digits(frac)) {
    return std::unexpected(DecimalRefusal::NotDecimal);
  }
  // Leading zeros are no digits: "0.5" has none before the point, so it fits
  // NUMERIC(4, 4).
  while (!whole.empty() && whole[0] == '0') whole.remove_prefix(1);
  if (whole.size() + scale > kMaxDecimalPrecision) return std::unexpected(DecimalRefusal::TooWide);
  std::int64_t v = 0;
  for (auto ch : whole) v = v * 10 + (ch - '0');
  for (std::uint8_t i = 0; i < scale; ++i) v = v * 10 + (i < frac.size() ? frac[i] - '0' : 0);
  if (frac.size() > scale && frac[scale] >= '5') ++v;
  return neg ? -v : v;
}

// A REAL or DOUBLE PRECISION DEFAULT parsed as its own type, so it is
// rounded once, as PostgreSQL's float4in and float8in round: a REAL parsed
// through a double would round twice near a float midpoint. So the catalog
// holds what a row stores. A literal that overflows, or underflows to zero,
// is out of range, as there; one that rounds to a denormal is kept.
template <typename T>
auto parse_number(const ast::Literal& l, SqlType t) -> double {
  using K = ast::Literal::Kind;
  if (l.kind != K::Integer && l.kind != K::Number) {
    throw SqlError{"DEFAULT is not a constant of type " + sql_type_name(t), l.pos};
  }
  T v = 0;
  auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
  if (end != l.text.data() + l.text.size() || (ec != std::errc{} && ec != std::errc::result_out_of_range)) {
    throw SqlError{"DEFAULT is not a constant of type " + sql_type_name(t), l.pos};
  }
  if (ec == std::errc::result_out_of_range) throw SqlError{"DEFAULT out of range for type " + sql_type_name(t), l.pos};
  return static_cast<double>(v);
}

// A DEFAULT constant folded to the column's stored value.
auto fold_default(const ast::Literal& l, SqlType t) -> Datum {
  using K = ast::Literal::Kind;
  auto bad = [&] [[noreturn]] { throw SqlError{"DEFAULT is not a constant of type " + sql_type_name(t), l.pos}; };
  if (l.kind == K::Null) return std::monostate{};
  switch (t.kind) {
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt: {
      std::int64_t v = 0;
      if (l.kind != K::Integer) bad();
      auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
      if (ec != std::errc{} || end != l.text.data() + l.text.size()) bad();
      if (out_of_range(t, v)) throw SqlError{"DEFAULT out of range for type " + sql_type_name(t), l.pos};
      return v;
    }
    case SqlKind::Decimal: {
      if (l.kind != K::Integer && l.kind != K::Number) bad();
      auto v = scaled_decimal(l.text, t.scale);
      if (!v && v.error() == DecimalRefusal::NotDecimal) bad();
      if (!v || out_of_range(t, *v)) throw SqlError{"DEFAULT out of range for type " + sql_type_name(t), l.pos};
      return *v;
    }
    case SqlKind::Real: return parse_number<float>(l, t);
    case SqlKind::Double: return parse_number<double>(l, t);
    case SqlKind::Boolean:
      if (l.kind != K::Bool) bad();
      return l.b;
    case SqlKind::Varchar:
    case SqlKind::Text:
    case SqlKind::Bytea: {
      if (l.kind != K::String) bad();
      auto b = std::as_bytes(std::span{l.text.data(), l.text.size()});
      if (auto why = out_of_range(t, b)) {
        throw SqlError{"DEFAULT is not a " + sql_type_name(t) + ": " + *why, l.pos};
      }
      return Bytes(b.begin(), b.end());
    }
    case SqlKind::Timestamp:
    case SqlKind::Date:
    case SqlKind::Time:
      throw SqlError{"a DEFAULT for type " + sql_type_name(t) + " is not supported yet", l.pos};
  }
  bad();
}

// The groups PostgreSQL's casts and operators sort the types into. Numbers
// compare with and assign to each other; a string takes a value of any type
// on assignment; no other pair meets.
enum class TypeClass { Number, Temporal, Boolean, String, Bytea };

constexpr auto type_class(SqlKind k) -> TypeClass {
  switch (k) {
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Decimal:
    case SqlKind::Real:
    case SqlKind::Double: return TypeClass::Number;
    case SqlKind::Timestamp:
    case SqlKind::Date:
    case SqlKind::Time: return TypeClass::Temporal;
    case SqlKind::Boolean: return TypeClass::Boolean;
    case SqlKind::Text:
    case SqlKind::Varchar: return TypeClass::String;
    case SqlKind::Bytea: return TypeClass::Bytea;
  }
  std::unreachable();
}

// The SQLSTATE PostgreSQL refuses a value of type `from` meeting a column of
// type `to` with: 42804 for an assignment, 42883 for a comparison (no such
// operator). None where PostgreSQL casts, and the layer, which holds the two
// as different physical types, does not yet: that stays 42601.
auto type_refusal(SqlKind from, SqlKind to, bool assignment) -> std::optional<SqlState> {
  auto a = type_class(from);
  auto b = type_class(to);
  if (a == b || (assignment && b == TypeClass::String)) return std::nullopt;
  return assignment ? kDatatypeMismatch : kUndefinedFunction;
}

// An integer or DECIMAL column: one whose arithmetic is exact in int64_t.
auto exact_number(const Column& c) -> bool {
  switch (c.sql.kind) {
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Decimal: return true;
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Timestamp:
    case SqlKind::Date:
    case SqlKind::Time: return false;
  }
  return false;
}

auto column_spec(const ast::ColumnDefAst& c) -> ColumnSpec {
  auto t = sql_type(c);
  return ColumnSpec{c.name, t, !c.not_null && !c.primary_key,
                    c.default_value ? fold_default(*c.default_value, t) : Datum{}};
}

class Binder {
public:
  explicit Binder(const Catalog& cat) : cat_{cat} {}

  auto table(const std::string& name, std::int32_t pos) const -> const Table* {
    const auto* t = cat_.table(name);
    if (t == nullptr) throw SqlError{"relation \"" + name + "\" does not exist", pos, kUndefinedTable};
    return t;
  }

  static auto column(const Table& t, const std::string& name, std::int32_t pos) -> const Column* {
    const auto* c = t.column(name);
    if (c == nullptr) throw SqlError{"column \"" + name + "\" does not exist", pos, kUndefinedColumn};
    return c;
  }

  // An ORDER BY item's column, as a column reference.
  static auto column_ref(const ast::OrderItem& o) -> ast::Expr {
    ast::Expr e{ast::Expr::Kind::Column, o.column, 0, nullptr, nullptr, o.pos};
    e.table = o.table;
    return e;
  }

  // A column reference of a one-table statement: qualified by that table's
  // name, or not at all.
  static auto column(const Table& t, const ast::Expr& e) -> const Column* {
    if (!e.table.empty() && e.table != t.name) {
      throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
    }
    return column(t, e.column, e.pos);
  }

  // A column reference of a join: the side whose table it names, or the one
  // that has the column; one both have is ambiguous (42702).
  static auto column(const Table* (&sides)[2], const ast::Expr& e) -> std::pair<std::uint32_t, const Column*> {
    std::optional<std::pair<std::uint32_t, const Column*>> found;
    for (std::uint32_t side = 0; side < 2; ++side) {
      if (!e.table.empty() && e.table != sides[side]->name) continue;
      const auto* c = sides[side]->column(e.column);
      if (c == nullptr) continue;
      if (found) throw SqlError{"column reference \"" + e.column + "\" is ambiguous", e.pos, kAmbiguousColumn};
      found = {side, c};
    }
    if (found) return *found;
    if (!e.table.empty() && e.table != sides[0]->name && e.table != sides[1]->name) {
      throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
    }
    throw SqlError{"column \"" + e.column + "\" does not exist", e.pos, kUndefinedColumn};
  }

  // How a statement uses a parameter: compared with a column (WHERE),
  // assigned to one (INSERT, SET), or the operand of col + $n or col - $n.
  enum class Use { Compared, Assigned, Added, Subtracted };

  // Gives parameter $n+1 a type from one use with `col`, which sets its
  // physical type and its SQL type. Every use must agree on both, whatever
  // their order: a $n used with two SQL types that share a physical type
  // (DOUBLE PRECISION and REAL, INTEGER and BIGINT, DECIMALs of two scales)
  // would be read as two values. Refused, where PostgreSQL infers one type
  // from its first use (docs/frontend_design.md, *Known differences*). A
  // DECIMAL of scale 0 is a BIGINT here (compared_type): its integer is the
  // number itself. A compared or operand $n is range-checked before the plan
  // runs (param_sql): against its column's type without the modifier, and a
  // DECIMAL operand against 18 digits at its scale, so a sum stays inside
  // int64; one only assigned is checked by the row builder.
  void use_param(std::uint32_t n, PhysType t, std::int32_t pos, const Column& col, std::uint32_t table, Use use) {
    if (params_.size() <= n) params_.resize(n + 1);
    auto& p = params_[n];
    // PostgreSQL's code only for two comparisons. It types a $n by its first
    // use, which for UPDATE is a WHERE before the SET the layer binds first,
    // and refuses two assignments of one $n whatever their types (42P08):
    // any other pair is its answer to a different statement, so 42601.
    auto clash = [&] {
      if (p.first_use != Use::Compared || use != Use::Compared) return kSyntaxError;
      return type_refusal(p.first->sql.kind, col.sql.kind, false).value_or(kSyntaxError);
    };
    if (p.phys && *p.phys != t) {
      throw SqlError{"parameter $" + std::to_string(n + 1) + " used as both " + type_name(*p.phys) + " and " +
                         type_name(t),
                     pos, clash()};
    }
    p.phys = t;
    if (p.first != nullptr && compared_type(p.first->sql).identity != compared_type(col.sql).identity) {
      auto verb = [](Use u) {
        switch (u) {
          case Use::Compared: return "compared with ";
          case Use::Assigned: return "assigned to ";
          case Use::Added: return "added to ";
          case Use::Subtracted: return "subtracted from ";
        }
        return "";
      };
      auto named = [](const Column& c) { return c.name + " (" + sql_type_name(c.sql) + ")"; };
      auto second = use == p.first_use ? named(col) : verb(use) + named(col);
      throw SqlError{"parameter $" + std::to_string(n + 1) + " is " + verb(p.first_use) + named(*p.first) + " and " +
                         second,
                     pos, clash()};
    }
    if (p.first == nullptr) {
      p.first = &col;
      p.first_use = use;
    }
    if (use == Use::Assigned) return;
    p.assigned_only = false;
    if (use != Use::Compared && col.sql.kind == SqlKind::Decimal) {
      narrow(p, SqlType{SqlKind::Decimal, 0, static_cast<std::uint8_t>(kMaxDecimalPrecision), col.sql.scale}, table);
    } else {
      narrow(p, compared_type(col.sql).range, table);
    }
  }

  // UPDATE's SET col = $n: $n is also checked before the plan runs, against
  // col's whole type, as PostgreSQL coerces it at bind and so refuses one
  // outside the type even when no row matches. An INSERT writes its row
  // whatever its parameters, and the row builder checks them in column
  // order, as PostgreSQL does.
  void check_assigned(std::uint32_t n, const Column& col, std::uint32_t table) { narrow(params_[n], col.sql, table); }

  // An aggregate of a one-table SELECT. SUM and MIN take a column whose
  // values the plan holds as int64_t: SUM an integer or DECIMAL one, as
  // PostgreSQL sums them exactly; MIN those and the date and time types.
  // MIN of a REAL or DOUBLE PRECISION column is later, and of a string or
  // bytes column outside the MVP (docs/frontend_design.md, *Aggregates*).
  static auto aggregate(const Table& t, const ast::Expr& e) -> Aggregate {
    if (e.column.empty()) return Aggregate{e.fn, std::nullopt, false, 0};
    return aggregate(*column(t, e), e);
  }

  // Whether the layer takes `fn` of `c`, and if not the SQLSTATE of the
  // refusal: 42883 where PostgreSQL has no such aggregate either, 42601 where
  // it has and the layer does not yet (SUM of a REAL, DOUBLE PRECISION or
  // TIME column, which PostgreSQL sums as an interval; MIN of a REAL, DOUBLE
  // PRECISION or string column).
  struct AggregateSupport {
    bool supported;
    SqlState sqlstate;
  };
  static auto aggregate_support(ast::Expr::Fn fn, const Column& c) -> AggregateSupport {
    using Fn = ast::Expr::Fn;
    if (fn == Fn::Count) return {true, kSyntaxError};
    auto sum = fn == Fn::Sum;
    switch (c.sql.kind) {
      case SqlKind::SmallInt:
      case SqlKind::Integer:
      case SqlKind::BigInt:
      case SqlKind::Decimal: return {true, kSyntaxError};
      case SqlKind::Real:
      case SqlKind::Double: return {false, kSyntaxError};
      case SqlKind::Text:
      case SqlKind::Varchar: return {false, sum ? kUndefinedFunction : kSyntaxError};
      case SqlKind::Boolean:
      case SqlKind::Bytea: return {false, kUndefinedFunction};
      case SqlKind::Timestamp:
      case SqlKind::Date: return {!sum, kUndefinedFunction};
      case SqlKind::Time: return {!sum, kSyntaxError};
    }
    std::unreachable();
  }

  static auto aggregate(const Column& col, const ast::Expr& e) -> Aggregate {
    const auto* c = &col;
    if (auto s = aggregate_support(e.fn, *c); !s.supported) {
      throw SqlError{std::string{e.fn == ast::Expr::Fn::Sum ? "SUM" : "MIN"} + " of a " + sql_type_name(c->sql) +
                         " column is not supported",
                     e.pos, s.sqlstate};
    }
    return Aggregate{e.fn, c->id, e.distinct, 0};
  }

  // An integer literal added to or subtracted from `c`, in c's
  // representation: a DECIMAL's scaled by 10^scale, within its 18 digits.
  // PostgreSQL types it integer, or bigint past integer, and the sum as the
  // column's type or wider; the row builder's check of the stored value is
  // the one PostgreSQL's assignment makes.
  static auto literal(const Column& c, const ast::Expr& e) -> std::int64_t {
    if (c.sql.kind == SqlKind::SmallInt || c.sql.kind == SqlKind::Integer) {
      // A SMALLINT or INTEGER holds less than 2^31 in magnitude, so a literal
      // of 2^32 or more leaves the column's range whatever it is added to,
      // as PostgreSQL's sum does: held as 2^32, the sum cannot overflow
      // int64 and the row builder refuses it (22003), or NULL stays NULL.
      constexpr std::int64_t kBeyond = std::int64_t{1} << 32;
      return std::clamp(e.constant, -kBeyond, kBeyond);
    }
    if (c.sql.kind != SqlKind::Decimal) return e.constant;
    std::int64_t v = e.constant;
    for (std::uint8_t i = 0; i < c.sql.scale; ++i) {
      if (v > kDecimalBound / 10 || v < -(kDecimalBound / 10)) throw SqlError{"numeric field overflow", e.pos, kNumericValueOutOfRange};
      v *= 10;
    }
    if (v >= kDecimalBound || v <= -kDecimalBound) throw SqlError{"numeric field overflow", e.pos, kNumericValueOutOfRange};
    return v;
  }
  static constexpr std::int64_t kDecimalBound = 1'000'000'000'000'000'000;  // 10^18: DECIMAL's 18 digits

  // An expression whose type is fixed by its target (an assigned column).
  auto value(const Table& t, const ast::Expr& e, const Column& target) -> Expr {
    auto want = target.type;
    switch (e.kind) {
      case ast::Expr::Kind::Param:
        use_param(e.param, want, e.pos, target, t.id, Use::Assigned);
        return Expr{Expr::Kind::Param, 0, e.param, want};
      case ast::Expr::Kind::Column: {
        const auto* c = column(t, e);
        if (c->type != want) {
          throw SqlError{"column \"" + c->name + "\" is " + type_name(c->type), e.pos,
                         type_refusal(c->sql.kind, target.sql.kind, true).value_or(kSyntaxError)};
        }
        return Expr{Expr::Kind::Column, c->id, 0, want};
      }
      case ast::Expr::Kind::Add:
      case ast::Expr::Kind::Sub: {
        auto op = e.kind == ast::Expr::Kind::Add ? "+" : "-";
        if (e.lhs->kind != ast::Expr::Kind::Column ||
            (e.rhs->kind != ast::Expr::Kind::Param && e.rhs->kind != ast::Expr::Kind::Const)) {
          throw SqlError{std::string{"only column "} + op + " parameter and column " + op + " integer are supported", e.pos};
        }
        const auto* c = column(t, *e.lhs);
        // On integers and DECIMAL, of the assigned column's type: the
        // operator's result type is then the column's, and the row
        // builder's range check on the stored value is the one PostgreSQL's
        // operator makes (22003).
        if (!exact_number(*c) || !exact_number(target) || compared_type(c->sql).identity != compared_type(target.sql).identity) {
          throw SqlError{std::string{"operator "} + op + " is only supported on an integer or DECIMAL column of the "
                                                         "assigned column's type",
                         e.pos};
        }
        auto kind = e.kind == ast::Expr::Kind::Add ? Expr::Kind::Add : Expr::Kind::Sub;
        if (e.rhs->kind == ast::Expr::Kind::Const) return Expr{kind, c->id, 0, PhysType::I64, literal(*c, *e.rhs)};
        use_param(e.rhs->param, PhysType::I64, e.rhs->pos, *c, t.id,
                  e.kind == ast::Expr::Kind::Add ? Use::Added : Use::Subtracted);
        return Expr{kind, c->id, e.rhs->param, PhysType::I64, std::nullopt};
      }
      case ast::Expr::Kind::Aggregate: throw SqlError{"aggregate functions are not allowed here", e.pos, kGroupingError};
      case ast::Expr::Kind::Const: throw SqlError{"an integer literal is supported only after + or - yet", e.pos};
    }
    throw SqlError{"expression not supported", e.pos};
  }

  auto where(const Table& t, const std::vector<ast::Compare>& in) -> std::vector<Compare> {
    std::vector<Compare> out;
    for (const auto& w : in) out.push_back(compare(w, [&](const ast::Expr& e) { return std::pair{t.id, column(t, e)}; }));
    return out;
  }

  // A comparison's operand other than a column: a parameter, or $n ± an
  // integer literal.
  static auto operand(const ast::Expr& e) -> bool {
    using K = ast::Expr::Kind;
    if (e.kind == K::Param) return true;
    return (e.kind == K::Add || e.kind == K::Sub) && e.lhs->kind == K::Param && e.rhs->kind == K::Const;
  }

  // One WHERE conjunct as column <op> operand, the column resolved by
  // `resolve`. In $n ± k, $n and the sum are INTEGERs, as PostgreSQL types
  // an unknown $n beside an integer literal; the column compared with them
  // is an INTEGER, so the sum is compared as one, and k must fit one.
  auto compare(const ast::Compare& w, const auto& resolve) -> Compare {
    const auto* col = &w.lhs;
    const auto* rhs = &w.rhs;
    auto op = w.op;
    if (operand(*col) && rhs->kind == ast::Expr::Kind::Column) {
      std::swap(col, rhs);
      op = mirrored(op);
    }
    if (col->kind != ast::Expr::Kind::Column || !operand(*rhs)) {
      throw SqlError{std::string{w.on ? "ON" : "WHERE"} + " must compare a column with a parameter", w.pos};
    }
    auto [table, c] = resolve(*col);
    if (rhs->kind == ast::Expr::Kind::Param) {
      use_param(rhs->param, c->type, rhs->pos, *c, table, Use::Compared);
      return Compare{op, c->id, rhs->param, std::nullopt};
    }
    if (c->sql.kind != SqlKind::Integer) {
      throw SqlError{"$n + n and $n - n are supported only compared with an INTEGER column yet", rhs->pos};
    }
    auto k = rhs->rhs->constant;
    if (!std::in_range<std::int32_t>(k)) {
      throw SqlError{"an integer literal past INTEGER is not supported here yet", rhs->rhs->pos};
    }
    use_param(rhs->lhs->param, c->type, rhs->lhs->pos, *c, table, Use::Compared);
    return Compare{op, c->id, rhs->lhs->param, rhs->kind == ast::Expr::Kind::Add ? k : -k};
  }

  // $1 < x is x > $1.
  static auto mirrored(ast::Compare::Op op) -> ast::Compare::Op {
    switch (op) {
      case ast::Compare::Op::Lt: return ast::Compare::Op::Gt;
      case ast::Compare::Op::Le: return ast::Compare::Op::Ge;
      case ast::Compare::Op::Gt: return ast::Compare::Op::Lt;
      case ast::Compare::Op::Ge: return ast::Compare::Op::Le;
      case ast::Compare::Op::Eq: return op;
    }
    return op;
  }

  // A join's WHERE, split by the side each conjunct's column is of; a
  // comparison of two columns, one of each table, goes to `on`.
  void where(const Table* (&sides)[2], const std::vector<ast::Compare>& in, std::vector<Compare> (&out)[2],
             std::vector<JoinOn>& on) {
    for (const auto& w : in) {
      if (w.lhs.kind == ast::Expr::Kind::Column && w.rhs.kind == ast::Expr::Kind::Column) {
        on.push_back(join_on(sides, w));
        continue;
      }
      std::uint32_t side = 0;
      auto c = compare(w, [&](const ast::Expr& e) {
        auto [s, col] = column(sides, e);
        side = s;
        return std::pair{sides[s]->id, col};
      });
      out[side].push_back(c);
    }
  }

  // A column of one table equal to a column of the other, of one SQL type.
  static auto join_on(const Table* (&sides)[2], const ast::Compare& w) -> JoinOn {
    auto [ls, lc] = column(sides, w.lhs);
    auto [rs, rc] = column(sides, w.rhs);
    if (ls == rs) throw SqlError{"comparing two columns of one table is not supported yet", w.pos};
    if (w.op != ast::Compare::Op::Eq) throw SqlError{"a join compares two columns with = only yet", w.pos};
    if (compared_type(lc->sql).identity != compared_type(rc->sql).identity) {
      throw SqlError{"comparing " + lc->name + " (" + sql_type_name(lc->sql) + ") with " + rc->name + " (" +
                         sql_type_name(rc->sql) + ") is not supported: a join compares columns of one type",
                     w.pos};
    }
    JoinOn j{};
    j.col[ls] = lc->id;
    j.col[rs] = rc->id;
    return j;
  }

  // A SELECT of two tables. No FOR UPDATE on one yet; its ORDER BY is the
  // planner's to refuse, unless an aggregate makes it 42803.
  auto join(const ast::Select& s) -> Join {
    Join out{table(s.table, s.table_pos), table(s.join, s.join_pos), {}, {}, !s.order_by.empty(), s.limit, {}, {}};
    if (out.table == out.second) throw SqlError{"a table joined with itself is not supported", s.pos};
    if (s.for_update) throw SqlError{"FOR UPDATE on a join is not supported yet", s.pos};
    const Table* sides[2]{out.table, out.second};
    where(sides, s.where, out.where, out.on);
    std::optional<std::pair<const Table*, const ast::Expr*>> bare;  // a column beside an aggregate: 42803
    for (const auto& e : s.targets) {
      if (e.kind == ast::Expr::Kind::Aggregate) {
        if (out.aggregate) throw SqlError{"only one aggregate per SELECT is supported", e.pos};
        if (e.column.empty()) {
          out.aggregate = Aggregate{e.fn, std::nullopt, false, 0};
          continue;
        }
        auto [side, c] = column(sides, e);
        out.aggregate = aggregate(*c, e);
        out.aggregate->side = side;
        continue;
      }
      if (e.kind != ast::Expr::Kind::Column) throw SqlError{"only columns and aggregates can be selected", e.pos};
      auto [side, c] = column(sides, e);
      out.targets.push_back({side, c->id});
      bare = {sides[side], &e};
    }
    if (out.aggregate && bare) ungrouped(*bare->first, bare->second->column, bare->second->pos);
    for (const auto& o : s.order_by) {
      auto [side, c] = column(sides, column_ref(o));
      if (out.aggregate) ungrouped(*sides[side], c->name, o.pos);
    }
    return out;
  }

  // 42803, in PostgreSQL's words.
  [[noreturn]] static void ungrouped(const Table& t, const std::string& col, std::int32_t pos) {
    throw SqlError{"column \"" + t.name + "." + col + "\" must appear in the GROUP BY clause or be used in an aggregate function",
                   pos, kGroupingError};
  }

  // Every parameter from $1 to the highest used must have a type.
  auto param_types(std::int32_t pos) const -> std::vector<PhysType> {
    std::vector<PhysType> out;
    for (std::size_t i = 0; i < params_.size(); ++i) {
      if (!params_[i].phys) throw SqlError{"could not determine data type of parameter $" + std::to_string(i + 1), pos, kIndeterminateDatatype};
      out.push_back(*params_[i].phys);
    }
    return out;
  }

  // For each parameter compared with or added to a column, what the
  // pre-plan range check checks it against (use_param).
  auto param_sql() const -> std::vector<std::optional<SqlType>> {
    std::vector<std::optional<SqlType>> out;
    for (const auto& p : params_) {
      out.push_back(p.range);
    }
    return out;
  }

  // Every parameter has a first use once param_types() has passed.
  auto param_info() const -> std::vector<ParamInfo> {
    std::vector<ParamInfo> out;
    for (const auto& p : params_) out.push_back({p.first->sql, p.assigned_only});
    return out;
  }

  auto param_table() const -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> out;
    for (const auto& p : params_) out.push_back(p.table);
    return out;
  }

private:
  // A parameter's type from its uses, which all agree.
  struct ParamUse {
    std::optional<PhysType> phys;
    const Column* first{nullptr};  // the column of its first use, for the SQL type
    Use first_use{Use::Assigned};
    std::optional<SqlType> range;  // what the pre-plan range check checks: none if only an INSERT assigns it
    std::uint32_t table{0};        // the table of the column `range` comes from
    bool assigned_only{true};      // every use an assignment
  };

  // Whether every value inside range a is inside range b, two ranges of one
  // parameter: of one SQL type, so they differ only by a modifier, or one is
  // a DECIMAL's and the other the BIGINT range its comparison takes.
  static auto within(SqlType a, SqlType b) -> bool {
    if (a.kind == SqlKind::Decimal) return b.kind != SqlKind::Decimal || a.precision <= b.precision;
    if (a.kind == SqlKind::Varchar) return b.kind != SqlKind::Varchar || a.length <= b.length;
    return b.kind != SqlKind::Decimal && b.kind != SqlKind::Varchar;
  }

  // The parameter's range becomes `r` where r is narrower: it is checked
  // against the narrowest of its uses, in whatever order they come, and a
  // refusal names that use's table.
  static void narrow(ParamUse& p, SqlType r, std::uint32_t table) {
    if (p.range && (!within(r, *p.range) || within(*p.range, r))) return;
    p.range = r;
    p.table = table;
  }

  const Catalog& cat_;
  std::vector<ParamUse> params_;
};

}  // namespace

auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound {
  Binder b{cat};
  auto bound_stmt = std::visit(
      [&](const auto& s) -> std::pair<Statement, std::int32_t> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ast::CreateTable>) {
          CreateTableDdl out{s.table, {}, s.pk};
          for (const auto& c : s.columns) {
            out.columns.push_back(column_spec(c));
            if (c.primary_key) out.pk.push_back(c.name);
          }
          if (out.pk.empty()) throw SqlError{"a table needs a PRIMARY KEY", s.pos};
          return {Ddl{std::move(out)}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateIndex>) {
          const auto* t = b.table(s.table, s.pos);
          CreateIndexDdl out{s.index, s.table, {}, s.unique};
          for (const auto& c : s.columns) {
            (void)Binder::column(*t, c.name, s.pos);
            out.columns.push_back({c.name, c.desc});
          }
          return {Ddl{std::move(out)}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::DropTable>) {
          return {Ddl{DropTableDdl{s.table, s.if_exists}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::DropIndex>) {
          return {Ddl{DropIndexDdl{s.index, s.if_exists}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::AddColumn>) {
          (void)b.table(s.table, s.pos);
          if (s.column.primary_key) throw SqlError{"ADD COLUMN cannot add a primary key column", s.column.pos};
          auto spec = column_spec(s.column);
          if (!spec.nullable && std::holds_alternative<std::monostate>(spec.default_value)) {
            throw SqlError{"column \"" + s.column.name + "\" is NOT NULL and needs a DEFAULT for the existing rows",
                           s.column.pos};
          }
          return {Ddl{AddColumnDdl{s.table, std::move(spec)}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Insert>) {
          const auto* t = b.table(s.table, s.pos);
          Insert out{t, std::vector<std::optional<Expr>>(t->columns.size())};
          std::vector<const Column*> targets;
          if (s.columns.empty()) {
            // As PostgreSQL: the values fill the leading columns and the rest
            // take their defaults, so a column added later changes nothing.
            for (std::size_t i = 0; i < std::min(s.values.size(), t->columns.size()); ++i) {
              targets.push_back(&t->columns[i]);
            }
            if (s.values.size() > t->columns.size()) {
              throw SqlError{"INSERT has more expressions than target columns", s.pos};
            }
          } else {
            for (const auto& name : s.columns) targets.push_back(Binder::column(*t, name, s.pos));
          }
          if (targets.size() != s.values.size()) {
            throw SqlError{"INSERT has " + std::to_string(s.values.size()) + " values for " +
                               std::to_string(targets.size()) + " columns",
                           s.pos};
          }
          for (std::size_t i = 0; i < targets.size(); ++i) {
            auto at = static_cast<std::size_t>(targets[i] - t->columns.data());
            if (out.values[at]) throw SqlError{"column \"" + targets[i]->name + "\" specified more than once", s.pos, kDuplicateColumn};
            out.values[at] = b.value(*t, s.values[i], *targets[i]);
            if (out.values[at]->kind != Expr::Kind::Param) {
              throw SqlError{"INSERT values must be parameters", s.values[i].pos};
            }
          }
          // A column left out takes its default. One that is NOT NULL with no
          // default, a primary key column among them, is refused when the
          // row is built (BC_C_NOT_NULL), as a NULL given for it is.
          return {out, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          if (!s.join.empty()) return {b.join(s), s.pos};
          const auto* t = b.table(s.table, s.pos);
          Select out{t, {}, std::nullopt, b.where(*t, s.where), {}, s.limit, s.for_update};
          const ast::Expr* bare = nullptr;  // a column beside an aggregate: 42803
          for (const auto& e : s.targets) {
            if (e.kind == ast::Expr::Kind::Aggregate) {
              if (out.aggregate) throw SqlError{"only one aggregate per SELECT is supported", e.pos};
              out.aggregate = Binder::aggregate(*t, e);
              continue;
            }
            if (e.kind != ast::Expr::Kind::Column) throw SqlError{"only columns and aggregates can be selected", e.pos};
            out.targets.push_back(Binder::column(*t, e)->id);
            bare = &e;
          }
          if (out.aggregate && bare != nullptr) Binder::ungrouped(*t, bare->column, bare->pos);
          // 0A000, as PostgreSQL refuses it.
          if (out.aggregate && s.for_update) throw SqlError{"FOR UPDATE is not allowed with aggregate functions", s.pos, kFeatureNotSupported};
          for (const auto& o : s.order_by) {
            const auto* c = Binder::column(*t, Binder::column_ref(o));
            if (out.aggregate) Binder::ungrouped(*t, c->name, o.pos);
            out.order_by.push_back({c->id, o.desc});
          }
          return {out, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Update>) {
          const auto* t = b.table(s.table, s.pos);
          Update out{t, {}, {}};
          for (const auto& a : s.set) {
            const auto* c = Binder::column(*t, a.column, a.pos);
            if (std::ranges::find(t->pk_col_ids, c->id) != t->pk_col_ids.end()) {
              throw SqlError{"updating a primary key column is not supported", a.pos};
            }
            out.set.emplace_back(c->id, b.value(*t, a.value, *c));
            if (const auto& v = out.set.back().second; v.kind == Expr::Kind::Param) b.check_assigned(v.param, *c, t->id);
          }
          out.where = b.where(*t, s.where);
          return {out, s.pos};
        } else {
          const auto* t = b.table(s.table, s.pos);
          return {Delete{t, b.where(*t, s.where)}, s.pos};
        }
      },
      stmt);
  auto types = b.param_types(bound_stmt.second);
  return Bound{std::move(bound_stmt.first), std::move(types), b.param_sql(), b.param_table(), b.param_info()};
}

}  // namespace bytecask::sql::bound
