#include <catch2/catch_test_macros.hpp>

import std;
import bytecask.hint_file;

namespace {

auto to_bytes(std::string_view sv) -> std::span<const std::byte> {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(const std::vector<std::byte> &bytes) -> std::string {
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

  const std::size_t expected =
      bytecask::kHintHeaderSize + 5U + bytecask::kHintCrcSize;
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
  const auto result = hf.scan(0);

  REQUIRE(result.has_value());
  const auto &[entry, next] = *result;

  CHECK(entry.sequence == kSeq);
  CHECK(entry.entry_type == bytecask::EntryType::Put);
  CHECK(entry.file_offset == kFileOffset);
  CHECK(entry.value_size == kValueSize);
  CHECK(to_string(entry.key) == key_sv);

  // next offset should point past this entry to EOF
  CHECK(next ==
        bytecask::kHintHeaderSize + key_sv.size() + bytecask::kHintCrcSize);

  // reading at EOF returns nullopt
  CHECK_FALSE(hf.scan(next).has_value());
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

  const auto r0 = hf.scan(0);
  REQUIRE(r0.has_value());
  const auto &[e0, next0] = *r0;
  CHECK(e0.sequence == 1);
  CHECK(e0.entry_type == bytecask::EntryType::Put);
  CHECK(e0.file_offset == 0);
  CHECK(e0.value_size == 10);
  CHECK(to_string(e0.key) == "key1");

  const auto r1 = hf.scan(next0);
  REQUIRE(r1.has_value());
  const auto &[e1, next1] = *r1;
  CHECK(e1.sequence == 2);
  CHECK(e1.entry_type == bytecask::EntryType::Delete);
  CHECK(e1.file_offset == 512);
  CHECK(e1.value_size == 20);
  CHECK(to_string(e1.key) == "key22");

  // end of file
  CHECK_FALSE(hf.scan(next1).has_value());
}

// ---------------------------------------------------------------------------
// Test 4: CRC corruption causes a throw (panic), not silent skip
// ---------------------------------------------------------------------------
TEST_CASE("HintFile CRC mismatch throws", "[hintfile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_hint_crc.hint";
  std::filesystem::remove(tmp);

  {
    auto hf = bytecask::HintFile::OpenForWrite(tmp);
    hf.append(7, bytecask::EntryType::Put, 1024, to_bytes("corrupt"), 50);
    hf.sync();
  }

  // Flip the first byte of the key to corrupt the entry.
  {
    std::fstream f{tmp, std::ios::in | std::ios::out | std::ios::binary};
    REQUIRE(f.is_open());
    f.seekp(static_cast<std::streamoff>(bytecask::kHintHeaderSize));
    const char flipped = '\xFF';
    f.write(&flipped, 1);
  }

  auto hf = bytecask::HintFile::OpenForRead(tmp);
  CHECK_THROWS_AS(hf.scan(0), std::runtime_error);
}
