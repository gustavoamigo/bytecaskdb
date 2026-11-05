// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// procedure.cppm — a stored procedure compiled to one C file
// (docs/procedures_design.md, *One generated file per procedure*): its body
// read into the procedure IR, its variables typed as slots of a frame, each
// embedded statement bound and planned as a prepared statement would be and
// printed as s<i>, and proc() printed from the IR.

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.procedure;

export import bytecask.sql.codegen;
import bytecask.sql.plpgsql;

export namespace bytecask::sql {

// An embedded statement as the procedure runs it: what a prepared
// statement's compile keeps (docs/procedures_design.md, *The statement
// boundary*), plus which variable feeds each $n and which receive its row.
struct ProcedureStatementPlan {
  std::string sql;
  GeneratedPlan plan;
  std::vector<std::optional<SqlType>> param_sql;
  std::vector<std::uint32_t> param_table;
  std::vector<bound::ParamInfo> params;
  std::vector<Datum> fixed;
  std::vector<std::uint32_t> views;
  std::vector<std::uint32_t> param_vars;  // $k+1's variable slot
  bool into{false};
  bool strict{false};
  std::vector<std::uint32_t> targets;  // INTO's or the FOR loop's variable slots, one per result column
  bool loop{false};  // FOR targets IN this query: its rows are kept for bc_rows_next
};

struct GeneratedProcedure {
  std::string source;  // the file: proc(), s<i>, their subqueries
  std::uint64_t catalog_version{0};
  std::vector<ProcedureStatementPlan> stmts;
  // Every slot's type: the variables (the arguments first, in order), then
  // the constants of proc()'s expressions, whose values are `constants`.
  std::vector<SqlType> slot_types;
  std::vector<std::string> var_names;
  std::vector<Datum> constants;
  std::uint32_t found{0};
};

// Throws ast::SqlError for a body the layer cannot run: a name that is both
// a variable and a column (42702), a type it does not convert (0A000), an
// IF or WHILE condition that is not BOOLEAN (42804); and what binding and
// planning a statement throw.
[[nodiscard]] auto generate_procedure(const Procedure& proc, const Catalog& cat, const RowCounts& counts = {},
                                      std::uint64_t memory_bytes = 0) -> GeneratedProcedure;

}  // namespace bytecask::sql

namespace bytecask::sql {

namespace {

using ast::SqlError;

[[noreturn]] void refuse(const std::string& what) { throw SqlError{what + " is not supported yet", -1, kFeatureNotSupported}; }

auto integral(SqlType t) -> bool {
  return t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer || t.kind == SqlKind::BigInt;
}

class Compiler {
public:
  Compiler(const Procedure& p, const Catalog& cat, const RowCounts& counts, std::uint64_t memory)
      : p_{p}, cat_{cat}, counts_{counts}, memory_{memory} {}

  auto compile() -> GeneratedProcedure {
    auto fn = pl::read_body(p_.definition, p_.args.size());
    frame(fn);
    out_.found = fn.found;
    out_.catalog_version = cat_.version();
    proc::Body body;
    // A declared variable's default, assigned on entry; the others start NULL.
    for (std::uint32_t i = 0; i < fn.vars.size(); ++i) {
      if (!fn.vars[i].default_expr.empty()) body.push_back({assign(i, fn.vars[i].default_expr, true)});
    }
    auto rest = stmts(fn.body);
    for (auto& s : rest) body.push_back(std::move(s));
    print(body);
    return std::move(out_);
  }

private:
  // The frame: a table of no catalog whose columns are the variables.
  void frame(const pl::Function& fn) {
    frame_.id = Table::kInstanceBase - 1;
    for (std::uint32_t i = 0; i < fn.vars.size(); ++i) {
      const auto& v = fn.vars[i];
      SqlType t;
      if (i < p_.args.size()) {
        t = p_.args[i].type;
      } else if (i == fn.found) {
        t = SqlType{SqlKind::Boolean};
      } else {
        t = bound::declared_type(v.type);
        if (v.not_null) refuse("a NOT NULL variable");
      }
      constant_.push_back(v.constant);
      frame_.columns.push_back({i, v.name, t, phys_type(t), true, std::monostate{}});
      out_.var_names.push_back(v.name);
      out_.slot_types.push_back(t);
    }
    frame_.next_col_id = static_cast<std::uint32_t>(fn.vars.size());
  }

