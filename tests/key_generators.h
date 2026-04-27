// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Shared key generators for benchmarks and tests.
// Each generator returns a vector of N string keys with a specific distribution.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace key_generators {

// ---------------------------------------------------------------------------
// splitmix64 — fast hash for deriving per-index pseudo-random state.
// Used by make_key functions that need random-looking content from an index.
// ---------------------------------------------------------------------------

inline auto splitmix64(std::uint64_t x) -> std::uint64_t {
  x += 0x9e3779b97f4a7c15;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
  x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
  return x ^ (x >> 31);
}

inline auto generate_uniform_keys(std::size_t n) -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    keys.push_back("key_" + std::to_string(i));
  return keys;
}

inline void make_uniform_key(std::size_t i, std::size_t /*n*/,
                             std::string &buf) {
  buf = "key_" + std::to_string(i);
}

inline auto generate_prefixed_keys(std::size_t n) -> std::vector<std::string> {
  static constexpr std::array prefixes = {
      "user::", "order::", "session::", "invoice::", "product::"};
  std::vector<std::string> keys;
  keys.reserve(n);
  auto per_prefix = n / prefixes.size();
  for (auto *pfx : prefixes) {
    for (std::size_t i = 0; i < per_prefix; ++i) {
      std::ostringstream oss;
      oss << pfx << "018f6e2c-" << std::hex << std::setfill('0') << std::setw(4)
          << (i >> 16) << "-7000-8000-" << std::setw(12) << (i & 0xFFFFFFFF);
      keys.push_back(oss.str());
    }
  }
  return keys;
}

inline void make_prefixed_key(std::size_t i, std::size_t n,
                              std::string &buf) {
  static constexpr std::array pfxs = {
      "user::", "order::", "session::", "invoice::", "product::"};
  auto per_prefix = n / pfxs.size();
  auto pfx_idx = (per_prefix > 0) ? i / per_prefix : 0;
  if (pfx_idx >= pfxs.size()) pfx_idx = pfxs.size() - 1;
  auto sub_i = i - pfx_idx * per_prefix;
  std::ostringstream oss;
  oss << pfxs[pfx_idx] << "018f6e2c-" << std::hex << std::setfill('0')
      << std::setw(4) << (sub_i >> 16) << "-7000-8000-" << std::setw(12)
      << (sub_i & 0xFFFFFFFF);
  buf = oss.str();
}

inline auto generate_incremental_keys(std::size_t n)
    -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 1; i <= n; ++i)
    keys.push_back(std::to_string(i));
  return keys;
}

inline void make_incremental_key(std::size_t i, std::size_t /*n*/,
                                 std::string &buf) {
  buf = std::to_string(i + 1);
}

