// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — recovery: rebuilding the engine state from the data and hint
// files at open

module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

module bytecask;

namespace bytecask {

namespace {

// The marker list recovery publishes: sorted by sequence, null when empty.
auto sorted_markers(std::vector<ChangeMarker> markers)
    -> std::shared_ptr<const std::vector<ChangeMarker>> {
  if (markers.empty()) return nullptr;
  std::ranges::sort(markers, {}, &ChangeMarker::since_sequence);
  return std::make_shared<const std::vector<ChangeMarker>>(std::move(markers));
}

// True if every committed entry of `copy` appears in `source`, in order, with
// the same sequence, type, key and value. Reads both files end to end; only
// called for two files whose sequences overlap, which a healthy directory
// never has.
auto is_compaction_of(const std::filesystem::path &copy,
                      const std::filesystem::path &source) -> bool {
  const auto copy_file = openDataFileForRead(copy);
  const auto source_file = openDataFileForRead(source);
  auto src = scan_committed(*source_file);
  auto it = src.begin();
  for (const auto &[entry, entry_off] : scan_committed(*copy_file)) {
    for (;; ++it) {
      if (it == std::default_sentinel) return false;
      const auto &cand = (*it).first;
      if (cand.sequence > entry.sequence) return false;
      if (cand.sequence == entry.sequence &&
          cand.entry_type == entry.entry_type && cand.key == entry.key &&
          cand.value == entry.value) {
        ++it;
        break;
      }
    }
  }
  return true;
}

// The first two files whose sequence ranges overlap, by id. Every file is
// sequence-disjoint from every other (D18) except for what an interrupted
// vacuum leaves, so this is nullopt on every healthy open.
auto find_sequence_overlap(const EngineState &s)
    -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
  struct Range {
    std::uint64_t min, max;
    std::uint32_t file_id;
  };
  std::vector<Range> ranges;
  for (const auto [fid, fs] : s.file_stats.all()) {
    if (fs.min_sequence > 0) {
      ranges.push_back({fs.min_sequence, fs.max_sequence, fid});
    }
  }
  // Sorted by min, a range overlaps an earlier one only if it overlaps the
  // one just before it: until an overlap is found the ranges are disjoint,
  // so the previous range is also the one reaching furthest.
  std::ranges::sort(ranges, {}, &Range::min);
  const Range *prev = nullptr;
  for (const auto &r : ranges) {
    if (prev != nullptr && r.min <= prev->max) {
      return std::pair{prev->file_id, r.file_id};
    }
    prev = &r;
  }
  return std::nullopt;
}

} // namespace

// Recovery, with one repair: the compacted file of an interrupted vacuum is
// deleted and recovery runs again. Vacuum renames its compacted file into
// place, commits, and only then unlinks the source, so a kill in between
// leaves both on disk under the same sequences. Recovery stops on that — by
// SequenceOverlap as soon as two files claim one key under one sequence, or
// by the file ranges check once it is done, for a pair that shares no key —
// and recovery_undo_interrupted_vacuum decides what the pair is. Each pass
// removes one file or throws, so this ends. A healthy open runs one pass and
// pays only for the ranges check, which is over files, not keys. See
// docs/vacuum_crash_recovery_design.md.
auto DB::recovery_open(const Options &opts) -> EngineState {
  for (;;) {
    std::filesystem::path a;
    std::filesystem::path b;
    // A rerun after undoing an interrupted vacuum decides afresh; an empty
    // directory leaves it empty, which keeps every tombstone.
    needed_tombstones_ = {};
    {
      EngineState s;
      auto files = recovery_prepare_files(s);
      // Maps the file ids in a SequenceOverlap back to paths once s is gone.
      const auto registered = s.files;
      const auto path_of = [&](std::uint32_t id) {
        return (*registered.get(id))->path();
      };
      try {
#if defined(BYTECASK_KEYDIR_BLIND)
        s = recovery_load_streams(std::move(s), std::move(files),
                                  opts.recovery_threads,
                                  opts.fail_recovery_on_crc_errors);
#else
        s = recovery_load_ranged(std::move(s), std::move(files),
                                 opts.recovery_threads,
                                 opts.fail_recovery_on_crc_errors);
#endif
        const auto overlap = find_sequence_overlap(s);
        if (!overlap) return s;
        a = path_of(overlap->first);
        b = path_of(overlap->second);
      } catch (const SequenceOverlap &e) {
        a = path_of(e.file_a);
        b = path_of(e.file_b);
      }
    }
    recovery_undo_interrupted_vacuum(a, b);
    // The next pass numbers files afresh, so an id can now name a different
    // file. The pool keys frames by id; a fresh one holds none of the old.
    if (pool_) pool_ = std::make_shared<BufferPool>(opts.buffer_pool);
  }
}

