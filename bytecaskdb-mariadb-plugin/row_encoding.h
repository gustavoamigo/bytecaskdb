// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_encoding.h — Row encoding for the ByteCaskDB / MariaDB plugin.
//
// Row values are stored with a 3-byte version envelope:
//   byte 0:   format version (currently 0x01)
//   bytes 1-2: schema_version (little-endian uint16)
//   bytes 3+:  MariaDB raw internal row buffer (reclength bytes)
//
// Phase 4+ will introduce a compact custom encoding for schema evolution and
// covering secondary indexes.

#pragma once

#include "my_global.h"
#include "handler.h"

#include <cstdint>
#include <vector>

namespace bytecaskdb {

// Encodes the row in `buf` (table->record[0]) into a byte vector.
// Prepends a 3-byte envelope: [format=0x01][schema_version LE u16].
std::vector<uint8_t> encode_row(TABLE *table, const uchar *buf,
                                uint16_t schema_version);

// Decodes a previously encoded row value back into `buf` (table->record[0]).
// Strips the 3-byte envelope, copies min(payload_len, reclength) bytes.
// Pads remaining bytes with zeros if the stored value is short (defensive).
void decode_row(TABLE *table, const uint8_t *value, std::size_t value_len,
                uchar *buf);

} // namespace bytecaskdb