// UUIDv7 keys — time-ordered UUIDs (RFC 9562). The first 48 bits encode a
// Unix timestamp in milliseconds. Bits 48-51 are version (7). Bits 52-63
// hold a 12-bit sub-millisecond counter. Bits 64-65 are variant (10).
// Bits 66-127 hold a 62-bit monotonic sequence seeded randomly each ms.
//
// In practice many keys share the same millisecond timestamp, making the
// first 12 hex chars (plus dashes) a shared prefix. The remaining fields
// form a monotonically increasing suffix — ideal for prefix compression.
//
// We simulate a realistic production burst: ~50 keys per millisecond.
// The full 128-bit value increases monotonically, so the hex representation
// shares a long common prefix that grows as keys are added within the same ms.
inline auto generate_uuidv7_keys(std::size_t n) -> std::vector<std::string> {
  static constexpr char hex_chars[] = "0123456789abcdef";

  auto encode_hex = [](char *dst, std::uint64_t val, int nibbles) {
    for (int j = nibbles - 1; j >= 0; --j) {
      dst[j] = hex_chars[val & 0xF];
      val >>= 4;
    }
  };

  // Simulate: ~50 keys per millisecond, timestamp increments each batch.
  auto base_ms = static_cast<std::uint64_t>(0x018F6E2C0000);
  constexpr std::size_t keys_per_ms = 50;
  // Random base for the lower 62 bits, incremented monotonically per key.
  auto seq_base = static_cast<std::uint64_t>(0x1A2B3C4D5E6F);

  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    auto ts = base_ms + i / keys_per_ms;
    auto seq = seq_base + i; // monotonic across all keys

    // Pack into 128-bit UUIDv7 layout:
    //   bits 0-47:   timestamp (48 bits)
    //   bits 48-51:  version = 0b0111 (4 bits)
    //   bits 52-63:  rand_a / sub-ms counter (12 bits)
    //   bits 64-65:  variant = 0b10 (2 bits)
    //   bits 66-127: rand_b / monotonic sequence (62 bits)
    auto rand_a = (seq >> 50) & 0xFFF;           // top 12 bits of seq
    auto rand_b = seq & 0x3FFFFFFFFFFFFFFF;       // lower 62 bits

    std::string uuid(36, '\0');
    encode_hex(uuid.data(), ts >> 16, 8);         // positions 0-7
    uuid[8] = '-';
    encode_hex(uuid.data() + 9, ts & 0xFFFF, 4); // positions 9-12
    uuid[13] = '-';
    uuid[14] = '7';                               // version
    encode_hex(uuid.data() + 15, rand_a, 3);      // positions 15-17
    uuid[18] = '-';
    // variant (10) in top 2 bits of this nibble
    uuid[19] = hex_chars[8 | ((rand_b >> 60) & 0x3)];
    encode_hex(uuid.data() + 20, (rand_b >> 48) & 0xFFF, 3); // positions 20-22
    uuid[23] = '-';
    encode_hex(uuid.data() + 24, rand_b & 0xFFFFFFFFFFFF, 12); // positions 24-35

    keys.push_back(std::move(uuid));
  }
  return keys;
}

inline void make_uuidv7_key(std::size_t i, std::size_t /*n*/,
                            std::string &buf) {
  static constexpr char hex_chars[] = "0123456789abcdef";
  auto encode_hex = [](char *dst, std::uint64_t val, int nibbles) {
    for (int j = nibbles - 1; j >= 0; --j) {
      dst[j] = hex_chars[val & 0xF];
      val >>= 4;
    }
  };
  auto base_ms = static_cast<std::uint64_t>(0x018F6E2C0000);
  constexpr std::size_t keys_per_ms = 50;
  auto seq_base = static_cast<std::uint64_t>(0x1A2B3C4D5E6F);
  auto ts = base_ms + i / keys_per_ms;
  auto seq = seq_base + i;
  auto rand_a = (seq >> 50) & 0xFFF;
  auto rand_b = seq & 0x3FFFFFFFFFFFFFFF;
  buf.resize(36);
  encode_hex(buf.data(), ts >> 16, 8);
  buf[8] = '-';
  encode_hex(buf.data() + 9, ts & 0xFFFF, 4);
  buf[13] = '-';
  buf[14] = '7';
  encode_hex(buf.data() + 15, rand_a, 3);
  buf[18] = '-';
  buf[19] = hex_chars[8 | ((rand_b >> 60) & 0x3)];
  encode_hex(buf.data() + 20, (rand_b >> 48) & 0xFFF, 3);
  buf[23] = '-';
  encode_hex(buf.data() + 24, rand_b & 0xFFFFFFFFFFFF, 12);
}

