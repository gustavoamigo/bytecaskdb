// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for DataFile read-back recovery (BC-156)

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

// ---------------------------------------------------------------------------
// BC-156: try_recover_failed_append
//
// B3 scenario: writev succeeds (full entry on disk), then throws before
// offset_ advances. The engine must read back and CRC-verify the entry
// instead of immediately poisoning.
// ---------------------------------------------------------------------------

TEST_CASE("DataFile::try_recover_failed_append: B3 valid CRC — returns offset "
          "and advances file",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b3_valid.data";
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

  CHECK(file.is_tainted());
  CHECK(file.size() == 0); // offset_ not advanced — append threw before updating it

  const auto recovered = file.try_recover_failed_append(
      static_cast<std::uint16_t>(key.size()),
      static_cast<std::uint32_t>(val.size()));

  REQUIRE(recovered.has_value());
  CHECK(*recovered == 0);         // entry sits at the start of the file
  CHECK(file.size() > 0);         // offset_ advanced past the entry
  CHECK_FALSE(file.is_tainted()); // tainted cleared — file is consistent again

  std::filesystem::remove(path);
}

TEST_CASE("DataFile::try_recover_failed_append: B2 truncated entry — returns "
          "nullopt, file stays tainted",
          "[data_file]") {
  const auto path =
      std::filesystem::temp_directory_path() / "bc_test_b2_invalid.data";
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

  const auto recovered = file.try_recover_failed_append(
      static_cast<std::uint16_t>(key.size()),
      static_cast<std::uint32_t>(val.size()));

  CHECK_FALSE(recovered.has_value()); // CRC invalid → cannot recover
  CHECK(file.is_tainted());           // still tainted — caller must poison

  std::filesystem::remove(path);
}
