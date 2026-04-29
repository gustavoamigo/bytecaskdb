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
#include <optional>
#include <span>
#include <sys/uio.h>
#include <sys/mman.h>
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
    // Hint to the kernel that reads will be random (point lookups by offset).
    // posix_fadvise is Linux-only; macOS has no equivalent.
#ifndef __APPLE__
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
#endif
    offset_ = std::filesystem::file_size(path_);
    preallocated_end_ = offset_;
  }

  ~DataFile() {
    if (mmap_base_) {
      ::munmap(mmap_base_, mmap_size_);
    }
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
        preallocated_end_{other.preallocated_end_},
        sealed_{other.sealed_}, tainted_{other.tainted_},
        mmap_base_{other.mmap_base_}, mmap_size_{other.mmap_size_} {
    other.fd_ = -1;
    other.mmap_base_ = nullptr;
    other.mmap_size_ = 0;
  }

  // Closes any fd we currently own before stealing 'other's resources.
  // The self-assignment guard avoids closing the fd we are about to adopt.
  //
  //   DataFile a{"a.data"};          // a: fd=5
  //   DataFile b{"b.data"};          // b: fd=6
  //   b = std::move(a);              // close(6), b: fd=5, a: fd=-1
  DataFile &operator=(DataFile &&other) noexcept {
    if (this != &other) {
      if (mmap_base_) {
        ::munmap(mmap_base_, mmap_size_);
      }
      if (fd_ != -1) {
        ::close(fd_);
      }
      path_ = std::move(other.path_);
      fd_ = other.fd_;
      offset_ = other.offset_;
      sealed_ = other.sealed_;
      tainted_ = other.tainted_;
      preallocated_end_ = other.preallocated_end_;
      mmap_base_ = other.mmap_base_;
      mmap_size_ = other.mmap_size_;
      other.fd_ = -1;
      other.mmap_base_ = nullptr;
      other.mmap_size_ = 0;
    }
    return *this;
  }

  // Writes an entry via writev(). Header and CRC are serialized into
  // hdr_crc_buf_ by data_entry; key and value are passed as direct iovecs.
  // No heap allocation, no copy of key/value data.
  // Precondition: the file must not have been sealed.
  [[nodiscard]] auto append_entry(std::uint64_t sequence, EntryType entry_type,
                            std::span<const std::byte> key,
                            std::span<const std::byte> value) -> Offset {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_data_file_append);
#endif
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
    ensure_preallocated(offset_ + static_cast<Offset>(total));
    const auto written = ::writev(fd_, iov.data(), std::ssize(iov));
#ifdef BYTECASK_TESTING
    // Post-write injection simulates partial or full writes followed by
    // failure. In both cases, bytes are on disk that offset_ will not
    // account for — the file is tainted before the checkpoint throws.
    tainted_ = true;
    FAULT_INJECTION_POST_WRITE(io_data_file_append_partial,
                               fd_, entry_offset, total);
    tainted_ = false;
#endif
    if (written != narrow<ssize_t>(total)) {
      // Any writev failure — whether the call returned -1 (possibly with
      // bytes in the page cache on FUSE/network filesystems) or a short
      // write — leaves the file in an indeterminate state. Always mark
      // tainted so the caller degrades rather than attempting in-flight
      // recovery. resume() will restore a consistent state.
      tainted_ = true;
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::append: writev failed"};
    }

    offset_ += static_cast<Offset>(total);
    return entry_offset;
  }

  // Batch-appends multiple entries in as few writev() calls as possible.
  // Each chunk of up to kMaxEntriesPerWritev entries is written with a single
  // writev(). Offsets are written into offsets_out (must be same size as
  // entries). Failure semantics identical to append_entry: tainted + throw.
  void append_entries(std::span<const DataEntryView> entries,
                      std::span<Offset> offsets_out) {
    assert(entries.size() == offsets_out.size());
    if (entries.empty()) return;
    assert(!sealed_);

    static constexpr std::size_t kIovecsPerEntry = 4;
#ifdef BYTECASK_TESTING
    // Force multi-writev chunking so proof tests exercise the loop.
    // A 5-entry large_batch becomes 3 writev calls (2+2+1).
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
      tainted_ = true;
      FAULT_INJECTION_POST_WRITE(io_data_file_append_partial,
                                 fd_, offset_, total_bytes);
      tainted_ = false;
#endif
      if (written != narrow<ssize_t>(total_bytes)) {
        tainted_ = true;
        throw std::system_error{errno, std::generic_category(),
                                "DataFile::append_entries: writev failed"};
      }

      offset_ += static_cast<Offset>(total_bytes);
    }
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

  // High-level read: extracts the value at offset into out.
  // When verify is true, reads the full entry and validates the CRC32 checksum.
  // When false, reads only the value bytes for minimum I/O.
  void read_value(Offset offset, std::uint16_t key_size,
                  std::uint32_t value_size, bool verify,
                  std::vector<std::byte> &io_buf,
                  std::vector<std::byte> &out) const {
    if (verify) {
      auto view = read_entry(offset, key_size, value_size, io_buf);
      out.assign(view.value.begin(), view.value.end());
    } else {
      read_value_unverified(offset, key_size, value_size, out);
    }
  }

  // Flushes all pending writes to physical storage via fdatasync.
  // Call after one or more append()s to guarantee crash-safety (Group Commit).
  void sync() {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_data_file_sync);
