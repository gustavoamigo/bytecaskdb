// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// parser.cppm — SQL text to the prototype's AST: libpg_query parses, its
// protobuf output is unpacked with protobuf-c, and the tree is transformed
// into bytecask.sql.ast's types. Anything outside the seven forms is rejected
// with a message and the position libpg_query recorded for the offending
// node. The module exports parse() and nothing of libpg_query's: its headers
// are in this unit's global module fragment, which no importer sees.
//
// prototype: replaced by the designed front end (#492).

module;
#include <charconv>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <pg_query.h>
#include <protobuf/pg_query.pb-c.h>
#pragma clang diagnostic pop

export module bytecask.sql.parser;

export import bytecask.sql.ast;

export namespace bytecask::sql::ast {

// Parses exactly one statement. Throws SqlError for a syntax error (with
// PostgreSQL's message and position) or a form the prototype does not take.
[[nodiscard]] auto parse(std::string_view sql) -> Statement;

}  // namespace bytecask::sql::ast

namespace bytecask::sql::ast {

namespace {

[[noreturn]] void reject(const std::string& what, std::int32_t pos, SqlState state = kSyntaxError) {
  throw SqlError{what, pos, state};
}

// protobuf-c gives an absent string field as "", never as NULL.
auto empty(const char* s) -> bool { return s == nullptr || s[0] == '\0'; }

auto str(const PgQuery__Node* n) -> std::string {
  if (n == nullptr || n->node_case != PG_QUERY__NODE__NODE_STRING) reject("expected a name", -1);
  return n->string->sval;
}

// A FROM item's table; its alias, if any, is read by select().
auto from_relation(const PgQuery__RangeVar* rv) -> std::string {
  if (rv == nullptr) reject("expected a table", -1);
  if (!empty(rv->schemaname)) {
    reject("schema-qualified names are not supported", rv->location);
  }
  return rv->relname;
}

auto relation(const PgQuery__RangeVar* rv) -> std::string {
  auto name = from_relation(rv);
  if (rv->alias != nullptr) reject("table aliases are not supported", rv->location);
  return name;
}

// A FROM item's alias: "" for none. A column list (t AS x (a, b)) is refused.
auto table_alias(const PgQuery__RangeVar* rv) -> std::string {
  if (rv->alias == nullptr) return "";
  if (rv->alias->n_colnames != 0) reject("column aliases of a table are not supported", rv->location);
  return rv->alias->aliasname;
}

auto expr(const PgQuery__Node* n) -> Expr;
auto literal(const PgQuery__Node* n) -> Literal;
auto select(const PgQuery__SelectStmt* s, std::int32_t pos) -> Select;

// A subquery's SELECT, parsed as any SELECT is.
auto subquery(const PgQuery__SubLink* l) -> std::shared_ptr<const Select> {
  if (l->subselect == nullptr || l->subselect->node_case != PG_QUERY__NODE__NODE_SELECT_STMT) {
    reject("a subquery must be a SELECT", l->location);
  }
  return std::make_shared<const Select>(select(l->subselect->select_stmt, l->location));
}

auto value_compare(const std::string& op, std::int32_t pos) -> ValueCompare {
  if (op == "=") return ValueCompare::Eq;
  if (op == "<>") return ValueCompare::Ne;
  if (op == "<") return ValueCompare::Lt;
  if (op == "<=") return ValueCompare::Le;
  if (op == ">") return ValueCompare::Gt;
  if (op == ">=") return ValueCompare::Ge;
  reject("operator " + op + " is not supported", pos);
}

auto literal_expr(Literal l) -> Expr {
  Expr e{Expr::Kind::Literal, {}, 0, nullptr, nullptr, l.pos};
  e.literal = std::move(l);
  return e;
}

// An operand of an arithmetic operator: an expression, an integer literal within BIGINT,
// as a Const, or any other literal (PostgreSQL types a larger integer as
// numeric), for the binder to fold or refuse.
auto operand(const PgQuery__Node* n) -> Expr {
  if (n == nullptr || n->node_case != PG_QUERY__NODE__NODE_A_CONST) return expr(n);
  auto l = literal(n);
  std::int64_t v = 0;
  auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
  if (l.kind != Literal::Kind::Integer || ec != std::errc{} || end != l.text.data() + l.text.size()) {
    return literal_expr(std::move(l));
  }
  Expr e{Expr::Kind::Const, {}, 0, nullptr, nullptr, l.pos};
  e.constant = v;
  return e;
}

auto expr(const PgQuery__Node* n) -> Expr {
  if (n == nullptr) reject("expected an expression", -1);
  // if-chains, not switches: libpg_query's node enum has 268 values.
  if (n->node_case == PG_QUERY__NODE__NODE_COLUMN_REF) {
    const auto* c = n->column_ref;
    if (c->n_fields != 1 && c->n_fields != 2) reject("column names take at most a table's name", c->location);
    const auto* last = c->fields[c->n_fields - 1];
    if (last->node_case == PG_QUERY__NODE__NODE_A_STAR) {
      Expr star{Expr::Kind::Star, {}, 0, nullptr, nullptr, c->location};
      if (c->n_fields == 2) star.table = str(c->fields[0]);
      return star;
    }
    if (last->node_case != PG_QUERY__NODE__NODE_STRING) reject("column reference not supported", c->location);
    Expr e{Expr::Kind::Column, str(last), 0, nullptr, nullptr, c->location};
    if (c->n_fields == 2) e.table = str(c->fields[0]);
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_PARAM_REF) {
    const auto* p = n->param_ref;
    if (p->number < 1) reject("bad parameter", p->location);
    return Expr{Expr::Kind::Param, {}, static_cast<std::uint32_t>(p->number - 1), nullptr, nullptr, p->location};
  }
  if (n->node_case == PG_QUERY__NODE__NODE_A_EXPR) {
    const auto* a = n->a_expr;
    auto op = a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_OP && a->n_name == 1 ? str(a->name[0]) : "";
    // A comparison as a value; PostgreSQL's grammar spells != as <>.
    if (a->lexpr != nullptr && (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=")) {
      Expr e{Expr::Kind::Compare, {}, 0, nullptr, nullptr, a->location};
      e.cmp = value_compare(op, a->location);
      e.lhs = std::make_unique<Expr>(operand(a->lexpr));
      e.rhs = std::make_unique<Expr>(operand(a->rexpr));
      return e;
    }
    if (op != "+" && op != "-" && op != "*" && op != "/") reject("only +, -, * and / are supported here", a->location);
    // Unary: PostgreSQL's grammar folds - of a number into the literal, so
    // what is left is + of anything and - of an expression.
    if (a->lexpr == nullptr) {
      if (op == "*" || op == "/") reject("operator " + op + " is not a prefix operator", a->location);
      Expr e{op == "+" ? Expr::Kind::Plus : Expr::Kind::Neg, {}, 0, nullptr, nullptr, a->location};
      e.rhs = std::make_unique<Expr>(operand(a->rexpr));
      return e;
    }
    Expr e{op == "+"   ? Expr::Kind::Add
           : op == "-" ? Expr::Kind::Sub
           : op == "*" ? Expr::Kind::Mul
                       : Expr::Kind::Div,
           {}, 0, nullptr, nullptr, a->location};
    e.lhs = std::make_unique<Expr>(operand(a->lexpr));
    e.rhs = std::make_unique<Expr>(operand(a->rexpr));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_FUNC_CALL) {
    const auto* f = n->func_call;
    auto name = str(f->funcname[f->n_funcname - 1]);
    auto known = name == "count" || name == "sum" || name == "min" || name == "max" || name == "avg";
    if (!known || (f->n_funcname == 2 && str(f->funcname[0]) != "pg_catalog") || f->n_funcname > 2) {
      reject("function " + name + " is not supported", f->location);
    }
    auto refuse = [&] [[noreturn]] {
      reject("only COUNT(*), COUNT([DISTINCT] column), and SUM, MIN, MAX and AVG of a column are supported", f->location);
    };
    if ((f->agg_distinct && name != "count") || f->n_agg_order != 0 || f->agg_filter != nullptr ||
        f->over != nullptr || f->agg_within_group || f->func_variadic) {
      refuse();
    }
    Expr e{Expr::Kind::Aggregate, {}, 0, nullptr, nullptr, f->location};
    e.fn = name == "count" ? Expr::Fn::Count
           : name == "sum" ? Expr::Fn::Sum
           : name == "min" ? Expr::Fn::Min
           : name == "max" ? Expr::Fn::Max
                           : Expr::Fn::Avg;
    e.distinct = f->agg_distinct;
    if (f->agg_star && e.fn == Expr::Fn::Count) return e;
    if (f->agg_star || f->n_args != 1 || f->args[0]->node_case != PG_QUERY__NODE__NODE_COLUMN_REF) refuse();
    auto arg = expr(f->args[0]);
    e.column = arg.column;
    e.table = arg.table;
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_A_CONST) return literal_expr(literal(n));
  if (n->node_case == PG_QUERY__NODE__NODE_SUB_LINK) {
    const auto* l = n->sub_link;
    if (l->sub_link_type != PG_QUERY__SUB_LINK_TYPE__EXPR_SUBLINK) {
      reject("only a subquery of one value, (SELECT …), is supported here", l->location);
    }
    Expr e{Expr::Kind::Subquery, {}, 0, nullptr, nullptr, l->location};
    e.subquery = subquery(l);
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_TYPE_CAST) {
    const auto* c = n->type_cast;
    const auto* tn = c->type_name;
    if (tn == nullptr || tn->n_names == 0 || tn->n_array_bounds != 0 || tn->setof || tn->pct_type) {
      reject("type not supported", c->location);
    }
    Expr e{Expr::Kind::Cast, {}, 0, nullptr, nullptr, c->location};
    e.cast_type = str(tn->names[tn->n_names - 1]);
    for (std::size_t k = 0; k < tn->n_typmods; ++k) {
      const auto* m = tn->typmods[k];
      if (m->node_case != PG_QUERY__NODE__NODE_A_CONST || m->a_const->val_case != PG_QUERY__A__CONST__VAL_IVAL) {
        reject("type modifier must be an integer", tn->location);
      }
      e.cast_typmods.push_back(m->a_const->ival->ival);
    }
    e.rhs = std::make_unique<Expr>(operand(c->arg));
    return e;
  }
  reject("expression not supported", -1);
}

auto compare_op(const std::string& op, std::int32_t pos) -> Compare::Op {
  if (op == "=") return Compare::Op::Eq;
  if (op == "<") return Compare::Op::Lt;
  if (op == "<=") return Compare::Op::Le;
  if (op == ">") return Compare::Op::Gt;
  if (op == ">=") return Compare::Op::Ge;
  reject("operator " + op + " is not supported in WHERE", pos);
}

auto predicate(const PgQuery__Node* n, const std::string& clause, std::int32_t pos) -> Compare;

auto compare_of(Compare::Op op, Expr lhs, Expr rhs, std::int32_t pos, bool on) -> Compare {
  return Compare{op, std::move(lhs), std::move(rhs), pos, on};
}

auto of_kind(Compare::Kind kind, std::vector<Compare> args, std::int32_t pos, bool on) -> Compare {
  Compare c{Compare::Op::Eq, {}, {}, pos, on};
  c.kind = kind;
  c.args = std::move(args);
  return c;
}

auto negated(Compare c) -> Compare {
  auto pos = c.pos;
  auto on = c.on;
  std::vector<Compare> one;
  one.push_back(std::move(c));
  return of_kind(Compare::Kind::Not, std::move(one), pos, on);
}

// A WHERE clause's conjunct, as a comparison or a predicate over them:
// AND, OR, NOT, IS [NOT] NULL, <> (NOT =), BETWEEN [SYMMETRIC] (>= and <=;
// SYMMETRIC either way round) and [NOT] IN of a list (= of each, OR'd).
auto predicate(const PgQuery__Node* n, const std::string& clause, std::int32_t pos) -> Compare {
  auto on = clause == "ON";
  if (n == nullptr) reject(clause + " must compare a column", pos);
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR) {
    const auto* b = n->bool_expr;
    std::vector<Compare> args;
    for (std::size_t i = 0; i < b->n_args; ++i) args.push_back(predicate(b->args[i], clause, pos));
    switch (b->boolop) {
      case PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR: return of_kind(Compare::Kind::And, std::move(args), b->location, on);
      case PG_QUERY__BOOL_EXPR_TYPE__OR_EXPR: return of_kind(Compare::Kind::Or, std::move(args), b->location, on);
      case PG_QUERY__BOOL_EXPR_TYPE__NOT_EXPR: return of_kind(Compare::Kind::Not, std::move(args), b->location, on);
      case PG_QUERY__BOOL_EXPR_TYPE__BOOL_EXPR_TYPE_UNDEFINED:
      case _PG_QUERY__BOOL_EXPR_TYPE_IS_INT_SIZE: break;
    }
    reject("boolean operator not supported in " + clause, b->location);
  }
  if (n->node_case == PG_QUERY__NODE__NODE_NULL_TEST) {
    const auto* t = n->null_test;
    if (t->argisrow) reject("IS NULL of a row is not supported", t->location);
    Compare c{Compare::Op::Eq, expr(t->arg), {}, t->location, on};
    c.kind = Compare::Kind::IsNull;
    if (t->nulltesttype == PG_QUERY__NULL_TEST_TYPE__IS_NOT_NULL) return negated(std::move(c));
    return c;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_SUB_LINK) {
    const auto* l = n->sub_link;
    Compare c{Compare::Op::Eq, {}, {}, l->location, on};
    if (l->sub_link_type == PG_QUERY__SUB_LINK_TYPE__EXISTS_SUBLINK) {
      c.kind = Compare::Kind::Exists;
    } else if (l->sub_link_type == PG_QUERY__SUB_LINK_TYPE__ANY_SUBLINK ||
               l->sub_link_type == PG_QUERY__SUB_LINK_TYPE__ALL_SUBLINK) {
      c.kind = Compare::Kind::Quantified;
      c.lhs = expr(l->testexpr);
      // IN (SELECT …) names no operator: it is = ANY.
      c.quantified = l->n_oper_name == 0 ? ValueCompare::Eq
                                         : value_compare(str(l->oper_name[l->n_oper_name - 1]), l->location);
      c.all = l->sub_link_type == PG_QUERY__SUB_LINK_TYPE__ALL_SUBLINK;
    } else if (l->sub_link_type == PG_QUERY__SUB_LINK_TYPE__EXPR_SUBLINK) {
      reject("a subquery of one value is not a predicate; compare it", l->location);
    } else {
      reject("this subquery form is not supported", l->location);
    }
    c.subquery = subquery(l);
    return c;
  }
  if (n->node_case != PG_QUERY__NODE__NODE_A_EXPR) reject(clause + " must compare a column", pos);
  const auto* a = n->a_expr;
  auto list = [&](const PgQuery__Node* l) {
    if (l == nullptr || l->node_case != PG_QUERY__NODE__NODE_LIST) reject(clause + " must compare a column", a->location);
    return l->list;
  };
  switch (a->kind) {
    case PG_QUERY__A__EXPR__KIND__AEXPR_OP: {
      if (a->n_name != 1 || a->lexpr == nullptr) break;
      auto op = str(a->name[0]);
      if (op == "<>") return negated(compare_of(Compare::Op::Eq, expr(a->lexpr), expr(a->rexpr), a->location, on));
      return compare_of(compare_op(op, a->location), expr(a->lexpr), expr(a->rexpr), a->location, on);
    }
    case PG_QUERY__A__EXPR__KIND__AEXPR_BETWEEN:
    case PG_QUERY__A__EXPR__KIND__AEXPR_NOT_BETWEEN:
    case PG_QUERY__A__EXPR__KIND__AEXPR_BETWEEN_SYM:
    case PG_QUERY__A__EXPR__KIND__AEXPR_NOT_BETWEEN_SYM: {
      const auto* bounds = list(a->rexpr);
      if (bounds->n_items != 2) reject("BETWEEN takes two bounds", a->location);
      auto x = expr(a->lexpr);
      auto lo = expr(bounds->items[0]);
      auto hi = expr(bounds->items[1]);
      auto within = [&](const Expr& from, const Expr& to) {
        std::vector<Compare> both;
        both.push_back(compare_of(Compare::Op::Ge, clone(x), clone(from), a->location, on));
        both.push_back(compare_of(Compare::Op::Le, clone(x), clone(to), a->location, on));
        return of_kind(Compare::Kind::And, std::move(both), a->location, on);
      };
      auto sym = a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_BETWEEN_SYM || a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_NOT_BETWEEN_SYM;
      auto c = within(lo, hi);
      if (sym) {
        std::vector<Compare> either;
        either.push_back(std::move(c));
        either.push_back(within(hi, lo));
        c = of_kind(Compare::Kind::Or, std::move(either), a->location, on);
      }
      auto negate = a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_NOT_BETWEEN || a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_NOT_BETWEEN_SYM;
      if (negate) return negated(std::move(c));
      return c;
    }
    case PG_QUERY__A__EXPR__KIND__AEXPR_LIKE: {
      // ~~ is LIKE, !~~ NOT LIKE; ESCAPE comes as like_escape(pattern, escape).
      auto op = a->n_name == 1 ? str(a->name[0]) : "";
      Compare c{Compare::Op::Eq, expr(a->lexpr), {}, a->location, on};
      c.kind = Compare::Kind::Like;
      const auto* r = a->rexpr;
      if (r != nullptr && r->node_case == PG_QUERY__NODE__NODE_FUNC_CALL &&
          str(r->func_call->funcname[r->func_call->n_funcname - 1]) == "like_escape" && r->func_call->n_args == 2) {
        const auto* e = r->func_call->args[1];
        if (e->node_case != PG_QUERY__NODE__NODE_A_CONST || e->a_const->val_case != PG_QUERY__A__CONST__VAL_SVAL) {
          reject("ESCAPE takes a string literal", a->location);
        }
        c.escape = e->a_const->sval->sval;
        r = r->func_call->args[0];
      }
      c.rhs = expr(r);
      if (op == "!~~") return negated(std::move(c));
      return c;
    }
    case PG_QUERY__A__EXPR__KIND__AEXPR_IN: {
      if (a->rexpr == nullptr || a->rexpr->node_case != PG_QUERY__NODE__NODE_LIST) {
        reject("IN of a subquery is not supported", a->location);
      }
      const auto* items = a->rexpr->list;
      auto x = expr(a->lexpr);
      std::vector<Compare> any;
      for (std::size_t i = 0; i < items->n_items; ++i) {
        any.push_back(compare_of(Compare::Op::Eq, clone(x), expr(items->items[i]), a->location, on));
      }
      // IN of one is its equality alone.
      auto c = any.size() == 1 ? std::move(any[0]) : of_kind(Compare::Kind::Or, std::move(any), a->location, on);
      if (a->n_name == 1 && str(a->name[0]) == "<>") return negated(std::move(c));  // NOT IN
      return c;
    }
    case PG_QUERY__A__EXPR__KIND__A_EXPR_KIND_UNDEFINED:
    case PG_QUERY__A__EXPR__KIND__AEXPR_OP_ANY:
    case PG_QUERY__A__EXPR__KIND__AEXPR_OP_ALL:
    case PG_QUERY__A__EXPR__KIND__AEXPR_DISTINCT:
    case PG_QUERY__A__EXPR__KIND__AEXPR_NOT_DISTINCT:
    case PG_QUERY__A__EXPR__KIND__AEXPR_ILIKE: reject("ILIKE is not supported: its case folding is a locale's", a->location);
    case PG_QUERY__A__EXPR__KIND__AEXPR_NULLIF:
    case PG_QUERY__A__EXPR__KIND__AEXPR_SIMILAR:
    case _PG_QUERY__A__EXPR__KIND_IS_INT_SIZE: break;
  }
  reject(clause + " must compare a column", a->location);
}

// A WHERE clause as a list of conjuncts: its top-level AND's, each a
// comparison or a predicate.
void where(const PgQuery__Node* n, std::vector<Compare>& out, const std::string& clause = "WHERE",
           std::int32_t pos = -1) {
  if (n == nullptr) return;
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR && n->bool_expr->boolop == PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR) {
    const auto* b = n->bool_expr;
    for (std::size_t i = 0; i < b->n_args; ++i) where(b->args[i], out, clause, pos);
    return;
  }
  out.push_back(predicate(n, clause, pos));
}

auto literal(const PgQuery__Node* n) -> Literal {
  if (n == nullptr || n->node_case != PG_QUERY__NODE__NODE_A_CONST) reject("DEFAULT must be a constant", -1);
  const auto* c = n->a_const;
  Literal l{Literal::Kind::Null, {}, false, c->location};
  if (c->isnull) return l;
  switch (c->val_case) {
    case PG_QUERY__A__CONST__VAL_IVAL:
      l.kind = Literal::Kind::Integer;
      l.text = std::to_string(c->ival->ival);
      return l;
    case PG_QUERY__A__CONST__VAL_FVAL:
      // PostgreSQL gives an integer that does not fit 32 bits as a Float too.
      l.text = c->fval->fval;
      l.kind = l.text.find_first_of(".eE") == std::string::npos ? Literal::Kind::Integer : Literal::Kind::Number;
      return l;
    case PG_QUERY__A__CONST__VAL_SVAL:
      l.kind = Literal::Kind::String;
      l.text = c->sval->sval;
      return l;
    case PG_QUERY__A__CONST__VAL_BOOLVAL:
      l.kind = Literal::Kind::Bool;
      l.b = c->boolval->boolval != 0;
      return l;
    case PG_QUERY__A__CONST__VAL_BSVAL:
    case PG_QUERY__A__CONST__VAL__NOT_SET:
    case _PG_QUERY__A__CONST__VAL__CASE_IS_INT_SIZE: break;
  }
  reject("constant not supported", c->location);
}

// A UNIQUE constraint as a unique index takes it: none of NULLS NOT
// DISTINCT, INCLUDE, DEFERRABLE, USING INDEX or index options yet.
void plain_unique(const PgQuery__Constraint* c) {
  if (c->nulls_not_distinct || c->n_including != 0 || c->deferrable || c->initdeferred || !empty(c->indexname) ||
      !empty(c->indexspace) || c->n_options != 0) {
    reject("UNIQUE with NULLS NOT DISTINCT, INCLUDE, DEFERRABLE, USING INDEX or options is not supported", c->location);
  }
}

// A FOREIGN KEY as it is accepted: MATCH SIMPLE, NO ACTION or RESTRICT on
// DELETE and UPDATE, not deferred. Not enforced, so an action it would take
// is refused rather than ignored.
auto foreign_key(const PgQuery__Constraint* c) -> ForeignKeyAst {
  auto action = [&](const char* a, std::string_view on) {
    auto v = std::string_view{a == nullptr ? "" : a};
    if (v.empty() || v == "a" || v == "r") return;
    auto what = v == "c" ? "CASCADE" : v == "n" ? "SET NULL" : v == "d" ? "SET DEFAULT" : "this action";
    reject("ON " + std::string{on} + " " + what + " is not supported", c->location);
  };
  action(c->fk_del_action, "DELETE");
  action(c->fk_upd_action, "UPDATE");
  if (!empty(c->fk_matchtype) && std::string_view{c->fk_matchtype} != "s") {
    reject("MATCH FULL and MATCH PARTIAL are not supported", c->location);
  }
  if (c->deferrable || c->initdeferred || c->skip_validation || c->n_fk_del_set_cols != 0) {
    reject("a FOREIGN KEY that is DEFERRABLE, NOT VALID or sets columns is not supported", c->location);
  }
  ForeignKeyAst f{c->conname, {}, relation(c->pktable), {}, c->location};
  for (std::size_t k = 0; k < c->n_fk_attrs; ++k) f.columns.push_back(str(c->fk_attrs[k]));
  for (std::size_t k = 0; k < c->n_pk_attrs; ++k) f.ref_columns.push_back(str(c->pk_attrs[k]));
  return f;
}

// Adds each PRIMARY KEY constraint on the column to `primary_keys`, so a
// repeated one counts twice.
auto column_def(const PgQuery__ColumnDef* cd, int& primary_keys) -> ColumnDefAst {
  const auto* tn = cd->type_name;
  if (tn == nullptr || tn->n_names == 0 || tn->n_array_bounds != 0 || tn->setof || tn->pct_type) {
    reject("column type not supported", cd->location);
  }
  ColumnDefAst col{cd->colname, str(tn->names[tn->n_names - 1]), {}, cd->is_not_null != 0, false, std::nullopt,
                   cd->location};
  for (std::size_t k = 0; k < tn->n_typmods; ++k) {
    const auto* m = tn->typmods[k];
    if (m->node_case != PG_QUERY__NODE__NODE_A_CONST || m->a_const->val_case != PG_QUERY__A__CONST__VAL_IVAL) {
      reject("type modifier must be an integer", tn->location);
    }
    col.typmods.push_back(m->a_const->ival->ival);
  }
  if (cd->coll_clause != nullptr) reject("COLLATE is not supported", cd->location);
  for (std::size_t k = 0; k < cd->n_constraints; ++k) {
    const auto* c = cd->constraints[k]->constraint;
    if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) {
      col.primary_key = true;
      ++primary_keys;
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_NOTNULL) {
      col.not_null = true;
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_DEFAULT) {
      col.default_value = literal(c->raw_expr);
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_UNIQUE) {
      plain_unique(c);
      col.unique.emplace_back(c->conname);
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_FOREIGN) {
      auto f = foreign_key(c);
      f.columns = {col.name};
      col.references.push_back(std::move(f));
    } else if (c->contype != PG_QUERY__CONSTR_TYPE__CONSTR_NULL) {
      reject("column constraint not supported", c->location);
    }
  }
  return col;
}

