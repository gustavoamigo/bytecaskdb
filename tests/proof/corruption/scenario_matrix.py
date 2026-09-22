# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Scenario matrix for the corruption axis (#104 class M4).
#
# Classes A through H all mean the same thing at the POSIX level: a syscall
# returned an error. This axis is over the *bytes* rather than the *calls* — a
# read that succeeds and hands back something wrong. Every degrade shape in the
# resume matrix leaves a well-formed active file whose orphaned bytes parse and
# whose CRCs hold; degrade_B2 reaches the scan's error branch by construction,
# but a torn tail is the only position a short write can produce.
#
# The contract under test is fail-stop, not recovery. Damage inside bytes that
# were already published leaves no consistent state to resume into, so the
# engine promises only to detect it and refuse: resume() throws, stays
# degraded and truncates nothing. It promises nothing about what the damaged
# file still holds.
#
# The entry layout makes a position addressable without parsing: a data entry
# is 15 bytes of header, then key, then value, then a 4-byte CRC, and the
# header is sequence(u64) entry_type(u8) key_size(u16) value_size(u32). With
# 2-byte keys and 2-byte values every entry is 23 bytes, so entry i begins at
# 23*i and each field sits at a fixed offset inside it.

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Generator, Tuple

# 15 header + 2 key + 2 value + 4 crc
ENTRY_BYTES = 23
MARKER_BYTES = 19  # header + crc, no key or value


class CorruptField(Enum):
    # Flip a byte of the key, which the entry CRC covers: the entry parses and
    # fails verification.
    CRC = "crc"
    # A value_size that runs past the end of the file — what #36 sized a read
    # buffer from before the bound was added.
    VALUE_SIZE = "value_size"
    # A byte that is not a valid EntryType.
    ENTRY_TYPE = "entry_type"
    # A zeroed sequence, which the scan already treats as end of file.
    SEQUENCE = "sequence"

    @property
    def detected_at_open(self) -> bool:
        """Whether a cold open can tell this damage from the end of the file.

        A CRC mismatch and an invalid entry type fail to parse, and the open
        refuses. A zeroed sequence and an entry running past the file's end
        both read as the end of written data — the same thing the unwritten,
        zero-filled tail of a crashed active file looks like — and a hint-less
        file records no committed length to tell them apart. resume() can,
        because the published state knows how far the file was published.
        """
        return self in (CorruptField.CRC, CorruptField.ENTRY_TYPE)


class Position(Enum):
    FIRST = "first"      # entry 0 — the scan parses nothing at all
    MIDDLE = "middle"    # entry 2
    LAST = "last"        # entry 5, the final entry
    IN_BATCH = "in_batch"  # an entry between BulkBegin and BulkEnd


# Field byte offsets within an entry.
FIELD_OFFSET = {
    CorruptField.SEQUENCE: 0,
    CorruptField.ENTRY_TYPE: 8,
    CorruptField.VALUE_SIZE: 11,
    CorruptField.CRC: 15,  # first key byte
}


@dataclass(frozen=True)
class CorruptionShape:
    label: str
    position: Position

    @property
    def batched(self) -> bool:
        return self.position == Position.IN_BATCH

    @property
    def corrupt_entry_index(self) -> int:
        return {Position.FIRST: 0, Position.MIDDLE: 2, Position.LAST: 5}.get(
            self.position, 0
        )

    @property
    def corrupt_offset(self) -> int:
        """Byte offset of the corrupted entry's start."""
        if self.position == Position.IN_BATCH:
            # k0, k1, then BulkBegin, then the first batch entry.
            return 2 * ENTRY_BYTES + MARKER_BYTES
        return self.corrupt_entry_index * ENTRY_BYTES

    @property
    def published_extent(self) -> int:
        """Bytes published before the damage — what resume() must not cut."""
        if self.position == Position.IN_BATCH:
            # k0, k1, then BulkBegin, b0, b1, BulkEnd.
            return 4 * ENTRY_BYTES + 2 * MARKER_BYTES
        return 6 * ENTRY_BYTES


CORRUPTION_SHAPES = [
    CorruptionShape("first_entry", Position.FIRST),
    CorruptionShape("mid_file", Position.MIDDLE),
    CorruptionShape("last_entry", Position.LAST),
    CorruptionShape("inside_batch", Position.IN_BATCH),
]

CORRUPT_FIELDS = list(CorruptField)


def is_valid_combination(shape: CorruptionShape, field: CorruptField) -> bool:
    # A marker carries no key or value, so there is no key byte to flip and no
    # value_size to overstate; IN_BATCH corrupts a real entry inside the batch,
    # so every field applies there too.
    return True


def generate_matrix() -> Generator[
    Tuple[CorruptionShape, CorruptField], None, None
]:
    for shape in CORRUPTION_SHAPES:
        for field in CORRUPT_FIELDS:
            if is_valid_combination(shape, field):
                yield (shape, field)
