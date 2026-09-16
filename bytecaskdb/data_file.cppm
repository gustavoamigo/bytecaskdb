// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — append-only data file writes and random-access reads

module;
#include <array>
#include <atomic>
#include <cassert>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <cerrno>
#include <concepts>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdio.h>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <algorithm>
#include <vector>
#include <ranges>

// macOS does not provide fdatasync; use F_FULLFSYNC which actually flushes
// to physical storage (fdatasync on macOS is a no-op shim).
#ifdef __APPLE__
static inline int portable_fdatasync(int fd) { return fcntl(fd, F_FULLFSYNC); }
#else
static inline int portable_fdatasync(int fd) { return fdatasync(fd); }
#endif

export module bytecask.data_file;

import bytecask.util;
import bytecask.buffer_pool;
import bytecask.data_entry;
import bytecask.types;

namespace bytecask {

// Byte offset into a data file, as returned by append() and consumed by read().
export using Offset = std::uint64_t;

// Reached only when an exclusive create loses to an existing file, which means
// the caller handed back a name the database already used. Opening it for write
// would silently adopt the sealed file's length and append past its end; those
// entries are then invisible to recovery, because hint generation skips any
// file whose hint already exists. Aborts rather than throws — see panic().
[[noreturn]] inline void panic_on_reused_path(
    const std::filesystem::path &path) {
  panic(std::format(
      "data file '{}' already exists. A new writable data file must never "
      "reuse a name; appending to an already-sealed file loses every appended "
      "entry at recovery.",
      path.string()));
}

// ---------------------------------------------------------------------------
// DataFile — abstract base for all data file implementations.
//
// Provides the read interface that the engine uses polymorphically.
// The file registry stores shared_ptr<DataFile>; readers call scan() and
// read_value() without knowing or caring whether the file is writable or
// read-only mmap-backed.
export class DataFile {
public:
  virtual ~DataFile();
  DataFile(const DataFile &) = delete;
  DataFile &operator=(const DataFile &) = delete;

  [[nodiscard]] virtual auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> = 0;

  virtual void read_value(Offset offset, std::uint16_t key_size,
                          std::uint32_t value_size, bool verify,
                          std::vector<std::byte> &io_buf,
                          std::vector<std::byte> &out) const = 0;

  [[nodiscard]] virtual auto size() const noexcept -> Offset = 0;

  // Reads full entry (header + key + value + CRC), verifies CRC.
  // key_size derived from on-disk header. Spans point into io_buf or mmap.
  [[nodiscard]] virtual auto read_entry(Offset offset, std::uint32_t value_size,
                                        std::vector<std::byte> &io_buf) const
      -> DataEntryView = 0;

  // Reads full entry without CRC verification. Spans point into mmap or io_buf.
  [[nodiscard]] virtual auto read_entry_unverified(
      Offset offset, std::uint32_t value_size,
      std::vector<std::byte> &io_buf) const -> DataEntryView = 0;

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

protected:
  explicit DataFile(std::filesystem::path path) : path_{std::move(path)} {}
  DataFile(DataFile &&) noexcept = default;
  DataFile &operator=(DataFile &&) noexcept = default;
  std::filesystem::path path_;
};

DataFile::~DataFile() = default;

// Writable data files are zero-filled this far ahead of the write cursor,
// never past their capacity. See WritableFileOps::ensure_zeroed.
export inline constexpr std::size_t kZeroFillChunkBytes = 4 * 1024 * 1024;

// ---------------------------------------------------------------------------
// WritableDataFile — pure interface for the write API.
//
// The engine's write path (execute_slots, ingest, vacuum, resume) calls
// append_entry / append_entries / sync / truncate on the active file.
// This interface makes the concrete writable implementation swappable.
export class WritableDataFile : public DataFile {
public:
  ~WritableDataFile() override;

  [[nodiscard]] virtual auto append_entry(
      std::uint64_t sequence, EntryType entry_type,
      std::span<const std::byte> key,
      std::span<const std::byte> value) -> Offset = 0;

  virtual void append_entries(std::span<const DataEntryView> entries,
                              std::span<Offset> offsets_out) = 0;

  virtual void sync() = 0;

  virtual void truncate(Offset new_size) = 0;

  // Releases the zero-filled tail: truncates the file to size() and syncs
  // the new length. Called once, when the file is sealed, so that a sealed
  // file's physical size is its logical size. Unlike truncate() this never
  // touches a mapping, so it is safe while readers hold snapshots of this
  // file: every published offset lies below size().
  virtual void shrink_to_fit() = 0;

protected:
  explicit WritableDataFile(std::filesystem::path path)
      : DataFile{std::move(path)} {}
};

WritableDataFile::~WritableDataFile() = default;

// ---------------------------------------------------------------------------
// WritableFileOps — shared write-path state and logic.
//
// Module-internal composition helper. Both WritableMmapDataFile and
// WritablePosixDataFile hold a WritableFileOps member and delegate write/scan
// operations to it. Read methods remain class-specific.
// ---------------------------------------------------------------------------

struct WritableFileOps {
  int fd_{-1};
  Offset offset_{0};       // logical end: next append lands here
  Offset zeroed_end_{0};   // physical end: zeros written through here
  std::size_t capacity_{0};  // zero-fill never extends past this (0 = off)
  std::array<std::byte, kHeaderSize + kCrcSize> hdr_crc_buf_{};

  [[nodiscard]] auto append_entry(std::uint64_t sequence, EntryType entry_type,
                                  std::span<const std::byte> key,
                                  std::span<const std::byte> value) -> Offset {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_data_file_append);
#endif
    const auto entry_offset = offset_;

    write_header_and_crc(hdr_crc_buf_, sequence, entry_type, key, value);

    const std::array<::iovec, 4> iov{{
        {hdr_crc_buf_.data(), kHeaderSize},
        {const_cast<std::byte *>(key.data()), key.size()},
        {const_cast<std::byte *>(value.data()), value.size()},
        {hdr_crc_buf_.data() + kHeaderSize, kCrcSize},
    }};
    const auto total = kHeaderSize + key.size() + value.size() + kCrcSize;
    ensure_zeroed(offset_ + static_cast<Offset>(total));

    const auto written = ::pwritev(fd_, iov.data(), std::ssize(iov),
                                   narrow<off_t>(offset_));
#ifdef BYTECASK_TESTING
    FAULT_INJECTION_POST_WRITE(io_data_file_append_partial,
                               fd_, entry_offset, total);
#endif
    if (written != narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableFileOps::append_entry: pwritev failed"};
    }

