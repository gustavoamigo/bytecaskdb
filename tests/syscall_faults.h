// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Fault injection by count, below the engine: bytecask_tests is linked with
// -Wl,--wrap=<call> for each I/O call it references, the statically linked
// C++ standard library's included (xmake.lua), and the
// interposers in syscall_faults.cpp count the calls made on files under one
// directory and fail the N-th. No checkpoint names the call, so a call added
// to the engine tomorrow is counted the day it is added.
// See docs/correctness_validation.md, "Counted fault sweep".

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bytecask::testing {

// What happens at the N-th counted call. Each mode counts only the calls it
// can fail, so N indexes a different sequence in each.
enum class SyscallFault {
  // Count, fail nothing: how many calls the operation makes.
  none,
  // The call is not made and reports EIO. Counts every wrapped call.
  before,
  // The call is made and then reports EIO: the fault lands after the bytes,
  // the cut or the rename did. Counts the calls that change something: the
  // writes, the syncs, and the calls that change a size, a directory entry
  // or a file's metadata (Kind::write and Kind::change, syscall_faults.cpp).
  after,
  // The call transfers half of what was asked and returns that count.
  // Counts pread, read, pwrite, pwritev, write, writev, sendfile and
  // copy_file_range.
  short_io,
};

// One schedule of failures. `call` limits it to the calls of that name, as
// the interposers spell them ("renameat2", "unlink"); empty counts every call
// the mode counts. See ScopedSyscallFaults for the rest.
struct SyscallFaultRule {
  std::string call;
  SyscallFault mode{SyscallFault::none};
  int nth{0};
  bool cascade{false};
  int err{0};
};

struct SyscallFaultReport {
  int calls{0};       // counted by the first rule while armed
  bool fired{false};  // some rule reached its N-th call
  std::string what;   // the call that failed first, e.g. "fdatasync(/db/x.data)"
  std::vector<std::string> failed;  // every call failed, in order (first 16)
};

// Arms the interposers, process-wide: the flush leader and the hint worker
// are not always the test's thread. Counts calls on files under `dir`, by
// path or by what the descriptor names, and fails the nth (1-based). With
// `cascade`, every counted call from the nth onward fails. A failed call
// reports `err` (0: the call's usual EIO, or ENOMEM for mmap), so a test can
// reach a caller's handling of one errno. Disarms and reports on destruction
// or on report().
//
// With several rules, each counts its own calls and the first that rules a
// call failed fails it: a fallback is reached by failing one call with the
// errno that selects it, and a later call in it by a second rule.
class ScopedSyscallFaults {
public:
  ScopedSyscallFaults(const std::filesystem::path &dir, SyscallFault mode,
                      int nth, bool cascade = false, int err = 0);
  ScopedSyscallFaults(const std::filesystem::path &dir,
                      std::vector<SyscallFaultRule> rules);
  ~ScopedSyscallFaults();
  ScopedSyscallFaults(const ScopedSyscallFaults &) = delete;
  auto operator=(const ScopedSyscallFaults &) -> ScopedSyscallFaults & = delete;
  ScopedSyscallFaults(ScopedSyscallFaults &&) = delete;
  auto operator=(ScopedSyscallFaults &&) -> ScopedSyscallFaults & = delete;

  // Disarms, so nothing after it is counted or failed.
  auto report() -> SyscallFaultReport;
};

}  // namespace bytecask::testing
