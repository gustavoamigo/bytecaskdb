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

// Writes the 8-byte big-endian representation of `v` into `out`.
void write_be64(uint8_t *out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

// Writes the 2-byte big-endian representation of `id` into `out`.
void write_be16(uint8_t *out, uint16_t id) {
  out[0] = static_cast<uint8_t>((id >> 8) & 0xFF);
  out[1] = static_cast<uint8_t>( id       & 0xFF);
}

// Reads a 2-byte big-endian value from `p`.
uint16_t read_be16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                               static_cast<uint16_t>(p[1]));
}

} // namespace

uint64_t read_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<uint64_t>(p[i]);
  }
  return v;
}

std::vector<uint8_t> encode_pk(TABLE *table, const uchar *buf,
                                uint32_t table_id,
                                uint64_t synthetic_rowid) {
  const uint pk_idx = table->s->primary_key;
  const uint suffix_len = pk_suffix_length(table);

  std::vector<uint8_t> key(5 + suffix_len);

  // Write namespace byte + table_id prefix.
  key[0] = kNsRow;
  write_be32(key.data() + 1, table_id);

  if (pk_idx == MAX_KEY) {
    write_be64(key.data() + 5, synthetic_rowid);
  } else if (suffix_len > 0) {
    key_copy(key.data() + 5,
             const_cast<uchar *>(buf),
             &table->key_info[pk_idx],
             suffix_len);
  }

  return key;
}

uint pk_suffix_length(TABLE *table) {
  if (table->s->primary_key == MAX_KEY) {
    return 8;  // synthetic rowid
  }
  return table->key_info[table->s->primary_key].key_length;
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

#ifndef BYTECASKDB_TESTS
// Fix VARCHAR key encoding by removing the 2-byte LE length prefix that key_copy()
// inserts (HA_KEY_BLOB_LENGTH = 2).  After stripping, the slot holds raw data
// left-justified with zero padding, giving correct lexicographic ordering.
// Only compiled when full MariaDB headers are available (not in test builds).
void fix_varchar_key_encoding(uint8_t *key_data, TABLE *table, uint active_index) {
  const KEY &key_info = table->key_info[active_index];
  uint8_t *key_ptr = key_data;

  for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
    const KEY_PART_INFO &kp = key_info.key_part[i];
    Field *field = table->field[kp.fieldnr - 1];

    if (field->type() == MYSQL_TYPE_VARCHAR) {
      // key_copy stores a 2-byte LE length prefix (HA_KEY_BLOB_LENGTH = 2).
      uint16_t length = static_cast<uint16_t>(key_ptr[0]) |
                        (static_cast<uint16_t>(key_ptr[1]) << 8);
      if (length > kp.length) length = kp.length;

      std::memmove(key_ptr, key_ptr + HA_KEY_BLOB_LENGTH, length);
      std::memset(key_ptr + length, 0, kp.store_length - length);
    }

    key_ptr += kp.store_length;
  }
}
#endif

std::vector<uint8_t> encode_sec_key(TABLE *table, const uchar *buf,
                                     uint32_t table_id, uint16_t index_id,
                                     uint active_index,
                                     uint64_t synthetic_rowid) {
  const KEY &key_info = table->key_info[active_index];
  const uint sec_key_len = key_info.key_length;
  const uint suffix_len = pk_suffix_length(table);

  // Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key | pk-or-rowid]
  std::vector<uint8_t> key(7 + sec_key_len + suffix_len);

  // Namespace + table_id + index_id prefix.
  key[0] = kNsIndex;
  write_be32(key.data() + 1, table_id);
  write_be16(key.data() + 5, index_id);

  // Pack secondary key columns, then fix VARCHAR encoding for proper ordering.
  key_copy(key.data() + 7,
           const_cast<uchar *>(buf),
           const_cast<KEY *>(&key_info),
           sec_key_len);

  // Post-process to fix VARCHAR length prefix issue for lexicographic ordering
#ifndef BYTECASKDB_TESTS
  fix_varchar_key_encoding(key.data() + 7, table, active_index);
#endif

  // Append PK bytes for uniqueness, or synthetic rowid for PK-less tables.
  if (table->s->primary_key == MAX_KEY) {
    write_be64(key.data() + 7 + sec_key_len, synthetic_rowid);
  } else if (suffix_len > 0) {
    key_copy(key.data() + 7 + sec_key_len,
             const_cast<uchar *>(buf),
             &table->key_info[table->s->primary_key],
             suffix_len);
  }

  return key;
}

std::vector<uint8_t> extract_pk_from_sec_key(const uint8_t *sec_key,
                                              std::size_t sec_key_len,
                                              uint sec_key_packed_len) {
  if (sec_key_len <= 7 + sec_key_packed_len) {
    return {};
  }

  const uint8_t *pk_start = sec_key + 7 + sec_key_packed_len;
  std::size_t pk_len = sec_key_len - 7 - sec_key_packed_len;

  return std::vector<uint8_t>(pk_start, pk_start + pk_len);
}

std::vector<uint8_t> index_id_prefix(uint32_t table_id, uint16_t index_id) {
  std::vector<uint8_t> prefix(7);
  prefix[0] = kNsIndex;
  write_be32(prefix.data() + 1, table_id);
  write_be16(prefix.data() + 5, index_id);
  return prefix;
}

std::vector<uint8_t> index_id_upper_bound(uint32_t table_id, uint16_t index_id) {
  std::vector<uint8_t> bound(7);
  bound[0] = kNsIndex;
  write_be32(bound.data() + 1, table_id);
  write_be16(bound.data() + 5, index_id + 1);
  return bound;
}

bool key_belongs_to_index(const uint8_t *key, std::size_t len,
                           uint32_t table_id, uint16_t index_id) {
  if (len < 7 || key[0] != kNsIndex) {
    return false;
  }

  uint8_t expected_prefix[6];
  write_be32(expected_prefix, table_id);
  write_be16(expected_prefix + 4, index_id);

  return std::memcmp(key + 1, expected_prefix, 6) == 0;
}

std::vector<uint8_t> encode_unique_sec_key(TABLE *table, const uchar *buf,
                                            uint32_t table_id, uint16_t index_id,
                                            uint active_index) {
  const KEY &key_info = table->key_info[active_index];
  const uint sec_key_len = key_info.key_length;

  // Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key] (no PK for uniqueness)
  std::vector<uint8_t> key(7 + sec_key_len);

  // Namespace + table_id + index_id prefix.
  key[0] = kNsIndex;
  write_be32(key.data() + 1, table_id);
  write_be16(key.data() + 5, index_id);

  // Pack secondary key columns only.
  key_copy(key.data() + 7,
           const_cast<uchar *>(buf),
           const_cast<KEY *>(&key_info),
           sec_key_len);

  // Fix VARCHAR encoding to match the format used by encode_sec_key.
#ifndef BYTECASKDB_TESTS
  fix_varchar_key_encoding(key.data() + 7, table, active_index);
#endif

  return key;
}

} // namespace bytecaskdb
