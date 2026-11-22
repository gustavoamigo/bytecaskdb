// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb stage 1 through the embedded API (docs/jsonb_design.md, *Stages of
// jsonb*): the column type and its text in and out, literals read at
// prepare with jsonb_in's SQLSTATE, DETAIL and CONTEXT, parameters typed
// jsonb, DEFAULT and ADD COLUMN ... DEFAULT across a reopen, casts with
// text, CASE and COALESCE, IS NULL and COUNT, and every refusal the stage
// keeps with its SQLSTATE.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.api;
import bytecask.sql.tests.util;

using namespace bytecask::sql;
using sql_test::bytes;

namespace {

using Grid = std::vector<std::vector<Value>>;
const Value kNull = std::monostate{};

auto i(std::int64_t v) -> Value { return v; }
auto t(std::string_view s) -> Value { return bytes(s); }

// The SQLSTATE a statement fails with, "" if it runs.
auto state_of(Database& db, std::string_view sql, const std::vector<Value>& params = {}) -> std::string {
  try {
    (void)db.execute(sql, params);
  } catch (const std::exception& e) {
    return std::string{sqlstate(e)};
  }
  return "";
}

}  // namespace

TEST_CASE("embedded API jsonb: a column holds jsonb_in's value and returns jsonb_out's text, across a reopen",
          "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb"};
  {
    auto db = Database::open(dir.path());
    db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb DEFAULT '{\"d\": [1, 2.50]}'::jsonb, s TEXT)");
    const auto& insert = db->prepare("INSERT INTO j (id, v) VALUES ($1, $2)");
    CHECK(insert.param_types() == std::vector<PhysType>{PhysType::I64, PhysType::Jsonb});
    CHECK(db->execute(insert, {i(1), t("{\"b\":1,  \"a\":{\"x\":1}, \"a\":[2]}")}).affected == 1);
    CHECK(db->execute(insert, {i(2), kNull}).affected == 1);
    CHECK(db->execute("INSERT INTO j VALUES (3, 'null', '[1, \"x\"]'), (4, DEFAULT, '{}')").affected == 2);
    CHECK(db->execute("INSERT INTO j (id) VALUES (5)").affected == 1);
    auto r = db->execute("SELECT id, v FROM j ORDER BY id");
    REQUIRE(r.types.size() == 2);
    CHECK(r.types[1].kind == SqlKind::Jsonb);
    CHECK(r.rows == Grid{{i(1), t("{\"a\": [2], \"b\": 1}")},
                         {i(2), kNull},
                         {i(3), t("null")},
                         {i(4), t("{\"d\": [1, 2.50]}")},
                         {i(5), t("{\"d\": [1, 2.50]}")}});
    // Casts with text, IS NULL, COUNT, CASE and COALESCE.
    CHECK(db->execute("SELECT v::text FROM j WHERE id = 1").rows == Grid{{t("{\"a\": [2], \"b\": 1}")}});
    CHECK(db->execute("SELECT s::jsonb FROM j WHERE id = 3").rows == Grid{{t("[1, \"x\"]")}});
    CHECK(db->execute("SELECT '{\"z\": 1, \"a\": 2}'::jsonb::text").rows == Grid{{t("{\"a\": 2, \"z\": 1}")}});
    CHECK(db->execute("SELECT count(v), count(*) FROM j").rows == Grid{{i(4), i(5)}});
    CHECK(db->execute("SELECT id FROM j WHERE v IS NULL").rows == Grid{{i(2)}});
    CHECK(db->execute("SELECT COALESCE(v, '[]') FROM j WHERE id = 2").rows == Grid{{t("[]")}});
    CHECK(db->execute("SELECT CASE WHEN id = 3 THEN v ELSE '{}'::jsonb END FROM j WHERE id IN (1, 3) ORDER BY id").rows ==
          Grid{{t("{}")}, {t("null")}});
    CHECK(db->execute("SELECT $1::jsonb", {t("[1.0, -0]")}).rows == Grid{{t("[1.0, 0]")}});
    CHECK(db->execute("UPDATE j SET v = $1 WHERE id = $2", {t("[true]"), i(2)}).affected == 1);
    CHECK(db->execute("ALTER TABLE j ADD COLUMN k jsonb NOT NULL DEFAULT '[]'").affected == 0);
    db->close();
  }
  auto db = Database::open(dir.path());
  CHECK(db->execute("SELECT id, v, k FROM j WHERE id <= 2 ORDER BY id").rows ==
        Grid{{i(1), t("{\"a\": [2], \"b\": 1}"), t("[]")}, {i(2), t("[true]"), t("[]")}});
  CHECK(db->execute("INSERT INTO j (id) VALUES (6)").affected == 1);
  CHECK(db->execute("SELECT v, k FROM j WHERE id = 6").rows == Grid{{t("{\"d\": [1, 2.50]}"), t("[]")}});
  CHECK(db->execute("CREATE TABLE c AS SELECT id, v FROM j WHERE id = 1").affected == 1);
  CHECK(db->execute("SELECT v FROM c").rows == Grid{{t("{\"a\": [2], \"b\": 1}")}});
}

