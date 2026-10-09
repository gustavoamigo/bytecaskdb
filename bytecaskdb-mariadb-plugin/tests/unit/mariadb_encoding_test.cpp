// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_encoding_test.cpp — Unit tests for the MariaDB plugin's pure
// encoding and catalog layer.  No MariaDB server dependency.

// The stubs header must be included BEFORE the plugin headers so that
// key_encoding.h and row_encoding.h resolve TABLE/uchar/etc. from
// our minimal stubs rather than MariaDB's server headers.
#include "mariadb_stubs.h"

#include "catalog.h"
#include "key_encoding.h"
#include "row_encoding.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace bytecaskdb;

// =========================================================================
// Catalog — name normalization
// =========================================================================

TEST_CASE("normalize_table_name", "[catalog]") {
  SECTION("strips leading ./") {
    auto r = normalize_table_name("./test/t1");
    REQUIRE(r == std::string("test\0t1\0", 8));
  }

  SECTION("no prefix") {
    auto r = normalize_table_name("test/t1");
    REQUIRE(r == std::string("test\0t1\0", 8));
  }

  SECTION("strips leading /") {
    auto r = normalize_table_name("/test/t1");
    REQUIRE(r == std::string("test\0t1\0", 8));
  }

  SECTION("longer names") {
    auto r = normalize_table_name("mydb/my_table");
    REQUIRE(r == std::string("mydb\0my_table\0", 14));
  }

  SECTION("single component") {
    auto r = normalize_table_name("t1");
    REQUIRE(r == std::string("t1\0", 3));
  }
}

// =========================================================================
// Catalog — counter key
// =========================================================================

TEST_CASE("counter_key", "[catalog]") {
  auto k = counter_key(kCounterTableId);
  REQUIRE(k.size() == 3);
  REQUIRE(k[0] == kNsCatalog);
  REQUIRE(k[1] == kSubCounter);
  REQUIRE(k[2] == kCounterTableId);
}

// =========================================================================
// Catalog — table_meta_key
// =========================================================================

TEST_CASE("table_meta_key", "[catalog]") {
  auto k = table_meta_key("./test/t1");
  REQUIRE(k.size() >= 2);
  REQUIRE(k[0] == kNsCatalog);
  REQUIRE(k[1] == kSubTable);

  // The suffix should be the normalized name.
  auto normalized = normalize_table_name("./test/t1");
  std::vector<uint8_t> expected_suffix(
      reinterpret_cast<const uint8_t *>(normalized.data()),
      reinterpret_cast<const uint8_t *>(normalized.data() + normalized.size()));
  std::vector<uint8_t> actual_suffix(k.begin() + 2, k.end());
  REQUIRE(actual_suffix == expected_suffix);
}

// =========================================================================
// Catalog — table_meta_scan_bounds
// =========================================================================

TEST_CASE("table_meta_scan_bounds", "[catalog]") {
  auto [lower, upper] = table_meta_scan_bounds();
  REQUIRE(lower == std::vector<uint8_t>{kNsCatalog, kSubTable});
  REQUIRE(upper == std::vector<uint8_t>{kNsCatalog,
                                        static_cast<uint8_t>(kSubTable + 1)});
  REQUIRE(lower < upper);
}

// =========================================================================
// Catalog — counter value encoding
// =========================================================================

TEST_CASE("counter value round-trip", "[catalog]") {
  SECTION("zero") {
    auto enc = encode_counter_value(0);
    REQUIRE(enc.size() == 8);
    REQUIRE(decode_counter_value(enc.data(), enc.size()) == 0);
  }

  SECTION("one") {
    auto enc = encode_counter_value(1);
    REQUIRE(decode_counter_value(enc.data(), enc.size()) == 1);
  }

  SECTION("UINT32_MAX") {
    uint64_t val = std::numeric_limits<uint32_t>::max();
    auto enc = encode_counter_value(val);
    REQUIRE(decode_counter_value(enc.data(), enc.size()) == val);
  }

  SECTION("large uint64") {
    uint64_t val = 0xDEADBEEFCAFEBABEULL;
    auto enc = encode_counter_value(val);
    REQUIRE(decode_counter_value(enc.data(), enc.size()) == val);
  }

  SECTION("short buffer returns 0") {
    uint8_t buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    REQUIRE(decode_counter_value(buf, 4) == 0);
  }

  SECTION("big-endian byte order") {
    auto enc = encode_counter_value(1);
    // 1 in big-endian: 7 zero bytes followed by 0x01.
    for (int i = 0; i < 7; ++i) {
      REQUIRE(enc[i] == 0);
    }
    REQUIRE(enc[7] == 1);
  }
}

// =========================================================================
// Catalog — TableMeta serialization
// =========================================================================

