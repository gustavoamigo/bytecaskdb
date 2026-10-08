// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// The replaceable global operator new and delete behind alloc_faults.h. Every
// form is replaced, so none of the standard library's is linked, and each
// allocates with malloc: under AddressSanitizer that is its interceptor, so
// leaks and overflows are still reported (new/delete mismatches are not).

#include "alloc_faults.h"

#include "fault_injector.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

#ifndef BYTECASK_NO_ALLOC_FAULTS

namespace {

std::atomic<bool> g_armed{false};
std::atomic<long> g_nth{0};
std::atomic<bool> g_cascade{false};
std::atomic<long> g_count{0};
std::atomic<bool> g_fired{false};
// BYTECASK_ALLOC_SWEEP_TRACE: print the stack of each allocation failed.
std::atomic<bool> g_trace{false};

void print_stack() noexcept {
#if defined(__linux__)
  void *frames[48];
  const auto n = ::backtrace(frames, 48);
  ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif
}

// Whether this allocation is to fail. Lock-free and allocation-free: it runs
// inside operator new, on every thread.
auto should_fail() noexcept -> bool {
  if (!g_armed.load(std::memory_order_acquire)) return false;
  if (bytecask::testing::syscall_faults_suspended > 0) return false;
  const auto n = g_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto nth = g_nth.load(std::memory_order_relaxed);
  if (nth == 0) return false;
  if (n == nth || (n > nth && g_cascade.load(std::memory_order_relaxed))) {
    if (n == nth && g_trace.load(std::memory_order_relaxed)) print_stack();
    g_fired.store(true, std::memory_order_relaxed);
    return true;
  }
  return false;
}

auto allocate(std::size_t size) noexcept -> void * {
  if (should_fail()) return nullptr;
  return std::malloc(size == 0 ? 1 : size);
}

auto allocate(std::size_t size, std::align_val_t align) noexcept -> void * {
  if (should_fail()) return nullptr;
  void *p = nullptr;
  const auto a = static_cast<std::size_t>(align);
  if (::posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a,
                       size == 0 ? 1 : size) != 0)
    return nullptr;
  return p;
}

}  // namespace

namespace bytecask::testing {

ScopedAllocFaults::ScopedAllocFaults(long nth, bool cascade) {
  g_count.store(0, std::memory_order_relaxed);
  g_fired.store(false, std::memory_order_relaxed);
  g_nth.store(nth, std::memory_order_relaxed);
  g_cascade.store(cascade, std::memory_order_relaxed);
  const char *trace = std::getenv("BYTECASK_ALLOC_SWEEP_TRACE");
  g_trace.store(trace != nullptr && *trace != '\0' && *trace != '0',
                std::memory_order_relaxed);
#if defined(__linux__)
  if (g_trace.load(std::memory_order_relaxed)) {
    // backtrace() loads the unwinder on first use; not under the count.
    void *warm[1];
    (void)::backtrace(warm, 1);
  }
#endif
  g_armed.store(true, std::memory_order_release);
}

ScopedAllocFaults::~ScopedAllocFaults() {
  g_armed.store(false, std::memory_order_release);
}

auto ScopedAllocFaults::report() -> AllocFaultReport {
  g_armed.store(false, std::memory_order_release);
  return {.allocations = g_count.load(std::memory_order_relaxed),
          .fired = g_fired.load(std::memory_order_relaxed)};
}

}  // namespace bytecask::testing

// ---- Replacements ---------------------------------------------------------

void *operator new(std::size_t size) {
  if (auto *p = allocate(size)) return p;
  throw std::bad_alloc{};
}
void *operator new[](std::size_t size) {
  if (auto *p = allocate(size)) return p;
  throw std::bad_alloc{};
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  return allocate(size);
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return allocate(size);
}
void *operator new(std::size_t size, std::align_val_t align) {
  if (auto *p = allocate(size, align)) return p;
  throw std::bad_alloc{};
}
void *operator new[](std::size_t size, std::align_val_t align) {
  if (auto *p = allocate(size, align)) return p;
  throw std::bad_alloc{};
}
void *operator new(std::size_t size, std::align_val_t align,
                   const std::nothrow_t &) noexcept {
  return allocate(size, align);
}
void *operator new[](std::size_t size, std::align_val_t align,
                     const std::nothrow_t &) noexcept {
  return allocate(size, align);
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept {
  std::free(p);
}
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  std::free(p);
}
void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  std::free(p);
}

#endif  // BYTECASK_NO_ALLOC_FAULTS
