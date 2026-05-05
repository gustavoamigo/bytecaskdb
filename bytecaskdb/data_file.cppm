// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — append-only data file writes and random-access reads

module;
#include <array>
#include <cassert>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <cerrno>
#include <concepts>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
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
import bytecask.data_entry;
import bytecask.types;

namespace bytecask {

// Byte offset into a data file, as returned by append() and consumed by read().
export using Offset = std::uint64_t;

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

// ---------------------------------------------------------------------------
// WritableDataFile — append-only data file for the active write path.
//
// Reads are served via pread. No mmap, no mode transitions.
// Thread safety: NOT thread-safe. External synchronization required for writes.
// Concurrent pread-based reads are safe (POSIX guarantees).
export class WritableDataFile : public DataFile {
public:
  [[nodiscard]] static auto openForWrite(std::filesystem::path path)
      -> std::shared_ptr<WritableDataFile> {
    return std::shared_ptr<WritableDataFile>(
        new WritableDataFile{std::move(path)});
  }

  ~WritableDataFile() override;

  WritableDataFile(WritableDataFile &&other) noexcept
      : DataFile{std::move(other)}, fd_{other.fd_}, offset_{other.offset_},
        preallocated_end_{other.preallocated_end_} {
    other.fd_ = -1;
  }

  WritableDataFile &operator=(WritableDataFile &&other) noexcept {
    if (this != &other) {
      if (fd_ != -1) {
        ::close(fd_);
      }
      DataFile::operator=(std::move(other));
      fd_ = other.fd_;
      offset_ = other.offset_;
      preallocated_end_ = other.preallocated_end_;
      other.fd_ = -1;
    }
    return *this;
  }

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
    ensure_preallocated(offset_ + static_cast<Offset>(total));
    const auto written = ::writev(fd_, iov.data(), std::ssize(iov));
#ifdef BYTECASK_TESTING
    FAULT_INJECTION_POST_WRITE(io_data_file_append_partial,
                               fd_, entry_offset, total);
#endif
    if (written != narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::append: writev failed"};
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

      ensure_preallocated(offset_ + static_cast<Offset>(total_bytes));
      const auto written =
          ::writev(fd_, iov.data(), narrow<int>(chunk_size * kIovecsPerEntry));

#ifdef BYTECASK_TESTING
      FAULT_INJECTION_POST_WRITE(io_data_file_append_partial,
                                 fd_, offset_, total_bytes);
#endif
      if (written != narrow<ssize_t>(total_bytes)) {
        throw std::system_error{errno, std::generic_category(),
                                "DataFile::append_entries: writev failed"};
      }

      offset_ += static_cast<Offset>(total_bytes);
    }
  }

  [[nodiscard]] auto scan(Offset offset) const
      -> std::optional<std::pair<DataEntry, Offset>> override {
    if (offset >= offset_) {
      return std::nullopt;
    }
    const auto header = read_header(offset);
    std::vector<std::byte> buf;
    auto view = read_entry(offset, header.key_size, header.value_size, buf);
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
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
      auto view = read_entry(offset, key_size, value_size, io_buf);
      out.assign(view.value.begin(), view.value.end());
    } else {
      read_value_unverified(offset, key_size, value_size, out);
    }
  }

  void sync() {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_data_file_sync);
#endif
    if (portable_fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::sync: fdatasync failed"};
    }
  }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return offset_;
  }

  void truncate(Offset new_size) {
    if (::ftruncate(fd_, narrow<off_t>(new_size)) != 0) {
      throw std::system_error{errno, std::system_category(),
                              "DataFile::truncate"};
    }
    offset_ = new_size;
    preallocated_end_ = new_size;
  }

private:
  explicit WritableDataFile(std::filesystem::path path)
      : DataFile{std::move(path)} {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ == -1) {
      throw std::system_error{
          errno, std::generic_category(),
          std::format("DataFile: cannot open '{}'", path_.string())};
    }
