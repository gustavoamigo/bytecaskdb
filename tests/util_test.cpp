// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for bytecask.util.

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <format>
#include <sys/types.h>
#include <string>
#include <system_error>
#include <vector>
import bytecask.util;

namespace {

auto check_write_error(ssize_t written, std::size_t wanted)
    -> std::system_error {
  try {
    bytecask::check_write(written, wanted, "test write");
  } catch (const std::system_error &e) {
    return e;
  }
  FAIL("check_write did not throw");
  return std::system_error{std::error_code{}};
}

} // namespace

TEST_CASE("check_write: a full write passes", "[util]") {
  CHECK_NOTHROW(bytecask::check_write(10, 10, "test write"));
  CHECK_NOTHROW(bytecask::check_write(0, 0, "test write"));
}

// A failed call reports its own errno.
TEST_CASE("check_write: -1 reports errno", "[util]") {
  errno = ENOSPC;
  const auto e = check_write_error(-1, 10);
  CHECK(e.code() == std::errc::no_space_on_device);
  CHECK(std::string{e.what()}.starts_with("test write: write failed"));
}

// A short count sets no errno, so whatever an earlier call left there is
// not what is reported (#221).
TEST_CASE("check_write: a short count is EIO with the byte counts",
          "[util]") {
  errno = EAGAIN;
  for (const ssize_t written : {0, 4}) {
    CAPTURE(written);
    const auto e = check_write_error(written, 10);
    CHECK(e.code() == std::errc::io_error);
    CHECK(std::string{e.what()}.starts_with(
        std::format("test write: short write: the file took {} of 10 bytes",
                    written)));
  }
}

// #384: a scratch buffer left past its limit is freed when the use ends; one
// at or below it is kept for the next use.
TEST_CASE("ScratchUse: frees a buffer left past its limit", "[util]") {
  constexpr std::size_t kLimit = 64 * 1024;
  std::vector<std::byte> buf;
  {
    bytecask::ScratchUse use{buf, kLimit};
    use.buf().resize(kLimit + 1);
  }
  CHECK(buf.capacity() == 0);
}

TEST_CASE("ScratchUse: keeps a buffer at its limit", "[util]") {
  constexpr std::size_t kLimit = 64 * 1024;
  std::vector<std::byte> buf;
  {
    bytecask::ScratchUse use{buf, kLimit};
    bytecask::resize_exact(use.buf(), kLimit);
  }
  CHECK(buf.capacity() == kLimit);
}
