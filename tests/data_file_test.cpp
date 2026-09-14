// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for DataFile writev failure handling.
// Any writev failure (partial write, full write + error, writev = -1)
// throws std::system_error. No in-flight recovery is attempted.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#include <tuple>
#include <unistd.h>
#include <vector>

#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif

import bytecask.data_file;
import bytecask.data_entry;
import bytecask.types;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

// Runs fn in a forked child and reports whether it died on SIGABRT.
// panic() aborts by design — it must not be catchable — so proving it fires
// needs a separate process rather than a REQUIRE_THROWS. The child's stderr is
// silenced so the expected panic message does not look like a test failure.
auto dies_by_panic(const std::function<void()> &fn) -> bool {
  const auto pid = ::fork();
  if (pid == 0) {
    // Catch2 traps SIGABRT and finishes the run from its handler, writing its
    // report on the way out. In a forked child that report lands in the same
    // --out file the parent will write, leaving two XML documents in it. Take
    // the handler back so abort() terminates the child immediately, and leave
    // the child no other route into Catch2's reporting.
    std::signal(SIGABRT, SIG_DFL);
    // A child forked from a multi-threaded parent can deadlock on a lock held
    // at fork time; fail the check instead of hanging CI.
    ::alarm(30);
    std::ignore = std::freopen("/dev/null", "w", stderr);
    try {
      fn();
    } catch (...) {
      ::_exit(2);  // threw instead of panicking
    }
    ::_exit(0);  // returned: no panic
  }
  REQUIRE(pid != -1);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

} // namespace

TEST_CASE("DataFile::append: B3 full write + error return — file throws",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b3_tainted.data";
  std::filesystem::remove(path);

  auto file = bytecask::openDataFileForWrite(path, 0, false);
  const auto key = to_bytes("hello");
  const auto val = to_bytes("world");

  {
    using PW = bytecask::testing::PostWriteMode;
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_append_partial",
                                              PW::throw_after};
    REQUIRE_THROWS_AS(file->append_entry(1, bytecask::EntryType::Put, key, val),
                      std::system_error);
  }

  // Full entry on disk but writev reported an error — append threw.
  // No silent recovery is attempted; the engine degrades and resume() handles it.
  CHECK(file->size() == 0); // offset_ not advanced — append threw before updating it

  std::filesystem::remove(path);
}

TEST_CASE("DataFile::append: B2 partial write — file throws",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b2_tainted.data";
  std::filesystem::remove(path);

  auto file = bytecask::openDataFileForWrite(path, 0, false);
  const auto key = to_bytes("hello");
  const auto val = to_bytes("world");

  {
    using PW = bytecask::testing::PostWriteMode;
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_append_partial",
                                              PW::short_write, 5};
    REQUIRE_THROWS_AS(file->append_entry(1, bytecask::EntryType::Put, key, val),
                      std::system_error);
  }

  CHECK(file->size() == 0);

  std::filesystem::remove(path);
}

