// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb, stage 3 (docs/jsonb_design.md, *Tests of jsonb by stage*): the
// accessors of sql/jsonb.cppm over nodes, each result recorded from
// PostgreSQL 17: -> by key and by index (missing keys, non-objects,
// negative and out-of-range indexes, scalars as arrays of themselves), #>
// over every path rule, array_in's reading of a path with its errors and
// DETAIL, ->>'s text forms, jsonb_typeof, jsonb_array_length and the casts
// out; a damaged value giving Corrupt at the read that meets it; and the
// budget and allocation-failure sweeps of the calls that allocate.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "tests/alloc_faults.h"

import bytecask.sql.jsonb;

using namespace bytecask::sql;
namespace jb = bytecask::sql::jsonb;

namespace {

using Value = std::vector<std::byte>;

// A stored value parsed from text; its node is node_of's.
struct Doc {
  Value value;
  explicit Doc(std::string_view text) { REQUIRE(jb::parse_value(text, value).ok()); }
  [[nodiscard]] auto node() const -> jb::ByteView { return *jb::node_of(value); }
};

auto text_of(jb::ByteView node) -> std::string {
  std::string out;
  REQUIRE(jb::node_text(node, out).ok());
  return out;
}

// -> key: the text found, or nullopt.
auto field(std::string_view doc, std::string_view key) -> std::optional<std::string> {
  Doc d{doc};
  std::optional<jb::ByteView> found;
  REQUIRE(jb::field(d.node(), key, found).ok());
  if (!found) return std::nullopt;
  return text_of(*found);
}

auto element(std::string_view doc, std::int64_t i) -> std::optional<std::string> {
  Doc d{doc};
  std::optional<jb::ByteView> found;
  REQUIRE(jb::element(d.node(), i, found).ok());
  if (!found) return std::nullopt;
  return text_of(*found);
}

// A path read by parse_path from a text[]'s text.
auto path(std::string_view text) -> Value {
  Value out;
  std::span<std::byte> p;
  auto r = jb::parse_path(text, jb::vector_out(out), p);
  REQUIRE(r.ok());
  return {p.begin(), p.end()};
}

auto at_path(std::string_view doc, std::string_view p) -> std::optional<std::string> {
  Doc d{doc};
  auto bytes = path(p);
  std::optional<jb::ByteView> found;
  REQUIRE(jb::at_path(d.node(), bytes, found).ok());
  if (!found) return std::nullopt;
  return text_of(*found);
}

// The error parse_path gives text: its SQLSTATE, message and DETAIL.
struct PathError {
  std::string state, message, detail;
};
auto path_error(std::string_view text) -> PathError {
  Value out;
  std::span<std::byte> p;
  auto r = jb::parse_path(text, jb::vector_out(out), p);
  REQUIRE(r.status == jb::Status::Data);
  return {std::string{r.error->state.view()}, r.error->message, r.error->detail};
}

// The elements of a path, NULL as nullopt.
auto elements(const Value& p) -> std::vector<std::optional<std::string>> {
  std::vector<std::optional<std::string>> out;
  auto n = jb::path_size(p);
  REQUIRE(n.has_value());
  std::size_t at = 4;
  auto rd = [&] {
    std::uint32_t x = 0;
    for (std::size_t i = 0; i < 4; ++i) x |= std::to_integer<std::uint32_t>(p[at + i]) << (8 * i);
    at += 4;
    return x;
  };
  for (std::uint32_t i = 0; i < *n; ++i) {
    auto len = rd();
    if (len == jb::kPathNull) {
      out.emplace_back(std::nullopt);
      continue;
    }
    out.emplace_back(std::string{reinterpret_cast<const char*>(p.data()) + at, len});
    at += len;
  }
  return out;
}

void append_u32(Value& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::byte>((x >> (8 * i)) & 0xFF));
}
auto bytes(std::initializer_list<int> b) -> Value {
  Value v;
  for (auto x : b) v.push_back(static_cast<std::byte>(x));
  return v;
}

}  // namespace

