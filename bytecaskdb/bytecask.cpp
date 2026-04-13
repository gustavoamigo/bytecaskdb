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

#pragma endregion

#pragma region TransientEngineState

TransientEngineState::TransientEngineState(
    TransientRadixTree<KeyDirEntry> key_dir, FileRegistry files,
    std::map<std::uint32_t, FileStats> file_stats,
    std::uint32_t active_file_id, std::uint32_t next_file_id,
    std::uint64_t next_lsn)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_file_id_{next_file_id}, next_lsn_{next_lsn} {}

auto EngineState::transient() const -> TransientEngineState {
  return TransientEngineState{
      key_dir.transient(), files, file_stats,
      active_file_id, next_file_id, next_lsn};
}

auto TransientEngineState::validate_preconditions(const WritePlan &plan) const
    -> bool {
  const auto *snap_state =
      plan.snap_ ? plan.snap_->state_.get() : nullptr;

  // 1. Point guards.
  for (const auto &[key, action] : plan.actions_) {
    const std::span<const std::byte> key_span{key};
    const auto cur_entry = key_dir_.get(key_span);

    switch (action.precondition) {
    case WritePlan::Precondition::MustExist:
      if (!cur_entry) return false;
      break;
    case WritePlan::Precondition::MustBeAbsent:
      if (cur_entry) return false;
      break;
    case WritePlan::Precondition::MustBeUnchanged: {
      // ensure_unchanged already enforced snap_ is present at build time.
      const auto snap_entry = snap_state->key_dir.get(key_span);
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence : 0;
      const std::uint64_t cur_seq = cur_entry ? cur_entry->sequence : 0;
      if (cur_seq != snap_seq) return false;
      break;
    }
    case WritePlan::Precondition::None:
      break;
    }
  }

  // 2. Range guards (only present when snap_ is set — enforced at build time).
  for (const auto &rg : plan.range_guards_) {
    const std::span<const std::byte> from_span{rg.from};
    const std::span<const std::byte> to_span{rg.to};

    // Check current state for keys modified since snapshot.
    for (auto it = key_dir_.lower_bound(from_span);
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (Key{key_span} >= Key{to_span}) break;
      const auto snap_entry = snap_state->key_dir.get(key_span);
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence : 0;
      if (entry.sequence != snap_seq) return false;
    }

    // Check snapshot for keys deleted since snapshot.
    for (auto it = snap_state->key_dir.lower_bound(from_span);
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (Key{key_span} >= Key{to_span}) break;
      if (!key_dir_.get(key_span)) return false;
    }
  }

  // 3. Implicit W-W check on all write keys (only when snapshot present).
  if (snap_state) {
    for (const auto &[key, action] : plan.actions_) {
      if (action.write == WritePlan::Write::None) continue;
      const std::span<const std::byte> key_span{key};
      const auto snap_entry = snap_state->key_dir.get(key_span);
      const auto cur_entry = key_dir_.get(key_span);
      const bool appeared = !snap_entry && cur_entry;
      const bool deleted = snap_entry && !cur_entry;
      const bool modified =
          snap_entry && cur_entry &&
          cur_entry->sequence != snap_entry->sequence;
      if (appeared || deleted || modified) return false;
    }
  }

  return true;
}

auto TransientEngineState::prepare_write(const WritePlan &plan) const
    -> std::vector<AppendEntry> {
  std::vector<AppendEntry> entries;
  const auto wc = plan.write_count();
  if (wc == 0) return entries;

  const bool multi = wc > 1;
  entries.reserve(multi ? wc + 2 : 1);

  auto lsn = next_lsn_;

  if (multi) {
    entries.push_back({lsn++, EntryType::BulkBegin, {}, {}});
  }

  for (const auto &[key, action] : plan.actions_) {
    const std::span<const std::byte> key_span{key};
    if (action.write == WritePlan::Write::Put) {
      entries.push_back(
          {lsn++, EntryType::Put, key_span,
           std::span<const std::byte>{action.value}});
    } else if (action.write == WritePlan::Write::Del) {
      entries.push_back({lsn++, EntryType::Delete, key_span, {}});
    }
  }

  if (multi) {
    entries.push_back({lsn++, EntryType::BulkEnd, {}, {}});
  }

  return entries;
}