TEST_CASE("DataFile::append_entries batches multiple entries into one writev",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_append_entries.data";
  std::filesystem::remove(path);

  auto file = bytecask::openDataFileForWrite(path, 0, false);
  const auto k0 = to_bytes("key0");
  const auto v0 = to_bytes("val0");
  const auto k1 = to_bytes("key1");
  const auto v1 = to_bytes("val1");
  const auto k2 = to_bytes("k2");

  const std::array<bytecask::DataEntryView, 3> entries{{
      {1, bytecask::EntryType::Put, k0, v0},
      {2, bytecask::EntryType::Put, k1, v1},
      {3, bytecask::EntryType::Delete, k2, {}},
  }};
  std::array<bytecask::Offset, 3> offsets{};
  file->append_entries(entries, offsets);
  file->sync();

  // Offsets must be sequential and start at 0.
  CHECK(offsets[0] == 0);
  const auto sz0 = bytecask::kHeaderSize + k0.size() + v0.size() + bytecask::kCrcSize;
  CHECK(offsets[1] == sz0);
  const auto sz1 = bytecask::kHeaderSize + k1.size() + v1.size() + bytecask::kCrcSize;
  CHECK(offsets[2] == sz0 + sz1);

  // Round-trip: scan each entry and verify contents.
  auto r0 = file->scan(offsets[0]);
  REQUIRE(r0.has_value());
  CHECK(r0->first.sequence == 1);
  CHECK(r0->first.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(r0->first.key.begin(), r0->first.key.end(), k0.begin()));
  CHECK(std::equal(r0->first.value.begin(), r0->first.value.end(), v0.begin()));

  auto r1 = file->scan(offsets[1]);
  REQUIRE(r1.has_value());
  CHECK(r1->first.sequence == 2);
  CHECK(std::equal(r1->first.key.begin(), r1->first.key.end(), k1.begin()));
  CHECK(std::equal(r1->first.value.begin(), r1->first.value.end(), v1.begin()));

  auto r2 = file->scan(offsets[2]);
  REQUIRE(r2.has_value());
  CHECK(r2->first.sequence == 3);
  CHECK(r2->first.entry_type == bytecask::EntryType::Delete);
  CHECK(std::equal(r2->first.key.begin(), r2->first.key.end(), k2.begin()));
  CHECK(r2->first.value.empty());

  // file.size() must equal total bytes written.
  const auto sz2 = bytecask::kHeaderSize + k2.size() + bytecask::kCrcSize;
  CHECK(file->size() == sz0 + sz1 + sz2);

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// WritableDataFile constructor tests
// ---------------------------------------------------------------------------

TEST_CASE("WritableDataFile constructor: fresh file with no buffer",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_ctor_no_buf.data";
  std::filesystem::remove(path);

  auto file = bytecask::openDataFileForWrite(path, 0, false);
  CHECK(file->size() == 0);
  CHECK(std::filesystem::exists(path));

  // Can append and read back.
  const auto key = to_bytes("k");
  const auto val = to_bytes("v");
  (void)file->append_entry(1, bytecask::EntryType::Put, key, val);
  CHECK(file->size() == bytecask::kHeaderSize + key.size() + val.size() + bytecask::kCrcSize);

  std::filesystem::remove(path);
}

TEST_CASE("WritableDataFile constructor: reopens existing file",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_ctor_reopen_buf.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("hello");
  const auto val = to_bytes("world");
  const auto entry_size =
      bytecask::kHeaderSize + key.size() + val.size() + bytecask::kCrcSize;

  // Write one entry and close.
  {
    auto file = bytecask::openDataFileForWrite(path, 0, false);
    (void)file->append_entry(1, bytecask::EntryType::Put, key, val);
    file->sync();
  }

  // Native builds request mmap; Emscripten always uses pread.
  auto file = bytecask::openDataFileForWrite(path, 4096, true);
  CHECK(file->size() == entry_size);

  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(0, static_cast<std::uint32_t>(val.size()), io_buf);
  CHECK(view.sequence == 1);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  // mmap lends a file-backed view; Emscripten's pread fallback owns it in io_buf.
#ifdef __EMSCRIPTEN__
  CHECK(!io_buf.empty());
#else
  CHECK(io_buf.empty());
#endif

  std::filesystem::remove(path);
}

TEST_CASE("WritableDataFile constructor: throws on invalid path",
          "[data_file]") {
  REQUIRE_THROWS_AS(
      bytecask::openDataFileForWrite("/nonexistent/dir/file.data", 0, false),
      std::system_error);
}

// ---------------------------------------------------------------------------
// read_entry_unverified — WritableDataFile
// ---------------------------------------------------------------------------

TEST_CASE("WritableDataFile::read_entry_unverified with mmap request",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_unverified_buf.data";
  std::filesystem::remove(path);

  auto file = bytecask::openDataFileForWrite(path, 4096, true);
  const auto key = to_bytes("bufkey");
  const auto val = to_bytes("bufval");
  (void)file->append_entry(42, bytecask::EntryType::Put, key, val);

  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 42);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
#ifdef __EMSCRIPTEN__
  CHECK(!io_buf.empty());
#else
  CHECK(io_buf.empty());
#endif

  std::filesystem::remove(path);
}