TEST_CASE("embedded API jsonb: a literal is read at prepare, with jsonb_in's SQLSTATE, DETAIL and CONTEXT",
          "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb)");
  try {
    (void)db->prepare("INSERT INTO j VALUES (1, '{\"a\":1 \"b\":2}')");
    FAIL("a literal that is no JSON must not prepare");
  } catch (const ast::SqlError& e) {
    CHECK(e.sqlstate() == "22P02");
    CHECK(std::string_view{e.what()} == "invalid input syntax for type json");
    CHECK(e.detail() == "Expected \",\" or \"}\", but found \"\"b\"\".");
    CHECK(error_context(e) == "JSON data, line 1: {\"a\":1 \"b\"...");
  }
  CHECK(state_of(*db, "SELECT '\"\\u0000\"'::jsonb") == "22P05");
  CHECK(state_of(*db, "SELECT '1e131072'::jsonb") == "22003");
  // A parameter's text: at execution, the same error with its lines.
  try {
    (void)db->execute("INSERT INTO j VALUES ($1, $2)", {i(1), t("[1, 2")});
    FAIL("a parameter that is no JSON must fail");
  } catch (const ExecError& e) {
    CHECK(sqlstate(e) == "22P02");
    CHECK(e.note() == "The input string ended unexpectedly.");
    CHECK(e.context() == "JSON data, line 1: [1, 2");
  }
  CHECK(db->execute("SELECT id FROM j").rows.empty());
}

