// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sqlstate.cppm — the SQLSTATEs the parser, binder and catalog refuse with:
// one flat set of constants, PostgreSQL's names for them. An error carries a
// SqlState, never a string, so a throw site cannot name a code that is not
// here, and the code outlives the error: every constant points at a literal.

module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

export module bytecask.sql.sqlstate;

export namespace bytecask::sql {

class SqlState {
public:
  // consteval: only a literal of five characters, so only the constants below.
  consteval explicit SqlState(const char (&code)[6]) noexcept : code_{code} {}
  [[nodiscard]] constexpr auto view() const noexcept -> std::string_view { return code_; }
  // As bc_error_detail carries it for BC_E_DATA and BC_E_ARITH: BC_SQLSTATE,
  // PostgreSQL's MAKE_SQLSTATE, six bits a character, the first lowest.
  [[nodiscard]] constexpr auto packed() const noexcept -> std::uint32_t {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 5; ++i) {
      v |= (static_cast<std::uint32_t>(static_cast<unsigned char>(code_[i]) - unsigned{'0'}) & 0x3Fu) << (6 * i);
    }
    return v;
  }

private:
  const char* code_;
};

inline constexpr SqlState kSyntaxError{"42601"};  // also a form the layer does not support yet
inline constexpr SqlState kFeatureNotSupported{"0A000"};
inline constexpr SqlState kNumericValueOutOfRange{"22003"};
inline constexpr SqlState kDivisionByZero{"22012"};
inline constexpr SqlState kNotNullViolation{"23502"};
inline constexpr SqlState kGroupingError{"42803"};
inline constexpr SqlState kDatatypeMismatch{"42804"};
inline constexpr SqlState kUndefinedFunction{"42883"};  // also an operator
inline constexpr SqlState kAmbiguousFunction{"42725"};  // also an operator
inline constexpr SqlState kUndefinedColumn{"42703"};
inline constexpr SqlState kUndefinedTable{"42P01"};
inline constexpr SqlState kUndefinedObject{"42704"};
inline constexpr SqlState kDuplicateColumn{"42701"};
inline constexpr SqlState kDuplicateTable{"42P07"};
inline constexpr SqlState kDuplicateObject{"42710"};
inline constexpr SqlState kAmbiguousColumn{"42702"};
inline constexpr SqlState kDuplicateAlias{"42712"};
inline constexpr SqlState kIndeterminateDatatype{"42P18"};
inline constexpr SqlState kWrongObjectType{"42809"};
inline constexpr SqlState kInvalidColumnReference{"42P10"};
inline constexpr SqlState kInvalidForeignKey{"42830"};
inline constexpr SqlState kDependentObjectsStillExist{"2BP01"};
inline constexpr SqlState kUndefinedParameter{"42P02"};
inline constexpr SqlState kDuplicateFunction{"42723"};  // also a procedure
inline constexpr SqlState kInvalidFunctionDefinition{"42P13"};
inline constexpr SqlState kNullValueNotAllowed{"22004"};
inline constexpr SqlState kNoDataFound{"P0002"};   // STRICT: no row
inline constexpr SqlState kTooManyRows{"P0003"};   // STRICT: a second row
// What an input function or a range check reports (sql/text.cppm,
// range_sqlstate): text its type does not read, or a value outside it.
inline constexpr SqlState kInvalidTextRepresentation{"22P02"};
inline constexpr SqlState kStringDataRightTruncation{"22001"};
inline constexpr SqlState kInvalidDatetimeFormat{"22007"};
inline constexpr SqlState kDatetimeFieldOverflow{"22008"};
inline constexpr SqlState kInvalidTimeZoneDisplacement{"22009"};
inline constexpr SqlState kCharacterNotInRepertoire{"22021"};
inline constexpr SqlState kInvalidParameterValue{"22023"};
inline constexpr SqlState kInvalidEscapeSequence{"22025"};
inline constexpr SqlState kSubstringError{"22011"};
inline constexpr SqlState kCannotCoerce{"42846"};
inline constexpr SqlState kInvalidRowCountInLimit{"2201W"};
inline constexpr SqlState kReservedName{"42939"};
inline constexpr SqlState kInvalidGrantOperation{"0LP01"};
inline constexpr SqlState kInvalidRowCountInOffset{"2201X"};
inline constexpr SqlState kInvalidTableDefinition{"42P16"};
inline constexpr SqlState kObjectNotInPrerequisiteState{"55000"};
inline constexpr SqlState kInvalidSavepointSpecification{"3B001"};
inline constexpr SqlState kReadOnlySqlTransaction{"25006"};
inline constexpr SqlState kActiveSqlTransaction{"25001"};
inline constexpr SqlState kObjectInUse{"55006"};
inline constexpr SqlState kInvalidSchemaName{"3F000"};
inline constexpr SqlState kDuplicateSchema{"42P06"};
inline constexpr SqlState kInvalidName{"42602"};
inline constexpr SqlState kInsufficientPrivilege{"42501"};
inline constexpr SqlState kInvalidSchemaDefinition{"42P15"};
inline constexpr SqlState kWithCheckOptionViolation{"44000"};
inline constexpr SqlState kInternalError{"XX000"};
// jsonb's (sql/jsonb.cppm): \u0000, a value past PostgreSQL's size bounds,
// one nested past the depth bound.
inline constexpr SqlState kUntranslatableCharacter{"22P05"};
inline constexpr SqlState kProgramLimitExceeded{"54000"};
inline constexpr SqlState kStatementTooComplex{"54001"};

