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
    meta.schema_version = 3;
    meta.full_name      = std::string("test\0t1\0", 8);
    meta.reclength      = 256;
    meta.null_bytes     = 2;
    meta.pk_parts       = 1;
    meta.columns        = {
        {/*field_type=*/3, /*field_length=*/11, /*is_nullable=*/0, /*charset_id=*/0},
        {/*field_type=*/15, /*field_length=*/100, /*is_nullable=*/1, /*charset_id=*/33},
    };

    auto buf = serialize_table_meta(meta);
    REQUIRE(!buf.empty());

    TableMeta out;
    REQUIRE(deserialize_table_meta(buf.data(), buf.size(), out));
    REQUIRE(out.table_id == 42);
    REQUIRE(out.schema_version == 3);
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
  }

  SECTION("empty columns") {
    TableMeta meta;
    meta.table_id       = 1;
    meta.schema_version = 1;
    meta.full_name      = "x";
    meta.reclength      = 10;
    meta.null_bytes     = 0;
    meta.pk_parts       = 0;
    meta.columns        = {};

    auto buf = serialize_table_meta(meta);
    TableMeta out;
    REQUIRE(deserialize_table_meta(buf.data(), buf.size(), out));
    REQUIRE(out.columns.empty());
    REQUIRE(out.table_id == 1);
  }

  SECTION("truncated buffer") {
    TableMeta meta;
    meta.table_id       = 1;
    meta.schema_version = 1;
    meta.full_name      = "db";
    meta.reclength      = 8;
    meta.null_bytes     = 0;
    meta.pk_parts       = 0;
    meta.columns        = {{3, 11, 0, 0}};

    auto buf = serialize_table_meta(meta);

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
  // Set up a stub TABLE with reclength = 8.
  TABLE_SHARE share{};
  share.reclength = 8;
  TABLE tbl{};
  tbl.s = &share;

  SECTION("encode produces correct envelope") {
    uchar row[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    auto encoded = encode_row(&tbl, row, /*schema_version=*/7);

    // 3-byte envelope + 8 bytes data.
    REQUIRE(encoded.size() == 11);
    REQUIRE(encoded[0] == 0x01);  // format version
    REQUIRE(encoded[1] == 7);     // schema_version low byte
    REQUIRE(encoded[2] == 0);     // schema_version high byte
    REQUIRE(encoded[3] == 0x10);  // first data byte
    REQUIRE(encoded[10] == 0x80); // last data byte
  }

  SECTION("schema_version > 255") {
    uchar row[8] = {};
    auto encoded = encode_row(&tbl, row, /*schema_version=*/0x0301);

    REQUIRE(encoded[1] == 0x01);  // low byte
    REQUIRE(encoded[2] == 0x03);  // high byte
  }

  SECTION("decode restores original data") {
    uchar original[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto encoded = encode_row(&tbl, original, /*schema_version=*/1);

    uchar decoded[8] = {};
    decode_row(&tbl, encoded.data(), encoded.size(), decoded);
    REQUIRE(std::memcmp(original, decoded, 8) == 0);
  }

  SECTION("decode zero-pads short payload") {
    // Simulate a stored value that's shorter than current reclength.
    // Envelope (3 bytes) + 4 bytes data = 7 bytes total, but reclength is 8.
    uint8_t short_value[] = {0x01, 0x01, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};

    uchar decoded[8];
    std::memset(decoded, 0xFF, 8);
    decode_row(&tbl, short_value, sizeof(short_value), decoded);

    REQUIRE(decoded[0] == 0xAA);
    REQUIRE(decoded[1] == 0xBB);
    REQUIRE(decoded[2] == 0xCC);
    REQUIRE(decoded[3] == 0xDD);
    // Remaining bytes zero-padded.
    REQUIRE(decoded[4] == 0);
    REQUIRE(decoded[5] == 0);
    REQUIRE(decoded[6] == 0);
    REQUIRE(decoded[7] == 0);
  }

  SECTION("decode with empty/too-short value zeros everything") {
    uchar decoded[8];
    std::memset(decoded, 0xFF, 8);
    // Only 2 bytes — less than the 3-byte envelope.
    uint8_t tiny[] = {0x01, 0x00};
    decode_row(&tbl, tiny, sizeof(tiny), decoded);

    for (int i = 0; i < 8; ++i) {
      REQUIRE(decoded[i] == 0);
    }
  }
}
