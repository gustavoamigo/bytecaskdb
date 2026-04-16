// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// row_encoding.cc — Row encoding implementation with version envelope.

#include "row_encoding.h"
#include "table.h"   // full TABLE / TABLE_SHARE definitions

#include <cstring>

namespace bytecaskdb {

// Row format version.  Increment when the encoding changes.
static constexpr uint8_t kRowFormatV1 = 0x01;

// Envelope size: 1 byte format + 2 bytes schema_version.
static constexpr std::size_t kEnvelopeSize = 3;

std::vector<uint8_t> encode_row(TABLE *table, const uchar *buf,
                                uint16_t schema_version) {
  const std::size_t len = table->s->reclength;
  std::vector<uint8_t> out(kEnvelopeSize + len);

  // Envelope.
  out[0] = kRowFormatV1;
  out[1] = static_cast<uint8_t>(schema_version & 0xFF);
  out[2] = static_cast<uint8_t>((schema_version >> 8) & 0xFF);

  // Raw row data.
  std::memcpy(out.data() + kEnvelopeSize, buf, len);
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
}

} // namespace bytecaskdb
