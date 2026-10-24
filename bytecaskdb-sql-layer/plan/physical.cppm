// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// physical.cppm — the physical plan (docs/planner_design.md, *The physical
// plan and codegen*): a tree of operators the search (search.cppm) builds
// and codegen (codegen.cppm) prints, each with its produce and consume.
// Every id, type and constant name the printer needs is here, so it never
// reads the catalog.
//
// The operators are Empty, the access paths, the joins, Filter, Limit, Aggregate
// and the sinks: Emit for result rows, Insert, Update, Delete. Slice 1 of the
// design: the access paths and choices of the MVP's planner, no sort and no
// hash join. check() enforces what the types can't express.

module;
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.physical;

export import bytecask.sql.logical;

export namespace bytecask::sql::physical {

using logical::ColumnRef;
using logical::Expr;
using Op = ast::Compare::Op;

// One end of a scan, in key order, on the part after the prefix: open (the
// prefix's first or last entry), a value's key part (a parameter, or $n ± k),
// or the key part that holds NULL; as is, or its successor.
struct KeyBound {
  enum class Kind { Open, Value, NullPart };
  Kind kind{Kind::Open};
  std::optional<Expr> value;  // Value
  bool successor{false};
};

struct Range {
  KeyBound lo, hi;
};

// No rows, and nothing read: the input a conjunct always false leaves.
struct Empty {};

// The whole primary key by equality, each part a parameter: at most one row.
struct PointGet {
  std::uint32_t table{0};
  std::vector<Expr> key;
  bool for_update{false};  // each row returned is guarded unchanged
};

// A scan of the table's primary key or of one of its indexes: every entry
// under an equality prefix of parameters, or a range of entries after it;
// forward, or in reverse.
struct Scan {
  std::uint32_t table{0};
  std::uint32_t object{0};                   // the table, or one of its indexes
  std::vector<Expr> prefix;                  // the equality prefix's parameters, in key order
  std::variant<std::monostate, Range> rest;  // nothing more, or a range
  bool reverse{false};
  bool covering{false};    // every column read comes from the entry's key
  bool for_update{false};  // each row returned is guarded unchanged: never a covering scan
};

// An ordered scan whose first ordered part is a nullable ASC part: the key
// order puts its NULLs first, PostgreSQL last for ASC and first for DESC.
// So two ranges: `present`, the part's values (a Range from past the NULL
// part), and the NULL part. Forward: the values, then the NULLs; with
// present.reverse (DESC), the NULLs reversed, then the values reversed.
struct NullSplitScan {
  Scan present;
};

// COUNT answered from the keys: bc_count_range over the scan's range, which
// holds one entry per row counted. An access path that also produces the
// aggregate's row.
struct CountRange {
  Scan scan;
  bool emits{true};  // false under LIMIT 0
};

// MIN answered from the keys: the scan's first entry, `arg` being the ASC
// part after its prefix and its range starting past that part's NULLs.
struct MinFirstEntry {
  Scan scan;
  ColumnRef arg;
  bool emits{true};
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

// An index nested loop: for each outer row, the inner table's row named by
// its whole primary key, each part a parameter or a column of the outer row.
// An outer row whose key column is NULL, or with no inner row, joins nothing.
struct IndexJoin {
  NodePtr outer;
  std::uint32_t inner{0};
  std::vector<Expr> inner_key;  // in the inner primary key's order
};

// The rows every conjunct holds for, run as soon as the row is read. NULL
// on either side of a comparison rejects the row. Never on a DOUBLE or REAL
// column.
struct Filter {
  NodePtr input;
  std::vector<Expr> conjuncts;
};

// The first n rows of Emit's input.
struct Limit {
  NodePtr input;
  std::int64_t n{0};
};

// Result rows: the targets of each row. No target is a row of no columns, as
// PostgreSQL's `SELECT FROM`.
struct Emit {
  NodePtr input;
  std::vector<ColumnRef> targets;  // from the row, or from the entry's key when the scan covers
};

// One aggregate row, of COUNT(*), or COUNT, COUNT(DISTINCT), SUM or MIN of
// its argument's non-NULL values, each held as an int64_t.
struct Aggregate {
  NodePtr input;
  Expr agg;
  bool emits{true};  // false under LIMIT 0
};

// The columns given, in table order, each from a parameter; the rest keep
// the default bc_row_begin writes.
struct Insert {
  std::uint32_t table{0};
  std::vector<std::pair<ColumnRef, Expr>> values;
};

// UPDATE and DELETE write each row their input gives, read from the
// snapshot the statement began on.
struct Update {
  NodePtr input;
  std::uint32_t table{0};
  std::vector<std::pair<ColumnRef, Expr>> set;
};

struct Delete {
  NodePtr input;
  std::uint32_t table{0};
};

struct Node {
  std::variant<Empty, PointGet, Scan, NullSplitScan, CountRange, MinFirstEntry, IndexJoin, Filter, Limit, Emit,
               Aggregate, Insert, Update, Delete>
      op;
};

template <typename T>
[[nodiscard]] auto node(T op) -> NodePtr {
  return std::make_unique<Node>(Node{std::move(op)});
}

// $param + offset as an INTEGER, computed once before anything is read:
// BC_C_RANGE on `table` when it leaves INTEGER, as PostgreSQL's operator
// raises 22003.
struct Computed {
  std::uint32_t param{0};
  std::int64_t offset{0};
  std::uint32_t table{0};

