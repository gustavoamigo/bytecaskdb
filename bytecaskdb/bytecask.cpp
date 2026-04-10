// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — engine implementation: open, close, read, write, recovery, compaction

module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <time.h>
#include <utility>
#include <variant>
#include <vector>

module bytecask;

import bytecask.concurrency;
import :internals;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.hint_file;
import bytecask.radix_tree;
import bytecask.types;
import bytecask.util;

namespace bytecask {

#pragma region Internal helpers

namespace {

  // Generates a data file stem using a microsecond-precision UTC timestamp.
// Format: "data_{YYYYMMDDHHmmssUUUUUU}"
auto make_data_file_stem() -> std::string {
  const auto now = std::chrono::system_clock::now();
  const auto us_total = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
  const auto subsec_us = us_total % 1'000'000;

  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  ::gmtime_r(&tt, &tm_buf);

  return std::format("data_{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}{:06d}",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, subsec_us);
}

// Nanoseconds since steady_clock epoch. Timestamps state publications;
// readers compare against this with a single relaxed load (plain MOV on x86).
auto now_ns() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

// Engine-specific WriteGroup slot. Extends the generic Slot with a typed
// lambda that operates on EngineState and a shared TransientRadixTree.
// Stack-allocated by each caller — no heap allocation per writer.
struct EngineSlot : WriteGroup::Slot {
  std::move_only_function<void(EngineState &,
                               TransientRadixTree<KeyDirEntry> &)>
      fn;
};

#pragma endregion

#pragma region Engine State
// EngineState::apply_rotation is defined here because it needs make_data_file_stem.
auto EngineState::apply_rotation(const std::filesystem::path &dir) const
    -> EngineState {
  auto s = *this;
  s.active_file_id = s.next_file_id++;
  const auto stem = make_data_file_stem();
  auto next_files =
      std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
          *s.files);
  next_files->emplace(s.active_file_id,
                      std::make_shared<DataFile>(dir / (stem + ".data")));
  s.files = std::move(next_files);
  return s;
}
#pragma endregion

#pragma region Construction

// Opens dir, runs recovery, creates initial active data file.
// Initialises file_stats_ for the new active file.
// Throws std::system_error if the directory cannot be prepared.
DB::DB(std::filesystem::path dir, Options opts)
    : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
      state_{std::make_shared<EngineState>()},
      write_group_{[this](auto &batch) { execute_write_batch(batch); }} {
  std::filesystem::create_directories(dir_);
  EngineState s;
  s.files =
      std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>();
  if (opts.recovery_threads <= 1) {
    s = recovery_load_serial(std::move(s));
  } else {
    s = recovery_load_parallel(std::move(s), opts.recovery_threads);
  }
  s.active_file_id = s.next_file_id++;
  const auto stem = make_data_file_stem();
  s.files->emplace(s.active_file_id,
                   std::make_shared<DataFile>(dir_ / (stem + ".data")));
  file_stats_[s.active_file_id] = FileStats{};
  state_.store(std::make_shared<EngineState>(std::move(s)));
  state_time_.store(now_ns(), std::memory_order_release);
}

#pragma endregion

#pragma region Lifecycle

// Seals the active file, drains background hint tasks, writes hint files for
// all sealed files, then purges stale files.
// At destruction no readers are active.
DB::~DB() {
  auto s = state_.load();
  if (s->files && !s->files->empty()) {
    try {
      auto &active = *s->files->at(s->active_file_id);
      active.sync();
      active.seal();
    } catch (...) {}
  }
  try {
    flush_hints();
  } catch (...) {}
  for (auto &sf : stale_files_) {
    try {
      auto path = sf.data_file->path();
      sf.data_file.reset();
      std::filesystem::remove(path);
      std::filesystem::remove(sf.hint_path);
    } catch (...) {}
  }
}

#pragma endregion

#pragma region Primary operations

// Writes the value for key into out, reusing its existing capacity to
// amortize allocation across calls. Returns true if the key was found,
// false otherwise.
// Routes the read to the correct data file via KeyDirEntry::file_id.
// Throws std::system_error on I/O failure or std::runtime_error on CRC
// mismatch.
auto DB::get(const ReadOptions &opts, BytesView key,
                   Bytes &out) const -> bool {
  auto s = load_state(opts);
  const auto kv = s->key_dir.get(key);
  if (!kv) {
    return false;
  }
  // Per-thread I/O scratch buffer — reused across calls to avoid heap churn.
  // Thread-exit destructor is intentional; suppress the Clang diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local Bytes io_buf;
#pragma clang diagnostic pop
  s->files->at(kv->file_id)
      ->read_value(kv->file_offset, narrow<std::uint16_t>(key.size()),
                   kv->value_size, opts.verify_checksums, io_buf, out);
  return true;
}

// Writes key → value. Overwrites any existing value.
// Rotates the active file if it has reached the threshold.
// opts.sync controls whether fdatasync is called after the write.
// Throws std::system_error on I/O failure or lock contention (try_lock).
void DB::put(const WriteOptions &opts, BytesView key, BytesView value) {
  EngineSlot slot;
  slot.sync = opts.sync;
  slot.fn = [&](EngineState &s, TransientRadixTree<KeyDirEntry> &t) {
    // Phase 1: I/O — can throw without corrupting in-memory state.
    const auto offset =
        s.active_file().append(s.next_lsn, EntryType::Put, key, value);
    // Phase 2: in-memory mutations — cannot fail.
    const auto existing = t.get(key);
    if (existing) stats_retire_entry(key, *existing);
    stats_publish_put(s.active_file_id, key, value);
    t.set(key, KeyDirEntry{s.next_lsn, s.active_file_id, offset,
                           narrow<std::uint32_t>(value.size())});
    ++s.next_lsn;
  };
  write_group_.submit(slot);
}

// Writes a tombstone for key.
// Returns true if the key existed and was removed, false if it was absent.
// Rotates the active file if it has reached the threshold.
// opts.sync controls whether fdatasync is called after the write.
// Throws std::system_error on I/O failure or lock contention (try_lock).
auto DB::del(const WriteOptions &opts, BytesView key) -> bool {
  auto existed = false;
  EngineSlot slot;
  slot.sync = opts.sync;
  slot.fn = [&](EngineState &s, TransientRadixTree<KeyDirEntry> &t) {
    const auto existing = t.get(key);
    if (!existing) return;
    // Phase 1: I/O — can throw without corrupting in-memory state.
    std::ignore =
        s.active_file().append(s.next_lsn, EntryType::Delete, key, {});
    // Phase 2: in-memory mutations — cannot fail.
    existed = true;
    stats_retire_entry(key, *existing);
    stats_publish_tombstone(s.active_file_id, key);
    ++s.next_lsn;
    t.erase(key);
  };
  write_group_.submit(slot);
  return existed;
}

auto DB::contains_key(BytesView key) const -> bool {
  auto s = state_.load();
  return s->key_dir.contains(key);
}

// Atomically applies all operations in batch, wrapped in BulkBegin/BulkEnd.
// batch is consumed (move-only). No-op if batch.empty().
// opts.sync controls whether a single fdatasync is issued at the end.
// Rotates the active file after the sync if the threshold is reached.
// Throws std::system_error on I/O failure or lock contention (try_lock).
void DB::apply_batch(const WriteOptions &opts, Batch batch) {
  if (batch.empty()) return;
  EngineSlot slot;
  slot.sync = opts.sync;
  slot.fn = [&](EngineState &s, TransientRadixTree<KeyDirEntry> &t) {
    const bool multi = batch.size() > 1;
    if (multi) {
      const auto bulk_begin_lsn = s.next_lsn;
      std::ignore =
          s.active_file().append(bulk_begin_lsn, EntryType::BulkBegin, {}, {});
      stats_publish_bulk_marker(s.active_file_id);
      ++s.next_lsn;
    }

    // Phase 1: ALL I/O — can throw, exits cleanly (no mutations yet).
    struct Deferred {
      BytesView key;
      std::uint64_t offset;
      std::uint64_t lsn;
      std::uint32_t val_size;
      bool is_put;
    };
    std::vector<Deferred> ops;
    ops.reserve(batch.size());

    for (auto &op : batch.operations_) {
      std::visit(
          [&](auto &o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, BatchInsert>) {
              const std::span<const std::byte> key_span{o.key};
              const std::span<const std::byte> val_span{o.value};
              const auto offset = s.active_file().append(
                  s.next_lsn, EntryType::Put, key_span, val_span);
              ops.push_back({key_span, offset, s.next_lsn,
                             narrow<std::uint32_t>(o.value.size()), true});
            } else if constexpr (std::is_same_v<T, BatchRemove>) {
              const std::span<const std::byte> key_span{o.key};
              std::ignore = s.active_file().append(s.next_lsn,
                                                   EntryType::Delete,
                                                   key_span, {});
              ops.push_back({key_span, 0, s.next_lsn, 0, false});
            }
            ++s.next_lsn;
          },
          op);
    }

    if (multi) {
      const auto bulk_end_lsn = s.next_lsn;
      std::ignore =
          s.active_file().append(bulk_end_lsn, EntryType::BulkEnd, {}, {});
      stats_publish_bulk_marker(s.active_file_id);
      ++s.next_lsn;
    }

    // Phase 2: ALL mutations — pure in-memory, cannot fail.
    for (auto &d : ops) {
      const auto existing = t.get(d.key);
      if (existing) stats_retire_entry(d.key, *existing);
      if (d.is_put) {
        stats_publish_put(s.active_file_id, d.key, d.val_size);
        t.set(d.key, KeyDirEntry{d.lsn, s.active_file_id, d.offset,
                                 d.val_size});
      } else {
        stats_publish_tombstone(s.active_file_id, d.key);
        t.erase(d.key);
      }
    }
  };
  write_group_.submit(slot);
}

