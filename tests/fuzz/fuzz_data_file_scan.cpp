// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// libFuzzer harness for the data file sweep: DataFileIterator, under both
// OnDamage modes, and CommittedEntryIterator on top of it. This is the code
// behind hint generation at open, resume(), vacuum's copy and changes_since.
//
// Input: one control byte, then the bytes of the data file.
//   bit 0  recompute each entry's CRC before the sweep, so mutations reach
//          the size checks, the chunk buffer and the batch state machine
//          instead of stopping at the first CRC mismatch. Left clear, the
//          damage paths stay covered.
//   bit 1  OnDamage::Stop instead of OnDamage::Throw.
//   bits 2-3  the sweep's chunk size, 16 B to 4 KiB. The engine's 1 MiB
//          chunk would hold any input whole, leaving the refill and the
//          growth for an entry larger than a chunk unreached, and costs a
//          1 MiB allocation per sweep.
//
// Properties checked on every input:
//   - the sweep ends or throws std::runtime_error; nothing else escapes;
//   - each yielded entry lies inside the file, after the previous one, and
//     equals deserialize_entry() of its own bytes;
//   - under Stop the sweep never throws;
//   - CommittedEntryIterator yields exactly the raw entries minus a trailing
//     unfinished batch, never part of a batch, and committed_offset() is the
//     end of the last entry it yielded.
//
// Build:  xmake f --sanitizer=fuzzer,address -m debug -y
//         xmake build fuzz_data_file_scan
// Run:    scripts/run_fuzz.sh fuzz_data_file_scan 300

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

import bytecask.batch_iterator;
import bytecask.buffer_pool;
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.serialization;
import bytecask.types;

namespace {

using bytecask::DataEntry;
using bytecask::DataEntryView;
using bytecask::DataFile;
using bytecask::EntryType;
using bytecask::kCrcSize;
using bytecask::kHeaderSize;
using bytecask::Offset;
using bytecask::OnDamage;

// The sweep reads only read_raw() and size(); the point reads are never
// reached, so they abort rather than pretend to work.
class MemoryDataFile final : public DataFile {
public:
  explicit MemoryDataFile(std::span<const std::byte> bytes)
      : DataFile{"fuzz.data"}, bytes_{bytes} {}

  [[nodiscard]] auto read_raw(Offset offset, std::span<std::byte> dst) const
      -> std::size_t override {
    if (offset >= bytes_.size()) return 0;
    const auto n = std::min<std::size_t>(dst.size(), bytes_.size() - offset);
    std::ranges::copy(bytes_.subspan(offset, n), dst.begin());
    return n;
  }

  [[nodiscard]] auto size() const noexcept -> Offset override {
    return bytes_.size();
  }

  void read_value(Offset, std::uint16_t, std::uint32_t, bool,
                  std::vector<std::byte> &,
                  std::vector<std::byte> &) const override {
    std::abort();
  }
  [[nodiscard]] auto read_entry(Offset, std::uint32_t,
                                std::vector<std::byte> &) const
      -> DataEntryView override {
    std::abort();
  }
  [[nodiscard]] auto read_entry_unverified(Offset, std::uint32_t,
                                           std::vector<std::byte> &) const
      -> DataEntryView override {
    std::abort();
  }
  [[nodiscard]] auto lend_record(Offset, std::uint32_t, bool,
                                 std::vector<std::byte> &,
                                 bytecask::FrameLease &) const
      -> DataEntryView override {
    std::abort();
  }

private:
  std::span<const std::byte> bytes_;
};

void check(bool ok) {
  if (!ok) std::abort();
}

auto record_size(std::span<const std::byte> file, std::size_t offset)
    -> std::size_t {
  const auto hdr = bytecask::read_header(file.subspan(offset, kHeaderSize));
  return kHeaderSize + hdr.key_size + hdr.value_size + kCrcSize;
}

// Gives every entry the sweep would frame a valid CRC, walking the same
// boundaries it does.
void fix_up_crcs(std::span<std::byte> file) {
  std::size_t offset = 0;
  while (offset + kHeaderSize <= file.size()) {
    const auto hdr = bytecask::read_header(file.subspan(offset, kHeaderSize));
    if (hdr.sequence == 0) return;
    const auto total = record_size(file, offset);
    if (offset + total > file.size()) return;
    const auto record = file.subspan(offset, total);
    bytecask::Crc32 crc{};
    crc.update(record.first(total - kCrcSize));
    bytecask::ByteWriter w{record.last(kCrcSize)};
    w.put(crc.finalize());
    offset += total;
  }
}

auto same(const DataEntry &a, const DataEntry &b) -> bool {
  return a.sequence == b.sequence && a.entry_type == b.entry_type &&
         a.key == b.key && a.value == b.value;
}

struct Raw {
  std::vector<std::pair<DataEntry, Offset>> entries;
  bool threw{false};
};

struct Sweep {
  OnDamage on_damage;
  std::size_t chunk_bytes;

