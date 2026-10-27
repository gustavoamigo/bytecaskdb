// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <optional>
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

TEST_CASE("parser: REFERENCES and FOREIGN KEY, NO ACTION or RESTRICT only, in the order written", "[sql][parser]") {
  auto t = std::get<CreateTable>(parse("CREATE TABLE c (a INT REFERENCES p (x), b INT CONSTRAINT bp REFERENCES p, "
                                       "CONSTRAINT ab FOREIGN KEY (a, b) REFERENCES q (y, x) ON DELETE NO ACTION "
                                       "ON UPDATE RESTRICT)"));
  REQUIRE(t.foreign_keys.size() == 3);
  const auto& f = t.foreign_keys;
  CHECK((f[0].name.empty() && f[0].columns == std::vector<std::string>{"a"} && f[0].table == "p" &&
         f[0].ref_columns == std::vector<std::string>{"x"}));
  CHECK((f[1].name == "bp" && f[1].columns == std::vector<std::string>{"b"} && f[1].ref_columns.empty()));
  CHECK((f[2].name == "ab" && f[2].columns == std::vector<std::string>{"a", "b"} && f[2].table == "q" &&
         f[2].ref_columns == std::vector<std::string>{"y", "x"}));
  CHECK(t.columns[0].references.empty());  // moved to the table's list
  for (auto sql : {"CREATE TABLE c (a INT REFERENCES p ON DELETE CASCADE)", "CREATE TABLE c (a INT REFERENCES p ON UPDATE SET NULL)",
                   "CREATE TABLE c (a INT REFERENCES p ON DELETE SET DEFAULT)", "CREATE TABLE c (a INT REFERENCES p MATCH FULL)",
                   "CREATE TABLE c (a INT REFERENCES p DEFERRABLE)",
                   "CREATE TABLE c (a INT, FOREIGN KEY (a) REFERENCES p INITIALLY DEFERRED)"}) {
    INFO(sql);
    CHECK_THROWS_AS(parse(sql), SqlError);
  }
}

TEST_CASE("parser: subqueries as values, EXISTS, IN and ANY, SOME or ALL", "[sql][parser]") {
  auto sel = std::get<Select>(parse("SELECT (SELECT a FROM u) FROM t WHERE id = (SELECT MAX(b) FROM u) AND "
                                    "EXISTS (SELECT 1 FROM u) AND id IN (SELECT a FROM u) AND id NOT IN (SELECT a FROM u) "
                                    "AND id < ALL (SELECT a FROM u) AND id <> SOME (SELECT a FROM u)"));
  REQUIRE(sel.targets[0].kind == Expr::Kind::Subquery);
  CHECK(sel.targets[0].subquery->table == "u");
  REQUIRE(sel.where.size() == 6);
  CHECK(sel.where[0].rhs.kind == Expr::Kind::Subquery);
  CHECK(sel.where[1].kind == Compare::Kind::Exists);
  const auto& in = sel.where[2];
  CHECK((in.kind == Compare::Kind::Quantified && in.quantified == ValueCompare::Eq && !in.all));
  CHECK((sel.where[3].kind == Compare::Kind::Not && sel.where[3].args[0].kind == Compare::Kind::Quantified));
  CHECK((sel.where[4].quantified == ValueCompare::Lt && sel.where[4].all));
  CHECK((sel.where[5].quantified == ValueCompare::Ne && !sel.where[5].all));
  for (auto sql : {"SELECT id FROM t WHERE (SELECT 1)", "SELECT id FROM t WHERE (id, qty) IN (SELECT a, b FROM u)",
                   "SELECT id FROM t WHERE id = ARRAY(SELECT a FROM u)", "SELECT id FROM t WHERE id IN (VALUES (1))"}) {
    INFO(sql);
    CHECK_THROWS_AS(parse(sql), SqlError);
  }
}

TEST_CASE("parser: UNIQUE on a column or the table, in the order written", "[sql][parser]") {
  auto t = std::get<CreateTable>(parse("CREATE TABLE u (a INT UNIQUE, b INT CONSTRAINT bu UNIQUE, "
                                       "CONSTRAINT ab UNIQUE (a, b), UNIQUE (b))"));
  REQUIRE(t.unique.size() == 4);
  CHECK((t.unique[0].name.empty() && t.unique[0].columns == std::vector<std::string>{"a"}));
  CHECK((t.unique[1].name == "bu" && t.unique[1].columns == std::vector<std::string>{"b"}));
  CHECK((t.unique[2].name == "ab" && t.unique[2].columns == std::vector<std::string>{"a", "b"}));
  CHECK((t.unique[3].name.empty() && t.unique[3].columns == std::vector<std::string>{"b"}));
  CHECK(t.columns[0].unique == std::vector<std::string>{""});
  for (auto sql : {"CREATE TABLE u (a INT UNIQUE NULLS NOT DISTINCT)", "CREATE TABLE u (a INT, b INT, UNIQUE (a) INCLUDE (b))",
                   "CREATE TABLE u (a INT UNIQUE DEFERRABLE)", "CREATE TABLE u (a INT, CHECK (a > 0))"}) {
    INFO(sql);
    CHECK_THROWS_AS(parse(sql), SqlError);
  }
}

