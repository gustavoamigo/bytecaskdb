// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — unit tests for hint file writing and recovery parsing

#include <algorithm>
#include <array>
#include <iterator>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
import bytecask.hint_entry;
import bytecask.hint_file;
import bytecask.serialization;
import bytecask.types;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(std::span<const std::byte> bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  std::ranges::transform(bytes, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

auto read_file(const std::filesystem::path &p) -> std::vector<std::byte> {
  std::ifstream f{p, std::ios::binary};
  std::vector<char> chars{std::istreambuf_iterator<char>{f}, {}};
  std::vector<std::byte> out(chars.size());
  std::ranges::transform(chars, out.begin(),
                         [](char c) { return static_cast<std::byte>(c); });
  return out;
}

void write_file(const std::filesystem::path &p,
                std::span<const std::byte> bytes) {
  std::ofstream f{p, std::ios::binary | std::ios::trunc};
  f.write(reinterpret_cast<const char *>(bytes.data()), std::ssize(bytes));
}

auto crc_of(std::span<const std::byte> bytes) -> std::uint32_t {
  bytecask::Crc32 crc{};
  crc.update(bytes);
  return crc.finalize();
}

void put_trailer(std::vector<std::byte> &file, std::uint32_t crc) {
  std::array<std::byte, 4> trailer{};
  bytecask::ByteWriter w{trailer};
  w.put(crc);
  std::ranges::copy(trailer, file.end() - 4);
}

// Every hint file written from here to the end of the scope cuts frames at
// `n` bytes.
struct FrameBytes {
  explicit FrameBytes(std::size_t n) {
    bytecask::set_hint_frame_bytes_for_testing(n);
  }
  ~FrameBytes() { bytecask::set_hint_frame_bytes_for_testing(0); }
  FrameBytes(const FrameBytes &) = delete;
  FrameBytes &operator=(const FrameBytes &) = delete;
};

struct Expected {
  std::uint64_t seq;
  bytecask::EntryType type;
  std::uint64_t offset;
  std::string key;
  std::uint32_t value_size;
  std::string end_key; // RangeDel only
};

auto key_for(std::size_t i) -> std::string {
  return "key:" + std::to_string(i) + std::string(i % 23, 'k');
}

// A head of markers and range tombstones, then a key-sorted run in which some
// keys repeat, as flush_hints_for writes it.
auto sample_entries(std::size_t n) -> std::vector<Expected> {
  using bytecask::EntryType;
  std::vector<Expected> es;
  es.push_back({1, EntryType::BulkBegin, 0, "", 0, ""});
  es.push_back({2, EntryType::RangeDel, 20, "a", 1, "b"});
  es.push_back({3, EntryType::BulkEnd, 40, "", 0, ""});
  for (std::size_t i = 0; i < n; ++i) {
    const auto type = i % 5 == 0 ? EntryType::Delete : EntryType::Put;
    es.push_back({100 + i, type, 1000 + 64 * i, key_for(i),
                  static_cast<std::uint32_t>(i % 300), ""});
    if (i % 7 == 0)
      es.push_back({50 + i, EntryType::Put, 900 + i, key_for(i), 3, ""});
  }
  return es;
}

void write_entries(const std::filesystem::path &p,
                   const std::vector<Expected> &es) {
  auto hf = bytecask::HintFile::OpenForWrite(p);
  for (const auto &e : es) {
    if (e.type == bytecask::EntryType::RangeDel)
      hf.append_range_del(e.seq, e.offset, to_bytes(e.key), to_bytes(e.end_key));
    else
      hf.append(e.seq, e.type, e.offset, to_bytes(e.key), e.value_size);
  }
  hf.close();
}

void check_entry(const bytecask::HintEntry &he, const Expected &e) {
  CHECK(he.sequence == e.seq);
  CHECK(he.entry_type == e.type);
  CHECK(he.file_offset == e.offset);
  CHECK(to_string(he.key) == e.key);
  if (e.type == bytecask::EntryType::RangeDel) {
    CHECK(to_string(he.end_key) == e.end_key);
  } else {
    CHECK(he.value_size == e.value_size);
  }
}

void check_scan(const bytecask::HintFile &hf, const std::vector<Expected> &es) {
  auto scanner = hf.make_scanner();
  for (const auto &e : es) {
    const auto he = scanner.next();
    REQUIRE(he.has_value());
    check_entry(*he, e);
  }
  CHECK_FALSE(scanner.next().has_value());
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: the file is a header, zstd frames, and an inverted CRC trailer
// ---------------------------------------------------------------------------
TEST_CASE("HintFile writes header, frames and inverted CRC trailer",
          "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_size.hint";
  std::filesystem::remove(tmp);

  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(1, bytecask::EntryType::Put, 0, to_bytes("hello"), 5);
    hf.close();
  }

  const auto file = read_file(tmp);
  REQUIRE(file.size() > 16 + 4);
  CHECK(to_string(std::span{file}.first(7)) == "BCHINTZ");
  CHECK(file[7] == std::byte{0x81});
  CHECK(file[8] == std::byte{1}); // version
  CHECK(file[9] == std::byte{1}); // codec: zstd
  CHECK(std::ranges::all_of(std::span{file}.subspan(10, 6),
                            [](std::byte b) { return b == std::byte{0}; }));
  const auto body = std::span{file}.first(file.size() - 4);
  const auto stored = bytecask::read_le<std::uint32_t>(file, file.size() - 4);
  CHECK(stored == static_cast<std::uint32_t>(~crc_of(body)));
}

// ---------------------------------------------------------------------------
// Test 2: single entry round-trip — append then read back
// ---------------------------------------------------------------------------
TEST_CASE("HintFile single entry round-trip", "[hintfile]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / "bc_hint_roundtrip.hint";
  std::filesystem::remove(tmp);

  constexpr std::uint64_t kSeq = 42;
  constexpr std::uint64_t kFileOffset = 256;
  constexpr std::uint32_t kValueSize = 100;
  const std::string_view key_sv = "mykey";

  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(kSeq, bytecask::EntryType::Put, kFileOffset, to_bytes(key_sv),
              kValueSize);
    hf.close();
  }

  auto hf = bytecask::HintFile::OpenForRead(tmp);
  auto scanner = hf.make_scanner();
  const auto result = scanner.next();

  REQUIRE(result.has_value());
  CHECK(result->sequence == kSeq);
  CHECK(result->entry_type == bytecask::EntryType::Put);
  CHECK(result->file_offset == kFileOffset);
  CHECK(result->value_size == kValueSize);
  CHECK(to_string(result->key) == key_sv);

  // end of file
  CHECK_FALSE(scanner.next().has_value());
}

// ---------------------------------------------------------------------------
// Test 3: two entries can be written and scanned sequentially
// ---------------------------------------------------------------------------
TEST_CASE("HintFile two entries sequential scan", "[hintfile]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / "bc_hint_two_entries.hint";
  std::filesystem::remove(tmp);

  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(1, bytecask::EntryType::Put, 0, to_bytes("key1"), 10);
    hf.append(2, bytecask::EntryType::Delete, 512, to_bytes("key22"), 20);
    hf.close();
  }

  auto hf = bytecask::HintFile::OpenForRead(tmp);
  auto scanner = hf.make_scanner();

  const auto r0 = scanner.next();
  REQUIRE(r0.has_value());
  CHECK(r0->sequence == 1);
  CHECK(r0->entry_type == bytecask::EntryType::Put);
  CHECK(r0->file_offset == 0);
  CHECK(r0->value_size == 10);
  CHECK(to_string(r0->key) == "key1");

  const auto r1 = scanner.next();
  REQUIRE(r1.has_value());
  CHECK(r1->sequence == 2);
  CHECK(r1->entry_type == bytecask::EntryType::Delete);
  CHECK(r1->file_offset == 512);
  CHECK(r1->value_size == 20);
  CHECK(to_string(r1->key) == "key22");

  // end of file
  CHECK_FALSE(scanner.next().has_value());
}

