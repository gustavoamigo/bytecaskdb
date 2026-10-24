// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Locating the reads of a hint file, for a test that fails one of them.
//
// Requires the including translation unit to `import bytecask;` and
// `import bytecask.hint_file;` before this header (same pattern as
// fault_injector.h).

#pragma once
#ifdef BYTECASK_TESTING

#include <catch2/catch_test_macros.hpp>
#include "fault_injector.h"
#include <algorithm>
#include <filesystem>
#include <limits>
#include <vector>

namespace bytecask::testing {

// What one hint file costs in reads to open and scan, and whether an open
// verified it. The first kind of read is the pass over the whole file that
// verifies it before a scanner touches one entry; the rest are the units the
// scanner reads. A damaged file is read in a pass before it is found damaged
// — which is what lets it be discarded with nothing to undo — so its reads
// count as pass reads. A missing file costs none and is not verified. Counted
// with an injector that fails none of them.
struct HintReads {
  int open_pass;
  int scan;
  bool verified;
};

inline auto hint_reads(const std::filesystem::path &hint) -> HintReads {
  ScopedFaultInjector fi{"io_hint_read"};
  fi.inj.fail_on_nth_match = std::numeric_limits<int>::max();
  auto opened = 0;
  try {
    auto hf = HintFile::OpenForRead(hint);
    opened = fi.inj.name_matches;
    auto scanner = hf.make_scanner();
    while (scanner.next()) {
    }
  } catch (...) {
    return {fi.inj.name_matches, 0, false};
  }
  return {opened, fi.inj.name_matches - opened, true};
}

// The index (1-based) of the first read of a scan in a DB::open of dir: the
// read after those that open the first hint file of that open whose scan
// reads anything. Recovery opens and then scans one hint per data file, in
// name order, so a fault at this index lands in a scan, whatever the file set
// is. What a hint is opened with depends on how it stands in dir: once, when
// it verifies as it is; twice when recovery first has to replace it — reading
// a damaged one whole to find it damaged, or generating it when it is
// missing — and the replacement is verified in a pass of its own.
//
// The replacement is read on a copy of dir that recovery has run over once:
// the caller may have removed or damaged every hint, and that open generates
// from the same data file, with the same code, the hint this one scans. If
// the two ever differ, the fault lands in the wrong phase and the test fails
// — the phases answer a failed read differently.
inline auto first_hint_scan_read(const std::filesystem::path &dir) -> int {
  // The data files of the set an open sees, in the name order recovery
  // numbers files in — listed before any open runs, because an open adds an
  // active file that has no hint until its DB closes. The salt in a file name
  // is random, so this is not the oldest file, only the first one read.
  std::vector<std::filesystem::path> hints;
  for (const auto &e : std::filesystem::directory_iterator{dir})
    if (e.path().extension() == ".data") {
      auto hint = e.path();
      hint.replace_extension(".hint");
      hints.push_back(hint);
    }
  REQUIRE_FALSE(hints.empty());
  std::ranges::sort(hints);

  std::filesystem::path probe;
  auto rebuilt = [&](const std::filesystem::path &hint) -> HintReads {
    if (probe.empty()) {
      probe = dir.parent_path() / (dir.filename().string() + ".probe");
      std::filesystem::remove_all(probe);
      std::filesystem::copy(dir, probe,
                            std::filesystem::copy_options::recursive);
      const auto db = DB::open(probe, {.recovery_threads = 1});
    }
    // The hint recovery opens after it has replaced the one the open under
    // test replaces: the copy's, by its stem. Any other hint of the probe is
    // not that one, and the copy's own active file has no hint.
    const auto grows = hint_reads(probe / hint.filename());
    REQUIRE(grows.verified);
    return grows;
  };

  auto reads = 0;
  for (const auto &hint : hints) {
    auto work = hint_reads(hint);
    if (!work.verified) {
      const auto grows = rebuilt(hint);
      work.open_pass += grows.open_pass;
      work.scan = grows.scan;
    }
    // An empty hint holds no units: its scan reads nothing, and the first
    // scan read of the open is the one of the next hint that has units.
    if (work.scan > 0) {
      if (!probe.empty()) std::filesystem::remove_all(probe);
      return reads + work.open_pass + 1;
    }
    reads += work.open_pass;
  }
  if (!probe.empty()) std::filesystem::remove_all(probe);
  FAIL("no hint file in " << dir << " has a unit to scan");
  return 0;
}

} // namespace bytecask::testing

#endif // BYTECASK_TESTING