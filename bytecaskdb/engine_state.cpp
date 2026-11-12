// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — TransientEngineState: the mutable engine state the write path builds and freezes

module;
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

module bytecask;

namespace bytecask {

TransientEngineState::TransientEngineState(
    KeyDirTransient key_dir,
    TransientU32Table<std::shared_ptr<DataFile>> files,
    FileStatsTransient file_stats,
    std::uint32_t active_file_id, std::uint64_t next_seq, std::uint64_t durable_seq,
    std::uint64_t sync_requested_seq,
    Mode mode, bool degraded, std::string degraded_reason,
    std::shared_ptr<const std::vector<ChangeMarker>> change_markers,
    std::size_t scratch_bytes)
    : key_dir_{std::move(key_dir)}, files_{std::move(files)},
      file_stats_{std::move(file_stats)}, active_file_id_{active_file_id},
      next_seq_{next_seq},
      durable_seq_{durable_seq}, sync_requested_seq_{sync_requested_seq},
      mode_{mode}, degraded_{degraded},
      degraded_reason_{std::move(degraded_reason)},
      change_markers_{std::move(change_markers)},
      scratch_bytes_{scratch_bytes} {}

auto EngineState::transient() const -> TransientEngineState {
  auto t = TransientEngineState{
      key_dir.transient(), files.transient(), file_stats.transient(),
      active_file_id, next_seq, durable_seq,
      sync_requested_seq, mode, degraded, degraded_reason, change_markers,
      scratch_bytes};
  t.recent_writes_ = recent_writes;
  return t;
}

void TransientEngineState::record_writes(const WritePlan &plan) {
  if (!recent_writes_) return;
  recent_writes_->record(plan.checks_, next_seq_ - 1,
                         [&](auto &&f) { plan.for_each_point_key(f); });
}

void TransientEngineState::raise_recent_floor() {
  if (!recent_writes_) return;
  recent_writes_->raise_floor(next_seq_ > 0 ? next_seq_ - 1 : 0);
}

void TransientEngineState::append_marker(ChangeMarker m) {
  auto list = change_markers_ ? *change_markers_ : std::vector<ChangeMarker>{};
  list.push_back(m);
  change_markers_ =
      std::make_shared<const std::vector<ChangeMarker>>(std::move(list));
}

void TransientEngineState::apply_change_marker(std::uint64_t sequence,
                                               std::uint64_t id) {
  const auto sz = change_marker_size(EntryType::ChangeMarker);
  file_stats_.patch(active_file_id_,
                    {.total_added = sz, .change_marker_added = sz,
                     .min_sequence = sequence, .max_sequence = sequence});
  if (sequence >= next_seq_) next_seq_ = sequence + 1;
  append_marker({sequence, id});
}

auto TransientEngineState::validate_preconditions(
    const WritePlan &plan, std::uint64_t &rests_on, bool defer_puts) const
    -> bool {
  return check_preconditions(key_dir_, kd_ctx(), next_seq_, plan, rests_on,
                             defer_puts);
}

template <typename KeyDir>
auto TransientEngineState::check_preconditions(
    const KeyDir &key_dir, const KeyDirCtx &ctx, std::uint64_t next_seq,
    const WritePlan &plan, std::uint64_t &rests_on, bool defer_puts) -> bool {
  const auto *snap_state =
      plan.snap_ ? plan.snap_->state_.get() : nullptr;
  // The head's latest sequence: what an outcome rests on when the entry it
  // read has no sequence of its own (deleted, or absent).
  const auto head_latest = next_seq - 1;
  const auto read_seq = [&](const std::optional<KeyDirEntry> &cur) {
    return cur ? cur->sequence() : head_latest;
  };
  const auto lost = [&](const std::optional<KeyDirEntry> &cur) {
    rests_on = read_seq(cur);
    return false;
  };
  std::uint64_t held_on = 0;

  // 1. Point guards.
  for (const auto &[key, guard] : plan.guards_) {
    const std::span<const std::byte> key_span{key};
    const auto cur_entry = kd_get(key_dir, key_span, ctx);

    switch (guard.precondition) {
    case WritePlan::Precondition::MustExist:
      if (!cur_entry) return lost(std::nullopt);
      held_on = std::max(held_on, read_seq(cur_entry));
      break;
    case WritePlan::Precondition::MustBeAbsent:
      if (cur_entry) return lost(cur_entry);
      held_on = std::max(held_on, read_seq(cur_entry));
      break;
    case WritePlan::Precondition::MustBeUnchanged: {
      // ensure_unchanged already enforced snap_ is present at build time.
      const auto snap_entry = kd_get(snap_state->key_dir, key_span, snap_state->kd_ctx());
      const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
      const std::uint64_t cur_seq = cur_entry ? cur_entry->sequence() : 0;
      if (cur_seq != snap_seq) return lost(cur_entry);
      held_on = std::max(held_on, read_seq(cur_entry));
      break;
    }
    case WritePlan::Precondition::None:
      break;
    }
  }

  // 2. Range guards (only present when snap_ is set — enforced at build time).
  // One that holds read keys absent from both states too, which have no
  // sequence: it rests on the head's latest.
  for (const auto &rg : plan.range_guards_) {
    if (range_changed(key_dir, ctx, next_seq, *snap_state, rg.from, rg.to,
                      rests_on))
      return false;
    held_on = head_latest;
  }

  // 3. Implicit W-W check on all write keys (only when snapshot present).
  // defer_puts is set only for a plan of puts alone, whose checks
  // apply_puts_checked makes in the descents that apply them.
  if (snap_state && !defer_puts) {
    // The key changed since the snapshot unless the head maps it to the
    // snapshot's record, or it is absent from both. The head is asked by
    // location first, which reads nothing; only a key that moved is read,
    // to tell a changed key from one vacuum relocated.
    const auto point_conflict = [&](std::size_t i, const Bytes &key) {
      const std::span<const std::byte> key_span{key};
      const auto snap_entry = plan.snapshot_entry(i, key_span);
      if (snap_entry && kd_holds(key_dir, key_span, *snap_entry)) return false;
      const auto cur_entry = kd_get(key_dir, key_span, ctx);
      const bool appeared = !snap_entry && cur_entry;
      const bool deleted = snap_entry && !cur_entry;
      const bool modified = snap_entry && cur_entry &&
                            cur_entry->sequence() != snap_entry->sequence();
      if (!(appeared || deleted || modified)) return false;
      rests_on = read_seq(cur_entry);
      return true;
    };
    for (std::size_t i = 0; i < plan.writes_.size(); ++i) {
      bool has_conflict = false;
      std::visit(
          [&](const auto &op) -> void {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, WritePlan::PointPut> ||
                          std::is_same_v<T, WritePlan::PointDel>) {
              has_conflict = point_conflict(i, op.key);
            } else if constexpr (std::is_same_v<T, WritePlan::RangeDel>) {
              has_conflict = range_changed(key_dir, ctx, next_seq, *snap_state,
                                           op.from, op.to, rests_on);
            }
          },
          plan.writes_[i]);
      if (has_conflict) return false;
    }
  }