    offset_ += static_cast<Offset>(total);
    return entry_offset;
  }

  void append_entries(std::span<const DataEntryView> entries,
                      std::span<Offset> offsets_out) {
    assert(entries.size() == offsets_out.size());
    if (entries.empty()) return;

    static constexpr std::size_t kIovecsPerEntry = 4;
#ifdef BYTECASK_TESTING
    static constexpr std::size_t kMaxEntriesPerWritev = 2;
#else
    static constexpr std::size_t kMaxEntriesPerWritev =
        IOV_MAX / kIovecsPerEntry;
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
    thread_local std::vector<std::array<std::byte, kHeaderSize + kCrcSize>>
        hdr_crcs;
    thread_local std::vector<::iovec> iov;
#pragma clang diagnostic pop

    for (std::size_t base = 0; base < entries.size();
         base += kMaxEntriesPerWritev) {
      const auto chunk_end =
          std::min(base + kMaxEntriesPerWritev, entries.size());
      const auto chunk_size = chunk_end - base;

      hdr_crcs.resize(chunk_size);
      iov.resize(chunk_size * kIovecsPerEntry);

      std::size_t total_bytes = 0;
    #ifdef BYTECASK_TESTING
      std::size_t serialized = 0;
    #endif
      for (std::size_t i = 0; i < chunk_size; ++i) {
        const auto &e = entries[base + i];

#ifdef BYTECASK_TESTING
        testing_fault_injection_append(iov, serialized, total_bytes);
#endif

        offsets_out[base + i] = offset_ + static_cast<Offset>(total_bytes);

        write_header_and_crc(hdr_crcs[i], e.sequence, e.entry_type,
                             e.key, e.value);

        const auto iov_base = i * kIovecsPerEntry;
        iov[iov_base] = {hdr_crcs[i].data(), kHeaderSize};
        iov[iov_base + 1] = {const_cast<std::byte *>(e.key.data()),
                              e.key.size()};
        iov[iov_base + 2] = {const_cast<std::byte *>(e.value.data()),
                              e.value.size()};
        iov[iov_base + 3] = {hdr_crcs[i].data() + kHeaderSize, kCrcSize};

        total_bytes += kHeaderSize + e.key.size() + e.value.size() + kCrcSize;
#ifdef BYTECASK_TESTING
        ++serialized;
#endif
      }

      ensure_zeroed(offset_ + static_cast<Offset>(total_bytes));
      const auto written =
          ::pwritev(fd_, iov.data(), narrow<int>(chunk_size * kIovecsPerEntry),
                    narrow<off_t>(offset_));

#ifdef BYTECASK_TESTING
      FAULT_INJECTION_POST_WRITE(io_data_file_append_partial,
                                 fd_, offset_, total_bytes);
#endif
      if (written != narrow<ssize_t>(total_bytes)) {
        throw std::system_error{errno, std::generic_category(),
                                "WritableFileOps::append_entries: pwritev failed"};
      }

      offset_ += static_cast<Offset>(total_bytes);
    }
  }

  void sync() {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_data_file_sync);
#endif
    if (portable_fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableFileOps::sync: fdatasync failed"};
    }
  }

  [[nodiscard]] auto size() const noexcept -> Offset { return offset_; }

  // Keeps the file zero-filled ahead of the write cursor: before an append
  // ends at write_end, zeros are written from zeroed_end_ up to the next
  // kZeroFillChunkBytes boundary, never past capacity_ (an oversize entry
  // beyond it gets exactly what it needs). capacity_ == 0 turns this off —
  // tests and tooling that reopen files they wrote expect exact sizes.
  // Extents that were only allocated (fallocate) are *unwritten*; the
  // first write into each block converts
  // one, a journaled metadata change every fdatasync then waits for — on
  // ext4 roughly doubling the cost of each commit. Writing zeros converts a
  // whole chunk at once: the commit that crosses into a new chunk flushes
  // it (one journal commit, ~30 ms for 4 MiB on an SSD), and every other
  // commit in the chunk is a pure data flush. The zeros are not synced
  // here; the next sync covers them. A 1 MiB scratch buffer is enough,
  // since the cost is the page-cache memcpy, not the call count.
  void ensure_zeroed(Offset write_end) {
#ifdef __EMSCRIPTEN__
    // MEMFS holds files in memory: nothing to gain, and a zero tail would
    // be resident memory.
    (void)write_end;
#else
    if (capacity_ == 0 || write_end <= zeroed_end_) return;
    const auto chunk_end = (write_end + kZeroFillChunkBytes - 1) /
                           kZeroFillChunkBytes * kZeroFillChunkBytes;
    const auto target =
        std::max(write_end, std::min<Offset>(chunk_end, capacity_));
    static constexpr std::size_t kBuf = 1024 * 1024;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
    thread_local const std::vector<std::byte> zeros(kBuf, std::byte{0});
#pragma clang diagnostic pop
    for (auto off = zeroed_end_; off < target;) {
      const auto len = std::min<Offset>(kBuf, target - off);
      if (::pwrite(fd_, zeros.data(), len, narrow<off_t>(off)) !=
          narrow<ssize_t>(len)) {
        throw std::system_error{errno, std::generic_category(),
                                "WritableFileOps::ensure_zeroed: pwrite failed"};
      }
      off += len;
    }
    zeroed_end_ = target;
#endif
  }

  // See WritableDataFile::shrink_to_fit. ftruncate + fdatasync: the size
  // change is metadata fdatasync is required to persist.
  void shrink_to_fit() {
    if (::ftruncate(fd_, narrow<off_t>(offset_)) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableFileOps::shrink_to_fit: ftruncate failed"};
    }
    if (portable_fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableFileOps::shrink_to_fit: fdatasync failed"};
    }
    zeroed_end_ = offset_;
  }

  // Adopts the file's current length as both logical and physical end, and
  // fills the first chunk of a fresh file so its first commits are cheap.
  void adopt(Offset file_size, std::size_t capacity) {
    offset_ = file_size;
    zeroed_end_ = file_size;
    capacity_ = capacity;
    if (file_size == 0 && capacity > 0) ensure_zeroed(1);
  }

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> {
    if (offset >= offset_) {
      return std::nullopt;
    }
    auto header = scan_read_header(offset);
    if (header.sequence == 0) return std::nullopt;
    std::vector<std::byte> buf;
    auto view = scan_read_entry(offset, header.key_size, header.value_size, buf);
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
    return std::make_pair(
        DataEntry{.sequence = view.sequence, .entry_type = view.entry_type,
                  .key = {view.key.begin(), view.key.end()},
                  .value = {view.value.begin(), view.value.end()}},
        next);
  }

