// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import bytecask.sql.binder;
import bytecask.sql.parser;
import bytecask.sql.rewrite;
import bytecask.sql.text;
import bytecask.sql.tests.fixture;

using namespace bytecask::sql;

namespace {

auto bind_sql(const Catalog& cat, std::string_view sql) -> bound::Bound { return bound::bind(ast::parse(sql), cat); }

auto root(const bound::Bound& b) -> logical::Node& { return *std::get<logical::NodePtr>(b.stmt); }

// The first operator of type T at or below n, the left side of a join first.
template <typename T>
auto find(const logical::Node& n) -> const T* {
  if (const auto* op = std::get_if<T>(&n.op)) return op;
  if (const auto* j = std::get_if<logical::Join>(&n.op)) {
    const auto* left = find<T>(*j->left);
    return left != nullptr ? left : find<T>(*j->right);
  }
  const auto* in = logical::input_of(n);
  return in != nullptr && *in ? find<T>(**in) : nullptr;
}

template <typename T>
auto get(const bound::Bound& b) -> const T& {
  const auto* op = find<T>(root(b));
  REQUIRE(op != nullptr);
  return *op;
}

auto rejected(const Catalog& cat, std::string_view sql) -> std::pair<std::string, std::int32_t> {
  try {
    (void)bind_sql(cat, sql);
  } catch (const ast::SqlError& e) {
    return {e.what(), e.pos()};
  }
  FAIL("accepted: " << sql);
  return {};
}

}  // namespace

TEST_CASE("binder: names become ids, parameters get types from use", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  {
    auto b = bind_sql(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v BYTEA NOT NULL, w BIGINT)");
    auto& s = std::get<CreateTableDdl>(std::get<Ddl>(b.stmt));
    CHECK(s.pk == std::vector<std::string>{"k"});
    REQUIRE(s.columns.size() == 3);
    CHECK(s.columns[1].sql.kind == SqlKind::Bytea);
    CHECK_FALSE(s.columns[1].nullable);
    CHECK(s.columns[2].nullable);
  }
  {
    auto b = bind_sql(cat, "INSERT INTO t VALUES ($1, $2, $3)");
    const auto& s = get<logical::Insert>(b);
    CHECK(s.table->id == 1);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::Bytes, PhysType::I64});
    REQUIRE(s.rows.size() == 1);
    REQUIRE(s.rows[0].size() == 3);
    CHECK(s.rows[0][1]->param == 1);
  }
  {
    // Each VALUES row binds as one INSERT of it, its parameters typed by its columns.
    auto b = bind_sql(cat, "INSERT INTO t (id, qty) VALUES ($1, $2), ($3, $4)");
    const auto& s = get<logical::Insert>(b);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64, PhysType::I64, PhysType::I64});
    REQUIRE(s.rows.size() == 2);
    CHECK(s.rows[1][0]->param == 2);
    CHECK(s.rows[1][2]->param == 3);
    CHECK_FALSE(s.rows[1][1].has_value());
  }
  {
    auto b = bind_sql(cat, "INSERT INTO t (qty, id) VALUES ($1, $2)");
    const auto& s = get<logical::Insert>(b);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
    CHECK(s.rows[0][0]->param == 1);
    CHECK_FALSE(s.rows[0][1].has_value());  // name is not given: its default, NULL
    CHECK(s.rows[0][2]->param == 0);
  }
  {
    auto b = bind_sql(cat, "SELECT id, qty FROM t WHERE id = $1");
    const auto& p = get<logical::Project>(b);
    REQUIRE(p.exprs.size() == 2);
    CHECK(p.exprs[0].expr.col.col == 0);
    CHECK(p.exprs[1].expr.col.col == 2);
    CHECK(p.exprs[1].name == "qty");
    const auto& where = get<logical::Filter>(b).conjuncts;
    REQUIRE(where.size() == 1);
    CHECK(where[0].args[0].col.col == 0);
    CHECK(get<logical::Get>(b).table->name == "t");
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64});
  }
  {
    auto b = bind_sql(cat, "SELECT id FROM t WHERE $1 <= name AND name < $2");
    const auto& w = get<logical::Filter>(b).conjuncts;
    CHECK(w[0].op == ast::Compare::Op::Ge);  // normalised: column on the left
    CHECK(w[0].args[0].kind == logical::Expr::Kind::Column);
    CHECK(w[0].type.kind == SqlKind::Boolean);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::Bytes, PhysType::Bytes});
  }
  {
    auto b = bind_sql(cat, "UPDATE t SET qty = qty - $2 WHERE id = $1");
    const auto& s = get<logical::Update>(b);
    REQUIRE(s.set.size() == 1);
    CHECK(s.set[0].first.col == 2);
    CHECK(s.set[0].second.kind == logical::Expr::Kind::Sub);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
  }
  {
    auto b = bind_sql(cat, "DELETE FROM t WHERE id = $1");
    CHECK(get<logical::Delete>(b).table->name == "t");
    CHECK(get<logical::Filter>(b).conjuncts.size() == 1);
  }
}

TEST_CASE("binder: anything outside the scope is rejected with a message and position", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  //                       0         1         2         3         4
  //                       01234567890123456789012345678901234567890123
  CHECK(rejected(cat, "SELECT id FROM nope WHERE id = $1").second == 0);
  CHECK(rejected(cat, "SELECT nope FROM t WHERE id = $1") == std::pair<std::string, std::int32_t>{
                                                                   "column \"nope\" does not exist", 7});
  CHECK(rejected(cat, "SELECT id FROM t WHERE name = $1 AND qty = $1").second == 43);  // $1 bytea, then bigint
  {
    // One $n compared with two columns of one physical type but two SQL
    // types: refused, so its pre-plan range check covers every key it builds.
    sql_test::Fixture c2;
    c2.ddl(std::get<Ddl>(bind_sql(c2.cat(), "CREATE TABLE dr (a DOUBLE PRECISION, b REAL, i INTEGER, j BIGINT, "
                                            "v VARCHAR(4), w TEXT, d2 NUMERIC(10, 2), d4 NUMERIC(10, 4), "
                                            "e2 NUMERIC(12, 2), n0 NUMERIC(10, 0), s SMALLINT, PRIMARY KEY (a, b))")
                             .stmt));
    auto refusal = [&](std::string_view where) {
      return rejected(c2.cat(), "SELECT a FROM dr WHERE " + std::string(where)).first;
    };
    CHECK(refusal("a = $1 AND b = $1") == "parameter $1 is compared with a (double precision) and b (real)");
    CHECK(refusal("a = $1 AND b = $2 AND i = $3 AND j = $3") ==
          "parameter $3 is compared with i (integer) and j (bigint)");
    // A DECIMAL's scale says what number its i64 is: two scales, or a
    // DECIMAL and an integer, are two types.
    CHECK(refusal("a = $1 AND b = $2 AND d2 = $3 AND d4 = $3") ==
          "parameter $3 is compared with d2 (numeric(10,2)) and d4 (numeric(10,4))");
    CHECK(refusal("a = $1 AND b = $2 AND d2 = $3 AND j = $3") ==
          "parameter $3 is compared with d2 (numeric(10,2)) and j (bigint)");
    // A modifier is no other type: VARCHAR(4) and TEXT, or DECIMAL(10, 2)
    // and DECIMAL(12, 2), compare one $n.
    CHECK_NOTHROW(bind_sql(c2.cat(), "SELECT a FROM dr WHERE a = $1 AND b = $2 AND d2 = $3 AND e2 = $3"));
    // At scale 0 a DECIMAL's integer is the number itself: a BIGINT.
    CHECK_NOTHROW(bind_sql(c2.cat(), "SELECT a FROM dr WHERE a = $1 AND b = $2 AND n0 = $3 AND j = $3"));
    // Only a BIGINT's: not an INTEGER's, a SMALLINT's or a scale 2 DECIMAL's.
    CHECK(refusal("a = $1 AND b = $2 AND n0 = $3 AND i = $3") ==
          "parameter $3 is compared with n0 (numeric(10,0)) and i (integer)");
    CHECK(refusal("a = $1 AND b = $2 AND n0 = $3 AND s = $3") ==
          "parameter $3 is compared with n0 (numeric(10,0)) and s (smallint)");
    CHECK(refusal("a = $1 AND b = $2 AND n0 = $3 AND d2 = $3") ==
          "parameter $3 is compared with n0 (numeric(10,0)) and d2 (numeric(10,2))");
    CHECK_NOTHROW(bind_sql(c2.cat(), "SELECT a FROM dr WHERE a = $1 AND b = $2 AND v = $3 AND w = $3"));
  }
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = $2").first ==
        "could not determine data type of parameter $1");
  // A SET value is any value expression, $n typed by its operator.
  CHECK(rejected(cat, "UPDATE t SET name = name - $1 WHERE id = $2").first == "operator does not exist: bytea - unknown");
  {
    // A primary key column is set as any other: the runtime moves the row.
    auto b = bind_sql(cat, "UPDATE t SET id = $1 WHERE id = $2");
    const auto& u = get<logical::Update>(b);
    REQUIRE(u.set.size() == 1);
    CHECK(u.set[0].first.col == 0);
  }
  CHECK(rejected(cat, "INSERT INTO t VALUES ($1, $2, $3, $4)").first == "INSERT has more expressions than target columns");
  CHECK(rejected(cat, "INSERT INTO t (id, qty) VALUES ($1)").first == "INSERT has 1 values for 2 columns");
  // CHAR(n) is a type; a CHAR of no length, which PostgreSQL leaves unbounded, and of length 0 are not.
  CHECK(rejected(cat, "CREATE TABLE u (k BPCHAR)") == std::pair<std::string, std::int32_t>{"CHAR takes one positive length", 16});
  CHECK(rejected(cat, "CREATE TABLE u (k CHAR(0))").first == "length for type char must be at least 1");
  CHECK(rejected(cat, "SELECT CAST('a' AS CHAR(5))").first.find("CHAR(n)") != std::string::npos);
  // No primary key: the catalog gives the table a row id.
  CHECK(std::get<CreateTableDdl>(std::get<Ddl>(bind_sql(cat, "CREATE TABLE u (k BIGINT)").stmt)).pk.empty());
  {
    // An expression operand is the select list's: $1 typed by qty, evaluated, never a key.
    auto b = bind_sql(cat, "SELECT id FROM t WHERE id = $1 + qty");
    const auto& w = get<logical::Filter>(b).conjuncts;
    REQUIRE(w.size() == 1);
    CHECK(w[0].args[1].kind == logical::Expr::Kind::Add);
    CHECK(b.params.at(0).type == SqlType{SqlKind::BigInt});
  }
  // Two columns of one table PostgreSQL has no operator for: its message, the operator as written.
  CHECK(rejected(cat, "SELECT id FROM t WHERE name <> qty").first == "operator does not exist: bytea <> bigint");
}