  rests_on = held_on;
  return true;
}

template <typename KeyDir>
auto TransientEngineState::range_changed(
    const KeyDir &key_dir, const KeyDirCtx &ctx, std::uint64_t next_seq,
    const EngineState &snap, std::span<const std::byte> from,
    std::span<const std::byte> to, std::uint64_t &rests_on) -> bool {
  // Keys changed or inserted since the snapshot: present in the head with a
  // sequence the snapshot does not hold for them. A key vacuum relocated
  // keeps its sequence, so it is not a change.
  for (auto it = kd_lower_bound(key_dir, from, ctx);
       it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (Key{key_span} >= Key{to}) break;
    const auto snap_entry = kd_get(snap.key_dir, key_span, snap.kd_ctx());
    const std::uint64_t snap_seq = snap_entry ? snap_entry->sequence() : 0;
    if (entry.sequence() != snap_seq) {
      rests_on = entry.sequence();
      return true;
    }
  }
  // Keys deleted since the snapshot, by del or by a range tombstone: present
  // in the snapshot, absent from the head. They leave no entry to carry a
  // sequence, so a retry has to see the head's latest.
  for (auto it = kd_lower_bound(snap.key_dir, from, snap.kd_ctx());
       it != std::default_sentinel; ++it) {
    auto [key_span, entry] = *it;
    if (Key{key_span} >= Key{to}) break;
    if (!kd_get(key_dir, key_span, ctx)) {
      rests_on = next_seq - 1;
      return true;
    }
  }
  return false;
}