#pragma endregion

#pragma region Snapshot and apply_batch_if

auto DB::snapshot() const -> Snapshot { return Snapshot{state_.load()}; }

// Solo path: evaluates guards and W-W checks under write_mu_, then inlines
// batch commit if all checks pass. Does not use WriteGroup (needs lower_bound
// for range guards). Returns true if committed, false on conflict.
auto DB::apply_batch_if(const Snapshot &snap, WriteOptions opts,
                        WritePlan plan) -> bool {
  if (plan.empty()) return true;

  {
    auto guard = acquire_write_lock(opts);
    auto current = state_.load();

    // 1. Point guards.
    for (const auto &[key, action] : plan.actions_) {
      const std::span<const std::byte> key_span{key};
      const auto snap_entry = snap.state_->key_dir.get(key_span);
      const auto cur_entry = current->key_dir.get(key_span);

      switch (action.precondition) {
      case WritePlan::Precondition::MustExist:
        if (!cur_entry) return false;
        break;
      case WritePlan::Precondition::MustBeAbsent:
        if (cur_entry) return false;
        break;
      case WritePlan::Precondition::MustBeUnchanged: {
        const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence : 0;
        const std::uint64_t cur_seq = cur_entry ? cur_entry->sequence : 0;
        if (cur_seq != snap_seq) return false;
        break;
      }
      case WritePlan::Precondition::None:
        break;
      }
    }

    // 2. Range guards.
    for (const auto &rg : plan.range_guards_) {
      const std::span<const std::byte> from_span{rg.from};
      const std::span<const std::byte> to_span{rg.to};

      // Check current state for keys modified since snapshot.
      for (auto it = current->key_dir.lower_bound(from_span);
           it != std::default_sentinel; ++it) {
        auto [key_span, entry] = *it;
        if (Key{key_span} >= Key{to_span}) break;
        const auto snap_entry = snap.state_->key_dir.get(key_span);
        const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence : 0;
        if (entry.sequence != snap_seq) return false;
      }

      // Check snapshot for keys deleted since snapshot.
      for (auto it = snap.state_->key_dir.lower_bound(from_span);
           it != std::default_sentinel; ++it) {
        auto [key_span, entry] = *it;
        if (Key{key_span} >= Key{to_span}) break;
        if (!current->key_dir.get(key_span)) return false;
      }
    }

    // 3. Implicit W-W check on all write keys.
    for (const auto &[key, action] : plan.actions_) {
      if (action.write == WritePlan::Write::None) continue;
      const std::span<const std::byte> key_span{key};
      const auto snap_entry = snap.state_->key_dir.get(key_span);
      const auto cur_entry = current->key_dir.get(key_span);
      const bool appeared = !snap_entry && cur_entry;
      const bool deleted = snap_entry && !cur_entry;
      const bool modified = snap_entry && cur_entry &&
                            cur_entry->sequence != snap_entry->sequence;
      if (appeared || deleted || modified) return false;
    }

    // All checks passed — convert WritePlan writes to Batch and commit.
    Batch batch;
    for (auto &[key, action] : plan.actions_) {
      if (action.write == WritePlan::Write::Put) {
        batch.put(std::span<const std::byte>{key},
                  std::span<const std::byte>{action.value});
      } else if (action.write == WritePlan::Write::Del) {
        batch.del(std::span<const std::byte>{key});
      }
    }

    if (!batch.empty()) {
      // Inline batch commit (solo path — not through WriteGroup).
      auto s = *current;
      const bool multi = batch.size() > 1;
      if (multi) {
        const auto bulk_begin_lsn = s.next_lsn;
        std::ignore =
            s.active_file().append(bulk_begin_lsn, EntryType::BulkBegin, {}, {});
        stats_publish_bulk_marker(s.active_file_id);
        ++s.next_lsn;
      }

      // Phase 1: ALL I/O — can throw without corrupting in-memory state.
      struct Deferred {
        std::span<const std::byte> key;
        std::uint64_t offset;
        std::uint64_t lsn;
        std::uint32_t val_size;
        bool is_put;
      };
      std::vector<Deferred> ops;
      ops.reserve(batch.size());

      for (auto &op : batch.operations_) {
        std::visit(
            [&](auto &o) {
              using T = std::decay_t<decltype(o)>;
              if constexpr (std::is_same_v<T, BatchInsert>) {
                const std::span<const std::byte> key_span{o.key};
                const std::span<const std::byte> val_span{o.value};
                const auto offset = s.active_file().append(
                    s.next_lsn, EntryType::Put, key_span, val_span);
                ops.push_back({key_span, offset, s.next_lsn,
                               narrow<std::uint32_t>(o.value.size()), true});
              } else if constexpr (std::is_same_v<T, BatchRemove>) {
                const std::span<const std::byte> key_span{o.key};
                std::ignore = s.active_file().append(
                    s.next_lsn, EntryType::Delete, key_span, {});
                ops.push_back({key_span, 0, s.next_lsn, 0, false});
              }
              ++s.next_lsn;
            },
            op);
      }

      if (multi) {
        const auto bulk_end_lsn = s.next_lsn;
        std::ignore =
            s.active_file().append(bulk_end_lsn, EntryType::BulkEnd, {}, {});
        ++s.next_lsn;
        stats_publish_bulk_marker(s.active_file_id);
      }

      // Phase 2: ALL mutations — pure in-memory, cannot fail.
      auto t = s.key_dir.transient();
      for (auto &d : ops) {
        const auto existing = t.get(d.key);
        if (existing) stats_retire_entry(d.key, *existing);
        if (d.is_put) {
          stats_publish_put(s.active_file_id, d.key, d.val_size);
          t.set(d.key,
                KeyDirEntry{d.lsn, s.active_file_id, d.offset, d.val_size});
        } else {
          stats_publish_tombstone(s.active_file_id, d.key);
          t.erase(d.key);
        }
      }

      s.key_dir = std::move(t).persistent();
      s = rotate_if_needed(std::move(s));
      if (opts.sync) {
        s.active_file().sync();
      }
      state_.store(std::make_shared<EngineState>(std::move(s)));
      state_time_.store(now_ns(), std::memory_order_release);
    }
  }
  return true;
}

