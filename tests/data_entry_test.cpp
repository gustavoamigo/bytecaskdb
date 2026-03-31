#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>
import bytecask.data_entry;
import bytecask.data_file;
import bytecask.types;

namespace {

// Read a little-endian value of type T from a byte buffer at offset.
template <typename T>
auto read_le(const std::vector<std::uint8_t> &buf, std::size_t offset) -> T {
  T v{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    v |= static_cast<T>(static_cast<T>(buf[offset + i]) << (8U * i));
  }
  return v;
}

// Read a binary file into a byte vector.
auto read_file_bytes(const std::filesystem::path &p)
    -> std::vector<std::uint8_t> {
  std::ifstream in{p, std::ios::binary | std::ios::ate};
  REQUIRE(in.is_open());
  const auto size = static_cast<std::size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> result(size);
  in.read(reinterpret_cast<char *>(result.data()),
          static_cast<std::streamsize>(size));
  return result;
}

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

// --------------------------------------------------------------------------
// Test 1: Verify byte layout and little-endian encoding of a serialized entry
// --------------------------------------------------------------------------
TEST_CASE("serialize_entry produces correct byte layout", "[serialize]") {
  const std::string_view key = "hello";
  const std::string_view value = "world";
  const std::uint64_t seq = 42;

  const auto buf = bytecask::serialize_entry(seq, bytecask::EntryType::Put,
                                             to_bytes(key), to_bytes(value));

  const std::size_t expected_size =
      bytecask::kHeaderSize + key.size() + value.size() + bytecask::kCrcSize;
  REQUIRE(buf.size() == expected_size);

  // Sequence at offset 0
  CHECK(read_le<std::uint64_t>(buf, 0) == seq);

  // EntryType at offset 8 should be Put (0x01)
  CHECK(static_cast<std::uint8_t>(buf[8]) == 0x01U);

  // Key size at offset 9
  CHECK(read_le<std::uint16_t>(buf, 9) ==
        static_cast<std::uint16_t>(key.size()));

  // Value size at offset 11
  CHECK(read_le<std::uint32_t>(buf, 11) ==
        static_cast<std::uint32_t>(value.size()));

  // Key bytes at offset 15 (kHeaderSize)
  for (std::size_t i = 0; i < key.size(); ++i) {
    CHECK(buf[bytecask::kHeaderSize + i] == static_cast<std::uint8_t>(key[i]));
  }

  // Value bytes immediately after key
  for (std::size_t i = 0; i < value.size(); ++i) {
    CHECK(buf[bytecask::kHeaderSize + key.size() + i] ==
          static_cast<std::uint8_t>(value[i]));
  }

  // CRC is the trailing 4 bytes; must be non-zero and deterministic
  const std::size_t crc_offset = buf.size() - bytecask::kCrcSize;
  const auto crc_first = read_le<std::uint32_t>(buf, crc_offset);
  const auto buf2 = bytecask::serialize_entry(seq, bytecask::EntryType::Put,
                                              to_bytes(key), to_bytes(value));
  const auto crc_second =
      read_le<std::uint32_t>(buf2, buf2.size() - bytecask::kCrcSize);
  CHECK(crc_first != 0);
  CHECK(crc_first == crc_second);
}

// --------------------------------------------------------------------------
// Test 2: Append two entries, reopen file, verify concatenated byte layout
//         and incrementing sequence numbers.
// --------------------------------------------------------------------------
TEST_CASE("DataFile appends two entries with correct offsets and sequences",
          "[datafile]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / "bc_test_append.data";
  std::filesystem::remove(tmp);

  {
    bytecask::DataFile df{tmp};
    const auto offset0 = df.append(1, bytecask::EntryType::Put,
                                   to_bytes("key1"), to_bytes("val1"));
    const auto offset1 = df.append(2, bytecask::EntryType::Put,
                                   to_bytes("key2"), to_bytes("val2"));

    CHECK(offset0 == 0);

    const std::size_t entry0_size =
        bytecask::kHeaderSize + 4U + 4U + bytecask::kCrcSize;
    CHECK(offset1 == entry0_size);

    // Explicit sync: batch both appends into one fdatasync call.
    df.sync();
  }

  const auto raw = read_file_bytes(tmp);

  const std::size_t entry_size =
      bytecask::kHeaderSize + 4U + 4U + bytecask::kCrcSize;
  REQUIRE(raw.size() == 2U * entry_size);

  // First entry: sequence == 1
  CHECK(read_le<std::uint64_t>(raw, 0) == 1U);
  // Second entry: sequence == 2
  CHECK(read_le<std::uint64_t>(raw, entry_size) == 2U);

  // Key bytes of first entry
  CHECK(raw[bytecask::kHeaderSize] == static_cast<std::uint8_t>('k'));
  CHECK(raw[bytecask::kHeaderSize + 3U] == static_cast<std::uint8_t>('1'));

  // Key bytes of second entry
  CHECK(raw[entry_size + bytecask::kHeaderSize + 3U] ==
        static_cast<std::uint8_t>('2'));

  std::filesystem::remove(tmp);
}

// --------------------------------------------------------------------------
// Test 3: read() round-trips sequence, key, and value at recorded offsets.
// --------------------------------------------------------------------------
TEST_CASE("DataFile::read round-trips entries at recorded offsets",
          "[datafile]") {
  const auto tmp = std::filesystem::temp_directory_path() / "bc_test_read.data";
  std::filesystem::remove(tmp);

  bytecask::DataFile df{tmp};
  const auto off0 = df.append(7, bytecask::EntryType::Put, to_bytes("hello"),
                              to_bytes("world"));
  const auto off1 =
      df.append(8, bytecask::EntryType::Put, to_bytes("foo"), to_bytes("bar"));
  df.sync();

  const auto r0 = df.read(off0);
  CHECK(r0.sequence == 7U);
  CHECK(r0.entry_type == bytecask::EntryType::Put);
  CHECK(to_string(r0.key) == "hello");
  CHECK(to_string(r0.value) == "world");

  const auto r1 = df.read(off1);
  CHECK(r1.sequence == 8U);
  CHECK(r1.entry_type == bytecask::EntryType::Put);
  CHECK(to_string(r1.key) == "foo");
  CHECK(to_string(r1.value) == "bar");

  std::filesystem::remove(tmp);
}

// --------------------------------------------------------------------------
// Test 4: Verify CRC32 detects corruption.
// --------------------------------------------------------------------------
TEST_CASE("CRC32 detects single-byte payload difference", "[crc]") {
  const auto buf_clean = bytecask::serialize_entry(
      1, bytecask::EntryType::Put, to_bytes("abc"), to_bytes("xyz"));
  const auto buf_corrupt =
      bytecask::serialize_entry(1, bytecask::EntryType::Put, to_bytes("abc"),
                                to_bytes("xYz")); // 'Y' vs 'y'

  const auto crc_clean =
      read_le<std::uint32_t>(buf_clean, buf_clean.size() - bytecask::kCrcSize);
  const auto crc_corrupt = read_le<std::uint32_t>(
      buf_corrupt, buf_corrupt.size() - bytecask::kCrcSize);

  CHECK(crc_clean != crc_corrupt);

  // Determinism: same inputs must always produce the same CRC.
  const auto buf_clean2 = bytecask::serialize_entry(
      1, bytecask::EntryType::Put, to_bytes("abc"), to_bytes("xyz"));
  const auto crc_clean2 = read_le<std::uint32_t>(
      buf_clean2, buf_clean2.size() - bytecask::kCrcSize);
  CHECK(crc_clean == crc_clean2);
}