#endif
    if (portable_fdatasync(fd_) != 0) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::sync: fdatasync failed"};
    }
  }

  // Returns the current file size in bytes (equal to the write offset).
  [[nodiscard]] auto size() const noexcept -> Offset { return offset_; }

  // Marks the file as sealed: no further appends are permitted.
  // The fd remains open for reads. seal() is called by DB on rotation.
  void seal() noexcept {
    sealed_ = true;
#ifndef __EMSCRIPTEN__
    if (offset_ > 0) {
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      auto *ptr = ::mmap(nullptr, offset_, PROT_READ, MAP_PRIVATE, fd_, 0);
      if (ptr != MAP_FAILED) {
        mmap_base_ = static_cast<std::byte *>(ptr);
        mmap_size_ = offset_;
        ::madvise(mmap_base_, mmap_size_, MADV_RANDOM);
      }
    }
#endif
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

  // Returns true if any writev failure has been detected on this file.
  // A tainted file has bytes on disk that offset_ does not account for —
  // subsequent writes would land at incorrect offsets.
  [[nodiscard]] auto is_tainted() const noexcept -> bool { return tainted_; }

  // Truncates the file to new_size bytes and resets offset_ accordingly.
  // Clears tainted_. Only valid on unsealed (active) files — a mapped file
  // must never be truncated. Throws std::system_error on failure.
  void truncate(Offset new_size) {
    assert(!mmap_base_);
    if (::ftruncate(fd_, narrow<off_t>(new_size)) != 0) {
      throw std::system_error{errno, std::system_category(),
                              "DataFile::truncate"};
    }
    offset_ = new_size;
    preallocated_end_ = new_size;
    tainted_ = false;
  }

private:
  std::filesystem::path path_;
  int fd_{-1};
  Offset offset_{0};
  Offset preallocated_end_{0};
  bool sealed_{false};
  bool tainted_{false};
  // Fixed buffer holding the 15-byte header and 4-byte CRC for each append.
  // Avoids heap allocation on the hot write path.
  std::array<std::byte, kHeaderSize + kCrcSize> hdr_crc_buf_{};
  std::byte *mmap_base_{nullptr};
  std::size_t mmap_size_{0};

  // Reads the fixed header at offset via mmap or pread.
  [[nodiscard]] auto read_header(Offset offset) const -> EntryHeader {
    if (mmap_base_) {
      assert(offset + kHeaderSize <= mmap_size_);
      return bytecask::read_header(std::span{mmap_base_ + offset, kHeaderSize});
    }
    std::array<std::byte, kHeaderSize> hdr{};
    if (::pread(fd_, hdr.data(), kHeaderSize, narrow<off_t>(offset)) !=
        std::ssize(hdr)) {
      throw std::system_error{errno, std::generic_category(),
                              "DataFile::read_header: pread failed"};
    }
    return bytecask::read_header(std::span{hdr});
  }

  // Reads the full entry at offset, verifies CRC, returns non-owning view.
  // When mmap is active, spans point into mapped memory (io_buf unused).
  // Otherwise, reads into io_buf and spans point into it.
  [[nodiscard]] auto read_entry(Offset offset, std::uint16_t key_size,
                                std::uint32_t value_size,
                                std::vector<std::byte>& io_buf) const
      -> DataEntryView {
    const auto total = kHeaderSize + key_size + value_size + kCrcSize;
    std::span<const std::byte> raw;
    if (mmap_base_) {
      assert(offset + total <= mmap_size_);
      raw = {mmap_base_ + offset, total};
    } else {
      io_buf.resize(total);
      if (::pread(fd_, io_buf.data(), total, narrow<off_t>(offset)) !=
          narrow<ssize_t>(total)) {
        throw std::system_error{errno, std::generic_category(),
                                "DataFile::read_entry: pread failed"};
      }
      raw = io_buf;
    }
    const auto header = parse_header_and_verify(raw);
    auto body = raw.subspan(kHeaderSize);
    return DataEntryView{
        .sequence = header.sequence,
        .entry_type = header.entry_type,
        .key = body.subspan(0, key_size),
        .value = body.subspan(key_size, value_size),
    };
  }

  // Reads only the value bytes — minimum I/O, no CRC check.
  void read_value_unverified(Offset offset, std::uint16_t key_size,
                             std::uint32_t value_size,
                             std::vector<std::byte>& out) const {
    const auto val_offset = offset + kHeaderSize + key_size;
    if (mmap_base_) {
      assert(val_offset + value_size <= mmap_size_);
      auto* base = mmap_base_ + val_offset;
      out.assign(base, base + value_size);
    } else {
      out.resize(value_size);
      if (::pread(fd_, out.data(), value_size,
                  narrow<off_t>(val_offset)) != narrow<ssize_t>(value_size)) {
        throw std::system_error{errno, std::generic_category(),
                                "DataFile::read_value: pread failed"};
      }
    }
  }

  // Preallocates disk blocks up to write_end without changing the file's
  // logical size. Reduces filesystem extent-allocation overhead on the write
  // path. Failure is silently ignored — preallocation is a pure optimization.
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
      tainted_ = true;
      throw;
    }
  }
#endif
};

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

  // Exposes the byte offset that will be read on the next increment.
  // After the last successful dereference, this is the offset past the
  // current entry — useful for callers that need to track file position.
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
