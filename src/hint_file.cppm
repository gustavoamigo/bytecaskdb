module;
#include <algorithm>
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
#include <vector>

export module bytecask.hint_file;

import bytecask.hint_entry;
import bytecask.serialization;
import bytecask.types;

namespace bytecask {

namespace {
constexpr std::size_t kFileCrcSize = 4;
} // namespace

// Read-write manager for a ByteCask hint file.
//
// Both modes operate on an in-memory buffer — no OS file descriptor is held
// open across calls. OpenForWrite buffers append()s in memory; sync() writes
// the entire buffer (plus a 4-byte file-level CRC-32C trailer) to disk in one
// shot. OpenForRead slurps the file into the buffer at construction time,
// verifies the CRC eagerly, and exposes a Scanner for sequential access.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class HintFile {
public:
  // Forward-only scanner over a hint file's entry region.
  // Owns a key accumulator for zero-copy prefix decompression:
  //   HintEntry.key is a span into key_buf_ valid until the next next() call.
  class Scanner {
  public:
    explicit Scanner(std::span<const std::byte> buf) : buf_{buf} {}

    // Returns the next entry, or nullopt at end of data.
    // key in the returned HintEntry is valid until the next call.
    // Throws std::runtime_error on a truncated entry.
    [[nodiscard]] auto next() -> std::optional<HintEntry> {
      if (pos_ >= buf_.size()) {
        return std::nullopt;
      }
      auto [he, consumed] = deserialize_entry(buf_.subspan(pos_), key_buf_);
      pos_ += consumed;
      return he;
    }

  private:
    std::span<const std::byte> buf_; // non-owning; excludes 4-byte CRC trailer
    std::size_t pos_{};
    std::vector<std::byte> key_buf_; // backing store for HintEntry.key spans
  };

  // Creates a write-mode HintFile that buffers entries in memory.
  [[nodiscard]] static auto OpenForWrite(std::filesystem::path path)
      -> HintFile {
    return HintFile{std::move(path)};
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

  ~HintFile() = default;
  HintFile(const HintFile &) = delete;
  HintFile &operator=(const HintFile &) = delete;
  HintFile(HintFile &&) noexcept = default;
  HintFile &operator=(HintFile &&) noexcept = default;

  // Encodes one hint entry with prefix compression and appends it to the
  // in-memory buffer. Keys must arrive in sorted order (as guaranteed by
  // flush_hints_for_file) for prefix sharing to be effective.
  void append(std::uint64_t sequence, EntryType entry_type,
              std::uint64_t file_offset, std::span<const std::byte> key,
              std::uint32_t value_size) {
    // Compute the shared prefix length with the previous key.
    const auto shared = [&] {
      const auto n = std::min(last_key_.size(), key.size());
      std::size_t i = 0;
      while (i < n && last_key_[i] == key[i]) {
        ++i;
      }
      return i;
    }();
    const auto prefix_len =
        static_cast<std::uint8_t>(std::min(shared, std::size_t{255}));
    const auto suffix  = key.subspan(prefix_len);
    auto entry_buf     = serialize_entry(sequence, entry_type, file_offset,
                                         value_size, prefix_len, suffix);
    buf_.insert(buf_.end(), entry_buf.begin(), entry_buf.end());
    last_key_.assign(key.begin(), key.end());
  }

  // Appends a 4-byte file-level CRC-32C over all buffered entry bytes, then
  // writes the entire buffer to disk in a single write() + fdatasync().
  void sync() {
    Crc32 crc{};
    crc.update(view());
    const auto crc_val  = crc.finalize();
    const auto old_size = buf_.size();
    buf_.resize(old_size + kFileCrcSize);
    ByteWriter tail{std::span{buf_}.subspan(old_size)};
    tail.put(crc_val);

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
  explicit HintFile(std::filesystem::path path)
      : path_{std::move(path)} {}

  explicit HintFile(std::filesystem::path path, std::vector<std::byte> buf)
      : path_{std::move(path)}, buf_{std::move(buf)} {}

  [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
    return {buf_.data(), buf_.size()};
  }

  std::filesystem::path path_;
  std::vector<std::byte> buf_;
  std::vector<std::byte> last_key_; // tracks previous key for prefix compression
};

} // namespace bytecask