TEST_CASE("binder: an assigned constant out of its column's type is refused, the first in column order", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(
      bind_sql(f.cat(), "CREATE TABLE v (k INTEGER PRIMARY KEY, s SMALLINT, c VARCHAR(4), d NUMERIC(6,2))").stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  CHECK(state("INSERT INTO v VALUES (1, 2, 'abcd', 1.5)") == "bound");
  CHECK(state("INSERT INTO v VALUES (1, 40000, 'abcd', 1.5)") == "22003 smallint out of range");
  CHECK(state("INSERT INTO v VALUES (1, 2, 'abcde', 1.5)") == "22001 value too long for type character varying(4)");
  CHECK(state("INSERT INTO v VALUES (1, 2, 'abcd', 12345.5)") == "22003 numeric field overflow");
  // As PostgreSQL: the string's column comes first, so its error is the one.
  CHECK(state("INSERT INTO v VALUES (1, 2, 'abcde', -1000000000000000000)") ==
        "22001 value too long for type character varying(4)");
  CHECK(state("INSERT INTO v (d, c) VALUES (12345.5, 'abcde')") == "22003 numeric field overflow");
  CHECK(state("UPDATE v SET c = 'abcde' WHERE k = 1") == "22001 value too long for type character varying(4)");
}

TEST_CASE("binder: constants are folded to the type they meet, as PostgreSQL types them", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE lt (k INTEGER PRIMARY KEY, s SMALLINT, d NUMERIC(6,2), "
                                         "t TEXT, b BOOLEAN, r REAL, dt DATE, g BIGINT)")
                          .stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  // The constants, after the statement's own parameters: $1 is the caller's,
  // the literal $2, which no client sees.
  auto b = bind_sql(cat, "SELECT k FROM lt WHERE s = $1 AND k = 5");
  CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
  CHECK(b.params.size() == 1);
  CHECK(b.param_sql.size() == 1);
  CHECK(b.fixed == std::vector<Datum>{std::int64_t{5}});

  // Alone in a select list: PostgreSQL's types, named ?column?.
  auto out = [&](std::string_view sql) {
    auto bound = bind_sql(cat, sql);
    const auto* p = find<logical::Project>(root(bound));
    REQUIRE(p != nullptr);
    REQUIRE(p->exprs.size() == 1);
    CHECK(p->exprs[0].name == "?column?");
    REQUIRE(bound.fixed.size() == 1);
    return std::pair{p->exprs[0].expr.type, bound.fixed[0]};
  };
  CHECK(out("SELECT 5") == std::pair{SqlType{SqlKind::Integer}, Datum{std::int64_t{5}}});
  CHECK(out("SELECT 2147483648") == std::pair{SqlType{SqlKind::BigInt}, Datum{std::int64_t{2147483648}}});
  CHECK(out("SELECT -7.80") == std::pair{SqlType{SqlKind::Decimal, 0, 0, 2}, Datum{std::int64_t{-780}}});
  CHECK(out("SELECT .2E+2") == std::pair{SqlType{SqlKind::Decimal, 0, 0, 0}, Datum{std::int64_t{20}}});
  CHECK(out("SELECT 2.2E-2") == std::pair{SqlType{SqlKind::Decimal, 0, 0, 3}, Datum{std::int64_t{22}}});
  CHECK(out("SELECT 'a'") == std::pair{SqlType{SqlKind::Text}, Datum{sql_test::bytes("a")}});
  CHECK(out("SELECT NULL") == std::pair{SqlType{SqlKind::Text}, Datum{}});
  CHECK(out("SELECT TRUE") == std::pair{SqlType{SqlKind::Boolean}, Datum{true}});
  // Folded: + and - as PostgreSQL's operators type and check them.
  CHECK(out("SELECT 1 - 2 - 3") == std::pair{SqlType{SqlKind::Integer}, Datum{std::int64_t{-4}}});
  CHECK(out("SELECT -(1 + 2)") == std::pair{SqlType{SqlKind::Integer}, Datum{std::int64_t{-3}}});
  CHECK(out("SELECT 2147483647 + 2147483648") == std::pair{SqlType{SqlKind::BigInt}, Datum{std::int64_t{4294967295}}});
  CHECK(out("SELECT 1 + 1.5") == std::pair{SqlType{SqlKind::Decimal, 0, 0, 1}, Datum{std::int64_t{25}}});
  CHECK(out("SELECT 1 + NULL") == std::pair{SqlType{SqlKind::Integer}, Datum{}});
  CHECK(state("SELECT 2147483647 + 1") == "22003");
  CHECK(state("SELECT -2147483648 - 1") == "22003");
  CHECK(state("SELECT 9223372036854775807 + 1") == "22003");
  CHECK(state("SELECT TRUE + 1") == "42883");
  // Not yet: PostgreSQL runs each.
  CHECK(rejected(cat, "SELECT 1e100").first == "a numeric literal of more than 18 digits is not supported yet");
  CHECK(rejected(cat, "SELECT 99999999999999999999").first == "a numeric literal of more than 18 digits is not supported yet");
  CHECK(rejected(cat, "SELECT 'a' + 1").first == "a string literal in arithmetic is not supported yet");
  CHECK(rejected(cat, "SELECT COUNT(*), 5 FROM lt").first ==
        "a constant in the select list of an aggregated query is not supported yet");
  CHECK(rejected(cat, "SELECT 1 WHERE 1 = 1").first ==
        "a SELECT without FROM with WHERE, GROUP BY, HAVING, ORDER BY or FOR UPDATE is not supported yet");
  CHECK(state("SELECT k") == "42703");  // a column of no table

  // Assigned: PostgreSQL's assignment casts.
  auto assigned = [&](std::string_view col, std::string_view lit) {
    auto bound = bind_sql(cat, "INSERT INTO lt (k, " + std::string{col} + ") VALUES ($1, " + std::string{lit} + ")");
    REQUIRE(bound.fixed.size() == 1);
    return bound.fixed[0];
  };
  CHECK(assigned("s", "2.5") == Datum{std::int64_t{3}});  // half away from zero
  CHECK(assigned("s", "-2.5") == Datum{std::int64_t{-3}});
  CHECK(assigned("d", "1.005") == Datum{std::int64_t{101}});
  CHECK(assigned("d", "7") == Datum{std::int64_t{700}});
  CHECK(assigned("t", "2.2E-2") == Datum{sql_test::bytes("0.022")});
  CHECK(assigned("t", "5") == Datum{sql_test::bytes("5")});
  CHECK(assigned("t", "TRUE") == Datum{sql_test::bytes("true")});
  CHECK(assigned("r", "0.1") == Datum{static_cast<double>(0.1F)});  // rounded once, to a float
  CHECK(assigned("dt", "'1970-01-02'") == Datum{std::int64_t{1}});  // read as the column's type
  CHECK(assigned("b", "'yes'") == Datum{true});
  CHECK(assigned("t", "NULL") == Datum{});
  CHECK(state("INSERT INTO lt (k, b) VALUES ($1, 1)") == "42804");
  CHECK(state("INSERT INTO lt (k, s) VALUES ($1, '1.5')") == "22P02");
  CHECK(state("INSERT INTO lt (k, dt) VALUES ($1, 'not a date')") == "22007");
  CHECK(state("INSERT INTO lt (k, d) VALUES ($1, 99999999999999999.5)") == "22003");  // past 18 digits at the scale
  CHECK(state("INSERT INTO lt (k, d) VALUES ($1, 9999999999999999999.5)") == "42601");  // a literal past 18: not yet
  // An UPDATE's constant is checked against the column now, as PostgreSQL
  // folds its cast when it plans the statement, whether or not a row matches.
  CHECK(state("UPDATE lt SET s = 40000 WHERE k = $1") == "22003");
  CHECK(state("UPDATE lt SET s = 32767 WHERE k = $1") == "bound");

  // Compared: only a constant the column's type holds exactly.
  auto compared = [&](std::string_view where) {
    auto bound = bind_sql(cat, "SELECT k FROM lt WHERE " + std::string{where});
    REQUIRE(bound.fixed.size() == 1);
    return bound.fixed[0];
  };
  CHECK(compared("s = 7") == Datum{std::int64_t{7}});
  CHECK(compared("7 < s") == Datum{std::int64_t{7}});
  CHECK(compared("s = 7.0") == Datum{std::int64_t{7}});
  CHECK(compared("d = 1.5") == Datum{std::int64_t{150}});
  CHECK(compared("s = '7'") == Datum{std::int64_t{7}});
  CHECK(compared("t = 'x'") == Datum{sql_test::bytes("x")});
  CHECK(compared("s = NULL") == Datum{});
  CHECK(compared("r = 16777216") == Datum{16777216.0});
  CHECK(rejected(cat, "SELECT k FROM lt WHERE s = 40000").first ==
        "comparing s (smallint) with a constant its type does not hold exactly is not supported yet");
  CHECK(rejected(cat, "SELECT k FROM lt WHERE s = 1.5").first ==
        "comparing s (smallint) with a constant its type does not hold exactly is not supported yet");
  CHECK(rejected(cat, "SELECT k FROM lt WHERE d = 1.005").first ==
        "comparing d (numeric(6,2)) with a constant its type does not hold exactly is not supported yet");
  CHECK(rejected(cat, "SELECT k FROM lt WHERE r = 0.1").first ==
        "comparing r (real) with a constant its type does not hold exactly is not supported yet");
  CHECK(rejected(cat, "SELECT k FROM lt WHERE d = '1.005'").first ==
        "comparing d (numeric(6,2)) with a constant its type does not hold exactly is not supported yet");
  CHECK(state("SELECT k FROM lt WHERE d = 'x'") == "22P02");
  CHECK(state("SELECT k FROM lt WHERE t = 5") == "42883");
  // In a join, and in its select list.
  auto joined = bind_sql(cat, "SELECT lt.k, 1 FROM lt, t WHERE lt.g = t.id AND t.name = 'n' AND lt.s = 3");
  CHECK(joined.fixed == std::vector<Datum>{sql_test::bytes("n"), std::int64_t{3}, std::int64_t{1}});  // in binding order
  CHECK(state("SELECT k FROM lt WHERE s = '1.5'") == "22P02");
  CHECK(state("SELECT k FROM lt WHERE s = '40000'") == "22003");
  // HAVING: a grouped column or an aggregate with a constant.
  CHECK(bind_sql(cat, "SELECT s, COUNT(*) FROM lt GROUP BY s HAVING COUNT(*) > 1").fixed ==
        std::vector<Datum>{std::int64_t{1}});
  CHECK(bind_sql(cat, "SELECT s FROM lt GROUP BY s HAVING 2 <= s").fixed == std::vector<Datum>{std::int64_t{2}});
}

TEST_CASE("binder: comparisons of constants fold to a BOOLEAN; a string literal casts to a type", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE ct (k INTEGER PRIMARY KEY, dt DATE, t TEXT)").stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  auto out = [&](std::string_view sql) {
    auto bound = bind_sql(cat, sql);
    const auto* p = find<logical::Project>(root(bound));
    REQUIRE(p != nullptr);
    REQUIRE(p->exprs.size() == 1);
    REQUIRE(bound.fixed.size() == 1);
    return std::tuple{p->exprs[0].name, p->exprs[0].expr.type, bound.fixed[0]};
  };
  auto truth = [&](std::string_view sql) {
    auto [name, type, v] = out(sql);
    CHECK(name == "?column?");
    CHECK(type == SqlType{SqlKind::Boolean});
    return v;
  };
  const Datum yes{true}, no{false}, null{};
  CHECK(truth("SELECT 3 < 5") == yes);
  CHECK(truth("SELECT 3 <> 5") == yes);
  CHECK(truth("SELECT 3 != 3") == no);
  CHECK(truth("SELECT 3 = 3.00") == yes);  // numbers compare exactly, whatever their scales
  CHECK(truth("SELECT 3.7 >= 5.2") == no);
  CHECK(truth("SELECT 9223372036854775807 > 99999999999999999.5") == yes);
  CHECK(truth("SELECT 'foo' < 'bar'") == no);
  CHECK(truth("SELECT 'B' < 'a'") == yes);  // bytewise: COLLATE "C"
  CHECK(truth("SELECT 'ab' < 'abc'") == yes);
  CHECK(truth("SELECT 1 < '2'") == yes);  // the string read as the other side's type
  CHECK(truth("SELECT 1.5 < '1.55'") == yes);
  CHECK(truth("SELECT TRUE > FALSE") == yes);
  CHECK(truth("SELECT NULL = NULL") == null);
  CHECK(truth("SELECT 1 = NULL") == null);
  CHECK(truth("SELECT DATE '2016-03-26' < DATE '2016-03-27'") == yes);
  CHECK(truth("SELECT DATE '2016-03-26' = '2016-03-26'") == yes);
  CHECK(truth("SELECT TIME '01:02:03' >= TIME '01:02:03'") == yes);
  CHECK(truth("SELECT CAST('NaN' AS DOUBLE PRECISION) > 1") == yes);  // NaN above every number
  CHECK(state("SELECT 1 < 'x'") == "22P02");
  CHECK(state("SELECT TRUE = 1") == "42883");
  CHECK(state("SELECT 'a'::text = 1") == "42883");
  CHECK(rejected(cat, "SELECT DATE '2016-03-26' < TIMESTAMP '2016-03-26 00:00:00'").first ==
        "comparing a date with a timestamp without time zone is not supported yet");
  CHECK(rejected(cat, "SELECT k + k, COUNT(*) FROM ct").first ==
        "column \"ct.k\" must appear in the GROUP BY clause or be used in an aggregate function");

  // A cast of a string literal: read as the type, named by it.
  CHECK(out("SELECT DATE '1970-01-02'") == std::tuple{std::string{"date"}, SqlType{SqlKind::Date}, Datum{std::int64_t{1}}});
  CHECK(out("SELECT '1970-01-02'::date") == out("SELECT DATE '1970-01-02'"));
  CHECK(out("SELECT CAST('5' AS INTEGER)") == std::tuple{std::string{"int4"}, SqlType{SqlKind::Integer}, Datum{std::int64_t{5}}});
  CHECK(out("SELECT CAST('1.005' AS NUMERIC(6,2))") ==
        std::tuple{std::string{"numeric"}, SqlType{SqlKind::Decimal, 0, 6, 2}, Datum{std::int64_t{101}}});
  CHECK(out("SELECT CAST(NULL AS DATE)") == std::tuple{std::string{"date"}, SqlType{SqlKind::Date}, Datum{}});
  CHECK(state("SELECT CAST('40000' AS SMALLINT)") == "22003");
  CHECK(state("SELECT CAST('12345.678' AS NUMERIC(6,2))") == "22003");
  CHECK(state("SELECT CAST('x' AS INTEGER)") == "22P02");
  CHECK(state("SELECT DATE '2016-02-30'") == "22008");
  // A typed constant cast as PostgreSQL's cast functions cast it: numbers
  // rounded (a numeric half away from zero, a float to the nearest even) and
  // range checked, a BOOLEAN to an INTEGER and back, anything to TEXT; a pair
  // with no cast is 42846, its NULL too.
  CHECK(out("SELECT CAST(1 AS TEXT)") == std::tuple{std::string{"text"}, SqlType{SqlKind::Text}, Datum{sql_test::bytes("1")}});
  CHECK(out("SELECT CAST(-2.5 AS INTEGER)") == std::tuple{std::string{"int4"}, SqlType{SqlKind::Integer}, Datum{std::int64_t{-3}}});
  CHECK(out("SELECT CAST(CAST('2.5' AS DOUBLE PRECISION) AS BIGINT)") ==
        std::tuple{std::string{"int8"}, SqlType{SqlKind::BigInt}, Datum{std::int64_t{2}}});
  CHECK(out("SELECT CAST(7 AS NUMERIC(5,2))") ==
        std::tuple{std::string{"numeric"}, SqlType{SqlKind::Decimal, 0, 5, 2}, Datum{std::int64_t{700}}});
  CHECK(out("SELECT CAST(TRUE AS INTEGER)") == std::tuple{std::string{"int4"}, SqlType{SqlKind::Integer}, Datum{std::int64_t{1}}});
  CHECK(out("SELECT CAST(0 AS BOOLEAN)") == std::tuple{std::string{"bool"}, SqlType{SqlKind::Boolean}, Datum{false}});
  CHECK(state("SELECT CAST(40000 AS SMALLINT)") == "22003");
  CHECK(state("SELECT CAST(CAST(32767 AS SMALLINT) + CAST(1 AS SMALLINT) AS INTEGER)") == "22003");
  CHECK(state("SELECT CAST(CAST('9.3e18' AS DOUBLE PRECISION) AS BIGINT)") == "22003");
  CHECK(state("SELECT CAST(99.99 AS NUMERIC(3,1))") == "22003");
  CHECK(state("SELECT CAST(1 AS DATE)") == "42846");
  CHECK(state("SELECT CAST(CAST(NULL AS INTEGER) AS DATE)") == "42846");
  CHECK(state("SELECT CAST(CAST(5 AS BIGINT) AS BOOLEAN)") == "42846");
  // %, unary minus and abs of constants: of the operand's type, its range checked.
  CHECK(out("SELECT -7 % 3") == std::tuple{std::string{"?column?"}, SqlType{SqlKind::Integer}, Datum{std::int64_t{-1}}});
  CHECK(out("SELECT -7.25 % 2.1") == std::tuple{std::string{"?column?"}, SqlType{SqlKind::Decimal, 0, 0, 2}, Datum{std::int64_t{-95}}});
  CHECK(out("SELECT abs(CAST(-5 AS SMALLINT))") == std::tuple{std::string{"abs"}, SqlType{SqlKind::SmallInt}, Datum{std::int64_t{5}}});
  CHECK(state("SELECT 1 % 0") == "22012");
  CHECK(state("SELECT -CAST(-32768 AS SMALLINT)") == "22003");
  CHECK(state("SELECT abs(CAST(-2147483648 AS INTEGER))") == "22003");
  CHECK(state("SELECT abs(TRUE)") == "42883");
  CHECK(state("SELECT CAST(1.5 AS DOUBLE PRECISION) % 2") == "42883");
  CHECK(rejected(cat, "SELECT CAST(CAST('1.5' AS DOUBLE PRECISION) AS TEXT)").first ==
        "a cast of a floating-point value to text is not supported yet");
  // A date, time or timestamp cast to another type.
  auto text = [&](std::string_view sql) { return std::get<Bytes>(std::get<2>(out(sql))); };
  CHECK(text("SELECT CAST(CAST('2016-03-26' AS DATE) AS VARCHAR)") == sql_test::bytes("2016-03-26"));
  CHECK(text("SELECT CAST(CAST('01:02:03' AS TIME) AS VARCHAR)") == sql_test::bytes("01:02:03"));
  CHECK(text("SELECT CAST(CAST('2016-03-26 01:02:03' AS TIMESTAMP) AS VARCHAR)") == sql_test::bytes("2016-03-26 01:02:03"));
  CHECK(out("SELECT CAST(CAST('2016-03-26 01:02:03' AS TIMESTAMP) AS DATE)") ==
        std::tuple{std::string{"date"}, SqlType{SqlKind::Date}, Datum{std::int64_t{16886}}});
  CHECK(out("SELECT CAST(CAST('2016-03-26 01:02:03' AS TIMESTAMP) AS TIME)") ==
        std::tuple{std::string{"time"}, SqlType{SqlKind::Time}, Datum{std::int64_t{3'723'000'000}}});
  CHECK(out("SELECT CAST(TIMESTAMP '1969-12-31 23:00:00' AS DATE)") ==  // floor, not truncation
        std::tuple{std::string{"date"}, SqlType{SqlKind::Date}, Datum{std::int64_t{-1}}});
  CHECK(out("SELECT CAST(DATE '1970-01-02' AS TIMESTAMP)") ==
        std::tuple{std::string{"timestamp"}, SqlType{SqlKind::Timestamp}, Datum{std::int64_t{86'400'000'000}}});
  CHECK(out("SELECT CAST(CAST('01:02:03' AS TIME) AS TIME)") == out("SELECT TIME '01:02:03'") );
  CHECK(out("SELECT CAST(CAST(NULL AS DATE) AS TEXT)") == std::tuple{std::string{"text"}, SqlType{SqlKind::Text}, Datum{}});
  CHECK(state("SELECT CAST(CAST('01:02:03' AS TIME) AS TIMESTAMP)") == "42846");  // PostgreSQL has no such cast
  CHECK(state("SELECT CAST(CAST('01:02:03' AS TIME) AS DATE)") == "42846");
  CHECK(state("SELECT CAST(DATE '2016-03-26' AS TIME)") == "42846");
  CHECK(state("SELECT CAST(DATE '5000000-01-01' AS TIMESTAMP)") == "22008");
  CHECK(rejected(cat, "SELECT CAST('abc' AS VARCHAR(2))").first == "a cast to VARCHAR(n) is not supported yet");
  // Where a typed constant meets a column.
  CHECK(bind_sql(cat, "INSERT INTO ct VALUES ($1, DATE '1970-01-03', DATE '1970-01-03')").fixed ==
        std::vector<Datum>{std::int64_t{2}, sql_test::bytes("1970-01-03")});
  CHECK(bind_sql(cat, "SELECT k FROM ct WHERE dt >= '1970-01-03'::date").fixed == std::vector<Datum>{std::int64_t{2}});
  // PostgreSQL's assignment cast, folded (#904 S1).
  CHECK(bind_sql(cat, "INSERT INTO ct (k, dt) VALUES ($1, TIMESTAMP '2020-01-02 10:00:00')").fixed.back() ==
        Datum{std::int64_t{18263}});
}

TEST_CASE("binder: SELECT DISTINCT is a GROUP BY of the selected columns", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  auto b = bind_sql(cat, "SELECT DISTINCT qty, name FROM t WHERE id > $1 ORDER BY name LIMIT 3");
  const auto& agg = get<logical::Aggregate>(b);
  CHECK(agg.grouped);
  REQUIRE(agg.group_by.size() == 2);
  CHECK(agg.group_by[0].col.col == f.cat().table({kPublicSchema, "t"})->column("qty")->id);
  CHECK(agg.group_by[1].col.col == f.cat().table({kPublicSchema, "t"})->column("name")->id);
  CHECK(agg.aggs.size() == 2);
  CHECK(find<logical::Sort>(root(b)) != nullptr);
  CHECK(find<logical::Limit>(root(b)) != nullptr);
  // Over a query already grouped by what it selects: nothing more.
  auto grouped = bind_sql(cat, "SELECT DISTINCT qty, COUNT(*) FROM t GROUP BY qty");
  CHECK(get<logical::Aggregate>(grouped).group_by.size() == 1);
  CHECK(state("SELECT DISTINCT COUNT(*) FROM t") == "bound");
  // PostgreSQL's refusals, in its order, and what is not yet.
  CHECK(state("SELECT DISTINCT qty FROM t ORDER BY name") == "42P10");
  CHECK(state("SELECT DISTINCT qty FROM t FOR UPDATE") == "0A000");
  CHECK(rejected(cat, "SELECT DISTINCT COUNT(*) FROM t GROUP BY qty").first ==
        "SELECT DISTINCT of groups it does not select every GROUP BY column of is not supported yet");
  // An item that is no column is a key computed per row (#904 G1); a
  // constant one too.
  auto constant = bind_sql(cat, "SELECT DISTINCT qty, 5 FROM t");
  REQUIRE(get<logical::Aggregate>(constant).group_by.size() == 2);
  CHECK(get<logical::Aggregate>(constant).group_by[1].kind == logical::Expr::Kind::Param);
  CHECK(get<logical::Aggregate>(constant).aggs[1].expr.kind == logical::Expr::Kind::GroupKey);
  CHECK(rejected(cat, "SELECT DISTINCT ON (qty) qty FROM t").first == "DISTINCT ON is not supported");
}

TEST_CASE("binder: JOIN USING and NATURAL JOIN compare their columns by =, each named alone as one", "[sql][binder]") {
  sql_test::Fixture f;
  for (auto ddl : {"CREATE TABLE l (k INTEGER PRIMARY KEY, a INTEGER, b TEXT)",
                   "CREATE TABLE r (j INTEGER PRIMARY KEY, a INTEGER, b TEXT, c BIGINT)"}) {
    f.ddl(std::get<Ddl>(bind_sql(f.cat(), ddl).stmt));
  }
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  const auto* l = cat.table({kPublicSchema, "l"});
  const auto* r = cat.table({kPublicSchema, "r"});
  auto equals = [&](std::string_view sql) {
    auto b = bind_sql(cat, sql);
    std::vector<std::string> out;
    for (const auto& c : get<logical::Filter>(b).conjuncts) {
      REQUIRE(c.kind == logical::Expr::Kind::Compare);
      CHECK(c.args[0].col.table == l->id);
      CHECK(c.args[1].col.table == r->id);
      out.push_back(l->column(c.args[0].col.col)->name + "=" + r->column(c.args[1].col.col)->name);
    }
    return out;
  };
  CHECK(equals("SELECT a FROM l JOIN r USING (a)") == std::vector<std::string>{"a=a"});
  CHECK(equals("SELECT a, b FROM l JOIN r USING (b, a)") == std::vector<std::string>{"b=b", "a=a"});
  CHECK(equals("SELECT k FROM l NATURAL JOIN r") == std::vector<std::string>{"a=a", "b=b"});  // the shared names, in l's order
  CHECK(equals("SELECT foo.a FROM l JOIN r USING (a) AS foo").size() == 1);
  // A USING column alone is the left side's; the others stay ambiguous.
  auto merged = bind_sql(cat, "SELECT a FROM l JOIN r USING (a)");
  CHECK(get<logical::Project>(merged).exprs[0].expr.col.table == l->id);
  CHECK(state("SELECT b FROM l JOIN r USING (a)") == "42702");
  CHECK(state("SELECT foo.b FROM l JOIN r USING (a) AS foo") == "42703");
  CHECK(rejected(cat, "SELECT k FROM l JOIN r USING (c)").first ==
        "column \"c\" specified in USING clause does not exist in left table");
  CHECK(rejected(cat, "SELECT k FROM l JOIN r USING (k)").first ==
        "column \"k\" specified in USING clause does not exist in right table");
  CHECK(state("SELECT k FROM l JOIN r USING (a, a)") == "42701");
  // FULL JOIN USING: the left arm's merged column is l's, the right arm's r's,
  // which is COALESCE(l.a, r.a) row by row.
  auto full = bind_sql(cat, "SELECT a FROM l FULL JOIN r USING (a)");
  const auto& set = std::get<logical::SetOp>(root(full).op);
  CHECK(find<logical::Project>(*set.left)->exprs[0].expr.col.table == l->id);
  CHECK(find<logical::Project>(*set.right)->exprs[0].expr.col.table == r->id);
}

TEST_CASE("binder: LEFT and RIGHT JOIN are a left outer join, ON its predicate, WHERE above it", "[sql][binder]") {
  sql_test::Fixture f;
  for (auto ddl : {"CREATE TABLE l (k INTEGER PRIMARY KEY, a INTEGER NOT NULL)",
                   "CREATE TABLE r (j INTEGER PRIMARY KEY, a INTEGER NOT NULL)"}) {
    f.ddl(std::get<Ddl>(bind_sql(f.cat(), ddl).stmt));
  }
  const auto& cat = f.cat();
  const auto* l = cat.table({kPublicSchema, "l"});
  const auto* r = cat.table({kPublicSchema, "r"});
  auto table_of = [](const logical::Node& n) { return std::get<logical::Get>(n.op).table->id; };
  for (auto sql : {"SELECT l.k, r.j FROM l LEFT JOIN r ON l.a = r.a AND r.j > 1 WHERE l.k < 5",
                   "SELECT l.k, r.j FROM r RIGHT JOIN l ON l.a = r.a AND r.j > 1 WHERE l.k < 5"}) {
    INFO(sql);
    auto b = bind_sql(cat, sql);
    const auto& j = get<logical::Join>(b);
    CHECK(j.left_outer);
    CHECK(table_of(*j.left) == l->id);  // RIGHT JOIN's preserved side becomes the left
    CHECK(table_of(*j.right) == r->id);
    CHECK(j.predicate.size() == 2);
    CHECK(get<logical::Filter>(b).conjuncts.size() == 1);
    // Above the join r's columns may be NULL, declared NOT NULL or not.
    const auto& out = get<logical::Project>(b).exprs;
    CHECK_FALSE(out[0].expr.col.nullable);
    CHECK(out[1].expr.col.nullable);
  }
  CHECK_FALSE(get<logical::Join>(bind_sql(cat, "SELECT l.k FROM l JOIN r ON l.a = r.a")).left_outer);
  CHECK(get<logical::Join>(bind_sql(cat, "SELECT l.k FROM l LEFT JOIN r USING (a)")).left_outer);
}

TEST_CASE("binder: FULL JOIN is a UNION ALL of a left outer join and an anti-join arm, WHERE above each", "[sql][binder]") {
  sql_test::Fixture f;
  for (auto ddl : {"CREATE TABLE l (k INTEGER PRIMARY KEY, a INTEGER NOT NULL)",
                   "CREATE TABLE r (j INTEGER PRIMARY KEY, a INTEGER NOT NULL)", "CREATE TABLE n (a INTEGER)",
                   "CREATE VIEW gv AS SELECT a, count(*) AS c FROM l GROUP BY a"}) {
    f.ddl(std::get<Ddl>(bind_sql(f.cat(), ddl).stmt));
  }
  const auto& cat = f.cat();
  const auto* l = cat.table({kPublicSchema, "l"});
  const auto* r = cat.table({kPublicSchema, "r"});
  auto table_of = [](const logical::Node& n) { return std::get<logical::Get>(n.op).table->id; };
  auto b = bind_sql(cat, "SELECT l.k, r.j FROM l FULL JOIN r ON l.a = r.a AND r.j > 1 WHERE l.k < 5 OR r.j < 5");
  const auto& set = std::get<logical::SetOp>(root(b).op);
  CHECK(set.kind == logical::SetOp::Kind::Union);
  CHECK(set.all);
  CHECK(set.outputs.size() == 2);
  CHECK(set.hidden.empty());
  // The left arm: every row of l, r NULL-extended, the WHERE above.
  const auto& lj = *find<logical::Join>(*set.left);
  CHECK(lj.left_outer);
  CHECK(table_of(*lj.left) == l->id);
  CHECK(table_of(*lj.right) == r->id);
  CHECK(lj.predicate.size() == 2);
  CHECK(find<logical::Filter>(*set.left)->conjuncts.size() == 1);
  // The right arm: every row of r, l NULL-extended, kept where l's primary
  // key is NULL: the rows of r no row of l matched. Its ON is the left
  // arm's; the WHERE is above it.
  const auto& rj = *find<logical::Join>(*set.right);
  CHECK(rj.left_outer);
  CHECK(table_of(*rj.left) == r->id);
  CHECK(table_of(*rj.right) == l->id);
  CHECK(rj.predicate.size() == 2);
  const auto& above = find<logical::Filter>(*set.right)->conjuncts;
  REQUIRE(above.size() == 2);
  CHECK(above[1].kind == logical::Expr::Kind::IsNull);
  CHECK(above[1].args.at(0).col.table == l->id);
  CHECK(above[1].args.at(0).col.col == l->pk_col_ids[0]);
  CHECK(above[1].args.at(0).col.nullable);
  // Each side's columns are NULL-able in the arm that NULL-extends it.
  CHECK(find<logical::Project>(*set.left)->exprs[1].expr.col.nullable);
  CHECK(find<logical::Project>(*set.right)->exprs[0].expr.col.nullable);
  // A table with no primary key is told NULL-extended by its hidden row id.
  auto hidden_key = bind_sql(cat, "SELECT n.a FROM n FULL JOIN r ON n.a = r.a");
  const auto& anti = find<logical::Filter>(*std::get<logical::SetOp>(root(hidden_key).op).right)->conjuncts;
  REQUIRE(anti.size() == 1);
  CHECK(cat.table({kPublicSchema, "n"})->column(anti[0].args.at(0).col.col)->hidden);

  // ORDER BY a selected item is its column; any other key is a hidden
  // column of both arms, after the returned ones. A LIMIT is the root's.
  auto ordered = bind_sql(cat, "SELECT l.k FROM l FULL JOIN r ON l.a = r.a ORDER BY r.j DESC, l.k LIMIT 3");
  REQUIRE(std::holds_alternative<logical::Limit>(root(ordered).op));
  const auto& top = std::get<logical::SetOp>(std::get<logical::Limit>(root(ordered).op).input->op);
  CHECK(top.outputs.size() == 1);
  CHECK(top.hidden.size() == 1);
  REQUIRE(top.order.size() == 2);
  CHECK((top.order[0].column == 1 && top.order[0].desc && top.order[0].nulls_first));
  CHECK((top.order[1].column == 0 && !top.order[1].desc));
  CHECK(find<logical::Project>(*top.left)->exprs.size() == 2);
  CHECK(find<logical::Project>(*top.right)->exprs.size() == 2);

  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  // As PostgreSQL plans it: by a merge or hash join, so some conjunct of
  // the ON is an equality of an expression of each side, or none reads a column.
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a = r.a AND l.k > r.j") == "bound");
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON TRUE") == "bound");
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON FALSE") == "bound");
  const auto* unjoinable = "0A000 FULL JOIN is only supported with merge-joinable or hash-joinable join conditions";
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a < r.a") == unjoinable);
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a = r.a OR l.k = r.j") == unjoinable);
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a = 1") == unjoinable);
  // Each NOT pushed down first, as PostgreSQL's negate_clause: NOT (a <> b) is a = b.
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON NOT (l.a <> r.a)") == "bound");
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON NOT (l.a <> r.a OR l.k < r.j)") == "bound");
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a <> r.a") == unjoinable);
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON NOT (l.a = r.a AND l.k = r.j)") == unjoinable);
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a = r.a FOR UPDATE") ==
        "0A000 FOR UPDATE cannot be applied to the nullable side of an outer join");
  // Not in this cut.
  for (auto sql : {"SELECT count(*) FROM l FULL JOIN r ON l.a = r.a", "SELECT DISTINCT l.k FROM l FULL JOIN r ON l.a = r.a",
                   "SELECT l.k FROM l FULL JOIN r ON l.a = r.a GROUP BY l.k"}) {
    INFO(sql);
    CHECK(state(sql) == "42601 an aggregate, GROUP BY or DISTINCT over a FULL JOIN is not supported yet");
  }
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a = r.a OFFSET 1") == "42601 OFFSET over a FULL JOIN is not supported yet");
  CHECK(state("SELECT l.k FROM l FULL JOIN r ON l.a = r.a UNION SELECT 1") ==
        "42601 a FULL JOIN in an arm of a set operation (UNION, INTERSECT, EXCEPT) is not supported yet");
  CHECK(state("INSERT INTO n SELECT l.k FROM l FULL JOIN r ON l.a = r.a") ==
        "42601 INSERT … SELECT of a FULL JOIN is not supported yet");
  CHECK(state("SELECT gv.a, r.j FROM gv FULL JOIN r ON gv.a = r.a") == "42601 a grouped view in a FULL JOIN is not supported yet");
  CHECK(state("SELECT gv.a, r.j FROM r FULL JOIN gv ON gv.a = r.a") == "42601 a grouped view in a FULL JOIN is not supported yet");
  // A bare $n ORDER BY key is the statement's parameter, typed as PostgreSQL types it there.
  CHECK(bind_sql(cat, "SELECT l.k FROM l FULL JOIN r ON l.a = r.a ORDER BY $1, 1").param_types ==
        bind_sql(cat, "SELECT l.k FROM l LEFT JOIN r ON l.a = r.a ORDER BY $1, 1").param_types);
  CHECK(bind_sql(cat, "SELECT l.k FROM l FULL JOIN r ON l.a = r.a ORDER BY $1, 1").param_types.size() == 1);
}

TEST_CASE("binder: an uncorrelated subquery is bound to run first, its values in hidden parameters", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE u (k INTEGER PRIMARY KEY, w TEXT)").stmt));
  const auto& cat = f.cat();
  using SK = logical::Subquery::Kind;
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  // Each after the statement's parameters and constants, as many as its
  // kind sets, NULL until it runs.
  auto b = bind_sql(cat, "SELECT id FROM t WHERE qty = $1 AND id IN (SELECT k FROM u) AND name = 'x' AND "
                         "qty > ALL (SELECT k FROM u) AND EXISTS (SELECT w, k FROM u) AND id = (SELECT MAX(k) FROM u)");
  REQUIRE(b.subqueries.size() == 4);
  CHECK(b.subqueries[0].kind == SK::Set);
  CHECK(b.subqueries[0].param == 1);  // $1, then its two flags
  CHECK(b.subqueries[1].kind == SK::Bounds);
  CHECK(b.subqueries[1].param == 4);  // after the constant 'x' at 3
  CHECK(b.subqueries[2].kind == SK::Exists);
  CHECK(b.subqueries[2].param == 8);
  CHECK(std::holds_alternative<logical::Limit>(b.subqueries[2].plan->op));  // a row at most
  CHECK(b.subqueries[3].kind == SK::Value);
  CHECK(b.subqueries[3].type.kind == SqlKind::Integer);
  CHECK(b.param_types.size() == 10);
  CHECK(std::ranges::all_of(b.fixed, [&](const Datum& d) { return &d == &b.fixed[2] || std::holds_alternative<std::monostate>(d); }));
  // In a select list: its value, named as its column.
  auto sel = bind_sql(cat, "SELECT (SELECT w FROM u WHERE k = 1), id FROM t");
  CHECK(get<logical::Project>(sel).exprs[0].name == "w");
  CHECK(get<logical::Project>(sel).exprs[0].expr.kind == logical::Expr::Kind::Param);
  CHECK(state("SELECT (SELECT MAX(k) FROM u)") == "bound");
  // Refused: nested, of two columns, FOR UPDATE, AVG, assigned, compared
  // across types, beside an aggregate.
  CHECK(state("SELECT id FROM t WHERE id IN (SELECT k FROM u WHERE k IN (SELECT k FROM u))") ==
        "42601 a subquery inside a subquery is not supported yet");
  CHECK(state("SELECT id FROM t WHERE id = (SELECT k, w FROM u)") == "42601 subquery must return only one column");
  CHECK(state("SELECT id FROM t WHERE id IN (SELECT k FROM u FOR UPDATE)") == "42601 FOR UPDATE in a subquery is not supported yet");
  CHECK(state("SELECT id FROM t WHERE id = (SELECT AVG(k) FROM u)") ==
        "0A000 a subquery of a NUMERIC value (NUMERIC without a precision) is not supported yet");
  CHECK(state("UPDATE t SET qty = (SELECT MAX(k) FROM u) WHERE id = 1") == "42601 a subquery assigned to a column is not supported yet");
  CHECK(state("SELECT id FROM t WHERE id = (SELECT w FROM u)").starts_with("42601 comparing id (bigint) with a subquery of text"));
  CHECK(state("SELECT COUNT(*), (SELECT 1) FROM t") == "42601 a subquery in the select list of an aggregated query is not supported yet");
  CHECK(state("SELECT id FROM t WHERE (SELECT 1) IN (SELECT k FROM u)").starts_with("42601"));
}

TEST_CASE("binder: a correlated subquery takes each outer column as an argument of its own", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE u (k INTEGER PRIMARY KEY, w TEXT, s SMALLINT, b BIGINT, c CHAR(2))").stmt));
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE r (j INTEGER PRIMARY KEY, x TEXT)").stmt));
  const auto& cat = f.cat();
  using SK = logical::Subquery::Kind;
  using K = logical::Expr::Kind;
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  const auto* t = cat.table({kPublicSchema, "t"});
  const auto qty = logical::column_of(t->id, *t->column("qty")).col;
  const auto id = logical::column_of(t->id, *t->column("id")).col;
  // e061_13_01_01: k = qty compares k with a parameter the outer row sets,
  // after the statement's own; the same column twice is one argument.
  auto b = bind_sql(cat, "SELECT id FROM t WHERE id > $1 AND qty IN (SELECT k FROM u WHERE k = qty OR u.s = t.id)");
  REQUIRE(b.subqueries.size() == 1);
  const auto& sub = b.subqueries[0];
  CHECK(sub.kind == SK::Set);
  REQUIRE(sub.args.size() == 2);
  CHECK(sub.args[0].col == qty);
  CHECK(sub.args[0].param == 1);
  CHECK(sub.args[1].col == id);
  CHECK(b.param_types.size() == 1 + 2 + 2);  // $1, the arguments, the set's flags
  CHECK(std::ranges::all_of(b.fixed, [](const Datum& d) { return std::holds_alternative<std::monostate>(d); }));
  const auto* found = find<logical::Filter>(*sub.plan);
  REQUIRE(found != nullptr);
  const auto& filter = *found;
  REQUIRE(filter.conjuncts.size() == 1);
  CHECK(filter.conjuncts[0].kind == K::Or);
  CHECK(filter.conjuncts[0].args[0].args[1].kind == K::Param);
  CHECK(filter.conjuncts[0].args[0].args[1].param == 1);
  auto twice = bind_sql(cat, "SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k = t.qty AND t.qty < u.k AND u.k = id)");
  REQUIRE(twice.subqueries[0].args.size() == 2);
  CHECK(twice.subqueries[0].args[0].col == qty);
  CHECK(twice.subqueries[0].args[1].col == id);
  // An outer column written first is mirrored: t.qty < u.k is u.k > the argument.
  const auto* twice_filter = find<logical::Filter>(*twice.subqueries[0].plan);
  REQUIRE(twice_filter != nullptr);
  const auto& conj = twice_filter->conjuncts;
  CHECK(conj[1].op == ast::Compare::Op::Gt);
  // NOT IN, ANY, ALL, in UPDATE and DELETE.
  CHECK(state("SELECT id FROM t WHERE qty NOT IN (SELECT k FROM u WHERE k = qty)") == "bound");
  CHECK(state("SELECT id FROM t WHERE qty > ALL (SELECT k FROM u WHERE u.b >= t.id)") == "bound");
  CHECK(state("UPDATE t SET qty = 1 WHERE EXISTS (SELECT 1 FROM u WHERE u.k = t.qty)") == "bound");
  CHECK(state("DELETE FROM t WHERE qty = ANY (SELECT MAX(k) FROM u WHERE u.k < t.qty)") == "bound");
  // Two columns PostgreSQL has no operator for: its 42883, in the order written.
  CHECK(state("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE t.qty = u.w)") ==
        "42883 operator does not exist: bigint = text");
  // Refused, each precisely (#836).
  auto refused = [&](std::string_view sql, std::string_view why) {
    INFO(sql);
    CHECK(state(sql) == "42601 " + std::string{why} + " is not supported yet");
  };
  refused("SELECT id FROM t WHERE qty = (SELECT k FROM u WHERE u.k = t.id)", "a correlated scalar subquery");
  refused("SELECT (SELECT k FROM u WHERE u.k = t.id) FROM t", "a correlated scalar subquery");
  refused("SELECT j FROM r JOIN u ON u.k = r.j WHERE EXISTS (SELECT 1 FROM t WHERE t.qty = r.j)",
          "a correlated subquery in a query of a join");
  refused("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k = t.id UNION SELECT 1 FROM r)",
          "a correlated subquery of a set operation (UNION, INTERSECT, EXCEPT)");
  refused("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u JOIN r ON r.j = u.k WHERE u.k = t.id)",
          "a correlated subquery of a join");
  refused("SELECT id FROM t WHERE EXISTS (SELECT t.qty FROM u)", "a column of the query around a subquery in its select list");
  refused("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k = t.qty + 1)",
          "a column of the query around a subquery other than compared with a column of the subquery");
  refused("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE t.qty = 1)",
          "a column of the query around a subquery other than compared with a column of the subquery");
  refused("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE t.name IS NULL)",
          "a column of the query around a subquery other than compared with a column of the subquery");
  refused("SELECT j FROM r WHERE EXISTS (SELECT 1 FROM u WHERE u.c = r.x)", "a correlated subquery comparing a CHAR(n) column");
  refused("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k IN (SELECT k FROM u WHERE u.k = t.id))",
          "a subquery inside a subquery");
}

TEST_CASE("binder: OR, NOT, IS NULL, <>, BETWEEN and IN are predicates over comparisons", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  using K = logical::Expr::Kind;
  // The shape of each conjunct, as a string: its kind, then its arguments'.
  auto shape = [](const logical::Expr& e) {
    auto name = [](K k) -> std::string {
      switch (k) {
        case K::And: return "AND";
        case K::Or: return "OR";
        case K::Not: return "NOT";
        case K::IsNull: return "ISNULL";
        case K::Compare: return "CMP";
        case K::Like: return "LIKE";
        case K::Truth: return "TRUTH";
        case K::InSet: return "INSET";
        case K::Column:
        case K::Param:
        case K::Const:
        case K::Add:
        case K::Sub:
        case K::Aggregate: break;
      }
      return "?";
    };
    std::function<std::string(const logical::Expr&)> walk = [&](const logical::Expr& x) {
      if (x.kind == K::Compare || x.kind == K::IsNull || x.args.empty()) return name(x.kind);
      std::string out = name(x.kind) + "(";
      for (std::size_t i = 0; i < x.args.size(); ++i) out += (i ? "," : "") + walk(x.args[i]);
      return out + ")";
    };
    return walk(e);
  };
  auto conjuncts = [&](std::string_view where) {
    auto b = bind_sql(cat, "SELECT id FROM t WHERE " + std::string{where});
    std::vector<std::string> out;
    for (const auto& c : get<logical::Filter>(b).conjuncts) out.push_back(shape(c));
    return out;
  };
  using V = std::vector<std::string>;
  CHECK(conjuncts("qty <> $1") == V{"NOT(CMP)"});
  CHECK(conjuncts("qty IS NULL") == V{"ISNULL"});
  CHECK(conjuncts("qty IS NOT NULL") == V{"NOT(ISNULL)"});
  CHECK(conjuncts("qty = $1 OR name = $2") == V{"OR(CMP,CMP)"});
  // BETWEEN's two bounds, at the top, are two conjuncts a key can take.
  CHECK(conjuncts("qty BETWEEN $1 AND $2") == V{"CMP", "CMP"});
  CHECK(conjuncts("qty NOT BETWEEN 1 AND 5") == V{"NOT(AND(CMP,CMP))"});
  CHECK(conjuncts("qty BETWEEN SYMMETRIC 5 AND 1") == V{"OR(AND(CMP,CMP),AND(CMP,CMP))"});
  CHECK(conjuncts("qty IN (1, 2, 3)") == V{"OR(CMP,CMP,CMP)"});
  CHECK(conjuncts("qty IN (1)") == V{"CMP"});
  CHECK(conjuncts("qty NOT IN (1, 2)") == V{"NOT(OR(CMP,CMP))"});
  // Of a subquery: IN tests its set; ANY and ALL its bounds, ALL as NOT ANY
  // of the negated operator; each NULL where a row is, and over no rows
  // false (TRUTH: the flags it sets).
  CHECK(conjuncts("qty IN (SELECT qty FROM t)") == V{"AND(OR(INSET(?),TRUTH(?)),TRUTH(?))"});
  CHECK(conjuncts("qty NOT IN (SELECT qty FROM t)") == V{"NOT(AND(OR(INSET(?),TRUTH(?)),TRUTH(?)))"});
  CHECK(conjuncts("qty < ALL (SELECT qty FROM t)") == V{"NOT(AND(OR(CMP,TRUTH(?)),TRUTH(?)))"});
  CHECK(conjuncts("qty <> SOME (SELECT qty FROM t)") == V{"AND(OR(OR(CMP,CMP),TRUTH(?)),TRUTH(?))"});
  CHECK(conjuncts("EXISTS (SELECT 1 FROM t)") == V{"TRUTH(?)"});
  CHECK(conjuncts("qty = (SELECT MAX(qty) FROM t)") == V{"CMP"});
  CHECK(conjuncts("id = $1 AND NOT (qty = 1 OR name IS NULL)") == V{"CMP", "NOT(OR(CMP,ISNULL))"});
  // Each comparison in a tree is a WHERE's: typed, folded, refused alike.
  CHECK(conjuncts("qty = 1 OR qty = id") == V{"OR(CMP,CMP)"});
  // $n ± k inside AND, OR or NOT is an expression, evaluated: the key's
  // shape is a conjunct's own.
  CHECK(conjuncts("qty = $1 + 1 OR id = 2") == V{"OR(CMP,CMP)"});
  CHECK(conjuncts("1 IS NULL") == V{"ISNULL"});
  CHECK(rejected(cat, "SELECT qty FROM t GROUP BY qty HAVING qty <> 1").first ==
        "HAVING with AND, OR, NOT, IS NULL, <>, BETWEEN, IN or a constant is not supported yet");
  // NULL is FALSE as a filter, but not under NOT, where it stays unknown.
  CHECK(rejected(cat, "SELECT id FROM t WHERE NOT (qty = 1 OR NULL)").first == "NULL under NOT in a predicate is not supported yet");
  CHECK(bind_sql(cat, "SELECT id FROM t WHERE qty = $1 OR qty <> $2").param_types ==
        std::vector<PhysType>{PhysType::I64, PhysType::I64});
}

TEST_CASE("binder: LIKE of a text column, its pattern TEXT, its escape one character", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE lk (k INTEGER PRIMARY KEY, v TEXT, w VARCHAR(4), b BYTEA)").stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  auto like = [&](std::string_view where) {
    auto b = bind_sql(cat, "SELECT k FROM lk WHERE " + std::string{where});
    return get<logical::Filter>(b).conjuncts.at(0);
  };
  auto plain = like("v LIKE $1");
  REQUIRE(plain.kind == logical::Expr::Kind::Like);
  CHECK(plain.constant == '\\');
  CHECK(plain.args[1].kind == logical::Expr::Kind::Param);
  CHECK(plain.args[1].type == SqlType{SqlKind::Text});  // a TEXT, whatever the column's length
  CHECK(bind_sql(cat, "SELECT k FROM lk WHERE w LIKE $1").params.at(0).type == SqlType{SqlKind::Text});
  CHECK(like("v NOT LIKE 'a%'").kind == logical::Expr::Kind::Not);
  CHECK(like("v LIKE 'a!%' ESCAPE '!'").constant == '!');
  CHECK(like("v LIKE 'a%' ESCAPE ''").constant == -1);
  CHECK(like("v LIKE 'a%' ESCAPE '\xc3\xa9'").constant == 0xE9);
  CHECK(state("SELECT k FROM lk WHERE v LIKE 'a' ESCAPE 'ab'") == "22025");
  CHECK(state("SELECT k FROM lk WHERE k LIKE '1%'") == "42883");
  CHECK(rejected(cat, "SELECT k FROM lk WHERE b LIKE 'a%'").first == "LIKE of a BYTEA column is not supported yet");
  // ILIKE is LIKE folding ASCII letters; ESCAPE $n is a TEXT parameter, checked when the plan runs.
  CHECK(like("v ILIKE 'a%'").fold);
  CHECK_FALSE(like("v LIKE 'a%'").fold);
  CHECK(like("v NOT ILIKE 'a%'").kind == logical::Expr::Kind::Not);
  CHECK(state("SELECT k FROM lk WHERE k ILIKE '1%'") == "42883");
  auto escaped = bind_sql(cat, "SELECT k FROM lk WHERE v LIKE $1 ESCAPE $2");
  REQUIRE(escaped.params.size() == 2);
  CHECK(escaped.params.at(1).type == SqlType{SqlKind::Text});
  CHECK(like("v LIKE 'a%' ESCAPE $1").args.size() == 3);
}

TEST_CASE("binder: a FOREIGN KEY is checked against the referenced table's keys, then not kept", "[sql][binder][catalog]") {
  sql_test::Fixture f;
  for (auto ddl : {"CREATE TABLE p (k INTEGER PRIMARY KEY, a INTEGER NOT NULL, b INTEGER NOT NULL, s TEXT, UNIQUE (a, b))",
                   "CREATE TABLE n (a INTEGER)"}) {
    f.ddl(std::get<Ddl>(bind_sql(f.cat(), ddl).stmt));
  }
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  // The primary key, by name or by default; a unique index's columns in any order.
  CHECK(state("CREATE TABLE c (x BIGINT REFERENCES p (k))") == "bound");
  CHECK(state("CREATE TABLE c (x SMALLINT REFERENCES p)") == "bound");
  CHECK(state("CREATE TABLE c (x INT, y INT, CONSTRAINT f FOREIGN KEY (x, y) REFERENCES p (b, a))") == "bound");
  CHECK(state("CREATE TABLE c (x VARCHAR(4) REFERENCES p (s))") ==
        "42830 there is no unique constraint matching given keys for referenced table \"p\"");
  CHECK(state("CREATE TABLE c (x INT REFERENCES p (a))") ==
        "42830 there is no unique constraint matching given keys for referenced table \"p\"");
  CHECK(state("CREATE TABLE c (x INT REFERENCES n)") == "42704 there is no primary key for referenced table \"n\"");
  CHECK(state("CREATE TABLE c (x INT REFERENCES nope)") == "42P01 relation \"nope\" does not exist");
  CHECK(state("CREATE TABLE c (x INT REFERENCES p (z))") ==
        "42703 column \"z\" referenced in foreign key constraint does not exist");
  CHECK(state("CREATE TABLE c (x INT, FOREIGN KEY (w) REFERENCES p)") ==
        "42703 column \"w\" referenced in foreign key constraint does not exist");
  CHECK(state("CREATE TABLE c (x INT, FOREIGN KEY (x) REFERENCES p (a, b))") ==
        "42830 number of referencing and referenced columns for foreign key disagree");
  CHECK(state("CREATE TABLE c (x INT, FOREIGN KEY (x, x) REFERENCES p (a, b))") == "bound");  // as PostgreSQL takes it
  CHECK(state("CREATE TABLE c (x INT, y INT, FOREIGN KEY (x, y) REFERENCES p (a, a))") ==
        "42830 foreign key referenced-columns list must not contain duplicates");
  CHECK(state("CREATE TABLE c (x TEXT REFERENCES p (k))") == "42804 foreign key constraint \"c_x_fkey\" cannot be implemented");
  // Each pair of types PostgreSQL 17 takes, a column's type and its key's.
  const std::vector<std::string> types{"SMALLINT", "INTEGER", "BIGINT", "REAL", "DOUBLE PRECISION", "NUMERIC(6,2)",
                                        "NUMERIC(8,0)", "TEXT", "VARCHAR(4)", "BYTEA", "BOOLEAN", "DATE", "TIMESTAMP", "TIME"};
  const std::set<std::pair<std::size_t, std::size_t>> takes{  // (column, key), by index in types
      {0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {2, 1}, {0, 2}, {1, 2}, {2, 2}, {0, 3}, {1, 3}, {2, 3},
      {3, 3}, {4, 3}, {5, 3}, {6, 3}, {0, 4}, {1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {0, 5},
      {1, 5}, {2, 5}, {5, 5}, {6, 5}, {0, 6}, {1, 6}, {2, 6}, {5, 6}, {6, 6}, {7, 7}, {8, 7}, {7, 8},
      {8, 8}, {9, 9}, {10, 10}, {11, 11}, {12, 11}, {11, 12}, {12, 12}, {13, 13}};
  for (std::size_t key = 0; key < types.size(); ++key) {
    sql_test::Fixture g;
    g.ddl(std::get<Ddl>(bind_sql(g.cat(), "CREATE TABLE k (v " + types[key] + " PRIMARY KEY)").stmt));
    for (std::size_t col = 0; col < types.size(); ++col) {
      INFO(types[col] << " REFERENCES " << types[key]);
      auto taken = true;
      try {
        (void)bind_sql(g.cat(), "CREATE TABLE c (x " + types[col] + " REFERENCES k)");
      } catch (const ast::SqlError& e) {
        CHECK(std::string{e.sqlstate()} == "42804");
        taken = false;
      }
      CHECK(taken == takes.contains({col, key}));
    }
  }
  // A table may reference itself, by the keys it is created with.
  CHECK(state("CREATE TABLE c (id INT PRIMARY KEY, up INT REFERENCES c)") == "bound");
  CHECK(state("CREATE TABLE c (id INT, up INT REFERENCES c (id))") ==
        "42830 there is no unique constraint matching given keys for referenced table \"c\"");
  CHECK(state("ALTER TABLE n ADD COLUMN x INT REFERENCES p").starts_with("42601"));
  // Not kept: the DDL is the table's, as it would be without it.
  auto ddl = std::get<CreateTableDdl>(std::get<Ddl>(bind_sql(cat, "CREATE TABLE c (x INT REFERENCES p)").stmt));
  CHECK(ddl.unique.empty());
}

TEST_CASE("binder: DDL types, defaults and the drop and alter forms", "[sql][binder][catalog]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  {
    auto b = bind_sql(cat,
                      "CREATE TABLE u (a SMALLINT PRIMARY KEY, b INTEGER DEFAULT -5, c NUMERIC(12, 2) DEFAULT 1.005, "
                      "d VARCHAR(4) DEFAULT 'h\xc3\xa9ll', e TIMESTAMP, f DATE, g TIME, h REAL DEFAULT 0.1, "
                      "i DOUBLE PRECISION DEFAULT 0.1, j BOOLEAN DEFAULT true, k TEXT, l BYTEA, m BIGINT DEFAULT 10000000000, "
                      "n DECIMAL(4) DEFAULT 12, o VARCHAR)");
    auto& s = std::get<CreateTableDdl>(std::get<Ddl>(b.stmt));
    REQUIRE(s.columns.size() == 15);
    auto kind = [&](std::size_t i) { return s.columns[i].sql.kind; };
    CHECK(kind(0) == SqlKind::SmallInt);
    CHECK(s.columns[1].default_value == Datum{std::int64_t{-5}});
    CHECK(s.columns[2].sql == SqlType{SqlKind::Decimal, 0, 12, 2});
    CHECK(s.columns[2].default_value == Datum{std::int64_t{101}});  // 1.005 rounds half away from zero
    CHECK(s.columns[3].sql == SqlType{SqlKind::Varchar, 4});
    CHECK((kind(4) == SqlKind::Timestamp && kind(5) == SqlKind::Date && kind(6) == SqlKind::Time));
    // The catalog holds a REAL default as a row stores it: its float.
    CHECK(s.columns[7].default_value == Datum{static_cast<double>(0.1f)});
    CHECK(s.columns[8].default_value == Datum{0.1});
    CHECK(s.columns[9].default_value == Datum{true});
    CHECK((kind(10) == SqlKind::Text && kind(11) == SqlKind::Bytea));
    CHECK(s.columns[12].default_value == Datum{std::int64_t{10'000'000'000}});
    CHECK(s.columns[13].sql == SqlType{SqlKind::Decimal, 0, 4, 0});
    CHECK(s.columns[13].default_value == Datum{std::int64_t{12}});
    CHECK(kind(14) == SqlKind::Text);  // VARCHAR without a length
  }
  {
    // A REAL DEFAULT is rounded once, to float, as PostgreSQL's float4in
    // rounds. This literal is just above the midpoint between 1 and the next
    // float, 1 + 2^-23; through a double it would round to the midpoint
    // itself, then to 1 by ties-to-even.
    auto b = bind_sql(cat, "CREATE TABLE r (k BIGINT PRIMARY KEY, a REAL DEFAULT 1.00000005960464477539062500000001)");
    auto& s = std::get<CreateTableDdl>(std::get<Ddl>(b.stmt));
    CHECK(s.columns[1].default_value == Datum{1.0 + 0x1p-23});
    auto real_default = [&](std::string_view literal) {
      auto sql = "CREATE TABLE r (k BIGINT PRIMARY KEY, a REAL DEFAULT " + std::string(literal) + ")";
      return std::get<CreateTableDdl>(std::get<Ddl>(bind_sql(cat, sql).stmt)).columns[1].default_value;
    };
    // A denormal float is kept, not refused as underflow.
    CHECK(real_default("1e-40") == Datum{static_cast<double>(1e-40f)});
    CHECK(std::get<double>(real_default("1e-40")) != 0.0);
    // FLT_MAX, and a literal above it that still rounds to it: below the
    // midpoint between FLT_MAX and 2^128, 3.40282356779...e38. Just above
    // that midpoint, 3.40282357e38 rounds to infinity: out of range.
    constexpr auto kFloatMax = static_cast<double>(std::numeric_limits<float>::max());
    CHECK(real_default("3.4028235e38") == Datum{kFloatMax});
    CHECK(real_default("3.40282356e38") == Datum{kFloatMax});
    CHECK(rejected(cat, "CREATE TABLE r (k BIGINT PRIMARY KEY, a REAL DEFAULT 3.40282357e38)").first ==
          "DEFAULT out of range for type real");
    CHECK(rejected(cat, "CREATE TABLE r (k BIGINT PRIMARY KEY, a REAL DEFAULT 1e39)").first ==
          "DEFAULT out of range for type real");
    // DOUBLE PRECISION, as PostgreSQL 16 takes it: 0.1 and a denormal kept;
    // overflow and underflow to zero out of range (before parse_number,
    // 1e400 was refused as "not a constant").
    auto double_default = [&](std::string_view literal) {
      auto sql = "CREATE TABLE r (k BIGINT PRIMARY KEY, a DOUBLE PRECISION DEFAULT " + std::string(literal) + ")";
      return std::get<CreateTableDdl>(std::get<Ddl>(bind_sql(cat, sql).stmt)).columns[1].default_value;
    };
    CHECK(double_default("0.1") == Datum{0.1});
    CHECK(double_default("1e-310") == Datum{1e-310});
    CHECK(std::get<double>(double_default("1e-310")) != 0.0);
    CHECK(rejected(cat, "CREATE TABLE r (k BIGINT PRIMARY KEY, a DOUBLE PRECISION DEFAULT 1e400)").first ==
          "DEFAULT out of range for type double precision");
    CHECK(rejected(cat, "CREATE TABLE r (k BIGINT PRIMARY KEY, a DOUBLE PRECISION DEFAULT 1e-400)").first ==
          "DEFAULT out of range for type double precision");
    CHECK(rejected(cat, "CREATE TABLE r (k BIGINT PRIMARY KEY, a REAL DEFAULT 1e-50)").first ==
          "DEFAULT out of range for type real");
  }
  {
    auto b = bind_sql(cat, "CREATE UNIQUE INDEX u1 ON t (qty DESC, name)");
    auto& s = std::get<CreateIndexDdl>(std::get<Ddl>(b.stmt));
    CHECK(s.unique);
    REQUIRE(s.columns.size() == 2);
    CHECK(s.columns[0].desc);
    CHECK_FALSE(s.columns[1].desc);
  }
  CHECK(std::get<DropTableDdl>(std::get<Ddl>(bind_sql(cat, "DROP TABLE IF EXISTS t").stmt)).if_exists);
  CHECK(std::get<DropIndexDdl>(std::get<Ddl>(bind_sql(cat, "DROP INDEX t_by_name").stmt)).name == QualifiedName{kPublicSchema, "t_by_name"});
  {
    auto b = bind_sql(cat, "ALTER TABLE t ADD COLUMN z INTEGER NOT NULL DEFAULT 3");
    auto& s = std::get<AddColumnDdl>(std::get<Ddl>(b.stmt));
    CHECK(s.column.name == "z");
    CHECK_FALSE(s.column.nullable);
    CHECK(s.column.default_value == Datum{std::int64_t{3}});
  }

  CHECK(rejected(cat, "CREATE TABLE u (k INTEGER PRIMARY KEY DEFAULT 2147483648)").first ==
        "DEFAULT out of range for type integer");
  {
    // NUMERIC(p, p): a whole part of zero has no digits.
    auto b = bind_sql(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, a NUMERIC(4, 4) DEFAULT 0.5, b NUMERIC(4, 4) DEFAULT 0, "
                           "c NUMERIC(4, 4) DEFAULT -0.00005, d NUMERIC(4, 4) DEFAULT 00.1234)");
    auto& s = std::get<CreateTableDdl>(std::get<Ddl>(b.stmt));
    CHECK(s.columns[1].default_value == Datum{std::int64_t{5000}});
    CHECK(s.columns[2].default_value == Datum{std::int64_t{0}});
    CHECK(s.columns[3].default_value == Datum{std::int64_t{-1}});  // half away from zero
    CHECK(s.columns[4].default_value == Datum{std::int64_t{1234}});
  }
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC(4, 4) DEFAULT 1)").first ==
        "DEFAULT out of range for type numeric(4,4)");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC(4, 4) DEFAULT 0.99995)").first ==
        "DEFAULT out of range for type numeric(4,4)");  // rounds to 1.0000
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC(3, 1) DEFAULT 100)").first ==
        "DEFAULT out of range for type numeric(3,1)");
  // More digits than any DECIMAL holds: refused by the one precision check, not overflowed while parsed.
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC(18, 0) DEFAULT 12345678901234567890123)").first ==
        "DEFAULT out of range for type numeric(18,0)");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v VARCHAR(2) DEFAULT 'abc')").first ==
        "DEFAULT is not a varchar(2): value too long for type character varying(2)");
  // Not UTF-8: refused with the reason, not as too long.
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TEXT DEFAULT '\xff')").first ==
        "DEFAULT is not a text: invalid byte sequence for encoding \"UTF8\"");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v BIGINT DEFAULT 'x')").first ==
        "DEFAULT is not a constant of type bigint");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIMESTAMP DEFAULT 0)").first ==
        "a DEFAULT for type timestamp is not supported yet");
  // A DEFAULT of the clock, of a type that takes it by assignment.
  {
    auto b = bind_sql(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, d DATE DEFAULT now(), t TIME DEFAULT LOCALTIMESTAMP(9))");
    const auto& ddl = std::get<CreateTableDdl>(std::get<Ddl>(b.stmt));
    CHECK(ddl.columns[1].clock_default == ClockDefault{ClockDefault::Fn::CurrentTimestamp, 6});
    CHECK(ddl.columns[2].clock_default == ClockDefault{ClockDefault::Fn::LocalTimestamp, 6});  // past 6 is 6
  }
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v INTEGER DEFAULT CURRENT_DATE)").first ==
        "column \"v\" is of type integer but default expression is of type date");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIMESTAMP DEFAULT CURRENT_DATE)").first ==
        "the clock's date assigned to a timestamp without time zone column is not supported yet");
  CHECK(rejected(cat, "ALTER TABLE t ADD COLUMN d DATE DEFAULT CURRENT_DATE").first ==
        "ADD COLUMN … DEFAULT of the clock is not supported yet");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC(19, 2))").first.starts_with("NUMERIC(19, 2)"));
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIMESTAMPTZ DEFAULT 0)").first ==
        "a DEFAULT for type timestamptz is not supported yet");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIMESTAMP(3) WITH TIME ZONE)").first ==
        "a precision other than 6 on TIMESTAMP WITH TIME ZONE is not supported yet");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIME(3) WITH TIME ZONE)").first ==
        "a precision under 6 on TIME WITH TIME ZONE is not supported yet");
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIMETZ DEFAULT '12:00+01')").first ==
        "a DEFAULT for type timetz is not supported yet");
  CHECK(rejected(cat, "ALTER TABLE t ADD COLUMN z BIGINT NOT NULL").first.find("needs a DEFAULT") != std::string::npos);
  CHECK(rejected(cat, "ALTER TABLE nope ADD COLUMN z BIGINT").first == "relation \"nope\" does not exist");
  CHECK(rejected(cat, "CREATE INDEX i ON t (nope)").first == "column \"nope\" does not exist");
  // A column left out is the runtime's to refuse (BC_C_NOT_NULL), a primary key column among them.
  CHECK_NOTHROW((void)bind_sql(cat, "INSERT INTO t (name) VALUES ($1)"));
  // One PRIMARY KEY a table: two column-level ones are not a composite key (PostgreSQL: 42P16).
  CHECK(rejected(cat, "CREATE TABLE u (a BIGINT PRIMARY KEY, b BIGINT PRIMARY KEY)").first ==
        "multiple primary keys for table \"u\" are not allowed");
  CHECK(rejected(cat, "CREATE TABLE u (a BIGINT PRIMARY KEY, b BIGINT, PRIMARY KEY (b))").first ==
        "multiple primary keys for table \"u\" are not allowed");
  CHECK(rejected(cat, "CREATE TABLE u (a BIGINT, b BIGINT, PRIMARY KEY (a), PRIMARY KEY (b))").first ==
        "multiple primary keys for table \"u\" are not allowed");
  CHECK(rejected(cat, "CREATE TABLE u (a BIGINT PRIMARY KEY PRIMARY KEY)").first ==
        "multiple primary keys for table \"u\" are not allowed");
}

