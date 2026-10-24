// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
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
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT)").first == "a table needs a PRIMARY KEY");
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = qty").first == "WHERE must compare a column with a parameter");
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
    CHECK(std::get<logical::Limit>(root(b).op).n == 2);
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
  CHECK(rejected(cat, "SELECT count(*), count(id) FROM t WHERE id = $1") ==
        std::pair<std::string, std::int32_t>{"only one aggregate per SELECT is supported", 17});
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
        "ON must compare a column with a parameter");
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

