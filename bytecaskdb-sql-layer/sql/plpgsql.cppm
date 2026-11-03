// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// plpgsql.cppm — a procedure's body as the procedure IR of
// docs/procedures_design.md, *Compilation pipeline*: libpg_query's
// pg_query_parse_plpgsql reads the CREATE PROCEDURE text, and its JSON
// becomes variables and statements. Expressions and embedded SQL stay text;
// the PL binder binds them. A PL/pgSQL statement the layer does not run is
// refused here, so CREATE PROCEDURE refuses it.

module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-identifier"
#pragma clang diagnostic ignored "-Wreserved-macro-identifier"
#include <pg_query.h>
#pragma clang diagnostic pop

export module bytecask.sql.plpgsql;

export import bytecask.sql.ast;
import bytecask.sql.json;

export namespace bytecask::sql::pl {

// A variable: an argument (the first ones, in order), FOUND, or a DECLAREd
// one. `type` is the type as written for a declared variable, and empty for
// an argument and FOUND, whose types the signature and the language give.
struct Var {
  std::string name;
  std::string type;
  std::string default_expr;  // DECLARE x T := expr; empty for none
  bool constant{false};
  bool not_null{false};
};

struct Stmt;
using Body = std::vector<Stmt>;

// x := expr. `expr` is the right side alone.
struct Assign {
  std::uint32_t var;
  std::string expr;
};
// IF … ELSIF … ELSE: each condition with its body, in order, then ELSE's.
struct If {
  std::vector<std::pair<std::string, Body>> branches;
  Body otherwise;
};
// FOR var IN lower..upper LOOP: the bounds are evaluated once, on entry.
struct ForRange {
  std::uint32_t var;
  std::string lower, upper;
  bool reverse{false};
  Body body;
};
// WHILE cond LOOP, or LOOP when `cond` is empty.
struct While {
  std::string cond;
  Body body;
};
// EXIT [WHEN cond] (`exit`), or CONTINUE [WHEN cond]: of the innermost loop.
struct Exit {
  bool exit{true};
  std::string cond;
};
// An embedded SQL statement, or PERFORM's SELECT. With INTO, its first row's
// columns go to `targets`, in order; `strict` makes no row (P0002) and a
// second row (P0003) errors.
struct Exec {
  std::string sql;
  bool into{false};
  bool strict{false};
  std::vector<std::uint32_t> targets;
  bool perform{false};  // PERFORM: a SELECT whose rows are dropped
};
struct Return {};
// FOR targets IN query LOOP: each row of the query, in order, into the
// targets (`query.targets`), the body run once per row.
struct ForQuery {
  Exec query;
  Body body;
};

struct Stmt {
  std::variant<Assign, If, ForRange, While, Exit, Exec, Return, ForQuery> v;
  std::int32_t line{0};
};

struct Function {
  std::vector<Var> vars;  // the arguments first, then FOUND, then the rest
  std::uint32_t found{0};  // FOUND's variable
  Body body;
};

// Reads the body of `create_procedure`, a CREATE PROCEDURE statement whose
// signature parse() has read, with `nargs` arguments. Throws ast::SqlError:
// 42601 for a body that does not parse, 0A000 for a PL/pgSQL statement the
// layer does not run; each at `pos`, the statement's position.
[[nodiscard]] auto read_body(std::string_view create_procedure, std::size_t nargs, std::int32_t pos = -1) -> Function;

}  // namespace bytecask::sql::pl

namespace bytecask::sql::pl {

namespace {

using json::Value;

// The one member of a node such as {"PLpgSQL_stmt_if": {...}}: its kind and body.
auto node(const Value& v) -> std::pair<std::string_view, const Value*> {
  if (v.kind != Value::Kind::Object || v.members.size() != 1) return {"", nullptr};
  const auto& [k, body] = *v.members.begin();
  return {k, &body};
}

auto expr_text(const Value* v) -> std::string {
  if (v == nullptr) return {};
  auto [kind, e] = node(*v);
  return kind == "PLpgSQL_expr" ? e->str("query") : std::string{};
}

class Reader {
public:
  Reader(const Value& fn, std::size_t nargs, std::int32_t pos) : fn_{fn}, nargs_{nargs}, pos_{pos} {}

