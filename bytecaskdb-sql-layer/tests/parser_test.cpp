// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import bytecask.sql.parser;

using namespace bytecask::sql::ast;

namespace {
auto rejected_at(std::string_view sql) -> std::int32_t {
  try {
    (void)parse(sql);
  } catch (const SqlError& e) {
    return e.pos();
  }
  FAIL("accepted: " << sql);
  return -2;
}
}  // namespace

TEST_CASE("parser: the seven statements of the prototype", "[sql][parser]") {
  {
    auto s = std::get<CreateTable>(parse("CREATE TABLE t (id BIGINT PRIMARY KEY, name BYTEA, qty BIGINT)"));
    CHECK(s.table == "t");
    REQUIRE(s.columns.size() == 3);
    CHECK(s.columns[0].name == "id");
    CHECK(s.columns[0].type == "int8");  // libpg_query canonicalises BIGINT
    CHECK(s.columns[0].primary_key);
    CHECK(s.columns[1].type == "bytea");
    CHECK_FALSE(s.columns[1].not_null);
  }
  {
    auto s = std::get<CreateIndex>(parse("CREATE INDEX t_by_name ON t (name)"));
    CHECK(s.index == "t_by_name");
    CHECK(s.table == "t");
    REQUIRE(s.columns.size() == 1);
    CHECK(s.columns[0].name == "name");
  }
  {
    auto s = std::get<Insert>(parse("INSERT INTO t VALUES ($1, $2, $3)"));
    CHECK(s.table == "t");
    CHECK(s.columns.empty());
    REQUIRE(s.values.size() == 3);
    CHECK(s.values[2].kind == Expr::Kind::Param);
    CHECK(s.values[2].param == 2);
  }
  {
    auto s = std::get<Select>(parse("SELECT id, qty FROM t WHERE id = $1"));
    REQUIRE(s.targets.size() == 2);
    CHECK(s.targets[1].column == "qty");
    REQUIRE(s.where.size() == 1);
    CHECK(s.where[0].op == Compare::Op::Eq);
    CHECK(s.where[0].lhs.column == "id");
    CHECK(s.where[0].rhs.param == 0);
  }
  {
    auto s = std::get<Select>(parse("SELECT id FROM t WHERE name >= $1 AND name < $2"));
    REQUIRE(s.where.size() == 2);
    CHECK(s.where[0].op == Compare::Op::Ge);
    CHECK(s.where[1].op == Compare::Op::Lt);
    CHECK(s.where[1].rhs.param == 1);
  }
  {
    auto s = std::get<Update>(parse("UPDATE t SET qty = qty - $2 WHERE id = $1"));
    REQUIRE(s.set.size() == 1);
    CHECK(s.set[0].column == "qty");
    CHECK(s.set[0].value.kind == Expr::Kind::Sub);
    CHECK(s.set[0].value.lhs->column == "qty");
    CHECK(s.set[0].value.rhs->param == 1);
    CHECK(s.where[0].rhs.param == 0);
  }
  {
    auto s = std::get<Delete>(parse("DELETE FROM t WHERE id = $1"));
    CHECK(s.table == "t");
    REQUIRE(s.where.size() == 1);
  }
}

TEST_CASE("parser: forms outside the scope are rejected with a position", "[sql][parser]") {
  //                    0         1         2         3
  //                    0123456789012345678901234567890123456789
  CHECK(rejected_at("SELECT id FROM t JOIN u ON t.id = u.id") == 22);
  CHECK(rejected_at("SELECT id FROM t WHERE id = 1") == 28);   // a literal
  CHECK(rejected_at("SELECT id FROM t WHERE id = $1 OR id = $2") >= 0);
  CHECK(rejected_at("SELEC id FROM t") == 0);                   // PostgreSQL's own syntax error
  CHECK(rejected_at("SELECT id FROM t; SELECT id FROM t") == -1);
  CHECK(rejected_at("DROP VIEW v") == 0);
  CHECK(rejected_at("DROP TABLE t CASCADE") == 0);
  CHECK(rejected_at("ALTER TABLE t DROP COLUMN x") == 0);
  CHECK_THROWS_AS(parse("CREATE INDEX u ON t (name NULLS LAST)"), SqlError);
  CHECK_THROWS_AS(parse("CREATE INDEX CONCURRENTLY u ON t (name)"), SqlError);
  CHECK_THROWS_AS(parse("CREATE TABLE u (k BIGINT DEFAULT now())"), SqlError);
}

TEST_CASE("parser: the stage-1 DDL forms", "[sql][parser][catalog]") {
  {
    auto s = std::get<CreateTable>(parse("CREATE TABLE u (k INTEGER NOT NULL DEFAULT -7, v VARCHAR(16) DEFAULT 'x', "
                                         "d NUMERIC(12, 2) DEFAULT 1.25, b BOOLEAN DEFAULT false, n TEXT DEFAULT NULL, "
                                         "PRIMARY KEY (k))"));
    REQUIRE(s.columns.size() == 5);
    CHECK(s.columns[0].type == "int4");
    CHECK(s.columns[0].default_value->kind == Literal::Kind::Integer);
    CHECK(s.columns[0].default_value->text == "-7");
    CHECK(s.columns[1].typmods == std::vector<std::int32_t>{16});
    CHECK(s.columns[1].default_value->text == "x");
    CHECK(s.columns[2].typmods == std::vector<std::int32_t>{12, 2});
    CHECK(s.columns[2].default_value->kind == Literal::Kind::Number);
    CHECK(s.columns[3].default_value->kind == Literal::Kind::Bool);
    CHECK_FALSE(s.columns[3].default_value->b);
    CHECK(s.columns[4].default_value->kind == Literal::Kind::Null);
  }
  {
    auto s = std::get<CreateIndex>(parse("CREATE UNIQUE INDEX u ON t (a DESC, b ASC, c)"));
    CHECK(s.unique);
    REQUIRE(s.columns.size() == 3);
    CHECK(s.columns[0].desc);
    CHECK_FALSE(s.columns[1].desc);
    CHECK_FALSE(s.columns[2].desc);
  }
  CHECK(std::get<DropTable>(parse("DROP TABLE t")).table == "t");
  CHECK(std::get<DropTable>(parse("DROP TABLE IF EXISTS t")).if_exists);
  CHECK(std::get<DropIndex>(parse("DROP INDEX IF EXISTS i")).index == "i");
  auto a = std::get<AddColumn>(parse("ALTER TABLE t ADD COLUMN c BIGINT DEFAULT 10000000000"));
  CHECK(a.table == "t");
  CHECK(a.column.name == "c");
  CHECK(a.column.default_value->kind == Literal::Kind::Integer);  // past 32 bits: PostgreSQL's Float node
  CHECK(a.column.default_value->text == "10000000000");
}
