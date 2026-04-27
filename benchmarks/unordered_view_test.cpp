// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// UnorderedView — unit tests

#include "unordered_view.h"
#include "key_generators.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import bytecask;

namespace {

auto to_bytes(std::string_view sv) -> bytecask::BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(const bytecask::Bytes &bytes) -> std::string {
  std::string s(bytes.size(), '\0');
  std::ranges::transform(bytes, s.begin(),
                         [](std::byte b) { return static_cast<char>(b); });
  return s;
}

struct TempDir {
  std::filesystem::path path;

  TempDir()
      : path{std::filesystem::temp_directory_path() /
             std::format(
                 "bc_uv_test_{}_{}",
                 std::chrono::system_clock::now().time_since_epoch().count(),
                 next_id())} {
    std::filesystem::create_directories(path);
  }

  ~TempDir() { std::filesystem::remove_all(path); }

 private:
  static auto next_id() -> unsigned {
    static unsigned counter = 0;
    return counter++;
  }
};

// Find two strings that collide on hash16 (same 16-bit fingerprint).
// Brute-forces with sequential strings — fast since hash16 is 16-bit.
auto find_hash16_collision()
    -> std::pair<std::string, std::string> {
  constexpr std::uint32_t kFpSeed = 0x9E3779B9;
  // Pick a target: hash16 of "collision_a".
  std::string key_a = "collision_a";
  auto h_a = static_cast<std::uint16_t>(unordered_view::murmur3_32(
      reinterpret_cast<const std::byte *>(key_a.data()), key_a.size(),
      kFpSeed));

  // Find another key with same hash16.
  for (unsigned i = 0; i < 200'000; ++i) {
    auto candidate = std::format("collision_{}", i);
    if (candidate == key_a) continue;
    auto h = static_cast<std::uint16_t>(unordered_view::murmur3_32(
        reinterpret_cast<const std::byte *>(candidate.data()),
        candidate.size(), kFpSeed));
    if (h == h_a) {
      // Also need same bucket — use seed 0 for routing.
      return {key_a, candidate};
    }
  }
  // Extremely unlikely to reach here with 200k candidates vs 65536 space.
  FAIL("Could not find hash16 collision");
  return {};
}

} // namespace

TEST_CASE("UnorderedView put and get round-trip", "[unordered_view]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  unordered_view::UnorderedView view{db, "uv"};

  view.put(to_bytes("key1"), to_bytes("value1"));

  bytecask::Bytes out;
  REQUIRE(view.get(to_bytes("key1"), out));
  CHECK(to_string(out) == "value1");
}

TEST_CASE("UnorderedView overwrite returns latest value", "[unordered_view]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  unordered_view::UnorderedView view{db, "uv"};

  view.put(to_bytes("key1"), to_bytes("first"));
  view.put(to_bytes("key1"), to_bytes("second"));

  bytecask::Bytes out;
  REQUIRE(view.get(to_bytes("key1"), out));
  CHECK(to_string(out) == "second");
}

TEST_CASE("UnorderedView contains_key", "[unordered_view]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  unordered_view::UnorderedView view{db, "uv"};

  CHECK_FALSE(view.contains_key(to_bytes("absent")));

  view.put(to_bytes("present"), to_bytes("val"));
  CHECK(view.contains_key(to_bytes("present")));
  CHECK_FALSE(view.contains_key(to_bytes("absent")));
}

TEST_CASE("UnorderedView del removes key", "[unordered_view]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  unordered_view::UnorderedView view{db, "uv"};

  view.put(to_bytes("key1"), to_bytes("value1"));
  REQUIRE(view.contains_key(to_bytes("key1")));

  view.del(to_bytes("key1"));

  bytecask::Bytes out;
  CHECK_FALSE(view.get(to_bytes("key1"), out));
  CHECK_FALSE(view.contains_key(to_bytes("key1")));
}

