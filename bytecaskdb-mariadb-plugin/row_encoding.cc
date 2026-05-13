// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_encoding.cc — V2 compact row encoding with trailing-space stripping.

#include "row_encoding.h"
#include "table.h"   // full TABLE / TABLE_SHARE definitions
#include "field.h"   // Field_blob

#include <cstring>
#include <stdexcept>

namespace bytecaskdb {

static constexpr uint8_t kRowFormatV2 = 0x02;
static constexpr std::size_t kEnvelopeSize = 3;

static bool is_compactable(const Field *f) {
  if (f->type() != MYSQL_TYPE_STRING) return false;
  if (f->binary()) return false;
  const CHARSET_INFO *cs = f->charset();
  if (!cs) return false;
  return cs->mbminlen == 1 && cs->mbmaxlen > 1;
}

static uint16_t strip_trailing_spaces(const uchar *data, uint field_length,
                                      uint mbmaxlen) {
  uint n_chars = field_length / mbmaxlen;
  uint len = field_length;
  while (len > n_chars && data[len - 1] == 0x20) {
    --len;
  }
  return static_cast<uint16_t>(len);
}

void encode_row_into(std::vector<uint8_t> &out, TABLE *table, const uchar *buf,
                     uint16_t schema_version) {
  const std::size_t null_bytes = table->s->null_bytes;

  // First pass: compute output size.
  std::size_t data_size = null_bytes;
  std::size_t blob_total = 0;

  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    std::size_t offset = field->ptr - table->record[0];

    if (field->type() == MYSQL_TYPE_BLOB) {
      auto *blob = static_cast<Field_blob *>(field);
      uint32_t blob_len = blob->get_length(buf + offset);
      blob_total += blob_len;
      data_size += field->pack_length();
    } else if (is_compactable(field)) {
      uint16_t actual_len = strip_trailing_spaces(
          buf + offset, field->field_length, field->charset()->mbmaxlen);
      data_size += 2 + actual_len;
    } else {
      data_size += field->pack_length();
    }
  }

  out.clear();
  out.resize(kEnvelopeSize + data_size + blob_total);

  // Envelope.
  out[0] = kRowFormatV2;
  out[1] = static_cast<uint8_t>(schema_version & 0xFF);
  out[2] = static_cast<uint8_t>((schema_version >> 8) & 0xFF);

  uint8_t *dst = out.data() + kEnvelopeSize;

  // Null bitmap.
  std::memcpy(dst, buf, null_bytes);
  dst += null_bytes;

  // Fields.
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    std::size_t offset = field->ptr - table->record[0];

    if (field->type() == MYSQL_TYPE_BLOB) {
      // Store the blob length/pointer metadata from the record buffer.
      std::memcpy(dst, buf + offset, field->pack_length());
      dst += field->pack_length();
    } else if (is_compactable(field)) {
      uint16_t actual_len = strip_trailing_spaces(
          buf + offset, field->field_length, field->charset()->mbmaxlen);
      dst[0] = static_cast<uint8_t>(actual_len & 0xFF);
      dst[1] = static_cast<uint8_t>((actual_len >> 8) & 0xFF);
      dst += 2;
      std::memcpy(dst, buf + offset, actual_len);
      dst += actual_len;
    } else {
      std::memcpy(dst, buf + offset, field->pack_length());
      dst += field->pack_length();
    }
  }

  // Append BLOB inline data.
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
          std::memcpy(dst, data_ptr, blob_len);
        }
      }
      dst += blob_len;
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
  const std::size_t null_bytes = table->s->null_bytes;

  if (value_len < kEnvelopeSize) {
    std::memset(buf, 0, reclength);
    return;
  }

  if (value[0] != kRowFormatV2) {
    throw std::runtime_error("unsupported row format version");
  }

  const uint8_t *src = value + kEnvelopeSize;
  const uint8_t *end = value + value_len;

  // Zero the buffer first so uninitialized gaps are clean.
  std::memset(buf, 0, reclength);

  // Null bitmap.
  if (src + null_bytes > end) return;
  std::memcpy(buf, src, null_bytes);
  src += null_bytes;

  // Fields.
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    std::size_t offset = field->ptr - table->record[0];

    if (field->type() == MYSQL_TYPE_BLOB) {
      uint pl = field->pack_length();
      if (src + pl > end) return;
      std::memcpy(buf + offset, src, pl);
      src += pl;
    } else if (is_compactable(field)) {
      if (src + 2 > end) return;
      uint16_t actual_len = static_cast<uint16_t>(src[0]) |
                            (static_cast<uint16_t>(src[1]) << 8);
      src += 2;
      if (src + actual_len > end) return;
      std::memcpy(buf + offset, src, actual_len);
      // Pad remainder with 0x20 (space).
      if (actual_len < field->field_length) {
        std::memset(buf + offset + actual_len, 0x20,
                    field->field_length - actual_len);
      }
      src += actual_len;
    } else {
      uint pl = field->pack_length();
      if (src + pl > end) return;
      std::memcpy(buf + offset, src, pl);
      src += pl;
    }
  }

  // Fix up BLOB pointers to point into the value buffer (after fields).
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (field->type() == MYSQL_TYPE_BLOB) {
      auto *blob = static_cast<Field_blob *>(field);
      std::size_t offset = field->ptr - table->record[0];
      uint32_t blob_len = blob->get_length(buf + offset);
      const uchar *ptr = (src + blob_len <= end) ? src : nullptr;
      std::memcpy(buf + offset + blob->pack_length_no_ptr(), &ptr, sizeof(ptr));
      src += blob_len;
    }
  }
}

} // namespace bytecaskdb
