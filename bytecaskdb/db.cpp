// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — DB: construction, lifecycle, primary operations, apply_batch entry, mode and stats

module;
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

module bytecask;

namespace bytecask {

// Opens dir, runs recovery, creates initial active data file.
// Throws std::system_error if the directory cannot be prepared.
DB::DB(std::filesystem::path dir, Options opts)
    : dir_{std::move(dir)}, rotation_threshold_{opts.max_file_bytes},
      max_hint_backlog_{opts.max_hint_backlog},
      io_backend_{opts.io_backend},
      size_limits_{opts.max_key_bytes, opts.max_value_bytes},
      scratch_bytes_{opts.thread_scratch_bytes},
      state_{std::make_shared<EngineState>(opts.thread_scratch_bytes)} {
  // A setting above a hard ceiling is refused, not lowered: the engine never
  // runs with a limit other than the one asked for.
  if (opts.max_key_bytes > kMaxKeySize) {
    throw std::invalid_argument{std::format(
        "max_key_bytes = {} exceeds the hard ceiling of {}",
        opts.max_key_bytes, kMaxKeySize)};
  }
  if (opts.max_value_bytes > kMaxValueSize) {
    throw std::invalid_argument{std::format(
        "max_value_bytes = {} exceeds the hard ceiling of {}",
        opts.max_value_bytes, kMaxValueSize)};
  }
  if (opts.max_file_bytes > kMaxFileBytes) {
    throw std::invalid_argument{std::format(
        "max_file_bytes = {} exceeds the hard ceiling of {}",
        opts.max_file_bytes, kMaxFileBytes)};
  }
  if (opts.thread_scratch_bytes < kMinThreadScratchBytes) {
    throw std::invalid_argument{std::format(
        "thread_scratch_bytes = {} is below the minimum of {}",
        opts.thread_scratch_bytes, kMinThreadScratchBytes)};
  }
  if (opts.recovery_threads == 0) {
    throw std::invalid_argument{
        "recovery_threads = 0: recovery needs at least one thread"};
  }
#ifdef __EMSCRIPTEN__
  if (opts.io_backend == IoBackend::Mmap) {
    throw std::invalid_argument{
        "IoBackend::Mmap is not supported on WASM/Emscripten builds: "
        "mmap emulation would double-buffer the data file into the WASM "
        "heap instead of avoiding a copy, so this build always uses the "
        "pread-based data file regardless of this option"};
  }
#endif
  if (opts.io_backend == IoBackend::BufferPool) {
    // The active file is pinned in the pool by a later phase; until then the
    // floor only has to leave room for a working set beyond one file. Rejecting
    // rather than degrading keeps the configured bound meaningful.
    if (opts.buffer_pool.capacity_bytes < 2 * opts.max_file_bytes) {
      throw std::invalid_argument{std::format(
          "IoBackend::BufferPool: buffer_pool.capacity_bytes = {} must be at "
          "least 2 x max_file_bytes = {}. Raise the pool, or lower "
          "max_file_bytes — the two are coupled.",
          opts.buffer_pool.capacity_bytes, 2 * opts.max_file_bytes)};
    }
    pool_ = std::make_shared<BufferPool>(opts.buffer_pool, scratch_bytes_);
  }
  // The directories open creates, each named by an entry in its parent that
  // has to be durable before the database's files can be. A directory that
  // already exists may have been created by an open that failed or was killed
  // before its sync, so the database directory's entry is synced every open.
  std::vector<std::filesystem::path> created;
  for (auto p = std::filesystem::absolute(dir_); !std::filesystem::exists(p);
       p = p.parent_path()) {
    created.push_back(p);
  }
  std::filesystem::create_directories(dir_);
  for (const auto &p : created)
    sync_directory(p.parent_path(), "io_dir_sync_create_dir");
  if (created.empty()) {
    sync_directory(std::filesystem::absolute(dir_).parent_path(),
                   "io_dir_sync_create_dir");
  }

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
    const auto recovery_start = std::chrono::steady_clock::now();
    auto s = recovery_open(opts);
    const auto recovery_end = std::chrono::steady_clock::now();
    counters_.recovery_duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            recovery_end - recovery_start)
            .count();
    // Count recovered files and keys.
    std::int64_t file_count = 0;
    for (auto it = s.files.begin(); it != std::default_sentinel; ++it)
      ++file_count;
    counters_.recovery_files = file_count;
    counters_.recovery_keys =
        static_cast<std::int64_t>(s.key_dir.size());
    // Count files opened during recovery.
    counters_.files_opened.store(file_count, std::memory_order_relaxed);
    s.active_file_id = s.next_file_id++;
    auto new_active = create_active_file(s.active_file_id, "io_dir_sync_open");
    if (pool_) pool_->set_active_file(s.active_file_id);
    // +1 for the new active file.
    counters_.files_opened.fetch_add(1, std::memory_order_relaxed);
    auto files_t = s.files.transient();
    files_t.set(s.active_file_id, new_active);
    s.files = std::move(files_t).persistent();
    auto fstats_t = s.file_stats.transient();
    fstats_t.set(s.active_file_id, FileStats{});
    s.file_stats = std::move(fstats_t).persistent();
    auto initial = std::make_shared<EngineState>(std::move(s));
    // Every recovered entry is durable: sealed files were synced whole
    // before they were sealed, and recovery_prepare_files rewrote and synced
    // every hint-less one before reading it.
    initial->durable_seq =
        initial->next_seq > 0 ? initial->next_seq - 1 : 0;
    initial->mode = opts.initial_mode;
#ifdef BYTECASK_TESTING
    if (test_before_validate_) test_before_validate_(*initial);
#endif
    validate_state_consistency(*initial);
    store_initial_state(std::move(initial));
  } catch (...) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw;
  }
}

