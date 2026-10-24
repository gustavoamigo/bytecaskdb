// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sqlstate.cppm — the SQLSTATEs the parser, binder and catalog refuse with:
// one flat set of constants, PostgreSQL's names for them. An error carries a
// SqlState, never a string, so a throw site cannot name a code that is not
// here, and the code outlives the error: every constant points at a literal.

module;
#include <string_view>

export module bytecask.sql.sqlstate;

export namespace bytecask::sql {

class SqlState {
public:
  // consteval: only a literal of five characters, so only the constants below.
  consteval explicit SqlState(const char (&code)[6]) noexcept : code_{code} {}
  [[nodiscard]] constexpr auto view() const noexcept -> std::string_view { return code_; }

private:
  const char* code_;
};

inline constexpr SqlState kSyntaxError{"42601"};  // also a form the layer does not support yet
inline constexpr SqlState kFeatureNotSupported{"0A000"};
inline constexpr SqlState kNumericValueOutOfRange{"22003"};
inline constexpr SqlState kNotNullViolation{"23502"};
inline constexpr SqlState kGroupingError{"42803"};
inline constexpr SqlState kDatatypeMismatch{"42804"};
inline constexpr SqlState kUndefinedFunction{"42883"};  // also an operator
inline constexpr SqlState kUndefinedColumn{"42703"};
inline constexpr SqlState kUndefinedTable{"42P01"};
inline constexpr SqlState kUndefinedObject{"42704"};
inline constexpr SqlState kDuplicateColumn{"42701"};
inline constexpr SqlState kDuplicateTable{"42P07"};
inline constexpr SqlState kAmbiguousColumn{"42702"};
inline constexpr SqlState kIndeterminateDatatype{"42P18"};
inline constexpr SqlState kWrongObjectType{"42809"};

}  // namespace bytecask::sql
