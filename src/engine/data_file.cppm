module;
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

export module bytecask.data_file;

import bytecask.util;
import bytecask.data_entry;
import bytecask.types;

namespace bytecask {

// Byte offset into a data file, as returned by append() and consumed by read().
export using Offset = std::uint64_t;

// Read-write manager for a ByteCask data file.
//
// Intent: Manages a physical `.data` file via POSIX I/O. Separates writing
// (append) from durability (sync) to enable Group Commit: callers batch
// multiple appends and issue a single sync() when they require crash-safety.
// The caller owns the global monotonic sequence number and passes it to
// append().
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class DataFile {
public:
  // Opens or creates the file via POSIX open(O_RDWR|O_CREAT|O_APPEND).
  // Throws std::system_error if the file cannot be opened.
  explicit DataFile(std::filesystem::path path) : path_{std::move(path)} {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("DataFile: cannot open '{}'", path_.string())};
    }
    offset_ = std::filesystem::file_size(path_);
  }

  ~DataFile() {
    if (fd_ != -1) {
      ::close(fd_);
    }
  }

  // Copying is disabled: a DataFile owns an OS file descriptor, and duplicating
  // it would create two objects that both believe they own the same fd and
  // would each close it on destruction, causing a double-close.
  //
  //   DataFile a{"x.data"};
  //   DataFile b = a;  // ERROR: would give both 'a' and 'b' fd=5;
  //                    // whichever destructs last calls close(5) twice.
  DataFile(const DataFile &) = delete;
  DataFile &operator=(const DataFile &) = delete;

  // Transfers ownership of the fd and offset from 'other'. Setting other.fd_
  // to -1 prevents the moved-from destructor from closing our fd.
  //
  //   DataFile a{"x.data"};          // a: fd=5
  //   DataFile b = std::move(a);     // b: fd=5, a: fd=-1 (harmless destructor)
  DataFile(DataFile &&other) noexcept
      : path_{std::move(other.path_)}, fd_{other.fd_}, offset_{other.offset_},
        sealed_{other.sealed_} {
    other.fd_ = -1;
  }

  // Closes any fd we currently own before stealing 'other's resources.
  // The self-assignment guard avoids closing the fd we are about to adopt.
  //
  //   DataFile a{"a.data"};          // a: fd=5
  //   DataFile b{"b.data"};          // b: fd=6
  //   b = std::move(a);              // close(6), b: fd=5, a: fd=-1
  DataFile &operator=(DataFile &&other) noexcept {
    if (this != &other) {
      if (fd_ != -1) {
        ::close(fd_);
      }
      path_ = std::move(other.path_);
      fd_ = other.fd_;
      offset_ = other.offset_;
      sealed_ = other.sealed_;
      other.fd_ = -1;
    }
    return *this;
  }

  // Writes an entry via writev(). Header and CRC are serialized into
  // hdr_crc_buf_ by data_entry; key and value are passed as direct iovecs.
  // No heap allocation, no copy of key/value data.
  // Precondition: the file must not have been sealed.
  [[nodiscard]] auto append(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value) -> Offset {
    assert(!sealed_);
    const auto entry_offset = offset_;

    write_header_and_crc(hdr_crc_buf_, sequence, entry_type, key, value);

    // Scatter-gather write: [header(15), key, value, crc(4)].
    const std::array<::iovec, 4> iov{{
        {hdr_crc_buf_.data(), kHeaderSize},
        {const_cast<std::byte *>(key.data()), key.size()},
        {const_cast<std::byte *>(value.data()), value.size()},
        {hdr_crc_buf_.data() + kHeaderSize, kCrcSize},
    }};
    const auto total = kHeaderSize + key.size() + value.size() + kCrcSize;
    const auto written = ::writev(fd_, iov.data(), std::ssize(iov));
    if (written != narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::append: writev failed"};
    }

    offset_ += static_cast<Offset>(total);
    return entry_offset;
  }

  // Reads and deserializes the entry at the given offset. Returns the entry
  // and the byte offset of the next entry. Returns std::nullopt when offset is
  // at or past end of file. Throws std::system_error on I/O failure or
  // std::runtime_error on CRC mismatch.
  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> {
    if (offset >= offset_) {
      return std::nullopt;
    }

    // Read the fixed header to determine variable-length field sizes.
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::scan: pread header failed"};
    }

    const auto header = read_header(std::span{hdr});
    std::vector<std::byte> buf;
    read_entry(offset, header.key_size, header.value_size, buf);
    auto entry = deserialize_entry(buf);
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
    return std::make_pair(std::move(entry), next);
  }

  // Preads the full entry at offset into io_buf (reusing existing capacity).
  // The caller supplies key_size and value_size (known from KeyDirEntry or
  // a prior header parse). After this call, io_buf contains a complete
  // entry that can be passed to deserialize_entry() or extract_value_into()
  // depending on what the caller needs.
  void read_entry(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size,
                  std::vector<std::byte> &io_buf) const {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    io_buf.resize(total);
    if (::pread(fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
        narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::read_entry: pread failed"};
    }
  }

  // High-level read: preads the entry, verifies CRC, and extracts only the
  // value into out. io_buf is a caller-owned scratch buffer whose capacity
  // is reused across calls to amortize allocation.
  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size,
                  std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const {
    read_entry(offset, key_size, value_size, io_buf);
    extract_value_into(io_buf, out);
  }

  // Flushes all pending writes to physical storage via fdatasync.
  // Call after one or more append()s to guarantee crash-safety (Group Commit).
  void sync() {
    if (::fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::sync: fdatasync failed"};
    }
  }

  // Returns the current file size in bytes (equal to the write offset).
  [[nodiscard]] auto size() const noexcept -> Offset { return offset_; }

  // Marks the file as sealed: no further appends are permitted.
  // The fd remains open for reads. seal() is called by Bytecask on rotation.
  void seal() noexcept { sealed_ = true; }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  std::filesystem::path path_;
  int fd_{-1};
  Offset offset_{0};
  bool sealed_{false};
  // Fixed buffer holding the 15-byte header and 4-byte CRC for each append.
  // Avoids heap allocation on the hot write path.
  std::array<std::byte, kHeaderSize + kCrcSize> hdr_crc_buf_{};
};

} // namespace bytecask