TEST_CASE("TableMeta round-trip", "[catalog]") {
  SECTION("with columns") {
    TableMeta meta;
    meta.table_id       = 42;
    meta.schema_version = 2;  // Only v2 supported
    meta.full_name      = std::string("test\0t1\0", 8);
    meta.reclength      = 256;
    meta.null_bytes     = 2;
    meta.pk_parts       = 1;
    meta.columns        = {
        {/*field_type=*/3, /*field_length=*/11, /*is_nullable=*/0, /*charset_id=*/0},
        {/*field_type=*/15, /*field_length=*/100, /*is_nullable=*/1, /*charset_id=*/33},
    };
    meta.indexes        = {}; // Empty indexes for this test

    auto buf = serialize_table_meta(meta);
    REQUIRE(!buf.empty());

    TableMeta out;
    REQUIRE(deserialize_table_meta(buf.data(), buf.size(), out));
    REQUIRE(out.table_id == 42);
    REQUIRE(out.schema_version == 2);
    REQUIRE(out.full_name == meta.full_name);
    REQUIRE(out.reclength == 256);
    REQUIRE(out.null_bytes == 2);
    REQUIRE(out.pk_parts == 1);
    REQUIRE(out.columns.size() == 2);
    REQUIRE(out.columns[0].field_type == 3);
    REQUIRE(out.columns[0].field_length == 11);
    REQUIRE(out.columns[0].is_nullable == 0);
    REQUIRE(out.columns[1].field_type == 15);
    REQUIRE(out.columns[1].is_nullable == 1);
    REQUIRE(out.columns[1].charset_id == 33);
    REQUIRE(out.indexes.empty()); // No indexes in this test
  }

  SECTION("empty columns") {
    TableMeta meta;
    meta.table_id       = 1;
    meta.schema_version = 2;  // Only v2 supported
    meta.full_name      = "x";
    meta.reclength      = 10;
    meta.null_bytes     = 0;
    meta.pk_parts       = 0;
    meta.columns        = {};
    meta.indexes        = {};

    auto buf = serialize_table_meta(meta);
    TableMeta out;
    REQUIRE(deserialize_table_meta(buf.data(), buf.size(), out));
    REQUIRE(out.columns.empty());
    REQUIRE(out.indexes.empty());
    REQUIRE(out.table_id == 1);
  }

  SECTION("unsupported schema version") {
    TableMeta meta;
    meta.table_id       = 1;
    meta.schema_version = 1;  // Unsupported v1
    meta.full_name      = "test";
    meta.reclength      = 8;
    meta.null_bytes     = 0;
    meta.pk_parts       = 0;
    meta.columns        = {};
    meta.indexes        = {};

    auto buf = serialize_table_meta(meta);
    REQUIRE(buf.empty()); // Should return empty for v1
  }

  SECTION("truncated buffer") {
    TableMeta meta;
    meta.table_id       = 1;
    meta.schema_version = 2;  // Use v2
    meta.full_name      = "db";
    meta.reclength      = 8;
    meta.null_bytes     = 0;
    meta.pk_parts       = 0;
    meta.columns        = {{3, 11, 0, 0}};
    meta.indexes        = {}; // Empty indexes

    auto buf = serialize_table_meta(meta);
    REQUIRE(!buf.empty()); // v2 should serialize successfully

    // Truncate at various points — all must fail gracefully.
    TableMeta out;
    REQUIRE_FALSE(deserialize_table_meta(buf.data(), 5, out));
    REQUIRE_FALSE(deserialize_table_meta(buf.data(), 0, out));
    // Truncate in the middle of columns.
    REQUIRE_FALSE(deserialize_table_meta(buf.data(), buf.size() - 1, out));
  }
}

// =========================================================================
// Key encoding — table_id_prefix
// =========================================================================

TEST_CASE("table_id_prefix", "[key_encoding]") {
  SECTION("table_id 1") {
    auto p = table_id_prefix(1);
    REQUIRE(p == std::vector<uint8_t>{0x02, 0, 0, 0, 1});
  }

  SECTION("table_id 0x01020304") {
    auto p = table_id_prefix(0x01020304);
    REQUIRE(p == std::vector<uint8_t>{0x02, 1, 2, 3, 4});
  }

  SECTION("table_id 0") {
    auto p = table_id_prefix(0);
    REQUIRE(p == std::vector<uint8_t>{0x02, 0, 0, 0, 0});
  }
}

// =========================================================================
// Key encoding — key_belongs_to_table
// =========================================================================

TEST_CASE("key_belongs_to_table", "[key_encoding]") {
  SECTION("matching key") {
    uint8_t key[] = {0x02, 0, 0, 0, 1, 0xAA, 0xBB};
    REQUIRE(key_belongs_to_table(key, sizeof(key), 1));
  }

  SECTION("wrong table_id") {
    uint8_t key[] = {0x02, 0, 0, 0, 1, 0xAA};
    REQUIRE_FALSE(key_belongs_to_table(key, sizeof(key), 2));
  }

  SECTION("wrong namespace byte") {
    uint8_t key[] = {0x01, 0, 0, 0, 1};
    REQUIRE_FALSE(key_belongs_to_table(key, sizeof(key), 1));
  }

  SECTION("key too short") {
    uint8_t key[] = {0x02, 0, 0};
    REQUIRE_FALSE(key_belongs_to_table(key, sizeof(key), 0));
  }

  SECTION("exact 5-byte key") {
    uint8_t key[] = {0x02, 0, 0, 0, 5};
    REQUIRE(key_belongs_to_table(key, sizeof(key), 5));
  }
}

// =========================================================================
// Key encoding — table_id_upper_bound
// =========================================================================

TEST_CASE("table_id_upper_bound", "[key_encoding]") {
  SECTION("table_id 1") {
    auto ub = table_id_upper_bound(1);
    REQUIRE(ub == std::vector<uint8_t>{0x02, 0, 0, 0, 2});
  }

  SECTION("upper bound > prefix (byte ordering)") {
    auto prefix = table_id_prefix(42);
    auto ub     = table_id_upper_bound(42);
    REQUIRE(prefix < ub);
  }

  SECTION("key at prefix boundary belongs, key at upper bound doesn't") {
    uint32_t tid = 10;
    auto prefix = table_id_prefix(tid);
    auto ub     = table_id_upper_bound(tid);

    REQUIRE(key_belongs_to_table(prefix.data(), prefix.size(), tid));
    REQUIRE_FALSE(key_belongs_to_table(ub.data(), ub.size(), tid));
  }
}

// =========================================================================
// Row encoding — version envelope
// =========================================================================

