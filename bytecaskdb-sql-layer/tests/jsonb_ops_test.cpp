// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// jsonb, stage 4 (docs/jsonb_design.md, *Tests of jsonb by stage*): the
// containment, existence, modification and construction of sql/jsonb.cppm
// over nodes, each result recorded from PostgreSQL 17: @> over nested
// values, arrays of scalars and top-level scalars; ? and ?| and ?&; ||'s
// merge, join and wrapping; - by key, index and text[]; #-, jsonb_set,
// jsonb_insert and jsonb_set_lax over setPath's rules and errors;
// jsonb_strip_nulls; jsonb_pretty; to_jsonb's scalars and the build
// functions' key rules. A result's depth (54001) and size (54000) checked
// before it is written; a damaged value Corrupt; a result that changes
// nothing its input, not a copy; and the budget and allocation-failure
// sweeps of every call that allocates, asserting the buffers they reach.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tests/alloc_faults.h"

import bytecask.sql.jsonb;

using namespace bytecask::sql;
namespace jb = bytecask::sql::jsonb;

namespace {

using Value = std::vector<std::byte>;

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

auto path(std::string_view text) -> Value {
  Value out;
  std::span<std::byte> p;
  REQUIRE(jb::parse_path(text, jb::vector_out(out), p).ok());
  return {p.begin(), p.end()};
}

// A call's error as "SQLSTATE message".
auto error_text(const jb::Outcome& r) -> std::string {
  if (r.status == jb::Status::Data) return std::string{r.error->state.view()} + " " + r.error->message;
  if (r.status == jb::Status::Corrupt) return "corrupt";
  return "refused";
}

using Build = std::function<jb::Outcome(jb::Alloc, jb::ByteView&)>;

auto built(const Build& f) -> std::string {
  Value out;
  jb::ByteView r;
  auto o = f(jb::vector_out(out), r);
  if (!o.ok()) return error_text(o);
  return text_of(r);
}

auto contains(std::string_view a, std::string_view b) -> bool {
  Doc x{a};
  Doc y{b};
  auto v = false;
  REQUIRE(jb::contains(x.node(), y.node(), {}, v).ok());
  return v;
}

auto exists(std::string_view a, std::string_view k) -> bool {
  Doc x{a};
  auto v = false;
  REQUIRE(jb::exists(x.node(), k, v).ok());
  return v;
}

auto exists_some(std::string_view a, std::string_view p, bool all) -> bool {
  Doc x{a};
  auto v = false;
  REQUIRE(jb::exists_some(x.node(), path(p), all, v).ok());
  return v;
}

auto concat(std::string_view a, std::string_view b) -> std::string {
  Doc x{a};
  Doc y{b};
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::concat(x.node(), y.node(), {}, out, r); });
}

auto minus_key(std::string_view a, std::string_view k) -> std::string {
  Doc x{a};
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::delete_key(x.node(), k, {}, out, r); });
}

auto minus_index(std::string_view a, std::int64_t i) -> std::string {
  Doc x{a};
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::delete_index(x.node(), i, {}, out, r); });
}

auto minus_keys(std::string_view a, std::string_view p) -> std::string {
  Doc x{a};
  auto bytes = path(p);
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::delete_keys(x.node(), bytes, {}, out, r); });
}

auto minus_path(std::string_view a, std::string_view p) -> std::string {
  Doc x{a};
  auto bytes = path(p);
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::delete_path(x.node(), bytes, {}, out, r); });
}

auto set(std::string_view a, std::string_view p, std::string_view v, bool create = true) -> std::string {
  Doc x{a};
  Doc y{v};
  auto bytes = path(p);
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::set(x.node(), bytes, y.node(), create, {}, out, r); });
}

auto insert(std::string_view a, std::string_view p, std::string_view v, bool after = false) -> std::string {
  Doc x{a};
  Doc y{v};
  auto bytes = path(p);
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::insert(x.node(), bytes, y.node(), after, {}, out, r); });
}

auto strip(std::string_view a) -> std::string {
  Doc x{a};
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::strip_nulls(x.node(), {}, out, r); });
}

