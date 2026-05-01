// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_view.h — small adapters between MariaDB's `uint8_t* + size_t`
// convention and bytecask's `BytesView` (std::span<const std::byte>).
//
// These exist so the plugin can keep using raw byte pointers (which is
// what the MariaDB handler API hands out) while talking to the strongly
// typed bytecask::DB / Snapshot / WritePlan surface in include/bytecask.hpp.

#pragma once

#include "bytecask.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bytecaskdb {

inline bytecask::BytesView as_view(const uint8_t *p, std::size_t n) {
  return std::as_bytes(std::span{p, n});
}

inline bytecask::BytesView as_view(const std::vector<uint8_t> &v) {
  return std::as_bytes(std::span{v.data(), v.size()});
}

// Copy a Bytes (std::vector<std::byte>) into a std::vector<uint8_t>.
inline std::vector<uint8_t> to_uint8(const bytecask::Bytes &src) {
  std::vector<uint8_t> out(src.size());
  for (std::size_t i = 0; i < src.size(); ++i) {
    out[i] = std::to_integer<uint8_t>(src[i]);
  }
  return out;
}

inline const uint8_t *u8_data(const bytecask::Bytes &b) {
  return reinterpret_cast<const uint8_t *>(b.data());
}

inline const uint8_t *u8_data(bytecask::BytesView v) {
  return reinterpret_cast<const uint8_t *>(v.data());
}

}  // namespace bytecaskdb