void TransientEngineState::apply_writes(
    const WritePlan &plan, const std::vector<std::uint64_t> &offsets) {
  std::size_t io_idx = 0;
  const auto wc = plan.write_count();
  const bool multi = wc > 1;

  // Account for BulkBegin marker.
  if (multi) {
    file_stats_[active_file_id_].total_bytes += kHeaderSize + kCrcSize;
    ++next_lsn_;
    ++io_idx;
  }

  for (const auto &[key, action] : plan.actions_) {
    const std::span<const std::byte> key_span{key};
    if (action.write == WritePlan::Write::Put) {
      const auto existing = key_dir_.get(key_span);
      if (existing) {
        file_stats_[existing->file_id].live_bytes -=
            entry_size(key_span.size(), existing->value_size);
      }
      const auto val_size = narrow<std::uint32_t>(action.value.size());
      const auto sz = entry_size(key_span.size(), val_size);
      auto &st = file_stats_[active_file_id_];
      st.live_bytes += sz;
      st.total_bytes += sz;
      key_dir_.set(key_span, KeyDirEntry{next_lsn_, active_file_id_,
                                          offsets[io_idx], val_size});
      ++next_lsn_;
      ++io_idx;
    } else if (action.write == WritePlan::Write::Del) {
      const auto existing = key_dir_.get(key_span);
      if (existing) {
        file_stats_[existing->file_id].live_bytes -=
            entry_size(key_span.size(), existing->value_size);
      }
      file_stats_[active_file_id_].total_bytes +=
          entry_size(key_span.size(), 0);
      key_dir_.erase(key_span);
      ++next_lsn_;
      ++io_idx;
    }
  }

  // Account for BulkEnd marker.
  if (multi) {
    file_stats_[active_file_id_].total_bytes += kHeaderSize + kCrcSize;
    ++next_lsn_;
    ++io_idx;
  }
}

void TransientEngineState::apply_rotate_file(
    std::shared_ptr<DataFile> new_file) {
  active_file_id_ = next_file_id_++;
  auto next_files =
      std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
          *files_);
  next_files->emplace(active_file_id_, std::move(new_file));
  files_ = std::move(next_files);
  file_stats_[active_file_id_] = FileStats{};
}

void TransientEngineState::apply_vacuum(
    std::uint32_t old_file_id, const VacuumScanResult &scan,
    std::shared_ptr<DataFile> new_sealed_file) {
  const auto dest_file_id =
      new_sealed_file ? next_file_id_++ : active_file_id_;

  auto actual_live_bytes = scan.live_bytes;
  for (const auto &m : scan.mappings) {
    const std::span<const std::byte> key_span{m.key};
    const auto cur = key_dir_.get(key_span);
    if (cur && cur->sequence == m.sequence) {
      key_dir_.set(key_span,
                   KeyDirEntry{m.sequence, dest_file_id, m.new_offset,
                               m.value_size});
    } else {
      actual_live_bytes -= entry_size(m.key.size(), m.value_size);
    }
  }

  auto next_files =
      std::make_shared<std::map<std::uint32_t, std::shared_ptr<DataFile>>>(
          *files_);
  next_files->erase(old_file_id);
  if (new_sealed_file) {
    next_files->emplace(dest_file_id, std::move(new_sealed_file));
  }
  files_ = std::move(next_files);

  file_stats_.erase(old_file_id);
  if (dest_file_id != active_file_id_) {
    file_stats_[dest_file_id] =
        FileStats{actual_live_bytes, scan.total_bytes};
  } else {
    auto &active_stats = file_stats_[dest_file_id];
    active_stats.live_bytes += actual_live_bytes;
    active_stats.total_bytes += scan.total_bytes;
  }
}

auto TransientEngineState::active_file() -> DataFile & {
  return *files_->at(active_file_id_);
}

auto TransientEngineState::active_file_id() const noexcept -> std::uint32_t {
  return active_file_id_;
}

auto TransientEngineState::is_rotation_needed(std::uint64_t threshold) const
    -> bool {
  return files_->at(active_file_id_)->size() >= threshold;
}

auto TransientEngineState::persistent() && -> std::shared_ptr<EngineState> {
  auto s = std::make_shared<EngineState>();
  s->key_dir = std::move(key_dir_).persistent();
  s->files = std::move(files_);
  s->file_stats = std::move(file_stats_);
  s->active_file_id = active_file_id_;
  s->next_file_id = next_file_id_;
  s->next_lsn = next_lsn_;
  return s;
}

#pragma endregion

#pragma region FileStats helpers

// Marks an existing entry as dead in its file's stats.
void stats_retire_entry(std::map<std::uint32_t, FileStats> &fs,
                        BytesView key, const KeyDirEntry &old) {
  fs[old.file_id].live_bytes -= entry_size(key.size(), old.value_size);
}

// Records a new Put entry: live + total on the active file.
void stats_publish_put(std::map<std::uint32_t, FileStats> &fs,
                       std::uint32_t active_file_id, BytesView key,
                       BytesView value) {
  const auto sz = entry_size(key.size(), value.size());
  auto &st = fs[active_file_id];
  st.live_bytes += sz;
  st.total_bytes += sz;
}

// Records a tombstone (Delete): total only on the active file.
void stats_publish_tombstone(std::map<std::uint32_t, FileStats> &fs,
                             std::uint32_t active_file_id, BytesView key) {
  fs[active_file_id].total_bytes += entry_size(key.size(), 0);
}

// Records a bulk marker (BulkBegin / BulkEnd): total only.
void stats_publish_bulk_marker(std::map<std::uint32_t, FileStats> &fs,
                               std::uint32_t active_file_id) {
  fs[active_file_id].total_bytes += kHeaderSize + kCrcSize;
}

