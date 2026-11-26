// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb, stage 1 (docs/jsonb_design.md, *Tests of jsonb by stage*): the
// parser against every row of *jsonb input grammar and its errors*, DETAIL,
// token extent and CONTEXT included, each recorded from PostgreSQL 17;
// normalisation and output text; numbers through numeric_parse; the encoder
// against hand-written layouts; every decoding rule broken in turn giving
// Corrupt; the output length's first pass against the bytes the second
// writes; the budget and allocation-failure sweeps of the parser and the
// output writer; and a round trip over generated values.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tests/alloc_faults.h"

import bytecask.sql.jsonb;
import bytecask.sql.numeric;
import bytecask.sql.row_codec;

#include "bytecaskdb-sql-layer/tests/fuzz/jsonb_properties.h"

using namespace bytecask::sql;
namespace jb = bytecask::sql::jsonb;

namespace {

using Value = std::vector<std::byte>;

auto bytes(std::initializer_list<int> b) -> Value {
  Value v;
  for (auto x : b) v.push_back(static_cast<std::byte>(x));
  return v;
}

void append(Value& v, std::string_view s) {
  for (auto c : s) v.push_back(static_cast<std::byte>(c));
}
void append_u32(Value& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::byte>((x >> (8 * i)) & 0xFF));
}
void append_u16(Value& v, std::uint32_t x) {
  v.push_back(static_cast<std::byte>(x & 0xFF));
  v.push_back(static_cast<std::byte>((x >> 8) & 0xFF));
}

// A number node: tag, sign, dscale, weight, digits.
auto number_node(int sign, std::uint32_t dscale, std::int32_t weight, std::string_view digits) -> Value {
  Value v{std::byte{jb::kTagNumber}, static_cast<std::byte>(sign)};
  append_u16(v, dscale);
  append_u32(v, static_cast<std::uint32_t>(weight));
  append(v, digits);
  return v;
}

auto with_format(const Value& node) -> Value {
  Value v{std::byte{jb::kFormat}};
  v.insert(v.end(), node.begin(), node.end());
  return v;
}

// The text of input, or the error's SQLSTATE, message, DETAIL and CONTEXT.
struct Parsed {
  std::string text;
  std::string state;
  std::string message;
  std::string detail;
  std::string context;
};

auto run(std::string_view input) -> Parsed {
  Value value;
  auto r = jb::parse_value(input, value);
  Parsed out;
  if (!r.ok()) {
    REQUIRE(r.status == jb::Status::Data);
    out.state = std::string{r.error->state.view()};
    out.message = r.error->message;
    out.detail = r.error->detail;
    out.context = r.error->context;
    return out;
  }
  // The encoder's output passes the full check, and its text parses back to
  // the same bytes.
  REQUIRE(jb::check_value(value, {}).ok());
  REQUIRE(jb::value_text(value, out.text).ok());
  Value again;
  REQUIRE(jb::parse_value(out.text, again).ok());
  REQUIRE(again == value);
  return out;
}

auto text_of(std::string_view input) -> std::string {
  auto p = run(input);
  INFO(input << " -> " << p.state << " " << p.detail);
  REQUIRE(p.state.empty());
  return p.text;
}

void syntax(std::string_view input, std::string_view detail, std::string_view context) {
  auto p = run(input);
  INFO(input);
  CHECK(p.state == "22P02");
  CHECK(p.message == "invalid input syntax for type json");
  CHECK(p.detail == detail);
  CHECK(p.context == context);
}

void failed(std::string_view input, std::string_view state, std::string_view message) {
  auto p = run(input);
  INFO(input);
  CHECK(p.state == state);
  CHECK(p.message == message);
}

}  // namespace

