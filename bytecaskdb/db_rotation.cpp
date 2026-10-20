// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — file rotation and hint file generation

module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif

module bytecask;

namespace bytecask {

// Drops the old active file's preallocated tail, opens it read-only,
// dispatches hint generation, and opens a new writable active file.
// Caller must sync the active file before calling if durability is required.
auto DB::prepare_rotation(TransientEngineState &t) -> PreparedRotation {
  wait_for_hint_backlog();
  t.active_file().shrink_to_fit();
  auto read_only_old = openDataFileForRead(t.active_file().path(), io_backend_, pool_,
                                         t.active_file_id());
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_rotate_file_creation);
#endif
  const auto new_file_id = t.reserve_file_id();
  auto new_file = create_active_file(new_file_id, "io_dir_sync_rotate");
  return {std::move(read_only_old), std::move(new_file), new_file_id};
}

void DB::finish_rotation(TransientEngineState &t, PreparedRotation r) {
#ifdef BYTECASK_TESTING
  if (test_in_finish_rotation_) test_in_finish_rotation_();
#endif
  t.apply_rotate_file(r.sealed, std::move(r.next), r.next_id);
  // The sealed file's frames become evictable and the new file's pinned.
  if (pool_) pool_->set_active_file(r.next_id);
  dispatch_hint(std::move(r.sealed));
}

auto DB::create_active_file(std::uint32_t file_id, const char *checkpoint)
    -> std::shared_ptr<WritableDataFile> {
  auto file = createDataFileForWrite(dir_, make_data_file_stem(), ".data",
                                     rotation_threshold_, io_backend_, pool_,
                                     file_id);
  // On failure the created file is left behind empty, which is the state a
  // crash right after the create leaves too; the next open recovers it.
  sync_directory(dir_, checkpoint);
  return file;
}

// The wait runs on the writer's thread with the write path held, so every
// writer stalls until the worker catches up; readers never touch the worker.
// It cannot deadlock: a hint task takes no lock a writer holds —
// flush_hints_for is static and reads the file outside the buffer pool.
void DB::wait_for_hint_backlog() {
  if (max_hint_backlog_ == 0 || worker_.pending() < max_hint_backlog_) return;
  const auto start = std::chrono::steady_clock::now();
  worker_.wait_pending_below(max_hint_backlog_);
  const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);
  counters_.hint_backpressure_stalls.fetch_add(1, std::memory_order_relaxed);
  counters_.hint_backpressure_stall_us.fetch_add(waited.count(),
                                                 std::memory_order_relaxed);
}

void DB::dispatch_hint(std::shared_ptr<DataFile> file) {
#ifdef BYTECASK_TESTING
  worker_.dispatch([f = std::move(file), d = dir_, hook = test_before_hint_] {
    if (hook) hook();
    flush_hints_for(f, d);
  });
#else
  worker_.dispatch(
      [f = std::move(file), d = dir_] { flush_hints_for(f, d); });
#endif
}

// True when the error says a file's bytes are bad or cannot be delivered — a
// CRC or parse failure, EIO, a short read — which a rebuild answers for a
// hint and a skip for a data file. False for an error of the process or the
// environment (EMFILE, ENOMEM, EACCES), which says nothing about the file:
// acting on the file would turn a transient failure into a lost index, or a
// lost file.
static auto is_file_damage(const std::exception &e) noexcept -> bool {
  if (const auto *se = dynamic_cast<const std::system_error *>(&e))
    return se->code() == std::errc::io_error;
  return dynamic_cast<const std::runtime_error *>(&e) != nullptr;
}

auto DB::open_hint_or_rebuild(const std::shared_ptr<DataFile> &data_file,
                              const std::filesystem::path &hint_path)
    -> HintFile {
  try {
    return HintFile::OpenForRead(hint_path);
  } catch (const std::exception &e) {
    if (!is_file_damage(e)) throw;
    // flush_hints_for leaves an existing hint alone, so the damaged one has
    // to go first. Nothing is lost by removing it: it is unreadable either
    // way, and a rebuild that does not finish here leaves the file hint-less,
    // which the next open regenerates through the same scan.
    std::filesystem::remove(hint_path);
    flush_hints_for(data_file, hint_path.parent_path());
    auto hint = HintFile::OpenForRead(hint_path);
    // The tail-drop recovery_prepare_files does for a hint-less file is not
    // repeated here: a file that had a hint at all was sealed, and sealing
    // already gave its preallocated tail back.
    std::fprintf(stderr,
                 "bytecask: rebuilt hint file '%s' from its data file: %s\n",
                 hint_path.string().c_str(), e.what());
    return hint;
  }
}

auto DB::open_hint_or_skip(const std::shared_ptr<DataFile> &data_file,
                           const std::filesystem::path &hint_path, bool strict)
    -> std::optional<HintFile> {
  try {
    return open_hint_or_rebuild(data_file, hint_path);
  } catch (const std::exception &e) {
    if (strict || !is_file_damage(e)) throw;
    std::fprintf(stderr,
                 "bytecask: skipping data file for hint '%s' — could not "
                 "read it or rebuild it from the data file: %s\n",
                 hint_path.string().c_str(), e.what());
    return std::nullopt;
  }
}

