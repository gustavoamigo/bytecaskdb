// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Fault injection infrastructure for correctness validation.
// Compiled only when BYTECASK_TESTING is defined.
// Never included directly — guarded by #ifdef BYTECASK_TESTING
// at every call site.

#pragma once

#ifdef BYTECASK_TESTING

#include <string>
#include <system_error>

namespace bytecask::testing {

// ---------------------------------------------------------------------------
// FaultInjector
//
// Controls when and where a fault is injected. Two injection modes:
//
//   By name  — fail at the first checkpoint matching fail_at_name
//   By count — fail when call_count exceeds fail_at
//
// Name-based injection takes priority if both are set.
//
// Thread-local: each thread has its own active injector so concurrent
// tests do not interfere with each other.
// ---------------------------------------------------------------------------
struct FaultInjector {
  std::string fail_at_name;    // fail at this named checkpoint
  int         fail_at    = -1; // fail after this many checkpoints (-1 = never)
  int         call_count = 0;  // number of checkpoints passed so far
  std::string last_checkpoint; // name of the last checkpoint that fired

  std::error_code error = std::make_error_code(std::errc::io_error);

  void checkpoint(const char* name) {
    last_checkpoint = name;
    ++call_count;

    // Name-based — targets a specific fault point explicitly
    if (!fail_at_name.empty() && fail_at_name == name) {
      throw std::system_error{error,
          std::string{"fault injection at: "} + name};
    }

    // Count-based — targets the Nth checkpoint in sequence
    if (fail_at >= 0 && call_count > fail_at) {
      throw std::system_error{error,
          std::string{"fault injection at: "} + name};
    }
  }
};

// Thread-local active injector.
// Set to non-null before a test scenario, reset to null after.
// Use ScopedFaultInjector to ensure cleanup even if the test throws.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunique-object-duplication"
inline thread_local FaultInjector* active_injector = nullptr;
#pragma clang diagnostic pop

// Called at every IO boundary via the FAULT_INJECTION macro.
inline void io_checkpoint(const char* name) {
  if (active_injector) active_injector->checkpoint(name);
}

// ---------------------------------------------------------------------------
// ScopedFaultInjector
//
// RAII guard that activates an injector and resets active_injector
// on destruction — even if the test body throws.
// ---------------------------------------------------------------------------
struct ScopedFaultInjector {
  FaultInjector inj;

  // Name-based injection
  explicit ScopedFaultInjector(std::string name) {
    inj.fail_at_name = std::move(name);
    active_injector = &inj;
  }

  // Count-based injection
  explicit ScopedFaultInjector(int fail_at) {
    inj.fail_at = fail_at;
    active_injector = &inj;
  }

  ~ScopedFaultInjector() {
    active_injector = nullptr;
  }

  ScopedFaultInjector(const ScopedFaultInjector&) = delete;
  ScopedFaultInjector& operator=(const ScopedFaultInjector&) = delete;
  ScopedFaultInjector(ScopedFaultInjector&&) = delete;
  ScopedFaultInjector& operator=(ScopedFaultInjector&&) = delete;
};

} // namespace bytecask::testing

// The macro — only defined when BYTECASK_TESTING is set.
// Not defined in release builds — FAULT_INJECTION does not exist.
#define FAULT_INJECTION(name) \
    ::bytecask::testing::io_checkpoint(#name)

#endif // BYTECASK_TESTING
