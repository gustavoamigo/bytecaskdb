// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — the degraded state: syncing the active file, degrading and resume()

module;
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif

module bytecask;

namespace bytecask {

void DB::sync_active_file(TransientEngineState &t,
                          const std::shared_ptr<const EngineState> &current,
                          std::string_view caller) {
  try {
    t.active_file().sync();
    counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
  } catch (...) {
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    degrade_transient(t, current, [&] {
      return std::format(
          "{} fdatasync failed on '{}': writes acknowledged without sync are "
          "not confirmed durable. Call resume() to recover.",
          caller, t.active_file().path().string());
    });
    throw;
  }
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
}

void DB::deem_as_degraded(std::string reason) {
  store_state(load_state()->degraded_copy(std::move(reason)));
  // A waiter on a publication that will now never come re-checks and sees
  // the degrade. Not under durable_mu_ here — no caller holds it.
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

void DB::resume() {
  if (auto s = load_state(); s->closed) {
    throw DbClosed{};
  } else if (!refused(*s)) {
    return;
  }

  WriteBarrier barrier{*this};
  auto current = load_state_for_write();
  if (current->closed) throw DbClosed{};
  if (!refused(*current)) return;  // re-check under lock

  // The failed flush left one or two heads derived from the published
  // state alive in head_. The key directory derives a version only from
  // the end of its chain, so drop them now — ~FlushRole would only do it at
  // the end of the barrier — and the resumed state is derived from the
  // published one with those heads already reclaimed.
  store_head(current);

  // resume() seals the active file like a rotation does.
  wait_for_hint_backlog();

  auto t = current->transient();
  // resume() seals the active file and needs an id for the next one; without
  // it nothing is touched and the engine stays degraded until a reopen.
  if (t.file_ids_left() == 0) throw file_ids_exhausted();
  const auto old_file_id = t.active_file_id();
  auto &file = t.active_file();

  // The degrade may be a failed fdatasync, which on Linux leaves the pages
  // it covered clean without writing them (#231): every byte appended since
  // the last successful sync, sync=false writes included, reads back from
  // the cache and is not on the device, and file.sync() below would find
  // nothing to write. Rewriting the file and syncing it makes what reads
  // return durable before the scan reads it (throws → stays degraded). Up to
  // the logical end, where the scan stops: past it lie only the zero-filled
  // preallocation and the bytes of an append that failed.
  rewrite_durably(file.path(), file.size());

  // Scan the active file to find the last valid committed offset
  // and collect valid committed entries for key_dir replay. Entries written to
  // disk but never published to EngineState (sync-failure paths, degraded
  // transitions between IO and state publication) would otherwise be invisible
  // until cold restart. The scan goes through read_raw, a pread of the page
  // cache on every back-end — never the buffer pool's frames, which hold what
  // was appended rather than what the rewrite found and made durable.
  Offset valid_offset = 0;
  std::vector<ResumeEntry> committed;
  try {
    auto iter = CommittedEntryIterator{DataFileIterator{file}};
    while (!(iter == std::default_sentinel)) {
      const auto &[entry, entry_off] = *iter;
      // Batch markers are collected like everything else. They have no key
      // directory effect — apply_resume says so once, in its switch — but
      // they do consume sequences, and the file's min/max bounds have to
      // count them or resume reports a min_sequence above a sequence the
      // file really contains. Hint files carry markers for the same reason,
      // so dropping them here made resume and a cold open disagree.
      // A range tombstone carries its exclusive upper bound in the value,
      // and apply_resume needs both bounds to suppress the range.
      auto range_end = entry.entry_type == EntryType::RangeDel
                           ? std::vector<std::byte>{entry.value.begin(),
                                                    entry.value.end()}
                           : std::vector<std::byte>{};
      committed.push_back({entry.sequence, entry.entry_type, entry_off,
                           narrow<std::uint32_t>(entry.value.size()),
                           {entry.key.begin(), entry.key.end()},
                           std::move(range_end),
                           entry.entry_type == EntryType::ChangeMarker
                               ? decode_marker_id(entry.value)
                               : 0});
      // Every entry yielded lies below the iterator's committed offset, so
      // recording it here keeps valid_offset in step with `committed` even
      // when the next entry is corrupt and ++iter throws.
      valid_offset = iter.committed_offset();
      ++iter;
    }
  } catch (const std::system_error &) {
    // An I/O error says nothing about the bytes — the next attempt may read
    // them fine. Truncating on it would destroy data over a transient fault.
    throw;  // stays degraded
  } catch (const std::runtime_error &) {
    // The scan stopped at an entry that does not parse. valid_offset is the
    // end of the last committed entry or batch before it; whether that is
    // a torn tail to trim or damage to refuse is decided below.
  }
  const auto active_stats = current->file_stats.get(old_file_id);
  const auto published_extent = active_stats ? active_stats->total_bytes : 0;
  // resume() trims what a failed write left behind: bytes appended but never
  // published. Everything below the published extent was acknowledged, and a
  // scan that stops short of it has found bytes readers were served that the
  // file no longer holds: damage, or sync=false writes a failed fdatasync
  // left undurable and the kernel evicted before the rewrite read them. The
  // key directory holds no older version to fall back to for the keys they
  // overwrote, so there is no consistent state to resume into, and resume()
  // refuses — before truncating, so the file is left exactly as it was found
  // and the engine stays degraded. A reopen recovers the state the device
  // holds. This is detection, not repair: resume() makes no promise about
  // what a damaged file still holds, only that it will not truncate
  // acknowledged bytes or report success over them.
  if (valid_offset < published_extent) {
    throw std::runtime_error{std::format(
        "resume: active file '{}' ends at offset {}, inside data already "
        "published (up to {}): damaged, or unsynced writes lost after a "
        "failed fdatasync; refusing to truncate it. Reopen the database to "
        "recover what the file holds",
        file.path().string(), valid_offset, published_extent)};
  }

  // Remove garbage bytes / orphaned batch markers via truncation.
  if (std::filesystem::file_size(file.path()) != valid_offset) {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_resume_truncate);
#endif
    file.truncate(valid_offset);  // throws std::system_error → stays degraded
  }

  // Sync the truncated file (may throw → stays degraded).
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_resume_sync);
#endif
  file.sync();

