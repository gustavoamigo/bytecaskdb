// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — hint file writing and sequential recovery scan

module;
#include <algorithm>
#include <array>
#include <cerrno>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>
#include <zstd.h>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif

#ifdef __APPLE__
static inline int portable_fdatasync(int fd) { return fcntl(fd, F_FULLFSYNC); }
#else
static inline int portable_fdatasync(int fd) { return fdatasync(fd); }
#endif

// The hint's fdatasync, with its fault point in the same call: a test that
// fails io_hint_sync fails only while the hint is still synced.
static inline int sync_hint(int fd) {
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_hint_sync);
#endif
  return portable_fdatasync(fd);
}

export module bytecask.hint_file;

import bytecask.hint_entry;
import bytecask.serialization;
import bytecask.types;

namespace bytecask {

// Hint files are written as a header, a run of zstd frames, and a trailer:
//
//   magic     8 bytes   "BCHINTZ" 0x81
//   version   u8        1
//   codec     u8        1 = zstd
//   reserved  6 bytes   zero
//   frames              one zstd frame per ~kHintFrameBytes of entries, each
//                       holding whole entries and its own decompressed size
//   trailer   u32       bitwise NOT of the CRC-32C over every byte before it
//
// A frame decompresses to exactly the bytes the entries serialize to
// (hint_entry.cppm); compression changes how a hint is stored, not what it
// says. Files written before compression — the entries back to back, then a
// plain CRC-32C — are still read. The first u64 of such a file is the
// sequence of its first entry, which never reaches 2^63, and the magic's last
// byte sets that bit, so the two cannot be confused. The trailer is inverted
// so that a reader that knows only the old layout fails the CRC on a new
// file and rebuilds it from its data file, instead of parsing frames as
// entries. See docs/hint_compression_design.md.

// Entries are buffered until a frame holds at least this many bytes. Small
// enough that a merge holding one decoded frame per cursor stays bounded,
// large enough that zstd keeps nearly all of its ratio.
export constexpr std::size_t kHintFrameBytes = 16 * 1024;

namespace {
constexpr std::size_t kFileCrcSize = 4;
constexpr std::array<std::byte, 8> kFramedMagic{
    std::byte{'B'}, std::byte{'C'}, std::byte{'H'}, std::byte{'I'},
    std::byte{'N'}, std::byte{'T'}, std::byte{'Z'}, std::byte{0x81}};
constexpr std::size_t kFramedHeaderSize = 16;
constexpr std::uint8_t kFramedVersion = 1;
constexpr std::uint8_t kCodecZstd = 1;
// Level 3 compressed no better than level 1 on any key shape measured.
constexpr int kZstdLevel = 1;
// A frame closes once it reaches the target, so it holds less than the
// target plus one entry; the largest entry is a RangeDel with two maximal
// keys. A frame claiming more is corrupt.
constexpr std::size_t kMaxHintEntryBytes =
    kHintHeaderSize + 0xFFFF + sizeof(std::uint16_t) + 0xFFFF;
constexpr std::size_t kMaxFrameBytes = kHintFrameBytes + kMaxHintEntryBytes;
// The most such a frame takes on disk: its entries, not compressed at all.
constexpr std::size_t kMaxPackedFrameBytes = ZSTD_COMPRESSBOUND(kMaxFrameBytes);

#ifdef BYTECASK_TESTING
std::size_t g_frame_bytes_for_testing = 0;
#endif

auto frame_target() noexcept -> std::size_t {
#ifdef BYTECASK_TESTING
  if (g_frame_bytes_for_testing != 0) return g_frame_bytes_for_testing;
#endif
  return kHintFrameBytes;
}

struct ZstdCCtxFree {
  void operator()(ZSTD_CCtx *c) const noexcept { ZSTD_freeCCtx(c); }
};
struct ZstdDCtxFree {
  void operator()(ZSTD_DCtx *c) const noexcept { ZSTD_freeDCtx(c); }
};

// One decompression context per thread, shared by every scanner the thread
// drives: a context is ~100 KB, and a recovery merge holds a scanner per
// file per range.
auto thread_dctx() -> ZSTD_DCtx & {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // The destructor is wanted: it frees the context when the thread exits.
  thread_local const std::unique_ptr<ZSTD_DCtx, ZstdDCtxFree> ctx{
      ZSTD_createDCtx()};
#pragma clang diagnostic pop
  if (!ctx) throw std::bad_alloc{};
  return *ctx;
}

// Scratch for the compressed bytes of the frame being decoded: a scanner
// needs them only until the frame is decompressed into its own buffer, so
// every scanner a thread drives shares one.
auto thread_packed_frame() -> std::vector<std::byte> & {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // The destructor is wanted: it frees the buffer when the thread exits.
  thread_local std::vector<std::byte> buf;
#pragma clang diagnostic pop
  return buf;
}

// Reads exactly out.size() bytes at offset. Every read of a hint file goes
// through here, so a failed read is a std::system_error — as every other read
// in the engine is — and never a SIGBUS, which is what a memory mapping turns
// a page it cannot fill into. A file that ends early changed under the reader.
void read_exact(int fd, std::span<std::byte> out, std::uint64_t offset,
                const std::filesystem::path &path) {
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_hint_read);
#endif
  try {
    pread_exact(fd, offset, out);
  } catch (const std::system_error &e) {
    throw std::system_error{
        e.code(),
        std::format("HintFile: cannot read '{}': {}", path.string(), e.what())};
  }
}

// A descriptor closed when it goes out of scope.
class ScopedFd {
public:
  explicit ScopedFd(int fd) noexcept : fd_{fd} {}
  ~ScopedFd() { ::close(fd_); }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  ScopedFd(ScopedFd &&) = delete;
  ScopedFd &operator=(ScopedFd &&) = delete;
  [[nodiscard]] auto get() const noexcept -> int { return fd_; }

private:
  int fd_;
};

// Opens a hint file for reading. Throws std::system_error when it cannot.
auto open_read_only(const std::filesystem::path &path) -> ScopedFd {
  const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    throw std::system_error{
        errno, std::generic_category(),
        std::format("HintFile: cannot open '{}' for read", path.string())};
  }
  return ScopedFd{fd};
}

