// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer harness for bytecask::data_entry::deserialize_entry.
// Feeds arbitrary bytes directly to the buffer-level parser, bypassing
// file I/O. Catches std::runtime_error (the expected rejection path for
// corrupt input) and lets any other exception or signal propagate as a
// finding.
//
// Build:  xmake f --sanitizer=fuzzer,address -m debug -y
//         xmake build fuzz_data_entry
// Run:    ./build/.../fuzz_data_entry tests/fuzz/corpus/data_entry/

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

import bytecask.data_entry;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto buf = std::as_bytes(std::span{data, size});
  try {
    auto entry = bytecask::deserialize_entry(buf);
    (void)entry;
  } catch (const std::runtime_error &) {
    // Expected: buffer too small, size mismatch, CRC mismatch.
  }
  return 0;
}
