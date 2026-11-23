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
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <utility>
#include <system_error>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <pg_query.h>
#include <protobuf/pg_query.pb-c.h>
#pragma clang diagnostic pop

export module bytecask.sql.parser;

export import bytecask.sql.ast;
import bytecask.sql.plpgsql;

export namespace bytecask::sql::ast {

// Parses exactly one statement. Throws SqlError for a syntax error (with
// PostgreSQL's message and position) or a form the prototype does not take.
[[nodiscard]] auto parse(std::string_view sql) -> Statement;

// Parses a DECLARE, FETCH, MOVE or CLOSE; nullopt for any other statement,
// which is parse()'s to take. Throws SqlError as parse() does. A DECLARE's
// query is returned as text, for parse() to read as the SELECT it must be.
[[nodiscard]] auto parse_cursor(std::string_view sql) -> std::optional<CursorStatement>;

// Parses a CALL; nullopt for any other statement. Throws SqlError as parse()
// does, and for a named or VARIADIC argument.
[[nodiscard]] auto parse_call(std::string_view sql) -> std::optional<Call>;

// Parses a SAVEPOINT, ROLLBACK TO or RELEASE; nullopt for any other
// statement. Throws SqlError as parse() does.
[[nodiscard]] auto parse_savepoint(std::string_view sql) -> std::optional<SavepointStatement>;

// Parses a SET, RESET or SHOW of a run-time parameter; nullopt for any other
// statement. Throws SqlError as parse() does.
[[nodiscard]] auto parse_variable(std::string_view sql) -> std::optional<VariableStatement>;

// Throws SqlError for what PostgreSQL's raw parser refuses, a syntax error,
// and nothing else: what an aborted transaction reports before 25P02, where
// PostgreSQL's parse analysis (42P16 for two primary keys, say) and the
// forms the layer does not take come after it.
void check_syntax(std::string_view sql);

}  // namespace bytecask::sql::ast