#pragma endregion

#pragma region Snapshot read methods

auto Snapshot::contains_key(BytesView key) const -> bool {
  return state_->key_dir.contains(key);
}

// Reads the value for key from the frozen snapshot state into out.
// Thread-local I/O buffer reused across calls to amortize allocation.
auto Snapshot::get(BytesView key, Bytes &out) const -> bool {
  const auto kv = state_->key_dir.get(key);
  if (!kv) return false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local Bytes io_buf;
#pragma clang diagnostic pop
  state_->files->at(kv->file_id)
      ->read_value(kv->file_offset, narrow<std::uint16_t>(key.size()),
                   kv->value_size, /*verify_checksums=*/true, io_buf, out);
  return true;
}

auto Snapshot::iter_from(BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto it =
      from.empty() ? state_->key_dir.begin() : state_->key_dir.lower_bound(from);
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{state_, std::move(it), /*verify_checksums=*/true},
      std::default_sentinel};
}

auto Snapshot::keys_from(BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto it =
      from.empty() ? state_->key_dir.begin() : state_->key_dir.lower_bound(from);
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto Snapshot::riter_from(BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator> {
  auto begin_it = from.empty()
      ? state_->key_dir.rbegin().base()
      : state_->key_dir.upper_bound(from);
  auto end_it = state_->key_dir.begin();
  return {ReverseEntryIterator{EntryIterator{state_, std::move(begin_it), /*verify_checksums=*/true}},
          ReverseEntryIterator{EntryIterator{state_, std::move(end_it), /*verify_checksums=*/true}}};
}

auto Snapshot::rkeys_from(BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto begin_it = from.empty()
      ? state_->key_dir.rbegin().base()
      : state_->key_dir.upper_bound(from);
  auto end_it = state_->key_dir.begin();
  return {ReverseKeyIterator{KeyIterator{std::move(begin_it)}},
          ReverseKeyIterator{KeyIterator{std::move(end_it)}}};
}

#pragma endregion

#pragma region Range iteration

// Returns an input range of (key, value) pairs with keys >= from.
// Pass an empty span to start from the first key. Each dereference reads
// one value from disk via a single pread (lazy). Results are in ascending
// key order.
// Throws std::system_error on I/O failure.
auto DB::iter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto s = state_.load();
  auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{s, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

// Returns an input range of keys >= from. Walks the in-memory key directory
// only; no disk I/O.
auto DB::keys_from(const ReadOptions & /*opts*/, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto s = state_.load();
  auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto DB::riter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator> {
  auto s = state_.load();
  auto begin_it = from.empty()
      ? s->key_dir.rbegin().base()
      : s->key_dir.upper_bound(from);
  auto end_it = s->key_dir.begin();
  return {ReverseEntryIterator{EntryIterator{s, std::move(begin_it), opts.verify_checksums}},
          ReverseEntryIterator{EntryIterator{s, std::move(end_it), opts.verify_checksums}}};
}

auto DB::rkeys_from(const ReadOptions & /*opts*/, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto s = state_.load();
  auto begin_it = from.empty()
      ? s->key_dir.rbegin().base()
      : s->key_dir.upper_bound(from);
  auto end_it = s->key_dir.begin();
  return {ReverseKeyIterator{KeyIterator{std::move(begin_it)}},
          ReverseKeyIterator{KeyIterator{std::move(end_it)}}};
}

#pragma endregion

#pragma region Vacuum

// Thread-safe: vacuum_mu_ serialises concurrent vacuum() calls independently
// from write_mu_, so normal put/del/apply_batch calls are not blocked while
// vacuum scans and rewrites data — only the brief commit step acquires
// write_mu_.
auto DB::vacuum(VacuumOptions opts) -> bool {
  std::lock_guard<std::mutex> vg{*vacuum_mu_};

  // Drain in-flight background hint writes so that vacuum's
  // flush_hints_for call cannot race on the same .hint.tmp file.
  worker_.drain();
  vacuum_purge_stale_files();

  // Snapshot file_stats and active-file info under write_mu_.
  std::map<std::uint32_t, FileStats> stats_snap;
  std::uint32_t active_id{};
  std::uint64_t active_size{};
  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    stats_snap = file_stats_;
    auto s = state_.load();
    active_id = s->active_file_id;
    active_size = s->active_file().size();
  }

  // Find the highest-fragmentation sealed file above threshold.
  std::uint32_t target_id{};
  double worst_frag = 0.0;
  for (const auto &[fid, fs] : stats_snap) {
    if (fid == active_id) continue;
    if (fs.total_bytes == 0) continue;
    const auto frag = 1.0 - static_cast<double>(fs.live_bytes) /
                                static_cast<double>(fs.total_bytes);
    if (frag > worst_frag && frag > opts.fragmentation_threshold) {
      worst_frag = frag;
      target_id = fid;
    }
  }

  if (target_id == 0 && worst_frag == 0.0) return false;

  // Absorb only if the file is small (below absorb_threshold) and its live
  // data fits in the active file without triggering rotation.
  const auto target_live = stats_snap.at(target_id).live_bytes;
  if (target_live <= opts.absorb_threshold &&
      target_live + active_size <= rotation_threshold_) {
    vacuum_absorb_file(target_id);
  } else {
    vacuum_compact_file(target_id);
  }
  return true;
}

#pragma endregion

#pragma region Hint internals

// Writes the hint file for a single data file using the temp-then-rename
// protocol. Batch-aware: entries between BulkBegin and BulkEnd are buffered
// and written only when BulkEnd is seen; an incomplete batch (crash
// mid-write) is silently discarded. Idempotent: skips files whose .hint
// already exists.
void DB::flush_hints_for(const std::shared_ptr<DataFile> &file,
                               const std::filesystem::path &dir) {
  const auto stem = file->path().stem().string();
  const auto hint_path = dir / (stem + ".hint");
  const auto tmp_path = dir / (stem + ".hint.tmp");

  if (std::filesystem::exists(hint_path)) {
    return;
  }

  struct PendingHint {
    std::uint64_t seq;
    EntryType type;
    std::uint64_t file_off;
    std::uint32_t val_size;
    std::vector<std::byte> key;
  };

  auto hint = HintFile::OpenForWrite(tmp_path);
  bool in_batch = false;
  std::vector<PendingHint> pending;  // staging buffer for current batch
  std::vector<PendingHint> all_hints; // all confirmed entries across the file
  Offset off = 0;

  while (auto result = file->scan(off)) {
    const auto entry_off = off;
    const auto &[entry, next] = *result;
    switch (entry.entry_type) {
    case EntryType::BulkBegin:
      in_batch = true;
      pending.clear();
      break;
    case EntryType::BulkEnd:
      for (auto &pe : pending) {
        all_hints.push_back(std::move(pe));
      }
      pending.clear();
      in_batch = false;
      break;
    case EntryType::Put:
    case EntryType::Delete:
      if (in_batch) {
        pending.push_back({entry.sequence, entry.entry_type, entry_off,
                           narrow<std::uint32_t>(entry.value.size()),
                           entry.key});
      } else {
        all_hints.push_back({entry.sequence, entry.entry_type, entry_off,
                             narrow<std::uint32_t>(entry.value.size()),
                             entry.key});
      }
      break;
    }
    off = next;
  }

  if (in_batch) {
    std::cerr << "bytecask: discarding incomplete batch in " << file->path()
              << " while generating hint file\n";
  }

  // Sort by key asc; within equal keys, seq desc so first entry = authoritative.
  std::ranges::sort(all_hints, [](const auto &a, const auto &b) {
    return a.key < b.key || (a.key == b.key && a.seq > b.seq);
  });
  // Erase all but the first (highest-seq) entry per key.
  auto tail = std::ranges::unique(all_hints, [](const auto &a, const auto &b) {
    return a.key == b.key;
  }).begin();
  all_hints.erase(tail, all_hints.end());

  for (const auto &pe : all_hints) {
    hint.append(pe.seq, pe.type, pe.file_off, pe.key, pe.val_size);
  }
  hint.sync();
  std::filesystem::rename(tmp_path, hint_path);
}

// Writes hint files for all sealed data files in the given state.
void DB::flush_hints(const EngineState &s) {
  for (auto &[file_id, file] : *s.files) {
    if (file_id == s.active_file_id) {
      continue;
    }
    flush_hints_for(file, dir_);
  }
}

// Drains background hint tasks then writes hint files for all sealed files.
void DB::flush_hints() {
  worker_.drain();
  flush_hints(*state_.load());
}

#pragma endregion

#pragma region Vacuum internals

// Batch-aware scan of source_file: copies live Puts (still current in
// snap->key_dir for source_file_id) and all tombstones into dest_file.
// Entries inside BulkBegin..BulkEnd are buffered and emitted only on
// BulkEnd; incomplete batches at EOF are silently discarded.
auto DB::vacuum_scan_and_copy(
    const std::shared_ptr<const EngineState> &snap,
    const DataFile &source_file, DataFile &dest_file,
    std::uint32_t source_file_id) -> VacuumScanResult {
  VacuumScanResult result;

  struct PendingEntry {
    DataEntry entry;
    Offset original_offset;
  };
  bool in_batch = false;
  std::vector<PendingEntry> pending;

  auto emit_entry = [&](const DataEntry &entry, Offset entry_off) {
    switch (entry.entry_type) {
    case EntryType::Put: {
      const auto existing = snap->key_dir.get(entry.key);
      if (existing && existing->file_id == source_file_id &&
          existing->file_offset == entry_off &&
          existing->sequence == entry.sequence) {
        const auto new_off =
            dest_file.append(entry.sequence, EntryType::Put, entry.key,
                             entry.value);
        const auto val_size = narrow<std::uint32_t>(entry.value.size());
        const auto sz = entry_size(entry.key.size(), entry.value.size());
        result.live_bytes += sz;
        result.total_bytes += sz;
        result.mappings.push_back({std::vector<std::byte>{entry.key.begin(),
                                                          entry.key.end()},
                                   new_off, entry.sequence, val_size});
      }
      break;
    }
    case EntryType::Delete: {
      std::ignore =
          dest_file.append(entry.sequence, EntryType::Delete, entry.key, {});
      result.total_bytes += entry_size(entry.key.size(), 0);
      break;
    }
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      break;
    }
  };

  Offset off = 0;
  while (auto scan_result = source_file.scan(off)) {
    const auto entry_off = off;
    const auto &[entry, next] = *scan_result;

    switch (entry.entry_type) {
    case EntryType::BulkBegin:
      in_batch = true;
      pending.clear();
      break;
    case EntryType::BulkEnd:
      for (auto &pe : pending) {
        emit_entry(pe.entry, pe.original_offset);
      }
      pending.clear();
      in_batch = false;
      break;
    case EntryType::Put:
    case EntryType::Delete:
      if (in_batch) {
        pending.push_back({entry, entry_off});
      } else {
        emit_entry(entry, entry_off);
      }
      break;
    }

    off = next;
  }

  return result;
}

// Purge stale files whose DataFile is only held by stale_files_ (no
// in-flight readers). Called at the start of vacuum() under vacuum_mu_.
void DB::vacuum_purge_stale_files() {
  std::erase_if(stale_files_, [](StaleFile &sf) {
    if (sf.data_file.use_count() == 1) {
      auto path = sf.data_file->path();
      sf.data_file.reset();
      std::filesystem::remove(path);
      std::filesystem::remove(sf.hint_path);
      return true;
    }
    return false;
  });
}

// Remaps key_dir entries from old_file_id to the destination file,
// updates the files map and file_stats_, and publishes the new
// EngineState. Caller must hold write_mu_.
// If new_sealed_file is non-null (compact), a fresh file-id is
// allocated and the new file is registered. Otherwise (absorb),
// the active file's stats are incremented.
void DB::vacuum_commit(std::uint32_t old_file_id,
                             const VacuumScanResult &scan,
                             std::shared_ptr<DataFile> new_sealed_file) {
  auto s = *state_.load();

  const auto dest_file_id =
      new_sealed_file ? s.next_file_id++ : s.active_file_id;

  auto t = s.key_dir.transient();
  auto actual_live_bytes = scan.live_bytes;
  for (const auto &m : scan.mappings) {
    const std::span<const std::byte> key_span{m.key};
    const auto cur = t.get(key_span);
    if (cur && cur->sequence == m.sequence) {
      t.set(key_span,
            KeyDirEntry{m.sequence, dest_file_id, m.new_offset, m.value_size});
    } else {
      actual_live_bytes -= entry_size(m.key.size(), m.value_size);
    }
  }
  s.key_dir = std::move(t).persistent();

  auto next_files =
      std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
          *s.files);
  next_files->erase(old_file_id);
  if (new_sealed_file) {
    next_files->emplace(dest_file_id, std::move(new_sealed_file));
  }
  s.files = std::move(next_files);

  file_stats_.erase(old_file_id);
  if (dest_file_id != s.active_file_id) {
    file_stats_[dest_file_id] = FileStats{actual_live_bytes, scan.total_bytes};
  } else {
    auto &active_stats = file_stats_[dest_file_id];
    active_stats.live_bytes += actual_live_bytes;
    active_stats.total_bytes += scan.total_bytes;
  }

  state_.store(std::make_shared<EngineState>(std::move(s)));
  state_time_.store(now_ns(), std::memory_order_release);
}

// Stashes the old data file and its hint path for deferred removal
// once no in-flight readers reference it.
void DB::vacuum_defer_old_file(
    const std::shared_ptr<const EngineState> &snap, std::uint32_t file_id) {
  auto old_data_file = snap->files->at(file_id);
  auto old_hint_path =
      dir_ / (old_data_file->path().stem().string() + ".hint");
  stale_files_.push_back({std::move(old_data_file), std::move(old_hint_path)});
}

// Rewrites a sealed file into a new sealed file containing only live
// entries and tombstones. Called under vacuum_mu_, not write_mu_.
// The new data file is written to .data.tmp, then renamed atomically.
// The old file is deferred for cleanup when no readers reference it.
void DB::vacuum_compact_file(std::uint32_t file_id) {
  auto snap = state_.load();
  const auto &old_file = *snap->files->at(file_id);

  const auto stem = make_data_file_stem();
  const auto tmp_data_path = dir_ / (stem + ".data.tmp");
  const auto final_data_path = dir_ / (stem + ".data");

  VacuumScanResult scan;
  {
    DataFile tmp_file(tmp_data_path);
    scan = vacuum_scan_and_copy(snap, old_file, tmp_file, file_id);
    tmp_file.sync();
  }

  std::filesystem::rename(tmp_data_path, final_data_path);
  auto new_file = std::make_shared<DataFile>(final_data_path);
  new_file->seal();
  flush_hints_for(new_file, dir_);

  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    vacuum_commit(file_id, scan, new_file);
  }
  vacuum_defer_old_file(snap, file_id);
}

// Appends live entries from a sealed file to the active file, then
// removes the sealed file. Called under vacuum_mu_.
// The entire I/O + commit phase runs under write_mu_ because
// scan_and_copy appends to the shared active DataFile, which is
// NOT thread-safe (requires external synchronization).
// The old file is deferred for cleanup when no readers reference it.
void DB::vacuum_absorb_file(std::uint32_t file_id) {
  auto snap = state_.load();
  const auto &old_file = *snap->files->at(file_id);

  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    auto &active = snap->active_file();
    auto scan = vacuum_scan_and_copy(snap, old_file, active, file_id);
    active.sync();
    vacuum_commit(file_id, scan, nullptr);
  }
  vacuum_defer_old_file(snap, file_id);
}

