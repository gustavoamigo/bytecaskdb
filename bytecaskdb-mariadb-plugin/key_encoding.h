// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// key_encoding.h — Primary key encoding for ByteCaskDB / MariaDB plugin.
//
// All keys stored in the global ByteCaskDB instance are prefixed with a
// namespace byte (0x02 for row data) followed by a 4-byte big-endian table_id,
// so that different tables and namespaces never collide.
//
// Key format:
//   [ns: 1 byte (0x02)][table_id: 4 bytes, big-endian][pk_columns: variable]
//
// Secondary index key format:
//   [ns: 1 byte (0x03)][table_id: 4 bytes, big-endian][index_id: 2 bytes, big-endian]
//   [index_columns: variable][pk_columns: variable]

#pragma once

#include "my_global.h"
#include "handler.h"
#include "field.h"
#include "row_encoding.h"

#include <cstdint>
#include <vector>

namespace bytecaskdb {

// How one key part moves between key_copy()'s bytes and the stored,
// mem-comparable form, read from its Field once (make_table_codec).
struct KeyPartPlan {
  enum class Flip : uint8_t { kNone, kSign, kFloat };
  uint32_t store_length{0};
  uint32_t length{0};
  uint32_t record_offset{0};  // the column's offset in the record
  bool nullable{false};       // a null byte precedes the value
  bool varchar{false};        // VARCHAR column
  bool varlen{false};         // VARCHAR or BLOB part: no byte transform
  bool pad_space{false};      // varlen under a PAD SPACE collation
  // A space in the column's charset: 0x20, 00 20 in ucs2/utf16, 20 00 in
  // utf16le, 00 00 00 20 in utf32. Format 1 trims and pads by whole spaces;
  // space_len 0 (charset could not encode one) leaves the value unpadded.
  uint8_t space[4]{0x20, 0, 0, 0};
  uint8_t space_len{1};
  bool be_temporal{false};    // DATETIME2/TIMESTAMP2/TIME2: big-endian already
  bool reverse{false};        // little-endian in the record: stored reversed
  Flip flip{Flip::kNone};     // applied with reverse
  bool desc{false};           // DESC part: its stored bytes are inverted
  // kKeyFormatCollationWeights: a CHAR, VARCHAR or TEXT part under any
  // collation but binary is stored as its collation's sort key (strnxfrm),
  // so it compares as the collation does: case- and accent-insensitive
  // collations make equal keys of 'a' and 'A'. Its slot is the null byte,
  // weight_width weight bytes and, under NO PAD, the weights' own length,
  // big-endian. The value cannot be restored from it; rows carry every
  // column. nullptr: the part is stored in key_copy()'s width.
  const CHARSET_INFO *weights_cs{nullptr};
  uint32_t weight_width{0};
  uint32_t nweights{0};       // strnxfrm's character budget for the part
  bool weight_length{false};  // NO PAD: weight length follows the weights
  // Width of the part in a stored key: store_length, or the weight slot.
  uint32_t stored_length{0};
};

struct KeyPlan {
  std::vector<KeyPartPlan> parts;
  // Primary key of a kKeyFormatValueOrderedPk table: a VARCHAR or BLOB part
  // keeps its slot, but its length moves behind the padded bytes, so the
  // part sorts by value and 'a' and 'a\0' stay apart under NO PAD.
  bool varlen_length_last{false};
  // kKeyFormatValueOrderedPk, every key: a VARCHAR or BLOB part under a PAD
  // SPACE collation has its trailing spaces trimmed, and is padded, by whole
  // characters of its charset, so it sorts as the collation compares it
  // ('a\0b' before 'a'). Format 0 trims 0x20 bytes and pads with zeros.
  bool space_pad{false};
  // Width of a stored key's columns: key_length, unless a part is stored as
  // collation weights. Offsets into stored keys (the PK suffix of a
  // secondary entry, ref_length) are counted in it.
  uint32_t stored_length{0};
  bool weights{false};        // some part is stored as collation weights
};

// Everything the row and key codecs ask of a table's definition, read once
// when the handler opens the table and rebuilt on every open, so it never
// outlives the definition it was read from. Asking the Fields per row costs a
// virtual call per column per question.
struct TableCodec {
  RowPlan row;
  std::vector<KeyPlan> keys;  // by key number, as table->key_info
};

// `key_format` is the table's kKeyFormat* from its catalog entry.
TableCodec make_table_codec(TABLE *table, uint8_t key_format);

// Encodes the primary-key columns of the row at `buf` (table->record[0] or
// table->record[1]) into a ByteCaskDB key: namespace byte (0x02) + 4-byte
// big-endian table_id + MariaDB's internal key representation (key_copy()).
//
// For tables without a PRIMARY KEY (table->s->primary_key == MAX_KEY), the
// caller supplies an 8-byte synthetic rowid which is appended big-endian
// after the table_id prefix. Result is exactly 13 bytes for PK-less tables.

// Hot-path overload: writes encoded bytes into `out`, reusing capacity.
void encode_pk_into(std::vector<uint8_t> &out, TABLE *table,
                    const TableCodec &codec, const uchar *buf,
                    uint32_t table_id, uint64_t synthetic_rowid = 0);

// Returning overload: thin wrapper for non-hot callers/tests.
std::vector<uint8_t> encode_pk(TABLE *table, const TableCodec &codec,
                                const uchar *buf, uint32_t table_id,
                                uint64_t synthetic_rowid = 0);

// Returns the 5-byte prefix [0x02 | table_id(BE,4)] for iterating a table's
// row keys.
std::vector<uint8_t> table_id_prefix(uint32_t table_id);

// Returns true if `key` (of `key_len` bytes) belongs to the given table's
// row namespace (i.e. starts with [0x02 | table_id(BE,4)]).
bool key_belongs_to_table(const uint8_t *key, std::size_t key_len,
                           uint32_t table_id);

// Returns the exclusive upper bound [0x02 | (table_id+1)(BE,4)] for bounded
// iteration over a table's keys.
std::vector<uint8_t> table_id_upper_bound(uint32_t table_id);

// Encodes a secondary index key: namespace (0x03) + table_id + index_id +
// packed secondary key columns + embedded primary key (or synthetic rowid).
// Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key_packed | pk_packed]
//
// For PK-less tables, the caller supplies an 8-byte synthetic rowid which is
// appended big-endian in place of the packed PK suffix.
void encode_sec_key_into(std::vector<uint8_t> &out, TABLE *table,
                          const TableCodec &codec, const uchar *buf,
                          uint32_t table_id, uint16_t index_id,
                          uint active_index,
                          uint64_t synthetic_rowid = 0);

std::vector<uint8_t> encode_sec_key(TABLE *table, const TableCodec &codec,
                                     const uchar *buf,
                                     uint32_t table_id, uint16_t index_id,
                                     uint active_index,
                                     uint64_t synthetic_rowid = 0);

// Returns the byte length of the trailing PK / rowid suffix used by
// encode_pk and encode_sec_key: 8 for PK-less tables, the primary key's
// stored_length otherwise.
uint pk_suffix_length(TABLE *table, const TableCodec &codec);

// Longest key the plugin stores: the engine's hard ceiling (u16 length).
inline constexpr uint32_t kMaxStoredKeyBytes = 65535;

// Longest key the table's definition can produce, a search key past a whole
// primary key included. Collation weights can make it larger than the
// server's own key limit, which counts key_copy() bytes; create() refuses a
// table whose keys would not fit in kMaxStoredKeyBytes.
std::size_t max_stored_key_bytes(TABLE *table, const TableCodec &codec);

// Reads a 8-byte big-endian unsigned integer from `p`.
uint64_t read_be64(const uint8_t *p);

// Extracts the primary key portion from a secondary index key.
// sec_key_packed_len is the length of the secondary key portion only.
// Returns a pointer to the PK portion within sec_key and writes the length
// to *pk_len_out. Returns nullptr if the key is too short.
const uint8_t *extract_pk_from_sec_key(const uint8_t *sec_key,
                                        std::size_t sec_key_len,
                                        uint sec_key_packed_len,
                                        std::size_t *pk_len_out);

// Returns the 7-byte prefix [0x03 | table_id(BE,4) | index_id(BE,2)] for
// iterating a specific secondary index.
std::vector<uint8_t> index_id_prefix(uint32_t table_id, uint16_t index_id);

// Returns the exclusive upper bound for secondary index iteration.
std::vector<uint8_t> index_id_upper_bound(uint32_t table_id, uint16_t index_id);

// Returns true if `key` belongs to the specified secondary index.
bool key_belongs_to_index(const uint8_t *key, std::size_t len,
                           uint32_t table_id, uint16_t index_id);

// Encodes a unique secondary index key for duplicate checking.
// Format: [0x03 | table_id(BE,4) | index_id(BE,2) | sec_key] (no PK suffix)
void encode_unique_sec_key_into(std::vector<uint8_t> &out,
                                 TABLE *table, const TableCodec &codec,
                                 const uchar *buf,
                                 uint32_t table_id, uint16_t index_id,
                                 uint active_index);

std::vector<uint8_t> encode_unique_sec_key(TABLE *table,
                                            const TableCodec &codec,
                                            const uchar *buf,
                                            uint32_t table_id, uint16_t index_id,
                                            uint active_index);

// Encodes key_copy() bytes `src` — the first `src_len` bytes of a key, whole
// parts — into their stored form at `dst`, which must hold
// plan.stored_length bytes, in one pass over the parts: the transform
// encode_pk_into (`pk`) or encode_sec_key_into applies. A weights part is
// written straight from its key_copy() slot (write_weights); every other
// part is made mem-comparable in place in `src` — integers big-endian with
// the sign flipped, floats IEEE-flipped, a VARCHAR or BLOB part padded per
// its collation and, in the primary key, its length moved behind its bytes,
// the null indicator flipped and a DESC part inverted, so stored keys sort
// in the index's order as the server reads it — then copied. `dst` may be
// `src` only for a plan without weights, whose parts keep their widths.
// Returns the stored bytes those parts take, a search key's prefix length.
uint encode_key_parts(uint8_t *dst, uint8_t *src, const KeyPlan &plan,
                      uint src_len, bool pk);

// Restores one fixed-width part's stored bytes at `slot` (store_length of
// them, null indicator included) to key_copy()'s native layout, in place.
// Read by the AUTO_INCREMENT seed, which takes the counter's integer column
// back from the largest key. False, and nothing changed, for a VARCHAR,
// BLOB or weights part: those are not restorable.
[[nodiscard]] bool undo_fixed_width_part(uint8_t *slot, const KeyPartPlan &kp);

// Decodes the key parts of `sec_key` directly into `record` for the columns
// covered by `plan`. Used to short-circuit the secondary→PK row fetch
// when the optimizer signals HA_EXTRA_KEYREAD.
//
// Returns true on success. Returns false if any key part is of a type the
// decoder does not (yet) handle (VARCHAR, BLOB, CHAR, nullable parts) — the
// caller must fall back to the full row fetch in that case.
//
// Initial scope: non-null fixed-width integers (signed + unsigned), floats,
// and temporal types whose mem-comparable transform is fully reversible.
bool decode_sec_key_into_record(const KeyPlan &plan,
                                 const uint8_t *sec_key, std::size_t sec_key_len,
                                 uchar *record);

} // namespace bytecaskdb
