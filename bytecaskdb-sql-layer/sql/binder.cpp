// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
// prototype: see binder.hpp.

#include "bytecaskdb-sql-layer/sql/binder.hpp"

#include <algorithm>
#include <string>

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

// SQL type names as libpg_query canonicalises them.
auto sql_type(const ast::ColumnDefAst& c) -> PhysType {
  if (c.type == "int8") return PhysType::I64;
  if (c.type == "bytea") return PhysType::Bytes;
  throw SqlError{"type " + c.type + " is not supported (only BIGINT and BYTEA)", c.pos};
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

private:
  const Catalog& cat_;
  std::vector<std::optional<PhysType>> types_;
};

}  // namespace

auto bind(const ast::Statement& stmt, const Catalog& cat) -> Bound {
  Binder b{cat};
  auto bound_stmt = std::visit(
      [&](const auto& s) -> std::pair<Statement, std::int32_t> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ast::CreateTable>) {
          CreateTable out{s.table, {}, s.pk};
          for (const auto& c : s.columns) {
            out.columns.push_back({c.name, sql_type(c), !c.not_null && !c.primary_key});
            if (c.primary_key) out.pk.push_back(c.name);
          }
          if (out.pk.empty()) throw SqlError{"a table needs a PRIMARY KEY", s.pos};
          return {out, s.pos};
        } else if constexpr (std::is_same_v<T, ast::CreateIndex>) {
          const auto* t = b.table(s.table, s.pos);
          for (const auto& c : s.columns) (void)Binder::column(*t, c, s.pos);
          return {CreateIndex{s.index, s.table, s.columns}, s.pos};
        } else if constexpr (std::is_same_v<T, ast::Insert>) {
          const auto* t = b.table(s.table, s.pos);
          Insert out{t, std::vector<std::optional<Expr>>(t->columns.size())};
          std::vector<const Column*> targets;
          if (s.columns.empty()) {
            for (const auto& c : t->columns) targets.push_back(&c);
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
          for (std::size_t i = 0; i < t->columns.size(); ++i) {
            if (!out.values[i] && !t->columns[i].nullable) {
              throw SqlError{"column \"" + t->columns[i].name + "\" is NOT NULL and has no value", s.pos};
            }
          }
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
  return Bound{std::move(bound_stmt.first), b.param_types(bound_stmt.second)};
}

}  // namespace bytecask::sql::bound