DB::~DB() {
  try {
    close();
  } catch (...) {}
  // close() took this DB's read-cache entries, but a read on another thread
  // racing an explicit close() is not ordered after it and can cache one
  // again. Every call on this DB has returned by now, so this pass is the
  // last word.
  try {
    ReadCacheRegistry::instance().release(this);
  } catch (...) {}
}

// Takes vacuum_mu_ before write_mu_, the order vacuum() takes them in. The
// barrier publishes every write already appended, and admits no new one
// before the closed state, so the state it reads holds every write the DB
// will ever acknowledge.
void DB::close() {
  std::lock_guard<std::mutex> vg{*vacuum_mu_};
  std::exception_ptr error;
  {
    WriteBarrier barrier{*this};
    auto current = load_state_for_write();
    if (current->closed) return;
    auto t = current->transient();
    const auto last_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
    if (refused(*current)) {
      // No sync here can be trusted: one that failed earlier left pages
      // clean that a later one does not write (#231). What was durable when
      // the engine degraded is what is durable. The trim is best effort; a
      // degraded active file may already be sealed.
      if (t.durable_seq() < last_seq) {
        error = std::make_exception_ptr(DbDegraded{std::format(
            "close: writes after sequence {} were acknowledged but are not "
            "durable; the engine was degraded: {}",
            t.durable_seq(), current->degraded_reason)});
      }
      try {
        t.active_file().shrink_to_fit();
      } catch (...) {}
    } else {
      try {
        t.active_file().sync();
        counters_.fsyncs.fetch_add(1, std::memory_order_relaxed);
        t.apply_sync(last_seq);
        t.active_file().shrink_to_fit();
        // With every byte of the active file durable, its hint spares the
        // next open the rewrite of a hint-less file. An empty file costs the
        // next open nothing.
        if (t.active_file().size() > 0)
          flush_hints_for(t.active_file_ptr(), dir_);
      } catch (...) {
        counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
        error = std::current_exception();
      }
    }
    // Sealed files were synced whole before they were sealed, so their hints
    // are written whether or not the engine is degraded.
    try {
      flush_hints();
    } catch (...) {
      counters_.io_errors.fetch_add(1, std::memory_order_relaxed);
      if (!error) error = std::current_exception();
    }
    store_state(current, std::move(t).persistent()->closed_copy());
  }
  // The closed state holds no files; the states threads cached do.
  ReadCacheRegistry::instance().release(this);
  if (lock_fd_ != -1) {
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
  if (error) std::rethrow_exception(error);
}

// Writes the value for key into out, reusing its existing capacity to
// amortize allocation across calls. Returns true if the key was found,
// false otherwise.
// Routes the read to the correct data file via KeyDirEntry::file_id.
// Throws std::system_error on I/O failure or std::runtime_error on CRC
// mismatch.
auto DB::get(const ReadOptions &opts, BytesView key,
                   Bytes &out) const -> bool {
  // The guard, not a copy of the state: copying it is a read-modify-write
  // on a control block every reader shares — the line that capped
  // concurrent gets before the read did.
  const auto s = load_state_for_read();
#ifdef BYTECASK_KEYDIR_BLIND
  // The read that confirms the key is the read of the value.
  if (!kd_read_value(s->key_dir, key, s->kd_ctx(opts.verify_checksums), out))
    return false;
  counters_.disk_reads.add(1);
  counters_.disk_read_bytes.add(std::ssize(out));
  return true;
#else
  const auto kv = kd_get(s->key_dir, key, s->kd_ctx());
  if (!kv) {
    return false;
  }
  if (kv->value_size() == 0) {
    out.clear();
    return true;
  }
  ScratchUse io_buf{value_read_buffer(), scratch_bytes_};
  (*s->files.get(kv->file_id()))
      ->read_value(kv->file_offset(), narrow<std::uint16_t>(key.size()),
                   kv->value_size(), opts.verify_checksums, io_buf.buf(), out);
  counters_.disk_reads.add(1);
  counters_.disk_read_bytes.add(static_cast<std::int64_t>(kv->value_size()));
  return true;
#endif
}

// Writes key → value. Overwrites any existing value. Cannot conflict.
// Rotates the active file if it has reached the threshold.
// opts.sync controls whether fdatasync is called after the write.
// Throws std::system_error on I/O failure or lock contention (try_lock).
auto DB::put(const WriteOptions &opts, BytesView key,
            BytesView value) -> CommitResult {
  WritePlan plan{size_limits_};
  plan.put(key, value);
  return *apply_batch(opts, std::move(plan));
}

auto DB::del(const WriteOptions &opts,
            BytesView key) -> std::optional<CommitResult> {
  WritePlan plan{size_limits_};
  plan.ensure_present(key);
  plan.del(key);
  return apply_batch(opts, std::move(plan));
}

auto DB::del_range(const WriteOptions &opts, BytesView from,
                  BytesView to) -> CommitResult {
  WritePlan plan{size_limits_};
  plan.del_range(from, to);
  return *apply_batch(opts, std::move(plan));
}

auto DB::contains_key(const ReadOptions& opts, BytesView key) const -> bool {
  const auto s = load_state_for_read();
  return kd_contains(s->key_dir, key, s->kd_ctx(opts.verify_checksums));
}

auto DB::snapshot() const -> Snapshot {
  return Snapshot{load_state_for_read().state(), size_limits_};
}

// Why a state refuses writes: is_write_allowed() is false.
static auto write_rejection(const EngineState &s) -> std::exception_ptr {
  if (s.closed) return std::make_exception_ptr(DbClosed{});
  if (s.degraded) return std::make_exception_ptr(DbDegraded{s.degraded_reason});
  return std::make_exception_ptr(
      DbFollowerMode{"write rejected: engine is in follower mode"});
}

auto DB::write_refusal(const EngineState &s) const -> std::exception_ptr {
  if (!s.is_write_allowed()) return write_rejection(s);
  if (write_fault_.load(std::memory_order_acquire))
    return std::make_exception_ptr(DbDegraded{kWriteFaultReason});
  return nullptr;
}

// The calling thread's last synced commit: the DB it went to and when it
// returned. The commit delay's round trip is measured from it, per DB, so a
// thread that writes to two databases does not mix their timings.
struct LastSyncedReturn {
  std::uint64_t db{0};  // DB::instance_id_
  std::chrono::steady_clock::time_point at{};
};
// TLS: per-DB — names its DB by instance_id_, not by address, so a DB opened
// where a destroyed one lived does not take its timing. Holds no resources.
// Tested by "commit delay: a DB at a destroyed DB's address does not inherit
// its round trip".
static thread_local LastSyncedReturn last_synced_return{};

// The single write path. Routes to either write_group_ (default) or
// solo_writer_ depending on plan characteristics. put/del/apply_batch are
// thin wrappers that construct a WritePlan and delegate here.
auto DB::apply_batch(WriteOptions opts,
                     WritePlan plan) -> std::optional<CommitResult> {
  if (auto refusal = write_refusal(*load_state())) {
    std::rethrow_exception(refusal);
  }
  if (plan.write_bytes() > max_batch_bytes()) {
    throw std::invalid_argument{std::format(
        "write plan of {} bytes exceeds the limit of {} bytes per write",
        plan.write_bytes(), max_batch_bytes())};
  }
  // With sync, even an empty plan goes through the pipeline: it returns once
  // every earlier write is durable (see execute_slots).
  if (plan.empty() && !opts.sync) {
    return CommitResult{.sequence = 0, .durable = true};
  }

  if (opts.sync && last_synced_return.db == instance_id_)
    commit_delay_.on_round_trip(std::chrono::steady_clock::now() -
                                last_synced_return.at);

  EngineSlot slot;
  slot.plan = std::move(plan);
  slot.opts = opts;
  slot.sync = opts.sync;
#ifdef BYTECASK_TESTING
  if (!test_resolve_by_key_)
#endif
    slot.plan.resolve_snapshot_entries();

  const bool use_solo = opts.solo
      || slot.plan.write_bytes() > kGroupWriteMaxBytes;

  if (use_solo) {
    solo_writer_.submit(slot);
  } else {
    write_group_.submit(slot);
  }
  if (opts.sync) commit_delay_.on_arrival();

  // Stage 1 is done: the slot's entries are in the prepared head and the
  // page cache. Stage 2 — fdatasync (if sync) and publication — happens
  // here, on this thread when the flush role is free.
#ifdef BYTECASK_TESTING
  if (test_before_commit_wait_) test_before_commit_wait_();
#endif
  if (slot.result && (slot.result->sequence != 0 || slot.sync_through != 0)) {
    commit_wait(slot);
  }
  if (opts.sync)
    last_synced_return = {instance_id_, std::chrono::steady_clock::now()};

  settle_unappended(slot);
  return slot.result;
}

auto DB::mode() const noexcept -> Mode {
  return load_state()->mode;
}

auto DB::is_degraded() const noexcept -> bool {
  return refused(*load_state());
}

auto DB::degraded_reason() const -> std::string {
  const auto s = load_state();
  if (s->degraded) return s->degraded_reason;
  if (write_fault_.load(std::memory_order_acquire)) return kWriteFaultReason;
  return {};
}

auto DB::stats() const -> std::map<std::string, std::int64_t> {
  scrape_read_caches();  // see vacuum()
  auto s = load_state();
  if (s->closed) throw DbClosed{};
  const auto *pool = pool_ ? &pool_->counters() : nullptr;
  const auto reclaim = KeyDirTree::reclamation_gauges();
  std::int64_t open_files = 0;
  for (auto it = s->files.begin(); it != std::default_sentinel; ++it)
    ++open_files;
  return {
      // What a pool has to be sized against: the key directory is resident
      // and not a cache. Multiply by the bytes/key your key shape measures
      // (scripts/run_memory_profile.py; about 50 for typical keys).
      {"bytecask.keydir_keys", narrow<std::int64_t>(s->key_dir.size())},
      // Gauges: versions of the key directory alive right now (1 = only the
      // published state) and retired nodes those older versions still pin.
      {"bytecask.keydir_versions_live", narrow<std::int64_t>(reclaim.versions)},
      {"bytecask.keydir_nodes_parked",
       narrow<std::int64_t>(reclaim.parked_nodes)},
      // Gauge, process-wide: freed key directory nodes held for reuse on the
      // node pool's shared list (thread caches not counted).
      {"bytecask.keydir_pool_bytes", btree_detail::node_pool_bytes()},
      {"bytecask.bytes_written",
       counters_.bytes_written.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_batches",
       counters_.group_writer_batches.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_coalesced",
       counters_.group_writer_coalesced.load(std::memory_order_relaxed)},
      {"bytecask.group_writer_busy_us",
       counters_.group_writer_busy_ns.load(std::memory_order_relaxed) / 1000},
      {"bytecask.file_rotations",
       counters_.file_rotations.load(std::memory_order_relaxed)},
      {"bytecask.fsyncs",
       counters_.fsyncs.load(std::memory_order_relaxed)},
      {"bytecask.commit_wait_blocked",
       counters_.commit_wait_blocked.load(std::memory_order_relaxed)},
      {"bytecask.flush_settles",
       counters_.flush_settles.load(std::memory_order_relaxed)},
      {"bytecask.commit_delay_waits", commit_delay_.waits()},
      {"bytecask.commit_delay_us",
       std::chrono::duration_cast<std::chrono::microseconds>(commit_delay_.waited()).count()},
      {"bytecask.commit_delay_fsync_us",
       std::chrono::duration_cast<std::chrono::microseconds>(commit_delay_.fsync_estimate()).count()},
      {"bytecask.commit_delay_round_trip_us",
       std::chrono::duration_cast<std::chrono::microseconds>(commit_delay_.round_trip_estimate()).count()},
      {"bytecask.disk_reads",
       counters_.disk_reads.load()},
      {"bytecask.disk_read_bytes",
       counters_.disk_read_bytes.load()},
      {"bytecask.pool_hits", pool ? pool->hits.load() : 0},
      {"bytecask.pool_misses", pool ? pool->misses.load() : 0},
      {"bytecask.pool_fills",
       pool ? pool->fills.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_evictions",
       pool ? pool->evictions.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_frames_total", pool ? pool->frames_total : 0},
      {"bytecask.pool_frames_resident",
       pool ? pool->frames_resident.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_frames_released",
       pool ? pool->frames_released.load(std::memory_order_relaxed) : 0},
      {"bytecask.pool_direct_io_fallbacks",
       pool ? pool->direct_io_fallbacks.load(std::memory_order_relaxed) : 0},
      {"bytecask.vacuum_bytes_reclaimed",
       counters_.vacuum_bytes_reclaimed.load(std::memory_order_relaxed)},
      {"bytecask.vacuum_files_unlinked",
       counters_.vacuum_files_unlinked.load(std::memory_order_relaxed)},
      {"bytecask.vacuum_tombstones_dropped",
       counters_.vacuum_tombstones_dropped.load(std::memory_order_relaxed)},
      {"bytecask.tombstones_needed",
       std::ssize(needed_tombstones_.sequences)},
      {"bytecask.recovery_files", counters_.recovery_files},
      {"bytecask.recovery_keys", counters_.recovery_keys},
      {"bytecask.recovery_duration_us", counters_.recovery_duration_us},
      {"bytecask.files_opened",
       counters_.files_opened.load(std::memory_order_relaxed)},
      {"bytecask.crc_failures",
       counters_.crc_failures.load(std::memory_order_relaxed)},
      {"bytecask.io_errors",
       counters_.io_errors.load(std::memory_order_relaxed)},
      {"bytecask.degraded_transitions",
       counters_.degraded_transitions.load(std::memory_order_relaxed)},
      {"bytecask.hint_backpressure_stalls",
       counters_.hint_backpressure_stalls.load(std::memory_order_relaxed)},
      {"bytecask.hint_backpressure_stall_us",
       counters_.hint_backpressure_stall_us.load(std::memory_order_relaxed)},
      // changes_since slices that looked the change index up, because their
      // first file holds from_sequence: those that entered it at a bookmark,
      // and those that scanned it from its first byte. A slice starting at a
      // file boundary looks nothing up and counts in neither.
      {"bytecask.change_index_hits",
       change_index_->counters().hits.load(std::memory_order_relaxed)},
      {"bytecask.change_index_misses",
       change_index_->counters().misses.load(std::memory_order_relaxed)},
      // Gauges — current state, not monotonic.
      {"bytecask.degraded", refused(*s) ? 1 : 0},
      // Sealed files whose hint is queued or being written. What close must
      // still write, and what an open after a crash would rebuild.
      {"bytecask.hint_backlog", narrow<std::int64_t>(worker_.pending())},
      {"bytecask.open_files", open_files},
      // Files the change index holds bookmarks for, and the bytes they take.
      {"bytecask.change_index_files",
       narrow<std::int64_t>(change_index_->files())},
      {"bytecask.change_index_bytes",
       narrow<std::int64_t>(change_index_->bytes())},
  };
}

} // namespace bytecask