  auto read() -> Function {
    auto datums = fn_.list("datums");
    // The datums are numbered as varno: the arguments, FOUND, then the
    // declarations of every block, and the rows INTO lists make.
    for (std::size_t i = 0; i < datums.size(); ++i) {
      auto [kind, d] = node(datums[i]);
      if (kind == "PLpgSQL_var") {
        Var v;
        v.name = d->str("refname");
        if (i >= nargs_ && v.name != "found") {
          if (const auto* t = d->get("datatype")) v.type = node(*t).second->str("typname");
          v.default_expr = expr_text(d->get("default_val"));
          v.constant = d->flag("isconst");
          v.not_null = d->flag("notnull");
          if (d->get("cursor_explicit_expr") != nullptr) refuse("cursor variables are not supported yet", line(*d));
        }
        for (const auto& [name, at] : var_of_) {
          if (vars_[at].name == v.name && i >= nargs_) refuse("two variables named " + v.name + " in one procedure", line(*d));
        }
        var_of_.emplace_back(i, static_cast<std::uint32_t>(vars_.size()));
        if (v.name == "found") found_ = static_cast<std::uint32_t>(vars_.size());
        vars_.push_back(std::move(v));
      } else if (kind != "PLpgSQL_row") {  // a row is an INTO list, read where it is used
        refuse("record and composite variables are not supported yet", line(*d));
      }
    }
    if (vars_.size() < nargs_ + 1) malformed("no procedure arguments");
    Function f;
    auto [kind, block] = node(*fn_.get("action"));
    if (kind != "PLpgSQL_stmt_block") malformed("no body");
    f.body = this->block(*block);
    // The body ends with an implicit RETURN, which ends it anyway.
    if (!f.body.empty() && std::holds_alternative<Return>(f.body.back().v)) f.body.pop_back();
    f.vars = std::move(vars_);
    f.found = found_;
    return f;
  }

private:
  // Every error of the body is at the CREATE PROCEDURE statement's position.
  [[noreturn]] void refuse(const std::string& what, std::int32_t at) const {
    throw ast::SqlError{what + (at > 0 ? " (line " + std::to_string(at) + " of the body)" : std::string{}), pos_,
                        kFeatureNotSupported};
  }
  [[noreturn]] void malformed(const std::string& what) const { throw ast::SqlError{"PL/pgSQL parse tree: " + what, pos_}; }

  static auto line(const Value& v) -> std::int32_t { return static_cast<std::int32_t>(v.integer("lineno")); }

  auto var(std::int64_t varno, std::int32_t at) -> std::uint32_t {
    for (const auto& [d, v] : var_of_) {
      if (static_cast<std::int64_t>(d) == varno) return v;
    }
    refuse("a target that is not a plain variable", at);
  }

  // INTO's targets: a row of variables, or one variable.
  auto targets(const Value* target, std::int32_t at) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> out;
    if (target == nullptr) return out;
    auto [kind, t] = node(*target);
    if (kind == "PLpgSQL_row") {
      for (const auto& field : t->list("fields")) out.push_back(var(field.integer("varno"), at));
      return out;
    }
    if (kind == "PLpgSQL_var") {
      // A var given whole: found by name, as its varno is not in it.
      auto name = t->str("refname");
      for (std::uint32_t k = 0; k < vars_.size(); ++k) {
        if (vars_[k].name == name) return {k};
      }
    }
    refuse("INTO a record is not supported yet", at);
  }

  auto block(const Value& b) -> Body {
    if (b.get("exceptions") != nullptr) refuse("EXCEPTION blocks are not supported yet", line(b));
    if (!b.str("label").empty()) refuse("block labels are not supported yet", line(b));
    return body(b.list("body"));
  }

  auto body(std::span<const Value> stmts) -> Body {
    Body out;
    for (const auto& s : stmts) {
      auto [kind, st] = node(s);
      if (st == nullptr) malformed("a statement that is not one");
      auto at = line(*st);
      if (kind == "PLpgSQL_stmt_block") {
        auto inner = block(*st);
        for (auto& x : inner) out.push_back(std::move(x));
        continue;
      }
      out.push_back({statement(kind, *st, at), at});
    }
    return out;
  }

  auto loop_label(const Value& st, std::int32_t at) {
    if (!st.str("label").empty()) refuse("loop labels are not supported yet", at);
  }