TEST_CASE("row encoding round-trip", "[row_encoding]") {
  // Set up a stub TABLE with reclength = 8, no fields (all data in null bitmap).
  TABLE_SHARE share{};
  share.reclength = 8;
  share.null_bytes = 8;
  share.fields = 0;
  TABLE tbl{};
  tbl.s = &share;
  uchar rec_buf[8] = {};
  tbl.record[0] = rec_buf;

  SECTION("encode produces correct envelope") {
    uchar row[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    auto encoded = encode_row(make_row_plan(&tbl), row, /*schema_version=*/7);

    // 3-byte envelope + 8 bytes null bitmap.
    REQUIRE(encoded.size() == 11);
    REQUIRE(encoded[0] == 0x02);  // format version V2
    REQUIRE(encoded[1] == 7);     // schema_version low byte
    REQUIRE(encoded[2] == 0);     // schema_version high byte
    REQUIRE(encoded[3] == 0x10);  // first data byte
    REQUIRE(encoded[10] == 0x80); // last data byte
  }

  SECTION("schema_version > 255") {
    uchar row[8] = {};
    auto encoded = encode_row(make_row_plan(&tbl), row, /*schema_version=*/0x0301);

    REQUIRE(encoded[1] == 0x01);  // low byte
    REQUIRE(encoded[2] == 0x03);  // high byte
  }

  SECTION("decode restores original data") {
    uchar original[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto encoded = encode_row(make_row_plan(&tbl), original, /*schema_version=*/1);

    uchar decoded[8] = {};
    decode_row(make_row_plan(&tbl), encoded.data(), encoded.size(), decoded);
    REQUIRE(std::memcmp(original, decoded, 8) == 0);
  }

  SECTION("decode zeros buf when payload too short for null bitmap") {
    // Envelope (3 bytes) + 4 bytes = 7 total, but null_bytes = 8.
    uint8_t short_value[] = {0x02, 0x01, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};

    uchar decoded[8];
    std::memset(decoded, 0xFF, 8);
    decode_row(make_row_plan(&tbl), short_value, sizeof(short_value), decoded);

    // buf zeroed by initial memset in decode.
    for (int i = 0; i < 8; ++i) {
      REQUIRE(decoded[i] == 0);
    }
  }

  SECTION("decode with empty/too-short value zeros everything") {
    uchar decoded[8];
    std::memset(decoded, 0xFF, 8);
    // Only 2 bytes — less than the 3-byte envelope.
    uint8_t tiny[] = {0x02, 0x00};
    decode_row(make_row_plan(&tbl), tiny, sizeof(tiny), decoded);

    for (int i = 0; i < 8; ++i) {
      REQUIRE(decoded[i] == 0);
    }
  }
}

// =========================================================================
// Row encoding V2 — compact format with trailing-space stripping
// =========================================================================

TEST_CASE("V2 row encoding with compactable CHAR fields", "[row_encoding]") {
  // Simulate: null_bytes=1, Field_long(4 bytes) at offset 1,
  //           Field_string(480 bytes, utf8mb4) at offset 5.
  static CHARSET_INFO utf8mb4_cs{45, 0, 1, 4};

  // Record buffer: [null_bitmap:1][int:4][char:480] = 485 bytes
  static constexpr uint reclength = 485;
  TABLE_SHARE share{};
  share.reclength = reclength;
  share.null_bytes = 1;
  share.fields = 2;

  // Set up record buffer.
  uchar record[reclength];
  std::memset(record, 0, reclength);

  // Field_long at offset 1.
  Field_long f_int;
  f_int.ptr = record + 1;
  f_int.field_length = 4;

  // Field_string at offset 5, CHAR(120) CHARACTER SET utf8mb4 = 480 bytes.
  Field_string f_char;
  f_char.ptr = record + 5;
  f_char.field_length = 480;
  f_char.cs_ = &utf8mb4_cs;

  Field *fields[] = {&f_int, &f_char};
  TABLE tbl{};
  tbl.s = &share;
  tbl.field = fields;
  tbl.record[0] = record;

  SECTION("encode strips trailing spaces") {
    record[0] = 0x00;  // null bitmap
    // INT = 42
    int32_t int_val = 42;
    std::memcpy(record + 1, &int_val, 4);
    // CHAR = "hello" padded with spaces to 480 bytes.
    std::memset(record + 5, 0x20, 480);
    std::memcpy(record + 5, "hello", 5);

    auto encoded = encode_row(make_row_plan(&tbl), record, /*schema_version=*/1);

    // Envelope (3) + null_bytes (1) + int (4) + len_prefix (2) + stripped_data.
    // Minimum stripped length = 480/4 = 120 chars.
    // "hello" is 5 bytes, but floor is 120, so actual_len = 120.
    REQUIRE(encoded[0] == 0x02);  // V2 format
    std::size_t expected_size = 3 + 1 + 4 + 2 + 120;
    REQUIRE(encoded.size() == expected_size);
  }

  SECTION("decode restores padded CHAR field") {
    record[0] = 0x00;
    int32_t int_val = 99;
    std::memcpy(record + 1, &int_val, 4);
    std::memset(record + 5, 0x20, 480);
    std::memcpy(record + 5, "world", 5);

    auto encoded = encode_row(make_row_plan(&tbl), record, 1);

    // Decode into a fresh buffer.
    uchar decoded[reclength];
    std::memset(decoded, 0xCC, reclength);
    decode_row(make_row_plan(&tbl), encoded.data(), encoded.size(), decoded);

    // Null bitmap preserved.
    REQUIRE(decoded[0] == 0x00);
    // INT preserved.
    int32_t decoded_int;
    std::memcpy(&decoded_int, decoded + 1, 4);
    REQUIRE(decoded_int == 99);
    // CHAR content preserved.
    REQUIRE(std::memcmp(decoded + 5, "world", 5) == 0);
    // Trailing bytes re-padded with 0x20.
    for (uint i = 5; i < 480; ++i) {
      REQUIRE(decoded[5 + i] == 0x20);
    }
  }

  SECTION("round-trip preserves full content") {
    record[0] = 0x03;  // some null bits set
    int32_t int_val = -12345;
    std::memcpy(record + 1, &int_val, 4);
    // Fill CHAR with multi-byte-ish ASCII content.
    std::memset(record + 5, 0x20, 480);
    for (int i = 0; i < 120; ++i) {
      record[5 + i] = static_cast<uchar>('A' + (i % 26));
    }

    auto encoded = encode_row(make_row_plan(&tbl), record, 7);

    uchar decoded[reclength];
    decode_row(make_row_plan(&tbl), encoded.data(), encoded.size(), decoded);

    REQUIRE(std::memcmp(record, decoded, reclength) == 0);
  }

  SECTION("all-space content strips to minimum length") {
    record[0] = 0x00;
    int32_t int_val = 0;
    std::memcpy(record + 1, &int_val, 4);
    // Entire CHAR field is spaces.
    std::memset(record + 5, 0x20, 480);

    auto encoded = encode_row(make_row_plan(&tbl), record, 1);

    // stripped to n_chars = 480/4 = 120 bytes (all spaces).
    std::size_t expected_size = 3 + 1 + 4 + 2 + 120;
    REQUIRE(encoded.size() == expected_size);

    // Round-trip: decoded should be all spaces in the CHAR region.
    uchar decoded[reclength];
    decode_row(make_row_plan(&tbl), encoded.data(), encoded.size(), decoded);
    for (uint i = 0; i < 480; ++i) {
      REQUIRE(decoded[5 + i] == 0x20);
    }
  }

  SECTION("content longer than n_chars preserves exact length") {
    record[0] = 0x00;
    int32_t int_val = 1;
    std::memcpy(record + 1, &int_val, 4);
    // 200 bytes of non-space content + 280 bytes of space.
    std::memset(record + 5, 0x20, 480);
    for (int i = 0; i < 200; ++i) {
      record[5 + i] = static_cast<uchar>('X');
    }

    auto encoded = encode_row(make_row_plan(&tbl), record, 1);

    // actual_len should be 200 (> n_chars=120, trailing spaces stripped).
    std::size_t expected_size = 3 + 1 + 4 + 2 + 200;
    REQUIRE(encoded.size() == expected_size);

    uchar decoded[reclength];
    decode_row(make_row_plan(&tbl), encoded.data(), encoded.size(), decoded);
    REQUIRE(std::memcmp(record, decoded, reclength) == 0);
  }
}

TEST_CASE("V2 row encoding with non-compactable CHAR (latin1)", "[row_encoding]") {
  // latin1: mbminlen=1, mbmaxlen=1 → not compactable.
  static CHARSET_INFO latin1_cs{8, 0, 1, 1};

  static constexpr uint reclength = 125;  // null(1) + int(4) + char(120)
  TABLE_SHARE share{};
  share.reclength = reclength;
  share.null_bytes = 1;
  share.fields = 2;

  uchar record[reclength];
  std::memset(record, 0, reclength);

  Field_long f_int;
  f_int.ptr = record + 1;
  f_int.field_length = 4;

  Field_string f_char;
  f_char.ptr = record + 5;
  f_char.field_length = 120;
  f_char.cs_ = &latin1_cs;

  Field *fields[] = {&f_int, &f_char};
  TABLE tbl{};
  tbl.s = &share;
  tbl.field = fields;
  tbl.record[0] = record;

  SECTION("latin1 CHAR stored verbatim — no stripping") {
    record[0] = 0x00;
    int32_t int_val = 7;
    std::memcpy(record + 1, &int_val, 4);
    std::memset(record + 5, 0x20, 120);
    std::memcpy(record + 5, "abc", 3);

    auto encoded = encode_row(make_row_plan(&tbl), record, 1);

    // No length prefix for non-compactable: envelope(3) + null(1) + int(4) + char(120)
    std::size_t expected_size = 3 + 1 + 4 + 120;
    REQUIRE(encoded.size() == expected_size);

    uchar decoded[reclength];
    decode_row(make_row_plan(&tbl), encoded.data(), encoded.size(), decoded);
    REQUIRE(std::memcmp(record, decoded, reclength) == 0);
  }
}

TEST_CASE("V2 decode rejects V1 format", "[row_encoding]") {
  TABLE_SHARE share{};
  share.reclength = 8;
  share.null_bytes = 0;
  share.fields = 0;
  TABLE tbl{};
  tbl.s = &share;
  tbl.record[0] = nullptr;

  uint8_t v1_data[] = {0x01, 0x01, 0x00, 1, 2, 3, 4, 5, 6, 7, 8};
  uchar decoded[8];

  REQUIRE_THROWS_AS(
      decode_row(make_row_plan(&tbl), v1_data, sizeof(v1_data), decoded),
      std::runtime_error);
}

// =========================================================================
// Secondary index key encoding
// =========================================================================

TEST_CASE("index_id_prefix", "[key_encoding]") {
  SECTION("table_id 1, index_id 2") {
    auto p = index_id_prefix(1, 2);
    REQUIRE(p == std::vector<uint8_t>{0x03, 0, 0, 0, 1, 0, 2});
  }

  SECTION("table_id 0x01020304, index_id 0x0506") {
    auto p = index_id_prefix(0x01020304, 0x0506);
    REQUIRE(p == std::vector<uint8_t>{0x03, 1, 2, 3, 4, 5, 6});
  }
}

TEST_CASE("index_id_upper_bound", "[key_encoding]") {
  SECTION("table_id 1, index_id 2") {
    auto ub = index_id_upper_bound(1, 2);
    REQUIRE(ub == std::vector<uint8_t>{0x03, 0, 0, 0, 1, 0, 3});
  }

  SECTION("upper bound > prefix") {
    auto prefix = index_id_prefix(42, 3);
    auto ub     = index_id_upper_bound(42, 3);
    REQUIRE(prefix < ub);
  }
}

TEST_CASE("key_belongs_to_index", "[key_encoding]") {
  SECTION("matching key") {
    uint8_t key[] = {0x03, 0, 0, 0, 1, 0, 2, 0xAA, 0xBB};
    REQUIRE(key_belongs_to_index(key, sizeof(key), 1, 2));
  }

  SECTION("wrong table_id") {
    uint8_t key[] = {0x03, 0, 0, 0, 1, 0, 2, 0xAA};
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 2, 2));
  }

  SECTION("wrong index_id") {
    uint8_t key[] = {0x03, 0, 0, 0, 1, 0, 2, 0xAA};
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 1, 3));
  }

  SECTION("wrong namespace byte") {
    uint8_t key[] = {0x02, 0, 0, 0, 1, 0, 2};
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 1, 2));
  }

  SECTION("key too short") {
    uint8_t key[] = {0x03, 0, 0};
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 0, 0));
  }

  SECTION("exact 7-byte key") {
    uint8_t key[] = {0x03, 0, 0, 0, 5, 0, 1};
    REQUIRE(key_belongs_to_index(key, sizeof(key), 5, 1));
  }
}

