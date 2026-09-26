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
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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
#include "mapping_probe.h"

import bytecask.data_file;
import bytecask.data_entry;
import bytecask.types;
import bytecask.buffer_pool;
import bytecask.counters;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(std::span<const std::byte> b) -> std::string {
  return {reinterpret_cast<const char *>(b.data()), b.size()};
}

// The entry at offset and the offset past it, read the way every sweep reads.
auto scan_at(const bytecask::DataFile &file, bytecask::Offset offset)
    -> std::optional<std::pair<bytecask::DataEntry, bytecask::Offset>> {
  bytecask::DataFileIterator it{file, offset};
  if (it == std::default_sentinel) return std::nullopt;
  return std::pair{(*it).first, it.next_offset()};
}

// Skips the calling test where dies_by_panic cannot run: WASM has no fork(),
// and a panic there ends the whole instance. Call it first thing in the test
// body; a SKIP from inside a CHECK expression is reported as a failure.
void skip_without_fork() {
#ifdef __EMSCRIPTEN__
  SKIP("panic() cannot be observed without fork()");
#endif
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

  auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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

  auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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

  auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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
  auto r0 = scan_at(*file, offsets[0]);
  REQUIRE(r0.has_value());
  CHECK(r0->first.sequence == 1);
  CHECK(r0->first.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(r0->first.key.begin(), r0->first.key.end(), k0.begin()));
  CHECK(std::equal(r0->first.value.begin(), r0->first.value.end(), v0.begin()));

  auto r1 = scan_at(*file, offsets[1]);
  REQUIRE(r1.has_value());
  CHECK(r1->first.sequence == 2);
  CHECK(std::equal(r1->first.key.begin(), r1->first.key.end(), k1.begin()));
  CHECK(std::equal(r1->first.value.begin(), r1->first.value.end(), v1.begin()));

  auto r2 = scan_at(*file, offsets[2]);
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

  auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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
    auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
    (void)file->append_entry(1, bytecask::EntryType::Put, key, val);
    file->sync();
  }

  // Native builds request mmap; Emscripten always uses pread.
  auto file = bytecask::openDataFileForWrite(path, 4096, bytecask::IoBackend::Mmap);
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
      bytecask::openDataFileForWrite("/nonexistent/dir/file.data", 0, bytecask::IoBackend::Pread),
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

  auto file = bytecask::openDataFileForWrite(path, 4096, bytecask::IoBackend::Mmap);
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
  // BufferPool is included to pin the documented invariant that it changes
  // nothing about the write path: the writable file it builds is a distinct
  // implementation, and it must zero-fill and seal exactly as Pread's does.
  const auto io_backend =
      GENERATE(bytecask::IoBackend::Pread, bytecask::IoBackend::Mmap,
               bytecask::IoBackend::BufferPool);
  CAPTURE(static_cast<int>(io_backend));
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_zero_fill.data";
  std::filesystem::remove(path);

  constexpr std::size_t kCapacity = 8 * 1024 * 1024;
  // Required by the BufferPool back-end, ignored by the other two.
  auto pool = std::make_shared<bytecask::BufferPool>(
      bytecask::BufferPoolOptions{.capacity_bytes = 4 * kCapacity});
  auto file = bytecask::createDataFileForWrite(
      std::filesystem::temp_directory_path(), "bc_test_zero_fill", ".data",
      kCapacity, io_backend, pool, /*file_id=*/1);
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
  CHECK(!scan_at(*file, 0).has_value());
  (void)file->append_entry(1, bytecask::EntryType::Put, to_bytes("k"),
                           to_bytes("v"));
  auto first = scan_at(*file, 0);
  REQUIRE(first.has_value());
  CHECK(first->first.sequence == 1);
  CHECK(!scan_at(*file, first->second).has_value());

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
  auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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

  auto file = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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
    auto w = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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
    auto w = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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
    auto w = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
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
  auto file = bytecask::openDataFileForWrite(path, 16, bytecask::IoBackend::Mmap);
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
  auto file = bytecask::openDataFileForWrite(path, 16, bytecask::IoBackend::Mmap);
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
  auto file = bytecask::openDataFileForWrite(path, 16, bytecask::IoBackend::Mmap);
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
// WritableMmapDataFile::truncate — mapping stability
//
// resume() truncates the active file while reads stay lock-free, so a reader
// can be holding a span into the mapping at that moment (EntryIterator hands
// spans out until the next operator++). Unmapping and remapping underneath it
// would leave that span addressing memory the process no longer owns.
// ---------------------------------------------------------------------------

