// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    REQUIRE(s.values.size() == 3);
    CHECK(s.values[1]->param == 1);
  }
  {
    auto b = bind_sql(cat, "INSERT INTO t (qty, id) VALUES ($1, $2)");
    const auto& s = get<logical::Insert>(b);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
    CHECK(s.values[0]->param == 1);
    CHECK_FALSE(s.values[1].has_value());  // name is not given: its default, NULL
    CHECK(s.values[2]->param == 0);
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
  CHECK(rejected(cat, "UPDATE t SET name = name - $1 WHERE id = $2").first ==
        "operator - is only supported on an integer or DECIMAL column of the assigned column's type");
  CHECK(rejected(cat, "UPDATE t SET id = $1 WHERE id = $2").second == 13);
  CHECK(rejected(cat, "INSERT INTO t VALUES ($1, $2, $3, $4)").first == "INSERT has more expressions than target columns");
  CHECK(rejected(cat, "INSERT INTO t (id, qty) VALUES ($1)").first == "INSERT has 1 values for 2 columns");
  CHECK(rejected(cat, "CREATE TABLE u (k CHAR(5) PRIMARY KEY)") ==
        std::pair<std::string, std::int32_t>{"CHAR(n) is not supported; use VARCHAR(n)", 16});
  // No primary key: the catalog gives the table a row id.
  CHECK(std::get<CreateTableDdl>(std::get<Ddl>(bind_sql(cat, "CREATE TABLE u (k BIGINT)").stmt)).pk.empty());
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = qty").first == "WHERE must compare a column with a parameter or a constant");
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
  CHECK(rejected(cat, "SELECT k < 5 FROM ct").first == "only columns, constants and aggregates can be selected");

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
  CHECK(rejected(cat, "SELECT CAST(1 AS TEXT)").first == "a cast of other than a string literal or NULL is not supported yet");
  CHECK(rejected(cat, "SELECT CAST('abc' AS VARCHAR(2))").first == "a cast to VARCHAR(n) is not supported yet");
  // Where a typed constant meets a column.
  CHECK(bind_sql(cat, "INSERT INTO ct VALUES ($1, DATE '1970-01-03', DATE '1970-01-03')").fixed ==
        std::vector<Datum>{std::int64_t{2}, sql_test::bytes("1970-01-03")});
  CHECK(bind_sql(cat, "SELECT k FROM ct WHERE dt >= '1970-01-03'::date").fixed == std::vector<Datum>{std::int64_t{2}});
  CHECK(rejected(cat, "INSERT INTO ct (k, dt) VALUES ($1, TIMESTAMP '2020-01-01 00:00:00')").first ==
        "a timestamp without time zone constant assigned to a date column is not supported yet");
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
  CHECK(agg.group_by[0].col.col == f.cat().table("t")->column("qty")->id);
  CHECK(agg.group_by[1].col.col == f.cat().table("t")->column("name")->id);
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
  CHECK(rejected(cat, "SELECT DISTINCT qty, 5 FROM t").first == "SELECT DISTINCT of other than columns is not supported yet");
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
  const auto* l = cat.table("l");
  const auto* r = cat.table("r");
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
  CHECK(rejected(cat, "SELECT k FROM l FULL JOIN r USING (a)").first == "FULL JOIN is not supported");
}

