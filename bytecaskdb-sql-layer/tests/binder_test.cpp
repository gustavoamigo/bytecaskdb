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
import bytecask.sql.tests.fixture;

using namespace bytecask::sql;

namespace {

auto bind_sql(const Catalog& cat, std::string_view sql) -> bound::Bound { return bound::bind(ast::parse(sql), cat); }

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
    auto& s = std::get<bound::Insert>(b.stmt);
    CHECK(s.table->id == 1);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::Bytes, PhysType::I64});
    REQUIRE(s.values.size() == 3);
    CHECK(s.values[1]->param == 1);
  }
  {
    auto b = bind_sql(cat, "INSERT INTO t (qty, id) VALUES ($1, $2)");
    auto& s = std::get<bound::Insert>(b.stmt);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
    CHECK(s.values[0]->param == 1);
    CHECK_FALSE(s.values[1].has_value());  // name is not given: its default, NULL
    CHECK(s.values[2]->param == 0);
  }
  {
    auto b = bind_sql(cat, "SELECT id, qty FROM t WHERE id = $1");
    auto& s = std::get<bound::Select>(b.stmt);
    CHECK(s.targets == std::vector<std::uint32_t>{0, 2});
    REQUIRE(s.where.size() == 1);
    CHECK(s.where[0].col_id == 0);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64});
  }
  {
    auto b = bind_sql(cat, "SELECT id FROM t WHERE $1 <= name AND name < $2");
    auto& s = std::get<bound::Select>(b.stmt);
    CHECK(s.where[0].op == ast::Compare::Op::Ge);  // normalised: column on the left
    CHECK(b.param_types == std::vector<PhysType>{PhysType::Bytes, PhysType::Bytes});
  }
  {
    auto b = bind_sql(cat, "UPDATE t SET qty = qty - $2 WHERE id = $1");
    auto& s = std::get<bound::Update>(b.stmt);
    REQUIRE(s.set.size() == 1);
    CHECK(s.set[0].first == 2);
    CHECK(s.set[0].second.kind == bound::Expr::Kind::Sub);
    CHECK(b.param_types == std::vector<PhysType>{PhysType::I64, PhysType::I64});
  }
  {
    auto b = bind_sql(cat, "DELETE FROM t WHERE id = $1");
    CHECK(std::get<bound::Delete>(b.stmt).where.size() == 1);
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
                                            "e2 NUMERIC(12, 2), n0 NUMERIC(10, 0), PRIMARY KEY (a, b))")
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
    CHECK_NOTHROW(bind_sql(c2.cat(), "SELECT a FROM dr WHERE a = $1 AND b = $2 AND v = $3 AND w = $3"));
  }
  CHECK(rejected(cat, "SELECT id FROM t WHERE id = $2").first ==
        "could not determine data type of parameter $1");
  CHECK(rejected(cat, "UPDATE t SET name = name - $1 WHERE id = $2").first == "operator - is only defined on bigint");
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