void WritePlan::resolve_snapshot_entries() {
  if (!snap_) return;
  const auto &s = *snap_->state_;
  snap_entries_.clear();
  snap_entries_.reserve(writes_.size());
  for (const auto &w : writes_) {
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, PointPut> ||
                        std::is_same_v<T, PointDel>) {
            snap_entries_.push_back(kd_get(s.key_dir, op.key, s.kd_ctx()));
          } else {
            snap_entries_.emplace_back();
          }
        },
        w);
  }
}

namespace {
// Whether [from, to) holds exactly the keys that start with `from`: `to` is
// the smallest key above every such key. Its bytes are `from`'s with the
// trailing 0xFF bytes dropped and the last one left raised by one.
auto is_prefix_range(std::span<const std::byte> from,
                     std::span<const std::byte> to) -> bool {
  auto n = from.size();
  while (n > 0 && from[n - 1] == std::byte{0xFF}) --n;
  if (n == 0 || to.size() != n) return false;
  return std::ranges::equal(from.first(n - 1), to.first(n - 1)) &&
         std::to_integer<unsigned>(to[n - 1]) ==
             std::to_integer<unsigned>(from[n - 1]) + 1;
}
}  // namespace

void WritePlan::prepare_validation(const EngineState &p,
                                   std::uint64_t generation,
                                   RecentWrites &recent) {
  auto &c = checks_;
  c.held = false;
  c.hashed = false;
  c.probes.clear();
  c.ranges.clear();
  c.writes.clear();
  c.prefixes.clear();
  c.generation = generation;
  c.seq = p.next_seq - 1;
  // The written keys are hashed here only once the table exists: before it,
  // the section records nothing, and a plan that slips in as it appears is
  // hashed by the section (RecentWrites::record).
  if (recent.has_table()) {
    RecentWrites::Lengths lengths{};
    const auto n = recent.lengths(lengths);
    c.writes.reserve(writes_.size());
    c.prefixes.reserve(writes_.size() * n);
    for_each_point_key([&](std::span<const std::byte> key) {
      c.writes.push_back(key_hash(key));
      for (const auto len : std::span{lengths}.first(n)) {
        if (key.size() >= len) c.prefixes.push_back(key_hash(key.first(len)));
      }
    });
    c.lengths = n;
    c.hashed = true;
  }

  // A plan with no writes appends nothing, and its rests_on needs the
  // section's answer (settle_unappended). A range delete changes keys the
  // table cannot name.
  if (writes_.empty() || has_range_delete()) return;
  // The same check the section makes, deferring the same writes to the
  // descents that apply them.
  const auto defer = TransientEngineState::can_apply_puts_checked(*this);
  c.writes_checked = !snap_ || !defer;
  // A plan with no guard and no write-write check left to make (no
  // snapshot, or puts the descents check) has nothing to read: it holds
  // whatever happens, so no check is made and nothing is probed.
  const auto checks_writes_at_p = snap_ && !defer;
  if (guards_.empty() && range_guards_.empty() && !checks_writes_at_p) {
    c.held = true;
    return;
  }
  for (const auto &rg : range_guards_) {
    if (!is_prefix_range(rg.from, rg.to)) return;
  }
  std::uint64_t ignored = 0;
  if (!TransientEngineState::check_preconditions(
          p.key_dir, p.kd_ctx(), p.next_seq, *this, ignored, defer))
    return;
  c.probes.reserve(guards_.size() + writes_.size());
  c.ranges.reserve(range_guards_.size());
  for (const auto &[key, guard] : guards_) {
    if (guard.precondition != Precondition::None)
      c.probes.push_back(key_hash(key));
  }
  if (checks_writes_at_p) {
    for_each_point_key([&](std::span<const std::byte> key) {
      c.probes.push_back(key_hash(key));
    });
  }
  for (const auto &rg : range_guards_) {
    c.ranges.push_back({key_hash(rg.from), narrow<std::uint32_t>(rg.from.size())});
  }
  // The answer read something: the table must exist for the section to
  // tell whether it still holds. Allocated here, not under the lock.
  recent.ensure_table();
  c.held = true;
}