auto pretty(std::string_view a) -> std::string {
  Doc x{a};
  Value out;
  std::span<char> t;
  REQUIRE(jb::pretty(x.node(), {}, jb::vector_out(out), t).ok());
  return {t.data(), t.size()};
}

auto scalar(jb::Scalar kind, std::string_view text) -> std::string {
  return built([&](jb::Alloc out, jb::ByteView& r) { return jb::scalar(kind, text, {}, out, r); });
}

// An argument of a build: a document, SQL NULL (nullptr) or a jsonb key.
struct Arg {
  const char* doc;
  bool composite{false};
};

auto build(bool object, std::initializer_list<Arg> args) -> std::string {
  std::vector<Doc> docs;
  docs.reserve(args.size());
  std::vector<jb::BuildArg> in;
  for (const auto& a : args) {
    jb::BuildArg b;
    b.null = a.doc == nullptr;
    b.composite = a.composite;
    if (!b.null) {
      docs.emplace_back(a.doc);
      b.node = docs.back().node();
    }
    in.push_back(b);
  }
  return built([&](jb::Alloc out, jb::ByteView& r) {
    return object ? jb::build_object(in, {}, out, r) : jb::build_array(in, {}, out, r);
  });
}

}  // namespace

TEST_CASE("jsonb ops: @> as JsonbDeepContains, nested values, arrays of scalars and a top-level scalar", "[sql][jsonb]") {
  CHECK(contains(R"({"a":[1,2],"b":1})", R"({"a":[2]})"));
  CHECK(contains("[1,2,3]", "[3,1,1]"));
  CHECK(contains("[1]", "1"));
  CHECK_FALSE(contains("1", "[1]"));
  CHECK(contains("1", "1"));
  CHECK(contains("1.0", "1"));
  CHECK_FALSE(contains("[[1,2]]", "[1]"));
  CHECK(contains("[1,[2,3]]", "[[3]]"));
  CHECK_FALSE(contains(R"({"a":[1]})", R"({"a":1})"));
  CHECK(contains(R"({"a":{"b":1,"c":2}})", R"({"a":{"b":1}})"));
  CHECK_FALSE(contains(R"({"a":{"b":1}})", R"({"a":[]})"));
  CHECK(contains(R"([1,[2,{"x":1}]])", "[[{}]]"));
  CHECK_FALSE(contains("[]", "{}"));
  CHECK_FALSE(contains("{}", "[]"));
  CHECK(contains("{}", "{}"));
  CHECK(contains("[]", "[]"));
  CHECK_FALSE(contains("[true]", "[false]"));
  CHECK_FALSE(contains(R"(["1"])", "[1]"));
  CHECK(contains(R"([{"a":1},{"b":2}])", R"([{"b":2},{"a":1}])"));
  CHECK_FALSE(contains(R"([{"a":1},{"b":2}])", R"([{"a":1,"b":2}])"));
}

TEST_CASE("jsonb ops: ? on top-level keys and string elements, ?| and ?& skip NULL elements", "[sql][jsonb]") {
  CHECK(exists(R"({"a":1})", "a"));
  CHECK_FALSE(exists(R"({"a":{"b":1}})", "b"));
  CHECK(exists(R"(["a",1])", "a"));
  CHECK_FALSE(exists(R"(["a",1])", "1"));
  CHECK(exists(R"("a")", "a"));
  CHECK_FALSE(exists("1", "1"));
  CHECK(exists(R"({"":1})", ""));
  CHECK(exists_some(R"({"a":1})", "{b,a}", false));
  CHECK_FALSE(exists_some(R"({"a":1})", "{b,NULL}", false));
  CHECK(exists_some(R"({"a":1})", "{a,NULL}", true));
  CHECK(exists_some(R"({"a":1})", "{}", true));
  CHECK_FALSE(exists_some(R"({"a":1})", "{}", false));
  CHECK(exists_some(R"({"a":1,"b":2})", "{{a},{b}}", true));  // read flat
}