namespace bytecask::sql::ast {

namespace {

[[noreturn]] void reject(const std::string& what, std::int32_t pos, SqlState state = kSyntaxError) {
  throw SqlError{what, pos, state};
}

// jsonb's operators and the JSON functions (docs/jsonb_design.md, stages 3
// and 4): PostgreSQL has them, the layer not yet, so they are refused as a
// feature it lacks (0A000) wherever they are written, before any operand
// is read.
void refuse_jsonb_operator(const std::string& op, std::int32_t pos) {
  static constexpr std::array<std::string_view, 12> kJsonbOperators{"->", "->>", "#>", "#>>", "@>", "<@",
                                                                     "?",  "?|",  "?&", "#-",  "@?", "@@"};
  if (std::ranges::find(kJsonbOperators, op) != kJsonbOperators.end()) {
    reject("operator " + op + " is not supported yet", pos, kFeatureNotSupported);
  }
}

void refuse_json_function(const std::string& name, std::int32_t pos) {
  auto json = name.starts_with("jsonb_") || name.starts_with("json_") || name == "to_json" || name == "to_jsonb" ||
              name == "row_to_json" || name == "array_to_json";
  if (json) reject("function " + name + " is not supported yet", pos, kFeatureNotSupported);
}

// protobuf-c gives an absent string field as "", never as NULL.
auto empty(const char* s) -> bool { return s == nullptr || s[0] == '\0'; }

auto str(const PgQuery__Node* n) -> std::string {
  if (n == nullptr || n->node_case != PG_QUERY__NODE__NODE_STRING) reject("expected a name", -1);
  return n->string->sval;
}

// A name of a relation, procedure or type, as written: [schema.]name
// (docs/catalog_design.md, *Name resolution with schemas*). A third part
// names a database, which the layer does not reach (0A000, PostgreSQL's
// "cross-database references"; `quoted` as PostgreSQL quotes a relation's);
// a fourth is malformed (42601), as `kind` words it.
auto dotted_name(PgQuery__Node* const* items, std::size_t n, std::int32_t pos, bool quoted = false,
                 std::string_view kind = "qualified name") -> ObjectName {
  if (n == 0) reject("expected a name", pos);
  std::string all;
  for (std::size_t i = 0; i < n; ++i) all += (i == 0 ? "" : ".") + str(items[i]);
  if (n > 3) reject("improper " + std::string{kind} + " (too many dotted names): " + all, pos);
  if (n == 3) {
    reject("cross-database references are not implemented: " + (quoted ? "\"" + all + "\"" : all), pos, kFeatureNotSupported);
  }
  return n == 2 ? ObjectName{str(items[0]), str(items[1])} : ObjectName{"", str(items[0])};
}

// A FROM item's table; its alias, if any, is read by select().
auto from_relation(const PgQuery__RangeVar* rv) -> ObjectName {
  if (rv == nullptr) reject("expected a table", -1);
  if (!empty(rv->catalogname)) {
    reject("cross-database references are not implemented: \"" + std::string{rv->catalogname} + "." +
               std::string{rv->schemaname} + "." + std::string{rv->relname} + "\"",
           rv->location, kFeatureNotSupported);
  }
  return ObjectName{empty(rv->schemaname) ? "" : rv->schemaname, rv->relname};
}

auto relation(const PgQuery__RangeVar* rv) -> ObjectName {
  auto name = from_relation(rv);
  if (rv->alias != nullptr) reject("table aliases are not supported", rv->location);
  return name;
}

// A FROM item's alias: "" for none.
auto table_alias(const PgQuery__RangeVar* rv) -> std::string {
  return rv->alias == nullptr ? "" : rv->alias->aliasname;
}

// The names a FROM item's alias gives its first columns (t AS x (a, b)).
auto table_column_aliases(const PgQuery__RangeVar* rv) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (rv->alias == nullptr) return out;
  for (std::size_t i = 0; i < rv->alias->n_colnames; ++i) out.push_back(str(rv->alias->colnames[i]));
  return out;
}

// How PostgreSQL 17 words a type name it cannot find: quoted, except for a
// CREATE PROCEDURE argument (its FunctionParameter path).
enum class TypeNameUse { Quoted, ProcedureArgument };

// The SQL spellings PostgreSQL's grammar turns into pg_catalog's types:
// none of them is a type of pg_catalog itself (pg_catalog.integer is 42704).
auto is_grammar_alias(std::string_view name) -> bool {
  for (std::string_view a : {"integer", "int", "smallint", "bigint", "boolean", "real", "float", "double", "decimal",
                             "dec", "character"}) {
    if (name == a) return true;
  }
  return false;
}

// A type name's schema, refused here as PostgreSQL 17 refuses it where the
// answer needs no catalog (docs/catalog_design.md, *Name resolution with
// schemas*): pg_catalog holds the built-in types, but not under the
// grammar's SQL spellings; public and the other schemas PostgreSQL always
// has hold none the layer can use (42704). Three parts name a database
// (0A000), four or more are malformed (42601). Any other schema is
// returned: a user schema's, which only the binder can tell missing (3F000)
// from present (42704), and "" for none or pg_catalog.
auto type_schema(const PgQuery__TypeName* tn, TypeNameUse use) -> std::string {
  if (tn->n_names == 1) return {};
  std::string dotted;
  for (std::size_t i = 0; i < tn->n_names; ++i) dotted += (i == 0 ? "" : ".") + str(tn->names[i]);
  if (tn->n_names > 3) reject("improper qualified name (too many dotted names): " + dotted, tn->location);
  if (tn->n_names == 3) reject("cross-database references are not implemented: " + dotted, tn->location, kFeatureNotSupported);
  auto missing_type = [&] [[noreturn]] {
    auto named = use == TypeNameUse::Quoted ? "\"" + dotted + "\"" : dotted;
    reject("type " + named + " does not exist", tn->location, kUndefinedObject);
  };
  auto schema = str(tn->names[0]);
  if (schema == "pg_catalog") {
    if (is_grammar_alias(str(tn->names[1]))) missing_type();
    return {};
  }
  if (system_schema(schema) != 0 || schema == "public") missing_type();
  return schema;
}

// A type as a column, a cast or an argument names it: libpg_query's last
// name (int4, varchar, numeric, …), qualified by nothing or pg_catalog, and
// integer modifiers. Arrays, SETOF and %TYPE are refused.
auto type_ast(const PgQuery__TypeName* tn, std::string name, std::int32_t pos, TypeNameUse use = TypeNameUse::Quoted)
    -> ColumnDefAst {
  if (tn == nullptr || tn->n_names == 0 || tn->setof || tn->pct_type) reject("type not supported", pos);
  auto schema = type_schema(tn, use);
  // A user schema's type is refused by the binder, its schema first.
  if (tn->n_array_bounds != 0 && schema.empty()) reject("type not supported", pos);
  ColumnDefAst out;
  out.type_schema = std::move(schema);
  out.name = std::move(name);
  out.type = str(tn->names[tn->n_names - 1]);
  out.pos = pos;
  for (std::size_t k = 0; k < tn->n_typmods; ++k) {
    const auto* m = tn->typmods[k];
    if (m->node_case != PG_QUERY__NODE__NODE_A_CONST || m->a_const->val_case != PG_QUERY__A__CONST__VAL_IVAL) {
      reject("type modifier must be an integer", tn->location);
    }
    out.typmods.push_back(m->a_const->ival->ival);
  }
  return out;
}

auto expr(const PgQuery__Node* n) -> Expr;

// The clock or session function a call of no arguments spells, or null.
auto call_clock(std::string_view name) -> const char* {
  if (name == "now") return "current_timestamp";
  if (name == "current_database") return "current_catalog";
  if (name == "current_schema") return "current_schema";
  return nullptr;
}
auto literal(const PgQuery__Node* n) -> Literal;
auto object_name(const PgQuery__Node* n, std::int32_t pos) -> ObjectName;
// `into_taken`: the statement's INTO, which its caller reads (SELECT … INTO);
// an INTO anywhere else is refused. `nested`: a subquery's, whose
// qualifiers may name the query around it (resolve_aliases).
auto select(const PgQuery__SelectStmt* s, std::int32_t pos, bool into_taken = false, bool nested = false) -> Select;

// A subquery's SELECT, parsed as any SELECT is.
auto subquery(const PgQuery__SubLink* l) -> std::shared_ptr<const Select> {
  if (l->subselect == nullptr || l->subselect->node_case != PG_QUERY__NODE__NODE_SELECT_STMT) {
    reject("a subquery must be a SELECT", l->location);
  }
  return std::make_shared<const Select>(select(l->subselect->select_stmt, l->location, false, true));
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
    const auto* last = c->fields[c->n_fields - 1];
    // [[database.]schema.]table.column, or .*: a database is not reached
    // (0A000), more parts are malformed (42601), as PostgreSQL 17 words them.
    if (c->n_fields > 3) {
      std::string all;
      for (std::size_t i = 0; i < c->n_fields; ++i) {
        all += (i == 0 ? "" : ".") + (c->fields[i]->node_case == PG_QUERY__NODE__NODE_A_STAR ? std::string{"*"} : str(c->fields[i]));
      }
      if (c->n_fields > 4) reject("improper qualified name (too many dotted names): " + all, c->location);
      reject("cross-database references are not implemented: " + all, c->location, kFeatureNotSupported);
    }
    auto qualify = [&](Expr& e) {
      if (c->n_fields >= 2) e.table = str(c->fields[c->n_fields - 2]);
      if (c->n_fields == 3) e.schema = str(c->fields[0]);
    };
    if (last->node_case == PG_QUERY__NODE__NODE_A_STAR) {
      Expr star{Expr::Kind::Star, {}, 0, nullptr, nullptr, c->location};
      qualify(star);
      return star;
    }
    if (last->node_case != PG_QUERY__NODE__NODE_STRING) reject("column reference not supported", c->location);
    Expr e{Expr::Kind::Column, str(last), 0, nullptr, nullptr, c->location};
    qualify(e);
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
    refuse_jsonb_operator(op, a->location);
    // A comparison as a value; PostgreSQL's grammar spells != as <>.
    if (a->lexpr != nullptr && (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=")) {
      Expr e{Expr::Kind::Compare, {}, 0, nullptr, nullptr, a->location};
      e.cmp = value_compare(op, a->location);
      e.lhs = std::make_unique<Expr>(operand(a->lexpr));
      e.rhs = std::make_unique<Expr>(operand(a->rexpr));
      return e;
    }
    if (a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_NULLIF) {
      Expr e{Expr::Kind::NullIf, {}, 0, nullptr, nullptr, a->location};
      e.lhs = std::make_unique<Expr>(operand(a->lexpr));
      e.rhs = std::make_unique<Expr>(operand(a->rexpr));
      return e;
    }
    if ((op == "||" || op == "%") && a->lexpr != nullptr) {
      Expr e{Expr::Kind::Func, {}, 0, nullptr, nullptr, a->location};
      e.func = op;
      e.args.push_back(operand(a->lexpr));
      e.args.push_back(operand(a->rexpr));
      return e;
    }
    if (op != "+" && op != "-" && op != "*" && op != "/") reject("only +, -, *, / and % are supported here", a->location);
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
    refuse_json_function(name, f->location);
    // PostgreSQL's grammar spells TRIM, POSITION, SUBSTRING, OVERLAY and
    // EXTRACT as calls of these, their operands in the function's order
    // (EXTRACT's unit a string constant first).
    static constexpr std::array kStringFunctions{"upper",        "lower",         "substring", "btrim",  "ltrim",  "rtrim",
                                                 "position",     "char_length",   "character_length", "octet_length", "overlay",
                                                 "to_timestamp", "substr", "random", "abs", "extract"};
    if (std::ranges::find(kStringFunctions, name) != kStringFunctions.end() &&
        (f->n_funcname == 1 || (f->n_funcname == 2 && str(f->funcname[0]) == "pg_catalog"))) {
      if (f->agg_star || f->agg_distinct || f->n_agg_order != 0 || f->agg_filter != nullptr || f->over != nullptr ||
          f->agg_within_group || f->func_variadic) {
        reject("function " + name + " takes plain arguments", f->location);
      }
      Expr e{Expr::Kind::Func, {}, 0, nullptr, nullptr, f->location};
      e.func = name;
      for (std::size_t k = 0; k < f->n_args; ++k) e.args.push_back(operand(f->args[k]));
      return e;
    }
    // now(), current_database() and current_schema() are CURRENT_TIMESTAMP,
    // CURRENT_CATALOG and CURRENT_SCHEMA spelled as calls, named as
    // PostgreSQL names the call.
    if (auto clock = call_clock(name); clock != nullptr && f->n_args == 0 &&
        (f->n_funcname == 1 || (f->n_funcname == 2 && str(f->funcname[0]) == "pg_catalog")) && !f->agg_star &&
        !f->agg_distinct && f->n_agg_order == 0 && f->agg_filter == nullptr && f->over == nullptr && !f->agg_within_group &&
        !f->func_variadic) {
      Expr e{Expr::Kind::Clock, {}, 0, nullptr, nullptr, f->location};
      e.func = clock;
      e.column = name;
      e.constant = -1;
      return e;
    }
    auto known = name == "count" || name == "sum" || name == "min" || name == "max" || name == "avg";
    if (!known || (f->n_funcname == 2 && str(f->funcname[0]) != "pg_catalog") || f->n_funcname > 2) {
      reject("function " + name + " is not supported", f->location);
    }
    auto refuse = [&] [[noreturn]] {
      reject("only COUNT(*), and COUNT, SUM, MIN, MAX and AVG of one argument, ALL or DISTINCT, are supported", f->location);
    };
    if (f->n_agg_order != 0 || f->agg_filter != nullptr ||
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
    if (f->agg_star && e.fn == Expr::Fn::Count) {
      e.star = true;
      return e;
    }
    if (f->agg_star || f->n_args != 1) refuse();
    e.args.push_back(operand(f->args[0]));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_A_CONST) return literal_expr(literal(n));
  if (n->node_case == PG_QUERY__NODE__NODE_NULL_TEST) {
    const auto* t = n->null_test;
    if (t->argisrow) reject("IS NULL of a row is not supported", t->location);
    Expr e{Expr::Kind::IsNull, {}, 0, nullptr, nullptr, t->location};
    e.negated = t->nulltesttype == PG_QUERY__NULL_TEST_TYPE__IS_NOT_NULL;
    e.rhs = std::make_unique<Expr>(operand(t->arg));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR) {
    const auto* b = n->bool_expr;
    auto kind = [&] {
      switch (b->boolop) {
        case PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR: return Expr::Kind::And;
        case PG_QUERY__BOOL_EXPR_TYPE__OR_EXPR: return Expr::Kind::Or;
        case PG_QUERY__BOOL_EXPR_TYPE__NOT_EXPR: return Expr::Kind::Not;
        case PG_QUERY__BOOL_EXPR_TYPE__BOOL_EXPR_TYPE_UNDEFINED:
        case _PG_QUERY__BOOL_EXPR_TYPE_IS_INT_SIZE: break;
      }
      reject("boolean operator not supported", b->location);
    }();
    Expr e{kind, {}, 0, nullptr, nullptr, b->location};
    for (std::size_t k = 0; k < b->n_args; ++k) e.args.push_back(operand(b->args[k]));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_SET_TO_DEFAULT) {
    return Expr{Expr::Kind::Default, {}, 0, nullptr, nullptr, n->set_to_default->location};
  }
  if (n->node_case == PG_QUERY__NODE__NODE_COALESCE_EXPR) {
    const auto* c = n->coalesce_expr;
    Expr e{Expr::Kind::Coalesce, {}, 0, nullptr, nullptr, c->location};
    for (std::size_t k = 0; k < c->n_args; ++k) e.args.push_back(operand(c->args[k]));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_CASE_EXPR) {
    const auto* c = n->case_expr;
    Expr e{Expr::Kind::Case, {}, 0, nullptr, nullptr, c->location};
    if (c->arg != nullptr) e.lhs = std::make_unique<Expr>(operand(c->arg));
    for (std::size_t k = 0; k < c->n_args; ++k) {
      const auto* w = c->args[k]->case_when;
      e.args.push_back(operand(w->expr));
      e.args.push_back(operand(w->result));
    }
    if (c->defresult != nullptr) e.rhs = std::make_unique<Expr>(operand(c->defresult));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_SUB_LINK) {
    const auto* l = n->sub_link;
    if (l->sub_link_type != PG_QUERY__SUB_LINK_TYPE__EXPR_SUBLINK) {
      reject("only a subquery of one value, (SELECT …), is supported here", l->location);
    }
    Expr e{Expr::Kind::Subquery, {}, 0, nullptr, nullptr, l->location};
    e.subquery = subquery(l);
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_SQLVALUE_FUNCTION) {
    const auto* f = n->sqlvalue_function;
    auto name = [&]() -> const char* {
      switch (f->op) {
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_DATE: return "current_date";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_TIME:
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_TIME_N: return "current_time";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_TIMESTAMP:
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_TIMESTAMP_N: return "current_timestamp";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_LOCALTIME:
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_LOCALTIME_N: return "localtime";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_LOCALTIMESTAMP:
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_LOCALTIMESTAMP_N: return "localtimestamp";
        // The session functions: NAMEs the API fills per run, as the clock.
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_ROLE: return "current_role";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_USER: return "current_user";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_USER: return "user";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_SESSION_USER: return "session_user";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_CATALOG: return "current_catalog";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SVFOP_CURRENT_SCHEMA: return "current_schema";
        case PG_QUERY__SQLVALUE_FUNCTION_OP__SQLVALUE_FUNCTION_OP_UNDEFINED:
        case _PG_QUERY__SQLVALUE_FUNCTION_OP_IS_INT_SIZE: break;
      }
      reject("this SQL value function is not supported", f->location);
    }();
    Expr e{Expr::Kind::Clock, {}, 0, nullptr, nullptr, f->location};
    e.func = name;
    e.constant = f->typmod;
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_TYPE_CAST) {
    const auto* c = n->type_cast;
    auto type = type_ast(c->type_name, "", c->location);
    Expr e{Expr::Kind::Cast, {}, 0, nullptr, nullptr, c->location};
    e.cast_type = std::move(type.type);
    e.cast_typmods = std::move(type.typmods);
    e.cast_schema = std::move(type.type_schema);
    e.rhs = std::make_unique<Expr>(operand(c->arg));
    return e;
  }
  reject("expression not supported", -1);
}

auto compare_op(const std::string& op, std::int32_t pos) -> Compare::Op {
  refuse_jsonb_operator(op, pos);
  if (op == "=") return Compare::Op::Eq;
  if (op == "<") return Compare::Op::Lt;
  if (op == "<=") return Compare::Op::Le;
  if (op == ">") return Compare::Op::Gt;
  if (op == ">=") return Compare::Op::Ge;
  reject("operator " + op + " is not supported in WHERE", pos);
}

auto predicate(const PgQuery__Node* n, const std::string& clause, std::int32_t pos, std::uint32_t on = 0,
               std::string_view arg_of = {}) -> Compare;

auto compare_of(Compare::Op op, Expr lhs, Expr rhs, std::int32_t pos, std::uint32_t on) -> Compare {
  return Compare{op, std::move(lhs), std::move(rhs), pos, on};
}

auto of_kind(Compare::Kind kind, std::vector<Compare> args, std::int32_t pos, std::uint32_t on) -> Compare {
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
// SYMMETRIC either way round) and [NOT] IN of a list (= of each, OR'd); or
// TRUE, FALSE or NULL. A numeric constant is PostgreSQL's 42804, naming
// what it is an argument of: `arg_of`, the AND, OR or NOT around it, else
// the clause.
auto predicate(const PgQuery__Node* n, const std::string& clause, std::int32_t pos, std::uint32_t on,
               std::string_view arg_of) -> Compare {
  if (n == nullptr) reject(clause + " must compare a column", pos);
  if (n->node_case == PG_QUERY__NODE__NODE_A_CONST && n->a_const->val_case != PG_QUERY__A__CONST__VAL_SVAL) {
    auto l = literal(n);
    Compare c{Compare::Op::Eq, {}, {}, l.pos, on};
    c.kind = Compare::Kind::Const;
    switch (l.kind) {
      case Literal::Kind::Null: return c;
      case Literal::Kind::Bool: c.truth = l.b; return c;
      case Literal::Kind::Integer:
      case Literal::Kind::Number: {
        // PostgreSQL's type of the literal: integer within int4, then
        // bigint within int8, then numeric.
        auto type = std::string{"numeric"};
        if (l.kind == Literal::Kind::Integer) {
          std::int64_t v = 0;
          const auto* end = l.text.data() + l.text.size();
          if (auto r = std::from_chars(l.text.data(), end, v); r.ec == std::errc{} && r.ptr == end) {
            type = std::in_range<std::int32_t>(v) ? "integer" : "bigint";
          }
        }
        auto of = arg_of.empty() ? (clause == "ON" ? std::string{"JOIN/ON"} : clause) : std::string{arg_of};
        reject("argument of " + of + " must be type boolean, not type " + type, l.pos, kDatatypeMismatch);
      }
      case Literal::Kind::String: break;
    }
    reject(clause + " must compare a column", l.pos);
  }
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR) {
    const auto* b = n->bool_expr;
    std::vector<Compare> args;
    const auto* op = b->boolop == PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR  ? "AND"
                     : b->boolop == PG_QUERY__BOOL_EXPR_TYPE__OR_EXPR ? "OR"
                                                                       : "NOT";
    for (std::size_t i = 0; i < b->n_args; ++i) args.push_back(predicate(b->args[i], clause, pos, on, op));
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
      if (op == "<>") {
        auto eq = compare_of(Compare::Op::Eq, expr(a->lexpr), expr(a->rexpr), a->location, on);
        eq.ne = true;
        return negated(std::move(eq));
      }
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
    case PG_QUERY__A__EXPR__KIND__AEXPR_LIKE:
    case PG_QUERY__A__EXPR__KIND__AEXPR_ILIKE: {
      // ~~ is LIKE, !~~ NOT LIKE, ~~* and !~~* the ILIKEs; ESCAPE comes as
      // like_escape(pattern, escape), a string literal or a $n.
      auto op = a->n_name == 1 ? str(a->name[0]) : "";
      Compare c{Compare::Op::Eq, expr(a->lexpr), {}, a->location, on};
      c.kind = Compare::Kind::Like;
      c.fold = a->kind == PG_QUERY__A__EXPR__KIND__AEXPR_ILIKE;
      const auto* r = a->rexpr;
      if (r != nullptr && r->node_case == PG_QUERY__NODE__NODE_FUNC_CALL &&
          str(r->func_call->funcname[r->func_call->n_funcname - 1]) == "like_escape" && r->func_call->n_args == 2) {
        const auto* e = r->func_call->args[1];
        if (e->node_case == PG_QUERY__NODE__NODE_PARAM_REF) {
          c.escape_param = expr(e).param;
        } else if (e->node_case == PG_QUERY__NODE__NODE_A_CONST && e->a_const->val_case == PG_QUERY__A__CONST__VAL_SVAL) {
          c.escape = e->a_const->sval->sval;
        } else {
          reject("ESCAPE takes a string literal or a parameter", a->location);
        }
        r = r->func_call->args[0];
      }
      c.rhs = expr(r);
      if (op == "!~~" || op == "!~~*") return negated(std::move(c));
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
    case PG_QUERY__A__EXPR__KIND__AEXPR_NULLIF:
    case PG_QUERY__A__EXPR__KIND__AEXPR_SIMILAR:
    case _PG_QUERY__A__EXPR__KIND_IS_INT_SIZE: break;
  }
  reject(clause + " must compare a column", a->location);
}

// A WHERE clause as a list of conjuncts: its top-level AND's, each a
// comparison or a predicate.
// `arg_of`: AND, once inside one.
void where(const PgQuery__Node* n, std::vector<Compare>& out, const std::string& clause = "WHERE",
           std::int32_t pos = -1, std::uint32_t on = 0, std::string_view arg_of = {}) {
  if (n == nullptr) return;
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR && n->bool_expr->boolop == PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR) {
    const auto* b = n->bool_expr;
    for (std::size_t i = 0; i < b->n_args; ++i) where(b->args[i], out, clause, pos, on, "AND");
    return;
  }
  out.push_back(predicate(n, clause, pos, on, arg_of));
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

// A CHECK constraint as it is accepted: not NO INHERIT, not NOT VALID. Its
// expression is kept as the text written between the CHECK's parentheses
// (the catalog stores the text, as a view stores its definition); `sql` is
// the statement, and the constraint's location the start of its CONSTRAINT
// or CHECK keyword, so the first parenthesis outside a quoted name opens it.
auto check_constraint(const PgQuery__Constraint* c, std::string_view sql) -> CheckAst {
  if (c->is_no_inherit || c->skip_validation || c->deferrable || c->initdeferred) {
    reject("a CHECK that is NO INHERIT, NOT VALID or DEFERRABLE is not supported", c->location);
  }
  auto at = static_cast<std::size_t>(std::max(c->location, 0));
  auto open = std::string_view::npos;
  for (auto i = at; i < sql.size() && open == std::string_view::npos; ++i) {
    if (sql[i] == '"') {
      i = sql.find('"', i + 1);
      if (i == std::string_view::npos) break;
    } else if (sql[i] == '(') {
      open = i;
    }
  }
  if (open == std::string_view::npos) reject("CHECK without an expression", c->location);
  auto depth = 0;
  for (auto i = open; i < sql.size(); ++i) {
    auto ch = sql[i];
    if (ch == '\'' || ch == '"') {
      i = sql.find(ch, i + 1);  // a doubled quote reads as two strings in a row, which is the same text
      if (i == std::string_view::npos) break;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')' && --depth == 0) {
      return {c->conname, std::string{sql.substr(open + 1, i - open - 1)}, c->location, {}};
    }
  }
  reject("CHECK without a closing parenthesis", c->location);
}

// Adds each PRIMARY KEY constraint on the column to `primary_keys`, so a
// repeated one counts twice.
auto column_def(const PgQuery__ColumnDef* cd, int& primary_keys, std::string_view sql) -> ColumnDefAst {
  auto col = type_ast(cd->type_name, cd->colname, cd->location);
  col.not_null = cd->is_not_null != 0;
  if (cd->coll_clause != nullptr) reject("COLLATE is not supported", cd->location);
  for (std::size_t k = 0; k < cd->n_constraints; ++k) {
    const auto* c = cd->constraints[k]->constraint;
    if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) {
      col.primary_key = true;
      ++primary_keys;
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_NOTNULL) {
      col.not_null = true;
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_DEFAULT) {
      const auto* d = c->raw_expr;
      auto now = d->node_case == PG_QUERY__NODE__NODE_FUNC_CALL && d->func_call->n_args == 0 &&
                 call_clock(str(d->func_call->funcname[d->func_call->n_funcname - 1])) != nullptr;
      if (d->node_case == PG_QUERY__NODE__NODE_SQLVALUE_FUNCTION || now) {
        auto e = expr(d);
        if (e.kind != Expr::Kind::Clock) reject("DEFAULT takes a constant, the clock or a session function", e.pos);
        col.default_clock = e.func;
        col.default_clock_precision = e.constant;
      } else if (d->node_case == PG_QUERY__NODE__NODE_TYPE_CAST && d->type_cast->arg != nullptr &&
                 d->type_cast->arg->node_case == PG_QUERY__NODE__NODE_A_CONST) {
        // '…'::T: the string, and the type the binder checks it against.
        auto type = type_ast(d->type_cast->type_name, "", d->type_cast->location);
        if (type.type != "jsonb" || !type.type_schema.empty() || !type.typmods.empty()) reject("DEFAULT must be a constant", -1);
        col.default_value = literal(d->type_cast->arg);
        col.default_cast = std::move(type.type);
      } else {
        col.default_value = literal(d);
      }
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_UNIQUE) {
      plain_unique(c);
      col.unique.emplace_back(c->conname);
    } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_CHECK) {
      col.checks.push_back(check_constraint(c, sql));
      col.checks.back().column = col.name;
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

auto create_table(const PgQuery__CreateStmt* s, std::int32_t pos, std::string_view sql) -> CreateTable {
  if (s->n_inh_relations != 0 || s->partspec != nullptr || s->of_typename != nullptr) {
    reject("CREATE TABLE option not supported", s->relation->location);
  }
  CreateTable t{relation(s->relation), {}, {}, pos};
  t.if_not_exists = s->if_not_exists;
  // One PRIMARY KEY, on a column or on the table: PostgreSQL refuses a
  // second (42P16), so two column-level ones are not a composite key.
  auto primary_keys = 0;
  auto at_most_one = [&](std::int32_t location) {
    if (primary_keys > 1) reject("multiple primary keys for table \"" + t.table.name + "\" are not allowed", location,
                                  kInvalidTableDefinition);
  };
  auto elements = [&] {
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
        if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_CHECK) {
          t.checks.push_back(check_constraint(c, sql));
          continue;
        }
        if (c->contype != PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) reject("table constraint not supported", c->location);
        ++primary_keys;
        at_most_one(c->location);
        for (std::size_t k = 0; k < c->n_keys; ++k) t.pk.push_back(str(c->keys[k]));
        continue;
      }
      if (el->node_case != PG_QUERY__NODE__NODE_COLUMN_DEF) reject("table element not supported", -1);
      t.columns.push_back(column_def(el->column_def, primary_keys, sql));
      at_most_one(t.columns.back().pos);
      for (const auto& name : t.columns.back().unique) t.unique.push_back({name, {t.columns.back().name}, t.columns.back().pos});
      for (auto& f : t.columns.back().references) t.foreign_keys.push_back(std::move(f));
      t.columns.back().references.clear();
      for (auto& k : t.columns.back().checks) t.checks.push_back(std::move(k));
      t.columns.back().checks.clear();
    }
  };
  if (!t.if_not_exists) {
    elements();
    return t;
  }
  // PostgreSQL looks for the name before it reads the definition: what the
  // definition would be refused for is raised only if the name is free.
  try {
    elements();
  } catch (const SqlError& e) {
    CreateTable skipped{t.table, {}, {}, pos};
    skipped.if_not_exists = true;
    skipped.refusal = e;
    return skipped;
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

// DROP PROCEDURE name [(argtype, …)]: the list's types, without those marked
// OUT, as libpg_query gives them (docs/procedures_design.md).
auto drop_procedure(const PgQuery__DropStmt* s, std::int32_t pos) -> DropProcedure {
  const auto* obj = s->objects[0];
  if (obj->node_case != PG_QUERY__NODE__NODE_OBJECT_WITH_ARGS) reject("expected a procedure", pos);
  const auto* o = obj->object_with_args;
  DropProcedure out{dotted_name(o->objname, o->n_objname, pos), std::nullopt, s->missing_ok != 0, pos};
  if (o->args_unspecified) return out;
  out.arg_types.emplace();
  for (std::size_t i = 0; i < o->n_objargs; ++i) {
    if (o->objargs[i]->node_case != PG_QUERY__NODE__NODE_TYPE_NAME) reject("expected a type", pos);
    out.arg_types->push_back(type_ast(o->objargs[i]->type_name, "", pos));
  }
  return out;
}

// DROP SCHEMA [IF EXISTS] name [, …] [RESTRICT | CASCADE].
auto drop_schema(const PgQuery__DropStmt* s, std::int32_t pos) -> DropSchema {
  DropSchema out{{}, s->missing_ok != 0, s->behavior == PG_QUERY__DROP_BEHAVIOR__DROP_CASCADE, pos};
  for (std::size_t i = 0; i < s->n_objects; ++i) out.names.push_back(str(s->objects[i]));
  return out;
}

// DROP TABLE, INDEX, VIEW and PROCEDURE, one object each, RESTRICT (the
// default); DROP SCHEMA of a list. PostgreSQL takes CASCADE on every DROP;
// the layer only on DROP SCHEMA, so elsewhere it is a feature not built
// (0A000, #872), not a syntax error.
auto drop(const PgQuery__DropStmt* s, std::int32_t pos) -> Statement {
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_SCHEMA) return drop_schema(s, pos);
  if (s->behavior == PG_QUERY__DROP_BEHAVIOR__DROP_CASCADE) {
    reject("DROP … CASCADE is not supported yet: only DROP SCHEMA takes CASCADE", pos, kFeatureNotSupported);
  }
  if (s->n_objects != 1 || s->concurrent) reject("DROP takes one object, without CONCURRENTLY", pos);
  const auto* obj = s->objects[0];
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_PROCEDURE) return drop_procedure(s, pos);
  using K = DropTypeOrSequence::Kind;
  auto other = s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_DOMAIN     ? std::optional{K::Domain}
               : s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_TYPE     ? std::optional{K::Type}
               : s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_SEQUENCE ? std::optional{K::Sequence}
                                                                          : std::nullopt;
  if (other) return DropTypeOrSequence{*other, object_name(obj, pos), s->missing_ok != 0, pos};
  if (obj->node_case != PG_QUERY__NODE__NODE_LIST) reject("expected a name", pos);
  auto name = dotted_name(obj->list->items, obj->list->n_items, pos, true, "relation name");
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_TABLE) return DropTable{name, s->missing_ok != 0, pos};
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_INDEX) return DropIndex{name, s->missing_ok != 0, pos};
  if (s->remove_type == PG_QUERY__OBJECT_TYPE__OBJECT_VIEW) return DropView{name, s->missing_ok != 0, pos};
  reject("only DROP TABLE, DROP INDEX, DROP VIEW and DROP PROCEDURE are supported", pos);
}

