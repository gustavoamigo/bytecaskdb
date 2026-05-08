// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for DataFile writev failure handling.
// Any writev failure (partial write, full write + error, writev = -1)
// throws std::system_error. No in-flight recovery is attempted.

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

} // namespace

TEST_CASE("DataFile::append: B3 full write + error return — file throws",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b3_tainted.data";
  std::filesystem::remove(path);

  auto file = bytecask::WritableDataFile::openForWrite(path);
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

  auto file = bytecask::WritableDataFile::openForWrite(path);
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

  auto file = bytecask::WritableDataFile::openForWrite(path);
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

  auto file = bytecask::WritableDataFile::openForWrite(path);
  CHECK(file->size() == 0);
  CHECK(std::filesystem::exists(path));

  // Can append and read back.
  const auto key = to_bytes("k");
  const auto val = to_bytes("v");
  (void)file->append_entry(1, bytecask::EntryType::Put, key, val);
  CHECK(file->size() == bytecask::kHeaderSize + key.size() + val.size() + bytecask::kCrcSize);

  std::filesystem::remove(path);
}

TEST_CASE("WritableDataFile constructor: reopens existing file with buffer pre-populated",
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
    auto file = bytecask::WritableDataFile::openForWrite(path);
    (void)file->append_entry(1, bytecask::EntryType::Put, key, val);
    file->sync();
  }

  // Re-open with buffer capacity covering the file.
  auto file = bytecask::WritableDataFile::openForWrite(path, 4096);
  CHECK(file->size() == entry_size);

  // Buffer is pre-populated: read_entry_unverified serves from buffer.
  std::vector<std::byte> io_buf;
  auto view = file->read_entry_unverified(0, static_cast<std::uint32_t>(val.size()), io_buf);
  CHECK(view.sequence == 1);
  CHECK(view.entry_type == bytecask::EntryType::Put);
  CHECK(std::equal(view.key.begin(), view.key.end(), key.begin()));
  CHECK(std::equal(view.value.begin(), view.value.end(), val.begin()));
  // Fast path: io_buf not used.
  CHECK(io_buf.empty());

  std::filesystem::remove(path);
}

TEST_CASE("WritableDataFile constructor: throws on invalid path",
          "[data_file]") {
  REQUIRE_THROWS_AS(
      bytecask::WritableDataFile::openForWrite("/nonexistent/dir/file.data"),
      std::system_error);
}

// ---------------------------------------------------------------------------
// Move assignment operator
// ---------------------------------------------------------------------------

TEST_CASE("WritableDataFile move assignment transfers ownership",
          "[data_file]") {
  const auto path_a =
      std::filesystem::temp_directory_path() / "bc_test_move_a.data";
  const auto path_b =
      std::filesystem::temp_directory_path() / "bc_test_move_b.data";
  std::filesystem::remove(path_a);
  std::filesystem::remove(path_b);

  auto a = bytecask::WritableDataFile::openForWrite(path_a);
  auto b = bytecask::WritableDataFile::openForWrite(path_b);

  const auto key = to_bytes("mk");
  const auto val = to_bytes("mv");
  (void)a->append_entry(1, bytecask::EntryType::Put, key, val);
  const auto expected_size = a->size();

  // Move-assign a into b.
  *b = std::move(*a);

  CHECK(b->path() == path_a);
  CHECK(b->size() == expected_size);

  // b can still write after receiving a's state.
  (void)b->append_entry(2, bytecask::EntryType::Put, key, val);
  CHECK(b->size() == expected_size * 2);

  // a's destructor runs without double-close (fd neutralized to -1).
  std::filesystem::remove(path_a);
  std::filesystem::remove(path_b);
}

// ---------------------------------------------------------------------------
// read_entry_unverified — WritableDataFile
// ---------------------------------------------------------------------------

TEST_CASE("WritableDataFile::read_entry_unverified buffer fast path",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_unverified_buf.data";
  std::filesystem::remove(path);

  auto file = bytecask::WritableDataFile::openForWrite(path, 4096);
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
  CHECK(io_buf.empty());

  std::filesystem::remove(path);
}

TEST_CASE("WritableDataFile::read_entry_unverified pread fallback",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_unverified_pread.data";
  std::filesystem::remove(path);

  // capacity=0 means no buffer — forces pread path.
  auto file = bytecask::WritableDataFile::openForWrite(path);
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
    auto w = bytecask::WritableDataFile::openForWrite(path);
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
    auto w = bytecask::WritableDataFile::openForWrite(path);
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
    auto w = bytecask::WritableDataFile::openForWrite(path);
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