TEST_CASE("jsonb ops: || merges objects, joins arrays and wraps the rest", "[sql][jsonb]") {
  CHECK(concat(R"({"a":1})", R"({"a":2,"b":3})") == R"({"a": 2, "b": 3})");
  CHECK(concat("[1]", "[2,[3]]") == "[1, 2, [3]]");
  CHECK(concat("1", "2") == "[1, 2]");
  CHECK(concat(R"({"a":1})", "[2]") == R"([{"a": 1}, 2])");
  CHECK(concat("[1]", R"({"a":1})") == R"([1, {"a": 1}])");
  CHECK(concat(R"({"a":1})", "1") == R"([{"a": 1}, 1])");
  CHECK(concat("[]", "{}") == "[{}]");
  CHECK(concat("{}", "{}") == "{}");
  CHECK(concat("[]", "1") == "[1]");
  CHECK(concat(R"({"b":1,"aa":2})", R"({"a":0})") == R"({"a": 0, "b": 1, "aa": 2})");
}

TEST_CASE("jsonb ops: - by key, index and text[], with PostgreSQL's errors", "[sql][jsonb]") {
  CHECK(minus_key(R"({"a":1,"b":2})", "a") == R"({"b": 2})");
  CHECK(minus_key(R"(["a","b","a",1])", "a") == R"(["b", 1])");
  CHECK(minus_key(R"({"a":1})", "x") == R"({"a": 1})");
  CHECK(minus_key("[]", "x") == "[]");
  CHECK(minus_key("1", "a") == "22023 cannot delete from scalar");
  CHECK(minus_index("[1,2,3]", 1) == "[1, 3]");
  CHECK(minus_index("[1,2,3]", -1) == "[1, 2]");
  CHECK(minus_index("[1,2,3]", -4) == "[1, 2, 3]");
  CHECK(minus_index("[1,2,3]", 3) == "[1, 2, 3]");
  CHECK(minus_index("[1,2,3]", INT64_MIN) == "[1, 2, 3]");
  CHECK(minus_index(R"({"a":1})", 0) == "22023 cannot delete from object using integer index");
  CHECK(minus_index("\"a\"", 0) == "22023 cannot delete from scalar");
  CHECK(minus_keys(R"({"a":1,"b":2,"c":3})", "{a,NULL,c}") == R"({"b": 2})");
  CHECK(minus_keys(R"(["a","b"])", "{b}") == R"(["a"])");
  CHECK(minus_keys("1", "{}") == "22023 cannot delete from scalar");
  CHECK(minus_keys(R"({"a":1})", "{{a},{b}}") == "2202E wrong number of array subscripts");
}

