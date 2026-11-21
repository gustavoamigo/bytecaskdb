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
#include <cctype>
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
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

// A constant of the statement that is the time, not a literal: its slot among
// `Bound::fixed` is filled with the statement's time each time it runs, not
// when it was compiled (docs/frontend_design.md, *The time*).
// The session functions are such constants too: their slots are filled with
// the session's user, the database's name or the schema (Kind::User,
// Catalog, Schema), so a cached plan serves every session.
struct ClockSlot {
  enum class Kind { Date, Time, Timestamp, TimestampTz, User, Catalog, Schema, TimeTz };
  std::uint32_t slot;  // index into Bound::fixed
  Kind kind;
  std::uint8_t precision;  // fractional digits of a time or timestamp, 0 to 6
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
  // The entries of `fixed` that are the time (CURRENT_DATE, LOCALTIME,
  // LOCALTIMESTAMP, CURRENT_TIMESTAMP): their values here are placeholders,
  // 0, which nothing may fold or check as the statement's value: a WHERE
  // compares a column with the reading the API puts there on each run.
  std::vector<ClockSlot> clocks;
  // Each uncorrelated subquery, run before the plan: it sets its hidden
  // parameters, among `fixed`'s, whose values there are NULL.
  std::vector<logical::Subquery> subqueries;
  // The views the statement was expanded through, each once, nested ones
  // included. A cached plan is good only while each still exists: ids are
  // never reused, so a dropped and recreated view is another id.
  std::vector<std::uint32_t> views;
  // The instances of tables the plan reads (Table::source): the logical plan
  // names them, so they live as long as it does.
  std::vector<std::unique_ptr<Table>> instances;
  // A cursor's query (ast::Select::row_key): how many trailing result columns
  // are the scanned table's primary key, hidden from the client, and that
  // table's storage id. 0 and 0 for a query that is no simple scan of one
  // table.
  std::uint32_t key_columns{0};
  std::uint32_t key_table{0};
  // An UPDATE or DELETE ... WHERE CURRENT OF: the primary key of its table
  // as that many parameters after the statement's own, bound by the
  // cursor's current row; key_table is the table.
  std::uint32_t key_params{0};
};

// Throws ast::SqlError with a message and the position of the offending node.
[[nodiscard]] auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound;
// The same with $1 .. $n of the types `fixed_params` gives, as PostgreSQL
// binds a statement with declared parameter types: a prepared statement's,
// bound again after a DDL (docs/transaction_design.md, *A transaction binds
// only against its own catalog*). A parameter so typed compared with an
// integer column of another width is taken, as PostgreSQL's cross-type
// operators take it, and range-checked as its own type; any other use the
// type does not fit is the binder's error, 42883 across type classes.
[[nodiscard]] auto bind(const ast::Statement& stmt, const Catalog& cat, std::span<const SqlType> fixed_params) -> Bound;

// CREATE TABLE AS's query, its parameters wanted although folding one of its
// constants raised a data exception at `pos` (docs/frontend_design.md, *The
// dialect*): that constant expression, a select-list item or a GROUP BY,
// WHERE, ON or HAVING operand of `s` itself, replaced by NULL. Whether one
// was; a constant in a set operation's arm or a subquery is not looked for.
[[nodiscard]] auto null_failing_constant(ast::Select& s, std::int32_t pos) -> bool;

// A procedure's body (docs/procedures_design.md, *Compilation pipeline*).
// `frame` is a table that is no catalog's: its columns are the procedure's
// variables, each column's id its slot.
//
// An expression over the variables, as PL/pgSQL evaluates `x := expr`, a
// condition or a FOR bound: each variable becomes parameter $slot, and each
// constant parameter $n from `first_constant` on, its value and type
// appended to `constants`. With `target`, the variable it is assigned to, a
// constant is coerced to its type as an INSERT's is.
struct ProcedureConstants {
  std::vector<Datum> values;
  std::vector<SqlType> types;
};
[[nodiscard]] auto bind_procedure_expr(const std::string& text, const Table& frame, const Catalog& cat,
                                       std::uint32_t first_constant, ProcedureConstants& constants,
                                       const Column* target = nullptr) -> logical::Expr;

// An embedded statement: each variable it names becomes a parameter, $1
// onwards in the order first named; `param_vars` gives each one's slot. A
// name that is both a variable and a column of a table the statement reads
// or writes is 42702, as under plpgsql.variable_conflict = error. A
// variable used where its type is not the parameter's is refused (0A000).
struct ProcedureStatement {
  Bound bound;
  std::vector<std::uint32_t> param_vars;
};
[[nodiscard]] auto bind_procedure_statement(const std::string& sql, const Table& frame, const Catalog& cat)
    -> ProcedureStatement;

// Whether a value of type `from` may be stored as one of `to` without a
// conversion: the same physical type and decimal scale, and both integers,
// both text, or of one kind. A narrower `to` is checked when the value is
// stored (22003, 22001), as PostgreSQL's assignment cast checks it.
[[nodiscard]] auto assignable(SqlType from, SqlType to) -> bool;

// CALL's arguments as the values of `proc`'s arguments, one per argument:
// each a constant expression (a literal, a cast, TO_TIMESTAMP), read as its
// argument's type as an INSERT reads a constant for a column; an OUT
// argument's is NULL, as PostgreSQL ignores it. Throws ast::SqlError: 42883
// for the wrong number of arguments, 42601 for one that is not constant, and
// a value outside its type as an INSERT's would.
[[nodiscard]] auto call_arguments(const ast::Call& call, const Procedure& proc) -> std::vector<Datum>;

// A declared variable's type as PL/pgSQL gives its text (int, numeric(12,2)),
// read as a cast's type is: a qualifier other than pg_catalog, or
// pg_catalog with a grammar spelling (pg_catalog.integer), is refused.
[[nodiscard]] auto declared_type(const std::string& text) -> SqlType;

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
    case PhysType::Num: return "numeric";
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
  if (c.type == "name") return no_mods(SqlKind::Name);
  if (c.type == "timestamp") return no_mods(SqlKind::Timestamp);
  if (c.type == "timestamptz") {
    // Precision 6 is the microseconds the type holds; a smaller one rounds
    // what is stored, which is not built yet.
    if (c.typmods.size() > 1 || (c.typmods.size() == 1 && c.typmods[0] != 6)) {
      throw SqlError{"a precision other than 6 on TIMESTAMP WITH TIME ZONE is not supported yet", c.pos};
    }
    return SqlType{SqlKind::TimestampTz};
  }
  if (c.type == "date") return no_mods(SqlKind::Date);
  if (c.type == "time") return no_mods(SqlKind::Time);
  if (c.type == "timetz") {
    // As TIMESTAMP WITH TIME ZONE: past 6 is 6, as PostgreSQL takes it.
    if (c.typmods.size() > 1 || (c.typmods.size() == 1 && c.typmods[0] < 6)) {
      throw SqlError{"a precision under 6 on TIME WITH TIME ZONE is not supported yet", c.pos, kFeatureNotSupported};
    }
    return SqlType{SqlKind::TimeTz};
  }
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
    // Without one it is a column's type alone (column_type) yet.
    if (c.typmods.empty()) {
      throw SqlError{"NUMERIC without a precision other than as a column's type is not supported yet", c.pos, kFeatureNotSupported};
    }
    if (c.typmods.size() > 2) throw SqlError{"NUMERIC takes a precision and a scale: " + limits, c.pos};
    auto p = c.typmods[0];
    auto sc = c.typmods.size() == 2 ? c.typmods[1] : 0;
    SqlType t{SqlKind::Decimal, 0, field(p), field(sc)};
    if (sc < 0 || !sql_type_valid(t)) {
      throw SqlError{"NUMERIC(" + std::to_string(p) + ", " + std::to_string(sc) + ") is not supported: " + limits, c.pos};
    }
    return t;
  }
  if (c.type == "bpchar") {
    // CHAR and CHARACTER arrive with the length 1; a bare bpchar, which
    // PostgreSQL leaves unbounded, has none.
    if (c.typmods.size() != 1) throw SqlError{"CHAR takes one positive length", c.pos};
    SqlType t{SqlKind::Char, std::in_range<std::uint32_t>(c.typmods[0]) ? static_cast<std::uint32_t>(c.typmods[0]) : 0};
    if (!sql_type_valid(t)) throw SqlError{"length for type char must be at least 1", c.pos};
    return t;
  }
  throw SqlError{"type " + c.type + " is not supported", c.pos};
}

// A number's text as a NUMERIC stores it (numeric_in): its error, at pos,
// the statement's.
auto numeric_bytes(std::string_view text, std::int32_t pos) -> Bytes {
  try {
    auto out = numeric_in(text);
    auto b = std::as_bytes(std::span{out.data(), out.size()});
    return {b.begin(), b.end()};
  } catch (const TextError& e) {
    throw SqlError{e.what(), pos, known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation)};
  }
}

// A column definition's type: sql_type's, and NUMERIC (DEC, DECIMAL)
// without a precision, which only a column is declared as: a value of any
// digits, stored as its text (docs/types_design.md). A cast to it, a
// domain or a procedure argument of it still needs a precision.
auto column_type(const ast::ColumnDefAst& c) -> SqlType {
  if (c.type == "numeric" && c.typmods.empty()) return SqlType{SqlKind::Numeric};
  return sql_type(c);
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
auto fold_default(const ast::Literal& l, SqlType t, std::string_view column) -> Datum {
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
    case SqlKind::Char:
    case SqlKind::Name:
    case SqlKind::Text:
    case SqlKind::Bytea: {
      if (l.kind != K::String) bad();
      auto b = std::as_bytes(std::span{l.text.data(), l.text.size()});
      if (auto why = out_of_range(t, b)) {
        throw SqlError{"DEFAULT is not a " + sql_type_name(t) + ": " + *why, l.pos};
      }
      if (t.kind == SqlKind::Name) b = name_prefix(b);
      return Bytes(b.begin(), b.end());
    }
    case SqlKind::Numeric: {
      // Its numeric_out text, as an INSERT stores it.
      if (l.kind == K::Bool) {
        throw SqlError{"column \"" + std::string{column} +
                           "\" is of type numeric but default expression is of type boolean",
                       l.pos, kDatatypeMismatch};
      }
      if (l.kind != K::Integer && l.kind != K::Number && l.kind != K::String) bad();
      return numeric_bytes(l.text, l.pos);
    }
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::TimeTz:
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
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::TimeTz: return TypeClass::Temporal;
    case SqlKind::Boolean: return TypeClass::Boolean;
    case SqlKind::Text:
    case SqlKind::Varchar:
    case SqlKind::Char:
    case SqlKind::Name: return TypeClass::String;
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
    case SqlKind::Char:
    case SqlKind::Name:
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::TimeTz:
    case SqlKind::Numeric: return false;  // its text: no int64_t arithmetic
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
    case SqlKind::Char: return "character";
    case SqlKind::Decimal:
    case SqlKind::Numeric: return "numeric";
    case SqlKind::Timestamp: return "timestamp without time zone";
    case SqlKind::TimestampTz: return "timestamp with time zone";
    case SqlKind::Time: return "time without time zone";
    case SqlKind::TimeTz: return "time with time zone";
    case SqlKind::SmallInt:
    case SqlKind::Integer:
    case SqlKind::BigInt:
    case SqlKind::Real:
    case SqlKind::Double:
    case SqlKind::Boolean:
    case SqlKind::Text:
    case SqlKind::Name:
    case SqlKind::Bytea:
    case SqlKind::Date: return sql_type_name(t);
  }
  return sql_type_name(t);
}

// A Clock expression as a refusal names it: the clock's family, or the
// session function as written.
auto is_session_name(std::string_view f) -> bool {
  return f == "current_user" || f == "current_role" || f == "user" || f == "session_user" || f == "current_catalog" ||
         f == "current_schema";
}
auto clock_words(const ast::Expr& e) -> std::string {
  if (!is_session_name(e.func)) return "CURRENT_DATE, LOCALTIME and LOCALTIMESTAMP";
  std::string out;
  for (auto ch : e.func) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  return out;
}

// A form the layer does not take yet: 42601, where PostgreSQL runs it.
[[noreturn]] void not_yet(const std::string& what, std::int32_t pos) { throw SqlError{what + " is not supported yet", pos}; }
// A gap PostgreSQL's dialect has and the layer refuses as PostgreSQL refuses
// a feature it lacks (0A000).
[[noreturn]] void not_supported(const std::string& what, std::int32_t pos) {
  throw SqlError{what + " is not supported yet", pos, kFeatureNotSupported};
}

// A Clock expression where the layer does not take it: a session function
// is a gap of PostgreSQL's dialect (0A000), the clock keeps its 42601.
[[noreturn]] void refuse_clock(const ast::Expr& e, const std::string& where) {
  if (is_session_name(e.func)) not_supported(clock_words(e) + " " + where, e.pos);
  not_yet(clock_words(e) + " " + where, e.pos);
}

// A NAME met with another string type where PostgreSQL resolves a union
// (CASE, COALESCE, a set operation): it keeps the first's type, a VARCHAR
// giving way to a NAME. A TEXT first is TEXT, which every string is. A
// NAME made of another string's value would be cut to 63 bytes, which the
// layer does not do in an expression yet: refused (0A000).
void name_union(SqlType first, SqlType next, const std::string& what, std::int32_t pos) {
  if (first.kind == next.kind || (first.kind != SqlKind::Name && next.kind != SqlKind::Name)) return;
  if (first.kind == SqlKind::Text) return;
  not_supported(what + " of " + pg_type_name(first) + " and " + pg_type_name(next) + ", a name made of another string type", pos);
}

// What a NUMERIC of no precision does not do yet: compare, order, group,
// compute, aggregate or cast (0A000). It is stored as its text, and
// byte equality is not numeric equality (1.5 = 1.50).
[[noreturn]] void refuse_numeric(const std::string& what, std::int32_t pos) {
  throw SqlError{what + " (NUMERIC without a precision) is not supported yet", pos, kFeatureNotSupported};
}

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
    case K::Cast:
    case K::IsNull: return is_constant(*e.rhs);
    case K::NullIf: return is_constant(*e.lhs) && is_constant(*e.rhs);
    case K::Coalesce:
    case K::Not:
    case K::And:
    case K::Or:
    // random(lo, hi) draws anew at each evaluation: never folded.
    case K::Func: return e.func != "random" && std::ranges::all_of(e.args, is_constant);
    case K::Case:
      return (!e.lhs || is_constant(*e.lhs)) && (!e.rhs || is_constant(*e.rhs)) && std::ranges::all_of(e.args, is_constant);
    case K::Column:
    case K::Param:
    case K::Aggregate:
    case K::Star:
    case K::Clock:
    case K::Subquery: return false;
  }
  return false;
}

// An integer or numeric constant as a numeric: its scaled value and scale.
struct Scaled {
  std::int64_t v;
  std::uint8_t scale;
};

auto scaled(const Constant& c) -> Scaled { return {std::get<std::int64_t>(c.value), decimal_scale(c.type)}; }

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

// The integer type PostgreSQL's operators give two integers: the wider of
// the two (int2 + int2 is int2, int2 + int4 int4).
auto wider_integer(SqlType x, SqlType y) -> SqlType {
  auto rank = [](SqlKind k) { return k == SqlKind::SmallInt ? 0 : k == SqlKind::Integer ? 1 : 2; };
  return SqlType{rank(x.kind) >= rank(y.kind) ? x.kind : y.kind};
}

// The type of x op y for integers, DECIMALs and DATE - DATE as PostgreSQL's
// operators type it, the one place the layer resolves it, for a constant and
// for columns alike: the wider integer of two integers; DATE - DATE an
// INTEGER count of days; with a numeric, + and - at the finer scale and * at
// the sum of the scales. A numeric quotient's scale depends on the operands'
// values (select_div_scale), so it has no type here: nullopt.
auto arith_type(Arith op, SqlType x, SqlType y) -> std::optional<SqlType> {
  if (is_integer(x) && is_integer(y)) return wider_integer(x, y);
  if (op == Arith::Sub && x.kind == SqlKind::Date && y.kind == SqlKind::Date) return SqlType{SqlKind::Integer};
  switch (op) {
    case Arith::Add:
    case Arith::Sub: return numeric_type(std::max(decimal_scale(x), decimal_scale(y)));
    case Arith::Mul: return numeric_type(static_cast<std::uint8_t>(decimal_scale(x) + decimal_scale(y)));
    case Arith::Div: return std::nullopt;
  }
  return std::nullopt;
}

auto is_temporal(SqlType t) -> bool { return type_class(t.kind) == TypeClass::Temporal; }

// x op y where either is a date or time type, as PostgreSQL 17 has the
// operators over the layer's types, its implicit casts (date to timestamp,
// time to interval) included. Returns for DATE - DATE, the one the layer
// takes; another PostgreSQL has (date +/- integer, a timestamp's or a time's
// difference, time scaled by a number, date + time with time zone, time with
// time zone +/- time, ...) is not supported yet (42601);
// time + time is ambiguous there (42725); any other pair has none (42883).
void temporal_operator(Arith op, SqlType x, SqlType y, std::int32_t pos) {
  auto text = pg_type_name(x) + " " + std::string{arith_text(op)} + " " + pg_type_name(y);
  auto date = [](SqlType t) { return t.kind == SqlKind::Date; };
  auto time = [](SqlType t) { return t.kind == SqlKind::Time; };
  auto timetz = [](SqlType t) { return t.kind == SqlKind::TimeTz; };
  auto stamp = [](SqlType t) { return t.kind == SqlKind::Timestamp || t.kind == SqlKind::TimestampTz; };
  auto int4 = [](SqlType t) { return t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer; };  // a bigint does not narrow
  auto number = [](SqlType t) { return type_class(t.kind) == TypeClass::Number; };
  auto exists = false;
  switch (op) {
    case Arith::Add:
      if (time(x) && time(y)) throw SqlError{"operator is not unique: " + text, pos, kAmbiguousFunction};
      exists = (date(x) && (int4(y) || time(y) || timetz(y))) || (date(y) && (int4(x) || time(x) || timetz(x))) ||
               (stamp(x) && time(y)) || (time(x) && stamp(y)) || (timetz(x) && time(y)) || (time(x) && timetz(y));
      break;
    case Arith::Sub:
      if (date(x) && date(y)) return;
      exists = (stamp(x) && (stamp(y) || date(y) || time(y))) || (date(x) && (int4(y) || stamp(y) || time(y))) ||
               (time(x) && time(y)) || (timetz(x) && time(y));
      break;
    case Arith::Mul: exists = (time(x) && number(y)) || (number(x) && time(y)); break;
    case Arith::Div: exists = time(x) && number(y); break;
  }
  if (exists) not_yet(text, pos);
  throw SqlError{"operator does not exist: " + text, pos, kUndefinedFunction};
}

// t op unknown (or unknown op t when untyped_left), where t is a date or time
// type and the other operand a $n, an unknown literal or NULL, as PostgreSQL
// 17 resolves it. Returns for -, where every such type has T - T and the
// unknown is read as t, but for a time with time zone, which has no T - T:
// its - reads the unknown as an interval. Otherwise the operator PostgreSQL picks is not supported yet
// (42601: time + interval, time scaled by a number, timestamp + interval,
// time with time zone - interval), or it has none it can pick: date or time
// with time zone + unknown is ambiguous (42725), and date, timestamp or time
// with time zone * or /, and unknown / time, have none (42883).
void temporal_untyped_operator(Arith op, SqlType t, bool untyped_left, std::int32_t pos) {
  auto text = untyped_left ? "unknown " + std::string{arith_text(op)} + " " + pg_type_name(t)
                           : pg_type_name(t) + " " + std::string{arith_text(op)} + " unknown";
  auto date = t.kind == SqlKind::Date;
  auto time = t.kind == SqlKind::Time;
  auto timetz = t.kind == SqlKind::TimeTz;
  auto exists = false;
  switch (op) {
    case Arith::Sub:
      if (!timetz) return;
      exists = !untyped_left;  // time with time zone - interval; unknown - time with time zone has none
      break;
    case Arith::Add:
      if (date || timetz) throw SqlError{"operator is not unique: " + text, pos, kAmbiguousFunction};
      exists = true;
      break;
    case Arith::Mul: exists = time; break;
    case Arith::Div: exists = time && !untyped_left; break;
  }
  if (exists) not_yet(text, pos);
  throw SqlError{"operator does not exist: " + text, pos, kUndefinedFunction};
}

// PostgreSQL's select_div_scale: the scale of the quotient of two numerics,
// from each operand's leading base-10000 digit (its value there and its
// weight, in groups of four digits from the point) and their scales. At
// least 16 significant digits, and at least the finer operand's scale.
auto div_scale(Scaled x, Scaled y) -> int {
  struct Leading {
    int weight;
    int first;
  };
  auto leading = [](Scaled v) -> Leading {
    if (v.v == 0) return {0, 0};
    auto magnitude = v.v < 0 ? -v.v : v.v;
    auto digits = 0;
    for (auto m = magnitude; m > 0; m /= 10) ++digits;
    auto top = digits - 1 - static_cast<int>(v.scale);  // the power of ten of its first digit
    auto weight = top >= 0 ? top / 4 : -((-top + 3) / 4);
    auto shift = static_cast<int>(v.scale) + 4 * weight;  // in [-3, 17]
    auto first = shift >= 0 ? magnitude / kPowersOfTen[static_cast<std::size_t>(shift)]
                            : magnitude * kPowersOfTen[static_cast<std::size_t>(-shift)];
    return {weight, static_cast<int>(first)};
  };
  auto a = leading(x);
  auto b = leading(y);
  auto qweight = a.weight - b.weight;
  if (a.first <= b.first) --qweight;
  constexpr auto kMinSignificantDigits = 16;
  constexpr auto kMaxDisplayScale = 1000;
  auto scale = kMinSignificantDigits - qweight * 4;
  scale = std::max({scale, static_cast<int>(x.scale), static_cast<int>(y.scale), 0});
  return std::min(scale, kMaxDisplayScale);
}

// x / y at scale `scale`, rounded half away from zero as PostgreSQL's
// division rounds; nullopt past 18 digits.
auto numeric_quotient(Scaled x, Scaled y, int scale) -> std::optional<std::int64_t> {
  if (x.v == 0) return 0;
  auto digits = [](std::int64_t v) {
    auto n = 0;
    for (auto m = v < 0 ? -v : v; m > 0; m /= 10) ++n;
    return n;
  };
  auto up = scale + static_cast<int>(y.scale) - static_cast<int>(x.scale);  // x.v * 10^up / y.v
  if (digits(x.v) + up - digits(y.v) >= static_cast<int>(kMaxDecimalPrecision) + 1) return std::nullopt;
  __int128 num = x.v < 0 ? -static_cast<__int128>(x.v) : x.v;
  for (auto i = 0; i < up; ++i) num *= 10;
  __int128 den = y.v < 0 ? -static_cast<__int128>(y.v) : y.v;
  auto q = (2 * num + den) / (2 * den);
  if (q >= kDecimalLimit) return std::nullopt;
  auto negative = (x.v < 0) != (y.v < 0);
  return static_cast<std::int64_t>(negative ? -q : q);
}

auto resolved(const Constant& k, SqlType t) -> Constant;

// x op y, of integer or numeric constants, as PostgreSQL's operators type
// it (arith_type); the range checked as there (22003). Integer / truncates
// toward zero; a numeric quotient has PostgreSQL's own scale (div_scale),
// within 18 digits. A zero divisor is 22012, a NULL operand NULL (a divisor
// of NULL first: strict operators do not run). NULL beside a number is NULL
// of the number's type. Of a date or time type only DATE - DATE is taken
// (temporal_operator), an unknown literal or NULL beside the DATE read as one,
// as PostgreSQL resolves it.
auto arithmetic(const Constant& x, const Constant& y, Arith op, std::int32_t pos) -> Constant {
  auto text = std::string{arith_text(op)};
  auto null_untyped = [](const Constant& c) { return c.unknown && std::holds_alternative<std::monostate>(c.value); };
  if (null_untyped(x) && null_untyped(y)) not_yet("NULL " + text + " NULL", pos);
  if ((!x.unknown && is_temporal(x.type)) || (!y.unknown && is_temporal(y.type))) {
    if (x.unknown || y.unknown) {
      temporal_untyped_operator(op, x.unknown ? y.type : x.type, x.unknown, pos);  // returns for - only
      return arithmetic(x.unknown ? resolved(x, y.type) : x, y.unknown ? resolved(y, x.type) : y, op, pos);
    }
    temporal_operator(op, x.type, y.type, pos);  // returns for DATE - DATE only
  }
  for (const auto* c : {&x, &y}) {
    if (c->unknown && !null_untyped(*c)) not_yet("a string literal in arithmetic", c->pos);
    if (!c->unknown && !is_integer(c->type) && c->type.kind != SqlKind::Decimal && !is_temporal(c->type)) {
      throw SqlError{"operator does not exist: " + pg_type_name(x.type) + " " + text + " " + pg_type_name(y.type), pos,
                     kUndefinedFunction};
    }
  }
  auto both_integer = !null_untyped(x) && !null_untyped(y) && is_integer(x.type) && is_integer(y.type);
  auto is_null = [](const Constant& c) { return std::holds_alternative<std::monostate>(c.value); };
  auto is_zero = [&](const Constant& c) { return !is_null(c) && std::get<std::int64_t>(c.value) == 0; };
  if (op == Arith::Div && !is_null(x) && is_zero(y)) throw SqlError{"division by zero", pos, kDivisionByZero};
  auto numeric_quotient_of = op == Arith::Div && !null_untyped(x) && !null_untyped(y) && !both_integer;
  auto too_wide = [&] { not_yet("a numeric of more than 18 digits", pos); };
  // The quotient of two numerics is typed by the values it divides, so a
  // NULL operand leaves it a numeric of no scale.
  auto type = [&] {
    if (null_untyped(x)) return y.type;
    if (null_untyped(y)) return x.type;
    if (numeric_quotient_of) {
      if (is_null(x) || is_null(y)) return numeric_type(0);
      auto scale = div_scale(scaled(x), scaled(y));
      if (scale > static_cast<int>(kMaxDecimalPrecision)) too_wide();
      return numeric_type(static_cast<std::uint8_t>(scale));
    }
    return *arith_type(op, x.type, y.type);
  }();
  if (is_null(x) || is_null(y)) return {std::monostate{}, type, false, {}, pos};
  auto overflow_error = [&] {
    return SqlError{pg_type_name(type) + " out of range", pos, kNumericValueOutOfRange};
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
    if (overflow || out_of_range(type, out)) throw overflow_error();
    return {out, type, false, {}, pos};
  }
  if (op == Arith::Div) {
    auto q = numeric_quotient(scaled(x), scaled(y), type.scale);
    if (!q) too_wide();
    return {*q, type, false, {}, pos};
  }
  if (op == Arith::Mul) {
    // The product's scale is the sum of the scales: no rescaling, only the 18 digits.
    if (type.scale > kMaxDecimalPrecision || __builtin_mul_overflow(scaled(x).v, scaled(y).v, &out) || out >= kDecimalLimit ||
        out <= -kDecimalLimit) {
      too_wide();
    }
    return {out, type, false, {}, pos};
  }
  auto a = rescale(scaled(x).v, static_cast<std::uint8_t>(type.scale - scaled(x).scale));
  auto b = rescale(scaled(y).v, static_cast<std::uint8_t>(type.scale - scaled(y).scale));
  if (!a || !b) too_wide();
  out = op == Arith::Sub ? *a - *b : *a + *b;  // each below 10^18: no overflow
  if (out >= kDecimalLimit || out <= -kDecimalLimit) too_wide();
  return {out, type, false, {}, pos};
}

// A range check's SQLSTATE.
auto range_state(const std::string& why) -> SqlState {
  return known_sqlstate(range_sqlstate(why)).value_or(kNumericValueOutOfRange);
}

// A range check's message as a SqlError, with its SQLSTATE.
[[noreturn]] void out_of_type(const std::string& why, std::int32_t pos) { throw SqlError{why, pos, range_state(why)}; }

auto is_number(SqlType t) -> bool { return is_integer(t) || t.kind == SqlKind::Decimal; }
auto is_float(SqlType t) -> bool { return t.kind == SqlKind::Real || t.kind == SqlKind::Double; }
// A NAME is one too: PostgreSQL's operators and functions take it as text.
auto is_string(SqlType t) -> bool { return t.kind == SqlKind::Text || t.kind == SqlKind::Varchar || t.kind == SqlKind::Name; }
// A string a column of this type takes: CHAR(n) is not is_string, whose LIKE,
// UNION and parameter rules are TEXT's and VARCHAR's.
auto holds_string(SqlType t) -> bool { return is_string(t) || t.kind == SqlKind::Char; }

auto bytes_of(std::string_view t) -> Bytes {
  auto b = std::as_bytes(std::span{t.data(), t.size()});
  return {b.begin(), b.end()};
}

// An input function's error reading unknown literal k: raised as
// PostgreSQL analyses the statement (SqlError::at_analysis), before it
// plans it.
[[noreturn]] void literal_input_error(const Constant& k, const std::string& why, SqlState state) {
  SqlError err{why, k.pos, state};
  err.mark_at_analysis();
  throw err;
}

// Text read as a value of type t, as t's input function reads it, the
// range checked: what an unknown literal is where it meets t. `assigned`
// rounds a DECIMAL finer than its scale, as a cast or an assignment does.
// Of an unknown literal, an error is the analysis's; of a typed string
// (k.unknown false), the cast's, raised as the constant is folded.
auto read_as(const Constant& k, SqlType t, bool assigned) -> Datum {
  Datum d;
  try {
    d = from_text(k.text, t, assigned);
  } catch (const TextError& e) {
    auto state = known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation);
    if (k.unknown) literal_input_error(k, e.what(), state);
    throw SqlError{e.what(), k.pos, state};
  }
  if (auto why = out_of_range(t, d)) {
    if (k.unknown) literal_input_error(k, *why, range_state(*why));
    out_of_type(*why, k.pos);
  }
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
      literal_input_error(k, "invalid input syntax for type numeric: \"" + k.text + "\"", kInvalidTextRepresentation);
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

constexpr std::int64_t kMicrosPerDay = 86'400'000'000;