// What a reader knows about a hint file: the path to open it at, and where
// its units are. A unit is what a scanner reads and decodes at once: a zstd
// frame, or in a raw file a run of whole entries about a frame long. The file
// is not held open: the pass that opens it and every unit read after it open
// the file for the duration of the read, so a recovery merging thousands of
// files costs each thread one descriptor, not one per file (#251).
struct HintSource {
  std::filesystem::path path;
  bool framed{false};
  // File offset of each unit, then the offset where the units end.
  std::vector<std::uint64_t> units;
};

// The open pass reads a file front to back in chunks of this size. A chunk
// holds any unit whole: the largest frame, or the largest raw entry.
constexpr std::size_t kOpenChunkBytes = 256 * 1024;
static_assert(kOpenChunkBytes >= kMaxPackedFrameBytes);
static_assert(kOpenChunkBytes >= kMaxHintEntryBytes);

// Sequential reader for the open pass. Every byte before `end` is read once,
// in order, and fed to the CRC as it arrives.
class OpenPass {
public:
  OpenPass(int fd, const std::filesystem::path &path, std::uint64_t end)
      : fd_{fd}, path_{path}, end_{end}, buf_(kOpenChunkBytes) {}

  // The bytes from `at` on that are in the buffer: at least
  // min(want, end - at) of them, reading more if needed. `at` only moves
  // forward, and never past what an earlier call returned.
  [[nodiscard]] auto window(std::uint64_t at, std::size_t want)
      -> std::span<const std::byte> {
    const auto read_to = buf_at_ + len_;
    const auto need = std::min<std::uint64_t>(want, end_ - at);
    if (at + need > read_to) {
      // Keep the tail not yet parsed, and fill the rest of the buffer.
      const auto keep = static_cast<std::size_t>(read_to - at);
      const auto from = static_cast<std::size_t>(at - buf_at_);
      std::copy(buf_.begin() + narrow<std::ptrdiff_t>(from),
                buf_.begin() + narrow<std::ptrdiff_t>(from + keep),
                buf_.begin());
      const auto n = static_cast<std::size_t>(
          std::min<std::uint64_t>(buf_.size() - keep, end_ - read_to));
      const auto fresh = std::span{buf_}.subspan(keep, n);
      read_exact(fd_, fresh, read_to, path_);
      crc_.update(fresh);
      buf_at_ = at;
      len_ = keep + n;
    }
    return std::span<const std::byte>{buf_}.subspan(
        static_cast<std::size_t>(at - buf_at_),
        static_cast<std::size_t>(buf_at_ + len_ - at));
  }