TEST_CASE("jsonb ops: #-, jsonb_set and jsonb_insert follow a path as setPath", "[sql][jsonb]") {
  CHECK(minus_path(R"({"a":{"b":1,"c":2}})", "{a,b}") == R"({"a": {"c": 2}})");
  CHECK(minus_path(R"({"a":[1,2]})", "{a,-1}") == R"({"a": [1]})");
  CHECK(minus_path(R"({"a":1})", "{a,b}") == R"({"a": 1})");
  CHECK(minus_path("{}", "{NULL}") == "{}");
  CHECK(minus_path(R"({"a":1})", "{NULL}") == "22004 path element at position 1 is null");
  CHECK(minus_path("[1,2]", "{x}") == "22P02 path element at position 1 is not an integer: \"x\"");
  CHECK(minus_path("1", "{a}") == "22023 cannot delete path in scalar");
  CHECK(minus_path("[1]", "{{0},{1}}") == "2202E wrong number of array subscripts");
  CHECK(set(R"({"a":[1,2]})", "{a,0}", "9") == R"({"a": [9, 2]})");
  CHECK(set(R"({"a":1})", "{a,b}", "2") == R"({"a": 1})");
  CHECK(set(R"({"a":1})", "{b}", "2") == R"({"a": 1, "b": 2})");
  CHECK(set(R"({"a":1})", "{b}", "2", false) == R"({"a": 1})");
  CHECK(set("[1,2]", "{-5}", "0") == "[0, 1, 2]");
  CHECK(set("[1,2]", "{9}", "3") == "[1, 2, 3]");
  CHECK(set("[1,2]", "{9}", "3", false) == "[1, 2]");
  CHECK(set("[]", "{0}", "1", false) == "[]");
  CHECK(set("[]", "{-1}", "1") == "[1]");
  CHECK(set(R"({"a":{}})", "{a,x}", R"({"y":[]})") == R"({"a": {"x": {"y": []}}})");
  CHECK(set(R"({"a":1})", "{a,NULL}", "2") == "22004 path element at position 2 is null");
  CHECK(set(R"({"a":1})", "{b,NULL}", "2") == R"({"a": 1})");
  CHECK(set("[1]", "{ +0}", "2") == "[2]");
  CHECK(set("[1]", "{2147483648}", "2") == "22P02 path element at position 1 is not an integer: \"2147483648\"");
  CHECK(set("1", "{a}", "2") == "22023 cannot set path in scalar");
  CHECK(set(R"({"a":1})", "{}", "2") == R"({"a": 1})");
  CHECK(insert("[1,2]", "{0}", "0") == "[0, 1, 2]");
  CHECK(insert("[1,2]", "{1}", "9", true) == "[1, 2, 9]");
  CHECK(insert("[1,2]", "{-9}", "0", true) == "[0, 1, 2]");
  CHECK(insert("[1,2]", "{9}", "3") == "[1, 2, 3]");
  CHECK(insert(R"({"a":1})", "{b}", "2") == R"({"a": 1, "b": 2})");
  CHECK(insert(R"({"a":1})", "{a}", "2") == "22023 cannot replace existing key");
  CHECK(insert("1", "{0}", "2") == "22023 cannot set path in scalar");
}

TEST_CASE("jsonb ops: jsonb_set_lax's treatments and their errors", "[sql][jsonb]") {
  jb::NullTreatment t{};
  CHECK(jb::null_treatment("delete_key", t).ok());
  CHECK(t == jb::NullTreatment::DeleteKey);
  CHECK(jb::null_treatment("return_target", t).ok());
  CHECK(t == jb::NullTreatment::ReturnTarget);
  CHECK(jb::null_treatment("use_json_null", t).ok());
  CHECK(jb::null_treatment("raise_exception", t).ok());
  CHECK(error_text(jb::null_treatment("Delete_Key", t)) ==
        R"(22023 null_value_treatment must be "delete_key", "return_target", "use_json_null", or "raise_exception")");
  CHECK(error_text(jb::null_treatment(std::nullopt, t)).starts_with("22023 "));
  auto e = jb::null_value_error();
  CHECK(e.state.view() == "22004");
  CHECK(e.detail == R"(Exception was raised because null_value_treatment is "raise_exception".)");
  CHECK(e.hint == "To avoid, either change the null_value_treatment argument or ensure that an SQL NULL is not passed.");
}

TEST_CASE("jsonb ops: a result that changes nothing is its input, not a copy", "[sql][jsonb]") {
  Doc x{R"({"a":[1,2]})"};
  Value out;
  jb::ByteView r;
  auto p = path("{b,c}");
  REQUIRE(jb::set(x.node(), p, x.node(), true, {}, jb::vector_out(out), r).ok());
  CHECK(r.data() == x.node().data());
  CHECK(out.empty());
  REQUIRE(jb::delete_key(x.node(), "zz", {}, jb::vector_out(out), r).ok());
  CHECK(r.data() == x.node().data());
  Doc s{"5"};
  REQUIRE(jb::strip_nulls(s.node(), {}, jb::vector_out(out), r).ok());
  CHECK(r.data() == s.node().data());
  CHECK(out.empty());
}

