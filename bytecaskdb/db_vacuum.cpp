// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — vacuum

module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ranges>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif

module bytecask;

namespace bytecask {

// Thread-safe: vacuum_mu_ serialises concurrent vacuum() calls independently
// from write_mu_, so normal put/del/apply_batch calls are not blocked while
// vacuum scans and rewrites data — only the brief commit step acquires
// write_mu_.
auto DB::vacuum(VacuumOptions opts) -> bool {
  std::lock_guard<std::mutex> vg{*vacuum_mu_};
  if (auto s = load_state(); s->closed) {
    throw DbClosed{};
  } else if (refused(*s)) {
    throw DbDegraded{degraded_reason()};
  }
  // Publishes scrape idle read caches; with no writes there are none, and
  // what the last writes retired stays pinned by whichever thread went idle
  // last. vacuum and stats are the entry points that keep running without
  // writes, so they scrape too.
  scrape_read_caches();

  // Drain in-flight background hint writes so that vacuum's
  // flush_hints_for call cannot race on the same .hint.tmp file.
  worker_.drain();

  // Snapshot file_stats and active-file info.
  FileStatsMap stats_snap;
  std::uint32_t active_id{};
  {
    auto s = load_state_for_write();
    stats_snap = s->file_stats;
    active_id = s->active_file_id;
  }

  // Sealed files above the threshold, most fragmented first. Tombstones and
  // batch markers count as kept, not as fragmentation: a file is compacted
  // for its dead Puts, so a file holding nothing else has nothing to
  // reclaim. Nor has a file whose every entry is above retain_after.
  std::vector<std::pair<double, std::uint32_t>> candidates;
  for (const auto [fid, fs] : stats_snap.all()) {
    if (fid == active_id) continue;
    if (fs.total_bytes == 0) continue;
    if (fs.min_sequence > opts.retain_after) continue;
    const auto frag = static_cast<double>(fs.reclaimable_bytes()) /
                      static_cast<double>(fs.total_bytes);
    if (frag > 0.0 && frag > opts.fragmentation_threshold)
      candidates.emplace_back(frag, fid);
  }
  std::ranges::sort(candidates, std::greater<>{});

  for (const auto &[frag, fid] : candidates) {
    const auto target = *stats_snap.get(fid);
    // Fast path: nothing in the file needs keeping — skip the scan and drop
    // it. A tombstone may need keeping even with no live key left: dropping
    // it could let recovery resurrect a Put it shadows in an older file, and
    // only compaction decides which tombstones can go. Nor may a file go
    // whole while it holds an entry above retain_after, or a change marker,
    // which is kept for ever.
    if (target.live_bytes == 0 && target.tombstone_bytes == 0 &&
        target.change_marker_bytes == 0 &&
        target.max_sequence <= opts.retain_after) {
      vacuum_remove_file(fid);
      return true;
    }
    // Compaction returns false when it cannot make the file smaller: every
    // dead entry in it may be above retain_after. The next candidate may
    // still have something to reclaim.
    if (vacuum_compact_file(fid, opts.retain_after)) return true;
  }
  return false;
}

// Scans source_file and copies live entries into dest_file.
// Live Puts (still current in snap->key_dir for source_file_id), the
// tombstones recovery found still needed or never examined, and
// BulkBegin/BulkEnd markers are emitted.
// Incomplete batches at EOF are silently discarded by the iterator.
auto DB::vacuum_scan_and_copy(
    const std::shared_ptr<const EngineState> &snap,
    const DataFile &source_file, WritableDataFile &dest_file,
    std::uint32_t source_file_id, const NeededTombstones &needed,
    std::uint64_t retain_after) -> VacuumScanResult {
  VacuumScanResult result;

  // The source is scanned in file order, where sequences ascend.
  auto track_seq = [&](std::uint64_t seq) {
    if (result.min_sequence == 0) result.min_sequence = seq;
    if (seq > result.max_sequence) result.max_sequence = seq;
  };

  auto emit_entry = [&](const DataEntry &entry, Offset entry_off) {
    switch (entry.entry_type) {
    case EntryType::Put: {
      const auto existing = kd_get(snap->key_dir, entry.key, snap->kd_ctx());
      // A location names one record, so an entry still at this one is this
      // record, sequence and all.
      if (existing && existing->file_id() == source_file_id &&
          existing->file_offset() == entry_off) {
        const auto new_off =
            dest_file.append_entry(entry.sequence, EntryType::Put, entry.key,
                             entry.value);
        const auto val_size = narrow<std::uint32_t>(entry.value.size());
        const auto sz = entry_size(entry.key.size(), entry.value.size());
        result.live_bytes += sz;
        result.total_bytes += sz;
        track_seq(entry.sequence);
        result.mappings.push_back({std::vector<std::byte>{entry.key.begin(),
                                                          entry.key.end()},
                                   new_off, entry.sequence, val_size});
      } else if (entry.sequence > retain_after) {
        // Dead, but a follower may still need it: kept as history. It is
        // counted in total_bytes and not in live_bytes, so it stays
        // reclaimable for a later vacuum that may drop it.
        std::ignore = dest_file.append_entry(entry.sequence, EntryType::Put,
                                             entry.key, entry.value);
        result.total_bytes += entry_size(entry.key.size(), entry.value.size());
        track_seq(entry.sequence);
      }
      break;
    }
    case EntryType::Delete:
    case EntryType::RangeDel: {
      // A tombstone no older Put in another file needs goes with its file.
      if (entry.sequence <= retain_after && needed.droppable(entry.sequence)) {
        ++result.tombstones_dropped;
        break;
      }
      std::ignore = dest_file.append_entry(entry.sequence, entry.entry_type,
                                           entry.key, entry.value);
      const auto sz = entry_size(entry.key.size(), entry.value.size());
      result.total_bytes += sz;
      result.tombstone_bytes += sz;
      track_seq(entry.sequence);
      break;
    }
    case EntryType::ChangeMarker: {
      // Kept for ever: the identity of the history from here on (#397).
      std::ignore = dest_file.append_entry(entry.sequence, entry.entry_type,
                                           {}, entry.value);
      const auto sz = entry_size(0, entry.value.size());
      result.total_bytes += sz;
      result.change_marker_bytes += sz;
      track_seq(entry.sequence);
      break;
    }
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      std::ignore =
          dest_file.append_entry(entry.sequence, entry.entry_type, {}, {});
      // A marker occupies a header and a CRC in the compacted file, exactly
      // as it did in the file the write path produced. Counting it keeps
      // total_bytes equal to the file's size on disk — which is what
      // recovery seeds it from — and keeps every published offset inside it.
      result.total_bytes += kHeaderSize + kCrcSize;
      result.marker_bytes += kHeaderSize + kCrcSize;
      track_seq(entry.sequence);
      break;
    }
  };

  for (const auto &[entry, entry_off] : scan_committed(source_file)) {
    emit_entry(entry, entry_off);
  }

  return result;
}

// Remaps key_dir entries from old_file_id to new_sealed_file, updates the
// files map and file_stats, and publishes the new EngineState. A null
// new_sealed_file removes old_file_id (see apply_vacuum). Caller must hold
// write_mu_.
void DB::vacuum_commit(std::uint32_t old_file_id,
                             const VacuumScanResult &scan,
                             std::shared_ptr<DataFile> new_sealed_file,
                             std::uint32_t dest_file_id,
                             std::uint64_t decided_at) {
  auto current = load_state_for_write();
  auto t = current->transient();
  // A record left out as superseded is gone for good once the source is
  // unlinked, but what superseded it may be a sync=false write a crash can
  // still lose, taking the key's last durable value with it (#245). So
  // everything the scan saw becomes durable before the source can go.
  if (t.durable_seq() < decided_at) {
    // A sync after a failed one proves nothing (#231).
    if (refused(*current)) throw DbDegraded{degraded_reason()};
    sync_active_file(t, current, "vacuum");
  }
  t.apply_vacuum(old_file_id, scan, std::move(new_sealed_file), dest_file_id);

  store_state(current, std::move(t).persistent());
}

// Unlinks the old data and hint files from the filesystem. Existing readers
// continue via their open fds (POSIX: pread succeeds on unlinked files).
void DB::vacuum_unlink_old_file(
    const std::shared_ptr<const EngineState> &snap, std::uint32_t file_id) {
#ifdef BYTECASK_TESTING
  // The state is committed and the compacted file is on disk; failing here
  // is the kill inside vacuum's publish window, with the source left behind.
  FAULT_INJECTION(io_vacuum_compact_unlink);
#endif
  auto old_data_file = *snap->files.get(file_id);
  auto old_hint_path =
      dir_ / (old_data_file->path().stem().string() + ".hint");
  std::filesystem::remove(old_data_file->path());
  std::filesystem::remove(old_hint_path);
  // Its frames would otherwise hold a frame each until the SIEVE hand came
  // round, which in a pool that rarely misses is a long time.
  if (pool_) {
    pool_->release_file(file_id, old_data_file->size());
  }
  change_index_->forget(file_id);
  counters_.vacuum_files_unlinked.fetch_add(1, std::memory_order_relaxed);
}

// Rewrites a sealed file into a new sealed file containing only live
// entries, markers and the tombstones still needed. Called under vacuum_mu_,
// not write_mu_.
// The new data file is written to .data.tmp, then renamed atomically.
// The old file is deferred for cleanup when no readers reference it.
auto DB::vacuum_compact_file(std::uint32_t file_id, std::uint64_t retain_after)
    -> bool {
  auto snap = load_state_for_write();
  // Checked again where the id is reserved; here so a vacuum that cannot
  // finish does not copy the file first.
  if (snap->next_file_id > KeyDirEntry::kMaxFileId) throw file_ids_exhausted();
  const auto &old_file = **snap->files.get(file_id);

  const auto stem = make_data_file_stem();
  const auto tmp_data_path = dir_ / (stem + ".data.tmp");
  const auto final_data_path = dir_ / (stem + ".data");

  // Until the commit publishes it, vacuum's copy is removed on every exit —
  // the early returns below and any failure: scan, copy, sync, shrink, the
  // rename, and after the rename the directory sync, the open, the hint and
  // the commit itself. A vacuum retried under a persistent fault (ENOSPC,
  // EMFILE) would otherwise leave a full copy behind per attempt, and the next
  // open runs a recovery pass per copy (#235, #304). Before the commit the
  // published state still holds the source, so nothing reads the copy. A
  // removal that fails under the same fault, or that a power cut loses, is
  // left to recovery, which deletes a .data.tmp and a placed copy beside its
  // source at open; it must not replace the exception in flight.
  struct CopyCleanup {
    std::vector<std::filesystem::path> paths;
    bool armed{true};
    ~CopyCleanup() {
      if (!armed) return;
      std::error_code ec;
      for (const auto &p : paths) std::filesystem::remove(p, ec);
    }
  } cleanup{{tmp_data_path}};

  VacuumScanResult scan;
  {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_vacuum_compact_tmp_create);
#endif
    auto tmp_file = createDataFileForWrite(
        dir_, stem, ".data.tmp", rotation_threshold_,
        stagingBackend(io_backend_));
    scan = vacuum_scan_and_copy(snap, old_file, *tmp_file, file_id,
                                needed_tombstones_, retain_after);
    tmp_file->sync();
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_vacuum_compact_shrink);
#endif
    tmp_file->shrink_to_fit();
  }

  // Nothing to reclaim: every byte in this file is live data, a needed
  // tombstone or a batch marker, and compaction must preserve all three.
  // Publishing an identical file would churn I/O for nothing.
  const auto old_total = snap->file_stats.get(file_id)->total_bytes;
  if (scan.total_bytes >= old_total) return false;
  // Nothing left at all: every Put was dead and every tombstone could go.
  // Remove the file rather than publish an empty one.
  if (scan.total_bytes == 0) {
    vacuum_remove_file(file_id);
    counters_.vacuum_tombstones_dropped.fetch_add(
        static_cast<std::int64_t>(scan.tombstones_dropped),
        std::memory_order_relaxed);
    return true;
  }