TEST_CASE("jsonb input: the DETAIL and CONTEXT of each syntax error, as PostgreSQL 17", "[sql][jsonb]") {
  syntax("", "The input string ended unexpectedly.", "JSON data, line 1: ");
  syntax("   ", "The input string ended unexpectedly.", "JSON data, line 1:    ");
  syntax("[1,2", "The input string ended unexpectedly.", "JSON data, line 1: [1,2");
  syntax("{\"a\"", "The input string ended unexpectedly.", "JSON data, line 1: {\"a\"");
  syntax("[1,   ", "The input string ended unexpectedly.", "JSON data, line 1: [1,   ");
  syntax("[1,]", "Expected JSON value, but found \"]\".", "JSON data, line 1: [1,]");
  syntax("[}", "Expected JSON value, but found \"}\".", "JSON data, line 1: [}");
  syntax("{\"a\":}", "Expected JSON value, but found \"}\".", "JSON data, line 1: {\"a\":}");
  syntax("{\"a\":1,}", "Expected string, but found \"}\".", "JSON data, line 1: {\"a\":1,}");
  syntax("{1:2}", "Expected string or \"}\", but found \"1\".", "JSON data, line 1: {1...");
  syntax("{,}", "Expected string or \"}\", but found \",\".", "JSON data, line 1: {,...");
  syntax("{\"a\" 1}", "Expected \":\", but found \"1\".", "JSON data, line 1: {\"a\" 1...");
  syntax("{\"a\",1}", "Expected \":\", but found \",\".", "JSON data, line 1: {\"a\",...");
  syntax("{\"a\":1 \"b\":2}", "Expected \",\" or \"}\", but found \"\"b\"\".", "JSON data, line 1: {\"a\":1 \"b\"...");
  syntax("[1 2]", "Expected \",\" or \"]\", but found \"2\".", "JSON data, line 1: [1 2...");
  syntax("[1,2}", "Expected \",\" or \"]\", but found \"}\".", "JSON data, line 1: [1,2}");
  syntax("1 2", "Expected end of input, but found \"2\".", "JSON data, line 1: 1 2");
  syntax("[1]]", "Expected end of input, but found \"]\".", "JSON data, line 1: [1]]");
  syntax("{a:1}", "Token \"a\" is invalid.", "JSON data, line 1: {a...");
  syntax("01", "Token \"01\" is invalid.", "JSON data, line 1: 01");
  syntax(".5", "Token \".\" is invalid.", "JSON data, line 1: ....");
  syntax("+1", "Token \"+\" is invalid.", "JSON data, line 1: +...");
  syntax("\"abc", "Token \"\"abc\" is invalid.", "JSON data, line 1: \"abc");
  syntax("\"a\tb\"", "Character with value 0x09 must be escaped.", "JSON data, line 1: \"a...");
  syntax("\"a\nb", "Character with value 0x0a must be escaped.", "JSON data, line 1: \"a");
  syntax("\"\\x\"", "Escape sequence \"\\x\" is invalid.", "JSON data, line 1: \"\\x...");
  syntax("\"\\u00\"", "\"\\u\" must be followed by four hexadecimal digits.", "JSON data, line 1: \"\\u00\"");
  syntax("\"\\u00zz\"", "\"\\u\" must be followed by four hexadecimal digits.", "JSON data, line 1: \"\\u00z...");
  syntax("\"\\ud83d\"", "Unicode low surrogate must follow a high surrogate.", "JSON data, line 1: \"\\ud83d\"");
  syntax("\"\\ud83dx\"", "Unicode low surrogate must follow a high surrogate.", "JSON data, line 1: \"\\ud83dx...");
  syntax("\"\\ude00\"", "Unicode low surrogate must follow a high surrogate.", "JSON data, line 1: \"\\ude00...");
  syntax("\"\\ud83dA\"", "Unicode low surrogate must follow a high surrogate.", "JSON data, line 1: \"\\ud83dA...");
  syntax("\"\\ud83d\\ud83d\"", "Unicode high surrogate must not follow a high surrogate.",
         "JSON data, line 1: \"\\ud83d\\ud83d...");
  syntax("{\n\"a\":\n [1,\n x]}", "Token \"x\" is invalid.", "JSON data, line 4:  x...");
  // A lex error of the next token comes before a number's range error.
  syntax("1e131072 x", "Token \"x\" is invalid.", "JSON data, line 1: 1e131072 x");
}

TEST_CASE("jsonb input: the extent of an invalid token", "[sql][jsonb]") {
  auto token = [](std::string_view input) {
    auto p = run(input);
    INFO(input);
    REQUIRE(p.state == "22P02");
    return p.detail;
  };
  CHECK(token("truex") == "Token \"truex\" is invalid.");
  CHECK(token("True") == "Token \"True\" is invalid.");
  CHECK(token("nul") == "Token \"nul\" is invalid.");
  CHECK(token("NaN") == "Token \"NaN\" is invalid.");
  CHECK(token("Infinity") == "Token \"Infinity\" is invalid.");
  CHECK(token("\xc3\xa9") == "Token \"\xc3\xa9\" is invalid.");
  CHECK(token("-01") == "Token \"-01\" is invalid.");
  CHECK(token("5.") == "Token \"5.\" is invalid.");
  CHECK(token("1e") == "Token \"1e\" is invalid.");
  CHECK(token("1.5e+") == "Token \"1.5e+\" is invalid.");
  CHECK(token("1e5x") == "Token \"1e5x\" is invalid.");
  CHECK(token("1_000") == "Token \"1_000\" is invalid.");
  CHECK(token("0x10") == "Token \"0x10\" is invalid.");
  CHECK(token("1\xc3\xa9") == "Token \"1\xc3\xa9\" is invalid.");
  CHECK(token("1\xc2\xa0") == "Token \"1\xc2\xa0\" is invalid.");
  CHECK(token("1.2.3") == "Token \".\" is invalid.");
  CHECK(token("1e5.") == "Token \".\" is invalid.");
  CHECK(token("-") == "Token \"-\" is invalid.");
  CHECK(token("\f1") == "Token \"\f\" is invalid.");
  CHECK(token("[\xc2\xa0]") == "Token \"\xc2\xa0\" is invalid.");
}

TEST_CASE("jsonb input: CONTEXT shows at most 49 bytes, from a character boundary", "[sql][jsonb]") {
  // An 80-byte bad token: "..." and its last 49 bytes.
  std::string token(80, 'x');
  auto p = run(token);
  CHECK(p.detail == "Token \"" + token + "\" is invalid.");
  CHECK(p.context == "JSON data, line 1: ..." + token.substr(31));
  // 40 é (80 bytes) then a bad token: the window starts at the next
  // character boundary.
  std::string e;
  for (int i = 0; i < 40; ++i) e += "\xc3\xa9";
  auto q = run("[\"" + e + "\", x]");
  CHECK(q.detail == "Token \"x\" is invalid.");
  CHECK(q.context.starts_with("JSON data, line 1: ..."));
  auto shown = q.context.substr(std::string_view{"JSON data, line 1: ..."}.size());
  CHECK(shown.ends_with("\", x..."));
  CHECK(shown.size() == 48 + 3);  // 22 é and `", x`, then "..."
  CHECK((static_cast<unsigned char>(shown[0]) & 0xC0u) != 0x80u);
  // Within 3 bytes of the line's start, the whole line.
  CHECK(run(std::string(50, ' ') + "x").context == "JSON data, line 1: " + std::string(50, ' ') + "x");
  CHECK(run(std::string(54, ' ') + "x").context == "JSON data, line 1: ..." + std::string(48, ' ') + "x");
}