// Two files hold entries under the same sequences. The one shape the engine
// produces is vacuum's: a compacted file C and its source S, both on disk
// because the kill came between the rename and the unlink. Vacuum is
// committed on disk only once S is gone, so C is unfinished work and is
// deleted — as recovery already deletes a leftover .data.tmp. That is safe on
// exactly what is checked here, that every entry of C is also in S: nothing
// is lost. Deleting S instead would also assume S's other entries are dead,
// which nothing here proves. Anything that is not such a pair is corruption.
void DB::recovery_undo_interrupted_vacuum(const std::filesystem::path &a,
                                          const std::filesystem::path &b) {
  // C holds a subset of S, so it is never the larger file. Two files of one
  // size that pass are identical, and either may go.
  const auto a_smaller =
      std::filesystem::file_size(a) <= std::filesystem::file_size(b);
  const auto &copy = a_smaller ? a : b;
  const auto &source = a_smaller ? b : a;
  if (!is_compaction_of(copy, source)) {
    throw std::runtime_error{std::format(
        "bytecask: corrupt database — data files '{}' and '{}' hold entries "
        "under the same sequence numbers, and neither is a compacted copy of "
        "the other",
        a.string(), b.string())};
  }
  std::fprintf(stderr,
               "bytecask: removing '%s', the compacted copy of '%s' left by "
               "an interrupted vacuum\n",
               copy.string().c_str(), source.string().c_str());
  auto hint = copy;
  hint.replace_extension(".hint");
  std::filesystem::remove(hint);
  std::filesystem::remove(copy);
}

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

  // Name order, so file ids are a function of the directory's contents and
  // not of the order the filesystem lists it in.
  std::vector<std::filesystem::path> data_paths;
  for (const auto &dir_entry : std::filesystem::directory_iterator{dir_}) {
    if (dir_entry.path().extension() == ".data") {
      data_paths.push_back(dir_entry.path());
    }
  }
  std::ranges::sort(data_paths);
  // Each data file takes an id, and the new active file one more.
  if (data_paths.size() > KeyDirEntry::kMaxFileId) {
    throw std::runtime_error{std::format(
        "DB::open: '{}' holds {} data files; at most {} can be opened",
        dir_.string(), data_paths.size(), KeyDirEntry::kMaxFileId)};
  }

  std::vector<RecoveredFile> files;
  auto files_t = s.files.transient();

  for (const auto &p : data_paths) {
    const auto file_id = s.next_file_id++;
    const auto hint_path = dir_ / (p.stem().string() + ".hint");
    const auto hintless = !std::filesystem::exists(hint_path);
    // A file without a hint was the active file when the last process
    // stopped, or a sealed file whose hint was not written yet. The active
    // file's last bytes may be in the page cache and not on the device: the
    // process was killed before its sync, or a sync failed and left its
    // pages clean without writing them (#231). A hint built from them would
    // outlive them at a power loss, and point into zeros. So they are made
    // durable before anything reads them — every hint-less file, since none
    // of them says which one was active.
    // No writer says where its entries end, so the whole file.
    if (hintless) rewrite_durably(p, std::filesystem::file_size(p));
    auto data_file =
        openDataFileForRead(p, io_backend_, pool_, file_id);

    if (hintless) {
      // Whatever lies past the file's last committed record goes, once
      // recovery_check_tail has ruled that it may: the preallocated tail a
      // crash leaves, or a torn write. That makes its physical size its
      // logical size, as for every other sealed file — file_size below is
      // what seeds total_bytes for vacuum. The cut is made and synced before
      // the hint is written (#476): every later open trusts a hint and never
      // cuts its file, so a hint over a file whose cut failed, or that a
      // crash interrupted, would keep the tail for good.
      auto cut = false;
      (void)flush_hints_for(data_file, dir_, [&](Offset end) {
        recovery_check_tail(*data_file, end, data_paths);
        if (end < std::filesystem::file_size(p)) {
          truncate_durably(p, end);
          cut = true;
        }
      });
      if (cut) {
        // Same file_id as the open above, deliberately. Under the buffer
        // pool that open's hint scan may have admitted frames under this id,
        // but the truncate only drops a tail: every byte below the end is
        // unchanged, and no reader addresses anything above it. Frames past
        // the new end are orphans CLOCK reclaims.
        data_file.reset();
        data_file = openDataFileForRead(p, io_backend_, pool_, file_id);
      }
    }
    files_t.set(file_id, data_file);

    files.push_back({file_id, std::move(data_file), hint_path,
                     std::filesystem::file_size(p)});
  }

  s.files = std::move(files_t).persistent();
  return files;
}


namespace {

// Whether every byte of file from end on is zero.
auto tail_is_zero(const DataFile &file, Offset end) -> bool {
  if (end >= file.size()) return true;
  std::vector<std::byte> buf(static_cast<std::size_t>(
      std::min<Offset>(Offset{1} << 20, file.size() - end)));
  for (auto off = end; off < file.size();) {
    const auto n = file.read_raw(off, buf);
    if (n == 0) break;
    if (std::ranges::any_of(std::span{buf}.first(n),
                            [](std::byte b) { return b != std::byte{0}; }))
      return false;
    off += n;
  }
  return true;
}

// The sequence in the file's first header, 0 when there is none. Read
// without its CRC: in the file a crash tore, the first record may be the
// torn one. A write that fails or comes back short can land any prefix of
// that record into the zeros the active file was preallocated with, and a
// prefix shorter than the sequence reads as a smaller one: one byte of 2454
// reads as 150 (#471). The type byte follows the sequence, so a header whose
// type is unknown and that has nothing but zeros after the sequence field is
// such a prefix, and has no sequence. An unknown type with anything after it
// is damage: its sequence is read as it stands, so a sealed file keeps the
// refusal below.
auto first_sequence(const DataFile &file) -> std::uint64_t {
  std::array<std::byte, kHeaderSize> hdr{};
  if (file.read_raw(0, hdr) < kHeaderSize) return 0;
  const auto h = read_header(hdr);
  if (!is_known_entry_type(h.entry_type) &&
      tail_is_zero(file, kEntryTypeOffset))
    return 0;
  return h.sequence;
}

} // namespace

// Only the file written last can hold a record a crash tore: every other
// file was fdatasync'd whole before it was sealed. So past a hint-less file's
// last committed record there may be:
// - nothing but zeros: the preallocated tail of the active file. Sealing
//   truncates it and syncs the truncate, so a sealed file carries one only
//   if an earlier build left it. It goes either way.
// - anything else, in the file written last: a torn write, which was never
//   acknowledged durable. It goes, as PostgreSQL and RocksDB truncate their
//   log at the first bad record. Damage there is indistinguishable and goes
//   with it.
// - anything else, in any other file: damage in data that was synced.
//   Refused, in every mode, and the file is left as it was.
// "Written last" is read off sequences, which no two files share: no other
// file may start at a higher one. A file whose first header is zero, or
// whose first write landed only part of its sequence, has no sequence to
// compare. It may be the newest, its first page lost at a power cut and a
// later one kept, so it goes whole.
// Either way it is the one file a crash tore, so no other file may need such
// a cut. The files open handled before this one have their hints by now;
// the rest are scanned here, before anything is cut.
void DB::recovery_check_tail(
    const DataFile &file, Offset end,
    const std::vector<std::filesystem::path> &data_paths) {
  if (tail_is_zero(file, end)) return;
  const auto seq = first_sequence(file);
  for (const auto &other : data_paths) {
    if (other == file.path()) continue;
    const auto other_file = openDataFileForRead(other);
    if (seq != 0 && first_sequence(*other_file) > seq) {
      throw std::runtime_error{std::format(
          "bytecask: corrupt database — data file '{}' does not parse past "
          "offset {}, and '{}' holds later sequences, so it is not the file a "
          "crash could have torn; refusing to truncate it",
          file.path().string(), end, other.string())};
    }
    auto hint = other;
    hint.replace_extension(".hint");
    if (std::filesystem::exists(hint)) continue;
    auto committed = scan_committed(*other_file, 0, OnDamage::Stop);
    auto it = committed.begin();
    while (it != std::default_sentinel) ++it;
    if (!tail_is_zero(*other_file, it.committed_offset())) {
      throw std::runtime_error{std::format(
          "bytecask: corrupt database — data files '{}' and '{}' both hold "
          "data past their last committed record, and a crash can tear only "
          "the file being written; refusing to truncate either",
          file.path().string(), other.string())};
    }
  }
}

