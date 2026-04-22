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
    std::uint64_t next_seq, std::uint64_t durable_seq)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_file_id_{next_file_id}, next_seq_{next_seq},
      durable_seq_{durable_seq} {}

auto EngineState::transient() const -> TransientEngineState {
  return TransientEngineState{
      key_dir.transient(), files.transient(), file_stats.transient(),
      active_file_id, next_file_id, next_seq, durable_seq};
}

auto TransientEngineState::validate_preconditions(const WritePlan &plan) const
    -> bool {
  const auto *snap_state =
      plan.snap_ ? plan.snap_->state_.get() : nullptr;

  // 1. Point guards.
  for (const auto &[key, guard] : plan.guards_) {
    const std::span<const std::byte> key_span{key};
    const auto cur_entry = key_dir_.get(key_span);

    switch (guard.precondition) {
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
    for (const auto &w : plan.writes_) {
      const std::byte *key_data = nullptr;
      std::size_t key_len = 0;
      std::visit(
          [&](const auto &op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
              key_data = op.key.data();
              key_len = op.key.size();
            } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
              key_data = op.key.data();
              key_len = op.key.size();
            }
          },
          w);
      if (!key_data) continue; // RangeDel — skip
      const std::span<const std::byte> key_span{key_data, key_len};
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

  auto seq = next_seq_;

  if (multi) {
    entries.push_back({seq++, EntryType::BulkBegin, {}, {}});
  }

  for (const auto &w : plan.writes_) {
    std::visit(
        [&entries, &seq](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
            entries.push_back(
                {seq++, EntryType::Put,
                 std::span<const std::byte>{op.key},
                 std::span<const std::byte>{op.value}});
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            entries.push_back(
                {seq++, EntryType::Delete,
                 std::span<const std::byte>{op.key}, {}});
          } else {
            entries.push_back(
                {seq++, EntryType::RangeDel,
                 std::span<const std::byte>{op.from},
                 std::span<const std::byte>{op.to}});
          }
        },
        w);
  }

  if (multi) {
    entries.push_back({seq++, EntryType::BulkEnd, {}, {}});
  }

  return entries;
}

void TransientEngineState::apply_writes(
    const WritePlan &plan, std::span<const std::uint64_t> offsets) {
  std::size_t io_idx = 0;
  const auto wc = plan.write_count();
  const bool multi = wc > 1;
  const auto batch_start_seq = next_seq_;

  // Account for BulkBegin marker.
  if (multi) {
    file_stats_.update(active_file_id_, [](FileStats &fs) {
      fs.total_bytes += kHeaderSize + kCrcSize;
    });
    ++next_seq_;
    ++io_idx;
  }

  for (const auto &w : plan.writes_) {
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
            const std::span<const std::byte> key_span{op.key};
            const auto existing = key_dir_.get(key_span);
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size);
              const auto ef = existing->file_id;
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
            }
            const auto val_size = narrow<std::uint32_t>(op.value.size());
            const auto sz = entry_size(key_span.size(), val_size);
            file_stats_.update(active_file_id_, [sz](FileStats &fs) {
              fs.live_bytes += sz;
              fs.total_bytes += sz;
            });
            key_dir_.set(key_span, KeyDirEntry{next_seq_, active_file_id_,
                                                offsets[io_idx], val_size});
            ++next_seq_;
            ++io_idx;
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            const std::span<const std::byte> key_span{op.key};
            const auto existing = key_dir_.get(key_span);
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size);
              const auto ef = existing->file_id;
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
            }
            const auto del_sz = entry_size(key_span.size(), 0);
            file_stats_.update(active_file_id_, [del_sz](FileStats &fs) {
              fs.total_bytes += del_sz;
            });
            key_dir_.erase(key_span);
            ++next_seq_;
            ++io_idx;
          } else {
            // RangeDel: iterate key_dir in [from, to), decrement live_bytes
            // on each affected file, erase from key_dir, then account for
            // the entry itself (total_bytes only — tombstones are not live).
            const std::span<const std::byte> from_span{op.from};
            const std::span<const std::byte> to_span{op.to};

            // Collect keys to erase — cannot erase during iteration.
            std::vector<Key> to_erase;
            for (auto it = key_dir_.lower_bound(from_span);
                 it != std::default_sentinel; ++it) {
              auto [key_span, entry] = *it;
              if (Key{key_span} >= Key{to_span}) break;
              const auto dec =
                  entry_size(key_span.size(), entry.value_size);
              const auto ef = entry.file_id;
              file_stats_.update(
                  ef, [dec](FileStats &fs) { fs.live_bytes -= dec; });
              to_erase.emplace_back(key_span);
            }
            for (const auto &k : to_erase) {
              key_dir_.erase(std::span<const std::byte>{k});
            }

            const auto rd_sz = entry_size(op.from.size(), op.to.size());
            file_stats_.update(active_file_id_, [rd_sz](FileStats &fs) {
              fs.total_bytes += rd_sz;
            });
            ++next_seq_;
            ++io_idx;
          }
        },
        w);
  }

  // Account for BulkEnd marker.
  if (multi) {
    file_stats_.update(active_file_id_, [](FileStats &fs) {
      fs.total_bytes += kHeaderSize + kCrcSize;
    });
    ++next_seq_;
    ++io_idx;
  }

  // Track per-file sequence bounds.
  const auto batch_end_seq = next_seq_ - 1;
  file_stats_.update(
      active_file_id_,
      [batch_start_seq, batch_end_seq](FileStats &fs) {
        if (fs.min_sequence == 0 || batch_start_seq < fs.min_sequence)
          fs.min_sequence = batch_start_seq;
        if (batch_end_seq > fs.max_sequence)
          fs.max_sequence = batch_end_seq;
      });
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
                    FileStats{actual_live_bytes, scan.total_bytes,
                              scan.min_sequence, scan.max_sequence});
  } else {
    file_stats_.update(dest_file_id, [actual_live_bytes,
                                      total = scan.total_bytes,
                                      smin = scan.min_sequence,
                                      smax = scan.max_sequence](FileStats &fs) {
      fs.live_bytes += actual_live_bytes;
      fs.total_bytes += total;
      if (smin > 0 && (fs.min_sequence == 0 || smin < fs.min_sequence))
        fs.min_sequence = smin;
      if (smax > fs.max_sequence) fs.max_sequence = smax;
    });
  }
}

