// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_stubs.h — Minimal type stubs for unit-testing MariaDB plugin
// encoding functions without linking against MariaDB server headers.
//
// Only provides the subset of types that row_encoding.cc and key_encoding.cc
// access: TABLE, TABLE_SHARE, KEY, Field, Field_blob, uchar, uint, etc.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// MariaDB type aliases.
typedef unsigned char uchar;
typedef unsigned int uint;

#define MAX_KEY 64

// MariaDB field type enum (subset).
enum enum_field_types {
  MYSQL_TYPE_DECIMAL,
  MYSQL_TYPE_TINY,
  MYSQL_TYPE_SHORT,
  MYSQL_TYPE_LONG,
  MYSQL_TYPE_FLOAT,
  MYSQL_TYPE_DOUBLE,
  MYSQL_TYPE_NULL,
  MYSQL_TYPE_TIMESTAMP,
  MYSQL_TYPE_LONGLONG,
  MYSQL_TYPE_INT24,
  MYSQL_TYPE_DATE,
  MYSQL_TYPE_TIME,
  MYSQL_TYPE_DATETIME,
  MYSQL_TYPE_YEAR,
  MYSQL_TYPE_NEWDATE,
  MYSQL_TYPE_VARCHAR,
  MYSQL_TYPE_BIT,
  MYSQL_TYPE_NEWDECIMAL = 246,
  MYSQL_TYPE_ENUM = 247,
  MYSQL_TYPE_SET = 248,
  MYSQL_TYPE_TINY_BLOB = 249,
  MYSQL_TYPE_MEDIUM_BLOB = 250,
  MYSQL_TYPE_LONG_BLOB = 251,
  MYSQL_TYPE_BLOB = 252,
  MYSQL_TYPE_VAR_STRING = 253,
  MYSQL_TYPE_STRING = 254,
  MYSQL_TYPE_GEOMETRY = 255,
};

struct KEY {
  uint key_length;
  uint user_defined_key_parts;
};

struct TABLE_SHARE {
  uint primary_key;
  uint32_t reclength;
  uint16_t null_bytes;
  uint fields;
  uint keys;
};

struct Field {
  uchar *ptr{nullptr};

  virtual ~Field() = default;
  virtual enum_field_types type() const { return MYSQL_TYPE_LONG; }
  virtual bool is_null_in_record(const uchar *) const { return false; }
};

struct Field_blob : public Field {
  uint32_t pack_len_no_ptr_{4};

  enum_field_types type() const override { return MYSQL_TYPE_BLOB; }

  uint32_t get_length(const uchar *pos) const {
    uint32_t len = 0;
    std::memcpy(&len, pos, pack_len_no_ptr_);
    return len;
  }

  uint32_t pack_length_no_ptr() const { return pack_len_no_ptr_; }
};

struct TABLE {
  TABLE_SHARE *s;
  KEY *key_info;
  Field **field;
  uchar *record[2]{nullptr, nullptr};
};

// Transaction-related stubs for MariaDBTxn tests
struct THD {
  int dummy = 0;
};

struct handlerton {
  int dummy = 0;
};

// Stubs for key_copy / key_restore — these are MariaDB server functions.
inline void key_copy(uchar * /*dst*/, const uchar * /*src*/,
                     KEY * /*key_info*/, uint /*key_length*/) {}
inline void key_restore(uchar * /*dst*/, const uchar * /*src*/,
                        KEY * /*key_info*/, uint /*key_length*/) {}