TEST_CASE("jsonb input: \\u0000, numbers past numeric's range, depth and encoding", "[sql][jsonb]") {
  auto p = run("\"\\u0000\"");
  CHECK(p.state == "22P05");
  CHECK(p.message == "unsupported Unicode escape sequence");
  CHECK(p.detail == "\\u0000 cannot be converted to text.");
  CHECK(p.context == "JSON data, line 1: \"\\u0000...");
  for (auto big : {"1e131072", "1e-16384", "0e-16384", "0e1073741824", "1e1073741824", "100e-16385", "[1, 1e131072]"}) {
    failed(big, "22003", "value overflows numeric format");
  }
  // Inside the range, whatever the spelling.
  CHECK(text_of("0e131072") == "0");
  CHECK(text_of("0e1000000") == "0");
  CHECK(text_of("0e1073741823") == "0");
  CHECK(text_of("1e131071").size() == 131072);
  CHECK(text_of("9.99e131071").size() == 131072);
  CHECK(text_of("0.0001e131075") == text_of("1e131071"));
  CHECK(text_of("-0.0e-16382").size() == 16385);
  // 10000 nested containers parse; 10001 is 54001.
  auto nest = [](std::size_t n, char open, char close) { return std::string(n, open) + std::string(n, close); };
  CHECK(text_of(nest(10000, '[', ']')) == nest(10000, '[', ']'));
  failed(nest(10001, '[', ']'), "54001", "stack depth limit exceeded");
  std::string objects;
  for (int i = 0; i < 10000; ++i) objects += "{\"a\":";
  objects += "1" + std::string(10000, '}');
  CHECK(run(objects).state.empty());
  failed("{\"a\":" + objects + "}", "54001", "stack depth limit exceeded");
  // Bytes that are not UTF-8: 22021 before any parse.
  failed("\"\xff\"", "22021", "invalid byte sequence for encoding \"UTF8\"");
  failed("\"\xed\xa0\x80\"", "22021", "invalid byte sequence for encoding \"UTF8\"");
}

TEST_CASE("jsonb normalisation and output text, byte for byte", "[sql][jsonb]") {
  CHECK(text_of("{\"a\":1, \"a\":{\"x\":1}, \"a\":[2]}") == "{\"a\": [2]}");
  CHECK(text_of("{\"aaa\":1,\"b\":2,\"ab\":3,\"B\":4,\"\xc3\xa9\":5,\"z\":6}") ==
        "{\"B\": 4, \"b\": 2, \"z\": 6, \"ab\": 3, \"\xc3\xa9\": 5, \"aaa\": 1}");
  CHECK(text_of("  [1 , 2,{ \"x\" : [ ] , \"y\":{}} ]  ") == "[1, 2, {\"x\": [], \"y\": {}}]");
  CHECK(text_of("{}") == "{}");
  CHECK(text_of("[]") == "[]");
  CHECK(text_of("1.0") == "1.0");
  CHECK(text_of("1e2") == "100");
  CHECK(text_of("1.000e2") == "100.0");
  CHECK(text_of("-0") == "0");
  CHECK(text_of("-0.0") == "0.0");
  CHECK(text_of("0.0e-3") == "0.0000");
  CHECK(text_of("123456789012345678901234567890.123456789012345678901234567890") ==
        "123456789012345678901234567890.123456789012345678901234567890");
  CHECK(text_of("0.0015") == "0.0015");
  CHECK(text_of("-12.50") == "-12.50");
  CHECK(text_of("1E-2") == "0.01");
  CHECK(text_of("\"abc\"") == "\"abc\"");
  CHECK(text_of("\"\\u00e9\\ud83d\\ude00\"") == "\"\xc3\xa9\xf0\x9f\x98\x80\"");
  CHECK(text_of("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\u0001 \\u001f \\u007f\"") ==
        "\"\\\" \\\\ / \\b \\f \\n \\r \\t \\u0001 \\u001f \x7f\"");
  CHECK(text_of(" true ") == "true");
  CHECK(text_of("false") == "false");
  CHECK(text_of("null") == "null");
  CHECK(text_of("[null, true, false, \"\", 0]") == "[null, true, false, \"\", 0]");
  // Keys are ordered by their de-escaped bytes.
  CHECK(text_of("{\"\\u0062\":1, \"a\":2, \"\\u00e9\":3, \"ab\":4}") == "{\"a\": 2, \"b\": 1, \"ab\": 4, \"\xc3\xa9\": 3}");
  CHECK(text_of("{\"a\\\"b\":1}") == "{\"a\\\"b\": 1}");
}

TEST_CASE("jsonb encoder: hand-written layouts of each tag", "[sql][jsonb]") {
  auto encoded = [](std::string_view text) {
    Value v;
    REQUIRE(jb::parse_value(text, v).ok());
    return v;
  };
  CHECK(encoded("null") == bytes({0xF8, 0x01}));
  CHECK(encoded("false") == bytes({0xF8, 0x02}));
  CHECK(encoded("true") == bytes({0xF8, 0x03}));
  CHECK(encoded("[]") == bytes({0xF8, 0x06, 0, 0, 0, 0}));
  CHECK(encoded("{}") == bytes({0xF8, 0x07, 0, 0, 0, 0}));
  CHECK(encoded("\"ab\"") == bytes({0xF8, 0x05, 'a', 'b'}));
  CHECK(encoded("0") == with_format(number_node(0, 0, 0, "")));
  CHECK(encoded("0.00") == with_format(number_node(0, 2, 0, "")));
  CHECK(encoded("123.45") == with_format(number_node(1, 2, 2, "12345")));
  CHECK(encoded("0.0015") == with_format(number_node(1, 4, -3, "15")));
  CHECK(encoded("100") == with_format(number_node(1, 0, 2, "1")));
  CHECK(encoded("1.0") == with_format(number_node(1, 1, 0, "1")));
  CHECK(encoded("-7") == with_format(number_node(2, 0, 0, "7")));
  CHECK(encoded("1e131071").size() == 10);
  // [1, "x"]: count, ends, then the elements.
  auto arr = bytes({0xF8, 0x06});
  append_u32(arr, 2);
  append_u32(arr, 9);
  append_u32(arr, 11);
  auto one = number_node(1, 0, 0, "1");
  arr.insert(arr.end(), one.begin(), one.end());
  append(arr, "\x05x");
  CHECK(encoded("[1, \"x\"]") == arr);
  // {"b": null, "a": true}: keys in storage order, then values.
  auto obj = bytes({0xF8, 0x07});
  append_u32(obj, 2);
  append_u32(obj, 1);
  append_u32(obj, 2);
  append_u32(obj, 1);
  append_u32(obj, 2);
  append(obj, "ab");
  obj.push_back(std::byte{0x03});
  obj.push_back(std::byte{0x01});
  CHECK(encoded("{\"b\": null, \"a\": true}") == obj);
}

