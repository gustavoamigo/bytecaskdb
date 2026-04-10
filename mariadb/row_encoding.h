// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_encoding.h — Row encoding for the ByteCaskDB / MariaDB plugin.
//
// Phase 1 strategy: store MariaDB's raw internal row buffer directly as the
// ByteCaskDB value.  This is the simplest approach — no custom serialization,
// no schema-awareness.  The format is MariaDB-specific and tied to the table
// schema (reclength).
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
// Phase 1: copies table->s->reclength raw bytes from buf.
std::vector<uint8_t> encode_row(TABLE *table, const uchar *buf);

// Decodes a previously encoded row value back into `buf` (table->record[0]).
// Phase 1: memcpy of min(value_len, reclength) bytes.  Pads remaining bytes
// with zeros if the stored value is unexpectedly short (defensive).
void decode_row(TABLE *table, const uint8_t *value, std::size_t value_len,
                uchar *buf);

} // namespace bytecaskdb
