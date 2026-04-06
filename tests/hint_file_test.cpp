#include <algorithm>
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

} // namespace

// ---------------------------------------------------------------------------
// Test 1: append writes the correct number of bytes to disk
// ---------------------------------------------------------------------------
TEST_CASE("HintFile append produces correct file size", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_size.hint";
  std::filesystem::remove(tmp);

  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(1, bytecask::EntryType::Put, 0, to_bytes("hello"), 5);
    hf.sync();
  }

  // "hello" has no shared prefix with the empty previous key → suffix_len = 5
  const std::size_t expected =
      bytecask::kHintHeaderSize + 5U + 4U; // entries + file CRC trailer
  CHECK(std::filesystem::file_size(tmp) == expected);
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
    hf.sync();
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
    hf.sync();
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
    hf.sync();
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
// Test 5: prefix compression round-trip — two entries with a common prefix
// ---------------------------------------------------------------------------
TEST_CASE("HintFile prefix compression round-trip", "[hintfile]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / "bc_hint_prefix.hint";
  std::filesystem::remove(tmp);

  // "user:alice" and "user:bob" share the 5-byte prefix "user:".
  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(1, bytecask::EntryType::Put, 0,   to_bytes("user:alice"), 10);
    hf.append(2, bytecask::EntryType::Put, 100, to_bytes("user:bob"),   20);
    hf.sync();
  }

  // File size: entry0 has suffix_len=10 (no previous key), entry1 has
  // suffix_len=3 ("bob") since "user:" (5 bytes) is the shared prefix.
  // Total = 2*kHintHeaderSize + 10 + 3 + 4 (file CRC)
  const std::size_t expected =
      2 * bytecask::kHintHeaderSize + 10U + 3U + 4U;
  CHECK(std::filesystem::file_size(tmp) == expected);

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