TEST_CASE("jsonb numbers: numeric_parse agrees with numeric_in's text", "[sql][jsonb]") {
  auto text = [](std::string_view s) {
    std::string digits(s.size(), '\0');
    NumericParts p;
    REQUIRE(numeric_parse(s, digits, p) == NumericStatus::Ok);
    REQUIRE(p.end == s.size());
    REQUIRE(numeric_in_range(p));
    return numeric_text(false, p, std::string_view{digits}.substr(0, p.ndigits));
  };
  CHECK(text("1.50") == "1.50");
  CHECK(text("1.5e1") == "15");
  CHECK(text("1.50e1") == "15.0");
  CHECK(text("0.0") == "0.0");
  CHECK(text("1_000.5") == "1000.5");
  CHECK(text(".5") == "0.5");
  NumericParts p;
  CHECK(numeric_parse("1e1073741824", {}, p) == NumericStatus::Overflow);
  CHECK(numeric_parse("x", {}, p) == NumericStatus::Syntax);
  CHECK(numeric_parse("1e", {}, p) == NumericStatus::Syntax);
  REQUIRE(numeric_parse("100e-16385", {}, p) == NumericStatus::Ok);
  CHECK(!numeric_in_range(p));
}

namespace {

auto corrupt_node(const Value& node) -> bool {
  auto r = jb::check_node(node, {});
  if (r.status != jb::Status::Corrupt) return false;
  std::uint64_t n = 0;
  return jb::text_length(node, {}, n).status == jb::Status::Corrupt;
}

auto array_node(std::uint32_t n, std::initializer_list<std::uint32_t> ends, const Value& elements) -> Value {
  Value v{std::byte{jb::kTagArray}};
  append_u32(v, n);
  for (auto e : ends) append_u32(v, e);
  v.insert(v.end(), elements.begin(), elements.end());
  return v;
}

auto object_node(std::uint32_t n, std::initializer_list<std::uint32_t> key_ends, std::initializer_list<std::uint32_t> val_ends,
                 std::string_view keys, const Value& values) -> Value {
  Value v{std::byte{jb::kTagObject}};
  append_u32(v, n);
  for (auto e : key_ends) append_u32(v, e);
  for (auto e : val_ends) append_u32(v, e);
  append(v, keys);
  v.insert(v.end(), values.begin(), values.end());
  return v;
}

}  // namespace