// A temporal constant as a value of `to`, where the conversion loses
// nothing (the session time zone is UTC, so a timestamp and a timestamptz
// hold the same microseconds): the same kind; a date to a timestamp or a
// timestamptz; a timestamp to a timestamptz and back; a time to a time with
// time zone, at offset 0. nullopt otherwise.
auto exact_temporal(const Constant& k, SqlKind to) -> std::optional<Datum> {
  using K = SqlKind;
  if (std::holds_alternative<std::monostate>(k.value)) return std::nullopt;
  auto from = k.type.kind;
  if (from == to) return k.value;
  auto instant = [](K x) { return x == K::Timestamp || x == K::TimestampTz; };
  if (instant(from) && instant(to)) return k.value;
  if (from == K::Time && to == K::TimeTz) return timetz_pack({std::get<std::int64_t>(k.value), 0});
  if (from == K::Date && instant(to)) {
    auto days = std::get<std::int64_t>(k.value);
    constexpr auto kLimit = std::numeric_limits<std::int64_t>::max() / kMicrosPerDay;
    if (days > kLimit || days < -kLimit) return std::nullopt;
    return days * kMicrosPerDay;
  }
  return std::nullopt;
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
    // PostgreSQL compares a date or a timestamp with a timestamptz as
    // timestamptz, and a time with a time with time zone as the latter; a
    // time with time zone meets no other type.
    auto tz = [](SqlType t) { return t.kind == SqlKind::TimeTz; };
    auto time = [](SqlType t) { return t.kind == SqlKind::Time || t.kind == SqlKind::TimeTz; };
    if ((tz(x.type) || tz(y.type)) && !(time(x.type) && time(y.type))) refuse();
    auto kind = x.type.kind == SqlKind::TimestampTz || y.type.kind == SqlKind::TimestampTz ? SqlKind::TimestampTz
                : tz(x.type) || tz(y.type)                                                 ? SqlKind::TimeTz
                                                                                           : x.type.kind;
    auto a = exact_temporal(x, kind);
    auto b = exact_temporal(y, kind);
    if (!a || !b) not_yet("comparing a " + pg_type_name(x.type) + " with a " + pg_type_name(y.type), pos);
    return sign(std::get<std::int64_t>(*a), std::get<std::int64_t>(*b));
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

auto text_of(const Constant& c) -> std::string;

// A date, time or timestamp cast to another type, as PostgreSQL casts it:
// to text; to its own type; a timestamp or timestamptz to a date or a time
// (read in UTC); a date to a timestamp or timestamptz; a timestamp to a
// timestamptz and back; a time with time zone to a time (its offset
// dropped); a time, or a timestamptz's time in UTC, to a time with time
// zone at offset 0. A time to a date, a timestamp or a timestamptz, a date
// to a time, and a time with time zone to or from any other, have no cast
// (42846).
auto cast_temporal(const Constant& arg, SqlType t, std::int32_t pos) -> Constant {
  auto from = arg.type.kind;
  auto null = std::holds_alternative<std::monostate>(arg.value);
  auto result = [&](Datum v) -> Constant { return {std::move(v), t, false, {}, pos}; };
  if (t.kind == SqlKind::Text) return null ? result(std::monostate{}) : result(bytes_of(text_of(arg)));
  if (from == t.kind) return {arg.value, t, false, {}, pos};
  using K = SqlKind;
  if ((from == K::Timestamp || from == K::TimestampTz) && (t.kind == K::Date || t.kind == K::Time)) {
    if (null) return result(std::monostate{});
    auto us = std::get<std::int64_t>(arg.value);
    auto days = us >= 0 ? us / kMicrosPerDay : -((-us + kMicrosPerDay - 1) / kMicrosPerDay);
    return result(t.kind == K::Date ? days : us - days * kMicrosPerDay);
  }
  if (from == K::TimestampTz && t.kind == K::TimeTz) {
    if (null) return result(std::monostate{});
    auto us = std::get<std::int64_t>(arg.value);
    auto days = us >= 0 ? us / kMicrosPerDay : -((-us + kMicrosPerDay - 1) / kMicrosPerDay);
    return result(timetz_pack({us - days * kMicrosPerDay, 0}));
  }
  if (from == K::TimeTz && t.kind == K::Time) {
    if (null) return result(std::monostate{});
    return result(timetz_unpack(std::get<std::int64_t>(arg.value)).time);
  }
  if (from == K::Time && t.kind == K::TimeTz) {
    if (null) return result(std::monostate{});
    return result(*exact_temporal(arg, t.kind));
  }
  if (from == K::Date && (t.kind == K::Timestamp || t.kind == K::TimestampTz)) {
    if (null) return result(std::monostate{});
    auto v = exact_temporal(arg, t.kind);
    if (!v) throw SqlError{"timestamp out of range", pos, kDatetimeFieldOverflow};
    return result(*v);
  }
  if ((from == K::Timestamp && t.kind == K::TimestampTz) || (from == K::TimestampTz && t.kind == K::Timestamp)) {
    return {arg.value, t, false, {}, pos};
  }
  throw SqlError{"cannot cast type " + pg_type_name(arg.type) + " to " + pg_type_name(t), pos, kCannotCoerce};
}

// A number cast to an integer type, as PostgreSQL's cast functions do it: a
// numeric rounded half away from zero, a float to the nearest even (rint);
// out of the type's range, or a float that is NaN or infinite, is 22003.
auto integer_of(const Constant& arg, SqlType t, std::int32_t pos) -> std::int64_t {
  auto out_of_range_error = [&] {
    return SqlError{pg_type_name(t) + " out of range", pos, kNumericValueOutOfRange};
  };
  std::int64_t v = 0;
  if (is_integer(arg.type)) {
    v = std::get<std::int64_t>(arg.value);
  } else if (arg.type.kind == SqlKind::Decimal) {
    auto scaled = std::get<std::int64_t>(arg.value);
    auto unit = kPowersOfTen[arg.type.scale];
    auto whole = scaled / unit;
    auto rest = scaled % unit;
    if (rest != 0 && 2 * (rest < 0 ? -rest : rest) >= unit) whole += scaled < 0 ? -1 : 1;
    v = whole;
  } else {
    auto d = std::rint(std::get<double>(arg.value));
    // 2^63 is the first double past int64_t; -2^63 is in range.
    if (!std::isfinite(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0) throw out_of_range_error();
    v = static_cast<std::int64_t>(d);
  }
  if (out_of_range(t, v)) throw out_of_range_error();
  return v;
}

// A typed constant cast to t, as PostgreSQL casts it: a number to any number
// type by pg_cast's functions; a BOOLEAN to an INTEGER and back; any type to
// TEXT by its output function (a float, whose text differs, is not taken
// yet); a string to any type by that type's input function. Every other pair
// has no cast (42846), a NULL of it included.
auto cast_typed(const Constant& arg, SqlType t, std::int32_t pos) -> Constant {
  using K = SqlKind;
  auto from = type_class(arg.type.kind);
  auto to = type_class(t.kind);
  auto exists = from == TypeClass::String || to == TypeClass::String || arg.type.kind == t.kind ||
                (from == TypeClass::Number && to == TypeClass::Number) ||
                (arg.type.kind == K::Boolean && t.kind == K::Integer) || (arg.type.kind == K::Integer && t.kind == K::Boolean);
  if (!exists) throw SqlError{"cannot cast type " + pg_type_name(arg.type) + " to " + pg_type_name(t), pos, kCannotCoerce};
  if (to == TypeClass::String && is_float(arg.type)) not_yet("a cast of a floating-point value to text", pos);
  auto result = [&](Datum v) -> Constant { return {std::move(v), t, false, {}, pos}; };
  if (std::holds_alternative<std::monostate>(arg.value)) return result(std::monostate{});
  if (to == TypeClass::String) {
    auto b = bytes_of(text_of(arg));
    if (t.kind == K::Name) b.resize(name_prefix(b).size());  // namein's cut, from any typed string
    return result(std::move(b));
  }
  if (from == TypeClass::String) {
    const auto& b = std::get<Bytes>(arg.value);
    Constant text{std::monostate{}, SqlType{K::Text}, false, std::string(b.size(), '\0'), pos};  // typed: the cast's error
    std::ranges::transform(b, text.text.begin(), [](std::byte x) { return static_cast<char>(x); });
    return result(read_as(text, t, true));
  }
  if (arg.type.kind == K::Boolean && t.kind == K::Integer) return result(std::int64_t{std::get<bool>(arg.value) ? 1 : 0});
  if (arg.type.kind == K::Integer && t.kind == K::Boolean) return result(std::get<std::int64_t>(arg.value) != 0);
  if (from != TypeClass::Number) return result(arg.value);  // a BOOLEAN or a BYTEA to its own type
  if (is_integer(t)) return result(integer_of(arg, t, pos));
  if (is_float(t)) {
    auto d = is_float(arg.type) ? std::get<double>(arg.value) : std::stod(text_of(arg));
    if (t.kind == K::Real && std::isfinite(d) && (std::fabs(d) > std::numeric_limits<float>::max() ||
                                                  (d != 0 && std::fabs(d) < std::numeric_limits<float>::denorm_min()))) {
      throw SqlError{std::string{"value out of range: "} + (std::fabs(d) > 1 ? "overflow" : "underflow"), pos,
                     kNumericValueOutOfRange};
    }
    return result(t.kind == K::Real ? static_cast<double>(static_cast<float>(d)) : d);
  }
  // To a DECIMAL: through its text, as the input function reads it, rounded
  // to the scale; a float at its type's digits, as float8_numeric does.
  if (is_float(arg.type) && !std::isfinite(std::get<double>(arg.value))) not_yet("NaN or Infinity as a numeric", pos);
  Constant text{std::monostate{}, SqlType{K::Text}, false, text_of(arg), pos};  // typed: the cast's error
  return result(read_as(text, t, true));
}

// type 'text', '…'::type and CAST(… AS type): a string literal read as the
// type, as its input function reads it, a DECIMAL rounded to its scale;
// NULL of the type; a typed constant cast by cast_typed, or by cast_temporal
// for a date, time or timestamp. A cast to VARCHAR(n) or CHAR(n), which
// PostgreSQL's cast truncates or pads, is not taken yet.
auto cast_constant(const ast::Expr& e, const Constant& arg) -> Constant {
  auto t = sql_type(ast::ColumnDefAst{"", e.cast_type, e.cast_typmods, false, false, std::nullopt, e.pos, {}});
  if (t.kind == SqlKind::Varchar) not_yet("a cast to VARCHAR(n)", e.pos);
  if (t.kind == SqlKind::Char) not_yet("a cast to CHAR(n)", e.pos);
  auto temporal = !arg.unknown && type_class(arg.type.kind) == TypeClass::Temporal;
  if (temporal) return cast_temporal(arg, t, e.pos);
  if (!arg.unknown) return cast_typed(arg, t, e.pos);
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

// NOT, AND or OR of constants, in SQL's three-valued logic: AND is false
// where an operand is, else NULL where one is, else true; OR the mirror. An
// operand must be a BOOLEAN or an unknown literal (42804). A runtime error
// of an operand is raised only if the result depends on it, as for a
// conditional.
auto fold_boolean(const ast::Expr& e) -> Constant {
  using K = ast::Expr::Kind;
  auto word = e.kind == K::Not ? "NOT" : e.kind == K::And ? "AND" : "OR";
  std::vector<std::optional<bool>> values;  // nullopt: NULL
  std::optional<SqlError> failed;
  for (const auto& a : e.args) {
    auto b = fold_branch(a);
    if (b.error) {
      failed = b.error;
      continue;
    }
    auto c = *b.value;
    if (c.unknown) c = resolved(c, SqlType{SqlKind::Boolean});
    if (c.type.kind != SqlKind::Boolean) {
      throw SqlError{std::string{"argument of "} + word + " must be type boolean, not type " + pg_type_name(c.type), a.pos,
                     kDatatypeMismatch};
    }
    values.push_back(std::holds_alternative<std::monostate>(c.value) ? std::nullopt : std::optional{std::get<bool>(c.value)});
  }
  auto boolean = [&](std::optional<bool> v) -> Constant {
    return {v ? Datum{*v} : Datum{std::monostate{}}, SqlType{SqlKind::Boolean}, false, {}, e.pos};
  };
  if (e.kind == K::Not) {
    if (failed) throw *failed;
    return boolean(values[0] ? std::optional{!*values[0]} : std::nullopt);
  }
  auto decisive = e.kind == K::Or;  // the value that settles the result
  if (std::ranges::any_of(values, [&](const auto& v) { return v == decisive; })) return boolean(decisive);
  if (failed) throw *failed;
  if (std::ranges::any_of(values, [](const auto& v) { return !v; })) return boolean(std::nullopt);
  return boolean(!decisive);
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
    } else if (type.kind != c->type.kind && is_string(type) && is_string(c->type)) {
      name_union(first->type, c->type, what, pos);
      type = SqlType{SqlKind::Text};
    } else if (type.kind != c->type.kind) {
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

// String functions and ||, over the text module's character helpers.

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
// x % y of constants, as PostgreSQL's int and numeric mod: the remainder of
// the quotient truncated toward zero, of x's sign; integers of the wider
// type, numerics at the finer scale. A zero divisor is 22012; a float has no
// % (42883). An unknown literal takes the other side's type.
auto fold_mod(Constant x, Constant y, std::int32_t pos) -> Constant {
  if (x.unknown && !y.unknown) x = resolved(x, y.type);
  if (y.unknown && !x.unknown) y = resolved(y, x.type);
  if (x.unknown && y.unknown) not_yet("% of two string literals or NULLs", pos);
  if (!is_number(x.type) || !is_number(y.type)) {
    throw SqlError{"operator does not exist: " + pg_type_name(x.type) + " % " + pg_type_name(y.type), pos, kUndefinedFunction};
  }
  auto type = is_integer(x.type) && is_integer(y.type) ? wider_integer(x.type, y.type)
                                                        : numeric_type(std::max(decimal_scale(x.type), decimal_scale(y.type)));
  if (is_null(x) || is_null(y)) return {std::monostate{}, type, false, {}, pos};
  auto a = rescale(scaled(x).v, static_cast<std::uint8_t>(decimal_scale(type) - scaled(x).scale));
  auto b = rescale(scaled(y).v, static_cast<std::uint8_t>(decimal_scale(type) - scaled(y).scale));
  if (!a || !b) not_yet("a numeric of more than 18 digits", pos);
  if (*b == 0) throw SqlError{"division by zero", pos, kDivisionByZero};
  return {*b == -1 ? std::int64_t{0} : *a % *b, type, false, {}, pos};
}

// abs(x) of a constant, as PostgreSQL's: of x's type, an integer's range
// checked ("smallint out of range" for -32768); an unknown literal read as a
// double precision, the type PostgreSQL resolves it to.
auto fold_abs(Constant x, std::int32_t pos) -> Constant {
  if (x.unknown) x = resolved(x, SqlType{SqlKind::Double});
  if (!is_number(x.type) && !is_float(x.type)) {
    throw SqlError{"function abs(" + pg_type_name(x.type) + ") does not exist", pos, kUndefinedFunction};
  }
  x.pos = pos;
  if (is_null(x)) return x;
  if (is_float(x.type)) {
    auto d = std::get<double>(x.value);
    x.value = d <= 0 ? 0.0 - d : d;  // -0.0 is 0, as fabs gives it
    return x;
  }
  auto v = std::get<std::int64_t>(x.value);
  if (v < 0 && (v == std::numeric_limits<std::int64_t>::min() || (is_integer(x.type) && out_of_range(x.type, -v)))) {
    throw SqlError{pg_type_name(x.type) + " out of range", pos, kNumericValueOutOfRange};
  }
  x.value = v < 0 ? -v : v;
  return x;
}

// EXTRACT(unit FROM x), which PostgreSQL's grammar spells
// pg_catalog.extract('unit', x). Resolved as PostgreSQL resolves the call:
// x of no type (an unknown literal, NULL, a bare $n) fits several of its
// forms (42725), and a type other than a date or time none (42883). The
// unit is a string constant.
auto extract_unit_text(const Constant& unit, std::int32_t pos) -> std::string {
  if (is_null(unit) || !(unit.unknown || is_string(unit.type))) not_yet("EXTRACT of a unit other than a name or a string", pos);
  return ascii_case(text_of(unit), false);
}

void extract_source(const Constant& unit, std::optional<SqlType> source, std::int32_t pos) {
  auto first = unit.unknown ? std::string{"unknown"} : pg_type_name(unit.type);
  if (!source) {
    throw SqlError{"function pg_catalog.extract(" + first + ", unknown) is not unique", pos, kAmbiguousFunction};
  }
  auto k = source->kind;
  if (k == SqlKind::TimeTz) not_supported("EXTRACT of a time with time zone", pos);
  if (k != SqlKind::Date && k != SqlKind::Time && k != SqlKind::Timestamp && k != SqlKind::TimestampTz) {
    throw SqlError{"function pg_catalog.extract(" + first + ", " + pg_type_name(*source) + ") does not exist", pos, kUndefinedFunction};
  }
}

// The field a unit names for a value of `source`, and EXTRACT's numeric of
// it: a unit no type has is 22023, one `source` lacks 0A000, as
// PostgreSQL's date_part reports them. EPOCH and JULIAN of a timestamp,
// whose values pass the 18 digits of the layer's numeric or are fractions
// of a day, are not built.
auto extract_field_type(const std::string& unit, SqlType source, std::int32_t pos) -> std::pair<DateField, SqlType> {
  auto u = date_unit(unit);
  if (u.kind == DateUnit::Kind::Unknown) {
    throw SqlError{"unit \"" + unit + "\" not recognized for type " + pg_type_name(source), pos, kInvalidParameterValue};
  }
  if (u.kind == DateUnit::Kind::Reserved || !date_field_of(u.field, source.kind)) {
    throw SqlError{"unit \"" + unit + "\" not supported for type " + pg_type_name(source), pos, kFeatureNotSupported};
  }
  if (!date_field_built(u.field, source.kind)) {
    not_yet(u.field == DateField::Epoch ? "EXTRACT(EPOCH FROM a timestamp)" : "EXTRACT(JULIAN FROM a timestamp)", pos);
  }
  return {u.field, numeric_type(date_field_scale(u.field, source.kind))};
}

auto fold_extract(const Constant& unit, const Constant& x, std::int32_t pos) -> Constant {
  auto name = extract_unit_text(unit, pos);
  extract_source(unit, x.unknown ? std::nullopt : std::optional{x.type}, pos);
  if (is_null(x)) return {std::monostate{}, numeric_type(0), false, {}, pos};  // strict: the unit is not read
  auto [field, type] = extract_field_type(name, x.type, pos);
  return {extract_field(field, x.type.kind, std::get<std::int64_t>(x.value)), type, false, {}, pos};
}

auto fold_function(const ast::Expr& e) -> Constant {
  std::vector<Constant> given;
  for (const auto& a : e.args) given.push_back(fold(a));
  // substr is substring's function-call form, its arguments in the same order.
  const std::string name = e.func == "substr" ? "substring" : e.func;
  if (name == "||") return fold_concat(given[0], given[1], e.pos);
  if (name == "%") return fold_mod(given[0], given[1], e.pos);
  if (name == "extract") {
    if (given.size() != 2) no_function("pg_catalog.extract", given, e.pos);
    return fold_extract(given[0], given[1], e.pos);
  }
  if (name == "abs") {
    if (given.size() != 1) no_function(name, given, e.pos);
    return fold_abs(given[0], e.pos);
  }
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
  if (name == "to_timestamp") {
    // The form HammerDB's driver sends, TO_TIMESTAMP(text, 'YYYYMMDDHH24MISS'),
    // in UTC, the layer's one time zone (docs/types_design.md).
    arity(2, 2);
    auto args = bind_args(name, given, std::vector{Slot::Text, Slot::Text}, false, e.pos);
    if (auto n = strict(args, SqlKind::TimestampTz)) return *n;
    if (text_of(args[1]) != "YYYYMMDDHH24MISS") not_yet("TO_TIMESTAMP of another format than 'YYYYMMDDHH24MISS'", e.pos);
    auto t = text_of(args[0]);
    if (t.size() != 14 || !std::ranges::all_of(t, [](char ch) { return ch >= '0' && ch <= '9'; })) {
      throw SqlError{"invalid value \"" + t + "\" for \"YYYYMMDDHH24MISS\"", e.pos, kInvalidDatetimeFormat};
    }
    auto iso = t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2) + " " + t.substr(8, 2) + ":" + t.substr(10, 2) +
               ":" + t.substr(12, 2);
    auto type = SqlType{SqlKind::TimestampTz};
    try {
      return {from_text(iso, type, false), type, false, {}, e.pos};
    } catch (const TextError& x) {
      throw SqlError{x.what(), e.pos, known_sqlstate(x.sqlstate()).value_or(kDatetimeFieldOverflow)};
    }
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
      if (v == std::numeric_limits<std::int64_t>::min() || (is_integer(c.type) && out_of_range(c.type, -v))) {
        throw SqlError{pg_type_name(c.type) + " out of range", e.pos, kNumericValueOutOfRange};
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
    case K::IsNull: {
      auto null = std::holds_alternative<std::monostate>(fold(*e.rhs).value);
      return {null != e.negated, SqlType{SqlKind::Boolean}, false, {}, e.pos};
    }
    case K::Not:
    case K::And:
    case K::Or: return fold_boolean(e);
    case K::Cast: return cast_constant(e, fold(*e.rhs));
    case K::Case: return fold_case(e);
    case K::Coalesce: return fold_coalesce(e);
    case K::NullIf: return fold_nullif(e);
    case K::Func: return fold_function(e);
    case K::Clock: refuse_clock(e, "other than as a select-list value");
    case K::Column:
    case K::Param:
    case K::Aggregate:
    case K::Star:
    case K::Subquery: break;
  }
  throw SqlError{"expression not supported", e.pos};
}

auto temporal_cast(SqlKind from, SqlKind to) -> bool;

// The constant as a value of column c's type: assigned to it (INSERT's
// VALUES, SET), with PostgreSQL's assignment casts, which round a numeric
// half away from zero and write a number or a boolean as its text; or
// compared with it by `op`. A compared constant is taken only where c's
// type holds it exactly, and refused otherwise (42601): PostgreSQL then
// compares in a wider type, which the layer's keys don't
// (docs/frontend_design.md, *Known differences*). A string literal or NULL
// is read as c's type, as PostgreSQL reads an unknown literal. The range of
// an assigned value is its caller's to check.
auto coerce_value(const Constant& k, const Column& c, bool assigned, std::string_view op) -> Datum {
  auto inexact = [&] [[noreturn]] {
    not_yet("comparing " + c.name + " (" + sql_type_name(c.sql) + ") with a constant its type does not hold exactly", k.pos);
  };
  // A NUMERIC is its text: 1.5 = 1.50 is not a byte equality, so it is not
  // compared with a number yet. A string literal is read first, so text that
  // is no number is 22P02 as in PostgreSQL; a boolean, temporal or string
  // constant falls through to PostgreSQL's 42883 (no such operator).
  if (c.sql.kind == SqlKind::Numeric && !assigned) {
    if (k.unknown && !std::holds_alternative<std::monostate>(k.value)) {
      try {
        (void)from_text(k.text, c.sql, true);
      } catch (const TextError& e) {
        literal_input_error(k, e.what(), known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation));
      }
    }
    if (k.unknown || type_class(k.type.kind) == TypeClass::Number) {
      refuse_numeric("a NUMERIC column compared or computed with a constant", k.pos);
    }
  }
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
          literal_input_error(k, e.what(), known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation));
        }
        inexact();
      }
      literal_input_error(k, e.what(), known_sqlstate(e.sqlstate()).value_or(kInvalidTextRepresentation));
    }
    if (!assigned) {
      if (auto why = out_of_range(compared_type(c.sql).range, d)) literal_input_error(k, *why, range_state(*why));
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
  if (c.sql.kind == SqlKind::Numeric) {
    // An integer or numeric constant: its text, the scale it is written
    // with kept, as numeric_out writes it. A float's is a cast, later.
    if (!is_number(k.type)) refuse_numeric("a " + pg_type_name(k.type) + " constant assigned to a NUMERIC column", k.pos);
    auto [v, scale] = scaled(k);
    return bytes_of(to_text(Datum{v}, numeric_type(scale)));
  }
  if (k.type.kind == SqlKind::Boolean) {
    auto b = std::get<bool>(k.value);
    if (c.sql.kind == SqlKind::Boolean) return b;
    return bytes_of(b ? "true" : "false");  // assigned to a string: its text
  }
  // A cast's: a string, bytes, a date or time, or a float.
  if (!is_number(k.type)) {
    if (is_string(k.type) && holds_string(c.sql)) return k.value;
    if (k.type.kind == c.sql.kind && k.type.kind != SqlKind::Real) return k.value;
    if (type_class(k.type.kind) == TypeClass::Temporal && type_class(c.sql.kind) == TypeClass::Temporal) {
      if (auto v = exact_temporal(k, c.sql.kind)) return *v;
      // A time with time zone assigned to a time drops its offset; a
      // timestamptz assigned to one is its UTC time at offset 0.
      if (assigned && k.type.kind == SqlKind::TimeTz && c.sql.kind == SqlKind::Time) {
        return timetz_unpack(std::get<std::int64_t>(k.value)).time;
      }
      if (assigned && k.type.kind == SqlKind::TimestampTz && c.sql.kind == SqlKind::TimeTz) {
        return cast_temporal(k, c.sql, k.pos).value;
      }
      // A pair PostgreSQL has no cast for: a date and a time, a time to a
      // date or a timestamp, a timestamp to a time with time zone.
      if (assigned && !temporal_cast(k.type.kind, c.sql.kind)) {
        throw SqlError{"column \"" + c.name + "\" is of type " + pg_type_name(c.sql) + " but expression is of type " +
                           pg_type_name(k.type),
                       k.pos, kDatatypeMismatch}
            .with_notes("", "You will need to rewrite or cast the expression.");
      }
    }
    if (assigned && holds_string(c.sql) && !is_float(k.type)) return bytes_of(to_text(k.value, k.type));
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
    case SqlKind::Char:
    case SqlKind::Name:
    case SqlKind::Varchar: {
      auto t = to_text(Datum{v}, numeric_type(scale));
      auto bytes = std::as_bytes(std::span{t.data(), t.size()});
      return Bytes(bytes.begin(), bytes.end());
    }
    case SqlKind::Boolean:
    case SqlKind::Bytea:
    case SqlKind::Timestamp:
    case SqlKind::TimestampTz:
    case SqlKind::Date:
    case SqlKind::Time:
    case SqlKind::TimeTz:
    case SqlKind::Numeric: break;  // refused by type_refusal
  }
  throw SqlError{"expression not supported", k.pos};
}

// coerce_value, then a CHAR(n)'s value without its trailing spaces, which are
// insignificant: the form the column stores, so a compared constant matches it.
auto coerce(const Constant& k, const Column& c, bool assigned, std::string_view op) -> Datum {
  auto d = coerce_value(k, c, assigned, op);
  if (auto* b = std::get_if<Bytes>(&d); b != nullptr && c.sql.kind == SqlKind::Char) {
    while (!b->empty() && b->back() == std::byte{' '}) b->pop_back();
  }
  return d;
}

auto select_param_count(const ast::Select& s) -> std::uint32_t;

// The largest $n the statement names, its subqueries' included: its own
// parameters, which come before the constants' (Binder::fixed).
void params_in(const ast::Expr& e, std::uint32_t& n) {
  if (e.kind == ast::Expr::Kind::Param) n = std::max(n, e.param + 1);
  if (e.subquery) n = std::max(n, select_param_count(*e.subquery));
  if (e.lhs) params_in(*e.lhs, n);
  if (e.rhs) params_in(*e.rhs, n);
  for (const auto& a : e.args) params_in(a, n);  // a function's, CASE's, COALESCE's
}

void params_in(const ast::Compare& c, std::uint32_t& n) {
  if (c.kind == ast::Compare::Kind::Compare || c.kind == ast::Compare::Kind::IsNull) {
    params_in(c.lhs, n);
    if (c.kind == ast::Compare::Kind::Compare) params_in(c.rhs, n);
  }
  if (c.kind == ast::Compare::Kind::Quantified || c.kind == ast::Compare::Kind::Like) {
    params_in(c.lhs, n);
    params_in(c.rhs, n);
  }
  if (c.subquery) n = std::max(n, select_param_count(*c.subquery));
  if (c.escape_param) n = std::max(n, *c.escape_param + 1);
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
          for (const auto& row : s.rows) {
            for (const auto& e : row) params_in(e, n);
          }
          if (s.query) n = std::max(n, select_param_count(*s.query));
          if (s.on_conflict && s.on_conflict->update) {
            for (const auto& a : *s.on_conflict->update) params_in(a.value, n);
          }
          for (const auto& e : s.returning) params_in(e, n);
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          n = select_param_count(s);
        } else if constexpr (std::is_same_v<T, ast::Update>) {
          for (const auto& a : s.set) params_in(a.value, n);
          conjuncts(s.where);
          for (const auto& e : s.returning) params_in(e, n);
        } else if constexpr (std::is_same_v<T, ast::Delete>) {
          conjuncts(s.where);
          for (const auto& e : s.returning) params_in(e, n);
        }
      },
      stmt);
  return n;
}

// A clock or session function by its SQL name, and the type it gives.
// CURRENT_ROLE and USER are CURRENT_USER, as in PostgreSQL.
auto clock_fn(const std::string& name, std::int32_t pos) -> ClockDefault::Fn {
  using F = ClockDefault::Fn;
  if (name == "current_user" || name == "current_role" || name == "user") return F::CurrentUser;
  if (name == "session_user") return F::SessionUser;
  if (name == "current_catalog") return F::CurrentCatalog;
  if (name == "current_schema") return F::CurrentSchema;
  if (name == "current_date") return F::CurrentDate;
  if (name == "current_timestamp") return F::CurrentTimestamp;
  if (name == "localtime") return F::LocalTime;
  if (name == "localtimestamp") return F::LocalTimestamp;
  if (name == "current_time") return F::CurrentTime;
  not_yet(name, pos);
}

auto clock_type(ClockDefault::Fn fn) -> SqlType {
  using F = ClockDefault::Fn;
  switch (fn) {
    case F::CurrentDate: return SqlType{SqlKind::Date};
    case F::CurrentTimestamp: return SqlType{SqlKind::TimestampTz};
    case F::LocalTime: return SqlType{SqlKind::Time};
    case F::LocalTimestamp: return SqlType{SqlKind::Timestamp};
    case F::CurrentTime: return SqlType{SqlKind::TimeTz};
    case F::CurrentUser:
    case F::SessionUser:
    case F::CurrentCatalog:
    case F::CurrentSchema: return SqlType{SqlKind::Name};
  }
  return SqlType{SqlKind::Timestamp};
}



// The slot a function's reading fills: a session function's by what it
// reads, the clock's by the type it is read as.
auto clock_slot_kind(ClockDefault::Fn fn, SqlKind as) -> ClockSlot::Kind {
  using Kind = ClockSlot::Kind;
  using F = ClockDefault::Fn;
  switch (fn) {
    case F::CurrentUser:
    case F::SessionUser: return Kind::User;
    case F::CurrentCatalog: return Kind::Catalog;
    case F::CurrentSchema: return Kind::Schema;
    case F::CurrentDate:
    case F::CurrentTimestamp:
    case F::LocalTime:
    case F::LocalTimestamp:
    case F::CurrentTime: break;
  }
  return as == SqlKind::Date          ? Kind::Date
         : as == SqlKind::Time        ? Kind::Time
         : as == SqlKind::TimeTz      ? Kind::TimeTz
         : as == SqlKind::TimestampTz ? Kind::TimestampTz
                                      : Kind::Timestamp;
}

// Whether PostgreSQL 17 has a cast from one date or time type to another:
// between a date and a timestamp, between the two timestamps, between the
// two times; a timestamp to a time, a timestamptz to a time with time zone.
// None between a date and a time, from a time to a date or a timestamp, or
// from a timestamp to a time with time zone.
auto temporal_cast(SqlKind from, SqlKind to) -> bool {
  using K = SqlKind;
  auto stamp = [](K k) { return k == K::Timestamp || k == K::TimestampTz; };
  auto time = [](K k) { return k == K::Time || k == K::TimeTz; };
  if (from == to || (stamp(from) && stamp(to)) || (time(from) && time(to))) return true;
  if (from == K::Date) return stamp(to);
  if (to == K::Date) return stamp(from);
  if (to == K::Time) return stamp(from);
  if (to == K::TimeTz) return from == K::TimestampTz;
  return false;
}

// A placeholder for a reading's slot, of its physical type: never the
// statement's value (ClockSlot).
auto clock_placeholder(SqlType t) -> Datum {
  if (phys_type(t) == PhysType::Bytes) return Bytes{};
  return std::int64_t{0};
}

// The clock's reading assigned to a column of `t`, as PostgreSQL's
// assignment cast gives it (clock_assignable): 42804 for a type with no such
// cast, refused for one it has that the layer does not compute (a date to a
// timestamp, which is midnight). A precision past 6 is 6, as PostgreSQL
// takes it, with a warning. `assigned` is a value in INSERT or UPDATE, not
// a column's DEFAULT, which PostgreSQL's 42804 message names.
auto clock_default(const std::string& name, std::int64_t precision, SqlType t, const std::string& column,
                   std::int32_t pos, bool assigned) -> ClockDefault {
  auto fn = clock_fn(name, pos);
  if (!clock_assignable(fn, t.kind)) {
    auto temporal = !is_session_fn(fn) && ((type_class(t.kind) == TypeClass::Temporal && temporal_cast(clock_type(fn).kind, t.kind)) ||
                                           t.kind == SqlKind::Text || t.kind == SqlKind::Varchar || t.kind == SqlKind::Name);
    if (temporal) not_yet("the clock's " + pg_type_name(clock_type(fn)) + " assigned to a " + pg_type_name(t) + " column", pos);
    throw SqlError{"column \"" + column + "\" is of type " + pg_type_name(t) + (assigned ? " but expression" : " but default expression") +
                       " is of type " + pg_type_name(clock_type(fn)),
                   pos, kDatatypeMismatch}
        .with_notes("", "You will need to rewrite or cast the expression.");
  }
  return {fn, static_cast<std::uint8_t>(precision < 0 || precision > 6 ? 6 : precision)};
}

