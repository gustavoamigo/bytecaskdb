// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ast.cppm — the small AST the parser hands the binder: the prototype's DML
// forms and the stage-1 DDL of docs/catalog_design.md. Names are as
// written; positions are byte offsets into the statement text, -1 where
// libpg_query gives none.
//
// prototype: replaced by the designed front end (#492).

module;
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.ast;

export import bytecask.sql.sqlstate;

export namespace bytecask::sql::ast {

// A failure to parse, or a form outside the prototype's scope. Carries the
// SQLSTATE PostgreSQL reports for the same fault; 42601 (syntax_error) is the
// default, for a syntax error and for a form the layer does not support yet.
class SqlError : public std::runtime_error {
public:
  SqlError(const std::string& what, std::int32_t pos, SqlState sqlstate = kSyntaxError)
      : std::runtime_error{what}, pos_{pos}, sqlstate_{sqlstate} {}
  SqlError(const SqlError&) = default;
  SqlError& operator=(const SqlError&) = default;
  ~SqlError() override;  // out of line: anchors the vtable (-Wweak-vtables)
  [[nodiscard]] auto pos() const noexcept -> std::int32_t { return pos_; }
  [[nodiscard]] auto sqlstate() const noexcept -> std::string_view { return sqlstate_.view(); }
  // Whether PostgreSQL raises it as it analyses the statement, before it
  // looks for a table the statement creates or plans the statement: an
  // unknown literal's input function rejecting the text ('abc'::int, DATE
  // '2020-13-01'). A data exception that is not raises as the constant is
  // folded, when the statement is planned (1/0).
  [[nodiscard]] auto at_analysis() const noexcept -> bool { return at_analysis_; }
  void mark_at_analysis() noexcept { at_analysis_ = true; }
  // PostgreSQL's DETAIL and HINT for the error, empty where it has none.
  // Shared, so copying the error cannot throw.
  [[nodiscard]] auto detail() const noexcept -> std::string_view { return notes_ ? std::string_view{notes_->first} : ""; }
  [[nodiscard]] auto hint() const noexcept -> std::string_view { return notes_ ? std::string_view{notes_->second} : ""; }
  [[nodiscard]] auto with_notes(std::string detail, std::string hint) && -> SqlError&& {
    notes_ = std::make_shared<const std::pair<std::string, std::string>>(std::move(detail), std::move(hint));
    return std::move(*this);
  }

private:
  std::int32_t pos_;
  SqlState sqlstate_;
  bool at_analysis_{false};
  std::shared_ptr<const std::pair<std::string, std::string>> notes_;
};

// A statement the layer does not support where it was given, the API's
// FeatureNotSupported (api/database.cppm): SQLSTATE 0A000. Here, below the
// planner, so the planner can refuse with it.
class FeatureNotSupported : public std::logic_error {
public:
  using std::logic_error::logic_error;
  FeatureNotSupported(const FeatureNotSupported&) = default;
  FeatureNotSupported& operator=(const FeatureNotSupported&) = default;
  ~FeatureNotSupported() override;  // out of line: anchors the vtable (-Wweak-vtables)
};

// A constant, as written: a literal, or DEFAULT's operand. Numbers keep
// their text, so the binder folds them exactly to the type they meet.
struct Literal {
  enum class Kind { Null, Integer, Number, String, Bool };
  Kind kind{Kind::Null};
  std::string text;  // Integer and Number: the digits as written; String: the bytes
  bool b{false};     // Bool
  std::int32_t pos{-1};
};

struct Select;

// A comparison as a value (SELECT 3 < 5): its operator, <> among them. A
// WHERE's comparisons are Compare's, below.
enum class ValueCompare { Eq, Ne, Lt, Le, Gt, Ge };

// A select-list, SET or comparison operand: a column, a parameter, a
// literal (an integer within BIGINT after + or - is a Const), a sum,
// difference, product or quotient of two, unary + or - of one, a comparison of two, IS [NOT]
// NULL of one, AND, OR or NOT of boolean expressions, a cast of
// one (DATE '…' and '…'::date alike), an aggregate: COUNT(*), or
// COUNT, SUM, MIN, MAX or AVG of an expression, [DISTINCT]; or a scalar
// subquery, (SELECT …), the value of its one row; CASE (searched, or simple
// on `lhs`), COALESCE and NULLIF; a string function or ||; or, in a select list only,
// * or `table`.*, which the binder expands to columns.
struct Expr {
  enum class Kind { Column, Param, Const, Literal, Add, Sub, Mul, Div, Star, Plus, Neg, Compare, IsNull, Not, And, Or, Cast, Aggregate, Subquery, Case, Coalesce, NullIf, Func, Clock, Default };
  enum class Fn { Count, Sum, Min, Max, Avg };
  Kind kind;
  std::string column;       // Column
  std::uint32_t param{0};   // Param: zero-based ($1 is 0)
  std::unique_ptr<Expr> lhs, rhs;  // Add, Sub, Mul, Div, Compare; Plus, Neg, IsNull, Not and Cast: rhs
  std::int32_t pos{-1};
  std::string table;          // Column, Star: the table it is qualified by, "" for none
  std::int64_t constant{0};   // Const: an integer literal (Add and Sub's right operand only)
  Fn fn{Fn::Count};           // Aggregate
  bool distinct{false};       // Aggregate: of DISTINCT values
  bool star{false};           // Aggregate: COUNT(*), no argument; any other has its one in `args`
  bool negated{false};        // IsNull: IS NOT NULL
  Literal literal{};          // Literal
  ValueCompare cmp{ValueCompare::Eq};  // Compare
  std::string cast_type;               // Cast: libpg_query's last type name, as a column's
  std::vector<std::int32_t> cast_typmods;
  std::shared_ptr<const Select> subquery;  // Subquery
  std::string alias;  // a select-list item's AS name, "" for none
  // And, Or: two or more operands. Case: WHEN, THEN, WHEN, THEN … in order, `lhs` the simple form's operand
  // and `rhs` its ELSE, each none where absent. Coalesce: its arguments.
  // NullIf: `lhs` and `rhs`. Aggregate: its argument, none for COUNT(*).
  std::vector<Expr> args;
  // Func: a string function, abs, % or ||, `func` its name as PostgreSQL resolves it
  // (btrim for TRIM(BOTH …), position(string, substring) for POSITION(sub IN
  // string)), its operands in `args`.
  std::string func;
  // Clock: `func` is the SQL name (current_date, localtime, …) and `constant`
  // the precision written after it, -1 for none; `column` is "now" for now(),
  // the name of its output column, "" for the others, named by `func`.
};

// One conjunct of a WHERE clause: two expressions compared; or, as a
// predicate, AND, OR or NOT of conjuncts (`args`), `lhs` IS NULL, `lhs`
// LIKE the pattern `rhs`, with its ESCAPE as written, EXISTS (`subquery`),
// or `lhs` compared by `quantified` with ANY or ALL (`all`) of the
// subquery's rows: IN is = ANY, NOT IN its NOT. The
// parser writes <>, BETWEEN and IN with them: NOT (a = b); a >= lo AND a <=
// hi; a = x OR a = y.
struct Compare {
  enum class Op { Eq, Lt, Le, Gt, Ge };
  enum class Kind { Compare, And, Or, Not, IsNull, Like, Exists, Quantified, Const };
  Op op;
  Expr lhs, rhs;
  std::int32_t pos{-1};
  std::uint32_t on{0};  // 0: the WHERE's; k: the ON conjunct of Select::from[k]
  // An ON's first FROM item: it sees from[on_from] to from[on], the items of
  // its own join. 0 but for a join in parentheses or after a comma.
  std::uint32_t on_from{0};
  Kind kind{Kind::Compare};
  std::vector<Compare> args;  // And, Or: two or more; Not: one
  std::optional<std::string> escape;  // Like: ESCAPE's string; none for the default, a backslash
  std::optional<std::uint32_t> escape_param;  // Like: ESCAPE $n, zero-based, in place of `escape`
  bool fold{false};  // Like: ILIKE, ASCII letters matched in either case
  std::shared_ptr<const Select> subquery;  // Exists, Quantified
  ValueCompare quantified{ValueCompare::Eq};
  bool all{false};
  bool ne{false};  // Compare, Eq: written <> (or !=), under its NOT; an error names <>
  std::optional<bool> truth;  // Const: TRUE or FALSE; none for NULL
};

// A copy of e, which owns its operands: what BETWEEN and IN repeat.
[[nodiscard]] auto clone(const Expr& e) -> Expr {
  Expr out{e.kind, e.column, e.param, nullptr, nullptr, e.pos};
  if (e.lhs) out.lhs = std::make_unique<Expr>(clone(*e.lhs));
  if (e.rhs) out.rhs = std::make_unique<Expr>(clone(*e.rhs));
  out.table = e.table;
  out.constant = e.constant;
  out.fn = e.fn;
  out.distinct = e.distinct;
  out.star = e.star;
  out.negated = e.negated;
  out.literal = e.literal;
  out.cmp = e.cmp;
  out.cast_type = e.cast_type;
  out.cast_typmods = e.cast_typmods;
  out.subquery = e.subquery;  // never changed once parsed: shared
  for (const auto& a : e.args) out.args.push_back(clone(a));
  out.func = e.func;
  return out;
}

// An ORDER BY key: an expression of the FROM columns, a column reference
// the most common one. An output name or a position of a plain SELECT's
// select item is that item's expression, as the parser resolves it.
struct OrderItem {
  Expr expr;
  // ORDER BY 2: the select-list position, 1-based, and no expression. The
  // parser resolves it in a plain SELECT; a set operation's the binder does.
  std::int64_t position{0};
  bool desc{false};
  std::int32_t pos{-1};
  // NULLS FIRST (true) or NULLS LAST (false); nullopt for PostgreSQL's
  // default, last under ASC and first under DESC.
  std::optional<bool> nulls_first;
};

[[nodiscard]] auto clone(const OrderItem& o) -> OrderItem {
  return {clone(o.expr), o.position, o.desc, o.pos, o.nulls_first};
}

[[nodiscard]] auto clone(const std::vector<OrderItem>& order_by) -> std::vector<OrderItem> {
  std::vector<OrderItem> out;
  for (const auto& o : order_by) out.push_back(clone(o));
  return out;
}

// Whether two expressions are written alike, as PostgreSQL's equal() finds
// two select items or an ORDER BY key and a select item one: their kinds,
// names, constants and operands; never their positions or AS names. A
// subquery is the same one only as the same parse.
[[nodiscard]] auto same_expr(const Expr& a, const Expr& b) -> bool {
  auto both = [](const std::unique_ptr<Expr>& x, const std::unique_ptr<Expr>& y) {
    return x == nullptr ? y == nullptr : y != nullptr && same_expr(*x, *y);
  };
  return a.kind == b.kind && a.column == b.column && a.param == b.param && a.table == b.table &&
         a.constant == b.constant && a.fn == b.fn && a.distinct == b.distinct && a.star == b.star && a.negated == b.negated &&
         a.literal.kind == b.literal.kind && a.literal.text == b.literal.text && a.literal.b == b.literal.b &&
         a.cmp == b.cmp && a.cast_type == b.cast_type && a.cast_typmods == b.cast_typmods && a.subquery == b.subquery &&
         a.func == b.func && both(a.lhs, b.lhs) && both(a.rhs, b.rhs) &&
         std::ranges::equal(a.args, b.args, same_expr);
}

// A FOREIGN KEY constraint of CREATE TABLE, on a column (REFERENCES) or
// on the table: its name, "" for none, its columns, and the table and
// columns it references, none for that table's primary key. Checked
// against the referenced table's keys, then not enforced.
struct ForeignKeyAst {
  std::string name;
  std::vector<std::string> columns;
  std::string table;
  std::vector<std::string> ref_columns;
  std::int32_t pos{-1};
};

// A CHECK constraint of CREATE TABLE, on a column or on the table: its name,
// "" for none, and the text of its expression, as written between the
// parentheses. `column` is the column a column CHECK is written on, "" for
// a table's.
struct CheckAst {
  std::string name;
  std::string text;
  std::int32_t pos{-1};
  std::string column;
};

struct ColumnDefAst {
  std::string name;
  std::string type;                    // libpg_query's last type name: int8, varchar, numeric, …
  std::vector<std::int32_t> typmods;   // VARCHAR(n): {n}; NUMERIC(p, s): {p, s} or {p}
  bool not_null{false};
  bool primary_key{false};
  std::optional<Literal> default_value;
  std::int32_t pos{-1};
  // Each UNIQUE constraint on the column: its name, "" for none.
  std::vector<std::string> unique;
  std::vector<ForeignKeyAst> references;  // each REFERENCES on the column
  std::vector<CheckAst> checks;    // each CHECK on the column
  // DEFAULT of the clock: current_date, current_timestamp (now() too),
  // localtime or localtimestamp, and its precision, -1 for none written.
  std::string default_clock;
  std::int64_t default_clock_precision{-1};
};

// A UNIQUE constraint of CREATE TABLE, on a column or on the table, in the
// order written: its name ("" for one the catalog chooses) and columns.
struct UniqueAst {
  std::string name;
  std::vector<std::string> columns;
  std::int32_t pos{-1};
};

struct CreateTable {
  std::string table;
  std::vector<ColumnDefAst> columns;
  std::vector<std::string> pk;  // a table-level PRIMARY KEY (…)
  std::int32_t pos{-1};
  std::vector<UniqueAst> unique;  // every UNIQUE constraint, columns' and the table's, in order
  std::vector<ForeignKeyAst> foreign_keys;  // every FOREIGN KEY, columns' and the table's, in order
  std::vector<CheckAst> checks;  // every CHECK, columns' and the table's, in order
  bool if_not_exists{false};     // IF NOT EXISTS: a relation of the name skips the statement
};

struct IndexColumnAst {
  std::string name;
  bool desc{false};
};

struct CreateIndex {
  std::string index;
  std::string table;
  std::vector<IndexColumnAst> columns;
  bool unique{false};
  std::int32_t pos{-1};
};

struct DropTable {
  std::string table;
  bool if_exists{false};
  std::int32_t pos{-1};
};

struct DropIndex {
  std::string index;
  bool if_exists{false};
  std::int32_t pos{-1};
};

// ALTER TABLE … ADD COLUMN, the one ALTER TABLE form in scope.
struct AddColumn {
  std::string table;
  ColumnDefAst column;
  std::int32_t pos{-1};
};

struct Insert {
  std::string table;
  std::vector<std::string> columns;  // empty: every column, in order
  std::vector<std::vector<Expr>> rows;  // VALUES' rows, each of one length; none with a query
  std::int32_t pos{-1};
  std::vector<Expr> returning;  // RETURNING's list, empty for none
  // INSERT … SELECT: the query whose rows are written, in place of VALUES.
  std::shared_ptr<const Select> query;
  // ON CONFLICT DO NOTHING: the columns its target names, empty for a
  // conflict on any unique constraint.
  struct OnConflict {
    std::vector<std::string> columns;
    std::int32_t pos{-1};
  };
  std::optional<OnConflict> on_conflict;
};


struct SetOperation;
struct Select;

// How a FROM item joins the ones before it: LEFT keeps every row of the
// items before it, RIGHT every row of the item, FULL both, each joined to
// NULLs where no row of the other matches. FULL is only the whole FROM of
// two tables (the parser).
enum class JoinKind { Inner, Left, Right, Full };

// A FROM item: a table or view, and how it joins the items before it. A
// comma is an inner join with no ON. USING (…) or NATURAL compare the
// columns the two sides share by =, each named alone as one column;
// `using_alias` names them too: USING (…) AS alias.
struct FromItem {
  std::string table;
  // The name the query gives the item. The parser has rewritten every
  // qualifier that names an alias to its table, so the alias stays only for a
  // query that names one table twice, where it tells the two apart.
  std::string alias;
  // The name the statement gives the item, its alias else its table, kept
  // where `alias` is dropped: what an error about the item names.
  std::string written;
  // t AS x (a, b): the names of the table's first columns in this query.
  std::vector<std::string> column_aliases;
  std::int32_t pos{-1};
  JoinKind kind{JoinKind::Inner};
  // The first FROM item of the join's left side: 0, or the first item after
  // a comma. A RIGHT JOIN keeps its item's rows against from[join_from] to
  // the item before it, never against the items before join_from.
  std::uint32_t join_from{0};
  std::vector<std::string> using_columns;
  bool natural{false};
  std::string using_alias;
  // A grouped view read as a relation of its own (docs/frontend_design.md,
  // *Views*): its query, expanded to base tables and without its ORDER BY,
  // whose rows the item's are; `table` is the view's name and
  // `column_aliases` its column names. Null for a table or a view merged into
  // the query.
  std::shared_ptr<const Select> derived;

