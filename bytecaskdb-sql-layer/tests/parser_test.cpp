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
  CHECK(sel.targets[0].subquery->from[0].table == "u");
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
                   "CREATE TABLE u (a INT UNIQUE DEFERRABLE)"}) {
    INFO(sql);
    CHECK_THROWS_AS(parse(sql), SqlError);
  }
}

TEST_CASE("parser: CHECK on a column or the table keeps the text between its parentheses", "[sql][parser]") {
  auto t = std::get<CreateTable>(parse("CREATE TABLE \"c(\" (a INT CHECK (a > 0), b INT CONSTRAINT bc CHECK ((b <> 'x)(') AND b < 9), "
                                       "CONSTRAINT ab CHECK (a < b), CHECK (a IS NOT NULL))"));
  REQUIRE(t.checks.size() == 4);
  CHECK((t.checks[0].name.empty() && t.checks[0].column == "a" && t.checks[0].text == "a > 0"));
  CHECK((t.checks[1].name == "bc" && t.checks[1].column == "b" && t.checks[1].text == "(b <> 'x)(') AND b < 9"));
  CHECK((t.checks[2].name == "ab" && t.checks[2].column.empty() && t.checks[2].text == "a < b"));
  CHECK((t.checks[3].name.empty() && t.checks[3].column.empty() && t.checks[3].text == "a IS NOT NULL"));
  for (auto sql : {"CREATE TABLE u (a INT CHECK (a > 0) NO INHERIT)", "CREATE TABLE u (a INT, CHECK (a > 0) NOT VALID)"}) {
    INFO(sql);
    CHECK_THROWS_AS(parse(sql), SqlError);
  }
}

