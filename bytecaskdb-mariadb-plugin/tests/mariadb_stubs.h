// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_stubs.h — Minimal type stubs for unit-testing MariaDB plugin
// encoding functions without linking against MariaDB server headers.
//
// Only provides the subset of types that row_encoding.cc and key_encoding.cc
// access: TABLE, TABLE_SHARE, KEY, uchar, uint, key_copy/key_restore stubs.

#pragma once

#include <cstdint>
#include <cstddef>

// MariaDB type aliases.
typedef unsigned char uchar;
typedef unsigned int uint;

#define MAX_KEY 64

struct KEY {
  uint key_length;
  uint user_defined_key_parts;
};

struct TABLE_SHARE {
  uint primary_key;
  uint32_t reclength;
  uint16_t null_bytes;
  uint fields;
  uint keys;  // Add keys field for unit tests
};

struct Field;

struct TABLE {
  TABLE_SHARE *s;
  KEY *key_info;
  Field **field;
};

// Stubs for key_copy / key_restore — these are MariaDB server functions.
// The unit tests only exercise functions that don't call these, but the
// linker needs symbols for the translation units that reference them.
inline void key_copy(uchar * /*dst*/, const uchar * /*src*/,
                     KEY * /*key_info*/, uint /*key_length*/) {}
inline void key_restore(uchar * /*dst*/, const uchar * /*src*/,
                        KEY * /*key_info*/, uint /*key_length*/) {}