#pragma endregion

#pragma region FileStats helpers

// Marks an existing entry as dead in its file's stats.
void DB::stats_retire_entry(BytesView key, const KeyDirEntry &old) {
  file_stats_[old.file_id].live_bytes -= entry_size(key.size(), old.value_size);
}

// Records a new Put entry: live + total on the active file.
void DB::stats_publish_put(std::uint32_t active_file_id, BytesView key,
                                 BytesView value) {
  stats_publish_put(active_file_id, key,
                    narrow<std::uint32_t>(value.size()));
}

void DB::stats_publish_put(std::uint32_t active_file_id, BytesView key,
                                 std::uint32_t value_size) {
  const auto sz = entry_size(key.size(), value_size);
  auto &st = file_stats_[active_file_id];
  st.live_bytes += sz;
  st.total_bytes += sz;
}

// Records a tombstone (Delete): total only on the active file.
void DB::stats_publish_tombstone(std::uint32_t active_file_id,
                                       BytesView key) {
  file_stats_[active_file_id].total_bytes += entry_size(key.size(), 0);
}

// Records a bulk marker (BulkBegin / BulkEnd): total only.
void DB::stats_publish_bulk_marker(std::uint32_t active_file_id) {
  file_stats_[active_file_id].total_bytes += kHeaderSize + kCrcSize;
}

