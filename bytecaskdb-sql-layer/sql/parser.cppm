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

auto expr(const PgQuery__Node* n) -> Expr;
auto literal(const PgQuery__Node* n) -> Literal;

// An operand of + or -: an expression, or an integer literal, which must
// fit BIGINT (PostgreSQL types a larger one as numeric).
auto operand(const PgQuery__Node* n) -> Expr {
  if (n == nullptr || n->node_case != PG_QUERY__NODE__NODE_A_CONST) return expr(n);
  auto l = literal(n);
  std::int64_t v = 0;
  auto [end, ec] = std::from_chars(l.text.data(), l.text.data() + l.text.size(), v);
  if (l.kind != Literal::Kind::Integer || ec != std::errc{} || end != l.text.data() + l.text.size()) {
    reject("only integer literals within BIGINT are supported", l.pos);
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
    if (last->node_case != PG_QUERY__NODE__NODE_STRING) reject("* is not supported", c->location);
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
    if ((op != "+" && op != "-") || a->lexpr == nullptr) reject("only + and - are supported here", a->location);
    Expr e{op == "+" ? Expr::Kind::Add : Expr::Kind::Sub, {}, 0, nullptr, nullptr, a->location};
    e.lhs = std::make_unique<Expr>(operand(a->lexpr));
    e.rhs = std::make_unique<Expr>(operand(a->rexpr));
    return e;
  }
  if (n->node_case == PG_QUERY__NODE__NODE_FUNC_CALL) {
    const auto* f = n->func_call;
    auto name = str(f->funcname[f->n_funcname - 1]);
    auto known = name == "count" || name == "sum" || name == "min";
    if (!known || (f->n_funcname == 2 && str(f->funcname[0]) != "pg_catalog") || f->n_funcname > 2) {
      reject("function " + name + " is not supported", f->location);
    }
    auto refuse = [&] [[noreturn]] {
      reject("only COUNT(*), COUNT([DISTINCT] column), SUM(column) and MIN(column) are supported", f->location);
    };
    if ((f->agg_distinct && name != "count") || f->n_agg_order != 0 || f->agg_filter != nullptr ||
        f->over != nullptr || f->agg_within_group || f->func_variadic) {
      refuse();
    }
    Expr e{Expr::Kind::Aggregate, {}, 0, nullptr, nullptr, f->location};
    e.fn = name == "count" ? Expr::Fn::Count : name == "sum" ? Expr::Fn::Sum : Expr::Fn::Min;
    e.distinct = f->agg_distinct;
    if (f->agg_star && e.fn == Expr::Fn::Count) return e;
    if (f->agg_star || f->n_args != 1 || f->args[0]->node_case != PG_QUERY__NODE__NODE_COLUMN_REF) refuse();
    auto arg = expr(f->args[0]);
    e.column = arg.column;
    e.table = arg.table;
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
void where(const PgQuery__Node* n, std::vector<Compare>& out, const std::string& clause = "WHERE",
           std::int32_t pos = -1) {
  if (n == nullptr) return;
  if (n->node_case == PG_QUERY__NODE__NODE_BOOL_EXPR) {
    const auto* b = n->bool_expr;
    if (b->boolop != PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR) reject("only AND is supported in " + clause, b->location);
    for (std::size_t i = 0; i < b->n_args; ++i) where(b->args[i], out, clause, pos);
    return;
  }
  if (n->node_case != PG_QUERY__NODE__NODE_A_EXPR) reject(clause + " must compare a column", pos);
  const auto* a = n->a_expr;
  if (a->kind != PG_QUERY__A__EXPR__KIND__AEXPR_OP || a->n_name != 1) {
    reject(clause + " must compare a column", a->location);
  }
  out.push_back(Compare{compare_op(str(a->name[0]), a->location), expr(a->lexpr), expr(a->rexpr), a->location,
                        clause == "ON"});
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
      if (c->contype != PG_QUERY__CONSTR_TYPE__CONSTR_PRIMARY) reject("table constraint not supported", c->location);
      ++primary_keys;
      at_most_one(c->location);
      for (std::size_t k = 0; k < c->n_keys; ++k) t.pk.push_back(str(c->keys[k]));
      continue;
    }
    if (el->node_case != PG_QUERY__NODE__NODE_COLUMN_DEF) reject("table element not supported", -1);
    t.columns.push_back(column_def(el->column_def, primary_keys));
    at_most_one(t.columns.back().pos);
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

auto select(const PgQuery__SelectStmt* s, std::int32_t pos) -> Select {
  if (s->op != PG_QUERY__SET_OPERATION__SETOP_NONE) reject("set operations are not supported", pos);
  // One table, or two: FROM a, b, or a [INNER] JOIN b ON …, whose ON
  // conjuncts join the WHERE's.
  if (s->n_from_clause != 1 && s->n_from_clause != 2) reject("SELECT reads one or two tables", pos);
  std::vector<const PgQuery__RangeVar*> tables;
  const PgQuery__Node* on = nullptr;
  for (std::size_t i = 0; i < s->n_from_clause; ++i) {
    const auto* from = s->from_clause[i];
    if (from->node_case == PG_QUERY__NODE__NODE_JOIN_EXPR) {
      const auto* j = from->join_expr;
      // The join has no position of its own; report the table it joins.
      const auto* r = j->rarg;
      auto at = r != nullptr && r->node_case == PG_QUERY__NODE__NODE_RANGE_VAR ? r->range_var->location : pos;
      if (s->n_from_clause != 1 || j->jointype != PG_QUERY__JOIN_TYPE__JOIN_INNER || j->is_natural ||
          j->n_using_clause != 0 || j->alias != nullptr || j->larg == nullptr || r == nullptr ||
          j->larg->node_case != PG_QUERY__NODE__NODE_RANGE_VAR || r->node_case != PG_QUERY__NODE__NODE_RANGE_VAR) {
        reject("only an inner join of two tables, by ON, is supported", at);
      }
      tables = {j->larg->range_var, r->range_var};
      on = j->quals;
      continue;
    }
    if (from->node_case != PG_QUERY__NODE__NODE_RANGE_VAR) reject("SELECT reads one or two tables", pos);
    tables.push_back(from->range_var);
  }
  if (s->n_distinct_clause != 0 || s->n_group_clause != 0 || s->having_clause != nullptr ||
      s->limit_offset != nullptr || s->n_locking_clause > 1 || s->with_clause != nullptr ||
      s->n_window_clause != 0 || s->into_clause != nullptr) {
    reject("SELECT clause not supported", pos);
  }
  Select sel{{}, relation(tables[0]), tables.size() == 2 ? relation(tables[1]) : "", {}, {}, limit(s), pos, false};
  sel.table_pos = tables[0]->location;
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
    if (sb->sortby_nulls != PG_QUERY__SORT_BY_NULLS__SORTBY_NULLS_DEFAULT) {
      reject("NULLS FIRST and NULLS LAST are not supported", col.pos);
    }
    if (sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_USING) reject("ORDER BY ... USING is not supported", col.pos);
    sel.order_by.push_back({col.column, sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_DESC, col.pos, col.table});
  }
  for (std::size_t i = 0; i < s->n_target_list; ++i) {
    const auto* rt = s->target_list[i]->res_target;
    if (!empty(rt->name)) reject("column aliases are not supported", rt->location);
    sel.targets.push_back(expr(rt->val));
  }
  where(on, sel.where, "ON", sel.join_pos);
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
  if (n->node_case == PG_QUERY__NODE__NODE_DROP_STMT) return drop(n->drop_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_ALTER_TABLE_STMT) return alter_table(n->alter_table_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_INSERT_STMT) return insert(n->insert_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_SELECT_STMT) return select(n->select_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_UPDATE_STMT) return update(n->update_stmt, pos);
  if (n->node_case == PG_QUERY__NODE__NODE_DELETE_STMT) return del(n->delete_stmt, pos);
  reject("statement not supported", pos);
}

}  // namespace bytecask::sql::ast