void WritePlan::prepare_paths(const EngineState &head,
                              std::uint64_t generation) {
  const auto ctx = head.kd_ctx();
  paths_.assign(writes_.size(), PreparedPath{});
  for (std::size_t i = 0; i < writes_.size(); ++i) {
    const auto resolved = resolved_entry(i).has_value();
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, PointPut>) {
            kd_prepare_path(head.key_dir, op.key, ctx, !resolved, paths_[i]);
          } else if constexpr (std::is_same_v<T, PointDel>) {
            // An unresolved delete erases by key, which reads the key to
            // find it: a path would not save that read.
            if (resolved)
              kd_prepare_path(head.key_dir, op.key, ctx, false, paths_[i]);
          }
        },
        writes_[i]);
  }
  paths_generation_ = generation;
}

auto WritePlan::snapshot_entry(std::size_t i, BytesView key) const
    -> std::optional<KeyDirEntry> {
  if (i < snap_entries_.size()) return snap_entries_[i];
  const auto &s = *snap_->state_;
  return kd_get(s.key_dir, key, s.kd_ctx());
}

void TransientEngineState::prepare_write(
    const WritePlan &plan, std::vector<DataEntryView> &entries) const {
  const auto wc = plan.write_count();
  if (wc == 0) return;

  const bool multi = wc > 1;
  // No reserve: `entries` is the group's list, and an exact reserve per slot
  // would reallocate it for every slot. push_back grows it geometrically.

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
}

void TransientEngineState::apply_writes(
    const WritePlan &plan, std::span<const std::uint64_t> offsets) {
  PathFollow follow;
  const auto path = [&](std::size_t w) { return plan.path(w); };
  std::size_t io_idx = 0;
  const auto wc = plan.write_count();
  const bool multi = wc > 1;
  const auto batch_start_seq = next_seq_;
  constexpr auto kMarker = marker_size(EntryType::BulkBegin);

  // Account for BulkBegin marker.
  if (multi) {
    file_stats_.patch(active_file_id_,
                      {.total_added = kMarker, .marker_added = kMarker});
    ++next_seq_;
    ++io_idx;
  }

  for (std::size_t w_idx = 0; w_idx < plan.writes_.size(); ++w_idx) {
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, WritePlan::PointPut>) {
            const std::span<const std::byte> key_span{op.key};
            const auto val_size = narrow<std::uint32_t>(op.value.size());
            note_pending(offsets[io_idx], next_seq_, key_span, val_size);
            const auto entry = KeyDirEntry::make(next_seq_, offsets[io_idx],
                                                 active_file_id_, val_size);
            // An update of a key still at the snapshot's record goes by that
            // location and reads nothing; anything else is placed by key.
            const auto &at = plan.resolved_entry(w_idx);
            const bool by_location =
                at && kd_put_at(key_dir_, key_span, *at, entry, path(w_idx),
                                follow);
            const auto existing =
                by_location ? std::optional{kd_hit(*at)}
                            : kd_put(key_dir_, key_span, entry, kd_ctx(),
                                     path(w_idx), follow);
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              file_stats_.patch(existing->file_id(), {.live_removed = dec});
            }
            const auto sz = entry_size(key_span.size(), val_size);
            file_stats_.patch(active_file_id_, {.live_added = sz, .total_added = sz});
            ++next_seq_;
            ++io_idx;
          } else if constexpr (std::is_same_v<T, WritePlan::PointDel>) {
            const std::span<const std::byte> key_span{op.key};
            const auto &at = plan.resolved_entry(w_idx);
            const bool by_location =
                at && kd_erase_at(key_dir_, key_span, *at, path(w_idx), follow);
            const auto existing =
                by_location ? std::optional{kd_hit(*at)}
                            : kd_erase(key_dir_, key_span, kd_ctx());
            if (existing) {
              const auto dec =
                  entry_size(key_span.size(), existing->value_size());
              file_stats_.patch(existing->file_id(), {.live_removed = dec});
            }
            const auto del_sz = entry_size(key_span.size(), 0);
            file_stats_.patch(active_file_id_, {.total_added = del_sz, .tombstone_added = del_sz});
            ++next_seq_;
            ++io_idx;
          } else {
            // RangeDel: iterate key_dir in [from, to), decrement live_bytes
            // on each affected file, erase from key_dir, then account for
            // the entry itself (a tombstone: total and tombstone bytes, never
            // live).
            const std::span<const std::byte> from_span{op.from};
            const std::span<const std::byte> to_span{op.to};

            // Collect keys to erase — cannot erase during iteration.
            std::vector<Key> to_erase;
            for (auto it = kd_lower_bound(key_dir_, from_span, kd_ctx());
                 it != std::default_sentinel; ++it) {
              auto [key_span, entry] = *it;
              if (Key{key_span} >= Key{to_span}) break;
              const auto dec =
                  entry_size(key_span.size(), entry.value_size());
              file_stats_.patch(entry.file_id(), {.live_removed = dec});
              to_erase.emplace_back(key_span);
            }
            for (const auto &k : to_erase) {
              (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
            }

            const auto rd_sz = entry_size(op.from.size(), op.to.size());
            file_stats_.patch(active_file_id_, {.total_added = rd_sz, .tombstone_added = rd_sz});
            ++next_seq_;
            ++io_idx;
          }
        },
        plan.writes_[w_idx]);
  }

  // Account for BulkEnd marker.
  if (multi) {
    file_stats_.patch(active_file_id_,
                      {.total_added = kMarker, .marker_added = kMarker});
    ++next_seq_;
    ++io_idx;
  }

  file_stats_.patch(active_file_id_, {.min_sequence = batch_start_seq, .max_sequence = next_seq_ - 1});
  record_writes(plan);
  // A range delete changes keys RecentWrites cannot name.
  if (plan.has_range_delete()) raise_recent_floor();
}