#ifndef __EMSCRIPTEN__
TEST_CASE("WritableMmapDataFile::truncate leaves the mapping in place",
          "[data_file][mmap]") {
  const auto path = std::filesystem::temp_directory_path() /
                    "bc_test_mmap_truncate_mapping.data";
  std::filesystem::remove(path);

  constexpr std::size_t kCapacity = 1024 * 1024;
  auto file = bytecask::openDataFileForWrite(path, kCapacity,
                                             bytecask::IoBackend::Mmap);
  const auto key = to_bytes("tk");
  const auto val = to_bytes("truncate_must_not_move_this");
  const auto kept = file->append_entry(1, bytecask::EntryType::Put, key, val);
  const auto valid_end = file->size();
  // The garbage tail a resume() would drop.
  const auto garbage_key = to_bytes("gk");
  const auto garbage_val = to_bytes("garbage");
  const auto dropped = file->append_entry(2, bytecask::EntryType::Put,
                                          garbage_key, garbage_val);

  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(
      kept, static_cast<std::uint32_t>(val.size()), io_buf);
  REQUIRE(io_buf.empty());  // the span points into the mapping, not io_buf
  const auto *addr = view.value.data();

  file->truncate(valid_end);
  CHECK(std::filesystem::file_size(path) == valid_end);

  // The span taken before the truncate still addresses live memory holding
  // the same bytes. Probing half the capacity is what makes this conclusive:
  // a remap of the truncated file covers only the surviving entry, so it
  // fails the probe even when mmap hands back the address just released.
  CHECK(bytecask::testing::is_mapped(addr, kCapacity / 2));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));

  // A fresh read of the same offset resolves to the same address.
  auto again = file->read_entry_unverified(
      kept, static_cast<std::uint32_t>(val.size()), io_buf);
  CHECK(again.value.data() == addr);
  CHECK(io_buf.empty());

  // Past the new end the file is gone: reads take the pread path and fail as
  // a short read rather than faulting on a mapped page beyond EOF.
  CHECK_THROWS_AS(
      file->read_entry_unverified(
          dropped, static_cast<std::uint32_t>(garbage_val.size()), io_buf),
      std::system_error);
  std::vector<std::byte> out;
  CHECK_THROWS_AS(
      file->read_value(dropped, static_cast<std::uint16_t>(garbage_key.size()),
                       static_cast<std::uint32_t>(garbage_val.size()), false,
                       io_buf, out),
      std::system_error);

  file.reset();
  std::filesystem::remove(path);
}
#endif

// ---------------------------------------------------------------------------
// ReadOnlyMmapDataFile::scan — truncated file handling
// ---------------------------------------------------------------------------