TEST_CASE("binder: COUNT, ORDER BY and column + parameter", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  {
    auto b = bind_sql(cat, "SELECT count(qty) FROM t WHERE name = $1");
    CHECK(find<logical::Project>(root(b)) == nullptr);
    const auto& a = get<logical::Aggregate>(b).aggs.at(0);
    REQUIRE(a.expr.args.size() == 1);
    CHECK(a.expr.args[0].col.col == 2u);
    CHECK(a.name == "count");
    CHECK(a.expr.type.kind == SqlKind::BigInt);
    auto star = bind_sql(cat, "SELECT count(*) FROM t WHERE name = $1");
    CHECK(get<logical::Aggregate>(star).aggs.at(0).expr.args.empty());
  }
  {
    auto b = bind_sql(cat, "SELECT id FROM t WHERE name = $1 ORDER BY qty DESC LIMIT 2");
    const auto& keys = get<logical::Sort>(b).keys;
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].expr.col.col == 2u);
    CHECK(keys[0].desc);
    CHECK(keys[0].nulls_first);  // PostgreSQL's default for DESC
    CHECK(std::get<logical::Limit>(root(b).op).n == 2);
  }
  {
    // ORDER BY an expression of the FROM columns: its value, bound as a
    // select item is. A constant orders nothing; a bare $n is text where
    // nothing before typed it, as PostgreSQL types it.
    auto b = bind_sql(cat, "SELECT id FROM t ORDER BY qty + 1 DESC");
    const auto& keys = get<logical::Sort>(b).keys;
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].expr.kind == logical::Expr::Kind::Add);
    CHECK(keys[0].desc);
    auto p = bind_sql(cat, "SELECT id FROM t ORDER BY $1, 1 + 1, qty");
    CHECK(p.param_types.at(0) == PhysType::Bytes);  // 1 + 1 is a hidden parameter after it
    CHECK(get<logical::Sort>(p).keys.size() == 1);
    auto q = bind_sql(cat, "SELECT id FROM t WHERE qty = $1 ORDER BY $1");
    CHECK(q.param_types == std::vector<PhysType>{PhysType::I64});
    CHECK(find<logical::Sort>(root(q)) == nullptr);
  }
  {
    // NULLS FIRST and NULLS LAST as written; ASC's default is last.
    auto b = bind_sql(cat, "SELECT id FROM t WHERE name = $1 ORDER BY qty NULLS FIRST, name DESC NULLS LAST, id");
    const auto& keys = get<logical::Sort>(b).keys;
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].nulls_first);
    CHECK_FALSE(keys[1].nulls_first);
    CHECK_FALSE(keys[2].nulls_first);
  }
  {
    // The operand is typed and range-checked as the column it is added to.
    auto b = bind_sql(cat, "UPDATE t SET qty = qty + $2 WHERE id = $1");
    CHECK(get<logical::Update>(b).set[0].second.kind == logical::Expr::Kind::Add);
    CHECK(b.param_sql[1].has_value());
    // An UPDATE's assigned $n is checked against the column's whole type,
    // an INSERT's by the row builder.
    CHECK(bind_sql(cat, "UPDATE t SET qty = $2 WHERE name = $1").param_sql[1].has_value());
    CHECK_FALSE(bind_sql(cat, "INSERT INTO t (id, qty) VALUES ($1, $2)").param_sql[1].has_value());
    // The narrowest of a $n's uses is what it is checked against, in either
    // order: here d's DECIMAL(4, 2), not the operand's 18 digits.
    sql_test::Fixture h;
    h.ddl(std::get<Ddl>(bind_sql(h.cat(), "CREATE TABLE m (k BIGINT PRIMARY KEY, d NUMERIC(4, 2), e NUMERIC(10, 2))").stmt));
    for (const auto* sql : {"UPDATE m SET e = e + $1, d = $1 WHERE k = $2", "UPDATE m SET d = $1, e = e + $1 WHERE k = $2"}) {
      INFO(sql);
      auto range = bind_sql(h.cat(), sql).param_sql[0];
      REQUIRE(range);
      CHECK(range->kind == SqlKind::Decimal);
      CHECK(range->precision == 4);
    }
  }
  //                       0         1         2         3         4
  //                       01234567890123456789012345678901234567890123456789
  CHECK(rejected(cat, "SELECT count(*), id FROM t WHERE id = $1") ==
        std::pair<std::string, std::int32_t>{
            "column \"t.id\" must appear in the GROUP BY clause or be used in an aggregate function", 17});
  CHECK(rejected(cat, "SELECT count(*) FROM t WHERE id = $1 ORDER BY id").first ==
        "column \"t.id\" must appear in the GROUP BY clause or be used in an aggregate function");
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = $1 ORDER BY nope").first == "column \"nope\" does not exist");
  CHECK(rejected(cat, "UPDATE t SET qty = count(*) WHERE id = $1").first == "aggregate functions are not allowed in UPDATE");
  CHECK(rejected(cat, "UPDATE t SET qty = qty + count(*) WHERE id = $1").first == "aggregate functions are not allowed in UPDATE");
  // SUM and MIN take a column held as an int64_t; SUM an exact number.
  {
    auto b = bind_sql(cat, "SELECT sum(qty) FROM t WHERE name = $1");
    const auto& sum = get<logical::Aggregate>(b).aggs.at(0);
    CHECK(sum.expr.fn == logical::Expr::Fn::Sum);
    CHECK(sum.name == "sum");
    CHECK(sql_type_name(sum.expr.type) == "numeric");  // of a BIGINT: no declared precision
  }
  CHECK(rejected(cat, "SELECT sum(name) FROM t WHERE id = $1") ==
        std::pair<std::string, std::int32_t>{"function sum(bytea) does not exist", 7});
  CHECK(rejected(cat, "SELECT min(name) FROM t WHERE id = $1").first == "function min(bytea) does not exist");
  CHECK(rejected(cat, "SELECT min(qty), id FROM t WHERE id = $1").first ==
        "column \"t.id\" must appear in the GROUP BY clause or be used in an aggregate function");
  CHECK(rejected(cat, "SELECT sum(qty) FROM t WHERE id = $1 FOR UPDATE").first ==
        "FOR UPDATE is not allowed with aggregate functions");
  // One SQL type per parameter, whichever use comes first.
  sql_test::Fixture g;
  g.ddl(std::get<Ddl>(bind_sql(g.cat(), "CREATE TABLE s (k BIGINT PRIMARY KEY, b BIGINT, a SMALLINT)").stmt));
  CHECK(rejected(g.cat(), "UPDATE s SET b = $1, a = a + $1 WHERE k = $2").first ==
        "inconsistent types deduced for parameter $1");  // PostgreSQL's 42P08
  CHECK(rejected(g.cat(), "UPDATE s SET a = a - $1, b = $1 WHERE k = $2").first ==
        "parameter $1 is combined with a (smallint) and assigned to b (bigint)");
  CHECK(rejected(g.cat(), "UPDATE s SET b = $1 WHERE a = $1").first ==
        "parameter $1 is assigned to b (bigint) and compared with a (smallint)");
  CHECK_NOTHROW(bind_sql(g.cat(), "UPDATE s SET b = b + $1 WHERE k = $1"));
  CHECK(rejected(cat, "SELECT count(*) FROM t WHERE id = $1 FOR UPDATE").first ==
        "FOR UPDATE is not allowed with aggregate functions");
}

