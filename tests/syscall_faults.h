// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Fault injection by count, below the engine: bytecask_tests is linked with
// -Wl,--wrap=<call> for each I/O call the engine makes (xmake.lua), and the
// interposers in syscall_faults.cpp count the calls made on files under one
// directory and fail the N-th. No checkpoint names the call, so a call added
// to the engine tomorrow is counted the day it is added.
// See docs/correctness_validation.md, "Counted fault sweep".

#pragma once

#include <filesystem>
#include <string>

namespace bytecask::testing {

// What happens at the N-th counted call. Each mode counts only the calls it
// can fail, so N indexes a different sequence in each.
enum class SyscallFault {
  // Count, fail nothing: how many calls the operation makes.
  none,
  // The call is not made and reports EIO. Counts every wrapped call.
  before,
  // The call is made and then reports EIO: the fault lands after the bytes,
  // the cut or the rename did. Counts the calls that change something:
  // pwrite, pwritev, write, fdatasync, fsync, ftruncate, renameat2, link,
  // unlink.
  after,
  // The call transfers half of what was asked and returns that count.
  // Counts pread, pwrite, pwritev and write.
  short_io,
};

struct SyscallFaultReport {
  int calls{0};       // counted while armed
  bool fired{false};  // the N-th call was reached
  std::string what;   // the call that failed first, e.g. "fdatasync(/db/x.data)"
};

// Arms the interposers, process-wide: the flush leader and the hint worker
// are not always the test's thread. Counts calls on files under `dir`, by
// path or by what the descriptor names, and fails the nth (1-based). With
// `cascade`, every counted call from the nth onward fails. A failed call
// reports `err` (0: the call's usual EIO, or ENOMEM for mmap), so a test can
// reach a caller's handling of one errno. Disarms and reports on destruction
// or on report().
class ScopedSyscallFaults {
public:
  ScopedSyscallFaults(const std::filesystem::path &dir, SyscallFault mode,
                      int nth, bool cascade = false, int err = 0);
  ~ScopedSyscallFaults();
  ScopedSyscallFaults(const ScopedSyscallFaults &) = delete;
  auto operator=(const ScopedSyscallFaults &) -> ScopedSyscallFaults & = delete;
  ScopedSyscallFaults(ScopedSyscallFaults &&) = delete;
  auto operator=(ScopedSyscallFaults &&) -> ScopedSyscallFaults & = delete;

  // Disarms, so nothing after it is counted or failed.
  auto report() -> SyscallFaultReport;
};

}  // namespace bytecask::testing