TEST_CASE("parser: forms outside the scope are rejected with a position", "[sql][parser]") {
  //                    0         1         2         3
  //                    0123456789012345678901234567890123456789
  CHECK(rejected_at("SELECT id FROM t FULL JOIN u ON t.id = u.id") == 27);
  CHECK(rejected_at("SELECT id FROM a, b, c, d, e, f, g, h, i WHERE id = $1") >= 0);  // nine tables
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
  CHECK(rejected_at("SELECT sum(DISTINCT qty + 1) FROM t WHERE id = $1") == 7);
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

TEST_CASE("parser: ORDER BY a position names the select-list column; a set operation's stays a position", "[sql][parser]") {
  auto s = std::get<Select>(parse("SELECT qty, id AS k FROM t ORDER BY 2 DESC, 1"));
  REQUIRE(s.order_by.size() == 2);
  CHECK(s.order_by[0].column == "id");
  CHECK(s.order_by[0].position == 0);
  CHECK(s.order_by[0].desc);
  CHECK(s.order_by[1].column == "qty");
  auto u = std::get<Select>(parse("SELECT a FROM t UNION SELECT b FROM u ORDER BY 1 DESC"));
  REQUIRE(u.order_by.size() == 1);
  CHECK(u.order_by[0].position == 1);
  CHECK(u.order_by[0].column.empty());
  auto sqlstate_of = [](const char* sql) {
    try {
      (void)parse(sql);
    } catch (const SqlError& e) {
      return std::string{e.sqlstate()};
    }
    return std::string{"parsed"};
  };
  CHECK(sqlstate_of("SELECT qty FROM t ORDER BY 2") == "42P10");
  CHECK(sqlstate_of("SELECT qty FROM t ORDER BY 0") == "42P10");
  CHECK(sqlstate_of("SELECT qty FROM t ORDER BY -1") == "42P10");
  CHECK(sqlstate_of("SELECT qty FROM t ORDER BY 'a'") == "42601");
  CHECK(sqlstate_of("SELECT qty FROM t ORDER BY 1.5") == "42601");
  CHECK(sqlstate_of("SELECT qty + 1 FROM t ORDER BY 1") == "42601");  // not a column: not yet
  CHECK(sqlstate_of("SELECT * FROM t ORDER BY 1") == "42601");        // a position after *: not yet
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
  CHECK(v.query.from[0].table == "t");
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
  CHECK(std::get<CreateView>(parse("CREATE VIEW v AS SELECT a FROM t UNION SELECT a FROM u")).query.set != nullptr);
  CHECK_THROWS_AS(parse("DROP VIEW a, b"), SqlError);
  CHECK_THROWS_AS(parse("DROP VIEW v CASCADE"), SqlError);
}

TEST_CASE("parser: UNION, INTERSECT and EXCEPT, their ALL and precedence", "[sql][parser][setop]") {
  using Kind = SetOperation::Kind;
  auto top = [](std::string_view sql) { return std::get<Select>(parse(sql)); };
  auto s = top("SELECT a FROM t UNION SELECT b FROM u");
  REQUIRE(s.set != nullptr);
  CHECK(s.set->kind == Kind::Union);
  CHECK_FALSE(s.set->all);
  CHECK(s.set->left.from[0].table == "t");
  CHECK(s.set->right.from[0].table == "u");
  CHECK(s.targets.empty());
  CHECK(top("SELECT a FROM t UNION ALL SELECT b FROM u").set->all);
  CHECK_FALSE(top("SELECT a FROM t UNION DISTINCT SELECT b FROM u").set->all);
  CHECK(top("SELECT a FROM t EXCEPT ALL SELECT b FROM u").set->kind == Kind::Except);
  CHECK(top("SELECT a FROM t INTERSECT SELECT b FROM u").set->kind == Kind::Intersect);

  // INTERSECT binds tighter; UNION and EXCEPT group to the left; parentheses group.
  auto p = top("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3");
  CHECK(p.set->kind == Kind::Union);
  CHECK(p.set->left.set == nullptr);
  REQUIRE(p.set->right.set != nullptr);
  CHECK(p.set->right.set->kind == Kind::Intersect);
  auto l = top("SELECT 1 EXCEPT SELECT 2 UNION SELECT 3");
  CHECK(l.set->kind == Kind::Union);
  REQUIRE(l.set->left.set != nullptr);
  CHECK(l.set->left.set->kind == Kind::Except);
  auto g = top("SELECT 1 UNION (SELECT 2 UNION SELECT 3)");
  CHECK(g.set->left.set == nullptr);
  CHECK(g.set->right.set != nullptr);

  // ORDER BY and LIMIT after the last arm are the combined rows'; a parenthesised arm keeps its own.
  auto o = top("SELECT a FROM t UNION SELECT b FROM u ORDER BY a LIMIT 3");
  CHECK(o.limit == 3);
  CHECK(o.order_by.size() == 1);
  CHECK_FALSE(o.set->right.limit.has_value());
  auto a = top("(SELECT a FROM t ORDER BY a LIMIT 1) UNION (SELECT b FROM u LIMIT 2)");
  CHECK_FALSE(a.limit.has_value());
  CHECK(a.set->left.limit == 1);
  CHECK(a.set->left.order_by.size() == 1);
  CHECK(a.set->right.limit == 2);

  // As a subquery.
  auto q = top("SELECT id FROM t WHERE EXISTS (SELECT a FROM t UNION ALL SELECT b FROM u)");
  REQUIRE(q.where.size() == 1);
  CHECK(q.where[0].subquery->set != nullptr);

  CHECK_THROWS_AS(parse("SELECT a FROM t UNION CORRESPONDING SELECT b FROM u"), SqlError);
  CHECK_THROWS_AS(parse("SELECT a FROM t UNION SELECT b FROM u FOR UPDATE"), SqlError);
  CHECK_THROWS_AS(parse("SELECT a FROM t UNION SELECT b FROM u OFFSET 1"), SqlError);
  CHECK_THROWS_AS(parse("WITH c AS (SELECT 1) SELECT a FROM t UNION SELECT b FROM u"), SqlError);
}

TEST_CASE("parser: SELECT … FOR UPDATE, and no other locking clause", "[sql][parser][txn]") {
  auto sel = std::get<Select>(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE"));
  CHECK(sel.for_update);
  CHECK_FALSE(std::get<Select>(parse("SELECT id FROM t WHERE id = $1")).for_update);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR SHARE"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR NO KEY UPDATE"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE NOWAIT"), SqlError);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE SKIP LOCKED"), SqlError);
  CHECK(std::get<Select>(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE OF t")).for_update);
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE OF u"), SqlError);  // not a FROM item
  CHECK_THROWS_AS(parse("SELECT id FROM t WHERE id = $1 FOR UPDATE FOR UPDATE"), SqlError);
}

TEST_CASE("parser: two tables, qualified columns and integer literals", "[sql][parser]") {
  auto j = std::get<Select>(parse("SELECT t.id, qty FROM t, u WHERE t.id = $1 AND k = $2"));
  REQUIRE(j.from.size() == 2);
  CHECK(j.from[0].table == "t");
  CHECK(j.from[1].table == "u");
  CHECK(j.targets[0].table == "t");
  CHECK(j.targets[1].table.empty());
  REQUIRE(j.where.size() == 2);
  CHECK(j.where[0].lhs.table == "t");
  // JOIN … ON: the ON conjuncts come first, then the WHERE's.
  auto on = std::get<Select>(parse("SELECT id FROM t JOIN u ON k = $1 WHERE id = $2"));
  CHECK(on.from[1].table == "u");
  REQUIRE(on.where.size() == 2);
  CHECK(on.where[0].lhs.column == "k");
  CHECK(on.where[0].on == 1);
  CHECK(on.where[1].on == 0);
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
  CHECK(bare.from.empty());
  REQUIRE(bare.targets.size() == 3);
  CHECK(literal_of(bare.targets[0]).text == "-5");
  CHECK(bare.targets[1].kind == Expr::Kind::Plus);
  CHECK(bare.targets[2].kind == Expr::Kind::Neg);
  // OR, in ON as in WHERE, is a predicate conjunct; ON's are marked so.
  auto ors = std::get<Select>(parse("SELECT id FROM t JOIN u ON id = $1 OR k = $2"));
  REQUIRE(ors.where.size() == 1);
  CHECK(ors.where[0].kind == Compare::Kind::Or);
  CHECK(ors.where[0].on == 1);
  CHECK(ors.where[0].args.size() == 2);
}

TEST_CASE("parser: a chain of joins, each with its own ON, kind and USING", "[sql][parser][join]") {
  auto q = std::get<Select>(
      parse("SELECT t.id FROM t JOIN u ON t.k = u.k LEFT JOIN v ON u.k < v.k AND v.k <> $1 JOIN w USING (k) WHERE t.id = $2"));
  REQUIRE(q.from.size() == 4);
  CHECK(q.from[1].kind == JoinKind::Inner);
  CHECK(q.from[2].kind == JoinKind::Left);
  CHECK(q.from[3].using_columns == std::vector<std::string>{"k"});
  // The ON conjuncts of each join come first, in order, marked with the join; the WHERE's last, marked 0.
  REQUIRE(q.where.size() == 4);
  CHECK(q.where[0].on == 1);
  CHECK(q.where[1].on == 2);
  CHECK(q.where[2].on == 2);
  CHECK(q.where[3].on == 0);
  // A comma list is joins with no ON.
  auto list = std::get<Select>(parse("SELECT 1 FROM a, b, c"));
  CHECK(list.from.size() == 3);
  CHECK(list.where.empty());
  // CROSS JOIN is an inner join with no condition.
  CHECK(std::get<Select>(parse("SELECT 1 FROM a CROSS JOIN b")).from.size() == 2);
  // At most 8 tables; a join on the right of a join, a join among commas, FULL and an alias of a join are refused.
  CHECK(std::get<Select>(parse("SELECT 1 FROM a, b, c, d, e, f, g, h")).from.size() == 8);
  CHECK_THROWS_AS(parse("SELECT 1 FROM a, b, c, d, e, f, g, h, i"), SqlError);
  CHECK_THROWS_AS(parse("SELECT 1 FROM a JOIN (b JOIN c ON b.k = c.k) ON a.k = b.k"), SqlError);
  CHECK_THROWS_AS(parse("SELECT 1 FROM a, b JOIN c ON b.k = c.k"), SqlError);
  CHECK_THROWS_AS(parse("SELECT 1 FROM a FULL JOIN b ON a.k = b.k"), SqlError);
  CHECK_THROWS_AS(parse("SELECT 1 FROM (a JOIN b ON a.k = b.k) AS j"), SqlError);
}

TEST_CASE("parser: a column list on a table alias is kept with the alias", "[sql][parser][join]") {
  auto s = std::get<Select>(parse("SELECT m.x, y FROM t AS m (x, y)"));
  CHECK(s.from[0].name() == "m");
  CHECK(s.from[0].column_aliases == std::vector<std::string>{"x", "y"});
  CHECK(s.targets[0].table == "m");
}

TEST_CASE("parser: an alias is rewritten to its table, and kept where a table is named twice", "[sql][parser][join]") {
  // Once: the qualifier names the table, and the item has no alias.
  auto once = std::get<Select>(parse("SELECT x.id FROM t AS x JOIN u y ON x.k = y.k"));
  CHECK(once.targets[0].table == "t");
  CHECK(once.from[0].alias.empty());
  CHECK(once.where[0].rhs.table == "u");
  // Twice: the aliases are what tell the two apart.
  auto twice = std::get<Select>(parse("SELECT x.id, y.id FROM t x JOIN t y ON x.k = y.k"));
  CHECK(twice.targets[0].table == "x");
  CHECK(twice.targets[1].table == "y");
  CHECK(twice.from[0].name() == "x");
  CHECK(twice.from[1].name() == "y");
  CHECK(twice.where[0].rhs.table == "y");
  // The table's own name is visible where an item is not aliased.
  auto mixed = std::get<Select>(parse("SELECT t.id, y.id FROM t JOIN t y ON t.k = y.k"));
  CHECK(mixed.targets[0].table == "t");
  CHECK(mixed.from[1].name() == "y");
  // Two items of one name, or a name an alias hides, are errors (42712, 42P01).
  CHECK_THROWS_AS(parse("SELECT 1 FROM t, t"), SqlError);
  CHECK_THROWS_AS(parse("SELECT 1 FROM t x, u x"), SqlError);
  CHECK_THROWS_AS(parse("SELECT t.id FROM t x JOIN t y ON x.k = y.k"), SqlError);
}

TEST_CASE("parser: DECLARE, FETCH, MOVE and CLOSE", "[sql][parser][cursor]") {
  auto declare = [](std::string_view sql) { return std::get<DeclareCursor>(*parse_cursor(sql)); };
  auto fetch = [](std::string_view sql) { return std::get<FetchCursor>(*parse_cursor(sql)); };

  SECTION("DECLARE: the options and the query") {
    auto d = declare("DECLARE c CURSOR FOR SELECT 1");
    CHECK(d.name == "c");
    CHECK(d.scroll == DeclareCursor::Scroll::Default);
    CHECK_FALSE(d.hold);
    CHECK_FALSE(d.binary);
    CHECK_FALSE(d.for_update);
    // The statement's own length, so a position into the query is one into the statement.
    CHECK(d.query == std::string(21, ' ') + "SELECT 1");
    d = declare("DECLARE \"C d\" BINARY INSENSITIVE NO SCROLL CURSOR WITH HOLD FOR SELECT a FROM t FOR UPDATE OF t");
    CHECK(d.name == "C d");
    CHECK(d.scroll == DeclareCursor::Scroll::NoScroll);
    CHECK(d.binary);
    CHECK(d.hold);
    CHECK(d.for_update);
    CHECK(declare("DECLARE c SCROLL CURSOR WITHOUT HOLD FOR SELECT 1").scroll == DeclareCursor::Scroll::Scroll);
    CHECK_FALSE(declare("DECLARE c SCROLL CURSOR WITHOUT HOLD FOR SELECT 1").hold);
    // CURSOR is also a name.
    CHECK(declare("DECLARE cursor CURSOR FOR SELECT 1").name == "cursor");
    CHECK(declare("DECLARE c CURSOR FOR SELECT a FROM t FOR READ ONLY").query.ends_with("FOR READ ONLY"));
  }

  SECTION("FETCH and MOVE: direction and count as PostgreSQL's tree has them") {
    using D = FetchCursor::Direction;
    auto f = fetch("FETCH c");
    CHECK(f.name == "c");
    CHECK(f.direction == D::Forward);
    CHECK(f.count == 1);
    CHECK_FALSE(f.move);
    CHECK(fetch("FETCH NEXT FROM c").count == 1);
    CHECK(fetch("FETCH 4 IN c").count == 4);
    CHECK(fetch("FETCH FORWARD 3 c").count == 3);
    CHECK(fetch("FETCH FORWARD c").count == 1);
    CHECK(fetch("FETCH ALL c").count == FetchCursor::kAll);
    CHECK(fetch("FETCH FORWARD ALL FROM c").count == FetchCursor::kAll);
    CHECK(fetch("FETCH 0 c").count == 0);
    CHECK(fetch("FETCH -2 c").count == -2);  // a negative count, Forward: PostgreSQL reads it as backward
    CHECK(fetch("FETCH PRIOR c").direction == D::Backward);
    CHECK(fetch("FETCH BACKWARD 2 c").count == 2);
    CHECK(fetch("FETCH ABSOLUTE 3 c").direction == D::Absolute);
    CHECK(fetch("FETCH LAST c").count == -1);  // LAST is ABSOLUTE -1
    CHECK(fetch("FETCH RELATIVE 2 c").direction == D::Relative);
    auto m = fetch("MOVE FORWARD 2 IN c");
    CHECK(m.move);
    CHECK(m.count == 2);
  }

  SECTION("CLOSE") {
    auto c = std::get<CloseCursor>(*parse_cursor("CLOSE c"));
    CHECK(c.name == "c");
    CHECK_FALSE(c.all);
    CHECK(std::get<CloseCursor>(*parse_cursor("CLOSE ALL")).all);
  }

  SECTION("anything else is parse()'s, and a syntax error is an error") {
    CHECK_FALSE(parse_cursor("SELECT 1").has_value());
    CHECK_FALSE(parse_cursor("BEGIN").has_value());
    CHECK_THROWS_AS(parse_cursor("FETCH"), SqlError);
    CHECK_THROWS_AS(parse_cursor("OPEN c"), SqlError);  // not PostgreSQL's
    CHECK_THROWS_AS(parse_cursor("FETCH c INTO t"), SqlError);
  }

  SECTION("DELETE FROM ONLY ( t ) is the tree of DELETE FROM t") {
    CHECK(std::get<Delete>(parse("DELETE FROM ONLY ( t )")).table == "t");
  }
}

TEST_CASE("parser: UPDATE and DELETE ... WHERE CURRENT OF name the cursor and have no other WHERE", "[sql][parser][cursor]") {
  auto u = std::get<Update>(parse("UPDATE t SET v = $1 WHERE CURRENT OF \"My C\""));
  CHECK(u.current_of == "My C");
  CHECK(u.where.empty());
  CHECK(u.set.size() == 1);
  auto d = std::get<Delete>(parse("DELETE FROM ONLY t WHERE CURRENT OF c"));
  CHECK(d.current_of == "c");
  CHECK(d.where.empty());
  CHECK(std::get<Update>(parse("UPDATE t SET v = 1 WHERE id = 1")).current_of.empty());
  // PostgreSQL's grammar takes it as the whole clause; so does the dialect's.
  CHECK_THROWS_AS(parse("UPDATE t SET v = 1 WHERE id = 1 AND CURRENT OF c"), SqlError);
  CHECK_THROWS_AS(parse("DELETE FROM t WHERE CURRENT OF $1"), SqlError);
  CHECK_THROWS_AS(parse("SELECT 1 FROM t WHERE CURRENT OF c"), SqlError);
}