  friend auto operator==(const Computed&, const Computed&) -> bool = default;
};

// A result column as PostgreSQL names and types it.
struct ResultColumn {
  std::string name;
  SqlType sql;
  PhysType type{PhysType::I64};
};

struct Plan {
  NodePtr root;
  std::vector<Catalog::PlanConstant> constants;  // its tables', in the catalog's order
  std::uint64_t catalog_version{0};
  std::vector<PhysType> param_types;
  // The parameters a NULL of which matches no row: the plan ends before it
  // reads anything, with what the sink gives for no rows.
  std::vector<std::uint32_t> null_checks;
  std::vector<Computed> computed;    // what the statement's $n ± k evaluate to
  std::vector<ResultColumn> result;  // empty for a write
};

// $n ± k, as the Computed it evaluates to, compared with a column of `table`.
[[nodiscard]] auto computed_of(const Expr& e, std::uint32_t table) -> Computed {
  auto k = e.args[1].constant;
  return {e.args[0].param, e.kind == Expr::Kind::Add ? k : -k, table};
}

// Checks what the types can't express. Throws std::logic_error naming the
// broken rule: a compiler bug, never a refusal of the statement.
void check(const Plan& p);

}  // namespace bytecask::sql::physical

namespace bytecask::sql::physical {

namespace {

[[noreturn]] void broken(const std::string& rule) { throw std::logic_error{"physical plan: " + rule}; }

class Checker {
public:
  explicit Checker(const Plan& p) : p_{p} {}

  void plan() {
    if (p_.constants.empty()) broken("a plan with no constants");
    if (!p_.root) broken("a plan with no operator");
    for (auto n : p_.null_checks) param(n);
    node(*p_.root, nullptr);
  }

private:
  void param(std::uint32_t n) const {
    if (n >= p_.param_types.size()) broken("a parameter past the statement's");
  }

  void params(const Expr& e) const {
    if (e.kind == Expr::Kind::Param) param(e.param);
    for (const auto& a : e.args) params(a);
  }

  // A NULL parameter matches nothing: the plan must end before it encodes
  // one into a key.
  void null_checked(const Expr& e) const {
    if (e.kind != Expr::Kind::Param) return;
    param(e.param);
    if (std::ranges::find(p_.null_checks, e.param) == p_.null_checks.end()) {
      broken("a compared parameter the plan does not check for NULL");
    }
  }

  // $n ± k is computed before anything is read.
  void operand(const Expr& e, std::uint32_t table) const {
    params(e);
    if (e.kind != Expr::Kind::Add && e.kind != Expr::Kind::Sub) return;
    if (std::ranges::find(p_.computed, computed_of(e, table)) == p_.computed.end()) broken("a $n ± k the plan does not compute");
  }

  void scan(const Scan& s) const {
    if (s.prefix.empty() && std::holds_alternative<std::monostate>(s.rest)) {
      broken("a scan needs an equality prefix or a range");
    }
    for (const auto& e : s.prefix) {
      if (e.kind != Expr::Kind::Param) broken("a scan prefix part that is not a parameter");
      null_checked(e);
    }
    // FOR UPDATE guards each row by its key, read from the row.
    if (s.covering && s.for_update) broken("a FOR UPDATE scan that covers");
    if (const auto* r = std::get_if<Range>(&s.rest)) {
      for (const auto* b : {&r->lo, &r->hi}) {
        if ((b->kind == KeyBound::Kind::Value) != b->value.has_value()) broken("a key bound's value");
        if (!b->value) continue;
        operand(*b->value, s.table);
        null_checked(*b->value);
      }
    }
  }