TEST_CASE("jsonb ops: jsonb_strip_nulls and jsonb_pretty", "[sql][jsonb]") {
  CHECK(strip(R"([null,{"a":null,"b":{"c":null}}])") == R"([null, {"b": {}}])");
  CHECK(strip(R"({"a":null})") == "{}");
  CHECK(strip("null") == "null");
  CHECK(pretty("{}") == "{\n}");
  CHECK(pretty("[]") == "[\n]");
  CHECK(pretty("1") == "1");
  CHECK(pretty("\"a\"") == "\"a\"");
  CHECK(pretty(R"({"a":[1,{"b":[]},{}],"c":{}})") ==
        "{\n    \"a\": [\n        1,\n        {\n            \"b\": [\n            ]\n        },\n        {\n        }\n    ],\n"
        "    \"c\": {\n    }\n}");
}

TEST_CASE("jsonb ops: to_jsonb's scalars and the build functions' rules", "[sql][jsonb]") {
  CHECK(scalar(jb::Scalar::Number, "1.50") == "1.50");
  CHECK(scalar(jb::Scalar::Number, "1e+100").size() == 101);
  CHECK(scalar(jb::Scalar::Number, "-0") == "0");
  CHECK(scalar(jb::Scalar::Number, "NaN") == "\"NaN\"");
  CHECK(scalar(jb::Scalar::Number, "-Infinity") == "\"-Infinity\"");
  CHECK(scalar(jb::Scalar::String, "a\"\n") == R"("a\"\n")");
  CHECK(scalar(jb::Scalar::String, "\xff") == "22021 invalid byte sequence for encoding \"UTF8\"");
  CHECK(scalar(jb::Scalar::True, "") == "true");
  CHECK(build(false, {}) == "[]");
  CHECK(build(true, {}) == "{}");
  CHECK(build(false, {{"1"}, {nullptr}, {"\"x\""}, {"[1]"}}) == R"([1, null, "x", [1]])");
  CHECK(build(true, {{"1"}, {"\"a\""}, {"true"}, {"2"}, {"1.50"}, {nullptr}}) == R"({"1": "a", "1.50": null, "true": 2})");
  CHECK(build(true, {{"\"a\""}, {"1"}, {"\"a\""}, {"2"}}) == R"({"a": 2})");
  CHECK(build(true, {{"\"a\""}}) == "22023 argument list must have even number of elements");
  CHECK(build(true, {{"\"a\""}, {"1"}, {nullptr}, {"1"}}) == "22023 argument 3: key must not be null");
  CHECK(build(true, {{"\"a\"", true}, {"1"}}) == "22023 key value must be scalar, not array, composite, or json");
}

TEST_CASE("jsonb ops: a result past depth 10000 is 54001 and past 2^28 - 1 bytes 54000, before it is written", "[sql][jsonb]") {
  std::string deep(10000, '[');
  deep += std::string(10000, ']');
  Doc d{deep};
  Value out;
  jb::ByteView r;
  std::array<jb::BuildArg, 1> one{};
  one[0].node = d.node();
  CHECK(error_text(jb::build_array(one, {}, jb::vector_out(out), r)) == "54001 stack depth limit exceeded");
  auto p = path("{0}");
  CHECK(error_text(jb::set(d.node(), p, d.node(), true, {}, jb::vector_out(out), r)) == "54001 stack depth limit exceeded");
  CHECK(error_text(jb::insert(d.node(), p, d.node(), false, {}, jb::vector_out(out), r)) == "54001 stack depth limit exceeded");
  Doc obj{"{\"a\": " + deep.substr(1, 9999) + std::string(9999, ']') + "}"};
  Doc empty{"[]"};
  CHECK(error_text(jb::concat(obj.node(), empty.node(), {}, jb::vector_out(out), r)) == "54001 stack depth limit exceeded");
  CHECK(out.empty());
  // A value of 45 strings of 3 MB: 135 MB, joined to itself past the bound.
  std::string big(3'000'000, 'x');
  Value s;
  jb::ByteView sn;
  REQUIRE(jb::scalar(jb::Scalar::String, big, {}, jb::vector_out(s), sn).ok());
  std::vector<jb::BuildArg> args(45);
  for (auto& a : args) a.node = sn;
  Value half;
  jb::ByteView h;
  REQUIRE(jb::build_array(args, {}, jb::vector_out(half), h).ok());
  args.resize(90, args[0]);
  CHECK(error_text(jb::build_array(args, {}, jb::vector_out(out), r)) ==
        "54000 total size of jsonb array elements exceeds the maximum of 268435455 bytes");
  CHECK(error_text(jb::concat(h, h, {}, jb::vector_out(out), r)) ==
        "54000 total size of jsonb array elements exceeds the maximum of 268435455 bytes");
  auto end = path("{45}");
  CHECK(error_text(jb::set(h, end, h, true, {}, jb::vector_out(out), r)) ==
        "54000 total size of jsonb array elements exceeds the maximum of 268435455 bytes");
  CHECK(out.empty());
}

