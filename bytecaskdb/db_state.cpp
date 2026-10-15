// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — engine state publication: the read cache, loading and storing the state

module;
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

module bytecask;

namespace bytecask {

// Returns the engine state from a thread-local cache (read path only).
// The hot path is two plain loads (publishing_, state_gen_; MOVs on x86)
// plus an owner-pointer compare. The entry is refreshed whenever a
// publication is in progress or has completed since it was filled, and
// whenever it holds a different DB instance's state. Returns a
// reference to the thread-local snapshot. The snapshot stays alive until
// the same thread calls load_state_for_read again, so callers must not
// stash the reference across a second load_state_for_read call.
auto DB::read_cache() -> ReadCacheSlot & {
  // Per-thread slot — thread-exit destructor is intentional: it is what
  // leaves the registry.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // TLS: per-DB — entries name their DB, and close() and ~DB take them from
  // every thread's slot, so no later DB at the same address finds one. Tested
  // by "closing a DB releases the state every thread's read cache holds" and
  // "BC-243: thread-local read cache does not leak across DB instances".
  thread_local ReadCacheSlot tl;
#pragma clang diagnostic pop
  return tl;
}

auto DB::load_state_for_read() const
    -> ReadStateGuard {
  auto &slot = read_cache();
  auto *e = slot.claim();
  // owner identifies which DB instance the entry belongs to: without it a
  // thread that reads from two DBs could see one DB's generation while
  // querying the other. An entry of another DB is dropped, not reused —
  // this DB must never take ownership of another DB's state.
  if (e == nullptr || e->owner.load(std::memory_order_relaxed) != this) {
    if (e == nullptr) {
      e = new ReadCacheEntry();
    } else {
      e->state.reset();
    }
    e->owner.store(this, std::memory_order_relaxed);
    e->gen = kNoStateGen;
  }
  // A read must see every state published before it began, and every state
  // another thread has already seen: a marker stored after the state could
  // not tell a reader that a publication it has not seen finish is already
  // visible to others. publishing_ can (see the raw store_state).
  const auto busy = publishing_.load(std::memory_order_acquire);
  const auto gen = state_gen_.load(std::memory_order_acquire);
  if (busy != 0 || gen != e->gen) {
    e->state = load_state();
    e->gen = busy != 0 ? kNoStateGen : gen;
  }
  e->used_epoch.store(ReadCacheRegistry::instance().epoch(),
                      std::memory_order_relaxed);
  if (e->state->closed) {
    slot.release(e);
    throw DbClosed{};
  }
  return ReadStateGuard{slot, e};
}

void DB::scrape_read_caches() const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto period =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          kReadCacheScrapePeriod)
          .count();
  if (now - last_scrape_ns_.load(std::memory_order_relaxed) < period) return;
  last_scrape_ns_.store(now, std::memory_order_relaxed);
  ReadCacheRegistry::instance().scrape(kReadCacheIdleEpochs);
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
  // Durability before visibility: a published state never contains a
  // sync=true write that fdatasync has not confirmed.
  if (new_state->sync_requested_seq > new_state->durable_seq) {
    deem_as_degraded(std::format(
        "invariant violation: publishing sync_requested_seq {} above "
        "durable_seq {}",
        new_state->sync_requested_seq, new_state->durable_seq));
    return;
  }

  const auto durable_advanced =
      new_state->durable_seq > old_state->durable_seq;
  const auto published_advanced =
      new_state->next_seq > old_state->next_seq;
  const auto became_degraded =
      new_state->degraded && !old_state->degraded;
  // close() publishes once; nothing publishes after it.
  const auto became_closed = new_state->closed;

#ifndef NDEBUG
  if constexpr (kKeyDirReadsKeys) {
    // A blind key directory holds locations only: sequences and sizes are
    // in the records, and reading every one on every publish would make
    // each debug commit O(n) in disk reads (and fill the buffer pool from
    // files no reader asked for). What the locations alone show is checked:
    // every entry starts inside its file's committed extent (invariant P).
    for (auto it = kd_value_lower_bound(new_state->key_dir, {},
                                        new_state->kd_ctx(/*verify=*/false));
         it != std::default_sentinel; ++it) {
      const auto loc = *it;
      const auto fs = new_state->file_stats.get(loc.file_id());
      if (!fs) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {}, which has no file_stats",
            loc.file_id()));
        return;
      }
      if (loc.file_offset() >= fs->total_bytes) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {} starts at {} but the "
            "file's committed extent is {}",
            loc.file_id(), loc.file_offset(), fs->total_bytes));
        return;
      }
    }
  } else {
    // Debug-only O(n) checks, sharing one walk: next_seq > max(all key_dir
    // sequences), and every entry inside its file's committed extent
    // (invariant P — see "Why a lent view survives file events" in
    // docs/bytecask_design.md).
    std::uint64_t max_seq = 0;
    for (auto it = kd_begin(new_state->key_dir, new_state->kd_ctx(/*verify=*/false));
         it != std::default_sentinel; ++it) {
      auto [key_span, entry] = *it;
      if (entry.sequence() > max_seq) max_seq = entry.sequence();
      const auto entry_end = entry.file_offset() +
                             entry_size(key_span.size(), entry.value_size());
      const auto fs = new_state->file_stats.get(entry.file_id());
      if (!fs) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {}, which has no file_stats",
            entry.file_id()));
        return;
      }
      if (entry_end > fs->total_bytes) {
        deem_as_degraded(std::format(
            "invariant violation: key in file_id {} ends at {} but the file's "
            "committed extent is {}",
            entry.file_id(), entry_end, fs->total_bytes));
        return;
      }
    }
    // next_seq starts at 1, so an empty key directory passes.
    if (new_state->next_seq <= max_seq) {
      deem_as_degraded(std::format(
          "invariant violation: next_seq {} <= max key_dir sequence {}",
          new_state->next_seq, max_seq));
      return;
    }
  }