auto TransientEngineState::can_apply_puts_checked(const WritePlan &plan)
    -> bool {
  if constexpr (!kKeyDirReadsKeys) {
    return false;
  } else {
    if (!plan.snap_ || plan.writes_.empty() ||
        plan.snap_entries_.size() != plan.writes_.size())
      return false;
    return std::ranges::all_of(plan.writes_, [](const WritePlan::WriteOp &w) {
      return std::holds_alternative<WritePlan::PointPut>(w);
    });
  }
}

auto TransientEngineState::apply_puts_checked(
    const WritePlan &plan, std::span<const std::uint64_t> offsets,
    std::vector<PutUndo> &undo) -> bool {
  PathFollow follow;
  // One per put applied, newest last. `key` points into the plan, which
  // outlives this call. Reserved up front, so a push never allocates after
  // the descent it records has changed the tree.
  undo.clear();
  undo.reserve(plan.writes_.size());

  const bool multi = plan.write_count() > 1;
  const auto first_seq = next_seq_;
  const std::size_t first_io = multi ? 1 : 0;
  std::size_t io_idx = first_io;
  if (multi) ++next_seq_;  // BulkBegin
  std::uint64_t put_bytes = 0;

  // Takes back every change the plan made, newest first, by location: no
  // record is read. The plan's pending records go too, up to and including
  // the put that failed, and its sequences are released.
  const auto abandon = [&]() -> bool {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
      const bool undone =
          it->was ? kd_restore(key_dir_, it->key, it->now, *it->was)
                  : kd_remove(key_dir_, it->key, it->now);
      if (!undone)
        throw std::logic_error{"apply_puts_checked: undo found a key moved "
                               "from where the plan put it"};
    }
    // io_idx is the put that failed: its pending record was just noted.
    for (auto i = first_io; i <= io_idx; ++i)
      pending_.erase(pending_slot(active_file_id_, offsets[i]));
    next_seq_ = first_seq;
    return false;
  };

  for (std::size_t w_idx = 0; w_idx < plan.writes_.size(); ++w_idx) {
    const auto &op = std::get<WritePlan::PointPut>(plan.writes_[w_idx]);
    const std::span<const std::byte> key_span{op.key};
    const auto val_size = narrow<std::uint32_t>(op.value.size());
    note_pending(offsets[io_idx], next_seq_, key_span, val_size);
    const auto entry = KeyDirEntry::make(next_seq_, offsets[io_idx],
                                         active_file_id_, val_size);
    const auto *path = plan.path(w_idx);
    if (const auto &at = plan.resolved_entry(w_idx)) {
      if (!kd_put_at(key_dir_, key_span, *at, entry, path, follow))
        return abandon();
      undo.push_back({key_span, entry, kd_hit(*at)});
    } else {
      undo.push_back({key_span, entry,
                      kd_put(key_dir_, key_span, entry, kd_ctx(), path,
                             follow)});
      // An insert that displaced a record: the key appeared since the
      // snapshot. The record goes back with the rest of the plan.
      if (undo.back().was) return abandon();
    }
    put_bytes += entry_size(key_span.size(), val_size);
    ++next_seq_;
    ++io_idx;
  }
  if (multi) ++next_seq_;  // BulkEnd

  // Confirmed. The file stats land as apply_writes would leave them. They
  // are recorded only now: a recorded patch cannot be taken back, and
  // abandon() must leave none behind.
  const auto markers =
      multi ? 2 * marker_size(EntryType::BulkBegin) : std::uint64_t{0};
  file_stats_.patch(active_file_id_, {.live_added = put_bytes,
                                      .total_added = put_bytes + markers,
                                      .marker_added = markers,
                                      .min_sequence = first_seq,
                                      .max_sequence = next_seq_ - 1});
  for (const auto &u : undo) {
    if (u.was)
      file_stats_.patch(u.was->file_id(),
                        {.live_removed = entry_size(u.key.size(),
                                                    u.was->value_size())});
  }
  record_writes(plan);
  return true;
}