// BC_RECOVERY_PHASES=1 prints how long each recovery phase took, so the two
// key directories can be compared phase by phase rather than in total.
namespace {
struct RecoveryPhaseLog {
  bool on{};
  std::chrono::steady_clock::time_point last{};
  explicit RecoveryPhaseLog() {
    const char *e = std::getenv("BC_RECOVERY_PHASES");
    on = e && *e == '1';
    last = std::chrono::steady_clock::now();
  }
  void mark(const char *name) {
    if (!on) return;
    const auto now = std::chrono::steady_clock::now();
    std::fprintf(stderr, "  phase %-22s %7.1f ms\n", name,
                 std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
  }
};
} // namespace

// Collects the tombstones recovery marked as needed into the sorted set
// vacuum consults. `horizon` is the highest sequence recovery saw: nothing
// above it was examined. After a lenient open that skipped a file, the
// horizon is 0 and every tombstone is kept: the skipped file may hold the
// Put a tombstone hides, and may be readable at the next open.
static auto recovery_needed_tombstones(
    std::vector<std::uint64_t> point, const std::vector<RangeTombstone> &ranges,
    std::uint64_t horizon, bool skipped_files) -> NeededTombstones {
  if (skipped_files) return {};
  for (const auto &rt : ranges)
    if (rt.needed) point.push_back(rt.seq);
  std::ranges::sort(point);
  const auto dup = std::ranges::unique(point);
  point.erase(dup.begin(), dup.end());
  return {horizon, std::move(point)};
}

static auto recovery_span_of(const Key &k) noexcept
    -> std::span<const std::byte> {
  return {k.begin(), k.size()};
}

// Runs body(0..n) on n threads. An exception leaving a std::thread is
// std::terminate, so each thread catches its own and the first is rethrown
// here once every thread is joined: a failure in any phase — a range
// merge's SequenceOverlap among them — is an exception from DB::open.
//
// std::thread, not std::jthread: libc++ 21's jthread leaves
// __atomic_unique_lock::__set_locked_bit undefined and the MSan build fails
// to link (llvm/llvm-project#172241).
template <typename Body>
static void recovery_parallel_for(unsigned n, Body &body) {
#ifdef BYTECASK_SINGLE_THREADED
  for (unsigned i = 0; i < n; ++i) body(i);
#else
  if (n <= 1) {
    if (n == 1) body(0);
    return;
  }
  std::vector<std::exception_ptr> errors(n);
  {
    std::vector<std::thread> threads;
    // Joins on every exit, a failed spawn included: destroying a joinable
    // std::thread is std::terminate.
    struct JoinAll {
      std::vector<std::thread> &threads;
      ~JoinAll() {
        for (auto &t : threads) t.join();
      }
    } join_all{threads};
    threads.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
      threads.emplace_back([&body, &errors, i] {
        try {
          body(i);
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    }
  }
  for (const auto &err : errors) {
    if (err) std::rethrow_exception(err);
  }
#endif
}


// Marks the tombstones that one key's hint entries show are still needed.
// `matches` holds one entry per file: that file's newest entry for the key.
// A tombstone is needed when an older Put from another file sits on the key.
// An older Put a file hides under its own newer entry needs nothing more:
// that newer entry is a Put that outranks it, or a Delete in the same file
// that compaction drops together with it.
template <typename EntryOf, typename FileOf>
static void recovery_mark_needed(std::span<const std::byte> key,
                                 std::span<const std::size_t> matches,
                                 EntryOf entry_of, FileOf file_of,
                                 std::vector<RangeTombstone> &ranges,
                                 std::vector<std::uint64_t> &needed) {
  const auto older_put_elsewhere = [&](std::uint64_t seq, std::uint32_t file) {
    for (const auto j : matches) {
      const auto &p = entry_of(j);
      if (p.entry_type == EntryType::Put && p.sequence < seq &&
          file_of(j) != file)
        return true;
    }
    return false;
  };
  for (const auto i : matches) {
    const auto &d = entry_of(i);
    if (d.entry_type == EntryType::Delete &&
        older_put_elsewhere(d.sequence, file_of(i)))
      needed.push_back(d.sequence);
  }
  for (auto &rt : ranges) {
    if (rt.needed) continue;
    if (recovery_key_cmp(key, recovery_span_of(rt.start)) < 0) continue;
    if (recovery_key_cmp(key, recovery_span_of(rt.end)) >= 0) continue;
    if (older_put_elsewhere(rt.seq, rt.file_id)) rt.needed = true;
  }
}

// Builds a RecoveryResult by merging sorted hint runs straight into a bulk
// loader, instead of inserting key by key into a transient.
//
// Inserting key by key costs a descent, a slot shift and a split every
// fanout inserts, per key; per-phase timing put it at 90 of the B+ tree's
// 128 ms at 1M keys and 4 threads, which was the whole of its deficit against
// the radix tree it replaced. A bulk loader writes each key exactly once with
// no descent and no split, but it needs its keys in ascending order — which
// is what a sorted hint file gives.
//
// Requires the sorted hint files flush_hints_for writes. A sorted hint file is
// (batch markers and range tombstones, in scan order) followed by one run of
// Put and Delete entries sorted by key. The merge checks the order it is
// given and throws rather than handing a bulk loader keys it cannot take.
auto DB::recovery_build_sorted(std::span<RecoveredFile> files, bool strict)
    -> RecoveryResult {
  std::uint64_t max_seq = 0;
  std::map<Key, PointTombstone> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::vector<std::uint64_t> needed;
  bool skipped = false;
  std::vector<ChangeMarker> markers;
  std::unordered_map<std::uint32_t, FileStats> fstats_scratch;
  for (const auto &rf : files)
    fstats_scratch.emplace(rf.file_id, FileStats{0, rf.total_bytes});

  auto note = [&](std::uint32_t file_id, const HintEntry &he) {
    const auto seq = he.sequence;
    if (seq > max_seq) max_seq = seq;
    auto &fs = fstats_scratch[file_id];
    if (fs.min_sequence == 0 || seq < fs.min_sequence) fs.min_sequence = seq;
    if (seq > fs.max_sequence) fs.max_sequence = seq;
    fs.tombstone_bytes +=
        tombstone_size(he.entry_type, he.key.size(), he.end_key.size());
    fs.marker_bytes += marker_size(he.entry_type);
    fs.change_marker_bytes += change_marker_size(he.entry_type);
  };

  // One cursor per hint file, parked on its next Put or Delete. A scanner's
  // entries last only until its next call, so the cursor owns the entries it
  // keeps.
  struct Cursor {
    HintFile::Scanner scanner;
    std::uint32_t file_id;
    HintRecord cur;       // best entry for the key it sits on
    HintRecord lookahead; // first entry of the following key
    bool has_cur{false};
    bool has_lookahead{false};
  };
  std::vector<Cursor> cursors;
  cursors.reserve(files.size());

  // Phase A: the head of each file — markers and range tombstones — is read
  // up front, so every range tombstone this worker owns is known before any
  // Put is admitted.
  for (auto &[file_id, data_file, hint_path, tb] : files) {
    // Only a file that cannot be opened is skipped. An error once its entries
    // are being read fails the open: some of them are already noted.
    const auto hint = open_hint_or_skip(data_file, hint_path, strict);
    if (!hint) {
      skipped = true;
      continue;
    }
    Cursor c{hint->make_scanner(), file_id, {}, {}, false, false};
    while (auto he = c.scanner.next()) {
      note(file_id, *he);
      if (he->entry_type == EntryType::RangeDel) {
        range_tombstones.push_back(
            {Key{he->key}, Key{he->end_key}, he->sequence, file_id});
        continue;
      }
      if (he->entry_type == EntryType::BulkBegin ||
          he->entry_type == EntryType::BulkEnd) {
        continue;
      }
      if (he->entry_type == EntryType::ChangeMarker) {
        markers.push_back({he->sequence, he->marker_id});
        continue;
      }
      c.lookahead.assign(*he);
      c.has_lookahead = true;
      break;
    }
    cursors.push_back(std::move(c));
  }

  // Phase B: merge the runs. For each key the highest sequence across every
  // file decides; a Delete that wins drops the key, and every Delete is
  // recorded so other workers' entries for that key are suppressed too.
  KeyDirBulkLoader out;
  std::vector<std::size_t> matches;
  matches.reserve(cursors.size());

  // The next Put or Delete in this file, counting every entry it steps over
  // towards the file's sequence bounds.
  auto next_data = [&](Cursor &c) -> std::optional<HintEntry> {
    while (auto he = c.scanner.next()) {
      note(c.file_id, *he);
      switch (he->entry_type) {
      case EntryType::BulkBegin:
      case EntryType::BulkEnd:
      case EntryType::ChangeMarker:
        continue;  // keyless: nothing to park a cursor on
      case EntryType::RangeDel:
        // Range tombstones only ever sit in the head of a sorted file.
        throw std::runtime_error{
            "bytecask: range tombstone inside a sorted hint run"};
      case EntryType::Put:
      case EntryType::Delete:
        return *he;
      }
    }
    return std::nullopt;
  };

  // Parks the cursor on the highest-sequence entry of its next distinct key.
  // Hints are no longer deduplicated, so one key can repeat within a run;
  // collapsing here is what keeps the merged stream strictly ascending.
  auto load = [&](Cursor &c) {
    if (c.has_lookahead) {
      std::swap(c.cur, c.lookahead);
      c.has_lookahead = false;
    } else if (auto he = next_data(c)) {
      c.cur.assign(*he);
    } else {
      c.has_cur = false;
      return;
    }
    c.has_cur = true;
    while (auto he = next_data(c)) {
      const auto ord = recovery_key_cmp(he->key, c.cur.key);
      if (ord < 0) {
        throw std::runtime_error{
            "bytecask: hint file is not sorted — recovery_build_sorted needs "
            "the sorted hint files flush_hints_for writes"};
      }
      if (ord == 0) {
        if (he->sequence > c.cur.sequence) c.cur.assign(*he);
        continue;
      }
      c.lookahead.assign(*he);
      c.has_lookahead = true;
      break;
    }
  };
  for (auto &c : cursors) load(c);

  // A heap, not a scan over the cursors: there is one cursor per hint file
  // this worker owns, and at one recovery thread that is every file in the
  // database. A linear scan costs O(files) per key, which at 10M keys and
  // ~180 files made recovery 3x slower than the heap; the heap makes
  // it O(log files).
  const auto ahead = [&](std::size_t a, std::size_t b) {
    return recovery_key_cmp(cursors[a].cur.key, cursors[b].cur.key) > 0;
  };
  std::vector<std::size_t> heap;
  heap.reserve(cursors.size());
  for (std::size_t i = 0; i < cursors.size(); ++i)
    if (cursors[i].has_cur) heap.push_back(i);
  std::ranges::make_heap(heap, ahead);

  while (!heap.empty()) {
    std::ranges::pop_heap(heap, ahead);
    const auto best = heap.back();
    heap.pop_back();

    // `key` is the cursor's own copy: valid until that cursor is loaded,
    // which happens only once this key is done.
    const auto key = std::span<const std::byte>{cursors[best].cur.key};
    auto winner_at = best;
    matches.clear();
    matches.push_back(best);
    // Every other cursor sitting on the same key is adjacent at the top.
    while (!heap.empty() &&
           recovery_key_cmp(cursors[heap.front()].cur.key, key) == 0) {
      std::ranges::pop_heap(heap, ahead);
      const auto i = heap.back();
      heap.pop_back();
      matches.push_back(i);
      if (cursors[i].cur.sequence > cursors[winner_at].cur.sequence)
        winner_at = i;
    }
    const auto &winner = cursors[winner_at].cur;
    const auto winner_file = cursors[winner_at].file_id;
    // A Delete that wins its file is recorded even when another file's Put
    // outranks it here, so workers that never saw this key still learn of it.
    // A Delete that loses within its own file cannot matter: the entry that
    // beat it is newer and lives in the same file.
    for (const auto i : matches) {
      if (cursors[i].cur.entry_type != EntryType::Delete) continue;
      auto &slot = tombstones[Key{key}];
      if (cursors[i].cur.sequence > slot.seq)
        slot = {cursors[i].cur.sequence, cursors[i].file_id};
    }
    recovery_mark_needed(
        key, matches,
        [&](std::size_t i) -> const HintRecord & { return cursors[i].cur; },
        [&](std::size_t i) { return cursors[i].file_id; }, range_tombstones,
        needed);

    if (winner.entry_type == EntryType::Put) {
      auto suppressed = false;
      for (const auto &rt : range_tombstones) {
        if (winner.sequence >= rt.seq) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt.start)) < 0) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt.end)) >= 0) continue;
        suppressed = true;
        break;
      }
      // Keys leave the heap ascending: each cursor's are (load checks it)
      // and equal keys are merged above. The bulk loader checks it again.
      if (!suppressed) {
        out.append(key, KeyDirEntry::make(winner.sequence, winner.file_offset,
                                          winner_file, winner.value_size));
      }
    }

    for (const auto i : matches) {
      load(cursors[i]);
      if (cursors[i].has_cur) {
        heap.push_back(i);
        std::ranges::push_heap(heap, ahead);
      }
    }
  }

  auto fstats_t = PersistentU32Table<FileStats>{}.transient();
  for (const auto &[id, fs] : fstats_scratch) fstats_t.set(id, fs);

  return {std::move(out).finish(), std::move(tombstones),
          std::move(range_tombstones), max_seq,
          std::move(fstats_t).persistent(), std::move(needed), skipped,
          std::move(markers)};
}

