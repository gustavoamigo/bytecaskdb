// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// key_encoding.h — Primary key encoding for ByteCaskDB / MariaDB plugin.
//
// All keys stored in the global ByteCaskDB instance are prefixed with a
// namespace byte (0x02 for row data) followed by a 4-byte big-endian table_id,
// so that different tables and namespaces never collide.
//
// Key format:
//   [ns: 1 byte (0x02)][table_id: 4 bytes, big-endian][pk_columns: variable]
//
// Secondary index key format:
//   [ns: 1 byte (0x03)][table_id: 4 bytes, big-endian][index_id: 2 bytes, big-endian]
//   [index_columns: variable][pk_columns: variable]

#pragma once

#include "my_global.h"
#include "handler.h"
#include "field.h"
#include "row_encoding.h"

#include <cstdint>
#include <vector>

namespace bytecaskdb {

// How one key part moves between key_copy()'s bytes and the stored,
// mem-comparable form, read from its Field once (make_table_codec).
struct KeyPartPlan {
  enum class Flip : uint8_t { kNone, kSign, kFloat };
  uint32_t store_length{0};
  uint32_t length{0};
  uint32_t record_offset{0};  // the column's offset in the record
  bool nullable{false};       // a null byte precedes the value
  bool varchar{false};        // VARCHAR column
  bool varlen{false};         // VARCHAR or BLOB part: no byte transform
  bool pad_space{false};      // varlen under a PAD SPACE collation
  bool be_temporal{false};    // DATETIME2/TIMESTAMP2/TIME2: big-endian already
  bool reverse{false};        // little-endian in the record: stored reversed
  Flip flip{Flip::kNone};     // applied with reverse
  // Stored bytes order exactly as the server's key_cmp orders the part: a
  // non-null integer, DATE, YEAR or big-endian temporal. Strings compare by
  // collation and are stored as bytes; floats have -0.0 and 0.0.
  bool order_exact{false};
};

struct KeyPlan {
  std::vector<KeyPartPlan> parts;
  // Every part is order_exact: a range over this key can be bounded by its
  // encoded ends alone, with no key_cmp per row.
  bool order_exact{false};
};

// Everything the row and key codecs ask of a table's definition, read once
// when the handler opens the table and rebuilt on every open, so it never
// outlives the definition it was read from. Asking the Fields per row costs a
// virtual call per column per question.
struct TableCodec {
  RowPlan row;
  std::vector<KeyPlan> keys;  // by key number, as table->key_info
};

TableCodec make_table_codec(TABLE *table);

// Encodes the primary-key columns of the row at `buf` (table->record[0] or
// table->record[1]) into a ByteCaskDB key: namespace byte (0x02) + 4-byte
// big-endian table_id + MariaDB's internal key representation (key_copy()).
//
// For tables without a PRIMARY KEY (table->s->primary_key == MAX_KEY), the
// caller supplies an 8-byte synthetic rowid which is appended big-endian
// after the table_id prefix. Result is exactly 13 bytes for PK-less tables.

// Hot-path overload: writes encoded bytes into `out`, reusing capacity.
void encode_pk_into(std::vector<uint8_t> &out, TABLE *table,
                    const TableCodec &codec, const uchar *buf,
                    uint32_t table_id, uint64_t synthetic_rowid = 0);

// Returning overload: thin wrapper for non-hot callers/tests.
std::vector<uint8_t> encode_pk(TABLE *table, const TableCodec &codec,
                                const uchar *buf, uint32_t table_id,
                                uint64_t synthetic_rowid = 0);

// Decodes a previously encoded key back into the row buffer `buf` using
// key_restore().  Strips the 1-byte namespace + 4-byte table_id prefix first.
// `scratch` is a caller-owned buffer reused across calls to avoid per-row
// heap allocations.
void decode_pk(TABLE *table, const TableCodec &codec, const uint8_t *key,
               std::size_t key_len, uchar *buf, std::vector<uint8_t> &scratch);

// Returns the 5-byte prefix [0x02 | table_id(BE,4)] for iterating a table's
// row keys.
std::vector<uint8_t> table_id_prefix(uint32_t table_id);

// Returns true if `key` (of `key_len` bytes) belongs to the given table's
// row namespace (i.e. starts with [0x02 | table_id(BE,4)]).
bool key_belongs_to_table(const uint8_t *key, std::size_t key_len,
                           uint32_t table_id);

// Returns the exclusive upper bound [0x02 | (table_id+1)(BE,4)] for bounded
// iteration over a table's keys.
std::vector<uint8_t> table_id_upper_bound(uint32_t table_id);

// Encodes a secondary index key: namespace (0x03) + table_id + index_id +
// packed secondary key columns + embedded primary key (or synthetic rowid).
// Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key_packed | pk_packed]
//
// For PK-less tables, the caller supplies an 8-byte synthetic rowid which is
// appended big-endian in place of the packed PK suffix.
void encode_sec_key_into(std::vector<uint8_t> &out, TABLE *table,
                          const TableCodec &codec, const uchar *buf,
                          uint32_t table_id, uint16_t index_id,
                          uint active_index,
                          uint64_t synthetic_rowid = 0);

std::vector<uint8_t> encode_sec_key(TABLE *table, const TableCodec &codec,
                                     const uchar *buf,
                                     uint32_t table_id, uint16_t index_id,
                                     uint active_index,
                                     uint64_t synthetic_rowid = 0);

// Returns the byte length of the trailing PK / rowid suffix used by
// encode_pk and encode_sec_key. 8 for PK-less tables, key_length otherwise.
uint pk_suffix_length(TABLE *table);

// Reads a 8-byte big-endian unsigned integer from `p`.
uint64_t read_be64(const uint8_t *p);

// Extracts the primary key portion from a secondary index key.
// sec_key_packed_len is the length of the secondary key portion only.
// Returns a pointer to the PK portion within sec_key and writes the length
// to *pk_len_out. Returns nullptr if the key is too short.
const uint8_t *extract_pk_from_sec_key(const uint8_t *sec_key,
                                        std::size_t sec_key_len,
                                        uint sec_key_packed_len,
                                        std::size_t *pk_len_out);

// Returns the 7-byte prefix [0x03 | table_id(BE,4) | index_id(BE,2)] for
// iterating a specific secondary index.
std::vector<uint8_t> index_id_prefix(uint32_t table_id, uint16_t index_id);

// Returns the exclusive upper bound for secondary index iteration.
std::vector<uint8_t> index_id_upper_bound(uint32_t table_id, uint16_t index_id);

// Returns true if `key` belongs to the specified secondary index.
bool key_belongs_to_index(const uint8_t *key, std::size_t len,
                           uint32_t table_id, uint16_t index_id);

// Encodes a unique secondary index key for duplicate checking.
// Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key] (no PK suffix)
void encode_unique_sec_key_into(std::vector<uint8_t> &out,
                                 TABLE *table, const TableCodec &codec,
                                 const uchar *buf,
                                 uint32_t table_id, uint16_t index_id,
                                 uint active_index);

std::vector<uint8_t> encode_unique_sec_key(TABLE *table,
                                            const TableCodec &codec,
                                            const uchar *buf,
                                            uint32_t table_id, uint16_t index_id,
                                            uint active_index);

// Removes the VARCHAR length prefix from packed secondary-index key bytes,
// converting key_copy() output to the same format used by encode_sec_key().
// Must be called on the key bytes starting immediately after the 7-byte prefix
// (kNsIndex + table_id + index_id). Used by index_read_map() to bring
// the optimizer-supplied search key into the same encoding as stored keys.
void fix_varchar_key_encoding(uint8_t *key_data, const KeyPlan &plan);

// Normalizes VARCHAR key parts for PAD SPACE collations in PK encoding.
// Strips trailing 0x20 bytes and updates the 2-byte LE length prefix so that
// 'a' and 'a ' produce identical encoded keys.
void normalize_padspace_pk(uint8_t *key_data, const KeyPlan &plan);

// Transforms key_copy() output (native little-endian integers) into a
// mem-comparable encoding (big-endian, sign-bit flipped for signed types).
// Operates in-place on the key_data starting immediately after any prefix
// (5-byte for PK, 7-byte for secondary). Only affects integer field types;
// VARCHARs and other types are left untouched.
void make_mem_comparable(uint8_t *key_data, const KeyPlan &plan, uint key_len);

// Reverses make_mem_comparable(): restores native little-endian format from
// the mem-comparable encoding. Must be called before key_restore().
void undo_mem_comparable(uint8_t *key_data, const KeyPlan &plan, uint key_len);

// Decodes the key parts of `sec_key` directly into `record` for the columns
// covered by `plan`. Used to short-circuit the secondary→PK row fetch
// when the optimizer signals HA_EXTRA_KEYREAD.
//
// Returns true on success. Returns false if any key part is of a type the
// decoder does not (yet) handle (VARCHAR, BLOB, CHAR, nullable parts) — the
// caller must fall back to the full row fetch in that case.
//
// Initial scope: non-null fixed-width integers (signed + unsigned), floats,
// and temporal types whose mem-comparable transform is fully reversible.
bool decode_sec_key_into_record(const KeyPlan &plan,
                                 const uint8_t *sec_key, std::size_t sec_key_len,
                                 uchar *record);

} // namespace bytecaskdb
