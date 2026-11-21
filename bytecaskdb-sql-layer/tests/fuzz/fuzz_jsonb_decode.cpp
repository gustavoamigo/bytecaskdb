// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer target: any bytes as a stored jsonb value (jsonb_properties.h,
// decode_holds). Build and run as the root xmake.lua's fuzz targets are:
//   CLANG_TARGET_TRIPLE=$(clang --print-target-triple) xmake f --sanitizer=fuzzer,address,undefined -m debug --sql_layer=y -y
//   xmake build fuzz_sql_jsonb_decode && ./build/.../fuzz_sql_jsonb_decode -max_total_time=60

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

import bytecask.sql.jsonb;

#include "bytecaskdb-sql-layer/tests/fuzz/jsonb_properties.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (!jsonb_fuzz::decode_holds(std::as_bytes(std::span{data, size}))) std::abort();
  return 0;
}
