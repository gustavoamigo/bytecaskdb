module;
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module bytecask.data_file;

import bytecask.crc32;
import bytecask.data_entry;
import std;

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

  // Writes an entry with the given sequence number and type to the OS page
  // cache. Returns the absolute byte offset where the entry begins. BulkBegin
  // and BulkEnd entries use empty key and value spans. Does not guarantee
  // durability — call sync() to flush to physical storage.
  // Precondition: the file must not have been sealed.
  [[nodiscard]] auto append(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value) -> Offset {
    assert(!sealed_);
    const auto entry_offset = offset_;
    const auto buf = serialize_entry(sequence, entry_type, key, value);

    const auto written = ::write(fd_, buf.data(), buf.size());
    if (written != std::ssize(buf)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::append: write failed"};
    }

    offset_ += static_cast<Offset>(buf.size());
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

    // Step 1: read the fixed header to determine variable-length field sizes.
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::scan: pread header failed"};
    }

    const auto header = parse_header(std::span{hdr});

    // Step 2: allocate full entry buffer, copy header, then read the rest.
    std::vector<std::byte> buf(kHeaderSize + header.key_size +
                               header.value_size + kCrcSize);
    std::ranges::copy(hdr, buf.begin());

    const auto tail = std::span{buf}.subspan(kHeaderSize);
    if (::pread(fd_, tail.data(), tail.size(),
                narrow<off_t>(offset + kHeaderSize)) != std::ssize(tail)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::scan: pread payload failed"};
    }

    auto entry = deserialize_entry(buf);
    const auto next = offset + static_cast<Offset>(buf.size());
    return std::make_pair(std::move(entry), next);
  }

  // Reads and deserializes the entry at the given offset. Delegates to scan().
  // Throws std::system_error on I/O failure, std::runtime_error on CRC
  // mismatch or if offset is at or past end of file.
  [[nodiscard]] auto read(Offset offset) const -> DataEntry {
    auto result = scan(offset);
    if (!result) {
      throw std::runtime_error{
          std::format("DataFile::read: offset {} is past end of file", offset)};
    }
    return std::move(result->first);
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
};

} // namespace bytecask