// ------------------------------------------------------- roles and privileges

// CREATE ROLE name, with no option. The layer has no logins (#653): who a
// session is comes from outside it, so LOGIN, PASSWORD and the like, and
// CREATE USER, which is CREATE ROLE … LOGIN, are not taken.
auto create_role(const PgQuery__CreateRoleStmt* s, std::int32_t pos) -> CreateRole {
  if (s->stmt_type != PG_QUERY__ROLE_STMT_TYPE__ROLESTMT_ROLE) reject("CREATE USER and CREATE GROUP are not supported: the layer has no logins", pos);
  if (s->n_options != 0) reject("CREATE ROLE takes no options: the layer has no logins or role attributes", pos);
  return CreateRole{s->role, pos};
}

// A RoleSpec naming a role: its name.
auto named_role(const PgQuery__RoleSpec* r, const char* where, std::int32_t pos) -> std::string {
  if (r->roletype != PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_CSTRING) {
    reject(std::string{"CURRENT_USER, CURRENT_ROLE and SESSION_USER are not supported in "} + where, pos);
  }
  return r->rolename;
}

auto drop_role(const PgQuery__DropRoleStmt* s, std::int32_t pos) -> DropRole {
  DropRole out{{}, s->missing_ok != 0, pos};
  for (std::size_t i = 0; i < s->n_roles; ++i) {
    const auto* r = s->roles[i]->role_spec;
    if (r->roletype != PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_CSTRING) {
      reject("cannot use special role specifier in DROP ROLE", r->location, kInvalidParameterValue);
    }
    out.roles.emplace_back(r->rolename);
  }
  return out;
}

// One name a DROP or a GRANT lists: a relation's RangeVar, a type's TypeName
// or a list of Strings, each [schema.]name.
auto object_name(const PgQuery__Node* n, std::int32_t pos) -> ObjectName {
  if (n->node_case == PG_QUERY__NODE__NODE_RANGE_VAR) return from_relation(n->range_var);
  if (n->node_case == PG_QUERY__NODE__NODE_TYPE_NAME) {
    const auto* tn = n->type_name;
    if (tn->n_typmods != 0 || tn->n_array_bounds != 0) reject("a type is named without modifiers", tn->location);
    return dotted_name(tn->names, tn->n_names, tn->location);
  }
  if (n->node_case == PG_QUERY__NODE__NODE_LIST) return dotted_name(n->list->items, n->list->n_items, pos, true, "relation name");
  reject("expected a name", pos);
}

// CREATE DOMAIN name [AS] type: no constraint, DEFAULT or COLLATE yet.
auto create_domain(const PgQuery__CreateDomainStmt* s, std::int32_t pos) -> CreateDomain {
  if (s->n_constraints != 0 || s->coll_clause != nullptr) {
    reject("a domain's DEFAULT, NOT NULL, CHECK and COLLATE are not supported yet", pos);
  }
  return CreateDomain{dotted_name(s->domainname, s->n_domainname, pos), type_ast(s->type_name, "", pos), pos};
}

// CREATE SEQUENCE [IF NOT EXISTS] name, with no option yet.
auto create_sequence(const PgQuery__CreateSeqStmt* s, std::int32_t pos) -> CreateSequence {
  const auto* rv = s->sequence;
  auto name = from_relation(rv);
  if (!empty(rv->relpersistence) && rv->relpersistence[0] != 'p') reject("CREATE TEMPORARY SEQUENCE is not supported", pos);
  if (s->n_options != 0 || s->for_identity) reject("a sequence's options are not supported yet", pos);
  return CreateSequence{std::move(name), s->if_not_exists != 0, pos};
}

// CREATE TYPE name, the shell type; any other CREATE TYPE, and the other
// definitions PostgreSQL's DefineStmt carries (aggregates, operators, …),
// are refused.
auto create_type(const PgQuery__DefineStmt* s, std::int32_t pos) -> CreateType {
  if (s->kind != PG_QUERY__OBJECT_TYPE__OBJECT_TYPE || s->n_definition != 0 || s->n_args != 0 || s->replace) {
    reject("only CREATE TYPE name, a shell type, is supported", pos);
  }
  return CreateType{dotted_name(s->defnames, s->n_defnames, pos), pos};
}

// GRANT and REVOKE of privileges ON [TABLE] names TO or FROM roles or
// PUBLIC. Not of other objects, ON ALL TABLES IN SCHEMA, role membership or
// ALTER DEFAULT PRIVILEGES. RESTRICT and CASCADE are taken, and alike: a
// session is the bootstrap superuser, so every grant's grantor is the
// owner and no grant depends on another's grant option.
auto grant(const PgQuery__GrantStmt* s, std::int32_t pos) -> Grant {
  // Privileges on a schema are not stored yet (docs/catalog_design.md,
  // *Schema owner and privileges*, #873).
  if (s->targtype != PG_QUERY__GRANT_TARGET_TYPE__ACL_TARGET_OBJECT) {
    reject("GRANT and REVOKE ON ALL … IN SCHEMA are not supported yet", pos, kFeatureNotSupported);
  }
  if (s->objtype == PG_QUERY__OBJECT_TYPE__OBJECT_SCHEMA) {
    reject("GRANT and REVOKE ON SCHEMA are not supported yet", pos, kFeatureNotSupported);
  }
  Grant out;
  if (s->objtype == PG_QUERY__OBJECT_TYPE__OBJECT_TABLE) {
    out.target = Grant::Target::Table;
  } else if (s->objtype == PG_QUERY__OBJECT_TYPE__OBJECT_SEQUENCE) {
    out.target = Grant::Target::Sequence;
  } else if (s->objtype == PG_QUERY__OBJECT_TYPE__OBJECT_DOMAIN) {
    out.target = Grant::Target::Domain;
  } else if (s->objtype == PG_QUERY__OBJECT_TYPE__OBJECT_TYPE) {
    out.target = Grant::Target::Type;
  } else {
    reject("GRANT and REVOKE are supported on tables, views, sequences, domains and types only", pos);
  }
  out.grant = s->is_grant != 0;
  out.grant_option = s->grant_option != 0;
  out.pos = pos;
  for (std::size_t i = 0; i < s->n_objects; ++i) out.objects.push_back(object_name(s->objects[i], pos));
  for (std::size_t i = 0; i < s->n_privileges; ++i) {
    const auto* p = s->privileges[i]->access_priv;
    PrivilegeAst priv{empty(p->priv_name) ? "all" : p->priv_name, {}};
    for (std::size_t k = 0; k < p->n_cols; ++k) priv.columns.push_back(str(p->cols[k]));
    out.privileges.push_back(std::move(priv));
  }
  for (std::size_t i = 0; i < s->n_grantees; ++i) {
    const auto* r = s->grantees[i]->role_spec;
    out.grantees.push_back(r->roletype == PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_PUBLIC ? std::string{}
                                                                                    : named_role(r, "a grantee", r->location));
  }
  if (s->grantor != nullptr && s->grantor->roletype == PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_CSTRING) {
    out.grantor = s->grantor->rolename;
  }
  return out;
}

// CREATE [OR REPLACE] PROCEDURE name (args) LANGUAGE plpgsql AS 'body':
// arguments IN, INOUT or OUT, named, without a default; no other option.
// The body's PL/pgSQL syntax is checked as PostgreSQL checks it with
// check_function_bodies on; its SQL is not bound (docs/procedures_design.md).
auto create_procedure(const PgQuery__CreateFunctionStmt* s, std::int32_t pos, const std::string& text)
    -> CreateProcedure {
  if (!s->is_procedure) reject("CREATE FUNCTION is not supported yet: only procedures are", pos, kFeatureNotSupported);
  CreateProcedure out{dotted_name(s->funcname, s->n_funcname, pos), s->replace != 0, {}, pos, {}};
  for (std::size_t i = 0; i < s->n_parameters; ++i) {
    if (s->parameters[i]->node_case != PG_QUERY__NODE__NODE_FUNCTION_PARAMETER) reject("expected an argument", pos);
    const auto* p = s->parameters[i]->function_parameter;
    auto at = p->arg_type == nullptr ? pos : p->arg_type->location;
    if (p->defexpr != nullptr) reject("a procedure argument's DEFAULT is not supported yet", at, kFeatureNotSupported);
    if (empty(p->name)) reject("a procedure argument needs a name", at, kFeatureNotSupported);
    auto mode = [&] {
      switch (p->mode) {
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNC_PARAM_IN:
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNC_PARAM_DEFAULT: return ProcedureArgAst::Mode::In;
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNC_PARAM_INOUT: return ProcedureArgAst::Mode::InOut;
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNC_PARAM_OUT: return ProcedureArgAst::Mode::Out;
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNC_PARAM_VARIADIC:
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNC_PARAM_TABLE:
        case PG_QUERY__FUNCTION_PARAMETER_MODE__FUNCTION_PARAMETER_MODE_UNDEFINED:
        case _PG_QUERY__FUNCTION_PARAMETER_MODE_IS_INT_SIZE: break;
      }
      reject("VARIADIC arguments are not supported", at, kFeatureNotSupported);
    }();
    out.args.push_back({mode, type_ast(p->arg_type, p->name, at, TypeNameUse::ProcedureArgument)});
  }
  if (s->sql_body != nullptr) reject("BEGIN ATOMIC bodies are not supported: use LANGUAGE plpgsql", pos, kFeatureNotSupported);
  auto has_body = false;
  std::string language;
  for (std::size_t i = 0; i < s->n_options; ++i) {
    const auto* d = s->options[i]->def_elem;
    std::string_view name = d->defname;
    if (name == "language" && d->arg != nullptr) {
      language = str(d->arg);
    } else if (name == "as") {
      has_body = true;
    } else {
      reject("procedure option " + std::string{name} + " is not supported", d->location, kFeatureNotSupported);
    }
  }
  if (language.empty()) reject("no language specified", pos, kInvalidFunctionDefinition);
  if (language != "plpgsql") {
    reject("LANGUAGE " + language + " is not supported: procedures are LANGUAGE plpgsql", pos, kFeatureNotSupported);
  }
  if (!has_body) reject("a procedure needs a body (AS)", pos, kInvalidFunctionDefinition);
  (void)pl::read_body(text, out.args.size(), pos);
  return out;
}

