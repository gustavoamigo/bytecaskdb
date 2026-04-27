# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo

"""ByteCaskDB Python bindings."""

__version__ = "0.1.0"

# Low-level C extension types — available as bytecaskdb._bytecaskdb.*
from ._bytecaskdb import (
    DB as _RawDB,
    Snapshot as _RawSnapshot,
    WritePlan as _RawWritePlan,
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

# Pythonic wrapper — the public API.
from .ext import DB, Snapshot, ConflictError, DegradedError, ByteCaskError

__all__ = [
    "DB",
    "Snapshot",
    "ConflictError",
    "DegradedError",
    "ByteCaskError",
    "IoError",
    "Options",
    "WriteOptions",
    "ReadOptions",
    "VacuumOptions",
    "DbDegraded",
    "EntryIterator",
    "KeyIterator",
    "ReverseEntryIterator",
    "ReverseKeyIterator",
]