TEST_CASE("binder: a join of two tables, qualified columns and integer literals", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE u (k BIGINT PRIMARY KEY, qty BIGINT, d NUMERIC(10, 2))").stmt));
  const auto& cat = f.cat();
  {
    auto b = bind_sql(cat, "SELECT name, u.qty FROM t, u WHERE id = $1 AND k = $2");
    const auto& bound_join = get<logical::Join>(b);
    CHECK(std::get<logical::Get>(bound_join.left->op).table->name == "t");
    CHECK(std::get<logical::Get>(bound_join.right->op).table->name == "u");
    const auto& targets = get<logical::Project>(b).exprs;
    REQUIRE(targets.size() == 2);
    CHECK(targets[1].expr.col.table == cat.table({kPublicSchema, "u"})->id);
    // The WHERE is one Filter above the join; pushdown moves each conjunct
    // onto the table it reads.
    CHECK(get<logical::Filter>(b).conjuncts.size() == 2);
    logical::rewrite(root(b));
    const auto& j = get<logical::Join>(b);  // the Filter above it is gone
    CHECK(find<logical::Filter>(root(b)) == &std::get<logical::Filter>(j.left->op));
    CHECK(std::get<logical::Filter>(j.left->op).conjuncts.size() == 1);
    CHECK(std::get<logical::Filter>(j.right->op).conjuncts.size() == 1);
    CHECK(j.predicate.empty());
    // Pruning: the join reads t.name and u.qty, and each side's filter its own key column too.
    CHECK(j.left->output.size() == 1);
    CHECK(std::get<logical::Filter>(j.left->op).input->output.size() == 2);
  }
  {
    // $n ± k compared with an INTEGER column: $n is an INTEGER, the literal
    // added as the offset.
    sql_test::Fixture g;
    g.ddl(std::get<Ddl>(bind_sql(g.cat(), "CREATE TABLE ol (w INTEGER, o INTEGER, s SMALLINT, PRIMARY KEY (w, o))").stmt));
    auto b = bind_sql(g.cat(), "SELECT o FROM ol WHERE w = $1 AND o < $2 AND o >= $2 - 20");
    const auto& w = get<logical::Filter>(b).conjuncts;
    const auto& sum = w[2].args[1];
    CHECK(sum.kind == logical::Expr::Kind::Sub);
    CHECK(sum.type.kind == SqlKind::Integer);
    CHECK(sum.args[0].param == 1u);
    CHECK(sum.args[1].constant == 20);
    CHECK(w[1].args[1].kind == logical::Expr::Kind::Param);
    // Beside another integer column, or past INTEGER, $n ± k is an
    // expression, $n typed by the literal as PostgreSQL types it.
    auto small = bind_sql(g.cat(), "SELECT o FROM ol WHERE w = $1 AND s < $2 + 1");
    CHECK(get<logical::Filter>(small).conjuncts[1].args[1].args[1].kind == logical::Expr::Kind::Param);
    CHECK(small.params.at(1).type == SqlType{SqlKind::Integer});
    auto big = bind_sql(g.cat(), "SELECT o FROM ol WHERE w = $1 AND o < $2 + 2147483648");
    CHECK(big.params.at(1).type == SqlType{SqlKind::BigInt});
  }
  {
    // A SET value is the select list's expression, a literal its constant;
    // the items are in the table's column order, whatever the written one.
    auto b = bind_sql(cat, "UPDATE u SET d = d + 1, qty = qty - 2 WHERE k = $1");
    const auto& up = get<logical::Update>(b);
    REQUIRE(up.set.size() == 2);
    CHECK(up.set[0].first.col < up.set[1].first.col);
    // qty - 2 is a bigint, stored as it is; d + 1 a numeric of d's scale,
    // cast to d's DECIMAL(10, 2), which checks its precision.
    CHECK(up.set[0].second.kind == logical::Expr::Kind::Sub);
    const auto& d = up.set[1].second;
    CHECK(d.kind == logical::Expr::Kind::Cast);
    CHECK(d.type == SqlType{SqlKind::Decimal, 0, 10, 2});
    CHECK(d.args[0].kind == logical::Expr::Kind::Add);
  }
  //                       0         1         2         3         4
  //                       01234567890123456789012345678901234567890123456789
  CHECK(rejected(cat, "SELECT qty FROM t, u WHERE id = $1 AND k = $2") ==
        std::pair<std::string, std::int32_t>{"column reference \"qty\" is ambiguous", 7});
  CHECK(rejected(cat, "SELECT v.qty FROM t, u WHERE id = $1").first == "missing FROM-clause entry for table \"v\"");
  CHECK(rejected(cat, "SELECT v.qty FROM t WHERE id = $1").first == "missing FROM-clause entry for table \"v\"");
  CHECK(rejected(cat, "SELECT id FROM t, t WHERE id = $1").first == "table name \"t\" specified more than once");
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE id = $1 AND k = $2 FOR UPDATE").first ==
        "FOR UPDATE on a join is not supported yet");
  {
    // A column of one table equal to one of the other joins them; an
    // aggregate on a join takes either table's column.
    auto b = bind_sql(cat, "SELECT count(DISTINCT u.qty) FROM t JOIN u ON u.qty = id WHERE id >= $1");
    logical::rewrite(root(b));
    const auto& on = get<logical::Join>(b).predicate;
    REQUIRE(on.size() == 1);  // pushed down onto the join: it reads both tables
    CHECK(on[0].args[0].col.table == cat.table({kPublicSchema, "u"})->id);
    CHECK(on[0].args[0].col.col == 1u);  // u.qty
    CHECK(on[0].args[1].col.col == 0u);  // t.id
    const auto& a = get<logical::Aggregate>(b).aggs.at(0).expr;
    CHECK(a.distinct);
    CHECK(a.args.at(0).col.table == cat.table({kPublicSchema, "u"})->id);
  }
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE id = k AND qty = id").first ==
        "column reference \"qty\" is ambiguous");
  {
    // Two columns of one table: a comparison of the two, a filter of its rows.
    auto b = bind_sql(cat, "SELECT id FROM t, u WHERE t.id = t.qty");
    CHECK(std::holds_alternative<logical::NodePtr>(b.stmt));
  }
  {
    // Any comparison of a column of each table joins them, as = does.
    auto b = bind_sql(cat, "SELECT id FROM t, u WHERE id < k");
    logical::rewrite(root(b));
    const auto& on = get<logical::Join>(b).predicate;
    REQUIRE(on.size() == 1);
    CHECK(on[0].op == ast::Compare::Op::Lt);
    CHECK(on[0].args[0].col.table == cat.table({kPublicSchema, "t"})->id);
    CHECK(on[0].args[1].col.table == cat.table({kPublicSchema, "u"})->id);
  }
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE name = k").first == "operator does not exist: bytea = bigint");
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE k > name").first == "operator does not exist: bigint > bytea");
  CHECK(rejected(cat, "SELECT u.qty, count(*) FROM t, u WHERE id = k").first ==
        "column \"u.qty\" must appear in the GROUP BY clause or be used in an aggregate function");
  CHECK(bind_sql(cat, "SELECT id FROM t JOIN u ON id = $1 + k WHERE k = $2").params.at(0).type == SqlType{SqlKind::BigInt});
  {
    // A range refusal names the table of the column the parameter is compared with.
    auto b = bind_sql(cat, "SELECT name FROM t, u WHERE id = $1 AND k = $2");
    CHECK(b.param_table == std::vector<std::uint32_t>{cat.table({kPublicSchema, "t"})->id, cat.table({kPublicSchema, "u"})->id});
  }
  //                       0         1         2
  //                       0123456789012345678901234567890
  CHECK(rejected(cat, "SELECT id FROM t, nope WHERE id = $1").second == 18);
  CHECK(rejected(cat, "UPDATE u SET d = d + 100000000000000000 WHERE k = $1").first == "numeric field overflow");
}


TEST_CASE("binder: each refusal carries PostgreSQL's SQLSTATE", "[sql][binder][sqlstate]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(),
                               "CREATE TABLE u (k BIGINT PRIMARY KEY, qty BIGINT, d NUMERIC(10, 2), dbl DOUBLE PRECISION, "
                               "s TEXT, ok BOOLEAN, tm TIME, ts TIMESTAMP)")
                        .stmt));
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE w (k BIGINT PRIMARY KEY, v VARCHAR(4), c CHAR(4), t TEXT)").stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  CHECK(state("SELECT id FROM nope WHERE id = $1") == "42P01");
  CHECK(state("SELECT id FROM t, nope WHERE id = $1") == "42P01");
  CHECK(state("SELECT x.id FROM t WHERE id = $1") == "42P01");  // missing FROM-clause entry
  CHECK(state("SELECT nope FROM t WHERE id = $1") == "42703");
  CHECK(state("SELECT id FROM t, u WHERE nope = $1") == "42703");
  CHECK(state("CREATE INDEX i ON t (nope)") == "42703");
  CHECK(state("SELECT id FROM t WHERE id = $2") == "42P18");
  CHECK(state("SELECT qty FROM t, u WHERE id = $1 AND k = $2") == "42702");
  CHECK(state("INSERT INTO t (id, id) VALUES ($1, $2)") == "42701");
  CHECK(state("SELECT count(*), id FROM t WHERE id = $1") == "42803");
  CHECK(state("UPDATE t SET qty = count(*) WHERE id = $1") == "42803");
  CHECK(state("SELECT sum(qty) FROM t WHERE id = $1 FOR UPDATE") == "0A000");
  CHECK(state("UPDATE u SET d = d + 100000000000000000 WHERE k = $1") == "22003");

  // A column assigned a column PostgreSQL has no assignment cast from: 42804.
  CHECK(state("UPDATE t SET name = qty WHERE id = $1") == "42804");
  CHECK(state("UPDATE u SET qty = s WHERE k = $1") == "42804");
  CHECK(state("UPDATE u SET ok = qty WHERE k = $1") == "42804");
  // A $n compared with two columns no operator relates: 42883.
  CHECK(state("SELECT id FROM t WHERE name = $1 AND qty = $1") == "42883");
  CHECK(state("SELECT k FROM u WHERE s = $1 AND qty = $1") == "42883");
  CHECK(state("SELECT k FROM u WHERE ts = $1 AND qty = $1") == "42883");
  CHECK(state("SELECT k FROM u WHERE ok = $1 AND s = $1") == "42883");
  // SUM and MIN where PostgreSQL has no such aggregate: 42883.
  CHECK(state("SELECT sum(name) FROM t WHERE id = $1") == "42883");
  CHECK(state("SELECT min(name) FROM t WHERE id = $1") == "42883");
  CHECK(state("SELECT sum(s) FROM u WHERE k = $1") == "42883");
  // An arithmetic operator PostgreSQL has none of: 42883; time + time, ambiguous there: 42725.
  CHECK(state("SELECT ts + ts FROM u") == "42883");
  CHECK(state("SELECT DATE '2026-01-01' + DATE '2026-01-01'") == "42883");
  CHECK(state("SELECT tm + tm FROM u") == "42725");
  CHECK(state("SELECT TIME '10:00' + TIME '10:00'") == "42725");
  CHECK(state("SELECT sum(ts) FROM u WHERE k = $1") == "42883");

  // PostgreSQL runs these: the layer refuses a form it does not support, and
  // does not claim a code. Syntax errors are 42601 too.
  CHECK(state("UPDATE u SET dbl = qty WHERE k = $1") == "bound");  // PostgreSQL 17's assignment casts (#904 S1)
  CHECK(state("UPDATE u SET s = qty WHERE k = $1") == "bound");
  CHECK(state("UPDATE u SET qty = dbl WHERE k = $1") == "bound");
  CHECK(state("UPDATE u SET s = dbl WHERE k = $1") == "0A000");  // a float's text is not float8out's yet
  CHECK(state("SELECT k FROM u WHERE d = $1 AND dbl = $1") == "42601");
  CHECK(state("UPDATE u SET dbl = $1, qty = $1 WHERE k = $2") == "42P08");  // PostgreSQL's (#904 S1)
  CHECK(state("UPDATE u SET s = $1, qty = $1 WHERE k = $2") == "42P08");
  CHECK(state("UPDATE u SET s = $1 WHERE qty = $1") == "42601");  // PostgreSQL types the WHERE's $1 first
  CHECK(state("UPDATE u SET qty = qty + $1 WHERE ts = $1") == "42601");
  CHECK(state("SELECT sum(dbl) FROM u WHERE k = $1") == "bound");
  CHECK(state("SELECT min(DISTINCT dbl), avg(ALL dbl) FROM u WHERE k = $1") == "bound");
  CHECK(state("SELECT sum(DISTINCT s) FROM u WHERE k = $1") == "42883");
  CHECK(state("SELECT min(s) FROM u WHERE k = $1") == "bound");  // bytewise (#904)
  CHECK(state("SELECT sum(tm) FROM u WHERE k = $1") == "42601");  // an interval sum there
  CHECK(state("SELEC 1") == "42601");
  // Two columns of one table: of one compared type, two numbers of no
  // DECIMAL, or an integer with a DOUBLE; DECIMAL with another number, not
  // yet; a pair PostgreSQL has no operator for, 42883.
  CHECK(state("SELECT id FROM t WHERE id = qty") == "bound");
  CHECK(state("SELECT k FROM u WHERE qty < dbl") == "bound");
  CHECK(state("SELECT k FROM u WHERE dbl <> k") == "bound");
  CHECK(state("SELECT k FROM u WHERE d = qty") == "42601");
  CHECK(state("SELECT k FROM u WHERE ts = s") == "42883");
  CHECK(state("SELECT k FROM u WHERE ts > tm") == "42883");
  CHECK(state("SELECT k FROM u WHERE ok = qty OR k = $1") == "42883");
  // The same pairs across two tables, in a WHERE, an ON or USING: 42883 too;
  // two numbers of different types joined, which PostgreSQL relates, not yet (#823).
  CHECK(state("SELECT a.k FROM u a JOIN w b ON a.s = b.k") == "42883");
  CHECK(state("SELECT a.k FROM u a, u b WHERE a.ts <> b.s") == "42883");
  CHECK(state("SELECT a.k FROM u a JOIN u b ON a.ts > b.tm OR a.k = b.k") == "42883");
  CHECK(state("SELECT k FROM t JOIN u USING (qty)") == "bound");
  CHECK(state("SELECT a.k FROM u a JOIN u b ON a.qty = b.dbl") == "42601");
  // A CHAR(n) with a VARCHAR, which PostgreSQL compares as bpchar: not yet, of one table or joined.
  CHECK(state("SELECT k FROM w WHERE v = c") == "42601");
  CHECK(state("SELECT a.k FROM w a JOIN w b ON a.v = b.c") == "42601");
  CHECK(state("SELECT a.k FROM w a JOIN w b ON a.c = b.v OR a.k = b.k") == "42601");
  CHECK(state("SELECT k FROM w WHERE c = t AND v < t") == "bound");
  CHECK(state("CREATE TABLE u2 (k BIGINT PRIMARY KEY, v TIMESTAMPTZ)") == "bound");
  CHECK(state("CREATE TABLE u3 (k BIGINT PRIMARY KEY, v TIME WITH TIME ZONE, w TIMETZ(6), x TIME WITH TIME ZONE DEFAULT CURRENT_TIME)") ==
        "bound");
  CHECK(state("CREATE TABLE u4 (k BIGINT PRIMARY KEY, v TIMETZ(0))") == "0A000");
  // The clock as a time with time zone's DEFAULT, as PostgreSQL's assignment casts take it.
  CHECK(state("CREATE TABLE u5 (k BIGINT PRIMARY KEY, v TIMETZ DEFAULT LOCALTIME, w TIMETZ DEFAULT CURRENT_TIMESTAMP)") == "bound");
  CHECK(state("CREATE TABLE u6 (k BIGINT PRIMARY KEY, v TIMETZ DEFAULT CURRENT_DATE)") == "42804");
  CHECK(state("CREATE TABLE u7 (k BIGINT PRIMARY KEY, v TIMETZ DEFAULT LOCALTIMESTAMP)") == "42804");
  CHECK(state("CREATE TABLE u8 (k BIGINT PRIMARY KEY, v DATE DEFAULT CURRENT_TIME)") == "42804");
  CHECK(state("CREATE TABLE u9 (k BIGINT PRIMARY KEY, v TIMESTAMPTZ DEFAULT CURRENT_TIME)") == "42804");
  CHECK(state("CREATE TABLE u10 (k BIGINT PRIMARY KEY, v TIME DEFAULT CURRENT_TIME)") == "bound");
}

