// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// key_encoding.cc — Primary key encoding implementation.

#include "key_encoding.h"
#include "catalog.h"   // kNsRow
#include "table.h"     // full TABLE / TABLE_SHARE definitions
#include "key.h"       // key_copy, key_restore

#include <cstring>

namespace bytecaskdb {

namespace {

// Writes the 4-byte big-endian representation of `id` into `out`.
void write_be32(uint8_t *out, uint32_t id) {
  out[0] = static_cast<uint8_t>((id >> 24) & 0xFF);
  out[1] = static_cast<uint8_t>((id >> 16) & 0xFF);
  out[2] = static_cast<uint8_t>((id >>  8) & 0xFF);
  out[3] = static_cast<uint8_t>( id        & 0xFF);
}

// Returns the maximum byte length of the primary key for `table`.
// MariaDB stores this in table->key_info[0].key_length for the primary index.
uint pk_key_length(TABLE *table) {
  if (table->s->primary_key == MAX_KEY) {
    return 0;
  }
  return table->key_info[table->s->primary_key].key_length;
}

} // namespace

std::vector<uint8_t> encode_pk(TABLE *table, const uchar *buf,
                                uint32_t table_id) {
  const uint pk_idx = table->s->primary_key;
  const uint pk_len = pk_key_length(table);

  std::vector<uint8_t> key(5 + pk_len);

  // Write namespace byte + table_id prefix.
  key[0] = kNsRow;
  write_be32(key.data() + 1, table_id);

  if (pk_idx != MAX_KEY && pk_len > 0) {
    key_copy(key.data() + 5,
             const_cast<uchar *>(buf),
             &table->key_info[pk_idx],
             pk_len);
  }

  return key;
}

void decode_pk(TABLE *table, const uint8_t *key, std::size_t key_len,
               uchar *buf) {
  const uint pk_idx = table->s->primary_key;
  if (pk_idx == MAX_KEY || key_len <= 5) {
    return;
  }
  // Strip the 1-byte namespace + 4-byte table_id prefix before key_restore().
  key_restore(buf,
              reinterpret_cast<const uchar *>(key + 5),
              &table->key_info[pk_idx],
              static_cast<uint>(key_len - 5));
}

std::vector<uint8_t> table_id_prefix(uint32_t table_id) {
  std::vector<uint8_t> prefix(5);
  prefix[0] = kNsRow;
  write_be32(prefix.data() + 1, table_id);
  return prefix;
}

bool key_belongs_to_table(const uint8_t *key, std::size_t key_len,
                           uint32_t table_id) {
  if (key_len < 5 || key[0] != kNsRow) {
    return false;
  }
  uint8_t expected[4];
  write_be32(expected, table_id);
  return std::memcmp(key + 1, expected, 4) == 0;
}

std::vector<uint8_t> table_id_upper_bound(uint32_t table_id) {
  std::vector<uint8_t> bound(5);
  bound[0] = kNsRow;
  write_be32(bound.data() + 1, table_id + 1);
  return bound;
}

} // namespace bytecaskdb