// UUIDv7 binary keys — raw 16 bytes with the same time-ordered monotonic
// structure as the text UUIDv7, but stored as binary (no hex encoding).
// Used by systems that store UUIDs in compact binary form.
inline auto generate_uuidv7_binary_keys(std::size_t n)
    -> std::vector<std::string> {
  auto base_ms = static_cast<std::uint64_t>(0x018F6E2C0000);
  constexpr std::size_t keys_per_ms = 50;
  auto seq_base = static_cast<std::uint64_t>(0x1A2B3C4D5E6F);

  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    auto ts = base_ms + i / keys_per_ms;
    auto seq = seq_base + i;

    // Pack into 16 raw bytes (128 bits):
    //   bytes 0-5:  timestamp (48 bits, big-endian)
    //   byte  6:    version (0111) | rand_a top 4 bits
    //   byte  7:    rand_a bottom 8 bits
    //   byte  8:    variant (10) | rand_b top 6 bits
    //   bytes 9-15: rand_b bottom 56 bits
    auto rand_a = (seq >> 50) & 0xFFF;
    auto rand_b = seq & 0x3FFFFFFFFFFFFFFF;

    std::string k(16, '\0');
    // Timestamp big-endian (6 bytes)
    k[0] = static_cast<char>((ts >> 40) & 0xFF);
    k[1] = static_cast<char>((ts >> 32) & 0xFF);
    k[2] = static_cast<char>((ts >> 24) & 0xFF);
    k[3] = static_cast<char>((ts >> 16) & 0xFF);
    k[4] = static_cast<char>((ts >> 8) & 0xFF);
    k[5] = static_cast<char>(ts & 0xFF);
    // Version + rand_a
    k[6] = static_cast<char>(0x70 | ((rand_a >> 8) & 0x0F));
    k[7] = static_cast<char>(rand_a & 0xFF);
    // Variant + rand_b
    k[8] = static_cast<char>(0x80 | ((rand_b >> 56) & 0x3F));
    k[9] = static_cast<char>((rand_b >> 48) & 0xFF);
    k[10] = static_cast<char>((rand_b >> 40) & 0xFF);
    k[11] = static_cast<char>((rand_b >> 32) & 0xFF);
    k[12] = static_cast<char>((rand_b >> 24) & 0xFF);
    k[13] = static_cast<char>((rand_b >> 16) & 0xFF);
    k[14] = static_cast<char>((rand_b >> 8) & 0xFF);
    k[15] = static_cast<char>(rand_b & 0xFF);
    keys.push_back(std::move(k));
  }
  return keys;
}

inline void make_uuidv7_binary_key(std::size_t i, std::size_t /*n*/,
                                   std::string &buf) {
  auto base_ms = static_cast<std::uint64_t>(0x018F6E2C0000);
  constexpr std::size_t keys_per_ms = 50;
  auto seq_base = static_cast<std::uint64_t>(0x1A2B3C4D5E6F);
  auto ts = base_ms + i / keys_per_ms;
  auto seq = seq_base + i;
  auto rand_a = (seq >> 50) & 0xFFF;
  auto rand_b = seq & 0x3FFFFFFFFFFFFFFF;
  buf.resize(16);
  buf[0] = static_cast<char>((ts >> 40) & 0xFF);
  buf[1] = static_cast<char>((ts >> 32) & 0xFF);
  buf[2] = static_cast<char>((ts >> 24) & 0xFF);
  buf[3] = static_cast<char>((ts >> 16) & 0xFF);
  buf[4] = static_cast<char>((ts >> 8) & 0xFF);
  buf[5] = static_cast<char>(ts & 0xFF);
  buf[6] = static_cast<char>(0x70 | ((rand_a >> 8) & 0x0F));
  buf[7] = static_cast<char>(rand_a & 0xFF);
  buf[8] = static_cast<char>(0x80 | ((rand_b >> 56) & 0x3F));
  buf[9] = static_cast<char>((rand_b >> 48) & 0xFF);
  buf[10] = static_cast<char>((rand_b >> 40) & 0xFF);
  buf[11] = static_cast<char>((rand_b >> 32) & 0xFF);
  buf[12] = static_cast<char>((rand_b >> 24) & 0xFF);
  buf[13] = static_cast<char>((rand_b >> 16) & 0xFF);
  buf[14] = static_cast<char>((rand_b >> 8) & 0xFF);
  buf[15] = static_cast<char>(rand_b & 0xFF);
}

// SHA-256 hex keys — 64-character hex strings like git object hashes or
// content-addressed storage keys. Fully random, no prefix structure.
inline auto generate_sha256_hex_keys(std::size_t n)
    -> std::vector<std::string> {
  std::mt19937 rng{42};
  std::uniform_int_distribution<int> hex_dist(0, 15);
  static constexpr char hex_chars[] = "0123456789abcdef";
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string k(64, '\0');
    for (auto &c : k)
      c = hex_chars[hex_dist(rng)];
    keys.push_back(std::move(k));
  }
  return keys;
}