auto alter_table(const PgQuery__AlterTableStmt* s, std::int32_t pos, std::string_view sql) -> AddColumn {
  if (s->objtype != PG_QUERY__OBJECT_TYPE__OBJECT_TABLE || s->n_cmds != 1 || s->missing_ok) {
    reject("only ALTER TABLE … ADD COLUMN is supported", pos);
  }
  const auto* cmd = s->cmds[0]->alter_table_cmd;
  if (cmd->subtype != PG_QUERY__ALTER_TABLE_TYPE__AT_AddColumn || cmd->missing_ok || cmd->def == nullptr ||
      cmd->def->node_case != PG_QUERY__NODE__NODE_COLUMN_DEF) {
    reject("only ALTER TABLE … ADD COLUMN is supported", pos);
  }
  auto primary_keys = 0;  // the binder refuses ADD COLUMN … PRIMARY KEY
  return AddColumn{relation(s->relation), column_def(cmd->def->column_def, primary_keys, sql), pos};
}

// RETURNING's list: its expressions, each with its AS name, as a select
// list's are read.
auto returning_list(PgQuery__Node** list, std::size_t n) -> std::vector<Expr> {
  std::vector<Expr> out;
  for (std::size_t i = 0; i < n; ++i) {
    const auto* rt = list[i]->res_target;
    out.push_back(expr(rt->val));
    if (!empty(rt->name)) out.back().alias = rt->name;
  }
  return out;
}

// ON CONFLICT DO NOTHING, with no target or a list of columns, or DO
// UPDATE SET with a list of columns: ON CONSTRAINT, an inference WHERE, DO
// UPDATE's WHERE and an expression, collation or operator class in the
// target are refused (0A000); ASC/DESC or NULLS FIRST/LAST on a target
// column is 42P10, and DO UPDATE with no target 42601, as in PostgreSQL.
auto on_conflict(const PgQuery__OnConflictClause* c) -> Insert::OnConflict {
  Insert::OnConflict out{{}, c->location, {}};
  if (c->action == PG_QUERY__ON_CONFLICT_ACTION__ONCONFLICT_UPDATE) {
    if (c->infer == nullptr) {
      throw SqlError{"ON CONFLICT DO UPDATE requires inference specification or constraint name", c->location}.with_notes(
          "", "For example, ON CONFLICT (column_name).");
    }
    if (c->where_clause != nullptr) reject("ON CONFLICT DO UPDATE … WHERE is not supported yet", c->location, kFeatureNotSupported);
    std::vector<Assignment> set;
    for (std::size_t i = 0; i < c->n_target_list; ++i) {
      const auto* rt = c->target_list[i]->res_target;
      if (rt->n_indirection != 0) reject("subscripted assignment is not supported", rt->location);
      set.push_back(Assignment{rt->name, expr(rt->val), rt->location});
    }
    out.update = std::make_shared<const std::vector<Assignment>>(std::move(set));
  }
  const auto* infer = c->infer;
  if (infer == nullptr) return out;
  if (!empty(infer->conname)) reject("ON CONFLICT ON CONSTRAINT is not supported yet", infer->location, kFeatureNotSupported);
  if (infer->where_clause != nullptr) {
    reject("ON CONFLICT with a WHERE on its target is not supported yet", infer->location, kFeatureNotSupported);
  }
  for (std::size_t i = 0; i < infer->n_index_elems; ++i) {
    const auto* e = infer->index_elems[i]->index_elem;
    if (e->ordering != PG_QUERY__SORT_BY_DIR__SORTBY_DEFAULT) {
      reject("ASC/DESC is not allowed in ON CONFLICT clause", infer->location, kInvalidColumnReference);
    }
    if (e->nulls_ordering != PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_DEFAULT) {
      reject("NULLS FIRST/LAST is not allowed in ON CONFLICT clause", infer->location, kInvalidColumnReference);
    }
    if (e->expr != nullptr || empty(e->name) || e->n_collation != 0 || e->n_opclass != 0) {
      reject("ON CONFLICT on an expression, a collation or an operator class is not supported yet", infer->location,
             kFeatureNotSupported);
    }
    out.columns.emplace_back(e->name);
  }
  return out;
}

void resolve_schema_qualifier(Expr& e, std::span<const FromItem> items, bool nested);
auto target_item(const ObjectName& table) -> FromItem;

// An INSERT's schema-qualified columns: its RETURNING's and DO UPDATE's
// name its table; a VALUES row names none.
void resolve_schema_qualifiers(Insert& ins) {
  const auto target = target_item(ins.table);
  std::span<const FromItem> items{&target, 1};
  for (auto& e : ins.returning) resolve_schema_qualifier(e, items, false);
  for (auto& row : ins.rows) {
    for (auto& e : row) resolve_schema_qualifier(e, {}, false);
  }
  if (ins.on_conflict && ins.on_conflict->update) {
    auto set = std::vector<Assignment>{};
    for (const auto& a : *ins.on_conflict->update) set.push_back(Assignment{a.column, clone(a.value), a.pos});
    for (auto& a : set) resolve_schema_qualifier(a.value, items, false);
    ins.on_conflict->update = std::make_shared<const std::vector<Assignment>>(std::move(set));
  }
}

auto insert(const PgQuery__InsertStmt* s, std::int32_t pos) -> Insert {
  if (s->with_clause != nullptr) reject("INSERT option not supported", s->relation->location);
  Insert ins{relation(s->relation), {}, {}, pos};
  if (s->on_conflict_clause != nullptr) ins.on_conflict = on_conflict(s->on_conflict_clause);
  for (std::size_t i = 0; i < s->n_cols; ++i) ins.columns.emplace_back(s->cols[i]->res_target->name);
  const auto* sel = s->select_stmt == nullptr ? nullptr : s->select_stmt->select_stmt;
  if (sel == nullptr) reject("INSERT takes VALUES rows or a query", s->relation->location);
  if (sel->n_values_lists == 0) {
    ins.query = std::make_shared<const Select>(select(sel, pos));
    ins.returning = returning_list(s->returning_list, s->n_returning_list);
    resolve_schema_qualifiers(ins);
    return ins;
  }
  for (std::size_t r = 0; r < sel->n_values_lists; ++r) {
    const auto* row = sel->values_lists[r]->list;
    auto& values = ins.rows.emplace_back();
    for (std::size_t i = 0; i < row->n_items; ++i) values.push_back(expr(row->items[i]));
    if (values.size() != ins.rows.front().size()) {
      reject("VALUES lists must all be the same length", values.empty() ? pos : values.front().pos);
    }
  }
  ins.returning = returning_list(s->returning_list, s->n_returning_list);
  resolve_schema_qualifiers(ins);
  return ins;
}

// CREATE VIEW name [(columns)] AS SELECT … [WITH [LOCAL | CASCADED] CHECK
// OPTION]: not OR REPLACE, TEMPORARY or view options (WITH (check_option=…)
// among them).
auto create_view(const PgQuery__ViewStmt* s, std::int32_t pos) -> CreateView {
  auto name = from_relation(s->view);
  if (s->replace || s->n_options != 0 || (!empty(s->view->relpersistence) && s->view->relpersistence[0] != 'p')) {
    reject("CREATE VIEW takes no OR REPLACE, TEMPORARY or options", s->view->location);
  }
  if (s->query == nullptr || s->query->node_case != PG_QUERY__NODE__NODE_SELECT_STMT) {
    reject("a view is defined by a SELECT", pos);
  }
  std::vector<std::string> columns;
  for (std::size_t i = 0; i < s->n_aliases; ++i) columns.push_back(str(s->aliases[i]));
  CreateView out{std::move(name), std::move(columns), select(s->query->select_stmt, pos), pos, {}, CheckOption::None};
  switch (s->with_check_option) {
    case PG_QUERY__VIEW_CHECK_OPTION__LOCAL_CHECK_OPTION: out.check_option = CheckOption::Local; break;
    case PG_QUERY__VIEW_CHECK_OPTION__CASCADED_CHECK_OPTION: out.check_option = CheckOption::Cascaded; break;
    case PG_QUERY__VIEW_CHECK_OPTION__NO_CHECK_OPTION:
    case PG_QUERY__VIEW_CHECK_OPTION__VIEW_CHECK_OPTION_UNDEFINED: break;
    case _PG_QUERY__VIEW_CHECK_OPTION_IS_INT_SIZE: reject("an unknown check option", s->view->location);
  }
  return out;
}

// LIMIT's or OFFSET's count: an integer constant, or NULL (none), as is
// LIMIT ALL. A negative one is refused, as PostgreSQL refuses it (2201W for
// LIMIT, 2201X for OFFSET).
auto row_count(const PgQuery__Node* n, const char* clause, SqlState negative) -> std::optional<std::int64_t> {
  if (n == nullptr) return std::nullopt;
  if (n->node_case != PG_QUERY__NODE__NODE_A_CONST) reject(std::string{clause} + " takes an integer constant", -1);
  auto l = literal(n);
  if (l.kind == Literal::Kind::Null) return std::nullopt;
  std::int64_t v = 0;
  auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
  if (l.kind != Literal::Kind::Integer || ec != std::errc{} || end != l.text.data() + l.text.size()) {
    reject(std::string{clause} + " takes an integer constant", l.pos);
  }
  if (v < 0) reject(std::string{clause} + " must not be negative", l.pos, negative);
  return v;
}

auto limit(const PgQuery__SelectStmt* s) -> std::optional<std::int64_t> {
  if (s->limit_option == PG_QUERY__LIMIT_OPTION__LIMIT_OPTION_WITH_TIES) reject("FETCH … WITH TIES is not supported", -1);
  return row_count(s->limit_count, "LIMIT", kInvalidRowCountInLimit);
}

auto offset(const PgQuery__SelectStmt* s) -> std::int64_t {
  return row_count(s->limit_offset, "OFFSET", kInvalidRowCountInOffset).value_or(0);
}

// Table aliases: a qualifier naming an alias is rewritten to its table, and
// one naming an aliased table by its own name is refused, as the alias hides
// it (42P01). A query that names one table twice keeps its aliases (`keep`),
// which are then what tells its items apart: the qualifiers are checked and
// left as written. An ORDER BY name that is a select-list item's AS name, and
// no qualified one, orders by that item's column, as PostgreSQL resolves it
// before the tables' columns. A subquery resolves its own. A subquery that
// names a table it aliases by the table's own name may mean the query around
// it, as PostgreSQL reads `FROM t WHERE EXISTS (SELECT 1 FROM t AS x WHERE
// x.a = t.b)`: it keeps its aliases and that qualifier as written (`open`),
// and the binder resolves it against the outer query, or refuses it (42P01).
struct Names {
  std::vector<std::string> tables;
  std::vector<std::string> aliases;
  std::vector<std::string> joins;  // USING aliases: each names a join, not a table
  bool keep{false};
  bool open{false};

  [[nodiscard]] auto visible(std::size_t i) const -> const std::string& {
    return aliases[i].empty() ? tables[i] : aliases[i];
  }
};

void requalify(Expr& e, const Names& n);
void requalify(Compare& c, const Names& n);