void TransientEngineState::apply_resume(
    std::uint32_t file_id, const std::vector<ResumeEntry> &entries,
    std::uint64_t valid_offset) {
  // Reset file stats for the truncated file — apply_resume owns the
  // full stats lifecycle so callers don't need mutable file_stats access.
  file_stats_.update(file_id, [valid_offset](FileStats &fs) {
    fs.total_bytes = valid_offset;
    fs.min_sequence = 0;
    fs.max_sequence = 0;
  });

  std::uint64_t max_seq = 0;
  std::uint64_t seq_min = 0;
  std::uint64_t seq_max = 0;
  for (const auto &e : entries) {
    const std::span<const std::byte> key_span{e.key};
    if (e.sequence > max_seq) max_seq = e.sequence;
    if (seq_min == 0 || e.sequence < seq_min) seq_min = e.sequence;
    if (e.sequence > seq_max) seq_max = e.sequence;

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

  if (max_seq >= next_seq_) {
    next_seq_ = max_seq + 1;
  }

  if (seq_max > 0) {
    file_stats_.update(file_id, [seq_min, seq_max](FileStats &fs) {
      fs.min_sequence = seq_min;
      fs.max_sequence = seq_max;
    });
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

auto TransientEngineState::next_seq() const noexcept -> std::uint64_t {
  return next_seq_;
}

void TransientEngineState::advance_next_seq(std::uint64_t new_seq) noexcept {
  next_seq_ = new_seq;
}

void TransientEngineState::apply_sync(std::uint64_t batch_max_seq) {
  if (batch_max_seq > durable_seq_) {
    durable_seq_ = batch_max_seq;
  }
}

auto TransientEngineState::durable_seq() const noexcept -> std::uint64_t {
  return durable_seq_;
}

auto TransientEngineState::persistent() && -> std::shared_ptr<EngineState> {
  auto s = std::make_shared<EngineState>();
  s->key_dir = std::move(key_dir_).persistent();
  s->files = std::move(files_).persistent();
  s->file_stats = std::move(file_stats_).persistent();
  s->active_file_id = active_file_id_;
  s->next_file_id = next_file_id_;
  s->next_seq = next_seq_;
  s->durable_seq = durable_seq_;
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
    auto initial = std::make_shared<EngineState>(std::move(s));
    // All recovered entries were previously synced.
    initial->durable_seq =
        initial->next_seq > 0 ? initial->next_seq - 1 : 0;
    validate_state_consistency(*initial);
    store_initial_state(std::move(initial));
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
  if (kv->value_size == 0) {
    out.clear();
    return true;
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
  (void)apply_batch(opts, std::move(plan));
}

auto DB::del(const WriteOptions &opts, BytesView key) -> bool {
  WritePlan plan;
  plan.ensure_present(key);
  plan.del(key);
  return apply_batch(opts, std::move(plan));
}

void DB::del_range(const WriteOptions &opts, BytesView from, BytesView to) {
  if (Key{from} >= Key{to}) return;
  WritePlan plan;
  plan.del_range(from, to);
  (void)apply_batch(opts, std::move(plan));
}

auto DB::contains_key(BytesView key) const -> bool {
  auto s = load_state_for_read(ReadOptions{});
  return s->key_dir.contains(key);
}

#pragma endregion

#pragma region Snapshot and apply_batch

auto DB::snapshot() const -> Snapshot {
  ReadOptions opts{};
  return Snapshot{load_state_for_read(opts)};
}

// The single write path. Routes to either write_group_ (default) or
// solo_writer_ depending on plan characteristics. put/del/apply_batch are
// thin wrappers that construct a WritePlan and delegate here.
auto DB::apply_batch(WriteOptions opts,
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

  // Highest sequence in this batch — used to advance durable_seq after sync.
  const auto batch_max_seq = t.next_seq() - 1;

  // Phase 2: one I/O call for all collected entries.
  std::vector<std::uint64_t> io_offsets(all_entries.size());
  try {
    file.append_entries(all_entries, io_offsets);
  } catch (...) {
    auto ex = std::current_exception();
    try { file.sync(); } catch (...) {}
    publish_seq_advance(current, t.next_seq());
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
      t.apply_sync(batch_max_seq);
    } catch (...) {
      auto ex = std::current_exception();
      publish_seq_advance(current, t.next_seq());
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
      store_state(current, std::move(t).persistent());
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  if (any_sync) {
    try {
      file.sync();
      t.apply_sync(batch_max_seq);
    } catch (...) {
      auto ex = std::current_exception();
      publish_seq_advance(current, t.next_seq());
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

  store_state(current, std::move(t).persistent());
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
  if (kv->value_size == 0) {
    out.clear();
    return true;
  }
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

  // Snapshot file_stats and active-file info.
  PersistentU32Map<FileStats> stats_snap;
  std::uint32_t active_id{};
  {
    auto s = load_state_for_write();
    stats_snap = s->file_stats;
    active_id = s->active_file_id;
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

  const auto target_live = stats_snap.get(target_id)->live_bytes;

  // Fast path: file has no live keys — skip scan entirely.
  if (target_live == 0) {
    vacuum_remove_file(target_id);
    return true;
  }

  // All files with live entries are compacted (sealed→sealed).
  vacuum_compact_file(target_id);
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
    std::vector<std::byte> end_key; // non-empty only for RangeDel
  };

  auto hint = HintFile::OpenForWrite(tmp_path);
  std::vector<PendingHint> entries;

  try {
    for (const auto &[entry, entry_off] : scan_committed(*file)) {
      if (entry.entry_type == EntryType::BulkBegin ||
          entry.entry_type == EntryType::BulkEnd) {
        continue;
      }
      entries.push_back(
          {entry.sequence, entry.entry_type, entry_off,
           narrow<std::uint32_t>(entry.value.size()), entry.key,
           entry.entry_type == EntryType::RangeDel
               ? std::vector<std::byte>{entry.value.begin(),
                                        entry.value.end()}
               : std::vector<std::byte>{}});
    }
  } catch (const std::exception &e) {
    if (strict) throw;
    std::cerr << "bytecask: truncated entry in " << file->path()
              << " while generating hint file, recovering up to this point: "
              << e.what() << "\n";
  }

  // Sort by key for prefix compression benefit in the hint file.
  std::ranges::sort(entries, [](const auto &a, const auto &b) {
    return a.key < b.key;
  });

  for (const auto &pe : entries) {
    if (pe.type == EntryType::RangeDel) {
      hint.append_range_del(pe.seq, pe.file_off, pe.key, pe.end_key);
    } else {
      hint.append(pe.seq, pe.type, pe.file_off, pe.key, pe.val_size);
    }
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

// Scans source_file and copies live entries into dest_file.
// Live Puts (still current in snap->key_dir for source_file_id),
// all tombstones, and BulkBegin/BulkEnd markers are emitted.
// Incomplete batches at EOF are silently discarded by the iterator.
auto DB::vacuum_scan_and_copy(
    const std::shared_ptr<const EngineState> &snap,
    const DataFile &source_file, DataFile &dest_file,
    std::uint32_t source_file_id) -> VacuumScanResult {
  VacuumScanResult result;

  auto track_seq = [&](std::uint64_t seq) {
    if (result.min_sequence == 0 || seq < result.min_sequence)
      result.min_sequence = seq;
    if (seq > result.max_sequence) result.max_sequence = seq;
  };

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
        track_seq(entry.sequence);
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
      track_seq(entry.sequence);
      break;
    }
    case EntryType::RangeDel: {
      std::ignore =
          dest_file.append_entry(entry.sequence, EntryType::RangeDel,
                                 entry.key, entry.value);
      result.total_bytes += entry_size(entry.key.size(), entry.value.size());
      track_seq(entry.sequence);
      break;
    }
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      std::ignore =
          dest_file.append_entry(entry.sequence, entry.entry_type, {}, {});
      track_seq(entry.sequence);
      break;
    }
  };

  for (const auto &[entry, entry_off] : scan_committed(source_file)) {
    emit_entry(entry, entry_off);
  }

  return result;
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

  store_state(current, std::move(t).persistent());
}

// Unlinks the old data and hint files from the filesystem. Existing readers
// continue via their open fds (POSIX: pread succeeds on unlinked files).
void DB::vacuum_unlink_old_file(
    const std::shared_ptr<const EngineState> &snap, std::uint32_t file_id) {
  auto old_data_file = *snap->files.get(file_id);
  auto old_hint_path =
      dir_ / (old_data_file->path().stem().string() + ".hint");
  std::filesystem::remove(old_data_file->path());
  std::filesystem::remove(old_hint_path);
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
  vacuum_unlink_old_file(snap, file_id);
}

// Removes a sealed file that has no live keys. No I/O scan needed — just
// commit the state change and unlink the files. Called under vacuum_mu_.
void DB::vacuum_remove_file(std::uint32_t file_id) {
  auto snap = load_state_for_write();
  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    VacuumScanResult empty{};
    vacuum_commit(file_id, empty, nullptr);
  }
  vacuum_unlink_old_file(snap, file_id);
}

// Appends live entries from a sealed file to the active file, then
#pragma endregion

#pragma region State access

void DB::publish_seq_advance(const std::shared_ptr<const EngineState> &current,
                              std::uint64_t target_seq) {
  auto seq_only = current->transient();
  seq_only.advance_next_seq(target_seq);
  store_state(current, std::move(seq_only).persistent());
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

  // Scan the active file to find the last valid committed offset
  // and collect valid committed entries for key_dir replay. Entries written to
  // disk but never published to EngineState (sync-failure paths, degraded
  // transitions between IO and state publication) would otherwise be invisible
  // until cold restart.
  Offset valid_offset = 0;
  std::vector<ResumeEntry> committed;
  try {
    auto iter = CommittedEntryIterator{DataFileIterator{file}};
    while (!(iter == std::default_sentinel)) {
      const auto &[entry, entry_off] = *iter;
      if (entry.entry_type != EntryType::BulkBegin &&
          entry.entry_type != EntryType::BulkEnd) {
        committed.push_back({entry.sequence, entry.entry_type, entry_off,
                             narrow<std::uint32_t>(entry.value.size()),
                             entry.key});
      }
      ++iter;
    }
    valid_offset = iter.committed_offset();
  } catch (...) {
    // Stop at first CRC error — valid_offset is the last known-good position.
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
  t.apply_resume(old_file_id, committed, valid_offset);
  t.apply_rotate_file(std::move(new_file));
  // All entries recovered from disk were previously synced.
  t.apply_sync(t.next_seq() > 0 ? t.next_seq() - 1 : 0);
  auto resumed = std::move(t).persistent();
  validate_state_consistency(*resumed);
  store_state(current, std::move(resumed));

  // Success: clear degraded flag.
  degraded_.store(false, std::memory_order_release);
}

auto DB::current_sequence(std::chrono::milliseconds timeout) const
    -> std::uint64_t {
  auto baseline = load_state()->durable_seq;
  if (timeout <= std::chrono::milliseconds{0}) return baseline;

  std::unique_lock<std::mutex> lk{durable_mu_};
  durable_cv_.wait_for(lk, timeout, [&] {
    return load_state()->durable_seq > baseline;
  });
  return load_state()->durable_seq;
}

auto DB::create_manifest() -> FileManifest {
  std::shared_ptr<const EngineState> manifest_state;
  std::uint64_t through_seq;
  {
    std::lock_guard<std::mutex> wg{*write_mu_};
    if (is_degraded()) throw DbDegraded{degraded_reason_};

    auto current = load_state_for_write();
    auto t = current->transient();

    // Sync active file to make all entries durable.
    auto &file = t.active_file();
    file.sync();
    const auto max_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
    t.apply_sync(max_seq);

    // Seal active file, dispatch hint generation, open new active.
    rotate_active_file(t, current);

    through_seq = max_seq;
    store_state(current, std::move(t).persistent());

    // Capture state under write_mu_ — a concurrent write after
    // store_state but before load_state would produce a snapshot
    // with entries beyond through_sequence, breaking the contract.
    manifest_state = load_state_for_write();
  }

  // Wait for all background hint generation to complete.
  worker_.drain();

  // Build manifest from sealed files.
  std::vector<FileInfo> files;
  for (const auto [file_id, file_ptr] : manifest_state->files) {
    if (file_id == manifest_state->active_file_id) continue;
    const auto data_path = file_ptr->path();
    const auto stem = data_path.stem().string();
    files.push_back({file_id, data_path, dir_ / (stem + ".hint")});
  }

  return FileManifest{Snapshot{manifest_state}, std::move(files), through_seq};
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
    tl.snapshot = load_state();
    tl.last_write_time = wt;
  }
  return tl.snapshot;
}

auto DB::load_state_for_write() const -> std::shared_ptr<EngineState> {
  return load_state();
}

void DB::store_state(const std::shared_ptr<const EngineState> &old_state,
                     std::shared_ptr<EngineState> new_state) {
  // O(1) invariant checks — always on, even in release.
  if (new_state->next_seq < old_state->next_seq) {
    deem_as_degraded(std::format(
        "invariant violation: next_seq regressed from {} to {}",
        old_state->next_seq, new_state->next_seq));
    return;
  }
  if (new_state->active_file_id < old_state->active_file_id) {
    deem_as_degraded(std::format(
        "invariant violation: active_file_id regressed from {} to {}",
        old_state->active_file_id, new_state->active_file_id));
    return;
  }
  if (new_state->next_file_id < old_state->next_file_id) {
    deem_as_degraded(std::format(
        "invariant violation: next_file_id regressed from {} to {}",
        old_state->next_file_id, new_state->next_file_id));
    return;
  }
  if (new_state->durable_seq < old_state->durable_seq) {
    deem_as_degraded(std::format(
        "invariant violation: durable_seq regressed from {} to {}",
        old_state->durable_seq, new_state->durable_seq));
    return;
  }

  const auto durable_advanced =
      new_state->durable_seq > old_state->durable_seq;

#ifndef NDEBUG
  // Debug-only O(n) check: next_seq > max(all key_dir sequences).
  std::uint64_t max_seq = 0;
  for (auto it = new_state->key_dir.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (entry.sequence > max_seq) max_seq = entry.sequence;
  }
  if (max_seq > 0 && new_state->next_seq <= max_seq) {
    deem_as_degraded(std::format(
        "invariant violation: next_seq {} <= max key_dir sequence {}",
        new_state->next_seq, max_seq));
    return;
  }
#endif

  store_state(std::move(new_state));
  state_time_.store(now_ns(), std::memory_order_release);

  if (durable_advanced) {
    { std::lock_guard<std::mutex> lk{durable_mu_}; }
    durable_cv_.notify_all();
  }
}

void DB::store_initial_state(std::shared_ptr<EngineState> s) {
  store_state(std::move(s));
  state_time_.store(now_ns(), std::memory_order_release);
}

void DB::validate_state_consistency(const EngineState &s) const {
  // 1. Active file exists in files registry.
  if (!s.files.contains(s.active_file_id)) {
    throw std::runtime_error{std::format(
        "state consistency: active_file_id {} not in files registry",
        s.active_file_id)};
  }

  // 4. file_stats covers all files.
  for (const auto [file_id, _] : s.files) {
    if (!s.file_stats.contains(file_id)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} missing from file_stats", file_id)};
    }
  }

  // O(n) key_dir walk: verify dangling file refs, live_bytes, and next_seq.
  // Expensive — only enabled in test builds.
#ifdef BYTECASK_TESTING
  std::map<std::uint32_t, std::uint64_t> computed_live;
  std::uint64_t max_seq = 0;
  for (auto it = s.key_dir.begin(); it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (!s.files.contains(entry.file_id)) {
      throw std::runtime_error{std::format(
          "state consistency: key references file_id {} not in registry",
          entry.file_id)};
    }
    computed_live[entry.file_id] +=
        entry_size(key_span.size(), entry.value_size);
    if (entry.sequence > max_seq) max_seq = entry.sequence;
  }

  if (max_seq > 0 && s.next_seq <= max_seq) {
    throw std::runtime_error{std::format(
        "state consistency: next_seq {} <= max key_dir sequence {}",
        s.next_seq, max_seq)};
  }

  for (const auto [file_id, fs] : s.file_stats) {
    auto it = computed_live.find(file_id);
    auto expected_live = (it != computed_live.end()) ? it->second : 0ULL;
    if (fs.live_bytes != expected_live) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} live_bytes={} but key_dir says {}",
          file_id, fs.live_bytes, expected_live)};
    }
  }
#endif

  // 6. min_sequence / max_sequence coherence.
  for (const auto [file_id, fs] : s.file_stats) {
    if ((fs.min_sequence == 0) != (fs.max_sequence == 0)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} has min_sequence={} max_sequence={} "
          "(one is zero, the other is not)",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
    if (fs.min_sequence > 0 && fs.min_sequence > fs.max_sequence) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} min_sequence {} > max_sequence {}",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
  }
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
  std::uint64_t max_seq = 0;
  auto t = PersistentRadixTree<KeyDirEntry>{}.transient();
  std::map<Key, std::uint64_t> tombstones;
  std::vector<RangeTombstone> range_tombstones;

  // Use a plain hash map for file_stats accumulation — in-place mutation is
  // O(1) per entry vs. the copy-out/write-back overhead of TransientU32Map::update().
  // Converted to PersistentU32Map once at the end.
  std::unordered_map<std::uint32_t, FileStats> fstats_scratch;
  for (const auto &rf : files) {
    fstats_scratch.emplace(rf.file_id, FileStats{0, rf.total_bytes});
  }

  // live_bytes are NOT tracked per-entry here — Phase 4 in
  // recovery_load_parallel recomputes them in a single pass after the
  // final merge, avoiding redundant O(N) map lookups per worker.
  auto seq_wins = [](const KeyDirEntry &existing, const KeyDirEntry &incoming) {
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
            if (he->sequence > max_seq) max_seq = he->sequence;
            continue;
          }
          // Check range tombstones — O(R) per Put, R expected small.
          bool suppressed = false;
          for (const auto &rt : range_tombstones) {
            if (rt.seq >= he->sequence && k >= rt.start && k < rt.end) {
              suppressed = true;
              break;
            }
          }
          if (suppressed) {
            if (he->sequence > max_seq) max_seq = he->sequence;
            continue;
          }
          t.upsert(he->key,
                   KeyDirEntry{he->sequence, file_id, he->file_offset,
                               he->value_size},
                   seq_wins);
        } else if (he->entry_type == EntryType::Delete) {
          const auto k = Key{he->key};
          auto &tomb_seq = tombstones[k];
          if (he->sequence > tomb_seq) tomb_seq = he->sequence;
          const auto existing = t.get(he->key);
          if (existing && existing->sequence < he->sequence) {
            t.erase(he->key);
          }
        } else if (he->entry_type == EntryType::RangeDel) {
          const auto start = Key{he->key};
          const auto end = Key{he->end_key};
          range_tombstones.push_back({start, end, he->sequence});
          // Erase keys in [start, end) with sequence < this tombstone.
          std::vector<Key> to_erase;
          for (auto it = t.lower_bound(he->key);
               it != std::default_sentinel; ++it) {
            auto [key_span, entry] = *it;
            if (Key{key_span} >= end) break;
            if (entry.sequence < he->sequence) {
              to_erase.emplace_back(key_span);
            }
          }
          for (const auto &ek : to_erase) {
            t.erase(std::span<const std::byte>{ek});
          }
        }
        if (he->sequence > max_seq) max_seq = he->sequence;
        auto &file_fs = fstats_scratch[file_id];
        if (file_fs.min_sequence == 0 || he->sequence < file_fs.min_sequence)
          file_fs.min_sequence = he->sequence;
        if (he->sequence > file_fs.max_sequence)
          file_fs.max_sequence = he->sequence;
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

  return {std::move(t).persistent(), std::move(tombstones),
          std::move(range_tombstones), max_seq,
          std::move(fstats_t).persistent()};
}