#pragma endregion

#pragma region WriteGroup batch executor

// The Template Method hook called by the WriteGroup leader.
// Acquires write_mu_, runs all queued lambdas on a shared TransientRadixTree,
// calls persistent(), rotate_if_needed, fdatasync if any_sync, then publishes
// a single EngineState. On exception from lambda K, lambdas 0..K-1 are
// committed and lambdas K+1..N-1 receive WriteGroupAborted.
void DB::execute_write_batch(std::vector<WriteGroup::Slot *> &batch) {
  std::lock_guard<std::mutex> wg{*write_mu_};
  auto s = *state_.load();
  auto t = s.key_dir.transient();
  auto any_sync = false;
  std::size_t completed = 0;

  for (auto *slot : batch) {
    auto *es = static_cast<EngineSlot *>(slot);
    try {
      es->fn(s, t);
      any_sync |= es->sync;
      ++completed;
    } catch (...) {
      es->err = std::current_exception();
      break;
    }
  }

  s.key_dir = std::move(t).persistent();
  s = rotate_if_needed(std::move(s));
  if (any_sync) {
    s.active_file().sync();
  }
  state_.store(std::make_shared<EngineState>(std::move(s)));
  state_time_.store(now_ns(), std::memory_order_release);

  // Mark unexecuted slots as aborted.
  for (auto i = completed + 1; i < batch.size(); ++i) {
    batch[i]->err = std::make_exception_ptr(WriteGroupAborted{});
  }
}

