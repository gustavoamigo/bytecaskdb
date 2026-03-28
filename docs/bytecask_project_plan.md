# ByteCask Project Plan

This file is the repository's simple issue tracker.

Canonical location: `docs/bytecask_project_plan.md`.

## How to use it

- Add new work as a short item with an ID, status, title, and note.
- Move active work to `In Progress`.
- Move finished work to `Done`.
- Keep follow-ups in `Backlog`.

## In Progress

_nothing currently in progress_

## Backlog

| ID | Title | Note |
| --- | --- | --- |
| BC-002 | Shared engine library target | xmake C++23 module BMI sharing across static-lib targets needs investigation; currently engine sources are compiled per-target. |
| BC-006 | Data file read path | Read entries back from a data file for recovery and verification. |
| BC-007 | Tombstone (delete) support | Set flags bit 0 when writing a delete entry. |
| BC-008 | File naming convention | Implement `data_{timestamp}.data` naming and file rotation. |
| BC-010 | CrcInputAdapter for read path | Matching bitsery input adapter that accumulates CRC while deserializing; needed when BC-006 (read path) is implemented. |

## Done

| ID | Title | Note |
| --- | --- | --- |
| BC-000 | Establish Copilot workflow | Added always-on repo instructions, living design doc, project plan, and test wiring. |
| BC-001 | Define storage engine MVP | Defined data entry format and DataFile API as the first storage-engine component. |
| BC-003 | Replace smoke test with behavior tests | Replaced `smoke_test.cpp` with `data_entry_test.cpp` covering serialization, append, and CRC verification. |
| BC-004 | Move living docs into docs folder | Relocated the design doc and task tracker to `docs/` and updated workflow references. |
| BC-005 | Data entry append to file | `DataFile` appends key-value entries with 19-byte LE header, CRC32, and auto-incrementing sequence numbers. Zero warnings. All tests pass. |
| BC-009 | Bitsery serialization + CRC-at-end | Replaced manual LE serialization with bitsery. Moved CRC to trailing position for one-pass write. Extracted `CrcOutputAdapter<TAdapter>` (reusable bitsery wrapper) in `data_entry.cppm`. `kHeaderSize` = 15, `kCrcSize` = 4. Zero warnings. All tests pass. |
| BC-011 | C++20/23 modernization & readability | Applied `std::span`, `std::as_bytes`, `std::to_integer`, `std::in_range`, `std::ssize`, `std::filesystem::file_size`, `is_integral_v`, `auto` consistency. Added `narrow<To>()` checked-narrowing helper and `write_bytes()` wrapper to hide `reinterpret_cast` from call sites. Updated copilot-instructions with coding guidelines. All tests pass. |
| BC-012 | Migrate tests to Catch2 | Replaced hand-rolled `fail()`/`expect()` harness with Catch2 v3. `TEST_CASE`/`CHECK`/`REQUIRE` macros, Catch2's own `main()`, randomized test order. 3 test cases, 28 assertions. |