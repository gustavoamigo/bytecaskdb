// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — common error codes and stored value types

module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
  ChangeMarker = 0x06, // A promotion — key empty, value = the marker's 8-byte id
};

// Whether a type byte read from storage names an entry type. Anything else
// is damage: the parsers refuse it rather than let it fall through a switch.
export constexpr auto is_known_entry_type(EntryType t) noexcept -> bool {
  switch (t) {
  case EntryType::Put:
  case EntryType::Delete:
  case EntryType::BulkBegin:
  case EntryType::BulkEnd:
  case EntryType::RangeDel:
  case EntryType::ChangeMarker:
    return true;
  }
  return false;
}

// The identity of a history from a promotion on. A ChangeMarker entry is
// appended when a follower is promoted; since_sequence is that entry's own
// sequence and id a random 64-bit value. A node's marker at a sequence is
// its last marker at or below it, kOriginMarker if it holds none. Two nodes
// with the same marker at a sequence hold the same history up to it (#397).
export struct ChangeMarker {
  std::uint64_t since_sequence{0};
  std::uint64_t id{0};
  friend auto operator==(const ChangeMarker &, const ChangeMarker &) noexcept
      -> bool = default;
};

// The history before any promotion: sequence 0 is below every sequence the
// engine assigns, so this is the marker at every position of a database that
// was never promoted.
export inline constexpr ChangeMarker kOriginMarker{};

// Where a slice of a history starts: the source's marker at from_sequence,
// and from_sequence. The slice holds the source's entries above it, in order.
export struct ChangeHeader {
  ChangeMarker marker{};
  std::uint64_t from_sequence{0};
};

// A ChangeMarker entry's value: its id, little-endian.
export inline constexpr std::size_t kChangeMarkerIdBytes = 8;

export constexpr auto encode_marker_id(std::uint64_t id) noexcept
    -> std::array<std::byte, kChangeMarkerIdBytes> {
  std::array<std::byte, kChangeMarkerIdBytes> out{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::byte>((id >> (8 * i)) & 0xFF);
  return out;
}

// The id in a ChangeMarker entry's value, which the caller has checked holds
// kChangeMarkerIdBytes.
export constexpr auto decode_marker_id(std::span<const std::byte> value) noexcept
    -> std::uint64_t {
  std::uint64_t id = 0;
  for (std::size_t i = 0; i < kChangeMarkerIdBytes && i < value.size(); ++i)
    id |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value[i]))
          << (8 * i);
  return id;
}

// changes_since's max_bytes meaning no cut: the slice runs to the snapshot's
// durable sequence.
export inline constexpr std::size_t kUnlimitedBytes =
    std::numeric_limits<std::size_t>::max();

export enum class Mode { Leader, Follower };

// Selects how data files are read. The choice applies to sealed files: the
// writable active file is built identically for Pread and BufferPool, because
// O_DIRECT never touches the write path and the buffer pool keeps the active
// file resident by inserting on append, not by changing how it is written.
export enum class IoBackend {
  Pread,      // pread(2) per read. Default — no virtual address space pressure.
  Mmap,       // sealed files memory-mapped; zero-copy reads, unbounded page cache.
              // A read the kernel cannot complete (EIO) is SIGBUS, not an
              // exception.
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
