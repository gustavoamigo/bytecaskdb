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

// #384: a scratch buffer grown past kScratchKeepBytes is freed when the
// outermost use ends, never while an outer use still holds it.
TEST_CASE("ScratchUse: frees a large buffer when the outermost use ends",
          "[util]") {
  bytecask::ThreadScratch<std::vector<std::byte>> s;
  {
    bytecask::ScratchUse outer{s};
    // Reserved up front, so only a free could move the buffer.
    outer.buf().reserve(bytecask::kScratchKeepBytes + 2);
    outer.buf().resize(bytecask::kScratchKeepBytes + 1);
    const auto *held = outer.buf().data();
    {
      bytecask::ScratchUse inner{s};
      inner.buf().resize(bytecask::kScratchKeepBytes + 2);
    }
    // The inner use ended, the outer one did not: nothing was freed.
    CHECK(outer.buf().data() == held);
    CHECK(s.users == 1);
  }
  CHECK(s.users == 0);
  CHECK(s.buf.capacity() == 0);
}

TEST_CASE("ScratchUse: keeps a buffer at or below kScratchKeepBytes",
          "[util]") {
  bytecask::ThreadScratch<std::vector<std::byte>> s;
  const std::byte *held = nullptr;
  {
    bytecask::ScratchUse use{s};
    use.buf().resize(bytecask::kScratchKeepBytes);
    use.buf().shrink_to_fit();
    REQUIRE(use.buf().capacity() <= bytecask::kScratchKeepBytes);
    held = use.buf().data();
  }
  CHECK(s.buf.data() == held);
}