// ---------------------------------------------------------------------------
// Test 4: CRC corruption causes a throw from OpenForRead (file-level check)
// ---------------------------------------------------------------------------
TEST_CASE("HintFile CRC mismatch throws", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_crc.hint";
  std::filesystem::remove(tmp);

  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(7, bytecask::EntryType::Put, 1024, to_bytes("corrupt"), 50);
    hf.close();
  }

  // Flip the first byte of the entry body to corrupt the file.
  {
    std::fstream f{tmp, std::ios::in | std::ios::out | std::ios::binary};
    REQUIRE(f.is_open());
    f.seekp(0);
    const char flipped = '\xFF';
    f.write(&flipped, 1);
  }

  // CRC is verified eagerly in OpenForRead — throws before any parsing.
  CHECK_THROWS_AS(bytecask::HintFile::OpenForRead(tmp), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Test 5: flat keys — both entries store full keys
// ---------------------------------------------------------------------------
TEST_CASE("HintFile flat keys round-trip", "[hintfile]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / "bc_hint_flat.hint";
  std::filesystem::remove(tmp);

  // "user:alice" (10 bytes) and "user:bob" (8 bytes) — both stored in full.
  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(1, bytecask::EntryType::Put, 0,   to_bytes("user:alice"), 10);
    hf.append(2, bytecask::EntryType::Put, 100, to_bytes("user:bob"),   20);
    hf.close();
  }

  auto hf = bytecask::HintFile::OpenForRead(tmp);
  auto scanner = hf.make_scanner();

  const auto r0 = scanner.next();
  REQUIRE(r0.has_value());
  CHECK(to_string(r0->key) == "user:alice");
  CHECK(r0->sequence == 1);

  const auto r1 = scanner.next();
  REQUIRE(r1.has_value());
  CHECK(to_string(r1->key) == "user:bob");
  CHECK(r1->sequence == 2);

  CHECK_FALSE(scanner.next().has_value());
}

