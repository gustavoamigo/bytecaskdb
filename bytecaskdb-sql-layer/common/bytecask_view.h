// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_view.h — small adapters between the `uint8_t* + size_t`
// convention and bytecask's `BytesView` (std::span<const std::byte>).
//
// They let a caller that holds raw byte pointers talk to the strongly typed
// bytecask::DB / Snapshot / WritePlan surface in include/bytecask.hpp. A
// plain C++20 header, so a caller built with another toolchain can include
// it.

#pragma once

#include "include/bytecask.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bytecask::sql {

inline bytecask::BytesView as_view(const uint8_t *p, std::size_t n) {
  return std::as_bytes(std::span{p, n});
}

inline bytecask::BytesView as_view(const std::vector<uint8_t> &v) {
  return std::as_bytes(std::span{v.data(), v.size()});
}

inline const uint8_t *u8_data(const bytecask::Bytes &b) {
  return reinterpret_cast<const uint8_t *>(b.data());
}

inline const uint8_t *u8_data(bytecask::BytesView v) {
  return reinterpret_cast<const uint8_t *>(v.data());
}

inline void assign_bytes(bytecask::Bytes &dst, const uint8_t *p, std::size_t n) {
  dst.assign(reinterpret_cast<const std::byte *>(p),
             reinterpret_cast<const std::byte *>(p + n));
}

inline void assign_bytes(bytecask::Bytes &dst, const std::vector<uint8_t> &src) {
  dst.assign(reinterpret_cast<const std::byte *>(src.data()),
             reinterpret_cast<const std::byte *>(src.data() + src.size()));
}

}  // namespace bytecask::sql
