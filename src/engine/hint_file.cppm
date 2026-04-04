module;
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

export module bytecask.hint_file;

import bytecask.hint_entry;
import bytecask.serialization;
import bytecask.types;

namespace bytecask {

// Byte offset into a hint file, as consumed by scan() and returned from scan().
export using Offset = std::uint64_t;

// Read-write manager for a ByteCask hint file.
//
// Intent: A compact companion to a sealed .data file containing only key
// metadata, used to rebuild the in-memory Key Directory without scanning full
// data files. Mirrors the DataFile API: append/sync for writing, scan() for
// sequential scanning. Opened via named factory functions to make intent
// explicit — OpenForWrite or OpenForRead — keeping their file descriptors
// independent.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class HintFile {
public:
  // Opens or creates the file for append-only writing.
  // Throws std::system_error if the file cannot be opened.
  [[nodiscard]] static auto OpenForWrite(std::filesystem::path path)
      -> HintFile {
    auto fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile: cannot open '{}' for write", path.string())};
    }
    return HintFile{std::move(path), fd};
  }

  // Opens an existing hint file for reading. Does not create the file.
  // Throws std::system_error if the file cannot be opened.
  [[nodiscard]] static auto OpenForRead(std::filesystem::path path)
      -> HintFile {
    auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile: cannot open '{}' for read", path.string())};
    }
    return HintFile{std::move(path), fd};
  }

  ~HintFile() {
    if (fd_ != -1) {
      ::close(fd_);
    }
  }

  // Copying is disabled: a HintFile owns an OS file descriptor, and duplicating
  // it would create two objects that both believe they own the same fd and
  // would each close it on destruction, causing a double-close.
  //
  //   auto a = HintFile::OpenForRead("x.hint");
  //   auto b = a;  // ERROR: would give both 'a' and 'b' fd=5;
  //                // whichever destructs last calls close(5) twice.
  HintFile(const HintFile &) = delete;
  HintFile &operator=(const HintFile &) = delete;

  // Transfers ownership of the fd from 'other'. Setting other.fd_ to -1
  // prevents the moved-from destructor from closing our fd.
  //
  //   auto a = HintFile::OpenForRead("x.hint");  // a: fd=5
  //   auto b = std::move(a);                     // b: fd=5, a: fd=-1 (harmless
  //   destructor)
  HintFile(HintFile &&other) noexcept
      : path_{std::move(other.path_)}, fd_{other.fd_} {
    other.fd_ = -1;
  }

  // Closes any fd we currently own before stealing 'other's resources.
  // The self-assignment guard avoids closing the fd we are about to adopt.
  //
  //   auto a = HintFile::OpenForRead("a.hint");  // a: fd=5
  //   auto b = HintFile::OpenForRead("b.hint");  // b: fd=6
  //   b = std::move(a);                          // close(6), b: fd=5, a: fd=-1
  HintFile &operator=(HintFile &&other) noexcept {
    if (this != &other) {
      if (fd_ != -1) {
        ::close(fd_);
      }
      path_ = std::move(other.path_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // Serializes one hint entry and writes it to the OS page cache.
  // Does not guarantee durability — call sync() after.
  void append(std::uint64_t sequence, EntryType entry_type,
              std::uint64_t file_offset, std::span<const std::byte> key,
              std::uint32_t value_size) {
    const auto buf = serialize_entry(sequence, entry_type, file_offset,
                                          key, value_size);
    if (::write(fd_, buf.data(), buf.size()) != std::ssize(buf)) {
      throw std::system_error{errno, std::generic_category(),
                              "HintFile::append: write failed"};
    }
  }

  // Flushes pending writes to physical storage via fdatasync.
  // Call after one or more append()s to guarantee crash-safety (Group Commit).
  void sync() {
    if (::fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "HintFile::sync: fdatasync failed"};
    }
  }

  // Returns the parsed entry starting at byte offset and the offset of the next
  // entry. Returns std::nullopt at end-of-file. Panics (throws
  // std::runtime_error) on CRC mismatch — corruption is not silently skipped.
  // Pass 0 to start scanning from the beginning.
  //
  // Typical scan loop:
  //   Offset off = 0;
  //   while (auto r = file.scan(off)) { auto [entry, next] = *r; off = next; }
  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<HintEntry, Offset>> {
    if (offset == std::filesystem::file_size(path_)) {
      return std::nullopt;
    }

    // Read the fixed header to learn the key size.
    std::array<std::byte, kHintHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHintHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "HintFile::scan: pread header failed"};
    }

    ByteReader hdr_reader{std::span<const std::byte>{hdr}};
    hdr_reader.get<std::uint64_t>(); // sequence
    hdr_reader.get<std::uint8_t>();  // entry_type
    hdr_reader.get<std::uint64_t>(); // file_offset
    const auto key_size = hdr_reader.get<std::uint16_t>();

    // Read the full entry (header + key + CRC) into a contiguous buffer.
    const auto entry_size = kHintHeaderSize + key_size + kHintCrcSize;
    std::vector<std::byte> buf(entry_size);
    if (::pread(fd_, buf.data(), entry_size, narrow<off_t>(offset)) !=
        narrow<ssize_t>(entry_size)) {
      throw std::system_error{errno, std::generic_category(),
                              "HintFile::scan: pread entry failed"};
    }

    auto entry = deserialize_entry(std::span<const std::byte>{buf});
    const auto next_offset = offset + entry_size;
    return std::make_pair(std::move(entry), next_offset);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  explicit HintFile(std::filesystem::path path, int fd)
      : path_{std::move(path)}, fd_{fd} {}

  std::filesystem::path path_;
  int fd_{-1};
};

} // namespace bytecask
