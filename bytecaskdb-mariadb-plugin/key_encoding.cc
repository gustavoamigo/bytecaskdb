// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// key_encoding.cc — Primary key encoding implementation.

#include "key_encoding.h"
#include "catalog.h"   // kNsRow
#include "table.h"     // full TABLE / TABLE_SHARE definitions
#include "key.h"       // key_copy, key_restore

#include <algorithm>
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
#ifndef BYTECASKDB_TESTS
    normalize_padspace_pk(key.data() + 5, &table->key_info[pk_idx]);
    make_mem_comparable(key.data() + 5, &table->key_info[pk_idx], suffix_len);
#endif
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
               uchar *buf, std::vector<uint8_t> &scratch) {
  const uint pk_idx = table->s->primary_key;
  if (pk_idx == MAX_KEY || key_len <= 5) {
    return;
  }
  // Strip the 1-byte namespace + 4-byte table_id prefix before key_restore().
  // Must undo mem-comparable encoding first since key_restore expects native format.
  uint pk_len = static_cast<uint>(key_len - 5);
  scratch.assign(key + 5, key + key_len);
#ifndef BYTECASKDB_TESTS
  undo_mem_comparable(scratch.data(), &table->key_info[pk_idx], pk_len);
#endif
  key_restore(buf, scratch.data(), &table->key_info[pk_idx], pk_len);
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
// For PAD SPACE collations, trailing spaces are stripped before zero-padding
// so that 'a' and 'a ' produce identical keys (SQL standard comparison semantics).
// Only compiled when full MariaDB headers are available (not in test builds).
void fix_varchar_key_encoding(uint8_t *key_data, TABLE *table, uint active_index) {
  const KEY &key_info = table->key_info[active_index];
  uint8_t *key_ptr = key_data;

  for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
    const KEY_PART_INFO &kp = key_info.key_part[i];
    Field *field = table->field[kp.fieldnr - 1];

    if (field->type() == MYSQL_TYPE_VARCHAR ||
        (kp.key_part_flag & HA_BLOB_PART)) {
      uint null_off = (kp.null_bit) ? 1 : 0;
      uint16_t length = static_cast<uint16_t>(key_ptr[null_off]) |
                        (static_cast<uint16_t>(key_ptr[null_off + 1]) << 8);
      if (length > kp.length) length = kp.length;

      std::memmove(key_ptr + null_off, key_ptr + null_off + HA_KEY_BLOB_LENGTH, length);

      // PAD SPACE: strip trailing 0x20 so 'a' == 'a ' under default collations.
      if (!(field->charset()->state & MY_CS_NOPAD)) {
        while (length > 0 && key_ptr[null_off + length - 1] == 0x20)
          --length;
      }

      std::memset(key_ptr + null_off + length, 0, kp.store_length - null_off - length);
    }

    key_ptr += kp.store_length;
  }
}

// Normalize VARCHAR key parts in PK encoding for PAD SPACE collations.
// PK format keeps the 2-byte LE length prefix (unlike fix_varchar_key_encoding
// which strips it). This function trims trailing 0x20 bytes and updates the
// length prefix so 'a' and 'a ' produce identical PK keys.
void normalize_padspace_pk(uint8_t *key_data, const KEY *key_info) {
  uint8_t *p = key_data;
  for (uint i = 0; i < key_info->user_defined_key_parts; ++i) {
    const KEY_PART_INFO &kp = key_info->key_part[i];
    Field *field = kp.field;
    if (field->type() == MYSQL_TYPE_VARCHAR &&
        !(field->charset()->state & MY_CS_NOPAD)) {
      uint null_off = (kp.null_bit) ? 1 : 0;
      uint16_t length = static_cast<uint16_t>(p[null_off]) |
                        (static_cast<uint16_t>(p[null_off + 1]) << 8);
      if (length > kp.length) length = kp.length;
      while (length > 0 && p[null_off + 2 + length - 1] == 0x20)
        --length;
      // Update the 2-byte LE length prefix.
      p[null_off] = static_cast<uint8_t>(length & 0xFF);
      p[null_off + 1] = static_cast<uint8_t>((length >> 8) & 0xFF);
      // Zero the trimmed trailing bytes.
      std::memset(p + null_off + 2 + length, 0,
                  kp.length - length);
    }
    p += kp.store_length;
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
  make_mem_comparable(key.data() + 7, &key_info, sec_key_len);
#endif

  // Append PK bytes for uniqueness, or synthetic rowid for PK-less tables.
  if (table->s->primary_key == MAX_KEY) {
    write_be64(key.data() + 7 + sec_key_len, synthetic_rowid);
  } else if (suffix_len > 0) {
    key_copy(key.data() + 7 + sec_key_len,
             const_cast<uchar *>(buf),
             &table->key_info[table->s->primary_key],
             suffix_len);
#ifndef BYTECASKDB_TESTS
    normalize_padspace_pk(key.data() + 7 + sec_key_len,
                          &table->key_info[table->s->primary_key]);
    make_mem_comparable(key.data() + 7 + sec_key_len,
                        &table->key_info[table->s->primary_key], suffix_len);
#endif
  }

  return key;
}

const uint8_t *extract_pk_from_sec_key(const uint8_t *sec_key,
                                        std::size_t sec_key_len,
                                        uint sec_key_packed_len,
                                        std::size_t *pk_len_out) {
  if (sec_key_len <= 7 + sec_key_packed_len) {
    *pk_len_out = 0;
    return nullptr;
  }

  *pk_len_out = sec_key_len - 7 - sec_key_packed_len;
  return sec_key + 7 + sec_key_packed_len;
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
  make_mem_comparable(key.data() + 7, &key_info, sec_key_len);
#endif

  return key;
}

#ifndef BYTECASKDB_TESTS

namespace {

bool is_signed_integer_type(enum_field_types t) {
  switch (t) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      return true;
    default:
      return false;
  }
}

bool is_unsigned_le_fixed_type(enum_field_types t) {
  switch (t) {
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_ENUM:
      return true;
    default:
      return false;
  }
}

bool is_float_type(enum_field_types t) {
  return t == MYSQL_TYPE_FLOAT || t == MYSQL_TYPE_DOUBLE;
}

// DATETIME2, TIMESTAMP2, TIME2 store data big-endian in key_copy() output.
// field->type() returns the old enum (MYSQL_TYPE_DATETIME etc.) so we must
// check real_type() to avoid reversing already-correct BE bytes.
bool is_new_temporal_be(Field *field) {
  auto rt = field->real_type();
  return rt == MYSQL_TYPE_DATETIME2 ||
         rt == MYSQL_TYPE_TIMESTAMP2 ||
         rt == MYSQL_TYPE_TIME2;
}

bool needs_byte_reversal(Field *field) {
  if (is_new_temporal_be(field)) return false;
  auto t = field->real_type();
  return is_signed_integer_type(t) || is_unsigned_le_fixed_type(t) || is_float_type(t);
}

} // namespace