#ifdef __linux__
// Counts the file's extents and how many are still unwritten (allocated by
// fallocate but never written). Returns nullopt where FIEMAP is unsupported
// (tmpfs, overlayfs) so the test can skip rather than fail.
struct ExtentCount { std::size_t total; std::size_t unwritten; };
auto count_extents(const std::filesystem::path &path)
    -> std::optional<ExtentCount> {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) return std::nullopt;
  constexpr std::size_t kMaxExtents = 256;
  std::vector<std::byte> buf(sizeof(fiemap) +
                             kMaxExtents * sizeof(fiemap_extent));
  auto *fm = reinterpret_cast<fiemap *>(buf.data());
  fm->fm_start = 0;
  fm->fm_length = FIEMAP_MAX_OFFSET;
  fm->fm_flags = FIEMAP_FLAG_SYNC;
  fm->fm_extent_count = kMaxExtents;
  const int rc = ::ioctl(fd, FS_IOC_FIEMAP, fm);
  ::close(fd);
  if (rc != 0) return std::nullopt;
  ExtentCount out{fm->fm_mapped_extents, 0};
  for (std::size_t i = 0; i < fm->fm_mapped_extents; ++i) {
    if (fm->fm_extents[i].fe_flags & FIEMAP_EXTENT_UNWRITTEN) ++out.unwritten;
  }
  return out;
}

// A fresh active file's first chunk must have every extent *written*, not
// merely allocated: an unwritten extent is converted on first write, and that
// conversion is journaled metadata every fdatasync then waits for. Holds for
// both file types, and shrink_to_fit must give the tail back.
TEST_CASE("WritableDataFile: fresh file has no unwritten extents",
          "[data_file]") {
  const bool use_mmap = GENERATE(false, true);
  CAPTURE(use_mmap);
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_zero_fill.data";
  std::filesystem::remove(path);

  constexpr std::size_t kCapacity = 8 * 1024 * 1024;
  auto file = bytecask::createDataFileForWrite(
      std::filesystem::temp_directory_path(), "bc_test_zero_fill", ".data",
      kCapacity, use_mmap);
  CHECK(file->size() == 0);
  // Zero-filled one chunk ahead, not to capacity.
  CHECK(std::filesystem::file_size(path) == bytecask::kZeroFillChunkBytes);

  if (const auto ext = count_extents(path)) {
    CHECK(ext->total > 0);
    CHECK(ext->unwritten == 0);
  } else {
    WARN("FIEMAP unsupported on this filesystem — extent check skipped");
  }

  // The zero tail must still read as end-of-data.
  CHECK(!file->scan(0).has_value());
  (void)file->append_entry(1, bytecask::EntryType::Put, to_bytes("k"),
                           to_bytes("v"));
  auto first = file->scan(0);
  REQUIRE(first.has_value());
  CHECK(first->first.sequence == 1);
  CHECK(!file->scan(first->second).has_value());

  // Sealing gives the tail back: physical size becomes the logical size.
  file->shrink_to_fit();
  CHECK(std::filesystem::file_size(path) == file->size());
  CHECK(file->size() == first->second);

  file.reset();
  std::filesystem::remove(path);
}
#endif

TEST_CASE("WritableDataFile::read_entry_unverified pread fallback",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_unverified_pread.data";
  std::filesystem::remove(path);

  // capacity=0 means no buffer — forces pread path.
  auto file = bytecask::openDataFileForWrite(path, 0, false);
  const auto key = to_bytes("pkey");
  const auto val = to_bytes("pval");
  (void)file->append_entry(7, bytecask::EntryType::Put, key, val);

  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 7);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  // pread path fills io_buf.
  CHECK(!io_buf.empty());

  std::filesystem::remove(path);
}

TEST_CASE("WritablePosixDataFile::read_entry_unverified long key triggers retry",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_posix_wr_longkey.data";
  std::filesystem::remove(path);

  // Key > 256 bytes exceeds kKeyBudget, forcing the second pread.
  const std::string long_key_str(300, 'L');
  const auto key = to_bytes(long_key_str);
  const auto val = to_bytes("lv");

  auto file = bytecask::openDataFileForWrite(path, 0, false);
  (void)file->append_entry(42, bytecask::EntryType::Put, key, val);

  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 42);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(view.key.size() == 300);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  CHECK(!io_buf.empty());

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// read_entry_unverified — ReadOnlyPosixDataFile
// ---------------------------------------------------------------------------

TEST_CASE("ReadOnlyPosixDataFile::read_entry_unverified short key",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_posix_unverified.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("shortkey");
  const auto val = to_bytes("shortval");
  {
    auto w = bytecask::openDataFileForWrite(path, 0, false);
    (void)w->append_entry(10, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  auto file = bytecask::ReadOnlyPosixDataFile::openForRead(path);
  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 10);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));

  std::filesystem::remove(path);
}