TEST_CASE("binder: a set operation binds its arms alone and resolves their columns", "[sql][binder][setop]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(),
                               "CREATE TABLE u (k INTEGER PRIMARY KEY, n SMALLINT, big BIGINT, dbl DOUBLE PRECISION, "
                               "r REAL, s TEXT, v VARCHAR(5), d NUMERIC(10, 2), e NUMERIC(8, 3), ts TIMESTAMP, dt DATE)")
                        .stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  auto type_of = [&](std::string_view sql) {
    auto b = bind_sql(cat, sql);
    const auto* set = std::get_if<logical::SetOp>(&root(b).op);
    REQUIRE(set != nullptr);
    return set->outputs.at(0).expr.type.kind;
  };
  auto b = bind_sql(cat, "SELECT qty AS a, id FROM t UNION ALL SELECT big, k FROM u");
  const auto& set = std::get<logical::SetOp>(root(b).op);
  CHECK(set.kind == logical::SetOp::Kind::Union);
  CHECK(set.all);
  CHECK(set.outputs.size() == 2);
  CHECK(set.outputs[0].name == "a");  // the left arm's
  CHECK(std::holds_alternative<logical::Project>(set.left->op));
  CHECK(std::holds_alternative<logical::Project>(set.right->op));

  // A LIMIT over all of it is the root's.
  auto limited = bind_sql(cat, "SELECT qty FROM t UNION SELECT big FROM u LIMIT 4");
  REQUIRE(std::holds_alternative<logical::Limit>(root(limited).op));
  CHECK(std::get<logical::Limit>(root(limited).op).n == 4);
  CHECK(std::holds_alternative<logical::SetOp>(std::get<logical::Limit>(root(limited).op).input->op));

  // The type of a column both arms give.
  CHECK(type_of("SELECT n FROM u UNION SELECT k FROM u") == SqlKind::Integer);
  CHECK(type_of("SELECT k FROM u UNION SELECT n FROM u") == SqlKind::Integer);
  CHECK(type_of("SELECT k FROM u UNION SELECT big FROM u") == SqlKind::BigInt);
  CHECK(type_of("SELECT k FROM u UNION SELECT dbl FROM u") == SqlKind::Double);
  CHECK(type_of("SELECT dbl FROM u UNION SELECT r FROM u") == SqlKind::Double);
  CHECK(type_of("SELECT big FROM u UNION SELECT dbl FROM u") == SqlKind::Double);
  CHECK(type_of("SELECT s FROM u UNION SELECT v FROM u") == SqlKind::Text);
  CHECK(type_of("SELECT v FROM u UNION SELECT v FROM u") == SqlKind::Varchar);
  CHECK(type_of("SELECT d FROM u UNION SELECT d FROM u") == SqlKind::Decimal);
  CHECK(type_of("SELECT dt FROM u INTERSECT SELECT dt FROM u") == SqlKind::Date);
  CHECK(type_of("SELECT 1 UNION SELECT 2147483648") == SqlKind::BigInt);

  CHECK(state("SELECT qty FROM t UNION SELECT big, k FROM u") == "42601");  // a different number of columns
  CHECK(state("SELECT qty FROM t INTERSECT SELECT s FROM u") == "42804");
  CHECK(state("SELECT s FROM u EXCEPT SELECT dbl FROM u") == "42804");
  CHECK(state("SELECT ts FROM u UNION SELECT k FROM u") == "42804");
  CHECK(state("SELECT ts FROM u UNION SELECT dt FROM u") == "42601");  // one class, not resolved yet
  CHECK(state("SELECT k FROM u UNION SELECT r FROM u") == "42601");  // an integer rounded to a REAL: not yet
  CHECK(state("SELECT d FROM u UNION SELECT e FROM u") == "42601");
  CHECK(state("SELECT d FROM u UNION SELECT k FROM u") == "42601");
  CHECK(state("SELECT qty FROM t UNION SELECT 'a'") == "42601");  // a literal of no type
  CHECK(state("SELECT qty FROM t UNION SELECT NULL") == "42601");
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM u ORDER BY qty") == "bound");
  CHECK(state("(SELECT qty FROM t UNION SELECT big FROM u ORDER BY qty) UNION SELECT k FROM u") == "42601");
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM u ORDER BY 3") == "42P10");
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM u ORDER BY big") == "42703");
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM nope") == "42P01");
  CHECK(state("SELECT qty FROM t UNION SELECT nope FROM u") == "42703");
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM u FOR UPDATE") == "0A000");
  CHECK(state("(SELECT qty FROM t FOR UPDATE) UNION SELECT big FROM u") == "0A000");
  CHECK(state("SELECT DISTINCT qty FROM t UNION SELECT big FROM u") == "42601");
  CHECK(state("SELECT COUNT(*) FROM t UNION SELECT big FROM u") == "42601");
  CHECK(state("SELECT qty FROM t GROUP BY qty UNION SELECT big FROM u") == "42601");
  CHECK(state("(SELECT qty FROM t UNION SELECT big FROM u LIMIT 1) UNION SELECT k FROM u") == "42601");
  CHECK(state("(SELECT qty FROM t UNION SELECT big FROM u) UNION SELECT k FROM u LIMIT 1") == "bound");

  // In a subquery: of one column, uncorrelated, not inside another.
  CHECK(state("SELECT id FROM t WHERE qty IN (SELECT big FROM u UNION SELECT k FROM u)") == "bound");
  CHECK(state("SELECT id FROM t WHERE qty IN (SELECT big, k FROM u UNION SELECT k, k FROM u)") == "42601");
  CHECK(state("SELECT id FROM t WHERE EXISTS (SELECT big, k FROM u UNION SELECT k, k FROM u)") == "bound");
  CHECK(state("SELECT id FROM t WHERE qty IN (SELECT big FROM u WHERE k = id UNION SELECT k FROM u)") == "42601");  // correlated
  CHECK(state("SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k = t.id) UNION SELECT k FROM u") == "42601");  // in an arm
  CHECK(state("SELECT id FROM t WHERE qty IN (SELECT k FROM u WHERE big IN (SELECT k FROM u UNION SELECT k FROM u) UNION SELECT k FROM u)") ==
        "42601");  // a subquery inside one
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM u WHERE big IN (SELECT k FROM u UNION SELECT n FROM u)") == "bound");

  // Each arm is rewritten alone: the Filter of a WHERE reaches its Get.
  auto filtered = bind_sql(cat, "SELECT qty FROM t WHERE id = $1 UNION SELECT big FROM u WHERE k = $2 LIMIT 2");
  logical::rewrite(root(filtered));
  const auto& top = std::get<logical::SetOp>(std::get<logical::Limit>(root(filtered).op).input->op);
  CHECK(find<logical::Filter>(*top.left) != nullptr);
  CHECK(find<logical::Filter>(*top.right) != nullptr);
  CHECK(find<logical::Get>(*top.right)->table->name == "u");
}

TEST_CASE("binder: MIN and MAX of text, varchar, name and a CHAR(n) column, typed as PostgreSQL 17's", "[sql][binder][group]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE m (k INT PRIMARY KEY, s TEXT, v VARCHAR(5), n NAME, ch CHAR(3), "
                                        "b BYTEA, j JSONB)").stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  {
    auto b = bind_sql(cat, "SELECT max(s), min(v), max(n), min(ch), max(j->>'k'), max(DISTINCT s) FROM m");
    const auto& a = get<logical::Aggregate>(b);
    REQUIRE(a.aggs.size() == 6);
    // PostgreSQL has max(text) and max(bpchar): a VARCHAR's and a NAME's are text.
    for (auto i : {0, 1, 2, 4, 5}) CHECK(a.aggs[static_cast<std::size_t>(i)].expr.type == SqlType{SqlKind::Text});
    // A CHAR(n)'s is bpchar of no length; the result pads it to the column's n.
    CHECK(a.aggs[3].expr.type == SqlType{SqlKind::Char});
    CHECK(a.aggs[3].name == "min");
    CHECK_FALSE(a.aggs[5].expr.distinct);  // MIN and MAX of the distinct values are of all
  }
  // HAVING compares MIN or MAX of text with a text parameter.
  {
    auto b = bind_sql(cat, "SELECT k FROM m GROUP BY k HAVING max(v) > $1");
    REQUIRE(b.params.size() == 1);
    CHECK(b.params[0].type == SqlType{SqlKind::Text});
  }
  CHECK(state("SELECT k FROM m GROUP BY k HAVING max(ch) > $1") == "42601 a parameter compared with a CHAR(n) column is not supported yet");
  CHECK(state("SELECT k FROM m GROUP BY k HAVING max(ch) > 'a'") == "bound");
  // PostgreSQL compares a CHAR beside a text constant as text, trailing
  // spaces counting; a HAVING has no such comparison, so it is refused
  // rather than compared as the CHAR. A varchar constant compares as the CHAR.
  CHECK(state("SELECT k FROM m GROUP BY k HAVING max(ch) = ' '::text") ==
        "0A000 HAVING comparing a CHAR(n) column, or MIN or MAX of one, with a text value is not supported yet");
  CHECK(state("SELECT k FROM m GROUP BY k HAVING min(ch) < 'a' || ''").starts_with("0A000 "));
  CHECK(state("SELECT ch FROM m GROUP BY ch HAVING ch = ' '::text").starts_with("0A000 "));
  CHECK(state("SELECT k FROM m GROUP BY k HAVING max(ch) = 'ab '::varchar") == "bound");
  // PostgreSQL's 42883 for what it has no aggregate of, at the call.
  CHECK(rejected(cat, "SELECT max(b) FROM m") == std::pair<std::string, std::int32_t>{"function max(bytea) does not exist", 7});
  CHECK(state("SELECT min(j) FROM m") == "42883 function min(jsonb) does not exist");
  CHECK(state("SELECT sum(s) FROM m") == "42883 function sum(text) does not exist");
  CHECK(state("SELECT avg(v) FROM m") == "42883 function avg(character varying) does not exist");
  CHECK(state("SELECT sum(ch) FROM m") == "42883 function sum(character) does not exist");
  // A CHAR-typed expression is not taken (#904, open question 9).
  CHECK(rejected(cat, "SELECT max(coalesce(ch, ch)) FROM m") ==
        std::pair<std::string, std::int32_t>{"MAX of a CHAR expression other than a column is not supported yet", 7});
  CHECK(state("SELECT max(coalesce(ch, ch)) FROM m").starts_with("0A000 "));
  // Where the n it is padded to would be lost: 0A000.
  CHECK(state("SELECT (SELECT max(ch) FROM m)").starts_with("0A000 "));
  CHECK(state("SELECT k FROM m WHERE ch = (SELECT max(ch) FROM m)") == "bound");
}

TEST_CASE("binder: GROUP BY, HAVING and the aggregates, as PostgreSQL binds them", "[sql][binder][group]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE g (k BIGINT PRIMARY KEY, a INTEGER, d NUMERIC(6,2), "
                                        "f DOUBLE PRECISION, s TEXT, day DATE)").stmt));
  const auto& cat = f.cat();
  bound::Bound held;
  auto grouped = [&](std::string_view sql) -> const logical::Aggregate& {
    held = bind_sql(cat, sql);
    return get<logical::Aggregate>(held);
  };
  {
    const auto& a = grouped("SELECT a, count(*), sum(d), max(a), avg(a), avg(d) FROM g GROUP BY a, a");
    CHECK(a.grouped);
    CHECK(a.group_by.size() == 1);  // each once
    REQUIRE(a.aggs.size() == 6);
    CHECK(a.aggs[0].expr.kind == logical::Expr::Kind::Column);
    CHECK(a.aggs[2].name == "sum");
    CHECK(sql_type_name(a.aggs[3].expr.type) == "integer");
    CHECK(a.aggs[4].name == "avg");
    CHECK(sql_type_name(a.aggs[4].expr.type) == "numeric");
    CHECK(a.aggs[5].expr.type.scale == 2);
  }
  // A column of a table its primary key groups is dependent: PostgreSQL's
  // functional dependency. It is read from a group's row, not grouped by.
  {
    const auto& a = grouped("SELECT k, s, count(*) FROM g GROUP BY k");
    CHECK(a.group_by.size() == 1);
    REQUIRE(a.dependent.size() == 1);  // each once
    CHECK(a.dependent[0] == a.aggs[1].expr);
  }
  // HAVING's parameter takes the aggregate's type, or the column's: a Filter
  // above the Aggregate.
  {
    auto b = bind_sql(cat, "SELECT a FROM g GROUP BY a HAVING count(*) > $1 AND sum(d) < $2 AND min(day) = $3 AND $4 <= a");
    REQUIRE(b.params.size() == 4);
    CHECK(sql_type_name(b.params[0].type) == "bigint");
    CHECK(b.params[1].type.kind == SqlKind::Decimal);
    CHECK(b.params[1].type.scale == 2);
    CHECK(sql_type_name(b.params[2].type) == "date");
    CHECK(sql_type_name(b.params[3].type) == "integer");
    const auto& having = std::get<logical::Filter>(root(b).op);
    CHECK(std::holds_alternative<logical::Aggregate>(having.input->op));
    CHECK(having.conjuncts[3].op == ast::Compare::Op::Ge);  // a >= $4
  }
  // A $n the WHERE types keeps its type in the HAVING, as PostgreSQL types
  // it by its first use: an INTEGER compared with COUNT(*), a SUM or a key.
  // A DECIMAL of scale 2 is no integer: compared with COUNT(*), refused.
  {
    auto b = bind_sql(cat, "SELECT a FROM g WHERE a > $1 GROUP BY a HAVING count(*) > $1 AND sum(a) < $1 AND a >= $1");
    REQUIRE(b.params.size() == 1);
    CHECK(sql_type_name(b.params[0].type) == "integer");
    CHECK(rejected(cat, "SELECT a FROM g WHERE d > $1 GROUP BY a HAVING count(*) > $1").first ==
          "parameter $1 is compared with d (numeric(6,2)) and count (bigint)");
  }
  // An ORDER BY sorts the groups, above the Aggregate.
  CHECK(std::holds_alternative<logical::Sort>(root(bind_sql(cat, "SELECT a FROM g GROUP BY a ORDER BY a")).op));
  // A column neither grouped nor of a table its primary key groups: 42803,
  // in the select list, ORDER BY and HAVING.
  const auto* not_grouped = "column \"g.s\" must appear in the GROUP BY clause or be used in an aggregate function";
  CHECK(rejected(cat, "SELECT s, count(*) FROM g GROUP BY a") == std::pair<std::string, std::int32_t>{not_grouped, 7});
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a ORDER BY s").first == not_grouped);
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a HAVING s = $1").first == not_grouped);
  CHECK(rejected(cat, "SELECT s FROM g HAVING count(*) > $1").first == not_grouped);
  // FOR UPDATE, in PostgreSQL's order of checks.
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a FOR UPDATE").first == "FOR UPDATE is not allowed with GROUP BY clause");
  CHECK(rejected(cat, "SELECT count(*) FROM g HAVING count(*) > $1 FOR UPDATE").first ==
        "FOR UPDATE is not allowed with HAVING clause");
  // The aggregates' types, as PostgreSQL refuses them: 42883 where it has no
  // such aggregate, 42601 where the layer does not take it yet.
  CHECK(rejected(cat, "SELECT avg(day) FROM g").first == "function avg(date) does not exist");
  {
    // SUM, AVG, MIN and MAX of a DOUBLE are a DOUBLE; DISTINCT stays on SUM and AVG, and
    // is no change to MIN and MAX.
    const auto& agg = grouped("SELECT sum(f), avg(DISTINCT f), min(DISTINCT f), max(ALL f) FROM g");
    REQUIRE(agg.aggs.size() == 4);
    for (const auto& o : agg.aggs) CHECK(o.expr.type.kind == SqlKind::Double);
    CHECK_FALSE(agg.aggs[0].expr.distinct);
    CHECK(agg.aggs[1].expr.distinct);
    CHECK_FALSE(agg.aggs[2].expr.distinct);
    CHECK_FALSE(agg.aggs[3].expr.distinct);
  }
  // HAVING on an aggregate of a DOUBLE: its $n a double precision, as the
  // aggregate is; COUNT's a bigint.
  CHECK(bind_sql(cat, "SELECT a FROM g GROUP BY a HAVING sum(f) > $1").param_types == std::vector<PhysType>{PhysType::F64});
  CHECK(bind_sql(cat, "SELECT a FROM g GROUP BY a HAVING max(f) <= $1").param_types == std::vector<PhysType>{PhysType::F64});
  CHECK(bind_sql(cat, "SELECT a FROM g GROUP BY a HAVING count(f) > $1").param_types == std::vector<PhysType>{PhysType::I64});
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a HAVING avg(a) > $1").first == "HAVING on AVG is not supported yet");
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a + 1").first ==
        "column \"g.a\" must appear in the GROUP BY clause or be used in an aggregate function");
}

TEST_CASE("binder: GROUP BY and DISTINCT of expressions, matched as written (#904 G1)", "[sql][binder][group]") {
  sql_test::Fixture f;
  auto ddl = [&](std::string_view sql) { f.ddl(std::get<Ddl>(bind_sql(f.cat(), sql).stmt)); };
  ddl("CREATE TABLE g9 (id BIGINT PRIMARY KEY, a INTEGER, b TEXT, s TEXT, d DOUBLE PRECISION, j JSONB, ch CHAR(3), n NUMERIC)");
  ddl("CREATE VIEW g9v AS SELECT a, count(*) AS k FROM g9 GROUP BY a");
  const auto& cat = f.cat();
  using Kind = logical::Expr::Kind;
  auto error_of = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what() + " @" + std::to_string(e.pos());
    }
    return "bound";
  };
  // A key is computed per row; an item written alike is its GroupKey, never
  // a copy of its tree, and named as in a select list.
  auto b = bind_sql(cat, "SELECT lower(b), count(*) FROM g9 GROUP BY lower(g9.b) HAVING lower(b) > $1 ORDER BY lower(b)");
  const auto& agg = get<logical::Aggregate>(b);
  REQUIRE(agg.group_by.size() == 1);
  CHECK(agg.group_by[0].kind == Kind::TextFn);
  CHECK(agg.aggs[0].expr.kind == Kind::GroupKey);
  CHECK(agg.aggs[0].expr.constant == 0);
  CHECK(agg.aggs[0].expr.type == SqlType{SqlKind::Text});
  CHECK(agg.aggs[0].name == "lower");
  const auto* having = find<logical::Filter>(root(b));
  REQUIRE(having != nullptr);
  CHECK(having->conjuncts[0].args[0].kind == Kind::GroupKey);
  CHECK(b.param_types == std::vector<PhysType>{PhysType::Bytes});
  const auto* sort = find<logical::Sort>(root(b));
  REQUIRE(sort != nullptr);
  CHECK(sort->keys[0].expr.kind == Kind::GroupKey);
  // A position or an output name naming an expression item; keys kept once
  // each, a column beside them a column key.
  auto mixed = bind_sql(cat, "SELECT s || 'x' AS k, a, count(*) FROM g9 GROUP BY k, 2, s || 'x', a + 1");
  const auto& m = get<logical::Aggregate>(mixed);
  REQUIRE(m.group_by.size() == 3);
  CHECK(m.group_by[0].kind == Kind::Concat);
  CHECK(m.group_by[1].kind == Kind::Column);
  CHECK(m.group_by[2].kind == Kind::Add);
  CHECK(m.aggs[0].expr.kind == Kind::GroupKey);
  CHECK(m.aggs[0].name == "k");
  CHECK(m.aggs[1].expr.kind == Kind::Column);
  // A key the same for every row stays a key: the query stays grouped, so
  // it gives no group over no rows (PostgreSQL 17).
  for (const auto* sql : {"SELECT count(*) FROM g9 GROUP BY 1+1", "SELECT count(*) FROM g9 GROUP BY $1",
                          "SELECT 1, count(*) FROM g9 GROUP BY 1", "SELECT DISTINCT 1 FROM g9"}) {
    auto k = bind_sql(cat, sql);
    CHECK(get<logical::Aggregate>(k).grouped);
    CHECK(get<logical::Aggregate>(k).group_by.size() == 1);
  }
  CHECK(bind_sql(cat, "SELECT count(*) FROM g9 GROUP BY $1").param_types == std::vector<PhysType>{PhysType::Bytes});
  // DISTINCT of expressions: each item a key.
  auto distinct = bind_sql(cat, "SELECT DISTINCT j->>'k', lower(b) FROM g9 ORDER BY 2");
  CHECK(get<logical::Aggregate>(distinct).group_by.size() == 2);
  CHECK(get<logical::Aggregate>(distinct).aggs[0].name == "?column?");
  CHECK(error_of("SELECT DISTINCT lower(b) FROM g9 ORDER BY b") ==
        "42P10 for SELECT DISTINCT, ORDER BY expressions must appear in select list @42");
  // An ungrouped column outside a key is 42803 at it, found before the
  // 42601 of an expression over keys (stage P3), as PostgreSQL 17 finds it.
  const auto ungrouped_b = std::string{"42803 column \"g9.b\" must appear in the GROUP BY clause or be used in an aggregate function"};
  CHECK(error_of("SELECT upper(b) FROM g9 GROUP BY lower(b)") == ungrouped_b + " @13");
  CHECK(error_of("SELECT b FROM g9 GROUP BY lower(b)") == ungrouped_b + " @7");
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY lower(b) HAVING upper(b) > 'a'") == ungrouped_b + " @55");
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY lower(b) ORDER BY b || 'x'") == ungrouped_b + " @51");
  const auto ungrouped_a = std::string{"42803 column \"g9.a\" must appear in the GROUP BY clause or be used in an aggregate function"};
  CHECK(error_of("SELECT a + 1 FROM g9 GROUP BY 1+a") == ungrouped_a + " @7");
  CHECK(error_of("SELECT a + 1.0 FROM g9 GROUP BY a + 1") == ungrouped_a + " @7");
  CHECK(error_of("SELECT a + 1.0 FROM g9 GROUP BY a + 1.00") == ungrouped_a + " @7");
  CHECK(error_of("SELECT a FROM g9 GROUP BY 1::int") == ungrouped_a + " @7");
  CHECK(error_of("SELECT a + 01 FROM g9 GROUP BY a + 1") == "bound");
  // A grouped primary key determines its columns beside an expression key;
  // an expression key determines nothing.
  CHECK(error_of("SELECT id, a, lower(b) FROM g9 GROUP BY 1, lower(b)") == "bound");
  CHECK(error_of("SELECT id, a FROM g9 GROUP BY id + 0") ==
        "42803 column \"g9.id\" must appear in the GROUP BY clause or be used in an aggregate function @7");
  // A key PostgreSQL binds to the column itself, a cast to the column's own
  // type, groups the column; any other key that binds to a column (unary +,
  // a relabel to another type) is an expression the column is not: 42803.
  CHECK(error_of("SELECT a, count(*) FROM g9 GROUP BY a::int4") == "bound");
  CHECK(error_of("SELECT a FROM g9 GROUP BY a::integer::int4 HAVING a > 1 ORDER BY a") == "bound");
  CHECK(error_of("SELECT s FROM g9 GROUP BY s::text") == "bound");
  CHECK(error_of("SELECT id, a FROM g9 GROUP BY id::int8") == "bound");
  CHECK(error_of("SELECT a::int4, count(*) FROM g9 GROUP BY a") == "bound");
  CHECK(error_of("SELECT a, count(*) FROM g9 GROUP BY +a") == ungrouped_a + " @7");
  CHECK(error_of("SELECT +a FROM g9 GROUP BY 1 ORDER BY a") == ungrouped_a + " @38");
  CHECK(error_of("SELECT a FROM g9 GROUP BY +a HAVING a > 1") == ungrouped_a + " @7");
  CHECK(error_of("SELECT +a, count(*) FROM g9 GROUP BY +a HAVING a > 1") == ungrouped_a + " @47");
  CHECK(error_of("SELECT +a, count(*) FROM g9 GROUP BY +a") == "bound");
  CHECK(error_of("SELECT s, count(*) FROM g9 GROUP BY s::varchar") ==
        "42803 column \"g9.s\" must appear in the GROUP BY clause or be used in an aggregate function @7");
  CHECK(error_of("SELECT id, a FROM g9 GROUP BY +id") ==
        "42803 column \"g9.id\" must appear in the GROUP BY clause or be used in an aggregate function @7");
  CHECK(error_of("SELECT a::int8 FROM g9 GROUP BY a::int4") ==
        "42601 an expression in the select list of an aggregated query is not supported yet @8");
  // Not yet: an expression over keys or aggregates (P3), over a dependent
  // column, and ORDER BY of AVG.
  CHECK(error_of("SELECT lower(b) || 'z' FROM g9 GROUP BY lower(b)") ==
        "42601 an expression in the select list of an aggregated query is not supported yet @16");
  CHECK(error_of("SELECT lower(b) FROM g9 GROUP BY id") ==
        "42601 an expression in the select list of an aggregated query is not supported yet @7");
  CHECK(error_of("SELECT a FROM g9 GROUP BY a ORDER BY avg(id)").starts_with("42601 ORDER BY AVG"));
  // ORDER BY an aggregate sorts the groups.
  CHECK(error_of("SELECT a FROM g9 GROUP BY a ORDER BY count(*) DESC, max(b)") == "bound");
  // Keys no sort key holds or groups as PostgreSQL does, and what reads
  // again or per group.
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY j->'k'").starts_with("0A000 GROUP BY of a jsonb value is not supported yet @"));
  CHECK(error_of("SELECT DISTINCT j->'k' FROM g9").starts_with("0A000 DISTINCT of a jsonb value is not supported yet @"));
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY coalesce(n, n)").starts_with("0A000 "));
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY coalesce(ch, ch)") ==
        "0A000 GROUP BY of a CHAR expression other than a column is not supported yet @33");
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY random()") == "42601 GROUP BY random() is not supported yet @33");
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY a + (SELECT 1)").starts_with("42601 a subquery in GROUP BY"));
  CHECK(error_of("SELECT count(*) FROM g9 GROUP BY now()") == "42601 the time in GROUP BY is not supported yet @33");
  CHECK(error_of("SELECT k + 1, count(*) FROM g9v GROUP BY k + 1").starts_with(
      "42601 GROUP BY of an expression of a grouped view's columns"));
}