// Merges two RecoveryResults. Tree merge uses sequence-based conflict
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

  auto seq_resolver = [](const KeyDirEntry &x, const KeyDirEntry &y) {
    return (x.sequence >= y.sequence) ? x : y;
  };

  auto merged =
      PersistentRadixTree<KeyDirEntry>::merge(a.key_dir, b.key_dir, seq_resolver);

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

  // Cross-apply range tombstones from both sides.
  auto cross_apply_range_tombs =
      [](PersistentRadixTree<KeyDirEntry> &tree,
         const std::vector<RangeTombstone> &rts) {
        for (const auto &rt : rts) {
          std::vector<Key> to_erase;
          for (auto it = tree.lower_bound(
                   std::span<const std::byte>{rt.start.begin(), rt.start.size()});
               it != std::default_sentinel; ++it) {
            auto [key_span, entry] = *it;
            if (Key{key_span} >= rt.end) break;
            if (entry.sequence < rt.seq) {
              to_erase.emplace_back(key_span);
            }
          }
          for (const auto &ek : to_erase) {
            tree = tree.erase(std::span<const std::byte>{ek});
          }
        }
      };
  cross_apply_range_tombs(merged, b.range_tombstones);
  cross_apply_range_tombs(merged, a.range_tombstones);

  // Union range tombstone vectors.
  auto &merged_range_tombs = a.range_tombstones;
  merged_range_tombs.insert(merged_range_tombs.end(),
                            std::make_move_iterator(b.range_tombstones.begin()),
                            std::make_move_iterator(b.range_tombstones.end()));

  return {std::move(merged), std::move(merged_tombs),
          std::move(merged_range_tombs),
          std::max(a.max_seq, b.max_seq), std::move(a.file_stats)};
}