  // Open the old active as read-only for hint generation.
  auto read_only_old = openDataFileForRead(file.path(), io_backend_, pool_,
                                         old_file_id);

  // Dispatch hint generation — idempotent (flush_hints_for skips files
  // whose .hint already exists).
  dispatch_hint(read_only_old);

  // Create the new active file (may throw → stays degraded).
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_resume_file_creation);
#endif
  const auto new_file_id = t.reserve_file_id();
  auto new_file = create_active_file(new_file_id, "io_dir_sync_resume");

  // Build and publish new state. Replay scanned entries into key_dir so that
  // entries on disk but not yet in EngineState become visible.
  t.apply_resume(old_file_id, committed, valid_offset);
  // The replay can give keys entries at or below a sequence a snapshot of
  // the degraded state already named, so no plan read before it can be
  // checked by sequence.
  deletes_.reset(t.next_seq() - 1);
  t.apply_rotate_file(std::move(read_only_old), std::move(new_file),
                      new_file_id);
  if (pool_) pool_->set_active_file(new_file_id);
  // Every entry replayed was read after rewrite_durably synced it.
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
  t.apply_clear_degraded();
  auto resumed = std::move(t).persistent();
#ifdef BYTECASK_TESTING
  if (test_before_validate_) test_before_validate_(*resumed);
#endif
  validate_state_consistency(*resumed);
  store_state(current, std::move(resumed));
  // The bookmarks of the file just cut all named durable units below the cut,
  // but a cache that rebuilds from a scan needs no argument to stay right.
  change_index_->clear();
  // Writers appended since the failed flush have all been told; new ones
  // start clean.
  std::lock_guard<std::mutex> lk{durable_mu_};
  flush_error_ = nullptr;
  write_fault_.store(false, std::memory_order_release);
}

} // namespace bytecask