private:
  [[nodiscard]] auto scan_read_header(Offset offset) const -> EntryHeader {
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableFileOps::scan: pread header failed"};
    }
    return bytecask::read_header(std::span{hdr});
  }

  [[nodiscard]] auto scan_read_entry(Offset offset, std::uint16_t key_size,
                                     std::uint32_t value_size,
                                     std::vector<std::byte>& io_buf) const
      -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    io_buf.resize(total);
    if (::pread(fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
        narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableFileOps::scan: pread entry failed"};
    }
    std::span<const std::byte> raw = io_buf;
    const auto header = parse_header_and_verify(raw);
    auto body = raw.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }

#ifdef BYTECASK_TESTING
  void testing_fault_injection_append(std::span<const ::iovec> iov_buf,
                                      std::size_t serialized,
                                      std::size_t byte_count) {
    static constexpr std::size_t kIovecsPerEntry = 4;
    try {
      FAULT_INJECTION(io_data_file_append);
    } catch (...) {
      if (serialized > 0) {
        const auto written =
            ::pwritev(fd_, iov_buf.data(),
                      narrow<int>(serialized * kIovecsPerEntry),
                      narrow<off_t>(offset_));
        if (written == narrow<ssize_t>(byte_count)) {
          offset_ += static_cast<Offset>(byte_count);
        }
      }
      throw;
    }
  }
#endif
};

// ---------------------------------------------------------------------------
// WritableMmapDataFile — mmap-backed writable data file.
//
// Maps the file at a fixed capacity with MAP_SHARED; the file itself is
// zero-filled in chunks ahead of the write cursor (WritableFileOps::
// ensure_zeroed), so pages past zeroed_end_ are mapped but never touched.
// Writes go through pwritev; reads come from the mmap region (zero syscalls).
// When a read offset falls beyond the mmap region (rare: file grew past the
// pre-allocated size in degraded mode), reads fall back to pread.
//
// The mapping is established once, in the constructor, and unmapped once, in
// the destructor: its address is stable for the object's whole life, so a
// reader holding a span into it can never be left with a stale address.
// mmap_end_ is the only mutable part — the prefix of the mapping still backed
// by the file. truncate() lowers it so reads at or past the new end take the
// pread path (a clean short-read error) instead of faulting on a page beyond
// EOF.
//
// Thread safety: NOT thread-safe for writes (external synchronization required).
// Concurrent reads are safe: readers only access offsets published in the
// key directory after pwritev + fdatasync.
export class WritableMmapDataFile : public WritableDataFile {
public:
  [[nodiscard]] static auto create(std::filesystem::path path,
                                   std::size_t capacity,
                                   bool exclusive = false)
      -> std::shared_ptr<WritableDataFile> {
    return std::shared_ptr<WritableDataFile>(
        new WritableMmapDataFile{std::move(path), capacity, exclusive});
  }

  ~WritableMmapDataFile() override;

  [[nodiscard]] auto append_entry(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value) -> Offset override {
    return ops_.append_entry(sequence, entry_type, key, value);
  }

  void append_entries(std::span<const DataEntryView> entries,
                      std::span<Offset> offsets_out) override {
    ops_.append_entries(entries, offsets_out);
  }

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> override {
    return ops_.scan(offset);
  }

  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size, bool verify,
                  std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const override {
    if (verify) {
      auto view = read_entry_with_key_size(offset, key_size, value_size, io_buf);
      out.assign(view.value.begin(), view.value.end());
    } else {
      const auto val_offset = offset + kHeaderSize + key_size;
      if (val_offset + value_size <= mmap_end()) {
        auto *base = mmap_base_ + val_offset;
        out.assign(base, base + value_size);
      } else {
        out.resize(value_size);
        if (::pread(ops_.fd_, out.data(), value_size,
                    narrow<off_t>(val_offset)) != narrow<ssize_t>(value_size)) {
          throw std::system_error{
              errno, std::generic_category(),
              "WritableMmapDataFile::read_value: pread failed"};
        }
      }
    }
  }

  [[nodiscard]] auto read_entry(Offset offset, std::uint32_t value_size,
                                std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = read_header(offset);
    return read_entry_with_key_size(offset, hdr.key_size, value_size, io_buf);
  }

  [[nodiscard]] auto read_entry_unverified(
      Offset offset, std::uint32_t value_size,
      std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = read_header(offset);
    const auto body_size = hdr.key_size + value_size;
    if (offset + kHeaderSize + body_size <= mmap_end()) {
      auto body = std::span<const std::byte>{
          mmap_base_ + offset + kHeaderSize, body_size};
      return DataEntryView{
          .sequence = hdr.sequence,
          .entry_type = hdr.entry_type,
          .key = body.subspan(0, hdr.key_size),
          .value = body.subspan(hdr.key_size, value_size),
      };
    }
    io_buf.resize(body_size);
    if (::pread(ops_.fd_, io_buf.data(), body_size,
                narrow<off_t>(offset + kHeaderSize)) !=
        narrow<ssize_t>(body_size)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableMmapDataFile::read_entry_unverified: pread failed"};
    }
    std::span<const std::byte> body{io_buf};
    return DataEntryView{
        .sequence = hdr.sequence,
        .entry_type = hdr.entry_type,
        .key = body.subspan(0, hdr.key_size),
        .value = body.subspan(hdr.key_size, value_size),
    };
  }

  void sync() override { ops_.sync(); }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return ops_.size();
  }

  // Called by resume() while reads stay lock-free, so the mapping must not be
  // disturbed: a reader can be holding a span into it (EntryIterator hands
  // spans out until the next operator++). The mapping is left alone and only
  // mmap_end_ moves down — every published offset lies below new_size by
  // construction, and anything at or past it now takes the pread path.
  void truncate(Offset new_size) override {
    if (::ftruncate(ops_.fd_, narrow<off_t>(new_size)) != 0) {
      throw std::system_error{errno, std::system_category(),
                              "WritableMmapDataFile::truncate"};
    }
    ops_.offset_ = new_size;
    ops_.zeroed_end_ = new_size;
    set_mmap_end(new_size);
  }

  // Like truncate(), this never touches the mapping — see the class comment.
  void shrink_to_fit() override {
    ops_.shrink_to_fit();
    set_mmap_end(ops_.size());
  }