TEST_CASE("binder: LEFT and RIGHT JOIN are a left outer join, ON its predicate, WHERE above it", "[sql][binder]") {
  sql_test::Fixture f;
  for (auto ddl : {"CREATE TABLE l (k INTEGER PRIMARY KEY, a INTEGER NOT NULL)",
                   "CREATE TABLE r (j INTEGER PRIMARY KEY, a INTEGER NOT NULL)"}) {
    f.ddl(std::get<Ddl>(bind_sql(f.cat(), ddl).stmt));
  }
  const auto& cat = f.cat();
  const auto* l = cat.table("l");
  const auto* r = cat.table("r");
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
  CHECK(rejected(cat, "SELECT l.k FROM l FULL JOIN r ON l.a = r.a").first == "FULL JOIN is not supported");
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
  // Refused: correlated, nested, of two columns, FOR UPDATE, AVG, assigned,
  // compared across types, beside an aggregate.
  CHECK(state("SELECT id FROM t WHERE id IN (SELECT k FROM u WHERE k = qty)").starts_with("42601 a correlated subquery"));
  CHECK(state("SELECT id FROM t WHERE id IN (SELECT k FROM u WHERE u.k = t.qty)").starts_with("42601 a correlated subquery"));
  CHECK(state("SELECT id FROM t WHERE id IN (SELECT k FROM u WHERE k IN (SELECT k FROM u))") ==
        "42601 a subquery inside a subquery is not supported yet");
  CHECK(state("SELECT id FROM t WHERE id = (SELECT k, w FROM u)") == "42601 subquery must return only one column");
  CHECK(state("SELECT id FROM t WHERE id IN (SELECT k FROM u FOR UPDATE)") == "42601 FOR UPDATE in a subquery is not supported yet");
  CHECK(state("SELECT id FROM t WHERE id = (SELECT AVG(k) FROM u)") == "42601 a subquery of AVG is not supported yet");
  CHECK(state("UPDATE t SET qty = (SELECT MAX(k) FROM u) WHERE id = 1") == "42601 a subquery assigned to a column is not supported yet");
  CHECK(state("SELECT id FROM t WHERE id = (SELECT w FROM u)").starts_with("42601 comparing id (bigint) with a subquery of text"));
  CHECK(state("SELECT COUNT(*), (SELECT 1) FROM t") == "42601 a subquery in the select list of an aggregated query is not supported yet");
  CHECK(state("SELECT id FROM t WHERE (SELECT 1) IN (SELECT k FROM u)").starts_with("42601"));
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
  CHECK(rejected(cat, "SELECT id FROM t WHERE qty = 1 OR qty = id").first ==
        "WHERE must compare a column with a parameter or a constant");
  CHECK(rejected(cat, "SELECT id FROM t WHERE qty = $1 + 1 OR id = 2").first ==
        "$n + k and $n - k inside AND, OR or NOT is not supported yet");
  CHECK(rejected(cat, "SELECT id FROM t WHERE 1 IS NULL").first == "IS NULL of other than a column is not supported yet");
  CHECK(rejected(cat, "SELECT qty FROM t GROUP BY qty HAVING qty <> 1").first ==
        "HAVING with AND, OR, NOT, IS NULL, <>, BETWEEN or IN is not supported yet");
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
  CHECK(rejected(cat, "SELECT k FROM lk WHERE v ILIKE 'a%'").first == "ILIKE is not supported: its case folding is a locale's");
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
  CHECK(std::get<DropIndexDdl>(std::get<Ddl>(bind_sql(cat, "DROP INDEX t_by_name").stmt)).name == "t_by_name");
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
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC)").first.starts_with("NUMERIC needs a precision"));
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v NUMERIC(19, 2))").first.starts_with("NUMERIC(19, 2)"));
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v TIMESTAMPTZ)").first == "type timestamptz is not supported");
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
  CHECK(rejected(cat, "UPDATE t SET qty = count(*) WHERE id = $1").first == "aggregate functions are not allowed here");
  // SUM and MIN take a column held as an int64_t; SUM an exact number.
  {
    auto b = bind_sql(cat, "SELECT sum(qty) FROM t WHERE name = $1");
    const auto& sum = get<logical::Aggregate>(b).aggs.at(0);
    CHECK(sum.expr.fn == logical::Expr::Fn::Sum);
    CHECK(sum.name == "sum");
    CHECK(sql_type_name(sum.expr.type) == "numeric");  // of a BIGINT: no declared precision
  }
  CHECK(rejected(cat, "SELECT sum(name) FROM t WHERE id = $1") ==
        std::pair<std::string, std::int32_t>{"SUM of a bytea column is not supported", 7});
  CHECK(rejected(cat, "SELECT min(name) FROM t WHERE id = $1").first == "MIN of a bytea column is not supported");
  CHECK(rejected(cat, "SELECT min(qty), id FROM t WHERE id = $1").first ==
        "column \"t.id\" must appear in the GROUP BY clause or be used in an aggregate function");
  CHECK(rejected(cat, "SELECT sum(qty) FROM t WHERE id = $1 FOR UPDATE").first ==
        "FOR UPDATE is not allowed with aggregate functions");
  // One SQL type per parameter, whichever use comes first.
  sql_test::Fixture g;
  g.ddl(std::get<Ddl>(bind_sql(g.cat(), "CREATE TABLE s (k BIGINT PRIMARY KEY, b BIGINT, a SMALLINT)").stmt));
  CHECK(rejected(g.cat(), "UPDATE s SET b = $1, a = a + $1 WHERE k = $2").first ==
        "parameter $1 is assigned to b (bigint) and added to a (smallint)");
  CHECK(rejected(g.cat(), "UPDATE s SET a = a - $1, b = $1 WHERE k = $2").first ==
        "parameter $1 is subtracted from a (smallint) and assigned to b (bigint)");
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
    CHECK(targets[1].expr.col.table == cat.table("u")->id);
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
    CHECK(rejected(g.cat(), "SELECT o FROM ol WHERE w = $1 AND s < $2 + 1").first ==
          "$n + n and $n - n are supported only compared with an INTEGER column yet");
    CHECK(rejected(g.cat(), "SELECT o FROM ol WHERE w = $1 AND o < $2 + 2147483648").first ==
          "an integer literal past INTEGER is not supported here yet");
  }
  {
    // A literal is in its column's representation: a DECIMAL's scaled.
    auto b = bind_sql(cat, "UPDATE u SET d = d + 1, qty = qty - 2 WHERE k = $1");
    const auto& up = get<logical::Update>(b);
    CHECK(up.set[0].second.args[1].constant == 100);
    CHECK(up.set[1].second.args[1].constant == 2);
  }
  //                       0         1         2         3         4
  //                       01234567890123456789012345678901234567890123456789
  CHECK(rejected(cat, "SELECT qty FROM t, u WHERE id = $1 AND k = $2") ==
        std::pair<std::string, std::int32_t>{"column reference \"qty\" is ambiguous", 7});
  CHECK(rejected(cat, "SELECT v.qty FROM t, u WHERE id = $1").first == "missing FROM-clause entry for table \"v\"");
  CHECK(rejected(cat, "SELECT v.qty FROM t WHERE id = $1").first == "missing FROM-clause entry for table \"v\"");
  CHECK(rejected(cat, "SELECT id FROM t, t WHERE id = $1").first == "a table joined with itself is not supported");
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE id = $1 AND k = $2 FOR UPDATE").first ==
        "FOR UPDATE on a join is not supported yet");
  {
    // A column of one table equal to one of the other joins them; an
    // aggregate on a join takes either table's column.
    auto b = bind_sql(cat, "SELECT count(DISTINCT u.qty) FROM t JOIN u ON u.qty = id WHERE id >= $1");
    logical::rewrite(root(b));
    const auto& on = get<logical::Join>(b).predicate;
    REQUIRE(on.size() == 1);  // pushed down onto the join: it reads both tables
    CHECK(on[0].args[0].col.table == cat.table("u")->id);
    CHECK(on[0].args[0].col.col == 1u);  // u.qty
    CHECK(on[0].args[1].col.col == 0u);  // t.id
    const auto& a = get<logical::Aggregate>(b).aggs.at(0).expr;
    CHECK(a.distinct);
    CHECK(a.args.at(0).col.table == cat.table("u")->id);
  }
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE id = k AND qty = id").first ==
        "column reference \"qty\" is ambiguous");
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE t.id = t.qty").first ==
        "comparing two columns of one table is not supported yet");
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE id < k").first == "a join compares two columns with = only yet");
  CHECK(rejected(cat, "SELECT id FROM t, u WHERE name = k").first ==
        "comparing name (bytea) with k (bigint) is not supported: a join compares columns of one type");
  CHECK(rejected(cat, "SELECT u.qty, count(*) FROM t, u WHERE id = k").first ==
        "column \"u.qty\" must appear in the GROUP BY clause or be used in an aggregate function");
  CHECK(rejected(cat, "SELECT id FROM t JOIN u ON id = $1 + k WHERE k = $2").first ==
        "ON must compare a column with a parameter or a constant");
  {
    // A range refusal names the table of the column the parameter is compared with.
    auto b = bind_sql(cat, "SELECT name FROM t, u WHERE id = $1 AND k = $2");
    CHECK(b.param_table == std::vector<std::uint32_t>{cat.table("t")->id, cat.table("u")->id});
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
  CHECK(state("SELECT sum(ts) FROM u WHERE k = $1") == "42883");

  // PostgreSQL runs these: the layer refuses a form it does not support, and
  // does not claim a code. Syntax errors are 42601 too.
  CHECK(state("UPDATE u SET dbl = qty WHERE k = $1") == "42601");  // bigint to double precision
  CHECK(state("UPDATE u SET s = qty WHERE k = $1") == "42601");    // any type to text
  CHECK(state("UPDATE u SET qty = dbl WHERE k = $1") == "42601");
  CHECK(state("SELECT k FROM u WHERE d = $1 AND dbl = $1") == "42601");
  CHECK(state("UPDATE u SET dbl = $1, qty = $1 WHERE k = $2") == "42601");  // 42P08 there: not claimed
  CHECK(state("UPDATE u SET s = $1, qty = $1 WHERE k = $2") == "42601");
  CHECK(state("UPDATE u SET s = $1 WHERE qty = $1") == "42601");  // PostgreSQL types the WHERE's $1 first
  CHECK(state("UPDATE u SET qty = qty + $1 WHERE ts = $1") == "42601");
  CHECK(state("SELECT sum(dbl) FROM u WHERE k = $1") == "42601");
  CHECK(state("SELECT min(dbl) FROM u WHERE k = $1") == "42601");
  CHECK(state("SELECT min(s) FROM u WHERE k = $1") == "42601");
  CHECK(state("SELECT sum(tm) FROM u WHERE k = $1") == "42601");  // an interval sum there
  CHECK(state("SELEC 1") == "42601");
  CHECK(state("SELECT id FROM t WHERE id = qty") == "42601");
  CHECK(state("CREATE TABLE u2 (k BIGINT PRIMARY KEY, v TIMESTAMPTZ)") == "42601");
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
  CHECK(state("SELECT qty FROM t UNION SELECT big FROM u ORDER BY qty") == "42601");
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
  CHECK(rejected(cat, "SELECT max(s) FROM g").first == "MAX of a text column is not supported");
  CHECK(rejected(cat, "SELECT avg(day) FROM g").first == "AVG of a date column is not supported");
  CHECK(rejected(cat, "SELECT avg(f) FROM g").first == "AVG of a double precision column is not supported");
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a HAVING avg(a) > $1").first == "HAVING on AVG is not supported yet");
  CHECK(rejected(cat, "SELECT a FROM g GROUP BY a + 1").first == "GROUP BY takes columns");
}