  // What a column of the item is qualified by.
  [[nodiscard]] auto name() const -> const std::string&;
};

auto FromItem::name() const -> const std::string& { return alias.empty() ? table : alias; }

struct Select {
  std::vector<Expr> targets;
  std::vector<FromItem> from;  // empty: a SELECT without FROM
  std::vector<Compare> where;
  std::vector<OrderItem> order_by;
  std::optional<std::int64_t> limit;  // nullopt: none, LIMIT ALL or LIMIT NULL
  std::int64_t offset{0};             // OFFSET n: the rows skipped before LIMIT counts
  // LIMIT, FETCH or OFFSET was written, even one that limits nothing (LIMIT
  // ALL, LIMIT NULL, OFFSET 0): PostgreSQL will not write through such a view.
  bool limit_written{false};
  std::int32_t pos{-1};
  bool for_update{false};  // FOR UPDATE: a guard on each row returned, never a lock
  std::vector<Expr> group_by;  // GROUP BY's columns, as written
  std::vector<Compare> having;  // HAVING's conjuncts
  bool distinct{false};         // SELECT DISTINCT, not DISTINCT ON
  // A cursor's query (docs/cursor_design.md, *WHERE CURRENT OF*): the binder
  // appends the table's primary key to the result as hidden trailing columns
  // when the query is a simple scan of one table, and adds nothing otherwise.
  bool row_key{false};
  // A set operation: this query is `set->left` combined with `set->right`,
  // and has no select list or FROM of its own; its order_by and limit apply
  // to the combined rows. A parenthesised arm keeps its own.
  std::shared_ptr<const SetOperation> set;
};

// UNION, INTERSECT or EXCEPT of two queries, each of them a set operation
// too. `all` keeps duplicates (UNION ALL), as PostgreSQL's ALL does.
struct SetOperation {
  enum class Kind { Union, Intersect, Except };
  Kind kind{Kind::Union};
  bool all{false};
  Select left, right;
};

// A copy of c and of s, which own their operands. A subquery is shared:
// it is never changed once parsed.
[[nodiscard]] auto clone(const Compare& c) -> Compare {
  Compare out{c.op, clone(c.lhs), clone(c.rhs), c.pos};
  out.on = c.on;
  out.on_from = c.on_from;
  out.kind = c.kind;
  for (const auto& a : c.args) out.args.push_back(clone(a));
  out.escape = c.escape;
  out.escape_param = c.escape_param;
  out.fold = c.fold;
  out.subquery = c.subquery;
  out.quantified = c.quantified;
  out.all = c.all;
  out.ne = c.ne;
  out.truth = c.truth;
  return out;
}

[[nodiscard]] auto clone(const Select& s) -> Select {
  Select out{};
  for (const auto& e : s.targets) {
    out.targets.push_back(clone(e));
    out.targets.back().alias = e.alias;
  }
  out.from = s.from;
  for (const auto& c : s.where) out.where.push_back(clone(c));
  out.order_by = clone(s.order_by);
  out.limit = s.limit;
  out.offset = s.offset;
  out.limit_written = s.limit_written;
  out.pos = s.pos;
  out.for_update = s.for_update;
  for (const auto& e : s.group_by) out.group_by.push_back(clone(e));
  for (const auto& c : s.having) out.having.push_back(clone(c));
  out.distinct = s.distinct;
  out.row_key = s.row_key;
  out.set = s.set;  // never changed once parsed: shared
  return out;
}

struct Assignment {
  std::string column;
  Expr value;
  std::int32_t pos{-1};
};

struct Update {
  std::string table;
  std::vector<Assignment> set;
  std::vector<Compare> where;
  std::int32_t pos{-1};
  std::string current_of;  // WHERE CURRENT OF name: the cursor, and `where` is empty
  std::vector<Expr> returning;  // RETURNING's list, empty for none
};

struct Delete {
  std::string table;
  std::vector<Compare> where;
  std::int32_t pos{-1};
  std::string current_of;  // WHERE CURRENT OF name: the cursor, and `where` is empty
  std::vector<Expr> returning;  // RETURNING's list, empty for none
};

// CREATE VIEW name [(columns)] AS query. `query` is the SELECT as written;
// the catalog stores the statement's text and parses it again at each use.
struct CreateView {
  std::string view;
  std::vector<std::string> columns;  // the view's column names, empty: the query's own
  Select query;
  std::int32_t pos{-1};
  std::string definition;  // the statement's text, set by parse()
};

// CREATE TABLE name [(columns)] AS query [WITH [NO] DATA], or SELECT … INTO
// name: a table of the query's columns, holding its rows (none for WITH NO
// DATA), created as one DDL. `form` is what was written, which names the
// statement in its errors and tag. `query` is the SELECT without its INTO.
struct CreateTableAs {
  enum class Form { CreateTableAs, SelectInto };
  Form form{Form::CreateTableAs};
  std::string table;
  std::vector<std::string> columns;  // the table's column names, empty: the query's own
  Select query;
  bool with_data{true};
  bool if_not_exists{false};  // IF NOT EXISTS: a relation of the name skips the statement
  std::int32_t pos{-1};
};

struct DropView {
  std::string view;
  bool if_exists{false};
  std::int32_t pos{-1};
};

// CREATE [OR REPLACE] PROCEDURE name (args) LANGUAGE plpgsql AS $$ body $$
// (docs/procedures_design.md). Each argument's type is read as a column's
// is; the body's PL/pgSQL syntax was checked by parse(). The catalog stores
// the statement's text.
struct ProcedureArgAst {
  enum class Mode { In, InOut, Out };
  Mode mode{Mode::In};
  ColumnDefAst def;  // name, type and modifiers; no constraint
};

struct CreateProcedure {
  std::string procedure;
  bool replace{false};
  std::vector<ProcedureArgAst> args;
  std::int32_t pos{-1};
  std::string definition;  // the statement's text, set by parse()
};

// DROP PROCEDURE [IF EXISTS] name [(argtype, …)]. `arg_types` is nullopt
// when no list was written, and holds each type's name and modifiers
// otherwise (a ColumnDefAst with no name).
struct DropProcedure {
  std::string procedure;
  std::optional<std::vector<ColumnDefAst>> arg_types;
  bool if_exists{false};
  std::int32_t pos{-1};
};

// CREATE ROLE name, with no option: the layer has no logins (#653).
struct CreateRole {
  std::string role;
  std::int32_t pos{-1};
};

// DROP ROLE [IF EXISTS] name [, …].
struct DropRole {
  std::vector<std::string> roles;
  bool if_exists{false};
  std::int32_t pos{-1};
};

// One privilege of a GRANT or REVOKE, as written: PostgreSQL's lower-case
// name ("select", …, or "all"), and the columns of a column privilege.
struct PrivilegeAst {
  std::string name;
  std::vector<std::string> columns;
};

// GRANT or REVOKE of privileges on tables and views (docs/frontend_design.md,
// *Roles and privileges*). Names are checked when the catalog applies it, in
// PostgreSQL's order.
// CREATE DOMAIN name [AS] type, with no constraint, DEFAULT or COLLATE yet.
// Stored, and granted on; not yet a column's type.
struct CreateDomain {
  std::string domain;
  ColumnDefAst base;  // its type, as a column's
  std::int32_t pos{-1};
};

// CREATE TYPE name: a shell type, which nothing can use, as in PostgreSQL.
struct CreateType {
  std::string type;
  std::int32_t pos{-1};
};

// CREATE SEQUENCE [IF NOT EXISTS] name, with no option yet. Stored, and
// granted on; nextval and the rest are not built.
struct CreateSequence {
  std::string sequence;
  bool if_not_exists{false};
  std::int32_t pos{-1};
};

// DROP DOMAIN, DROP TYPE (a domain or a shell type) or DROP SEQUENCE
// [IF EXISTS] name.
struct DropTypeOrSequence {
  enum class Kind { Domain, Type, Sequence };
  Kind kind{Kind::Domain};
  std::string name;
  bool if_exists{false};
  std::int32_t pos{-1};
};

struct Grant {
  enum class Target { Table, Sequence, Domain, Type };
  Target target{Target::Table};          // what ON names
  bool grant{true};                      // false: REVOKE
  bool grant_option{false};              // GRANT … WITH GRANT OPTION, REVOKE GRANT OPTION FOR …
  std::vector<PrivilegeAst> privileges;  // empty: ALL PRIVILEGES
  std::vector<std::string> objects;
  std::vector<std::string> grantees;     // "" is PUBLIC
  std::optional<std::string> grantor;    // GRANTED BY a role named; nullopt for none or the current user
  std::int32_t pos{-1};
};

using Statement = std::variant<CreateTable, CreateIndex, DropTable, DropIndex, AddColumn, CreateView, DropView,
                               CreateProcedure, DropProcedure, CreateRole, DropRole, Grant, CreateDomain, CreateType,
                               CreateSequence, DropTypeOrSequence, CreateTableAs, Insert, Select, Update, Delete>;

// Cursor statements (docs/cursor_design.md). They are not a Statement: they
// name a session's cursor and carry no plan, so the session runs them, as it
// does BEGIN. A DECLARE's query is parsed again as the SELECT it is.
struct DeclareCursor {
  enum class Scroll { Default, Scroll, NoScroll };
  std::string name;
  Scroll scroll{Scroll::Default};
  bool binary{false};
  bool hold{false};
  bool for_update{false};  // the query has a FOR UPDATE
  // The query's text: the DECLARE's own with everything before the query
  // blanked, so an error position into it is a position into the statement.
  std::string query;
};

// FETCH and MOVE. `count` is PostgreSQL's howMany: kAll for ALL, negative for
// "FETCH -2", the target row for Absolute and the offset for Relative.
struct FetchCursor {
  enum class Direction { Forward, Backward, Absolute, Relative };
  static constexpr std::int64_t kAll = std::numeric_limits<std::int64_t>::max();
  std::string name;
  Direction direction{Direction::Forward};
  std::int64_t count{1};
  bool move{false};
};

// CLOSE name, or CLOSE ALL when `all`.
struct CloseCursor {
  std::string name;
  bool all{false};
};

using CursorStatement = std::variant<DeclareCursor, FetchCursor, CloseCursor>;

// CALL name(arg, …) (docs/procedures_design.md, *Wire protocol for CALL*).
// Not a Statement: the session runs it through the embedded API's call, as
// it runs BEGIN. Each argument is an expression of constants, bound against
// the procedure's argument types when it runs.
struct Call {
  std::string procedure;
  std::vector<Expr> args;
  std::int32_t pos{-1};
};

}  // namespace bytecask::sql::ast

namespace bytecask::sql::ast {

SqlError::~SqlError() = default;
FeatureNotSupported::~FeatureNotSupported() = default;

}  // namespace bytecask::sql::ast