TEST_CASE("binder: COUNT, SUM and AVG of DISTINCT values of an expression (#904 G2)", "[sql][binder][group]") {
  sql_test::Fixture f;
  auto ddl = [&](std::string_view sql) { f.ddl(std::get<Ddl>(bind_sql(f.cat(), sql).stmt)); };
  ddl("CREATE TABLE g2 (id BIGINT PRIMARY KEY, a INTEGER, b TEXT, j JSONB, ch CHAR(3), n NUMERIC)");
  const auto& cat = f.cat();
  using Kind = logical::Expr::Kind;
  auto error_of = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what() + " @" + std::to_string(e.pos());
    }
    return "bound";
  };
  // The argument is bound as a select item's expression, the call typed by
  // its type.
  auto b = bind_sql(cat, "SELECT count(DISTINCT j->>'u'), sum(DISTINCT a + 1), avg(DISTINCT a + 1) FROM g2");
  const auto& agg = get<logical::Aggregate>(b);
  REQUIRE(agg.aggs.size() == 3);
  CHECK(agg.aggs[0].expr.distinct);
  CHECK(agg.aggs[0].expr.args[0].kind == Kind::Jsonb);
  CHECK(agg.aggs[0].expr.type == SqlType{SqlKind::BigInt});
  // Two calls over arguments written alike share one bound copy, its
  // literal's parameter the first's.
  CHECK(agg.aggs[1].expr.args[0].kind == Kind::Add);
  CHECK(agg.aggs[1].expr.args[0] == agg.aggs[2].expr.args[0]);
  auto two = bind_sql(cat, "SELECT sum(DISTINCT a + 1), sum(DISTINCT g2.a + 1), count(DISTINCT a + 2), count(a + 1) FROM g2");
  const auto& calls = get<logical::Aggregate>(two).aggs;
  REQUIRE(calls.size() == 4);
  CHECK(calls[0].expr.args[0] == calls[1].expr.args[0]);
  CHECK(calls[0].expr.args[0] != calls[2].expr.args[0]);
  // One that draws random() is never shared: each call draws its own, as
  // PostgreSQL shares no volatile aggregate.
  auto drawn = bind_sql(cat, "SELECT sum(DISTINCT random(1, 9)), sum(DISTINCT random(1, 9)) FROM g2");
  const auto& draws = get<logical::Aggregate>(drawn).aggs;
  REQUIRE(draws.size() == 2);
  CHECK(draws[0].expr.args[0].kind == Kind::Random);
  CHECK(draws[0].expr.args[0] != draws[1].expr.args[0]);
  CHECK(calls[0].expr.args[0] != calls[3].expr.args[0]);
  CHECK(error_of("SELECT a % 2, count(DISTINCT lower(b)) FROM g2 GROUP BY 1") == "bound");
  // Refused as PostgreSQL refuses them, or as the argument's type is.
  CHECK(error_of("SELECT sum(DISTINCT lower(b)) FROM g2").starts_with("42883 function sum(text) does not exist"));
  CHECK(error_of("SELECT count(DISTINCT j->'u') FROM g2").starts_with("0A000 COUNT of DISTINCT jsonb values"));
  CHECK(error_of("SELECT count(DISTINCT coalesce(n, n)) FROM g2").starts_with("0A000 "));
  CHECK(error_of("SELECT count(DISTINCT coalesce(ch, ch)) FROM g2") ==
        "0A000 COUNT of DISTINCT values of a CHAR expression other than a column is not supported yet @7");
  CHECK(error_of("SELECT count(DISTINCT ch) FROM g2") == "bound");
  CHECK(error_of("SELECT sum(DISTINCT coalesce(ch, ch)) FROM g2").starts_with("42883 function sum(character) does not exist"));
  CHECK(error_of("SELECT count(DISTINCT a + count(*)) FROM g2").starts_with("42803 aggregate function calls cannot be nested"));
}

TEST_CASE("binder: a GROUP BY name is an input column first, else an output name (#904 G0)", "[sql][binder][group]") {
  sql_test::Fixture f;
  auto ddl = [&](std::string_view sql) { f.ddl(std::get<Ddl>(bind_sql(f.cat(), sql).stmt)); };
  ddl("CREATE TABLE g0 (id BIGINT PRIMARY KEY, a INTEGER, b TEXT, c INTEGER)");
  ddl("CREATE TABLE g1 (id BIGINT PRIMARY KEY, d INTEGER)");
  ddl("CREATE VIEW gv AS SELECT a AS x, b FROM g0");
  ddl("CREATE VIEW gk AS SELECT a AS k, count(*) AS n FROM g0 GROUP BY k");
  const auto& cat = f.cat();
  bound::Bound held;
  auto keys = [&](std::string_view sql) -> std::vector<std::string> {
    held = bind_sql(cat, sql);
    const auto* agg = find<logical::Aggregate>(root(held));
    REQUIRE(agg != nullptr);
    std::vector<std::string> out;
    for (const auto& g : agg->group_by) {
      const auto* t = cat.table(g.col.table);
      out.push_back(t == nullptr ? "?" : t->column(g.col.col)->name);
    }
    return out;
  };
  auto error_of = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what() + " @" + std::to_string(e.pos());
    }
    return "bound";
  };
  // An output name no input column has is its item; an input column wins
  // (PostgreSQL 17 groups SELECT a AS id … GROUP BY id by the table's id).
  CHECK(keys("SELECT a AS k, count(*) FROM g0 GROUP BY k") == std::vector<std::string>{"a"});
  CHECK(keys("SELECT a AS id, count(*) FROM g0 GROUP BY id") == std::vector<std::string>{"id"});
  CHECK(keys("SELECT a AS k, count(*) FROM g0 GROUP BY \"k\", 1") == std::vector<std::string>{"a"});
  CHECK(keys("SELECT a AS k, g0.a AS k FROM g0 GROUP BY k") == std::vector<std::string>{"a"});
  CHECK(keys("SELECT g0.a AS k, count(*) FROM g0 JOIN g1 ON g0.id = g1.id GROUP BY k") == std::vector<std::string>{"a"});
  // An output name over a join's other column is still the input column.
  CHECK(error_of("SELECT a AS d, count(*) FROM g0 JOIN g1 ON g0.id = g1.id GROUP BY d") ==
        "42803 column \"g0.a\" must appear in the GROUP BY clause or be used in an aggregate function @7");
  // In a subquery, and with ORDER BY and HAVING of the item.
  (void)bind_sql(cat, "SELECT id FROM g1 WHERE d IN (SELECT a AS k FROM g0 GROUP BY k)");
  (void)bind_sql(cat, "SELECT a AS k, count(*) FROM g0 GROUP BY k HAVING a > $1 ORDER BY k");
  // Through a view: its columns are the input columns, before it is merged.
  CHECK(keys("SELECT x AS k, count(*) FROM gv GROUP BY k") == std::vector<std::string>{"a"});
  CHECK(keys("SELECT x, count(*) FROM gv GROUP BY 1") == std::vector<std::string>{"a"});
  CHECK(error_of("SELECT x AS b, count(*) FROM gv GROUP BY b").starts_with("42803 "));
  // A view's own GROUP BY of an output name.
  CHECK(keys("SELECT k, n FROM gk") == std::vector<std::string>{"a"});
  // PostgreSQL 17's errors, at its positions.
  CHECK(error_of("SELECT a AS k, c AS k FROM g0 GROUP BY k") == "42702 GROUP BY \"k\" is ambiguous @39");
  CHECK(error_of("SELECT count(*) AS k FROM g0 GROUP BY k") == "42803 aggregate functions are not allowed in GROUP BY @7");
  CHECK(error_of("SELECT zz AS k FROM g0 GROUP BY k") == "42703 column \"zz\" does not exist @7");
  CHECK(error_of("SELECT a AS \"K\" FROM g0 GROUP BY k") == "42703 column \"k\" does not exist @33");
  CHECK(error_of("SELECT b AS k, a AS zz FROM g0 GROUP BY zz") ==
        "42803 column \"g0.b\" must appear in the GROUP BY clause or be used in an aggregate function @7");
  CHECK(error_of("SELECT a AS k FROM g0 GROUP BY k HAVING k > 1").starts_with("42703 "));
  // An expression item is its expression's key (G1).
  CHECK(error_of("SELECT a + 1 AS k FROM g0 GROUP BY k") == "bound");
  // A table's system columns are input columns PostgreSQL groups by: the
  // layer has none, so the name is refused, never an output name.
  CHECK(error_of("SELECT a AS ctid, count(*) FROM g0 GROUP BY ctid") == "42703 column \"ctid\" does not exist @44");
  CHECK(error_of("SELECT a AS xmin, count(*) FROM g0 GROUP BY xmin").starts_with("42703 "));
  CHECK(keys("SELECT x AS ctid, count(*) FROM gv GROUP BY ctid") == std::vector<std::string>{"a"});
  // An unaliased item's name is PostgreSQL's FigureColname's.
  CHECK(error_of("SELECT count(*) FROM g0 GROUP BY count") == "42803 aggregate functions are not allowed in GROUP BY @7");
  CHECK(error_of("SELECT max(c) FROM g0 GROUP BY max") == "42803 aggregate functions are not allowed in GROUP BY @7");
  CHECK(error_of("SELECT upper(b) FROM g0 GROUP BY upper") == "bound");
  CHECK(error_of("SELECT count(*) AS n FROM g0 GROUP BY count") == "42703 column \"count\" does not exist @38");
  // A USING column is the merged one: the left side's of an inner or LEFT
  // join, the right table's of a RIGHT join.
  CHECK(keys("SELECT g0.id AS z, id AS z FROM g0 JOIN g1 USING (id) GROUP BY z") == std::vector<std::string>{"id"});
  CHECK(error_of("SELECT g1.id AS z, id AS z FROM g0 JOIN g1 USING (id) GROUP BY z") == "42702 GROUP BY \"z\" is ambiguous @63");
  CHECK(error_of("SELECT g1.id AS z, id AS z FROM g0 LEFT JOIN g1 USING (id) GROUP BY z").starts_with("42702 "));
  CHECK(error_of("SELECT g0.id AS z, id AS z FROM g0 RIGHT JOIN g1 USING (id) GROUP BY z").starts_with("42702 "));
  CHECK(error_of("SELECT g0.id AS z, id AS z FROM g0 RIGHT JOIN g1 USING (id) ORDER BY z").starts_with("42702 "));
  CHECK(error_of("SELECT g1.id AS z, id AS z FROM g0 NATURAL JOIN g1 ORDER BY z").starts_with("42601 "));
  (void)bind_sql(cat, "SELECT g1.id AS z, id AS z FROM g0 RIGHT JOIN g1 USING (id) ORDER BY z");
  (void)bind_sql(cat, "SELECT g0.id AS z, id AS z FROM g0 NATURAL JOIN g1 ORDER BY z");
  // A subquery's output name the query around it has as a column is still
  // the subquery's item, not a correlation.
  (void)bind_sql(cat, "SELECT id FROM g1 WHERE id IN (SELECT a AS d FROM g0 GROUP BY d)");
  (void)bind_sql(cat, "SELECT id FROM g1 WHERE EXISTS (SELECT a AS d FROM g0 GROUP BY d)");
  (void)bind_sql(cat, "SELECT id FROM g1 WHERE id IN (SELECT x AS d FROM gv GROUP BY d)");
}

TEST_CASE("binder: a chain of joins is a left-deep tree, each ON bound against the tables before it", "[sql][binder][join]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE u (k BIGINT PRIMARY KEY, qty BIGINT)").stmt));
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE w (j BIGINT PRIMARY KEY, qty BIGINT)").stmt));
  const auto& cat = f.cat();
  auto tables_of = [](const logical::Node& n) {
    std::vector<std::string> out;
    std::function<void(const logical::Node&)> walk = [&](const logical::Node& x) {
      if (const auto* g = std::get_if<logical::Get>(&x.op)) out.push_back(g->table->name);
      if (const auto* j = std::get_if<logical::Join>(&x.op)) {
        walk(*j->left);
        walk(*j->right);
      }
      if (const auto* in = logical::input_of(x); in != nullptr && *in) walk(**in);
    };
    walk(n);
    return out;
  };
  {
    // ((t, u), w): the second Join's left side is the first.
    auto b = bind_sql(cat, "SELECT t.id FROM t JOIN u ON t.qty = u.qty JOIN w ON u.qty = w.qty");
    const auto& top = get<logical::Join>(b);
    REQUIRE(std::holds_alternative<logical::Join>(top.left->op));
    CHECK(std::get<logical::Get>(top.right->op).table->name == "w");
    CHECK(tables_of(root(b)) == std::vector<std::string>{"t", "u", "w"});
    // Both ONs are inner: they sit with the WHERE's conjuncts, in order, above the tables.
    CHECK(get<logical::Filter>(b).conjuncts.size() == 2);
    CHECK_FALSE(top.left_outer);
    // Pushdown puts each on the join that has both its tables.
    logical::rewrite(root(b));
    const auto& pushed = get<logical::Join>(b);
    CHECK(pushed.predicate.size() == 1);
    CHECK(std::get<logical::Join>(pushed.left->op).predicate.size() == 1);
  }
  {
    // A comma list is the same tree, its conditions the WHERE's.
    auto b = bind_sql(cat, "SELECT t.id FROM t, u, w WHERE t.qty = u.qty AND u.qty < w.qty");
    CHECK(tables_of(root(b)) == std::vector<std::string>{"t", "u", "w"});
    logical::rewrite(root(b));
    const auto& top = get<logical::Join>(b);
    REQUIRE(top.predicate.size() == 1);
    CHECK(top.predicate[0].op == ast::Compare::Op::Lt);
  }
  {
    // An outer join keeps its ON; the NULL-extended table's columns are NULL-able above it, and in the ON
    // of every later join, whatever the catalog declares: u.k is a primary key.
    auto b = bind_sql(cat, "SELECT u.k, w.j FROM t LEFT JOIN u ON u.k = t.qty LEFT JOIN w ON u.k = w.j WHERE u.k > $1");
    const auto& top = get<logical::Join>(b);
    CHECK(top.left_outer);
    REQUIRE(top.predicate.size() == 1);
    CHECK(top.predicate[0].args[0].col.table == cat.table({kPublicSchema, "u"})->id);
    CHECK(top.predicate[0].args[0].col.nullable);  // read from a row the first join may have NULL-extended
    CHECK_FALSE(top.predicate[0].args[1].col.nullable);  // w's own row
    const auto& first = std::get<logical::Join>(top.left->op);
    CHECK(first.left_outer);
    CHECK_FALSE(first.predicate[0].args[0].col.nullable);  // u in its own ON reads real rows
    const auto& selected = get<logical::Project>(b).exprs;
    CHECK(selected[0].expr.col.nullable);
    CHECK(selected[1].expr.col.nullable);
    CHECK(get<logical::Filter>(b).conjuncts[0].args[0].col.nullable);
  }
  {
    // A RIGHT JOIN first swaps its two tables: the preserved one is the left of the first join, the other is
    // NULL-able in the ON of the join after it.
    auto b = bind_sql(cat, "SELECT t.id FROM t RIGHT JOIN u ON t.qty = u.qty JOIN w ON t.id = w.j");
    CHECK(tables_of(root(b)) == std::vector<std::string>{"u", "t", "w"});
    const auto& top = get<logical::Join>(b);
    CHECK_FALSE(top.left_outer);
    const auto& first = std::get<logical::Join>(top.left->op);
    CHECK(first.left_outer);
    REQUIRE(first.predicate.size() == 1);
    REQUIRE(get<logical::Filter>(b).conjuncts.size() == 1);
    const auto& later = get<logical::Filter>(b).conjuncts[0];
    CHECK(later.args[0].col.table == cat.table({kPublicSchema, "t"})->id);
    CHECK(later.args[0].col.nullable);  // t.id, a primary key, read from a row the RIGHT JOIN may NULL-extend
    CHECK_FALSE(later.args[1].col.nullable);
  }
  {
    // An inner join after an outer one is inner; the ON's columns of the outer join's right table are NULL-able.
    auto b = bind_sql(cat, "SELECT t.id FROM t LEFT JOIN u ON t.qty = u.qty JOIN w ON u.k = w.j");
    const auto& top = get<logical::Join>(b);
    CHECK_FALSE(top.left_outer);
    CHECK(std::get<logical::Join>(top.left->op).left_outer);
    CHECK(get<logical::Filter>(b).conjuncts[0].args[0].col.nullable);
  }
  // The names an ON may use, and what is not taken yet.
  CHECK(rejected(cat, "SELECT t.id FROM t JOIN u ON t.qty = w.qty JOIN w ON u.qty = w.qty").first ==
        "missing FROM-clause entry for table \"w\"");
  CHECK(rejected(cat, "SELECT t.id FROM t LEFT JOIN u ON t.qty = u.qty RIGHT JOIN w ON u.qty = w.qty").first ==
        "a RIGHT JOIN of a join that has an outer join is not supported yet");
  CHECK(rejected(cat, "SELECT t.id FROM t RIGHT JOIN u ON t.qty = u.qty RIGHT JOIN w ON u.qty = w.qty").first ==
        "a RIGHT JOIN of a join that has an outer join is not supported yet");
  {
    // A RIGHT JOIN after the first join: its table first, then the join before it as a group, the right side of
    // one left outer join, its own ON inside it.
    auto b = bind_sql(cat, "SELECT t.id FROM t JOIN u ON t.qty = u.qty RIGHT JOIN w ON u.k = w.j WHERE w.j > 1");
    CHECK(tables_of(root(b)) == std::vector<std::string>{"w", "t", "u"});
    const auto& top = get<logical::Join>(b);
    CHECK(top.left_outer);
    CHECK(std::get<logical::Get>(top.left->op).table->name == "w");
    REQUIRE(top.predicate.size() == 1);
    CHECK_FALSE(top.predicate[0].args[0].col.nullable);  // u.k in the ON that matches its rows reads real ones
    const auto& group = std::get<logical::Filter>(top.right->op);
    REQUIRE(group.conjuncts.size() == 1);  // t.qty = u.qty, the group's own
    const auto& inner = std::get<logical::Join>(group.input->op);
    CHECK_FALSE(inner.left_outer);
    CHECK(inner.predicate.empty());
    // The WHERE is above the join, every column of the group NULL-able there.
    const auto& selected = get<logical::Project>(b).exprs;
    CHECK(selected[0].expr.col.nullable);
    const auto& where = std::get<logical::Filter>(std::get<logical::Project>(root(b).op).input->op);
    REQUIRE(where.conjuncts.size() == 1);
    CHECK(where.conjuncts[0].args[0].col.table == cat.table({kPublicSchema, "w"})->id);
  }
  {
    // After a comma, the group is the RIGHT JOIN's own join: the comma's item is joined to its rows by no ON.
    auto b = bind_sql(cat, "SELECT t.id FROM t, u RIGHT JOIN w ON u.qty = w.qty");
    CHECK(tables_of(root(b)) == std::vector<std::string>{"t", "w", "u"});
    const auto& top = get<logical::Join>(b);
    CHECK(top.left_outer);
    CHECK(std::get<logical::Get>(top.right->op).table->name == "u");
    const auto& first = std::get<logical::Join>(top.left->op);
    CHECK_FALSE(first.left_outer);
    CHECK(first.predicate.empty());
  }
  // A later join's USING or NATURAL reads the columns of the join before it: a name two of them have is refused.
  CHECK(rejected(cat, "SELECT t.id FROM t JOIN u ON t.qty = u.qty NATURAL JOIN w").first ==
        "common column name \"qty\" appears more than once in left table");
  CHECK(rejected(cat, "SELECT t.id FROM t JOIN u ON t.qty = u.qty JOIN w USING (j)").first ==
        "column \"j\" specified in USING clause does not exist in left table");
  {
    // The first join's USING column is one column, t's: the second USING compares it with w's.
    auto b = bind_sql(cat, "SELECT qty FROM t JOIN u USING (qty) JOIN w USING (qty)");
    const auto& on = get<logical::Filter>(b).conjuncts;
    REQUIRE(on.size() == 2);
    CHECK(on[0].args[0].col.table == cat.table({kPublicSchema, "t"})->id);
    CHECK(on[0].args[1].col.table == cat.table({kPublicSchema, "u"})->id);
    CHECK(on[1].args[0].col.table == cat.table({kPublicSchema, "t"})->id);
    CHECK(on[1].args[1].col.table == cat.table({kPublicSchema, "w"})->id);
    CHECK(get<logical::Project>(b).exprs[0].expr.col.table == cat.table({kPublicSchema, "t"})->id);
    // SELECT * gives qty once, first, then each table's other columns.
    std::vector<std::string> names;
    for (const auto& o : get<logical::Project>(bind_sql(cat, "SELECT * FROM t JOIN u USING (qty) JOIN w USING (qty)")).exprs) {
      names.push_back(o.name);
    }
    CHECK(names.front() == "qty");
    CHECK(std::ranges::count(names, "qty") == 1);
  }
  CHECK(rejected(cat, "SELECT qty FROM t JOIN u ON t.qty = u.qty JOIN w ON u.qty = w.qty").first ==
        "column reference \"qty\" is ambiguous");
  // The first join's USING column is the left table's, unless a third table has the name.
  CHECK(rejected(cat, "SELECT qty FROM t JOIN u USING (qty) JOIN w ON w.j = t.id").first ==
        "column reference \"qty\" is ambiguous");
  CHECK_NOTHROW(bind_sql(cat, "SELECT * FROM t JOIN u USING (qty) JOIN w ON w.j = t.id"));
  // A USING alias is a FROM name: it may match no other alias or table name of the FROM (42712).
  for (auto [sql, name] : std::vector<std::pair<std::string_view, std::string>>{
           {"SELECT q.qty FROM t JOIN u USING (qty) AS q JOIN w USING (qty) AS q", "q"},
           {"SELECT w.qty FROM t JOIN u USING (qty) AS w JOIN w USING (qty)", "w"},
           {"SELECT t.id FROM t JOIN u USING (qty) JOIN w USING (qty) AS u", "u"},
           {"SELECT k.qty FROM t AS k JOIN u USING (qty) AS k", "k"},
           {"SELECT t.qty FROM t JOIN u USING (qty) AS t", "t"},
           {"SELECT q.qty FROM t JOIN u USING (qty) AS q JOIN w AS q ON true", "q"}}) {
    CAPTURE(sql);
    CHECK(rejected(cat, sql).first == "table name \"" + name + "\" specified more than once");
  }
  // A table the FROM aliases leaves its name free for a USING alias, which then names the join.
  CHECK_NOTHROW(bind_sql(cat, "SELECT t.qty, k.id FROM t AS k JOIN u USING (qty) AS t"));
  CHECK(rejected(cat, "SELECT t.id FROM t AS k JOIN u USING (qty) AS t").first == "column t.id does not exist");
}

TEST_CASE("binder: an ON in parentheses or after a comma reads its own join's tables only", "[sql][binder][join]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE u (k BIGINT PRIMARY KEY, qty BIGINT)").stmt));
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE w (j BIGINT PRIMARY KEY, qty BIGINT, z BIGINT)").stmt));
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE v (z BIGINT PRIMARY KEY)").stmt));
  const auto& cat = f.cat();
  // The inner join's ON and the outer one's are both inner: conjuncts above the three tables.
  CHECK(get<logical::Filter>(bind_sql(cat, "SELECT t.id FROM t JOIN (u JOIN w ON u.qty = w.qty) ON t.id = u.k"))
            .conjuncts.size() == 2);
  // An unqualified name resolves among the ON's own tables: qty is u's and w's, not t's too.
  CHECK(rejected(cat, "SELECT t.id FROM t, u JOIN w ON qty = 1").first == "column reference \"qty\" is ambiguous");
  CHECK_NOTHROW(bind_sql(cat, "SELECT t.id FROM t, u JOIN w ON k = j"));
  // A USING join before a comma: the later ON's tables are its own, whatever the join before merged.
  CHECK_NOTHROW(bind_sql(cat, "SELECT t.id FROM t JOIN u USING (qty), w JOIN v ON w.z = v.z"));
  // A name only a table before the ON's join has is 42703, with PostgreSQL's DETAIL naming that table, or
  // saying there are several.
  auto missing = [&](std::string_view sql) -> std::pair<std::string, std::string> {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      CHECK(e.sqlstate() == "42703");
      return {std::string{e.detail()}, std::string{e.hint()}};
    }
    FAIL("accepted: " << sql);
    return {};
  };
  CHECK(missing("SELECT 1 FROM t AS x, u JOIN v ON name = v.z").first ==
        "There is a column named \"name\" in table \"x\", but it cannot be referenced from this part of the query.");
  CHECK(missing("SELECT 1 FROM w, t, u JOIN v ON j = v.z").first ==
        "There is a column named \"j\" in table \"w\", but it cannot be referenced from this part of the query.");
  auto several = missing("SELECT 1 FROM w, w AS y, u JOIN v ON j = v.z");
  CHECK(several.first == "There are columns named \"j\", but they are in tables that cannot be referenced from this part of the query.");
  CHECK(several.second == "Try using a table-qualified name.");
  // A later table, or a USING alias before the ON's join, is missing (42P01), as in PostgreSQL.
  CHECK(rejected(cat, "SELECT 1 FROM u JOIN v ON w.z = v.z, w").first == "missing FROM-clause entry for table \"w\"");
  CHECK(rejected(cat, "SELECT 1 FROM t JOIN u USING (qty) AS q, w JOIN v ON q.qty = 1").first ==
        "missing FROM-clause entry for table \"q\"");
}

