// key_encoding.cc — Primary key encoding implementation.

#include "key_encoding.h"
#include "table.h"   // full TABLE / TABLE_SHARE definitions
#include "key.h"    // key_copy, key_restore

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

  std::vector<uint8_t> key(4 + pk_len);

  // Write table_id prefix.
  write_be32(key.data(), table_id);

  if (pk_idx != MAX_KEY && pk_len > 0) {
    // key_copy() extracts the PK columns from the row buffer into a
    // fixed-size key buffer using MariaDB's internal ordering-preserving
    // key format.  The resulting bytes are byte-comparable for most types,
    // which is exactly what we need for prefix-bounded range scans.
    key_copy(key.data() + 4,
             const_cast<uchar *>(buf),
             &table->key_info[pk_idx],
             pk_len);
  }

  return key;
}

void decode_pk(TABLE *table, const uint8_t *key, std::size_t key_len,
               uchar *buf) {
  const uint pk_idx = table->s->primary_key;
  if (pk_idx == MAX_KEY || key_len <= 4) {
    return;
  }
  // Strip the 4-byte table_id prefix before calling key_restore().
  key_restore(buf,
              reinterpret_cast<const uchar *>(key + 4),
              &table->key_info[pk_idx],
              static_cast<uint>(key_len - 4));
}

std::vector<uint8_t> table_id_prefix(uint32_t table_id) {
  std::vector<uint8_t> prefix(4);
  write_be32(prefix.data(), table_id);
  return prefix;
}

bool key_belongs_to_table(const uint8_t *key, std::size_t key_len,
                           uint32_t table_id) {
  if (key_len < 4) {
    return false;
  }
  uint8_t expected[4];
  write_be32(expected, table_id);
  return std::memcmp(key, expected, 4) == 0;
}

} // namespace bytecaskdb
