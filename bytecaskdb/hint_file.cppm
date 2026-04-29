// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — hint file writing and sequential recovery scan

module;
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
static inline int portable_fdatasync(int fd) { return fcntl(fd, F_FULLFSYNC); }
#else
static inline int portable_fdatasync(int fd) { return fdatasync(fd); }
#endif

export module bytecask.hint_file;

import bytecask.hint_entry;
import bytecask.serialization;
import bytecask.types;

namespace bytecask {

namespace {
constexpr std::size_t kFileCrcSize = 4;
} // namespace

// Writer and reader for ByteCask hint files.
//
// Write mode (OpenForWrite): opens the file immediately and writes each
// entry directly to disk via the fd. A running CRC-32C is accumulated
// across all appended bytes. close() writes the 4-byte CRC trailer,
// calls fdatasync, and closes the fd.
//
// Read mode (OpenForRead): slurps the file into an in-memory buffer,
// verifies the file-level CRC eagerly, and exposes a Scanner.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class HintFile {
public:
  // Forward-only scanner over a hint file's entry region.
  // HintEntry.key and .end_key are spans into the backing file buffer,
  // valid for the lifetime of this HintFile.
  //
  // Two usage modes:
  //   next()-style: construct via Scanner(buf), call next() in a while loop.
  //   iterator-style: construct via Scanner(buf, eager), use in range-for.
  class Scanner {
  public:
    using iterator_concept = std::input_iterator_tag;
    using value_type = HintEntry;
    using difference_type = std::ptrdiff_t;

    // next()-style constructor (does not eagerly read).
    explicit Scanner(std::span<const std::byte> buf) : buf_{buf} {}

    // Iterator-style constructor: eagerly reads the first entry so that
    // operator* is valid immediately after construction.
    struct eager_t {};
    static constexpr eager_t eager{};
    Scanner(std::span<const std::byte> buf, eager_t) : buf_{buf} {
      cached_ = next();
    }

    // Returns the next entry, or nullopt at end of data.
    // Throws std::runtime_error on a truncated entry.
    [[nodiscard]] auto next() -> std::optional<HintEntry> {
      if (pos_ >= buf_.size()) {
        return std::nullopt;
      }
      auto [he, consumed] = deserialize_entry(buf_.subspan(pos_));
      pos_ += consumed;
      return he;
    }

    // Iterator interface — used only when constructed with eager_t.
    auto operator*() const -> const value_type& { return *cached_; }
    auto operator++() -> Scanner& { cached_ = next(); return *this; }
    void operator++(int) { ++*this; }
    auto operator==(std::default_sentinel_t) const noexcept -> bool {
      return !cached_.has_value();
    }

  private:
    std::span<const std::byte> buf_; // non-owning; excludes 4-byte CRC trailer
    std::size_t pos_{};
    std::optional<HintEntry> cached_; // populated only in iterator mode
  };

  // Creates a write-mode HintFile. Opens the file immediately for writing.
  [[nodiscard]] static auto OpenForWrite(std::filesystem::path path)
      -> HintFile {
    auto fd = ::open(path.c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile: cannot open '{}' for write", path.string())};
    }
    return HintFile{std::move(path), fd};
  }

  // Opens an existing hint file for reading. Reads the entire file into an
  // in-memory buffer in one syscall, then verifies the file-level CRC-32C
  // before returning. Throws on I/O failure or CRC mismatch.
  [[nodiscard]] static auto OpenForRead(std::filesystem::path path)
      -> HintFile {
    auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile: cannot open '{}' for read", path.string())};
    }
    const auto file_sz = std::filesystem::file_size(path);
    std::vector<std::byte> buf(file_sz);
    if (file_sz > 0) {
      if (::pread(fd, buf.data(), file_sz, 0) != narrow<ssize_t>(file_sz)) {
        const auto err = errno;
        ::close(fd);
        throw std::system_error{
            err, std::generic_category(),
            std::format("HintFile: cannot read '{}' into buffer",
                        path.string())};
      }
    }
    ::close(fd);

    // Eagerly verify the file-level CRC before any parsing.
    if (buf.size() < kFileCrcSize) {
      throw std::runtime_error{std::format(
          "HintFile: '{}' is too small to contain a CRC trailer",
          path.string())};
    }
    Crc32 crc{};
    crc.update(std::span{buf}.subspan(0, buf.size() - kFileCrcSize));
    const auto computed = crc.finalize();
    const auto stored   = read_le<std::uint32_t>(
        std::span<const std::byte>{buf}, buf.size() - kFileCrcSize);
    if (computed != stored) {
      throw std::runtime_error{
          std::format("HintFile: CRC mismatch in '{}'", path.string())};
    }

