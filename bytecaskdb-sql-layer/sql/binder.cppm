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
    case K::Compare: return is_constant(*e.lhs) && is_constant(*e.rhs);
    case K::Cast: return is_constant(*e.rhs);
    case K::Column:
    case K::Param:
    case K::Aggregate: return false;
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

// x + y or x - y, of integer or numeric constants, as PostgreSQL's
// operators type it: INTEGER of two INTEGERs, BIGINT of two integers
// otherwise, numeric at the finer scale with a numeric; the range checked
// as there (22003). NULL beside a number is NULL of the number's type.
auto add(const Constant& x, const Constant& y, bool sub, std::int32_t pos) -> Constant {
  auto op = std::string{sub ? "-" : "+"};
  auto null_untyped = [](const Constant& c) { return c.unknown && std::holds_alternative<std::monostate>(c.value); };
  if (null_untyped(x) && null_untyped(y)) not_yet("NULL " + op + " NULL", pos);
  for (const auto* c : {&x, &y}) {
    if (c->unknown && !null_untyped(*c)) not_yet("a string literal in arithmetic", c->pos);
    if (!c->unknown && !is_integer(c->type) && c->type.kind != SqlKind::Decimal) {
      throw SqlError{"operator does not exist: " + pg_type_name(x.type) + " " + op + " " + pg_type_name(y.type), pos,
                     kUndefinedFunction};
    }
  }
  auto type = [&] {
    if (null_untyped(x)) return y.type;
    if (null_untyped(y)) return x.type;
    if (is_integer(x.type) && is_integer(y.type)) {
      return SqlType{x.type.kind == SqlKind::Integer && y.type.kind == SqlKind::Integer ? SqlKind::Integer : SqlKind::BigInt};
    }
    return numeric_type(std::max(scaled(x).scale, scaled(y).scale));
  }();
  if (null_untyped(x) || null_untyped(y)) return {std::monostate{}, type, false, {}, pos};
  if (std::holds_alternative<std::monostate>(x.value) || std::holds_alternative<std::monostate>(y.value)) {
    return {std::monostate{}, type, false, {}, pos};
  }
  std::int64_t out = 0;
  if (is_integer(type)) {
    auto a = std::get<std::int64_t>(x.value);
    auto b = std::get<std::int64_t>(y.value);
    auto overflow = sub ? __builtin_sub_overflow(a, b, &out) : __builtin_add_overflow(a, b, &out);
    if (overflow || (type.kind == SqlKind::Integer && !std::in_range<std::int32_t>(out))) {
      throw SqlError{type.kind == SqlKind::Integer ? "integer out of range" : "bigint out of range", pos,
                     kNumericValueOutOfRange};
    }
    return {out, type, false, {}, pos};
  }
  auto a = rescale(scaled(x).v, static_cast<std::uint8_t>(type.scale - scaled(x).scale));
  auto b = rescale(scaled(y).v, static_cast<std::uint8_t>(type.scale - scaled(y).scale));
  if (!a || !b) not_yet("a numeric of more than 18 digits", pos);
  out = sub ? *a - *b : *a + *b;  // each below 10^18: no overflow
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
    case K::Add:
    case K::Sub: return add(fold(*e.lhs), fold(*e.rhs), e.kind == K::Sub, e.pos);
    case K::Compare: return compare_constants(fold(*e.lhs), fold(*e.rhs), e.cmp, e.pos);
    case K::Cast: return cast_constant(e, fold(*e.rhs));
    case K::Column:
    case K::Param:
    case K::Aggregate: break;
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
// an assigned value is checked where the value is written, as an assigned
// parameter's is.
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

auto param_count(const ast::Statement& stmt) -> std::uint32_t {
  std::uint32_t n = 0;
  auto conjuncts = [&](const std::vector<ast::Compare>& cs) {
    for (const auto& c : cs) {
      params_in(c.lhs, n);
      params_in(c.rhs, n);
    }
  };
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ast::Insert>) {
          for (const auto& e : s.values) params_in(e, n);
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          for (const auto& e : s.targets) params_in(e, n);
          for (const auto& e : s.group_by) params_in(e, n);
          conjuncts(s.where);
          conjuncts(s.having);
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

  // A constant in a select list, of its own type: an unknown one is TEXT,
  // as PostgreSQL resolves it there. Named as PostgreSQL names it: a cast
  // by its type's name (int4, date), anything else ?column?.
  auto constant_output(const ast::Expr& e) -> logical::Output {
    auto c = fold(e);
    return {fixed(std::move(c.value), c.type), e.kind == ast::Expr::Kind::Cast ? e.cast_type : "?column?"};
  }

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
    if (is_constant(e)) return fixed(coerce(fold(e), target, true, "="), target.sql);
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
      case ast::Expr::Kind::Aggregate: throw SqlError{"aggregate functions are not allowed here", e.pos, kGroupingError};
      case ast::Expr::Kind::Plus:
      case ast::Expr::Kind::Neg: not_yet("unary + and - of other than a constant", e.pos);
      case ast::Expr::Kind::Compare: not_yet("a comparison of other than constants as a value", e.pos);
      case ast::Expr::Kind::Cast: not_yet("a cast of other than a constant", e.pos);
      case ast::Expr::Kind::Const:
      case ast::Expr::Kind::Literal: break;  // constants, above
    }
    throw SqlError{"expression not supported", e.pos};
  }

  auto where(const Table& t, const std::vector<ast::Compare>& in) -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
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
  auto compare(const ast::Compare& w, const auto& resolve) -> logical::Expr {
    const auto* col = &w.lhs;
    const auto* rhs = &w.rhs;
    auto op = w.op;
    if ((operand(*col) || is_constant(*col)) && rhs->kind == ast::Expr::Kind::Column) {
      std::swap(col, rhs);
      op = mirrored(op);
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
  auto where(const Table* (&sides)[2], const std::vector<ast::Compare>& in) -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
    for (const auto& w : in) {
      if (w.lhs.kind == ast::Expr::Kind::Column && w.rhs.kind == ast::Expr::Kind::Column) {
        out.push_back(join_on(sides, w));
        continue;
      }
      out.push_back(compare(w, [&](const ast::Expr& e) {
        auto [side, col] = column(sides, e);
        return std::pair{sides[side]->id, col};
      }));
    }
    return out;
  }

  // A column of one table equal to a column of the other, of one SQL type.
  static auto join_on(const Table* (&sides)[2], const ast::Compare& w) -> logical::Expr {
    auto [ls, lc] = column(sides, w.lhs);
    auto [rs, rc] = column(sides, w.rhs);
    if (ls == rs) throw SqlError{"comparing two columns of one table is not supported yet", w.pos};
    if (w.op != ast::Compare::Op::Eq) throw SqlError{"a join compares two columns with = only yet", w.pos};
    if (compared_type(lc->sql).identity != compared_type(rc->sql).identity) {
      throw SqlError{"comparing " + lc->name + " (" + sql_type_name(lc->sql) + ") with " + rc->name + " (" +
                         sql_type_name(rc->sql) + ") is not supported: a join compares columns of one type",
                     w.pos};
    }
    return logical::compare(ast::Compare::Op::Eq, logical::column_of(sides[ls]->id, *lc),
                            logical::column_of(sides[rs]->id, *rc));
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
    if (s.table.empty()) return select_without_from(s);
    auto joined = !s.join.empty();
    const Table* sides[2]{table(s.table, joined ? s.table_pos : s.pos), nullptr};
    std::vector<logical::Expr> conjuncts;
    if (joined) {
      sides[1] = table(s.join, s.join_pos);
      if (sides[0] == sides[1]) throw SqlError{"a table joined with itself is not supported", s.pos};
      if (s.for_update) throw SqlError{"FOR UPDATE on a join is not supported yet", s.pos};
      conjuncts = where(sides, s.where);
    } else {
      conjuncts = where(*sides[0], s.where);
    }
    // A column reference: of the one table, or of the side that has it.
    auto resolve = [&](const ast::Expr& e) -> std::pair<const Table*, const Column*> {
      if (!joined) return {sides[0], column(*sides[0], e)};
      auto [side, c] = column(sides, e);
      return {sides[side], c};
    };
    auto aggregated = !s.group_by.empty() || !s.having.empty() ||
                      std::ranges::any_of(s.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Aggregate; });
    // DISTINCT is a GROUP BY of the selected columns. Over a query already
    // aggregated it is nothing more where every grouped column is
    // selected, each group then one row; else not yet. Checked in
    // PostgreSQL's order.
    auto distinct_groups = s.distinct && !aggregated;
    if (s.distinct) {
      if (s.for_update) throw SqlError{"FOR UPDATE is not allowed with DISTINCT clause", s.pos, kFeatureNotSupported};
      auto selected = [&](const ast::Expr& e) {
        auto ref = resolve(e);
        return std::ranges::any_of(s.targets, [&](const ast::Expr& t) { return t.kind == ast::Expr::Kind::Column && resolve(t) == ref; });
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
      for (const auto& e : s.targets) {
        if (distinct_groups && e.kind != ast::Expr::Kind::Column) not_yet("SELECT DISTINCT of other than columns", e.pos);
      }
    }
    aggregated = aggregated || distinct_groups;
    auto grouping = !s.group_by.empty() || distinct_groups;
    std::vector<logical::Expr> group_by;
    for (const auto& e : distinct_groups ? s.targets : s.group_by) {
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
    for (const auto& e : s.targets) {
      if (is_constant(e)) {
        if (aggregated) not_yet("a constant in the select list of an aggregated query", e.pos);
        targets.push_back(constant_output(e));
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
                                                     logical::node(logical::Get{sides[1], false}), {}})
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
    return rows;
  }

  // A SELECT without FROM: its constants, one row of them, under its LIMIT.
  // A column is a column of no table (42703), as in PostgreSQL.
  auto select_without_from(const ast::Select& s) -> logical::NodePtr {
    if (!s.where.empty() || !s.group_by.empty() || !s.having.empty() || !s.order_by.empty() || s.for_update) {
      not_yet("a SELECT without FROM with WHERE, GROUP BY, HAVING, ORDER BY or FOR UPDATE", s.pos);
    }
    std::vector<logical::Output> targets;
    for (const auto& e : s.targets) {
      if (e.kind == ast::Expr::Kind::Column) {
        throw SqlError{"column \"" + e.column + "\" does not exist", e.pos, kUndefinedColumn};
      }
      if (!is_constant(e)) not_yet("a SELECT without FROM of other than constants", e.pos);
      targets.push_back(constant_output(e));
    }
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
  std::vector<ParamUse> params_;
  std::vector<Datum> fixed_;  // the constants' values, $own_params_ + 1 onwards
  std::vector<PhysType> fixed_types_;
  // What a HAVING parameter compared with COUNT or SUM is typed by: no
  // column of a table, kept where a parameter's first use points.
  std::deque<Column> synthetic_;
};

}  // namespace

auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound {
  Binder b{cat, param_count(stmt)};
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
          if (!s.column.unique.empty()) not_yet("ADD COLUMN … UNIQUE", s.column.pos);
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
          auto rows = Binder::rows(*t, b.where(*t, s.where));
          return {logical::node(logical::Update{std::move(rows), t, std::move(set)}), s.pos};
        } else {
          const auto* t = b.table(s.table, s.pos);
          return {logical::node(logical::Delete{Binder::rows(*t, b.where(*t, s.where)), t}), s.pos};
        }
      },
      stmt);
  auto types = b.param_types(bound_stmt.second);
  types.insert(types.end(), b.fixed_types().begin(), b.fixed_types().end());
  return Bound{std::move(bound_stmt.first), std::move(types), b.param_sql(), b.param_table(), b.param_info(), b.fixed_values()};
}

}  // namespace bytecask::sql::bound
