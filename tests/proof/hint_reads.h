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
#include "invariants.h"
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <cstring>
#include <cerrno>
#include <string>
#include <sys/stat.h>

namespace bytecask::testing {

// What one hint file costs in reads to open, and whether an open verified it.
// The first kind of read is the pass over the whole file that verifies it
// before a scanner touches one entry; a damaged file is read in such a pass
// before it is found damaged — which is what lets it be discarded with
// nothing to undo — so its reads are pass reads too. A missing file costs
// none and is not verified. Counts are taken with an injector that fails no
// read; the probe runs the same open it is counting, so its injector must not
// fail a read either.
struct HintReads {
  int open_pass;
  bool scans;    // whether the scan of the file reads at least one unit
  bool verified;
};

// Whether an error says a hint's bytes are bad or cannot be delivered — a CRC
// or parse failure, EIO, a short read — which is what open_hint_or_rebuild
// answers for with a rebuild (is_file_damage, db_rotation.cpp). An error of
// the process or the environment says nothing about the file and reaches the
// caller, as it does there.
inline auto is_hint_damage(const std::exception &e) noexcept -> bool {
  if (const auto *se = dynamic_cast<const std::system_error *>(&e))
    return se->code() == std::errc::io_error;
  return dynamic_cast<const std::runtime_error *>(&e) != nullptr;
}

inline auto hint_reads(const std::filesystem::path &hint) -> HintReads {
  if (!std::filesystem::exists(hint)) return {0, false, false};
  ScopedFaultInjector fi{"io_hint_read"};
  fi.inj.fail_on_nth_match = std::numeric_limits<int>::max();
  std::optional<HintFile> hf;
  try {
    hf = HintFile::OpenForRead(hint);
  } catch (const std::exception &e) {
    // Only the file's own damage counts as read whole and left unverified.
    // Anything else — the environment's, or anything the scan reports —
    // propagates; a scan error is not answerable with "the hint is damaged".
    if (!is_hint_damage(e)) throw;
    return {fi.inj.name_matches, false, false};
  }
  const auto opened = fi.inj.name_matches;
  auto scanner = hf->make_scanner();
  static_cast<void>(scanner.next());  // reads the first unit, if it has one
  return {opened, fi.inj.name_matches > opened, true};
}

// The hint of the first data file an open reads: files are numbered in name
// order and the first is the first read. The salt in a file name is random,
// so the first is not the oldest file. Call it before an open runs — an open
// adds an active file, and with it one more name to sort.
inline auto first_hint_of(const std::filesystem::path &dir)
    -> std::filesystem::path {
  auto data = sorted_paths(dir, ".data");
  REQUIRE_FALSE(data.empty());
  auto hint = data.front();
  hint.replace_extension(".hint");
  return hint;
}

// What says a hint file now is the file it was before an open: the inode it
// took, its size, and the time it was written — as text, so a CHECK that
// fails shows both sides. The inode alone is not identity.
// open_hint_or_rebuild removes a hint and flush_hints_for writes another one,
// and a create near a remove can take the inode the remove just freed (ext4
// does), leaving the same number to a different file. A rebuild then changes
// nothing a stat of the inode sees; the write time is what says the file was
// made again. "<absent>" is only for a file that is gone — any other
// failure to stat it is a bug in the test environment, not a missing hint, so
// it is a failure rather than a comparison of "<absent>" against a real
// identity.
inline auto hint_identity(const std::filesystem::path &hint) -> std::string {
  struct stat st{};
  if (::stat(hint.c_str(), &st) != 0) {
    if (errno != ENOENT)
      FAIL("stat failed on " << hint.string() << ": " <<
           std::strerror(errno));
    return "<absent>";
  }
  return std::format("{}-{}.{:09}-{}",
                     static_cast<std::uint64_t>(st.st_ino),
                     static_cast<std::int64_t>(st.st_mtim.tv_sec),
                     static_cast<long>(st.st_mtim.tv_nsec),
                     static_cast<std::uint64_t>(st.st_size));
}

// A sibling directory of a database, for a probe run of it. The name carries
// a timestamp so two probes in one run never collide, and the name is unique
// enough that a stale probe from a killed run cannot be reused by accident.
// Removed when it goes out of scope, however the caller leaves; a probe left
// on disk by a killed run stays (a unique name is the price of not touching
// anything that might be in use).
class ProbeDir {
public:
  explicit ProbeDir(const std::filesystem::path &base) {
    auto norm = base.lexically_normal();
    if (norm.filename().empty()) norm = norm.parent_path();  // trailing '/'
    auto ts = std::chrono::system_clock::now().time_since_epoch().count();
    path_ = norm.parent_path() /
            (norm.filename().string() + ".probe_" +
             std::to_string(ts));
  }
  ~ProbeDir() { std::filesystem::remove_all(path_); }
  ProbeDir(const ProbeDir &) = delete;
  ProbeDir &operator=(const ProbeDir &) = delete;
  ProbeDir(ProbeDir &&) = delete;
  ProbeDir &operator=(ProbeDir &&) = delete;
  [[nodiscard]] auto path() const noexcept -> const std::filesystem::path & {
    return path_;
  }

private:
  std::filesystem::path path_;
};

// The index (1-based) of the first read of a scan in a DB::open of dir with
// opts: the read after those that open the first hint file of that open whose
// scan reads anything. Recovery opens and then scans one hint per data file,
// in name order, so a fault at this index lands in a scan, whatever the file
// set is. A hint is opened once when it verifies as it stands, twice when
// recovery first replaces it — reading a damaged one whole to find it
// damaged, or generating it when it is missing — and the replacement is
// verified in a pass of its own.
//
// The replacement is read on a copy of dir that recovery has run over once:
// the caller may have removed or damaged every hint, and that open generates
// from the same data file, with the same code and the same options, the hint
// this one scans. If the two ever differ, the fault lands in the wrong phase
// and the test fails — the phases answer a failed read differently.
inline auto first_hint_scan_read(const std::filesystem::path &dir,
                                 const Options &opts) -> int {
  const auto data = sorted_paths(dir, ".data");
  REQUIRE_FALSE(data.empty());
  auto hints = data;
  for (auto &hint : hints) hint.replace_extension(".hint");
  ProbeDir probe{dir};
  auto replaced = [&](const std::filesystem::path &hint) -> HintReads {
    // The probe open runs the same open the scan cell is counting, with the
    // same opts. It runs before the caller installs the cell's fault injector
    // (which fails an io_hint_read), so it is not under the injector — that
    // is a property of the call order, not of this function. If the probe
    // open throws, REQUIRE_NOTHROW fails the test instead of unwinding past
    // an armed injector. The probe is lenient where the cell is lenient
    // (lenient cells pass .fail_recovery_on_crc_errors = false), and strict
    // otherwise, so it matches the open under test.
    HintReads grows{};
    REQUIRE_NOTHROW([&] () -> void {
      if (!std::filesystem::exists(probe.path())) {
        std::filesystem::copy(dir, probe.path(),
                              std::filesystem::copy_options::recursive);
        (void)DB::open(probe.path(), opts);
      }
      grows = hint_reads(probe.path() / hint.filename());
    }());
    REQUIRE(grows.verified);
    return grows;
  };

  auto reads = 0;
  for (const auto &hint : hints) {
    auto work = hint_reads(hint);
    if (!work.verified) {
      const auto grows = replaced(hint);
      work.open_pass += grows.open_pass;
      work.scans = grows.scans;
    }
    // An empty hint holds no units: its scan reads nothing, and the first
    // scan read of the open is the one of the next hint that has units.
    if (work.scans) return reads + work.open_pass + 1;
    reads += work.open_pass;
  }
  FAIL("no hint file in " << dir << " holds a unit to scan");
  return 0;
}

} // namespace bytecask::testing

#endif // BYTECASK_TESTING