void TransientEngineState::apply_ingest(
    std::span<const DataEntryView> entries,
    std::span<const std::uint64_t> offsets) {
  assert(entries.size() == offsets.size());
  if (entries.empty()) return;

  const auto batch_start_seq = entries.front().sequence;
  std::uint64_t max_seq = 0;

  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto &e = entries[i];
    const auto offset = offsets[i];
    if (e.sequence > max_seq) max_seq = e.sequence;

    switch (e.entry_type) {
    case EntryType::BulkBegin:
    case EntryType::BulkEnd: {
      const auto m = marker_size(e.entry_type);
      file_stats_.patch(active_file_id_, {.total_added = m, .marker_added = m});
      break;
    }

    case EntryType::ChangeMarker: {
      // The source's promotion, stored as this node's: from here on the two
      // histories carry the same identity (#397).
      const auto m = change_marker_size(e.entry_type);
      file_stats_.patch(active_file_id_,
                        {.total_added = m, .change_marker_added = m});
      append_marker({e.sequence, decode_marker_id(e.value)});
      break;
    }

    case EntryType::Put: {
      const auto val_size = narrow<std::uint32_t>(e.value.size());
      note_pending(offset, e.sequence, e.key, val_size);
      const auto existing = kd_put(
          key_dir_, e.key,
          KeyDirEntry::make(e.sequence, offset, active_file_id_, val_size),
          kd_ctx());
      if (existing) {
        const auto dec =
            entry_size(e.key.size(), existing->value_size());
        file_stats_.patch(existing->file_id(), {.live_removed = dec});
      }
      const auto sz = entry_size(e.key.size(), val_size);
      file_stats_.patch(active_file_id_, {.live_added = sz, .total_added = sz});
      break;
    }

    case EntryType::Delete: {
      const auto existing = kd_erase(key_dir_, e.key, kd_ctx());
      if (existing) {
        const auto dec =
            entry_size(e.key.size(), existing->value_size());
        file_stats_.patch(existing->file_id(), {.live_removed = dec});
      }
      const auto del_sz = entry_size(e.key.size(), 0);
      file_stats_.patch(active_file_id_, {.total_added = del_sz, .tombstone_added = del_sz});
      break;
    }

    case EntryType::RangeDel: {
      // key = from, value = to
      std::vector<Key> to_erase;
      for (auto it = kd_lower_bound(key_dir_, e.key, kd_ctx());
           it != std::default_sentinel; ++it) {
        auto [key_span, entry] = *it;
        if (Key{key_span} >= Key{e.value}) break;
        const auto dec =
            entry_size(key_span.size(), entry.value_size());
        file_stats_.patch(entry.file_id(), {.live_removed = dec});
        to_erase.emplace_back(key_span);
      }
      for (const auto &k : to_erase) {
        (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
      }
      const auto rd_sz = entry_size(e.key.size(), e.value.size());
      file_stats_.patch(active_file_id_, {.total_added = rd_sz, .tombstone_added = rd_sz});
      break;
    }
    }
  }

  // Advance next_seq past the highest ingested sequence.
  if (max_seq >= next_seq_) next_seq_ = max_seq + 1;

  file_stats_.patch(active_file_id_, {.min_sequence = batch_start_seq, .max_sequence = max_seq});
}

