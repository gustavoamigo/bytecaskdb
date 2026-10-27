// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// binder.cppm — resolves an AST against the catalog: table and column names to
// ids, each parameter's type from where it is used, column types and DEFAULT
// constants folded to the catalog's. A DML statement binds to its logical
// plan (logical.cppm), built directly: Get, Filter, Join, Project,
// Aggregate, Sort, Limit and the writes, over typed expressions.
//
// prototype: the seven forms only; replaced by the designed binder (#492).

module;
#include <algorithm>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
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

export import bytecask.sql.logical;
import bytecask.sql.parser;
import bytecask.sql.text;

export namespace bytecask::sql::bound {

// A parameter as a client sees it: the declared type of the column of its
// first use, and whether every use assigns it (INSERT's VALUES, UPDATE's SET
// col = $n), so a value finer than a DECIMAL's scale may be rounded as
// PostgreSQL rounds on assignment. A compared or added parameter is not.
struct ParamInfo {
  SqlType type;
  bool assigned_only{true};
};

// A bound statement: DDL as the catalog's statement values, names checked,
// types and defaults folded; anything else as its logical plan.
struct Bound {
  std::variant<Ddl, logical::NodePtr> stmt;
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
  // The values of the statement's constants, folded (docs/frontend_design.md,
  // *Literals*): the parameters after its own, in order. param_types holds
  // their types after the statement's own parameters'; param_sql,
  // param_table and params hold the statement's own only.
  std::vector<Datum> fixed;
  // Each uncorrelated subquery, run before the plan: it sets its hidden
  // parameters, among `fixed`'s, whose values there are NULL.
  std::vector<logical::Subquery> subqueries;
  // The views the statement was expanded through, each once, nested ones
  // included. A cached plan is good only while each still exists: ids are
  // never reused, so a dropped and recreated view is another id.
  std::vector<std::uint32_t> views;
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
    case SqlKind::Numeric:
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
    case SqlKind::Double:
    case SqlKind::Numeric: return TypeClass::Number;
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
    case SqlKind::Time:
    case SqlKind::Numeric: return false;  // no column has it
  }
  return false;
}

// ------------------------------------------------------------- constants

// A constant expression, folded when the statement is bound
// (docs/frontend_design.md, *Literals*): a literal, or unary + or -, + or -
// of constants. Its value is in its type's representation, a numeric's
// scaled by 10^scale, and its type PostgreSQL's: an integer literal is an
// INTEGER, or a BIGINT past it; a number with a point or an exponent a
// numeric of no declared precision at the scale it is written with
// (SqlType{Decimal, 0, 0, scale}), within 18 digits; TRUE and FALSE
// BOOLEAN. A string literal and NULL are of no type yet (`unknown`):
// PostgreSQL types one by its use and reads a string with that type's input
// function, and alone in a select list it is TEXT.
struct Constant {
  Datum value;
  SqlType type;
  bool unknown{false};
  std::string text;  // an unknown string's bytes
  std::int32_t pos{-1};
};

constexpr std::int64_t kDecimalLimit = kPowersOfTen[kMaxDecimalPrecision];  // 10^18: past every DECIMAL

auto numeric_type(std::uint8_t scale) -> SqlType { return SqlType{SqlKind::Decimal, 0, 0, scale}; }

auto is_integer(SqlType t) -> bool {
  return t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer || t.kind == SqlKind::BigInt;
}

// A type as PostgreSQL's messages name it.
auto pg_type_name(SqlType t) -> std::string {
  switch (t.kind) {
    case SqlKind::Varchar: return "character varying";
    case SqlKind::Decimal:
    case SqlKind::Numeric: return "numeric";
    case SqlKind::Timestamp: return "timestamp without time zone";
    case SqlKind::Time: return "time without time zone";
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Text:
    case SqlKind::Bytea:
    case SqlKind::Date: return sql_type_name(t);
  }
  return sql_type_name(t);
}

// A form the layer does not take yet: 42601, where PostgreSQL runs it.
[[noreturn]] void not_yet(const std::string& what, std::int32_t pos) { throw SqlError{what + " is not supported yet", pos}; }

auto integer_constant(std::int64_t v, std::int32_t pos) -> Constant {
  return {v, SqlType{std::in_range<std::int32_t>(v) ? SqlKind::Integer : SqlKind::BigInt}, false, {}, pos};
}

// A number as PostgreSQL's numeric input reads it, [digits][.digits][e[+-]n],
// at the scale it is written with: the digits after the point, less the
// exponent, and none below 0 (".2E+2" is 20, "2.2E-2" 0.022 at scale 3).
auto numeric_constant(std::string_view written, std::int32_t pos) -> Constant {
  auto text = written;
  auto negative = !text.empty() && text.front() == '-';  // PostgreSQL's grammar folds a minus into the literal
  if (negative) text.remove_prefix(1);
  auto e_at = text.find_first_of("eE");
  auto mantissa = text.substr(0, e_at);
  std::int64_t exp = 0;
  if (e_at != std::string_view::npos) {
    auto x = text.substr(e_at + 1);
    if (!x.empty() && x.front() == '+') x.remove_prefix(1);
    auto [end, ec] = std::from_chars(x.data(), x.data() + x.size(), exp);
    if (x.empty() || ec != std::errc{} || end != x.data() + x.size()) not_yet("the numeric literal " + std::string{written}, pos);
  }
  auto dot = mantissa.find('.');
  auto frac = dot == std::string_view::npos ? std::int64_t{0} : std::ssize(mantissa) - std::ssize(mantissa.substr(0, dot)) - 1;
  std::string digits;
  for (auto ch : mantissa) {
    if (ch != '.') digits.push_back(ch);
  }
  // Too far from 0 either way to be 18 digits at its scale.
  if (exp > 18 || exp < -18) not_yet("a numeric literal of more than 18 digits", pos);
  auto scale = std::max<std::int64_t>(0, frac - exp);
  auto shift = exp - frac + scale;  // the zeros after the digits: >= 0
  auto first = digits.find_first_not_of('0');
  auto significant = first == std::string::npos ? 0 : std::ssize(digits) - static_cast<std::ptrdiff_t>(first);
  if (significant + shift > kMaxDecimalPrecision || scale > kMaxDecimalPrecision) {
    not_yet("a numeric literal of more than 18 digits", pos);
  }
  std::int64_t v = 0;
  for (auto ch : digits) v = v * 10 + (ch - '0');
  v *= kPowersOfTen[static_cast<std::size_t>(shift)];
  return {negative ? -v : v, numeric_type(static_cast<std::uint8_t>(scale)), false, {}, pos};
}

auto literal_constant(const ast::Literal& l) -> Constant {
  using K = ast::Literal::Kind;
  switch (l.kind) {
    case K::Null: return {std::monostate{}, SqlType{SqlKind::Text}, true, {}, l.pos};
    case K::Bool: return {l.b, SqlType{SqlKind::Boolean}, false, {}, l.pos};
    case K::String: {
      auto b = std::as_bytes(std::span{l.text.data(), l.text.size()});
      return {Bytes(b.begin(), b.end()), SqlType{SqlKind::Text}, true, l.text, l.pos};
    }
    case K::Integer: {
      std::int64_t v = 0;
      auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
      if (ec == std::errc{} && end == l.text.data() + l.text.size()) return integer_constant(v, l.pos);
      return numeric_constant(l.text, l.pos);  // PostgreSQL types an integer past BIGINT as numeric
    }
    case K::Number: return numeric_constant(l.text, l.pos);
  }
  std::unreachable();
}

auto is_constant(const ast::Expr& e) -> bool {
  using K = ast::Expr::Kind;
  switch (e.kind) {
    case K::Const:
    case K::Literal: return true;
    case K::Plus:
    case K::Neg: return is_constant(*e.rhs);
    case K::Add:
    case K::Sub:
    case K::Mul:
    case K::Div:
    case K::Compare: return is_constant(*e.lhs) && is_constant(*e.rhs);
    case K::Cast: return is_constant(*e.rhs);
    case K::NullIf: return is_constant(*e.lhs) && is_constant(*e.rhs);
    case K::Coalesce:
    case K::Func: return std::ranges::all_of(e.args, is_constant);
    case K::Case:
      return (!e.lhs || is_constant(*e.lhs)) && (!e.rhs || is_constant(*e.rhs)) && std::ranges::all_of(e.args, is_constant);
    case K::Column:
    case K::Param:
    case K::Aggregate:
    case K::Star:
    case K::Subquery: return false;
  }
  return false;
}

// An integer or numeric constant as a numeric: its scaled value and scale.
struct Scaled {
  std::int64_t v;
  std::uint8_t scale;
};

auto scaled(const Constant& c) -> Scaled { return {std::get<std::int64_t>(c.value), c.type.kind == SqlKind::Decimal ? c.type.scale : std::uint8_t{0}}; }

// v at a scale `up` places finer, or nullopt past 18 digits.
auto rescale(std::int64_t v, std::uint8_t up) -> std::optional<std::int64_t> {
  std::int64_t out = 0;
  if (__builtin_mul_overflow(v, kPowersOfTen[up], &out) || out >= kDecimalLimit || out <= -kDecimalLimit) return std::nullopt;
  return out;
}

enum class Arith { Add, Sub, Mul, Div };

constexpr auto arith_text(Arith op) -> std::string_view {
  switch (op) {
    case Arith::Add: return "+";
    case Arith::Sub: return "-";
    case Arith::Mul: return "*";
    case Arith::Div: return "/";
  }
  return "+";
}

// x op y, of integer or numeric constants, as PostgreSQL's operators type
// it: INTEGER of two INTEGERs, BIGINT of two integers otherwise; with a
// numeric, + and - at the finer scale and * at the sum of the scales; the
// range checked as there (22003). Integer / truncates toward zero; a
// numeric quotient has PostgreSQL's own scale rule, not taken yet. A zero
// divisor is 22012, a NULL operand NULL (a divisor of NULL first: strict
// operators do not run). NULL beside a number is NULL of the number's type.
auto arithmetic(const Constant& x, const Constant& y, Arith op, std::int32_t pos) -> Constant {
  auto text = std::string{arith_text(op)};
  auto null_untyped = [](const Constant& c) { return c.unknown && std::holds_alternative<std::monostate>(c.value); };
  if (null_untyped(x) && null_untyped(y)) not_yet("NULL " + text + " NULL", pos);
  for (const auto* c : {&x, &y}) {
    if (c->unknown && !null_untyped(*c)) not_yet("a string literal in arithmetic", c->pos);
    if (!c->unknown && !is_integer(c->type) && c->type.kind != SqlKind::Decimal) {
      throw SqlError{"operator does not exist: " + pg_type_name(x.type) + " " + text + " " + pg_type_name(y.type), pos,
                     kUndefinedFunction};
    }
  }
  auto both_integer = !null_untyped(x) && !null_untyped(y) && is_integer(x.type) && is_integer(y.type);
  auto type = [&] {
    if (null_untyped(x)) return y.type;
    if (null_untyped(y)) return x.type;
    if (both_integer) {
      return SqlType{x.type.kind == SqlKind::Integer && y.type.kind == SqlKind::Integer ? SqlKind::Integer : SqlKind::BigInt};
    }
    if (op == Arith::Mul) return numeric_type(static_cast<std::uint8_t>(scaled(x).scale + scaled(y).scale));
    return numeric_type(std::max(scaled(x).scale, scaled(y).scale));
  }();
  auto is_null = [](const Constant& c) { return std::holds_alternative<std::monostate>(c.value); };
  auto is_zero = [&](const Constant& c) { return !is_null(c) && std::get<std::int64_t>(c.value) == 0; };
  if (op == Arith::Div && !is_null(x) && is_zero(y)) throw SqlError{"division by zero", pos, kDivisionByZero};
  if (is_null(x) || is_null(y)) {
    if (op == Arith::Div && !both_integer && !null_untyped(x) && !null_untyped(y)) not_yet("division of numerics", pos);
    return {std::monostate{}, type, false, {}, pos};
  }
  auto overflow_error = [&] {
    return SqlError{type.kind == SqlKind::Integer ? "integer out of range" : "bigint out of range", pos, kNumericValueOutOfRange};
  };
  std::int64_t out = 0;
  if (is_integer(type)) {
    auto a = std::get<std::int64_t>(x.value);
    auto b = std::get<std::int64_t>(y.value);
    auto overflow = false;
    switch (op) {
      case Arith::Add: overflow = __builtin_add_overflow(a, b, &out); break;
      case Arith::Sub: overflow = __builtin_sub_overflow(a, b, &out); break;
      case Arith::Mul: overflow = __builtin_mul_overflow(a, b, &out); break;
      case Arith::Div:
        overflow = a == std::numeric_limits<std::int64_t>::min() && b == -1;
        if (!overflow) out = a / b;
        break;
    }
    if (overflow || (type.kind == SqlKind::Integer && !std::in_range<std::int32_t>(out))) throw overflow_error();
    return {out, type, false, {}, pos};
  }
  if (op == Arith::Div) not_yet("division of numerics", pos);
  if (op == Arith::Mul) {
    // The product's scale is the sum of the scales: no rescaling, only the 18 digits.
    if (__builtin_mul_overflow(scaled(x).v, scaled(y).v, &out) || out >= kDecimalLimit || out <= -kDecimalLimit) {
      not_yet("a numeric of more than 18 digits", pos);
    }
    return {out, type, false, {}, pos};
  }
  auto a = rescale(scaled(x).v, static_cast<std::uint8_t>(type.scale - scaled(x).scale));
  auto b = rescale(scaled(y).v, static_cast<std::uint8_t>(type.scale - scaled(y).scale));
  if (!a || !b) not_yet("a numeric of more than 18 digits", pos);
  out = op == Arith::Sub ? *a - *b : *a + *b;  // each below 10^18: no overflow
  if (out >= kDecimalLimit || out <= -kDecimalLimit) not_yet("a numeric of more than 18 digits", pos);
  return {out, type, false, {}, pos};
}

// A range check's message as a SqlError, with its SQLSTATE.
[[noreturn]] void out_of_type(const std::string& why, std::int32_t pos) {
  throw SqlError{why, pos, known_sqlstate(range_sqlstate(why)).value_or(kNumericValueOutOfRange)};
}

auto is_number(SqlType t) -> bool { return is_integer(t) || t.kind == SqlKind::Decimal; }
auto is_float(SqlType t) -> bool { return t.kind == SqlKind::Real || t.kind == SqlKind::Double; }
auto is_string(SqlType t) -> bool { return t.kind == SqlKind::Text || t.kind == SqlKind::Varchar; }

auto bytes_of(std::string_view t) -> Bytes {
  auto b = std::as_bytes(std::span{t.data(), t.size()});
  return {b.begin(), b.end()};
}

// Text read as a value of type t, as t's input function reads it, the
// range checked: what an unknown literal is where it meets t. `assigned`
// rounds a DECIMAL finer than its scale, as a cast or an assignment does.
auto read_as(const Constant& k, SqlType t, bool assigned) -> Datum {
  Datum d;
  try {
    d = from_text(k.text, t, assigned);
  } catch (const TextError& e) {
    throw SqlError{e.what(), k.pos, known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation)};
  }
  if (auto why = out_of_range(t, d)) out_of_type(*why, k.pos);
  return d;
}

// An unknown literal beside a constant of type t, as PostgreSQL resolves
// it: of t's type; a numeric at the scale its text is written with.
auto resolved(const Constant& k, SqlType t) -> Constant {
  if (std::holds_alternative<std::monostate>(k.value)) return {std::monostate{}, t, false, {}, k.pos};
  if (t.kind == SqlKind::Decimal) {
    auto text = std::string_view{k.text};
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    auto body = text.substr(!text.empty() && text.front() == '-' ? 1 : 0);
    auto e_at = body.find_first_of("eE");
    auto mantissa = body.substr(0, e_at);
    auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    auto points = std::ranges::count(mantissa, '.');
    auto exponent_ok = e_at == std::string_view::npos ||
                       (e_at + 1 < body.size() && std::ranges::all_of(body.substr(e_at + 1 + (body[e_at + 1] == '+' || body[e_at + 1] == '-')), digit) &&
                        e_at + 1 + (body[e_at + 1] == '+' || body[e_at + 1] == '-') < body.size());
    if (points > 1 || mantissa == "." || mantissa.empty() || !exponent_ok ||
        !std::ranges::all_of(mantissa, [&](char ch) { return digit(ch) || ch == '.'; })) {
      throw SqlError{"invalid input syntax for type numeric: \"" + k.text + "\"", k.pos, kInvalidTextRepresentation};
    }
    auto c = numeric_constant(text, k.pos);
    return c;
  }
  return {read_as(k, t, false), t, false, {}, k.pos};
}

constexpr auto value_op_text(ast::ValueCompare op) -> std::string_view {
  switch (op) {
    case ast::ValueCompare::Eq: return "=";
    case ast::ValueCompare::Ne: return "<>";
    case ast::ValueCompare::Lt: return "<";
    case ast::ValueCompare::Le: return "<=";
    case ast::ValueCompare::Gt: return ">";
    case ast::ValueCompare::Ge: return ">=";
  }
  return "=";
}