TEST_CASE("jsonb ops: a damaged value is Corrupt, never a read outside it", "[sql][jsonb]") {
  auto raw = [](std::initializer_list<int> b) {
    Value v;
    for (auto x : b) v.push_back(static_cast<std::byte>(x));
    return v;
  };
  // An array whose element end runs past its area; an object whose key end
  // runs past its keys.
  auto bad_array = raw({0x06, 1, 0, 0, 0, 9, 0, 0, 0, 0x01});
  auto bad_object = raw({0x07, 1, 0, 0, 0, 9, 0, 0, 0, 1, 0, 0, 0, 'a', 0x01});
  Doc good_object{R"({"a":1})"};
  Doc good_array{"[1]"};
  Value out;
  jb::ByteView r;
  auto v = false;
  auto p = path("{0,0}");
  for (const auto* bad : {&bad_array, &bad_object}) {
    const auto& good = bad == &bad_array ? good_array : good_object;
    CHECK(jb::contains(*bad, good.node(), {}, v).status == jb::Status::Corrupt);
    CHECK(jb::concat(*bad, good.node(), {}, jb::vector_out(out), r).status == jb::Status::Corrupt);
    CHECK(jb::strip_nulls(*bad, {}, jb::vector_out(out), r).status == jb::Status::Corrupt);
    CHECK(jb::delete_path(*bad, p, {}, jb::vector_out(out), r).status == jb::Status::Corrupt);
    std::span<char> t;
    CHECK(jb::pretty(*bad, {}, jb::vector_out(out), t).status == jb::Status::Corrupt);
    std::array<jb::BuildArg, 1> one{};
    one[0].node = *bad;
    CHECK(jb::build_array(one, {}, jb::vector_out(out), r).status == jb::Status::Corrupt);
  }
  CHECK(jb::exists(bad_array, "a", v).status == jb::Status::Corrupt);
  CHECK(jb::delete_key(bad_array, "a", {}, jb::vector_out(out), r).status == jb::Status::Corrupt);
  CHECK(jb::contains(good_object.node(), raw({0x07, 1, 0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0, 'a', 0x01}), {}, v).status ==
        jb::Status::Corrupt);
  // A number whose digits pass its scale, compared as a scalar.
  auto bad_number = raw({0x04, 1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, '1', '2'});
  Doc one_number{"[1]"};
  CHECK(jb::contains(one_number.node(), bad_number, {}, v).status == jb::Status::Corrupt);
}

