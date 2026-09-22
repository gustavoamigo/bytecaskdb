// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Invariant-checking helpers for the correctness validation framework.
//
// Requires the including translation unit to `import bytecask;` before
// this header (same pattern as fault_injector.h).

#pragma once
#ifdef BYTECASK_TESTING

#include <catch2/catch_test_macros.hpp>
#include "mapping_probe.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace bytecask::testing {

// ---- Conversion helpers (same logic as bytecask_test.cpp) ----------------

inline auto to_bytes(std::string_view sv) -> BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

inline auto to_string(const Bytes &bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  std::ranges::transform(bytes, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

inline auto to_string(BytesView bv) -> std::string {
  std::string s(bv.size(), '\0');
  std::ranges::transform(bv, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

inline auto to_string(const Key &key) -> std::string {
  std::string s(key.size(), '\0');
  std::ranges::transform(key, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

// ---- Data types ----------------------------------------------------------

// Owned snapshot of DB state before a transition.
// Uses an owned map (not a Snapshot) to avoid holding file descriptors alive
// during fault injection tests.
struct Baseline {
  std::uint64_t next_seq;
  std::map<std::string, Bytes> key_values;
};

// Expected outcome of a state transition, as defined by the reference model.
struct ExpectedDelta {
  std::vector<std::string> keys_added;
  std::vector<std::string> keys_removed;
  std::map<std::string, std::string> expected_values;  // key -> expected value
  std::uint64_t seq_advance;
  bool degraded;
};

// ---- Core functions ------------------------------------------------------

// Captures a baseline snapshot of the DB for later delta comparison.
inline auto capture_baseline(const DB &db) -> Baseline {
  Baseline bl;
  bl.next_seq = db.engine_state()->next_seq;
  for (const auto &entry : db.iter_from({})) {
    bl.key_values[to_string(entry.key)] = Bytes{entry.value.begin(), entry.value.end()};
  }
  return bl;
}

// Validates structural consistency of the published EngineState.
// Uses Catch2 CHECK macros — call from a TEST_CASE context.
inline void assert_consistent(const DB &db) {
  auto state = db.engine_state();

  // 1. live_bytes matches key_dir.
  std::map<std::uint32_t, std::uint64_t> computed_live;
  std::uint64_t max_seq = 0;
  for (auto it = state->key_dir.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    computed_live[entry.file_id()] +=
        entry_size(key_span.size(), entry.value_size());

    // 2. No dangling file references.
    INFO("key references file_id=" << entry.file_id());
    CHECK(state->files.contains(entry.file_id()));

    // 5. Track max sequence for next_seq check.
    if (entry.sequence() > max_seq) max_seq = entry.sequence();
  }

  for (const auto [file_id, fs] : state->file_stats) {
    auto it = computed_live.find(file_id);
    auto expected_live = (it != computed_live.end()) ? it->second : 0ULL;
    INFO("file_id=" << file_id << " live_bytes");
    CHECK(fs.live_bytes == expected_live);
  }

  // 3. Active file exists.
  CHECK(state->files.contains(state->active_file_id));

  // 4. file_stats covers all files.
  for (const auto [file_id, _] : state->files) {
    INFO("file_id=" << file_id << " missing from file_stats");
    CHECK(state->file_stats.contains(file_id));
  }

  // 5. next_seq ahead of all sequences.
  if (max_seq > 0) {
    CHECK(state->next_seq > max_seq);
  }
}

// Validates the transition delta against the reference model's expected delta.
inline void assert_delta(const Baseline &before, const DB &db,
                         const ExpectedDelta &expected) {
  // Key membership.
  for (const auto &key : expected.keys_added) {
    INFO("expected key added: " << key);
    CHECK(db.contains_key({}, to_bytes(key)));
  }
  for (const auto &key : expected.keys_removed) {
    INFO("expected key removed: " << key);
    CHECK_FALSE(db.contains_key({}, to_bytes(key)));
  }

  // Value verification (causality: last write determines value).
  for (const auto &[key, value] : expected.expected_values) {
    Bytes out;
    INFO("expected value for key: " << key);
    REQUIRE(db.get({}, to_bytes(key), out));
    CHECK(to_string(out) == value);
  }

  // Sequence advancement.
  auto after = db.engine_state();
  CHECK(after->next_seq == before.next_seq + expected.seq_advance);

  // Structural consistency.
  assert_consistent(db);

  // Degraded state.
  CHECK(db.is_degraded() == expected.degraded);
}

// The view handed out before the transition still addresses live memory and
// still holds the bytes it had.
//
// Comparing addresses is not enough: mmap(nullptr, ...) often returns the
// address munmap just released, so an unmap-and-remap can look identical to
// never having unmapped. mincore() answers whether the range is still mapped
// at all; the byte comparison is what actually discriminates, because a
// remapped region is mapped but need not hold the same file bytes at the
// same offset. See "View and span lifetimes" in CONTRACT.md.
inline void assert_view_stable(std::span<const std::byte> held,
                               std::span<const std::byte> expected) {
  REQUIRE(held.size() == expected.size());
#ifndef __EMSCRIPTEN__
  CHECK(is_mapped(held.data(), held.size()));
#endif
  CHECK(std::ranges::equal(held, expected));
}

// Calls resume() and verifies the engine recovers fully.
inline void assert_resumable(DB &db) {
  REQUIRE_NOTHROW(db.resume());
  CHECK_FALSE(db.is_degraded());
  assert_consistent(db);
}

// Opens a fresh DB from on-disk files and verifies the recovered state matches
// the expected state. Validates the persistence invariant: committed
// transitions survive recovery, uncommitted ones do not.
//
// The caller must ensure no other DB instance has `dir` open (the original
// DB's destructor flushes hint files that recovery depends on).
inline void assert_recoverable(const std::filesystem::path &dir,
                               const Baseline &before,
                               const ExpectedDelta &expected,
                               const Options &opts = {}) {
  auto recovered = DB::open(dir, opts);

  // Pre-existing keys that were not removed or overwritten must be present
  // with their original values.
  for (const auto &[key, value] : before.key_values) {
    bool was_removed =
        std::ranges::find(expected.keys_removed, key) !=
        expected.keys_removed.end();
    bool has_new_value = expected.expected_values.contains(key);
    if (!was_removed && !has_new_value) {
      INFO("pre-existing key must survive recovery: " << key);
      CHECK(recovered.contains_key({}, to_bytes(key)));
      Bytes out;
      if (recovered.get({}, to_bytes(key), out)) {
        CHECK(out == value);
      }
    }
  }

  // Added keys must be present.
  for (const auto &key : expected.keys_added) {
    INFO("added key must survive recovery: " << key);
    CHECK(recovered.contains_key({}, to_bytes(key)));
  }

  // Removed keys must be absent.
  for (const auto &key : expected.keys_removed) {
    INFO("removed key must be absent after recovery: " << key);
    CHECK_FALSE(recovered.contains_key({}, to_bytes(key)));
  }

  // Value verification (causality: last write determines value after recovery).
  for (const auto &[key, value] : expected.expected_values) {
    Bytes out;
    INFO("expected value after recovery for key: " << key);
    if (recovered.get({}, to_bytes(key), out)) {
      CHECK(to_string(out) == value);
    }
  }

  // No extra keys.
  for (const auto &rk : recovered.keys_from({})) {
    auto key_str = to_string(rk);
    bool in_baseline =
        before.key_values.contains(key_str) &&
        std::ranges::find(expected.keys_removed, key_str) ==
            expected.keys_removed.end();
    bool in_added =
        std::ranges::find(expected.keys_added, key_str) !=
        expected.keys_added.end();
    INFO("unexpected key after recovery: " << key_str);
    CHECK((in_baseline || in_added));
  }

  // Structural consistency on recovered state.
  assert_consistent(recovered);
}

// Opens a fresh DB and verifies that specific keys recover with the values
// they are owed, and that specific others are absent. Used by the resume
// proof tests.
//
// Present keys are read with get(), not contains_key(): a truncation that cut
// too far leaves the key directory intact while the bytes behind it are gone,
// which is how the BC-036 bug stayed invisible to a test named for it.
inline void assert_keys_recoverable(
    const std::filesystem::path &dir,
    const std::map<std::string, std::string> &keys_present,
    const std::vector<std::string> &keys_absent = {},
    const Options &opts = {}) {
  auto recovered = DB::open(dir, opts);
  for (const auto &[key, value] : keys_present) {
    INFO("key must recover with its value: " << key);
    Bytes out;
    CHECK(recovered.get({}, to_bytes(key), out));
    CHECK(to_string(out) == value);
  }
  for (const auto &key : keys_absent) {
    INFO("key must be absent after recovery: " << key);
    CHECK_FALSE(recovered.contains_key({}, to_bytes(key)));
  }
  assert_consistent(recovered);
}

// A fingerprint of everything a resumed engine and a cold-opened one must
// agree on. File ids are assigned by directory order at recovery and do not
// survive a reopen, so per-file stats are keyed by the data file's stem.
struct EngineFingerprint {
  std::uint64_t next_seq{0};
  std::map<std::string, Bytes> key_values;
  std::map<std::string, FileStats> file_stats;
};

inline auto fingerprint(const DB &db) -> EngineFingerprint {
  EngineFingerprint fp;
  auto state = db.engine_state();
  fp.next_seq = state->next_seq;
  for (const auto &entry : db.iter_from({})) {
    fp.key_values[to_string(entry.key)] =
        Bytes{entry.value.begin(), entry.value.end()};
  }
  for (const auto [file_id, fs] : state->file_stats) {
    auto file = state->files.get(file_id);
    if (!file) continue;
    fp.file_stats[(*file)->path().stem().string()] = fs;
  }
  return fp;
}

// resume() and a cold open read the same bytes, so they must reconstruct the
// same engine. This is the property resume() exists to preserve, and the one
// every bug this matrix has found so far violated: a range tombstone that was
// replayed as a no-op left keys behind that recovery removed, and a filtered
// batch marker left a min_sequence recovery computed differently. Both were
// invisible to assertions that only asked whether the resumed state was
// *right*, because it looked right on its own terms.
//
// The recovered DB opens its own fresh active file, which has no counterpart
// in `before`; stems absent from the resumed state are therefore skipped.
// What a cold open may know about consumed sequences.
enum class NextSeq {
  Exact,  // the usual case: the file still carries every sequence it used
  // The damage erased entries, and with them the only evidence that their
  // sequences were ever consumed. A cold open cannot know what it cannot
  // read, so it may report less — never more. Reuse is safe here precisely
  // because the entries that held those sequences are gone.
  RecoveryMayLag,
};

inline void assert_matches_recovery(const std::filesystem::path &dir,
                                    const EngineFingerprint &before,
                                    const Options &opts = {},
                                    NextSeq next_seq = NextSeq::Exact) {
  auto recovered = DB::open(dir, opts);
  const auto after = fingerprint(recovered);

  if (next_seq == NextSeq::Exact) {
    INFO("next_seq must survive recovery");
    CHECK(after.next_seq == before.next_seq);
  } else {
    INFO("a cold open may know fewer consumed sequences, never more");
    CHECK(after.next_seq <= before.next_seq);
  }

  for (const auto &[key, value] : before.key_values) {
    INFO("key present after resume must be present after recovery: " << key);
    auto it = after.key_values.find(key);
    REQUIRE(it != after.key_values.end());
    CHECK(it->second == value);
  }
  for (const auto &[key, _] : after.key_values) {
    INFO("key present after recovery must have been present after resume: "
         << key);
    CHECK(before.key_values.contains(key));
  }

  for (const auto &[stem, fs] : before.file_stats) {
    auto it = after.file_stats.find(stem);
    if (it == after.file_stats.end()) continue;
    INFO("file_stats for " << stem << " must match after recovery");
    CHECK(it->second.live_bytes == fs.live_bytes);
    CHECK(it->second.total_bytes == fs.total_bytes);
    CHECK(it->second.min_sequence == fs.min_sequence);
    CHECK(it->second.max_sequence == fs.max_sequence);
  }
}

// ---- Recovery state shaping -----------------------------------------------
//
// A hint file is a rebuildable index, not the record. These helpers put a
// directory into the states recovery has to cope with: the hint-less data file
// a crash leaves behind, and a hint whose CRC no longer holds.

inline auto sorted_paths(const std::filesystem::path &dir,
                         std::string_view ext) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (const auto &e : std::filesystem::directory_iterator{dir}) {
    if (e.path().extension() == ext) out.push_back(e.path());
  }
  std::ranges::sort(out);  // stems lead with a timestamp
  return out;
}

// True when the newest data file has no hint beside it. A clean close leaves
// exactly this: flush_hints skips the active file, so the file that was active
// at shutdown is hint-less whether the process stopped cleanly or crashed.
// recovery_prepare_files regenerates it at the next open.
inline auto newest_data_is_hintless(const std::filesystem::path &dir) -> bool {
  const auto data = sorted_paths(dir, ".data");
  if (data.empty()) return false;
  auto hint = data.back();
  hint.replace_extension(".hint");
  return !std::filesystem::exists(hint);
}

// Removes every hint file that exists, so recovery regenerates all of them.
inline auto drop_all_hints(const std::filesystem::path &dir) -> int {
  int dropped = 0;
  for (const auto &h : sorted_paths(dir, ".hint")) {
    std::error_code ec;
    if (std::filesystem::remove(h, ec)) ++dropped;
  }
  return dropped;
}

// Flips a byte inside the newest hint file so its CRC-32C trailer no longer
// matches. open_hint_or_rebuild must notice, discard it, and rebuild from the
// data file rather than dropping the keys behind it.
inline auto corrupt_newest_hint(const std::filesystem::path &dir) -> bool {
  const auto hints = sorted_paths(dir, ".hint");
  if (hints.empty()) return false;
  const auto &path = hints.back();
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0) return false;
  std::fstream f{path, std::ios::binary | std::ios::in | std::ios::out};
  if (!f) return false;
  // Byte 0 is inside the first entry, which the trailer CRC covers.
  f.seekg(0);
  char b = 0;
  f.read(&b, 1);
  b = static_cast<char>(b ^ 0xFF);
  f.seekp(0);
  f.write(&b, 1);
  return static_cast<bool>(f);
}

// ---- Byte-level corruption ------------------------------------------------
//
// The fault injector models calls that fail. These model a read that succeeds
// and returns something wrong: the damage is already in the file. Offsets are
// computable rather than parsed — a data entry is 15 bytes of header
// (sequence u64, entry_type u8, key_size u16, value_size u32), then key, then
// value, then a 4-byte CRC.

// Writes `bytes` at `off` in the newest data file.
inline void poke_active_file(const std::filesystem::path &dir,
                             std::uint64_t off,
                             std::span<const unsigned char> bytes) {
  const auto data = sorted_paths(dir, ".data");
  REQUIRE_FALSE(data.empty());
  std::fstream f{data.back(), std::ios::binary | std::ios::in | std::ios::out};
  REQUIRE(f);
  f.seekp(static_cast<std::streamoff>(off));
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.flush();
  REQUIRE(f);
}

// Flips every bit of one byte, so the value is guaranteed to change whatever
// it was.
inline void flip_byte_at(const std::filesystem::path &dir, std::uint64_t off) {
  const auto data = sorted_paths(dir, ".data");
  REQUIRE_FALSE(data.empty());
  std::fstream f{data.back(), std::ios::binary | std::ios::in | std::ios::out};
  REQUIRE(f);
  f.seekg(static_cast<std::streamoff>(off));
  char b = 0;
  f.read(&b, 1);
  b = static_cast<char>(~static_cast<unsigned char>(b));
  f.seekp(static_cast<std::streamoff>(off));
  f.write(&b, 1);
  f.flush();
  REQUIRE(f);
}

// A little-endian u32 large enough that the entry it describes ends past any
// file this suite writes — what #36 sized a read buffer from.
inline void poke_huge_value_size(const std::filesystem::path &dir,
                                 std::uint64_t off) {
  const std::array<unsigned char, 4> huge{0xF0, 0xFF, 0xFF, 0xFF};
  poke_active_file(dir, off, huge);
}

// A byte that is not a valid EntryType (valid are 0x01..0x05).
inline void poke_invalid_entry_type(const std::filesystem::path &dir,
                                    std::uint64_t off) {
  const std::array<unsigned char, 1> bad{0x7F};
  poke_active_file(dir, off, bad);
}

// Zero the 8-byte sequence, which the scan already treats as end of file.
inline void poke_zero_sequence(const std::filesystem::path &dir,
                               std::uint64_t off) {
  const std::array<unsigned char, 8> zero{};
  poke_active_file(dir, off, zero);
}

// ---- Vacuum baseline and helpers -----------------------------------------

// Owned snapshot of DB state before a vacuum operation.
struct VacuumBaseline {
  Baseline keys;
  std::map<std::uint32_t, FileStats> file_stats;  // pre-vacuum per-file stats
  std::map<EntryType, int> structural;            // marker / tombstone counts
};

// Counts the entries that carry no key data of their own: the BulkBegin and
// BulkEnd markers that make a batch atomic on disk, and range tombstones.
// Compaction must preserve all three, and no key-and-value assertion can see
// it when a retried compaction drops one.
inline auto count_structural_entries(const DB &db) -> std::map<EntryType, int> {
  std::map<EntryType, int> counts{{EntryType::BulkBegin, 0},
                                  {EntryType::BulkEnd, 0},
                                  {EntryType::RangeDel, 0}};
  auto snap = db.snapshot();
  for (const auto &e : db.changes_since(snap, 0)) {
    auto it = counts.find(e.entry_type);
    if (it != counts.end()) ++it->second;
  }
  return counts;
}

// Captures key-values, next_seq, per-file stats and structural entry counts.
inline auto capture_vacuum_baseline(const DB &db) -> VacuumBaseline {
  VacuumBaseline bl;
  bl.keys = capture_baseline(db);
  auto state = db.engine_state();
  for (const auto [file_id, fs] : state->file_stats) {
    bl.file_stats.emplace(file_id, fs);
  }
  bl.structural = count_structural_entries(db);
  return bl;
}

// Every batch marker and range tombstone the DB held before the vacuum is
// still there afterwards. Holds on both outcomes: a successful compaction
// copies them into the new file, and a failed one leaves the old file alone.
inline void assert_structural_entries_preserved(const DB &db,
                                                const VacuumBaseline &before) {
  const auto now = count_structural_entries(db);
  for (const auto &[type, count] : before.structural) {
    INFO("structural entry type " << static_cast<int>(type)
                                  << " must survive vacuum");
    CHECK(now.at(type) == count);
  }
}

// Returns the file_id of the sealed file vacuum() would select — the one with
// the highest fragmentation above 0, matching DB::vacuum()'s selection logic.
inline auto find_vacuum_target(const DB &db) -> std::uint32_t {
  auto state = db.engine_state();
  std::uint32_t target_id{};
  double worst_frag = 0.0;
  for (const auto [file_id, fs] : state->file_stats) {
    if (file_id == state->active_file_id) continue;
    if (fs.total_bytes == 0) continue;
    const double frag = 1.0 - static_cast<double>(fs.live_bytes) /
                                  static_cast<double>(fs.total_bytes);
    if (frag > worst_frag) {
      worst_frag = frag;
      target_id = file_id;
    }
  }
  if (worst_frag == 0.0) FAIL("no fragmented sealed file found for vacuum");
  return target_id;
}

// Verifies that vacuum succeeded: vacuumed_file_id removed from state, all
// pre-vacuum keys readable with correct values, next_seq unchanged,
// total_bytes for the active file matches its actual size (no staleness),
// assert_consistent passes.
inline void assert_vacuum_success(const DB &db, const VacuumBaseline &before,
                                  std::uint32_t vacuumed_file_id) {
  auto state = db.engine_state();

  // Vacuumed file must be gone.
  INFO("vacuumed file_id=" << vacuumed_file_id << " must be removed from state");
  CHECK_FALSE(state->files.contains(vacuumed_file_id));
  CHECK_FALSE(state->file_stats.contains(vacuumed_file_id));

  // next_seq unchanged — vacuum does not advance sequence.
  CHECK(state->next_seq == before.keys.next_seq);

  // All pre-vacuum keys must be present with correct values.
  for (const auto &[key, value] : before.keys.key_values) {
    INFO("pre-vacuum key must survive vacuum: " << key);
    CHECK(db.contains_key({}, to_bytes(key)));
    Bytes out;
    if (db.get({}, to_bytes(key), out)) {
      CHECK(out == value);
    }
  }

  // total_bytes for active file must match actual file size.
  auto active_id = state->active_file_id;
  auto actual_size = (*state->files.get(active_id))->size();
  INFO("active file total_bytes staleness check");
  CHECK(state->file_stats.get(active_id)->total_bytes == actual_size);

  assert_structural_entries_preserved(db, before);
  assert_consistent(db);
}

// Verifies that vacuum failed cleanly: vacuumed_file_id still in state, all
// pre-vacuum keys intact with correct values, next_seq unchanged,
// file_stats unchanged from baseline (no partial stat updates),
// total_bytes for active file matches its actual size, assert_consistent passes.
inline void assert_vacuum_no_change(const DB &db, const VacuumBaseline &before,
                                    std::uint32_t vacuumed_file_id) {
  auto state = db.engine_state();

  // Vacuumed file must still be present.
  INFO("vacuumed file_id=" << vacuumed_file_id << " must remain after failure");
  CHECK(state->files.contains(vacuumed_file_id));
  CHECK(state->file_stats.contains(vacuumed_file_id));

  // next_seq unchanged.
  CHECK(state->next_seq == before.keys.next_seq);

  // All pre-vacuum keys must be present with correct values.
  for (const auto &[key, value] : before.keys.key_values) {
    INFO("pre-vacuum key must survive failed vacuum: " << key);
    CHECK(db.contains_key({}, to_bytes(key)));
    Bytes out;
    if (db.get({}, to_bytes(key), out)) {
      CHECK(out == value);
    }
  }

  // file_stats must be unchanged from baseline (vacuum_commit never ran).
  for (const auto &[file_id, fs] : before.file_stats) {
    INFO("file_id=" << file_id << " file_stats must be unchanged after failed vacuum");
    if (state->file_stats.contains(file_id)) {
      CHECK(state->file_stats.get(file_id)->live_bytes == fs.live_bytes);
      CHECK(state->file_stats.get(file_id)->total_bytes == fs.total_bytes);
    }
  }

  // total_bytes for active file must match actual file size (no dead bytes
  // were written without updating stats).
  auto active_id = state->active_file_id;
  auto actual_size = (*state->files.get(active_id))->size();
  INFO("active file total_bytes staleness check after failed vacuum");
  CHECK(state->file_stats.get(active_id)->total_bytes == actual_size);

  assert_structural_entries_preserved(db, before);
  assert_consistent(db);
}

// Opens a fresh DB and verifies all pre-vacuum keys survive.
inline void assert_vacuum_recoverable(const std::filesystem::path &dir,
                                      const VacuumBaseline &before,
                                      const Options &opts = {}) {
  auto recovered = DB::open(dir, opts);
  for (const auto &[key, value] : before.keys.key_values) {
    INFO("key must survive vacuum and recovery: " << key);
    CHECK(recovered.contains_key({}, to_bytes(key)));
    Bytes out;
    if (recovered.get({}, to_bytes(key), out)) {
      CHECK(out == value);
    }
  }
  // Markers and range tombstones must survive the round trip through disk as
  // well as the vacuum itself: hint generation deliberately drops markers,
  // but the data file recovery reads them back from must still carry them.
  assert_structural_entries_preserved(recovered, before);
  assert_consistent(recovered);
}

// ---- Replication helpers ----------------------------------------------------

// Owned copies of entries collected from a ChangeIterator. The views
// returned by the iterator are transient — they point into the
// iterator's internal buffer and are invalidated on advance.
struct OwnedEntries {
  struct Entry {
    std::uint64_t sequence;
    EntryType entry_type;
    Bytes key;
    Bytes value;
  };
  std::vector<Entry> entries;

  // Builds a DataEntryView span referencing the owned data.
  [[nodiscard]] auto views() const -> std::vector<DataEntryView> {
    std::vector<DataEntryView> v;
    v.reserve(entries.size());
    for (const auto &e : entries) {
      v.push_back({e.sequence, e.entry_type, e.key, e.value});
    }
    return v;
  }
};

// Collects all entries from a changes_since range into owned storage.
template <typename Range>
inline auto collect_changes(Range &&range) -> OwnedEntries {
  OwnedEntries result;
  for (const auto &e : range) {
    result.entries.push_back({
        e.sequence,
        e.entry_type,
        Bytes{e.key.begin(), e.key.end()},
        Bytes{e.value.begin(), e.value.end()},
    });
  }
  return result;
}

// Snapshot of leader's durable state for replication tests.
// key_values is built by replaying changes_since(snap, 0) — this captures
// exactly the entries the replication pipeline would deliver to a follower.
// For nosync workloads, unsync'd entries are excluded (durable_seq boundary).
struct ReplicationBaseline {
  std::uint64_t durable_seq;
  std::uint64_t next_seq;
  std::map<std::string, Bytes> key_values;
};

inline auto capture_replication_baseline(const DB &db) -> ReplicationBaseline {
  ReplicationBaseline bl;
  auto state = db.engine_state();
  bl.durable_seq = state->durable_seq;
  bl.next_seq = state->next_seq;
  // Replay changes_since to build the exact key-value set that the
  // replication pipeline would deliver. This respects the durable_seq
  // boundary — unsync'd entries are excluded.
  auto snap = db.snapshot();
  for (const auto &e : db.changes_since(snap, 0)) {
    auto key_str = to_string(e.key);
    switch (e.entry_type) {
      case EntryType::Put:
        bl.key_values[key_str] = Bytes{e.value.begin(), e.value.end()};
        break;
      case EntryType::Delete:
        bl.key_values.erase(key_str);
        break;
      case EntryType::RangeDel: {
        auto to_str = to_string(e.value);
        auto it = bl.key_values.lower_bound(key_str);
        while (it != bl.key_values.end() && it->first < to_str) {
          it = bl.key_values.erase(it);
        }
        break;
      }
      case EntryType::BulkBegin:
      case EntryType::BulkEnd:
        break;
    }
  }
  return bl;
}

// Asserts that the follower matches the leader's baseline (SUCCESS case).
// Invariants 1-5 from the replication design doc.
inline void assert_replication_match(const ReplicationBaseline &leader,
                                     const DB &follower) {
  // 1. Sequence continuity.
  CHECK(follower.durable_sequence() == leader.durable_seq);

  // 2. next_seq monotonicity.
  auto fstate = follower.engine_state();
  CHECK(fstate->next_seq > 0);

  // 3. Key-value equivalence.
  for (const auto &[key, value] : leader.key_values) {
    INFO("leader key must exist on follower: " << key);
    Bytes out;
    REQUIRE(follower.get({}, to_bytes(key), out));
    CHECK(out == value);
  }

  // 4. No extra keys.
  for (const auto &fk : follower.keys_from({})) {
    auto key_str = to_string(fk);
    INFO("unexpected key on follower: " << key_str);
    CHECK(leader.key_values.contains(key_str));
  }

  // 5. Structural consistency.
  assert_consistent(follower);
}

// Asserts that the follower state is unchanged from its own baseline
// (failure case where no entries were published).
inline void assert_replication_no_change(const Baseline &before,
                                         const DB &follower) {
  auto fstate = follower.engine_state();

  // Key membership unchanged.
  for (const auto &[key, value] : before.key_values) {
    INFO("pre-ingest key must survive: " << key);
    Bytes out;
    REQUIRE(follower.get({}, to_bytes(key), out));
    CHECK(out == value);
  }
  for (const auto &fk : follower.keys_from({})) {
    auto key_str = to_string(fk);
    INFO("unexpected key on follower after failed ingest: " << key_str);
    CHECK(before.key_values.contains(key_str));
  }

  assert_consistent(follower);
}

// Reopens follower from disk and verifies replication survived recovery.
// Invariant 6 from the design doc.
inline void assert_replication_recovery(const std::filesystem::path &dir,
                                        const ReplicationBaseline &leader) {
  auto recovered = DB::open(dir);

  for (const auto &[key, value] : leader.key_values) {
    INFO("leader key must survive recovery: " << key);
    Bytes out;
    REQUIRE(recovered.get({}, to_bytes(key), out));
    CHECK(out == value);
  }
  for (const auto &rk : recovered.keys_from({})) {
    auto key_str = to_string(rk);
    INFO("unexpected key after recovery: " << key_str);
    CHECK(leader.key_values.contains(key_str));
  }

  assert_consistent(recovered);
}

// Asserts that all entries in a changes_since stream respect the durable
// boundary. Invariant 8 from the design doc.
template <typename Range>
inline void assert_durable_boundary(Range &&range,
                                    std::uint64_t durable_seq) {
  for (const auto &e : range) {
    INFO("entry seq=" << e.sequence << " exceeds durable_seq=" << durable_seq);
    CHECK(e.sequence <= durable_seq);
  }
}

}  // namespace bytecask::testing

#endif  // BYTECASK_TESTING