// The order of two values: below 0, 0 or above 0, as PostgreSQL's operator
// of their types orders them: numbers exactly, a float as a double with
// NaN above every value and equal to itself, strings and bytes bytewise
// (the layer's one collation, COLLATE "C"), FALSE before TRUE, a date or
// time by its value.
auto order(const Constant& x, const Constant& y, ast::ValueCompare op, std::int32_t pos) -> int {
  auto refuse = [&] [[noreturn]] {
    throw SqlError{"operator does not exist: " + pg_type_name(x.type) + " " + std::string{value_op_text(op)} + " " +
                       pg_type_name(y.type),
                   pos, kUndefinedFunction};
  };
  auto sign = [](auto a, auto b) { return a < b ? -1 : a > b ? 1 : 0; };
  if (is_number(x.type) && is_number(y.type)) {
    auto [a, sa] = scaled(x);
    auto [b, sb] = scaled(y);
    auto wa = static_cast<__int128>(a);
    auto wb = static_cast<__int128>(b);
    // Each below 10^19 at a scale of at most 18: aligned, within 10^37.
    for (auto i = sa; i < sb; ++i) wa *= 10;
    for (auto i = sb; i < sa; ++i) wb *= 10;
    return sign(wa, wb);
  }
  if ((is_float(x.type) || is_number(x.type)) && (is_float(y.type) || is_number(y.type))) {
    auto as_double = [](const Constant& c) {
      if (is_float(c.type)) return std::get<double>(c.value);
      auto text = to_text(c.value, c.type);
      double d = 0;
      (void)std::from_chars(text.data(), text.data() + text.size(), d);
      return d;
    };
    auto a = as_double(x);
    auto b = as_double(y);
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b) ? 0 : std::isnan(a) ? 1 : -1;
    return sign(a, b);
  }
  if ((is_string(x.type) && is_string(y.type)) || (x.type.kind == SqlKind::Bytea && y.type.kind == SqlKind::Bytea)) {
    const auto& a = std::get<Bytes>(x.value);
    const auto& b = std::get<Bytes>(y.value);
    auto c = std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
    return std::is_lt(c) ? -1 : std::is_gt(c) ? 1 : 0;
  }
  if (x.type.kind == SqlKind::Boolean && y.type.kind == SqlKind::Boolean) {
    return sign(std::get<bool>(x.value) ? 1 : 0, std::get<bool>(y.value) ? 1 : 0);
  }
  if (type_class(x.type.kind) == TypeClass::Temporal && type_class(y.type.kind) == TypeClass::Temporal) {
    if (x.type.kind != y.type.kind) not_yet("comparing a " + pg_type_name(x.type) + " with a " + pg_type_name(y.type), pos);
    return sign(std::get<std::int64_t>(x.value), std::get<std::int64_t>(y.value));
  }
  refuse();
}

// x op y, of constants: a BOOLEAN, NULL if either is. An unknown literal
// takes the other side's type; two compare as text.
auto compare_constants(Constant x, Constant y, ast::ValueCompare op, std::int32_t pos) -> Constant {
  if (!x.unknown && !y.unknown && type_class(x.type.kind) != type_class(y.type.kind)) {
    throw SqlError{"operator does not exist: " + pg_type_name(x.type) + " " + std::string{value_op_text(op)} + " " +
                       pg_type_name(y.type),
                   pos, kUndefinedFunction};
  }
  if (x.unknown && y.unknown) {
    x.unknown = y.unknown = false;  // text: their bytes, or NULL
  } else if (x.unknown) {
    x = resolved(x, y.type);
  } else if (y.unknown) {
    y = resolved(y, x.type);
  }
  if (std::holds_alternative<std::monostate>(x.value) || std::holds_alternative<std::monostate>(y.value)) {
    return {std::monostate{}, SqlType{SqlKind::Boolean}, false, {}, pos};
  }
  auto o = order(x, y, op, pos);
  auto truth = [&] {
    switch (op) {
      case ast::ValueCompare::Eq: return o == 0;
      case ast::ValueCompare::Ne: return o != 0;
      case ast::ValueCompare::Lt: return o < 0;
      case ast::ValueCompare::Le: return o <= 0;
      case ast::ValueCompare::Gt: return o > 0;
      case ast::ValueCompare::Ge: return o >= 0;
    }
    return false;
  }();
  return {truth, SqlType{SqlKind::Boolean}, false, {}, pos};
}

// type 'text', '…'::type and CAST('…' AS type): a string literal read as
// the type, as its input function reads it, a DECIMAL rounded to its scale;
// NULL of the type. A cast of anything else, and to VARCHAR(n), which
// PostgreSQL's cast truncates, are not taken yet.
auto cast_constant(const ast::Expr& e, const Constant& arg) -> Constant {
  auto t = sql_type(ast::ColumnDefAst{"", e.cast_type, e.cast_typmods, false, false, std::nullopt, e.pos, {}});
  if (t.kind == SqlKind::Varchar) not_yet("a cast to VARCHAR(n)", e.pos);
  if (!arg.unknown) not_yet("a cast of other than a string literal or NULL", e.pos);
  if (std::holds_alternative<std::monostate>(arg.value)) return {std::monostate{}, t, false, {}, e.pos};
  return {read_as(arg, t, true), t, false, {}, e.pos};
}

auto fold(const ast::Expr& e) -> Constant;

// A conditional's branch, folded. A runtime error (SQLSTATE class 22) is
// kept, not thrown: PostgreSQL raises it only if the branch is evaluated,
// and a branch the conditional does not reach is not.
struct Branch {
  std::optional<Constant> value;
  std::optional<SqlError> error;
};

auto fold_branch(const ast::Expr& e) -> Branch {
  try {
    return {fold(e), std::nullopt};
  } catch (const SqlError& err) {
    if (!err.sqlstate().starts_with("22")) throw;
    return {std::nullopt, err};
  }
}

// The type of CASE, COALESCE or the branches of one, as PostgreSQL's union
// rules resolve it: the unknown literals give way to the others, which
// must be of one class (42804); integers widen to the widest, a numeric
// to a numeric; all unknown is TEXT. `what` names the construct.
auto common_type(const std::vector<const Constant*>& parts, const std::string& what, std::int32_t pos) -> SqlType {
  const Constant* first = nullptr;
  auto type = SqlType{SqlKind::Text};
  for (const auto* c : parts) {
    if (c->unknown) continue;
    if (first == nullptr) {
      first = c;
      type = c->type;
      if (type.kind == SqlKind::Varchar) type = SqlType{SqlKind::Text};
      continue;
    }
    if (type_class(type.kind) != type_class(c->type.kind)) {
      throw SqlError{what + " types " + pg_type_name(type) + " and " + pg_type_name(c->type) + " cannot be matched", c->pos,
                     kDatatypeMismatch};
    }
    if (is_float(type) || is_float(c->type)) not_yet(what + " of floating-point values", pos);
    if (is_number(type)) {
      if (type.kind == SqlKind::Decimal || c->type.kind == SqlKind::Decimal) {
        type = numeric_type(0);  // NUMERIC of no scale: each value keeps its own, as PostgreSQL's
      } else {
        auto rank = [](SqlType t) { return t.kind == SqlKind::SmallInt ? 0 : t.kind == SqlKind::Integer ? 1 : 2; };
        if (rank(c->type) > rank(type)) type = c->type;
      }
    } else if (type.kind != c->type.kind && !(is_string(type) && is_string(c->type))) {
      not_yet(what + " of " + pg_type_name(type) + " and " + pg_type_name(c->type), pos);
    }
  }
  return type;
}

// c as a value of the common type t: an unknown literal read as t, an
// integer widened to a numeric, which keeps its value and so its scale.
auto as_common(const Constant& c, SqlType t) -> Constant {
  auto out = c.unknown ? resolved(c, t) : c;
  out.unknown = false;
  out.pos = c.pos;
  if (std::holds_alternative<std::monostate>(out.value)) {
    out.type = t;
    return out;
  }
  out.type = t.kind == SqlKind::Decimal ? numeric_type(scaled(out).scale) : t;
  return out;
}

// The result of a conditional: the selected branch (nullptr for NULL),
// typed with the common type of `parts`, every branch counted. A branch
// that raises and is not selected leaves the type unknown to us: refused.
auto conditional_result(const std::vector<Branch>& parts, const Branch* selected, const std::string& what, std::int32_t pos)
    -> Constant {
  std::vector<const Constant*> known;
  for (const auto& b : parts) {
    if (b.error && &b != selected) not_yet("a branch of " + what + " that raises an error and is not taken", pos);
    if (b.value) known.push_back(&*b.value);
  }
  if (selected != nullptr && selected->error) throw *selected->error;
  auto type = common_type(known, what, pos);
  for (const auto* c : known) (void)as_common(*c, type);  // an unknown literal that is not a value of the type: 22P02
  if (selected == nullptr) return {std::monostate{}, type, false, {}, pos};
  return as_common(*selected->value, type);
}

auto is_null(const Constant& c) -> bool { return std::holds_alternative<std::monostate>(c.value); }

// CASE WHEN c THEN r … [ELSE r] END, or CASE x WHEN v THEN r … END, which
// tests x = v. Only the branch taken is evaluated; a NULL condition is not
// true.
auto fold_case(const ast::Expr& e) -> Constant {
  std::optional<Constant> operand;
  if (e.lhs) operand = fold(*e.lhs);
  std::vector<Branch> conds;
  std::vector<Branch> parts;  // each THEN, then the ELSE
  for (std::size_t i = 0; i + 1 < e.args.size(); i += 2) {
    conds.push_back(fold_branch(e.args[i]));
    parts.push_back(fold_branch(e.args[i + 1]));
  }
  if (e.rhs) parts.push_back(fold_branch(*e.rhs));
  const Branch* selected = e.rhs ? &parts.back() : nullptr;
  for (std::size_t i = 0; i < conds.size(); ++i) {
    if (conds[i].error) throw *conds[i].error;
    auto truth = operand ? compare_constants(*operand, *conds[i].value, ast::ValueCompare::Eq, e.args[2 * i].pos) : *conds[i].value;
    if (!operand) {
      if (truth.unknown && !is_null(truth)) not_yet("a string literal as a CASE condition", truth.pos);
      if (!truth.unknown && truth.type.kind != SqlKind::Boolean) {
        throw SqlError{"argument of CASE/WHEN must be type boolean, not type " + pg_type_name(truth.type), truth.pos,
                       kDatatypeMismatch};
      }
    }
    if (!is_null(truth) && std::get<bool>(truth.value)) {
      selected = &parts[i];
      break;
    }
  }
  return conditional_result(parts, selected, "CASE", e.pos);
}

// COALESCE(a, b, …): the first argument that is not NULL, each evaluated
// only if the ones before it are NULL.
auto fold_coalesce(const ast::Expr& e) -> Constant {
  std::vector<Branch> parts;
  for (const auto& a : e.args) parts.push_back(fold_branch(a));
  const Branch* selected = nullptr;
  for (const auto& b : parts) {
    if (b.error) throw *b.error;
    if (!is_null(*b.value)) {
      selected = &b;
      break;
    }
  }
  return conditional_result(parts, selected, "COALESCE", e.pos);
}

// NULLIF(a, b): NULL if a = b, else a, of a's type (an unknown literal
// takes b's).
auto fold_nullif(const ast::Expr& e) -> Constant {
  auto a = fold(*e.lhs);
  auto b = fold(*e.rhs);
  auto equal = compare_constants(a, b, ast::ValueCompare::Eq, e.pos);
  if (a.unknown) a = resolved(a, b.unknown ? SqlType{SqlKind::Text} : b.type);
  a.unknown = false;
  a.pos = e.pos;
  if (!is_null(equal) && std::get<bool>(equal.value)) return {std::monostate{}, a.type, false, {}, e.pos};
  return a;
}

// String functions and ||. Text is UTF-8 and counted in characters, as
// PostgreSQL's is; UPPER and LOWER fold ASCII only, the layer's one
// collation being COLLATE "C".

auto utf8_width(std::string_view s, std::size_t i) -> std::size_t {
  auto b = static_cast<unsigned char>(s[i]);
  auto w = b < 0xC0 ? 1U : b < 0xE0 ? 2U : b < 0xF0 ? 3U : 4U;  // a stray continuation byte is a character of its own
  return std::min<std::size_t>(w, s.size() - i);
}

auto char_count(std::string_view s) -> std::int64_t {
  std::int64_t n = 0;
  for (std::size_t i = 0; i < s.size(); i += utf8_width(s, i)) ++n;
  return n;
}

// The byte offset of the character `n` characters in; s.size() past the end.
auto char_offset(std::string_view s, std::int64_t n) -> std::size_t {
  std::size_t i = 0;
  while (n-- > 0 && i < s.size()) i += utf8_width(s, i);
  return i;
}

[[noreturn]] void substring_error(std::int32_t pos) {
  throw SqlError{"negative substring length not allowed", pos, kSubstringError};
}

// PostgreSQL's text_substring: `count` characters from the 1-based `start`
// (a start before the first character takes from it, the characters before
// it counting toward `count`), to the end where no count is given.
auto substring_of(std::string_view s, std::int32_t start, std::optional<std::int32_t> count, std::int32_t pos) -> std::string {
  auto first = std::max<std::int64_t>(start, 1);
  std::optional<std::int64_t> chars;
  if (count) {
    if (*count < 0) substring_error(pos);
    auto end = static_cast<std::int64_t>(start) + *count;
    if (end <= std::numeric_limits<std::int32_t>::max()) {  // past that, to the end
      if (end < 1) return {};
      chars = end - first;
    }
  }
  auto from = char_offset(s, first - 1);
  auto to = chars ? from + char_offset(s.substr(from), *chars) : s.size();
  return std::string{s.substr(from, to - from)};
}

auto overlay_of(std::string_view s, std::string_view replacement, std::int32_t start, std::optional<std::int32_t> count,
                std::int32_t pos) -> std::string {
  auto replaced = static_cast<std::int64_t>(count ? *count : static_cast<std::int32_t>(char_count(replacement)));
  if (start <= 0) substring_error(pos);
  auto resume = start + replaced;
  if (!std::in_range<std::int32_t>(resume)) throw SqlError{"integer out of range", pos, kNumericValueOutOfRange};
  return substring_of(s, 1, start - 1, pos) + std::string{replacement} +
         substring_of(s, static_cast<std::int32_t>(resume), std::nullopt, pos);
}

// 1-based character position of the first `needle` in `s`, 0 if none; an
// empty needle is found at 1.
auto position_of(std::string_view needle, std::string_view s) -> std::int64_t {
  auto at = s.find(needle);
  return at == std::string_view::npos ? 0 : char_count(s.substr(0, at)) + 1;
}