auto create_table(const PgQuery__CreateStmt* s, std::int32_t pos) -> CreateTable {
  if (s->n_inh_relations != 0 || s->partspec != nullptr || s->of_typename != nullptr) {
    reject("CREATE TABLE option not supported", s->relation->location);
  }
  CreateTable t{relation(s->relation), {}, {}, pos};
  // One PRIMARY KEY, on a column or on the table: PostgreSQL refuses a
  // second (42P16), so two column-level ones are not a composite key.
  auto primary_keys = 0;
  auto at_most_one = [&](std::int32_t location) {
    if (primary_keys > 1) reject("multiple primary keys for table \"" + t.table + "\" are not allowed", location);
  };
  for (std::size_t i = 0; i < s->n_table_elts; ++i) {
    const auto* el = s->table_elts[i];
    if (el->node_case == PG_QUERY__NODE__NODE_CONSTRAINT) {
      const auto* c = el->constraint;
      if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_UNIQUE) {
        plain_unique(c);
        UniqueAst u{c->conname, {}, c->location};
        for (std::size_t k = 0; k < c->n_keys; ++k) u.columns.push_back(str(c->keys[k]));
        t.unique.push_back(std::move(u));
        continue;
      }
      if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_FOREIGN) {
        t.foreign_keys.push_back(foreign_key(c));
        continue;
      }
      if (c->contype != PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) reject("table constraint not supported", c->location);
      ++primary_keys;
      at_most_one(c->location);
      for (std::size_t k = 0; k < c->n_keys; ++k) t.pk.push_back(str(c->keys[k]));
      continue;
    }
    if (el->node_case != PG_QUERY__NODE__NODE_COLUMN_DEF) reject("table element not supported", -1);
    t.columns.push_back(column_def(el->column_def, primary_keys));
    at_most_one(t.columns.back().pos);
    for (const auto& name : t.columns.back().unique) t.unique.push_back({name, {t.columns.back().name}, t.columns.back().pos});
    for (auto& f : t.columns.back().references) t.foreign_keys.push_back(std::move(f));
    t.columns.back().references.clear();
  }
  return t;
}

