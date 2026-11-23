// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The plan API's cast helpers (#904, stage C1; docs/planner_design.md, *Plan
// API calls for runtime casts*): each helper's value and each error's class,
// SQLSTATE, message and DETAIL, as PostgreSQL 17's cast functions give them;
// an input helper charging its text's size before it parses and releasing
// it after, success or failure; and an allocation failure in each
// allocating helper being BC_E_MEMORY, never an exception into the plan.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"
#include "tests/alloc_faults.h"

import bytecask.sql.tests.fixture;

using namespace sql_test;
using bytecask::sql::PhysType;

namespace {

// The body of the one plan the tests run: each case sets its own.
std::function<void(bc_ctx*)> g_body;
void body(bc_ctx* cx) { g_body(cx); }

// How a run ended: its error's code, SQLSTATE (BC_E_DATA), constraint and
// object (BC_E_CONSTRAINT), message and DETAIL; or the count it emitted.
struct Outcome {
  int code{BC_OK};
  std::uint32_t detail{0};
  std::string message, note;
  std::int64_t count{0};
};

auto run_body(Fixture& f, std::vector<bc_value> p, std::function<void(bc_ctx*)> fn, bc_budget budget = {}) -> Outcome {
  g_body = std::move(fn);
  auto ps = params(std::move(p));
  SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes({}), f.cat().version()};
  auto ref = sr.get();
  Outcome o;
  if (bc_execute(f.cx, body, &ref, &ps, budget, false, 0) == BC_FAILED) {
    o.code = bc_error(f.cx);
    o.detail = bc_error_detail(f.cx);
    o.message = bc_error_message(f.cx);
    o.note = bc_error_note(f.cx);
  } else {
    o.count = bc_ctx_affected(f.cx);
  }
  return o;
}

constexpr auto sqlstate(std::string_view s) -> std::uint32_t { return BC_SQLSTATE(s[0], s[1], s[2], s[3], s[4]); }

// A BC_E_DATA error of `state` and `message`.
auto data_error(const Outcome& o, std::string_view state, std::string_view message) -> bool {
  return o.code == BC_E_DATA && o.detail == sqlstate(state) && o.message == message;
}

// A BC_C_RANGE error on `object` with `message`.
auto range_error(const Outcome& o, std::uint32_t object, std::string_view message) -> bool {
  return o.code == BC_E_CONSTRAINT && (o.detail >> 24U) == BC_C_RANGE && (o.detail & 0xFFFFFFU) == object &&
         o.message == message;
}

// The helpers over $1, emitting an integer as the count.
auto text_in_i64(Fixture& f, std::string_view text, std::uint32_t type, std::uint32_t precision = 0, std::uint32_t scale = 0,
                 std::uint32_t object = 0) -> Outcome {
  return run_body(f, {bytes(std::string{text})}, [=](bc_ctx* cx) {
    bc_emit_count(cx, bc_text_in_i64(cx, bc_param_bytes(cx, 0), type, precision, scale, object));
  });
}

// Whether a view-giving helper's result equals $2.
auto view_is(Fixture& f, bc_value in, std::string_view want, std::function<bc_view(bc_ctx*)> make) -> Outcome {
  return run_body(f, {std::move(in), bytes(std::string{want})}, [make = std::move(make)](bc_ctx* cx) {
    auto v = make(cx);
    if (bc_error(cx) != BC_OK) return;
    bc_emit_count(cx, bc_bytes_eq(cx, v, bc_param_bytes(cx, 1)) ? 1 : 0);
  });
}

// Whether a double-giving helper's result is `want`, bit for bit but for NaN.
auto f64_is(Fixture& f, std::vector<bc_value> in, double want, std::function<double(bc_ctx*)> make) -> Outcome {
  return run_body(f, std::move(in), [=, make = std::move(make)](bc_ctx* cx) {
    auto v = make(cx);
    if (bc_error(cx) != BC_OK) return;
    bc_emit_count(cx, bc_cmp_f64(v, want) == 0 ? 1 : 0);
  });
}

constexpr auto kOk = [](const Outcome& o, std::int64_t count) { return o.code == BC_OK && o.count == count; };

}  // namespace

