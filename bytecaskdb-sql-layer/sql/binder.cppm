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
  // the plan's sum and count, docs/planner_design.md, *GROUP BY as built*).
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
      case Fn::Avg: agg.type = SqlType{SqlKind::Decimal, 0, 0, c->sql.scale}; return {std::move(agg), "avg"};
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
      case ast::Expr::Kind::Const: throw SqlError{"an integer literal is supported only after + or - yet", e.pos};
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
  // dependency, so it is grouped by too, which makes the same groups.
  auto select(const ast::Select& s) -> logical::NodePtr {
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
    std::vector<logical::Expr> group_by;
    for (const auto& e : s.group_by) {
      auto [t, c] = resolve(e);
      auto g = logical::column_of(t->id, *c);
      if (std::ranges::find(group_by, g) == group_by.end()) group_by.push_back(std::move(g));
    }
    // A column of an aggregated query: grouped, or grouped by now as its
    // table's primary key determines it; else 42803.
    auto grouped = [&](const Table& t, const Column& c, std::int32_t pos) -> logical::Expr {
      auto g = logical::column_of(t.id, c);
      if (std::ranges::find(group_by, g) != group_by.end()) return g;
      auto by_key = !s.group_by.empty() && std::ranges::all_of(t.pk_col_ids, [&](std::uint32_t id) {
        return std::ranges::find(group_by, logical::column_of(t.id, *t.column(id))) != group_by.end();
      });
      if (!by_key) ungrouped(t, c.name, pos);
      group_by.push_back(g);
      return g;
    };
    std::vector<logical::Output> targets;
    for (const auto& e : s.targets) {
      if (e.kind == ast::Expr::Kind::Aggregate) {
        if (e.column.empty()) {
          targets.push_back(aggregate(nullptr, 0, e));
        } else {
          auto [t, c] = resolve(e);
          targets.push_back(aggregate(c, t->id, e));
        }
        continue;
      }
      if (e.kind != ast::Expr::Kind::Column) throw SqlError{"only columns and aggregates can be selected", e.pos};
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
      rows = logical::node(logical::Aggregate{std::move(rows), std::move(targets), std::move(group_by), !s.group_by.empty()});
      if (!having.empty()) rows = logical::node(logical::Filter{std::move(rows), std::move(having)});
      if (!keys.empty()) rows = logical::node(logical::Sort{std::move(rows), std::move(keys)});
    } else {
      if (!keys.empty()) rows = logical::node(logical::Sort{std::move(rows), std::move(keys)});
      rows = logical::node(logical::Project{std::move(rows), std::move(targets)});
    }
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
    if (lhs->kind == ast::Expr::Kind::Param) {
      std::swap(lhs, rhs);
      op = mirrored(op);
    }
    if ((lhs->kind != ast::Expr::Kind::Column && lhs->kind != ast::Expr::Kind::Aggregate) ||
        rhs->kind != ast::Expr::Kind::Param) {
      throw SqlError{"HAVING must compare a grouped column or an aggregate with a parameter", w.pos};
    }
    if (lhs->kind == ast::Expr::Kind::Column) {
      auto [t, c] = resolve(*lhs);
      auto g = grouped(*t, *c, lhs->pos);
      use_param(rhs->param, c->type, rhs->pos, *c, t->id, Use::Compared);
      return logical::compare(op, std::move(g), logical::param(rhs->param, c->sql));
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
    use_param(rhs->param, PhysType::I64, rhs->pos, *as, table_id, Use::Compared);
    return logical::compare(op, std::move(agg.expr), logical::param(rhs->param, as->sql));
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
  std::vector<ParamUse> params_;
  // What a HAVING parameter compared with COUNT or SUM is typed by: no
  // column of a table, kept where a parameter's first use points.
  std::deque<Column> synthetic_;
};

}  // namespace

auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound {
  Binder b{cat};
  auto bound_stmt = std::visit(
      [&](const auto& s) -> std::pair<std::variant<Ddl, logical::NodePtr>, std::int32_t> {
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
          logical::Insert out{t, std::vector<std::optional<logical::Expr>>(t->columns.size())};
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
            if (out.values[at]->kind != logical::Expr::Kind::Param) {
              throw SqlError{"INSERT values must be parameters", s.values[i].pos};
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
            if (v.kind == logical::Expr::Kind::Param) b.check_assigned(v.param, *c, t->id);
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
  return Bound{std::move(bound_stmt.first), std::move(types), b.param_sql(), b.param_table(), b.param_info()};
}

}  // namespace bytecask::sql::bound