#ifndef __APPLE__
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
#endif
    offset_ = std::filesystem::file_size(path_);
    preallocated_end_ = offset_;
  }

  int fd_{-1};
  Offset offset_{0};
  Offset preallocated_end_{0};
  std::array<std::byte, kHeaderSize + kCrcSize> hdr_crc_buf_{};

  [[nodiscard]] auto read_header(Offset offset) const -> EntryHeader {
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::read_header: pread failed"};
    }
    return bytecask::read_header(std::span{hdr});
  }

  [[nodiscard]] auto read_entry(Offset offset, std::uint16_t key_size,
                                std::uint32_t value_size,
                                std::vector<std::byte>& io_buf) const
      -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    io_buf.resize(total);
    if (::pread(fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
        narrow<ssize_t>(total)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::read_entry: pread failed"};
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

  void read_value_unverified(Offset offset, std::uint16_t key_size,
                             std::uint32_t value_size,
                             std::vector<std::byte>& out) const {
    const auto val_offset = offset + kHeaderSize + key_size;
    out.resize(value_size);
    if (::pread(fd_, out.data(), value_size,
                narrow<off_t>(val_offset)) != narrow<ssize_t>(value_size)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::read_value: pread failed"};
    }
  }

  void ensure_preallocated(Offset write_end) {
#ifdef __linux__
    if (write_end <= preallocated_end_) return;
    static constexpr Offset kPreallocChunk = 4 * 1024 * 1024;  // 4 MiB
    auto alloc_end =
        ((write_end + kPreallocChunk - 1) / kPreallocChunk) * kPreallocChunk;
    if (::fallocate(fd_, FALLOC_FL_KEEP_SIZE, narrow<off_t>(preallocated_end_),
                    narrow<off_t>(alloc_end - preallocated_end_)) == 0) {
      preallocated_end_ = alloc_end;
    }
#else
    (void)write_end;
#endif
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
            ::writev(fd_, iov_buf.data(), narrow<int>(serialized * kIovecsPerEntry));
        if (written == narrow<ssize_t>(byte_count)) {
          offset_ += static_cast<Offset>(byte_count);
        }
      }
      throw;
    }
  }
#endif
};

WritableDataFile::~WritableDataFile() {
  if (fd_ != -1) {
    ::close(fd_);
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
    if (offset >= file_size_) {
      return std::nullopt;
    }
    const auto header = read_header(offset);
    std::vector<std::byte> buf;
    auto view = read_entry(offset, header.key_size, header.value_size, buf);
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
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
      auto view = read_entry(offset, key_size, value_size, io_buf);
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

  [[nodiscard]] auto read_entry(Offset offset, std::uint16_t key_size,
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
    if (offset >= mmap_size_) {
      return std::nullopt;
    }
    const auto header = read_header(offset);
    auto view = read_entry(offset, header.key_size, header.value_size);
    const auto next =
        offset + kHeaderSize + header.key_size + header.value_size + kCrcSize;
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
      auto view = read_entry(offset, key_size, value_size);
      out.assign(view.value.begin(), view.value.end());
    } else {
      const auto val_offset = offset + kHeaderSize + key_size;
      assert(val_offset + value_size <= mmap_size_);
      auto *base = mmap_base_ + val_offset;
      out.assign(base, base + value_size);
    }
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

  [[nodiscard]] auto read_entry(Offset offset, std::uint16_t key_size,
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

// Generic factory: returns mmap-backed DataFile when possible, pread-based otherwise.
export [[nodiscard]] inline auto openDataFileForRead(std::filesystem::path path)
    -> std::shared_ptr<DataFile> {
#ifndef __EMSCRIPTEN__
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) {
    return ReadOnlyMmapDataFile::openForRead(std::move(path));
  }
#endif
  return ReadOnlyPosixDataFile::openForRead(std::move(path));
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