TEST_CASE("jsonb accessors: -> and ->> by key, by binary search over an object's sorted keys", "[sql][jsonb]") {
  const std::string_view doc = R"({"b": 1, "a": {"x": [1, 2]}, "ab": "s", "B": null, "": 0, "aaa": true, "é": 5})";
  CHECK(field(doc, "a") == "{\"x\": [1, 2]}");
  CHECK(field(doc, "b") == "1");
  CHECK(field(doc, "ab") == "\"s\"");
  CHECK(field(doc, "B") == "null");
  CHECK(field(doc, "") == "0");
  CHECK(field(doc, "aaa") == "true");
  CHECK(field(doc, "é") == "5");
  // Missing keys on every side of the order.
  for (auto k : {"c", "A", "aa", "aab", "zzzz", "\x01"}) {
    INFO(k);
    CHECK(field(doc, k) == std::nullopt);
  }
  // A key of no object: NULL, as jsonb_object_field.
  for (auto d : {"[1]", "5", "\"a\"", "null", "{}"}) {
    INFO(d);
    CHECK(field(d, "a") == std::nullopt);
  }
  // Every key of a larger object is found by the search.
  std::string big = "{";
  for (int i = 0; i < 300; ++i) big += (i == 0 ? "" : ", ") + std::string{"\"k"} + std::to_string(i) + "\": " + std::to_string(i);
  big += "}";
  for (int i = 0; i < 300; ++i) CHECK(field(big, "k" + std::to_string(i)) == std::to_string(i));
  CHECK(field(big, "k300") == std::nullopt);
}

TEST_CASE("jsonb accessors: -> by index, negative from the end, a scalar an array of itself", "[sql][jsonb]") {
  CHECK(element("[1, 2, 3]", 0) == "1");
  CHECK(element("[1, 2, 3]", 2) == "3");
  CHECK(element("[1, 2, 3]", -1) == "3");
  CHECK(element("[1, 2, 3]", -3) == "1");
  CHECK(element("[1, 2, 3]", -4) == std::nullopt);
  CHECK(element("[1, 2, 3]", 3) == std::nullopt);
  CHECK(element("[1, 2, 3]", std::numeric_limits<std::int64_t>::max()) == std::nullopt);
  CHECK(element("[1, 2, 3]", std::numeric_limits<std::int64_t>::min()) == std::nullopt);
  CHECK(element("[]", 0) == std::nullopt);
  CHECK(element("[]", -1) == std::nullopt);
  // PostgreSQL's raw scalar: '5' -> 0 and -> -1 are 5, -> 1 NULL.
  CHECK(element("5", 0) == "5");
  CHECK(element("5", -1) == "5");
  CHECK(element("5", 1) == std::nullopt);
  CHECK(element("\"x\"", 0) == "\"x\"");
  CHECK(element("null", 0) == "null");
  CHECK(element("{\"a\": 1}", 0) == std::nullopt);
}

