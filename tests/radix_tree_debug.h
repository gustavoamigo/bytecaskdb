// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Debug dump utility for PersistentRadixTree — produces a key-value listing
// using the public iterator API. Intended for test failure diagnostics via
// Catch2's INFO() macro.
//
// Requires: import bytecask.radix_tree; must appear BEFORE including this header.

#pragma once

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>

namespace radix_tree_debug {

template <typename V>
auto dump_tree(const bytecask::PersistentRadixTree<V> &tree) -> std::string {
  std::ostringstream oss;
  oss << "PersistentRadixTree (size=" << tree.size() << "):\n";
  for (auto it = tree.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, value] = *it;
    oss << "  key=[";
    for (std::size_t i = 0; i < key_span.size(); ++i) {
      auto c = static_cast<unsigned char>(
          std::to_integer<uint8_t>(key_span[i]));
      if (c >= 0x20 && c < 0x7F && c != '\\') {
        oss << static_cast<char>(c);
      } else {
        oss << "\\x" << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(c) << std::dec;
      }
    }
    oss << "]";
    if constexpr (std::is_integral_v<V>) {
      oss << " value=" << value;
    }
    oss << "\n";
  }
  return oss.str();
}

} // namespace radix_tree_debug