TEST_CASE("ReadOnlyPosixDataFile::read_entry_unverified long key triggers retry",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_posix_longkey.data";
  std::filesystem::remove(path);

  // Key > 256 bytes to exceed kKeyBudget and trigger the second pread.
  const std::string long_key_str(300, 'K');
  const auto key = to_bytes(long_key_str);
  const auto val = to_bytes("lv");
  {
    auto w = bytecask::openDataFileForWrite(path, 0, false);
    (void)w->append_entry(99, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  auto file = bytecask::ReadOnlyPosixDataFile::openForRead(path);
  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 99);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(view.key.size() == 300);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// read_entry_unverified — ReadOnlyMmapDataFile
// ---------------------------------------------------------------------------

TEST_CASE("ReadOnlyMmapDataFile::read_entry_unverified",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_unverified.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("mmapkey");
  const auto val = to_bytes("mmapval");
  {
    auto w = bytecask::openDataFileForWrite(path, 0, false);
    (void)w->append_entry(55, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  auto file = bytecask::ReadOnlyMmapDataFile::openForRead(path);
  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 55);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  // mmap path does not use io_buf.
  CHECK(io_buf.empty());

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// WritableMmapDataFile — pread fallback (entry beyond mmap region)
// ---------------------------------------------------------------------------

TEST_CASE("WritableMmapDataFile: read_header pread fallback beyond mmap",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_hdr_fallback.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("k");
  const auto val = to_bytes("v");

  // capacity=16 — far too small for any entry, so all reads fall through to pread.
  auto file = bytecask::openDataFileForWrite(path, 16, true);
  (void)file->append_entry(10, bytecask::EntryType::Put, key, val);

  // read_entry uses read_header internally — if header is beyond mmap, it uses pread.
  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 10);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  // pread fallback uses io_buf.
  CHECK(!io_buf.empty());

  std::filesystem::remove(path);
}

TEST_CASE("WritableMmapDataFile: read_value pread fallback beyond mmap",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_rv_fallback.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("rk");
  const auto val = to_bytes("read_value_test_payload");

  // capacity=16 — entry written beyond mmap region.
  auto file = bytecask::openDataFileForWrite(path, 16, true);
  (void)file->append_entry(20, bytecask::EntryType::Put, key, val);

  // read_value with verify=false exercises the pread fallback in read_value.
  std::vector<std::byte> io_buf;
  std::vector<std::byte> out;
  file->read_value(0, static_cast<std::uint16_t>(key.size()),
                   static_cast<std::uint32_t>(val.size()), false, io_buf, out);

  CHECK(std::equal(out.begin(), out.end(), val.begin()));

  std::filesystem::remove(path);
}

TEST_CASE("WritableMmapDataFile: read_entry_with_key_size pread fallback",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_entry_fallback.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("ekey");
  const auto val = to_bytes("eval");

  // capacity=16 — forces pread path for read_entry (verified).
  auto file = bytecask::openDataFileForWrite(path, 16, true);
  (void)file->append_entry(30, bytecask::EntryType::Put, key, val);

  std::vector<std::byte> io_buf;
  auto view = file->read_entry(0, static_cast<std::uint32_t>(val.size()), io_buf);

  CHECK(view.sequence == 30);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  CHECK(!io_buf.empty());

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// ReadOnlyMmapDataFile::scan — truncated file handling
// ---------------------------------------------------------------------------

TEST_CASE("ReadOnlyMmapDataFile::scan returns nullopt on truncated header",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_scan_trunc_hdr.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("scankey");
  const auto val = to_bytes("scanval");
  {
    auto w = bytecask::openDataFileForWrite(path, 0, false);
    (void)w->append_entry(1, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  // Truncate to less than kHeaderSize bytes — scan at offset 0 should return nullopt.
  std::filesystem::resize_file(path, bytecask::kHeaderSize - 1);

  auto file = bytecask::ReadOnlyMmapDataFile::openForRead(path);
  auto result = file->scan(0);
  CHECK(!result.has_value());

  std::filesystem::remove(path);
}

TEST_CASE("ReadOnlyMmapDataFile::scan returns nullopt on truncated entry body",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_scan_trunc_body.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("bkey");
  const auto val = to_bytes("bodyval");
  const auto full_size =
      bytecask::kHeaderSize + key.size() + val.size() + bytecask::kCrcSize;
  {
    auto w = bytecask::openDataFileForWrite(path, 0, false);
    (void)w->append_entry(2, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  // Truncate mid-entry: header is valid but body is incomplete.
  std::filesystem::resize_file(path, full_size - 2);

  auto file = bytecask::ReadOnlyMmapDataFile::openForRead(path);
  auto result = file->scan(0);
  CHECK(!result.has_value());

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// createDataFileForWrite — a stem is never reused
//
// make_data_file_stem names files <UTC second>_<32-bit salt>. The salt is a
// birthday collision away from repeating within a directory, and a rotation
// threshold of a few bytes mints thousands of files per second. Reusing a stem
// used to open the sealed file for write and adopt its length: the assert in
// execute_slots caught it in debug, and in release the appended entries were
// lost at recovery, because hint generation skips a file that already has one.
// Both halves of the name must now be refused outright.
// ---------------------------------------------------------------------------

TEST_CASE("createDataFileForWrite panics when the data file already exists",
          "[data_file][panic]") {
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_stem_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = "data_20260912164544_deadbeef_V01";

  // A sealed file from an earlier rotation, with content past the threshold.
  {
    auto sealed = bytecask::openDataFileForWrite(dir / (stem + ".data"), 0,
                                                 false);
    (void)sealed->append_entry(1, bytecask::EntryType::Put, to_bytes("k"),
                               to_bytes("v"));
    sealed->sync();
  }
  REQUIRE(std::filesystem::file_size(dir / (stem + ".data")) > 0);

  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data", 0, false);
  }));

  // mmap-backed files take a separate open() path — guard both.
  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data", 4096, true);
  }));

  std::filesystem::remove_all(dir);
}

TEST_CASE("createDataFileForWrite panics when the stem was already hinted",
          "[data_file][panic]") {
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_stem_hint";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = "data_20260912164544_cafebabe_V01";

  // The data file is gone (vacuumed), but its hint marks the stem as sealed.
  { std::ofstream hint{dir / (stem + ".hint")}; }
  REQUIRE(std::filesystem::exists(dir / (stem + ".hint")));
  REQUIRE_FALSE(std::filesystem::exists(dir / (stem + ".data")));

  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data", 0, false);
  }));

  // Vacuum stages under .data.tmp; a hinted stem is off limits there too.
  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data.tmp", 0, false);
  }));

  std::filesystem::remove_all(dir);
}

