// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// key_encoding.cc — Primary key encoding implementation.

#include "key_encoding.h"
#include "catalog.h"   // kNsRow
#include "table.h"     // full TABLE / TABLE_SHARE definitions
#include "key.h"       // key_copy

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

// The value length in a VARCHAR or BLOB slot as key_copy() writes it: a
// 2-byte little-endian prefix, clamped to the part's length.
uint16_t varlen_slot_length(const uint8_t *slot, const KeyPartPlan &kp) {
  const auto length = static_cast<uint16_t>(slot[0] | (slot[1] << 8));
  return static_cast<uint16_t>(std::min<uint32_t>(length, kp.length));
}

// Bytes [0, length) of a PAD SPACE value with its trailing spaces dropped,
// so 'a' and 'a ' make the same key. Format 1 drops whole spaces of the
// charset; format 0 dropped 0x20 bytes, and its keys were written that way.
uint16_t trim_trailing_spaces(const uint8_t *data, uint16_t length,
                              const KeyPlan &plan, const KeyPartPlan &kp) {
  if (!plan.space_pad) {
    while (length > 0 && data[length - 1] == 0x20) --length;
    return length;
  }
  const uint w = kp.space_len;
  if (w == 0) return length;
  while (length >= w && std::memcmp(data + length - w, kp.space, w) == 0) {
    length = static_cast<uint16_t>(length - w);
  }
  return length;
}

// Pads a value of `used` bytes to `width`. A PAD SPACE collation compares a
// shorter value as if padded with spaces, so 'a\0b' sorts before 'a';
// padding with whole spaces of the charset makes the bytes sort that way,
// and since trailing spaces are already trimmed, equal padded bytes mean
// equal values. Any other collation, and a format 0 table, pads with zero
// bytes. A tail narrower than one space stays zero, alike in every value.
void pad_value(uint8_t *data, uint used, uint width, const KeyPlan &plan,
               const KeyPartPlan &kp) {
  std::memset(data + used, 0, width - used);
  if (!plan.space_pad || !kp.pad_space || kp.space_len == 0) return;
  for (uint i = used; i + kp.space_len <= width; i += kp.space_len) {
    std::memcpy(data + i, kp.space, kp.space_len);
  }
}

// Writes a CHAR, VARCHAR or TEXT part, read from key_copy()'s slot `src`,
// as its collation's sort key: the null byte (0 for NULL, as every part
// stores it), weight_width weight bytes and, under NO PAD, the weights'
// length. Under PAD SPACE the weights are padded with the space's weight,
// so values equal up to trailing spaces make equal bytes; under NO PAD they
// are padded with zeros, and the length keeps 'a' and 'a' followed by a
// character weighing zero bytes apart only if their weights differ.
void write_weights(uint8_t *dst, const uint8_t *src, const KeyPartPlan &kp) {
  const uint null_off = kp.nullable ? 1 : 0;
  const bool is_null = kp.nullable && src[0] != 0;
  if (kp.nullable) dst[0] = is_null ? 0 : 1;
  uint8_t *w = dst + null_off;
  const uint tail = kp.weight_length ? 2 : 0;
  if (is_null) {
    std::memset(w, 0, kp.weight_width + tail);
  } else {
    const uint8_t *data = src + null_off;
    std::size_t len = kp.length;
    if (kp.varlen) {
      len = varlen_slot_length(data, kp);
      data += HA_KEY_BLOB_LENGTH;
    }
    const uint flags = kp.weight_length
                           ? 0
                           : MY_STRXFRM_PAD_WITH_SPACE | MY_STRXFRM_PAD_TO_MAXLEN;
    const std::size_t n = std::min<std::size_t>(
        kp.weights_cs->strnxfrm(w, kp.weight_width, kp.nweights, data, len,
                                flags),
        kp.weight_width);
    std::memset(w + n, 0, kp.weight_width - n);
    if (kp.weight_length) {
      write_be16(w + kp.weight_width, static_cast<uint16_t>(n));
    }
  }
  if (kp.desc) {
    for (uint i = 0; i < kp.stored_length; ++i) {
      dst[i] = static_cast<uint8_t>(~dst[i]);
    }
  }
}

