// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer harness for bytecask::data_entry::deserialize_entry.
// Feeds arbitrary bytes directly to the buffer-level parser, bypassing
// file I/O. Catches std::runtime_error (the expected rejection path for
// corrupt input) and lets any other exception or signal propagate as a
// finding.
//
// Input: one control byte, then the entry. With bit 0 set the last four
// bytes are overwritten with the CRC of the rest, so mutations get past the
// CRC check; clear, the CRC rejection stays covered.
//
// Build:  xmake f --sanitizer=fuzzer,address -m debug -y
//         xmake build fuzz_data_entry
// Run:    scripts/run_fuzz.sh fuzz_data_entry 300

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

import bytecask.data_entry;
import bytecask.serialization;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) return 0;
  const auto control = data[0];
  auto in = std::as_bytes(std::span{data + 1, size - 1});
  std::vector<std::byte> buf{in.begin(), in.end()};
  if ((control & 1U) != 0 && buf.size() >= bytecask::kCrcSize) {
    const auto record = std::span{buf};
    bytecask::Crc32 crc{};
    crc.update(record.first(record.size() - bytecask::kCrcSize));
    bytecask::ByteWriter w{record.last(bytecask::kCrcSize)};
    w.put(crc.finalize());
  }
  try {
    auto entry = bytecask::deserialize_entry(buf);
    (void)entry;
  } catch (const std::runtime_error &) {
    // Expected: buffer too small, size mismatch, CRC mismatch.
  }
  return 0;
}