auto create_index(const PgQuery__IndexStmt* s, std::int32_t pos) -> CreateIndex {
  if (s->where_clause != nullptr || s->n_index_including_params != 0 || empty(s->idxname) || s->primary ||
      s->isconstraint || s->concurrent || s->if_not_exists || s->nulls_not_distinct ||
      (!empty(s->access_method) && std::string_view{s->access_method} != "btree")) {
    reject("only a named index on plain columns is supported (no WHERE, INCLUDE, CONCURRENTLY, IF NOT EXISTS, "
           "NULLS NOT DISTINCT or access method)",
           s->relation->location);
  }
  CreateIndex ix{s->idxname, relation(s->relation), {}, s->unique != 0, pos};
  for (std::size_t i = 0; i < s->n_index_params; ++i) {
    const auto* e = s->index_params[i]->index_elem;
    if (empty(e->name) || e->expr != nullptr || e->n_collation != 0 || e->n_opclass != 0) {
      reject("index expressions, collations and operator classes are not supported", s->relation->location);
    }
    if (e->nulls_ordering != PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_DEFAULT) {
      reject("NULLS FIRST and NULLS LAST are not supported: nulls sort first under ASC and last under DESC",
             s->relation->location);
    }
    auto desc = e->ordering == PG_QUERY__SORT_BY_DIR__SORTBY_DESC;
    if (!desc && e->ordering != PG_QUERY__SORT_BY_DIR__SORTBY_DEFAULT && e->ordering != PG_QUERY__SORT_BY_DIR__SORTBY_ASC) {
      reject("index ordering not supported", s->relation->location);
    }
    ix.columns.push_back({e->name, desc});
  }
  return ix;
}