TEST_CASE("UnorderedView many keys trigger splits", "[unordered_view]") {
  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  // Small capacity to force splits early.
  unordered_view::Options opts;
  opts.initial_size = 4;
  opts.bucket_capacity = 8;
  unordered_view::UnorderedView view{db, "uv", opts};

  constexpr std::size_t N = 500;
  for (std::size_t i = 0; i < N; ++i) {
    auto key = std::format("key_{:05d}", i);
    auto val = std::format("val_{:05d}", i);
    view.put(to_bytes(key), to_bytes(val));
  }

  // At least one split should have occurred.
  CHECK(view.stats().splits > 0);

  // All keys must be retrievable.
  bytecask::Bytes out;
  for (std::size_t i = 0; i < N; ++i) {
    auto key = std::format("key_{:05d}", i);
    auto val = std::format("val_{:05d}", i);
    REQUIRE(view.get(to_bytes(key), out));
    CHECK(to_string(out) == val);
  }
}

TEST_CASE("UnorderedView multiple key formats", "[unordered_view]") {
  auto shapes = std::vector<std::string>{"uuidv4_text", "sha256_hex", "prefixed"};
  constexpr std::size_t N = 200;

  for (auto &shape_name : shapes) {
    SECTION(shape_name) {
      TempDir td;
      auto db = bytecask::DB::open(td.path / "db");
      unordered_view::UnorderedView view{db, "uv"};

      auto *shape = key_generators::key_shape_by_name(shape_name);
      REQUIRE(shape != nullptr);
      auto keys = shape->generate(N);

      for (std::size_t i = 0; i < N; ++i) {
        auto val = std::format("v{}", i);
        view.put(to_bytes(keys[i]), to_bytes(val));
      }

      bytecask::Bytes out;
      for (std::size_t i = 0; i < N; ++i) {
        auto val = std::format("v{}", i);
        REQUIRE(view.get(to_bytes(keys[i]), out));
        CHECK(to_string(out) == val);
      }
    }
  }
}

TEST_CASE("UnorderedView reopen persistence", "[unordered_view]") {
  TempDir td;
  auto db_path = td.path / "db";
  constexpr std::size_t N = 100;

  {
    auto db = bytecask::DB::open(db_path);
    unordered_view::UnorderedView view{db, "uv"};
    for (std::size_t i = 0; i < N; ++i) {
      auto key = std::format("k{}", i);
      auto val = std::format("v{}", i);
      view.put(to_bytes(key), to_bytes(val));
    }
    // Sync final write so data is durable.
    db.put({.sync = true}, to_bytes("__sync__"), to_bytes(""));
  }

  // Reopen — UnorderedView should reload metadata and recount entries.
  {
    auto db = bytecask::DB::open(db_path);
    unordered_view::UnorderedView view{db, "uv"};

    bytecask::Bytes out;
    for (std::size_t i = 0; i < N; ++i) {
      auto key = std::format("k{}", i);
      auto val = std::format("v{}", i);
      REQUIRE(view.get(to_bytes(key), out));
      CHECK(to_string(out) == val);
    }
  }
}

TEST_CASE("UnorderedView hash16 collision handling", "[unordered_view]") {
  auto [key_a, key_b] = find_hash16_collision();
  REQUIRE(key_a != key_b);

  TempDir td;
  auto db = bytecask::DB::open(td.path / "db");
  // Use a single large bucket so both keys land in the same bucket.
  unordered_view::Options opts;
  opts.initial_size = 1;
  opts.bucket_capacity = 10000;
  unordered_view::UnorderedView view{db, "uv", opts};

  view.put(to_bytes(key_a), to_bytes("val_a"));
  view.put(to_bytes(key_b), to_bytes("val_b"));

  bytecask::Bytes out;
  REQUIRE(view.get(to_bytes(key_a), out));
  CHECK(to_string(out) == "val_a");

  REQUIRE(view.get(to_bytes(key_b), out));
  CHECK(to_string(out) == "val_b");

  // Overwrite one — the other must remain.
  view.put(to_bytes(key_a), to_bytes("val_a2"));
  REQUIRE(view.get(to_bytes(key_a), out));
  CHECK(to_string(out) == "val_a2");
  REQUIRE(view.get(to_bytes(key_b), out));
  CHECK(to_string(out) == "val_b");

  // Delete one — the other must remain.
  view.del(to_bytes(key_a));
  CHECK_FALSE(view.get(to_bytes(key_a), out));
  REQUIRE(view.get(to_bytes(key_b), out));
  CHECK(to_string(out) == "val_b");
}