TEST_CASE("Sweep over ReadOnlyMmapDataFile ends at truncated header",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_scan_trunc_hdr.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("scankey");
  const auto val = to_bytes("scanval");
  {
    auto w = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
    (void)w->append_entry(1, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  // Truncate to less than kHeaderSize bytes — scan at offset 0 should return nullopt.
  std::filesystem::resize_file(path, bytecask::kHeaderSize - 1);

  auto file = bytecask::ReadOnlyMmapDataFile::openForRead(path);
  auto result = scan_at(*file, 0);
  CHECK(!result.has_value());

  std::filesystem::remove(path);
}

TEST_CASE("Sweep over ReadOnlyMmapDataFile ends at truncated entry body",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_mmap_scan_trunc_body.data";
  std::filesystem::remove(path);

  const auto key = to_bytes("bkey");
  const auto val = to_bytes("bodyval");
  const auto full_size =
      bytecask::kHeaderSize + key.size() + val.size() + bytecask::kCrcSize;
  {
    auto w = bytecask::openDataFileForWrite(path, 0, bytecask::IoBackend::Pread);
    (void)w->append_entry(2, bytecask::EntryType::Put, key, val);
    w->sync();
  }

  // Truncate mid-entry: header is valid but body is incomplete.
  std::filesystem::resize_file(path, full_size - 2);

  auto file = bytecask::ReadOnlyMmapDataFile::openForRead(path);
  auto result = scan_at(*file, 0);
  CHECK(!result.has_value());

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// DataFileIterator — chunked sweep (#146)
//
// The iterator reads kChunkBytes at a time and frames entries out of that
// buffer. These cover what the chunking adds: entries that straddle a chunk
// boundary, an entry larger than a whole chunk, the zero-filled tail of an
// active file, a truncated last entry, and a CRC failure in the middle —
// on every back-end, active and sealed.
// ---------------------------------------------------------------------------
TEST_CASE("DataFileIterator sweeps across chunk boundaries", "[data_file][iterator]") {
  const auto io_backend =
      GENERATE(bytecask::IoBackend::Pread, bytecask::IoBackend::Mmap,
               bytecask::IoBackend::BufferPool);
  const auto sealed = GENERATE(false, true);
  CAPTURE(static_cast<int>(io_backend), sealed);

  const auto dir =
      std::filesystem::temp_directory_path() / "bc_test_iter_chunks";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "chunks.data";

  constexpr std::size_t kCapacity = 8 * 1024 * 1024;
  constexpr auto kChunk = bytecask::DataFileIterator::kChunkBytes;
  auto pool = std::make_shared<bytecask::BufferPool>(
      bytecask::BufferPoolOptions{.capacity_bytes = 4 * kCapacity});

  struct Expected {
    std::uint64_t seq;
    std::string key;
    std::string value;
    bytecask::Offset offset;
  };
  std::vector<Expected> expected;
  std::shared_ptr<bytecask::DataFile> file;
  {
    auto w = bytecask::createDataFileForWrite(dir, "chunks", ".data",
                                              kCapacity, io_backend, pool,
                                              /*file_id=*/1);
    std::uint64_t seq = 1;
    auto append = [&](std::string value) {
      auto key = std::format("key{:06d}", seq);
      const auto off = w->append_entry(seq, bytecask::EntryType::Put,
                                       to_bytes(key), to_bytes(value));
      expected.push_back({seq, std::move(key), std::move(value), off});
      ++seq;
    };
    // Odd sizes, so entries land across chunk boundaries at arbitrary
    // offsets; one value larger than a whole chunk in the middle.
    while (w->size() < kChunk + kChunk / 2)
      append(std::string((seq * 7919) % 5000 + 1, static_cast<char>('a' + seq % 26)));
    append(std::string(kChunk + kChunk / 2, 'L'));
    while (w->size() < 3 * kChunk + kChunk / 3)
      append(std::string((seq * 7919) % 5000 + 1, static_cast<char>('a' + seq % 26)));
    w->sync();
    if (sealed) {
      w->shrink_to_fit();
      w.reset();
      file = bytecask::openDataFileForRead(path, io_backend, pool, 1);
    } else {
      file = std::move(w);  // zero-filled tail past the last entry
    }
  }

  auto check_prefix = [&](const bytecask::DataFile &f, std::size_t n) {
    std::size_t i = 0;
    for (const auto &[entry, off] : bytecask::scan_entries(f)) {
      REQUIRE(i < n);
      const auto &e = expected[i];
      CHECK(entry.sequence == e.seq);
      CHECK(off == e.offset);
      CHECK(to_string(entry.key) == e.key);
      CHECK(entry.value.size() == e.value.size());
      CHECK(to_string(entry.value) == e.value);
      ++i;
    }
    CHECK(i == n);
  };

  SECTION("every entry, in order, at its offset") {
    check_prefix(*file, expected.size());
  }

  if (sealed) {
    SECTION("a truncated last entry ends the sweep before it") {
      file.reset();
      std::filesystem::resize_file(path,
                                   std::filesystem::file_size(path) - 3);
      file = bytecask::openDataFileForRead(path, io_backend, pool, 1);
      check_prefix(*file, expected.size() - 1);
    }

    SECTION("a CRC failure past the first chunk throws") {
      file.reset();
      const auto &victim = expected[expected.size() - 2];
      {
        std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
        f.seekp(static_cast<std::streamoff>(victim.offset +
                                            bytecask::kHeaderSize));
        f.put('!');
      }
      file = bytecask::openDataFileForRead(path, io_backend, pool, 1);
      CHECK_THROWS_AS(
          [&] {
            for (const auto &e : bytecask::scan_entries(*file)) (void)e;
          }(),
          std::runtime_error);
    }
  }

  file.reset();
  std::filesystem::remove_all(dir);
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
  skip_without_fork();
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_stem_data";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = "data_20260912164544_deadbeef_V01";

  // A sealed file from an earlier rotation, with content past the threshold.
  {
    auto sealed = bytecask::openDataFileForWrite(
        dir / (stem + ".data"), 0, bytecask::IoBackend::Pread);
    (void)sealed->append_entry(1, bytecask::EntryType::Put, to_bytes("k"),
                               to_bytes("v"));
    sealed->sync();
  }
  REQUIRE(std::filesystem::file_size(dir / (stem + ".data")) > 0);

  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data", 0, bytecask::IoBackend::Pread);
  }));

  // mmap-backed files take a separate open() path — guard both.
  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data", 4096, bytecask::IoBackend::Mmap);
  }));

  std::filesystem::remove_all(dir);
}