// s without the characters of `set` from the front, the back, or both.
auto trimmed(std::string_view s, std::string_view set, bool front, bool back) -> std::string {
  auto in_set = [&](std::string_view ch) {
    for (std::size_t i = 0; i < set.size(); i += utf8_width(set, i)) {
      if (set.substr(i, utf8_width(set, i)) == ch) return true;
    }
    return false;
  };
  std::size_t b = 0;
  std::size_t e = s.size();
  while (front && b < e && in_set(s.substr(b, utf8_width(s, b)))) b += utf8_width(s, b);
  while (back && e > b) {
    auto i = e - 1;
    while (i > b && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    if (!in_set(s.substr(i, e - i))) break;
    e = i;
  }
  return std::string{s.substr(b, e - b)};
}

auto ascii_case(std::string s, bool upper) -> std::string {
  for (auto& ch : s) {
    if (upper && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (!upper && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return s;
}

[[noreturn]] void no_function(const std::string& name, const std::vector<Constant>& args, std::int32_t pos) {
  std::string list;
  for (const auto& a : args) list += (list.empty() ? "" : ", ") + (a.unknown ? std::string{"unknown"} : pg_type_name(a.type));
  throw SqlError{"function " + name + "(" + list + ") does not exist", pos, kUndefinedFunction};
}

enum class Slot { Text, Int4 };

// The arguments as the function's parameters, as PostgreSQL's resolution
// reads them: an unknown literal is of the parameter's type, a string or
// smallint/integer is taken as it is, anything else matches no function
// (42883). `bytea_overload`: the function also takes bytea, not taken yet.
auto bind_args(const std::string& name, std::vector<Constant> args, const std::vector<Slot>& slots, bool bytea_overload,
               std::int32_t pos) -> std::vector<Constant> {
  const auto original = args;
  auto slot = slots.begin();
  for (const auto& a : original) {
    auto fits = a.unknown || (*slot == Slot::Text ? is_string(a.type) : a.type.kind == SqlKind::SmallInt || a.type.kind == SqlKind::Integer);
    if (!fits && *slot == Slot::Text && a.type.kind == SqlKind::Bytea && bytea_overload) not_yet(name + " of bytea", pos);
    if (!fits) no_function(name, original, pos);
    ++slot;
  }
  slot = slots.begin();
  for (auto& a : args) {
    if (a.unknown) {
      auto t = SqlType{*slot == Slot::Text ? SqlKind::Text : SqlKind::Integer};
      if (!is_null(a) && *slot == Slot::Int4) a.value = read_as(a, t, false);
      a.type = t;
      a.unknown = false;
    }
    ++slot;
  }
  return args;
}

// A value as the text PostgreSQL's cast to text makes of it.
auto text_of(const Constant& c) -> std::string {
  if (c.unknown) return c.text;
  if (c.type.kind == SqlKind::Boolean) return std::get<bool>(c.value) ? "true" : "false";
  return to_text(c.value, c.type);
}

auto text_constant(std::string s, std::int32_t pos) -> Constant {
  return {bytes_of(s), SqlType{SqlKind::Text}, false, {}, pos};
}

auto null_constant(SqlKind kind, std::int32_t pos) -> Constant { return {std::monostate{}, SqlType{kind}, false, {}, pos}; }

auto int_of(const Constant& c) -> std::int32_t { return static_cast<std::int32_t>(std::get<std::int64_t>(c.value)); }

// a || b: text with text, or with any other type through its text (so 'a' ||
// 1 is 'a1'); bytea with bytea. Two values neither of which is a string
// match no operator.
auto fold_concat(const Constant& a, const Constant& b, std::int32_t pos) -> Constant {
  auto is_bytea = [](const Constant& c) { return !c.unknown && c.type.kind == SqlKind::Bytea; };
  if (is_bytea(a) && is_bytea(b)) {
    if (is_null(a) || is_null(b)) return null_constant(SqlKind::Bytea, pos);
    auto out = std::get<Bytes>(a.value);
    const auto& tail = std::get<Bytes>(b.value);
    out.insert(out.end(), tail.begin(), tail.end());
    return {std::move(out), SqlType{SqlKind::Bytea}, false, {}, pos};
  }
  auto stringish = [](const Constant& c) { return c.unknown || is_string(c.type); };
  if (!stringish(a) && !stringish(b)) {
    throw SqlError{"operator does not exist: " + pg_type_name(a.type) + " || " + pg_type_name(b.type), pos, kUndefinedFunction};
  }
  for (const auto* c : {&a, &b}) {
    if (is_bytea(*c)) not_yet("|| of bytea and a string", pos);
    if (!c->unknown && is_float(c->type)) not_yet("|| of a floating-point value", pos);
  }
  if (is_null(a) || is_null(b)) return null_constant(SqlKind::Text, pos);
  return text_constant(text_of(a) + text_of(b), pos);
}

// A call of one of the string functions on constants. All arguments are
// evaluated and typed first; a NULL argument then gives NULL (the functions
// are strict).
auto fold_function(const ast::Expr& e) -> Constant {
  std::vector<Constant> given;
  for (const auto& a : e.args) given.push_back(fold(a));
  const auto& name = e.func;
  if (name == "||") return fold_concat(given[0], given[1], e.pos);
  auto arity = [&](std::size_t lo, std::size_t hi) {
    if (given.size() < lo || given.size() > hi) no_function(name, given, e.pos);
  };
  auto strict = [&](const std::vector<Constant>& args, SqlKind result) -> std::optional<Constant> {
    if (std::ranges::any_of(args, is_null)) return null_constant(result, e.pos);
    return std::nullopt;
  };
  if (name == "upper" || name == "lower") {
    arity(1, 1);
    auto args = bind_args(name, given, std::vector{Slot::Text}, false, e.pos);
    if (auto n = strict(args, SqlKind::Text)) return *n;
    return text_constant(ascii_case(text_of(args[0]), name == "upper"), e.pos);
  }
  if (name == "char_length" || name == "character_length" || name == "octet_length") {
    arity(1, 1);
    auto args = bind_args(name, given, std::vector{Slot::Text}, name == "octet_length", e.pos);
    if (auto n = strict(args, SqlKind::Integer)) return *n;
    auto s = text_of(args[0]);
    return {name == "octet_length" ? static_cast<std::int64_t>(s.size()) : char_count(s), SqlType{SqlKind::Integer}, false, {}, e.pos};
  }
  if (name == "substring") {
    arity(2, 3);
    // SUBSTRING(s FROM 'x' [FOR '#']) is PostgreSQL's regular-expression
    // substring: the only function that takes the operands where they are
    // all strings or unknown literals.
    auto rest = std::span{given}.subspan(1);
    if (std::ranges::all_of(rest, [](const Constant& c) { return c.unknown || is_string(c.type); })) {
      not_yet("SUBSTRING with a pattern (a regular expression)", e.pos);
    }
    auto args = bind_args(name, given, given.size() == 2 ? std::vector{Slot::Text, Slot::Int4} : std::vector{Slot::Text, Slot::Int4, Slot::Int4},
                          true, e.pos);
    if (auto n = strict(args, SqlKind::Text)) return *n;
    return text_constant(substring_of(text_of(args[0]), int_of(args[1]),
                                      args.size() == 3 ? std::optional{int_of(args[2])} : std::nullopt, e.pos),
                         e.pos);
  }
  if (name == "btrim" || name == "ltrim" || name == "rtrim") {
    arity(1, 2);
    auto args = bind_args(name, given, given.size() == 1 ? std::vector{Slot::Text} : std::vector{Slot::Text, Slot::Text},
                          true, e.pos);
    if (auto n = strict(args, SqlKind::Text)) return *n;
    auto set = args.size() == 2 ? text_of(args[1]) : std::string{" "};
    return text_constant(trimmed(text_of(args[0]), set, name != "rtrim", name != "ltrim"), e.pos);
  }
  if (name == "position") {
    arity(2, 2);
    auto args = bind_args(name, given, std::vector{Slot::Text, Slot::Text}, true, e.pos);
    if (auto n = strict(args, SqlKind::Integer)) return *n;
    return {position_of(text_of(args[1]), text_of(args[0])), SqlType{SqlKind::Integer}, false, {}, e.pos};
  }
  if (name == "overlay") {
    arity(3, 4);
    auto args = bind_args(name, given,
                          given.size() == 3 ? std::vector{Slot::Text, Slot::Text, Slot::Int4}
                                            : std::vector{Slot::Text, Slot::Text, Slot::Int4, Slot::Int4},
                          true, e.pos);
    if (auto n = strict(args, SqlKind::Text)) return *n;
    return text_constant(overlay_of(text_of(args[0]), text_of(args[1]), int_of(args[2]),
                                    args.size() == 4 ? std::optional{int_of(args[3])} : std::nullopt, e.pos),
                         e.pos);
  }
  no_function(name, given, e.pos);
}

// The constant expression e, folded.
auto fold(const ast::Expr& e) -> Constant {
  using K = ast::Expr::Kind;
  switch (e.kind) {
    case K::Const: return integer_constant(e.constant, e.pos);
    case K::Literal: return literal_constant(e.literal);
    case K::Plus:
    case K::Neg: {
      auto c = fold(*e.rhs);
      if (c.unknown) not_yet(std::string{"unary "} + (e.kind == K::Plus ? "+" : "-") + " of a string literal or NULL", e.pos);
      if (!is_integer(c.type) && c.type.kind != SqlKind::Decimal) {
        throw SqlError{std::string{"operator does not exist: "} + (e.kind == K::Plus ? "+ " : "- ") + pg_type_name(c.type), e.pos,
                       kUndefinedFunction};
      }
      if (e.kind == K::Plus || std::holds_alternative<std::monostate>(c.value)) return c;
      auto v = std::get<std::int64_t>(c.value);
      if (v == std::numeric_limits<std::int64_t>::min() || (c.type.kind == SqlKind::Integer && !std::in_range<std::int32_t>(-v))) {
        throw SqlError{c.type.kind == SqlKind::Integer ? "integer out of range" : "bigint out of range", e.pos,
                       kNumericValueOutOfRange};
      }
      c.value = -v;
      c.pos = e.pos;
      return c;
    }
    case K::Add: return arithmetic(fold(*e.lhs), fold(*e.rhs), Arith::Add, e.pos);
    case K::Sub: return arithmetic(fold(*e.lhs), fold(*e.rhs), Arith::Sub, e.pos);
    case K::Mul: return arithmetic(fold(*e.lhs), fold(*e.rhs), Arith::Mul, e.pos);
    case K::Div: return arithmetic(fold(*e.lhs), fold(*e.rhs), Arith::Div, e.pos);
    case K::Compare: return compare_constants(fold(*e.lhs), fold(*e.rhs), e.cmp, e.pos);
    case K::Cast: return cast_constant(e, fold(*e.rhs));
    case K::Case: return fold_case(e);
    case K::Coalesce: return fold_coalesce(e);
    case K::NullIf: return fold_nullif(e);
    case K::Func: return fold_function(e);
    case K::Column:
    case K::Param:
    case K::Aggregate:
    case K::Star:
    case K::Subquery: break;
  }
  throw SqlError{"expression not supported", e.pos};
}

// The constant as a value of column c's type: assigned to it (INSERT's
// VALUES, SET), with PostgreSQL's assignment casts, which round a numeric
// half away from zero and write a number or a boolean as its text; or
// compared with it by `op`. A compared constant is taken only where c's
// type holds it exactly, and refused otherwise (42601): PostgreSQL then
// compares in a wider type, which the layer's keys don't
// (docs/frontend_design.md, *Known differences*). A string literal or NULL
// is read as c's type, as PostgreSQL reads an unknown literal. The range of
// an assigned value is its caller's to check.
auto coerce(const Constant& k, const Column& c, bool assigned, std::string_view op) -> Datum {
  auto inexact = [&] [[noreturn]] {
    not_yet("comparing " + c.name + " (" + sql_type_name(c.sql) + ") with a constant its type does not hold exactly", k.pos);
  };
  if (k.unknown) {
    if (std::holds_alternative<std::monostate>(k.value)) return std::monostate{};
    Datum d;
    try {
      d = from_text(k.text, c.sql, assigned);
    } catch (const TextError& e) {
      // A number finer than a DECIMAL's scale reads only as assigned: in a
      // comparison it is a constant the type does not hold exactly.
      if (!assigned && c.sql.kind == SqlKind::Decimal) {
        try {
          (void)from_text(k.text, c.sql, true);
        } catch (const TextError&) {
          throw SqlError{e.what(), k.pos, known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation)};
        }
        inexact();
      }
      throw SqlError{e.what(), k.pos, known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation)};
    }
    if (!assigned) {
      if (auto why = out_of_range(compared_type(c.sql).range, d)) out_of_type(*why, k.pos);
    }
    return d;
  }
  if (auto refusal = type_refusal(k.type.kind, c.sql.kind, assigned)) {
    if (assigned) {
      throw SqlError{"column \"" + c.name + "\" is of type " + pg_type_name(c.sql) + " but expression is of type " +
                         pg_type_name(k.type),
                     k.pos, *refusal};
    }
    throw SqlError{"operator does not exist: " + pg_type_name(c.sql) + " " + std::string{op} + " " + pg_type_name(k.type),
                   k.pos, *refusal};
  }
  if (std::holds_alternative<std::monostate>(k.value)) return std::monostate{};
  if (k.type.kind == SqlKind::Boolean) {
    auto b = std::get<bool>(k.value);
    if (c.sql.kind == SqlKind::Boolean) return b;
    return bytes_of(b ? "true" : "false");  // assigned to a string: its text
  }
  // A cast's: a string, bytes, a date or time, or a float.
  if (!is_number(k.type)) {
    if (is_string(k.type) && is_string(c.sql)) return k.value;
    if (k.type.kind == c.sql.kind && k.type.kind != SqlKind::Real) return k.value;
    if (assigned && is_string(c.sql) && !is_float(k.type)) return bytes_of(to_text(k.value, k.type));
    not_yet("a " + pg_type_name(k.type) + " constant " + (assigned ? "assigned to " : "compared with ") + "a " +
                pg_type_name(c.sql) + " column",
            k.pos);
  }
  auto [v, scale] = scaled(k);
  auto unit = kPowersOfTen[scale];
  switch (c.sql.kind) {
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt: {
      auto n = v / unit;
      if (auto r = v % unit; r != 0) {
        if (!assigned) inexact();
        if (2 * (r < 0 ? -r : r) >= unit) n += v < 0 ? -1 : 1;
      }
      if (!assigned && out_of_range(compared_type(c.sql).range, n)) inexact();
      return n;
    }
    case SqlKind::Decimal: {
      if (scale <= c.sql.scale) {
        auto n = rescale(v, static_cast<std::uint8_t>(c.sql.scale - scale));
        if (!n && assigned) throw SqlError{"numeric field overflow", k.pos, kNumericValueOutOfRange};
        if (!n) inexact();
        return *n;
      }
      auto d = kPowersOfTen[scale - c.sql.scale];
      auto n = v / d;
      if (auto r = v % d; r != 0) {
        if (!assigned) inexact();
        if (2 * (r < 0 ? -r : r) >= d) n += v < 0 ? -1 : 1;
      }
      return n;
    }
    case SqlKind::Real:
    case SqlKind::Double: {
      // As the type's input reads the number's text: once rounded, a REAL
      // to a float.
      auto text = to_text(Datum{v}, numeric_type(scale));
      if (c.sql.kind == SqlKind::Double) {
        double d = 0;
        (void)std::from_chars(text.data(), text.data() + text.size(), d);
        return d;
      }
      float f = 0;
      (void)std::from_chars(text.data(), text.data() + text.size(), f);
      // PostgreSQL compares a REAL with a number as a double: only one the
      // float holds exactly compares the same.
      if (!assigned && (v % unit != 0 || v / unit > (1 << 24) || v / unit < -(1 << 24))) inexact();
      return static_cast<double>(f);
    }
    case SqlKind::Text:
    case SqlKind::Varchar: {
      auto t = to_text(Datum{v}, numeric_type(scale));
      auto bytes = std::as_bytes(std::span{t.data(), t.size()});
      return Bytes(bytes.begin(), bytes.end());
    }
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Timestamp:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::Numeric: break;  // refused by type_refusal
  }
  throw SqlError{"expression not supported", k.pos};
}

// The largest $n the statement names: its own parameters, which come
// before the constants' (Binder::fixed).
void params_in(const ast::Expr& e, std::uint32_t& n) {
  if (e.kind == ast::Expr::Kind::Param) n = std::max(n, e.param + 1);
  if (e.lhs) params_in(*e.lhs, n);
  if (e.rhs) params_in(*e.rhs, n);
}

void params_in(const ast::Compare& c, std::uint32_t& n) {
  if (c.kind == ast::Compare::Kind::Compare || c.kind == ast::Compare::Kind::IsNull) {
    params_in(c.lhs, n);
    if (c.kind == ast::Compare::Kind::Compare) params_in(c.rhs, n);
  }
  for (const auto& a : c.args) params_in(a, n);
}

auto select_param_count(const ast::Select& s) -> std::uint32_t {
  if (s.set) return std::max(select_param_count(s.set->left), select_param_count(s.set->right));
  std::uint32_t n = 0;
  for (const auto& e : s.targets) params_in(e, n);
  for (const auto& e : s.group_by) params_in(e, n);
  for (const auto& c : s.where) params_in(c, n);
  for (const auto& c : s.having) params_in(c, n);
  return n;
}

auto param_count(const ast::Statement& stmt) -> std::uint32_t {
  std::uint32_t n = 0;
  auto conjuncts = [&](const std::vector<ast::Compare>& cs) {
    for (const auto& c : cs) params_in(c, n);
  };
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ast::Insert>) {
          for (const auto& e : s.values) params_in(e, n);
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          n = select_param_count(s);
        } else if constexpr (std::is_same_v<T, ast::Update>) {
          for (const auto& a : s.set) params_in(a.value, n);
          conjuncts(s.where);
        } else if constexpr (std::is_same_v<T, ast::Delete>) {
          conjuncts(s.where);
        }
      },
      stmt);
  return n;
}

auto column_spec(const ast::ColumnDefAst& c) -> ColumnSpec {
  auto t = sql_type(c);
  return ColumnSpec{c.name, t, !c.not_null && !c.primary_key,
                    c.default_value ? fold_default(*c.default_value, t) : Datum{}};
}

// The keys a FOREIGN KEY may reference: a table's columns, its primary
// key, and the column sets of its unique indexes.
struct ReferencedKeys {
  std::vector<std::pair<std::string, SqlType>> columns;
  std::vector<std::string> pk;  // empty: no primary key
  std::vector<std::vector<std::string>> unique;
};

auto referenced_keys(const Table& t) -> ReferencedKeys {
  ReferencedKeys k;
  for (const auto& c : t.columns) {
    if (!c.hidden) k.columns.emplace_back(c.name, c.sql);
  }
  if (t.row_id() == nullptr) {
    for (auto id : t.pk_col_ids) k.pk.push_back(t.column(id)->name);
  }
  for (const auto& ix : t.indexes) {
    if (!ix.unique) continue;
    auto& cols = k.unique.emplace_back();
    for (const auto& p : ix.parts) cols.push_back(t.column(p.col_id)->name);
  }
  return k;
}

// The keys of the table CREATE TABLE makes, for a FOREIGN KEY on itself.
auto referenced_keys(const CreateTableDdl& t) -> ReferencedKeys {
  ReferencedKeys k;
  for (const auto& c : t.columns) k.columns.emplace_back(c.name, c.sql);
  k.pk = t.pk;
  for (const auto& u : t.unique) k.unique.push_back(u.columns);
  return k;
}

// Whether a column of type `fk` may reference one of type `key`, as
// PostgreSQL decides it: by an equality of the key's type, the column cast
// to it implicitly where they differ. So it goes one way: an INTEGER
// references a DECIMAL or a REAL key, not the reverse.
auto references_type(SqlType fk, SqlType key) -> bool {
  auto is_date = [](SqlType t) { return t.kind == SqlKind::Date || t.kind == SqlKind::Timestamp; };
  if (is_integer(key)) return is_integer(fk);
  if (key.kind == SqlKind::Decimal) return is_integer(fk) || fk.kind == SqlKind::Decimal;
  if (is_float(key)) return is_integer(fk) || fk.kind == SqlKind::Decimal || is_float(fk);
  if (is_string(key)) return is_string(fk);
  if (is_date(key)) return is_date(fk);
  return fk.kind == key.kind;
}

