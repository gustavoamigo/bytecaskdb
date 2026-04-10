// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — common error codes and stored value types

module;
#include <cstdint>

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
};

} // namespace bytecask
