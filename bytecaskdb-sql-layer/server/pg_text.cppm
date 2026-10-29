// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// pg_text.cppm — each SQL type as PostgreSQL's wire protocol names it (its
// OID, length and modifier), and its text format: a value written as
// PostgreSQL's output function writes it, and a parameter's text read as
// its input function reads it, for the forms docs/server_design.md (*Types
// and text*) lists. The text format itself is the layer's (sql/text.cppm),
// which string literals share. Pure functions: no I/O, no state.

module;
#include <cstdint>
#include <string>
#include <string_view>

export module bytecask.sql.pg_text;

import bytecask.sql.api;
export import bytecask.sql.text;

export namespace bytecask::sql::pg {

// A type as RowDescription and ParameterDescription carry it.
struct TypeInfo {
  std::int32_t oid;
  std::int16_t length;  // -1: variable
  std::int32_t modifier;
};

[[nodiscard]] auto type_info(SqlType t) -> TypeInfo;

// A value's text is sql::to_text's; TextError is sql::TextError.
using bytecask::sql::TextError;

// A parameter's text as its type's value (sql::from_text): a DECIMAL finer
// than its scale is rounded only for a parameter every use assigns.
[[nodiscard]] inline auto from_text(std::string_view text, const ParamInfo& p) -> Value {
  return sql::from_text(text, p.type, p.assigned_only);
}

}  // namespace bytecask::sql::pg

namespace bytecask::sql::pg {

auto type_info(SqlType t) -> TypeInfo {
  switch (t.kind) {
    case SqlKind::SmallInt: return {21, 2, -1};
    case SqlKind::Integer: return {23, 4, -1};
    case SqlKind::BigInt: return {20, 8, -1};
    case SqlKind::Real: return {700, 4, -1};
    case SqlKind::Double: return {701, 8, -1};
    case SqlKind::Boolean: return {16, 1, -1};
    case SqlKind::Bytea: return {17, -1, -1};
    case SqlKind::Text: return {25, -1, -1};
    case SqlKind::Varchar: return {1043, -1, static_cast<std::int32_t>(t.length) + 4};
    case SqlKind::Char: return {1042, -1, static_cast<std::int32_t>(t.length) + 4};
    case SqlKind::Decimal:
      // Precision 0 is unconstrained numeric: a SUM's type.
      if (t.precision == 0) return {1700, -1, -1};
      return {1700, -1, ((static_cast<std::int32_t>(t.precision) << 16) | t.scale) + 4};
    case SqlKind::Timestamp: return {1114, 8, -1};
    case SqlKind::TimestampTz: return {1184, 8, -1};
    case SqlKind::Date: return {1082, 4, -1};
    case SqlKind::Time: return {1083, 8, -1};
    case SqlKind::Numeric: return {1700, -1, -1};
  }
  return {25, -1, -1};
}

}  // namespace bytecask::sql::pg