TEST_CASE("embedded API jsonb: what stage 1 refuses, each with PostgreSQL's SQLSTATE or 0A000", "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb, s TEXT, n INTEGER)");
  struct Refusal {
    std::string_view sql;
    std::string_view state;
  };
  for (const auto& r : {
           // Comparison, order, groups and duplicates: stage 2.
           Refusal{"SELECT id FROM j WHERE v = '{}'", "0A000"},
           Refusal{"SELECT id FROM j WHERE v = v", "0A000"},
           Refusal{"SELECT v = v FROM j", "0A000"},
           Refusal{"SELECT id FROM j WHERE v = $1", "0A000"},
           Refusal{"SELECT id FROM j WHERE v IN ('1', '2')", "0A000"},
           Refusal{"SELECT v FROM j ORDER BY v", "0A000"},
           Refusal{"SELECT v FROM j GROUP BY v", "0A000"},
           Refusal{"SELECT DISTINCT v FROM j", "0A000"},
           Refusal{"SELECT count(DISTINCT v) FROM j", "0A000"},
           Refusal{"SELECT v FROM j UNION SELECT v FROM j", "0A000"},
           Refusal{"SELECT (SELECT v FROM j LIMIT 1)", "0A000"},
           // The same of constants, an unknown literal beside a jsonb read by
           // jsonb_in first.
           Refusal{"SELECT '1'::jsonb = '1'::jsonb", "0A000"},
           Refusal{"SELECT '1'::jsonb <> '1'", "0A000"},
           Refusal{"SELECT ('1'::jsonb) < ('2'::jsonb)", "0A000"},
           Refusal{"SELECT nullif('1'::jsonb, '1')", "0A000"},
           Refusal{"SELECT CASE '1'::jsonb WHEN '1' THEN 1 END", "0A000"},
           Refusal{"SELECT '1'::jsonb = 'x'", "22P02"},
           Refusal{"SELECT nullif('1'::jsonb, 'x')", "22P02"},
           // Keys: the next phase.
           Refusal{"CREATE TABLE k1 (v jsonb PRIMARY KEY)", "0A000"},
           Refusal{"CREATE TABLE k2 (id INTEGER PRIMARY KEY, v jsonb UNIQUE)", "0A000"},
           Refusal{"CREATE INDEX j_v ON j (v)", "0A000"},
           // Operators and functions: stage 4; json: stage 1c; a cast to a
           // DECIMAL(p, s).
           Refusal{"SELECT v::numeric(10,2) FROM j", "0A000"},
           Refusal{"SELECT '1'::jsonb::numeric(10,2)", "0A000"},
           Refusal{"SELECT v - 'a' FROM j", "0A000"},
           Refusal{"SELECT v || '[1]' FROM j", "0A000"},
           Refusal{"SELECT '[1]'::jsonb || '[2]'", "0A000"},
           Refusal{"SELECT '[1]' || '{}'::jsonb", "0A000"},
           Refusal{"SELECT '1'::jsonb || '2'::jsonb", "0A000"},
           Refusal{"SELECT coalesce('[1]'::jsonb || '2', '0')", "0A000"},
           Refusal{"SELECT '\"a\"'::jsonb || 'b'", "22P02"},
           Refusal{"SELECT '[1,2]'::jsonb - 1", "0A000"},
           Refusal{"SELECT '[1,2]'::jsonb - 1::smallint", "0A000"},
           Refusal{"SELECT '1'::jsonb - 'a'", "0A000"},
           Refusal{"SELECT '1'::jsonb - 'a'::text", "0A000"},
           Refusal{"SELECT id FROM j WHERE v @> '{}'", "0A000"},
           Refusal{"SELECT id FROM j WHERE v ? 'a'", "0A000"},
           Refusal{"SELECT to_jsonb(1)", "0A000"},
           Refusal{"UPDATE j SET s = v", "0A000"},
           Refusal{"SELECT v::varchar(3) FROM j", "0A000"},
           Refusal{"CREATE TABLE k3 (v json)", "0A000"},
           Refusal{"SELECT '1'::json", "0A000"},
           Refusal{"CREATE DOMAIN d AS jsonb", "0A000"},
           Refusal{"CREATE PROCEDURE p(x jsonb) LANGUAGE plpgsql AS $$ BEGIN END $$", "0A000"},
           Refusal{"INSERT INTO j (id, s) SELECT 1, v FROM j", "0A000"},
           // PostgreSQL's own refusals.
           Refusal{"SELECT v + 1 FROM j", "42883"},
           // The accessors' resolution: stage 3 (docs/jsonb_design.md).
           Refusal{"SELECT v -> 1::bigint FROM j", "42883"},
           Refusal{"SELECT v -> 1.5 FROM j", "42883"},
           Refusal{"SELECT v -> true FROM j", "42883"},
           Refusal{"SELECT s -> 'a' FROM j", "42883"},
           Refusal{"SELECT v #> 'a'::text FROM j", "42883"},
           Refusal{"SELECT v #> ARRAY[1] FROM j", "42883"},
           Refusal{"SELECT $1 -> 'a'", "42725"},
           Refusal{"SELECT '{}' -> 'a'", "42725"},
           Refusal{"SELECT '[1]' -> 0", "42725"},
           Refusal{"SELECT jsonb_typeof(n) FROM j", "42883"},
           Refusal{"SELECT jsonb_typeof(1)", "42883"},
           Refusal{"SELECT v #> ARRAY[] FROM j", "42P18"},
           Refusal{"SELECT v #> 'a' FROM j", "22P02"},
           Refusal{"SELECT '[1]'::jsonb #> '{{a},{b,c}}'", "22P02"},
           Refusal{"SELECT jsonb_array_length('{}')", "22023"},
           Refusal{"SELECT '\"x\"'::jsonb::integer", "22023"},
           Refusal{"SELECT '1e10'::jsonb::integer", "22003"},
           Refusal{"SELECT max(v) FROM j", "42883"},
           Refusal{"SELECT sum(v) FROM j", "42883"},
           Refusal{"SELECT id FROM j WHERE v = 1", "42883"},
           Refusal{"SELECT '1'::jsonb = 1", "42883"},
           Refusal{"SELECT '1'::jsonb = 'a'::text", "42883"},
           Refusal{"SELECT '1'::jsonb - '1'::jsonb", "42883"},
           Refusal{"SELECT '[1]'::jsonb - 1::bigint", "42883"},
           Refusal{"SELECT '[1]'::jsonb - 1.0", "42883"},
           Refusal{"SELECT '[1]'::jsonb + 1", "42883"},
           Refusal{"SELECT '[1]'::jsonb * '1'", "42883"},
           Refusal{"SELECT '[1]'::jsonb % 1", "42883"},
           Refusal{"SELECT 'x' - '1'::jsonb", "42883"},
           Refusal{"SELECT 1 - '1'::jsonb", "42883"},
           Refusal{"SELECT '[1]'::jsonb || 1", "42883"},
           Refusal{"SELECT 1::jsonb", "42846"},
           Refusal{"SELECT n::jsonb FROM j", "42846"},
           Refusal{"SELECT '1'::jsonb::bytea", "42846"},
           Refusal{"UPDATE j SET v = s", "42804"},
           Refusal{"INSERT INTO j (id, v) VALUES (1, 'x'::text)", "42804"},
           Refusal{"INSERT INTO j (id, v) SELECT 1, s FROM j", "42804"},
           Refusal{"CREATE TABLE k4 (v jsonb DEFAULT 1)", "42804"},
           Refusal{"SELECT CASE WHEN true THEN v ELSE 1 END FROM j", "42804"},
           Refusal{"SELECT id FROM j WHERE v = 'x'", "22P02"},
       }) {
    INFO(r.sql);
    CHECK(state_of(*db, r.sql) == r.state);
  }
}

