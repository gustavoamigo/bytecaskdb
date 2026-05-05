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
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

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