// DROP TABLE and DROP INDEX, one object each, RESTRICT (the default).
auto drop(const PgQuery__DropStmt* s, std::int32_t pos) -> Statement {
  if (s->n_objects != 1 || s->behavior == PG_QUERY__DROP_BEHAVIOR__DROP_CASCADE || s->concurrent) {
    reject("DROP takes one object, without CASCADE or CONCURRENTLY", pos);
  }
  const auto* obj = s->objects[0];
  if (obj->node_case != PG_QUERY__NODE__NODE_LIST || obj->list->n_items != 1) {
    reject("schema-qualified names are not supported", pos);
  }
  auto name = str(obj->list->items[0]);
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_TABLE) return DropTable{name, s->missing_ok != 0, pos};
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_INDEX) return DropIndex{name, s->missing_ok != 0, pos};
  reject("only DROP TABLE and DROP INDEX are supported", pos);
}

auto alter_table(const PgQuery__AlterTableStmt* s, std::int32_t pos) -> AddColumn {
  if (s->objtype != PG_QUERY__OBJECT_TYPE__OBJECT_TABLE || s->n_cmds != 1 || s->missing_ok) {
    reject("only ALTER TABLE … ADD COLUMN is supported", pos);
  }
  const auto* cmd = s->cmds[0]->alter_table_cmd;
  if (cmd->subtype != PG_QUERY__ALTER_TABLE_TYPE__AT_AddColumn || cmd->missing_ok || cmd->def == nullptr ||
      cmd->def->node_case != PG_QUERY__NODE__NODE_COLUMN_DEF) {
    reject("only ALTER TABLE … ADD COLUMN is supported", pos);
  }
  auto primary_keys = 0;  // the binder refuses ADD COLUMN … PRIMARY KEY
  return AddColumn{relation(s->relation), column_def(cmd->def->column_def, primary_keys), pos};
}