TEST_CASE("extract_pk_from_sec_key", "[key_encoding]") {
  SECTION("normal case") {
    // Key format: [ns:1][tid:4][iid:2][sec_key:3][pk:2]
    uint8_t sec_key[] = {0x03, 0, 0, 0, 1, 0, 2,  // 7-byte prefix
                         0xAA, 0xBB, 0xCC,          // 3-byte sec key
                         0xDD, 0xEE};               // 2-byte PK
    std::size_t pk_len = 0;
    const uint8_t *pk = extract_pk_from_sec_key(sec_key, sizeof(sec_key), 3, &pk_len);
    REQUIRE(pk != nullptr);
    REQUIRE(pk_len == 2);
    REQUIRE(pk[0] == 0xDD);
    REQUIRE(pk[1] == 0xEE);
  }

  SECTION("empty PK") {
    uint8_t sec_key[] = {0x03, 0, 0, 0, 1, 0, 2,  // 7-byte prefix
                         0xAA, 0xBB};              // 2-byte sec key only
    std::size_t pk_len = 0;
    const uint8_t *pk = extract_pk_from_sec_key(sec_key, sizeof(sec_key), 2, &pk_len);
    REQUIRE(pk == nullptr);
    REQUIRE(pk_len == 0);
  }

  SECTION("key too short") {
    uint8_t sec_key[] = {0x03, 0, 0};
    std::size_t pk_len = 0;
    const uint8_t *pk = extract_pk_from_sec_key(sec_key, sizeof(sec_key), 10, &pk_len);
    REQUIRE(pk == nullptr);
    REQUIRE(pk_len == 0);
  }
}

