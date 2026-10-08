// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Allocation failure by count (#364): alloc_faults.cpp replaces the global
// operator new in bytecask_tests, counts the allocations made while armed and
// fails the N-th with std::bad_alloc (nullptr for the nothrow forms). The
// allocation-failure sweep (alloc_sweep_test.cpp) runs each operation once per
// N. See docs/correctness_validation.md, "Counted allocation-failure sweep".
//
// The replacement is compiled only where a sanitizer runtime does not define
// operator new itself (ThreadSanitizer and MemorySanitizer do, and the link
// would fail). kAllocFaultsAvailable says whether it is in the binary.

#pragma once

namespace bytecask::testing {

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define BYTECASK_NO_ALLOC_FAULTS 1
#endif
#endif
#ifdef __EMSCRIPTEN__
#define BYTECASK_NO_ALLOC_FAULTS 1
#endif

#ifdef BYTECASK_NO_ALLOC_FAULTS
inline constexpr bool kAllocFaultsAvailable = false;
#else
inline constexpr bool kAllocFaultsAvailable = true;
#endif

struct AllocFaultReport {
  long allocations{0};  // counted while armed
  bool fired{false};    // the N-th was reached and failed
};

// Arms the count, process-wide: the committer thread and the hint worker
// allocate on an operation's behalf. Fails the nth counted allocation
// (1-based; 0 counts without failing); with `cascade`, every one after it
// too. A thread holding SuspendSyscallFaults (fault_injector.h) is neither
// counted nor failed: test instrumentation, and test code that builds an
// operation's inputs, must not be. Disarms on destruction or on report().
class ScopedAllocFaults {
public:
  ScopedAllocFaults(long nth, bool cascade);
  ~ScopedAllocFaults();
  ScopedAllocFaults(const ScopedAllocFaults &) = delete;
  auto operator=(const ScopedAllocFaults &) -> ScopedAllocFaults & = delete;
  ScopedAllocFaults(ScopedAllocFaults &&) = delete;
  auto operator=(ScopedAllocFaults &&) -> ScopedAllocFaults & = delete;

  auto report() -> AllocFaultReport;
};

}  // namespace bytecask::testing
