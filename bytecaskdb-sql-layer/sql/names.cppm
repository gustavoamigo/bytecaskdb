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
#include <string_view>

export module bytecask.sql.names;

export namespace bytecask::sql {

// `public`: schema 0, implicit, never written (docs/catalog_design.md,
// *Schema semantics*). Every other schema has a record and an id from the
// catalog's one counter.
inline constexpr std::uint32_t kPublicSchema = 0;

// The schemas PostgreSQL 17 always has and the layer holds nothing in: a
// name qualified by one resolves to one of these ids, which no record has,
// so a lookup in it finds nothing and the catalog refuses a DDL into it
// (3F000). pg_temp is not one: a session has no temporary schema until it
// creates a temporary object, which the layer has none of, so pg_temp does
// not exist, as in a PostgreSQL session that has made none. Above every object id (kMaxObjectId, 2^24 - 1) and every role id.
inline constexpr std::uint32_t kPgCatalogSchema = 0xFFFFFF00;
inline constexpr std::uint32_t kInformationSchema = 0xFFFFFF01;
inline constexpr std::uint32_t kPgToastSchema = 0xFFFFFF02;

// The system schema a name is, or 0 (public's id, never a system schema's).
[[nodiscard]] constexpr auto system_schema(std::string_view name) -> std::uint32_t {
  if (name == "pg_catalog") return kPgCatalogSchema;
  if (name == "information_schema") return kInformationSchema;
  if (name == "pg_toast") return kPgToastSchema;
  return 0;
}

// Where a name of a schema that does not exist is looked up: no record has
// it and the catalog refuses a DDL into it, so every lookup there finds
// nothing.
inline constexpr std::uint32_t kMissingSchema = 0xFFFFFFFE;

// An aggregate, so it is built only with braces naming both parts:
// QualifiedName{kPublicSchema, "t"}.
struct QualifiedName {
  std::uint32_t schema;
  std::string name;

  friend auto operator==(const QualifiedName&, const QualifiedName&) -> bool = default;
  friend auto operator<=>(const QualifiedName&, const QualifiedName&) = default;
};

}  // namespace bytecask::sql
