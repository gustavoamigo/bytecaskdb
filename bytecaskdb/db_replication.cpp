// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — replication: mode changes, manifests, changes_since and ingest

module;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

module bytecask;

namespace bytecask {

namespace {

// A ChangeMarker's id: 64 random bits, from the entropy source, like the
// file stem's salt. Two promotions anywhere must not share one.
auto random_marker_id() -> std::uint64_t {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
  // TLS: process — an entropy source; no DB data.
  static thread_local std::random_device rd;
#pragma clang diagnostic pop
  return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

} // namespace

void DB::set_mode(Mode mode) {
  WriteBarrier barrier{*this};
  auto current = load_state_for_write();
  if (current->closed) throw DbClosed{};
  if (mode == current->mode) return;
  auto t = current->transient();
  // A leader stepping down makes every write it acknowledged durable, and
  // so shippable: changes_since stops at durable_sequence, and a sync=false
  // write left above it would never reach the next leader, which then
  // reuses its sequence. Only a leader can hold such a write: ingest syncs
  // before it publishes.
  const auto last_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;
  if (mode == Mode::Follower) {
    if (!refused(*current) && t.durable_seq() < last_seq) {
      sync_active_file(t, current, "set_mode(Follower)");
    }
  } else {
    // A promotion. The marker names the history from here on, so it has to
    // be on disk before any write of the new leader is, and the mode must
    // not change without it: a degraded engine cannot write it and stays a
    // follower (#397).
    if (refused(*current)) throw DbDegraded{degraded_reason()};
    const auto seq = t.next_seq();
    // The marker is a write: past the last packable sequence it is refused
    // as a write is, and the engine stays a healthy follower.
    if (seq > KeyDirEntry::kMaxSequence) {
      throw std::runtime_error{std::format(
          "sequence space exhausted: a promotion needs sequence {}, past the "
          "limit of {}",
          seq, KeyDirEntry::kMaxSequence)};
    }
    const auto id = random_marker_id();
    const auto id_bytes = encode_marker_id(id);
    auto &file = t.active_file();
    try {
      std::ignore = file.append_entry(seq, EntryType::ChangeMarker, {},
                                      id_bytes);
    } catch (...) {
      auto ex = std::current_exception();
      try { file.sync(); } catch (...) {}
      degrade_or_refuse([&] {
        store_state(current->degraded_copy(
            "set_mode(Leader) marker append IO error: call resume() to "
            "recover."));
      });
      std::rethrow_exception(ex);
    }
    // From here the marker is in the file. A failure below that no handler
    // degrades for — an allocation, typically — would leave a healthy
    // follower whose next promotion appends a second marker under the same
    // sequence, so writes are refused until resume() reconciles the file, as
    // after any append (#364).
    try {
      t.apply_change_marker(seq, id);
      counters_.bytes_written.fetch_add(
          static_cast<std::int64_t>(
              change_marker_size(EntryType::ChangeMarker)),
          std::memory_order_relaxed);
      // Publishes t degraded, marker included, on failure: the entry is in
      // the file, and resume() finds it there either way.
      sync_active_file(t, current, "set_mode(Leader)");
      if (t.is_rotation_needed(rotation_threshold_) &&
          t.file_ids_left() > 0) {
        PreparedRotation rotation;
        try {
          rotation = prepare_rotation(t);
        } catch (...) {
          degrade_or_refuse([&] {
            t.apply_degrade(
                "set_mode(Leader) rotation file creation failed: call "
                "resume().");
            store_state(current, std::move(t).persistent());
          });
          throw;
        }
        finish_rotation(t, std::move(rotation));
      }
      t.apply_set_mode(mode);
      store_state(current, std::move(t).persistent());
    } catch (...) {
      refuse_writes();
      throw;
    }
    return;
  }
  t.apply_set_mode(mode);
  store_state(current, std::move(t).persistent());
}

auto DB::create_manifest() -> FileManifest {
  std::shared_ptr<const EngineState> manifest_state;
  std::uint64_t through_seq;
  {
    WriteBarrier barrier{*this};

    auto current = load_state_for_write();
    if (current->closed) throw DbClosed{};
    if (refused(*current)) throw DbDegraded{degraded_reason()};

    auto t = current->transient();

    // Make every entry durable before sealing. A failure degrades: the pages
    // it left clean are written by no later fdatasync (#231, #281).
    sync_active_file(t, current, "create_manifest");
    const auto max_seq = t.next_seq() > 0 ? t.next_seq() - 1 : 0;

    // Seal active file, dispatch hint generation, open new active.
    PreparedRotation rotation;
    try {
      rotation = prepare_rotation(t);
    } catch (...) {
      degrade_or_refuse([&] {
        t.apply_degrade(
            "create_manifest rotation failed: active file is sealed "
            "but new file could not be created. Call resume() to recover.");
        store_state(current, std::move(t).persistent());
      });
      throw;
    }
    try {
      finish_rotation(t, std::move(rotation));
    } catch (...) {
      // The active file is sealed on disk and t half rotated: refused, so
      // nothing is appended to the sealed file before resume().
      refuse_writes();
      throw;
    }

    through_seq = max_seq;
    store_state(current, std::move(t).persistent());

    // Capture the published state under write_mu_ and the flush role —
    // nothing can be appended or published in between, so the snapshot
    // has no entries beyond through_sequence.
    manifest_state = load_state();
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

  return FileManifest{Snapshot{manifest_state, size_limits_}, std::move(files), through_seq};
}

// DB::changes_since implementation
auto DB::changes_since(const Snapshot& snap, std::uint64_t from_sequence,
                       std::size_t max_bytes) const -> ChangeBatch {
  if (load_state()->closed) throw DbClosed{};
  auto state = snap.state();
  ChangeHeader header{state->marker_at(from_sequence), from_sequence};
  auto begin = ChangeIterator{state, change_index_, from_sequence,
                              state->durable_seq, max_bytes};
  return {header, {std::move(begin), std::default_sentinel}};
}

auto DB::ingest_chunk(std::span<const DataEntryView> entries,
                      std::uint64_t file_size) const -> IngestChunk {
  auto in_batch = false;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    file_size += entry_size(entries[i].key.size(), entries[i].value.size());
    if (entries[i].entry_type == EntryType::BulkBegin) in_batch = true;
    else if (entries[i].entry_type == EntryType::BulkEnd) in_batch = false;

    if (!in_batch && file_size >= rotation_threshold_ &&
        i + 1 < entries.size()) {
      return {.end = i + 1, .needs_rotation = true, .end_size = file_size};
    }
  }
  return {.end = entries.size(), .needs_rotation = false,
          .end_size = file_size};
}

void DB::ingest(const ChangeHeader &header,
                std::span<const DataEntryView> entries) {
  if (auto s = load_state(); !s->is_ingestion_allowed()) {
    if (s->closed) throw DbClosed{};
    if (s->degraded) throw DbDegraded{s->degraded_reason};
    throw std::logic_error{"ingest rejected: engine is not in follower mode"};
  }
  if (write_fault_.load(std::memory_order_acquire))
    throw DbDegraded{kWriteFaultReason};

  // An atomic batch is written to one file like a plan, so it is held to the
  // same byte limit; with it every entry's offset fits the packed field.
  std::uint64_t batch_bytes = 0;
  auto in_batch = false;
  auto previous = header.from_sequence;
  for (const auto &e : entries) {
    if (e.sequence <= previous) {
      throw std::invalid_argument{std::format(
          "ingest: sequence {} after {}: a slice is strictly increasing "
          "above its header's from_sequence",
          e.sequence, previous)};
    }
    previous = e.sequence;
    // A type byte outside the enum would be appended as given and read back
    // as damage, cutting the follower's newest file at the next open.
    if (!is_known_entry_type(e.entry_type)) {
      throw std::invalid_argument{std::format(
          "ingest: unknown entry type {}",
          static_cast<unsigned>(e.entry_type))};
    }
    if (e.entry_type == EntryType::ChangeMarker) {
      // A marker is a unit of its own, and its value is its id.
      if (in_batch || !e.key.empty() ||
          e.value.size() != kChangeMarkerIdBytes) {
        throw std::invalid_argument{
            "ingest: a change marker inside an atomic batch, or malformed"};
      }
    }
    check_key_size(e.key.size(), size_limits_.max_key_bytes);
    if (e.entry_type == EntryType::Put) {
      check_value_size(e.value.size(), size_limits_.max_value_bytes);
    }
    if (e.sequence > KeyDirEntry::kMaxSequence) {
      throw std::invalid_argument{std::format(
          "ingest: sequence {} exceeds the limit of {}", e.sequence,
          KeyDirEntry::kMaxSequence)};
    }
    // A batch is whole or nothing: a BulkBegin inside one, or a BulkEnd
    // with none open, is a slice cut inside a batch somewhere else.
    if (e.entry_type == EntryType::BulkBegin) {
      if (in_batch) {
        throw std::invalid_argument{
            "ingest: a BulkBegin inside an atomic batch"};
      }
      in_batch = true;
      batch_bytes = 0;
    }
    if (e.entry_type == EntryType::BulkEnd && !in_batch) {
      throw std::invalid_argument{
          "ingest: a BulkEnd with no atomic batch open"};
    }
    if (in_batch) {
      batch_bytes += entry_size(e.key.size(), e.value.size());
      if (batch_bytes > max_batch_bytes()) {
        throw std::invalid_argument{std::format(
            "ingest: atomic batch exceeds the limit of {} bytes",
            max_batch_bytes())};
      }
    }
    if (e.entry_type == EntryType::BulkEnd) in_batch = false;
  }
  // A slice published as given would expose half a batch, and a rotation
  // after it would seal the BulkBegin into a file with no BulkEnd: recovery
  // then drops entries this follower acknowledged and synced (#188).
  if (in_batch) {
    throw std::invalid_argument{
        "ingest: the slice ends inside an atomic batch; cut slices after a "
        "BulkEnd or a standalone entry"};
  }

  WriteBarrier barrier{*this};

  auto current = load_state_for_write();
  if (!current->is_ingestion_allowed()) {
    if (current->closed) throw DbClosed{};
    if (refused(*current)) throw DbDegraded{degraded_reason()};
    throw std::logic_error{"ingest rejected: engine is not in follower mode"};
  }

  // The slice must continue this node's own history (#397). With P the
  // position and X the lower of P and the slice's last sequence: a slice
  // that starts past P has a hole before it, and the source's marker at X
  // must be this node's, or the two histories diverged at a promotion. X
  // stops at the slice's last sequence because the slice says nothing past
  // it, and at P because the slice is skipped as duplicates up to there.
  // An empty slice compares at from_sequence, which is how a node ahead of
  // its source on another history is found out.
  const auto position = current->durable_seq;
  if (header.from_sequence > position) {
    throw std::invalid_argument{std::format(
        "ingest: the slice starts at {}, past this node's durable sequence "
        "{}: a gap",
        header.from_sequence, position)};
  }
  const auto last =
      entries.empty() ? header.from_sequence : entries.back().sequence;
  const auto at = std::min(position, last);
  auto source = header.marker;
  for (const auto &e : entries) {
    if (e.sequence > at) break;
    if (e.entry_type == EntryType::ChangeMarker)
      source = {e.sequence, decode_marker_id(e.value)};
  }
  if (const auto mine = current->marker_at(at); source != mine) {
    throw DbChangeMarkerMismatch{std::format(
        "ingest: at sequence {} the source's history is marker {{{}, {:#x}}} "
        "and this node's is {{{}, {:#x}}}: they diverged at a promotion; "
        "re-bootstrap this node",
        at, source.since_sequence, source.id, mine.since_sequence, mine.id)};
  }

  auto t = current->transient();

  // Filter: skip already-ingested entries (idempotency).
  auto remaining = entries;
  while (!remaining.empty() &&
         remaining.front().sequence <= current->durable_seq) {
    remaining = remaining.subspan(1);
  }
  if (remaining.empty()) return;

  // Every file the slice will need is counted before anything is written:
  // a rotation that finds no id would fail after its file was sealed.
  {
    std::uint32_t rotations = 0;
    auto size = static_cast<std::uint64_t>(t.active_file().size());
    for (auto rest = remaining; !rest.empty();) {
      const auto chunk = ingest_chunk(rest, size);
      if (chunk.needs_rotation) {
        ++rotations;
        size = 0;
      } else {
        size = chunk.end_size;
      }
      rest = rest.subspan(chunk.end);
    }
    if (size >= rotation_threshold_) ++rotations;
    if (rotations > t.file_ids_left()) throw file_ids_exhausted();
  }

  // Chunk-and-rotate loop: write entries in chunks, rotating between chunks
  // at safe boundaries (never inside BulkBegin..BulkEnd).
  // Once a chunk is in the file, a throw that no handler below degraded
  // for — an allocation, typically — leaves bytes nothing published, and
  // writes are refused until resume() reconciles them.
  auto appended = false;
  try {
    while (!remaining.empty()) {
      auto &file = t.active_file();

      const auto next =
          ingest_chunk(remaining, static_cast<std::uint64_t>(file.size()));
      const auto chunk_end = next.end;
      const auto needs_rotation = next.needs_rotation;

      auto chunk = remaining.subspan(0, chunk_end);

      // Phase 1: compute offsets, apply in-memory state.
      auto file_offset = static_cast<std::uint64_t>(file.size());
      std::vector<std::uint64_t> offsets(chunk.size());
      for (std::size_t i = 0; i < chunk.size(); ++i) {
        offsets[i] = file_offset;
        file_offset += entry_size(chunk[i].key.size(), chunk[i].value.size());
      }

      t.apply_ingest(chunk, offsets);
      auto chunk_max_seq = chunk.back().sequence;

      // Phase 2: writev chunk to active file.
      std::vector<std::uint64_t> io_offsets(chunk.size());
      try {
        file.append_entries(chunk, io_offsets, scratch_bytes_);
      } catch (...) {
        auto ex = std::current_exception();
        try { file.sync(); } catch (...) {}
        degrade_or_refuse([&] {
          store_state(current->degraded_copy(
              "ingest append IO error: call resume() to recover."));
        });
        std::rethrow_exception(ex);
      }
      appended = true;

      // Phase 3: if rotation needed, sync before sealing.
      if (needs_rotation) {
        try {
          file.sync();
          t.apply_sync(chunk_max_seq);
        } catch (...) {
          degrade_or_refuse([&] {
            store_state(current->degraded_copy(
                "ingest rotation fdatasync failed: call resume() to "
                "recover."));
          });
          throw;
        }
        PreparedRotation rotation;
        try {
          rotation = prepare_rotation(t);
        } catch (...) {
          degrade_or_refuse([&] {
            t.apply_degrade(
                "ingest post-rotation file creation failed: call resume().");
            store_state(current, std::move(t).persistent());
          });
          throw;
        }
        finish_rotation(t, std::move(rotation));
      }

      remaining = remaining.subspan(chunk_end);
    }

    // Post-loop rotation: if the last chunk pushed the active file past the
    // threshold, rotate now so the invariant (active file <= threshold) holds.
    if (t.is_rotation_needed(rotation_threshold_)) {
      try {
        t.active_file().sync();
        t.apply_sync(t.next_seq() - 1);
      } catch (...) {
        degrade_or_refuse([&] {
          store_state(current->degraded_copy(
              "ingest rotation fdatasync failed: call resume() to recover."));
        });
        throw;
      }
      PreparedRotation rotation;
      try {
        rotation = prepare_rotation(t);
      } catch (...) {
        degrade_or_refuse([&] {
          t.apply_degrade(
              "ingest post-rotation file creation failed: call resume().");
          store_state(current, std::move(t).persistent());
        });
        throw;
      }
      finish_rotation(t, std::move(rotation));
    }

    // Final sync: ensure last chunk is durable before publishing.
    try {
      t.active_file().sync();
      t.apply_sync(t.next_seq() - 1);
    } catch (...) {
      degrade_or_refuse([&] {
        store_state(current->degraded_copy(
            "ingest fdatasync failed: call resume() to recover."));
      });
      throw;
    }

    assert(t.active_file().size() <= rotation_threshold_);
    store_state(current, std::move(t).persistent());
  } catch (...) {
    if (appended) refuse_writes();
    throw;
  }
}

} // namespace bytecask