// Reconstructs the key directory from hint files (serial path).
// Pre-generates missing hint files from raw data scans,
// then recovers exclusively from hints — single code path.
// Returns a new EngineState with key_dir populated and next_seq set to
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

  std::uint64_t max_seq = 0;
  auto transient_key_dir = s.key_dir.transient();
  std::map<Key, std::uint64_t> tombstones;
  std::vector<RangeTombstone> range_tombstones;

  for (auto &[file_id, data_file, hint_path, tb] : files) {
    try {
      auto hint = HintFile::OpenForRead(hint_path);
      auto scanner = hint.make_scanner();
      while (auto he = scanner.next()) {
        if (he->entry_type == EntryType::Put) {
          const auto k = Key{he->key};
          const auto tomb_it = tombstones.find(k);
          if (tomb_it != tombstones.end() && tomb_it->second >= he->sequence) {
            if (he->sequence > max_seq) max_seq = he->sequence;
            continue;
          }
          // Check range tombstones.
          bool suppressed = false;
          for (const auto &rt : range_tombstones) {
            if (rt.seq >= he->sequence && k >= rt.start && k < rt.end) {
              suppressed = true;
              break;
            }
          }
          if (suppressed) {
            if (he->sequence > max_seq) max_seq = he->sequence;
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
        } else if (he->entry_type == EntryType::RangeDel) {
          const auto start = Key{he->key};
          const auto end = Key{he->end_key};
          range_tombstones.push_back({start, end, he->sequence});
          // Erase keys in [start, end) with sequence < this tombstone.
          std::vector<Key> to_erase;
          for (auto it = transient_key_dir.lower_bound(he->key);
               it != std::default_sentinel; ++it) {
            auto [key_span, entry] = *it;
            if (Key{key_span} >= end) break;
            if (entry.sequence < he->sequence) {
              const auto dec = entry_size(key_span.size(), entry.value_size);
              fstats_scratch[entry.file_id].live_bytes -= dec;
              to_erase.emplace_back(key_span);
            }
          }
          for (const auto &ek : to_erase) {
            transient_key_dir.erase(std::span<const std::byte>{ek});
          }
        }
        if (he->sequence > max_seq) max_seq = he->sequence;
        auto &file_fs = fstats_scratch[file_id];
        if (file_fs.min_sequence == 0 || he->sequence < file_fs.min_sequence)
          file_fs.min_sequence = he->sequence;
        if (he->sequence > file_fs.max_sequence)
          file_fs.max_sequence = he->sequence;
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
  s.next_seq = max_seq + 1;
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
  s.next_seq = final_result.max_seq + 1;
  s.file_stats = std::move(final_result.file_stats);
  return s;
}

#pragma endregion

#pragma region Change Since


// ---------------------------------------------------------------------------
// ChangeIterator implementation
// ---------------------------------------------------------------------------
// Lazy ChangeIterator: walks files in min_sequence order, scanning one
// batch at a time. Only the current batch's entries are held in memory.
// Files in a single-writer engine have disjoint, monotonically increasing
// sequence ranges, so iterating files by min_sequence and scanning forward
// within each file yields globally ascending sequence order.
class ChangeIterator::Impl {
public:
  Impl(std::shared_ptr<const EngineState> state,
       std::uint64_t from_sequence,
       std::uint64_t durable_sequence)
    : state_(std::move(state)), from_sequence_(from_sequence),
      durable_sequence_(durable_sequence) {

    // Build sorted file list — O(num_files), typically tiny.
    for (const auto [file_id, stats] : state_->file_stats) {
      if (stats.max_sequence > from_sequence && stats.min_sequence <= durable_sequence) {
        file_queue_.emplace_back(stats.min_sequence, file_id);
      }
    }
    std::sort(file_queue_.begin(), file_queue_.end());

    // Position at first valid entry.
    advance_to_next_valid();
  }

  auto has_more() const -> bool { return has_entry_; }

  auto current() const -> const RawEntry& { return cached_entry_; }

  void advance() {
    advance_to_next_valid();
  }

private:
  auto should_include(std::uint64_t seq) const -> bool {
    return seq > from_sequence_ && seq <= durable_sequence_;
  }

  // Scans forward across entries and files until a valid entry is found
  // or all files are exhausted. The cached BytesView spans point into
  // the iterator's current entry, so we must NOT advance the iterator
  // after caching — that would invalidate the view. Instead, we set
  // needs_advance_ and increment on the next call.
  void advance_to_next_valid() {
    has_entry_ = false;

    // Advance past the previously cached entry (deferred from last call).
    if (needs_advance_ && entry_iter_) {
      ++(*entry_iter_);
      needs_advance_ = false;
    }

    while (true) {
      // Try next entry in the current file.
      if (entry_iter_ && !(*entry_iter_ == std::default_sentinel)) {
        const auto& [entry, entry_off] = **entry_iter_;
        if (should_include(entry.sequence)) {
          cache_entry(entry);
          needs_advance_ = true;
          return;
        }
        ++(*entry_iter_);
        continue;
      }

      // Try next file.
      if (file_idx_ < file_queue_.size()) {
        auto file_id = file_queue_[file_idx_].second;
        ++file_idx_;
        auto file_ptr = state_->files.get(file_id);
        if (file_ptr) {
          entry_iter_.emplace(DataFileIterator{**file_ptr});
        }
        continue;
      }

      // All files exhausted.
      return;
    }
  }

  // Caches the current entry. The BytesView spans point into
  // the iterator's internal buffer which stays alive until advance().
  void cache_entry(const DataEntry& entry) {
    cached_entry_ = {
      .sequence = entry.sequence,
      .entry_type = entry.entry_type,
      .key = BytesView{entry.key},
      .value = BytesView{entry.value}
    };
    has_entry_ = true;
  }

  std::shared_ptr<const EngineState> state_;
  std::uint64_t from_sequence_;
  std::uint64_t durable_sequence_;

  // File traversal — sorted by min_sequence.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> file_queue_;
  std::size_t file_idx_{0};

  // Current file's committed entry scanner.
  std::optional<CommittedEntryIterator> entry_iter_;
  bool needs_advance_{false};

  // Cached current entry — BytesView into entry_iter_'s internal buffer.
  bool has_entry_{false};
  RawEntry cached_entry_;
};

// ChangeIterator methods
ChangeIterator::ChangeIterator(std::shared_ptr<const EngineState> state,
                               std::uint64_t from_sequence,
                               std::uint64_t durable_sequence)
  : impl_(std::make_unique<Impl>(std::move(state), from_sequence, durable_sequence)) {}

ChangeIterator::~ChangeIterator() = default;

ChangeIterator::ChangeIterator(ChangeIterator&&) noexcept = default;
ChangeIterator& ChangeIterator::operator=(ChangeIterator&&) noexcept = default;

auto ChangeIterator::operator++() -> ChangeIterator& {
  if (impl_) {
    impl_->advance();
  }
  return *this;
}

void ChangeIterator::operator++(int) {
  ++(*this);
}

auto ChangeIterator::operator*() const -> const value_type& {
  return impl_->current();
}

auto ChangeIterator::operator==(std::default_sentinel_t) const noexcept -> bool {
  return !impl_ || !impl_->has_more();
}

// DB::changes_since implementation
auto DB::changes_since(const Snapshot& snap, std::uint64_t from_sequence) const
    -> std::ranges::subrange<ChangeIterator, std::default_sentinel_t> {

  auto state = snap.state();
  auto begin = ChangeIterator{state, from_sequence, state->durable_seq};
  return {std::move(begin), std::default_sentinel};
}

#pragma endregion

} // namespace bytecask
