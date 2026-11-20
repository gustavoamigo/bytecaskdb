// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// names.cppm — the name of a relation, procedure or type: its schema and
// its name within it (docs/catalog_design.md, *Stages of schemas*, stage
// 1a). Every reference to such an object, in the AST, a DDL and the
// catalog's lookups, is one of these; no bare string converts to it, so
// each site says which schema it means. Roles, columns and an index's own
// name in CREATE INDEX are not schema objects and stay strings.

module;
#include <compare>
#include <cstdint>
#include <string>

export module bytecask.sql.names;

export namespace bytecask::sql {

// `public`: schema 0, implicit, never written. The only schema until stage
// 1b builds the schema record.
inline constexpr std::uint32_t kPublicSchema = 0;

// An aggregate, so it is built only with braces naming both parts:
// QualifiedName{kPublicSchema, "t"}.
struct QualifiedName {
  std::uint32_t schema;
  std::string name;

  friend auto operator==(const QualifiedName&, const QualifiedName&) -> bool = default;
  friend auto operator<=>(const QualifiedName&, const QualifiedName&) = default;
};

}  // namespace bytecask::sql