TEST_CASE("embedded API jsonb: an assignment refusal reads as PostgreSQL's, with its HINT", "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb_assign"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb, s TEXT, c VARCHAR(1))");
  db->execute("INSERT INTO j (id, v, s) VALUES (1, '[1]', 'x')");
  auto error_of = [&](std::string_view sql) -> std::pair<std::string, std::string> {
    try {
      (void)db->execute(sql);
    } catch (const std::exception& e) {
      return {e.what(), std::string{error_hint(e)}};
    }
    return {};
  };
  constexpr std::string_view kHint = "You will need to rewrite or cast the expression.";
  // PostgreSQL's 42804, its HINT included, for every way a value meets a jsonb column.
  for (auto sql : {"INSERT INTO j (id, v) VALUES (2, 'x'::text)", "UPDATE j SET v = s",
                   "INSERT INTO j (id, v) SELECT 2, s FROM j"}) {
    INFO(sql);
    auto [message, hint] = error_of(sql);
    CHECK(message.find("but expression is of type") != std::string::npos);
    CHECK(hint == kHint);
  }
  // A jsonb column's value into a string column: not built (0A000).
  CHECK(error_of("UPDATE j SET s = v").first == "assigning a jsonb value to a column of type text is not supported yet");
  // A jsonb constant into a string column is its text, length-checked as PostgreSQL's assignment cast is.
  db->execute("INSERT INTO j (id, s) VALUES (3, '{\"a\": 1}'::jsonb)");
  CHECK(db->execute("SELECT s FROM j WHERE id = 3").rows == Grid{{t("{\"a\": 1}")}});
  CHECK(state_of(*db, "INSERT INTO j (id, c) VALUES (4, '[1]'::jsonb)") == "22001");
  // jsonb || a typed text is text concatenation, as PostgreSQL's anynonarray || text.
  CHECK(db->execute("SELECT '[1]'::jsonb || 'a'::text").rows == Grid{{t("[1]a")}});
}

TEST_CASE("embedded API jsonb: the accessors and casts out of columns, parameters and constants (stage 3)", "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb_accessors"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb, s TEXT, n INTEGER)");
  db->execute(R"(INSERT INTO j VALUES (1, '{"a": {"b": [1, "x"]}, "k": "v"}', 'k', 1), (2, '[10, 20.5]', 'a', -1), (3, NULL, 'k', 0))");
  CHECK(db->execute("SELECT id, v -> 'a' ->> 'b', v ->> s, v -> n FROM j ORDER BY id").rows ==
        Grid{{i(1), t("[1, \"x\"]"), t("v"), kNull}, {i(2), kNull, kNull, t("20.5")}, {i(3), kNull, kNull, kNull}});
  // In WHERE, an evaluated filter; $n typed text, its path text[].
  const auto& q = db->prepare("SELECT id FROM j WHERE v ->> 'k' = $1 OR v #>> $2 = '1'");
  CHECK(q.param_types() == std::vector<PhysType>{PhysType::Bytes, PhysType::Bytes});
  CHECK(q.params()[1].path);
  CHECK_FALSE(q.params()[0].path);
  CHECK(db->execute(q, {t("v"), t("{a,b,0}")}).rows == Grid{{i(1)}});
  CHECK(db->execute(q, {t("w"), t("{0}")}).rows.empty());
  CHECK(db->execute("SELECT id FROM j WHERE v #> ARRAY['a', $1] IS NOT NULL", {t("b")}).rows == Grid{{i(1)}});
  CHECK(db->execute("SELECT jsonb_typeof(v), jsonb_array_length(v -> 'a' -> 'b') FROM j WHERE id = 1").rows ==
        Grid{{t("object"), i(2)}});
  CHECK(db->execute("SELECT (v -> 1)::integer, (v -> 0)::bigint FROM j WHERE id = 2").rows == Grid{{i(21), i(10)}});
  CHECK(db->execute("SELECT sum((v -> 0)::integer) FROM j").rows == Grid{{i(10)}});
  CHECK(db->execute("SELECT id FROM j ORDER BY v ->> 'k', id").rows == Grid{{i(1)}, {i(2)}, {i(3)}});
  // Constants are folded, as PostgreSQL folds them.
  CHECK(db->execute(R"(SELECT '{"a":[1,2]}'::jsonb #>> '{a,-1}', '5'::jsonb -> 0, '1.5'::jsonb::integer)").rows ==
        Grid{{t("2"), t("5"), i(2)}});
  CHECK(state_of(*db, "SELECT (v -> 'a')::integer FROM j") == "22023");
  CHECK(state_of(*db, "SELECT v #> $1 FROM j", {t("a")}) == "22P02");
}