  auto nvars() const -> std::uint32_t { return static_cast<std::uint32_t>(out_.var_names.size()); }

  auto expr(const std::string& text, const Column* target = nullptr) -> logical::Expr {
    bound::ProcedureConstants k;
    auto first = nvars() + static_cast<std::uint32_t>(out_.constants.size());
    auto e = bound::bind_procedure_expr(text, frame_, cat_, first, k, target);
    for (std::size_t i = 0; i < k.values.size(); ++i) {
      out_.constants.push_back(std::move(k.values[i]));
      out_.slot_types.push_back(k.types[i]);
    }
    return e;
  }

  auto condition(const std::string& text, const char* what) -> logical::Expr {
    auto e = expr(text);
    if (e.type.kind != SqlKind::Boolean) {
      throw SqlError{std::string{"argument of "} + what + " must be type boolean, not " + sql_type_name(e.type), -1,
                     kDatatypeMismatch};
    }
    return e;
  }

  auto assign(std::uint32_t var, const std::string& text, bool initial = false) -> proc::Assign {
    const auto* col = frame_.column(var);
    if (constant_[var] && !initial) {
      throw SqlError{"variable \"" + col->name + "\" is declared CONSTANT", -1};
    }
    auto e = expr(text, col);
    // An exact number of another scale is brought to the variable's, as
    // PostgreSQL's assignment cast to NUMERIC(p, s) does; then checked
    // against its type when stored.
    auto exact = [](SqlType t) {
      return t.kind == SqlKind::SmallInt || t.kind == SqlKind::Integer || t.kind == SqlKind::BigInt || t.kind == SqlKind::Decimal;
    };
    if (exact(e.type) && col->sql.kind == SqlKind::Decimal && decimal_scale(e.type) != decimal_scale(col->sql)) {
      auto rescale = static_cast<int>(decimal_scale(col->sql)) - static_cast<int>(decimal_scale(e.type));
      return {var, std::move(e), rescale};
    }
    if (!bound::assignable(e.type, col->sql)) {
      refuse("assigning " + sql_type_name(e.type) + " to variable " + col->name + " (" + sql_type_name(col->sql) + ")");
    }
    return {var, std::move(e), 0};
  }

  auto stmts(const pl::Body& body) -> proc::Body {
    proc::Body out;
    for (const auto& s : body) out.push_back(std::visit([&](const auto& x) { return stmt(x); }, s.v));
    return out;
  }

  auto stmt(const pl::Assign& a) -> proc::Stmt { return {assign(a.var, a.expr)}; }

  auto stmt(const pl::If& i) -> proc::Stmt {
    proc::If out;
    for (const auto& [cond, body] : i.branches) out.branches.emplace_back(condition(cond, "IF"), stmts(body));
    out.otherwise = stmts(i.otherwise);
    return {std::move(out)};
  }

  auto stmt(const pl::ForRange& f) -> proc::Stmt {
    if (!integral(frame_.column(f.var)->sql)) refuse("a FOR loop over a variable that is not an integer");
    auto lo = expr(f.lower);
    auto hi = expr(f.upper);
    if (!integral(lo.type) || !integral(hi.type)) refuse("a FOR bound that is not an integer");
    return {proc::ForRange{f.var, ranges_++, std::move(lo), std::move(hi), f.reverse, stmts(f.body)}};
  }

  auto stmt(const pl::While& w) -> proc::Stmt {
    proc::While out;
    if (!w.cond.empty()) out.cond = condition(w.cond, "WHILE");
    out.body = stmts(w.body);
    return {std::move(out)};
  }

  auto stmt(const pl::Exit& e) -> proc::Stmt {
    proc::Exit out{e.exit, std::nullopt};
    if (!e.cond.empty()) out.cond = condition(e.cond, e.exit ? "EXIT WHEN" : "CONTINUE WHEN");
    return {std::move(out)};
  }

  auto stmt(const pl::Return&) -> proc::Stmt { return {proc::Return{}}; }

  auto stmt(const pl::ForQuery& f) -> proc::Stmt {
    auto k = exec(f.query, true);
    return {proc::ForQuery{k, stmts(f.body)}};
  }