// A subquery's qualifiers that name the query around it: an alias of the
// outer query is its table, as the outer query's own qualifiers are; a name
// of the subquery's own FROM is its own. One level: a subquery inside a
// subquery is refused by the binder.
void requalify_outer(Expr& e, const Names& outer, const std::vector<std::string>& own) {
  if (!e.table.empty() && !std::ranges::contains(own, e.table)) {
    for (std::size_t i = 0; i < outer.tables.size(); ++i) {
      if (outer.aliases[i].empty()) continue;
      if (e.table == outer.aliases[i]) {
        // The subquery reads the outer table too: the name would be its own.
        if (std::ranges::contains(own, outer.tables[i])) {
          reject("a subquery that reads the table of the query around it, naming the outer one by an alias, is not supported yet (#836)",
                 e.pos);
        }
        e.table = outer.tables[i];
        break;
      }
      if (e.table == outer.tables[i]) {
        reject("invalid reference to FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable);
      }
    }
  }
  if (e.lhs) requalify_outer(*e.lhs, outer, own);
  if (e.rhs) requalify_outer(*e.rhs, outer, own);
  for (auto& a : e.args) requalify_outer(a, outer, own);
}

void requalify_outer(Compare& c, const Names& outer, const std::vector<std::string>& own) {
  requalify_outer(c.lhs, outer, own);
  requalify_outer(c.rhs, outer, own);
  for (auto& a : c.args) requalify_outer(a, outer, own);
}

void requalify_outer(Select& q, const Names& outer) {
  if (q.set) {
    auto set = std::make_shared<SetOperation>(SetOperation{q.set->kind, q.set->all, clone(q.set->left), clone(q.set->right)});
    requalify_outer(set->left, outer);
    requalify_outer(set->right, outer);
    q.set = std::move(set);
    return;
  }
  std::vector<std::string> own;
  for (const auto& item : q.from) own.push_back(item.name());
  for (auto& e : q.targets) requalify_outer(e, outer, own);
  for (auto& c : q.where) requalify_outer(c, outer, own);
  for (auto& c : q.having) requalify_outer(c, outer, own);
  for (auto& e : q.group_by) requalify_outer(e, outer, own);
  for (auto& o : q.order_by) requalify_outer(o.expr, outer, own);
}

// A subquery is shared once parsed: requalified as a copy.
auto requalified(const Select& q, const Names& outer) -> std::shared_ptr<const Select> {
  auto copy = std::make_shared<Select>(clone(q));
  requalify_outer(*copy, outer);
  return copy;
}

void requalify(Expr& e, const Names& n) {
  if (e.subquery && !n.keep) e.subquery = requalified(*e.subquery, n);
  if (!e.table.empty()) {
    auto named = [&](std::size_t i) { return n.visible(i) == e.table; };
    std::size_t visible = 0;
    while (visible < n.tables.size() && !named(visible)) ++visible;
    if (visible < n.tables.size()) {
      if (!n.keep && !n.aliases[visible].empty()) e.table = n.tables[visible];
    } else if (!n.open && !std::ranges::contains(n.joins, e.table)) {
      for (std::size_t i = 0; i < n.tables.size(); ++i) {
        if (!n.aliases[i].empty() && e.table == n.tables[i]) {
          reject("invalid reference to FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable);
        }
      }
    }
  }
  if (e.lhs) requalify(*e.lhs, n);
  if (e.rhs) requalify(*e.rhs, n);
  for (auto& a : e.args) requalify(a, n);
}

void requalify(Compare& c, const Names& n) {
  if (c.subquery && !n.keep) c.subquery = requalified(*c.subquery, n);
  requalify(c.lhs, n);
  requalify(c.rhs, n);
  for (auto& a : c.args) requalify(a, n);
}

// An unqualified ORDER BY name of a plain SELECT is an output name first, as
// PostgreSQL reads it: an item's AS name, or an unaliased column's name. It
// orders by that item's expression, which hides any input column of the
// name; only a name no item has is an input column. Two items of the name
// are 42702 unless they are one: the same column, or expressions written
// alike. Runs before resolve_positions, whose positions already name their
// item.
void resolve_output_names(Select& sel) {
  auto output_name = [](const Expr& t) -> const std::string& {
    return t.alias.empty() && t.kind == Expr::Kind::Column ? t.column : t.alias;
  };
  // Two column items are one where they name one column, a qualified and an
  // unqualified reference included (the binder rejects the latter if ambiguous).
  auto same_item = [](const Expr& x, const Expr& y) {
    if (x.kind == Expr::Kind::Column && y.kind == Expr::Kind::Column) {
      return x.column == y.column && (x.table == y.table || x.table.empty() || y.table.empty());
    }
    return same_expr(x, y);
  };
  for (auto& o : sel.order_by) {
    if (o.position != 0 || o.expr.kind != Expr::Kind::Column || !o.expr.table.empty()) continue;
    std::vector<const Expr*> hits;
    for (const auto& t : sel.targets) {
      if (t.kind != Expr::Kind::Star && output_name(t) == o.expr.column) hits.push_back(&t);
    }
    if (hits.empty()) continue;
    for (std::size_t i = 0; i < hits.size(); ++i) {
      for (std::size_t j = 0; j < i; ++j) {
        if (!same_item(*hits[i], *hits[j])) {
          reject("ORDER BY \"" + o.expr.column + "\" is ambiguous", o.pos, kAmbiguousColumn);
        }
      }
    }
    const auto* hit = *std::ranges::max_element(hits, {}, [](const Expr* t) { return !t->table.empty(); });
    o.expr = clone(*hit);
  }
}

// ORDER BY 2 of a plain SELECT orders by the second select-list item's
// expression. A position past the list is 42P10. An item after a * has no
// position yet: the stars are expanded by the binder.
void resolve_positions(Select& sel) {
  for (auto& o : sel.order_by) {
    if (o.position == 0) continue;
    auto stars = std::ranges::any_of(sel.targets, [](const Expr& e) { return e.kind == Expr::Kind::Star; });
    if (stars) reject("ORDER BY position with * in the select list is not supported yet", o.pos);
    if (std::cmp_greater(o.position, sel.targets.size())) {
      reject("ORDER BY position " + std::to_string(o.position) + " is not in select list", o.pos, kInvalidColumnReference);
    }
    o.expr = clone(sel.targets[static_cast<std::size_t>(o.position - 1)]);
    o.position = 0;
  }
}

// Whether a qualifier of sel's own clauses, not its subqueries', names a
// table sel aliases by the table's name, which the alias hides.
auto names_hidden(const Select& sel, const Names& n) -> bool {
  auto hidden = [&](const std::string& table) {
    if (table.empty()) return false;
    for (std::size_t i = 0; i < n.tables.size(); ++i) {
      if (n.visible(i) == table) return false;
    }
    for (std::size_t i = 0; i < n.tables.size(); ++i) {
      if (!n.aliases[i].empty() && n.tables[i] == table) return true;
    }
    return false;
  };
  std::function<bool(const Expr&)> in_expr = [&](const Expr& e) {
    return hidden(e.table) || (e.lhs && in_expr(*e.lhs)) || (e.rhs && in_expr(*e.rhs)) || std::ranges::any_of(e.args, in_expr);
  };
  std::function<bool(const Compare&)> in_compare = [&](const Compare& c) {
    return in_expr(c.lhs) || in_expr(c.rhs) || std::ranges::any_of(c.args, in_compare);
  };
  return std::ranges::any_of(sel.targets, in_expr) || std::ranges::any_of(sel.where, in_compare) ||
         std::ranges::any_of(sel.having, in_compare) || std::ranges::any_of(sel.group_by, in_expr) ||
         std::ranges::any_of(sel.order_by, [&](const OrderItem& o) { return in_expr(o.expr); });
}

// A qualifier in an ON that names a FROM item before the ON's own join, in
// parentheses or after a comma (Compare::on_from), is PostgreSQL's 42P01,
// with its DETAIL naming the item. A name of no item, or of a later one, is
// the binder's "missing FROM-clause entry"; one an alias hides, resolve_aliases'.
void check_on_scope(const Select& sel, const Names& n) {
  // An item of the ON's own join owns a qualifier only by its visible name: an
  // alias there hides its table's name, so an earlier item by that name is
  // still the invalid reference. An earlier item is matched by either name.
  auto reaches = [&](const std::string& q, std::size_t i) { return n.visible(i) == q || n.tables[i] == q; };
  std::function<void(const Expr&, const Compare&)> in_expr = [&](const Expr& e, const Compare& c) {
    if (!e.table.empty()) {
      auto own = false;
      for (auto i = std::size_t{c.on_from}; i <= c.on && i < n.tables.size(); ++i) own = own || n.visible(i) == e.table;
      for (std::size_t i = 0; i < c.on_from && !own; ++i) {
        if (!reaches(e.table, i)) continue;
        throw SqlError{"invalid reference to FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable}.with_notes(
            "There is an entry for table \"" + n.visible(i) + "\", but it cannot be referenced from this part of the query.", "");
      }
    }
    if (e.lhs) in_expr(*e.lhs, c);
    if (e.rhs) in_expr(*e.rhs, c);
    for (const auto& a : e.args) in_expr(a, c);
  };
  std::function<void(const Compare&)> in_compare = [&](const Compare& c) {
    in_expr(c.lhs, c);
    in_expr(c.rhs, c);
    for (const auto& a : c.args) in_compare(a);
  };
  for (const auto& c : sel.where) {
    if (c.on != 0 && c.on_from != 0) in_compare(c);
  }
}

// Whether two schemas as written are one: an unqualified FROM item is
// public's, the one schema of a session's path (docs/catalog_design.md,
// *The path*).
auto same_schema(std::string_view a, std::string_view b) -> bool {
  auto of = [](std::string_view x) { return x.empty() ? std::string_view{"public"} : x; };
  return of(a) == of(b);
}

// A column qualified by a schema, s.t.c (or s.t.*), names the FROM item of
// table t of schema s that has no alias: t.c alone then names that item, so
// the schema is dropped. Otherwise PostgreSQL 17's 42P01, in its three
// forms (docs/catalog_design.md, *FROM and column references*). In a
// subquery, one that names no item of its own FROM is not supported yet.
void resolve_schema_qualifier(Expr& e, std::span<const FromItem> items, bool nested) {
  if (!e.schema.empty()) {
    auto named = [&](const FromItem& item) { return item.table.name == e.table && same_schema(item.table.schema, e.schema); };
    if (std::ranges::any_of(items, [&](const FromItem& item) { return item.alias.empty() && named(item); })) {
      e.schema.clear();
    } else {
      // In a subquery the name may be an enclosing query's item, which
      // PostgreSQL then reads; this scope's items decide nothing.
      if (nested) {
        reject("a schema-qualified column of the query around a subquery is not supported yet", e.pos, kFeatureNotSupported);
      }
      SqlError invalid{"invalid reference to FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable};
      for (const auto& item : items) {
        if (!item.alias.empty() && named(item)) {
          throw std::move(invalid).with_notes("", "Perhaps you meant to reference the table alias \"" + item.alias + "\".");
        }
      }
      if (std::ranges::any_of(items, [&](const FromItem& item) { return item.name() == e.table; })) {
        throw std::move(invalid).with_notes(
            "There is an entry for table \"" + e.table + "\", but it cannot be referenced from this part of the query.", "");
      }
      reject("missing FROM-clause entry for table \"" + e.table + "\"", e.pos, kUndefinedTable);
    }
  }
  if (e.lhs) resolve_schema_qualifier(*e.lhs, items, nested);
  if (e.rhs) resolve_schema_qualifier(*e.rhs, items, nested);
  for (auto& a : e.args) resolve_schema_qualifier(a, items, nested);
}

void resolve_schema_qualifier(Compare& c, std::span<const FromItem> items, bool nested) {
  resolve_schema_qualifier(c.lhs, items, nested);
  resolve_schema_qualifier(c.rhs, items, nested);
  for (auto& a : c.args) resolve_schema_qualifier(a, items, nested);
}

void resolve_schema_qualifiers(Select& sel, bool nested) {
  for (auto& e : sel.targets) resolve_schema_qualifier(e, sel.from, nested);
  for (auto& c : sel.where) resolve_schema_qualifier(c, sel.from, nested);
  for (auto& c : sel.having) resolve_schema_qualifier(c, sel.from, nested);
  for (auto& e : sel.group_by) resolve_schema_qualifier(e, sel.from, nested);
  for (auto& o : sel.order_by) resolve_schema_qualifier(o.expr, sel.from, nested);
}

// An INSERT's, UPDATE's or DELETE's table, as the one FROM item its own
// expressions' schema-qualified columns may name.
auto target_item(const ObjectName& table) -> FromItem {
  FromItem item;
  item.table = table;
  return item;
}

void resolve_aliases(Select& sel, bool nested) {
  resolve_schema_qualifiers(sel, nested);
  Names names;
  for (const auto& item : sel.from) {
    names.tables.push_back(item.table.name);  // bare: alias erasure compares names alone
    names.aliases.push_back(item.alias);
    if (!item.using_alias.empty()) names.joins.push_back(item.using_alias);
  }
  for (std::size_t i = 0; i < names.tables.size(); ++i) {
    // A USING alias with the name of a table the FROM aliases otherwise
    // would be that table once the aliases are dropped.
    names.keep = names.keep || std::ranges::contains(names.joins, names.tables[i]);
    for (std::size_t j = 0; j < i; ++j) {
      if (names.visible(i) == names.visible(j) && sel.from[i].alias.empty() && sel.from[j].alias.empty() &&
          !same_schema(sel.from[i].table.schema, sel.from[j].table.schema)) {
        // PostgreSQL reads both; a qualifier then picks one, an unqualified
        // one is 42P09. Not built: the binder tells FROM items apart by
        // name alone.
        reject("a FROM of one table name from two schemas, neither aliased, is not supported yet: give one an alias",
               sel.from[i].pos, kFeatureNotSupported);
      }
      if (names.visible(i) == names.visible(j)) {
        reject("table name \"" + names.visible(i) + "\" specified more than once", sel.from[i].pos, kDuplicateAlias);
      }
      names.keep = names.keep || names.tables[i] == names.tables[j];
    }
    // A USING alias names its join in the same FROM, as a table or alias
    // does: no two of these names may match (42712).
    auto twice = [&] [[noreturn]] (const std::string& name) {
      reject("table name \"" + name + "\" specified more than once", sel.from[i].pos, kDuplicateAlias);
    };
    for (std::size_t j = 0; j < i; ++j) {
      if (!sel.from[j].using_alias.empty() && sel.from[j].using_alias == names.visible(i)) twice(names.visible(i));
    }
    const auto& alias = sel.from[i].using_alias;
    if (alias.empty()) continue;
    for (std::size_t j = 0; j <= i; ++j) {
      if (names.visible(j) == alias || (j < i && sel.from[j].using_alias == alias)) twice(alias);
    }
  }
  check_on_scope(sel, names);
  // Renamed columns exist only under the alias, which the binder reads.
  for (const auto& item : sel.from) names.keep = names.keep || !item.column_aliases.empty();
  if (nested && names_hidden(sel, names)) {
    names.keep = true;
    names.open = true;
  }
  if (std::ranges::all_of(names.aliases, [](const std::string& a) { return a.empty(); })) return;
  for (auto& e : sel.targets) requalify(e, names);
  for (auto& c : sel.where) requalify(c, names);
  for (auto& c : sel.having) requalify(c, names);
  for (auto& e : sel.group_by) requalify(e, names);
  for (auto& o : sel.order_by) requalify(o.expr, names);
  if (names.keep) return;
  for (auto& item : sel.from) item.alias.clear();
}

// ORDER BY's keys, in order: expressions, or select-list positions.
auto order_items(const PgQuery__SelectStmt* s, std::int32_t pos) -> std::vector<OrderItem> {
  std::vector<OrderItem> out;
  for (std::size_t i = 0; i < s->n_sort_clause; ++i) {
    const auto* sb = s->sort_clause[i]->sort_by;
    if (sb->node == nullptr) reject("ORDER BY takes expressions", pos);
    Expr key{Expr::Kind::Const, {}, 0, nullptr, nullptr, -1};  // a position's: none
    std::int64_t position = 0;
    if (sb->node->node_case == PG_QUERY__NODE__NODE_A_CONST) {
      // An integer is a select-list position, as PostgreSQL reads it; any
      // other constant orders by nothing it accepts.
      auto l = literal(sb->node);
      auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), position);
      if (l.kind != Literal::Kind::Integer || ec != std::errc{} || end != l.text.data() + l.text.size()) {
        reject("non-integer constant in ORDER BY", l.pos);
      }
      if (position < 1) reject("ORDER BY position " + l.text + " is not in select list", l.pos, kInvalidColumnReference);
      key.pos = l.pos;
    } else {
      key = expr(sb->node);
      // PostgreSQL orders by the whole row; the layer has no row values.
      if (key.kind == Expr::Kind::Star) reject("ORDER BY a whole row (table.*) is not supported yet", key.pos);
    }
    if (sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_USING) reject("ORDER BY ... USING is not supported", key.pos);
    std::optional<bool> nulls_first;
    if (sb->sortby_nulls == PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_FIRST) nulls_first = true;
    if (sb->sortby_nulls == PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_LAST) nulls_first = false;
    auto at = key.pos;
    out.push_back({std::move(key), position, sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_DESC, at, nulls_first});
  }
  return out;
}

// An INTO in a set operation's arms. PostgreSQL takes the leftmost arm's as
// the statement's (a gap here: refused, 0A000) and refuses any other.
void refuse_arm_into(const PgQuery__SelectStmt* s, bool leftmost, std::int32_t pos) {
  if (s == nullptr) return;
  if (s->op != PG_QUERY__SET_OPERATION__SETOP_NONE) {
    refuse_arm_into(s->larg, leftmost, pos);
    refuse_arm_into(s->rarg, false, pos);
    return;
  }
  if (s->into_clause == nullptr) return;
  auto at = s->into_clause->rel != nullptr ? s->into_clause->rel->location : pos;
  if (leftmost) {
    reject("SELECT … INTO of a set operation is not supported yet: use CREATE TABLE … AS", at, kFeatureNotSupported);
  }
  reject("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT", at);
}

// UNION, INTERSECT or EXCEPT of the statement's two queries, whose own
// ORDER BY and LIMIT are the combined rows'. Each arm is parsed as any
// SELECT is; a parenthesised one keeps its ORDER BY and LIMIT.
auto set_operation(const PgQuery__SelectStmt* s, std::int32_t pos, bool nested) -> Select {
  if (s->larg == nullptr || s->rarg == nullptr) reject("a set operation needs two queries", pos);
  refuse_arm_into(s->larg, true, pos);
  refuse_arm_into(s->rarg, false, pos);
  if (s->with_clause != nullptr) reject("SELECT clause not supported", pos);
  if (s->limit_offset != nullptr) reject("OFFSET of a set operation is not supported yet", pos);
  if (s->n_locking_clause != 0) {
    reject("FOR UPDATE is not allowed with UNION/INTERSECT/EXCEPT", pos, kFeatureNotSupported);
  }
  using Kind = SetOperation::Kind;
  auto kind = s->op == PG_QUERY__SET_OPERATION__SETOP_UNION       ? Kind::Union
              : s->op == PG_QUERY__SET_OPERATION__SETOP_INTERSECT ? Kind::Intersect
                                                                  : Kind::Except;
  Select sel;
  sel.pos = pos;
  sel.limit = limit(s);
  sel.limit_written = s->limit_count != nullptr;
  sel.order_by = order_items(s, pos);
  sel.set = std::make_shared<const SetOperation>(SetOperation{kind, s->all != 0, select(s->larg, pos, false, nested), select(s->rarg, pos, false, nested)});
  return sel;
}

// The most tables one SELECT reads: the search holds a set of them as the
// bits of a 32-bit mask (docs/planner_design.md, *Joins of several tables*).
// PostgreSQL has no such cap.
constexpr std::size_t kMaxFrom = 32;

// A FROM item that is a table, with its alias.
auto from_item(const PgQuery__RangeVar* rv) -> FromItem {
  FromItem item;
  item.table = from_relation(rv);
  item.alias = table_alias(rv);
  item.written = item.alias.empty() ? item.table.name : item.alias;
  item.column_aliases = table_column_aliases(rv);
  item.pos = rv->location;
  return item;
}

// Each conjunct of an ON, and what it is made of, sees the FROM items from
// `first` on (Compare::on_from).
void scope_on(Compare& c, std::uint32_t first) {
  c.on_from = first;
  for (auto& a : c.args) scope_on(a, first);
}

// The items of a join tree and their ON conjuncts, flattened in the order
// written: `a JOIN b … JOIN c …`, each join's right side a table, or, for an
// inner join, an inner join of tables in parentheses, `a JOIN (b JOIN c ON
// …) ON …`, which joins the same rows in any order. Each ON sees the items of
// its own join, from the first of its left side (the tree's first, `first`
// for a nested tree or one after a comma) to its last. A LEFT or RIGHT join
// in parentheses or of a join in parentheses would not, and is refused, as
// is USING or NATURAL there or after a comma, whose left side is not every
// item before it. A FULL join is only the whole FROM, of two tables:
// `whole` says the tree is.
void join_items(const PgQuery__Node* n, std::int32_t pos, std::vector<FromItem>& items, std::vector<Compare>& on,
                std::uint32_t first, bool nested, bool whole) {
  if (n != nullptr && n->node_case == PG_QUERY__NODE__NODE_RANGE_VAR) {
    items.push_back(from_item(n->range_var));
    return;
  }
  const auto* j = n != nullptr && n->node_case == PG_QUERY__NODE__NODE_JOIN_EXPR ? n->join_expr : nullptr;
  // The join has no position of its own; report the table it joins.
  const auto* r = j != nullptr ? j->rarg : nullptr;
  auto at = r != nullptr && r->node_case == PG_QUERY__NODE__NODE_RANGE_VAR ? r->range_var->location : pos;
  if (j == nullptr) reject("SELECT reads tables", pos);
  auto kind = j->jointype == PG_QUERY__JOIN_TYPE__JOIN_LEFT    ? JoinKind::Left
              : j->jointype == PG_QUERY__JOIN_TYPE__JOIN_RIGHT ? JoinKind::Right
              : j->jointype == PG_QUERY__JOIN_TYPE__JOIN_FULL  ? JoinKind::Full
                                                               : JoinKind::Inner;
  auto table = r != nullptr && r->node_case == PG_QUERY__NODE__NODE_RANGE_VAR;
  auto join = r != nullptr && r->node_case == PG_QUERY__NODE__NODE_JOIN_EXPR;
  // A FULL JOIN is the union of two plans of its two tables (the binder),
  // so it is the whole FROM: never in a chain, in parentheses or after a
  // comma (#712).
  auto two_tables = j->larg != nullptr && j->larg->node_case == PG_QUERY__NODE__NODE_RANGE_VAR && table;
  if (kind == JoinKind::Full && !(whole && two_tables && j->alias == nullptr)) {
    reject("a FULL JOIN that is not the whole FROM of two tables is not supported yet", at);
  }
  if ((j->jointype != PG_QUERY__JOIN_TYPE__JOIN_INNER && kind == JoinKind::Inner) || j->alias != nullptr ||
      j->larg == nullptr || !(table || join)) {
    reject("only an inner, LEFT, RIGHT or FULL join of tables, without an alias, is supported", at);
  }
  if (kind != JoinKind::Inner && (nested || join)) {
    reject("a LEFT or RIGHT join in parentheses, or of a join in parentheses, is not supported yet", at);
  }
  auto merges = j->is_natural || j->n_using_clause != 0;
  if (merges && (first != 0 || join)) {
    reject("USING or NATURAL in a join in parentheses, of one, or after a comma is not supported yet", at);
  }
  join_items(j->larg, pos, items, on, first, nested, false);
  if (join) {
    join_items(r, pos, items, on, static_cast<std::uint32_t>(items.size()), true, false);
  } else {
    auto item = from_item(r->range_var);
    item.kind = kind;
    item.join_from = first;
    item.natural = j->is_natural;
    for (std::size_t k = 0; k < j->n_using_clause; ++k) item.using_columns.push_back(str(j->using_clause[k]));
    if (j->join_using_alias != nullptr) {
      if (j->join_using_alias->n_colnames != 0) reject("a USING alias takes no column list", at);
      item.using_alias = j->join_using_alias->aliasname;
    }
    items.push_back(std::move(item));
  }
  // The ON is the last item's: of the table joined, or of the join in
  // parentheses, which is inner and so has its ON with the WHERE's.
  auto last = static_cast<std::uint32_t>(items.size() - 1);
  auto written = on.size();
  where(j->quals, on, "ON", items.back().pos, last);
  for (auto i = written; i < on.size(); ++i) scope_on(on[i], first);
}

auto select(const PgQuery__SelectStmt* s, std::int32_t pos, bool into_taken, bool nested) -> Select {
  if (s->op != PG_QUERY__SET_OPERATION__SETOP_NONE) return set_operation(s, pos, nested);
  // No table, or tables joined: FROM a, b, c, or a [INNER] JOIN b ON … JOIN
  // c …, or both, whose ON conjuncts join the WHERE's.
  if (s->n_values_lists != 0) reject("VALUES is supported only in INSERT", pos);
  std::vector<FromItem> items;
  std::vector<Compare> on;
  for (std::size_t i = 0; i < s->n_from_clause; ++i) {
    const auto* from = s->from_clause[i];
    if (from->node_case == PG_QUERY__NODE__NODE_JOIN_EXPR) {
      join_items(from, pos, items, on, static_cast<std::uint32_t>(items.size()), false, s->n_from_clause == 1);
      continue;
    }
    if (from->node_case != PG_QUERY__NODE__NODE_RANGE_VAR) reject("SELECT reads tables", pos);
    items.push_back(from_item(from->range_var));
  }
  if (items.size() > kMaxFrom) {
    reject("joins of more than " + std::to_string(kMaxFrom) + " tables are not supported", items[kMaxFrom].pos,
           kFeatureNotSupported);
  }
  // DISTINCT is a list of one empty node; DISTINCT ON a list of its expressions.
  auto distinct = s->n_distinct_clause == 1 &&
                  (s->distinct_clause[0] == nullptr || s->distinct_clause[0]->node_case == PG_QUERY__NODE__NODE__NOT_SET);
  if (s->n_distinct_clause != 0 && !distinct) reject("DISTINCT ON is not supported", pos);
  if (s->group_distinct || s->n_locking_clause > 1 || s->with_clause != nullptr ||
      s->n_window_clause != 0 || (s->into_clause != nullptr && !into_taken)) {
    reject("SELECT clause not supported", pos);
  }
  Select sel;
  sel.from = std::move(items);
  sel.limit = limit(s);
  sel.offset = offset(s);
  sel.limit_written = s->limit_count != nullptr || s->limit_offset != nullptr;
  sel.pos = pos;
  sel.distinct = distinct;
  if (s->n_locking_clause == 1) {
    // FOR UPDATE alone: never NOWAIT or SKIP LOCKED, which only mean
    // something to a locker, nor a weaker lock or OF a table list.
    const auto* lc = s->locking_clause[0]->locking_clause;
    if (lc->strength != PG_QUERY__LOCK_CLAUSE_STRENGTH__LCS_FORUPDATE) reject("only FOR UPDATE is supported", pos);
    if (lc->wait_policy != PG_QUERY__LOCK_WAIT_POLICY__LockWaitBlock) {
      reject("NOWAIT and SKIP LOCKED are not supported", pos);
    }
    // OF t, u: each must be a FROM item. The lock covers every table the query
    // reads, which includes the named ones (docs/cursor_design.md).
    for (std::size_t k = 0; k < lc->n_locked_rels; ++k) {
      const auto* rv = lc->locked_rels[k]->range_var;
      if (!empty(rv->schemaname)) reject("schema-qualified names are not supported", rv->location);
      auto named = std::ranges::any_of(sel.from, [&](const FromItem& f) { return f.name() == rv->relname; });
      if (!named) {
        reject(std::string{"relation \""} + rv->relname + "\" in FOR UPDATE clause not found in FROM clause",
               rv->location, kUndefinedTable);
      }
    }
    sel.for_update = true;
  }
  sel.order_by = order_items(s, pos);
  for (std::size_t i = 0; i < s->n_target_list; ++i) {
    const auto* rt = s->target_list[i]->res_target;
    sel.targets.push_back(expr(rt->val));
    if (!empty(rt->name)) sel.targets.back().alias = rt->name;
  }
  sel.where = std::move(on);
  where(s->where_clause, sel.where);
  // GROUP BY of columns: no expressions, grouping sets, ROLLUP or CUBE.
  for (std::size_t i = 0; i < s->n_group_clause; ++i) {
    const auto* g = s->group_clause[i];
    if (g->node_case != PG_QUERY__NODE__NODE_COLUMN_REF) reject("GROUP BY takes columns", pos);
    sel.group_by.push_back(expr(g));
  }
  where(s->having_clause, sel.having, "HAVING", pos);
  resolve_output_names(sel);
  resolve_positions(sel);
  resolve_aliases(sel, nested);
  return sel;
}

// The cursor of a WHERE that is CURRENT OF name alone; PostgreSQL's grammar
// takes it as the whole clause or not at all (WHERE x AND CURRENT OF c is a
// syntax error there, and here too: predicate() has no such node).
auto current_of(const PgQuery__Node* where_clause) -> std::optional<std::string> {
  if (where_clause == nullptr || where_clause->node_case != PG_QUERY__NODE__NODE_CURRENT_OF_EXPR) return std::nullopt;
  const auto* c = where_clause->current_of_expr;
  if (empty(c->cursor_name)) reject("WHERE CURRENT OF takes a cursor name, not a parameter", -1);
  return std::string{c->cursor_name};
}

auto update(const PgQuery__UpdateStmt* s, std::int32_t pos) -> Update {
  if (s->n_from_clause != 0 || s->with_clause != nullptr) {
    reject("UPDATE option not supported", s->relation->location);
  }
  Update u{relation(s->relation), {}, {}, pos, {}};
  for (std::size_t i = 0; i < s->n_target_list; ++i) {
    const auto* rt = s->target_list[i]->res_target;
    if (rt->n_indirection != 0) reject("subscripted assignment is not supported", rt->location);
    u.set.push_back(Assignment{rt->name, expr(rt->val), rt->location});
  }
  if (auto cursor = current_of(s->where_clause)) {
    u.current_of = std::move(*cursor);
  } else {
    where(s->where_clause, u.where);
  }
  u.returning = returning_list(s->returning_list, s->n_returning_list);
  const auto target = target_item(u.table);
  std::span<const FromItem> items{&target, 1};
  for (auto& a : u.set) resolve_schema_qualifier(a.value, items, false);
  for (auto& e : u.returning) resolve_schema_qualifier(e, items, false);
  for (auto& c : u.where) resolve_schema_qualifier(c, items, false);
  return u;
}

auto del(const PgQuery__DeleteStmt* s, std::int32_t pos) -> Delete {
  if (s->n_using_clause != 0 || s->with_clause != nullptr) {
    reject("DELETE option not supported", s->relation->location);
  }
  Delete d{relation(s->relation), {}, pos, {}};
  if (auto cursor = current_of(s->where_clause)) {
    d.current_of = std::move(*cursor);
  } else {
    where(s->where_clause, d.where);
  }
  d.returning = returning_list(s->returning_list, s->n_returning_list);
  const auto target = target_item(d.table);
  std::span<const FromItem> items{&target, 1};
  for (auto& e : d.returning) resolve_schema_qualifier(e, items, false);
  for (auto& c : d.where) resolve_schema_qualifier(c, items, false);
  return d;
}

// The table an INTO clause names, of CREATE TABLE AS or SELECT … INTO: a
// permanent table with no options. TEMP, UNLOGGED, WITH (…), USING and
// TABLESPACE are not supported (0A000); ON COMMIT is PostgreSQL's error
// on a table that is not temporary.
auto into_target(const PgQuery__IntoClause* into, CreateTableAs& out) -> void {
  if (into == nullptr || into->rel == nullptr) reject("expected a table", out.pos);
  const auto* rv = into->rel;
  out.table = relation(rv);
  if (!empty(rv->relpersistence) && rv->relpersistence[0] == 't') {
    reject("temporary tables are not supported", rv->location, kFeatureNotSupported);
  }
  if (!empty(rv->relpersistence) && rv->relpersistence[0] == 'u') {
    reject("unlogged tables are not supported", rv->location, kFeatureNotSupported);
  }
  if (into->on_commit != PG_QUERY__ON_COMMIT_ACTION__ONCOMMIT_NOOP &&
      into->on_commit != PG_QUERY__ON_COMMIT_ACTION__ON_COMMIT_ACTION_UNDEFINED) {
    reject("ON COMMIT can only be used on temporary tables", rv->location, kInvalidTableDefinition);
  }
  if (into->n_options != 0) reject("storage parameters (WITH …) are not supported", rv->location, kFeatureNotSupported);
  if (!empty(into->access_method)) reject("USING an access method is not supported", rv->location, kFeatureNotSupported);
  if (!empty(into->table_space_name)) reject("TABLESPACE is not supported", rv->location, kFeatureNotSupported);
  for (std::size_t i = 0; i < into->n_col_names; ++i) out.columns.push_back(str(into->col_names[i]));
  out.with_data = !into->skip_data;
}

auto create_table_as(const PgQuery__CreateTableAsStmt* s, std::int32_t pos) -> CreateTableAs {
  CreateTableAs out;
  out.pos = pos;
  if (s->objtype == PG_QUERY__OBJECT_TYPE__OBJECT_MATVIEW) {
    reject("materialized views are not supported", pos, kFeatureNotSupported);
  }
  into_target(s->into, out);
  out.if_not_exists = s->if_not_exists;
  if (s->query != nullptr && s->query->node_case == PG_QUERY__NODE__NODE_EXECUTE_STMT) {
    reject("CREATE TABLE AS EXECUTE is not supported", pos, kFeatureNotSupported);
  }
  if (s->query == nullptr || s->query->node_case != PG_QUERY__NODE__NODE_SELECT_STMT) {
    reject("CREATE TABLE AS takes a SELECT", pos);
  }
  out.query = select(s->query->select_stmt, pos);
  return out;
}

// SELECT … INTO name: CREATE TABLE name AS the SELECT without its INTO.
auto select_into(const PgQuery__SelectStmt* s, std::int32_t pos) -> CreateTableAs {
  CreateTableAs out;
  out.form = CreateTableAs::Form::SelectInto;
  out.pos = pos;
  into_target(s->into_clause, out);
  out.query = select(s, pos, true);
  return out;
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

// Owns libpg_query's scan of a text, its tokens; `tree` is null when the
// scanner refused it.
struct TokenScan {
  PgQueryScanResult raw{};
  PgQuery__ScanResult* tree{nullptr};
  explicit TokenScan(const std::string& text) : raw{pg_query_scan(text.c_str())} {
    if (raw.error == nullptr) {
      tree = pg_query__scan_result__unpack(nullptr, raw.pbuf.len, reinterpret_cast<const std::uint8_t*>(raw.pbuf.data));
    }
  }
  TokenScan(const TokenScan&) = delete;
  TokenScan& operator=(const TokenScan&) = delete;
  ~TokenScan() {
    if (tree != nullptr) pg_query__scan_result__free_unpacked(tree, nullptr);
    pg_query_free_scan_result(raw);
  }
};

// The byte range of each of CREATE SCHEMA's elements in its text
// (docs/catalog_design.md, *Schema DDL batches and transactional DDL*): from
// a CREATE or GRANT outside parentheses, past the statement's own CREATE, to
// the last token before the next one (or the statement's end), comments and
// a closing semicolon left out. A GRANT after WITH (WITH GRANT OPTION) and a
// CREATE or GRANT after GRANT or a comma (GRANT CREATE ON …, a privilege)
// start nothing. Such a token elsewhere inside an element (SELECT a AS
// create) cuts it short; a view's cut is checked against its tree. A range
// the scan cannot give is empty.
auto element_ranges(const std::string& text, std::size_t count) -> std::vector<std::pair<std::size_t, std::size_t>> {
  std::vector<std::pair<std::size_t, std::size_t>> out(count, {0, 0});
  TokenScan scan{text};
  if (scan.tree == nullptr) return out;
  const std::span<PgQuery__ScanToken* const> tokens{scan.tree->tokens, scan.tree->n_tokens};
  auto ignored = [](const PgQuery__ScanToken* t) {
    return t->token == PG_QUERY__TOKEN__SQL_COMMENT || t->token == PG_QUERY__TOKEN__C_COMMENT ||
           t->token == PG_QUERY__TOKEN__ASCII_59;
  };
  std::vector<std::size_t> starts;
  auto depth = 0;
  auto own = true;  // the statement's CREATE SCHEMA
  auto previous = PG_QUERY__TOKEN__NUL;  // the last token not ignored
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto token = tokens[i]->token;
    const auto before = std::exchange(previous, ignored(tokens[i]) ? previous : token);
    if (token == PG_QUERY__TOKEN__ASCII_40) ++depth;
    if (token == PG_QUERY__TOKEN__ASCII_41) --depth;
    if (depth != 0 || (token != PG_QUERY__TOKEN__CREATE && token != PG_QUERY__TOKEN__GRANT)) continue;
    if (before == PG_QUERY__TOKEN__WITH || before == PG_QUERY__TOKEN__GRANT || before == PG_QUERY__TOKEN__ASCII_44) continue;
    if (std::exchange(own, false)) continue;
    starts.push_back(i);
  }
  for (std::size_t k = 0; k < count && k < starts.size(); ++k) {
    auto last = k + 1 < starts.size() ? starts[k + 1] : tokens.size();
    while (last > starts[k] + 1 && ignored(tokens[last - 1])) --last;
    out[k] = {static_cast<std::size_t>(tokens[starts[k]]->start), static_cast<std::size_t>(tokens[last - 1]->end)};
  }
  return out;
}

// The bytes of a protobuf-c message's member at `offset`: protobuf-c's
// descriptors name members by offset into the C struct, the one way to walk
// a tree of any message type.
template <typename T>
auto member_at(ProtobufCMessage* m, unsigned offset) -> T& {
  return *reinterpret_cast<T*>(reinterpret_cast<std::byte*>(m) + offset);
}

// Every `location` in a parse tree set to 0, so two trees of one statement
// read alike wherever its text sat.
void zero_locations(ProtobufCMessage* m) {
  const auto* d = m->descriptor;
  for (const auto& f : std::span{d->fields, d->n_fields}) {
    if (f.type == PROTOBUF_C_TYPE_INT32 && f.label != PROTOBUF_C_LABEL_REPEATED && std::string_view{f.name} == "location") {
      member_at<std::int32_t>(m, f.offset) = 0;
      continue;
    }
    if (f.type != PROTOBUF_C_TYPE_MESSAGE) continue;
    if (f.label == PROTOBUF_C_LABEL_REPEATED) {
      const std::span items{member_at<ProtobufCMessage**>(m, f.offset), member_at<std::size_t>(m, f.quantifier_offset)};
      for (auto* item : items) {
        if (item != nullptr) zero_locations(item);
      }
      continue;
    }
    if ((f.flags & PROTOBUF_C_FIELD_FLAG_ONEOF) != 0 && member_at<std::uint32_t>(m, f.quantifier_offset) != f.id) continue;
    if (auto* sub = member_at<ProtobufCMessage*>(m, f.offset); sub != nullptr) zero_locations(sub);
  }
}

// Every message of a parse tree, `m` first, given to `visit`.
void each_message(ProtobufCMessage* m, const std::function<void(ProtobufCMessage*)>& visit) {
  visit(m);
  const auto* d = m->descriptor;
  for (const auto& f : std::span{d->fields, d->n_fields}) {
    if (f.type != PROTOBUF_C_TYPE_MESSAGE) continue;
    if (f.label == PROTOBUF_C_LABEL_REPEATED) {
      const std::span items{member_at<ProtobufCMessage**>(m, f.offset), member_at<std::size_t>(m, f.quantifier_offset)};
      for (auto* item : items) {
        if (item != nullptr) each_message(item, visit);
      }
      continue;
    }
    if ((f.flags & PROTOBUF_C_FIELD_FLAG_ONEOF) != 0 && member_at<std::uint32_t>(m, f.quantifier_offset) != f.id) continue;
    if (auto* sub = member_at<ProtobufCMessage*>(m, f.offset); sub != nullptr) each_message(sub, visit);
  }
}

// A view element's column s.t.c, s the new schema or public, where its
// query reads t unqualified and unaliased: which schema's t that is is
// known only when the element binds, after the parser has resolved the
// qualifier against public alone, so it is refused (0A000) rather than read
// against the wrong item or refused with a wrong 42P01
// (docs/catalog_design.md, *Schema semantics*).
void refuse_path_qualified_columns(PgQuery__Node* view, const std::string& schema, std::int32_t pos) {
  std::vector<std::string> unqualified;
  std::vector<std::pair<std::string, std::string>> qualified;
  each_message(&view->base, [&](ProtobufCMessage* m) {
    if (m->descriptor == &pg_query__range_var__descriptor) {
      const auto* rv = reinterpret_cast<const PgQuery__RangeVar*>(m);
      if (empty(rv->schemaname) && empty(rv->catalogname) && rv->alias == nullptr) unqualified.emplace_back(rv->relname);
    } else if (m->descriptor == &pg_query__column_ref__descriptor) {
      const auto* c = reinterpret_cast<const PgQuery__ColumnRef*>(m);
      if (c->n_fields == 3 && c->fields[0]->node_case == PG_QUERY__NODE__NODE_STRING &&
          c->fields[1]->node_case == PG_QUERY__NODE__NODE_STRING) {
        qualified.emplace_back(c->fields[0]->string->sval, c->fields[1]->string->sval);
      }
    }
  });
  for (const auto& [s, t] : qualified) {
    if ((s == schema || s == "public") && std::ranges::contains(unqualified, t)) {
      reject("a schema-qualified column of a table a view element names unqualified is not supported yet", pos,
             kFeatureNotSupported);
    }
  }
}

// A node's tree, locations aside, as bytes to compare.
auto tree_bytes(PgQuery__Node* n) -> std::string {
  zero_locations(&n->base);
  std::string out(pg_query__node__get_packed_size(n), '\0');
  (void)pg_query__node__pack(n, reinterpret_cast<std::uint8_t*>(out.data()));
  return out;
}

// A CREATE VIEW element's own text: its range of the statement, kept if it
// parses alone to one CREATE VIEW of the element's tree, locations aside,
// so the view stores an ordinary CREATE VIEW of the client's text and
// nothing is deparsed (docs/catalog_design.md, *Schema DDL batches and
// transactional DDL*). Called once the element is read: it zeroes the
// element's locations.
auto view_element_text(PgQuery__Node* element, const std::string& text, std::pair<std::size_t, std::size_t> range,
                       std::int32_t pos) -> std::string {
  auto slice = text.substr(range.first, range.second - range.first);
  ParseResources res;
  res.raw = pg_query_parse_protobuf(slice.c_str());
  if (res.raw.error == nullptr) {
    res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                              reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  }
  if (res.tree == nullptr || res.tree->n_stmts != 1 ||
      res.tree->stmts[0]->stmt->node_case != PG_QUERY__NODE__NODE_VIEW_STMT ||
      tree_bytes(res.tree->stmts[0]->stmt) != tree_bytes(element)) {
    reject("CREATE VIEW as a schema element whose text cannot be cut out of the statement is not supported", pos,
           kFeatureNotSupported);
  }
  return slice;
}

// Whether a RangeVar names a temporary relation.
auto temporary(const PgQuery__RangeVar* rv) -> bool { return !empty(rv->relpersistence) && rv->relpersistence[0] == 't'; }

// One element of CREATE SCHEMA `schema`, read as its statement alone would
// be, then placed in the schema (docs/catalog_design.md, *Schema
// semantics*): a name it creates, and an index's table, unqualified is the
// schema's; qualified by another is 42P15, raised once the schema exists.
// A temporary relation is 42P16, raised when the element runs, as
// PostgreSQL 17 raises both.
auto schema_element(PgQuery__Node* n, const std::string& schema, const std::string& text,
                    std::pair<std::size_t, std::size_t> range, std::int32_t pos) -> SchemaElement {
  if (range.second > range.first) pos = static_cast<std::int32_t>(range.first);
  SchemaElement out{CreateSequence{}, std::nullopt, std::nullopt};
  auto place = [&](ObjectName& name, std::int32_t at) {
    if (name.schema.empty()) {
      name.schema = schema;
    } else if (name.schema != schema && !out.misplaced) {
      out.misplaced = SqlError{"CREATE specifies a schema (" + name.schema + ") different from the one being created (" +
                                   schema + ")",
                               at, kInvalidSchemaDefinition};
    }
  };
  auto refuse_temporary = [&](const PgQuery__RangeVar* rv) {
    if (temporary(rv)) {
      out.refusal = SqlError{"cannot create temporary relation in non-temporary schema", rv->location, kInvalidTableDefinition};
    }
    return out.refusal.has_value();
  };
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_SEQ_STMT) {
    const auto* rv = n->create_seq_stmt->sequence;
    auto seq = refuse_temporary(rv) ? CreateSequence{from_relation(rv), false, pos} : create_sequence(n->create_seq_stmt, pos);
    place(seq.sequence, rv->location);
    out.statement = std::move(seq);
  } else if (n->node_case == PG_QUERY__NODE__NODE_CREATE_STMT) {
    const auto* rv = n->create_stmt->relation;
    auto table = refuse_temporary(rv) ? CreateTable{from_relation(rv), {}, {}, pos} : create_table(n->create_stmt, pos, text);
    place(table.table, rv->location);
    out.statement = std::move(table);
  } else if (n->node_case == PG_QUERY__NODE__NODE_VIEW_STMT) {
    const auto* rv = n->view_stmt->view;
    CreateView view{from_relation(rv), {}, {}, pos, {}};
    if (!refuse_temporary(rv)) {
      refuse_path_qualified_columns(n, schema, pos);
      view = create_view(n->view_stmt, pos);
      view.definition = view_element_text(n, text, range, pos);
    }
    place(view.view, rv->location);
    out.statement = std::move(view);
  } else if (n->node_case == PG_QUERY__NODE__NODE_INDEX_STMT) {
    auto index = create_index(n->index_stmt, pos);
    place(index.table, n->index_stmt->relation->location);
    out.statement = std::move(index);
  } else if (n->node_case == PG_QUERY__NODE__NODE_GRANT_STMT) {
    // The grammar takes only GRANT: REVOKE is its syntax error.
    out.statement = grant(n->grant_stmt, pos);
  } else if (n->node_case == PG_QUERY__NODE__NODE_CREATE_TRIG_STMT) {
    reject("triggers are not supported", pos, kFeatureNotSupported);
  } else {
    reject("CREATE SCHEMA element not supported", pos);
  }
  return out;
}

