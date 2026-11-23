// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer target: any text through jsonb_in's parser and the encoder
// (jsonb_properties.h, parse_holds). Built as fuzz_sql_jsonb_decode is.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

import bytecask.sql.jsonb;

#include "bytecaskdb-sql-layer/tests/fuzz/jsonb_properties.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (!jsonb_fuzz::parse_holds(std::string_view{reinterpret_cast<const char*>(data), size})) std::abort();
  return 0;
}