TEST_CASE("binder: a table read twice is two instances of it, named as the query names them", "[sql][binder][join]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  const auto id = cat.table({kPublicSchema, "t"})->id;
  auto b = bind_sql(cat, "SELECT x.id, y.qty FROM t x JOIN t y ON x.id = y.qty");
  const auto& join = get<logical::Join>(b);
  const auto& left = *std::get<logical::Get>(join.left->op).table;
  const auto& right = *std::get<logical::Get>(join.right->op).table;
  // Each use named by an alias is an instance with an id of its own, above every object's, that reads the
  // table's rows (Table::source).
  CHECK(left.name == "x");
  CHECK(right.name == "y");
  CHECK(left.id != right.id);
  CHECK(left.id >= Table::kInstanceBase);
  CHECK(right.id >= Table::kInstanceBase);
  CHECK(left.storage_id() == id);
  CHECK(right.source == id);
  // The columns of each use are its own, so a plan can tell them apart.
  const auto& targets = get<logical::Project>(b).exprs;
  CHECK(targets[0].expr.col.table == left.id);
  CHECK(targets[1].expr.col.table == right.id);
  // The bound statement owns the instances the plan names.
  CHECK(b.instances.size() == 2);
  // A table named by its own name is the catalog's table, when it is the first use.
  auto c = bind_sql(cat, "SELECT t.id FROM t JOIN t y ON t.id = y.id");
  CHECK(std::get<logical::Get>(get<logical::Join>(c).left->op).table->id == id);
  CHECK(std::get<logical::Get>(get<logical::Join>(c).right->op).table->id >= Table::kInstanceBase);
  // A parameter compared with a column of an instance is checked against the table the rows are in.
  auto d = bind_sql(cat, "SELECT x.id FROM t x JOIN t y ON x.id = y.id WHERE y.qty = $1");
  REQUIRE(d.param_table.size() == 1);
  CHECK(d.param_table[0] == id);
  // Three uses are three instances.
  auto e = bind_sql(cat, "SELECT 1 FROM t a, t b, t c WHERE a.id = b.id AND b.id = c.id");
  CHECK(e.instances.size() == 3);
  auto g = bind_sql(cat, "SELECT t.id FROM t JOIN t y ON t.id = y.id");
  CHECK(g.instances.size() == 1);
}

TEST_CASE("binder: CALL's arguments are constants read as the procedure's argument types", "[sql][binder][procedure]") {
  Procedure p{1000, "p", "CREATE PROCEDURE p …",
              {{ProcedureArg::Mode::In, "n", SqlType{SqlKind::Integer}},
               {ProcedureArg::Mode::InOut, "amount", SqlType{SqlKind::Decimal, 0, 6, 2}},
               {ProcedureArg::Mode::InOut, "name", SqlType{SqlKind::Varchar, 4}},
               {ProcedureArg::Mode::Out, "o", SqlType{SqlKind::Integer}},
               {ProcedureArg::Mode::In, "at", SqlType{SqlKind::Timestamp}}}};
  auto args = [&](std::string_view sql) { return bound::call_arguments(*ast::parse_call(sql), p); };
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)args(sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return "bound";
  };
  auto v = args("CALL p(7, 0.0, '', 99, TO_TIMESTAMP('20261103120530','YYYYMMDDHH24MISS')::timestamp without time zone)");
  REQUIRE(v.size() == 5);
  CHECK(v[0] == Datum{std::int64_t{7}});
  CHECK(v[1] == Datum{std::int64_t{0}});  // 0.00 at scale 2
  CHECK(v[2] == Datum{Bytes{}});
  CHECK(std::holds_alternative<std::monostate>(v[3]));  // OUT: ignored
  // 2026-11-03 12:05:30 UTC, in microseconds since 1970.
  CHECK(v[4] == Datum{std::int64_t{1'793'707'530} * 1'000'000});
  CHECK(args("CALL p(1, 12.345, 'abcd', NULL, NULL)")[1] == Datum{std::int64_t{1235}});  // rounded on assignment
  CHECK(state("CALL p(1, 0, '', 0)") == "42883");                                     // a count of no procedure
  CHECK(state("CALL p(1, 0, 'abcde', 0, NULL)") == "22001");                          // past VARCHAR(4)
  CHECK(state("CALL p(40000000000, 0, '', 0, NULL)") == "22003");
  CHECK(state("CALL p(x, 0, '', 0, NULL)") == "42601");                               // not a constant
  CHECK(state("CALL p(1, 0, '', 0, TO_TIMESTAMP('2026', 'YYYY'))") == "42601");       // another format
  CHECK(state("CALL p(1, 0, '', 0, TO_TIMESTAMP('20261332000000', 'YYYYMMDDHH24MISS'))") != "bound");
}

TEST_CASE("binder: $n::T in a value types $n as T; a SELECT without FROM binds any expression", "[sql][binder]") {
  sql_test::Fixture f;
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  {
    // Named by its type, as PostgreSQL names a cast of a parameter; an
    // operator of it is ?column?.
    auto b = bind_sql(cat, "SELECT $1::integer, $1::integer + 1");
    const auto& p = get<logical::Project>(b);
    REQUIRE(p.exprs.size() == 2);
    CHECK(p.exprs[0].name == "int4");
    CHECK(p.exprs[0].expr.type == SqlType{SqlKind::Integer});
    CHECK(p.exprs[1].name == "?column?");
    CHECK(p.exprs[1].expr.type == SqlType{SqlKind::Integer});
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});  // $1, then the literal 1
    REQUIRE(b.params.size() == 1);
    CHECK(b.params[0].type == SqlType{SqlKind::Integer});
    REQUIRE(b.param_sql.size() == 1);
    CHECK(b.param_sql[0] == SqlType{SqlKind::Integer});  // checked as an INTEGER before the plan runs
  }
  {
    auto b = bind_sql(cat, "SELECT $1::text AS a, qty + $2::bigint FROM t");
    const auto& p = get<logical::Project>(b);
    CHECK(p.exprs[0].name == "a");
    CHECK(p.exprs[1].name == "?column?");
    CHECK(b.param_types == std::vector<PhysType>{PhysType::Bytes, PhysType::I64});
  }
  // Cast to the type its other use gives it: one type, taken.
  CHECK(state("SELECT $1::bigint FROM t WHERE id = $1") == "bound");
  // A cast to another type after another use: PostgreSQL types $n by its
  // first use and casts it; the layer refuses (a known difference).
  CHECK(state("SELECT $1::integer FROM t WHERE id = $1") ==
        "42601 parameter $1 is compared with id (bigint) and combined with $1 (integer)");
  CHECK(state("SELECT $1::numeric(5, 2)") == "42601 a cast of a parameter to DECIMAL, VARCHAR(n) or CHAR(n) is not supported yet");
  CHECK(state("SELECT $1::varchar(3)") == "42601 a cast of a parameter to DECIMAL, VARCHAR(n) or CHAR(n) is not supported yet");
  CHECK(state("SELECT $1::char(3)") == "42601 a cast of a parameter to DECIMAL, VARCHAR(n) or CHAR(n) is not supported yet");
  // A SELECT without FROM: a column anywhere in it is of no table.
  CHECK(state("SELECT x + 1") == "42703 column \"x\" does not exist");
  CHECK(state("SELECT 1, abs(x)") == "42703 column \"x\" does not exist");
  CHECK(state("SELECT u.x") == "42P01 missing FROM-clause entry for table \"u\"");
  // A typed constant types a $n beside it; two untyped operands of an
  // operator are PostgreSQL's 42725.
  CHECK(state("SELECT 1 + $1") == "bound");
  CHECK(state("SELECT $1 + $2") == "42725 operator is not unique: unknown + unknown");
  // Assigned, $n::T of the column's type binds as $n, as in a WHERE.
  {
    auto b = bind_sql(cat, "INSERT INTO t VALUES ($1::bigint, NULL, $2::bigint)");
    REQUIRE(b.params.size() == 2);
    CHECK(b.params[0].type == SqlType{SqlKind::BigInt});
    CHECK(b.params[1].type == SqlType{SqlKind::BigInt});
  }
  CHECK(state("UPDATE t SET qty = $1::bigint WHERE id = 1") == "bound");
  CHECK(state("UPDATE t SET qty = qty::bigint WHERE id = 1") == "bound");
  CHECK(state("INSERT INTO t VALUES ($1::integer, NULL, 1)") ==
        "42601 a cast of a parameter to other than its column's type is not supported yet");
  {
    // $n::T of another type than its column's is a T, assigned (#904 S1).
    auto b = bind_sql(cat, "UPDATE t SET qty = $1::integer WHERE id = 1");
    REQUIRE(!b.params.empty());
    CHECK(b.params[0].type == SqlType{SqlKind::Integer});
  }
  // An operator is ?column?, % among them, as PostgreSQL names it.
  CHECK(get<logical::Project>(bind_sql(cat, "SELECT qty % 2 FROM t")).exprs[0].name == "?column?");
  CHECK(get<logical::Project>(bind_sql(cat, "SELECT (SELECT 1) % 2")).exprs[0].name == "?column?");
}

TEST_CASE("binder: a query that aggregates over a grouped view reads it as a derived relation", "[sql][binder][view]") {
  sql_test::Fixture f;
  auto view = [&](std::string_view sql) { f.ddl(std::get<Ddl>(bind_sql(f.cat(), sql).stmt)); };
  view("CREATE VIEW h AS SELECT qty, MIN(id) AS lo, COUNT(*) AS n FROM t GROUP BY qty");
  view("CREATE VIEW k AS SELECT name, COUNT(*) AS c FROM t GROUP BY name");
  view("CREATE VIEW g AS SELECT qty FROM t GROUP BY qty");
  view("CREATE VIEW a AS SELECT qty, AVG(qty) AS m FROM t GROUP BY qty");
  view("CREATE VIEW hh AS SELECT n, SUM(lo) AS s FROM h GROUP BY n");
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
      return "bound";
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
  };
  {
    auto b = bind_sql(cat, "SELECT SUM(lo), n FROM h WHERE lo > 0 GROUP BY n");
    const auto& d = get<logical::Derived>(b);
    REQUIRE(d.table != nullptr);
    CHECK(Table::derived(d.table->id));
    CHECK(d.table->name == "h");
    CHECK(d.table->pk_col_ids.empty());
    REQUIRE(d.table->columns.size() == 3);
    // Each column's NULLs are its output's: the grouped column's, COUNT never, MIN always.
    CHECK(d.table->columns[0].name == "qty");
    CHECK(d.table->columns[0].nullable);
    CHECK(d.table->columns[1].nullable);
    CHECK_FALSE(d.table->columns[2].nullable);
    CHECK(d.table->columns[2].sql == SqlType{SqlKind::BigInt});
    // Its query is the view's: grouped, a plan of its own below the relation.
    REQUIRE(d.plan != nullptr);
    CHECK(find<logical::Aggregate>(*d.plan) != nullptr);
    // The outer query groups and filters the relation's columns.
    const auto& agg = get<logical::Aggregate>(b);
    REQUIRE(agg.group_by.size() == 1);
    CHECK(agg.group_by[0].col.table == d.table->id);
    // The bound statement owns the relation's table.
    CHECK(std::ranges::any_of(b.instances, [&](const auto& t) { return t.get() == d.table; }));
  }
  // A grouped view merged as before: no derived relation.
  CHECK(find<logical::Derived>(root(bind_sql(cat, "SELECT qty FROM g GROUP BY qty"))) == nullptr);
  CHECK(find<logical::Derived>(root(bind_sql(cat, "SELECT MAX(qty) FROM g"))) == nullptr);
  CHECK(find<logical::Derived>(root(bind_sql(cat, "SELECT qty, n FROM h"))) == nullptr);
  // Aggregated otherwise, read as a relation; an aggregate's column grouped by too.
  CHECK(find<logical::Derived>(root(bind_sql(cat, "SELECT COUNT(*) FROM g"))) != nullptr);
  CHECK(find<logical::Derived>(root(bind_sql(cat, "SELECT n, COUNT(*) FROM h GROUP BY n"))) != nullptr);
  // No primary key: an ungrouped column is 42803, as in PostgreSQL.
  CHECK(state("SELECT qty, lo FROM h GROUP BY qty") ==
        "42803 column \"h.lo\" must appear in the GROUP BY clause or be used in an aggregate function");
  CHECK(state("SELECT name, SUM(c) FROM k GROUP BY name") ==
        "42601 grouping a grouped view's rows by a bytea column is not supported yet");
  CHECK(state("SELECT SUM(m) FROM a").starts_with("42601 aggregating over a grouped view's AVG of an integer"));
  CHECK(state("SELECT SUM(lo) FROM h WHERE nope = 1") == "42703 column \"nope\" does not exist");
  // One level: a view that reads a grouped view as a relation is read, and not aggregated over.
  CHECK(state("SELECT n, s FROM hh") == "bound");
  CHECK(state("SELECT SUM(s) FROM hh") ==
        "42601 aggregating over a view that aggregates over a grouped view is not supported yet");
}

TEST_CASE("binder: a grouped view joined with a table is a derived relation, its query's table read again by an instance",
          "[sql][binder][view]") {
  sql_test::Fixture f;
  auto view = [&](std::string_view sql) { f.ddl(std::get<Ddl>(bind_sql(f.cat(), sql).stmt)); };
  f.ddl(CreateTableDdl{{kPublicSchema, "u"}, {sql_test::Fixture::col("id", PhysType::I64, false), sql_test::Fixture::col("qty", PhysType::I64, true)}, {"id"}});
  view("CREATE VIEW h AS SELECT qty, COUNT(*) AS n FROM t GROUP BY qty");
  view("CREATE VIEW p AS SELECT id, qty FROM u");
  view("CREATE VIEW hh AS SELECT n, COUNT(*) AS m FROM h GROUP BY n");
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
      return "bound";
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
  };
  {
    // USING: the view's rows drive, its column is the join's.
    auto b = bind_sql(cat, "SELECT qty, n, id FROM h JOIN u USING (qty)");
    const auto& j = get<logical::Join>(b);
    const auto* d = std::get_if<logical::Derived>(&j.left->op);
    REQUIRE(d != nullptr);
    CHECK(Table::derived(d->table->id));
    CHECK(find<logical::Get>(*j.right)->table->name == "u");
    const auto& p = get<logical::Project>(b);
    CHECK(p.exprs[0].expr.col.table == d->table->id);
  }
  {
    // A table the view's query reads is read by an instance of its own.
    auto b = bind_sql(cat, "SELECT h.n, t.name FROM t JOIN h ON t.qty = h.qty");
    const auto& j = get<logical::Join>(b);
    const auto* g = std::get_if<logical::Get>(&j.left->op);
    REQUIRE(g != nullptr);
    CHECK(g->table->id >= Table::kInstanceBase);
    CHECK(g->table->source == cat.table({kPublicSchema, "t"})->id);
    CHECK(std::get_if<logical::Derived>(&j.right->op) != nullptr);
  }
  CHECK(state("SELECT * FROM h NATURAL JOIN u") == "bound");
  CHECK(state("SELECT * FROM u RIGHT JOIN h ON u.qty = h.qty") == "bound");
  CHECK(state("SELECT * FROM u LEFT JOIN h ON u.qty = h.qty") ==
        "42601 a grouped view on the NULL-extended side of an outer join is not supported yet");
  CHECK(state("SELECT COUNT(*) FROM h JOIN u USING (qty)") ==
        "42601 grouping, aggregating or DISTINCT over a join of a grouped view is not supported yet");
  CHECK(state("SELECT * FROM h JOIN p USING (qty)") == "42601 a join of a grouped view with another view is not supported yet");
  CHECK(state("SELECT * FROM hh JOIN u ON u.qty = hh.n") ==
        "42601 a join of a view that aggregates over a grouped view is not supported yet");
  CHECK(state("SELECT * FROM h JOIN u USING (nope)") == "42703 column \"nope\" specified in USING clause does not exist in left table");
}

TEST_CASE("binder: an expression in a WHERE or ON is the select list's, a BOOLEAN, of no aggregate and no random()", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE ex (k INTEGER PRIMARY KEY, a INTEGER, s TEXT, d DECIMAL(10,2), c BIGINT, "
                                        "flag BOOLEAN)").stmt));
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE ey (x INTEGER PRIMARY KEY, y INTEGER)").stmt));
  const auto& cat = f.cat();
  using K = logical::Expr::Kind;
  auto where = [&](std::string_view w) {
    auto b = bind_sql(cat, "SELECT k FROM ex WHERE " + std::string{w});
    return get<logical::Filter>(b).conjuncts;
  };
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what();
    }
    return "bound";
  };
  auto types = [&](std::string_view sql) {
    std::vector<SqlType> out;
    for (const auto& p : bind_sql(cat, sql).params) out.push_back(p.type);
    return out;
  };
  const SqlType text{SqlKind::Text};
  const SqlType integer{SqlKind::Integer};
  const SqlType boolean{SqlKind::Boolean};

  // Each form binds to one conjunct of its tree.
  CHECK(where("lower(s) = $1").at(0).args[0].kind == K::TextFn);
  CHECK(where("a + 1 > $1").at(0).args[0].kind == K::Add);
  CHECK(where("lower(s) LIKE 'a%'").at(0).args[0].kind == K::TextFn);
  CHECK(where("s LIKE s").at(0).args[1].kind == K::Column);
  CHECK(where("a + 1 IS NULL").at(0).args[0].kind == K::Add);
  CHECK(where("a + 0 IN (1, 5)").at(0).kind == K::Or);
  CHECK(where("a + 1 BETWEEN 1 AND 2").size() == 2);  // two conjuncts, as a column's
  CHECK(where("flag").at(0).kind == K::Column);
  CHECK(where("NOT flag").at(0).kind == K::Not);
  CHECK(where("CASE WHEN a > 0 THEN flag ELSE false END").at(0).kind == K::Case);
  CHECK(where("(a > 0) = flag").at(0).args[0].kind == K::Compare);
  // $n typed by an expression beside it, as PostgreSQL types it.
  CHECK(types("SELECT k FROM ex WHERE lower(s) = $1") == std::vector{text});
  CHECK(types("SELECT k FROM ex WHERE a + 1 IN ($1, $2)") == std::vector{integer, integer});
  CHECK(types("SELECT k FROM ex WHERE c > $1 - 100") == std::vector{integer});
  CHECK(types("SELECT k FROM ex WHERE $1 = $2") == std::vector{text, text});
  CHECK(types("SELECT k FROM ex WHERE $1") == std::vector{boolean});
  CHECK(types("SELECT k FROM ex WHERE lower(s) LIKE $1") == std::vector{text});
  CHECK(types("SELECT lower(s) = $1 FROM ex") == std::vector{text});
  // $n ± k under OR is an expression of a hidden parameter, never the key's $n ± k.
  {
    const auto w = where("a = $1 - 1 OR k = 2");
    const auto& sub = w.at(0).args[0].args[1];
    CHECK(sub.kind == K::Sub);
    CHECK(sub.args[1].kind == K::Param);
  }
  // $n::T beside a column of another type: the parameter T, compared as
  // the select list compares it, at the DECIMAL's scale.
  {
    auto b = bind_sql(cat, "SELECT k FROM ex WHERE d > $1::int");
    CHECK(b.params.at(0).type == integer);
    const auto& c = get<logical::Filter>(b).conjuncts.at(0);
    CHECK(c.args[1].type == integer);
  }
  // An ON of expressions binds as the WHERE's.
  CHECK(state("SELECT k FROM ex JOIN ey ON ex.a + 1 = ey.x") == "bound");
  CHECK(state("SELECT k FROM ex LEFT JOIN ey ON lower(ex.s) = 'x' AND ey.y * 2 > ex.a") == "bound");
  // A column compared as today keeps its shape: a key may take it.
  CHECK(where("a = $1").at(0).args[1].kind == K::Param);
  CHECK(where("a = $1 + 1").at(0).args[1].args[1].kind == K::Const);

  // The refusals, PostgreSQL's SQLSTATEs and messages, each naming its clause.
  CHECK(state("SELECT k FROM ex WHERE a + 1") == "42804 argument of WHERE must be type boolean, not type integer");
  CHECK(state("SELECT k FROM ex WHERE a + 1 AND flag") == "42804 argument of AND must be type boolean, not type integer");
  CHECK(state("SELECT k FROM ex WHERE flag OR s") == "42804 argument of OR must be type boolean, not type text");
  CHECK(state("SELECT k FROM ex WHERE NOT a") == "42804 argument of NOT must be type boolean, not type integer");
  CHECK(state("SELECT k FROM ex JOIN ey ON ex.a") == "42804 argument of JOIN/ON must be type boolean, not type integer");
  CHECK(state("SELECT k FROM ex WHERE (SELECT 1)") == "42804 argument of WHERE must be type boolean, not type integer");
  // At the expression's leftmost token, as PostgreSQL's exprLocation.
  auto at = [&](std::string_view sql) { return rejected(cat, sql).second; };
  CHECK(at("SELECT k FROM ex WHERE a + 1") == 23);
  CHECK(at("SELECT k FROM ex WHERE (a + 1)") == 24);
  CHECK(at("SELECT k FROM ex WHERE NOT (a + 1)") == 28);
  CHECK(at("SELECT k FROM ex WHERE flag AND a * 2") == 32);
  CHECK(at("SELECT k FROM ex WHERE s || 'x'") == 23);
  CHECK(state("SELECT k FROM ex WHERE a + count(*) > 1") == "42803 aggregate functions are not allowed in WHERE");
  CHECK(state("SELECT k FROM ex JOIN ey ON ex.a + count(*) > 1") == "42803 aggregate functions are not allowed in JOIN conditions");
  CHECK(state("UPDATE ex SET a = 1 WHERE count(*) > 1") == "42803 aggregate functions are not allowed in WHERE");
  CHECK(state("SELECT k FROM ex WHERE random(1, 2) = 1") == "42601 random() in a WHERE, ON or HAVING is not supported yet");
  CHECK(state("DELETE FROM ex WHERE a = random(1, 2)") == "42601 random() in a WHERE, ON or HAVING is not supported yet");
  CHECK(state("SELECT k FROM ex WHERE s + 1 > 0") == "42883 operator does not exist: text + integer");
  CHECK(state("SELECT k FROM ex WHERE lower(a) = 'x'") == "42883 function lower(integer) does not exist");
  CHECK(state("SELECT k FROM ex WHERE a + 1 LIKE 'x'") == "42883 operator does not exist: integer ~~ unknown");
  CHECK(state("SELECT k FROM ex WHERE s LIKE a") == "42883 operator does not exist: text ~~ integer");
  CHECK(state("SELECT k FROM ex WHERE a + 1 = '1.5'") == "22P02 invalid input syntax for type integer: \"1.5\"");
  CHECK(state("SELECT k FROM ex WHERE $1 + $2 = a") == "42725 operator is not unique: unknown + unknown");
  CHECK(state("SELECT k FROM ex WHERE $1 IS NULL") == "42P18 could not determine data type of parameter $1");
  CHECK(state("SELECT k FROM ex WHERE a + 1 IN (SELECT x FROM ey)") ==
        "42601 IN, ANY or ALL of a subquery of other than a column is not supported yet");
  CHECK(state("SELECT k FROM ex WHERE d > $1::numeric(5,1)") ==
        "42601 a cast of a parameter to DECIMAL, VARCHAR(n) or CHAR(n) is not supported yet");
  CHECK(rejected(cat, "SELECT k FROM ex WHERE greatest(a, 1) > 1") == std::pair<std::string, std::int32_t>{"expression not supported", 23});
}