// ---------------------------------------------------------------------------
// Range-partitioned recovery — the B+ tree path.
//
// Folding W per-worker trees together pairwise would rewrite every surviving
// key once per level of the fan-in: log2(W) passes over the key set, the
// last of them serial. That fold is what stopped recovery scaling past a few
// threads when the radix tree used it.
//
// Here the fold is replaced by a partition. The workers' trees are cut at the
// same splitters, each range is merged by one thread straight into a bulk
// loader, and the results — disjoint and already in order — are concatenated.
// Every key is written exactly once, in parallel, and the serial tail
// rebuilds only the levels above the leaves.
//
// The splitters are free: a B+ tree's inner separators are already a uniform
// one-per-leaf sample of its keys, so each worker offers a few hundred of
// them and the pooled sample is cut into R even parts. Only the *output* has
// to be disjoint — each worker still reads whatever keys its own files held —
// so nothing is shuffled and the phase costs two barriers.
// ---------------------------------------------------------------------------
auto DB::recovery_load_ranged(EngineState s, std::vector<RecoveredFile> files,
                              unsigned recovery_threads, bool strict)
    -> EngineState {
  RecoveryPhaseLog plog;
  if (files.empty()) {
    return s;
  }

#ifdef BYTECASK_SINGLE_THREADED
  const auto W = 1u;
  (void)recovery_threads;
#else
  // Both are at least 1: open rejects recovery_threads = 0, and an empty
  // file list returned above.
  const auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
#endif

  const auto parallel_for = [](unsigned n, auto &&body) {
    recovery_parallel_for(n, body);
  };

  // Phase 1: round-robin file assignment.
  std::vector<std::vector<RecoveredFile>> worker_files(W);
  for (unsigned i = 0; i < files.size(); ++i) {
    worker_files[i % W].push_back(std::move(files[i]));
  }

  // Phase 2: one tree per worker, in parallel.
  std::vector<RecoveryResult> parts(W);
  std::vector<std::exception_ptr> worker_errors(W, nullptr);
  // Hint files are sorted, so a worker merges its files' runs straight into
  // a bulk loader rather than inserting key by key.
  parallel_for(W, [&](unsigned i) {
    try {
      parts[i] = recovery_build_sorted(worker_files[i], strict);
    } catch (...) {
      worker_errors[i] = std::current_exception();
    }
  });
  // In both modes: the lenient skip happens inside the worker
  // (open_hint_or_skip), so what escapes it is an error no mode answers.
  for (const auto &err : worker_errors) {
    if (err) std::rethrow_exception(err);
  }
  bool skipped_files = false;
  plog.mark("build_sorted");

  // Phase 3: union the parts' metadata, and pool their separators into R
  // splitters. Both are O(W × files) or O(W × R) — nothing here touches a key.
  std::map<Key, PointTombstone> tombstones;
  std::vector<RangeTombstone> range_tombstones;
  std::vector<std::uint64_t> needed;
  std::unordered_map<std::uint32_t, FileStats> fstats;
  std::vector<ChangeMarker> change_markers;
  std::uint64_t max_seq = 0;
  for (auto &part : parts) {
    max_seq = std::max(max_seq, part.max_seq);
    change_markers.insert(change_markers.end(), part.change_markers.begin(),
                          part.change_markers.end());
    for (const auto &[key, tomb] : part.tombstones) {
      auto &existing = tombstones[key];
      if (tomb.seq > existing.seq) existing = tomb;
    }
    needed.insert(needed.end(), part.needed_tombstones.begin(),
                  part.needed_tombstones.end());
    skipped_files = skipped_files || part.skipped_files;
    range_tombstones.insert(
        range_tombstones.end(),
        std::make_move_iterator(part.range_tombstones.begin()),
        std::make_move_iterator(part.range_tombstones.end()));
    part.range_tombstones.clear();
    // Files are assigned round-robin, so no two parts report the same one.
    for (const auto [fid, fs] : part.file_stats) fstats.emplace(fid, fs);
  }

  const auto R = W;
  std::vector<std::vector<std::byte>> splitters;
  if (R > 1) {
    // Oversampling smooths over the leaves a worker happens to leave short.
    constexpr std::size_t kOversample = 8;
    std::vector<std::vector<std::byte>> pool;
    for (const auto &part : parts) {
      auto seps = part.key_dir.sample_separators(R * kOversample);
      pool.insert(pool.end(), std::make_move_iterator(seps.begin()),
                  std::make_move_iterator(seps.end()));
    }
    std::sort(pool.begin(), pool.end());
    splitters.reserve(R - 1);
    for (unsigned r = 1; r < R && !pool.empty(); ++r) {
      const auto idx = static_cast<std::size_t>(r) * pool.size() / R;
      if (idx < pool.size()) splitters.push_back(pool[idx]);
    }
    // Repeated splitters would only produce empty ranges.
    splitters.erase(std::unique(splitters.begin(), splitters.end()),
                    splitters.end());
  }
  const auto ranges = static_cast<unsigned>(splitters.size()) + 1;
  plog.mark("union + splitters");

  // Phase 4: one thread per range. Each merges that slice of every part,
  // resolves by sequence, applies the pooled tombstones, and bulk-loads the
  // survivors. live_bytes is accumulated here rather than in a pass of its
  // own — the keys are already in hand.
  struct RangeOut {
    KeyDirLeafRun run;
    std::unordered_map<std::uint32_t, std::uint64_t> live;
    std::vector<std::uint64_t> needed;
    std::vector<std::size_t> needed_ranges; // indexes into range_tombstones
  };
  std::vector<RangeOut> outs(ranges);

  parallel_for(ranges, [&](unsigned r) {
    const auto lo = r == 0 ? std::span<const std::byte>{}
                           : std::span<const std::byte>{splitters[r - 1]};
    const auto *hi = r + 1 < ranges ? &splitters[r] : nullptr;
    const auto hi_span =
        hi ? std::span<const std::byte>{*hi} : std::span<const std::byte>{};

    // Only the range tombstones that reach into this slice can matter.
    std::vector<const RangeTombstone *> rts;
    for (const auto &rt : range_tombstones) {
      const auto start = recovery_span_of(rt.start);
      const auto end = recovery_span_of(rt.end);
      const auto starts_below_hi =
          hi == nullptr || recovery_key_cmp(start, hi_span) < 0;
      const auto ends_above_lo =
          lo.empty() || recovery_key_cmp(end, lo) > 0;
      if (starts_below_hi && ends_above_lo) rts.push_back(&rt);
    }

    // Point tombstones are sorted the way this merge emits keys, so one
    // cursor walks them in lockstep instead of a lookup per key.
    auto tomb = lo.empty() ? tombstones.begin()
                           : tombstones.lower_bound(Key{lo});

    // One cursor per part, each holding the key it currently sits on. The
    // key spans into the iterator's own buffer and stays valid until that
    // iterator advances, so a cursor is only refreshed right after its own
    // increment.
    struct Cursor {
      RecoveryKeyDirIter it;
      std::span<const std::byte> key;
      KeyDirEntry entry{};
      bool live{false};
    };
    auto refresh = [&](Cursor &c) {
      c.live = false;
      if (c.it == std::default_sentinel) return;
      const auto [k, e] = *c.it;
      if (hi != nullptr && recovery_key_cmp(k, hi_span) >= 0) return;
      c.key = k;
      c.entry = e;
      c.live = true;
    };
    std::vector<Cursor> cursors;
    cursors.reserve(parts.size());
    for (const auto &part : parts) {
      cursors.push_back(Cursor{part.key_dir.lower_bound(lo), {}, {}, false});
      refresh(cursors.back());
    }

    KeyDirBulkLoader out;
    auto &live = outs[r].live;
    std::vector<std::size_t> matches;
    matches.reserve(cursors.size());

    // A linear scan picks the smallest key: W comparisons per emitted key,
    // where W is bounded by Options::recovery_threads. A loser tree would
    // make it log2(W), which is only worth the code at thread counts far
    // above a core count.
    while (true) {
      auto best = cursors.size();
      for (std::size_t i = 0; i < cursors.size(); ++i) {
        if (!cursors[i].live) continue;
        if (best == cursors.size() ||
            recovery_key_cmp(cursors[i].key, cursors[best].key) < 0)
          best = i;
      }
      if (best == cursors.size()) break;

      const auto key = cursors[best].key;
      auto winner = cursors[best].entry;
      matches.clear();
      matches.push_back(best);
      for (std::size_t i = 0; i < cursors.size(); ++i) {
        if (i == best || !cursors[i].live) continue;
        if (recovery_key_cmp(cursors[i].key, key) != 0) continue;
        matches.push_back(i);
        if (kde_newer(cursors[i].entry, winner)) winner = cursors[i].entry;
      }

      while (tomb != tombstones.end() &&
             recovery_key_cmp(recovery_span_of(tomb->first), key) < 0)
        ++tomb;
      const auto on_key =
          tomb != tombstones.end() &&
          recovery_key_cmp(recovery_span_of(tomb->first), key) == 0;

      // Each part offers its own newest Put for the key; the tombstones
      // come from every part. One that is newer than a Put from another
      // file is needed. Within a part, recovery_build_sorted already marked
      // what it saw.
      for (const auto i : matches) {
        const auto &e = cursors[i].entry;
        // recovery_build_sorted reduces each file to its newest entry for a
        // key, so a file offers a Put or a tombstone for it, never both: a
        // Put older than the tombstone is from another file.
        if (on_key && e.sequence() < tomb->second.seq)
          outs[r].needed.push_back(tomb->second.seq);
        for (const auto *rt : rts) {
          if (e.sequence() >= rt->seq || e.file_id() == rt->file_id) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->start)) < 0) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->end)) >= 0) continue;
          outs[r].needed_ranges.push_back(
              static_cast<std::size_t>(rt - range_tombstones.data()));
        }
      }

      auto drop = on_key && winner.sequence() < tomb->second.seq;
      if (!drop) {
        for (const auto *rt : rts) {
          if (winner.sequence() >= rt->seq) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->start)) < 0) continue;
          if (recovery_key_cmp(key, recovery_span_of(rt->end)) >= 0) continue;
          drop = true;
          break;
        }
      }
      if (!drop) {
        out.append(key, winner);
        live[winner.file_id()] += entry_size(key.size(), winner.value_size());
      }

      // `key` points into cursors[best]'s buffer — nothing advances until here.
      for (const auto i : matches) {
        ++cursors[i].it;
        refresh(cursors[i]);
      }
    }

    outs[r].run = std::move(out).seal();
  });

  plog.mark("range merge");

  // Phase 5: concatenate the range-disjoint slices and fold in live_bytes.
  std::vector<KeyDirLeafRun> runs;
  runs.reserve(outs.size());
  std::unordered_map<std::uint32_t, std::uint64_t> live_accum;
  for (auto &o : outs) {
    for (const auto &[fid, bytes] : o.live) live_accum[fid] += bytes;
    runs.push_back(std::move(o.run));
  }
  auto key_dir = KeyDirBulkLoader::concat(std::move(runs));
  plog.mark("concat");

  auto fstats_t = PersistentU32Table<FileStats>{}.transient();
  for (auto &[fid, fs] : fstats) {
    const auto it = live_accum.find(fid);
    fs.live_bytes = it != live_accum.end() ? it->second : 0ULL;
    fstats_t.set(fid, fs);
  }

  for (auto &o : outs) {
    needed.insert(needed.end(), o.needed.begin(), o.needed.end());
    for (const auto idx : o.needed_ranges) range_tombstones[idx].needed = true;
  }
  needed_tombstones_ = recovery_needed_tombstones(
      std::move(needed), range_tombstones, max_seq, skipped_files);

  s.key_dir = key_dir_from_recovered(std::move(key_dir));
  s.next_seq = max_seq + 1;
  s.file_stats = FileStatsMap{std::move(fstats_t).persistent()};
  s.change_markers = sorted_markers(std::move(change_markers));
  return s;
}

