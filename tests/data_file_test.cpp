// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for DataFile writev failure handling.
// Any writev failure (partial write, full write + error, writev = -1)
// marks the file as tainted. No in-flight recovery is attempted.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
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

TEST_CASE("DataFile::append: B3 full write + error return — file is tainted",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b3_tainted.data";
  std::filesystem::remove(path);

  bytecask::DataFile file{path};
  const auto key = to_bytes("hello");
  const auto val = to_bytes("world");

  {
    using PW = bytecask::testing::PostWriteMode;
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_append_partial",
                                              PW::throw_after};
    REQUIRE_THROWS_AS(file.append(1, bytecask::EntryType::Put, key, val),
                      std::system_error);
  }

  // Full entry on disk but writev reported an error — file must be tainted.
  // No silent recovery is attempted; the engine degrades and resume() handles it.
  CHECK(file.is_tainted());
  CHECK(file.size() == 0); // offset_ not advanced — append threw before updating it

  std::filesystem::remove(path);
}

TEST_CASE("DataFile::append: B2 partial write — file is tainted",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b2_tainted.data";
  std::filesystem::remove(path);

  bytecask::DataFile file{path};
  const auto key = to_bytes("hello");
  const auto val = to_bytes("world");

  {
    using PW = bytecask::testing::PostWriteMode;
    bytecask::testing::ScopedFaultInjector fi{"io_data_file_append_partial",
                                              PW::short_write, 5};
    REQUIRE_THROWS_AS(file.append(1, bytecask::EntryType::Put, key, val),
                      std::system_error);
  }

  CHECK(file.is_tainted());
  CHECK(file.size() == 0);

  std::filesystem::remove(path);
}