  auto stmt(const pl::Exec& x) -> proc::Stmt { return {proc::Exec{exec(x, false)}}; }

  // The statement's plan, its index among the procedure's.
  auto exec(const pl::Exec& x, bool loop) -> std::uint32_t {
    auto k = static_cast<std::uint32_t>(out_.stmts.size());
    auto st = bound::bind_procedure_statement(x.sql, frame_, cat_);
    ProcedureStatementPlan sp;
    sp.sql = x.sql;
    sp.param_sql = st.bound.param_sql;
    sp.param_table = st.bound.param_table;
    sp.params = st.bound.params;
    sp.fixed = st.bound.fixed;
    sp.views = st.bound.views;
    if (!st.bound.clocks.empty()) refuse("CURRENT_DATE, LOCALTIME and LOCALTIMESTAMP in a procedure's statement");
    sp.param_vars = std::move(st.param_vars);
    sp.into = x.into;
    sp.strict = x.strict;
    sp.targets = x.targets;
    sp.loop = loop;
    sp.plan = generate_plan(std::move(st.bound), cat_, x.sql, counts_, memory_, "s" + std::to_string(k));
    if (sp.plan.returns_rows && !x.into && !x.perform && !loop) {
      throw SqlError{"query has no destination for result data: use PERFORM or INTO", -1};
    }
    if (x.into || loop) {
      if (!sp.plan.returns_rows) {
        if (loop) throw SqlError{"cannot open a FOR loop over a statement that returns no rows", -1};
        refuse("INTO of a statement that returns no rows (RETURNING)");
      }
      if (sp.plan.result_sql.size() != sp.plan.result_types.size()) refuse("INTO a NUMERIC average");
      if (sp.plan.result_sql.size() != x.targets.size()) {
        throw SqlError{"INTO names " + std::to_string(x.targets.size()) + " variables but the query returns " +
                           std::to_string(sp.plan.result_sql.size()) + " columns",
                       -1};
      }
      for (std::size_t i = 0; i < x.targets.size(); ++i) {
        const auto* col = frame_.column(x.targets[i]);
        if (!bound::assignable(sp.plan.result_sql[i], col->sql)) {
          refuse("INTO variable " + col->name + " (" + sql_type_name(col->sql) + ") of a " +
                 sql_type_name(sp.plan.result_sql[i]) + " column");
        }
        if (constant_[x.targets[i]]) throw SqlError{"variable \"" + col->name + "\" is declared CONSTANT", -1};
      }
    }
    out_.stmts.push_back(std::move(sp));
    return k;
  }

  void print(const proc::Body& body) {
    // The constants of every statement, each name once: a name comes from an
    // id, so two statements that use it give it the same value.
    std::vector<std::pair<std::string, std::uint32_t>> constants;
    std::string functions;
    for (const auto& s : out_.stmts) {
      for (const auto& c : s.plan.constants) {
        if (std::ranges::none_of(constants, [&](const auto& d) { return d.first == c.first; })) constants.push_back(c);
      }
      functions += s.plan.functions;
    }
    std::string text = "// PROCEDURE " + p_.name + "   schema: catalog v" + std::to_string(cat_.version()) + "\n" +
                       "#include \"bc_proc.h\"\n";
    if (!constants.empty()) {
      text += "enum { ";
      for (std::size_t i = 0; i < constants.size(); ++i) {
        text += (i == 0 ? "" : ", ") + constants[i].first + " = " + std::to_string(constants[i].second);
      }
      text += " };\n";
    }
    text += functions + "\nvoid proc(bc_proc *px) {\n" + print_procedure_body(body) + "}\n";
    out_.source = std::move(text);
  }

  const Procedure& p_;
  const Catalog& cat_;
  const RowCounts& counts_;
  std::uint64_t memory_;
  Table frame_;
  std::vector<bool> constant_;
  std::uint32_t ranges_{0};
  GeneratedProcedure out_;
};

}  // namespace

auto generate_procedure(const Procedure& proc, const Catalog& cat, const RowCounts& counts, std::uint64_t memory_bytes)
    -> GeneratedProcedure {
  return Compiler{proc, cat, counts, memory_bytes}.compile();
}

}  // namespace bytecask::sql