private:
  WritableMmapDataFile(std::filesystem::path path, std::size_t capacity,
                       bool exclusive)
      : WritableDataFile{std::move(path)} {
    ops_.fd_ = ::open(path_.c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : 0),
                      0644);
    if (ops_.fd_ == -1) {
      if (exclusive && errno == EEXIST) panic_on_reused_path(path_);
      throw std::system_error{
          errno, std::generic_category(),
          std::format("WritableMmapDataFile: cannot open '{}'", path_.string())};
    }
#ifndef __APPLE__
    ::posix_fadvise(ops_.fd_, 0, 0, POSIX_FADV_RANDOM);
#endif
    ops_.adopt(std::filesystem::file_size(path_), capacity);
    if (capacity > 0) {
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      auto *ptr = ::mmap(nullptr, capacity, PROT_READ, MAP_SHARED, ops_.fd_, 0);
      if (ptr == MAP_FAILED) {
        throw std::system_error{errno, std::generic_category(),
                                "WritableMmapDataFile: mmap failed"};
      }
      mmap_base_ = static_cast<std::byte *>(ptr);
      mmap_len_ = capacity;
      mmap_end_.store(capacity, std::memory_order_release);
      ::madvise(mmap_base_, mmap_len_, MADV_RANDOM);
    }
  }

  WritableFileOps ops_;
  std::byte *mmap_base_{nullptr};
  std::size_t mmap_len_{0};   // mapped length — fixed after construction
  // Upper bound for mapping-served reads; past it, reads take the pread path.
  // Not the file's length — a fresh mapping spans the whole capacity while the
  // file is only zero-filled ahead of the write cursor. What holds is that
  // every path shrinking the file lowers this with it. Read on the lock-free
  // read path while truncate() lowers it, so it is atomic.
  std::atomic<std::size_t> mmap_end_{0};

  [[nodiscard]] auto mmap_end() const noexcept -> std::size_t {
    return mmap_end_.load(std::memory_order_acquire);
  }

  // The file just shrank to file_size: clamp the readable prefix to it.
  void set_mmap_end(Offset file_size) noexcept {
    mmap_end_.store(std::min(mmap_len_, static_cast<std::size_t>(file_size)),
                    std::memory_order_release);
  }

  [[nodiscard]] auto read_header(Offset offset) const -> EntryHeader {
    if (offset + kHeaderSize <= mmap_end()) {
      return bytecask::read_header(
          std::span{mmap_base_ + offset, kHeaderSize});
    }
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(ops_.fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableMmapDataFile::read_header: pread failed"};
    }
    return bytecask::read_header(std::span{hdr});
  }

  [[nodiscard]] auto read_entry_with_key_size(
      Offset offset, std::uint16_t key_size, std::uint32_t value_size,
      std::vector<std::byte> &io_buf) const -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    if (offset + total <= mmap_end()) {
      std::span<const std::byte> raw{mmap_base_ + offset, total};
      const auto header = parse_header_and_verify(raw);
      auto body = raw.subspan(kHeaderSize);
      return DataEntryView{
          .sequence = header.sequence,
          .entry_type = header.entry_type,
          .key = body.subspan(0, key_size),
          .value = body.subspan(key_size, value_size),
      };
    }
    io_buf.resize(total);
    if (::pread(ops_.fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
        narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritableMmapDataFile::read_entry: pread failed"};
    }
    std::span<const std::byte> raw = io_buf;
    const auto header = parse_header_and_verify(raw);
    auto body = raw.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }
};

WritableMmapDataFile::~WritableMmapDataFile() {
  if (mmap_base_) {
    ::munmap(mmap_base_, mmap_len_);
  }
  if (ops_.fd_ != -1) {
    ::close(ops_.fd_);
  }
}

// ---------------------------------------------------------------------------
// WritablePosixDataFile — pread-based writable data file.
//
// Used on platforms without mmap (Emscripten) or when mmap is disabled.
// All reads go through pread syscalls. Writes use pwritev.
//
// Thread safety: NOT thread-safe for writes (external synchronization required).
// Concurrent reads are safe: readers only access offsets published in the
// key directory after pwritev + fdatasync.
export class WritablePosixDataFile : public WritableDataFile {
public:
  [[nodiscard]] static auto create(std::filesystem::path path,
                                   std::size_t capacity,
                                   bool exclusive = false)
      -> std::shared_ptr<WritableDataFile> {
    return std::shared_ptr<WritableDataFile>(
        new WritablePosixDataFile{std::move(path), capacity, exclusive});
  }

  ~WritablePosixDataFile() override;

  [[nodiscard]] auto append_entry(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value) -> Offset override {
    return ops_.append_entry(sequence, entry_type, key, value);
  }

  void append_entries(std::span<const DataEntryView> entries,
                      std::span<Offset> offsets_out) override {
    ops_.append_entries(entries, offsets_out);
  }

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> override {
    return ops_.scan(offset);
  }

  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size, bool verify,
                  std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const override {
    if (verify) {
      auto view = read_entry_with_key_size(offset, key_size, value_size, io_buf);
      out.assign(view.value.begin(), view.value.end());
    } else {
      const auto val_offset = offset + kHeaderSize + key_size;
      out.resize(value_size);
      if (::pread(ops_.fd_, out.data(), value_size,
                  narrow<off_t>(val_offset)) != narrow<ssize_t>(value_size)) {
        throw std::system_error{
            errno, std::generic_category(),
            "WritablePosixDataFile::read_value: pread failed"};
      }
    }
  }

  [[nodiscard]] auto read_entry(Offset offset, std::uint32_t value_size,
                                std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = read_header(offset);
    return read_entry_with_key_size(offset, hdr.key_size, value_size, io_buf);
  }

