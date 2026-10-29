// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sqlstate.cppm — the SQLSTATEs the parser, binder and catalog refuse with:
// one flat set of constants, PostgreSQL's names for them. An error carries a
// SqlState, never a string, so a throw site cannot name a code that is not
// here, and the code outlives the error: every constant points at a literal.

module;
#include <optional>
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
inline constexpr SqlState kDuplicateAlias{"42712"};
inline constexpr SqlState kIndeterminateDatatype{"42P18"};
inline constexpr SqlState kWrongObjectType{"42809"};
inline constexpr SqlState kInvalidColumnReference{"42P10"};
inline constexpr SqlState kInvalidForeignKey{"42830"};
// What an input function or a range check reports (sql/text.cppm,
// range_sqlstate): text its type does not read, or a value outside it.
inline constexpr SqlState kInvalidTextRepresentation{"22P02"};
inline constexpr SqlState kStringDataRightTruncation{"22001"};
inline constexpr SqlState kInvalidDatetimeFormat{"22007"};
inline constexpr SqlState kDatetimeFieldOverflow{"22008"};
inline constexpr SqlState kCharacterNotInRepertoire{"22021"};
inline constexpr SqlState kInvalidParameterValue{"22023"};
inline constexpr SqlState kInvalidEscapeSequence{"22025"};

// The constant above whose code is `code`, or nullopt.
[[nodiscard]] constexpr auto known_sqlstate(std::string_view code) noexcept -> std::optional<SqlState> {
  for (auto s : {kSyntaxError, kFeatureNotSupported, kNumericValueOutOfRange, kNotNullViolation, kGroupingError,
                 kDatatypeMismatch, kUndefinedFunction, kUndefinedColumn, kUndefinedTable, kUndefinedObject,
                 kDuplicateColumn, kDuplicateTable, kAmbiguousColumn, kDuplicateAlias, kIndeterminateDatatype, kWrongObjectType, kInvalidColumnReference, kInvalidForeignKey,
                 kInvalidTextRepresentation, kStringDataRightTruncation, kInvalidDatetimeFormat,
                 kDatetimeFieldOverflow, kCharacterNotInRepertoire, kInvalidParameterValue, kInvalidEscapeSequence}) {
    if (s.view() == code) return s;
  }
  return std::nullopt;
}

}  // namespace bytecask::sql