  // The CRC of every byte read. Complete once the pass has reached `end`.
  [[nodiscard]] auto crc() const noexcept -> std::uint32_t {
    return crc_.finalize();
  }

private:
  int fd_;
  const std::filesystem::path &path_;
  std::uint64_t end_;
  std::vector<std::byte> buf_;
  std::uint64_t buf_at_{0}; // file offset of buf_[0]
  std::size_t len_{0};      // bytes of buf_ holding file data
  Crc32 crc_{};
};

// Opens a hint file for reading. One pass over the file checks the header,
// records where each unit starts, and checks the trailer at the end. Nothing
// the file says is handed out before every byte of it has been read and
// matched against the trailer: a damaged or unreadable hint throws here,
// before any entry reaches recovery, which is what lets recovery rebuild it
// with nothing to undo. The pass holds one chunk; the file is never held
// whole, and its descriptor is closed when the pass is done. Throws
// std::system_error on I/O failure and std::runtime_error on damage.
auto open_source(std::filesystem::path path)
    -> std::shared_ptr<const HintSource> {
  auto src = std::make_shared<HintSource>();
  src->path = std::move(path);
  const auto fd = open_read_only(src->path);
  // The size of the file this fd reads, not of whatever the path names now.
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0) {
    throw std::system_error{
        errno, std::generic_category(),
        std::format("HintFile: cannot stat '{}'", src->path.string())};
  }
#ifndef __APPLE__
  // The pass walks the file front to back.
  ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  const auto file_size = narrow<std::uint64_t>(st.st_size);
  if (file_size < kFileCrcSize) {
    throw std::runtime_error{std::format(
        "HintFile: '{}' is too small to contain a CRC trailer",
        src->path.string())};
  }
  const auto end = file_size - kFileCrcSize; // what the CRC covers
  OpenPass pass{fd.get(), src->path, end};

  const auto head = pass.window(0, kFramedHeaderSize);
  src->framed = head.size() >= kFramedMagic.size() &&
                std::ranges::equal(head.first(kFramedMagic.size()),
                                   kFramedMagic);
  std::uint64_t at = 0;
  if (src->framed) {
    if (head.size() < kFramedHeaderSize) {
      throw std::runtime_error{std::format(
          "HintFile: '{}' is too small for its header", src->path.string())};
    }
    const auto version = std::to_integer<std::uint8_t>(head[8]);
    const auto codec = std::to_integer<std::uint8_t>(head[9]);
    if (version != kFramedVersion || codec != kCodecZstd) {
      throw std::runtime_error{std::format(
          "HintFile: '{}' has unsupported version {} / codec {}",
          src->path.string(), version, codec)};
    }
    at = kFramedHeaderSize;
  }

  while (at < end) {
    src->units.push_back(at);
    if (src->framed) {
      const auto bytes = pass.window(at, kMaxPackedFrameBytes);
      const auto packed =
          ZSTD_findFrameCompressedSize(bytes.data(), bytes.size());
      if (ZSTD_isError(packed) || packed > kMaxPackedFrameBytes) {
        throw std::runtime_error{std::format(
            "HintFile: truncated or corrupt frame in '{}'",
            src->path.string())};
      }
      const auto size = ZSTD_getFrameContentSize(bytes.data(), packed);
      if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ||
          size > kMaxFrameBytes) {
        throw std::runtime_error{std::format(
            "HintFile: frame of invalid size in '{}'", src->path.string())};
      }
      at += packed;
    } else {
      // A raw file has no frames; cut it into units at entry boundaries.
      const auto start = at;
      do {
        const auto bytes = pass.window(at, kMaxHintEntryBytes);
        try {
          at += deserialize_entry(bytes).second;
        } catch (const std::runtime_error &e) {
          // The pass has not reached the trailer, so name the file: this is
          // the only report its damage gets.
          throw std::runtime_error{std::format(
              "HintFile: '{}' is damaged: {}", src->path.string(), e.what())};
        }
      } while (at < end && at - start < frame_target());
    }
  }
  src->units.push_back(end);

  std::array<std::byte, kFileCrcSize> trailer{};
  read_exact(fd.get(), trailer, end, src->path);
  const auto stored = read_le<std::uint32_t>(trailer, 0);
  const auto computed = src->framed ? ~pass.crc() : pass.crc();
  if (computed != stored) {
    throw std::runtime_error{
        std::format("HintFile: CRC mismatch in '{}'", src->path.string())};
  }
  return src;
}
} // namespace