inline void make_sha256_hex_key(std::size_t i, std::size_t /*n*/,
                                std::string &buf) {
  static constexpr char hex_chars[] = "0123456789abcdef";
  buf.resize(64);
  // 64 hex chars = 32 nibbles from 4 splitmix64 calls (64 bits = 16 nibbles each).
  for (int block = 0; block < 4; ++block) {
    auto h = splitmix64(42 + i * 4 + static_cast<std::size_t>(block));
    for (int j = 0; j < 16; ++j) {
      buf[static_cast<std::size_t>(block * 16 + j)] =
          hex_chars[h & 0xF];
      h >>= 4;
    }
  }
}

// SHA-256 binary keys — raw 32 bytes, as stored by content-addressed systems
// that skip hex encoding for compactness.
inline auto generate_sha256_bin_keys(std::size_t n)
    -> std::vector<std::string> {
  std::mt19937 rng{43};
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string k(32, '\0');
    for (auto &c : k)
      c = static_cast<char>(byte_dist(rng));
    keys.push_back(std::move(k));
  }
  return keys;
}

inline void make_sha256_bin_key(std::size_t i, std::size_t /*n*/,
                                std::string &buf) {
  buf.resize(32);
  // 32 bytes = 4 splitmix64 calls (8 bytes each).
  for (int block = 0; block < 4; ++block) {
    auto h = splitmix64(43 + i * 4 + static_cast<std::size_t>(block));
    for (int j = 0; j < 8; ++j) {
      buf[static_cast<std::size_t>(block * 8 + j)] =
          static_cast<char>(h & 0xFF);
      h >>= 8;
    }
  }
}

// Zipfian / skewed distribution — a small number of "hot" keys appear
// repeatedly in the keyspace. Returns N unique keys, but the first ~5%
// account for half the total key population (simulating hot-key overwrites).
// For memory testing, we generate the unique key set since duplicates just
// overwrite the same slot.
inline auto generate_zipfian_keys(std::size_t n) -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    // Keys have varying "temperature" — hot keys are short, cold keys longer.
    if (i < n / 20) {
      // Hot 5%: short keys, high overwrite frequency in workloads.
      keys.push_back("hot_" + std::to_string(i));
    } else {
      // Cold 95%: longer keys with a partition-like prefix.
      std::ostringstream oss;
      oss << "cold_partition_" << (i % 10) << "/key_"
          << std::setfill('0') << std::setw(8) << i;
      keys.push_back(oss.str());
    }
  }
  return keys;
}

inline void make_zipfian_key(std::size_t i, std::size_t n,
                             std::string &buf) {
  if (i < n / 20) {
    buf = "hot_" + std::to_string(i);
  } else {
    std::ostringstream oss;
    oss << "cold_partition_" << (i % 10) << "/key_"
        << std::setfill('0') << std::setw(8) << i;
    buf = oss.str();
  }
}

// High-cardinality clustering — many keys under a few partitions.
// Mimics a database where most keys belong to a handful of large tables.
inline auto generate_clustered_keys(std::size_t n)
    -> std::vector<std::string> {
  static constexpr std::array partitions = {
      "users/", "orders/", "events/"};
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::ostringstream oss;
    oss << partitions[i % partitions.size()]
        << std::setfill('0') << std::setw(10) << i;
    keys.push_back(oss.str());
  }
  return keys;
}

inline void make_clustered_key(std::size_t i, std::size_t /*n*/,
                               std::string &buf) {
  static constexpr std::array partitions = {
      "users/", "orders/", "events/"};
  std::ostringstream oss;
  oss << partitions[i % partitions.size()]
      << std::setfill('0') << std::setw(10) << i;
  buf = oss.str();
}