TEST_CASE("createDataFileForWrite panics when the stem was already hinted",
          "[data_file][panic]") {
  skip_without_fork();
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_stem_hint";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = "data_20260912164544_cafebabe_V01";

  // The data file is gone (vacuumed), but its hint marks the stem as sealed.
  { std::ofstream hint{dir / (stem + ".hint")}; }
  REQUIRE(std::filesystem::exists(dir / (stem + ".hint")));
  REQUIRE_FALSE(std::filesystem::exists(dir / (stem + ".data")));

  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data", 0, bytecask::IoBackend::Pread);
  }));

  // Vacuum stages under .data.tmp; a hinted stem is off limits there too.
  CHECK(dies_by_panic([&] {
    (void)bytecask::createDataFileForWrite(dir, stem, ".data.tmp", 0, bytecask::IoBackend::Pread);
  }));

  std::filesystem::remove_all(dir);
}

TEST_CASE("createDataFileForWrite accepts an unused stem", "[data_file]") {
  const auto dir = std::filesystem::temp_directory_path() / "bc_test_stem_ok";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = "data_20260912164544_00000001_V01";

  auto file = bytecask::createDataFileForWrite(dir, stem, ".data", 0, bytecask::IoBackend::Pread);
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
  skip_without_fork();
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

// ---------------------------------------------------------------------------
// lend_record — every back-end, writable and sealed
// ---------------------------------------------------------------------------

// Records of every shape the speculative first read can get wrong: a short
// one, a key past the 256-byte key budget, a value past the page, and one
// starting near the end of a page. Each is read with no size hint, with the
// right one and with a wrong one, verified and not; the sizes always come
// from the record's own header.
TEST_CASE("DataFile::lend_record reads any record with or without a hint",
          "[data_file]") {
  const auto io_backend =
      GENERATE(bytecask::IoBackend::Pread, bytecask::IoBackend::Mmap,
               bytecask::IoBackend::BufferPool);
  const bool sealed = GENERATE(false, true);
  CAPTURE(static_cast<int>(io_backend), sealed);
  const auto dir = std::filesystem::temp_directory_path();
  const auto path = dir / "bc_test_lend_record.data";
  std::filesystem::remove(path);

  struct Rec {
    std::string key;
    std::string value;
  };
  const std::vector<Rec> recs{
      {"k", "v"},
      {std::string(300, 'K'), "long key"},
      {"long value", std::string(10'000, 'V')},
      {"after", std::string(4'000, 'x')},  // pushes the next one near a page end
      {"near page end", std::string(90, 'y')},
      {"", ""},
  };

  constexpr std::size_t kCapacity = 1 << 20;
  auto pool = std::make_shared<bytecask::BufferPool>(
      bytecask::BufferPoolOptions{.capacity_bytes = 8 * kCapacity});
  auto writer = bytecask::createDataFileForWrite(
      dir, "bc_test_lend_record", ".data", kCapacity, io_backend, pool,
      /*file_id=*/7);
  std::vector<bytecask::Offset> offsets;
  for (std::size_t i = 0; i < recs.size(); ++i)
    offsets.push_back(writer->append_entry(i + 1, bytecask::EntryType::Put,
                                           to_bytes(recs[i].key),
                                           to_bytes(recs[i].value)));
  writer->sync();
  std::shared_ptr<bytecask::DataFile> file = writer;
  if (sealed) {
    writer->shrink_to_fit();
    writer.reset();
    file.reset();
    file = bytecask::openDataFileForRead(path, io_backend, pool, /*file_id=*/8);
  }

  std::vector<std::byte> io_buf;
  bytecask::FrameLease lease;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    const auto size = static_cast<std::uint32_t>(recs[i].value.size());
    for (const std::uint32_t hint : {0u, size, size + 1'000u, 1u}) {
      for (const bool verify : {true, false}) {
        CAPTURE(i, hint, verify);
        const auto view =
            file->lend_record(offsets[i], hint, verify, io_buf, lease);
        CHECK(view.sequence == i + 1);
        CHECK(view.entry_type == bytecask::EntryType::Put);
        CHECK(std::ranges::equal(view.key, to_bytes(recs[i].key)));
        CHECK(std::ranges::equal(view.value, to_bytes(recs[i].value)));
      }
    }
  }
  lease.reset();
  file.reset();
  std::filesystem::remove(path);
}

TEST_CASE("DataFile::lend_record rejects a damaged record when verifying",
          "[data_file]") {
  const auto io_backend =
      GENERATE(bytecask::IoBackend::Pread, bytecask::IoBackend::Mmap,
               bytecask::IoBackend::BufferPool);
  CAPTURE(static_cast<int>(io_backend));
  const auto dir = std::filesystem::temp_directory_path();
  const auto path = dir / "bc_test_lend_record_crc.data";
  std::filesystem::remove(path);
  auto pool = std::make_shared<bytecask::BufferPool>(
      bytecask::BufferPoolOptions{.capacity_bytes = 8 << 20});
  {
    auto writer = bytecask::createDataFileForWrite(
        dir, "bc_test_lend_record_crc", ".data", 1 << 20,
        bytecask::IoBackend::Pread);
    (void)writer->append_entry(1, bytecask::EntryType::Put, to_bytes("key"),
                               to_bytes("value"));
    writer->sync();
    writer->shrink_to_fit();
  }
  {
    std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(static_cast<std::streamoff>(bytecask::kHeaderSize + 1));  // "k[e]y"
    f.put('E');
  }
  auto file = bytecask::openDataFileForRead(path, io_backend, pool, 9);
  std::vector<std::byte> io_buf;
  bytecask::FrameLease lease;
  CHECK_THROWS_AS(file->lend_record(0, 0, true, io_buf, lease),
                  std::runtime_error);
  const auto view = file->lend_record(0, 0, false, io_buf, lease);
  CHECK(view.key.size() == 3);
  lease.reset();
  file.reset();
  std::filesystem::remove(path);
}

namespace {

// A record placed so a buffer-pool frame boundary falls inside it: `before`
// is how many of its bytes come before the boundary.
struct Straddler {
  const char *part;
  std::string key;
  std::string value;
  std::size_t before;
};

// Writes each straddler across its own frame boundary, with a filler record
// before it to put it there. Returns each straddler's offset.
auto write_straddlers(bytecask::WritableDataFile &writer,
                      const std::vector<Straddler> &recs)
    -> std::vector<bytecask::Offset> {
  constexpr std::size_t kFrame = bytecask::kPoolFrameBytes;
  constexpr std::size_t kOverhead = bytecask::kHeaderSize + bytecask::kCrcSize;
  std::vector<bytecask::Offset> offsets;
  std::uint64_t seq = 1;
  std::size_t cur = 0;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    const auto boundary = (4 * i + 1) * kFrame;
    const auto start = boundary - recs[i].before;
    const auto filler = start - cur - kOverhead - 1;  // key "f"
    (void)writer.append_entry(seq++, bytecask::EntryType::Put, to_bytes("f"),
                              to_bytes(std::string(filler, '.')));
    offsets.push_back(writer.append_entry(seq++, bytecask::EntryType::Put,
                                          to_bytes(recs[i].key),
                                          to_bytes(recs[i].value)));
    REQUIRE(offsets.back() == start);
    cur = start + kOverhead + recs[i].key.size() + recs[i].value.size();
  }
  return offsets;
}

auto straddlers() -> std::vector<Straddler> {
  constexpr std::size_t kFrame = bytecask::kPoolFrameBytes;
  constexpr std::size_t kH = bytecask::kHeaderSize;
  return {
      {"header", "hkey", std::string(100, 'h'), 7},
      {"key", std::string(40, 'k'), std::string(100, 'v'), kH + 20},
      {"value", "vkey", std::string(300, 'w'), kH + 4 + 150},
      {"crc", "ckey", std::string(50, 'c'), kH + 4 + 50 + 2},
      {"several frames", "big", std::string(2 * kFrame + 100, 'b'), 30},
  };
}

}  // namespace

// A record or value that crosses a buffer-pool frame boundary — in its
// header, key, value or CRC, or across several frames — reads the same through
// lend_record and read_value, verified or not, from a writable file (resident
// as it was written) and a sealed one (resident after the first pass).
TEST_CASE("DataFile: records across buffer pool frames read whole",
          "[data_file][buffer_pool]") {
  const bool sealed = GENERATE(false, true);
  CAPTURE(sealed);
  const auto dir = std::filesystem::temp_directory_path();
  const auto path = dir / "bc_test_straddle.data";
  std::filesystem::remove(path);
  auto pool = std::make_shared<bytecask::BufferPool>(
      bytecask::BufferPoolOptions{.capacity_bytes = 8 << 20});
  const auto recs = straddlers();
  auto writer = bytecask::createDataFileForWrite(
      dir, "bc_test_straddle", ".data", 1 << 20,
      bytecask::IoBackend::BufferPool, pool, /*file_id=*/21);
  const auto offsets = write_straddlers(*writer, recs);
  writer->sync();
  std::shared_ptr<bytecask::DataFile> file = writer;
  if (sealed) {
    writer->shrink_to_fit();
    writer.reset();
    file.reset();
    file = bytecask::openDataFileForRead(path, bytecask::IoBackend::BufferPool,
                                         pool, /*file_id=*/22);
  }

  std::vector<std::byte> io_buf;
  bytecask::FrameLease lease;
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t i = 0; i < recs.size(); ++i) {
      for (const bool verify : {true, false}) {
        CAPTURE(pass, recs[i].part, verify);
        const auto view = file->lend_record(offsets[i], 0, verify, io_buf, lease);
        CHECK(std::ranges::equal(view.key, to_bytes(recs[i].key)));
        CHECK(std::ranges::equal(view.value, to_bytes(recs[i].value)));
        lease.reset();
        std::vector<std::byte> out(3, std::byte{0x5A});  // stale contents
        file->read_value(offsets[i],
                         static_cast<std::uint16_t>(recs[i].key.size()),
                         static_cast<std::uint32_t>(recs[i].value.size()),
                         verify, io_buf, out);
        CHECK(std::ranges::equal(out, to_bytes(recs[i].value)));
      }
    }
  }
  file.reset();
  std::filesystem::remove(path);
}

// Damage after the frame boundary is caught by the CRC on the copy that
// joins the two frames, through lend_record and read_value alike, cold and
// with the pool warm.
TEST_CASE("DataFile: a damaged record across pool frames fails verification",
          "[data_file][buffer_pool]") {
  const auto dir = std::filesystem::temp_directory_path();
  const auto path = dir / "bc_test_straddle_crc.data";
  std::filesystem::remove(path);
  const std::vector<Straddler> recs{
      {"value", "vkey", std::string(300, 'w'), bytecask::kHeaderSize + 4 + 150}};
  bytecask::Offset offset = 0;
  {
    auto writer = bytecask::createDataFileForWrite(
        dir, "bc_test_straddle_crc", ".data", 1 << 20,
        bytecask::IoBackend::Pread);
    offset = write_straddlers(*writer, recs)[0];
    writer->sync();
    writer->shrink_to_fit();
  }
  {
    // A value byte 10 bytes past the frame boundary.
    std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(static_cast<std::streamoff>(bytecask::kPoolFrameBytes + 10));
    f.put('X');
  }
  auto pool = std::make_shared<bytecask::BufferPool>(
      bytecask::BufferPoolOptions{.capacity_bytes = 8 << 20});
  auto file = bytecask::openDataFileForRead(
      path, bytecask::IoBackend::BufferPool, pool, /*file_id=*/23);
  std::vector<std::byte> io_buf;
  std::vector<std::byte> out;
  bytecask::FrameLease lease;
  for (int pass = 0; pass < 2; ++pass) {
    CAPTURE(pass);
    CHECK_THROWS_AS(file->lend_record(offset, 0, true, io_buf, lease),
                    std::runtime_error);
    lease.reset();
    CHECK_THROWS_AS(file->read_value(offset, 4, 300, true, io_buf, out),
                    std::runtime_error);
    const auto view = file->lend_record(offset, 0, false, io_buf, lease);
    CHECK(view.value.size() == 300);
    lease.reset();
  }
  file.reset();
  std::filesystem::remove(path);
}