TEST_CASE("createDataFileForWrite accepts an unused stem", "[data_file]") {
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_stem_ok";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = "data_20260912164544_00000001_V01";

  auto file = bytecask::createDataFileForWrite(dir, stem, ".data", 0, false);
  CHECK(file->size() == 0);
  CHECK(file->path() == dir / (stem + ".data"));
  CHECK(std::filesystem::exists(dir / (stem + ".data")));

  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// renameDataFileExclusive — final placement claims the name atomically
//
// Vacuum stages its compacted copy under .data.tmp holding only vacuum_mu_,
// so a rotation can mint the same stem before the copy finishes. Checking the
// target and then renaming loses that race; refusing inside the placement
// itself does not.
// ---------------------------------------------------------------------------

TEST_CASE("renameDataFileExclusive places a staged file", "[data_file]") {
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_place_ok";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto from = dir / "staged.data.tmp";
  const auto to = dir / "staged.data";
  { std::ofstream f{from}; f << "compacted"; }

  bytecask::renameDataFileExclusive(from, to);

  CHECK(std::filesystem::exists(to));
  CHECK_FALSE(std::filesystem::exists(from));  // staged copy is consumed
  CHECK(std::filesystem::file_size(to) == 9);

  std::filesystem::remove_all(dir);
}

TEST_CASE("renameDataFileExclusive panics rather than replacing a live file",
          "[data_file][panic]") {
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_place_bad";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto from = dir / "staged.data.tmp";
  const auto to = dir / "staged.data";
  { std::ofstream f{from}; f << "compacted"; }
  { std::ofstream f{to}; f << "live data that must survive"; }

  CHECK(dies_by_panic([&] { bytecask::renameDataFileExclusive(from, to); }));

  // The panicking child must not have touched either file.
  CHECK(std::filesystem::file_size(to) == 27);
  CHECK(std::filesystem::exists(from));

  std::filesystem::remove_all(dir);
}
