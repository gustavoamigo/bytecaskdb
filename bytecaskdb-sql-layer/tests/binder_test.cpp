// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
                      "d VARCHAR(4) DEFAULT 'h\xc3\xa9ll', e TIMESTAMP, f DATE, g TIME, h REAL DEFAULT 2, "
                      "i DOUBLE PRECISION DEFAULT 2.5, j BOOLEAN DEFAULT true, k TEXT, l BYTEA, m BIGINT DEFAULT 10000000000, "
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
    CHECK(s.columns[7].default_value == Datum{2.0});
    CHECK(s.columns[8].default_value == Datum{2.5});
    CHECK(s.columns[9].default_value == Datum{true});
    CHECK((kind(10) == SqlKind::Text && kind(11) == SqlKind::Bytea));
    CHECK(s.columns[12].default_value == Datum{std::int64_t{10'000'000'000}});
    CHECK(s.columns[13].sql == SqlType{SqlKind::Decimal, 0, 4, 0});
    CHECK(s.columns[13].default_value == Datum{std::int64_t{12}});
    CHECK(kind(14) == SqlKind::Text);  // VARCHAR without a length
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
  CHECK(rejected(cat, "CREATE TABLE u (k BIGINT PRIMARY KEY, v VARCHAR(2) DEFAULT 'abc')").first ==
        "DEFAULT too long for type varchar(2)");
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
}