  auto statement(std::string_view kind, const Value& st, std::int32_t at) -> decltype(Stmt::v) {
    if (kind == "PLpgSQL_stmt_assign") {
      auto target = var(st.integer("varno"), at);
      // "x := expr" or "x = expr": the right side.
      auto text = expr_text(st.get("expr"));
      auto colon = text.find(":=");
      auto eq = text.find('=');
      auto cut = colon != std::string::npos && colon <= eq ? colon + 2 : eq + 1;
      if (eq == std::string::npos && colon == std::string::npos) refuse("an assignment it cannot read", at);
      if (text.find_first_of(".[") < std::min(colon, eq)) refuse("assignment to a field or an element", at);
      return Assign{target, text.substr(cut)};
    }
    if (kind == "PLpgSQL_stmt_if") {
      If out;
      out.branches.emplace_back(expr_text(st.get("cond")), body(st.list("then_body")));
      for (const auto& e : st.list("elsif_list")) {
        auto [k, elsif] = node(e);
        out.branches.emplace_back(expr_text(elsif->get("cond")), body(elsif->list("stmts")));
      }
      out.otherwise = body(st.list("else_body"));
      return out;
    }
    if (kind == "PLpgSQL_stmt_fori") {
      loop_label(st, at);
      if (st.get("step") != nullptr) refuse("FOR … BY is not supported yet", at);
      auto [k, v] = node(*st.get("var"));
      auto name = v->str("refname");
      std::optional<std::uint32_t> target;
      for (std::uint32_t i = 0; i < vars_.size(); ++i) {
        if (vars_[i].name == name) target = i;
      }
      if (!target) refuse("a FOR variable it cannot find", at);
      return ForRange{*target, expr_text(st.get("lower")), expr_text(st.get("upper")), st.flag("reverse"),
                      body(st.list("body"))};
    }
    if (kind == "PLpgSQL_stmt_fors") {
      loop_label(st, at);
      Exec q{expr_text(st.get("query")), false, false, targets(st.get("var"), at), false};
      if (q.targets.empty()) refuse("FOR over a query needs its variables", at);
      return ForQuery{std::move(q), body(st.list("body"))};
    }
    if (kind == "PLpgSQL_stmt_while") {
      loop_label(st, at);
      return While{expr_text(st.get("cond")), body(st.list("body"))};
    }
    if (kind == "PLpgSQL_stmt_loop") {
      loop_label(st, at);
      return While{"", body(st.list("body"))};
    }
    if (kind == "PLpgSQL_stmt_exit") {
      if (!st.str("label").empty()) refuse("EXIT and CONTINUE with a label are not supported yet", at);
      return Exit{st.flag("is_exit"), expr_text(st.get("cond"))};
    }
    if (kind == "PLpgSQL_stmt_execsql") {
      Exec out{expr_text(st.get("sqlstmt")), st.flag("into"), st.flag("strict"), {}, false};
      if (out.into) out.targets = targets(st.get("target"), at);
      return out;
    }
    if (kind == "PLpgSQL_stmt_perform") return Exec{expr_text(st.get("expr")), false, false, {}, true};
    if (kind == "PLpgSQL_stmt_return") {
      if (st.get("expr") != nullptr) refuse("RETURN with a value in a procedure", at);
      return Return{};
    }
    if (kind == "PLpgSQL_stmt_commit" || kind == "PLpgSQL_stmt_rollback") {
      refuse("COMMIT and ROLLBACK inside a procedure are not supported", at);
    }
    auto name = std::string{kind};
    if (name.starts_with("PLpgSQL_stmt_")) name = name.substr(13);
    refuse("the PL/pgSQL statement " + name + " is not supported yet", at);
  }

  const Value& fn_;
  std::size_t nargs_;
  std::int32_t pos_;
  std::vector<Var> vars_;
  std::vector<std::pair<std::size_t, std::uint32_t>> var_of_;  // datum number -> variable
  std::uint32_t found_{0};
};

}  // namespace

auto read_body(std::string_view create_procedure, std::size_t nargs, std::int32_t pos) -> Function {
  std::string text{create_procedure};
  auto parsed = pg_query_parse_plpgsql(text.c_str());
  std::optional<std::string> error;
  std::string tree;
  if (parsed.error != nullptr) {
    error = parsed.error->message;
  } else if (parsed.plpgsql_funcs != nullptr) {
    tree = parsed.plpgsql_funcs;
  }
  pg_query_free_plpgsql_parse_result(parsed);
  if (error) throw ast::SqlError{*error, pos};
  auto funcs = json::parse(tree);
  if (funcs.kind != json::Value::Kind::Array || funcs.items.size() != 1) {
    throw ast::SqlError{"expected one procedure", pos};
  }
  auto [kind, fn] = node(funcs.items[0]);
  if (kind != "PLpgSQL_function") throw ast::SqlError{"expected a PL/pgSQL procedure", pos};
  return Reader{*fn, nargs, pos}.read();
}

}  // namespace bytecask::sql::pl