// ---------------------------------------------------------------------------
// Frames: every reader sees the entries written, whatever the frame size
// ---------------------------------------------------------------------------
TEST_CASE("HintFile round-trips across frame boundaries", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_frames.hint";
  std::filesystem::remove(tmp);
  const auto es = sample_entries(2000);

  for (const std::size_t frame : {std::size_t{1}, std::size_t{64},
                                  std::size_t{1000}, std::size_t{0}}) {
    DYNAMIC_SECTION("frame bytes " << frame) {
      FrameBytes fb{frame};
      write_entries(tmp, es);
      check_scan(bytecask::HintFile::OpenForRead(tmp), es);
      check_scan(bytecask::HintFile::OpenForMerge(tmp), es);
    }
  }
}

TEST_CASE("HintFile compresses its entries", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_small.hint";
  std::filesystem::remove(tmp);
  const auto es = sample_entries(5000);
  write_entries(tmp, es);

  std::size_t raw = 0;
  for (const auto &e : es) {
    raw += bytecask::kHintHeaderSize + e.key.size();
    if (e.type == bytecask::EntryType::RangeDel) raw += 2 + e.end_key.size();
  }
  CHECK(std::filesystem::file_size(tmp) < raw / 2);
}

TEST_CASE("HintFile entries larger than a frame round-trip", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_big.hint";
  std::filesystem::remove(tmp);
  using bytecask::EntryType;
  const std::string max_key(0xFFFF, 'm');
  const std::vector<Expected> es{
      {1, EntryType::RangeDel, 0, max_key, 0xFFFF, std::string(0xFFFF, 'n')},
      {2, EntryType::Put, 10, "a", 1, ""},
      {3, EntryType::Put, 20, std::string(40000, 'b'), 7, ""},
      {4, EntryType::Delete, 30, max_key, 0, ""},
  };
  write_entries(tmp, es);
  check_scan(bytecask::HintFile::OpenForRead(tmp), es);
  check_scan(bytecask::HintFile::OpenForMerge(tmp), es);
}

TEST_CASE("HintFile empty file has no entries", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_empty.hint";
  std::filesystem::remove(tmp);
  write_entries(tmp, {});
  const auto hf = bytecask::HintFile::OpenForMerge(tmp);
  auto scanner = hf.make_scanner();
  const auto end = scanner.position();
  CHECK_FALSE(scanner.next().has_value());
  scanner.seek(end);
  CHECK_FALSE(scanner.next().has_value());
}

// ---------------------------------------------------------------------------
// Positions: seek() returns to any position a scan reported
// ---------------------------------------------------------------------------
TEST_CASE("HintFile seek returns to every position a scan reported",
          "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_seek.hint";
  std::filesystem::remove(tmp);
  const auto es = sample_entries(600);
  FrameBytes fb{100};
  write_entries(tmp, es);

  const auto hf = bytecask::HintFile::OpenForMerge(tmp);
  std::vector<bytecask::HintFile::Scanner::Position> at;
  {
    auto scanner = hf.make_scanner();
    for (;;) {
      at.push_back(scanner.position());
      if (!scanner.next()) break;
    }
  }
  REQUIRE(at.size() == es.size() + 1);
  CHECK(std::ranges::is_sorted(at));
  CHECK(at.front().frame != at[es.size() / 2].frame); // many frames

  // Backwards, so that every seek lands in a frame other than the current.
  auto scanner = hf.make_scanner();
  for (std::size_t i = es.size(); i-- > 0;) {
    scanner.seek(at[i]);
    const auto he = scanner.next();
    REQUIRE(he.has_value());
    check_entry(*he, es[i]);
    CHECK(scanner.position() == at[i + 1]);
  }
  scanner.seek(at.back());
  CHECK_FALSE(scanner.next().has_value());
}