#endif

  store_state(std::move(new_state));

  // The version this publish superseded is freed once nothing holds it;
  // an idle thread's read cache is the holder nothing else can reach.
  scrape_read_caches();

  // Wakes the sequence waiters: durable_sequence on a durable advance, and
  // wait_published on any publication, which is why a nosync publish that
  // advances no durable sequence notifies too. (A failed flush publishes
  // through the raw store under durable_mu_ and finish_flush notifies for
  // it; deem_as_degraded notifies for itself.)
  if (durable_advanced || published_advanced || became_degraded ||
      became_closed) {
    { std::lock_guard<std::mutex> lk{durable_mu_}; }
    durable_cv_.notify_all();
  }
}

void DB::store_initial_state(std::shared_ptr<EngineState> s) {
  store_head(s);
  store_state(std::move(s));
}

void DB::validate_state_consistency(const EngineState &s) const {
  // 1. Active file exists in files registry.
  if (!s.files.contains(s.active_file_id)) {
    throw std::runtime_error{std::format(
        "state consistency: active_file_id {} not in files registry",
        s.active_file_id)};
  }

  // 2. Nothing published owes an fdatasync.
  if (s.sync_requested_seq > s.durable_seq) {
    throw std::runtime_error{std::format(
        "state consistency: sync_requested_seq {} > durable_seq {}",
        s.sync_requested_seq, s.durable_seq)};
  }

  // 4. file_stats covers all files.
  for (const auto [file_id, _] : s.files) {
    if (!s.file_stats.contains(file_id)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} missing from file_stats", file_id)};
    }
  }

  // 5. The marker list is in sequence order and below next_seq.
  if (const auto *ms = s.change_markers.get()) {
    std::uint64_t prev = 0;
    for (const auto &m : *ms) {
      if (m.since_sequence <= prev) {
        throw std::runtime_error{std::format(
            "state consistency: change marker at {} out of order after {}",
            m.since_sequence, prev)};
      }
      if (m.since_sequence >= s.next_seq) {
        throw std::runtime_error{std::format(
            "state consistency: change marker at {} at or above next_seq {}",
            m.since_sequence, s.next_seq)};
      }
      prev = m.since_sequence;
    }
  }

  // O(n) key_dir walk: verify dangling file refs, live_bytes, and next_seq.
  // Expensive — only enabled in test builds.
#ifdef BYTECASK_TESTING
  std::map<std::uint32_t, std::uint64_t> computed_live;
  std::uint64_t max_seq = 0;
  for (auto it = kd_begin(s.key_dir, s.kd_ctx(/*verify=*/false));
       it != std::default_sentinel;
       ++it) {
    auto [key_span, entry] = *it;
    if (!s.files.contains(entry.file_id())) {
      throw std::runtime_error{std::format(
          "state consistency: key references file_id {} not in registry",
          entry.file_id())};
    }
    // Offset containment: a published entry must lie inside the committed
    // extent of the file it names. This is what lets resume() shorten the
    // active file under lock-free readers — with use_mmap the mapping stays
    // put and only mmap_end_ moves, so an entry above the new extent would
    // leave a reader's span addressing a page beyond EOF. See "Why a lent
    // view survives file events" in docs/bytecask_design.md.
    // Check 4 put every registered file in file_stats.
    const auto size = entry_size(key_span.size(), entry.value_size());
    const auto entry_end = entry.file_offset() + size;
    if (entry_end > s.file_stats.get(entry.file_id())->total_bytes) {
      throw std::runtime_error{std::format(
          "state consistency: key in file_id {} ends at {} but the file's "
          "committed extent is {}",
          entry.file_id(), entry_end,
          s.file_stats.get(entry.file_id())->total_bytes)};
    }
    computed_live[entry.file_id()] += size;
    if (entry.sequence() > max_seq) max_seq = entry.sequence();
  }

  if (max_seq > 0 && s.next_seq <= max_seq) {
    throw std::runtime_error{std::format(
        "state consistency: next_seq {} <= max key_dir sequence {}",
        s.next_seq, max_seq)};
  }

  for (const auto [file_id, fs] : s.file_stats.all()) {
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
  for (const auto [file_id, fs] : s.file_stats.all()) {
    if ((fs.min_sequence == 0) != (fs.max_sequence == 0)) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} has min_sequence={} max_sequence={} "
          "(one is zero, the other is not)",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
    // Both are zero or neither is (above), so this needs no zero test.
    if (fs.min_sequence > fs.max_sequence) {
      throw std::runtime_error{std::format(
          "state consistency: file_id {} min_sequence {} > max_sequence {}",
          file_id, fs.min_sequence, fs.max_sequence)};
    }
  }
}

} // namespace bytecask