// PostgreSQL's checks of a FOREIGN KEY when it is created: its columns,
// those it references, and a primary key or unique index of exactly those
// columns, in any order, of types an equality compares. Nothing is kept:
// the constraint is not enforced (docs/frontend_design.md, *Known
// differences*).
void check_foreign_key(const CreateTableDdl& t, const ast::ForeignKeyAst& f, const ReferencedKeys& ref) {
  auto type_of = [&](const auto& cols, const std::string& name) -> std::optional<SqlType> {
    for (const auto& [n, ty] : cols) {
      if (n == name) return ty;
    }
    return std::nullopt;
  };
  std::vector<std::pair<std::string, SqlType>> own;
  for (const auto& c : t.columns) own.emplace_back(c.name, c.sql);
  auto missing = [&](const std::string& c) {
    throw SqlError{"column \"" + c + "\" referenced in foreign key constraint does not exist", f.pos, kUndefinedColumn};
  };
  for (const auto& c : f.columns) {
    if (!type_of(own, c)) missing(c);  // one named twice is taken, as PostgreSQL takes it
  }
  auto refs = f.ref_columns;
  if (refs.empty()) {
    if (ref.pk.empty()) throw SqlError{"there is no primary key for referenced table \"" + f.table + "\"", f.pos, kUndefinedObject};
    refs = ref.pk;
  } else {
    for (const auto& c : refs) {
      if (!type_of(ref.columns, c)) missing(c);
      if (std::ranges::count(refs, c) > 1) {
        throw SqlError{"foreign key referenced-columns list must not contain duplicates", f.pos, kInvalidForeignKey};
      }
    }
  }
  if (refs.size() != f.columns.size()) {
    throw SqlError{"number of referencing and referenced columns for foreign key disagree", f.pos, kInvalidForeignKey};
  }
  auto same_set = [&](const std::vector<std::string>& key) {
    return key.size() == refs.size() && std::ranges::all_of(refs, [&](const auto& c) { return std::ranges::contains(key, c); });
  };
  if (!same_set(ref.pk) && std::ranges::none_of(ref.unique, same_set)) {
    throw SqlError{"there is no unique constraint matching given keys for referenced table \"" + f.table + "\"", f.pos,
                   kInvalidForeignKey};
  }
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (!references_type(*type_of(own, f.columns[i]), *type_of(ref.columns, refs[i]))) {
      auto name = f.name;
      if (name.empty()) {
        name = t.name;
        for (const auto& c : f.columns) name += "_" + c;
        name += "_fkey";
      }
      throw SqlError{"foreign key constraint \"" + name + "\" cannot be implemented", f.pos, kDatatypeMismatch};
    }
  }
}

class Binder {
public:
  // `own_params`: how many parameters the statement's text names, $1 to
  // its largest $n, each of which must get a type.
  Binder(const Catalog& cat, std::uint32_t own_params) : cat_{cat}, own_params_{own_params} { params_.resize(own_params); }

  // A constant's value as a parameter the API binds after the statement's
  // own. Its value is checked here or where it is written, so it has no
  // range check before the plan runs.
  auto fixed(Datum v, SqlType t) -> logical::Expr {
    auto n = own_params_ + bytecask::sql::narrow<std::uint32_t>(fixed_.size());
    fixed_.push_back(std::move(v));
    fixed_types_.push_back(phys_type(t));
    return logical::param(n, t);
  }
  [[nodiscard]] auto is_fixed(std::uint32_t n) const -> bool { return n >= own_params_; }
  [[nodiscard]] auto fixed_value(std::uint32_t n) const -> const Datum& { return fixed_.at(n - own_params_); }
  [[nodiscard]] auto fixed_values() const -> const std::vector<Datum>& { return fixed_; }
  [[nodiscard]] auto fixed_types() const -> const std::vector<PhysType>& { return fixed_types_; }
  [[nodiscard]] auto take_subqueries() -> std::vector<logical::Subquery> { return std::move(subqueries_); }
  void set_scope(std::vector<const Table*> tables) { scope_ = std::move(tables); }

  // A subquery's SELECT, bound as any is, then listed to run before the
  // statement's plan (logical::Subquery), its values in hidden parameters
  // after the constants': its index and first parameter. Uncorrelated, one
  // level deep, not FOR UPDATE; of one column, but for EXISTS, whose rows
  // are counted and taken one at most.
  struct BoundSubquery {
    std::uint32_t index;
    std::uint32_t param;
    SqlType type;
    std::string name;
  };
  auto subquery(const ast::Select& q, logical::Subquery::Kind kind, std::int32_t pos) -> BoundSubquery {
    if (depth_ > 0) not_yet("a subquery inside a subquery", pos);
    if (q.for_update) not_yet("FOR UPDATE in a subquery", pos);
    if (correlated(q)) not_yet("a correlated subquery, one that reads a column of the query around it", pos);
    auto using_columns = std::move(using_);
    auto using_alias = std::move(using_alias_);
    auto scope = std::move(scope_);
    ++depth_;
    auto root = select(q);
    --depth_;
    using_ = std::move(using_columns);
    using_alias_ = std::move(using_alias);
    scope_ = std::move(scope);
    using K = logical::Subquery::Kind;
    SqlType type{SqlKind::Boolean};
    std::string name = "?column?";
    if (kind == K::Exists) {
      const auto* l = std::get_if<logical::Limit>(&root->op);
      if (l == nullptr || l->n > 1) root = logical::node(logical::Limit{std::move(root), 1});
    } else {
      const auto& outs = outputs_of(*root);
      if (outs.size() != 1) throw SqlError{"subquery must return only one column", pos};
      type = outs[0].expr.type;
      if (type.kind == SqlKind::Numeric) not_yet("a subquery of AVG", pos);
      name = outs[0].name;
    }
    // Its parameters, NULL until it runs: its value or values, then its flags.
    auto value = [&] { return fixed(Datum{}, type).param; };
    auto flag = [&] { return fixed(Datum{}, SqlType{SqlKind::Boolean}).param; };
    std::uint32_t first = 0;
    switch (kind) {
      case K::Value: first = value(); break;
      case K::Exists: first = flag(); break;
      case K::Bounds:
        first = value();
        (void)value();
        (void)flag();
        (void)flag();
        break;
      case K::Set:
        first = flag();
        (void)flag();
        break;
    }
    auto index = bytecask::sql::narrow<std::uint32_t>(subqueries_.size());
    subqueries_.push_back({std::move(root), kind, type, first});
    return {index, first, type, name};
  }

  // The select list of a bound SELECT: its Project's or its Aggregate's,
  // under any Sort, Limit or HAVING Filter.
  static auto outputs_of(const logical::Node& n) -> const std::vector<logical::Output>& {
    if (const auto* p = std::get_if<logical::Project>(&n.op)) return p->exprs;
    if (const auto* a = std::get_if<logical::Aggregate>(&n.op)) return a->aggs;
    if (const auto* o = std::get_if<logical::SetOp>(&n.op)) return o->outputs;
    const auto* in = logical::input_of(n);
    if (in == nullptr || !*in) throw std::logic_error{"a bound SELECT with no select list"};
    return outputs_of(**in);
  }

  // Whether the subquery names a column of the query around it: qualified
  // by one of its tables, or unqualified, of none of the subquery's own
  // tables but of one of the outer's.
  auto correlated(const ast::Select& q) const -> bool {
    if (q.set) return correlated(q.set->left) || correlated(q.set->right);
    std::vector<const Table*> own;
    for (const auto& name : {q.table, q.join}) {
      if (const auto* t = name.empty() ? nullptr : cat_.table(name)) own.push_back(t);
    }
    auto outer_names = [&](const ast::Expr& e) {
      if (!e.table.empty()) {
        if (e.table == q.table || e.table == q.join) return false;
        return std::ranges::any_of(scope_, [&](const Table* t) { return t->name == e.table; });
      }
      if (std::ranges::any_of(own, [&](const Table* t) { return t->column(e.column) != nullptr; })) return false;
      return std::ranges::any_of(scope_, [&](const Table* t) { return t->column(e.column) != nullptr; });
    };
    std::function<bool(const ast::Expr&)> in_expr = [&](const ast::Expr& e) {
      if ((e.kind == ast::Expr::Kind::Column || (e.kind == ast::Expr::Kind::Aggregate && !e.column.empty())) &&
          outer_names(e)) {
        return true;
      }
      return (e.lhs && in_expr(*e.lhs)) || (e.rhs && in_expr(*e.rhs));
    };
    std::function<bool(const ast::Compare&)> in_compare = [&](const ast::Compare& c) {
      return in_expr(c.lhs) || in_expr(c.rhs) || std::ranges::any_of(c.args, in_compare);
    };
    return std::ranges::any_of(q.targets, in_expr) || std::ranges::any_of(q.where, in_compare) ||
           std::ranges::any_of(q.having, in_compare) || std::ranges::any_of(q.group_by, in_expr) ||
           std::ranges::any_of(q.order_by, [&](const ast::OrderItem& o) { return in_expr(column_ref(o)); });
  }

  // Whether a column of type `col` is compared with a subquery's value of
  // type `sub` as it holds it: integers with integers, a key encoding each
  // as 64 bits whatever its declared width, so one past the column's type
  // matches nothing and bounds a range where it falls; a DECIMAL of its
  // scale; strings with strings; any other type its own.
  static void comparable(SqlType sub, const Column& c, std::int32_t pos) {
    auto ok = (is_integer(sub) && is_integer(c.sql)) ||
              (sub.kind == SqlKind::Decimal && c.sql.kind == SqlKind::Decimal && sub.scale == c.sql.scale) ||
              (is_string(sub) && is_string(c.sql)) || (!is_integer(sub) && sub.kind != SqlKind::Decimal && sub.kind == c.sql.kind);
    if (!ok) {
      not_yet("comparing " + c.name + " (" + sql_type_name(c.sql) + ") with a subquery of " + sql_type_name(sub), pos);
    }
  }

  // x op ANY (subquery), x a column, three-valued as PostgreSQL's: true
  // where a row's value makes x op it true; else NULL where a row is NULL,
  // or x is and a row exists; else false, over no rows too. = ANY tests the
  // subquery's set; every other operator its bounds.
  auto any_of(const logical::Expr& x, const Column& c, ast::ValueCompare op, const ast::Select& q, std::int32_t pos)
      -> logical::Expr {
    using K = logical::Expr::Kind;
    auto truth = [](std::uint32_t p) {
      auto e = logical::expr(K::Truth, SqlType{SqlKind::Boolean});
      e.args.push_back(logical::param(p, SqlType{SqlKind::Boolean}));
      return e;
    };
    logical::Expr core;
    std::uint32_t flags = 0;
    if (op == ast::ValueCompare::Eq) {
      auto s = subquery(q, logical::Subquery::Kind::Set, pos);
      comparable(s.type, c, pos);
      core = logical::expr(K::InSet, SqlType{SqlKind::Boolean});
      core.args.push_back(x);
      core.constant = s.index;
      flags = s.param;
    } else {
      auto s = subquery(q, logical::Subquery::Kind::Bounds, pos);
      comparable(s.type, c, pos);
      auto hi = logical::param(s.param, c.sql);
      auto lo = logical::param(s.param + 1, c.sql);
      flags = s.param + 2;
      switch (op) {
        case ast::ValueCompare::Lt: core = logical::compare(ast::Compare::Op::Lt, x, std::move(hi)); break;
        case ast::ValueCompare::Le: core = logical::compare(ast::Compare::Op::Le, x, std::move(hi)); break;
        case ast::ValueCompare::Gt: core = logical::compare(ast::Compare::Op::Gt, x, std::move(lo)); break;
        case ast::ValueCompare::Ge: core = logical::compare(ast::Compare::Op::Ge, x, std::move(lo)); break;
        case ast::ValueCompare::Ne: {  // a value other than x: one above it or one below
          std::vector<logical::Expr> either;
          either.push_back(logical::compare(ast::Compare::Op::Lt, x, std::move(hi)));
          either.push_back(logical::compare(ast::Compare::Op::Gt, x, std::move(lo)));
          core = logical::predicate_of(K::Or, std::move(either));
          break;
        }
        case ast::ValueCompare::Eq: break;
      }
    }
    std::vector<logical::Expr> or_null;
    or_null.push_back(std::move(core));
    or_null.push_back(truth(flags));
    std::vector<logical::Expr> both;
    both.push_back(logical::predicate_of(K::Or, std::move(or_null)));
    both.push_back(truth(flags + 1));
    return logical::predicate_of(K::And, std::move(both));
  }

  // x op ALL (subquery) is NOT (x op' ANY), op' its negation: true over no
  // rows, false where a row makes x op it false.
  static auto negation(ast::ValueCompare op) -> ast::ValueCompare {
    switch (op) {
      case ast::ValueCompare::Eq: return ast::ValueCompare::Ne;
      case ast::ValueCompare::Ne: return ast::ValueCompare::Eq;
      case ast::ValueCompare::Lt: return ast::ValueCompare::Ge;
      case ast::ValueCompare::Le: return ast::ValueCompare::Gt;
      case ast::ValueCompare::Gt: return ast::ValueCompare::Le;
      case ast::ValueCompare::Ge: return ast::ValueCompare::Lt;
    }
    return op;
  }

  // A constant in a select list, of its own type: an unknown one is TEXT,
  // as PostgreSQL resolves it there. Named as PostgreSQL names it: a cast
  // by its type's name (int4, date), anything else ?column?.
  auto constant_output(const ast::Expr& e) -> logical::Output {
    auto c = fold(e);
    auto name = [&]() -> std::string {
      switch (e.kind) {
        case ast::Expr::Kind::Cast: return e.cast_type;
        case ast::Expr::Kind::Case: return "case";
        case ast::Expr::Kind::Coalesce: return "coalesce";
        case ast::Expr::Kind::NullIf: return "nullif";
        case ast::Expr::Kind::Func: return e.func == "||" ? "?column?" : e.func;
        case ast::Expr::Kind::Column:
        case ast::Expr::Kind::Param:
        case ast::Expr::Kind::Const:
        case ast::Expr::Kind::Literal:
        case ast::Expr::Kind::Add:
        case ast::Expr::Kind::Sub:
        case ast::Expr::Kind::Mul:
        case ast::Expr::Kind::Div:
        case ast::Expr::Kind::Star:
        case ast::Expr::Kind::Plus:
        case ast::Expr::Kind::Neg:
        case ast::Expr::Kind::Compare:
        case ast::Expr::Kind::Aggregate:
        case ast::Expr::Kind::Subquery: break;
      }
      return "?column?";
    }();
    return {fixed(std::move(c.value), c.type), name};
  }

  // A scalar subquery in a select list: its value, named as PostgreSQL
  // names it, by its own column's name.
  auto subquery_output(const ast::Expr& e) -> logical::Output {
    auto s = subquery(*e.subquery, logical::Subquery::Kind::Value, e.pos);
    return {logical::param(s.param, s.type), s.name};
  }

  auto table(const std::string& name, std::int32_t pos) const -> const Table* {
    const auto* t = cat_.table(name);
    if (t == nullptr && cat_.view(name) != nullptr) {
      throw SqlError{"\"" + name + "\" is a view, not a table: views are not updatable", pos, kFeatureNotSupported};
    }
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
  // that has the column; one both have is ambiguous (42702), but for a
  // column of its USING, which is one column: the left side's, equal to the
  // right's in every pair. The USING's alias names those columns alone.
  auto column(const Table* (&sides)[2], const ast::Expr& e) const -> std::pair<std::uint32_t, const Column*> {
    auto in_using = std::ranges::contains(using_, e.column);
    if (!using_alias_.empty() && e.table == using_alias_) {
      if (!in_using) throw SqlError{"column " + e.table + "." + e.column + " does not exist", e.pos, kUndefinedColumn};
      return {0, sides[0]->column(e.column)};
    }
    if (e.table.empty() && in_using) return {0, sides[0]->column(e.column)};
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
      if (!p.first || p.first->use != Use::Compared || use != Use::Compared) return kSyntaxError;
      return type_refusal(p.first->col->sql.kind, col.sql.kind, false).value_or(kSyntaxError);
    };
    if (p.first && p.first->phys != t) {
      throw SqlError{"parameter $" + std::to_string(n + 1) + " used as both " + type_name(p.first->phys) + " and " +
                         type_name(t),
                     pos, clash()};
    }
    if (p.first && compared_type(p.first->col->sql).identity != compared_type(col.sql).identity) {
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
      auto second = use == p.first->use ? named(col) : verb(use) + named(col);
      throw SqlError{"parameter $" + std::to_string(n + 1) + " is " + verb(p.first->use) + named(*p.first->col) + " and " +
                         second,
                     pos, clash()};
    }
    if (!p.first) p.first = ParamUse::FirstUse{t, &col, use};
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

  // Whether the layer takes `fn` of `c`, and if not the SQLSTATE of the
  // refusal: 42883 where PostgreSQL has no such aggregate either, 42601 where
  // it has and the layer does not yet (SUM or AVG of a REAL, DOUBLE PRECISION
  // or TIME column, which PostgreSQL takes as an interval; MIN or MAX of a
  // REAL, DOUBLE PRECISION or string column).
  struct AggregateSupport {
    bool supported;
    SqlState sqlstate;
  };
  static auto aggregate_support(ast::Expr::Fn fn, const Column& c) -> AggregateSupport {
    using Fn = ast::Expr::Fn;
    if (fn == Fn::Count) return {true, kSyntaxError};
    auto sum = fn == Fn::Sum || fn == Fn::Avg;
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
      case SqlKind::Numeric: return {false, kSyntaxError};  // no column has it
    }
    std::unreachable();
  }

  static constexpr auto aggregate_name(ast::Expr::Fn fn) -> std::string_view {
    switch (fn) {
      case ast::Expr::Fn::Count: return "COUNT";
      case ast::Expr::Fn::Sum: return "SUM";
      case ast::Expr::Fn::Min: return "MIN";
      case ast::Expr::Fn::Max: return "MAX";
      case ast::Expr::Fn::Avg: return "AVG";
    }
    std::unreachable();
  }

