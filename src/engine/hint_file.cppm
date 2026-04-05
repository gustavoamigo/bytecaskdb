module;
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
// Both modes operate on an in-memory buffer — no OS file descriptor is held
// open across calls. OpenForWrite buffers append()s in memory; sync() writes
// the entire buffer to disk in one shot. OpenForRead slurps the file into the
// buffer at construction time so scan() operate on memory.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class HintFile {
public:
  // Creates a write-mode HintFile that buffers entries in memory.
  [[nodiscard]] static auto OpenForWrite(std::filesystem::path path)
      -> HintFile {
    return HintFile{std::move(path)};
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

  ~HintFile() = default;
  HintFile(const HintFile &) = delete;
  HintFile &operator=(const HintFile &) = delete;
  HintFile(HintFile &&) noexcept = default;
  HintFile &operator=(HintFile &&) noexcept = default;

  // Serializes one hint entry and appends it to the in-memory buffer.
  void append(std::uint64_t sequence, EntryType entry_type,
              std::uint64_t file_offset, std::span<const std::byte> key,
              std::uint32_t value_size) {
    auto entry_buf = serialize_entry(sequence, entry_type, file_offset,
                                     key, value_size);
    buf_.insert(buf_.end(), entry_buf.begin(), entry_buf.end());
  }

  // Writes the in-memory buffer to disk in a single write() and fdatasyncs.
  void sync() {
    auto fd = ::open(path_.c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile::sync: cannot open '{}'", path_.string())};
    }
    if (!buf_.empty()) {
      if (::write(fd, buf_.data(), buf_.size()) != std::ssize(buf_)) {
        const auto err = errno;
        ::close(fd);
        throw std::system_error{err, std::generic_category(),
                                "HintFile::sync: write failed"};
      }
    }
    if (::fdatasync(fd) != 0) {
      const auto err = errno;
      ::close(fd);
      throw std::system_error{err, std::generic_category(),
                              "HintFile::sync: fdatasync failed"};
    }
    ::close(fd);
  }

  // The key in the returned HintEntry is a span into the HintFile's
  // backing buffer — valid only while this HintFile is alive.
  // Returns std::nullopt at end-of-file. Throws std::runtime_error on CRC
  // mismatch. Pass 0 to start scanning from the beginning.
  //
  // Typical scan loop:
  //   Offset off = 0;
  //   while (auto r = file.scan(off)) { auto& [e, next] = *r; off = next; }
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

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  // Slices the entry span at offset and returns it together with the next
  // Shared by scan() to avoid duplicating bounds checks.
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

  // Constructor for OpenForWrite: empty buffer, no I/O.
  explicit HintFile(std::filesystem::path path)
      : path_{std::move(path)} {}

  // Constructor for OpenForRead: owns the buffer pre-populated with file data.
  explicit HintFile(std::filesystem::path path, std::vector<std::byte> buf)
      : path_{std::move(path)}, buf_{std::move(buf)} {}

  // Returns a span over the backing buffer.
  [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
    return {buf_.data(), buf_.size()};
  }

  std::filesystem::path path_;
  std::vector<std::byte> buf_;
};

} // namespace bytecask
