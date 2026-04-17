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
#ifdef BYTECASK_TESTING
#include "fault_injector.h"
#endif
#include <fcntl.h>
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
#include <sys/file.h>
#include <system_error>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
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
import bytecask.u32_map;
import bytecask.util;

namespace bytecask {

DbDegraded::~DbDegraded() = default;

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
    TransientRadixTree<KeyDirEntry> key_dir,
    TransientU32Map<std::shared_ptr<DataFile>> files,
    TransientU32Map<FileStats> file_stats,
    std::uint32_t active_file_id, std::uint32_t next_file_id,
    std::uint64_t next_lsn)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_file_id_{next_file_id}, next_lsn_{next_lsn} {}

auto EngineState::transient() const -> TransientEngineState {
  return TransientEngineState{
      key_dir.transient(), files.transient(), file_stats.transient(),
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
    const WritePlan &plan, std::span<const std::uint64_t> offsets) {
  std::size_t io_idx = 0;
  const auto wc = plan.write_count();
  const bool multi = wc > 1;

  // Account for BulkBegin marker.
  if (multi) {
    file_stats_.update(active_file_id_, [](FileStats &fs) {
      fs.total_bytes += kHeaderSize + kCrcSize;
    });
    ++next_lsn_;
    ++io_idx;
  }

  for (const auto &[key, action] : plan.actions_) {
    const std::span<const std::byte> key_span{key};
    if (action.write == WritePlan::Write::Put) {
      const auto existing = key_dir_.get(key_span);
      if (existing) {
        const auto dec = entry_size(key_span.size(), existing->value_size);
        const auto ef = existing->file_id;
        file_stats_.update(ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
      }
      const auto val_size = narrow<std::uint32_t>(action.value.size());
      const auto sz = entry_size(key_span.size(), val_size);
      file_stats_.update(active_file_id_, [sz](FileStats &fs) {
        fs.live_bytes += sz;
        fs.total_bytes += sz;
      });
      key_dir_.set(key_span, KeyDirEntry{next_lsn_, active_file_id_,
                                          offsets[io_idx], val_size});
      ++next_lsn_;
      ++io_idx;
    } else if (action.write == WritePlan::Write::Del) {
      const auto existing = key_dir_.get(key_span);
      if (existing) {
        const auto dec = entry_size(key_span.size(), existing->value_size);
        const auto ef = existing->file_id;
        file_stats_.update(ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
      }
      const auto del_sz = entry_size(key_span.size(), 0);
      file_stats_.update(active_file_id_,
                         [del_sz](FileStats &fs) { fs.total_bytes += del_sz; });
      key_dir_.erase(key_span);
      ++next_lsn_;
      ++io_idx;
    }
  }

  // Account for BulkEnd marker.
  if (multi) {
    file_stats_.update(active_file_id_, [](FileStats &fs) {
      fs.total_bytes += kHeaderSize + kCrcSize;
    });
    ++next_lsn_;
    ++io_idx;
  }
}

void TransientEngineState::apply_rotate_file(
    std::shared_ptr<DataFile> new_file) {
  active_file_id_ = next_file_id_++;
  files_.set(active_file_id_, std::move(new_file));
  file_stats_.set(active_file_id_, FileStats{});
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

  files_.erase(old_file_id);
  if (new_sealed_file) {
    files_.set(dest_file_id, std::move(new_sealed_file));
  }

  file_stats_.erase(old_file_id);
  if (dest_file_id != active_file_id_) {
    file_stats_.set(dest_file_id,
                    FileStats{actual_live_bytes, scan.total_bytes});
  } else {
    file_stats_.update(dest_file_id, [actual_live_bytes,
                                      total = scan.total_bytes](FileStats &fs) {
      fs.live_bytes += actual_live_bytes;
      fs.total_bytes += total;
    });
  }
}

void TransientEngineState::apply_resume(
    std::uint32_t file_id, const std::vector<ResumeEntry> &entries) {
  std::uint64_t max_lsn = 0;
  for (const auto &e : entries) {
    const std::span<const std::byte> key_span{e.key};
    if (e.sequence > max_lsn) max_lsn = e.sequence;

    if (e.entry_type == EntryType::Put) {
      const auto existing = key_dir_.get(key_span);
      if (!existing || existing->sequence < e.sequence) {
        if (existing) {
          const auto dec = entry_size(key_span.size(), existing->value_size);
          const auto ef = existing->file_id;
          file_stats_.update(ef,
                             [dec](FileStats &fs) { fs.live_bytes -= dec; });
        }
        const auto inc = entry_size(key_span.size(), e.value_size);
        file_stats_.update(file_id,
                           [inc](FileStats &fs) { fs.live_bytes += inc; });
        key_dir_.set(key_span,
                     KeyDirEntry{e.sequence, file_id, e.file_offset,
                                 e.value_size});
      }
    } else if (e.entry_type == EntryType::Delete) {
      const auto existing = key_dir_.get(key_span);
      if (existing && existing->sequence < e.sequence) {
        const auto dec = entry_size(key_span.size(), existing->value_size);
        const auto ef = existing->file_id;
        file_stats_.update(ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
        key_dir_.erase(key_span);
      }
    }
  }

  if (max_lsn >= next_lsn_) {
    next_lsn_ = max_lsn + 1;
  }
}

auto TransientEngineState::active_file() -> DataFile & {
  return **files_.get(active_file_id_);
}

auto TransientEngineState::active_file_id() const noexcept -> std::uint32_t {
  return active_file_id_;
}

auto TransientEngineState::is_rotation_needed(std::uint64_t threshold) const
    -> bool {
  return (*files_.get(active_file_id_))->size() >= threshold;
}

auto TransientEngineState::next_lsn() const noexcept -> std::uint64_t {
  return next_lsn_;
}

void TransientEngineState::advance_next_lsn(std::uint64_t new_lsn) noexcept {
  next_lsn_ = new_lsn;
}

auto TransientEngineState::persistent() && -> std::shared_ptr<EngineState> {
  auto s = std::make_shared<EngineState>();
  s->key_dir = std::move(key_dir_).persistent();
  s->files = std::move(files_).persistent();
  s->file_stats = std::move(file_stats_).persistent();
  s->active_file_id = active_file_id_;
  s->next_file_id = next_file_id_;
  s->next_lsn = next_lsn_;
  return s;
}

#pragma endregion

#pragma region Construction

// Opens dir, runs recovery, creates initial active data file.
// Throws std::system_error if the directory cannot be prepared.
DB::DB(std::filesystem::path dir, Options opts)
    : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
      state_{std::make_shared<EngineState>()} {
  std::filesystem::create_directories(dir_);

  // Acquire exclusive advisory lock on the database directory.
  const auto lock_path = dir_ / ".lock";
  lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ == -1) {
    throw std::system_error{
        errno, std::generic_category(),
        std::format("DB: cannot open lock file '{}'", lock_path.string())};
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == -1) {
    auto err = errno;
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw std::system_error{
        err, std::generic_category(),
        std::format("DB: directory '{}' is locked by another process",
                    dir_.string())};
  }

  try {
    EngineState s;
    if (opts.recovery_threads <= 1) {
      s = recovery_load_serial(std::move(s), opts.fail_recovery_on_crc_errors);
    } else {
      s = recovery_load_parallel(std::move(s), opts.recovery_threads,
                                 opts.fail_recovery_on_crc_errors);
    }
    s.active_file_id = s.next_file_id++;
    const auto stem = make_data_file_stem();
    auto new_active = std::make_shared<DataFile>(dir_ / (stem + ".data"));
    auto files_t = s.files.transient();
    files_t.set(s.active_file_id, new_active);
    s.files = std::move(files_t).persistent();
    auto fstats_t = s.file_stats.transient();
    fstats_t.set(s.active_file_id, FileStats{});
    s.file_stats = std::move(fstats_t).persistent();
    store_state(std::make_shared<EngineState>(std::move(s)));
  } catch (...) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw;
  }
}

#pragma endregion

#pragma region Lifecycle

// Seals the active file, drains background hint tasks, writes hint files for
// all sealed files, then purges stale files.
// At destruction no readers are active.
DB::~DB() {
  auto s = load_state_for_write();
  if (!s->files.empty()) {
    try {
      auto &active = **s->files.get(s->active_file_id);
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
  if (lock_fd_ != -1) {
    ::close(lock_fd_);
    lock_fd_ = -1;
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
  auto s = load_state_for_read(opts);
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
  (*s->files.get(kv->file_id))
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
  auto s = load_state_for_read(ReadOptions{});
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

auto DB::snapshot() const -> Snapshot {
  ReadOptions opts{};
  return Snapshot{load_state_for_read(opts)};
}

// The single write path. Routes to either write_group_ (default) or
// solo_writer_ depending on plan characteristics. put/del/apply_batch are
// thin wrappers that construct a WritePlan and delegate here.
auto DB::apply_batch_if(WriteOptions opts,
                        WritePlan plan) -> bool {
  if (is_degraded()) throw DbDegraded{degraded_reason_};
  if (plan.empty()) return true;

  EngineSlot slot;
  slot.plan = std::move(plan);
  slot.opts = opts;
  slot.sync = opts.sync;

  const bool use_solo = opts.solo
      || slot.plan.write_bytes() > kGroupWriteMaxBytes;

  if (use_solo) {
    solo_writer_.submit(slot);
  } else {
    write_group_.submit(slot);
  }

  return slot.result;
}

#pragma endregion

#pragma region Writer executors

// Prepares and applies one slot against the transient. Pure in-memory: no
// I/O. Entries are appended to all_entries; running_offset is advanced by
// the total byte size of entries produced. Returns false on validation
// failure (slot.result set to false).
auto DB::execute_slot(TransientEngineState &t, EngineSlot &slot,
                      std::vector<AppendEntry> &all_entries,
                      std::uint64_t &running_offset) -> bool {
  if (slot.plan.empty()) {
    slot.result = true;
    return true;
  }

  if (!t.validate_preconditions(slot.plan)) {
    slot.result = false;
    return false;
  }

  auto entries = t.prepare_write(slot.plan);
  if (entries.empty()) {
    slot.result = true;
    return true;
  }

  // Pre-compute offsets from running_offset (tracks the file position
  // across all slots in the group, without actual I/O).
  std::vector<std::uint64_t> offsets(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    offsets[i] = running_offset;
    running_offset += entry_size(entries[i].key.size(),
                                 entries[i].value.size());
  }

  t.apply_writes(slot.plan, offsets);

  all_entries.insert(all_entries.end(),
                     std::make_move_iterator(entries.begin()),
                     std::make_move_iterator(entries.end()));

  slot.result = true;
  return true;
}

// Executor callback shared by solo_writer_ and write_group_. Three phases:
// (1) per-slot validate/prepare/apply in-memory, collecting entries;
// (2) one append_entries call for all collected entries;
// (3) sync/rotate/publish once.
void DB::execute_slots(std::vector<Slot *> &batch) {
  std::lock_guard<std::mutex> wg{*write_mu_};

  if (is_degraded()) {
    auto ex = std::make_exception_ptr(DbDegraded{degraded_reason_});
    for (auto *s : batch) s->err = ex;
    return;
  }

  auto current = load_state_for_write();
  auto t = current->transient();
  auto &file = t.active_file();
  auto running_offset = static_cast<std::uint64_t>(file.size());
  std::vector<AppendEntry> all_entries;
  auto any_sync = false;

  // Phase 1: pure in-memory — validate, prepare, pre-compute offsets,
  // apply_writes for each slot sequentially.
  for (auto *s : batch) {
    auto &slot = static_cast<EngineSlot &>(*s);
    slot.sync = slot.opts.sync;
    execute_slot(t, slot, all_entries, running_offset);
    any_sync |= slot.opts.sync;
  }

  if (all_entries.empty()) return;

  // Phase 2: one I/O call for all collected entries.
  std::vector<std::uint64_t> io_offsets(all_entries.size());
  try {
    file.append_entries(all_entries, io_offsets);
  } catch (...) {
    auto ex = std::current_exception();
    try { file.sync(); } catch (...) {}
    publish_lsn_advance(current, t.next_lsn());
    deem_as_degraded(std::format(
        "append IO error on '{}': call resume() to recover.",
        file.path().string()));
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }

  // Phase 3: sync/rotate/publish.
  if (t.is_rotation_needed(rotation_threshold_)) {
    try {
      file.sync();
    } catch (...) {
      auto ex = std::current_exception();
      publish_lsn_advance(current, t.next_lsn());
      deem_as_degraded(std::format(
          "rotation fdatasync failed on '{}': bytes in page cache but "
          "durability not confirmed. Call resume() to recover.",
          file.path().string()));
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
    try {
      rotate_active_file(t, current);
    } catch (...) {
      auto ex = std::current_exception();
      deem_as_degraded(std::format(
          "post-write rotation failed for '{}': active file is sealed "
          "but new file could not be created. Call resume() to recover.",
          file.path().string()));
      store_state(std::move(t).persistent());
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  if (any_sync) {
    try {
      file.sync();
    } catch (...) {
      auto ex = std::current_exception();
      publish_lsn_advance(current, t.next_lsn());
      deem_as_degraded(std::format(
          "commit fdatasync failed on '{}': bytes in page cache but "
          "durability not confirmed. Call resume() to recover.",
          file.path().string()));
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  store_state(std::move(t).persistent());
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
  (*state_->files.get(kv->file_id))
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
  auto s = load_state_for_read(opts);
  auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{s, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

// Returns an input range of keys >= from. Walks the in-memory key directory
// only; no disk I/O.
auto DB::keys_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto s = load_state_for_read(opts);
  auto it = from.empty() ? s->key_dir.begin() : s->key_dir.lower_bound(from);
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto DB::riter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator> {
  auto s = load_state_for_read(opts);
  auto begin_it = from.empty()
      ? s->key_dir.rbegin().base()
      : s->key_dir.upper_bound(from);
  auto end_it = s->key_dir.begin();
  return {ReverseEntryIterator{EntryIterator{s, std::move(begin_it), opts.verify_checksums}},
          ReverseEntryIterator{EntryIterator{s, std::move(end_it), opts.verify_checksums}}};
}

auto DB::rkeys_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto s = load_state_for_read(opts);
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
  if (is_degraded()) throw DbDegraded{degraded_reason_};

  // Drain in-flight background hint writes so that vacuum's
  // flush_hints_for call cannot race on the same .hint.tmp file.
  worker_.drain();
  vacuum_purge_stale_files();

  // Snapshot file_stats and active-file info.
  PersistentU32Map<FileStats> stats_snap;
  std::uint32_t active_id{};
  std::uint64_t active_size{};
  {
    auto s = load_state_for_write();
    stats_snap = s->file_stats;
    active_id = s->active_file_id;
    active_size = s->active_file().size();
  }

  // Find the highest-fragmentation sealed file above threshold.
  std::uint32_t target_id{};
  double worst_frag = 0.0;
  for (const auto [fid, fs] : stats_snap) {
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
  const auto target_live = stats_snap.get(target_id)->live_bytes;
  if (target_live <= opts.absorb_threshold &&
      target_live + active_size <= rotation_threshold_) {
    vacuum_absorb_file(target_id);
  } else {
    vacuum_compact_file(target_id);
  }
  return true;
}

#pragma endregion

#pragma region File rotation

// Seals the active file, dispatches hint file writing for it to the
// background worker, and opens a new active file.
// Caller must sync the active file before calling if durability is required.
void DB::rotate_active_file(TransientEngineState &t,
                            const std::shared_ptr<const EngineState> &current) {
  t.active_file().seal();
  auto sealed_file = *current->files.get(t.active_file_id());
  auto dir = dir_;
  worker_.dispatch([f = std::move(sealed_file), d = std::move(dir)] {
    flush_hints_for(f, d);
  });
  const auto stem = make_data_file_stem();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_rotate_file_creation);
#endif
  auto new_file = std::make_shared<DataFile>(dir_ / (stem + ".data"));
  t.apply_rotate_file(std::move(new_file));
}

#pragma endregion

#pragma region Hint internals

// Writes the hint file for a single data file using the temp-then-rename
// protocol. Batch-aware: entries between BulkBegin and BulkEnd are buffered
// and written only when BulkEnd is seen; an incomplete batch (crash
// mid-write) is silently discarded. Idempotent: skips files whose .hint
// already exists.
void DB::flush_hints_for(const std::shared_ptr<DataFile> &file,
                               const std::filesystem::path &dir,
                               bool strict) {
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

  try {
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
  } catch (const std::exception &e) {
    if (strict) throw;
    std::cerr << "bytecask: truncated entry in " << file->path()
              << " while generating hint file, recovering up to this point: "
              << e.what() << "\n";
    // Fall through — write hint file with entries collected so far.
    in_batch = false;  // discard any pending incomplete batch
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
  flush_hints(*load_state_for_write());
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
            dest_file.append_entry(entry.sequence, EntryType::Put, entry.key,
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
          dest_file.append_entry(entry.sequence, EntryType::Delete, entry.key, {});
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
  auto current = load_state_for_write();
  auto t = current->transient();
  t.apply_vacuum(old_file_id, scan, std::move(new_sealed_file));

  store_state(std::move(t).persistent());
}

// Stashes the old data file and its hint path for deferred removal
// once no in-flight readers reference it.
void DB::vacuum_defer_old_file(
    const std::shared_ptr<const EngineState> &snap, std::uint32_t file_id) {
  auto old_data_file = *snap->files.get(file_id);
  auto old_hint_path =
      dir_ / (old_data_file->path().stem().string() + ".hint");
  stale_files_.push_back({std::move(old_data_file), std::move(old_hint_path)});
}

// Rewrites a sealed file into a new sealed file containing only live
// entries and tombstones. Called under vacuum_mu_, not write_mu_.
// The new data file is written to .data.tmp, then renamed atomically.
// The old file is deferred for cleanup when no readers reference it.
void DB::vacuum_compact_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  const auto &old_file = **snap->files.get(file_id);

  const auto stem = make_data_file_stem();
  const auto tmp_data_path = dir_ / (stem + ".data.tmp");
  const auto final_data_path = dir_ / (stem + ".data");

  VacuumScanResult scan;
  {
#ifdef BYTECASK_TESTING
    FAULT_INJECTION(io_vacuum_compact_tmp_create);
#endif
    DataFile tmp_file(tmp_data_path);
    scan = vacuum_scan_and_copy(snap, old_file, tmp_file, file_id);
    tmp_file.sync();
  }

#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_vacuum_compact_rename);
#endif
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
  auto snap = load_state_for_write();
  const auto &old_file = **snap->files.get(file_id);

  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    auto &active = snap->active_file();
    const auto pre_vacuum_offset = active.size();
    try {
      auto scan = vacuum_scan_and_copy(snap, old_file, active, file_id);
#ifdef BYTECASK_TESTING
      FAULT_INJECTION(io_vacuum_absorb_sync);
#endif
      active.sync();
      vacuum_commit(file_id, scan, nullptr);
    } catch (...) {
      // Roll back bytes written during copy so file_stats stays consistent.
      try { active.truncate(pre_vacuum_offset); } catch (...) {}
      throw;
    }
  }
  vacuum_defer_old_file(snap, file_id);
}

#pragma endregion

#pragma region State access

void DB::publish_lsn_advance(const std::shared_ptr<const EngineState> &current,
                              std::uint64_t target_lsn) {
  auto lsn_only = current->transient();
  lsn_only.advance_next_lsn(target_lsn);
  store_state(std::move(lsn_only).persistent());
}

void DB::deem_as_degraded(std::string reason) {
  degraded_reason_ = std::move(reason);
  degraded_.store(true, std::memory_order_release);
}

void DB::resume() {
  if (!is_degraded()) return;

  auto guard = std::unique_lock<std::mutex>{*write_mu_};
  if (!is_degraded()) return;  // re-check under lock

  auto current = load_state_for_write();
  const auto old_file_id = current->active_file_id;
  auto &file = **current->files.get(old_file_id);

  // Scan the active file (batch-aware) to find the last valid committed offset
  // and collect valid committed entries for key_dir replay. Entries written to
  // disk but never published to EngineState (sync-failure paths, degraded
  // transitions between IO and state publication) would otherwise be invisible
  // until cold restart.
  Offset valid_offset = 0;
  Offset off = 0;
  bool in_batch = false;
  Offset batch_start = 0;
  std::vector<ResumeEntry> committed;
  std::vector<ResumeEntry> pending;  // staging buffer for current batch
  try {
    while (auto result = file.scan(off)) {
      const auto entry_off = off;
      const auto &[entry, next] = *result;
      switch (entry.entry_type) {
        case EntryType::BulkBegin:
          in_batch = true;
          batch_start = off;
          pending.clear();
          break;
        case EntryType::BulkEnd:
          in_batch = false;
          valid_offset = next;
          for (auto &pe : pending) {
            committed.push_back(std::move(pe));
          }
          pending.clear();
          break;
        case EntryType::Put:
        case EntryType::Delete: {
          ResumeEntry re{entry.sequence, entry.entry_type, entry_off,
                         narrow<std::uint32_t>(entry.value.size()),
                         entry.key};
          if (in_batch) {
            pending.push_back(std::move(re));
          } else {
            valid_offset = next;
            committed.push_back(std::move(re));
          }
          break;
        }
      }
      off = next;
    }
  } catch (...) {
    // Stop at first CRC error — valid_offset is the last known-good position.
    // Discard any pending incomplete batch.
    pending.clear();
  }
  if (in_batch) {
    valid_offset = batch_start;  // discard orphaned batch
    pending.clear();
  }

  // Remove garbage bytes / orphaned batch markers via truncation.
  if (file.size() != valid_offset) {
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

  // Seal and dispatch hint generation. Both are idempotent: seal() is a flag
  // set, and flush_hints_for skips files whose .hint already exists.
  file.seal();
  worker_.dispatch([f = *current->files.get(old_file_id), d = dir_] {
    flush_hints_for(f, d);
  });

  // Create the new active file (may throw → stays degraded).
  const auto stem = make_data_file_stem();
#ifdef BYTECASK_TESTING
  FAULT_INJECTION(io_resume_file_creation);
#endif
  auto new_file = std::make_shared<DataFile>(dir_ / (stem + ".data"));

  // Build and publish new state. Replay scanned entries into key_dir so that
  // entries on disk but not yet in EngineState become visible.
  auto t = current->transient();
  t.file_stats().update(old_file_id, [valid_offset](FileStats &fs) {
    fs.total_bytes = valid_offset;
  });
  t.apply_resume(old_file_id, committed);
  t.apply_rotate_file(std::move(new_file));
  store_state(std::move(t).persistent());

  // Success: clear degraded flag.
  degraded_.store(false, std::memory_order_release);
}

// Returns the engine state from a thread-local cache (read path only).
// The hot path is a single relaxed load of state_time_ (plain MOV on x86).
// The snapshot is refreshed only when the last write timestamp exceeds
// staleness_tolerance (session mode: tolerance=0, refreshes on every write).
// Returns a reference to the thread-local snapshot. The snapshot stays
// alive until the same thread calls load_state_for_read again, so callers must
// not stash the reference across a second load_state_for_read call.
auto DB::load_state_for_read(const ReadOptions &opts) const
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

auto DB::load_state_for_write() const -> std::shared_ptr<EngineState> {
  return state_.load();
}

void DB::store_state(std::shared_ptr<EngineState> s) {
  state_.store(std::move(s));
  state_time_.store(now_ns(), std::memory_order_release);
}

#pragma endregion

#pragma region Recovery

// Phase 1 shared by serial and parallel recovery: remove stale .hint.tmp
// files, open all data files, seal them, register in s.files, and
// generate missing hint files. Returns the RecoveredFile list.
auto DB::recovery_prepare_files(EngineState &s, bool strict)
    -> std::vector<RecoveredFile> {
  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    const auto &p = dir_entry.path();
    if (p.extension() == ".tmp" &&
        (p.stem().extension() == ".hint" || p.stem().extension() == ".data")) {
      std::filesystem::remove(p);
    }
  }

  std::vector<RecoveredFile> files;
  auto files_t = s.files.transient();

  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    const auto &p = dir_entry.path();
    if (p.extension() != ".data") {
      continue;
    }

    const auto file_id = s.next_file_id++;
    auto data_file = std::make_shared<DataFile>(p);
    data_file->seal();
    files_t.set(file_id, data_file);

    const auto hint_path = dir_ / (p.stem().string() + ".hint");
    if (!std::filesystem::exists(hint_path)) {
      flush_hints_for(data_file, dir_, strict);
    }

    files.push_back({file_id, std::move(data_file), hint_path,
                     std::filesystem::file_size(p)});
  }

  s.files = std::move(files_t).persistent();
  return files;
}

// Builds a RecoveryResult from a subset of hint files.
// Each worker calls this independently — no shared mutable state.
// When strict is false, corrupt or unreadable hint files are skipped
// with a warning instead of throwing.
auto DB::recovery_build_from_hints(std::span<RecoveredFile> files, bool strict)
    -> RecoveryResult {
  std::uint64_t max_lsn = 0;
  auto t = PersistentRadixTree<KeyDirEntry>{}.transient();
  std::map<Key, std::uint64_t> tombstones;
  auto fstats_t = PersistentU32Map<FileStats>{}.transient();

  for (const auto &rf : files) {
    fstats_t.set(rf.file_id, FileStats{0, rf.total_bytes});
  }

  // live_bytes are NOT tracked per-entry here — Phase 4 in
  // recovery_load_parallel recomputes them in a single pass after the
  // final merge, avoiding redundant O(N) map lookups per worker.
  auto lsn_wins = [](const KeyDirEntry &existing, const KeyDirEntry &incoming) {
    return existing.sequence < incoming.sequence;
  };

  for (auto &[file_id, data_file, hint_path, tb] : files) {
    try {
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
    } catch (const std::exception &e) {
      if (strict) throw;
      std::fprintf(stderr,
                   "bytecask: skipping hint file '%s' due to CRC error: %s\n",
                   hint_path.string().c_str(), e.what());
    }
  }

  return {std::move(t).persistent(), std::move(tombstones), max_lsn,
          std::move(fstats_t).persistent()};
}

// Merges two RecoveryResults. Tree merge uses LSN-based conflict
// resolution, then tombstones from both sides are cross-applied to
// suppress stale PUTs. Tombstone maps and file_stats are unioned.
// live_bytes are NOT recomputed here — deferred to a single pass
// after the final merge to avoid O(N × log₂ W) redundant traversals.
auto DB::recovery_merge_results(RecoveryResult a, RecoveryResult b)
-> RecoveryResult {
  auto merged_stats_t = a.file_stats.transient();
  for (const auto [fid, fs] : b.file_stats) {
    merged_stats_t.set(fid, fs);
  }
  a.file_stats = std::move(merged_stats_t).persistent();

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
          std::max(a.max_lsn, b.max_lsn), std::move(a.file_stats)};
}

// Reconstructs the key directory from hint files (serial path).
// Pre-generates missing hint files from raw data scans (batch-aware),
// then recovers exclusively from hints — single code path.
// Returns a new EngineState with key_dir populated and next_lsn set to
// max_seen + 1. next_file_id is advanced for each recovered file.
auto DB::recovery_load_serial(EngineState s, bool strict) -> EngineState {
  auto files = recovery_prepare_files(s, strict);

  // Use a plain hash map for live_bytes accumulation — in-place mutation is
  // O(1) per entry vs. the copy-out/write-back overhead of TransientU32Map::update().
  // Converted to PersistentU32Map once at the end.
  std::unordered_map<std::uint32_t, FileStats> fstats_scratch;
  for (const auto &rf : files) {
    fstats_scratch.emplace(rf.file_id, FileStats{0, rf.total_bytes});
  }

  std::uint64_t max_lsn = 0;
  auto transient_key_dir = s.key_dir.transient();
  std::map<Key, std::uint64_t> tombstones;

  for (auto &[file_id, data_file, hint_path, tb] : files) {
    try {
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
              const auto dec =
                  entry_size(he->key.size(), existing->value_size);
              fstats_scratch[existing->file_id].live_bytes -= dec;
            }
            const auto inc = entry_size(he->key.size(), he->value_size);
            fstats_scratch[file_id].live_bytes += inc;
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
            const auto dec =
                entry_size(he->key.size(), existing->value_size);
            fstats_scratch[existing->file_id].live_bytes -= dec;
            transient_key_dir.erase(he->key);
          }
        }
        if (he->sequence > max_lsn) max_lsn = he->sequence;
      }
    } catch (const std::exception &e) {
      if (strict) throw;
      std::fprintf(stderr,
                   "bytecask: skipping hint file '%s' due to CRC error: %s\n",
                   hint_path.string().c_str(), e.what());
    }
  }

  auto fstats_t = PersistentU32Map<FileStats>{}.transient();
  for (const auto &[id, fs] : fstats_scratch) fstats_t.set(id, fs);
  s.key_dir = std::move(transient_key_dir).persistent();
  s.file_stats = std::move(fstats_t).persistent();
  s.next_lsn = max_lsn + 1;
  return s;
}

// Parallel recovery: file-level partitioning with sequential accumulator merge.
// Round-robin assigns files to W workers, each builds a RecoveryResult,
// then results are merged one-at-a-time into an accumulator as workers finish.
auto DB::recovery_load_parallel(EngineState s, unsigned recovery_threads,
                                bool strict) -> EngineState {
  auto files = recovery_prepare_files(s, strict);

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
  std::vector<std::exception_ptr> worker_errors(W, nullptr);
  unsigned finished_count = 0;

  {
    std::vector<std::jthread> threads;
    threads.reserve(W);
    for (unsigned i = 0; i < W; ++i) {
      threads.emplace_back([&, i] {
        try {
          auto result = recovery_build_from_hints(worker_files[i], strict);
          std::unique_lock<std::mutex> lk{queue_mu};
          queue.push_back(std::move(result));
          ++finished_count;
          queue_cv.notify_one();
        } catch (...) {
          std::unique_lock<std::mutex> lk{queue_mu};
          worker_errors[i] = std::current_exception();
          ++finished_count;  // still advances so main thread doesn't deadlock
          queue_cv.notify_one();
        }
      });
    }

    // Main thread: consume results as they arrive.
    RecoveryResult acc{};
    bool acc_initialized = false;
    unsigned merged_count = 0;

    while (merged_count < W) {
      std::unique_lock<std::mutex> lk{queue_mu};
      queue_cv.wait(lk, [&] { return finished_count > merged_count; });
      std::vector<RecoveryResult> local;
      local.swap(queue);
      merged_count = finished_count;  // advance past all finished, including errored
      lk.unlock();

      for (auto &incoming : local) {
        if (!acc_initialized) {
          acc = std::move(incoming);
          acc_initialized = true;
        } else {
          acc = recovery_merge_results(std::move(acc), std::move(incoming));
        }
      }
    }

    // Store final result for phases 4-5 (threads join at scope exit).
    queue.clear();
    queue.push_back(std::move(acc));
  }

  // Threads are joined. Propagate any worker exceptions now.
  for (const auto &err : worker_errors) {
    if (err) {
      if (strict) std::rethrow_exception(err);
      // lenient: warning already emitted inside recovery_build_from_hints
    }
  }

  auto &final_result = queue[0];

  // Phase 4: recompute live_bytes once from the fully-merged tree.
  // Accumulate into a hash map (O(1) in-place), then apply to PersistentU32Map
  // in a single pass over the (small) file set — avoids O(N) radix tree
  // mutations for N key_dir entries.
  std::unordered_map<std::uint32_t, std::uint64_t> live_accum;
  for (auto it = final_result.key_dir.begin(); it != std::default_sentinel;
       ++it) {
    const auto &[key_span, kde] = *it;
    live_accum[kde.file_id] += entry_size(key_span.size(), kde.value_size);
  }
  auto fstats_t = final_result.file_stats.transient();
  for (const auto [fid, _] : final_result.file_stats) {
    const auto acc_it = live_accum.find(fid);
    const auto live = (acc_it != live_accum.end()) ? acc_it->second : 0ULL;
    fstats_t.update(fid, [live](FileStats &fs) { fs.live_bytes = live; });
  }
  final_result.file_stats = std::move(fstats_t).persistent();

  // Phase 5: assembly.
  s.key_dir = std::move(final_result.key_dir);
  s.next_lsn = final_result.max_lsn + 1;
  s.file_stats = std::move(final_result.file_stats);
  return s;
}

#pragma endregion

} // namespace bytecask