// CREATE SCHEMA [IF NOT EXISTS] name [AUTHORIZATION role] [element …], or
// CREATE SCHEMA AUTHORIZATION role [element …]. PUBLIC is no role (42704),
// as in PostgreSQL 17. IF NOT EXISTS with elements is the grammar's 0A000
// (parse()).
auto create_schema(const PgQuery__CreateSchemaStmt* s, std::int32_t pos, const std::string& text) -> CreateSchema {
  CreateSchema out{empty(s->schemaname) ? "" : s->schemaname, std::nullopt, s->if_not_exists != 0, pos, {}};
  if (const auto* r = s->authrole) {
    switch (r->roletype) {
      case PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_CSTRING: out.authorization = r->rolename; break;
      case PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_CURRENT_ROLE:
      case PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_CURRENT_USER:
      case PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_SESSION_USER: out.authorization = ""; break;
      case PG_QUERY__ROLE_SPEC_TYPE__ROLESPEC_PUBLIC: reject("role \"public\" does not exist", r->location, kUndefinedObject);
      case PG_QUERY__ROLE_SPEC_TYPE__ROLE_SPEC_TYPE_UNDEFINED:
      case _PG_QUERY__ROLE_SPEC_TYPE_IS_INT_SIZE: reject("expected a role", r->location);
    }
  }
  // CREATE SCHEMA AUTHORIZATION CURRENT_USER names the schema after the
  // session's user, which the bootstrap role every session is has no name for.
  if (out.name.empty() && out.authorization && out.authorization->empty()) {
    reject("CREATE SCHEMA AUTHORIZATION CURRENT_USER without a schema name is not supported", pos, kFeatureNotSupported);
  }
  if (out.name.empty()) out.name = *out.authorization;
  const auto ranges = element_ranges(text, s->n_schema_elts);
  for (std::size_t i = 0; i < s->n_schema_elts; ++i) {
    out.elements.push_back(schema_element(s->schema_elts[i], out.name, text, ranges[i], pos));
  }
  return out;
}

