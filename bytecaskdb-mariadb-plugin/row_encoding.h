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

// A table's column layout, read from its Fields once (make_row_plan) so the
// row codecs below do not ask every Field again on every row: each question
// is a virtual call into the server, and none of the answers change between
// rows. Built when the handler opens the table; offsets are relative to the
// record, so one plan serves record[0] and record[1].
struct RowPlan {
  enum class Kind : uint8_t {
    kFixed,        // pack_length bytes, verbatim
    kBlob,         // length + pointer in the record; data after the fields
    kCompactChar,  // multibyte CHAR stored without its trailing spaces
  };
  struct Column {
    uint32_t offset{0};         // in the record
    uint32_t pack_length{0};    // kFixed, kBlob: bytes in the record
    uint32_t field_length{0};   // kCompactChar: the column's byte length
    uint32_t mbmaxlen{1};       // kCompactChar: the charset's widest character
    uint32_t blob_len_bytes{0}; // kBlob: bytes of the length before the pointer
    Kind kind{Kind::kFixed};
  };
  std::size_t null_bytes{0};
  std::size_t reclength{0};
  std::vector<Column> columns;
};

RowPlan make_row_plan(TABLE *table);

// Encodes the row in `buf` (table->record[0] or record[1]) into a byte vector.
// Uses V2 compact format: strips trailing spaces from multi-byte CHAR fields.
void encode_row_into(std::vector<uint8_t> &out, const RowPlan &plan,
                     const uchar *buf, uint16_t schema_version);

std::vector<uint8_t> encode_row(const RowPlan &plan, const uchar *buf,
                                uint16_t schema_version);

// Decodes a V2-encoded row value back into `buf`. A value shorter than the
// envelope zeros buf; a format byte other than 0x02 throws.
void decode_row(const RowPlan &plan, const uint8_t *value,
                std::size_t value_len, uchar *buf);

} // namespace bytecaskdb