  // An aggregate call, of column c of `table`, or COUNT(*) with none. SUM,
  // MIN, MAX and AVG take a column whose values the plan holds as int64_t:
  // SUM and AVG an integer or DECIMAL one, as PostgreSQL sums them exactly;
  // MIN and MAX those and the date and time types. Of a REAL or DOUBLE
  // PRECISION column they are later, and MIN and MAX of a string or bytes
  // column outside the MVP (docs/frontend_design.md, *Aggregates*).
  //
  // Its result as PostgreSQL names and types it: COUNT a bigint; SUM of a
  // SMALLINT or INTEGER a bigint, of a BIGINT or DECIMAL a numeric of no
  // declared precision (the total may take 19 digits), held at the column's
  // scale; MIN and MAX of the column's type; AVG a numeric of no declared
  // precision, its scale PostgreSQL's division's (the API computes it from
  // the plan's sum and count, docs/planner_design.md, *Aggregates*).
  static auto aggregate(const Column* c, std::uint32_t table, const ast::Expr& e) -> logical::Output {
    using Fn = ast::Expr::Fn;
    auto agg = logical::expr(logical::Expr::Kind::Aggregate, SqlType{SqlKind::BigInt});
    agg.distinct = e.distinct;
    switch (e.fn) {
      case Fn::Count: agg.fn = logical::Expr::Fn::Count; break;
      case Fn::Sum: agg.fn = logical::Expr::Fn::Sum; break;
      case Fn::Min: agg.fn = logical::Expr::Fn::Min; break;
      case Fn::Max: agg.fn = logical::Expr::Fn::Max; break;
      case Fn::Avg: agg.fn = logical::Expr::Fn::Avg; break;
    }
    if (c == nullptr) return {std::move(agg), "count"};
    if (auto s = aggregate_support(e.fn, *c); !s.supported) {
      throw SqlError{std::string{aggregate_name(e.fn)} + " of a " + sql_type_name(c->sql) +
                         " column is not supported",
                     e.pos, s.sqlstate};
    }
    agg.args.push_back(logical::column_of(table, *c));
    switch (e.fn) {
      case Fn::Count: return {std::move(agg), "count"};
      case Fn::Sum:
        if (c->sql.kind != SqlKind::SmallInt && c->sql.kind != SqlKind::Integer) {
          agg.type = SqlType{SqlKind::Decimal, 0, 0, c->sql.scale};
        }
        return {std::move(agg), "sum"};
      case Fn::Min: agg.type = c->sql; return {std::move(agg), "min"};
      case Fn::Max: agg.type = c->sql; return {std::move(agg), "max"};
      case Fn::Avg: agg.type = SqlType{SqlKind::Numeric, 0, 0, c->sql.scale}; return {std::move(agg), "avg"};
    }
    return {std::move(agg), ""};
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
  auto value(const Table& t, const ast::Expr& e, const Column& target) -> logical::Expr {
    auto want = target.type;
    if (is_constant(e)) {
      // Out of the column's type, it is refused here, before anything runs,
      // as PostgreSQL's planner folds its assignment cast: so of several the
      // first in the statement's order, as there.
      auto k = fold(e);
      auto v = coerce(k, target, true, "=");
      if (auto why = out_of_range(target.sql, v)) out_of_type(*why, k.pos);
      return fixed(std::move(v), target.sql);
    }
    switch (e.kind) {
      case ast::Expr::Kind::Param:
        use_param(e.param, want, e.pos, target, t.id, Use::Assigned);
        return logical::param(e.param, target.sql);
      case ast::Expr::Kind::Column: {
        const auto* c = column(t, e);
        if (c->type != want) {
          throw SqlError{"column \"" + c->name + "\" is " + type_name(c->type), e.pos,
                         type_refusal(c->sql.kind, target.sql.kind, true).value_or(kSyntaxError)};
        }
        return logical::column_of(t.id, *c);
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
        auto kind = e.kind == ast::Expr::Kind::Add ? logical::Expr::Kind::Add : logical::Expr::Kind::Sub;
        if (e.rhs->kind == ast::Expr::Kind::Const) {
          return logical::binary(kind, logical::column_of(t.id, *c), logical::constant(literal(*c, *e.rhs), c->sql),
                                 c->sql);
        }
        use_param(e.rhs->param, PhysType::I64, e.rhs->pos, *c, t.id,
                  e.kind == ast::Expr::Kind::Add ? Use::Added : Use::Subtracted);
        return logical::binary(kind, logical::column_of(t.id, *c), logical::param(e.rhs->param, c->sql), c->sql);
      }
      case ast::Expr::Kind::Mul:
      case ast::Expr::Kind::Div: not_yet("* and / of other than constants", e.pos);
      case ast::Expr::Kind::Star: throw SqlError{"syntax error at or near \"*\"", e.pos};
      case ast::Expr::Kind::Aggregate: throw SqlError{"aggregate functions are not allowed here", e.pos, kGroupingError};
      case ast::Expr::Kind::Plus:
      case ast::Expr::Kind::Neg: not_yet("unary + and - of other than a constant", e.pos);
      case ast::Expr::Kind::Compare: not_yet("a comparison of other than constants as a value", e.pos);
      case ast::Expr::Kind::Cast: not_yet("a cast of other than a constant", e.pos);
      case ast::Expr::Kind::Subquery: not_yet("a subquery assigned to a column", e.pos);
      case ast::Expr::Kind::Case:
      case ast::Expr::Kind::Coalesce:
      case ast::Expr::Kind::NullIf: not_yet("CASE, COALESCE and NULLIF of other than constants", e.pos);
      case ast::Expr::Kind::Func: not_yet("|| and the string functions of other than constants", e.pos);
      case ast::Expr::Kind::Const:
      case ast::Expr::Kind::Literal: break;  // constants, above
    }
    throw SqlError{"expression not supported", e.pos};
  }

  auto where(const Table& t, const std::vector<ast::Compare>& in) -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
    for (const auto* w : top_level(in)) {
      out.push_back(predicate(*w, [&](const ast::Expr& e) { return std::pair{t.id, column(t, e)}; }));
    }
    return out;
  }

  // The conjuncts, each AND among them (a BETWEEN's) taken apart, so a key
  // may take each comparison.
  static auto top_level(const std::vector<ast::Compare>& in) -> std::vector<const ast::Compare*> {
    std::vector<const ast::Compare*> out;
    std::function<void(const ast::Compare&)> add = [&](const ast::Compare& c) {
      if (c.kind != ast::Compare::Kind::And) {
        out.push_back(&c);
        return;
      }
      for (const auto& a : c.args) add(a);
    };
    for (const auto& c : in) add(c);
    return out;
  }

  // col LIKE pattern: a TEXT or VARCHAR column, as PostgreSQL's ~~ takes
  // them, and a pattern a parameter or a constant, of TEXT, as PostgreSQL
  // types it; the escape a backslash, ESCAPE's one character, or none for
  // ESCAPE ''.
  auto like(const ast::Compare& w, const auto& resolve) -> logical::Expr {
    if (w.lhs.kind != ast::Expr::Kind::Column) not_yet("LIKE of other than a column", w.pos);
    auto [table, c] = resolve(w.lhs);
    if (!is_string(c->sql)) {
      if (c->sql.kind == SqlKind::Bytea) not_yet("LIKE of a BYTEA column", w.pos);
      throw SqlError{"operator does not exist: " + pg_type_name(c->sql) + " ~~ unknown", w.pos, kUndefinedFunction};
    }
    synthetic_.push_back(Column{0, c->name, SqlType{SqlKind::Text}, PhysType::Bytes, true, {}});
    const auto& text = synthetic_.back();
    logical::Expr pattern;
    if (w.rhs.kind == ast::Expr::Kind::Param) {
      use_param(w.rhs.param, PhysType::Bytes, w.rhs.pos, text, table, Use::Compared);
      pattern = logical::param(w.rhs.param, text.sql);
    } else if (is_constant(w.rhs)) {
      pattern = fixed(coerce(fold(w.rhs), text, false, "~~"), text.sql);
    } else {
      not_yet("LIKE of other than a parameter or a constant pattern", w.pos);
    }
    std::int64_t escape = '\\';
    if (w.escape) {
      auto b = std::as_bytes(std::span{w.escape->data(), w.escape->size()});
      auto n = utf8_code_points(b);
      if (!n || *n > 1) throw SqlError{"invalid escape string", w.pos, kInvalidEscapeSequence};
      escape = -1;
      if (*n == 1) {
        auto lead = static_cast<unsigned char>((*w.escape)[0]);
        std::int64_t cp = w.escape->size() == 1 ? lead : lead & (0x7F >> w.escape->size());
        for (std::size_t k = 1; k < w.escape->size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>((*w.escape)[k]) & 0x3F);
        escape = cp;
      }
    }
    auto e = logical::expr(logical::Expr::Kind::Like, SqlType{SqlKind::Boolean});
    e.args.push_back(logical::column_of(table, *c));
    e.args.push_back(std::move(pattern));
    e.constant = escape;
    return e;
  }

