// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Test helper: asks the kernel whether an address range is still mapped.
//
// Proving that a span handed to a reader still points into a live mapping
// cannot be done by comparing addresses: mmap(nullptr, ...) often hands back
// the address a preceding munmap just released, so an unmap-and-remap can
// look identical to never unmapping at all. Probing a range longer than the
// remapped region would be does not have that blind spot.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace bytecask::testing {

#ifndef __EMSCRIPTEN__

// True if every page overlapping [addr, addr + len) is mapped. mincore()
// fails with ENOMEM as soon as the range contains an unmapped page.
[[nodiscard]] inline auto is_mapped(const void *addr, std::size_t len) -> bool {
#ifdef __APPLE__
  using VecByte = char;
#else
  using VecByte = unsigned char;
#endif
  const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  const auto first = reinterpret_cast<std::uintptr_t>(addr);
  const auto start = first & ~(page - 1);
  const auto span = static_cast<std::size_t>(first + len - start);
  std::vector<VecByte> resident((span + page - 1) / page);
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return ::mincore(reinterpret_cast<void *>(start), span, resident.data()) == 0;
}

#endif  // __EMSCRIPTEN__

}  // namespace bytecask::testing
