// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_encoding.cc — Row encoding implementation with version envelope.

#include "row_encoding.h"
#include "table.h"   // full TABLE / TABLE_SHARE definitions
#include "field.h"   // Field_blob

#include <cstring>

namespace bytecaskdb {

// Row format version.  Increment when the encoding changes.
static constexpr uint8_t kRowFormatV1 = 0x01;

// Envelope size: 1 byte format + 2 bytes schema_version.
static constexpr std::size_t kEnvelopeSize = 3;

void encode_row_into(std::vector<uint8_t> &out, TABLE *table, const uchar *buf,
                     uint16_t schema_version) {
  const std::size_t len = table->s->reclength;

  // First pass: compute total BLOB data size.
  std::size_t blob_total = 0;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field->type() == MYSQL_TYPE_BLOB) {
      auto *blob = static_cast<Field_blob *>(field);
      std::size_t offset = field->ptr - table->record[0];
      uint32_t blob_len = blob->get_length(buf + offset);
      blob_total += blob_len;
    }
  }

  out.clear();
  out.resize(kEnvelopeSize + len + blob_total);

  // Envelope.
  out[0] = kRowFormatV1;
  out[1] = static_cast<uint8_t>(schema_version & 0xFF);
  out[2] = static_cast<uint8_t>((schema_version >> 8) & 0xFF);

  // Raw row data (includes BLOB length fields and stale pointers).
  std::memcpy(out.data() + kEnvelopeSize, buf, len);

  // Append actual BLOB data after the record.
  uint8_t *blob_dst = out.data() + kEnvelopeSize + len;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field->type() == MYSQL_TYPE_BLOB) {
      auto *blob = static_cast<Field_blob *>(field);
      std::size_t offset = field->ptr - table->record[0];
      uint32_t blob_len = blob->get_length(buf + offset);
      if (blob_len > 0) {
        const uchar *data_ptr = nullptr;
        std::memcpy(&data_ptr, buf + offset + blob->pack_length_no_ptr(),
                    sizeof(data_ptr));
        if (data_ptr) {
          std::memcpy(blob_dst, data_ptr, blob_len);
        }
      }
      blob_dst += blob_len;
    }
  }
}

std::vector<uint8_t> encode_row(TABLE *table, const uchar *buf,
                                uint16_t schema_version) {
  std::vector<uint8_t> out;
  encode_row_into(out, table, buf, schema_version);
  return out;
}

void decode_row(TABLE *table, const uint8_t *value, std::size_t value_len,
                uchar *buf) {
  const std::size_t reclength = table->s->reclength;

  // Strip the envelope.
  if (value_len < kEnvelopeSize) {
    std::memset(buf, 0, reclength);
    return;
  }
  const uint8_t *payload = value + kEnvelopeSize;
  const std::size_t payload_len = value_len - kEnvelopeSize;

  const std::size_t copy_len = std::min(payload_len, reclength);

  if (copy_len > 0) {
    std::memcpy(buf, payload, copy_len);
  }

  // Zero-pad if the stored payload is shorter than the current reclength.
  if (copy_len < reclength) {
    std::memset(buf + copy_len, 0, reclength - copy_len);
  }

  // Fix up BLOB pointers to point into the value buffer (after the record).
  const uint8_t *blob_src = payload + reclength;
  const uint8_t *blob_end = value + value_len;
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field->type() == MYSQL_TYPE_BLOB) {
      auto *blob = static_cast<Field_blob *>(field);
      std::size_t offset = field->ptr - table->record[0];
      uint32_t blob_len = blob->get_length(buf + offset);
      const uchar *ptr = (blob_src + blob_len <= blob_end) ? blob_src : nullptr;
      std::memcpy(buf + offset + blob->pack_length_no_ptr(), &ptr, sizeof(ptr));
      blob_src += blob_len;
    }
  }
}

} // namespace bytecaskdb