// key_copy() of `key_info` from the row at `buf`, stored in the plan's form
// at out[offset, offset + plan.stored_length). Without weights every part
// keeps its width and is encoded in place. With weights the widths differ,
// so key_copy() writes behind the stored slot, growing `out` as far as it
// needs, and encode_key_parts reads it from there: no bound on the key's
// size is assumed. The caller trims `out` to the key's length after.
void copy_and_encode(std::vector<uint8_t> &out, std::size_t offset,
                     const uchar *buf, const KEY &key_info,
                     const KeyPlan &plan, bool pk) {
  const uint len = key_info.key_length;
  auto *row = const_cast<uchar *>(buf);
  auto *key = const_cast<KEY *>(&key_info);
  if (!plan.weights) {
    key_copy(out.data() + offset, row, key, len);
    encode_key_parts(out.data() + offset, out.data() + offset, plan, len, pk);
    return;
  }
  const std::size_t src = offset + plan.stored_length;
  if (out.size() < src + len) out.resize(src + len);
  key_copy(out.data() + src, row, key, len);
  encode_key_parts(out.data() + offset, out.data() + src, plan, len, pk);
}

} // namespace

uint64_t read_be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<uint64_t>(p[i]);
  }
  return v;
}

void encode_pk_into(std::vector<uint8_t> &out, TABLE *table,
                    [[maybe_unused]] const TableCodec &codec, const uchar *buf,
                    uint32_t table_id, uint64_t synthetic_rowid) {
  const uint pk_idx = table->s->primary_key;
  const uint suffix_len = pk_suffix_length(table, codec);

  out.clear();
  out.resize(5 + suffix_len);

  // Write namespace byte + table_id prefix.
  out[0] = kNsRow;
  write_be32(out.data() + 1, table_id);

  if (pk_idx == MAX_KEY) {
    write_be64(out.data() + 5, synthetic_rowid);
  } else if (suffix_len > 0) {
    copy_and_encode(out, 5, buf, table->key_info[pk_idx], codec.keys[pk_idx],
                    /*pk=*/true);
    out.resize(5 + suffix_len);
  }
}

std::vector<uint8_t> encode_pk(TABLE *table, const TableCodec &codec,
                                const uchar *buf, uint32_t table_id,
                                uint64_t synthetic_rowid) {
  std::vector<uint8_t> key;
  encode_pk_into(key, table, codec, buf, table_id, synthetic_rowid);
  return key;
}

uint pk_suffix_length(TABLE *table, const TableCodec &codec) {
  if (table->s->primary_key == MAX_KEY) {
    return 8;  // synthetic rowid
  }
  return codec.keys[table->s->primary_key].stored_length;
}