#pragma endregion

#pragma region Construction

// Opens dir, runs recovery, creates initial active data file.
// Throws std::system_error if the directory cannot be prepared.
DB::DB(std::filesystem::path dir, Options opts)
    : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
      state_{std::make_shared<EngineState>()} {
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
  s.file_stats[s.active_file_id] = FileStats{};
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
  WritePlan plan;
  plan.put(key, value);
  (void)apply_batch_if(opts, std::move(plan));
}

auto DB::del(const WriteOptions &opts, BytesView key) -> bool {
  WritePlan plan;
  plan.ensure_present(key);
  plan.del(key);
  return apply_batch_if(opts, std::move(plan));
}

auto DB::contains_key(BytesView key) const -> bool {
  auto s = state_.load();
  return s->key_dir.contains(key);
}

void DB::apply_batch(const WriteOptions &opts, Batch batch) {
  if (batch.empty()) return;
  WritePlan plan;
  for (auto &op : batch.operations_) {
    std::visit(
        [&](auto &o) {
          using T = std::decay_t<decltype(o)>;
          if constexpr (std::is_same_v<T, BatchInsert>) {
            plan.put(std::span<const std::byte>{o.key},
                     std::span<const std::byte>{o.value});
          } else if constexpr (std::is_same_v<T, BatchRemove>) {
            plan.del(std::span<const std::byte>{o.key});
          }
        },
        op);
  }
  (void)apply_batch_if(opts, std::move(plan));
}

#pragma endregion

#pragma region Snapshot and apply_batch_if

auto DB::snapshot() const -> Snapshot { return Snapshot{state_.load()}; }

// The single write path. Validates preconditions, runs IO, applies state
// transitions via TransientEngineState, handles rotation and sync.
// put/del/apply_batch are thin wrappers that construct a WritePlan and
// delegate here.
auto DB::apply_batch_if(WriteOptions opts,
                        WritePlan plan) -> bool {
  if (plan.empty()) return true;

  auto guard = acquire_write_lock(opts);
  auto current = state_.load();

  // 1. Create transient working copy.
  auto t = current->transient();

  // 2. Validate preconditions (TransientEngineState decides).
  if (!t.validate_preconditions(plan)) return false;

  // 3. Prepare IO plan (TransientEngineState assigns LSNs, inserts markers).
  auto entries = t.prepare_write(plan);
  if (entries.empty()) {
    // Guards-only plan with no writes.
    return true;
  }

  // 4. IO phase — coordinator's job (can throw).
  auto &file = t.active_file();
  std::vector<std::uint64_t> offsets;
  offsets.reserve(entries.size());
  for (const auto &entry : entries) {
    offsets.push_back(
        file.append(entry.sequence, entry.entry_type, entry.key, entry.value));
  }

  // 5. State transition — TransientEngineState's job (cannot fail).
  t.apply_writes(plan, offsets);

  // 6. Rotation — IO is coordinator's job, state change is transient's.
  if (t.is_rotation_needed(rotation_threshold_)) {
    file.sync();
    file.seal();
    auto sealed = t.active_file_id();
    auto sealed_file = current->files->at(sealed);
    auto dir = dir_;
    worker_.dispatch([f = std::move(sealed_file), d = std::move(dir)] {
      flush_hints_for(f, d);
    });
    const auto stem = make_data_file_stem();
    auto new_file =
        std::make_shared<DataFile>(dir_ / (stem + ".data"));
    t.apply_rotate_file(std::move(new_file));
  }

  // 7. Durability before visibility.
  if (opts.sync) {
    file.sync();
  }

  // 8. Publish.
  state_.store(std::move(t).persistent());
  state_time_.store(now_ns(), std::memory_order_release);
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

  // Snapshot file_stats and active-file info.
  std::map<std::uint32_t, FileStats> stats_snap;
  std::uint32_t active_id{};
  std::uint64_t active_size{};
  {
    auto s = state_.load();
    stats_snap = s->file_stats;
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
// updates the files map and file_stats, and publishes the new
// EngineState. Caller must hold write_mu_.
// If new_sealed_file is non-null (compact), a fresh file-id is
// allocated and the new file is registered. Otherwise (absorb),
// the active file's stats are incremented.
void DB::vacuum_commit(std::uint32_t old_file_id,
                             const VacuumScanResult &scan,
                             std::shared_ptr<DataFile> new_sealed_file) {
  auto current = state_.load();
  auto t = current->transient();
  t.apply_vacuum(old_file_id, scan, std::move(new_sealed_file));

  state_.store(std::move(t).persistent());
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
    s.file_stats[rf.file_id].total_bytes = rf.total_bytes;
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
            s.file_stats[existing->file_id].live_bytes -=
                entry_size(he->key.size(), existing->value_size);
          }
          s.file_stats[file_id].live_bytes +=
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
          s.file_stats[existing->file_id].live_bytes -=
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
  s.file_stats = std::move(final_stats);
  return s;
}

#pragma endregion

} // namespace bytecask