// #904 stage C0. Expected values are PostgreSQL 17's (int2in, int4in,
// int8in, float4in, float8in, numeric_in into a NUMERIC(p, s)).
TEST_CASE("binder: text is read as a value by PostgreSQL 17's input functions, for constants and parameters alike",
          "[sql][binder][text]") {
  using bytecask::sql::from_text;
  using bytecask::sql::TextError;
  const SqlType i2{SqlKind::SmallInt};
  const SqlType i4{SqlKind::Integer};
  const SqlType i8{SqlKind::BigInt};
  auto value = [](std::string_view text, SqlType t) { return std::get<std::int64_t>(from_text(text, t, false)); };
  // What from_text raises: SQLSTATE, message and DETAIL.
  auto raised = [](std::string_view text, SqlType t) -> std::string {
    try {
      (void)from_text(text, t, true);
    } catch (const TextError& e) {
      auto out = std::string{e.sqlstate()} + " " + e.what();
      if (!e.detail().empty()) out += " | " + std::string{e.detail()};
      return out;
    }
    return "read";
  };

  SECTION("integers: spaces, a sign, 0x, 0o, 0b and _ between digits") {
    CHECK(value(" 12 ", i4) == 12);
    CHECK(value("\t12\n", i4) == 12);
    CHECK(value("00012", i4) == 12);
    CHECK(value("0x1F", i4) == 31);
    CHECK(value("0X1f", i4) == 31);
    CHECK(value("-0x1F", i4) == -31);
    CHECK(value("+0x1F", i4) == 31);
    CHECK(value("0o17", i4) == 15);
    CHECK(value("0b101", i4) == 5);
    CHECK(value("1_000", i4) == 1000);
    CHECK(value("1_000_000", i8) == 1'000'000);
    CHECK(value("0x_1F", i4) == 31);
    CHECK(value("0x1_F", i4) == 31);
    CHECK(value("-0", i4) == 0);
    CHECK(value("-2147483648", i4) == std::numeric_limits<std::int32_t>::min());
    CHECK(value("-0x80000000", i4) == std::numeric_limits<std::int32_t>::min());
    CHECK(value("0b111111111111111", i2) == 32767);
    CHECK(value("-0b1000000000000000", i2) == std::numeric_limits<std::int16_t>::min());
    CHECK(value("0x7FFFFFFFFFFFFFFF", i8) == std::numeric_limits<std::int64_t>::max());
    CHECK(value("-0x8000000000000000", i8) == std::numeric_limits<std::int64_t>::min());
    CHECK(value("-9223372036854775808", i8) == std::numeric_limits<std::int64_t>::min());
  }

  SECTION("integers: 22P02 for what int4in does not read") {
    for (const auto* bad : {"", "1.5", "1e3", "1 2", "- 1", "--1", "1__0", "_1", "1_", "0x", "0b", "0b_", "0b1_", "0o8",
                            "00x1", "0x 1", "0x1__F", "2147483649x"}) {
      CAPTURE(bad);
      CHECK(raised(bad, i4) == std::string{"22P02 invalid input syntax for type integer: \""} + bad + "\"");
    }
    CHECK(raised("0b", i2) == "22P02 invalid input syntax for type smallint: \"0b\"");
  }

  SECTION("integers: 22003 names the text, found before trailing junk") {
    CHECK(raised("3000000000", i4) == "22003 value \"3000000000\" is out of range for type integer");
    CHECK(raised(" 3000000000 ", i4) == "22003 value \" 3000000000 \" is out of range for type integer");
    CHECK(raised("3000000000x", i4) == "22003 value \"3000000000x\" is out of range for type integer");
    CHECK(raised("21474836490x", i4) == "22003 value \"21474836490x\" is out of range for type integer");
    CHECK(raised("0x80000000", i4) == "22003 value \"0x80000000\" is out of range for type integer");
    CHECK(raised("70000", i2) == "22003 value \"70000\" is out of range for type smallint");
    CHECK(raised("0b1111111111111111", i2) == "22003 value \"0b1111111111111111\" is out of range for type smallint");
    CHECK(raised("0x8000", i2) == "22003 value \"0x8000\" is out of range for type smallint");
    CHECK(raised("0x8000000000000000", i8) == "22003 value \"0x8000000000000000\" is out of range for type bigint");
    CHECK(raised("99999999999999999999", i8) ==
          "22003 value \"99999999999999999999\" is out of range for type bigint");
  }

  SECTION("a REAL is read as float4in reads it, rounded once") {
    const SqlType real{SqlKind::Real};
    // Bit for bit: 0.1 rounded to a float once, not the double 0.1.
    CHECK(std::bit_cast<std::uint64_t>(std::get<double>(from_text("0.1", real, false))) ==
          std::bit_cast<std::uint64_t>(static_cast<double>(0.1f)));
    CHECK(std::get<double>(from_text("1e-45", real, false)) > 0);  // a subnormal float is in range
    CHECK(std::get<double>(from_text("8e-46", real, false)) > 0);
    for (const auto* big : {"1e39", "3.4028236e38", "3.40282357e38", "1e-50", "1e-46", "7e-46"}) {
      CAPTURE(big);
      CHECK(raised(big, real) == std::string{"22003 \""} + big + "\" is out of range for type real");
    }
    CHECK(raised("x", real) == "22P02 invalid input syntax for type real: \"x\"");
    CHECK(raised("", real) == "22P02 invalid input syntax for type real: \"\"");
    CHECK(raised("1.5 x", real) == "22P02 invalid input syntax for type real: \"1.5 x\"");
    CHECK(raised("x", SqlType{SqlKind::Double}) == "22P02 invalid input syntax for type double precision: \"x\"");
    CHECK(raised("1e400", SqlType{SqlKind::Double}) == "22003 \"1e400\" is out of range for type double precision");
    // As float4in and float8in: out of range names only the number read,
    // sign kept and spaces dropped, and wins over text after it.
    CHECK(raised(" 1e39 ", real) == "22003 \"1e39\" is out of range for type real");
    CHECK(raised(" +1e39 ", real) == "22003 \"+1e39\" is out of range for type real");
    CHECK(raised("-1e39x", real) == "22003 \"-1e39\" is out of range for type real");
    CHECK(raised("1e-50x", real) == "22003 \"1e-50\" is out of range for type real");
    CHECK(raised("1e3x", real) == "22P02 invalid input syntax for type real: \"1e3x\"");
    CHECK(raised(" 1e999 ", SqlType{SqlKind::Double}) == "22003 \"1e999\" is out of range for type double precision");
    CHECK(raised("+1e999 x", SqlType{SqlKind::Double}) ==
          "22003 \"+1e999\" is out of range for type double precision");
  }

  SECTION("a DECIMAL(p, s) holds no infinity and no NaN: 22003 with the DETAIL") {
    const SqlType d30{SqlKind::Decimal, 0, 3, 0};
    const SqlType d52{SqlKind::Decimal, 0, 5, 2};
    auto inf30 = std::string{"22003 numeric field overflow | A field with precision 3, scale 0 cannot hold an infinite value."};
    for (const auto* inf : {"Infinity", "-Infinity", "+Infinity", "inf", " infinity ", "-INF"}) {
      CAPTURE(inf);
      CHECK(raised(inf, d30) == inf30);
    }
    CHECK(raised("-inf", d52) ==
          "22003 numeric field overflow | A field with precision 5, scale 2 cannot hold an infinite value.");
    CHECK(raised("NaN", d30) == "22003 numeric field overflow | A field with precision 3, scale 0 cannot hold NaN.");
    CHECK(raised(" nan ", d30) == "22003 numeric field overflow | A field with precision 3, scale 0 cannot hold NaN.");
    for (const auto* bad : {"-NaN", "+nan", "infinit", "Infinityx"}) {
      CAPTURE(bad);
      CHECK(raised(bad, d30) == std::string{"22P02 invalid input syntax for type numeric: \""} + bad + "\"");
    }
  }

  SECTION("a constant reads alike, its error the analysis's") {
    sql_test::Fixture f;
    f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE c0 (k INTEGER PRIMARY KEY, s SMALLINT, d NUMERIC(3,0))").stmt));
    auto state = [&](std::string_view sql) -> std::string {
      try {
        (void)bind_sql(f.cat(), sql);
      } catch (const ast::SqlError& e) {
        auto out = std::string{e.sqlstate()} + " " + e.what();
        if (!e.detail().empty()) out += " | " + std::string{e.detail()};
        return out;
      }
      return "bound";
    };
    CHECK(state("SELECT '0x1F'::int + 1") == "bound");
    CHECK(state("INSERT INTO c0 VALUES ('0b11', '1_000', 1)") == "bound");
    CHECK(state("SELECT k FROM c0 WHERE k = '0o17'") == "bound");
    CHECK(state("SELECT '3000000000'::int") == "22003 value \"3000000000\" is out of range for type integer");
    CHECK(state("INSERT INTO c0 VALUES (1, '40000', 1)") == "22003 value \"40000\" is out of range for type smallint");
    CHECK(state("INSERT INTO c0 VALUES (1, 1, 'Infinity')") ==
          "22003 numeric field overflow | A field with precision 3, scale 0 cannot hold an infinite value.");
    CHECK(state("SELECT '1e39'::real") == "22003 \"1e39\" is out of range for type real");
  }
}

TEST_CASE("binder: ARRAY[ARRAY[...]] as a jsonb path is refused as a multidimensional ARRAY", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE ja (k INTEGER PRIMARY KEY, j JSONB)").stmt));
  for (const auto* sql : {"SELECT j #> ARRAY[ARRAY['a'], ARRAY['b']] FROM ja", "SELECT j #>> ARRAY[['a'], ['b']] FROM ja",
                          "SELECT '{\"a\": 1}'::jsonb #> ARRAY[ARRAY['a']]", "SELECT j #> ARRAY[k::text, ARRAY['b']] FROM ja"}) {
    CAPTURE(sql);
    try {
      (void)bind_sql(f.cat(), sql);
      FAIL("bound");
    } catch (const ast::SqlError& e) {
      CHECK(std::string{e.sqlstate()} == "42601");
      CHECK(std::string{e.what()} == "a multidimensional ARRAY is not supported yet");
    }
  }
}

TEST_CASE("binder: a cast of an expression binds by the cast table", "[sql][binder][cast]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(), "CREATE TABLE c (k INT PRIMARY KEY, s TEXT, v VARCHAR(5), n NAME, ch CHAR(3), "
                                        "b BYTEA, j JSONB, d NUMERIC(5,2), r REAL, x DOUBLE PRECISION, dt DATE, "
                                        "ts TIMESTAMP, tm TIME, bo BOOLEAN, sm SMALLINT, nu NUMERIC)").stmt));
  const auto& cat = f.cat();
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what() + " @" + std::to_string(e.pos());
    }
    return "bound";
  };
  auto first = [&](std::string_view sql) -> logical::Output {
    auto b = bind_sql(cat, sql);
    return get<logical::Project>(b).exprs.at(0);
  };

  SECTION("the classes of cast_kind, as pg_cast and CoerceViaIO") {
    using K = SqlKind;
    auto kind = [](K from, K to) { return cast_kind(SqlType{from}, SqlType{to}); };
    CHECK(kind(K::Text, K::Text) == CastKind::Relabel);
    CHECK(kind(K::Name, K::Text) == CastKind::Relabel);
    CHECK(kind(K::Varchar, K::Text) == CastKind::Relabel);
    CHECK(kind(K::Text, K::Name) == CastKind::Function);  // cut to 63 bytes
    CHECK(kind(K::Char, K::Text) == CastKind::Function);  // rtrim1
    CHECK(kind(K::Boolean, K::Text) == CastKind::Function);  // booltext
    CHECK(kind(K::Boolean, K::Name) == CastKind::InOut);     // boolout
    CHECK(kind(K::BigInt, K::Integer) == CastKind::Function);
    CHECK(kind(K::Integer, K::Boolean) == CastKind::Function);
    CHECK(kind(K::SmallInt, K::Boolean) == CastKind::None);
    CHECK(kind(K::Boolean, K::BigInt) == CastKind::None);
    CHECK(kind(K::Decimal, K::Boolean) == CastKind::None);
    CHECK(kind(K::Date, K::Timestamp) == CastKind::Function);
    CHECK(kind(K::Time, K::Timestamp) == CastKind::None);
    CHECK(kind(K::Timestamp, K::TimeTz) == CastKind::None);
    CHECK(kind(K::TimestampTz, K::TimeTz) == CastKind::Function);
    CHECK(kind(K::Date, K::Integer) == CastKind::None);
    CHECK(kind(K::Integer, K::Text) == CastKind::InOut);
    CHECK(kind(K::Text, K::Date) == CastKind::InOut);
    CHECK(kind(K::Text, K::Jsonb) == CastKind::InOut);
    CHECK(kind(K::Jsonb, K::Integer) == CastKind::Function);
    CHECK(kind(K::Jsonb, K::Bytea) == CastKind::None);
    CHECK(kind(K::Integer, K::Jsonb) == CastKind::None);
    CHECK(cast_kind(SqlType{K::Decimal, 0, 5, 2}, SqlType{K::Decimal, 0, 4, 1}) == CastKind::Function);
    CHECK(cast_refusal(SqlType{K::Double}, SqlType{K::Text}) == CastRefusal::FloatText);
    CHECK(cast_refusal(SqlType{K::Numeric}, SqlType{K::Integer}) == CastRefusal::Numeric);
    CHECK(cast_refusal(SqlType{K::Numeric}, SqlType{K::Text}) == CastRefusal::None);
    CHECK(cast_refusal(SqlType{K::Jsonb}, SqlType{K::Numeric}) == CastRefusal::None);
    CHECK(cast_refusal(SqlType{K::Char, 3}, SqlType{K::Integer}) == CastRefusal::CharInput);
  }

  SECTION("a cast of a column is a Cast node of the target type; a relabel is the operand retyped") {
    auto o = first("SELECT s::int FROM c");
    CHECK(o.expr.kind == logical::Expr::Kind::Cast);
    CHECK(o.expr.type == SqlType{SqlKind::Integer});
    REQUIRE(o.expr.args.size() == 1);
    CHECK(o.expr.args[0].type == SqlType{SqlKind::Text});
    auto relabel = first("SELECT n::text FROM c");
    CHECK(relabel.expr.kind == logical::Expr::Kind::Column);
    CHECK(relabel.expr.type == SqlType{SqlKind::Text});
    auto d = first("SELECT k::numeric(4,1) FROM c");
    CHECK(d.expr.type == SqlType{SqlKind::Decimal, 0, 4, 1});
    // jsonb's casts out are the same node.
    CHECK(first("SELECT (j->'a')::int FROM c").expr.kind == logical::Expr::Kind::Cast);
    CHECK(first("SELECT (j->'a')::numeric FROM c").expr.type == SqlType{SqlKind::Numeric});
  }

  SECTION("$1::text::int types $1 as text and casts it per row") {
    auto b = bind_sql(cat, "SELECT $1::text::int");
    const auto& o = get<logical::Project>(b).exprs.at(0);
    CHECK(o.name == "int4");
    CHECK(o.expr.kind == logical::Expr::Kind::Cast);
    REQUIRE(b.params.size() == 1);
    CHECK(b.params[0].type == SqlType{SqlKind::Text});
    // In a WHERE it is an operand of a filter, not a key's bound.
    CHECK(state("SELECT k FROM c WHERE k = $1::text::int") == "bound");
  }

  SECTION("a NULL constant operand folds to a NULL of the target type") {
    auto o = first("SELECT (s || NULL)::int FROM c");
    CHECK(o.expr.kind == logical::Expr::Kind::Param);
    CHECK(o.expr.type == SqlType{SqlKind::Integer});
  }

  SECTION("no cast is 42846 at the cast; what the layer does not take yet is refused") {
    CHECK(state("SELECT dt::int FROM c") == "42846 cannot cast type date to integer @9");
    CHECK(state("SELECT sm::bool FROM c") == "42846 cannot cast type smallint to boolean @9");
    CHECK(state("SELECT tm::timestamp FROM c") ==
          "42846 cannot cast type time without time zone to timestamp without time zone @9");
    CHECK(state("SELECT k::jsonb FROM c") == "42846 cannot cast type integer to jsonb @8");
    CHECK(state("SELECT x::text FROM c") == "42601 a cast of a floating-point value to text is not supported yet @8");
    CHECK(state("SELECT r::name FROM c") == "42601 a cast of a floating-point value to text is not supported yet @8");
    CHECK(state("SELECT s::varchar(2) FROM c") == "42601 a cast to VARCHAR(n) is not supported yet @8");
    CHECK(state("SELECT s::char(2) FROM c") == "42601 a cast to CHAR(n) is not supported yet @8");
    CHECK(state("SELECT ch::int FROM c").starts_with("0A000 a cast of a CHAR(n) value to integer"));
    CHECK(state("SELECT nu::int FROM c").starts_with("0A000 a cast of a NUMERIC to integer"));
    CHECK(state("SELECT s::numeric FROM c").starts_with("0A000 a cast to NUMERIC"));
    CHECK(state("SELECT (j->'a')::numeric(5,2) FROM c").starts_with("0A000 a cast of jsonb to numeric(5,2)"));
    CHECK(state("SELECT nu::text FROM c") == "bound");
  }

  SECTION("a cast is named as PostgreSQL 17's FigureColname") {
    auto bound = bind_sql(cat, "SELECT s::int, upper(s)::text, coalesce(k, 0)::text, (k + 1)::text, (k * 2)::bigint, "
                      "(j->>'k')::int, (CASE WHEN k > 0 THEN 1 ELSE 2 END)::int, (CASE WHEN k > 0 THEN 1 ELSE k END)::text, "
                      "(s || 'x')::text, (SELECT 1 AS y)::text, (SELECT 1)::text, nullif(k, 1)::text, s::text::int, "
                      "(j->0)::int::text, coalesce(1, 2)::text, '1'::int FROM c");
    std::vector<std::string> got;
    for (const auto& o : get<logical::Project>(bound).exprs) got.push_back(o.name);
    CHECK(got == std::vector<std::string>{"s", "upper", "coalesce", "text", "int8", "int4", "int4", "k", "text", "y",
                                          "?column?", "nullif", "s", "text", "coalesce", "int4"});
  }
}

TEST_CASE("binder: a SET value is any value expression, assigned by PostgreSQL 17's assignment casts", "[sql][binder]") {
  sql_test::Fixture f;
  f.ddl(std::get<Ddl>(bind_sql(f.cat(),
                               "CREATE TABLE s (id INT PRIMARY KEY, a SMALLINT, n INT, d NUMERIC(5,2), f DOUBLE PRECISION, "
                               "t TEXT, v VARCHAR(3), ch CHAR(3), nm NAME, bo BOOLEAN, dt DATE, tm TIME, ts TIMESTAMP, "
                               "j JSONB, num NUMERIC)")
                        .stmt));
  const auto& cat = f.cat();
  // SQLSTATE, message, position and HINT, or "bound".
  auto state = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      auto out = std::string{e.sqlstate()} + " " + e.what() + " @" + std::to_string(e.pos());
      if (!e.hint().empty()) out += " | " + std::string{e.hint()};
      return out;
    }
    return "bound";
  };
  constexpr std::string_view kHint = " | You will need to rewrite or cast the expression.";
  auto set_of = [&](std::string_view sql) { return get<logical::Update>(bind_sql(cat, sql)).set; };

  // Stored as it is: its own type, an integer into another integer, a
  // string into another string type (the row builder checks each).
  for (auto sql : {"UPDATE s SET n = n * 2", "UPDATE s SET a = n + 100000", "UPDATE s SET v = t || 'x'",
                   "UPDATE s SET ch = lower(t)", "UPDATE s SET nm = t", "UPDATE s SET num = num"}) {
    INFO(sql);
    auto set = set_of(sql);
    REQUIRE(set.size() == 1);
    CHECK(set[0].second.kind != logical::Expr::Kind::Cast);
  }
  // A cast to the column's type where the value changes.
  for (auto sql : {"UPDATE s SET t = n", "UPDATE s SET t = bo", "UPDATE s SET t = j", "UPDATE s SET t = ch",
                   "UPDATE s SET v = d", "UPDATE s SET n = f", "UPDATE s SET n = d", "UPDATE s SET d = n * 1.005",
                   "UPDATE s SET f = n", "UPDATE s SET dt = ts", "UPDATE s SET ts = dt", "UPDATE s SET nm = bo",
                   "UPDATE s SET n = (j->>'n')::bigint"}) {
    INFO(sql);
    auto set = set_of(sql);
    REQUIRE(set.size() == 1);
    CHECK(set[0].second.kind == logical::Expr::Kind::Cast);
  }
  CHECK(set_of("UPDATE s SET nm = bo")[0].second.type == SqlType{SqlKind::Name});  // boolout's t
  CHECK(set_of("UPDATE s SET v = d")[0].second.type == SqlType{SqlKind::Text});
  CHECK(set_of("UPDATE s SET d = n")[0].second.type == SqlType{SqlKind::Decimal, 0, 5, 2});
  // No assignment cast: PostgreSQL's 42804 with its HINT, at the value.
  //                0         1         2
  //                0123456789012345678901234
  CHECK(state("UPDATE s SET n = t") ==
        "42804 column \"n\" is of type integer but expression is of type text @17" + std::string{kHint});
  CHECK(state("UPDATE s SET bo = n") ==
        "42804 column \"bo\" is of type boolean but expression is of type integer @18" + std::string{kHint});
  CHECK(state("UPDATE s SET n = bo").starts_with("42804"));
  CHECK(state("UPDATE s SET j = t").starts_with("42804"));
  CHECK(state("UPDATE s SET n = j").starts_with("42804"));
  CHECK(state("UPDATE s SET tm = dt") == "42804 column \"tm\" is of type time without time zone but expression is of type "
                                         "date @18" + std::string{kHint});
  CHECK(state("UPDATE s SET ts = tm").starts_with("42804"));
  CHECK(state("UPDATE s SET n = $1::text").starts_with("42804"));
  // Not taken yet: a float's text, NUMERIC of no precision.
  CHECK(state("UPDATE s SET t = f").starts_with("0A000"));
  CHECK(state("UPDATE s SET n = num").starts_with("0A000"));
  CHECK(state("UPDATE s SET num = n").starts_with("0A000"));
  // What a SET value may not hold.
  CHECK(state("UPDATE s SET n = sum(n)") == "42803 aggregate functions are not allowed in UPDATE @17");
  CHECK(state("UPDATE s SET n = (SELECT 1)").starts_with("42601"));
  CHECK(state("UPDATE s SET n = n + (SELECT 1)").starts_with("42601"));
  CHECK(state("INSERT INTO s (id, n) VALUES (1, n + 1)").starts_with("42601 INSERT values must be parameters or constants"));
  CHECK(state("INSERT INTO s (id, n) VALUES (1, 1) ON CONFLICT (id) DO UPDATE SET n = s.n + excluded.n")
            .starts_with("42601 EXCLUDED in an expression is not supported yet"));
  CHECK(state("INSERT INTO s (id, n) VALUES (1, 1) ON CONFLICT (id) DO UPDATE SET n = s.n * 2, t = upper(s.t)") == "bound");
  // $n::T of another type than its column's types $n as T; $n beside a
  // column is typed by it, as in a select list.
  {
    auto b = bind_sql(cat, "UPDATE s SET t = $1::int, n = n + $2, v = $3 || t");
    REQUIRE(b.params.size() >= 3);
    CHECK(b.params[0].type == SqlType{SqlKind::Integer});
    CHECK(b.params[1].type == SqlType{SqlKind::Integer});
    CHECK(b.params[2].type == SqlType{SqlKind::Text});
  }
  // Every item reads the row as read, stored in the table's column order.
  {
    auto set = set_of("UPDATE s SET t = t || n, n = n + 1, a = a - 1");
    REQUIRE(set.size() == 3);
    CHECK(set[0].first.col < set[1].first.col);
    CHECK(set[1].first.col < set[2].first.col);
  }
  // Two classes of error: a literal's input error is raised as met, in
  // written order, with its position; a folded constant's (1/0, a constant
  // out of its column's type) is the lowest-numbered column's, after the
  // statement is analysed, with none.
  CHECK(state("UPDATE s SET n = 'zz', a = 99999") == "22P02 invalid input syntax for type integer: \"zz\" @17");
  CHECK(state("UPDATE s SET a = '99999', n = 1/0") == "22003 value \"99999\" is out of range for type smallint @17");
  CHECK(state("UPDATE s SET n = 1/0, a = 99999") == "22003 smallint out of range @-1");
  CHECK(state("UPDATE s SET a = 99999, n = 1/0") == "22003 smallint out of range @-1");
  CHECK(state("UPDATE s SET n = n + 1/0, d = 1000") == "22012 division by zero @-1");
  CHECK(state("UPDATE s SET n = 1/0 WHERE id = 'zz'").starts_with("22P02"));
  CHECK(state("UPDATE s SET n = 1/0 RETURNING nosuch").starts_with("42703"));
  // A column assigned twice: PostgreSQL's rewriter's 42601, after every
  // analysis error and before a folding error, with no position.
  CHECK(state("UPDATE s SET n = 1, n = 2") == "42601 multiple assignments to same column \"n\" @-1");
  CHECK(state("UPDATE s SET n = n + 1, n = 2") == "42601 multiple assignments to same column \"n\" @-1");
  CHECK(state("UPDATE s SET n = 1/0, n = 2") == "42601 multiple assignments to same column \"n\" @-1");
  CHECK(state("UPDATE s SET n = 1, n = 2 WHERE nosuch = 1").starts_with("42703"));
  CHECK(state("UPDATE s SET n = 1, zz = 2") == "42703 column \"zz\" of relation \"s\" does not exist @20");
  CHECK(state("INSERT INTO s (id) VALUES (1) ON CONFLICT (id) DO UPDATE SET n = 1/0, n = 2") ==
        "42601 multiple assignments to same column \"n\" @-1");
  // A constant of any assignment PostgreSQL has, folded by C1's cast.
  for (auto sql : {"UPDATE s SET n = 2.5::float8", "UPDATE s SET d = 2.5::float8", "UPDATE s SET a = 2.5::real",
                   "UPDATE s SET dt = '2024-01-01 10:00'::timestamp", "INSERT INTO s (id, n) VALUES (1, 2.5::float8)",
                   "UPDATE s SET nm = true"}) {
    INFO(sql);
    CHECK(state(sql) == "bound");
  }
  CHECK(state("UPDATE s SET n = '1e30'::float8 WHERE false") == "22003 integer out of range @-1");
  CHECK(state("UPDATE s SET d = 1e10::float8").starts_with("22003 numeric field overflow"));
  // An untyped $n a SET list or a VALUES row types two ways: PostgreSQL's
  // 42P08 with its DETAIL, at the bare $n it coerces last.
  auto deduced = [&](std::string_view sql) -> std::string {
    try {
      (void)bind_sql(cat, sql);
    } catch (const ast::SqlError& e) {
      return std::string{e.sqlstate()} + " " + e.what() + " @" + std::to_string(e.pos()) + " | " + std::string{e.detail()};
    }
    return "bound";
  };
  //                 0         1         2         3
  //                 0123456789012345678901234567890123456789
  CHECK(deduced("UPDATE s SET a = $1, n = $1 + 1") ==
        "42P08 inconsistent types deduced for parameter $1 @17 | integer versus smallint");
  CHECK(deduced("UPDATE s SET nm = $1, a = $1") == "42P08 inconsistent types deduced for parameter $1 @26 | name versus smallint");
  CHECK(deduced("UPDATE s SET a = $1, t = lower($1)").starts_with("42P08"));
  CHECK(deduced("INSERT INTO s (id, a, n) VALUES (8, $1, $1)") ==
        "42P08 inconsistent types deduced for parameter $1 @40 | smallint versus integer");
  // Not where PostgreSQL types $1 first elsewhere, nor where it runs.
  CHECK(deduced("UPDATE s SET a = $1, n = $1 + 1 WHERE n = $1").starts_with("42601"));
  CHECK(deduced("UPDATE s SET n = $1 + 1, a = $1").starts_with("42601"));
  CHECK(deduced("UPDATE s SET n = $1::int, a = $1::smallint").starts_with("42601"));
  CHECK(deduced("UPDATE s SET a = $1, nm = $1, n = $1 + 1").starts_with("42601"));
}