void TransientEngineState::apply_rotate_file(
    std::shared_ptr<DataFile> sealed_old, std::shared_ptr<DataFile> new_file) {
  const auto new_file_id = new_file->id();
  // Every record placed in the sealed file was written before the rotation
  // (a rotation follows a successful append), so the key directory reads it
  // from the file. Dropping them keeps the list in one file, where offsets
  // rise: the next file's id may be lower (PendingRecords).
  pending_.clear();
  files_.set(active_file_id_, std::move(sealed_old));
  active_file_id_ = new_file_id;
  files_.set(active_file_id_, std::move(new_file));
  file_stats_.set(active_file_id_, FileStats{});
}

void TransientEngineState::apply_vacuum(
    std::uint32_t old_file_id, const VacuumScanResult &scan,
    std::shared_ptr<DataFile> new_sealed_file) {
  if (!new_sealed_file) {
    // Removal: nothing in the file is live, so there is nothing to remap.
    if (!scan.mappings.empty())
      throw std::logic_error{"apply_vacuum: live entries without a destination"};
    files_.erase(old_file_id);
    file_stats_.erase(old_file_id);
    return;
  }

  // The destination is registered before the remap: a key directory that
  // reads keys back may resolve an already remapped record while placing the
  // next one.
  const auto dest_file_id = new_sealed_file->id();
  files_.set(dest_file_id, std::move(new_sealed_file));

  auto actual_live_bytes = scan.live_bytes;
  for (const auto &m : scan.mappings) {
    const std::span<const std::byte> key_span{m.key};
    const auto cur = kd_get(key_dir_, key_span, kd_ctx());
    if (cur && cur->sequence() == m.sequence) {
      (void)kd_put(key_dir_, key_span,
                   KeyDirEntry::make(m.sequence, m.new_offset, dest_file_id,
                                     m.value_size),
                   kd_ctx());
    } else {
      actual_live_bytes -= entry_size(m.key.size(), m.value_size);
    }
  }

  files_.erase(old_file_id);
  file_stats_.erase(old_file_id);
  file_stats_.set(dest_file_id,
                  FileStats{actual_live_bytes, scan.total_bytes,
                            scan.min_sequence, scan.max_sequence,
                            scan.tombstone_bytes, scan.marker_bytes,
                            scan.change_marker_bytes});
}