  // A conjunct: a comparison, or a predicate's tree, each comparison in it
  // bound as a WHERE's is and IS NULL of a column (docs/frontend_design.md,
  // *WHERE*). In a tree, $n ± k is not taken yet.
  auto predicate(const ast::Compare& w, const auto& resolve) -> logical::Expr {
    using K = ast::Compare::Kind;
    auto tree = [&](logical::Expr::Kind kind) {
      auto e = logical::expr(kind, SqlType{SqlKind::Boolean});
      for (const auto& a : w.args) {
        auto computed = [](const ast::Expr& x) { return x.kind == ast::Expr::Kind::Add || x.kind == ast::Expr::Kind::Sub; };
        if (a.kind == K::Compare && ((computed(a.lhs) && !is_constant(a.lhs)) || (computed(a.rhs) && !is_constant(a.rhs)))) {
          not_yet("$n + k and $n - k inside AND, OR or NOT", a.pos);
        }
        e.args.push_back(predicate(a, resolve));
      }
      return e;
    };
    switch (w.kind) {
      case K::Compare: return compare(w, resolve);
      case K::IsNull: {
        if (w.lhs.kind != ast::Expr::Kind::Column) not_yet("IS NULL of other than a column", w.pos);
        auto [table, c] = resolve(w.lhs);
        auto e = logical::expr(logical::Expr::Kind::IsNull, SqlType{SqlKind::Boolean});
        e.args.push_back(logical::column_of(table, *c));
        return e;
      }
      case K::Like: return like(w, resolve);
      case K::Exists: {
        auto s = subquery(*w.subquery, logical::Subquery::Kind::Exists, w.pos);
        auto e = logical::expr(logical::Expr::Kind::Truth, SqlType{SqlKind::Boolean});
        e.args.push_back(logical::param(s.param, SqlType{SqlKind::Boolean}));
        return e;
      }
      case K::Quantified: {
        if (w.lhs.kind != ast::Expr::Kind::Column) not_yet("IN, ANY or ALL of a subquery of other than a column", w.pos);
        auto [table, c] = resolve(w.lhs);
        auto x = logical::column_of(table, *c);
        if (!w.all) return any_of(x, *c, w.quantified, *w.subquery, w.pos);
        std::vector<logical::Expr> one;
        one.push_back(any_of(x, *c, negation(w.quantified), *w.subquery, w.pos));
        return logical::predicate_of(logical::Expr::Kind::Not, std::move(one));
      }
      case K::And: return tree(logical::Expr::Kind::And);
      case K::Or: return tree(logical::Expr::Kind::Or);
      case K::Not: return tree(logical::Expr::Kind::Not);
    }
    std::unreachable();
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
  auto compare(const ast::Compare& w, const auto& resolve) -> logical::Expr {
    const auto* col = &w.lhs;
    const auto* rhs = &w.rhs;
    auto op = w.op;
    if ((operand(*col) || is_constant(*col) || col->kind == ast::Expr::Kind::Subquery) &&
        rhs->kind == ast::Expr::Kind::Column) {
      std::swap(col, rhs);
      op = mirrored(op);
    }
    // A scalar subquery: its value, a parameter it sets before the plan runs.
    if (col->kind == ast::Expr::Kind::Column && rhs->kind == ast::Expr::Kind::Subquery) {
      auto [table, c] = resolve(*col);
      auto s = subquery(*rhs->subquery, logical::Subquery::Kind::Value, rhs->pos);
      comparable(s.type, *c, rhs->pos);
      return logical::compare(op, logical::column_of(table, *c), logical::param(s.param, c->sql));
    }
    // A constant: folded to the column's type, where it holds it exactly.
    if (col->kind == ast::Expr::Kind::Column && is_constant(*rhs)) {
      auto [table, c] = resolve(*col);
      auto v = coerce(fold(*rhs), *c, false, op_text(op));
      return logical::compare(op, logical::column_of(table, *c), fixed(std::move(v), c->sql));
    }
    if (col->kind != ast::Expr::Kind::Column || !operand(*rhs)) {
      throw SqlError{std::string{w.on ? "ON" : "WHERE"} + " must compare a column with a parameter or a constant", w.pos};
    }
    auto [table, c] = resolve(*col);
    if (rhs->kind == ast::Expr::Kind::Param) {
      use_param(rhs->param, c->type, rhs->pos, *c, table, Use::Compared);
      return logical::compare(op, logical::column_of(table, *c), logical::param(rhs->param, c->sql));
    }
    if (c->sql.kind != SqlKind::Integer) {
      throw SqlError{"$n + n and $n - n are supported only compared with an INTEGER column yet", rhs->pos};
    }
    auto k = rhs->rhs->constant;
    if (!std::in_range<std::int32_t>(k)) {
      throw SqlError{"an integer literal past INTEGER is not supported here yet", rhs->rhs->pos};
    }
    use_param(rhs->lhs->param, c->type, rhs->lhs->pos, *c, table, Use::Compared);
    auto kind = rhs->kind == ast::Expr::Kind::Add ? logical::Expr::Kind::Add : logical::Expr::Kind::Sub;
    auto operand = logical::binary(kind, logical::param(rhs->lhs->param, c->sql), logical::constant(k, c->sql), c->sql);
    return logical::compare(op, logical::column_of(table, *c), std::move(operand));
  }

  static constexpr auto op_text(ast::Compare::Op op) -> std::string_view {
    switch (op) {
      case ast::Compare::Op::Eq: return "=";
      case ast::Compare::Op::Lt: return "<";
      case ast::Compare::Op::Le: return "<=";
      case ast::Compare::Op::Gt: return ">";
      case ast::Compare::Op::Ge: return ">=";
    }
    return "=";
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

  // A join's WHERE: each conjunct a comparison of a column of one side with
  // a parameter, or a column of each, in WHERE order.
  // A join's WHERE; with `on`, of its ON's conjuncts alone, or of the
  // others alone.
  auto where(const Table* (&sides)[2], const std::vector<ast::Compare>& in, std::optional<bool> on = std::nullopt)
      -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
    for (const auto* c : top_level(in)) {
      const auto& w = *c;
      if (on && w.on != *on) continue;
      if (w.kind == ast::Compare::Kind::Compare && w.lhs.kind == ast::Expr::Kind::Column &&
          w.rhs.kind == ast::Expr::Kind::Column) {
        out.push_back(join_on(sides, w));
        continue;
      }
      out.push_back(predicate(w, [&](const ast::Expr& e) {
        auto [side, col] = column(sides, e);
        return std::pair{sides[side]->id, col};
      }));
    }
    return out;
  }

  // A column of one table equal to a column of the other, of one SQL type.
  auto join_on(const Table* (&sides)[2], const ast::Compare& w) const -> logical::Expr {
    auto [ls, lc] = column(sides, w.lhs);
    auto [rs, rc] = column(sides, w.rhs);
    if (ls == rs) throw SqlError{"comparing two columns of one table is not supported yet", w.pos};
    if (w.op != ast::Compare::Op::Eq) throw SqlError{"a join compares two columns with = only yet", w.pos};
    return joined_equal(*sides[ls], *lc, *sides[rs], *rc, w.pos);
  }

  // l.lc = r.rc, columns of one SQL type: two of different ones are
  // refused (docs/frontend_design.md, *Known differences*).
  static auto joined_equal(const Table& l, const Column& lc, const Table& r, const Column& rc, std::int32_t pos)
      -> logical::Expr {
    if (compared_type(lc.sql).identity != compared_type(rc.sql).identity) {
      throw SqlError{"comparing " + lc.name + " (" + sql_type_name(lc.sql) + ") with " + rc.name + " (" +
                         sql_type_name(rc.sql) + ") is not supported: a join compares columns of one type",
                     pos};
    }
    return logical::compare(ast::Compare::Op::Eq, logical::column_of(l.id, lc), logical::column_of(r.id, rc));
  }

  // USING (…) or NATURAL: its columns, each of both tables and named once
  // (NATURAL: every column name they share, in the left table's order), and
  // the equality of each, in that order.
  // The columns a USING clause or NATURAL joins on, in order (NATURAL:
  // every column name both tables share, in the left table's order).
  static auto shared_columns(const Table* const (&sides)[2], const ast::Select& s) -> std::vector<std::string> {
    auto names = s.using_columns;
    if (s.natural) {
      for (const auto& c : sides[0]->columns) {
        if (!c.hidden && sides[1]->column(c.name) != nullptr) names.push_back(c.name);
      }
    }
    return names;
  }
  auto using_conjuncts(const Table* (&sides)[2], const ast::Select& s) -> std::vector<logical::Expr> {
    using_ = shared_columns(sides, s);
    using_alias_ = s.using_alias;
    std::vector<logical::Expr> out;
    for (const auto& name : using_) {
      if (std::ranges::count(using_, name) > 1) {
        throw SqlError{"column name \"" + name + "\" appears more than once in USING clause", s.join_pos, kDuplicateColumn};
      }
      for (std::uint32_t side = 0; side < 2; ++side) {
        if (sides[side]->column(name) == nullptr) {
          throw SqlError{"column \"" + name + "\" specified in USING clause does not exist in " +
                             (side == 0 ? "left" : "right") + " table",
                         s.join_pos, kUndefinedColumn};
        }
      }
      out.push_back(joined_equal(*sides[0], *sides[0]->column(name), *sides[1], *sides[1]->column(name), s.join_pos));
    }
    return out;
  }

  // A SELECT of one table, or of two joined: Limit? -> Project -> Sort? ->
  // Filter? -> Get, or a Join of two Gets; aggregated, Limit? -> Sort? ->
  // Filter? -> Aggregate -> Filter? -> the tables, the Filter above the
  // Aggregate its HAVING. The WHERE's conjuncts, a join's ON among them, sit
  // in one Filter above the tables; predicate pushdown moves each down. No
  // FOR UPDATE on a join yet.
  //
  // A query is aggregated by a GROUP BY, a HAVING or an aggregate call. Then
  // a column it selects, orders by or compares in HAVING must be grouped,
  // or of a table whose primary key is (42803): PostgreSQL's functional
  // dependency, so it is the same in every row of a group, read from any
  // one (Aggregate::dependent), and the GROUP BY stays as written.
  auto select(const ast::Select& s) -> logical::NodePtr {
    if (s.set) return set_operation(s);
    if (s.table.empty()) return select_without_from(s);
    auto starred = std::ranges::any_of(s.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Star; });
    std::vector<ast::Expr> expanded;
    if (starred) expanded = expand_stars(s);
    const auto& list = starred ? expanded : s.targets;
    auto joined = !s.join.empty();
    const Table* sides[2]{table(s.table, joined ? s.table_pos : s.pos), nullptr};
    std::vector<logical::Expr> conjuncts;
    auto outer = joined && s.join_kind != ast::Select::JoinKind::Inner;
    std::vector<logical::Expr> on;  // an outer join's ON: its Join's predicate
    if (joined) {
      sides[1] = table(s.join, s.join_pos);
      set_scope({sides[0], sides[1]});
      if (sides[0] == sides[1]) throw SqlError{"a table joined with itself is not supported", s.pos};
      if (s.for_update) throw SqlError{"FOR UPDATE on a join is not supported yet", s.pos};
      conjuncts = using_conjuncts(sides, s);
      // An outer join's ON decides its pairs and its WHERE filters them, so
      // the two stay apart; the table every row of which it keeps comes
      // first: RIGHT is LEFT of the tables swapped, and a USING column named
      // alone is that table's, as in PostgreSQL.
      if (s.join_kind == ast::Select::JoinKind::Right) std::swap(sides[0], sides[1]);
      if (outer) {
        on = std::move(conjuncts);
        auto rest = where(sides, s.where, true);
        on.insert(on.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
        conjuncts = where(sides, s.where, false);
      } else {
        auto rest = where(sides, s.where);
        conjuncts.insert(conjuncts.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
      }
    } else {
      set_scope({sides[0]});
      conjuncts = where(*sides[0], s.where);
    }
    // A column reference: of the one table, or of the side that has it.
    auto resolve = [&](const ast::Expr& e) -> std::pair<const Table*, const Column*> {
      if (!joined) return {sides[0], column(*sides[0], e)};
      auto [side, c] = column(sides, e);
      return {sides[side], c};
    };
    auto aggregated = !s.group_by.empty() || !s.having.empty() ||
                      std::ranges::any_of(list, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Aggregate; });
    // DISTINCT is a GROUP BY of the selected columns. Over a query already
    // aggregated it is nothing more where every grouped column is
    // selected, each group then one row; else not yet. Checked in
    // PostgreSQL's order.
    auto distinct_groups = s.distinct && !aggregated;
    if (s.distinct) {
      if (s.for_update) throw SqlError{"FOR UPDATE is not allowed with DISTINCT clause", s.pos, kFeatureNotSupported};
      auto selected = [&](const ast::Expr& e) {
        auto ref = resolve(e);
        return std::ranges::any_of(list, [&](const ast::Expr& t) { return t.kind == ast::Expr::Kind::Column && resolve(t) == ref; });
      };
      for (const auto& o : s.order_by) {
        if (!selected(column_ref(o))) {
          throw SqlError{"for SELECT DISTINCT, ORDER BY expressions must appear in select list", o.pos,
                         kInvalidColumnReference};
        }
      }
      if (aggregated && !std::ranges::all_of(s.group_by, selected)) {
        not_yet("SELECT DISTINCT of groups it does not select every GROUP BY column of", s.pos);
      }
      for (const auto& e : list) {
        if (distinct_groups && e.kind != ast::Expr::Kind::Column) not_yet("SELECT DISTINCT of other than columns", e.pos);
      }
    }
    aggregated = aggregated || distinct_groups;
    auto grouping = !s.group_by.empty() || distinct_groups;
    std::vector<logical::Expr> group_by;
    for (const auto& e : distinct_groups ? list : s.group_by) {
      auto [t, c] = resolve(e);
      auto g = logical::column_of(t->id, *c);
      if (std::ranges::find(group_by, g) == group_by.end()) group_by.push_back(std::move(g));
    }
    // A column of an aggregated query: grouped, or dependent, as its
    // table's grouped primary key determines it; else 42803.
    std::vector<logical::Expr> dependent;
    auto grouped = [&](const Table& t, const Column& c, std::int32_t pos) -> logical::Expr {
      auto g = logical::column_of(t.id, c);
      if (std::ranges::find(group_by, g) != group_by.end()) return g;
      auto by_key = grouping && std::ranges::all_of(t.pk_col_ids, [&](std::uint32_t id) {
        return std::ranges::find(group_by, logical::column_of(t.id, *t.column(id))) != group_by.end();
      });
      if (!by_key) ungrouped(t, c.name, pos);
      if (std::ranges::find(dependent, g) == dependent.end()) dependent.push_back(g);
      return g;
    };
    std::vector<logical::Output> targets;
    for (const auto& e : list) {
      if (is_constant(e) || e.kind == ast::Expr::Kind::Subquery) {
        if (aggregated) {
          not_yet(std::string{e.kind == ast::Expr::Kind::Subquery ? "a subquery" : "a constant"} +
                      " in the select list of an aggregated query",
                  e.pos);
        }
        targets.push_back(e.kind == ast::Expr::Kind::Subquery ? subquery_output(e) : constant_output(e));
        continue;
      }
      if (e.kind == ast::Expr::Kind::Aggregate) {
        if (e.column.empty()) {
          targets.push_back(aggregate(nullptr, 0, e));
        } else {
          auto [t, c] = resolve(e);
          targets.push_back(aggregate(c, t->id, e));
        }
        continue;
      }
      if (e.kind != ast::Expr::Kind::Column) throw SqlError{"only columns, constants and aggregates can be selected", e.pos};
      auto [t, c] = resolve(e);
      targets.push_back({aggregated ? grouped(*t, *c, e.pos) : logical::column_of(t->id, *c), c->name});
    }
    named(list, targets);
    std::vector<logical::Expr> having;
    for (const auto& h : s.having) having.push_back(having_conjunct(h, resolve, grouped));
    // 0A000, as PostgreSQL refuses it, and in its order of checks.
    if (aggregated && s.for_update) {
      auto what = !s.group_by.empty() ? "GROUP BY clause" : !s.having.empty() ? "HAVING clause" : "aggregate functions";
      throw SqlError{std::string{"FOR UPDATE is not allowed with "} + what, s.pos, kFeatureNotSupported};
    }
    std::vector<logical::SortKey> keys;
    for (const auto& o : s.order_by) {
      auto [t, c] = resolve(column_ref(o));
      auto key = aggregated ? grouped(*t, *c, o.pos) : logical::column_of(t->id, *c);
      keys.push_back({std::move(key), o.desc, o.nulls_first.value_or(o.desc)});
    }

    auto rows = joined ? logical::node(logical::Join{logical::node(logical::Get{sides[0], false}),
                                                     logical::node(logical::Get{sides[1], false}), std::move(on), outer})
                       : logical::node(logical::Get{sides[0], s.for_update});
    if (!conjuncts.empty()) rows = logical::node(logical::Filter{std::move(rows), std::move(conjuncts)});
    if (aggregated) {
      rows = logical::node(logical::Aggregate{std::move(rows), std::move(targets), std::move(group_by), grouping,
                                              std::move(dependent)});
      if (!having.empty()) rows = logical::node(logical::Filter{std::move(rows), std::move(having)});
      if (!keys.empty()) rows = logical::node(logical::Sort{std::move(rows), std::move(keys)});
    } else {
      if (!keys.empty()) rows = logical::node(logical::Sort{std::move(rows), std::move(keys)});
      rows = logical::node(logical::Project{std::move(rows), std::move(targets)});
    }
    if (s.limit) rows = logical::node(logical::Limit{std::move(rows), *s.limit});
    if (outer) nullable_above_join(*rows, sides[1]->id);
    return rows;
  }

  // An arm of a set operation: its rows, and for each column whether it is
  // an untyped literal, which PostgreSQL types by the other arm.
  struct Arm {
    logical::NodePtr node;
    std::vector<bool> untyped;
  };

  // UNION, INTERSECT or EXCEPT. Each arm is bound as a SELECT of its own
  // and must be a plain one: a Project, not an Aggregate (GROUP BY, an
  // aggregate call or DISTINCT), because the plan streams an arm's rows into
  // the operation and has no group to stream. The columns' types resolve
  // pair by pair, as PostgreSQL resolves UNION's (set_type); the names are
  // the left arm's. ORDER BY of the combined rows would need the plan to
  // hold typed rows, which the plan API has no object for.
  auto set_operation(const ast::Select& s) -> logical::NodePtr {
    auto arm = set_arm(s, true);
    return std::move(arm.node);
  }

  static auto set_name(ast::SetOperation::Kind k) -> std::string {
    switch (k) {
      case ast::SetOperation::Kind::Union: return "UNION";
      case ast::SetOperation::Kind::Intersect: return "INTERSECT";
      case ast::SetOperation::Kind::Except: return "EXCEPT";
    }
    return "UNION";
  }

  auto set_arm(const ast::Select& q, bool root) -> Arm {
    if (q.set) {
      if (!q.order_by.empty()) not_yet("ORDER BY on a set operation", q.pos);
      if (!root && q.limit) not_yet("LIMIT on a parenthesised set operation inside another", q.pos);
      const auto& op = *q.set;
      auto l = set_arm(op.left, false);
      auto r = set_arm(op.right, false);
      const auto& lo = outputs_of(*l.node);
      const auto& ro = outputs_of(*r.node);
      auto what = set_name(op.kind);
      if (lo.size() != ro.size()) throw SqlError{"each " + what + " query must have the same number of columns", q.pos};
      logical::SetOp out;
      out.kind = op.kind;
      out.all = op.all;
      Arm arm;
      for (std::size_t i = 0; i < lo.size(); ++i) {
        auto type = set_type(lo[i].expr.type, ro[i].expr.type, l.untyped[i], r.untyped[i], what, q.pos);
        out.outputs.push_back({logical::param(0, type), lo[i].name});
        arm.untyped.push_back(l.untyped[i] && r.untyped[i]);
      }
      out.left = std::move(l.node);
      out.right = std::move(r.node);
      arm.node = logical::node(std::move(out));
      if (q.limit) arm.node = logical::node(logical::Limit{std::move(arm.node), *q.limit});
      return arm;
    }
    if (q.for_update) throw SqlError{"FOR UPDATE is not allowed with UNION/INTERSECT/EXCEPT", q.pos, kFeatureNotSupported};
    Arm arm;
    arm.node = select(q);
    const logical::Node* n = arm.node.get();
    while (!std::holds_alternative<logical::Project>(n->op)) {
      const auto* in = logical::input_of(*n);
      if (in == nullptr || !std::holds_alternative<logical::Limit>(n->op)) {
        not_yet("a set operation over an aggregated, grouped or DISTINCT query", q.pos);
      }
      n = in->get();
    }
    const auto& outs = outputs_of(*arm.node);
    auto stars = std::ranges::any_of(q.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Star; });
    for (std::size_t i = 0; i < outs.size(); ++i) {
      const auto* e = stars || i >= q.targets.size() ? nullptr : &q.targets[i];
      arm.untyped.push_back(e != nullptr && e->kind == ast::Expr::Kind::Literal &&
                            (e->literal.kind == ast::Literal::Kind::Null || e->literal.kind == ast::Literal::Kind::String));
    }
    return arm;
  }

  // The type of a column both arms give, as PostgreSQL's UNION resolves it:
  // the same type; integers widen to the wider; an integer or REAL meeting
  // DOUBLE PRECISION is that; DECIMALs of one scale a DECIMAL; TEXT meeting
  // VARCHAR is TEXT, and VARCHARs of two lengths VARCHAR; types of one class
  // that are not these, or a literal of no type of its own, are refused; else
  // 42804.
  static auto set_type(SqlType a, SqlType b, bool a_untyped, bool b_untyped, const std::string& what, std::int32_t pos)
      -> SqlType {
    if (a == b) return a;
    auto refuse = [&] [[noreturn]] (const std::string& why) { not_yet(what + " of " + why, pos); };
    if (a_untyped || b_untyped) refuse("a literal of no type and a column of another type");
    if (type_class(a.kind) != type_class(b.kind)) {
      throw SqlError{what + " types " + pg_type_name(a) + " and " + pg_type_name(b) + " cannot be matched", pos,
                     kDatatypeMismatch};
    }
    auto approximate = [](SqlType t) { return t.kind == SqlKind::Real || t.kind == SqlKind::Double; };
    if (is_integer(a) && is_integer(b)) {
      auto rank = [](SqlType t) { return t.kind == SqlKind::SmallInt ? 0 : t.kind == SqlKind::Integer ? 1 : 2; };
      return rank(a) >= rank(b) ? a : b;
    }
    if ((is_integer(a) || approximate(a)) && (is_integer(b) || approximate(b))) {
      // An integer is read as a double exactly as far as a double holds it, as PostgreSQL's cast
      // does; a REAL would round it to a float, which the layer does not.
      if ((is_integer(a) && b.kind == SqlKind::Real) || (a.kind == SqlKind::Real && is_integer(b))) {
        refuse("an integer and REAL");
      }
      return SqlType{SqlKind::Double};
    }
    if (a.kind == SqlKind::Decimal && b.kind == SqlKind::Decimal && a.scale == b.scale) return numeric_type(a.scale);
    if (is_string(a) && is_string(b)) {
      if (a.kind == SqlKind::Varchar && b.kind == SqlKind::Varchar) return SqlType{SqlKind::Varchar};
      return SqlType{SqlKind::Text};
    }
    refuse(pg_type_name(a) + " and " + pg_type_name(b));
  }