#ifdef BYTECASK_TESTING
// Makes every hint file written afterwards cut frames at `bytes` instead of
// kHintFrameBytes; 0 restores the default. Tests use tiny frames so that
// every scan and merge crosses frame boundaries.
export void set_hint_frame_bytes_for_testing(std::size_t bytes) noexcept {
  g_frame_bytes_for_testing = bytes;
}
#endif

// Writer and reader for ByteCask hint files.
//
// Write mode (OpenForWrite): opens the file, writes the header, and buffers
// entries into frames, compressing and writing each frame as it fills. A
// running CRC-32C is accumulated over every byte written. close() writes the
// last frame and the trailer, calls fdatasync, and closes the fd.
//
// Read mode (OpenForRead): verifies the whole file against its trailer before
// returning, then hands out Scanners that read it a unit at a time with
// pread. Neither holds the file open between reads. Both layouts — framed,
// and the raw layout written before compression — are read.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class HintFile {
public:
  // Forward-only scanner over a hint file's entries. It shares its HintFile's
  // map of the file, so either may outlive the other.
  //
  // HintEntry.key and .end_key are valid until the scanner's next call to
  // next() or seek(): they point into the unit the scanner has decoded, which
  // the next unit overwrites. A reader that keeps an entry longer copies it
  // (HintRecord).
  class Scanner {
  public:
    // Where an entry starts: the index of the unit holding it — a frame, or a
    // run of a raw file's entries — and its offset inside the decoded unit.
    // Positions order like the entries they name.
    struct Position {
      std::size_t frame{};
      std::size_t offset{};
      auto operator<=>(const Position &) const = default;
    };

    explicit Scanner(std::shared_ptr<const HintSource> src)
        : src_{std::move(src)} {}

    // Returns the next entry, or nullopt at end of data.
    // Throws std::system_error on a failed read, std::runtime_error on a
    // truncated entry or a corrupt frame.
    [[nodiscard]] auto next() -> std::optional<HintEntry> {
      while (pos_ >= frame_.size()) {
        if (next_frame_ >= unit_count()) return std::nullopt;
        load_frame(next_frame_);
      }
      auto [he, consumed] = deserialize_entry(frame_.subspan(pos_));
      pos_ += consumed;
      return he;
    }

    // Where the next entry starts. seek() returns there; it must be a
    // position this scanner's file produced.
    [[nodiscard]] auto position() const noexcept -> Position {
      if (pos_ >= frame_.size() && next_frame_ < unit_count())
        return {next_frame_, 0};
      return {frame_at_, pos_};
    }

    void seek(Position p) {
      if (!loaded_ || p.frame != frame_at_) load_frame(p.frame);
      if (p.offset > frame_.size())
        throw std::runtime_error{"HintFile: seek past the end of a frame"};
      pos_ = p.offset;
    }

  private:
    [[nodiscard]] auto unit_count() const noexcept -> std::size_t {
      return src_->units.size() - 1;
    }

    // Reads unit i and decodes it into buf_. Its bounds, and a frame's
    // header, were checked when the file was opened.
    void load_frame(std::size_t i) {
      if (i >= unit_count()) {
        // The end of the units: where an empty file's scan stops.
        frame_ = {};
        frame_at_ = i;
        next_frame_ = unit_count();
        pos_ = 0;
        loaded_ = true;
        return;
      }
      const auto at = src_->units[i];
      const auto len = narrow<std::size_t>(src_->units[i + 1] - at);
      if (src_->framed) {
        auto &packed = thread_packed_frame();
        packed.resize(len);
        read_unit(packed, at);
        const auto size = ZSTD_getFrameContentSize(packed.data(), len);
        if (size == ZSTD_CONTENTSIZE_UNKNOWN ||
            size == ZSTD_CONTENTSIZE_ERROR || size > kMaxFrameBytes)
          throw std::runtime_error{"HintFile: frame of invalid size"};
        reset_buffer(static_cast<std::size_t>(size));
        const auto got = ZSTD_decompressDCtx(&thread_dctx(), buf_.data(),
                                             buf_.size(), packed.data(), len);
        if (ZSTD_isError(got) || got != size)
          throw std::runtime_error{"HintFile: frame does not decompress"};
      } else {
        reset_buffer(len);
        read_unit(buf_, at);
      }
      frame_ = buf_;
      frame_at_ = i;
      next_frame_ = i + 1;
      pos_ = 0;
      loaded_ = true;
    }

    // One read of the file, which is opened for it and closed after it.
    void read_unit(std::span<std::byte> out, std::uint64_t at) const {
      const auto fd = open_read_only(src_->path);
      read_exact(fd.get(), out, at, src_->path);
    }

    void reset_buffer(std::size_t size) {
#ifdef BYTECASK_TESTING
      // A fresh allocation per unit frees the previous one, so a reader that
      // kept an entry past its unit reads freed memory and ASan says so,
      // instead of it silently reading the next unit's bytes.
      buf_ = std::vector<std::byte>(size);
#else
      buf_.resize(size);
#endif
    }

    std::shared_ptr<const HintSource> src_;
    std::vector<std::byte> buf_;       // the decoded unit
    std::span<const std::byte> frame_; // entries being read, in buf_
    std::size_t frame_at_{0};          // Position::frame of frame_
    std::size_t next_frame_{0};        // the unit after frame_
    std::size_t pos_{0};               // next entry, inside frame_
    bool loaded_{false};
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
    HintFile hint{std::move(path), fd};
    hint.cctx_.reset(ZSTD_createCCtx());
    if (!hint.cctx_) throw std::bad_alloc{};
    std::array<std::byte, kFramedHeaderSize> header{};
    std::ranges::copy(kFramedMagic, header.begin());
    header[8] = std::byte{kFramedVersion};
    header[9] = std::byte{kCodecZstd};
    hint.write_bytes(header);
    return hint;
  }

  // Opens an existing hint file for reading, and reads and verifies all of it
  // before returning (open_source). Holds no descriptor afterwards: scanners
  // open the file for each unit they read.
  // Throws std::system_error on I/O failure, std::runtime_error on damage.
  [[nodiscard]] static auto OpenForRead(std::filesystem::path path)
      -> HintFile {
    auto src = open_source(path);
    return HintFile{std::move(path), std::move(src)};
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
        src_{std::move(other.src_)},
        write_fd_{std::exchange(other.write_fd_, -1)},
        crc_{other.crc_},
        cctx_{std::move(other.cctx_)},
        pending_{std::move(other.pending_)},
        packed_{std::move(other.packed_)} {}

  HintFile &operator=(HintFile &&other) noexcept {
    if (this != &other) {
      if (write_fd_ != -1) ::close(write_fd_);
      path_ = std::move(other.path_);
      src_ = std::move(other.src_);
      write_fd_ = std::exchange(other.write_fd_, -1);
      crc_ = other.crc_;
      cctx_ = std::move(other.cctx_);
      pending_ = std::move(other.pending_);
      packed_ = std::move(other.packed_);
    }
    return *this;
  }

  // Serializes one hint entry into the current frame.
  void append(std::uint64_t sequence, EntryType entry_type,
              std::uint64_t file_offset, std::span<const std::byte> key,
              std::uint32_t value_size) {
    add_entry(serialize_entry(sequence, entry_type, file_offset, value_size,
                              key));
  }

  // Serializes a RangeDel hint entry into the current frame.
  void append_range_del(std::uint64_t sequence, std::uint64_t file_offset,
                        std::span<const std::byte> start_key,
                        std::span<const std::byte> end_key) {
    add_entry(serialize_range_del_entry(sequence, file_offset, start_key,
                                        end_key));
  }

  // Writes the last frame and the trailer, calls fdatasync, and closes the
  // fd. Must be called exactly once on a write-mode HintFile.
  void close() {
    flush_frame();
    std::array<std::byte, kFileCrcSize> trailer{};
    ByteWriter w{trailer};
    w.put(static_cast<std::uint32_t>(~crc_.finalize()));
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_hint_write);
    // The data file this indexes must be durable first (PageCacheModel).
    FAULT_HINT_WRITTEN(path_);
#endif
    if (::write(write_fd_, trailer.data(), trailer.size()) !=
        std::ssize(trailer)) {
      const auto err = errno;
      ::close(write_fd_);
      write_fd_ = -1;
      throw std::system_error{err, std::generic_category(),
                              "HintFile::close: write CRC trailer failed"};
    }
    if (sync_hint(write_fd_) != 0) {
      const auto err = errno;
      ::close(write_fd_);
      write_fd_ = -1;
      throw std::system_error{err, std::generic_category(),
                              "HintFile::close: fdatasync failed"};
    }
    ::close(write_fd_);
    write_fd_ = -1;
  }

  // Returns a Scanner over the file's entries. Read mode only.
  [[nodiscard]] auto make_scanner() const -> Scanner {
    if (!src_)
      throw std::logic_error{"HintFile: make_scanner on a write-mode file"};
    return Scanner{src_};
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  // Write-mode constructor: holds the open fd.
  explicit HintFile(std::filesystem::path path, int fd)
      : path_{std::move(path)}, write_fd_{fd} {}

  // Read-mode constructor: holds the verified, open file.
  HintFile(std::filesystem::path path, std::shared_ptr<const HintSource> src)
      : path_{std::move(path)}, src_{std::move(src)} {}

  void add_entry(std::span<const std::byte> entry) {
    pending_.insert(pending_.end(), entry.begin(), entry.end());
    if (pending_.size() >= frame_target()) flush_frame();
  }

  // Compresses the buffered entries into one frame and writes it. zstd's
  // one-shot call records the decompressed size in the frame header, which
  // is what lets a reader size its buffer and find the frames without an
  // index stored in the file.
  void flush_frame() {
    if (pending_.empty()) return;
    packed_.resize(ZSTD_compressBound(pending_.size()));
    const auto n =
        ZSTD_compressCCtx(cctx_.get(), packed_.data(), packed_.size(),
                          pending_.data(), pending_.size(), kZstdLevel);
    if (ZSTD_isError(n)) {
      throw std::runtime_error{std::format(
          "HintFile: cannot compress a frame: {}", ZSTD_getErrorName(n))};
    }
    write_bytes(std::span{packed_}.first(n));
    pending_.clear();
  }

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

  std::filesystem::path path_;
  std::shared_ptr<const HintSource> src_; // read mode only
  int write_fd_{-1};             // write mode only; -1 when closed or read mode
  Crc32 crc_{};                  // write mode only; running CRC accumulator
  std::unique_ptr<ZSTD_CCtx, ZstdCCtxFree> cctx_; // write mode only
  std::vector<std::byte> pending_; // write mode: entries of the open frame
  std::vector<std::byte> packed_;  // write mode: the frame, compressed
};

// A hint entry that owns its key, for readers that keep an entry past the
// scanner's next call. assign() reuses the key buffer's capacity, so a cursor
// that holds one per key allocates only while its keys grow.
export struct HintRecord {
  std::uint64_t sequence{};
  EntryType entry_type{};
  std::uint64_t file_offset{};
  std::uint32_t value_size{};
  std::vector<std::byte> key;

  void assign(const HintEntry &he) {
    sequence = he.sequence;
    entry_type = he.entry_type;
    file_offset = he.file_offset;
    value_size = he.value_size;
    key.assign(he.key.begin(), he.key.end());
  }
};

} // namespace bytecask
