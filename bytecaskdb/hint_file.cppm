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
#include <sys/mman.h>
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

// Checks the trailer (and, for a framed file, the header) and returns the
// region the scanner reads: the frames, or a raw file's entries.
struct VerifiedHint {
  std::span<const std::byte> body;
  bool framed;
};
auto verify_hint(std::span<const std::byte> file,
                 const std::filesystem::path &path) -> VerifiedHint {
  if (file.size() < kFileCrcSize) {
    throw std::runtime_error{std::format(
        "HintFile: '{}' is too small to contain a CRC trailer",
        path.string())};
  }
  const auto framed =
      file.size() >= kFramedMagic.size() &&
      std::ranges::equal(file.first(kFramedMagic.size()), kFramedMagic);
  Crc32 crc{};
  crc.update(file.first(file.size() - kFileCrcSize));
  const auto computed = framed ? ~crc.finalize() : crc.finalize();
  const auto stored = read_le<std::uint32_t>(file, file.size() - kFileCrcSize);
  if (computed != stored) {
    throw std::runtime_error{
        std::format("HintFile: CRC mismatch in '{}'", path.string())};
  }
  if (!framed) {
    return {file.first(file.size() - kFileCrcSize), false};
  }
  if (file.size() < kFramedHeaderSize + kFileCrcSize) {
    throw std::runtime_error{
        std::format("HintFile: '{}' is too small for its header",
                    path.string())};
  }
  const auto version = std::to_integer<std::uint8_t>(file[8]);
  const auto codec = std::to_integer<std::uint8_t>(file[9]);
  if (version != kFramedVersion || codec != kCodecZstd) {
    throw std::runtime_error{std::format(
        "HintFile: '{}' has unsupported version {} / codec {}", path.string(),
        version, codec)};
  }
  return {file.subspan(kFramedHeaderSize,
                       file.size() - kFramedHeaderSize - kFileCrcSize),
          true};
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
// Read mode (OpenForRead, OpenForMerge): verifies the trailer before any
// parsing and exposes a Scanner. Both layouts — framed, and the raw layout
// written before compression — are read.
//
// Thread safety: NOT thread-safe. External synchronization is required.
export class HintFile {
public:
  // Forward-only scanner over a hint file's entries.
  //
  // HintEntry.key and .end_key are valid until the scanner's next call to
  // next() or seek(): in a framed file they point into the frame the scanner
  // has decoded, which the next frame overwrites. A reader that keeps an entry
  // longer copies it (HintRecord).
  class Scanner {
  public:
    // Where an entry starts: the frame holding it, as an offset into the
    // file's frame region, and its offset inside the decoded frame. A raw
    // file is one frame at 0. Positions order like the entries they name.
    struct Position {
      std::size_t frame{};
      std::size_t offset{};
      auto operator<=>(const Position &) const = default;
    };

    Scanner(std::span<const std::byte> body, bool framed)
        : body_{body}, framed_{framed} {
      if (!framed_) {
        frame_ = body_;
        next_frame_ = body_.size();
        loaded_ = true;
      }
    }

    // Returns the next entry, or nullopt at end of data.
    // Throws std::runtime_error on a truncated entry or a corrupt frame.
    [[nodiscard]] auto next() -> std::optional<HintEntry> {
      while (pos_ >= frame_.size()) {
        if (next_frame_ >= body_.size()) return std::nullopt;
        load_frame(next_frame_);
      }
      auto [he, consumed] = deserialize_entry(frame_.subspan(pos_));
      pos_ += consumed;
      return he;
    }

    // Where the next entry starts. seek() returns there; it must be a
    // position this scanner's file produced.
    [[nodiscard]] auto position() const noexcept -> Position {
      if (pos_ >= frame_.size() && next_frame_ < body_.size())
        return {next_frame_, 0};
      return {frame_at_, pos_};
    }

    void seek(Position p) {
      if (framed_ && (!loaded_ || p.frame != frame_at_)) load_frame(p.frame);
      if (p.offset > frame_.size())
        throw std::runtime_error{"HintFile: seek past the end of a frame"};
      pos_ = p.offset;
    }

  private:
    void load_frame(std::size_t at) {
      if (at >= body_.size()) {
        // The end of the frames: where an empty file's scan stops.
        frame_ = {};
        frame_at_ = at;
        next_frame_ = body_.size();
        pos_ = 0;
        loaded_ = true;
        return;
      }
      const auto src = body_.subspan(at);
      const auto packed = ZSTD_findFrameCompressedSize(src.data(), src.size());
      if (ZSTD_isError(packed))
        throw std::runtime_error{"HintFile: truncated or corrupt frame"};
      const auto size = ZSTD_getFrameContentSize(src.data(), packed);
      if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ||
          size > kMaxFrameBytes)
        throw std::runtime_error{"HintFile: frame of invalid size"};
#ifdef BYTECASK_TESTING
      // A fresh allocation per frame frees the previous one, so a reader
      // that kept an entry past the frame reads freed memory and ASan says
      // so, instead of it silently reading the next frame's bytes.
      buf_ = std::vector<std::byte>(static_cast<std::size_t>(size));
#else
      buf_.resize(static_cast<std::size_t>(size));
#endif
      const auto got = ZSTD_decompressDCtx(&thread_dctx(), buf_.data(),
                                           buf_.size(), src.data(), packed);
      if (ZSTD_isError(got) || got != size)
        throw std::runtime_error{"HintFile: frame does not decompress"};
      frame_ = buf_;
      frame_at_ = at;
      next_frame_ = at + packed;
      pos_ = 0;
      loaded_ = true;
    }

    std::span<const std::byte> body_; // non-owning; excludes header, trailer
    bool framed_;
    std::vector<std::byte> buf_;       // framed: the decoded frame
    std::span<const std::byte> frame_; // entries being read
    std::size_t frame_at_{0};          // Position::frame of frame_
    std::size_t next_frame_{0};        // where the following frame starts
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

  // Opens an existing hint file for reading. Reads the entire file into an
  // in-memory buffer in one syscall, then verifies the trailer before
  // returning. Throws on I/O failure or CRC mismatch.
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

    const auto verified = verify_hint(buf, path);
    const auto body_at = static_cast<std::size_t>(verified.body.data() -
                                                  buf.data());
    const auto body_size = verified.body.size();
    return HintFile{std::move(path), std::move(buf), body_at, body_size,
                    verified.framed};
  }

  // Read mode backed by a shared, read-only mapping instead of a heap copy.
  //
  // A k-way merge has to hold every file it merges open at once, and slurping
  // each one makes recovery's working set grow with the database: one worker
  // at recovery_threads = 1 owns every hint file there is. Mapping keeps the
  // bytes in the page cache, where they are file-backed and reclaimable, so
  // the anonymous memory recovery needs stays bounded by the tree it builds
  // and the one decoded frame each scanner holds.
  [[nodiscard]] static auto OpenForMerge(std::filesystem::path path)
      -> HintFile {
    auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile: cannot open '{}' for read", path.string())};
    }
    const auto file_sz = std::filesystem::file_size(path);
    if (file_sz < kFileCrcSize) {
      ::close(fd);
      throw std::runtime_error{std::format(
          "HintFile: '{}' is too small to contain a CRC trailer",
          path.string())};
    }
    // file_size() is 64-bit everywhere; std::size_t is 32-bit on wasm32, so
    // the mapping length needs checking rather than casting. Checked here
    // rather than through narrow<> because the fd has to be closed before
    // throwing.
    if (!std::in_range<std::size_t>(file_sz)) {
      ::close(fd);
      throw std::runtime_error{
          std::format("HintFile: '{}' is too large to map on this platform",
                      path.string())};
    }
    const auto map_size = static_cast<std::size_t>(file_sz);
    auto *addr = ::mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // the mapping keeps the file alive
    if (addr == MAP_FAILED) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("HintFile: cannot map '{}'", path.string())};
    }
    auto map = std::span<const std::byte>{
        static_cast<const std::byte *>(addr), map_size};
    // The merge walks each file front to back exactly once.
    ::madvise(addr, map_size, MADV_SEQUENTIAL);

    VerifiedHint verified;
    try {
      verified = verify_hint(map, path);
    } catch (...) {
      ::munmap(addr, map_size);
      throw;
    }
    const auto body_at =
        static_cast<std::size_t>(verified.body.data() - map.data());
    return HintFile{std::move(path), addr, map_size, body_at,
                    verified.body.size(), verified.framed};
  }

  ~HintFile() {
    if (map_ != nullptr) {
      ::munmap(map_, map_size_);
    }
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
        map_{other.map_},
        map_size_{other.map_size_},
        body_at_{other.body_at_},
        body_size_{other.body_size_},
        framed_{other.framed_},
        write_fd_{other.write_fd_},
        crc_{other.crc_},
        cctx_{std::move(other.cctx_)},
        pending_{std::move(other.pending_)},
        packed_{std::move(other.packed_)} {
    other.write_fd_ = -1;
    other.map_ = nullptr;
    other.map_size_ = 0;
  }

  HintFile &operator=(HintFile &&other) noexcept {
    if (this != &other) {
      if (write_fd_ != -1) ::close(write_fd_);
      if (map_ != nullptr) ::munmap(map_, map_size_);
      path_ = std::move(other.path_);
      buf_ = std::move(other.buf_);
      map_ = other.map_;
      map_size_ = other.map_size_;
      body_at_ = other.body_at_;
      body_size_ = other.body_size_;
      framed_ = other.framed_;
      write_fd_ = other.write_fd_;
      crc_ = other.crc_;
      cctx_ = std::move(other.cctx_);
      pending_ = std::move(other.pending_);
      packed_ = std::move(other.packed_);
      other.write_fd_ = -1;
      other.map_ = nullptr;
      other.map_size_ = 0;
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
#endif
    if (::write(write_fd_, trailer.data(), trailer.size()) !=
        std::ssize(trailer)) {
      const auto err = errno;
      ::close(write_fd_);
      write_fd_ = -1;
      throw std::system_error{err, std::generic_category(),
                              "HintFile::close: write CRC trailer failed"};
    }
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_hint_sync);
#endif
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

  // Returns a Scanner over the file's entries. The Scanner reads this
  // HintFile's buffer or mapping; this HintFile must outlive it.
  [[nodiscard]] auto make_scanner() const -> Scanner {
    return Scanner{view().subspan(body_at_, body_size_), framed_};
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  // Write-mode constructor: holds the open fd.
  explicit HintFile(std::filesystem::path path, int fd)
      : path_{std::move(path)}, write_fd_{fd} {}

  // Read-mode constructor: holds the file buffer.
  HintFile(std::filesystem::path path, std::vector<std::byte> buf,
           std::size_t body_at, std::size_t body_size, bool framed)
      : path_{std::move(path)}, buf_{std::move(buf)}, body_at_{body_at},
        body_size_{body_size}, framed_{framed} {}

  // Merge-mode constructor: holds the mapping, unmapped by the destructor.
  HintFile(std::filesystem::path path, void *addr, std::size_t size,
           std::size_t body_at, std::size_t body_size, bool framed)
      : path_{std::move(path)}, map_{addr}, map_size_{size},
        body_at_{body_at}, body_size_{body_size}, framed_{framed} {}

  void add_entry(std::span<const std::byte> entry) {
    pending_.insert(pending_.end(), entry.begin(), entry.end());
    if (pending_.size() >= frame_target()) flush_frame();
  }

  // Compresses the buffered entries into one frame and writes it. zstd's
  // one-shot call records the decompressed size in the frame header, which
  // is what lets a reader size its buffer and walk frames without an index.
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

  [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
    if (map_ != nullptr)
      return {static_cast<const std::byte *>(map_), map_size_};
    return {buf_.data(), buf_.size()};
  }

  std::filesystem::path path_;
  std::vector<std::byte> buf_;   // read mode only
  void *map_{nullptr};           // merge mode only; owns the mapping
  std::size_t map_size_{0};
  std::size_t body_at_{0};       // read modes: where the scanned region starts
  std::size_t body_size_{0};
  bool framed_{false};           // read modes: the file is zstd-framed
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
