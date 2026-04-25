// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Global allocation tracker — counts bytes allocated via operator new.
// Thread-safe (atomic), but only tracks allocations that go through the
// replaceable global operator new, not container-internal allocators.
//
// Each allocation prepends a size_t header so that both sized and unsized
// operator delete can accurately subtract freed bytes from the running total.
//
// Include this header in exactly ONE translation unit per binary (the global
// operator new/delete overrides are link-time unique).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace alloc_tracker {

inline std::atomic<std::size_t> g_allocated{0};
inline std::atomic<std::size_t> g_freed{0};

inline constexpr std::size_t kHeaderSize = alignof(std::max_align_t);

inline void reset() noexcept {
  g_allocated.store(0, std::memory_order_relaxed);
  g_freed.store(0, std::memory_order_relaxed);
}

inline auto allocated() noexcept -> std::size_t {
  return g_allocated.load(std::memory_order_relaxed);
}

inline auto freed() noexcept -> std::size_t {
  return g_freed.load(std::memory_order_relaxed);
}

inline auto net_bytes() noexcept -> std::size_t {
  return g_allocated.load(std::memory_order_relaxed) -
         g_freed.load(std::memory_order_relaxed);
}

} // namespace alloc_tracker

// Replaceable global operator new/delete.
void *operator new(std::size_t size) {
  alloc_tracker::g_allocated.fetch_add(size, std::memory_order_relaxed);
  void *raw = std::malloc(size + alloc_tracker::kHeaderSize);
  if (!raw)
    throw std::bad_alloc();
  // Store requested size in the header, return the user pointer after it.
  std::memcpy(raw, &size, sizeof(size));
  return static_cast<std::byte *>(raw) + alloc_tracker::kHeaderSize;
}

void operator delete(void *p) noexcept {
  if (!p)
    return;
  auto *raw = static_cast<std::byte *>(p) - alloc_tracker::kHeaderSize;
  std::size_t size = 0;
  std::memcpy(&size, raw, sizeof(size));
  alloc_tracker::g_freed.fetch_add(size, std::memory_order_relaxed);
  std::free(raw);
}

void operator delete(void *p, std::size_t /*size*/) noexcept {
  // Delegate to unsized delete which reads the header.
  ::operator delete(p);
}