  [[nodiscard]] auto iterator(const DataFile &file) const
      -> bytecask::DataFileIterator {
    return bytecask::DataFileIterator{file, 0, on_damage, chunk_bytes};
  }
};

auto sweep_raw(const MemoryDataFile &file, std::span<const std::byte> bytes,
               Sweep sweep) -> Raw {
  Raw out;
  Offset prev_end = 0;
  try {
    for (auto it = sweep.iterator(file); it != std::default_sentinel; ++it) {
      const auto &[entry, offset] = *it;
      check(offset == prev_end);
      const auto total = record_size(bytes, offset);
      check(offset + total <= bytes.size());
      check(it.next_offset() == offset + total);
      check(same(entry, bytecask::deserialize_entry(bytes.subspan(offset, total))));
      prev_end = offset + total;
      out.entries.emplace_back(entry, offset);
    }
  } catch (const std::runtime_error &) {
    out.threw = true;
  }
  return out;
}

// What CommittedEntryIterator must yield from the raw entries of a sweep
// that did not throw: everything but a batch still open at the end.
auto committed_model(const std::vector<std::pair<DataEntry, Offset>> &raw)
    -> std::vector<std::pair<DataEntry, Offset>> {
  std::vector<std::pair<DataEntry, Offset>> out;
  std::optional<std::size_t> open_batch;
  for (const auto &e : raw) {
    if (!open_batch && e.first.entry_type == EntryType::BulkBegin) {
      open_batch = out.size();
    } else if (open_batch && e.first.entry_type == EntryType::BulkEnd) {
      open_batch.reset();
    }
    out.push_back(e);
  }
  if (open_batch) out.resize(*open_batch);
  return out;
}

void sweep_committed(const MemoryDataFile &file,
                     std::span<const std::byte> bytes, Sweep sweep,
                     const Raw &raw) {
  std::vector<std::pair<DataEntry, Offset>> got;
  auto threw = false;
  std::optional<Offset> end_offset;
  try {
    // Constructing the iterator parses the first entry, so it can throw.
    bytecask::CommittedEntryIterator it{sweep.iterator(file)};
    for (; it != std::default_sentinel; ++it) {
      got.push_back(*it);
      const auto &[entry, offset] = got.back();
      check(it.committed_offset() >= offset + record_size(bytes, offset));
    }
    end_offset = it.committed_offset();
  } catch (const std::runtime_error &) {
    threw = true;
  }
  check(threw == raw.threw);
  const auto want = committed_model(raw.entries);
  if (threw) {
    // A throw ends the sweep before the batch it was staging is yielded, so
    // what was yielded is a prefix of the model.
    check(got.size() <= want.size());
  } else {
    check(got.size() == want.size());
    const auto end = want.empty() ? Offset{0}
                                  : want.back().second +
                                        record_size(bytes, want.back().second);
    check(end_offset == end);
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    check(got[i].second == want[i].second);
    check(same(got[i].first, want[i].first));
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) return 0;
  const auto control = data[0];
  auto in = std::as_bytes(std::span{data + 1, size - 1});
  std::vector<std::byte> bytes{in.begin(), in.end()};
  if ((control & 1U) != 0) fix_up_crcs(bytes);
  const auto sweep = Sweep{
      .on_damage = (control & 2U) != 0 ? OnDamage::Stop : OnDamage::Throw,
      .chunk_bytes = std::size_t{16} << (2 * ((control >> 2) & 3U)),
  };

  const MemoryDataFile file{bytes};
  const auto raw = sweep_raw(file, bytes, sweep);
  if (sweep.on_damage == OnDamage::Stop) check(!raw.threw);
  sweep_committed(file, bytes, sweep, raw);
  return 0;
}