  // Above a left outer join every column of its right table may be NULL, in
  // the rows it NULL-extends, whatever the column's declaration: so marked,
  // the plan tests it. The join's own predicate and inputs read real rows.
  static void nullable_above_join(logical::Node& n, std::uint32_t table) {
    std::function<void(logical::Expr&)> mark = [&](logical::Expr& e) {
      if (e.kind == logical::Expr::Kind::Column && e.col.table == table) e.col.nullable = true;
      for (auto& a : e.args) mark(a);
    };
    std::visit(
        [&](auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, logical::Join>) {
            return;
          } else {
            if constexpr (std::is_same_v<T, logical::Filter>) {
              for (auto& c : op.conjuncts) mark(c);
            } else if constexpr (std::is_same_v<T, logical::Project>) {
              for (auto& o : op.exprs) mark(o.expr);
            } else if constexpr (std::is_same_v<T, logical::Aggregate>) {
              for (auto& o : op.aggs) mark(o.expr);
              for (auto& g : op.group_by) mark(g);
              for (auto& g : op.dependent) mark(g);
            } else if constexpr (std::is_same_v<T, logical::Sort>) {
              for (auto& k : op.keys) mark(k.expr);
            }
            if constexpr (requires { op.input; }) nullable_above_join(*op.input, table);
          }
        },
        n.op);
  }

  // A select-list item's AS name is its output column's.
  static void named(const std::vector<ast::Expr>& list, std::vector<logical::Output>& targets) {
    for (std::size_t i = 0; i < targets.size(); ++i) {
      if (!list[i].alias.empty()) targets[i].name = list[i].alias;
    }
  }

  // The select list with each * and `table`.* replaced by the columns it
  // stands for, in table order, hidden ones left out. A * over a USING or
  // NATURAL join gives the USING columns first, each once, named alone.
  auto expand_stars(const ast::Select& s) -> std::vector<ast::Expr> {
    std::vector<ast::Expr> out;
    const Table* from[2]{table(s.table, s.join.empty() ? s.pos : s.table_pos), nullptr};
    if (!s.join.empty()) from[1] = table(s.join, s.join_pos);
    auto column_expr = [](const Table& t, const Column& c, std::int32_t pos) {
      ast::Expr e{ast::Expr::Kind::Column, c.name, 0, nullptr, nullptr, pos};
      e.table = t.name;
      return e;
    };
    for (const auto& e : s.targets) {
      if (e.kind != ast::Expr::Kind::Star) {
        out.push_back(ast::clone(e));
        out.back().alias = e.alias;
        continue;
      }
      if (!e.alias.empty()) throw SqlError{"syntax error at or near \"AS\"", e.pos};
      std::vector<std::string> merged;
      if (e.table.empty() && from[1] != nullptr) {
        merged = shared_columns(from, s);
        for (const auto& name : merged) {
          ast::Expr c{ast::Expr::Kind::Column, name, 0, nullptr, nullptr, e.pos};
          out.push_back(std::move(c));
        }
      }
      auto any = false;
      for (const auto* t : from) {
        if (t == nullptr || (!e.table.empty() && e.table != t->name)) continue;
        any = true;
        for (const auto& c : t->columns) {
          if (!c.hidden && std::ranges::find(merged, c.name) == merged.end()) out.push_back(column_expr(*t, c, e.pos));
        }
      }
      if (!any) throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
    }
    return out;
  }

  // A SELECT without FROM: its constants, one row of them, under its LIMIT.
  // A column is a column of no table (42703), as in PostgreSQL.
  auto select_without_from(const ast::Select& s) -> logical::NodePtr {
    for (const auto& e : s.targets) {
      if (e.kind == ast::Expr::Kind::Star) throw SqlError{"SELECT * with no tables specified is not valid", e.pos};
    }
    if (!s.where.empty() || !s.group_by.empty() || !s.having.empty() || !s.order_by.empty() || s.for_update) {
      not_yet("a SELECT without FROM with WHERE, GROUP BY, HAVING, ORDER BY or FOR UPDATE", s.pos);
    }
    std::vector<logical::Output> targets;
    for (const auto& e : s.targets) {
      if (e.kind == ast::Expr::Kind::Column) {
        throw SqlError{"column \"" + e.column + "\" does not exist", e.pos, kUndefinedColumn};
      }
      if (e.kind == ast::Expr::Kind::Subquery) {
        targets.push_back(subquery_output(e));
        continue;
      }
      if (!is_constant(e)) not_yet("a SELECT without FROM of other than constants and subqueries", e.pos);
      targets.push_back(constant_output(e));
    }
    named(s.targets, targets);
    auto rows = logical::node(logical::Project{logical::node(logical::OneRow{}), std::move(targets)});
    if (s.limit) rows = logical::node(logical::Limit{std::move(rows), *s.limit});
    return rows;
  }

  // One HAVING conjunct: a grouped column or an aggregate call compared with
  // a parameter, in either order. The parameter takes the column's type, or
  // the aggregate's: COUNT's and SUM's of a SMALLINT or INTEGER a BIGINT,
  // SUM's of a BIGINT or DECIMAL the numeric at its scale, checked as a
  // BIGINT is (compared_type), MIN's and MAX's their column's. HAVING on AVG
  // is not supported: its quotient is no type the plan holds.
  auto having_conjunct(const ast::Compare& w, const auto& resolve, const auto& grouped) -> logical::Expr {
    if (w.kind != ast::Compare::Kind::Compare) not_yet("HAVING with AND, OR, NOT, IS NULL, <>, BETWEEN or IN", w.pos);
    const auto* lhs = &w.lhs;
    const auto* rhs = &w.rhs;
    auto op = w.op;
    if (lhs->kind == ast::Expr::Kind::Param || is_constant(*lhs)) {
      std::swap(lhs, rhs);
      op = mirrored(op);
    }
    auto constant = is_constant(*rhs);
    if ((lhs->kind != ast::Expr::Kind::Column && lhs->kind != ast::Expr::Kind::Aggregate) ||
        (rhs->kind != ast::Expr::Kind::Param && !constant)) {
      throw SqlError{"HAVING must compare a grouped column or an aggregate with a parameter or a constant", w.pos};
    }
    if (lhs->kind == ast::Expr::Kind::Column) {
      auto [t, c] = resolve(*lhs);
      auto g = grouped(*t, *c, lhs->pos);
      if (constant) return logical::compare(op, std::move(g), fixed(coerce(fold(*rhs), *c, false, op_text(op)), c->sql));
      auto sql = having_param(rhs->param, c->type, rhs->pos, *c, t->id);
      return logical::compare(op, std::move(g), logical::param(rhs->param, sql));
    }
    logical::Output agg;
    const Column* arg = nullptr;
    std::uint32_t table_id = 0;
    if (lhs->column.empty()) {
      agg = aggregate(nullptr, 0, *lhs);
    } else {
      auto [t, c] = resolve(*lhs);
      arg = c;
      table_id = t->id;
      agg = aggregate(c, t->id, *lhs);
    }
    if (agg.expr.fn == logical::Expr::Fn::Avg) throw SqlError{"HAVING on AVG is not supported yet", lhs->pos};
    const Column* as = arg;
    if (agg.expr.fn == logical::Expr::Fn::Count || agg.expr.fn == logical::Expr::Fn::Sum) {
      synthetic_.push_back(Column{0, agg.name, agg.expr.type, PhysType::I64, true, {}});
      as = &synthetic_.back();
    }
    if (constant) {
      return logical::compare(op, std::move(agg.expr), fixed(coerce(fold(*rhs), *as, false, op_text(op)), as->sql));
    }
    auto sql = having_param(rhs->param, PhysType::I64, rhs->pos, *as, table_id);
    return logical::compare(op, std::move(agg.expr), logical::param(rhs->param, sql));
  }

  // A HAVING parameter is typed by its first use, as PostgreSQL types it:
  // the WHERE is bound first, so a $n it compares already has its type, and
  // the HAVING compares its key or aggregate with that. Where both are
  // integers of scale 0 (SMALLINT, INTEGER, BIGINT, a DECIMAL of scale 0) the
  // comparison is exact whatever the value, so it takes the first use's type
  // and range check; any other pair is refused, as two uses of one $n are
  // (use_param). Its SQL type.
  auto having_param(std::uint32_t n, PhysType t, std::int32_t pos, const Column& as, std::uint32_t table) -> SqlType {
    auto integral = [](SqlType s) {
      return s.kind == SqlKind::SmallInt || s.kind == SqlKind::Integer || s.kind == SqlKind::BigInt ||
             (s.kind == SqlKind::Decimal && s.scale == 0);
    };
    if (n < params_.size() && params_[n].first && params_[n].first->phys == PhysType::I64 && t == PhysType::I64 &&
        integral(params_[n].first->col->sql) && integral(as.sql)) {
      return params_[n].first->col->sql;
    }
    use_param(n, t, pos, as, table, Use::Compared);
    return as.sql;
  }

  // The rows an UPDATE or DELETE writes: its table's, by its WHERE.
  static auto rows(const Table& t, std::vector<logical::Expr> conjuncts) -> logical::NodePtr {
    auto get = logical::node(logical::Get{&t, false});
    if (conjuncts.empty()) return get;
    return logical::node(logical::Filter{std::move(get), std::move(conjuncts)});
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
      if (!params_[i].first) throw SqlError{"could not determine data type of parameter $" + std::to_string(i + 1), pos, kIndeterminateDatatype};
      out.push_back(params_[i].first->phys);
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
    for (const auto& p : params_) out.push_back({p.first->col->sql, p.assigned_only});
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
    // Its first use, which gives it its physical type and its SQL type. The
    // three are set together, so a typed parameter always has a column.
    struct FirstUse {
      PhysType phys;
      const Column* col;
      Use use;
    };
    std::optional<FirstUse> first;
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
  std::uint32_t own_params_;
  std::vector<std::string> using_;  // a join's USING or NATURAL columns: one column each
  std::string using_alias_;         // USING (…) AS alias
  std::vector<ParamUse> params_;
  std::vector<Datum> fixed_;  // the constants' values, $own_params_ + 1 onwards
  std::vector<PhysType> fixed_types_;
  std::vector<logical::Subquery> subqueries_;  // in the order bound; InSet names one by its index
  int depth_{0};                               // inside a subquery: one more is refused
  std::vector<const Table*> scope_;            // the tables of the query being bound
  // What a HAVING parameter compared with COUNT or SUM is typed by: no
  // column of a table, kept where a parameter's first use points.
  std::deque<Column> synthetic_;
};

// ------------------------------------------------------------------- views

// Each subquery slot of an expression or a WHERE conjunct, `f` called with
// the shared_ptr (const or not as `e` is).
template <typename E, typename F>
void subqueries_in(E& e, F& f) {
  if (e.subquery) f(e.subquery);
  if (e.lhs) subqueries_in(*e.lhs, f);
  if (e.rhs) subqueries_in(*e.rhs, f);
  for (auto& a : e.args) subqueries_in(a, f);
}

template <typename C, typename F>
void subqueries_in_compare(C& c, F& f) {
  subqueries_in(c.lhs, f);
  subqueries_in(c.rhs, f);
  if (c.subquery) f(c.subquery);
  for (auto& a : c.args) subqueries_in_compare(a, f);
}

template <typename S, typename F>
void each_subquery(S& s, F&& f) {
  for (auto& e : s.targets) subqueries_in(e, f);
  for (auto& e : s.group_by) subqueries_in(e, f);
  for (auto& c : s.where) subqueries_in_compare(c, f);
  for (auto& c : s.having) subqueries_in_compare(c, f);
}

// A view's column in a reference: the select-list item it names, to take its
// place. nullopt: the reference is left as written.
using ColumnSwap = std::function<std::optional<ast::Expr>(ast::Expr&)>;

void qualify(ast::Expr& e, const std::string& table) {
  if ((e.kind == ast::Expr::Kind::Column || (e.kind == ast::Expr::Kind::Aggregate && !e.column.empty())) && e.table.empty()) {
    e.table = table;
  }
  if (e.lhs) qualify(*e.lhs, table);
  if (e.rhs) qualify(*e.rhs, table);
  for (auto& a : e.args) qualify(a, table);
}

void qualify(ast::Compare& c, const std::string& table) {
  using K = ast::Compare::Kind;
  switch (c.kind) {
    case K::Compare:
    case K::Like:
      qualify(c.lhs, table);
      qualify(c.rhs, table);
      break;
    case K::IsNull:
    case K::Quantified: qualify(c.lhs, table); break;
    case K::And:
    case K::Or:
    case K::Not:
      for (auto& a : c.args) qualify(a, table);
      break;
    case K::Exists: break;
  }
}

// Replaces each column of `e` that `swap` answers for, and the column an
// aggregate reads, which must stay a column. A subquery is left: it reads
// no column of the query around it.
void swap_columns(ast::Expr& e, const ColumnSwap& swap) {
  if (e.kind == ast::Expr::Kind::Column) {
    if (auto repl = swap(e)) {
      repl->pos = e.pos;
      e = std::move(*repl);
    }
    return;
  }
  if (e.kind == ast::Expr::Kind::Aggregate && !e.column.empty()) {
    ast::Expr ref{ast::Expr::Kind::Column, e.column, 0, nullptr, nullptr, e.pos};
    ref.table = e.table;
    if (auto repl = swap(ref)) {
      if (repl->kind != ast::Expr::Kind::Column) {
        not_yet("an aggregate of a view column that is an expression", e.pos);
      }
      ref = std::move(*repl);
    }
    e.column = ref.column;
    e.table = ref.table;
    return;
  }
  if (e.lhs) swap_columns(*e.lhs, swap);
  if (e.rhs) swap_columns(*e.rhs, swap);
  for (auto& a : e.args) swap_columns(a, swap);
}

void swap_columns(ast::Compare& c, const ColumnSwap& swap) {
  using K = ast::Compare::Kind;
  switch (c.kind) {
    case K::Compare:
    case K::Like:
      swap_columns(c.lhs, swap);
      swap_columns(c.rhs, swap);
      break;
    case K::IsNull:
    case K::Quantified: swap_columns(c.lhs, swap); break;
    case K::And:
    case K::Or:
    case K::Not:
      for (auto& a : c.args) swap_columns(a, swap);
      break;
    case K::Exists: break;
  }
}

// The same column, one reference perhaps qualified and the other not.
auto same_column(const ast::Expr& a, const ast::Expr& b) -> bool {
  return a.column == b.column && (a.table == b.table || a.table.empty() || b.table.empty());
}

auto is_group_column(const ast::Expr& e, const std::vector<ast::Expr>& group) -> bool {
  return e.kind == ast::Expr::Kind::Column &&
         std::ranges::any_of(group, [&](const ast::Expr& g) { return same_column(e, g); });
}

// Whether `e` reads only columns of `group` and calls no aggregate.
auto over_group(const ast::Expr& e, const std::vector<ast::Expr>& group) -> bool {
  if (e.kind == ast::Expr::Kind::Column) return is_group_column(e, group);
  if (e.kind == ast::Expr::Kind::Aggregate) return false;
  return (!e.lhs || over_group(*e.lhs, group)) && (!e.rhs || over_group(*e.rhs, group)) &&
         std::ranges::all_of(e.args, [&](const ast::Expr& a) { return over_group(a, group); });
}

auto over_group(const ast::Compare& c, const std::vector<ast::Expr>& group) -> bool {
  using K = ast::Compare::Kind;
  switch (c.kind) {
    case K::Compare:
    case K::Like: return over_group(c.lhs, group) && over_group(c.rhs, group);
    case K::IsNull:
    case K::Quantified: return over_group(c.lhs, group);
    case K::And:
    case K::Or:
    case K::Not: return std::ranges::all_of(c.args, [&](const ast::Compare& a) { return over_group(a, group); });
    case K::Exists: return true;
  }
  return false;
}

// A view, expanded to base tables: its SELECT, and its column names, the
// select list's items in order (docs/frontend_design.md, *Views*).
struct ViewBody {
  ast::Select select;
  std::vector<std::string> names;
};

// Expands the views a statement reads before it is bound, so the binder,
// planner and plans see base tables alone: a FROM that names a view takes the
// view's FROM and WHERE, and each reference to one of its columns the
// select-list item the column is. The view's text is parsed and bound at each
// use, so it is checked against the tables as they are then.
class ViewExpander {
public:
  explicit ViewExpander(const Catalog& cat) : cat_{cat} {}

  [[nodiscard]] auto used() const -> const std::vector<std::uint32_t>& { return used_; }

  // The statement with its views expanded; nullopt where it reads none.
  [[nodiscard]] auto statement(const ast::Statement& stmt) -> std::optional<ast::Statement> {
    if (const auto* s = std::get_if<ast::Select>(&stmt)) {
      if (!mentions(*s)) return std::nullopt;
      return std::optional<ast::Statement>{std::in_place, std::in_place_type<ast::Select>, expand(*s)};
    }
    if (const auto* u = std::get_if<ast::Update>(&stmt)) {
      ast::Update out{u->table, {}, {}, u->pos};
      for (const auto& a : u->set) out.set.push_back({a.column, ast::clone(a.value), a.pos});
      for (const auto& c : u->where) out.where.push_back(ast::clone(c));
      if (!rewrite(out.set, out.where)) return std::nullopt;
      return std::optional<ast::Statement>{std::in_place, std::in_place_type<ast::Update>, std::move(out)};
    }
    if (const auto* d = std::get_if<ast::Delete>(&stmt)) {
      ast::Delete out{d->table, {}, d->pos};
      for (const auto& c : d->where) out.where.push_back(ast::clone(c));
      std::vector<ast::Assignment> none;
      if (!rewrite(none, out.where)) return std::nullopt;
      return std::optional<ast::Statement>{std::in_place, std::in_place_type<ast::Delete>, std::move(out)};
    }
    return std::nullopt;
  }

  // A view's body from its defining SELECT and column list: expanded, its
  // stars written out, its columns named and checked.
  auto describe(const ast::Select& query, const std::vector<std::string>& columns) -> ViewBody {
    ViewBody b{expand(query), {}};
    Binder scratch{cat_, 0};
    if (!b.select.set &&
        std::ranges::any_of(b.select.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Star; })) {
      b.select.targets = scratch.expand_stars(b.select);
    }
    auto root = scratch.select(b.select);
    for (const auto& out : Binder::outputs_of(*root)) b.names.push_back(out.name);
    if (!b.select.set && b.names.size() != b.select.targets.size()) {
      throw std::logic_error{"a view's columns are not its select list"};
    }
    if (columns.size() > b.names.size()) {
      throw SqlError{"CREATE VIEW specifies more column names than columns", b.select.pos};
    }
    std::ranges::copy(columns, b.names.begin());
    for (std::size_t i = 0; i < b.names.size(); ++i) {
      if (std::ranges::find(b.names, b.names[i]) != b.names.begin() + static_cast<std::ptrdiff_t>(i)) {
        throw SqlError{"column \"" + b.names[i] + "\" specified more than once", b.select.pos, kDuplicateColumn};
      }
    }
    return b;
  }

  // The tables and views a SELECT names directly, subqueries included.
  void relations(const ast::Select& s, std::vector<std::uint32_t>& out) const {
    if (s.set) {
      relations(s.set->left, out);
      relations(s.set->right, out);
      return;
    }
    for (const auto* name : {&s.table, &s.join}) {
      if (name->empty()) continue;
      if (const auto* t = cat_.table(*name)) out.push_back(t->id);
      if (const auto* v = cat_.view(*name)) out.push_back(v->id);
    }
    each_subquery(s, [&](const std::shared_ptr<const ast::Select>& q) { relations(*q, out); });
  }