// Many small partitions — keys spread across many distinct prefixes.
// Opposite of clustered: wide fanout at the first level.
inline auto generate_many_partitions_keys(std::size_t n)
    -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(n);
  auto num_partitions = n / 2; // 2 keys per partition on average.
  for (std::size_t i = 0; i < n; ++i) {
    std::ostringstream oss;
    oss << "p" << std::setfill('0') << std::setw(6) << (i % num_partitions)
        << "/k" << std::setw(4) << (i / num_partitions);
    keys.push_back(oss.str());
  }
  return keys;
}

inline void make_many_partitions_key(std::size_t i, std::size_t n,
                                     std::string &buf) {
  auto num_partitions = n / 2;
  std::ostringstream oss;
  oss << "p" << std::setfill('0') << std::setw(6) << (i % num_partitions)
      << "/k" << std::setw(4) << (i / num_partitions);
  buf = oss.str();
}

// UUIDv4 text format — 36-byte keys like "550e8400-e29b-41d4-a716-446655440000".
// No shared prefix; only the fixed dashes at positions 8, 13, 18, 23 are common.
// This is the most common key format in modern web applications.
inline auto generate_uuidv4_text_keys(std::size_t n)
    -> std::vector<std::string> {
  std::mt19937 rng{314};
  std::uniform_int_distribution<int> hex_dist(0, 15);
  static constexpr char hex_chars[] = "0123456789abcdef";
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string uuid(36, '\0');
    for (std::size_t j = 0; j < 36; ++j) {
      if (j == 8 || j == 13 || j == 18 || j == 23) {
        uuid[j] = '-';
      } else {
        uuid[j] = hex_chars[hex_dist(rng)];
      }
    }
    // Force version nibble (pos 14) = '4' and variant nibble (pos 19) in {8,9,a,b}.
    uuid[14] = '4';
    uuid[19] = hex_chars[8 + (hex_dist(rng) & 0x3)];
    keys.push_back(std::move(uuid));
  }
  return keys;
}

inline void make_uuidv4_text_key(std::size_t i, std::size_t /*n*/,
                                 std::string &buf) {
  static constexpr char hex_chars[] = "0123456789abcdef";
  buf.resize(36);
  // 32 hex nibbles needed (36 - 4 dashes). Use 2 splitmix64 calls (32 nibbles).
  auto h0 = splitmix64(314 + i * 2);
  auto h1 = splitmix64(314 + i * 2 + 1);
  int nibble = 0;
  std::uint64_t h = h0;
  for (std::size_t j = 0; j < 36; ++j) {
    if (j == 8 || j == 13 || j == 18 || j == 23) {
      buf[j] = '-';
    } else {
      if (nibble == 16) { h = h1; }
      buf[j] = hex_chars[h & 0xF];
      h >>= 4;
      ++nibble;
    }
  }
  buf[14] = '4';
  buf[19] = hex_chars[8 | (static_cast<int>(buf[19]) & 0x3)];
}

// UUIDv4 text with type prefixes — "user::550e8400-...", "order::550e8400-...".
// The most realistic key shape for a database key directory: structured prefix
// with random UUID suffix. Prefix compression should amortize the type prefix.
inline auto generate_prefixed_uuidv4_keys(std::size_t n)
    -> std::vector<std::string> {
  static constexpr std::array prefixes = {
      "user::", "order::", "session::", "invoice::", "product::"};
  std::mt19937 rng{271};
  std::uniform_int_distribution<int> hex_dist(0, 15);
  static constexpr char hex_chars[] = "0123456789abcdef";
  std::vector<std::string> keys;
  keys.reserve(n);
  auto per_prefix = n / prefixes.size();
  for (auto *pfx : prefixes) {
    for (std::size_t i = 0; i < per_prefix; ++i) {
      std::string uuid(36, '\0');
      for (std::size_t j = 0; j < 36; ++j) {
        if (j == 8 || j == 13 || j == 18 || j == 23) {
          uuid[j] = '-';
        } else {
          uuid[j] = hex_chars[hex_dist(rng)];
        }
      }
      uuid[14] = '4';
      uuid[19] = hex_chars[8 + (hex_dist(rng) & 0x3)];
      keys.push_back(std::string(pfx) + uuid);
    }
  }
  return keys;
}