auto insert(const PgQuery__InsertStmt* s, std::int32_t pos) -> Insert {
  if (s->on_conflict_clause != nullptr || s->n_returning_list != 0 || s->with_clause != nullptr) {
    reject("INSERT option not supported", s->relation->location);
  }
  Insert ins{relation(s->relation), {}, {}, pos};
  for (std::size_t i = 0; i < s->n_cols; ++i) ins.columns.emplace_back(s->cols[i]->res_target->name);
  const auto* sel = s->select_stmt == nullptr ? nullptr : s->select_stmt->select_stmt;
  if (sel == nullptr || sel->n_values_lists != 1) reject("INSERT takes exactly one VALUES row", s->relation->location);
  const auto* row = sel->values_lists[0]->list;
  for (std::size_t i = 0; i < row->n_items; ++i) ins.values.push_back(expr(row->items[i]));
  return ins;
}

// LIMIT: an integer constant, or ALL or NULL (no limit). A negative one is
// refused, as PostgreSQL refuses it (2201W).
auto limit(const PgQuery__SelectStmt* s) -> std::optional<std::int64_t> {
  if (s->limit_option == PG_QUERY__LIMIT_OPTION__LIMIT_OPTION_WITH_TIES) reject("FETCH … WITH TIES is not supported", -1);
  const auto* n = s->limit_count;
  if (n == nullptr) return std::nullopt;
  if (n->node_case != PG_QUERY__NODE__NODE_A_CONST) reject("LIMIT takes an integer constant", -1);
  auto l = literal(n);
  if (l.kind == Literal::Kind::Null) return std::nullopt;
  std::int64_t v = 0;
  auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
  if (l.kind != Literal::Kind::Integer || ec != std::errc{} || end != l.text.data() + l.text.size()) {
    reject("LIMIT takes an integer constant", l.pos);
  }
  if (v < 0) reject("LIMIT must not be negative", l.pos);
  return v;
}