TEST_CASE("jsonb accessors: #> walks a path as jsonb_get_element", "[sql][jsonb]") {
  const std::string_view doc = R"({"a": {"b": [1, 2]}, "a b": 1, "NULL": 2, "": 3})";
  CHECK(at_path(doc, "{a,b,1}") == "2");
  CHECK(at_path(doc, "{a,b,-1}") == "2");
  CHECK(at_path(doc, "{a,b,-3}") == std::nullopt);
  CHECK(at_path(doc, "{a,b,x}") == std::nullopt);
  CHECK(at_path(doc, "{a,b,1,0}") == std::nullopt);  // a scalar mid-path ends the walk
  CHECK(at_path(doc, "{}") == "{\"\": 3, \"a\": {\"b\": [1, 2]}, \"a b\": 1, \"NULL\": 2}");
  CHECK(at_path(doc, "{a,NULL}") == std::nullopt);
  CHECK(at_path(doc, "{NULL,a}") == std::nullopt);
  CHECK(at_path(doc, "{{a},{b}}") == "[1, 2]");  // a 2-D path read flat
  CHECK(at_path(doc, "[1:2]={a,b}") == "[1, 2]");
  CHECK(at_path(doc, "{ a b }") == "1");
  CHECK(at_path(doc, "{\"NULL\"}") == "2");
  CHECK(at_path(doc, "{\"\"}") == "3");
  // An index as strtoint reads it.
  CHECK(at_path("[1, 2]", "{ 1 }") == "2");
  CHECK(at_path("[1, 2]", "{+1}") == "2");
  CHECK(at_path("[1, 2]", "{01}") == "2");
  CHECK(at_path("[1, 2]", "{\" 1\"}") == "2");
  CHECK(at_path("[1, 2]", "{\"1 \"}") == std::nullopt);
  CHECK(at_path("[1, 2]", "{1x}") == std::nullopt);
  CHECK(at_path("[1, 2]", "{9999999999}") == std::nullopt);
  CHECK(at_path("[1, 2]", "{-9999999999}") == std::nullopt);
  CHECK(at_path("[1, 2]", "{-2147483648}") == std::nullopt);
  CHECK(at_path("[1, 2]", "{\"\"}") == std::nullopt);
  // The top-level scalar: the empty path gives it, any other NULL.
  CHECK(at_path("5", "{}") == "5");
  CHECK(at_path("5", "{0}") == std::nullopt);
  CHECK(at_path("\"s\"", "{}") == "\"s\"");
  // Depth: a path down 200 nested arrays and objects.
  std::string deep;
  std::string p = "{";
  for (int i = 0; i < 100; ++i) {
    deep += "{\"k\": [";
    p += std::string{i == 0 ? "" : ","} + "k,0";
  }
  deep += "7";
  for (int i = 0; i < 100; ++i) deep += "]}";
  p += "}";
  CHECK(at_path(deep, p) == "7");
  // The empty view is the empty path.
  Doc d{"[1]"};
  std::optional<jb::ByteView> found;
  REQUIRE(jb::at_path(d.node(), {}, found).ok());
  CHECK(found.has_value());
}

TEST_CASE("jsonb accessors: a path is read as array_in reads a text[], with its errors", "[sql][jsonb]") {
  using E = std::optional<std::string>;
  CHECK(elements(path("{a,b}")) == std::vector<E>{"a", "b"});
  CHECK(elements(path("  { a b , \"c d\" ,NULL, null,\"NULL\", \\NULL}  ")) ==
        std::vector<E>{"a b", "c d", std::nullopt, std::nullopt, "NULL", "NULL"});
  CHECK(elements(path("{\"a\\\"b\",a\\,b,a\\ }")) == std::vector<E>{"a\"b", "a,b", "a "});
  CHECK(elements(path("{}")) == std::vector<E>{});
  CHECK(elements(path("{{a,b},{c,d}}")) == std::vector<E>{"a", "b", "c", "d"});
  CHECK(elements(path("[0:1]={x,y}")) == std::vector<E>{"x", "y"});
  CHECK(elements(path("[1:1][1:2]={{x,y}}")) == std::vector<E>{"x", "y"});
  // array_in's errors, as PostgreSQL 17 words them.
  struct Bad {
    std::string_view text, state, detail;
  };
  for (const auto& b : {
           Bad{"a", "22P02", "Array value must start with \"{\" or dimension information."},
           Bad{"{a", "22P02", "Unexpected end of input."},
           Bad{"{{a,b},{c}}", "22P02", "Multidimensional arrays must have sub-arrays with matching dimensions."},
           Bad{"{{a},b}", "22P02", "Multidimensional arrays must have sub-arrays with matching dimensions."},
           Bad{"{a}}", "22P02", "Junk after closing right brace."},
           Bad{"{a} x", "22P02", "Junk after closing right brace."},
           Bad{"{\"a\"x}", "22P02", "Incorrectly quoted array element."},
           Bad{"{a\"b}", "22P02", "Incorrectly quoted array element."},
           Bad{"{a,}", "22P02", "Unexpected \"}\" character."},
           Bad{"{,a}", "22P02", "Unexpected \",\" character."},
           Bad{"{{a}b}", "22P02", "Unexpected array element."},
           Bad{"{a{b}}", "22P02", "Unexpected \"{\" character."},
           Bad{"[1:3]={a,b}", "22P02", "Specified array dimensions do not match array contents."},
           Bad{"[1:2]{a,b}", "22P02", "Missing \"=\" after array dimensions."},
           Bad{"[1:2={a,b}", "22P02", "Missing \"]\" after array dimensions."},
           Bad{"[]={a,b}", "22P02", "\"[\" must introduce explicitly-specified array dimensions."},
           Bad{"[1:]={a}", "22P02", "Missing array dimension value."},
           Bad{"[1:2]=a", "22P02", "Array contents must start with \"{\"."},
           Bad{"[3:1]={a,b}", "2202E", ""},
           Bad{"{{{{{{{a}}}}}}}", "54000", ""},
           Bad{"[1][1][1][1][1][1][1]={a}", "54000", ""},
           Bad{"[1:2147483647]={a}", "54000", ""},
           Bad{"[99999999999]={a,b}", "54000", ""},
       }) {
    INFO(b.text);
    auto e = path_error(b.text);
    CHECK(e.state == b.state);
    CHECK(e.detail == b.detail);
    if (b.state == "22P02") CHECK(e.message == "malformed array literal: \"" + std::string{b.text} + "\"");
  }
  CHECK(path_error("[3:1]={a,b}").message == "upper bound cannot be less than lower bound");
  CHECK(path_error("{{{{{{{a}}}}}}}").message == "number of array dimensions exceeds the maximum allowed (6)");
  CHECK(path_error("[1:2147483647]={a}").message == "array upper bound is too large: 2147483647");
  CHECK(path_error("[99999999999]={a,b}").message == "array bound is out of integer range");
}