TEST_CASE("jsonb decoding: every rule broken in turn is Corrupt", "[sql][jsonb]") {
  auto null1 = bytes({0x01});
  auto two_nulls = bytes({0x01, 0x01});
  // The good forms the breaks start from.
  REQUIRE(jb::check_node(array_node(2, {1, 2}, two_nulls), {}).ok());
  REQUIRE(jb::check_node(object_node(2, {1, 2}, {1, 2}, "ab", two_nulls), {}).ok());
  REQUIRE(jb::check_node(object_node(1, {0}, {1}, "", null1), {}).ok());  // an empty key
  // A count past its bound, and 2^32 - 1.
  CHECK(corrupt_node(array_node(3, {1, 2}, two_nulls)));
  CHECK(corrupt_node(array_node(0xFFFFFFFFu, {1, 2}, two_nulls)));
  CHECK(corrupt_node(object_node(0xFFFFFFFFu, {1, 2}, {1, 2}, "ab", two_nulls)));
  // A decreasing end, an overrunning one, an empty element, trailing bytes.
  CHECK(corrupt_node(array_node(2, {2, 1}, two_nulls)));
  CHECK(corrupt_node(array_node(2, {1, 3}, two_nulls)));
  CHECK(corrupt_node(array_node(2, {0, 2}, two_nulls)));
  CHECK(corrupt_node(array_node(2, {1, 1}, two_nulls)));
  auto trailing = two_nulls;
  trailing.push_back(std::byte{0x01});
  CHECK(corrupt_node(array_node(2, {1, 2}, trailing)));
  // An empty container of extent 6.
  CHECK(corrupt_node(bytes({0x06, 0, 0, 0, 0, 0x01})));
  CHECK(corrupt_node(bytes({0x07, 0, 0, 0, 0, 0x01})));
  CHECK(corrupt_node(bytes({0x06, 0, 0, 0})));
  // key_end[n-1] past the extent; a key end below its predecessor.
  CHECK(corrupt_node(object_node(2, {1, 200}, {1, 2}, "ab", two_nulls)));
  CHECK(corrupt_node(object_node(2, {2, 1}, {1, 2}, "ab", two_nulls)));
  // Keys out of storage order, or equal.
  CHECK(corrupt_node(object_node(2, {1, 2}, {1, 2}, "ba", two_nulls)));
  CHECK(corrupt_node(object_node(2, {1, 2}, {1, 2}, "aa", two_nulls)));
  CHECK(corrupt_node(object_node(2, {2, 3}, {1, 2}, "abc", two_nulls)));  // longer key first
  // A value end out of order.
  CHECK(corrupt_node(object_node(2, {1, 2}, {2, 2}, "ab", two_nulls)));
  // A 0x00 inside a string or a key, and bytes that are not UTF-8.
  CHECK(corrupt_node(bytes({0x05, 'a', 0x00, 'b'})));
  CHECK(corrupt_node(object_node(1, {1}, {1}, std::string_view{"\0", 1}, null1)));
  CHECK(corrupt_node(bytes({0x05, 0xC3})));
  CHECK(corrupt_node(bytes({0x05, 0xED, 0xA0, 0x80})));
  // Scalars of the wrong extent, an unknown tag, an empty node.
  CHECK(corrupt_node(bytes({0x01, 0x00})));
  CHECK(corrupt_node(bytes({0x03, 0x01})));
  CHECK(corrupt_node(bytes({0x08})));
  CHECK(corrupt_node(bytes({0x00})));
  CHECK(corrupt_node(Value{}));
  // Numbers: weights INT32_MIN and INT32_MAX, dscale 16384, a '0' at either
  // end, a sign of 3, a zero with digits, too short, a non-digit, digits past
  // the scale.
  REQUIRE(jb::check_node(number_node(1, 0, 0, "1"), {}).ok());
  CHECK(corrupt_node(number_node(1, 0, std::numeric_limits<std::int32_t>::min(), "1")));
  CHECK(corrupt_node(number_node(1, 0, std::numeric_limits<std::int32_t>::max(), "1")));
  CHECK(corrupt_node(number_node(1, 0, 131072, "1")));
  CHECK(corrupt_node(number_node(1, 16384, 0, "1")));
  CHECK(corrupt_node(number_node(1, 0, 0, "01")));
  CHECK(corrupt_node(number_node(1, 1, 0, "10")));
  CHECK(corrupt_node(number_node(3, 0, 0, "1")));
  CHECK(corrupt_node(number_node(0, 0, 0, "1")));
  CHECK(corrupt_node(number_node(0, 0, 1, "")));
  CHECK(corrupt_node(number_node(1, 0, 0, "")));
  CHECK(corrupt_node(number_node(1, 0, 0, "1a")));
  CHECK(corrupt_node(number_node(1, 0, 0, "12")));  // 1.2 needs dscale 1
  CHECK(corrupt_node(bytes({0x04, 0x01, 0x00})));
  // Depth 10001 of stored arrays.
  Value deep{std::byte{0x01}};
  for (int i = 0; i < 10001; ++i) {
    deep = array_node(1, {static_cast<std::uint32_t>(deep.size())}, deep);
  }
  CHECK(corrupt_node(deep));
  // The format byte: 0x01 with \x01\x01 (the wire's version byte and text)
  // is no value, nor is canonical text, nor a format byte alone.
  CHECK(jb::check_value(bytes({0x01, 0x01}), {}).status == jb::Status::Corrupt);
  CHECK(jb::check_value(bytes({'[', ']'}), {}).status == jb::Status::Corrupt);
  CHECK(jb::check_value(bytes({0xF8}), {}).status == jb::Status::Corrupt);
  CHECK(jb::check_value(bytes({0xF9, 0x01}), {}).status == jb::Status::Corrupt);
}

TEST_CASE("jsonb output: the length pass matches the bytes the write pass writes", "[sql][jsonb]") {
  for (auto weight : {-1, -3, -16383, 0, 131071}) {
    for (auto dscale : {0u, 16383u}) {
      auto node = number_node(2, dscale, weight, "7");
      if (static_cast<std::int64_t>(weight) < -static_cast<std::int64_t>(dscale)) continue;  // the layout forbids it
      std::uint64_t n = 0;
      REQUIRE(jb::text_length(node, {}, n).ok());
      std::string out(n, '\0');
      REQUIRE(jb::write_text(node, {}, std::span{out.data(), out.size()}).ok());
      CHECK(out.size() == (1 + (weight >= 0 ? static_cast<std::size_t>(weight) + 1 : 1) + (dscale > 0 ? 1 + dscale : 0)));
      CHECK(out.find('\0') == std::string::npos);
      // A buffer one byte short is Corrupt, never a write past it.
      std::string shorter(n - 1, '\0');
      CHECK(jb::write_text(node, {}, std::span{shorter.data(), shorter.size()}).status == jb::Status::Corrupt);
    }
  }
  std::uint64_t n = 0;
  REQUIRE(jb::text_length(number_node(1, 16383, -16383, "1"), {}, n).ok());
  CHECK(n == 16385);
}

