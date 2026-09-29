// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — filesystem utilities, RNG helpers, and string formatting

module;
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <crc32c/crc32c.h>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

export module bytecask.util;

namespace bytecask {

// Checked narrowing conversion: validates that the source value fits in the
// target type using std::in_range and returns the converted value.
export template <typename To, typename From>
constexpr auto narrow(From value) -> To {
  if (!std::in_range<To>(value)) {
    throw std::runtime_error{"narrowing conversion out of range"};
  }
  return static_cast<To>(value);
}

// Unrecoverable invariant violation: prints msg to stderr and aborts.
//
// Deliberately not an exception. The write path wraps its I/O in catch (...)
// and turns any failure into a degraded state that resume() clears, so a
// thrown invariant violation would be swallowed and retried — masking the bug
// it is meant to expose. Active in release builds: the conditions that reach
// it risk silent data loss, which NDEBUG must not disable.
export [[noreturn]] inline void panic(std::string_view msg) {
  std::fputs("bytecask: PANIC: ", stderr);
  std::fwrite(msg.data(), 1, msg.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

// Reads from offset until dst is full or the file ends, and returns how many
// bytes it read. Throws std::system_error when pread fails.
export [[nodiscard]] inline auto pread_upto(int fd, std::uint64_t offset,
                                            std::span<std::byte> dst)
    -> std::size_t {
  std::size_t done = 0;
  while (done < dst.size()) {
    const auto n = ::pread(fd, dst.data() + done, dst.size() - done,
                           narrow<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::system_error{
          errno, std::generic_category(),
          std::format("bytecask: pread of {} bytes at offset {} failed",
                      dst.size(), offset)};
    }
    if (n == 0) break;  // end of file
    done += static_cast<std::size_t>(n);
  }
  return done;
}

// A read of `wanted` bytes at offset that met the end of the file after
// `got`. A pread that returns 0 sets no errno, so this is reported as EIO
// saying how much of the read the file held, never as errno 0 ("Success").
export [[noreturn]] inline void throw_short_read(std::uint64_t offset,
                                                 std::size_t wanted,
                                                 std::size_t got) {
  throw std::system_error{
      std::make_error_code(std::errc::io_error),
      std::format("bytecask: short read: the file ends {} bytes into a read "
                  "of {} at offset {}",
                  got, wanted, offset)};
}

// Fills dst from offset; the file ending first is a short read.
export inline void pread_exact(int fd, std::uint64_t offset,
                               std::span<std::byte> dst) {
  const auto got = pread_upto(fd, offset, dst);
  if (got < dst.size()) throw_short_read(offset, dst.size(), got);
}

// ---------------------------------------------------------------------------
// CRC-32C (Castagnoli, polynomial 0x1EDC6F41) via google/crc32c library.
//
// The library auto-detects hardware acceleration at runtime (SSE4.2 on x86-64,
// CRC instructions on AArch64) and falls back to a software implementation
// when neither is available.
// ---------------------------------------------------------------------------

// Stateful CRC-32C accumulator. Feed chunks via update(), read via finalize().
export class Crc32 {
public:
  void update(std::span<const std::byte> data) noexcept {
    state_ = crc32c::Extend(state_,
        reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }

  [[nodiscard]] auto finalize() const noexcept -> std::uint32_t {
    return state_;
  }

private:
  std::uint32_t state_ = 0;
};

} // namespace bytecask