TEST_CASE("plan API: the cast helpers give PostgreSQL 17's values", "[sql][algebra][cast]") {
  Fixture f;
  // Text to an integer, a DECIMAL and a date or time type.
  CHECK(kOk(text_in_i64(f, " 12 ", BC_TYPE_INTEGER), 12));
  CHECK(kOk(text_in_i64(f, "0x1F", BC_TYPE_SMALLINT), 31));
  CHECK(kOk(text_in_i64(f, "-9223372036854775808", BC_TYPE_BIGINT), std::numeric_limits<std::int64_t>::min()));
  CHECK(kOk(text_in_i64(f, "1_000", BC_TYPE_BIGINT), 1000));
  CHECK(kOk(text_in_i64(f, "12.345", BC_TYPE_DECIMAL, 4, 2), 1235));  // rounded half away from zero
  CHECK(kOk(text_in_i64(f, "-12.345", BC_TYPE_DECIMAL, 4, 2), -1235));
  CHECK(kOk(text_in_i64(f, "0.5", BC_TYPE_DECIMAL, 18, 0), 1));
  CHECK(kOk(text_in_i64(f, "1970-01-02", BC_TYPE_DATE), 1));
  CHECK(kOk(text_in_i64(f, "00:00:01", BC_TYPE_TIME), 1'000'000));
  CHECK(kOk(text_in_i64(f, "1970-01-01 00:00:01", BC_TYPE_TIMESTAMP), 1'000'000));
  CHECK(kOk(text_in_i64(f, "1970-01-01 00:00:01+00", BC_TYPE_TIMESTAMPTZ), 1'000'000));
  // To a float, a REAL read once by float4in.
  CHECK(kOk(f64_is(f, {bytes(" 2.5 ")}, 2.5, [](bc_ctx* cx) { return bc_text_in_f64(cx, bc_param_bytes(cx, 0), BC_TYPE_DOUBLE); }), 1));
  CHECK(kOk(f64_is(f, {bytes("0.1")}, static_cast<double>(0.1F),
                   [](bc_ctx* cx) { return bc_text_in_f64(cx, bc_param_bytes(cx, 0), BC_TYPE_REAL); }),
            1));
  CHECK(kOk(f64_is(f, {bytes("-Infinity")}, -std::numeric_limits<double>::infinity(),
                   [](bc_ctx* cx) { return bc_text_in_f64(cx, bc_param_bytes(cx, 0), BC_TYPE_DOUBLE); }),
            1));
  // To a boolean, bytes and a name.
  CHECK(kOk(run_body(f, {bytes(" yes ")}, [](bc_ctx* cx) { bc_emit_count(cx, bc_text_in_bool(cx, bc_param_bytes(cx, 0)) ? 1 : 0); }), 1));
  CHECK(kOk(view_is(f, bytes("\\x6162"), "ab", [](bc_ctx* cx) { return bc_text_in_bytes(cx, bc_param_bytes(cx, 0), BC_TYPE_BYTEA); }), 1));
  CHECK(kOk(view_is(f, bytes(std::string(70, 'x')), std::string(63, 'x'),
                    [](bc_ctx* cx) { return bc_text_in_bytes(cx, bc_param_bytes(cx, 0), BC_TYPE_NAME); }),
            1));
  // Output functions.
  auto text_of = [&](std::int64_t v, std::uint32_t type, std::uint32_t scale, std::string_view want) {
    return view_is(f, v, want, [=](bc_ctx* cx) { return bc_value_text(cx, bc_param_i64(cx, 0), type, scale); });
  };
  CHECK(kOk(text_of(-1250, BC_TYPE_DECIMAL, 2, "-12.50"), 1));
  CHECK(kOk(text_of(5, BC_TYPE_DECIMAL, 3, "0.005"), 1));
  CHECK(kOk(text_of(std::numeric_limits<std::int64_t>::min(), BC_TYPE_BIGINT, 0, "-9223372036854775808"), 1));
  CHECK(kOk(text_of(1, BC_TYPE_BOOLEAN, 0, "true"), 1));
  CHECK(kOk(text_of(0, BC_TYPE_BOOLEAN, 1, "f"), 1));  // boolout, the I/O cast to a name
  CHECK(kOk(text_of(1, BC_TYPE_DATE, 0, "1970-01-02"), 1));
  CHECK(kOk(text_of(86'399'000'000, BC_TYPE_TIMESTAMPTZ, 0, "1970-01-01 23:59:59+00"), 1));
  CHECK(kOk(view_is(f, bytes("ab"), "\\x6162", [](bc_ctx* cx) { return bc_bytea_text(cx, bc_param_bytes(cx, 0)); }), 1));
  CHECK(kOk(view_is(f, bytes(""), "\\x", [](bc_ctx* cx) { return bc_bytea_text(cx, bc_param_bytes(cx, 0)); }), 1));
  // Numbers: rint ties to even; a DECIMAL half away from zero.
  auto f64_to_int = [&](double d, std::uint32_t type) {
    return run_body(f, {d}, [=](bc_ctx* cx) { bc_emit_count(cx, bc_f64_to_int(cx, bc_param_f64(cx, 0), type, 0)); });
  };
  CHECK(kOk(f64_to_int(2.5, BC_INTEGER), 2));
  CHECK(kOk(f64_to_int(3.5, BC_INTEGER), 4));
  CHECK(kOk(f64_to_int(-0.5, BC_SMALLINT), 0));
  CHECK(kOk(f64_to_int(-9223372036854775808.0, BC_BIGINT), std::numeric_limits<std::int64_t>::min()));
  auto rescale = [&](std::int64_t v, std::uint32_t from, std::uint32_t to, std::uint32_t p, std::uint32_t s, std::uint32_t object = 0) {
    return run_body(f, {v}, [=](bc_ctx* cx) {
      bc_emit_count(cx, bc_decimal_rescale(cx, bc_param_i64(cx, 0), from, to, p, s, object));
    });
  };
  CHECK(kOk(rescale(250, 2, BC_TYPE_INTEGER, 0, 0), 3));
  CHECK(kOk(rescale(-250, 2, BC_TYPE_INTEGER, 0, 0), -3));
  CHECK(kOk(rescale(350, 2, BC_TYPE_SMALLINT, 0, 0), 4));
  CHECK(kOk(rescale(7, 0, BC_TYPE_DECIMAL, 5, 2), 700));
  CHECK(kOk(rescale(1235, 3, BC_TYPE_DECIMAL, 3, 2), 124));
  CHECK(kOk(rescale(std::numeric_limits<std::int64_t>::min(), 18, BC_TYPE_BIGINT, 0, 0), -9));
  CHECK(kOk(rescale(999'999'999'999'999'999, 18, BC_TYPE_DECIMAL, 1, 0), 1));
  CHECK(kOk(run_body(f, {2.5}, [](bc_ctx* cx) { bc_emit_count(cx, bc_f64_to_decimal(cx, bc_param_f64(cx, 0), false, 3, 0, 0)); }), 3));
  CHECK(kOk(run_body(f, {1.0 / 3.0}, [](bc_ctx* cx) { bc_emit_count(cx, bc_f64_to_decimal(cx, bc_param_f64(cx, 0), false, 18, 17, 0)); }),
            33'333'333'333'333'300));  // 15 significant digits, as float8_numeric
  CHECK(kOk(f64_is(f, {std::int64_t{123}}, 1.23, [](bc_ctx* cx) { return bc_decimal_to_f64(cx, bc_param_i64(cx, 0), 2, false); }), 1));
  CHECK(kOk(f64_is(f, {std::int64_t{1}}, static_cast<double>(0.1F), [](bc_ctx* cx) { return bc_decimal_to_f64(cx, bc_param_i64(cx, 0), 1, true); }), 1));
  CHECK(kOk(f64_is(f, {0.1}, static_cast<double>(0.1F), [](bc_ctx* cx) { return bc_f64_to_f32(cx, bc_param_f64(cx, 0)); }), 1));
  CHECK(bc_i64_to_f32(16'777'217) == 16'777'216.0);
  CHECK(bc_i64_to_f32(std::numeric_limits<std::int64_t>::max()) == 9223372036854775808.0);
  // Dates and times, in UTC.
  auto temporal = [&](std::int64_t v, std::uint32_t from, std::uint32_t to) {
    return run_body(f, {v}, [=](bc_ctx* cx) { bc_emit_count(cx, bc_temporal_cast(cx, bc_param_i64(cx, 0), from, to)); });
  };
  CHECK(kOk(temporal(2, BC_TYPE_DATE, BC_TYPE_TIMESTAMP), 2 * 86'400'000'000));
  CHECK(kOk(temporal(-1, BC_TYPE_TIMESTAMPTZ, BC_TYPE_DATE), -1));
  CHECK(kOk(temporal(-1, BC_TYPE_TIMESTAMP, BC_TYPE_TIME), 86'399'999'999));
  // A time to a time with time zone at offset 0, and back.
  CHECK(kOk(run_body(f, {std::int64_t{5}}, [](bc_ctx* cx) {
              bc_emit_count(cx, bc_temporal_cast(cx, bc_temporal_cast(cx, bc_param_i64(cx, 0), BC_TYPE_TIME, BC_TYPE_TIMETZ),
                                                 BC_TYPE_TIMETZ, BC_TYPE_TIME));
            }),
            5));
}

TEST_CASE("plan API: the cast helpers keep their input function's SQLSTATE", "[sql][algebra][cast]") {
  Fixture f;
  CHECK(data_error(text_in_i64(f, "ab", BC_TYPE_INTEGER), "22P02", "invalid input syntax for type integer: \"ab\""));
  CHECK(data_error(text_in_i64(f, "", BC_TYPE_SMALLINT), "22P02", "invalid input syntax for type smallint: \"\""));
  CHECK(data_error(text_in_i64(f, "x", BC_TYPE_DECIMAL, 5, 2), "22P02", "invalid input syntax for type numeric: \"x\""));
  CHECK(data_error(text_in_i64(f, "x", BC_TYPE_DATE), "22007", "invalid input syntax for type date: \"x\""));
  CHECK(data_error(text_in_i64(f, "2024-02-30", BC_TYPE_DATE), "22008", "date/time field value out of range: \"2024-02-30\""));
  // An integer or DECIMAL out of its type is BC_C_RANGE on the object given, with PostgreSQL's message and DETAIL.
  CHECK(range_error(text_in_i64(f, "3000000000", BC_TYPE_INTEGER, 0, 0, 7), 7,
                    "value \"3000000000\" is out of range for type integer"));
  auto o = text_in_i64(f, "123.4", BC_TYPE_DECIMAL, 3, 1, 9);
  CHECK(range_error(o, 9, "numeric field overflow"));
  CHECK(o.note == "A field with precision 3, scale 1 must round to an absolute value less than 10^2.");
  o = text_in_i64(f, "99999999999999999999", BC_TYPE_DECIMAL, 18, 0);
  CHECK(range_error(o, 0, "numeric field overflow"));
  CHECK(o.note == "A field with precision 18, scale 0 must round to an absolute value less than 10^18.");
  o = text_in_i64(f, "1", BC_TYPE_DECIMAL, 2, 2);
  CHECK(o.note == "A field with precision 2, scale 2 must round to an absolute value less than 1.");
  o = text_in_i64(f, "-Infinity", BC_TYPE_DECIMAL, 3, 0);
  CHECK(range_error(o, 0, "numeric field overflow"));
  CHECK(o.note == "A field with precision 3, scale 0 cannot hold an infinite value.");
  o = run_body(f, {std::numeric_limits<double>::quiet_NaN()},
               [](bc_ctx* cx) { bc_emit_count(cx, bc_f64_to_decimal(cx, bc_param_f64(cx, 0), false, 3, 0, 4)); });
  CHECK(range_error(o, 4, "numeric field overflow"));
  CHECK(o.note == "A field with precision 3, scale 0 cannot hold NaN.");
  // Floats: a REAL's own messages; a float's range error is BC_E_DATA.
  auto f64 = [&](std::string_view text, std::uint32_t type) {
    return run_body(f, {bytes(std::string{text})}, [=](bc_ctx* cx) { (void)bc_text_in_f64(cx, bc_param_bytes(cx, 0), type); });
  };
  CHECK(data_error(f64("1e39", BC_TYPE_REAL), "22003", "\"1e39\" is out of range for type real"));
  CHECK(data_error(f64("x", BC_TYPE_REAL), "22P02", "invalid input syntax for type real: \"x\""));
  CHECK(data_error(f64("1e400", BC_TYPE_DOUBLE), "22003", "\"1e400\" is out of range for type double precision"));
  CHECK(data_error(run_body(f, {bytes("maybe")}, [](bc_ctx* cx) { (void)bc_text_in_bool(cx, bc_param_bytes(cx, 0)); }), "22P02",
                   "invalid input syntax for type boolean: \"maybe\""));
  CHECK(data_error(run_body(f, {bytes("\\xzz")}, [](bc_ctx* cx) { (void)bc_text_in_bytes(cx, bc_param_bytes(cx, 0), BC_TYPE_BYTEA); }),
                   "22023", "invalid hexadecimal digit: \"z\""));
  auto to_int = [&](double d, std::uint32_t type) {
    return run_body(f, {d}, [=](bc_ctx* cx) { (void)bc_f64_to_int(cx, bc_param_f64(cx, 0), type, 3); });
  };
  CHECK(range_error(to_int(std::numeric_limits<double>::quiet_NaN(), BC_INTEGER), 3, "integer out of range"));
  CHECK(range_error(to_int(std::numeric_limits<double>::infinity(), BC_BIGINT), 3, "bigint out of range"));
  CHECK(range_error(to_int(9223372036854775808.0, BC_BIGINT), 3, "bigint out of range"));
  CHECK(range_error(to_int(32767.5, BC_SMALLINT), 3, "smallint out of range"));
  auto rescale = [&](std::int64_t v, std::uint32_t from, std::uint32_t to, std::uint32_t p, std::uint32_t s) {
    return run_body(f, {v}, [=](bc_ctx* cx) { (void)bc_decimal_rescale(cx, bc_param_i64(cx, 0), from, to, p, s, 2); });
  };
  CHECK(range_error(rescale(3'000'000'000, 0, BC_TYPE_INTEGER, 0, 0), 2, "integer out of range"));
  CHECK(range_error(rescale(3'276'750, 2, BC_TYPE_SMALLINT, 0, 0), 2, "smallint out of range"));
  o = rescale(995, 1, BC_TYPE_DECIMAL, 2, 0);
  CHECK(range_error(o, 2, "numeric field overflow"));
  CHECK(o.note == "A field with precision 2, scale 0 must round to an absolute value less than 10^2.");
  CHECK(range_error(rescale(std::numeric_limits<std::int64_t>::max(), 0, BC_TYPE_DECIMAL, 18, 2), 2, "numeric field overflow"));
  CHECK(data_error(run_body(f, {1e39}, [](bc_ctx* cx) { (void)bc_f64_to_f32(cx, bc_param_f64(cx, 0)); }), "22003",
                   "value out of range: overflow"));
  CHECK(data_error(run_body(f, {1e-50}, [](bc_ctx* cx) { (void)bc_f64_to_f32(cx, bc_param_f64(cx, 0)); }), "22003",
                   "value out of range: underflow"));
  CHECK(data_error(run_body(f, {std::int64_t{106'762'940}},
                            [](bc_ctx* cx) { (void)bc_temporal_cast(cx, bc_param_i64(cx, 0), BC_TYPE_DATE, BC_TYPE_TIMESTAMPTZ); }),
                   "22008", "date out of range for timestamp"));
  // Before PostgreSQL's end, past the layer's (its microseconds count from 1970).
  CHECK(data_error(run_body(f, {std::int64_t{106'762'939}},
                            [](bc_ctx* cx) { (void)bc_temporal_cast(cx, bc_param_i64(cx, 0), BC_TYPE_DATE, BC_TYPE_TIMESTAMP); }),
                   "22008", "timestamp out of range"));
  CHECK(kOk(run_body(f, {std::int64_t{106'751'991}},
                     [](bc_ctx* cx) { bc_emit_count(cx, bc_temporal_cast(cx, bc_param_i64(cx, 0), BC_TYPE_DATE, BC_TYPE_TIMESTAMP) > 0); }),
            1));
  // A type, scale or pair outside a helper's lists is a plan bug.
  CHECK(text_in_i64(f, "1", BC_TYPE_TEXT).code == BC_E_HANDLE);
  CHECK(text_in_i64(f, "1", BC_TYPE_DECIMAL, 19, 0).code == BC_E_HANDLE);
  CHECK(text_in_i64(f, "1", BC_TYPE_DECIMAL, 2, 3).code == BC_E_HANDLE);
  CHECK(text_in_i64(f, "1", 99).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_value_text(cx, 1, BC_TYPE_DECIMAL, 19); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_value_text(cx, 2, BC_TYPE_BOOLEAN, 0); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_value_text(cx, 1, BC_TYPE_TEXT, 0); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_decimal_rescale(cx, 1, 19, BC_TYPE_BIGINT, 0, 0, 0); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_decimal_to_f64(cx, 1, 19, false); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_f64_to_int(cx, 1.0, 3, 0); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_temporal_cast(cx, 1, BC_TYPE_TIME, BC_TYPE_DATE); }).code == BC_E_HANDLE);
  CHECK(run_body(f, {}, [](bc_ctx* cx) { (void)bc_text_in_bytes(cx, bc_empty(cx), BC_TYPE_TEXT); }).code == BC_E_HANDLE);
}

TEST_CASE("plan API: an input helper charges its text before it parses, and releases it after", "[sql][algebra][cast]") {
  Fixture f;
  // Text no integer reads: past its charge it is 54000, before the parse
  // that would find it is no integer.
  const std::string junk(4000, 'z');
  std::optional<std::uint64_t> held_before, held_after;
  auto body_of = [&](bc_ctx* cx) {
    auto v = bc_param_bytes(cx, 0);
    held_before = bc_ctx_held_for_testing(cx);
    (void)bc_text_in_i64(cx, v, BC_TYPE_INTEGER, 0, 0, 0);
    held_after = bc_ctx_held_for_testing(cx);
  };
  auto limited = false;
  auto parsed = false;
  for (std::uint64_t budget = 1024; budget < 64 * 1024; budget += 256) {
    INFO("budget " << budget);
    held_before.reset();
    auto o = run_body(f, {bytes(junk)}, body_of, {.max_reads = 0, .max_bytes = budget, .deadline_ns = 0, .cancel = nullptr});
    if (!held_before) continue;  // the parameter's view did not fit
    REQUIRE(held_after);
    CHECK(*held_after == *held_before);  // released, success or failure
    if (o.code == BC_E_LIMIT) {
      CHECK_FALSE(parsed);  // never a parse under a budget smaller than one that refused
      limited = true;
      CHECK(*held_before + junk.size() > budget);
    } else {
      CHECK(data_error(o, "22P02", "invalid input syntax for type integer: \"" + junk + "\""));
      parsed = true;
    }
  }
  CHECK(limited);
  CHECK(parsed);
  // A success releases its charge too; bytes keep only their arena copy.
  CHECK(kOk(run_body(f, {bytes(" 42 "), bytes("\\x6162")}, [&](bc_ctx* cx) {
              auto v = bc_param_bytes(cx, 0);
              auto w = bc_param_bytes(cx, 1);
              auto before = bc_ctx_held_for_testing(cx);
              auto n = bc_text_in_i64(cx, v, BC_TYPE_BIGINT, 0, 0, 0);
              auto same = bc_ctx_held_for_testing(cx) == before;
              auto b = bc_text_in_bytes(cx, w, BC_TYPE_BYTEA);
              auto grown = bc_ctx_held_for_testing(cx) - before;
              bc_emit_count(cx, n == 42 && same && grown >= 2 && bc_octet_length(cx, b) == 2 ? 1 : 0);
            }),
            1));
}

#ifdef BYTECASK_NO_ALLOC_FAULTS
#else
namespace {

// Each allocating cast helper, called in turn; the one an error ended; and
// whether a failed helper kept more charged than the same helper keeps when
// it succeeds (`g_kept`, from a run with no faults). A failed helper may
// keep the arena bytes it had charged for its output, released with the
// arena at reset; never its text's temporaries, which are larger.
enum class CastStep : std::uint8_t { TextInI64, TextInBytes, ValueText, ByteaText, NameCut };
std::optional<CastStep> g_ended;
std::array<std::uint64_t, 5> g_kept{};
bool g_recording = false;
bool g_failed_step_kept_more = false;

void cast_steps(bc_ctx* cx) {
  auto step = [cx](CastStep s, const auto& call) {
    if (bc_error(cx) != BC_OK) return;
    const auto before = bc_ctx_held_for_testing(cx);
    call();
    const auto kept = bc_ctx_held_for_testing(cx) - before;
    const auto at = static_cast<std::size_t>(s);
    if (g_recording) g_kept.at(at) = kept;
    if (bc_error(cx) != BC_OK) {
      g_ended = s;
      if (kept > g_kept.at(at)) g_failed_step_kept_more = true;
    }
  };
  const bc_view num = bc_param_bytes(cx, 0);
  const bc_view hex = bc_param_bytes(cx, 1);
  step(CastStep::TextInI64, [&] { (void)bc_text_in_i64(cx, num, BC_TYPE_DECIMAL, 10, 2, 0); });
  step(CastStep::TextInBytes, [&] { (void)bc_text_in_bytes(cx, hex, BC_TYPE_BYTEA); });
  step(CastStep::ValueText, [&] { (void)bc_value_text(cx, INT64_MIN, BC_TYPE_DECIMAL, 2); });
  step(CastStep::ByteaText, [&] { (void)bc_bytea_text(cx, hex); });
  step(CastStep::NameCut, [&] { (void)bc_text_in_bytes(cx, num, BC_TYPE_NAME); });
}

}  // namespace
#endif

TEST_CASE("plan API: an allocation failure in each cast helper is BC_E_MEMORY, its temporaries' charge released",
          "[sql][algebra][cast][alloc]") {
#ifdef BYTECASK_NO_ALLOC_FAULTS
  SKIP("the allocation-fault seam is absent under this sanitizer");
#else
  Fixture f;
  // The sweep must reach every helper (tests/CLAUDE.md, *Fault sweeps must
  // reach every structural transition*).
  std::set<CastStep> ended;
  const std::string number = "  123456.78500000000000000000000  ";  // past the short-string buffer
  auto ps = params({bytes(number), bytes("\\x" + std::string(200, 'a'))});
  {
    // What each helper keeps charged when nothing fails.
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes({}), f.cat().version()};
    auto ref = sr.get();
    auto* cx = bc_ctx_new(&f.bdb, &f.bcat);
    bc_ctx_begin(cx, &ps, {});
    g_recording = true;
    bc_ctx_run(cx, cast_steps, &ref);
    g_recording = false;
    REQUIRE(bc_error(cx) == BC_OK);
    REQUIRE(g_kept.at(static_cast<std::size_t>(CastStep::TextInI64)) == 0);  // an integer keeps nothing
    bc_ctx_free(cx);
  }
  for (long nth = 1;; ++nth) {
    REQUIRE(nth < 100000);
    INFO("allocation " << nth);
    g_ended.reset();
    g_failed_step_kept_more = false;
    SchemaRef sr{all_objects(f.cat()), all_tables(f.cat()), all_cols(f.cat()), type_codes({}), f.cat().version()};
    auto ref = sr.get();
    auto* cx = bc_ctx_new(&f.bdb, &f.bcat);
    bc_ctx_begin(cx, &ps, {});
    auto threw = false;
    bytecask::testing::AllocFaultReport report;
    {
      bytecask::testing::ScopedAllocFaults faults{nth, false};
      try {
        bc_ctx_run(cx, cast_steps, &ref);
      } catch (...) {
        threw = true;
      }
      report = faults.report();
    }
    CHECK_FALSE(threw);
    const auto error = bc_error(cx);
    if (!report.fired) {
      CHECK(error == BC_OK);
      bc_ctx_free(cx);
      break;
    }
    CHECK((error == BC_E_MEMORY || error == BC_OK));
    if (g_ended) ended.insert(*g_ended);
    CHECK_FALSE(g_failed_step_kept_more);  // before reset, which zeroes the count
    bc_ctx_reset(cx);
    bc_ctx_free(cx);
  }
  CHECK(ended == std::set<CastStep>{CastStep::TextInI64, CastStep::TextInBytes, CastStep::ValueText, CastStep::ByteaText,
                                    CastStep::NameCut});
#endif
}