TEST_CASE("parser: forms outside the scope are rejected with a position", "[sql][parser]") {
  //                    0         1         2         3
  //                    0123456789012345678901234567890123456789
  CHECK(rejected_at("SELECT id FROM t FULL JOIN u ON t.id = u.id") == 27);
  CHECK(rejected_at("SELECT id FROM t, u, v WHERE id = $1") >= 0);
  CHECK(rejected_at("SELECT id FROM t WHERE id = B'1'") == 28);  // a bit-string literal
  CHECK(rejected_at("VALUES (1)") == 0);
  CHECK(rejected_at("SELECT id FROM t WHERE id ILIKE $1") >= 0);
  CHECK(rejected_at("SELEC id FROM t") == 0);                   // PostgreSQL's own syntax error
  CHECK(rejected_at("SELECT id FROM t; SELECT id FROM t") == -1);
  CHECK(rejected_at("DROP SEQUENCE s") == 0);
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
  CHECK(rejected_at("SELECT stddev(qty) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT sum(*) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT min(qty + $2) FROM t WHERE id = $1") == 7);
  CHECK(rejected_at("SELECT sum(DISTINCT qty) FROM t WHERE id = $1") == 7);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 LIMIT $2"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 OFFSET 1"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 ORDER BY qty + $2"), SqlError);
}

TEST_CASE("parser: ORDER BY takes NULLS FIRST and NULLS LAST; an index does not", "[sql][parser]") {
  auto s = std::get<Select>(parse("SELECT id FROM t WHERE id = $1 ORDER BY id NULLS FIRST, qty DESC NULLS LAST, name"));
  REQUIRE(s.order_by.size() == 3);
  CHECK(s.order_by[0].nulls_first == std::optional<bool>{true});
  CHECK(s.order_by[1].desc);
  CHECK(s.order_by[1].nulls_first == std::optional<bool>{false});
  CHECK_FALSE(s.order_by[2].nulls_first.has_value());
  CHECK_THROWS_AS(parse("CREATE INDEX u ON t (name NULLS FIRST)"), SqlError);
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

TEST_CASE("parser: CREATE VIEW and DROP VIEW", "[sql][parser][view]") {
  const std::string text = "CREATE VIEW v (x, y) AS SELECT a, b FROM t WHERE a > 1";
  auto v = std::get<CreateView>(parse(text));
  CHECK(v.view == "v");
  CHECK(v.columns == std::vector<std::string>{"x", "y"});
  CHECK(v.query.table == "t");
  CHECK(v.query.targets.size() == 2);
  CHECK(v.definition == text);  // what the catalog stores and parses again
  CHECK(std::get<CreateView>(parse("CREATE VIEW v AS SELECT 1")).columns.empty());
  CHECK(std::get<DropView>(parse("DROP VIEW v")).view == "v");
  CHECK(std::get<DropView>(parse("DROP VIEW IF EXISTS v")).if_exists);
  // Updatable views, and what only a stored definition could change, are not supported.
  CHECK_THROWS_AS(parse("CREATE VIEW v AS SELECT a FROM t WITH CHECK OPTION"), SqlError);
  CHECK_THROWS_AS(parse("CREATE VIEW v AS SELECT a FROM t WITH LOCAL CHECK OPTION"), SqlError);
  CHECK_THROWS_AS(parse("CREATE VIEW v AS SELECT a FROM t WITH CASCADED CHECK OPTION"), SqlError);
  CHECK_THROWS_AS(parse("CREATE OR REPLACE VIEW v AS SELECT a FROM t"), SqlError);
  CHECK_THROWS_AS(parse("CREATE TEMPORARY VIEW v AS SELECT a FROM t"), SqlError);
  CHECK_THROWS_AS(parse("CREATE VIEW v WITH (security_barrier) AS SELECT a FROM t"), SqlError);
  CHECK_THROWS_AS(parse("CREATE VIEW s.v AS SELECT a FROM t"), SqlError);
  CHECK_THROWS_AS(parse("CREATE VIEW v AS SELECT a FROM t UNION SELECT a FROM u"), SqlError);
  CHECK_THROWS_AS(parse("DROP VIEW a, b"), SqlError);
  CHECK_THROWS_AS(parse("DROP VIEW v CASCADE"), SqlError);
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
  // Any other literal is a Literal, as written, for the binder to fold or
  // refuse.
  auto literal_of = [](const Expr& e) {
    REQUIRE(e.kind == Expr::Kind::Literal);
    return e.literal;
  };
  auto number = literal_of(*std::get<Update>(parse("UPDATE t SET qty = qty + 1.5 WHERE id = $1")).set[0].value.rhs);
  CHECK((number.kind == Literal::Kind::Number && number.text == "1.5"));
  auto wide = literal_of(*std::get<Update>(parse("UPDATE t SET qty = qty + 99999999999999999999 WHERE id = $1")).set[0].value.rhs);
  CHECK((wide.kind == Literal::Kind::Integer && wide.text == "99999999999999999999"));
  CHECK(literal_of(std::get<Select>(parse("SELECT id FROM t WHERE id = 1")).where[0].rhs).text == "1");
  auto string = literal_of(std::get<Insert>(parse("INSERT INTO t VALUES ('it''s', NULL, TRUE)")).values[0]);
  CHECK((string.kind == Literal::Kind::String && string.text == "it's"));
  // A SELECT without FROM; PostgreSQL's grammar folds - of a number into the
  // literal, and leaves unary + an operator.
  auto bare = std::get<Select>(parse("SELECT -5, +7.8, -(1 + 2)"));
  CHECK(bare.table.empty());
  REQUIRE(bare.targets.size() == 3);
  CHECK(literal_of(bare.targets[0]).text == "-5");
  CHECK(bare.targets[1].kind == Expr::Kind::Plus);
  CHECK(bare.targets[2].kind == Expr::Kind::Neg);
  // OR, in ON as in WHERE, is a predicate conjunct; ON's are marked so.
  auto ors = std::get<Select>(parse("SELECT id FROM t JOIN u ON id = $1 OR k = $2"));
  REQUIRE(ors.where.size() == 1);
  CHECK(ors.where[0].kind == Compare::Kind::Or);
  CHECK(ors.where[0].on);
  CHECK(ors.where[0].args.size() == 2);
}