private:
  static constexpr int kMaxDepth = 16;

  [[nodiscard]] auto mentions(const ast::Select& s) const -> bool {
    if (s.set) return mentions(s.set->left) || mentions(s.set->right);
    auto found = cat_.view(s.table) != nullptr || cat_.view(s.join) != nullptr;
    each_subquery(s, [&](const std::shared_ptr<const ast::Select>& q) { found = found || mentions(*q); });
    return found;
  }

  // Whether a view is named in the SET values or WHERE, subqueries included;
  // if so they are rewritten in place.
  auto rewrite(std::vector<ast::Assignment>& set, std::vector<ast::Compare>& where) -> bool {
    auto found = false;
    auto expand_slot = [&](std::shared_ptr<const ast::Select>& q) {
      if (!mentions(*q)) return;
      q = std::make_shared<const ast::Select>(expand(*q));
      found = true;
    };
    for (auto& a : set) subqueries_in(a.value, expand_slot);
    for (auto& c : where) subqueries_in_compare(c, expand_slot);
    return found;
  }

  auto body(const View& v) -> const ViewBody& {
    if (auto it = bodies_.find(v.id); it != bodies_.end()) return it->second;
    if (depth_ >= kMaxDepth) throw SqlError{"views are nested too deeply", -1, kFeatureNotSupported};
    ++depth_;
    auto parsed = ast::parse(v.definition);
    auto* def = std::get_if<ast::CreateView>(&parsed);
    if (def == nullptr) throw SqlError{"the definition of view \"" + v.name + "\" is not a CREATE VIEW", -1};
    auto described = describe(def->query, def->columns);
    --depth_;
    if (described.names != v.columns) {
      throw SqlError{"view \"" + v.name + "\" no longer has the columns it was created with: the tables it reads changed; drop it and create it again",
                     -1, kFeatureNotSupported};
    }
    if (std::ranges::find(used_, v.id) == used_.end()) used_.push_back(v.id);
    return bodies_.emplace(v.id, std::move(described)).first->second;
  }

  static auto aggregated(const ast::Select& s) -> bool {
    return !s.group_by.empty() || !s.having.empty() ||
           std::ranges::any_of(s.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Aggregate; });
  }

  static auto column_named(const ast::Expr& star, const std::string& table, const std::string& name) -> ast::Expr {
    ast::Expr c{ast::Expr::Kind::Column, name, 0, nullptr, nullptr, star.pos};
    c.table = table;
    return c;
  }

  auto expand(const ast::Select& in) -> ast::Select {
    auto s = ast::clone(in);
    if (s.set) {
      if (mentions(in)) {
        s.set = std::make_shared<const ast::SetOperation>(
            ast::SetOperation{s.set->kind, s.set->all, expand(s.set->left), expand(s.set->right)});
      }
      return s;
    }
    auto expand_slot = [&](std::shared_ptr<const ast::Select>& q) {
      if (mentions(*q)) q = std::make_shared<const ast::Select>(expand(*q));
    };
    each_subquery(s, expand_slot);
    const auto* left = cat_.view(s.table);
    const auto* right = cat_.view(s.join);
    if (left == nullptr && right == nullptr) return s;
    if (s.join.empty()) {
      merge_one(s, body(*left));
    } else {
      merge_two(s, left, right);
    }
    return s;
  }

  // FROM view [WHERE …]: the view's FROM, and its WHERE and the query's
  // together; over a grouped view the query's WHERE is a HAVING, the groups
  // it keeps. The query may not group or aggregate again, but where a group
  // is a row of the view (regroup, below); nor may it read through a view
  // that cuts rows (DISTINCT, LIMIT).
  void merge_one(ast::Select& s, const ViewBody& v) {
    const auto& view = v.select;
    if (view.set) not_yet("reading a view defined with a set operation", s.table_pos);
    auto grouped = aggregated(view);
    if (view.distinct || view.limit) not_yet("a view defined with DISTINCT or LIMIT", s.table_pos);
    if (view.for_update) not_yet("a view defined FOR UPDATE", s.table_pos);
    auto again = aggregated(s) || s.distinct;
    std::vector<ast::Expr> targets;
    for (auto& e : s.targets) {
      if (e.kind != ast::Expr::Kind::Star) {
        targets.push_back(std::move(e));
        continue;
      }
      if (!e.table.empty() && e.table != s.table) {
        throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
      }
      if (!e.alias.empty()) throw SqlError{"syntax error at or near \"AS\"", e.pos};
      for (const auto& name : v.names) targets.push_back(column_named(e, "", name));
    }
    s.targets = std::move(targets);
    auto swap = [&](ast::Expr& ref) -> std::optional<ast::Expr> {
      if (!ref.table.empty() && ref.table != s.table) {
        throw SqlError{"missing FROM-clause entry for table \"" + ref.table + "\"", ref.pos, kUndefinedTable};
      }
      auto at = std::ranges::find(v.names, ref.column);
      if (at == v.names.end()) throw SqlError{"column \"" + ref.column + "\" does not exist", ref.pos, kUndefinedColumn};
      return ast::clone(view.targets[static_cast<std::size_t>(at - v.names.begin())]);
    };
    apply(s, swap);
    if (grouped && again) {
      if (!regroup(s, view)) not_yet("grouping, aggregating or DISTINCT over a grouped view", s.pos);
    } else if (grouped) {
      s.having.clear();
      for (const auto& c : view.having) s.having.push_back(ast::clone(c));
      for (auto& c : s.where) s.having.push_back(std::move(c));
      s.where.clear();
      s.group_by.clear();
      for (const auto& g : view.group_by) s.group_by.push_back(ast::clone(g));
    }
    // The query takes the view's FROM, and its WHERE joins the query's.
    s.table = view.table;
    s.join = view.join;
    s.table_pos = view.table_pos;
    s.join_pos = view.join_pos;
    s.using_columns = view.using_columns;
    s.natural = view.natural;
    s.using_alias = view.using_alias;
    s.join_kind = view.join_kind;
    std::vector<ast::Compare> where;
    for (const auto& c : view.where) where.push_back(ast::clone(c));
    for (auto& c : s.where) where.push_back(std::move(c));
    s.where = std::move(where);
    if (s.order_by.empty() && !s.distinct) s.order_by = view.order_by;
  }

  // A query that groups again over a grouped view, already substituted, is
  // answered from the view's own query where that is the same answer: GROUP BY
  // the view's group columns, no aggregate, only those columns read. A view
  // row is a group, so grouping the rows by what groups them again is the
  // view; the query's WHERE and HAVING are the view's HAVING. Any other
  // waits for a derived table.
  static auto regroup(ast::Select& s, const ast::Select& view) -> bool {
    const auto& group = view.group_by;
    auto in_group = [&](const ast::Expr& e) { return is_group_column(e, group); };
    auto ordered_by_group = [&](const ast::OrderItem& o) {
      return std::ranges::any_of(group, [&](const ast::Expr& g) {
        return g.column == o.column && (o.table.empty() || g.table.empty() || g.table == o.table);
      });
    };
    auto groups_alike = std::ranges::all_of(s.group_by, in_group) &&
                        std::ranges::all_of(group, [&](const ast::Expr& g) {
                          return std::ranges::any_of(s.group_by, [&](const ast::Expr& e) { return same_column(e, g); });
                        });
    if (s.distinct || s.group_by.empty() || !groups_alike || !std::ranges::all_of(s.targets, in_group) ||
        !std::ranges::all_of(s.order_by, ordered_by_group) ||
        !std::ranges::all_of(s.having, [&](const ast::Compare& c) { return over_group(c, group); })) {
      return false;
    }
    std::vector<ast::Compare> having;
    for (const auto& c : view.having) having.push_back(ast::clone(c));
    for (auto& c : s.where) having.push_back(std::move(c));
    for (auto& c : s.having) having.push_back(std::move(c));
    s.having = std::move(having);
    s.where.clear();
    s.group_by.clear();
    for (const auto& g : group) s.group_by.push_back(ast::clone(g));
    return true;
  }

  // The query's clauses, each view column replaced by its item. A select-list
  // column keeps its view name; a grouping or sort column must stay a column.
  void apply(ast::Select& s, const ColumnSwap& swap) {
    for (auto& e : s.targets) {
      auto name = e.kind == ast::Expr::Kind::Column ? (e.alias.empty() ? e.column : e.alias) : std::string{};
      swap_columns(e, swap);
      if (!name.empty()) e.alias = name;
    }
    for (auto& c : s.where) swap_columns(c, swap);
    for (auto& c : s.having) swap_columns(c, swap);
    for (auto& g : s.group_by) {
      swap_columns(g, swap);
      if (g.kind != ast::Expr::Kind::Column) not_yet("grouping by a view column that is an expression", g.pos);
    }
    for (auto& o : s.order_by) {
      auto ref = ast::Expr{ast::Expr::Kind::Column, o.column, 0, nullptr, nullptr, o.pos};
      ref.table = o.table;
      swap_columns(ref, swap);
      if (ref.kind != ast::Expr::Kind::Column) not_yet("ordering by a view column that is an expression", o.pos);
      o.column = ref.column;
      o.table = ref.table;
    }
  }

  // FROM view JOIN table, a table JOIN view, or two views, inner or outer:
  // each view a single table's plain SELECT, replaced by its table, its
  // columns by its items qualified with it, its WHERE added to the query's.
  struct Side {
    std::string name;
    const ViewBody* view{nullptr};
    const Table* table{nullptr};
  };
  void merge_two(ast::Select& s, const View* left, const View* right) {
    Side sides[2]{{s.table, left != nullptr ? &body(*left) : nullptr, cat_.table(s.table)},
                  {s.join, right != nullptr ? &body(*right) : nullptr, cat_.table(s.join)}};
    for (auto i = 0; i < 2; ++i) {
      if (sides[i].view == nullptr && sides[i].table == nullptr) {
        throw SqlError{"relation \"" + sides[i].name + "\" does not exist", i == 0 ? s.table_pos : s.join_pos, kUndefinedTable};
      }
    }
    auto outer = s.join_kind != ast::Select::JoinKind::Inner;
    if (!s.using_columns.empty() || s.natural) not_yet("USING or NATURAL join of a view", s.join_pos);
    for (const auto& side : sides) {
      if (side.view == nullptr) continue;
      const auto& v = side.view->select;
      if (v.table.empty() || !v.join.empty() || aggregated(v) || v.distinct || v.limit || v.for_update) {
        not_yet("a join of view \"" + side.name + "\", which is not a plain SELECT of one table", s.join_pos);
      }
      if (outer && !v.where.empty()) not_yet("an outer join of view \"" + side.name + "\", which has a WHERE", s.join_pos);
    }
    auto has = [](const Side& side, const std::string& column) {
      if (side.view != nullptr) return std::ranges::find(side.view->names, column) != side.view->names.end();
      return side.table->column(column) != nullptr;
    };
    // The side a column reference names or, unqualified, the only one that has it.
    auto side_of = [&](const ast::Expr& ref) -> int {
      if (!ref.table.empty()) return ref.table == sides[0].name ? 0 : ref.table == sides[1].name ? 1 : -1;
      auto hit = -1;
      for (auto i = 0; i < 2; ++i) {
        if (!has(sides[i], ref.column)) continue;
        if (hit >= 0) throw SqlError{"column reference \"" + ref.column + "\" is ambiguous", ref.pos, kAmbiguousColumn};
        hit = i;
      }
      return hit;
    };
    auto swap = [&](ast::Expr& ref) -> std::optional<ast::Expr> {
      auto i = side_of(ref);
      if (i < 0) return std::nullopt;
      const auto& side = sides[i];
      if (side.view == nullptr) {
        ref.table = side.name;
        return std::nullopt;
      }
      auto at = std::ranges::find(side.view->names, ref.column);
      if (at == side.view->names.end()) {
        throw SqlError{"column \"" + ref.column + "\" does not exist", ref.pos, kUndefinedColumn};
      }
      auto item = ast::clone(side.view->select.targets[static_cast<std::size_t>(at - side.view->names.begin())]);
      qualify(item, side.view->select.table);
      return item;
    };
    std::vector<ast::Expr> targets;
    for (auto& e : s.targets) {
      if (e.kind != ast::Expr::Kind::Star) {
        targets.push_back(std::move(e));
        continue;
      }
      if (!e.alias.empty()) throw SqlError{"syntax error at or near \"AS\"", e.pos};
      if (!e.table.empty() && e.table != sides[0].name && e.table != sides[1].name) {
        throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
      }
      for (const auto& side : sides) {
        if (!e.table.empty() && e.table != side.name) continue;
        if (side.view != nullptr) {
          for (const auto& name : side.view->names) targets.push_back(column_named(e, side.name, name));
        } else {
          for (const auto& c : side.table->columns) {
            if (!c.hidden) targets.push_back(column_named(e, side.name, c.name));
          }
        }
      }
    }
    s.targets = std::move(targets);
    apply(s, swap);
    for (const auto& side : sides) {
      if (side.view == nullptr) continue;
      for (const auto& c : side.view->select.where) {
        s.where.push_back(ast::clone(c));
        qualify(s.where.back(), side.view->select.table);
      }
    }
    s.table = sides[0].view != nullptr ? sides[0].view->select.table : sides[0].name;
    s.join = sides[1].view != nullptr ? sides[1].view->select.table : sides[1].name;
  }

  const Catalog& cat_;
  std::map<std::uint32_t, ViewBody> bodies_;
  std::vector<std::uint32_t> used_;
  int depth_{0};
};

// CREATE VIEW: the body is bound as a use would, so its errors are the
// statement's; its definition is the statement's text.
auto bind_create_view(const ast::CreateView& cv, const Catalog& cat) -> Bound {
  if (select_param_count(cv.query) != 0) throw SqlError{"there is no parameter $1", cv.pos, kUndefinedParameter};
  ViewExpander views{cat};
  CreateViewDdl ddl{cv.view, cv.definition, {}, views.describe(cv.query, cv.columns).names};
  views.relations(cv.query, ddl.depends_on);
  return Bound{Ddl{std::move(ddl)}, {}, {}, {}, {}, {}, {}, {}};
}

}  // namespace

auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound {
  if (const auto* cv = std::get_if<ast::CreateView>(&stmt)) return bind_create_view(*cv, cat);
  ViewExpander views{cat};
  auto expanded = views.statement(stmt);
  const auto& target = expanded ? *expanded : stmt;
  Binder b{cat, param_count(target)};
  auto bound_stmt = std::visit(
      [&](const auto& s) -> std::pair<std::variant<Ddl, logical::NodePtr>, std::int32_t> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ast::CreateTable>) {
          CreateTableDdl out{s.table, {}, s.pk, {}};
          for (const auto& c : s.columns) {
            out.columns.push_back(column_spec(c));
            if (c.primary_key) out.pk.push_back(c.name);
          }
          for (const auto& u : s.unique) out.unique.push_back({u.name, u.columns});
          for (const auto& f : s.foreign_keys) {
            // A table may reference itself, by the keys it is created with.
            check_foreign_key(out, f, f.table == s.table ? referenced_keys(out) : referenced_keys(*b.table(f.table, f.pos)));
          }
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
        } else if constexpr (std::is_same_v<T, ast::DropView>) {
          return {Ddl{DropViewDdl{s.view, s.if_exists}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateView>) {
          throw std::logic_error("CREATE VIEW is bound by bind_create_view");
        } else if constexpr (std::is_same_v<T, ast::AddColumn>) {
          (void)b.table(s.table, s.pos);
          if (s.column.primary_key) throw SqlError{"ADD COLUMN cannot add a primary key column", s.column.pos};
          if (!s.column.unique.empty()) not_yet("ADD COLUMN … UNIQUE", s.column.pos);
          if (!s.column.references.empty()) not_yet("ADD COLUMN … REFERENCES", s.column.pos);
          auto spec = column_spec(s.column);
          if (!spec.nullable && std::holds_alternative<std::monostate>(spec.default_value)) {
            throw SqlError{"column \"" + s.column.name + "\" is NOT NULL and needs a DEFAULT for the existing rows",
                           s.column.pos};
          }
          return {Ddl{AddColumnDdl{s.table, std::move(spec)}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Insert>) {
          const auto* t = b.table(s.table, s.pos);
          logical::Insert out{t, std::vector<std::optional<logical::Expr>>(t->columns.size())};
          std::vector<const Column*> targets;
          if (s.columns.empty()) {
            // As PostgreSQL: the values fill the leading columns and the rest
            // take their defaults, so a column added later changes nothing.
            // The hidden row id is no column a statement fills.
            for (const auto& c : t->columns) {
              if (targets.size() == s.values.size()) break;
              if (!c.hidden) targets.push_back(&c);
            }
            if (s.values.size() > targets.size()) {
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
            if (out.values[at]->kind != logical::Expr::Kind::Param) {
              throw SqlError{"INSERT values must be parameters or constants", s.values[i].pos};
            }
          }
          // A column left out takes its default. One that is NOT NULL with no
          // default, a primary key column among them, is refused when the
          // row is built (BC_C_NOT_NULL), as a NULL given for it is.
          return {logical::node(std::move(out)), s.pos};
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          return {b.select(s), s.pos};
        } else if constexpr (std::is_same_v<T, ast::Update>) {
          const auto* t = b.table(s.table, s.pos);
          std::vector<std::pair<logical::ColumnRef, logical::Expr>> set;
          for (const auto& a : s.set) {
            const auto* c = Binder::column(*t, a.column, a.pos);
            if (std::ranges::find(t->pk_col_ids, c->id) != t->pk_col_ids.end()) {
              throw SqlError{"updating a primary key column is not supported", a.pos};
            }
            auto v = b.value(*t, a.value, *c);
            // A constant is checked against the column's type now, as
            // PostgreSQL folds its cast when it plans the statement.
            if (v.kind == logical::Expr::Kind::Param && b.is_fixed(v.param)) {
              if (auto why = out_of_range(c->sql, b.fixed_value(v.param))) out_of_type(*why, a.value.pos);
            } else if (v.kind == logical::Expr::Kind::Param) {
              b.check_assigned(v.param, *c, t->id);
            }
            set.emplace_back(logical::column_of(t->id, *c).col, std::move(v));
          }
          b.set_scope({t});
          auto rows = Binder::rows(*t, b.where(*t, s.where));
          return {logical::node(logical::Update{std::move(rows), t, std::move(set)}), s.pos};
        } else {
          const auto* t = b.table(s.table, s.pos);
          b.set_scope({t});
          return {logical::node(logical::Delete{Binder::rows(*t, b.where(*t, s.where)), t}), s.pos};
        }
      },
      target);
  auto types = b.param_types(bound_stmt.second);
  types.insert(types.end(), b.fixed_types().begin(), b.fixed_types().end());
  return Bound{std::move(bound_stmt.first), std::move(types), b.param_sql(), b.param_table(), b.param_info(),
               b.fixed_values(), b.take_subqueries(), views.used()};
}

}  // namespace bytecask::sql::bound