TEST_CASE("jsonb accessors: bytes that are no path are refused before a walk", "[sql][jsonb]") {
  CHECK(jb::path_size({}) == 0u);
  CHECK(jb::path_size(path("{a,NULL}")) == 2u);
  Value p;
  append_u32(p, 1);
  CHECK(jb::path_size(p) == std::nullopt);  // an element missing
  append_u32(p, 5);
  p.push_back(std::byte{'a'});
  CHECK(jb::path_size(p) == std::nullopt);  // a length past the extent
  Value q;
  append_u32(q, 0);
  q.push_back(std::byte{0});
  CHECK(jb::path_size(q) == std::nullopt);  // bytes after the last element
  CHECK(jb::path_size(bytes({1, 0})) == std::nullopt);
  Doc d{"[1]"};
  std::optional<jb::ByteView> found;
  CHECK(jb::at_path(d.node(), p, found).status == jb::Status::Corrupt);
}

TEST_CASE("jsonb accessors: ->> is NULL for a JSON null, a string's own bytes, or the text", "[sql][jsonb]") {
  struct Case {
    std::string_view doc;
    jb::TextForm form;
    std::string_view text;
  };
  for (const auto& c : {Case{"null", jb::TextForm::Null, ""}, Case{"\"x\\ny\\u00e9\"", jb::TextForm::Bytes, "x\ny\xc3\xa9"},
                        Case{"\"\"", jb::TextForm::Bytes, ""}, Case{"1.50", jb::TextForm::Text, "1.50"},
                        Case{"true", jb::TextForm::Text, "true"}, Case{"[1, \"a\"]", jb::TextForm::Text, "[1, \"a\"]"},
                        Case{"{}", jb::TextForm::Text, "{}"}}) {
    INFO(c.doc);
    Doc d{c.doc};
    jb::TextForm form{};
    jb::ByteView b;
    REQUIRE(jb::text_form(d.node(), form, b).ok());
    CHECK(form == c.form);
    if (form == jb::TextForm::Bytes) {
      CHECK(std::string_view{reinterpret_cast<const char*>(b.data()), b.size()} == c.text);
      // A view of the stored bytes, with no copy.
      CHECK(b.data() == d.node().data() + 1);
    }
    if (form == jb::TextForm::Text) CHECK(text_of(d.node()) == c.text);
  }
}

