// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// catalog.cc — Persistent catalog encoding implementation.

#include "catalog.h"

#include <cstring>

namespace bytecaskdb {

namespace {

void write_le16(uint8_t *out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}

void write_le32(uint8_t *out, uint32_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >>  8) & 0xFF);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t read_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0])       |
         (static_cast<uint32_t>(p[1]) << 8)  |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void write_be64(uint8_t *out, uint64_t v) {
  out[0] = static_cast<uint8_t>((v >> 56) & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 48) & 0xFF);
  out[2] = static_cast<uint8_t>((v >> 40) & 0xFF);
  out[3] = static_cast<uint8_t>((v >> 32) & 0xFF);
  out[4] = static_cast<uint8_t>((v >> 24) & 0xFF);
  out[5] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[6] = static_cast<uint8_t>((v >>  8) & 0xFF);
  out[7] = static_cast<uint8_t>( v        & 0xFF);
}

uint64_t read_be64(const uint8_t *p) {
  return (static_cast<uint64_t>(p[0]) << 56) |
         (static_cast<uint64_t>(p[1]) << 48) |
         (static_cast<uint64_t>(p[2]) << 40) |
         (static_cast<uint64_t>(p[3]) << 32) |
         (static_cast<uint64_t>(p[4]) << 24) |
         (static_cast<uint64_t>(p[5]) << 16) |
         (static_cast<uint64_t>(p[6]) <<  8) |
         (static_cast<uint64_t>(p[7]));
}

} // namespace

// ---------------------------------------------------------------------------
// Key builders
// ---------------------------------------------------------------------------

std::vector<uint8_t> counter_key(uint8_t id) {
  return {kNsCatalog, kSubCounter, id};
}

std::vector<uint8_t> table_meta_key(const char *name) {
  auto normalized = normalize_table_name(name);
  std::vector<uint8_t> key;
  key.reserve(2 + normalized.size());
  key.push_back(kNsCatalog);
  key.push_back(kSubTable);
  key.insert(key.end(),
             reinterpret_cast<const uint8_t *>(normalized.data()),
             reinterpret_cast<const uint8_t *>(normalized.data() + normalized.size()));
  return key;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> table_meta_scan_bounds() {
  return {{kNsCatalog, kSubTable},
          {kNsCatalog, static_cast<uint8_t>(kSubTable + 1)}};
}

// ---------------------------------------------------------------------------
// Counter value encoding
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_counter_value(uint64_t val) {
  std::vector<uint8_t> buf(8);
  write_be64(buf.data(), val);
  return buf;
}

uint64_t decode_counter_value(const uint8_t *data, std::size_t len) {
  if (len < 8) {
    return 0;
  }
  return read_be64(data);
}

// ---------------------------------------------------------------------------
// Name normalization
// ---------------------------------------------------------------------------

std::string normalize_table_name(const char *name) {
  std::string s(name);

  // Strip leading "./" if present.
  if (s.size() >= 2 && s[0] == '.' && s[1] == '/') {
    s = s.substr(2);
  }

  // Strip leading "/" if present.
  if (!s.empty() && s[0] == '/') {
    s = s.substr(1);
  }

  // Replace '/' with '\0' to form null-separated components,
  // and append a trailing '\0'.
  std::string result;
  result.reserve(s.size() + 1);
  for (char c : s) {
    if (c == '/') {
      result.push_back('\0');
    } else {
      result.push_back(c);
    }
  }
  result.push_back('\0');

  return result;
}

// ---------------------------------------------------------------------------
// Table metadata serialization
//
// Binary format (all little-endian):
//   [table_id:       4 bytes]
//   [schema_version: 4 bytes]
//   [name_len:       2 bytes]
//   [full_name:      name_len bytes]
//   [reclength:      2 bytes]
//   [null_bytes:     2 bytes]
//   [pk_parts:       4 bytes]
//   [col_count:      4 bytes]
//   For each column:
//     [field_type:   2 bytes]
//     [field_length: 2 bytes]
//     [is_nullable:  1 byte]
//     [charset_id:   2 bytes]
// ---------------------------------------------------------------------------

std::vector<uint8_t> serialize_table_meta(const TableMeta &meta) {
  // Fixed header: 4 + 4 + 2 + name + 2 + 2 + 4 + 4 = 22 + name
  // Per column: 2 + 2 + 1 + 2 = 7
  auto name_len = static_cast<uint16_t>(meta.full_name.size());
  auto col_count = static_cast<uint32_t>(meta.columns.size());
  std::size_t total = 22 + name_len + col_count * 7;

  std::vector<uint8_t> buf(total);
  uint8_t *p = buf.data();

  write_le32(p, meta.table_id);       p += 4;
  write_le32(p, meta.schema_version); p += 4;
  write_le16(p, name_len);            p += 2;
  std::memcpy(p, meta.full_name.data(), name_len); p += name_len;
  write_le16(p, meta.reclength);      p += 2;
  write_le16(p, meta.null_bytes);     p += 2;
  write_le32(p, meta.pk_parts);       p += 4;
  write_le32(p, col_count);           p += 4;

  for (const auto &col : meta.columns) {
    write_le16(p, col.field_type);    p += 2;
    write_le16(p, col.field_length);  p += 2;
    *p++ = col.is_nullable;
    write_le16(p, col.charset_id);    p += 2;
  }

  return buf;
}

bool deserialize_table_meta(const uint8_t *data, std::size_t len,
                            TableMeta &out) {
  // Minimum: 22 bytes (header with empty name and zero columns).
  if (len < 22) {
    return false;
  }

  const uint8_t *p = data;

  out.table_id       = read_le32(p); p += 4;
  out.schema_version = read_le32(p); p += 4;

  uint16_t name_len = read_le16(p); p += 2;
  if (static_cast<std::size_t>(p - data) + name_len + 12 > len) {
    return false;
  }
  out.full_name.assign(reinterpret_cast<const char *>(p), name_len); p += name_len;

  out.reclength  = read_le16(p); p += 2;
  out.null_bytes = read_le16(p); p += 2;
  out.pk_parts   = read_le32(p); p += 4;

  uint32_t col_count = read_le32(p); p += 4;

  std::size_t remaining = len - static_cast<std::size_t>(p - data);
  if (remaining < col_count * 7) {
    return false;
  }

  out.columns.resize(col_count);
  for (uint32_t i = 0; i < col_count; ++i) {
    out.columns[i].field_type   = read_le16(p); p += 2;
    out.columns[i].field_length = read_le16(p); p += 2;
    out.columns[i].is_nullable  = *p++;
    out.columns[i].charset_id   = read_le16(p); p += 2;
  }

  return true;
}

} // namespace bytecaskdb
