// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.h — Persistent catalog encoding for the ByteCaskDB / MariaDB plugin.
//
// The catalog stores table metadata and ID counters inside ByteCaskDB itself,
// under the 0x01 namespace prefix.  This makes table-id assignments survive
// server restarts without external state.
//
// Key-space layout (all namespaces):
//   0x01  Catalog   — table metadata and counters
//   0x02  Row data  — [table_id][pk_columns]
//   0x03  Secondary — [table_id][index_id][sec_key][pk] (future)
//   0x04  Admin     — reserved for future use

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// Namespace tags — first byte of every key stored in ByteCaskDB.
// ---------------------------------------------------------------------------

inline constexpr uint8_t kNsCatalog  = 0x01;
inline constexpr uint8_t kNsRow      = 0x02;
inline constexpr uint8_t kNsIndex    = 0x03;
inline constexpr uint8_t kNsAdmin    = 0x04;

// ---------------------------------------------------------------------------
// Catalog sub-namespace tags — second byte under kNsCatalog.
// ---------------------------------------------------------------------------

inline constexpr uint8_t kSubCounter = 0x01;
inline constexpr uint8_t kSubTable   = 0x02;

// ---------------------------------------------------------------------------
// Counter IDs (third byte under kSubCounter).
// ---------------------------------------------------------------------------

inline constexpr uint8_t kCounterTableId = 0x01;

// ---------------------------------------------------------------------------
// Key builders
// ---------------------------------------------------------------------------

// Returns [0x01 | 0x01 | id] — a 3-byte key for the given counter.
std::vector<uint8_t> counter_key(uint8_t id);

// Returns [0x01 | 0x02 | db\0table\0] — catalog key for the named table.
// `name` is a MariaDB-style path like "./test/t1" or "test/t1".
// The function normalizes: strips leading "./" prefix, then encodes as
// "db\0table\0" (null-separated components).
std::vector<uint8_t> table_meta_key(const char *name);

// Returns the lower and upper bounds for scanning all table metadata keys.
// lower = [0x01 | 0x02], upper = [0x01 | 0x03].
// Use with an iterator: start at lower, stop when key >= upper.
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> table_meta_scan_bounds();

// ---------------------------------------------------------------------------
// Counter value encoding (big-endian uint64)
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_counter_value(uint64_t val);
uint64_t decode_counter_value(const uint8_t *data, std::size_t len);

// ---------------------------------------------------------------------------
// Table metadata
// ---------------------------------------------------------------------------

struct ColumnMeta {
  uint16_t field_type;
  uint16_t field_length;
  uint8_t  is_nullable;
  uint16_t charset_id;
};

struct IndexMeta {
  uint16_t index_id;                      // MariaDB key_info[] position
  uint16_t key_parts;                     // user_defined_key_parts
  uint8_t  is_unique;                     // unique constraint flag
  std::vector<uint16_t> column_indexes;   // fieldnr for each key part
};

struct FKMeta {
  std::string name;
  std::string ref_db;
  std::string ref_table;
  std::vector<std::string> fk_cols;
  std::vector<std::string> ref_cols;
  uint8_t update_opt{0};
  uint8_t delete_opt{0};
};

struct TableMeta {
  uint32_t    table_id;
  uint32_t    schema_version;
  std::string full_name;      // normalized "db\0table\0" form
  uint16_t    reclength;
  uint16_t    null_bytes;
  uint32_t    pk_parts;
  std::vector<ColumnMeta> columns;
  std::vector<IndexMeta>  indexes;        // non-PK secondary indexes
  std::vector<FKMeta>     fks;            // FK constraint names (DDL lifecycle only)
};

// Serializes a TableMeta into a byte vector (all little-endian).
std::vector<uint8_t> serialize_table_meta(const TableMeta &meta);

// Deserializes a TableMeta from a byte buffer. Returns false on malformed data.
bool deserialize_table_meta(const uint8_t *data, std::size_t len,
                            TableMeta &out);

// ---------------------------------------------------------------------------
// Name normalization
// ---------------------------------------------------------------------------

// Normalizes a MariaDB table path (e.g. "./test/t1") into the canonical
// form used as the suffix of table_meta_key: "test\0t1\0".
std::string normalize_table_name(const char *name);

} // namespace bytecaskdb