TEST_CASE("jsonb accessors: jsonb_typeof and jsonb_array_length", "[sql][jsonb]") {
  struct Case {
    std::string_view doc, type;
  };
  for (const auto& c : {Case{"1", "number"}, Case{"\"a\"", "string"}, Case{"null", "null"}, Case{"true", "boolean"},
                        Case{"false", "boolean"}, Case{"[]", "array"}, Case{"{}", "object"}}) {
    Doc d{c.doc};
    jb::Type t{};
    REQUIRE(jb::type_of(d.node(), t).ok());
    CHECK(jb::typeof_name(t) == c.type);
  }
  std::int64_t n = -1;
  REQUIRE(jb::array_length(Doc{"[1, [2, 3], {}]"}.node(), n).ok());
  CHECK(n == 3);
  REQUIRE(jb::array_length(Doc{"[]"}.node(), n).ok());
  CHECK(n == 0);
  for (auto s : {"5", "null", "\"a\"", "true"}) {
    auto r = jb::array_length(Doc{s}.node(), n);
    REQUIRE(r.status == jb::Status::Data);
    CHECK(r.error->state.view() == "22023");
    CHECK(r.error->message == "cannot get array length of a scalar");
  }
  auto r = jb::array_length(Doc{"{}"}.node(), n);
  REQUIRE(r.status == jb::Status::Data);
  CHECK(r.error->message == "cannot get array length of a non-array");
}