#ifdef BYTECASK_KEYDIR_BLIND
// ---------------------------------------------------------------------------
// Hint-stream recovery — the blind key directory's path.
//
// Two blind trees cannot be merged without reading their keys back from the
// data files, so this path never builds one per worker. It merges the hint
// files themselves, which are sorted and carry every key:
//
//   1. Per file, in parallel: open it (rebuilding a damaged hint), collect its
//      range tombstones and sequence bounds, and record a fence — the key and
//      position of an entry — at the start of every frame and every 4 KiB
//      inside one. A fence sits only on the first entry of a key, so seeking
//      to one never skips an older duplicate of it.
//   2. Pool the fence keys and cut them into one range per thread.
//   3. Per range, in parallel: seek every file to its last fence below the
//      range and k-way merge its slice. The newest entry of a key wins
//      (kde_newer, so two files under one sequence still throw
//      SequenceOverlap); a winning Delete, or a range tombstone newer than
//      the winner, drops the key. Survivors go straight into a blind loader.
//   4. Concatenate the ranges.
//
// Every file is merged in every range, so a Delete in one file and a Put in
// another meet in the same merge: no tombstone map crosses threads.
// ---------------------------------------------------------------------------
auto DB::recovery_load_streams(EngineState s, std::vector<RecoveredFile> files,
                               unsigned recovery_threads, bool strict)
    -> EngineState {
  RecoveryPhaseLog plog;
  if (files.empty()) {
    return s;
  }
#ifdef BYTECASK_SINGLE_THREADED
  const auto W = 1u;
  (void)recovery_threads;
#else
  // Both are at least 1: open rejects recovery_threads = 0, and an empty
  // file list returned above.
  const auto W = std::min(static_cast<unsigned>(files.size()), recovery_threads);
#endif
  constexpr std::size_t kFenceStep = 4096;

  using Position = HintFile::Scanner::Position;
  struct Fence {
    std::vector<std::byte> key; // a copy: the scanner's frame is reused
    Position at;
  };
  struct FileRun {
    std::optional<HintFile> hint; // empty: skipped (lenient open)
    Position data_start;          // where the first Put or Delete starts
    std::vector<Fence> fences;
    std::vector<RangeTombstone> range_tombstones;
    FileStats stats;
    std::uint64_t max_seq{0};
    std::vector<ChangeMarker> markers;
  };
  std::vector<FileRun> runs(files.size());

  // Phase 1: fences, range tombstones and sequence bounds, per file.
  auto scan_file = [&](std::size_t f) {
    auto &run = runs[f];
    const auto &rf = files[f];
    run.stats = FileStats{0, rf.total_bytes};
    run.hint = open_hint_or_skip(rf.data_file, rf.hint_path, strict);
    if (!run.hint) return;
    auto scanner = run.hint->make_scanner();
    bool in_data = false;
    Position next_fence{};
    std::vector<std::byte> prev_key;
    bool have_prev = false;
    for (;;) {
      const auto at = scanner.position();
      const auto he = scanner.next();
      if (!he) break;
      if (run.stats.min_sequence == 0 || he->sequence < run.stats.min_sequence)
        run.stats.min_sequence = he->sequence;
      run.stats.max_sequence = std::max(run.stats.max_sequence, he->sequence);
      run.stats.tombstone_bytes +=
          tombstone_size(he->entry_type, he->key.size(), he->end_key.size());
      run.max_seq = std::max(run.max_seq, he->sequence);
      run.stats.marker_bytes += marker_size(he->entry_type);
      run.stats.change_marker_bytes += change_marker_size(he->entry_type);
      switch (he->entry_type) {
      case EntryType::BulkBegin:
      case EntryType::BulkEnd:
        continue;
      case EntryType::ChangeMarker:
        run.markers.push_back({he->sequence, he->marker_id});
        continue;
      case EntryType::RangeDel:
        // Range tombstones only ever sit in the head of a sorted file.
        if (in_data)
          throw std::runtime_error{
              "bytecask: range tombstone inside a sorted hint run"};
        run.range_tombstones.push_back(
            {Key{he->key}, Key{he->end_key}, he->sequence, rf.file_id});
        continue;
      case EntryType::Put:
      case EntryType::Delete:
        break;
      }
      if (!in_data) {
        in_data = true;
        run.data_start = at;
      }
      const bool new_key =
          !have_prev || recovery_key_cmp(he->key, prev_key) != 0;
      if (!new_key) continue;
      // A new frame always passes: its position sorts after any fence
      // target inside the previous one.
      if (at >= next_fence) {
        run.fences.push_back({{he->key.begin(), he->key.end()}, at});
        next_fence = {at.frame, (at.offset / kFenceStep + 1) * kFenceStep};
      }
      prev_key.assign(he->key.begin(), he->key.end());
      have_prev = true;
    }
    if (!in_data) run.data_start = scanner.position();
  };
  auto scan_worker = [&](unsigned w) {
    for (std::size_t f = w; f < files.size(); f += W) scan_file(f);
  };
  recovery_parallel_for(W, scan_worker);
  plog.mark("scan + fences");

  std::uint64_t max_seq = 0;
  std::vector<RangeTombstone> range_tombstones;
  std::vector<ChangeMarker> change_markers;
  for (auto &run : runs) {
    max_seq = std::max(max_seq, run.max_seq);
    change_markers.insert(change_markers.end(), run.markers.begin(),
                          run.markers.end());
    range_tombstones.insert(
        range_tombstones.end(),
        std::make_move_iterator(run.range_tombstones.begin()),
        std::make_move_iterator(run.range_tombstones.end()));
  }

  // Phase 2: splitters from the pooled fences, which sample every file's keys
  // every 4 KiB of hint — even by bytes, close to even by keys.
  std::vector<std::vector<std::byte>> splitters;
  if (W > 1) {
    std::vector<std::span<const std::byte>> pool;
    for (const auto &run : runs)
      for (const auto &fence : run.fences) pool.push_back(fence.key);
    std::ranges::sort(pool, [](auto a, auto b) {
      return recovery_key_cmp(a, b) < 0;
    });
    for (unsigned r = 1; r < W && !pool.empty(); ++r) {
      const auto k = pool[static_cast<std::size_t>(r) * pool.size() / W];
      if (splitters.empty() ||
          recovery_key_cmp(k, std::span<const std::byte>{splitters.back()}) > 0)
        splitters.emplace_back(k.begin(), k.end());
    }
  }
  const auto ranges = static_cast<unsigned>(splitters.size()) + 1;
  plog.mark("splitters");

  // Phase 3: one k-way merge per range, into blind leaves.
  struct RangeOut {
    btree_detail::LeafRun<BlindRef> run;
    std::unordered_map<std::uint32_t, std::uint64_t> live;
    std::vector<std::uint64_t> needed;
    // This range's own copy, so marking needs no lock; OR-ed after the merge.
    std::vector<RangeTombstone> ranges;
  };
  std::vector<RangeOut> outs(ranges);
  auto merge_range = [&](unsigned r) {
    const auto lo = r == 0 ? std::span<const std::byte>{}
                           : std::span<const std::byte>{splitters[r - 1]};
    const bool bounded = r + 1 < ranges;
    const auto hi = bounded ? std::span<const std::byte>{splitters[r]}
                            : std::span<const std::byte>{};
    auto below_hi = [&](std::span<const std::byte> k) {
      return !bounded || recovery_key_cmp(k, hi) < 0;
    };

    std::vector<const RangeTombstone *> rts;
    for (const auto &rt : range_tombstones) {
      const auto start = recovery_span_of(rt.start);
      const auto end = recovery_span_of(rt.end);
      if (below_hi(start) && (lo.empty() || recovery_key_cmp(end, lo) > 0))
        rts.push_back(&rt);
    }
    auto &mark_ranges = outs[r].ranges;
    for (const auto *rt : rts) mark_ranges.push_back(*rt);

    // A scanner's entries last only until its next call, so the cursor owns
    // the entries it keeps.
    struct Cursor {
      HintFile::Scanner scanner;
      std::uint32_t file_id;
      HintRecord cur;       // newest entry of the key it sits on
      HintRecord lookahead; // first entry of the following key
      bool has_cur{false};
      bool has_lookahead{false};
    };
    std::vector<Cursor> cursors;
    cursors.reserve(files.size());
    auto next_data = [](Cursor &c) -> std::optional<HintEntry> {
      while (auto he = c.scanner.next()) {
        if (he->entry_type == EntryType::Put ||
            he->entry_type == EntryType::Delete)
          return *he;
        if (he->entry_type == EntryType::RangeDel)
          throw std::runtime_error{
              "bytecask: range tombstone inside a sorted hint run"};
      }
      return std::nullopt;
    };
    // Parks the cursor on the newest entry of its next distinct key inside
    // the range, or empties it.
    auto load = [&](Cursor &c) {
      c.has_cur = false;
      if (c.has_lookahead) {
        std::swap(c.cur, c.lookahead);
        c.has_lookahead = false;
      } else if (auto he = next_data(c)) {
        c.cur.assign(*he);
      } else {
        return;
      }
      if (!below_hi(c.cur.key)) return;
      c.has_cur = true;
      while (auto he = next_data(c)) {
        const auto ord = recovery_key_cmp(he->key, c.cur.key);
        if (ord < 0)
          throw std::runtime_error{
              "bytecask: hint file is not sorted — recovery needs the sorted "
              "hint files flush_hints_for writes"};
        if (ord == 0) {
          if (he->sequence > c.cur.sequence) c.cur.assign(*he);
          continue;
        }
        c.lookahead.assign(*he);
        c.has_lookahead = true;
        break;
      }
    };

    for (std::size_t f = 0; f < files.size(); ++f) {
      const auto &run = runs[f];
      if (!run.hint) continue;
      // The last fence below lo: every entry at or above lo lies after it.
      auto start = run.data_start;
      if (!lo.empty()) {
        const auto it = std::ranges::lower_bound(
            run.fences, lo,
            [](std::span<const std::byte> a, std::span<const std::byte> b) {
              return recovery_key_cmp(a, b) < 0;
            },
            &Fence::key);
        if (it != run.fences.begin()) start = std::prev(it)->at;
      }
      Cursor c{run.hint->make_scanner(), files[f].file_id, {}, {}, false, false};
      c.scanner.seek(start);
      // Step over the keys below lo.
      if (!lo.empty()) {
        while (auto he = next_data(c)) {
          if (recovery_key_cmp(he->key, lo) >= 0) {
            c.lookahead.assign(*he);
            c.has_lookahead = true;
            break;
          }
        }
      }
      load(c);
      if (c.has_cur) cursors.push_back(std::move(c));
    }

    const auto ahead = [&](std::size_t a, std::size_t b) {
      return recovery_key_cmp(cursors[a].cur.key, cursors[b].cur.key) > 0;
    };
    std::vector<std::size_t> heap(cursors.size());
    for (std::size_t i = 0; i < cursors.size(); ++i) heap[i] = i;
    std::ranges::make_heap(heap, ahead);

    BlindBulkLoader<kBlindLeafBytes> out{kBlindRecoveryFillMin,
                                         kBlindRecoveryFillMax};
    auto &live = outs[r].live;
    std::vector<std::size_t> matches;
    auto entry_of = [](const Cursor &c) {
      return KeyDirEntry::make(c.cur.sequence, c.cur.file_offset, c.file_id,
                               c.cur.value_size);
    };
    while (!heap.empty()) {
      std::ranges::pop_heap(heap, ahead);
      const auto first = heap.back();
      heap.pop_back();
      // The cursor's own copy: valid until that cursor is loaded, which
      // happens only once this key is done.
      const auto key = std::span<const std::byte>{cursors[first].cur.key};
      auto winner = first;
      auto winner_entry = entry_of(cursors[first]);
      matches.assign(1, first);
      while (!heap.empty() &&
             recovery_key_cmp(cursors[heap.front()].cur.key, key) == 0) {
        std::ranges::pop_heap(heap, ahead);
        const auto i = heap.back();
        heap.pop_back();
        matches.push_back(i);
        const auto e = entry_of(cursors[i]);
        if (kde_newer(e, winner_entry)) {
          winner = i;
          winner_entry = e;
        }
      }

      // Every file is merged here, so this is the whole picture for the key.
      recovery_mark_needed(
          key, matches,
          [&](std::size_t i) -> const HintRecord & { return cursors[i].cur; },
          [&](std::size_t i) { return cursors[i].file_id; }, mark_ranges,
          outs[r].needed);

      auto keep = cursors[winner].cur.entry_type == EntryType::Put;
      for (const auto *rt : rts) {
        if (!keep) break;
        if (winner_entry.sequence() >= rt->seq) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt->start)) < 0) continue;
        if (recovery_key_cmp(key, recovery_span_of(rt->end)) >= 0) continue;
        keep = false;
      }
      if (keep) {
        out.append(key, BlindRef{winner_entry.file_id(),
                                 static_cast<std::uint32_t>(
                                     winner_entry.file_offset())});
        live[winner_entry.file_id()] +=
            entry_size(key.size(), winner_entry.value_size());
      }

      for (const auto i : matches) {
        load(cursors[i]);
        if (cursors[i].has_cur) {
          heap.push_back(i);
          std::ranges::push_heap(heap, ahead);
        }
      }
    }
    outs[r].run = std::move(out).seal();
  };
  recovery_parallel_for(ranges, merge_range);
  plog.mark("range merge");

  // Phase 4: concatenate the ranges and fold in live bytes.
  std::vector<btree_detail::LeafRun<BlindRef>> leaf_runs;
  leaf_runs.reserve(outs.size());
  std::unordered_map<std::uint32_t, std::uint64_t> live_accum;
  std::vector<std::uint64_t> needed;
  for (auto &o : outs) {
    for (const auto &[fid, bytes] : o.live) live_accum[fid] += bytes;
    leaf_runs.push_back(std::move(o.run));
    needed.insert(needed.end(), o.needed.begin(), o.needed.end());
    for (const auto &rt : o.ranges)
      if (rt.needed) needed.push_back(rt.seq);
  }
  const auto skipped_files =
      std::ranges::any_of(runs, [](const FileRun &run) { return !run.hint; });
  needed_tombstones_ =
      recovery_needed_tombstones(std::move(needed), {}, max_seq, skipped_files);
  auto fstats_t = PersistentU32Table<FileStats>{}.transient();
  for (std::size_t f = 0; f < files.size(); ++f) {
    auto fs = runs[f].stats;
    const auto it = live_accum.find(files[f].file_id);
    fs.live_bytes = it != live_accum.end() ? it->second : 0ULL;
    fstats_t.set(files[f].file_id, fs);
  }
  s.key_dir = BlindBulkLoader<kBlindLeafBytes>::concat(std::move(leaf_runs));
  plog.mark("concat");
  s.next_seq = max_seq + 1;
  s.file_stats = FileStatsMap{std::move(fstats_t).persistent()};
  s.change_markers = sorted_markers(std::move(change_markers));
  return s;
}
#endif  // BYTECASK_KEYDIR_BLIND

} // namespace bytecask