auto column_spec(const ast::ColumnDefAst& c) -> ColumnSpec {
  auto t = column_type(c);
  ColumnSpec out{c.name, t, !c.not_null && !c.primary_key, c.default_value ? fold_default(*c.default_value, t, c.name) : Datum{}, {}};
  if (!c.default_clock.empty()) out.clock_default = clock_default(c.default_clock, c.default_clock_precision, t, c.name, c.pos, false);
  return out;
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
  auto is_date = [](SqlType t) { return t.kind == SqlKind::Date || t.kind == SqlKind::Timestamp || t.kind == SqlKind::TimestampTz; };
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
    if (type_of(own, f.columns[i])->kind == SqlKind::Numeric) {
      throw SqlError{"NUMERIC column \"" + f.columns[i] + "\" without a precision in a foreign key is not supported yet", f.pos,
                     kFeatureNotSupported};
    }
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

  // $1 .. $n start typed (bind's fixed_params): each as a column of its type,
  // which its first use is then checked against.
  void fix_params(std::span<const SqlType> types) {
    if (params_.size() < types.size()) params_.resize(types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
      const auto& col = typed_params_.emplace_back(
          Column{0, "$" + std::to_string(i + 1), types[i], phys_type(types[i]), true, std::monostate{}});
      params_[i].first = ParamUse::FirstUse{phys_type(types[i]), &col, Use::Compared};
    }
    fixed_params_ = types.size();
  }

  // What INSERT … SELECT's query fills (assign): `listed`, the statement
  // names its columns; else they are the table's, the leading ones filled.
  struct AssignTo {
    const Table* table;
    std::vector<const Column*> columns;
    bool listed;
  };

  // A constant's value as a parameter the API binds after the statement's
  // own. Its value is checked here or where it is written, so it has no
  // range check before the plan runs.
  // A REAL constant is the float its text rounds to, as PostgreSQL reads a
  // float4: a filter compares that, as the column holds it.
  auto fixed(Datum v, SqlType t) -> logical::Expr {
    if (auto* d = std::get_if<double>(&v); d != nullptr && t.kind == SqlKind::Real) *d = float_rounding(*d);
    auto n = own_params_ + bytecask::sql::narrow<std::uint32_t>(fixed_.size());
    fixed_.push_back(std::move(v));
    fixed_types_.push_back(phys_type(t));
    fixed_sql_.push_back(t);
    return logical::param(n, t);
  }
  // CURRENT_DATE, LOCALTIME or LOCALTIMESTAMP, or a session function
  // (CURRENT_USER, …): a constant slot the runtime fills with the
  // statement's time, or the session's name, on every run.
  auto clock(const ast::Expr& e) -> logical::Output {
    auto fn = clock_fn(e.func, e.pos);
    // PostgreSQL warns and uses 6 for a larger precision.
    auto precision = static_cast<std::uint8_t>(e.constant < 0 || e.constant > 6 ? 6 : e.constant);
    auto type = clock_type(fn);
    // A placeholder, never the statement's value: no rewrite may fold or
    // range-check it, as the API replaces it on every run (ClockSlot).
    auto expr = fixed(clock_placeholder(type), type);
    clocks_.push_back({expr.param - own_params_, clock_slot_kind(fn, type.kind), precision});
    return {std::move(expr), e.column.empty() ? e.func : e.column};
  }
  // The statement's reading of the clock as a column of `target`'s type
  // takes it (clock_assignable): a parameter the API fills on each run, as
  // it fills a clock in a select list.
  auto clock_reading(ClockDefault d, const Column& target) -> logical::Expr {
    // A placeholder, never the statement's value: no rewrite may fold or
    // range-check it, as the API replaces it on every run (ClockSlot). A
    // session function's NAME is checked as it is written into the column
    // (a VARCHAR(n) too short is 22001), as PostgreSQL's assignment cast.
    auto expr = fixed(clock_placeholder(target.sql), target.sql);
    clocks_.push_back({expr.param - own_params_, clock_slot_kind(d.fn, target.sql.kind), d.precision});
    return expr;
  }
  // A clock reading of a TIME (LOCALTIME) compared with a TIME WITH TIME
  // ZONE, `peer`: its slot read as a time with time zone instead, the same
  // reading at offset 0, as PostgreSQL casts a time to one. False for any
  // other pair.
  auto clock_as_timetz(logical::Expr& x, const logical::Expr& peer) -> bool {
    if (x.type.kind != SqlKind::Time || peer.type.kind != SqlKind::TimeTz) return false;
    if (x.kind != logical::Expr::Kind::Param || !is_fixed(x.param)) return false;
    auto slot = x.param - own_params_;
    auto k = std::ranges::find_if(clocks_, [&](const ClockSlot& c) { return c.slot == slot; });
    if (k == clocks_.end() || k->kind != ClockSlot::Kind::Time) return false;
    k->kind = ClockSlot::Kind::TimeTz;
    x.type = SqlType{SqlKind::TimeTz};
    fixed_sql_.at(slot) = x.type;
    return true;
  }
  [[nodiscard]] auto take_clocks() -> std::vector<ClockSlot> { return std::move(clocks_); }
  [[nodiscard]] auto is_fixed(std::uint32_t n) const -> bool { return n >= own_params_; }
  [[nodiscard]] auto fixed_value(std::uint32_t n) const -> const Datum& { return fixed_.at(n - own_params_); }
  [[nodiscard]] auto fixed_values() const -> const std::vector<Datum>& { return fixed_; }
  [[nodiscard]] auto fixed_types() const -> const std::vector<PhysType>& { return fixed_types_; }
  [[nodiscard]] auto fixed_sql() const -> const std::vector<SqlType>& { return fixed_sql_; }
  [[nodiscard]] auto take_subqueries() -> std::vector<logical::Subquery> { return std::move(subqueries_); }
  // The tables the plan names that the catalog does not hold: the instances
  // and the derived relations.
  [[nodiscard]] auto take_instances() -> std::vector<std::unique_ptr<Table>> {
    auto out = std::move(instances_);
    for (auto& d : derived_) out.push_back(std::move(d.table));
    derived_.clear();
    return out;
  }
  void set_scope(std::vector<const Table*> tables) { scope_ = std::move(tables); }

  // A subquery's SELECT, bound as any is, then listed to run before the
  // statement's plan (logical::Subquery), its values in hidden parameters
  // after the constants': its index and first parameter. One level deep,
  // not FOR UPDATE; of one column, but for EXISTS, whose rows are counted
  // and taken one at most. A correlated one (correlation, below) takes
  // each outer column it reads as a hidden parameter of its own, an
  // argument set from the outer row before each run.
  struct BoundSubquery {
    std::uint32_t index;
    std::uint32_t param;
    SqlType type;
    std::string name;
  };
  auto subquery(const ast::Select& q, logical::Subquery::Kind kind, std::int32_t pos) -> BoundSubquery {
    if (depth_ > 0) not_yet("a subquery inside a subquery", pos);
    if (q.for_update) not_yet("FOR UPDATE in a subquery", pos);
    if (correlated(q)) correlation(q, kind, pos);
    auto joins = std::move(joins_);
    auto scope = std::move(scope_);
    ++depth_;
    auto root = select(q);
    --depth_;
    std::vector<logical::Subquery::Arg> args;
    if (correlating_) args = std::move(correlating_->args);
    correlating_.reset();
    joins_ = std::move(joins);
    scope_ = std::move(scope);
    using K = logical::Subquery::Kind;
    SqlType type{SqlKind::Boolean};
    std::string name = "?column?";
    if (kind == K::Exists) {
      // EXISTS needs one row: past the OFFSET, if there is one.
      auto* l = std::get_if<logical::Limit>(&root->op);
      if (l == nullptr) {
        root = logical::node(logical::Limit{std::move(root), 1});
      } else if (l->n - l->offset > 1) {
        l->n = l->offset + 1;
      }
    } else {
      const auto& outs = outputs_of(*root);
      if (outs.size() != 1) throw SqlError{"subquery must return only one column", pos};
      type = outs[0].expr.type;
      // AVG's or a NUMERIC column's: compared or matched where a subquery
      // stands, which a NUMERIC is not yet.
      if (type.kind == SqlKind::Numeric) refuse_numeric("a subquery of a NUMERIC value", pos);
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
    subqueries_.push_back({std::move(root), kind, type, first, std::move(args)});
    return {index, first, type, name};
  }

  // A correlated subquery, as far as the layer runs one (docs/frontend_design.md,
  // *Subqueries*): IN, NOT IN, EXISTS, ANY or ALL, a conjunct of the WHERE
  // of a query of one table, itself of one table, each outer column it reads
  // compared with a column of its own in its WHERE. Anything else is refused
  // here, before it is bound (#836). Sets correlating_, which compare()
  // reads.
  void correlation(const ast::Select& q, logical::Subquery::Kind kind, std::int32_t pos) {
    if (kind == logical::Subquery::Kind::Value) not_yet("a correlated scalar subquery", pos);
    if (scope_.size() != 1) not_yet("a correlated subquery in a query of a join", pos);
    if (!in_where_) not_yet("a correlated subquery outside a WHERE", pos);
    // An arm's plan is printed apart from the statement's subqueries.
    if (in_set_arm_) not_yet("a correlated subquery in an arm of a set operation (UNION, INTERSECT, EXCEPT)", pos);
    if (Table::derived(scope_[0]->id)) not_yet("a correlated subquery in a query of a grouped view", pos);
    if (q.set) not_yet("a correlated subquery of a set operation (UNION, INTERSECT, EXCEPT)", pos);
    if (q.from.size() != 1) not_yet(q.from.empty() ? "a correlated subquery without FROM" : "a correlated subquery of a join", pos);
    if (q.from[0].derived) not_yet("a correlated subquery of a grouped view", pos);
    auto outer = [&](const ast::Expr& e) { return outer_ref(q, scope_, e); };
    std::function<void(const ast::Expr&, const char*)> none = [&](const ast::Expr& e, const char* where) {
      if (e.kind == ast::Expr::Kind::Column && outer(e)) {
        not_yet(std::string{"a column of the query around a subquery "} + where, e.pos);
      }
      if (e.lhs) none(*e.lhs, where);
      if (e.rhs) none(*e.rhs, where);
      for (const auto& a : e.args) none(a, where);
    };
    constexpr auto kElsewhere = "other than compared with a column of the subquery";
    std::function<void(const ast::Compare&)> conjunct = [&](const ast::Compare& c) {
      using K = ast::Compare::Kind;
      switch (c.kind) {
        case K::And:
        case K::Or:
        case K::Not:
          for (const auto& a : c.args) conjunct(a);
          return;
        case K::Compare: {
          auto column = [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Column; };
          auto lo = column(c.lhs) && outer(c.lhs);
          auto ro = column(c.rhs) && outer(c.rhs);
          if ((lo && column(c.rhs) && !ro) || (ro && column(c.lhs) && !lo)) return;
          none(c.lhs, kElsewhere);
          none(c.rhs, kElsewhere);
          return;
        }
        case K::IsNull:
        case K::Like:
        case K::Exists:
        case K::Quantified:
        case K::Const:
          none(c.lhs, kElsewhere);
          none(c.rhs, kElsewhere);
          return;
      }
    };
    for (const auto& c : q.where) conjunct(c);
    for (const auto& e : q.targets) none(e, "in its select list");
    for (const auto& e : q.group_by) none(e, "in its GROUP BY");
    for (const auto& o : q.order_by) none(o.expr, "in its ORDER BY");
    std::function<void(const ast::Compare&)> having = [&](const ast::Compare& c) {
      none(c.lhs, "in its HAVING");
      none(c.rhs, "in its HAVING");
      for (const auto& a : c.args) having(a);
    };
    for (const auto& c : q.having) having(c);
    correlating_ = Correlation{&q, scope_, {}};
  }

  // A column of the outer query the correlated subquery being bound
  // compares with its column c, as a hidden parameter: one per column and
  // type, NULL until an outer row sets it. Of a type a comparison of the
  // two takes (comparable), as a subquery's value compared with a column
  // is: an integer wider than c's key part is a filter, never a key bound.
  auto outer_param(const ast::Expr& ref, const Column& c, std::int32_t pos) -> logical::Expr {
    const auto* t = correlating_->outer[0];
    const auto* oc = column(*t, ref);
    if (oc->sql.kind == SqlKind::Char || c.sql.kind == SqlKind::Char) {
      not_yet("a correlated subquery comparing a CHAR(n) column", pos);
    }
    numeric_compared(oc->sql, c.sql, pos);
    if (!compares(oc->sql, c)) {
      not_yet("comparing " + c.name + " (" + sql_type_name(c.sql) + ") with the outer column " + oc->name + " (" +
                  sql_type_name(oc->sql) + ") in a correlated subquery",
              pos);
    }
    auto as = subquery_param_type(oc->sql, c);
    auto col = logical::column_of(t->id, *oc).col;
    auto& args = correlating_->args;
    auto it = std::ranges::find_if(args, [&](const logical::Subquery::Arg& a) {
      return a.col == col && fixed_sql_.at(a.param - own_params_) == as;
    });
    if (it != args.end()) return logical::param(it->param, as);
    auto p = fixed(Datum{}, as).param;
    args.push_back({col, p});
    return logical::param(p, as);
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

  // Whether column reference e of subquery q names a column of the query
  // around it, whose relations are `outer`: qualified by one of their names
  // and none of q's, or unqualified, of none of q's own relations but of
  // one of `outer`'s.
  auto outer_ref(const ast::Select& q, std::span<const Table* const> outer, const ast::Expr& e) const -> bool {
    if (!e.table.empty()) {
      if (std::ranges::any_of(q.from, [&](const ast::FromItem& item) { return e.table == item.name(); })) return false;
      return std::ranges::any_of(outer, [&](const Table* t) { return t->name == e.table; });
    }
    for (const auto& item : q.from) {
      if (item.derived) {  // its columns are its aliases
        if (std::ranges::contains(item.column_aliases, e.column)) return false;
      } else if (const auto* t = cat_.table(item.table); t != nullptr && t->column(e.column) != nullptr) {
        return false;
      }
    }
    return std::ranges::any_of(outer, [&](const Table* t) { return t->column(e.column) != nullptr; });
  }

  // Whether the subquery names a column of the query around it.
  auto correlated(const ast::Select& q) const -> bool {
    if (q.set) return correlated(q.set->left) || correlated(q.set->right);
    std::function<bool(const ast::Expr&)> in_expr = [&](const ast::Expr& e) {
      if (e.kind == ast::Expr::Kind::Column && outer_ref(q, scope_, e)) return true;
      return (e.lhs && in_expr(*e.lhs)) || (e.rhs && in_expr(*e.rhs)) || std::ranges::any_of(e.args, in_expr);
    };
    std::function<bool(const ast::Compare&)> in_compare = [&](const ast::Compare& c) {
      return in_expr(c.lhs) || in_expr(c.rhs) || std::ranges::any_of(c.args, in_compare);
    };
    return std::ranges::any_of(q.targets, in_expr) || std::ranges::any_of(q.where, in_compare) ||
           std::ranges::any_of(q.having, in_compare) || std::ranges::any_of(q.group_by, in_expr) ||
           std::ranges::any_of(q.order_by, [&](const ast::OrderItem& o) { return in_expr(o.expr); });
  }

  // Whether a column of type `col` is compared with a subquery's value of
  // type `sub` as it holds it: integers with integers, compared as 64 bits
  // whatever their declared widths, so one past the column's type matches
  // nothing and bounds a range where it falls; a DECIMAL of its
  // scale; strings with strings; any other type its own.
  // The type of a parameter holding a subquery's value compared with c: c's
  // type, unless the value is a wider integer than c's key part holds; then
  // its own, which the search compares as a filter on the row, never through
  // the key (types_design.md, *Key encoding*).
  static auto subquery_param_type(SqlType sub, const Column& c) -> SqlType {
    auto wider = is_integer(c.sql) && key_part_of(sub, false, false).width > key_part_of(c.sql, false, false).width;
    return wider ? sub : c.sql;
  }

  [[nodiscard]] static auto compares(SqlType sub, const Column& c) -> bool {
    return (is_integer(sub) && is_integer(c.sql)) ||
           (sub.kind == SqlKind::Decimal && c.sql.kind == SqlKind::Decimal && sub.scale == c.sql.scale) ||
           (is_string(sub) && is_string(c.sql)) || (!is_integer(sub) && sub.kind != SqlKind::Decimal && sub.kind == c.sql.kind);
  }

  static void comparable(SqlType sub, const Column& c, std::int32_t pos) {
    if (type_class(sub.kind) == type_class(c.sql.kind)) numeric_compared(sub, c.sql, pos);
    if (!compares(sub, c)) {
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
      auto hi = logical::param(s.param, subquery_param_type(s.type, c));
      auto lo = logical::param(s.param + 1, subquery_param_type(s.type, c));
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
        case ast::Expr::Kind::Func: return e.func == "||" || e.func == "%" ? "?column?" : e.func;  // an operator is unnamed
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
        case ast::Expr::Kind::IsNull:
        case ast::Expr::Kind::Not:
        case ast::Expr::Kind::And:
        case ast::Expr::Kind::Or:
        case ast::Expr::Kind::Aggregate:
        case ast::Expr::Kind::Clock:
        case ast::Expr::Kind::Default:
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

  // How a statement uses a parameter: compared with a column (WHERE or a
  // select list), assigned to one (INSERT, SET), the operand of col + $n or
  // col - $n, or an operand of another operator in a select list.
  enum class Use { Compared, Assigned, Added, Subtracted, Operand };

  // ------------------------------------------------------ expressions over columns

  // A select-list expression over columns (docs/frontend_design.md,
  // *Expressions over columns*): a tree of columns, constants (each a hidden
  // parameter, folded first), scalar subqueries (the parameters they set),
  // $n, + - * /, comparisons, IS [NOT] NULL, and AND, OR and NOT. The
  // search plans it as a result column and codegen evaluates it per row.
  using Resolve = std::function<std::pair<const Table*, const Column*>(const ast::Expr&)>;

  // What a $n or an unknown literal takes its type from: the other operand
  // of its operator. A $n needs a plain column there, as in a WHERE.
  struct Peer {
    SqlType type;
    const Table* table{nullptr};
    const Column* column{nullptr};
    Use use{Use::Compared};
  };

  auto value(const ast::Expr& e, const Resolve& resolve, const std::optional<Peer>& peer = std::nullopt) -> logical::Expr {
    using K = ast::Expr::Kind;
    if (is_constant(e)) {
      auto c = fold(e);
      if (c.unknown && peer) c = resolved(c, peer->type);
      return fixed(std::move(c.value), c.type);
    }
    switch (e.kind) {
      case K::Column: {
        auto [t, c] = resolve(e);
        return logical::column_of(t->id, *c);
      }
      case K::Param:
        if (!peer || peer->column == nullptr) not_yet("a parameter beside other than a column", e.pos);
        use_param(e.param, phys_type(peer->column->sql), e.pos, *peer->column, peer->table->id, peer->use);
        return logical::param(e.param, peer->column->sql);
      case K::Subquery: return subquery_output(e).expr;
      case K::Clock: return clock(e).expr;
      case K::Add:
      case K::Sub:
      case K::Mul:
      case K::Div: return arithmetic_value(e, resolve);
      case K::Compare: return comparison_value(e, resolve);
      case K::IsNull: {
        auto is_null = logical::expr(logical::Expr::Kind::IsNull, SqlType{SqlKind::Boolean});
        is_null.args.push_back(value(*e.rhs, resolve));
        return e.negated ? logical::predicate_of(logical::Expr::Kind::Not, {std::move(is_null)}) : is_null;
      }
      case K::Not:
      case K::And:
      case K::Or: return boolean_value(e, resolve);
      case K::Aggregate: not_yet("an aggregate inside an expression", e.pos);
      case K::Star: throw SqlError{"syntax error at or near \"*\"", e.pos};
      case K::Plus:
      case K::Neg: return sign_value(e, resolve);
      case K::Cast: {
        // $n::T types $n as T, as PostgreSQL types an untyped $n by its
        // cast; a cast of anything else that is not a constant is not
        // taken yet.
        if (!param_cast(e)) {
          if (e.rhs->kind == K::Column && resolve(*e.rhs).second->sql.kind == SqlKind::Numeric) refuse_numeric("a cast of a NUMERIC", e.pos);
          // A NAME, TEXT or VARCHAR to TEXT (or VARCHAR of no length), or a
          // NAME to NAME, keeps its bytes: a relabelling, as PostgreSQL's
          // binary-coercible cast.
          // VARCHAR without a length is TEXT, as everywhere in the layer.
          if (e.cast_typmods.empty() && (e.cast_type == "text" || e.cast_type == "varchar" || e.cast_type == "name")) {
            auto v = value(*e.rhs, resolve);
            auto to = e.cast_type == "name" ? SqlKind::Name : SqlKind::Text;
            if (to == SqlKind::Text ? is_string(v.type) : v.type.kind == SqlKind::Name) {
              v.type = SqlType{to};
              return v;
            }
          }
          not_yet("a cast of other than a constant or a parameter", e.pos);
        }
        if (e.cast_type == "numeric" && e.cast_typmods.empty()) refuse_numeric("a cast of a parameter to NUMERIC", e.pos);
        if (modified_cast(e)) not_yet("a cast of a parameter to DECIMAL, VARCHAR(n) or CHAR(n)", e.pos);
        auto as = cast_type(e);
        const auto& col =
            typed_params_.emplace_back(Column{0, "$" + std::to_string(e.rhs->param + 1), as, phys_type(as), true, std::monostate{}});
        use_param(e.rhs->param, phys_type(as), e.rhs->pos, col, 0, Use::Operand);
        return logical::param(e.rhs->param, as);
      }
      case K::Case:
      case K::Coalesce:
      case K::NullIf: return conditional_value(e, resolve);
      case K::Func: return function_value(e, resolve);
      case K::Const:
      case K::Literal: break;  // constants, above
    }
    throw SqlError{"expression not supported", e.pos};
  }

  // The type CASE, COALESCE or NULLIF gives branches of these types, as
  // PostgreSQL's union rules resolve it (the constants' fold is common_type):
  // one class (42804), integers widen, a numeric is at the finest scale of
  // its branches (one scale for every row, where PostgreSQL's keeps each
  // value's own), a float the wider float. Floats mixed with other numbers
  // and different date and time types are refused.
  static auto union_type(const std::vector<SqlType>& known, const std::string& what, std::int32_t pos) -> SqlType {
    if (known.empty()) return SqlType{SqlKind::Text};
    auto type = known.front();
    if (type.kind == SqlKind::Varchar) type = SqlType{SqlKind::Text};
    for (const auto& t : std::span{known}.subspan(1)) {
      if (type_class(type.kind) != type_class(t.kind)) {
        throw SqlError{what + " types " + pg_type_name(type) + " and " + pg_type_name(t) + " cannot be matched", pos, kDatatypeMismatch};
      }
      if (type_class(type.kind) == TypeClass::String) {
        name_union(known.front(), t, what, pos);
        // NAMEs alone stay a NAME, as PostgreSQL resolves them.
        if (type.kind != SqlKind::Name || t.kind != SqlKind::Name) type = SqlType{SqlKind::Text};
      } else if (type_class(type.kind) == TypeClass::Number) {
        // NUMERICs alone are carried as their text; with another number the
        // result is computed, a cast, which is not built yet.
        if (type.kind == SqlKind::Numeric && t.kind == SqlKind::Numeric) continue;
        if (type.kind == SqlKind::Numeric || t.kind == SqlKind::Numeric) refuse_numeric(what + " of a NUMERIC and another number", pos);
        if (!(is_number(type) || is_float(type)) || !(is_number(t) || is_float(t))) not_yet(what + " of a numeric of no scale", pos);
        if (is_float(type) != is_float(t)) not_yet(what + " of a floating-point value and another number", pos);
        if (is_float(t)) {
          if (t.kind == SqlKind::Double) type = t;
        } else if (type.kind == SqlKind::Decimal || t.kind == SqlKind::Decimal) {
          type = numeric_type(std::max(decimal_scale(type), decimal_scale(t)));
        } else {
          type = wider_integer(type, t);
        }
      } else if (type.kind != t.kind) {
        not_yet(what + " of " + pg_type_name(type) + " and " + pg_type_name(t), pos);
      }
    }
    return type;
  }

  // CASE, COALESCE and NULLIF over columns, parameters and subqueries
  // (docs/frontend_design.md, *Expressions over columns*). A simple CASE is a
  // searched one over `x = v`, NULLIF(a, b) is CASE WHEN a = b THEN NULL ELSE
  // a END. The branches are typed together (union_type) and then each is
  // bound to that type, an unknown literal read as it (22P02); a branch
  // is evaluated, in the plan, only when it is taken. A `$n` takes its type
  // from a column among the branches.
  // || and SUBSTRING (substr) of a column, a parameter-bound value or a
  // procedure's variable: text built at run time (bc_concat, bc_substr).
  // `||` takes text with text, or with an integer or DECIMAL through its
  // text, as PostgreSQL's anynonarray || text does; the other string
  // functions of non-constants are not built yet (#671).
  // +x and -x of a number other than a constant: + is x; - of x's type, an
  // integer's range checked when the plan runs. Of a parameter it is
  // ambiguous in PostgreSQL, and not taken.
  auto sign_value(const ast::Expr& e, const Resolve& resolve) -> logical::Expr {
    auto plus = e.kind == ast::Expr::Kind::Plus;
    if (e.rhs->kind == ast::Expr::Kind::Param) not_yet(std::string{"unary "} + (plus ? "+" : "-") + " of a parameter", e.pos);
    auto v = value(*e.rhs, resolve);
    if (v.type.kind == SqlKind::Numeric) refuse_numeric(std::string{"unary "} + (plus ? "+" : "-") + " of a NUMERIC", e.pos);
    if (!is_number(v.type) && !is_float(v.type)) {
      throw SqlError{std::string{"operator does not exist: "} + (plus ? "+ " : "- ") + pg_type_name(v.type), e.pos,
                     kUndefinedFunction};
    }
    if (plus) return v;
    auto out = logical::expr(logical::Expr::Kind::Neg, v.type);
    out.args.push_back(std::move(v));
    return out;
  }

  auto function_value(const ast::Expr& e, const Resolve& resolve) -> logical::Expr {
    auto texty = [](SqlType t) { return holds_string(t); };
    auto exact = [](SqlType t) {
      return t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer || t.kind == SqlKind::BigInt || t.kind == SqlKind::Decimal;
    };
    // A $n is of the type its place takes, as PostgreSQL infers it there:
    // text beside ||, integer for SUBSTRING's start and count.
    auto arg = [&](const ast::Expr& a, SqlType as) {
      if (a.kind != ast::Expr::Kind::Param) return value(a, resolve);
      const auto& col = typed_params_.emplace_back(Column{0, "$" + std::to_string(a.param + 1), as, phys_type(as), true, std::monostate{}});
      use_param(a.param, phys_type(as), a.pos, col, 0, Use::Operand);
      return logical::param(a.param, as);
    };
    if (e.func == "%") {
      auto [a, b] = operands(e.args[0], e.args[1], resolve, Use::Operand);
      numeric_computed(a.type, b.type, e.pos);
      if (!is_number(a.type) || !is_number(b.type)) {
        throw SqlError{"operator does not exist: " + pg_type_name(a.type) + " % " + pg_type_name(b.type), e.pos, kUndefinedFunction};
      }
      auto type = is_integer(a.type) && is_integer(b.type) ? wider_integer(a.type, b.type)
                                                            : numeric_type(std::max(decimal_scale(a.type), decimal_scale(b.type)));
      rescalable(a, b, decimal_scale(type), e.pos);
      return logical::binary(logical::Expr::Kind::Mod, std::move(a), std::move(b), type);
    }
    if (e.func == "abs") {
      if (e.args.size() != 1) throw SqlError{"function abs takes one argument", e.pos, kUndefinedFunction};
      if (e.args[0].kind == ast::Expr::Kind::Param) not_yet("abs of a parameter", e.pos);
      auto v = value(e.args[0], resolve);
      if (v.type.kind == SqlKind::Numeric) refuse_numeric("abs of a NUMERIC", e.pos);
      if (!is_number(v.type) && !is_float(v.type)) {
        throw SqlError{"function abs(" + pg_type_name(v.type) + ") does not exist", e.pos, kUndefinedFunction};
      }
      auto out = logical::expr(logical::Expr::Kind::Abs, v.type);
      out.args.push_back(std::move(v));
      return out;
    }
    if (e.func == "extract") {
      if (e.args.size() != 2) not_yet("pg_catalog.extract of other than two arguments", e.pos);
      if (!is_constant(e.args[0])) not_yet("EXTRACT of a unit other than a name or a string", e.pos);
      auto unit = fold(e.args[0]);
      auto name = extract_unit_text(unit, e.pos);
      if (e.args[1].kind == ast::Expr::Kind::Param) extract_source(unit, std::nullopt, e.pos);
      auto x = value(e.args[1], resolve);
      extract_source(unit, x.type, e.pos);
      auto [field, type] = extract_field_type(name, x.type, e.pos);
      auto out = logical::expr(logical::Expr::Kind::Extract, type);
      out.constant = static_cast<std::int64_t>(field);
      out.args.push_back(std::move(x));
      return out;
    }
    if (e.func == "||") {
      auto a = arg(e.args[0], SqlType{SqlKind::Text});
      auto b = arg(e.args[1], SqlType{SqlKind::Text});
      for (const auto* x : {&a, &b}) {
        if (x->type.kind == SqlKind::Numeric) refuse_numeric("|| of a NUMERIC", e.pos);
        if (!texty(x->type) && !exact(x->type)) not_yet("|| of a " + pg_type_name(x->type) + " value", e.pos);
      }
      if (!texty(a.type) && !texty(b.type)) {
        throw SqlError{"operator does not exist: " + pg_type_name(a.type) + " || " + pg_type_name(b.type), e.pos,
                       kUndefinedFunction};
      }
      return logical::binary(logical::Expr::Kind::Concat, std::move(a), std::move(b), SqlType{SqlKind::Text});
    }
    if (e.func == "substring" || e.func == "substr") {
      if (e.args.size() < 2 || e.args.size() > 3) throw SqlError{"function " + e.func + " takes 2 or 3 arguments", e.pos, kUndefinedFunction};
      auto out = logical::expr(logical::Expr::Kind::Substr, SqlType{SqlKind::Text});
      out.args.push_back(arg(e.args[0], SqlType{SqlKind::Text}));
      if (!texty(out.args[0].type)) not_yet(e.func + " of a " + pg_type_name(out.args[0].type) + " value", e.pos);
      for (std::size_t i = 1; i < e.args.size(); ++i) {
        auto n = arg(e.args[i], SqlType{SqlKind::Integer});
        if (n.type.kind != SqlKind::SmallInt && n.type.kind != SqlKind::Integer) {
          if (texty(n.type)) not_yet("SUBSTRING(s FROM 'pattern'), the regular-expression form", e.pos);
          throw SqlError{"function " + e.func + "(text, " + pg_type_name(n.type) + ") does not exist", e.pos, kUndefinedFunction};
        }
        out.args.push_back(std::move(n));
      }
      return out;
    }
    // The string functions of a value only run time gives: each argument
    // text, as a $n is taken to be.
    using Text = logical::Expr::Text;
    static const std::array<std::pair<std::string_view, Text>, 9> kTextFunctions{{{"upper", Text::Upper},
                                                                                 {"lower", Text::Lower},
                                                                                 {"char_length", Text::CharLength},
                                                                                 {"character_length", Text::CharLength},
                                                                                 {"octet_length", Text::OctetLength},
                                                                                 {"btrim", Text::Btrim},
                                                                                 {"ltrim", Text::Ltrim},
                                                                                 {"rtrim", Text::Rtrim},
                                                                                 {"position", Text::Position}}};
    if (auto f = std::ranges::find(kTextFunctions, e.func, &std::pair<std::string_view, Text>::first); f != kTextFunctions.end()) {
      auto two = f->second == Text::Position;
      auto trim = f->second == Text::Btrim || f->second == Text::Ltrim || f->second == Text::Rtrim;
      auto lo = two ? 2U : 1U;
      auto hi = two || trim ? 2U : 1U;
      std::vector<logical::Expr> args;
      for (const auto& a : e.args) args.push_back(arg(a, SqlType{SqlKind::Text}));
      if (args.size() < lo || args.size() > hi || !std::ranges::all_of(args, [&](const logical::Expr& a) { return texty(a.type); })) {
        std::string types;
        for (const auto& a : args) types += (types.empty() ? "" : ", ") + pg_type_name(a.type);
        // POSITION is grammar for pg_catalog.position, which PostgreSQL names so.
        throw SqlError{"function " + std::string{two ? "pg_catalog." : ""} + e.func + "(" + types + ") does not exist", e.pos,
                       kUndefinedFunction};
      }
      auto counts = f->second == Text::CharLength || f->second == Text::OctetLength || two;
      auto out = logical::expr(logical::Expr::Kind::TextFn, SqlType{counts ? SqlKind::Integer : SqlKind::Text});
      out.text = f->second;
      out.args = std::move(args);
      return out;
    }
    if (e.func == "random") {
      // PostgreSQL 17's random(min, max) of integers, both bounds included.
      if (e.args.size() != 2) not_yet("random() other than random(min, max) of integers", e.pos);
      auto lo = arg(e.args[0], SqlType{SqlKind::Integer});
      auto hi = arg(e.args[1], SqlType{SqlKind::Integer});
      auto integer = [](SqlType t) { return t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer || t.kind == SqlKind::BigInt; };
      if (!integer(lo.type) || !integer(hi.type)) {
        throw SqlError{"function random(" + pg_type_name(lo.type) + ", " + pg_type_name(hi.type) + ") does not exist", e.pos,
                       kUndefinedFunction};
      }
      auto big = lo.type.kind == SqlKind::BigInt || hi.type.kind == SqlKind::BigInt;
      return logical::binary(logical::Expr::Kind::Random, std::move(lo), std::move(hi),
                             SqlType{big ? SqlKind::BigInt : SqlKind::Integer});
    }
    not_yet(e.func + " of other than constants", e.pos);
  }

  auto conditional_value(const ast::Expr& e, const Resolve& resolve) -> logical::Expr {
    using K = ast::Expr::Kind;
    auto what = std::string{e.kind == K::Case ? "CASE" : e.kind == K::Coalesce ? "COALESCE" : "NULLIF"};
    auto equals = [](const ast::Expr& a, const ast::Expr& b, std::int32_t pos) {
      ast::Expr c{K::Compare, {}, 0, nullptr, nullptr, pos};
      c.cmp = ast::ValueCompare::Eq;
      c.lhs = std::make_unique<ast::Expr>(ast::clone(a));
      c.rhs = std::make_unique<ast::Expr>(ast::clone(b));
      return c;
    };
    std::deque<ast::Expr> made;  // the conditions and the NULL this builds: stable addresses
    std::vector<const ast::Expr*> conds, results;
    if (e.kind == K::Case) {
      for (std::size_t i = 0; i + 1 < e.args.size(); i += 2) {
        conds.push_back(e.lhs ? &made.emplace_back(equals(*e.lhs, e.args[i], e.args[i].pos)) : &e.args[i]);
        results.push_back(&e.args[i + 1]);
      }
      if (e.rhs) results.push_back(e.rhs.get());
    } else if (e.kind == K::Coalesce) {
      for (const auto& a : e.args) results.push_back(&a);
    } else {
      conds.push_back(&made.emplace_back(equals(*e.lhs, *e.rhs, e.pos)));
      auto& null = made.emplace_back(K::Literal, std::string{}, 0, nullptr, nullptr, e.pos);
      results = {&null, e.lhs.get()};
    }

    std::vector<logical::Expr> args;
    // Each condition is a BOOLEAN (42804); NULL is not true.
    std::vector<logical::Expr> bound_conds;
    for (const auto* c : conds) {
      auto v = value(*c, resolve, Peer{SqlType{SqlKind::Boolean}, nullptr, nullptr, Use::Compared});
      if (v.type.kind != SqlKind::Boolean) {
        throw SqlError{"argument of CASE/WHEN must be type boolean, not type " + pg_type_name(v.type), c->pos, kDatatypeMismatch};
      }
      bound_conds.push_back(std::move(v));
    }

    // The results: constants folded (an error they raise is PostgreSQL's too,
    // which folds them as it plans), parameters waiting for a type, the rest bound.
    struct Part {
      std::optional<Constant> constant;
      std::optional<logical::Expr> bound;
    };
    std::vector<Part> parts(results.size());
    std::optional<Peer> peer;
    std::vector<SqlType> known;
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = *results[i];
      if (is_constant(r)) {
        parts[i].constant = fold(r);
        if (!parts[i].constant->unknown) known.push_back(parts[i].constant->type);
      } else if (r.kind != K::Param) {
        parts[i].bound = value(r, resolve);
        known.push_back(parts[i].bound->type);
        if (r.kind == K::Column && !peer) {
          auto [t, c] = resolve(r);
          peer = Peer{c->sql, t, c, Use::Operand};
        }
      }
    }
    if (std::ranges::any_of(results, [](const ast::Expr* r) { return r->kind == K::Param; })) {
      if (!peer) not_yet("a parameter in " + what + " with no column among its results to give it a type", e.pos);
      known.push_back(peer->type);
    }
    auto type = union_type(known, what, e.pos);
    // A VARCHAR(n) or CHAR(n) of every branch of a CASE or COALESCE keeps
    // its length, as PostgreSQL's exprTypmod does, when none is a literal or
    // a $n and a CASE has an ELSE (its default NULL has no length).
    // (union_type keeps a lone CHAR(n), NULLIF's first argument's included.)
    auto lengthed = [](SqlType t) { return t.kind == SqlKind::Varchar || t.kind == SqlKind::Char; };
    if (e.kind != K::NullIf && (e.kind != K::Case || e.rhs) && !known.empty() && lengthed(known.front()) &&
        std::ranges::none_of(results, [](const ast::Expr* r) { return r->kind == K::Param; }) &&
        std::ranges::none_of(parts, [](const Part& p) { return p.constant && p.constant->unknown; }) &&
        std::ranges::all_of(known, [&](SqlType t) { return t == known.front(); })) {
      type = known.front();
    }
    if (type.kind == SqlKind::Decimal) {  // an unknown literal may be written at a finer scale
      auto scale = decimal_scale(type);
      for (const auto& p : parts) {
        if (p.constant && p.constant->unknown) scale = std::max(scale, decimal_scale(resolved(*p.constant, type).type));
      }
      if (scale > kMaxDecimalPrecision) not_yet("a numeric of more than 18 digits", e.pos);
      type = numeric_type(scale);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      auto& p = parts[i];
      if (!p.bound && !p.constant) {
        p.bound = value(*results[i], resolve, peer);
      } else if (p.constant) {
        auto c = p.constant->unknown ? resolved(*p.constant, type) : *p.constant;
        p.bound = fixed(std::move(c.value), c.type);
      }
      if (type.kind == SqlKind::Decimal) rescalable(*p.bound, *p.bound, decimal_scale(type), results[i]->pos);
    }

    if (e.kind == K::Coalesce) {
      if (parts.size() == 1) return std::move(*parts[0].bound);
      for (auto& p : parts) args.push_back(std::move(*p.bound));
      auto out = logical::expr(logical::Expr::Kind::Coalesce, type);
      out.args = std::move(args);
      return out;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i < bound_conds.size()) args.push_back(std::move(bound_conds[i]));
      args.push_back(std::move(*parts[i].bound));
    }
    auto out = logical::expr(logical::Expr::Kind::Case, type);
    out.args = std::move(args);
    return out;
  }

  // The two operands of an operator. One a $n or an unknown literal takes
  // its type from the other, which is bound first; `settled_type`, when
  // given, sees that type (and whether the untyped operand is the left one)
  // before the untyped operand is read as it, to refuse an operator the
  // reading would be wrong for.
  auto operands(const ast::Expr& lhs, const ast::Expr& rhs, const Resolve& resolve, Use use,
                const std::function<void(SqlType, bool)>& settled_type = {}) -> std::pair<logical::Expr, logical::Expr> {
    auto settled = [](const ast::Expr& x) { return x.kind != ast::Expr::Kind::Param && !is_constant(x); };
    auto peer_of = [&](const logical::Expr& bound, const ast::Expr& written) {
      Peer p{bound.type, nullptr, nullptr, use};
      if (written.kind == ast::Expr::Kind::Column) std::tie(p.table, p.column) = resolve(written);
      return p;
    };
    if (!settled(lhs) && !settled(rhs)) not_yet("a parameter or a constant with no column beside it to give it a type", lhs.pos);
    if (settled(lhs) && settled(rhs)) {
      auto l = value(lhs, resolve);
      return {std::move(l), value(rhs, resolve)};
    }
    if (settled(lhs)) {
      auto l = value(lhs, resolve);
      if (settled_type) settled_type(l.type, false);
      auto r = value(rhs, resolve, peer_of(l, lhs));
      return {std::move(l), std::move(r)};
    }
    auto r = value(rhs, resolve);
    if (settled_type) settled_type(r.type, true);
    auto l = value(lhs, resolve, peer_of(r, rhs));
    return {std::move(l), std::move(r)};
  }

  // The digits an operand can hold, for the rescaling an operator does to
  // meet the finer scale of its other operand: it must stay within the 18
  // digits a DECIMAL holds. A constant's are those of its value.
  auto digits_of(const logical::Expr& x) const -> int {
    if (x.kind == logical::Expr::Kind::Param && is_fixed(x.param)) {
      const auto* v = std::get_if<std::int64_t>(&fixed_value(x.param));
      auto digits = 0;
      for (auto m = v == nullptr ? std::int64_t{0} : *v; m != 0; m /= 10) ++digits;
      return digits;
    }
    switch (x.type.kind) {
      case SqlKind::SmallInt: return 5;
      case SqlKind::Integer: return 10;
      case SqlKind::BigInt: return 19;
      case SqlKind::Decimal: return x.type.precision == 0 ? int{kMaxDecimalPrecision} : static_cast<int>(x.type.precision);
      case SqlKind::Real:
      case SqlKind::Double:
      case SqlKind::Boolean:
      case SqlKind::Text:
      case SqlKind::Varchar:
      case SqlKind::Char:
      case SqlKind::Name:
      case SqlKind::Bytea:
      case SqlKind::Timestamp:
      case SqlKind::TimestampTz:
      case SqlKind::Date:
      case SqlKind::Time:
      case SqlKind::TimeTz:
      case SqlKind::Numeric: break;
    }
    return 0;
  }

  // Both operands brought to scale `scale` by multiplying by a power of
  // ten, which must not leave the 18 digits.
  void rescalable(const logical::Expr& x, const logical::Expr& y, std::uint8_t scale, std::int32_t pos) const {
    for (const auto* t : {&x, &y}) {
      auto up = scale - decimal_scale(t->type);
      if (up > 0 && digits_of(*t) + up > static_cast<int>(kMaxDecimalPrecision)) not_yet("a numeric of more than 18 digits", pos);
    }
  }

  auto arithmetic_value(const ast::Expr& e, const Resolve& resolve) -> logical::Expr {
    using K = ast::Expr::Kind;
    auto op = e.kind == K::Add ? Arith::Add : e.kind == K::Sub ? Arith::Sub : e.kind == K::Mul ? Arith::Mul : Arith::Div;
    auto untyped = [&](const ast::Expr& x) { return x.kind == K::Param || (is_constant(x) && fold(x).unknown); };
    // Decided before a $n or an unknown literal is read as the date or time
    // type beside it, which PostgreSQL does only for -: a literal that does
    // not parse as that type must not answer 22007 for an operator refused.
    auto untyped_beside_temporal = [&](SqlType t, bool untyped_left) {
      if (is_temporal(t) && untyped(untyped_left ? *e.lhs : *e.rhs)) temporal_untyped_operator(op, t, untyped_left, e.pos);
    };
    auto [l, r] = operands(*e.lhs, *e.rhs, resolve, Use::Operand, untyped_beside_temporal);
    if (is_temporal(l.type) || is_temporal(r.type)) {
      temporal_operator(op, l.type, r.type, e.pos);  // returns for DATE - DATE only
      auto days = *arith_type(op, l.type, r.type);
      return logical::binary(logical::Expr::Kind::Sub, std::move(l), std::move(r), days);
    }
    numeric_computed(l.type, r.type, e.pos);
    for (const auto* x : {&l, &r}) {
      if (is_float(x->type)) not_yet("arithmetic of REAL and DOUBLE PRECISION", e.pos);
      if (!is_number(x->type)) {
        throw SqlError{"operator does not exist: " + pg_type_name(l.type) + " " + std::string{arith_text(op)} + " " +
                           pg_type_name(r.type),
                       e.pos, kUndefinedFunction};
      }
    }
    auto type = arith_type(op, l.type, r.type);
    if (!type) {
      not_yet("the quotient of a numeric other than constants: its scale depends on the values (PostgreSQL's select_div_scale)",
              e.pos);
    }
    if (type->kind == SqlKind::Decimal && type->scale > kMaxDecimalPrecision) not_yet("a numeric of more than 18 digits", e.pos);
    if (op == Arith::Add || op == Arith::Sub) rescalable(l, r, decimal_scale(*type), e.pos);
    auto kind = op == Arith::Add   ? logical::Expr::Kind::Add
                : op == Arith::Sub ? logical::Expr::Kind::Sub
                : op == Arith::Mul ? logical::Expr::Kind::Mul
                                   : logical::Expr::Kind::Div;
    return logical::binary(kind, std::move(l), std::move(r), *type);
  }

  // A comparison as a value: of operands of one class (42883 otherwise), as
  // a WHERE's, with <> as NOT of =. Numbers meet at the finer scale.
  auto comparison_value(const ast::Expr& e, const Resolve& resolve) -> logical::Expr {
    auto [l, r] = operands(*e.lhs, *e.rhs, resolve, Use::Compared);
    if (type_class(l.type.kind) != type_class(r.type.kind)) {
      throw SqlError{"operator does not exist: " + pg_type_name(l.type) + " " + std::string{value_op_text(e.cmp)} + " " +
                         pg_type_name(r.type),
                     e.pos, kUndefinedFunction};
    }
    numeric_compared(l.type, r.type, e.pos);
    if (is_float(l.type) || is_float(r.type)) not_yet("a comparison of REAL and DOUBLE PRECISION as a value", e.pos);
    if (type_class(l.type.kind) == TypeClass::Temporal && l.type.kind != r.type.kind) {
      // LOCALTIME beside a time with time zone is read as one, as PostgreSQL
      // casts it: the same reading at the session's zone.
      if (!(clock_as_timetz(l, r) || clock_as_timetz(r, l))) {
        not_yet("a comparison of different date and time types as a value", e.pos);
      }
    }
    if (is_number(l.type)) rescalable(l, r, std::max(decimal_scale(l.type), decimal_scale(r.type)), e.pos);
    using Op = ast::Compare::Op;
    auto op = [&] {
      switch (e.cmp) {
        case ast::ValueCompare::Eq:
        case ast::ValueCompare::Ne: return Op::Eq;
        case ast::ValueCompare::Lt: return Op::Lt;
        case ast::ValueCompare::Le: return Op::Le;
        case ast::ValueCompare::Gt: return Op::Gt;
        case ast::ValueCompare::Ge: return Op::Ge;
      }
      return Op::Eq;
    }();
    auto compared = logical::compare(op, std::move(l), std::move(r));
    if (e.cmp != ast::ValueCompare::Ne) return compared;
    return logical::predicate_of(logical::Expr::Kind::Not, {std::move(compared)});
  }

  // AND, OR and NOT of BOOLEAN operands (42804 otherwise).
  auto boolean_value(const ast::Expr& e, const Resolve& resolve) -> logical::Expr {
    using K = ast::Expr::Kind;
    auto word = e.kind == K::Not ? "NOT" : e.kind == K::And ? "AND" : "OR";
    std::vector<logical::Expr> args;
    for (const auto& a : e.args) {
      auto v = value(a, resolve, Peer{SqlType{SqlKind::Boolean}, nullptr, nullptr, Use::Compared});
      if (v.type.kind != SqlKind::Boolean) {
        throw SqlError{std::string{"argument of "} + word + " must be type boolean, not type " + pg_type_name(v.type), a.pos,
                       kDatatypeMismatch};
      }
      args.push_back(std::move(v));
    }
    auto kind = e.kind == K::Not ? logical::Expr::Kind::Not : e.kind == K::And ? logical::Expr::Kind::And : logical::Expr::Kind::Or;
    return logical::predicate_of(kind, std::move(args));
  }

  // The condition of a CHECK constraint, from its text, over the columns of
  // `t`: a BOOLEAN value of columns and constants, as a select list gives
  // one. What a write cannot evaluate over the row it has built (a
  // parameter, a subquery, the clock, an aggregate) is refused.
  auto check(const Table& t, const std::string& text, std::int32_t pos) -> logical::Expr {
    auto stmt = ast::parse("SELECT (" + text + "\n)");
    const auto* q = std::get_if<ast::Select>(&stmt);
    if (q == nullptr || q->targets.size() != 1 || !q->from.empty() || !q->where.empty() || q->set) {
      throw SqlError{"syntax error in the CHECK constraint", pos};
    }
    refuse_in_check(q->targets[0]);
    const Table* rels[] = {&t};
    auto e = value(q->targets[0], [&](const ast::Expr& c) { return std::pair{&t, column(rels, c).second}; });
    if (e.type.kind != SqlKind::Boolean) {
      throw SqlError{"argument of CHECK must be type boolean, not type " + pg_type_name(e.type), pos, kDatatypeMismatch};
    }
    return e;
  }

  // An expression of a procedure's body over `frame`, the variables
  // (bind_procedure_expr): a value, or, with `target`, one assigned to it.
  auto frame_value(const Table& frame, const std::string& text, const Column* target) -> logical::Expr {
    auto stmt = ast::parse("SELECT (" + text + "\n)");
    const auto* q = std::get_if<ast::Select>(&stmt);
    if (q == nullptr || q->targets.size() != 1 || !q->from.empty() || !q->where.empty() || q->set) {
      throw SqlError{"syntax error in the expression " + text, -1};
    }
    refuse_in_procedure(q->targets[0]);
    const Table* rels[] = {&frame};
    if (target != nullptr && is_constant(q->targets[0])) return value(frame, q->targets[0], *target);
    return value(q->targets[0], [&](const ast::Expr& c) { return std::pair{&frame, column(rels, c).second}; });
  }

  static void refuse_in_procedure(const ast::Expr& e) {
    using K = ast::Expr::Kind;
    switch (e.kind) {
      case K::Param: throw SqlError{"there is no parameter $" + std::to_string(e.param + 1), e.pos, kUndefinedParameter};
      case K::Subquery: not_yet("a subquery in a procedure's expression", e.pos);
      case K::Clock: refuse_clock(e, "in a procedure's expression");
      case K::Aggregate: throw SqlError{"aggregate functions are not allowed here", e.pos, kGroupingError};
      case K::Star: throw SqlError{"syntax error at or near \"*\"", e.pos};
      case K::Column:
      case K::Const:
      case K::Literal:
      case K::Add:
      case K::Sub:
      case K::Mul:
      case K::Div:
      case K::Plus:
      case K::Neg:
      case K::Compare:
      case K::IsNull:
      case K::Not:
      case K::And:
      case K::Or:
      case K::Cast:
      case K::Case:
      case K::Coalesce:
      case K::NullIf:
      case K::Func: break;
      case K::Default: throw SqlError{"DEFAULT is not allowed in this context", e.pos};
    }
    if (e.lhs) refuse_in_procedure(*e.lhs);
    if (e.rhs) refuse_in_procedure(*e.rhs);
    for (const auto& a : e.args) refuse_in_procedure(a);
  }

  static void refuse_in_check(const ast::Expr& e) {
    using K = ast::Expr::Kind;
    switch (e.kind) {
      case K::Param: throw SqlError{"there is no parameter $" + std::to_string(e.param + 1) + " in a CHECK constraint", e.pos, kUndefinedParameter};
      case K::Subquery: not_yet("a subquery in a CHECK constraint", e.pos);
      case K::Clock: refuse_clock(e, "in a CHECK constraint");
      case K::Aggregate: throw SqlError{"aggregate functions are not allowed in check constraints", e.pos, kGroupingError};
      case K::Star: throw SqlError{"syntax error at or near \"*\"", e.pos};
      case K::Column:
      case K::Const:
      case K::Literal:
      case K::Add:
      case K::Sub:
      case K::Mul:
      case K::Div:
      case K::Plus:
      case K::Neg:
      case K::Compare:
      case K::IsNull:
      case K::Not:
      case K::And:
      case K::Or:
      case K::Cast:
      case K::Case:
      case K::Coalesce:
      case K::NullIf:
      case K::Func: break;
    }
    if (e.lhs) refuse_in_check(*e.lhs);
    if (e.rhs) refuse_in_check(*e.rhs);
    for (const auto& a : e.args) refuse_in_check(a);
  }

  // The table's CHECK constraints, bound for a write to it.
  auto checks(const Table& t) -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
    for (const auto& c : t.checks) out.push_back(check(t, c.expr, -1));
    return out;
  }

  // The table an UPDATE or DELETE writes; a view is refused (#678).
  auto updated_table(const std::string& name, std::int32_t pos) const -> const Table* {
    if (cat_.table(name) == nullptr && cat_.view(name) != nullptr) {
      throw SqlError{"\"" + name + "\" is a view: UPDATE and DELETE through a view are not supported yet (#678)", pos,
                     kFeatureNotSupported};
    }
    return table(name, pos);
  }

  auto table(const std::string& name, std::int32_t pos) const -> const Table* {
    const auto* t = cat_.table(name);
    if (t == nullptr && cat_.view(name) != nullptr) {
      throw SqlError{"\"" + name + "\" is a view, not a table", pos, kFeatureNotSupported};
    }
    if (t == nullptr) throw SqlError{"relation \"" + name + "\" does not exist", pos, kUndefinedTable};
    return t;
  }

  static auto column(const Table& t, const std::string& name, std::int32_t pos) -> const Column* {
    const auto* c = t.column(name);
    if (c == nullptr) throw SqlError{"column \"" + name + "\" does not exist", pos, kUndefinedColumn};
    return c;
  }

  // A column reference of a one-table statement: qualified by that table's
  // name, or not at all.
  static auto column(const Table& t, const ast::Expr& e) -> const Column* {
    if (!e.table.empty() && e.table != t.name) {
      throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
    }
    return column(t, e.column, e.pos);
  }

  // The FROM items before the part of a FROM an ON reads, and the names
  // the statement gives them: what PostgreSQL names where the ON reads a
  // column only they have (column).
  struct Hidden {
    std::span<const Table* const> rels;
    std::span<const std::string> names;
  };

  // A column reference of a join: the relation whose name it carries, or,
  // unqualified, the column of that name among the join's columns
  // (JoinScope) of the relations in scope, a USING or NATURAL column one
  // column; a name more than one has is ambiguous (42702). A USING's alias
  // names that join's USING columns alone.
  //
  // `rels` is a prefix of the FROM items, or, for an ON in parentheses or
  // after a comma, the items of its own join, the FROM items before them
  // `hidden`: a name only those have is PostgreSQL's 42703 with its DETAIL.
  // Such a join has no USING (the parser refuses it), so its columns are
  // its relations'.
  auto column(std::span<const Table* const> rels, const ast::Expr& e, const Hidden& hidden = {}) const
      -> std::pair<std::uint32_t, const Column*> {
    const auto* level = hidden.rels.empty() ? joins_.level(rels.size()) : nullptr;
    if (level != nullptr) {
      auto at = [&](const JoinColumn& j) -> std::pair<std::uint32_t, const Column*> {
        auto side = std::ranges::find(rels, j.table);
        if (side == rels.end()) throw std::logic_error{"a join column of a relation out of scope"};
        return {static_cast<std::uint32_t>(side - rels.begin()), j.col};
      };
      for (const auto& alias : joins_.aliases) {
        if (alias.join >= rels.size() || e.table != alias.name) continue;
        auto it = std::ranges::find(alias.columns, e.column, &JoinColumn::name);
        if (it == alias.columns.end()) {
          throw SqlError{"column " + e.table + "." + e.column + " does not exist", e.pos, kUndefinedColumn};
        }
        return at(*it);
      }
      if (e.table.empty()) {
        auto named = [&](const JoinColumn& j) { return j.name == e.column; };
        auto n = std::ranges::count_if(*level, named);
        if (n > 1) throw SqlError{"column reference \"" + e.column + "\" is ambiguous", e.pos, kAmbiguousColumn};
        if (n == 1) return at(*std::ranges::find_if(*level, named));
      }
    }
    std::optional<std::pair<std::uint32_t, const Column*>> found;
    for (std::uint32_t side = 0; side < rels.size(); ++side) {
      if (!e.table.empty() && e.table != rels[side]->name) continue;
      const auto* c = rels[side]->column(e.column);
      if (c == nullptr) continue;
      if (found) throw SqlError{"column reference \"" + e.column + "\" is ambiguous", e.pos, kAmbiguousColumn};
      found = {side, c};
    }
    if (found) return *found;
    if (!e.table.empty() && std::ranges::none_of(rels, [&](const Table* t) { return t->name == e.table; })) {
      throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
    }
    SqlError missing{"column \"" + e.column + "\" does not exist", e.pos, kUndefinedColumn};
    std::vector<std::size_t> out_of_reach;
    for (std::size_t i = 0; e.table.empty() && i < hidden.rels.size(); ++i) {
      if (hidden.rels[i]->column(e.column) != nullptr) out_of_reach.push_back(i);
    }
    if (out_of_reach.size() == 1) {
      throw std::move(missing).with_notes("There is a column named \"" + e.column + "\" in table \"" +
                                              hidden.names[out_of_reach[0]] +
                                              "\", but it cannot be referenced from this part of the query.",
                                          "");
    }
    if (out_of_reach.size() > 1) {
      throw std::move(missing).with_notes("There are columns named \"" + e.column +
                                              "\", but they are in tables that cannot be referenced from this part of the query.",
                                          "Try using a table-qualified name.");
    }
    throw missing;
  }

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
    // A CHAR(n) compares without its trailing spaces: a parameter would need
    // them cut at run time, as a constant is cut here.
    if (col.sql.kind == SqlKind::Char && use != Use::Assigned) not_yet("a parameter compared with a CHAR(n) column", pos);
    if (col.sql.kind == SqlKind::Numeric && use != Use::Assigned) refuse_numeric("a parameter compared or computed with a NUMERIC", pos);
    if (params_.size() <= n) params_.resize(n + 1);
    auto& p = params_[n];
    if (n < fixed_params_ && p.first) {
      const auto& fixed = p.first->col->sql;
      // A fixed integer type used with an integer column of another width:
      // PostgreSQL's cross-type operator or assignment cast. Its value is
      // checked as its own type before the plan runs, as PostgreSQL's Bind
      // reads it as that type, however it is used.
      auto integral = [](SqlKind k) { return k == SqlKind::SmallInt || k == SqlKind::Integer || k == SqlKind::BigInt; };
      if (p.first->phys == t && integral(fixed.kind) && integral(col.sql.kind) && fixed.kind != col.sql.kind) {
        if (use != Use::Assigned) p.assigned_only = false;
        narrow(p, compared_type(fixed).range, table);
        return;
      }
      if (p.first->phys != t || compared_type(fixed).identity != compared_type(col.sql).identity) {
        fixed_misfit(n, fixed, col, use, pos);
      }
    }
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
          case Use::Operand: return "combined with ";
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

  // A $n whose fixed type (fix_params) does not fit its use with `col`. Where
  // PostgreSQL has no operator or assignment cast either, its SQLSTATE:
  // 42883, or 42804 for an assignment, in its words. Where it converts the
  // value (an integer compared with a DOUBLE PRECISION, any value assigned to
  // a string), the layer, which would have to convert it before the plan
  // runs, does not yet: 42601 (docs/frontend_design.md, *Fixed parameter
  // types*).
  [[noreturn]] static void fixed_misfit(std::uint32_t n, SqlType fixed, const Column& col, Use use, std::int32_t pos) {
    auto param = "parameter $" + std::to_string(n + 1) + " of type " + sql_type_name(fixed);
    auto assigned = use == Use::Assigned;
    auto refusal = type_refusal(fixed.kind, col.sql.kind, assigned);
    if (refusal && assigned) {
      throw SqlError{"column \"" + col.name + "\" is of type " + sql_type_name(col.sql) + " but expression is of type " +
                         sql_type_name(fixed),
                     pos, *refusal};
    }
    auto with = (assigned ? " assigned to " : " used with ") + col.name + " (" + sql_type_name(col.sql) + ")";
    if (refusal) throw SqlError{param + " cannot be" + with, pos, *refusal};
    throw SqlError{param + with + " is not supported yet", pos};
  }

  // UPDATE's SET col = $n: $n is also checked before the plan runs, against
  // col's whole type, as PostgreSQL coerces it at bind and so refuses one
  // outside the type even when no row matches. An INSERT writes its row
  // whatever its parameters, and the row builder checks them in column
  // order, as PostgreSQL does.
  void check_assigned(std::uint32_t n, const Column& col, std::uint32_t table) { narrow(params_[n], col.sql, table); }

  // Whether the layer takes `fn` of a value of type `t`, and if not the SQLSTATE of the
  // refusal: 42883 where PostgreSQL has no such aggregate either, 42601 where
  // it has and the layer does not yet (SUM, AVG, MIN or MAX of a REAL, SUM or
  // AVG of a TIME column, which PostgreSQL takes as an interval; MIN or MAX
  // of a string column).
  struct AggregateSupport {
    bool supported;
    SqlState sqlstate;
  };
  static auto aggregate_support(ast::Expr::Fn fn, SqlType t) -> AggregateSupport {
    using Fn = ast::Expr::Fn;
    if (fn == Fn::Count) return {true, kSyntaxError};
    auto sum = fn == Fn::Sum || fn == Fn::Avg;
    switch (t.kind) {
      case SqlKind::SmallInt:
      case SqlKind::Integer:
      case SqlKind::BigInt:
      case SqlKind::Decimal: return {true, kSyntaxError};
      case SqlKind::Double: return {true, kSyntaxError};
      case SqlKind::Real: return {false, kSyntaxError};
      case SqlKind::Text:
      case SqlKind::Varchar:
      case SqlKind::Char:
      case SqlKind::Name: return {false, sum ? kUndefinedFunction : kSyntaxError};
      case SqlKind::Boolean:
      case SqlKind::Bytea: return {false, kUndefinedFunction};
      case SqlKind::Timestamp:
      case SqlKind::TimestampTz:
      case SqlKind::Date:
      case SqlKind::TimeTz: return {!sum, kUndefinedFunction};
      case SqlKind::Time: return {!sum, kSyntaxError};
      case SqlKind::Numeric: return {false, kFeatureNotSupported};  // a column's text: refuse_numeric
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

  // An aggregate call's argument, bound as a select item's expression is
  // (plain_output): a column, or an expression of the FROM columns evaluated
  // per row. none for COUNT(*). An aggregate inside it is 42803, as
  // PostgreSQL refuses it; DISTINCT of other than a column is not taken yet
  // (0A000).
  auto aggregate_arg(const ast::Expr& e, const Resolve& resolve) -> std::optional<logical::Expr> {
    if (e.star) return std::nullopt;
    const auto& a = e.args.at(0);
    std::function<const ast::Expr*(const ast::Expr&)> inner = [&](const ast::Expr& x) -> const ast::Expr* {
      if (x.kind == ast::Expr::Kind::Aggregate) return &x;
      for (const auto* side : {x.lhs.get(), x.rhs.get()}) {
        if (side == nullptr) continue;
        if (const auto* found = inner(*side)) return found;
      }
      for (const auto& arg : x.args) {
        if (const auto* found = inner(arg)) return found;
      }
      return nullptr;
    };
    if (const auto* nested = inner(a)) throw SqlError{"aggregate function calls cannot be nested", nested->pos, kGroupingError};
    // A $n nothing typed, or a NULL or quoted literal not under a cast, is of
    // unknown type: PostgreSQL finds no one SUM or AVG of unknown, and no type
    // for COUNT's $n (COUNT of such a literal counts it as text does).
    const auto untyped_param = a.kind == ast::Expr::Kind::Param && (params_.size() <= a.param || !params_[a.param].first);
    const auto untyped_literal =
        a.kind == ast::Expr::Kind::Literal && (a.literal.kind == ast::Literal::Kind::Null || a.literal.kind == ast::Literal::Kind::String);
    if (untyped_param || untyped_literal) {
      if (untyped_param && e.fn == ast::Expr::Fn::Count) {
        throw SqlError{"could not determine data type of parameter $" + std::to_string(a.param + 1), a.pos, kIndeterminateDatatype};
      }
      if (e.fn == ast::Expr::Fn::Sum || e.fn == ast::Expr::Fn::Avg) {
        throw SqlError{std::string{"function "} + (e.fn == ast::Expr::Fn::Sum ? "sum" : "avg") + "(unknown) is not unique", e.pos,
                       kAmbiguousFunction};
      }
    }
    auto arg = plain_output(a, resolve).expr;
    if (e.distinct && a.kind != ast::Expr::Kind::Column) not_supported("an aggregate of DISTINCT values of an expression", e.pos);
    return arg;
  }

  // An aggregate call of `arg`, or COUNT(*) with none: a column, or an
  // expression of columns, whose type decides the call's. SUM, MIN, MAX and
  // AVG take a value the plan holds as int64_t: SUM and AVG an integer or
  // DECIMAL one, as PostgreSQL sums them exactly; MIN and MAX those and the
  // date and time types; and each a DOUBLE PRECISION one. Of a REAL they are
  // later, and MIN and MAX of a string or bytes value outside the MVP
  // (docs/frontend_design.md, *Aggregates*).
  //
  // Its result as PostgreSQL names and types it: COUNT a bigint; SUM of a
  // SMALLINT or INTEGER a bigint, of a BIGINT or DECIMAL a numeric of no
  // declared precision (the total may take 19 digits), held at the
  // argument's scale; MIN and MAX of the argument's type; AVG a numeric of no
  // declared precision, its scale PostgreSQL's division's (the API computes
  // it from the plan's sum and count, docs/planner_design.md, *Aggregates*).
  static auto aggregate(std::optional<logical::Expr> arg, const ast::Expr& e) -> logical::Output {
    using Fn = ast::Expr::Fn;
    auto agg = logical::expr(logical::Expr::Kind::Aggregate, SqlType{SqlKind::BigInt});
    // MIN and MAX of the distinct values are those of all of them.
    agg.distinct = e.distinct && e.fn != Fn::Min && e.fn != Fn::Max;
    switch (e.fn) {
      case Fn::Count: agg.fn = logical::Expr::Fn::Count; break;
      case Fn::Sum: agg.fn = logical::Expr::Fn::Sum; break;
      case Fn::Min: agg.fn = logical::Expr::Fn::Min; break;
      case Fn::Max: agg.fn = logical::Expr::Fn::Max; break;
      case Fn::Avg: agg.fn = logical::Expr::Fn::Avg; break;
    }
    if (!arg) return {std::move(agg), "count"};
    const auto t = arg->type;
    if (t.kind == SqlKind::Numeric && (e.fn != Fn::Count || agg.distinct)) {
      refuse_numeric(std::string{aggregate_name(e.fn)} + (agg.distinct ? " of DISTINCT values" : "") + " of a NUMERIC", e.pos);
    }
    if (auto s = aggregate_support(e.fn, t); !s.supported) {
      auto of = arg->kind == logical::Expr::Kind::Column ? " column" : " value";
      throw SqlError{std::string{aggregate_name(e.fn)} + " of a " + sql_type_name(t) + of + " is not supported", e.pos,
                     s.sqlstate};
    }
    agg.args.push_back(std::move(*arg));
    switch (e.fn) {
      case Fn::Count: return {std::move(agg), "count"};
      case Fn::Sum:
        if (t.kind == SqlKind::Double) {
          agg.type = t;
        } else if (t.kind != SqlKind::SmallInt && t.kind != SqlKind::Integer) {
          agg.type = SqlType{SqlKind::Decimal, 0, 0, t.scale};
        }
        return {std::move(agg), "sum"};
      case Fn::Min: agg.type = t; return {std::move(agg), "min"};
      case Fn::Max: agg.type = t; return {std::move(agg), "max"};
      case Fn::Avg:
        agg.type = t.kind == SqlKind::Double ? t : SqlType{SqlKind::Numeric, 0, 0, t.scale};
        return {std::move(agg), "avg"};
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
    // A number literal assigned to a NUMERIC: read as written, of any digits.
    if (target.sql.kind == SqlKind::Numeric && e.kind == ast::Expr::Kind::Literal &&
        (e.literal.kind == ast::Literal::Kind::Integer || e.literal.kind == ast::Literal::Kind::Number)) {
      return fixed(numeric_bytes(e.literal.text, e.pos), target.sql);
    }
    if (is_constant(e)) {
      // Out of the column's type, it is refused here, before anything runs,
      // as PostgreSQL's planner folds its assignment cast: so of several the
      // first in the statement's order, as there.
      auto k = fold(e);
      auto v = coerce(k, target, true, "=");
      if (auto why = out_of_range(target.sql, v)) out_of_type(*why, k.pos);
      return fixed(std::move(v), target.sql);
    }
    // $n::T assigned to a column of type T binds as $n, as in a WHERE.
    if (param_cast(e)) return value(t, *uncast_param(&e, target), target);
    switch (e.kind) {
      case ast::Expr::Kind::Param:
        use_param(e.param, want, e.pos, target, t.id, Use::Assigned);
        return logical::param(e.param, target.sql);
      case ast::Expr::Kind::Column: {
        const auto* c = column(t, e);
        if ((c->sql.kind == SqlKind::Numeric) != (target.sql.kind == SqlKind::Numeric) &&
            !type_refusal(c->sql.kind, target.sql.kind, true)) {
          refuse_numeric("assigning a column of type " + pg_type_name(c->sql) + " to one of type " + pg_type_name(target.sql), e.pos);
        }
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
        if (c->sql.kind == SqlKind::Numeric || target.sql.kind == SqlKind::Numeric) refuse_numeric(std::string{"operator "} + op + " of a NUMERIC", e.pos);
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
      case ast::Expr::Kind::IsNull:
      case ast::Expr::Kind::Not:
      case ast::Expr::Kind::And:
      case ast::Expr::Kind::Or: not_yet("IS NULL, AND, OR and NOT of other than constants as a value", e.pos);
      case ast::Expr::Kind::Cast: not_yet("a cast of other than a constant or a parameter", e.pos);
      case ast::Expr::Kind::Clock: {
        auto d = clock_default(e.func, e.constant, target.sql, target.name, e.pos, true);
        return clock_reading(d, target);
      }
      case ast::Expr::Kind::Subquery: not_yet("a subquery assigned to a column", e.pos);
      case ast::Expr::Kind::Default:
        if (target.clock_default) return clock_reading(*target.clock_default, target);
        return fixed(target.default_value, target.sql);
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
    auto was = std::exchange(in_where_, true);
    for (const auto* w : top_level(in)) {
      out.push_back(predicate(*w, [&](const ast::Expr& e) { return std::pair{t.id, column(t, e)}; }));
    }
    in_where_ = was;
    return out;
  }

  // WHERE CURRENT OF: `pk = $n` for each primary key column of `t`, the
  // parameters from `first` (zero-based) on. The hidden row id of a table
  // without a primary key has no name a statement can write, so the column
  // is resolved by the lambda, not by its name.
  auto current_of(const Table& t, std::uint32_t first, std::int32_t pos) -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
    for (auto id : t.pk_col_ids) {
      const auto* col = t.column(id);
      ast::Compare w{ast::Compare::Op::Eq, ast::Expr{ast::Expr::Kind::Column, col->name, 0, nullptr, nullptr, pos},
                     ast::Expr{ast::Expr::Kind::Param, {}, first + bytecask::sql::narrow<std::uint32_t>(out.size()), nullptr, nullptr, pos},
                     pos};
      out.push_back(predicate(w, [&](const ast::Expr&) { return std::pair{t.id, col}; }));
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
      throw SqlError{"operator does not exist: " + pg_type_name(c->sql) + (w.fold ? " ~~* unknown" : " ~~ unknown"), w.pos,
                     kUndefinedFunction};
    }
    synthetic_.push_back(Column{0, c->name, SqlType{SqlKind::Text}, PhysType::Bytes, true, {}});
    const auto& text = synthetic_.back();
    logical::Expr pattern;
    if (w.rhs.kind == ast::Expr::Kind::Param) {
      use_param(w.rhs.param, PhysType::Bytes, w.rhs.pos, text, table, Use::Compared);
      pattern = logical::param(w.rhs.param, text.sql);
    } else if (is_constant(w.rhs)) {
      pattern = fixed(coerce(fold(w.rhs), text, false, w.fold ? "~~*" : "~~"), text.sql);
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
    e.fold = w.fold;
    // ESCAPE $n: text, checked as an escape when the plan runs.
    if (w.escape_param) {
      use_param(*w.escape_param, PhysType::Bytes, w.pos, text, table, Use::Compared);
      e.args.push_back(logical::param(*w.escape_param, text.sql));
    }
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
      case K::Not:
        if (null_constant(w.args[0])) not_yet("NULL under NOT in a predicate", w.pos);
        return tree(logical::Expr::Kind::Not);
      // TRUE or FALSE. A NULL is never true, so as a filter it is FALSE
      // wherever no NOT is above it.
      case K::Const: return logical::constant(w.truth.value_or(false) ? 1 : 0, SqlType{SqlKind::Boolean});
    }
    std::unreachable();
  }

  // Whether predicate c holds a NULL constant.
  static auto null_constant(const ast::Compare& c) -> bool {
    if (c.kind == ast::Compare::Kind::Const) return !c.truth.has_value();
    return std::ranges::any_of(c.args, [](const ast::Compare& a) { return null_constant(a); });
  }

  // A comparison's operand other than a column: a parameter, or $n ± an
  // integer literal.
  static auto operand(const ast::Expr& e) -> bool {
    using K = ast::Expr::Kind;
    if (e.kind == K::Param) return true;
    return (e.kind == K::Add || e.kind == K::Sub) && e.lhs->kind == K::Param && e.rhs->kind == K::Const;
  }

  static auto param_cast(const ast::Expr& e) -> bool {
    return e.kind == ast::Expr::Kind::Cast && e.rhs->kind == ast::Expr::Kind::Param;
  }

  // CAST($n AS T) beside a column of type T is $n: PostgreSQL types an
  // untyped $n by its cast, and a cast of a value to its own type changes
  // nothing. A cast to another type is not taken yet, nor one to DECIMAL,
  // VARCHAR(n) or CHAR(n), whose casts round, cut or pad to the modifier.
  static auto uncast_param(const ast::Expr* e, const Column& c) -> const ast::Expr* {
    if (!param_cast(*e)) return e;
    if (c.sql.kind == SqlKind::Numeric && e->cast_type == "numeric" && e->cast_typmods.empty()) return e->rhs.get();
    if (modified_cast(*e) || cast_type(*e).kind != c.sql.kind) not_yet("a cast of a parameter to other than its column's type", e->pos);
    return e->rhs.get();
  }

  // A cast to DECIMAL, VARCHAR(n) or CHAR(n), which rounds, cuts or pads
  // to the modifier: a parameter cast to one is not taken yet.
  static auto modified_cast(const ast::Expr& e) -> bool {
    return e.cast_type == "numeric" || e.cast_type == "bpchar" || (e.cast_type == "varchar" && !e.cast_typmods.empty());
  }

  static auto cast_type(const ast::Expr& e) -> SqlType {
    return sql_type(ast::ColumnDefAst{"", e.cast_type, e.cast_typmods, false, false, std::nullopt, e.pos, {}});
  }

  // One WHERE conjunct as column <op> operand, the column resolved by
  // `resolve`. In $n ± k, $n and the sum are INTEGERs, as PostgreSQL types
  // an unknown $n beside an integer literal; the column compared with them
  // is an INTEGER, so the sum is compared as one, and k must fit one.
  auto compare(const ast::Compare& w, const auto& resolve) -> logical::Expr {
    const auto* col = &w.lhs;
    const auto* rhs = &w.rhs;
    auto op = w.op;
    if ((operand(*col) || param_cast(*col) || is_constant(*col) || col->kind == ast::Expr::Kind::Subquery ||
         col->kind == ast::Expr::Kind::Clock) &&
        rhs->kind == ast::Expr::Kind::Column) {
      std::swap(col, rhs);
      op = mirrored(op);
    }
    // In a correlated subquery, its column against a column of the query
    // around it: the outer one is a parameter the outer row sets
    // (correlation). Checked as two columns are, in the order written.
    if (correlating_ && col->kind == ast::Expr::Kind::Column && rhs->kind == ast::Expr::Kind::Column) {
      const auto& q = *correlating_->query;
      auto lo = outer_ref(q, correlating_->outer, *col);
      if (lo || outer_ref(q, correlating_->outer, *rhs)) {
        const auto* outer_table = correlating_->outer[0];
        auto type_of = [&](const ast::Expr& e, bool is_outer) {
          return is_outer ? column(*outer_table, e)->sql : resolve(e).second->sql;
        };
        no_operator(type_of(*col, lo), written_op(w, op), type_of(*rhs, !lo), w.pos);
        if (lo) {
          std::swap(col, rhs);
          op = mirrored(op);
        }
        auto [table, c] = resolve(*col);
        return logical::compare(op, logical::column_of(table, *c), outer_param(*rhs, *c, w.pos));
      }
    }
    if (col->kind == ast::Expr::Kind::Column && param_cast(*rhs)) rhs = uncast_param(rhs, *resolve(*col).second);
    // A column of one relation against a column of another, inside AND, OR or
    // NOT: <> is NOT =. Two columns of one relation: a filter of its rows.
    if (col->kind == ast::Expr::Kind::Column && rhs->kind == ast::Expr::Kind::Column) {
      auto [lt, lc] = resolve(*col);
      auto [rt, rc] = resolve(*rhs);
      if (lt != rt) return joined_compare(op, written_op(w, op), lt, *lc, rt, *rc, w.pos);
      return columns_compare(w, op, lt, *lc, *rc);
    }
    // A scalar subquery: its value, a parameter it sets before the plan runs.
    if (col->kind == ast::Expr::Kind::Column && rhs->kind == ast::Expr::Kind::Subquery) {
      auto [table, c] = resolve(*col);
      auto s = subquery(*rhs->subquery, logical::Subquery::Kind::Value, rhs->pos);
      comparable(s.type, *c, rhs->pos);
      return logical::compare(op, logical::column_of(table, *c), logical::param(s.param, subquery_param_type(s.type, *c)));
    }
    // The clock: the statement's reading, filled in on each run.
    if (col->kind == ast::Expr::Kind::Column && rhs->kind == ast::Expr::Kind::Clock) {
      auto [table, c] = resolve(*col);
      return logical::compare(op, logical::column_of(table, *c), compared_clock(*rhs, *c, w.lhs.kind == ast::Expr::Kind::Clock, w));
    }
    // A constant: folded to the column's type, where it holds it exactly.
    if (col->kind == ast::Expr::Kind::Column && is_constant(*rhs)) {
      auto [table, c] = resolve(*col);
      auto v = coerce(fold(*rhs), *c, false, written_op(w, op));
      return logical::compare(op, logical::column_of(table, *c), fixed(std::move(v), c->sql));
    }
    if (col->kind == ast::Expr::Kind::Clock || rhs->kind == ast::Expr::Kind::Clock) {
      const auto& k = col->kind == ast::Expr::Kind::Clock ? *col : *rhs;
      if (clock_words(k).starts_with("CURRENT_DATE")) not_yet("comparing the time with a constant, a parameter or the time", w.pos);
      // The planner plans a WHERE conjunct that compares a column; one that
      // compares none is not built yet, so neither is this.
      not_supported("comparing " + clock_words(k) + " in WHERE or ON with other than a column", w.pos);
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

  // The clock compared with column `c`: its reading as a parameter of the
  // column's type, which the API fills on each run, as a select list's. A
  // date with a date, a time with a time, either timestamp with either, a
  // time with time zone with CURRENT_TIME or LOCALTIME: the session's time
  // zone is UTC, so the instant is the local time. A date with a timestamp
  // compares the date's midnight, and a time with CURRENT_TIME the time as
  // a time with time zone, which the layer does not compute yet; any other
  // pair has no operator in PostgreSQL (42883), named in the order written.
  auto compared_clock(const ast::Expr& clock, const Column& c, bool clock_first, const ast::Compare& w) -> logical::Expr {
    using F = ClockDefault::Fn;
    auto fn = clock_fn(clock.func, clock.pos);
    // A session function's NAME with a string column: compared as text.
    if (is_session_fn(fn) && holds_string(c.sql)) return clock_reading(ClockDefault{fn, 0}, c);
    auto stamp = [](SqlKind k) { return k == SqlKind::Timestamp || k == SqlKind::TimestampTz; };
    auto clock_stamp = fn == F::CurrentTimestamp || fn == F::LocalTimestamp;
    auto same = (c.sql.kind == SqlKind::Date && fn == F::CurrentDate) || (c.sql.kind == SqlKind::Time && fn == F::LocalTime) ||
                (stamp(c.sql.kind) && clock_stamp) ||
                (c.sql.kind == SqlKind::TimeTz && (fn == F::CurrentTime || fn == F::LocalTime));
    if (same) {
      auto precision = static_cast<std::uint8_t>(clock.constant < 0 || clock.constant > 6 ? 6 : clock.constant);
      return clock_reading(ClockDefault{fn, precision}, c);
    }
    auto ct = clock_type(fn);
    if ((c.sql.kind == SqlKind::Date && clock_stamp) || (stamp(c.sql.kind) && fn == F::CurrentDate) ||
        (c.sql.kind == SqlKind::Time && fn == F::CurrentTime)) {
      not_yet("comparing " + c.name + " (" + sql_type_name(c.sql) + ") with the clock's " + pg_type_name(ct), w.pos);
    }
    auto l = clock_first ? pg_type_name(ct) : pg_type_name(c.sql);
    auto r = clock_first ? pg_type_name(c.sql) : pg_type_name(ct);
    throw SqlError{"operator does not exist: " + l + " " + std::string{written_op(w, w.op)} + " " + r, w.pos, kUndefinedFunction};
  }

  // `op` as the statement wrote it, for an error: the parser writes <> as
  // NOT (=), and PostgreSQL names <>.
  static auto written_op(const ast::Compare& w, ast::Compare::Op op) -> std::string_view {
    return w.ne && op == ast::Compare::Op::Eq ? "<>" : op_text(op);
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

  // The conjuncts of one clause of a join: the WHERE's (`join` 0) or the ON
  // of from[join], each a comparison of a column of one relation with a
  // parameter, or of columns of two relations, in the order written. An ON
  // sees the relations of its own join, up to from[join] and from
  // from[on_from] (ast::Compare::on_from); one it names later is 42P01, a
  // name of one before it 42P01 (the parser's) or 42703. `written` names
  // each relation as the statement does.
  auto conjuncts(std::span<const Table* const> rels, std::span<const std::string> written,
                 const std::vector<ast::Compare>& in, std::uint32_t join) -> std::vector<logical::Expr> {
    std::vector<logical::Expr> out;
    for (const auto* c : top_level(in)) {
      const auto& w = *c;
      if (w.on != join) continue;
      auto end = join == 0 ? rels.size() : std::min<std::size_t>(join + 1, rels.size());
      auto first = std::min<std::size_t>(w.on_from, end);
      auto visible = rels.subspan(first, end - first);
      Hidden hidden{rels.first(first), written.first(first)};
      if (w.kind == ast::Compare::Kind::Compare && w.lhs.kind == ast::Expr::Kind::Column &&
          w.rhs.kind == ast::Expr::Kind::Column) {
        out.push_back(join_on(visible, w, hidden));
        continue;
      }
      out.push_back(predicate(w, [&](const ast::Expr& e) {
        auto [side, col] = column(visible, e, hidden);
        return std::pair{visible[side]->id, col};
      }));
    }
    return out;
  }

  // A column of one relation compared with a column of another, of one SQL
  // type, by any of the operators; two columns of one relation, a filter of
  // its rows (columns_compare).
  auto join_on(std::span<const Table* const> rels, const ast::Compare& w, const Hidden& hidden = {}) const -> logical::Expr {
    auto [ls, lc] = column(rels, w.lhs, hidden);
    auto [rs, rc] = column(rels, w.rhs, hidden);
    if (ls == rs) return columns_compare(w, w.op, rels[ls]->id, *lc, *rc);
    return joined_compare(w.op, written_op(w, w.op), rels[ls]->id, *lc, rels[rs]->id, *rc, w.pos);
  }

  // t.lc <op> t.rc, two columns of one table: a filter of its rows, never a
  // key bound. Of one compared type (compared_type: VARCHAR or CHAR(n) with
  // TEXT, a DECIMAL of scale 0 with a BIGINT), two integers, two floats, or an
  // integer with a float, which compares as float8, as PostgreSQL casts the
  // integer. Two DECIMALs of different scales, or a DECIMAL with another
  // number, are not supported yet: the comparison does not rescale. Nor is
  // a CHAR(n) with a VARCHAR, which PostgreSQL compares as bpchar, its
  // trailing spaces ignored on both sides, nor a date with a timestamp. A
  // pair PostgreSQL has no operator for is 42883, named in the order written.
  static auto columns_compare(const ast::Compare& w, ast::Compare::Op op, std::uint32_t table, const Column& lc,
                              const Column& rc) -> logical::Expr {
    auto integral = [](SqlType t) { return is_integer(compared_type(t).identity); };
    auto floating = [](SqlType t) { return t.kind == SqlKind::Real || t.kind == SqlKind::Double; };
    auto number = [&](SqlType t) { return integral(t) || floating(t); };
    auto a = lc.sql;
    auto b = rc.sql;
    if (a.kind == SqlKind::Numeric || b.kind == SqlKind::Numeric) {
      no_operator(a, written_op(w, op), b, w.pos);
      numeric_compared(a, b, w.pos);
    }
    if ((compared_type(a).identity == compared_type(b).identity && !bpchar_pair(a, b)) || (number(a) && number(b))) {
      return logical::compare(op, logical::column_of(table, lc), logical::column_of(table, rc));
    }
    no_operator(a, written_op(w, op), b, w.pos);
    numeric_compared(a, b, w.pos);
    not_yet("comparing " + lc.name + " (" + sql_type_name(a) + ") with " + rc.name + " (" + sql_type_name(b) + ")", w.pos);
  }

  // Throws PostgreSQL's 42883 when it has no operator comparing a with b:
  // types of two classes, or a TIME or TIME WITH TIME ZONE with a date or a
  // timestamp. Columns of one class that the layer does not compare yet are
  // the caller's to refuse.
  static void no_operator(SqlType a, std::string_view written, SqlType b, std::int32_t pos) {
    auto time = [](SqlType t) { return t.kind == SqlKind::Time || t.kind == SqlKind::TimeTz; };
    if (type_class(a.kind) == type_class(b.kind) && time(a) == time(b)) return;
    throw SqlError{"operator does not exist: " + pg_type_name(a) + " " + std::string{written} + " " + pg_type_name(b), pos,
                   kUndefinedFunction};
  }

  // Arithmetic of a NUMERIC with a number: not yet (refuse_numeric). With
  // another class PostgreSQL has no operator, 42883, the caller's.
  static void numeric_computed(SqlType a, SqlType b, std::int32_t pos) {
    if ((a.kind == SqlKind::Numeric || b.kind == SqlKind::Numeric) && type_class(a.kind) == type_class(b.kind)) {
      refuse_numeric("arithmetic of a NUMERIC", pos);
    }
  }

  // A NUMERIC compared: not yet (refuse_numeric), once PostgreSQL has the
  // operator (no_operator before it).
  static void numeric_compared(SqlType a, SqlType b, std::int32_t pos) {
    if (a.kind == SqlKind::Numeric || b.kind == SqlKind::Numeric) refuse_numeric("a comparison of a NUMERIC column", pos);
  }

  // A CHAR(n) with a VARCHAR: PostgreSQL compares the two as bpchar, the
  // trailing spaces of both ignored, where the layer would compare the
  // VARCHAR's bytes with the spaces kept.
  static auto bpchar_pair(SqlType a, SqlType b) -> bool {
    return (a.kind == SqlKind::Char && b.kind == SqlKind::Varchar) || (a.kind == SqlKind::Varchar && b.kind == SqlKind::Char);
  }

  // l.lc <op> r.rc, columns of one SQL type: a pair PostgreSQL has no
  // operator for is its 42883 (no_operator, the operator as written); two
  // of different types it does compare are refused (docs/frontend_design.md,
  // *Known differences*, #823), and a CHAR(n) with a VARCHAR (bpchar_pair)
  // is not supported yet.
  static auto joined_compare(ast::Compare::Op op, std::string_view written, std::uint32_t l, const Column& lc,
                             std::uint32_t r, const Column& rc, std::int32_t pos) -> logical::Expr {
    no_operator(lc.sql, written, rc.sql, pos);
    numeric_compared(lc.sql, rc.sql, pos);
    if (bpchar_pair(lc.sql, rc.sql)) {
      not_yet("comparing " + lc.name + " (" + sql_type_name(lc.sql) + ") with " + rc.name + " (" + sql_type_name(rc.sql) + ")", pos);
    }
    if (compared_type(lc.sql).identity != compared_type(rc.sql).identity) {
      throw SqlError{"comparing " + lc.name + " (" + sql_type_name(lc.sql) + ") with " + rc.name + " (" +
                         sql_type_name(rc.sql) + ") is not supported: a join compares columns of one type",
                     pos};
    }
    return logical::compare(op, logical::column_of(l, lc), logical::column_of(r, rc));
  }

  // The join of a query's FROM items, left-deep: the columns of each
  // prefix of two or more of them, in PostgreSQL's order. A join's USING or
  // NATURAL columns come first, each once and named alone, then its left
  // side's other columns, then its right table's. A USING column is its
  // left side's, equal to the right's in every pair or the right's NULL
  // where a LEFT JOIN extends; the right's for a RIGHT JOIN, whose right
  // table is the one it keeps. Each USING ... AS alias, with the
  // FROM item it is on, names that join's USING columns.
  struct JoinColumn {
    std::string name;
    const Table* table;
    const Column* col;
  };
  struct JoinScope {
    std::vector<std::vector<JoinColumn>> levels;  // [k]: the join of the first k + 2 relations
    struct Alias {
      std::string name;
      std::size_t join;
      std::vector<JoinColumn> columns;
    };
    std::vector<Alias> aliases;

    // The columns of the join of the first `n` relations, or null for one relation.
    [[nodiscard]] auto level(std::size_t n) const -> const std::vector<JoinColumn>* {
      return n >= 2 && n - 2 < levels.size() ? &levels[n - 2] : nullptr;
    }
  };

  // Binds the join of `rels`, the FROM items of `s` in their order, into
  // joins_, and returns, for each join k, the equality of each of its USING
  // or NATURAL columns, in order: the left side's column = the right
  // table's. NATURAL's are every name of the left side's columns the right
  // table has, in the left side's order. A USING name must be named once
  // (42701) and be one column of the left side (42702, 42703) and one of
  // the right table (42703), as in PostgreSQL.
  auto join_scope(std::span<const Table* const> rels, const ast::Select& s) -> std::vector<std::vector<logical::Expr>> {
    joins_ = {};
    std::vector<std::vector<logical::Expr>> equal(rels.size());
    std::vector<JoinColumn> left;
    auto columns_of = [](const Table& t, std::vector<JoinColumn>& into, const std::vector<std::string>& except) {
      for (const auto& c : t.columns) {
        if (!c.hidden && !std::ranges::contains(except, c.name)) into.push_back({c.name, &t, &c});
      }
    };
    columns_of(*rels[0], left, {});
    for (std::size_t k = 1; k < rels.size(); ++k) {
      const auto& item = s.from[k];
      const auto& right = *rels[k];
      auto names = item.using_columns;
      if (item.natural) {
        for (const auto& c : left) {
          if (right.column(c.name) != nullptr) names.push_back(c.name);
        }
      }
      std::vector<JoinColumn> next;
      for (auto i = names.begin(); i != names.end(); ++i) {
        const auto& name = *i;
        if (std::find(names.begin(), i, name) != i) {
          throw SqlError{"column name \"" + name + "\" appears more than once in USING clause", item.pos, kDuplicateColumn};
        }
        auto n = std::ranges::count(left, name, &JoinColumn::name);
        if (n > 1) {
          throw SqlError{"common column name \"" + name + "\" appears more than once in left table", item.pos, kAmbiguousColumn};
        }
        if (n == 0) {
          throw SqlError{"column \"" + name + "\" specified in USING clause does not exist in left table", item.pos,
                         kUndefinedColumn};
        }
        const auto* rc = right.column(name);
        if (rc == nullptr) {
          throw SqlError{"column \"" + name + "\" specified in USING clause does not exist in right table", item.pos,
                         kUndefinedColumn};
        }
        const auto& lc = *std::ranges::find(left, name, &JoinColumn::name);
        equal[k].push_back(joined_compare(ast::Compare::Op::Eq, "=", lc.table->id, *lc.col, right.id, *rc, item.pos));
        next.push_back(item.kind == ast::JoinKind::Right ? JoinColumn{name, &right, rc} : lc);
      }
      if (!item.using_alias.empty()) joins_.aliases.push_back({item.using_alias, k, next});
      for (const auto& c : left) {
        if (!std::ranges::contains(names, c.name)) next.push_back(c);
      }
      columns_of(right, next, names);
      joins_.levels.push_back(next);
      left = std::move(next);
    }
    return equal;
  }

  // The tables the FROM items name, each read by an instance of its own
  // (instance). A derived relation's query reads its tables by their own
  // ids, in the loops the query around it runs inside: a table it reads is
  // read already, so the query's own use of it is an instance.
  auto relations(const ast::Select& s) -> std::vector<const Table*> {
    std::vector<const Table*> rels;
    std::vector<const Table*> read;
    for (const auto& item : s.from) {
      if (!item.derived) continue;
      for (const auto& inner : item.derived->from) {
        if (const auto* t = cat_.table(inner.table)) read.push_back(t);
      }
    }
    for (const auto& item : s.from) {
      const auto pos = s.from.size() == 1 ? s.pos : item.pos;
      if (item.derived) {
        rels.push_back(derived_relation(item, pos));
        continue;
      }
      auto seen = read;
      seen.insert(seen.end(), rels.begin(), rels.end());
      rels.push_back(instance(*table(item.table, pos), item.name(), seen));
      if (!item.column_aliases.empty()) rename_columns(*rels.back(), item, pos);
      // Two views of one table are merged into the query under one name.
      if (std::ranges::count_if(rels, [&](const Table* r) { return r->name == rels.back()->name; }) > 1) {
        not_yet("a table joined with itself through views", pos);
      }
    }
    return rels;
  }

  // A grouped view read as a relation (ast::FromItem::derived): its query
  // bound as a SELECT of its own, the query around it out of its scope, as
  // a subquery's is, and a table of the binder's made of its outputs, named
  // as the view names its columns (docs/frontend_design.md, *Views*). A
  // column's NULLs are its output's: a grouped column's as its table
  // declares, COUNT never, any other aggregate's where its group has no
  // value. The table has no primary key, so no column of it is dependent on
  // a GROUP BY. An AVG of an integer or DECIMAL, PostgreSQL's numeric, is
  // two plan columns and none a relation's: refused.
  auto derived_relation(const ast::FromItem& item, std::int32_t pos) -> const Table* {
    auto joins = std::move(joins_);
    auto scope = std::move(scope_);
    auto root = select(*item.derived);
    joins_ = std::move(joins);
    scope_ = std::move(scope);
    const auto& outs = outputs_of(*root);
    if (outs.size() != item.column_aliases.size()) throw std::logic_error{"a derived relation's columns are not its query's"};
    auto t = std::make_unique<Table>();
    t->id = Table::kDerivedBase + bytecask::sql::narrow<std::uint32_t>(derived_.size());
    if (!Table::derived(t->id)) not_yet("a statement of so many derived relations", pos);
    t->name = item.table;
    for (std::size_t i = 0; i < outs.size(); ++i) {
      const auto& e = outs[i].expr;
      if (e.type.kind == SqlKind::Numeric) {
        not_yet("aggregating over a grouped view's AVG of an integer or numeric (PostgreSQL's numeric)", pos);
      }
      auto nullable = e.kind == logical::Expr::Kind::Column ? e.col.nullable
                                                             : !(e.kind == logical::Expr::Kind::Aggregate && e.fn == logical::Expr::Fn::Count);
      t->columns.push_back(Column{bytecask::sql::narrow<std::uint32_t>(i), item.column_aliases[i], e.type, e.phys(), nullable, std::monostate{}});
    }
    t->next_col_id = bytecask::sql::narrow<std::uint32_t>(t->columns.size());
    const auto* first = cat_.table(item.derived->from.at(0).table);
    auto source = first != nullptr ? first->id : 0;
    derived_.push_back({std::move(t), std::move(root), source});
    return derived_.back().table.get();
  }

  // t AS x (a, b): the instance's first columns carry the names the alias
  // gives; the rest keep theirs. More names than columns is 42P10. The
  // instance is the alias's own (FromItem::name differs from the table's).
  void rename_columns(const Table& read, const ast::FromItem& item, std::int32_t pos) {
    auto visible = static_cast<std::size_t>(std::ranges::count_if(read.columns, [](const Column& c) { return !c.hidden; }));
    if (item.column_aliases.size() > visible) {
      throw SqlError{"table \"" + item.name() + "\" has " + std::to_string(visible) + " columns available but " +
                         std::to_string(item.column_aliases.size()) + " columns specified",
                     pos, kInvalidColumnReference};
    }
    auto owned = std::ranges::find_if(instances_, [&](const auto& i) { return i.get() == &read; });
    if (owned == instances_.end()) not_yet("renaming the columns of a table under its own name", pos);
    auto& copy = **owned;
    auto next = item.column_aliases.begin();
    for (auto& c : copy.columns) {
      if (c.hidden || next == item.column_aliases.end()) continue;
      c.name = *next++;
    }
    if (std::ranges::any_of(copy.columns, [&](const Column& c) {
          return !c.hidden && std::ranges::count_if(copy.columns, [&](const Column& d) { return !d.hidden && d.name == c.name; }) > 1;
        })) {
      throw SqlError{"column name specified more than once in the alias of \"" + item.name() + "\"", pos, kDuplicateColumn};
    }
  }

  // The table to read for a use of `t` named `name`: the table itself, or,
  // where the query reads it already or names it otherwise, an instance of
  // it with an id of its own, which the plan reads the table's rows through
  // (Table::source). The instance outlives the plan: the bound statement
  // owns it.
  auto instance(const Table& t, const std::string& name, const std::vector<const Table*>& read) -> const Table* {
    auto again = std::ranges::any_of(read, [&](const Table* r) { return r->storage_id() == t.id; });
    if (!again && name == t.name) return &t;
    auto copy = std::make_unique<Table>(t);
    copy->id = Table::kInstanceBase + bytecask::sql::narrow<std::uint32_t>(instances_.size());
    copy->source = t.id;
    copy->name = name;
    instances_.push_back(std::move(copy));
    return instances_.back().get();
  }

  // A SELECT of tables: Limit? -> Project -> Sort? -> Filter? -> a tree of
  // Joins over Gets, left-deep, each Join's right side the next FROM item,
  // or a RIGHT JOIN's group (join_steps);
  // aggregated, Limit? -> Sort? -> Filter? -> Aggregate -> Filter? -> the
  // tables, the Filter above the Aggregate its HAVING. The WHERE's conjuncts,
  // an inner join's ON among them, sit in one Filter above the tables;
  // predicate pushdown moves each down. An outer join's ON stays its Join's.
  // No FOR UPDATE on a join yet.
  //
  // A query is aggregated by a GROUP BY, a HAVING or an aggregate call. Then
  // a column it selects, orders by or compares in HAVING must be grouped,
  // or of a table whose primary key is (42803): PostgreSQL's functional
  // dependency, so it is the same in every row of a group, read from any
  // one (Aggregate::dependent), and the GROUP BY stays as written.
  // LIMIT and OFFSET over a query's rows: a Limit passing its first n rows
  // and dropping the first `offset` of them, n the two counts' sum, or every
  // row without a LIMIT (logical::kAllRows).
  static auto limited(logical::NodePtr rows, const ast::Select& s) -> logical::NodePtr {
    if (!s.limit && s.offset == 0) return rows;
    auto n = logical::kAllRows;
    if (s.limit && __builtin_add_overflow(*s.limit, s.offset, &n)) n = logical::kAllRows;
    return logical::node(logical::Limit{std::move(rows), n, s.offset});
  }

  // A step of a join's plan: the relations it reads, by FROM position, and
  // how it joins the rows before it. An inner step reads one relation; an
  // outer one the NULL-extended side of a left outer join, a LEFT JOIN's
  // table or a group: the items a RIGHT JOIN's own table keeps its rows
  // against, from[join_from] to the item before it, inner joins of each
  // other under their ONs (`group_on`), all NULL-extended alike. `on` is the
  // step's join's ON, or an inner step's, which joins the WHERE's.
  struct JoinStep {
    std::vector<std::size_t> rels;
    bool outer{false};
    std::size_t join{0};  // the FROM position whose ON `on` is; 0 for none
    std::vector<logical::Expr> on;
    std::vector<std::size_t> group_joins;  // a group's own joins, whose ONs are group_on
    std::vector<logical::Expr> group_on;
  };

  // The order a join's relations are read in: FROM order, but for a RIGHT
  // JOIN, whose table comes before its group, the LEFT JOIN of the two
  // swapped. Its group's rows are the same in any order its items are joined
  // in when they are inner joins of tables; a group that holds an outer join
  // is not supported yet.
  static auto join_steps(const ast::Select& s) -> std::vector<JoinStep> {
    std::vector<JoinStep> steps;
    steps.push_back({{0}, false, 0, {}, {}, {}});
    for (std::size_t k = 1; k < s.from.size(); ++k) {
      const auto& item = s.from[k];
      if (item.kind != ast::JoinKind::Right) {
        steps.push_back({{k}, item.kind == ast::JoinKind::Left, k, {}, {}, {}});
        continue;
      }
      // The steps from[join_from] on, the tail: a comma's items before them
      // are joined to the RIGHT JOIN's rows, its ON reading none of them.
      auto first = std::ranges::find_if(steps, [&](const JoinStep& st) { return st.rels[0] >= item.join_from; });
      JoinStep group{{}, true, k, {}, {}, {}};
      for (auto st = first; st != steps.end(); ++st) {
        if (st->outer || st->rels.size() != 1) {
          not_yet("a RIGHT JOIN of a join that has an outer join", item.pos);
        }
        group.rels.push_back(st->rels[0]);
        if (st->join != 0) group.group_joins.push_back(st->join);
      }
      steps.erase(first, steps.end());
      // Its table, joined to the comma's items before the group by no ON.
      steps.push_back({{k}, false, 0, {}, {}, {}});
      steps.push_back(std::move(group));
    }
    return steps;
  }

  // Where join k's ON goes: its step's, or its group's.
  static auto step_on(std::vector<JoinStep>& steps, std::size_t k) -> std::vector<logical::Expr>& {
    for (auto& st : steps) {
      if (st.join == k) return st.on;
      if (std::ranges::contains(st.group_joins, k)) return st.group_on;
    }
    throw std::logic_error{"binder: a join with no step"};
  }

  auto select(const ast::Select& s) -> logical::NodePtr {
    // Only this query's select list is assigned, not a subquery's.
    auto assign = std::exchange(assign_, std::nullopt);
    if (s.set) {
      if (assign) not_supported("INSERT … SELECT of a set operation (UNION, INTERSECT, EXCEPT)", s.pos);
      return set_operation(s);
    }
    if (s.from.empty()) return select_without_from(s, assign);
    if (std::ranges::any_of(s.from, [](const ast::FromItem& f) { return f.kind == ast::JoinKind::Full; })) {
      return full_join(s, assign.has_value());
    }
    auto rels = relations(s);
    joins_ = {};
    // Join k's USING or NATURAL equalities.
    std::vector<std::vector<logical::Expr>> using_equal;
    if (rels.size() > 1) using_equal = join_scope(rels, s);
    auto starred = std::ranges::any_of(s.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Star; });
    std::vector<ast::Expr> expanded;
    if (starred) expanded = expand_stars(s, rels);
    const auto& list = starred ? expanded : s.targets;
    const auto n = rels.size();
    auto joined = n > 1;
    std::vector<logical::Expr> where_conjuncts;
    // The plan's joins, left-deep, in the order they run (join_steps): each
    // step's relations by FROM position, joined to the rows before it.
    std::vector<JoinStep> steps;
    // The relations a join NULL-extends: none for an inner join.
    std::vector<std::uint32_t> nulled;
    std::vector<std::string> written;
    for (const auto& item : s.from) written.push_back(item.written.empty() ? item.name() : item.written);
    set_scope(rels);
    if (joined) {
      if (s.for_update) throw SqlError{"FOR UPDATE on a join is not supported yet", s.pos};
      steps = join_steps(s);
      // A derived relation's rows are read only as the plan's outermost
      // loop, never as an outer join's NULL-extended side.
      for (const auto& step : steps) {
        if (!step.outer) continue;
        for (auto r : step.rels) {
          if (Table::derived(rels[r]->id)) not_yet("a grouped view on the NULL-extended side of an outer join", s.from[r].pos);
        }
      }
      // An outer join's ON decides its pairs and its WHERE filters them, so
      // the two stay apart; an inner join's ON is the WHERE's, but inside an
      // outer join's group, where it decides which rows the group has.
      for (std::size_t k = 1; k < n; ++k) {
        auto here = conjuncts(rels, written, s.where, static_cast<std::uint32_t>(k));
        here.insert(here.begin(), std::make_move_iterator(using_equal[k].begin()), std::make_move_iterator(using_equal[k].end()));
        // A column of a NULL-extended relation is NULL-able in the ON of every later join.
        for (auto& e : here) {
          for (auto t : nulled) mark_nullable(e, t);
        }
        if (s.from[k].kind == ast::JoinKind::Left) nulled.push_back(rels[k]->id);
        if (s.from[k].kind == ast::JoinKind::Right) {
          for (auto r = std::size_t{s.from[k].join_from}; r < k; ++r) nulled.push_back(rels[r]->id);
        }
        auto& into = step_on(steps, k);
        into.insert(into.end(), std::make_move_iterator(here.begin()), std::make_move_iterator(here.end()));
      }
      // An inner step's ON joins the WHERE's.
      for (auto& step : steps) {
        if (step.outer) continue;
        where_conjuncts.insert(where_conjuncts.end(), std::make_move_iterator(step.on.begin()),
                               std::make_move_iterator(step.on.end()));
        step.on.clear();
      }
      auto rest = conjuncts(rels, written, s.where, 0);
      where_conjuncts.insert(where_conjuncts.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
    } else {
      where_conjuncts = where(*rels[0], s.where);
    }
    auto from = std::span<const Table* const>{rels};
    // A column reference: of the one table, or of the relation that has it.
    auto resolve = [&](const ast::Expr& e) -> std::pair<const Table*, const Column*> {
      if (!joined) return {from[0], column(*from[0], e)};
      auto [side, c] = column(from, e);
      return {from[side], c};
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
      // An expression key is selected where an item is written alike.
      auto key_selected = [&](const ast::Expr& e) {
        if (e.kind == ast::Expr::Kind::Column) return selected(e);
        return std::ranges::any_of(list, [&](const ast::Expr& t) { return ast::same_expr(t, e); });
      };
      for (const auto& o : s.order_by) {
        if (!key_selected(o.expr)) {
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
    if (aggregated && joined && std::ranges::any_of(rels, [](const Table* t) { return Table::derived(t->id); })) {
      not_yet("grouping, aggregating or DISTINCT over a join of a grouped view", s.pos);
    }
    auto grouping = !s.group_by.empty() || distinct_groups;
    std::vector<logical::Expr> group_by;
    for (const auto& e : distinct_groups ? list : s.group_by) {
      auto [t, c] = resolve(e);
      if (c->sql.kind == SqlKind::Numeric) refuse_numeric(std::string{distinct_groups ? "DISTINCT" : "GROUP BY"} + " of a NUMERIC column", e.pos);
      // A derived relation's groups are keyed by values the plan holds in
      // locals: integers and booleans (#833).
      if (Table::derived(t->id) && c->type != PhysType::I64 && c->type != PhysType::Bool) {
        not_yet("grouping a grouped view's rows by a " + pg_type_name(c->sql) + " column", e.pos);
      }
      auto g = logical::column_of(t->id, *c);
      if (std::ranges::find(group_by, g) == group_by.end()) group_by.push_back(std::move(g));
    }
    // A column of an aggregated query: grouped, or dependent, as its
    // table's grouped primary key determines it; else 42803.
    std::vector<logical::Expr> dependent;
    auto grouped = [&](const Table& t, const Column& c, std::int32_t pos) -> logical::Expr {
      auto g = logical::column_of(t.id, c);
      if (std::ranges::find(group_by, g) != group_by.end()) return g;
      // A table with no primary key, a derived relation, determines nothing.
      auto by_key = grouping && !t.pk_col_ids.empty() && std::ranges::all_of(t.pk_col_ids, [&](std::uint32_t id) {
        return std::ranges::find(group_by, logical::column_of(t.id, *t.column(id))) != group_by.end();
      });
      if (!by_key) ungrouped(t, c.name, pos);
      if (std::ranges::find(dependent, g) == dependent.end()) dependent.push_back(g);
      return g;
    };
    std::vector<logical::Output> targets;
    std::vector<bool> deferred(list.size(), false);
    for (std::size_t i = 0; i < list.size(); ++i) {
      const auto& e = list[i];
      if (e.kind == ast::Expr::Kind::Aggregate) {
        targets.push_back(aggregate(aggregate_arg(e, resolve), e));
        continue;
      }
      if (!aggregated) {
        if (assign && assigned_later(e)) {
          deferred[i] = true;
          targets.push_back({logical::Expr{}, {}});
          continue;
        }
        targets.push_back(plain_output(e, resolve));
        continue;
      }
      if (e.kind == ast::Expr::Kind::Clock) {
        if (clock_words(e).starts_with("CURRENT_DATE")) not_yet("the time in the select list of an aggregated query", e.pos);
        refuse_clock(e, "in the select list of an aggregated query");
      }
      if (is_constant(e) || e.kind == ast::Expr::Kind::Subquery) {
        not_yet(std::string{e.kind == ast::Expr::Kind::Subquery ? "a subquery" : "a constant"} +
                    " in the select list of an aggregated query",
                e.pos);
      }
      if (e.kind != ast::Expr::Kind::Column) not_yet("an expression in the select list of an aggregated query", e.pos);
      auto [t, c] = resolve(e);
      targets.push_back({grouped(*t, *c, e.pos), c->name});
    }
    named(list, targets);
    if (assign) assign_outputs(*assign, list, deferred, targets, s.pos);
    if (s.row_key && row_key_allowed_ && !joined && !aggregated) {
      const auto& t = *rels[0];
      for (auto id : t.pk_col_ids) targets.push_back({logical::column_of(t.id, *t.column(id)), "?key?"});
      key_columns_ = bytecask::sql::narrow<std::uint32_t>(t.pk_col_ids.size());
      key_table_ = t.storage_id();
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
      if (o.expr.kind == ast::Expr::Kind::Column) {
        auto [t, c] = resolve(o.expr);
        if (c->sql.kind == SqlKind::Numeric) refuse_numeric("ORDER BY a NUMERIC column", o.pos);
        auto key = aggregated ? grouped(*t, *c, o.pos) : logical::column_of(t->id, *c);
        keys.push_back({std::move(key), o.desc, o.nulls_first.value_or(o.desc)});
        continue;
      }
      if (aggregated) not_yet("ORDER BY an expression in a grouped or aggregated query", o.pos);
      if (auto key = order_key(o.expr, resolve)) keys.push_back({std::move(*key), o.desc, o.nulls_first.value_or(o.desc)});
    }

    // Each relation's rows: a table's, or a derived relation's query.
    auto leaf = [&](const Table* t) {
      if (Table::derived(t->id)) return logical::node(logical::Derived{std::move(derived_.at(t->id - Table::kDerivedBase).plan), t});
      return logical::node(logical::Get{t, !joined && s.for_update});
    };
    auto rows = leaf(rels[joined ? steps[0].rels[0] : 0]);
    for (std::size_t i = 1; i < steps.size(); ++i) {
      auto& step = steps[i];
      // A group: its relations' inner joins, under the ON of each.
      auto right = leaf(rels[step.rels[0]]);
      for (std::size_t r = 1; r < step.rels.size(); ++r) {
        right = logical::node(logical::Join{std::move(right), leaf(rels[step.rels[r]]), {}, false});
      }
      if (!step.group_on.empty()) right = logical::node(logical::Filter{std::move(right), std::move(step.group_on)});
      rows = logical::node(logical::Join{std::move(rows), std::move(right), std::move(step.on), step.outer});
    }
    if (!where_conjuncts.empty()) rows = logical::node(logical::Filter{std::move(rows), std::move(where_conjuncts)});
    if (aggregated) {
      rows = logical::node(logical::Aggregate{std::move(rows), std::move(targets), std::move(group_by), grouping,
                                              std::move(dependent)});
      if (!having.empty()) rows = logical::node(logical::Filter{std::move(rows), std::move(having)});
      if (!keys.empty()) rows = logical::node(logical::Sort{std::move(rows), std::move(keys)});
    } else {
      if (!keys.empty()) rows = logical::node(logical::Sort{std::move(rows), std::move(keys)});
      rows = logical::node(logical::Project{std::move(rows), std::move(targets)});
    }
    rows = limited(std::move(rows), s);
    for (auto t : nulled) nullable_above_join(*rows, t);
    return rows;
  }

  // FROM a FULL JOIN b ON …, the whole FROM (docs/planner_design.md, *FULL
  // JOIN*): UNION ALL of two plans, each a SELECT of the statement's list
  // and WHERE. The left arm is a LEFT JOIN b, every row of a. The right arm,
  // an anti-join, is a RIGHT JOIN b under a's primary key IS NULL: the rows
  // of b the ON pairs with no row of a, NULL-extended. A table's primary key,
  // its hidden row id for one declared without, is never NULL in a row, so
  // that conjunct holds exactly of the NULL-extended rows. The WHERE is
  // above each arm's join, after the NULL-extension. An ORDER BY key the
  // select list has is its column; any other is a trailing column of both
  // arms the statement does not return (logical::SetOp::hidden).
  auto full_join(const ast::Select& s, bool assigned) -> logical::NodePtr {
    // The parser takes a FULL JOIN only as the whole FROM, of two tables (#712).
    if (s.from.size() != 2) throw std::logic_error{"binder: a FULL JOIN of other than two tables"};
    for (const auto& f : s.from) {
      if (f.derived) not_yet("a grouped view in a FULL JOIN", f.pos);
    }
    auto aggregate = [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Aggregate; };
    if (!s.group_by.empty() || !s.having.empty() || s.distinct || std::ranges::any_of(s.targets, aggregate) ||
        std::ranges::any_of(s.order_by, [&](const ast::OrderItem& o) { return aggregate(o.expr); })) {
      not_yet("an aggregate, GROUP BY or DISTINCT over a FULL JOIN", s.pos);
    }
    if (s.for_update) {
      throw SqlError{"FOR UPDATE cannot be applied to the nullable side of an outer join", s.pos, kFeatureNotSupported};
    }
    if (assigned) not_yet("INSERT … SELECT of a FULL JOIN", s.pos);
    if (in_set_arm_) not_yet("a FULL JOIN in an arm of a set operation (UNION, INTERSECT, EXCEPT)", s.pos);
    if (s.offset != 0) not_yet("OFFSET over a FULL JOIN", s.pos);
    auto arm = ast::clone(s);
    arm.order_by.clear();
    arm.limit.reset();
    arm.limit_written = false;
    arm.row_key = false;
    // Each ORDER BY key: a select-list item written alike, or a hidden
    // column appended to both arms. A constant orders nothing; a bare $n
    // stays an ORDER BY item of the arms, which sorts on nothing but binds
    // the parameter as a plain query's ORDER BY does (order_key).
    struct Key {
      bool hidden;
      std::size_t at;
      const ast::OrderItem* item;
    };
    std::vector<Key> keys;
    auto stars = std::ranges::any_of(s.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Star; });
    std::size_t hidden = 0;
    for (const auto& o : s.order_by) {
      if (o.expr.kind == ast::Expr::Kind::Param) arm.order_by.push_back(ast::clone(o));
      if (is_constant(o.expr) || o.expr.kind == ast::Expr::Kind::Param) continue;
      auto same = stars ? s.targets.end()
                        : std::ranges::find_if(s.targets, [&](const ast::Expr& t) { return ast::same_expr(t, o.expr); });
      if (same != s.targets.end()) {
        keys.push_back({false, static_cast<std::size_t>(same - s.targets.begin()), &o});
        continue;
      }
      auto key = ast::clone(o.expr);
      key.alias = "?order?";
      arm.targets.push_back(std::move(key));
      keys.push_back({true, hidden++, &o});
    }
    auto left_arm = ast::clone(arm);
    left_arm.from[1].kind = ast::JoinKind::Left;
    auto right_arm = std::move(arm);
    right_arm.from[1].kind = ast::JoinKind::Right;
    // The arms are a set operation's in the plan: what they cannot hold is
    // refused as there.
    auto was = std::exchange(in_set_arm_, true);
    auto left = select(left_arm);
    auto right = select(right_arm);
    in_set_arm_ = was;
    full_join_on(*left, s.from[1].pos);
    anti_join(*right);
    const auto& lo = outputs_of(*left);
    const auto& ro = outputs_of(*right);
    if (lo.size() != ro.size() || lo.size() < hidden) throw std::logic_error{"binder: FULL JOIN arms of other columns"};
    const auto visible = lo.size() - hidden;
    logical::SetOp out;
    out.kind = ast::SetOperation::Kind::Union;
    out.all = true;
    for (std::size_t i = 0; i < lo.size(); ++i) {
      auto type = set_type(lo[i].expr.type, ro[i].expr.type, false, false, "FULL JOIN", s.pos);
      (i < visible ? out.outputs : out.hidden).push_back({logical::param(0, type), lo[i].name});
    }
    for (const auto& k : keys) {
      const auto& o = *k.item;
      out.order.push_back({k.hidden ? visible + k.at : k.at, o.desc, o.nulls_first.value_or(o.desc)});
    }
    out.left = std::move(left);
    out.right = std::move(right);
    auto rows = logical::node(std::move(out));
    if (s.limit) rows = logical::node(logical::Limit{std::move(rows), *s.limit});
    return rows;
  }

  // The Join of a FULL JOIN's arm, under its Project and WHERE.
  static auto arm_join(logical::Node& n) -> logical::Join& {
    auto* at = &n;
    while (!std::holds_alternative<logical::Join>(at->op)) {
      auto* in = logical::input_of(*at);
      if (in == nullptr || !*in) throw std::logic_error{"binder: a FULL JOIN arm with no join"};
      at = in->get();
    }
    return std::get<logical::Join>(at->op);
  }

  // A FULL JOIN's ON, as PostgreSQL plans one: by a merge or hash join,
  // so a conjunct that reads columns is an equality of an expression of
  // each side, or one such equality is among them; else 0A000.
  static void full_join_on(logical::Node& left_arm, std::int32_t pos) {
    const auto& j = arm_join(left_arm);
    auto tables = [](const logical::Expr& e) {
      std::vector<logical::ColumnRef> cols;
      logical::columns_of(e, cols);
      std::vector<std::uint32_t> out;
      for (const auto& c : cols) {
        if (!std::ranges::contains(out, c.table)) out.push_back(c.table);
      }
      return out;
    };
    // An equality of the two sides, as PostgreSQL finds one after pushing
    // each NOT down (negate_clause): NOT (a <> b), the parser's NOT (NOT
    // (a = b)), is a = b, and NOT (x OR y) is NOT x AND NOT y.
    std::function<bool(const logical::Expr&, bool)> joins = [&](const logical::Expr& c, bool negated) {
      using K = logical::Expr::Kind;
      if (c.kind == K::Not && c.args.size() == 1) return joins(c.args[0], !negated);
      if ((c.kind == K::And && !negated) || (c.kind == K::Or && negated)) {
        return std::ranges::any_of(c.args, [&](const logical::Expr& a) { return joins(a, negated); });
      }
      if (negated || c.kind != K::Compare || c.op != logical::Op::Eq || c.args.size() != 2) return false;
      auto l = tables(c.args[0]);
      auto r = tables(c.args[1]);
      return l.size() == 1 && r.size() == 1 && l[0] != r[0];
    };
    auto reads = [&](const logical::Expr& c) { return !tables(c).empty(); };
    if (std::ranges::any_of(j.predicate, [&](const logical::Expr& c) { return joins(c, false); }) || std::ranges::none_of(j.predicate, reads)) return;
    throw SqlError{"FULL JOIN is only supported with merge-joinable or hash-joinable join conditions", pos,
                   kFeatureNotSupported};
  }

  // The right arm's anti-join: its rows whose left table, NULL-extended,
  // has a NULL primary key, a conjunct above the join with the WHERE's.
  static void anti_join(logical::Node& right_arm) {
    auto& j = arm_join(right_arm);
    const auto* get = std::get_if<logical::Get>(&j.right->op);
    if (get == nullptr || get->table->pk_col_ids.empty()) throw std::logic_error{"binder: a FULL JOIN of no table"};
    const auto& t = *get->table;
    auto key = logical::column_of(t.id, *t.column(t.pk_col_ids[0]));
    key.col.nullable = true;
    auto unmatched = logical::predicate_of(logical::Expr::Kind::IsNull, {std::move(key)});
    auto* p = std::get_if<logical::Project>(&right_arm.op);
    if (p == nullptr) throw std::logic_error{"binder: a FULL JOIN arm that is not a Project"};
    if (auto* f = std::get_if<logical::Filter>(&p->input->op)) {
      f->conjuncts.push_back(std::move(unmatched));
      return;
    }
    p->input = logical::node(logical::Filter{std::move(p->input), {std::move(unmatched)}});
  }

  // An ORDER BY key of a query that is not aggregated, an expression of its
  // FROM columns bound as a select item is (plain_output), and evaluated per
  // row by the sort. A constant orders nothing and is no key (nullopt): a bare
  // $n is typed as text where nothing before typed it, as PostgreSQL types
  // it there. random() and an aggregate are refused.
  auto order_key(const ast::Expr& e, const Resolve& resolve) -> std::optional<logical::Expr> {
    std::function<bool(const ast::Expr&, ast::Expr::Kind, const char*)> holds = [&](const ast::Expr& x, ast::Expr::Kind k,
                                                                                    const char* func) {
      if (x.kind == k && (func == nullptr || x.func == func)) return true;
      return (x.lhs && holds(*x.lhs, k, func)) || (x.rhs && holds(*x.rhs, k, func)) ||
             std::ranges::any_of(x.args, [&](const ast::Expr& a) { return holds(a, k, func); });
    };
    if (holds(e, ast::Expr::Kind::Func, "random")) not_yet("ORDER BY random()", e.pos);
    if (holds(e, ast::Expr::Kind::Aggregate, nullptr)) not_yet("an aggregate in ORDER BY", e.pos);
    if (e.kind == ast::Expr::Kind::Param) {
      if (params_.size() <= e.param || !params_[e.param].first) {
        const auto text = SqlType{SqlKind::Text};
        const auto& col = typed_params_.emplace_back(
            Column{0, "$" + std::to_string(e.param + 1), text, phys_type(text), true, std::monostate{}});
        use_param(e.param, phys_type(text), e.pos, col, 0, Use::Operand);
      }
      return std::nullopt;
    }
    auto key = plain_output(e, resolve).expr;
    if (key.kind == logical::Expr::Kind::Param || key.kind == logical::Expr::Kind::Const) return std::nullopt;
    if (key.type.kind == SqlKind::Numeric) refuse_numeric("ORDER BY a NUMERIC value", e.pos);
    return key;
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
  // the left arm's. An ORDER BY of the combined rows names output columns.
  auto set_operation(const ast::Select& s) -> logical::NodePtr {
    auto arm = set_arm(s, true);
    return std::move(arm.node);
  }

  // An ORDER BY item of a set operation's rows: an output column by name
  // (the left arm's) or by position, as PostgreSQL takes them, and nothing
  // else: an expression is 0A000, as there.
  static auto set_order(const ast::OrderItem& o, const std::vector<logical::Output>& outputs) -> logical::SetOp::Order {
    auto nulls_first = o.nulls_first.value_or(o.desc);
    if (o.position != 0) {
      if (std::cmp_greater(o.position, outputs.size())) {
        throw SqlError{"ORDER BY position " + std::to_string(o.position) + " is not in select list", o.pos,
                       kInvalidColumnReference};
      }
      return {static_cast<std::size_t>(o.position - 1), o.desc, nulls_first};
    }
    if (o.expr.kind != ast::Expr::Kind::Column) {
      throw SqlError{"invalid UNION/INTERSECT/EXCEPT ORDER BY clause", o.pos, kFeatureNotSupported};
    }
    const auto& name = o.expr.column;
    if (!o.expr.table.empty()) {
      throw SqlError{"missing FROM-clause entry for table \"" + o.expr.table + "\"", o.pos, kUndefinedTable};
    }
    std::optional<std::size_t> hit;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (outputs[i].name != name) continue;
      if (hit) throw SqlError{"ORDER BY \"" + name + "\" is ambiguous", o.pos, kAmbiguousColumn};
      hit = i;
    }
    if (!hit) throw SqlError{"column \"" + name + "\" does not exist", o.pos, kUndefinedColumn};
    return {*hit, o.desc, nulls_first};
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
      if (!root && !q.order_by.empty()) not_yet("ORDER BY on a parenthesised set operation inside another", q.pos);
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
        // UNION ALL only carries a NUMERIC; the others compare its rows.
        if (type.kind == SqlKind::Numeric && (op.kind != ast::SetOperation::Kind::Union || !op.all)) {
          refuse_numeric(what + " of a NUMERIC column", q.pos);
        }
        out.outputs.push_back({logical::param(0, type), lo[i].name});
        arm.untyped.push_back(l.untyped[i] && r.untyped[i]);
      }
      for (const auto& o : q.order_by) {
        out.order.push_back(set_order(o, out.outputs));
        if (out.outputs[out.order.back().column].expr.type.kind == SqlKind::Numeric) refuse_numeric("ORDER BY a NUMERIC column", o.pos);
      }
      out.left = std::move(l.node);
      out.right = std::move(r.node);
      arm.node = logical::node(std::move(out));
      if (q.limit) arm.node = logical::node(logical::Limit{std::move(arm.node), *q.limit});
      return arm;
    }
    if (q.for_update) throw SqlError{"FOR UPDATE is not allowed with UNION/INTERSECT/EXCEPT", q.pos, kFeatureNotSupported};
    if (q.offset != 0) not_yet("OFFSET in an arm of a set operation", q.pos);
    Arm arm;
    auto was = std::exchange(in_set_arm_, true);
    arm.node = select(q);
    in_set_arm_ = was;
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
    if ((a.kind == SqlKind::Numeric || b.kind == SqlKind::Numeric) && type_class(a.kind) == type_class(b.kind)) {
      refuse_numeric(what + " of " + pg_type_name(a) + " and " + pg_type_name(b), pos);
    }
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
      name_union(a, b, what, pos);
      if (a.kind == SqlKind::Varchar && b.kind == SqlKind::Varchar) return SqlType{SqlKind::Varchar};
      if (a.kind == SqlKind::Name && b.kind == SqlKind::Name) return SqlType{SqlKind::Name};
      return SqlType{SqlKind::Text};
    }
    refuse(pg_type_name(a) + " and " + pg_type_name(b));
  }

  // Above a left outer join every column of its right table may be NULL, in
  // the rows it NULL-extends, whatever the column's declaration: so marked,
  // the plan tests it. The join's own predicate and inputs read real rows;
  // a later join's ON is marked where it is bound.
  static void mark_nullable(logical::Expr& e, std::uint32_t table) {
    if (e.kind == logical::Expr::Kind::Column && e.col.table == table) e.col.nullable = true;
    for (auto& a : e.args) mark_nullable(a, table);
  }
  static void nullable_above_join(logical::Node& n, std::uint32_t table) {
    std::visit(
        [&](auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, logical::Join>) {
            return;
          } else {
            if constexpr (std::is_same_v<T, logical::Filter>) {
              for (auto& c : op.conjuncts) mark_nullable(c, table);
            } else if constexpr (std::is_same_v<T, logical::Project>) {
              for (auto& o : op.exprs) mark_nullable(o.expr, table);
            } else if constexpr (std::is_same_v<T, logical::Aggregate>) {
              for (auto& o : op.aggs) mark_nullable(o.expr, table);
              for (auto& g : op.group_by) mark_nullable(g, table);
              for (auto& g : op.dependent) mark_nullable(g, table);
            } else if constexpr (std::is_same_v<T, logical::Sort>) {
              for (auto& k : op.keys) mark_nullable(k.expr, table);
            }
            if constexpr (requires { op.input; }) nullable_above_join(*op.input, table);
          }
        },
        n.op);
  }

  // A select-list item an INSERT … SELECT binds against the column it
  // fills, as a VALUES item is (value): a constant, whose unknown literal
  // or NULL takes the column's type, and a $n, typed by the column.
  static auto assigned_later(const ast::Expr& e) -> bool { return is_constant(e) || e.kind == ast::Expr::Kind::Param; }

  // An INSERT … SELECT's select list as the values of the columns it fills,
  // checked as PostgreSQL checks them: their number (42601), then each in
  // order, a deferred item bound against its column, any other of a type
  // that assigns to it (assigns_to). Sets assigned_.
  void assign_outputs(const AssignTo& to, const std::vector<ast::Expr>& list, const std::vector<bool>& deferred,
                      std::vector<logical::Output>& targets, std::int32_t pos) {
    auto columns = to.columns;
    if (targets.size() > columns.size()) {
      throw SqlError{"INSERT has more expressions than target columns", list[columns.size()].pos};
    }
    if (to.listed && targets.size() < columns.size()) throw SqlError{"INSERT has more target columns than expressions", pos};
    columns.resize(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
      if (deferred[i]) {
        auto name = std::move(targets[i].name);
        targets[i] = {value(*to.table, list[i], *columns[i]), std::move(name)};
      } else {
        assigns_to(targets[i].expr.type, *columns[i], list[i].pos);
      }
    }
    assigned_ = std::move(columns);
  }

  // A value of type `from` written into column c by INSERT … SELECT: one of
  // c's type or of its family, which the row builder range-checks (an
  // integer into a SMALLINT, a TEXT into a VARCHAR(n)), a REAL or DOUBLE
  // PRECISION into the other, or an integer into either, which the plan
  // converts. A pair PostgreSQL refuses is 42804; any other assignment cast
  // it makes is not taken yet (0A000, #824).
  static void assigns_to(SqlType from, const Column& c, std::int32_t pos) {
    const auto to = c.sql;
    if (from.kind == SqlKind::Numeric && to.kind == SqlKind::Numeric) return;  // its text, as it is stored
    if (from.kind != SqlKind::Numeric && assignable(from, to) && (to.kind != SqlKind::Char || from == to)) return;
    if (is_float(to) && (is_float(from) || is_integer(from))) return;
    if (auto refusal = type_refusal(from.kind, to.kind, true)) {
      throw SqlError{"column \"" + c.name + "\" is of type " + pg_type_name(to) + " but expression is of type " + pg_type_name(from),
                     pos, *refusal};
    }
    if (from.kind == SqlKind::Numeric || to.kind == SqlKind::Numeric) {
      refuse_numeric("INSERT … SELECT of a " + pg_type_name(from) + " value into column \"" + c.name + "\" of type " + pg_type_name(to), pos);
    }
    const auto from_name = pg_type_name(from);
    const std::string article = from_name.find_first_of("aeiou") == 0 ? "an " : "a ";
    not_supported("INSERT … SELECT of " + article + from_name + " value into column \"" + c.name + "\" of type " +
                      pg_type_name(to),
                  pos);
  }

  // A select-list item of no aggregated query, named as PostgreSQL names
  // it: a column by its name, a CASE, COALESCE or NULLIF by the word, a
  // function by its own, $n::T by its type, any other expression ?column?.
  auto plain_output(const ast::Expr& e, const Resolve& resolve) -> logical::Output {
    if (e.kind == ast::Expr::Kind::Clock) return clock(e);
    if (e.kind == ast::Expr::Kind::Subquery) return subquery_output(e);
    if (is_constant(e)) return constant_output(e);
    if (e.kind != ast::Expr::Kind::Column) {
      auto name = e.kind == ast::Expr::Kind::Case                       ? std::string{"case"}
                  : e.kind == ast::Expr::Kind::Coalesce                 ? std::string{"coalesce"}
                  : e.kind == ast::Expr::Kind::NullIf                   ? std::string{"nullif"}
                  : e.kind == ast::Expr::Kind::Func && e.func != "||" && e.func != "%" ? e.func
                  : param_cast(e)                                       ? e.cast_type
                  // x::T is named by x, as PostgreSQL names it.
                  : e.kind == ast::Expr::Kind::Cast && e.rhs->kind == ast::Expr::Kind::Column ? e.rhs->column
                  : e.kind == ast::Expr::Kind::Cast && e.rhs->kind == ast::Expr::Kind::Clock
                      ? (e.rhs->column.empty() ? e.rhs->func : e.rhs->column)
                                                                        : std::string{"?column?"};
      return {value(e, resolve), name};
    }
    auto [t, c] = resolve(e);
    return {logical::column_of(t->id, *c), c->name};
  }

  // RETURNING's list over the row `t` wrote: a column of the row as
  // inserted or updated, or as it was before a DELETE (the plan binds the
  // one row it has in hand); * is every column. No aggregate (42803).
  auto returning(const Table& t, const std::vector<ast::Expr>& list) -> std::vector<logical::Output> {
    if (list.empty()) return {};
    ast::Select s;
    for (const auto& e : list) {
      if (e.kind == ast::Expr::Kind::Aggregate) {
        throw SqlError{"aggregate functions are not allowed in RETURNING", e.pos, kGroupingError};
      }
      s.targets.push_back(ast::clone(e));
      s.targets.back().alias = e.alias;
    }
    auto expanded = expand_stars(s, {&t});
    Resolve resolve = [&](const ast::Expr& e) -> std::pair<const Table*, const Column*> { return {&t, column(t, e)}; };
    std::vector<logical::Output> out;
    for (const auto& e : expanded) out.push_back(plain_output(e, resolve));
    named(expanded, out);
    return out;
  }

  // A select-list item's AS name is its output column's.
  static void named(const std::vector<ast::Expr>& list, std::vector<logical::Output>& targets) {
    for (std::size_t i = 0; i < targets.size(); ++i) {
      if (!list[i].alias.empty()) targets[i].name = list[i].alias;
    }
  }

  // The select list with each * and `table`.* replaced by the columns it
  // stands for, in table order, hidden ones left out; a * over a join, the
  // join's columns (JoinScope), each USING or NATURAL column once.
  auto expand_stars(const ast::Select& s, const std::vector<const Table*>& from) -> std::vector<ast::Expr> {
    std::vector<ast::Expr> out;
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
      // Each column named by its relation: another may have a column of the name too.
      if (const auto* joined = joins_.level(from.size()); e.table.empty() && joined != nullptr) {
        for (const auto& c : *joined) out.push_back(column_expr(*c.table, *c.col, e.pos));
        continue;
      }
      auto any = false;
      for (const auto* t : from) {
        if (!e.table.empty() && e.table != t->name) continue;
        any = true;
        for (const auto& c : t->columns) {
          if (!c.hidden) out.push_back(column_expr(*t, c, e.pos));
        }
      }
      if (!any) throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
    }
    return out;
  }

  // A SELECT without FROM: one row of its items, under its LIMIT. An item
  // is any select-list expression with no column in it: a column is a
  // column of no table (42703), as in PostgreSQL.
  auto select_without_from(const ast::Select& s, const std::optional<AssignTo>& assign) -> logical::NodePtr {
    for (const auto& e : s.targets) {
      if (e.kind == ast::Expr::Kind::Star) throw SqlError{"SELECT * with no tables specified is not valid", e.pos};
    }
    if (!s.where.empty() || !s.group_by.empty() || !s.having.empty() || !s.order_by.empty() || s.for_update) {
      not_yet("a SELECT without FROM with WHERE, GROUP BY, HAVING, ORDER BY or FOR UPDATE", s.pos);
    }
    Resolve no_table = [](const ast::Expr& e) -> std::pair<const Table*, const Column*> {
      if (!e.table.empty()) throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
      throw SqlError{"column \"" + e.column + "\" does not exist", e.pos, kUndefinedColumn};
    };
    std::vector<logical::Output> targets;
    std::vector<bool> deferred(s.targets.size(), false);
    for (std::size_t i = 0; i < s.targets.size(); ++i) {
      const auto& e = s.targets[i];
      deferred[i] = assign && assigned_later(e);
      targets.push_back(deferred[i] ? logical::Output{logical::Expr{}, {}} : plain_output(e, no_table));
    }
    named(s.targets, targets);
    if (assign) assign_outputs(*assign, s.targets, deferred, targets, s.pos);
    auto rows = logical::node(logical::Project{logical::node(logical::OneRow{}), std::move(targets)});
    rows = limited(std::move(rows), s);
    return rows;
  }

  // One HAVING conjunct: a grouped column or an aggregate call compared with
  // a parameter, in either order. The parameter takes the column's type, or
  // the aggregate's: COUNT's and SUM's of a SMALLINT or INTEGER a BIGINT,
  // SUM's of a BIGINT or DECIMAL the numeric at its scale, checked as a
  // BIGINT is (compared_type), MIN's and MAX's their column's; SUM's, MIN's
  // and MAX's of a DOUBLE PRECISION a double precision, compared in
  // PostgreSQL's float8 order. HAVING on AVG is not supported: its quotient
  // is numeric text (PhysType::Num), which a plan does not compare.
  auto having_conjunct(const ast::Compare& w, const auto& resolve, const auto& grouped) -> logical::Expr {
    if (w.kind != ast::Compare::Kind::Compare) not_yet("HAVING with AND, OR, NOT, IS NULL, <>, BETWEEN, IN or a constant", w.pos);
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
    // The call is the plan's aggregate of the groups that the select list's
    // alike call is (the planner finds it by its bound expression), or one of
    // its own.
    const Column* arg = nullptr;
    std::uint32_t table_id = 0;
    if (!lhs->star && lhs->args.at(0).kind == ast::Expr::Kind::Column) {
      auto [t, c] = resolve(lhs->args[0]);
      arg = c;
      table_id = t->id;
    }
    auto agg = aggregate(aggregate_arg(*lhs, resolve), *lhs);
    if (agg.expr.fn == logical::Expr::Fn::Avg) throw SqlError{"HAVING on AVG is not supported yet", lhs->pos};
    auto phys = agg.expr.fn != logical::Expr::Fn::Count && agg.expr.args[0].type.kind == SqlKind::Double ? PhysType::F64
                                                                                                        : PhysType::I64;
    const Column* as = arg;
    if (agg.expr.fn == logical::Expr::Fn::Count || agg.expr.fn == logical::Expr::Fn::Sum || arg == nullptr) {
      synthetic_.push_back(Column{0, agg.name, agg.expr.type, phys, true, {}});
      as = &synthetic_.back();
    }
    if (constant) {
      return logical::compare(op, std::move(agg.expr), fixed(coerce(fold(*rhs), *as, false, op_text(op)), as->sql));
    }
    auto sql = having_param(rhs->param, phys, rhs->pos, *as, table_id);
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
    for (const auto& p : params_) {
      // A refusal names the table the rows are in, not an instance of it.
      // A derived relation's, the table its query reads first.
      if (Table::derived(p.table)) {
        out.push_back(derived_.at(p.table - Table::kDerivedBase).source);
        continue;
      }
      out.push_back(p.table >= Table::kInstanceBase ? instances_.at(p.table - Table::kInstanceBase)->source : p.table);
    }
    return out;
  }

  // A cursor's query is a simple scan of one table (ast::Select::row_key):
  // select() appended the table's primary key to the result, key_columns()
  // of it, as hidden trailing columns.
  [[nodiscard]] auto key_columns() const -> std::uint32_t { return key_columns_; }
  [[nodiscard]] auto key_table() const -> std::uint32_t { return key_table_; }
  // The query reads a view, whose rows are no table's: no key is added.
  void forbid_row_key() { row_key_allowed_ = false; }

  // INSERT … SELECT: the next select() binds its rows as the values of
  // `table`'s `columns`, in order (assign_outputs).
  void assign(AssignTo to) { assign_ = std::move(to); }
  // The columns the select()'s outputs fill, one per output.
  [[nodiscard]] auto take_assigned() -> std::vector<const Column*> { return std::move(assigned_); }

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
  std::optional<AssignTo> assign_;          // see assign()
  std::vector<const Column*> assigned_;     // see take_assigned()
  bool row_key_allowed_{true};
  std::uint32_t key_columns_{0};
  std::uint32_t key_table_{0};
  JoinScope joins_;  // the join being bound: its columns, as unqualified names see them (join_scope)
  std::vector<std::unique_ptr<Table>> instances_;  // see instance()
  // The derived relations bound (derived_relation): each table, the plan
  // of its query until select() takes it, and the table its first FROM item
  // reads, which a refusal of a parameter compared with its column names.
  struct DerivedRelation {
    std::unique_ptr<Table> table;
    logical::NodePtr plan;
    std::uint32_t source{0};
  };
  std::vector<DerivedRelation> derived_;
  std::vector<ParamUse> params_;
  std::size_t fixed_params_{0};      // $1 .. $fixed_params_ were typed by fix_params
  std::deque<Column> typed_params_;  // what a $n typed by its place (function_value) is used as; never moves
  std::vector<Datum> fixed_;  // the constants' values, $own_params_ + 1 onwards
  std::vector<PhysType> fixed_types_;
  std::vector<SqlType> fixed_sql_;
  std::vector<ClockSlot> clocks_;
  std::vector<logical::Subquery> subqueries_;  // in the order bound; InSet names one by its index
  int depth_{0};                               // inside a subquery: one more is refused
  std::vector<const Table*> scope_;            // the tables of the query being bound
  // The correlated subquery being bound (correlation): its query, the outer
  // query's tables, and the arguments it has taken so far.
  struct Correlation {
    const ast::Select* query;
    std::vector<const Table*> outer;
    std::vector<logical::Subquery::Arg> args;
  };
  std::optional<Correlation> correlating_;
  bool in_where_{false};  // binding a one-table query's WHERE (where)
  bool in_set_arm_{false};  // binding an arm of a set operation (set_arm)
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
  if (e.kind == ast::Expr::Kind::Column && e.table.empty()) {
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
    case K::Exists:
    case K::Const: break;
  }
}

// Replaces each column of `e` that `swap` answers for, an aggregate's
// argument's among them. A subquery is left: it reads no column of the
// query around it.
void swap_columns(ast::Expr& e, const ColumnSwap& swap) {
  if (e.kind == ast::Expr::Kind::Column) {
    if (auto repl = swap(e)) {
      repl->pos = e.pos;
      e = std::move(*repl);
    }
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
    case K::Exists:
    case K::Const: break;
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
    case K::Exists:
    case K::Const: return true;
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

  // A list of expressions copied, each with its AS name.
  static auto cloned(const std::vector<ast::Expr>& list) -> std::vector<ast::Expr> {
    std::vector<ast::Expr> out;
    for (const auto& e : list) {
      out.push_back(ast::clone(e));
      out.back().alias = e.alias;
    }
    return out;
  }

  // The statement with its views expanded; nullopt where it reads none.
  [[nodiscard]] auto statement(const ast::Statement& stmt) -> std::optional<ast::Statement> {
    if (const auto* s = std::get_if<ast::Select>(&stmt)) {
      if (!mentions(*s)) return std::nullopt;
      return std::optional<ast::Statement>{std::in_place, std::in_place_type<ast::Select>, expand(*s)};
    }
    if (const auto* i = std::get_if<ast::Insert>(&stmt)) {
      const auto* view = cat_.view(i->table);
      auto found = view != nullptr;
      ast::Insert out{i->table, i->columns, {}, i->pos, cloned(i->returning), i->query, i->on_conflict};
      if (i->query && mentions(*i->query)) {
        out.query = std::make_shared<const ast::Select>(expand(*i->query));
        found = true;
      }
      auto expand_slot = [&](std::shared_ptr<const ast::Select>& q) {
        if (!mentions(*q)) return;
        q = std::make_shared<const ast::Select>(expand(*q));
        found = true;
      };
      for (const auto& row : i->rows) {
        out.rows.push_back(cloned(row));
        for (auto& e : out.rows.back()) subqueries_in(e, expand_slot);
      }
      if (view != nullptr) into_base_table(out, *view);
      for (auto& e : out.returning) subqueries_in(e, expand_slot);
      if (!found) return std::nullopt;
      return std::optional<ast::Statement>{std::in_place, std::in_place_type<ast::Insert>, std::move(out)};
    }
    if (const auto* u = std::get_if<ast::Update>(&stmt)) {
      ast::Update out{u->table, {}, {}, u->pos, u->current_of};
      for (const auto& a : u->set) out.set.push_back({a.column, ast::clone(a.value), a.pos});
      for (const auto& c : u->where) out.where.push_back(ast::clone(c));
      out.returning = cloned(u->returning);
      if (!rewrite(out.table, out.set, out.where)) return std::nullopt;
      return std::optional<ast::Statement>{std::in_place, std::in_place_type<ast::Update>, std::move(out)};
    }
    if (const auto* d = std::get_if<ast::Delete>(&stmt)) {
      ast::Delete out{d->table, {}, d->pos, d->current_of};
      for (const auto& c : d->where) out.where.push_back(ast::clone(c));
      out.returning = cloned(d->returning);
      std::vector<ast::Assignment> none;
      if (!rewrite(out.table, none, out.where)) return std::nullopt;
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
      b.select.targets = scratch.expand_stars(b.select, scratch.relations(b.select));
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
    for (const auto& item : s.from) {
      if (const auto* t = cat_.table(item.table)) out.push_back(t->id);
      if (const auto* v = cat_.view(item.table)) out.push_back(v->id);
    }
    each_subquery(s, [&](const std::shared_ptr<const ast::Select>& q) { relations(*q, out); });
  }

private:
  static constexpr int kMaxDepth = 16;

  // INSERT INTO view: written to the view's one base table, as PostgreSQL's
  // automatically updatable views are (docs/frontend_design.md, *Views*).
  // The view's columns the statement fills become the base columns they
  // are, its RETURNING the view's columns over the row written. The view's
  // WHERE does not apply: there is no WITH CHECK OPTION. Errors come in
  // PostgreSQL's order: the column list, the row's width, the view's shape
  // (55000), each column filled (0A000), two filling one base column (42601).
  void into_base_table(ast::Insert& s, const View& v) {
    if (s.on_conflict) not_supported("INSERT … ON CONFLICT into a view", s.on_conflict->pos);
    const auto& b = body(v);
    std::vector<std::string> filled;
    if (s.columns.empty()) {
      auto width = s.query ? query_width(*s.query) : s.rows.front().size();
      if (width > v.columns.size()) throw SqlError{"INSERT has more expressions than target columns", s.pos};
      filled.assign(v.columns.begin(), v.columns.begin() + static_cast<std::ptrdiff_t>(width));
    } else {
      for (const auto& name : s.columns) {
        if (std::ranges::find(v.columns, name) == v.columns.end()) {
          throw SqlError{"column \"" + name + "\" of relation \"" + v.name + "\" does not exist", s.pos, kUndefinedColumn};
        }
        if (std::ranges::contains(filled, name)) {
          throw SqlError{"column \"" + name + "\" specified more than once", s.pos, kDuplicateColumn};
        }
        filled.push_back(name);
      }
    }
    // Each view of the chain down to the table, its own definition as
    // written, as PostgreSQL rewrites them one at a time: its shape, then the
    // columns filled, each mapped to its relation's, which must differ.
    std::string table;
    for (const auto* at = &v; at != nullptr;) {
      auto parsed = ast::parse(at->definition);
      const auto& q = std::get<ast::CreateView>(parsed).query;
      if (auto why = not_updatable(q); !why.empty()) {
        throw SqlError{"cannot insert into view \"" + at->name + "\"", s.pos, kObjectNotInPrerequisiteState}.with_notes(
            why, "To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.");
      }
      const auto& item = q.from[0];
      // The FROM item's columns as its relation names them, and as the query does.
      auto inner = relation_columns(item.table);
      auto named = inner;
      for (std::size_t k = 0; k < item.column_aliases.size() && k < named.size(); ++k) named[k] = item.column_aliases[k];
      std::vector<ast::Expr> items;
      for (const auto& e : q.targets) {
        if (e.kind != ast::Expr::Kind::Star) {
          items.push_back(ast::clone(e));
          continue;
        }
        for (const auto& name : named) items.push_back(column_named(e, "", name));
      }
      std::vector<std::string> next;
      for (const auto& name : filled) {
        const auto& e = items.at(static_cast<std::size_t>(std::ranges::find(at->columns, name) - at->columns.begin()));
        if (e.kind != ast::Expr::Kind::Column) {
          throw SqlError{"cannot insert into column \"" + name + "\" of view \"" + at->name + "\"", s.pos, kFeatureNotSupported}
              .with_notes("View columns that are not columns of their base relation are not updatable.", "");
        }
        auto k = std::ranges::find(named, e.column);
        if (k == named.end()) not_yet("inserting into a view column that is no column of its relation", s.pos);
        const auto& column = inner[static_cast<std::size_t>(k - named.begin())];
        if (std::ranges::contains(next, column)) throw SqlError{"multiple assignments to same column \"" + column + "\"", -1};
        next.push_back(column);
      }
      filled = std::move(next);
      table = item.table;
      at = cat_.view(item.table);
    }
    // RETURNING reads the view's body, merged down to the table.
    const auto& item = b.select.from.at(0);
    const auto* base = cat_.table(item.table);
    if (base == nullptr || base->name != table) throw std::logic_error{"an updatable view's body reads another table"};
    auto visible = relation_columns(table);
    auto base_name = [&](const std::string& column) {
      auto at = std::ranges::find(item.column_aliases, column);
      if (at == item.column_aliases.end()) return column;
      return visible.at(static_cast<std::size_t>(at - item.column_aliases.begin()));
    };
    auto item_of = [&](const std::string& name) -> const ast::Expr& {
      return b.select.targets.at(static_cast<std::size_t>(std::ranges::find(v.columns, name) - v.columns.begin()));
    };
    s.table = table;
    s.columns = std::move(filled);
    // RETURNING: * is the view's columns; each the item it is, over the base row.
    std::vector<ast::Expr> returning;
    for (auto& e : s.returning) {
      if (e.kind != ast::Expr::Kind::Star) {
        returning.push_back(std::move(e));
        continue;
      }
      if (!e.table.empty() && e.table != v.name) {
        throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
      }
      for (const auto& name : v.columns) returning.push_back(column_named(e, "", name));
    }
    auto to_base = [&](ast::Expr& ref) -> std::optional<ast::Expr> { return column_named(ref, "", base_name(ref.column)); };
    auto swap = [&](ast::Expr& ref) -> std::optional<ast::Expr> {
      if (!ref.table.empty() && ref.table != v.name) {
        throw SqlError{"missing FROM-clause entry for table \"" + ref.table + "\"", ref.pos, kUndefinedTable};
      }
      if (std::ranges::find(v.columns, ref.column) == v.columns.end()) {
        throw SqlError{"column \"" + ref.column + "\" does not exist", ref.pos, kUndefinedColumn};
      }
      auto out = ast::clone(item_of(ref.column));
      swap_columns(out, to_base);
      return out;
    };
    for (auto& e : returning) {
      auto name = e.kind == ast::Expr::Kind::Column ? (e.alias.empty() ? e.column : e.alias) : e.alias;
      swap_columns(e, swap);
      e.alias = name;
    }
    s.returning = std::move(returning);
  }

  // A table's or view's columns, in order; a table's hidden row id is none.
  auto relation_columns(const std::string& name) const -> std::vector<std::string> {
    if (const auto* w = cat_.view(name)) return w->columns;
    std::vector<std::string> out;
    for (const auto& c : cat_.table(name)->columns) {
      if (!c.hidden) out.push_back(c.name);
    }
    return out;
  }

  // Why PostgreSQL would not write through a view of this definition, its
  // DETAIL; empty where it would.
  static auto not_updatable(const ast::Select& q) -> std::string {
    auto aggregates = [](const ast::Expr& e) { return has_aggregate(e); };
    if (q.distinct) return "Views containing DISTINCT are not automatically updatable.";
    if (!q.group_by.empty()) return "Views containing GROUP BY are not automatically updatable.";
    if (!q.having.empty()) return "Views containing HAVING are not automatically updatable.";
    if (q.set) return "Views containing UNION, INTERSECT, or EXCEPT are not automatically updatable.";
    if (q.limit_written) return "Views containing LIMIT or OFFSET are not automatically updatable.";
    if (std::ranges::any_of(q.targets, aggregates) ||
        std::ranges::any_of(q.order_by, [&](const ast::OrderItem& o) { return aggregates(o.expr); })) {
      return "Views that return aggregate functions are not automatically updatable.";
    }
    if (q.from.size() != 1) return "Views that do not select from a single table or view are not automatically updatable.";
    return {};
  }

  static auto has_aggregate(const ast::Expr& e) -> bool {
    if (e.kind == ast::Expr::Kind::Aggregate) return true;
    return (e.lhs && has_aggregate(*e.lhs)) || (e.rhs && has_aggregate(*e.rhs)) ||
           std::ranges::any_of(e.args, [](const ast::Expr& a) { return has_aggregate(a); });
  }

  // The number of columns a query gives, its stars written out.
  auto query_width(const ast::Select& q) -> std::size_t {
    if (q.set) return query_width(q.set->left);
    if (std::ranges::none_of(q.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Star; })) {
      return q.targets.size();
    }
    Binder scratch{cat_, 0};
    return scratch.expand_stars(q, scratch.relations(q)).size();
  }

  // A correlated subquery where a view is read, around it or in it: the
  // view's merge does not rewrite the references that cross the subquery's
  // edge (#836). Correlated: it names a column of a relation of `outer`,
  // the FROM around it, qualified by its name and none of the subquery's,
  // or unqualified, of none of the subquery's relations.
  void refuse_correlated(const std::vector<ast::FromItem>& outer, const ast::Select& q) const {
    if (q.set) {
      refuse_correlated(outer, q.set->left);
      refuse_correlated(outer, q.set->right);
      return;
    }
    auto columns_of = [&](const ast::FromItem& item) -> std::vector<std::string> {
      if (item.derived) return item.column_aliases;
      if (cat_.table(item.table) == nullptr && cat_.view(item.table) == nullptr) return {};
      return relation_columns(item.table);
    };
    auto has = [&](const std::vector<ast::FromItem>& from, const std::string& column) {
      return std::ranges::any_of(from, [&](const ast::FromItem& item) { return std::ranges::contains(columns_of(item), column); });
    };
    auto named = [](const std::vector<ast::FromItem>& from, const std::string& name) {
      return std::ranges::any_of(from, [&](const ast::FromItem& item) { return item.name() == name; });
    };
    auto outer_ref = [&](const ast::Expr& e) {
      if (!e.table.empty()) return !named(q.from, e.table) && named(outer, e.table);
      return !has(q.from, e.column) && has(outer, e.column);
    };
    std::function<void(const ast::Expr&)> expr = [&](const ast::Expr& e) {
      if (e.kind == ast::Expr::Kind::Column && outer_ref(e)) not_yet("a correlated subquery where a view is read", e.pos);
      if (e.lhs) expr(*e.lhs);
      if (e.rhs) expr(*e.rhs);
      for (const auto& a : e.args) expr(a);
    };
    std::function<void(const ast::Compare&)> compare = [&](const ast::Compare& c) {
      expr(c.lhs);
      expr(c.rhs);
      for (const auto& a : c.args) compare(a);
    };
    for (const auto& e : q.targets) expr(e);
    for (const auto& e : q.group_by) expr(e);
    for (const auto& o : q.order_by) expr(o.expr);
    for (const auto& c : q.where) compare(c);
    for (const auto& c : q.having) compare(c);
  }

  // A FROM item that names a view still to expand: not one already read
  // as a derived relation.
  [[nodiscard]] auto names_view(const ast::FromItem& item) const -> bool {
    return item.derived == nullptr && cat_.view(item.table) != nullptr;
  }

  [[nodiscard]] auto mentions(const ast::Select& s) const -> bool {
    if (s.set) return mentions(s.set->left) || mentions(s.set->right);
    auto found = std::ranges::any_of(s.from, [&](const ast::FromItem& item) { return names_view(item); });
    each_subquery(s, [&](const std::shared_ptr<const ast::Select>& q) { found = found || mentions(*q); });
    return found;
  }

  // Whether a view is named in the SET values or WHERE, subqueries included;
  // if so they are rewritten in place.
  auto rewrite(const std::string& table, std::vector<ast::Assignment>& set, std::vector<ast::Compare>& where) -> bool {
    auto found = false;
    ast::FromItem written;
    written.table = table;
    const std::vector<ast::FromItem> outer{written};
    auto expand_slot = [&](std::shared_ptr<const ast::Select>& q) {
      if (!mentions(*q)) return;
      refuse_correlated(outer, *q);
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
    auto views = std::ranges::count_if(s.from, [&](const ast::FromItem& item) { return names_view(item); });
    auto expand_slot = [&](std::shared_ptr<const ast::Select>& q) {
      auto inner = mentions(*q);
      if (views > 0 || inner) refuse_correlated(s.from, *q);
      if (inner) q = std::make_shared<const ast::Select>(expand(*q));
    };
    each_subquery(s, expand_slot);
    if (views == 0) return s;
    if (std::ranges::any_of(s.from, [](const ast::FromItem& item) { return !item.alias.empty(); })) {
      not_yet("a view in a query that names a table twice", s.from[0].pos);
    }
    if (s.from.size() == 1) {
      merge_one(s, body(*cat_.view(s.from[0].table)));
    } else if (s.from.size() == 2) {
      merge_two(s, cat_.view(s.from[0].table), cat_.view(s.from[1].table));
    } else {
      not_yet("a view in a join of three or more tables", s.from[0].pos);
    }
    return s;
  }

  // FROM view [WHERE …]: the view's FROM, and its WHERE and the query's
  // together; over a grouped view the query's WHERE is a HAVING, the groups
  // it keeps. Where the query groups or aggregates again, it is merged only
  // where a group is a row of the view (regroup, below) or its aggregates
  // cannot tell a row from its base rows (rows_of_base); else it reads the
  // view as a derived relation (derive). It may not read through a view that
  // cuts rows (DISTINCT, LIMIT).
  void merge_one(ast::Select& s, const ViewBody& v) {
    const auto& view = v.select;
    const auto& name = s.from[0].table;
    if (view.set) not_yet("reading a view defined with a set operation", s.from[0].pos);
    auto grouped = aggregated(view);
    if (view.distinct || view.limit || view.offset != 0) not_yet("a view defined with DISTINCT, LIMIT or OFFSET", s.from[0].pos);
    if (view.for_update) not_yet("a view defined FOR UPDATE", s.from[0].pos);
    auto again = aggregated(s) || s.distinct;
    // Decided on a copy, before the query is changed: a merge that cannot
    // say it (a GROUP BY of a view column that is an expression, an
    // aggregate of an aggregate) is a derived relation instead.
    auto merged = ast::clone(s);
    std::vector<ast::Expr> targets;
    for (auto& e : merged.targets) {
      if (e.kind != ast::Expr::Kind::Star) {
        targets.push_back(std::move(e));
        continue;
      }
      if (!e.table.empty() && e.table != name) {
        throw SqlError{"missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
      }
      if (!e.alias.empty()) throw SqlError{"syntax error at or near \"AS\"", e.pos};
      for (const auto& column : v.names) targets.push_back(column_named(e, "", column));
    }
    merged.targets = std::move(targets);
    auto swap = [&](ast::Expr& ref) -> std::optional<ast::Expr> {
      if (!ref.table.empty() && ref.table != name) {
        throw SqlError{"missing FROM-clause entry for table \"" + ref.table + "\"", ref.pos, kUndefinedTable};
      }
      auto at = std::ranges::find(v.names, ref.column);
      if (at == v.names.end()) throw SqlError{"column \"" + ref.column + "\" does not exist", ref.pos, kUndefinedColumn};
      return ast::clone(view.targets[static_cast<std::size_t>(at - v.names.begin())]);
    };
    if (grouped && again && groups_by_expression(merged, v)) {
      derive(s, v);
      return;
    }
    apply(merged, swap);
    if (grouped && again && rows_of_base(view) && duplicate_blind(merged)) {
      // The view's rows are base-row values: the query reads the base rows,
      // those its HAVING (over group columns, so a WHERE) keeps.
      for (const auto& c : view.having) merged.where.push_back(ast::clone(c));
    } else if (grouped && again) {
      if (!regroup(merged, view)) {
        derive(s, v);
        return;
      }
    }
    s = std::move(merged);
    if (grouped && !again) {
      s.having.clear();
      for (const auto& c : view.having) s.having.push_back(ast::clone(c));
      for (auto& c : s.where) s.having.push_back(std::move(c));
      s.where.clear();
      s.group_by.clear();
      for (const auto& g : view.group_by) s.group_by.push_back(ast::clone(g));
    }
    // The query takes the view's FROM, and its WHERE joins the query's.
    s.from = view.from;
    std::vector<ast::Compare> where;
    for (const auto& c : view.where) where.push_back(ast::clone(c));
    for (auto& c : s.where) where.push_back(std::move(c));
    s.where = std::move(where);
    if (s.order_by.empty() && !s.distinct) s.order_by = ast::clone(view.order_by);
  }

  // Whether the query groups by a column of the view that is an expression
  // of its rows, an aggregate among them: no column of the base rows.
  static auto groups_by_expression(const ast::Select& s, const ViewBody& v) -> bool {
    return std::ranges::any_of(s.group_by, [&](const ast::Expr& g) {
      if (g.kind != ast::Expr::Kind::Column) return false;
      auto at = std::ranges::find(v.names, g.column);
      return at != v.names.end() && v.select.targets[static_cast<std::size_t>(at - v.names.begin())].kind != ast::Expr::Kind::Column;
    });
  }

  // The query reads the grouped view as a relation of its own, a derived
  // table the binder makes from the view's query (Binder::derived_relation):
  // its rows are the view's groups, which the query groups, filters and
  // aggregates as a table's rows. The view's ORDER BY orders nothing the
  // query sees. One level: a view that already reads one is refused.
  void derive(ast::Select& s, const ViewBody& v) {
    if (std::ranges::any_of(v.select.from, [](const ast::FromItem& item) { return item.derived != nullptr; })) {
      not_yet("aggregating over a view that aggregates over a grouped view", s.pos);
    }
    ast::FromItem item;
    item.table = s.from[0].table;
    item.pos = s.from[0].pos;
    derived_item(item, v);
    s.from = {std::move(item)};
  }

  // A grouped view without aggregates, whose HAVING tests group columns alone,
  // selects columns of base rows, one row per distinct group: the same values
  // as the base rows it keeps, fewer times.
  static auto rows_of_base(const ast::Select& view) -> bool {
    return !view.group_by.empty() &&
           std::ranges::all_of(view.having, [&](const ast::Compare& c) { return over_group(c, view.group_by); }) &&
           std::ranges::all_of(view.targets, [](const ast::Expr& e) { return e.kind == ast::Expr::Kind::Column; });
  }

  // A query, already substituted, whose aggregates cannot tell a value once
  // from a value many times (MIN, MAX, COUNT(DISTINCT)), and has at least one.
  static auto duplicate_blind(const ast::Select& s) -> bool {
    using Fn = ast::Expr::Fn;
    auto blind = [](const ast::Expr& e) {
      return e.fn == Fn::Min || e.fn == Fn::Max || (e.fn == Fn::Count && e.distinct);
    };
    auto aggregates = 0;
    for (const auto& e : s.targets) {
      if (e.kind != ast::Expr::Kind::Aggregate) continue;
      if (!blind(e)) return false;
      ++aggregates;
    }
    return !s.distinct && s.having.empty() && aggregates > 0;
  }

  // A query that groups again over a grouped view, already substituted, is
  // answered from the view's own query where that is the same answer: GROUP BY
  // the view's group columns, no aggregate, only those columns read. A view
  // row is a group, so grouping the rows by what groups them again is the
  // view; the query's WHERE and HAVING are the view's HAVING. Any other
  // reads the view as a derived relation (derive).
  static auto regroup(ast::Select& s, const ast::Select& view) -> bool {
    const auto& group = view.group_by;
    auto in_group = [&](const ast::Expr& e) { return is_group_column(e, group); };
    auto ordered_by_group = [&](const ast::OrderItem& o) { return is_group_column(o.expr, group); };
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
  // column keeps its view name; a grouping column must stay a column.
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
    for (auto& o : s.order_by) swap_columns(o.expr, swap);
  }

  // FROM view JOIN table, a table JOIN view, or two views, inner or outer:
  // each view a single table's plain SELECT, replaced by its table, its
  // columns by its items qualified with it, its WHERE added to the query's.
  // A grouped view joined with a table is kept as a derived relation
  // (ast::FromItem::derived), its query whole, its columns named by it: the
  // binder reads its groups as a table's rows, USING and NATURAL included.
  struct Side {
    std::string name;
    const ViewBody* view{nullptr};
    const Table* table{nullptr};
    bool derived{false};
  };
  void merge_two(ast::Select& s, const View* left, const View* right) {
    const auto& join = s.from[1];
    Side sides[2]{{s.from[0].table, left != nullptr ? &body(*left) : nullptr, cat_.table(s.from[0].table)},
                  {join.table, right != nullptr ? &body(*right) : nullptr, cat_.table(join.table)}};
    for (auto i = 0; i < 2; ++i) {
      if (sides[i].view == nullptr && sides[i].table == nullptr) {
        throw SqlError{"relation \"" + sides[i].name + "\" does not exist", s.from[static_cast<std::size_t>(i)].pos, kUndefinedTable};
      }
    }
    auto outer = join.kind != ast::JoinKind::Inner;
    for (auto& side : sides) {
      if (side.view == nullptr) continue;
      const auto& v = side.view->select;
      side.derived = !v.set && aggregated(v) && !v.distinct && !v.limit && v.offset == 0 && !v.for_update;
      if (side.derived && std::ranges::any_of(v.from, [](const ast::FromItem& item) { return item.derived != nullptr; })) {
        not_yet("a join of a view that aggregates over a grouped view", join.pos);
      }
    }
    auto merged_view = [](const Side& side) { return side.view != nullptr && !side.derived; };
    if ((sides[0].derived || sides[1].derived) && (sides[0].view != nullptr && sides[1].view != nullptr)) {
      not_yet("a join of a grouped view with another view", join.pos);
    }
    if ((!join.using_columns.empty() || join.natural) && (merged_view(sides[0]) || merged_view(sides[1]))) {
      not_yet("USING or NATURAL join of a view", join.pos);
    }
    for (const auto& side : sides) {
      if (!merged_view(side)) continue;
      const auto& v = side.view->select;
      if (v.from.size() != 1 || aggregated(v) || v.distinct || v.limit || v.offset != 0 || v.for_update) {
        not_yet("a join of view \"" + side.name + "\", which is not a plain SELECT of one table", join.pos);
      }
      if (outer && !v.where.empty()) not_yet("an outer join of view \"" + side.name + "\", which has a WHERE", join.pos);
    }
    // A grouped view and a table: the binder reads both as relations, its
    // stars, USING and NATURAL as any join's.
    if (!merged_view(sides[0]) && !merged_view(sides[1])) {
      for (std::size_t i = 0; i < 2; ++i) {
        if (sides[i].derived) derived_item(s.from[i], *sides[i].view);
      }
      return;
    }
    // The side a column reference names or, unqualified, the only one that has it.
    auto side_of = [&](const ast::Expr& ref) -> int {
      if (!ref.table.empty()) return ref.table == sides[0].name ? 0 : ref.table == sides[1].name ? 1 : -1;
      auto hit = -1;
      for (auto i = 0; i < 2; ++i) {
        if (!has_column(sides[i], ref.column)) continue;
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
      qualify(item, side.view->select.from[0].table);
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
      if (!merged_view(side)) continue;
      for (const auto& c : side.view->select.where) {
        s.where.push_back(ast::clone(c));
        qualify(s.where.back(), side.view->select.from[0].table);
      }
    }
    s.from[0].table = sides[0].view != nullptr ? sides[0].view->select.from[0].table : sides[0].name;
    s.from[1].table = sides[1].view != nullptr ? sides[1].view->select.from[0].table : sides[1].name;
  }

  // A FROM item that names a grouped view, read as a derived relation of
  // its query, without its ORDER BY, which orders nothing the query sees.
  static void derived_item(ast::FromItem& item, const ViewBody& v) {
    auto inner = ast::clone(v.select);
    inner.order_by.clear();
    item.column_aliases = v.names;
    item.derived = std::make_shared<const ast::Select>(std::move(inner));
  }

  static auto has_column(const Side& side, const std::string& column) -> bool {
    if (side.view != nullptr) return std::ranges::find(side.view->names, column) != side.view->names.end();
    return side.table->column(column) != nullptr;
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
  return Bound{Ddl{std::move(ddl)}, {}, {}, {}, {}, {}, {}, {}, {}};
}

}  // namespace

namespace {

// Each variable of a procedure's statement as a parameter
// (bind_procedure_statement): an unqualified column reference that names a
// variable becomes $k. `scope` is every table or view the statement names
// but an INSERT's target, whose columns a VALUES list cannot name.
class VariableRewriter {
public:
  VariableRewriter(const Table& frame, const Catalog& cat) : frame_{frame}, cat_{cat} {}

  void statement(ast::Statement& stmt) {
    std::visit(
        [&](auto& st) {
          using T = std::decay_t<decltype(st)>;
          if constexpr (std::is_same_v<T, ast::Select>) {
            names(st);
            select(st);
          } else if constexpr (std::is_same_v<T, ast::Insert>) {
            if (!st.returning.empty()) not_yet("RETURNING in a procedure", st.pos);
            if (st.query) not_supported("INSERT … SELECT in a procedure", st.pos);
            if (st.on_conflict) not_supported("INSERT … ON CONFLICT in a procedure", st.on_conflict->pos);
            for (auto& row : st.rows) {
              for (auto& v : row) names(v);
              for (auto& v : row) expr(v);
            }
          } else if constexpr (std::is_same_v<T, ast::Update>) {
            if (!st.returning.empty()) not_yet("RETURNING in a procedure", st.pos);
            scope_.insert(st.table);
            for (auto& a : st.set) names(a.value);
            for (auto& c : st.where) names(c);
            for (auto& a : st.set) expr(a.value);
            for (auto& c : st.where) compare(c);
            if (!st.current_of.empty()) not_yet("WHERE CURRENT OF in a procedure", st.pos);
          } else if constexpr (std::is_same_v<T, ast::Delete>) {
            if (!st.returning.empty()) not_yet("RETURNING in a procedure", st.pos);
            scope_.insert(st.table);
            for (auto& c : st.where) names(c);
            for (auto& c : st.where) compare(c);
            if (!st.current_of.empty()) not_yet("WHERE CURRENT OF in a procedure", st.pos);
          } else {
            not_yet("this statement in a procedure: only SELECT, INSERT, UPDATE and DELETE run in one", -1);
          }
        },
        stmt);
  }

  [[nodiscard]] auto params() const -> const std::vector<std::uint32_t>& { return vars_; }

private:
  // The tables and views a statement names, in every FROM, subqueries' included.
  void names(const ast::Select& q) {
    for (const auto& f : q.from) scope_.insert(f.table);
    for (const auto& e : q.targets) names(e);
    for (const auto& c : q.where) names(c);
    for (const auto& c : q.having) names(c);
    if (q.set) {
      names(q.set->left);
      names(q.set->right);
    }
  }
  void names(const ast::Expr& e) {
    if (e.subquery) names(*e.subquery);
    if (e.lhs) names(*e.lhs);
    if (e.rhs) names(*e.rhs);
    for (const auto& a : e.args) names(a);
  }
  void names(const ast::Compare& c) {
    names(c.lhs);
    names(c.rhs);
    for (const auto& a : c.args) names(a);
    if (c.subquery) names(*c.subquery);
  }

  auto has_column(const std::string& relation, const std::string& name) const -> bool {
    if (const auto* t = cat_.table(relation)) return t->column(name) != nullptr;
    if (const auto* v = cat_.view(relation)) return std::ranges::find(v->columns, name) != v->columns.end();
    return false;
  }

  void expr(ast::Expr& e) {
    if (e.kind == ast::Expr::Kind::Column && e.table.empty()) {
      if (const auto* var = frame_.column(e.column)) {
        for (const auto& t : scope_) {
          if (has_column(t, e.column)) {
            throw SqlError{"column reference \"" + e.column + "\" is ambiguous: it could refer to either a PL/pgSQL "
                               "variable or a table column",
                           e.pos, kAmbiguousColumn};
          }
        }
        auto it = std::ranges::find(vars_, var->id);
        auto k = static_cast<std::uint32_t>(it - vars_.begin());
        if (it == vars_.end()) vars_.push_back(var->id);
        auto pos = e.pos;
        e = ast::Expr{ast::Expr::Kind::Param, {}, k, nullptr, nullptr, pos};
        return;
      }
    }
    if (e.kind == ast::Expr::Kind::Param) {
      throw SqlError{"there is no parameter $" + std::to_string(e.param + 1), e.pos, kUndefinedParameter};
    }
    if (e.subquery) e.subquery = rewritten(*e.subquery);
    if (e.lhs) expr(*e.lhs);
    if (e.rhs) expr(*e.rhs);
    for (auto& a : e.args) expr(a);
  }
  void compare(ast::Compare& c) {
    expr(c.lhs);
    expr(c.rhs);
    for (auto& a : c.args) compare(a);
    if (c.subquery) c.subquery = rewritten(*c.subquery);
  }
  void select(ast::Select& q) {
    for (auto& e : q.targets) expr(e);
    for (auto& c : q.where) compare(c);
    for (auto& e : q.group_by) expr(e);
    for (auto& c : q.having) compare(c);
    if (q.set) {
      auto set = std::make_shared<ast::SetOperation>();
      set->kind = q.set->kind;
      set->all = q.set->all;
      set->left = ast::clone(q.set->left);
      set->right = ast::clone(q.set->right);
      select(set->left);
      select(set->right);
      q.set = std::move(set);
    }
  }
  // A subquery is shared once parsed: rewritten as a copy.
  auto rewritten(const ast::Select& q) -> std::shared_ptr<const ast::Select> {
    auto copy = std::make_shared<ast::Select>(ast::clone(q));
    select(*copy);
    return copy;
  }

  const Table& frame_;
  const Catalog& cat_;
  std::set<std::string> scope_;
  std::vector<std::uint32_t> vars_;  // $k+1's variable
};

// Each constant of `e` from `first` on, and each variable its slot.
void to_frame(logical::Expr& e, const Table& frame, std::uint32_t first) {
  if (e.kind == logical::Expr::Kind::Column && e.col.table == frame.id) {
    e = logical::param(e.col.col, e.type);
    return;
  }
  if (e.kind == logical::Expr::Kind::Param) e.param += first;
  for (auto& a : e.args) to_frame(a, frame, first);
}

}  // namespace

auto bind_procedure_expr(const std::string& text, const Table& frame, const Catalog& cat, std::uint32_t first_constant,
                         ProcedureConstants& constants, const Column* target) -> logical::Expr {
  Binder b{cat, 0};
  auto e = b.frame_value(frame, text, target);
  if (!b.take_clocks().empty() || !b.take_subqueries().empty()) not_yet("this expression in a procedure", -1);
  auto first = first_constant + bytecask::sql::narrow<std::uint32_t>(constants.values.size());
  to_frame(e, frame, first);
  for (std::size_t i = 0; i < b.fixed_values().size(); ++i) {
    constants.values.push_back(b.fixed_values()[i]);
    constants.types.push_back(b.fixed_sql().at(i));
  }
  return e;
}

auto assignable(SqlType from, SqlType to) -> bool {
  if (phys_type(from) != phys_type(to) || decimal_scale(from) != decimal_scale(to)) return false;
  auto family = [](SqlKind k) {
    if (k == SqlKind::SmallInt || k == SqlKind::Integer || k == SqlKind::BigInt) return 1;
    if (k == SqlKind::Text || k == SqlKind::Varchar || k == SqlKind::Name) return 2;
    return 0;
  };
  return from.kind == to.kind || (family(from.kind) != 0 && family(from.kind) == family(to.kind));
}

auto call_arguments(const ast::Call& call, const Procedure& proc) -> std::vector<Datum> {
  if (call.args.size() != proc.args.size()) {
    throw SqlError{"procedure " + call.procedure + " takes " + std::to_string(proc.args.size()) + " arguments, not " +
                       std::to_string(call.args.size()),
                   call.pos, kUndefinedFunction};
  }
  std::vector<Datum> out;
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    const auto& a = proc.args[i];
    const auto& e = call.args[i];
    if (!is_constant(e)) not_yet("an argument of CALL that is not a constant", e.pos);
    auto k = fold(e);
    if (a.mode == ProcedureArg::Mode::Out) {
      out.emplace_back(std::monostate{});
      continue;
    }
    Column target{static_cast<std::uint32_t>(i), a.name, a.type, phys_type(a.type), true, std::monostate{}};
    auto v = coerce_value(k, target, true, "=");
    if (auto why = out_of_range(a.type, v)) out_of_type(*why, k.pos);
    out.push_back(std::move(v));
  }
  return out;
}

auto declared_type(const std::string& text) -> SqlType {
  auto stmt = ast::parse("SELECT CAST(NULL AS " + text + ")");
  const auto* q = std::get_if<ast::Select>(&stmt);
  if (q == nullptr || q->targets.size() != 1 || q->targets[0].kind != ast::Expr::Kind::Cast) {
    throw SqlError{"type " + text + " is not supported", -1};
  }
  ast::ColumnDefAst c;
  c.type = q->targets[0].cast_type;
  c.typmods = q->targets[0].cast_typmods;
  return sql_type(c);
}

auto bind_procedure_statement(const std::string& sql, const Table& frame, const Catalog& cat) -> ProcedureStatement {
  auto stmt = ast::parse(sql);
  VariableRewriter vars{frame, cat};
  vars.statement(stmt);
  ProcedureStatement out{bound::bind(stmt, cat), vars.params()};
  if (std::ranges::any_of(out.bound.subqueries, [](const logical::Subquery& s) { return s.correlated(); })) {
    not_yet("a correlated subquery in a procedure's statement", -1);
  }
  // A variable is its parameter's value as it is: of the type the statement
  // takes it as, or refused (no conversion is built).
  for (std::size_t k = 0; k < out.param_vars.size(); ++k) {
    const auto* var = frame.column(out.param_vars[k]);
    const auto& as = out.bound.params.at(k).type;
    if (!assignable(var->sql, as)) {
      not_yet("variable " + var->name + " (" + sql_type_name(var->sql) + ") used as " + sql_type_name(as), -1);
    }
  }
  return out;
}

namespace {

// An INSERT's column list: each a column of t, named once (42701).
auto listed_columns(const Table& t, const ast::Insert& s) -> std::vector<const Column*> {
  std::vector<const Column*> out;
  for (const auto& name : s.columns) {
    const auto* c = t.column(name);
    if (c == nullptr || c->hidden) {
      throw SqlError{"column \"" + name + "\" of relation \"" + t.name + "\" does not exist", s.pos, kUndefinedColumn};
    }
    if (std::ranges::contains(out, c)) throw SqlError{"column \"" + name + "\" specified more than once", s.pos, kDuplicateColumn};
    out.push_back(c);
  }
  return out;
}

// ON CONFLICT DO NOTHING's arbiter: with no target any unique constraint,
// else one whose columns are the target's, as a set, as PostgreSQL infers
// it: the primary key (the table's id) or a unique index. The runtime makes
// every unique key over those columns an arbiter. A column the table lacks
// is 42703; no such constraint, 42P10.
auto arbiter(const Table& t, const ast::Insert::OnConflict& oc) -> std::uint32_t {
  if (oc.columns.empty()) return logical::kAnyArbiter;
  std::set<std::uint32_t> target;
  for (const auto& name : oc.columns) target.insert(Binder::column(t, name, oc.pos)->id);
  auto same = [&](const auto& ids) { return std::set<std::uint32_t>(ids.begin(), ids.end()) == target; };
  if (t.row_id() == nullptr && same(t.pk_col_ids)) return t.id;
  for (const auto& ix : t.indexes) {
    if (!ix.unique) continue;
    std::vector<std::uint32_t> cols;
    for (const auto& p : ix.parts) cols.push_back(p.col_id);
    if (same(cols)) return ix.id;
  }
  throw SqlError{"there is no unique or exclusion constraint matching the ON CONFLICT specification", oc.pos,
                 kInvalidColumnReference};
}

// SET col = e of an UPDATE or of ON CONFLICT DO UPDATE, e bound over the
// row as read. A constant is checked against the column's type now, as
// PostgreSQL folds its cast when it plans the statement; a parameter
// before the plan runs (Binder::check_assigned).
auto assigned_value(Binder& b, const Table& t, const ast::Expr& e, const Column& c) -> logical::Expr {
  auto v = b.value(t, e, c);
  if (v.kind == logical::Expr::Kind::Param && b.is_fixed(v.param)) {
    if (auto why = out_of_range(c.sql, b.fixed_value(v.param))) out_of_type(*why, e.pos);
  } else if (v.kind == logical::Expr::Kind::Param) {
    b.check_assigned(v.param, c, t.id);
  }
  return v;
}

// ON CONFLICT DO UPDATE's SET, one list per row of VALUES (`rows`, the
// values bound for each column): each column from an expression over the
// row that holds the key, its columns qualified by the table's name, or
// from EXCLUDED.col, the value the row proposed for col: its parameter, or
// col's default. EXCLUDED.col is assigned to a column of its type, or of
// another integer type, which the row builder checks the value against; a
// type PostgreSQL does not assign from is 42804. An unqualified column is
// ambiguous (42702), as both relations have it. EXCLUDED inside an
// expression is not supported yet.
auto conflict_update(Binder& b, const Table& t, const std::vector<ast::Assignment>& set,
                     const std::vector<std::vector<std::optional<logical::Expr>>>& rows)
    -> std::vector<std::vector<std::pair<logical::ColumnRef, logical::Expr>>> {
  constexpr std::string_view kExcluded = "excluded";
  auto excluded_column = [&](const ast::Expr& e) {
    const auto* c = t.column(e.column);
    if (c == nullptr || c->hidden) throw SqlError{"column excluded." + e.column + " does not exist", e.pos, kUndefinedColumn};
    return c;
  };
  // The references inside a value: none unqualified, no EXCLUDED.
  auto qualified = [&](this const auto& self, const ast::Expr& e) -> void {
    if (e.kind == ast::Expr::Kind::Column && e.table == kExcluded) {
      (void)excluded_column(e);
      not_yet("EXCLUDED in an expression", e.pos);
    }
    if (e.kind == ast::Expr::Kind::Column && e.table.empty()) {
      (void)Binder::column(t, e.column, e.pos);
      throw SqlError{"column reference \"" + e.column + "\" is ambiguous", e.pos, kAmbiguousColumn};
    }
    if (e.lhs) self(*e.lhs);
    if (e.rhs) self(*e.rhs);
    for (const auto& a : e.args) self(a);
  };
  // Each assignment: its column and its value, or the column EXCLUDED gives.
  struct Item {
    logical::ColumnRef target;
    std::optional<logical::Expr> value;
    const Column* excluded{nullptr};
  };
  std::vector<Item> items;
  std::set<std::uint32_t> assigned;
  for (const auto& a : set) {
    const auto* c = t.column(a.column);
    if (c == nullptr || c->hidden) {
      throw SqlError{"column \"" + a.column + "\" of relation \"" + t.name + "\" does not exist", a.pos, kUndefinedColumn};
    }
    if (!assigned.insert(c->id).second) throw SqlError{"multiple assignments to same column \"" + a.column + "\"", a.pos};
    const auto& e = a.value;
    auto target = logical::column_of(t.id, *c).col;
    if (e.kind == ast::Expr::Kind::Column && e.table == kExcluded) {
      const auto* x = excluded_column(e);
      if (x->sql != c->sql && !(is_integer(x->sql) && is_integer(c->sql))) {
        if (auto state = type_refusal(x->sql.kind, c->sql.kind, true)) {
          throw SqlError{"column \"" + c->name + "\" is of type " + pg_type_name(c->sql) + " but expression is of type " +
                             pg_type_name(x->sql),
                         e.pos, *state}
              .with_notes("", "You will need to rewrite or cast the expression.");
        }
        not_yet("EXCLUDED." + x->name + " assigned to a column of another type", e.pos);
      }
      items.push_back({target, std::nullopt, x});
      continue;
    }
    qualified(e);
    items.push_back({target, assigned_value(b, t, e, *c), nullptr});
  }
  // A column the rows leave out proposes its default, bound once.
  std::map<std::size_t, logical::Expr> defaults;
  std::vector<std::vector<std::pair<logical::ColumnRef, logical::Expr>>> out;
  for (const auto& row : rows) {
    auto& list = out.emplace_back();
    for (const auto& it : items) {
      if (it.value) {
        list.emplace_back(it.target, *it.value);
        continue;
      }
      auto at = static_cast<std::size_t>(it.excluded - t.columns.data());
      if (row[at]) {
        list.emplace_back(it.target, *row[at]);
        continue;
      }
      auto d = defaults.find(at);
      if (d == defaults.end()) d = defaults.emplace(at, b.fixed(it.excluded->default_value, it.excluded->sql)).first;
      list.emplace_back(it.target, d->second);
    }
  }
  return out;
}

// INSERT … SELECT: the query bound with its select list as the values of
// the columns written (Binder::assign), a SELECT of rows: refused (0A000)
// where it groups, aggregates, is DISTINCT, or is a set operation, whose
// rows the plan gives through no Emit (#682). A column left out takes its
// default, a DEFAULT of the clock the statement's reading.
auto insert_query(Binder& b, const Table& t, const ast::Insert& s) -> logical::NodePtr {
  std::vector<const Column*> columns;
  if (s.columns.empty()) {
    // The hidden row id is no column a statement fills.
    for (const auto& c : t.columns) {
      if (!c.hidden) columns.push_back(&c);
    }
  } else {
    columns = listed_columns(t, s);
  }
  b.assign({&t, std::move(columns), !s.columns.empty()});
  auto rows = b.select(*s.query);
  const auto* n = rows.get();
  while (std::holds_alternative<logical::Limit>(n->op) || std::holds_alternative<logical::Sort>(n->op)) {
    n = logical::input_of(*n)->get();
  }
  if (!std::holds_alternative<logical::Project>(n->op)) {
    not_supported("INSERT … SELECT of an aggregated, grouped or DISTINCT query", s.query->pos);
  }
  if (s.on_conflict && s.on_conflict->update) not_yet("INSERT … SELECT … ON CONFLICT DO UPDATE", s.on_conflict->pos);
  logical::InsertFrom out{std::move(rows), &t, {}, {}, b.checks(t), b.returning(t, s.returning), {}};
  if (s.on_conflict) out.arbiter = arbiter(t, *s.on_conflict);
  const auto filled = b.take_assigned();
  for (const auto* c : filled) out.columns.push_back(logical::column_of(t.id, *c).col);
  for (const auto& c : t.columns) {
    if (c.clock_default && !std::ranges::contains(filled, &c)) {
      out.fixed.emplace_back(logical::column_of(t.id, c).col, b.clock_reading(*c.clock_default, c));
    }
  }
  return logical::node(std::move(out));
}

}  // namespace

auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound { return bind(stmt, cat, {}); }

auto bind(const ast::Statement& stmt, const Catalog& cat, std::span<const SqlType> fixed_params) -> Bound {
  if (const auto* cv = std::get_if<ast::CreateView>(&stmt)) return bind_create_view(*cv, cat);
  ViewExpander views{cat};
  auto expanded = views.statement(stmt);
  const auto& target = expanded ? *expanded : stmt;
  // WHERE CURRENT OF takes the table's key as parameters after the statement's own.
  auto own = param_count(target);
  std::uint32_t key_params = 0;
  std::uint32_t positioned_table = 0;
  auto positioned = [&](const std::string& table, const std::string& cursor, std::int32_t pos) {
    if (cursor.empty()) return;
    const auto* t = cat.table(table);
    if (t == nullptr) {
      if (cat.view(table) != nullptr) {
        throw SqlError{"\"" + table + "\" is a view: UPDATE and DELETE through a view are not supported yet (#678)", pos, kFeatureNotSupported};
      }
      throw SqlError{"relation \"" + table + "\" does not exist", pos, kUndefinedTable};
    }
    key_params = bytecask::sql::narrow<std::uint32_t>(t->pk_col_ids.size());
    positioned_table = t->storage_id();
  };
  if (const auto* u = std::get_if<ast::Update>(&target)) positioned(u->table, u->current_of, u->pos);
  if (const auto* d = std::get_if<ast::Delete>(&target)) positioned(d->table, d->current_of, d->pos);
  Binder b{cat, own + key_params};
  b.fix_params(fixed_params);
  if (!views.used().empty()) b.forbid_row_key();
  auto bound_stmt = std::visit(
      [&](const auto& s) -> std::pair<std::variant<Ddl, logical::NodePtr>, std::int32_t> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ast::CreateTable>) {
          // IF NOT EXISTS and the name taken: PostgreSQL skips the statement
          // before it reads the definition, so a wrong one is skipped too.
          if (s.if_not_exists && (cat.table(s.table) != nullptr || cat.view(s.table) != nullptr ||
                                  cat.index(s.table).first != nullptr)) {
            return {Ddl{CreateTableDdl{s.table, {}, {}, {}, {}, true}}, s.pos};
          }
          if (s.refusal) throw *s.refusal;
          CreateTableDdl out{s.table, {}, s.pk, {}, {}, s.if_not_exists};
          for (const auto& c : s.columns) {
            out.columns.push_back(column_spec(c));
            if (c.primary_key) out.pk.push_back(c.name);
          }
          for (const auto& u : s.unique) out.unique.push_back({u.name, u.columns});
          if (!s.checks.empty()) {
            // Bound over the table as it will be, to refuse what a write could not
            // evaluate; the text is what the catalog keeps.
            Table scratch;
            scratch.name = s.table;
            for (const auto& c : out.columns) {
              scratch.columns.push_back({scratch.next_col_id++, c.name, c.sql, phys_type(c.sql), c.nullable, c.default_value});
            }
            Binder checker{cat, 0};
            for (const auto& c : s.checks) {
              (void)checker.check(scratch, c.text, c.pos);
              out.checks.push_back({c.name, c.text, c.column});
            }
          }
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
        } else if constexpr (std::is_same_v<T, ast::CreateProcedure>) {
          CreateProcedureDdl out{s.procedure, s.definition, s.replace, {}};
          for (const auto& a : s.args) {
            auto mode = a.mode == ast::ProcedureArgAst::Mode::In      ? ProcedureArg::Mode::In
                        : a.mode == ast::ProcedureArgAst::Mode::InOut ? ProcedureArg::Mode::InOut
                                                                      : ProcedureArg::Mode::Out;
            out.args.push_back({mode, a.def.name, sql_type(a.def)});
          }
          return {Ddl{std::move(out)}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateRole>) {
          return {Ddl{CreateRoleDdl{s.role}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::DropRole>) {
          return {Ddl{DropRoleDdl{s.roles, s.if_exists}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Grant>) {
          GrantDdl out{s.grant, s.grant_option, {}, s.objects, s.grantees, s.grantor, GrantDdl::Target::Table};
          switch (s.target) {
            case ast::Grant::Target::Table: out.target = GrantDdl::Target::Table; break;
            case ast::Grant::Target::Sequence: out.target = GrantDdl::Target::Sequence; break;
            case ast::Grant::Target::Domain: out.target = GrantDdl::Target::Domain; break;
            case ast::Grant::Target::Type: out.target = GrantDdl::Target::Type; break;
          }
          for (const auto& p : s.privileges) out.privileges.push_back({p.name, p.columns});
          return {Ddl{std::move(out)}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateDomain>) {
          return {Ddl{CreateDomainDdl{s.domain, sql_type(s.base)}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateType>) {
          return {Ddl{CreateTypeDdl{s.type}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateSequence>) {
          return {Ddl{CreateSequenceDdl{s.sequence, s.if_not_exists}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::DropTypeOrSequence>) {
          using K = ast::DropTypeOrSequence::Kind;
          if (s.kind == K::Sequence) return {Ddl{DropSequenceDdl{s.name, s.if_exists}}, s.pos};
          return {Ddl{DropTypeDdl{s.name, s.kind == K::Domain, s.if_exists}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::DropProcedure>) {
          DropProcedureDdl out{s.procedure, std::nullopt, s.if_exists};
          if (s.arg_types) {
            out.arg_types.emplace();
            // Only the kind is compared, so a NUMERIC needs no precision here.
            for (const auto& t : *s.arg_types) {
              out.arg_types->push_back(t.type == "numeric" && t.typmods.empty() ? SqlType{SqlKind::Decimal} : sql_type(t));
            }
          }
          return {Ddl{std::move(out)}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::AddColumn>) {
          (void)b.table(s.table, s.pos);
          if (s.column.primary_key) throw SqlError{"ADD COLUMN cannot add a primary key column", s.column.pos};
          if (!s.column.unique.empty()) not_yet("ADD COLUMN … UNIQUE", s.column.pos);
          if (!s.column.references.empty()) not_yet("ADD COLUMN … REFERENCES", s.column.pos);
          if (!s.column.checks.empty()) not_yet("ADD COLUMN … CHECK", s.column.pos);
          // Plans compiled before it fill a new column with its stored default.
          if (is_session_name(s.column.default_clock)) not_supported("ADD COLUMN … DEFAULT of a session function", s.column.pos);
          if (!s.column.default_clock.empty()) not_yet("ADD COLUMN … DEFAULT of the clock", s.column.pos);
          auto spec = column_spec(s.column);
          if (!spec.nullable && std::holds_alternative<std::monostate>(spec.default_value)) {
            throw SqlError{"column \"" + s.column.name + "\" is NOT NULL and needs a DEFAULT for the existing rows",
                           s.column.pos};
          }
          return {Ddl{AddColumnDdl{s.table, std::move(spec)}}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateTableAs>) {
          // The API binds the query alone and creates the table from its rows.
          throw std::logic_error("CREATE TABLE AS is bound through its query");
        } else if constexpr (std::is_same_v<T, ast::Insert>) {
          const auto* t = b.table(s.table, s.pos);
          if (s.query) return {insert_query(b, *t, s), s.pos};
          logical::Insert out{t, {}, {}};
          const auto width = s.rows.front().size();  // every row's: the parser checked
          std::vector<const Column*> targets;
          if (s.columns.empty()) {
            // As PostgreSQL: the values fill the leading columns and the rest
            // take their defaults, so a column added later changes nothing.
            // The hidden row id is no column a statement fills.
            for (const auto& c : t->columns) {
              if (targets.size() == width) break;
              if (!c.hidden) targets.push_back(&c);
            }
            if (width > targets.size()) {
              throw SqlError{"INSERT has more expressions than target columns", s.pos};
            }
          } else {
            targets = listed_columns(*t, s);
          }
          if (targets.size() != width) {
            throw SqlError{"INSERT has " + std::to_string(width) + " values for " + std::to_string(targets.size()) + " columns",
                           s.pos};
          }
          // Each row is written as one INSERT of it would be, in order. A
          // column left out with a clock default takes the statement's reading.
          for (const auto& values : s.rows) {
            auto& row = out.rows.emplace_back(t->columns.size());
            for (std::size_t i = 0; i < targets.size(); ++i) {
              auto at = static_cast<std::size_t>(targets[i] - t->columns.data());
              row[at] = b.value(*t, values[i], *targets[i]);
              if (row[at]->kind != logical::Expr::Kind::Param) {
                throw SqlError{"INSERT values must be parameters or constants", values[i].pos};
              }
            }
            for (std::size_t at = 0; at < t->columns.size(); ++at) {
              const auto& c = t->columns[at];
              if (!row[at] && c.clock_default) row[at] = b.clock_reading(*c.clock_default, c);
            }
          }
          // A column left out takes its default. One that is NOT NULL with no
          // default, a primary key column among them, is refused when the
          // row is built (BC_C_NOT_NULL), as a NULL given for it is.
          out.checks = b.checks(*t);
          out.returning = b.returning(*t, s.returning);
          if (s.on_conflict) out.arbiter = arbiter(*t, *s.on_conflict);
          if (s.on_conflict && s.on_conflict->update) out.update = conflict_update(b, *t, *s.on_conflict->update, out.rows);
          return {logical::node(std::move(out)), s.pos};
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          return {b.select(s), s.pos};
        } else if constexpr (std::is_same_v<T, ast::Update>) {
          const auto* t = b.updated_table(s.table, s.pos);
          std::vector<std::pair<logical::ColumnRef, logical::Expr>> set;
          for (const auto& a : s.set) {
            // A primary key column too: the row moves to its new key, each
            // row's checked in turn, as PostgreSQL's unique check does.
            const auto* c = Binder::column(*t, a.column, a.pos);
            set.emplace_back(logical::column_of(t->id, *c).col, assigned_value(b, *t, a.value, *c));
          }
          b.set_scope({t});
          auto rows = Binder::rows(*t, s.current_of.empty() ? b.where(*t, s.where) : b.current_of(*t, own, s.pos));
          return {logical::node(logical::Update{std::move(rows), t, std::move(set), b.checks(*t), b.returning(*t, s.returning)}),
                  s.pos};
        } else {
          const auto* t = b.updated_table(s.table, s.pos);
          b.set_scope({t});
          auto rows = Binder::rows(*t, s.current_of.empty() ? b.where(*t, s.where) : b.current_of(*t, own, s.pos));
          return {logical::node(logical::Delete{std::move(rows), t, b.returning(*t, s.returning)}), s.pos};
        }
      },
      target);
  auto types = b.param_types(bound_stmt.second);
  types.insert(types.end(), b.fixed_types().begin(), b.fixed_types().end());
  Bound out{std::move(bound_stmt.first), std::move(types), b.param_sql(), b.param_table(), b.param_info(),
            b.fixed_values(), b.take_clocks(), b.take_subqueries(), views.used(), b.take_instances()};
  out.key_columns = b.key_columns();
  out.key_table = key_params != 0 ? positioned_table : b.key_table();
  out.key_params = key_params;
  return out;
}

namespace {

auto has_pos(const ast::Expr& e, std::int32_t pos) -> bool {
  return e.pos == pos || (e.lhs && has_pos(*e.lhs, pos)) || (e.rhs && has_pos(*e.rhs, pos)) ||
         std::ranges::any_of(e.args, [&](const ast::Expr& a) { return has_pos(a, pos); });
}

// The largest constant expression in e holding the node at `pos`, made NULL.
auto null_constant_at(ast::Expr& e, std::int32_t pos) -> bool {
  if (is_constant(e)) {
    if (!has_pos(e, pos)) return false;
    ast::Expr null{ast::Expr::Kind::Literal, {}, 0, nullptr, nullptr, e.pos};
    null.alias = std::move(e.alias);
    e = std::move(null);
    return true;
  }
  return (e.lhs && null_constant_at(*e.lhs, pos)) || (e.rhs && null_constant_at(*e.rhs, pos)) ||
         std::ranges::any_of(e.args, [&](ast::Expr& a) { return null_constant_at(a, pos); });
}

auto null_constant_at(ast::Compare& c, std::int32_t pos) -> bool {
  return null_constant_at(c.lhs, pos) || null_constant_at(c.rhs, pos) ||
         std::ranges::any_of(c.args, [&](ast::Compare& a) { return null_constant_at(a, pos); });
}

}  // namespace

auto null_failing_constant(ast::Select& s, std::int32_t pos) -> bool {
  if (pos < 0 || s.set) return false;
  auto in = [&](auto& list) { return std::ranges::any_of(list, [&](auto& x) { return null_constant_at(x, pos); }); };
  return in(s.targets) || in(s.group_by) || in(s.where) || in(s.having);
}

}  // namespace bytecask::sql::bound