#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_vacuum_compact_rename);
#endif
  // Exclusive placement, not std::filesystem::rename: vacuum stages its copy
  // holding only vacuum_mu_, so a concurrent rotation can mint this stem
  // between here and the staging create. renameDataFileExclusive refuses the
  // target instead of replacing it.
  renameDataFileExclusive(tmp_data_path, final_data_path);
  // Placed: from here the copy is the final file and the hint flush_hints_for
  // writes for it, staged or renamed, hint first as recovery removes them.
  // Only once the rename succeeded: until then the final name may be a file
  // rotation minted. Frames the hint scan admits under the buffer pool are
  // left to eviction if the copy goes: their file id was reserved for it and
  // is never reused.
  cleanup.paths = {dir_ / (stem + ".hint.tmp"), dir_ / (stem + ".hint"),
                   final_data_path};
  // The source is unlinked once this commits. Were that unlink durable and
  // the rename not, the next open would find only .data.tmp, delete it as
  // staging, and lose every live entry the source held.
  sync_directory(dir_, "io_dir_sync_vacuum");
#ifdef BYTECASK_TESTING
  // *Orphaned .data files* in docs/correctness_validation.md: the rename
  // completed and the commit did not. The compacted file is on disk under its
  // final name while the old one is still the published state's; cleanup
  // removes it.
  FAULT_INJECTION(io_vacuum_compact_post_rename);
  if (test_before_vacuum_commit_) test_before_vacuum_commit_();