// PostgreSQL's CURSOR_OPT_* bits in DeclareCursorStmt::options.
constexpr std::int32_t kCursorBinary = 0x1;
constexpr std::int32_t kCursorScroll = 0x2;
constexpr std::int32_t kCursorNoScroll = 0x4;
constexpr std::int32_t kCursorHold = 0x20;

// The offset where a DECLARE's query begins: past the FOR that follows
// CURSOR [WITH | WITHOUT HOLD]. The parse tree has no location for it, so the
// scanner's tokens are read. The statement parsed, so the tokens are there.
auto declare_query_start(const std::string& text) -> std::size_t {
  TokenScan scan{text};
  if (scan.tree == nullptr) reject("unreadable DECLARE", -1);
  // Tokens 0 and 1 are DECLARE and the name, which may itself be spelled cursor.
  auto n = scan.tree->n_tokens;
  for (std::size_t i = 2; i < n; ++i) {
    if (scan.tree->tokens[i]->token != PG_QUERY__TOKEN__CURSOR) continue;
    auto j = i + 1;
    if (j < n && (scan.tree->tokens[j]->token == PG_QUERY__TOKEN__WITH || scan.tree->tokens[j]->token == PG_QUERY__TOKEN__WITHOUT)) j += 2;
    if (j >= n || scan.tree->tokens[j]->token != PG_QUERY__TOKEN__FOR) break;
    return static_cast<std::size_t>(scan.tree->tokens[j]->end);
  }
  reject("unreadable DECLARE", -1);
}

}  // namespace