// The five characters `packed` holds (SqlState::packed), or nullopt for 0,
// for bits past the 30 it uses, and for a character outside 0-9 and A-Z,
// none of which a SqlState packs to.
[[nodiscard]] constexpr auto unpack_sqlstate(std::uint32_t packed) noexcept -> std::optional<std::array<char, 5>> {
  if (packed == 0 || (packed >> 30) != 0) return std::nullopt;
  std::array<char, 5> code{};
  for (std::size_t i = 0; i < 5; ++i) {
    auto c = static_cast<char>('0' + ((packed >> (6 * i)) & 0x3Fu));
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return std::nullopt;
    code[i] = c;
  }
  return code;
}

// The constant above whose code is `code`, or nullopt.
[[nodiscard]] constexpr auto known_sqlstate(std::string_view code) noexcept -> std::optional<SqlState> {
  for (auto s : {kSyntaxError, kFeatureNotSupported, kNumericValueOutOfRange, kDivisionByZero, kNotNullViolation, kGroupingError,
                 kDatatypeMismatch, kUndefinedFunction, kAmbiguousFunction, kUndefinedColumn, kUndefinedTable, kUndefinedObject,
                 kDuplicateColumn, kDuplicateTable, kDuplicateObject, kAmbiguousColumn, kDuplicateAlias, kIndeterminateDatatype, kWrongObjectType, kInvalidColumnReference, kInvalidForeignKey, kDependentObjectsStillExist, kUndefinedParameter, kDuplicateFunction, kInvalidFunctionDefinition, kNullValueNotAllowed, kNoDataFound, kTooManyRows,
                 kInvalidTextRepresentation, kStringDataRightTruncation, kInvalidDatetimeFormat,
                 kDatetimeFieldOverflow, kInvalidTimeZoneDisplacement, kCharacterNotInRepertoire, kInvalidParameterValue, kInvalidEscapeSequence, kSubstringError, kCannotCoerce,
                 kInvalidRowCountInLimit, kInvalidRowCountInOffset, kReservedName, kInvalidGrantOperation,
                 kInvalidSavepointSpecification, kReadOnlySqlTransaction, kActiveSqlTransaction, kObjectInUse, kInvalidSchemaName,
                 kDuplicateSchema, kInvalidName, kInsufficientPrivilege, kInvalidSchemaDefinition, kWithCheckOptionViolation, kInternalError,
                 kUntranslatableCharacter, kProgramLimitExceeded, kStatementTooComplex}) {
    if (s.view() == code) return s;
  }
  return std::nullopt;
}

}  // namespace bytecask::sql