  [[nodiscard]] auto read_entry_unverified(
      Offset offset, std::uint32_t value_size,
      std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    static constexpr std::size_t kKeyBudget = 256;
    const auto speculative_total = kHeaderSize + kKeyBudget + value_size + kCrcSize;
    io_buf.resize(speculative_total);
    auto bytes_read = ::pread(ops_.fd_, io_buf.data(), speculative_total,
                             narrow<off_t>(offset));
    if (bytes_read < narrow<ssize_t>(kHeaderSize)) {
      throw std::system_error{errno, std::generic_category(),
                              "WritablePosixDataFile::read_entry_unverified: pread failed"};
    }
    auto hdr = bytecask::read_header(
        std::span<const std::byte>{io_buf.data(), kHeaderSize});
    const auto total = kHeaderSize + hdr.key_size + value_size + kCrcSize;
    if (total > speculative_total) {
      io_buf.resize(total);
      if (::pread(ops_.fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
          narrow<ssize_t>(total)) {
        throw std::system_error{errno, std::generic_category(),
                                "WritablePosixDataFile::read_entry_unverified: pread failed"};
      }
    }
    auto body = std::span<const std::byte>{io_buf.data() + kHeaderSize,
                                           hdr.key_size + value_size};
    return DataEntryView{
        .sequence = hdr.sequence,
        .entry_type = hdr.entry_type,
        .key = body.subspan(0, hdr.key_size),
        .value = body.subspan(hdr.key_size, value_size),
    };
  }

  void sync() override { ops_.sync(); }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return ops_.size();
  }

  void truncate(Offset new_size) override {
    if (::ftruncate(ops_.fd_, narrow<off_t>(new_size)) != 0) {
      throw std::system_error{errno, std::system_category(),
                              "WritablePosixDataFile::truncate"};
    }
    ops_.offset_ = new_size;
    ops_.zeroed_end_ = new_size;
  }

  void shrink_to_fit() override { ops_.shrink_to_fit(); }

private:
  WritablePosixDataFile(std::filesystem::path path, std::size_t capacity,
                        bool exclusive)
      : WritableDataFile{std::move(path)} {
    ops_.fd_ = ::open(path_.c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : 0),
                      0644);
    if (ops_.fd_ == -1) {
      if (exclusive && errno == EEXIST) panic_on_reused_path(path_);
      throw std::system_error{
          errno, std::generic_category(),
          std::format("WritablePosixDataFile: cannot open '{}'", path_.string())};
    }
#ifndef __APPLE__
    ::posix_fadvise(ops_.fd_, 0, 0, POSIX_FADV_RANDOM);
#endif
    ops_.adopt(std::filesystem::file_size(path_), capacity);
  }

  WritableFileOps ops_;

  [[nodiscard]] auto read_header(Offset offset) const -> EntryHeader {
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(ops_.fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{
          errno, std::generic_category(),
          "WritablePosixDataFile::read_header: pread failed"};
    }
    return bytecask::read_header(std::span{hdr});
  }

  [[nodiscard]] auto read_entry_with_key_size(Offset offset,
                                              std::uint16_t key_size,
                                              std::uint32_t value_size,
                                              std::vector<std::byte> &io_buf) const
      -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    io_buf.resize(total);
    if (::pread(ops_.fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
        narrow<ssize_t>(total)) {
      throw std::system_error{
          errno, std::generic_category(),
          "WritablePosixDataFile::read_entry: pread failed"};
    }
    const auto header = parse_header_and_verify(io_buf);
    auto body = std::span<const std::byte>{io_buf}.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }

};

WritablePosixDataFile::~WritablePosixDataFile() {
  if (ops_.fd_ != -1) {
    ::close(ops_.fd_);
  }
}

// ---------------------------------------------------------------------------
// ReadOnlyPosixDataFile — pread-based read-only data file.
//
// Used on platforms without mmap (Emscripten) or for empty files where mmap
// is not possible. All reads go through pread syscalls.
export class ReadOnlyPosixDataFile : public DataFile {
public:
  [[nodiscard]] static auto openForRead(std::filesystem::path path)
      -> std::shared_ptr<ReadOnlyPosixDataFile> {
    auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("ReadOnlyPosixDataFile: cannot open '{}'",
                      path.string())};
    }
    struct stat st {};
    std::size_t file_size = 0;
    if (::fstat(fd, &st) == 0) {
      file_size = static_cast<std::size_t>(st.st_size);
    }
    return std::shared_ptr<ReadOnlyPosixDataFile>(
        new ReadOnlyPosixDataFile{std::move(path), fd, file_size});
  }

  ~ReadOnlyPosixDataFile() override;

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> override {
    if (offset + kHeaderSize > file_size_) {
      return std::nullopt;
    }
    const auto header = read_header(offset);
    if (header.sequence == 0) return std::nullopt;
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
    if (next > file_size_) {
      return std::nullopt;
    }
    std::vector<std::byte> buf;
    auto view = read_entry_with_key_size(offset, header.key_size, header.value_size, buf);
    return std::make_pair(
        DataEntry{.sequence = view.sequence, .entry_type = view.entry_type,
                  .key = {view.key.begin(), view.key.end()},
                  .value = {view.value.begin(), view.value.end()}},
        next);
  }

  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size, bool verify,
                  std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const override {
    if (verify) {
      auto view = read_entry_with_key_size(offset, key_size, value_size, io_buf);
      out.assign(view.value.begin(), view.value.end());
    } else {
      const auto val_offset = offset + kHeaderSize + key_size;
      out.resize(value_size);
      if (::pread(fd_, out.data(), value_size,
                  narrow<off_t>(val_offset)) != narrow<ssize_t>(value_size)) {
        throw std::system_error{
            errno, std::generic_category(),
            "ReadOnlyPosixDataFile::read_value: pread failed"};
      }
    }
  }

  [[nodiscard]] auto read_entry(Offset offset, std::uint32_t value_size,
                                std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = read_header(offset);
    return read_entry_with_key_size(offset, hdr.key_size, value_size, io_buf);
  }

  [[nodiscard]] auto read_entry_unverified(
      Offset offset, std::uint32_t value_size,
      std::vector<std::byte> &io_buf) const -> DataEntryView override {
    static constexpr std::size_t kKeyBudget = 256;
    const auto speculative_total = kHeaderSize + kKeyBudget + value_size + kCrcSize;
    io_buf.resize(speculative_total);
    auto bytes_read = ::pread(fd_, io_buf.data(), speculative_total,
                             narrow<off_t>(offset));
    if (bytes_read < narrow<ssize_t>(kHeaderSize)) {
      throw std::system_error{errno, std::generic_category(),
                              "ReadOnlyPosixDataFile::read_entry_unverified: pread failed"};
    }
    auto hdr = bytecask::read_header(
        std::span<const std::byte>{io_buf.data(), kHeaderSize});
    const auto total = kHeaderSize + hdr.key_size + value_size + kCrcSize;
    if (total > speculative_total) {
      io_buf.resize(total);
      if (::pread(fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
          narrow<ssize_t>(total)) {
        throw std::system_error{errno, std::generic_category(),
                                "ReadOnlyPosixDataFile::read_entry_unverified: pread failed"};
      }
    }
    auto body = std::span<const std::byte>{io_buf.data() + kHeaderSize,
                                           hdr.key_size + value_size};
    return DataEntryView{
        .sequence = hdr.sequence,
        .entry_type = hdr.entry_type,
        .key = body.subspan(0, hdr.key_size),
        .value = body.subspan(hdr.key_size, value_size),
    };
  }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return static_cast<Offset>(file_size_);
  }