void TransientEngineState::apply_resume(
    std::uint32_t file_id, const std::vector<ResumeEntry> &entries,
    std::uint64_t valid_offset) {
  std::uint64_t max_seq = 0;
  std::uint64_t seq_min = 0;
  std::uint64_t seq_max = 0;
  std::uint64_t tomb = 0;
  std::uint64_t mark = 0;
  std::uint64_t cm = 0;
  for (const auto &e : entries) {
    const std::span<const std::byte> key_span{e.key};
    if (e.sequence > max_seq) max_seq = e.sequence;
    if (seq_min == 0) seq_min = e.sequence;  // entries are in file order
    if (e.sequence > seq_max) seq_max = e.sequence;
    // entries holds every committed entry in the file, so the file's
    // tombstones are rebuilt from scratch here, like its bounds.
    tomb += tombstone_size(e.entry_type, e.key.size(), e.range_end.size());
    mark += marker_size(e.entry_type);
    cm += change_marker_size(e.entry_type);

    switch (e.entry_type) {
    case EntryType::Put: {
      const auto existing = kd_get(key_dir_, key_span, kd_ctx());
      if (!existing || existing->sequence() < e.sequence) {
        if (existing) {
          const auto dec = entry_size(key_span.size(), existing->value_size());
          file_stats_.patch(existing->file_id(), {.live_removed = dec});
        }
        const auto inc = entry_size(key_span.size(), e.value_size);
        file_stats_.patch(file_id, {.live_added = inc});
        (void)kd_put(key_dir_, key_span,
                     KeyDirEntry::make(e.sequence, e.file_offset, file_id,
                                       e.value_size),
                     kd_ctx());
      }
      break;
    }
    case EntryType::Delete: {
      const auto existing = kd_get(key_dir_, key_span, kd_ctx());
      if (existing && existing->sequence() < e.sequence) {
        const auto dec = entry_size(key_span.size(), existing->value_size());
        file_stats_.patch(existing->file_id(), {.live_removed = dec});
        (void)kd_erase(key_dir_, key_span, kd_ctx());
      }
      break;
    }
    case EntryType::RangeDel: {
      // Same suppression rule recovery applies when it replays a range
      // tombstone from a hint file: erase every key in [from, to) carrying a
      // lower sequence. Skipping it here would leave the resumed key
      // directory holding keys a fresh open would not — the one divergence
      // resume() exists to prevent.
      const Key end{std::span<const std::byte>{e.range_end}};
      std::vector<Key> to_erase;
      for (auto it = kd_lower_bound(key_dir_, key_span, kd_ctx());
           it != std::default_sentinel; ++it) {
        auto [k, entry] = *it;
        if (Key{k} >= end) break;
        if (entry.sequence() >= e.sequence) continue;
        const auto dec = entry_size(k.size(), entry.value_size());
        file_stats_.patch(entry.file_id(), {.live_removed = dec});
        to_erase.emplace_back(k);
      }
      for (const auto &k : to_erase)
        (void)kd_erase(key_dir_, std::span<const std::byte>{k}, kd_ctx());
      break;
    }
    case EntryType::ChangeMarker:
      // A promotion whose marker reached the file but was never published —
      // set_mode(Leader) failed after its append — is in the list from here.
      // One already published is in it already, so only a marker above the
      // list's last is added.
      // The list is never empty when it exists.
      if (const auto *ms = change_markers_.get();
          !ms || ms->back().since_sequence < e.sequence) {
        append_marker({e.sequence, e.marker_id});
      }
      break;
    case EntryType::BulkBegin:
    case EntryType::BulkEnd:
      // A marker carries no key and no value, so it moves no key directory
      // entry and no live byte. Its sequence is counted above with every
      // other entry's, which is the whole reason the scan collects it: the
      // file's bounds must describe the sequences the file actually holds.
      // An orphaned pair never reaches here — it lies above valid_offset and
      // is truncated rather than replayed.
      break;
    }
  }

  if (max_seq >= next_seq_) {
    next_seq_ = max_seq + 1;
  }

  // apply_resume owns the truncated file's stats, so callers need no
  // mutable file_stats access. Its live bytes moved entry by entry above;
  // its extent, bounds and tombstones are rebuilt from the entries.
  if (auto fs = file_stats_.get(file_id)) {
    fs->total_bytes = valid_offset;
    fs->min_sequence = seq_min;
    fs->max_sequence = seq_max;
    fs->tombstone_bytes = tomb;
    fs->marker_bytes = mark;
    fs->change_marker_bytes = cm;
    file_stats_.set(file_id, *fs);
  }
}

auto TransientEngineState::active_file() -> WritableDataFile & {
  return static_cast<WritableDataFile &>(**files_.get(active_file_id_));
}

auto TransientEngineState::active_file_ptr() const -> std::shared_ptr<DataFile> {
  return *files_.get(active_file_id_);
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

void TransientEngineState::apply_sync(std::uint64_t batch_max_seq) {
  if (batch_max_seq > durable_seq_) {
    durable_seq_ = batch_max_seq;
  }
}

auto TransientEngineState::durable_seq() const noexcept -> std::uint64_t {
  return durable_seq_;
}

void TransientEngineState::note_sync_requested(std::uint64_t seq) {
  sync_requested_seq_ = std::max(sync_requested_seq_, seq);
}

auto TransientEngineState::persistent() && -> std::shared_ptr<EngineState> {
  auto s = std::make_shared<EngineState>(scratch_bytes_);
  s->key_dir = std::move(key_dir_).persistent();
  s->files = std::move(files_).persistent();
  s->file_stats = std::move(file_stats_).persistent();
  s->active_file_id = active_file_id_;
  s->next_seq = next_seq_;
  s->durable_seq = durable_seq_;
  s->sync_requested_seq = sync_requested_seq_;
  s->mode = mode_;
  s->degraded = degraded_;
  s->degraded_reason = std::move(degraded_reason_);
  s->change_markers = std::move(change_markers_);
  s->recent_writes = std::move(recent_writes_);
  return s;
}

} // namespace bytecask