std::size_t max_stored_key_bytes(TABLE *table, const TableCodec &codec) {
  const std::size_t pk = pk_suffix_length(table, codec);
  // A search key padded high runs one byte past a whole primary key.
  std::size_t longest = 5 + pk + 1;
  for (uint k = 0; k < table->s->keys; ++k) {
    if (k == table->s->primary_key) continue;
    longest = std::max(longest, 7 + std::size_t{codec.keys[k].stored_length} + pk);
  }
  return longest;
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

void encode_sec_key_into(std::vector<uint8_t> &out, TABLE *table,
                          [[maybe_unused]] const TableCodec &codec, const uchar *buf,
                          uint32_t table_id, uint16_t index_id,
                          uint active_index,
                          uint64_t synthetic_rowid) {
  const uint sec_key_len = codec.keys[active_index].stored_length;
  const uint suffix_len = pk_suffix_length(table, codec);

  // Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key | pk-or-rowid]
  out.clear();
  out.resize(7 + sec_key_len + suffix_len);

  // Namespace + table_id + index_id prefix.
  out[0] = kNsIndex;
  write_be32(out.data() + 1, table_id);
  write_be16(out.data() + 5, index_id);

  copy_and_encode(out, 7, buf, table->key_info[active_index],
                  codec.keys[active_index], /*pk=*/false);

  // Append PK bytes for uniqueness, or synthetic rowid for PK-less tables.
  const uint pk_idx = table->s->primary_key;
  if (pk_idx == MAX_KEY) {
    write_be64(out.data() + 7 + sec_key_len, synthetic_rowid);
  } else if (suffix_len > 0) {
    copy_and_encode(out, 7 + sec_key_len, buf, table->key_info[pk_idx],
                    codec.keys[pk_idx], /*pk=*/true);
  }
  out.resize(7 + sec_key_len + suffix_len);
}

std::vector<uint8_t> encode_sec_key(TABLE *table, const TableCodec &codec,
                                     const uchar *buf,
                                     uint32_t table_id, uint16_t index_id,
                                     uint active_index,
                                     uint64_t synthetic_rowid) {
  std::vector<uint8_t> key;
  encode_sec_key_into(key, table, codec, buf, table_id, index_id, active_index,
                       synthetic_rowid);
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

void encode_unique_sec_key_into(std::vector<uint8_t> &out,
                                 TABLE *table, [[maybe_unused]] const TableCodec &codec,
                                 const uchar *buf,
                                 uint32_t table_id, uint16_t index_id,
                                 uint active_index) {
  const uint sec_key_len = codec.keys[active_index].stored_length;

  // Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key] (no PK for uniqueness)
  out.clear();
  out.resize(7 + sec_key_len);

  // Namespace + table_id + index_id prefix.
  out[0] = kNsIndex;
  write_be32(out.data() + 1, table_id);
  write_be16(out.data() + 5, index_id);

  copy_and_encode(out, 7, buf, table->key_info[active_index],
                  codec.keys[active_index], /*pk=*/false);
  out.resize(7 + sec_key_len);
}

std::vector<uint8_t> encode_unique_sec_key(TABLE *table,
                                            const TableCodec &codec,
                                            const uchar *buf,
                                            uint32_t table_id, uint16_t index_id,
                                            uint active_index) {
  std::vector<uint8_t> key;
  encode_unique_sec_key_into(key, table, codec, buf, table_id, index_id,
                             active_index);
  return key;
}

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
bool is_new_temporal_be(enum_field_types rt) {
  return rt == MYSQL_TYPE_DATETIME2 ||
         rt == MYSQL_TYPE_TIMESTAMP2 ||
         rt == MYSQL_TYPE_TIME2;
}

// A space in `cs`, written to `out`; its length, or 0 if the charset
// cannot encode one.
uint8_t encoded_space(const CHARSET_INFO *cs, uint8_t (&out)[4]) {
  if (cs->mbminlen == 1) {
    out[0] = 0x20;
    return 1;
  }
  if (cs->cset == nullptr) return 0;
  const int n = cs->cset->wc_mb(cs, 0x20, out, out + sizeof(out));
  return n > 0 ? static_cast<uint8_t>(n) : 0;
}

KeyPartPlan make_key_part_plan(TABLE *table, const KEY_PART_INFO &kp) {
  Field *field = kp.field;
  KeyPartPlan p;
  p.store_length = kp.store_length;
  p.stored_length = kp.store_length;
  p.length = kp.length;
  p.record_offset = static_cast<uint32_t>(field->ptr - table->record[0]);
  p.nullable = kp.null_bit != 0;
  p.varchar = field->type() == MYSQL_TYPE_VARCHAR;
  p.varlen = p.varchar || (kp.key_part_flag & HA_BLOB_PART);
  if (p.varlen) {
    const CHARSET_INFO *cs = field->charset();
    p.pad_space = cs != nullptr && !(cs->state & MY_CS_NOPAD);
    if (p.pad_space) {
      p.space_len = encoded_space(cs, p.space);
    }
  }
  const auto rt = field->real_type();
  p.be_temporal = is_new_temporal_be(rt);
  p.reverse = !p.be_temporal && (is_signed_integer_type(rt) ||
                                 is_unsigned_le_fixed_type(rt) ||
                                 is_float_type(rt));
  if (is_signed_integer_type(field->type()) && !field->is_unsigned()) {
    p.flip = KeyPartPlan::Flip::kSign;
  } else if (is_float_type(rt)) {
    p.flip = KeyPartPlan::Flip::kFloat;
  }
#ifdef HA_REVERSE_SORT  // DESC key parts exist from MariaDB 10.8
  p.desc = (kp.key_part_flag & HA_REVERSE_SORT) != 0;
#endif
  return p;
}

// A VARCHAR or BLOB slot as key_copy() writes it: a 2-byte little-endian
// length, then up to kp.length bytes. Rewritten in place, in the same width,
// as the bytes padded to kp.length (pad_value) and then the length,
// big-endian, so the part sorts by value: under NO PAD or a binary collation
// the padding is zero bytes and, of 'a' and 'a\0', the shorter sorts first,
// as the server orders them; under PAD SPACE equal padded bytes never differ
// in length.
void move_length_last(uint8_t *slot, const KeyPlan &plan,
                      const KeyPartPlan &kp) {
  const uint16_t length = varlen_slot_length(slot, kp);
  std::memmove(slot, slot + HA_KEY_BLOB_LENGTH, length);
  pad_value(slot, length, kp.length, plan, kp);
  slot[kp.length]     = static_cast<uint8_t>(length >> 8);
  slot[kp.length + 1] = static_cast<uint8_t>(length & 0xFF);
}

// Inverts a DESC part's stored bytes; its own inverse, so it both applies and
// undoes the descending order. A search key may end inside the part.
void invert_desc_part(uint8_t *p, const uint8_t *end, const KeyPartPlan &kp) {
  const uint8_t *part_end = std::min<const uint8_t *>(p + kp.store_length, end);
  for (; p < part_end; ++p) *p = static_cast<uint8_t>(~*p);
}

// A secondary key's VARCHAR or BLOB slot, in place: key_copy()'s 2-byte
// length prefix removed and the bytes left-justified and padded
// (pad_value), so the part sorts by value; the PK suffix tells apart values
// that differ only in trailing padding. Under PAD SPACE trailing spaces are
// trimmed first, so 'a' and 'a ' make the same key.
void strip_varlen_length(uint8_t *slot, const KeyPlan &plan,
                         const KeyPartPlan &kp) {
  const uint null_off = kp.nullable ? 1 : 0;
  uint16_t length = varlen_slot_length(slot + null_off, kp);
  std::memmove(slot + null_off, slot + null_off + HA_KEY_BLOB_LENGTH, length);
  if (kp.pad_space) {
    length = trim_trailing_spaces(slot + null_off, length, plan, kp);
  }
  pad_value(slot + null_off, length, kp.store_length - null_off, plan, kp);
}

// A primary key's VARCHAR slot under PAD SPACE, in place: trailing spaces
// trimmed and the length prefix updated, so 'a' and 'a ' make the same key.
// The PK keeps the prefix (move_length_last moves it behind the bytes).
void trim_pk_varchar(uint8_t *slot, const KeyPlan &plan,
                     const KeyPartPlan &kp) {
  const uint null_off = kp.nullable ? 1 : 0;
  uint8_t *data = slot + null_off + HA_KEY_BLOB_LENGTH;
  const uint16_t length = trim_trailing_spaces(
      data, varlen_slot_length(slot + null_off, kp), plan, kp);
  slot[null_off] = static_cast<uint8_t>(length & 0xFF);
  slot[null_off + 1] = static_cast<uint8_t>((length >> 8) & 0xFF);
  std::memset(data + length, 0, kp.length - length);
}

// One part of key_copy()'s bytes, other than a weights part, made
// mem-comparable in place: integers big-endian with the sign flipped,
// floats IEEE-flipped, a VARCHAR or BLOB part's padding and (in the primary
// key) its length moved behind its bytes, the null indicator flipped so
// NULL sorts first, and a DESC part inverted. `end` bounds a search key,
// which may stop inside the part.
void mem_comparable_part(uint8_t *p, const uint8_t *end, const KeyPlan &plan,
                         const KeyPartPlan &kp, bool pk) {
  if (kp.varlen) {
    if (pk) {
      if (kp.varchar && kp.pad_space) trim_pk_varchar(p, plan, kp);
    } else {
      strip_varlen_length(p, plan, kp);
    }
    if (plan.varlen_length_last) {
      move_length_last(p + (kp.nullable ? 1 : 0), plan, kp);
    }
    if (kp.nullable) p[0] ^= 0x01;
    if (kp.desc) invert_desc_part(p, end, kp);
    return;
  }

  const uint null_bytes = kp.store_length - kp.length;
  const bool is_not_null = (null_bytes == 0 || p[0] == 0);
  if (is_not_null && kp.reverse) {
    uint8_t *data = p + null_bytes;
    const uint len = kp.length;
    std::reverse(data, data + len);
    if (kp.flip == KeyPartPlan::Flip::kSign) {
      data[0] ^= 0x80;
    } else if (kp.flip == KeyPartPlan::Flip::kFloat) {
      // IEEE 754: if sign bit set (negative), flip all bits;
      // if sign bit clear (positive/zero), flip only the sign bit.
      if (data[0] & 0x80) {
        for (uint j = 0; j < len; ++j) data[j] ^= 0xFF;
      } else {
        data[0] ^= 0x80;
      }
    }
  }
  if (null_bytes > 0) p[0] ^= 0x01;
  if (kp.desc) invert_desc_part(p, end, kp);
}

// Stores `p` as collation weights, if it is a CHAR, VARCHAR or TEXT part
// under any charset but binary. Its width is what filesort reserves for the
// same bytes (strnxfrmlen), so every value's weights fit.
void use_weights(KeyPartPlan &p, const KEY_PART_INFO &kp) {
  const Field *field = kp.field;
  const auto rt = field->real_type();
  const bool string_part = rt == MYSQL_TYPE_STRING ||
                           rt == MYSQL_TYPE_VARCHAR ||
                           (kp.key_part_flag & HA_BLOB_PART);
  const CHARSET_INFO *cs = field->charset();
  if (!string_part || cs == nullptr || cs == &my_charset_bin) return;
  p.weights_cs = cs;
  p.weight_width = static_cast<uint32_t>(cs->strnxfrmlen(p.length));
  p.nweights = (p.length / std::max(cs->mbmaxlen, 1u)) *
               std::max(cs->strxfrm_multiply, 1u);
  p.weight_length = (cs->state & MY_CS_NOPAD) != 0;
  p.stored_length = (p.nullable ? 1 : 0) + p.weight_width +
                    (p.weight_length ? 2 : 0);
}

} // namespace

TableCodec make_table_codec(TABLE *table, uint8_t key_format) {
  TableCodec codec;
  codec.row = make_row_plan(table);
  codec.keys.resize(table->s->keys);
  for (uint k = 0; k < table->s->keys; ++k) {
    const KEY &key = table->key_info[k];
    auto &parts = codec.keys[k].parts;
    parts.reserve(key.user_defined_key_parts);
    for (uint i = 0; i < key.user_defined_key_parts; ++i) {
      parts.push_back(make_key_part_plan(table, key.key_part[i]));
      if (key_format >= kKeyFormatCollationWeights) {
        use_weights(parts.back(), key.key_part[i]);
      }
    }
    for (const auto &p : parts) {
      codec.keys[k].stored_length += p.stored_length;
      codec.keys[k].weights |= p.weights_cs != nullptr;
    }
  }
  // Secondary keys drop the length altogether (strip_varlen_length), their
  // PK suffix telling such values apart; only the primary key carries it.
  if (key_format >= kKeyFormatValueOrderedPk) {
    for (auto &plan : codec.keys) plan.space_pad = true;
    const uint pk = table->s->primary_key;
    if (pk != MAX_KEY) codec.keys[pk].varlen_length_last = true;
  }
  return codec;
}

uint encode_key_parts(uint8_t *dst, uint8_t *src, const KeyPlan &plan,
                      uint src_len, bool pk) {
  uint in = 0;
  uint out = 0;
  for (const auto &kp : plan.parts) {
    if (in >= src_len) break;
    uint8_t *part = src + in;
    if (kp.weights_cs != nullptr) {
      write_weights(dst + out, part, kp);
    } else {
      mem_comparable_part(part, src + src_len, plan, kp, pk);
      if (dst + out != part) std::memcpy(dst + out, part, kp.store_length);
    }
    in += kp.store_length;
    out += kp.stored_length;
  }
  return out;
}

bool undo_fixed_width_part(uint8_t *slot, const KeyPartPlan &kp) {
  if (kp.varlen || kp.weights_cs != nullptr) return false;
  const uint8_t *end = slot + kp.store_length;
  if (kp.desc) invert_desc_part(slot, end, kp);
  const uint null_bytes = kp.store_length - kp.length;
  if (null_bytes > 0) slot[0] ^= 0x01;
  const bool is_not_null = (null_bytes == 0 || slot[0] == 0);
  if (is_not_null && kp.reverse) {
    uint8_t *data = slot + null_bytes;
    const uint len = kp.length;
    if (kp.flip == KeyPartPlan::Flip::kSign) {
      data[0] ^= 0x80;
    } else if (kp.flip == KeyPartPlan::Flip::kFloat) {
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
  return true;
}

bool decode_sec_key_into_record(const KeyPlan &plan,
                                 const uint8_t *sec_key, std::size_t sec_key_len,
                                 uchar *record) {
  // Skip the 7-byte [ns | tid | iid] prefix.
  if (sec_key_len < 7) return false;
  const uint8_t *p   = sec_key + 7;
  const uint8_t *end = sec_key + sec_key_len;

  for (const auto &kp : plan.parts) {
    // Bail on anything that's not a non-null fixed-width type we know how
    // to reverse cleanly. The caller falls back to the full row fetch.
    if (kp.nullable || kp.varlen) return false;
    if (!kp.be_temporal && !kp.reverse) return false;

    if (kp.store_length != kp.length) return false;  // unexpected null padding
    if (p + kp.length > end) return false;

    // Stage the bytes locally so we don't mutate the caller's key buffer.
    uint8_t tmp[16];
    if (kp.length > sizeof(tmp)) return false;
    std::memcpy(tmp, p, kp.length);
    if (kp.desc) {
      for (uint j = 0; j < kp.length; ++j) tmp[j] = static_cast<uint8_t>(~tmp[j]);
    }

    if (kp.reverse) {
      if (kp.flip == KeyPartPlan::Flip::kSign) {
        tmp[0] ^= 0x80;
      } else if (kp.flip == KeyPartPlan::Flip::kFloat) {
        if (tmp[0] & 0x80) {
          tmp[0] ^= 0x80;
        } else {
          for (uint j = 0; j < kp.length; ++j) tmp[j] ^= 0xFF;
        }
      }
      std::reverse(tmp, tmp + kp.length);
    }

    std::memcpy(record + kp.record_offset, tmp, kp.length);

    p += kp.store_length;
  }
  return true;
}

} // namespace bytecaskdb