auto parse_cursor(std::string_view sql) -> std::optional<CursorStatement> {
  std::string text{sql};
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1);
  }
  res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                            reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  if (res.tree == nullptr) reject("unreadable parse tree", -1);
  if (res.tree->n_stmts != 1) reject("expected exactly one statement", -1);
  const auto* n = res.tree->stmts[0]->stmt;
  if (n->node_case == PG_QUERY__NODE__NODE_DECLARE_CURSOR_STMT) {
    const auto* d = n->declare_cursor_stmt;
    DeclareCursor out;
    out.name = d->portalname;
    out.binary = (d->options & kCursorBinary) != 0;
    out.hold = (d->options & kCursorHold) != 0;
    if ((d->options & kCursorScroll) != 0) out.scroll = DeclareCursor::Scroll::Scroll;
    if ((d->options & kCursorNoScroll) != 0) out.scroll = DeclareCursor::Scroll::NoScroll;
    const auto* q = d->query;
    out.for_update = q != nullptr && q->node_case == PG_QUERY__NODE__NODE_SELECT_STMT && q->select_stmt->n_locking_clause != 0;
    auto start = declare_query_start(text);
    out.query = std::string(start, ' ') + text.substr(start);
    return out;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_FETCH_STMT) {
    const auto* f = n->fetch_stmt;
    FetchCursor out;
    out.name = f->portalname;
    out.move = f->ismove != 0;
    out.count = f->how_many;
    if (f->direction == PG_QUERY__FETCH_DIRECTION__FETCH_BACKWARD) out.direction = FetchCursor::Direction::Backward;
    if (f->direction == PG_QUERY__FETCH_DIRECTION__FETCH_ABSOLUTE) out.direction = FetchCursor::Direction::Absolute;
    if (f->direction == PG_QUERY__FETCH_DIRECTION__FETCH_RELATIVE) out.direction = FetchCursor::Direction::Relative;
    return out;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_CLOSE_PORTAL_STMT) {
    CloseCursor out;
    out.name = n->close_portal_stmt->portalname;
    out.all = out.name.empty();
    return out;
  }
  return std::nullopt;
}

auto parse_call(std::string_view sql) -> std::optional<Call> {
  std::string text{sql};
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1);
  }
  res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                            reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  if (res.tree == nullptr) reject("unreadable parse tree", -1);
  if (res.tree->n_stmts != 1) return std::nullopt;
  const auto* n = res.tree->stmts[0]->stmt;
  if (n->node_case != PG_QUERY__NODE__NODE_CALL_STMT) return std::nullopt;
  const auto* f = n->call_stmt->funccall;
  auto pos = res.tree->stmts[0]->stmt_location;
  if (f == nullptr) reject("expected a procedure", pos);
  if (f->func_variadic) reject("VARIADIC is not supported", f->location, kFeatureNotSupported);
  Call out{dotted_name(f->funcname, f->n_funcname, f->location), {}, pos};
  for (std::size_t i = 0; i < f->n_args; ++i) {
    if (f->args[i]->node_case == PG_QUERY__NODE__NODE_NAMED_ARG_EXPR) {
      reject("named arguments are not supported", f->location, kFeatureNotSupported);
    }
    out.args.push_back(expr(f->args[i]));
  }
  return out;
}

auto parse_savepoint(std::string_view sql) -> std::optional<SavepointStatement> {
  std::string text{sql};
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1);
  }
  res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                            reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  if (res.tree == nullptr) reject("unreadable parse tree", -1);
  if (res.tree->n_stmts != 1) return std::nullopt;
  const auto* n = res.tree->stmts[0]->stmt;
  if (n->node_case != PG_QUERY__NODE__NODE_TRANSACTION_STMT) return std::nullopt;
  const auto* t = n->transaction_stmt;
  using Kind = SavepointStatement::Kind;
  if (t->kind == PG_QUERY__TRANSACTION_STMT_KIND__TRANS_STMT_SAVEPOINT) return SavepointStatement{Kind::Savepoint, t->savepoint_name};
  if (t->kind == PG_QUERY__TRANSACTION_STMT_KIND__TRANS_STMT_ROLLBACK_TO) return SavepointStatement{Kind::RollbackTo, t->savepoint_name};
  if (t->kind == PG_QUERY__TRANSACTION_STMT_KIND__TRANS_STMT_RELEASE) return SavepointStatement{Kind::Release, t->savepoint_name};
  return std::nullopt;
}

auto parse_variable(std::string_view sql) -> std::optional<VariableStatement> {
  std::string text{sql};
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1);
  }
  res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                            reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  if (res.tree == nullptr) reject("unreadable parse tree", -1);
  if (res.tree->n_stmts != 1) return std::nullopt;
  const auto* n = res.tree->stmts[0]->stmt;
  using Kind = VariableStatement::Kind;
  if (n->node_case == PG_QUERY__NODE__NODE_VARIABLE_SHOW_STMT) {
    return VariableStatement{Kind::Show, false, n->variable_show_stmt->name, {}};
  }
  if (n->node_case != PG_QUERY__NODE__NODE_VARIABLE_SET_STMT) return std::nullopt;
  const auto* v = n->variable_set_stmt;
  VariableStatement out{Kind::Other, v->is_local != 0, v->name, {}};
  switch (v->kind) {
    case PG_QUERY__VARIABLE_SET_KIND__VAR_SET_VALUE: {
      // Each value is a constant: a string (an identifier, TRUE, FALSE and
      // ON are read as one) or a number; anything else is a form not read.
      for (std::size_t i = 0; i < v->n_args; ++i) {
        const auto* a = v->args[i];
        if (a->node_case != PG_QUERY__NODE__NODE_A_CONST || a->a_const->isnull) return out;
        const auto* c = a->a_const;
        switch (c->val_case) {
          case PG_QUERY__A__CONST__VAL_IVAL: out.values.push_back(std::to_string(c->ival->ival)); break;
          case PG_QUERY__A__CONST__VAL_FVAL: out.values.emplace_back(c->fval->fval); break;
          case PG_QUERY__A__CONST__VAL_SVAL: out.values.emplace_back(c->sval->sval); break;
          case PG_QUERY__A__CONST__VAL_BOOLVAL: out.values.emplace_back(c->boolval->boolval != 0 ? "true" : "false"); break;
          case PG_QUERY__A__CONST__VAL_BSVAL:
          case PG_QUERY__A__CONST__VAL__NOT_SET:
          case _PG_QUERY__A__CONST__VAL__CASE_IS_INT_SIZE: return out;
        }
      }
      out.kind = Kind::Set;
      return out;
    }
    case PG_QUERY__VARIABLE_SET_KIND__VAR_SET_DEFAULT: out.kind = Kind::Default; return out;
    case PG_QUERY__VARIABLE_SET_KIND__VAR_RESET: out.kind = Kind::Reset; return out;
    case PG_QUERY__VARIABLE_SET_KIND__VAR_RESET_ALL: out.kind = Kind::ResetAll; return out;
    case PG_QUERY__VARIABLE_SET_KIND__VAR_SET_CURRENT:
    case PG_QUERY__VARIABLE_SET_KIND__VAR_SET_MULTI:
    case PG_QUERY__VARIABLE_SET_KIND__VARIABLE_SET_KIND_UNDEFINED:
    case _PG_QUERY__VARIABLE_SET_KIND_IS_INT_SIZE: return out;
  }
  return out;
}

void check_syntax(std::string_view sql) {
  std::string text{sql};
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1);
  }
}

auto parse(std::string_view sql) -> Statement {
  std::string text{sql};  // libpg_query takes a NUL-terminated string
  ParseResources res;
  res.raw = pg_query_parse_protobuf(text.c_str());
  if (res.raw.error != nullptr) {
    // PostgreSQL's grammar raises this one as 0A000, not as a syntax error.
    auto state = std::string_view{res.raw.error->message} == "CREATE SCHEMA IF NOT EXISTS cannot include schema elements"
                     ? kFeatureNotSupported
                     : kSyntaxError;
    reject(res.raw.error->message, res.raw.error->cursorpos > 0 ? res.raw.error->cursorpos - 1 : -1, state);
  }
  res.tree = pg_query__parse_result__unpack(nullptr, res.raw.parse_tree.len,
                                            reinterpret_cast<const std::uint8_t*>(res.raw.parse_tree.data));
  if (res.tree == nullptr) reject("unreadable parse tree", -1);
  if (res.tree->n_stmts != 1) reject("expected exactly one statement", -1);
  const auto* raw = res.tree->stmts[0];
  const auto* n = raw->stmt;
  auto pos = raw->stmt_location;
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_STMT) return create_table(n->create_stmt, pos, sql);
  if (n->node_case == PG_QUERY__NODE__NODE_INDEX_STMT) return create_index(n->index_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DROP_STMT) return drop(n->drop_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_ALTER_TABLE_STMT) return alter_table(n->alter_table_stmt, pos, sql);
  if (n->node_case == PG_QUERY__NODE__NODE_VIEW_STMT) {
    auto view = create_view(n->view_stmt, pos);
    view.definition = std::string{sql};
    return view;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_FUNCTION_STMT) {
    auto proc = create_procedure(n->create_function_stmt, pos, text);
    proc.definition = std::string{sql};
    return proc;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_CALL_STMT) {
    reject("CALL runs as a simple Query, not as a prepared statement", pos, kFeatureNotSupported);
  }
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_ROLE_STMT) return create_role(n->create_role_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_SCHEMA_STMT) return create_schema(n->create_schema_stmt, pos, text);
  if (n->node_case == PG_QUERY__NODE__NODE_DROP_ROLE_STMT) return drop_role(n->drop_role_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_GRANT_STMT) return grant(n->grant_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_DOMAIN_STMT) return create_domain(n->create_domain_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_SEQ_STMT) return create_sequence(n->create_seq_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DEFINE_STMT) return create_type(n->define_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_INSERT_STMT) return insert(n->insert_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_CREATE_TABLE_AS_STMT) return create_table_as(n->create_table_as_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_SELECT_STMT) {
    if (n->select_stmt->into_clause != nullptr) return select_into(n->select_stmt, pos);
    return select(n->select_stmt, pos);
  }
  if (n->node_case == PG_QUERY__NODE__NODE_UPDATE_STMT) return update(n->update_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DELETE_STMT) return del(n->delete_stmt, pos);
  reject("statement not supported", pos);
}

}  // namespace bytecask::sql::ast
