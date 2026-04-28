// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer harness for bytecask::hint_entry::deserialize_entry.
// Parses the fuzz input as a sequence of hint entries, simulating
// HintFile::Scanner::next().
//
// Build:  xmake f --sanitizer=fuzzer,address -m debug -y
//         xmake build fuzz_hint_entry
// Run:    ./build/.../fuzz_hint_entry tests/fuzz/corpus/hint_entry/

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

import bytecask.hint_entry;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto buf = std::as_bytes(std::span{data, size});
  std::size_t pos = 0;
  while (pos < size) {
    try {
      auto [entry, consumed] =
          bytecask::deserialize_entry(buf.subspan(pos));
      pos += consumed;
      (void)entry;
    } catch (const std::runtime_error &) {
      break; // Truncated — expected, stop scanning.
    }
  }
  return 0;
}