  // The scan an operator's rows come from, or null.
  static auto scan_below(const Node& n) -> const Scan* {
    if (const auto* s = std::get_if<Scan>(&n.op)) return s;
    if (const auto* s = std::get_if<NullSplitScan>(&n.op)) return &s->present;
    if (const auto* f = std::get_if<Filter>(&n.op)) return scan_below(*f->input);
    if (const auto* l = std::get_if<Limit>(&n.op)) return scan_below(*l->input);
    return nullptr;
  }

  void node(const Node& n, const Node* parent) const {
    std::visit(
        [&](const auto& op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, Empty>) {
            // Reads nothing.
          } else if constexpr (std::is_same_v<T, PointGet>) {
            if (op.key.empty()) broken("a point lookup with no key part");
            for (const auto& e : op.key) null_checked(e);
          } else if constexpr (std::is_same_v<T, Scan>) {
            scan(op);
          } else if constexpr (std::is_same_v<T, NullSplitScan>) {
            scan(op.present);
            const auto* r = std::get_if<Range>(&op.present.rest);
            if (r == nullptr || r->lo.kind != KeyBound::Kind::NullPart || !r->lo.successor) {
              broken("a NULL split whose values do not start past the NULL part");
            }
          } else if constexpr (std::is_same_v<T, CountRange>) {
            scan(op.scan);
          } else if constexpr (std::is_same_v<T, MinFirstEntry>) {
            scan(op.scan);
            if (op.scan.reverse) broken("MIN from the first entry of a reverse scan");
          } else if constexpr (std::is_same_v<T, IndexJoin>) {
            if (op.inner_key.empty()) broken("a join whose inner key has no part");
            for (const auto& e : op.inner_key) null_checked(e);
            node(*op.outer, &n);
          } else if constexpr (std::is_same_v<T, Filter>) {
            for (const auto& c : op.conjuncts) {
              if (c.kind != Expr::Kind::Compare) continue;
              if (c.args[0].kind == Expr::Kind::Column && c.args[0].col.type == PhysType::F64) {
                broken("a filter on a DOUBLE column");
              }
              operand(c.args[1], c.args[0].col.table);
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Limit>) {
            if (parent == nullptr || !std::holds_alternative<Emit>(parent->op)) broken("a LIMIT not under the rows emitted");
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Emit>) {
            const auto* sc = scan_below(*op.input);
            for (const auto& t : op.targets) {
              // The key encoding writes -0.0 as 0.0 and every NaN as one NaN.
              if (sc != nullptr && sc->covering && t.type == PhysType::F64) broken("a DOUBLE target read from a key");
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Aggregate>) {
            if (op.agg.kind != Expr::Kind::Aggregate) broken("an aggregate that is not an aggregate call");
            if ((op.agg.fn != Expr::Fn::Count || op.agg.distinct) && op.agg.args.empty()) {
              broken("SUM, MIN or COUNT(DISTINCT) with no argument");
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Insert>) {
            if (op.values.empty()) broken("an insert that sets no column");
            for (const auto& [col, e] : op.values) params(e);
          } else if constexpr (std::is_same_v<T, Update>) {
            if (op.set.empty()) broken("an update that sets no column");
            for (const auto& [col, e] : op.set) {
              params(e);
              if (e.kind == Expr::Kind::Column && e.col.type != col.type) broken("a column set from a column of another type");
              if ((e.kind == Expr::Kind::Add || e.kind == Expr::Kind::Sub) &&
                  (e.args[0].kind != Expr::Kind::Column || e.args[0].col.type != PhysType::I64 || col.type != PhysType::I64)) {
                broken("column + or - parameter on a type other than an integer");
              }
            }
            node(*op.input, &n);
          } else if constexpr (std::is_same_v<T, Delete>) {
            node(*op.input, &n);
          }
        },
        n.op);
  }

  const Plan& p_;
};

}  // namespace

void check(const Plan& p) { Checker{p}.plan(); }

}  // namespace bytecask::sql::physical
