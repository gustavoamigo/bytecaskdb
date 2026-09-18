// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — common error codes and stored value types

module;
#include <cstddef>
#include <cstdint>
#include <span>

export module bytecask.types;

namespace bytecask {

// Entry kind discriminant, shared by both the data file and hint file formats.
// Value 0 is deliberately unassigned — a zero byte always indicates
// corrupt/uninitialized storage, which lets the scanner detect truncated writes
// without a separate magic number.
export enum class EntryType : std::uint8_t {
  Put = 0x01,       // Standard key-value pair
  Delete = 0x02,    // Tombstone — key present, value empty
  BulkBegin = 0x03, // Start of atomic batch — key and value empty
  BulkEnd = 0x04,   // End of atomic batch   — key and value empty
  RangeDel = 0x05,  // Range tombstone — key = start_key, value = end_key
};

export enum class Mode { Leader, Follower };

// Selects how data files are read. The choice applies to sealed files: the
// writable active file is built identically for Pread and BufferPool, because
// O_DIRECT never touches the write path and the buffer pool keeps the active
// file resident by inserting on append, not by changing how it is written.
export enum class IoBackend {
  Pread,      // pread(2) per read. Default — no virtual address space pressure.
  Mmap,       // sealed files memory-mapped; zero-copy reads, unbounded page cache.
  BufferPool, // sealed files served from a bounded, engine-owned cache.
};

export struct DataEntryView {
  std::uint64_t sequence;
  EntryType entry_type;
  std::span<const std::byte> key;
  std::span<const std::byte> value;
};

export struct EntryView {
  std::span<const std::byte> key;
  std::span<const std::byte> value;
};

} // namespace bytecask