namespace {

// The sites an allocation-failure sweep must fail: jb::Site's four (the
// parser's nodes, key index and key pool, a walk's stack) and the encoded
// value or text written through Out.
constexpr std::size_t kOutSite = 4;
constexpr std::size_t kSites = 5;
static_assert(static_cast<std::size_t>(jb::Site::Stack) + 1 == kOutSite);

// A Memory over a budget: refuses a charge past `limit`, and counts.
struct Budget {
  std::uint64_t limit;
  std::uint64_t used{0};
  std::uint64_t peak{0};
  // In an allocation-failure sweep: the sweep's faults, the allocation they
  // fail, and the sites (kSites) whose allocation it was, marked without
  // allocating so the count is not disturbed.
  const bytecask::testing::ScopedAllocFaults* faults{nullptr};
  long nth{0};
  std::array<bool, kSites>* failed_at{nullptr};
  // Marks `site` when the next allocation is the failing one: each charge
  // is made just before the one allocation it is for.
  void note(std::size_t site) const {
#ifdef BYTECASK_NO_ALLOC_FAULTS
    // The seam is not built under this sanitizer; no sweep sets `faults`.
    (void)site;
#else
    if (faults != nullptr && faults->allocations() + 1 == nth) (*failed_at)[site] = true;
#endif
  }
  static auto take(void* self, std::uint64_t n, jb::Site site) -> bool {
    auto& b = *static_cast<Budget*>(self);
    b.note(static_cast<std::size_t>(site));
    return charge(b, n);
  }
  static auto charge(Budget& b, std::uint64_t n) -> bool {
    if (n > b.limit - std::min(b.used, b.limit)) return false;
    b.used += n;
    b.peak = std::max(b.peak, b.used);
    return true;
  }
  static void release(void* self, std::uint64_t n) noexcept { static_cast<Budget*>(self)->used -= n; }
  auto memory() -> jb::Memory { return {this, take, release}; }
};

struct Out {
  Budget* budget;
  std::vector<std::byte> buf;
  static auto alloc(void* self, std::size_t n) -> std::byte* {
    auto& o = *static_cast<Out*>(self);
    o.budget->note(kOutSite);
    if (!Budget::charge(*o.budget, n)) return nullptr;
    o.buf.resize(n);
    return o.buf.data();
  }
};

}  // namespace

TEST_CASE("jsonb parse and output: a budget sweep gives a refusal or the full result", "[sql][jsonb]") {
  std::string input = "{\"k\": [1, 2.50, \"\\u00e9x\", {\"b\": null, \"a\": [true, false]}], \"\\u0061\": -0.001, \"z\": {}}";
  Value want;
  REQUIRE(jb::parse_value(input, want).ok());
  std::string want_text;
  REQUIRE(jb::value_text(want, want_text).ok());
  auto refusals = 0;
  for (std::uint64_t limit = 0;; ++limit) {
    Budget b{limit};
    Out out{&b, {}};
    std::span<std::byte> value;
    auto r = jb::parse(input, b.memory(), {&out, Out::alloc}, value);
    if (!r.ok()) {
      REQUIRE(r.status == jb::Status::Refused);
      ++refusals;
      continue;
    }
    CHECK(std::vector<std::byte>(value.begin(), value.end()) == want);
    // Only the value stays charged.
    CHECK(b.used == want.size());
    break;
  }
  CHECK(refusals > 0);
  auto node = *jb::node_of(want);
  for (std::uint64_t limit = 0;; ++limit) {
    Budget b{limit};
    Out out{&b, {}};
    std::span<char> text;
    auto r = jb::to_text(node, b.memory(), {&out, Out::alloc}, text);
    if (!r.ok()) {
      REQUIRE(r.status == jb::Status::Refused);
      continue;
    }
    CHECK(std::string{text.data(), text.size()} == want_text);
    CHECK(b.used == want_text.size());
    break;
  }
}

