# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo

"""ByteCaskDB Python bindings."""

from ._bytecaskdb import (
    DB,
    Snapshot,
    WritePlan,
    Batch,
    Options,
    WriteOptions,
    ReadOptions,
    VacuumOptions,
    DbDegraded,
    EntryIterator,
    KeyIterator,
    ReverseEntryIterator,
    ReverseKeyIterator,
)

# IoError is Python's built-in OSError — C++ std::system_error maps to it.
IoError = OSError

__all__ = [
    "DB",
    "Snapshot",
    "WritePlan",
    "Batch",
    "Options",
    "WriteOptions",
    "ReadOptions",
    "VacuumOptions",
    "DbDegraded",
    "IoError",
    "EntryIterator",
    "KeyIterator",
    "ReverseEntryIterator",
    "ReverseKeyIterator",
]