    return HintFile{std::move(path), std::move(buf)};
  }

  ~HintFile() {
    // If the write fd is still open (close() was not called — e.g. exception
    // path), close without writing CRC. The .hint.tmp file will be cleaned
    // up on next startup.
    if (write_fd_ != -1) {
      ::close(write_fd_);
    }
  }

  HintFile(const HintFile &) = delete;
  HintFile &operator=(const HintFile &) = delete;

  HintFile(HintFile &&other) noexcept
      : path_{std::move(other.path_)},
        buf_{std::move(other.buf_)},
        write_fd_{other.write_fd_},
        crc_{other.crc_} {
    other.write_fd_ = -1;
  }

  HintFile &operator=(HintFile &&other) noexcept {
    if (this != &other) {
      if (write_fd_ != -1) ::close(write_fd_);
      path_ = std::move(other.path_);
      buf_ = std::move(other.buf_);
      write_fd_ = other.write_fd_;
      crc_ = other.crc_;
      other.write_fd_ = -1;
    }
    return *this;
  }

  // Serializes one hint entry and writes it directly to disk.
  void append(std::uint64_t sequence, EntryType entry_type,
              std::uint64_t file_offset, std::span<const std::byte> key,
              std::uint32_t value_size) {
    auto entry_buf = serialize_entry(sequence, entry_type, file_offset,
                                     value_size, key);
    write_bytes(entry_buf);
  }

  // Serializes a RangeDel hint entry and writes it directly to disk.
  void append_range_del(std::uint64_t sequence, std::uint64_t file_offset,
                        std::span<const std::byte> start_key,
                        std::span<const std::byte> end_key) {
    auto entry_buf = serialize_range_del_entry(sequence, file_offset,
                                               start_key, end_key);
    write_bytes(entry_buf);
  }

  // Writes the 4-byte CRC-32C trailer, calls fdatasync, and closes the fd.
  // Must be called exactly once on a write-mode HintFile.
  void close() {
    std::array<std::byte, kFileCrcSize> trailer{};
    ByteWriter w{trailer};
    w.put(crc_.finalize());
    if (::write(write_fd_, trailer.data(), trailer.size()) !=
        std::ssize(trailer)) {
      const auto err = errno;
      ::close(write_fd_);
      write_fd_ = -1;
      throw std::system_error{err, std::generic_category(),
                              "HintFile::close: write CRC trailer failed"};
    }
    if (portable_fdatasync(write_fd_) != 0) {
      const auto err = errno;
      ::close(write_fd_);
      write_fd_ = -1;
      throw std::system_error{err, std::generic_category(),
                              "HintFile::close: fdatasync failed"};
    }
    ::close(write_fd_);
    write_fd_ = -1;
  }

  // Returns a Scanner over the entry bytes (excluding the 4-byte CRC trailer).
  // The Scanner holds a non-owning view into this HintFile's buffer; this
  // HintFile must outlive the Scanner.
  [[nodiscard]] auto make_scanner() const -> Scanner {
    const auto b       = view();
    const auto entries = (b.size() >= kFileCrcSize)
                             ? b.subspan(0, b.size() - kFileCrcSize)
                             : std::span<const std::byte>{};
    return Scanner{entries};
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  // Write-mode constructor: holds the open fd.
  explicit HintFile(std::filesystem::path path, int fd)
      : path_{std::move(path)}, write_fd_{fd} {}

  // Read-mode constructor: holds the file buffer.
  explicit HintFile(std::filesystem::path path, std::vector<std::byte> buf)
      : path_{std::move(path)}, buf_{std::move(buf)} {}

  void write_bytes(std::span<const std::byte> data) {
    if (::write(write_fd_, data.data(), data.size()) !=
        std::ssize(data)) {
      const auto err = errno;
      ::close(write_fd_);
      write_fd_ = -1;
      throw std::system_error{err, std::generic_category(),
                              "HintFile::append: write failed"};
    }
    crc_.update(data);
  }

  [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
    return {buf_.data(), buf_.size()};
  }

  std::filesystem::path path_;
  std::vector<std::byte> buf_;   // read mode only
  int write_fd_{-1};             // write mode only; -1 when closed or read mode
  Crc32 crc_{};                  // write mode only; running CRC accumulator
};

} // namespace bytecask