TEST_CASE("jsonb accessors: the casts out round, range-check and refuse as PostgreSQL 17", "[sql][jsonb]") {
  auto integer = [](std::string_view doc) {
    std::optional<std::int64_t> v;
    REQUIRE(jb::to_integer(Doc{doc}.node(), "bigint", v).ok());
    return v;
  };
  CHECK(integer("1.5") == 2);
  CHECK(integer("-2.5") == -3);
  CHECK(integer("2.49") == 2);
  CHECK(integer("0.5") == 1);
  CHECK(integer("0.49") == 0);
  CHECK(integer("-0.5") == -1);
  CHECK(integer("0.0001") == 0);
  CHECK(integer("0") == 0);
  CHECK(integer("-0.0") == 0);
  CHECK(integer("1e3") == 1000);
  CHECK(integer("9223372036854775807") == std::numeric_limits<std::int64_t>::max());
  CHECK(integer("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
  CHECK(integer("-9223372036854775808.4") == std::numeric_limits<std::int64_t>::min());
  CHECK(integer("9223372036854775807.5") == std::nullopt);
  CHECK(integer("-9223372036854775808.5") == std::nullopt);
  CHECK(integer("1e19") == std::nullopt);
  CHECK(integer("1e131071") == std::nullopt);
  CHECK(integer("1e-16383") == 0);
  auto mismatch = [](std::string_view doc, std::string_view target) {
    std::optional<std::int64_t> v;
    auto r = jb::to_integer(Doc{doc}.node(), target, v);
    REQUIRE(r.status == jb::Status::Data);
    CHECK(r.error->state.view() == "22023");
    return r.error->message;
  };
  CHECK(mismatch("\"1\"", "integer") == "cannot cast jsonb string to type integer");
  CHECK(mismatch("null", "integer") == "cannot cast jsonb null to type integer");
  CHECK(mismatch("[1]", "integer") == "cannot cast jsonb array to type integer");
  CHECK(mismatch("{}", "bigint") == "cannot cast jsonb object to type bigint");
  CHECK(mismatch("true", "smallint") == "cannot cast jsonb boolean to type smallint");
  auto b = false;
  REQUIRE(jb::to_bool(Doc{"true"}.node(), b).ok());
  CHECK(b);
  REQUIRE(jb::to_bool(Doc{"false"}.node(), b).ok());
  CHECK_FALSE(b);
  auto br = jb::to_bool(Doc{"1"}.node(), b);
  REQUIRE(br.status == jb::Status::Data);
  CHECK(br.error->message == "cannot cast jsonb numeric to type boolean");
  auto flt = [](std::string_view doc, bool real) {
    Value buf;
    double d = 0;
    auto r = jb::to_float(Doc{doc}.node(), real, {}, jb::vector_out(buf), d);
    return std::pair{r, d};
  };
  CHECK(std::bit_cast<std::uint64_t>(flt("1.5", false).second) == std::bit_cast<std::uint64_t>(1.5));
  CHECK(std::bit_cast<std::uint64_t>(flt("0.1", true).second) == std::bit_cast<std::uint64_t>(static_cast<double>(0.1F)));
  CHECK(flt("1e-310", false).first.ok());  // a denormal is kept
  CHECK(flt("1e-40", true).first.ok());
  for (auto [doc, real, message] : {std::tuple{"1e400", false, "is out of range for type double precision"},
                                    std::tuple{"1e-400", false, "is out of range for type double precision"},
                                    std::tuple{"1e40", true, "is out of range for type real"},
                                    std::tuple{"1e-50", true, "is out of range for type real"}}) {
    INFO(doc);
    auto [r, d] = flt(doc, real);
    REQUIRE(r.status == jb::Status::Data);
    CHECK(r.error->state.view() == "22003");
    CHECK(r.error->message.starts_with("\""));
    CHECK(r.error->message.ends_with(message));
  }
  CHECK(flt("1e40", true).first.error->message == "\"10000000000000000000000000000000000000000\" is out of range for type real");
  CHECK(flt("\"x\"", true).first.error->message == "cannot cast jsonb string to type real");
  CHECK(flt("null", false).first.error->message == "cannot cast jsonb null to type double precision");
  Value buf;
  std::span<char> text;
  REQUIRE(jb::to_numeric(Doc{"1.50"}.node(), "numeric", {}, jb::vector_out(buf), text).ok());
  CHECK(std::string_view{text.data(), text.size()} == "1.50");
  auto nr = jb::to_numeric(Doc{"[]"}.node(), "numeric", {}, jb::vector_out(buf), text);
  REQUIRE(nr.status == jb::Status::Data);
  CHECK(nr.error->message == "cannot cast jsonb array to type numeric");
}

TEST_CASE("jsonb accessors: a damaged value is Corrupt at the read that meets it", "[sql][jsonb]") {
  // An object of two keys, "a" and "b", values 1 and 2 (tags 0x04 numbers
  // written as null/true here, one byte each).
  auto object = [](std::uint32_t k0, std::uint32_t k1, std::uint32_t v0, std::uint32_t v1) {
    Value v{std::byte{0x07}};
    append_u32(v, 2);
    append_u32(v, k0);
    append_u32(v, k1);
    append_u32(v, v0);
    append_u32(v, v1);
    v.push_back(std::byte{'a'});
    v.push_back(std::byte{'b'});
    v.push_back(std::byte{0x01});
    v.push_back(std::byte{0x03});
    return v;
  };
  std::optional<jb::ByteView> found;
  // The sound one.
  auto good = object(1, 2, 1, 2);
  REQUIRE(jb::field(good, "b", found).ok());
  REQUIRE(found.has_value());
  CHECK(text_of(*found) == "true");
  // A key end below its predecessor, met on the search.
  CHECK(jb::field(object(2, 1, 1, 2), "a", found).status == jb::Status::Corrupt);
  // A key end past the keys' area.
  CHECK(jb::field(object(1, 9, 1, 2), "b", found).status == jb::Status::Corrupt);
  // A value end past its area, or not the area's end for the last.
  CHECK(jb::field(object(1, 2, 1, 9), "b", found).status == jb::Status::Corrupt);
  CHECK(jb::field(object(1, 2, 1, 1), "b", found).status == jb::Status::Corrupt);
  CHECK(jb::field(object(1, 2, 0, 2), "a", found).status == jb::Status::Corrupt);  // an empty value
  // A count past the extent, in an object and an array.
  Value huge{std::byte{0x07}};
  append_u32(huge, 0xFFFFFFFF);
  CHECK(jb::field(huge, "a", found).status == jb::Status::Corrupt);
  huge[0] = std::byte{0x06};
  CHECK(jb::element(huge, 0, found).status == jb::Status::Corrupt);
  // An array element end past its area; an empty container with bytes after.
  Value arr{std::byte{0x06}};
  append_u32(arr, 1);
  append_u32(arr, 7);
  arr.push_back(std::byte{0x01});
  CHECK(jb::element(arr, 0, found).status == jb::Status::Corrupt);
  CHECK(jb::element(arr, -1, found).status == jb::Status::Corrupt);
  CHECK(jb::element(bytes({0x06, 0, 0, 0, 0, 0x01}), 0, found).status == jb::Status::Corrupt);
  // An unknown tag, an empty node.
  CHECK(jb::field(bytes({0x09}), "a", found).status == jb::Status::Corrupt);
  CHECK(jb::element(Value{}, 0, found).status == jb::Status::Corrupt);
  jb::Type t{};
  CHECK(jb::type_of(Value{}, t).status == jb::Status::Corrupt);
  // A string that is no UTF-8, or holds a NUL byte, to ->>.
  jb::TextForm form{};
  jb::ByteView b;
  CHECK(jb::text_form(bytes({0x05, 0xff}), form, b).status == jb::Status::Corrupt);
  CHECK(jb::text_form(bytes({0x05, 'a', 0x00}), form, b).status == jb::Status::Corrupt);
  // A number with a leading zero digit, to a cast.
  std::optional<std::int64_t> v;
  CHECK(jb::to_integer(bytes({0x04, 1, 0, 0, 0, 0, 0, 0, '0', '1'}), "integer", v).status == jb::Status::Corrupt);
  auto flag = false;
  CHECK(jb::to_bool(bytes({0x03, 0x00}), flag).status == jb::Status::Corrupt);
  // A path whose walk meets a damaged step.
  auto p = path("{b}");
  CHECK(jb::at_path(object(2, 1, 1, 2), p, found).status == jb::Status::Corrupt);
}

namespace {

// A Memory over a budget, which marks which buffer a failing allocation of
// a sweep was for: the walk's stack (jb::Site::Stack) or the bytes written
// through Out (kOutSite).
constexpr std::size_t kOutSite = 4;
constexpr std::size_t kSites = 5;
static_assert(static_cast<std::size_t>(jb::Site::Stack) + 1 == kOutSite);

struct Budget {
  std::uint64_t limit;
  std::uint64_t used{0};
#ifdef BYTECASK_NO_ALLOC_FAULTS
  // The seam is not built under this sanitizer: no sweep marks a site.
  void note(std::size_t) const {}
#else
  const bytecask::testing::ScopedAllocFaults* faults{nullptr};
  long nth{0};
  std::array<bool, kSites>* failed_at{nullptr};
  void note(std::size_t site) const {
    if (faults != nullptr && faults->allocations() + 1 == nth) (*failed_at)[site] = true;
  }
#endif
  static auto take(void* self, std::uint64_t n, jb::Site site) -> bool {
    auto& b = *static_cast<Budget*>(self);
    b.note(static_cast<std::size_t>(site));
    return charge(b, n);
  }
  static auto charge(Budget& b, std::uint64_t n) -> bool {
    if (n > b.limit - std::min(b.used, b.limit)) return false;
    b.used += n;
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

// The calls of stage 3 that allocate: a path read, a number's text, a
// float read through its text. Each returns its result as text.
enum class Call : std::uint8_t { Path, Numeric, Float };

auto call(Call c, Budget& b, Out& out, jb::ByteView node) -> std::pair<jb::Outcome, std::string> {
  switch (c) {
    case Call::Path: {
      std::span<std::byte> p;
      auto r = jb::parse_path("{a, \"b c\", NULL, d\\,e}", {&out, Out::alloc}, p);
      return {r, r.ok() ? std::string{reinterpret_cast<const char*>(p.data()), p.size()} : ""};
    }
    case Call::Numeric: {
      std::span<char> text;
      auto r = jb::to_numeric(node, "numeric", b.memory(), {&out, Out::alloc}, text);
      return {r, r.ok() ? std::string{text.data(), text.size()} : ""};
    }
    case Call::Float: {
      double d = 0;
      auto r = jb::to_float(node, false, b.memory(), {&out, Out::alloc}, d);
      return {r, r.ok() ? std::to_string(d) : ""};
    }
  }
  return {};
}

}  // namespace

TEST_CASE("jsonb accessors: a budget sweep of the calls that allocate gives a refusal or the full result", "[sql][jsonb]") {
  Doc number{"-123456789.125e3"};
  for (auto c : {Call::Path, Call::Numeric, Call::Float}) {
    INFO(static_cast<int>(c));
    Budget open{1u << 20};
    Out open_out{&open, {}};
    auto [ok, want] = call(c, open, open_out, number.node());
    REQUIRE(ok.ok());
    auto refusals = 0;
    for (std::uint64_t limit = 0;; ++limit) {
      Budget b{limit};
      Out out{&b, {}};
      auto [r, got] = call(c, b, out, number.node());
      if (!r.ok()) {
        REQUIRE(r.status == jb::Status::Refused);
        ++refusals;
        continue;
      }
      CHECK(got == want);
      break;
    }
    CHECK(refusals > 0);
  }
}

TEST_CASE("jsonb accessors: every allocation of the calls that allocate fails in turn as bad_alloc", "[sql][jsonb][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Doc number{"[-123456789.125e3]"};
  std::optional<jb::ByteView> first;
  REQUIRE(jb::element(number.node(), 0, first).ok());
  // The lookups allocate nothing: a whole run fires no fault.
  {
    Doc object{"{\"a\": 1}"};
    bytecask::testing::ScopedAllocFaults faults{1, false};
    std::optional<jb::ByteView> found;
    auto ok = jb::element(number.node(), -1, found).ok() && jb::field(object.node(), "a", found).ok();
    auto fired = faults.report().fired;
    CHECK(ok);
    CHECK_FALSE(fired);
  }
  // Which buffers each call's sweep must fail: what it writes through Out
  // (a number's walk holds no container, so it keeps no stack).
  struct Want {
    Call call;
    std::array<bool, kSites> sites;
  };
  for (const auto& w : {Want{Call::Path, {false, false, false, false, true}}, Want{Call::Numeric, {false, false, false, false, true}},
                        Want{Call::Float, {false, false, false, false, true}}}) {
    INFO(static_cast<int>(w.call));
    std::array<bool, kSites> failed{};
    long failures = 0;
    for (long nth = 1;; ++nth) {
      REQUIRE(nth < 10000);
      Budget b{1u << 20};
      Out out{&b, {}};
      auto threw = false;
      {
        bytecask::testing::ScopedAllocFaults faults{nth, false};
        b.faults = &faults;
        b.nth = nth;
        b.failed_at = &failed;
        try {
          auto [r, got] = call(w.call, b, out, *first);
          REQUIRE(r.ok());
        } catch (const std::bad_alloc&) {
          threw = true;
        }
        if (!faults.report().fired) {
          REQUIRE_FALSE(threw);
          break;
        }
      }
      REQUIRE(threw);
      ++failures;
    }
    CHECK(failures > 0);
    CHECK(failed == w.sites);
  }
#endif
}

TEST_CASE("jsonb accessors: path_append grows a path as parse_path writes it", "[sql][jsonb]") {
  Value p;
  auto add = [&](std::optional<std::string> e) {
    auto view = e ? std::optional<jb::ByteView>{std::as_bytes(std::span{e->data(), e->size()})} : std::nullopt;
    auto n = jb::path_size(p);
    REQUIRE(n.has_value());
    auto old = p.size();
    p.resize(jb::path_append_size(p, view));
    jb::path_append(jb::ByteView{p.data(), old}, *n, view, p);  // in place
  };
  add("a");
  add(std::nullopt);
  add("");
  add("bc");
  CHECK(p == path("{a,NULL,\"\",bc}"));
  CHECK(elements(p) == std::vector<std::optional<std::string>>{"a", std::nullopt, "", "bc"});
}