private:
  ReadOnlyPosixDataFile(std::filesystem::path path, int fd,
                        std::size_t file_size)
      : DataFile{std::move(path)}, fd_{fd}, file_size_{file_size} {}

  int fd_;
  std::size_t file_size_;

  [[nodiscard]] auto read_header(Offset offset) const -> EntryHeader {
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{
          errno, std::generic_category(),
          "ReadOnlyPosixDataFile::read_header: pread failed"};
    }
    return bytecask::read_header(std::span{hdr});
  }

  [[nodiscard]] auto read_entry_with_key_size(Offset offset,
                                              std::uint16_t key_size,
                                              std::uint32_t value_size,
                                              std::vector<std::byte> &io_buf) const
      -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    io_buf.resize(total);
    if (::pread(fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
        narrow<ssize_t>(total)) {
      throw std::system_error{
          errno, std::generic_category(),
          "ReadOnlyPosixDataFile::read_entry: pread failed"};
    }
    const auto header = parse_header_and_verify(io_buf);
    auto body = std::span<const std::byte>{io_buf}.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }
};

ReadOnlyPosixDataFile::~ReadOnlyPosixDataFile() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

// ---------------------------------------------------------------------------
// ReadOnlyMmapDataFile — mmap-backed read-only data file.
//
// Created from sealed data files. All reads are served directly from the
// memory-mapped region with no syscall overhead. The file must be non-empty.
export class ReadOnlyMmapDataFile : public DataFile {
public:
  [[nodiscard]] static auto openForRead(std::filesystem::path path)
      -> std::shared_ptr<ReadOnlyMmapDataFile> {
    auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("ReadOnlyMmapDataFile: cannot open '{}'",
                      path.string())};
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size == 0) {
      ::close(fd);
      throw std::system_error{
          errno, std::generic_category(),
          std::format("ReadOnlyMmapDataFile: file empty or fstat failed '{}'",
                      path.string())};
    }

    auto mmap_size = static_cast<std::size_t>(st.st_size);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *ptr = ::mmap(nullptr, mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) {
      ::close(fd);
      throw std::system_error{
          errno, std::generic_category(),
          std::format("ReadOnlyMmapDataFile: mmap failed '{}'",
                      path.string())};
    }
    auto *mmap_base = static_cast<std::byte *>(ptr);
    ::madvise(mmap_base, mmap_size, MADV_RANDOM);

    return std::shared_ptr<ReadOnlyMmapDataFile>(
        new ReadOnlyMmapDataFile{std::move(path), fd, mmap_base, mmap_size});
  }

  ~ReadOnlyMmapDataFile() override;

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> override {
    if (offset + kHeaderSize > mmap_size_) {
      return std::nullopt;
    }
    const auto header = read_header(offset);
    if (header.sequence == 0) return std::nullopt;
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
    if (next > mmap_size_) {
      return std::nullopt;
    }
    auto view = read_entry_with_key_size(offset, header.key_size, header.value_size);
    return std::make_pair(
        DataEntry{.sequence = view.sequence, .entry_type = view.entry_type,
                  .key = {view.key.begin(), view.key.end()},
                  .value = {view.value.begin(), view.value.end()}},
        next);
  }

  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size, bool verify,
                  [[maybe_unused]] std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const override {
    if (verify) {
      auto view = read_entry_with_key_size(offset, key_size, value_size);
      out.assign(view.value.begin(), view.value.end());
    } else {
      const auto val_offset = offset + kHeaderSize + key_size;
      assert(val_offset + value_size <= mmap_size_);
      auto *base = mmap_base_ + val_offset;
      out.assign(base, base + value_size);
    }
  }

  [[nodiscard]] auto read_entry(Offset offset, std::uint32_t value_size,
                                [[maybe_unused]] std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = read_header(offset);
    return read_entry_with_key_size(offset, hdr.key_size, value_size);
  }

  [[nodiscard]] auto read_entry_unverified(
      Offset offset, std::uint32_t value_size,
      [[maybe_unused]] std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = bytecask::read_header(std::span{mmap_base_ + offset, kHeaderSize});
    auto body = std::span{mmap_base_ + offset + kHeaderSize,
                          hdr.key_size + value_size};
    return DataEntryView{
        .sequence = hdr.sequence,
        .entry_type = hdr.entry_type,
        .key = body.subspan(0, hdr.key_size),
        .value = body.subspan(hdr.key_size, value_size),
    };
  }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return static_cast<Offset>(mmap_size_);
  }

private:
  ReadOnlyMmapDataFile(std::filesystem::path path, int fd,
                       std::byte *mmap_base, std::size_t mmap_size)
      : DataFile{std::move(path)}, fd_{fd},
        mmap_base_{mmap_base}, mmap_size_{mmap_size} {}

  int fd_;
  std::byte *mmap_base_;
  std::size_t mmap_size_;

  [[nodiscard]] auto read_header(Offset offset) const -> EntryHeader {
    assert(offset + kHeaderSize <= mmap_size_);
    return bytecask::read_header(std::span{mmap_base_ + offset, kHeaderSize});
  }

  [[nodiscard]] auto read_entry_with_key_size(Offset offset,
                                              std::uint16_t key_size,
                                              std::uint32_t value_size) const
      -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    assert(offset + total <= mmap_size_);
    std::span<const std::byte> raw{mmap_base_ + offset, total};
    const auto header = parse_header_and_verify(raw);
    auto body = raw.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }
};

ReadOnlyMmapDataFile::~ReadOnlyMmapDataFile() {
  ::munmap(mmap_base_, mmap_size_);
  if (fd_ != -1) {
    ::close(fd_);
  }
}