// ---------------------------------------------------------------------------
// Compatibility: the layout written before compression
// ---------------------------------------------------------------------------
TEST_CASE("HintFile reads the uncompressed layout", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_raw.hint";
  std::filesystem::remove(tmp);
  const auto es = sample_entries(300);

  std::vector<std::byte> file;
  for (const auto &e : es) {
    const auto bytes =
        e.type == bytecask::EntryType::RangeDel
            ? bytecask::serialize_range_del_entry(e.seq, e.offset,
                                                  to_bytes(e.key),
                                                  to_bytes(e.end_key))
            : bytecask::serialize_entry(e.seq, e.type, e.offset, e.value_size,
                                        to_bytes(e.key));
    file.insert(file.end(), bytes.begin(), bytes.end());
  }
  const auto crc = crc_of(file);
  file.resize(file.size() + 4);
  put_trailer(file, crc);
  write_file(tmp, file);

  check_scan(bytecask::HintFile::OpenForRead(tmp), es);
  check_scan(bytecask::HintFile::OpenForMerge(tmp), es);

  SECTION("positions are byte offsets, seekable like a framed file's") {
    const auto hf = bytecask::HintFile::OpenForMerge(tmp);
    auto scanner = hf.make_scanner();
    (void)scanner.next();
    const auto second = scanner.position();
    CHECK(second.frame == 0);
    CHECK(second.offset == bytecask::kHintHeaderSize); // BulkBegin, no key
    (void)scanner.next();
    scanner.seek(second);
    const auto he = scanner.next();
    REQUIRE(he.has_value());
    check_entry(*he, es[1]);
  }
}

TEST_CASE("HintFile trailer fails a check that expects the old layout",
          "[hintfile]") {
  // A binary that predates compression checks a plain CRC-32C. The framed
  // trailer is inverted so that check fails, and such a binary rebuilds the
  // hint from its data file instead of parsing frames as entries.
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_old.hint";
  std::filesystem::remove(tmp);
  write_entries(tmp, sample_entries(50));
  const auto file = read_file(tmp);
  const auto stored = bytecask::read_le<std::uint32_t>(file, file.size() - 4);
  CHECK(stored != crc_of(std::span{file}.first(file.size() - 4)));
}

// ---------------------------------------------------------------------------
// Damage: a framed file that does not verify is refused before any parsing
// ---------------------------------------------------------------------------
TEST_CASE("HintFile damaged frame fails the CRC", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_bad.hint";
  std::filesystem::remove(tmp);
  write_entries(tmp, sample_entries(500));
  auto file = read_file(tmp);
  file[file.size() / 2] ^= std::byte{0x10};
  write_file(tmp, file);
  CHECK_THROWS_AS(bytecask::HintFile::OpenForRead(tmp), std::runtime_error);
  CHECK_THROWS_AS(bytecask::HintFile::OpenForMerge(tmp), std::runtime_error);
}

TEST_CASE("HintFile refuses an unknown version", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_ver.hint";
  std::filesystem::remove(tmp);
  write_entries(tmp, sample_entries(10));
  auto file = read_file(tmp);
  file[8] = std::byte{2};
  put_trailer(file, ~crc_of(std::span{file}.first(file.size() - 4)));
  write_file(tmp, file);
  CHECK_THROWS_AS(bytecask::HintFile::OpenForRead(tmp), std::runtime_error);
}

TEST_CASE("HintFile frame claiming a corrupt size is refused", "[hintfile]") {
  // Damage the CRC cannot see — the trailer recomputed over it — must still
  // not reach the parser as a frame.
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_zf.hint";
  std::filesystem::remove(tmp);
  write_entries(tmp, sample_entries(10));
  auto file = read_file(tmp);
  std::ranges::fill(std::span{file}.subspan(16, 8), std::byte{0xAB});
  put_trailer(file, ~crc_of(std::span{file}.first(file.size() - 4)));
  write_file(tmp, file);
  const auto hf = bytecask::HintFile::OpenForRead(tmp);
  auto scanner = hf.make_scanner();
  CHECK_THROWS_AS((void)scanner.next(), std::runtime_error);
}
