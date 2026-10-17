// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — filesystem utilities, RNG helpers, and string formatting

module;
#include <atomic>
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

// A thread's scratch keeps its memory between calls, so a hot path does not
// allocate, but not past the thread scratch limit: a thread can outlive
// every DB, and one large record must not stay with it (#384). It is the
// largest record a thread serves without allocating, and what each of its
// scratch buffers keeps at most, so it is process-wide, not per DB: a thread
// shares its scratch between every DB it touches.
export inline constexpr std::size_t kDefaultThreadScratchLimit =
    std::size_t{256} << 10;
// Below this a buffer pool fill run or a commit chunk would hold a handful
// of frames or entries; there is no use for a smaller limit.
export inline constexpr std::size_t kMinThreadScratchLimit = std::size_t{64}
                                                              << 10;

// Relaxed: a new limit only has to reach each thread at some next call; it
// orders nothing else.
inline std::atomic<std::size_t> thread_scratch_limit_bytes{
    kDefaultThreadScratchLimit};

export [[nodiscard]] inline auto thread_scratch_limit() noexcept
    -> std::size_t {
  return thread_scratch_limit_bytes.load(std::memory_order_relaxed);
}

// Takes effect on each thread at its next call that uses a scratch buffer: a
// thread already holding more keeps it until then.
export inline void set_thread_scratch_limit(std::size_t bytes) {
  if (bytes < kMinThreadScratchLimit)
    throw std::invalid_argument{std::format(
        "thread scratch limit {} is below the minimum {}", bytes,
        kMinThreadScratchLimit)};
  thread_scratch_limit_bytes.store(bytes, std::memory_order_relaxed);
}

// A per-thread buffer, and whether a call on the thread is using it.
export template <typename Buf> struct ThreadScratch {
  Buf buf;
  bool in_use{false};
};

// One call's use of a ThreadScratch. A second use while one is open would
// hand the same buffer to two callers, and the inner one's growth would
// leave the outer one's spans dangling, so it is a bug: refused with
// std::logic_error before the buffer is handed out. A buffer
// left past thread_scratch_limit() is freed when the use ends. Buf needs
// capacity() and a noexcept move.
export template <typename Buf> class ScratchUse {
public:
  explicit ScratchUse(ThreadScratch<Buf> &s) : s_{s} {
    if (s_.in_use)
      throw std::logic_error{"thread scratch taken while already in use"};
    s_.in_use = true;
  }
  ~ScratchUse() {
    s_.in_use = false;
    if (s_.buf.capacity() > thread_scratch_limit()) s_.buf = Buf{};
  }
  ScratchUse(const ScratchUse &) = delete;
  auto operator=(const ScratchUse &) -> ScratchUse & = delete;

  [[nodiscard]] auto buf() noexcept -> Buf & { return s_.buf; }

private:
  ThreadScratch<Buf> &s_;
};
export template <typename Buf>
ScratchUse(ThreadScratch<Buf> &) -> ScratchUse<Buf>;

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

// Checks a write(2)-family result against the bytes asked for. -1 throws
// with errno. A short count — the file ran out of room part way: a full
// disk, RLIMIT_FSIZE — sets no errno, so it is reported as EIO with the byte
// counts, never as whatever an earlier call left in errno (#221). The write
// is not continued: a write that fails part way degrades the engine either
// way, and the counts point at the cause.
export inline void check_write(ssize_t written, std::size_t wanted,
                               std::string_view what) {
  if (written == narrow<ssize_t>(wanted)) return;
  if (written < 0) {
    throw std::system_error{errno, std::generic_category(),
                            std::format("{}: write failed", what)};
  }
  throw std::system_error{
      std::make_error_code(std::errc::io_error),
      std::format("{}: short write: the file took {} of {} bytes", what,
                  written, wanted)};
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
