// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_encoding.h — Row encoding for the ByteCaskDB / MariaDB plugin.
//
// V2 compact format (sequential per-field):
//   byte 0:      0x02 (format version)
//   bytes 1-2:   schema_version (LE uint16)
//   bytes 3+:    null bitmap (null_bytes), then each field sequentially:
//     - compactable CHAR (multibyte charset): [actual_len: LE u16][data]
//     - all others: [data: pack_length() bytes] verbatim from record offset
//     - BLOB: inline data appended after all fields

#pragma once

#include "my_global.h"
#include "handler.h"

#include <cstdint>
#include <vector>

namespace bytecaskdb {

// Encodes the row in `buf` (table->record[0]) into a byte vector.
// Uses V2 compact format: strips trailing spaces from multi-byte CHAR fields.
void encode_row_into(std::vector<uint8_t> &out, TABLE *table, const uchar *buf,
                     uint16_t schema_version);

std::vector<uint8_t> encode_row(TABLE *table, const uchar *buf,
                                uint16_t schema_version);

// Decodes a V2-encoded row value back into `buf` (table->record[0]).
// Fails (zeros buf) if the format byte is not 0x02.
void decode_row(TABLE *table, const uint8_t *value, std::size_t value_len,
                uchar *buf);

} // namespace bytecaskdb
