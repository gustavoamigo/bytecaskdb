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
  CHECK(rejected_at("SELECT id FROM t LEFT JOIN u ON t.id = u.id") == 27);
  CHECK(rejected_at("SELECT id FROM t, u, v WHERE id = $1") >= 0);
  CHECK(rejected_at("SELECT id FROM t WHERE id = 1") == 28);   // a literal
  CHECK(rejected_at("SELECT id FROM t WHERE id = $1 OR id = $2") >= 0);
  CHECK(rejected_at("SELEC id FROM t") == 0);                   // PostgreSQL's own syntax error
  CHECK(rejected_at("SELECT id FROM t; SELECT id FROM t") == -1);
  CHECK(rejected_at("DROP VIEW v") == 0);
  CHECK(rejected_at("DROP TABLE t CASCADE") == 0);
  CHECK(rejected_at("ALTER TABLE t DROP COLUMN x") == 0);
  CHECK_THROWS_AS(parse("CREATE INDEX u ON t (name NULLS LAST)"), SqlError);
  CHECK_THROWS_AS(parse("CREATE INDEX CONCURRENTLY u ON t (name)"), SqlError);
  CHECK_THROWS_AS(parse("CREATE UNIQUE INDEX u ON t (name) NULLS NOT DISTINCT"), SqlError);
  CHECK_THROWS_AS(parse("CREATE TABLE u (k BIGINT DEFAULT now())"), SqlError);
  //                    0         1         2         3         4
  //                    01234567890123456789012345678901234567890123456789
  CHECK(rejected_at("SELECT id FROM t WHERE id = $1 LIMIT -1") == 37);
  CHECK(rejected_at("SELECT id FROM t WHERE id = $1 LIMIT 1.5") == 37);
  CHECK(rejected_at("SELECT max(qty) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT sum(*) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT min(qty + $2) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT sum(DISTINCT qty) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT id FROM t WHERE id = $1 ORDER BY id NULLS FIRST") == 40);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 LIMIT $2"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 OFFSET 1"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 ORDER BY qty + $2"), SqlError);
}

TEST_CASE("parser: COUNT, ORDER BY, LIMIT and column + parameter", "[sql][parser]") {
  auto s = std::get<Select>(parse("SELECT count(*), count(qty) FROM t WHERE name = $1 ORDER BY name DESC, id LIMIT 3"));
  REQUIRE(s.targets.size() == 2);
  CHECK(s.targets[0].kind == Expr::Kind::Aggregate);
  CHECK(s.targets[0].fn == Expr::Fn::Count);
  CHECK(s.targets[0].column.empty());
  CHECK(s.targets[1].column == "qty");
  auto sum_min = std::get<Select>(parse("SELECT sum(qty), pg_catalog.min(t.qty) FROM t WHERE name = $1"));
  REQUIRE(sum_min.targets.size() == 2);
  CHECK(sum_min.targets[0].fn == Expr::Fn::Sum);
  CHECK(sum_min.targets[0].column == "qty");
  CHECK(sum_min.targets[1].fn == Expr::Fn::Min);
  CHECK(sum_min.targets[1].table == "t");
  auto distinct = std::get<Select>(parse("SELECT count(DISTINCT qty) FROM t WHERE name = $1"));
  CHECK(distinct.targets[0].fn == Expr::Fn::Count);
  CHECK(distinct.targets[0].distinct);
  CHECK_FALSE(sum_min.targets[0].distinct);
  REQUIRE(s.order_by.size() == 2);
  CHECK(s.order_by[0].column == "name");
  CHECK(s.order_by[0].desc);
  CHECK_FALSE(s.order_by[1].desc);
  CHECK(s.limit == 3);
  CHECK_FALSE(std::get<Select>(parse("SELECT id FROM t WHERE id = $1 LIMIT ALL")).limit);
  CHECK_FALSE(std::get<Select>(parse("SELECT id FROM t WHERE id = $1 LIMIT NULL")).limit);
  CHECK(std::get<Select>(parse("SELECT id FROM t WHERE id = $1 LIMIT 10000000000")).limit == 10000000000);
  auto u = std::get<Update>(parse("UPDATE t SET qty = qty + $2 WHERE id = $1"));
  CHECK(u.set[0].value.kind == Expr::Kind::Add);
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

TEST_CASE("parser: SELECT … FOR UPDATE, and no other locking clause", "[sql][parser][txn]") {
  auto sel = std::get<Select>(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE"));
  CHECK(sel.for_update);
  CHECK_FALSE(std::get<Select>(parse("SELECT id FROM t WHERE id = $1")).for_update);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR SHARE"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR NO KEY UPDATE"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE NOWAIT"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE SKIP LOCKED"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE OF t"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE FOR UPDATE"), SqlError);
}

TEST_CASE("parser: two tables, qualified columns and integer literals", "[sql][parser]") {
  auto j = std::get<Select>(parse("SELECT t.id, qty FROM t, u WHERE t.id = $1 AND k = $2"));
  CHECK(j.table == "t");
  CHECK(j.join == "u");
  CHECK(j.targets[0].table == "t");
  CHECK(j.targets[1].table.empty());
  REQUIRE(j.where.size() == 2);
  CHECK(j.where[0].lhs.table == "t");
  // JOIN … ON: the ON conjuncts come first, then the WHERE's.
  auto on = std::get<Select>(parse("SELECT id FROM t JOIN u ON k = $1 WHERE id = $2"));
  CHECK(on.join == "u");
  REQUIRE(on.where.size() == 2);
  CHECK(on.where[0].lhs.column == "k");
  auto u = std::get<Update>(parse("UPDATE t SET qty = qty + 1 WHERE id = $1"));
  CHECK(u.set[0].value.rhs->kind == Expr::Kind::Const);
  CHECK(u.set[0].value.rhs->constant == 1);
  CHECK(std::get<Update>(parse("UPDATE t SET qty = qty - 10000000000 WHERE id = $1")).set[0].value.rhs->constant ==
        10000000000);
  CHECK_THROWS_AS(parse("UPDATE t SET qty = qty + 1.5 WHERE id = $1"), SqlError);
  CHECK_THROWS_AS(parse("UPDATE t SET qty = qty + 99999999999999999999 WHERE id = $1"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = 1"), SqlError);
  try {
    (void)parse("SELECT id FROM t JOIN u ON id = $1 OR k = $2");
    FAIL("accepted");
  } catch (const SqlError& e) {
    CHECK(std::string{e.what()} == "only AND is supported in ON");
  }
}

