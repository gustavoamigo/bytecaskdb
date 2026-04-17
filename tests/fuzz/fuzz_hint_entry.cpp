// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer harness for bytecask::hint_entry::deserialize_entry.
// Parses the fuzz input as a sequence of hint entries, simulating
// HintFile::Scanner::next(). The loop is important: prefix_len
// references the previous entry's key_buf, so sequential parsing is
// where prefix compression bugs surface.
//
// Build:  xmake f --sanitizer=fuzzer,address -m debug -y
//         xmake build fuzz_hint_entry
// Run:    ./build/.../fuzz_hint_entry tests/fuzz/corpus/hint_entry/

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

import bytecask.hint_entry;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto buf = std::as_bytes(std::span{data, size});
  std::vector<std::byte> key_buf;
  std::vector<std::byte> end_key_buf;
  std::size_t pos = 0;
  while (pos < size) {
    try {
      auto [entry, consumed] =
          bytecask::deserialize_entry(buf.subspan(pos), key_buf, end_key_buf);
      pos += consumed;
      (void)entry;
    } catch (const std::runtime_error &) {
      break; // Truncated — expected, stop scanning.
    }
  }
  return 0;
}
