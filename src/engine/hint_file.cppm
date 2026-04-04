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
// data files. Mirrors the DataFile API: append/sync for writing, scan() /
// scan_view() for sequential scanning. Opened via named factory functions to
// make intent explicit — OpenForWrite or OpenForRead — keeping their file
// descriptors independent.
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

  // Opens an existing hint file for reading. Reads the entire file into an
  // in-memory buffer in one syscall so that scan() operates on memory rather
  // than issuing two pread calls per entry. Throws std::system_error on I/O
  // failure.
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
            std::format("HintFile: cannot read '{}' into buffer", path.string())};
      }
    }
    ::close(fd);
    return HintFile{std::move(path), std::move(buf)};
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
      : path_{std::move(other.path_)}, fd_{other.fd_},
        buf_{std::move(other.buf_)} {
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
      buf_ = std::move(other.buf_);
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

  // Returns the parsed entry at byte offset (owning key copy).
  // Returns std::nullopt at end-of-file. Throws std::runtime_error on CRC
  // mismatch. Pass 0 to start scanning from the beginning.
  //
  // Operates entirely from the backing store populated at open time;
  // no I/O per call.
  //
  // Typical scan loop:
  //   Offset off = 0;
  //   while (auto r = file.scan(off)) { auto [entry, next] = *r; off = next; }
  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<HintEntry, Offset>> {
    const auto b = view();
    if (offset >= b.size()) {
      return std::nullopt;
    }
    const auto [entry_span, next] = entry_span_at(b, offset);
    auto entry = deserialize_entry(entry_span);
    return std::make_pair(std::move(entry), next);
  }

  // Zero-copy variant: the key in the returned HintEntryView is a span into
  // the HintFile's backing buffer. Valid only while this HintFile is alive.
  // Use in scan loops where the key is consumed before the next scan_view()
  // call and ownership is not required.
  [[nodiscard]] auto scan_view(Offset offset) const
      -> std::optional<std::pair<HintEntryView, Offset>> {
    const auto b = view();
    if (offset >= b.size()) {
      return std::nullopt;
    }
    const auto [entry_span, next] = entry_span_at(b, offset);
    auto entry = deserialize_entry_view(entry_span);
    return std::make_pair(std::move(entry), next);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  // Slices the entry span at offset and returns it together with the next
  // offset. Shared by scan() and scan_view() to avoid duplicating bounds checks.
  [[nodiscard]] static auto entry_span_at(std::span<const std::byte> b,
                                          Offset offset)
      -> std::pair<std::span<const std::byte>, Offset> {
    if (offset + kHintHeaderSize > b.size()) {
      throw std::runtime_error{"HintFile: truncated header in buffer"};
    }
    const std::span<const std::byte> hdr_span{b.data() + offset,
                                              kHintHeaderSize};
    ByteReader hdr_reader{hdr_span};
    hdr_reader.get<std::uint64_t>(); // sequence
    hdr_reader.get<std::uint8_t>();  // entry_type
    hdr_reader.get<std::uint64_t>(); // file_offset
    const auto key_size = hdr_reader.get<std::uint16_t>();

    const auto entry_size = kHintHeaderSize + key_size + kHintCrcSize;
    if (offset + entry_size > b.size()) {
      throw std::runtime_error{"HintFile: truncated entry in buffer"};
    }
    return {std::span<const std::byte>{b.data() + offset, entry_size},
            offset + entry_size};
}

  // Constructor for OpenForWrite: owns a live fd, empty buffer.
  explicit HintFile(std::filesystem::path path, int fd)
      : path_{std::move(path)}, fd_{fd} {}

  // Constructor for OpenForRead: no fd (already closed), owns the buffer.
  explicit HintFile(std::filesystem::path path, std::vector<std::byte> buf)
      : path_{std::move(path)}, fd_{-1}, buf_{std::move(buf)} {}

  // Returns a span over the backing buffer; empty for write instances.
  [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
    return {buf_.data(), buf_.size()};
  }

  std::filesystem::path path_;
  int fd_{-1};
  std::vector<std::byte> buf_; // populated by OpenForRead; empty for write instances
};

} // namespace bytecask
