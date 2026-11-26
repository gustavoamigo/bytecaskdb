// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb stage 1 through the embedded API (docs/jsonb_design.md, *Stages of
// jsonb*): the column type and its text in and out, literals read at
// prepare with jsonb_in's SQLSTATE, DETAIL and CONTEXT, parameters typed
// jsonb, DEFAULT and ADD COLUMN ... DEFAULT across a reopen, casts with
// text, CASE and COALESCE, IS NULL and COUNT, and every refusal the stage
// keeps with its SQLSTATE; and the later stages' operators, comparison and
// order (stage 2) among them.

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

TEST_CASE("embedded API jsonb: what the stages refuse, each with PostgreSQL's SQLSTATE or 0A000", "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb, s TEXT, n INTEGER)");
  struct Refusal {
    std::string_view sql;
    std::string_view state;
  };
  for (const auto& r : {
           // A literal beside a jsonb is read by jsonb_in first.
           Refusal{"SELECT '1'::jsonb = 'x'", "22P02"},
           Refusal{"SELECT nullif('1'::jsonb, 'x')", "22P02"},
           // Keys: the next phase.
           Refusal{"CREATE TABLE k1 (v jsonb PRIMARY KEY)", "0A000"},
           Refusal{"CREATE TABLE k2 (id INTEGER PRIMARY KEY, v jsonb UNIQUE)", "0A000"},
           Refusal{"CREATE INDEX j_v ON j (v)", "0A000"},
           // jsonpath and SQL/JSON, json_build_*: json is stage 1c; a cast to
           // a DECIMAL(p, s).
           Refusal{"SELECT v::numeric(10,2) FROM j", "0A000"},
           Refusal{"SELECT '1'::jsonb::numeric(10,2)", "0A000"},
           Refusal{"SELECT '\"a\"'::jsonb || 'b'", "22P02"},
           Refusal{"SELECT id FROM j WHERE v @? '$.a'", "0A000"},
           Refusal{"SELECT id FROM j WHERE v @@ '$.a == 1'", "0A000"},
           Refusal{"SELECT json_build_array(1)", "0A000"},
           Refusal{"SELECT jsonb_path_query(v, '$') FROM j", "0A000"},
           // Stage 4's own errors, as PostgreSQL's (docs/jsonb_design.md).
           Refusal{"SELECT '1'::jsonb - 'a'", "22023"},
           Refusal{"SELECT '1'::jsonb - 'a'::text", "22023"},
           Refusal{"SELECT '{}' @> '{}'", "42725"},
           Refusal{"SELECT '{}' ?| '{a}'", "42725"},
           Refusal{"SELECT v @> 1 FROM j", "42883"},
           Refusal{"SELECT v ? 1 FROM j", "42883"},
           Refusal{"SELECT v ?& 'a'::text FROM j", "42883"},
           Refusal{"SELECT jsonb_set(v, '{a}', 1) FROM j", "42883"},
           Refusal{"SELECT to_jsonb('a')", "42804"},
           Refusal{"SELECT to_jsonb($1)", "42804"},
           Refusal{"SELECT jsonb_build_array($1)", "42P18"},
           Refusal{"SELECT jsonb_build_object(1)", "22023"},
           Refusal{"SELECT jsonb_set('{}', '{{a},{b}}', '1')", "2202E"},
           Refusal{"SELECT jsonb_set('{\"a\":1}', '{a,NULL}', '1')", "22004"},
           Refusal{"SELECT jsonb_insert('{\"a\":1}', '{a}', '1')", "22023"},
           Refusal{"SELECT v::varchar(3) FROM j", "42601"},
           Refusal{"SELECT jsonb_set_lax(v, '{a}', NULL, null_value_treatment => 'delete_key') FROM j", "42601"},
           Refusal{"SELECT jsonb_build_array(VARIADIC ARRAY[1,2])", "42601"},
           Refusal{"CREATE TABLE k3 (v json)", "0A000"},
           Refusal{"SELECT '1'::json", "0A000"},
           Refusal{"CREATE DOMAIN d AS jsonb", "0A000"},
           Refusal{"CREATE PROCEDURE p(x jsonb) LANGUAGE plpgsql AS $$ BEGIN END $$", "0A000"},
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
  // A jsonb column's value into a string column is its text, PostgreSQL's
  // I/O assignment cast, length-checked against a VARCHAR(n) (#904 S1).
  CHECK(error_of("UPDATE j SET s = v WHERE id = 1").first.empty());
  CHECK(db->execute("SELECT s FROM j WHERE id = 1").rows == Grid{{t("[1]")}});
  CHECK(error_of("UPDATE j SET c = v WHERE id = 1").first.starts_with("value too long for type character varying(1)"));
  // A jsonb constant into a string column is its text, length-checked as PostgreSQL's assignment cast is.
  db->execute("INSERT INTO j (id, s) VALUES (3, '{\"a\": 1}'::jsonb)");
  CHECK(db->execute("SELECT s FROM j WHERE id = 3").rows == Grid{{t("{\"a\": 1}")}});
  CHECK(state_of(*db, "INSERT INTO j (id, c) VALUES (4, '[1]'::jsonb)") == "22001");
  // jsonb || a typed text is text concatenation, as PostgreSQL's anynonarray || text.
  CHECK(db->execute("SELECT '[1]'::jsonb || 'a'::text").rows == Grid{{t("[1]a")}});
}

TEST_CASE("embedded API jsonb: comparison, order, groups and set operations by PostgreSQL 17's jsonb_ops (stage 2)",
          "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb_compare"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb, w jsonb)");
  db->execute(R"(INSERT INTO j VALUES (1, '{"b": 1, "c": 1}', '1'), (2, '[]', '1.0'), (3, 'true', NULL), (4, '"a"', '"a"'),
                 (5, '1.00', '1'), (6, 'null', '[1]'), (7, '[1, 2]', '[1,2]'), (8, '{}', '{}'), (9, '-1', '-1.0'),
                 (10, '[null]', NULL), (11, NULL, '2'), (12, '1', '{"a": 1}'))");
  auto ids = [&](std::string_view sql, const std::vector<Value>& params = {}) {
    std::vector<std::int64_t> out;
    for (const auto& r : db->execute(sql, params).rows) out.push_back(std::get<std::int64_t>(r[0]));
    return out;
  };
  using Ids = std::vector<std::int64_t>;
  // The type order, the top-level empty array first, NULLs last; equal
  // values (1.00, 1) in their id order.
  CHECK(ids("SELECT id FROM j ORDER BY v, id") == Ids{2, 6, 4, 9, 5, 12, 3, 10, 7, 8, 1, 11});
  CHECK(ids("SELECT id FROM j ORDER BY v DESC, id") == Ids{11, 1, 8, 7, 10, 3, 5, 12, 9, 4, 6, 2});
  // = by value, columns, literals and parameters alike.
  CHECK(ids("SELECT id FROM j WHERE v = '1' ORDER BY id") == Ids{5, 12});
  CHECK(ids("SELECT id FROM j WHERE v = $1 ORDER BY id", {t("1.0")}) == Ids{5, 12});
  CHECK(ids("SELECT id FROM j WHERE v = w ORDER BY id") == Ids{4, 5, 7, 8, 9});
  CHECK(ids("SELECT id FROM j WHERE v < w ORDER BY id") == Ids{2, 6, 12});
  CHECK(ids("SELECT id FROM j WHERE v <> '1' AND v IN ('[]', '\"a\"', 'null') ORDER BY id") == Ids{2, 4, 6});
  CHECK(ids("SELECT id FROM j WHERE v BETWEEN '\"a\"' AND 'true' ORDER BY id") == Ids{3, 4, 5, 9, 12});
  CHECK(ids("SELECT id FROM j WHERE v -> 'b' = $1::jsonb", {t("1")}) == Ids{1});
  CHECK(ids("SELECT id FROM j WHERE v IN (SELECT w FROM j) ORDER BY id") == Ids{4, 5, 7, 8, 9, 12});
  CHECK(ids("SELECT id FROM j WHERE v > ALL (SELECT w FROM j WHERE w IS NOT NULL) ORDER BY id") == Ids{1});
  CHECK(ids("SELECT a.id FROM j a JOIN j b ON a.v = b.w WHERE b.id = 2 ORDER BY a.id") == Ids{5, 12});
  // A correlated subquery carries the outer row's jsonb into its own
  // comparison (bc_sub_arg_jsonb); m holds j's rows.
  db->execute("CREATE TABLE m (id INTEGER PRIMARY KEY, v jsonb, w jsonb)");
  db->execute("INSERT INTO m SELECT id, v, w FROM j");
  CHECK(ids("SELECT id FROM j WHERE EXISTS (SELECT 1 FROM m WHERE m.w = j.v) ORDER BY id") == Ids{4, 5, 7, 8, 9, 12});
  CHECK(ids("SELECT id FROM j WHERE NOT EXISTS (SELECT 1 FROM m WHERE m.w = j.v) ORDER BY id") == Ids{1, 2, 3, 6, 10, 11});
  CHECK(ids("SELECT id FROM j WHERE NOT EXISTS (SELECT 1 FROM m WHERE m.v < j.w) ORDER BY id") == Ids{3, 10});
  CHECK(db->execute("SELECT v = w, v < w, nullif(v, '1.0') FROM j WHERE id = 5").rows == Grid{{true, false, kNull}});
  // A group prints its first row's value, its scale kept; DISTINCT,
  // COUNT(DISTINCT) and UNION merge equal values.
  CHECK(db->execute("SELECT w, count(*) FROM j GROUP BY w HAVING count(*) > 1 ORDER BY w").rows ==
        Grid{{t("1"), i(3)}, {kNull, i(2)}});
  CHECK(db->execute("SELECT count(DISTINCT v), count(DISTINCT w), count(DISTINCT v -> 'b') FROM j").rows ==
        Grid{{i(10), i(8), i(1)}});
  CHECK(db->execute("SELECT DISTINCT w FROM j WHERE id < 6 ORDER BY w").rows == Grid{{t("\"a\"")}, {t("1")}, {kNull}});
  CHECK(db->execute("SELECT v FROM j WHERE id IN (5, 12) UNION SELECT w FROM j WHERE id = 1").rows == Grid{{t("1.00")}});
  CHECK(db->execute("SELECT (SELECT v FROM j WHERE id = 1), '[1,2]'::jsonb > '[3]', '{\"a\":1,\"b\":2}'::jsonb = '{\"b\":2,\"a\":1}'").rows ==
        Grid{{t("{\"b\": 1, \"c\": 1}"), true, true}});
  // Still refused: MIN and MAX (PostgreSQL's 42883), a jsonb key (the next
  // phase).
  CHECK(state_of(*db, "SELECT max(v) FROM j") == "42883");
  CHECK(state_of(*db, "SELECT id FROM j WHERE v = 'x'::text") == "42883");
  CHECK(state_of(*db, "CREATE INDEX j_v ON j (v)") == "0A000");
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

TEST_CASE("embedded API jsonb: containment, existence, modification and construction (stage 4)", "[sql][api][jsonb]") {
  sql_test::TempDir dir{"api_jsonb_stage4"};
  auto db = Database::open(dir.path());
  db->execute("CREATE TABLE j (id INTEGER PRIMARY KEY, v jsonb, s TEXT, n INTEGER)");
  db->execute(R"(INSERT INTO j VALUES (1, '{"a": 1, "b": [1, 2], "n": null}', 'a', 1), (2, '["a", 1, [2]]', 'b', 0), (3, NULL, NULL, NULL))");
  // In WHERE and ON, evaluated filters; $n typed jsonb, text and text[].
  const auto& q = db->prepare("SELECT id FROM j WHERE v @> $1 OR v ? $2 OR v ?& $3 ORDER BY id");
  CHECK(q.param_types() == std::vector<PhysType>{PhysType::Jsonb, PhysType::Bytes, PhysType::Bytes});
  CHECK(q.params()[2].path);
  CHECK(db->execute(q, {t(R"({"b": [2]})"), t("z"), t("{z}")}).rows == Grid{{i(1)}});
  CHECK(db->execute(q, {t("[[2]]"), t("a"), t("{}")}).rows == Grid{{i(1)}, {i(2)}});
  CHECK(db->execute("SELECT x.id FROM j x JOIN j y ON x.v <@ y.v WHERE x.id <> y.id").rows.empty());
  CHECK(db->execute("SELECT id FROM j WHERE NOT v ? s ORDER BY id").rows == Grid{{i(2)}});
  // Modification of columns: what is not changed is copied.
  CHECK(db->execute(R"(SELECT v || '{"c": 3}', v - 'a', v #- '{b,0}' FROM j WHERE id = 1)").rows ==
        Grid{{t(R"({"a": 1, "b": [1, 2], "c": 3, "n": null})"), t(R"({"b": [1, 2], "n": null})"), t(R"({"a": 1, "b": [2], "n": null})")}});
  CHECK(state_of(*db, "SELECT v - n FROM j WHERE id = 1") == "22023");
  CHECK(db->execute("SELECT v - n, v - s FROM j WHERE id = 2").rows == Grid{{t(R"([1, [2]])"), t(R"(["a", 1, [2]])")}});
  CHECK(db->execute("SELECT jsonb_set(v, '{b,-1}', to_jsonb(s)), jsonb_insert(v, '{b,0}', '0'), jsonb_strip_nulls(v) FROM j WHERE id = 1")
            .rows == Grid{{t(R"({"a": 1, "b": [1, "a"], "n": null})"), t(R"({"a": 1, "b": [0, 1, 2], "n": null})"),
                           t(R"({"a": 1, "b": [1, 2]})")}});
  // UPDATE … SET of a function of the row, as Mattermost writes it.
  db->execute("UPDATE j SET v = jsonb_set(v, $2, $3) WHERE id = $1", {i(1), t("{b,5}"), t("true")});
  CHECK(db->execute("SELECT v FROM j WHERE id = 1").rows == Grid{{t(R"({"a": 1, "b": [1, 2, true], "n": null})")}});
  db->execute("UPDATE j SET v = v || $2 WHERE id = $1 AND NOT v ? $3", {i(2), t(R"(["c"])"), t("c")});
  db->execute("UPDATE j SET v = v || $2 WHERE id = $1 AND NOT v ? $3", {i(2), t(R"(["c"])"), t("c")});
  CHECK(db->execute("SELECT v FROM j WHERE id = 2").rows == Grid{{t(R"(["a", 1, [2], "c"])")}});
  // Construction: to_jsonb of each argument, SQL NULL a JSON null in a build.
  CHECK(db->execute("SELECT jsonb_build_object(s, n, 'v', v), jsonb_build_array(n, s, NULL), to_jsonb(n) FROM j WHERE id < 3 ORDER BY id")
            .rows == Grid{{t(R"({"a": 1, "v": {"a": 1, "b": [1, 2, true], "n": null}})"), t(R"([1, "a", null])"), t("1")},
                          {t(R"({"b": 0, "v": ["a", 1, [2], "c"]})"), t(R"([0, "b", null])"), t("0")}});
  CHECK(db->execute("SELECT jsonb_build_array(n, s), to_jsonb(n), jsonb_set(v, '{a}', '1') FROM j WHERE id = 3").rows ==
        Grid{{t("[null, null]"), kNull, kNull}});
  CHECK(state_of(*db, "SELECT jsonb_build_object(s, 1) FROM j") == "22023");
  CHECK(db->execute("SELECT jsonb_pretty(v) FROM j WHERE id = 2").rows == Grid{{t("[\n    \"a\",\n    1,\n    [\n        2\n    ],\n    \"c\"\n]")}});
  // Constants are folded at prepare, their errors the statement's.
  CHECK(db->execute(R"(SELECT '{"a":1}'::jsonb @> '{}', '["x"]'::jsonb ? 'x', '{"a":1}' || '[2]'::jsonb, jsonb_build_object(1.5, true))").rows ==
        Grid{{true, true, t(R"([{"a": 1}, 2])"), t(R"({"1.5": true})")}});
  CHECK(state_of(*db, R"(SELECT jsonb_set_lax('{"a":1}', '{a}', NULL, true, 'raise_exception'))") == "22004");
}