void make_mem_comparable(uint8_t *key_data, const KEY *key_info, uint key_len) {
  uint8_t *p = key_data;
  uint8_t *end = key_data + key_len;

  for (uint i = 0; i < key_info->user_defined_key_parts && p < end; ++i) {
    const KEY_PART_INFO &kp = key_info->key_part[i];
    Field *field = kp.field;

    bool is_varlen = (field->type() == MYSQL_TYPE_VARCHAR ||
                      (kp.key_part_flag & HA_BLOB_PART));
    if (is_varlen) {
      p += kp.store_length;
      continue;
    }

    uint null_bytes = kp.store_length - kp.length;
    bool is_not_null = (null_bytes == 0 || p[0] == 0);

    if (is_not_null && needs_byte_reversal(field)) {
      uint8_t *data = p + null_bytes;
      uint len = kp.length;
      std::reverse(data, data + len);
      if (is_signed_integer_type(field->type()) && !field->is_unsigned()) {
        data[0] ^= 0x80;
      } else if (is_float_type(field->real_type())) {
        // IEEE 754: if sign bit set (negative), flip all bits;
        // if sign bit clear (positive/zero), flip only the sign bit.
        if (data[0] & 0x80) {
          for (uint j = 0; j < len; ++j) data[j] ^= 0xFF;
        } else {
          data[0] ^= 0x80;
        }
      }
    }

    if (null_bytes > 0) {
      p[0] ^= 0x01;
    }

    p += kp.store_length;
  }
}

void undo_mem_comparable(uint8_t *key_data, const KEY *key_info, uint key_len) {
  uint8_t *p = key_data;
  uint8_t *end = key_data + key_len;

  for (uint i = 0; i < key_info->user_defined_key_parts && p < end; ++i) {
    const KEY_PART_INFO &kp = key_info->key_part[i];
    Field *field = kp.field;

    bool is_varlen = (field->type() == MYSQL_TYPE_VARCHAR ||
                      (kp.key_part_flag & HA_BLOB_PART));
    if (is_varlen) {
      p += kp.store_length;
      continue;
    }

    uint null_bytes = kp.store_length - kp.length;

    if (null_bytes > 0) {
      p[0] ^= 0x01;
    }

    bool is_not_null = (null_bytes == 0 || p[0] == 0);

    if (is_not_null && needs_byte_reversal(field)) {
      uint8_t *data = p + null_bytes;
      uint len = kp.length;
      if (is_signed_integer_type(field->type()) && !field->is_unsigned()) {
        data[0] ^= 0x80;
      } else if (is_float_type(field->real_type())) {
        // Undo IEEE 754: if sign bit set (was positive), flip only sign bit;
        // if sign bit clear (was negative), flip all bits.
        if (data[0] & 0x80) {
          data[0] ^= 0x80;
        } else {
          for (uint j = 0; j < len; ++j) data[j] ^= 0xFF;
        }
      }
      std::reverse(data, data + len);
    }

    p += kp.store_length;
  }
}

#endif

} // namespace bytecaskdb