#pragma endregion

#pragma region State access

// Returns the engine state from a thread-local cache.
// The hot path is a single relaxed load of state_time_ (plain MOV on x86).
// The snapshot is refreshed only when the last write timestamp exceeds
// staleness_tolerance (session mode: tolerance=0, refreshes on every write).
// Returns a reference to the thread-local snapshot. The snapshot stays
// alive until the same thread calls load_state again, so callers must
// not stash the reference across a second load_state call.
auto DB::load_state(const ReadOptions &opts) const
    -> const std::shared_ptr<const EngineState> & {
  struct TlState {
    std::shared_ptr<const EngineState> snapshot;
    std::int64_t last_write_time{0};
  };
  // Per-thread state cache — thread-exit destructor is intentional.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  thread_local TlState tl;
#pragma clang diagnostic pop
  const auto wt = state_time_.load(std::memory_order_relaxed);
  const auto tolerance =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          opts.staleness_tolerance)
          .count();
  if (wt - tl.last_write_time > tolerance) {
    tl.snapshot = state_.load();
    tl.last_write_time = wt;
  }
  return tl.snapshot;
}

// Acquires the write mutex. Blocking or try-lock based on opts.try_lock.
auto DB::acquire_write_lock(const WriteOptions &opts)
    -> std::unique_lock<std::mutex> {
  if (opts.try_lock) {
    std::unique_lock<std::mutex> lk{*write_mu_, std::try_to_lock};
    if (!lk.owns_lock()) {
      throw std::system_error{
          std::make_error_code(std::errc::resource_unavailable_try_again),
          "bytecask: write lock unavailable"};
    }
    return lk;
  }
  return std::unique_lock<std::mutex>{*write_mu_};
}

