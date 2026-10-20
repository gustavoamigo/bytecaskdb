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

// A column, a parameter, or column - parameter (the only arithmetic in scope).
struct Expr {
  enum class Kind { Column, Param, Sub };
  Kind kind;
  std::uint32_t col_id{0};  // Column, and the left of Sub
  std::uint32_t param{0};   // Param, and the right of Sub
  PhysType type;
};

// column <op> parameter, normalised so the column is on the left.
struct Compare {
  ast::Compare::Op op;
  std::uint32_t col_id;
  std::uint32_t param;
};

struct Insert {
  const Table* table;
  // One per table column, in column order; nullopt: the column is not given
  // and takes its default, which bc_row_begin writes.
  std::vector<std::optional<Expr>> values;
};

struct Select {
  const Table* table;
  std::vector<std::uint32_t> targets;  // column ids, in select-list order
  std::vector<Compare> where;
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

// DDL binds to the catalog's statement values: names checked, types and
// defaults folded.
using Statement = std::variant<Ddl, Insert, Select, Update, Delete>;

struct Bound {
  Statement stmt;
  std::vector<PhysType> param_types;  // $1 is [0]
  // For each parameter compared with a column (WHERE), that column's type
  // without its modifier (param_type), as PostgreSQL types a $n: what its
  // value is checked against before the plan runs, so a key or bound
  // outside it is refused whatever the plan would read. nullopt for one
  // only assigned (INSERT, SET), which the row builder checks in column
  // order, as PostgreSQL's assignment does.
  std::vector<std::optional<SqlType>> param_sql;
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

// A decimal literal's sign, whole digits and fraction digits:
// [-]digits[.digits]; nullopt if it is not one.
// Views into the literal's text, which decimal_literal does not copy: valid
// only while that text lives. scaled_decimal consumes it at once.
struct DecimalLiteral {
  bool neg;
  std::string_view whole, frac;
};
auto decimal_literal(std::string_view text) -> std::optional<DecimalLiteral> {
  auto neg = !text.empty() && text[0] == '-';
  if (neg) text.remove_prefix(1);
  auto dot = text.find('.');
  auto whole = text.substr(0, dot);
  auto frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  auto digits = [](std::string_view d) { return std::ranges::all_of(d, [](char ch) { return ch >= '0' && ch <= '9'; }); };
  if ((whole.empty() && frac.empty()) || !digits(whole) || !digits(frac)) return std::nullopt;
  return DecimalLiteral{neg, whole, frac};
}

// The literal scaled by 10^scale, rounded half away from zero; nullopt if it
// has more digits than any DECIMAL holds, so it cannot be scaled in an i64.
// The column's precision is checked by out_of_range, not here.
auto scaled_decimal(DecimalLiteral d, std::uint8_t scale) -> std::optional<std::int64_t> {
  auto [neg, whole, frac] = d;
  // Leading zeros are no digits: "0.5" has none before the point, so it fits
  // NUMERIC(4, 4).
  while (!whole.empty() && whole[0] == '0') whole.remove_prefix(1);
  if (whole.size() + scale > kMaxDecimalPrecision) return std::nullopt;
  std::int64_t v = 0;
  for (auto ch : whole) v = v * 10 + (ch - '0');
  for (std::uint8_t i = 0; i < scale; ++i) v = v * 10 + (i < frac.size() ? frac[i] - '0' : 0);
  if (frac.size() > scale && frac[scale] >= '5') ++v;
  return neg ? -v : v;
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
      auto d = decimal_literal(l.text);
      if (!d) bad();
      auto v = scaled_decimal(*d, t.scale);
      if (!v || out_of_range(t, *v)) throw SqlError{"DEFAULT out of range for type " + sql_type_name(t), l.pos};
      return *v;
    }
    case SqlKind::Real:
    case SqlKind::Double: {
      double v = 0;
      if (l.kind != K::Integer && l.kind != K::Number) bad();
      auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
      if (ec != std::errc{} || end != l.text.data() + l.text.size()) bad();
      if (out_of_range(t, v)) throw SqlError{"DEFAULT out of range for type " + sql_type_name(t), l.pos};
      // The catalog holds what a row stores: a REAL default as its float.
      return t.kind == SqlKind::Real ? float_rounding(v) : v;
    }
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
    if (t == nullptr) throw SqlError{"relation \"" + name + "\" does not exist", pos};
    return t;
  }

  static auto column(const Table& t, const std::string& name, std::int32_t pos) -> const Column* {
    const auto* c = t.column(name);
    if (c == nullptr) throw SqlError{"column \"" + name + "\" does not exist", pos};
    return c;
  }

