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

// --- Data entry seeds ---

void gen_data_entry_corpus(const std::filesystem::path &dir) {
  using bytecask::EntryType;
  using bytecask::serialize_entry;
  std::filesystem::create_directories(dir);

  // 1. Put with short key/value
  write_file(dir / "put_short",
             serialize_entry(1, EntryType::Put, to_bytes("k"), to_bytes("v")));

  // 2. Delete (value_size=0)
  write_file(dir / "delete",
             serialize_entry(2, EntryType::Delete, to_bytes("k"), {}));

  // 3. Put with empty value
  write_file(dir / "put_empty_value",
             serialize_entry(3, EntryType::Put, to_bytes("key"), {}));

  // 4. BulkBegin
  write_file(dir / "bulk_begin",
             serialize_entry(4, EntryType::BulkBegin, {}, {}));

  // 5. BulkEnd
  write_file(dir / "bulk_end",
             serialize_entry(5, EntryType::BulkEnd, {}, {}));

  // 6. Multi-entry: BulkBegin + Put + Put + BulkEnd
  auto begin = serialize_entry(10, EntryType::BulkBegin, {}, {});
  auto put1 =
      serialize_entry(11, EntryType::Put, to_bytes("a"), to_bytes("alpha"));
  auto put2 =
      serialize_entry(12, EntryType::Put, to_bytes("b"), to_bytes("bravo"));
  auto end = serialize_entry(13, EntryType::BulkEnd, {}, {});
  std::vector<std::byte> batch;
  batch.insert(batch.end(), begin.begin(), begin.end());
  batch.insert(batch.end(), put1.begin(), put1.end());
  batch.insert(batch.end(), put2.begin(), put2.end());
  batch.insert(batch.end(), end.begin(), end.end());
  write_file(dir / "batch", batch);
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
  gen_hint_entry_corpus(base / "hint_entry");
  return 0;
}
