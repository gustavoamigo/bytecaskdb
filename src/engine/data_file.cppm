module;
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module bytecask.data_file;

import bytecask.data_entry;
import std;

namespace bytecask {

// Append-only writer for a ByteCask data file.
//
// Intent: Manages a physical `.data` file via POSIX I/O. Separates writing
// (append) from durability (sync) to enable Group Commit: callers batch
// multiple appends and issue a single sync() when they require crash-safety.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class DataFile {
public:
  // Opens or creates the file via POSIX open(O_WRONLY|O_CREAT|O_APPEND).
  // Throws std::system_error if the file cannot be opened.
  explicit DataFile(std::filesystem::path path) : path_{std::move(path)} {
    fd_ =
        ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
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

  DataFile(const DataFile &) = delete;
  DataFile &operator=(const DataFile &) = delete;

  DataFile(DataFile &&other) noexcept
      : path_{std::move(other.path_)}, fd_{other.fd_},
        sequence_{other.sequence_}, offset_{other.offset_} {
    other.fd_ = -1;
  }

  DataFile &operator=(DataFile &&other) noexcept {
    if (this != &other) {
      if (fd_ != -1) {
        ::close(fd_);
      }
      path_ = std::move(other.path_);
      fd_ = other.fd_;
      sequence_ = other.sequence_;
      offset_ = other.offset_;
      other.fd_ = -1;
    }
    return *this;
  }

  // Writes a key-value entry to the OS page cache and increments the sequence.
  // Returns the absolute byte offset where the entry begins.
  // Does not guarantee durability — call sync() to flush to physical storage.
  auto append(std::span<const std::byte> key, std::span<const std::byte> value)
      -> std::uint64_t {
    const auto entry_offset = offset_;
    const auto buf = serialize_entry(++sequence_, key, value);

    const auto written = ::write(fd_, buf.data(), buf.size());
    if (written != std::ssize(buf)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::append: write failed"};
    }

    offset_ += static_cast<std::uint64_t>(buf.size());
    return entry_offset;
  }

  // Flushes all pending writes to physical storage via fdatasync.
  // Call after one or more append()s to guarantee crash-safety (Group Commit).
  void sync() {
    if (::fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::sync: fdatasync failed"};
    }
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }
  [[nodiscard]] auto sequence() const -> std::uint64_t { return sequence_; }

private:
  std::filesystem::path path_;
  int fd_{-1};
  std::uint64_t sequence_{0};
  std::uint64_t offset_{0};
};

} // namespace bytecask