  // Gives parameter $n+1 a type; a second use must agree with the first.
  void use_param(std::uint32_t n, PhysType t, std::int32_t pos) {
    if (types_.size() <= n) types_.resize(n + 1);
    if (types_[n] && *types_[n] != t) {
      throw SqlError{"parameter $" + std::to_string(n + 1) + " used as both " + type_name(*types_[n]) + " and " +
                         type_name(t),
                     pos};
    }
    types_[n] = t;
  }

  // An expression whose type is fixed by its target (an assigned column).
  auto value(const Table& t, const ast::Expr& e, PhysType want) -> Expr {
    switch (e.kind) {
      case ast::Expr::Kind::Param:
        use_param(e.param, want, e.pos);
        return Expr{Expr::Kind::Param, 0, e.param, want};
      case ast::Expr::Kind::Column: {
        const auto* c = column(t, e.column, e.pos);
        if (c->type != want) throw SqlError{"column \"" + c->name + "\" is " + type_name(c->type), e.pos};
        return Expr{Expr::Kind::Column, c->id, 0, want};
      }
      case ast::Expr::Kind::Sub: {
        if (e.lhs->kind != ast::Expr::Kind::Column || e.rhs->kind != ast::Expr::Kind::Param) {
          throw SqlError{"only column - parameter is supported", e.pos};
        }
        const auto* c = column(t, e.lhs->column, e.lhs->pos);
        if (c->type != PhysType::I64 || want != PhysType::I64) {
          throw SqlError{"operator - is only defined on bigint", e.pos};
        }
        use_param(e.rhs->param, PhysType::I64, e.rhs->pos);
        return Expr{Expr::Kind::Sub, c->id, e.rhs->param, PhysType::I64};
      }
    }
    throw SqlError{"expression not supported", e.pos};
  }

  auto where(const Table& t, const std::vector<ast::Compare>& in) -> std::vector<Compare> {
    std::vector<Compare> out;
    for (const auto& w : in) {
      const auto* col = &w.lhs;
      const auto* param = &w.rhs;
      auto op = w.op;
      if (col->kind == ast::Expr::Kind::Param && param->kind == ast::Expr::Kind::Column) {
        std::swap(col, param);  // $1 < x is x > $1
        switch (op) {
          case ast::Compare::Op::Lt: op = ast::Compare::Op::Gt; break;
          case ast::Compare::Op::Le: op = ast::Compare::Op::Ge; break;
          case ast::Compare::Op::Gt: op = ast::Compare::Op::Lt; break;
          case ast::Compare::Op::Ge: op = ast::Compare::Op::Le; break;
          case ast::Compare::Op::Eq: break;
        }
      }
      if (col->kind != ast::Expr::Kind::Column || param->kind != ast::Expr::Kind::Param) {
        throw SqlError{"WHERE must compare a column with a parameter", w.pos};
      }
      const auto* c = column(t, col->column, col->pos);
      use_param(param->param, c->type, param->pos);
      if (sql_.size() <= param->param) sql_.resize(param->param + 1);
      if (!sql_[param->param]) sql_[param->param] = param_type(c->sql);  // the first comparison types it
      out.push_back(Compare{op, c->id, param->param});
    }
    return out;
  }

  // Every parameter from $1 to the highest used must have a type.
  auto param_types(std::int32_t pos) const -> std::vector<PhysType> {
    std::vector<PhysType> out;
    for (std::size_t i = 0; i < types_.size(); ++i) {
      if (!types_[i]) throw SqlError{"could not determine data type of parameter $" + std::to_string(i + 1), pos};
      out.push_back(*types_[i]);
    }
    return out;
  }

  auto param_sql() const -> std::vector<std::optional<SqlType>> {
    auto out = sql_;
    out.resize(types_.size());
    return out;
  }

private:
  const Catalog& cat_;
  std::vector<std::optional<PhysType>> types_;
  std::vector<std::optional<SqlType>> sql_;
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
            if (out.values[at]) throw SqlError{"column \"" + targets[i]->name + "\" specified more than once", s.pos};
            out.values[at] = b.value(*t, s.values[i], targets[i]->type);
            if (out.values[at]->kind != Expr::Kind::Param) {
              throw SqlError{"INSERT values must be parameters", s.values[i].pos};
            }
          }
          // A column left out takes its default. One that is NOT NULL with no
          // default, a primary key column among them, is refused when the
          // row is built (BC_C_NOT_NULL), as a NULL given for it is.
          return {out, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Select>) {
          const auto* t = b.table(s.table, s.pos);
          Select out{t, {}, b.where(*t, s.where)};
          for (const auto& e : s.targets) {
            if (e.kind != ast::Expr::Kind::Column) throw SqlError{"only columns can be selected", e.pos};
            out.targets.push_back(Binder::column(*t, e.column, e.pos)->id);
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
            out.set.emplace_back(c->id, b.value(*t, a.value, c->type));
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
  return Bound{std::move(bound_stmt.first), std::move(types), b.param_sql()};
}

}  // namespace bytecask::sql::bound
