// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Fault injection infrastructure for correctness validation.
// Compiled only when BYTECASK_TESTING is defined.
// Never included directly — guarded by #ifdef BYTECASK_TESTING
// at every call site.

#pragma once

#ifdef BYTECASK_TESTING

#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <unistd.h>

namespace bytecask::testing {

// ---------------------------------------------------------------------------
// PostWriteMode
//
// Controls what happens at a post-write checkpoint (after writev succeeds).
//
//   none        — no injection (default)
//   short_write — ftruncate to simulate partial writev, then throw
//   throw_after — throw without truncating (full entry on disk)
// ---------------------------------------------------------------------------
enum class PostWriteMode { none, short_write, throw_after };

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
// Post-write mode (short_write/throw_after) fires at FAULT_INJECTION_POST_WRITE
// checkpoints matching fail_at_name. It does not use count-based triggering.
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

  PostWriteMode post_write_mode{PostWriteMode::none};
  ssize_t       short_write_bytes{0};   // bytes to keep for short_write mode

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

// Called at post-write IO boundaries via FAULT_INJECTION_POST_WRITE.
// Does NOT increment call_count — the pre-write checkpoint already did.
// Fires only when post_write_mode != none AND fail_at_name matches.
inline void io_post_write_checkpoint(const char* name, int fd,
                                      std::uint64_t offset,
                                      std::size_t /*total*/) {
  if (!active_injector) return;
  auto& inj = *active_injector;
  inj.last_checkpoint = name;

  if (inj.post_write_mode == PostWriteMode::none) return;
  if (inj.fail_at_name != name) return;

  if (inj.post_write_mode == PostWriteMode::short_write) {
    // Truncate to simulate partial writev return. The actual writev
    // already succeeded; we rewrite history to look like a short write.
    (void)::ftruncate(fd, static_cast<off_t>(offset + static_cast<std::uint64_t>(inj.short_write_bytes)));
  }
  // For throw_after: data stays fully on disk — simulates writev
  // success followed by a failure before offset_ advances.
  throw std::system_error{inj.error,
      std::string{"fault injection (post-write) at: "} + name};
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

  // Name-based with post-write mode
  ScopedFaultInjector(std::string name, PostWriteMode mode,
                      ssize_t short_bytes = 0) {
    inj.fail_at_name = std::move(name);
    inj.post_write_mode = mode;
    inj.short_write_bytes = short_bytes;
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

#define FAULT_INJECTION_POST_WRITE(name, fd, offset, total) \
    ::bytecask::testing::io_post_write_checkpoint(#name, fd, offset, total)

#endif // BYTECASK_TESTING
