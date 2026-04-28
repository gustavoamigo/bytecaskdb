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

#include <cstdint>
#include <vector>

namespace bytecaskdb {

// Encodes the primary-key columns of the row at `buf` (table->record[0] or
// table->record[1]) into a ByteCaskDB key: namespace byte (0x02) + 4-byte
// big-endian table_id + MariaDB's internal key representation (key_copy()).
//
// Returns the encoded key as a byte vector.
std::vector<uint8_t> encode_pk(TABLE *table, const uchar *buf,
                                uint32_t table_id);

// Decodes a previously encoded key back into the row buffer `buf` using
// key_restore().  Strips the 1-byte namespace + 4-byte table_id prefix first.
void decode_pk(TABLE *table, const uint8_t *key, std::size_t key_len,
               uchar *buf);

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
// packed secondary key columns + embedded primary key.
// Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key_packed | pk_packed]
std::vector<uint8_t> encode_sec_key(TABLE *table, const uchar *buf,
                                     uint32_t table_id, uint16_t index_id,
                                     uint active_index);

// Extracts the primary key portion from a secondary index key.
// sec_key_packed_len is the length of the secondary key portion only.
std::vector<uint8_t> extract_pk_from_sec_key(const uint8_t *sec_key,
                                              std::size_t sec_key_len,
                                              uint sec_key_packed_len);

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
std::vector<uint8_t> encode_unique_sec_key(TABLE *table, const uchar *buf,
                                            uint32_t table_id, uint16_t index_id,
                                            uint active_index);

} // namespace bytecaskdb