#pragma endregion

#pragma region File rotation

// Seals the active file, opens a new one, and dispatches hint file writing
// for the now-sealed file to the background worker. Returns a new EngineState.
// fdatasync on the sealed file remains synchronous for durability correctness.
auto DB::rotate_active_file(EngineState s) -> EngineState {
  s.active_file().sync();
  s.active_file().seal();
  auto sealed = s.files->at(s.active_file_id);
  auto dir = dir_;
  worker_.dispatch([f = std::move(sealed), d = std::move(dir)] {
    flush_hints_for(f, d);
  });
  s = s.apply_rotation(dir_);
  file_stats_[s.active_file_id] = FileStats{};
  return s;
}

auto DB::rotate_if_needed(EngineState s) -> EngineState {
  if (s.active_file().size() >= rotation_threshold_) {
    return rotate_active_file(std::move(s));
  }
  return s;
}

#pragma endregion

#pragma region Recovery

// Phase 1 shared by serial and parallel recovery: remove stale .hint.tmp
// files, open all data files, seal them, register in s.files, and
// generate missing hint files. Returns the RecoveredFile list.
auto DB::recovery_prepare_files(EngineState &s)
    -> std::vector<RecoveredFile> {
  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    const auto &p = dir_entry.path();
    if (p.extension() == ".tmp" &&
        (p.stem().extension() == ".hint" || p.stem().extension() == ".data")) {
      std::filesystem::remove(p);
    }
  }

  std::vector<RecoveredFile> files;

  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    const auto &p = dir_entry.path();
    if (p.extension() != ".data") {
      continue;
    }

    const auto file_id = s.next_file_id++;
    auto data_file = std::make_shared<DataFile>(p);
    data_file->seal();
    s.files->emplace(file_id, data_file);

    const auto hint_path = dir_ / (p.stem().string() + ".hint");
    if (!std::filesystem::exists(hint_path)) {
      flush_hints_for(data_file, dir_);
    }

    files.push_back({file_id, std::move(data_file), hint_path,
                     std::filesystem::file_size(p)});
  }

  return files;
}

// Builds a RecoveryResult from a subset of hint files.
// Each worker calls this independently — no shared mutable state.
auto DB::recovery_build_from_hints(std::span<RecoveredFile> files)
    -> RecoveryResult {
  std::uint64_t max_lsn = 0;
  auto t = PersistentRadixTree<KeyDirEntry>{}.transient();
  std::map<Key, std::uint64_t> tombstones;
  std::map<std::uint32_t, FileStats> fstats;

  for (const auto &rf : files) {
    fstats[rf.file_id].total_bytes = rf.total_bytes;
  }

  // live_bytes are NOT tracked per-entry here — Phase 4 in
  // recovery_load_parallel recomputes them in a single pass after the
  // final merge, avoiding redundant O(N) map lookups per worker.
  auto lsn_wins = [](const KeyDirEntry &existing, const KeyDirEntry &incoming) {
    return existing.sequence < incoming.sequence;
  };

  for (auto &[file_id, data_file, hint_path, tb] : files) {
    auto hint = HintFile::OpenForRead(hint_path);
    auto scanner = hint.make_scanner();
    while (auto he = scanner.next()) {
      if (he->entry_type == EntryType::Put) {
        const auto k = Key{he->key};
        const auto tomb_it = tombstones.find(k);
        if (tomb_it != tombstones.end() && tomb_it->second >= he->sequence) {
          if (he->sequence > max_lsn) max_lsn = he->sequence;
          continue;
        }
        t.upsert(he->key,
                 KeyDirEntry{he->sequence, file_id, he->file_offset,
                             he->value_size},
                 lsn_wins);
      } else if (he->entry_type == EntryType::Delete) {
        const auto k = Key{he->key};
        auto &tomb_seq = tombstones[k];
        if (he->sequence > tomb_seq) tomb_seq = he->sequence;
        const auto existing = t.get(he->key);
        if (existing && existing->sequence < he->sequence) {
          t.erase(he->key);
        }
      }
      if (he->sequence > max_lsn) max_lsn = he->sequence;
    }
  }

  return {std::move(t).persistent(), std::move(tombstones), max_lsn,
          std::move(fstats)};
}

// Merges two RecoveryResults. Tree merge uses LSN-based conflict
// resolution, then tombstones from both sides are cross-applied to
// suppress stale PUTs. Tombstone maps and file_stats are unioned.
// live_bytes are NOT recomputed here — deferred to a single pass
// after the final merge to avoid O(N × log₂ W) redundant traversals.
auto DB::recovery_merge_results(RecoveryResult a, RecoveryResult b)
-> RecoveryResult {
  auto &merged_stats = a.file_stats;
  for (auto &[fid, fs] : b.file_stats) {
    merged_stats[fid] = fs;
  }

  auto lsn_resolver = [](const KeyDirEntry &x, const KeyDirEntry &y) {
    return (x.sequence >= y.sequence) ? x : y;
  };

  auto merged =
      PersistentRadixTree<KeyDirEntry>::merge(a.key_dir, b.key_dir, lsn_resolver);

  for (const auto &[key, tomb_seq] : b.tombstones) {
    std::span<const std::byte> key_span{key.begin(), key.size()};
    const auto entry = merged.get(key_span);
    if (entry && entry->sequence < tomb_seq) {
      merged = merged.erase(key_span);
    }
  }

  for (const auto &[key, tomb_seq] : a.tombstones) {
    std::span<const std::byte> key_span{key.begin(), key.size()};
    const auto entry = merged.get(key_span);
    if (entry && entry->sequence < tomb_seq) {
      merged = merged.erase(key_span);
    }
  }

  auto &merged_tombs = a.tombstones;
  for (auto &[key, seq] : b.tombstones) {
    auto &existing = merged_tombs[key];
    if (seq > existing) existing = seq;
  }

  return {std::move(merged), std::move(merged_tombs),
          std::max(a.max_lsn, b.max_lsn), std::move(merged_stats)};
}

