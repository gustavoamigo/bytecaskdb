// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Generates seed corpus files for the fuzz harnesses.
// Build: xmake build gen_fuzz_corpus
// Run:   ./build/.../gen_fuzz_corpus

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

import bytecask.data_entry;
import bytecask.hint_entry;
import bytecask.types;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

void write_file(const std::filesystem::path &path,
                std::span<const std::byte> data) {
  std::ofstream f{path, std::ios::binary};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  f.write(reinterpret_cast<const char *>(data.data()),
          static_cast<std::streamsize>(data.size()));
}

void write_file(const std::filesystem::path &path,
                const std::vector<std::byte> &data) {
  write_file(path, std::span<const std::byte>{data});
}

// The data harnesses read a control byte before the input (see their
// headers). Bit 0 recomputes CRCs, so a valid seed keeps it set and its
// mutations stay past the CRC check.
constexpr std::byte kFixUpCrc{0x01};
constexpr std::byte kOnDamageStop{0x02};

void write_seed(const std::filesystem::path &path, std::byte control,
                const std::vector<std::byte> &data) {
  std::vector<std::byte> out{control};
  out.insert(out.end(), data.begin(), data.end());
  write_file(path, out);
}

auto concat(std::initializer_list<std::vector<std::byte>> parts)
    -> std::vector<std::byte> {
  std::vector<std::byte> out;
  for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// --- Data entry seeds ---

void gen_data_entry_corpus(const std::filesystem::path &dir) {
  using bytecask::EntryType;
  using bytecask::serialize_entry;
  std::filesystem::create_directories(dir);

  // 1. Put with short key/value
  write_seed(dir / "put_short", kFixUpCrc,
             serialize_entry(1, EntryType::Put, to_bytes("k"), to_bytes("v")));

  // 2. Delete (value_size=0)
  write_seed(dir / "delete", kFixUpCrc,
             serialize_entry(2, EntryType::Delete, to_bytes("k"), {}));

  // 3. Put with empty value
  write_seed(dir / "put_empty_value", kFixUpCrc,
             serialize_entry(3, EntryType::Put, to_bytes("key"), {}));

  // 4. BulkBegin
  write_seed(dir / "bulk_begin", kFixUpCrc,
             serialize_entry(4, EntryType::BulkBegin, {}, {}));

  // 5. BulkEnd
  write_seed(dir / "bulk_end", kFixUpCrc,
             serialize_entry(5, EntryType::BulkEnd, {}, {}));

  // 6. Multi-entry: BulkBegin + Put + Put + BulkEnd
  write_seed(
      dir / "batch", kFixUpCrc,
      concat({serialize_entry(10, EntryType::BulkBegin, {}, {}),
              serialize_entry(11, EntryType::Put, to_bytes("a"),
                              to_bytes("alpha")),
              serialize_entry(12, EntryType::Put, to_bytes("b"),
                              to_bytes("bravo")),
              serialize_entry(13, EntryType::BulkEnd, {}, {})}));
}

// --- Data file scan seeds ---

void gen_data_file_scan_corpus(const std::filesystem::path &dir) {
  using bytecask::EntryType;
  using bytecask::serialize_entry;
  std::filesystem::create_directories(dir);

  const auto put = [](std::uint64_t seq, std::string_view k,
                      std::string_view v) {
    return serialize_entry(seq, EntryType::Put, to_bytes(k), to_bytes(v));
  };
  const auto del = [](std::uint64_t seq, std::string_view k) {
    return serialize_entry(seq, EntryType::Delete, to_bytes(k), {});
  };
  const auto range_del = [](std::uint64_t seq, std::string_view from,
                            std::string_view to) {
    return serialize_entry(seq, EntryType::RangeDel, to_bytes(from),
                           to_bytes(to));
  };
  const auto marker = [](std::uint64_t seq, EntryType type) {
    return serialize_entry(seq, type, {}, {});
  };

  // Standalone entries, a closed batch holding a range tombstone, and a
  // torn tail: the first half of one more entry.
  auto torn = put(9, "torn", "tail");
  torn.resize(torn.size() / 2);
  write_seed(dir / "batch_and_torn_tail", kFixUpCrc,
             concat({put(1, "a", "alpha"), del(2, "b"),
                     marker(3, EntryType::BulkBegin), put(4, "c", "charlie"),
                     range_del(5, "d", "f"), marker(6, EntryType::BulkEnd),
                     torn}));

  // A batch still open at the end of the file: never committed.
  write_seed(dir / "open_batch", kFixUpCrc,
             concat({put(1, "a", "alpha"), marker(2, EntryType::BulkBegin),
                     put(3, "b", "bravo")}));

  // The zero-filled tail an active file is preallocated with.
  write_seed(dir / "zero_tail", kFixUpCrc,
             concat({put(1, "a", "alpha"), put(2, "b", "bravo"),
                     std::vector<std::byte>(64)}));

  // A damaged entry between two intact ones, CRCs left as they are: Throw
  // and Stop.
  auto damaged = put(2, "b", "bravo");
  damaged[bytecask::kHeaderSize] ^= std::byte{0xff};
  const auto with_damage =
      concat({put(1, "a", "alpha"), damaged, put(3, "c", "charlie")});
  write_seed(dir / "damaged_throw", std::byte{0}, with_damage);
  write_seed(dir / "damaged_stop", kOnDamageStop, with_damage);
}

// --- Hint entry seeds ---

void gen_hint_entry_corpus(const std::filesystem::path &dir) {
  using bytecask::EntryType;
  std::filesystem::create_directories(dir);

  // Hint serialize_entry: (sequence, entry_type, file_offset, value_size, key)

  // 1. Single entry
  write_file(dir / "single",
             bytecask::serialize_entry(1, EntryType::Put,
                                       uint64_t{0}, uint32_t{100},
                                       to_bytes("key1")));

  // 2. Two entries with keys that share a prefix
  auto e1 = bytecask::serialize_entry(1, EntryType::Put,
                                       uint64_t{0}, uint32_t{100},
                                       to_bytes("user:alice"));
  auto e2 = bytecask::serialize_entry(2, EntryType::Put,
                                       uint64_t{200}, uint32_t{50},
                                       to_bytes("user:bob"));
  std::vector<std::byte> two_entries;
  two_entries.insert(two_entries.end(), e1.begin(), e1.end());
  two_entries.insert(two_entries.end(), e2.begin(), e2.end());
  write_file(dir / "two_entries", two_entries);

  // 3. Entry with a longer key
  write_file(dir / "long_key",
             bytecask::serialize_entry(3, EntryType::Put,
                                       uint64_t{500}, uint32_t{200},
                                       to_bytes("a_relatively_longer_suffix_key")));

  // 4. Multiple entries with varying keys
  auto h1 = bytecask::serialize_entry(1, EntryType::Put,
                                       uint64_t{0}, uint32_t{10},
                                       to_bytes("stock:widget"));
  auto h2 = bytecask::serialize_entry(2, EntryType::Put,
                                       uint64_t{100}, uint32_t{20},
                                       to_bytes("stock:gadget"));
  auto h3 = bytecask::serialize_entry(3, EntryType::Delete,
                                       uint64_t{200}, uint32_t{0},
                                       to_bytes("stock:gizmo"));
  std::vector<std::byte> multi;
  multi.insert(multi.end(), h1.begin(), h1.end());
  multi.insert(multi.end(), h2.begin(), h2.end());
  multi.insert(multi.end(), h3.begin(), h3.end());
  write_file(dir / "multi_entries", multi);
}

} // namespace

int main() {
  const auto base = std::filesystem::path{"tests/fuzz/seed"};
  gen_data_entry_corpus(base / "data_entry");
  gen_data_file_scan_corpus(base / "data_file_scan");
  gen_hint_entry_corpus(base / "hint_entry");
  return 0;
}