#endif

  // Reserve the destination id before opening the file, so the file carries
  // its engine file_id from construction — the buffer pool keys frames by it.
  // Reading next_file_id_ outside the barrier would race rotation, which
  // mints from the same counter, so the reservation is published first and
  // the commit below consumes it.
  std::uint32_t dest_file_id = 0;
  {
    WriteBarrier barrier{*this};
    auto current = load_state_for_write();
    auto t = current->transient();
    dest_file_id = t.reserve_file_id();
    store_state(current, std::move(t).persistent());
  }

  auto new_file = openDataFileForRead(final_data_path, io_backend_, pool_,
                                      dest_file_id);
  flush_hints_for(new_file, dir_);

  {
    WriteBarrier barrier{*this};
    vacuum_commit(file_id, scan, new_file, dest_file_id,
                  snap->next_seq > 0 ? snap->next_seq - 1 : 0);
  }
  // Published: the copy is the file now. A failure from here on, the source's
  // unlink, leaves the source beside it, which open resolves.
  cleanup.armed = false;
  // Bytes reclaimed = the shrinkage, not old_total - live_bytes: the compacted
  // file also carries the tombstones and markers that had to be preserved.
  counters_.vacuum_bytes_reclaimed.fetch_add(
      static_cast<std::int64_t>(old_total - scan.total_bytes),
      std::memory_order_relaxed);
  counters_.vacuum_tombstones_dropped.fetch_add(
      static_cast<std::int64_t>(scan.tombstones_dropped),
      std::memory_order_relaxed);
  counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
  vacuum_unlink_old_file(snap, file_id);
  return true;
}

// Removes a sealed file that has no live keys and nothing compaction would
// keep: no tombstone (vacuum() checks) or none still needed (the compaction
// that found its output empty). No I/O scan needed — just commit the state
// change and unlink the files. Called under vacuum_mu_.
void DB::vacuum_remove_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  auto old_total = snap->file_stats.get(file_id)->total_bytes;
  {
    WriteBarrier barrier{*this};
    VacuumScanResult empty{};
    // No new sealed file, so no id is consumed.
    vacuum_commit(file_id, empty, nullptr, 0,
                  snap->next_seq > 0 ? snap->next_seq - 1 : 0);
  }
  counters_.vacuum_bytes_reclaimed.fetch_add(
      static_cast<std::int64_t>(old_total), std::memory_order_relaxed);
  vacuum_unlink_old_file(snap, file_id);
}

} // namespace bytecask