// Reconstructs the key directory from hint files (serial path).
// Pre-generates missing hint files from raw data scans (batch-aware),
// then recovers exclusively from hints — single code path.
// Returns a new EngineState with key_dir populated and next_lsn set to
// max_seen + 1. next_file_id is advanced for each recovered file.
auto DB::recovery_load_serial(EngineState s) -> EngineState {
  auto files = recovery_prepare_files(s);

  for (const auto &rf : files) {
    file_stats_[rf.file_id].total_bytes = rf.total_bytes;
  }

  std::uint64_t max_lsn = 0;
  auto transient_key_dir = s.key_dir.transient();
  std::map<Key, std::uint64_t> tombstones;

  for (auto &[file_id, data_file, hint_path, tb] : files) {
    auto hint = HintFile::OpenForRead(hint_path);
    auto scanner = hint.make_scanner();
    while (auto he = scanner.next()) {
      if (he->entry_type == EntryType::Put) {
        const auto k = Key{he->key};
        const auto tomb_it = tombstones.find(k);
        if (tomb_it != tombstones.end() && tomb_it->second >= he->sequence) {
          if (he->sequence > max_lsn) max_lsn = he->sequence;
          continue;
        }
        const auto existing = transient_key_dir.get(he->key);
        if (!existing || existing->sequence < he->sequence) {
          if (existing) {
            file_stats_[existing->file_id].live_bytes -=
                entry_size(he->key.size(), existing->value_size);
          }
          file_stats_[file_id].live_bytes +=
              entry_size(he->key.size(), he->value_size);
          transient_key_dir.set(he->key,
                                KeyDirEntry{he->sequence, file_id,
                                            he->file_offset, he->value_size});
        }
      } else if (he->entry_type == EntryType::Delete) {
        const auto k = Key{he->key};
        auto &tomb_seq = tombstones[k];
        if (he->sequence > tomb_seq) tomb_seq = he->sequence;
        const auto existing = transient_key_dir.get(he->key);
        if (existing && existing->sequence < he->sequence) {
          file_stats_[existing->file_id].live_bytes -=
              entry_size(he->key.size(), existing->value_size);
          transient_key_dir.erase(he->key);
        }
      }
      if (he->sequence > max_lsn) max_lsn = he->sequence;
    }
  }

  s.key_dir = std::move(transient_key_dir).persistent();
  s.next_lsn = max_lsn + 1;
  return s;
}

// Parallel recovery: file-level partitioning with sequential accumulator merge.
// Round-robin assigns files to W workers, each builds a RecoveryResult,
// then results are merged one-at-a-time into an accumulator as workers finish.
auto DB::recovery_load_parallel(EngineState s,
                                      unsigned recovery_threads) -> EngineState {
  auto files = recovery_prepare_files(s);

  if (files.empty()) {
    return s;
  }

  auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
  if (W == 0) W = 1;

  // Phase 1: round-robin file assignment.
  std::vector<std::vector<RecoveredFile>> worker_files(W);
  for (unsigned i = 0; i < files.size(); ++i) {
    worker_files[i % W].push_back(std::move(files[i]));
  }

  // Phase 2: parallel build + Phase 3: sequential accumulator merge.
  // Workers push finished results into a queue; the main thread merges
  // each into an accumulator as it arrives. Each ~N/W-key tree is merged
  // once; disjoint subtrees are shared O(1) by the persistent tree, so
  // total merge work is proportional to overlap, not N × log₂(W).
  std::mutex queue_mu;
  std::condition_variable queue_cv;
  std::vector<RecoveryResult> queue;
  unsigned finished_count = 0;

  {
    std::vector<std::jthread> threads;
    threads.reserve(W);
    for (unsigned i = 0; i < W; ++i) {
      threads.emplace_back([&, i] {
        auto result = recovery_build_from_hints(worker_files[i]);
        std::unique_lock<std::mutex> lk{queue_mu};
        queue.push_back(std::move(result));
        ++finished_count;
        queue_cv.notify_one();
      });
    }

    // Main thread: consume results as they arrive.
    RecoveryResult acc{};
    bool acc_initialized = false;
    unsigned merged_count = 0;

    while (merged_count < W) {
      std::unique_lock<std::mutex> lk{queue_mu};
      queue_cv.wait(lk, [&] { return !queue.empty(); });
      auto incoming = std::move(queue.back());
      queue.pop_back();
      lk.unlock();

      if (!acc_initialized) {
        acc = std::move(incoming);
        acc_initialized = true;
      } else {
        acc = recovery_merge_results(std::move(acc), std::move(incoming));
      }
      ++merged_count;
    }

    // Store final result for phases 4-5 (threads join at scope exit).
    queue.clear();
    queue.push_back(std::move(acc));
  }

  auto &final_result = queue[0];

  // Phase 4: recompute live_bytes once from the fully-merged tree.
  auto &final_stats = final_result.file_stats;
  for (auto &[fid, fs] : final_stats) {
    fs.live_bytes = 0;
  }
  for (auto it = final_result.key_dir.begin(); it != std::default_sentinel;
       ++it) {
    const auto &[key_span, kde] = *it;
    final_stats[kde.file_id].live_bytes +=
        entry_size(key_span.size(), kde.value_size);
  }

  // Phase 5: assembly.
  s.key_dir = std::move(final_result.key_dir);
  s.next_lsn = final_result.max_lsn + 1;
  file_stats_ = std::move(final_stats);
  return s;
}

#pragma endregion

} // namespace bytecask