inline void make_prefixed_uuidv4_key(std::size_t i, std::size_t n,
                                     std::string &buf) {
  static constexpr std::array prefixes = {
      "user::", "order::", "session::", "invoice::", "product::"};
  static constexpr char hex_chars[] = "0123456789abcdef";
  auto per_prefix = n / prefixes.size();
  auto pfx_idx = (per_prefix > 0) ? i / per_prefix : 0;
  if (pfx_idx >= prefixes.size()) pfx_idx = prefixes.size() - 1;
  auto sub_i = i - pfx_idx * per_prefix;
  // Generate UUID from splitmix64 keyed on sub-index.
  auto h0 = splitmix64(271 + sub_i * 2);
  auto h1 = splitmix64(271 + sub_i * 2 + 1);
  std::string uuid(36, '\0');
  int nibble = 0;
  std::uint64_t h = h0;
  for (std::size_t j = 0; j < 36; ++j) {
    if (j == 8 || j == 13 || j == 18 || j == 23) {
      uuid[j] = '-';
    } else {
      if (nibble == 16) { h = h1; }
      uuid[j] = hex_chars[h & 0xF];
      h >>= 4;
      ++nibble;
    }
  }
  uuid[14] = '4';
  uuid[19] = hex_chars[8 | (static_cast<int>(uuid[19]) & 0x3)];
  buf = std::string(prefixes[pfx_idx]) + uuid;
}

// UUIDv4 binary format — raw 16 bytes, no hex encoding.
// Some systems store UUIDs as raw bytes for compactness. Keys are 16 bytes
// with random content — the tree sees opaque binary, no structure to exploit.
inline auto generate_uuidv4_binary_keys(std::size_t n)
    -> std::vector<std::string> {
  std::mt19937 rng{161};
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string k(16, '\0');
    for (auto &c : k)
      c = static_cast<char>(byte_dist(rng));
    // Version nibble and variant bits for correctness, though the tree doesn't care.
    k[6] = static_cast<char>((static_cast<unsigned char>(k[6]) & 0x0F) | 0x40);
    k[8] = static_cast<char>((static_cast<unsigned char>(k[8]) & 0x3F) | 0x80);
    // Stamp index in last 4 bytes for uniqueness.
    k[12] = static_cast<char>((i >> 24) & 0xFF);
    k[13] = static_cast<char>((i >> 16) & 0xFF);
    k[14] = static_cast<char>((i >> 8) & 0xFF);
    k[15] = static_cast<char>(i & 0xFF);
    keys.push_back(std::move(k));
  }
  return keys;
}

inline void make_uuidv4_binary_key(std::size_t i, std::size_t /*n*/,
                                   std::string &buf) {
  buf.resize(16);
  // 16 bytes = 2 splitmix64 calls.
  auto h0 = splitmix64(161 + i * 2);
  auto h1 = splitmix64(161 + i * 2 + 1);
  for (int j = 0; j < 8; ++j) {
    buf[static_cast<std::size_t>(j)] = static_cast<char>(h0 & 0xFF);
    h0 >>= 8;
  }
  for (int j = 0; j < 8; ++j) {
    buf[static_cast<std::size_t>(8 + j)] = static_cast<char>(h1 & 0xFF);
    h1 >>= 8;
  }
  buf[6] = static_cast<char>((static_cast<unsigned char>(buf[6]) & 0x0F) | 0x40);
  buf[8] = static_cast<char>((static_cast<unsigned char>(buf[8]) & 0x3F) | 0x80);
  // Stamp index in last 4 bytes for uniqueness.
  buf[12] = static_cast<char>((i >> 24) & 0xFF);
  buf[13] = static_cast<char>((i >> 16) & 0xFF);
  buf[14] = static_cast<char>((i >> 8) & 0xFF);
  buf[15] = static_cast<char>(i & 0xFF);
}