// Table aliases: a qualifier naming an alias is rewritten to its table, and
// one naming an aliased table by its own name is refused, as the alias hides
// it (42P01). An ORDER BY name that is a select-list item's AS name, and no
// qualified one, orders by that item's column, as PostgreSQL resolves it
// before the tables' columns. A subquery resolves its own.
void requalify(Expr& e, const std::vector<std::string>& tables, const std::vector<std::string>& aliases) {
  if (!e.table.empty()) {
    for (std::size_t i = 0; i < aliases.size(); ++i) {
      if (!aliases[i].empty() && e.table == aliases[i]) {
        e.table = tables[i];
        break;
      }
      if (!aliases[i].empty() && e.table == tables[i] && !std::ranges::contains(aliases, e.table)) {
        reject("invalid reference to FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable);
      }
    }
  }
  if (e.lhs) requalify(*e.lhs, tables, aliases);
  if (e.rhs) requalify(*e.rhs, tables, aliases);
}

void requalify(Compare& c, const std::vector<std::string>& tables, const std::vector<std::string>& aliases) {
  requalify(c.lhs, tables, aliases);
  requalify(c.rhs, tables, aliases);
  for (auto& a : c.args) requalify(a, tables, aliases);
}

void resolve_aliases(Select& sel, const std::vector<std::string>& aliases) {
  std::vector<std::string> tables{sel.table};
  if (!sel.join.empty()) tables.push_back(sel.join);
  for (std::size_t i = 0; i < aliases.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      auto name = [&](std::size_t k) { return aliases[k].empty() ? tables[k] : aliases[k]; };
      if (name(i) == name(j) && !(aliases[i].empty() && aliases[j].empty())) {
        reject("table name \"" + name(i) + "\" specified more than once", i == 0 ? sel.table_pos : sel.join_pos,
             kDuplicateAlias);
      }
    }
  }
  for (auto& o : sel.order_by) {
    if (!o.table.empty()) continue;
    const Expr* hit = nullptr;
    for (const auto& t : sel.targets) {
      if (t.alias != o.column) continue;
      if (hit != nullptr) reject("ORDER BY \"" + o.column + "\" is ambiguous", o.pos);
      hit = &t;
    }
    if (hit != nullptr && hit->kind == Expr::Kind::Column) {
      o.column = hit->column;
      o.table = hit->table;
    }
  }
  if (std::ranges::all_of(aliases, [](const std::string& a) { return a.empty(); })) return;
  for (auto& e : sel.targets) requalify(e, tables, aliases);
  for (auto& c : sel.where) requalify(c, tables, aliases);
  for (auto& c : sel.having) requalify(c, tables, aliases);
  for (auto& e : sel.group_by) requalify(e, tables, aliases);
  for (auto& o : sel.order_by) {
    Expr e{Expr::Kind::Column, o.column, 0, nullptr, nullptr, o.pos};
    e.table = o.table;
    requalify(e, tables, aliases);
    o.table = e.table;
  }
}

