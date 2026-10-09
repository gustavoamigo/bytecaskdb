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

// A blob's length, stored little-endian in the record ahead of its pointer.
static uint32_t blob_length(const uchar *p, uint32_t len_bytes) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < len_bytes; ++i) {
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  }
  return v;
}

RowPlan make_row_plan(TABLE *table) {
  RowPlan plan;
  plan.null_bytes = table->s->null_bytes;
  plan.reclength = table->s->reclength;
  plan.columns.reserve(table->s->fields);
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    RowPlan::Column c;
    c.offset = static_cast<uint32_t>(field->ptr - table->record[0]);
    c.pack_length = field->pack_length();
    if (field->type() == MYSQL_TYPE_BLOB) {
      c.kind = RowPlan::Kind::kBlob;
      c.blob_len_bytes = static_cast<Field_blob *>(field)->pack_length_no_ptr();
    } else if (is_compactable(field)) {
      c.kind = RowPlan::Kind::kCompactChar;
      c.field_length = field->field_length;
      c.mbmaxlen = field->charset()->mbmaxlen;
    }
    plan.columns.push_back(c);
  }
  return plan;
}

void encode_row_into(std::vector<uint8_t> &out, const RowPlan &plan,
                     const uchar *buf, uint16_t schema_version) {
  using Kind = RowPlan::Kind;

  // First pass: compute output size.
  std::size_t data_size = plan.null_bytes;
  std::size_t blob_total = 0;
  for (const auto &c : plan.columns) {
    switch (c.kind) {
    case Kind::kBlob:
      blob_total += blob_length(buf + c.offset, c.blob_len_bytes);
      data_size += c.pack_length;
      break;
    case Kind::kCompactChar:
      data_size += 2 + strip_trailing_spaces(buf + c.offset, c.field_length,
                                             c.mbmaxlen);
      break;
    case Kind::kFixed:
      data_size += c.pack_length;
      break;
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
  std::memcpy(dst, buf, plan.null_bytes);
  dst += plan.null_bytes;

  // Fields. A blob's length/pointer metadata is stored as it is in the record.
  for (const auto &c : plan.columns) {
    if (c.kind == Kind::kCompactChar) {
      uint16_t actual_len =
          strip_trailing_spaces(buf + c.offset, c.field_length, c.mbmaxlen);
      dst[0] = static_cast<uint8_t>(actual_len & 0xFF);
      dst[1] = static_cast<uint8_t>((actual_len >> 8) & 0xFF);
      dst += 2;
      std::memcpy(dst, buf + c.offset, actual_len);
      dst += actual_len;
    } else {
      std::memcpy(dst, buf + c.offset, c.pack_length);
      dst += c.pack_length;
    }
  }

  // Append BLOB inline data.
  for (const auto &c : plan.columns) {
    if (c.kind != Kind::kBlob) continue;
    uint32_t blob_len = blob_length(buf + c.offset, c.blob_len_bytes);
    if (blob_len > 0) {
      const uchar *data_ptr = nullptr;
      std::memcpy(&data_ptr, buf + c.offset + c.blob_len_bytes,
                  sizeof(data_ptr));
      if (data_ptr) {
        std::memcpy(dst, data_ptr, blob_len);
      }
    }
    dst += blob_len;
  }
}

std::vector<uint8_t> encode_row(const RowPlan &plan, const uchar *buf,
                                uint16_t schema_version) {
  std::vector<uint8_t> out;
  encode_row_into(out, plan, buf, schema_version);
  return out;
}

void decode_row(const RowPlan &plan, const uint8_t *value,
                std::size_t value_len, uchar *buf) {
  using Kind = RowPlan::Kind;

  if (value_len < kEnvelopeSize) {
    std::memset(buf, 0, plan.reclength);
    return;
  }

  if (value[0] != kRowFormatV2) {
    throw std::runtime_error("unsupported row format version");
  }

  const uint8_t *src = value + kEnvelopeSize;
  const uint8_t *end = value + value_len;

  // Zero the buffer first so uninitialized gaps are clean.
  std::memset(buf, 0, plan.reclength);

  // Null bitmap.
  if (src + plan.null_bytes > end) return;
  std::memcpy(buf, src, plan.null_bytes);
  src += plan.null_bytes;

  // Fields.
  for (const auto &c : plan.columns) {
    if (c.kind == Kind::kCompactChar) {
      if (src + 2 > end) return;
      uint16_t actual_len = static_cast<uint16_t>(src[0]) |
                            (static_cast<uint16_t>(src[1]) << 8);
      src += 2;
      if (src + actual_len > end) return;
      std::memcpy(buf + c.offset, src, actual_len);
      // Pad remainder with 0x20 (space).
      if (actual_len < c.field_length) {
        std::memset(buf + c.offset + actual_len, 0x20,
                    c.field_length - actual_len);
      }
      src += actual_len;
    } else {
      if (src + c.pack_length > end) return;
      std::memcpy(buf + c.offset, src, c.pack_length);
      src += c.pack_length;
    }
  }

  // Fix up BLOB pointers to point into the value buffer (after fields).
  for (const auto &c : plan.columns) {
    if (c.kind != Kind::kBlob) continue;
    uint32_t blob_len = blob_length(buf + c.offset, c.blob_len_bytes);
    const uchar *ptr = (src + blob_len <= end) ? src : nullptr;
    std::memcpy(buf + c.offset + c.blob_len_bytes, &ptr, sizeof(ptr));
    src += blob_len;
  }
}

} // namespace bytecaskdb