// Hash-prefixed ordered keys — a common pattern in Cassandra/DynamoDB where
// the partition key is a hash (for even distribution) followed by an ordered
// sort key. Example: "a3f7b2c1::item::000001", "a3f7b2c1::item::000002".
// The hash prefix defeats prefix compression at the top level, but within
// each partition the sort keys share structure.
inline auto generate_hash_prefixed_keys(std::size_t n)
    -> std::vector<std::string> {
  // 8 partitions, each with a random 8-hex-char hash prefix.
  static constexpr std::size_t kPartitions = 8;
  std::mt19937 rng{77};
  std::uniform_int_distribution<int> hex_dist(0, 15);
  static constexpr char hex_chars[] = "0123456789abcdef";

  // Pre-generate partition hashes.
  std::array<std::string, kPartitions> hashes;
  for (auto &h : hashes) {
    h.resize(8);
    for (auto &c : h)
      c = hex_chars[hex_dist(rng)];
  }

  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::ostringstream oss;
    oss << hashes[i % kPartitions] << "::item::" << std::setfill('0')
        << std::setw(8) << (i / kPartitions);
    keys.push_back(oss.str());
  }
  return keys;
}

inline void make_hash_prefixed_key(std::size_t i, std::size_t /*n*/,
                                   std::string &buf) {
  static constexpr std::size_t kPartitions = 8;
  static constexpr char hex_chars[] = "0123456789abcdef";
  // Derive partition hashes deterministically from splitmix64.
  auto part = i % kPartitions;
  auto h = splitmix64(77 + part);
  char hash_prefix[9];
  for (int j = 0; j < 8; ++j) {
    hash_prefix[j] = hex_chars[h & 0xF];
    h >>= 4;
  }
  hash_prefix[8] = '\0';
  std::ostringstream oss;
  oss << hash_prefix << "::item::" << std::setfill('0')
      << std::setw(8) << (i / kPartitions);
  buf = oss.str();
}

// Binary / non-ASCII keys with 0x00, 0x80, 0xFF bytes embedded.
// Tests edge cases in prefix comparison and child byte transitions.
inline auto generate_binary_keys(std::size_t n) -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string k(8, '\0');
    // Embed interesting byte values: 0x00, 0x80, 0xFF interspersed.
    k[0] = static_cast<char>(i & 0xFF);
    k[1] = '\x00';
    k[2] = '\x80';
    k[3] = '\xFF';
    k[4] = static_cast<char>((i >> 8) & 0xFF);
    k[5] = static_cast<char>((i >> 16) & 0xFF);
    k[6] = static_cast<char>((i >> 24) & 0xFF);
    k[7] = static_cast<char>(i % 127);
    keys.push_back(k);
  }
  return keys;
}

inline void make_binary_key(std::size_t i, std::size_t /*n*/,
                            std::string &buf) {
  buf.resize(8);
  buf[0] = static_cast<char>(i & 0xFF);
  buf[1] = '\x00';
  buf[2] = '\x80';
  buf[3] = '\xFF';
  buf[4] = static_cast<char>((i >> 8) & 0xFF);
  buf[5] = static_cast<char>((i >> 16) & 0xFF);
  buf[6] = static_cast<char>((i >> 24) & 0xFF);
  buf[7] = static_cast<char>(i % 127);
}

// Mixed shape — combines real-world key types into one pool to test whether
// high-entropy keys degrade the tree structure for well-behaved keys.
inline auto generate_mixed_keys(std::size_t n) -> std::vector<std::string> {
  struct Slice {
    double pct;
    std::vector<std::string> (*gen)(std::size_t);
  };
  std::array<Slice, 7> slices = {{
      {0.25, generate_prefixed_uuidv4_keys},  // database key directory
      {0.20, generate_clustered_keys},      // partitioned tables
      {0.15, generate_hash_prefixed_keys},  // Cassandra/DynamoDB
      {0.10, generate_uuidv4_text_keys},           // bare UUIDv4 text
      {0.10, generate_uuidv4_binary_keys},    // compact UUID storage
      {0.10, generate_sha256_hex_keys},     // content-addressed (hex)
      {0.10, generate_sha256_bin_keys},     // content-addressed (binary)
  }};
  std::vector<std::string> keys;
  keys.reserve(n);
  for (auto &[pct, gen] : slices) {
    auto count = static_cast<std::size_t>(static_cast<double>(n) * pct);
    auto batch = gen(count);
    keys.insert(keys.end(),
                std::make_move_iterator(batch.begin()),
                std::make_move_iterator(batch.end()));
  }
  while (keys.size() < n)
    keys.push_back("pad_" + std::to_string(keys.size()));
  keys.resize(n);
  std::mt19937 rng{999};
  std::shuffle(keys.begin(), keys.end(), rng);
  return keys;
}