auto select(const PgQuery__SelectStmt* s, std::int32_t pos) -> Select {
  if (s->op != PG_QUERY__SET_OPERATION__SETOP_NONE) reject("set operations are not supported", pos);
  // No table, one, or two: FROM a, b, or a [INNER] JOIN b ON …, whose ON
  // conjuncts join the WHERE's.
  if (s->n_values_lists != 0) reject("VALUES is supported only in INSERT", pos);
  if (s->n_from_clause > 2) reject("SELECT reads one or two tables", pos);
  std::vector<const PgQuery__RangeVar*> tables;
  const PgQuery__Node* on = nullptr;
  std::vector<std::string> using_columns;
  std::string using_alias;
  auto natural = false;
  auto join_kind = Select::JoinKind::Inner;
  for (std::size_t i = 0; i < s->n_from_clause; ++i) {
    const auto* from = s->from_clause[i];
    if (from->node_case == PG_QUERY__NODE__NODE_JOIN_EXPR) {
      const auto* j = from->join_expr;
      // The join has no position of its own; report the table it joins.
      const auto* r = j->rarg;
      auto at = r != nullptr && r->node_case == PG_QUERY__NODE__NODE_RANGE_VAR ? r->range_var->location : pos;
      auto kind = j->jointype == PG_QUERY__JOIN_TYPE__JOIN_LEFT    ? Select::JoinKind::Left
                  : j->jointype == PG_QUERY__JOIN_TYPE__JOIN_RIGHT ? Select::JoinKind::Right
                                                                   : Select::JoinKind::Inner;
      if (j->jointype == PG_QUERY__JOIN_TYPE__JOIN_FULL) reject("FULL JOIN is not supported", at);
      if (s->n_from_clause != 1 ||
          (j->jointype != PG_QUERY__JOIN_TYPE__JOIN_INNER && kind == Select::JoinKind::Inner) || j->alias != nullptr ||
          j->larg == nullptr || r == nullptr || j->larg->node_case != PG_QUERY__NODE__NODE_RANGE_VAR ||
          r->node_case != PG_QUERY__NODE__NODE_RANGE_VAR) {
        reject("only an inner, LEFT or RIGHT join of two tables, by ON, USING or NATURAL, is supported", at);
      }
      join_kind = kind;
      tables = {j->larg->range_var, r->range_var};
      on = j->quals;
      natural = j->is_natural;
      for (std::size_t k = 0; k < j->n_using_clause; ++k) using_columns.push_back(str(j->using_clause[k]));
      if (j->join_using_alias != nullptr) {
        if (j->join_using_alias->n_colnames != 0) reject("a USING alias takes no column list", at);
        using_alias = j->join_using_alias->aliasname;
      }
      continue;
    }
    if (from->node_case != PG_QUERY__NODE__NODE_RANGE_VAR) reject("SELECT reads one or two tables", pos);
    tables.push_back(from->range_var);
  }
  // DISTINCT is a list of one empty node; DISTINCT ON a list of its expressions.
  auto distinct = s->n_distinct_clause == 1 &&
                  (s->distinct_clause[0] == nullptr || s->distinct_clause[0]->node_case == PG_QUERY__NODE__NODE__NOT_SET);
  if (s->n_distinct_clause != 0 && !distinct) reject("DISTINCT ON is not supported", pos);
  if (s->group_distinct || s->limit_offset != nullptr || s->n_locking_clause > 1 || s->with_clause != nullptr ||
      s->n_window_clause != 0 || s->into_clause != nullptr) {
    reject("SELECT clause not supported", pos);
  }
  Select sel{{}, tables.empty() ? "" : from_relation(tables[0]), tables.size() == 2 ? from_relation(tables[1]) : "", {}, {},
             limit(s), pos, false};
  std::vector<std::string> aliases;  // each table's alias, "" for none
  for (const auto* t : tables) aliases.push_back(table_alias(t));
  sel.distinct = distinct;
  sel.using_columns = std::move(using_columns);
  sel.natural = natural;
  sel.using_alias = std::move(using_alias);
  sel.join_kind = join_kind;
  if (!tables.empty()) sel.table_pos = tables[0]->location;
  if (tables.size() == 2) sel.join_pos = tables[1]->location;
  if (s->n_locking_clause == 1) {
    // FOR UPDATE alone: never NOWAIT or SKIP LOCKED, which only mean
    // something to a locker, nor a weaker lock or OF a table list.
    const auto* lc = s->locking_clause[0]->locking_clause;
    if (lc->strength != PG_QUERY__LOCK_CLAUSE_STRENGTH__LCS_FORUPDATE) reject("only FOR UPDATE is supported", pos);
    if (lc->wait_policy != PG_QUERY__LOCK_WAIT_POLICY__LockWaitBlock) {
      reject("NOWAIT and SKIP LOCKED are not supported", pos);
    }
    if (lc->n_locked_rels != 0) reject("FOR UPDATE OF is not supported", pos);
    sel.for_update = true;
  }
  for (std::size_t i = 0; i < s->n_sort_clause; ++i) {
    const auto* sb = s->sort_clause[i]->sort_by;
    // libpg_query records no position for a SortBy: report its column's.
    if (sb->node == nullptr || sb->node->node_case != PG_QUERY__NODE__NODE_COLUMN_REF) {
      reject("ORDER BY takes columns", pos);
    }
    auto col = expr(sb->node);
    if (sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_USING) reject("ORDER BY ... USING is not supported", col.pos);
    std::optional<bool> nulls_first;
    if (sb->sortby_nulls == PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_FIRST) nulls_first = true;
    if (sb->sortby_nulls == PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_LAST) nulls_first = false;
    sel.order_by.push_back({col.column, sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_DESC, col.pos, col.table,
                            nulls_first});
  }
  for (std::size_t i = 0; i < s->n_target_list; ++i) {
    const auto* rt = s->target_list[i]->res_target;
    sel.targets.push_back(expr(rt->val));
    if (!empty(rt->name)) sel.targets.back().alias = rt->name;
  }
  where(on, sel.where, "ON", sel.join_pos);
  where(s->where_clause, sel.where);
  // GROUP BY of columns: no expressions, grouping sets, ROLLUP or CUBE.
  for (std::size_t i = 0; i < s->n_group_clause; ++i) {
    const auto* g = s->group_clause[i];
    if (g->node_case != PG_QUERY__NODE__NODE_COLUMN_REF) reject("GROUP BY takes columns", pos);
    sel.group_by.push_back(expr(g));
  }
  where(s->having_clause, sel.having, "HAVING", pos);
  resolve_aliases(sel, aliases);
  return sel;
}

