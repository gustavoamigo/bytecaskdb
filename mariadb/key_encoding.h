// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// key_encoding.h — Primary key encoding for ByteCaskDB / MariaDB plugin.
//
// All keys stored in the global ByteCaskDB instance are prefixed with a
// 4-byte big-endian table_id so that different tables never collide.
//
// Key format (Phase 1):
//   [table_id: 4 bytes, big-endian][pk_columns: variable, MariaDB key format]

#pragma once

#include "my_global.h"
#include "handler.h"

#include <cstdint>
#include <vector>

namespace bytecaskdb {

// Encodes the primary-key columns of the row at `buf` (table->record[0] or
// table->record[1]) into a ByteCaskDB key: 4-byte big-endian table_id followed
// by MariaDB's internal key representation (produced by key_copy()).
//
// Returns the encoded key as a byte vector.
std::vector<uint8_t> encode_pk(TABLE *table, const uchar *buf,
                                uint32_t table_id);

// Decodes a previously encoded key back into the row buffer `buf` using
// key_restore().  Strips the 4-byte table_id prefix first.
void decode_pk(TABLE *table, const uint8_t *key, std::size_t key_len,
               uchar *buf);

// Returns the 4-byte big-endian representation of table_id, which serves
// as the lower-bound prefix for iterating a table's keys.
std::vector<uint8_t> table_id_prefix(uint32_t table_id);

// Returns true if `key` (of `key_len` bytes) belongs to the given table
// (i.e. its first 4 bytes match the big-endian table_id).
bool key_belongs_to_table(const uint8_t *key, std::size_t key_len,
                           uint32_t table_id);

} // namespace bytecaskdb