// ---------------------------------------------------------------------------
// ReadOnlyBufferPoolDataFile — sealed file served through the buffer pool.
//
// Byte fetching goes to BufferPool::read_at; parsing and CRC verification use
// the same free functions as every other back-end, so the decoded result is
// identical by construction. The pool is owned by the DB and outlives every
// file registered with it. Frames are keyed by the engine's file_id, which the
// caller reserves before opening the file — see TransientEngineState::
// reserve_file_id, which exists so vacuum can supply one here.
//
// Spans returned by read_entry / read_entry_unverified point into the caller's
// io_buf, exactly as ReadOnlyPosixDataFile does — never into a frame. A frame
// is reused memory, so a span into one would dangle the moment it was evicted,
// and an EntryIterator holds its span across the user's whole loop body.
export class ReadOnlyBufferPoolDataFile : public DataFile {
public:
  [[nodiscard]] static auto openForRead(std::filesystem::path path,
                                        std::uint32_t file_id, BufferPool &pool)
      -> std::shared_ptr<ReadOnlyBufferPoolDataFile> {
    auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("ReadOnlyBufferPoolDataFile: cannot open '{}'",
                      path.string())};
    }
    struct stat st {};
    std::size_t file_size = 0;
    if (::fstat(fd, &st) == 0) {
      file_size = static_cast<std::size_t>(st.st_size);
    }
    return std::shared_ptr<ReadOnlyBufferPoolDataFile>(
        new ReadOnlyBufferPoolDataFile{std::move(path), fd, file_size, file_id,
                                       pool});
  }

  ~ReadOnlyBufferPoolDataFile() override;

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> override {
    if (offset + kHeaderSize > file_size_) {
      return std::nullopt;
    }
    const auto header = read_header(offset, Source::Direct);
    if (header.sequence == 0) return std::nullopt;
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
    if (next > file_size_) {
      return std::nullopt;
    }
    std::vector<std::byte> buf;
    auto view = read_entry_with_key_size(offset, header.key_size,
                                         header.value_size, buf,
                                         Source::Direct);
    return std::make_pair(
        DataEntry{.sequence = view.sequence, .entry_type = view.entry_type,
                  .key = {view.key.begin(), view.key.end()},
                  .value = {view.value.begin(), view.value.end()}},
        next);
  }

  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size, bool verify,
                  std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const override {
    if (verify) {
      auto view = read_entry_with_key_size(offset, key_size, value_size,
                                           io_buf, Source::Pool);
      out.assign(view.value.begin(), view.value.end());
    } else {
      const auto val_offset = offset + kHeaderSize + key_size;
      out.resize(value_size);
      fetch(val_offset, value_size, out.data(), Source::Pool);
    }
  }

  [[nodiscard]] auto read_entry(Offset offset, std::uint32_t value_size,
                                std::vector<std::byte> &io_buf) const
      -> DataEntryView override {
    auto hdr = read_header(offset, Source::Pool);
    return read_entry_with_key_size(offset, hdr.key_size, value_size, io_buf,
                                    Source::Pool);
  }

  // No speculative over-read here, unlike the pread back-end: that exists to
  // save a second syscall, and a pool hit has no syscall to save. Reading the
  // header first is both simpler and usually free — it lands in the same frame.
  [[nodiscard]] auto read_entry_unverified(
      Offset offset, std::uint32_t value_size,
      std::vector<std::byte> &io_buf) const -> DataEntryView override {
    const auto hdr = read_header(offset, Source::Pool);
    const auto total = kHeaderSize + hdr.key_size + value_size + kCrcSize;
    io_buf.resize(total);
    fetch(offset, total, io_buf.data(), Source::Pool);
    auto body = std::span<const std::byte>{io_buf.data() + kHeaderSize,
                                           hdr.key_size + value_size};
    return DataEntryView{
        .sequence = hdr.sequence,
        .entry_type = hdr.entry_type,
        .key = body.subspan(0, hdr.key_size),
        .value = body.subspan(hdr.key_size, value_size),
    };
  }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return static_cast<Offset>(file_size_);
  }

private:
  ReadOnlyBufferPoolDataFile(std::filesystem::path path, int fd,
                             std::size_t file_size, std::uint32_t file_id,
                             BufferPool &pool)
      : DataFile{std::move(path)}, fd_{fd}, file_size_{file_size},
        file_id_{file_id}, pool_{&pool} {}

  int fd_;
  std::size_t file_size_;
  std::uint32_t file_id_;
  BufferPool *pool_;

  // Point reads go through the pool; scans deliberately do not. scan() sweeps
  // a whole file once — vacuum, hint generation, create_manifest — and
  // admitting those frames would flush the working set on every vacuum pass
  // (design §7). Making the source an explicit argument means a new read path
  // has to choose rather than inherit whichever default was nearest.
  enum class Source { Pool, Direct };

  void fetch(Offset offset, std::size_t len, std::byte *dst,
             Source source) const {
    if (source == Source::Pool) {
      pool_->read_at(file_id_, fd_, offset, len, file_size_, dst);
      return;
    }
    std::size_t done = 0;
    while (done < len) {
      const auto n = ::pread(fd_, dst + done, len - done,
                             narrow<off_t>(offset + done));
      if (n <= 0) {
        throw std::system_error{
            errno, std::generic_category(),
            "ReadOnlyBufferPoolDataFile: pread failed"};
      }
      done += static_cast<std::size_t>(n);
    }
  }

  [[nodiscard]] auto read_header(Offset offset, Source source) const
      -> EntryHeader {
    std::array<std::byte, kHeaderSize> hdr{};
    fetch(offset, kHeaderSize, hdr.data(), source);
    return bytecask::read_header(std::span{hdr});
  }

  [[nodiscard]] auto read_entry_with_key_size(
      Offset offset, std::uint16_t key_size, std::uint32_t value_size,
      std::vector<std::byte> &io_buf, Source source) const -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    io_buf.resize(total);
    fetch(offset, total, io_buf.data(), source);
    const auto header = parse_header_and_verify(io_buf);
    auto body = std::span<const std::byte>{io_buf}.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }
};

ReadOnlyBufferPoolDataFile::~ReadOnlyBufferPoolDataFile() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