TEST_CASE("TableMeta with indexes (schema v2)", "[catalog]") {
  SECTION("schema version 2 with indexes") {
    TableMeta meta;
    meta.table_id       = 42;
    meta.schema_version = 2;  // v2 includes indexes
    meta.full_name      = std::string("test\0t1\0", 8);
    meta.reclength      = 256;
    meta.null_bytes     = 2;
    meta.pk_parts       = 1;
    meta.columns        = {{3, 11, 0, 0}};
    meta.indexes        = {
        {/*index_id=*/1, /*key_parts=*/1, /*is_unique=*/1, /*columns=*/{0}},
        {/*index_id=*/2, /*key_parts=*/2, /*is_unique=*/0, /*columns=*/{0, 1}}
    };

    auto buf = serialize_table_meta(meta);
    REQUIRE(!buf.empty());

    TableMeta out;
    REQUIRE(deserialize_table_meta(buf.data(), buf.size(), out));
    REQUIRE(out.schema_version == 2);
    REQUIRE(out.indexes.size() == 2);
    REQUIRE(out.indexes[0].index_id == 1);
    REQUIRE(out.indexes[0].key_parts == 1);
    REQUIRE(out.indexes[0].is_unique == 1);
    REQUIRE(out.indexes[0].column_indexes == std::vector<uint16_t>{0});
    REQUIRE(out.indexes[1].index_id == 2);
    REQUIRE(out.indexes[1].key_parts == 2);
    REQUIRE(out.indexes[1].is_unique == 0);
    REQUIRE(out.indexes[1].column_indexes == std::vector<uint16_t>{0, 1});
  }
}

// =========================================================================
// Secondary index operations - key structure tests only
// =========================================================================

TEST_CASE("index_id_prefix and bounds", "[key_encoding]") {
  SECTION("index prefix generation") {
    auto prefix = index_id_prefix(100, 5);
    REQUIRE(prefix.size() == 7);
    REQUIRE(prefix[0] == 0x03);  // index namespace
    // table_id = 100 in big-endian
    REQUIRE(prefix[1] == 0x00);
    REQUIRE(prefix[2] == 0x00);
    REQUIRE(prefix[3] == 0x00);
    REQUIRE(prefix[4] == 0x64);  // 100
    // index_id = 5 in big-endian
    REQUIRE(prefix[5] == 0x00);
    REQUIRE(prefix[6] == 0x05);  // 5
  }

  SECTION("index upper bound") {
    auto upper = index_id_upper_bound(100, 5);
    REQUIRE(upper.size() == 7);
    REQUIRE(upper[0] == 0x03);
    REQUIRE(upper[4] == 0x64);   // table_id = 100
    REQUIRE(upper[6] == 0x06);   // index_id = 6 (5 + 1)
  }

  SECTION("bounds ordering") {
    auto prefix = index_id_prefix(42, 3);
    auto upper = index_id_upper_bound(42, 3);
    REQUIRE(prefix < upper);
  }
}

TEST_CASE("key_belongs_to_index validation", "[key_encoding]") {
  SECTION("matching index key") {
    uint8_t key[] = {0x03, 0, 0, 0, 50, 0, 7, 0xAA, 0xBB, 0xCC};
    REQUIRE(key_belongs_to_index(key, sizeof(key), 50, 7));
  }

  SECTION("wrong table_id") {
    uint8_t key[] = {0x03, 0, 0, 0, 50, 0, 7, 0xAA};
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 51, 7));
  }

  SECTION("wrong index_id") {
    uint8_t key[] = {0x03, 0, 0, 0, 50, 0, 7, 0xAA};
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 50, 8));
  }

  SECTION("wrong namespace") {
    uint8_t key[] = {0x02, 0, 0, 0, 50, 0, 7};  // row namespace, not index
    REQUIRE_FALSE(key_belongs_to_index(key, sizeof(key), 50, 7));
  }
}

TEST_CASE("extract_pk_from_sec_key validation", "[key_encoding]") {
  SECTION("normal extraction") {
    // Format: [ns:1][tid:4][iid:2][sec_key:N][pk:M]
    uint8_t sec_key[] = {
      0x03,                    // namespace
      0x00, 0x00, 0x00, 0x2A,  // table_id = 42
      0x00, 0x03,              // index_id = 3
      0xAA, 0xBB, 0xCC,        // 3-byte secondary key
      0x00, 0x00, 0x00, 0x10   // 4-byte PK = 16
    };

    std::size_t pk_len = 0;
    const uint8_t *pk = extract_pk_from_sec_key(sec_key, sizeof(sec_key), 3, &pk_len);
    REQUIRE(pk != nullptr);
    REQUIRE(pk_len == 4);
    REQUIRE(std::vector<uint8_t>(pk, pk + pk_len) ==
            std::vector<uint8_t>{0x00, 0x00, 0x00, 0x10});
  }

  SECTION("empty PK section") {
    uint8_t sec_key[] = {
      0x03, 0, 0, 0, 42, 0, 3,  // 7-byte prefix
      0xAA, 0xBB                // 2-byte secondary key only (no PK)
    };
    std::size_t pk_len = 0;
    const uint8_t *pk = extract_pk_from_sec_key(sec_key, sizeof(sec_key), 2, &pk_len);
    REQUIRE(pk == nullptr);
    REQUIRE(pk_len == 0);
  }

  SECTION("truncated key") {
    uint8_t sec_key[] = {0x03, 0, 0};  // Too short
    std::size_t pk_len = 0;
    const uint8_t *pk = extract_pk_from_sec_key(sec_key, sizeof(sec_key), 10, &pk_len);
    REQUIRE(pk == nullptr);
    REQUIRE(pk_len == 0);
  }
}