namespace {

// A Memory over a budget, which marks the buffer a failing allocation of a
// sweep was for: a jb::Site or the result written through Out (kOutSite).
constexpr std::size_t kOutSite = 4;
constexpr std::size_t kSites = 5;
static_assert(static_cast<std::size_t>(jb::Site::Stack) + 1 == kOutSite);

struct Budget {
  std::uint64_t limit;
  std::uint64_t used{0};
#ifdef BYTECASK_NO_ALLOC_FAULTS
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

// The calls of stage 4 that allocate, each over fixed documents, its
// result as text.
enum class Call : std::uint8_t { Contains, Concat, Set, DeleteKeys, Strip, Pretty, BuildObject };

struct Docs {
  Doc a{R"({"b": [1, 2.50, {"z": null, "a": "é\n"}], "a": -0.001, "n": null})"};
  Doc b{R"({"b": [{"a": "é\n"}], "c": [true]})"};
  Doc num{"1.5"};
  Value p = path("{b,2,z}");
  Value keys = path("{a,zz,n}");
};

// `text`: the result as text, which allocates; a sweep under faults takes
// the outcome alone.
auto call(Call c, const Docs& d, Budget& b, Out& out, bool text = true) -> std::pair<jb::Outcome, std::string> {
  jb::ByteView r;
  jb::Alloc alloc{&out, Out::alloc};
  auto as_text = [&](const jb::Outcome& o) -> std::pair<jb::Outcome, std::string> {
    if (!o.ok() || !text) return {o, ""};
    std::string t;
    REQUIRE(jb::node_text(r, t).ok());
    return {o, t};
  };
  switch (c) {
    case Call::Contains: {
      auto v = false;
      auto o = jb::contains(d.a.node(), d.b.node(), b.memory(), v);
      return {o, v ? "t" : "f"};
    }
    case Call::Concat: return as_text(jb::concat(d.a.node(), d.b.node(), b.memory(), alloc, r));
    case Call::Set: return as_text(jb::set(d.a.node(), d.p, d.b.node(), true, b.memory(), alloc, r));
    case Call::DeleteKeys: return as_text(jb::delete_keys(d.a.node(), d.keys, b.memory(), alloc, r));
    case Call::Strip: return as_text(jb::strip_nulls(d.a.node(), b.memory(), alloc, r));
    case Call::Pretty: {
      std::span<char> t;
      auto o = jb::pretty(d.a.node(), b.memory(), alloc, t);
      return {o, o.ok() && text ? std::string{t.data(), t.size()} : ""};
    }
    case Call::BuildObject: {
      std::array<jb::BuildArg, 4> args{};
      args[0].node = d.num.node();
      args[1].node = d.a.node();
      args[2].node = d.num.node();
      args[3].node = d.b.node();
      return as_text(jb::build_object(args, b.memory(), alloc, r));
    }
  }
  return {};
}

}  // namespace

TEST_CASE("jsonb ops: a budget sweep of the calls that allocate gives a refusal or the full result", "[sql][jsonb]") {
  Docs d;
  for (auto c : {Call::Contains, Call::Concat, Call::Set, Call::DeleteKeys, Call::Strip, Call::Pretty, Call::BuildObject}) {
    INFO(static_cast<int>(c));
    Budget open{1u << 20};
    Out open_out{&open, {}};
    auto [ok, want] = call(c, d, open, open_out);
    REQUIRE(ok.ok());
    auto refusals = 0;
    for (std::uint64_t limit = 0;; ++limit) {
      Budget b{limit};
      Out out{&b, {}};
      auto [r, got] = call(c, d, b, out);
      if (!r.ok()) {
        REQUIRE(r.status == jb::Status::Refused);
        ++refusals;
        continue;
      }
      CHECK(got == want);
      CHECK(b.used == out.buf.size());  // every working buffer released
      break;
    }
    CHECK(refusals > 0);
  }
}

TEST_CASE("jsonb ops: every allocation of the calls that allocate fails in turn as bad_alloc", "[sql][jsonb][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Docs d;
  // Which buffers each call's sweep must fail: a builder's items (Nodes),
  // its members' order (Keys), its key pool (Pool: a number's text as a
  // key), its open containers and a walk's stack (Stack), and the result
  // written through Out.
  struct Want {
    Call call;
    std::array<bool, kSites> sites;
  };
  for (const auto& w : {Want{Call::Contains, {false, false, false, true, false}},
                        Want{Call::Concat, {true, true, false, true, true}},
                        Want{Call::Set, {true, true, false, true, true}},
                        Want{Call::DeleteKeys, {true, true, false, true, true}},
                        Want{Call::Strip, {true, true, false, true, true}},
                        Want{Call::Pretty, {false, false, false, true, true}},
                        Want{Call::BuildObject, {true, true, true, true, true}}}) {
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
          auto r = call(w.call, d, b, out, false).first;
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