auto update(const PgQuery__UpdateStmt* s, std::int32_t pos) -> Update {
  if (s->n_from_clause != 0 || s->n_returning_list != 0 || s->with_clause != nullptr) {
    reject("UPDATE option not supported", s->relation->location);
  }
  Update u{relation(s->relation), {}, {}, pos};
  for (std::size_t i = 0; i < s->n_target_list; ++i) {
    const auto* rt = s->target_list[i]->res_target;
    if (rt->n_indirection != 0) reject("subscripted assignment is not supported", rt->location);
    u.set.push_back(Assignment{rt->name, expr(rt->val), rt->location});
  }
  where(s->where_clause, u.where);
  return u;
}

auto del(const PgQuery__DeleteStmt* s, std::int32_t pos) -> Delete {
  if (s->n_using_clause != 0 || s->n_returning_list != 0 || s->with_clause != nullptr) {
    reject("DELETE option not supported", s->relation->location);
  }
  Delete d{relation(s->relation), {}, pos};
  where(s->where_clause, d.where);
  return d;
}

// Owns libpg_query's and protobuf-c's results for the length of a parse.
struct ParseResources {
  PgQueryProtobufParseResult raw{};
  PgQuery__ParseResult* tree{nullptr};
  ParseResources() = default;
  ParseResources(const ParseResources&) = delete;
  ParseResources& operator=(const ParseResources&) = delete;
  ~ParseResources() {
    if (tree != nullptr) pg_query__parse_result__free_unpacked(tree, nullptr);
    pg_query_free_protobuf_parse_result(raw);
  }
};

}  // namespace

auto parse(std::string_view sql) -> Statement {
  std::string text{sql};  // libpg_query takes a NUL-terminated string
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1);
  }
  res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                            reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  if (res.tree == nullptr) reject("unreadable parse tree", -1);
  if (res.tree->n_stmts != 1) reject("expected exactly one statement", -1);
  const auto* raw = res.tree->stmts[0];
  const auto* n = raw->stmt;
  auto pos = raw->stmt_location;
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_STMT) return create_table(n->create_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_INDEX_STMT) return create_index(n->index_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DROP_STMT) return drop(n->drop_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_ALTER_TABLE_STMT) return alter_table(n->alter_table_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_INSERT_STMT) return insert(n->insert_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_SELECT_STMT) return select(n->select_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_UPDATE_STMT) return update(n->update_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DELETE_STMT) return del(n->delete_stmt, pos);
  reject("statement not supported", pos);
}

}  // namespace bytecask::sql::ast
