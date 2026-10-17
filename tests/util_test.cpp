// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for bytecask.util.

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <format>
#include <sys/types.h>
#include <stdexcept>
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

// #384: a scratch buffer left past the thread scratch limit is freed when its use
// ends; one at or below the cap is kept for the next use.
TEST_CASE("ScratchUse: frees a buffer left past the cap when the use ends",
          "[util]") {
  bytecask::ThreadScratch<std::vector<std::byte>> s;
  {
    bytecask::ScratchUse use{s};
    CHECK(s.in_use);
    use.buf().resize(bytecask::thread_scratch_limit() + 1);
  }
  CHECK_FALSE(s.in_use);
  CHECK(s.buf.capacity() == 0);
}

TEST_CASE("ScratchUse: a second use while one is open is refused",
          "[util]") {
  bytecask::ThreadScratch<std::vector<std::byte>> s;
  bytecask::ScratchUse outer{s};
  CHECK_THROWS_AS([&] { bytecask::ScratchUse inner{s}; }(), std::logic_error);
  CHECK(s.in_use);  // the outer use still holds it
}

TEST_CASE("ScratchUse: keeps a buffer at or below the cap", "[util]") {
  bytecask::ThreadScratch<std::vector<std::byte>> s;
  const std::byte *held = nullptr;
  {
    bytecask::ScratchUse use{s};
    use.buf().resize(bytecask::thread_scratch_limit());
    use.buf().shrink_to_fit();
    REQUIRE(use.buf().capacity() <= bytecask::thread_scratch_limit());
    held = use.buf().data();
  }
  CHECK(s.buf.data() == held);
  // The next use gets the same buffer.
  bytecask::ScratchUse again{s};
  CHECK(again.buf().data() == held);
}

TEST_CASE("thread scratch limit: defaults, applies and refuses a value "
          "below the minimum",
          "[util]") {
  REQUIRE(bytecask::thread_scratch_limit() ==
          bytecask::kDefaultThreadScratchLimit);
  CHECK_THROWS_AS(
      bytecask::set_thread_scratch_limit(bytecask::kMinThreadScratchLimit - 1),
      std::invalid_argument);
  CHECK(bytecask::thread_scratch_limit() ==
        bytecask::kDefaultThreadScratchLimit);

  bytecask::ThreadScratch<std::vector<std::byte>> s;
  const auto big = bytecask::kDefaultThreadScratchLimit * 4;
  bytecask::set_thread_scratch_limit(big);
  {
    bytecask::ScratchUse use{s};
    use.buf().resize(big);
  }
  CHECK(s.buf.capacity() >= big);  // within the raised limit: kept
  bytecask::set_thread_scratch_limit(bytecask::kMinThreadScratchLimit);
  { bytecask::ScratchUse use{s}; }
  CHECK(s.buf.capacity() == 0);  // over the lowered one: freed at next use
  bytecask::set_thread_scratch_limit(bytecask::kDefaultThreadScratchLimit);
}
