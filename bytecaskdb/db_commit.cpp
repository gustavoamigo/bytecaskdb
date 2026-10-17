// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — the commit path: writer executors, pipeline stage 2, durability waits

module;
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

module bytecask;

namespace bytecask {

// Prepares and applies one slot against the transient. Appends nothing: its
// only I/O is the record reads a key directory that holds no key bytes needs
// to place keys and check guards. Entries are appended to all_entries;
// running_offset is advanced by
// the total byte size of entries produced. Returns false on validation
// failure (slot.result set to nullopt). durable is left false on success;
// execute_slots fills it in once the batch's durability is known.
auto DB::execute_slot(TransientEngineState &t, EngineSlot &slot,
                      std::vector<DataEntryView> &all_entries,
                      std::uint64_t &running_offset, bool can_rotate) -> bool {
  if (slot.plan.empty()) {
    slot.result = CommitResult{};
    return true;
  }

  if (!t.validate_preconditions(slot.plan, slot.conflict_lost_to)) {
    slot.result = std::nullopt;
    slot.conflict_snap_next =
        slot.plan.snap_ ? slot.plan.snap_->state_->next_seq : 0;
    return false;
  }

  auto entries = t.prepare_write(slot.plan);
  if (entries.empty()) {
    slot.result = CommitResult{};
    return true;
  }

  // Both refusals come before apply_writes touches the transient, so the
  // slot fails alone and the rest of the group commits.
  if (entries.back().sequence > KeyDirEntry::kMaxSequence) {
    slot.err = std::make_exception_ptr(std::runtime_error{std::format(
        "sequence space exhausted: the write needs sequences up to {}, "
        "above the limit of {}",
        entries.back().sequence, KeyDirEntry::kMaxSequence)});
    return false;
  }
  if (!can_rotate &&
      running_offset + slot.plan.write_bytes() >= rotation_threshold_) {
    slot.err = std::make_exception_ptr(file_ids_exhausted());
    return false;
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

  const auto committed_sequence = entries.back().sequence;

  all_entries.insert(all_entries.end(),
                     std::make_move_iterator(entries.begin()),
                     std::make_move_iterator(entries.end()));

  slot.result = CommitResult{.sequence = committed_sequence};
  return true;
}

// Executor callback shared by solo_writer_ and write_group_ — stage 1 of
// the commit pipeline: (1) per-slot validate/prepare/apply in-memory,
// collecting entries; (2) one append_entries call for all collected
// entries; then the resulting state becomes the prepared head. No fdatasync
// and no publication here — commit_wait does both, so this batch's phase 1
// and 2 overlap the previous batch's fdatasync. The one exception is a
// batch that crosses the rotation threshold: it quiesces the pipeline and
// runs sync / rotate / publish inline, once per max_file_bytes.
void DB::execute_slots(std::vector<Slot *> &batch) {
  std::lock_guard<std::mutex> wg{*write_mu_};
  // Declared after the lock, so it measures only the time the lock is held.
  struct BusyTimer {
    std::atomic<std::int64_t> &total;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    ~BusyTimer() {
      total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count(),
                      std::memory_order_relaxed);
    }
  } busy{counters_.group_writer_busy_ns};

  counters_.group_writer_batches.fetch_add(1, std::memory_order_relaxed);
  counters_.group_writer_coalesced.fetch_add(
      static_cast<std::int64_t>(batch.size()), std::memory_order_relaxed);

  // Every group starts below max_file_bytes (a group that reaches it
  // rotates), and its first slot holds at most kMaxBatchBytes, so that slot
  // ends inside the 32-bit packed offset. A later slot that would end past
  // it starts the next group, after the rotation. At the default
  // max_file_bytes a group would need gigabytes of writes to split.
  std::span<Slot *> rest{batch};
  while (!rest.empty()) rest = rest.subspan(execute_group(rest));
}

auto DB::execute_group(std::span<Slot *> batch) -> std::size_t {
  // Admission is decided on the published state, not the head: a flush
  // fails without write_mu_ and head_ is only reset by the next barrier,
  // so the head can be non-degraded while the engine is. (Mode is the same
  // in both — set_mode is a barrier.) A batch that loaded head_ just before
  // such a failure still ends in commit_wait with the flush error, its
  // bytes handled by resume() like the failed flush's.
  auto published = load_state();
  if (const auto ex = write_refusal(*published)) {
    for (auto *s : batch) s->err = ex;
    return batch.size();
  }
  auto current = load_head();

  auto t = current->transient();
  auto &file = t.active_file();
  auto initial_offset = static_cast<std::uint64_t>(file.size());
  auto running_offset = initial_offset;
  std::vector<DataEntryView> all_entries;
  auto any_sync = false;
  // Without an id for the next file the active file cannot be rotated, so a
  // write that would take it to the threshold is refused before any I/O.
  const auto can_rotate = t.file_ids_left() > 0;

  // Phase 1: pure in-memory — validate, prepare, pre-compute offsets,
  // apply_writes for each slot sequentially, up to the slot that reaches
  // the rotation threshold.
  // The group ends before a slot that would take the file past the 32-bit
  // packed offset; the first slot always fits (see execute_slots).
  std::size_t taken = 0;
  for (; taken < batch.size(); ++taken) {
    auto &slot = static_cast<EngineSlot &>(*batch[taken]);
    if (taken > 0 &&
        running_offset + slot.plan.write_bytes() > max_group_file_bytes()) {
      break;
    }
    slot.sync = slot.opts.sync;
    execute_slot(t, slot, all_entries, running_offset, can_rotate);
    any_sync |= slot.opts.sync;
  }
  batch = batch.first(taken);

  // A sync slot that appended nothing (an empty, guard-only or no-op plan)
  // still makes the promise every sync=true write makes: on return, every
  // write before it is durable. It waits for the head's last sequence, which
  // is how a caller that writes with sync=false bounds what a crash can lose.
  const auto head_last_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
  auto sync_only_pending = false;
  for (auto *s : batch) {
    auto &slot = static_cast<EngineSlot &>(*s);
    if (slot.opts.sync && slot.result && slot.result->sequence == 0 &&
        head_last_seq > published->durable_seq) {
      slot.sync_through = head_last_seq;
      sync_only_pending = true;
    }
  }

  if (all_entries.empty()) {
    // No entry was appended by any slot: every result is {sequence = 0}.
    // durable is true once nothing earlier is unsynced — at once, or after
    // the flush this head now asks for (commit_wait).
    for (auto *s : batch) {
      auto &slot = static_cast<EngineSlot &>(*s);
      if (slot.result) slot.result->durable = slot.sync_through == 0;
    }
    if (sync_only_pending) {
      t.note_sync_requested(head_last_seq);
      store_head(std::move(t).persistent());
    }
    return batch.size();
  }

  // Highest sequence in this batch — the fdatasync that covers it advances
  // durable_seq to it.
  const auto batch_max_seq = t.next_seq() - 1;

  // Phase 2: one I/O call for all collected entries.
  std::vector<std::uint64_t> io_offsets(all_entries.size());
  try {
    file.append_entries(all_entries, io_offsets, scratch_bytes_);
  } catch (...) {
    auto ex = std::current_exception();
    try { file.sync(); } catch (...) {}
    // Publishing needs the flush role: an in-flight flush must land (or
    // fail) first, so its publication cannot overwrite the degraded state.
    auto role = quiesce();
    auto err_s = load_state()->degraded_copy(std::format(
        "append IO error on '{}': call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(std::move(err_s));
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return batch.size();
  }

  try {
#ifdef BYTECASK_TESTING
    if (test_after_append_) test_after_append_();
#endif
    commit_appended(t, std::move(published), batch, any_sync, batch_max_seq,
                    running_offset - initial_offset);
  } catch (...) {
    // The batch is in the file and nothing published it: what the engine
    // holds no longer matches the file, so writes after it would build on
    // a wrong state. Refused until resume(), which replays the batch.
    const auto ex = std::current_exception();
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    refuse_writes();
  }
  return batch.size();
}

// Everything after a group's entries reach the file: publishing the head,
// or the rotation barrier and the publication after it. Throws only for a
// failure no handler here degrades for, which execute_group turns into a
// refusal: the entries are in the file either way.
void DB::commit_appended(TransientEngineState &t,
                         std::shared_ptr<const EngineState> published,
                         std::span<Slot *> batch, bool any_sync,
                         std::uint64_t batch_max_seq,
                         std::uint64_t appended_bytes) {
  auto &file = t.active_file();
  counters_.bytes_written.fetch_add(static_cast<std::int64_t>(appended_bytes),
                                    std::memory_order_relaxed);
  if (any_sync) t.note_sync_requested(batch_max_seq);

  if (!t.is_rotation_needed(rotation_threshold_)) {
    assert(t.active_file().size() <= rotation_threshold_);
    store_head(std::move(t).persistent());
    // durable is filled in by commit_wait once the flush covering this
    // batch has landed.
    return;
  }

  // Rotation barrier: everything before this batch is flushed and
  // published, and this thread holds the flush role until `role` dies.
  // Two fdatasyncs, once per max_file_bytes: quiesce() flushes the previous
  // head, then this batch is synced before the file is sealed. apply_sync
  // gives t the durable_seq the head chain does not carry; degraded
  // transitions build on the published state.
  auto role = quiesce();
  published = load_state();  // quiesce may have published the previous head
  // The flush quiesce() waited on — this thread's or another writer's — may
  // have failed. t is built on a head the engine has already given up on,
  // so publishing it would clear the degrade without resume() and leave
  // flush_error_ set under a healthy-looking state. The batch fails with
  // that flush's error, like every other write appended behind it; resume()
  // recovers its bytes with theirs.
  if (refused(*published)) {
    std::exception_ptr ex;
    {
      std::lock_guard<std::mutex> lk{durable_mu_};
      ex = flush_error_;
    }
    if (!ex) ex = std::make_exception_ptr(DbDegraded{degraded_reason()});
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }
  try {
    file.sync();
    counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
    t.apply_sync(batch_max_seq);
  } catch (...) {
    auto ex = std::current_exception();
    auto err_s = published->degraded_copy(std::format(
        "rotation fdatasync failed on '{}': bytes in page cache but "
        "durability not confirmed. Call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(std::move(err_s));
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }
  PreparedRotation rotation;
  try {
    rotation = prepare_rotation(t);
  } catch (...) {
    auto ex = std::current_exception();
    t.apply_degrade(std::format(
        "post-write rotation failed for '{}': active file is sealed "
        "but new file could not be created. Call resume() to recover.",
        file.path().string()));
    counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
    store_state(published, std::move(t).persistent());
    for (auto *s : batch) {
      if (!s->err) s->err = ex;
    }
    return;
  }
  finish_rotation(t, std::move(rotation));
  counters_.file_rotations.fetch_add(1, std::memory_order_relaxed);
  counters_.files_opened.fetch_add(1, std::memory_order_relaxed);

  if (any_sync) {
    try {
      file.sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
      t.apply_sync(batch_max_seq);
    } catch (...) {
      auto ex = std::current_exception();
      auto err_s = published->degraded_copy(std::format(
          "commit fdatasync failed on '{}': bytes in page cache but "
          "durability not confirmed. Call resume() to recover.",
          file.path().string()));
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      store_state(std::move(err_s));
      for (auto *s : batch) {
        if (!s->err) s->err = ex;
      }
      return;
    }
  }

  assert(t.active_file().size() <= rotation_threshold_);
  const auto final_durable_seq = t.durable_seq();
  store_state(published, std::move(t).persistent());
  for (auto *s : batch) {
    auto &slot = static_cast<EngineSlot &>(*s);
    if (slot.result) slot.result->durable = final_durable_seq >= slot.result->sequence;
  }
}

void DB::flush_pending() {
  auto head = load_head();
  auto published = load_state();
  if (refused(*published)) return;
  const bool need_sync = head->sync_requested_seq > published->durable_seq;
  // Nothing appended and no sync asked for. A sync-only write appends
  // nothing but asks for one: the entries it covers may all be published
  // already, by flushes that did not sync them.
  if (head->next_seq <= published->next_seq && !need_sync) return;

  if (need_sync) {
#ifdef BYTECASK_TESTING
    if (test_before_flush_sync_) test_before_flush_sync_();
#endif
    commit_delay_.on_sync_start();
    const auto sync_start = std::chrono::steady_clock::now();
    try {
      static_cast<WritableDataFile &>(head->active_file()).sync();
      counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      flush_failed(std::current_exception(), published,
                   head->active_file().path());
      return;
    }
    commit_delay_.on_sync_end(std::chrono::steady_clock::now() - sync_start);
  }

#ifdef BYTECASK_TESTING
  if (test_before_publish_) test_before_publish_();
#endif
  // Publish the head as it stood before the fdatasync. Entries appended
  // since are not claimed: the next flush covers them. O(1): persistent
  // roots are shared, only the integers differ. durable_seq is assigned
  // here; the head's own value is whatever state it was derived from.
  auto pub = std::make_shared<EngineState>(*head);
  pub->durable_seq = need_sync ? head->next_seq - 1 : published->durable_seq;
  store_state(published, std::move(pub));
}

void DB::flush_failed(std::exception_ptr ex,
                      const std::shared_ptr<const EngineState> &published,
                      const std::filesystem::path &active_path) {
  auto err_s = published->degraded_copy(std::format(
      "commit fdatasync failed on '{}': bytes in page cache but "
      "durability not confirmed. Call resume() to recover.",
      active_path.string()));
  counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
  // Published and recorded under one durable_mu_ hold: commit_wait checks
  // flush_error_ under the same mutex, so no waiter can see the degraded
  // state without the error and report DbDegraded where the contract
  // promises the I/O error. (Raw store: the checked store_state takes
  // durable_mu_ itself.)
  std::lock_guard<std::mutex> lk{durable_mu_};
  store_state(std::move(err_s));
  flush_error_ = std::move(ex);
}

void DB::finish_flush() {
  flush_in_flight_.store(false, std::memory_order_release);
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

void DB::flush_once() {
  // Let slots already inside the write group finish stage 1 so this flush
  // covers them. Writers released by the previous flush re-enter within a
  // few microseconds but need a stage-1 batch (validate, apply, pwritev)
  // before they are in the head; a flush that starts the instant the role
  // is won leaves them for the flush after. Measured with 8 zero-think-time
  // writers: 4.15 → 7.78 commits per fdatasync, +64% throughput, same
  // fdatasync rate. A throughput heuristic only, which is why it lives
  // here and not in flush_pending: quiesce() callers hold write_mu_, so a
  // running leader would be blocked on it and busy() could never clear.
  // Bounded so a stalled leader cannot hold the disk; the bound only binds
  // under continuous arrivals, where it is ~8% of a flush.
  //
  // Only a flush that will fdatasync settles: one the head already owes, or
  // one a synced writer still in stage 1 will need. An unsynced flush has
  // no fdatasync to share, and settling would only delay its publication —
  // under continuous unsynced writers, by the whole bound on every flush
  // (docs/write_path_investigation.md: +19% oltp_write_only without it).
  {
    const auto owes_sync = [&] {
      return write_group_.sync_busy() ||
             load_head()->sync_requested_seq > load_state()->durable_seq;
    };
    if (owes_sync()) commit_delay_.wait();
    const auto deadline =
        std::chrono::steady_clock::now() + kFlushSettleMax;
    bool settled = false;
    while (write_group_.busy() && owes_sync()
           && std::chrono::steady_clock::now() < deadline) {
      settled = true;
      std::this_thread::yield();
    }
    if (settled) counters_.flush_settles.fetch_add(1, std::memory_order_relaxed);
  }
  try {
    flush_pending();
  } catch (...) {
    flush_threw();
  }
  finish_flush();
}

void DB::flush_threw() noexcept {
  // The head's entries are in the file and the flush that should have
  // published them did not: writes are refused, and every writer waiting
  // on this flush gets its error, as after a failed fdatasync.
  {
    std::lock_guard<std::mutex> lk{durable_mu_};
    flush_error_ = std::current_exception();
  }
  refuse_writes();
}

auto DB::refused(const EngineState &s) const noexcept -> bool {
  return s.degraded || write_fault_.load(std::memory_order_acquire);
}

void DB::refuse_writes() noexcept {
  write_fault_.store(true, std::memory_order_release);
  try {
#ifdef BYTECASK_TESTING
    if (test_before_refusal_publish_) test_before_refusal_publish_();
#endif
    // A state that is degraded already keeps its own reason.
    if (const auto current = load_state(); !current->degraded) {
      auto s = current->degraded_copy(kWriteFaultReason);
      std::lock_guard<std::mutex> lk{durable_mu_};
      store_state(std::move(s));
    }
  } catch (...) {
    // No memory to publish it: write_fault_ alone refuses until resume().
  }
  { std::lock_guard<std::mutex> lk{durable_mu_}; }
  durable_cv_.notify_all();
}

auto DB::quiesce() -> FlushRole {
  for (;;) {
    if (!flush_in_flight_.exchange(true, std::memory_order_acq_rel)) {
      try {
        flush_pending();
      } catch (...) {
        flush_threw();
        finish_flush();
        throw;
      }
      return FlushRole{*this};
    }
    std::unique_lock<std::mutex> lk{durable_mu_};
    durable_cv_.wait(lk, [&] {
      return !flush_in_flight_.load(std::memory_order_acquire);
    });
  }
}

void DB::commit_wait(EngineSlot &slot) {
  auto &result = *slot.result;
  const auto target =
      result.sequence != 0 ? result.sequence : slot.sync_through;
  const bool want_durable = slot.opts.sync;
  for (;;) {
    auto published = load_state();
    const bool covered = want_durable
        ? published->durable_seq >= target
        : published->next_seq > target;
    if (covered) {
      result.durable = published->durable_seq >= target;
      // Another thread may still be inside the publication that covers this
      // write. Reads that start after this return reload all the same: this
      // thread saw the state, so it happens-after the publisher raised
      // publishing_ (see the raw store_state).
      return;
    }
    {
      std::lock_guard<std::mutex> lk{durable_mu_};
      if (flush_error_) std::rethrow_exception(flush_error_);
    }
    if (refused(*published)) throw DbDegraded{degraded_reason()};

    if (!flush_in_flight_.exchange(true, std::memory_order_acq_rel)) {
      flush_once();
      continue;
    }
    counters_.commit_wait_blocked.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk{durable_mu_};
    // Woken by finish_flush (role released, error recorded) and by
    // store_state when durable_seq advances. The coverage term is for a
    // waiter that loaded `published` before a flush landed and reached the
    // wait after: it must not sleep until the flush after that one.
    durable_cv_.wait(lk, [&] {
      if (!flush_in_flight_.load(std::memory_order_acquire)) return true;
      if (flush_error_ != nullptr) return true;
      auto now = load_state();
      return want_durable ? now->durable_seq >= target
                          : now->next_seq > target;
    });
  }
}

void DB::wait_published(std::uint64_t sequence) const {
  std::unique_lock<std::mutex> lk{durable_mu_};
  durable_cv_.wait(lk, [&] {
#ifdef BYTECASK_TESTING
    if (test_in_sequence_wait_) test_in_sequence_wait_();
#endif
    const auto s = load_state();
    // mcdc-exempt(C3): close() takes the write barrier, which waits for the
    // flush this waiter's sequence is in, so by the time a state is closed
    // that flush has either published the sequence (C1) or degraded (C2).
    // The test stays so a waiter can never outlive the engine.
    return s->next_seq > sequence || refused(*s) || s->closed;
  });
}

auto DB::durable_sequence(std::uint64_t min_sequence,
                         std::chrono::milliseconds timeout) const
    -> std::uint64_t {
  const auto s = load_state();
  if (s->closed) throw DbClosed{};
  if (min_sequence == 0 || s->durable_seq >= min_sequence
      || timeout <= std::chrono::milliseconds{0}) {
    return s->durable_seq;
  }

  const auto reached = [&] {
#ifdef BYTECASK_TESTING
    if (test_in_sequence_wait_) test_in_sequence_wait_();
#endif
    const auto cur = load_state();
    return cur->durable_seq >= min_sequence || cur->closed;
  };
  std::unique_lock<std::mutex> lk{durable_mu_};
  // wait_for adds the timeout to now() in nanoseconds, which overflows for a
  // timeout such as milliseconds::max(). One past what the clock can
  // represent waits with no deadline.
  const auto now = std::chrono::steady_clock::now();
  const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::time_point::max() - now);
  if (timeout >= room) {
    durable_cv_.wait(lk, reached);
  } else {
    durable_cv_.wait_until(lk, now + timeout, reached);
  }
  const auto last = load_state();
  if (last->closed) throw DbClosed{};
  return last->durable_seq;
}

} // namespace bytecask