// Generic factory: returns the read-only DataFile for the selected back-end.
// BufferPool cannot reach here — DB::open rejects it until the pool lands.
export [[nodiscard]] inline auto openDataFileForRead(
    std::filesystem::path path, IoBackend backend = IoBackend::Pread,
    BufferPool *pool = nullptr, std::uint32_t file_id = 0)
    -> std::shared_ptr<DataFile> {
#ifndef __EMSCRIPTEN__
  if (backend == IoBackend::BufferPool) {
    if (pool == nullptr) {
      // Falling back to pread would make the configured bound quietly
      // meaningless, which is the whole point of the subsystem.
      throw std::logic_error{
          "openDataFileForRead: IoBackend::BufferPool requires a pool"};
    }
    return ReadOnlyBufferPoolDataFile::openForRead(std::move(path), file_id,
                                                   *pool);
  }
  if (backend == IoBackend::Mmap) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) {
      return ReadOnlyMmapDataFile::openForRead(std::move(path));
    }
  }
#endif
  return ReadOnlyPosixDataFile::openForRead(std::move(path));
}

// Factory for writable data files: mmap-backed when requested, pread-based otherwise.
// Opens path for write, creating it if absent and adopting its current length
// if present. The engine never uses this to create a new file — see
// createDataFileForWrite — but tests and tooling reopen a file they wrote.
export [[nodiscard]] inline auto openDataFileForWrite(
    std::filesystem::path path, std::size_t capacity, IoBackend backend)
    -> std::shared_ptr<WritableDataFile> {
#ifndef __EMSCRIPTEN__
  if (backend == IoBackend::Mmap && capacity > 0) {
    return WritableMmapDataFile::create(std::move(path), capacity);
  }
#endif
  return WritablePosixDataFile::create(std::move(path), capacity);
}

// Creates the one writable data file for stem in dir: "<stem>.data", or
// "<stem>.data.tmp" for vacuum's staging copy. This is how the engine creates
// every data file it writes to.
//
// Panics if the name is already in use — either "<stem><suffix>" exists
// (rejected by O_EXCL, race-free against a concurrent creator) or a
// "<stem>.hint" was already written, which marks the stem as sealed. Both mean
// the stem generator returned a name this database already used. Neither is
// recoverable: the two guards are a pair, so they live in one function that a
// caller cannot half-apply.
export [[nodiscard]] inline auto createDataFileForWrite(
    const std::filesystem::path &dir, const std::string &stem,
    std::string_view suffix, std::size_t capacity, IoBackend backend)
    -> std::shared_ptr<WritableDataFile> {
  const auto hint_path = dir / (stem + ".hint");
  // error_code overload: the question is "is this stem taken", and a stat
  // failure is not an answer to it — only an existing hint is.
  std::error_code hint_ec;
  if (std::filesystem::exists(hint_path, hint_ec)) {
    panic(std::format(
        "data file stem '{}' is already sealed: '{}' exists. Hint generation "
        "skips a file whose hint is already written, so every entry appended "
        "to it would be lost at recovery.",
        stem, hint_path.string()));
  }
  auto path = dir / (stem + std::string{suffix});
#ifndef __EMSCRIPTEN__
  if (backend == IoBackend::Mmap && capacity > 0) {
    return WritableMmapDataFile::create(std::move(path), capacity,
                                        /*exclusive=*/true);
  }
#endif
  return WritablePosixDataFile::create(std::move(path), capacity,
                                       /*exclusive=*/true);
}

// Moves a staged data file onto its final name, refusing to replace an
// existing target. std::filesystem::rename replaces silently, and the target
// is minted by the same stem generator as every other data file — so the
// replacement it would perform is a stem reuse destroying a live file.
//
// Checking the target first and then renaming is not equivalent: vacuum stages
// its copy while writers keep rotating, so a stem can appear in that window.
// The refusal has to be part of the placement itself.
//
// Panics on a name already in use, for the same reason createDataFileForWrite
// does; any other failure is an ordinary I/O error and throws.
export void renameDataFileExclusive(const std::filesystem::path &from,
                                    const std::filesystem::path &to) {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
  if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                  RENAME_NOREPLACE) == 0) {
    return;
  }
  if (errno == EEXIST) panic_on_reused_path(to);
  // Filesystems that do not implement the flag report EINVAL or ENOSYS; those
  // fall through to the portable path below. Anything else is a real error.
  if (errno != EINVAL && errno != ENOSYS) {
    throw std::system_error{
        errno, std::generic_category(),
        std::format("renameDataFileExclusive: cannot rename '{}' to '{}'",
                    from.string(), to.string())};
  }
#endif
  // link() fails with EEXIST instead of replacing, so the target is claimed
  // atomically. A crash between link and unlink leaves the staged file behind;
  // recovery removes stale .tmp files at open.
  if (::link(from.c_str(), to.c_str()) != 0) {
    if (errno == EEXIST) panic_on_reused_path(to);
    throw std::system_error{
        errno, std::generic_category(),
        std::format("renameDataFileExclusive: cannot link '{}' to '{}'",
                    from.string(), to.string())};
  }
  if (::unlink(from.c_str()) != 0) {
    throw std::system_error{
        errno, std::generic_category(),
        std::format("renameDataFileExclusive: cannot unlink '{}'",
                    from.string())};
  }
}

// Forward-only iterator over raw entries in a DataFile.
// Wraps DataFile::scan(offset) into a standard C++ input iterator.
// Exceptions from scan() (CRC errors, I/O failures) propagate to the caller.
export class DataFileIterator {
public:
  using iterator_concept = std::input_iterator_tag;
  using value_type = std::pair<DataEntry, Offset>;
  using difference_type = std::ptrdiff_t;

  DataFileIterator() = default;

  explicit DataFileIterator(const DataFile& file, Offset start = 0)
      : file_{&file}, next_offset_{start} {
    advance();
  }

  auto operator*() const -> const value_type& { return *cached_; }

  auto operator++() -> DataFileIterator& {
    advance();
    return *this;
  }

  void operator++(int) { ++*this; }

  auto operator==(std::default_sentinel_t) const noexcept -> bool {
    return !cached_.has_value();
  }

  [[nodiscard]] auto next_offset() const noexcept -> Offset {
    return next_offset_;
  }

private:
  void advance() {
    auto result = file_->scan(next_offset_);
    if (!result) {
      cached_.reset();
      return;
    }
    auto& [entry, next] = *result;
    cached_.emplace(std::move(entry), next_offset_);
    next_offset_ = next;
  }

  const DataFile* file_{};
  Offset next_offset_{};
  std::optional<value_type> cached_;
};

export inline auto scan_entries(const DataFile& file, Offset start = 0)
    -> std::ranges::subrange<DataFileIterator, std::default_sentinel_t> {
  return {DataFileIterator{file, start}, std::default_sentinel};
}

} // namespace bytecask
