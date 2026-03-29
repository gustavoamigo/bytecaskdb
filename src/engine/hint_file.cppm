module;
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module bytecask.hint_file;

export import bytecask.hint_entry;
import bytecask.serialization;
import std;

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
    const auto buf = serialize_hint_entry(sequence, entry_type, file_offset,
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

    // Read the fixed header.
    std::array<std::byte, kHintHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHintHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "HintFile::scan: pread header failed"};
    }

    const auto hdr_span = std::span<const std::byte>{hdr};
    const auto sequence = read_le<std::uint64_t>(hdr_span, 0);
    const auto entry_type =
        static_cast<EntryType>(read_le<std::uint8_t>(hdr_span, 8));
    const auto file_offset_val = read_le<std::uint64_t>(hdr_span, 9);
    const auto key_size = read_le<std::uint16_t>(hdr_span, 17);
    const auto value_size = read_le<std::uint32_t>(hdr_span, 19);

    // Read key bytes.
    std::vector<std::byte> key(key_size);
    if (key_size > 0) {
      if (::pread(fd_, key.data(), key_size,
                  narrow<off_t>(offset + kHintHeaderSize)) != std::ssize(key)) {
        throw std::system_error{errno, std::generic_category(),
                                "HintFile::scan: pread key failed"};
      }
    }

    // Read trailing CRC.
    std::array<std::byte, kHintCrcSize> crc_buf{};
    if (::pread(fd_, crc_buf.data(), kHintCrcSize,
                narrow<off_t>(offset + kHintHeaderSize + key_size)) !=
        std::ssize(crc_buf)) {
      throw std::system_error{errno, std::generic_category(),
                              "HintFile::scan: pread CRC failed"};
    }

    // Verify CRC over header + key — same bytes that serialize_hint_entry fed
    // through CrcOutputAdapter.
    Crc32 crc{};
    crc.update(hdr_span);
    crc.update(std::span<const std::byte>{key});
    const auto computed = crc.finalize();
    const auto stored =
        read_le<std::uint32_t>(std::span<const std::byte>{crc_buf}, 0);
    if (computed != stored) {
      throw std::runtime_error{
          std::format("HintFile::scan: CRC mismatch at offset {}", offset)};
    }

    const auto next_offset = offset + kHintHeaderSize + key_size + kHintCrcSize;
    return std::make_pair(HintEntry{.sequence = sequence,
                                    .entry_type = entry_type,
                                    .file_offset = file_offset_val,
                                    .key = std::move(key),
                                    .value_size = value_size},
                          next_offset);
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