// Streaming mixed: generates from sub-generators in order (no shuffle).
// The batch version shuffles for benchmark fairness; streaming skips that
// since memory profiling doesn't care about insertion order.
inline void make_mixed_key(std::size_t i, std::size_t n, std::string &buf) {
  struct Slice {
    double pct;
    void (*make)(std::size_t, std::size_t, std::string &);
  };
  static constexpr std::array<Slice, 7> slices = {{
      {0.25, make_prefixed_uuidv4_key},
      {0.20, make_clustered_key},
      {0.15, make_hash_prefixed_key},
      {0.10, make_uuidv4_text_key},
      {0.10, make_uuidv4_binary_key},
      {0.10, make_sha256_hex_key},
      {0.10, make_sha256_bin_key},
  }};
  // Map global index i to (slice, sub-index).
  std::size_t offset = 0;
  for (auto &[pct, make] : slices) {
    auto count = static_cast<std::size_t>(static_cast<double>(n) * pct);
    if (i < offset + count) {
      make(i - offset, count, buf);
      return;
    }
    offset += count;
  }
  // Padding keys for remainder.
  buf = "pad_" + std::to_string(i);
}

// Key shape descriptor for parameterized tests and benchmarks.
// generate(): returns all N keys in a vector (for benchmarks/tests).
// make_key(): writes one key into a reusable buffer (for memory profiling).
struct KeyShape {
  std::string name;
  std::vector<std::string> (*generate)(std::size_t);
  void (*make_key)(std::size_t i, std::size_t n, std::string &buf);
};

inline auto all_key_shapes() -> std::vector<KeyShape> {
  return {
      {"uniform", generate_uniform_keys, make_uniform_key},
      {"prefixed", generate_prefixed_keys, make_prefixed_key},
      {"incremental", generate_incremental_keys, make_incremental_key},
      {"uuidv7", generate_uuidv7_keys, make_uuidv7_key},
      {"uuidv7_binary", generate_uuidv7_binary_keys, make_uuidv7_binary_key},
      {"sha256_hex", generate_sha256_hex_keys, make_sha256_hex_key},
      {"sha256_bin", generate_sha256_bin_keys, make_sha256_bin_key},
      {"uuidv4_text", generate_uuidv4_text_keys, make_uuidv4_text_key},
      {"uuidv4_prefixed", generate_prefixed_uuidv4_keys, make_prefixed_uuidv4_key},
      {"uuidv4_binary", generate_uuidv4_binary_keys, make_uuidv4_binary_key},
      {"hash_prefixed", generate_hash_prefixed_keys, make_hash_prefixed_key},
      {"binary", generate_binary_keys, make_binary_key},
      {"zipfian", generate_zipfian_keys, make_zipfian_key},
      {"clustered", generate_clustered_keys, make_clustered_key},
      {"many_partitions", generate_many_partitions_keys, make_many_partitions_key},
      {"mixed", generate_mixed_keys, make_mixed_key},
  };
}

// Look up a key shape by name. Returns nullptr if not found.
inline auto key_shape_by_name(std::string_view name) -> const KeyShape * {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wunique-object-duplication"
  static auto shapes = all_key_shapes(); // intentional: lives for process lifetime
#pragma clang diagnostic pop
  for (auto &s : shapes) {
    if (s.name == name)
      return &s;
  }
  return nullptr;
}

} // namespace key_generators
