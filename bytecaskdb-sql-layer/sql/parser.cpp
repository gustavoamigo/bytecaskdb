// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// parser.cpp — parse()'s definition; see parser.cppm. The only unit, with
// the interface's own global module fragment excepted, that sees libpg_query's
// and protobuf-c's headers.

module;
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <pg_query.h>
#include <protobuf/pg_query.pb-c.h>
#pragma clang diagnostic pop

module bytecask.sql.parser;

namespace bytecask::sql::ast {

namespace {

[[noreturn]] void reject(const std::string& what, std::int32_t pos) { throw SqlError{what, pos}; }

// protobuf-c gives an absent string field as "", never as NULL.
auto empty(const char* s) -> bool { return s == nullptr || s[0] == '\0'; }

auto str(const PgQuery__Node* n) -> std::string {
  if (n == nullptr || n->node_case != PG_QUERY__NODE__NODE_STRING) reject("expected a name", -1);
  return n->string->sval;
}

auto relation(const PgQuery__RangeVar* rv) -> std::string {
  if (rv == nullptr) reject("expected a table", -1);
  if (!empty(rv->schemaname)) {
    reject("schema-qualified names are not supported", rv->location);
  }
  if (rv->alias != nullptr) reject("table aliases are not supported", rv->location);
  return rv->relname;
}

auto expr(const PgQuery__Node* n) -> Expr {
  if (n == nullptr) reject("expected an expression", -1);
  // if-chains, not switches: libpg_query's node enum has 268 values.
  if (n->node_case == PG_QUERY__NODE__NODE_COLUMN_REF) {
    const auto* c = n->column_ref;
    if (c->n_fields != 1) reject("qualified column names are not supported", c->location);
    if (c->fields[0]->node_case != PG_QUERY__NODE__NODE_STRING) reject("* is not supported", c->location);
    return Expr{Expr::Kind::Column, str(c->fields[0]), 0, nullptr, nullptr, c->location};
  }
  if (n->node_case == PG_QUERY__NODE__NODE_PARAM_REF) {
    const auto* p = n->param_ref;
    if (p->number < 1) reject("bad parameter", p->location);
    return Expr{Expr::Kind::Param, {}, static_cast<std::uint32_t>(p->number - 1), nullptr, nullptr, p->location};
  }
  if (n->node_case == PG_QUERY__NODE__NODE_A_EXPR) {
    const auto* a = n->a_expr;
    if (a->kind != PG_QUERY__A__EXPR__KIND__AEXPR_OP || a->n_name != 1 || str(a->name[0]) != "-" ||
        a->lexpr == nullptr) {
      reject("only column - expression is supported here", a->location);
    }
    Expr e{Expr::Kind::Sub, {}, 0, nullptr, nullptr, a->location};
    e.lhs = std::make_unique<Expr>(expr(a->lexpr));
    e.rhs = std::make_unique<Expr>(expr(a->rexpr));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_A_CONST) {
    reject("literals are not supported; use parameters", n->a_const->location);
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

// A WHERE clause as a list of conjuncts, each a comparison.
void where(const PgQuery__Node* n, std::vector<Compare>& out) {
  if (n == nullptr) return;
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR) {
    const auto* b = n->bool_expr;
    if (b->boolop != PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR) reject("only AND is supported in WHERE", b->location);
    for (std::size_t i = 0; i < b->n_args; ++i) where(b->args[i], out);
    return;
  }
  if (n->node_case != PG_QUERY__NODE__NODE_A_EXPR) reject("WHERE must compare a column", -1);
  const auto* a = n->a_expr;
  if (a->kind != PG_QUERY__A__EXPR__KIND__AEXPR_OP || a->n_name != 1) {
    reject("WHERE must compare a column", a->location);
  }
  out.push_back(Compare{compare_op(str(a->name[0]), a->location), expr(a->lexpr), expr(a->rexpr), a->location});
}

auto create_table(const PgQuery__CreateStmt* s, std::int32_t pos) -> CreateTable {
  if (s->n_inh_relations != 0 || s->partspec != nullptr || s->of_typename != nullptr) {
    reject("CREATE TABLE option not supported", s->relation->location);
  }
  CreateTable t{relation(s->relation), {}, {}, pos};
  for (std::size_t i = 0; i < s->n_table_elts; ++i) {
    const auto* el = s->table_elts[i];
    if (el->node_case == PG_QUERY__NODE__NODE_CONSTRAINT) {
      const auto* c = el->constraint;
      if (c->contype != PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) reject("table constraint not supported", c->location);
      for (std::size_t k = 0; k < c->n_keys; ++k) t.pk.push_back(str(c->keys[k]));
      continue;
    }
    if (el->node_case != PG_QUERY__NODE__NODE_COLUMN_DEF) reject("table element not supported", -1);
    const auto* cd = el->column_def;
    const auto* tn = cd->type_name;
    if (tn == nullptr || tn->n_names == 0 || tn->n_array_bounds != 0 || tn->n_typmods != 0) {
      reject("column type not supported", cd->location);
    }
    ColumnDefAst col{cd->colname, str(tn->names[tn->n_names - 1]), cd->is_not_null != 0, false, cd->location};
    for (std::size_t k = 0; k < cd->n_constraints; ++k) {
      const auto* c = cd->constraints[k]->constraint;
      if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) {
        col.primary_key = true;
      } else if (c->contype == PG_QUERY__CONSTR_TYPE__CONSTR_NOTNULL) {
        col.not_null = true;
      } else if (c->contype != PG_QUERY__CONSTR_TYPE__CONSTR_NULL) {
        reject("column constraint not supported", c->location);
      }
    }
    t.columns.push_back(std::move(col));
  }
  return t;
}

auto create_index(const PgQuery__IndexStmt* s, std::int32_t pos) -> CreateIndex {
  if (s->unique || s->where_clause != nullptr || s->n_index_including_params != 0 || empty(s->idxname)) {
    reject("only a named, non-unique index on plain columns is supported", s->relation->location);
  }
  CreateIndex ix{s->idxname, relation(s->relation), {}, pos};
  for (std::size_t i = 0; i < s->n_index_params; ++i) {
    const auto* e = s->index_params[i]->index_elem;
    if (empty(e->name) || e->expr != nullptr) reject("index expressions are not supported", s->relation->location);
    if (e->ordering != PG_QUERY__SORT_BY_DIR__SORTBY_DEFAULT) reject("DESC is not supported", s->relation->location);
    ix.columns.emplace_back(e->name);
  }
  return ix;
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

auto select(const PgQuery__SelectStmt* s, std::int32_t pos) -> Select {
  if (s->op != PG_QUERY__SET_OPERATION__SETOP_NONE) reject("set operations are not supported", pos);
  if (s->n_from_clause != 1) reject("SELECT reads exactly one table", pos);
  const auto* from = s->from_clause[0];
  if (from->node_case == PG_QUERY__NODE__NODE_JOIN_EXPR) {
    // The join has no position of its own; report the table it joins.
    const auto* r = from->join_expr->rarg;
    auto at = r != nullptr && r->node_case == PG_QUERY__NODE__NODE_RANGE_VAR ? r->range_var->location : pos;
    reject("JOIN is not supported", at);
  }
  if (from->node_case != PG_QUERY__NODE__NODE_RANGE_VAR) reject("SELECT reads exactly one table", pos);
  if (s->n_distinct_clause != 0 || s->n_group_clause != 0 || s->having_clause != nullptr || s->n_sort_clause != 0 ||
      s->limit_count != nullptr || s->limit_offset != nullptr || s->n_locking_clause != 0 || s->with_clause != nullptr ||
      s->n_window_clause != 0 || s->into_clause != nullptr) {
    reject("SELECT clause not supported", pos);
  }
  Select sel{{}, relation(from->range_var), {}, pos};
  for (std::size_t i = 0; i < s->n_target_list; ++i) {
    const auto* rt = s->target_list[i]->res_target;
    if (!empty(rt->name)) reject("column aliases are not supported", rt->location);
    sel.targets.push_back(expr(rt->val));
  }
  where(s->where_clause, sel.where);
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
  if (n->node_case == PG_QUERY__NODE__NODE_INSERT_STMT) return insert(n->insert_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_SELECT_STMT) return select(n->select_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_UPDATE_STMT) return update(n->update_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DELETE_STMT) return del(n->delete_stmt, pos);
  reject("statement not supported", pos);
}

}  // namespace bytecask::sql::ast