TEST_CASE("jsonb parse and output: every allocation fails in turn as bad_alloc, leaving nothing", "[sql][jsonb]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  std::string input = "{\"k\": [1, 2.50, \"\\u00e9x\", {\"b\": null, \"a\": [true, false]}], \"\\u0061\": -0.001, \"z\": {}}";
  Value want;
  REQUIRE(jb::parse_value(input, want).ok());
  std::string want_text;
  REQUIRE(jb::value_text(want, want_text).ok());
  // Each sweep fails, in some run, an allocation of every site it uses:
  // its reach asserted, not trusted to the input (tests/CLAUDE.md).
  std::array<bool, kSites> parse_failed{};
  long failures = 0;
  for (long nth = 1;; ++nth) {
    Budget b{1u << 20};
    Out out{&b, {}};
    bool threw = false;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      b.faults = &faults;
      b.nth = nth;
      b.failed_at = &parse_failed;
      try {
        std::span<std::byte> value;
        auto r = jb::parse(input, b.memory(), {&out, Out::alloc}, value);
        REQUIRE(r.ok());
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      if (!faults.report().fired) {
        REQUIRE(!threw);
        break;
      }
    }
    REQUIRE(threw);
    ++failures;
    // What a failed parse charged is released, but for the output the
    // allocator kept.
    CHECK(b.used <= want.size());
  }
  CHECK(failures > 0);
  // The parser's nodes, key index and pool of de-escaped keys ("\u0061"),
  // and the encoded value (a parse walks no stored value).
  CHECK(parse_failed == std::array<bool, kSites>{true, true, true, false, true});
  auto node = *jb::node_of(want);
  std::array<bool, kSites> text_failed{};
  long out_failures = 0;
  for (long nth = 1;; ++nth) {
    Budget b{1u << 20};
    Out out{&b, {}};
    bool threw = false;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      b.faults = &faults;
      b.nth = nth;
      b.failed_at = &text_failed;
      try {
        std::span<char> text;
        auto r = jb::to_text(node, b.memory(), {&out, Out::alloc}, text);
        REQUIRE(r.ok());
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      if (!faults.report().fired) {
        REQUIRE(!threw);
        break;
      }
    }
    REQUIRE(threw);
    ++out_failures;
  }
  CHECK(out_failures > 0);
  // The output writer: its walk's stack and the text.
  CHECK(text_failed == std::array<bool, kSites>{false, false, false, true, true});
#endif
}

TEST_CASE("jsonb round trip: generated values parse, check, print and parse back to the same bytes", "[sql][jsonb]") {
  std::mt19937_64 rng{20261121};
  auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
  static constexpr std::string_view kScalars[] = {"null", "true", "false", "0", "-0.0", "1.50", "1e-3", "123456789012345678901234567890",
                                                  "\"\"", "\"a\"", "\"\\u00e9\"", "\"\\ud83d\\ude00\"", "\"\\n\\t\"", "-12e2"};
  static constexpr std::string_view kKeys[] = {"a", "b", "ab", "B", "\\u0062", "", "aaa", "\\u00e9"};
  for (int round = 0; round < 300; ++round) {
    std::string s;
    // A random nesting written iteratively: a stack of open containers.
    std::vector<char> open;
    std::vector<int> items;
    auto budget = 40;
    auto value = [&] {
      while (true) {
        auto c = pick(5);
        if (c < 3 || budget-- <= 0 || open.size() > 6) {
          s += kScalars[pick(std::size(kScalars))];
          return;
        }
        auto obj = c == 4;
        s += obj ? '{' : '[';
        open.push_back(obj ? '}' : ']');
        items.push_back(0);
        return;
      }
    };
    value();
    while (!open.empty()) {
      auto close = pick(3) == 0 || budget <= 0;
      if (close) {
        s += open.back();
        open.pop_back();
        items.pop_back();
        continue;
      }
      if (items.back()++ > 0) s += ", ";
      if (open.back() == '}') s += "\"" + std::string{kKeys[pick(std::size(kKeys))]} + "\": ";
      value();
    }
    INFO(s);
    auto p = run(s);
    CHECK(p.state.empty());
  }
}

TEST_CASE("jsonb sort key: memcmp order is PostgreSQL 17's jsonb_ops order, byte equality its equality", "[sql][jsonb]") {
  // Each row one class of equal values, the rows in PostgreSQL 17's order
  // (C collation), as `SELECT j FROM t ORDER BY j` printed them.
  const std::vector<std::vector<std::string_view>> order{
      {"[]"}, {"null"}, {"\"\""}, {"\"10\""}, {"\"9\""}, {"\"B\""}, {"\"a\""}, {"\"ab\""}, {"\"b\""}, {"\"é\""},
      {"-1e131071"}, {"-10"}, {"-1"}, {"-0.5"}, {"-1e-3"}, {"-1e-16383"}, {"0", "-0", "0.0", "0e-5"}, {"1e-16383"},
      {"1e-3"}, {"1", "1.0", "1.00", "10e-1"}, {"100", "1e2"}, {"1e131071"}, {"false"}, {"true"}, {"[null]"}, {"[\"a\"]"},
      {"[-1]"}, {"[1]", "[1.0]"}, {"[2]"}, {"[true]"}, {"[[]]"}, {"[[1]]"}, {"[[3]]"}, {"[[1, 2]]"}, {"[{}]"}, {"[1, 2]"},
      {"[1, 1, 1]"}, {"{}"}, {"{\"a\": null}"}, {"{\"a\": 1}", "{\"a\": 1.0}"}, {"{\"a\": 2}"}, {"{\"a\": [1]}"},
      {"{\"a\": {\"b\": null}}"}, {"{\"a\": {\"b\": []}}"}, {"{\"aa\": 0}"}, {"{\"b\": 0}"}, {"{\"z\": 0}"},
      {"{\"a\": 1, \"b\": 1}", "{\"b\": 1, \"a\": 1}"}, {"{\"a\": 1, \"c\": 1}"}, {"{\"ab\": 1, \"ac\": 1}"},
      {"{\"b\": 1, \"aa\": 1}"}, {"{\"b\": 1, \"c\": 1}"}, {"{\"z\": 1, \"aa\": 1}"}};
  struct Keyed {
    std::size_t rank;
    Value key;
  };
  std::vector<Keyed> keys;
  for (std::size_t r = 0; r < order.size(); ++r) {
    for (auto text : order[r]) {
      Value value;
      REQUIRE(jb::parse_value(text, value).ok());
      auto node = *jb::node_of(value);
      Value key;
      INFO(text);
      REQUIRE(jb::sort_key_value(node, key).ok());
      CHECK(key.size() <= jb::sort_key_bound(node));
      keys.push_back({r, std::move(key)});
    }
  }
  for (const auto& a : keys) {
    for (const auto& b : keys) {
      auto o = std::lexicographical_compare_three_way(a.key.begin(), a.key.end(), b.key.begin(), b.key.end());
      INFO(a.rank << " vs " << b.rank);
      CHECK((a.rank < b.rank) == std::is_lt(o));
      CHECK((a.rank == b.rank) == std::is_eq(o));
      // Prefix-free: a key concatenates with the next part of a sort key.
      if (a.key.size() < b.key.size()) CHECK_FALSE(std::equal(a.key.begin(), a.key.end(), b.key.begin()));
    }
  }
  // A nested empty array is an array, not the top level's lowest value.
  Value inner;
  REQUIRE(jb::parse_value("[[]]", inner).ok());
  std::optional<jb::ByteView> found;
  REQUIRE(jb::element(*jb::node_of(inner), 0, found).ok());
  Value key;
  REQUIRE(jb::sort_key_value(*found, key).ok());
  CHECK(key == bytes({0x01}));  // a value of its own, as PostgreSQL's -> result is
}

TEST_CASE("jsonb sort key: a damaged node is Corrupt, never a key; its stack is charged", "[sql][jsonb]") {
  Value key;
  auto corrupt = [&](const Value& node) { return jb::sort_key_value(node, key).status == jb::Status::Corrupt; };
  // A NUL in a string or a key would let two keys share a prefix.
  CHECK(corrupt(bytes({0x05, 'a', 0x00, 'b'})));
  CHECK(corrupt(object_node(1, {1}, {1}, std::string_view{"\0", 1}, bytes({0x01}))));
  CHECK(corrupt(number_node(1, 0, 0, "10")));
  CHECK(corrupt(number_node(1, 0, std::numeric_limits<std::int32_t>::max(), "1")));
  CHECK(corrupt(array_node(2, {1, 3}, bytes({0x01, 0x01}))));
  CHECK(corrupt(bytes({0x02, 0x00})));
  CHECK(corrupt(bytes({0x08})));
  CHECK(corrupt(Value{}));
  Value deep{std::byte{0x01}};
  for (int i = 0; i < 10001; ++i) deep = array_node(1, {static_cast<std::uint32_t>(deep.size())}, deep);
  CHECK(corrupt(deep));
  // A buffer short of the bound is Corrupt, never a write past it.
  Value value;
  REQUIRE(jb::parse_value("{\"a\": [1, \"x\"]}", value).ok());
  auto node = *jb::node_of(value);
  std::vector<std::byte> out(4);
  std::size_t length = 0;
  CHECK(jb::sort_key(node, {}, out, length).status == jb::Status::Corrupt);
  // A refused charge of the walk's stack is Refused.
  jb::Memory none{nullptr, [](void*, std::uint64_t, jb::Site) { return false; }, nullptr};
  out.resize(jb::sort_key_bound(node));
  CHECK(jb::sort_key(node, none, out, length).status == jb::Status::Refused);
  REQUIRE(jb::parse_value("\"x\"", value).ok());
  CHECK(jb::sort_key(*jb::node_of(value), none, out, length).ok());  // a scalar takes no stack
}

TEST_CASE("PhysType: every predicate answers for every value of the enum, Jsonb a variable non-key type that sorts", "[sql][jsonb]") {
  struct Row {
    PhysType t;
    bool fixed, key;
    SqlKind plain;
  };
  for (const auto& r : {Row{PhysType::I64, true, true, SqlKind::BigInt}, Row{PhysType::F64, true, true, SqlKind::Double},
                        Row{PhysType::Bool, true, true, SqlKind::Boolean}, Row{PhysType::Bytes, false, true, SqlKind::Bytea},
                        Row{PhysType::Num, false, false, SqlKind::Numeric}, Row{PhysType::Jsonb, false, false, SqlKind::Jsonb}}) {
    INFO(static_cast<int>(r.t));
    CHECK(is_fixed(r.t) == r.fixed);
    CHECK(keyable(r.t) == r.key);
    CHECK(sortable(r.t) == (r.key || r.t == PhysType::Jsonb));
    CHECK(phys_type_known(r.t));
    CHECK(plain_sql_type(r.t).kind == r.plain);
    CHECK(fixed_width_valid(r.t, 8) == (r.t == PhysType::I64 || r.t == PhysType::F64));
  }
  CHECK_FALSE(phys_type_known(static_cast<PhysType>(0)));
  CHECK_FALSE(phys_type_known(static_cast<PhysType>(7)));
  CHECK(static_cast<int>(SqlKind::Jsonb) == 19);
  CHECK(phys_type(SqlType{SqlKind::Jsonb}) == PhysType::Jsonb);
  CHECK(fixed_width(SqlType{SqlKind::Jsonb}) == 0);
  // A layout with a jsonb column keeps the format's invariants; one whose
  // type code no build knows does not.
  auto layout = make_layout(std::vector<ColumnDef>{{0, SqlType{SqlKind::BigInt}, false}, {1, SqlType{SqlKind::Jsonb}, true}});
  CHECK(layout_valid(layout));
  layout.columns[1].type = static_cast<PhysType>(7);
  CHECK_FALSE(layout_valid(layout));
}

TEST_CASE("jsonb fuzz properties: mutated values and texts give a result or a refusal, and round-trip", "[sql][jsonb]") {
  // The fuzz targets' properties (tests/fuzz/jsonb_properties.h), over
  // seeded mutations of valid values and texts: a deterministic slice of
  // what fuzz_sql_jsonb_decode and fuzz_sql_jsonb_parse run open-ended.
  std::mt19937_64 rng{889};
  static constexpr std::string_view kSeeds[] = {
      "{\"b\": [1, 2.50, {\"z\": null, \"a\": \"\\u00e9\\n\"}], \"a\": -0.001, \"\": {}}",
      "[[], {}, [[1e5]], \"x\", true, false, null, 0, -0.0]",
      "123456789012345678901234567890.1234567890",
      "\"\\ud83d\\ude00 \\\" \\\\ \\/ \\b\"",
  };
  auto decode_failures = 0;
  auto parse_failures = 0;
  for (auto seed : kSeeds) {
    Value value;
    REQUIRE(jb::parse_value(seed, value).ok());
    REQUIRE(jsonb_fuzz::decode_holds(value));
    REQUIRE(jsonb_fuzz::parse_holds(seed));
    for (int round = 0; round < 4000; ++round) {
      auto v = value;
      auto s = std::string{seed};
      for (auto edits = 1 + rng() % 3; edits-- > 0;) {
        auto at = rng() % v.size();
        switch (rng() % 4) {
          case 0: v[at] = static_cast<std::byte>(rng() & 0xFF); break;
          case 1: v[at] ^= static_cast<std::byte>(1u << (rng() % 8)); break;
          case 2: v.erase(v.begin() + static_cast<std::ptrdiff_t>(at)); break;
          default: v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::byte>(rng() & 0xFF)); break;
        }
        if (v.empty()) v.push_back(std::byte{jb::kFormat});
        auto sat = rng() % s.size();
        static constexpr std::string_view kBytes = "{}[],:\"\\0123456789.eE+-tfnul \t\n\xc3\xa9\x01";
        switch (rng() % 3) {
          case 0: s[sat] = kBytes[rng() % kBytes.size()]; break;
          case 1: s.erase(sat, 1); break;
          default: s.insert(sat, 1, kBytes[rng() % kBytes.size()]); break;
        }
        if (s.empty()) s = " ";
      }
      if (!jsonb_fuzz::decode_holds(v)) ++decode_failures;
      if (!jsonb_fuzz::parse_holds(s)) ++parse_failures;
    }
  }
  CHECK(decode_failures == 0);
  CHECK(parse_failures == 0);
}