auto DB::stage_hint(const DataFile &file, OnDamage on_damage) -> HintStage {
  // Put and Delete entries go out sorted by key, restoring what BC-088
  // introduced and 45d0e90 traded away for O(1) working memory. Sorting costs
  // one buffer per data file — bounded by max_file_bytes, not by the database
  // — and lets recovery bulk-load a B+ tree instead of inserting key by key.
  // Every reader resolves entries by sequence rather than by position, so the
  // order is free to serve the key directory being built.
  HintStage stage;
  const auto keep = [&](std::span<const std::byte> bytes) {
    stage.arena.insert(stage.arena.end(), bytes.begin(), bytes.end());
  };
  auto committed = scan_committed(file, 0, on_damage);
  auto it = committed.begin();
  for (; it != std::default_sentinel; ++it) {
    const auto &[entry, entry_off] = *it;
    HintStage::Entry e{.seq = entry.sequence,
                       .file_off = entry_off,
                       .val_size = 0,
                       .key_off = narrow<std::uint32_t>(stage.arena.size()),
                       .key_len = 0,
                       .type = entry.entry_type};
    switch (entry.entry_type) {
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      // Structural markers carry no key; recovery ignores them. They stay in
      // scan order ahead of the sorted run.
      stage.markers.push_back({entry.sequence, entry_off, entry.entry_type});
      break;
    case EntryType::ChangeMarker:
      // Recovery rebuilds the marker list from hints alone, so the id goes
      // with the entry. Keyless, like the batch markers, ahead of the run.
      e.val_size = narrow<std::uint32_t>(entry.value.size());
      keep(entry.value);
      stage.keyless.push_back(e);
      break;
    case EntryType::RangeDel:
      // A range tombstone's key is a range bound, not a key of the file, so
      // it must not join the sorted run — deduplicating by key would let it
      // collide with a real key. Recovery's tombstone handling is order
      // independent, so writing these first is safe.
      e.key_len = narrow<std::uint32_t>(entry.key.size());
      e.val_size = narrow<std::uint32_t>(entry.value.size());
      keep(entry.key);
      keep(entry.value);
      stage.keyless.push_back(e);
      break;
    case EntryType::Put:
    case EntryType::Delete:
      e.key_len = narrow<std::uint32_t>(entry.key.size());
      e.val_size = narrow<std::uint32_t>(entry.value.size());
      keep(entry.key);
      stage.keyed.push_back(e);
      break;
    }
  }
  stage.end = it.committed_offset();
  return stage;
}

void DB::write_hint(HintStage stage, const std::filesystem::path &dir,
                    const std::string &stem) {
  const auto hint_path = dir / (stem + ".hint");
  const auto tmp_path = dir / (stem + ".hint.tmp");
  const auto bytes = [&](std::uint32_t off, std::uint32_t len) {
    return std::span<const std::byte>{stage.arena.data() + off, len};
  };
  const auto key_of = [&](const HintStage::Entry &e) {
    return bytes(e.key_off, e.key_len);
  };
  // Key ascending, and within a key sequence descending, so the first entry
  // for each key is the authoritative one.
  std::ranges::sort(stage.keyed, [&](const auto &a, const auto &b) {
    const auto c = recovery_key_cmp(key_of(a), key_of(b));
    return c < 0 || (c == 0 && a.seq > b.seq);
  });

  const auto value_of = [&](const HintStage::Entry &e) {
    return bytes(e.key_off + e.key_len, e.val_size);
  };

  auto hint = HintFile::OpenForWrite(tmp_path);
  // The keyless entries go first in scan order: the batch markers and the
  // rest, merged back by their offsets in the file.
  auto marker = stage.markers.begin();
  const auto put_markers_before = [&](std::uint64_t file_off) {
    for (; marker != stage.markers.end() && marker->file_off < file_off;
         ++marker)
      hint.append(marker->seq, marker->type, marker->file_off, {}, 0);
  };
  for (const auto &e : stage.keyless) {
    put_markers_before(e.file_off);
    switch (e.type) {
    case EntryType::ChangeMarker:
      hint.append_change_marker(e.seq, e.file_off,
                                decode_marker_id(value_of(e)));
      break;
    case EntryType::RangeDel:
      hint.append_range_del(e.seq, e.file_off, key_of(e), value_of(e));
      break;
    case EntryType::Put:
    case EntryType::Delete:
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      panic("write_hint: stage_hint keeps keyed entries and batch markers "
            "out of the keyless ones");
    }
  }
  put_markers_before(std::numeric_limits<std::uint64_t>::max());
  // BC-088 also deduplicated, keeping the highest-sequence entry per key.
  // That is not done here: file_stats.min_sequence and max_sequence are
  // rebuilt at recovery from the entries the hint file still holds, and
  // ChangeIterator selects data files by that range. Dropping an entry can
  // raise min_sequence above a sequence the data file really contains, and
  // replication would then skip the file. Recovering it needs the bounds in
  // the hint header, which is a format change.
  for (const auto &e : stage.keyed)
    hint.append(e.seq, e.type, e.file_off, key_of(e), e.val_size);

  hint.close();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_hint_rename);
#endif
  std::filesystem::rename(tmp_path, hint_path);
  // Only restart time depends on this: a lost rename leaves the file
  // hint-less, and the next open rebuilds the hint from it.
  sync_directory(dir, "io_dir_sync_hint");
}

void DB::flush_hints_for(const std::shared_ptr<DataFile> &file,
                         const std::filesystem::path &dir) {
  const auto stem = file->path().stem().string();
  if (std::filesystem::exists(dir / (stem + ".hint"))) return;
  write_hint(stage_hint(*file, OnDamage::Throw), dir, stem);
}

// Writes hint files for all sealed data files in the given state.
void DB::flush_hints(const EngineState &s) {
  for (const auto [file_id, file] : s.files) {
    if (file_id == s.active_file_id) {
      continue;
    }
    flush_hints_for(file, dir_);
  }
}

// Drains background hint tasks then writes hint files for all sealed files.
void DB::flush_hints() {
  worker_.drain();
  flush_hints(*load_state());
}

} // namespace bytecask