// =========================================================================
// _into variants — buffer-reuse API
// =========================================================================

TEST_CASE("encode_row_into matches encode_row", "[row_encoding]") {
  TABLE_SHARE share{};
  share.reclength = 8;
  share.null_bytes = 8;
  share.fields = 0;
  TABLE tbl{};
  tbl.s = &share;
  uchar rec_buf[8] = {};
  tbl.record[0] = rec_buf;

  uchar row[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};

  SECTION("equivalent output") {
    auto returned = encode_row(make_row_plan(&tbl), row, 7);
    std::vector<uint8_t> into;
    encode_row_into(into, make_row_plan(&tbl), row, 7);
    REQUIRE(into == returned);
  }

  SECTION("clear+append semantics — pre-populated buffer is overwritten") {
    std::vector<uint8_t> into(64, 0xCC);
    auto cap_before = into.capacity();
    encode_row_into(into, make_row_plan(&tbl), row, 7);
    auto returned = encode_row(make_row_plan(&tbl), row, 7);
    REQUIRE(into == returned);
    REQUIRE(into.capacity() >= cap_before);
  }

  SECTION("repeated reuse across rows") {
    std::vector<uint8_t> into;
    encode_row_into(into, make_row_plan(&tbl), row, 7);
    auto cap_after_first = into.capacity();

    uchar row2[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    encode_row_into(into, make_row_plan(&tbl), row2, 9);
    REQUIRE(into == encode_row(make_row_plan(&tbl), row2, 9));
    REQUIRE(into.capacity() >= cap_after_first);
  }
}

TEST_CASE("encode_pk_into matches encode_pk (PK-less synthetic rowid)",
          "[key_encoding]") {
  // PK-less path is exercisable from stubs: pk_idx == MAX_KEY → 8-byte rowid.
  TABLE_SHARE share{};
  share.primary_key = MAX_KEY;
  TABLE tbl{};
  tbl.s = &share;
  uchar buf[1] = {0};

  SECTION("equivalent output") {
    auto returned = encode_pk(&tbl, make_table_codec(&tbl), buf, 42, 0xDEADBEEFCAFEBABEULL);
    std::vector<uint8_t> into;
    encode_pk_into(into, &tbl, make_table_codec(&tbl), buf, 42, 0xDEADBEEFCAFEBABEULL);
    REQUIRE(into == returned);
  }

  SECTION("clear+append — pre-populated junk overwritten") {
    std::vector<uint8_t> into(32, 0xAB);
    auto cap_before = into.capacity();
    encode_pk_into(into, &tbl, make_table_codec(&tbl), buf, 42, 1);
    REQUIRE(into == encode_pk(&tbl, make_table_codec(&tbl), buf, 42, 1));
    REQUIRE(into.capacity() >= cap_before);
  }

  SECTION("repeated reuse across rows") {
    std::vector<uint8_t> into;
    encode_pk_into(into, &tbl, make_table_codec(&tbl), buf, 42, 1);
    auto cap_after_first = into.capacity();
    encode_pk_into(into, &tbl, make_table_codec(&tbl), buf, 42, 999);
    REQUIRE(into == encode_pk(&tbl, make_table_codec(&tbl), buf, 42, 999));
    REQUIRE(into.capacity() >= cap_after_first);
  }
}

TEST_CASE("schema v2 index metadata", "[catalog]") {
  SECTION("index metadata serialization") {
    TableMeta meta;
    meta.table_id = 123;
    meta.schema_version = 2;
    meta.full_name = std::string("test\0table\0", 11);
    meta.reclength = 32;
    meta.null_bytes = 1;
    meta.pk_parts = 1;
    meta.columns = {{1, 4, 0, 0}};
    meta.indexes = {
      {1, 1, 0, {0}},  // Regular index on column 0
      {2, 1, 1, {0}}   // Unique index on column 0
    };

    auto serialized = serialize_table_meta(meta);
    REQUIRE(!serialized.empty());

    TableMeta deserialized;
    REQUIRE(deserialize_table_meta(serialized.data(), serialized.size(), deserialized));

    REQUIRE(deserialized.schema_version == 2);
    REQUIRE(deserialized.indexes.size() == 2);
    REQUIRE(deserialized.indexes[0].index_id == 1);
    REQUIRE(deserialized.indexes[0].is_unique == 0);
    REQUIRE(deserialized.indexes[1].index_id == 2);
    REQUIRE(deserialized.indexes[1].is_unique == 1);
  }
}

// =========================================================================
// TableCodec — the per-table plan the row and key codecs walk
// =========================================================================

namespace {

struct UnsignedLongField : Field_long {
  bool is_unsigned() const override { return true; }
};

struct DoubleField : Field {
  enum_field_types type() const override { return MYSQL_TYPE_DOUBLE; }
  uint pack_length() const override { return 8; }
};

// One table with a single-part key on `field`, whose value sits at record
// offset 1 (after a one-byte null bitmap).
struct OneKeyTable {
  uchar record[16]{};
  TABLE_SHARE share{};
  KEY_PART_INFO part{};
  KEY key{};
  Field *fields[1]{};
  TABLE tbl{};

  OneKeyTable(Field &field, uint length, bool nullable) {
    field.ptr = record + 1;
    field.field_length = length;
    fields[0] = &field;
    share.reclength = sizeof(record);
    share.null_bytes = 1;
    share.fields = 1;
    share.keys = 1;
    part.field = &field;
    part.fieldnr = 1;
    part.length = length;
    part.store_length = length + (nullable ? 1 : 0);
    part.null_bit = nullable ? 1 : 0;
    key.key_length = part.store_length;
    key.user_defined_key_parts = 1;
    key.key_part = &part;
    tbl.s = &share;
    tbl.key_info = &key;
    tbl.field = fields;
    tbl.record[0] = record;
  }
};

template <typename T>
std::vector<uint8_t> native_bytes(T v) {
  std::vector<uint8_t> b(sizeof(T));
  std::memcpy(b.data(), &v, sizeof(T));
  return b;
}

// Encodes each value's native bytes, checks the encoded keys sort as the
// values do, and that undo restores the native bytes exactly.
template <typename T>
void check_order_and_round_trip(const KeyPlan &plan, std::vector<T> values) {
  std::sort(values.begin(), values.end());
  std::vector<std::vector<uint8_t>> encoded;
  for (T v : values) {
    auto bytes = native_bytes(v);
    auto key = bytes;
    make_mem_comparable(key.data(), plan, static_cast<uint>(key.size()));
    auto back = key;
    undo_mem_comparable(back.data(), plan, static_cast<uint>(back.size()));
    INFO("value " << v);
    REQUIRE(back == bytes);
    encoded.push_back(key);
  }
  REQUIRE(std::is_sorted(encoded.begin(), encoded.end()));
}

}  // namespace

TEST_CASE("make_row_plan reads each column's layout once", "[row_encoding]") {
  static CHARSET_INFO utf8mb4_cs{45, 0, 1, 4};
  static CHARSET_INFO latin1_cs{8, 0, 1, 1};
  uchar record[64]{};
  TABLE_SHARE share{};
  share.reclength = sizeof(record);
  share.null_bytes = 1;
  share.fields = 4;

  Field_long f_int;
  f_int.ptr = record + 1;
  Field_string f_utf8;  // CHAR(4) utf8mb4: compacted
  f_utf8.ptr = record + 5;
  f_utf8.field_length = 16;
  f_utf8.cs_ = &utf8mb4_cs;
  Field_string f_latin1;  // CHAR(8) latin1: stored as is
  f_latin1.ptr = record + 21;
  f_latin1.field_length = 8;
  f_latin1.cs_ = &latin1_cs;
  Field_blob f_blob;
  f_blob.ptr = record + 29;
  f_blob.pack_len_no_ptr_ = 2;

  Field *fields[] = {&f_int, &f_utf8, &f_latin1, &f_blob};
  TABLE tbl{};
  tbl.s = &share;
  tbl.field = fields;
  tbl.record[0] = record;

  const RowPlan plan = make_row_plan(&tbl);
  using Kind = RowPlan::Kind;
  REQUIRE(plan.null_bytes == 1);
  REQUIRE(plan.reclength == sizeof(record));
  REQUIRE(plan.columns.size() == 4);
  CHECK(plan.columns[0].kind == Kind::kFixed);
  CHECK(plan.columns[0].offset == 1);
  CHECK(plan.columns[0].pack_length == 4);
  CHECK(plan.columns[1].kind == Kind::kCompactChar);
  CHECK(plan.columns[1].offset == 5);
  CHECK(plan.columns[1].field_length == 16);
  CHECK(plan.columns[1].mbmaxlen == 4);
  CHECK(plan.columns[2].kind == Kind::kFixed);
  CHECK(plan.columns[2].pack_length == 8);
  CHECK(plan.columns[3].kind == Kind::kBlob);
  CHECK(plan.columns[3].offset == 29);
  CHECK(plan.columns[3].blob_len_bytes == 2);
  CHECK(plan.columns[3].pack_length == 2 + sizeof(uchar *));

  SECTION("a blob round-trips through the plan") {
    const char text[] = "blob bytes";
    const uint16_t len = sizeof(text) - 1;
    std::memcpy(record + 29, &len, 2);
    const uchar *data_ptr = reinterpret_cast<const uchar *>(text);
    std::memcpy(record + 31, &data_ptr, sizeof(data_ptr));
    auto encoded = encode_row(plan, record, 1);

    uchar decoded[64]{};
    decode_row(plan, encoded.data(), encoded.size(), decoded);
    uint16_t got_len = 0;
    std::memcpy(&got_len, decoded + 29, 2);
    REQUIRE(got_len == len);
    const uchar *got_ptr = nullptr;
    std::memcpy(&got_ptr, decoded + 31, sizeof(got_ptr));
    REQUIRE(got_ptr != nullptr);
    REQUIRE(std::memcmp(got_ptr, text, len) == 0);
  }
}

TEST_CASE("key plan transforms sort by value and round-trip",
          "[key_encoding]") {
  SECTION("signed integer") {
    Field_long f;
    OneKeyTable t(f, 4, /*nullable=*/false);
    const TableCodec codec = make_table_codec(&t.tbl);
    REQUIRE(codec.keys.size() == 1);
    const KeyPartPlan &p = codec.keys[0].parts.at(0);
    CHECK(p.reverse);
    CHECK(p.flip == KeyPartPlan::Flip::kSign);
    CHECK(p.record_offset == 1);
    check_order_and_round_trip<int32_t>(
        codec.keys[0], {std::numeric_limits<int32_t>::min(), -5, -1, 0, 1, 7,
                        std::numeric_limits<int32_t>::max()});
  }

  SECTION("unsigned integer") {
    UnsignedLongField f;
    OneKeyTable t(f, 4, false);
    const TableCodec codec = make_table_codec(&t.tbl);
    CHECK(codec.keys[0].parts.at(0).flip == KeyPartPlan::Flip::kNone);
    check_order_and_round_trip<uint32_t>(
        codec.keys[0], {0u, 1u, 255u, 256u, 0x80000000u, 0xFFFFFFFFu});
  }

  SECTION("double") {
    DoubleField f;
    OneKeyTable t(f, 8, false);
    const TableCodec codec = make_table_codec(&t.tbl);
    CHECK(codec.keys[0].parts.at(0).flip == KeyPartPlan::Flip::kFloat);
    check_order_and_round_trip<double>(
        codec.keys[0], {-1e10, -2.5, -0.5, 0.0, 0.5, 1.5, 1e10});
  }

  SECTION("nullable: NULL sorts first and both round-trip") {
    Field_long f;
    OneKeyTable t(f, 4, /*nullable=*/true);
    const TableCodec codec = make_table_codec(&t.tbl);
    const KeyPlan &plan = codec.keys[0];
    std::vector<uint8_t> null_key = {1, 0, 0, 0, 0};  // null byte set
    std::vector<uint8_t> zero_key = {0, 0, 0, 0, 0};  // NOT NULL, value 0
    auto null_enc = null_key, zero_enc = zero_key;
    make_mem_comparable(null_enc.data(), plan, 5);
    make_mem_comparable(zero_enc.data(), plan, 5);
    REQUIRE(null_enc < zero_enc);
    undo_mem_comparable(null_enc.data(), plan, 5);
    undo_mem_comparable(zero_enc.data(), plan, 5);
    REQUIRE(null_enc == null_key);
    REQUIRE(zero_enc == zero_key);
  }
}

TEST_CASE("decode_sec_key_into_record restores the key's columns",
          "[key_encoding]") {
  Field_long f;
  OneKeyTable t(f, 4, false);
  const TableCodec codec = make_table_codec(&t.tbl);
  const KeyPlan &plan = codec.keys[0];

  auto value = native_bytes<int32_t>(-42);
  auto encoded = value;
  make_mem_comparable(encoded.data(), plan, 4);
  std::vector<uint8_t> sec_key = {0x03, 0, 0, 0, 1, 0, 1};  // ns | tid | iid
  sec_key.insert(sec_key.end(), encoded.begin(), encoded.end());

  uchar record[16]{};
  REQUIRE(decode_sec_key_into_record(plan, sec_key.data(), sec_key.size(),
                                     record));
  REQUIRE(std::memcmp(record + 1, value.data(), 4) == 0);

  SECTION("a nullable part is left to the full row fetch") {
    Field_long g;
    OneKeyTable n(g, 4, /*nullable=*/true);
    const TableCodec ncodec = make_table_codec(&n.tbl);
    const KeyPlan &nplan = ncodec.keys[0];
    REQUIRE_FALSE(decode_sec_key_into_record(nplan, sec_key.data(),
                                             sec_key.size(), record));
  }
}

namespace {

// A temporal column as MariaDB reports it: type() is the old enum, and
// real_type() says whether the column is the big-endian *2 format.
struct TemporalField : Field {
  enum_field_types old_type;
  enum_field_types real;
  uint bytes;
  TemporalField(enum_field_types t, enum_field_types r, uint n)
      : old_type{t}, real{r}, bytes{n} {}
  enum_field_types type() const override { return old_type; }
  enum_field_types real_type() const override { return real; }
  uint pack_length() const override { return bytes; }
};

}  // namespace

TEST_CASE("key plan leaves big-endian temporal parts as they are",
          "[key_encoding]") {
  struct Kind {
    const char *name;
    enum_field_types type, real;
    uint bytes;
  };
  const Kind kinds[] = {
      {"DATETIME2", MYSQL_TYPE_DATETIME, MYSQL_TYPE_DATETIME2, 5},
      {"TIMESTAMP2", MYSQL_TYPE_TIMESTAMP, MYSQL_TYPE_TIMESTAMP2, 4},
      {"TIME2", MYSQL_TYPE_TIME, MYSQL_TYPE_TIME2, 3},
  };
  for (const auto &k : kinds) {
    INFO(k.name);
    TemporalField f{k.type, k.real, k.bytes};
    OneKeyTable t(f, k.bytes, /*nullable=*/false);
    const TableCodec codec = make_table_codec(&t.tbl);
    const KeyPlan &plan = codec.keys[0];
    const KeyPartPlan &p = plan.parts.at(0);
    CHECK(p.be_temporal);
    CHECK_FALSE(p.reverse);
    CHECK(p.flip == KeyPartPlan::Flip::kNone);

    // key_copy() already wrote these big-endian: the transforms leave them
    // unchanged both ways, so the stored keys sort as the values do.
    std::vector<std::vector<uint8_t>> values;
    for (uint8_t lead : {0x80, 0x81, 0x99, 0xFE}) {
      std::vector<uint8_t> v(k.bytes, 0);
      v[0] = lead;
      v[k.bytes - 1] = 0x01;
      values.push_back(v);
    }
    std::vector<std::vector<uint8_t>> encoded;
    for (const auto &v : values) {
      auto key = v;
      make_mem_comparable(key.data(), plan, k.bytes);
      REQUIRE(key == v);
      auto back = key;
      undo_mem_comparable(back.data(), plan, k.bytes);
      REQUIRE(back == v);
      encoded.push_back(key);
    }
    REQUIRE(std::is_sorted(encoded.begin(), encoded.end()));

    // The key-only read copies it into the record as it is.
    std::vector<uint8_t> sec_key = {0x03, 0, 0, 0, 1, 0, 1};
    sec_key.insert(sec_key.end(), values[2].begin(), values[2].end());
    uchar record[16]{};
    REQUIRE(decode_sec_key_into_record(plan, sec_key.data(), sec_key.size(),
                                       record));
    REQUIRE(std::memcmp(record + 1, values[2].data(), k.bytes) == 0);
  }

  SECTION("the old little-endian DATETIME is still reversed") {
    TemporalField f{MYSQL_TYPE_DATETIME, MYSQL_TYPE_DATETIME, 8};
    OneKeyTable t(f, 8, false);
    const TableCodec codec = make_table_codec(&t.tbl);
    const KeyPartPlan &p = codec.keys[0].parts.at(0);
    CHECK_FALSE(p.be_temporal);
    CHECK(p.reverse);
    CHECK(p.flip == KeyPartPlan::Flip::kNone);
    check_order_and_round_trip<uint64_t>(
        codec.keys[0], {0ULL, 1ULL, 20261009000000ULL, 99991231235959ULL});
  }
}
